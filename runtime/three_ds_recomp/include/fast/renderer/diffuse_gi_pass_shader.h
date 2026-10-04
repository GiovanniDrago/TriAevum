#pragma once

// Compute kernels of the M5 runtime pass of the diffuse screen-space GI
// (docs/TRIAEVUM_SCREEN_SPACE_GI.md, "M5"). The kernels are the M2-M4 GLSL
// twins unchanged (normal pass, hierarchy level 0 and reduction, M4 gather,
// geometry pack, temporal accumulation, a-trous level) plus one own output
// kernel that writes the composition outputs. They are precompiled into
// builtin_pass.spvpack by build_pass_artifacts (the catalogue in
// fast/oot3d/builtin_pass_shaders.cpp appends BuildDiffuseGiPassKernels());
// the pass resolves each kernel by its exact source text, never by name.
//
// Output kernel, set 0: 0 uniform OutputShaderParams; 1 vec4 colour[] (J.rgb,
// A raw per viewport pixel: the last a-trous level, or the temporal colour
// without a filter); 2 vec4 geometry[] (receivers: normal.xyz, view depth);
// 3 rgba16f image (J.rgb, receiver flag); 4 r32f image (A clamped to [0, 1]).
// The images cover the whole buffer extent: texels outside the viewport, and
// every texel in the neutral mode, take J = 0, receiver 0, A = 1. Workgroups
// 8 x 8 over the buffer extent; no barriers, so out-of-range invocations
// return at once.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Fast::Renderer::DiffuseGi {

enum class PassKernel : uint8_t {
    Normal,          // M2 geometric normal (renderer face normal or depth reconstruction)
    HierarchyLevel0, // M4 hierarchy level 0 with the radiance cache
    HierarchyReduce, // M4 hierarchy reduction, one dispatch per level
    Gather,          // M4 gather (metric radial, footprint strips, cache)
    Pack,            // M3 geometry pack (receivers for the temporal kernels)
    Temporal,        // M3 stratum reconstruction and temporal accumulation
    Filter,          // M3 a-trous level, one dispatch per level
    Output,          // M5 composition outputs
};
inline constexpr uint32_t kPassKernelCount = 8;

struct PassKernelSource {
    PassKernel Kernel = PassKernel::Normal;
    const char* Name = ""; // pack name of the module (diagnostics only)
    std::string Source;    // exact GLSL text: the lookup key of the precompiled module
};

// The pass kernels in PassKernel order.
[[nodiscard]] std::vector<PassKernelSource> BuildDiffuseGiPassKernels();

inline constexpr uint32_t kOutputModeFinal = 0U;
inline constexpr uint32_t kOutputModeNeutral = 1U;
// Largest finite FP16 value: J is limited to it before the RGBA16F store so
// an HDR signal never becomes infinite (a storage bound, not an artistic clamp).
inline constexpr float kOutputHalfMax = 65504.0F;

// std140 uniform block of the output kernel, 48 bytes.
struct OutputShaderParams {
    std::array<uint32_t, 4> Extent{};   // buffer width, buffer height, viewport x, viewport y
    std::array<uint32_t, 4> Viewport{}; // viewport width, viewport height, mode, 0
    std::array<float, 4> Limits{};      // largest stored |J|, 0, 0, 0
};
static_assert(sizeof(OutputShaderParams) == 48);

[[nodiscard]] std::string BuildDiffuseGiOutputShader();

// FP32 twin of one output texel (buffer coordinates), in the operation order
// of the kernel. `colour` and `geometry` are viewport-sized vec4 arrays.
struct OutputTexel {
    std::array<float, 4> Illumination{}; // J.rgb, receiver flag (before the FP16 store)
    float Accessibility = 1.0F;
};
[[nodiscard]] OutputTexel OutputTexelF32(const OutputShaderParams& params, uint32_t x, uint32_t y,
                                         const float* colour, const float* geometry) noexcept;

} // namespace Fast::Renderer::DiffuseGi
