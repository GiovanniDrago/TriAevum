#include "oot3d_scene_environment.h"
#include "ship/config/ConfigPersistence.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <numbers>
#include <stdexcept>

namespace Oot3dNativeGame {
namespace {
using Json = nlohmann::json;
void Validate(const SceneEnvironmentProfile &profile) {
  const auto scalar = [](float value, float low, float high) {
    if (!std::isfinite(value) || value < low || value > high)
      throw std::runtime_error(
          "Environment value is non-finite or out of range");
  };
  scalar(profile.NativeDirectStrength, 0.0F, 2.0F);
  scalar(profile.NativeLocalStrength, 0.0F, 2.0F);
  scalar(profile.NativeAmbientStrength, 0.0F, 2.0F);
  scalar(profile.AmbientIntensity, 0.0F, 2.0F);
  scalar(profile.FogStrength, 0.0F, 1.0F);
  for (float value : profile.AmbientColor)
    scalar(value, 0.0F, 1.0F);
  for (const auto &light : profile.Lights) {
    scalar(light.Azimuth, -180.0F, 180.0F);
    scalar(light.Elevation, -90.0F, 90.0F);
    scalar(light.Intensity, 0.0F, 4.0F);
    for (float value : light.Color)
      scalar(value, 0.0F, 1.0F);
  }
  scalar(profile.KeyAzimuthOffset, -180.0F, 180.0F);
  scalar(profile.KeyElevationOffset, -90.0F, 90.0F);
  if (profile.KeyDirection > Fast::Renderer3ds::PicaEnvironmentKeyDirection::SceneAmbient)
    throw std::runtime_error("Unknown key light direction");
  for (const auto &light : profile.PointLights) {
    scalar(light.Intensity, 0.0F, 8.0F);
    scalar(light.RadiusScale, 0.1F, 8.0F);
    for (float value : light.ColorTint)
      scalar(value, 0.0F, 1.0F);
  }
}
float Scalar(const Json &object, const char *key, float low, float high) {
  const auto &value = object.at(key);
  if (!value.is_number())
    throw std::runtime_error(std::string(key) + " must be numeric");
  const float result = value.get<float>();
  if (!std::isfinite(result) || result < low || result > high)
    throw std::runtime_error(std::string(key) + " is out of range");
  return result;
}
std::array<float, 3> Color(const Json &object, const char *key) {
  const auto &values = object.at(key);
  if (!values.is_array() || values.size() != 3)
    throw std::runtime_error(std::string(key) + " requires RGB");
  std::array<float, 3> result;
  for (size_t i = 0; i < 3; ++i) {
    if (!values[i].is_number())
      throw std::runtime_error("RGB must be numeric");
    result[i] = values[i].get<float>();
    if (!std::isfinite(result[i]) || result[i] < 0.0F || result[i] > 1.0F)
      throw std::runtime_error("RGB must be finite and within 0..1");
  }
  return result;
}
using KeyDirection = Fast::Renderer3ds::PicaEnvironmentKeyDirection;
KeyDirection DecodeKeyDirection(const std::string &value) {
  if (value == "authored")
    return KeyDirection::Authored;
  if (value == "scene_main")
    return KeyDirection::SceneMain;
  if (value == "scene_ambient")
    return KeyDirection::SceneAmbient;
  throw std::runtime_error("Unknown key light direction: " + value);
}
const char *EncodeKeyDirection(KeyDirection value) {
  switch (value) {
  case KeyDirection::SceneMain:
    return "scene_main";
  case KeyDirection::SceneAmbient:
    return "scene_ambient";
  case KeyDirection::Authored:
    break;
  }
  return "authored";
}
SceneEnvironmentProfile Decode(const Json &doc, uint16_t scene) {
  if (doc.at("schema") != "triaevum.scene-environment" ||
      (doc.at("version") != 1 && doc.at("version") != 2 &&
       doc.at("version") != 3 && doc.at("version") != 4 &&
       doc.at("version") != 5) ||
      doc.at("sceneId") != scene)
    throw std::runtime_error(
        "Environment schema, version or scene identity mismatch");
  SceneEnvironmentProfile result;
  result.Enabled = doc.at("enabled").get<bool>();
  // Legacy authored profiles retain their explicit black/ambient choices.
  // Loading never rewrites them; new profiles use the scene reference.
  result.PreserveSceneBrightness = doc.at("version") >= 4
      ? doc.at("preserveSceneBrightness").get<bool>() : false;
  result.NativeDirectStrength = Scalar(doc, "nativeDirectStrength", 0.0F, 2.0F);
  // Older profiles used one gain for every selected native direct source.
  result.NativeLocalStrength = doc.at("version") >= 3
      ? Scalar(doc, "nativeLocalStrength", 0.0F, 2.0F) : result.NativeDirectStrength;
  result.NativeAmbientStrength =
      Scalar(doc, "nativeAmbientStrength", 0.0F, 2.0F);
  result.AmbientColor = Color(doc, "ambientColor");
  result.AmbientIntensity = Scalar(doc, "ambientIntensity", 0.0F, 2.0F);
  result.FogStrength = Scalar(doc, "fogStrength", 0.0F, 1.0F);
  const auto &lights = doc.at("directionalLights");
  if (!lights.is_array() || lights.size() != result.Lights.size())
    throw std::runtime_error(
        "Environment requires a key light and a fill light");
  for (size_t i = 0; i < result.Lights.size(); ++i) {
    auto &light = result.Lights[i];
    light.Azimuth = Scalar(lights[i], "azimuth", -180.0F, 180.0F);
    light.Elevation = Scalar(lights[i], "elevation", -90.0F, 90.0F);
    light.Intensity = Scalar(lights[i], "intensity", 0.0F, 4.0F);
    light.Color = Color(lights[i], "color");
  }
  if (doc.at("version") >= 5) {
    const auto &key = doc.at("keyLightDirection");
    result.KeyDirection = DecodeKeyDirection(key.get<std::string>());
    result.KeyAzimuthOffset = Scalar(doc, "keyLightAzimuthOffset", -180.0F, 180.0F);
    result.KeyElevationOffset = Scalar(doc, "keyLightElevationOffset", -90.0F, 90.0F);
  }
  if (doc.at("version") >= 2) {
    const auto &points = doc.at("pointLights");
    if (!points.is_array() || points.size() != result.PointLights.size())
      throw std::runtime_error("Environment requires Navi and fairy point-light groups");
    for (size_t i = 0; i < result.PointLights.size(); ++i) {
      auto &light = result.PointLights[i];
      light.Enabled = points[i].at("enabled").get<bool>();
      light.Intensity = Scalar(points[i], "intensity", 0.0F, 8.0F);
      light.RadiusScale = Scalar(points[i], "radiusScale", 0.1F, 8.0F);
      light.ColorTint = Color(points[i], "colorTint");
    }
  }
  return result;
}
Json Encode(const SceneEnvironmentProfile &value, uint16_t scene) {
  Json lights = Json::array();
  for (const auto &light : value.Lights)
    lights.push_back({{"azimuth", light.Azimuth},
                      {"elevation", light.Elevation},
                      {"color", light.Color},
                      {"intensity", light.Intensity}});
  Json points = Json::array();
  for (const auto &light : value.PointLights)
    points.push_back({{"enabled", light.Enabled},
                      {"intensity", light.Intensity},
                      {"radiusScale", light.RadiusScale},
                      {"colorTint", light.ColorTint}});
  return {{"schema", "triaevum.scene-environment"},
          {"version", 5},
          {"sceneId", scene},
          {"enabled", value.Enabled},
          {"preserveSceneBrightness", value.PreserveSceneBrightness},
          {"nativeDirectStrength", value.NativeDirectStrength},
          {"nativeLocalStrength", value.NativeLocalStrength},
          {"nativeAmbientStrength", value.NativeAmbientStrength},
          {"ambientColor", value.AmbientColor},
          {"ambientIntensity", value.AmbientIntensity},
          {"fogStrength", value.FogStrength},
          {"pointLights", std::move(points)},
          {"directionalLights", std::move(lights)},
          {"keyLightDirection", EncodeKeyDirection(value.KeyDirection)},
          {"keyLightAzimuthOffset", value.KeyAzimuthOffset},
          {"keyLightElevationOffset", value.KeyElevationOffset}};
}
} // namespace

SceneEnvironmentProfile BaseSceneEnvironmentProfile() {
  // Captured 2026-10-06 from the Kokiri play configuration's title intro
  // profile (scene-006B.environment.json).
  SceneEnvironmentProfile base;
  base.Enabled = true;
  base.PreserveSceneBrightness = false;
  base.NativeDirectStrength = 0.0F;
  base.NativeLocalStrength = 0.0F;
  base.NativeAmbientStrength = 0.6F;
  base.AmbientColor = {1.0F, 1.0F, 1.0F};
  base.AmbientIntensity = 0.0F;
  base.FogStrength = 1.0F;
  base.Lights = {{{45.0F, 45.0F, {1.0F, 1.0F, 1.0F}, 1.0F},
                  {-135.0F, 30.0F, {1.0F, 1.0F, 1.0F}, 0.0F}}};
  base.KeyDirection = KeyDirection::SceneMain;
  base.KeyAzimuthOffset = 0.0F;
  base.KeyElevationOffset = 0.0F;
  base.PointLights = {};
  return base;
}

SceneEnvironmentRuntime::SceneEnvironmentRuntime(
    std::filesystem::path directory, Publisher publish)
    : mDirectory(std::move(directory)), mPublish(std::move(publish)) {}

void SceneEnvironmentRuntime::ObserveScene(std::optional<uint16_t> scene) {
  std::scoped_lock lock(mMutex);
  if (mState.SceneId == scene)
    return;
  mState = {};
  mSaved = {};
  mState.SceneId = scene;
  if (scene) {
    char name[40];
    std::snprintf(name, sizeof(name), "scene-%04X.environment.json",
                  unsigned(*scene));
    mState.Path = mDirectory / name;
    ReloadLocked();
  }
  PublishLocked();
}
SceneEnvironmentSnapshot SceneEnvironmentRuntime::Snapshot() const {
  std::scoped_lock lock(mMutex);
  return mState;
}
void SceneEnvironmentRuntime::Preview(SceneEnvironmentProfile profile) {
  std::scoped_lock lock(mMutex);
  if (!mState.SceneId)
    return;
  // Validate callers as well as files, without publishing partial state.
  try {
    Validate(profile);
    mState.Profile = profile;
    mState.Dirty = mState.Profile != mSaved;
    PublishLocked();
  } catch (const std::exception &error) {
    mState.Error = error.what();
  }
}
bool SceneEnvironmentRuntime::Save() {
  std::scoped_lock lock(mMutex);
  if (!mState.SceneId)
    return false;
  std::error_code error;
  std::filesystem::create_directories(mDirectory, error);
  if (error) {
    mState.Error = error.message();
    return false;
  }
  const auto saved = Ship::WriteConfigFileAtomically(
      mState.Path, Encode(mState.Profile, *mState.SceneId).dump(2));
  mState.Error = saved.Error;
  if (saved.Success) {
    mSaved = mState.Profile;
    mState.Dirty = false;
    mState.FileExists = true;
  }
  return saved.Success;
}
bool SceneEnvironmentRuntime::Reload() {
  std::scoped_lock lock(mMutex);
  const bool loaded = ReloadLocked();
  if (loaded)
    PublishLocked();
  return loaded;
}
bool SceneEnvironmentRuntime::ReloadLocked() {
  if (!mState.SceneId)
    return false;
  try {
    // A scene without its own profile takes the base environment.
    auto candidate = BaseSceneEnvironmentProfile();
    const bool exists = std::filesystem::exists(mState.Path);
    if (exists) {
      if (std::filesystem::file_size(mState.Path) > 65536)
        throw std::runtime_error("Environment profile exceeds 64 KiB");
      std::ifstream file(mState.Path);
      candidate = Decode(Json::parse(file), *mState.SceneId);
    }
    mSaved = mState.Profile = candidate;
    mState.FileExists = exists;
    mState.Dirty = false;
    mState.Error.clear();
    return true;
  } catch (const std::exception &error) {
    mState.Error = error.what();
    return false;
  }
}
void SceneEnvironmentRuntime::PublishLocked() {
  Fast::Renderer3ds::PicaEnvironmentLighting result;
  if (mState.SceneId) {
    const auto &value = mState.Profile;
    result.ScopeIdentity = uint64_t(*mState.SceneId) + 1;
    result.Enabled = value.Enabled;
    result.PreserveSceneBrightness = value.PreserveSceneBrightness;
    result.NativeDirectStrength = value.NativeDirectStrength;
    result.NativeLocalStrength = value.NativeLocalStrength;
    result.NativeAmbientStrength = value.NativeAmbientStrength;
    result.AmbientColor = value.AmbientColor;
    result.AmbientIntensity = value.AmbientIntensity;
    result.FogStrength = value.FogStrength;
    result.PointLights = value.PointLights;
    result.KeyDirection = value.KeyDirection;
    result.KeyAzimuthOffset =
        value.KeyAzimuthOffset * std::numbers::pi_v<float> / 180.0F;
    result.KeyElevationOffset =
        value.KeyElevationOffset * std::numbers::pi_v<float> / 180.0F;
    // The same axes as the authored azimuth/elevation below.
    result.WorldUp = {0.0F, 1.0F, 0.0F};
    result.WorldAzimuthZero = {0.0F, 0.0F, 1.0F};
    for (size_t i = 0; i < value.Lights.size(); ++i) {
      const auto &light = value.Lights[i];
      const float azimuth = light.Azimuth * std::numbers::pi_v<float> / 180.0F;
      const float elevation =
          light.Elevation * std::numbers::pi_v<float> / 180.0F;
      // OOT3D world is Y-up. Zero azimuth points toward +Z; +90 toward +X.
      result.Lights[i] = {{std::sin(azimuth) * std::cos(elevation),
                           std::sin(elevation),
                           std::cos(azimuth) * std::cos(elevation)},
                          light.Color,
                          light.Intensity};
    }
  }
  mPublish(result);
}
} // namespace Oot3dNativeGame
