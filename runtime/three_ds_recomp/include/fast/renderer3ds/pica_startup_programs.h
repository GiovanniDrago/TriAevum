#pragma once

#ifdef ENABLE_RENDERER3DS_VULKAN

#include "fast/renderer3ds/nri_pica_pipeline_bridge.h"
#include "fast/renderer3ds/pica_fragment_artifact.h"
#include "fast/renderer3ds/pica_vertex_artifact.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace Fast::Renderer3ds {

// The startup preparation's program parts: every shader-bearing library part
// an always-available draw links. The renderer creates them at startup and
// Forge's device preparation creates the same parts into the device pipeline
// cache. Both derive them here: one differing parameter is a cache miss.
struct PicaStartupProgramDevice {
    VkFormat DepthFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits Samples = VK_SAMPLE_COUNT_1_BIT;
    // Fragment primitive IDs (PBR seams) exist only with geometry shaders.
    bool GeometryShaders = false;
};

struct PicaStartupProgramPart {
    std::span<const uint32_t> Vertex;
    std::span<const uint32_t> Fragment;
    bool AlphaToCoverage = false;
};

// SPIR-V declares its capabilities first, right after the five-word header.
inline bool DeclaresSpirvCapability(std::span<const uint32_t> spirv, uint32_t capability) {
    constexpr uint32_t kOpCapability = 17U;
    for (size_t at = 5U; at + 1U < spirv.size();) {
        const uint32_t words = spirv[at] >> 16U;
        if (words == 0U || (spirv[at] & 0xffffU) != kOpCapability) break;
        if (spirv[at + 1U] == capability) return true;
        at += words;
    }
    return false;
}

// The title vertex family, the superset effect programs (empty when draws do
// not use them) and the canonical bases, each once, not the vertex/fragment
// product. Vertex parts first (cheap, every link needs one; the first one
// comes with every fragment item), then the superset programs: they take
// longest and are what a first draw waits for.
inline std::vector<PicaStartupProgramPart> PicaStartupProgramParts(
    std::span<const PicaVertexArtifact> vertices,
    std::span<const PicaFragmentArtifact> superset,
    std::span<const PicaFragmentArtifact> canonical,
    const PicaStartupProgramDevice& device) {
    if (vertices.empty()) throw std::runtime_error("native program preparation has no vertex family");
    if (canonical.empty()) throw std::runtime_error("native program preparation has no fragment family");
    std::vector<PicaStartupProgramPart> parts;
    const auto fragmentFamily = [&](std::span<const PicaFragmentArtifact> family) {
        for (const auto& artifact : family) {
            if (!artifact.SeparateSamplers) continue;
            constexpr uint32_t kCapabilityGeometry = 2U;
            if (!device.GeometryShaders && DeclaresSpirvCapability(artifact.Spirv, kCapabilityGeometry)) continue;
            parts.push_back({vertices.front().Spirv, artifact.Spirv, false});
            // Alpha-tested draws cover by alpha once samples exceed one.
            if (device.Samples != VK_SAMPLE_COUNT_1_BIT)
                parts.push_back({vertices.front().Spirv, artifact.Spirv, true});
        }
    };
    for (const auto& vertex : vertices.subspan(1))
        parts.push_back({vertex.Spirv, canonical.front().Spirv, false});
    fragmentFamily(superset);
    const size_t beforeCanonical = parts.size();
    fragmentFamily(canonical);
    if (parts.size() == beforeCanonical)
        throw std::runtime_error("native program preparation has no fragment family");
    return parts;
}

// Creates the library parts of one item through the bridge's live factory. A
// preparation interface, never geometry submitted to a draw.
template <typename Bridge>
bool PreparePicaStartupProgramPart(Bridge& bridge, const PicaStartupProgramPart& part,
                                   const PicaStartupProgramDevice& device) {
    VkVertexInputBindingDescription binding{0, 16U * 16U, VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription, 16> attributes{};
    for (uint32_t i = 0; i < attributes.size(); ++i)
        attributes[i] = {i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16U};
    NriPicaGraphicsPipelineDesc desc;
    desc.VertexBindings = {&binding, 1};
    desc.VertexAttributes = attributes;
    desc.VertexSpirv = part.Vertex;
    desc.FragmentSpirv = part.Fragment;
    desc.ColorAttachmentCount = 1;
    desc.ColorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    desc.Colors[0].colorWriteMask = 15;
    desc.DepthStencilFormat = device.DepthFormat;
    desc.Samples = device.Samples;
    desc.AlphaToCoverage = part.AlphaToCoverage;
    return bridge.PreparePipeline(desc, NriPicaPipelineFactory::Libraries);
}

} // namespace Fast::Renderer3ds

#endif
