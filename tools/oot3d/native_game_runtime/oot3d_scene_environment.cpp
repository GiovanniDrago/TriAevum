#include "oot3d_scene_environment.h"
#include "oot3d_scene_environment_codec.h"
#include "oot3d_light_gizmo.h"
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
using KeyDirection = Fast::Renderer3ds::PicaEnvironmentKeyDirection;
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
  mFileSignature = FileSignatureLocked();
  PublishLocked();
}
void SceneEnvironmentRuntime::EditLightGizmo(uint32_t selectedLight, bool enabled) {
  std::scoped_lock lock(mMutex);
  mGizmoSelected = selectedLight;
  mGizmoEnabled = enabled;
  mGizmoEdited = std::chrono::steady_clock::now();
}

void SceneEnvironmentRuntime::PublishLightGizmo(const std::optional<std::array<float, 3>> &eye,
                                                bool forced) {
  std::optional<Fast::Oot3d::WorldDebugLineSet> lines;
  std::scoped_lock lock(mMutex);
  const bool editing =
      std::chrono::steady_clock::now() - mGizmoEdited < std::chrono::milliseconds(500);
  if (mGizmoEnabled && (editing || forced) && mState.SceneId && eye &&
      !mState.Profile.PlacedLights.empty())
    lines = BuildPlacedLightGizmo(mState.Profile.PlacedLights, mGizmoSelected, *eye);
  if (lines) {
    Fast::Oot3d::WorldDebugLineRuntime::Instance().Publish(std::move(*lines));
    mGizmoPublished = true;
  } else if (mGizmoPublished) {
    Fast::Oot3d::WorldDebugLineRuntime::Instance().Clear();
    mGizmoPublished = false;
  }
}

void SceneEnvironmentRuntime::SetHotReload(bool enabled) {
  std::scoped_lock lock(mMutex);
  mHotReload = enabled;
  mFileSignature = FileSignatureLocked();
}

std::string SceneEnvironmentRuntime::FileSignatureLocked() const {
  if (!mState.SceneId)
    return {};
  std::string signature;
  for (const bool night : {false, true}) {
    std::error_code error;
    const auto path = ProfilePathLocked(night);
    const auto size = std::filesystem::file_size(path, error);
    const auto time = error ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(path, error);
    signature += error ? std::string("-|") : std::to_string(size) + ":" +
                                                 std::to_string(time.time_since_epoch().count()) + "|";
  }
  return signature;
}

void SceneEnvironmentRuntime::PollProfileFiles() {
  std::scoped_lock lock(mMutex);
  const auto now = std::chrono::steady_clock::now();
  if (!mHotReload || !mState.SceneId || now - mLastFilePoll < std::chrono::milliseconds(250))
    return;
  mLastFilePoll = now;
  auto signature = FileSignatureLocked();
  if (signature == mFileSignature)
    return;
  mFileSignature = std::move(signature);
  const auto report = [&](const char *result, const std::string &detail) {
    std::fprintf(stderr, "TRIAEVUM_ENVIRONMENT_RELOAD scene=0x%04X time=%s result=%s file=%s%s%s\n",
                 unsigned(*mState.SceneId), mState.EditingNight ? "night" : "day", result,
                 mState.Path.filename().string().c_str(), detail.empty() ? "" : " detail=", detail.c_str());
  };
  if (mState.Dirty) {
    mState.Error = "The profile changed on disk; the unsaved edits here are kept (save or reload).";
    report("kept-unsaved", "");
    return;
  }
  // A night file created or removed changes which file the night uses.
  std::error_code error;
  mState.NightProfileExists = std::filesystem::exists(ProfilePathLocked(true), error);
  const bool editingNight = mState.Night && mState.NightProfileExists;
  if (editingNight != mState.EditingNight) {
    mState.EditingNight = editingNight;
    mState.Path = ProfilePathLocked(editingNight);
  }
  if (!std::filesystem::exists(mState.Path, error)) {
    report("kept-missing", "");
    return;
  }
  if (!ReloadLocked()) {
    report("kept-invalid", mState.Error);
    return;
  }
  PublishLocked();
  report("applied", "");
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
  if (const auto errors = ValidateSceneEnvironment(profile); !errors.empty()) {
    mState.Error = DescribeSceneEnvironmentErrors(errors);
    return;
  }
  mState.Profile = profile;
  mState.Dirty = mState.Profile != mSaved;
  PublishLocked();
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
      FormatSceneEnvironment(EncodeSceneEnvironment(mState.Profile, *mState.SceneId, mState.EditingNight)));
  mState.Error = saved.Error;
  if (saved.Success) {
    mSaved = mState.Profile;
    mState.Dirty = false;
    mState.FileExists = true;
  }
  // Its own writes are not changes to pick up.
  mFileSignature = FileSignatureLocked();
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
      path, FormatSceneEnvironment(EncodeSceneEnvironment(mState.Profile, *mState.SceneId, true)));
  mState.Error = saved.Error;
  mFileSignature = FileSignatureLocked();
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
  mFileSignature = FileSignatureLocked();
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
      auto document = DecodeSceneEnvironment(Json::parse(file), *mState.SceneId, mState.EditingNight);
      if (!document.Profile)
        throw std::runtime_error(DescribeSceneEnvironmentErrors(document.Errors));
      candidate = std::move(*document.Profile);
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
  mPublish(mState.SceneId
               ? ResolveSceneEnvironment(mState.Profile, *mState.SceneId, mState.EditingNight)
               : Fast::Renderer3ds::PicaEnvironmentLighting{});
}
} // namespace Oot3dNativeGame
