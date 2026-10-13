#pragma once

#include "oot3d_native_a32_memory.h"
#include "oot3d_top_screen_input_cadence.h"
#include "oot3d_top_screen_mod_profile.h"

#include <array>
#include <cstdint>

namespace Oot3dNativeGame {

// The TopScreen pause transactions of payload 0x005C9940: START closes the
// active page, and B on a closing page opens Save/Quit. Both call original
// OoT3D page resets and pause services. They run as guest calls at the
// native UI input boundary inside PauseUi_Update, the frame position of the
// payload's own controller, and never at the host refresh boundary: there the
// primary guest thread can be suspended inside a native routine, and its tail
// resumes after the reset and undoes it. A detail message being prepared
// reopened over gameplay once its page was gone (issue 60), and an icon being
// loaded faulted on the released resources.
class TopScreenPauseTransactionRuntime final {
public:
  explicit TopScreenPauseTransactionRuntime(NativeA32Memory &memory) : mMemory(memory) {}

  // The page is resolved again at delivery, so a page that has already
  // closed or changed hands is not reset on behalf of the previous one.
  void RequestStartClose() noexcept;
  void RequestSystemOpen(const TopScreenPauseSystemOpenPlan &plan) noexcept;
  // Called once per host refresh. The native UI update runs at the title's
  // 30 or 20 Hz, so a request waits a few refreshes for it; one it never
  // reaches is dropped rather than carried into a later pause session.
  void AgeUndelivered() noexcept;
  bool Requested() const noexcept { return mRequest != Request::None; }
  bool NeedsExecution() const noexcept { return Requested() || Pending(); }
  // Between two guest calls of a transaction.
  bool Pending() const noexcept { return mStepCount != 0U; }
  bool Execute(oot3d::recomp::a32::GuestState &state,
               oot3d::recomp::a32::ExecutionResult *result,
               std::uint32_t *blocksConsumed);
  void Reset() noexcept;

  std::uint64_t StartCloses() const noexcept { return mStartCloses; }
  std::uint64_t SystemOpens() const noexcept { return mSystemOpens; }
  std::uint64_t Retired() const noexcept { return mRetired; }
  TopScreenPauseClosePage LastClosedPage() const noexcept { return mLastClosedPage; }
  TopScreenPauseClosePage LastSystemSourcePage() const noexcept { return mLastSystemSourcePage; }

private:
  enum class Request : std::uint8_t { None, StartClose, SystemOpen };
  struct Step {
    bool Call = false;
    std::uint32_t Address = 0;
    std::uint32_t Value0 = 0;
    std::uint32_t Value1 = 0;
  };
  bool Begin();

  NativeA32Memory &mMemory;
  Request mRequest = Request::None;
  std::uint32_t mRequestAge = 0;
  TopScreenPauseSystemOpenPlan mSystemOpenPlan;
  std::array<Step, 5> mSteps{};
  std::size_t mStepCount = 0;
  std::size_t mNextStep = 0;
  oot3d::recomp::a32::GuestState mCallerState{};
  std::uint64_t mStartCloses = 0;
  std::uint64_t mSystemOpens = 0;
  std::uint64_t mRetired = 0;
  TopScreenPauseClosePage mLastClosedPage = TopScreenPauseClosePage::None;
  TopScreenPauseClosePage mLastSystemSourcePage = TopScreenPauseClosePage::None;
};

} // namespace Oot3dNativeGame
