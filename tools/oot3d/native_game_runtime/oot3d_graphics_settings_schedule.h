#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Oot3dNativeGame {

// Developer qualification input (--graphics-settings-schedule): at a run
// frame, a partial "Graphics" block is merged onto the current settings and
// applied through the same runtime call as an F1 commit, without persisting.
// It lets deterministic runs switch effects on and off mid-run; the settings
// runtime stays the only owner of the values.
//
// { "schema": "oot3d.graphics_settings_schedule.v1",
//   "steps": [ { "frame": 300,
//                "graphics": { "Effects": { "GlobalIllumination": { "Mode": "Off" } } } } ] }
struct GraphicsSettingsScheduleStep {
  uint32_t Frame = 0;
  nlohmann::json Graphics;
};

class GraphicsSettingsSchedule {
public:
  static constexpr const char *kSchema = "oot3d.graphics_settings_schedule.v1";

  // An empty path disables the schedule. Throws on malformed documents:
  // unknown schema, missing or non-object "graphics", frames not strictly
  // increasing.
  [[nodiscard]] static GraphicsSettingsSchedule
  Parse(const nlohmann::json &document);
  [[nodiscard]] static GraphicsSettingsSchedule
  LoadFile(const std::filesystem::path &path);

  [[nodiscard]] bool Enabled() const { return !mSteps.empty(); }
  [[nodiscard]] const std::vector<GraphicsSettingsScheduleStep> &
  Steps() const {
    return mSteps;
  }
  // The steps due at `runFrame`, each returned once, in order.
  [[nodiscard]] std::vector<const GraphicsSettingsScheduleStep *>
  TakeDue(uint32_t runFrame);

private:
  std::vector<GraphicsSettingsScheduleStep> mSteps;
  size_t mNext = 0;
};

// Merges a step onto the serialized current block (JSON merge patch).
[[nodiscard]] nlohmann::json
MergeScheduledGraphics(nlohmann::json current, const nlohmann::json &patch);

} // namespace Oot3dNativeGame
