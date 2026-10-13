#include "oot3d_top_screen_pause_transaction.h"

#include <stdexcept>

namespace Oot3dNativeGame {
namespace {
// Native pause controller state and scene control written by payload
// 0x005C9940, and the original OoT3D services it calls.
constexpr std::uint32_t kPauseControllerState = 0x0050AF68U;
constexpr std::uint32_t kPauseSceneControl = 0x005043E8U;
constexpr std::uint32_t kPauseInputSetTouchEnabled = 0x002FD84CU;
constexpr std::uint32_t kPauseSystemMenuOpen = 0x0043ACA8U;
constexpr std::uint32_t kCallStackReservation = 0x20U;
// Host refreshes a request waits for the native UI update.
constexpr std::uint32_t kUndeliveredRefreshLimit = 8U;

std::uint32_t PageState(const TopScreenPauseDrawInputs &inputs, TopScreenPauseClosePage page) {
  switch (page) {
  case TopScreenPauseClosePage::Gear: return inputs.ChildStates[0];
  case TopScreenPauseClosePage::Items: return inputs.ChildStates[1];
  case TopScreenPauseClosePage::DungeonMap: return inputs.ChildStates[2];
  case TopScreenPauseClosePage::SystemMenu: return inputs.ChildStates[3];
  case TopScreenPauseClosePage::None: break;
  }
  return 0U;
}
} // namespace

void TopScreenPauseTransactionRuntime::RequestStartClose() noexcept {
  mRequest = Request::StartClose;
  mRequestAge = 0;
}

void TopScreenPauseTransactionRuntime::RequestSystemOpen(
    const TopScreenPauseSystemOpenPlan &plan) noexcept {
  // A START close requested in the same refresh wins over Save/Quit.
  if (mRequest == Request::StartClose) return;
  mRequest = Request::SystemOpen;
  mRequestAge = 0;
  mSystemOpenPlan = plan;
}

void TopScreenPauseTransactionRuntime::AgeUndelivered() noexcept {
  if (mRequest == Request::None || ++mRequestAge <= kUndeliveredRefreshLimit) return;
  mRequest = Request::None;
  ++mRetired;
}

void TopScreenPauseTransactionRuntime::Reset() noexcept {
  mRequest = Request::None;
  mRequestAge = 0;
  mSystemOpenPlan = {};
  mStepCount = 0;
  mNextStep = 0;
}

bool TopScreenPauseTransactionRuntime::Begin() {
  const auto request = mRequest;
  mRequest = Request::None;
  TopScreenPauseDrawInputs inputs;
  if (!ReadTopScreenPauseDrawInputs(mMemory, &inputs))
    throw std::runtime_error("TopScreen pause transaction cannot read native pause state");
  mStepCount = 0;
  mNextStep = 0;
  const auto add = [&](bool call, std::uint32_t address, std::uint32_t value0 = 0U,
                       std::uint32_t value1 = 0U) {
    mSteps[mStepCount++] = {call, address, value0, value1};
  };
  if (request == Request::StartClose) {
    const auto plan = ResolveTopScreenPauseStartClose(inputs, true);
    if (plan.Page == TopScreenPauseClosePage::None || plan.ResetFunction == 0U) {
      ++mRetired;
      return false;
    }
    add(true, plan.ResetFunction);
    add(false, kPauseControllerState, 2U);
    add(false, kPauseSceneControl, 0U);
    add(true, kPauseInputSetTouchEnabled, 0U, 1U);
    ++mStartCloses;
    mLastClosedPage = plan.Page;
    return true;
  }
  if (request == Request::SystemOpen) {
    const auto plan = mSystemOpenPlan;
    if (plan.Page == TopScreenPauseClosePage::None || plan.ResetFunction == 0U ||
        PageState(inputs, plan.Page) == 0U) {
      ++mRetired;
      return false;
    }
    add(true, plan.ResetFunction);
    add(false, kPauseSceneControl, 1U);
    add(true, kPauseInputSetTouchEnabled, 1U, 0U);
    add(false, kPauseControllerState, 0U);
    add(true, kPauseSystemMenuOpen);
    ++mSystemOpens;
    mLastSystemSourcePage = plan.Page;
    return true;
  }
  return false;
}

bool TopScreenPauseTransactionRuntime::Execute(oot3d::recomp::a32::GuestState &state,
                                               oot3d::recomp::a32::ExecutionResult *result,
                                               std::uint32_t *blocksConsumed) {
  if (!Pending()) {
    if (!Requested() || !Begin()) return false;
    if (state.r[13] < kCallStackReservation ||
        !mMemory.IsWritable(state.r[13] - kCallStackReservation, kCallStackReservation))
      throw std::runtime_error("TopScreen pause transaction has no valid caller stack");
    mCallerState = state;
  }
  while (mNextStep < mStepCount) {
    const auto step = mSteps[mNextStep++];
    if (!step.Call) {
      if (!mMemory.Write32(step.Address, step.Value0))
        throw std::runtime_error("TopScreen pause transaction state write failed");
      continue;
    }
    state = mCallerState;
    state.r[0] = step.Value0;
    state.r[1] = step.Value1;
    state.r[13] -= kCallStackReservation;
    state.r[14] = kTopScreenInputUpdateBoundary;
    state.r[15] = step.Address;
    *result = {oot3d::recomp::a32::ExitKind::Branch, step.Address,
               oot3d::recomp::a32::FallbackReason::None, kTopScreenInputUpdateBoundary};
    if (blocksConsumed != nullptr) *blocksConsumed = 1U;
    return true;
  }
  // Every call returned to the boundary: the native UI update continues.
  state = mCallerState;
  mStepCount = 0;
  mNextStep = 0;
  return false;
}

} // namespace Oot3dNativeGame
