#pragma once
// Generated from maintained PICA equations, not captured game shaders.
// Vulkan 1.1, shaderc performance optimization; canonical and NRI interfaces.
// Regenerate pica_native_fragment.spvpack with
// tools/renderer/tev_program/build_fragment_artifacts.
#include "pica_fragment_artifact.h"

namespace Fast::Renderer3ds {
std::span<const PicaFragmentArtifact> NativeFragmentArtifacts();
} // namespace Fast::Renderer3ds
