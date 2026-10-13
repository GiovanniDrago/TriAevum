#include "oot3d_top_screen_pause_transaction.h"

#include <cstdlib>
#include <iostream>

namespace {
void Require(bool condition, const char *message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

constexpr std::uint32_t kItemsState = 0x0050672CU;
constexpr std::uint32_t kControllerState = 0x0050AF68U;
constexpr std::uint32_t kSceneControl = 0x005043E8U;
constexpr std::uint32_t kItemsReset = 0x002EB628U;
constexpr std::uint32_t kSetTouchEnabled = 0x002FD84CU;
constexpr std::uint32_t kSystemMenuOpen = 0x0043ACA8U;

oot3d::recomp::a32::GuestState CallerState() {
  oot3d::recomp::a32::GuestState state;
  state.r[0] = 123;
  state.r[4] = 456;
  state.r[13] = 0x005FF000;
  state.r[14] = 0x00400000;
  state.r[15] = Oot3dNativeGame::kTopScreenInputUpdateBoundary;
  state.vfp[0] = 789;
  state.cpsr = 0xA0000010;
  return state;
}

bool Same(const oot3d::recomp::a32::GuestState &a, const oot3d::recomp::a32::GuestState &b) {
  for (std::size_t index = 0; index < 16U; ++index)
    if (a.r[index] != b.r[index]) return false;
  return a.vfp[0] == b.vfp[0] && a.cpsr == b.cpsr;
}

// One guest call returns to the boundary, as the native callee's `bx lr` does.
bool CallsAndReturns(Oot3dNativeGame::TopScreenPauseTransactionRuntime &runtime,
                     oot3d::recomp::a32::GuestState &state, std::uint32_t entry,
                     std::uint32_t r0, std::uint32_t r1) {
  oot3d::recomp::a32::ExecutionResult result;
  std::uint32_t blocks = 0;
  if (!runtime.Execute(state, &result, &blocks) || !runtime.Pending()) return false;
  const bool ok = state.r[15] == entry && result.pc == entry && state.r[0] == r0 &&
                  state.r[1] == r1 && state.r[13] == 0x005FF000 - 0x20 &&
                  state.r[14] == Oot3dNativeGame::kTopScreenInputUpdateBoundary && blocks == 1;
  state.r[15] = Oot3dNativeGame::kTopScreenInputUpdateBoundary;
  state.r[0] = 0xDEAD;
  return ok;
}
} // namespace

int main() {
  using namespace Oot3dNativeGame;
  NativeA32Memory memory;
  std::string error;
  Require(memory.MapRegion({"pause-transaction-test", 0x00300000, 0x00300000, true, false, {}}, &error),
          "cannot map pause fixture");
  TopScreenPauseTransactionRuntime runtime(memory);
  oot3d::recomp::a32::ExecutionResult result;
  std::uint32_t blocks = 0;

  auto state = CallerState();
  Require(!runtime.Execute(state, &result, &blocks) && Same(state, CallerState()),
          "an idle runtime entered the guest");

  // START on the open Items page: the original reset, the controller and scene
  // writes, then the touch service, and the native update resumes unchanged.
  Require(memory.Write32(kItemsState, 2) && memory.Write32(kControllerState, 0) &&
              memory.Write32(kSceneControl, 7),
          "cannot seed an open Items page");
  runtime.RequestStartClose();
  Require(runtime.NeedsExecution() && !runtime.Pending(), "START close was not queued");
  Require(CallsAndReturns(runtime, state, kItemsReset, 0, 0), "START did not call the Items reset");
  std::uint32_t value = 0;
  Require(memory.Read32(kControllerState, &value) && value == 0,
          "pause state written before the page reset returned");
  Require(CallsAndReturns(runtime, state, kSetTouchEnabled, 0, 1),
          "START did not restore pause touch ownership");
  Require(memory.Read32(kControllerState, &value) && value == 2 &&
              memory.Read32(kSceneControl, &value) && value == 0,
          "START did not write the payload close state");
  Require(!runtime.Execute(state, &result, &blocks) && !runtime.NeedsExecution() &&
              Same(state, CallerState()),
          "the native update did not resume with its own registers");
  Require(runtime.StartCloses() == 1 && runtime.LastClosedPage() == TopScreenPauseClosePage::Items,
          "START close was not recorded");

  // A page that closed before delivery is not reset again.
  Require(memory.Write32(kItemsState, 0), "cannot close the fixture page");
  runtime.RequestStartClose();
  Require(!runtime.Execute(state, &result, &blocks) && !runtime.NeedsExecution() &&
              runtime.Retired() == 1 && Same(state, CallerState()),
          "a START close without a page reached the guest");

  // A request the native update never reaches expires.
  runtime.RequestStartClose();
  for (int refresh = 0; refresh < 8; ++refresh) runtime.AgeUndelivered();
  Require(runtime.Requested(), "a request expired before the native update could run");
  runtime.AgeUndelivered();
  Require(!runtime.Requested() && runtime.Retired() == 2, "an undelivered request survived");

  // B to Save/Quit from a closing Items page, in payload order.
  Require(memory.Write32(kItemsState, 0xE) && memory.Write32(kControllerState, 2),
          "cannot seed a closing Items page");
  runtime.RequestSystemOpen({TopScreenPauseClosePage::Items, kItemsReset});
  Require(CallsAndReturns(runtime, state, kItemsReset, 0, 0), "Save/Quit did not reset Items");
  Require(CallsAndReturns(runtime, state, kSetTouchEnabled, 1, 0),
          "Save/Quit did not hand touch to the system menu");
  Require(memory.Read32(kSceneControl, &value) && value == 1 &&
              memory.Read32(kControllerState, &value) && value == 2,
          "Save/Quit wrote out of payload order");
  Require(CallsAndReturns(runtime, state, kSystemMenuOpen, 0, 0), "Save/Quit did not open the menu");
  Require(memory.Read32(kControllerState, &value) && value == 0,
          "Save/Quit did not clear the pause controller");
  Require(!runtime.Execute(state, &result, &blocks) && Same(state, CallerState()) &&
              runtime.SystemOpens() == 1 &&
              runtime.LastSystemSourcePage() == TopScreenPauseClosePage::Items,
          "Save/Quit did not complete");

  // START wins over Save/Quit requested in the same refresh.
  Require(memory.Write32(kItemsState, 2), "cannot reopen the fixture page");
  runtime.RequestStartClose();
  runtime.RequestSystemOpen({TopScreenPauseClosePage::Items, kItemsReset});
  Require(CallsAndReturns(runtime, state, kItemsReset, 0, 0) &&
              CallsAndReturns(runtime, state, kSetTouchEnabled, 0, 1) &&
              !runtime.Execute(state, &result, &blocks) && runtime.StartCloses() == 2 &&
              runtime.SystemOpens() == 1,
          "Save/Quit replaced a START close");

  // A savestate load discards an in-flight transaction.
  runtime.RequestStartClose();
  Require(CallsAndReturns(runtime, state, kItemsReset, 0, 0), "cannot start a transaction");
  runtime.Reset();
  Require(!runtime.NeedsExecution(), "reset kept an in-flight transaction");
  std::cout << "pause transaction tests passed\n";
  return 0;
}
