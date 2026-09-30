#include "fast/oot3d/effect_transient_configuration_cache.h"

namespace Fast::Oot3d {
namespace {

[[nodiscard]] bool SameSlot(
    const EffectTransientAllocationSlot& lhs,
    const EffectTransientAllocationSlot& rhs) noexcept {
    return lhs.StorageClass == rhs.StorageClass &&
           lhs.Format == rhs.Format && lhs.Width == rhs.Width &&
           lhs.Height == rhs.Height && lhs.MipLevels == rhs.MipLevels &&
           lhs.Layers == rhs.Layers && lhs.Samples == rhs.Samples &&
           lhs.Usage == rhs.Usage && lhs.ResourceMask == rhs.ResourceMask &&
           lhs.ResourceCount == rhs.ResourceCount &&
           lhs.FirstUse == rhs.FirstUse && lhs.LastUse == rhs.LastUse;
}

[[nodiscard]] bool SameRequirement(
    const EffectTransientImageRequirement& lhs,
    const EffectTransientImageRequirement& rhs) noexcept {
    return lhs.Resource == rhs.Resource && lhs.Format == rhs.Format &&
           lhs.Width == rhs.Width && lhs.Height == rhs.Height &&
           lhs.MipLevels == rhs.MipLevels && lhs.Layers == rhs.Layers &&
           lhs.Samples == rhs.Samples && lhs.Usage == rhs.Usage;
}

[[nodiscard]] bool SameResource(
    const EffectTransientAllocationResource& lhs,
    const EffectTransientAllocationResource& rhs) noexcept {
    return SameRequirement(lhs.Requirement, rhs.Requirement) &&
           lhs.StorageClass == rhs.StorageClass &&
           lhs.FirstUse == rhs.FirstUse && lhs.LastUse == rhs.LastUse &&
           lhs.Slot == rhs.Slot;
}

} // namespace

bool SameEffectTransientAllocation(
    const EffectTransientAllocationPlan& lhs,
    const EffectTransientAllocationPlan& rhs) noexcept {
    if (lhs.SlotCount() != rhs.SlotCount() ||
        lhs.ResourceCount() != rhs.ResourceCount()) {
        return false;
    }
    for (size_t index = 0U; index < lhs.SlotCount(); ++index) {
        if (!SameSlot(lhs.SlotAt(index), rhs.SlotAt(index))) return false;
    }
    for (size_t index = 0U; index < lhs.ResourceCount(); ++index) {
        if (!SameResource(lhs.ResourceAt(index), rhs.ResourceAt(index))) {
            return false;
        }
    }
    return true;
}

EffectTransientConfigurationCache::Selection
EffectTransientConfigurationCache::Select(
    const EffectTransientAllocationPlan& plan) const noexcept {
    for (size_t index = 0U; index < kCapacity; ++index) {
        if (mOccupied[index] && SameEffectTransientAllocation(mPlans[index], plan)) {
            return {index, true, false};
        }
    }
    for (size_t index = 0U; index < kCapacity; ++index) {
        if (!mOccupied[index]) return {index, false, false};
    }
    size_t oldest = 0U;
    for (size_t index = 1U; index < kCapacity; ++index) {
        if (mLastUse[index] < mLastUse[oldest]) oldest = index;
    }
    return {oldest, false, true};
}

void EffectTransientConfigurationCache::Commit(
    size_t index, const EffectTransientAllocationPlan& plan) noexcept {
    if (index >= kCapacity) return;
    mPlans[index] = plan;
    mOccupied[index] = true;
    mLastUse[index] = ++mClock;
}

void EffectTransientConfigurationCache::Remove(size_t index) noexcept {
    if (index >= kCapacity) return;
    mPlans[index] = {};
    mOccupied[index] = false;
    mLastUse[index] = 0U;
}

void EffectTransientConfigurationCache::Clear() noexcept {
    mPlans = {};
    mOccupied = {};
    mLastUse = {};
}

size_t EffectTransientConfigurationCache::Count() const noexcept {
    size_t count = 0U;
    for (const bool occupied : mOccupied) count += occupied ? 1U : 0U;
    return count;
}

} // namespace Fast::Oot3d
