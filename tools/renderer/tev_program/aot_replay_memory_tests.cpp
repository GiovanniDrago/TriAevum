#include "aot_replay_memory.h"
#include <iostream>
#include <memory>

using Oot3dNativeGame::NativeA32Memory;

int main() {
    NativeA32Memory initial;
    std::string error;
    if (!initial.MapRegion({"partial", 0x1101U, 8192U, true, false, {}}, &error) ||
        !initial.MapRegion({"code", 0x8000U, 4096U, true, true, {}}, &error))
        throw std::runtime_error(error);
    const auto fingerprint = initial.ContentFingerprint();
    NativeA32Memory output(initial);
    const auto mutate = [&] {
        return output.WriteFast<uint8_t>(0x1101U, 0x55U) &&
               output.WriteFast<uint32_t>(0x2004U, 0x12345678U) &&
               output.WriteFast<uint8_t>(0x3100U, 0xAAU);
    };
    if (!mutate()) throw std::runtime_error("Cannot prepare replay writes");
    auto pages = std::make_unique<std::array<uint8_t*, 1U << 20>>();
    AotReplayMemoryReset reset(initial, output, *pages);
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        reset.Restore();
        if (output.ContentFingerprint() != fingerprint || initial.ContentFingerprint() != fingerprint)
            throw std::runtime_error("Partial mapping reset changed initial bytes or missed a boundary");
        if (!mutate()) throw std::runtime_error("Cannot repeat replay writes");
    }
    reset.Restore();
    if (!output.WriteFast<uint32_t>(0x8000U, 0xE12FFF1EU))
        throw std::runtime_error("Cannot prepare executable write");
    bool rejected = false;
    try { AotReplayMemoryReset unsupported(initial, output, *pages); }
    catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error("Self-modifying execution cannot qualify a cached JIT replay");
    std::cout << "Replay reset: partial head, full page, partial tail, repeated state, executable write passed\n";
}
