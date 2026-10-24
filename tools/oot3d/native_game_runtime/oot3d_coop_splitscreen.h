#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Oot3dNativeGame {

inline constexpr float kSplitGraceSeconds = 0.5F;
inline constexpr float kSplitSafeMargin = 0.9F;
inline constexpr float kDogCameraDistance = 180.0F;
inline constexpr float kDogCameraHeight = 90.0F;
inline constexpr float kDogCameraTargetHeight = 30.0F;

enum class SplitMode : uint8_t { Single, Split };

enum class SplitReason : uint8_t {
  DogActive,
  NoDog,
  P2Inactive,
  Cutscene,
  Pause,
  Loading,
  InvalidActor,
  InvalidCamera,
  Reset,
  Disabled,
};

enum class CoopSplitSource : uint8_t { Link, Dog };

struct SplitRect {
  float X = 0.0F;
  float Y = 0.0F;
  float Width = 0.0F;
  float Height = 0.0F;
};

struct SplitCamera {
  std::array<float, 3> Eye{};
  std::array<float, 3> At{};
  std::array<float, 3> Up{0.0F, 1.0F, 0.0F};
  float NearPlane = 0.0F;
  float FarPlane = 0.0F;
  float Aspect = 1.0F;
  std::array<std::array<float, 4>, 3> View{};
  std::array<std::array<float, 4>, 4> Projection{};
  // Column-major world-to-view for the native light binding.
  std::array<float, 16> WorldToViewColumnMajor{};
  bool Valid = false;
};

struct CharacterFrame {
  uint32_t ActorAddress = 0U;
  std::array<float, 3> WorldPosition{};
  float WorldHeight = 0.0F;
  std::array<float, 3> Projected{};
  bool InsideSafeMargin = false;
  bool Valid = false;
};

struct CoopSplitPanel {
  CoopSplitSource Source = CoopSplitSource::Link;
  SplitRect Rect{};
  SplitCamera Camera;
  bool Valid = false;
};

struct CoopSplitRenderPlan {
  SplitMode Mode = SplitMode::Single;
  std::array<CoopSplitPanel, 2> Panels{};
  bool UiSingle = true;
  bool TopTransferAfterSplit = true;
  SplitReason Reason = SplitReason::NoDog;
};

struct SplitScreenState {
  SplitMode Mode = SplitMode::Single;
  float OutsideTimerSeconds = 0.0F;
  float InsideTimerSeconds = 0.0F;
  bool LinkFramed = false;
  bool DogFramed = false;
  uint32_t SplitCount = 0U;
  uint32_t MergeCount = 0U;
  SplitReason Reason = SplitReason::NoDog;
};

struct SplitScreenInputs {
  bool DogActive = false;
  bool P2Active = false;
  bool Gameplay = false;
  bool Cutscene = false;
  bool Pause = false;
  bool Loading = false;
  bool LinkValid = false;
  bool DogValid = false;
  bool ResetRequested = false;
  bool MergeRequested = false;
  std::array<float, 3> LinkPosition{};
  std::array<float, 3> DogPosition{};
  float LinkHeight = 0.0F;
  float DogHeight = 0.0F;
  float DogYaw = 0.0F; // radians; 0 faces +Z
  float SceneLeft = -1.0F;
  float SceneRight = 1.0F;
  float SceneBottom = -1.0F;
  float SceneTop = 1.0F;
  float SceneNear = 1.0F;
  float SceneFar = 100.0F;
  std::array<float, 3> SceneEye{};
  std::array<float, 3> SceneAt{};
  float DeltaSeconds = 0.0F;
  float ViewportWidth = 1.0F;
  float ViewportHeight = 1.0F;
  float SafeMargin = kSplitSafeMargin;
  float GraceSeconds = kSplitGraceSeconds;
  float DogCameraDistance = kDogCameraDistance;
  float DogCameraHeight = kDogCameraHeight;
  float DogCameraTargetHeight = kDogCameraTargetHeight;
};

struct SplitScreenResult {
  SplitScreenState State;
  CoopSplitRenderPlan Plan;
};

namespace detail {

constexpr float kEpsilon = 1.0e-5F;

inline bool Finite3(const std::array<float, 3>& v) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

inline std::array<float, 3> Sub(const std::array<float, 3>& a,
                                const std::array<float, 3>& b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

inline float Dot(const std::array<float, 3>& a,
                 const std::array<float, 3>& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline std::array<float, 3> Cross(const std::array<float, 3>& a,
                                  const std::array<float, 3>& b) {
  return {a[1] * b[2] - a[2] * b[1],
          a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}

inline bool Normalize(std::array<float, 3>& v) {
  const float length = std::sqrt(Dot(v, v));
  if (!std::isfinite(length) || length <= kEpsilon) return false;
  v[0] /= length;
  v[1] /= length;
  v[2] /= length;
  return true;
}

inline bool BuildView(const std::array<float, 3>& eye,
                      const std::array<float, 3>& at,
                      SplitCamera& camera) {
  if (!Finite3(eye) || !Finite3(at)) return false;
  auto forward = Sub(at, eye);
  if (!Normalize(forward)) return false;
  std::array<float, 3> up{0.0F, 1.0F, 0.0F};
  auto side = Cross(forward, up);
  if (!Normalize(side)) {
    side = Cross(forward, {0.0F, 0.0F, 1.0F});
    if (!Normalize(side)) return false;
  }
  auto correctedUp = Cross(side, forward);
  if (!Normalize(correctedUp)) return false;

  camera.Eye = eye;
  camera.At = at;
  camera.Up = correctedUp;
  camera.View = {{
      {side[0], side[1], side[2], -Dot(side, eye)},
      {correctedUp[0], correctedUp[1], correctedUp[2],
       -Dot(correctedUp, eye)},
      {-forward[0], -forward[1], -forward[2], Dot(forward, eye)},
  }};
  camera.WorldToViewColumnMajor.fill(0.0F);
  for (size_t row = 0; row < 3U; ++row) {
    for (size_t column = 0; column < 4U; ++column) {
      camera.WorldToViewColumnMajor[column * 4U + row] =
          camera.View[row][column];
    }
  }
  camera.WorldToViewColumnMajor[15] = 1.0F;
  return true;
}

inline bool BuildProjection(float left, float right, float bottom, float top,
                            float nearPlane, float farPlane,
                            SplitCamera& camera) {
  if (!(right > left) || !(top > bottom) || !(nearPlane > 0.0F) ||
      !(farPlane > nearPlane)) {
    return false;
  }
  camera.Projection = {};
  camera.Projection[0][0] = 2.0F * nearPlane / (right - left);
  camera.Projection[1][1] = 2.0F * nearPlane / (top - bottom);
  camera.Projection[2][0] = (right + left) / (right - left);
  camera.Projection[2][1] = (top + bottom) / (top - bottom);
  camera.Projection[2][2] = -(farPlane + nearPlane) / (farPlane - nearPlane);
  camera.Projection[2][3] =
      -2.0F * farPlane * nearPlane / (farPlane - nearPlane);
  camera.Projection[3][2] = -1.0F;
  camera.NearPlane = nearPlane;
  camera.FarPlane = farPlane;
  return true;
}

inline bool PanelFrustum(const SplitScreenInputs& inputs, float panelAspect,
                         float* left, float* right) {
  const float sourceWidth = inputs.SceneRight - inputs.SceneLeft;
  const float sourceHeight = inputs.SceneTop - inputs.SceneBottom;
  if (!(sourceWidth > 0.0F) || !(sourceHeight > 0.0F) ||
      !(panelAspect > 0.0F)) {
    return false;
  }
  const float center = (inputs.SceneLeft + inputs.SceneRight) * 0.5F;
  const float halfHeight = sourceHeight * 0.5F;
  const float halfWidth = halfHeight * panelAspect;
  *left = center - halfWidth;
  *right = center + halfWidth;
  return true;
}

inline bool ProjectPoint(const SplitCamera& camera,
                         const std::array<float, 3>& point,
                         std::array<float, 3>* ndc) {
  if (!camera.Valid || ndc == nullptr) return false;
  const float viewX = camera.View[0][0] * point[0] +
                      camera.View[0][1] * point[1] +
                      camera.View[0][2] * point[2] + camera.View[0][3];
  const float viewY = camera.View[1][0] * point[0] +
                      camera.View[1][1] * point[1] +
                      camera.View[1][2] * point[2] + camera.View[1][3];
  const float viewZ = camera.View[2][0] * point[0] +
                      camera.View[2][1] * point[1] +
                      camera.View[2][2] * point[2] + camera.View[2][3];
  const auto& p = camera.Projection;
  const float clipX = p[0][0] * viewX + p[0][1] * viewY + p[0][2] * viewZ +
                      p[0][3];
  const float clipY = p[1][0] * viewX + p[1][1] * viewY + p[1][2] * viewZ +
                      p[1][3];
  const float clipZ = p[2][0] * viewX + p[2][1] * viewY + p[2][2] * viewZ +
                      p[2][3];
  const float clipW = p[3][0] * viewX + p[3][1] * viewY + p[3][2] * viewZ +
                      p[3][3];
  if (!(clipW > kEpsilon)) return false;
  (*ndc) = {clipX / clipW, clipY / clipW, clipZ / clipW};
  return std::isfinite((*ndc)[0]) && std::isfinite((*ndc)[1]) &&
         std::isfinite((*ndc)[2]);
}

inline CharacterFrame FrameCharacter(uint32_t actorAddress,
                                     const std::array<float, 3>& position,
                                     float height, bool valid,
                                     const SplitCamera& camera,
                                     float safeMargin) {
  CharacterFrame frame;
  frame.ActorAddress = actorAddress;
  frame.WorldPosition = position;
  frame.WorldHeight = height;
  frame.Valid = valid && Finite3(position);
  if (!frame.Valid) return frame;
  const std::array<float, 3> body{position[0], position[1] + height,
                                  position[2]};
  std::array<float, 3> ndc{};
  if (!ProjectPoint(camera, body, &ndc)) return frame;
  frame.Projected = ndc;
  frame.InsideSafeMargin = std::fabs(ndc[0]) <= safeMargin &&
                           std::fabs(ndc[1]) <= safeMargin &&
                           ndc[2] >= -1.0F && ndc[2] <= 1.0F;
  return frame;
}

inline CoopSplitRenderPlan SinglePlan(const SplitScreenState& state) {
  CoopSplitRenderPlan plan;
  plan.Mode = SplitMode::Single;
  plan.UiSingle = true;
  plan.TopTransferAfterSplit = true;
  plan.Reason = state.Reason;
  return plan;
}

inline bool BuildDogCamera(const SplitScreenInputs& inputs, float aspect,
                           SplitCamera* camera) {
  if (camera == nullptr || !Finite3(inputs.DogPosition) ||
      !std::isfinite(inputs.DogYaw)) {
    return false;
  }
  const float distance = std::max(0.0F, inputs.DogCameraDistance);
  const float height = std::max(0.0F, inputs.DogCameraHeight);
  const float targetHeight = std::max(0.0F, inputs.DogCameraTargetHeight);
  const std::array<float, 3> forward{std::sin(inputs.DogYaw), 0.0F,
                                     std::cos(inputs.DogYaw)};
  const std::array<float, 3> eye{
      inputs.DogPosition[0] - forward[0] * distance,
      inputs.DogPosition[1] + height,
      inputs.DogPosition[2] - forward[2] * distance};
  const std::array<float, 3> at{inputs.DogPosition[0],
                                inputs.DogPosition[1] + targetHeight,
                                inputs.DogPosition[2]};
  if (!BuildView(eye, at, *camera)) return false;
  float left = 0.0F;
  float right = 0.0F;
  if (!PanelFrustum(inputs, aspect, &left, &right)) return false;
  if (!BuildProjection(left, right, inputs.SceneBottom, inputs.SceneTop,
                       inputs.SceneNear, inputs.SceneFar, *camera)) {
    return false;
  }
  camera->Aspect = aspect;
  camera->Valid = true;
  return true;
}

inline bool BuildLinkCamera(const SplitScreenInputs& inputs, float aspect,
                            SplitCamera* camera) {
  if (camera == nullptr || !Finite3(inputs.SceneEye) ||
      !Finite3(inputs.SceneAt)) {
    return false;
  }
  if (!BuildView(inputs.SceneEye, inputs.SceneAt, *camera)) return false;
  float left = 0.0F;
  float right = 0.0F;
  if (!PanelFrustum(inputs, aspect, &left, &right)) return false;
  if (!BuildProjection(left, right, inputs.SceneBottom, inputs.SceneTop,
                       inputs.SceneNear, inputs.SceneFar, *camera)) {
    return false;
  }
  camera->Aspect = aspect;
  camera->Valid = true;
  return true;
}

inline bool BuildSourceCamera(const SplitScreenInputs& inputs,
                              SplitCamera* camera) {
  if (camera == nullptr || !Finite3(inputs.SceneEye) ||
      !Finite3(inputs.SceneAt)) {
    return false;
  }
  if (!BuildView(inputs.SceneEye, inputs.SceneAt, *camera)) return false;
  if (!BuildProjection(inputs.SceneLeft, inputs.SceneRight, inputs.SceneBottom,
                       inputs.SceneTop, inputs.SceneNear, inputs.SceneFar,
                       *camera)) {
    return false;
  }
  const float width = inputs.SceneRight - inputs.SceneLeft;
  const float height = inputs.SceneTop - inputs.SceneBottom;
  camera->Aspect = width / height;
  camera->Valid = true;
  return true;
}

inline CoopSplitRenderPlan SplitPlan(const SplitScreenInputs& inputs,
                                     const CharacterFrame& link,
                                     const CharacterFrame& dog) {
  const float panelWidth = std::max(1.0F, inputs.ViewportWidth);
  const float panelHeight = std::max(1.0F, inputs.ViewportHeight * 0.5F);
  const float aspect = panelWidth / panelHeight;

  CoopSplitRenderPlan plan;
  plan.Mode = SplitMode::Split;
  plan.UiSingle = true;
  plan.TopTransferAfterSplit = true;
  plan.Reason = SplitReason::DogActive;

  plan.Panels[0].Source = CoopSplitSource::Link;
  plan.Panels[0].Rect = {0.0F, 0.5F, 1.0F, 0.5F};
  plan.Panels[0].Valid = BuildLinkCamera(inputs, aspect, &plan.Panels[0].Camera);

  plan.Panels[1].Source = CoopSplitSource::Dog;
  plan.Panels[1].Rect = {0.0F, 0.0F, 1.0F, 0.5F};
  plan.Panels[1].Valid = BuildDogCamera(inputs, aspect, &plan.Panels[1].Camera);

  if (!plan.Panels[0].Valid || !plan.Panels[1].Valid ||
      !link.Valid || !dog.Valid) {
    plan.Mode = SplitMode::Single;
    plan.Reason = SplitReason::InvalidCamera;
    plan.Panels = {};
    plan.UiSingle = true;
    plan.TopTransferAfterSplit = true;
  }
  return plan;
}

} // namespace detail

inline SplitScreenResult UpdateSplitScreen(const SplitScreenState& previous,
                                           const SplitScreenInputs& inputs) {
  SplitScreenState state = previous;
  const float delta = std::max(0.0F, std::isfinite(inputs.DeltaSeconds)
                                         ? inputs.DeltaSeconds
                                         : 0.0F);
  const float grace = std::max(0.0F, inputs.GraceSeconds);
  const float margin = std::clamp(inputs.SafeMargin, 0.0F, 1.0F);

  const auto forceSingle = [&](SplitReason reason) {
    state.Mode = SplitMode::Single;
    state.OutsideTimerSeconds = 0.0F;
    state.InsideTimerSeconds = 0.0F;
    state.LinkFramed = false;
    state.DogFramed = false;
    state.Reason = reason;
    SplitScreenResult result;
    result.State = state;
    result.Plan = detail::SinglePlan(state);
    return result;
  };

  if (inputs.ResetRequested) {
    return forceSingle(SplitReason::Reset);
  }
  if (!inputs.DogActive) {
    return forceSingle(SplitReason::NoDog);
  }
  if (!inputs.P2Active) {
    return forceSingle(SplitReason::P2Inactive);
  }
  if (inputs.Cutscene) {
    return forceSingle(SplitReason::Cutscene);
  }
  if (inputs.Pause || !inputs.Gameplay) {
    return forceSingle(SplitReason::Pause);
  }
  if (inputs.Loading) {
    return forceSingle(SplitReason::Loading);
  }
  if (!inputs.LinkValid || !inputs.DogValid) {
    return forceSingle(SplitReason::InvalidActor);
  }

  SplitCamera source;
  if (!detail::BuildSourceCamera(inputs, &source)) {
    return forceSingle(SplitReason::InvalidCamera);
  }

  const auto link = detail::FrameCharacter(
      0U, inputs.LinkPosition, inputs.LinkHeight, inputs.LinkValid, source,
      margin);
  const auto dog = detail::FrameCharacter(
      0U, inputs.DogPosition, inputs.DogHeight, inputs.DogValid, source,
      margin);

  state.LinkFramed = link.InsideSafeMargin;
  state.DogFramed = dog.InsideSafeMargin;

  if (inputs.MergeRequested && state.Mode == SplitMode::Split) {
    state.Mode = SplitMode::Single;
    state.InsideTimerSeconds = 0.0F;
    state.OutsideTimerSeconds = 0.0F;
    ++state.MergeCount;
  }

  if (state.Mode == SplitMode::Single) {
    if (!link.InsideSafeMargin || !dog.InsideSafeMargin) {
      state.OutsideTimerSeconds += delta;
      state.InsideTimerSeconds = 0.0F;
      if (state.OutsideTimerSeconds >= grace) {
        state.Mode = SplitMode::Split;
        state.OutsideTimerSeconds = 0.0F;
        ++state.SplitCount;
      }
    } else {
      state.OutsideTimerSeconds = 0.0F;
    }
  } else {
    if (link.InsideSafeMargin && dog.InsideSafeMargin) {
      state.InsideTimerSeconds += delta;
      state.OutsideTimerSeconds = 0.0F;
      if (state.InsideTimerSeconds >= grace) {
        state.Mode = SplitMode::Single;
        state.InsideTimerSeconds = 0.0F;
        ++state.MergeCount;
      }
    } else {
      state.InsideTimerSeconds = 0.0F;
    }
  }

  state.Reason = SplitReason::DogActive;
  SplitScreenResult result;
  result.State = state;
  if (state.Mode == SplitMode::Split) {
    result.Plan = detail::SplitPlan(inputs, link, dog);
    if (result.Plan.Mode == SplitMode::Single) {
      state.Mode = SplitMode::Single;
      state.Reason = SplitReason::InvalidCamera;
      result.State = state;
    }
  } else {
    result.Plan = detail::SinglePlan(state);
  }
  return result;
}

inline void ResetSplitScreen(SplitScreenState& state) {
  state.Mode = SplitMode::Single;
  state.OutsideTimerSeconds = 0.0F;
  state.InsideTimerSeconds = 0.0F;
  state.LinkFramed = false;
  state.DogFramed = false;
  state.SplitCount = 0U;
  state.MergeCount = 0U;
  state.Reason = SplitReason::NoDog;
}

} // namespace Oot3dNativeGame
