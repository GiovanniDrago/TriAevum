#pragma once

#include <cstdint>
#include <optional>

#include <vulkan/vulkan.h>

namespace Fast::Renderer3ds {

// This process's use of one device-local heap and the budget the driver grants
// it (VK_EXT_memory_budget), in bytes.
struct VulkanDeviceLocalMemory {
    uint32_t Heap = 0;
    uint64_t Usage = 0;
    uint64_t Budget = 0;
    uint64_t Size = 0;
};

[[nodiscard]] bool VulkanMemoryBudgetSupported(VkPhysicalDevice physicalDevice) noexcept;

// The heap's usage and budget; nullopt without the extension, for a heap that
// is not device-local, or when the driver reports no budget.
[[nodiscard]] std::optional<VulkanDeviceLocalMemory> QueryVulkanHeapMemory(VkPhysicalDevice physicalDevice,
                                                                         uint32_t heap) noexcept;

// The largest device-local heap: the card's VRAM on a discrete GPU.
[[nodiscard]] std::optional<VulkanDeviceLocalMemory> QueryVulkanDeviceLocalMemory(
    VkPhysicalDevice physicalDevice) noexcept;

} // namespace Fast::Renderer3ds
