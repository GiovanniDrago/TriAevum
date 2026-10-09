#pragma once

// The architecture's own light: emitters found in the room geometry by the
// installer (tools/triaevum_release/scene_lights.py over the room CMBs:
// torches drawn in the room mesh, lava and fire surfaces, windows and lamps,
// light shafts, magic glows), published as scene point lights of the rooms
// the title has loaded. ROM-derived data, kept with the user's other
// generated files; without a catalog nothing changes.

#include "fast/renderer3ds/pica_light_binding.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
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

// A room file's identity: FNV-1a 64 of the first bytes of its command list
// (the file from offset 0x10, loaded verbatim), which tells a scene's
// standard and Master Quest rooms apart.
inline constexpr uint32_t kNativeRoomFingerprintBytes = 128U;
[[nodiscard]] uint64_t NativeRoomFingerprint(std::span<const uint8_t> commands) noexcept;

class NativeStaticEmitterCatalog {
public:
    // A triaevum.scene-lights version 1 catalog.
    [[nodiscard]] static std::shared_ptr<const NativeStaticEmitterCatalog> Parse(std::string_view text,
                                                                                 std::string* error = nullptr);
    [[nodiscard]] static std::shared_ptr<const NativeStaticEmitterCatalog> LoadFile(
        const std::filesystem::path& path, std::string* error = nullptr);
    // TRIAEVUM_SCENE_LIGHT_CATALOG names the catalog (the launcher's
    // --scene-light-catalog); none: no catalog.
    [[nodiscard]] static std::shared_ptr<const NativeStaticEmitterCatalog> FromEnvironment();
    // The emitters of the room file with this fingerprint; none for a file
    // the catalog does not know.
    [[nodiscard]] std::span<const NativeStaticEmitter> Room(uint16_t scene, int room, uint64_t fingerprint) const;
    [[nodiscard]] size_t Count() const noexcept { return mCount; }

private:
    std::map<std::tuple<uint16_t, int, uint64_t>, std::vector<NativeStaticEmitter>> mRooms;
    size_t mCount = 0;
};

struct NativeLoadedRoom {
    int Number = -1;
    uint64_t Fingerprint = 0;
    bool operator==(const NativeLoadedRoom&) const = default;
};

// The rooms the title has loaded (EUR PlayState: current room s8 +0x4C30 with
// its command list +0x4FFC, previous room s8 +0x500C with +0x53D8; a room is
// loaded once its commands are installed) and their files' fingerprints.
[[nodiscard]] std::vector<NativeLoadedRoom> NativeLoadedRooms(uint32_t playState,
                                                              oot3d::recomp::a32::MemoryBus& memory);

// Appends the lights of the loaded rooms of the scene (group 3). Identities
// are synthetic (0xA5 in the top byte, then scene, room and index): no guest
// address can take them.
void CaptureStaticEmitters(const NativeStaticEmitterCatalog& catalog, uint16_t scene,
                           std::span<const NativeLoadedRoom> rooms,
                           std::vector<Fast::Renderer3ds::PicaScenePointLight>& lights);

} // namespace Oot3dNativeGame
