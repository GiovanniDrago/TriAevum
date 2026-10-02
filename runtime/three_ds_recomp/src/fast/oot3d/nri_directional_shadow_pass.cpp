#include "fast/oot3d/nri_directional_shadow_pass.h"

#ifdef ENABLE_OOT3D_VULKAN

#include "fast/oot3d/nri_interop_context.h"
#include "fast/oot3d/pica_directional_shadow_caster.h"
#include "fast/oot3d/pica_directional_shadow_semantics.h"
#include "fast/oot3d/resource_state_tracker.h"

#ifdef ENABLE_OOT3D_NRI
#include "nri_interop_internal.h"
#include <NRI.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Fast::Oot3d {
namespace {

constexpr uint32_t kFrameSlots = 2U;
constexpr uint32_t kMaximumCasters = 512U;
// Caster descriptor sets of one frame slot: the directional map and the
// point maps each bind a caster once.
constexpr uint32_t kCasterBindingsPerFrame = 2U * kMaximumCasters;
constexpr size_t kMaximumShadowTargets = 8U;

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    return ((value + alignment - 1U) / alignment) * alignment;
}

struct alignas(16) ShadowTransformUniforms {
    DirectionalShadowMatrix ViewToWorld{};
    DirectionalShadowMatrix WorldToLightClip{};
};

static_assert(sizeof(ShadowTransformUniforms) == 128U);

struct ShadowTargetKey {
    uint64_t RenderTargetNamespace = 0U;
    uint32_t ColorPhysicalAddress = 0U;

    bool operator==(const ShadowTargetKey&) const = default;
};

struct ShadowTargetKeyHash {
    size_t operator()(const ShadowTargetKey& key) const noexcept {
        uint64_t value = key.RenderTargetNamespace;
        value ^=
            static_cast<uint64_t>(key.ColorPhysicalAddress) + 0x9e3779b97f4a7c15ULL + (value << 6U) + (value >> 2U);
        return static_cast<size_t>(value);
    }
};

std::vector<uint32_t> Compile(Renderer::CachedPassShaderCompiler& shaders, const std::string& source,
                              Renderer::SpirvStage kind, const char* name) {
    return shaders.Resolve(source, kind, name);
}

uint64_t HashBytes(uint64_t hash, const void* data, size_t size) {
    constexpr uint64_t kPrime = 1099511628211ULL;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= kPrime;
    }
    return hash;
}

template <typename T> uint64_t HashValue(uint64_t hash, const T& value) {
    return HashBytes(hash, &value, sizeof(value));
}

using ::Fast::Renderer3ds::PicaCompareFunction;
using ::Fast::Renderer3ds::PicaSceneTextureBinding;
using ::Fast::Renderer3ds::PicaSceneTextureImageFormat;

// Descriptor ranges of the caster layout: the canonical program's own
// bindings (vertex uniforms 0, textures 1-3, fragment uniforms 4, split
// samplers 7-9, lighting LUT 13-14) plus the caster transform. Opaque casters
// write only the vertex uniforms and the transform.
enum CasterRange : uint32_t {
    VertexUniformRange,
    TransformRange,
    Texture0Range,
    Texture1Range,
    Texture2Range,
    FragmentUniformRange,
    Sampler0Range,
    Sampler1Range,
    Sampler2Range,
    LightingLutRange,
    LightingLutSamplerRange,
    CasterRangeCount,
};

// An alpha-tested caster runs its canonical fragment program up to the native
// alpha test; "Always" passes every fragment and stays a depth-only caster.
bool UsesAlphaCasterProgram(const PicaSceneDrawRecord& caster) noexcept {
    return caster.Material.AlphaTestEnabled && caster.Material.AlphaCompare != PicaCompareFunction::Always &&
           caster.FragmentShader != nullptr && caster.FragmentShader->FragmentHooksAvailable() &&
           caster.FragmentUniformSize != 0U;
}

VkFormat CasterTextureFormat(const PicaSceneTextureBinding& texture) noexcept {
    return texture.ImageFormat == PicaSceneTextureImageFormat::R32Uint ? VK_FORMAT_R32_UINT
                                                                        : VK_FORMAT_R8G8B8A8_UNORM;
}

#ifdef ENABLE_OOT3D_NRI
// The sampler the ordered draw binds for this texture: native filters (a mip
// filter only with more than one uploaded level), PICA wrap modes and the
// native LOD clamp within the uploaded chain. The LOD bias stays in the program.
nri::SamplerDesc CasterSamplerDesc(const PicaSceneTextureBinding& texture) noexcept {
    const auto address = [](uint8_t wrap) {
        switch (wrap) {
            case 2U:
            case 6U:
            case 7U:
                return nri::AddressMode::REPEAT;
            case 3U:
                return nri::AddressMode::MIRRORED_REPEAT;
            default:
                return nri::AddressMode::CLAMP_TO_EDGE;
        }
    };
    const uint32_t highestMip = texture.MipLevels > 0U ? texture.MipLevels - 1U : 0U;
    nri::SamplerDesc desc{};
    desc.filters = { texture.MinLinear ? nri::Filter::LINEAR : nri::Filter::NEAREST,
                     texture.MagLinear ? nri::Filter::LINEAR : nri::Filter::NEAREST,
                     texture.MipLinear && texture.MipLevels > 1U ? nri::Filter::LINEAR : nri::Filter::NEAREST };
    desc.mipMin = static_cast<float>(std::min<uint32_t>(texture.MinMipLevel, highestMip));
    desc.mipMax = static_cast<float>(std::min<uint32_t>(texture.MaxMipLevel, highestMip));
    desc.addressModes = { address(texture.NativeWrapS), address(texture.NativeWrapT), nri::AddressMode::REPEAT };
    desc.isInteger = texture.ImageFormat == PicaSceneTextureImageFormat::R32Uint;
    return desc;
}

uint64_t CasterSamplerKey(const nri::SamplerDesc& desc) noexcept {
    uint64_t hash = 1469598103934665603ULL;
    hash = HashValue(hash, desc.filters.min);
    hash = HashValue(hash, desc.filters.mag);
    hash = HashValue(hash, desc.filters.mip);
    hash = HashValue(hash, desc.mipMin);
    hash = HashValue(hash, desc.mipMax);
    hash = HashValue(hash, desc.addressModes.u);
    hash = HashValue(hash, desc.addressModes.v);
    return HashValue(hash, desc.isInteger);
}
#endif

#ifdef ENABLE_OOT3D_NRI
nri::Topology ToNriTopology(PicaSceneTopology topology) {
    switch (topology) {
        case PicaSceneTopology::TriangleList:
            return nri::Topology::TRIANGLE_LIST;
        case PicaSceneTopology::TriangleStrip:
            return nri::Topology::TRIANGLE_STRIP;
    }
    return nri::Topology::MAX_NUM;
}

nri::CullMode ToNriCullMode(PicaSceneCullMode mode) {
    switch (mode) {
        case PicaSceneCullMode::None:
            return nri::CullMode::NONE;
        case PicaSceneCullMode::Front:
            return nri::CullMode::FRONT;
        case PicaSceneCullMode::Back:
            return nri::CullMode::BACK;
    }
    return nri::CullMode::MAX_NUM;
}

template <typename Handle> Handle FromSceneNativeHandle(uintptr_t value) noexcept {
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<Handle>(value);
    } else {
        return static_cast<Handle>(value);
    }
}
#endif

} // namespace

struct NriDirectionalShadowPass::Impl {
    struct HistorySlot {
        NriOwnedTexture2D Texture;
        PicaDirectionalShadowHistoryState State;
    };

    struct TargetHistory {
        HistorySlot Slot;
        NriOwnedTexture2D StaticTexture;
        Renderer3ds::VirtualShadowMapCache Cache;
        uint32_t Resolution = 0U;
        uint64_t LastUsedFrameId = 0U;
    };

    VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
    VkDevice Device = VK_NULL_HANDLE;
    VkSampler ReceiverSampler = VK_NULL_HANDLE;
    NriInteropContext* Interop = nullptr;
    std::unordered_map<ShadowTargetKey, TargetHistory, ShadowTargetKeyHash> Histories;
    struct RetiredTexture {
        NriOwnedTexture2D Texture;
        uint64_t FrameId;
    };
    std::vector<RetiredTexture> RetiredTextures;
    // Point-light cube atlases, one per target.
    struct PointTarget {
        NriOwnedTexture2D Texture;
        Renderer3ds::PointShadowLayout Layout;
        Renderer3ds::PointShadowSlots Slots{};
        std::array<uint64_t, Renderer3ds::kMaximumPointShadowLights> Signatures{};
        float Range = 0.0F;
        float Strength = 0.0F;
        uint64_t ProducedFrameId = 0U;
        uint64_t LastUsedFrameId = 0U;
    };
    std::unordered_map<ShadowTargetKey, PointTarget, ShadowTargetKeyHash> PointTargets;
    NriPointShadowTelemetry LastPoint;
    ResourceStateTracker OwnedStates;
    bool LastNativeLight = false;
    uint32_t LastCasters = 0U;
    uint32_t LastAlphaCasters = 0U;
    NriDirectionalShadowLightTelemetry LastLight;
    NriDirectionalShadowExecutionStage LastStage = NriDirectionalShadowExecutionStage::NotAttempted;
    EffectPassBarrierExecution LastShadowMapBarrierExecution;
    std::string Reason = "NRI directional shadow pass is not initialized";
#ifdef ENABLE_OOT3D_NRI
    nri::CoreInterface* Core = nullptr;
    nri::PipelineLayout* ShadowLayout = nullptr;
    struct FrameDescriptors {
        nri::DescriptorPool* CasterPool = nullptr;
        std::vector<nri::DescriptorSet*> CasterSets;
        nri::Buffer* ShadowUniformBuffer = nullptr;
        std::vector<nri::Descriptor*> ShadowUniformViews;
        uint8_t* ShadowUniformMapped = nullptr;
        uint64_t ShadowUniformStride = 0U;
        std::vector<nri::Descriptor*> TransientViews;
        uint64_t FrameId = std::numeric_limits<uint64_t>::max();
        uint32_t CastersUsed = 0U;
    };
    std::array<FrameDescriptors, kFrameSlots> Frames;
    std::unordered_map<uint64_t, nri::Pipeline*> ShadowPipelines;
    std::unordered_set<uint64_t> FailedShadowPipelines;
    std::unordered_map<uint64_t, nri::Pipeline*> PointPipelines;
    std::unordered_set<uint64_t> FailedPointPipelines;
    std::unordered_map<uint64_t, nri::Descriptor*> CasterSamplers;
#endif

    void Retire(TargetHistory& history, uint64_t frameId) {
        for (const auto* texture : { &history.Slot.Texture, &history.StaticTexture }) {
            if (texture->Image != VK_NULL_HANDLE) {
                OwnedStates.Forget(reinterpret_cast<uintptr_t>(texture->Image));
                RetiredTextures.push_back({ *texture, frameId });
            }
        }
        history = {};
    }

    void DestroyOwnedTextures() {
        if (Interop != nullptr) {
            for (auto& [key, history] : Histories) {
                (void)key;
                Interop->DestroyOwnedTexture(history.Slot.Texture.Image);
                Interop->DestroyOwnedTexture(history.StaticTexture.Image);
            }
            for (auto& texture : RetiredTextures) {
                Interop->DestroyOwnedTexture(texture.Texture.Image);
            }
            for (auto& [key, point] : PointTargets) {
                (void)key;
                Interop->DestroyOwnedTexture(point.Texture.Image);
            }
        }
        Histories.clear();
        PointTargets.clear();
        RetiredTextures.clear();
        OwnedStates.Clear();
    }

#ifdef ENABLE_OOT3D_NRI
    TargetHistory* EnsureTarget(const ShadowTargetKey& key, uint32_t resolution, uint64_t frameId) {
        // The host has waited for this frame slot's fence. An atlas last
        // referenced a full slot rotation ago can no longer be sampled.
        std::erase_if(RetiredTextures, [&](const auto& retired) {
            if (frameId < retired.FrameId || frameId - retired.FrameId < kFrameSlots)
                return false;
            Interop->DestroyOwnedTexture(retired.Texture.Image);
            return true;
        });
        auto found = Histories.find(key);
        if (found != Histories.end() && found->second.Resolution == resolution &&
            found->second.Slot.Texture.Image != VK_NULL_HANDLE) {
            found->second.LastUsedFrameId = frameId;
            return &found->second;
        }

        TargetHistory replacement;
        replacement.Resolution = resolution;
        replacement.LastUsedFrameId = frameId;
        NriOwnedTexture2DDesc textureDesc;
        textureDesc.Width = resolution;
        textureDesc.Height = resolution;
        textureDesc.Format = VK_FORMAT_D32_SFLOAT;
        textureDesc.Usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!Interop->CreateOwnedTexture2D(textureDesc, replacement.Slot.Texture))
            return nullptr;
        textureDesc.Usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!Interop->CreateOwnedTexture2D(textureDesc, replacement.StaticTexture)) {
            Interop->DestroyOwnedTexture(replacement.Slot.Texture.Image);
            return nullptr;
        }

        if (found != Histories.end()) {
            Retire(found->second, frameId);
            found->second = std::move(replacement);
            return &found->second;
        }
        if (Histories.size() >= kMaximumShadowTargets) {
            const auto oldest =
                std::min_element(Histories.begin(), Histories.end(), [](const auto& left, const auto& right) {
                    return left.second.LastUsedFrameId < right.second.LastUsedFrameId;
                });
            if (oldest != Histories.end()) {
                Retire(oldest->second, frameId);
                Histories.erase(oldest);
            }
        }
        return &Histories.emplace(key, std::move(replacement)).first->second;
    }

    uint64_t PipelineKey(const PicaSceneDrawRecord& caster, const DirectionalShadowFramePlan& plan) const {
        uint64_t hash = 1469598103934665603ULL;
        hash = HashValue(hash, caster.VertexShader->Key);
        hash = HashValue(hash, caster.Material.Topology);
        hash = HashValue(hash, caster.Material.CullMode);
        hash = HashValue(hash, caster.Material.FrontFace);
        hash = HashValue(hash, plan.DepthBiasConstant);
        hash = HashValue(hash, plan.DepthBiasSlope);
        hash = HashValue(hash, caster.VertexLayout->StructuralKey);
        if (UsesAlphaCasterProgram(caster))
            hash = HashValue(hash, caster.FragmentShader->Key);
        return hash;
    }

    nri::Descriptor* SampledView(const PicaSceneTextureBinding& texture) {
        const auto image = FromSceneNativeHandle<VkImage>(texture.NativeImageHandle);
        if (image == VK_NULL_HANDLE || texture.ImageWidth == 0U || texture.ImageHeight == 0U ||
            texture.MipLevels == 0U ||
            !Interop->WrapTexture(image, CasterTextureFormat(texture), VK_IMAGE_TYPE_2D, VK_IMAGE_USAGE_SAMPLED_BIT,
                                  texture.ImageWidth, texture.ImageHeight, texture.MipLevels))
            return nullptr;
        return NriInteropAccess::TextureView(*Interop, image, false);
    }

    nri::Descriptor* Sampler(const PicaSceneTextureBinding& texture) {
        const auto desc = CasterSamplerDesc(texture);
        auto [found, inserted] = CasterSamplers.try_emplace(CasterSamplerKey(desc), nullptr);
        if (inserted &&
            Core->CreateSampler(*NriInteropAccess::Device(*Interop), desc, found->second) != nri::Result::SUCCESS) {
            CasterSamplers.erase(found);
            return nullptr;
        }
        return found->second;
    }

    nri::Pipeline* GetOrCreatePipeline(const PicaSceneDrawRecord& caster, const DirectionalShadowFramePlan& plan) {
        const uint64_t key = PipelineKey(caster, plan);
        const auto found = ShadowPipelines.find(key);
        if (found != ShadowPipelines.end()) {
            return found->second;
        }
        if (FailedShadowPipelines.contains(key)) {
            return nullptr;
        }
        const auto reject = [this, key]() -> nri::Pipeline* {
            FailedShadowPipelines.insert(key);
            return nullptr;
        };
        const nri::Topology topology = ToNriTopology(caster.Material.Topology);
        const nri::CullMode cull = ToNriCullMode(caster.Material.CullMode);
        if (topology == nri::Topology::MAX_NUM || cull == nri::CullMode::MAX_NUM) {
            return reject();
        }

        std::vector<nri::VertexStreamDesc> streams;
        streams.reserve(caster.VertexLayout->Bindings.size());
        for (const auto& source : caster.VertexLayout->Bindings) {
            if (source.Binding > std::numeric_limits<uint16_t>::max() ||
                source.ByteStride > std::numeric_limits<uint16_t>::max()) {
                return reject();
            }
            streams.push_back({
                static_cast<uint16_t>(source.Binding),
                source.PerInstance ? nri::VertexStreamStepRate::PER_INSTANCE : nri::VertexStreamStepRate::PER_VERTEX,
                static_cast<uint16_t>(source.ByteStride),
            });
        }
        std::vector<nri::VertexAttributeDesc> attributes;
        attributes.reserve(caster.VertexLayout->Attributes.size());
        for (const auto& source : caster.VertexLayout->Attributes) {
            if (source.Location > std::numeric_limits<uint16_t>::max() ||
                source.Binding > std::numeric_limits<uint16_t>::max()) {
                return reject();
            }
            nri::VertexAttributeDesc attribute{};
            attribute.vk.location = source.Location;
            attribute.offset = source.ByteOffset;
            attribute.format = nri::Format::RGBA32_SFLOAT;
            attribute.streamIndex = static_cast<uint16_t>(source.Binding);
            attributes.push_back(attribute);
        }
        const nri::VertexInputDesc vertexInput{ attributes.data(), static_cast<uint8_t>(attributes.size()),
                                                streams.data(), static_cast<uint8_t>(streams.size()) };

        std::vector<uint32_t> vertex;
        std::vector<uint32_t> fragment;
        try {
            if (caster.VertexShader == nullptr || !caster.VertexShader->VertexHooksAvailable()) {
                return reject();
            }
            const auto casterShader = BuildPicaDirectionalShadowCasterShader(
                caster.VertexShader->Source, caster.VertexShader->Key, caster.VertexShader->VertexHooks);
            if (!casterShader.Applied()) {
                return reject();
            }
            vertex = Compile(Interop->Shaders(), casterShader.Source, Renderer::SpirvStage::Vertex,
                             "oot3d_nri_directional_shadow.vert");
            if (UsesAlphaCasterProgram(caster)) {
                const auto alphaShader = BuildPicaShadowCasterAlphaFragmentShader(
                    caster.FragmentShader->Source, caster.FragmentShader->Key, caster.FragmentShader->FragmentHooks);
                if (!alphaShader.Applied()) {
                    return reject();
                }
                fragment = Compile(Interop->Shaders(), alphaShader.Source, Renderer::SpirvStage::Fragment,
                                   "oot3d_nri_shadow_caster_alpha.frag");
            } else {
                fragment = Compile(Interop->Shaders(), kPicaDirectionalShadowFragmentShader,
                                   Renderer::SpirvStage::Fragment, "oot3d_nri_directional_shadow.frag");
            }
        } catch (const std::exception&) { return reject(); }
        const std::array<nri::ShaderDesc, 2> shaders{ {
            { nri::StageBits::VERTEX_SHADER, vertex.data(), vertex.size() * sizeof(uint32_t) },
            { nri::StageBits::FRAGMENT_SHADER, fragment.data(), fragment.size() * sizeof(uint32_t) },
        } };
        nri::MultisampleDesc multisample{};
        multisample.sampleMask = 0xFFFFFFFFU;
        multisample.sampleNum = 1;
        nri::GraphicsPipelineDesc pipeline{};
        pipeline.pipelineLayout = ShadowLayout;
        pipeline.vertexInput = &vertexInput;
        pipeline.inputAssembly.topology = topology;
        pipeline.rasterization.fillMode = nri::FillMode::SOLID;
        pipeline.rasterization.cullMode = cull;
        pipeline.rasterization.frontCounterClockwise =
            caster.Material.FrontFace == PicaSceneFrontFace::CounterClockwise;
        pipeline.rasterization.depthBias = { plan.DepthBiasConstant, 0.0F, plan.DepthBiasSlope };
        pipeline.multisample = &multisample;
        pipeline.outputMerger.depth.compareOp = nri::CompareOp::LESS_EQUAL;
        pipeline.outputMerger.depth.write = true;
        pipeline.outputMerger.depthStencilFormat = nri::Format::D32_SFLOAT;
        pipeline.shaders = shaders.data();
        pipeline.shaderNum = static_cast<uint32_t>(shaders.size());
        nri::Pipeline* result = nullptr;
        pipeline.cache = NriInteropAccess::PipelineCache(*Interop);
        if (Core->CreateGraphicsPipeline(*NriInteropAccess::Device(*Interop), pipeline, result) !=
            nri::Result::SUCCESS) {
            return reject();
        }
        ShadowPipelines.emplace(key, result);
        return result;
    }

    PointTarget* EnsurePointTarget(const ShadowTargetKey& key, const Renderer3ds::PointShadowLayout& layout,
                                   uint64_t frameId) {
        auto found = PointTargets.find(key);
        if (found != PointTargets.end() && found->second.Layout.Width == layout.Width &&
            found->second.Layout.Height == layout.Height && found->second.Texture.Image != VK_NULL_HANDLE) {
            found->second.LastUsedFrameId = frameId;
            return &found->second;
        }
        PointTarget replacement;
        replacement.Layout = layout;
        replacement.LastUsedFrameId = frameId;
        NriOwnedTexture2DDesc textureDesc;
        textureDesc.Width = layout.Width;
        textureDesc.Height = layout.Height;
        textureDesc.Format = VK_FORMAT_D32_SFLOAT;
        textureDesc.Usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!Interop->CreateOwnedTexture2D(textureDesc, replacement.Texture))
            return nullptr;
        if (found != PointTargets.end()) {
            OwnedStates.Forget(reinterpret_cast<uintptr_t>(found->second.Texture.Image));
            RetiredTextures.push_back({ found->second.Texture, frameId });
            found->second = std::move(replacement);
            return &found->second;
        }
        if (PointTargets.size() >= kMaximumShadowTargets) {
            const auto oldest = std::min_element(PointTargets.begin(), PointTargets.end(), [](const auto& l, const auto& r) {
                return l.second.LastUsedFrameId < r.second.LastUsedFrameId;
            });
            OwnedStates.Forget(reinterpret_cast<uintptr_t>(oldest->second.Texture.Image));
            RetiredTextures.push_back({ oldest->second.Texture, frameId });
            PointTargets.erase(oldest);
        }
        return &PointTargets.emplace(key, std::move(replacement)).first->second;
    }

    nri::Pipeline* GetOrCreatePointPipeline(const PicaSceneDrawRecord& caster) {
        uint64_t key = 0x504f494e54534844ULL;
        key = HashValue(key, caster.VertexShader->Key);
        key = HashValue(key, caster.Material.Topology);
        key = HashValue(key, caster.VertexLayout->StructuralKey);
        if (UsesAlphaCasterProgram(caster))
            key = HashValue(key, caster.FragmentShader->Key);
        if (const auto found = PointPipelines.find(key); found != PointPipelines.end())
            return found->second;
        if (FailedPointPipelines.contains(key))
            return nullptr;
        const auto reject = [this, key]() -> nri::Pipeline* {
            FailedPointPipelines.insert(key);
            return nullptr;
        };
        const nri::Topology topology = ToNriTopology(caster.Material.Topology);
        if (topology == nri::Topology::MAX_NUM || caster.VertexShader == nullptr ||
            !caster.VertexShader->VertexHooksAvailable())
            return reject();
        std::vector<nri::VertexStreamDesc> streams;
        for (const auto& source : caster.VertexLayout->Bindings) {
            if (source.Binding > std::numeric_limits<uint16_t>::max() ||
                source.ByteStride > std::numeric_limits<uint16_t>::max())
                return reject();
            streams.push_back({ static_cast<uint16_t>(source.Binding),
                                source.PerInstance ? nri::VertexStreamStepRate::PER_INSTANCE
                                                   : nri::VertexStreamStepRate::PER_VERTEX,
                                static_cast<uint16_t>(source.ByteStride) });
        }
        std::vector<nri::VertexAttributeDesc> attributes;
        for (const auto& source : caster.VertexLayout->Attributes) {
            if (source.Location > std::numeric_limits<uint16_t>::max() ||
                source.Binding > std::numeric_limits<uint16_t>::max())
                return reject();
            nri::VertexAttributeDesc attribute{};
            attribute.vk.location = source.Location;
            attribute.offset = source.ByteOffset;
            attribute.format = nri::Format::RGBA32_SFLOAT;
            attribute.streamIndex = static_cast<uint16_t>(source.Binding);
            attributes.push_back(attribute);
        }
        const nri::VertexInputDesc vertexInput{ attributes.data(), static_cast<uint8_t>(attributes.size()),
                                                streams.data(), static_cast<uint8_t>(streams.size()) };
        std::vector<uint32_t> vertex;
        std::vector<uint32_t> fragment;
        try {
            const auto casterShader = BuildPicaPointShadowCasterShader(
                caster.VertexShader->Source, caster.VertexShader->Key, caster.VertexShader->VertexHooks);
            if (!casterShader.Applied())
                return reject();
            vertex = Compile(Interop->Shaders(), casterShader.Source, Renderer::SpirvStage::Vertex,
                             "oot3d_nri_point_shadow.vert");
            if (UsesAlphaCasterProgram(caster)) {
                const auto alphaShader = BuildPicaShadowCasterAlphaFragmentShader(
                    caster.FragmentShader->Source, caster.FragmentShader->Key, caster.FragmentShader->FragmentHooks);
                if (!alphaShader.Applied())
                    return reject();
                fragment = Compile(Interop->Shaders(), alphaShader.Source, Renderer::SpirvStage::Fragment,
                                   "oot3d_nri_shadow_caster_alpha.frag");
            } else {
                fragment = Compile(Interop->Shaders(), kPicaDirectionalShadowFragmentShader,
                                   Renderer::SpirvStage::Fragment, "oot3d_nri_directional_shadow.frag");
            }
        } catch (const std::exception&) { return reject(); }
        const std::array<nri::ShaderDesc, 2> shaders{ {
            { nri::StageBits::VERTEX_SHADER, vertex.data(), vertex.size() * sizeof(uint32_t) },
            { nri::StageBits::FRAGMENT_SHADER, fragment.data(), fragment.size() * sizeof(uint32_t) },
        } };
        nri::MultisampleDesc multisample{};
        multisample.sampleMask = 0xFFFFFFFFU;
        multisample.sampleNum = 1;
        nri::GraphicsPipelineDesc pipeline{};
        pipeline.pipelineLayout = ShadowLayout;
        pipeline.vertexInput = &vertexInput;
        pipeline.inputAssembly.topology = topology;
        pipeline.rasterization.fillMode = nri::FillMode::SOLID;
        pipeline.rasterization.cullMode = nri::CullMode::NONE;
        pipeline.rasterization.depthBias = { 0.0F, 0.0F, 2.0F };
        pipeline.multisample = &multisample;
        pipeline.outputMerger.depth.compareOp = nri::CompareOp::LESS_EQUAL;
        pipeline.outputMerger.depth.write = true;
        pipeline.outputMerger.depthStencilFormat = nri::Format::D32_SFLOAT;
        pipeline.shaders = shaders.data();
        pipeline.shaderNum = static_cast<uint32_t>(shaders.size());
        nri::Pipeline* result = nullptr;
        pipeline.cache = NriInteropAccess::PipelineCache(*Interop);
        if (Core->CreateGraphicsPipeline(*NriInteropAccess::Device(*Interop), pipeline, result) != nri::Result::SUCCESS)
            return reject();
        PointPipelines.emplace(key, result);
        return result;
    }

    struct PreparedCaster {
        nri::Pipeline* Pipeline = nullptr;
        nri::Buffer* Geometry = nullptr;
        nri::DescriptorSet* Set = nullptr;
    };

    // Descriptors of one caster in this frame slot: vertex uniforms, the
    // caster transform and, for an alpha-tested caster, its native fragment
    // state. The set stays valid for every map recorded in the frame.
    bool BindCaster(FrameDescriptors& frame, const PicaSceneFrame& sceneFrame, const PicaSceneDrawRecord& caster,
                    const DirectionalShadowMatrix& worldToLightClip, PreparedCaster& draw) {
        if (frame.CastersUsed == frame.CasterSets.size())
            return false;
        const auto uniforms = FromSceneNativeHandle<VkBuffer>(caster.UniformBuffer.NativeHandle);
        const auto vertices = FromSceneNativeHandle<VkBuffer>(caster.GeometryBuffer.NativeHandle);
        if (!Interop->WrapBuffer(uniforms, caster.UniformBuffer.Size) ||
            !Interop->WrapBuffer(vertices, caster.GeometryBuffer.Size))
            return false;
        auto* uniformBuffer = NriInteropAccess::Buffer(*Interop, uniforms);
        draw.Geometry = NriInteropAccess::Buffer(*Interop, vertices);
        if (uniformBuffer == nullptr || draw.Geometry == nullptr ||
            caster.VertexUniformOffset > caster.UniformBuffer.Size ||
            caster.VertexUniformSize > caster.UniformBuffer.Size - caster.VertexUniformOffset)
            return false;
        for (const auto& binding : sceneFrame.VertexBindings(caster))
            if (binding.Size == 0 || binding.Stride == 0 || binding.Offset > caster.GeometryBuffer.Size ||
                binding.Size > caster.GeometryBuffer.Size - binding.Offset)
                return false;
        if (caster.Indexed &&
            (caster.IndexOffset > caster.GeometryBuffer.Size ||
             uint64_t(caster.VertexOrIndexCount) * sizeof(uint16_t) > caster.GeometryBuffer.Size - caster.IndexOffset))
            return false;
        ShadowTransformUniforms transform;
        if (!InvertDirectionalShadowMatrix(*caster.LightingWorldToView, transform.ViewToWorld))
            return false;
        transform.WorldToLightClip = worldToLightClip;
        nri::BufferViewDesc uniformView{};
        uniformView.buffer = uniformBuffer;
        uniformView.type = nri::BufferView::CONSTANT_BUFFER;
        uniformView.offset = caster.VertexUniformOffset;
        uniformView.size = caster.VertexUniformSize;
        nri::Descriptor* vertexUniform = nullptr;
        if (Core->CreateBufferView(uniformView, vertexUniform) != nri::Result::SUCCESS)
            return false;
        frame.TransientViews.push_back(vertexUniform);
        const uint32_t slot = frame.CastersUsed++;
        std::memcpy(frame.ShadowUniformMapped + frame.ShadowUniformStride * slot, &transform, sizeof(transform));
        draw.Set = frame.CasterSets[slot];
        std::array<nri::UpdateDescriptorRangeDesc, CasterRangeCount> updates{};
        uint32_t updateCount = 0;
        updates[updateCount++] = { draw.Set, VertexUniformRange, 0, &vertexUniform, 1 };
        updates[updateCount++] = { draw.Set, TransformRange, 0, &frame.ShadowUniformViews[slot], 1 };
        // The alpha program reads its native textures, samplers, fragment
        // uniforms and, when declared, the lighting LUT, as the ordered draw does.
        std::array<nri::Descriptor*, 3> textures{};
        std::array<nri::Descriptor*, 3> samplers{};
        nri::Descriptor* fragmentUniform = nullptr;
        nri::Descriptor* lightingLut = nullptr;
        nri::Descriptor* lightingLutSampler = nullptr;
        if (UsesAlphaCasterProgram(caster)) {
            if (caster.FragmentUniformOffset > caster.UniformBuffer.Size ||
                caster.FragmentUniformSize > caster.UniformBuffer.Size - caster.FragmentUniformOffset)
                return false;
            nri::BufferViewDesc fragmentView{};
            fragmentView.buffer = uniformBuffer;
            fragmentView.type = nri::BufferView::CONSTANT_BUFFER;
            fragmentView.offset = caster.FragmentUniformOffset;
            fragmentView.size = caster.FragmentUniformSize;
            if (Core->CreateBufferView(fragmentView, fragmentUniform) != nri::Result::SUCCESS)
                return false;
            frame.TransientViews.push_back(fragmentUniform);
            for (size_t unit = 0; unit < textures.size(); ++unit) {
                textures[unit] = SampledView(caster.Textures[unit]);
                samplers[unit] = Sampler(caster.Textures[unit]);
                if (textures[unit] == nullptr || samplers[unit] == nullptr)
                    return false;
                updates[updateCount++] = { draw.Set, static_cast<uint32_t>(Texture0Range + unit), 0, &textures[unit], 1 };
                updates[updateCount++] = { draw.Set, static_cast<uint32_t>(Sampler0Range + unit), 0, &samplers[unit], 1 };
            }
            updates[updateCount++] = { draw.Set, FragmentUniformRange, 0, &fragmentUniform, 1 };
            if (caster.LightingLut.Bound) {
                lightingLut = SampledView(caster.LightingLut);
                lightingLutSampler = Sampler(caster.LightingLut);
                if (lightingLut == nullptr || lightingLutSampler == nullptr)
                    return false;
                updates[updateCount++] = { draw.Set, LightingLutRange, 0, &lightingLut, 1 };
                updates[updateCount++] = { draw.Set, LightingLutSamplerRange, 0, &lightingLutSampler, 1 };
            }
        }
        Core->UpdateDescriptorRanges(updates.data(), updateCount);
        return true;
    }
#endif
};

NriDirectionalShadowPass::NriDirectionalShadowPass() : mImpl(std::make_unique<Impl>()) {
}

NriDirectionalShadowPass::~NriDirectionalShadowPass() {
    Shutdown();
}

bool NriDirectionalShadowPass::Initialize(VkPhysicalDevice physicalDevice, VkDevice device,
                                          NriInteropContext& interop) {
    Shutdown();
    mImpl->PhysicalDevice = physicalDevice;
    mImpl->Device = device;
    mImpl->Interop = &interop;
    if (const char* enabled = std::getenv("OOT3D_GRAPHICS_NRI_DIRECTIONAL_SHADOWS");
        enabled != nullptr && std::string_view(enabled) == "0") {
        mImpl->Reason = "NRI directional shadows disabled by environment";
        return false;
    }
    if (!interop.Available()) {
        mImpl->Reason = interop.UnavailableReason();
        return false;
    }
    if (!interop.DynamicRenderingAvailable()) {
        mImpl->Reason = "NRI directional shadows require dynamic rendering";
        return false;
    }
#ifndef ENABLE_OOT3D_NRI
    mImpl->Reason = "NRI directional shadows were not compiled";
    return false;
#else
    try {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, VK_FORMAT_D32_SFLOAT, &properties);
        const VkFormatFeatureFlags required =
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((properties.optimalTilingFeatures & required) != required) {
            throw std::runtime_error("D32 sampled depth attachments are unavailable");
        }
        nri::Device* nriDevice = NriInteropAccess::Device(interop);
        mImpl->Core = NriInteropAccess::Core(interop);
        if (nriDevice == nullptr || mImpl->Core == nullptr) {
            throw std::runtime_error("NRI directional shadow device is unavailable");
        }

        const std::array<nri::DescriptorRangeDesc, CasterRangeCount> ranges{ {
            { 0, 1, nri::DescriptorType::CONSTANT_BUFFER, nri::StageBits::VERTEX_SHADER },
            { kPicaShadowCasterTransformBinding, 1, nri::DescriptorType::CONSTANT_BUFFER,
              nri::StageBits::VERTEX_SHADER },
            { 1, 1, nri::DescriptorType::TEXTURE, nri::StageBits::FRAGMENT_SHADER },
            { 2, 1, nri::DescriptorType::TEXTURE, nri::StageBits::FRAGMENT_SHADER },
            { 3, 1, nri::DescriptorType::TEXTURE, nri::StageBits::FRAGMENT_SHADER },
            { 4, 1, nri::DescriptorType::CONSTANT_BUFFER, nri::StageBits::FRAGMENT_SHADER },
            { 7, 1, nri::DescriptorType::SAMPLER, nri::StageBits::FRAGMENT_SHADER },
            { 8, 1, nri::DescriptorType::SAMPLER, nri::StageBits::FRAGMENT_SHADER },
            { 9, 1, nri::DescriptorType::SAMPLER, nri::StageBits::FRAGMENT_SHADER },
            { 13, 1, nri::DescriptorType::TEXTURE, nri::StageBits::FRAGMENT_SHADER },
            { 14, 1, nri::DescriptorType::SAMPLER, nri::StageBits::FRAGMENT_SHADER },
        } };
        nri::DescriptorSetDesc descriptorSet{};
        descriptorSet.ranges = ranges.data();
        descriptorSet.rangeNum = static_cast<uint32_t>(ranges.size());
        nri::PipelineLayoutDesc layout{};
        const nri::RootConstantDesc pageConstants{ 0, sizeof(float) * 16, nri::StageBits::VERTEX_SHADER };
        layout.rootConstants = &pageConstants;
        layout.rootConstantNum = 1;
        layout.descriptorSets = &descriptorSet;
        layout.descriptorSetNum = 1;
        layout.shaderStages = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;
        layout.flags = nri::PipelineLayoutBits::IGNORE_GLOBAL_SPIRV_OFFSETS;
        if (mImpl->Core->CreatePipelineLayout(*nriDevice, layout, mImpl->ShadowLayout) != nri::Result::SUCCESS) {
            throw std::runtime_error("NRI directional shadow layout creation failed");
        }

        VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 0.0F;
        if (vkCreateSampler(device, &samplerInfo, nullptr, &mImpl->ReceiverSampler) != VK_SUCCESS) {
            throw std::runtime_error("directional shadow receiver sampler creation failed");
        }

        const auto& deviceDesc = mImpl->Core->GetDeviceDesc(*nriDevice);
        const uint64_t uniformAlignment = std::max<uint64_t>(1U, deviceDesc.memoryAlignment.constantBufferOffset);
        for (auto& frame : mImpl->Frames) {
            frame.ShadowUniformStride = AlignUp(sizeof(ShadowTransformUniforms), uniformAlignment);
            nri::BufferDesc bufferDesc{};
            bufferDesc.size = frame.ShadowUniformStride * kCasterBindingsPerFrame;
            bufferDesc.usage = nri::BufferUsageBits::CONSTANT_BUFFER;
            if (mImpl->Core->CreateCommittedBuffer(*nriDevice, nri::MemoryLocation::HOST_UPLOAD, 0.0F, bufferDesc,
                                                   frame.ShadowUniformBuffer) != nri::Result::SUCCESS) {
                throw std::runtime_error("NRI directional shadow uniform arena creation failed");
            }
            frame.ShadowUniformMapped =
                static_cast<uint8_t*>(mImpl->Core->MapBuffer(*frame.ShadowUniformBuffer, 0, bufferDesc.size));
            if (frame.ShadowUniformMapped == nullptr) {
                throw std::runtime_error("NRI directional shadow uniform arena mapping failed");
            }
            frame.ShadowUniformViews.resize(kCasterBindingsPerFrame, nullptr);
            for (uint32_t index = 0U; index < kCasterBindingsPerFrame; ++index) {
                nri::BufferViewDesc view{};
                view.buffer = frame.ShadowUniformBuffer;
                view.type = nri::BufferView::CONSTANT_BUFFER;
                view.offset = frame.ShadowUniformStride * index;
                view.size = sizeof(ShadowTransformUniforms);
                if (mImpl->Core->CreateBufferView(view, frame.ShadowUniformViews[index]) != nri::Result::SUCCESS) {
                    throw std::runtime_error("NRI directional shadow uniform view creation failed");
                }
            }

            nri::DescriptorPoolDesc pool{};
            pool.descriptorSetMaxNum = kCasterBindingsPerFrame;
            pool.constantBufferMaxNum = 3U * kCasterBindingsPerFrame;
            pool.textureMaxNum = 4U * kCasterBindingsPerFrame;
            pool.samplerMaxNum = 4U * kCasterBindingsPerFrame;
            frame.CasterSets.resize(kCasterBindingsPerFrame);
            if (mImpl->Core->CreateDescriptorPool(*nriDevice, pool, frame.CasterPool) != nri::Result::SUCCESS ||
                mImpl->Core->AllocateDescriptorSets(*frame.CasterPool, *mImpl->ShadowLayout, 0, frame.CasterSets.data(),
                                                    kCasterBindingsPerFrame, 0) != nri::Result::SUCCESS) {
                throw std::runtime_error("NRI directional shadow caster descriptors failed");
            }
        }
        mImpl->Reason.clear();
        return true;
    } catch (const std::exception& exception) {
        const std::string reason = exception.what();
        Shutdown();
        mImpl->Reason = reason;
        return false;
    }
#endif
}

bool NriDirectionalShadowPass::ExecuteMap(const NriDirectionalShadowExecuteDesc& desc, const NativeSceneView& sceneView,
                                          const EffectPassBarrierPlan& shadowMapBarrierPlan) {
    mImpl->LastNativeLight = false;
    mImpl->LastCasters = 0;
    mImpl->LastAlphaCasters = 0;
    mImpl->LastLight = {};
    mImpl->LastStage = NriDirectionalShadowExecutionStage::NotAttempted;
    mImpl->LastShadowMapBarrierExecution = {};
#ifndef ENABLE_OOT3D_NRI
    (void)desc;
    (void)sceneView;
    (void)shadowMapBarrierPlan;
    return false;
#else
    const auto* mapBarrier = shadowMapBarrierPlan.Find(EffectResource::DirectionalShadowMap);
    const auto* staticBarrier = shadowMapBarrierPlan.Find(EffectResource::DirectionalShadowStaticCache);
    const auto expectedPassId =
        ::Fast::Renderer::StableExtensionPassId(PicaExtensionPassName(PicaExtensionPass::DirectionalShadowMap));
    if (!Available() ||
        (!desc.ScheduleAuthorization.Matches(expectedPassId, EffectStage::BeforeOpaque, desc.ExpectedSurface) &&
         !desc.ScheduleAuthorization.Matches(expectedPassId, EffectStage::BeforeUi, desc.ExpectedSurface)) ||
        desc.ExpectedSurface.Namespace != desc.RenderTargetNamespace ||
        desc.ExpectedSurface.Resource != desc.ColorPhysicalAddress ||
        desc.Settings.Mode != DirectionalShadowMode::VirtualShadowMaps || !sceneView.Active() ||
        sceneView.FrameId() != desc.FrameId || sceneView.PicaFrame() == nullptr || !sceneView.PicaFrame()->Active() ||
        sceneView.PicaFrame()->FrameId() != desc.FrameId || sceneView.PicaFrame()->FrameSlot() != desc.FrameSlot ||
        !shadowMapBarrierPlan.Valid() || mapBarrier == nullptr || staticBarrier == nullptr ||
        staticBarrier->DispatchAccess != ResourceAccess::DepthAttachment ||
        staticBarrier->CompletionAccess != ResourceAccess::TransferRead ||
        mapBarrier->DispatchAccess != ResourceAccess::TransferWrite ||
        mapBarrier->IntermediateAccess != ResourceAccess::DepthAttachment ||
        mapBarrier->CompletionAccess != ResourceAccess::ShaderRead || !mapBarrier->DownstreamRead)
        return false;
    mImpl->LastStage = NriDirectionalShadowExecutionStage::InputAccepted;

    const auto& sceneFrame = *sceneView.PicaFrame();
    std::array<const PicaSceneDrawRecord*, kMaximumCasters> casterStorage{};
    size_t casterCount = 0;
    const auto sceneDraws = sceneFrame.Draws();
    for (size_t index = 0; index < sceneDraws.size(); ++index) {
        const auto& caster = sceneDraws[index];
        if (!sceneFrame.DirectionalShadowCaster(index) || !caster.NativeTransform.CurrentViewToWorldAvailable ||
            !caster.NativeTransform.CurrentClipToWorldAvailable || !caster.LightingWorldToView ||
            caster.RenderTarget.RenderTargetNamespace != desc.RenderTargetNamespace ||
            caster.RenderTarget.ColorPhysicalAddress != desc.ColorPhysicalAddress)
            continue;
        if (casterCount == casterStorage.size())
            return false;
        casterStorage[casterCount++] = &caster;
    }
    const auto casters = std::span(casterStorage).first(casterCount);
    if (casters.empty())
        return false;
    mImpl->LastStage = NriDirectionalShadowExecutionStage::CastersMatched;
    PicaDirectionalShadowLightSelection light;
    const bool authoredKey = desc.EnvironmentKey && desc.EnvironmentKey->Intensity > 0.0F;
    if (authoredKey) {
        light.WorldDirectionTowardSource = desc.EnvironmentKey->DirectionTowardSource;
        light.ReferenceDraw = casters.front();
        light.CandidateCount = light.ClusterCount = 1;
        light.GeometryLightWeight = desc.EnvironmentKey->Intensity;
    } else {
        if (desc.NativeDirectStrength <= 0.0F) return false;
        light = ResolvePicaDirectionalShadowLightSelection(casters);
    }
    if (!light.Valid())
        return false;
    mImpl->LastLight = { light.WorldDirectionTowardSource, light.CandidateCount, light.ClusterCount,
                         light.GeometryLightWeight };
    mImpl->LastStage = NriDirectionalShadowExecutionStage::NativeLightResolved;

    // Use the exact camera captured with this draw, for both authored and
    // native lights. Later gameplay camera samples cannot move existing pages.
    DirectionalShadowMatrix viewToWorld;
    if (!InvertDirectionalShadowMatrix(*light.ReferenceDraw->LightingWorldToView, viewToWorld))
        return false;
    DirectionalShadowFrameInput input;
    input.CameraWorld = { viewToWorld[12], viewToWorld[13], viewToWorld[14] };
    input.LightDirectionTowardSource = light.WorldDirectionTowardSource;
    input.Settings = desc.Settings;
    const auto previous = mImpl->Histories.find({ desc.RenderTargetNamespace, desc.ColorPhysicalAddress });
    if (previous != mImpl->Histories.end() && previous->second.Slot.State.Valid())
        input.PreviousLayout = &previous->second.Slot.State.Virtual;
    const auto plan = BuildDirectionalShadowFramePlan(input);
    if (!plan.Ready())
        return false;
    mImpl->LastLight.WorldDirectionTowardSource = plan.LightDirectionTowardSource;
    mImpl->LastStage = NriDirectionalShadowExecutionStage::FramePlanReady;
    // A caster whose program cannot be built casts nothing; it never fails
    // the map for the others. A discarding alpha test without its program
    // never becomes a solid caster.
    std::array<nri::Pipeline*, kMaximumCasters> pipelines{};
    size_t drawableCount = 0;
    for (size_t index = 0; index < casterCount; ++index) {
        const auto& material = casterStorage[index]->Material;
        if (material.AlphaTestEnabled && material.AlphaCompare != PicaCompareFunction::Always &&
            !UsesAlphaCasterProgram(*casterStorage[index]))
            continue;
        auto* pipeline = mImpl->GetOrCreatePipeline(*casterStorage[index], plan);
        if (pipeline == nullptr)
            continue;
        pipelines[drawableCount] = pipeline;
        casterStorage[drawableCount++] = casterStorage[index];
    }
    casterCount = drawableCount;
    const auto drawable = std::span(casterStorage).first(casterCount);
    auto* target = mImpl->EnsureTarget({ desc.RenderTargetNamespace, desc.ColorPhysicalAddress },
                                       plan.Virtual.AtlasResolution, desc.FrameId);
    if (target == nullptr)
        return false;
    auto& history = target->Slot;
    history.State = {};
    mImpl->LastStage = NriDirectionalShadowExecutionStage::ResourcesReady;
    std::array<Renderer3ds::VirtualShadowCaster, kMaximumCasters> geometry{};
    for (size_t index = 0; index < casterCount; ++index) {
        geometry[index] = drawable[index]->ShadowCaster;
        if (geometry[index].ContentVersion != 0)
            geometry[index].ContentVersion =
                HashValue(geometry[index].ContentVersion, mImpl->PipelineKey(*drawable[index], plan));
    }
    target->Cache.Prepare(plan.Virtual, std::span(geometry).first(casterCount), desc.FrameId);
    mImpl->LastLight.Pages = target->Cache.Statistics();
    mImpl->LastLight.UpdatedPages = static_cast<uint32_t>(target->Cache.Updates().size());
    mImpl->LastLight.AtlasBytes =
        uint64_t(plan.Virtual.AtlasResolution) * plan.Virtual.AtlasResolution * sizeof(float) * 2U;

    auto* command = NriInteropAccess::CommandBuffer(*mImpl->Interop, desc.FrameSlot);
    auto& frame = mImpl->Frames[desc.FrameSlot % kFrameSlots];
    if (command == nullptr || frame.CasterPool == nullptr || frame.ShadowUniformMapped == nullptr)
        return false;
    if (frame.FrameId != desc.FrameId) {
        for (auto* view : frame.TransientViews)
            mImpl->Core->DestroyDescriptor(view);
        frame.TransientViews.clear();
        frame.FrameId = desc.FrameId;
        frame.CastersUsed = 0;
    }
    std::array<Impl::PreparedCaster, kMaximumCasters> prepared{};
    std::array<bool, kMaximumCasters> required{};
    for (const auto& page : target->Cache.Updates()) {
        for (auto index : target->Cache.StaticCasters(page))
            required[index] = true;
        for (auto index : target->Cache.DynamicCasters(page))
            required[index] = true;
    }
    for (size_t index = 0; index < casterCount; ++index) {
        if (!required[index])
            continue;
        const auto& caster = *drawable[index];
        auto& draw = prepared[index];
        draw.Pipeline = pipelines[index];
        if (draw.Pipeline == nullptr || !mImpl->BindCaster(frame, sceneFrame, caster, plan.Virtual.WorldToLight, draw))
            return false;
    }

    mImpl->LastShadowMapBarrierExecution = shadowMapBarrierPlan.BeginExecution();
    const auto transition = [&](VkImage image, ResourceAccess access) {
        const auto change = mImpl->OwnedStates.PlanTransition(reinterpret_cast<uintptr_t>(image), { access, 0 });
        if (!NriInteropAccess::CmdTextureBarrier(*mImpl->Interop, desc.FrameSlot, image, change))
            return false;
        mImpl->LastShadowMapBarrierExecution.RecordGraphTransition(change);
        mImpl->OwnedStates.Commit(change);
        return true;
    };
    constexpr uint32_t pageSize = Renderer3ds::kVirtualShadowPhysicalPageTexels;
    const uint32_t pagesAcross = plan.Virtual.AtlasResolution / pageSize;
    const auto rasterize = [&](VkImage image, bool staticLayer) {
        nri::RenderingDesc rendering{};
        rendering.depth.descriptor = NriInteropAccess::DepthAttachmentView(*mImpl->Interop, image);
        if (rendering.depth.descriptor == nullptr)
            return false;
        rendering.depth.loadOp = nri::LoadOp::LOAD;
        rendering.depth.storeOp = nri::StoreOp::STORE;
        mImpl->Core->CmdSetDescriptorPool(*command, *frame.CasterPool);
        mImpl->Core->CmdBeginRendering(*command, rendering);
        mImpl->Core->CmdSetPipelineLayout(*command, nri::BindPoint::GRAPHICS, *mImpl->ShadowLayout);
        nri::Pipeline* boundPipeline = nullptr;
        for (const auto& page : target->Cache.Updates()) {
            if (staticLayer ? !page.RefreshStatic : page.DynamicCasterCount == 0)
                continue;
            const uint32_t x = (page.PhysicalPage % pagesAcross) * pageSize;
            const uint32_t y = (page.PhysicalPage / pagesAcross) * pageSize;
            const nri::Rect scissor{ static_cast<int32_t>(x), static_cast<int32_t>(y), pageSize, pageSize };
            if (staticLayer) {
                nri::ClearAttachmentDesc clear{};
                clear.planes = nri::PlaneBits::DEPTH;
                clear.value.depthStencil.depth = 1;
                mImpl->Core->CmdClearAttachments(*command, &clear, 1, &scissor, 1);
            }
            const nri::Viewport viewport{ float(x), float(y), float(pageSize), float(pageSize), 0, 1, true };
            mImpl->Core->CmdSetViewports(*command, &viewport, 1);
            mImpl->Core->CmdSetScissors(*command, &scissor, 1);
            constexpr float coverage = float(Renderer3ds::kVirtualShadowPageTexels) / pageSize;
            const float scale = 2.0F * coverage / plan.Virtual.Levels[page.Key.Level].PageWorldSize;
            const std::array<float, 4> pageTransform{ scale, scale, (-2.0F * float(page.Key.X) - 1.0F) * coverage,
                                                      (-2.0F * float(page.Key.Y) - 1.0F) * coverage };
            const nri::SetRootConstantsDesc constants{ 0, pageTransform.data(), sizeof(pageTransform) };
            mImpl->Core->CmdSetRootConstants(*command, constants);
            const auto indices =
                staticLayer ? target->Cache.StaticCasters(page) : target->Cache.DynamicCasters(page);
            for (auto index : indices) {
                const auto& caster = *drawable[index];
                const auto& draw = prepared[index];
                if (boundPipeline != draw.Pipeline) {
                    mImpl->Core->CmdSetPipeline(*command, *draw.Pipeline);
                    boundPipeline = draw.Pipeline;
                }
                const nri::SetDescriptorSetDesc set{ 0, draw.Set, nri::BindPoint::GRAPHICS };
                mImpl->Core->CmdSetDescriptorSet(*command, set);
                for (const auto& binding : sceneFrame.VertexBindings(caster)) {
                    const nri::VertexBufferDesc vertex{ draw.Geometry, binding.Offset, binding.Stride };
                    mImpl->Core->CmdSetVertexBuffers(*command, binding.Binding, &vertex, 1);
                }
                if (caster.Indexed) {
                    mImpl->Core->CmdSetIndexBuffer(*command, *draw.Geometry, caster.IndexOffset,
                                                   nri::IndexType::UINT16);
                    const nri::DrawIndexedDesc nativeDraw{ caster.VertexOrIndexCount, 1, 0, caster.BaseVertex, 0 };
                    mImpl->Core->CmdDrawIndexed(*command, nativeDraw);
                } else {
                    const nri::DrawDesc nativeDraw{ caster.VertexOrIndexCount, 1, 0, 0 };
                    mImpl->Core->CmdDraw(*command, nativeDraw);
                }
                ++mImpl->LastCasters;
                if (UsesAlphaCasterProgram(caster))
                    ++mImpl->LastAlphaCasters;
            }
        }
        mImpl->Core->CmdEndRendering(*command);
        return true;
    };
    if (target->Cache.Statistics().StaticUpdated != 0 &&
        (!transition(target->StaticTexture.Image, staticBarrier->DispatchAccess) ||
         !rasterize(target->StaticTexture.Image, true)))
        return false;
    if (!target->Cache.Updates().empty()) {
        auto* source = NriInteropAccess::Texture(*mImpl->Interop, target->StaticTexture.Image);
        auto* destination = NriInteropAccess::Texture(*mImpl->Interop, history.Texture.Image);
        if (source == nullptr || destination == nullptr ||
            !transition(target->StaticTexture.Image, staticBarrier->CompletionAccess) ||
            !transition(history.Texture.Image, mapBarrier->DispatchAccess))
            return false;
        // Restore every changed page, even when its last actor has left.
        // Unchanged final pages remain untouched and retain their exact depth.
        bool dynamicDraws = false;
        for (const auto& page : target->Cache.Updates()) {
            nri::TextureRegionDesc region{};
            region.x = static_cast<nri::Dim_t>((page.PhysicalPage % pagesAcross) * pageSize);
            region.y = static_cast<nri::Dim_t>((page.PhysicalPage / pagesAcross) * pageSize);
            region.width = region.height = pageSize;
            region.depth = 1;
            region.planes = nri::PlaneBits::DEPTH;
            mImpl->Core->CmdCopyTexture(*command, *destination, &region, *source, &region);
            dynamicDraws |= page.DynamicCasterCount != 0;
        }
        if (dynamicDraws &&
            (!transition(history.Texture.Image, mapBarrier->IntermediateAccess) ||
             !rasterize(history.Texture.Image, false)))
            return false;
    }
    mImpl->LastStage = NriDirectionalShadowExecutionStage::ShadowRasterized;
    if (!transition(history.Texture.Image, mapBarrier->CompletionAccess))
        return false;
    target->Cache.Commit();
    history.State.Virtual = plan.Virtual;
    history.State.PageTable = target->Cache.Table();
    history.State.WorldLightDirectionTowardSource = plan.LightDirectionTowardSource;
    history.State.ProducedFrameId = desc.FrameId;
    history.State.Strength = plan.Strength;
    history.State.DepthBias = std::max(plan.DepthBiasConstant, 0.0F) * 0.5F / (2.0F * plan.Virtual.DepthExtent);
    history.State.PcfRadius = plan.PcfRadius;
    target->LastUsedFrameId = desc.FrameId;
    mImpl->LastNativeLight = !authoredKey;
    mImpl->LastStage = NriDirectionalShadowExecutionStage::HistoryPublished;
    return true;
#endif
}

bool NriDirectionalShadowPass::ExecutePointMaps(const NriPointShadowExecuteDesc& desc,
                                                const NativeSceneView& sceneView,
                                                const EffectPassBarrierPlan& pointShadowMapBarrierPlan) {
    mImpl->LastPoint = {};
#ifndef ENABLE_OOT3D_NRI
    (void)desc;
    (void)sceneView;
    (void)pointShadowMapBarrierPlan;
    return false;
#else
    using namespace ::Fast::Renderer3ds;
    const auto* mapBarrier = pointShadowMapBarrierPlan.Find(EffectResource::PointShadowMap);
    const auto passId =
        ::Fast::Renderer::StableExtensionPassId(PicaExtensionPassName(PicaExtensionPass::PointShadowMap));
    if (!Available() ||
        (!desc.ScheduleAuthorization.Matches(passId, EffectStage::BeforeOpaque, desc.ExpectedSurface) &&
         !desc.ScheduleAuthorization.Matches(passId, EffectStage::BeforeUi, desc.ExpectedSurface)) ||
        desc.ExpectedSurface.Namespace != desc.RenderTargetNamespace ||
        desc.ExpectedSurface.Resource != desc.ColorPhysicalAddress ||
        desc.Settings.Mode != PointShadowMode::CubeMaps || !sceneView.Active() ||
        sceneView.FrameId() != desc.FrameId || sceneView.PicaFrame() == nullptr ||
        !sceneView.PicaFrame()->Active() || sceneView.PicaFrame()->FrameId() != desc.FrameId ||
        sceneView.PicaFrame()->FrameSlot() != desc.FrameSlot || !pointShadowMapBarrierPlan.Valid() ||
        mapBarrier == nullptr || mapBarrier->DispatchAccess != ResourceAccess::DepthAttachment ||
        mapBarrier->CompletionAccess != ResourceAccess::ShaderRead || !mapBarrier->DownstreamRead)
        return false;

    // Casters of this target and the point lights their lighting sees.
    const auto& sceneFrame = *sceneView.PicaFrame();
    std::vector<const PicaSceneDrawRecord*> casters;
    std::vector<nri::Pipeline*> pipelines;
    std::array<PointShadowLight, 64> candidates{};
    size_t candidateCount = 0;
    const auto addCandidate = [&](const PointShadowLight& light) {
        if (light.SourceIdentity == 0U || light.Radius <= 0.0F || candidateCount == candidates.size() ||
            std::any_of(candidates.begin(), candidates.begin() + candidateCount,
                        [&](const auto& existing) { return existing.SourceIdentity == light.SourceIdentity; }))
            return;
        candidates[candidateCount++] = light;
    };
    const auto& environment = desc.Environment;
    std::optional<ShadowVector> camera;
    const auto draws = sceneFrame.Draws();
    for (size_t index = 0; index < draws.size(); ++index) {
        const auto& draw = draws[index];
        if (draw.RenderTarget.RenderTargetNamespace != desc.RenderTargetNamespace ||
            draw.RenderTarget.ColorPhysicalAddress != desc.ColorPhysicalAddress || !draw.LightingWorldToView)
            continue;
        DirectionalShadowMatrix viewToWorld;
        if (!camera && InvertDirectionalShadowMatrix(*draw.LightingWorldToView, viewToWorld))
            camera = ShadowVector{ viewToWorld[12], viewToWorld[13], viewToWorld[14] };
        if (draw.ScenePoints) {
            for (uint32_t light = 0; light < std::min<uint32_t>(draw.ScenePoints->Count, kMaximumPicaScenePointLights); ++light) {
                const auto& point = draw.ScenePoints->Lights[light];
                if (point.Group >= environment.PointLights.size()) continue;
                const auto& group = environment.PointLights[point.Group];
                if (!group.Enabled || group.Intensity <= 0.0F || point.Radius <= 0.0F) continue;
                addCandidate({ point.SourceIdentity, point.PositionWorld, point.Radius * group.RadiusScale,
                               std::max({ point.Color[0], point.Color[1], point.Color[2] }) * group.Intensity });
            }
        }
        if (!environment.Enabled || environment.NativeLocalStrength > 0.0F) {
            for (const auto& source : draw.NativeLightSources) {
                if (source.Kind != PicaNativeLightSource::Type::Point || source.Radius <= 0.0F) continue;
                const bool scenePoint = draw.ScenePoints &&
                    std::any_of(draw.ScenePoints->Lights.begin(), draw.ScenePoints->Lights.begin() + draw.ScenePoints->Count,
                                [&](const auto& point) { return point.SourceIdentity == source.SourceIdentity; });
                if (!scenePoint)
                    addCandidate({ source.SourceIdentity, source.PositionWorld, source.Radius,
                                   std::max({ source.UnattenuatedColor[0], source.UnattenuatedColor[1],
                                              source.UnattenuatedColor[2] }) });
            }
        }
        if (!sceneFrame.DirectionalShadowCaster(index) || !draw.NativeTransform.CurrentViewToWorldAvailable ||
            !draw.NativeTransform.CurrentClipToWorldAvailable)
            continue;
        const auto& material = draw.Material;
        if (material.AlphaTestEnabled && material.AlphaCompare != PicaCompareFunction::Always &&
            !UsesAlphaCasterProgram(draw))
            continue;
        auto* pipeline = mImpl->GetOrCreatePointPipeline(draw);
        if (pipeline == nullptr || casters.size() == kMaximumCasters)
            continue;
        casters.push_back(&draw);
        pipelines.push_back(pipeline);
    }
    mImpl->LastPoint.CandidateLights = static_cast<uint32_t>(candidateCount);
    const auto layout = BuildPointShadowLayout(desc.Settings.Resolution, desc.Settings.MaxLights);
    if (!camera || !layout.Valid())
        return false;
    auto* target = mImpl->EnsurePointTarget({ desc.RenderTargetNamespace, desc.ColorPhysicalAddress }, layout, desc.FrameId);
    if (target == nullptr)
        return false;
    const bool fresh = target->ProducedFrameId == 0U;
    const auto slots = SelectPointShadowSlots(std::span(candidates).first(candidateCount), *camera,
                                              desc.Settings.Range, layout.SlotCount, target->Slots);

    // A row is re-rendered unless its light, reach and static casters are
    // unchanged; an actor or an unproven caster in reach renders it again.
    std::array<bool, kMaximumPointShadowLights> render{};
    std::array<uint64_t, kMaximumPointShadowLights> signatures{};
    for (uint32_t row = 0; row < layout.SlotCount; ++row) {
        const auto& slot = slots[row];
        if (!slot.Valid()) continue;
        ++mImpl->LastPoint.Lights;
        uint64_t signature = HashValue(1469598103934665603ULL, slot);
        bool dynamic = false;
        for (const auto* caster : casters) {
            const auto& bounds = caster->ShadowCaster.WorldBounds;
            bool reached = false;
            for (uint32_t face = 0; face < kPointShadowFaceCount && !reached; ++face)
                reached = PointShadowFaceIntersects(slot, face, layout, bounds);
            if (!reached) continue;
            dynamic |= caster->ShadowCaster.Dynamic || caster->ShadowCaster.ContentVersion == 0U;
            signature = HashValue(signature, caster->ShadowCaster.ContentVersion);
        }
        signatures[row] = dynamic ? 0U : signature;
        render[row] = fresh || dynamic || signatures[row] != target->Signatures[row];
    }

    auto* command = NriInteropAccess::CommandBuffer(*mImpl->Interop, desc.FrameSlot);
    auto& frame = mImpl->Frames[desc.FrameSlot % kFrameSlots];
    if (command == nullptr || frame.CasterPool == nullptr || frame.ShadowUniformMapped == nullptr)
        return false;
    if (frame.FrameId != desc.FrameId) {
        for (auto* view : frame.TransientViews)
            mImpl->Core->DestroyDescriptor(view);
        frame.TransientViews.clear();
        frame.FrameId = desc.FrameId;
        frame.CastersUsed = 0;
    }
    const auto transition = [&](VkImage image, ResourceAccess access) {
        const auto change = mImpl->OwnedStates.PlanTransition(reinterpret_cast<uintptr_t>(image), { access, 0 });
        if (!NriInteropAccess::CmdTextureBarrier(*mImpl->Interop, desc.FrameSlot, image, change))
            return false;
        mImpl->OwnedStates.Commit(change);
        return true;
    };
    if (!transition(target->Texture.Image, mapBarrier->DispatchAccess))
        return false;
    nri::RenderingDesc rendering{};
    rendering.depth.descriptor = NriInteropAccess::DepthAttachmentView(*mImpl->Interop, target->Texture.Image);
    if (rendering.depth.descriptor == nullptr)
        return false;
    rendering.depth.loadOp = nri::LoadOp::LOAD;
    rendering.depth.storeOp = nri::StoreOp::STORE;
    mImpl->Core->CmdSetDescriptorPool(*command, *frame.CasterPool);
    mImpl->Core->CmdBeginRendering(*command, rendering);
    mImpl->Core->CmdSetPipelineLayout(*command, nri::BindPoint::GRAPHICS, *mImpl->ShadowLayout);
    const uint32_t tile = layout.TileTexels;
    if (fresh) {
        // A new atlas holds no depth yet: every row reads as unoccluded.
        nri::ClearAttachmentDesc clear{};
        clear.planes = nri::PlaneBits::DEPTH;
        clear.value.depthStencil.depth = 1;
        const nri::Rect whole{ 0, 0, static_cast<nri::Dim_t>(layout.Width), static_cast<nri::Dim_t>(layout.Height) };
        mImpl->Core->CmdClearAttachments(*command, &clear, 1, &whole, 1);
    }
    std::vector<Impl::PreparedCaster> prepared(casters.size());
    std::vector<bool> bound(casters.size(), false);
    nri::Pipeline* boundPipeline = nullptr;
    bool failed = false;
    for (uint32_t row = 0; row < layout.SlotCount && !failed; ++row) {
        if (!render[row]) continue;
        const auto& slot = slots[row];
        ++mImpl->LastPoint.RenderedLights;
        nri::ClearAttachmentDesc clear{};
        clear.planes = nri::PlaneBits::DEPTH;
        clear.value.depthStencil.depth = 1;
        const nri::Rect rowRect{ 0, static_cast<int32_t>(row * tile), static_cast<nri::Dim_t>(layout.Width),
                                 static_cast<nri::Dim_t>(tile) };
        mImpl->Core->CmdClearAttachments(*command, &clear, 1, &rowRect, 1);
        for (uint32_t face = 0; face < kPointShadowFaceCount && !failed; ++face) {
            const nri::Rect scissor{ static_cast<int32_t>(face * tile), static_cast<int32_t>(row * tile),
                                     static_cast<nri::Dim_t>(tile), static_cast<nri::Dim_t>(tile) };
            const nri::Viewport viewport{ float(face * tile), float(row * tile), float(tile), float(tile), 0, 1, true };
            mImpl->Core->CmdSetViewports(*command, &viewport, 1);
            mImpl->Core->CmdSetScissors(*command, &scissor, 1);
            const auto faceMatrix = BuildPointShadowFaceMatrix(slot, face, layout);
            const nri::SetRootConstantsDesc constants{ 0, faceMatrix.data(), sizeof(faceMatrix) };
            mImpl->Core->CmdSetRootConstants(*command, constants);
            for (size_t index = 0; index < casters.size(); ++index) {
                const auto& caster = *casters[index];
                if (!PointShadowFaceIntersects(slot, face, layout, caster.ShadowCaster.WorldBounds)) continue;
                auto& draw = prepared[index];
                if (!bound[index]) {
                    draw.Pipeline = pipelines[index];
                    if (!mImpl->BindCaster(frame, sceneFrame, caster, IdentityDirectionalShadowMatrix(), draw)) {
                        failed = true;
                        break;
                    }
                    bound[index] = true;
                }
                if (boundPipeline != draw.Pipeline) {
                    mImpl->Core->CmdSetPipeline(*command, *draw.Pipeline);
                    boundPipeline = draw.Pipeline;
                }
                const nri::SetDescriptorSetDesc set{ 0, draw.Set, nri::BindPoint::GRAPHICS };
                mImpl->Core->CmdSetDescriptorSet(*command, set);
                for (const auto& binding : sceneFrame.VertexBindings(caster)) {
                    const nri::VertexBufferDesc vertex{ draw.Geometry, binding.Offset, binding.Stride };
                    mImpl->Core->CmdSetVertexBuffers(*command, binding.Binding, &vertex, 1);
                }
                if (caster.Indexed) {
                    mImpl->Core->CmdSetIndexBuffer(*command, *draw.Geometry, caster.IndexOffset, nri::IndexType::UINT16);
                    const nri::DrawIndexedDesc nativeDraw{ caster.VertexOrIndexCount, 1, 0, caster.BaseVertex, 0 };
                    mImpl->Core->CmdDrawIndexed(*command, nativeDraw);
                } else {
                    const nri::DrawDesc nativeDraw{ caster.VertexOrIndexCount, 1, 0, 0 };
                    mImpl->Core->CmdDraw(*command, nativeDraw);
                }
                ++mImpl->LastPoint.CasterDraws;
                if (UsesAlphaCasterProgram(caster))
                    ++mImpl->LastPoint.AlphaCasterDraws;
            }
        }
    }
    mImpl->Core->CmdEndRendering(*command);
    if (!transition(target->Texture.Image, mapBarrier->CompletionAccess) || failed) {
        target->ProducedFrameId = 0U;
        target->Signatures.fill(0U);
        return false;
    }
    target->Slots = slots;
    target->Signatures = signatures;
    target->Range = desc.Settings.Range;
    target->Strength = desc.Settings.Strength;
    target->ProducedFrameId = desc.FrameId;
    target->LastUsedFrameId = desc.FrameId;
    mImpl->LastPoint.AtlasBytes = uint64_t(layout.Width) * layout.Height * sizeof(float);
    return true;
#endif
}

NriPointShadowHistorySnapshot NriDirectionalShadowPass::FindLatestPointHistory(
    uint64_t renderTargetNamespace, uint32_t colorPhysicalAddress, uint64_t atFrameId) const noexcept {
    NriPointShadowHistorySnapshot result;
    const auto found = mImpl->PointTargets.find({ renderTargetNamespace, colorPhysicalAddress });
    if (!Available() || atFrameId == 0U || found == mImpl->PointTargets.end())
        return result;
    const auto& target = found->second;
    if (target.ProducedFrameId == 0U || target.ProducedFrameId > atFrameId)
        return result;
    result.Image = target.Texture.Image;
    result.Layout = target.Layout;
    result.Slots = target.Slots;
    result.Range = target.Range;
    result.Strength = target.Strength;
    result.ProducedFrameId = target.ProducedFrameId;
    return result.Valid() ? result : NriPointShadowHistorySnapshot{};
}

const NriPointShadowTelemetry& NriDirectionalShadowPass::LastPointTelemetry() const {
    return mImpl->LastPoint;
}

NriDirectionalShadowHistorySnapshot NriDirectionalShadowPass::FindLatestHistory(uint64_t renderTargetNamespace,
                                                                                uint32_t colorPhysicalAddress,
                                                                                uint64_t atFrameId) const noexcept {
    NriDirectionalShadowHistorySnapshot result;
    if (!Available() || atFrameId == 0)
        return result;
    const auto found = mImpl->Histories.find({ renderTargetNamespace, colorPhysicalAddress });
    if (found == mImpl->Histories.end())
        return result;
    const auto& slot = found->second.Slot;
    if (!slot.State.Valid() || slot.State.ProducedFrameId > atFrameId)
        return result;
    result.Image = slot.Texture.Image;
    result.View = slot.Texture.SampledView;
    result.Sampler = mImpl->ReceiverSampler;
    result.State = slot.State;
    return result.Valid() ? result : NriDirectionalShadowHistorySnapshot{};
}

void NriDirectionalShadowPass::InvalidateHistory() noexcept {
    for (auto& [key, history] : mImpl->Histories) {
        history.Slot.State = {};
        history.Cache.Invalidate();
    }
    for (auto& [key, point] : mImpl->PointTargets) {
        (void)key;
        point.ProducedFrameId = 0U;
        point.Signatures.fill(0U);
    }
}

void NriDirectionalShadowPass::InvalidateScreenResources() {
    if (mImpl->Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(mImpl->Device);
    }
    mImpl->DestroyOwnedTextures();
}

void NriDirectionalShadowPass::Shutdown() {
    mImpl->DestroyOwnedTextures();
#ifdef ENABLE_OOT3D_NRI
    if (mImpl->Core != nullptr) {
        for (auto& frame : mImpl->Frames) {
            for (nri::Descriptor* view : frame.TransientViews) {
                mImpl->Core->DestroyDescriptor(view);
            }
            if (frame.ShadowUniformMapped != nullptr && frame.ShadowUniformBuffer != nullptr) {
                mImpl->Core->UnmapBuffer(*frame.ShadowUniformBuffer);
            }
            for (nri::Descriptor* view : frame.ShadowUniformViews) {
                if (view != nullptr) {
                    mImpl->Core->DestroyDescriptor(view);
                }
            }
            if (frame.ShadowUniformBuffer != nullptr) {
                mImpl->Core->DestroyBuffer(frame.ShadowUniformBuffer);
            }
            if (frame.CasterPool != nullptr) {
                mImpl->Core->DestroyDescriptorPool(frame.CasterPool);
            }
        }
        for (auto& [key, pipeline] : mImpl->ShadowPipelines) {
            (void)key;
            mImpl->Core->DestroyPipeline(pipeline);
        }
        for (auto& [key, pipeline] : mImpl->PointPipelines) {
            (void)key;
            mImpl->Core->DestroyPipeline(pipeline);
        }
        for (auto& [key, sampler] : mImpl->CasterSamplers) {
            (void)key;
            mImpl->Core->DestroyDescriptor(sampler);
        }
        if (mImpl->ShadowLayout != nullptr) {
            mImpl->Core->DestroyPipelineLayout(mImpl->ShadowLayout);
        }
    }
#endif
    if (mImpl->ReceiverSampler != VK_NULL_HANDLE && mImpl->Device != VK_NULL_HANDLE) {
        vkDestroySampler(mImpl->Device, mImpl->ReceiverSampler, nullptr);
    }
    *mImpl = {};
    mImpl->Reason = "NRI directional shadow pass is not initialized";
}

bool NriDirectionalShadowPass::Available() const {
#ifdef ENABLE_OOT3D_NRI
    return mImpl->Reason.empty() && mImpl->Interop != nullptr && mImpl->Core != nullptr &&
           mImpl->ShadowLayout != nullptr && mImpl->ReceiverSampler != VK_NULL_HANDLE;
#else
    return false;
#endif
}

bool NriDirectionalShadowPass::LastExecutionUsedNativeLight() const {
    return mImpl->LastNativeLight;
}

uint32_t NriDirectionalShadowPass::LastCasterCount() const {
    return mImpl->LastCasters;
}

uint32_t NriDirectionalShadowPass::LastAlphaCasterCount() const {
    return mImpl->LastAlphaCasters;
}

const NriDirectionalShadowLightTelemetry& NriDirectionalShadowPass::LastLightTelemetry() const {
    return mImpl->LastLight;
}

NriDirectionalShadowExecutionStage NriDirectionalShadowPass::LastExecutionStage() const {
    return mImpl->LastStage;
}

const EffectPassBarrierExecution& NriDirectionalShadowPass::LastShadowMapBarrierExecution() const {
    return mImpl->LastShadowMapBarrierExecution;
}

const std::string& NriDirectionalShadowPass::UnavailableReason() const {
    return mImpl->Reason;
}

} // namespace Fast::Oot3d

#endif
