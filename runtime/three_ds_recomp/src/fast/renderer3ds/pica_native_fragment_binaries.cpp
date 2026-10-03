// Generated from maintained PICA equations, not captured game shaders.
// Vulkan 1.1, shaderc performance optimization; canonical and NRI interfaces.
// Regenerate pica_native_fragment.spvpack with
// tools/renderer/tev_program/build_fragment_artifacts.
#include "fast/renderer3ds/pica_native_fragment_binaries.h"
#include "fast/renderer/spirv_pack.h"

namespace Fast::Renderer3ds {
namespace {
TRIAEVUM_SPIRV_PACK_EMBED_BEGIN
alignas(16) const unsigned char kPack[] = {
#if TRIAEVUM_SPIRV_PACK_EMBED
#embed "pica_native_fragment.spvpack"
#else
#include "pica_native_fragment.spvpack.inc"
#endif
};
TRIAEVUM_SPIRV_PACK_EMBED_END
} // namespace

std::span<const PicaFragmentArtifact> NativeFragmentArtifacts() {
    static const auto entries = Renderer::ReadSpirvPack(kPack);
    static const auto artifacts = PicaFragmentArtifactsFromPack(entries);
    return artifacts;
}
} // namespace Fast::Renderer3ds
