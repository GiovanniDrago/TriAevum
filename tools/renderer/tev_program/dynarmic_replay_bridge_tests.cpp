#include "dynarmic_replay_bridge.h"
#include <windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
struct Memory {
    std::array<uint8_t, 8192> Bytes{};
    bool Writable = true;
    static bool Read(void* user, uint32_t address, uint8_t size, uint64_t* result) {
        auto& memory = *static_cast<Memory*>(user);
        if (address < 0x1000 || uint64_t{address} + size > 0x3000) return false;
        *result = 0;
        std::memcpy(result, memory.Bytes.data() + address - 0x1000, size);
        return true;
    }
    static bool Write(void* user, uint32_t address, uint8_t size, uint64_t value) {
        auto& memory = *static_cast<Memory*>(user);
        if (!memory.Writable || address < 0x2000 || uint64_t{address} + size > 0x3000) return false;
        std::memcpy(memory.Bytes.data() + address - 0x1000, &value, size);
        return true;
    }
    static bool Executable(void*, uint32_t address) { return address >= 0x1000 && address < 0x2000; }
    uint32_t Value() const { uint32_t value; std::memcpy(&value, Bytes.data() + 4096, 4); return value; }
    void Value(uint32_t value) { std::memcpy(Bytes.data() + 4096, &value, 4); }
};
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
DynarmicReplayState State() {
    DynarmicReplayState state{};
    state.Registers[0] = 0x2000;
    state.Registers[14] = 0x1100;
    state.Registers[15] = 0x1000;
    state.Cpsr = 0x10;
    return state;
}
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2, "bridge DLL argument required");
        auto dll = LoadLibraryA(argv[1]);
        Require(dll != nullptr, "cannot load bridge");
        auto query = reinterpret_cast<DynarmicReplayQuery>(GetProcAddress(dll, "triaevum_dynarmic_replay_api"));
        Require(query != nullptr, "missing bridge API");
        const auto* api = query();
        Require(api && api->Version == 1, "bridge API mismatch");
        for (bool fast : {false, true}) {
            Memory memory;
            const std::array<uint32_t, 4> code{0xE5901000, 0xE2811001, 0xE5801000, 0xE12FFF1E};
            std::memcpy(memory.Bytes.data(), code.data(), sizeof(code));
            const uint32_t svc = 0xEFABCDEF;
            std::memcpy(memory.Bytes.data() + 0x100, &svc, 4);
            const uint32_t loop = 0xEAFFFFFE;
            std::memcpy(memory.Bytes.data() + 0x20, &loop, 4);
            auto pages = std::make_unique<std::array<uint8_t*, 1U << 20>>();
            (*pages)[2] = memory.Bytes.data() + 4096;
            DynarmicReplayConfig config{&memory, Memory::Read, Memory::Write, Memory::Executable,
                fast ? pages->data() : nullptr, 0x1000, 0x1100, nullptr, 0};
            auto replay = api->Create(&config);
            Require(replay != nullptr, "create failed");
            for (uint32_t input : {0U, 41U, 0xFFFFFFFFU}) {
                memory.Value(input);
                auto state = State();
                const auto result = api->Execute(replay, &state, 1000);
                Require(result.Exit == DynarmicReplayExit::Returned && result.Instructions == 4 &&
                    state.Registers[15] == 0x1100 && state.Registers[1] == input + 1 && memory.Value() == input + 1,
                    "return boundary or warm-state reset changed guest behavior");
            }
            auto state = State();
            state.Registers[15] = 0x1020;
            auto result = api->Execute(replay, &state, 16);
            Require(result.Exit == DynarmicReplayExit::Budget && state.Registers[15] == 0x1020,
                    "backward loop must stop on its instruction budget");
            state = State(); state.Registers[0] = 0x2FFF;
            result = api->Execute(replay, &state, 1000);
            Require(result.Exit == DynarmicReplayExit::MemoryFault, "cross-page access must preserve fault");
            api->Destroy(replay);
            const uint32_t observer = 0x1008;
            config.Observers = &observer; config.ObserverCount = 1;
            replay = api->Create(&config);
            memory.Value(9); state = State();
            result = api->Execute(replay, &state, 1000);
            Require(result.Exit == DynarmicReplayExit::Observer && memory.Value() == 9,
                    "observer must stop before its guest store");
            api->Destroy(replay);
        }
        std::cout << "Dynarmic checked/page-table replay: return, warm reset, overflow, budget, fault, observer passed\n";
        FreeLibrary(dll);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
