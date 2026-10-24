// Renderer-facing split-screen contract tests. They use synthetic PICA draw
// views; no guest, backend, or game instance is started.
#include "oot3d_native_pica_visual_frame.h"
#include "oot3d_coop_splitscreen.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace Oot3dNativeGame;

int gChecks = 0;

void Require(bool condition, const std::string& what) {
  ++gChecks;
  if (!condition) throw std::runtime_error(what);
}

Oot3d::Renderer::PicaVertexTransformLayout MakeTransformLayout() {
  Oot3d::Renderer::PicaVertexTransformLayout layout;
  layout.Operation = Oot3d::Renderer::PicaVertexTransformOperation::ModelViewProjection3x4;
  layout.ProjectionFirstUniform = 8;
  layout.ProjectionRowCount = 4;
  layout.ViewFirstUniform = 16;
  layout.ViewRowCount = 3;
  layout.ModelFirstUniform = 20;
  layout.ModelRowCount = 3;
  layout.PositionInputRegister = 0;
  layout.NormalInputRegister = 1;
  return layout;
}

Oot3dPicaVulkanDrawPlan MakePlan(Oot3dPicaCompositionDomain domain) {
  Oot3dPicaVulkanDrawPlan plan;
  plan.CompositionDomain = domain;
  plan.State.Framebuffer.Width = 1280;
  plan.State.Framebuffer.Height = 720;
  plan.State.Framebuffer.Flipped = false;
  plan.State.Viewport = {640.0F, 360.0F, 0.0F, 1.0F, 0, 0, true};
  plan.State.Scissor = {Oot3dPicaScissorMode::Disabled, 0, 0, 1280, 720};
  auto program = std::make_shared<Oot3dPicaTemporalVertexProgram>();
  program->Hooks.Transform = MakeTransformLayout();
  plan.VertexShader.TemporalProgram = program;
  for (size_t row = 0; row < plan.VertexShader.Uniforms.Floats.size(); ++row) {
    plan.VertexShader.Uniforms.Floats[row] = {
        static_cast<float>(row), static_cast<float>(row) + 0.25F,
        static_cast<float>(row) + 0.5F, static_cast<float>(row) + 0.75F};
  }
  return plan;
}

CoopSplitRenderPlan MakeSplitPlan() {
  CoopSplitRenderPlan plan;
  plan.Mode = SplitMode::Split;
  plan.UiSingle = true;
  plan.Panels[0].Source = CoopSplitSource::Link;
  plan.Panels[0].Rect = {0.0F, 0.5F, 1.0F, 0.5F};
  plan.Panels[0].Camera.Eye = {0.0F, 0.0F, 10.0F};
  plan.Panels[0].Camera.At = {0.0F, 0.0F, 0.0F};
  plan.Panels[0].Camera.NearPlane = 1.0F;
  plan.Panels[0].Camera.FarPlane = 100.0F;
  plan.Panels[0].Camera.Aspect = 1280.0F / 360.0F;
  plan.Panels[0].Camera.Valid = true;
  plan.Panels[1].Source = CoopSplitSource::Dog;
  plan.Panels[1].Rect = {0.0F, 0.0F, 1.0F, 0.5F};
  plan.Panels[1].Camera.Eye = {1000.0F, 90.0F, -180.0F};
  plan.Panels[1].Camera.At = {1000.0F, 30.0F, 0.0F};
  plan.Panels[1].Camera.NearPlane = 1.0F;
  plan.Panels[1].Camera.FarPlane = 100.0F;
  plan.Panels[1].Camera.Aspect = 1280.0F / 360.0F;
  plan.Panels[1].Camera.Valid = true;
  return plan;
}

void TestSceneIsDuplicatedAndUiIsSingle() {
  auto scenePlan = MakePlan(Oot3dPicaCompositionDomain::Scene);
  auto uiPlan = MakePlan(Oot3dPicaCompositionDomain::Ui);
  auto unknownPlan = MakePlan(Oot3dPicaCompositionDomain::Unknown);

  Oot3dPicaVisualFrameViewSample sample;
  sample.Draws.push_back({&scenePlan});
  sample.Draws.push_back({&uiPlan});
  sample.Draws.push_back({&unknownPlan});

  const auto split = MakeSplitPlan();
  const auto views = BuildCoopSplitDrawViews(sample, split);
  Require(views.size() == 4U,
          "scene is duplicated, UI and unknown stay single");

  Require(views[0].Target == CoopSplitDrawTarget::LinkPanel,
          "first scene view is the Link panel");
  Require(views[1].Target == CoopSplitDrawTarget::DogPanel,
          "second scene view is the Dog panel");
  Require(!views[0].View.HostReplay && views[1].View.HostReplay,
          "only the dog panel is a host replay of the native scene draw");
  Require(views[2].Target == CoopSplitDrawTarget::Single,
          "UI draw stays single");
  Require(views[3].Target == CoopSplitDrawTarget::Single,
          "unknown draw stays single");

  Require(views[0].View.Viewport.has_value() &&
              views[1].View.Viewport.has_value(),
          "scene panels carry viewport overrides");
  Require(views[2].View.Viewport.has_value() == false,
          "UI keeps its original viewport");

  const auto& top = *views[0].View.Viewport;
  const auto& bottom = *views[1].View.Viewport;
  Require(std::fabs(top.HalfHeight - 180.0F) < 0.01F,
          "Link panel is half of 720");
  Require(std::fabs(bottom.HalfHeight - 180.0F) < 0.01F,
          "Dog panel is half of 720");
  Require(top.CornerY != bottom.CornerY,
          "the panels are vertically disjoint");
  Require(views[0].View.Scissor.has_value() &&
              views[1].View.Scissor.has_value(),
          "scene panels carry scissor overrides");
}

void TestPanelUniformsArePatched() {
  auto scenePlan = MakePlan(Oot3dPicaCompositionDomain::Scene);
  Oot3dPicaVisualFrameViewSample sample;
  sample.Draws.push_back({&scenePlan});

  const auto split = MakeSplitPlan();
  const auto views = BuildCoopSplitDrawViews(sample, split);
  Require(views.size() == 2U, "one scene draw becomes two views");
  Require(views[0].View.VertexUniforms.has_value(),
          "Link panel has patched uniforms");
  Require(views[1].View.VertexUniforms.has_value(),
          "Dog panel has patched uniforms");

  const auto& layout = scenePlan.VertexShader.TemporalProgram->Hooks.Transform;
  const auto& base = scenePlan.VertexShader.Uniforms.Floats;
  const auto& link = views[0].View.VertexUniforms->Floats;
  const auto& dog = views[1].View.VertexUniforms->Floats;
  Require(link[layout.ViewFirstUniform][0] != base[layout.ViewFirstUniform][0],
          "Link view row was patched");
  Require(dog[layout.ViewFirstUniform][0] != link[layout.ViewFirstUniform][0],
          "Dog view differs from Link view");
  Require(link[layout.ModelFirstUniform][0] ==
              base[layout.ModelFirstUniform][0],
          "model row is preserved");
  Require(dog[layout.ProjectionFirstUniform][0] !=
              base[layout.ProjectionFirstUniform][0],
          "projection was rebuilt for the panel");
}

void TestSinglePlanDoesNotDuplicate() {
  auto scenePlan = MakePlan(Oot3dPicaCompositionDomain::Scene);
  Oot3dPicaVisualFrameViewSample sample;
  sample.Draws.push_back({&scenePlan});

  CoopSplitRenderPlan single;
  single.Mode = SplitMode::Single;
  const auto views = BuildCoopSplitDrawViews(sample, single);
  Require(views.size() == 1U, "single mode leaves one view");
  Require(views[0].Target == CoopSplitDrawTarget::Single,
          "single target in single mode");
  Require(!views[0].View.Viewport.has_value(),
          "single mode keeps the native viewport");
}

void TestInvalidPanelFallsBackToSingle() {
  auto scenePlan = MakePlan(Oot3dPicaCompositionDomain::Scene);
  Oot3dPicaVisualFrameViewSample sample;
  sample.Draws.push_back({&scenePlan});

  auto split = MakeSplitPlan();
  split.Panels[1].Camera.Valid = false;
  const auto views = BuildCoopSplitDrawViews(sample, split);
  Require(views.size() == 1U, "invalid panel falls back to one view");
  Require(views[0].Target == CoopSplitDrawTarget::Single,
          "invalid split uses the single target");
}

} // namespace

int main() {
  try {
    TestSceneIsDuplicatedAndUiIsSingle();
    TestPanelUniformsArePatched();
    TestSinglePlanDoesNotDuplicate();
    TestInvalidPanelFallsBackToSingle();
  } catch (const std::exception& error) {
    std::cerr << "pica split tests failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "pica split tests passed (" << gChecks << " checks)\n";
  return 0;
}
