#include "fast/oot3d/grass_gpu_memory.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

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

void TrackGrassGpuMemory(bool deviceLocal, int64_t bytes, const char* owner, uint32_t usage) noexcept {
    (deviceLocal ? gDeviceLocal : gHostVisible).fetch_add(bytes, std::memory_order_relaxed);
    static const bool log = std::getenv("TRIAEVUM_GRASS_GPU_MEMORY_LOG") != nullptr;
    if (log && bytes > 0)
        std::fprintf(stderr, "TRIAEVUM_GRASS_GPU_ALLOC owner=%s device_local=%d usage=%08x bytes=%lld\n",
                     owner != nullptr ? owner : "unknown", deviceLocal ? 1 : 0, usage,
                     static_cast<long long>(bytes));
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

#ifdef ENABLE_OOT3D_VULKAN
namespace {

bool SupportsMemoryBudget(VkPhysicalDevice physicalDevice) {
    static std::mutex mutex;
    static VkPhysicalDevice cached = VK_NULL_HANDLE;
    static bool supported = false;
    std::scoped_lock lock(mutex);
    if (cached != physicalDevice) {
        uint32_t count = 0;
        std::vector<VkExtensionProperties> extensions;
        if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr) == VK_SUCCESS) {
            extensions.resize(count);
            if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, extensions.data()) != VK_SUCCESS)
                count = 0;
        }
        supported = std::any_of(extensions.begin(), extensions.begin() + count, [](const auto& extension) {
            return std::strcmp(extension.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0;
        });
        cached = physicalDevice;
    }
    return supported;
}

std::optional<uint64_t> ReserveOverride() {
    static const std::optional<uint64_t> reserve = []() -> std::optional<uint64_t> {
        const char* value = std::getenv("TRIAEVUM_GRASS_VRAM_RESERVE_MIB");
        if (value == nullptr || *value == '\0') return std::nullopt;
        return std::strtoull(value, nullptr, 10) << 20U;
    }();
    return reserve;
}

} // namespace

bool GrassGpuAllocationRefused(VkPhysicalDevice physicalDevice, uint32_t memoryType, uint64_t bytes) noexcept {
    if (GrassGpuAllocationRefused(bytes)) return true;
    try {
        if (physicalDevice == VK_NULL_HANDLE || !SupportsMemoryBudget(physicalDevice)) return false;
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget};
        vkGetPhysicalDeviceMemoryProperties2(physicalDevice, &properties);
        const auto& memory = properties.memoryProperties;
        if (memoryType >= memory.memoryTypeCount) return false;
        const uint32_t heap = memory.memoryTypes[memoryType].heapIndex;
        const uint64_t heapBudget = budget.heapBudget[heap];
        if ((memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0U || heapBudget == 0U)
            return false;
        const uint64_t reserve = ReserveOverride().value_or(heapBudget / 16U);
        const uint64_t usage = budget.heapUsage[heap];
        static const bool log = std::getenv("TRIAEVUM_GRASS_GPU_MEMORY_LOG") != nullptr;
        if (log)
            std::fprintf(stderr, "TRIAEVUM_GRASS_VRAM_BUDGET heap=%u usage_mib=%llu budget_mib=%llu\n", heap,
                         static_cast<unsigned long long>(usage >> 20U),
                         static_cast<unsigned long long>(heapBudget >> 20U));
        if (usage + bytes + reserve <= heapBudget) return false;
        std::fprintf(stderr, "[grass] VRAM budget: heap %u uses %llu MiB of %llu MiB (reserve %llu MiB); "
                     "refusing a %llu MiB grass buffer\n", heap, static_cast<unsigned long long>(usage >> 20U),
                     static_cast<unsigned long long>(heapBudget >> 20U),
                     static_cast<unsigned long long>(reserve >> 20U),
                     static_cast<unsigned long long>((bytes + (1U << 20U) - 1U) >> 20U));
        return true;
    } catch (...) {
        return false;
    }
}
#endif

} // namespace Fast::Oot3d
