#pragma once

#include "fast/renderer3ds/pica_scene_payloads.h"
#include "fast/renderer3ds/pica_vertex_input_layout.h"
#include "fast/renderer3ds/virtual_shadow_map.h"

namespace Fast::Renderer3ds {

struct PicaShadowGeometryView {
    std::span<const uint8_t> Payload;
    std::span<const PicaSceneVertexBufferBinding> Bindings;
    std::span<const PicaNriPackedVertexAttribute> Attributes;
    std::span<const PicaNriPackedVertexBinding> InputBindings;
    uint64_t BufferBaseOffset = 0;
    uint64_t IndexOffset = 0;
    uint32_t VertexOrIndexCount = 0;
    int32_t BaseVertex = 0;
    bool Indexed = false;
};

struct PicaShadowCasterCache {
    uint64_t Signature = 0;
    ShadowBounds Bounds;
};

// Uses only typed native transform/skeleton semantics and the same packed
// float4 inputs submitted to the GPU. Unknown programs remain unbounded and
// uncached; no guessed bounds may discard a real occluder.
[[nodiscard]] VirtualShadowCaster
BuildPicaShadowCaster(const PicaShadowGeometryView& geometry, const PicaVertexShaderHookLayout& hooks,
                      uint32_t booleanUniforms, std::span<const uint8_t> floatUniforms, const ShadowMatrix& viewToWorld,
                      uint64_t geometryVersion, PicaShadowCasterCache& cache) noexcept;

} // namespace Fast::Renderer3ds
