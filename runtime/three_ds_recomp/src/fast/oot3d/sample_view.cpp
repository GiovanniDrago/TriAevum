#include "fast/oot3d/sample_view.h"

#include "fast/oot3d/directional_shadows.h"

#include <algorithm>
#include <cmath>

namespace Fast::Oot3d {
namespace {

bool SameView(const SampleViewMatrix& left, const SampleViewMatrix& right) noexcept {
    for (size_t i = 0; i < left.size(); ++i) {
        const float tolerance = 1.0e-4F * std::max(1.0F, std::abs(left[i]));
        if (!(std::abs(left[i] - right[i]) <= tolerance)) return false;
    }
    return true;
}

} // namespace

void SampleViewAccumulator::Observe(uint64_t frame, const SampleViewMatrix& worldToView) noexcept {
    for (const float value : worldToView)
        if (!std::isfinite(value)) return;
    if (frame != mFrame) {
        mFrame = frame;
        mCount = 0;
    }
    for (uint32_t i = 0; i < mCount; ++i) {
        if (SameView(mCandidates[i].View, worldToView)) {
            ++mCandidates[i].Draws;
            return;
        }
    }
    if (mCount < kCandidates) mCandidates[mCount++] = { worldToView, 1U };
}

std::optional<SampleViewMatrix> SampleViewAccumulator::Resolve(uint64_t frame) const noexcept {
    if (frame != mFrame || mCount == 0U) return std::nullopt;
    const auto* best = &mCandidates[0];
    for (uint32_t i = 1; i < mCount; ++i)
        if (mCandidates[i].Draws > best->Draws) best = &mCandidates[i];
    return best->View;
}

uint32_t SampleViewAccumulator::DistinctViews(uint64_t frame) const noexcept {
    return frame == mFrame ? mCount : 0U;
}

PerspectiveViewState ResolveSamplePerspective(const PerspectiveViewState& guest,
                                              const SampleViewMatrix& worldToView) noexcept {
    DirectionalShadowMatrix viewToWorld{};
    if (!InvertDirectionalShadowMatrix(worldToView, viewToWorld)) return guest;
    PerspectiveViewState result = guest;
    result.Serial = 0U;
    for (size_t column = 0; column < 4; ++column)
        for (size_t row = 0; row < 4; ++row) {
            float value = 0.0F;
            for (size_t k = 0; k < 4; ++k) value += guest.Projection[k * 4 + row] * worldToView[column * 4 + k];
            result.WorldToClip[column * 4 + row] = value;
        }
    const float w = viewToWorld[15] != 0.0F ? viewToWorld[15] : 1.0F;
    result.Eye = { viewToWorld[12] / w, viewToWorld[13] / w, viewToWorld[14] / w };
    // The view looks down -Z.
    std::array<float, 3> forward{ -viewToWorld[8], -viewToWorld[9], -viewToWorld[10] };
    const float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    if (length <= 1.0e-12F) return guest;
    float distance = 1.0F;
    if (guest.CameraAvailable) {
        const float dx = guest.At[0] - guest.Eye[0], dy = guest.At[1] - guest.Eye[1], dz = guest.At[2] - guest.Eye[2];
        distance = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1.0e-3F);
    }
    for (size_t i = 0; i < 3; ++i) result.At[i] = result.Eye[i] + forward[i] / length * distance;
    result.CameraAvailable = true;
    return result;
}

} // namespace Fast::Oot3d
