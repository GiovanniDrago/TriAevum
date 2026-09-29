#include "fast/oot3d/brixelizer_gi_pass.h"

#ifdef ENABLE_OOT3D_VULKAN

#include "fast/oot3d/brixelizer_scene.h"
#include "fast/oot3d/nri_effect_graph_transient_image_arena.h"
#include "fast/oot3d/nri_interop_context.h"
#include "fast/oot3d/nri_storage_clear_binding.h"
#include "fast/oot3d/resource_state_tracker.h"

#if defined(ENABLE_OOT3D_NRI) && defined(ENABLE_OOT3D_BRIXELIZER_GI) && \
    __has_include(<FidelityFX/host/backends/vk/ffx_vk.h>) && __has_include(<FidelityFX/host/ffx_brixelizergi.h>)
#define OOT3D_HAS_BRIXELIZER_GI_PASS 1
#include "brixelizer_gi_access.h"
#include "nri_interop_internal.h"
#else
#define OOT3D_HAS_BRIXELIZER_GI_PASS 0
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace Fast::Oot3d {
namespace {

// Linear lit colour, depth and world normals of one presented sample: the
// current inputs of the GI dispatch and, one sample later, its histories.
constexpr const char* kPrepareShader = R"glsl(#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x=8,local_size_y=8) in;
layout(set=0,binding=0) uniform texture2D scene_color;
layout(set=0,binding=1) uniform texture2D native_depth;
layout(set=0,binding=2) uniform texture2D normal_guide;
layout(set=0,binding=3,rgba16f) uniform writeonly image2D lit_output;
layout(set=0,binding=4,r32f) uniform writeonly image2D depth_output;
layout(set=0,binding=5,rgba16f) uniform writeonly image2D normal_output;
layout(push_constant) uniform Prepare { mat4 view_to_world; } pc;
vec3 srgb_to_linear(vec3 value) {
    value=max(value,vec3(0.0));
    return mix(pow((value+0.055)/1.055,vec3(2.4)),value/12.92,lessThanEqual(value,vec3(0.04045)));
}
void main() {
    ivec2 p=ivec2(gl_GlobalInvocationID.xy);
    ivec2 extent=imageSize(lit_output);
    if(any(greaterThanEqual(p,extent))) return;
    ivec2 colorTexel=ivec2((vec2(p)+0.5)*vec2(textureSize(scene_color,0))/vec2(extent));
    imageStore(lit_output,p,vec4(srgb_to_linear(texelFetch(scene_color,colorTexel,0).rgb),1.0));
    imageStore(depth_output,p,vec4(texelFetch(native_depth,p,0).r));
    vec3 n=texelFetch(normal_guide,p,0).xyz*2.0-1.0;
    n=normalize(mat3(pc.view_to_world)*n);
    imageStore(normal_output,p,vec4(n*0.5+0.5,1.0));
}
)glsl";

std::array<float, 16> MultiplyColumnMajor(const std::array<float, 16>& a, const std::array<float, 16>& b) {
    std::array<float, 16> out{};
    for (size_t column = 0; column < 4; ++column)
        for (size_t row = 0; row < 4; ++row)
            for (size_t k = 0; k < 4; ++k) out[column * 4 + row] += a[k * 4 + row] * b[column * 4 + k];
    return out;
}

VkImageCreateInfo ImageInfo(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1U};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
}

} // namespace

struct BrixelizerGiInstance {
    BrixelizerGiInputs Inputs;
    GlobalIlluminationResolution Resolution = GlobalIlluminationResolution::Half;
    // Prepared inputs, alternating: [Current] this sample, the other the history.
    std::array<NriOwnedTexture2D, 2> Lit{};
    std::array<NriOwnedTexture2D, 2> Depth{};
    std::array<NriOwnedTexture2D, 2> Normal{};
    // Required by the dispatch even with specular disabled.
    NriOwnedTexture2D Specular{};
    // GI of this surface's latest sample. The GI context runs once per
    // presented frame (its history and the backend's per-dispatch resource
    // views assume it); every composition of the surface copies this.
    NriOwnedTexture2D Output{};
    ResourceStateTracker States;
    uint32_t Current = 0;
    bool HistoryPrepared = false;
    uint64_t LastUsedFrame = 0;
    uint64_t LastTracedFrame = 0;
    bool Traced = false;
#if OOT3D_HAS_BRIXELIZER_GI_PASS
    nri::DescriptorPool* Pool = nullptr;
    std::array<nri::DescriptorSet*, 2> Sets{};
    FfxBrixelizerGIContext Context{};
    bool ContextCreated = false;
#endif
};

struct BrixelizerGiPass::Impl {
    VkDevice Device = VK_NULL_HANDLE;
    NriInteropContext* Interop = nullptr;
    BrixelizerScene* Scene = nullptr;
    std::vector<std::unique_ptr<BrixelizerGiInstance>> Instances;
    BrixelizerGiInstance* Active = nullptr;
    EffectPassBarrierExecution LastBarriers;
    NriStorageClearBinding Clear;
    bool Initialized = false;
    std::string Reason = "Brixelizer GI is not initialized";
#if OOT3D_HAS_BRIXELIZER_GI_PASS
    nri::CoreInterface* Core = nullptr;
    nri::PipelineLayout* Layout = nullptr;
    nri::Pipeline* Pipeline = nullptr;

    void Destroy(BrixelizerGiInstance& instance) {
        if (instance.ContextCreated) {
            ffxBrixelizerGIContextDestroy(&instance.Context);
            instance.ContextCreated = false;
        }
        if (instance.Pool != nullptr) Core->DestroyDescriptorPool(instance.Pool);
        instance.Pool = nullptr;
        instance.Sets = {};
        for (auto* textures : {&instance.Lit, &instance.Depth, &instance.Normal})
            for (auto& texture : *textures)
                if (texture.Image != VK_NULL_HANDLE) Interop->DestroyOwnedTexture(texture.Image);
        if (instance.Specular.Image != VK_NULL_HANDLE) Interop->DestroyOwnedTexture(instance.Specular.Image);
        if (instance.Output.Image != VK_NULL_HANDLE) Interop->DestroyOwnedTexture(instance.Output.Image);
        instance.Lit = instance.Depth = instance.Normal = {};
        instance.Specular = instance.Output = {};
        instance.States.Clear();
    }
    void Create(BrixelizerGiInstance& instance, FfxInterface& backend) {
        const auto& in = instance.Inputs;
        const auto allocate = [&](NriOwnedTexture2D& texture, VkFormat format) {
            if (!Interop->CreateOwnedTexture2D(in.Width, in.Height, format, texture))
                throw std::runtime_error("Brixelizer GI image allocation failed");
        };
        for (uint32_t i = 0; i < 2; ++i) {
            allocate(instance.Lit[i], VK_FORMAT_R16G16B16A16_SFLOAT);
            allocate(instance.Depth[i], VK_FORMAT_R32_SFLOAT);
            allocate(instance.Normal[i], VK_FORMAT_R16G16B16A16_SFLOAT);
        }
        allocate(instance.Specular, VK_FORMAT_R16G16B16A16_SFLOAT);
        allocate(instance.Output, VK_FORMAT_R16G16B16A16_SFLOAT);
        for (const auto& [image, format] : {std::pair{in.SceneColor, in.SceneColorFormat},
                                            std::pair{in.Depth, in.DepthFormat},
                                            std::pair{in.NormalGuide, in.NormalGuideFormat}})
            if (!Interop->WrapTexture(image, format, VK_IMAGE_TYPE_2D, VK_IMAGE_USAGE_SAMPLED_BIT, in.Width,
                                      in.Height))
                throw std::runtime_error("Brixelizer GI input wrapping failed");
        nri::DescriptorPoolDesc pool{};
        pool.descriptorSetMaxNum = 2;
        pool.textureMaxNum = 6;
        pool.storageTextureMaxNum = 6;
        auto* device = NriInteropAccess::Device(*Interop);
        if (Core->CreateDescriptorPool(*device, pool, instance.Pool) != nri::Result::SUCCESS ||
            Core->AllocateDescriptorSets(*instance.Pool, *Layout, 0, instance.Sets.data(), 2, 0) !=
                nri::Result::SUCCESS)
            throw std::runtime_error("Brixelizer GI descriptor allocation failed");
        for (uint32_t i = 0; i < 2; ++i) {
            std::array<nri::Descriptor*, 6> views{
                NriInteropAccess::TextureView(*Interop, in.SceneColor, false),
                NriInteropAccess::TextureView(*Interop, in.Depth, false),
                NriInteropAccess::TextureView(*Interop, in.NormalGuide, false),
                NriInteropAccess::TextureView(*Interop, instance.Lit[i].Image, true),
                NriInteropAccess::TextureView(*Interop, instance.Depth[i].Image, true),
                NriInteropAccess::TextureView(*Interop, instance.Normal[i].Image, true)};
            for (auto* view : views)
                if (view == nullptr) throw std::runtime_error("Brixelizer GI descriptors unavailable");
            std::array<nri::UpdateDescriptorRangeDesc, 6> updates{};
            for (uint32_t range = 0; range < 6; ++range)
                updates[range] = {instance.Sets[i], range, 0, &views[range], 1};
            Core->UpdateDescriptorRanges(updates.data(), static_cast<uint32_t>(updates.size()));
        }
        FfxBrixelizerGIContextDescription description{};
        description.flags = FFX_BRIXELIZER_GI_FLAG_DISABLE_SPECULAR;
        description.internalResolution = instance.Resolution == GlobalIlluminationResolution::Quarter
                                             ? FFX_BRIXELIZER_GI_INTERNAL_RESOLUTION_25_PERCENT
                                             : FFX_BRIXELIZER_GI_INTERNAL_RESOLUTION_50_PERCENT;
        description.displaySize = {in.Width, in.Height};
        description.backendInterface = backend;
        if (ffxBrixelizerGIContextCreate(&instance.Context, &description) != FFX_OK)
            throw std::runtime_error("Brixelizer GI context creation failed");
        instance.ContextCreated = true;
    }
#endif
};

BrixelizerGiPass::BrixelizerGiPass() : mImpl(std::make_unique<Impl>()) {}
BrixelizerGiPass::~BrixelizerGiPass() { Shutdown(); }

bool BrixelizerGiPass::Initialize(VkPhysicalDevice physicalDevice, VkDevice device, NriInteropContext& interop,
                                  BrixelizerScene& scene) {
    Shutdown();
    mImpl->Device = device;
    mImpl->Interop = &interop;
    mImpl->Scene = &scene;
#if !OOT3D_HAS_BRIXELIZER_GI_PASS
    (void)physicalDevice;
    mImpl->Reason = "Brixelizer GI is not built into this renderer";
    return false;
#else
    try {
        if (!interop.Available()) throw std::runtime_error(interop.UnavailableReason());
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, VK_FORMAT_R32_SFLOAT, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0U)
            throw std::runtime_error("R32F storage images are unavailable");
        mImpl->Core = NriInteropAccess::Core(interop);
        auto* nriDevice = NriInteropAccess::Device(interop);
        if (mImpl->Core == nullptr || nriDevice == nullptr)
            throw std::runtime_error("Brixelizer GI NRI device unavailable");
        std::array<nri::DescriptorRangeDesc, 6> ranges{};
        for (uint32_t i = 0; i < 6; ++i)
            ranges[i] = {i, 1, i < 3 ? nri::DescriptorType::TEXTURE : nri::DescriptorType::STORAGE_TEXTURE,
                         nri::StageBits::COMPUTE_SHADER};
        nri::DescriptorSetDesc set{};
        set.ranges = ranges.data();
        set.rangeNum = static_cast<uint32_t>(ranges.size());
        nri::RootConstantDesc root{};
        root.size = sizeof(float) * 16;
        root.shaderStages = nri::StageBits::COMPUTE_SHADER;
        nri::PipelineLayoutDesc layout{};
        layout.rootConstants = &root;
        layout.rootConstantNum = 1;
        layout.descriptorSets = &set;
        layout.descriptorSetNum = 1;
        layout.shaderStages = nri::StageBits::COMPUTE_SHADER;
        layout.flags = nri::PipelineLayoutBits::IGNORE_GLOBAL_SPIRV_OFFSETS;
        if (mImpl->Core->CreatePipelineLayout(*nriDevice, layout, mImpl->Layout) != nri::Result::SUCCESS)
            throw std::runtime_error("Brixelizer GI layout creation failed");
        const auto code =
            interop.Shaders().Resolve(kPrepareShader, Renderer::SpirvStage::Compute, "brixelizer_gi_prepare.comp");
        nri::ComputePipelineDesc pipeline{};
        pipeline.pipelineLayout = mImpl->Layout;
        pipeline.shader.stage = nri::StageBits::COMPUTE_SHADER;
        pipeline.shader.bytecode = code.data();
        pipeline.shader.size = code.size() * sizeof(uint32_t);
        if (mImpl->Core->CreateComputePipeline(*nriDevice, pipeline, mImpl->Pipeline) != nri::Result::SUCCESS)
            throw std::runtime_error("Brixelizer GI prepare pipeline creation failed");
        if (!mImpl->Clear.Initialize(interop)) throw std::runtime_error(mImpl->Clear.UnavailableReason());
        mImpl->Initialized = true;
        mImpl->Reason.clear();
        return true;
    } catch (const std::exception& exception) {
        mImpl->Reason = exception.what();
        return false;
    }
#endif
}

bool BrixelizerGiPass::Configure(uint64_t frameId, const BrixelizerGiInputs& inputs,
                                 GlobalIlluminationResolution resolution) {
    mImpl->Active = nullptr;
    if (!mImpl->Initialized || inputs.Width == 0U || inputs.Height == 0U || inputs.SceneColor == VK_NULL_HANDLE ||
        inputs.Depth == VK_NULL_HANDLE || inputs.NormalGuide == VK_NULL_HANDLE ||
        inputs.MaterialGuide == VK_NULL_HANDLE || inputs.Motion == VK_NULL_HANDLE) {
        mImpl->Reason = "Brixelizer GI inputs are unavailable";
        return false;
    }
    for (auto& instance : mImpl->Instances) {
        if (instance->Inputs == inputs && instance->Resolution == resolution) {
            instance->LastUsedFrame = frameId;
            mImpl->Active = instance.get();
            return true;
        }
    }
#if !OOT3D_HAS_BRIXELIZER_GI_PASS
    (void)frameId;
    return false;
#else
    FfxInterface* backend = BrixelizerSceneGiAccess::Backend(*mImpl->Scene);
    if (backend == nullptr) {
        mImpl->Reason = "the scene distance field is unavailable";
        return false;
    }
    if (mImpl->Instances.size() >= BrixelizerScene::kMaxGiSurfaces) {
        // Retire the least recently used surface, never one this frame's
        // command buffer already references.
        auto oldest = std::min_element(mImpl->Instances.begin(), mImpl->Instances.end(),
                                       [](const auto& a, const auto& b) { return a->LastUsedFrame < b->LastUsedFrame; });
        if ((*oldest)->LastUsedFrame >= frameId) {
            mImpl->Reason = "Brixelizer GI has no free surface instance this frame";
            return false;
        }
        if (!mImpl->Interop->CompleteRecordedWork()) {
            mImpl->Reason = "Brixelizer GI retirement wait failed";
            return false;
        }
        mImpl->Destroy(**oldest);
        mImpl->Instances.erase(oldest);
    }
    auto instance = std::make_unique<BrixelizerGiInstance>();
    instance->Inputs = inputs;
    instance->Resolution = resolution;
    instance->LastUsedFrame = frameId;
    try {
        mImpl->Create(*instance, *backend);
    } catch (const std::exception& exception) {
        mImpl->Reason = exception.what();
        mImpl->Destroy(*instance);
        return false;
    }
    mImpl->Active = instance.get();
    mImpl->Instances.push_back(std::move(instance));
    mImpl->Reason.clear();
    return true;
#endif
}

bool BrixelizerGiPass::Execute(VkCommandBuffer commandBuffer, uint32_t frameSlot, uint64_t frameId,
                               const TemporalViewState& temporal, const PerspectiveViewState& view,
                               PicaSurfaceCoordinates coordinates, const NriEffectGraphTransientImageBinding& output,
                               const EffectPassBarrierPlan& barriers) {
    mImpl->LastBarriers = {};
#if !OOT3D_HAS_BRIXELIZER_GI_PASS
    (void)commandBuffer, (void)frameSlot, (void)frameId, (void)temporal, (void)view, (void)coordinates, (void)output,
        (void)barriers;
    return false;
#else
    const auto* outputBarrier = barriers.Find(EffectResource::DiffuseIllumination);
    if (mImpl->Active == nullptr || !mImpl->Active->ContextCreated || commandBuffer == VK_NULL_HANDLE ||
        !barriers.Valid() || outputBarrier == nullptr || !outputBarrier->Managed() || !output.Valid() ||
        output.Resource != EffectResource::DiffuseIllumination || output.Format != VK_FORMAT_R16G16B16A16_SFLOAT ||
        output.Width != mImpl->Active->Inputs.Width || output.Height != mImpl->Active->Inputs.Height) {
        mImpl->Reason = "Brixelizer GI output is not declared";
        return false;
    }
    auto& instance = *mImpl->Active;
    auto* command = NriInteropAccess::CommandBuffer(*mImpl->Interop, frameSlot);
    if (command == nullptr) return false;
    mImpl->LastBarriers = barriers.BeginOutputExecution(EffectResource::DiffuseIllumination);
    const auto privateTransition = [&](VkImage image, ResourceAccess access) {
        const auto change = instance.States.PlanTransition(reinterpret_cast<uintptr_t>(image), {access, 0});
        if (!NriInteropAccess::CmdTextureBarrier(*mImpl->Interop, frameSlot, image, change))
            throw std::runtime_error("Brixelizer GI transition failed");
        instance.States.Commit(change);
        mImpl->LastBarriers.RecordPrivateTransition(change);
    };
    const auto graphTransition = [&](ResourceAccess access) {
        const auto change =
            output.StateTracker->PlanTransition(reinterpret_cast<uintptr_t>(output.Texture.Image), {access, 0});
        if (!NriInteropAccess::CmdTextureBarrier(*mImpl->Interop, frameSlot, output.Texture.Image, change))
            throw std::runtime_error("Brixelizer GI output transition failed");
        output.StateTracker->Commit(change);
        mImpl->LastBarriers.RecordGraphTransition(change);
    };
    if (instance.LastTracedFrame != frameId || !instance.Traced) {
        if (!Trace(instance, commandBuffer, frameSlot, frameId, temporal, view, coordinates, privateTransition))
            return false;
        instance.LastTracedFrame = frameId;
        instance.Traced = true;
    }
    try {
        privateTransition(instance.Output.Image, ResourceAccess::TransferRead);
        graphTransition(ResourceAccess::TransferWrite);
        auto* source = NriInteropAccess::Texture(*mImpl->Interop, instance.Output.Image);
        auto* destination = NriInteropAccess::Texture(*mImpl->Interop, output.Texture.Image);
        if (source == nullptr || destination == nullptr) throw std::runtime_error("Brixelizer GI copy textures unavailable");
        mImpl->Core->CmdCopyTexture(*command, *destination, nullptr, *source, nullptr);
        graphTransition(outputBarrier->CompletionAccess);
    } catch (const std::exception& exception) {
        mImpl->Reason = exception.what();
        return false;
    }
    mImpl->Reason.clear();
    return true;
#endif
}

#if OOT3D_HAS_BRIXELIZER_GI_PASS
template <typename Transition>
bool BrixelizerGiPass::Trace(BrixelizerGiInstance& instance, VkCommandBuffer commandBuffer, uint32_t frameSlot,
                             uint64_t frameId, const TemporalViewState& temporal, const PerspectiveViewState& view,
                             PicaSurfaceCoordinates coordinates, const Transition& privateTransition) {
    auto* command = NriInteropAccess::CommandBuffer(*mImpl->Interop, frameSlot);
    // view = projection^-1 * worldToClip; the storage orientation rotates
    // clip XY only (as for SSSR), so the view basis stays unrotated.
    std::array<float, 16> inverseProjection{}, inverseView{};
    if (!InvertTemporalMatrix(view.Projection, inverseProjection)) {
        mImpl->Reason = "Brixelizer GI projection is not invertible";
        return false;
    }
    const auto viewMatrix = MultiplyColumnMajor(inverseProjection, temporal.CurrentWorldToClip);
    const auto previousView = MultiplyColumnMajor(inverseProjection, temporal.PreviousWorldToClip);
    if (!InvertTemporalMatrix(viewMatrix, inverseView)) {
        mImpl->Reason = "Brixelizer GI view is not invertible";
        return false;
    }
    FfxBrixelizerGIDispatchDescription dispatch{};
    if (!BrixelizerSceneGiAccess::Bind(*mImpl->Scene, commandBuffer, frameId, dispatch)) {
        mImpl->Reason = "the scene distance field has no data yet";
        return false;
    }
    const uint32_t current = instance.Current;
    // Without a prepared history the current sample stands in for it.
    const uint32_t history = instance.HistoryPrepared ? current ^ 1U : current;
    try {
        for (auto* texture : {&instance.Lit[current], &instance.Depth[current], &instance.Normal[current]})
            privateTransition(texture->Image, ResourceAccess::ComputeWrite);
        mImpl->Core->CmdSetDescriptorPool(*command, *instance.Pool);
        mImpl->Core->CmdSetPipelineLayout(*command, nri::BindPoint::COMPUTE, *mImpl->Layout);
        mImpl->Core->CmdSetPipeline(*command, *mImpl->Pipeline);
        mImpl->Core->CmdSetDescriptorSet(*command, {0, instance.Sets[current]});
        mImpl->Core->CmdSetRootConstants(*command, {0, inverseView.data(), sizeof(inverseView)});
        mImpl->Core->CmdDispatch(*command,
                                 {(instance.Inputs.Width + 7) / 8, (instance.Inputs.Height + 7) / 8, 1});
        for (auto* texture : {&instance.Lit[current], &instance.Depth[current], &instance.Normal[current],
                              &instance.Lit[history], &instance.Depth[history], &instance.Normal[history]})
            privateTransition(texture->Image, ResourceAccess::ShaderRead);
        privateTransition(instance.Specular.Image, ResourceAccess::ComputeWrite);
        privateTransition(instance.Output.Image, ResourceAccess::ComputeWrite);
    } catch (const std::exception& exception) {
        mImpl->Reason = exception.what();
        return false;
    }

    const auto& in = instance.Inputs;
    const auto resource = [](VkImage image, const VkImageCreateInfo& info, const wchar_t* name,
                             FfxResourceStates state = FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ) {
        return ffxGetResourceVK(reinterpret_cast<void*>(image), ffxGetImageResourceDescriptionVK(image, info), name,
                                state);
    };
    const auto owned = [&](const NriOwnedTexture2D& texture, VkFormat format, const wchar_t* name,
                           FfxResourceStates state = FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ) {
        return resource(texture.Image,
                        ImageInfo(in.Width, in.Height, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT),
                        name, state);
    };
    dispatch.depth = owned(instance.Depth[current], VK_FORMAT_R32_SFLOAT, L"OOT3D_GI_Depth");
    dispatch.historyDepth = owned(instance.Depth[history], VK_FORMAT_R32_SFLOAT, L"OOT3D_GI_HistoryDepth");
    dispatch.normal = owned(instance.Normal[current], VK_FORMAT_R16G16B16A16_SFLOAT, L"OOT3D_GI_Normal");
    dispatch.historyNormal =
        owned(instance.Normal[history], VK_FORMAT_R16G16B16A16_SFLOAT, L"OOT3D_GI_HistoryNormal");
    dispatch.prevLitOutput = owned(instance.Lit[history], VK_FORMAT_R16G16B16A16_SFLOAT, L"OOT3D_GI_PrevLit");
    dispatch.roughness = resource(in.MaterialGuide,
                                  ImageInfo(in.Width, in.Height, in.MaterialGuideFormat, VK_IMAGE_USAGE_SAMPLED_BIT),
                                  L"OOT3D_GI_Material");
    dispatch.motionVectors = resource(in.Motion,
                                      ImageInfo(in.Width, in.Height, in.MotionFormat,
                                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT),
                                      L"OOT3D_GI_Motion");
    dispatch.outputDiffuseGI = owned(instance.Output, VK_FORMAT_R16G16B16A16_SFLOAT, L"OOT3D_GI_Diffuse",
                                     FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.outputSpecularGI = owned(instance.Specular, VK_FORMAT_R16G16B16A16_SFLOAT, L"OOT3D_GI_Specular",
                                      FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    const auto projection = coordinates.FidelityFxClip(view.Projection);
    std::memcpy(dispatch.view, viewMatrix.data(), sizeof(dispatch.view));
    std::memcpy(dispatch.projection, projection.data(), sizeof(dispatch.projection));
    std::memcpy(dispatch.prevView, previousView.data(), sizeof(dispatch.prevView));
    std::memcpy(dispatch.prevProjection, projection.data(), sizeof(dispatch.prevProjection));
    dispatch.cameraPosition[0] = inverseView[12];
    dispatch.cameraPosition[1] = inverseView[13];
    dispatch.cameraPosition[2] = inverseView[14];
    dispatch.startCascade = BrixelizerScene::kMergedCascadeBase;
    dispatch.endCascade = BrixelizerScene::kMergedCascadeBase + BrixelizerScene::kCascadeCount - 1;
    // Native world units: offsets follow the finest voxel.
    dispatch.rayPushoff = BrixelizerScene::kBaseVoxelSize * 0.5F;
    dispatch.sdfSolveEps = BrixelizerScene::kBaseVoxelSize * 0.125F;
    dispatch.specularRayPushoff = dispatch.rayPushoff;
    dispatch.specularSDFSolveEps = dispatch.sdfSolveEps;
    dispatch.tMin = 0.0F;
    dispatch.tMax = 1.0e5F;
    dispatch.normalsUnpackMul = 2.0F;
    dispatch.normalsUnpackAdd = -1.0F;
    dispatch.isRoughnessPerceptual = true;
    dispatch.roughnessChannel = 1U;
    dispatch.roughnessThreshold = 0.9F;
    dispatch.environmentMapIntensity = 0.0F;
    dispatch.motionVectorScale = {1.0F, 1.0F};
    if (ffxBrixelizerGIContextDispatch(&instance.Context, &dispatch, ffxGetCommandListVK(commandBuffer)) != FFX_OK) {
        mImpl->Reason = "Brixelizer GI dispatch failed";
        return false;
    }
    instance.HistoryPrepared = true;
    instance.Current ^= 1U;
    return true;
}
#endif

bool BrixelizerGiPass::ClearOutput(uint32_t frameSlot, const NriEffectGraphTransientImageBinding& output,
                                   const EffectPassBarrierPlan& barriers) {
    mImpl->LastBarriers = {};
#if !OOT3D_HAS_BRIXELIZER_GI_PASS
    (void)frameSlot, (void)output, (void)barriers;
    return false;
#else
    const auto* outputBarrier = barriers.Find(EffectResource::DiffuseIllumination);
    if (!mImpl->Initialized || !barriers.Valid() || outputBarrier == nullptr || !outputBarrier->Managed() ||
        !output.Valid() || output.Resource != EffectResource::DiffuseIllumination)
        return false;
    mImpl->LastBarriers = barriers.BeginOutputExecution(EffectResource::DiffuseIllumination);
    const auto transition = [&](ResourceAccess access) {
        const auto change =
            output.StateTracker->PlanTransition(reinterpret_cast<uintptr_t>(output.Texture.Image), {access, 0});
        if (!NriInteropAccess::CmdTextureBarrier(*mImpl->Interop, frameSlot, output.Texture.Image, change))
            return false;
        output.StateTracker->Commit(change);
        mImpl->LastBarriers.RecordGraphTransition(change);
        return true;
    };
    return transition(ResourceAccess::StorageClear) &&
           mImpl->Clear.ClearUint32(frameSlot, output.Texture.Image, 0U) &&
           transition(outputBarrier->CompletionAccess);
#endif
}

void BrixelizerGiPass::InvalidateScreenResources() {
#if OOT3D_HAS_BRIXELIZER_GI_PASS
    for (auto& instance : mImpl->Instances) mImpl->Destroy(*instance);
#endif
    mImpl->Instances.clear();
    mImpl->Active = nullptr;
    mImpl->LastBarriers = {};
}

void BrixelizerGiPass::Shutdown() {
    InvalidateScreenResources();
    mImpl->Clear.Shutdown();
#if OOT3D_HAS_BRIXELIZER_GI_PASS
    if (mImpl->Core != nullptr) {
        if (mImpl->Pipeline != nullptr) mImpl->Core->DestroyPipeline(mImpl->Pipeline);
        if (mImpl->Layout != nullptr) mImpl->Core->DestroyPipelineLayout(mImpl->Layout);
    }
#endif
    mImpl = std::make_unique<Impl>();
}

bool BrixelizerGiPass::Available() const { return mImpl->Initialized; }
const EffectPassBarrierExecution& BrixelizerGiPass::LastBarrierExecution() const { return mImpl->LastBarriers; }
const std::string& BrixelizerGiPass::UnavailableReason() const { return mImpl->Reason; }

} // namespace Fast::Oot3d

#endif
