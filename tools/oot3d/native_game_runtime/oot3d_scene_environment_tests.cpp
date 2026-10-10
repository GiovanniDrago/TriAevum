// Scene environment runtime with developer hot reload: files changed on disk
// while the game runs (built with TRIAEVUM_DEVELOPER_TOOLS).
#include "oot3d_scene_environment.h"
#include "oot3d_scene_environment_codec.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace Oot3dNativeGame;
int gChecks = 0;

void Check(bool condition, const char *what) {
  ++gChecks;
  if (!condition) throw std::runtime_error(what);
}

void Write(const std::filesystem::path &path, const std::string &text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
}

std::string Profile(float ambient, bool night) {
  auto profile = BaseSceneEnvironmentProfile();
  profile.AmbientIntensity = ambient;
  return FormatSceneEnvironment(EncodeSceneEnvironment(profile, 0x55, night));
}

struct Fixture {
  std::filesystem::path Directory;
  std::filesystem::path Day, Night;
  SceneEnvironmentRuntime Runtime;
  std::chrono::steady_clock::time_point Now = std::chrono::steady_clock::now();
  int Publications = 0;

  explicit Fixture(const std::filesystem::path &directory)
      : Directory(directory), Day(directory / "scene-0055.environment.json"),
        Night(directory / "scene-0055.night.environment.json"),
        Runtime(directory, [this](const Fast::Renderer3ds::PicaEnvironmentLighting &) { ++Publications; }) {}

  // Two looks 300 ms apart: a change is acted on once they agree.
  void Settle() {
    for (int i = 0; i < 2; ++i) {
      Now += std::chrono::milliseconds(300);
      Runtime.PollProfileFiles(Now);
    }
  }
  float Ambient() const { return Runtime.Snapshot().Profile.AmbientIntensity; }
};
} // namespace

int main() try {
  const auto directory = std::filesystem::temp_directory_path() / "triaevum_environment_tests";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  Fixture f(directory);
  Write(f.Day, Profile(0.3F, false));
  f.Runtime.ObserveScene(0x55, false);
  f.Runtime.SetHotReload(true);
  Check(f.Ambient() == 0.3F, "the scene's profile was not loaded");

  // A half-written file is never applied; a whole one is.
  auto revision = f.Runtime.Snapshot().Revision;
  Write(f.Day, "{\"schema\":");
  f.Settle();
  Check(f.Ambient() == 0.3F && !f.Runtime.Snapshot().Error.empty() && f.Runtime.Snapshot().Revision == revision,
        "an invalid file replaced the profile in use");
  Write(f.Day, Profile(0.65F, false));
  f.Settle();
  Check(f.Ambient() == 0.65F && f.Runtime.Snapshot().Error.empty() && f.Runtime.Snapshot().Revision > revision,
        "a valid file was not applied");

  // The same content written again is no change.
  revision = f.Runtime.Snapshot().Revision;
  const int publications = f.Publications;
  Write(f.Day, Profile(0.65F, false));
  f.Settle();
  Check(f.Runtime.Snapshot().Revision == revision && f.Publications == publications,
        "an unchanged file was applied again");

  // One look is not enough: a file still changing is not read.
  Write(f.Day, Profile(0.7F, false));
  f.Now += std::chrono::milliseconds(300);
  f.Runtime.PollProfileFiles(f.Now);
  Check(f.Ambient() == 0.65F, "a change was applied before two looks agreed");
  f.Now += std::chrono::milliseconds(300);
  f.Runtime.PollProfileFiles(f.Now);
  Check(f.Ambient() == 0.7F, "a settled change was not applied");

  // Unsaved edits are kept; an explicit reload takes the file.
  auto edited = f.Runtime.Snapshot().Profile;
  edited.AmbientIntensity = 1.25F;
  Check(f.Runtime.Preview(edited, f.Runtime.Snapshot().Revision) && f.Runtime.Snapshot().Dirty,
        "an edit on the current revision was refused");
  Write(f.Day, Profile(0.45F, false));
  f.Settle();
  Check(f.Ambient() == 1.25F && f.Runtime.Snapshot().Dirty, "a file on disk replaced unsaved edits");
  Check(f.Runtime.Reload() && f.Ambient() == 0.45F && !f.Runtime.Snapshot().Dirty,
        "reload did not take the file that changed during the edit");

  // An edit made on a replaced profile is refused.
  const auto stale = f.Runtime.Snapshot();
  Write(f.Day, Profile(0.555F, false));
  f.Settle();
  edited = stale.Profile;
  edited.AmbientIntensity = 1.5F;
  Check(!f.Runtime.Preview(edited, stale.Revision) && f.Ambient() == 0.555F,
        "an edit based on a replaced profile overwrote it");

  // The game's own save is not picked up as a change.
  edited = f.Runtime.Snapshot().Profile;
  edited.AmbientIntensity = 0.85F;
  Check(f.Runtime.Preview(edited, f.Runtime.Snapshot().Revision) && f.Runtime.Save(), "the edit was not saved");
  revision = f.Runtime.Snapshot().Revision;
  f.Settle();
  Check(f.Runtime.Snapshot().Revision == revision && f.Ambient() == 0.85F, "its own save was reloaded");

  // Night: the day file until a night file appears; an invalid night file
  // changes neither the profile nor which file a save writes.
  f.Runtime.ObserveScene(0x55, true);
  Check(!f.Runtime.Snapshot().EditingNight && f.Ambient() == 0.85F, "night without a night file left the day file");
  Write(f.Night, "{");
  f.Settle();
  Check(!f.Runtime.Snapshot().EditingNight && f.Runtime.Snapshot().Path == f.Day && f.Ambient() == 0.85F,
        "an invalid night file changed the profile or the file saved");
  Write(f.Night, Profile(0.15F, true));
  f.Settle();
  Check(f.Runtime.Snapshot().EditingNight && f.Runtime.Snapshot().Path == f.Night && f.Ambient() == 0.15F,
        "a night file created on disk was not used at night");
  std::filesystem::remove(f.Night);
  f.Settle();
  Check(!f.Runtime.Snapshot().EditingNight && f.Runtime.Snapshot().Path == f.Day && f.Ambient() == 0.85F,
        "removing the night file did not return the night to the day file");

  // A missing file keeps the profile in use.
  f.Runtime.ObserveScene(0x55, false);
  std::filesystem::remove(f.Day);
  f.Settle();
  Check(f.Ambient() == 0.85F, "a missing file replaced the profile in use");

  {
    using Faces = Fast::Renderer3ds::ShadowCasterFaces;
    Check(BaseSceneEnvironmentProfile().CasterFaces == Faces::InteriorShells, "base profile does not pass interior shells");
    auto document = EncodeSceneEnvironment(BaseSceneEnvironmentProfile(), 0x55, false);
    Check(document["shadowCasterFaces"] == "interiorShells" && !document.contains("shadowCasterCulling"),
          "new enum was not encoded canonically");
    document.erase("shadowCasterFaces");
    const auto older = DecodeSceneEnvironment(document);
    Check(older.Profile && older.Profile->CasterFaces == Faces::Native, "missing legacy key lost native culling");
    for (bool enabled : {true, false}) {
      document["shadowCasterCulling"] = enabled;
      const auto decoded = DecodeSceneEnvironment(document);
      Check(decoded.Profile && decoded.Profile->CasterFaces == (enabled ? Faces::Native : Faces::BothFaces),
            "legacy boolean policy changed");
    }
    for (auto mode : {Faces::Native, Faces::BothFaces, Faces::InteriorShells}) {
      auto profile = BaseSceneEnvironmentProfile(); profile.CasterFaces = mode;
      auto encoded = EncodeSceneEnvironment(profile, 0x55, false);
      encoded["shadowCasterCulling"] = true; // explicit new setting takes precedence
      const auto decoded = DecodeSceneEnvironment(encoded);
      Check(decoded.Profile && decoded.Profile->CasterFaces == mode, "caster faces did not round trip");
      Check(ResolveSceneEnvironment(profile, 0x55, false).CasterFaces == mode, "renderer lost caster faces");
    }
    for (const auto& invalid : {nlohmann::json(1), nlohmann::json(true), nlohmann::json("bad"), nlohmann::json(nullptr)}) {
      document["shadowCasterFaces"] = invalid;
      Check(!DecodeSceneEnvironment(document).Errors.empty(), "invalid enum accepted");
    }
    document.erase("shadowCasterFaces"); document["shadowCasterCulling"] = 1;
    Check(!DecodeSceneEnvironment(document).Errors.empty(), "non-boolean legacy setting accepted");
  }

  {
    using Scope = Fast::Renderer3ds::PicaRimLightScope;
    auto profile = BaseSceneEnvironmentProfile();
    Check(profile.Rim.Enabled && profile.Rim.Scope == Scope::Actors && profile.Rim.Intensity == 1.0F &&
              profile.Rim.Sharpness == 4.0F && profile.Rim.Source == Fast::Renderer3ds::PicaRimLightSource::Independent &&
              profile.Rim.Color == (std::array<float, 3>{1, 0.96F, 0.9F}) && profile.Rim.Azimuth == 0 &&
              profile.Rim.Coverage == 0.25F, "base rim defaults changed");
    auto document = EncodeSceneEnvironment(profile, 0x55, false);
    const auto decoded = DecodeSceneEnvironment(document);
    Check(decoded.Profile && decoded.Profile->Rim == profile.Rim, "rim did not round trip");
    Check(ResolveSceneEnvironment(profile, 0x55, false).Rim == profile.Rim, "renderer lost rim settings");
    auto round27 = document;
    for (const auto* key : {"source", "color", "azimuth", "coverage"}) round27["rimLight"].erase(key);
    round27["rimLight"]["ambient"] = 0.05;
    round27["rimLight"]["intensity"] = 0.35;
    const auto migrated = DecodeSceneEnvironment(round27);
    Check(migrated.Profile && migrated.Profile->Rim.Source == Fast::Renderer3ds::PicaRimLightSource::Independent &&
              migrated.Profile->Rim.Color == profile.Rim.Color && migrated.Profile->Rim.Coverage == 0.25F &&
              migrated.Profile->Rim.Azimuth == 0 && migrated.Profile->Rim.Intensity == 0.35F,
          "round-27 rim did not acquire independent defaults or lost its saved intensity");
    Check(!EncodeSceneEnvironment(*migrated.Profile, 0x55, false)["rimLight"].contains("ambient"),
          "retired ambient control was saved again");
    for (const auto* key : {"source", "color", "azimuth", "coverage"}) {
      auto optional = document; optional["rimLight"].erase(key);
      Check(DecodeSceneEnvironment(optional).Profile->Rim == profile.Rim, "optional rim member lost its default");
      optional = document; optional["rimLight"][key] = nullptr;
      Check(!DecodeSceneEnvironment(optional).Profile, "null optional rim member accepted");
    }
    document.erase("rimLight");
    const auto legacy = DecodeSceneEnvironment(document);
    Check(legacy.Profile && !legacy.Profile->Rim.Enabled, "legacy profile acquired a rim");
    profile.Rim.Scope = Scope::ActorsAndWorld;
    profile.Rim.Source = Fast::Renderer3ds::PicaRimLightSource::SceneLights;
    profile.Rim.Color = {0.4F, 0.5F, 0.8F};
    profile.Rim.Azimuth = -90;
    profile.Rim.Coverage = 0.75F;
    Check(DecodeSceneEnvironment(EncodeSceneEnvironment(profile, 0x55, true)).Profile->Rim == profile.Rim,
          "world scope or night rim did not round trip");
    document = EncodeSceneEnvironment(profile, 0x55, false);
    for (const auto& invalid : {nlohmann::json(1), nlohmann::json(nullptr), nlohmann::json("on")}) {
      auto bad = document; bad["rimLight"] = invalid;
      Check(!DecodeSceneEnvironment(bad).Profile, "mistyped rim accepted");
    }
    for (const auto& key : {"enabled", "intensity", "sharpness", "scope"}) {
      auto bad = document; bad["rimLight"].erase(key);
      const auto result = DecodeSceneEnvironment(bad);
      Check(!result.Profile && result.Errors.front().Pointer == std::string("/rimLight/") + key,
            "missing rim member not diagnosed at owning boundary");
      bad = document; bad["rimLight"][key] = nlohmann::json::array();
      Check(!DecodeSceneEnvironment(bad).Profile, "mistyped rim member accepted");
    }
    for (const auto& [key, badValue] : std::vector<std::pair<std::string, nlohmann::json>>{
        {"enabled", 1}, {"intensity", 8.01}, {"intensity", -0.1}, {"sharpness", 0}, {"sharpness", 17},
        {"ambient", 0.26}, {"scope", "all"}, {"shaprness", 4}, {"source", "moon"},
        {"source", 1}, {"color", nlohmann::json::array({1, 0})}, {"color", nlohmann::json::array({1, 1.1, 0})},
        {"color", nlohmann::json::array({1, "white", 0})}, {"azimuth", 181}, {"azimuth", -181},
        {"azimuth", false}, {"coverage", -0.1}, {"coverage", 1.1}, {"coverage", "wide"}}) {
      auto bad = document; bad["rimLight"][key] = badValue;
      Check(!DecodeSceneEnvironment(bad).Profile, "invalid rim value accepted");
    }
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
      auto bad = profile; bad.Rim.Intensity = invalid;
      Check(!ValidateSceneEnvironment(bad).empty(), "nonfinite direct rim accepted");
      bad = profile; bad.Rim.Sharpness = invalid;
      Check(!ValidateSceneEnvironment(bad).empty(), "nonfinite rim exponent accepted");
      bad = profile; bad.Rim.Coverage = invalid;
      Check(!ValidateSceneEnvironment(bad).empty(), "nonfinite rim coverage accepted");
      bad = profile; bad.Rim.Azimuth = invalid;
      Check(!ValidateSceneEnvironment(bad).empty(), "nonfinite rim azimuth accepted");
      bad = profile; bad.Rim.Color[2] = invalid;
      Check(!ValidateSceneEnvironment(bad).empty(), "nonfinite rim color accepted");
      auto badJson = document; badJson["rimLight"]["intensity"] = invalid;
      Check(!DecodeSceneEnvironment(badJson).Profile, "nonfinite JSON rim accepted");
    }
    Check(f.Runtime.Preview(profile) && f.Runtime.Save() && f.Runtime.Reload() &&
              f.Runtime.Snapshot().Profile.Rim == profile.Rim, "runtime did not save/reload rim");
    const auto previous = f.Runtime.Snapshot();
    profile.Rim.Intensity = 9.0F;
    Check(!f.Runtime.Preview(profile) && f.Runtime.Snapshot().Profile == previous.Profile,
          "invalid rim preview replaced the accepted profile");
  }
  std::filesystem::remove_all(directory);
  std::printf("scene environment tests passed: %d checks\n", gChecks);
  return 0;
} catch (const std::exception &error) {
  std::fprintf(stderr, "scene environment tests FAILED: %s\n", error.what());
  return 1;
}
