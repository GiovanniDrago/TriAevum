#pragma once

#include "fast/renderer3ds/pica_light_binding.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <optional>

namespace oot3d::recomp::a32 {
class MemoryBus;
struct GuestState;
} // namespace oot3d::recomp::a32

namespace Oot3dNativeGame {

// EUR revision 0. The point/directional sites are the accepted native record
// writes, after distance/capacity selection; observing them avoids reproducing
// selection with host floating-point arithmetic. No guest code is replaced.
inline constexpr std::array kNativeA32LightingHookPcs{ 0x00213DFCU, 0x00253A74U, 0x002CF63CU, 0x0035BFB4U, 0x0035BBE0U,
                                                       0x00340F44U, 0x00310F7CU, 0x0033D174U, 0x0033D14CU };

// The native environment's light state for one guest frame (EUR, read at
// the start of Gameplay_Draw, after Environment_Update).
struct NativeEnvironmentState {
    uint16_t Scene = 0;
    uint16_t DayTime = 0;
    uint8_t LightMode = 0;
    uint8_t SettingOverride = 0;
    uint8_t Setting = 0;
    uint8_t PreviousSetting = 0;
    uint8_t Blending = 0;
    float BlendWeight = 0.0F;
    bool operator==(const NativeEnvironmentState&) const = default;

    // The lights follow the clock: lightMode zero, no light setting override.
    [[nodiscard]] bool TimeDriven() const noexcept { return LightMode == 0U && SettingOverride == 0xFFU; }
};

struct NativeSceneLightTraceRecord {
    NativeEnvironmentState Environment;
    std::array<std::array<uint8_t, 6>, 2> Lights{};
    uint32_t LightCount = 0;
    bool operator==(const NativeSceneLightTraceRecord&) const = default;
};

class NativeA32LightingProbe {
  public:
    void ObserveBlockEntry(uint32_t pc, const oot3d::recomp::a32::GuestState& state,
                           oot3d::recomp::a32::MemoryBus& memory);
    [[nodiscard]] Fast::Renderer3ds::PicaNativeLightBinding
    CaptureRenderer(uint32_t renderer, oot3d::recomp::a32::MemoryBus& memory) const;
    [[nodiscard]] static std::optional<std::array<float, 16>> CaptureWorldToView(oot3d::recomp::a32::MemoryBus& memory);
    void CaptureSceneLights(uint32_t playState, oot3d::recomp::a32::MemoryBus& memory);
    void Reset() noexcept;

  private:
    struct BoundLights {
        std::array<Fast::Renderer3ds::PicaNativeLightSource, 7> Sources{};
        uint8_t Count = 0;
    };

    void ObserveAcceptedLight(uint32_t lights, uint32_t source, uint32_t record, bool point,
                              oot3d::recomp::a32::MemoryBus& memory);
    void ObserveAssignment(const oot3d::recomp::a32::GuestState& state, oot3d::recomp::a32::MemoryBus& memory);

    std::unordered_map<uint32_t, BoundLights> mBoundLights;
    std::unordered_map<uint32_t, Fast::Renderer3ds::PicaNativeLightBinding> mRenderStates;
    std::shared_ptr<const Fast::Renderer3ds::PicaScenePointLights> mScenePoints;
    std::shared_ptr<const Fast::Renderer3ds::PicaSceneDirectionalLights> mSceneDirectionals;
    uint64_t mSceneLightFrames = 0;
    NativeSceneLightTraceRecord mSceneLightTrace;
};

} // namespace Oot3dNativeGame
