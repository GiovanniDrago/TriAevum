#include "oot3d_graphics_settings_schedule.h"

#include <iostream>
#include <stdexcept>

namespace {

void Expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool Rejects(const char* text) {
    try {
        (void)Oot3dNativeGame::GraphicsSettingsSchedule::Parse(
            nlohmann::json::parse(text));
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    using Oot3dNativeGame::GraphicsSettingsSchedule;
    try {
        Expect(!GraphicsSettingsSchedule::LoadFile({}).Enabled(),
               "an empty path must disable the schedule");

        auto schedule = GraphicsSettingsSchedule::Parse(nlohmann::json::parse(R"({
            "schema": "oot3d.graphics_settings_schedule.v1",
            "steps": [
                {"frame": 10, "graphics": {"Effects": {"GlobalIllumination": {"Mode": "Off"}}}},
                {"frame": 20, "graphics": {"Effects": {"GlobalIllumination": {"Mode": "BrixelizerGI"}}}}
            ]})"));
        Expect(schedule.Enabled() && schedule.Steps().size() == 2U,
               "two steps expected");
        Expect(schedule.TakeDue(9).empty(), "no step is due before frame 10");
        const auto first = schedule.TakeDue(10);
        Expect(first.size() == 1U && first[0]->Frame == 10U,
               "the first step is due at frame 10");
        Expect(schedule.TakeDue(10).empty(), "a step is returned once");
        Expect(schedule.TakeDue(25).size() == 1U,
               "a late frame returns the remaining step");

        const auto merged = Oot3dNativeGame::MergeScheduledGraphics(
            nlohmann::json::parse(R"({"SchemaVersion": 14, "Effects": {
                "AmbientOcclusion": "Gtao",
                "GlobalIllumination": {"Mode": "BrixelizerGI", "Resolution": "Half"}}})"),
            first[0]->Graphics);
        Expect(merged["SchemaVersion"] == 14 &&
                   merged["Effects"]["AmbientOcclusion"] == "Gtao" &&
                   merged["Effects"]["GlobalIllumination"]["Mode"] == "Off" &&
                   merged["Effects"]["GlobalIllumination"]["Resolution"] == "Half",
               "a step changes only the fields it names");

        Expect(Rejects(R"({"schema": "other", "steps": []})"),
               "an unknown schema must be rejected");
        Expect(Rejects(R"({"schema": "oot3d.graphics_settings_schedule.v1",
                           "steps": [{"frame": 5}]})"),
               "a step without graphics must be rejected");
        Expect(Rejects(R"({"schema": "oot3d.graphics_settings_schedule.v1",
                           "steps": [{"frame": 5, "graphics": {}},
                                     {"frame": 5, "graphics": {}}]})"),
               "frames must increase");

        std::cout << "oot3d graphics settings schedule tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "oot3d graphics settings schedule tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
