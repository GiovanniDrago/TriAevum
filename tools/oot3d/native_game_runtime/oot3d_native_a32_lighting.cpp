#include "oot3d_native_a32_lighting.h"

#include "recomp/a32_runtime.h"
#include "oot3d/actor_spawn.h"
#include <cstddef>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace Oot3dNativeGame {
namespace {

using LightSource = Fast::Renderer3ds::PicaNativeLightSource;

bool ReadColor(oot3d::recomp::a32::MemoryBus& memory, uint32_t address, std::array<float, 3>& color) {
    for (uint32_t channel = 0; channel < 3U; ++channel) {
        uint8_t value = 0;
        if (!memory.Read8(address + channel, &value))
            return false;
        color[channel] = static_cast<float>(value) / 255.0F;
    }
    return true;
}

bool ReadPosition(oot3d::recomp::a32::MemoryBus& memory, uint32_t address, std::array<float, 3>& position) {
    for (uint32_t axis = 0; axis < 3U; ++axis) {
        uint32_t bits = 0;
        if (!memory.Read32(address + axis * 4U, &bits))
            return false;
        const float value = std::bit_cast<float>(bits);
        if (!std::isfinite(value))
            return false;
        position[axis] = value;
    }
    return true;
}

bool CaptureFairyEmitter(oot3d::recomp::a32::MemoryBus& memory, uint32_t actor,
                         Fast::Renderer3ds::PicaScenePointLight& light) {
    // EnElf_Draw (EUR 001D6EC4) gates the glow with phase, flags, scale
    // and core opacity. Non-Navi variants leave their native LightInfos
    // dormant, so the extension emitter follows the live actor instead.
    uint16_t phase = 0, flags = 0;
    uint32_t draw = 0;
    const auto scalar = [&](uint32_t offset, float& value) {
        uint32_t bits = 0;
        if (!memory.Read32(actor + offset, &bits))
            return false;
        value = std::bit_cast<float>(bits);
        return std::isfinite(value);
    };
    float scale = 0, opacity = 0, alpha = 0;
    std::array<float, 3> position{}, color{};
    if (!memory.Read32(actor + offsetof(Oot3dActor, draw), &draw) || draw == 0 ||
        !memory.Read16(actor + 0x9A8U, &phase) || phase == 8U || !memory.Read16(actor + 0x9C4U, &flags) ||
        (flags & 8U) != 0U || !scalar(0x54U, scale) || scale == 0 || !scalar(0x924U, opacity) || opacity <= 0 ||
        !scalar(0x934U, alpha) || alpha <= 0 || !ReadPosition(memory, actor + 0x28U, position) ||
        !ReadPosition(memory, actor + 0x928U, color))
        return false;
    const float emission = std::clamp(opacity, 0.0F, 1.0F) * std::clamp(alpha / 255.0F, 0.0F, 1.0F);
    light.PositionWorld = position;
    for (size_t channel = 0; channel < color.size(); ++channel)
        light.Color[channel] = std::clamp(color[channel] / 255.0F, 0.0F, 1.0F) * emission;
    // Title extension default, in world units, matching Navi's normal reach.
    // Environment owns the user's radius multiplier; native state is untouched.
    light.Radius = 100.0F;
    return true;
}

} // namespace

void NativeA32LightingProbe::ObserveAcceptedLight(uint32_t lights, uint32_t source, uint32_t record, bool point,
                                                  oot3d::recomp::a32::MemoryBus& memory) {
    // Lights holds a 16-byte header and seven 16-byte native records. The
    // observed instruction runs only after the guest accepted this record.
    const uint64_t first = static_cast<uint64_t>(lights) + 0x10U;
    if (record < first || record - first >= 7U * 0x10U || ((record - first) & 0xFU) != 0U ||
        source > std::numeric_limits<uint32_t>::max() - 0x14U)
        return;
    const size_t slot = static_cast<size_t>((record - first) / 0x10U);
    auto& bound = mBoundLights[lights];
    auto& light = bound.Sources[slot];
    light = {};
    bound.Count = std::max(bound.Count, static_cast<uint8_t>(slot + 1U));
    if (!ReadColor(memory, source + (point ? 0xCU : 3U), light.UnattenuatedColor))
        return;
    if (point) {
        uint16_t radius = 0;
        if (!ReadPosition(memory, source, light.PositionWorld) || !memory.Read16(source + 0x10U, &radius))
            return;
        light.Radius = std::abs(static_cast<float>(std::bit_cast<int16_t>(radius)));
        if (light.Radius == 0.0F)
            return;
    }
    light.Kind = point ? LightSource::Type::Point : LightSource::Type::Directional;
    light.SourceIdentity = source;
}

void NativeA32LightingProbe::ObserveAssignment(const oot3d::recomp::a32::GuestState& state,
                                               oot3d::recomp::a32::MemoryBus& memory) {
    if (state.r[1] >= 3U)
        return;
    if (const auto found = mRenderStates.find(state.r[0]); found != mRenderStates.end())
        found->second.Sources[state.r[1]] = {};
    // At the native Cmb direction setter's demonstrated callsite, r5 is the
    // selected Light_t and r6 is its Lights owner. Selection (including the
    // >3-light strongest-point branch and its tie behavior) remains native.
    if (state.r[14] != 0x0035BEE8U)
        return;
    const auto found = mBoundLights.find(state.r[6]);
    if (found == mBoundLights.end())
        return;
    const uint64_t first = static_cast<uint64_t>(state.r[6]) + 0x10U;
    const uint32_t record = state.r[5];
    if (record < first || ((record - first) & 0xFU) != 0U)
        return;
    const uint64_t slot = (record - first) / 0x10U;
    if (slot >= found->second.Count)
        return;
    auto light = found->second.Sources[static_cast<size_t>(slot)];
    if (!ReadColor(memory, record, light.ReferenceColor))
        return;
    mRenderStates[state.r[0]].Sources[state.r[1]] = light;
}

void NativeA32LightingProbe::ObserveBlockEntry(uint32_t pc, const oot3d::recomp::a32::GuestState& state,
                                               oot3d::recomp::a32::MemoryBus& memory) {
    switch (pc) {
        case 0x00213DFCU:
        case 0x00253A74U:
            ObserveAcceptedLight(state.r[0], state.r[1], state.r[2], pc == 0x00213DFCU, memory);
            break;
        case 0x002CF63CU:
            if (const auto found = mBoundLights.find(state.r[1]); found != mBoundLights.end())
                found->second = {};
            break;
        case 0x0035BFB4U: {
            uint32_t cursor = 0;
            if (state.r[1] <= std::numeric_limits<uint32_t>::max() - 0xCU &&
                memory.Read32(state.r[1] + 0xCU, &cursor) && cursor >= 0x80U) {
                if (const auto found = mBoundLights.find(cursor - 0x80U); found != mBoundLights.end())
                    found->second = {};
            }
            break;
        }
        case 0x0035BBE0U:
        case 0x00340F44U:
            // The ambient-only owner never binds source directions.
            if (const auto found = mRenderStates.find(state.r[1]); found != mRenderStates.end())
                found->second = {};
            break;
        case 0x00310F7CU:
            if (const auto found = mRenderStates.find(state.r[1]); found != mRenderStates.end())
                mRenderStates[state.r[0]] = found->second;
            else if (const auto destination = mRenderStates.find(state.r[0]); destination != mRenderStates.end())
                destination->second = {};
            break;
        case 0x0033D174U:
            if (state.r[1] < 3U) {
                if (const auto found = mRenderStates.find(state.r[0]); found != mRenderStates.end())
                    found->second.Sources[state.r[1]] = {};
            }
            break;
        case 0x0033D14CU:
            ObserveAssignment(state, memory);
            break;
        default:
            break;
    }
}

void NativeA32LightingProbe::CaptureSceneLights(uint32_t playState, oot3d::recomp::a32::MemoryBus& memory) {
    using namespace Fast::Renderer3ds;
    PicaSceneDirectionalLights directionals;
    // EUR Gameplay_Init (00449218..00449228) passes PlayState+3190 to
    // Environment_Init, which inserts LightInfo records at +2C and +44.
    // Read only records still linked in this PlayState's LightContext.
    if (playState != 0 && playState <= std::numeric_limits<uint32_t>::max() - 0x31DDU) {
        uint32_t node = 0;
        if (memory.Read32(playState + 0xA70U, &node)) {
            for (uint32_t visited = 0; node != 0 && visited < 32U; ++visited) {
                uint32_t info = 0, next = 0;
                if (node > std::numeric_limits<uint32_t>::max() - 8U ||
                    !memory.Read32(node, &info) || !memory.Read32(node + 8U, &next))
                    break;
                if (info == playState + 0x31BCU || info == playState + 0x31D4U) {
                    uint8_t kind = 0;
                    PicaSceneDirectionalLight light;
                    bool valid = memory.Read8(info, &kind) && kind == 1U &&
                        ReadColor(memory, info + 7U, light.Color);
                    for (uint32_t axis = 0; valid && axis < 3U; ++axis) {
                        uint8_t component = 0;
                        valid = memory.Read8(info + 4U + axis, &component);
                        // Native assignment negates /127 before CMB upload
                        // (0035BDF8..0035BEE4); the source itself points toward light.
                        light.DirectionTowardSource[axis] = float(std::bit_cast<int8_t>(component)) / 127.0F;
                    }
                    light.SourceIdentity = info + 4U;
                    const auto end = directionals.Lights.begin() + directionals.Count;
                    if (valid && directionals.Count < directionals.Lights.size() &&
                        std::none_of(directionals.Lights.begin(), end, [&](const auto& existing) {
                            return existing.SourceIdentity == light.SourceIdentity;
                        }))
                        directionals.Lights[directionals.Count++] = light;
                }
                node = next;
            }
        }
    }
    if (directionals.Count == 0)
        mSceneDirectionals.reset();
    else if (!mSceneDirectionals || *mSceneDirectionals != directionals)
        mSceneDirectionals = std::make_shared<const PicaSceneDirectionalLights>(std::move(directionals));
    PicaScenePointLights snapshot;
    // EUR LightContext_InsertLight (0034FAA8) owns a 32-node pool; only
    // records still linked through their real native LightNode are accepted.
    static_assert(kMaximumPicaScenePointLights >= 32U);
    if (playState != 0 && playState <= std::numeric_limits<uint32_t>::max() - OOT3D_ACTOR_CONTEXT_PLAY_OFFSET -
                                           sizeof(Oot3dActorContext)) {
        const uint32_t context = playState + OOT3D_ACTOR_CONTEXT_PLAY_OFFSET;
        for (uint32_t category = 0; category < OOT3D_ACTOR_CATEGORY_COUNT; ++category) {
            const uint32_t list = context + offsetof(Oot3dActorContext, lists) + category * sizeof(Oot3dActorListEntry);
            uint32_t count = 0, actor = 0;
            if (!memory.Read32(list, &count) || !memory.Read32(list + 4U, &actor))
                continue;
            for (uint32_t i = 0; actor != 0 && i < std::min(count, 255U); ++i) {
                if (actor > std::numeric_limits<uint32_t>::max() - 0x9D0U)
                    break;
                uint16_t id = 0, params = 0;
                uint32_t next = 0, update = 0;
                uint8_t actualCategory = 0;
                if (!memory.Read16(actor + offsetof(Oot3dActor, id), &id) ||
                    !memory.Read8(actor + offsetof(Oot3dActor, category), &actualCategory) ||
                    actualCategory != category || !memory.Read32(actor + offsetof(Oot3dActor, next), &next) ||
                    !memory.Read32(actor + offsetof(Oot3dActor, update), &update))
                    break;
                // En_Elf's native profile owns both records. params 0 is Navi;
                // all other En_Elf variants belong to the fairy control group.
                if (id == 0x18U && update != 0 && memory.Read16(actor + offsetof(Oot3dActor, params), &params)) {
                    for (uint32_t offset : { 0x948U, 0x964U }) {
                        const uint32_t info = actor + offset;
                        uint32_t node = 0, owner = 0;
                        uint16_t radius = 0;
                        PicaScenePointLight light;
                        if (!memory.Read32(info + 0x18U, &node) || node == 0 || !memory.Read32(node, &owner) ||
                            owner != info || !ReadPosition(memory, info + 4U, light.PositionWorld) ||
                            !ReadColor(memory, info + 0x10U, light.Color) || !memory.Read16(info + 0x14U, &radius))
                            continue;
                        light.SourceIdentity = info + 4U;
                        light.Radius = std::max(0.0F, float(std::bit_cast<int16_t>(radius)));
                        light.Group = params == 0 ? 0U : 1U;
                        if (light.Group == 1U && offset == 0x948U && light.Radius == 0.0F)
                            (void)CaptureFairyEmitter(memory, actor, light);
                        const auto end = snapshot.Lights.begin() + snapshot.Count;
                        if (std::none_of(snapshot.Lights.begin(), end,
                                         [&](const auto& existing) {
                                             return existing.SourceIdentity == light.SourceIdentity;
                                         }) &&
                            snapshot.Count < snapshot.Lights.size())
                            snapshot.Lights[snapshot.Count++] = light;
                    }
                }
                actor = next;
            }
        }
    }
    if (snapshot.Count == 0)
        mScenePoints.reset();
    else if (!mScenePoints || *mScenePoints != snapshot)
        mScenePoints = std::make_shared<const PicaScenePointLights>(std::move(snapshot));
}

Fast::Renderer3ds::PicaNativeLightBinding
NativeA32LightingProbe::CaptureRenderer(uint32_t renderer, oot3d::recomp::a32::MemoryBus& memory) const {
    Fast::Renderer3ds::PicaNativeLightBinding result;
    uint32_t renderState = 0;
    if (renderer <= std::numeric_limits<uint32_t>::max() - 0x10U && memory.Read32(renderer + 0x10U, &renderState)) {
        if (const auto found = mRenderStates.find(renderState); found != mRenderStates.end())
            result = found->second;
    }
    result.WorldToView = CaptureWorldToView(memory);
    result.ScenePoints = mScenePoints;
    result.SceneDirectionals = mSceneDirectionals;
    return result;
}

std::optional<std::array<float, 16>> NativeA32LightingProbe::CaptureWorldToView(oot3d::recomp::a32::MemoryBus& memory) {
    // EUR revision 0: the matrices returned by 00313644 and 00466F3C.
    // CmbRenderState_PrepareLights multiplies post-view * world-to-view;
    // the bone-palette producer uses the same world-to-view for geometry.
    // Capture at packet submission, before another viewport can replace them.
    constexpr std::array<uint32_t, 2> addresses{ 0x005B5018U, 0x005B5058U };
    std::array<std::array<float, 12>, 2> matrices{};
    for (size_t matrix = 0; matrix < matrices.size(); ++matrix) {
        for (size_t component = 0; component < matrices[matrix].size(); ++component) {
            uint32_t word = 0;
            if (!memory.Read32(addresses[matrix] + static_cast<uint32_t>(component * 4U), &word))
                return std::nullopt;
            matrices[matrix][component] = std::bit_cast<float>(word);
            if (!std::isfinite(matrices[matrix][component]))
                return std::nullopt;
        }
    }
    std::array<float, 16> worldToView{};
    worldToView[15] = 1.0F;
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            float value = column == 3 ? matrices[1][row * 4 + 3] : 0.0F;
            for (size_t axis = 0; axis < 3; ++axis)
                value += matrices[1][row * 4 + axis] * matrices[0][axis * 4 + column];
            worldToView[column * 4 + row] = value;
        }
    }
    return worldToView;
}

void NativeA32LightingProbe::Reset() noexcept {
    mBoundLights.clear();
    mRenderStates.clear();
    mScenePoints.reset();
    mSceneDirectionals.reset();
}

} // namespace Oot3dNativeGame
