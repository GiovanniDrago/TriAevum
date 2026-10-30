#include "oot3d_netplay_remote_actor.h"

#include "oot3d_native_a32_process.h"

#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace Oot3dNativeGame {
namespace {

// Recovered OoT3D native contracts shared with the launcher/runtime probes.
constexpr std::uint32_t kPlayStatePointer = 0x005043D4U + 0x0CU;
constexpr std::uint32_t kPlayMode = 0x100U;       // u8
constexpr std::uint32_t kPlaySceneId = 0x104U;    // u16
constexpr std::uint32_t kPlayPlayer = 0x20ACU;    // u32
constexpr std::uint32_t kActorContext = 0x208CU;
constexpr std::uint32_t kActorListOffset = 0x0CU;
constexpr std::uint32_t kActorListCount = 12U;
constexpr std::uint32_t kActorNext = 0x130U;
constexpr std::uint32_t kActorId = 0x00U;         // s16
constexpr std::uint32_t kActorObjectSlot = 0x1EU;  // u8
constexpr std::uint32_t kActorHomePos = 0x08U;
constexpr std::uint32_t kActorHomeRot = 0x14U;
constexpr std::uint32_t kActorWorldPos = 0x28U;
constexpr std::uint32_t kActorWorldRot = 0x34U;
constexpr std::uint32_t kActorShapeRot = 0xBCU;
constexpr std::uint32_t kPlayerSkelAnime = 0x254U;
constexpr std::uint32_t kSkelAnimeSize = 0x84U;
constexpr std::uint32_t kSkelAnimationIndex = 0x30U;
constexpr std::uint32_t kSkelCurrentFrame = 0x3CU;
constexpr std::uint32_t kSkelPlaySpeed = 0x40U;
constexpr std::uint32_t kSkelModelHandle = 0x28U;
constexpr std::uint32_t kSkelJointMatrices = 0x78U;
constexpr std::uint32_t kSkelMorphMatrices = 0x7CU;
constexpr std::uint32_t kSkelAnimeUpdate = 0x0036B4ECU;
constexpr float kSkelAnimeUpdateRate = 2.0F;
constexpr std::uint32_t kActorUpdate = 0x13CU;
constexpr std::uint32_t kActorDraw = 0x140U;
constexpr std::uint32_t kActorSpawn = 0x003738D0U;
constexpr std::uint32_t kActorKill = 0x00374428U;
constexpr std::uint32_t kActorNoop = 0x00161640U;
constexpr std::uint32_t kDirectCallReturn = 0x60000000U;
constexpr std::uint16_t kActorPlayer = 0x0000U;

bool TraceEnabled() {
  static const bool enabled =
      std::getenv("TRIAEVUM_NETPLAY_REMOTE_ACTOR_TRACE") != nullptr;
  return enabled;
}

bool ReadFloat(NativeA32Memory& memory, std::uint32_t address, float* value) {
  if (value == nullptr) return false;
  std::uint32_t bits = 0U;
  if (!memory.Read32(address, &bits)) return false;
  *value = std::bit_cast<float>(bits);
  return true;
}

bool WriteFloat(NativeA32Memory& memory, std::uint32_t address, float value) {
  return memory.Write32(address, std::bit_cast<std::uint32_t>(value));
}

} // namespace

void NetplayRemoteActor::SetRemotePose(const NetplayPose* pose) {
  if (pose == nullptr) {
    mRemote.reset();
    mPreviousPose.reset();
    mPoseBlend = 1.0F;
    return;
  }
  const bool newSequence = !mRemote.has_value() ||
                           mRemote->Sequence != pose->Sequence;
  if (newSequence) {
    mPreviousPose = mRemote.has_value() ? *mRemote : *pose;
    mPoseBlend = 0.0F;
  }
  mRemote = *pose;
}

NetplayPose NetplayRemoteActor::CurrentPose() const {
  if (!mRemote.has_value()) return {};
  if (!mPreviousPose.has_value()) return *mRemote;
  return InterpolateNetplayPose(*mPreviousPose, *mRemote, mPoseBlend);
}

void NetplayRemoteActor::Reset() {
  mRemote.reset();
  mPreviousPose.reset();
  mPoseBlend = 1.0F;
  mPlay = 0;
  mActor = 0;
  mPending = 0;
  mPendingFrames = 0;
  mCooldown = 0;
  mScene = 0;
  mAnimationInitialized = false;
}

void NetplayRemoteActor::BeforeGuest(NativeA32Process& process) {
  auto& memory = process.Memory();
  std::uint32_t play = 0U;
  std::uint8_t mode = 0U;
  if (!memory.Read32(kPlayStatePointer, &play) || play == 0U ||
      !memory.Read8(play + kPlayMode, &mode) || mode != 3U) {
    return;
  }

  std::uint16_t scene = 0U;
  if (!memory.Read16(play + kPlaySceneId, &scene)) return;
  if (play != mPlay || scene != mScene) {
    Abandon(process);
    mPlay = play;
    mScene = scene;
    mCooldown = 0;
  }

  if (!mRemote.has_value() || mRemote->SceneId == 0U ||
      mRemote->SceneId != scene) {
    Abandon(process);
    return;
  }

  if (mPoseBlend < 1.0F) {
    mPoseBlend = std::min(1.0F, mPoseBlend + 0.35F);
  }
  const NetplayPose pose = CurrentPose();
  if (mActor != 0U) {
    if (!ActorUsable(process, mActor)) {
      mActor = 0U;
      mAnimationInitialized = false;
      mCooldown = 0;
    } else {
      ApplyPose(process, mActor, pose);
      return;
    }
  }

  if (mPending != 0U) {
    ++mPendingFrames;
    if (ActorUsable(process, mPending)) {
      mActor = mPending;
      mPending = 0;
      mPendingFrames = 0;
      ApplyPose(process, mActor, pose);
      if (TraceEnabled()) {
        std::fprintf(stderr,
                     "TRIAEVUM_NETPLAY_REMOTE_ACTOR ready=0x%08X scene=%u\n",
                     mActor, static_cast<unsigned>(scene));
      }
      return;
    }
    if (mPendingFrames > 30U) {
      if (TraceEnabled()) {
        std::uint16_t id = 0xFFFFU;
        std::uint32_t draw = 0U;
        std::uint32_t update = 0U;
        process.Memory().Read16(mPending + kActorId, &id);
        process.Memory().Read32(mPending + kActorDraw, &draw);
        process.Memory().Read32(mPending + kActorUpdate, &update);
        std::fprintf(stderr,
                     "TRIAEVUM_NETPLAY_REMOTE_ACTOR timeout=0x%08X id=%u draw=0x%08X update=0x%08X list=%d\n",
                     mPending, static_cast<unsigned>(id), draw, update,
                     ActorInCurrentLists(process, mPlay, mPending) ? 1 : 0);
      }
      Abandon(process);
      mCooldown = 120U;
    }
    return;
  }

  if (mCooldown != 0U) {
    --mCooldown;
    return;
  }
  if (Spawn(process, play)) {
    return;
  }
  mCooldown = 120U;
}

bool NetplayRemoteActor::Spawn(NativeA32Process& process,
                               std::uint32_t play) {
  if (!mRemote.has_value()) return false;
  auto& memory = process.Memory();
  std::uint32_t localPlayer = 0U;
  if (!memory.Read32(play + kPlayPlayer, &localPlayer) || localPlayer == 0U) {
    return false;
  }
  std::uint16_t localId = 0xFFFFU;
  if (!memory.Read16(localPlayer + kActorId, &localId) ||
      localId != kActorPlayer) {
    return false;
  }

  const std::uint16_t yaw = NetplayYawToBinaryAngle(mRemote->Yaw);
  std::uint32_t actor = 0U;
  if (!CallActorSpawn(process, play, mRemote->Position[0],
                      mRemote->Position[1], mRemote->Position[2],
                      static_cast<std::int16_t>(yaw), &actor) ||
      actor == 0U) {
    if (TraceEnabled()) std::fprintf(stderr, "TRIAEVUM_NETPLAY_REMOTE_ACTOR reject=spawn_call\n");
    return false;
  }

  std::uint16_t id = 0xFFFFU;
  if (!memory.Read16(actor + kActorId, &id) || id != kActorPlayer) {
    if (TraceEnabled()) std::fprintf(stderr, "TRIAEVUM_NETPLAY_REMOTE_ACTOR reject=spawn_id=%u actor=0x%08X\n", static_cast<unsigned>(id), actor);
    Abandon(process);
    return false;
  }

  std::uint8_t objectSlot = 0U;
  if (memory.Read8(localPlayer + kActorObjectSlot, &objectSlot) &&
      objectSlot < 19U) {
    memory.Write8(actor + kActorObjectSlot, objectSlot);
  }
  // Player's profile has no draw callback until its specialized init runs.
  // Reuse the local Player's validated draw entry so the guest-owned actor is
  // rendered by the same native path; update remains the no-op.
  std::uint32_t draw = 0U;
  if (memory.Read32(localPlayer + kActorDraw, &draw) && draw != 0U) {
    memory.Write32(actor + kActorDraw, draw);
  }
  // The actor is guest-owned, but its update must never run local input or
  // camera logic. Draw is left to the native Player_Draw path.
  if (!memory.Write32(actor + kActorUpdate, kActorNoop)) {
    Abandon(process);
    return false;
  }
  mPending = actor;
  mPendingFrames = 0;
  if (TraceEnabled()) {
    std::fprintf(stderr,
                 "TRIAEVUM_NETPLAY_REMOTE_ACTOR spawned=0x%08X local=0x%08X "
                 "object_slot=%u scene=%u\n",
                 actor, localPlayer, static_cast<unsigned>(objectSlot),
                 static_cast<unsigned>(mScene));
  }
  return true;
}

bool NetplayRemoteActor::CallActorSpawn(NativeA32Process& process,
                                        std::uint32_t play, float x, float y,
                                        float z, std::int16_t yaw,
                                        std::uint32_t* actor) {
  if (actor != nullptr) *actor = 0U;
  auto state = process.PrimaryThreadState();
  const std::uint32_t stack = (state.r[13] - 0x100U) & ~7U;
  auto& memory = process.Memory();
  if (!memory.Write32(stack + 0U, static_cast<std::uint32_t>(yaw)) ||
      !memory.Write32(stack + 4U, 0U) || !memory.Write32(stack + 8U, 0U) ||
      !memory.Write32(stack + 12U, 1U)) {
    return false;
  }
  state.r[0] = play + kActorContext;
  state.r[1] = play;
  state.r[2] = kActorPlayer;
  state.r[3] = 0U;
  state.r[13] = stack;
  state.vfp[0] = std::bit_cast<std::uint32_t>(x);
  state.vfp[1] = std::bit_cast<std::uint32_t>(y);
  state.vfp[2] = std::bit_cast<std::uint32_t>(z);
  state.vfp[3] = 0U;
  std::string error;
  if (!process.InvokeFunctionWithState(kActorSpawn, state, kDirectCallReturn,
                                       &error, 10'000'000U, 100'000U)) {
    if (TraceEnabled()) {
      std::fprintf(stderr, "TRIAEVUM_NETPLAY_REMOTE_ACTOR spawn_failed=%s\n",
                   error.c_str());
    }
    return false;
  }
  if (actor != nullptr) *actor = state.r[0];
  return true;
}

bool NetplayRemoteActor::ActorInCurrentLists(NativeA32Process& process,
                                             std::uint32_t play,
                                             std::uint32_t actor) const {
  if (actor == 0U) return false;
  auto& memory = process.Memory();
  for (std::uint32_t list = 0U; list < kActorListCount; ++list) {
    std::uint32_t candidate = 0U;
    if (!memory.Read32(play + kActorContext + kActorListOffset +
                           list * 8U + 4U,
                       &candidate)) {
      continue;
    }
    for (std::uint32_t index = 0U; candidate != 0U && index < 512U;
         ++index) {
      if (candidate == actor) return true;
      std::uint32_t next = 0U;
      if (!memory.Read32(candidate + kActorNext, &next)) break;
      candidate = next;
    }
  }
  return false;
}

bool NetplayRemoteActor::ActorUsable(NativeA32Process& process,
                                     std::uint32_t actor) const {
  if (actor == 0U) return false;
  auto& memory = process.Memory();
  std::uint16_t id = 0xFFFFU;
  std::uint32_t draw = 0U;
  if (!memory.Read16(actor + kActorId, &id) || id != kActorPlayer ||
      !memory.Read32(actor + kActorDraw, &draw) || draw == 0U) {
    return false;
  }
  return mPlay != 0U && ActorInCurrentLists(process, mPlay, actor);
}

void NetplayRemoteActor::ApplyPose(NativeA32Process& process,
                                   std::uint32_t actor,
                                   const NetplayPose& pose) const {
  auto& memory = process.Memory();
  const std::int16_t yaw = NetplayYawToBinaryAngle(pose.Yaw);
  for (std::uint32_t offset : {kActorWorldPos, kActorHomePos}) {
    WriteFloat(memory, actor + offset, pose.Position[0]);
    WriteFloat(memory, actor + offset + 4U, pose.Position[1]);
    WriteFloat(memory, actor + offset + 8U, pose.Position[2]);
  }
  memory.Write16(actor + kActorWorldRot + 2U,
                 static_cast<std::uint16_t>(yaw));
  memory.Write16(actor + kActorHomeRot + 2U,
                 static_cast<std::uint16_t>(yaw));
  memory.Write16(actor + kActorShapeRot + 2U,
                 static_cast<std::uint16_t>(yaw));
  // The remote actor has no update callback, so its own SkelAnime never
  // advances. Initialize its animation resources from the local Player once,
  // then apply the peer's streamed index/frame range and advance the remote
  // actor's own pose buffers.
  std::uint32_t localPlayer = 0U;
  std::uint16_t localId = 0xFFFFU;
  if (mPlay != 0U &&
      memory.Read32(mPlay + kPlayPlayer, &localPlayer) && localPlayer != 0U &&
      memory.Read16(localPlayer + kActorId, &localId) &&
      localId == kActorPlayer) {
    std::array<std::uint8_t, kSkelAnimeSize> animation{};
    const bool haveLocalAnimation =
        memory.ReadBytes(localPlayer + kPlayerSkelAnime, animation);
    if (haveLocalAnimation && !mAnimationInitialized) {
      // Preserve the remote actor's own model and pose buffers. Copying the
      // local pointers would make a remote SkelAnime update overwrite the
      // local Link's joint matrices.
      std::array<std::uint8_t, kSkelAnimeSize> remoteAnimation{};
      std::array<std::uint8_t, 12> remoteBuffers{};
      if (memory.ReadBytes(actor + kPlayerSkelAnime, remoteAnimation)) {
        std::memcpy(remoteBuffers.data(), remoteAnimation.data() + kSkelModelHandle, 4);
        std::memcpy(remoteBuffers.data() + 4, remoteAnimation.data() + kSkelJointMatrices, 4);
        std::memcpy(remoteBuffers.data() + 8, remoteAnimation.data() + kSkelMorphMatrices, 4);
      }
      (void)memory.WriteBytes(actor + kPlayerSkelAnime, animation);
      if (!remoteBuffers.empty()) {
        (void)memory.WriteBytes(actor + kPlayerSkelAnime + kSkelModelHandle,
                                std::span<const std::uint8_t>(remoteBuffers.data(), 4));
        (void)memory.WriteBytes(actor + kPlayerSkelAnime + kSkelJointMatrices,
                                std::span<const std::uint8_t>(remoteBuffers.data() + 4, 4));
        (void)memory.WriteBytes(actor + kPlayerSkelAnime + kSkelMorphMatrices,
                                std::span<const std::uint8_t>(remoteBuffers.data() + 8, 4));
      }
      mAnimationInitialized = true;
    }
    const bool streamedAnimation =
        std::isfinite(pose.AnimationFrame) &&
        std::isfinite(pose.AnimationSpeed) &&
        std::isfinite(pose.AnimationStart) &&
        std::isfinite(pose.AnimationEnd) &&
        pose.AnimationEnd > pose.AnimationStart;
    if (streamedAnimation) {
      memory.Write32(actor + kPlayerSkelAnime + kSkelAnimationIndex,
                     pose.Animation);
      memory.Write32(actor + kPlayerSkelAnime + kSkelCurrentFrame,
                     std::bit_cast<std::uint32_t>(pose.AnimationFrame));
      memory.Write32(actor + kPlayerSkelAnime + kSkelPlaySpeed,
                     std::bit_cast<std::uint32_t>(pose.AnimationSpeed));
      memory.Write32(actor + kPlayerSkelAnime + 0x44U,
                     std::bit_cast<std::uint32_t>(pose.AnimationStart));
      memory.Write32(actor + kPlayerSkelAnime + 0x48U,
                     std::bit_cast<std::uint32_t>(pose.AnimationEnd));
      memory.Write32(actor + kPlayerSkelAnime + 0x4CU,
                     std::bit_cast<std::uint32_t>(pose.AnimationEnd -
                                                  pose.AnimationStart));
    } else if (haveLocalAnimation) {
      // Compatibility fallback for peers that do not yet stream animation.
      std::array<std::uint8_t, 12> remoteBuffers{};
      std::array<std::uint8_t, kSkelAnimeSize> remoteAnimation{};
      if (memory.ReadBytes(actor + kPlayerSkelAnime, remoteAnimation)) {
        std::memcpy(remoteBuffers.data(), remoteAnimation.data() + kSkelModelHandle, 4);
        std::memcpy(remoteBuffers.data() + 4, remoteAnimation.data() + kSkelJointMatrices, 4);
        std::memcpy(remoteBuffers.data() + 8, remoteAnimation.data() + kSkelMorphMatrices, 4);
      }
      (void)memory.WriteBytes(actor + kPlayerSkelAnime, animation);
      (void)memory.WriteBytes(actor + kPlayerSkelAnime + kSkelModelHandle,
                              std::span<const std::uint8_t>(remoteBuffers.data(), 4));
      (void)memory.WriteBytes(actor + kPlayerSkelAnime + kSkelJointMatrices,
                              std::span<const std::uint8_t>(remoteBuffers.data() + 4, 4));
      (void)memory.WriteBytes(actor + kPlayerSkelAnime + kSkelMorphMatrices,
                              std::span<const std::uint8_t>(remoteBuffers.data() + 8, 4));
    }
    if (mAnimationInitialized) {
      auto state = process.PrimaryThreadState();
      state.r[0] = actor + kPlayerSkelAnime;
      state.r[1] = mPlay;
      state.vfp[0] = std::bit_cast<std::uint32_t>(kSkelAnimeUpdateRate);
      std::string error;
      (void)process.InvokeFunctionWithState(kSkelAnimeUpdate, state,
                                            0x60000000U, &error);
    }
    static std::uint32_t diagnosticTick = 0;
    if (TraceEnabled() && (++diagnosticTick % 30U) == 1U) {
        std::int32_t localIndex = 0;
        std::int32_t remoteIndex = 0;
        float localFrame = 0.0F;
        float remoteFrame = 0.0F;
        float localSpeed = 0.0F;
        std::uint32_t localModel = 0U;
        std::uint32_t remoteModel = 0U;
        memory.Read32(localPlayer + kPlayerSkelAnime + kSkelAnimationIndex,
                      reinterpret_cast<std::uint32_t*>(&localIndex));
        memory.Read32(actor + kPlayerSkelAnime + kSkelAnimationIndex,
                      reinterpret_cast<std::uint32_t*>(&remoteIndex));
        memory.Read32(localPlayer + kPlayerSkelAnime + kSkelCurrentFrame,
                      reinterpret_cast<std::uint32_t*>(&localFrame));
        memory.Read32(actor + kPlayerSkelAnime + kSkelCurrentFrame,
                      reinterpret_cast<std::uint32_t*>(&remoteFrame));
        memory.Read32(localPlayer + kPlayerSkelAnime + kSkelPlaySpeed,
                      reinterpret_cast<std::uint32_t*>(&localSpeed));
        memory.Read32(localPlayer + kPlayerSkelAnime + kSkelModelHandle,
                      &localModel);
        memory.Read32(actor + kPlayerSkelAnime + kSkelModelHandle, &remoteModel);
        std::fprintf(stderr,
                     "TRIAEVUM_NETPLAY_REMOTE_ANIM local_index=%d remote_index=%d "
                     "local_frame=%.3f remote_frame=%.3f local_speed=%.3f "
                     "local_model=0x%08X remote_model=0x%08X\n",
                     localIndex, remoteIndex, localFrame, remoteFrame, localSpeed,
                     localModel, remoteModel);
    }
  }
}

void NetplayRemoteActor::PrepareSceneTransition(NativeA32Process& process) {
  const std::uint32_t actor = mActor != 0U ? mActor : mPending;
  if (actor != 0U) {
    auto& memory = process.Memory();
    memory.Write32(actor + kActorUpdate, kActorNoop);
    memory.Write32(actor + kActorDraw, 0U);
  }
  Abandon(process);
}

void NetplayRemoteActor::Abandon(NativeA32Process& process) {
  const std::uint32_t actor = mActor != 0U ? mActor : mPending;
  mActor = 0;
  mPending = 0;
  mPendingFrames = 0;
  mAnimationInitialized = false;
  if (actor == 0U) return;
  if (!ActorInCurrentLists(process, mPlay, actor)) return;
  std::uint16_t id = 0xFFFFU;
  if (!process.Memory().Read16(actor + kActorId, &id) || id != kActorPlayer) {
    return;
  }
  const std::array<std::uint32_t, 1> arguments{actor};
  std::string error;
  (void)process.InvokeFunction(kActorKill, arguments, kDirectCallReturn,
                               &error);
}

} // namespace Oot3dNativeGame
