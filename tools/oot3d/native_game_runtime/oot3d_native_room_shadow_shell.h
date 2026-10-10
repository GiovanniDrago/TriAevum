#pragma once

#include "fast/renderer3ds/room_shadow_shell.h"
#include "oot3d_native_static_emitters.h"
#include <optional>

namespace Oot3dNativeGame {

// Bounded version-6 CMB adapter. Nothing is published on failure. Rigid bind
// transforms follow Rz*Ry*Rx*S and the parent chain. Unsupported/skinned opaque
// geometry makes replacement incomplete, so its native draws remain active.
struct NativeRoomShadowGeometry {
  std::vector<Fast::Renderer3ds::RoomShadowTriangle> Triangles;
  bool Complete = true;
};
[[nodiscard]] std::optional<NativeRoomShadowGeometry>
ParseNativeRoomShadowCmb(std::span<const uint8_t> bytes);
// Reads command 0x0A's bounded mesh entries; CMB sizes bound the only bulk
// reads. Guest memory is copied here, never accessed by the worker.
// The room loader relocates mesh+4 to an absolute guest pointer. Command
// arguments and CMB entry offsets remain relative to CommandsAddress.
enum class NativeRoomShadowSource { Installed, File };
[[nodiscard]] std::optional<std::vector<std::vector<uint8_t>>>
CaptureNativeRoomShadowCmbs(
    const NativeLoadedRoom &room, oot3d::recomp::a32::MemoryBus &memory,
    NativeRoomShadowSource source = NativeRoomShadowSource::Installed,
    std::string *error = nullptr);

class NativeRoomShadowShells {
public:
  NativeRoomShadowShells();
  ~NativeRoomShadowShells();
  NativeRoomShadowShells(const NativeRoomShadowShells &) = delete;
  NativeRoomShadowShells &operator=(const NativeRoomShadowShells &) = delete;
  // Disabled: no directory lookup, guest reads, allocations or worker start.
  [[nodiscard]] std::shared_ptr<const Fast::Renderer3ds::RoomShadowShellScene>
  Capture(bool enabled, uint64_t scope, std::span<const NativeLoadedRoom> rooms,
          oot3d::recomp::a32::MemoryBus &memory);

private:
  struct Impl;
  std::unique_ptr<Impl> mImpl;
};
} // namespace Oot3dNativeGame
