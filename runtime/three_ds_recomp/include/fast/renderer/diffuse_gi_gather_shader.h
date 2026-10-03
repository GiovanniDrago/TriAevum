#pragma once

// GLSL compute kernels of the diffuse GI gathering (milestone M2): geometric
// normal reconstruction, gathering with sector masks, and a unit kernel for
// masks and sector weights. They follow GatherImageF32 operation by operation
// (diffuse_gi_gather.h) and are compiled at run time by the GPU tests. Not
// wired into the renderer, the effect graph, settings or F1.
//
// Bindings (set 0): 0 uniform GatherShaderParams; 1 float depth[]; 2 uint
// class[]; 3 vec4 radiance[] (rgb); 4 vec4 normal[] (xyz, w = 1 when valid;
// face normals in, oriented normals out); 5 vec4 result[] per viewport pixel
// (J.rgb, A raw); 6 vec4 aux[] per viewport pixel (C, valid, scheduled,
// occupied sectors). Buffers 1-4 cover the whole buffer extent; only the
// declared viewport is read. Workgroups are 8 x 8; the kernels have no
// barriers, so out-of-range invocations return immediately.

#include "fast/renderer/diffuse_gi_gather.h"

#include <array>
#include <cstdint>
#include <string>

namespace Fast::Renderer::DiffuseGi {

inline constexpr uint32_t kShaderFlagInterleave = 1U;
inline constexpr uint32_t kShaderFlagRejectSelf = 2U;
inline constexpr uint32_t kShaderFlagFaceNormals = 4U;

// std140 uniform block, 128 bytes.
struct GatherShaderParams {
    std::array<uint32_t, 4> BufferViewport{}; // buffer width, buffer height, viewport x, viewport y
    std::array<uint32_t, 4> ExtentCounts{};   // viewport width, viewport height, sections, sectors
    std::array<uint32_t, 4> Modes{};          // depth encoding, schedule, candidates per side, flags
    std::array<float, 4> ViewX{};             // ax, bx, cx, 0
    std::array<float, 4> ViewY{};             // ay, by, cy, 0
    std::array<float, 4> Depth{};             // near, far, infinite far (1/0), 0
    std::array<float, 4> Gather{};            // radius, thickness, rotation, chord epsilon
    std::array<float, 4> Radial{};            // exponent, jitter, self-intersection sine, depth
};
static_assert(sizeof(GatherShaderParams) == 128);

[[nodiscard]] GatherShaderParams MakeGatherShaderParams(const GatherInput& input, const GatherSettings& settings);

[[nodiscard]] std::string BuildDiffuseGiNormalShader();
[[nodiscard]] std::string BuildDiffuseGiGatherShader();
// Unit kernel: binding 0 uniform uvec4 (count, sectors, mode, 0); binding 1
// vec4 inputs[]; binding 2 uvec4 outputs[2 * count] (mode 0: centre-rule mask
// of [x, y) in 8 words) or vec4 outputs[count] (mode 1: M1 weight of
// a = x, b = y, [z, w]).
[[nodiscard]] std::string BuildDiffuseGiUnitShader();

} // namespace Fast::Renderer::DiffuseGi
