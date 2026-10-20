#pragma once

#ifdef ENABLE_OOT3D_VULKAN

#include "fast/renderer/shaderc_compiler.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>

namespace Fast::Oot3d {

// A pass program from the cached compiler (the built-in pack, or a compile),
// with one optional define. Throws std::runtime_error.
[[nodiscard]] VkShaderModule CompileGrassShaderModule(Renderer::CachedPassShaderCompiler& shaders, VkDevice device,
                                                      const char* source, Renderer::SpirvStage stage,
                                                      const char* name, const char* defineName = nullptr,
                                                      const char* defineValue = nullptr);

// Where the grass draws render: the native PICA targets (canonical colour
// only, instrumented guides, instrumented with the GI albedo guide), under a
// render pass or dynamic rendering.
struct GrassPipelineTargets {
    VkRenderPass CanonicalRenderPass = VK_NULL_HANDLE;
    VkRenderPass InstrumentedRenderPass = VK_NULL_HANDLE;
    VkSampleCountFlagBits SampleCount = VK_SAMPLE_COUNT_1_BIT;
    bool DynamicRendering = false;
    VkFormat DepthFormat = VK_FORMAT_UNDEFINED;
    VkPipelineCache PipelineCache = VK_NULL_HANDLE;
};

// [0..2] explicit PICA depth, [3..5] rasterized depth, each canonical,
// instrumented, instrumented with the albedo guide (dynamic rendering only;
// VK_NULL_HANDLE otherwise).
using GrassPipelineSet = std::array<VkPipeline, 6>;

// Fragments: canonical, instrumented, canonical raster depth, instrumented
// raster depth. No vertex input (the shaders read storage buffers), triangle
// list, no culling, depth test and write (less or equal), the grass blend and
// write masks of every attachment. Throws std::runtime_error on failure
// after destroying what it created.
void CreateGrassPipelineSet(VkDevice device, const GrassPipelineTargets& targets, VkPipelineLayout layout,
                            VkShaderModule vertex, const std::array<VkShaderModule, 4>& fragments,
                            GrassPipelineSet& pipelines);

void DestroyGrassPipelineSet(VkDevice device, GrassPipelineSet& pipelines) noexcept;

} // namespace Fast::Oot3d

#endif
