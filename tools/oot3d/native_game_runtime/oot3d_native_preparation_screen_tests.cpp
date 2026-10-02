#include "oot3d_native_preparation_screen.h"

#include <cstdlib>
#include <iostream>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "oot3d_native_preparation_screen_tests: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    using Oot3dNativeGame::NativePreparationScreen;
    using Clock = NativePreparationScreen::Clock;
    using std::chrono::milliseconds;
    const auto start = Clock::time_point{} + std::chrono::hours(1);

    Fast::Renderer3ds::PicaPreparationStatus status;
    status.Parts = 39U;
    status.PreparedParts = 12U;
    status.ProgramsReady = false;
    NativePreparationScreen screen(start);
    Require(screen.HoldsGuest(status, start + milliseconds(500)), "guest ran before its programs");
    Require(screen.CoversFrame() && !screen.Done(), "screen not shown while programs prepare");
    status.PreparedParts = 39U;
    status.ProgramsReady = true;
    status.UpscalerReady = false;
    Require(screen.HoldsGuest(status, start + milliseconds(900)), "guest ran before its upscaler");
    status.UpscalerReady = true;
    Require(!screen.HoldsGuest(status, start + milliseconds(1500)), "guest held after preparation");
    Require(screen.CurrentPhase() == NativePreparationScreen::Phase::FirstFrames &&
                screen.StartupSeconds() == 1.5 && screen.WaitSeconds() == 1.0 &&
                screen.Parts() == 39U,
            "programs phase not recorded");
    Require(!screen.HoldsGuest(status, start + milliseconds(1600)), "guest held twice");

    // A slow frame (first-use work) restarts the count of steady frames.
    auto now = start + milliseconds(1500);
    for (uint32_t frame = 0; frame < NativePreparationScreen::kSteadyFrames - 1U; ++frame)
        screen.ObserveFrame(16.0, now += milliseconds(16));
    Require(screen.CoversFrame(), "first frames ended early");
    screen.ObserveFrame(1200.0, now += milliseconds(1200));
    Require(screen.SteadyFrames() == 0U && screen.CoversFrame(), "slow frame did not restart the count");
    for (uint32_t frame = 0; frame < NativePreparationScreen::kSteadyFrames; ++frame)
        screen.ObserveFrame(16.0, now += milliseconds(16));
    Require(screen.Done() && !screen.CoversFrame(), "steady frames did not end the screen");
    Require(screen.FirstFrames() == 2U * NativePreparationScreen::kSteadyFrames, "first frames miscounted");

    // The first frames never hold the screen past their limit.
    NativePreparationScreen slow(start);
    Require(!slow.HoldsGuest(status, start), "ready preparation held the guest");
    now = start;
    for (uint32_t frame = 0; frame < 25U && !slow.Done(); ++frame)
        slow.ObserveFrame(2000.0, now += milliseconds(2000));
    Require(slow.Done() && slow.FirstFramesSeconds() >= NativePreparationScreen::kFirstFramesLimitSeconds,
            "slow first frames held the screen past the limit");

    std::cout << "oot3d_native_preparation_screen_tests: ok\n";
    return 0;
}
