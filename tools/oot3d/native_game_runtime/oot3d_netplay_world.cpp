#include "oot3d_netplay_world.h"

#include "oot3d_native_a32_process.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace Oot3dNativeGame {
namespace {

constexpr std::uint32_t kPlayStatePointer = 0x005043D4U + 0x0CU;
constexpr std::uint32_t kPlayMode = 0x100U;
constexpr std::uint32_t kPlaySceneId = 0x104U;
constexpr std::uint32_t kPlayRoom = 0x4C30U;
constexpr std::uint32_t kPlaySwitchFlags32 = 0x2228U;
constexpr std::uint32_t kPlaySwitchFlags64 = 0x222CU;
constexpr std::uint32_t kSaveContext = 0x00587958U;
constexpr std::uint32_t kSaveEntrance = kSaveContext + 0x00U;  // s32
constexpr std::uint32_t kFlagsGetCollectible = 0x0036405CU;
constexpr std::uint32_t kFlagsSetCollectible = 0x003329D8U;
constexpr std::uint32_t kRequestTransition = 0x003348E8U;
constexpr std::uint32_t kPrepareTransitionEffect = 0x0035DA3CU;
constexpr std::uint32_t kDirectCallReturn = 0x60000000U;
constexpr std::uint32_t kCollectibleFlagCount = 64U;
constexpr std::uint32_t kCollectibleQueryInterval = 30U;
constexpr std::uint32_t kTransitionCooldownFrames = 180U;

bool TraceEnabled() {
  static const bool enabled =
      std::getenv("TRIAEVUM_NETPLAY_WORLD_TRACE") != nullptr;
  return enabled;
}

bool SceneTransitionEnabled() {
  static const bool enabled =
      std::getenv("TRIAEVUM_NETPLAY_SCENE_TRANSITION") != nullptr;
  return enabled;
}

bool ReadPlayState(NativeA32Process& process, std::uint32_t* play,
                   std::uint16_t* scene, std::uint8_t* room) {
  auto& memory = process.Memory();
  if (!memory.Read32(kPlayStatePointer, play) || *play == 0U) return false;
  std::uint8_t mode = 0U;
  return memory.Read8(*play + kPlayMode, &mode) && mode == 3U &&
         memory.Read16(*play + kPlaySceneId, scene) &&
         memory.Read8(*play + kPlayRoom, room);
}

std::uint64_t ReadSwitchFlags(NativeA32Process& process,
                              std::uint32_t play) {
  std::uint32_t low = 0U;
  std::uint32_t high = 0U;
  auto& memory = process.Memory();
  if (!memory.Read32(play + kPlaySwitchFlags32, &low) ||
      !memory.Read32(play + kPlaySwitchFlags64, &high)) {
    return 0U;
  }
  return static_cast<std::uint64_t>(low) |
         (static_cast<std::uint64_t>(high) << 32U);
}

} // namespace

void NetplayWorldSync::Reset() {
  mCollectibleFlags = 0;
  mAppliedCollectible = 0;
  mFrame = 0;
  mTransitionCooldown = 0;
  mLastScene = 0;
  mPlay = 0;
}

bool NetplayWorldSync::BeforeGuest(NativeA32Process& process, bool host,
                                   const NetplayPose* remote) {
  std::uint32_t play = 0U;
  std::uint16_t scene = 0U;
  std::uint8_t room = 0U;
  if (!ReadPlayState(process, &play, &scene, &room)) return false;
  if (play != mPlay || scene != mLastScene) {
    mPlay = play;
    mLastScene = scene;
    mAppliedCollectible = 0;
    mTransitionCooldown = 0;
    mFrame = 0;
    mSceneFrames = 0;
  }
  ++mFrame;
  if (mSceneFrames < 0xFFFFFFFFU) ++mSceneFrames;

  if (host) {
    if (mFrame % kCollectibleQueryInterval == 1U)
      QueryCollectibleFlags(process, play);
    return false;
  }

  if (remote == nullptr || remote->SceneId == 0U) return false;
  if (remote->SceneId == scene) {
    if (remote->RoomId == room) {
      ApplySwitchFlags(process, play, remote->SwitchFlags);
      ApplyCollectibleFlags(process, play, remote->CollectibleFlags);
    }
    return false;
  }

  // Automatic scene transitions are fail-closed until the native transition
  // path is validated with a live second Player actor. The guarded code is
  // retained for controlled debugging only.
  if (!SceneTransitionEnabled()) return false;

  // A transition request during scene load/name-entry can accept entrance 0
  // and leave the guest in a half-built scene. Wait until the local scene has
  // been stable for two seconds and require a real entrance.
  if (mSceneFrames < 120U) return false;
  if (mTransitionCooldown != 0U) {
    --mTransitionCooldown;
    return false;
  }
  if (remote->EntranceId <= 0 || remote->EntranceId > 0xFFFF) return false;
  mPendingEntrance = remote->EntranceId;
  return true;
}

bool NetplayWorldSync::CommitPendingTransition(NativeA32Process& process) {
  const std::int32_t entrance = mPendingEntrance;
  mPendingEntrance = 0;
  if (entrance <= 0) return false;
  std::uint32_t play = 0U;
  std::uint16_t scene = 0U;
  std::uint8_t room = 0U;
  if (!ReadPlayState(process, &play, &scene, &room)) return false;
  if (RequestTransition(process, play, entrance)) {
    mTransitionCooldown = kTransitionCooldownFrames;
    return true;
  }
  mTransitionCooldown = 30U;
  return false;
}

void NetplayWorldSync::FillLocalPose(NativeA32Process& process,
                                     NetplayPose* local) const {
  if (local == nullptr) return;
  std::uint32_t play = 0U;
  std::uint16_t scene = 0U;
  std::uint8_t room = 0U;
  if (!ReadPlayState(process, &play, &scene, &room)) return;
  local->SceneId = scene;
  local->SwitchFlags = ReadSwitchFlags(process, play);
  local->CollectibleFlags = mCollectibleFlags;
  std::int32_t entrance = 0;
  if (process.Memory().Read32(kSaveEntrance,
                              reinterpret_cast<std::uint32_t*>(&entrance))) {
    local->EntranceId = entrance;
  }
}

bool NetplayWorldSync::ApplySwitchFlags(NativeA32Process& process,
                                        std::uint32_t play,
                                        std::uint64_t flags) const {
  auto& memory = process.Memory();
  std::uint32_t currentLow = 0U;
  std::uint32_t currentHigh = 0U;
  if (!memory.Read32(play + kPlaySwitchFlags32, &currentLow) ||
      !memory.Read32(play + kPlaySwitchFlags64, &currentHigh)) {
    return false;
  }
  const std::uint32_t low = static_cast<std::uint32_t>(flags);
  const std::uint32_t high = static_cast<std::uint32_t>(flags >> 32U);
  if (low == currentLow && high == currentHigh) return true;
  if (TraceEnabled()) {
    std::fprintf(stderr,
                 "TRIAEVUM_NETPLAY_WORLD switches=0x%08X%08X\n", high, low);
  }
  return memory.Write32(play + kPlaySwitchFlags32, low) &&
         memory.Write32(play + kPlaySwitchFlags64, high);
}

void NetplayWorldSync::ApplyCollectibleFlags(NativeA32Process& process,
                                             std::uint32_t play,
                                             std::uint64_t flags) {
  const std::uint64_t pending = flags & ~mAppliedCollectible;
  if (pending == 0U) return;
  if (TraceEnabled()) {
    std::fprintf(stderr,
                 "TRIAEVUM_NETPLAY_WORLD collectibles=0x%016llX\n",
                 static_cast<unsigned long long>(pending));
  }
  for (std::uint32_t flag = 0U; flag < kCollectibleFlagCount; ++flag) {
    const std::uint64_t bit = std::uint64_t{1} << flag;
    if ((pending & bit) == 0U) continue;
    const std::array<std::uint32_t, 2> arguments{play, flag};
    std::string error;
    const bool accepted = process.InvokeFunctionWithResult(
        kFlagsSetCollectible, arguments, kDirectCallReturn, nullptr, &error);
    if (!accepted) {
      if (TraceEnabled()) {
        std::fprintf(stderr,
                     "TRIAEVUM_NETPLAY_WORLD collectible_set_failed=%u %s\n",
                     flag, error.c_str());
      }
      return;
    }
    mAppliedCollectible |= bit;
  }
}

void NetplayWorldSync::QueryCollectibleFlags(NativeA32Process& process,
                                             std::uint32_t play) {
  std::uint64_t flags = 0U;
  for (std::uint32_t flag = 0U; flag < kCollectibleFlagCount; ++flag) {
    const std::array<std::uint32_t, 2> arguments{play, flag};
    std::uint32_t value = 0U;
    std::string error;
    if (!process.InvokeFunctionWithResult(kFlagsGetCollectible, arguments,
                                          kDirectCallReturn, &value, &error)) {
      if (TraceEnabled()) {
        std::fprintf(stderr,
                     "TRIAEVUM_NETPLAY_WORLD collectible_get_failed=%u %s\n",
                     flag, error.c_str());
      }
      return;
    }
    if (value != 0U) flags |= std::uint64_t{1} << flag;
  }
  mCollectibleFlags = flags;
  if (TraceEnabled()) {
    std::fprintf(stderr,
                 "TRIAEVUM_NETPLAY_WORLD host_collectibles=0x%016llX\n",
                 static_cast<unsigned long long>(flags));
  }
}

bool NetplayWorldSync::RequestTransition(NativeA32Process& process,
                                         std::uint32_t play,
                                         std::int32_t entrance) {
  const std::array<std::uint32_t, 3> arguments{
      play, static_cast<std::uint32_t>(entrance), 20U};
  std::uint32_t accepted = 0U;
  std::string error;
  if (!process.InvokeFunctionWithResult(kRequestTransition, arguments,
                                        kDirectCallReturn, &accepted,
                                        &error) ||
      accepted == 0U) {
    if (TraceEnabled()) {
      std::fprintf(stderr,
                   "TRIAEVUM_NETPLAY_WORLD transition_rejected=%d\n",
                   entrance);
    }
    return false;
  }
  const std::array<std::uint32_t, 1> prepare{play};
  if (!process.InvokeFunction(kPrepareTransitionEffect, prepare,
                              kDirectCallReturn, &error)) {
    if (TraceEnabled()) {
      std::fprintf(stderr, "TRIAEVUM_NETPLAY_WORLD transition_prepare_failed=%s\n",
                   error.c_str());
    }
    return false;
  }
  if (TraceEnabled()) {
    std::fprintf(stderr, "TRIAEVUM_NETPLAY_WORLD transition_requested=%d\n",
                 entrance);
  }
  return true;
}

} // namespace Oot3dNativeGame
