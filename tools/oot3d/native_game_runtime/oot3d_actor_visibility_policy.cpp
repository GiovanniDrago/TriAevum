#include "oot3d_actor_visibility_policy.h"

#include <algorithm>
#include <cmath>

namespace Oot3dNativeGame {
namespace {

bool Finite(const ActorCullingSample& sample) noexcept {
    return std::isfinite(sample.X) && std::isfinite(sample.Y) &&
           std::isfinite(sample.Z) && std::isfinite(sample.W) &&
           std::isfinite(sample.Forward) && std::isfinite(sample.Scale) &&
           std::isfinite(sample.Downward);
}

float Factor(float value) noexcept {
    return std::isfinite(value) ? std::max(value, 1.0F) : 1.0F;
}

ActorCullingClass Classify(const ActorCullingSample& sample, float forward,
                           float horizontal, float vertical) noexcept {
    if (!Finite(sample)) return ActorCullingClass::Outside;
    if (!(sample.Z < forward + sample.Scale)) return ActorCullingClass::Far;
    if (!(sample.Z > -sample.Scale)) return ActorCullingClass::Outside;
    // The title divides by W only from 1 up.
    const float q = sample.W < 1.0F ? 1.0F : 1.0F / sample.W;
    const bool inside = (std::abs(sample.X) - sample.Scale) * q < horizontal &&
                        (sample.Y + sample.Downward) * q > -vertical &&
                        (sample.Y - sample.Scale) * q < vertical;
    return inside ? ActorCullingClass::Inside : ActorCullingClass::Outside;
}

} // namespace

ActorCullingClass ClassifyNativeActorCulling(
    const ActorCullingSample& sample) noexcept {
    return Classify(sample, sample.Forward, 1.0F, 1.0F);
}

ActorCullingClass ClassifyExtendedActorCulling(
    const ActorCullingSample& sample,
    const ActorCullingVolume& volume) noexcept {
    const float multiplier = Factor(volume.DistanceMultiplier);
    float forward = sample.Forward;
    if (multiplier > 1.0F)
        forward = (forward + (forward < 500.0F ? 1000.0F : 0.0F)) * multiplier;
    forward = std::max(forward, sample.Forward);
    return Classify(sample, forward, Factor(volume.Horizontal), Factor(volume.Vertical));
}

float ObjMure2DistanceScalar(float nativeScalar, float projectedX,
                             float projectedZ, float horizontal,
                             bool extended) noexcept {
    if (!std::isfinite(nativeScalar)) return nativeScalar;
    float scalar = nativeScalar;
    if (extended)
        scalar = std::max(scalar, kExtendedActorDistanceMultiplier * kExtendedActorDistanceMultiplier);
    const float h = Factor(horizontal);
    if (h > 1.0F && std::isfinite(projectedX) && std::isfinite(projectedZ)) {
        // (X / h)^2 + Z^2 < T s'  <=>  X^2 + Z^2 < T s' (X^2 + Z^2) / ((X / h)^2 + Z^2).
        const float full = projectedX * projectedX + projectedZ * projectedZ;
        const float presented = (projectedX / h) * (projectedX / h) + projectedZ * projectedZ;
        if (presented > 1.0e-6F && std::isfinite(full)) scalar *= full / presented;
    }
    return scalar;
}

} // namespace Oot3dNativeGame
