// Pure unit tests for the co-op split-screen controller. These tests do not
// boot the guest or the renderer: they pin the framing rule, timers, gating,
// panel layout, and dog camera.
#include "oot3d_coop_splitscreen.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace Oot3dNativeGame;

int gChecks = 0;

void Require(bool condition, const std::string& what) {
  ++gChecks;
  if (!condition) throw std::runtime_error(what);
}

void RequireNear(float actual, float expected, const std::string& what,
                 float tolerance = 0.01F) {
  Require(std::fabs(actual - expected) <= tolerance, what);
}

SplitScreenInputs BaseInputs() {
  SplitScreenInputs inputs;
  inputs.DogActive = true;
  inputs.P2Active = true;
  inputs.Gameplay = true;
  inputs.LinkValid = true;
  inputs.DogValid = true;
  inputs.LinkPosition = {0.0F, 0.0F, 0.0F};
  inputs.DogPosition = {0.0F, 0.0F, 0.0F};
  inputs.LinkHeight = 0.0F;
  inputs.DogHeight = 0.0F;
  inputs.DogYaw = 0.0F;
  inputs.SceneLeft = -1.0F;
  inputs.SceneRight = 1.0F;
  inputs.SceneBottom = -1.0F;
  inputs.SceneTop = 1.0F;
  inputs.SceneNear = 1.0F;
  inputs.SceneFar = 100.0F;
  inputs.SceneEye = {0.0F, 0.0F, 10.0F};
  inputs.SceneAt = {0.0F, 0.0F, 0.0F};
  inputs.DeltaSeconds = 0.1F;
  inputs.ViewportWidth = 1280.0F;
  inputs.ViewportHeight = 720.0F;
  return inputs;
}

SplitScreenResult Step(SplitScreenState& state, const SplitScreenInputs& inputs) {
  const auto result = UpdateSplitScreen(state, inputs);
  state = result.State;
  return result;
}

void TestStartsSingleWhenFramedTogether() {
  SplitScreenState state;
  const auto result = UpdateSplitScreen(state, BaseInputs());
  Require(result.State.Mode == SplitMode::Single, "framed pair starts single");
  Require(result.State.Reason == SplitReason::DogActive,
          "active dog is the single-view reason");
  Require(result.Plan.Mode == SplitMode::Single, "single plan mode");
  Require(result.Plan.UiSingle, "UI remains single");
  Require(result.State.LinkFramed && result.State.DogFramed,
          "both characters are framed");
}

void TestSplitsAfterGraceWhenDogLeaves() {
  SplitScreenState state;
  auto inputs = BaseInputs();
  inputs.DogPosition = {1000.0F, 0.0F, 0.0F};
  inputs.DeltaSeconds = 0.25F;
  auto result = Step(state, inputs);
  Require(result.State.Mode == SplitMode::Single,
          "split does not happen before the grace period");
  Require(result.State.OutsideTimerSeconds > 0.0F,
          "outside timer accumulates");

  inputs.DeltaSeconds = 0.30F;
  result = Step(state, inputs);
  Require(result.State.Mode == SplitMode::Split, "split after grace");
  Require(result.State.SplitCount == 1U, "one split transition");
  Require(result.Plan.Mode == SplitMode::Split, "split plan mode");
  Require(result.Plan.Panels[0].Source == CoopSplitSource::Link,
          "top panel is Link");
  Require(result.Plan.Panels[1].Source == CoopSplitSource::Dog,
          "bottom panel is Dog");
  RequireNear(result.Plan.Panels[0].Rect.Y, 0.5F, "Link panel is top");
  RequireNear(result.Plan.Panels[0].Rect.Height, 0.5F,
              "Link panel is half height");
  RequireNear(result.Plan.Panels[1].Rect.Y, 0.0F, "Dog panel is bottom");
  RequireNear(result.Plan.Panels[1].Rect.Height, 0.5F,
              "Dog panel is half height");
  Require(result.Plan.Panels[0].Valid && result.Plan.Panels[1].Valid,
          "both panels are valid");
  Require(result.Plan.UiSingle, "UI stays single in split mode");
}

void TestMergesAfterGraceWhenTogether() {
  SplitScreenState state;
  auto inputs = BaseInputs();
  inputs.DogPosition = {1000.0F, 0.0F, 0.0F};
  inputs.DeltaSeconds = 0.6F;
  auto result = Step(state, inputs);
  Require(result.State.Mode == SplitMode::Split, "entered split");

  inputs.DogPosition = {0.0F, 0.0F, 0.0F};
  inputs.DeltaSeconds = 0.25F;
  result = Step(state, inputs);
  Require(result.State.Mode == SplitMode::Split,
          "merge does not happen before the grace period");
  Require(result.State.InsideTimerSeconds > 0.0F,
          "inside timer accumulates");

  inputs.DeltaSeconds = 0.30F;
  result = Step(state, inputs);
  Require(result.State.Mode == SplitMode::Single, "merge after grace");
  Require(result.State.MergeCount == 1U, "one merge transition");
  Require(result.Plan.Mode == SplitMode::Single, "single plan after merge");
}

void TestNoDogAlwaysSingle() {
  SplitScreenState state;
  auto inputs = BaseInputs();
  inputs.DogActive = false;
  inputs.DogValid = false;
  inputs.DogPosition = {1000.0F, 0.0F, 0.0F};
  inputs.DeltaSeconds = 5.0F;
  const auto result = UpdateSplitScreen(state, inputs);
  Require(result.State.Mode == SplitMode::Single, "no dog is single");
  Require(result.State.Reason == SplitReason::NoDog, "no-dog reason");
}

void TestP2InactiveAlwaysSingle() {
  SplitScreenState state;
  auto inputs = BaseInputs();
  inputs.P2Active = false;
  inputs.DeltaSeconds = 5.0F;
  const auto result = UpdateSplitScreen(state, inputs);
  Require(result.State.Mode == SplitMode::Single, "inactive P2 is single");
  Require(result.State.Reason == SplitReason::P2Inactive,
          "inactive-P2 reason");
}

void TestNonGameplayStatesAlwaysSingle() {
  for (const auto reason : {SplitReason::Cutscene, SplitReason::Pause,
                            SplitReason::Loading}) {
    SplitScreenState state;
    auto inputs = BaseInputs();
    inputs.DogPosition = {1000.0F, 0.0F, 0.0F};
    inputs.DeltaSeconds = 5.0F;
    if (reason == SplitReason::Cutscene) inputs.Cutscene = true;
    if (reason == SplitReason::Pause) inputs.Pause = true;
    if (reason == SplitReason::Loading) inputs.Loading = true;
    const auto result = UpdateSplitScreen(state, inputs);
    Require(result.State.Mode == SplitMode::Single,
            "non-gameplay state stays single");
    Require(result.State.Reason == reason,
            "non-gameplay reason is preserved");
  }
}

void TestInvalidActorAndCameraSingle() {
  SplitScreenState state;
  auto inputs = BaseInputs();
  inputs.LinkValid = false;
  inputs.DogPosition = {1000.0F, 0.0F, 0.0F};
  auto result = UpdateSplitScreen(state, inputs);
  Require(result.State.Mode == SplitMode::Single, "invalid Link is single");
  Require(result.State.Reason == SplitReason::InvalidActor,
          "invalid-actor reason");

  inputs = BaseInputs();
  inputs.DogValid = false;
  inputs.DogPosition = {1000.0F, 0.0F, 0.0F};
  result = UpdateSplitScreen(state, inputs);
  Require(result.State.Mode == SplitMode::Single, "invalid dog is single");

  inputs = BaseInputs();
  inputs.SceneAt = inputs.SceneEye;
  inputs.DogPosition = {1000.0F, 0.0F, 0.0F};
  result = UpdateSplitScreen(state, inputs);
  Require(result.State.Mode == SplitMode::Single, "invalid camera is single");
  Require(result.State.Reason == SplitReason::InvalidCamera,
          "invalid-camera reason");
}

void TestResetAndSummonMerge() {
  SplitScreenState state;
  auto inputs = BaseInputs();
  inputs.DogPosition = {1000.0F, 0.0F, 0.0F};
  inputs.DeltaSeconds = 0.6F;
  auto result = Step(state, inputs);
  Require(result.State.Mode == SplitMode::Split, "entered split");

  inputs = BaseInputs();
  inputs.MergeRequested = true;
  result = UpdateSplitScreen(state, inputs);
  Require(result.State.Mode == SplitMode::Single,
          "summon beside Link merges immediately");
  Require(result.State.SplitCount == 1U,
          "summon does not invent a split count");
  Require(result.State.MergeCount == 1U,
          "summon increments merge count");

  inputs = BaseInputs();
  inputs.ResetRequested = true;
  result = UpdateSplitScreen(state, inputs);
  Require(result.State.Mode == SplitMode::Single,
          "reset forces single");
  Require(result.State.OutsideTimerSeconds == 0.0F &&
              result.State.InsideTimerSeconds == 0.0F,
          "reset clears timers");
}

void TestDogCameraAndPanelAspect() {
  SplitScreenState state;
  auto inputs = BaseInputs();
  inputs.DogPosition = {10.0F, 0.0F, 20.0F};
  inputs.DogYaw = 0.0F;
  inputs.DogPosition = {1000.0F, 0.0F, 0.0F};
  inputs.DeltaSeconds = 0.6F;
  const auto result = Step(state, inputs);
  Require(result.Plan.Mode == SplitMode::Split, "split for camera test");
  const auto& dog = result.Plan.Panels[1].Camera;
  RequireNear(dog.Eye[0], 1000.0F, "dog camera eye x");
  RequireNear(dog.Eye[1], 90.0F, "dog camera eye height");
  RequireNear(dog.Eye[2], -180.0F, "dog camera eye behind the dog");
  RequireNear(dog.At[0], 1000.0F, "dog camera at x");
  RequireNear(dog.At[1], 30.0F, "dog camera target height");
  RequireNear(dog.At[2], 0.0F, "dog camera at z");

  const float linkAspect = 1280.0F / (720.0F * 0.5F);
  RequireNear(result.Plan.Panels[0].Camera.Aspect, linkAspect,
              "Link panel aspect");
  const auto& projection = result.Plan.Panels[0].Camera.Projection;
  RequireNear(projection[0][0] / projection[1][1], 1.0F / linkAspect,
              "panel projection preserves vertical extents", 0.02F);
}

void TestHysteresisDoesNotFlicker() {
  SplitScreenState state;
  auto inputs = BaseInputs();
  inputs.DeltaSeconds = 0.1F;
  int transitions = 0;
  SplitMode previous = SplitMode::Single;
  for (int frame = 0; frame < 30; ++frame) {
    inputs.DogPosition = (frame % 2 == 0) ? std::array<float, 3>{1000.0F, 0.0F, 0.0F}
                                         : std::array<float, 3>{0.0F, 0.0F, 0.0F};
    const auto result = UpdateSplitScreen(state, inputs);
    if (result.State.Mode != previous) {
      ++transitions;
      previous = result.State.Mode;
    }
    state = result.State;
  }
  Require(transitions == 0,
          "boundary hover within the grace period does not flicker");
}

} // namespace

int main() {
  try {
    TestStartsSingleWhenFramedTogether();
    TestSplitsAfterGraceWhenDogLeaves();
    TestMergesAfterGraceWhenTogether();
    TestNoDogAlwaysSingle();
    TestP2InactiveAlwaysSingle();
    TestNonGameplayStatesAlwaysSingle();
    TestInvalidActorAndCameraSingle();
    TestResetAndSummonMerge();
    TestDogCameraAndPanelAspect();
    TestHysteresisDoesNotFlicker();
  } catch (const std::exception& error) {
    std::cerr << "coop splitscreen tests failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "coop splitscreen tests passed (" << gChecks << " checks)\n";
  return 0;
}
