#pragma once

#include <cstdint>

namespace Fast::Oot3d {

// Live GPU memory of the grass passes, counted where their buffers are
// allocated and freed: device-local (VRAM) and host-visible.
struct GrassGpuMemoryUse {
    uint64_t DeviceLocal = 0;
    uint64_t HostVisible = 0;
};

void TrackGrassGpuMemory(bool deviceLocal, int64_t bytes) noexcept;
[[nodiscard]] GrassGpuMemoryUse GrassGpuMemoryInUse() noexcept;

// Diagnostic fault injection: TRIAEVUM_GRASS_FAIL_ALLOCATIONS_ABOVE_MIB=<n>
// refuses an allocation that would take the grass passes beyond n MiB, as a
// full card would. Unset: never refuses.
[[nodiscard]] bool GrassGpuAllocationRefused(uint64_t bytes) noexcept;

} // namespace Fast::Oot3d
