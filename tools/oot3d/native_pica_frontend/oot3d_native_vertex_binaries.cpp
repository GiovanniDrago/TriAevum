// Generated translated OOT3D title vertex SPIR-V. Not title-neutral code.
// Source: original SHBIN interfaces + maintained GLSL wrapper and translated body.
// Citra/Azahar translator donor: GPL-2.0-or-later; preserve donor notices.
// Regenerate oot3d_native_vertex.spvpack with
// tools/renderer/tev_program/build_vertex_artifacts (see the provenance JSON).
#include "oot3d_native_vertex_binaries.h"
#include "fast/renderer/spirv_pack.h"

namespace Oot3dNativeGame {
namespace {
TRIAEVUM_SPIRV_PACK_EMBED_BEGIN
alignas(16) const unsigned char kPack[] = {
#if TRIAEVUM_SPIRV_PACK_EMBED
#embed "oot3d_native_vertex.spvpack"
#else
#include "oot3d_native_vertex.spvpack.inc"
#endif
};
TRIAEVUM_SPIRV_PACK_EMBED_END
const std::vector<Fast::Renderer::SpirvPackEntry>& Entries() {
    static const auto entries = Fast::Renderer::ReadSpirvPack(kPack);
    return entries;
}
} // namespace

std::span<const Fast::Renderer3ds::PicaVertexArtifact> TitleVertexArtifacts() {
    static const auto artifacts = Fast::Renderer3ds::PicaVertexArtifactsFromPack(Entries());
    return artifacts;
}
std::span<const Fast::Renderer::PassShaderArtifact> TitlePassArtifacts() {
    static const auto artifacts = Fast::Renderer::PassShaderArtifactsFromPack(Entries());
    return artifacts;
}
} // namespace Oot3dNativeGame
