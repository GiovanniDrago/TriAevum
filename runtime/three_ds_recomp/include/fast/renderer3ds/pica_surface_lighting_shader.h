#pragma once
#include "fast/renderer3ds/pica_shader_hooks.h"
#include "fast/renderer3ds/pica_surface_lighting.h"
#include "fast/renderer3ds/pica_vertex_surface.h"
#include <string>

namespace Fast::Renderer3ds {
inline std::string BuildPicaSurfaceLightingVertexShader(std::string_view source,
                                                        const PicaVertexShaderHookLayout& hooks, bool surfaceLighting) {
    if (!hooks.ValidFor(source) || !hooks.Supports(PicaVertexShaderHook::MainBodyEnd) ||
        !hooks.Supports(PicaVertexShaderHook::RegisterStateEnd))
        return {};
    const auto declarations = hooks.Offset(PicaVertexShaderHook::RegisterStateEnd);
    const auto end = hooks.Offset(PicaVertexShaderHook::MainBodyEnd);
    if (declarations > end)
        return {};
    const auto surface = surfaceLighting ? BuildPicaVertexSurfaceShader(hooks) : PicaVertexSurfaceShader{};
    if (surfaceLighting && !surface.Available)
        return {};
    std::string result(source.substr(0, declarations));
    result += "\nlayout(push_constant) uniform SurfaceAtlas { uvec4 allocation; } surface_atlas;\n";
    result += surface.Declarations;
    result += source.substr(declarations, end - declarations);
    result += surface.MainBody;
    result += R"glsl(
    uint atlas_index = surface_atlas.allocation.x + uint(gl_VertexIndex);
    vec2 cell = vec2(atlas_index % surface_atlas.allocation.y, atlas_index / surface_atlas.allocation.y);
    gl_Position = vec4((cell + 0.5) / vec2(surface_atlas.allocation.yz) * 2.0 - 1.0, 0.0, 1.0);
    gl_PointSize = 1.0;
)glsl";
    result += source.substr(end);
    return result;
}
inline constexpr const char* kPicaSurfaceLightingFragmentShader = R"glsl(#version 450
layout(location=0) in vec4 pica_primary_color;
layout(location=0) out vec4 surface_primary;
void main() { surface_primary = vec4(pica_primary_color.rgb, 1.0); }
)glsl";
inline std::string BuildPicaSurfaceLightingFragmentShader(bool surfaceLighting) {
    if (!surfaceLighting)
        return kPicaSurfaceLightingFragmentShader;
    std::string result = "#version 450\n"
                         "layout(location=0) in vec4 pica_primary_color;\n"
                         "layout(location=6) in vec3 pica_view;\n"
                         "layout(location=0) out vec4 surface_primary;\n";
    result += PicaSurfaceLightingDeclarations();
    result += R"glsl(
vec3 pica_surface_shadow(int slot) { return vec3(1.0); }
void main() {
    vec3 color = pica_primary_color.rgb;
    if (pica_surface.control.x != 0u || pica_surface.environment_parameters.w != 0.0 ||
        pica_surface.control.y != 0u || pica_surface.control.z != 0u)
        color = pica_surface_primary(color, pica_surface_unit(pica_surface_normal));
    surface_primary = vec4(color, 1.0);
}
)glsl";
    return result;
}
} // namespace Fast::Renderer3ds
