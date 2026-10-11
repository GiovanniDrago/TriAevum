#pragma once
#include <filesystem>
#include <nlohmann/json_fwd.hpp>

namespace Oot3dNativeGame {
// Offline only: no guest process, title plugin, window, or renderer device.
[[nodiscard]] nlohmann::json
PrebuildInteriorSun(const std::filesystem::path &romfs,
                    const std::filesystem::path &cacheDirectory);
[[nodiscard]] nlohmann::json ListNativeEnvironmentGroups(const std::filesystem::path& romfs);
int InteriorSunToolMain(int argc, char **argv);
} // namespace Oot3dNativeGame
