#include "fast/renderer3ds/vulkan_memory_budget.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace Fast::Renderer3ds {

bool VulkanMemoryBudgetSupported(VkPhysicalDevice physicalDevice) noexcept {
    static std::mutex mutex;
    static VkPhysicalDevice cached = VK_NULL_HANDLE;
    static bool supported = false;
    try {
        std::scoped_lock lock(mutex);
        if (cached != physicalDevice) {
            uint32_t count = 0;
            std::vector<VkExtensionProperties> extensions;
            if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr) == VK_SUCCESS) {
                extensions.resize(count);
                if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, extensions.data()) !=
                    VK_SUCCESS)
                    count = 0;
            }
            supported = std::any_of(extensions.begin(), extensions.begin() + count, [](const auto& extension) {
                return std::strcmp(extension.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0;
            });
            cached = physicalDevice;
        }
        return supported;
    } catch (...) {
        return false;
    }
}

namespace {

struct BudgetProperties {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT Budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 Properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
};

bool ReadBudget(VkPhysicalDevice physicalDevice, BudgetProperties& properties) noexcept {
    if (physicalDevice == VK_NULL_HANDLE || !VulkanMemoryBudgetSupported(physicalDevice)) return false;
    properties.Properties.pNext = &properties.Budget;
    vkGetPhysicalDeviceMemoryProperties2(physicalDevice, &properties.Properties);
    return true;
}

std::optional<VulkanDeviceLocalMemory> HeapMemory(const BudgetProperties& properties, uint32_t heap) {
    const auto& memory = properties.Properties.memoryProperties;
    if (heap >= memory.memoryHeapCount || (memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0U ||
        properties.Budget.heapBudget[heap] == 0U)
        return std::nullopt;
    return VulkanDeviceLocalMemory{heap, properties.Budget.heapUsage[heap], properties.Budget.heapBudget[heap],
                                   memory.memoryHeaps[heap].size};
}

} // namespace

std::optional<VulkanDeviceLocalMemory> QueryVulkanHeapMemory(VkPhysicalDevice physicalDevice, uint32_t heap) noexcept {
    BudgetProperties properties;
    if (!ReadBudget(physicalDevice, properties)) return std::nullopt;
    return HeapMemory(properties, heap);
}

std::optional<VulkanDeviceLocalMemory> QueryVulkanDeviceLocalMemory(VkPhysicalDevice physicalDevice) noexcept {
    BudgetProperties properties;
    if (!ReadBudget(physicalDevice, properties)) return std::nullopt;
    std::optional<VulkanDeviceLocalMemory> largest;
    for (uint32_t heap = 0; heap < properties.Properties.memoryProperties.memoryHeapCount; ++heap) {
        const auto memory = HeapMemory(properties, heap);
        if (memory && (!largest || memory->Size > largest->Size)) largest = memory;
    }
    return largest;
}

} // namespace Fast::Renderer3ds
