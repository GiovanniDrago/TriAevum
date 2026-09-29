#include "oot3d_graphics_settings_schedule.h"

#include <fstream>
#include <stdexcept>

namespace Oot3dNativeGame {

GraphicsSettingsSchedule
GraphicsSettingsSchedule::Parse(const nlohmann::json &document) {
  if (!document.is_object() || !document.contains("schema") ||
      document.at("schema") != kSchema)
    throw std::runtime_error(std::string("graphics settings schedule schema must be ") + kSchema);
  const auto steps = document.find("steps");
  if (steps == document.end() || !steps->is_array())
    throw std::runtime_error("graphics settings schedule requires a steps array");
  GraphicsSettingsSchedule schedule;
  for (const auto &step : *steps) {
    if (!step.is_object() || !step.contains("frame") ||
        !step.at("frame").is_number_unsigned() || !step.contains("graphics") ||
        !step.at("graphics").is_object())
      throw std::runtime_error(
          "graphics settings schedule steps need an unsigned frame and a graphics object");
    const auto frame = step.at("frame").get<uint64_t>();
    if (frame > UINT32_MAX ||
        (!schedule.mSteps.empty() && frame <= schedule.mSteps.back().Frame))
      throw std::runtime_error("graphics settings schedule frames must increase");
    schedule.mSteps.push_back({static_cast<uint32_t>(frame), step.at("graphics")});
  }
  return schedule;
}

GraphicsSettingsSchedule
GraphicsSettingsSchedule::LoadFile(const std::filesystem::path &path) {
  if (path.empty()) return {};
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("cannot open graphics settings schedule " + path.string());
  return Parse(nlohmann::json::parse(input));
}

std::vector<const GraphicsSettingsScheduleStep *>
GraphicsSettingsSchedule::TakeDue(uint32_t runFrame) {
  std::vector<const GraphicsSettingsScheduleStep *> due;
  while (mNext < mSteps.size() && mSteps[mNext].Frame <= runFrame)
    due.push_back(&mSteps[mNext++]);
  return due;
}

nlohmann::json MergeScheduledGraphics(nlohmann::json current,
                                      const nlohmann::json &patch) {
  current.merge_patch(patch);
  return current;
}

} // namespace Oot3dNativeGame
