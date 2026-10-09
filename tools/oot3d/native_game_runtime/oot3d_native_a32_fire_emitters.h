#pragma once

// Point lights for the fire the EUR title draws but never lights: actors with
// no LightContext record, lit while their flame is drawn (traced with Codex in
// the EUR code). Group 2, the flames group, as En_Light and the torches.

#include "fast/renderer3ds/pica_light_binding.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace oot3d::recomp::a32 {
class MemoryBus;
} // namespace oot3d::recomp::a32

namespace Oot3dNativeGame {

[[nodiscard]] bool IsNativeFireEmitterActor(uint16_t id) noexcept;

// Appends the lights of one fire actor (none, one, or one per jet or wall
// segment). Each light's identity is the actor's position address, with the
// emitter slot in the upper 32 bits for actors that light several points.
void CaptureNativeFireEmitters(oot3d::recomp::a32::MemoryBus& memory, uint32_t playState, uint32_t actor,
                               uint16_t id, uint16_t params,
                               std::vector<Fast::Renderer3ds::PicaScenePointLight>& lights);

// Fills the fixed snapshot from all candidates: groups 0 and 1 (Navi and the
// fairies) first, then the others nearest to the player (distance less
// reach), ties by identity, so a full room keeps the lights around Link
// whatever the actor-list order.
void SelectScenePointLights(std::span<const Fast::Renderer3ds::PicaScenePointLight> candidates,
                            const std::array<float, 3>* player,
                            Fast::Renderer3ds::PicaScenePointLights& snapshot);

} // namespace Oot3dNativeGame
