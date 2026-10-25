#pragma once

#include "oot3d_netplay.h"

#include <cstdint>

namespace Oot3dNativeGame {

class NativeA32Process;

// Host-authoritative world state that can be read and applied without taking
// ownership of the guest's actor update or save file.
//
// Switches are the 64 room bits read by Flags_GetSwitch (PlayState +0x2228
// and +0x222C). Collectible flags are queried through the title's
// Flags_GetCollectible/Flags_SetCollectible helpers; the helper only sets a
// bit, so a chest that was opened on the host stays open on the client.
class NetplayWorldSync final {
public:
  [[nodiscard]] bool BeforeGuest(NativeA32Process& process, bool host,
                                 const NetplayPose* remote);
  bool CommitPendingTransition(NativeA32Process& process);
  void FillLocalPose(NativeA32Process& process, NetplayPose* local) const;
  void Reset();

private:
  bool ApplySwitchFlags(NativeA32Process& process, std::uint32_t play,
                        std::uint64_t flags) const;
  void ApplyCollectibleFlags(NativeA32Process& process, std::uint32_t play,
                             std::uint64_t flags);
  void QueryCollectibleFlags(NativeA32Process& process, std::uint32_t play);
  bool RequestTransition(NativeA32Process& process, std::uint32_t play,
                         std::int32_t entrance);

  std::uint64_t mCollectibleFlags = 0;
  std::uint64_t mAppliedCollectible = 0;
  std::uint32_t mFrame = 0;
  std::uint32_t mSceneFrames = 0;
  std::uint32_t mTransitionCooldown = 0;
  std::uint16_t mLastScene = 0;
  std::uint32_t mPlay = 0;
  std::int32_t mPendingEntrance = 0;
  std::uint16_t mPendingScene = 0;
};

} // namespace Oot3dNativeGame
