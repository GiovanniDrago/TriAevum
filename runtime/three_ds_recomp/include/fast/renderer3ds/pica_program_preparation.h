#pragma once
#include "pica_vertex_artifact.h"
#include "fast/renderer/pass_shader_artifact.h"

#include <span>

namespace Fast::Renderer3ds {
// Explicit loading boundary. Calls complete before gameplay; no draw packet or
// borrowed asynchronous work survives the call. Other backends may omit it.
class PicaProgramPreparationBackend {
  public:
    virtual ~PicaProgramPreparationBackend() = default;
    virtual void PrepareNativePicaPrograms(std::span<const PicaVertexArtifact> vertices) = 0;
    // True when draws should carry their always-available (parametric)
    // fragment program next to the specialized one (hybrid preparation).
    [[nodiscard]] virtual bool UsesAlwaysAvailablePrograms() const { return false; }
    // Pass programs a title derives from its own programs (shadow casters,
    // surface lighting), built with its vertex family at development time.
    virtual void RegisterTitlePassPrograms(std::span<const Renderer::PassShaderArtifact> programs) { (void)programs; }
};
}
