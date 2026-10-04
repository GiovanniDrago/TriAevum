#include "fast/renderer/diffuse_gi_pass_shader.h"

// Kernel catalogue of the M5 runtime pass and its own output kernel. The
// M2-M4 kernels are referenced unchanged; the output kernel and its FP32 twin
// are own code (no third-party shader was consulted).

#include "fast/renderer/diffuse_gi_gather_shader.h"
#include "fast/renderer/diffuse_gi_m4_shader.h"
#include "fast/renderer/diffuse_gi_temporal_shader.h"

#include <algorithm>
#include <cmath>

namespace Fast::Renderer::DiffuseGi {
namespace {

constexpr const char* kOutputShader = R"glsl(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(std140, set = 0, binding = 0) uniform OutputParams {
    uvec4 extent;   // buffer width, buffer height, viewport x, viewport y
    uvec4 viewport; // viewport width, viewport height, mode (0 final, 1 neutral), 0
    vec4 limits;    // largest stored |J|
} p;
layout(std430, set = 0, binding = 1) readonly buffer Colour { vec4 colour[]; };
layout(std430, set = 0, binding = 2) readonly buffer Geometry { vec4 geo[]; };
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D illumination;
layout(set = 0, binding = 4, r32f) uniform writeonly image2D accessibility;

bool finite1(float v) { return !isnan(v) && !isinf(v); }

float stored(float v, float limit) { return finite1(v) ? clamp(v, -limit, limit) : 0.0; }

void main() {
    uvec2 id = gl_GlobalInvocationID.xy;
    if (id.x >= p.extent.x || id.y >= p.extent.y) return; // no barriers in this kernel
    vec4 j = vec4(0.0);
    float a = 1.0;
    bool inside = id.x >= p.extent.z && id.y >= p.extent.w && id.x - p.extent.z < p.viewport.x &&
                  id.y - p.extent.w < p.viewport.y;
    if (p.viewport.z == 0u && inside) {
        uint i = (id.y - p.extent.w) * p.viewport.x + (id.x - p.extent.z);
        vec4 g = geo[i];
        bool receiver = g.w > 0.0 && finite1(g.w) && finite1(g.x) && finite1(g.y) && finite1(g.z) &&
                        g.x * g.x + g.y * g.y + g.z * g.z > 0.0;
        if (receiver) {
            vec4 c = colour[i];
            float limit = p.limits.x;
            j = vec4(stored(c.x, limit), stored(c.y, limit), stored(c.z, limit), 1.0);
            a = finite1(c.w) ? clamp(c.w, 0.0, 1.0) : 1.0;
        }
    }
    imageStore(illumination, ivec2(id), j);
    imageStore(accessibility, ivec2(id), vec4(a, 0.0, 0.0, 0.0));
}
)glsl";

float Stored(float v, float limit) noexcept { return std::isfinite(v) ? std::clamp(v, -limit, limit) : 0.0F; }

} // namespace

std::string BuildDiffuseGiOutputShader() { return kOutputShader; }

std::vector<PassKernelSource> BuildDiffuseGiPassKernels() {
    std::vector<PassKernelSource> kernels;
    kernels.reserve(kPassKernelCount);
    kernels.push_back({PassKernel::Normal, "diffuse_gi_normal.comp", BuildDiffuseGiNormalShader()});
    kernels.push_back(
        {PassKernel::HierarchyLevel0, "diffuse_gi_hierarchy_level0.comp", BuildDiffuseGiHierarchyLevel0Shader()});
    kernels.push_back(
        {PassKernel::HierarchyReduce, "diffuse_gi_hierarchy_reduce.comp", BuildDiffuseGiHierarchyReduceShader()});
    kernels.push_back({PassKernel::Gather, "diffuse_gi_gather.comp", BuildDiffuseGiGatherM4Shader()});
    kernels.push_back({PassKernel::Pack, "diffuse_gi_pack.comp", BuildDiffuseGiPackShader()});
    kernels.push_back({PassKernel::Temporal, "diffuse_gi_temporal.comp", BuildDiffuseGiTemporalShader()});
    kernels.push_back({PassKernel::Filter, "diffuse_gi_filter.comp", BuildDiffuseGiFilterShader()});
    kernels.push_back({PassKernel::Output, "diffuse_gi_output.comp", BuildDiffuseGiOutputShader()});
    return kernels;
}

OutputTexel OutputTexelF32(const OutputShaderParams& p, uint32_t x, uint32_t y, const float* colour,
                           const float* geometry) noexcept {
    OutputTexel out;
    const bool inside = x >= p.Extent[2] && y >= p.Extent[3] && x - p.Extent[2] < p.Viewport[0] &&
                        y - p.Extent[3] < p.Viewport[1];
    if (p.Viewport[2] != kOutputModeFinal || !inside) return out;
    const size_t i = static_cast<size_t>(y - p.Extent[3]) * p.Viewport[0] + (x - p.Extent[2]);
    const float* g = geometry + 4 * i;
    const bool receiver = g[3] > 0.0F && std::isfinite(g[3]) && std::isfinite(g[0]) && std::isfinite(g[1]) &&
                          std::isfinite(g[2]) && g[0] * g[0] + g[1] * g[1] + g[2] * g[2] > 0.0F;
    if (!receiver) return out;
    const float* c = colour + 4 * i;
    const float limit = p.Limits[0];
    out.Illumination = {Stored(c[0], limit), Stored(c[1], limit), Stored(c[2], limit), 1.0F};
    out.Accessibility = std::isfinite(c[3]) ? std::clamp(c[3], 0.0F, 1.0F) : 1.0F;
    return out;
}

} // namespace Fast::Renderer::DiffuseGi
