#pragma once

#include "oot3d_netplay.h"

#include <cstdint>
#include <optional>

namespace Oot3dNativeGame {

class NativeA32Process;

// Spawns a second, guest-owned Player actor for the remote pose. Its update
// callback is replaced with the title's no-op immediately after Actor_Spawn;
// the netplay runtime writes only the actor transform, so the actor cannot
// read local input, move the camera or write the save. The actor is abandoned
// and respawned after a scene change or when its guest state becomes invalid.
class NetplayRemoteActor final {
public:
  void SetRemotePose(const NetplayPose* pose);
  void BeforeGuest(NativeA32Process& process);
  void Reset();

  [[nodiscard]] std::uint32_t ActorAddress() const noexcept { return mActor; }
  [[nodiscard]] std::uint32_t PendingAddress() const noexcept { return mPending; }

private:
  bool Spawn(NativeA32Process& process, std::uint32_t play);
  bool CallActorSpawn(NativeA32Process& process, std::uint32_t play,
                      float x, float y, float z, std::int16_t yaw,
                      std::uint32_t* actor);
  bool ActorInCurrentLists(NativeA32Process& process, std::uint32_t play,
                           std::uint32_t actor) const;
  bool ActorUsable(NativeA32Process& process, std::uint32_t actor) const;
  void ApplyPose(NativeA32Process& process, std::uint32_t actor,
                 const NetplayPose& pose) const;
  void Abandon(NativeA32Process& process);

  std::optional<NetplayPose> mRemote;
  std::uint32_t mPlay = 0;
  std::uint32_t mActor = 0;
  std::uint32_t mPending = 0;
  std::uint32_t mPendingFrames = 0;
  std::uint32_t mCooldown = 0;
  std::uint16_t mScene = 0;
};

} // namespace Oot3dNativeGame
