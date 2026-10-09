#pragma once

// The architecture's own light: emitters found offline in the room geometry
// (lighting-library emitters/static_emitters.py over the room CMBs: torches
// drawn in the room mesh, lava and fire surfaces, windows and lamps, light
// shafts, magic glows), published as scene point lights of the rooms the
// title has loaded. ROM-derived data, kept with the user's other generated
// files; without a catalog nothing changes.

#include "fast/renderer3ds/pica_light_binding.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace oot3d::recomp::a32 {
class MemoryBus;
} // namespace oot3d::recomp::a32

namespace Oot3dNativeGame {

enum class NativeStaticEmitterCategory : uint8_t { Torch, FireLava, WindowLamp, LightShaft, MagicGlow };

struct NativeStaticEmitter {
    std::array<float, 3> Position{};
    std::array<float, 3> Colour{};
    float Radius = 0.0F;
    float Gain = 0.0F;
    NativeStaticEmitterCategory Category = NativeStaticEmitterCategory::Torch;
};

class NativeStaticEmitterCatalog {
public:
    // Master Quest rooms are not distinguished at run time yet: their entries
    // are skipped.
    [[nodiscard]] static std::shared_ptr<const NativeStaticEmitterCatalog> LoadFile(
        const std::filesystem::path& path, std::string* error = nullptr);
    // TRIAEVUM_STATIC_EMITTERS names the catalog; none: no catalog.
    [[nodiscard]] static std::shared_ptr<const NativeStaticEmitterCatalog> FromEnvironment();
    [[nodiscard]] std::span<const NativeStaticEmitter> Room(uint16_t scene, int room) const;
    [[nodiscard]] size_t Count() const noexcept { return mCount; }

private:
    std::map<std::pair<uint16_t, int>, std::vector<NativeStaticEmitter>> mRooms;
    size_t mCount = 0;
};

// The rooms the title has loaded (EUR PlayState: current room s8 +0x4C30 with
// its commands +0x4FFC, previous room s8 +0x500C with +0x53D8; a room is
// loaded once its commands are installed).
[[nodiscard]] std::vector<int> NativeLoadedRooms(uint32_t playState, oot3d::recomp::a32::MemoryBus& memory);

// Appends the lights of the loaded rooms of the scene (group 3). Identities
// are synthetic (0xA5 in the top byte, then scene, room and index): no guest
// address can take them.
void CaptureStaticEmitters(const NativeStaticEmitterCatalog& catalog, uint16_t scene,
                           std::span<const int> rooms,
                           std::vector<Fast::Renderer3ds::PicaScenePointLight>& lights);

} // namespace Oot3dNativeGame
