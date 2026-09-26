#pragma once

#include "fast/oot3d/graphics_settings_window.h"
#include "fast/renderer3ds/pica_environment_lighting.h"
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>

namespace Oot3dNativeGame {

struct SceneDirectionalLight {
  float Azimuth = 45.0F;
  float Elevation = 45.0F;
  std::array<float, 3> Color{1.0F, 1.0F, 1.0F};
  float Intensity = 0.0F;
  bool operator==(const SceneDirectionalLight &) const = default;
};
struct SceneEnvironmentProfile {
  bool Enabled = false;
  bool PreserveSceneBrightness = true;
  float NativeDirectStrength = 0.0F;
  float NativeLocalStrength = 0.0F;
  float NativeAmbientStrength = 0.0F;
  std::array<float, 3> AmbientColor{1.0F, 1.0F, 1.0F};
  float AmbientIntensity = 0.2F;
  float FogStrength = 1.0F;
  std::array<SceneDirectionalLight, 2> Lights{
      {{45.0F, 45.0F, {1.0F, 1.0F, 1.0F}, 1.0F},
       {-135.0F, 30.0F, {1.0F, 1.0F, 1.0F}, 0.0F}}};
  std::array<Fast::Renderer3ds::PicaScenePointLightSettings,
             Fast::Renderer3ds::kPicaScenePointLightGroups> PointLights{};
  bool operator==(const SceneEnvironmentProfile &) const = default;
};
struct SceneEnvironmentSnapshot {
  std::optional<uint16_t> SceneId;
  SceneEnvironmentProfile Profile;
  std::filesystem::path Path;
  std::string Error;
  bool Dirty = false;
  bool FileExists = false;
};

class SceneEnvironmentRuntime final {
public:
  using Publisher =
      std::function<void(const Fast::Renderer3ds::PicaEnvironmentLighting &)>;
  SceneEnvironmentRuntime(std::filesystem::path directory, Publisher publish);
  void ObserveScene(std::optional<uint16_t> scene);
  [[nodiscard]] SceneEnvironmentSnapshot Snapshot() const;
  void Preview(SceneEnvironmentProfile profile);
  bool Save();
  bool Reload();

private:
  bool ReloadLocked();
  void PublishLocked();
  std::filesystem::path mDirectory;
  Publisher mPublish;
  mutable std::mutex mMutex;
  SceneEnvironmentSnapshot mState;
  SceneEnvironmentProfile mSaved;
};

std::shared_ptr<Fast::Oot3d::GraphicsSettingsPanelTab>
CreateSceneEnvironmentPanel(std::shared_ptr<SceneEnvironmentRuntime> runtime);

} // namespace Oot3dNativeGame
