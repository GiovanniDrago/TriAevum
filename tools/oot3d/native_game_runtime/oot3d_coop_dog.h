#pragma once

#include "a32_runtime.h"
#include "oot3d_coop_splitscreen.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <atomic>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace oot3d::recomp::a32 {
struct ExecutionResult;
struct GuestState;
}

namespace Oot3dNativeGame {

class NativeA32Process;
class NativeA32Memory;

// Actor_SpawnTransitionActors: the scene/room-load entry used to start the
// guest-side OBJECT_DOG request. It is a function/block entry in the title.
inline constexpr uint32_t kCoopActorSpawnTransitionActors = 0x0032525CU;
inline constexpr uint32_t kCoopSceneExecuteCommands = 0x002E4DE4U;
inline constexpr uint32_t kCoopActorListCommandHandler = 0x0023447CU;
inline constexpr uint32_t kCoopEnDogUpdate = 0x001D699CU;
inline constexpr uint32_t kCoopEnDogDraw = 0x001D6918U;
inline constexpr uint32_t kCoopEnDogBehavior = 0x0640U;
inline constexpr uint32_t kCoopEnDogNextBehavior = 0x0642U;
inline constexpr uint32_t kCoopEnDogSkelAnime = 0x01A4U;
inline constexpr uint32_t kCoopDogAnimationInfoPointer = 0x001D6C88U;
inline constexpr uint32_t kCoopSkelAnimeUpdate = 0x00370734U;
inline constexpr uint32_t kCoopAnimationChangeByInfo = 0x003717ACU;
inline constexpr uint32_t kCoopActorKill = 0x00374428U;
inline constexpr uint32_t kCoopActorApplyDamage = 0x00375EB8U;
inline constexpr uint32_t kCoopRupeesChangeBy = 0x00376A60U;
inline constexpr uint16_t kCoopActorItem00 = 0x0015U;
// Bounded evidence budget for the En_Item00 collectible-delay diagnostic.
inline constexpr uint32_t kCoopRupeeDiagnosticLimit = 6U;

// The co-op companion reads this from data/config/coop_controls.json.  The
// controller port is zero-based: port 1 is the second SDL gamepad, so the
// first pad keeps driving Link and the second one drives the dog.
struct CoopDogConfig {
  bool Enabled = true;
  uint8_t ControllerPort = 1;
  std::string MovementStick = "left";
  float Deadzone = 0.20F;
  bool InvertY = false;
  std::string JumpButton = "a";
  std::string BarkButton = "b";
  std::string RespawnButton = "start";
  std::string SummonButton = "dpad_down";
  std::string SprintButton = "zl";

  // Split-screen tuning.  The grace period is expressed in milliseconds and
  // the safe margin is the inner normalized frame used to decide whether both
  // characters share the same view.  Camera values are world units.
  bool SplitScreenEnabled = true;
  float SplitGraceMs = 500.0F;
  float SplitSafeMargin = 0.9F;
  // "horizontal" means top/bottom panels; "vertical" means left/right.
  std::string SplitLayout = "horizontal";
  float DogCameraDistance = 180.0F;
  float DogCameraHeight = 90.0F;
  float DogCameraTargetHeight = 30.0F;

  // Movement and jump values are expressed in world units per second.
  // TurnSpeed is in degrees per second; gravity is positive downward.
  float MoveSpeed = 120.0F;
  float SprintSpeed = 200.0F;
  float Acceleration = 600.0F;
  float Deceleration = 800.0F;
  float BarkRange = 55.0F;
  int BarkDamage = 2;
  float RupeePickupRange = 30.0F;
  float TurnSpeed = 180.0F;
  // Camera-relative sign correction, exposed for easy validation on hardware.
  float CameraForwardSign = 1.0F;
  // The native camera yaw uses the opposite screen-right convention.
  float CameraStrafeSign = -1.0F;
  float JumpVelocity = 200.0F;
  float Gravity = 600.0F;
  float TickSeconds = 1.0F / 30.0F;

  // The dog can always be sent back to Link with the respawn button.  It is
  // never teleported automatically because it wandered away or died.
  float RespawnOffset = 45.0F;
  bool AutoSpawnOnSceneChange = false;
  bool SpawnIfMissing = true;
  bool Invincible = true;

  // 0 means "first dog in the actor list".  The low colour nibble selects the
  // Dog Lady's palette; the default keeps the native dark dog when present.
  uint16_t DogParams = 0x8001U;
  bool Log = true;
};

// Snapshot of the render-side split decision for the guest-side probe writer.
struct CoopSplitProbeState {
  bool Active = false;
  SplitReason Reason = SplitReason::NoDog;
  float TimerMs = 0.0F;
  bool LinkFramed = false;
  bool DogFramed = false;
  uint32_t SplitCount = 0U;
  uint32_t MergeCount = 0U;
  bool Panels = false;
  std::array<float, 3> DogEye{};
  std::array<float, 3> DogAt{};
};

class CoopDogRuntime {
 public:
  CoopDogRuntime() = default;

  // Loads the optional JSON file and creates a documented default file when
  // it does not exist.  Parse errors are reported but leave safe defaults.
  bool LoadConfig(const std::filesystem::path& path, std::string* error = nullptr);

  // Samples the second controller once per host frame.  Presses are latched
  // until the next guest refresh so a 60/120 Hz window cannot drop them.
  void PollInput();

  // Runs immediately before the guest refresh.  It finds or creates the dog
  // and writes its actor fields for the native draw pass.
  void BeforeGuest(NativeA32Process& process);

  // Scene-load hook: a normal guest call to Object_Spawn.  Invoking the same
  // function from the host deadlocks on its file-request wait, while the
  // scheduler services it when the call runs inside the guest.
  void ObserveBlockEntry(uint32_t pc,
                         oot3d::recomp::a32::GuestState& state,
                         NativeA32Memory& memory);

  // Native-block hook used by the whole-AOT product dispatcher.  It branches
  // into the guest's own Object_Spawn so its asynchronous file wait is
  // serviced by the guest scheduler, then resumes the interrupted function.
  bool ObserveNativeBlock(uint32_t pc,
                          oot3d::recomp::a32::GuestState& state,
                          NativeA32Memory& memory,
                          oot3d::recomp::a32::ExecutionResult* result,
                          uint32_t* blocksConsumed);

  bool Enabled() const noexcept { return config_.Enabled; }
  const CoopDogConfig& Config() const noexcept { return config_; }

  // Read-only dog state for the split-screen controller.  DogUsable() is false
  // whenever the dog actor is absent or failed validation; DogYawRadians()
  // reports the actor yaw in radians with 0 facing +Z.
  uint32_t DogActor() const noexcept { return dogActor_; }
  bool DogUsable() const noexcept { return dogUsable_; }
  std::array<float, 3> DogPosition() const noexcept { return dogPosition_; }
  float DogYawRadians() const noexcept { return dogYawRadians_; }

  // Render-side split decision published for the guest-side probe writer.
  void PublishSplitProbeState(const CoopSplitProbeState& state);

 private:
  struct InputState {
    float MoveX = 0.0F;
    float MoveY = 0.0F;
    bool Connected = false;
    bool Sprinting = false;
  };

  bool ParseConfig(const std::filesystem::path& path, std::string* error);
  void WriteDefaultConfig(const std::filesystem::path& path) const;
  void LoadTestInput();
  void SampleTestInput(uint32_t frame);
  void MaybeWriteProbe(NativeA32Process& process, uint32_t play);
  uint32_t CountNearbyRupees(NativeA32Memory& memory, uint32_t play,
                             uint32_t dog);
  void LogRupeeDiagnostic(NativeA32Memory& memory, uint32_t actor,
                          uint8_t itemCode, uint16_t params);
  uint32_t FindDog(NativeA32Process& process, uint32_t play) const;
  bool IsDogUsable(NativeA32Process& process, uint32_t actor) const;
  bool ActorInCurrentLists(NativeA32Process& process, uint32_t play,
                           uint32_t actor) const;
  bool IsDogReadyForAnimation(NativeA32Process& process,
                              uint32_t actor) const;
  void AbandonDog(NativeA32Process& process);
  bool SpawnDog(NativeA32Process& process, uint32_t play);
  bool CallObjectSpawn(NativeA32Process& process, uint32_t play);
  bool CallActorSpawn(NativeA32Process& process, uint32_t play,
                      float x, float y, float z, uint32_t* actor);
  bool CallActorBgCheck(NativeA32Process& process, uint32_t play, uint32_t actor);
  bool RequestTestWarp(NativeA32Process& process, uint32_t play, uint16_t entrance);
  void CallBark(NativeA32Process& process, uint32_t play, uint32_t actor);
  void ApplyBarkAttack(NativeA32Process& process, uint32_t play, uint32_t actor);
  void CollectRupees(NativeA32Process& process, uint32_t play, uint32_t actor);
  void TeleportDog(NativeA32Process& process, uint32_t play, uint32_t actor);
  void UpdateDog(NativeA32Process& process, uint32_t play, uint32_t actor,
                 bool canMove);
  float ReadCameraYaw(NativeA32Memory& memory, uint32_t play);
  void ApplyDesiredState(NativeA32Memory& memory, uint32_t actor) const;
  // Publishes position/yaw for the current dogActor_ after validation.
  void RefreshDogState(NativeA32Memory& memory);
  void ResetSceneState();
  void Log(const std::string& message) const;

  CoopDogConfig config_{};
  InputState input_{};
  uint32_t dogActor_ = 0U;
  // Published dog state for the split-screen controller.  Only valid while
  // dogUsable_ is true.
  bool dogUsable_ = false;
  std::array<float, 3> dogPosition_{};
  float dogYawRadians_ = 0.0F;
  mutable std::mutex splitProbeMutex_;
  CoopSplitProbeState splitProbe_;
  std::atomic<bool> splitActive_{false};
  uint32_t pendingDogActor_ = 0U;
  uint32_t pendingDogFrames_ = 0U;
  uint32_t lastPlay_ = 0U;
  uint16_t lastScene_ = 0xFFFFU;
  bool sceneSpawnPending_ = false;
  uint32_t spawnCooldown_ = 0U;
  bool jumpLatch_ = false;
  bool barkLatch_ = false;
  bool respawnLatch_ = false;
  bool summonLatch_ = false;
  bool summonHeldPrevious_ = false;
  bool hasDesiredState_ = false;
  float desiredX_ = 0.0F;
  float desiredY_ = 0.0F;
  float desiredZ_ = 0.0F;
  int16_t desiredYaw_ = 0;
  float currentVelocityX_ = 0.0F;
  float currentVelocityZ_ = 0.0F;
  uint32_t lastGameplayFrame_ = 0U;
  bool hasGameplayFrame_ = false;
  uint32_t testWarpRetryCooldown_ = 0U;
  // Successful dog pickups for this run. T007 wires the increment; until then
  // both fields stay zero and the probe reports the baseline.
  uint32_t rupeesCollected_ = 0U;
  uint16_t lastRupeeValue_ = 0U;
  // Actors already credited in this scene. Prevents a second credit if the
  // guest defers or rejects the kill. Cleared on play/scene change.
  std::vector<uint32_t> creditedRupeeActors_;
  bool cameraProbeLogged_ = false;
  bool enDogUpdateProbeLogged_ = false;
  bool animationFailureLogged_ = false;
  uint32_t desiredAnimation_ = 0xFFFFFFFFU;
  bool testWarpRequested_ = false;
  uint16_t testWarpEntrance_ = 0U;
  enum class ObjectLoadStage : uint8_t { Idle, OriginalObject, DogObject };
  ObjectLoadStage objectLoadStage_ = ObjectLoadStage::Idle;
  bool objectLoadWaiting_ = false;
  bool pendingObjectLoad_ = false;
  bool objectSpawnProbeLogged_ = false;
  bool objectIndexProbeLogged_ = false;
  bool objectContextProbeLogged_ = false;
  uint32_t lastObjectContextLogFrame_ = 0U;
  bool objectSpawnNativeProbeLogged_ = false;
  bool restoreActorList_ = false;
  uint32_t actorListRestoreAddress_ = 0U;
  uint16_t savedActorId_ = 0U;
  uint32_t objectListScratch_ = 0U;
  uint16_t lastObjectLoadScene_ = 0xFFFFU;
  uint16_t pendingObjectScene_ = 0xFFFFU;
  uint32_t pendingObjectContext_ = 0U;
  uint32_t savedObjectLoadReturnPc_ = 0U;
  oot3d::recomp::a32::GuestState savedObjectLoadState_{};
  uint32_t savedObjectLoadPlay_ = 0U;
  uint32_t savedObjectLoadR1_ = 0U;
  uint32_t savedObjectLoadReturn_ = 0U;
  uint32_t savedObjectLoadResult_ = 0U;
  bool jumpHeldPrevious_ = false;
  bool barkHeldPrevious_ = false;
  bool respawnHeldPrevious_ = false;
  int32_t controllerInstance_ = -1;
  bool loggedNoController_ = false;

  // Test-only deterministic P2 input and runtime probe. Both are opt-in via
  // environment variables and are never active in a normal launch.
  struct TestInputSegment {
    uint32_t StartFrame = 0U;
    uint32_t EndFrameExclusive = 0U;
    float MoveX = 0.0F;
    float MoveY = 0.0F;
    bool Sprint = false;
    bool Start = false;
    bool A = false;
    bool B = false;
    bool Summon = false;
  };
  std::vector<TestInputSegment> testInputSegments_;
  bool testInputActive_ = false;
  uint32_t testFrame_ = 0U;
  bool testStartHeld_ = false;
  bool testAHeld_ = false;
  bool testBHeld_ = false;
  bool testSummonHeld_ = false;
  std::string probePath_;
  std::ofstream probeStream_;
  uint32_t probeInterval_ = 15U;
  uint32_t lastProbeFrame_ = 0xFFFFFFFFU;
  bool probeOpenFailed_ = false;

  // Bounded, run-scoped evidence for the En_Item00 collectible-delay offset.
  // Only the first few distinct rupee actors are logged, once each.
  std::array<uint32_t, kCoopRupeeDiagnosticLimit> rupeeProbeLoggedActors_{};
  uint32_t rupeeProbeLoggedCount_ = 0U;
};

} // namespace Oot3dNativeGame
