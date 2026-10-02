#ifdef ENABLE_OOT3D_VULKAN
#include <bit>
#include "fast/oot3d/pica_guide_diagnostics.h"
#include "fast/oot3d/outline_diagnostics.h"
#include "fast/oot3d/outline_occlusion_pass.h"

#include "fast/backends/gfx_vulkan.h"
#include "oot3d/renderer/ui_presentation_layout.h"
#include "fast/oot3d/pica_shadow2d.h"
#include "fast/oot3d/pica_display_composition.h"
#include "fast/oot3d/display_effect_plan.h"
#include "fast/oot3d/display_effect_resources.h"
#include "fast/oot3d/effect_graph_physical_plan.h"
#include "fast/oot3d/graphics_settings_runtime.h"
#include "fast/oot3d/scene_view_runtime.h"
#include "fast/oot3d/texture_catalog_runtime.h"
#include "fast/oot3d/grass_interaction_bridge.h"
#include "fast/oot3d/grass_primitive_topology.h"
#include "fast/oot3d/grass_render_telemetry.h"
#include "fast/oot3d/grass_surface_eligibility.h"
#include "fast/oot3d/grass_texture_source_cache.h"
#include "fast/oot3d/grass_scene_bridge.h"
#include "fast/oot3d/visual_clock.h"
#include "fast/oot3d/render_resolution_policy.h"
#include "fast/oot3d/pica_toon_shader.h"
#include "fast/oot3d/pica_toon_telemetry.h"
#include "fast/oot3d/pica_scanout_effects.h"
#include "fast/oot3d/pica_ambient_occlusion_guide.h"
#include "fast/oot3d/pica_scene_domain_guide.h"
#include "fast/oot3d/pica_grass_texture_coordinates.h"
#include "fast/oot3d/pica_guide_sampling_barriers.h"
#include "fast/oot3d/pica_reactive_mask.h"
#include "fast/oot3d/pica_nri_shader_contract.h"
#include "fast/oot3d/pica_uber_effect_program.h"
#include "fast/oot3d/pica_nri_vertex_input.h"
#include "fast/oot3d/pica_nri_pipeline_state.h"
#include "fast/renderer3ds/pica_pipeline_identity.h"
#include "fast/renderer3ds/pica_surface_lighting.h"
#include "fast/renderer3ds/pica_tev_program.h"
#include "fast/oot3d/pica_uniform_layout.h"
#include "fast/oot3d/pica_nri_draw_ownership.h"
#include "fast/oot3d/pica_directional_shadow_lighting.h"
#include "fast/oot3d/pica_scene_semantics.h"
#include "fast/oot3d/pica_alpha_coverage_policy.h"
#include "fast/oot3d/linear_scene_color.h"
#include "fast/oot3d/upscaler_guides.h"
#include "fast/oot3d/pica_reflection_material.h"
#include "fast/oot3d/reflection_provider.h"
#include "fast/oot3d/pica_texture_decode.h"
#include "oot3d/renderer/azahar_texture_pack.h"
#include <spdlog/spdlog.h>
#include "fast/backends/gfx_native_pica_layout.h"
#include "fast/renderer3ds/pica_ui_canvas.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include "fast/renderer3ds/slow_step_trace.h"

namespace Fast {
namespace {

constexpr uint64_t kCustomTextureUploadBudgetBytes =
    32ULL * 1024ULL * 1024ULL;

void CheckNativeVk(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) +
                                 " failed with VkResult " +
                                 std::to_string(static_cast<int>(result)));
    }
}

void SetNativeError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

template <typename Handle>
uintptr_t ToPicaSceneNativeHandle(Handle handle) noexcept {
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<uintptr_t>(handle);
    } else {
        return static_cast<uintptr_t>(handle);
    }
}

uint64_t HashNativeBytes(std::span<const uint8_t> bytes) {
    uint64_t hash = 1469598103934665603ULL;
    for (const uint8_t value : bytes) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return hash;
}

using Oot3d::ToNativeVkCompare;

using Oot3d::ToNativeVkStencil;

using Oot3d::ToNativeVkLogic;

using Oot3d::ToNativeVkTopology;

Oot3d::PicaSceneTopology ToPicaSceneTopology(
    GfxNativePicaTopology topology) {
    switch (topology) {
        case GfxNativePicaTopology::TriangleList:
        case GfxNativePicaTopology::GeometryShader:
            return Oot3d::PicaSceneTopology::TriangleList;
        case GfxNativePicaTopology::TriangleStrip:
            return Oot3d::PicaSceneTopology::TriangleStrip;
        case GfxNativePicaTopology::TriangleFan:
            throw std::runtime_error(
                "native PICA triangle fan requires index expansion");
    }
    throw std::runtime_error("invalid native PICA topology");
}

Oot3d::PicaSceneCullMode ToPicaSceneCullMode(const GfxNativePicaDrawView& draw) noexcept {
    return draw.CullMode == GfxNativeCullMode::KeepAll ? Oot3d::PicaSceneCullMode::None
        : draw.FramebufferFlipped                      ? Oot3d::PicaSceneCullMode::Front
                                                       : Oot3d::PicaSceneCullMode::Back;
}

Oot3d::PicaSceneFrontFace ToPicaSceneFrontFace(const GfxNativePicaDrawView& draw) noexcept {
    return draw.CullMode == GfxNativeCullMode::KeepCounterClockwise ? Oot3d::PicaSceneFrontFace::Clockwise
                                                                     : Oot3d::PicaSceneFrontFace::CounterClockwise;
}

// Opaque-geometry admission from the draw's own state, shared by shadow
// casters and the scene distance field: depth-writing, colour-writing and
// without an alpha cutout, which a depth-only caster would draw solid.
bool AdmitsOpaqueSceneGeometry(const GfxNativePicaDrawView& draw) noexcept {
    return draw.FragmentOperationMode == 0U && draw.DepthTestEnabled && draw.DepthWriteEnabled &&
           !draw.AlphaTestEnabled && (draw.ColorWriteMask & 0x7U) != 0U;
}

// An alpha test that can discard fragments; "Always" passes every fragment.
bool RunsDiscardingAlphaTest(const GfxNativePicaDrawView& draw) noexcept {
    return draw.AlphaTestEnabled && draw.AlphaCompare != GfxNativePicaCompareFunction::Always;
}

// Shadow-caster admission: depth-writing, colour-writing geometry. An
// alpha-tested caster runs its native alpha test in the map, which needs the
// published fragment hooks, the fragment uniforms and directly sampled native
// textures (no packed shadow image or GPU-produced copy). A test that never
// passes casts nothing.
bool AdmitsShadowCasterGeometry(const GfxNativePicaDrawView& draw) noexcept {
    if (draw.FragmentOperationMode != 0U || !draw.DepthTestEnabled || !draw.DepthWriteEnabled ||
        (draw.ColorWriteMask & 0x7U) == 0U)
        return false;
    if (!RunsDiscardingAlphaTest(draw))
        return true;
    return draw.AlphaCompare != GfxNativePicaCompareFunction::Never &&
           draw.FragmentShaderHooks.ValidFor(draw.FragmentShaderSource) &&
           draw.FragmentShaderHooks.Supports(Renderer3ds::PicaShaderHook::AfterNativeTev) &&
           draw.FragmentShaderHooks.Supports(Renderer3ds::PicaShaderHook::BeforeDepth) &&
           draw.FragmentUniformBytes.size() == Oot3d::kPicaPackedFragmentUniformSize &&
           std::all_of(draw.Textures.begin(), draw.Textures.end(), [](const auto& texture) {
               return texture.Slot < 3U && texture.CopyReferences.empty() &&
                      (texture.NativeType == 0U || (texture.Slot == 0U && texture.NativeType == 3U));
           });
}

// Native state that shapes an alpha-tested caster: TEV stages, constants and
// combiner buffer, the alpha test, texture LOD bias, the lighting and
// procedural-texture programs and the sampled contents. Light and fog values
// leave the cutout unchanged, so they do not invalidate cached pages.
uint64_t HashAlphaCasterMaterial(const GfxNativePicaDrawView& draw,
                                 const std::array<uint64_t, 3>& replacementHashes) {
    const auto bytes = draw.FragmentUniformBytes;
    uint64_t hash = HashNativeBytes(bytes.first(Oot3d::kPicaPackedFragmentDepthScaleOffset));
    const auto mix = [&hash](uint64_t value) {
        hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
    };
    mix(HashNativeBytes(bytes.subspan(Oot3d::kPicaPackedFragmentTextureLodBiasOffset, 16U)));
    mix(HashNativeBytes(bytes.subspan(Oot3d::kPicaPackedFragmentTevProgramOffset,
                                      Oot3d::kPicaPackedFragmentProcTexOffset -
                                          Oot3d::kPicaPackedFragmentTevProgramOffset)));
    uint32_t textureConfig = 0;
    std::memcpy(&textureConfig, bytes.data() + Oot3d::kPicaPackedFragmentControlOffset + 8U, sizeof(textureConfig));
    if ((textureConfig & (1U << 10U)) != 0U)
        mix(HashNativeBytes(bytes.subspan(Oot3d::kPicaPackedFragmentProcTexOffset)));
    for (const auto& texture : draw.Textures) {
        mix(texture.Slot);
        mix(texture.NativeContentHash);
        mix(replacementHashes[texture.Slot]);
        mix(uint64_t{texture.NativeWrapS} | uint64_t{texture.NativeWrapT} << 8U |
            uint64_t{texture.MinLinear} << 16U | uint64_t{texture.MagLinear} << 17U |
            uint64_t{texture.MipLinear} << 18U);
    }
    return hash;
}

VkFormat ToNativeVkVertexFormat(GfxNativePicaVertexFormat format,
                                uint8_t components) {
    if (components == 0 || components > 4) {
        throw std::runtime_error("invalid native PICA vertex component count");
    }
    static constexpr std::array<VkFormat, 4> kSignedByte{
        VK_FORMAT_R8_SSCALED, VK_FORMAT_R8G8_SSCALED,
        VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R8G8B8A8_SSCALED};
    static constexpr std::array<VkFormat, 4> kUnsignedByte{
        VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED,
        VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED};
    static constexpr std::array<VkFormat, 4> kSignedShort{
        VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED,
        VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED};
    static constexpr std::array<VkFormat, 4> kFloat{
        VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
        VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
    switch (format) {
        case GfxNativePicaVertexFormat::SignedByte:
            return kSignedByte[components - 1U];
        case GfxNativePicaVertexFormat::UnsignedByte:
            return kUnsignedByte[components - 1U];
        case GfxNativePicaVertexFormat::SignedShort:
            return kSignedShort[components - 1U];
        case GfxNativePicaVertexFormat::Float:
            return kFloat[components - 1U];
    }
    throw std::runtime_error("invalid native PICA vertex format");
}

GfxNativeTextureWrap DecodeNativePicaWrap(uint8_t wrap) {
    switch (wrap) {
        case 0:
            return GfxNativeTextureWrap::ClampToEdge;
        case 2:
        case 6:
        case 7:
            return GfxNativeTextureWrap::Repeat;
        case 3:
            return GfxNativeTextureWrap::MirroredRepeat;
        case 1:
            throw std::runtime_error(
                "native PICA clamp-to-border requires border color state");
        case 4:
        case 5:
            throw std::runtime_error(
                "native PICA asymmetric wrap mode is unsupported");
        default:
            throw std::runtime_error("invalid native PICA texture wrap mode");
    }
}

std::optional<Oot3d::GrassTextureWrap>
DecodeNativePicaGrassWrap(uint8_t wrap) {
    switch (wrap) {
        case 0:
            return Oot3d::GrassTextureWrap::Clamp;
        case 2:
        case 6:
        case 7:
            return Oot3d::GrassTextureWrap::Repeat;
        case 3:
            return Oot3d::GrassTextureWrap::Mirror;
        default:
            return std::nullopt;
    }
}

using Oot3d::UsesTranslucentBlend;

VkFilter ToNativePicaVkFilter(GfxNativeTextureFilter filter) {
    switch (filter) {
        case GfxNativeTextureFilter::Linear:
        case GfxNativeTextureFilter::LinearMipmapNearest:
        case GfxNativeTextureFilter::LinearMipmapLinear:
            return VK_FILTER_LINEAR;
        default:
            return VK_FILTER_NEAREST;
    }
}

VkSamplerMipmapMode ToNativePicaVkMipmapMode(
    GfxNativeTextureFilter filter) {
    return filter == GfxNativeTextureFilter::NearestMipmapLinear ||
                   filter == GfxNativeTextureFilter::LinearMipmapLinear
        ? VK_SAMPLER_MIPMAP_MODE_LINEAR
        : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

VkSamplerAddressMode ToNativePicaVkAddressMode(
    GfxNativeTextureWrap wrap) {
    switch (wrap) {
        case GfxNativeTextureWrap::ClampToEdge:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case GfxNativeTextureWrap::MirroredRepeat:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case GfxNativeTextureWrap::Repeat:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

using Oot3d::ToNativeVkBlendOperation;

using Oot3d::ToNativeVkBlendFactor;

VkImageAspectFlags NativeDepthAspect(VkFormat format) {
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
        format == VK_FORMAT_D24_UNORM_S8_UINT) {
        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return aspect;
}

uint32_t NativePicaColorBytesPerPixel(uint8_t format) {
    return format == 0U ? 4U : (format == 1U ? 3U : 2U);
}

uint32_t NativePicaDepthBytesPerPixel(uint8_t format) {
    return format == 0U ? 2U : (format == 2U ? 3U : 4U);
}

uint32_t NativePicaFillElementSize(uint16_t control) {
    return (control & (1U << 9U)) != 0U
               ? 4U
               : ((control & (1U << 8U)) != 0U ? 3U : 2U);
}

uint8_t NativePicaFillByte(const GfxNativePicaMemoryFillView& fill,
                           uint32_t index) {
    const uint32_t shift = (index % NativePicaFillElementSize(fill.Control)) * 8U;
    return static_cast<uint8_t>(fill.Value >> shift);
}

float NativePicaUnorm(uint32_t value, uint32_t maximum) {
    return static_cast<float>(value) / static_cast<float>(maximum);
}

VkClearColorValue DecodeNativePicaClearColor(
    const GfxNativePicaMemoryFillView& fill, uint8_t format) {
    const uint8_t b0 = NativePicaFillByte(fill, 0U);
    const uint8_t b1 = NativePicaFillByte(fill, 1U);
    const uint8_t b2 = NativePicaFillByte(fill, 2U);
    const uint8_t b3 = NativePicaFillByte(fill, 3U);
    std::array<float, 4> color{};
    if (format == 0U) {
        color = {NativePicaUnorm(b3, 0xFFU), NativePicaUnorm(b2, 0xFFU),
                 NativePicaUnorm(b1, 0xFFU), NativePicaUnorm(b0, 0xFFU)};
    } else if (format == 1U) {
        color = {NativePicaUnorm(b2, 0xFFU), NativePicaUnorm(b1, 0xFFU),
                 NativePicaUnorm(b0, 0xFFU), 1.0F};
    } else {
        const uint16_t packed = static_cast<uint16_t>(b0) |
                                static_cast<uint16_t>(b1) << 8U;
        if (format == 2U) {
            color = {NativePicaUnorm((packed >> 11U) & 0x1FU, 0x1FU),
                     NativePicaUnorm((packed >> 5U) & 0x3FU, 0x3FU),
                     NativePicaUnorm(packed & 0x1FU, 0x1FU), 1.0F};
        } else if (format == 3U) {
            color = {NativePicaUnorm((packed >> 11U) & 0x1FU, 0x1FU),
                     NativePicaUnorm((packed >> 6U) & 0x1FU, 0x1FU),
                     NativePicaUnorm((packed >> 1U) & 0x1FU, 0x1FU),
                     static_cast<float>(packed & 1U)};
        } else {
            color = {NativePicaUnorm((packed >> 12U) & 0xFU, 0xFU),
                     NativePicaUnorm((packed >> 8U) & 0xFU, 0xFU),
                     NativePicaUnorm((packed >> 4U) & 0xFU, 0xFU),
                     NativePicaUnorm(packed & 0xFU, 0xFU)};
        }
    }
    VkClearColorValue result{};
    std::copy(color.begin(), color.end(), result.float32);
    return result;
}

VkClearDepthStencilValue DecodeNativePicaClearDepthStencil(
    const GfxNativePicaMemoryFillView& fill, uint8_t format) {
    const uint32_t b0 = NativePicaFillByte(fill, 0U);
    const uint32_t b1 = NativePicaFillByte(fill, 1U);
    const uint32_t b2 = NativePicaFillByte(fill, 2U);
    const uint32_t depth = format == 0U ? b0 | (b1 << 8U)
                                        : b0 | (b1 << 8U) | (b2 << 16U);
    return {NativePicaUnorm(depth, format == 0U ? 0xFFFFU : 0xFFFFFFU),
            format == 3U ? NativePicaFillByte(fill, 3U) : 0U};
}

bool BuildNativePicaMemoryFillSmoke(
    uint64_t renderTargetNamespace, uint32_t colorAddress,
    uint32_t depthAddress, uint64_t colorEnd, uint64_t depthEnd,
    GfxNativePicaMemoryFillView& fill) {
    static const bool smokeEnabled = [] {
        const char* smoke = std::getenv("OOT3D_GRAPHICS_TEST_PICA_MEMORY_FILL");
        return smoke != nullptr && std::string_view(smoke) == "1";
    }();
    if (!smokeEnabled)
        return false;
    const uint64_t start =
        std::min<uint64_t>(colorAddress, depthAddress);
    const uint64_t end = std::max(colorEnd, depthEnd);
    if (start == 0U || end > std::numeric_limits<uint32_t>::max())
        return false;
    fill = {
        renderTargetNamespace,
        static_cast<uint32_t>(start),
        static_cast<uint32_t>(end),
        0xFFFFFFFFU,
        static_cast<uint16_t>(1U | (1U << 9U)),
    };
    return true;
}

} // namespace

void GfxRenderingAPIVulkan::CreateNativePicaShaderResources() {
    CreateNativePicaRenderPass();
    VkPhysicalDeviceFeatures seamFeatures{};
    vkGetPhysicalDeviceFeatures(mPhysicalDevice, &seamFeatures);
    mPbrSeamsAvailable = seamFeatures.geometryShader == VK_TRUE;
    std::array<VkDescriptorSetLayoutBinding, 14 + Renderer3ds::kPicaPbrTextureCount> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    for (uint32_t binding = 1; binding <= 3; ++binding) {
        bindings[binding] = {binding,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    }
    bindings[4] = {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[6] = {6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    bindings[7] = {10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[8] = {12, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[9] = {13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                  VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[10] = {15, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[11] = {16, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[12] = {17, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (uint32_t index = 13U; index < 13U + Renderer3ds::kPicaPbrTextureCount; ++index)
        bindings[index] = {index + 5U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings.back() = {26, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo descriptorInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptorInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    descriptorInfo.pBindings = bindings.data();
    CheckNativeVk(vkCreateDescriptorSetLayout(
                      mDevice, &descriptorInfo, nullptr,
                      &mNativePicaDescriptorSetLayout),
                  "vkCreateDescriptorSetLayout(native PICA)");

    VkPipelineLayoutCreateInfo pipelineInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineInfo.setLayoutCount = 1;
    pipelineInfo.pSetLayouts = &mNativePicaDescriptorSetLayout;
    VkPushConstantRange nativeDrawPush{};
    nativeDrawPush.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    nativeDrawPush.size = 8U * sizeof(float);
    pipelineInfo.pushConstantRangeCount = 1;
    pipelineInfo.pPushConstantRanges = &nativeDrawPush;
    CheckNativeVk(vkCreatePipelineLayout(mDevice, &pipelineInfo, nullptr,
                                         &mNativePicaPipelineLayout),
                  "vkCreatePipelineLayout(native PICA)");

    std::array<VkDescriptorSetLayoutBinding, 11> scanoutBindings{};
    for (uint32_t binding = 0; binding < scanoutBindings.size(); ++binding) {
        scanoutBindings[binding].binding = binding;
        scanoutBindings[binding].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        scanoutBindings[binding].descriptorCount = 1;
        scanoutBindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    descriptorInfo.bindingCount = static_cast<uint32_t>(scanoutBindings.size());
    descriptorInfo.pBindings = scanoutBindings.data();
    CheckNativeVk(vkCreateDescriptorSetLayout(
                      mDevice, &descriptorInfo, nullptr,
                      &mNativePicaScanoutDescriptorSetLayout),
                  "vkCreateDescriptorSetLayout(native PICA scanout)");

    VkPushConstantRange scanoutPushConstant{};
    scanoutPushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    scanoutPushConstant.size = sizeof(Oot3d::PicaScanoutPushConstants);
    pipelineInfo.pSetLayouts = &mNativePicaScanoutDescriptorSetLayout;
    pipelineInfo.pushConstantRangeCount = 1;
    pipelineInfo.pPushConstantRanges = &scanoutPushConstant;
    CheckNativeVk(vkCreatePipelineLayout(
                      mDevice, &pipelineInfo, nullptr,
                      &mNativePicaScanoutPipelineLayout),
                  "vkCreatePipelineLayout(native PICA scanout)");

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    CheckNativeVk(vkCreateSampler(mDevice, &samplerInfo, nullptr,
                                  &mNativePicaScanoutSampler),
                  "vkCreateSampler(native PICA scanout)");
}

void GfxRenderingAPIVulkan::DestroyNativePicaShaderResources() {
    EndNativePicaRenderPass();
    for (auto& [key, texture] : mNativePicaTextures) {
        DestroyTexture(texture);
    }
    mNativePicaTextures.clear();
    for (auto& [hash, material] : mNativePicaMaterials) {
        for (auto& image : material.Active.Images) DestroyTexture(image);
        for (auto& image : material.Pending.Images) DestroyTexture(image);
    }
    mNativePicaMaterials.clear();
    for (auto& frame : mFrameResources) {
        for (auto& image : frame.RetiredTextures) DestroyTexture(image);
        frame.RetiredTextures.clear();
    }
    for (auto& [key, lightingLut] : mNativePicaLightingLuts) {
        DestroyTexture(lightingLut.Texture);
    }
    mNativePicaLightingLuts.clear();
    for (auto& [key, target] : mNativePicaRenderTargets) {
        DestroyNativePicaRenderTarget(target);
    }
    mNativePicaRenderTargets.clear();
    mNativePicaDisplayDepthTargets.clear();
    for (auto& [address, image] : mNativePicaDisplayImages) {
        DestroyNativePicaDisplayImage(image);
    }
    mNativePicaDisplayImages.clear();
    DestroyNativePicaSceneBeforeUiImages();
    const auto destroyShaderCache = [this](auto& cache) {
        for (auto& [key, shader] : cache) {
            (void)key;
            if (shader.VertexShader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(
                    mDevice, shader.VertexShader, nullptr);
            }
            if (shader.FragmentShader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(
                    mDevice, shader.FragmentShader, nullptr);
            }
        }
        cache.clear();
    };
    destroyShaderCache(mCanonicalNativePicaShaders);
    destroyShaderCache(mInstrumentedNativePicaShaders);
    destroyShaderCache(mUberNativePicaShaders);
    if (mNativePicaScanoutSampler != VK_NULL_HANDLE) {
        vkDestroySampler(mDevice, mNativePicaScanoutSampler, nullptr);
        mNativePicaScanoutSampler = VK_NULL_HANDLE;
    }
    if (mNativePicaScanoutPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(mDevice, mNativePicaScanoutPipelineLayout,
                                nullptr);
        mNativePicaScanoutPipelineLayout = VK_NULL_HANDLE;
    }
    if (mNativePicaScanoutDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(
            mDevice, mNativePicaScanoutDescriptorSetLayout, nullptr);
        mNativePicaScanoutDescriptorSetLayout = VK_NULL_HANDLE;
    }
    if (mNativePicaPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(mDevice, mNativePicaPipelineLayout, nullptr);
        mNativePicaPipelineLayout = VK_NULL_HANDLE;
    }
    if (mNativePicaDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(mDevice, mNativePicaDescriptorSetLayout,
                                     nullptr);
        mNativePicaDescriptorSetLayout = VK_NULL_HANDLE;
    }
    if (mNativePicaRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(mDevice, mNativePicaRenderPass, nullptr);
        mNativePicaRenderPass = VK_NULL_HANDLE;
    }
    if (mNativePicaCanonicalRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(mDevice, mNativePicaCanonicalRenderPass, nullptr);
        mNativePicaCanonicalRenderPass = VK_NULL_HANDLE;
    }
}

void GfxRenderingAPIVulkan::RetireNativePicaGeometry(uint64_t identity) {
    const auto found = mNativePicaGeometryBuffers.find(identity);
    if (found == mNativePicaGeometryBuffers.end()) {
        return;
    }
    for (uint32_t frameIndex = 0; frameIndex < kFramesInFlight; ++frameIndex) {
        auto& buffer = found->second.Buffers[frameIndex];
        if (buffer.Buffer == VK_NULL_HANDLE) {
            continue;
        }
        mRetiredNativePicaGeometryBuffers[frameIndex].push_back(buffer);
        buffer = {};
    }
    mNativePicaGeometryBuffers.erase(found);
}

void GfxRenderingAPIVulkan::ReleaseRetiredNativePicaGeometryBuffers(uint32_t frameIndex) {
    auto& retired = mRetiredNativePicaGeometryBuffers[frameIndex % kFramesInFlight];
    for (auto& buffer : retired) {
        if (buffer.Buffer != VK_NULL_HANDLE) {
            mNriInterop.ForgetBuffer(buffer.Buffer);
        }
        DestroyBuffer(buffer);
    }
    retired.clear();
}

void GfxRenderingAPIVulkan::DestroyNativePicaGeometryResources() {
    for (auto& [_, geometry] : mNativePicaGeometryBuffers) {
        for (auto& buffer : geometry.Buffers) {
            if (buffer.Buffer != VK_NULL_HANDLE) {
                mNriInterop.ForgetBuffer(buffer.Buffer);
            }
            DestroyBuffer(buffer);
        }
    }
    mNativePicaGeometryBuffers.clear();
    for (auto& retired : mRetiredNativePicaGeometryBuffers) {
        for (auto& buffer : retired) {
            if (buffer.Buffer != VK_NULL_HANDLE) {
                mNriInterop.ForgetBuffer(buffer.Buffer);
            }
            DestroyBuffer(buffer);
        }
        retired.clear();
    }
    mPicaGeometryRegistry.Clear();
    mPicaSceneFrame.Reset();
    mNativeSceneView.Reset();
    mPreparedShadowCasterFrame.Reset();
    mPreparedShadowCasterView.Reset();
    mPreparedShadowCasters.clear();
    mPicaScenePublications.Reset();
    mGrassGeometryRegistry.Clear();
    Oot3d::GrassSceneBridge::Instance().Clear();
}

void GfxRenderingAPIVulkan::CreateNativePicaScanoutPipeline() {
    if (mNativePicaScanoutPipeline != VK_NULL_HANDLE &&
        mNativePicaScanoutOverlayPipeline != VK_NULL_HANDLE) {
        return;
    }
    if (mRenderPass == VK_NULL_HANDLE || mOverlayRenderPass == VK_NULL_HANDLE) {
        throw std::runtime_error(
            "native PICA scanout requires swapchain base and overlay render passes");
    }

    const std::string vertexSource =
        Oot3d::BuildPicaScanoutVertexShader();
    const std::string fragmentSource =
        Oot3d::BuildPicaScanoutFragmentShader();

    VkShaderModule vertexShader = CreateShaderModuleFromSpirv(ResolveNativePicaShaderSpirv(
        vertexSource, Oot3d::PicaAotShaderStage::Vertex, true, "oot3d_native_pica_scanout.vert"));
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    try {
        fragmentShader = CreateShaderModuleFromSpirv(ResolveNativePicaShaderSpirv(
            fragmentSource, Oot3d::PicaAotShaderStage::Fragment, false, "oot3d_native_pica_scanout.frag"));
    } catch (...) {
        vkDestroyShaderModule(mDevice, vertexShader, nullptr);
        throw;
    }

    const std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, vertexShader, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, fragmentShader, "main", nullptr},
    }};
    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;
    const std::array<VkDynamicState, 2> dynamicStates{
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount =
        static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();
    VkGraphicsPipelineCreateInfo pipelineInfo{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = mNativePicaScanoutPipelineLayout;
    pipelineInfo.renderPass = mRenderPass;
    VkResult result = VK_SUCCESS;
    if (mNativePicaScanoutPipeline == VK_NULL_HANDLE) {
        result = vkCreateGraphicsPipelines(
            mDevice, mPipelineCache, 1, &pipelineInfo, nullptr,
            &mNativePicaScanoutPipeline);
    }
    if (result == VK_SUCCESS &&
        mNativePicaScanoutOverlayPipeline == VK_NULL_HANDLE) {
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor =
            VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor =
            VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        pipelineInfo.renderPass = mOverlayRenderPass;
        result = vkCreateGraphicsPipelines(
            mDevice, mPipelineCache, 1, &pipelineInfo, nullptr,
            &mNativePicaScanoutOverlayPipeline);
    }
    vkDestroyShaderModule(mDevice, vertexShader, nullptr);
    vkDestroyShaderModule(mDevice, fragmentShader, nullptr);
    CheckNativeVk(result,
                  "vkCreateGraphicsPipelines(native PICA scanout)");
}

void GfxRenderingAPIVulkan::CreateNativePicaRenderPass() {
    if (mNativePicaRenderPass != VK_NULL_HANDLE && mNativePicaCanonicalRenderPass != VK_NULL_HANDLE) {
        return;
    }
    if (mDepthFormat == VK_FORMAT_UNDEFINED) {
        throw std::runtime_error("native PICA render pass requires a resolved depth format");
    }
    // Android's API-29 loader stub does not export Vulkan 1.2 entry points.
    // Resolve against the selected device, whose Vulkan capabilities are checked
    // at initialization; this also works with desktop and custom driver loaders.
    const auto createRenderPass2 = reinterpret_cast<PFN_vkCreateRenderPass2>(
        vkGetDeviceProcAddr(mDevice, "vkCreateRenderPass2"));
    if (createRenderPass2 == nullptr) {
        throw std::runtime_error("native PICA requires Vulkan vkCreateRenderPass2");
    }
    const auto createRenderPass = [this, createRenderPass2](uint32_t colorCount, VkRenderPass& renderPass) {
        const bool multisampled = mNativePicaSampleCount != VK_SAMPLE_COUNT_1_BIT;
        const uint32_t depthIndex = colorCount;
        const uint32_t resolveBase = colorCount + 1U;
        const uint32_t depthResolveIndex = resolveBase + colorCount;
        std::vector<VkAttachmentDescription2> attachments(multisampled ? depthResolveIndex + 1U : depthIndex + 1U);
        for (auto& attachment : attachments) {
            attachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        }
        constexpr std::array<VkFormat, Oot3d::kPicaColorAttachmentCount> colorFormats{
            VK_FORMAT_R8G8B8A8_UNORM,      VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_R32G32B32A32_SFLOAT
        };
        for (uint32_t index = 0; index < colorCount; ++index) {
            attachments[index].format = colorFormats[index];
            attachments[index].samples = mNativePicaSampleCount;
            attachments[index].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachments[index].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[index].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[index].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[index].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachments[index].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        attachments[depthIndex].format = mDepthFormat;
        attachments[depthIndex].samples = mNativePicaSampleCount;
        attachments[depthIndex].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[depthIndex].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[depthIndex].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[depthIndex].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[depthIndex].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments[depthIndex].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        std::array<VkAttachmentReference2, Oot3d::kPicaColorAttachmentCount> colorReferences{};
        for (uint32_t index = 0; index < colorCount; ++index) {
            colorReferences[index] = { VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, index,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT };
        }
        VkAttachmentReference2 depthReference{ VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, depthIndex,
                                               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                               NativeDepthAspect(mDepthFormat) };
        std::array<VkAttachmentReference2, Oot3d::kPicaColorAttachmentCount> resolveReferences{};
        VkAttachmentReference2 depthResolveReference{ VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr,
                                                      depthResolveIndex,
                                                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                                      VK_IMAGE_ASPECT_DEPTH_BIT };
        VkSubpassDescriptionDepthStencilResolve depthResolve{
            VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE
        };
        if (multisampled) {
            for (uint32_t index = 0; index < colorCount; ++index) {
                attachments[resolveBase + index] = attachments[index];
                attachments[resolveBase + index].samples = VK_SAMPLE_COUNT_1_BIT;
                resolveReferences[index] = { VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, resolveBase + index,
                                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT };
            }
            attachments[depthResolveIndex] = attachments[depthIndex];
            attachments[depthResolveIndex].samples = VK_SAMPLE_COUNT_1_BIT;
            depthResolve.depthResolveMode = mNativePicaDepthResolveMode;
            depthResolve.stencilResolveMode = VK_RESOLVE_MODE_NONE;
            depthResolve.pDepthStencilResolveAttachment = &depthResolveReference;
        }
        VkSubpassDescription2 subpass{ VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2 };
        subpass.pNext = multisampled ? &depthResolve : nullptr;
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = colorCount;
        subpass.pColorAttachments = colorReferences.data();
        subpass.pResolveAttachments = multisampled ? resolveReferences.data() : nullptr;
        subpass.pDepthStencilAttachment = &depthReference;
        std::array<VkSubpassDependency2, 2> dependencies{};
        for (auto& dependency : dependencies) {
            dependency.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        }
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dependencies[0].dstStageMask =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dependencies[1].srcAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        VkRenderPassCreateInfo2 info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2 };
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = static_cast<uint32_t>(dependencies.size());
        info.pDependencies = dependencies.data();
        CheckNativeVk(createRenderPass2(mDevice, &info, nullptr, &renderPass), "vkCreateRenderPass2(native PICA)");
    };
    createRenderPass(1U, mNativePicaCanonicalRenderPass);
    // The legacy render pass has no albedo guide; GI requires dynamic rendering.
    createRenderPass(static_cast<uint32_t>(Oot3d::kPicaBaseInstrumentedColorAttachmentCount), mNativePicaRenderPass);
}

void GfxRenderingAPIVulkan::ApplyNativePicaSampleCount(VkSampleCountFlagBits sampleCount) {
    if (sampleCount == mNativePicaSampleCount) return;
    if (sampleCount != VK_SAMPLE_COUNT_1_BIT &&
        ((mNativePicaSupportedSampleCounts & sampleCount) == 0U ||
         mNativePicaDepthResolveMode == VK_RESOLVE_MODE_NONE)) {
        sampleCount = VK_SAMPLE_COUNT_1_BIT;
    }
    WaitForAllPresents();
    CheckNativeVk(vkDeviceWaitIdle(mDevice),
                  "vkDeviceWaitIdle(native PICA MSAA transaction)");
    EndNativePicaRenderPass();
    ForgetNativePicaEffectNriTextures();
    mAmbientOcclusionPass.InvalidateScreenResources();
    mCacaoPass.InvalidateScreenResources();
    mMxaoPass.InvalidateScreenResources();
    mFidelityFxSssrPass.InvalidateScreenResources();
    mBrixelizerGiPass.InvalidateScreenResources();
    mReflectionMaterialResolvePass.InvalidateScreenResources();
    mLinearSceneColorPass.InvalidateScreenResources();
    mHiZDepthPyramidPass.InvalidateScreenResources();
    if (mHiZReflectionSurface.has_value()) {
        mSceneSurfaces.Retire(*mHiZReflectionSurface);
        mHiZReflectionSurface.reset();
    }
    mHiZReflectionPass.InvalidateScreenResources();
    if (mMotionSurface.has_value()) {
        mSceneSurfaces.Retire(*mMotionSurface);
        mMotionSurface.reset();
    }
    mMotionVectorPass.InvalidateScreenResources();
    if (mLinearColorSurface.has_value()) {
        mSceneSurfaces.Retire(*mLinearColorSurface);
        mLinearColorSurface.reset();
    }
    mNriUpscalerPass.InvalidateScreenResources();
    mUpscalerGuidePass.InvalidateScreenResources();
    mNativeUiOverlayPass.InvalidateScreenResources();
    if (mCompositeSurface.has_value()) {
        mSceneSurfaces.Retire(*mCompositeSurface);
        mCompositeSurface.reset();
    }
    mSceneCompositePass.InvalidateScreenResources();
    mSmaa1xPass.InvalidateScreenResources();
    mNriEffectGraphTransientImageArena.InvalidateScreenResources();
    if (mTaaSurface.has_value()) {
        mSceneSurfaces.Retire(*mTaaSurface);
        mTaaSurface.reset();
    }
    mTemporalAaPass.InvalidateScreenResources();
    mTaaOutputValid = false;
    mAmbientOcclusionOutputValid = false;
    mHiZOutputValid = false;
    mInteractiveGrassPass.Shutdown();
    mTemporalHistory.Reset();
    mRigidMotionTracker.Reset();
    mPreviousPicaVertexUniforms.clear();
    for (auto& [key, target] : mNativePicaRenderTargets) {
        DestroyNativePicaRenderTarget(target);
    }
    mNativePicaRenderTargets.clear();
    mNativePicaDisplayDepthTargets.clear();
    DestroyNativePicaPipelines();
    if (mNativePicaRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(mDevice, mNativePicaRenderPass, nullptr);
        mNativePicaRenderPass = VK_NULL_HANDLE;
    }
    if (mNativePicaCanonicalRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(mDevice, mNativePicaCanonicalRenderPass, nullptr);
        mNativePicaCanonicalRenderPass = VK_NULL_HANDLE;
    }
    mActiveNativePicaRenderTarget = nullptr;
    mNativePicaRenderPassActive = false;
    mNativePicaSampleCount = sampleCount;
    CreateNativePicaRenderPass();
    if (!mInteractiveGrassPass.Initialize(mPhysicalDevice, mDevice,
                                           mNriInterop.Shaders(),
                                           mNativePicaCanonicalRenderPass,
                                           mNativePicaRenderPass,
                                           mNativePicaSampleCount,
                                           mPicaDynamicRenderingScope.Available(),
                                           mDepthFormat, mPipelineCache)) {
        SPDLOG_INFO("OOT3D Vulkan interactive grass unavailable after MSAA change: {}",
                    mInteractiveGrassPass.UnavailableReason());
    }
    SPDLOG_INFO("OOT3D Vulkan native PICA MSAA changed to {} samples",
                static_cast<uint32_t>(mNativePicaSampleCount));
}

std::optional<Oot3d::PerspectiveViewState> GfxRenderingAPIVulkan::SamplePerspective(
    const NativePicaRenderTarget* target) const {
    auto perspective = Oot3d::SceneViewRuntime::Instance().LatestPerspective();
    if (!perspective.has_value() || target == nullptr) return perspective;
    if (const auto view = target->SampleView.Resolve(mFrameCounter))
        return Oot3d::ResolveSamplePerspective(*perspective, *view);
    return perspective;
}

GfxRenderingAPIVulkan::NativePicaRenderTarget&
GfxRenderingAPIVulkan::GetOrCreateNativePicaRenderTarget(
    const GfxNativePicaDrawView& draw,
    uint32_t restoredWidth, uint32_t restoredHeight) {
    NativePicaRenderTargetKey key{
        draw.RenderTargetNamespace,
        draw.FramebufferColorPhysicalAddress,
        draw.FramebufferDepthPhysicalAddress,
        draw.FramebufferWidth,
        draw.FramebufferHeight,
        draw.FramebufferColorFormat,
        draw.FramebufferDepthFormat,
        static_cast<uint16_t>(std::lround(mInternalResolutionScale * 1000.0F))};
    const bool restoredExtentAvailable =
        restoredWidth != 0U && restoredHeight != 0U;
    const auto targetExtent = restoredExtentAvailable
        ? Oot3d::RenderExtent{restoredWidth, restoredHeight}
        : Oot3d::ResolveNativePicaRenderExtent(
              {key.Width, key.Height},
              {mSwapchainExtent.width, mSwapchainExtent.height},
              mInternalResolutionScale);
    if (const auto found = mNativePicaRenderTargets.find(key);
        found != mNativePicaRenderTargets.end()) {
        mDiagnostics.RecordRenderResolution(mInternalResolutionScale, mSwapchainExtent.width,
            mSwapchainExtent.height, found->second.Width, found->second.Height);
        PrepareNativePicaTargetAttachments(found->second);
        return found->second;
    }
    mDiagnostics.RecordRenderResolution(mInternalResolutionScale, mSwapchainExtent.width,
        mSwapchainExtent.height, targetExtent.Width, targetExtent.Height);

    NativePicaRenderTarget target;
    target.Key = key;
    target.Width = targetExtent.Width;
    target.Height = targetExtent.Height;
    target.Attachments = mFramePicaAttachmentRequirements.BaseGuides();
    const bool instrumented = !target.Attachments.NativeColorOnly();
    VkFormatProperties shadowFormatProperties{};
    vkGetPhysicalDeviceFormatProperties(mPhysicalDevice, VK_FORMAT_R32_UINT,
                                        &shadowFormatProperties);
    if ((shadowFormatProperties.optimalTilingFeatures &
         VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT) == 0U) {
        throw std::runtime_error(
            "VK_FORMAT_R32_UINT storage image atomics are unavailable");
    }

    Oot3d::NriPicaRenderTargetImages nriImages;
    if (mNriPicaRenderTargetOwner.Create(
            {target.Width, target.Height, mDepthFormat,
             mNativePicaSampleCount, target.Attachments},
            nriImages)) {
        target.ColorImage = nriImages.Color;
        target.NormalGuideImage = nriImages.NormalGuide;
        target.MaterialGuideImage = nriImages.MaterialGuide;
        target.RigidMotionGuideImage = nriImages.RigidMotionGuide;
        target.AmbientGuideImage = nriImages.AmbientGuide;
        target.FogGuideImage = nriImages.FogGuide;
        target.OutlineGeometryGuideImage = nriImages.OutlineGeometryGuide;
        target.ShadowImage = nriImages.Shadow;
        target.DepthImage = nriImages.Depth;
        target.MsaaColorImage = nriImages.MsaaColor;
        target.MsaaNormalGuideImage = nriImages.MsaaNormalGuide;
        target.MsaaMaterialGuideImage = nriImages.MsaaMaterialGuide;
        target.MsaaRigidMotionGuideImage =
            nriImages.MsaaRigidMotionGuide;
        target.MsaaAmbientGuideImage = nriImages.MsaaAmbientGuide;
        target.MsaaFogGuideImage = nriImages.MsaaFogGuide;
        target.MsaaOutlineGeometryGuideImage = nriImages.MsaaOutlineGeometryGuide;
        target.MsaaDepthImage = nriImages.MsaaDepth;
        target.NriOwned = true;
        mDiagnostics.RecordNriPicaRenderTarget(
            true, Oot3d::CountNriPicaRenderTargetImages(nriImages));
    } else {
        const auto createImage = [&](
                                     VkFormat format,
                                     VkImageUsageFlags usage,
                                     VkSampleCountFlagBits samples,
                                     VkImage& image,
                                     VkDeviceMemory& memory) {
            VkImageCreateInfo imageInfo{
                VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.extent = {target.Width, target.Height, 1};
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.format = format;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imageInfo.usage = usage;
            imageInfo.samples = samples;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            CheckNativeVk(
                vkCreateImage(mDevice, &imageInfo, nullptr, &image),
                "vkCreateImage(native PICA target)");
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(mDevice, image, &requirements);
            VkMemoryAllocateInfo allocation{
                VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = FindMemoryType(
                requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            CheckNativeVk(
                vkAllocateMemory(
                    mDevice, &allocation, nullptr, &memory),
                "vkAllocateMemory(native PICA target)");
            CheckNativeVk(
                vkBindImageMemory(mDevice, image, memory, 0),
                "vkBindImageMemory(native PICA target)");
        };
        createImage(
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_SAMPLE_COUNT_1_BIT, target.ColorImage,
            target.ColorMemory);
        if (instrumented) {
            createImage(
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_SAMPLE_COUNT_1_BIT, target.NormalGuideImage,
                target.NormalGuideMemory);
            createImage(
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_SAMPLE_COUNT_1_BIT, target.MaterialGuideImage,
                target.MaterialGuideMemory);
            createImage(
                VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_SAMPLE_COUNT_1_BIT, target.RigidMotionGuideImage,
                target.RigidMotionGuideMemory);
            createImage(
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_SAMPLE_COUNT_1_BIT, target.AmbientGuideImage,
                target.AmbientGuideMemory);
            createImage(VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_SAMPLE_COUNT_1_BIT, target.FogGuideImage, target.FogGuideMemory);
            createImage(VK_FORMAT_R32G32B32A32_SFLOAT,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_SAMPLE_COUNT_1_BIT, target.OutlineGeometryGuideImage, target.OutlineGeometryGuideMemory);
        }
        createImage(
            VK_FORMAT_R32_UINT,
            VK_IMAGE_USAGE_STORAGE_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_SAMPLE_COUNT_1_BIT, target.ShadowImage,
            target.ShadowMemory);
        createImage(
            mDepthFormat,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_SAMPLE_COUNT_1_BIT, target.DepthImage,
            target.DepthMemory);
        if (mNativePicaSampleCount != VK_SAMPLE_COUNT_1_BIT) {
            const VkImageUsageFlags msaaColorUsage =
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            createImage(
                VK_FORMAT_R8G8B8A8_UNORM, msaaColorUsage,
                mNativePicaSampleCount, target.MsaaColorImage,
                target.MsaaColorMemory);
            if (instrumented) {
                createImage(
                    VK_FORMAT_R8G8B8A8_UNORM, msaaColorUsage,
                    mNativePicaSampleCount, target.MsaaNormalGuideImage,
                    target.MsaaNormalGuideMemory);
                createImage(
                    VK_FORMAT_R8G8B8A8_UNORM, msaaColorUsage,
                    mNativePicaSampleCount, target.MsaaMaterialGuideImage,
                    target.MsaaMaterialGuideMemory);
                createImage(
                    VK_FORMAT_R16G16B16A16_SFLOAT, msaaColorUsage,
                    mNativePicaSampleCount,
                    target.MsaaRigidMotionGuideImage,
                    target.MsaaRigidMotionGuideMemory);
                createImage(
                    VK_FORMAT_R8G8B8A8_UNORM, msaaColorUsage,
                    mNativePicaSampleCount,
                    target.MsaaAmbientGuideImage,
                    target.MsaaAmbientGuideMemory);
                createImage(VK_FORMAT_R8G8B8A8_UNORM, msaaColorUsage,
                    mNativePicaSampleCount, target.MsaaFogGuideImage, target.MsaaFogGuideMemory);
                createImage(VK_FORMAT_R32G32B32A32_SFLOAT, msaaColorUsage, mNativePicaSampleCount,
                            target.MsaaOutlineGeometryGuideImage, target.MsaaOutlineGeometryGuideMemory);
            }
            createImage(
                mDepthFormat,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                mNativePicaSampleCount, target.MsaaDepthImage,
                target.MsaaDepthMemory);
        }
    }

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    view.image = target.ColorImage;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    CheckNativeVk(vkCreateImageView(mDevice, &view, nullptr,
                                    &target.ColorView),
                  "vkCreateImageView(native PICA color)");
    if (instrumented) {
        view.image = target.NormalGuideImage;
        CheckNativeVk(vkCreateImageView(mDevice, &view, nullptr,
                                        &target.NormalGuideView),
                      "vkCreateImageView(native PICA normal guide)");
        view.image = target.MaterialGuideImage;
        CheckNativeVk(vkCreateImageView(mDevice, &view, nullptr,
                                        &target.MaterialGuideView),
                      "vkCreateImageView(native PICA material guide)");
        view.image = target.RigidMotionGuideImage;
        view.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        CheckNativeVk(vkCreateImageView(mDevice, &view, nullptr,
                                        &target.RigidMotionGuideView),
                      "vkCreateImageView(native PICA rigid motion guide)");
        view.image = target.AmbientGuideImage;
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
        CheckNativeVk(vkCreateImageView(mDevice, &view, nullptr,
                                        &target.AmbientGuideView),
                      "vkCreateImageView(native PICA ambient guide)");
        view.image = target.FogGuideImage;
        CheckNativeVk(vkCreateImageView(mDevice, &view, nullptr, &target.FogGuideView),
                      "vkCreateImageView(native PICA fog guide)");
        view.image = target.OutlineGeometryGuideImage;
        view.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        CheckNativeVk(vkCreateImageView(mDevice, &view, nullptr, &target.OutlineGeometryGuideView),
                      "vkCreateImageView(native PICA outline geometry guide)");
    }
    view.image = target.ShadowImage;
    view.format = VK_FORMAT_R32_UINT;
    CheckNativeVk(vkCreateImageView(mDevice, &view, nullptr,
                                    &target.ShadowView),
                  "vkCreateImageView(native PICA shadow storage)");
    view.image = target.DepthImage;
    view.format = mDepthFormat;
    view.subresourceRange.aspectMask = NativeDepthAspect(mDepthFormat);
    CheckNativeVk(vkCreateImageView(mDevice, &view, nullptr,
                                    &target.DepthView),
                  "vkCreateImageView(native PICA depth)");
    if (mNativePicaSampleCount != VK_SAMPLE_COUNT_1_BIT) {
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.image = target.MsaaColorImage;
        CheckNativeVk(vkCreateImageView(mDevice, &view, nullptr,
                                        &target.MsaaColorView),
                      "vkCreateImageView(native PICA MSAA color)");
        if (instrumented) {
            view.image = target.MsaaNormalGuideImage;
            CheckNativeVk(vkCreateImageView(mDevice, &view, nullptr,
                                            &target.MsaaNormalGuideView),
                          "vkCreateImageView(native PICA MSAA normal guide)");
            view.image = target.MsaaMaterialGuideImage;
            CheckNativeVk(vkCreateImageView(mDevice, &view, nullptr,
                                            &target.MsaaMaterialGuideView),
                          "vkCreateImageView(native PICA MSAA material guide)");
            view.image = target.MsaaRigidMotionGuideImage;
            view.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            CheckNativeVk(vkCreateImageView(mDevice, &view, nullptr,
                                            &target.MsaaRigidMotionGuideView),
                          "vkCreateImageView(native PICA MSAA rigid motion guide)");
            view.image = target.MsaaAmbientGuideImage;
            view.format = VK_FORMAT_R8G8B8A8_UNORM;
            CheckNativeVk(vkCreateImageView(mDevice, &view, nullptr,
                                            &target.MsaaAmbientGuideView),
                          "vkCreateImageView(native PICA MSAA ambient guide)");
            view.image = target.MsaaFogGuideImage;
            CheckNativeVk(vkCreateImageView(mDevice, &view, nullptr, &target.MsaaFogGuideView),
                          "vkCreateImageView(native PICA MSAA fog guide)");
            view.image = target.MsaaOutlineGeometryGuideImage;
            view.format = VK_FORMAT_R32G32B32A32_SFLOAT;
            CheckNativeVk(vkCreateImageView(mDevice, &view, nullptr, &target.MsaaOutlineGeometryGuideView),
                          "vkCreateImageView(native PICA MSAA outline geometry guide)");
        }
        view.image = target.MsaaDepthImage;
        view.format = mDepthFormat;
        view.subresourceRange.aspectMask = NativeDepthAspect(mDepthFormat);
        CheckNativeVk(vkCreateImageView(mDevice, &view, nullptr,
                                        &target.MsaaDepthView),
                      "vkCreateImageView(native PICA MSAA depth)");
    }

    std::vector<VkImageView> views;
    if (mNativePicaSampleCount == VK_SAMPLE_COUNT_1_BIT && instrumented) {
        views = { target.ColorView,
                  target.NormalGuideView,
                  target.MaterialGuideView,
                  target.RigidMotionGuideView,
                  target.AmbientGuideView,
                  target.FogGuideView,
                  target.OutlineGeometryGuideView,
                  target.DepthView };
    } else if (mNativePicaSampleCount != VK_SAMPLE_COUNT_1_BIT &&
               instrumented) {
        views = { target.MsaaColorView,
                  target.MsaaNormalGuideView,
                  target.MsaaMaterialGuideView,
                  target.MsaaRigidMotionGuideView,
                  target.MsaaAmbientGuideView,
                  target.MsaaFogGuideView,
                  target.MsaaOutlineGeometryGuideView,
                  target.MsaaDepthView,
                  target.ColorView,
                  target.NormalGuideView,
                  target.MaterialGuideView,
                  target.RigidMotionGuideView,
                  target.AmbientGuideView,
                  target.FogGuideView,
                  target.OutlineGeometryGuideView,
                  target.DepthView };
    } else if (mNativePicaSampleCount == VK_SAMPLE_COUNT_1_BIT) {
        views = {target.ColorView, target.DepthView};
    } else {
        views = {target.MsaaColorView, target.MsaaDepthView,
                 target.ColorView, target.DepthView};
    }
    VkFramebufferCreateInfo framebuffer{
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = instrumented
        ? mNativePicaRenderPass
        : mNativePicaCanonicalRenderPass;
    framebuffer.attachmentCount = static_cast<uint32_t>(views.size());
    framebuffer.pAttachments = views.data();
    framebuffer.width = target.Width;
    framebuffer.height = target.Height;
    framebuffer.layers = 1;
    CheckNativeVk(vkCreateFramebuffer(mDevice, &framebuffer, nullptr, &target.Framebuffer),
                  "vkCreateFramebuffer(native PICA)");
    if (!instrumented) {
        target.CanonicalFramebuffer = target.Framebuffer;
        target.Framebuffer = VK_NULL_HANDLE;
    }

    if (mNriPicaRenderTargetInitPass.Available()) {
        EndNativePicaRenderPass();
        if (mRenderPassActive) {
            vkCmdEndRenderPass(mCommandBuffers[mCurrentFrame]);
            mRenderPassActive = false;
            mOverlayRenderPassActive = false;
        }
    }
    const Oot3d::NriPicaRenderTargetImages initImages{
        target.ColorImage,
        target.NormalGuideImage,
        target.MaterialGuideImage,
        target.RigidMotionGuideImage,
        target.AmbientGuideImage,
        target.FogGuideImage,
        target.OutlineGeometryGuideImage,
        target.ShadowImage,
        target.DepthImage,
        target.MsaaColorImage,
        target.MsaaNormalGuideImage,
        target.MsaaMaterialGuideImage,
        target.MsaaRigidMotionGuideImage,
        target.MsaaAmbientGuideImage,
        target.MsaaFogGuideImage,
        target.MsaaOutlineGeometryGuideImage,
        target.MsaaDepthImage,
    };
    const bool nriInitialized =
        mFrameActive && mNriPicaRenderTargetInitPass.Execute({
            mCurrentFrame,
            target.Width,
            target.Height,
            mDepthFormat,
            mNativePicaSampleCount,
            target.Attachments,
            initImages,
        });
    if (nriInitialized) {
        mDiagnostics.RecordNriPicaRenderTargetInitialization(true, mNriPicaRenderTargetInitPass.LastClearedImageCount(),
                                                             mNriPicaRenderTargetInitPass.LastBarrierCount());
    } else if (instrumented) {
        VkCommandBuffer command = BeginImmediateCommands();
        std::array<VkImageMemoryBarrier, 9> toTransfer{};
        toTransfer[0] = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        toTransfer[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer[0].image = target.ColorImage;
        toTransfer[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer[0].subresourceRange.levelCount = 1;
        toTransfer[0].subresourceRange.layerCount = 1;
        toTransfer[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer[1] = toTransfer[0];
        toTransfer[1].image = target.NormalGuideImage;
        toTransfer[2] = toTransfer[0];
        toTransfer[2].image = target.MaterialGuideImage;
        toTransfer[3] = toTransfer[0];
        toTransfer[3].image = target.RigidMotionGuideImage;
        toTransfer[4] = toTransfer[0];
        toTransfer[4].image = target.AmbientGuideImage;
        toTransfer[5] = toTransfer[0];
        toTransfer[5].image = target.ShadowImage;
        toTransfer[6] = toTransfer[0];
        toTransfer[6].image = target.DepthImage;
        toTransfer[6].subresourceRange.aspectMask = NativeDepthAspect(mDepthFormat);
        toTransfer[7] = toTransfer[0];
        toTransfer[7].image = target.FogGuideImage;
        toTransfer[8] = toTransfer[7];
        toTransfer[8].image = target.OutlineGeometryGuideImage;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, static_cast<uint32_t>(toTransfer.size()), toTransfer.data());
        VkClearColorValue black{};
        const VkClearColorValue noFog{{0.0F, 0.0F, 0.0F, 1.0F}};
        vkCmdClearColorImage(command, target.FogGuideImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &noFog, 1, &toTransfer[7].subresourceRange);
        vkCmdClearColorImage(command, target.OutlineGeometryGuideImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &noFog, 1,
                             &toTransfer[8].subresourceRange);
        vkCmdClearColorImage(command, target.ColorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                             &toTransfer[0].subresourceRange);
        const VkClearColorValue flatNormal{ { 0.5F, 0.5F, 1.0F, 0.0F } };
        vkCmdClearColorImage(command, target.NormalGuideImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &flatNormal, 1,
                             &toTransfer[1].subresourceRange);
        vkCmdClearColorImage(command, target.MaterialGuideImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                             &toTransfer[2].subresourceRange);
        vkCmdClearColorImage(command, target.RigidMotionGuideImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                             &toTransfer[3].subresourceRange);
        const VkClearColorValue ambientFallback{
            {1.0F, 1.0F, 1.0F, 0.0F}};
        vkCmdClearColorImage(command, target.AmbientGuideImage,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &ambientFallback, 1,
                             &toTransfer[4].subresourceRange);
        VkClearColorValue emptyShadow{};
        emptyShadow.uint32[0] = 0xFFFFFFFFU;
        vkCmdClearColorImage(command, target.ShadowImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &emptyShadow, 1,
                             &toTransfer[5].subresourceRange);
        const VkClearDepthStencilValue clearDepth{ 1.0F, 0U };
        vkCmdClearDepthStencilImage(command, target.DepthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearDepth, 1,
                                    &toTransfer[6].subresourceRange);
        std::array<VkImageMemoryBarrier, 9> toAttachment = toTransfer;
        toAttachment[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toAttachment[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toAttachment[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toAttachment[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toAttachment[1] = toAttachment[0];
        toAttachment[7] = toAttachment[0];
        toAttachment[7].image = target.FogGuideImage;
        toAttachment[8] = toAttachment[7];
        toAttachment[8].image = target.OutlineGeometryGuideImage;
        toAttachment[1].image = target.NormalGuideImage;
        toAttachment[2] = toAttachment[0];
        toAttachment[2].image = target.MaterialGuideImage;
        toAttachment[3] = toAttachment[0];
        toAttachment[3].image = target.RigidMotionGuideImage;
        toAttachment[4].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toAttachment[4].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toAttachment[4].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toAttachment[4].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toAttachment[5].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toAttachment[5].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toAttachment[5].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toAttachment[5].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        toAttachment[6].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toAttachment[6].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        toAttachment[6].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toAttachment[6].dstAccessMask =
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(toAttachment.size()),
                             toAttachment.data());
        EndImmediateCommands(command);
        if (mNativePicaSampleCount != VK_SAMPLE_COUNT_1_BIT) {
            command = BeginImmediateCommands();
            std::array<VkImageMemoryBarrier, 8> msaaBarriers{};
            const std::array<VkImage, 8> msaaImages{
                target.MsaaColorImage,         target.MsaaNormalGuideImage,
                target.MsaaMaterialGuideImage, target.MsaaRigidMotionGuideImage,
                target.MsaaAmbientGuideImage,  target.MsaaDepthImage,
                target.MsaaFogGuideImage,      target.MsaaOutlineGeometryGuideImage
            };
            for (uint32_t index = 0; index < msaaBarriers.size(); ++index) {
                auto& barrier = msaaBarriers[index];
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = msaaImages[index];
                barrier.subresourceRange.aspectMask =
                    index == 5U ? NativeDepthAspect(mDepthFormat) : VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount = 1U;
                barrier.subresourceRange.layerCount = 1U;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            }
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, static_cast<uint32_t>(msaaBarriers.size()), msaaBarriers.data());
            vkCmdClearColorImage(command, target.MsaaColorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                                 &msaaBarriers[0].subresourceRange);
            vkCmdClearColorImage(command, target.MsaaNormalGuideImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &flatNormal, 1, &msaaBarriers[1].subresourceRange);
            vkCmdClearColorImage(command, target.MsaaMaterialGuideImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black,
                                 1, &msaaBarriers[2].subresourceRange);
            vkCmdClearColorImage(command, target.MsaaRigidMotionGuideImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &black, 1, &msaaBarriers[3].subresourceRange);
            vkCmdClearColorImage(command, target.MsaaAmbientGuideImage,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &ambientFallback, 1,
                                 &msaaBarriers[4].subresourceRange);
            vkCmdClearDepthStencilImage(command, target.MsaaDepthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        &clearDepth, 1, &msaaBarriers[5].subresourceRange);
            vkCmdClearColorImage(command, target.MsaaFogGuideImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &noFog, 1, &msaaBarriers[6].subresourceRange);
            vkCmdClearColorImage(command, target.MsaaOutlineGeometryGuideImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &noFog, 1, &msaaBarriers[7].subresourceRange);
            for (uint32_t index = 0; index < msaaBarriers.size(); ++index) {
                auto& barrier = msaaBarriers[index];
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.newLayout = index == 5U ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask =
                    index == 5U
                        ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                        : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            }
            vkCmdPipelineBarrier(
                command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0,
                nullptr, 0, nullptr, static_cast<uint32_t>(msaaBarriers.size()), msaaBarriers.data());
            EndImmediateCommands(command);
        }
    } else {
        VkCommandBuffer command = BeginImmediateCommands();
        std::array<VkImageMemoryBarrier, 3> barriers{};
        const std::array<VkImage, 3> images{
            target.ColorImage, target.ShadowImage, target.DepthImage};
        for (uint32_t index = 0; index < barriers.size(); ++index) {
            auto& barrier = barriers[index];
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = images[index];
            barrier.subresourceRange.aspectMask =
                index == 2U ? NativeDepthAspect(mDepthFormat) : VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1U;
            barrier.subresourceRange.layerCount = 1U;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, static_cast<uint32_t>(barriers.size()), barriers.data());
        const VkClearColorValue black{};
        vkCmdClearColorImage(command, target.ColorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                             &barriers[0].subresourceRange);
        VkClearColorValue emptyShadow{};
        emptyShadow.uint32[0] = 0xFFFFFFFFU;
        vkCmdClearColorImage(command, target.ShadowImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &emptyShadow, 1,
                             &barriers[1].subresourceRange);
        const VkClearDepthStencilValue clearDepth{ 1.0F, 0U };
        vkCmdClearDepthStencilImage(command, target.DepthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearDepth, 1,
                                    &barriers[2].subresourceRange);
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barriers[2].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[2].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barriers[2].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[2].dstAccessMask =
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(barriers.size()), barriers.data());
        EndImmediateCommands(command);

        if (mNativePicaSampleCount != VK_SAMPLE_COUNT_1_BIT) {
            command = BeginImmediateCommands();
            std::array<VkImageMemoryBarrier, 2> msaaBarriers{};
            const std::array<VkImage, 2> msaaImages{
                target.MsaaColorImage, target.MsaaDepthImage};
            for (uint32_t index = 0; index < msaaBarriers.size(); ++index) {
                auto& barrier = msaaBarriers[index];
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = msaaImages[index];
                barrier.subresourceRange.aspectMask =
                    index == 1U ? NativeDepthAspect(mDepthFormat) : VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount = 1U;
                barrier.subresourceRange.layerCount = 1U;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            }
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, static_cast<uint32_t>(msaaBarriers.size()), msaaBarriers.data());
            vkCmdClearColorImage(command, target.MsaaColorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                                 &msaaBarriers[0].subresourceRange);
            vkCmdClearDepthStencilImage(command, target.MsaaDepthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        &clearDepth, 1, &msaaBarriers[1].subresourceRange);
            for (uint32_t index = 0; index < msaaBarriers.size(); ++index) {
                auto& barrier = msaaBarriers[index];
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.newLayout = index == 1U ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask =
                    index == 1U
                        ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                        : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            }
            vkCmdPipelineBarrier(
                command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0,
                nullptr, 0, nullptr, static_cast<uint32_t>(msaaBarriers.size()), msaaBarriers.data());
            EndImmediateCommands(command);
        }
    }

    auto& inserted = mNativePicaRenderTargets.emplace(key, std::move(target))
                         .first->second;
    const auto publishSurface = [&](VkImage image, VkFormat format,
                                    VkImageUsageFlags usage,
                                    Oot3d::SceneSurfaceKind kind,
                                    uint32_t physicalAddress) -> uint64_t {
        if (image == VK_NULL_HANDLE)
            return 0U;
        const uintptr_t nativeImage = reinterpret_cast<uintptr_t>(image);
        const auto& surface = mSceneSurfaces.Publish(
            { { key.RenderTargetNamespace, physicalAddress, kind },
              nativeImage, inserted.Width, inserted.Height,
              static_cast<uint32_t>(format), 0, true });
        mResourceStates.Transition(nativeImage,
            { kind == Oot3d::SceneSurfaceKind::Depth
                  ? Oot3d::ResourceAccess::DepthAttachment
                  : Oot3d::ResourceAccess::ColorAttachment,
              mGraphicsQueueFamily });
        mNriInterop.WrapTexture(image, format, VK_IMAGE_TYPE_2D, usage,
                                inserted.Width, inserted.Height);
        return surface.Generation;
    };
    inserted.ColorSurfaceGeneration = publishSurface(
        inserted.ColorImage, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
        Oot3d::SceneSurfaceKind::Color, key.ColorPhysicalAddress);
    inserted.DepthSurfaceGeneration = publishSurface(
        inserted.DepthImage, mDepthFormat,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
        Oot3d::SceneSurfaceKind::Depth, key.DepthPhysicalAddress);
    publishSurface(inserted.NormalGuideImage, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT,
                   Oot3d::SceneSurfaceKind::NormalGuide,
                   key.ColorPhysicalAddress);
    publishSurface(inserted.MaterialGuideImage, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT,
                   Oot3d::SceneSurfaceKind::MaterialGuide,
                   key.ColorPhysicalAddress);
    publishSurface(inserted.RigidMotionGuideImage,
                   VK_FORMAT_R16G16B16A16_SFLOAT,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT,
                   Oot3d::SceneSurfaceKind::RigidMotionGuide,
                   key.ColorPhysicalAddress);
    publishSurface(inserted.AmbientGuideImage,
                   VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT,
                   Oot3d::SceneSurfaceKind::AmbientGuide,
                   key.ColorPhysicalAddress);
    publishSurface(inserted.FogGuideImage, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT,
                   Oot3d::SceneSurfaceKind::FogGuide, key.ColorPhysicalAddress);
    publishSurface(inserted.OutlineGeometryGuideImage, VK_FORMAT_R32G32B32A32_SFLOAT,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   Oot3d::SceneSurfaceKind::OutlineGeometryGuide, key.ColorPhysicalAddress);
    if (key.RenderTargetNamespace == 0U) {
        mInvalidatedNativePicaRenderTargetAddresses.erase(
            key.ColorPhysicalAddress);
    }
    return inserted;
}

void GfxRenderingAPIVulkan::DestroyNativePicaRenderTarget(
    NativePicaRenderTarget& target) {
    for (VkImage image :
         { target.ColorImage, target.DepthImage, target.NormalGuideImage, target.MaterialGuideImage,
           target.RigidMotionGuideImage, target.AmbientGuideImage, target.FogGuideImage,
           target.OutlineGeometryGuideImage, target.ShadowImage, target.MsaaColorImage, target.MsaaNormalGuideImage,
           target.MsaaMaterialGuideImage, target.MsaaRigidMotionGuideImage, target.MsaaAmbientGuideImage,
           target.MsaaFogGuideImage, target.MsaaOutlineGeometryGuideImage, target.MsaaDepthImage,
           target.DiffuseAlbedoGuideImage, target.MsaaDiffuseAlbedoGuideImage }) {
        if (image == VK_NULL_HANDLE) continue;
        mNriPicaDisplayCopyPass.ForgetTexture(image);
        mResourceStates.Forget(reinterpret_cast<uintptr_t>(image));
        if (!target.NriOwned) mNriInterop.ForgetTexture(image);
    }
    mSceneSurfaces.Retire({ target.Key.RenderTargetNamespace,
                            target.Key.ColorPhysicalAddress,
                            Oot3d::SceneSurfaceKind::Color });
    mSceneSurfaces.Retire({ target.Key.RenderTargetNamespace,
                            target.Key.DepthPhysicalAddress,
                            Oot3d::SceneSurfaceKind::Depth });
    mSceneSurfaces.Retire({ target.Key.RenderTargetNamespace,
                            target.Key.ColorPhysicalAddress,
                            Oot3d::SceneSurfaceKind::NormalGuide });
    mSceneSurfaces.Retire({ target.Key.RenderTargetNamespace,
                            target.Key.ColorPhysicalAddress,
                            Oot3d::SceneSurfaceKind::MaterialGuide });
    mSceneSurfaces.Retire({ target.Key.RenderTargetNamespace,
                            target.Key.ColorPhysicalAddress,
                            Oot3d::SceneSurfaceKind::RigidMotionGuide });
    mSceneSurfaces.Retire({ target.Key.RenderTargetNamespace,
                            target.Key.ColorPhysicalAddress,
                            Oot3d::SceneSurfaceKind::AmbientGuide });
    mSceneSurfaces.Retire({ target.Key.RenderTargetNamespace,
                            target.Key.ColorPhysicalAddress, Oot3d::SceneSurfaceKind::FogGuide });
    mSceneSurfaces.Retire({ target.Key.RenderTargetNamespace, target.Key.ColorPhysicalAddress,
                            Oot3d::SceneSurfaceKind::OutlineGeometryGuide });
    mSceneSurfaces.Retire({ target.Key.RenderTargetNamespace, target.Key.ColorPhysicalAddress,
                            Oot3d::SceneSurfaceKind::DiffuseAlbedoGuide });
    if (target.Framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(mDevice, target.Framebuffer, nullptr);
    }
    if (target.CanonicalFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(mDevice, target.CanonicalFramebuffer, nullptr);
    }
    if (target.ColorView != VK_NULL_HANDLE) {
        vkDestroyImageView(mDevice, target.ColorView, nullptr);
    }
    if (target.NormalGuideView != VK_NULL_HANDLE) {
        vkDestroyImageView(mDevice, target.NormalGuideView, nullptr);
    }
    if (target.MaterialGuideView != VK_NULL_HANDLE) {
        vkDestroyImageView(mDevice, target.MaterialGuideView, nullptr);
    }
    if (target.RigidMotionGuideView != VK_NULL_HANDLE) {
        vkDestroyImageView(mDevice, target.RigidMotionGuideView, nullptr);
    }
    if (target.AmbientGuideView != VK_NULL_HANDLE) {
        vkDestroyImageView(mDevice, target.AmbientGuideView, nullptr);
    }
    if (target.FogGuideView != VK_NULL_HANDLE) {
        vkDestroyImageView(mDevice, target.FogGuideView, nullptr);
    }
    if (target.OutlineGeometryGuideView != VK_NULL_HANDLE) {
        vkDestroyImageView(mDevice, target.OutlineGeometryGuideView, nullptr);
    }
    if (target.ShadowView != VK_NULL_HANDLE) {
        vkDestroyImageView(mDevice, target.ShadowView, nullptr);
    }
    if (target.DepthView != VK_NULL_HANDLE) {
        vkDestroyImageView(mDevice, target.DepthView, nullptr);
    }
    for (VkImageView view : { target.MsaaColorView, target.MsaaNormalGuideView, target.MsaaMaterialGuideView,
                              target.MsaaRigidMotionGuideView, target.MsaaAmbientGuideView, target.MsaaFogGuideView,
                              target.MsaaOutlineGeometryGuideView, target.MsaaDepthView,
                              target.DiffuseAlbedoGuideView, target.MsaaDiffuseAlbedoGuideView }) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(mDevice, view, nullptr);
    }
    // The albedo guide is grown outside the target owner's image set.
    for (const auto& [image, memory] :
         { std::pair{ target.DiffuseAlbedoGuideImage, target.DiffuseAlbedoGuideMemory },
           std::pair{ target.MsaaDiffuseAlbedoGuideImage, target.MsaaDiffuseAlbedoGuideMemory } }) {
        if (image == VK_NULL_HANDLE) continue;
        if (target.NriOwned) {
            mNriInterop.DestroyOwnedTexture(image);
        } else {
            vkDestroyImage(mDevice, image, nullptr);
            if (memory != VK_NULL_HANDLE) vkFreeMemory(mDevice, memory, nullptr);
        }
    }
    if (target.NriOwned) {
        mNriPicaRenderTargetOwner.Destroy({
            target.ColorImage,
            target.NormalGuideImage,
            target.MaterialGuideImage,
            target.RigidMotionGuideImage,
            target.AmbientGuideImage,
            target.FogGuideImage,
            target.OutlineGeometryGuideImage,
            target.ShadowImage,
            target.DepthImage,
            target.MsaaColorImage,
            target.MsaaNormalGuideImage,
            target.MsaaMaterialGuideImage,
            target.MsaaRigidMotionGuideImage,
            target.MsaaAmbientGuideImage,
            target.MsaaFogGuideImage,
            target.MsaaOutlineGeometryGuideImage,
            target.MsaaDepthImage,
        });
    } else {
        for (VkImage image :
             { target.ColorImage, target.NormalGuideImage, target.MaterialGuideImage, target.RigidMotionGuideImage,
               target.AmbientGuideImage, target.FogGuideImage, target.OutlineGeometryGuideImage, target.ShadowImage,
               target.DepthImage, target.MsaaColorImage, target.MsaaNormalGuideImage, target.MsaaMaterialGuideImage,
               target.MsaaRigidMotionGuideImage, target.MsaaAmbientGuideImage, target.MsaaFogGuideImage,
               target.MsaaOutlineGeometryGuideImage, target.MsaaDepthImage }) {
            if (image != VK_NULL_HANDLE)
                vkDestroyImage(mDevice, image, nullptr);
        }
        for (VkDeviceMemory memory :
             { target.ColorMemory, target.NormalGuideMemory, target.MaterialGuideMemory, target.RigidMotionGuideMemory,
               target.AmbientGuideMemory, target.FogGuideMemory, target.OutlineGeometryGuideMemory, target.ShadowMemory,
               target.DepthMemory, target.MsaaColorMemory, target.MsaaNormalGuideMemory, target.MsaaMaterialGuideMemory,
               target.MsaaRigidMotionGuideMemory, target.MsaaAmbientGuideMemory, target.MsaaFogGuideMemory,
               target.MsaaOutlineGeometryGuideMemory, target.MsaaDepthMemory }) {
            if (memory != VK_NULL_HANDLE)
                vkFreeMemory(mDevice, memory, nullptr);
        }
    }
    target = {};
}

GfxRenderingAPIVulkan::NativePicaDisplayImage&
GfxRenderingAPIVulkan::GetOrCreateNativePicaDisplayImage(
    uint64_t renderTargetNamespace, uint32_t physicalAddress,
    uint32_t width, uint32_t height) {
    return GetOrCreateNativePicaDisplayImage(
        mNativePicaDisplayImages, renderTargetNamespace, physicalAddress,
        width, height);
}

void GfxRenderingAPIVulkan::DestroyNativePicaSceneBeforeUiImages() {
    for (auto* images : {&mNativePicaSceneBeforeUiTargets,
                         &mNativePicaSceneBeforeUiDisplays}) {
        for (auto& [key, image] : *images) {
            DestroyNativePicaDisplayImage(image);
        }
        images->clear();
    }
}

GfxRenderingAPIVulkan::NativePicaDisplayImage&
GfxRenderingAPIVulkan::GetOrCreateNativePicaDisplayImage(
    std::map<std::pair<uint64_t, uint32_t>, NativePicaDisplayImage>& images,
    uint64_t renderTargetNamespace, uint32_t physicalAddress,
    uint32_t width, uint32_t height) {
    const auto key = std::make_pair(renderTargetNamespace, physicalAddress);
    auto found = images.find(key);
    if (found != images.end() &&
        found->second.Width == width && found->second.Height == height) {
        return found->second;
    }
    if (found != images.end()) {
        DestroyNativePicaDisplayImage(found->second);
        images.erase(found);
    }

    NativePicaDisplayImage image;
    image.Width = width;
    image.Height = height;
    Oot3d::NriPicaTextureImage nriImage;
    if (mNriPicaDisplayImageOwner.Create(
            width, height, VK_FORMAT_R8G8B8A8_UNORM, 1U, nriImage,
            true)) {
        image.Image = nriImage.Image;
        image.View = nriImage.View;
        image.NriOwned = true;
        mDiagnostics.RecordNriPicaDisplayImage(true);
    } else {
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_STORAGE_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CheckNativeVk(
            vkCreateImage(mDevice, &imageInfo, nullptr, &image.Image),
            "vkCreateImage(native PICA display transfer)");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(mDevice, image.Image, &requirements);
        VkMemoryAllocateInfo allocation{
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = FindMemoryType(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CheckNativeVk(
            vkAllocateMemory(
                mDevice, &allocation, nullptr, &image.Memory),
            "vkAllocateMemory(native PICA display transfer)");
        CheckNativeVk(
            vkBindImageMemory(mDevice, image.Image, image.Memory, 0),
            "vkBindImageMemory(native PICA display transfer)");
        VkImageViewCreateInfo view{
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.Image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        CheckNativeVk(
            vkCreateImageView(mDevice, &view, nullptr, &image.View),
            "vkCreateImageView(native PICA display transfer)");
    }
    return images.emplace(key, std::move(image)).first->second;
}

void GfxRenderingAPIVulkan::DestroyNativePicaDisplayImage(
    NativePicaDisplayImage& image) {
    mNriPicaDisplayCopyPass.ForgetTexture(image.Image);
    if (image.NriOwned) {
        mNriPicaDisplayImageOwner.Destroy(image.Image);
    } else {
        if (image.Image != VK_NULL_HANDLE)
            mNriInterop.ForgetTexture(image.Image);
        if (image.View != VK_NULL_HANDLE) {
            vkDestroyImageView(mDevice, image.View, nullptr);
        }
        if (image.Image != VK_NULL_HANDLE) {
            vkDestroyImage(mDevice, image.Image, nullptr);
        }
        if (image.Memory != VK_NULL_HANDLE) {
            vkFreeMemory(mDevice, image.Memory, nullptr);
        }
    }
    image = {};
}

uint32_t NativePicaMipLevelCount(
    const GfxNativePicaTextureView& texture) {
    if (texture.Width == 0U || texture.Height == 0U) {
        return 0U;
    }
    uint32_t width = texture.Width;
    uint32_t height = texture.Height;
    uint32_t levels = 1U;
    while (width > 8U && height > 8U) {
        ++levels;
        width >>= 1U;
        height >>= 1U;
    }
    return std::min(levels,
                    static_cast<uint32_t>(texture.MaxMipLevel) + 1U);
}

std::optional<size_t> NativePicaEncodedMipSize(
    uint8_t format, uint32_t width, uint32_t height) {
    static constexpr std::array<uint8_t, 14> kNibblesPerPixel{
        8U, 6U, 4U, 4U, 4U, 4U, 4U,
        2U, 2U, 2U, 1U, 1U, 1U, 2U};
    if (format >= kNibblesPerPixel.size() || width == 0U ||
        height == 0U) {
        return std::nullopt;
    }
    const uint64_t tiledWidth = (std::max(8U, width) + 7U) & ~7ULL;
    const uint64_t tiledHeight = (std::max(8U, height) + 7U) & ~7ULL;
    const uint64_t nibbles = tiledWidth * tiledHeight *
                             kNibblesPerPixel[format];
    const uint64_t bytes = (nibbles + 1U) / 2U;
    if (bytes > std::numeric_limits<size_t>::max()) {
        return std::nullopt;
    }
    return static_cast<size_t>(bytes);
}

std::optional<uint64_t> ResolveNativeTextureBaseLevelContentHash(
    const GfxNativePicaTextureView& texture) {
    if (!texture.CopyReferences.empty()) return std::nullopt;
    if (texture.NativeBaseLevelContentHashAvailable) {
        return texture.NativeBaseLevelContentHash;
    }
    const auto byteCount = NativePicaEncodedMipSize(
        texture.NativeFormat, texture.Width, texture.Height);
    if (!byteCount.has_value() || *byteCount > texture.NativeBytes.size()) {
        return std::nullopt;
    }
    return HashNativeBytes(texture.NativeBytes.first(*byteCount));
}

std::optional<size_t> NativePicaDecodedMipChainSize(
    uint32_t width, uint32_t height, uint32_t levels) {
    if (width == 0U || height == 0U || levels == 0U) {
        return std::nullopt;
    }
    uint64_t bytes = 0U;
    for (uint32_t level = 0U; level < levels; ++level) {
        bytes += static_cast<uint64_t>(std::max(1U, width >> level)) *
                 std::max(1U, height >> level) * 4U;
    }
    if (bytes > std::numeric_limits<size_t>::max()) {
        return std::nullopt;
    }
    return static_cast<size_t>(bytes);
}

GfxNativeTextureFilter NativePicaMinFilter(
    bool minLinear, bool mipLinear, uint32_t mipLevels) {
    if (mipLevels <= 1U) {
        return minLinear ? GfxNativeTextureFilter::Linear
                         : GfxNativeTextureFilter::Nearest;
    }
    if (mipLinear) {
        return minLinear ? GfxNativeTextureFilter::LinearMipmapLinear
                         : GfxNativeTextureFilter::NearestMipmapLinear;
    }
    return minLinear ? GfxNativeTextureFilter::LinearMipmapNearest
                     : GfxNativeTextureFilter::NearestMipmapNearest;
}

std::vector<uint8_t> GfxRenderingAPIVulkan::CaptureNativePicaImageBytes(
    VkImage image, uint32_t width, uint32_t height,
    uint32_t bytesPerPixel,
    VkImageLayout stableLayout, VkPipelineStageFlags stableStage,
    VkAccessFlags stableAccess, uint32_t mipLevel) {
    const uint64_t byteCount64 =
        static_cast<uint64_t>(width) * height * bytesPerPixel;
    if (image == VK_NULL_HANDLE || bytesPerPixel == 0U ||
        byteCount64 == 0U ||
        byteCount64 > std::numeric_limits<size_t>::max()) {
        throw std::runtime_error(
            "native PICA RGBA8 image has an invalid identity");
    }
    const auto byteCount = static_cast<VkDeviceSize>(byteCount64);
    auto readback = CreateBuffer(
        byteCount, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        true);
    try {
        VkCommandBuffer command = BeginImmediateCommands();
        VkImageMemoryBarrier toTransfer{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransfer.oldLayout = stableLayout;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = image;
        toTransfer.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.baseMipLevel = mipLevel;
        toTransfer.subresourceRange.levelCount = 1U;
        toTransfer.subresourceRange.layerCount = 1U;
        toTransfer.srcAccessMask = stableAccess;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(
            command, stableStage, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toTransfer);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = mipLevel;
        copy.imageSubresource.layerCount = 1U;
        copy.imageExtent = {width, height, 1U};
        vkCmdCopyImageToBuffer(
            command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            readback.Buffer, 1U, &copy);
        VkImageMemoryBarrier toStable = toTransfer;
        toStable.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toStable.newLayout = stableLayout;
        toStable.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toStable.dstAccessMask = stableAccess;
        vkCmdPipelineBarrier(
            command, VK_PIPELINE_STAGE_TRANSFER_BIT, stableStage,
            0, 0, nullptr, 0, nullptr, 1, &toStable);
        EndImmediateCommands(command);

        const auto* bytes = static_cast<const uint8_t*>(readback.Mapped);
        std::vector<uint8_t> result(
            bytes, bytes + static_cast<size_t>(byteCount64));
        DestroyBuffer(readback);
        return result;
    } catch (...) {
        DestroyBuffer(readback);
        throw;
    }
}

void GfxRenderingAPIVulkan::RestoreNativePicaImageBytes(
    VkImage image, uint32_t width, uint32_t height,
    uint32_t bytesPerPixel, std::span<const uint8_t> bytes,
    VkImageLayout initialLayout,
    VkPipelineStageFlags initialStage, VkAccessFlags initialAccess,
    VkImageLayout stableLayout, VkPipelineStageFlags stableStage,
    VkAccessFlags stableAccess) {
    const uint64_t expectedBytes =
        static_cast<uint64_t>(width) * height * bytesPerPixel;
    if (image == VK_NULL_HANDLE || bytesPerPixel == 0U ||
        expectedBytes == 0U || expectedBytes != bytes.size()) {
        throw std::runtime_error(
            "native PICA RGBA8 restore payload is invalid");
    }
    auto staging = CreateBuffer(
        static_cast<VkDeviceSize>(expectedBytes),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        true);
    std::memcpy(staging.Mapped, bytes.data(), bytes.size());
    try {
        VkCommandBuffer command = BeginImmediateCommands();
        VkImageMemoryBarrier toTransfer{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransfer.oldLayout = initialLayout;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = image;
        toTransfer.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.levelCount = 1U;
        toTransfer.subresourceRange.layerCount = 1U;
        toTransfer.srcAccessMask = initialAccess;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(
            command, initialStage, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toTransfer);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1U;
        copy.imageExtent = {width, height, 1U};
        vkCmdCopyBufferToImage(
            command, staging.Buffer, image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &copy);
        VkImageMemoryBarrier toStable = toTransfer;
        toStable.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toStable.newLayout = stableLayout;
        toStable.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toStable.dstAccessMask = stableAccess;
        vkCmdPipelineBarrier(
            command, VK_PIPELINE_STAGE_TRANSFER_BIT, stableStage,
            0, 0, nullptr, 0, nullptr, 1, &toStable);
        EndImmediateCommands(command);
    } catch (...) {
        DestroyBuffer(staging);
        throw;
    }
    DestroyBuffer(staging);
}

void GfxRenderingAPIVulkan::CaptureNativePicaDepthStencilImage(
    VkImage image, uint32_t width, uint32_t height,
    std::vector<float>& depthValues,
    std::vector<uint8_t>& stencilValues) {
    depthValues.clear();
    stencilValues.clear();
    const uint64_t pixelCount64 =
        static_cast<uint64_t>(width) * height;
    if (image == VK_NULL_HANDLE || pixelCount64 == 0U ||
        pixelCount64 > std::numeric_limits<size_t>::max()) {
        throw std::runtime_error(
            "native PICA depth image has an invalid identity");
    }
    const bool hasStencil =
        mDepthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT ||
        mDepthFormat == VK_FORMAT_D24_UNORM_S8_UINT;
    if (mDepthFormat != VK_FORMAT_D32_SFLOAT &&
        mDepthFormat != VK_FORMAT_D32_SFLOAT_S8_UINT &&
        mDepthFormat != VK_FORMAT_D24_UNORM_S8_UINT) {
        throw std::runtime_error(
            "native PICA depth capture format is unsupported");
    }
    const auto pixelCount = static_cast<size_t>(pixelCount64);
    const VkDeviceSize depthBytes =
        static_cast<VkDeviceSize>(pixelCount64 * sizeof(uint32_t));
    const VkDeviceSize stencilOffset = depthBytes;
    const VkDeviceSize totalBytes =
        depthBytes + (hasStencil ? pixelCount64 : 0U);
    auto readback = CreateBuffer(
        totalBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        true);
    try {
        VkCommandBuffer command = BeginImmediateCommands();
        VkImageMemoryBarrier toTransfer{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransfer.oldLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = image;
        toTransfer.subresourceRange.aspectMask =
            NativeDepthAspect(mDepthFormat);
        toTransfer.subresourceRange.levelCount = 1U;
        toTransfer.subresourceRange.layerCount = 1U;
        toTransfer.srcAccessMask =
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(
            command,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
            nullptr, 1, &toTransfer);
        VkBufferImageCopy depthCopy{};
        depthCopy.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_DEPTH_BIT;
        depthCopy.imageSubresource.layerCount = 1U;
        depthCopy.imageExtent = {width, height, 1U};
        vkCmdCopyImageToBuffer(
            command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            readback.Buffer, 1U, &depthCopy);
        if (hasStencil) {
            VkBufferImageCopy stencilCopy{};
            stencilCopy.bufferOffset = stencilOffset;
            stencilCopy.imageSubresource.aspectMask =
                VK_IMAGE_ASPECT_STENCIL_BIT;
            stencilCopy.imageSubresource.layerCount = 1U;
            stencilCopy.imageExtent = {width, height, 1U};
            vkCmdCopyImageToBuffer(
                command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                readback.Buffer, 1U, &stencilCopy);
        }
        VkImageMemoryBarrier toAttachment = toTransfer;
        toAttachment.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toAttachment.newLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        toAttachment.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toAttachment.dstAccessMask =
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(
            command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toAttachment);
        EndImmediateCommands(command);

        depthValues.resize(pixelCount);
        const auto* bytes = static_cast<const uint8_t*>(readback.Mapped);
        if (mDepthFormat == VK_FORMAT_D24_UNORM_S8_UINT) {
            constexpr float scale = 1.0F / 16777215.0F;
            for (size_t index = 0; index < pixelCount; ++index) {
                uint32_t packed = 0U;
                std::memcpy(&packed,
                            bytes + index * sizeof(uint32_t),
                            sizeof(packed));
                depthValues[index] =
                    static_cast<float>(packed & 0x00FFFFFFU) * scale;
            }
        } else {
            std::memcpy(depthValues.data(), bytes,
                        static_cast<size_t>(depthBytes));
        }
        if (hasStencil) {
            stencilValues.assign(
                bytes + static_cast<size_t>(stencilOffset),
                bytes + static_cast<size_t>(stencilOffset + pixelCount64));
        }
        DestroyBuffer(readback);
    } catch (...) {
        DestroyBuffer(readback);
        throw;
    }
}

void GfxRenderingAPIVulkan::RestoreNativePicaDepthStencilImage(
    VkImage image, uint32_t width, uint32_t height,
    std::span<const float> depthValues,
    std::span<const uint8_t> stencilValues) {
    const uint64_t pixelCount64 =
        static_cast<uint64_t>(width) * height;
    if (image == VK_NULL_HANDLE || pixelCount64 == 0U ||
        pixelCount64 != depthValues.size() ||
        !std::all_of(depthValues.begin(), depthValues.end(),
                     [](float value) {
                         return std::isfinite(value) && value >= 0.0F &&
                                value <= 1.0F;
                     })) {
        throw std::runtime_error(
            "native PICA depth restore payload is invalid");
    }
    const bool hasStencil =
        mDepthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT ||
        mDepthFormat == VK_FORMAT_D24_UNORM_S8_UINT;
    if (mDepthFormat != VK_FORMAT_D32_SFLOAT &&
        mDepthFormat != VK_FORMAT_D32_SFLOAT_S8_UINT &&
        mDepthFormat != VK_FORMAT_D24_UNORM_S8_UINT) {
        throw std::runtime_error(
            "native PICA depth restore format is unsupported");
    }
    if (!stencilValues.empty() &&
        stencilValues.size() != pixelCount64) {
        throw std::runtime_error(
            "native PICA stencil restore payload is invalid");
    }
    const auto pixelCount = static_cast<size_t>(pixelCount64);
    const VkDeviceSize depthBytes =
        static_cast<VkDeviceSize>(pixelCount64 * sizeof(uint32_t));
    const VkDeviceSize stencilOffset = depthBytes;
    const VkDeviceSize totalBytes =
        depthBytes + (hasStencil ? pixelCount64 : 0U);
    auto staging = CreateBuffer(
        totalBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        true);
    auto* bytes = static_cast<uint8_t*>(staging.Mapped);
    if (mDepthFormat == VK_FORMAT_D24_UNORM_S8_UINT) {
        for (size_t index = 0; index < pixelCount; ++index) {
            const float depth = std::clamp(
                depthValues[index], 0.0F, 1.0F);
            const uint32_t packed = static_cast<uint32_t>(
                std::lround(depth * 16777215.0F));
            std::memcpy(bytes + index * sizeof(uint32_t),
                        &packed, sizeof(packed));
        }
    } else {
        std::memcpy(bytes, depthValues.data(),
                    static_cast<size_t>(depthBytes));
    }
    if (hasStencil) {
        auto* stencil = bytes + static_cast<size_t>(stencilOffset);
        if (stencilValues.empty()) {
            std::memset(stencil, 0, pixelCount);
        } else {
            std::memcpy(stencil, stencilValues.data(), pixelCount);
        }
    }
    try {
        VkCommandBuffer command = BeginImmediateCommands();
        VkImageMemoryBarrier toTransfer{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransfer.oldLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = image;
        toTransfer.subresourceRange.aspectMask =
            NativeDepthAspect(mDepthFormat);
        toTransfer.subresourceRange.levelCount = 1U;
        toTransfer.subresourceRange.layerCount = 1U;
        toTransfer.srcAccessMask =
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(
            command,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
            nullptr, 1, &toTransfer);
        VkBufferImageCopy depthCopy{};
        depthCopy.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_DEPTH_BIT;
        depthCopy.imageSubresource.layerCount = 1U;
        depthCopy.imageExtent = {width, height, 1U};
        vkCmdCopyBufferToImage(
            command, staging.Buffer, image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &depthCopy);
        if (hasStencil) {
            VkBufferImageCopy stencilCopy{};
            stencilCopy.bufferOffset = stencilOffset;
            stencilCopy.imageSubresource.aspectMask =
                VK_IMAGE_ASPECT_STENCIL_BIT;
            stencilCopy.imageSubresource.layerCount = 1U;
            stencilCopy.imageExtent = {width, height, 1U};
            vkCmdCopyBufferToImage(
                command, staging.Buffer, image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U,
                &stencilCopy);
        }
        VkImageMemoryBarrier toAttachment = toTransfer;
        toAttachment.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toAttachment.newLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        toAttachment.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toAttachment.dstAccessMask =
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(
            command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toAttachment);
        EndImmediateCommands(command);
    } catch (...) {
        DestroyBuffer(staging);
        throw;
    }
    DestroyBuffer(staging);
}

void GfxRenderingAPIVulkan::BeginNativePicaRenderPass(
    NativePicaRenderTarget& target) {
    PrepareNativePicaTargetAttachments(target);
    if (mNativePicaRenderPassActive &&
        mActiveNativePicaRenderTarget == &target) {
        return;
    }
    EndNativePicaRenderPass();
    if (mRenderPassActive) {
        vkCmdEndRenderPass(mCommandBuffers[mCurrentFrame]);
        mRenderPassActive = false;
        mOverlayRenderPassActive = false;
    }
    if (mPicaDynamicRenderingScope.Available()) {
        const bool multisampled =
            mNativePicaSampleCount != VK_SAMPLE_COUNT_1_BIT;
        Oot3d::PicaDynamicRenderingTarget renderingTarget;
        renderingTarget.Colors =
            multisampled
                ? std::array<VkImageView, Oot3d::kPicaColorAttachmentCount>{ target.MsaaColorView,
                                                                             target.MsaaNormalGuideView,
                                                                             target.MsaaMaterialGuideView,
                                                                             target.MsaaRigidMotionGuideView,
                                                                             target.MsaaAmbientGuideView,
                                                                             target.MsaaFogGuideView,
                                                                             target.MsaaOutlineGeometryGuideView,
                                                                             target.MsaaDiffuseAlbedoGuideView }
                : std::array<VkImageView, Oot3d::kPicaColorAttachmentCount>{ target.ColorView,
                                                                             target.NormalGuideView,
                                                                             target.MaterialGuideView,
                                                                             target.RigidMotionGuideView,
                                                                             target.AmbientGuideView,
                                                                             target.FogGuideView,
                                                                             target.OutlineGeometryGuideView,
                                                                             target.DiffuseAlbedoGuideView };
        renderingTarget.Resolves =
            multisampled ? std::array<VkImageView, Oot3d::kPicaColorAttachmentCount>{ target.ColorView,
                                                                                      target.NormalGuideView,
                                                                                      target.MaterialGuideView,
                                                                                      target.RigidMotionGuideView,
                                                                                      target.AmbientGuideView,
                                                                                      target.FogGuideView,
                                                                                      target.OutlineGeometryGuideView,
                                                                                      target.DiffuseAlbedoGuideView }
                         : std::array<VkImageView, Oot3d::kPicaColorAttachmentCount>{};
        renderingTarget.Depth =
            multisampled ? target.MsaaDepthView : target.DepthView;
        renderingTarget.DepthResolve =
            multisampled ? target.DepthView : VK_NULL_HANDLE;
        renderingTarget.ColorImages =
            multisampled ? std::array<VkImage, Oot3d::kPicaColorAttachmentCount>{ target.MsaaColorImage,
                                                                                  target.MsaaNormalGuideImage,
                                                                                  target.MsaaMaterialGuideImage,
                                                                                  target.MsaaRigidMotionGuideImage,
                                                                                  target.MsaaAmbientGuideImage,
                                                                                  target.MsaaFogGuideImage,
                                                                                  target.MsaaOutlineGeometryGuideImage,
                                                                                  target.MsaaDiffuseAlbedoGuideImage }
                         : std::array<VkImage, Oot3d::kPicaColorAttachmentCount>{ target.ColorImage,
                                                                                  target.NormalGuideImage,
                                                                                  target.MaterialGuideImage,
                                                                                  target.RigidMotionGuideImage,
                                                                                  target.AmbientGuideImage,
                                                                                  target.FogGuideImage,
                                                                                  target.OutlineGeometryGuideImage,
                                                                                  target.DiffuseAlbedoGuideImage };
        renderingTarget.ResolveImages =
            multisampled ? std::array<VkImage, Oot3d::kPicaColorAttachmentCount>{ target.ColorImage,
                                                                                  target.NormalGuideImage,
                                                                                  target.MaterialGuideImage,
                                                                                  target.RigidMotionGuideImage,
                                                                                  target.AmbientGuideImage,
                                                                                  target.FogGuideImage,
                                                                                  target.OutlineGeometryGuideImage,
                                                                                  target.DiffuseAlbedoGuideImage }
                         : std::array<VkImage, Oot3d::kPicaColorAttachmentCount>{};
        renderingTarget.DepthImage =
            multisampled ? target.MsaaDepthImage : target.DepthImage;
        renderingTarget.DepthResolveImage =
            multisampled ? target.DepthImage : VK_NULL_HANDLE;
        renderingTarget.ColorFormats = { VK_FORMAT_R8G8B8A8_UNORM,     VK_FORMAT_R8G8B8A8_UNORM,
                                         VK_FORMAT_R8G8B8A8_UNORM,     VK_FORMAT_R16G16B16A16_SFLOAT,
                                         VK_FORMAT_R8G8B8A8_UNORM,     VK_FORMAT_R8G8B8A8_UNORM,
                                         VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM };
        renderingTarget.DepthFormat = mDepthFormat;
        renderingTarget.ColorAttachmentCount =
            mFramePicaAttachmentRequirements.ColorAttachmentCount();
        renderingTarget.Width = target.Width;
        renderingTarget.Height = target.Height;
        renderingTarget.Samples = mNativePicaSampleCount;
        if (!mPicaDynamicRenderingScope.Begin(
                mCurrentFrame, mCommandBuffers[mCurrentFrame],
                renderingTarget))
            throw std::runtime_error(
                "native PICA dynamic rendering scope failed");
        mDiagnostics.RecordNativePicaDynamicRendering(
            mPicaDynamicRenderingScope.ActiveOwnedByNri());
    } else {
        VkRenderPassBeginInfo begin{
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = mFramePicaAttachmentRequirements.NativeColorOnly()
            ? mNativePicaCanonicalRenderPass
            : mNativePicaRenderPass;
        begin.framebuffer = NativePicaFramebuffer(target, mFramePicaAttachmentRequirements.NativeColorOnly());
        begin.renderArea.extent = {target.Width, target.Height};
        vkCmdBeginRenderPass(mCommandBuffers[mCurrentFrame], &begin,
                             VK_SUBPASS_CONTENTS_INLINE);
    }
    mGpuProfiler.BeginScope(
        mCurrentFrame, Oot3d::GpuProfileScope::NativePica,
        mCommandBuffers[mCurrentFrame]);
    if (mFrameGraphicsSettings.Effects.Toon !=
        Oot3d::ToonMode::Off) {
        mGpuProfiler.BeginScope(
            mCurrentFrame, Oot3d::GpuProfileScope::ToonRaster,
            mCommandBuffers[mCurrentFrame]);
    }
    mNativePicaRenderPassActive = true;
    mActiveNativePicaRenderTarget = &target;
}

void GfxRenderingAPIVulkan::EndNativePicaRenderPass() {
    if (!mNativePicaRenderPassActive) {
        return;
    }
    if (mPicaDynamicRenderingScope.Active()) {
        mPicaDynamicRenderingScope.End(
            mCurrentFrame, mCommandBuffers[mCurrentFrame]);
        mDiagnostics.RecordNriPicaGlobalBarriers(
            mPicaDynamicRenderingScope.LastNriGlobalBarrierCount());
    } else {
        vkCmdEndRenderPass(mCommandBuffers[mCurrentFrame]);
    }
    mGpuProfiler.EndScope(
        mCurrentFrame, Oot3d::GpuProfileScope::ToonRaster,
        mCommandBuffers[mCurrentFrame]);
    mGpuProfiler.EndScope(
        mCurrentFrame, Oot3d::GpuProfileScope::NativePica,
        mCommandBuffers[mCurrentFrame]);
    mNativePicaRenderPassActive = false;
    mActiveNativePicaRenderTarget = nullptr;
}

void GfxRenderingAPIVulkan::ApplyPendingNativePicaMemoryFills(NativePicaRenderTarget& target) {
    const uint64_t pixelCount = static_cast<uint64_t>(target.Key.Width) * target.Key.Height;
    const uint64_t colorEnd = static_cast<uint64_t>(target.Key.ColorPhysicalAddress) +
                              pixelCount * NativePicaColorBytesPerPixel(target.Key.ColorFormat);
    const uint64_t depthEnd = static_cast<uint64_t>(target.Key.DepthPhysicalAddress) +
                              pixelCount * NativePicaDepthBytesPerPixel(target.Key.DepthFormat);
    if (!mPicaMemoryFillSmokeInjected) {
        GfxNativePicaMemoryFillView smokeFill;
        if (BuildNativePicaMemoryFillSmoke(
                target.Key.RenderTargetNamespace,
                target.Key.ColorPhysicalAddress,
                target.Key.DepthPhysicalAddress, colorEnd, depthEnd,
                smokeFill)) {
            mPendingNativePicaMemoryFills.push_back(smokeFill);
            mPicaMemoryFillSmokeInjected = true;
            mDiagnostics.RecordMemoryFill();
        }
    }
    if (mPendingNativePicaMemoryFills.empty()) {
        return;
    }

    auto fill = mPendingNativePicaMemoryFills.begin();
    while (fill != mPendingNativePicaMemoryFills.end()) {
        if (fill->RenderTargetNamespace != target.Key.RenderTargetNamespace) {
            ++fill;
            continue;
        }
        const bool coversColor =
            fill->StartPhysicalAddress <= target.Key.ColorPhysicalAddress && fill->EndPhysicalAddress >= colorEnd;
        const bool coversDepth =
            fill->StartPhysicalAddress <= target.Key.DepthPhysicalAddress && fill->EndPhysicalAddress >= depthEnd;
        if (!coversColor && !coversDepth) {
            ++fill;
            continue;
        }
        bool nriShadowClear = false;
        uint32_t shadowBarrierCount = 0;
        if (coversColor && target.ShadowImage != VK_NULL_HANDLE) {
            EndNativePicaRenderPass();
            const uint32_t shadowClearValue = static_cast<uint32_t>(NativePicaFillByte(*fill, 0U)) |
                                              (static_cast<uint32_t>(NativePicaFillByte(*fill, 1U)) << 8U) |
                                              (static_cast<uint32_t>(NativePicaFillByte(*fill, 2U)) << 16U) |
                                              (static_cast<uint32_t>(NativePicaFillByte(*fill, 3U)) << 24U);
            nriShadowClear = mNriPicaMemoryFillClearPass.ClearShadow({
                mCurrentFrame,
                target.ShadowImage,
                target.Width,
                target.Height,
                shadowClearValue,
            });
            if (nriShadowClear) {
                shadowBarrierCount = mNriPicaMemoryFillClearPass.LastShadowBarrierCount();
            } else {
                VkImageMemoryBarrier toClear{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                toClear.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                toClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toClear.image = target.ShadowImage;
                toClear.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                toClear.subresourceRange.levelCount = 1U;
                toClear.subresourceRange.layerCount = 1U;
                toClear.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                toClear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                auto command = mCommandBuffers[mCurrentFrame];
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                     0, nullptr, 0, nullptr, 1, &toClear);
                VkClearColorValue shadowValue{};
                shadowValue.uint32[0] = shadowClearValue;
                vkCmdClearColorImage(command, target.ShadowImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &shadowValue, 1,
                                     &toClear.subresourceRange);
                std::swap(toClear.oldLayout, toClear.newLayout);
                toClear.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toClear.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                     0, nullptr, 0, nullptr, 1, &toClear);
            }
        }
        BeginNativePicaRenderPass(target);
        const VkClearColorValue decodedColor = DecodeNativePicaClearColor(*fill, target.Key.ColorFormat);
        const VkClearDepthStencilValue decodedDepth = DecodeNativePicaClearDepthStencil(*fill, target.Key.DepthFormat);
        Oot3d::NriPicaAttachmentClearDesc nriClear;
        nriClear.FrameIndex = mCurrentFrame;
        nriClear.Width = target.Width;
        nriClear.Height = target.Height;
        nriClear.Color = { decodedColor.float32[0], decodedColor.float32[1], decodedColor.float32[2],
                           decodedColor.float32[3] };
        nriClear.Depth = decodedDepth.depth;
        nriClear.Stencil = static_cast<uint8_t>(decodedDepth.stencil);
        nriClear.ClearColor = coversColor;
        nriClear.ClearDepth = coversDepth;
        nriClear.HasStencil = target.Key.DepthFormat == 3U;
        nriClear.NriRenderingScope = mPicaDynamicRenderingScope.ActiveOwnedByNri();
        nriClear.Attachments = mFramePicaAttachmentRequirements;
        const bool nriAttachmentClear = mNriPicaMemoryFillClearPass.ClearAttachments(nriClear);
        if (!nriAttachmentClear) {
            std::array<VkClearAttachment, Oot3d::kPicaColorAttachmentCount + 1U> attachments{};
            uint32_t attachmentCount = 0;
            if (coversColor) {
                auto& attachment = attachments[attachmentCount++];
                attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                attachment.colorAttachment = 0U;
                attachment.clearValue.color = decodedColor;
            }
            if (coversColor && !mFramePicaAttachmentRequirements.NativeColorOnly()) {
                auto& normalGuide = attachments[attachmentCount++];
                normalGuide.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                normalGuide.colorAttachment = 1U;
                normalGuide.clearValue.color = { { 0.5F, 0.5F, 1.0F, 0.0F } };
                auto& materialGuide = attachments[attachmentCount++];
                materialGuide.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                materialGuide.colorAttachment = 2U;
                materialGuide.clearValue.color = { { 0.0F, 0.0F, 0.0F, 0.0F } };
                auto& rigidMotionGuide = attachments[attachmentCount++];
                rigidMotionGuide.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                rigidMotionGuide.colorAttachment = 3U;
                rigidMotionGuide.clearValue.color = { { 0.0F, 0.0F, 0.0F, 0.0F } };
                auto& ambientGuide = attachments[attachmentCount++];
                ambientGuide.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                ambientGuide.colorAttachment = 4U;
                ambientGuide.clearValue.color = { { 1.0F, 1.0F, 1.0F, 0.0F } };
                auto& fogGuide = attachments[attachmentCount++];
                fogGuide.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                fogGuide.colorAttachment = static_cast<uint32_t>(Oot3d::PicaColorAttachment::FogGuide);
                fogGuide.clearValue.color = {{0, 0, 0, 1}};
            }
            if (coversColor && !mFramePicaAttachmentRequirements.NativeColorOnly()) {
                auto& guide = attachments[attachmentCount++];
                guide.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                guide.colorAttachment = static_cast<uint32_t>(Oot3d::PicaColorAttachment::OutlineGeometryGuide);
                guide.clearValue.color = {{0, 0, 0, 1}};
            }
            if (coversColor &&
                mFramePicaAttachmentRequirements.Requires(Oot3d::PicaAuxiliaryOutput::DiffuseAlbedoGuide)) {
                auto& guide = attachments[attachmentCount++];
                guide.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                guide.colorAttachment = static_cast<uint32_t>(Oot3d::PicaColorAttachment::DiffuseAlbedoGuide);
                guide.clearValue.color = {};
            }
            if (coversDepth) {
                auto& attachment = attachments[attachmentCount++];
                attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                if (target.Key.DepthFormat == 3U) {
                    attachment.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
                }
                attachment.clearValue.depthStencil = decodedDepth;
            }
            const VkClearRect clearRect{ { { 0, 0 }, { target.Width, target.Height } }, 0U, 1U };
            vkCmdClearAttachments(mCommandBuffers[mCurrentFrame], attachmentCount, attachments.data(), 1U, &clearRect);
        }
        mDiagnostics.RecordNriPicaMemoryFillClear(nriShadowClear, nriAttachmentClear, shadowBarrierCount);
        if (coversColor) {
            // Filled colour is new content: it carries no resolved scene
            // effects any more, so the provenance serial moves on.
            target.ContentWriteSerial = ++mGpuTextureCopyWriteSerial;
        }
        fill = mPendingNativePicaMemoryFills.erase(fill);
    }
}

VkDescriptorSet GfxRenderingAPIVulkan::AllocateNativePicaDescriptorSet() {
    VkDescriptorSetAllocateInfo allocation{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = mFrameResources[mCurrentFrame].DescriptorPool;
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &mNativePicaDescriptorSetLayout;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    CheckNativeVk(vkAllocateDescriptorSets(mDevice, &allocation, &descriptor),
                  "vkAllocateDescriptorSets(native PICA)");
    return descriptor;
}

VkDescriptorSet
GfxRenderingAPIVulkan::AllocateNativePicaScanoutDescriptorSet() {
    VkDescriptorSetAllocateInfo allocation{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = mFrameResources[mCurrentFrame].DescriptorPool;
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &mNativePicaScanoutDescriptorSetLayout;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    CheckNativeVk(vkAllocateDescriptorSets(mDevice, &allocation, &descriptor),
                  "vkAllocateDescriptorSets(native PICA scanout)");
    return descriptor;
}

GfxRenderingAPIVulkan::TextureRecord*
GfxRenderingAPIVulkan::CreateGpuCopiedTextureRecord(
    const NativePicaTextureKey& textureKey,
    const GfxNativePicaTextureView& texture, uint32_t width, uint32_t height) {
    auto [inserted, wasInserted] = mNativePicaTextures.emplace(textureKey,
                                                               TextureRecord{});
    if (!wasInserted) {
        return &inserted->second;
    }
    auto& record = inserted->second;
    record.SamplerState.MinFilter = NativePicaMinFilter(
        texture.MinLinear, texture.MipLinear, 1U);
    record.SamplerState.MagFilter = texture.MagLinear
        ? GfxNativeTextureFilter::Linear
        : GfxNativeTextureFilter::Nearest;
    record.SamplerState.WrapS = DecodeNativePicaWrap(texture.NativeWrapS);
    record.SamplerState.WrapT = DecodeNativePicaWrap(texture.NativeWrapT);
    record.SamplerState.LodBias = 0.0F;
    record.SamplerState.MinMipLevel = texture.MinMipLevel;
    record.SamplerState.MaxMipLevel = texture.MaxMipLevel;
    // The image is written by the base fill and the owned regions below, so it
    // is allocated without an upload and needs a storage view.
    CreateNativePicaTextureStorage(record, width, height, 1U,
                                   VK_FORMAT_R8G8B8A8_UNORM,
                                   /*storageView=*/true);
    record.UploadedMipLevels = 1U;
    record.Uploaded = true;
    RecreateSampler(record);
    return &record;
}

GfxRenderingAPIVulkan::TextureRecord*
GfxRenderingAPIVulkan::GetOrCreateNativePicaTexture(
    const GfxNativePicaTextureView& textureView, std::string* error,
    uint64_t replacementGeneration, uint64_t renderTargetNamespace,
    const uint64_t* precomputedContentHash) {
    const GfxNativePicaTextureView& texture = textureView;
    try {
        NativePicaTextureKey key;
        key.ReplacementGeneration = replacementGeneration;
        key.PhysicalAddress = texture.PhysicalAddress;
        key.Width = texture.Width;
        key.Height = texture.Height;
        key.Format = texture.NativeFormat;
        key.Type = texture.NativeType;
        key.WrapS = texture.NativeWrapS;
        key.WrapT = texture.NativeWrapT;
        key.MinLinear = texture.MinLinear;
        key.MagLinear = texture.MagLinear;
        key.MipLinear = texture.MipLinear;
        key.LodBiasRaw = texture.LodBiasRaw;
        key.MinMipLevel = texture.MinMipLevel;
        key.MaxMipLevel = texture.MaxMipLevel;
        const bool shadow2d = texture.NativeType == 2U;
        if (shadow2d && !texture.CopyReferences.empty()) {
            throw std::runtime_error(
                "GPU-produced PICA copies require an RGBA sampler, not a packed shadow sampler");
        }
        ++mGpuTextureCopySamplerCost.Resolutions;
        // Ownership comes from the producer references the view carries, never
        // from a missing content hash: a view with references is served by the
        // immutable copy that producer captured at its internal resolution.
        GpuTextureCopyDerivation derivation;
        GpuTextureCopyFallback copyFallback = GpuTextureCopyFallback::None;
        std::string derivationError;
        const bool derivesCopy =
            shadow2d == false && texture.CopyReferences.empty() == false &&
            BuildGpuTextureCopyDerivation(texture, renderTargetNamespace,
                                          derivation, &copyFallback,
                                          &derivationError);
        if (shadow2d == false && texture.CopyReferences.empty() == false &&
            derivesCopy == false) {
            switch (copyFallback) {
                case GpuTextureCopyFallback::UnknownProducer:
                    ++mGpuTextureCopySamplerCost.FallbackUnknownProducer;
                    break;
                case GpuTextureCopyFallback::UncapturedProducer:
                    ++mGpuTextureCopySamplerCost.FallbackUncapturedProducer;
                    break;
                case GpuTextureCopyFallback::MipChain:
                    ++mGpuTextureCopySamplerCost.FallbackMipChain;
                    break;
                case GpuTextureCopyFallback::Layout:
                case GpuTextureCopyFallback::None:
                    ++mGpuTextureCopySamplerCost.FallbackLayout;
                    break;
            }
            static bool derivationReported = false;
            if (!derivationReported) {
                derivationReported = true;
                std::fprintf(
                    stderr,
                    "TRIAEVUM_GPU_COPY_UNRESOLVED reason=%u texture=%ux%u "
                    "address=0x%08X detail=%s\n",
                    static_cast<unsigned>(copyFallback), texture.Width,
                    texture.Height, texture.PhysicalAddress,
                    derivationError.c_str());
            }
            throw std::runtime_error(
                "raw PICA copy samples cannot be resolved: " + derivationError);
        }
        key.GpuCopyIdentity = derivesCopy
            ? GpuTextureCopyReferenceIdentity(texture, renderTargetNamespace)
            : 0U;
        key.ContentHash = derivesCopy && derivation.CoversDeclaredLayout
            ? 0U
            : (precomputedContentHash != nullptr
                   ? *precomputedContentHash
                   : HashNativeBytes(texture.NativeBytes));
        if (derivesCopy) {
            const auto exact = mNativePicaTextures.find(key);
            if (exact != mNativePicaTextures.end()) {
                for (const auto identity : exact->second.CopyProducerIdentities) {
                    const auto producer = mGpuTextureCopySurfaces.find(identity);
                    if (producer != mGpuTextureCopySurfaces.end())
                        producer->second.LastReferencedFrame = mFrameCounter;
                }
                return &exact->second;
            }
            TextureRecord* record = CreateGpuCopiedTextureRecord(
                key, texture, derivation.ImageWidth, derivation.ImageHeight);
            try {
                // Only CPU-owned (uncovered) bytes are initialized from the
                // snapshot, at the declared native size: the expand into the
                // sampled density happens on the GPU, in the copy's own pass. A
                // fully owned view needs no CPU decode and no upload at all.
                if (derivation.CoversDeclaredLayout == false) {
                    const auto encodedBytes = NativePicaEncodedMipSize(
                        texture.NativeFormat, texture.Width, texture.Height);
                    if (encodedBytes.has_value() == false ||
                        *encodedBytes > texture.NativeBytes.size()) {
                        throw std::runtime_error(
                            "native PICA copy source bytes are truncated");
                    }
                    std::vector<uint8_t> nativePixels;
                    std::string decodeError;
                    if (Oot3d::DecodePicaTextureRgba8(
                            texture.NativeFormat, texture.Width,
                            texture.Height,
                            texture.NativeBytes.first(*encodedBytes),
                            nativePixels, &decodeError) == false) {
                        throw std::runtime_error(
                            "native PICA copy base decode failed: " +
                            decodeError);
                    }
                    TextureRecord base;
                    CreateNativePicaTextureImage(
                        base, nativePixels, texture.Width, texture.Height, 1U,
                        VK_FORMAT_R8G8B8A8_UNORM, /*storageView=*/false);
                    // The record owns the base image: it is sampled only until
                    // the fill dispatch that expands it has executed.
                    record->BaseImage = base.Image;
                    base.Image = VK_NULL_HANDLE;
                    base.NriImage = VK_NULL_HANDLE;
                    base.Memory = VK_NULL_HANDLE;
                    DestroyTexture(base);
                    if (RecordGpuTextureCopyBaseFill(derivation,
                                                    record->BaseImage,
                                                    *record) == false) {
                        throw std::runtime_error(
                            "native PICA copy base fill was refused");
                    }
                }
                if (RecordGpuTextureCopyDerivation(derivation, *record) ==
                    false) {
                    throw std::runtime_error(
                        "native PICA copy derivation recorded no region");
                }
            } catch (...) {
                const auto created = mNativePicaTextures.find(key);
                if (created != mNativePicaTextures.end()) {
                    DestroyTexture(created->second);
                    mNativePicaTextures.erase(created);
                }
                throw;
            }
            // The record itself carries the provenance and the owned rectangle,
            // so a later draw reads them on a cache hit as well, and a restored
            // record keeps them.
            record->SourceResolvedColour = derivation.SceneEffectsResolved;
            record->SourceOwnedX0 = derivation.OwnedX0;
            record->SourceOwnedY0 = derivation.OwnedY0;
            record->SourceOwnedX1 = derivation.OwnedX1;
            record->SourceOwnedY1 = derivation.OwnedY1;
            record->SourceDeclaredWidth = derivation.DeclaredWidth;
            record->SourceDeclaredHeight = derivation.DeclaredHeight;
            DumpGpuTextureCopyConsumer(
                record->Image, record->Width, record->Height, texture,
                derivation.FirstProducerCompletionId, derivation.Encoding,
                derivation.SceneEffectsResolved);
            ++mGpuTextureCopySamplerCost.GpuBound;
            mGpuTextureCopySamplerCost.GpuBytes +=
                uint64_t{derivation.ImageWidth} * derivation.ImageHeight * 4U;
            mGpuTextureCopySamplerCost.GpuExtentPixels +=
                uint64_t{derivation.ImageWidth} * derivation.ImageHeight;
            mGpuTextureCopySamplerCost.NativeExtentPixels +=
                uint64_t{texture.Width} * texture.Height;
            return record;
        }
        const auto baseLevelContentHash = !shadow2d
            ? ResolveNativeTextureBaseLevelContentHash(texture)
            : std::nullopt;
        if (baseLevelContentHash.has_value()) {
            Oot3d::TextureCatalogRuntime::Instance().Observe(
                *baseLevelContentHash, key.PhysicalAddress, key.Width,
                key.Height, key.Format);
        }

        const auto createTexture =
            [&](const NativePicaTextureKey& textureKey,
                std::span<const uint8_t> pixels,
                uint32_t width, uint32_t height,
                uint32_t mipLevels,
                VkFormat format, uint64_t replacementHash,
                bool replacement, bool* insertedResult = nullptr,
                uint64_t replacementPixelHash = 0U) {
                auto [inserted, wasInserted] =
                    mNativePicaTextures.emplace(
                        textureKey, TextureRecord{});
                if (insertedResult != nullptr) {
                    *insertedResult = wasInserted;
                }
                if (!wasInserted) {
                    return &inserted->second;
                }
                auto& record = inserted->second;
                record.SamplerState.MinFilter = !shadow2d
                    ? NativePicaMinFilter(texture.MinLinear,
                                          texture.MipLinear, mipLevels)
                    : GfxNativeTextureFilter::Nearest;
                record.SamplerState.MagFilter =
                    !shadow2d && texture.MagLinear
                        ? GfxNativeTextureFilter::Linear
                        : GfxNativeTextureFilter::Nearest;
                record.SamplerState.WrapS =
                    DecodeNativePicaWrap(texture.NativeWrapS);
                record.SamplerState.WrapT =
                    DecodeNativePicaWrap(texture.NativeWrapT);
                record.SamplerState.LodBias = 0.0F;
                record.SamplerState.MinMipLevel = !shadow2d
                    ? texture.MinMipLevel : 0U;
                record.SamplerState.MaxMipLevel = !shadow2d
                    ? texture.MaxMipLevel : 0U;
                record.CustomReplacementHash = replacementHash;
                record.CustomReplacementPixelHash = replacementPixelHash;
                record.CustomReplacementApplied = replacement;
                record.CustomReplacementReady = replacement;
                CreateNativePicaTextureImage(
                    record, pixels, width, height, mipLevels, format);
                return &record;
            };

        const auto ensureTextureUploaded =
            [&](const NativePicaTextureKey& textureKey,
                TextureRecord& record) -> TextureRecord* {
                if (record.Uploaded) {
                    return &record;
                }
                const auto expectedBytes = NativePicaDecodedMipChainSize(
                    record.Width, record.Height, record.MipLevels);
                if (!expectedBytes.has_value() ||
                    record.Rgba8.size() != *expectedBytes) {
                    throw std::runtime_error(
                        "restored native PICA texture payload is invalid");
                }
                auto pixels = std::move(record.Rgba8);
                // Snapshot uploads bypass the normal decode observer. Restore
                // native mask pixels too, never the custom replacement image.
                if (!shadow2d && baseLevelContentHash.has_value()) {
                    const size_t baseBytes =
                        static_cast<size_t>(key.Width) * key.Height * 4U;
                    if (!textureKey.CustomReplacement &&
                        record.Width == key.Width && record.Height == key.Height &&
                        pixels.size() >= baseBytes) {
                        Oot3d::GrassTextureSourceCache::Instance().ObserveDecoded(
                            *baseLevelContentHash, key.Width, key.Height,
                            std::span<const uint8_t>(pixels).first(baseBytes));
                    } else {
                        const auto encodedBytes = NativePicaEncodedMipSize(
                            texture.NativeFormat, key.Width, key.Height);
                        std::vector<uint8_t> nativePixels;
                        if (encodedBytes.has_value() &&
                            *encodedBytes <= texture.NativeBytes.size() &&
                            Oot3d::DecodePicaTextureRgba8(
                                texture.NativeFormat, key.Width, key.Height,
                                texture.NativeBytes.first(*encodedBytes),
                                nativePixels, nullptr)) {
                            Oot3d::GrassTextureSourceCache::Instance().ObserveDecoded(
                                *baseLevelContentHash, key.Width, key.Height,
                                nativePixels);
                        }
                    }
                }
                const VkFormat format =
                    textureKey.Type == 2U &&
                            !textureKey.CustomReplacement
                        ? VK_FORMAT_R32_UINT
                        : VK_FORMAT_R8G8B8A8_UNORM;
                CreateNativePicaTextureImage(
                    record, pixels, record.Width, record.Height,
                    record.MipLevels, format);
                record.Rgba8.clear();
                return &record;
            };

        NativePicaTextureKey replacementKey = key;
        replacementKey.CustomReplacement = true;
        if (!shadow2d) {
            if (const auto replacement =
                    mNativePicaTextures.find(replacementKey);
                replacement != mNativePicaTextures.end()) {
                return ensureTextureUploaded(
                    replacement->first, replacement->second);
            }
        }

        const auto found = mNativePicaTextures.find(key);
        if (found != mNativePicaTextures.end()) {
            auto& record = found->second;
            if (!shadow2d &&
                record.CustomReplacementHash != 0U &&
                (record.CustomReplacementPending ||
                 record.CustomReplacementReady)) {
                const auto resolved =
                    ::Oot3d::Renderer::AzaharTexturePackRuntime::
                        Instance()
                            .PollQueued(
                                record.CustomReplacementHash);
                if (resolved.State ==
                        ::Oot3d::Renderer::
                            AzaharTextureResolveState::Ready &&
                    resolved.Replacement != nullptr &&
                    resolved.Replacement->Rgba8 != nullptr) {
                    record.CustomReplacementPending = false;
                    record.CustomReplacementReady = true;
                    replacementKey.ReplacementGeneration = resolved.Replacement->Generation;
                    const uint64_t replacementBytes =
                        resolved.Replacement->Rgba8->size();
                    const bool budgetAvailable =
                        mCustomTextureUploadBytesThisFrame == 0U ||
                        (mCustomTextureUploadBytesThisFrame <
                             kCustomTextureUploadBudgetBytes &&
                         replacementBytes <=
                             kCustomTextureUploadBudgetBytes -
                                 mCustomTextureUploadBytesThisFrame);
                    if (!budgetAvailable) {
                        return ensureTextureUploaded(found->first, record);
                    }
                    bool inserted = false;
                    TextureRecord* promoted = createTexture(
                        replacementKey,
                        *resolved.Replacement->Rgba8,
                        resolved.Replacement->Width,
                        resolved.Replacement->Height,
                        1U,
                        VK_FORMAT_R8G8B8A8_UNORM,
                        resolved.NativeHash, true, &inserted, resolved.Replacement->PixelContentHash);
                    if (inserted) {
                        mCustomTextureUploadBytesThisFrame +=
                            replacementBytes;
                    }
                    return promoted;
                }
                if (resolved.State !=
                    ::Oot3d::Renderer::
                        AzaharTextureResolveState::Pending) {
                    record.CustomReplacementPending = false;
                    record.CustomReplacementReady = false;
                }
            }
            return ensureTextureUploaded(found->first, record);
        }

        if (shadow2d) {
            const auto liveTarget = std::find_if(
                mNativePicaRenderTargets.begin(),
                mNativePicaRenderTargets.end(),
                [&](const auto& entry) {
                    return entry.first.ColorPhysicalAddress ==
                               texture.PhysicalAddress &&
                           entry.first.Width == texture.Width &&
                           entry.first.Height == texture.Height &&
                           entry.second.ShadowImage != VK_NULL_HANDLE;
                });
            if (liveTarget != mNativePicaRenderTargets.end()) {
                auto [inserted, wasInserted] =
                    mNativePicaTextures.emplace(key, TextureRecord{});
                auto& record = inserted->second;
                record.NriImage = liveTarget->second.ShadowImage;
                record.ImageBorrowed = true;
                record.LiveRenderTargetAlias = true;
                record.Width = liveTarget->second.Width;
                record.Height = liveTarget->second.Height;
                record.MipLevels = 1U;
                record.UploadedMipLevels = 1U;
                record.SamplerState.MinFilter =
                    GfxNativeTextureFilter::Nearest;
                record.SamplerState.MagFilter =
                    GfxNativeTextureFilter::Nearest;
                record.SamplerState.WrapS =
                    DecodeNativePicaWrap(texture.NativeWrapS);
                record.SamplerState.WrapT =
                    DecodeNativePicaWrap(texture.NativeWrapT);
                record.SamplerState.MaxMipLevel = 0U;
                record.SamplerState.MinMipLevel = 0U;
                VkImageViewCreateInfo view{
                    VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                view.image = liveTarget->second.ShadowImage;
                view.viewType = VK_IMAGE_VIEW_TYPE_2D;
                view.format = VK_FORMAT_R32_UINT;
                view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                view.subresourceRange.levelCount = 1U;
                view.subresourceRange.layerCount = 1U;
                CheckNativeVk(vkCreateImageView(mDevice, &view, nullptr,
                                                &record.View),
                              "vkCreateImageView(native PICA live Shadow2D)");
                record.ImageLayout = VK_IMAGE_LAYOUT_GENERAL;
                record.Uploaded = true;
                RecreateSampler(record);
                return &record;
            }
        }

        std::vector<uint8_t> pixels;
        std::string decodeError;
        VkFormat imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
        uint32_t mipLevels = 1U;
        size_t baseNativeBytes = texture.NativeBytes.size();
        size_t baseDecodedBytes =
            static_cast<size_t>(texture.Width) * texture.Height * 4U;
        if (shadow2d) {
            std::vector<uint32_t> shadowPixels;
            if (!Oot3d::DetilePicaShadow2d(
                    texture.NativeBytes, texture.Width, texture.Height,
                    shadowPixels, &decodeError)) {
                throw std::runtime_error(
                    "native PICA Shadow2D decode failed: " + decodeError);
            }
            pixels.resize(shadowPixels.size() * sizeof(uint32_t));
            std::memcpy(pixels.data(), shadowPixels.data(), pixels.size());
            imageFormat = VK_FORMAT_R32_UINT;
        } else {
            mipLevels = NativePicaMipLevelCount(texture);
            if (mipLevels == 0U) {
                throw std::runtime_error(
                    "native PICA texture has no valid mip levels");
            }
            size_t nativeOffset = 0U;
            for (uint32_t level = 0U; level < mipLevels; ++level) {
                const uint32_t mipWidth =
                    std::max(1U, static_cast<uint32_t>(texture.Width) >> level);
                const uint32_t mipHeight =
                    std::max(1U, static_cast<uint32_t>(texture.Height) >> level);
                const auto encodedBytes = NativePicaEncodedMipSize(
                    texture.NativeFormat, mipWidth, mipHeight);
                if (!encodedBytes.has_value() ||
                    nativeOffset > texture.NativeBytes.size() ||
                    *encodedBytes > texture.NativeBytes.size() - nativeOffset) {
                    throw std::runtime_error(
                        "native PICA mip chain is truncated");
                }
                std::vector<uint8_t> decodedMip;
                if (!Oot3d::DecodePicaTextureRgba8(
                        texture.NativeFormat,
                        static_cast<uint16_t>(mipWidth),
                        static_cast<uint16_t>(mipHeight),
                        texture.NativeBytes.subspan(nativeOffset,
                                                    *encodedBytes),
                        decodedMip, &decodeError)) {
                    throw std::runtime_error(
                        "native PICA texture mip decode failed: " +
                        decodeError);
                }
                if (level == 0U) {
                    baseNativeBytes = *encodedBytes;
                    baseDecodedBytes = decodedMip.size();
                }
                pixels.insert(pixels.end(), decodedMip.begin(),
                              decodedMip.end());
                nativeOffset += *encodedBytes;
            }
            Oot3d::GrassTextureSourceCache::Instance().ObserveDecoded(
                baseLevelContentHash.value_or(key.ContentHash),
                key.Width, key.Height,
                std::span<const uint8_t>(pixels).first(baseDecodedBytes));
        }
        uint32_t uploadWidth = texture.Width;
        uint32_t uploadHeight = texture.Height;
        ::Oot3d::Renderer::AzaharTextureResolveResult
            replacementResult;
        if (!shadow2d) {
            replacementResult =
                ::Oot3d::Renderer::AzaharTexturePackRuntime::
                    Instance()
                        .ResolveOrQueue({
                            .Width = texture.Width,
                            .Height = texture.Height,
                            .NativeFormat =
                                texture.NativeFormat,
                            .MipLevel = 0U,
                            .NativeBytes =
                                texture.NativeBytes.first(baseNativeBytes),
                            .NativeRgba8 =
                                std::span<const uint8_t>(pixels)
                                    .first(baseDecodedBytes),
                        });
            if (baseLevelContentHash && replacementResult.NativeHashAvailable)
                Oot3d::TextureCatalogRuntime::Instance().AssociateMaterial(
                    *baseLevelContentHash, texture.Width, texture.Height, texture.NativeFormat,
                    replacementResult.NativeHash, replacementResult.UseNewHash);
            if (replacementResult.State ==
                    ::Oot3d::Renderer::
                        AzaharTextureResolveState::Ready &&
                replacementResult.Replacement != nullptr &&
                replacementResult.Replacement->Rgba8 != nullptr) {
                replacementKey.ReplacementGeneration = replacementResult.Replacement->Generation;
                const uint64_t replacementBytes =
                    replacementResult.Replacement->Rgba8->size();
                const bool budgetAvailable =
                    mCustomTextureUploadBytesThisFrame == 0U ||
                    (mCustomTextureUploadBytesThisFrame <
                         kCustomTextureUploadBudgetBytes &&
                     replacementBytes <=
                         kCustomTextureUploadBudgetBytes -
                             mCustomTextureUploadBytesThisFrame);
                if (budgetAvailable) {
                    bool inserted = false;
                    TextureRecord* promoted = createTexture(
                        replacementKey,
                        *replacementResult.Replacement->Rgba8,
                        replacementResult.Replacement->Width,
                        replacementResult.Replacement->Height,
                        1U,
                        VK_FORMAT_R8G8B8A8_UNORM,
                        replacementResult.NativeHash, true,
                        &inserted, replacementResult.Replacement->PixelContentHash);
                    if (inserted) {
                        mCustomTextureUploadBytesThisFrame +=
                            replacementBytes;
                    }
                    return promoted;
                }
            }
        }
        TextureRecord* record = createTexture(
            key, pixels, uploadWidth, uploadHeight, mipLevels, imageFormat,
            replacementResult.NativeHash, false);
        record->CustomReplacementPending =
            replacementResult.State ==
                ::Oot3d::Renderer::
                    AzaharTextureResolveState::Pending ||
            replacementResult.State ==
                ::Oot3d::Renderer::
                    AzaharTextureResolveState::Ready;
        record->CustomReplacementReady =
            replacementResult.State ==
            ::Oot3d::Renderer::
                AzaharTextureResolveState::Ready;
        return record;
    } catch (const std::exception& exception) {
        SetNativeError(error, exception.what());
        return nullptr;
    }
}

GfxRenderingAPIVulkan::TextureRecord*
GfxRenderingAPIVulkan::GetOrCreateNativePicaLightingLut(
    const ::Oot3d::Renderer::PicaLightingLutView& lightingLut,
    std::string* error) {
    uint64_t insertedHash = 0U;
    bool inserted = false;
    try {
        if (!lightingLut.Valid()) {
            throw std::runtime_error(
                "native PICA lighting LUT state is invalid");
        }
        auto [found, wasInserted] = mNativePicaLightingLuts.try_emplace(
            lightingLut.ContentHash);
        inserted = wasInserted;
        insertedHash = lightingLut.ContentHash;
        auto& cached = found->second;
        cached.LastUsedFrame = mFrameCounter;
        if (!wasInserted) {
            return &cached.Texture;
        }
        auto& texture = cached.Texture;
        texture.SamplerState.MinFilter =
            GfxNativeTextureFilter::Nearest;
        texture.SamplerState.MagFilter =
            GfxNativeTextureFilter::Nearest;
        texture.SamplerState.WrapS =
            GfxNativeTextureWrap::ClampToEdge;
        texture.SamplerState.WrapT =
            GfxNativeTextureWrap::ClampToEdge;
        texture.SamplerState.MinMipLevel = 0U;
        texture.SamplerState.MaxMipLevel = 0U;
        const auto bytes = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(
                lightingLut.PackedEntries.data()),
            lightingLut.PackedEntries.size() * sizeof(uint32_t));
        CreateNativePicaTextureImage(
            texture, bytes, 256U, 24U, 1U, VK_FORMAT_R32_UINT);
        return &texture;
    } catch (const std::exception& exception) {
        if (inserted) {
            const auto found = mNativePicaLightingLuts.find(insertedHash);
            if (found != mNativePicaLightingLuts.end()) {
                DestroyTexture(found->second.Texture);
                mNativePicaLightingLuts.erase(found);
            }
        }
        SetNativeError(error, exception.what());
        return nullptr;
    }
}

void GfxRenderingAPIVulkan::CreateNativePicaTextureStorage(
    TextureRecord& texture, uint32_t width, uint32_t height, uint32_t mipLevels,
    VkFormat format, bool storageView) {

    texture.Width = width;
    texture.Height = height;
    texture.MipLevels = mipLevels;
    texture.UploadedMipLevels = 0;

    Oot3d::NriPicaTextureImage nriTexture;
    if (mNriPicaTextureImageOwner.Create(width, height, format, mipLevels,
                                         nriTexture, storageView)) {
        texture.Image = nriTexture.Image;
        texture.NriImage = nriTexture.Image;
        texture.View = nriTexture.View;
        texture.NriOwnedImage = true;
        mDiagnostics.RecordNriPicaTextureImage(true);
    } else {
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = 1;
        imageInfo.format = format;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage =
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (storageView) {
            // A derived raw-copy image is materialized on the GPU, so its
            // compute pass needs a storage view of it.
            imageInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        }
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CheckNativeVk(
            vkCreateImage(mDevice, &imageInfo, nullptr, &texture.Image),
            "vkCreateImage(native PICA texture)");
        texture.NriImage = texture.Image;

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(mDevice, texture.Image, &requirements);
        VkMemoryAllocateInfo allocation{
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = FindMemoryType(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CheckNativeVk(vkAllocateMemory(
                          mDevice, &allocation, nullptr, &texture.Memory),
                      "vkAllocateMemory(native PICA texture)");
        CheckNativeVk(
            vkBindImageMemory(
                mDevice, texture.Image, texture.Memory, 0),
            "vkBindImageMemory(native PICA texture)");
    }
    if (!texture.NriOwnedImage) {
        VkImageViewCreateInfo view{
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = texture.Image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = mipLevels;
        view.subresourceRange.layerCount = 1;
        CheckNativeVk(
            vkCreateImageView(mDevice, &view, nullptr, &texture.View),
            "vkCreateImageView(native PICA texture)");
    }
}

void GfxRenderingAPIVulkan::CreateNativePicaTextureImage(
    TextureRecord& texture, std::span<const uint8_t> pixels, uint32_t width,
    uint32_t height, uint32_t mipLevels, VkFormat format, bool storageView) {
    const auto expectedBytes = NativePicaDecodedMipChainSize(width, height, mipLevels);
    if (!expectedBytes.has_value() || pixels.size() != *expectedBytes || pixels.empty())
        throw std::runtime_error("native PICA texture upload size is invalid");
    CreateNativePicaTextureStorage(texture, width, height, mipLevels, format, storageView);
    size_t offset = 0U;
    for (uint32_t level = 0U; level < mipLevels; ++level) {
        const uint32_t mipWidth = std::max(1U, width >> level);
        const uint32_t mipHeight = std::max(1U, height >> level);
        const size_t bytes = static_cast<size_t>(mipWidth) * mipHeight * 4U;
        UploadNativePicaTextureRows(texture, pixels.subspan(offset, bytes),
            mipWidth, mipHeight, level, mipLevels, format, 0U, mipHeight);
        offset += bytes;
    }
    texture.UploadedMipLevels = mipLevels;
    texture.Uploaded = true;
    RecreateSampler(texture);
}

void GfxRenderingAPIVulkan::UploadNativePicaTextureRows(
    TextureRecord& texture, std::span<const uint8_t> pixels,
    uint32_t width, uint32_t height, uint32_t level, uint32_t mipLevels,
    VkFormat format, uint32_t rowOffset, uint32_t rowCount) {
    if (rowCount == 0U || rowOffset > height || rowCount > height - rowOffset ||
        static_cast<uint64_t>(width) * rowCount * 4U != pixels.size())
        throw std::runtime_error("native PICA texture upload region is invalid");
    EndNativePicaRenderPass();
    if (mRenderPassActive) {
        vkCmdEndRenderPass(mCommandBuffers[mCurrentFrame]);
        mRenderPassActive = false;
        mOverlayRenderPassActive = false;
    }
    Oot3d::NriPicaTextureUploadDesc upload;
    upload.FrameIndex = mCurrentFrame;
    upload.FrameId = mFrameCounter;
    upload.Image = texture.Image;
    upload.Format = format;
    upload.Width = width;
    upload.Height = height;
    upload.MipLevel = level;
    upload.MipLevels = mipLevels;
    upload.Pixels = pixels;
    upload.RowOffset = rowOffset;
    upload.RowCount = rowCount;
    if (mNriPicaTextureUploadPass.Execute(upload)) {
        mDiagnostics.RecordNriPicaTextureUpload(
            true, mNriPicaTextureUploadPass.LastUploadedBytes());
        return;
    }
    auto& frame = mFrameResources[mCurrentFrame];
    const VkDeviceSize uploadOffset = (frame.VertexBytesUsed + 15U) & ~VkDeviceSize(15U);
    if (uploadOffset + pixels.size() > frame.VertexBuffer.Size)
        throw std::runtime_error("native PICA texture exceeds the per-frame upload arena");
    std::memcpy(static_cast<uint8_t*>(frame.VertexBuffer.Mapped) + uploadOffset,
                pixels.data(), pixels.size());
    frame.VertexBytesUsed = uploadOffset + pixels.size();

    VkCommandBuffer commandBuffer = mCommandBuffers[mCurrentFrame];
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.oldLayout = rowOffset == 0U ? VK_IMAGE_LAYOUT_UNDEFINED :
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = texture.Image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.baseMipLevel = level;
    toTransfer.subresourceRange.levelCount = 1U;
    toTransfer.subresourceRange.layerCount = 1U;
    toTransfer.srcAccessMask = rowOffset == 0U ? 0U : VK_ACCESS_SHADER_READ_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer,
        rowOffset == 0U ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);
    VkBufferImageCopy copy{};
    copy.bufferOffset = uploadOffset;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel = level;
    copy.imageSubresource.layerCount = 1U;
    copy.imageOffset.y = static_cast<int32_t>(rowOffset);
    copy.imageExtent = {width, rowCount, 1U};
    vkCmdCopyBufferToImage(commandBuffer, frame.VertexBuffer.Buffer, texture.Image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &copy);
    VkImageMemoryBarrier toShaderRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.image = texture.Image;
    toShaderRead.subresourceRange = toTransfer.subresourceRange;
    toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toShaderRead);
}

void GfxRenderingAPIVulkan::RetireNativePicaTexture(TextureRecord& texture) {
    if (texture.Image != VK_NULL_HANDLE || texture.NriImage != VK_NULL_HANDLE ||
        texture.Memory != VK_NULL_HANDLE || texture.Sampler != VK_NULL_HANDLE)
        mFrameResources[mCurrentFrame].RetiredTextures.push_back(std::move(texture));
    texture = {};
}

void GfxRenderingAPIVulkan::RetireNativePicaMaterial(NativePicaMaterialBundle& material) {
    for (auto& image : material.Images) RetireNativePicaTexture(image);
    material = {};
}

GfxRenderingAPIVulkan::NativePicaMaterialBundle*
GfxRenderingAPIVulkan::GetOrCreateNativePicaMaterial(
    const std::shared_ptr<const ::Oot3d::Renderer::AzaharPbrMaterial>& material) {
    auto& entry = mNativePicaMaterials[material->Identity.NativeHash];
    entry.LastUsedFrame = mFrameCounter;
    if (entry.Active.Material && entry.Active.Material->Identity != material->Identity) {
        RetireNativePicaMaterial(entry.Active);
        RetireNativePicaMaterial(entry.Pending);
        entry.FailedGeneration = 0U;
    }
    const auto active = [&]() -> NativePicaMaterialBundle* {
        return entry.Active.Material ? &entry.Active : nullptr;
    };
    if ((entry.Active.Material && entry.Active.Material->Generation == material->Generation) ||
        entry.FailedGeneration == material->Generation) return active();
    const auto sourceAt = [](const auto& source, size_t role) -> const auto& {
        return role < source.Textures.size() ? source.Textures[role] : source.ColorReplacement;
    };
    if (!entry.Pending.Material || entry.Pending.Material->Generation != material->Generation) {
        RetireNativePicaMaterial(entry.Pending);
        entry.Pending.Material = material;
        if (entry.Active.Material) {
            for (size_t role = 0U; role < entry.Pending.Images.size(); ++role) {
                const auto& old = sourceAt(*entry.Active.Material, role);
                const auto& next = sourceAt(*material, role);
                if (old && next && old->PixelContentHash == next->PixelContentHash &&
                    old->Width == next->Width && old->Height == next->Height)
                    entry.Pending.ReusedImages |= static_cast<uint8_t>(1U << role);
            }
        }
    }
    try {
        for (size_t role = 0U; role < entry.Pending.Images.size(); ++role) {
            const auto& source = sourceAt(*material, role);
            if (!source || (entry.Pending.ReusedImages & (1U << role)) != 0U) continue;
            auto& image = entry.Pending.Images[role];
            auto& uploaded = entry.Pending.UploadedRows[role];
            if (uploaded == source->Height) continue;
            const uint64_t rowBytes = static_cast<uint64_t>(source->Width) * 4U;
            if (!source->Rgba8 || rowBytes == 0U || source->Height == 0U ||
                rowBytes * source->Height != source->Rgba8->size())
                throw std::runtime_error("invalid decoded PBR texture dimensions");
            const uint64_t remaining = kCustomTextureUploadBudgetBytes -
                std::min(kCustomTextureUploadBudgetBytes, mCustomTextureUploadBytesThisFrame);
            const auto rows = static_cast<uint32_t>(
                std::min<uint64_t>(source->Height - uploaded, remaining / rowBytes));
            if (rows == 0U) return active();
            if (image.Image == VK_NULL_HANDLE)
                CreateNativePicaTextureStorage(image, source->Width, source->Height,
                    1U, VK_FORMAT_R8G8B8A8_UNORM, false);
            const auto pixels = std::span<const uint8_t>(*source->Rgba8).subspan(
                static_cast<size_t>(uploaded * rowBytes), static_cast<size_t>(rows * rowBytes));
            UploadNativePicaTextureRows(image, pixels, source->Width, source->Height,
                0U, 1U, VK_FORMAT_R8G8B8A8_UNORM, uploaded, rows);
            uploaded += rows;
            mCustomTextureUploadBytesThisFrame += pixels.size();
            if (uploaded != source->Height) return active();
            image.UploadedMipLevels = 1U;
            image.Uploaded = true;
            image.CustomReplacementHash = material->Identity.NativeHash;
            image.CustomReplacementPixelHash = source->PixelContentHash;
            image.CustomReplacementApplied = true;
            image.CustomReplacementReady = true;
        }
        for (size_t role = 0U; role < entry.Pending.Images.size(); ++role) {
            if ((entry.Pending.ReusedImages & (1U << role)) == 0U) continue;
            entry.Pending.Images[role] = std::move(entry.Active.Images[role]);
            entry.Active.Images[role] = {};
        }
        RetireNativePicaMaterial(entry.Active);
        entry.Active = std::move(entry.Pending);
        entry.Pending = {};
        mTemporalAaPass.ResetHistory();
        mTaaOutputValid = false;
        return &entry.Active;
    } catch (const std::exception& exception) {
        RetireNativePicaMaterial(entry.Pending);
        entry.FailedGeneration = material->Generation;
        ::Oot3d::Renderer::AzaharTexturePackRuntime::Instance().ReportMaterialUploadFailure(
            material->Identity.NativeHash, material->Generation,
            fmt::format("PBR material {:016X}: {}", material->Identity.NativeHash, exception.what()));
        return active();
    }
}

void GfxRenderingAPIVulkan::EnsureNativePicaVulkanShaderModules(
    const GfxNativePicaDrawView& draw, NativePicaShaderProgram& shader) {
    if (shader.VertexShader != VK_NULL_HANDLE && shader.FragmentShader != VK_NULL_HANDLE)
        return;
    if (shader.VertexShader != VK_NULL_HANDLE || shader.FragmentShader != VK_NULL_HANDLE)
        throw std::logic_error("partial native PICA Vulkan shader ownership");
    // NRI consumes SPIR-V directly. The combined-sampler program and Vulkan
    // modules belong exclusively to a draw that actually needs the fallback.
    const auto fragment = ResolveNativePicaShaderSpirv(draw.FragmentShaderSource,
        Oot3d::PicaAotShaderStage::Fragment, false, "native_pica_fallback.frag");
    const auto vertex = CreateShaderModuleFromSpirv(shader.NriVertexSpirv);
    VkShaderModule fragmentModule = VK_NULL_HANDLE;
    try {
        fragmentModule = CreateShaderModuleFromSpirv(fragment);
    } catch (...) {
        vkDestroyShaderModule(mDevice, vertex, nullptr);
        throw;
    }
    shader.VertexShader = vertex;
    shader.FragmentShader = fragmentModule;
    ++mNativePicaVulkanShaderPairCreations;
}

void GfxRenderingAPIVulkan::PrepareNativePicaProgram(NativePicaProgramPreparation& preparation) {
    // Owned inputs only; the render thread adopts the results.
    try {
        if (preparation.ResolveVertex)
            preparation.VertexSpirv = ResolveNativePicaShaderSpirv(
                preparation.VertexSource, Oot3d::PicaAotShaderStage::Vertex, true,
                (preparation.Stem + ".vert").c_str());
    } catch (const std::exception& exception) {
        preparation.CompileError = exception.what();
        preparation.ResolveVertex = false;
        preparation.Nri = false;
        preparation.VertexSource = "!";
        return;
    }
    if (!preparation.Nri) return;
    const auto nriVariant = Oot3d::BuildPicaNriFragmentShaderVariant(preparation.FragmentSource);
    preparation.NriApplied = nriVariant.Applied;
    if (!nriVariant.Applied) {
        preparation.NriError = nriVariant.Error;
        return;
    }
    preparation.NriFragmentSource = nriVariant.Source;
    try {
        preparation.NriFragmentSpirv = ResolveNativePicaShaderSpirv(
            nriVariant.Source, Oot3d::PicaAotShaderStage::NriFragment, false,
            (preparation.Stem + "_nri.frag").c_str());
    } catch (const std::exception& exception) {
        preparation.CompileError = exception.what();
    }
}

bool GfxRenderingAPIVulkan::AdoptNativePicaProgram(NativePicaShaderProgram& shader, bool complete) {
    const auto preparation = shader.Preparation;
    if (preparation == nullptr) return true;
    if (preparation->Task != nullptr && !preparation->Task->Done()) {
        if (!complete) return false;
        const auto start = std::chrono::steady_clock::now();
        mPreparationQueue->Complete(*preparation->Task);
        const double milliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        mNativePicaPreparationCounters.CompleteMilliseconds += milliseconds;
        mNativePicaPreparationCounters.MaximumCompleteMilliseconds =
            std::max(mNativePicaPreparationCounters.MaximumCompleteMilliseconds, milliseconds);
    }
    shader.Preparation.reset();
    // A vertex program that cannot be compiled has no draw (as before).
    if (preparation->VertexSource == "!") throw std::runtime_error(preparation->CompileError);
    if (preparation->ResolveVertex) shader.NriVertexSpirv = std::move(preparation->VertexSpirv);
    if (!preparation->Nri) return true;
    if (!preparation->NriApplied) {
        SPDLOG_WARN("NRI PICA shader contract unavailable for {}: {}",
                    preparation->Stem, preparation->NriError);
        return true;
    }
    shader.NriFragmentSource = Oot3d::IdentifyPicaAotShaderSource(preparation->NriFragmentSource);
    mPicaEffectiveShaderInventory.Observe(
        Oot3d::PicaAotShaderStage::NriFragment, preparation->NriFragmentSource,
        preparation->DescriptorSchemaVersion, preparation->CanonicalPipelineId,
        preparation->FragmentShaderKey, preparation->SettingsRevision);
    if (!preparation->CompileError.empty()) {
        if (mPicaAotShaderStrict) throw std::runtime_error(preparation->CompileError);
        SPDLOG_WARN("NRI PICA shader contract compile failed for {}: {}",
                    preparation->Stem, preparation->CompileError);
        return true;
    }
    shader.NriFragmentSpirv = std::move(preparation->NriFragmentSpirv);
    shader.NriDescriptorContract = true;
    return true;
}

Renderer3ds::PicaDevicePipelineRecord& GfxRenderingAPIVulkan::GetOrCreateNativePicaPipeline(
    const GfxNativePicaDrawView& draw,
    NativePicaShaderProgram& shader, bool writesReactiveMask,
    Oot3d::PicaShaderDomain domain,
    Oot3d::PicaShaderInstrumentationFeature requestedFeatures,
    Oot3d::PicaShaderInstrumentationFeature appliedFeatures,
    bool recordInventory, bool outlineOcclusionOnly, bool requireVulkan,
    NativePicaPipelineAcquisition acquisition, bool* pending) {
    if (pending != nullptr) *pending = false;
    if (recordInventory && mPicaPipelineInventory.Enabled()) {
        auto entry = Oot3d::DescribePicaGraphicsPipelineDraw(draw);
        entry.DescriptorSchemaVersion =
            draw.CanonicalDescriptorSchemaVersion;
        entry.Domain = domain == Oot3d::PicaShaderDomain::Canonical
            ? Oot3d::PicaGraphicsPipelineDomain::Canonical
            : Oot3d::PicaGraphicsPipelineDomain::Instrumented;
        entry.VertexShaderKey = draw.VertexShaderKey;
        entry.FragmentShaderKey = draw.FragmentShaderKey;
        entry.VertexSource = shader.VertexSource;
        entry.FragmentSource = shader.FragmentSource;
        entry.NriFragmentSource = shader.NriFragmentSource;
        entry.NriFragmentAvailable = shader.NriDescriptorContract;
        entry.RequestedFeatures = requestedFeatures;
        entry.AppliedFeatures = appliedFeatures;
        entry.AttachmentRequirementsKey =
            mFramePicaAttachmentRequirements.Key();
        entry.SampleCount =
            static_cast<uint8_t>(mNativePicaSampleCount);
        entry.WritesReactiveMask = writesReactiveMask;
        entry.OutlineOcclusionOnly = outlineOcclusionOnly;
        entry.ShaderOutputs = shader.FragmentOutputs;
        mPicaPipelineInventory.Observe(
            std::move(entry), draw.CanonicalPipelineId,
            mFrameGraphicsSettingsRevision);
    }
    auto resolvedState = Oot3d::BuildPicaNriPipelineState(
        draw, shader.FragmentOutputs, mFramePicaAttachmentRequirements,
        mNativePicaSampleCount, mDepthFormat, writesReactiveMask, outlineOcclusionOnly);
    // The superset program declares every guide output; the attachments its
    // specialized program does not declare stay untouched, as they do there.
    for (size_t location = 1U; location < resolvedState.Colors.size(); ++location)
        if ((shader.DeclaredOutputs & (1U << location)) == 0U) resolvedState.Colors[location].colorWriteMask = 0U;
    const auto sourceIdentity = [](const Oot3d::PicaAotShaderSourceIdentity& value) {
        return std::array<uint64_t, 3>{value.Id, value.SecondaryHash, value.Size};
    };
    const Renderer3ds::PicaPipelineProgramIdentity programs{
        sourceIdentity(shader.VertexSource), sourceIdentity(shader.FragmentSource),
        sourceIdentity(shader.NriFragmentSource), shader.NriDescriptorContract};
    auto key = Renderer3ds::BuildPicaPipelineIdentity(
        programs, draw, resolvedState, mPicaDynamicRenderingScope.Available());
    auto [found, inserted] = mNativePicaPipelines.try_emplace(std::move(key));
    auto& record = found->second;
    if (inserted) record.Id = {mNextPicaPipelineId++};
    if (record.Vulkan != VK_NULL_HANDLE ||
        (!requireVulkan && mNriPicaPipelineBridge.OwnedPipelineReady(record.Id))) {
        return record;
    }
    if (!requireVulkan && record.NriPreparationAttempted) {
        // A creation of this record already started; Failed falls back below.
        const auto state = acquisition == NativePicaPipelineAcquisition::Background
            ? mNriPicaPipelineBridge.PollOwnedPipeline(record.Id)
            : mNriPicaPipelineBridge.CompleteOwnedPipeline(record.Id);
        if (state == Oot3d::NriPicaPipelineRequest::Ready) return record;
        if (state == Oot3d::NriPicaPipelineRequest::Pending) {
            if (pending != nullptr) *pending = true;
            return record;
        }
        if (acquisition == NativePicaPipelineAcquisition::Libraries) return record;
    }

    std::vector<VkVertexInputBindingDescription> bindings;
    bindings.reserve(draw.VertexBindings.size());
    for (const auto& binding : draw.VertexBindings) {
        if (binding.ByteStride == 0 || binding.Bytes.empty()) {
            throw std::runtime_error("native PICA vertex binding is empty");
        }
        bindings.push_back({binding.Binding, binding.ByteStride,
                            binding.PerInstance
                                ? VK_VERTEX_INPUT_RATE_INSTANCE
                                : VK_VERTEX_INPUT_RATE_VERTEX});
    }
    std::vector<VkVertexInputAttributeDescription> attributes;
    attributes.reserve(draw.VertexAttributes.size());
    for (const auto& attribute : draw.VertexAttributes) {
        const VkFormat format = ToNativeVkVertexFormat(
            attribute.Format, attribute.ComponentCount);
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(mPhysicalDevice, format,
                                            &properties);
        if ((properties.bufferFeatures &
             VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) == 0) {
            throw std::runtime_error(
                "native PICA vertex format is not supported by this GPU");
        }
        attributes.push_back({attribute.Location, attribute.Binding, format,
                              attribute.ByteOffset});
    }
    std::vector<Oot3d::PicaNriSourceVertexBinding> nriSourceBindings;
    nriSourceBindings.reserve(draw.VertexBindings.size());
    for (const auto& binding : draw.VertexBindings) {
        nriSourceBindings.push_back(
            {binding.Binding, binding.ByteStride, binding.PerInstance});
    }
    std::vector<Oot3d::PicaNriSourceVertexAttribute> nriSourceAttributes;
    nriSourceAttributes.reserve(draw.VertexAttributes.size());
    for (const auto& attribute : draw.VertexAttributes) {
        nriSourceAttributes.push_back({
            attribute.Location, attribute.Binding,
            static_cast<Oot3d::PicaNriVertexScalar>(attribute.Format),
            attribute.ComponentCount, attribute.ByteOffset});
    }
    const auto nriVertexLayout = Oot3d::BuildPicaNriVertexInputLayout(
        nriSourceBindings, nriSourceAttributes);
    if (!nriVertexLayout.Valid())
        throw std::runtime_error(nriVertexLayout.Error);
    std::vector<VkVertexInputBindingDescription> nriBindings;
    nriBindings.reserve(nriVertexLayout.Bindings.size());
    for (const auto& binding : nriVertexLayout.Bindings) {
        nriBindings.push_back({
            binding.Binding, binding.ByteStride,
            binding.PerInstance ? VK_VERTEX_INPUT_RATE_INSTANCE
                                : VK_VERTEX_INPUT_RATE_VERTEX});
    }
    std::vector<VkVertexInputAttributeDescription> nriAttributes;
    nriAttributes.reserve(nriVertexLayout.Attributes.size());
    for (const auto& attribute : nriVertexLayout.Attributes) {
        nriAttributes.push_back({
            attribute.Location, attribute.Binding,
            VK_FORMAT_R32G32B32A32_SFLOAT, attribute.ByteOffset});
    }

    if (!record.NriPreparationAttempted && shader.NriDescriptorContract &&
        mNriPicaPipelineBridge.OwnedDrawsEnabled()) {
        record.NriPreparationAttempted = true;
        auto nriPipeline = resolvedState;
        nriPipeline.VertexSpirv = shader.NriVertexSpirv;
        nriPipeline.FragmentSpirv = shader.NriFragmentSpirv;
        nriPipeline.VertexBindings = nriBindings;
        nriPipeline.VertexAttributes = nriAttributes;
        if (acquisition == NativePicaPipelineAcquisition::Background && mPreparationQueue) {
            const auto state = mNriPicaPipelineBridge.RequestOwnedPipeline(
                record.Id, nriPipeline, Oot3d::NriPicaPipelineFactory::Default, *mPreparationQueue);
            if (state == Oot3d::NriPicaPipelineRequest::Pending) {
                if (pending != nullptr) *pending = true;
                return record;
            }
            if (state == Oot3d::NriPicaPipelineRequest::Failed)
                SPDLOG_WARN(
                    "NRI PICA pipeline creation failed for shader {}:{}",
                    draw.VertexShaderKey, draw.FragmentShaderKey);
        } else if (!mNriPicaPipelineBridge.CreateOwnedPipeline(
                       record.Id, nriPipeline,
                       acquisition == NativePicaPipelineAcquisition::Libraries
                           ? Oot3d::NriPicaPipelineFactory::Libraries
                           : Oot3d::NriPicaPipelineFactory::Default)) {
            SPDLOG_WARN(
                "NRI PICA pipeline creation failed for shader {}:{}",
                draw.VertexShaderKey, draw.FragmentShaderKey);
        }
    }
    if (!requireVulkan && mNriPicaPipelineBridge.OwnedPipelineReady(record.Id))
        return record;
    // An always-available program never takes the compiling Vulkan fallback.
    if (acquisition == NativePicaPipelineAcquisition::Libraries) return record;

    EnsureNativePicaVulkanShaderModules(draw, shader);
    const VkPipelineShaderStageCreateInfo stages[] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, shader.VertexShader, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, shader.FragmentShader, "main", nullptr},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount =
        static_cast<uint32_t>(bindings.size());
    vertexInput.pVertexBindingDescriptions = bindings.data();
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = ToNativeVkTopology(draw.Topology);
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.lineWidth = 1.0F;
    rasterization.cullMode = resolvedState.CullMode;
    rasterization.frontFace = resolvedState.FrontFace;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = resolvedState.Samples;
    multisample.alphaToCoverageEnable = resolvedState.AlphaToCoverage;
    VkPipelineDepthStencilStateCreateInfo depthStencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = resolvedState.DepthTest;
    depthStencil.depthWriteEnable = resolvedState.DepthWrite;
    depthStencil.depthCompareOp = resolvedState.DepthCompare;
    depthStencil.stencilTestEnable = resolvedState.StencilTest;
    depthStencil.front = resolvedState.FrontStencil;
    depthStencil.back = resolvedState.BackStencil;
    const auto& colorAttachments = resolvedState.Colors;
    const uint32_t colorAttachmentCount = resolvedState.ColorAttachmentCount;
    VkPipelineColorBlendStateCreateInfo colorBlend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.logicOpEnable = resolvedState.LogicOpEnabled;
    colorBlend.logicOp = resolvedState.LogicOp;
    colorBlend.attachmentCount = colorAttachmentCount;
    colorBlend.pAttachments = colorAttachments.data();
    const VkDynamicState dynamicStates[]{VK_DYNAMIC_STATE_VIEWPORT,
                                         VK_DYNAMIC_STATE_SCISSOR,
                                         VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount =
        static_cast<uint32_t>(std::size(dynamicStates));
    dynamic.pDynamicStates = dynamicStates;
    VkGraphicsPipelineCreateInfo pipelineInfo{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = static_cast<uint32_t>(std::size(stages));
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = mNativePicaPipelineLayout;
    const auto& renderingColorFormats = resolvedState.ColorFormats;
    VkPipelineRenderingCreateInfoKHR rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    if (mPicaDynamicRenderingScope.Available()) {
        rendering.colorAttachmentCount = colorAttachmentCount;
        rendering.pColorAttachmentFormats =
            renderingColorFormats.data();
        rendering.depthAttachmentFormat = mDepthFormat;
        if (mDepthFormat == VK_FORMAT_D16_UNORM_S8_UINT ||
            mDepthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
            mDepthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT)
            rendering.stencilAttachmentFormat = mDepthFormat;
        pipelineInfo.pNext = &rendering;
        pipelineInfo.renderPass = VK_NULL_HANDLE;
    } else {
        pipelineInfo.renderPass =
            mFramePicaAttachmentRequirements.NativeColorOnly()
                ? mNativePicaCanonicalRenderPass
                : mNativePicaRenderPass;
    }
    VkPipeline pipeline = VK_NULL_HANDLE;
    CheckNativeVk(vkCreateGraphicsPipelines(mDevice, mPipelineCache, 1,
                                            &pipelineInfo, nullptr, &pipeline),
                  "vkCreateGraphicsPipelines(native PICA)");
    record.Vulkan = pipeline;
    ++mNativePicaVulkanPipelineCreations;
    return record;
}

void GfxRenderingAPIVulkan::PrewarmNativePicaPipelines() {
    if (!mPicaPipelinePrewarmEnabled ||
        !mPicaPipelineManifest.Loaded() ||
        !mPicaAotShaderPack.Loaded()) {
        return;
    }
    const uint8_t sampleCount =
        static_cast<uint8_t>(mNativePicaSampleCount);
    const auto activeFeatures = Oot3d::ResolvePicaShaderProfileFeatures(
        mFrameGraphicsSettings.Effects, mTemporalMotionEnabled,
        Oot3d::PicaExtensionPassEnabled(
            mPicaExtensionGraph,
            Oot3d::PicaExtensionPass::DirectionalShadowLighting));
    const uint64_t profile =
        static_cast<uint64_t>(mFramePicaAttachmentRequirements.Key()) |
        (static_cast<uint64_t>(sampleCount) << 8U) |
        (static_cast<uint64_t>(activeFeatures) << 16U) |
        (mNativeFidelityProfile ? 1ULL << 48U : 0U);
    if (!mPicaPipelinePrewarmedProfiles.insert(profile).second) {
        return;
    }

    const std::array<uint8_t, 1> dummyVertexBytes{};
    for (const auto& entry : mPicaPipelineManifest.Entries()) {
        if (entry.AttachmentRequirementsKey !=
                mFramePicaAttachmentRequirements.Key() ||
            entry.SampleCount != sampleCount ||
            entry.DescriptorSchemaVersion !=
                mPicaAotShaderPack.DescriptorSchemaVersion() ||
            !entry.MatchesPrewarmProfile(
                activeFeatures, mNativeFidelityProfile)) {
            continue;
        }

        auto& shaderCache =
            entry.Domain == Oot3d::PicaGraphicsPipelineDomain::Canonical
                ? mCanonicalNativePicaShaders
                : mInstrumentedNativePicaShaders;
        const auto shaderKey = Renderer3ds::BuildPicaShaderModuleIdentity(
            {entry.VertexSource.Id, entry.VertexSource.SecondaryHash, entry.VertexSource.Size},
            {entry.FragmentSource.Id, entry.FragmentSource.SecondaryHash, entry.FragmentSource.Size},
            entry.DescriptorSchemaVersion, mNriPicaPipelineBridge.Available(), entry.ShaderOutputs);
        if (mNriPicaPipelineBridge.Available() &&
            !entry.NriFragmentAvailable) {
            ++mPicaPipelinePrewarmSkipped;
            continue;
        }
        auto shaderIt = shaderCache.find(shaderKey);
        if (shaderIt != shaderCache.end() &&
            (shaderIt->second.VertexSource != entry.VertexSource ||
             shaderIt->second.FragmentSource != entry.FragmentSource ||
             shaderIt->second.NriDescriptorContract !=
                 (entry.NriFragmentAvailable &&
                  mNriPicaPipelineBridge.Available()) ||
             (shaderIt->second.NriDescriptorContract &&
              shaderIt->second.NriFragmentSource !=
                  entry.NriFragmentSource) ||
             shaderIt->second.FragmentOutputs != entry.ShaderOutputs)) {
            ++mPicaPipelinePrewarmSkipped;
            continue;
        }
        if (shaderIt == shaderCache.end()) {
            const auto vertexSpirv = mPicaAotShaderPack.Find(
                Oot3d::PicaAotShaderStage::Vertex, entry.VertexSource);
            const auto fragmentSpirv = mPicaAotShaderPack.Find(
                Oot3d::PicaAotShaderStage::Fragment,
                entry.FragmentSource);
            if (vertexSpirv.empty() || fragmentSpirv.empty()) {
                ++mPicaPipelinePrewarmSkipped;
                continue;
            }
            std::span<const uint32_t> nriFragmentSpirv;
            if (entry.NriFragmentAvailable &&
                mNriPicaPipelineBridge.Available()) {
                nriFragmentSpirv = mPicaAotShaderPack.Find(
                    Oot3d::PicaAotShaderStage::NriFragment,
                    entry.NriFragmentSource);
                if (nriFragmentSpirv.empty()) {
                    ++mPicaPipelinePrewarmSkipped;
                    continue;
                }
            }

            NativePicaShaderProgram shader;
            shader.VertexSource = entry.VertexSource;
            shader.FragmentSource = entry.FragmentSource;
            shader.NriFragmentSource = entry.NriFragmentSource;
            shader.FragmentOutputs = entry.ShaderOutputs;
            try {
                shader.NriVertexSpirv.assign(vertexSpirv.begin(),
                                             vertexSpirv.end());
                shader.VertexShader =
                    CreateShaderModuleFromSpirv(vertexSpirv);
                shader.FragmentShader =
                    CreateShaderModuleFromSpirv(fragmentSpirv);
                ++mNativePicaVulkanShaderPairCreations;
                if (!nriFragmentSpirv.empty()) {
                    shader.NriFragmentSpirv.assign(
                        nriFragmentSpirv.begin(), nriFragmentSpirv.end());
                    shader.NriDescriptorContract = true;
                }
                shaderIt = shaderCache.emplace(shaderKey,
                                               std::move(shader)).first;
            } catch (const std::exception& exception) {
                if (shader.FragmentShader != VK_NULL_HANDLE) {
                    vkDestroyShaderModule(mDevice, shader.FragmentShader,
                                          nullptr);
                }
                if (shader.VertexShader != VK_NULL_HANDLE) {
                    vkDestroyShaderModule(mDevice, shader.VertexShader,
                                          nullptr);
                }
                ++mPicaPipelinePrewarmSkipped;
                SPDLOG_WARN(
                    "Native PICA pipeline prewarm shader creation failed: {}",
                    exception.what());
                continue;
            }
        }

        std::vector<GfxNativePicaVertexBindingView> bindings;
        bindings.reserve(entry.VertexBindings.size());
        for (const auto& source : entry.VertexBindings) {
            bindings.push_back({
                source.Binding, source.ByteStride, source.PerInstance,
                dummyVertexBytes});
        }
        std::vector<GfxNativePicaVertexAttributeView> attributes;
        attributes.reserve(entry.VertexAttributes.size());
        for (const auto& source : entry.VertexAttributes) {
            attributes.push_back({
                source.Location, source.Binding, source.Format,
                source.ComponentCount, source.ByteOffset});
        }
        GfxNativePicaDrawView draw;
        draw.CanonicalDescriptorSchemaVersion =
            entry.DescriptorSchemaVersion;
        if (!entry.CanonicalPipelineIds.empty()) {
            draw.CanonicalPipelineId =
                *entry.CanonicalPipelineIds.begin();
        }
        draw.VertexShaderKey = entry.VertexShaderKey;
        draw.FragmentShaderKey = entry.FragmentShaderKey;
        draw.VertexBindings = bindings;
        draw.VertexAttributes = attributes;
        draw.Topology = entry.Topology;
        draw.CullMode = entry.CullMode;
        draw.FramebufferFlipped = entry.FramebufferFlipped;
        draw.ColorWriteMask = entry.ColorWriteMask;
        draw.FragmentOperationMode = entry.FragmentOperationMode;
        draw.LogicOperation = entry.LogicOperation;
        draw.Blend.Enabled = entry.Blend.Enabled;
        draw.Blend.EquationRgb = entry.Blend.EquationRgb;
        draw.Blend.EquationAlpha = entry.Blend.EquationAlpha;
        draw.Blend.SourceRgb = entry.Blend.SourceRgb;
        draw.Blend.DestRgb = entry.Blend.DestRgb;
        draw.Blend.SourceAlpha = entry.Blend.SourceAlpha;
        draw.Blend.DestAlpha = entry.Blend.DestAlpha;
        draw.AlphaTestEnabled = entry.AlphaTestEnabled;
        draw.DepthTestEnabled = entry.DepthTestEnabled;
        draw.DepthWriteEnabled = entry.DepthWriteEnabled;
        draw.DepthCompare = entry.DepthCompare;
        draw.Stencil.Enabled = entry.Stencil.Enabled;
        draw.Stencil.Compare = entry.Stencil.Compare;
        draw.Stencil.Reference = entry.Stencil.Reference;
        draw.Stencil.CompareMask = entry.Stencil.CompareMask;
        draw.Stencil.WriteMask = entry.Stencil.WriteMask;
        draw.Stencil.Fail = entry.Stencil.Fail;
        draw.Stencil.DepthFail = entry.Stencil.DepthFail;
        draw.Stencil.Pass = entry.Stencil.Pass;

        const size_t pipelineCount = mNativePicaPipelines.size();
        try {
            GetOrCreateNativePicaPipeline(
                draw, shaderIt->second, entry.WritesReactiveMask,
                entry.Domain ==
                        Oot3d::PicaGraphicsPipelineDomain::Canonical
                    ? Oot3d::PicaShaderDomain::Canonical
                    : Oot3d::PicaShaderDomain::Instrumented,
                entry.RequestedFeatures, entry.AppliedFeatures,
                false, entry.OutlineOcclusionOnly);
            if (mNativePicaPipelines.size() == pipelineCount) {
                ++mPicaPipelinePrewarmReused;
            } else {
                ++mPicaPipelinePrewarmCreated;
            }
        } catch (const std::exception& exception) {
            ++mPicaPipelinePrewarmSkipped;
            SPDLOG_WARN("Native PICA pipeline prewarm failed: {}",
                        exception.what());
        }
    }
}

bool PublishNativePicaGrassSurface(
    const GfxNativePicaDrawView& draw, uint64_t frameId,
    std::unordered_map<uint64_t, uint32_t>& occurrences,
    const Oot3d::InteractiveGrassSettings& settings,
    bool worldSurface, bool fragmentUsesTexture0,
    bool perspectiveProjection,
    Oot3d::GrassGeometryRegistry& geometryRegistry);

bool GfxRenderingAPIVulkan::PublishPicaCompositionSequence(
    const ::Fast::Renderer3ds::PicaCompositionSequenceView& sequence,
    std::string* error) {
    try {
        if (!mFrameActive) {
            throw std::runtime_error(
                "native PICA composition sequence published outside an active Vulkan frame");
        }
        if (mNativePicaCompositionSchedule.Valid()) {
            const auto previous = mNativePicaCompositionSchedule.Stats();
            if (previous.ConsumedDrawCount != previous.DrawCount) {
                throw std::runtime_error(
                    "previous native PICA composition sequence was not fully consumed");
            }
            if (mNativePicaCompositionSchedule.SequenceId() ==
                sequence.SequenceId) {
                throw std::runtime_error(
                    "native PICA composition sequence identity was reused");
            }
        }

        Oot3d::PicaCompositionSchedule compiled;
        std::string compileError;
        if (!compiled.Compile(sequence, &compileError)) {
            throw std::runtime_error(
                "native PICA composition schedule failed: " +
                compileError);
        }
        mNativePicaCompositionSchedule = std::move(compiled);
        // Prepared casters belong to exactly one published sequence.
        mPreparedShadowCasterFrame.BeginFrame(mCurrentFrame, mFrameCounter);
        mPreparedShadowCasters.clear();
        mDiagnostics.RecordPicaCompositionSchedule(
            mNativePicaCompositionSchedule.Stats());
        return true;
    } catch (const std::exception& exception) {
        SetNativeError(error, exception.what());
        return false;
    }
}

void GfxRenderingAPIVulkan::ExecuteNativePicaBeforeDrawAnchors(
    NativePicaRenderTarget& target, const GfxNativePicaDrawView& draw,
    double& shadowMilliseconds, double& grassMilliseconds) {
    if (mNativePicaCompositionSchedule.Purpose() ==
        Renderer3ds::PicaCompositionPurpose::NativeMemory) return;
    const auto reference = Renderer3ds::BuildPicaCompositionDrawReference(draw);
    const bool timed = mDiagnostics.Enabled();
    using Clock = std::chrono::steady_clock;
    auto begin = timed ? Clock::now() : Clock::time_point{};
    if (const auto* anchor = Oot3d::FindPicaEffectPassAnchor(
            mNativePicaCompositionSchedule, mDirectionalShadowSchedulePlan, reference.Target);
        anchor != nullptr && !anchor->AtTargetEnd && anchor->BeforeSubmissionId == draw.SubmissionId)
        TryRenderDirectionalShadowMap(target, mFrameGraphicsSettings, *anchor);
    if (const auto* anchor = Oot3d::FindPicaEffectPassAnchor(
            mNativePicaCompositionSchedule, mPointShadowSchedulePlan, reference.Target);
        anchor != nullptr && !anchor->AtTargetEnd && anchor->BeforeSubmissionId == draw.SubmissionId)
        TryRenderPointShadowMaps(target, mFrameGraphicsSettings, *anchor);
    if (timed) {
        const auto now = Clock::now();
        shadowMilliseconds += std::chrono::duration<double, std::milli>(now - begin).count();
        begin = now;
    }
    const auto* grassAnchor = mInteractiveGrassProviderPlan.Valid()
        ? mNativePicaCompositionSchedule.FindPublishedGeometryAnchor(reference.Target) : nullptr;
    if (grassAnchor != nullptr && !grassAnchor->AtTargetEnd &&
        grassAnchor->BeforeSubmissionId == draw.SubmissionId)
        TryRenderInteractiveGrass(target, mFrameGraphicsSettings,
            Oot3d::EffectGeometryProviderInvocationKind::DeclaredBoundary);
    if (timed)
        grassMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    const auto& effects = mFrameGraphicsSettings.Effects;
    // Scene stages, same-extent anti-aliasing included: it resolves the scene
    // here so the UI that follows is drawn over the result, never through it.
    const bool sceneEffects = effects.AmbientOcclusion != Oot3d::AmbientOcclusionMode::Off ||
        effects.Reflections != Oot3d::ReflectionMode::Off ||
        effects.GlobalIllumination.Mode != Oot3d::GlobalIlluminationMode::Off ||
        (effects.Toon != Oot3d::ToonMode::Off && effects.ToonStyle.OutlineEnabled) ||
        mSceneAntiAliasing;
    // Presentation stages (AA, upscalers) read the display's world surface.
    // When native UI reaches the target before its transfer, that surface is
    // proven only by the scene completion recorded at this boundary.
    if (!mNativeFidelityProfile && (sceneEffects || mPresentationWorldSurface) &&
        draw.CompositionDomain == Renderer3ds::PicaCompositionDomain::Ui) {
        const auto* beforeUi = mNativePicaCompositionSchedule.FindAnchorBeforeDraw(
            Oot3d::EffectStage::BeforeUi, reference.Target, draw.SubmissionId);
        if (beforeUi != nullptr) {
            GpuTextureCopySourceImport composed;
            if (sceneEffects) {
                std::string error;
                if (!ComposeNativePicaSceneForCopy(target, draw.SubmissionId, composed, &error, beforeUi))
                    throw std::runtime_error(error);
            }
            if (composed.Image == VK_NULL_HANDLE || composed.Image == target.ColorImage) {
                // No scene stage was composed here: the target's current
                // content is already the scene as of this boundary, before UI.
                if (mPresentationWorldSurface)
                    target.SceneEffectsResolvedSerial = target.ContentWriteSerial;
            } else {
                if (!mNriPicaDisplayCopyPass.Execute({
                        mCurrentFrame, mFrameCounter, composed.Image, target.ColorImage,
                        composed.Format, {target.Width, target.Height, target.Width, target.Height},
                        composed.ShaderRead, true, true}))
                    throw std::runtime_error("native pre-UI scene commit failed");
                mDiagnostics.RecordNriPicaDisplayCopy(
                    true, mNriPicaDisplayCopyPass.LastBarrierCount());
                target.ContentWriteSerial = ++mGpuTextureCopyWriteSerial;
                target.SceneEffectsResolvedSerial = target.ContentWriteSerial;
            }
            // An upscaler changes the extent, so it cannot resolve here: keep
            // the scene this boundary completed, which the display transfer
            // transfers beside the display for the upscaler to reconstruct.
            if (mPresentationUpscaler && target.SceneEffectsResolvedSerial != 0U &&
                target.SceneEffectsResolvedSerial == target.ContentWriteSerial) {
                EndNativePicaRenderPass();
                if (mRenderPassActive) {
                    vkCmdEndRenderPass(mCommandBuffers[mCurrentFrame]);
                    mRenderPassActive = false;
                    mOverlayRenderPassActive = false;
                }
                auto& snapshot = GetOrCreateNativePicaDisplayImage(
                    mNativePicaSceneBeforeUiTargets, target.Key.RenderTargetNamespace,
                    target.Key.ColorPhysicalAddress, target.Width, target.Height);
                if (!mNriPicaDisplayCopyPass.Execute({
                        mCurrentFrame, mFrameCounter, target.ColorImage, snapshot.Image,
                        VK_FORMAT_R8G8B8A8_UNORM, {target.Width, target.Height, target.Width, target.Height},
                        false, snapshot.Initialized}))
                    throw std::runtime_error("native scene before UI snapshot failed");
                snapshot.Initialized = true;
                target.SceneBeforeUiSerial = target.ContentWriteSerial;
                mDiagnostics.RecordSceneBeforeUi(true, false);
            }
        }
    }
}

GfxRenderingAPIVulkan::NativePicaGeometryPlacement GfxRenderingAPIVulkan::PlaceNativePicaGeometry(
    const Oot3d::PicaGeometryResolveResult& resolution, Oot3dVulkanNativePicaCpuTimings* cpuTimings) {
    const auto& geometry = *resolution.Geometry;
    auto& frame = mFrameResources[mCurrentFrame];
    if (resolution.Cacheable) {
        auto& cached = mNativePicaGeometryBuffers[geometry.Identity];
        auto& buffer = cached.Buffers[mCurrentFrame];
        const VkDeviceSize payloadSize = static_cast<VkDeviceSize>(geometry.Payload.size());
        if (buffer.Buffer == VK_NULL_HANDLE || buffer.Size < payloadSize) {
            if (buffer.Buffer != VK_NULL_HANDLE) {
                mNriInterop.ForgetBuffer(buffer.Buffer);
            }
            DestroyBuffer(buffer);
            buffer = CreateBuffer(payloadSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
            cached.ContentVersions[mCurrentFrame] = 0U;
            cached.StructuralSignatures[mCurrentFrame] = 0U;
        }
        if (cached.ContentVersions[mCurrentFrame] != geometry.ContentVersion ||
            cached.StructuralSignatures[mCurrentFrame] != geometry.StructuralSignature) {
            std::memcpy(buffer.Mapped, geometry.Payload.data(), geometry.Payload.size());
            if (cpuTimings != nullptr) {
                cpuTimings->GeometryPersistentUploadBytes += geometry.Payload.size();
            }
            cached.ContentVersions[mCurrentFrame] = geometry.ContentVersion;
            cached.StructuralSignatures[mCurrentFrame] = geometry.StructuralSignature;
        }
        cached.LastUsedFrame = mFrameCounter;
        if (cpuTimings != nullptr) {
            cpuTimings->GeometryPersistentDraws = 1U;
        }
        return { buffer.Buffer, buffer.Size, static_cast<uint8_t*>(buffer.Mapped), 0U, true };
    }
    const VkDeviceSize baseOffset = (frame.VertexBytesUsed + 15U) & ~VkDeviceSize(15U);
    if (baseOffset + geometry.Payload.size() > frame.VertexBuffer.Size) {
        throw std::runtime_error("native PICA transient geometry arena exhausted");
    }
    std::memcpy(static_cast<uint8_t*>(frame.VertexBuffer.Mapped) + baseOffset, geometry.Payload.data(),
                geometry.Payload.size());
    frame.VertexBytesUsed = baseOffset + geometry.Payload.size();
    if (cpuTimings != nullptr) {
        cpuTimings->GeometryTransientUploadBytes += geometry.Payload.size();
        cpuTimings->GeometryTransientDraws = 1U;
    }
    return { frame.VertexBuffer.Buffer, frame.VertexBuffer.Size, static_cast<uint8_t*>(frame.VertexBuffer.Mapped),
             baseOffset, false };
}

bool GfxRenderingAPIVulkan::WantsPicaSequenceDrawPreparation(
    const Renderer3ds::PicaCompositionDrawReference& draw) const {
    // The directional and point shadow maps run at the opaque boundary of
    // this sample, so they need its opaque world before native order reaches it.
    const bool directionalMap =
        mDirectionalShadowMapBarrierPlan.Valid() && mDirectionalShadowSchedulePlan.Valid() &&
        Oot3d::PicaExtensionPassEnabled(mPicaExtensionGraph, Oot3d::PicaExtensionPass::DirectionalShadowLighting);
    const bool pointMaps =
        mPointShadowMapBarrierPlan.Valid() && mPointShadowSchedulePlan.Valid() &&
        Oot3d::PicaExtensionPassEnabled(mPicaExtensionGraph, Oot3d::PicaExtensionPass::PointShadowLighting);
    return mFrameActive && mNativePicaCompositionSchedule.Valid() &&
           mNativePicaCompositionSchedule.Purpose() == Renderer3ds::PicaCompositionPurpose::Presentation &&
           (directionalMap || pointMaps) &&
           mNriPicaPipelineBridge.OwnedDrawsEnabled() &&
           draw.Domain == Renderer3ds::PicaCompositionDomain::Scene &&
           draw.Composition.Layer == Renderer3ds::PicaCompositionLayer::OpaqueWorld &&
           !draw.Composition.NativeProjectedShadow;
}

bool GfxRenderingAPIVulkan::PreparePicaSequenceDraw(
    const GfxNativePicaDrawView& draw, std::string* error) {
    try {
        if (!mFrameActive || !mNativePicaCompositionSchedule.Valid() || !mPreparedShadowCasterFrame.Active() ||
            mPreparedShadowCasterFrame.FrameId() != mFrameCounter) {
            throw std::runtime_error("native PICA draw prepared outside its published sequence");
        }
        if (!WantsPicaSequenceDrawPreparation(Renderer3ds::BuildPicaCompositionDrawReference(draw))) {
            return true;
        }
        // Caster admission: depth-writing perspective geometry with its
        // captured view; an alpha-tested caster also needs its alpha test.
        Oot3d::DirectionalShadowMatrix viewToWorld;
        const bool admitted =
            AdmitsShadowCasterGeometry(draw) &&
            draw.Topology != GfxNativePicaTopology::TriangleFan && !draw.VertexShaderSource.empty() &&
            !draw.FragmentShaderSource.empty() && draw.VertexCount != 0U && !draw.VertexBindings.empty() &&
            !draw.VertexAttributes.empty() && draw.VertexUniformBytes.size() >= Oot3d::kPicaPackedVertexFloatOffset &&
            Oot3d::UsesPicaPerspectiveProjection(draw.VertexUniformBytes) && draw.LightBinding.WorldToView &&
            Oot3d::InvertDirectionalShadowMatrix(*draw.LightBinding.WorldToView, viewToWorld);
        if (!admitted) {
            mDiagnostics.RecordNriDirectionalShadowPreparation(false, false);
            return true;
        }

        std::vector<Oot3d::PicaNriSourceVertexStream> streams;
        std::vector<Oot3d::PicaNriSourceVertexBinding> sourceBindings;
        std::vector<Oot3d::PicaNriSourceVertexAttribute> attributes;
        streams.reserve(draw.VertexBindings.size());
        sourceBindings.reserve(draw.VertexBindings.size());
        for (const auto& binding : draw.VertexBindings) {
            streams.push_back({ { binding.Binding, binding.ByteStride, binding.PerInstance }, binding.Bytes });
            sourceBindings.push_back({ binding.Binding, binding.ByteStride, binding.PerInstance });
        }
        attributes.reserve(draw.VertexAttributes.size());
        for (const auto& attribute : draw.VertexAttributes) {
            attributes.push_back({ attribute.Location, attribute.Binding,
                                   static_cast<Oot3d::PicaNriVertexScalar>(attribute.Format),
                                   attribute.ComponentCount, attribute.ByteOffset });
        }
        const auto layout = Oot3d::BuildPicaNriVertexInputLayout(sourceBindings, attributes);
        const auto resolution = mPicaGeometryRegistry.Resolve({
            draw.GeometryIdentity,
            draw.GeometryContentVersion,
            draw.GeometryIdentityAvailable,
            mFrameCounter,
            streams,
            attributes,
            draw.IndexBytes,
            draw.Indexed,
            draw.IndicesAre16Bit,
            draw.VertexCount,
            true,
        });
        if (resolution.Geometry == nullptr) {
            throw std::runtime_error("native PICA geometry preparation failed");
        }
        for (const uint64_t evicted : mPicaGeometryRegistry.TakeEvictedIdentities()) {
            RetireNativePicaGeometry(evicted);
        }
        const auto& geometry = *resolution.Geometry;
        auto& frame = mFrameResources[mCurrentFrame];
        // An alpha-tested caster samples the textures and lighting LUT the
        // ordered draw resolves, through the same cache (custom replacements
        // included); a texture that cannot be resolved leaves it out of the map.
        const bool alphaTested = RunsDiscardingAlphaTest(draw);
        std::array<TextureRecord*, 3> casterTextures{ &mFallbackTexture, &mFallbackTexture, &mFallbackTexture };
        TextureRecord* casterLightingLut = nullptr;
        if (alphaTested) {
            std::string textureError;
            bool resolved = true;
            for (const auto& texture : draw.Textures) {
                TextureRecord* record = GetOrCreateNativePicaTexture(
                    texture, &textureError, mFrameAzaharTextureGeneration, draw.RenderTargetNamespace,
                    texture.NativeContentHashAvailable ? &texture.NativeContentHash : nullptr);
                resolved = resolved && record != nullptr;
                if (record != nullptr)
                    casterTextures[texture.Slot] = record;
            }
            if (resolved && !draw.LightingLut.PackedEntries.empty()) {
                casterLightingLut = GetOrCreateNativePicaLightingLut(draw.LightingLut, &textureError);
                resolved = casterLightingLut != nullptr;
            }
            mDiagnostics.RecordNriDirectionalShadowPreparedAlpha(resolved);
            if (!resolved) {
                mDiagnostics.RecordNriDirectionalShadowPreparation(false, !resolution.Cacheable);
                return true;
            }
        }
        // The ordered submission owns the transient arena: a caster that does
        // not fit stays out of this sample's map rather than exhausting it.
        const bool placeable = resolution.Cacheable ||
            ((frame.VertexBytesUsed + 15U) & ~VkDeviceSize(15U)) + geometry.Payload.size() <= frame.VertexBuffer.Size;
        if (!geometry.PackedValid || !layout.Valid() || layout.Bindings.empty() || layout.Attributes.empty() ||
            !placeable) {
            mDiagnostics.RecordNriDirectionalShadowPreparation(false, !resolution.Cacheable);
            return true;
        }
        const auto placement = PlaceNativePicaGeometry(resolution, nullptr);
        std::vector<Oot3d::PicaSceneVertexBufferBinding> bindings;
        bindings.reserve(geometry.PackedBindings.size());
        for (const auto& binding : geometry.PackedBindings) {
            bindings.push_back({ binding.Binding, placement.BaseOffset + binding.Offset, binding.Size, binding.Stride });
        }
        // The map reads these uniforms before the ordered submission uploads
        // its own copy. Both blocks are reserved first: growth replaces the arena.
        const auto alignedUniform = [this](VkDeviceSize bytes) {
            return (bytes + mUniformBufferAlignment - 1U) & ~(mUniformBufferAlignment - 1U);
        };
        EnsureFrameUniformCapacity(
            alignedUniform(draw.VertexUniformBytes.size()) +
                (alphaTested ? alignedUniform(draw.FragmentUniformBytes.size()) : 0U),
            mUniformBufferAlignment);
        const auto uploadUniform = [&](std::span<const uint8_t> bytes) {
            const VkDeviceSize offset = alignedUniform(frame.UniformBytesUsed);
            std::memcpy(static_cast<uint8_t*>(frame.UniformBuffer.Mapped) + offset, bytes.data(), bytes.size());
            frame.UniformBytesUsed = offset + bytes.size();
            return offset;
        };
        const VkDeviceSize vertexUniformOffset = uploadUniform(draw.VertexUniformBytes);
        const VkDeviceSize fragmentUniformOffset = alphaTested ? uploadUniform(draw.FragmentUniformBytes) : 0U;

        uint32_t booleanUniforms = 0;
        std::memcpy(&booleanUniforms, draw.VertexUniformBytes.data() + Oot3d::kPicaPackedVertexBooleanMaskOffset,
                    sizeof(booleanUniforms));
        Renderer3ds::PicaShadowCasterCache transientCache;
        auto& shadowCache = resolution.Cacheable
            ? mNativePicaGeometryBuffers.at(geometry.Identity).ShadowCaster : transientCache;
        const uint64_t version = resolution.Cacheable
            ? geometry.ContentVersion ^ (geometry.StructuralSignature * 0x9e3779b97f4a7c15ULL) ^
              geometry.Identity ^ draw.VertexShaderKey : 0;
        auto shadowCaster = Renderer3ds::BuildPicaShadowCaster(
            {geometry.Payload, bindings, layout.Attributes, layout.Bindings, placement.BaseOffset,
             geometry.IndexOffset, geometry.VertexOrIndexCount, draw.BaseVertex, geometry.Indexed},
            draw.TemporalVertexProgram.Hooks, booleanUniforms,
            draw.VertexUniformBytes.subspan(Oot3d::kPicaPackedVertexFloatOffset), viewToWorld, version, shadowCache);
        shadowCaster.Dynamic = draw.Composition.NativeActorGeometry;
        shadowCaster.OwnerIdentity = draw.Composition.NativeActorIdentity;
        std::array<Oot3d::PicaSceneTextureBinding, 3> sceneTextures{};
        Oot3d::PicaSceneTextureBinding sceneLightingLut;
        if (alphaTested) {
            std::array<uint64_t, 3> replacementHashes{};
            for (size_t slot = 0; slot < sceneTextures.size(); ++slot) {
                const auto* texture = casterTextures[slot];
                auto& destination = sceneTextures[slot];
                destination.NativeImageHandle = ToPicaSceneNativeHandle(texture->NriImage);
                destination.ReplacementContentHash = texture->CustomReplacementPixelHash;
                destination.ImageWidth = texture->Width;
                destination.ImageHeight = texture->Height;
                destination.MipLevels = texture->UploadedMipLevels;
                destination.Slot = static_cast<uint8_t>(slot);
                destination.CustomReplacement = texture->CustomReplacementApplied;
                replacementHashes[slot] = texture->CustomReplacementPixelHash;
            }
            for (const auto& texture : draw.Textures) {
                auto& destination = sceneTextures[texture.Slot];
                destination.NativeContentHash = texture.NativeContentHash;
                destination.NativeContentHashAvailable = texture.NativeContentHashAvailable;
                destination.PhysicalAddress = texture.PhysicalAddress;
                destination.SourceWidth = texture.Width;
                destination.SourceHeight = texture.Height;
                destination.LodBiasRaw = texture.LodBiasRaw;
                destination.NativeFormat = texture.NativeFormat;
                destination.NativeType = texture.NativeType;
                destination.NativeWrapS = texture.NativeWrapS;
                destination.NativeWrapT = texture.NativeWrapT;
                destination.MinMipLevel = texture.MinMipLevel;
                destination.MaxMipLevel = texture.MaxMipLevel;
                destination.MinLinear = texture.MinLinear;
                destination.MagLinear = texture.MagLinear;
                destination.MipLinear = texture.MipLinear;
                destination.Bound = true;
            }
            if (casterLightingLut != nullptr) {
                sceneLightingLut.NativeImageHandle = ToPicaSceneNativeHandle(casterLightingLut->NriImage);
                sceneLightingLut.ImageWidth = casterLightingLut->Width;
                sceneLightingLut.ImageHeight = casterLightingLut->Height;
                sceneLightingLut.MipLevels = 1U;
                sceneLightingLut.ImageFormat = Oot3d::PicaSceneTextureImageFormat::R32Uint;
                sceneLightingLut.Bound = true;
            }
            // The cutout is part of the caster's content: a changed texture or
            // alpha state invalidates the pages it touches.
            if (shadowCaster.ContentVersion != 0U) {
                shadowCaster.ContentVersion ^= HashAlphaCasterMaterial(draw, replacementHashes) +
                    0x9e3779b97f4a7c15ULL + (shadowCaster.ContentVersion << 6U) + (shadowCaster.ContentVersion >> 2U);
            }
        }

        // Only what the map consumes is recorded: the canonical programs stand
        // in for the effective ones. Fragment state and textures are recorded
        // for an alpha-tested caster only; no resolved raster/target value is
        // known before the ordered submission.
        Oot3d::PicaSceneDrawRecordDesc record{
            .SubmissionId = draw.SubmissionId,
            .CommandListAddress = draw.CommandListAddress,
            .CommandListOffsetWords = draw.CommandListOffsetWords,
            .CompositionDomain = draw.CompositionDomain,
            .Composition = draw.Composition,
            .LightingWorldToView = draw.LightBinding.WorldToView,
            .SceneDirectionals = draw.LightBinding.SceneDirectionals,
            .ScenePoints = draw.LightBinding.ScenePoints,
            .NativeLightSources = draw.LightBinding.Sources,
            .ShadowCaster = shadowCaster,
            .Raster = {
                .SchemaVersion = Oot3d::kPicaSceneResolvedRasterStateSchemaVersion,
                .NativeViewportX = draw.ViewportX,
                .NativeViewportY = draw.ViewportY,
                .NativeViewportWidth = draw.ViewportWidth,
                .NativeViewportHeight = draw.ViewportHeight,
                .DepthRange = draw.DepthRange,
                .NearPlane = draw.NearPlane,
                .ScissorMode = draw.ScissorMode,
                .NativeScissorX1 = draw.ScissorX1,
                .NativeScissorY1 = draw.ScissorY1,
                .NativeScissorX2 = draw.ScissorX2,
                .NativeScissorY2 = draw.ScissorY2,
                .WBuffering = draw.WBuffering,
                .FramebufferFlipped = draw.FramebufferFlipped,
            },
            .RenderTarget = {
                .SchemaVersion = Oot3d::kPicaSceneRenderTargetStateSchemaVersion,
                .RenderTargetNamespace = draw.RenderTargetNamespace,
                .ColorPhysicalAddress = draw.FramebufferColorPhysicalAddress,
                .DepthPhysicalAddress = draw.FramebufferDepthPhysicalAddress,
                .NativeWidth = draw.FramebufferWidth,
                .NativeHeight = draw.FramebufferHeight,
                .NativeColorFormat = draw.FramebufferColorFormat,
                .NativeDepthFormat = draw.FramebufferDepthFormat,
                .SampleCount = static_cast<uint32_t>(mNativePicaSampleCount),
            },
            .CanonicalDescriptorSchemaVersion = draw.CanonicalDescriptorSchemaVersion,
            .CanonicalVertexProgramId = draw.CanonicalVertexProgramId,
            .CanonicalFragmentProgramId = draw.CanonicalFragmentProgramId,
            .CanonicalRasterStateId = draw.CanonicalRasterStateId,
            .CanonicalPipelineId = draw.CanonicalPipelineId,
            .CanonicalDynamicStateId = draw.CanonicalDynamicStateId,
            .CanonicalFullRegisterStateId = draw.CanonicalFullRegisterStateId,
            .VertexShaderKey = draw.VertexShaderKey,
            .VertexShaderSource = draw.VertexShaderSource,
            .FragmentShaderKey = draw.FragmentShaderKey,
            .FragmentShaderSource = draw.FragmentShaderSource,
            .EffectiveVertexShaderKey = draw.VertexShaderKey,
            .EffectiveVertexShaderSource = draw.VertexShaderSource,
            .EffectiveFragmentShaderKey = draw.FragmentShaderKey,
            .EffectiveFragmentShaderSource = draw.FragmentShaderSource,
            .GeometryIdentity = draw.GeometryIdentity,
            .GeometryContentVersion = draw.GeometryContentVersion,
            .GeometryIdentityAvailable = draw.GeometryIdentityAvailable,
            .VertexLayoutBindings = layout.Bindings,
            .VertexLayoutAttributes = layout.Attributes,
            .Material = {
                .SchemaVersion = Oot3d::kPicaSceneResolvedMaterialStateSchemaVersion,
                .Topology = ToPicaSceneTopology(draw.Topology),
                .CullMode = ToPicaSceneCullMode(draw),
                .FrontFace = ToPicaSceneFrontFace(draw),
                .FragmentOperationMode = draw.FragmentOperationMode,
                .ColorWriteMask = draw.ColorWriteMask,
                .LogicOperation = draw.LogicOperation,
                .Blend = draw.Blend,
                .AlphaTestEnabled = draw.AlphaTestEnabled,
                .AlphaCompare = draw.AlphaCompare,
                .AlphaReference = draw.AlphaReference,
                .DepthTestEnabled = draw.DepthTestEnabled,
                .DepthWriteEnabled = draw.DepthWriteEnabled,
                .DepthCompare = draw.DepthCompare,
                .Stencil = draw.Stencil,
                .FragmentFeatures = draw.FragmentFeatures,
            },
            .UniformBuffer = {
                ToPicaSceneNativeHandle(frame.UniformBuffer.Buffer),
                frame.UniformBuffer.Size,
            },
            .VertexUniformOffset = vertexUniformOffset,
            .VertexUniformSize = draw.VertexUniformBytes.size(),
            .GeometryBuffer = {
                ToPicaSceneNativeHandle(placement.Buffer),
                placement.Size,
            },
            .VertexBindings = bindings,
            .Indexed = draw.Indexed,
            .IndexOffset = placement.BaseOffset + geometry.IndexOffset,
            .VertexOrIndexCount = draw.Indexed ? geometry.VertexOrIndexCount : draw.VertexCount,
            .BaseVertex = draw.BaseVertex,
            .PerspectiveProjection = true,
            .DirectionalShadowCaster = true,
            .VertexShaderHooks = draw.TemporalVertexProgram.Hooks,
            .PackedVertexUniforms = draw.VertexUniformBytes,
            .VertexSourceIdentity = draw.VertexShaderSourceIdentity,
            .FragmentSourceIdentity = draw.FragmentShaderSourceIdentity,
            .EffectiveVertexSourceIdentity = draw.VertexShaderSourceIdentity,
            .EffectiveFragmentSourceIdentity = draw.FragmentShaderSourceIdentity,
        };
        if (alphaTested) {
            record.FragmentUniformOffset = fragmentUniformOffset;
            record.FragmentUniformSize = draw.FragmentUniformBytes.size();
            record.Textures = sceneTextures;
            record.LightingLut = sceneLightingLut;
            record.FragmentShaderHooks = draw.FragmentShaderHooks;
            record.PackedFragmentUniforms = draw.FragmentUniformBytes;
            // The parametric base of the always-available pair evaluates the
            // same TEV and alpha test from the packed uniforms: its caster is
            // one of a finite built-in family.
            if (!draw.UberFragmentShaderSource.empty() &&
                draw.UberFragmentShaderHooks.ValidFor(draw.UberFragmentShaderSource)) {
                record.FragmentShaderKey = draw.UberFragmentShaderKey;
                record.FragmentShaderSource = draw.UberFragmentShaderSource;
                record.EffectiveFragmentShaderKey = draw.UberFragmentShaderKey;
                record.EffectiveFragmentShaderSource = draw.UberFragmentShaderSource;
                record.FragmentSourceIdentity = draw.UberFragmentShaderSourceIdentity;
                record.EffectiveFragmentSourceIdentity = draw.UberFragmentShaderSourceIdentity;
                record.FragmentShaderHooks = draw.UberFragmentShaderHooks;
            }
        }
        const auto status = mPreparedShadowCasterFrame.Record(record);
        const bool caster = status == Oot3d::PicaSceneRecordStatus::Recorded &&
            mPreparedShadowCasterFrame.DirectionalShadowCaster(mPreparedShadowCasterFrame.Draws().size() - 1U);
        if (caster) {
            mPreparedShadowCasters.insert_or_assign(draw.SubmissionId, shadowCaster);
        }
        mDiagnostics.RecordNriDirectionalShadowPreparation(caster, !resolution.Cacheable);
        return true;
    } catch (const std::exception& exception) {
        SetNativeError(error, exception.what());
        return false;
    }
}

bool GfxRenderingAPIVulkan::SubmitPicaDraw(
    const GfxNativePicaDrawView& draw, std::string* error) {
    Renderer3ds::SlowStepScope slowStep("pica_draw");
    try {
        const bool collectCpuTimings = mDiagnostics.Enabled();
        using CpuClock = std::chrono::steady_clock;
        auto cpuStart = CpuClock::time_point{};
        auto cpuStageStart = CpuClock::time_point{};
        Oot3dVulkanNativePicaCpuTimings cpuTimings;
        if (collectCpuTimings) {
            cpuStart = CpuClock::now();
            cpuStageStart = cpuStart;
            cpuTimings.DrawCount = 1U;
        }
        const auto finishCpuStage =
            [&](double& destinationMilliseconds) {
                if (!collectCpuTimings) {
                    return;
                }
                const auto now = CpuClock::now();
                destinationMilliseconds +=
                    std::chrono::duration<double, std::milli>(
                        now - cpuStageStart).count();
                cpuStageStart = now;
            };
        if (!mFrameActive) {
            throw std::runtime_error(
                "native PICA draw submitted outside an active Vulkan frame");
        }
        if (draw.VertexShaderSource.empty() ||
            draw.FragmentShaderSource.empty() || draw.VertexCount == 0 ||
            draw.VertexBindings.empty() || draw.VertexAttributes.empty()) {
            throw std::runtime_error("native PICA draw is incomplete");
        }
        if (mPicaAotShaderPack.Loaded() &&
            (draw.CanonicalDescriptorSchemaVersion == 0U ||
             draw.CanonicalDescriptorSchemaVersion !=
                 mPicaAotShaderPack.DescriptorSchemaVersion())) {
            throw std::runtime_error(
                "native PICA draw descriptor schema does not match the "
                "AOT shader pack");
        }
        if (draw.FragmentOperationMode != 0U &&
            draw.FragmentOperationMode != 3U) {
            throw std::runtime_error(
                "native PICA non-default fragment operation is unsupported");
        }
        if (draw.ScissorMode == 1U) {
            throw std::runtime_error(
                "native PICA exclude scissor requires shader emulation");
        }
        if (draw.ScissorMode != 0U && draw.ScissorMode != 3U) {
            throw std::runtime_error("invalid native PICA scissor mode");
        }
        if (draw.FramebufferWidth == 0 || draw.FramebufferHeight == 0) {
            throw std::runtime_error("native PICA framebuffer dimensions are zero");
        }

        if (mNativePicaCompositionSchedule.Valid()) {
            std::string scheduleError;
            const bool matched =
                mNativePicaCompositionSchedule.ConsumeDraw(
                    Renderer3ds::BuildPicaCompositionDrawReference(draw),
                    &scheduleError);
            mDiagnostics.RecordPicaCompositionScheduleDraw(matched);
            if (!matched) {
                throw std::runtime_error(scheduleError);
            }
        }
        const bool sceneDraw =
            draw.CompositionDomain ==
                ::Oot3d::Renderer::PicaCompositionDomain::Scene;
        const bool opaqueWorldDraw =
            sceneDraw &&
            draw.Composition.Layer ==
                ::Oot3d::Renderer::PicaCompositionLayer::OpaqueWorld;
        const bool nativeMemoryOnly = mNativePicaCompositionSchedule.Purpose() ==
            Renderer3ds::PicaCompositionPurpose::NativeMemory;

        const auto& graphicsSettings = mFrameGraphicsSettings;
        if (!nativeMemoryOnly && !mNativeFidelityProfile && sceneDraw &&
            draw.Composition.NativeProjectedShadow &&
            Oot3d::UsesFullSceneLighting(graphicsSettings.Effects.SurfaceLighting)) {
            // Consume native ordering and its graph boundaries, but do not
            // rasterize the legacy projected shadow into presentation or guides.
            // The canonical native-memory replay remains untouched.
            auto& target = GetOrCreateNativePicaRenderTarget(draw);
            ExecuteNativePicaBeforeDrawAnchors(target, draw,
                cpuTimings.DirectionalShadowMilliseconds, cpuTimings.GrassRenderMilliseconds);
            return true;
        }
        const uint64_t azaharTextureGeneration =
            mFrameAzaharTextureGeneration;
        // Material guide values: screen-space reflections or GI specular
        // (a reflection source resolved through the same guide).
        const bool reflectionsEnabled = !nativeMemoryOnly &&
            (graphicsSettings.Effects.Reflections !=
                 Oot3d::ReflectionMode::Off ||
             (graphicsSettings.Effects.GlobalIllumination.Mode !=
                  Oot3d::GlobalIlluminationMode::Off &&
              graphicsSettings.Effects.GlobalIllumination.Specular));
        std::array<Oot3d::ReflectionMaterialTextureIdentity, 3U>
            reflectionTextureIdentities{};
        size_t reflectionTextureCount = 0U;
        if (reflectionsEnabled) {
            for (const auto& texture : draw.Textures) {
                const auto contentHash =
                    ResolveNativeTextureBaseLevelContentHash(texture);
                if (!contentHash.has_value() || texture.NativeType != 0U ||
                    texture.Slot >= 3U ||
                    reflectionTextureCount >=
                        reflectionTextureIdentities.size()) {
                    continue;
                }
                reflectionTextureIdentities[
                    reflectionTextureCount++] = {
                    *contentHash,
                    texture.Width,
                    texture.Height,
                    texture.Slot,
                };
            }
        }
        std::optional<Oot3d::ResolvedReflectionMaterialProfile>
            resolvedReflectionMaterial;
        if (reflectionsEnabled) {
            resolvedReflectionMaterial =
                Oot3d::ResolveReflectionMaterialProfile(
                    graphicsSettings.Effects.ReflectionMaterials,
                    std::span(
                        reflectionTextureIdentities.data(),
                        reflectionTextureCount));
        }
        const bool grassPerspectiveProjection =
            Oot3d::UsesPicaPerspectiveProjection(
                draw.VertexUniformBytes);
        const bool directionalShadowReceiver = !nativeMemoryOnly &&
            Oot3d::UsesFullSceneLighting(graphicsSettings.Effects.SurfaceLighting) &&
            mDirectionalShadowMapBarrierPlan.Valid() &&
            Oot3d::PicaExtensionPassEnabled(
                mPicaExtensionGraph,
                Oot3d::PicaExtensionPass::DirectionalShadowLighting) &&
            grassPerspectiveProjection;
        // Point lights with a cube map: only NRI-owned draws bind the atlas.
        const bool pointShadowReceiver = !nativeMemoryOnly && !mNativeFidelityProfile &&
            graphicsSettings.Effects.PointShadows.Mode == Oot3d::PointShadowMode::CubeMaps &&
            Oot3d::UsesFullSceneLighting(graphicsSettings.Effects.SurfaceLighting) &&
            mPointShadowMapBarrierPlan.Valid() &&
            Oot3d::PicaExtensionPassEnabled(
                mPicaExtensionGraph, Oot3d::PicaExtensionPass::PointShadowLighting) &&
            mNriPicaPipelineBridge.OwnedDrawsEnabled() && grassPerspectiveProjection;
        Oot3d::NriPointShadowHistorySnapshot pointShadowHistory;
        Renderer3ds::PicaPointShadowAssignment pointShadowAssignment;
        if (pointShadowReceiver) {
            pointShadowHistory = mNriDirectionalShadowPass.FindLatestPointHistory(
                draw.RenderTargetNamespace, draw.FramebufferColorPhysicalAddress, mFrameCounter);
            if (pointShadowHistory.Valid()) {
                const auto& layout = pointShadowHistory.Layout;
                for (uint32_t row = 0; row < layout.SlotCount; ++row)
                    if (pointShadowHistory.Slots[row].Valid())
                        pointShadowAssignment.Identities[row] = pointShadowHistory.Slots[row].SourceIdentity;
                pointShadowAssignment.Parameters = {
                    pointShadowHistory.Range, pointShadowHistory.Strength,
                    static_cast<float>(layout.FaceTexels),
                    static_cast<float>(Renderer3ds::kPointShadowBorderTexels) };
                pointShadowAssignment.Atlas = {
                    1.0F / static_cast<float>(layout.Width), 1.0F / static_cast<float>(layout.Height),
                    Renderer3ds::kPointShadowNearFraction, 1.0F };
            }
        }
        std::optional<Renderer3ds::PicaSurfaceLightingUniforms> surfaceLighting;
        const auto* environmentWorldToView = draw.LightBinding.WorldToView
            ? &*draw.LightBinding.WorldToView : nullptr;
        Oot3d::DirectionalShadowMatrix environmentViewToWorld{};
        if (!nativeMemoryOnly && !mNativeFidelityProfile &&
            graphicsSettings.Effects.SurfaceLighting != Oot3d::SurfaceLightingMode::Native &&
            draw.CompositionDomain == Renderer3ds::PicaCompositionDomain::Scene &&
            Renderer3ds::PicaCompositionReceivesSurfaceLighting(draw.Composition.Layer) &&
            grassPerspectiveProjection && draw.FragmentOperationMode != 3U &&
            draw.VertexUniformBytes.size() >= Oot3d::kPicaPackedVertexFloatOffset) {
            uint32_t booleanUniforms = 0U;
            std::memcpy(&booleanUniforms, draw.VertexUniformBytes.data() +
                        Oot3d::kPicaPackedVertexBooleanMaskOffset, sizeof(booleanUniforms));
            if (environmentWorldToView &&
                (!mSurfaceLightingView || mSurfaceLightingView->WorldToView != *environmentWorldToView))
                mSurfaceLightingView = Renderer3ds::BuildPicaSurfaceLightingView(*environmentWorldToView);
            surfaceLighting = Renderer3ds::BuildPicaSurfaceLightingUniforms(
                draw.TemporalVertexProgram.Hooks, booleanUniforms,
                draw.VertexUniformBytes.subspan(Oot3d::kPicaPackedVertexFloatOffset),
                draw.LightBinding, mFrameEnvironment,
                environmentWorldToView && mSurfaceLightingView ? &*mSurfaceLightingView : nullptr,
                pointShadowHistory.Valid() ? &pointShadowAssignment : nullptr);
            if (surfaceLighting) {
                // Relative source weights (the guide's 8-bit radiance would
                // clip gains above one); the gain applies when GI is composed.
                const auto sources =
                    Oot3d::ResolveGlobalIlluminationSources(graphicsSettings.Effects.GlobalIllumination);
                surfaceLighting->GlobalIlluminationSources = {sources.Weights[0], sources.Weights[1],
                                                              sources.Weights[2], 0.0F};
            }
            if (surfaceLighting && environmentWorldToView && directionalShadowReceiver &&
                !Oot3d::InvertDirectionalShadowMatrix(*environmentWorldToView, environmentViewToWorld))
                surfaceLighting.reset();
        }
        std::array<TextureRecord*, 3> textures{
            &mFallbackTexture, &mFallbackTexture, &mFallbackTexture};
        std::array<VkFormat, 3> textureFormats{
            VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_R8G8B8A8_UNORM};
        std::array<bool, 3> integerTextures{};
        for (const auto& texture : draw.Textures) {
            if (texture.Slot >= textures.size()) {
                throw std::runtime_error("native PICA texture slot is invalid");
            }
            if (texture.NativeType != 0U &&
                !(texture.Slot == 0U &&
                  (texture.NativeType == 2U || texture.NativeType == 3U))) {
                throw std::runtime_error(
                    "native PICA texture type is unsupported by sampler2D");
            }
            const auto resolutionStart = std::chrono::steady_clock::now();
            MaterializeRestoredGpuTextureCopyProducers();
            TextureRecord* record =
                GetOrCreateNativePicaTexture(
                    texture, error, azaharTextureGeneration,
                    draw.RenderTargetNamespace,
                    texture.NativeContentHashAvailable
                        ? &texture.NativeContentHash
                        : nullptr);
            if (record == nullptr) {
                return false;
            }
            const auto finished = std::chrono::steady_clock::now();
            const auto elapsed = [](const auto& from, const auto& to) {
                return std::chrono::duration<double>(to - from).count();
            };
            if (texture.CopyReferences.empty() == false) {
                // Producer references resolved into the sampled image on the
                // GPU: no CPU hash of the copy's ranges, no decode of them and no
                // queue idle.
                mGpuTextureCopySamplerCost.BindSeconds +=
                    elapsed(resolutionStart, finished);
            } else {
                mGpuTextureCopySamplerCost.ResolveSeconds +=
                    elapsed(resolutionStart, finished);
            }
            textures[texture.Slot] = record;
            textureFormats[texture.Slot] =
                texture.NativeType == 2U ? VK_FORMAT_R32_UINT
                                         : VK_FORMAT_R8G8B8A8_UNORM;
            integerTextures[texture.Slot] =
                texture.NativeType == 2U;
        }
        TextureRecord* lightingLutTexture = &mFallbackTexture;
        bool lightingLutInteger = false;
        if (!draw.LightingLut.PackedEntries.empty()) {
            lightingLutTexture =
                GetOrCreateNativePicaLightingLut(draw.LightingLut, error);
            if (lightingLutTexture == nullptr) {
                return false;
            }
            lightingLutInteger = true;
        }
        finishCpuStage(cpuTimings.TextureMilliseconds);
        const auto nativeTextures = textures;
        NativePicaMaterialBundle* pbrMaterial = nullptr;
        Renderer3ds::PicaPbrShaderOptions pbrOptions;
        std::optional<Renderer3ds::PicaPbrUniforms> pbrUniforms;
        std::optional<Renderer3ds::PicaPbrMaterialParameters> materialPreview;
        std::array<TextureRecord*, Renderer3ds::kPicaPbrTextureCount> pbrTextures;
        pbrTextures.fill(&mFallbackTexture);
        VkSampler pbrSampler = mFallbackTexture.Sampler;
        if (surfaceLighting &&
            Oot3d::UsesPbrMaterials(graphicsSettings.Effects.SurfaceLighting) &&
            draw.FragmentShaderHooks.ValidFor(draw.FragmentShaderSource) &&
            draw.FragmentShaderHooks.Supports(Renderer3ds::PicaShaderHook::AfterNativeTev) &&
            (!draw.FragmentFeatures.FragmentLightingEnabled ||
             (draw.FragmentFeatures.FragmentLighting.Valid() &&
              draw.FragmentUniformBytes.size() >= Oot3d::kPicaPackedFragmentLightingProgramOffset +
                  sizeof(Renderer3ds::PicaLightingProgram))) &&
            draw.FragmentUniformBytes.size() >= Oot3d::kPicaPackedFragmentTevProgramOffset +
                sizeof(Renderer3ds::PicaTevProgram)) {
            Renderer3ds::PicaTevProgram tev;
            std::memcpy(&tev, draw.FragmentUniformBytes.data() +
                Oot3d::kPicaPackedFragmentTevProgramOffset, sizeof(tev));
            const uint8_t colorMask = Renderer3ds::PicaTevMaterialTextureMask(tev) & 7U;
            uint8_t automaticMask = colorMask;
            if (draw.FragmentFeatures.FragmentLighting.BumpMode != Renderer3ds::PicaFragmentLightingBumpMode::None)
                automaticMask &= static_cast<uint8_t>(
                    ~(1U << draw.FragmentFeatures.FragmentLighting.BumpTextureUnit));
            std::shared_ptr<const ::Oot3d::Renderer::AzaharPbrMaterial> selected;
            uint8_t selectedSlot = 0xffU;
            for (uint8_t slot = 0; slot < 3U; ++slot) {
                const auto* sample = draw.FragmentShaderHooks.TextureSample(slot);
                if ((colorMask & (1U << slot)) == 0U || integerTextures[slot] || sample == nullptr ||
                    sample->Coordinate > 2U ||
                    (sample->Operation != Renderer3ds::PicaTextureCoordinateOperation::NativeVFlip &&
                     sample->Operation != Renderer3ds::PicaTextureCoordinateOperation::ProjectedNativeVFlip))
                    continue;
                const auto resolved = ::Oot3d::Renderer::AzaharTexturePackRuntime::Instance().
                    ResolveMaterialOrQueue(textures[slot]->CustomReplacementHash, azaharTextureGeneration);
                if (!resolved.Material) continue;
                const uint8_t explicitSlot = resolved.Preview
                    ? resolved.Preview->TextureSlot : resolved.Material->Parameters.TextureSlot;
                if ((explicitSlot == 0xffU && automaticMask != (1U << slot)) ||
                    (explicitSlot != 0xffU && explicitSlot != slot)) continue;
                if (selected) {
                    selected.reset();
                    break; // Two authored material selectors are ambiguous, not an ordering rule.
                }
                selected = resolved.Material;
                materialPreview = resolved.Preview;
                selectedSlot = slot;
            }
            if (selected) pbrMaterial = GetOrCreateNativePicaMaterial(selected);
            if (pbrMaterial != nullptr) {
                pbrOptions.TextureSlot = selectedSlot;
                pbrOptions.NativeFragmentLighting = draw.FragmentFeatures.FragmentLightingEnabled;
                const auto& layout = draw.FragmentFeatures.FragmentLighting;
                for (size_t slot = 0; slot < layout.ActiveLightCount; ++slot) {
                    const auto& light = layout.Lights[layout.LightPermutation[slot]];
                    pbrOptions.NativeAttenuationLuts |= light.SpotAttenuationEnabled || light.DistanceAttenuationEnabled;
                }
                Renderer3ds::PicaLightingProgram lighting;
                if (draw.FragmentUniformBytes.size() >= Oot3d::kPicaPackedFragmentLightingProgramOffset + sizeof(lighting))
                    std::memcpy(&lighting, draw.FragmentUniformBytes.data() +
                        Oot3d::kPicaPackedFragmentLightingProgramOffset, sizeof(lighting));
                uint8_t mapMask = 0U;
                for (size_t role = 0; role < pbrTextures.size(); ++role) {
                    if (!pbrMaterial->Material->Textures[role]) continue;
                    mapMask |= static_cast<uint8_t>(1U << role);
                    pbrTextures[role] = &pbrMaterial->Images[role];
                }
                pbrUniforms = Renderer3ds::BuildPicaPbrUniforms(
                    materialPreview ? *materialPreview : pbrMaterial->Material->Parameters, mapMask, lighting);
                pbrOptions.SeamSoftening = mPbrSeamsAvailable && pbrUniforms->SeamSettings[0] > 0 &&
                    std::bit_cast<uint32_t>(pbrUniforms->SeamSettings[2]) != 0 &&
                    draw.FragmentShaderHooks.TextureSample(selectedSlot)->Operation ==
                        Renderer3ds::PicaTextureCoordinateOperation::NativeVFlip;
                pbrSampler = nativeTextures[selectedSlot]->Sampler;
                resolvedReflectionMaterial.reset();
            }
        }
        if (graphicsSettings.Effects.SurfaceLighting == Oot3d::SurfaceLightingMode::NativePbr &&
            pbrMaterial == nullptr)
            surfaceLighting.reset();
        // AO texture relief reads the one texture whose colour the draw's TEV
        // passes to its output (never a bump or shadow unit); several colour
        // textures leave the draw without relief rather than guess a layer.
        uint8_t reliefTexture = 0xffU;
        if (!nativeMemoryOnly && sceneDraw && draw.DepthWriteEnabled &&
            graphicsSettings.Effects.AmbientOcclusion != Oot3d::AmbientOcclusionMode::Off &&
            graphicsSettings.Effects.AoRelief != Oot3d::AoTextureRelief::Off &&
            draw.FragmentUniformBytes.size() >= Oot3d::kPicaPackedFragmentTevProgramOffset +
                sizeof(Renderer3ds::PicaTevProgram)) {
            Renderer3ds::PicaTevProgram tev;
            std::memcpy(&tev, draw.FragmentUniformBytes.data() +
                Oot3d::kPicaPackedFragmentTevProgramOffset, sizeof(tev));
            uint8_t colorTextures = Renderer3ds::PicaTevMaterialTextureMask(tev) & 7U;
            if (draw.FragmentFeatures.FragmentLighting.BumpMode != Renderer3ds::PicaFragmentLightingBumpMode::None)
                colorTextures &= static_cast<uint8_t>(
                    ~(1U << draw.FragmentFeatures.FragmentLighting.BumpTextureUnit));
            for (uint8_t slot = 0; slot < 3U; ++slot)
                if (integerTextures[slot]) colorTextures &= static_cast<uint8_t>(~(1U << slot));
            if (std::has_single_bit(colorTextures)) {
                const auto slot = static_cast<uint8_t>(std::countr_zero(colorTextures));
                const auto* sample = draw.FragmentShaderHooks.TextureSample(slot);
                if (sample != nullptr &&
                    sample->Operation == Renderer3ds::PicaTextureCoordinateOperation::NativeVFlip)
                    reliefTexture = slot;
            }
            mDiagnostics.RecordAoReliefDraw(reliefTexture < 3U);
        }
        finishCpuStage(cpuTimings.TextureMilliseconds);
        const Oot3d::PicaShaderPipelineRequest variantRequest{
            draw.VertexShaderSource,
            draw.VertexShaderKey,
            draw.FragmentShaderSource,
            draw.FragmentShaderKey,
            {
                draw.FramebufferWidth,
                draw.FramebufferHeight,
                draw.FragmentOperationMode,
                draw.DepthTestEnabled,
                draw.DepthWriteEnabled,
                draw.Blend,
                draw.ColorWriteMask,
                grassPerspectiveProjection,
                directionalShadowReceiver,
                draw.CompositionDomain,
                draw.DepthCompare,
                draw.Composition.Layer,
                pointShadowReceiver && surfaceLighting.has_value(),
            },
            mTemporalMotionEnabled && !nativeMemoryOnly,
            resolvedReflectionMaterial.has_value()
                ? std::optional<Oot3d::ReflectionMaterialParameters>(
                      resolvedReflectionMaterial->Parameters)
                : std::nullopt,
            mNativeFidelityProfile || nativeMemoryOnly,
            draw.TemporalVertexProgram,
            draw.FragmentShaderHooks,
            draw.VertexShaderSourceIdentity,
            draw.FragmentShaderSourceIdentity,
            surfaceLighting.has_value(),
            pbrOptions,
            reliefTexture,
        };
        static const Oot3d::EffectsSettings nativeMemoryEffects;
        bool variantCacheHit = false;
        const auto& shaderVariant =
            mPicaShaderPipelineCache.Resolve(
                variantRequest,
                nativeMemoryOnly ? nativeMemoryEffects : graphicsSettings.Effects,
                &variantCacheHit);
        if (pbrMaterial != nullptr && pbrMaterial->Material->ColorReplacement &&
            Oot3d::HasPicaShaderInstrumentationFeature(
                shaderVariant.AppliedFeatures, Oot3d::PicaShaderInstrumentationFeature::PbrMaterial))
            textures[pbrOptions.TextureSlot] = &pbrMaterial->Images[Renderer3ds::kPicaPbrTextureCount];
        const auto variantCacheStats =
            mPicaShaderPipelineCache.Stats();
        mDiagnostics.RecordPicaShaderSelection(
            shaderVariant.IsCanonical(),
            shaderVariant.RequestedFeatures !=
                Oot3d::PicaShaderInstrumentationFeature::None,
            static_cast<uint32_t>(std::min<size_t>(
                variantCacheStats.CanonicalEntries,
                std::numeric_limits<uint32_t>::max())),
            static_cast<uint32_t>(std::min<size_t>(
                variantCacheStats.InstrumentationEntries,
                std::numeric_limits<uint32_t>::max())),
            shaderVariant.DirectFragmentHooksUsed,
            Oot3d::HasPicaShaderInstrumentationFeature(
                shaderVariant.RequestedFeatures,
                Oot3d::PicaShaderInstrumentationFeature::TemporalVertex),
            shaderVariant.DirectTemporalVertexProgramUsed,
            static_cast<uint32_t>(std::min<uint64_t>(
                variantCacheStats.CanonicalOutputAudits,
                std::numeric_limits<uint32_t>::max())),
            static_cast<uint32_t>(std::min<uint64_t>(
                variantCacheStats.CanonicalOutputContractRejects,
                std::numeric_limits<uint32_t>::max())),
            static_cast<uint32_t>(std::min<uint64_t>(
                variantCacheStats.CanonicalCompatibilityAnalyses,
                std::numeric_limits<uint32_t>::max())),
            static_cast<uint32_t>(std::min<uint64_t>(
                variantCacheStats.TypedInstrumentationContractRejects,
                std::numeric_limits<uint32_t>::max())));
        cpuTimings.ShaderVariantCacheHits =
            variantCacheHit ? 1U : 0U;
        cpuTimings.ShaderVariantCacheMisses =
            variantCacheHit ? 0U : 1U;
        cpuTimings.ShaderVariantCacheEntries =
            static_cast<uint32_t>(std::min<size_t>(
                variantCacheStats.CanonicalEntries +
                    variantCacheStats.InstrumentationEntries,
                std::numeric_limits<uint32_t>::max()));
        GfxNativePicaDrawView effectiveDraw = draw;
        effectiveDraw.VertexShaderSource =
            shaderVariant.VertexShaderSource;
        effectiveDraw.VertexShaderKey =
            shaderVariant.VertexShaderKey;
        effectiveDraw.VertexShaderSourceIdentity =
            shaderVariant.VertexShaderSourceIdentity;
        effectiveDraw.FragmentShaderSource =
            shaderVariant.FragmentShaderSource;
        effectiveDraw.FragmentShaderKey =
            shaderVariant.FragmentShaderKey;
        effectiveDraw.FragmentShaderSourceIdentity =
            shaderVariant.FragmentShaderSourceIdentity;
        mPicaEffectiveShaderInventory.Observe(
            Oot3d::PicaAotShaderStage::Vertex,
            effectiveDraw.VertexShaderSource,
            effectiveDraw.CanonicalDescriptorSchemaVersion,
            effectiveDraw.CanonicalPipelineId,
            effectiveDraw.VertexShaderKey,
            mFrameGraphicsSettingsRevision);
        mPicaEffectiveShaderInventory.Observe(
            Oot3d::PicaAotShaderStage::Fragment,
            effectiveDraw.FragmentShaderSource,
            effectiveDraw.CanonicalDescriptorSchemaVersion,
            effectiveDraw.CanonicalPipelineId,
            effectiveDraw.FragmentShaderKey,
            mFrameGraphicsSettingsRevision);

        Oot3d::TraceOutlineDraw(mFrameCounter, draw, grassPerspectiveProjection,
                               shaderVariant.FragmentOutputs.WritesOutlineGeometryGuide);
        static const bool toonDiagnosticsEnabled = [] {
            const char* value =
                std::getenv("OOT3D_GRAPHICS_TOON_DIAGNOSTICS");
            return value != nullptr &&
                   std::string_view(value) == "1";
        }();
        if (toonDiagnosticsEnabled) {
            static uint32_t diagnosticDrawCount = 0;
            if (diagnosticDrawCount++ < 24U) {
                std::fprintf(stderr,
                             "OOT3D_TOON draw=%u mode=%u target=%ux%u depth=%u/%u blend=%u mask=%02x eligibility=%u "
                             "applied=%u\n",
                    diagnosticDrawCount,
                    static_cast<unsigned>(graphicsSettings.Effects.Toon),
                    draw.FramebufferWidth, draw.FramebufferHeight,
                    draw.DepthTestEnabled ? 1U : 0U,
                    draw.DepthWriteEnabled ? 1U : 0U,
                    draw.Blend.Enabled ? 1U : 0U, draw.ColorWriteMask,
                    static_cast<unsigned>(
                        shaderVariant.ToonEligibility),
                    shaderVariant.ToonEligibility ==
                            Oot3d::PicaToonEligibility::Eligible
                        ? 1U
                        : 0U);
            }
        }
        if (graphicsSettings.Effects.Toon != Oot3d::ToonMode::Off) {
            auto& telemetry = Oot3d::PicaToonTelemetry::Instance();
            const uint64_t telemetryTotal =
                telemetry.Record(shaderVariant.ToonEligibility);
            if (shaderVariant.ToonEligibility ==
                Oot3d::PicaToonEligibility::Eligible) {
                mDiagnostics.RecordToonDraw(
                    shaderVariant.ToonMaterialPath);
            }
            if (telemetryTotal == 512U) {
                const auto snapshot = telemetry.Snapshot();
                spdlog::info(
                    "PICA toon classifier: applied={}, outside-scene={}, no-depth={}, blended={}, no-rgb={}, unsupported={}",
                    snapshot.Count(Oot3d::PicaToonEligibility::Eligible),
                    snapshot.Count(Oot3d::PicaToonEligibility::OutsideScene),
                    snapshot.Count(Oot3d::PicaToonEligibility::NoDepth),
                    snapshot.Count(Oot3d::PicaToonEligibility::Blended),
                    snapshot.Count(Oot3d::PicaToonEligibility::NoRgbOutput),
                    snapshot.Count(Oot3d::PicaToonEligibility::UnsupportedShader));
            }
        }
        if (Oot3d::HasPicaShaderInstrumentationFeature(
                shaderVariant.AppliedFeatures, Oot3d::PicaShaderInstrumentationFeature::NormalGuide))
            mDiagnostics.RecordNormalGuideDraw(
                shaderVariant.TypedNormalGuide,
                Oot3d::HasPicaShaderInstrumentationFeature(
                    shaderVariant.AppliedFeatures, Oot3d::PicaShaderInstrumentationFeature::NormalGuideTextureRelief));
        if (shaderVariant.AmbientGuideEligibility ==
                Oot3d::PicaAmbientOcclusionGuideEligibility::
                    AppliedExact ||
            shaderVariant.AmbientGuideEligibility ==
                Oot3d::PicaAmbientOcclusionGuideEligibility::
                    AppliedFallback) {
            mDiagnostics.RecordAmbientOcclusionAmbientGuideDraw(
                shaderVariant.AmbientGuideEligibility ==
                    Oot3d::PicaAmbientOcclusionGuideEligibility::
                        AppliedExact);
        }
        if (reflectionsEnabled) {
            const auto eligibility =
                shaderVariant.ReflectionEligibility;
            const bool profiled =
                eligibility ==
                Oot3d::PicaReflectionMaterialEligibility::
                    ExplicitTextureProfile;
            mDiagnostics.RecordReflectionMaterialDraw(
                eligibility ==
                        Oot3d::PicaReflectionMaterialEligibility::
                            Eligible ||
                    profiled,
                profiled,
                eligibility ==
                    Oot3d::PicaReflectionMaterialEligibility::
                        NoSpecularTevUse,
                eligibility ==
                    Oot3d::PicaReflectionMaterialEligibility::
                        UnsupportedShader);
            const bool worldMaterialCandidate =
                eligibility ==
                    Oot3d::PicaReflectionMaterialEligibility::Eligible ||
                eligibility ==
                    Oot3d::PicaReflectionMaterialEligibility::
                        ExplicitTextureProfile ||
                eligibility ==
                    Oot3d::PicaReflectionMaterialEligibility::
                        NoSpecularTevUse ||
                eligibility ==
                    Oot3d::PicaReflectionMaterialEligibility::
                        UnsupportedShader;
            if (worldMaterialCandidate) {
                for (size_t textureIndex = 0U;
                     textureIndex < reflectionTextureCount;
                     ++textureIndex) {
                    const auto& texture =
                        reflectionTextureIdentities[textureIndex];
                    const bool selectedProfile =
                        profiled &&
                        resolvedReflectionMaterial.has_value() &&
                        resolvedReflectionMaterial->TextureHash ==
                            texture.ContentHash &&
                        resolvedReflectionMaterial->MapperSlot ==
                            texture.MapperSlot;
                    mDiagnostics.RecordReflectionTextureUsage(
                        texture.ContentHash,
                        texture.Width,
                        texture.Height,
                        texture.MapperSlot,
                        selectedProfile,
                        selectedProfile
                            ? resolvedReflectionMaterial->RuleId
                            : 0U,
                        selectedProfile
                            ? static_cast<uint8_t>(
                                  resolvedReflectionMaterial->Profile)
                            : uint8_t{0},
                        selectedProfile
                            ? resolvedReflectionMaterial->Parameters
                                  .Reflectivity
                            : 0.0F,
                        selectedProfile
                            ? resolvedReflectionMaterial->Parameters
                                  .Roughness
                            : 1.0F);
                }
            }
            // Read once: the CRT environment lookup locks and scans per call.
            static const bool reflectionMaterialDiagnostics = [] {
                const char* value = std::getenv("OOT3D_GRAPHICS_REFLECTION_MATERIAL_DIAGNOSTICS");
                return value != nullptr && std::string_view(value) == "1";
            }();
            if (reflectionMaterialDiagnostics) {
                static uint32_t reflectionMaterialDiagnosticCount = 0;
                const bool explicitProfileMatched =
                    resolvedReflectionMaterial.has_value();
                if (reflectionMaterialDiagnosticCount++ < 24U ||
                    explicitProfileMatched) {
                    const auto shader =
                        effectiveDraw.FragmentShaderSource;
                    std::fprintf(
                        stderr,
                        "OOT3D_REFLECTION_MATERIAL draw=%u eligibility=%u "
                        "target=%ux%u depth=%u/%u mask=%02x guide=%u "
                        "signal=%u marker=%u secondary=%u "
                        "profile_matched=%u profile_applied=%u "
                        "texture=%016llx\n",
                        reflectionMaterialDiagnosticCount,
                        static_cast<unsigned>(eligibility),
                        draw.FramebufferWidth, draw.FramebufferHeight,
                        draw.DepthTestEnabled ? 1U : 0U,
                        draw.DepthWriteEnabled ? 1U : 0U,
                        draw.ColorWriteMask,
                        shader.find("pica_material_guide") !=
                            std::string_view::npos,
                        shader.find("oot3d_specular_signal") !=
                            std::string_view::npos,
                        shader.find(
                            "OOT3D_PICA_MATERIAL_TOON_POINT") !=
                            std::string_view::npos,
                        shader.find("secondary_fragment_color") !=
                            std::string_view::npos,
                        explicitProfileMatched ? 1U : 0U,
                        profiled ? 1U : 0U,
                        static_cast<unsigned long long>(
                            resolvedReflectionMaterial.has_value()
                                ? resolvedReflectionMaterial->TextureHash
                                : reflectionTextureCount == 0U
                                      ? 0U
                                      : reflectionTextureIdentities[0]
                                            .ContentHash));
                }
            }
        }
        if (shaderVariant.Reactive) {
            mDiagnostics.RecordReactiveWorldDraw();
        }
        finishCpuStage(cpuTimings.ShaderVariantMilliseconds);

        const auto shaderKey = Renderer3ds::BuildPicaShaderModuleIdentity(
            effectiveDraw.VertexShaderSourceIdentity,
            effectiveDraw.FragmentShaderSourceIdentity,
            effectiveDraw.CanonicalDescriptorSchemaVersion,
            mNriPicaPipelineBridge.Available(), shaderVariant.FragmentOutputs);
        auto& nativeShaderCache = shaderVariant.IsCanonical()
            ? mCanonicalNativePicaShaders
            : mInstrumentedNativePicaShaders;
        auto shaderIt = nativeShaderCache.find(shaderKey);
        if (shaderIt == nativeShaderCache.end()) {
            NativePicaShaderProgram shader;
            shader.VertexSource = Oot3d::IdentifyPicaAotShaderSource(
                effectiveDraw.VertexShaderSource);
            shader.FragmentSource = Oot3d::IdentifyPicaAotShaderSource(
                effectiveDraw.FragmentShaderSource);
            shader.FragmentOutputs = shaderVariant.FragmentOutputs;
            auto preparation = std::make_shared<NativePicaProgramPreparation>();
            preparation->Stem =
                "native_pica_" + std::to_string(effectiveDraw.VertexShaderKey) + "_" +
                std::to_string(effectiveDraw.FragmentShaderKey);
            const auto vertexArtifact = Renderer3ds::FindPicaVertexArtifact(
                effectiveDraw.VertexArtifacts, effectiveDraw.VertexShaderSource);
            if (!vertexArtifact.Spirv.empty()) {
                shader.NriVertexSpirv.assign(vertexArtifact.Spirv.begin(), vertexArtifact.Spirv.end());
                ++mNativeVertexArtifactHits;
            } else {
                preparation->ResolveVertex = true;
                preparation->VertexSource = std::string(effectiveDraw.VertexShaderSource);
            }
            preparation->Nri = mNriPicaPipelineBridge.Available();
            if (preparation->Nri)
                preparation->FragmentSource = std::string(effectiveDraw.FragmentShaderSource);
            preparation->DescriptorSchemaVersion = effectiveDraw.CanonicalDescriptorSchemaVersion;
            preparation->CanonicalPipelineId = effectiveDraw.CanonicalPipelineId;
            preparation->FragmentShaderKey = effectiveDraw.FragmentShaderKey;
            preparation->SettingsRevision = mFrameGraphicsSettingsRevision;
            if (preparation->ResolveVertex || preparation->Nri) {
                if (mPreparationQueue) {
                    preparation->Task = mPreparationQueue->Submit(
                        [this, preparation] { PrepareNativePicaProgram(*preparation); });
                    ++mNativePicaPreparationCounters.ProgramTasks;
                } else {
                    PrepareNativePicaProgram(*preparation);
                }
                shader.Preparation = std::move(preparation);
            }
            shaderIt = nativeShaderCache.emplace(shaderKey, std::move(shader)).first;
        }
        // Inline preparation has nothing to wait for; queued work is adopted
        // (or completed) when the draw selects its program below.
        if (!mPreparationQueue) AdoptNativePicaProgram(shaderIt->second, true);
        finishCpuStage(cpuTimings.ShaderCacheMilliseconds);

        if (!nativeMemoryOnly && mInteractiveGrassProviderPlan.Valid() && opaqueWorldDraw) {
            auto latestGrassPerspective =
                Oot3d::SceneViewRuntime::Instance()
                    .LatestPerspective();
            if (latestGrassPerspective && draw.LightBinding.WorldToView)
                latestGrassPerspective = Oot3d::ResolveSamplePerspective(
                    *latestGrassPerspective, *draw.LightBinding.WorldToView);
            (void)mNativeSceneView.Publish(
                mFrameCounter, mPicaSceneFrame,
                latestGrassPerspective);
            (void)mPicaScenePublications.Publish(
                mFrameCounter, mPicaSceneFrame, mNativeSceneView);
            const auto* grassPerspective =
                mPicaScenePublications.ResolvePerspectiveCamera();
            const auto grassSourceDecision =
                mInteractiveGrassProviderPlan.ResolveInputs({
                    mPicaScenePublications.View(),
                });
            const bool grassSourcePublished =
                grassSourceDecision.Authorized() &&
                grassPerspective != nullptr &&
                PublishNativePicaGrassSurface(
                draw, mFrameCounter,
                mGrassSurfaceOccurrencesThisFrame,
                graphicsSettings.Grass,
                opaqueWorldDraw,
                shaderVariant.GrassTexture0Sampled,
                grassPerspectiveProjection,
                mGrassGeometryRegistry);
            mInteractiveGrassProviderExecution.RecordSourceInspection(
                grassSourceDecision, grassSourcePublished);
        }
        finishCpuStage(cpuTimings.GrassSurfaceMilliseconds);

        auto& renderTarget = GetOrCreateNativePicaRenderTarget(draw);
        finishCpuStage(cpuTimings.RenderTargetStateMilliseconds);
        ExecuteNativePicaBeforeDrawAnchors(renderTarget, draw,
            cpuTimings.DirectionalShadowMilliseconds, cpuTimings.GrassRenderMilliseconds);
        if (collectCpuTimings) cpuStageStart = CpuClock::now();
        renderTarget.WBuffering = draw.WBuffering;
        const bool reflectionEnvironmentEligible =
            sceneDraw &&
            draw.FragmentOperationMode == 0U &&
            draw.DepthTestEnabled && draw.DepthWriteEnabled &&
            (draw.ColorWriteMask & 0x7U) != 0U;
        renderTarget.ReflectionEnvironment.Observe(
            mFrameCounter, draw.FragmentShaderSource,
            draw.FragmentUniformBytes, reflectionEnvironmentEligible, draw.FragmentShaderKey);
        finishCpuStage(cpuTimings.ReflectionEnvironmentMilliseconds);
        cpuTimings.SceneStateMilliseconds =
            cpuTimings.RenderTargetStateMilliseconds +
            cpuTimings.DirectionalShadowMilliseconds +
            cpuTimings.GrassRenderMilliseconds +
            cpuTimings.ReflectionEnvironmentMilliseconds;
        Oot3d::RigidMotionSample rigidMotion{};
        const std::array<float, 2> jitterUv = mTemporalJitterEnabled && !nativeMemoryOnly
            ? std::array<float, 2>{
                  mTemporalJitterPixels[0] /
                      static_cast<float>(renderTarget.Width),
                  mTemporalJitterPixels[1] /
                      static_cast<float>(renderTarget.Height)}
            : std::array<float, 2>{};
        std::array<float, 4> rigidOriginClip{};
        const bool temporalEligible = mTemporalMotionEnabled && !nativeMemoryOnly &&
            sceneDraw &&
            draw.FragmentOperationMode == 0U &&
            draw.DepthTestEnabled && draw.DepthWriteEnabled;
        uint64_t temporalIdentity = draw.GeometryIdentityAvailable ? draw.GeometryIdentity : draw.VertexShaderKey;
        bool repeatedTemporalIdentity = false;
        if (temporalEligible) {
            const auto mixIdentity = [&temporalIdentity](uint64_t value) {
                temporalIdentity ^= value + 0x9e3779b97f4a7c15ULL +
                            (temporalIdentity << 6U) +
                            (temporalIdentity >> 2U);
            };
            if (!draw.GeometryIdentityAvailable) {
                for (const auto& binding : draw.VertexBindings) {
                    if (!binding.PerInstance) {
                        mixIdentity(HashNativeBytes(binding.Bytes));
                    }
                }
                mixIdentity(HashNativeBytes(draw.IndexBytes));
            }
            const uint32_t occurrence =
                mRigidMotionOccurrencesThisFrame[temporalIdentity]++;
            mixIdentity(occurrence);
            repeatedTemporalIdentity = occurrence != 0U;
            mixIdentity(draw.RenderTargetNamespace);
            if (temporalIdentity == 0U) temporalIdentity = 1U;
        }
        const bool rigidEligible = temporalEligible &&
            Oot3d::DecodeCommonRigidOriginClip(
                draw.VertexUniformBytes, draw.FramebufferFlipped,
                rigidOriginClip);
        if (rigidEligible) {
            rigidMotion = mRigidMotionTracker.Track(
                temporalIdentity, mFrameCounter,
                rigidOriginClip, jitterUv);
        }
        std::span<const uint8_t> previousVertexUniformBytes =
            draw.VertexUniformBytes;
        // Owns the previous sample's uniforms for this draw: the history entry
        // is replaced below, which would free the storage under the span.
        std::vector<uint8_t> previousVertexUniformStorage;
        std::array<float, 2> previousJitterNdc{
            2.0F * jitterUv[0], 2.0F * jitterUv[1]};
        bool exactMotionHistory = false;
        if (temporalEligible) {
            const auto previous =
                mPreviousPicaVertexUniforms.find(temporalIdentity);
            if (previous != mPreviousPicaVertexUniforms.end() &&
                previous->second.FrameId + 1U == mFrameCounter &&
                previous->second.Bytes.size() ==
                    draw.VertexUniformBytes.size()) {
                previousVertexUniformStorage = std::move(previous->second.Bytes);
                previousVertexUniformBytes = previousVertexUniformStorage;
                previousJitterNdc = previous->second.JitterNdc;
                exactMotionHistory = true;
            }
            PreviousPicaVertexUniforms next;
            next.FrameId = mFrameCounter;
            next.Bytes.assign(draw.VertexUniformBytes.begin(),
                              draw.VertexUniformBytes.end());
            next.JitterNdc = {
                2.0F * jitterUv[0], 2.0F * jitterUv[1]};
            mPreviousPicaVertexUniforms[temporalIdentity] = std::move(next);
        }
        finishCpuStage(cpuTimings.TemporalStateMilliseconds);
        cpuTimings.DrawStateMilliseconds =
            cpuTimings.GrassSurfaceMilliseconds +
            cpuTimings.SceneStateMilliseconds +
            cpuTimings.TemporalStateMilliseconds;
        const size_t previousPipelineCount = mNativePicaPipelines.size();
        // Hybrid preparation: the specialized program draws once it and its
        // pipeline are ready, the draw's always-available program until then.
        // A draw with neither completes its specialized program here.
        const auto backgroundAcquisition = mPreparationQueue
            ? NativePicaPipelineAcquisition::Background : NativePicaPipelineAcquisition::Blocking;
        const Oot3d::PicaShaderPipelineResult* selectedVariant = &shaderVariant;
        NativePicaShaderProgram* selectedProgram = &shaderIt->second;
        Renderer3ds::PicaDevicePipelineRecord* selectedPipeline = nullptr;
        GfxNativePicaDrawView uberDraw;
        const GfxNativePicaDrawView* selectedDraw = &effectiveDraw;
        if (mNativePicaPreparationMode != NativePicaPreparationMode::Uber &&
            AdoptNativePicaProgram(shaderIt->second, false)) {
            bool specializedPending = false;
            selectedPipeline = &GetOrCreateNativePicaPipeline(
                effectiveDraw, shaderIt->second, shaderVariant.Reactive,
                shaderVariant.Domain, shaderVariant.RequestedFeatures,
                shaderVariant.AppliedFeatures, true, false, false,
                backgroundAcquisition, &specializedPending);
            if (specializedPending) selectedPipeline = nullptr;
        }
        if (selectedPipeline == nullptr && UsesAlwaysAvailablePrograms()) {
            auto unavailable = NativePicaUberUnavailable::NoParametricProgram;
            if (!draw.UberFragmentShaderSource.empty()) {
                // The always-available program executes the specialized
                // variant's decisions: its outputs, features and bindings
                // stay the variant's, only the program differs.
                if (auto* uberProgram = ResolveUberNativePicaProgram(shaderVariant, effectiveDraw, unavailable)) {
                    uberDraw = effectiveDraw;
                    uberDraw.FragmentShaderSource = draw.UberFragmentShaderSource;
                    uberDraw.FragmentShaderKey = draw.UberFragmentShaderKey;
                    uberDraw.FragmentShaderSourceIdentity = draw.UberFragmentShaderSourceIdentity;
                    uberDraw.FragmentShaderHooks = draw.UberFragmentShaderHooks;
                    const auto acquireStart = std::chrono::steady_clock::now();
                    auto& uberPipeline = GetOrCreateNativePicaPipeline(
                        uberDraw, *uberProgram, shaderVariant.Reactive, shaderVariant.Domain,
                        shaderVariant.RequestedFeatures, shaderVariant.AppliedFeatures, true, false, false,
                        NativePicaPipelineAcquisition::Libraries);
                    const double acquired = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - acquireStart).count();
                    mNativePicaPreparationCounters.UberAcquireMilliseconds += acquired;
                    mNativePicaPreparationCounters.MaximumUberAcquireMilliseconds =
                        std::max(mNativePicaPreparationCounters.MaximumUberAcquireMilliseconds, acquired);
                    if (mNriPicaPipelineBridge.OwnedPipelineReady(uberPipeline.Id)) {
                        selectedPipeline = &uberPipeline;
                        selectedProgram = uberProgram;
                        selectedDraw = &uberDraw;
                    } else {
                        unavailable = NativePicaUberUnavailable::PipelineFailed;
                    }
                }
            }
            if (selectedPipeline == nullptr && unavailable == NativePicaUberUnavailable::NoParametricProgram) {
                static const bool diagnostics = [] {
                    const char* value = std::getenv("TRIAEVUM_UBER_DIAGNOSTICS");
                    return value != nullptr && value[0] == '1';
                }();
                if (diagnostics)
                    std::fprintf(stderr,
                        "TRIAEVUM_UBER_NO_PAIR fragment=%016llx bytes=%zu domain=%u op=%u memory_only=%u requested=%08x\n",
                        static_cast<unsigned long long>(draw.FragmentShaderKey), draw.FragmentShaderSource.size(),
                        static_cast<unsigned>(draw.CompositionDomain), static_cast<unsigned>(draw.FragmentOperationMode),
                        nativeMemoryOnly ? 1U : 0U, static_cast<unsigned>(shaderVariant.RequestedFeatures));
            }
            if (selectedPipeline == nullptr) {
                ++mNativePicaPreparationCounters.UberUnavailable[static_cast<size_t>(unavailable)];
            } else {
                auto& counters = mNativePicaPreparationCounters;
                const auto& decisions = shaderVariant.Decisions;
                ++counters.UberDraws;
                counters.UberSurfaceDraws += decisions.Has(Oot3d::PicaEffectDecisions::SurfaceLighting) ? 1U : 0U;
                counters.UberPbrDraws += decisions.Has(Oot3d::PicaEffectDecisions::Pbr) ? 1U : 0U;
                counters.UberReliefDraws += decisions.Has(Oot3d::PicaEffectDecisions::Relief) ? 1U : 0U;
                counters.UberToonDraws += decisions.Has(Oot3d::PicaEffectDecisions::Toon) ? 1U : 0U;
            }
        }
        if (selectedPipeline == nullptr) {
            const auto completionStart = std::chrono::steady_clock::now();
            AdoptNativePicaProgram(shaderIt->second, true);
            selectedPipeline = &GetOrCreateNativePicaPipeline(
                effectiveDraw, shaderIt->second, shaderVariant.Reactive,
                shaderVariant.Domain, shaderVariant.RequestedFeatures,
                shaderVariant.AppliedFeatures);
            if (mPreparationQueue) {
                ++mNativePicaPreparationCounters.CompletedDraws;
                const double milliseconds = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - completionStart).count();
                mNativePicaPreparationCounters.CompleteMilliseconds += milliseconds;
                mNativePicaPreparationCounters.MaximumCompleteMilliseconds =
                    std::max(mNativePicaPreparationCounters.MaximumCompleteMilliseconds, milliseconds);
            }
        } else if (selectedProgram == &shaderIt->second) {
            ++mNativePicaPreparationCounters.SpecializedDraws;
        }
        mDiagnostics.RecordNriPicaShaderContract(
            selectedProgram->NriDescriptorContract,
            mNriPicaPipelineBridge.DescriptorLayoutOwnedByNri());
        // Everything below binds what the selected program declares.
        const auto& drawVariant = *selectedVariant;
        auto& pipelineRecord = *selectedPipeline;
        VkPipeline pipeline = pipelineRecord.Vulkan;
        cpuTimings.PipelineLookupHits = mNativePicaPipelines.size() == previousPipelineCount ? 1U : 0U;
        cpuTimings.PipelineCreations = mNativePicaPipelines.size() != previousPipelineCount ? 1U : 0U;
        cpuTimings.PipelineEntries = static_cast<uint32_t>(mNativePicaPipelines.size());
        finishCpuStage(cpuTimings.PipelineMilliseconds);
        const bool nriOwnedDrawsEnabled =
            mNriPicaPipelineBridge.OwnedDrawsEnabled();
        const bool nriOwnedPipelineReady =
            mNriPicaPipelineBridge.OwnedPipelineReady(pipelineRecord.Id);
        auto nriDrawOwnership = Oot3d::PreparePicaNriDrawOwnership(
            nriOwnedDrawsEnabled, nriOwnedPipelineReady);
        std::vector<Oot3d::PicaNriSourceVertexStream>
            nriDrawSourceStreams;
        std::vector<Oot3d::PicaNriSourceVertexAttribute>
            nriDrawSourceAttributes;
        nriDrawSourceStreams.reserve(draw.VertexBindings.size());
        for (const auto& binding : draw.VertexBindings) {
            nriDrawSourceStreams.push_back(
                { { binding.Binding, binding.ByteStride, binding.PerInstance }, binding.Bytes });
        }
        nriDrawSourceAttributes.reserve(draw.VertexAttributes.size());
        for (const auto& attribute : draw.VertexAttributes) {
            nriDrawSourceAttributes.push_back({ attribute.Location, attribute.Binding,
                                                static_cast<Oot3d::PicaNriVertexScalar>(attribute.Format),
                                                attribute.ComponentCount, attribute.ByteOffset });
        }
        const bool directionalShadowGeometry =
            mDirectionalShadowMapBarrierPlan.Valid() &&
            Oot3d::PicaExtensionPassEnabled(
                mPicaExtensionGraph,
                Oot3d::PicaExtensionPass::DirectionalShadowLighting);
        const auto geometryResolution = mPicaGeometryRegistry.Resolve({
            draw.GeometryIdentity,
            draw.GeometryContentVersion,
            draw.GeometryIdentityAvailable,
            mFrameCounter,
            nriDrawSourceStreams,
            nriDrawSourceAttributes,
            draw.IndexBytes,
            draw.Indexed,
            draw.IndicesAre16Bit,
            draw.VertexCount,
            nriDrawOwnership.Prepared || directionalShadowGeometry || pbrOptions.SeamSoftening,
        });
        if (geometryResolution.Geometry == nullptr) {
            throw std::runtime_error("native PICA geometry preparation failed");
        }
        for (const uint64_t evicted : mPicaGeometryRegistry.TakeEvictedIdentities()) {
            RetireNativePicaGeometry(evicted);
        }
        const auto& geometry = *geometryResolution.Geometry;
        if (nriDrawOwnership.Prepared && !geometry.PackedValid) {
            nriDrawOwnership.Prepared = false;
        }
        Renderer3ds::PicaPbrSeamCache transientSeams;
        const Renderer3ds::PicaPbrSeamData* seamData = nullptr;
        if (pbrOptions.SeamSoftening && geometry.PackedValid &&
            Oot3d::HasPicaShaderInstrumentationFeature(
                drawVariant.AppliedFeatures, Oot3d::PicaShaderInstrumentationFeature::PbrMaterial)) {
            std::array<Oot3d::PicaNriSourceVertexBinding, 16> sourceBindings{};
            if (nriDrawSourceStreams.size() > sourceBindings.size())
                throw std::runtime_error("PICA seam input exceeds native binding count");
            for (size_t i = 0; i < nriDrawSourceStreams.size(); ++i)
                sourceBindings[i] = nriDrawSourceStreams[i].Binding;
            const auto layout = Oot3d::BuildPicaNriVertexInputLayout(
                std::span(sourceBindings).first(nriDrawSourceStreams.size()), nriDrawSourceAttributes);
            std::array<Renderer3ds::PicaSceneVertexBufferBinding, 16> bindings{};
            if (geometry.PackedBindings.size() > bindings.size())
                throw std::runtime_error("PICA seam geometry exceeds native binding count");
            for (size_t i = 0; i < geometry.PackedBindings.size(); ++i) {
                const auto& b = geometry.PackedBindings[i];
                bindings[i] = {b.Binding, b.Offset, b.Size, b.Stride};
            }
            const Renderer3ds::PicaShadowGeometryView view{
                geometry.Payload, std::span(bindings).first(geometry.PackedBindings.size()),
                layout.Attributes, layout.Bindings, 0, geometry.IndexOffset,
                geometry.VertexOrIndexCount, draw.BaseVertex, geometry.Indexed};
            uint32_t booleans = 0;
            std::memcpy(&booleans, draw.VertexUniformBytes.data() +
                Oot3d::kPicaPackedVertexBooleanMaskOffset, sizeof(booleans));
            auto& cache = geometryResolution.Cacheable
                ? mNativePicaGeometryBuffers[geometry.Identity].Seams : transientSeams;
            seamData = &cache.Resolve(view, draw.TemporalVertexProgram.Hooks, booleans,
                draw.VertexUniformBytes.subspan(Oot3d::kPicaPackedVertexFloatOffset),
                draw.FragmentShaderHooks.TextureSample(pbrOptions.TextureSlot)->Coordinate,
                draw.Topology == GfxNativePicaTopology::TriangleStrip,
                geometryResolution.Cacheable ? geometry.ContentVersion ^
                    (geometry.StructuralSignature * 0x9e3779b97f4a7c15ULL) : 0);
            pbrUniforms->SeamSettings[3] = std::bit_cast<float>(seamData->TriangleCount);
        }
        if (materialPreview && materialPreview->SeamSoftening && pbrMaterial) {
            const std::string_view status = !mPbrSeamsAvailable
                ? "Unavailable: GPU fragment primitive IDs are not supported"
                : materialPreview->SeamStrength == 0 ? "Inactive: seam strength is zero"
                : materialPreview->SeamChannels == 0 ? "Inactive: no seam channels selected"
                : pbrUniforms && std::bit_cast<uint32_t>(pbrUniforms->SeamSettings[2]) == 0
                    ? "Inactive: selected channels have no loaded auxiliary maps"
                : seamData ? std::string_view(seamData->Status)
                : "Unavailable: native geometry or shader contract is incompatible";
            ::Oot3d::Renderer::AzaharTexturePackRuntime::Instance().ReportMaterialSeams(
                pbrMaterial->Material->Identity.NativeHash, pbrMaterial->Material->Generation, status);
        }
        const auto geometryStats = mPicaGeometryRegistry.Stats();
        cpuTimings.GeometryRegistryHits = geometryResolution.CacheHit ? 1U : 0U;
        cpuTimings.GeometryRegistryMisses = geometryResolution.Cacheable && !geometryResolution.CacheHit ? 1U : 0U;
        cpuTimings.GeometryDynamicBuilds = geometryResolution.Cacheable ? 0U : 1U;
        cpuTimings.GeometryRegistryEntries =
            static_cast<uint32_t>(std::min<size_t>(geometryStats.Entries, std::numeric_limits<uint32_t>::max()));
        bool nriOwnedDraw = nriDrawOwnership.Prepared;

        auto& frame = mFrameResources[mCurrentFrame];
        VkCommandBuffer commandBuffer = mCommandBuffers[mCurrentFrame];
        const auto placement = PlaceNativePicaGeometry(geometryResolution, &cpuTimings);
        const VkBuffer geometryBuffer = placement.Buffer;
        const VkDeviceSize geometryBufferSize = placement.Size;
        uint8_t* const geometryMappedMemory = placement.Mapped;
        const VkDeviceSize geometryBaseOffset = placement.BaseOffset;
        const bool persistentGeometry = placement.Persistent;

        std::vector<std::pair<uint32_t, VkDeviceSize>> bindingOffsets;
        bindingOffsets.reserve(geometry.SourceBindings.size());
        for (const auto& binding : geometry.SourceBindings) {
            bindingOffsets.emplace_back(binding.Binding, geometryBaseOffset + binding.Offset);
        }
        std::vector<Oot3d::NriPicaVertexBufferBindingDesc>
            nriVertexBindings;
        if (geometry.PackedValid) {
            nriVertexBindings.reserve(geometry.PackedBindings.size());
            for (const auto& binding : geometry.PackedBindings) {
                nriVertexBindings.push_back({
                    binding.Binding,
                    geometryBaseOffset + binding.Offset,
                    binding.Size,
                    binding.Stride,
                });
            }
        }

        const VkDeviceSize indexOffset = geometryBaseOffset + geometry.IndexOffset;
        const uint32_t indexCount = geometry.Indexed ? geometry.VertexOrIndexCount : 0U;

        Oot3d::NriDirectionalShadowHistorySnapshot
            directionalShadowHistory;
        Oot3d::PicaDirectionalShadowReceiverBinding
            directionalShadowBinding{};
        const bool directionalShadowShaderApplied =
            drawVariant.DirectionalShadowEligibility ==
                Oot3d::PicaDirectionalShadowLightingEligibility::VertexPrimary ||
            drawVariant.DirectionalShadowEligibility ==
                Oot3d::PicaDirectionalShadowLightingEligibility::FragmentPrimary;
        if (directionalShadowReceiver &&
            directionalShadowShaderApplied) {
            directionalShadowHistory =
                mNriDirectionalShadowPass.FindLatestHistory(
                    draw.RenderTargetNamespace,
                    draw.FramebufferColorPhysicalAddress,
                    mFrameCounter);
            if (directionalShadowHistory.Valid()) {
                const auto nativeEnvironment =
                    Oot3d::DecodePicaNativeDrawEnvironment(
                        draw.VertexUniformBytes,
                        draw.FragmentUniformBytes,
                        draw.FragmentFeatures);
                Oot3d::DirectionalShadowMatrix receiverViewToWorld;
                if (draw.LightBinding.WorldToView &&
                    Oot3d::InvertDirectionalShadowMatrix(*draw.LightBinding.WorldToView, receiverViewToWorld)) {
                    if (surfaceLighting &&
                        Oot3d::HasPicaShaderInstrumentationFeature(drawVariant.AppliedFeatures,
                            Oot3d::PicaShaderInstrumentationFeature::NativePerPixelLighting) &&
                        (surfaceLighting->EnvironmentParameters[3] != 0.0F ||
                         surfaceLighting->Control[0] != 0U ||
                         surfaceLighting->Control[2] != 0U ||
                         (pbrMaterial != nullptr && pbrOptions.NativeFragmentLighting))) {
                        std::array<Oot3d::PicaSurfaceShadowLight, 12> lights{};
                        if (pbrMaterial != nullptr && pbrOptions.NativeFragmentLighting) {
                            const auto& native = nativeEnvironment.FragmentLighting;
                            for (size_t index = 0; index < native.Lights.size(); ++index) {
                                const auto& source = native.Lights[index];
                                lights[index].DirectionViewTowardSource = source.PositionOrDirectionView;
                                lights[index].ShaderSlot = static_cast<uint8_t>(index);
                                lights[index].Enabled = source.Active && source.ValuesAvailable;
                                lights[index].Point = !native.Layout.Lights[index].Directional;
                            }
                        } else {
                            for (size_t slot = 0; slot < surfaceLighting->Lights.size(); ++slot) {
                                const auto& source = surfaceLighting->Lights[slot];
                                std::copy_n(source.DirectionEnabled.begin(), 3,
                                    lights[slot].DirectionViewTowardSource.begin());
                                lights[slot].ShaderSlot = static_cast<uint8_t>(slot);
                                lights[slot].Enabled = source.DirectionEnabled[3] != 0.0F;
                                lights[slot].Point = source.PositionRadius[3] > 0.0F;
                            }
                        }
                        if (surfaceLighting->EnvironmentParameters[3] != 0.0F) {
                            // Only the authored key casts; native slots retain their own matching.
                            const auto& direction = surfaceLighting->EnvironmentDirections[0];
                            std::copy_n(direction.begin(), 3, lights[8].DirectionViewTowardSource.begin());
                            lights[8].ShaderSlot = 8;
                            lights[8].Enabled = direction[3] > 0.0F;
                        }
                        for (uint32_t slot = 0; slot < surfaceLighting->Control[2]; ++slot) {
                            std::copy_n(surfaceLighting->SceneDirections[slot].begin(), 3,
                                lights[10 + slot].DirectionViewTowardSource.begin());
                            lights[10 + slot].ShaderSlot = static_cast<uint8_t>(10 + slot);
                            lights[10 + slot].Enabled = true;
                        }
                        directionalShadowBinding = Oot3d::BuildPicaSurfaceShadowReceiverBinding(
                            receiverViewToWorld,
                            lights, directionalShadowHistory.State);
                    }
                }
            }
        }
        const auto directionalShadowReceiverClass =
            directionalShadowBinding.Classification;
        const bool directionalShadowHistoryBound =
            directionalShadowBinding.Bound();
        mDiagnostics.RecordNriDirectionalShadowReceiver(
            directionalShadowReceiver && directionalShadowShaderApplied,
            directionalShadowReceiverClass !=
                Oot3d::PicaDirectionalShadowReceiverClass::LightingUnavailable,
            directionalShadowReceiverClass !=
                    Oot3d::PicaDirectionalShadowReceiverClass::LightingUnavailable &&
                directionalShadowReceiverClass !=
                    Oot3d::PicaDirectionalShadowReceiverClass::LightingDisabled,
            directionalShadowReceiverClass ==
                Oot3d::PicaDirectionalShadowReceiverClass::AmbientOnly,
            directionalShadowReceiverClass ==
                Oot3d::PicaDirectionalShadowReceiverClass::DirectUnmatched,
            directionalShadowReceiverClass ==
                Oot3d::PicaDirectionalShadowReceiverClass::DirectMatched,
            directionalShadowHistoryBound);
        const auto& directionalShadowUniforms =
            directionalShadowBinding.Uniforms;
        const auto toonUniforms = Oot3d::PackToonSurfaceParameters(
            graphicsSettings.Effects.Toon, graphicsSettings.Effects.ToonStyle, false);
        // The superset effect program reads its decisions (and the toon
        // parameters) from the toon binding.
        std::optional<Oot3d::PicaUberEffectBlock> effectBlock;
        if (selectedProgram->EffectBlock) {
            std::array<float, 32> toon{};
            static_assert(sizeof(toonUniforms) == sizeof(toon));
            std::memcpy(toon.data(), &toonUniforms, sizeof(toon));
            effectBlock = Oot3d::PackPicaUberEffectBlock(drawVariant.Decisions, toon);
        }

        const bool uniformToon = effectBlock.has_value() || Oot3d::HasPicaShaderInstrumentationFeature(
            drawVariant.AppliedFeatures, Oot3d::PicaShaderInstrumentationFeature::Toon);
        const std::span<const uint8_t> toonBytes = effectBlock
            ? std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&*effectBlock), sizeof(*effectBlock))
            : std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&toonUniforms), sizeof(toonUniforms));
        const bool uniformSurface = Oot3d::HasPicaShaderInstrumentationFeature(
            drawVariant.AppliedFeatures, Oot3d::PicaShaderInstrumentationFeature::NativePerPixelLighting);
        const bool uniformPbr = Oot3d::HasPicaShaderInstrumentationFeature(
            drawVariant.AppliedFeatures, Oot3d::PicaShaderInstrumentationFeature::PbrMaterial);
        const bool uniformShadow = Oot3d::HasPicaShaderInstrumentationFeature(
            drawVariant.AppliedFeatures, Oot3d::PicaShaderInstrumentationFeature::DirectionalShadowLighting);
        const VkDeviceSize directionalShadowUniformSize = uniformShadow
            ? sizeof(directionalShadowUniforms) : draw.FragmentUniformBytes.size();
        const std::array<VkDeviceSize, 7> uniformSizes{
            draw.VertexUniformBytes.size(), previousVertexUniformBytes.size(),
            draw.FragmentUniformBytes.size(), uniformShadow ? sizeof(directionalShadowUniforms) : 0U,
            uniformToon ? toonBytes.size() : 0U,
            uniformSurface ? sizeof(*surfaceLighting) : 0U,
            uniformPbr ? sizeof(*pbrUniforms) : 0U};
        VkDeviceSize uniformBytes = 0;
        for (const auto size : uniformSizes)
            uniformBytes += (size + mUniformBufferAlignment - 1U) & ~(mUniformBufferAlignment - 1U);
        const auto seamBytes = seamData && !seamData->Words.empty()
            ? std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(seamData->Words.data()),
                                      seamData->Words.size() * sizeof(seamData->Words[0]))
            : std::span<const uint8_t>{};
        uniformBytes += (seamBytes.size() + mUniformBufferAlignment - 1U) & ~(mUniformBufferAlignment - 1U);
        EnsureFrameUniformCapacity(uniformBytes, mUniformBufferAlignment);

        const auto uploadUniform = [&](std::span<const uint8_t> bytes) {
            const VkDeviceSize offset =
                (frame.UniformBytesUsed + mUniformBufferAlignment - 1U) &
                ~(mUniformBufferAlignment - 1U);
            if (bytes.empty() || offset + bytes.size() >
                                     frame.UniformBuffer.Size) {
                throw std::runtime_error(
                    "native PICA per-frame uniform arena exhausted");
            }
            std::memcpy(static_cast<uint8_t*>(frame.UniformBuffer.Mapped) +
                            offset,
                        bytes.data(), bytes.size());
            frame.UniformBytesUsed = offset + bytes.size();
            return offset;
        };
        const VkDeviceSize vertexUniformOffset =
            uploadUniform(draw.VertexUniformBytes);
        const VkDeviceSize previousVertexUniformOffset =
            uploadUniform(previousVertexUniformBytes);
        const VkDeviceSize fragmentUniformOffset =
            uploadUniform(draw.FragmentUniformBytes);
        const VkDeviceSize directionalShadowUniformOffset = uniformShadow
            ? uploadUniform(std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(&directionalShadowUniforms),
                sizeof(directionalShadowUniforms)))
            : fragmentUniformOffset;
        // Inactive extension bindings reuse an existing range; no upload.
        const VkDeviceSize toonUniformOffset = uniformToon
            ? uploadUniform(toonBytes)
            : directionalShadowUniformOffset;
        const VkDeviceSize toonUniformSize = uniformToon
            ? toonBytes.size() : directionalShadowUniformSize;
        VkDeviceSize surfaceUniformOffset = directionalShadowUniformOffset;
        if (uniformSurface) {
            // Compare host-cached state, not potentially write-combined mapped
            // upload memory. The offset belongs to this frame and arena only.
            if (!frame.SurfaceLightingUniformOffset ||
                std::memcmp(&frame.SurfaceLightingUniforms, &*surfaceLighting, sizeof(*surfaceLighting)) != 0) {
                frame.SurfaceLightingUniformOffset = uploadUniform(std::span<const uint8_t>(
                    reinterpret_cast<const uint8_t*>(&*surfaceLighting), sizeof(*surfaceLighting)));
                frame.SurfaceLightingUniforms = *surfaceLighting;
            }
            surfaceUniformOffset = *frame.SurfaceLightingUniformOffset;
        }
        const VkDeviceSize surfaceUniformSize = uniformSurface
            ? sizeof(*surfaceLighting) : directionalShadowUniformSize;
        const VkDeviceSize pbrUniformOffset = uniformPbr
            ? uploadUniform(std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(&pbrUniforms.value()), sizeof(*pbrUniforms)))
            : directionalShadowUniformOffset;
        const VkDeviceSize pbrUniformSize = uniformPbr ? sizeof(*pbrUniforms) : directionalShadowUniformSize;
        const VkDeviceSize seamOffset = seamBytes.empty() ? fragmentUniformOffset : uploadUniform(seamBytes);
        const VkDeviceSize seamSize = seamBytes.empty() ? 16U : seamBytes.size();
        finishCpuStage(cpuTimings.UploadMilliseconds);

        const VkDescriptorSet descriptor = AllocateNativePicaDescriptorSet();
        const VkDescriptorBufferInfo vertexUniform{
            frame.UniformBuffer.Buffer, vertexUniformOffset,
            draw.VertexUniformBytes.size()};
        const VkDescriptorBufferInfo fragmentUniform{
            frame.UniformBuffer.Buffer, fragmentUniformOffset,
            draw.FragmentUniformBytes.size()};
        const VkDescriptorBufferInfo previousVertexUniform{
            frame.UniformBuffer.Buffer, previousVertexUniformOffset,
            previousVertexUniformBytes.size()};
        const VkDescriptorBufferInfo directionalShadowUniform{
            frame.UniformBuffer.Buffer,
            directionalShadowUniformOffset,
            directionalShadowUniformSize};
        const VkDescriptorBufferInfo toonUniform{
            frame.UniformBuffer.Buffer, toonUniformOffset, toonUniformSize};
        const VkDescriptorBufferInfo surfaceUniform{
            frame.UniformBuffer.Buffer, surfaceUniformOffset, surfaceUniformSize};
        const VkDescriptorBufferInfo pbrUniform{
            frame.UniformBuffer.Buffer, pbrUniformOffset, pbrUniformSize};
        const VkDescriptorBufferInfo seamBuffer{frame.UniformBuffer.Buffer, seamOffset, seamSize};
        std::array<VkDescriptorImageInfo, Renderer3ds::kPicaPbrTextureCount> pbrImageInfos{};
        for (size_t role = 0; role < pbrImageInfos.size(); ++role)
            pbrImageInfos[role] = {pbrSampler, pbrTextures[role]->View, pbrTextures[role]->ImageLayout};
        std::array<VkDescriptorImageInfo, 3> imageInfos{};
        for (size_t slot = 0; slot < imageInfos.size(); ++slot) {
            imageInfos[slot] = {nativeTextures[slot]->Sampler, textures[slot]->View,
                                textures[slot]->ImageLayout};
        }
        const VkDescriptorImageInfo shadowStorage{
            VK_NULL_HANDLE, renderTarget.ShadowView, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo directionalShadowImage{
            directionalShadowHistoryBound
                ? directionalShadowHistory.Sampler
                : mFallbackTexture.Sampler,
            directionalShadowHistoryBound
                ? directionalShadowHistory.View
                : mFallbackTexture.View,
            directionalShadowHistoryBound
                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                : mFallbackTexture.ImageLayout};
        const VkDescriptorImageInfo lightingLutImage{
            lightingLutTexture->Sampler,
            lightingLutTexture->View,
            lightingLutTexture->ImageLayout};
        std::array<VkWriteDescriptorSet, 14 + Renderer3ds::kPicaPbrTextureCount> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     descriptor, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                     nullptr, &vertexUniform, nullptr};
        for (uint32_t slot = 0; slot < 3; ++slot) {
            writes[slot + 1U] = {
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor,
                slot + 1U, 0, 1,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                &imageInfos[slot], nullptr, nullptr};
        }
        writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     descriptor, 4, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                     nullptr, &fragmentUniform, nullptr};
        writes[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     descriptor, 5, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                     &shadowStorage, nullptr, nullptr};
        writes[6] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     descriptor, 6, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                     nullptr, &previousVertexUniform, nullptr};
        writes[7] = {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            descriptor, 10, 0, 1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            &directionalShadowImage, nullptr, nullptr};
        writes[8] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     descriptor, 12, 0, 1,
                     VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                     nullptr, &directionalShadowUniform, nullptr};
        writes[9] = {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            descriptor, 13, 0, 1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            &lightingLutImage, nullptr, nullptr};
        writes[10] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                      descriptor, 15, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                      nullptr, &toonUniform, nullptr};
        writes[11] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                      descriptor, 16, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                      nullptr, &surfaceUniform, nullptr};
        writes[12] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                      descriptor, 17, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                      nullptr, &pbrUniform, nullptr};
        for (uint32_t role = 0; role < pbrImageInfos.size(); ++role)
            writes[13U + role] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                descriptor, 18U + role, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                &pbrImageInfos[role], nullptr, nullptr};
        writes.back() = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            descriptor, 26, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &seamBuffer, nullptr};
        vkUpdateDescriptorSets(mDevice, static_cast<uint32_t>(writes.size() - (pbrOptions.SeamSoftening ? 0U : 1U)),
                               writes.data(), 0, nullptr);
        finishCpuStage(cpuTimings.DescriptorMilliseconds);

        const bool samplesLiveShadow = std::any_of(
            textures.begin(), textures.end(), [](const TextureRecord* texture) {
                return texture != nullptr && texture->LiveRenderTargetAlias;
            });
        if (samplesLiveShadow) {
            EndNativePicaRenderPass();
            VkMemoryBarrier shadowVisibility{
                VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            shadowVisibility.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            shadowVisibility.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(commandBuffer,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                                 &shadowVisibility, 0, nullptr, 0, nullptr);
        }
        BeginNativePicaRenderPass(renderTarget);
        ApplyPendingNativePicaMemoryFills(renderTarget);
        // Preparation is independent of the render scope. Ownership must be
        // finalized only after the current target's scope has been opened;
        // checking the previous scope here used to force its first draw
        // through the Vulkan fallback.
        nriDrawOwnership = Oot3d::FinalizePicaNriDrawOwnership(
            nriDrawOwnership,
            mPicaDynamicRenderingScope.ActiveOwnedByNri());
        nriOwnedDraw = nriDrawOwnership.SubmitOwned;
        if (!nriOwnedDraw &&
            !Oot3d::PicaNriVulkanFallbackAllowed(
                nriDrawOwnership)) {
            throw std::runtime_error(
                "NRI PICA draw ownership is unavailable in an "
                "NRI-owned rendering scope [owned_draws=" +
                std::to_string(nriOwnedDrawsEnabled) +
                ", owned_pipeline=" +
                std::to_string(nriOwnedPipelineReady) +
                ", packed_geometry=" +
                std::to_string(geometry.PackedValid) +
                ", submission_id=" +
                std::to_string(draw.SubmissionId) +
                ", vertex_shader=" +
                std::to_string(draw.VertexShaderKey) +
                ", fragment_shader=" +
                std::to_string(draw.FragmentShaderKey) +
                ", packed_error=" + geometry.PackedError + "]");
        }
        // A UI-classified draw that renders into the colour target the frame's
        // scene draws use **and** covers that whole target is not a placed UI
        // element: it is two-dimensional frame content the title composites over
        // the whole native screen (movie frames, full-frame effect re-draws,
        // screen-space post-process). It is composed like the scene so the
        // title's own effect reaches the whole frame and our recomposition stays
        // the single owner of the output. Placed elements - HUD, menus, the
        // file-select screen - stay `Ui` and keep the uniform native canvas that
        // their layout and pointer mapping depend on.
        const float viewportRight = draw.ViewportX + draw.ViewportWidth;
        const float viewportBottom = draw.ViewportY + draw.ViewportHeight;
        const bool coversWholeTarget =
            draw.ViewportX <= 0.0F && draw.ViewportY <= 0.0F &&
            viewportRight >= static_cast<float>(draw.FramebufferWidth) &&
            viewportBottom >= static_cast<float>(draw.FramebufferHeight);
        const auto compositionDomain =
            draw.CompositionDomain == Renderer3ds::PicaCompositionDomain::Ui &&
                    coversWholeTarget &&
                    mPicaSceneFrame.HasSceneDrawOnColorTarget(
                        draw.FramebufferColorPhysicalAddress)
                ? Renderer3ds::PicaCompositionDomain::Screen
                : draw.CompositionDomain;
        const auto rasterCanvas = ::Oot3d::Renderer::ResolvePicaRasterCanvas(
            compositionDomain, draw.FramebufferWidth, draw.FramebufferHeight,
            renderTarget.Width, renderTarget.Height);
        const float framebufferScaleX = rasterCanvas.ScaleX;
        const float framebufferScaleY = rasterCanvas.ScaleY;
        mViewport = {rasterCanvas.X + draw.ViewportX * framebufferScaleX,
                     rasterCanvas.Y + draw.ViewportY * framebufferScaleY,
                     draw.ViewportWidth * framebufferScaleX,
                     draw.ViewportHeight * framebufferScaleY, 0.0F, 1.0F};
        if (draw.ScissorMode == 3U) {
            const uint32_t x1 = draw.ScissorX1;
            const uint32_t y1 = draw.ScissorY1;
            const uint32_t x2 = static_cast<uint32_t>(draw.ScissorX2) + 1U;
            const uint32_t y2 = static_cast<uint32_t>(draw.ScissorY2) + 1U;
            mScissor = {
                {static_cast<int32_t>(rasterCanvas.X + static_cast<float>(x1) *
                                      framebufferScaleX),
                 static_cast<int32_t>(rasterCanvas.Y + static_cast<float>(y1) *
                                      framebufferScaleY)},
                {static_cast<uint32_t>(static_cast<float>(
                     x2 > x1 ? x2 - x1 : 0U) * framebufferScaleX),
                 static_cast<uint32_t>(static_cast<float>(
                     y2 > y1 ? y2 - y1 : 0U) * framebufferScaleY)}};
        } else {
            mScissor = {{0, 0},
                        {renderTarget.Width, renderTarget.Height}};
        }
        mViewportSet = true;
        mScissorSet = true;
        // Resolved-colour provenance of the target this draw writes, evaluated
        // here because only now are the resolved texture records, the effective
        // typed composition domain and the effective raster extent all known.
        // The conditions are: the draw is composed as full-frame screen content,
        // its effective viewport and scissor cover the whole target, every
        // non-fallback texture it samples is a resolved copy record, and the UV
        // footprint it samples lies inside that record's owned (producer-covered)
        // rectangle - so the pixels actually written are resolved copy pixels,
        // not the CPU-owned remainder of a padded texture. Nothing here uses the
        // raw draw viewport as a geometry proof, and a tiny mesh cannot qualify.
        {
            // Native UI consumes the completed scene; its pixels must neither
            // invalidate that completion nor become inputs to scene effects.
            const bool retainResolvedScene =
                draw.CompositionDomain == Renderer3ds::PicaCompositionDomain::Ui &&
                renderTarget.SceneEffectsResolvedSerial != 0U &&
                renderTarget.SceneEffectsResolvedSerial == renderTarget.ContentWriteSerial;
            const bool retainSceneBeforeUi =
                draw.CompositionDomain == Renderer3ds::PicaCompositionDomain::Ui &&
                renderTarget.SceneBeforeUiSerial != 0U &&
                renderTarget.SceneBeforeUiSerial == renderTarget.ContentWriteSerial;
            renderTarget.ContentWriteSerial = ++mGpuTextureCopyWriteSerial;
            if (retainResolvedScene)
                renderTarget.SceneEffectsResolvedSerial = renderTarget.ContentWriteSerial;
            if (retainSceneBeforeUi)
                renderTarget.SceneBeforeUiSerial = renderTarget.ContentWriteSerial;
            const bool fullTargetRaster =
                compositionDomain ==
                    ::Fast::Renderer3ds::PicaCompositionDomain::Screen &&
                mViewport.x <= 0.0F && mViewport.y <= 0.0F &&
                mViewport.x + static_cast<float>(mViewport.width) >=
                    static_cast<float>(renderTarget.Width) &&
                mViewport.y + static_cast<float>(mViewport.height) >=
                    static_cast<float>(renderTarget.Height) &&
                mScissor.offset.x <= 0 &&
                mScissor.offset.y <= 0 &&
                static_cast<uint32_t>(mScissor.offset.x) + mScissor.extent.width >=
                    renderTarget.Width &&
                static_cast<uint32_t>(mScissor.offset.y) + mScissor.extent.height >=
                    renderTarget.Height;
            if (fullTargetRaster) {
                bool sampledResolvedCopy = false;
                bool resolvedContentDraw = true;
                for (const TextureRecord* sampled : textures) {
                    if (sampled == nullptr || sampled == &mFallbackTexture) {
                        continue;
                    }
                    if (sampled->SourceResolvedColour == false ||
                        SampledFootprintInsideOwnedRect(draw, *sampled) == false) {
                        resolvedContentDraw = false;
                        break;
                    }
                    sampledResolvedCopy = true;
                }
                if (resolvedContentDraw && sampledResolvedCopy) {
                    // This draw's own write is resolved content, so the target's
                    // current content version carries the resolved scene effects;
                    // any later draw, clear or fill of it bumps the write serial
                    // and the accessor answers false again.
                    renderTarget.SceneEffectsResolvedSerial =
                        renderTarget.ContentWriteSerial;
                }
            }
        }
        if (compositionDomain == Renderer3ds::PicaCompositionDomain::Ui) {
            const auto clip = Renderer3ds::ClipToPicaCanvas(
                {mScissor.offset.x, mScissor.offset.y,
                 mScissor.extent.width, mScissor.extent.height},
                rasterCanvas, draw.FramebufferWidth, draw.FramebufferHeight);
            mScissor = {{clip.X, clip.Y}, {clip.Width, clip.Height}};
        }
        struct NativeDrawPush {
            std::array<float, 4> RigidMotion{};
            std::array<float, 4> Jitter{};
        } nativeDrawPush;
        nativeDrawPush.RigidMotion = {
            rigidMotion.MotionUv[0], rigidMotion.MotionUv[1],
            rigidMotion.Valid ? 1.0F : 0.0F,
            exactMotionHistory ? 1.0F : 0.0F};
        nativeDrawPush.Jitter = {
            2.0F * jitterUv[0], 2.0F * jitterUv[1],
            previousJitterNdc[0], previousJitterNdc[1]};
        if (temporalEligible)
            mDiagnostics.RecordTemporalDraw(
                rigidMotion.Valid, exactMotionHistory,
                exactMotionHistory && !rigidEligible, repeatedTemporalIdentity);

        Oot3d::NriPicaOwnedDrawDesc nriDraw;
        Oot3d::PicaNriVertexInputLayout sceneVertexLayout;
        if (nriOwnedDraw) {
            nriDraw.FrameIndex = mCurrentFrame;
            nriDraw.FrameId = mFrameCounter;
            nriDraw.PipelineId = pipelineRecord.Id;
            nriDraw.UniformBuffer = frame.UniformBuffer.Buffer;
            nriDraw.UniformBufferSize = frame.UniformBuffer.Size;
            nriDraw.UniformMappedMemory =
                static_cast<uint8_t*>(frame.UniformBuffer.Mapped);
            nriDraw.SeamGeometry = {seamOffset, pbrOptions.SeamSoftening ? seamSize : 0U};
            nriDraw.Uniforms = {{
                {vertexUniformOffset,
                 draw.VertexUniformBytes.size()},
                {fragmentUniformOffset,
                 draw.FragmentUniformBytes.size()},
                {previousVertexUniformOffset,
                 previousVertexUniformBytes.size()},
                {directionalShadowUniformOffset,
                 directionalShadowUniformSize},
                {toonUniformOffset, toonUniformSize},
                {surfaceUniformOffset, surfaceUniformSize},
                {pbrUniformOffset, pbrUniformSize},
            }};
            nriDraw.VertexBuffer = geometryBuffer;
            nriDraw.VertexBufferSize = geometryBufferSize;
            nriDraw.VertexMappedMemory = geometryMappedMemory;
            nriDraw.VertexContentPersistent = persistentGeometry;
            nriDraw.VertexBindings = nriVertexBindings;
            nriDraw.Indexed = draw.Indexed;
            nriDraw.IndexOffset = indexOffset;
            nriDraw.VertexOrIndexCount =
                draw.Indexed ? indexCount : draw.VertexCount;
            nriDraw.BaseVertex = draw.BaseVertex;
            const auto bindTexture = [](Oot3d::NriPicaTextureBindingDesc& destination,
                                        const TextureRecord& texture, const TextureRecord& sampler,
                                        VkFormat format, bool integer) {
                destination.Image = texture.NriImage;
                destination.Format = format;
                destination.Usage = VK_IMAGE_USAGE_SAMPLED_BIT;
                if (texture.ImageLayout == VK_IMAGE_LAYOUT_GENERAL)
                    destination.Usage |= VK_IMAGE_USAGE_STORAGE_BIT;
                destination.Width = texture.Width;
                destination.Height = texture.Height;
                destination.MipLevels = texture.MipLevels;
                destination.MinFilter = ToNativePicaVkFilter(sampler.SamplerState.MinFilter);
                destination.MagFilter = ToNativePicaVkFilter(sampler.SamplerState.MagFilter);
                destination.MipmapMode = ToNativePicaVkMipmapMode(sampler.SamplerState.MinFilter);
                destination.AddressU = ToNativePicaVkAddressMode(sampler.SamplerState.WrapS);
                destination.AddressV = ToNativePicaVkAddressMode(sampler.SamplerState.WrapT);
                destination.MipBias = sampler.SamplerState.LodBias;
                const uint32_t highestMip = texture.UploadedMipLevels > 0U ? texture.UploadedMipLevels - 1U : 0U;
                destination.MaxLod = static_cast<float>(std::min(sampler.SamplerState.MaxMipLevel, highestMip));
                destination.MinLod = static_cast<float>(std::min(sampler.SamplerState.MinMipLevel, highestMip));
                destination.Integer = integer;
            };
            for (size_t slot = 0; slot < nriDraw.Textures.size(); ++slot)
                bindTexture(nriDraw.Textures[slot], *textures[slot], *nativeTextures[slot],
                    textureFormats[slot], integerTextures[slot]);
            for (size_t role = 0; role < nriDraw.PbrTextures.size(); ++role)
                bindTexture(nriDraw.PbrTextures[role], *pbrTextures[role],
                    pbrMaterial ? *nativeTextures[pbrOptions.TextureSlot] : mFallbackTexture,
                    VK_FORMAT_R8G8B8A8_UNORM, false);
            {
                auto& destination =
                    nriDraw.DirectionalShadowTexture;
                destination.Image = directionalShadowHistoryBound
                    ? directionalShadowHistory.Image
                    : mFallbackTexture.NriImage;
                destination.Format = directionalShadowHistoryBound
                    ? directionalShadowHistory.Format
                    : VK_FORMAT_R8G8B8A8_UNORM;
                destination.Usage = VK_IMAGE_USAGE_SAMPLED_BIT;
                destination.Width = directionalShadowHistoryBound
                    ? directionalShadowHistory.State.Virtual.AtlasResolution
                    : mFallbackTexture.Width;
                destination.Height = directionalShadowHistoryBound
                    ? directionalShadowHistory.State.Virtual.AtlasResolution
                    : mFallbackTexture.Height;
                destination.MipLevels = 1U;
                destination.MinFilter = VK_FILTER_NEAREST;
                destination.MagFilter = VK_FILTER_NEAREST;
                destination.MipmapMode =
                    VK_SAMPLER_MIPMAP_MODE_NEAREST;
                destination.AddressU =
                    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                destination.AddressV =
                    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                destination.MinLod = 0.0F;
                destination.MaxLod = 0.0F;
                destination.Integer = false;
            }
            {
                // The cube atlas only where the variant reads it.
                const bool pointShadowBound = pointShadowHistory.Valid() &&
                    Oot3d::HasPicaShaderInstrumentationFeature(
                        drawVariant.AppliedFeatures, Oot3d::PicaShaderInstrumentationFeature::PointShadowLighting);
                if (pointShadowReceiver)
                    mDiagnostics.RecordNriPointShadowReceiver(pointShadowBound);
                auto& destination = nriDraw.PointShadowTexture;
                destination.Image = pointShadowBound ? pointShadowHistory.Image : mFallbackTexture.NriImage;
                destination.Format = pointShadowBound ? pointShadowHistory.Format : VK_FORMAT_R8G8B8A8_UNORM;
                destination.Usage = VK_IMAGE_USAGE_SAMPLED_BIT;
                destination.Width = pointShadowBound ? pointShadowHistory.Layout.Width : mFallbackTexture.Width;
                destination.Height = pointShadowBound ? pointShadowHistory.Layout.Height : mFallbackTexture.Height;
                destination.MipLevels = 1U;
                destination.MinFilter = VK_FILTER_NEAREST;
                destination.MagFilter = VK_FILTER_NEAREST;
                destination.MipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                destination.AddressU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                destination.AddressV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                destination.MinLod = 0.0F;
                destination.MaxLod = 0.0F;
                destination.Integer = false;
            }
            {
                auto& destination = nriDraw.LightingLutTexture;
                destination.Image = lightingLutTexture->NriImage;
                destination.Format = lightingLutInteger
                    ? VK_FORMAT_R32_UINT
                    : VK_FORMAT_R8G8B8A8_UNORM;
                destination.Usage = VK_IMAGE_USAGE_SAMPLED_BIT;
                destination.Width = lightingLutTexture->Width;
                destination.Height = lightingLutTexture->Height;
                destination.MipLevels = 1U;
                destination.MinFilter = VK_FILTER_NEAREST;
                destination.MagFilter = VK_FILTER_NEAREST;
                destination.MipmapMode =
                    VK_SAMPLER_MIPMAP_MODE_NEAREST;
                destination.AddressU =
                    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                destination.AddressV =
                    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                destination.MinLod = 0.0F;
                destination.MaxLod = 0.0F;
                destination.Integer = lightingLutInteger;
            }
            nriDraw.StorageImage = renderTarget.ShadowImage;
            nriDraw.StorageFormat = VK_FORMAT_R32_UINT;
            nriDraw.StorageWidth = renderTarget.Width;
            nriDraw.StorageHeight = renderTarget.Height;
            std::copy(nativeDrawPush.RigidMotion.begin(),
                      nativeDrawPush.RigidMotion.end(),
                      nriDraw.RootConstants.begin());
            std::copy(nativeDrawPush.Jitter.begin(),
                      nativeDrawPush.Jitter.end(),
                      nriDraw.RootConstants.begin() + 4);
            nriDraw.Viewport = mViewport;
            nriDraw.Scissor = mScissor;
            std::copy(std::begin(draw.Blend.ConstantColor),
                      std::end(draw.Blend.ConstantColor),
                      nriDraw.BlendConstants.begin());
            nriDraw.StencilTest = draw.Stencil.Enabled;
            nriDraw.StencilReference = draw.Stencil.Reference;

            std::vector<Oot3d::PicaNriSourceVertexBinding> shadowSourceBindings;
            shadowSourceBindings.reserve(draw.VertexBindings.size());
            for (const auto& binding : draw.VertexBindings) {
                shadowSourceBindings.push_back({ binding.Binding, binding.ByteStride, binding.PerInstance });
            }
            sceneVertexLayout =
                Oot3d::BuildPicaNriVertexInputLayout(shadowSourceBindings, nriDrawSourceAttributes);
        }
        const bool directionalShadowCaster =
            nriOwnedDraw && opaqueWorldDraw && sceneVertexLayout.Valid() &&
            !sceneVertexLayout.Bindings.empty() &&
            !sceneVertexLayout.Attributes.empty() &&
            mDirectionalShadowMapBarrierPlan.Valid() &&
            Oot3d::PicaExtensionPassEnabled(
                mPicaExtensionGraph,
                Oot3d::PicaExtensionPass::DirectionalShadowLighting) &&
            AdmitsOpaqueSceneGeometry(draw);
        if (nriOwnedDraw && !nativeMemoryOnly) {
            std::vector<Oot3d::PicaSceneVertexBufferBinding>
                sceneVertexBindings;
            sceneVertexBindings.reserve(nriVertexBindings.size());
            for (const auto& binding : nriVertexBindings) {
                sceneVertexBindings.push_back(
                    { binding.Binding, binding.Offset, binding.Size,
                      binding.Stride });
            }
            Renderer3ds::VirtualShadowCaster shadowCaster;
            Oot3d::DirectionalShadowMatrix shadowViewToWorld;
            const auto prepared = mPreparedShadowCasters.find(draw.SubmissionId);
            if (directionalShadowCaster && prepared != mPreparedShadowCasters.end()) {
                // Prepared from the same inputs for this sample's map.
                shadowCaster = prepared->second;
                mDiagnostics.RecordNriDirectionalShadowPreparedReuse();
            } else if (directionalShadowCaster && draw.LightBinding.WorldToView &&
                draw.VertexUniformBytes.size() >= Oot3d::kPicaPackedVertexFloatOffset &&
                Oot3d::InvertDirectionalShadowMatrix(*draw.LightBinding.WorldToView, shadowViewToWorld)) {
                uint32_t booleanUniforms = 0;
                std::memcpy(&booleanUniforms, draw.VertexUniformBytes.data() +
                            Oot3d::kPicaPackedVertexBooleanMaskOffset, sizeof(booleanUniforms));
                Renderer3ds::PicaShadowCasterCache transientCache;
                auto& shadowCache = geometryResolution.Cacheable
                    ? mNativePicaGeometryBuffers.at(geometry.Identity).ShadowCaster : transientCache;
                const uint64_t version = geometryResolution.Cacheable
                    ? geometry.ContentVersion ^ (geometry.StructuralSignature * 0x9e3779b97f4a7c15ULL) ^
                      geometry.Identity ^ draw.VertexShaderKey : 0;
                shadowCaster = Renderer3ds::BuildPicaShadowCaster(
                    {geometry.Payload, sceneVertexBindings, sceneVertexLayout.Attributes, sceneVertexLayout.Bindings,
                     geometryBaseOffset, geometry.IndexOffset, geometry.VertexOrIndexCount,
                     draw.BaseVertex, geometry.Indexed},
                    draw.TemporalVertexProgram.Hooks, booleanUniforms,
                    draw.VertexUniformBytes.subspan(Oot3d::kPicaPackedVertexFloatOffset),
                    shadowViewToWorld, version, shadowCache);
            }
            shadowCaster.Dynamic = draw.Composition.NativeActorGeometry;
            shadowCaster.OwnerIdentity = draw.Composition.NativeActorIdentity;
            if (opaqueWorldDraw && draw.LightBinding.WorldToView)
                renderTarget.SampleView.Observe(mFrameCounter, *draw.LightBinding.WorldToView);
            // Native frame geometry for the scene distance field: the shadow
            // caster admission, from the first non-synthetic execution.
            const bool sceneDistanceFieldGeometry =
                Oot3d::PicaExtensionPassEnabled(
                    mPicaExtensionGraph, Oot3d::PicaExtensionPass::SceneDistanceField) &&
                opaqueWorldDraw && sceneVertexLayout.Valid() && !sceneVertexLayout.Bindings.empty() &&
                AdmitsOpaqueSceneGeometry(draw) && draw.LightBinding.WorldToView &&
                draw.VertexUniformBytes.size() >= Oot3d::kPicaPackedVertexFloatOffset;
            if (sceneDistanceFieldGeometry) {
                if (!mBrixelizerScene.Available() && !mBrixelizerInitializationAttempted) {
                    mBrixelizerInitializationAttempted = true;
                    if (!mBrixelizerScene.Initialize(mPhysicalDevice, mDevice, kFramesInFlight))
                        std::fprintf(stderr, "TRIAEVUM_SCENE_DISTANCE_FIELD unavailable=\"%s\"\n",
                                     mBrixelizerScene.UnavailableReason().c_str());
                }
                const auto& sample = mPicaSceneFrame.TemporalSample();
                Oot3d::DirectionalShadowMatrix giViewToWorld;
                if (!sample.Synthetic &&
                    mBrixelizerScene.AcceptsExecution(sample.CurrentSourceFrameId, draw.RenderTargetNamespace) &&
                    Oot3d::InvertDirectionalShadowMatrix(*draw.LightBinding.WorldToView, giViewToWorld)) {
                    uint32_t booleanUniforms = 0;
                    std::memcpy(&booleanUniforms, draw.VertexUniformBytes.data() +
                                Oot3d::kPicaPackedVertexBooleanMaskOffset, sizeof(booleanUniforms));
                    Renderer3ds::PicaGiGeometryCache transientGiCache;
                    auto& giCache = geometryResolution.Cacheable
                        ? mNativePicaGeometryBuffers.at(geometry.Identity).GiGeometry : transientGiCache;
                    const uint64_t giVersion = geometryResolution.Cacheable
                        ? geometry.ContentVersion ^ (geometry.StructuralSignature * 0x9e3779b97f4a7c15ULL) ^
                          geometry.Identity ^ draw.VertexShaderKey : 0;
                    const auto giInstance = Renderer3ds::BuildPicaGiInstance(
                        {geometry.Payload, sceneVertexBindings, sceneVertexLayout.Attributes,
                         sceneVertexLayout.Bindings, geometryBaseOffset, geometry.IndexOffset,
                         geometry.VertexOrIndexCount, draw.BaseVertex, geometry.Indexed},
                        ToPicaSceneTopology(draw.Topology), draw.TemporalVertexProgram.Hooks, booleanUniforms,
                        draw.VertexUniformBytes.subspan(Oot3d::kPicaPackedVertexFloatOffset),
                        giViewToWorld, giVersion, giCache);
                    mBrixelizerScene.Submit(giInstance);
                }
            }
            std::array<Oot3d::PicaSceneTextureBinding, 3>
                sceneTextures{};
            for (size_t slot = 0; slot < sceneTextures.size(); ++slot) {
                const auto* texture = textures[slot];
                auto& destination = sceneTextures[slot];
                destination.NativeImageHandle =
                    ToPicaSceneNativeHandle(texture->NriImage);
                destination.ReplacementContentHash =
                    texture->CustomReplacementPixelHash;
                destination.ImageWidth = texture->Width;
                destination.ImageHeight = texture->Height;
                destination.MipLevels = texture->UploadedMipLevels;
                destination.Slot = static_cast<uint8_t>(slot);
                destination.ImageFormat = integerTextures[slot]
                    ? Oot3d::PicaSceneTextureImageFormat::R32Uint
                    : Oot3d::PicaSceneTextureImageFormat::Rgba8;
                destination.CustomReplacement =
                    texture->CustomReplacementApplied;
            }
            for (const auto& texture : draw.Textures) {
                auto& destination = sceneTextures[texture.Slot];
                destination.NativeContentHash =
                    texture.NativeContentHash;
                destination.NativeBaseLevelContentHash =
                    texture.NativeBaseLevelContentHash;
                destination.PhysicalAddress = texture.PhysicalAddress;
                destination.SourceWidth = texture.Width;
                destination.SourceHeight = texture.Height;
                destination.LodBiasRaw = texture.LodBiasRaw;
                destination.NativeFormat = texture.NativeFormat;
                destination.NativeType = texture.NativeType;
                destination.NativeWrapS = texture.NativeWrapS;
                destination.NativeWrapT = texture.NativeWrapT;
                destination.MinMipLevel = texture.MinMipLevel;
                destination.MaxMipLevel = texture.MaxMipLevel;
                destination.Bound = true;
                destination.NativeContentHashAvailable =
                    texture.NativeContentHashAvailable;
                destination.NativeBaseLevelContentHashAvailable =
                    texture.NativeBaseLevelContentHashAvailable;
                if (!destination.NativeBaseLevelContentHashAvailable) {
                    const auto baseLevelHash =
                        ResolveNativeTextureBaseLevelContentHash(texture);
                    if (baseLevelHash.has_value()) {
                        destination.NativeBaseLevelContentHash =
                            *baseLevelHash;
                        destination.NativeBaseLevelContentHashAvailable =
                            true;
                    }
                }
                destination.MinLinear = texture.MinLinear;
                destination.MagLinear = texture.MagLinear;
                destination.MipLinear = texture.MipLinear;
            }
            Oot3d::PicaSceneTextureBinding orderedLightingLut;
            if (lightingLutInteger) {
                orderedLightingLut.NativeImageHandle = ToPicaSceneNativeHandle(lightingLutTexture->NriImage);
                orderedLightingLut.ImageWidth = lightingLutTexture->Width;
                orderedLightingLut.ImageHeight = lightingLutTexture->Height;
                orderedLightingLut.MipLevels = 1U;
                orderedLightingLut.ImageFormat = Oot3d::PicaSceneTextureImageFormat::R32Uint;
                orderedLightingLut.Bound = true;
            }
            const Oot3d::PicaSceneCullMode sceneCull = ToPicaSceneCullMode(draw);
            const Oot3d::PicaSceneFrontFace sceneFront = ToPicaSceneFrontFace(draw);
            mPicaSceneFrame.Record({
                .SubmissionId = draw.SubmissionId,
                .CommandListAddress = draw.CommandListAddress,
                .CommandListOffsetWords = draw.CommandListOffsetWords,
                .CompositionDomain = compositionDomain,
                .Composition = draw.Composition,
                .LightingWorldToView = draw.LightBinding.WorldToView,
                .SceneDirectionals = draw.LightBinding.SceneDirectionals,
                .ScenePoints = draw.LightBinding.ScenePoints,
                .NativeLightSources = draw.LightBinding.Sources,
                .ShadowCaster = shadowCaster,
                .Raster = {
                    .SchemaVersion =
                        Oot3d::kPicaSceneResolvedRasterStateSchemaVersion,
                    .NativeViewportX = draw.ViewportX,
                    .NativeViewportY = draw.ViewportY,
                    .NativeViewportWidth = draw.ViewportWidth,
                    .NativeViewportHeight = draw.ViewportHeight,
                    .ResolvedViewportX = mViewport.x,
                    .ResolvedViewportY = mViewport.y,
                    .ResolvedViewportWidth = mViewport.width,
                    .ResolvedViewportHeight = mViewport.height,
                    .DepthRange = draw.DepthRange,
                    .NearPlane = draw.NearPlane,
                    .ScissorMode = draw.ScissorMode,
                    .NativeScissorX1 = draw.ScissorX1,
                    .NativeScissorY1 = draw.ScissorY1,
                    .NativeScissorX2 = draw.ScissorX2,
                    .NativeScissorY2 = draw.ScissorY2,
                    .ResolvedScissorX = mScissor.offset.x,
                    .ResolvedScissorY = mScissor.offset.y,
                    .ResolvedScissorWidth = mScissor.extent.width,
                    .ResolvedScissorHeight = mScissor.extent.height,
                    .WBuffering = draw.WBuffering,
                    .FramebufferFlipped = draw.FramebufferFlipped,
                },
                .RenderTarget = {
                    .SchemaVersion =
                        Oot3d::kPicaSceneRenderTargetStateSchemaVersion,
                    .RenderTargetNamespace =
                        draw.RenderTargetNamespace,
                    .ColorPhysicalAddress =
                        draw.FramebufferColorPhysicalAddress,
                    .DepthPhysicalAddress =
                        draw.FramebufferDepthPhysicalAddress,
                    .NativeWidth = draw.FramebufferWidth,
                    .NativeHeight = draw.FramebufferHeight,
                    .NativeColorFormat =
                        draw.FramebufferColorFormat,
                    .NativeDepthFormat =
                        draw.FramebufferDepthFormat,
                    .SampleCount =
                        static_cast<uint32_t>(mNativePicaSampleCount),
                    .ResolvedColor = {
                        .NativeHandle = ToPicaSceneNativeHandle(
                            renderTarget.ColorImage),
                        .ResourceGeneration =
                            renderTarget.ColorSurfaceGeneration,
                        .Width = renderTarget.Width,
                        .Height = renderTarget.Height,
                        .Format = static_cast<uint32_t>(
                            VK_FORMAT_R8G8B8A8_UNORM),
                        .Sampleable = true,
                    },
                    .ResolvedDepth = {
                        .NativeHandle = ToPicaSceneNativeHandle(
                            renderTarget.DepthImage),
                        .ResourceGeneration =
                            renderTarget.DepthSurfaceGeneration,
                        .Width = renderTarget.Width,
                        .Height = renderTarget.Height,
                        .Format = static_cast<uint32_t>(mDepthFormat),
                        .Sampleable = true,
                    },
                },
                .CanonicalDescriptorSchemaVersion =
                    draw.CanonicalDescriptorSchemaVersion,
                .CanonicalVertexProgramId =
                    draw.CanonicalVertexProgramId,
                .CanonicalFragmentProgramId =
                    draw.CanonicalFragmentProgramId,
                .CanonicalRasterStateId =
                    draw.CanonicalRasterStateId,
                .CanonicalPipelineId = draw.CanonicalPipelineId,
                .CanonicalDynamicStateId =
                    draw.CanonicalDynamicStateId,
                .CanonicalFullRegisterStateId =
                    draw.CanonicalFullRegisterStateId,
                .VertexShaderKey = draw.VertexShaderKey,
                .VertexShaderSource = draw.VertexShaderSource,
                .FragmentShaderKey = draw.FragmentShaderKey,
                .FragmentShaderSource = draw.FragmentShaderSource,
                .EffectiveVertexShaderKey =
                    effectiveDraw.VertexShaderKey,
                .EffectiveVertexShaderSource =
                    effectiveDraw.VertexShaderSource,
                .EffectiveFragmentShaderKey =
                    effectiveDraw.FragmentShaderKey,
                .EffectiveFragmentShaderSource =
                    effectiveDraw.FragmentShaderSource,
                .GeometryIdentity = draw.GeometryIdentity,
                .GeometryContentVersion =
                    draw.GeometryContentVersion,
                .GeometryIdentityAvailable =
                    draw.GeometryIdentityAvailable,
                .VertexLayoutBindings = sceneVertexLayout.Bindings,
                .VertexLayoutAttributes = sceneVertexLayout.Attributes,
                .Material = {
                    .SchemaVersion =
                        Oot3d::kPicaSceneResolvedMaterialStateSchemaVersion,
                    .Topology = ToPicaSceneTopology(draw.Topology),
                    .CullMode = sceneCull,
                    .FrontFace = sceneFront,
                    .FragmentOperationMode =
                        draw.FragmentOperationMode,
                    .ColorWriteMask = draw.ColorWriteMask,
                    .LogicOperation = draw.LogicOperation,
                    .Blend = draw.Blend,
                    .AlphaTestEnabled = draw.AlphaTestEnabled,
                    .AlphaCompare = draw.AlphaCompare,
                    .AlphaReference = draw.AlphaReference,
                    .DepthTestEnabled = draw.DepthTestEnabled,
                    .DepthWriteEnabled = draw.DepthWriteEnabled,
                    .DepthCompare = draw.DepthCompare,
                    .Stencil = draw.Stencil,
                    .FragmentFeatures = draw.FragmentFeatures,
                },
                .UniformBuffer = {
                    ToPicaSceneNativeHandle(frame.UniformBuffer.Buffer),
                    frame.UniformBuffer.Size,
                },
                .VertexUniformOffset = vertexUniformOffset,
                .VertexUniformSize = draw.VertexUniformBytes.size(),
                .PreviousVertexUniformOffset =
                    previousVertexUniformOffset,
                .PreviousVertexUniformSize =
                    exactMotionHistory
                        ? previousVertexUniformBytes.size()
                        : 0U,
                .PreviousVertexUniformHistoryAvailable =
                    exactMotionHistory,
                .FragmentUniformOffset = fragmentUniformOffset,
                .FragmentUniformSize =
                    draw.FragmentUniformBytes.size(),
                .SurfaceLightingUniformOffset = uniformSurface ? surfaceUniformOffset : 0U,
                .SurfaceLightingUniformSize = uniformSurface ? surfaceUniformSize : 0U,
                .GeometryBuffer = {
                    ToPicaSceneNativeHandle(geometryBuffer),
                    geometryBufferSize,
                },
                .VertexBindings = sceneVertexBindings,
                .Indexed = draw.Indexed,
                .IndexOffset = indexOffset,
                .VertexOrIndexCount =
                    draw.Indexed ? indexCount : draw.VertexCount,
                .BaseVertex = draw.BaseVertex,
                .Textures = sceneTextures,
                .LightingLut = orderedLightingLut,
                .PerspectiveProjection = grassPerspectiveProjection,
                .DirectionalShadowCaster = directionalShadowCaster,
                .VertexShaderHooks =
                    draw.TemporalVertexProgram.Hooks,
                .FragmentShaderHooks = draw.FragmentShaderHooks,
                .PackedVertexUniforms = draw.VertexUniformBytes,
                .PackedPreviousVertexUniforms =
                    exactMotionHistory
                        ? previousVertexUniformBytes
                        : std::span<const uint8_t>{},
                .PackedFragmentUniforms = draw.FragmentUniformBytes,
                .VertexSourceIdentity = draw.VertexShaderSourceIdentity,
                .FragmentSourceIdentity = draw.FragmentShaderSourceIdentity,
                .EffectiveVertexSourceIdentity = effectiveDraw.VertexShaderSourceIdentity,
                .EffectiveFragmentSourceIdentity = effectiveDraw.FragmentShaderSourceIdentity,
            });
        }
        const auto* coveragePipeline = drawVariant.FragmentOutputs.SceneDomainTransparentDepthOverlay
            ? &GetOrCreateNativePicaPipeline(
                *selectedDraw, *selectedProgram, drawVariant.Reactive,
                drawVariant.Domain, drawVariant.RequestedFeatures, drawVariant.AppliedFeatures,
                true, true, !nriOwnedDraw,
                selectedDraw == &uberDraw && nriOwnedDraw
                    ? NativePicaPipelineAcquisition::Libraries
                    : NativePicaPipelineAcquisition::Blocking)
            : nullptr;
        // Coverage runs before the native draw: stencil must still have its
        // original value if that draw modifies it. The auxiliary pass is read-only.
        nriDraw.PipelineId = coveragePipeline ? coveragePipeline->Id : pipelineRecord.Id;
        const bool nriOwnedDrawBound =
            nriOwnedDraw &&
            mNriPicaPipelineBridge.BindOwnedDraw(nriDraw);
        if (nriOwnedDraw && !nriOwnedDrawBound) {
            throw std::runtime_error(
                "NRI PICA draw binding failed in an NRI-owned "
                "rendering scope");
        }
        if (nriOwnedDrawBound && coveragePipeline != nullptr) {
            nriDraw.PipelineId = pipelineRecord.Id;
            if (!mNriPicaPipelineBridge.DrawBoundGeometry(nriDraw))
                throw std::runtime_error("NRI native draw after outline coverage failed");
        }
        bool nriPipelineBound = nriOwnedDrawBound;
        if (!nriOwnedDrawBound) {
            pipeline = GetOrCreateNativePicaPipeline(
                *selectedDraw, *selectedProgram, drawVariant.Reactive,
                drawVariant.Domain, drawVariant.RequestedFeatures,
                drawVariant.AppliedFeatures, false, false, true).Vulkan;
            nriPipelineBound =
                mNriPicaPipelineBridge.Bind(
                    mCurrentFrame, pipeline);
            if (!nriPipelineBound)
                vkCmdBindPipeline(
                    commandBuffer,
                    VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            for (const auto& [binding, offset] :
                 bindingOffsets) {
                vkCmdBindVertexBuffers(commandBuffer, binding, 1, &geometryBuffer, &offset);
            }
            vkCmdBindDescriptorSets(
                commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                mNativePicaPipelineLayout, 0, 1, &descriptor,
                0, nullptr);
            vkCmdSetViewport(commandBuffer, 0, 1, &mViewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &mScissor);
            vkCmdSetBlendConstants(
                commandBuffer, draw.Blend.ConstantColor);
            vkCmdPushConstants(
                commandBuffer, mNativePicaPipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT |
                    VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(nativeDrawPush), &nativeDrawPush);
            if (draw.Indexed) {
                vkCmdBindIndexBuffer(commandBuffer, geometryBuffer, indexOffset, VK_INDEX_TYPE_UINT16);
                if (coveragePipeline != nullptr) {
                    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, coveragePipeline->Vulkan);
                    vkCmdDrawIndexed(commandBuffer, indexCount, 1, 0, draw.BaseVertex, 0);
                    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                }
                vkCmdDrawIndexed(
                    commandBuffer, indexCount, 1, 0,
                    draw.BaseVertex, 0);
            } else {
                if (coveragePipeline != nullptr) {
                    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, coveragePipeline->Vulkan);
                    vkCmdDraw(commandBuffer, draw.VertexCount, 1, 0, 0);
                    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                }
                vkCmdDraw(
                    commandBuffer, draw.VertexCount, 1, 0, 0);
            }
        }
        if (coveragePipeline != nullptr) {
            ++mDrawCallCountThisFrame;
            mDiagnostics.RecordOutlineOcclusionDraw();
        }
        finishCpuStage(cpuTimings.CommandMilliseconds);
        if (collectCpuTimings) {
            cpuTimings.TotalMilliseconds =
                std::chrono::duration<double, std::milli>(
                    CpuClock::now() - cpuStart).count();
            mDiagnostics.RecordNativePicaCpuTimings(cpuTimings);
        }
        mDiagnostics.RecordNriPicaPipelineBind(
            nriPipelineBound);
        mDiagnostics.RecordNriPicaOwnedPipeline(
            mNriPicaPipelineBridge.OwnedPipelineReady(pipelineRecord.Id));
        mDiagnostics.RecordNriPicaOwnedDraw(
            nriOwnedDrawBound,
            mNriPicaPipelineBridge.DescriptorsOwnedByNri(),
            mNriPicaPipelineBridge.LastDrawUploadsOwnedByNri(),
            mNriPicaPipelineBridge.LastDrawUploadedBytes());
        if (opaqueWorldDraw) {
            if (mInteractiveGrassProviderPlan.Valid()) {
                mGrassWorldTargetsThisFrame.insert(renderTarget.Key);
            }
        }
        if (draw.CompositionDomain ==
                ::Oot3d::Renderer::PicaCompositionDomain::Scene &&
            draw.DepthWriteEnabled) {
            ++mNativePicaDepthWritingDrawsThisFrame;
        }
        ++mDrawCallCountThisFrame;
        mDiagnostics.RecordNativePicaDraw(
            draw.SubmissionId, draw.RenderTargetNamespace,
            draw.Indexed ? indexCount : draw.VertexCount, draw.Indexed,
            draw.FramebufferColorPhysicalAddress,
            draw.FramebufferDepthPhysicalAddress, draw.FramebufferWidth,
            draw.FramebufferHeight, draw.DepthRange, draw.NearPlane,
            draw.DepthTestEnabled, draw.DepthWriteEnabled,
            draw.AlphaTestEnabled,
            Oot3d::ResolvePicaAlphaCoveragePolicy(
                mNativePicaSampleCount, draw.AlphaTestEnabled,
                draw.DepthTestEnabled,
                draw.DepthWriteEnabled, UsesTranslucentBlend(draw.Blend),
                draw.ColorWriteMask).Enable);
        return true;
    } catch (const std::exception& exception) {
        SetNativeError(error, exception.what());
        return false;
    }
}

bool GfxRenderingAPIVulkan::TryRenderInteractiveGrass(
    NativePicaRenderTarget& target,
    const Oot3d::GraphicsSettings& settings,
    Oot3d::EffectGeometryProviderInvocationKind invocationKind) {
    Renderer3ds::SlowStepScope slowStep("interactive_grass");
    const auto latestPerspective = SamplePerspective(&target);
    (void)mNativeSceneView.Publish(
        mFrameCounter, mPicaSceneFrame, latestPerspective);
    (void)mPicaScenePublications.Publish(
        mFrameCounter, mPicaSceneFrame, mNativeSceneView);
    const auto* perspective =
        mPicaScenePublications.ResolvePerspectiveCamera();
    const Oot3d::EffectGeometryProviderInvocation invocation{
        invocationKind,
        Oot3d::EffectStage::BeforeTransparent,
        {
            mPicaScenePublications.View(),
        },
        mGrassWorldTargetsThisFrame.contains(target.Key),
    };
    const auto decision =
        mInteractiveGrassProviderPlan.ResolveInvocation(invocation);
    if (!decision.Authorized() || perspective == nullptr) {
        mInteractiveGrassProviderExecution.RecordInvocation(
            invocationKind, decision,
            Oot3d::EffectGeometryProviderExecutionOutcome::Skipped);
        return false;
    }
    if (!mGrassInsertionAttemptedTargetsThisFrame
             .insert(target.Key)
             .second) {
        const bool reused =
            mGrassRenderedTargetsThisFrame.contains(target.Key);
        mInteractiveGrassProviderExecution.RecordInvocation(
            invocationKind, decision,
            reused
                ? Oot3d::EffectGeometryProviderExecutionOutcome::Reused
                : Oot3d::EffectGeometryProviderExecutionOutcome::Skipped);
        return reused;
    }
    EndNativePicaRenderPass();
    mGpuProfiler.BeginScope(
        mCurrentFrame, Oot3d::GpuProfileScope::Grass,
        mCommandBuffers[mCurrentFrame]);
    Oot3d::NriDirectionalShadowHistorySnapshot grassShadowHistory;
    if (settings.Grass.Appearance.ReceiveLighting &&
        mInteractiveGrassProviderPlan.Requires(Oot3d::EffectResource::DirectionalShadowHistory)) {
        // Resolve atlas and page-table version together at the declared native
        // insertion anchor, just like other receivers at this boundary.
        grassShadowHistory = mNriDirectionalShadowPass.FindLatestHistory(
            target.Key.RenderTargetNamespace, target.Key.ColorPhysicalAddress, mFrameCounter);
    }
    const bool prepared = mInteractiveGrassPass.Prepare(
        mCommandBuffers[mCurrentFrame], target.Width,
        target.Height, *perspective, settings.Grass,
        Oot3d::PackToonSurfaceParameters(settings.Effects.Toon, settings.Effects.ToonStyle, false),
        mPicaSceneFrame.ResolvedDrawStream(),
        mFrameCounter, target.Key.RenderTargetNamespace,
        target.Key.ColorPhysicalAddress, mCurrentFrame,
        mInteractiveGrassProviderPlan, grassShadowHistory,
        mTemporalJitterPixels, mTemporalJitterEnabled, mTemporalMotionEnabled);
    bool executed = false;
    if (prepared) {
        BeginNativePicaRenderPass(target);
        executed = mInteractiveGrassPass.DrawPrepared(
            mCommandBuffers[mCurrentFrame],
            target.Width, target.Height,
            mInteractiveGrassProviderPlan,
            mFramePicaAttachmentRequirements);
    }
    mGpuProfiler.EndScope(
        mCurrentFrame, Oot3d::GpuProfileScope::Grass,
        mCommandBuffers[mCurrentFrame]);
    mGrassExecutedThisFrame |= executed;
    if (executed) {
        mGrassRenderedTargetsThisFrame.insert(target.Key);
        mDiagnostics.RecordGrassMotion(
            mInteractiveGrassPass.LastMotionBladeCount());
    }
    const auto grassTelemetry =
        Oot3d::GrassRenderTelemetry::Instance().Snapshot();
    const bool failed =
        grassTelemetry.Status == Oot3d::GrassRenderStatus::Error ||
        (prepared && !executed);
    mInteractiveGrassProviderExecution.RecordInvocation(
        invocationKind, decision,
        executed
            ? Oot3d::EffectGeometryProviderExecutionOutcome::Executed
            : failed
                  ? Oot3d::EffectGeometryProviderExecutionOutcome::Failed
                  : Oot3d::EffectGeometryProviderExecutionOutcome::Skipped);

    if (std::getenv("OOT3D_GRASS_DIAGNOSTICS") != nullptr ||
        std::getenv("OOT3D_GRAPHICS_GRASS_AUTO") != nullptr) {
        static uint32_t grassSmokeReports = 0U;
        static uint64_t lastGrassSmokeFrame = 0U;
        static auto lastGrassSmokeStatus = Oot3d::GrassRenderStatus::Disabled;
        const auto telemetry =
            Oot3d::GrassRenderTelemetry::Instance().Snapshot();
        const bool initialReport = grassSmokeReports < 3U;
        const bool periodicReport =
            telemetry.FrameId >= lastGrassSmokeFrame + 15U;
        const bool diagnosticEvent =
            (telemetry.PlacementCacheMisses != 0U ||
             telemetry.CpuMilliseconds > 16.0) &&
            telemetry.FrameId >= lastGrassSmokeFrame + 5U;
        const bool statusChanged = telemetry.Status != lastGrassSmokeStatus;
        if (initialReport || periodicReport || diagnosticEvent || statusChanged) {
            ++grassSmokeReports;
            lastGrassSmokeFrame = telemetry.FrameId;
            lastGrassSmokeStatus = telemetry.Status;
            std::fprintf(
                stderr,
                "OOT3D grass smoke: frame=%llu status=%s camera=%d meshes=%zu "
                "textures=%zu rules=%zu masks=%zu matches=%zu "
                "scoped=%zu placements=%zu cache=%zu/%zu pending=%zu clusters=%zu "
                "visibility_nodes=%llu candidate_clusters=%llu "
                "world_cache=%llu/%llu/%zu "
                "async_build=%llu/%llu/%.3f "
                "anchors=%llu evaluated=%llu blades=%u draws=%u workers=%u "
                "compact=%d upload=%llu/%llu/%llu "
                "place_ms=%.3f select_ms=%.3f copy_ms=%.3f "
                "cpu_ms=%.3f miss_geometry=%llx miss_content=%llx "
                "reason=%s\n",
                static_cast<unsigned long long>(telemetry.FrameId),
                Oot3d::GrassRenderStatusName(telemetry.Status),
                perspective->CameraAvailable ? 1 : 0,
                telemetry.AvailableMeshes,
                telemetry.AvailableTextures,
                telemetry.ConfiguredRules, telemetry.ReadyMasks,
                telemetry.MatchingMeshes, telemetry.ScopedMeshes,
                telemetry.Placements,
                telemetry.PlacementCacheHits,
                telemetry.PlacementCacheMisses,
                telemetry.PendingPlacements,
                telemetry.Clusters,
                static_cast<unsigned long long>(telemetry.VisibilityNodesTested),
                static_cast<unsigned long long>(telemetry.CandidateClusters),
                static_cast<unsigned long long>(
                    telemetry.WorldPlacementCacheHits),
                static_cast<unsigned long long>(
                    telemetry.WorldPlacementCacheMisses),
                telemetry.WorldPlacementCacheEntries,
                static_cast<unsigned long long>(
                    telemetry.AsyncPlacementBuilds),
                static_cast<unsigned long long>(
                    telemetry.AsyncPlacementFailures),
                telemetry.AsyncPlacementBuildMilliseconds,
                static_cast<unsigned long long>(
                    telemetry.ExtractedAnchors),
                static_cast<unsigned long long>(
                    telemetry.EvaluatedAnchors),
                telemetry.VisibleBlades, telemetry.DrawCalls,
                telemetry.CullingWorkers,
                telemetry.GpuCompaction ? 1 : 0,
                static_cast<unsigned long long>(
                    telemetry.UploadedBytes),
                static_cast<unsigned long long>(
                    telemetry.DynamicUploadedBytes),
                static_cast<unsigned long long>(
                    telemetry.StaticUploadedBytes),
                telemetry.PlacementMilliseconds,
                telemetry.SelectionMilliseconds,
                telemetry.UploadMilliseconds,
                telemetry.CpuMilliseconds,
                static_cast<unsigned long long>(
                    telemetry.LastMissGeometryId),
                static_cast<unsigned long long>(
                    telemetry.LastMissContentVersion),
                mInteractiveGrassPass.UnavailableReason().c_str());
        }
    }
    static bool grassPassReported = false;
    if (mGrassExecutedThisFrame && !grassPassReported) {
        SPDLOG_INFO(
            "OOT3D Vulkan interactive grass drew {} blades",
            mInteractiveGrassPass.LastBladeCount());
        grassPassReported = true;
    }
    return executed;
}

bool GfxRenderingAPIVulkan::TryRenderDirectionalShadowMap(
    NativePicaRenderTarget& target,
    const Oot3d::GraphicsSettings& settings,
    const Oot3d::PicaCompositionStageAnchor& anchor) {
    Renderer3ds::SlowStepScope slowStep("directional_shadows");
    const auto boundary = Oot3d::BuildPicaExtensionScheduleBoundary(
        mNativePicaCompositionSchedule, anchor);
    const auto decision = mDirectionalShadowSchedulePlan.Resolve(boundary);
    if (!decision.Authorized()) {
        mDiagnostics.RecordNriDirectionalShadowSchedule(
            false, false,
            boundary.Kind ==
                ::Fast::Renderer::ExtensionScheduleBoundaryKind::SurfaceEnd);
        return false;
    }
    if (!mDirectionalShadowInsertionAttemptedTargetsThisFrame
             .insert({boundary.ScheduleId, target.Key})
             .second) {
        mDiagnostics.RecordNriDirectionalShadowSchedule(
            true, true,
            boundary.Kind ==
                ::Fast::Renderer::ExtensionScheduleBoundaryKind::SurfaceEnd);
        return false;
    }
    mDiagnostics.RecordNriDirectionalShadowSchedule(
        true, false,
        boundary.Kind ==
            ::Fast::Renderer::ExtensionScheduleBoundaryKind::SurfaceEnd);

    const auto latestPerspective = SamplePerspective(&target);
    (void)mNativeSceneView.Publish(
        mFrameCounter, mPicaSceneFrame, latestPerspective);
    // The map reads this sample's prepared casters, complete at the opaque
    // boundary before any of their receivers draws.
    (void)mPreparedShadowCasterView.Publish(
        mFrameCounter, mPreparedShadowCasterFrame, latestPerspective);
    mDiagnostics.RecordNriDirectionalShadowPreparedMap(
        boundary.Stage == Oot3d::EffectStage::BeforeOpaque);
    EndNativePicaRenderPass();
    if (mRenderPassActive) {
        vkCmdEndRenderPass(mCommandBuffers[mCurrentFrame]);
        mRenderPassActive = false;
        mOverlayRenderPassActive = false;
    }

    const bool executionRequested = mPreparedShadowCasterView.Active();
    const bool executed =
        executionRequested &&
        mNriDirectionalShadowPass.ExecuteMap(
            {
                mCurrentFrame,
                mFrameCounter,
                target.Key.RenderTargetNamespace,
                target.Key.ColorPhysicalAddress,
                decision.Authorization,
                boundary.Surface,
                settings.Effects.DirectionalShadows,
                mFrameEnvironment.Enabled
                    ? std::optional(mFrameEnvironment.Lights[0]) : std::nullopt,
                mFrameEnvironment.Enabled ? mFrameEnvironment.NativeDirectStrength : 1.0F,
            },
            mPreparedShadowCasterView,
            mDirectionalShadowMapBarrierPlan);
    if (executionRequested) {
        const auto& shadowMapTransitions =
            mNriDirectionalShadowPass.LastShadowMapBarrierExecution();
        mDiagnostics.RecordEffectGraphInternalImageTransitions(
            shadowMapTransitions.PlannedGraphTransitionCount(),
            shadowMapTransitions.EmittedGraphTransitionCount(),
            shadowMapTransitions.PlannedPrivateTransitionCount(),
            shadowMapTransitions.EmittedPrivateTransitionCount());
        mDiagnostics.RecordNriDirectionalShadowGraphTransitions(
            shadowMapTransitions.PlannedGraphTransitionCount(),
            shadowMapTransitions.EmittedGraphTransitionCount());
    }

    const auto sceneStats = mPreparedShadowCasterFrame.Stats();
    mDiagnostics.RecordNriDirectionalShadowAttempt(
        sceneStats.DirectionalShadowCasterCount,
        sceneStats.DirectionalShadowNativeLightCasterCount,
        static_cast<uint32_t>(
            mNriDirectionalShadowPass.LastExecutionStage()));
    if (executed) {
        const auto& lightTelemetry =
            mNriDirectionalShadowPass.LastLightTelemetry();
        mDiagnostics.RecordNriVirtualShadowPages(
            lightTelemetry.Pages, lightTelemetry.UpdatedPages, lightTelemetry.AtlasBytes);
        mDiagnostics.RecordNriDirectionalShadowLightSelection(
            lightTelemetry.CandidateCount,
            lightTelemetry.ClusterCount,
            lightTelemetry.GeometryLightWeight,
            lightTelemetry.WorldDirectionTowardSource[0],
            lightTelemetry.WorldDirectionTowardSource[1],
            lightTelemetry.WorldDirectionTowardSource[2]);
        mDiagnostics.RecordNriDirectionalShadow(
            mNriDirectionalShadowPass.LastCasterCount(),
            mNriDirectionalShadowPass.LastExecutionUsedNativeLight(),
            mNriDirectionalShadowPass.LastAlphaCasterCount());
    }
    return executed;
}

bool GfxRenderingAPIVulkan::TryRenderPointShadowMaps(
    NativePicaRenderTarget& target,
    const Oot3d::GraphicsSettings& settings,
    const Oot3d::PicaCompositionStageAnchor& anchor) {
    Renderer3ds::SlowStepScope slowStep("point_shadows");
    const auto boundary = Oot3d::BuildPicaExtensionScheduleBoundary(
        mNativePicaCompositionSchedule, anchor);
    const auto decision = mPointShadowSchedulePlan.Resolve(boundary);
    if (!decision.Authorized() ||
        !mPointShadowInsertionAttemptedTargetsThisFrame.insert({boundary.ScheduleId, target.Key}).second)
        return false;
    // The same prepared casters as the directional map, complete at the
    // opaque boundary before any of their receivers draws.
    (void)mPreparedShadowCasterView.Publish(
        mFrameCounter, mPreparedShadowCasterFrame, SamplePerspective(&target));
    if (!mPreparedShadowCasterView.Active())
        return false;
    EndNativePicaRenderPass();
    if (mRenderPassActive) {
        vkCmdEndRenderPass(mCommandBuffers[mCurrentFrame]);
        mRenderPassActive = false;
        mOverlayRenderPassActive = false;
    }
    const bool executed = mNriDirectionalShadowPass.ExecutePointMaps(
        {
            mCurrentFrame,
            mFrameCounter,
            target.Key.RenderTargetNamespace,
            target.Key.ColorPhysicalAddress,
            decision.Authorization,
            boundary.Surface,
            settings.Effects.PointShadows,
            mFrameEnvironment,
        },
        mPreparedShadowCasterView,
        mPointShadowMapBarrierPlan);
    const auto& telemetry = mNriDirectionalShadowPass.LastPointTelemetry();
    mDiagnostics.RecordNriPointShadowMap(
        executed, telemetry.CandidateLights, telemetry.Lights, telemetry.RenderedLights,
        telemetry.CasterDraws, telemetry.AlphaCasterDraws);
    return executed;
}

namespace {

// The declared end-of-target providers of one composition target: the shadow
// map at its graph-authorized native anchor and the interactive geometry
// published for the target. Both the display transfer and native copy boundaries compose
// the same scene, so the providers and their anchors live here once instead of
// in two lists.
struct NativePicaTargetEndDispatch {
    const Oot3d::PicaCompositionStageAnchor* ShadowAnchor = nullptr;
    const Oot3d::PicaCompositionStageAnchor* PointShadowAnchor = nullptr;
    bool DispatchGeometry = false;
};

// `submissionId` is 0 for a display transfer, which composes at the target's own
// end, and the consumer's real order id for a native boundary, which composes
// the scene as of that submission: an anchor the native sequence has not
// reached is never flushed into the composition, and later draws are never
// moved across it. A boundary therefore accepts the target's declared anchor
// once the sequence has passed its insertion point - including a target-end
// anchor, which is reached only after every draw of the sequence.
[[nodiscard]] NativePicaTargetEndDispatch ResolveNativePicaTargetEndDispatch(
    const Oot3d::PicaCompositionSchedule& schedule,
    const ::Fast::Renderer::ExtensionPassSchedulePlan& shadowSchedule,
    const ::Fast::Renderer::ExtensionPassSchedulePlan& pointShadowSchedule,
    bool geometryProviderValid,
    const ::Oot3d::Renderer::PicaCompositionTargetReference& target,
    uint64_t submissionId, bool geometryTargetPublished,
    bool geometryAlreadyAttempted) {
    NativePicaTargetEndDispatch dispatch;
    if (!schedule.Valid()) {
        return dispatch;
    }
    if (shadowSchedule.Valid()) {
        const auto* anchor = Oot3d::FindPicaEffectPassAnchor(schedule, shadowSchedule, target);
        if (anchor != nullptr && schedule.Reached(*anchor) &&
            (submissionId != 0U || anchor->AtTargetEnd)) {
            dispatch.ShadowAnchor = anchor;
        }
    }
    if (pointShadowSchedule.Valid()) {
        const auto* anchor = Oot3d::FindPicaEffectPassAnchor(schedule, pointShadowSchedule, target);
        if (anchor != nullptr && schedule.Reached(*anchor) &&
            (submissionId != 0U || anchor->AtTargetEnd)) {
            dispatch.PointShadowAnchor = anchor;
        }
    }
    if (geometryProviderValid) {
        const auto* anchor = schedule.FindPublishedGeometryAnchor(target);
        dispatch.DispatchGeometry =
            anchor != nullptr && anchor->AtTargetEnd &&
            geometryTargetPublished && !geometryAlreadyAttempted &&
            (submissionId == 0U || schedule.Reached(*anchor));
    }
    return dispatch;
}

} // namespace

bool GfxRenderingAPIVulkan::SubmitPicaDisplayTransfer(
    const GfxNativePicaDisplayTransferView& transfer, std::string* error) {
    Renderer3ds::SlowStepScope slowStep("display_transfer");
    if (transfer.TextureCopyBytes != 0U) return SubmitNativePicaTextureCopy(transfer, error);
    try {
        if (!mFrameActive || transfer.CompletionId == 0U ||
            transfer.InputWidth == 0U || transfer.InputHeight == 0U ||
            transfer.OutputWidth == 0U || transfer.OutputHeight == 0U) {
            throw std::runtime_error(
                "native PICA display transfer is incomplete");
        }
        if ((transfer.Flags & 8U) != 0U ||
            (transfer.Flags & 0x10000U) != 0U ||
            ((transfer.Flags >> 24U) & 3U) > 2U) {
            throw std::runtime_error(
                "native PICA display transfer mode is unsupported");
        }
        if (!transfer.Present) {
            NativePicaRenderTarget* source = nullptr;
            for (auto& [key, target] : mNativePicaRenderTargets) {
                if (key.RenderTargetNamespace ==
                        transfer.RenderTargetNamespace &&
                    key.ColorPhysicalAddress ==
                    transfer.InputPhysicalAddress) {
                    source = &target;
                    break;
                }
            }
            if (source == nullptr) {
                throw std::runtime_error(
                    "native PICA display transfer source has no Vulkan render target");
            }
            if (transfer.InputWidth > source->Key.Width ||
                transfer.InputHeight > source->Key.Height) {
                throw std::runtime_error(
                    "native PICA display transfer exceeds its Vulkan render target");
            }
            std::string transferPlanError;
            const auto transferPlan =
                Oot3d::BuildPicaDisplayTransferPlan({
                    transfer.InputWidth,
                    transfer.InputHeight,
                    transfer.OutputWidth,
                    transfer.OutputHeight,
                    source->Width,
                    source->Height,
                    transfer.Flags,
                }, &transferPlanError);
            if (!transferPlan.has_value()) {
                throw std::runtime_error(
                    "native PICA display transfer plan failed: " +
                    transferPlanError);
            }
            if (source->Key.Width == Oot3d::kNativeTopScreenPhysicalWidth &&
                source->Key.Height == Oot3d::kNativeTopScreenPhysicalHeight) {
                Oot3d::GraphicsSettingsRuntime::Instance().PublishSceneExtent(
                    transferPlan->DestinationHeight, transferPlan->DestinationWidth);
            }

            const auto& graphicsSettings =
                mFrameGraphicsSettings;
            const auto perspective = SamplePerspective(source);
            (void)mNativeSceneView.Publish(
                mFrameCounter, mPicaSceneFrame, perspective);
            const ::Oot3d::Renderer::PicaCompositionTargetReference
                compositionTarget{
                    source->Key.RenderTargetNamespace,
                    source->Key.ColorPhysicalAddress,
                    source->Key.DepthPhysicalAddress,
                    source->Key.Width,
                    source->Key.Height,
                    source->Key.ColorFormat,
                    source->Key.DepthFormat,
                };
            const auto targetEnd = ResolveNativePicaTargetEndDispatch(
                mNativePicaCompositionSchedule,
                mDirectionalShadowSchedulePlan,
                mPointShadowSchedulePlan,
                mInteractiveGrassProviderPlan.Valid(), compositionTarget,
                /*submissionId=*/0U,
                mGrassWorldTargetsThisFrame.contains(source->Key),
                mGrassInsertionAttemptedTargetsThisFrame.contains(
                    source->Key));
            if (targetEnd.ShadowAnchor != nullptr) {
                TryRenderDirectionalShadowMap(
                    *source, graphicsSettings, *targetEnd.ShadowAnchor);
            }
            if (targetEnd.PointShadowAnchor != nullptr) {
                TryRenderPointShadowMaps(
                    *source, graphicsSettings, *targetEnd.PointShadowAnchor);
            }
            if (targetEnd.DispatchGeometry) {
                TryRenderInteractiveGrass(
                    *source, graphicsSettings,
                    Oot3d::EffectGeometryProviderInvocationKind::DeclaredBoundary);
            }
            EndNativePicaRenderPass();
            if (mRenderPassActive) {
                vkCmdEndRenderPass(mCommandBuffers[mCurrentFrame]);
                mRenderPassActive = false;
                mOverlayRenderPassActive = false;
            }
            auto& display = GetOrCreateNativePicaDisplayImage(
                transfer.RenderTargetNamespace,
                transfer.OutputPhysicalAddress,
                transferPlan->DestinationWidth,
                transferPlan->DestinationHeight);
            const auto displayKey = std::make_pair(
                transfer.RenderTargetNamespace,
                transfer.OutputPhysicalAddress);
            const auto displayComposition =
                Oot3d::ResolvePicaDisplayComposition(
                    mNativePicaCompositionSchedule,
                    compositionTarget);
            display.CompositionDomain = displayComposition.Domain;
            display.CompositionSequenceId =
                displayComposition.SequenceId;
            display.SceneResolved =
                displayComposition.SceneResolved;
            // A display whose producing draw consumed a resolved scene
            // composition over the whole target already carries those scene
            // effects, so presentation must not apply them a second time.
            // Resolved here, at the moment the scene colour is snapshotted, and
            // carried on the display image: a later UI write to the same
            // attachment bumps the target's content serial and must not
            // invalidate the version that was actually imported, so this is
            // never re-queried when the snapshot is presented.
            display.SceneEffectsResolved =
                NativePicaSceneFullyResolvedForTarget(
                    source->Key.RenderTargetNamespace,
                    source->Key.ColorPhysicalAddress);
            mDiagnostics.RecordPicaDisplayComposition(
                display.CompositionDomain, display.SceneResolved);
            if (source->DepthView != VK_NULL_HANDLE &&
                (display.SceneResolved || display.SceneEffectsResolved)) {
                mNativePicaDisplayDepthTargets[displayKey] = source->Key;
            } else {
                mNativePicaDisplayDepthTargets.erase(displayKey);
            }
            mGpuProfiler.BeginScope(
                mCurrentFrame,
                Oot3d::GpuProfileScope::DisplayTransfer,
                mCommandBuffers[mCurrentFrame]);
            const VkImage displayCopySource = source->ColorImage;
            const bool nriDisplayCopy =
                mNriPicaDisplayCopyPass.Execute({
                    mCurrentFrame,
                    mFrameCounter,
                    displayCopySource,
                    display.Image,
                    VK_FORMAT_R8G8B8A8_UNORM,
                    *transferPlan,
                    false,
                    display.Initialized,
                });
            if (nriDisplayCopy) {
                mDiagnostics.RecordNriPicaDisplayCopy(
                    true,
                    mNriPicaDisplayCopyPass.LastBarrierCount());
            } else {
                VkCommandBuffer command = mCommandBuffers[mCurrentFrame];
                std::array<VkImageMemoryBarrier, 2> toTransfer{};
                toTransfer[0] = {
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                toTransfer[0].oldLayout =
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                toTransfer[0].newLayout =
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                toTransfer[0].srcQueueFamilyIndex =
                    VK_QUEUE_FAMILY_IGNORED;
                toTransfer[0].dstQueueFamilyIndex =
                    VK_QUEUE_FAMILY_IGNORED;
                toTransfer[0].image = displayCopySource;
                toTransfer[0].subresourceRange.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                toTransfer[0].subresourceRange.levelCount = 1;
                toTransfer[0].subresourceRange.layerCount = 1;
                toTransfer[0].srcAccessMask =
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                toTransfer[0].dstAccessMask =
                    VK_ACCESS_TRANSFER_READ_BIT;
                toTransfer[1] = toTransfer[0];
                toTransfer[1].oldLayout =
                    display.Initialized
                        ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                        : VK_IMAGE_LAYOUT_UNDEFINED;
                toTransfer[1].newLayout =
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toTransfer[1].image = display.Image;
                toTransfer[1].srcAccessMask =
                    display.Initialized
                        ? VK_ACCESS_SHADER_READ_BIT
                        : 0U;
                toTransfer[1].dstAccessMask =
                    VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(
                    command,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                    0, nullptr,
                    static_cast<uint32_t>(toTransfer.size()),
                    toTransfer.data());
                VkImageBlit blit{};
                blit.srcSubresource.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                blit.srcSubresource.layerCount = 1;
                blit.srcOffsets[1] = {
                    static_cast<int32_t>(
                        transferPlan->BlitSourceWidth()),
                    static_cast<int32_t>(
                        transferPlan->BlitSourceHeight()),
                    1};
                blit.dstSubresource.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                blit.dstSubresource.layerCount = 1;
                blit.dstOffsets[1] = {
                    static_cast<int32_t>(
                        transferPlan->DestinationWidth),
                    static_cast<int32_t>(
                        transferPlan->DestinationHeight),
                    1};
                vkCmdBlitImage(
                    command, displayCopySource,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    display.Image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                    &blit,
                    transferPlan->HorizontalSamples == 1U &&
                            transferPlan->VerticalSamples == 1U
                        ? VK_FILTER_NEAREST
                        : VK_FILTER_LINEAR);
                std::array<VkImageMemoryBarrier, 2> fromTransfer =
                    toTransfer;
                fromTransfer[0].oldLayout =
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                fromTransfer[0].newLayout =
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                fromTransfer[0].srcAccessMask =
                    VK_ACCESS_TRANSFER_READ_BIT;
                fromTransfer[0].dstAccessMask =
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                fromTransfer[1].oldLayout =
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                fromTransfer[1].newLayout =
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                fromTransfer[1].srcAccessMask =
                    VK_ACCESS_TRANSFER_WRITE_BIT;
                fromTransfer[1].dstAccessMask =
                    VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(
                    command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr,
                    static_cast<uint32_t>(fromTransfer.size()),
                    fromTransfer.data());
            }
            // The scene this display showed before native UI, transferred
            // exactly as the display: an upscaler reconstructs it and lays the
            // display's UI over its output.
            display.SceneBeforeUi = false;
            if (mPresentationUpscaler && nriDisplayCopy && display.SceneEffectsResolved &&
                source->SceneBeforeUiSerial != 0U &&
                source->SceneBeforeUiSerial == source->ContentWriteSerial) {
                const auto snapshot = mNativePicaSceneBeforeUiTargets.find(
                    {source->Key.RenderTargetNamespace, source->Key.ColorPhysicalAddress});
                if (snapshot != mNativePicaSceneBeforeUiTargets.end() && snapshot->second.Initialized &&
                    snapshot->second.Width == source->Width && snapshot->second.Height == source->Height) {
                    auto& beforeUi = GetOrCreateNativePicaDisplayImage(
                        mNativePicaSceneBeforeUiDisplays, transfer.RenderTargetNamespace,
                        transfer.OutputPhysicalAddress, transferPlan->DestinationWidth,
                        transferPlan->DestinationHeight);
                    display.SceneBeforeUi = mNriPicaDisplayCopyPass.Execute({
                        mCurrentFrame, mFrameCounter, snapshot->second.Image, beforeUi.Image,
                        VK_FORMAT_R8G8B8A8_UNORM, *transferPlan, true, beforeUi.Initialized});
                    if (display.SceneBeforeUi)
                        beforeUi.Initialized = true;
                    mDiagnostics.RecordSceneBeforeUi(false, display.SceneBeforeUi);
                }
            }
            mGpuProfiler.EndScope(
                mCurrentFrame,
                Oot3d::GpuProfileScope::DisplayTransfer,
                mCommandBuffers[mCurrentFrame]);
            display.Initialized = true;
            mDiagnostics.RecordDisplayTransfer(false);
            return true;
        }

        const auto displayFound = mNativePicaDisplayImages.find(
            std::make_pair(transfer.RenderTargetNamespace,
                           transfer.OutputPhysicalAddress));
        if (displayFound == mNativePicaDisplayImages.end() ||
            !displayFound->second.Initialized) {
            if (transfer.RenderTargetNamespace == 0U &&
                mInvalidatedNativePicaRenderTargetAddresses.contains(
                    transfer.InputPhysicalAddress)) {
                return true;
            }
            throw std::runtime_error(
                "native PICA display transfer output has no GPU snapshot");
        }
        const auto& display = displayFound->second;
        return ExecuteNativePicaDisplayGraph(transfer, display, nullptr,
                                             nullptr, error);
    } catch (const std::exception& exception) {
        SetNativeError(error, exception.what());
        return false;
    }
}

bool GfxRenderingAPIVulkan::ExecuteNativePicaDisplayGraph(
    const GfxNativePicaDisplayTransferView& transfer,
    const NativePicaDisplayImage& display,
    NativePicaRenderTarget* copySource,
    GpuTextureCopySourceImport* copyOutput, std::string* error,
    bool beforeNativeUi) {
    Renderer3ds::SlowStepTrace slowSteps("display_graph");

        const auto& presentationSettings =
            mFrameGraphicsSettings;
        // The traced GI light carries the sources' relative weights; their
        // largest gain applies when it is composed.
        const auto giSources =
            Oot3d::ResolveGlobalIlluminationSources(presentationSettings.Effects.GlobalIllumination);
        // The camera of the scene this boundary or transfer composes.
        const NativePicaRenderTarget* sceneViewTarget = copySource;
        if (copyOutput == nullptr) {
            sceneViewTarget = nullptr;
            const auto depthAssociation = mNativePicaDisplayDepthTargets.find(
                std::make_pair(transfer.RenderTargetNamespace, transfer.OutputPhysicalAddress));
            if (depthAssociation != mNativePicaDisplayDepthTargets.end()) {
                const auto target = mNativePicaRenderTargets.find(depthAssociation->second);
                if (target != mNativePicaRenderTargets.end()) sceneViewTarget = &target->second;
            }
        }
        const auto latestPerspective = SamplePerspective(sceneViewTarget);
        (void)mNativeSceneView.Publish(
            mFrameCounter, mPicaSceneFrame, latestPerspective);
        const auto& presentationPerspective =
            mNativeSceneView.CurrentPerspective();
        const auto& nativeTemporalSample = mNativeSceneView.TemporalSample();

        EndNativePicaRenderPass();
        if (mRenderPassActive) {
            vkCmdEndRenderPass(mCommandBuffers[mCurrentFrame]);
            mRenderPassActive = false;
            mOverlayRenderPassActive = false;
        }
        // A native boundary (raw copy / native screen effect) composes the
        // scene for its consumer at the boundary's own order and materializes
        // it for that consumer. Presentation keeps the swapchain target.
        const bool copyBoundary = copyOutput != nullptr;
        VkCommandBuffer command = mCommandBuffers[mCurrentFrame];
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        if (!copyBoundary) {
            CreateNativePicaScanoutPipeline();
            descriptor = AllocateNativePicaScanoutDescriptorSet();
        }
        const bool alphaOverlay = !copyBoundary &&
            transfer.PresentationMode ==
                GfxNativePicaPresentationMode::AlphaOverlay;
        const auto environmentEnabled = [](const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr && std::string_view(value) == "1";
        };
        const bool worldSurface =
            display.CompositionDomain ==
                ::Oot3d::Renderer::PicaCompositionDomain::Scene &&
            (display.SceneResolved || display.SceneEffectsResolved);
        // The scene before native UI transferred beside this display.
        const NativePicaDisplayImage* sceneBeforeUi = nullptr;
        if (!copyBoundary && display.SceneBeforeUi) {
            const auto found = mNativePicaSceneBeforeUiDisplays.find(
                {transfer.RenderTargetNamespace, transfer.OutputPhysicalAddress});
            if (found != mNativePicaSceneBeforeUiDisplays.end() && found->second.Initialized &&
                found->second.Width == display.Width && found->second.Height == display.Height)
                sceneBeforeUi = &found->second;
        }
        // One scene surface per frame owns the anti-aliasing history and
        // output. A boundary before native UI of another surface in the same
        // frame composes its other scene stages without them.
        const bool antiAliasingAvailableHere = [&] {
            if (!copyBoundary || !beforeNativeUi || copySource == nullptr) return true;
            const Oot3d::SceneSurfaceKey surface{
                transfer.RenderTargetNamespace, copySource->Key.ColorPhysicalAddress,
                Oot3d::SceneSurfaceKind::Output};
            const bool taaTaken = mTaaExecutedThisFrame && mTaaSurface.has_value() &&
                *mTaaSurface != surface;
            const bool smaaTaken = mSmaaExecutedThisFrame &&
                (mSmaaRenderTargetNamespace != transfer.RenderTargetNamespace ||
                 mSmaaDisplayPhysicalAddress != copySource->Key.ColorPhysicalAddress);
            return !taaTaken && !smaaTaken;
        }();
        const auto effectPlan = Oot3d::BuildDisplayEffectPlan({
            .AlphaOverlay = alphaOverlay,
            .WorldSurface = worldSurface,
            .PerspectiveAvailable = presentationPerspective.has_value(),
            .CameraAvailable = presentationPerspective.has_value() &&
                presentationPerspective->CameraAvailable,
            .SmaaAvailable = mSmaa1xPass.Available(),
            .ForceTaa = environmentEnabled("OOT3D_GRAPHICS_TAA"),
            .ForceMotion = environmentEnabled("OOT3D_GRAPHICS_MOTION"),
            .AmbientOcclusion =
                presentationSettings.Effects.AmbientOcclusion,
            .AmbientOcclusionTemporal =
                presentationSettings.Effects.AmbientOcclusion == Oot3d::AmbientOcclusionMode::Mxao &&
                presentationSettings.Effects.MxaoTemporal && mMxaoPass.Available(),
            .Reflections = presentationSettings.Effects.Reflections,
            .GlobalIllumination =
                presentationSettings.Effects.GlobalIllumination.Mode ==
                    Oot3d::GlobalIlluminationMode::BrixelizerGi &&
                mBrixelizerGiPass.Available() && mBrixelizerScene.Available(),
            .GlobalIlluminationSpecular = presentationSettings.Effects.GlobalIllumination.Specular,
            .Toon = presentationSettings.Effects.Toon,
            .OutlineEnabled =
                presentationSettings.Effects.ToonStyle.OutlineEnabled,
            .AntiAliasing = presentationSettings.AntiAliasing,
            .Upscaler = presentationSettings.Upscaler,
            .GuideDiagnostics = Oot3d::PicaGuideDiagnosticMode() != 0,
            .NativeSceneBoundary = copyBoundary,
            .BeforeNativeUi = copyBoundary && beforeNativeUi && antiAliasingAvailableHere,
            .SceneEffectsResolved = display.SceneEffectsResolved,
            .SceneBeforeUi = sceneBeforeUi != nullptr,
            .Purpose = mNativePicaCompositionSchedule.Purpose(),
        });
        const auto motionBarrierPlan =
            Oot3d::BuildEffectPassBarrierPlan(
                effectPlan.Graph,
                Oot3d::DisplayEffectPassName(
                    Oot3d::DisplayEffectPass::Motion));
        const auto aoBarrierPlan =
            Oot3d::BuildEffectPassBarrierPlan(
                effectPlan.Graph,
                Oot3d::DisplayEffectPassName(
                    Oot3d::DisplayEffectPass::AmbientOcclusion));
        const auto hiZBarrierPlan =
            Oot3d::BuildEffectPassBarrierPlan(
                effectPlan.Graph,
                Oot3d::DisplayEffectPassName(
                    Oot3d::DisplayEffectPass::HiZ));
        const auto workingColorBarrierPlan =
            Oot3d::BuildEffectPassBarrierPlan(
                effectPlan.Graph,
                Oot3d::DisplayEffectPassName(
                    Oot3d::DisplayEffectPass::WorkingColor));
        const auto reflectionBarrierPlan =
            Oot3d::BuildEffectPassBarrierPlan(
                effectPlan.Graph,
                Oot3d::DisplayEffectPassName(
                    Oot3d::DisplayEffectPass::Reflections));
        const auto compositeBarrierPlan =
            Oot3d::BuildEffectPassBarrierPlan(
                effectPlan.Graph,
                Oot3d::DisplayEffectPassName(
                    Oot3d::DisplayEffectPass::Composite));
        const auto globalIlluminationBarrierPlan =
            Oot3d::BuildEffectPassBarrierPlan(
                effectPlan.Graph,
                Oot3d::DisplayEffectPassName(
                    Oot3d::DisplayEffectPass::GlobalIllumination));
        const auto taaBarrierPlan =
            Oot3d::BuildEffectPassBarrierPlan(
                effectPlan.Graph,
                Oot3d::DisplayEffectPassName(
                    Oot3d::DisplayEffectPass::Taa));
        const auto temporalUpscalerBarrierPlan =
            Oot3d::BuildEffectPassBarrierPlan(
                effectPlan.Graph,
                Oot3d::DisplayEffectPassName(
                    Oot3d::DisplayEffectPass::TemporalUpscaler));
        const auto nisBarrierPlan =
            Oot3d::BuildEffectPassBarrierPlan(
                effectPlan.Graph,
                Oot3d::DisplayEffectPassName(
                    Oot3d::DisplayEffectPass::Nis));
        const auto smaaBarrierPlan =
            Oot3d::BuildEffectPassBarrierPlan(
                effectPlan.Graph,
                Oot3d::DisplayEffectPassName(
                    Oot3d::DisplayEffectPass::Smaa));
        const auto scanoutBarrierPlan =
            Oot3d::BuildEffectPassBarrierPlan(
                effectPlan.Graph,
                Oot3d::DisplayEffectPassName(
                    Oot3d::DisplayEffectPass::Scanout));
        const auto sceneCompositionBarrierPlan =
            Oot3d::BuildEffectPassBarrierPlan(
                effectPlan.Graph,
                Oot3d::DisplayEffectPassName(
                    Oot3d::DisplayEffectPass::SceneComposition));
        if (!effectPlan.Graph.Valid()) {
            throw std::runtime_error(
                "display effect graph is invalid: " +
                effectPlan.Graph.Error);
        }
        const auto recordEffectPassTransitions =
            [this](const Oot3d::EffectPassBarrierExecution& execution) {
                mDiagnostics.RecordEffectGraphInternalImageTransitions(
                    execution.PlannedGraphTransitionCount(),
                    execution.EmittedGraphTransitionCount(),
                    execution.PlannedPrivateTransitionCount(),
                    execution.EmittedPrivateTransitionCount());
            };
        Oot3d::DisplayEffectExecutionLedger effectExecution(effectPlan);
        using EffectOutcome = Oot3d::DisplayEffectExecutionOutcome;
        // Every declared pass of this boundary is accounted for exactly once,
        // including the boundary composition that a native consumer samples.
        const auto recordDeclared = [&](Oot3d::DisplayEffectPass pass,
                                        EffectOutcome outcome) {
            slowSteps.Mark(Oot3d::DisplayEffectPassName(pass));
            if (effectPlan.Enabled(pass) &&
                !effectExecution.Record(pass, outcome)) {
                throw std::runtime_error(
                    "display effect execution ledger rejected " +
                    std::string(Oot3d::DisplayEffectPassName(pass)));
            }
        };
        const auto producedOutcome = [](bool attempted, bool produced,
                                        bool reused = false) {
            if (produced) {
                return attempted ? EffectOutcome::Executed
                                 : EffectOutcome::Reused;
            }
            if (reused) {
                return EffectOutcome::Reused;
            }
            return attempted ? EffectOutcome::Failed
                             : EffectOutcome::Skipped;
        };
        const bool displayEffectsPreviouslyPresented =
            mNativePicaPresentedThisFrame;
        mDiagnostics.RecordEffectGraph(
            static_cast<uint32_t>(effectPlan.Graph.Passes.size()),
            static_cast<uint32_t>(effectPlan.Graph.ResourceLifetimes.size()),
            static_cast<uint32_t>(std::min<size_t>(
                effectPlan.DeclaredBindingCount(),
                std::numeric_limits<uint32_t>::max())),
            static_cast<uint32_t>(effectPlan.Graph.Barriers.size()));
        const bool wantsAmbientOcclusion = effectPlan.Enabled(
            Oot3d::DisplayEffectPass::AmbientOcclusion);
        const bool wantsOutline = effectPlan.Enabled(
            Oot3d::DisplayEffectPass::Outline);
        const bool wantsReflections = effectPlan.Enabled(
            Oot3d::DisplayEffectPass::Reflections);
        const bool wantsHiZ = effectPlan.Enabled(
            Oot3d::DisplayEffectPass::HiZ);
        const bool wantsWorkingColor = effectPlan.Enabled(
            Oot3d::DisplayEffectPass::WorkingColor);
        const bool wantsFidelityFxSssr =
            wantsReflections && effectPlan.ReflectionProvider ==
                Oot3d::ReflectionMode::FidelityFxSssr;
        const bool wantsTemporalMetadata = effectPlan.TemporalMetadata;
        const bool wantsTaa = effectPlan.Enabled(
            Oot3d::DisplayEffectPass::Taa);
        const bool wantsSmaa = effectPlan.Enabled(
            Oot3d::DisplayEffectPass::Smaa);
        const bool wantsNis = effectPlan.Enabled(
            Oot3d::DisplayEffectPass::Nis);
        const bool wantsTemporalUpscaler = effectPlan.Enabled(
            Oot3d::DisplayEffectPass::TemporalUpscaler);
        const bool wantsFsr = wantsTemporalUpscaler &&
            effectPlan.Upscaler == Oot3d::UpscalerProvider::Fsr;
        const bool wantsDlss = wantsTemporalUpscaler &&
            effectPlan.Upscaler == Oot3d::UpscalerProvider::Dlss;
        const bool wantsMotionVectors = effectPlan.Enabled(
            Oot3d::DisplayEffectPass::Motion);
        const bool wantsGlobalIllumination = effectPlan.Enabled(
            Oot3d::DisplayEffectPass::GlobalIllumination);
        NativePicaRenderTarget* effectGuideTarget = nullptr;
        if (copyBoundary) {
            // A boundary declares its own source: the target whose scene it
            // composes, not a presentation transfer's display mapping.
            if (copySource != nullptr &&
                copySource->DepthView != VK_NULL_HANDLE) {
                PrepareNativePicaTargetAttachments(*copySource);
                effectGuideTarget = copySource;
            }
        } else if (!effectPlan.Graph.Attachments.NativeColorOnly()) {
            const auto depthAssociation = mNativePicaDisplayDepthTargets.find(
                std::make_pair(transfer.RenderTargetNamespace,
                               transfer.OutputPhysicalAddress));
            if (depthAssociation != mNativePicaDisplayDepthTargets.end()) {
                const auto target =
                    mNativePicaRenderTargets.find(depthAssociation->second);
                if (target != mNativePicaRenderTargets.end() &&
                    target->second.DepthView != VK_NULL_HANDLE) {
                    PrepareNativePicaTargetAttachments(target->second);
                    effectGuideTarget = &target->second;
                }
            }
        }
        const Oot3d::NriEffectGraphTransientImageBinding*
            transientMotionOutput = nullptr;
        const Oot3d::NriEffectGraphTransientImageBinding*
            transientReactiveOutput = nullptr;
        const Oot3d::NriEffectGraphTransientImageBinding*
            transientAmbientOcclusionOutput = nullptr;
        const Oot3d::NriEffectGraphTransientImageBinding*
            transientHiZOutput = nullptr;
        const Oot3d::NriEffectGraphTransientImageBinding*
            transientLinearWorkingColor = nullptr;
        const Oot3d::NriEffectGraphTransientImageBinding*
            transientReflectionOutput = nullptr;
        const Oot3d::NriEffectGraphTransientImageBinding*
            transientDiffuseIllumination = nullptr;
        const Oot3d::NriEffectGraphTransientImageBinding*
            transientSpecularIllumination = nullptr;
        const Oot3d::NriEffectGraphTransientImageBinding*
            transientCompositeOutput = nullptr;
        const Oot3d::NriEffectGraphTransientImageBinding*
            transientAntiAliasedOutput = nullptr;
        const Oot3d::NriEffectGraphTransientImageBinding*
            transientUpscaledOutput = nullptr;
        const bool smaaReadsComposite = effectPlan.Enabled(
            Oot3d::DisplayEffectPass::Composite);
        const uint32_t plannedSmaaWidth =
            smaaReadsComposite && effectGuideTarget != nullptr
                ? effectGuideTarget->Width
                : display.Width;
        const uint32_t plannedSmaaHeight =
            smaaReadsComposite && effectGuideTarget != nullptr
                ? effectGuideTarget->Height
                : display.Height;
        // The working colour converts the scene colour at that colour's
        // extent, unless a scene stage traced on the guide target (SSSR)
        // reads it; a native display transfer may have resized the colour.
        const bool workingColorOnGuideTarget =
            effectGuideTarget != nullptr && wantsFidelityFxSssr;
        const uint32_t plannedWorkingColorWidth = workingColorOnGuideTarget
            ? effectGuideTarget->Width : display.Width;
        const uint32_t plannedWorkingColorHeight = workingColorOnGuideTarget
            ? effectGuideTarget->Height : display.Height;
        // An upscaler's render resolution is its colour input's extent: the
        // display (SceneColor), the working colour, or a composition
        // allocated on the scene target.
        const auto colorInput = [&effectPlan](Oot3d::DisplayEffectPass pass) {
            for (const auto& use : effectPlan.Bindings(pass)) {
                if (Oot3d::ReadsEffectResource(use.Access) &&
                    Oot3d::EffectResourceCarriesColor(use.Resource))
                    return use.Resource;
            }
            return Oot3d::EffectResource::SceneColor;
        };
        const auto upscalerColor = colorInput(wantsTemporalUpscaler
            ? Oot3d::DisplayEffectPass::TemporalUpscaler
            : Oot3d::DisplayEffectPass::Nis);
        // The display and its scene before native UI share the display's
        // extent; the working colour has its planned one; a composition on the
        // scene target has the target's.
        const bool upscalerReadsGuideDomain = effectGuideTarget != nullptr &&
            (wantsTemporalUpscaler || wantsNis) &&
            upscalerColor != Oot3d::EffectResource::SceneColor &&
            upscalerColor != Oot3d::EffectResource::SceneBeforeUi &&
            upscalerColor != Oot3d::EffectResource::LinearWorkingColor;
        const uint32_t plannedUpscalerInputWidth = upscalerReadsGuideDomain
            ? effectGuideTarget->Width
            : upscalerColor == Oot3d::EffectResource::LinearWorkingColor
                ? plannedWorkingColorWidth : display.Width;
        const uint32_t plannedUpscalerInputHeight = upscalerReadsGuideDomain
            ? effectGuideTarget->Height
            : upscalerColor == Oot3d::EffectResource::LinearWorkingColor
                ? plannedWorkingColorHeight : display.Height;
        const auto plannedUpscalerContract =
            Oot3d::ResolveUpscalerContractFromRender(
                effectPlan.Upscaler, presentationSettings.UpscalerMode,
                plannedUpscalerInputWidth, plannedUpscalerInputHeight);
        const uint32_t plannedUpscalerWidth =
            plannedUpscalerContract.Output.Width;
        const uint32_t plannedUpscalerHeight =
            plannedUpscalerContract.Output.Height;
        std::array<Oot3d::EffectTransientImageRequirement, 11U>
            requirements{};
        size_t requirementCount = 0U;
        const auto requireImageWithMips = [
            &requirements, &requirementCount](
                Oot3d::EffectResource resource,
                VkFormat format, uint32_t width, uint32_t height,
                uint32_t mipLevels) {
            requirements[requirementCount++] = {
                resource,
                static_cast<uint32_t>(format),
                width,
                height,
                mipLevels,
                1U,
                static_cast<uint32_t>(VK_SAMPLE_COUNT_1_BIT),
                static_cast<uint32_t>(
                    VK_IMAGE_USAGE_STORAGE_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT),
            };
        };
        const auto requireImage = [&requireImageWithMips](
            Oot3d::EffectResource resource, VkFormat format,
            uint32_t width, uint32_t height) {
            requireImageWithMips(resource, format, width, height, 1U);
        };
        const auto requireRgba16f = [&requireImage](
            Oot3d::EffectResource resource,
            uint32_t width, uint32_t height) {
            requireImage(resource, VK_FORMAT_R16G16B16A16_SFLOAT,
                         width, height);
        };
        if (wantsMotionVectors && effectGuideTarget != nullptr) {
            requireRgba16f(
                Oot3d::EffectResource::MotionVectors,
                effectGuideTarget->Width, effectGuideTarget->Height);
            requireImage(
                Oot3d::EffectResource::ReactiveMask,
                VK_FORMAT_R16_SFLOAT,
                effectGuideTarget->Width, effectGuideTarget->Height);
        }
        if (wantsAmbientOcclusion && effectGuideTarget != nullptr) {
            requireImage(
                Oot3d::EffectResource::AmbientOcclusion,
                VK_FORMAT_R32_SFLOAT,
                effectGuideTarget->Width, effectGuideTarget->Height);
        }
        if (wantsHiZ && effectGuideTarget != nullptr) {
            requireImageWithMips(
                Oot3d::EffectResource::HierarchicalDepth,
                VK_FORMAT_R32_SFLOAT,
                effectGuideTarget->Width, effectGuideTarget->Height,
                Oot3d::ResolveHiZMipCount(
                    effectGuideTarget->Width,
                    effectGuideTarget->Height));
        }
        if (wantsWorkingColor && effectGuideTarget != nullptr) {
            requireRgba16f(
                Oot3d::EffectResource::LinearWorkingColor,
                plannedWorkingColorWidth, plannedWorkingColorHeight);
        }
        if (wantsReflections && effectGuideTarget != nullptr) {
            requireRgba16f(
                Oot3d::EffectResource::ReflectionColor,
                effectGuideTarget->Width, effectGuideTarget->Height);
        }
        if (wantsGlobalIllumination && effectGuideTarget != nullptr) {
            requireRgba16f(
                Oot3d::EffectResource::DiffuseIllumination,
                effectGuideTarget->Width, effectGuideTarget->Height);
            if (effectPlan.ReflectionsFromGlobalIllumination ||
                effectPlan.ReflectionMissesFromGlobalIllumination)
                requireRgba16f(
                    Oot3d::EffectResource::SpecularIllumination,
                    effectGuideTarget->Width, effectGuideTarget->Height);
        }
        if (effectPlan.Enabled(Oot3d::DisplayEffectPass::Composite) &&
            effectGuideTarget != nullptr) {
            requireRgba16f(
                Oot3d::EffectResource::CompositeColor,
                effectGuideTarget->Width, effectGuideTarget->Height);
        }
        if (wantsSmaa) {
            requireRgba16f(
                Oot3d::EffectResource::AntiAliasedColor,
                plannedSmaaWidth, plannedSmaaHeight);
        }
        if (wantsTemporalUpscaler || wantsNis) {
            requireRgba16f(
                Oot3d::EffectResource::UpscaledColor,
                plannedUpscalerWidth, plannedUpscalerHeight);
        }
        // An inactive graph declares no new bindings; it does not retire the
        // resources used by an earlier native composition anchor. Each plan
        // that recurs (a native boundary and the presentations of one frame)
        // keeps its own images. Replacing a cached configuration destroys
        // images that recorded commands may still use, so DeviceWaitIdle alone
        // cannot authorize it: it first completes the recorded frame prefix
        // and releases the providers' descriptors.
        if (requirementCount != 0U) {
            const std::span<const Oot3d::EffectTransientImageRequirement>
                activeRequirements(requirements.data(), requirementCount);
            const auto allocationPlan =
                Oot3d::BuildEffectTransientAllocationPlan(
                    effectPlan.Graph, activeRequirements);
            if (mNriEffectGraphTransientImageArena.ReplacesConfigurationFor(
                    allocationPlan)) {
                if (!mNriInterop.CompleteRecordedWork())
                    throw std::runtime_error(
                        "effect graph image clients: recorded work did not complete");
                ReleaseEffectGraphImageClients();
            }
            const bool arenaConfigured =
                mNriEffectGraphTransientImageArena.Configure(allocationPlan);
            const auto& arenaStats =
                mNriEffectGraphTransientImageArena.Stats();
            mDiagnostics.RecordEffectTransientImageArena(
                allocationPlan.Summary(), arenaConfigured,
                arenaStats.LastAllocatedSlotCount,
                arenaStats.LastConfigurationReused);
            if (arenaConfigured) {
                transientMotionOutput =
                    mNriEffectGraphTransientImageArena.Find(
                        Oot3d::EffectResource::MotionVectors);
                transientReactiveOutput =
                    mNriEffectGraphTransientImageArena.Find(
                        Oot3d::EffectResource::ReactiveMask);
                transientAmbientOcclusionOutput =
                    mNriEffectGraphTransientImageArena.Find(
                        Oot3d::EffectResource::AmbientOcclusion);
                transientHiZOutput =
                    mNriEffectGraphTransientImageArena.Find(
                        Oot3d::EffectResource::HierarchicalDepth);
                transientLinearWorkingColor =
                    mNriEffectGraphTransientImageArena.Find(
                        Oot3d::EffectResource::LinearWorkingColor);
                transientReflectionOutput =
                    mNriEffectGraphTransientImageArena.Find(
                        Oot3d::EffectResource::ReflectionColor);
                transientDiffuseIllumination =
                    mNriEffectGraphTransientImageArena.Find(
                        Oot3d::EffectResource::DiffuseIllumination);
                transientSpecularIllumination =
                    effectPlan.ReflectionsFromGlobalIllumination ||
                            effectPlan.ReflectionMissesFromGlobalIllumination
                        ? mNriEffectGraphTransientImageArena.Find(
                              Oot3d::EffectResource::SpecularIllumination)
                        : nullptr;
                transientCompositeOutput =
                    mNriEffectGraphTransientImageArena.Find(
                        Oot3d::EffectResource::CompositeColor);
                transientAntiAliasedOutput =
                    mNriEffectGraphTransientImageArena.Find(
                        Oot3d::EffectResource::AntiAliasedColor);
                transientUpscaledOutput =
                    mNriEffectGraphTransientImageArena.Find(
                        Oot3d::EffectResource::UpscaledColor);
            }
        }
        bool guidesMaterialized = effectGuideTarget != nullptr;
        if (guidesMaterialized) {
            for (const auto& binding : effectPlan.Bindings(
                     Oot3d::DisplayEffectPass::Guides)) {
                if (!Oot3d::WritesEffectResource(binding.Access)) {
                    continue;
                }
                switch (binding.Resource) {
                    case Oot3d::EffectResource::NormalGuide:
                        guidesMaterialized =
                            effectGuideTarget->NormalGuideView !=
                            VK_NULL_HANDLE;
                        break;
                    case Oot3d::EffectResource::MaterialGuide:
                        guidesMaterialized =
                            effectGuideTarget->MaterialGuideView !=
                            VK_NULL_HANDLE;
                        break;
                    case Oot3d::EffectResource::RigidMotionGuide:
                        guidesMaterialized =
                            effectGuideTarget->RigidMotionGuideView !=
                            VK_NULL_HANDLE;
                        break;
                    case Oot3d::EffectResource::AmbientGuide:
                        guidesMaterialized =
                            effectGuideTarget->AmbientGuideView !=
                            VK_NULL_HANDLE;
                        break;
                    case Oot3d::EffectResource::FogGuide:
                        guidesMaterialized = effectGuideTarget->FogGuideView != VK_NULL_HANDLE;
                        break;
                    case Oot3d::EffectResource::OutlineGeometryGuide:
                        guidesMaterialized = effectGuideTarget->OutlineGeometryGuideView != VK_NULL_HANDLE;
                        break;
                    case Oot3d::EffectResource::DiffuseAlbedoGuide:
                        guidesMaterialized = effectGuideTarget->DiffuseAlbedoGuideView != VK_NULL_HANDLE;
                        break;
                    default:
                        guidesMaterialized = false;
                        break;
                }
                if (!guidesMaterialized) {
                    break;
                }
            }
        }
        Oot3d::DisplayEffectResourceTable effectResources;
        const auto bindEffectImage = [&effectResources, this](
            Oot3d::EffectResource resource, VkImage image,
            VkImageView view, VkFormat format, uint32_t width,
            uint32_t height,
            Oot3d::SceneColorEncoding colorEncoding =
                Oot3d::SceneColorEncoding::Unknown) {
            if (!effectResources.BindImage(
                    resource, reinterpret_cast<uintptr_t>(image),
                    reinterpret_cast<uintptr_t>(view),
                    static_cast<uint32_t>(format), width, height,
                    mFrameCounter, colorEncoding)) {
                throw std::runtime_error(
                    "display effect resource binding is incomplete");
            }
        };
        const auto bindEffectSemanticObject = [&effectResources](
            Oot3d::EffectResource resource, const void* object,
            uint32_t schemaVersion, uint64_t generation) {
            if (!effectResources.BindSemanticObject(
                    resource, object, schemaVersion, generation)) {
                throw std::runtime_error(
                    "display effect semantic object binding is incomplete");
            }
        };
        bindEffectImage(
            Oot3d::EffectResource::SceneColor,
            display.Image, display.View,
            VK_FORMAT_R8G8B8A8_UNORM,
            display.Width, display.Height,
            Oot3d::NativePicaSceneColorEncoding());
        if (sceneBeforeUi != nullptr) {
            bindEffectImage(
                Oot3d::EffectResource::SceneBeforeUi,
                sceneBeforeUi->Image, sceneBeforeUi->View,
                VK_FORMAT_R8G8B8A8_UNORM,
                sceneBeforeUi->Width, sceneBeforeUi->Height,
                Oot3d::NativePicaSceneColorEncoding());
        }
        bindEffectImage(
            Oot3d::EffectResource::PresentationOutput,
            mSwapchainImages[mCurrentImage],
            mSwapchainImageViews[mCurrentImage], mSwapchainFormat,
            mSwapchainExtent.width, mSwapchainExtent.height);
        if (mPicaSceneFrame.Active() &&
            mPicaSceneFrame.FrameId() != 0U) {
            bindEffectSemanticObject(
                Oot3d::EffectResource::PicaSceneFrame,
                &mPicaSceneFrame,
                Oot3d::PicaSceneFrame::kSchemaVersion,
                mPicaSceneFrame.FrameId());
        }
        if (mNativeSceneView.Active()) {
            bindEffectSemanticObject(
                Oot3d::EffectResource::NativeSceneView,
                &mNativeSceneView,
                Oot3d::NativeSceneView::kSchemaVersion,
                mNativeSceneView.Generation());
        }
        if (effectGuideTarget != nullptr) {
            bindEffectImage(
                Oot3d::EffectResource::NativeDepth,
                effectGuideTarget->DepthImage,
                effectGuideTarget->DepthView, mDepthFormat,
                effectGuideTarget->Width, effectGuideTarget->Height);
            const auto bindGuide = [&](Oot3d::EffectResource resource,
                                       VkImage image, VkImageView view,
                                       VkFormat format) {
                if (image != VK_NULL_HANDLE && view != VK_NULL_HANDLE) {
                    bindEffectImage(
                        resource, image, view, format,
                        effectGuideTarget->Width,
                        effectGuideTarget->Height);
                }
            };
            bindGuide(
                Oot3d::EffectResource::NormalGuide,
                effectGuideTarget->NormalGuideImage,
                effectGuideTarget->NormalGuideView,
                VK_FORMAT_R8G8B8A8_UNORM);
            bindGuide(
                Oot3d::EffectResource::MaterialGuide,
                effectGuideTarget->MaterialGuideImage,
                effectGuideTarget->MaterialGuideView,
                VK_FORMAT_R8G8B8A8_UNORM);
            bindGuide(
                Oot3d::EffectResource::RigidMotionGuide,
                effectGuideTarget->RigidMotionGuideImage,
                effectGuideTarget->RigidMotionGuideView,
                VK_FORMAT_R16G16B16A16_SFLOAT);
            bindGuide(
                Oot3d::EffectResource::AmbientGuide,
                effectGuideTarget->AmbientGuideImage,
                effectGuideTarget->AmbientGuideView,
                VK_FORMAT_R8G8B8A8_UNORM);
            bindGuide(Oot3d::EffectResource::FogGuide,
                effectGuideTarget->FogGuideImage, effectGuideTarget->FogGuideView,
                VK_FORMAT_R8G8B8A8_UNORM);
            bindGuide(Oot3d::EffectResource::OutlineGeometryGuide, effectGuideTarget->OutlineGeometryGuideImage,
                      effectGuideTarget->OutlineGeometryGuideView, VK_FORMAT_R32G32B32A32_SFLOAT);
            bindGuide(Oot3d::EffectResource::DiffuseAlbedoGuide, effectGuideTarget->DiffuseAlbedoGuideImage,
                      effectGuideTarget->DiffuseAlbedoGuideView, VK_FORMAT_R16G16B16A16_UNORM);
        }
        const auto resolveEffectPassReads = [
            &effectPlan, &effectResources, this](
                Oot3d::DisplayEffectPass pass) {
            auto reads = effectResources.ResolveReads(
                effectPlan.Bindings(pass));
            mDiagnostics.RecordDisplayEffectGraphBindings(
                reads.Validation, false);
            if (!reads.Complete()) {
                throw std::runtime_error(
                    "display effect pass has unresolved declared reads: " +
                    std::string(Oot3d::DisplayEffectPassName(pass)));
            }
            return reads;
        };
        const auto requireEffectImageRead = [](
            const Oot3d::EffectResourceReadSet& reads,
            Oot3d::EffectResource resource)
                -> const Oot3d::EffectResourceBinding& {
            const auto* binding = reads.Find(resource);
            if (binding == nullptr ||
                binding->Kind !=
                    Oot3d::EffectResourceBindingKind::Image) {
                throw std::runtime_error(
                    "display effect pass used an undeclared or non-image read");
            }
            return *binding;
        };
        const auto findEffectColorRead = [&effectPlan](
            const Oot3d::EffectResourceReadSet& reads,
            Oot3d::DisplayEffectPass pass)
                -> const Oot3d::EffectResourceBinding* {
            return reads.FindFirstColorRead(effectPlan.Bindings(pass));
        };
        std::optional<Oot3d::TemporalViewState> temporalState;
        // A native boundary resolves the same temporal state the display that
        // presents this view will use: TemporalHistoryManager::Prepare is keyed
        // by view and frame, so it returns that frame's state instead of
        // advancing history a second time. A scene stage that needs motion
        // vectors (SSSR) therefore executes at the boundary with the frame's own
        // jitter, and the presentation keeps its history untouched.
        if (wantsTemporalMetadata && effectGuideTarget != nullptr) {
            Oot3d::TemporalViewInput temporalInput{};
            temporalInput.Key = {
                effectGuideTarget->Key.RenderTargetNamespace,
                effectGuideTarget->Key.ColorPhysicalAddress,
                effectGuideTarget->Key.DepthPhysicalAddress};
            temporalInput.RenderedFrameId = mFrameCounter;
            temporalInput.SourceResetEpoch =
                nativeTemporalSample.Available() ? nativeTemporalSample.ContinuityEpoch : 0U;
            temporalInput.Width = effectGuideTarget->Width;
            temporalInput.Height = effectGuideTarget->Height;
            temporalInput.Projection = presentationPerspective->Projection;
            temporalInput.WorldToClip =
                presentationPerspective->WorldToClip;
            temporalInput.Eye = presentationPerspective->Eye;
            temporalInput.At = presentationPerspective->At;
            temporalInput.JitterUv = {
                mTemporalJitterPixels[0] /
                    static_cast<float>(effectGuideTarget->Width),
                mTemporalJitterPixels[1] /
                    static_cast<float>(effectGuideTarget->Height)};
            temporalState = mTemporalHistory.Prepare(temporalInput);
            mDiagnostics.RecordTemporalHistory(
                temporalState->HistoryValid, temporalState->CameraCut,
                static_cast<uint32_t>(temporalState->ResetReason));
        }
        const bool outlineForDisplay =
            wantsOutline && effectGuideTarget != nullptr;
        bool reflectionsForDisplay =
            wantsReflections && effectGuideTarget != nullptr &&
            effectGuideTarget->NormalGuideView != VK_NULL_HANDLE &&
            effectGuideTarget->MaterialGuideView != VK_NULL_HANDLE;
        // GI-sourced reflections without GI for this sample: the declared
        // reflection holds no reflection and is not composed.
        bool reflectionsSkipped = false;
        bool reflectionNriWrapped = false;
        uint64_t guideSampledResourceMask = 0U;
        const auto transitionEffectPassGuides = [
            &effectPlan, &effectResources, &guideSampledResourceMask,
            &command, this](Oot3d::DisplayEffectPass pass) {
            const auto* compiledPass = effectPlan.FindPass(pass);
            if (compiledPass == nullptr) {
                return;
            }
            const auto guideBarriers =
                Oot3d::BuildPicaGuideSamplingBarriers(
                    *compiledPass, effectResources,
                    guideSampledResourceMask);
            if (!guideBarriers.Complete()) {
                throw std::runtime_error(
                    "display effect graph has unresolved PICA guide barriers");
            }
            guideSampledResourceMask |=
                guideBarriers.ResourceMask;
            mDiagnostics.RecordEffectGraphGuideImageBarriers(
                guideBarriers.Count);
            if (guideBarriers.Count != 0U) {
                vkCmdPipelineBarrier(
                    command, guideBarriers.SourceStages,
                    guideBarriers.DestinationStages,
                    0, 0, nullptr, 0,
                    nullptr,
                    guideBarriers.Count,
                    guideBarriers.Images.data());
            }
        };
        // Every guide this graph sampled returns to its attachment role. A
        // boundary that composes mid-frame must do this before it returns,
        // because the native draws that follow it write those same guides.
        const auto restoreEffectPassGuides = [
            &effectResources, &guideSampledResourceMask,
            &command, this]() {
            if (guideSampledResourceMask == 0U) {
                return;
            }
            const auto guideBarriers =
                Oot3d::BuildPicaGuideAttachmentBarriers(
                    effectResources, guideSampledResourceMask);
            if (!guideBarriers.Complete()) {
                throw std::runtime_error(
                    "display effect graph has unresolved PICA guide barriers");
            }
            mDiagnostics.RecordEffectGraphGuideImageBarriers(
                guideBarriers.Count);
            if (guideBarriers.Count != 0U) {
                vkCmdPipelineBarrier(
                    command, guideBarriers.SourceStages,
                    guideBarriers.DestinationStages,
                    0, 0, nullptr, 0, nullptr,
                    guideBarriers.Count,
                    guideBarriers.Images.data());
            }
        };
        // A boundary samples the source's own colour target, which the native
        // draw path leaves in its attachment layout: the scene stages read it as
        // a texture, and the draws that follow attach it again. Same barrier
        // pair the native encode path uses for this image. Only a plan that
        // declares a read of that colour is transitioned: a plan whose scene
        // stages are already resolved for this version declares none.
        const bool boundarySamplesSourceColor =
            copyBoundary && copySource != nullptr &&
            copySource->ColorImage != VK_NULL_HANDLE &&
            std::any_of(
                effectPlan.Graph.Passes.begin(),
                effectPlan.Graph.Passes.end(),
                [](const Oot3d::CompiledEffectPass& pass) {
                    return std::any_of(
                        pass.Resources.begin(), pass.Resources.end(),
                        [](const Oot3d::EffectResourceUse& use) {
                            return use.Resource ==
                                       Oot3d::EffectResource::SceneColor &&
                                   Oot3d::ReadsEffectResource(use.Access);
                        });
                });
        const auto setSourceColorLayout = [&command, this](
            VkImage image, VkImageLayout from, VkImageLayout to,
            VkPipelineStageFlags sourceStage,
            VkPipelineStageFlags destinationStage,
            VkAccessFlags sourceAccess, VkAccessFlags destinationAccess) {
            VkImageMemoryBarrier barrier{
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.oldLayout = from;
            barrier.newLayout = to;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = {
                VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
            barrier.srcAccessMask = sourceAccess;
            barrier.dstAccessMask = destinationAccess;
            vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0U,
                                 0U, nullptr, 0U, nullptr, 1U, &barrier);
        };
        if (boundarySamplesSourceColor) {
            setSourceColorLayout(
                copySource->ColorImage,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT);
        }
        const auto restoreBoundarySourceColor = [&]() {
            if (!boundarySamplesSourceColor) {
                return;
            }
            setSourceColorLayout(
                copySource->ColorImage,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        };
        if (presentationPerspective.has_value() && !mSceneDistanceFieldUpdatedThisFrame &&
            mBrixelizerScene.Available() &&
            Oot3d::PicaExtensionPassEnabled(
                mPicaExtensionGraph, Oot3d::PicaExtensionPass::SceneDistanceField)) {
            mSceneDistanceFieldUpdatedThisFrame = true;
            if (mTitleWorldChanged) {
                // Another world: the field and every GI history start over
                // (this frame's collection too; the next frame refills it).
                mTitleWorldChanged = false;
                mBrixelizerGiPass.ResetHistory();
                mBrixelizerScene.Reset();
            }
            // Developer inspection: OOT3D_GRAPHICS_BRIXELIZER_DEBUG=<directory>
            // writes the field's debug view every 150 updates, in the storage
            // orientation of the scene guides (OOT3D_GRAPHICS_BRIXELIZER_DEBUG_MODE,
            // FFX_BRIXELIZER_TRACE_DEBUG_MODE_*, default 3 = normals).
            std::optional<Oot3d::BrixelizerDebugView> debugView;
            if (const char* directory = std::getenv("OOT3D_GRAPHICS_BRIXELIZER_DEBUG");
                directory != nullptr && *directory != '\0' && transfer.InputWidth != 0U &&
                (mBrixelizerScene.Stats().Updates + 1U) % 150U == 0U) {
                const auto coordinates = Oot3d::PicaSurfaceCoordinates::FromTransferFlags(transfer.Flags);
                auto& view = debugView.emplace();
                view.Projection = coordinates.FidelityFxClip(presentationPerspective->Projection);
                view.WorldToClip = coordinates.FidelityFxClip(presentationPerspective->WorldToClip);
                // Native storage extent at 3x (scene guides may not exist).
                view.Width = transfer.InputWidth * 3U;
                view.Height = transfer.InputHeight * 3U;
                if (const char* mode = std::getenv("OOT3D_GRAPHICS_BRIXELIZER_DEBUG_MODE"); mode != nullptr)
                    view.Mode = static_cast<uint32_t>(std::strtoul(mode, nullptr, 10));
                view.Path = std::string(directory) + "/sdf-u" +
                    std::to_string(mBrixelizerScene.Stats().Updates + 1U) + "-m" + std::to_string(view.Mode) + ".ppm";
            }
            if (mBrixelizerScene.Update(command, mCurrentFrame, presentationPerspective->Eye,
                                        debugView ? &*debugView : nullptr)) {
                const auto& field = mBrixelizerScene.Stats();
                if (field.Updates % 150U == 1U) {
                    std::fprintf(stderr,
                        "TRIAEVUM_SCENE_DISTANCE_FIELD update=%llu collected=%llu rejected=%llu static=%u "
                        "created=%llu evicted=%llu dynamic=%u dynamic_triangles=%llu meshes=%u "
                        "arena_vertex_kib=%llu arena_index_kib=%llu scratch_mib=%llu free_bricks=%u "
                        "static_bricks=%u dynamic_bricks=%u peak_references=%u peak_triangles=%u "
                        "peak_bricks=%u overflows=%llu gpu_ms_mean=%.3f gpu_ms_max=%.3f\n",
                        (unsigned long long)field.Updates, (unsigned long long)field.CollectedInstances,
                        (unsigned long long)field.RejectedInstances, field.StaticInstances,
                        (unsigned long long)field.StaticInstancesCreated,
                        (unsigned long long)field.StaticInstancesEvicted, field.DynamicInstances,
                        (unsigned long long)field.DynamicTriangles, field.ResidentMeshes,
                        (unsigned long long)(field.ArenaVertexBytes >> 10),
                        (unsigned long long)(field.ArenaIndexBytes >> 10),
                        (unsigned long long)(field.GpuScratchBytes >> 20), field.FreeBricks,
                        field.StaticBricksAllocated, field.DynamicBricksAllocated, field.PeakReferences,
                        field.PeakTriangles, field.PeakBricks, (unsigned long long)field.BudgetOverflows,
                        field.GpuSamples != 0U ? field.GpuMillisecondsTotal / double(field.GpuSamples) : 0.0,
                        field.GpuMillisecondsMax);
                }
            } else if (!mBrixelizerScene.UnavailableReason().empty()) {
                std::fprintf(stderr, "TRIAEVUM_SCENE_DISTANCE_FIELD error=\"%s\"\n",
                             mBrixelizerScene.UnavailableReason().c_str());
            }
        }
        // Motion vectors precede the scene stages that read them: temporal AO
        // and GI reproject their history with this frame's motion.
        bool motionNriWrapped = false;
        std::optional<Oot3d::SceneSurfaceKey> motionKey;
        if (effectGuideTarget != nullptr) {
            motionKey = Oot3d::SceneSurfaceKey{
                transfer.RenderTargetNamespace,
                effectGuideTarget->Key.ColorPhysicalAddress,
                Oot3d::SceneSurfaceKind::Motion};
        }
        const bool canReuseMotion =
            wantsMotionVectors && mMotionExecutedThisFrame &&
            motionKey.has_value() && mMotionSurface.has_value() &&
            *mMotionSurface == *motionKey;
        bool motionForDisplay = wantsMotionVectors && temporalState.has_value() &&
                                effectGuideTarget != nullptr &&
                                !mMotionExecutedThisFrame;
        const bool motionAttempted = motionForDisplay;
        if (motionForDisplay) {
            const auto motionReads = resolveEffectPassReads(
                Oot3d::DisplayEffectPass::Motion);
            const auto& motionDepthBinding = requireEffectImageRead(
                motionReads,
                Oot3d::EffectResource::NativeDepth);
            const auto& motionMaterialBinding = requireEffectImageRead(
                motionReads,
                Oot3d::EffectResource::MaterialGuide);
            const auto& motionRigidBinding = requireEffectImageRead(
                motionReads,
                Oot3d::EffectResource::RigidMotionGuide);
            const VkImage motionDepthImage =
                reinterpret_cast<VkImage>(motionDepthBinding.NativeImage);
            const VkFormat motionDepthFormat =
                static_cast<VkFormat>(motionDepthBinding.Format);
            const VkImage motionMaterialImage =
                reinterpret_cast<VkImage>(motionMaterialBinding.NativeImage);
            const VkFormat motionMaterialFormat =
                static_cast<VkFormat>(motionMaterialBinding.Format);
            const VkImage motionRigidImage =
                reinterpret_cast<VkImage>(motionRigidBinding.NativeImage);
            const VkFormat motionRigidFormat =
                static_cast<VkFormat>(motionRigidBinding.Format);
            if (mMotionSurface.has_value() &&
                *mMotionSurface != *motionKey) {
                mSceneSurfaces.Retire(*mMotionSurface);
                mMotionSurface.reset();
            }
            const auto convention = effectGuideTarget->WBuffering
                ? Oot3d::DepthConvention::WBuffer
                : Oot3d::DepthConvention::Perspective;
            transitionEffectPassGuides(
                Oot3d::DisplayEffectPass::Motion);
            mGpuProfiler.BeginScope(
                mCurrentFrame,
                Oot3d::GpuProfileScope::MotionVectors,
                command);
            motionForDisplay =
                transientMotionOutput != nullptr &&
                transientReactiveOutput != nullptr &&
                mMotionVectorPass.Configure(
                    *transientMotionOutput, *transientReactiveOutput,
                    motionDepthImage, motionDepthFormat,
                    motionMaterialImage, motionMaterialFormat,
                    motionRigidImage, motionRigidFormat) &&
                mMotionVectorPass.Execute(command, mCurrentFrame,
                    *temporalState, *presentationPerspective, convention,
                    motionBarrierPlan,
                    Oot3d::PicaSurfaceCoordinates::FromTransferFlags(transfer.Flags));
            mGpuProfiler.EndScope(
                mCurrentFrame,
                Oot3d::GpuProfileScope::MotionVectors,
                command);
            if (motionForDisplay) {
                const VkImage motionImage = mMotionVectorPass.OutputImage();
                mSceneSurfaces.Publish({*motionKey,
                    reinterpret_cast<uintptr_t>(motionImage),
                    effectGuideTarget->Width, effectGuideTarget->Height,
                    static_cast<uint32_t>(VK_FORMAT_R16G16B16A16_SFLOAT),
                    0, true});
                motionNriWrapped = mNriInterop.WrapTexture(
                    motionImage, VK_FORMAT_R16G16B16A16_SFLOAT,
                    VK_IMAGE_TYPE_2D,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    effectGuideTarget->Width, effectGuideTarget->Height);
                mMotionSurface = *motionKey;
                mMotionExecutedThisFrame = true;
            }
            mDiagnostics.RecordMotionVectors(
                motionForDisplay, temporalState->HistoryValid,
                motionNriWrapped, mMotionVectorPass.OutputsOwnedByNri(),
                mMotionVectorPass.ComputeOwnedByNri(),
                mMotionVectorPass.BarriersOwnedByNri());
            if (motionForDisplay) {
                recordEffectPassTransitions(
                    mMotionVectorPass.LastBarrierExecution());
            }
        }
        const bool motionAvailableForDisplay =
            motionForDisplay || canReuseMotion;
        if (motionAvailableForDisplay) {
            bindEffectImage(
                Oot3d::EffectResource::MotionVectors,
                mMotionVectorPass.OutputImage(),
                mMotionVectorPass.OutputView(),
                VK_FORMAT_R16G16B16A16_SFLOAT,
                effectGuideTarget->Width, effectGuideTarget->Height);
            bindEffectImage(
                Oot3d::EffectResource::ReactiveMask,
                mMotionVectorPass.ReactiveImage(),
                mMotionVectorPass.ReactiveView(),
                VK_FORMAT_R16_SFLOAT,
                effectGuideTarget->Width, effectGuideTarget->Height);
        }
        const bool cacao = presentationSettings.Effects.AmbientOcclusion == Oot3d::AmbientOcclusionMode::Cacao;
        const bool mxao = presentationSettings.Effects.AmbientOcclusion == Oot3d::AmbientOcclusionMode::Mxao;
        bool aoAttempted = false;
        bool aoExecutedNow = false;
        // An AO that declares the motion vectors (temporal MXAO) runs with them.
        const auto aoBindings = effectPlan.Bindings(Oot3d::DisplayEffectPass::AmbientOcclusion);
        const bool aoReadsMotion = std::any_of(
            aoBindings.begin(), aoBindings.end(), [](const Oot3d::EffectResourceUse& use) {
                return Oot3d::ReadsEffectResource(use.Access) &&
                       use.Resource == Oot3d::EffectResource::MotionVectors;
            });
        if (wantsAmbientOcclusion && effectGuideTarget != nullptr &&
            !mAmbientOcclusionExecutedThisFrame &&
            (!aoReadsMotion || motionAvailableForDisplay)) {
            aoAttempted = true;
            mAmbientOcclusionExecutedThisFrame = true;
            const auto aoReads = resolveEffectPassReads(
                Oot3d::DisplayEffectPass::AmbientOcclusion);
            const auto& aoDepthBinding = requireEffectImageRead(
                aoReads,
                Oot3d::EffectResource::NativeDepth);
            const auto& aoNormalBinding = requireEffectImageRead(
                aoReads,
                Oot3d::EffectResource::NormalGuide);
            transitionEffectPassGuides(
                Oot3d::DisplayEffectPass::AmbientOcclusion);
            mGpuProfiler.BeginScope(
                mCurrentFrame, Oot3d::GpuProfileScope::AmbientOcclusion,
                command);
            mAmbientOcclusionOutputValid = transientAmbientOcclusionOutput != nullptr;
            if (mAmbientOcclusionOutputValid && cacao) {
                mAmbientOcclusionOutputValid =
                    mCacaoPass.Configure(*transientAmbientOcclusionOutput,
                        reinterpret_cast<VkImageView>(aoDepthBinding.NativeView),
                        reinterpret_cast<VkImageView>(aoNormalBinding.NativeView),
                        Oot3d::ResolveCacaoSettings(presentationSettings.Effects)) &&
                    mCacaoPass.Execute(command, presentationPerspective->Projection, aoBarrierPlan,
                        Oot3d::PicaSurfaceCoordinates::FromTransferFlags(transfer.Flags));
            } else if (mAmbientOcclusionOutputValid && mxao) {
                // Temporal MXAO reprojects its history with the declared
                // motion vectors of this frame.
                const auto* aoMotion = aoReads.Find(Oot3d::EffectResource::MotionVectors);
                const bool mxaoTemporal = aoMotion != nullptr &&
                    aoMotion->Kind == Oot3d::EffectResourceBindingKind::Image && temporalState.has_value();
                const Oot3d::MxaoMotionInput mxaoMotion = mxaoTemporal
                    ? Oot3d::MxaoMotionInput{reinterpret_cast<VkImage>(aoMotion->NativeImage),
                                             static_cast<VkFormat>(aoMotion->Format), aoMotion->Width,
                                             aoMotion->Height}
                    : Oot3d::MxaoMotionInput{};
                const Oot3d::MxaoTemporalFrame mxaoFrame{mxaoTemporal ? &*temporalState : nullptr, mFrameCounter};
                mAmbientOcclusionOutputValid =
                    mMxaoPass.Configure(*transientAmbientOcclusionOutput,
                        reinterpret_cast<VkImage>(aoDepthBinding.NativeImage),
                        static_cast<VkFormat>(aoDepthBinding.Format),
                        reinterpret_cast<VkImage>(aoNormalBinding.NativeImage),
                        static_cast<VkFormat>(aoNormalBinding.Format),
                        mxaoTemporal ? &mxaoMotion : nullptr) &&
                    mMxaoPass.Execute(mCurrentFrame, *presentationPerspective,
                        effectGuideTarget->WBuffering, presentationSettings.Effects, aoBarrierPlan,
                        Oot3d::PicaSurfaceCoordinates::FromTransferFlags(transfer.Flags),
                        mxaoTemporal ? &mxaoFrame : nullptr);
                mDiagnostics.RecordMxaoTemporal(mxaoTemporal, mMxaoPass.LastHistoryValid());
            } else if (mAmbientOcclusionOutputValid) {
                mAmbientOcclusionOutputValid =
                    mAmbientOcclusionPass.Configure(*transientAmbientOcclusionOutput,
                        reinterpret_cast<VkImage>(aoDepthBinding.NativeImage),
                        static_cast<VkFormat>(aoDepthBinding.Format),
                        reinterpret_cast<VkImage>(aoNormalBinding.NativeImage),
                        static_cast<VkFormat>(aoNormalBinding.Format)) &&
                    mAmbientOcclusionPass.Execute(mCurrentFrame, *presentationPerspective,
                        effectGuideTarget->WBuffering, presentationSettings.Effects, aoBarrierPlan,
                        Oot3d::PicaSurfaceCoordinates::FromTransferFlags(transfer.Flags));
            }
            mGpuProfiler.EndScope(
                mCurrentFrame, Oot3d::GpuProfileScope::AmbientOcclusion,
                command);
            aoExecutedNow = mAmbientOcclusionOutputValid;
            if (mAmbientOcclusionOutputValid) {
                recordEffectPassTransitions(
                    cacao  ? mCacaoPass.LastBarrierExecution()
                    : mxao ? mMxaoPass.LastBarrierExecution()
                           : mAmbientOcclusionPass.LastBarrierExecution());
                mDiagnostics.RecordAmbientOcclusionPass(
                    cacao  ? mCacaoPass.OutputOwnedByNri()
                    : mxao ? mMxaoPass.OutputOwnedByNri()
                           : mAmbientOcclusionPass.OutputOwnedByNri(),
                    !cacao || mCacaoPass.UsesPicaNormalGuide());
                mAmbientOcclusionRenderTargetNamespace =
                    transfer.RenderTargetNamespace;
                mAmbientOcclusionDisplayPhysicalAddress =
                    transfer.OutputPhysicalAddress;
            }
        }
        const bool aoForDisplay =
            mAmbientOcclusionOutputValid && wantsAmbientOcclusion &&
            mAmbientOcclusionRenderTargetNamespace == transfer.RenderTargetNamespace &&
            mAmbientOcclusionDisplayPhysicalAddress == transfer.OutputPhysicalAddress;
        if (aoForDisplay) {
            bindEffectImage(
                Oot3d::EffectResource::AmbientOcclusion,
                cacao  ? mCacaoPass.OutputImage()
                : mxao ? mMxaoPass.OutputImage()
                       : mAmbientOcclusionPass.OutputImage(),
                cacao  ? mCacaoPass.OutputView()
                : mxao ? mMxaoPass.OutputView()
                       : mAmbientOcclusionPass.OutputView(),
                VK_FORMAT_R32_SFLOAT, effectGuideTarget->Width,
                effectGuideTarget->Height);
        }
        // Diffuse GI: traced where the field, the motion vectors and a
        // perspective depth exist; otherwise the declared output is zero
        // irradiance, so the composite adds nothing and the graph stays whole.
        bool giForDisplay = false;
        if (wantsGlobalIllumination && effectGuideTarget != nullptr &&
            transientDiffuseIllumination != nullptr &&
            ((!effectPlan.ReflectionsFromGlobalIllumination &&
          !effectPlan.ReflectionMissesFromGlobalIllumination) ||
         transientSpecularIllumination != nullptr)) {
            const bool giAttempted = motionAvailableForDisplay && temporalState.has_value() &&
                                     !effectGuideTarget->WBuffering;
            const auto giCoordinates = Oot3d::PicaSurfaceCoordinates::FromTransferFlags(transfer.Flags);
            if (giAttempted) {
                const auto giReads = resolveEffectPassReads(Oot3d::DisplayEffectPass::GlobalIllumination);
                const auto image = [&](Oot3d::EffectResource resource) {
                    const auto& binding = requireEffectImageRead(giReads, resource);
                    return std::pair{reinterpret_cast<VkImage>(binding.NativeImage),
                                     static_cast<VkFormat>(binding.Format)};
                };
                Oot3d::BrixelizerGiInputs giInputs{};
                giInputs.Width = effectGuideTarget->Width;
                giInputs.Height = effectGuideTarget->Height;
                std::tie(giInputs.SurfaceGuide, giInputs.SurfaceGuideFormat) =
                    image(Oot3d::EffectResource::DiffuseAlbedoGuide);
                std::tie(giInputs.Depth, giInputs.DepthFormat) = image(Oot3d::EffectResource::NativeDepth);
                std::tie(giInputs.NormalGuide, giInputs.NormalGuideFormat) = image(Oot3d::EffectResource::NormalGuide);
                std::tie(giInputs.MaterialGuide, giInputs.MaterialGuideFormat) =
                    image(Oot3d::EffectResource::MaterialGuide);
                std::tie(giInputs.Motion, giInputs.MotionFormat) = image(Oot3d::EffectResource::MotionVectors);
                transitionEffectPassGuides(Oot3d::DisplayEffectPass::GlobalIllumination);
                // Sky light: rays leaving the scene receive the native sky
                // colour (linear) at its source weight.
                std::array<float, 3> giSky{};
                const auto sky = effectGuideTarget->ReflectionEnvironment.Resolve(mFrameCounter).Sky;
                for (size_t channel = 0; channel < giSky.size(); ++channel)
                    giSky[channel] = giSources.Weights[3] * std::pow(std::clamp(sky[channel], 0.0F, 1.0F), 2.2F);
                mGpuProfiler.BeginScope(mCurrentFrame, Oot3d::GpuProfileScope::GlobalIllumination, command);
                giForDisplay =
                    mBrixelizerGiPass.Configure(mFrameCounter, giInputs,
                                                presentationSettings.Effects.GlobalIllumination.Resolution,
                                                transientSpecularIllumination != nullptr, giSources.Weights) &&
                    mBrixelizerGiPass.Execute(command, mCurrentFrame, mFrameCounter, *temporalState,
                                              *presentationPerspective, giCoordinates,
                                              *transientDiffuseIllumination, transientSpecularIllumination,
                                              giSky, globalIlluminationBarrierPlan);
                mGpuProfiler.EndScope(mCurrentFrame, Oot3d::GpuProfileScope::GlobalIllumination, command);
            }
            if (!giForDisplay) {
                static uint64_t giFallbacks = 0;
                if (giFallbacks++ % 300U == 0U)
                    std::fprintf(stderr, "TRIAEVUM_GLOBAL_ILLUMINATION fallback=%llu reason=\"%s\"\n",
                                 (unsigned long long)giFallbacks,
                                 !giAttempted ? (effectGuideTarget->WBuffering ? "W-buffer depth"
                                                                               : "motion vectors unavailable")
                                              : mBrixelizerGiPass.UnavailableReason().c_str());
                if (!mBrixelizerGiPass.ClearOutput(mCurrentFrame, *transientDiffuseIllumination,
                                                   transientSpecularIllumination, globalIlluminationBarrierPlan))
                    throw std::runtime_error("global illumination output could not be cleared");
            }
            if (const auto& gi = mBrixelizerGiPass.Stats();
                gi.InstancesCreated != mReportedGiInstances || gi.WorldResets != mReportedGiResets ||
                gi.CreationFailures != mReportedGiCreationFailures) {
                mReportedGiInstances = gi.InstancesCreated;
                mReportedGiResets = gi.WorldResets;
                mReportedGiCreationFailures = gi.CreationFailures;
                std::fprintf(stderr,
                             "TRIAEVUM_GLOBAL_ILLUMINATION frame=%llu instances_created=%llu retired=%llu "
                             "world_resets=%llu source_resets=%llu creation_failures=%llu\n",
                             (unsigned long long)mFrameCounter, (unsigned long long)gi.InstancesCreated,
                             (unsigned long long)gi.InstancesRetired, (unsigned long long)gi.WorldResets,
                             (unsigned long long)gi.SourceResets, (unsigned long long)gi.CreationFailures);
            }
            recordEffectPassTransitions(mBrixelizerGiPass.LastBarrierExecution());
            recordDeclared(Oot3d::DisplayEffectPass::GlobalIllumination,
                           giForDisplay ? EffectOutcome::Executed : EffectOutcome::Skipped);
            bindEffectImage(Oot3d::EffectResource::DiffuseIllumination,
                            transientDiffuseIllumination->Texture.Image,
                            transientDiffuseIllumination->Texture.SampledView,
                            VK_FORMAT_R16G16B16A16_SFLOAT, effectGuideTarget->Width,
                            effectGuideTarget->Height);
            if (transientSpecularIllumination != nullptr)
                bindEffectImage(Oot3d::EffectResource::SpecularIllumination,
                                transientSpecularIllumination->Texture.Image,
                                transientSpecularIllumination->Texture.SampledView,
                                VK_FORMAT_R16G16B16A16_SFLOAT, effectGuideTarget->Width,
                                effectGuideTarget->Height);
        }
        const bool giComposed = wantsGlobalIllumination && effectGuideTarget != nullptr &&
                                transientDiffuseIllumination != nullptr;
        bool linearDepthForFsr = wantsFsr && effectGuideTarget != nullptr;
        const bool giReflections = effectPlan.ReflectionsFromGlobalIllumination;
        const bool hiZRequested =
            (reflectionsForDisplay && !giReflections) || linearDepthForFsr;
        const bool canReuseHiZ =
            hiZRequested && mHiZExecutedThisFrame && mHiZOutputValid &&
            mHiZRenderTargetNamespace == transfer.RenderTargetNamespace &&
            mHiZDisplayPhysicalAddress == transfer.OutputPhysicalAddress;
        const bool hiZAttempted = hiZRequested && !canReuseHiZ;
        bool hiZExecutedNow = false;
        if (hiZAttempted) {
            const auto hiZReads = resolveEffectPassReads(
                Oot3d::DisplayEffectPass::HiZ);
            const auto& hiZDepthBinding = requireEffectImageRead(
                hiZReads,
                Oot3d::EffectResource::NativeDepth);
            const VkImage hiZDepthImage =
                reinterpret_cast<VkImage>(hiZDepthBinding.NativeImage);
            const VkFormat hiZDepthFormat =
                static_cast<VkFormat>(hiZDepthBinding.Format);
            transitionEffectPassGuides(
                Oot3d::DisplayEffectPass::HiZ);
            mGpuProfiler.BeginScope(
                mCurrentFrame,
                Oot3d::GpuProfileScope::DepthPreparation,
                command);
            mHiZOutputValid = transientHiZOutput != nullptr &&
                mHiZDepthPyramidPass.Configure(
                    *transientHiZOutput,
                    hiZDepthImage, hiZDepthFormat) &&
                mHiZDepthPyramidPass.Execute(
                    command, mCurrentFrame,
                    presentationPerspective->NearPlane,
                    presentationPerspective->FarPlane,
                    effectGuideTarget->WBuffering
                        ? Oot3d::DepthConvention::WBuffer
                        : Oot3d::DepthConvention::Perspective,
                    hiZBarrierPlan);
            mGpuProfiler.EndScope(
                mCurrentFrame,
                Oot3d::GpuProfileScope::DepthPreparation,
                command);
            if (mHiZOutputValid) {
                hiZExecutedNow = true;
                mHiZRenderTargetNamespace = transfer.RenderTargetNamespace;
                mHiZDisplayPhysicalAddress = transfer.OutputPhysicalAddress;
                mHiZExecutedThisFrame = true;
                mDiagnostics.RecordHiZPass(
                    mHiZDepthPyramidPass.MipCount(),
                    effectGuideTarget->WBuffering
                        ? static_cast<uint32_t>(Oot3d::DepthConvention::WBuffer)
                        : static_cast<uint32_t>(
                              Oot3d::DepthConvention::Perspective),
                    mHiZDepthPyramidPass.OutputOwnedByNri(),
                    mHiZDepthPyramidPass.ComputeOwnedByNri(),
                    mHiZDepthPyramidPass.BarriersOwnedByNri());
                recordEffectPassTransitions(
                    mHiZDepthPyramidPass.LastBarrierExecution());
            }
        }
        const bool hiZForDisplay =
            mHiZOutputValid && effectGuideTarget != nullptr &&
            mHiZRenderTargetNamespace == transfer.RenderTargetNamespace &&
            mHiZDisplayPhysicalAddress == transfer.OutputPhysicalAddress;
        reflectionsForDisplay = reflectionsForDisplay && (giReflections || hiZForDisplay);
        linearDepthForFsr = linearDepthForFsr && hiZForDisplay;
        if (hiZForDisplay) {
            bindEffectImage(
                Oot3d::EffectResource::HierarchicalDepth,
                mHiZDepthPyramidPass.OutputImage(),
                mHiZDepthPyramidPass.OutputView(),
                VK_FORMAT_R32_SFLOAT, effectGuideTarget->Width,
                effectGuideTarget->Height);
        }
        std::optional<Oot3d::SceneSurfaceKey> linearKey;
        if (effectGuideTarget != nullptr) {
            linearKey = Oot3d::SceneSurfaceKey{
                transfer.RenderTargetNamespace,
                effectGuideTarget->Key.ColorPhysicalAddress,
                copyBoundary
                    ? Oot3d::SceneSurfaceKind::BoundaryWorkingColor
                    : Oot3d::SceneSurfaceKind::LinearWorkingColor};
        }
        const bool canReuseWorkingColor =
            wantsWorkingColor && mLinearColorExecutedThisFrame &&
            linearKey.has_value() && mLinearColorSurface.has_value() &&
            *mLinearColorSurface == *linearKey &&
            mLinearSceneColorPass.OutputImage() != VK_NULL_HANDLE &&
            mLinearSceneColorPass.OutputView() != VK_NULL_HANDLE;
        const bool workingColorAttempted =
            wantsWorkingColor && effectGuideTarget != nullptr &&
            !canReuseWorkingColor;
        bool linearWorkingColorForDisplay = canReuseWorkingColor;
        if (workingColorAttempted) {
            const auto workingColorReads = resolveEffectPassReads(
                Oot3d::DisplayEffectPass::WorkingColor);
            const auto* workingColorSourceRead = findEffectColorRead(
                workingColorReads, Oot3d::DisplayEffectPass::WorkingColor);
            if (workingColorSourceRead == nullptr ||
                workingColorSourceRead->Kind !=
                    Oot3d::EffectResourceBindingKind::Image) {
                throw std::runtime_error(
                    "working colour pass has no declared color input");
            }
            const auto& workingColorSourceBinding = *workingColorSourceRead;
            const VkImage workingColorSourceImage =
                reinterpret_cast<VkImage>(
                    workingColorSourceBinding.NativeImage);
            const VkFormat workingColorSourceFormat =
                static_cast<VkFormat>(workingColorSourceBinding.Format);
            const uint32_t workingColorSourceWidth =
                workingColorSourceBinding.Width;
            const uint32_t workingColorSourceHeight =
                workingColorSourceBinding.Height;
            const auto linearColorPolicy =
                Oot3d::BuildLinearSceneColorPolicy(
                    workingColorSourceBinding.ColorEncoding);
            linearWorkingColorForDisplay =
                linearColorPolicy.Valid &&
                transientLinearWorkingColor != nullptr &&
                mLinearSceneColorPass.Configure(
                    *transientLinearWorkingColor) &&
                mLinearSceneColorPass.Execute(
                    command, mCurrentFrame,
                    workingColorSourceImage,
                    workingColorSourceFormat,
                    workingColorSourceWidth,
                    workingColorSourceHeight,
                    linearColorPolicy.Source,
                    workingColorBarrierPlan);
            if (linearWorkingColorForDisplay) {
                recordEffectPassTransitions(
                    mLinearSceneColorPass.LastBarrierExecution());
                if (mLinearColorSurface.has_value() &&
                    *mLinearColorSurface != *linearKey) {
                    mSceneSurfaces.Retire(*mLinearColorSurface);
                    mLinearColorSurface.reset();
                }
                mSceneSurfaces.Publish({
                    *linearKey,
                    reinterpret_cast<uintptr_t>(
                        mLinearSceneColorPass.OutputImage()),
                    mLinearSceneColorPass.Width(),
                    mLinearSceneColorPass.Height(),
                    static_cast<uint32_t>(
                        VK_FORMAT_R16G16B16A16_SFLOAT),
                    0, true, Oot3d::SceneColorEncoding::Linear});
                mLinearColorSurface = *linearKey;
                mLinearColorExecutedThisFrame = true;
                mDiagnostics.RecordLinearWorkingColor(
                    linearColorPolicy.DecodeSrgb,
                    mLinearSceneColorPass.OutputOwnedByNri(),
                    mLinearSceneColorPass.ComputeOwnedByNri(),
                    mLinearSceneColorPass.BarriersOwnedByNri());
            }
        }
        if (linearWorkingColorForDisplay) {
            bindEffectImage(
                Oot3d::EffectResource::LinearWorkingColor,
                mLinearSceneColorPass.OutputImage(),
                mLinearSceneColorPass.OutputView(),
                VK_FORMAT_R16G16B16A16_SFLOAT,
                mLinearSceneColorPass.Width(),
                mLinearSceneColorPass.Height(),
                Oot3d::SceneColorEncoding::Linear);
        }
        VkImage activeReflectionImage = VK_NULL_HANDLE;
        VkImageView activeReflectionView = VK_NULL_HANDLE;
        Oot3d::SceneColorEncoding reflectionOutputEncoding =
            Oot3d::SceneColorEncoding::Unknown;
        Oot3d::ReflectionProvider activeReflectionProvider =
            Oot3d::ReflectionProvider::Off;
        std::optional<Oot3d::SceneSurfaceKey> reflectionKey;
        if (effectGuideTarget != nullptr) {
            reflectionKey = Oot3d::SceneSurfaceKey{
                transfer.RenderTargetNamespace,
                transfer.OutputPhysicalAddress,
                Oot3d::SceneSurfaceKind::Reflection};
        }
        const bool reflectionRequestedForDisplay =
            reflectionsForDisplay;
        const bool canReuseReflections =
            reflectionRequestedForDisplay &&
            mReflectionExecutedThisFrame && reflectionKey.has_value() &&
            mHiZReflectionSurface.has_value() &&
            *mHiZReflectionSurface == *reflectionKey;
        const bool reflectionsAttempted =
            reflectionRequestedForDisplay && !canReuseReflections;
        if (canReuseReflections) {
            activeReflectionProvider = mReflectionProviderThisFrame;
            reflectionOutputEncoding =
                mReflectionOutputEncodingThisFrame;
            if (activeReflectionProvider ==
                    Oot3d::ReflectionProvider::FidelityFxSssr ||
                activeReflectionProvider ==
                    Oot3d::ReflectionProvider::GlobalIllumination) {
                activeReflectionImage =
                    mReflectionMaterialResolvePass.OutputImage();
                activeReflectionView =
                    mReflectionMaterialResolvePass.OutputView();
            } else if (activeReflectionProvider ==
                           Oot3d::ReflectionProvider::HiZ) {
                activeReflectionImage =
                    mHiZReflectionPass.OutputImage();
                activeReflectionView =
                    mHiZReflectionPass.OutputView();
            }
            reflectionsForDisplay =
                activeReflectionProvider !=
                    Oot3d::ReflectionProvider::Off &&
                activeReflectionImage != VK_NULL_HANDLE &&
                activeReflectionView != VK_NULL_HANDLE;
        } else if (reflectionsAttempted && giReflections) {
            // GI specular is the reflection radiance: the material resolve
            // weights it by reflectivity, gain and the split-sum BRDF exactly
            // as it weights a screen-space trace.
            if (mHiZReflectionSurface.has_value() &&
                *mHiZReflectionSurface != *reflectionKey) {
                mSceneSurfaces.Retire(*mHiZReflectionSurface);
                mHiZReflectionSurface.reset();
            }
            const auto reflectionReads = resolveEffectPassReads(
                Oot3d::DisplayEffectPass::Reflections);
            const auto& normalBinding = requireEffectImageRead(
                reflectionReads, Oot3d::EffectResource::NormalGuide);
            const auto& materialBinding = requireEffectImageRead(
                reflectionReads, Oot3d::EffectResource::MaterialGuide);
            const auto& specularBinding = requireEffectImageRead(
                reflectionReads, Oot3d::EffectResource::SpecularIllumination);
            transitionEffectPassGuides(Oot3d::DisplayEffectPass::Reflections);
            mGpuProfiler.BeginScope(mCurrentFrame, Oot3d::GpuProfileScope::Reflection, command);
            const bool iblExecuted =
                giForDisplay && mReflectionIblPass.Available() &&
                mReflectionIblPass.Execute(
                    mCurrentFrame,
                    effectGuideTarget->ReflectionEnvironment.Resolve(mFrameCounter));
            if (iblExecuted)
                recordEffectPassTransitions(mReflectionIblPass.LastBarrierExecution());
            const bool resolved =
                iblExecuted && transientReflectionOutput != nullptr &&
                mReflectionMaterialResolvePass.Configure(*transientReflectionOutput) &&
                mReflectionMaterialResolvePass.Execute(
                    mCurrentFrame,
                    reinterpret_cast<VkImage>(specularBinding.NativeImage),
                    static_cast<VkFormat>(specularBinding.Format),
                    reinterpret_cast<VkImage>(normalBinding.NativeImage),
                    static_cast<VkFormat>(normalBinding.Format),
                    reinterpret_cast<VkImage>(materialBinding.NativeImage),
                    static_cast<VkFormat>(materialBinding.Format),
                    mReflectionIblPass.BrdfImage(), VK_FORMAT_R16G16_SFLOAT,
                    *presentationPerspective, 0.0F, reflectionBarrierPlan,
                    Oot3d::PicaSurfaceCoordinates::FromTransferFlags(transfer.Flags));
            if (resolved) {
                recordEffectPassTransitions(mReflectionMaterialResolvePass.LastBarrierExecution());
                activeReflectionProvider = Oot3d::ReflectionProvider::GlobalIllumination;
                activeReflectionImage = mReflectionMaterialResolvePass.OutputImage();
                activeReflectionView = mReflectionMaterialResolvePass.OutputView();
                reflectionOutputEncoding = Oot3d::SceneColorEncoding::Linear;
            } else if (!giForDisplay && transientReflectionOutput != nullptr &&
                       mReflectionMaterialResolvePass.Configure(*transientReflectionOutput) &&
                       mReflectionMaterialResolvePass.ClearOutput(mCurrentFrame, reflectionBarrierPlan)) {
                // Without GI for this sample nothing is composed: resolving the
                // cleared specular image would darken every reflective surface.
                // The declared reflection still exists, as no reflection (zero
                // weight) like the GI pass's zero irradiance, so its readers
                // resolve and the graph stays complete.
                recordEffectPassTransitions(mReflectionMaterialResolvePass.LastBarrierExecution());
                activeReflectionImage = mReflectionMaterialResolvePass.OutputImage();
                activeReflectionView = mReflectionMaterialResolvePass.OutputView();
                reflectionOutputEncoding = Oot3d::SceneColorEncoding::Linear;
                reflectionsSkipped = true;
            }
            reflectionsForDisplay = resolved;
            mGpuProfiler.EndScope(mCurrentFrame, Oot3d::GpuProfileScope::Reflection, command);
            if (reflectionsForDisplay) {
                mSceneSurfaces.Publish({*reflectionKey,
                    reinterpret_cast<uintptr_t>(activeReflectionImage),
                    effectGuideTarget->Width, effectGuideTarget->Height,
                    static_cast<uint32_t>(VK_FORMAT_R16G16B16A16_SFLOAT),
                    0, true, reflectionOutputEncoding});
                reflectionNriWrapped = mNriInterop.WrapTexture(
                    activeReflectionImage, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TYPE_2D,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    effectGuideTarget->Width, effectGuideTarget->Height);
                mHiZReflectionSurface = *reflectionKey;
                mReflectionExecutedThisFrame = true;
                mReflectionProviderThisFrame = activeReflectionProvider;
                mReflectionOutputEncodingThisFrame = reflectionOutputEncoding;
            }
        } else if (reflectionsAttempted) {
            if (mHiZReflectionSurface.has_value() &&
                *mHiZReflectionSurface != *reflectionKey) {
                mSceneSurfaces.Retire(*mHiZReflectionSurface);
                mHiZReflectionSurface.reset();
            }
            const auto reflectionReads = resolveEffectPassReads(
                Oot3d::DisplayEffectPass::Reflections);
            const auto* reflectionSceneBinding = findEffectColorRead(
                reflectionReads,
                Oot3d::DisplayEffectPass::Reflections);
            if (reflectionSceneBinding == nullptr ||
                reflectionSceneBinding->Kind !=
                    Oot3d::EffectResourceBindingKind::Image) {
                throw std::runtime_error(
                    "reflection pass has no declared color input");
            }
            const auto& reflectionNormalBinding = requireEffectImageRead(
                reflectionReads,
                Oot3d::EffectResource::NormalGuide);
            const auto& reflectionMaterialBinding = requireEffectImageRead(
                reflectionReads,
                Oot3d::EffectResource::MaterialGuide);
            const auto& reflectionHiZBinding = requireEffectImageRead(
                reflectionReads,
                Oot3d::EffectResource::HierarchicalDepth);
            const bool sssrContract =
                effectPlan.ReflectionProvider ==
                    Oot3d::ReflectionMode::FidelityFxSssr;
            const auto* reflectionDepthBinding = sssrContract
                ? &requireEffectImageRead(
                      reflectionReads,
                      Oot3d::EffectResource::NativeDepth)
                : nullptr;
            const auto* reflectionMotionBinding = sssrContract
                ? &requireEffectImageRead(
                      reflectionReads,
                      Oot3d::EffectResource::MotionVectors)
                : nullptr;
            const VkImage reflectionSceneImage =
                reinterpret_cast<VkImage>(
                    reflectionSceneBinding->NativeImage);
            const VkFormat reflectionSceneFormat =
                static_cast<VkFormat>(reflectionSceneBinding->Format);
            const VkImage reflectionDepthImage =
                reflectionDepthBinding == nullptr
                    ? VK_NULL_HANDLE
                    : reinterpret_cast<VkImage>(
                          reflectionDepthBinding->NativeImage);
            const VkFormat reflectionDepthFormat =
                reflectionDepthBinding == nullptr
                    ? VK_FORMAT_UNDEFINED
                    : static_cast<VkFormat>(reflectionDepthBinding->Format);
            const VkImage reflectionNormalImage =
                reinterpret_cast<VkImage>(
                    reflectionNormalBinding.NativeImage);
            const VkFormat reflectionNormalFormat =
                static_cast<VkFormat>(reflectionNormalBinding.Format);
            const VkImage reflectionMaterialImage =
                reinterpret_cast<VkImage>(
                    reflectionMaterialBinding.NativeImage);
            const VkFormat reflectionMaterialFormat =
                static_cast<VkFormat>(reflectionMaterialBinding.Format);
            const VkImage reflectionHiZImage =
                reinterpret_cast<VkImage>(
                    reflectionHiZBinding.NativeImage);
            const VkFormat reflectionHiZFormat =
                static_cast<VkFormat>(reflectionHiZBinding.Format);
            const VkImage reflectionMotionImage =
                reflectionMotionBinding == nullptr
                    ? VK_NULL_HANDLE
                    : reinterpret_cast<VkImage>(
                          reflectionMotionBinding->NativeImage);
            const VkFormat reflectionMotionFormat =
                reflectionMotionBinding == nullptr
                    ? VK_FORMAT_UNDEFINED
                    : static_cast<VkFormat>(reflectionMotionBinding->Format);
            transitionEffectPassGuides(
                Oot3d::DisplayEffectPass::Reflections);
            mGpuProfiler.BeginScope(
                mCurrentFrame,
                Oot3d::GpuProfileScope::Reflection,
                command);
            activeReflectionProvider = Oot3d::SelectReflectionProvider(
                presentationSettings.Effects.Reflections,
                {
                    mHiZReflectionPass.Available() && mHiZOutputValid,
                    mFidelityFxSssrPass.Available() &&
                        mReflectionIblPass.Available() &&
                        mReflectionMaterialResolvePass.Available(),
                    motionAvailableForDisplay,
                    temporalState.has_value(),
                    !effectGuideTarget->WBuffering,
                    mLinearSceneColorPass.Available(),
                });
            if (presentationSettings.Effects.Reflections ==
                    Oot3d::ReflectionMode::FidelityFxSssr &&
                activeReflectionProvider !=
                    Oot3d::ReflectionProvider::FidelityFxSssr) {
                const std::string fallbackReason =
                    !mFidelityFxSssrPass.Available()
                        ? mFidelityFxSssrPass.UnavailableReason()
                    : !mReflectionIblPass.Available()
                        ? mReflectionIblPass.UnavailableReason()
                    : !mReflectionMaterialResolvePass.Available()
                        ? mReflectionMaterialResolvePass.UnavailableReason()
                    : effectGuideTarget->WBuffering
                        ? "FidelityFX SSSR does not support PICA W-buffer depth"
                    : !motionAvailableForDisplay
                        ? "FidelityFX SSSR motion vectors are unavailable"
                    : !mLinearSceneColorPass.Available()
                        ? mLinearSceneColorPass.UnavailableReason()
                        : "FidelityFX SSSR temporal metadata is unavailable";
                mDiagnostics.RecordFidelityFxSssrFallback(
                    fallbackReason);
            }
            if (activeReflectionProvider ==
                Oot3d::ReflectionProvider::FidelityFxSssr) {
                const auto reflectionEnvironment =
                    effectGuideTarget->ReflectionEnvironment.Resolve(
                        mFrameCounter);
                const bool reflectionIblExecuted =
                    mReflectionIblPass.Execute(
                        mCurrentFrame, reflectionEnvironment);
                const bool fidelityFxIblConfigured =
                    reflectionIblExecuted &&
                    mFidelityFxSssrPass.SetIblResources(
                        mReflectionIblPass.EnvironmentImage(),
                        Oot3d::ReflectionIblPass::EnvironmentSize,
                        Oot3d::ReflectionIblPass::EnvironmentMipCount,
                        mReflectionIblPass.BrdfImage(),
                        Oot3d::ReflectionIblPass::BrdfSize);
                if (reflectionIblExecuted) {
                    recordEffectPassTransitions(
                        mReflectionIblPass.LastBarrierExecution());
                    mDiagnostics.RecordReflectionIbl(
                        mReflectionIblPass.ProfileUpdatedLastExecute(),
                        mReflectionIblPass.PicaDerivedLastExecute(),
                        Oot3d::ReflectionIblPass::EnvironmentMipCount,
                        mReflectionIblPass.EnvironmentOwnedByNri(),
                        mReflectionIblPass.BrdfOwnedByNri(),
                        mReflectionIblPass.ComputeOwnedByNri(),
                        mReflectionIblPass.BarriersOwnedByNri());
                }
                const bool reflectionIblReady =
                    reflectionIblExecuted && fidelityFxIblConfigured;
                const bool rawFidelityFxExecuted =
                    reflectionIblReady &&
                    linearWorkingColorForDisplay &&
                    mFidelityFxSssrPass.Configure(
                        mFrameCounter,
                        effectGuideTarget->Width,
                        effectGuideTarget->Height,
                        reflectionSceneImage,
                        reflectionSceneFormat,
                        reflectionDepthImage,
                        reflectionDepthFormat,
                        reflectionMotionImage,
                        reflectionMotionFormat,
                        reflectionNormalImage,
                        reflectionNormalFormat,
                        reflectionMaterialImage,
                        reflectionMaterialFormat) &&
                    mFidelityFxSssrPass.Execute(
                        command, *temporalState,
                        *presentationPerspective,
                        presentationSettings.Effects, mCurrentFrame,
                        Oot3d::PicaSurfaceCoordinates::FromTransferFlags(transfer.Flags));
                const bool fidelityFxExecuted =
                    rawFidelityFxExecuted &&
                    transientReflectionOutput != nullptr &&
                    mReflectionMaterialResolvePass.Configure(
                        *transientReflectionOutput) &&
                    mReflectionMaterialResolvePass.Execute(
                        mCurrentFrame,
                        mFidelityFxSssrPass.OutputImage(),
                        VK_FORMAT_R16G16B16A16_SFLOAT,
                        reflectionNormalImage,
                        reflectionNormalFormat,
                        reflectionMaterialImage,
                        reflectionMaterialFormat,
                        mReflectionIblPass.BrdfImage(),
                        VK_FORMAT_R16G16_SFLOAT,
                        *presentationPerspective,
                        presentationSettings.Effects
                            .ReflectionRoughnessBias,
                        reflectionBarrierPlan,
                        Oot3d::PicaSurfaceCoordinates::FromTransferFlags(transfer.Flags));
                if (rawFidelityFxExecuted) {
                    recordEffectPassTransitions(
                        mFidelityFxSssrPass.LastBarrierExecution());
                }
                if (fidelityFxExecuted) {
                    recordEffectPassTransitions(
                        mReflectionMaterialResolvePass
                            .LastBarrierExecution());
                    activeReflectionImage =
                        mReflectionMaterialResolvePass.OutputImage();
                    activeReflectionView =
                        mReflectionMaterialResolvePass.OutputView();
                    mDiagnostics.RecordReflectionMaterialResolve(
                        mReflectionMaterialResolvePass.OutputOwnedByNri(),
                        mReflectionMaterialResolvePass.ComputeOwnedByNri(),
                        mReflectionMaterialResolvePass.BarriersOwnedByNri());
                    reflectionOutputEncoding =
                        Oot3d::SceneColorEncoding::Linear;
                } else {
                    mDiagnostics.RecordFidelityFxSssrFallback(
                        !reflectionIblExecuted
                            ? mReflectionIblPass.UnavailableReason()
                        : !fidelityFxIblConfigured
                            ? mFidelityFxSssrPass.UnavailableReason()
                        : !linearWorkingColorForDisplay
                            ? mLinearSceneColorPass.UnavailableReason()
                        : !rawFidelityFxExecuted
                            ? mFidelityFxSssrPass.UnavailableReason()
                            : mReflectionMaterialResolvePass
                                  .UnavailableReason());
                    activeReflectionProvider =
                        Oot3d::ReflectionProvider::HiZ;
                    reflectionOutputEncoding =
                        Oot3d::SceneColorEncoding::Unknown;
                }
            }
            if (activeReflectionProvider ==
                Oot3d::ReflectionProvider::HiZ) {
                const bool hiZExecuted =
                    transientReflectionOutput != nullptr &&
                    mHiZReflectionPass.Configure(
                        *transientReflectionOutput,
                        reflectionSceneImage,
                        reflectionSceneFormat,
                        reflectionHiZImage,
                        reflectionHiZFormat,
                        reflectionNormalImage,
                        reflectionNormalFormat,
                        reflectionMaterialImage,
                        reflectionMaterialFormat) &&
                    mHiZReflectionPass.Execute(
                        command, mCurrentFrame,
                        mHiZDepthPyramidPass.MipCount(),
                        *presentationPerspective,
                        presentationSettings.Effects,
                        reflectionBarrierPlan,
                        Oot3d::PicaSurfaceCoordinates::FromTransferFlags(transfer.Flags));
                if (hiZExecuted) {
                    recordEffectPassTransitions(
                        mHiZReflectionPass.LastBarrierExecution());
                    activeReflectionImage =
                        mHiZReflectionPass.OutputImage();
                    activeReflectionView =
                        mHiZReflectionPass.OutputView();
                    reflectionOutputEncoding =
                        reflectionSceneBinding == nullptr
                            ? Oot3d::NativePicaSceneColorEncoding()
                            : reflectionSceneBinding->ColorEncoding;
                    // GI specular fills what the screen-space trace missed,
                    // in place, weighted by the same material resolve.
                    if (effectPlan.ReflectionMissesFromGlobalIllumination && giForDisplay &&
                        mReflectionIblPass.Available() &&
                        mReflectionMaterialResolvePass.Available()) {
                        const auto& specularBinding = requireEffectImageRead(
                            reflectionReads, Oot3d::EffectResource::SpecularIllumination);
                        const bool iblExecuted = mReflectionIblPass.Execute(
                            mCurrentFrame,
                            effectGuideTarget->ReflectionEnvironment.Resolve(mFrameCounter));
                        if (iblExecuted)
                            recordEffectPassTransitions(mReflectionIblPass.LastBarrierExecution());
                        const float screenSpaceStrength =
                            std::max(presentationSettings.Effects.ReflectionStrength, 0.01F);
                        if (iblExecuted &&
                            mReflectionMaterialResolvePass.Configure(*transientReflectionOutput) &&
                            mReflectionMaterialResolvePass.Execute(
                                mCurrentFrame,
                                reinterpret_cast<VkImage>(specularBinding.NativeImage),
                                static_cast<VkFormat>(specularBinding.Format),
                                reflectionNormalImage, reflectionNormalFormat,
                                reflectionMaterialImage, reflectionMaterialFormat,
                                mReflectionIblPass.BrdfImage(), VK_FORMAT_R16G16_SFLOAT,
                                *presentationPerspective, 0.0F, reflectionBarrierPlan,
                                Oot3d::PicaSurfaceCoordinates::FromTransferFlags(transfer.Flags),
                                {true,
                                 presentationSettings.Effects.GlobalIllumination.SpecularIntensity *
                                     giSources.Gain / screenSpaceStrength,
                                 reflectionOutputEncoding == Oot3d::SceneColorEncoding::Srgb}))
                            recordEffectPassTransitions(
                                mReflectionMaterialResolvePass.LastBarrierExecution());
                    }
                } else {
                    activeReflectionProvider =
                        Oot3d::ReflectionProvider::Off;
                    reflectionOutputEncoding =
                        Oot3d::SceneColorEncoding::Unknown;
                }
            }
            reflectionsForDisplay =
                activeReflectionProvider !=
                    Oot3d::ReflectionProvider::Off &&
                activeReflectionImage != VK_NULL_HANDLE &&
                activeReflectionView != VK_NULL_HANDLE;
            mGpuProfiler.EndScope(
                mCurrentFrame,
                Oot3d::GpuProfileScope::Reflection,
                command);
            if (reflectionsForDisplay) {
                mSceneSurfaces.Publish({*reflectionKey,
                    reinterpret_cast<uintptr_t>(activeReflectionImage),
                    effectGuideTarget->Width, effectGuideTarget->Height,
                    static_cast<uint32_t>(VK_FORMAT_R16G16B16A16_SFLOAT),
                    0, true, reflectionOutputEncoding});
                reflectionNriWrapped = mNriInterop.WrapTexture(
                    activeReflectionImage,
                    VK_FORMAT_R16G16B16A16_SFLOAT,
                    VK_IMAGE_TYPE_2D,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    effectGuideTarget->Width, effectGuideTarget->Height);
                mHiZReflectionSurface = *reflectionKey;
                mReflectionExecutedThisFrame = true;
                mReflectionProviderThisFrame =
                    activeReflectionProvider;
                mReflectionOutputEncodingThisFrame =
                    reflectionOutputEncoding;
            }
        }
        if (reflectionsForDisplay || reflectionsSkipped) {
            bindEffectImage(
                Oot3d::EffectResource::ReflectionColor,
                activeReflectionImage, activeReflectionView,
                VK_FORMAT_R16G16B16A16_SFLOAT,
                effectGuideTarget->Width, effectGuideTarget->Height,
                reflectionOutputEncoding);
        }
        std::optional<Oot3d::SceneSurfaceKey> taaKey;
        if (effectGuideTarget != nullptr) {
            taaKey = Oot3d::SceneSurfaceKey{
                transfer.RenderTargetNamespace,
                effectGuideTarget->Key.ColorPhysicalAddress,
                Oot3d::SceneSurfaceKind::Output};
        }
        const bool canReuseTaa =
            wantsTaa && mTaaExecutedThisFrame && mTaaOutputValid &&
            taaKey.has_value() && mTaaSurface.has_value() &&
            *mTaaSurface == *taaKey;
        std::optional<Oot3d::SceneSurfaceKey> compositeKey;
        if (effectGuideTarget != nullptr) {
            compositeKey = Oot3d::SceneSurfaceKey{
                transfer.RenderTargetNamespace,
                effectGuideTarget->Key.ColorPhysicalAddress,
                // A boundary composes the source's own domain, so its result is
                // not the presentation's composite: reusing it for a later
                // display of this target would scan out the pre-consumer scene.
                copyBoundary
                    ? Oot3d::SceneSurfaceKind::BoundaryComposition
                    : Oot3d::SceneSurfaceKind::Composite};
        }
        const bool canReuseComposite =
            effectPlan.Enabled(Oot3d::DisplayEffectPass::Composite) &&
            mCompositeExecutedThisFrame && compositeKey.has_value() &&
            mCompositeSurface.has_value() &&
            *mCompositeSurface == *compositeKey;
        const bool canReuseUpscaler =
            (wantsTemporalUpscaler || wantsNis) &&
            mUpscalerExecutedThisFrame &&
            mUpscalerProviderThisFrame == effectPlan.Upscaler &&
            mUpscalerRenderTargetNamespace ==
                transfer.RenderTargetNamespace &&
            mUpscalerDisplayPhysicalAddress ==
                transfer.OutputPhysicalAddress;
        // A boundary always composes when any scene stage applies, because its
        // consumer samples the composed colour instead of a later presentation
        // fusing those stages in. Presentation keeps its existing gate.
        bool compositeForTaa =
            (copyBoundary ||
             ((((wantsTaa && !canReuseTaa) ||
                (wantsTemporalUpscaler && !canReuseUpscaler)) &&
               motionAvailableForDisplay) ||
              (wantsNis && !canReuseUpscaler) ||
              (wantsSmaa && !mSmaaExecutedThisFrame) || giComposed)) &&
            (aoForDisplay || reflectionsForDisplay ||
             outlineForDisplay || giComposed) &&
            mSceneCompositePass.Available();
        const bool compositeAttempted = compositeForTaa;
        Oot3d::SceneColorEncoding compositeInputEncoding =
            linearWorkingColorForDisplay
                ? Oot3d::SceneColorEncoding::Linear
                : Oot3d::NativePicaSceneColorEncoding();
        if (compositeForTaa) {
            const auto compositeReads = resolveEffectPassReads(
                Oot3d::DisplayEffectPass::Composite);
            const auto* compositeColorBinding = findEffectColorRead(
                compositeReads,
                Oot3d::DisplayEffectPass::Composite);
            if (compositeColorBinding == nullptr ||
                compositeColorBinding->Kind !=
                    Oot3d::EffectResourceBindingKind::Image) {
                throw std::runtime_error(
                    "composite pass has no declared color input");
            }
            const VkImage compositeColorImage =
                reinterpret_cast<VkImage>(
                    compositeColorBinding->NativeImage);
            const VkFormat compositeColorFormat =
                static_cast<VkFormat>(compositeColorBinding->Format);
            compositeInputEncoding =
                compositeColorBinding->ColorEncoding;
            const auto optionalCompositeImage = [
                &compositeReads, &requireEffectImageRead,
                compositeColorImage](Oot3d::EffectResource resource) {
                return compositeReads.Declares(resource)
                    ? reinterpret_cast<VkImage>(
                          requireEffectImageRead(
                              compositeReads, resource).NativeImage)
                    : compositeColorImage;
            };
            const auto optionalCompositeFormat = [
                &compositeReads, &requireEffectImageRead,
                compositeColorFormat](Oot3d::EffectResource resource) {
                return compositeReads.Declares(resource)
                    ? static_cast<VkFormat>(
                          requireEffectImageRead(
                              compositeReads, resource).Format)
                    : compositeColorFormat;
            };
            const VkImage compositeAoImage = optionalCompositeImage(
                Oot3d::EffectResource::AmbientOcclusion);
            const VkFormat compositeAoFormat = optionalCompositeFormat(
                Oot3d::EffectResource::AmbientOcclusion);
            const VkImage compositeReflectionImage = optionalCompositeImage(
                Oot3d::EffectResource::ReflectionColor);
            const VkFormat compositeReflectionFormat =
                optionalCompositeFormat(
                    Oot3d::EffectResource::ReflectionColor);
            const VkImage compositeMaterialImage = optionalCompositeImage(
                Oot3d::EffectResource::MaterialGuide);
            const VkFormat compositeMaterialFormat = optionalCompositeFormat(
                Oot3d::EffectResource::MaterialGuide);
            const VkImage compositeNormalImage = optionalCompositeImage(
                Oot3d::EffectResource::NormalGuide);
            const VkFormat compositeNormalFormat = optionalCompositeFormat(
                Oot3d::EffectResource::NormalGuide);
            const VkImage compositeTransparentDepthImage =
                optionalCompositeImage(
                    Oot3d::EffectResource::RigidMotionGuide);
            const VkFormat compositeTransparentDepthFormat =
                optionalCompositeFormat(
                    Oot3d::EffectResource::RigidMotionGuide);
            const VkImage compositeAmbientImage = optionalCompositeImage(
                Oot3d::EffectResource::AmbientGuide);
            const VkFormat compositeAmbientFormat = optionalCompositeFormat(
                Oot3d::EffectResource::AmbientGuide);
            const VkImage compositeDepthImage = optionalCompositeImage(
                Oot3d::EffectResource::NativeDepth);
            const VkFormat compositeDepthFormat = optionalCompositeFormat(
                Oot3d::EffectResource::NativeDepth);
            transitionEffectPassGuides(
                Oot3d::DisplayEffectPass::Composite);
            mGpuProfiler.BeginScope(
                mCurrentFrame,
                Oot3d::GpuProfileScope::SceneComposite,
                command);
            slowSteps.Mark("scene_effects");
            compositeForTaa =
                transientCompositeOutput != nullptr && mSceneCompositePass.Configure(*transientCompositeOutput) &&
                mSceneCompositePass.Execute(
                    command, mCurrentFrame, mFrameCounter, compositeColorImage, compositeColorFormat, compositeAoImage,
                    compositeAoFormat, compositeReflectionImage, compositeReflectionFormat, compositeMaterialImage,
                    compositeMaterialFormat, compositeNormalImage, compositeNormalFormat,
                    compositeTransparentDepthImage, compositeTransparentDepthFormat, compositeAmbientImage,
                    compositeAmbientFormat, compositeDepthImage, compositeDepthFormat,
                    optionalCompositeImage(Oot3d::EffectResource::FogGuide),
                    optionalCompositeFormat(Oot3d::EffectResource::FogGuide),
                    optionalCompositeImage(Oot3d::EffectResource::OutlineGeometryGuide),
                    optionalCompositeFormat(Oot3d::EffectResource::OutlineGeometryGuide),
                    optionalCompositeImage(Oot3d::EffectResource::DiffuseIllumination),
                    optionalCompositeFormat(Oot3d::EffectResource::DiffuseIllumination),
                    optionalCompositeImage(Oot3d::EffectResource::DiffuseAlbedoGuide),
                    optionalCompositeFormat(Oot3d::EffectResource::DiffuseAlbedoGuide), giForDisplay,
                    presentationSettings.Effects.GlobalIllumination.DiffuseIntensity * giSources.Gain, aoForDisplay,
                    presentationSettings.Effects.AmbientOcclusion == Oot3d::AmbientOcclusionMode::Mxao
                        ? static_cast<uint32_t>(presentationSettings.Effects.MxaoDebug) : 0U,
                    reflectionsForDisplay,
                    giReflections ? presentationSettings.Effects.GlobalIllumination.SpecularIntensity * giSources.Gain
                                  : presentationSettings.Effects.ReflectionStrength,
                    presentationSettings.Effects.ReflectionDebugView, outlineForDisplay,
                    presentationSettings.Effects.ToonStyle, compositeInputEncoding, compositeBarrierPlan);
            mGpuProfiler.EndScope(
                mCurrentFrame,
                Oot3d::GpuProfileScope::SceneComposite,
                command);
            if (compositeForTaa) {
                if (mCompositeSurface.has_value() &&
                    *mCompositeSurface != *compositeKey) {
                    mSceneSurfaces.Retire(*mCompositeSurface);
                    mCompositeSurface.reset();
                }
                const VkImage compositeImage =
                    mSceneCompositePass.OutputImage();
                mSceneSurfaces.Publish({*compositeKey,
                    reinterpret_cast<uintptr_t>(compositeImage),
                    effectGuideTarget->Width, effectGuideTarget->Height,
                    static_cast<uint32_t>(VK_FORMAT_R16G16B16A16_SFLOAT),
                    0, true, mSceneCompositePass.OutputEncoding()});
                mNriInterop.WrapTexture(
                    compositeImage, VK_FORMAT_R16G16B16A16_SFLOAT,
                    VK_IMAGE_TYPE_2D,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    effectGuideTarget->Width, effectGuideTarget->Height);
                mCompositeSurface = *compositeKey;
                mCompositeExecutedThisFrame = true;
                mCompositeOutputEncodingThisFrame =
                    mSceneCompositePass.OutputEncoding();
                mDiagnostics.RecordSceneCompositePass(
                    mSceneCompositePass.OutputOwnedByNri(),
                    mSceneCompositePass.ComputeOwnedByNri(),
                    mSceneCompositePass.BarriersOwnedByNri());
                recordEffectPassTransitions(
                    mSceneCompositePass.LastBarrierExecution());
                bindEffectImage(
                    Oot3d::EffectResource::CompositeColor,
                    mSceneCompositePass.OutputImage(),
                    mSceneCompositePass.OutputView(),
                    VK_FORMAT_R16G16B16A16_SFLOAT,
                    effectGuideTarget->Width,
                    effectGuideTarget->Height,
                    mSceneCompositePass.OutputEncoding());
            }
        }
        bool taaNriWrapped = false;
        bool taaForDisplay = canReuseTaa;
        bool taaAttempted = false;
        if (!taaForDisplay && wantsTaa && motionAvailableForDisplay &&
            temporalState.has_value() && !mTaaExecutedThisFrame) {
            taaAttempted = true;
            const auto taaReads = resolveEffectPassReads(
                Oot3d::DisplayEffectPass::Taa);
            const auto* colorBinding = findEffectColorRead(
                taaReads, Oot3d::DisplayEffectPass::Taa);
            if (colorBinding == nullptr ||
                colorBinding->Kind !=
                    Oot3d::EffectResourceBindingKind::Image) {
                throw std::runtime_error(
                    "TAA pass has no declared color input");
            }
            const auto& motionBinding = requireEffectImageRead(
                taaReads,
                Oot3d::EffectResource::MotionVectors);
            const VkImage taaInputImage =
                reinterpret_cast<VkImage>(colorBinding->NativeImage);
            const VkFormat taaInputFormat =
                static_cast<VkFormat>(colorBinding->Format);
            const VkImage taaMotionImage =
                reinterpret_cast<VkImage>(motionBinding.NativeImage);
            const VkFormat taaMotionFormat =
                static_cast<VkFormat>(motionBinding.Format);
            const Oot3d::SceneColorEncoding taaInputEncoding =
                colorBinding->ColorEncoding;
            if (mTaaSurface.has_value() && *mTaaSurface != *taaKey) {
                mSceneSurfaces.Retire(*mTaaSurface);
                mTaaSurface.reset();
            }
            transitionEffectPassGuides(
                Oot3d::DisplayEffectPass::Taa);
            mGpuProfiler.BeginScope(
                mCurrentFrame,
                Oot3d::GpuProfileScope::AntiAliasing,
                command);
            // TAA resolves its colour input, which a native display
            // transfer may have downscaled from the scene target.
            taaForDisplay = mTemporalAaPass.Configure(
                    colorBinding->Width, colorBinding->Height) &&
                mTemporalAaPass.Execute(
                    command, mCurrentFrame, mFrameCounter,
                    taaInputImage, taaInputFormat,
                    taaMotionImage, taaMotionFormat,
                    motionBinding.Width, motionBinding.Height,
                    temporalState->HistoryValid,
                    presentationSettings.TaaHistoryWeight,
                    presentationSettings.TaaClampExpansion,
                    presentationSettings.TaaSharpness,
                    taaInputEncoding,
                    taaBarrierPlan);
            mGpuProfiler.EndScope(
                mCurrentFrame,
                Oot3d::GpuProfileScope::AntiAliasing,
                command);
            if (taaForDisplay) {
                const VkImage taaImage = mTemporalAaPass.OutputImage();
                mSceneSurfaces.Publish({*taaKey,
                    reinterpret_cast<uintptr_t>(taaImage),
                    mTemporalAaPass.Width(), mTemporalAaPass.Height(),
                    static_cast<uint32_t>(VK_FORMAT_R16G16B16A16_SFLOAT),
                    0, true, mTemporalAaPass.OutputEncoding()});
                taaNriWrapped = mNriInterop.WrapTexture(
                    taaImage, VK_FORMAT_R16G16B16A16_SFLOAT,
                    VK_IMAGE_TYPE_2D,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    mTemporalAaPass.Width(), mTemporalAaPass.Height());
                mTaaSurface = *taaKey;
                mTaaExecutedThisFrame = true;
                mTaaOutputValid = true;
                mTaaOutputEncodingThisFrame =
                    mTemporalAaPass.OutputEncoding();
                mTaaEffectsCompositedThisFrame = compositeForTaa;
                recordEffectPassTransitions(
                    mTemporalAaPass.LastBarrierExecution());
                bindEffectImage(
                    Oot3d::EffectResource::TemporalColor,
                    mTemporalAaPass.OutputImage(),
                    mTemporalAaPass.OutputView(),
                    VK_FORMAT_R16G16B16A16_SFLOAT,
                    mTemporalAaPass.Width(),
                    mTemporalAaPass.Height(),
                    mTaaOutputEncodingThisFrame);
            }
            mDiagnostics.RecordTemporalAa(
                taaForDisplay,
                taaForDisplay && mTemporalAaPass.HistoryUsedLastExecute(),
                taaNriWrapped, mTemporalAaPass.HistoryOwnedByNri(),
                mTemporalAaPass.ComputeOwnedByNri(),
                mTemporalAaPass.BarriersOwnedByNri());
        }
        if (canReuseTaa) {
            bindEffectImage(
                Oot3d::EffectResource::TemporalColor,
                mTemporalAaPass.OutputImage(),
                mTemporalAaPass.OutputView(),
                VK_FORMAT_R16G16B16A16_SFLOAT,
                mTemporalAaPass.Width(), mTemporalAaPass.Height(),
                mTaaOutputEncodingThisFrame);
        }
        const auto retainUpscalerOutput = [this, &effectPlan, &transfer](
            Oot3d::SceneColorEncoding outputEncoding,
            bool effectsComposited,
            uint32_t width, uint32_t height) {
            mUpscalerExecutedThisFrame = true;
            mUpscalerProviderThisFrame = effectPlan.Upscaler;
            mUpscalerOutputEncodingThisFrame = outputEncoding;
            mUpscalerEffectsCompositedThisFrame = effectsComposited;
            mUpscalerRenderTargetNamespace = transfer.RenderTargetNamespace;
            mUpscalerDisplayPhysicalAddress =
                transfer.OutputPhysicalAddress;
            mUpscalerOutputWidth = width;
            mUpscalerOutputHeight = height;
        };
        // A temporal upscaler reconstructs at its colour input's extent. Its
        // guides keep the scene target's, which differs when the native display
        // transfer resized the colour: they are then resampled to the colour's
        // extent, and the jitter (applied in target pixels) is given in input
        // pixels.
        const auto prepareUpscalerGuides = [&](Oot3d::NriTemporalUpscaleDispatchDesc& desc) {
            const uint32_t guideWidth = effectGuideTarget->Width;
            const uint32_t guideHeight = effectGuideTarget->Height;
            desc.JitterPixels = Oot3d::UpscalerJitterInInputPixels(
                mTemporalJitterPixels, guideWidth, guideHeight, desc.InputWidth, desc.InputHeight);
            const bool resample = Oot3d::UpscalerGuidesNeedResample(
                desc.InputWidth, desc.InputHeight, guideWidth, guideHeight);
            mDiagnostics.RecordUpscalerGuides(resample, guideWidth, guideHeight);
            if (!resample)
                return true;
            if (!mUpscalerGuidePass.Execute(
                    mCurrentFrame,
                    {desc.DepthImage, desc.DepthFormat, desc.MotionImage, desc.MotionFormat,
                     desc.ReactiveImage, desc.ReactiveFormat, guideWidth, guideHeight},
                    desc.InputWidth, desc.InputHeight))
                return false;
            recordEffectPassTransitions(mUpscalerGuidePass.LastBarrierExecution());
            desc.DepthImage = mUpscalerGuidePass.DepthImage();
            desc.DepthFormat = VK_FORMAT_R32_SFLOAT;
            desc.MotionImage = mUpscalerGuidePass.MotionImage();
            desc.MotionFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
            desc.ReactiveImage = mUpscalerGuidePass.ReactiveImage();
            desc.ReactiveFormat = VK_FORMAT_R16_SFLOAT;
            return true;
        };
        bool fsrForDisplay = wantsFsr && canReuseUpscaler;
        bool fsrAttempted = false;
        uint32_t fsrOutputWidth = canReuseUpscaler
            ? mUpscalerOutputWidth : plannedUpscalerWidth;
        uint32_t fsrOutputHeight = canReuseUpscaler
            ? mUpscalerOutputHeight : plannedUpscalerHeight;
        if (!fsrForDisplay && wantsFsr && motionAvailableForDisplay &&
            linearDepthForFsr && temporalState.has_value() &&
            !mUpscalerExecutedThisFrame && mNriUpscalerPass.FsrAvailable()) {
            fsrAttempted = true;
            const auto fsrReads = resolveEffectPassReads(
                Oot3d::DisplayEffectPass::TemporalUpscaler);
            const auto* fsrInputBinding = findEffectColorRead(
                fsrReads,
                Oot3d::DisplayEffectPass::TemporalUpscaler);
            if (fsrInputBinding == nullptr ||
                fsrInputBinding->Kind !=
                    Oot3d::EffectResourceBindingKind::Image) {
                throw std::runtime_error(
                    "FSR pass has no declared color input");
            }
            const auto& fsrDepthBinding = requireEffectImageRead(
                fsrReads,
                Oot3d::EffectResource::HierarchicalDepth);
            const auto& fsrMotionBinding = requireEffectImageRead(
                fsrReads,
                Oot3d::EffectResource::MotionVectors);
            const auto& fsrReactiveBinding = requireEffectImageRead(
                fsrReads,
                Oot3d::EffectResource::ReactiveMask);
            Oot3d::NriTemporalUpscaleDispatchDesc fsr;
            fsr.FrameIndex = mCurrentFrame;
            fsr.InputImage = reinterpret_cast<VkImage>(
                fsrInputBinding->NativeImage);
            fsr.InputFormat =
                static_cast<VkFormat>(fsrInputBinding->Format);
            fsr.InputEncoding = fsrInputBinding->ColorEncoding;
            fsr.DepthImage = reinterpret_cast<VkImage>(
                fsrDepthBinding.NativeImage);
            fsr.DepthFormat =
                static_cast<VkFormat>(fsrDepthBinding.Format);
            fsr.MotionImage = reinterpret_cast<VkImage>(
                fsrMotionBinding.NativeImage);
            fsr.MotionFormat =
                static_cast<VkFormat>(fsrMotionBinding.Format);
            fsr.ReactiveImage = reinterpret_cast<VkImage>(
                fsrReactiveBinding.NativeImage);
            fsr.ReactiveFormat =
                static_cast<VkFormat>(fsrReactiveBinding.Format);
            fsr.InputWidth = fsrInputBinding->Width;
            fsr.InputHeight = fsrInputBinding->Height;
            fsr.Quality = presentationSettings.UpscalerMode;
            fsr.NearPlane = presentationPerspective->NearPlane;
            fsr.FarPlane = presentationPerspective->FarPlane;
            fsr.VerticalFovRadians = 2.0F * std::atan(
                std::abs(presentationPerspective->Top -
                         presentationPerspective->Bottom) /
                std::max(2.0F * presentationPerspective->NearPlane,
                         1.0e-6F));
            fsr.FrameTimeMilliseconds = nativeTemporalSample.Available()
                                            ? nativeTemporalSample.SampleDeltaSeconds * 1000.0F
                                            : 1000.0F / 30.0F;
            fsr.ViewUnitsPerMeter = 100.0F;
            fsr.Sharpness = presentationSettings.UpscalerSharpness;
            fsr.ResetHistory = !temporalState->HistoryValid ||
                               temporalState->CameraCut;
            transitionEffectPassGuides(
                Oot3d::DisplayEffectPass::TemporalUpscaler);
            mGpuProfiler.BeginScope(
                mCurrentFrame,
                Oot3d::GpuProfileScope::Upscaler,
                command);
            fsrForDisplay = transientUpscaledOutput != nullptr &&
                prepareUpscalerGuides(fsr) &&
                mNriUpscalerPass.Configure(*transientUpscaledOutput) &&
                mNriUpscalerPass.ExecuteFsr(
                    command, fsr, temporalUpscalerBarrierPlan);
            mGpuProfiler.EndScope(
                mCurrentFrame,
                Oot3d::GpuProfileScope::Upscaler,
                command);
            if (fsrForDisplay) {
                recordEffectPassTransitions(mNriUpscalerPass.LastBarrierExecution());
                retainUpscalerOutput(mNriUpscalerPass.OutputEncoding(), compositeForTaa, fsrOutputWidth,
                                     fsrOutputHeight);
            }
            mDiagnostics.RecordFsrUpscale(fsrForDisplay, fsr.ResetHistory, fsr.InputWidth, fsr.InputHeight,
                                          fsrOutputWidth, fsrOutputHeight);
            mDiagnostics.RecordUpscalerOutputOwnership(fsrForDisplay, mNriUpscalerPass.OutputOwnedByNri(),
                                                       mNriUpscalerPass.BarriersOwnedByNri());
        }
        bool dlssForDisplay = wantsDlss && canReuseUpscaler;
        bool dlssAttempted = false;
        uint32_t dlssOutputWidth = canReuseUpscaler ? mUpscalerOutputWidth : plannedUpscalerWidth;
        uint32_t dlssOutputHeight = canReuseUpscaler ? mUpscalerOutputHeight : plannedUpscalerHeight;
        slowSteps.Mark("composite_taa");
#if defined(_WIN32) && defined(ENABLE_OOT3D_D3D12_NGX_PROVIDER)
        const bool d3d12FrameDlssRequested = mD3d12NgxProvider.FrameDispatchRequested() &&
                                             mD3d12NgxProvider.Available() && !mNativePicaPresentedThisFrame &&
                                             !mFrameExternalComputeSplit;
#else
        constexpr bool d3d12FrameDlssRequested = false;
#endif
        const bool dlssImplementationAvailable = d3d12FrameDlssRequested || mNriUpscalerPass.DlssAvailable();
        if (!dlssForDisplay && wantsDlss && motionAvailableForDisplay && temporalState.has_value() &&
            effectGuideTarget != nullptr && !effectGuideTarget->WBuffering && !mUpscalerExecutedThisFrame &&
            dlssImplementationAvailable) {
            dlssAttempted = true;
            const auto dlssReads = resolveEffectPassReads(Oot3d::DisplayEffectPass::TemporalUpscaler);
            const auto* dlssInputBinding = findEffectColorRead(dlssReads, Oot3d::DisplayEffectPass::TemporalUpscaler);
            if (dlssInputBinding == nullptr || dlssInputBinding->Kind != Oot3d::EffectResourceBindingKind::Image) {
                throw std::runtime_error("DLSS pass has no declared color input");
            }
            const auto& dlssDepthBinding = requireEffectImageRead(dlssReads, Oot3d::EffectResource::NativeDepth);
            const auto& dlssMotionBinding = requireEffectImageRead(dlssReads, Oot3d::EffectResource::MotionVectors);
            const auto& dlssReactiveBinding = requireEffectImageRead(dlssReads, Oot3d::EffectResource::ReactiveMask);
            Oot3d::NriTemporalUpscaleDispatchDesc dlss;
            dlss.FrameIndex = mCurrentFrame;
            dlss.InputImage = reinterpret_cast<VkImage>(dlssInputBinding->NativeImage);
            dlss.InputFormat = static_cast<VkFormat>(dlssInputBinding->Format);
            dlss.InputEncoding = dlssInputBinding->ColorEncoding;
            // NGX DLSR consumes the native hardware-depth surface. W-buffer
            // views are rejected above instead of being mislabeled as HW depth.
            dlss.DepthImage = reinterpret_cast<VkImage>(dlssDepthBinding.NativeImage);
            dlss.DepthFormat = static_cast<VkFormat>(dlssDepthBinding.Format);
            dlss.MotionImage = reinterpret_cast<VkImage>(dlssMotionBinding.NativeImage);
            dlss.MotionFormat = static_cast<VkFormat>(dlssMotionBinding.Format);
            dlss.ReactiveImage = reinterpret_cast<VkImage>(dlssReactiveBinding.NativeImage);
            dlss.ReactiveFormat = static_cast<VkFormat>(dlssReactiveBinding.Format);
            dlss.InputWidth = dlssInputBinding->Width;
            dlss.InputHeight = dlssInputBinding->Height;
            dlss.Quality = presentationSettings.UpscalerMode;
            dlss.ResetHistory = !temporalState->HistoryValid || temporalState->CameraCut;
            dlss.DlssPreset = Oot3d::DlssRenderPreset(presentationSettings.UpscalerDlssModel,
                                                      presentationSettings.UpscalerMode);
            transitionEffectPassGuides(Oot3d::DisplayEffectPass::TemporalUpscaler);
            const bool dlssGuidesReady = prepareUpscalerGuides(dlss);
            bool d3d12FrameDlss = false;
            bool d3d12FrameContractReady = false;
            bool d3d12FramePrepared = false;
            bool d3d12FrameSplitSubmitted = false;
            bool d3d12FrameQueued = false;
            bool d3d12OutputAcquired = false;
            uint64_t d3d12FrameDispatchCount = 0U;
#if defined(_WIN32) && defined(ENABLE_OOT3D_D3D12_NGX_PROVIDER)
            if (d3d12FrameDlssRequested && dlssGuidesReady) {
                const bool dlssGuidesResampled =
                    dlss.DepthImage != reinterpret_cast<VkImage>(dlssDepthBinding.NativeImage);
                const Oot3d::D3d12NgxFrameContract contract{
                    dlss.InputWidth,
                    dlss.InputHeight,
                    dlssOutputWidth,
                    dlssOutputHeight,
                    kFramesInFlight,
                    dlss.Quality,
                    Oot3d::SceneColorIsSrgb(dlss.InputEncoding),
                };
                if (!mD3d12NgxProvider.ConfiguredForFrameContract(contract)) {
                    CheckNativeVk(vkDeviceWaitIdle(mDevice), "vkDeviceWaitIdle(D3D12 NGX frame contract)");
                    for (uint32_t slot = 0U; slot < kFramesInFlight; ++slot) {
                        const VkImage oldOutput = mD3d12NgxProvider.FrameOutputImage(slot);
                        if (oldOutput != VK_NULL_HANDLE)
                            mNriInterop.ForgetTexture(oldOutput);
                    }
                    mD3d12NgxProvider.ConfigureFrameContract(contract);
                }
                d3d12FrameContractReady = mD3d12NgxProvider.ConfiguredForFrameContract(contract);
                Oot3d::D3d12NgxFrameInputs bridgeInputs{
                    reinterpret_cast<VkImageView>(dlssInputBinding->NativeView),
                    dlssGuidesResampled ? mUpscalerGuidePass.DepthView()
                                        : reinterpret_cast<VkImageView>(dlssDepthBinding.NativeView),
                    dlssGuidesResampled ? mUpscalerGuidePass.MotionView()
                                        : reinterpret_cast<VkImageView>(dlssMotionBinding.NativeView),
                    dlssGuidesResampled ? mUpscalerGuidePass.ReactiveView()
                                        : reinterpret_cast<VkImageView>(dlssReactiveBinding.NativeView),
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    dlssGuidesResampled ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                        : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    dlss.JitterPixels,
                    dlss.ResetHistory,
                };
                Oot3d::D3d12NgxFrameSynchronization synchronization;
                if (d3d12FrameContractReady) {
                    d3d12FramePrepared =
                        mD3d12NgxProvider.PrepareFrame(command, mCurrentFrame, bridgeInputs, synchronization);
                }
                if (d3d12FramePrepared) {
                    VkCommandBuffer continuation = SplitFrameForExternalCompute(
                        synchronization.Semaphore, synchronization.VulkanInputsReady, synchronization.D3d12OutputReady);
                    if (continuation != VK_NULL_HANDLE) {
                        command = continuation;
                        d3d12FrameSplitSubmitted = true;
                        d3d12FrameQueued = mD3d12NgxProvider.QueuePreparedFrame(synchronization);
                        if (d3d12FrameQueued) {
                            d3d12OutputAcquired = mD3d12NgxProvider.RecordOutputAcquire(command, synchronization);
                        }
                        d3d12FrameDlss = d3d12FrameQueued && d3d12OutputAcquired;
                    }
                }
            }
            d3d12FrameDispatchCount = mD3d12NgxProvider.Status().FrameDispatchCount;
#endif
            dlssForDisplay = d3d12FrameDlss;
            if (!dlssForDisplay && dlssGuidesReady && mNriUpscalerPass.DlssAvailable()) {
                mGpuProfiler.BeginScope(mCurrentFrame, Oot3d::GpuProfileScope::Upscaler, command);
                dlssForDisplay = transientUpscaledOutput != nullptr &&
                                 mNriUpscalerPass.Configure(*transientUpscaledOutput) &&
                                 mNriUpscalerPass.ExecuteDlss(command, dlss, temporalUpscalerBarrierPlan);
                mGpuProfiler.EndScope(mCurrentFrame, Oot3d::GpuProfileScope::Upscaler, command);
            }
            if (dlssForDisplay) {
                mUpscalerUsedD3d12ThisFrame = d3d12FrameDlss;
                if (!d3d12FrameDlss) {
                    recordEffectPassTransitions(mNriUpscalerPass.LastBarrierExecution());
                }
                retainUpscalerOutput(d3d12FrameDlss ? dlss.InputEncoding : mNriUpscalerPass.OutputEncoding(),
                                     compositeForTaa, dlssOutputWidth, dlssOutputHeight);
            }
            mDiagnostics.RecordDlssUpscale(dlssForDisplay, dlss.ResetHistory, dlss.InputWidth, dlss.InputHeight,
                                           dlssOutputWidth, dlssOutputHeight);
            mDiagnostics.RecordD3d12NgxFrame(d3d12FrameDlssRequested, d3d12FrameContractReady, d3d12FramePrepared,
                                             d3d12FrameSplitSubmitted, d3d12FrameQueued, d3d12OutputAcquired,
                                             d3d12FrameDispatchCount);
            mDiagnostics.RecordUpscalerOutputOwnership(dlssForDisplay,
                                                       d3d12FrameDlss || mNriUpscalerPass.OutputOwnedByNri(),
                                                       d3d12FrameDlss || mNriUpscalerPass.BarriersOwnedByNri());
        }
        bool nisForDisplay = wantsNis && canReuseUpscaler;
        bool nisAttempted = false;
        uint32_t nisOutputWidth = canReuseUpscaler
            ? mUpscalerOutputWidth : plannedUpscalerWidth;
        uint32_t nisOutputHeight = canReuseUpscaler
            ? mUpscalerOutputHeight : plannedUpscalerHeight;
        if (!nisForDisplay && wantsNis && !mUpscalerExecutedThisFrame &&
            mNriUpscalerPass.Available()) {
            nisAttempted = true;
            const auto nisReads = resolveEffectPassReads(
                Oot3d::DisplayEffectPass::Nis);
            const auto* nisInputBinding = findEffectColorRead(
                nisReads, Oot3d::DisplayEffectPass::Nis);
            if (nisInputBinding == nullptr ||
                nisInputBinding->Kind !=
                    Oot3d::EffectResourceBindingKind::Image) {
                throw std::runtime_error(
                    "NIS pass has no declared color input");
            }
            const VkImage nisInputImage = reinterpret_cast<VkImage>(
                nisInputBinding->NativeImage);
            const VkFormat nisInputFormat =
                static_cast<VkFormat>(nisInputBinding->Format);
            const Oot3d::SceneColorEncoding nisInputEncoding =
                nisInputBinding->ColorEncoding;
            const uint32_t nisInputWidth = nisInputBinding->Width;
            const uint32_t nisInputHeight = nisInputBinding->Height;
            // The typed display-effect plan authorizes only a resolved scene
            // surface, so the shared provider never sees the secondary UI
            // output regardless of either target's dimensions.
            mGpuProfiler.BeginScope(
                mCurrentFrame,
                Oot3d::GpuProfileScope::Upscaler,
                command);
            nisForDisplay = transientUpscaledOutput != nullptr &&
                mNriUpscalerPass.Configure(*transientUpscaledOutput) &&
                mNriUpscalerPass.Execute(
                    command, mCurrentFrame,
                    nisInputImage, nisInputFormat,
                    nisInputWidth, nisInputHeight,
                    presentationSettings.UpscalerMode,
                    presentationSettings.UpscalerSharpness,
                    nisBarrierPlan,
                    nisInputEncoding);
            mGpuProfiler.EndScope(
                mCurrentFrame,
                Oot3d::GpuProfileScope::Upscaler,
                command);
            if (nisForDisplay) {
                recordEffectPassTransitions(
                    mNriUpscalerPass.LastBarrierExecution());
                retainUpscalerOutput(
                    mNriUpscalerPass.OutputEncoding(), compositeForTaa,
                    nisOutputWidth, nisOutputHeight);
            }
            mDiagnostics.RecordNisUpscale(
                nisForDisplay, nisInputWidth, nisInputHeight,
                nisOutputWidth, nisOutputHeight);
            mDiagnostics.RecordUpscalerOutputOwnership(
                nisForDisplay, mNriUpscalerPass.OutputOwnedByNri(),
                mNriUpscalerPass.BarriersOwnedByNri());
        }
        slowSteps.Mark("upscaler");
        const bool smaaEffectsNeedComposite =
            aoForDisplay || reflectionsForDisplay ||
            outlineForDisplay;
        const bool canReuseSmaa = wantsSmaa &&
            mSmaaExecutedThisFrame &&
            mSmaaRenderTargetNamespace ==
                transfer.RenderTargetNamespace &&
            mSmaaDisplayPhysicalAddress ==
                transfer.OutputPhysicalAddress &&
            mSmaaWidth == plannedSmaaWidth &&
            mSmaaHeight == plannedSmaaHeight;
        bool smaaForDisplay = canReuseSmaa;
        bool smaaAttempted = false;
        Oot3d::SceneColorEncoding smaaInputEncoding = canReuseSmaa
            ? mSmaaOutputEncodingThisFrame
            : compositeForTaa
                ? mSceneCompositePass.OutputEncoding()
                : Oot3d::NativePicaSceneColorEncoding();
        if (!smaaForDisplay && wantsSmaa &&
            !mSmaaExecutedThisFrame &&
            (!smaaEffectsNeedComposite || compositeForTaa)) {
            smaaAttempted = true;
            const auto smaaReads = resolveEffectPassReads(
                Oot3d::DisplayEffectPass::Smaa);
            const auto* smaaInputBinding = findEffectColorRead(
                smaaReads, Oot3d::DisplayEffectPass::Smaa);
            if (smaaInputBinding == nullptr ||
                smaaInputBinding->Kind !=
                    Oot3d::EffectResourceBindingKind::Image) {
                throw std::runtime_error(
                    "SMAA pass has no declared color input");
            }
            const VkImage smaaInputImage = reinterpret_cast<VkImage>(
                smaaInputBinding->NativeImage);
            const VkFormat smaaInputFormat =
                static_cast<VkFormat>(smaaInputBinding->Format);
            const uint32_t smaaInputWidth = smaaInputBinding->Width;
            const uint32_t smaaInputHeight = smaaInputBinding->Height;
            smaaInputEncoding = smaaInputBinding->ColorEncoding;
            mGpuProfiler.BeginScope(
                mCurrentFrame,
                Oot3d::GpuProfileScope::AntiAliasing,
                command);
            smaaForDisplay = transientAntiAliasedOutput != nullptr &&
                transientAntiAliasedOutput->Width == smaaInputWidth &&
                transientAntiAliasedOutput->Height == smaaInputHeight &&
                mSmaa1xPass.Configure(*transientAntiAliasedOutput) &&
                mSmaa1xPass.Execute(
                    command, mCurrentFrame, mFrameCounter,
                    smaaInputImage, smaaInputFormat,
                    smaaInputEncoding,
                    smaaBarrierPlan);
            mGpuProfiler.EndScope(
                mCurrentFrame,
                Oot3d::GpuProfileScope::AntiAliasing,
                command);
            if (smaaForDisplay) {
                recordEffectPassTransitions(
                    mSmaa1xPass.LastBarrierExecution());
                mSmaaExecutedThisFrame = true;
                mSmaaEffectsCompositedThisFrame =
                    compositeForTaa;
                mSmaaOutputEncodingThisFrame =
                    mSmaa1xPass.OutputEncoding();
                mSmaaRenderTargetNamespace =
                    transfer.RenderTargetNamespace;
                mSmaaDisplayPhysicalAddress =
                    transfer.OutputPhysicalAddress;
                mSmaaWidth = smaaInputWidth;
                mSmaaHeight = smaaInputHeight;
                mDiagnostics.RecordSmaa1x(
                    mSmaa1xPass.OutputsOwnedByNri(),
                    mSmaa1xPass.LookupsOwnedByNri(),
                    mSmaa1xPass.ComputeOwnedByNri(),
                    mSmaa1xPass.BarriersOwnedByNri(),
                    mSmaa1xPass.LookupUploadOwnedByNri());
            }
        }
        if (copyBoundary) {
            // A native boundary stops here: the scene stages are resolved, with
            // the same-extent anti-aliasing of a boundary before native UI, and
            // the boundary's declared output is what the native consumer
            // samples. Upscaling and scanout belong to the display that later
            // presents this scene, and this frame never presents.
            if (effectPlan.Enabled(Oot3d::DisplayEffectPass::Guides)) {
                recordDeclared(Oot3d::DisplayEffectPass::Guides,
                               guidesMaterialized ? EffectOutcome::Fused
                                                  : EffectOutcome::Failed);
            }
            recordDeclared(
                Oot3d::DisplayEffectPass::WorkingColor,
                producedOutcome(workingColorAttempted,
                                linearWorkingColorForDisplay,
                                canReuseWorkingColor));
            recordDeclared(
                Oot3d::DisplayEffectPass::AmbientOcclusion,
                producedOutcome(aoAttempted, aoForDisplay));
            recordDeclared(
                Oot3d::DisplayEffectPass::HiZ,
                producedOutcome(hiZAttempted, hiZExecutedNow, canReuseHiZ));
            recordDeclared(
                Oot3d::DisplayEffectPass::Reflections,
                reflectionsSkipped
                    ? EffectOutcome::Skipped
                    : producedOutcome(reflectionsAttempted, reflectionsForDisplay,
                                      canReuseReflections));
            recordDeclared(
                Oot3d::DisplayEffectPass::Motion,
                producedOutcome(motionAttempted, motionForDisplay,
                                canReuseMotion));
            if (effectPlan.Enabled(Oot3d::DisplayEffectPass::Outline)) {
                // The outline is fused into the composition at a boundary:
                // there is no presentation pass left to apply it in.
                EffectOutcome outlineOutcome = EffectOutcome::Skipped;
                if (outlineForDisplay && !guidesMaterialized) {
                    outlineOutcome = EffectOutcome::Failed;
                } else if (outlineForDisplay && compositeForTaa) {
                    outlineOutcome = EffectOutcome::Fused;
                }
                recordDeclared(Oot3d::DisplayEffectPass::Outline,
                               outlineOutcome);
            }
            recordDeclared(
                Oot3d::DisplayEffectPass::Composite,
                producedOutcome(compositeAttempted, compositeForTaa,
                                canReuseComposite));
            recordDeclared(
                Oot3d::DisplayEffectPass::Taa,
                producedOutcome(taaAttempted, taaForDisplay, canReuseTaa));
            recordDeclared(
                Oot3d::DisplayEffectPass::Smaa,
                producedOutcome(smaaAttempted, smaaForDisplay, canReuseSmaa));
            if (beforeNativeUi) {
                mDiagnostics.RecordBeforeUiAntiAliasing(
                    taaForDisplay || smaaForDisplay,
                    !antiAliasingAvailableHere);
            }
            // A declared scene stage that did not produce its output fails the
            // copy: an enabled effect must never silently sample the base
            // colour target. GI-sourced reflections without GI for this sample
            // produced theirs (no reflection), like the GI's zero irradiance.
            const bool declaredSceneStageFailed =
                (effectPlan.Enabled(Oot3d::DisplayEffectPass::AmbientOcclusion) &&
                 !aoForDisplay) ||
                (effectPlan.Enabled(Oot3d::DisplayEffectPass::Reflections) &&
                 !reflectionsForDisplay && !reflectionsSkipped) ||
                (effectPlan.Enabled(Oot3d::DisplayEffectPass::Outline) &&
                 !outlineForDisplay) ||
                (effectPlan.Enabled(Oot3d::DisplayEffectPass::WorkingColor) &&
                 !linearWorkingColorForDisplay) ||
                (effectPlan.Enabled(Oot3d::DisplayEffectPass::Composite) &&
                 !compositeForTaa) ||
                (wantsTaa && !taaForDisplay) ||
                (wantsSmaa && !smaaForDisplay);
            if (declaredSceneStageFailed) {
                mDiagnostics.RecordDisplayEffectGraphExecution(
                    effectExecution.Summary());
                // A declared stage that did not produce its output is a fatal
                // boundary failure, not a fallback to the base colour target.
                restoreEffectPassGuides();
                restoreBoundarySourceColor();
                SetNativeError(
                    error,
                    "native scene boundary did not resolve every declared "
                    "scene stage");
                return false;
            }
            // The declared scene colour, not an effect name, decides whether
            // this boundary composed anything: when no scene stage applies, the
            // native target already is the composed scene.
            const bool composedScene =
                effectPlan.SceneOutput != Oot3d::EffectResource::SceneColor;
            const auto compositionPass = effectPlan.FindPass(
                Oot3d::DisplayEffectPass::SceneComposition);
            if (compositionPass == nullptr || copySource == nullptr) {
                throw std::runtime_error(
                    "native scene boundary has no declared composition pass");
            }
            if (smaaForDisplay) {
                bindEffectImage(
                    Oot3d::EffectResource::AntiAliasedColor,
                    mSmaa1xPass.OutputImage(), mSmaa1xPass.OutputView(),
                    VK_FORMAT_R16G16B16A16_SFLOAT,
                    mSmaaWidth, mSmaaHeight,
                    mSmaaOutputEncodingThisFrame);
            }
            const auto compositionReads = resolveEffectPassReads(
                Oot3d::DisplayEffectPass::SceneComposition);
            const auto* compositionSource = findEffectColorRead(
                compositionReads,
                Oot3d::DisplayEffectPass::SceneComposition);
            if (compositionSource == nullptr ||
                compositionSource->Kind !=
                    Oot3d::EffectResourceBindingKind::Image ||
                !Oot3d::IsKnownSceneColorEncoding(
                    compositionSource->ColorEncoding)) {
                throw std::runtime_error(
                    "native scene boundary composition source is incomplete");
            }
            const uint32_t compositionWidth = copySource->Width;
            const uint32_t compositionHeight = copySource->Height;
            if (compositionWidth == 0U || compositionHeight == 0U) {
                throw std::runtime_error(
                    "native scene boundary source has no internal extent");
            }
            if (!composedScene) {
                // No scene stage applies to this version: either none is
                // declared, or the colour the source target already holds
                // carries them. The identity import of that target is the
                // composed scene, and it keeps the version's own provenance so
                // a consumer of this copy does not resolve the scene again.
                recordDeclared(Oot3d::DisplayEffectPass::SceneComposition,
                               EffectOutcome::Skipped);
                copyOutput->Image = copySource->ColorImage;
                copyOutput->Format = VK_FORMAT_R8G8B8A8_UNORM;
                copyOutput->Width = compositionWidth;
                copyOutput->Height = compositionHeight;
                copyOutput->ShaderRead = false;
                copyOutput->Encoding = Oot3d::NativePicaSceneColorEncoding();
                copyOutput->SceneEffectsResolved =
                    display.SceneEffectsResolved;
                mDiagnostics.RecordDisplayEffectGraphExecution(
                    effectExecution.Summary());
                restoreEffectPassGuides();
                restoreBoundarySourceColor();
                return true;
            }
            // The composed scene is materialized once, at the source's own
            // internal extent, in the encoded domain the native consumer
            // samples. The target is offscreen: no swapchain, no present, and
            // the pixel domain is the source's own, so the composition is a
            // format/encode conversion and not a resample.
            // ponytail: one reusable canvas per extent; a device wait on extent
            // change is the ceiling. Key it per extent if copy resolutions ever
            // thrash within a frame.
            if (mNativePicaSceneCopyCanvas.Image == VK_NULL_HANDLE ||
                mNativePicaSceneCopyCanvasWidth != compositionWidth ||
                mNativePicaSceneCopyCanvasHeight != compositionHeight) {
                if (mNativePicaSceneCopyCanvas.Image != VK_NULL_HANDLE) {
                    CheckNativeVk(
                        vkDeviceWaitIdle(mDevice),
                        "vkDeviceWaitIdle(native scene composition canvas)");
                    // The canvas is an NRI-owned texture, so the owned path
                    // releases its views and image together.
                    mNriInterop.DestroyOwnedTexture(
                        mNativePicaSceneCopyCanvas.Image);
                    mNativePicaSceneCopyCanvas = {};
                }
                Oot3d::NriOwnedTexture2DDesc canvas{};
                canvas.Width = compositionWidth;
                canvas.Height = compositionHeight;
                canvas.Format = VK_FORMAT_R8G8B8A8_UNORM;
                canvas.Usage =
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                if (!mNriInterop.CreateOwnedTexture2D(
                        canvas, mNativePicaSceneCopyCanvas)) {
                    throw std::runtime_error(
                        "native scene composition canvas is unavailable");
                }
                mNativePicaSceneCopyCanvasWidth = compositionWidth;
                mNativePicaSceneCopyCanvasHeight = compositionHeight;
            }
            bindEffectImage(
                Oot3d::EffectResource::SceneComposition,
                mNativePicaSceneCopyCanvas.Image,
                mNativePicaSceneCopyCanvas.ColorAttachmentView,
                VK_FORMAT_R8G8B8A8_UNORM, compositionWidth,
                compositionHeight);
            const VkImage composedImage = reinterpret_cast<VkImage>(
                compositionSource->NativeImage);
            const VkFormat composedFormat =
                static_cast<VkFormat>(compositionSource->Format);
            std::array<VkImage, 11> compositionImages{};
            std::array<VkFormat, 11> compositionFormats{};
            compositionImages.fill(composedImage);
            compositionFormats.fill(composedFormat);
            Oot3d::PicaScanoutPolicyInput compositionPolicy{};
            compositionPolicy.TransferFlags = 0U;
            compositionPolicy.AaMode = 0U;
            compositionPolicy.Width = compositionWidth;
            compositionPolicy.Height = compositionHeight;
            compositionPolicy.EffectsComposited = true;
            compositionPolicy.InputEncoding =
                compositionSource->ColorEncoding;
            compositionPolicy.TargetSrgb = false;
            if (!Oot3d::ValidatePicaScanoutPolicyInput(compositionPolicy)) {
                throw std::runtime_error(
                    "native scene composition color policy is invalid");
            }
            const Oot3d::PicaScanoutPushConstants composition =
                Oot3d::BuildPicaScanoutPushConstants(compositionPolicy);
            // The composition samples the same guide attachments the scene
            // stages do, through the same declared graph edges.
            transitionEffectPassGuides(
                Oot3d::DisplayEffectPass::SceneComposition);
            mGpuProfiler.BeginScope(
                mCurrentFrame, Oot3d::GpuProfileScope::SceneComposite,
                command);
            const bool nriComposition =
                mNriPicaScanoutPass.ConfigureOffscreen(
                    VK_FORMAT_R8G8B8A8_UNORM) &&
                mNriPicaScanoutPass.Execute(
                    {
                        .FrameSlot = mCurrentFrame,
                        .FrameId = mFrameCounter,
                        .CommandBuffer = command,
                        .TargetImage = mNativePicaSceneCopyCanvas.Image,
                        .TargetView =
                            mNativePicaSceneCopyCanvas.ColorAttachmentView,
                        .TargetFormat = VK_FORMAT_R8G8B8A8_UNORM,
                        .TargetWidth = compositionWidth,
                        .TargetHeight = compositionHeight,
                        .InputImages = compositionImages,
                        .InputFormats = compositionFormats,
                        .Push = composition,
                        .ViewportX = 0.0F,
                        .ViewportY = 0.0F,
                        .ViewportWidth =
                            static_cast<float>(compositionWidth),
                        .ViewportHeight =
                            static_cast<float>(compositionHeight),
                        .ScissorX = 0,
                        .ScissorY = 0,
                        .ScissorWidth = compositionWidth,
                        .ScissorHeight = compositionHeight,
                        .ClearColor = {},
                        .Overlay = false,
                        .OffscreenTarget = true,
                    },
                    sceneCompositionBarrierPlan);
            mGpuProfiler.EndScope(
                mCurrentFrame, Oot3d::GpuProfileScope::SceneComposite,
                command);
            if (!nriComposition) {
                // Without the materialization there is no composed source, and
                // sampling the base target instead would silently drop the
                // enabled effects.
                restoreEffectPassGuides();
                restoreBoundarySourceColor();
                SetNativeError(
                    error, "native scene composition pass is unavailable");
                return false;
            }
            recordDeclared(Oot3d::DisplayEffectPass::SceneComposition,
                           EffectOutcome::Executed);
            recordEffectPassTransitions(
                mNriPicaScanoutPass.LastBarrierExecution());
            copyOutput->Image = mNativePicaSceneCopyCanvas.Image;
            copyOutput->Format = VK_FORMAT_R8G8B8A8_UNORM;
            copyOutput->Width = compositionWidth;
            copyOutput->Height = compositionHeight;
            copyOutput->ShaderRead = true;
            // The materialization wrote the encoded display domain: the
            // composed colour is what a display-encoded native target holds.
            copyOutput->Encoding = Oot3d::NativePicaSceneColorEncoding();
            copyOutput->SceneEffectsResolved = true;
            mDiagnostics.RecordDisplayEffectGraphExecution(
                effectExecution.Summary());
            restoreEffectPassGuides();
            restoreBoundarySourceColor();
            return true;
        }
        const bool effectsCompositedForScanout =
            smaaForDisplay
                ? mSmaaEffectsCompositedThisFrame
                : (fsrForDisplay || dlssForDisplay || nisForDisplay)
                    ? mUpscalerEffectsCompositedThisFrame
                : taaForDisplay
                    ? mTaaEffectsCompositedThisFrame
                    : compositeForTaa;

        if (linearWorkingColorForDisplay) {
            bindEffectImage(
                Oot3d::EffectResource::LinearWorkingColor,
                mLinearSceneColorPass.OutputImage(),
                mLinearSceneColorPass.OutputView(),
                VK_FORMAT_R16G16B16A16_SFLOAT,
                mLinearSceneColorPass.Width(),
                mLinearSceneColorPass.Height(),
                Oot3d::SceneColorEncoding::Linear);
        }
        if (aoForDisplay) {
            bindEffectImage(
                Oot3d::EffectResource::AmbientOcclusion,
                cacao  ? mCacaoPass.OutputImage()
                : mxao ? mMxaoPass.OutputImage()
                       : mAmbientOcclusionPass.OutputImage(),
                cacao  ? mCacaoPass.OutputView()
                : mxao ? mMxaoPass.OutputView()
                       : mAmbientOcclusionPass.OutputView(),
                VK_FORMAT_R32_SFLOAT, effectGuideTarget->Width,
                effectGuideTarget->Height);
        }
        if (mHiZOutputValid && effectGuideTarget != nullptr &&
            mHiZRenderTargetNamespace == transfer.RenderTargetNamespace &&
            mHiZDisplayPhysicalAddress == transfer.OutputPhysicalAddress) {
            bindEffectImage(
                Oot3d::EffectResource::HierarchicalDepth,
                mHiZDepthPyramidPass.OutputImage(),
                mHiZDepthPyramidPass.OutputView(),
                VK_FORMAT_R32_SFLOAT, effectGuideTarget->Width,
                effectGuideTarget->Height);
        }
        if (reflectionsForDisplay || reflectionsSkipped) {
            bindEffectImage(
                Oot3d::EffectResource::ReflectionColor,
                activeReflectionImage, activeReflectionView,
                VK_FORMAT_R16G16B16A16_SFLOAT,
                effectGuideTarget->Width, effectGuideTarget->Height,
                reflectionOutputEncoding);
        }
        if (compositeForTaa || canReuseComposite) {
            bindEffectImage(
                Oot3d::EffectResource::CompositeColor,
                mSceneCompositePass.OutputImage(),
                mSceneCompositePass.OutputView(),
                VK_FORMAT_R16G16B16A16_SFLOAT,
                effectGuideTarget->Width, effectGuideTarget->Height,
                compositeForTaa
                    ? mSceneCompositePass.OutputEncoding()
                    : mCompositeOutputEncodingThisFrame);
        }
        if (motionAvailableForDisplay) {
            bindEffectImage(
                Oot3d::EffectResource::MotionVectors,
                mMotionVectorPass.OutputImage(),
                mMotionVectorPass.OutputView(),
                VK_FORMAT_R16G16B16A16_SFLOAT,
                effectGuideTarget->Width, effectGuideTarget->Height);
        }
        if (taaForDisplay) {
            bindEffectImage(
                Oot3d::EffectResource::TemporalColor,
                mTemporalAaPass.OutputImage(),
                mTemporalAaPass.OutputView(),
                VK_FORMAT_R16G16B16A16_SFLOAT,
                mTemporalAaPass.Width(), mTemporalAaPass.Height(),
                mTaaOutputEncodingThisFrame);
        }
        if (fsrForDisplay || dlssForDisplay || nisForDisplay) {
            const uint32_t outputWidth = fsrForDisplay
                ? fsrOutputWidth
                : dlssForDisplay ? dlssOutputWidth : nisOutputWidth;
            const uint32_t outputHeight = fsrForDisplay
                ? fsrOutputHeight
                : dlssForDisplay ? dlssOutputHeight : nisOutputHeight;
            VkImage upscaledImage = mNriUpscalerPass.OutputImage();
            VkImageView upscaledView = mNriUpscalerPass.OutputView();
            #if defined(_WIN32) && defined(ENABLE_OOT3D_D3D12_NGX_PROVIDER)
            if (dlssForDisplay && mUpscalerUsedD3d12ThisFrame) {
                upscaledImage =
                    mD3d12NgxProvider.FrameOutputImage(mCurrentFrame);
                upscaledView =
                    mD3d12NgxProvider.FrameOutputView(mCurrentFrame);
            }
            #endif
            if (effectPlan.NativeUiOverlay) {
                // The upscaler reconstructed the scene before native UI: lay
                // the display's UI over it (once per frame, reused with it).
                bool overlaid = canReuseUpscaler && mNativeUiOverlayExecutedThisFrame;
                if (!overlaid) {
                    const auto overlayReads = resolveEffectPassReads(
                        wantsNis ? Oot3d::DisplayEffectPass::Nis
                                 : Oot3d::DisplayEffectPass::TemporalUpscaler);
                    const auto& shown = requireEffectImageRead(
                        overlayReads, Oot3d::EffectResource::SceneColor);
                    const auto* beforeUiBinding = &requireEffectImageRead(
                        overlayReads, Oot3d::EffectResource::SceneBeforeUi);
                    mGpuProfiler.BeginScope(
                        mCurrentFrame, Oot3d::GpuProfileScope::Upscaler, command);
                    overlaid = mNativeUiOverlayPass.Execute(
                        mCurrentFrame, mFrameCounter,
                        {reinterpret_cast<VkImage>(shown.NativeImage),
                         static_cast<VkFormat>(shown.Format),
                         reinterpret_cast<VkImage>(beforeUiBinding->NativeImage),
                         static_cast<VkFormat>(beforeUiBinding->Format),
                         shown.Width, shown.Height,
                         upscaledImage, VK_FORMAT_R16G16B16A16_SFLOAT,
                         outputWidth, outputHeight,
                         Oot3d::SceneColorIsLinear(mUpscalerOutputEncodingThisFrame)});
                    mGpuProfiler.EndScope(
                        mCurrentFrame, Oot3d::GpuProfileScope::Upscaler, command);
                    if (!overlaid) {
                        throw std::runtime_error(
                            "native UI overlay failed: " +
                            mNativeUiOverlayPass.UnavailableReason());
                    }
                    recordEffectPassTransitions(
                        mNativeUiOverlayPass.LastBarrierExecution());
                    mNativeUiOverlayExecutedThisFrame = true;
                    mDiagnostics.RecordNativeUiOverlay();
                }
                upscaledImage = mNativeUiOverlayPass.OutputImage();
                upscaledView = mNativeUiOverlayPass.OutputView();
            }
            bindEffectImage(
                Oot3d::EffectResource::UpscaledColor,
                upscaledImage, upscaledView,
                VK_FORMAT_R16G16B16A16_SFLOAT,
                outputWidth, outputHeight,
                mUpscalerOutputEncodingThisFrame);
        }
        if (smaaForDisplay) {
            bindEffectImage(
                Oot3d::EffectResource::AntiAliasedColor,
                mSmaa1xPass.OutputImage(), mSmaa1xPass.OutputView(),
                VK_FORMAT_R16G16B16A16_SFLOAT,
                mSmaaWidth, mSmaaHeight,
                mSmaaOutputEncodingThisFrame);
        }

        transitionEffectPassGuides(Oot3d::DisplayEffectPass::Scanout);
        const auto physicalEffectPlan =
            Oot3d::BuildEffectGraphPhysicalPlan(
                effectPlan.Graph, effectResources);
        mDiagnostics.RecordDisplayEffectPhysicalPlan(
            physicalEffectPlan.Summary());

        const auto* scanoutPass = effectPlan.FindPass(
            Oot3d::DisplayEffectPass::Scanout);
        if (scanoutPass == nullptr) {
            throw std::runtime_error(
                "display effect graph has no compiled scanout pass");
        }
        const auto scanoutReads = resolveEffectPassReads(
            Oot3d::DisplayEffectPass::Scanout);
        const auto* graphScanoutSource = findEffectColorRead(
            scanoutReads, Oot3d::DisplayEffectPass::Scanout);
        if (graphScanoutSource == nullptr ||
            graphScanoutSource->Kind !=
                Oot3d::EffectResourceBindingKind::Image ||
            !Oot3d::IsKnownSceneColorEncoding(
                graphScanoutSource->ColorEncoding)) {
            throw std::runtime_error(
                "display effect scanout source contract is incomplete");
        }

        const VkImage scanoutImage = reinterpret_cast<VkImage>(
            graphScanoutSource->NativeImage);
        const VkImageView scanoutView = reinterpret_cast<VkImageView>(
            graphScanoutSource->NativeView);
        const VkFormat scanoutFormat =
            static_cast<VkFormat>(graphScanoutSource->Format);
        const Oot3d::SceneColorEncoding scanoutInputEncoding =
            graphScanoutSource->ColorEncoding;

        std::array<VkImage, 11> scanoutImages{};
        std::array<VkFormat, 11> scanoutFormats{};
        scanoutImages.fill(scanoutImage);
        scanoutFormats.fill(scanoutFormat);
        std::array<VkDescriptorImageInfo, 11> imageInfos{};
        imageInfos[0].sampler = mNativePicaScanoutSampler;
        imageInfos[0].imageView = scanoutView;
        imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos.fill(imageInfos[0]);
        constexpr std::array<Oot3d::EffectResource, 11> scanoutSlotResources{
            Oot3d::EffectResource::Count,
            Oot3d::EffectResource::AmbientOcclusion,
            Oot3d::EffectResource::NativeDepth,
            Oot3d::EffectResource::NormalGuide,
            Oot3d::EffectResource::HierarchicalDepth,
            Oot3d::EffectResource::MaterialGuide,
            Oot3d::EffectResource::ReflectionColor,
            Oot3d::EffectResource::AmbientGuide,
            Oot3d::EffectResource::RigidMotionGuide,
            Oot3d::EffectResource::FogGuide,
            Oot3d::EffectResource::OutlineGeometryGuide,
        };
        for (size_t slot = 1; slot < scanoutSlotResources.size(); ++slot) {
            if (!scanoutPass->ReadsResource(scanoutSlotResources[slot])) {
                continue;
            }
            const auto* binding =
                scanoutReads.Find(scanoutSlotResources[slot]);
            if (binding == nullptr || binding->Kind !=
                    Oot3d::EffectResourceBindingKind::Image) {
                throw std::runtime_error(
                    "display effect scanout used a non-image read");
            }
            scanoutImages[slot] = reinterpret_cast<VkImage>(
                binding->NativeImage);
            scanoutFormats[slot] = static_cast<VkFormat>(binding->Format);
            imageInfos[slot].imageView =
                reinterpret_cast<VkImageView>(binding->NativeView);
        }
        std::array<VkWriteDescriptorSet, 11> descriptorWrites{};
        for (uint32_t binding = 0; binding < descriptorWrites.size(); ++binding) {
            descriptorWrites[binding] = {
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            descriptorWrites[binding].dstSet = descriptor;
            descriptorWrites[binding].dstBinding = binding;
            descriptorWrites[binding].descriptorCount = 1;
            descriptorWrites[binding].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrites[binding].pImageInfo = &imageInfos[binding];
        }
        vkUpdateDescriptorSets(mDevice,
                               static_cast<uint32_t>(descriptorWrites.size()),
                               descriptorWrites.data(), 0, nullptr);

        const auto presentation = FitNativePicaPresentation(
            mSwapchainExtent.width, mSwapchainExtent.height,
            static_cast<float>(display.Height),
            static_cast<float>(display.Width));
        const VkViewport viewport{
            presentation.X, presentation.Y, presentation.Width,
            presentation.Height, 0.0F, 1.0F};
        const int32_t scissorLeft =
            static_cast<int32_t>(std::floor(presentation.X));
        const int32_t scissorTop =
            static_cast<int32_t>(std::floor(presentation.Y));
        const int32_t scissorRight = std::min(
            static_cast<int32_t>(mSwapchainExtent.width),
            static_cast<int32_t>(std::ceil(
                presentation.X + presentation.Width)));
        const int32_t scissorBottom = std::min(
            static_cast<int32_t>(mSwapchainExtent.height),
            static_cast<int32_t>(std::ceil(
                presentation.Y + presentation.Height)));
        const VkRect2D scissor{
            {scissorLeft, scissorTop},
            {static_cast<uint32_t>(scissorRight - scissorLeft),
             static_cast<uint32_t>(scissorBottom - scissorTop)}};
        const Oot3d::PicaScanoutPolicyInput scanoutPolicy{
                transfer.Flags,
                wantsSmaa ? 0U : mSpatialAaMode,
                display.Width,
                display.Height,
                mHiZDepthPyramidPass.MipCount(),
                aoForDisplay,
                outlineForDisplay,
                reflectionsForDisplay,
                taaForDisplay || fsrForDisplay || dlssForDisplay,
                effectsCompositedForScanout,
                scanoutInputEncoding,
                mSwapchainFormat == VK_FORMAT_B8G8R8A8_SRGB ||
                    mSwapchainFormat == VK_FORMAT_R8G8B8A8_SRGB,
                presentationSettings.Effects,
                presentationPerspective};
        if (!Oot3d::ValidatePicaScanoutPolicyInput(scanoutPolicy)) {
            throw std::runtime_error(
                "PICA scanout color policy is invalid");
        }
        const Oot3d::PicaScanoutPushConstants scanout =
            Oot3d::BuildPicaScanoutPushConstants(scanoutPolicy);
        mGpuProfiler.BeginScope(
            mCurrentFrame, Oot3d::GpuProfileScope::Scanout,
            command);
        const bool nriScanout = mNriPicaScanoutPass.Execute({
            mCurrentFrame,
            mFrameCounter,
            command,
            mSwapchainImages[mCurrentImage],
            mSwapchainImageViews[mCurrentImage],
            mSwapchainFormat,
            mSwapchainExtent.width,
            mSwapchainExtent.height,
            scanoutImages,
            scanoutFormats,
            scanout,
            viewport.x,
            viewport.y,
            viewport.width,
            viewport.height,
            scissor.offset.x,
            scissor.offset.y,
            scissor.extent.width,
            scissor.extent.height,
            {mClearColor[0], mClearColor[1],
             mClearColor[2], mClearColor[3]},
            alphaOverlay}, scanoutBarrierPlan);
        if (nriScanout) {
            recordEffectPassTransitions(
                mNriPicaScanoutPass.LastBarrierExecution());
            mDiagnostics.RecordNriScanout(
                true, mNriPicaScanoutPass.PipelineOwnedByNri(),
                mNriPicaScanoutPass.DescriptorsOwnedByNri(), true);
        } else {
            if (alphaOverlay) {
                VkRenderPassBeginInfo begin{
                    VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                begin.renderPass = mOverlayRenderPass;
                begin.framebuffer =
                    mSwapchainFramebuffers[mCurrentImage];
                begin.renderArea.extent = mSwapchainExtent;
                vkCmdBeginRenderPass(command, &begin,
                                     VK_SUBPASS_CONTENTS_INLINE);
                mRenderPassActive = true;
                mOverlayRenderPassActive = false;
            } else {
                BeginRenderPassIfNeeded();
            }
            vkCmdSetViewport(command, 0, 1, &viewport);
            vkCmdSetScissor(command, 0, 1, &scissor);
            vkCmdBindPipeline(
                command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                alphaOverlay ? mNativePicaScanoutOverlayPipeline
                             : mNativePicaScanoutPipeline);
            vkCmdBindDescriptorSets(
                command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                mNativePicaScanoutPipelineLayout, 0, 1,
                &descriptor, 0, nullptr);
            vkCmdPushConstants(
                command, mNativePicaScanoutPipelineLayout,
                VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(scanout),
                &scanout);
            vkCmdDraw(command, 3, 1, 0, 0);
            vkCmdEndRenderPass(command);
            mRenderPassActive = false;
            mOverlayRenderPassActive = false;
        }
        mGpuProfiler.EndScope(
            mCurrentFrame, Oot3d::GpuProfileScope::Scanout,
            command);
        if (scanout.InputLinear != 0U) {
            mDiagnostics.RecordLinearScanout(
                scanout.EncodeSrgb != 0U);
        }
        if (aoForDisplay && guideSampledResourceMask != 0U &&
            (compositeForTaa || !effectsCompositedForScanout)) {
            mDiagnostics.RecordAmbientOcclusionAmbientComposite();
        }

        if (effectPlan.Enabled(Oot3d::DisplayEffectPass::Guides)) {
            recordDeclared(
                Oot3d::DisplayEffectPass::Guides,
                guidesMaterialized
                    ? displayEffectsPreviouslyPresented
                        ? EffectOutcome::Reused
                        : EffectOutcome::Fused
                    : effectGuideTarget != nullptr
                        ? EffectOutcome::Failed
                        : EffectOutcome::Skipped);
        }
        recordDeclared(
            Oot3d::DisplayEffectPass::WorkingColor,
            producedOutcome(
                workingColorAttempted,
                linearWorkingColorForDisplay,
                canReuseWorkingColor));
        recordDeclared(
            Oot3d::DisplayEffectPass::AmbientOcclusion,
            producedOutcome(aoAttempted, aoForDisplay));
        recordDeclared(
            Oot3d::DisplayEffectPass::HiZ,
            producedOutcome(hiZAttempted, hiZExecutedNow,
                            canReuseHiZ));
        recordDeclared(
            Oot3d::DisplayEffectPass::Reflections,
            reflectionsSkipped
                ? EffectOutcome::Skipped
                : producedOutcome(reflectionsAttempted,
                                  reflectionsForDisplay,
                                  canReuseReflections));
        if (effectPlan.Enabled(Oot3d::DisplayEffectPass::Outline)) {
            EffectOutcome outlineOutcome = EffectOutcome::Skipped;
            if (outlineForDisplay && !guidesMaterialized) {
                outlineOutcome = EffectOutcome::Failed;
            } else if (outlineForDisplay && canReuseSmaa &&
                       effectsCompositedForScanout) {
                outlineOutcome = EffectOutcome::Reused;
            } else if (outlineForDisplay &&
                       (compositeForTaa || scanout.Outline != 0U)) {
                outlineOutcome = EffectOutcome::Fused;
            }
            recordDeclared(Oot3d::DisplayEffectPass::Outline,
                           outlineOutcome);
        }
        recordDeclared(
            Oot3d::DisplayEffectPass::Composite,
            producedOutcome(
                compositeAttempted, compositeForTaa,
                canReuseComposite));
        recordDeclared(
            Oot3d::DisplayEffectPass::Motion,
            producedOutcome(motionAttempted, motionForDisplay,
                            canReuseMotion));
        recordDeclared(
            Oot3d::DisplayEffectPass::Taa,
            producedOutcome(taaAttempted, taaForDisplay));
        recordDeclared(
            Oot3d::DisplayEffectPass::TemporalUpscaler,
            producedOutcome(fsrAttempted || dlssAttempted,
                            fsrForDisplay || dlssForDisplay));
        recordDeclared(
            Oot3d::DisplayEffectPass::Nis,
            producedOutcome(nisAttempted, nisForDisplay));
        recordDeclared(
            Oot3d::DisplayEffectPass::Smaa,
            producedOutcome(smaaAttempted, smaaForDisplay,
                            canReuseSmaa));
        recordDeclared(Oot3d::DisplayEffectPass::Scanout,
                       EffectOutcome::Executed);
        mDiagnostics.RecordDisplayEffectGraphExecution(
            effectExecution.Summary());

        restoreEffectPassGuides();

        if (!alphaOverlay) {
            mLastPresentedNativePicaDisplayTransfer = transfer;
        }
        mNativePicaPresentedThisFrame = true;
        if (reflectionsForDisplay && reflectionsAttempted &&
            activeReflectionProvider != Oot3d::ReflectionProvider::GlobalIllumination) {
            if (activeReflectionProvider ==
                Oot3d::ReflectionProvider::HiZ) {
                mDiagnostics.RecordHiZReflection(
                    true,
                    presentationSettings.Effects.ReflectionDebugView,
                    reflectionNriWrapped,
                    mHiZReflectionPass.OutputsOwnedByNri(),
                    mHiZReflectionPass.ComputeOwnedByNri(),
                    mHiZReflectionPass.BarriersOwnedByNri());
            } else {
                mDiagnostics.RecordFidelityFxSssr(
                    reflectionNriWrapped,
                    mFidelityFxSssrPass.OutputOwnedByNri(),
                    temporalState.has_value() &&
                        temporalState->HistoryValid,
                    linearWorkingColorForDisplay);
            }
        }
        mDiagnostics.RecordDisplayTransfer(
            true, smaaForDisplay ? 2U : mSpatialAaMode,
            static_cast<uint32_t>(mNativePicaSampleCount));
        return true;
}

// Resolves the sampled source of a raw copy at its order boundary. The copy's
// consumer samples the renderer-composed scene when a scene stage applies at
// that boundary, and the identity import of the native colour target
// otherwise. A false return fails the copy: an enabled effect never silently
// falls back to sampling the base target.
bool GfxRenderingAPIVulkan::ComposeNativePicaSceneForCopy(
    NativePicaRenderTarget& source, uint64_t afterDrawSubmissionId,
    GpuTextureCopySourceImport& output, std::string* error,
    const Renderer3ds::PicaCompositionStageAnchor* beforeUi) {
    output = {};
    if (!mFrameActive || source.ColorImage == VK_NULL_HANDLE ||
        source.Width == 0U || source.Height == 0U ||
        !mNativePicaCompositionSchedule.Valid()) {
        SetNativeError(
            error, "native scene boundary has no composable frame state");
        return false;
    }
    const auto perspective = SamplePerspective(&source);
    (void)mNativeSceneView.Publish(
        mFrameCounter, mPicaSceneFrame, perspective);
    const ::Oot3d::Renderer::PicaCompositionTargetReference compositionTarget{
        source.Key.RenderTargetNamespace,
        source.Key.ColorPhysicalAddress,
        source.Key.DepthPhysicalAddress,
        source.Key.Width,
        source.Key.Height,
        source.Key.ColorFormat,
        source.Key.DepthFormat,
    };
    const auto composition = Oot3d::ResolvePicaDisplayComposition(
        mNativePicaCompositionSchedule, compositionTarget);
    if (beforeUi != nullptr &&
        (beforeUi != mNativePicaCompositionSchedule.FindAnchorBeforeDraw(
             Oot3d::EffectStage::BeforeUi, compositionTarget, afterDrawSubmissionId) ||
         !mNativePicaCompositionSchedule.Reached(*beforeUi))) {
        SetNativeError(error, "native pre-UI composition has no reached schedule anchor");
        return false;
    }
    // The scene this copy samples is the scene as of this copy's own order, so
    // the declared end-of-target providers are dispatched only where the native
    // sequence has already reached them. Nothing later is flushed into the
    // composition, and transparent/UI content stays after it.
    const auto targetEnd = ResolveNativePicaTargetEndDispatch(
        mNativePicaCompositionSchedule,
        mDirectionalShadowSchedulePlan,
        mPointShadowSchedulePlan,
        mInteractiveGrassProviderPlan.Valid(), compositionTarget,
        afterDrawSubmissionId,
        mGrassWorldTargetsThisFrame.contains(source.Key),
        mGrassInsertionAttemptedTargetsThisFrame.contains(source.Key));
    const bool presentation =
        mNativePicaCompositionSchedule.Purpose() ==
        Renderer3ds::PicaCompositionPurpose::Presentation;
    if (presentation && targetEnd.ShadowAnchor != nullptr) {
        TryRenderDirectionalShadowMap(
            source, mFrameGraphicsSettings, *targetEnd.ShadowAnchor);
    }
    if (presentation && targetEnd.PointShadowAnchor != nullptr) {
        TryRenderPointShadowMaps(
            source, mFrameGraphicsSettings, *targetEnd.PointShadowAnchor);
    }
    if (presentation && targetEnd.DispatchGeometry) {
        TryRenderInteractiveGrass(
            source, mFrameGraphicsSettings,
            Oot3d::EffectGeometryProviderInvocationKind::DeclaredBoundary);
    }
    // The boundary is described by the target it composes, not by a display
    // transfer: the copy's own order, the source's own surface coordinates and
    // its own internal extent. Nothing here fabricates a present or rotates a
    // swapchain, and the composition never writes back into the source target,
    // so a repeated boundary recomposes the same scene instead of compounding
    // effects onto it.
    GfxNativePicaDisplayTransferView boundary{};
    boundary.AfterDrawSubmissionId = afterDrawSubmissionId;
    boundary.RenderTargetNamespace = source.Key.RenderTargetNamespace;
    boundary.InputPhysicalAddress = source.Key.ColorPhysicalAddress;
    boundary.OutputPhysicalAddress = source.Key.ColorPhysicalAddress;
    boundary.InputWidth = static_cast<uint16_t>(source.Width);
    boundary.InputHeight = static_cast<uint16_t>(source.Height);
    boundary.OutputWidth = static_cast<uint16_t>(source.Width);
    boundary.OutputHeight = static_cast<uint16_t>(source.Height);
    boundary.Flags = 0U;
    boundary.Present = false;
    NativePicaDisplayImage boundaryImage{};
    boundaryImage.Width = source.Width;
    boundaryImage.Height = source.Height;
    boundaryImage.Image = source.ColorImage;
    boundaryImage.View = source.ColorView;
    boundaryImage.Initialized = true;
    boundaryImage.NriOwned = false;
    boundaryImage.CompositionDomain = composition.Domain;
    boundaryImage.CompositionSequenceId = composition.SequenceId;
    // ConsumeDraw advances the schedule before submitting the draw. The
    // explicit pre-UI anchor proves this attachment has not received UI yet.
    boundaryImage.SceneResolved = beforeUi != nullptr || composition.SceneResolved;
    // The version this boundary consumes is the source's own: when a native
    // draw already sampled a resolved composition over this target, its colour
    // carries the scene stages and this boundary must not apply them again. The
    // provenance is resolved here, for this version, exactly as presentation
    // resolves it, so chaining raw copies and native effects stays generic.
    boundaryImage.SceneEffectsResolved =
        NativePicaSceneFullyResolvedForTarget(
            source.Key.RenderTargetNamespace,
            source.Key.ColorPhysicalAddress);
    if (!ExecuteNativePicaDisplayGraph(
            boundary, boundaryImage, &source, &output, error,
            beforeUi != nullptr)) {
        output = {};
        return false;
    }
    if (output.Image == VK_NULL_HANDLE) {
        output = {};
        SetNativeError(
            error, "native scene boundary declared no sampled source");
        return false;
    }
    return true;
}

bool GfxRenderingAPIVulkan::ClearPicaRenderTarget(
    uint64_t renderTargetNamespace, uint32_t colorPhysicalAddress,
    std::string* error) {
    try {
        if (!mFrameActive || colorPhysicalAddress == 0U) {
            throw std::runtime_error(
                "native PICA render-target clear is incomplete");
        }
        for (auto& [key, target] : mNativePicaRenderTargets) {
            if (key.RenderTargetNamespace != renderTargetNamespace ||
                key.ColorPhysicalAddress != colorPhysicalAddress) {
                continue;
            }
            if (target.ShadowImage != VK_NULL_HANDLE) {
                EndNativePicaRenderPass();
                VkImageMemoryBarrier barrier{
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = target.ShadowImage;
                barrier.subresourceRange.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount = 1U;
                barrier.subresourceRange.layerCount = 1U;
                barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                        VK_ACCESS_SHADER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                auto command = mCommandBuffers[mCurrentFrame];
                vkCmdPipelineBarrier(
                    command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                    nullptr, 1, &barrier);
                VkClearColorValue emptyShadow{};
                emptyShadow.uint32[0] = 0xFFFFFFFFU;
                vkCmdClearColorImage(command, target.ShadowImage,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     &emptyShadow, 1,
                                     &barrier.subresourceRange);
                std::swap(barrier.oldLayout, barrier.newLayout);
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                        VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(
                    command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                    nullptr, 1, &barrier);
            }
            BeginNativePicaRenderPass(target);
            std::array<VkClearAttachment, 5> attachments{};
            attachments[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            attachments[0].colorAttachment = 0U;
            attachments[0].clearValue.color = {{0.0F, 0.0F, 0.0F, 0.0F}};
            attachments[1].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            attachments[1].colorAttachment = 1U;
            attachments[1].clearValue.color = {{0.5F, 0.5F, 1.0F, 0.0F}};
            attachments[2].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            attachments[2].colorAttachment = 2U;
            attachments[2].clearValue.color = {{0.0F, 0.0F, 0.0F, 0.0F}};
            attachments[3].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            attachments[3].colorAttachment = 3U;
            attachments[3].clearValue.color = {{0.0F, 0.0F, 0.0F, 0.0F}};
            attachments[4].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (key.DepthFormat == 3U) {
                attachments[4].aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }
            attachments[4].clearValue.depthStencil = {1.0F, 0U};
            const VkClearRect clearRect{
                {{0, 0}, {target.Width, target.Height}}, 0U, 1U};
            vkCmdClearAttachments(mCommandBuffers[mCurrentFrame],
                                  static_cast<uint32_t>(attachments.size()),
                                  attachments.data(), 1U, &clearRect);
            // The cleared content is new: it carries no resolved scene effects
            // any more, so the provenance serial moves on.
            target.ContentWriteSerial = ++mGpuTextureCopyWriteSerial;
        }
        // A target not created yet already starts with the same transparent
        // color and default depth values in GetOrCreateNativePicaRenderTarget.
        return true;
    } catch (const std::exception& exception) {
        SetNativeError(error, exception.what());
        return false;
    }
}

bool GfxRenderingAPIVulkan::PrepareOverlay(
    std::string* error) {
    if (!mFrameActive) {
        SetNativeError(error,
                       "native PICA overlay requested outside an active frame");
        return false;
    }
    if (mOverlayRenderPassActive && mRenderPassActive) {
        return true;
    }
    if (!mLastPresentedNativePicaDisplayTransfer.has_value()) {
        return true;
    }
    GfxNativePicaDisplayTransferView transfer =
        *mLastPresentedNativePicaDisplayTransfer;
    transfer.Present = true;
    if (!SubmitPicaDisplayTransfer(transfer, error)) {
        return false;
    }
    if (mOverlayRenderPass == VK_NULL_HANDLE ||
        mSwapchainFramebuffers.empty()) {
        SetNativeError(error, "native PICA overlay render pass is unavailable");
        return false;
    }

    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = mOverlayRenderPass;
    begin.framebuffer = mSwapchainFramebuffers[mCurrentImage];
    begin.renderArea.extent = mSwapchainExtent;
    vkCmdBeginRenderPass(mCommandBuffers[mCurrentFrame], &begin,
                         VK_SUBPASS_CONTENTS_INLINE);
    mGpuProfiler.BeginScope(
        mCurrentFrame, Oot3d::GpuProfileScope::Overlay,
        mCommandBuffers[mCurrentFrame]);
    mRenderPassActive = true;
    mOverlayRenderPassActive = true;
    ApplyDynamicViewportAndScissor();
    mDiagnostics.RecordOverlay();
    return true;
}

bool GfxRenderingAPIVulkan::SubmitPicaMemoryFill(
    const GfxNativePicaMemoryFillView& fill, std::string* error) {
    try {
        if (!mFrameActive || fill.StartPhysicalAddress == 0U ||
            fill.EndPhysicalAddress <= fill.StartPhysicalAddress ||
            (fill.Control & 1U) == 0U) {
            throw std::runtime_error("native PICA memory fill is incomplete");
        }
        const auto previous = std::find_if(
            mPendingNativePicaMemoryFills.begin(),
            mPendingNativePicaMemoryFills.end(),
            [&](const auto& pending) {
                return pending.StartPhysicalAddress == fill.StartPhysicalAddress &&
                       pending.EndPhysicalAddress == fill.EndPhysicalAddress &&
                       pending.RenderTargetNamespace ==
                           fill.RenderTargetNamespace;
            });
        if (previous != mPendingNativePicaMemoryFills.end()) {
            *previous = fill;
        } else {
            mPendingNativePicaMemoryFills.push_back(fill);
        }
        for (auto& [key, target] : mNativePicaRenderTargets) {
            ApplyPendingNativePicaMemoryFills(target);
        }
        mDiagnostics.RecordMemoryFill();
        return true;
    } catch (const std::exception& exception) {
        SetNativeError(error, exception.what());
        return false;
    }
}

bool GfxRenderingAPIVulkan::CapturePicaTextureCache(
    std::vector<GfxNativePicaTextureCacheEntrySnapshot>& snapshots,
    std::string* error) {
    snapshots.clear();
    try {
        if (mDevice == VK_NULL_HANDLE || mFrameActive || mFrameSubmitted) {
            throw std::runtime_error(
                "native PICA texture capture requires an idle frame boundary");
        }
        WaitForAllPresents();
        CheckNativeVk(vkDeviceWaitIdle(mDevice),
                      "vkDeviceWaitIdle(native PICA texture capture)");
        for (const auto& [key, texture] : mNativePicaTextures) {
            // Live render-target aliases are restored with the render target.
            if (texture.LiveRenderTargetAlias) {
                continue;
            }
            // A derived raw-copy sample is a function of its producers and of the
            // snapshot's own bytes, so it is regenerated on restore instead of
            // being serialized; only the immutable producer versions travel.
            if (key.GpuCopyIdentity != 0U) {
                continue;
            }
            const auto expectedBytes = NativePicaDecodedMipChainSize(
                texture.Width, texture.Height, texture.MipLevels);
            if (texture.Width == 0U || texture.Height == 0U ||
                !expectedBytes.has_value()) {
                throw std::runtime_error(
                    "native PICA cached texture has an invalid identity");
            }
            GfxNativePicaTextureCacheEntrySnapshot snapshot;
            snapshot.ContentHash = key.ContentHash;
            snapshot.ReplacementGeneration = key.ReplacementGeneration;
            snapshot.PhysicalAddress = key.PhysicalAddress;
            snapshot.SourceWidth = key.Width;
            snapshot.SourceHeight = key.Height;
            snapshot.NativeFormat = key.Format;
            snapshot.NativeType = key.Type;
            snapshot.NativeWrapS = key.WrapS;
            snapshot.NativeWrapT = key.WrapT;
            snapshot.MinLinear = key.MinLinear;
            snapshot.MagLinear = key.MagLinear;
            snapshot.MipLinear = key.MipLinear;
            snapshot.LodBiasRaw = key.LodBiasRaw;
            snapshot.MinMipLevel = key.MinMipLevel;
            snapshot.MaxMipLevel = key.MaxMipLevel;
            snapshot.CustomReplacement = key.CustomReplacement;
            snapshot.ImageWidth = texture.Width;
            snapshot.ImageHeight = texture.Height;
            snapshot.MipLevels = static_cast<uint8_t>(texture.MipLevels);
            snapshot.ImageFormat =
                key.Type == 2U && !key.CustomReplacement
                    ? GfxNativePicaTextureSnapshotFormat::R32Uint
                    : GfxNativePicaTextureSnapshotFormat::Rgba8;
            snapshot.CustomReplacementHash =
                texture.CustomReplacementHash;
            snapshot.CustomReplacementPending =
                texture.CustomReplacementPending;
            snapshot.CustomReplacementReady =
                texture.CustomReplacementReady;
            if (texture.Uploaded) {
                for (uint32_t level = 0U; level < texture.MipLevels;
                     ++level) {
                    auto mip = CaptureNativePicaImageBytes(
                        texture.Image,
                        std::max(1U, texture.Width >> level),
                        std::max(1U, texture.Height >> level), 4U,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT, level);
                    snapshot.PixelBytes.insert(snapshot.PixelBytes.end(),
                                               mip.begin(), mip.end());
                }
            } else {
                snapshot.PixelBytes = texture.Rgba8;
            }
            if (snapshot.PixelBytes.size() != *expectedBytes) {
                throw std::runtime_error(
                    "native PICA cached texture payload is invalid");
            }
            snapshots.push_back(std::move(snapshot));
        }
        // Immutable copy producers travel in the same vector: each carries the
        // full captured GPU image at its internal extent plus the identity and
        // span table a resumed plan's references resolve against.
        for (const auto& [identity, surface] : mGpuTextureCopySurfaces) {
            if (surface.Image == VK_NULL_HANDLE || surface.ImageWidth == 0U ||
                surface.ImageHeight == 0U) {
                continue;
            }
            GfxNativePicaTextureCacheEntrySnapshot snapshot;
            snapshot.HasCopyProducer = true;
            snapshot.CopyProducer.RenderTargetNamespace =
                surface.RenderTargetNamespace;
            snapshot.CopyProducer.CompletionId = surface.CompletionId;
            snapshot.CopyProducer.AfterDrawSubmissionId =
                surface.AfterDrawSubmissionId;
            snapshot.CopyProducer.SourcePhysicalAddress =
                surface.SourceSurfaceAddress;
            snapshot.CopyProducer.SourceWidth = surface.SourceWidth;
            snapshot.CopyProducer.SourceHeight = surface.SourceHeight;
            snapshot.CopyProducer.SourceNativeFormat = surface.SourceFormat;
            snapshot.CopyProducer.Spans = surface.Spans;
            snapshot.CopyProducer.ColorEncoding =
                static_cast<uint8_t>(surface.Encoding);
            snapshot.CopyProducer.SceneEffectsResolved =
                surface.SceneEffectsResolved;
            snapshot.ImageWidth = surface.ImageWidth;
            snapshot.ImageHeight = surface.ImageHeight;
            snapshot.ImageFormat = GfxNativePicaTextureSnapshotFormat::Rgba8;
            snapshot.MipLevels = 1U;
            snapshot.PixelBytes = CaptureNativePicaImageBytes(
                surface.Image, surface.ImageWidth, surface.ImageHeight, 4U,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT);
            if (snapshot.PixelBytes.size() !=
                static_cast<size_t>(surface.ImageWidth) * surface.ImageHeight *
                    4U) {
                throw std::runtime_error(
                    "native PICA copy producer capture is incomplete");
            }
            snapshots.push_back(std::move(snapshot));
        }
        return true;
    } catch (const std::exception& exception) {
        snapshots.clear();
        SetNativeError(error, exception.what());
        return false;
    }
}

bool GfxRenderingAPIVulkan::RestorePicaTextureCache(
    std::span<const GfxNativePicaTextureCacheEntrySnapshot> snapshots,
    std::string* error) {
    try {
        if (mDevice == VK_NULL_HANDLE || mFrameActive || mFrameSubmitted) {
            throw std::runtime_error(
                "native PICA texture restore requires an idle frame boundary");
        }
        if (std::any_of(
                mNativePicaTextures.begin(), mNativePicaTextures.end(),
                [](const auto& entry) {
                    return entry.second.LiveRenderTargetAlias;
                })) {
            throw std::runtime_error(
                "native PICA texture restore requires reset renderer state");
        }
        std::set<NativePicaTextureKey> identities;
        for (const auto& snapshot : snapshots) {
            if (snapshot.HasCopyProducer) {
                const auto& producer = snapshot.CopyProducer;
                // Namespace 0 is the valid default namespace, not a sentinel.
                if (producer.CompletionId == 0U || producer.SourceWidth == 0U ||
                    producer.SourceHeight == 0U ||
                    producer.SourceNativeFormat > 4U || producer.Spans.empty() ||
                    snapshot.ImageWidth == 0U || snapshot.ImageHeight == 0U ||
                    snapshot.ImageFormat !=
                        GfxNativePicaTextureSnapshotFormat::Rgba8 ||
                    snapshot.MipLevels != 1U ||
                    snapshot.PixelBytes.size() !=
                        static_cast<size_t>(snapshot.ImageWidth) *
                            snapshot.ImageHeight * 4U) {
                    throw std::runtime_error(
                        "native PICA copy producer snapshot is incompatible");
                }
                continue;
            }
            const auto expectedBytes = NativePicaDecodedMipChainSize(
                snapshot.ImageWidth, snapshot.ImageHeight,
                snapshot.MipLevels);
            const uint8_t imageFormat =
                static_cast<uint8_t>(snapshot.ImageFormat);
            NativePicaTextureKey key;
            key.ContentHash = snapshot.ContentHash;
            key.ReplacementGeneration = snapshot.ReplacementGeneration;
            key.PhysicalAddress = snapshot.PhysicalAddress;
            key.Width = snapshot.SourceWidth;
            key.Height = snapshot.SourceHeight;
            key.Format = snapshot.NativeFormat;
            key.Type = snapshot.NativeType;
            key.WrapS = snapshot.NativeWrapS;
            key.WrapT = snapshot.NativeWrapT;
            key.MinLinear = snapshot.MinLinear;
            key.MagLinear = snapshot.MagLinear;
            key.MipLinear = snapshot.MipLinear;
            key.LodBiasRaw = snapshot.LodBiasRaw;
            key.MinMipLevel = snapshot.MinMipLevel;
            key.MaxMipLevel = snapshot.MaxMipLevel;
            key.CustomReplacement = snapshot.CustomReplacement;
            if (snapshot.SourceWidth == 0U ||
                snapshot.SourceHeight == 0U ||
                snapshot.ImageWidth == 0U ||
                snapshot.ImageHeight == 0U ||
                snapshot.MipLevels == 0U ||
                !expectedBytes.has_value() ||
                imageFormat > static_cast<uint8_t>(
                                  GfxNativePicaTextureSnapshotFormat::R32Uint) ||
                (snapshot.ImageFormat ==
                         GfxNativePicaTextureSnapshotFormat::R32Uint &&
                 (snapshot.NativeType != 2U ||
                  snapshot.CustomReplacement)) ||
                (snapshot.ImageFormat ==
                         GfxNativePicaTextureSnapshotFormat::R32Uint &&
                 snapshot.MipLevels != 1U) ||
                *expectedBytes != snapshot.PixelBytes.size() ||
                !identities.insert(key).second) {
                throw std::runtime_error(
                    "native PICA texture snapshot is incompatible");
            }
        }
        for (auto& [key, texture] : mNativePicaTextures) {
            DestroyTexture(texture);
        }
        mNativePicaTextures.clear();
        DestroyGpuTextureCopySurfaces();
        for (const auto& snapshot : snapshots) {
            if (snapshot.HasCopyProducer) {
                // Rebuild the immutable version under the same register key a
                // resumed plan's references resolve against. The image itself is
                // created in the next active frame, where an upload path exists.
                const auto& producer = snapshot.CopyProducer;
                GpuTextureCopySurface surface;
                surface.Identity = ++mGpuTextureCopySerial;
                surface.RenderTargetNamespace = producer.RenderTargetNamespace;
                surface.CompletionId = producer.CompletionId;
                surface.AfterDrawSubmissionId = producer.AfterDrawSubmissionId;
                // The declared destination surface is not part of the persisted
                // identity: the sampler's own layout defines it. Keep the bytes
                // consistent with the source geometry for the live sweep that
                // still consults these fields.
                const uint32_t restoredBpp =
                    producer.SourceNativeFormat == 0U   ? 4U
                    : producer.SourceNativeFormat == 1U ? 3U
                                                        : 2U;
                surface.DestinationAddress = producer.SourcePhysicalAddress;
                surface.DestinationBytes = producer.SourceWidth *
                                           producer.SourceHeight * restoredBpp;
                surface.TextureWidth = producer.SourceWidth;
                surface.TextureHeight = producer.SourceHeight;
                surface.TextureFormat = producer.SourceNativeFormat;
                surface.SourceSurfaceAddress = producer.SourcePhysicalAddress;
                surface.SourceWidth = producer.SourceWidth;
                surface.SourceHeight = producer.SourceHeight;
                surface.SourceFormat = producer.SourceNativeFormat;
                surface.Spans = producer.Spans;
                surface.ImageWidth = snapshot.ImageWidth;
                surface.ImageHeight = snapshot.ImageHeight;
                surface.Encoding =
                    static_cast<Oot3d::SceneColorEncoding>(
                        producer.ColorEncoding);
                surface.SceneEffectsResolved =
                    producer.SceneEffectsResolved;
                surface.PendingPixels = snapshot.PixelBytes;
                const uint64_t identity = surface.Identity;
                mGpuTextureCopySurfaces.emplace(identity, std::move(surface));
                mGpuTextureCopyProducers[{producer.RenderTargetNamespace,
                                          producer.CompletionId,
                                          producer.AfterDrawSubmissionId}] =
                    identity;
                ++mGpuTextureCopySamplerCost.ProducerRestores;
                continue;
            }
            NativePicaTextureKey key;
            key.ContentHash = snapshot.ContentHash;
            key.ReplacementGeneration = snapshot.ReplacementGeneration;
            key.PhysicalAddress = snapshot.PhysicalAddress;
            key.Width = snapshot.SourceWidth;
            key.Height = snapshot.SourceHeight;
            key.Format = snapshot.NativeFormat;
            key.Type = snapshot.NativeType;
            key.WrapS = snapshot.NativeWrapS;
            key.WrapT = snapshot.NativeWrapT;
            key.MinLinear = snapshot.MinLinear;
            key.MagLinear = snapshot.MagLinear;
            key.MipLinear = snapshot.MipLinear;
            key.LodBiasRaw = snapshot.LodBiasRaw;
            key.MinMipLevel = snapshot.MinMipLevel;
            key.MaxMipLevel = snapshot.MaxMipLevel;
            key.CustomReplacement = snapshot.CustomReplacement;
            TextureRecord texture;
            texture.Width = snapshot.ImageWidth;
            texture.Height = snapshot.ImageHeight;
            texture.MipLevels = snapshot.MipLevels;
            texture.Rgba8 = snapshot.PixelBytes;
            texture.CustomReplacementApplied = snapshot.CustomReplacement;
            if (snapshot.CustomReplacement)
                texture.CustomReplacementPixelHash = ::Oot3d::Renderer::AzaharCityHash64(texture.Rgba8);
            texture.SamplerState.MinFilter = snapshot.NativeType != 2U
                ? NativePicaMinFilter(snapshot.MinLinear,
                                      snapshot.MipLinear,
                                      snapshot.MipLevels)
                : GfxNativeTextureFilter::Nearest;
            texture.SamplerState.MagFilter =
                snapshot.NativeType != 2U && snapshot.MagLinear
                    ? GfxNativeTextureFilter::Linear
                    : GfxNativeTextureFilter::Nearest;
            texture.SamplerState.WrapS =
                DecodeNativePicaWrap(snapshot.NativeWrapS);
            texture.SamplerState.WrapT =
                DecodeNativePicaWrap(snapshot.NativeWrapT);
            texture.SamplerState.LodBias = 0.0F;
            texture.SamplerState.MinMipLevel = snapshot.NativeType != 2U
                ? snapshot.MinMipLevel : 0U;
            texture.SamplerState.MaxMipLevel = snapshot.NativeType != 2U
                ? snapshot.MaxMipLevel : 0U;
            texture.CustomReplacementHash =
                snapshot.CustomReplacementHash;
            texture.CustomReplacementPending =
                snapshot.CustomReplacementPending;
            texture.CustomReplacementReady =
                snapshot.CustomReplacementReady;
            mNativePicaTextures.emplace(key, std::move(texture));
        }
        return true;
    } catch (const std::exception& exception) {
        SetNativeError(error, exception.what());
        return false;
    }
}

bool GfxRenderingAPIVulkan::CapturePicaColorTargets(
    std::vector<GfxNativePicaRenderTargetColorSnapshot>& snapshots,
    std::string* error) {
    snapshots.clear();
    try {
        if (mDevice == VK_NULL_HANDLE || mFrameActive || mFrameSubmitted) {
            throw std::runtime_error(
                "native PICA color capture requires an idle frame boundary");
        }
        if (mNativePicaSampleCount != VK_SAMPLE_COUNT_1_BIT) {
            throw std::runtime_error(
                "native PICA color capture does not yet support multisampled targets");
        }
        WaitForAllPresents();
        CheckNativeVk(vkDeviceWaitIdle(mDevice),
                      "vkDeviceWaitIdle(native PICA color capture)");
        for (const auto& [key, target] : mNativePicaRenderTargets) {
            if (target.ColorImage == VK_NULL_HANDLE) {
                continue;
            }
            GfxNativePicaRenderTargetColorSnapshot snapshot;
            // The saved colour carries the resolved scene effects only while the
            // last write to this target is the one that produced them; a sticky
            // flag would suppress the scene stages for unrelated content.
            snapshot.SceneEffectsResolved =
                target.SceneEffectsResolvedSerial != 0U &&
                target.SceneEffectsResolvedSerial == target.ContentWriteSerial;
            snapshot.RenderTargetNamespace = key.RenderTargetNamespace;
            snapshot.ColorPhysicalAddress = key.ColorPhysicalAddress;
            snapshot.DepthPhysicalAddress = key.DepthPhysicalAddress;
            snapshot.FramebufferWidth = key.Width;
            snapshot.FramebufferHeight = key.Height;
            snapshot.FramebufferColorFormat = key.ColorFormat;
            snapshot.FramebufferDepthFormat = key.DepthFormat;
            snapshot.RenderScalePermille = key.RenderScalePermille;
            snapshot.ImageWidth = target.Width;
            snapshot.ImageHeight = target.Height;
            snapshot.SampleCount = 1U;
            snapshot.WBuffering = target.WBuffering;
            const auto captureColorAttachment =
                [&](VkImage image, uint32_t bytesPerPixel) {
                    return CaptureNativePicaImageBytes(
                        image, target.Width, target.Height,
                        bytesPerPixel,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                };
            snapshot.ColorRgba8 =
                captureColorAttachment(target.ColorImage, 4U);
            if (target.Attachments.NativeColorOnly()) {
                const size_t pixelCount =
                    static_cast<size_t>(target.Width) * target.Height;
                snapshot.NormalGuideRgba8.resize(pixelCount * 4U);
                snapshot.MaterialGuideRgba8.assign(pixelCount * 4U, 0U);
                snapshot.RigidMotionGuideRgba16FloatLe.assign(
                    pixelCount * 8U, 0U);
                snapshot.AmbientGuideRgba8.resize(pixelCount * 4U);
                for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
                    const size_t offset = pixel * 4U;
                    snapshot.NormalGuideRgba8[offset] = 128U;
                    snapshot.NormalGuideRgba8[offset + 1U] = 128U;
                    snapshot.NormalGuideRgba8[offset + 2U] = 255U;
                    snapshot.NormalGuideRgba8[offset + 3U] = 0U;
                    snapshot.AmbientGuideRgba8[offset] = 255U;
                    snapshot.AmbientGuideRgba8[offset + 1U] = 255U;
                    snapshot.AmbientGuideRgba8[offset + 2U] = 255U;
                    snapshot.AmbientGuideRgba8[offset + 3U] = 0U;
                }
            } else {
                snapshot.NormalGuideRgba8 =
                    captureColorAttachment(target.NormalGuideImage, 4U);
                snapshot.MaterialGuideRgba8 =
                    captureColorAttachment(target.MaterialGuideImage, 4U);
                snapshot.RigidMotionGuideRgba16FloatLe =
                    captureColorAttachment(
                        target.RigidMotionGuideImage, 8U);
                snapshot.AmbientGuideRgba8 =
                    captureColorAttachment(target.AmbientGuideImage, 4U);
            }
            snapshot.ShadowR32UintLe = CaptureNativePicaImageBytes(
                target.ShadowImage, target.Width, target.Height, 4U,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            CaptureNativePicaDepthStencilImage(
                target.DepthImage, target.Width, target.Height,
                snapshot.DepthValues, snapshot.StencilValues);
            snapshots.push_back(std::move(snapshot));
        }
        return true;
    } catch (const std::exception& exception) {
        snapshots.clear();
        SetNativeError(error, exception.what());
        return false;
    }
}

bool GfxRenderingAPIVulkan::RestorePicaColorTargets(
    std::span<const GfxNativePicaRenderTargetColorSnapshot> snapshots,
    std::string* error) {
    try {
        if (mDevice == VK_NULL_HANDLE || mFrameActive || mFrameSubmitted) {
            throw std::runtime_error(
                "native PICA color restore requires an idle frame boundary");
        }
        if (!mNativePicaRenderTargets.empty()) {
            throw std::runtime_error(
                "native PICA color restore requires reset renderer state");
        }
        if (mNativePicaSampleCount != VK_SAMPLE_COUNT_1_BIT) {
            throw std::runtime_error(
                "native PICA color restore does not yet support multisampled targets");
        }
        const uint16_t currentScale = static_cast<uint16_t>(std::lround(
            mInternalResolutionScale * 1000.0F));
        std::set<std::tuple<uint64_t, uint32_t, uint32_t, uint16_t,
                            uint16_t, uint8_t, uint8_t, uint16_t>>
            identities;
        for (const auto& snapshot : snapshots) {
            const uint64_t expectedBytes =
                static_cast<uint64_t>(snapshot.ImageWidth) *
                snapshot.ImageHeight * 4U;
            const uint64_t expectedPixels =
                static_cast<uint64_t>(snapshot.ImageWidth) *
                snapshot.ImageHeight;
            if (snapshot.ColorPhysicalAddress == 0U ||
                snapshot.FramebufferWidth == 0U ||
                snapshot.FramebufferHeight == 0U ||
                snapshot.ImageWidth == 0U || snapshot.ImageHeight == 0U ||
                snapshot.SampleCount != 1U ||
                snapshot.RenderScalePermille != currentScale ||
                expectedBytes != snapshot.ColorRgba8.size() ||
                expectedBytes != snapshot.NormalGuideRgba8.size() ||
                expectedBytes != snapshot.MaterialGuideRgba8.size() ||
                expectedPixels * 8U !=
                    snapshot.RigidMotionGuideRgba16FloatLe.size() ||
                expectedBytes != snapshot.AmbientGuideRgba8.size() ||
                expectedBytes != snapshot.ShadowR32UintLe.size() ||
                expectedPixels != snapshot.DepthValues.size() ||
                (!snapshot.StencilValues.empty() &&
                 expectedPixels != snapshot.StencilValues.size()) ||
                !identities
                     .insert({snapshot.RenderTargetNamespace,
                              snapshot.ColorPhysicalAddress,
                              snapshot.DepthPhysicalAddress,
                              snapshot.FramebufferWidth,
                              snapshot.FramebufferHeight,
                              snapshot.FramebufferColorFormat,
                              snapshot.FramebufferDepthFormat,
                              snapshot.RenderScalePermille})
                     .second) {
                throw std::runtime_error(
                    "native PICA color snapshot is incompatible");
            }
        }
        for (const auto& snapshot : snapshots) {
            GfxNativePicaDrawView targetDescription;
            targetDescription.RenderTargetNamespace =
                snapshot.RenderTargetNamespace;
            targetDescription.FramebufferWidth = snapshot.FramebufferWidth;
            targetDescription.FramebufferHeight = snapshot.FramebufferHeight;
            targetDescription.FramebufferColorPhysicalAddress =
                snapshot.ColorPhysicalAddress;
            targetDescription.FramebufferDepthPhysicalAddress =
                snapshot.DepthPhysicalAddress;
            targetDescription.FramebufferColorFormat =
                snapshot.FramebufferColorFormat;
            targetDescription.FramebufferDepthFormat =
                snapshot.FramebufferDepthFormat;
            auto& target = GetOrCreateNativePicaRenderTarget(
                targetDescription, snapshot.ImageWidth,
                snapshot.ImageHeight);
            if (target.Width != snapshot.ImageWidth ||
                target.Height != snapshot.ImageHeight) {
                throw std::runtime_error(
                    "native PICA color snapshot render extent changed");
            }
            const auto restoreColorAttachment =
                [&](VkImage image, uint32_t bytesPerPixel,
                    std::span<const uint8_t> bytes) {
                    RestoreNativePicaImageBytes(
                        image, snapshot.ImageWidth,
                        snapshot.ImageHeight, bytesPerPixel, bytes,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                };
            // The restored colour becomes this target's current content version.
            // If it was saved as resolved content, the provenance answers true
            // immediately - including the first presentation of a restored
            // checkpoint, before any further native draw - and any later draw,
            // clear or memory fill of this target invalidates it again.
            target.ContentWriteSerial = ++mGpuTextureCopyWriteSerial;
            target.SceneEffectsResolvedSerial =
                snapshot.SceneEffectsResolved ? target.ContentWriteSerial : 0U;
            restoreColorAttachment(
                target.ColorImage, 4U, snapshot.ColorRgba8);
            if (!target.Attachments.NativeColorOnly()) {
                restoreColorAttachment(
                    target.NormalGuideImage, 4U,
                    snapshot.NormalGuideRgba8);
                restoreColorAttachment(
                    target.MaterialGuideImage, 4U,
                    snapshot.MaterialGuideRgba8);
                restoreColorAttachment(
                    target.RigidMotionGuideImage, 8U,
                    snapshot.RigidMotionGuideRgba16FloatLe);
                restoreColorAttachment(
                    target.AmbientGuideImage, 4U,
                    snapshot.AmbientGuideRgba8);
            }
            RestoreNativePicaImageBytes(
                target.ShadowImage, snapshot.ImageWidth,
                snapshot.ImageHeight, 4U, snapshot.ShadowR32UintLe,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            RestoreNativePicaDepthStencilImage(
                target.DepthImage, snapshot.ImageWidth,
                snapshot.ImageHeight, snapshot.DepthValues,
                snapshot.StencilValues);
            target.WBuffering = snapshot.WBuffering;
        }
        return true;
    } catch (const std::exception& exception) {
        SetNativeError(error, exception.what());
        return false;
    }
}

bool GfxRenderingAPIVulkan::CapturePicaPresentationState(
    GfxNativePicaPresentationStateSnapshot& snapshot,
    std::string* error) {
    snapshot = {};
    try {
        if (mDevice == VK_NULL_HANDLE || mFrameActive || mFrameSubmitted) {
            throw std::runtime_error(
                "native PICA presentation capture requires an idle frame boundary");
        }
        WaitForAllPresents();
        CheckNativeVk(vkDeviceWaitIdle(mDevice),
                      "vkDeviceWaitIdle(native PICA presentation capture)");
        for (const auto& [key, image] : mNativePicaDisplayImages) {
            GfxNativePicaDisplayImageColorSnapshot imageSnapshot;
            imageSnapshot.RenderTargetNamespace = key.first;
            imageSnapshot.OutputPhysicalAddress = key.second;
            imageSnapshot.ImageWidth = image.Width;
            imageSnapshot.ImageHeight = image.Height;
            imageSnapshot.Initialized = image.Initialized;
            if (image.Initialized) {
                imageSnapshot.ColorRgba8 = CaptureNativePicaImageBytes(
                    image.Image, image.Width, image.Height, 4U,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT);
            }
            const auto depthAssociation =
                mNativePicaDisplayDepthTargets.find(key);
            if (depthAssociation != mNativePicaDisplayDepthTargets.end()) {
                const auto& target = depthAssociation->second;
                imageSnapshot.HasDepthTarget = true;
                imageSnapshot.DepthTargetNamespace =
                    target.RenderTargetNamespace;
                imageSnapshot.DepthTargetColorPhysicalAddress =
                    target.ColorPhysicalAddress;
                imageSnapshot.DepthTargetDepthPhysicalAddress =
                    target.DepthPhysicalAddress;
                imageSnapshot.DepthTargetFramebufferWidth = target.Width;
                imageSnapshot.DepthTargetFramebufferHeight = target.Height;
                imageSnapshot.DepthTargetFramebufferColorFormat =
                    target.ColorFormat;
                imageSnapshot.DepthTargetFramebufferDepthFormat =
                    target.DepthFormat;
                imageSnapshot.DepthTargetRenderScalePermille =
                    target.RenderScalePermille;
            }
            snapshot.DisplayImages.push_back(std::move(imageSnapshot));
        }
        snapshot.LastPresentedTransfer =
            mLastPresentedNativePicaDisplayTransfer;
        return true;
    } catch (const std::exception& exception) {
        snapshot = {};
        SetNativeError(error, exception.what());
        return false;
    }
}

bool GfxRenderingAPIVulkan::RestorePicaPresentationState(
    const GfxNativePicaPresentationStateSnapshot& snapshot,
    std::string* error) {
    try {
        if (mDevice == VK_NULL_HANDLE || mFrameActive || mFrameSubmitted) {
            throw std::runtime_error(
                "native PICA presentation restore requires an idle frame boundary");
        }
        if (!mNativePicaDisplayImages.empty() ||
            !mNativePicaDisplayDepthTargets.empty() ||
            mLastPresentedNativePicaDisplayTransfer.has_value()) {
            throw std::runtime_error(
                "native PICA presentation restore requires reset renderer state");
        }
        std::set<std::pair<uint64_t, uint32_t>> identities;
        for (const auto& image : snapshot.DisplayImages) {
            const uint64_t expectedBytes =
                static_cast<uint64_t>(image.ImageWidth) *
                image.ImageHeight * 4U;
            if (image.OutputPhysicalAddress == 0U ||
                image.ImageWidth == 0U || image.ImageHeight == 0U ||
                (image.Initialized
                     ? expectedBytes != image.ColorRgba8.size()
                     : !image.ColorRgba8.empty()) ||
                !identities
                     .insert({image.RenderTargetNamespace,
                              image.OutputPhysicalAddress})
                     .second) {
                throw std::runtime_error(
                    "native PICA display image snapshot is incompatible");
            }
            if (image.HasDepthTarget) {
                const NativePicaRenderTargetKey targetKey{
                    image.DepthTargetNamespace,
                    image.DepthTargetColorPhysicalAddress,
                    image.DepthTargetDepthPhysicalAddress,
                    image.DepthTargetFramebufferWidth,
                    image.DepthTargetFramebufferHeight,
                    image.DepthTargetFramebufferColorFormat,
                    image.DepthTargetFramebufferDepthFormat,
                    image.DepthTargetRenderScalePermille};
                if (mNativePicaRenderTargets.find(targetKey) ==
                    mNativePicaRenderTargets.end()) {
                    throw std::runtime_error(
                        "native PICA display depth association is unavailable");
                }
            }
        }
        if (snapshot.LastPresentedTransfer.has_value()) {
            const auto& transfer = *snapshot.LastPresentedTransfer;
            const auto displayKey = std::make_pair(
                transfer.RenderTargetNamespace,
                transfer.OutputPhysicalAddress);
            const auto displaySnapshot = std::find_if(
                snapshot.DisplayImages.begin(),
                snapshot.DisplayImages.end(),
                [&](const auto& image) {
                    return image.RenderTargetNamespace ==
                               displayKey.first &&
                           image.OutputPhysicalAddress ==
                               displayKey.second &&
                           image.Initialized;
                });
            if (transfer.CompletionId == 0U ||
                transfer.InputPhysicalAddress == 0U ||
                transfer.OutputPhysicalAddress == 0U ||
                transfer.InputWidth == 0U || transfer.InputHeight == 0U ||
                transfer.OutputWidth == 0U || transfer.OutputHeight == 0U ||
                static_cast<uint8_t>(transfer.PresentationMode) >
                    static_cast<uint8_t>(
                        GfxNativePicaPresentationMode::AlphaOverlay) ||
                displaySnapshot == snapshot.DisplayImages.end()) {
                throw std::runtime_error(
                    "native PICA last presentation snapshot is incompatible");
            }
        }

        for (const auto& imageSnapshot : snapshot.DisplayImages) {
            auto& image = GetOrCreateNativePicaDisplayImage(
                imageSnapshot.RenderTargetNamespace,
                imageSnapshot.OutputPhysicalAddress,
                imageSnapshot.ImageWidth, imageSnapshot.ImageHeight);
            if (imageSnapshot.Initialized) {
                RestoreNativePicaImageBytes(
                    image.Image, image.Width, image.Height, 4U,
                    imageSnapshot.ColorRgba8, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0U,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT);
            }
            image.Initialized = imageSnapshot.Initialized;
            if (imageSnapshot.HasDepthTarget) {
                mNativePicaDisplayDepthTargets[{
                    imageSnapshot.RenderTargetNamespace,
                    imageSnapshot.OutputPhysicalAddress}] = {
                    imageSnapshot.DepthTargetNamespace,
                    imageSnapshot.DepthTargetColorPhysicalAddress,
                    imageSnapshot.DepthTargetDepthPhysicalAddress,
                    imageSnapshot.DepthTargetFramebufferWidth,
                    imageSnapshot.DepthTargetFramebufferHeight,
                    imageSnapshot.DepthTargetFramebufferColorFormat,
                    imageSnapshot.DepthTargetFramebufferDepthFormat,
                    imageSnapshot.DepthTargetRenderScalePermille};
            }
        }
        mLastPresentedNativePicaDisplayTransfer =
            snapshot.LastPresentedTransfer;
        return true;
    } catch (const std::exception& exception) {
        for (auto& [key, image] : mNativePicaDisplayImages) {
            DestroyNativePicaDisplayImage(image);
        }
        mNativePicaDisplayImages.clear();
        DestroyNativePicaSceneBeforeUiImages();
        mNativePicaDisplayDepthTargets.clear();
        mLastPresentedNativePicaDisplayTransfer.reset();
        SetNativeError(error, exception.what());
        return false;
    }
}

bool GfxRenderingAPIVulkan::QueuePicaCompletion(
    uint64_t completionId, std::string* error) {
    if (!mFrameActive || completionId == 0U) {
        SetNativeError(
            error,
            "native PICA completion queued outside an active Vulkan frame");
        return false;
    }
    mFrameResources[mCurrentFrame].NativePicaCompletionIds.push_back(
        completionId);
    return true;
}

std::vector<uint64_t>
GfxRenderingAPIVulkan::TakePicaCompletions() {
    auto completions = std::move(mCompletedNativePicaIds);
    mCompletedNativePicaIds.clear();
    return completions;
}

bool GfxRenderingAPIVulkan::ResetPicaState(
    std::string* error) {
    try {
        EndNativePicaRenderPass();
        CheckNativeVk(vkDeviceWaitIdle(mDevice),
                      "vkDeviceWaitIdle(native PICA state reset)");
        mBrixelizerScene.Reset();
        DestroyNativePicaGeometryResources();
            ForgetNativePicaEffectNriTextures();
        mAmbientOcclusionPass.InvalidateScreenResources();
        mCacaoPass.InvalidateScreenResources();
        mMxaoPass.InvalidateScreenResources();
        mFidelityFxSssrPass.InvalidateScreenResources();
        mBrixelizerGiPass.InvalidateScreenResources();
        mReflectionMaterialResolvePass.InvalidateScreenResources();
        mReflectionIblPass.InvalidateProfile();
        mLinearSceneColorPass.InvalidateScreenResources();
        mHiZDepthPyramidPass.InvalidateScreenResources();
        if (mHiZReflectionSurface.has_value()) {
            mSceneSurfaces.Retire(*mHiZReflectionSurface);
            mHiZReflectionSurface.reset();
        }
        mHiZReflectionPass.InvalidateScreenResources();
        if (mMotionSurface.has_value()) {
            mSceneSurfaces.Retire(*mMotionSurface);
            mMotionSurface.reset();
        }
        mMotionVectorPass.InvalidateScreenResources();
        if (mLinearColorSurface.has_value()) {
            mSceneSurfaces.Retire(*mLinearColorSurface);
            mLinearColorSurface.reset();
        }
        mNriUpscalerPass.InvalidateScreenResources();
        mUpscalerGuidePass.InvalidateScreenResources();
        mNativeUiOverlayPass.InvalidateScreenResources();
        if (mCompositeSurface.has_value()) {
            mSceneSurfaces.Retire(*mCompositeSurface);
            mCompositeSurface.reset();
        }
        mSceneCompositePass.InvalidateScreenResources();
        mSmaa1xPass.InvalidateScreenResources();
        mNriEffectGraphTransientImageArena.InvalidateScreenResources();
        if (mTaaSurface.has_value()) {
            mSceneSurfaces.Retire(*mTaaSurface);
            mTaaSurface.reset();
        }
        mTemporalAaPass.InvalidateScreenResources();
        mTaaOutputValid = false;
        mHiZOutputValid = false;
        mAmbientOcclusionOutputValid = false;
        mInteractiveGrassPass.ResetTemporalState();
        Oot3d::GrassSceneBridge::Instance().Clear();
        mTemporalHistory.Reset();
        mRigidMotionTracker.Reset();
        mPreviousPicaVertexUniforms.clear();
        mNativePicaCompositionSchedule.Reset();
        for (auto texture = mNativePicaTextures.begin();
             texture != mNativePicaTextures.end();) {
            if (!texture->second.ImageBorrowed) {
                ++texture;
                continue;
            }
            DestroyTexture(texture->second);
            texture = mNativePicaTextures.erase(texture);
        }
        DestroyGpuTextureCopySurfaces();
        for (auto& [key, target] : mNativePicaRenderTargets) {
            DestroyNativePicaRenderTarget(target);
        }
        mNativePicaRenderTargets.clear();
        mNativePicaDisplayDepthTargets.clear();
        for (auto& [key, image] : mNativePicaDisplayImages) {
            DestroyNativePicaDisplayImage(image);
        }
        mNativePicaDisplayImages.clear();
        DestroyNativePicaSceneBeforeUiImages();
        mInvalidatedNativePicaRenderTargetAddresses.clear();
        mLastPresentedNativePicaDisplayTransfer.reset();
        mPendingNativePicaMemoryFills.clear();
        mPicaMemoryFillSmokeInjected = false;
        mCompletedNativePicaIds.clear();
        for (auto& frame : mFrameResources) {
            frame.NativePicaCompletionIds.clear();
        }
        mActiveNativePicaRenderTarget = nullptr;
        mNativePicaRenderPassActive = false;
        mNativePicaPresentedThisFrame = false;
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("native PICA state reset failed: ") +
                     exception.what();
        }
        return false;
    }
}

bool GfxRenderingAPIVulkan::ResetTitleState(std::string* error) {
    try {
        Oot3d::GrassSceneBridge::Instance().Clear();
        Oot3d::GrassInteractionBridge::Instance().Reset();
        Oot3d::VisualClock::Instance().Reset();
        Oot3d::SceneViewRuntime::Instance().Reset();
        mNativeSceneView.Reset();
        mPreparedShadowCasterView.Reset();
        mPicaScenePublications.Reset();
        return true;
    } catch (const std::exception& exception) {
        SetNativeError(error, exception.what());
        return false;
    }
}

const GfxNativePicaVertexBindingView* FindNativeBinding(
    const GfxNativePicaDrawView& draw, uint8_t binding) {
    const auto found = std::find_if(
        draw.VertexBindings.begin(), draw.VertexBindings.end(),
        [binding](const auto& value) { return value.Binding == binding; });
    return found == draw.VertexBindings.end() ? nullptr : &*found;
}

const GfxNativePicaVertexAttributeView* FindNativeAttribute(
    const GfxNativePicaDrawView& draw, uint8_t location) {
    const auto found = std::find_if(
        draw.VertexAttributes.begin(), draw.VertexAttributes.end(),
        [location](const auto& value) { return value.Location == location; });
    return found == draw.VertexAttributes.end() ? nullptr : &*found;
}

bool PublishNativePicaGrassSurface(
    const GfxNativePicaDrawView& draw, uint64_t frameId,
    std::unordered_map<uint64_t, uint32_t>& occurrences,
    const Oot3d::InteractiveGrassSettings& settings,
    bool worldSurface, bool fragmentUsesTexture0,
    bool perspectiveProjection,
    Oot3d::GrassGeometryRegistry& geometryRegistry) {
    if (settings.Rules.empty()) {
        return false;
    }
    const auto texture = std::find_if(
        draw.Textures.begin(), draw.Textures.end(),
        [](const auto& value) { return value.Slot == 0U; });
    const Oot3d::GrassSurfaceDrawInfo eligibility{
        draw.FragmentOperationMode == 0U,
        worldSurface,
        texture != draw.Textures.end(),
        fragmentUsesTexture0,
        draw.PositionAttributeLocation != 0xff,
        draw.TexCoord0AttributeLocation != 0xff,
        draw.DepthTestEnabled,
        draw.DepthWriteEnabled,
        draw.DepthCompare ==
            GfxNativePicaCompareFunction::Always,
        perspectiveProjection,
        (draw.ColorWriteMask & 0x7U) != 0U,
        UsesTranslucentBlend(draw.Blend),
        Oot3d::IsGrassPrimitiveTopologySupported(
            draw.Topology),
    };
    const bool eligible =
        Oot3d::IsGrassSurfaceDrawEligible(eligibility);
    static const bool diagnostics =
        std::getenv("OOT3D_GRASS_DIAGNOSTICS") != nullptr;
    if (!eligible && !diagnostics) {
        return false;
    }
    const auto textureHash =
        ResolveNativeTextureBaseLevelContentHash(*texture);
    if (!textureHash.has_value()) {
        return false;
    }
    const uint64_t observedTextureHash =
        Oot3d::GrassTextureSourceCache::Instance()
            .ResolveObservedHash(
                *textureHash, texture->Width,
                texture->Height);
    const bool configuredSource = std::any_of(
        settings.Rules.begin(), settings.Rules.end(),
        [&](const auto& rule) {
            return rule.Target.Rgba8Hash ==
                       observedTextureHash &&
                (rule.Target.MapperSlotMask & 1U) != 0U &&
                (rule.Target.Width == 0U ||
                 rule.Target.Width == texture->Width) &&
                (rule.Target.Height == 0U ||
                 rule.Target.Height == texture->Height);
        });
    if (diagnostics) {
        static uint32_t sourceDrawReports = 0U;
        if (sourceDrawReports++ < 128U) {
            std::fprintf(
                stderr,
                "OOT3D grass source draw: hash=%016llx observed=%016llx slot=%u size=%ux%u configured=%d eligible=%d "
                "op=%d world=%d tex0=%d sample0=%d "
                "pos=%d uv=%d depth=%d/%d cmpalways=%d perspective=%d rgb=%d translucent=%d topology=%d\n",
                static_cast<unsigned long long>(*textureHash),
                static_cast<unsigned long long>(observedTextureHash),
                texture->Slot, texture->Width, texture->Height,
                configuredSource ? 1 : 0, eligible ? 1 : 0,
                eligibility.ColorOperation ? 1 : 0,
                eligibility.WorldSurface ? 1 : 0,
                eligibility.HasTexture0 ? 1 : 0,
                eligibility.FragmentUsesTexture0 ? 1 : 0,
                eligibility.HasPosition ? 1 : 0,
                eligibility.HasTexCoord0 ? 1 : 0,
                eligibility.DepthTest ? 1 : 0,
                eligibility.DepthWrite ? 1 : 0,
                eligibility.DepthCompareAlways ? 1 : 0,
                eligibility.PerspectiveProjection ? 1 : 0,
                eligibility.WritesRgb ? 1 : 0,
                eligibility.TranslucentBlend ? 1 : 0,
                static_cast<int>(draw.Topology));
        }
    }
    if (!eligible || !configuredSource) {
        return false;
    }
    const auto rejectConfiguredSource = [&](const char* reason) {
        if (diagnostics) {
            static uint32_t rejectionReports = 0U;
            if (rejectionReports++ < 64U) {
                std::fprintf(
                    stderr,
                    "OOT3D grass configured source rejected: hash=%016llx reason=%s\n",
                    static_cast<unsigned long long>(*textureHash), reason);
            }
        }
        return false;
    };
    const auto materialWrapS =
        DecodeNativePicaGrassWrap(texture->NativeWrapS);
    const auto materialWrapT =
        DecodeNativePicaGrassWrap(texture->NativeWrapT);
    if (!materialWrapS.has_value() ||
        !materialWrapT.has_value()) {
        return rejectConfiguredSource("unsupported texture wrap");
    }
    const auto* position = FindNativeAttribute(draw, draw.PositionAttributeLocation);
    const auto* uv = FindNativeAttribute(draw, draw.TexCoord0AttributeLocation);
    if (position == nullptr || uv == nullptr || position->ComponentCount < 3U ||
        uv->ComponentCount < 2U) {
        return rejectConfiguredSource("invalid vertex attributes");
    }
    const auto* positionBinding = FindNativeBinding(draw, position->Binding);
    const auto* uvBinding = FindNativeBinding(draw, uv->Binding);
    if (positionBinding == nullptr || uvBinding == nullptr ||
        positionBinding->PerInstance || uvBinding->PerInstance) {
        return rejectConfiguredSource("invalid vertex bindings");
    }
    const auto textureCoordinateTransform =
        Oot3d::DecodePicaGrassTextureCoordinateTransform(
            draw.VertexShaderSource,
            draw.FragmentShaderSource,
            draw.VertexUniformBytes,
            draw.TexCoord0AttributeLocation,
            &draw.TemporalVertexProgram.Hooks,
            &draw.FragmentShaderHooks);
    if (!textureCoordinateTransform.Applied()) {
        return rejectConfiguredSource(
            Oot3d::PicaGrassTextureCoordinateEligibilityName(
                textureCoordinateTransform.Eligibility));
    }

    // OoT3D's common rigid CMB vertex program transforms attribute 0 through
    // f[20..22], then f[4..6], and finally projection f[0..3]. Preserve that
    // exact model-to-clip path for rendering. A separately reconstructed
    // model-to-world anchor is used only for placement and interaction.
    // Skinned draws select a matrix palette with vertex attributes and need a
    // separate producer; they are intentionally excluded here.
    constexpr size_t kVertexBooleanMaskOffset = 0U;
    constexpr size_t kVertexFloatUniformOffset =
        16U + 4U * 4U * sizeof(uint32_t);
    constexpr size_t kProjectionUniformFirst = 0U;
    constexpr size_t kProjectionUniformRows = 4U;
    constexpr size_t kViewUniformFirst = 4U;
    constexpr size_t kViewUniformRows = 3U;
    constexpr size_t kModelUniformFirst = 20U;
    constexpr size_t kModelUniformRows = 3U;
    constexpr size_t kVec4Bytes = 4U * sizeof(float);
    const auto uniformOffset = [](size_t first) {
        return kVertexFloatUniformOffset + first * kVec4Bytes;
    };
    const size_t modelOffset = uniformOffset(kModelUniformFirst);
    if (draw.VertexUniformBytes.size() <
        modelOffset + kModelUniformRows * kVec4Bytes) {
        return rejectConfiguredSource("vertex uniforms unavailable");
    }
    uint32_t booleanMask = 0;
    std::memcpy(&booleanMask,
                draw.VertexUniformBytes.data() + kVertexBooleanMaskOffset,
                sizeof(booleanMask));
    if ((booleanMask & 4U) != 0U) {
        return rejectConfiguredSource("skinned geometry unsupported");
    }

    const auto identity = [] {
        return std::array<float, 16>{
            1.0F, 0.0F, 0.0F, 0.0F,
            0.0F, 1.0F, 0.0F, 0.0F,
            0.0F, 0.0F, 1.0F, 0.0F,
            0.0F, 0.0F, 0.0F, 1.0F};
    };
    const auto multiply = [](const std::array<float, 16>& left,
                             const std::array<float, 16>& right) {
        std::array<float, 16> result{};
        for (size_t row = 0; row < 4U; ++row) {
            for (size_t column = 0; column < 4U; ++column) {
                for (size_t inner = 0; inner < 4U; ++inner) {
                    result[row * 4U + column] +=
                        left[row * 4U + inner] *
                        right[inner * 4U + column];
                }
            }
        }
        return result;
    };
    std::array<float, 16> projection{};
    std::array<float, 16> viewTransform = identity();
    std::array<float, 16> modelTransform = identity();
    std::memcpy(projection.data(),
                draw.VertexUniformBytes.data() +
                    uniformOffset(kProjectionUniformFirst),
                kProjectionUniformRows * kVec4Bytes);
    std::memcpy(viewTransform.data(),
                draw.VertexUniformBytes.data() +
                    uniformOffset(kViewUniformFirst),
                kViewUniformRows * kVec4Bytes);
    std::memcpy(modelTransform.data(),
                draw.VertexUniformBytes.data() + modelOffset,
                kModelUniformRows * kVec4Bytes);
    const auto finiteMatrix = [](const auto& matrix) {
        return std::all_of(matrix.begin(), matrix.end(),
                           [](float value) { return std::isfinite(value); });
    };
    if (!finiteMatrix(projection) || !finiteMatrix(viewTransform) ||
        !finiteMatrix(modelTransform)) {
        return rejectConfiguredSource("non-finite transform");
    }
    const auto modelToView = multiply(viewTransform, modelTransform);

    Oot3d::DirectionalShadowMatrix capturedViewToWorld;
    if (!draw.LightBinding.WorldToView ||
        !Oot3d::InvertDirectionalShadowMatrix(*draw.LightBinding.WorldToView, capturedViewToWorld)) {
        return rejectConfiguredSource("captured camera unavailable");
    }
    // Placement and lighting must undo the camera paired with these exact
    // vertex uniforms, not the later live gameplay camera (or an Eye/At
    // reconstruction that loses roll).
    std::array<float, 16> viewToWorld;
    for (size_t row = 0; row < 4; ++row)
        for (size_t column = 0; column < 4; ++column)
            viewToWorld[row * 4 + column] = capturedViewToWorld[column * 4 + row];
    const auto modelToWorld = multiply(viewToWorld, modelToView);
    auto picaModelToClip = multiply(projection, modelToView);
    if (draw.FramebufferFlipped) {
        for (size_t column = 0; column < 4U; ++column) {
            picaModelToClip[1U * 4U + column] *= -1.0F;
        }
    }
    const float determinant =
        modelToWorld[0] * (modelToWorld[5] * modelToWorld[10] -
                           modelToWorld[6] * modelToWorld[9]) -
        modelToWorld[1] * (modelToWorld[4] * modelToWorld[10] -
                           modelToWorld[6] * modelToWorld[8]) +
        modelToWorld[2] * (modelToWorld[4] * modelToWorld[9] -
                           modelToWorld[5] * modelToWorld[8]);
    if (std::abs(determinant) < 1.0e-12F) {
        return rejectConfiguredSource("degenerate model transform");
    }

    const auto grassGeometryResolution =
        geometryRegistry.Resolve({
            draw.GeometryIdentity,
            draw.GeometryContentVersion,
            draw.GeometryIdentityAvailable,
            frameId,
            {
                positionBinding->Bytes,
                positionBinding->ByteStride,
                position->Format,
                position->ComponentCount,
                position->ByteOffset,
            },
            {
                uvBinding->Bytes,
                uvBinding->ByteStride,
                uv->Format,
                uv->ComponentCount,
                uv->ByteOffset,
            },
            draw.IndexBytes,
            draw.Indexed,
            draw.IndicesAre16Bit,
            draw.BaseVertex,
            draw.VertexCount,
            draw.Topology,
            textureCoordinateTransform,
        });
    if (grassGeometryResolution.Geometry == nullptr) {
        return rejectConfiguredSource("geometry extraction failed");
    }
    const auto& grassGeometry =
        *grassGeometryResolution.Geometry;
    Oot3d::GrassSceneMesh mesh;
    mesh.SubmissionId = draw.SubmissionId;
    mesh.GeometryId = grassGeometry.GeometryId;
    mesh.AnchorVersion = grassGeometry.AnchorVersion;
    mesh.ContentVersion = grassGeometry.ContentVersion;
    if (std::getenv("OOT3D_GRASS_DIAGNOSTICS") != nullptr) {
        static uint32_t coordinateReports = 0U;
        if (coordinateReports < 48U) {
            ++coordinateReports;
            const auto& transform =
                textureCoordinateTransform.RawToSample;
            std::fprintf(
                stderr,
                "OOT3D grass UV: frame=%llu geometry=%016llx "
                "texture=%016llx version=%016llx "
                "transform=[%.9g %.9g %.9g %.9g %.9g %.9g]\n",
                static_cast<unsigned long long>(frameId),
                static_cast<unsigned long long>(mesh.GeometryId),
                static_cast<unsigned long long>(*textureHash),
                static_cast<unsigned long long>(
                    mesh.ContentVersion),
                transform[0], transform[1], transform[2],
                transform[3], transform[4], transform[5]);
        }
    }
    mesh.TextureHash = *textureHash;
    uint64_t drawIdentity = mesh.GeometryId ^
        (mesh.TextureHash + 0x9e3779b97f4a7c15ULL + (mesh.GeometryId << 6U) +
         (mesh.GeometryId >> 2U));
    const uint32_t occurrence = occurrences[drawIdentity]++;
    mesh.InstanceId = drawIdentity ^
        (static_cast<uint64_t>(occurrence) + 0x9e3779b97f4a7c15ULL +
         (drawIdentity << 6U) + (drawIdentity >> 2U));
    mesh.InstanceId ^= draw.RenderTargetNamespace + 0x9e3779b97f4a7c15ULL +
                       (mesh.InstanceId << 6U) + (mesh.InstanceId >> 2U);
    mesh.InstanceId ^=
        static_cast<uint64_t>(
            draw.FramebufferColorPhysicalAddress) +
        0x9e3779b97f4a7c15ULL +
        (mesh.InstanceId << 6U) +
        (mesh.InstanceId >> 2U);
    if (mesh.InstanceId == 0U) mesh.InstanceId = 1U;
    mesh.RenderTargetNamespace =
        draw.RenderTargetNamespace;
    mesh.FramebufferColorPhysicalAddress =
        draw.FramebufferColorPhysicalAddress;
    mesh.FrameId = frameId;
    mesh.TextureWidth = texture->Width;
    mesh.TextureHeight = texture->Height;
    mesh.MapperSlot = 0;
    mesh.MaterialWrapS = *materialWrapS;
    mesh.MaterialWrapT = *materialWrapT;
    mesh.TransformBakedIntoVertices = false;
    mesh.PreserveWorldAnchor = true;
    mesh.ModelToWorld = modelToWorld;
    // CPU matrices above are row-major. GLSL consumes the push-constant mat4
    // as column-major, so transpose while storing the exact PICA transform.
    for (size_t row = 0; row < 4U; ++row) {
        for (size_t column = 0; column < 4U; ++column) {
            mesh.PicaModelToClip[column * 4U + row] =
                picaModelToClip[row * 4U + column];
        }
    }
    mesh.PicaModelToClipAvailable = true;
    mesh.Shading = Oot3d::DecodeGrassShadingEnvironment(
        draw.VertexUniformBytes, draw.FragmentUniformBytes,
        draw.FragmentFeatures, capturedViewToWorld);
    mesh.Vertices = grassGeometry.Vertices;
    mesh.Indices = grassGeometry.Indices;
    Oot3d::GrassSceneBridge::Instance().Publish(
        std::move(mesh));
    return true;
}

void GfxRenderingAPIVulkan::ForgetNativePicaEffectNriTextures() {
    for (VkImage image : {
            mMotionVectorPass.OutputImage(),
            mMotionVectorPass.ReactiveImage()}) {
        if (image != VK_NULL_HANDLE) mNriInterop.ForgetTexture(image);
    }
    // Temporal AA histories are committed NRI textures and are destroyed by
    // the pass through the owned-resource path.
}

void GfxRenderingAPIVulkan::ReleaseEffectGraphImageClients() {
    mAmbientOcclusionPass.InvalidateScreenResources();
    mCacaoPass.InvalidateScreenResources();
    mMxaoPass.InvalidateScreenResources();
    mHiZDepthPyramidPass.InvalidateScreenResources();
    mFidelityFxSssrPass.InvalidateScreenResources();
    mBrixelizerGiPass.InvalidateScreenResources();
    mReflectionMaterialResolvePass.InvalidateScreenResources();
    mLinearSceneColorPass.InvalidateScreenResources();
    if (mHiZReflectionSurface.has_value()) {
        mSceneSurfaces.Retire(*mHiZReflectionSurface);
        mHiZReflectionSurface.reset();
    }
    mHiZReflectionPass.InvalidateScreenResources();
    if (mMotionSurface.has_value()) {
        mSceneSurfaces.Retire(*mMotionSurface);
        mMotionSurface.reset();
    }
    mMotionVectorPass.InvalidateScreenResources();
    if (mLinearColorSurface.has_value()) {
        mSceneSurfaces.Retire(*mLinearColorSurface);
        mLinearColorSurface.reset();
    }
    mNriUpscalerPass.InvalidateScreenResources();
    mUpscalerGuidePass.InvalidateScreenResources();
    mNativeUiOverlayPass.InvalidateScreenResources();
    if (mCompositeSurface.has_value()) {
        mSceneSurfaces.Retire(*mCompositeSurface);
        mCompositeSurface.reset();
    }
    mSceneCompositePass.InvalidateScreenResources();
    mSmaa1xPass.InvalidateScreenResources();
    if (mTaaSurface.has_value()) {
        mSceneSurfaces.Retire(*mTaaSurface);
        mTaaSurface.reset();
    }
    mTemporalAaPass.InvalidateScreenResources();
    mTaaOutputValid = false;
    mAmbientOcclusionOutputValid = false;
    mHiZOutputValid = false;
    mTemporalHistory.Reset();
    mRigidMotionTracker.Reset();
}

void GfxRenderingAPIVulkan::ApplyInternalResolutionScale(float scale) {
    ResetNativePicaRenderTargets(true);
    mInternalResolutionScale = scale;
}

void GfxRenderingAPIVulkan::ResetNativePicaRenderTargets(bool preserveDisplayImages) {
    EndNativePicaRenderPass();
    WaitForAllPresents();
    CheckNativeVk(vkDeviceWaitIdle(mDevice),
                  "vkDeviceWaitIdle(PICA render target reconfiguration)");
    ForgetNativePicaEffectNriTextures();
    mAmbientOcclusionPass.InvalidateScreenResources();
    mCacaoPass.InvalidateScreenResources();
    mMxaoPass.InvalidateScreenResources();
    mFidelityFxSssrPass.InvalidateScreenResources();
    mBrixelizerGiPass.InvalidateScreenResources();
    mReflectionMaterialResolvePass.InvalidateScreenResources();
    mLinearSceneColorPass.InvalidateScreenResources();
    mHiZDepthPyramidPass.InvalidateScreenResources();
    if (mHiZReflectionSurface.has_value()) {
        mSceneSurfaces.Retire(*mHiZReflectionSurface);
        mHiZReflectionSurface.reset();
    }
    mHiZReflectionPass.InvalidateScreenResources();
    if (mMotionSurface.has_value()) {
        mSceneSurfaces.Retire(*mMotionSurface);
        mMotionSurface.reset();
    }
    mMotionVectorPass.InvalidateScreenResources();
    if (mLinearColorSurface.has_value()) {
        mSceneSurfaces.Retire(*mLinearColorSurface);
        mLinearColorSurface.reset();
    }
    mNriUpscalerPass.InvalidateScreenResources();
    mUpscalerGuidePass.InvalidateScreenResources();
    mNativeUiOverlayPass.InvalidateScreenResources();
    if (mCompositeSurface.has_value()) {
        mSceneSurfaces.Retire(*mCompositeSurface);
        mCompositeSurface.reset();
    }
    mSceneCompositePass.InvalidateScreenResources();
    mSmaa1xPass.InvalidateScreenResources();
    mNriEffectGraphTransientImageArena.InvalidateScreenResources();
    if (mTaaSurface.has_value()) {
        mSceneSurfaces.Retire(*mTaaSurface);
        mTaaSurface.reset();
    }
    mTemporalAaPass.InvalidateScreenResources();
    mTaaOutputValid = false;
    mHiZOutputValid = false;
    mAmbientOcclusionOutputValid = false;
    mTemporalHistory.Reset();
    mRigidMotionTracker.Reset();
    mPreviousPicaVertexUniforms.clear();
    for (auto texture = mNativePicaTextures.begin();
         texture != mNativePicaTextures.end();) {
        if (!texture->second.ImageBorrowed) {
            ++texture;
            continue;
        }
        DestroyTexture(texture->second);
        texture = mNativePicaTextures.erase(texture);
    }
    DestroyGpuTextureCopySurfaces();
    for (auto& [key, target] : mNativePicaRenderTargets) {
        DestroyNativePicaRenderTarget(target);
    }
    mNativePicaRenderTargets.clear();
    mNativePicaDisplayDepthTargets.clear();
    // The scenes before UI belong to the targets' frames and are retired with
    // them, whatever happens to the presented pixels.
    DestroyNativePicaSceneBeforeUiImages();
    if (!preserveDisplayImages) {
        for (auto& [key, image] : mNativePicaDisplayImages) {
            DestroyNativePicaDisplayImage(image);
        }
        mNativePicaDisplayImages.clear();
        mLastPresentedNativePicaDisplayTransfer.reset();
    } else {
        // Retained pixels remain presentable, but their old scene/depth guides
        // no longer exist. The next native transfer republishes that capability;
        // until then the graph must not declare reads from retired attachments,
        // and a resolved-composition provenance from an earlier frame must not
        // suppress the scene stages for these pixels.
        for (auto& [key, image] : mNativePicaDisplayImages) {
            image.SceneResolved = false;
            image.SceneEffectsResolved = false;
        }
    }
    mInvalidatedNativePicaRenderTargetAddresses.clear();
    mActiveNativePicaRenderTarget = nullptr;
    mNativePicaRenderPassActive = false;
    mNativePicaPresentedThisFrame = false;
}

} // namespace Fast

#endif
