#include "fast/oot3d/grass_gpu_memory.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>

namespace Fast::Oot3d {
namespace {

std::atomic<int64_t> gDeviceLocal{0};
std::atomic<int64_t> gHostVisible{0};

uint64_t RefusalThreshold() {
    static const uint64_t threshold = [] {
        const char* value = std::getenv("TRIAEVUM_GRASS_FAIL_ALLOCATIONS_ABOVE_MIB");
        return value == nullptr || *value == '\0' ? UINT64_MAX : std::strtoull(value, nullptr, 10) << 20U;
    }();
    return threshold;
}

} // namespace

void TrackGrassGpuMemory(bool deviceLocal, int64_t bytes) noexcept {
    (deviceLocal ? gDeviceLocal : gHostVisible).fetch_add(bytes, std::memory_order_relaxed);
}

GrassGpuMemoryUse GrassGpuMemoryInUse() noexcept {
    return {uint64_t(std::max<int64_t>(0, gDeviceLocal.load(std::memory_order_relaxed))),
            uint64_t(std::max<int64_t>(0, gHostVisible.load(std::memory_order_relaxed)))};
}

bool GrassGpuAllocationRefused(uint64_t bytes) noexcept {
    const uint64_t threshold = RefusalThreshold();
    if (threshold == UINT64_MAX) return false;
    const auto use = GrassGpuMemoryInUse();
    return use.DeviceLocal + use.HostVisible + bytes > threshold;
}

} // namespace Fast::Oot3d
