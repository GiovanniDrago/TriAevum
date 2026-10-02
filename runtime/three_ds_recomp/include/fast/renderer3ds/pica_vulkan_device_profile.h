#pragma once

#include <vulkan/vulkan.h>

#include <array>

namespace Fast::Renderer3ds {

// Preparation and live rendering must use the same Vulkan application contract.
// Drivers may expose different pipeline-cache UUIDs for different API versions.
inline VkApplicationInfo PicaVulkanApplicationInfo() {
    VkApplicationInfo result{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    result.pApplicationName = "TriAevum";
    result.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    result.pEngineName = "ThreeDsRecomp Runtime";
    result.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    result.apiVersion = VK_API_VERSION_1_2;
    return result;
}

inline VkPhysicalDeviceFeatures PicaVulkanCoreFeatures(const VkPhysicalDeviceFeatures& supported) {
    VkPhysicalDeviceFeatures enabled{};
    enabled.independentBlend = supported.independentBlend;
    enabled.multiViewport = supported.multiViewport;
    // Fragment primitive IDs address opt-in per-primitive seam metadata.
    enabled.geometryShader = supported.geometryShader;
    enabled.shaderImageGatherExtended = supported.shaderImageGatherExtended;
    enabled.shaderStorageImageWriteWithoutFormat = supported.shaderStorageImageWriteWithoutFormat;
    enabled.shaderStorageImageExtendedFormats = supported.shaderStorageImageExtendedFormats;
    // Native fragment-operation programs write the storage-image target.
    enabled.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
    enabled.shaderInt16 = supported.shaderInt16;
    return enabled;
}

inline VkPhysicalDeviceVulkan12Features PicaVulkan12Features(const VkPhysicalDeviceVulkan12Features& supported) {
    VkPhysicalDeviceVulkan12Features enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    enabled.bufferDeviceAddress = supported.bufferDeviceAddress;
    enabled.timelineSemaphore = supported.timelineSemaphore;
    enabled.shaderFloat16 = supported.shaderFloat16;
    enabled.descriptorBindingSampledImageUpdateAfterBind = supported.descriptorBindingSampledImageUpdateAfterBind;
    enabled.descriptorBindingStorageImageUpdateAfterBind = supported.descriptorBindingStorageImageUpdateAfterBind;
    // FidelityFX Brixelizer binds its geometry arenas and bricks through
    // partially bound, non-uniformly indexed descriptor arrays.
    enabled.descriptorBindingPartiallyBound = supported.descriptorBindingPartiallyBound;
    enabled.runtimeDescriptorArray = supported.runtimeDescriptorArray;
    enabled.shaderStorageBufferArrayNonUniformIndexing = supported.shaderStorageBufferArrayNonUniformIndexing;
    enabled.shaderSampledImageArrayNonUniformIndexing = supported.shaderSampledImageArrayNonUniformIndexing;
    enabled.shaderStorageImageArrayNonUniformIndexing = supported.shaderStorageImageArrayNonUniformIndexing;
    return enabled;
}

// Graphics pipeline libraries: always-available programs link prepared parts,
// and the extended dynamic state keeps raster and depth state out of them.
// The live device and Forge's device preparation negotiate the same set (it
// changes the pipelines NRI creates, hence their cache identity).
inline constexpr std::array<const char*, 3> kPicaPipelineLibraryExtensions{
    VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME,
    VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
};

inline bool PicaPipelineLibrariesSupported(
    bool extensionsAvailable,
    const VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT& libraries,
    const VkPhysicalDeviceExtendedDynamicStateFeaturesEXT& dynamicState,
    const VkPhysicalDeviceFeatures& core) {
    return extensionsAvailable && libraries.graphicsPipelineLibrary == VK_TRUE &&
           dynamicState.extendedDynamicState == VK_TRUE && core.fragmentStoresAndAtomics == VK_TRUE;
}

} // namespace Fast::Renderer3ds
