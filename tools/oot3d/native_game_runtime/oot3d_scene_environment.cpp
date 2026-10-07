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
constexpr size_t kPlacedLightNameLength = 64U;
constexpr float kPlacedLightMaximumIntensity = 16.0F;
constexpr float kPlacedLightMaximumCoordinate = 100000.0F;
constexpr float kPlacedLightMaximumSize = 20000.0F;
constexpr const char *kPlacedLightTypeNames[] = {"sun", "spot", "point", "surface", "omni"};
constexpr const char *kPlacedLightShapeNames[] = {"rectangle", "disc"};
template <class Enum, size_t Count>
Enum DecodeName(const Json &object, const char *key, const char *const (&names)[Count]) {
  const auto value = object.at(key).get<std::string>();
  for (size_t i = 0; i < Count; ++i)
    if (value == names[i])
      return static_cast<Enum>(i);
  throw std::runtime_error(std::string("Unknown ") + key + ": " + value);
}
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
  if (profile.PlacedLights.size() > kMaximumPlacedLights)
    throw std::runtime_error("Too many placed lights");
  for (size_t i = 0; i < profile.PlacedLights.size(); ++i) {
    const auto &light = profile.PlacedLights[i];
    if (light.Id == 0 || light.Name.size() > kPlacedLightNameLength ||
        light.Type > PlacedLightType::Omni || light.Shape > PlacedLightShape::Disc)
      throw std::runtime_error("Placed light identity, name, type or shape is invalid");
    for (size_t j = 0; j < i; ++j)
      if (profile.PlacedLights[j].Id == light.Id)
        throw std::runtime_error("Placed lights share an identity");
    for (float value : light.Color)
      scalar(value, 0.0F, 1.0F);
    scalar(light.Intensity, 0.0F, kPlacedLightMaximumIntensity);
    for (float value : light.Position)
      scalar(value, -kPlacedLightMaximumCoordinate, kPlacedLightMaximumCoordinate);
    scalar(light.Rotation[0], -180.0F, 180.0F);
    scalar(light.Rotation[1], -90.0F, 90.0F);
    scalar(light.Rotation[2], -180.0F, 180.0F);
    scalar(light.Radius, 1.0F, kPlacedLightMaximumSize);
    scalar(light.Width, 1.0F, kPlacedLightMaximumSize);
    scalar(light.Height, 1.0F, kPlacedLightMaximumSize);
    scalar(light.Range, 1.0F, kPlacedLightMaximumSize);
    scalar(light.BeamAngle, 1.0F, 179.0F);
    scalar(light.BeamSoftness, 0.0F, 1.0F);
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
std::array<float, 3> Vector(const Json &object, const char *key, float low, float high) {
  const auto &values = object.at(key);
  if (!values.is_array() || values.size() != 3)
    throw std::runtime_error(std::string(key) + " requires three values");
  std::array<float, 3> result;
  for (size_t i = 0; i < 3; ++i) {
    if (!values[i].is_number())
      throw std::runtime_error(std::string(key) + " must be numeric");
    result[i] = values[i].get<float>();
    if (!std::isfinite(result[i]) || result[i] < low || result[i] > high)
      throw std::runtime_error(std::string(key) + " is out of range");
  }
  return result;
}
PlacedLight DecodePlacedLight(const Json &object) {
  PlacedLight light;
  light.Id = object.at("id").get<uint32_t>();
  light.Name = object.at("name").get<std::string>();
  light.Type = DecodeName<PlacedLightType>(object, "type", kPlacedLightTypeNames);
  light.Visible = object.at("visible").get<bool>();
  light.Color = Color(object, "color");
  light.Intensity = Scalar(object, "intensity", 0.0F, kPlacedLightMaximumIntensity);
  light.Position = Vector(object, "position", -kPlacedLightMaximumCoordinate, kPlacedLightMaximumCoordinate);
  light.Rotation = Vector(object, "rotation", -180.0F, 180.0F);
  light.CastShadows = object.at("castShadows").get<bool>();
  light.Radius = Scalar(object, "radius", 1.0F, kPlacedLightMaximumSize);
  light.Width = Scalar(object, "width", 1.0F, kPlacedLightMaximumSize);
  light.Height = Scalar(object, "height", 1.0F, kPlacedLightMaximumSize);
  light.Shape = DecodeName<PlacedLightShape>(object, "shape", kPlacedLightShapeNames);
  light.TwoSided = object.at("twoSided").get<bool>();
  light.BeamAngle = Scalar(object, "beamAngle", 1.0F, 179.0F);
  light.BeamSoftness = Scalar(object, "beamSoftness", 0.0F, 1.0F);
  light.Range = Scalar(object, "range", 1.0F, kPlacedLightMaximumSize);
  return light;
}
Json EncodePlacedLight(const PlacedLight &light) {
  return {{"id", light.Id},
          {"name", light.Name},
          {"type", kPlacedLightTypeNames[static_cast<size_t>(light.Type)]},
          {"visible", light.Visible},
          {"color", light.Color},
          {"intensity", light.Intensity},
          {"position", light.Position},
          {"rotation", light.Rotation},
          {"castShadows", light.CastShadows},
          {"radius", light.Radius},
          {"width", light.Width},
          {"height", light.Height},
          {"shape", kPlacedLightShapeNames[static_cast<size_t>(light.Shape)]},
          {"twoSided", light.TwoSided},
          {"beamAngle", light.BeamAngle},
          {"beamSoftness", light.BeamSoftness},
          {"range", light.Range}};
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
SceneEnvironmentProfile Decode(const Json &doc, uint16_t scene, bool night) {
  // A night profile names its time of day; the scene's own profile does not.
  if (doc.value("timeOfDay", std::string("day")) != (night ? "night" : "day"))
    throw std::runtime_error("Environment time of day mismatch");
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
  // Optional in version 5: a profile without placed lights omits them, and
  // a runtime that predates them ignores the field.
  if (const auto placed = doc.find("placedLights"); placed != doc.end()) {
    if (!placed->is_array())
      throw std::runtime_error("placedLights must be an array");
    for (const auto &light : *placed)
      result.PlacedLights.push_back(DecodePlacedLight(light));
  }
  Validate(result);
  return result;
}
Json Encode(const SceneEnvironmentProfile &value, uint16_t scene, bool night) {
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
  Json result = {{"schema", "triaevum.scene-environment"},
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
  if (night)
    result["timeOfDay"] = "night";
  if (!value.PlacedLights.empty()) {
    Json placed = Json::array();
    for (const auto &light : value.PlacedLights)
      placed.push_back(EncodePlacedLight(light));
    result["placedLights"] = std::move(placed);
  }
  return result;
}
} // namespace

bool PlacedLightRendered(PlacedLightType type) noexcept {
  return type == PlacedLightType::Point || type == PlacedLightType::Omni;
}

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

void SceneEnvironmentRuntime::ObserveScene(std::optional<uint16_t> scene,
                                           bool night,
                                           std::optional<ScenePlayerPlacement> player) {
  std::scoped_lock lock(mMutex);
  mState.Player = scene ? player : std::nullopt;
  if (mState.SceneId == scene && mState.Night == night)
    return;
  // The time changed: only a scene with a night profile changes profile.
  if (mState.SceneId == scene && scene &&
      (night && mState.NightProfileExists) == mState.EditingNight) {
    mState.Night = night;
    return;
  }
  SelectLocked(scene, night);
  mState.Player = scene ? player : std::nullopt;
}
std::filesystem::path
SceneEnvironmentRuntime::ProfilePathLocked(bool night) const {
  char name[48];
  std::snprintf(name, sizeof(name),
                night ? "scene-%04X.night.environment.json"
                      : "scene-%04X.environment.json",
                unsigned(mState.SceneId.value_or(0)));
  return mDirectory / name;
}
void SceneEnvironmentRuntime::SelectLocked(std::optional<uint16_t> scene,
                                           bool night) {
  mState = {};
  mSaved = {};
  mState.SceneId = scene;
  mState.Night = night;
  if (scene) {
    std::error_code error;
    mState.NightProfileExists =
        std::filesystem::exists(ProfilePathLocked(true), error);
    mState.EditingNight = night && mState.NightProfileExists;
    mState.Path = ProfilePathLocked(mState.EditingNight);
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
      mState.Path,
      Encode(mState.Profile, *mState.SceneId, mState.EditingNight).dump(2));
  mState.Error = saved.Error;
  if (saved.Success) {
    mSaved = mState.Profile;
    mState.Dirty = false;
    mState.FileExists = true;
  }
  return saved.Success;
}
bool SceneEnvironmentRuntime::CreateNightProfile() {
  std::scoped_lock lock(mMutex);
  if (!mState.SceneId)
    return false;
  std::error_code error;
  std::filesystem::create_directories(mDirectory, error);
  if (error) {
    mState.Error = error.message();
    return false;
  }
  const auto path = ProfilePathLocked(true);
  const auto saved = Ship::WriteConfigFileAtomically(
      path, Encode(mState.Profile, *mState.SceneId, true).dump(2));
  mState.Error = saved.Error;
  if (!saved.Success)
    return false;
  mState.NightProfileExists = true;
  if (mState.Night && !mState.EditingNight) {
    // The values shown become the night profile in use; the scene's own
    // profile keeps what it last saved.
    mState.EditingNight = true;
    mState.Path = path;
    mSaved = mState.Profile;
    mState.Dirty = false;
    mState.FileExists = true;
    PublishLocked();
  }
  return true;
}
bool SceneEnvironmentRuntime::RemoveNightProfile() {
  std::scoped_lock lock(mMutex);
  if (!mState.SceneId || !mState.NightProfileExists)
    return false;
  std::error_code error;
  std::filesystem::remove(ProfilePathLocked(true), error);
  if (error) {
    mState.Error = error.message();
    return false;
  }
  mState.NightProfileExists = false;
  if (mState.EditingNight) {
    mState.EditingNight = false;
    mState.Path = ProfilePathLocked(false);
    ReloadLocked();
    PublishLocked();
  }
  return true;
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
      candidate = Decode(Json::parse(file), *mState.SceneId, mState.EditingNight);
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
    // Day and night profiles are separate scopes (no shared shadow history).
    result.ScopeIdentity = uint64_t(*mState.SceneId) + 1 +
                           (mState.EditingNight ? 0x10000U : 0U);
    result.Enabled = value.Enabled;
    result.PreserveSceneBrightness = value.PreserveSceneBrightness;
    result.NativeDirectStrength = value.NativeDirectStrength;
    result.NativeLocalStrength = value.NativeLocalStrength;
    result.NativeAmbientStrength = value.NativeAmbientStrength;
    result.AmbientColor = value.AmbientColor;
    result.AmbientIntensity = value.AmbientIntensity;
    result.FogStrength = value.FogStrength;
    result.PointLights = value.PointLights;
    // Placed point and omni lights; the other types are not drawn yet.
    for (const auto &light : value.PlacedLights) {
      if (!light.Visible || !PlacedLightRendered(light.Type) ||
          result.PlacedPointCount == result.PlacedPoints.size())
        continue;
      auto &point = result.PlacedPoints[result.PlacedPointCount++];
      // Unique per scene, time of day and light.
      point.SourceIdentity = 0xD1A6000000000000ULL | (uint64_t(*mState.SceneId) << 32U) |
                             (mState.EditingNight ? 0x80000000ULL : 0ULL) | light.Id;
      point.PositionWorld = light.Position;
      for (size_t channel = 0; channel < 3; ++channel)
        point.Color[channel] = light.Color[channel] * light.Intensity;
      point.Radius = light.Radius;
      point.CastShadows = light.CastShadows;
    }
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
