#ifdef ENABLE_OOT3D_VULKAN
#include "fast/backends/gfx_vulkan.h"
#include "fast/renderer3ds/pica_framebuffer_encode.h"
#include "fast/renderer3ds/pica_framebuffer_encode_gpu.h"
#include "fast/renderer/spirv_cache.h"
#include <algorithm>
#include <stdexcept>

namespace Fast {
GfxNativePicaTextureView GfxRenderingAPIVulkan::ResolveNativePicaCopiedTexture(
    const GfxNativePicaTextureView& texture, uint64_t renderTargetNamespace,
    uint64_t drawId, std::vector<uint8_t>& coherentBytes) {
    // A captured plan can precede completion of the raw GPU copy. Refresh only
    // unchanged, stale bytes after that copy, never a newer CPU-owned payload.
    auto resolved = texture;
    coherentBytes.clear();
    for (const auto& [identity, copy] : mRawTextureCopyWritebacks) {
        if (identity.first != renderTargetNamespace || drawId <= copy.AfterDrawSubmissionId) continue;
        for (const auto& write : copy.Writes) {
            const uint64_t lo = std::max<uint64_t>(texture.PhysicalAddress, write.Address);
            const uint64_t hi = std::min<uint64_t>(uint64_t{texture.PhysicalAddress} + texture.NativeBytes.size(),
                                                  uint64_t{write.Address} + write.Bytes.size());
            if (lo >= hi) continue;
            const size_t textureOffset = lo - texture.PhysicalAddress, writeOffset = lo - write.Address;
            const size_t count = hi - lo;
            if (!std::equal(texture.NativeBytes.begin() + textureOffset,
                            texture.NativeBytes.begin() + textureOffset + count,
                            write.Before.begin() + writeOffset)) continue;
            std::vector<uint8_t> live(count);
            if (!mPhysicalMemoryRead || !mPhysicalMemoryRead(static_cast<uint32_t>(lo), live) ||
                !std::equal(live.begin(), live.end(), write.Bytes.begin() + writeOffset)) continue;
            if (coherentBytes.empty()) coherentBytes.assign(texture.NativeBytes.begin(), texture.NativeBytes.end());
            std::copy(live.begin(), live.end(), coherentBytes.begin() + textureOffset);
        }
    }
    if (!coherentBytes.empty()) {
        resolved.NativeBytes = coherentBytes;
        resolved.NativeContentHashAvailable = false;
        resolved.NativeBaseLevelContentHashAvailable = false;
    }
    return resolved;
}

namespace {
void RequireCopyVk(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(operation);
}
}

void GfxRenderingAPIVulkan::DestroyRawCopyEncodeResources() {
    if (mDevice != VK_NULL_HANDLE) {
        if (mRawCopyEncodePipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(mDevice, mRawCopyEncodePipeline, nullptr);
        }
        if (mRawCopyEncodePipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(mDevice, mRawCopyEncodePipelineLayout, nullptr);
        }
        if (mRawCopyEncodeDescriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(mDevice, mRawCopyEncodeDescriptorPool, nullptr);
        }
        if (mRawCopyEncodeDescriptorLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(mDevice, mRawCopyEncodeDescriptorLayout, nullptr);
        }
        if (mRawCopyEncodeSampler != VK_NULL_HANDLE) {
            vkDestroySampler(mDevice, mRawCopyEncodeSampler, nullptr);
        }
        if (mRawCopyEncodeImage.View != VK_NULL_HANDLE) {
            vkDestroyImageView(mDevice, mRawCopyEncodeImage.View, nullptr);
        }
        if (mRawCopyEncodeImage.Image != VK_NULL_HANDLE) {
            vkDestroyImage(mDevice, mRawCopyEncodeImage.Image, nullptr);
        }
        if (mRawCopyEncodeImage.Memory != VK_NULL_HANDLE) {
            vkFreeMemory(mDevice, mRawCopyEncodeImage.Memory, nullptr);
        }
    }
    mRawCopyEncodePipeline = VK_NULL_HANDLE;
    mRawCopyEncodePipelineLayout = VK_NULL_HANDLE;
    mRawCopyEncodeDescriptorPool = VK_NULL_HANDLE;
    mRawCopyEncodeDescriptorLayout = VK_NULL_HANDLE;
    mRawCopyEncodeDescriptorSet = VK_NULL_HANDLE;
    mRawCopyEncodeSampler = VK_NULL_HANDLE;
    mRawCopyEncodeImage = {};
    DestroyBuffer(mRawCopyEncodedBuffer);
    mRawCopyEncodedBuffer = {};
}

bool GfxRenderingAPIVulkan::EnsureRawCopyEncodeResources(uint32_t encodedBytes,
                                                        std::string* error) {
    const auto fail = [&](const char* message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (encodedBytes == 0U || mDevice == VK_NULL_HANDLE) {
        return fail("raw PICA copy GPU encode has no output extent");
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(mPhysicalDevice, &properties);
    // The dispatch is recorded on the graphics queue, so its family must be
    // compute-capable; anything else is refused instead of silently failing at
    // submit time.
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &familyCount, families.data());
    if (mGraphicsQueueFamily >= families.size() ||
        (families[mGraphicsQueueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0U) {
        return fail("raw PICA copy GPU encode needs a compute-capable graphics queue");
    }
    // R32_UINT storage is the format the shader needs; when it is absent the
    // copy must fail visibly rather than fall back to something else.
    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(mPhysicalDevice, VK_FORMAT_R32_UINT, &formatProperties);
    if ((formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0U ||
        (formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) == 0U) {
        return fail("raw PICA copy GPU encode needs R32_UINT storage and transfer-source support");
    }
    const uint32_t wordCount = (encodedBytes + 3U) / 4U;
    const uint32_t wordsPerRow = std::min<uint32_t>(wordCount, 1024U);
    const uint32_t rows = (wordCount + wordsPerRow - 1U) / wordsPerRow;
    if (wordsPerRow == 0U || rows == 0U || rows > properties.limits.maxImageDimension2D ||
        wordsPerRow > properties.limits.maxImageDimension2D) {
        return fail("raw PICA copy GPU encode image extent is out of range");
    }
    const uint64_t groups = (uint64_t{wordCount} + 63U) / 64U;
    if (groups == 0U || groups > properties.limits.maxComputeWorkGroupCount[0]) {
        return fail("raw PICA copy GPU encode dispatch exceeds the workgroup limit");
    }
    if (mRawCopyEncodedBuffer.Size < static_cast<VkDeviceSize>(wordCount) * 4U) {
        DestroyBuffer(mRawCopyEncodedBuffer);
        mRawCopyEncodedBuffer = CreateReadbackBuffer(
            static_cast<VkDeviceSize>(wordCount) * 4U, &mRawCopyReadbackCoherent);
    }
    if (mRawCopyEncodeImage.Capacity < static_cast<VkDeviceSize>(wordCount) * 4U) {
        if (mRawCopyEncodeImage.View != VK_NULL_HANDLE) {
            vkDestroyImageView(mDevice, mRawCopyEncodeImage.View, nullptr);
            mRawCopyEncodeImage.View = VK_NULL_HANDLE;
        }
        if (mRawCopyEncodeImage.Image != VK_NULL_HANDLE) {
            vkDestroyImage(mDevice, mRawCopyEncodeImage.Image, nullptr);
            mRawCopyEncodeImage.Image = VK_NULL_HANDLE;
        }
        if (mRawCopyEncodeImage.Memory != VK_NULL_HANDLE) {
            vkFreeMemory(mDevice, mRawCopyEncodeImage.Memory, nullptr);
            mRawCopyEncodeImage.Memory = VK_NULL_HANDLE;
        }
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R32_UINT;
        imageInfo.extent = {wordsPerRow, rows, 1U};
        imageInfo.mipLevels = 1U;
        imageInfo.arrayLayers = 1U;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        RequireCopyVk(vkCreateImage(mDevice, &imageInfo, nullptr, &mRawCopyEncodeImage.Image),
                "vkCreateImage(raw copy encode)");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(mDevice, mRawCopyEncodeImage.Image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        RequireCopyVk(vkAllocateMemory(mDevice, &allocation, nullptr, &mRawCopyEncodeImage.Memory),
                "vkAllocateMemory(raw copy encode)");
        RequireCopyVk(vkBindImageMemory(mDevice, mRawCopyEncodeImage.Image, mRawCopyEncodeImage.Memory, 0U),
                "vkBindImageMemory(raw copy encode)");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = mRawCopyEncodeImage.Image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R32_UINT;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
        RequireCopyVk(vkCreateImageView(mDevice, &viewInfo, nullptr, &mRawCopyEncodeImage.View),
                "vkCreateImageView(raw copy encode)");
        mRawCopyEncodeImage.Capacity = static_cast<VkDeviceSize>(wordCount) * 4U;
        mRawCopyEncodeImage.Stride = wordsPerRow;
        mRawCopyEncodeImage.Height = rows;
    }
    if (mRawCopyEncodeSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        RequireCopyVk(vkCreateSampler(mDevice, &samplerInfo, nullptr, &mRawCopyEncodeSampler),
                "vkCreateSampler(raw copy encode)");
    }
    if (mRawCopyEncodePipeline == VK_NULL_HANDLE) {
        std::array<VkDescriptorSetLayoutBinding, 2U> bindings{};
        bindings[0] = {0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1U,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1U,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        RequireCopyVk(vkCreateDescriptorSetLayout(mDevice, &layoutInfo, nullptr,
                                            &mRawCopyEncodeDescriptorLayout),
                "vkCreateDescriptorSetLayout(raw copy encode)");
        const VkDescriptorPoolSize poolSizes[2]{
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1U},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1U}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1U;
        poolInfo.poolSizeCount = 2U;
        poolInfo.pPoolSizes = poolSizes;
        RequireCopyVk(vkCreateDescriptorPool(mDevice, &poolInfo, nullptr,
                                       &mRawCopyEncodeDescriptorPool),
                "vkCreateDescriptorPool(raw copy encode)");
        VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocateInfo.descriptorPool = mRawCopyEncodeDescriptorPool;
        allocateInfo.descriptorSetCount = 1U;
        allocateInfo.pSetLayouts = &mRawCopyEncodeDescriptorLayout;
        RequireCopyVk(vkAllocateDescriptorSets(mDevice, &allocateInfo, &mRawCopyEncodeDescriptorSet),
                "vkAllocateDescriptorSets(raw copy encode)");
        const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0U, 32U};
        VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayout.setLayoutCount = 1U;
        pipelineLayout.pSetLayouts = &mRawCopyEncodeDescriptorLayout;
        pipelineLayout.pushConstantRangeCount = 1U;
        pipelineLayout.pPushConstantRanges = &pushRange;
        RequireCopyVk(vkCreatePipelineLayout(mDevice, &pipelineLayout, nullptr,
                                       &mRawCopyEncodePipelineLayout),
                "vkCreatePipelineLayout(raw copy encode)");
        const std::string source(Renderer3ds::PicaFramebufferEncodeComputeSource);
        const auto spirv = mCompiledShaderCache.Resolve(
            source, Renderer::SpirvStage::Compute, [&] {
                return Renderer::CompileShadercSpirv(source, Renderer::SpirvStage::Compute,
                                                     "pica_framebuffer_encode.comp");
            });
        const VkShaderModule module = CreateShaderModuleFromSpirv(spirv);
        if (module == VK_NULL_HANDLE) {
            return fail("raw PICA copy GPU encode shader module could not be created");
        }
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = mRawCopyEncodePipelineLayout;
        const VkResult created =
            vkCreateComputePipelines(mDevice, VK_NULL_HANDLE, 1U, &pipelineInfo, nullptr,
                                     &mRawCopyEncodePipeline);
        vkDestroyShaderModule(mDevice, module, nullptr);
        RequireCopyVk(created, "vkCreateComputePipelines(raw copy encode)");
    }
    return true;
}

bool GfxRenderingAPIVulkan::RecordRawCopyEncode(
    VkCommandBuffer command, VkImage sourceImage, VkImageView sourceView,
    uint32_t imageWidth, uint32_t imageHeight, uint32_t width, uint32_t height,
    uint8_t format, uint32_t encodedBytes) {
    const uint32_t bpp = format == 0U ? 4U : format == 1U ? 3U : 2U;
    // The CPU oracle refuses these; the shader cannot, so the host does, and the
    // half-texel numerator is bounded so it cannot wrap in 32-bit arithmetic.
    if (width == 0U || height == 0U || width % 8U != 0U || height % 8U != 0U ||
        format > 4U || imageWidth == 0U || imageHeight == 0U ||
        uint64_t{width} * height * bpp != encodedBytes ||
        (uint64_t{width} * 2U + 1U) * imageWidth > 0xFFFFFFFFull) {
        throw std::runtime_error("raw PICA copy GPU encode refuses this geometry");
    }
    const uint32_t wordCount = (encodedBytes + 3U) / 4U;
    VkDescriptorImageInfo sourceInfo{};
    sourceInfo.sampler = mRawCopyEncodeSampler;
    sourceInfo.imageView = sourceView;
    sourceInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo outputInfo{};
    outputInfo.imageView = mRawCopyEncodeImage.View;
    outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    std::array<VkWriteDescriptorSet, 2U> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = mRawCopyEncodeDescriptorSet;
    writes[0].dstBinding = 0U;
    writes[0].descriptorCount = 1U;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &sourceInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = mRawCopyEncodeDescriptorSet;
    writes[1].dstBinding = 1U;
    writes[1].descriptorCount = 1U;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &outputInfo;
    vkUpdateDescriptorSets(mDevice, static_cast<uint32_t>(writes.size()), writes.data(), 0U, nullptr);

    const auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to,
                             VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                             VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
        VkImageMemoryBarrier imageBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        imageBarrier.oldLayout = from;
        imageBarrier.newLayout = to;
        imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imageBarrier.image = image;
        imageBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
        imageBarrier.srcAccessMask = srcAccess;
        imageBarrier.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(command, srcStage, dstStage, 0U, 0U, nullptr, 0U, nullptr, 1U,
                             &imageBarrier);
    };
    barrier(sourceImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    // Every consumed byte is rewritten by the dispatch, so discarding the
    // previous contents is legal here.
    barrier(mRawCopyEncodeImage.Image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U,
            VK_ACCESS_SHADER_WRITE_BIT);

    const uint32_t push[8] = {imageWidth, imageHeight, width, height,
                              static_cast<uint32_t>(format), bpp,
                              mRawCopyEncodeImage.Stride, encodedBytes};
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, mRawCopyEncodePipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, mRawCopyEncodePipelineLayout,
                            0U, 1U, &mRawCopyEncodeDescriptorSet, 0U, nullptr);
    vkCmdPushConstants(command, mRawCopyEncodePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                       sizeof(push), push);
    vkCmdDispatch(command, static_cast<uint32_t>((wordCount + 63U) / 64U), 1U, 1U);

    barrier(mRawCopyEncodeImage.Image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 0U, 1U};
    region.imageExtent = {mRawCopyEncodeImage.Stride, mRawCopyEncodeImage.Height, 1U};
    vkCmdCopyImageToBuffer(command, mRawCopyEncodeImage.Image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mRawCopyEncodedBuffer.Buffer, 1U,
                           &region);
    barrier(sourceImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    return true;
}

bool GfxRenderingAPIVulkan::SubmitNativePicaTextureCopy(
    const GfxNativePicaDisplayTransferView& transfer, std::string* error) {
    try {
        const auto previous = mRawTextureCopyWritebacks.find({transfer.RenderTargetNamespace, transfer.OutputPhysicalAddress});
        if (previous != mRawTextureCopyWritebacks.end() && previous->second.CompletionId == transfer.CompletionId)
            return true;
        if (!mFrameActive || transfer.Present || !mPhysicalMemoryRead || !mPhysicalMemoryCommit ||
            mFrameExternalComputeSplit || mNativePicaPresentedThisFrame)
            throw std::runtime_error("raw PICA copy requires ordered offscreen work and coherent memory access");
        const uint32_t size = transfer.TextureCopyBytes & ~15U;
        const uint32_t inputGap = uint32_t{transfer.InputHeight} * 16U;
        const uint32_t outputGap = uint32_t{transfer.OutputHeight} * 16U;
        const uint32_t inputWidth = inputGap == 0 ? size : uint32_t{transfer.InputWidth} * 16U;
        const uint32_t outputWidth = outputGap == 0 ? size : uint32_t{transfer.OutputWidth} * 16U;
        if (!size || !inputWidth || !outputWidth)
            throw std::runtime_error("raw PICA copy has zero size or strided width");
        const uint64_t inputEnd = uint64_t{transfer.InputPhysicalAddress} + size + uint64_t{(size - 1) / inputWidth} * inputGap;
        const uint64_t outputEnd = uint64_t{transfer.OutputPhysicalAddress} + size + uint64_t{(size - 1) / outputWidth} * outputGap;
        if (inputEnd > (uint64_t{1} << 32) || outputEnd > (uint64_t{1} << 32))
            throw std::runtime_error("raw PICA copy wraps physical addresses");
        NativePicaRenderTarget* source = nullptr;
        for (auto& [key, target] : mNativePicaRenderTargets) {
            const uint32_t bpp = key.ColorFormat == 0 ? 4 : key.ColorFormat == 1 ? 3 : 2;
            const uint64_t end = uint64_t{key.ColorPhysicalAddress} + uint64_t{key.Width} * key.Height * bpp;
            if (key.RenderTargetNamespace == transfer.RenderTargetNamespace &&
                transfer.InputPhysicalAddress >= key.ColorPhysicalAddress && inputEnd <= end) {
                if (source != nullptr) throw std::runtime_error("raw PICA copy source ownership is ambiguous");
                source = &target;
            }
        }
        if (!source) throw std::runtime_error("raw PICA copy has no coherent GPU source");
        // Writing an existing attachment needs an upload as well as guest-memory
        // writeback. Never claim completion for that not-yet-supported case.
        for (const auto& [key, target] : mNativePicaRenderTargets) {
            const uint32_t bpp = key.ColorFormat == 0 ? 4 : key.ColorFormat == 1 ? 3 : 2;
            const uint64_t end = uint64_t{key.ColorPhysicalAddress} + uint64_t{key.Width} * key.Height * bpp;
            if (key.RenderTargetNamespace == transfer.RenderTargetNamespace &&
                key.ColorPhysicalAddress < outputEnd && end > transfer.OutputPhysicalAddress)
                throw std::runtime_error("raw PICA copy destination aliases an active GPU attachment");
        }
        EndNativePicaRenderPass();
        if (mRenderPassActive) {
            vkCmdEndRenderPass(mCommandBuffers[mCurrentFrame]);
            mRenderPassActive = false;
            mOverlayRenderPassActive = false;
        }
        // A rare raw transfer is a GPU/CPU coherence boundary. Submit the exact
        // recorded prefix, not the whole frame or an unrelated presentation.
        // The wait is on this submission's own fence: queue submissions of one
        // queue complete in submission order, so this still guarantees every
        // earlier submission retired, while later submissions and other queues
        // keep running instead of idling the whole queue.
        WaitForAllPresents();
        const VkCommandBuffer command = mCommandBuffers[mCurrentFrame];
        // The native tiled bytes are packed on the GPU inside the submission the
        // copy already waits on, then copied to host staging for the guest
        // writeback: no CPU encode, and the staging shrinks from the whole colour
        // target to the encoded bytes themselves.
        const uint32_t copyBpp = source->Key.ColorFormat == 0U   ? 4U
                                 : source->Key.ColorFormat == 1U ? 3U
                                                                 : 2U;
        const uint32_t encodedBytes =
            uint32_t{source->Key.Width} * source->Key.Height * copyBpp;
        std::string encodeError;
        if (!EnsureRawCopyEncodeResources(encodedBytes, &encodeError)) {
            throw std::runtime_error("raw PICA copy GPU encode is unavailable: " +
                                     encodeError);
        }
        if (!RecordRawCopyEncode(command, source->ColorImage, source->ColorView,
                                 source->Width, source->Height, source->Key.Width,
                                 source->Key.Height, source->Key.ColorFormat,
                                 encodedBytes)) {
            throw std::runtime_error("raw PICA copy GPU encode was not recorded");
        }
        RequireCopyVk(vkEndCommandBuffer(command), "end raw-copy prefix");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        RequireCopyVk(vkResetFences(mDevice, 1, &mRawCopyFence), "reset raw-copy fence");
        RequireCopyVk(vkQueueSubmit(mGraphicsQueue, 1, &submit, mRawCopyFence), "submit raw-copy prefix");
        RequireCopyVk(vkWaitForFences(mDevice, 1, &mRawCopyFence, VK_TRUE, UINT64_MAX),
                      "wait raw-copy fence");
        const auto copyPhaseClock = [] { return std::chrono::steady_clock::now(); };
        const auto phaseMilliseconds = [](const auto& from, const auto& to) {
            return std::chrono::duration<double, std::milli>(to - from).count();
        };
        const auto readbackStart = copyPhaseClock();
        if (!mRawCopyReadbackCoherent) {
            VkMappedMemoryRange readbackRange{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            readbackRange.memory = mRawCopyEncodedBuffer.Memory;
            readbackRange.offset = 0;
            readbackRange.size = VK_WHOLE_SIZE;
            RequireCopyVk(vkInvalidateMappedMemoryRanges(mDevice, 1, &readbackRange),
                          "invalidate raw-copy encode staging");
        }
        const auto* encodedPointer =
            static_cast<const uint8_t*>(mRawCopyEncodedBuffer.Mapped);
        if (encodedPointer == nullptr) {
            throw std::runtime_error("raw PICA copy encode staging is not mapped");
        }
        std::vector<uint8_t> native(encodedPointer, encodedPointer + encodedBytes);
        const auto encodeStart = copyPhaseClock();
        // Diagnostic differential: keep the CPU oracle in the loop and compare
        // it with what the GPU produced, on the real colour targets.
        static const bool encodeVerify =
            std::getenv("TRIAEVUM_RAW_COPY_ENCODE_VERIFY") != nullptr;
        mRawCopyEncodeVerify = encodeVerify;
        if (encodeVerify) {
            const auto rgba = CaptureNativePicaImageBytes(
                source->ColorImage, source->Width, source->Height, 4,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            const auto oracle = Renderer3ds::EncodePicaFramebuffer(
                rgba, source->Width, source->Height, source->Key.Width,
                source->Key.Height, source->Key.ColorFormat);
            uint64_t mismatches = 0;
            uint64_t firstOffset = 0;
            for (size_t index = 0; index < oracle.size() && index < native.size(); ++index) {
                if (oracle[index] != native[index]) {
                    if (mismatches == 0) firstOffset = index;
                    ++mismatches;
                }
            }
            if (mismatches != 0 && mRawCopyEncodeVerifyMismatches == 0) {
                std::fprintf(stderr,
                             "TRIAEVUM_RAW_COPY_ENCODE_MISMATCH offset=%llu oracle=0x%02X gpu=0x%02X\n",
                             static_cast<unsigned long long>(firstOffset),
                             oracle[firstOffset], native[firstOffset]);
            }
            mRawCopyEncodeVerifyBytes += oracle.size();
            mRawCopyEncodeVerifyMismatches += mismatches;
        }
        const auto invalidateStart = copyPhaseClock();
        // The prefix fence has retired every prior use. A framebuffer copy
        // changes each frame; retaining all its content-hash texture versions
        // would grow both GPU memory and savestates without bound.
        for (auto it = mNativePicaTextures.begin(); it != mNativePicaTextures.end();) {
            const auto& key = it->first;
            const uint64_t address = key.PhysicalAddress;
            // Bound native storage conservatively by RGBA8 across all mips.
            // Over-invalidation costs an upload; under-invalidation retains
            // every animated version when a copy writes inside a texture.
            uint64_t extent = 0;
            for (uint32_t width = key.Width, height = key.Height;;
                 width = std::max(1U, width / 2), height = std::max(1U, height / 2)) {
                extent += uint64_t{std::max(8U, width)} * std::max(8U, height) * 4U;
                if (width <= 1 && height <= 1) break;
            }
            if (address < outputEnd && address + extent > transfer.OutputPhysicalAddress &&
                it->second.ImageLayout != VK_IMAGE_LAYOUT_GENERAL) {
                DestroyTexture(it->second);
                ++mRawCopyTiming.InvalidatedTextures;
                it = mNativePicaTextures.erase(it);
            } else ++it;
        }
        RequireCopyVk(vkResetCommandBuffer(command, 0), "reset raw-copy continuation");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        RequireCopyVk(vkBeginCommandBuffer(command, &begin), "begin raw-copy continuation");
        if (mNriInterop.Available() && !mNriInterop.WrapFrameCommandBuffer(mCurrentFrame, command))
            throw std::runtime_error("cannot wrap raw-copy continuation in NRI");

        const auto spanStart = copyPhaseClock();
        RawTextureCopyWriteback writeback;
        writeback.CompletionId = transfer.CompletionId;
        writeback.AfterDrawSubmissionId = transfer.AfterDrawSubmissionId;
        uint64_t input = transfer.InputPhysicalAddress, output = transfer.OutputPhysicalAddress;
        uint32_t left = size, inputLeft = inputWidth, outputLeft = outputWidth;
        while (left) {
            const uint32_t count = std::min({left, inputLeft, outputLeft});
            Renderer3ds::PicaPhysicalMemoryWrite write;
            write.Address = static_cast<uint32_t>(output);
            write.Before.resize(count);
            if (!mPhysicalMemoryRead(write.Address, write.Before))
                throw std::runtime_error("raw PICA copy destination is unmapped");
            const size_t offset = static_cast<size_t>(input - source->Key.ColorPhysicalAddress);
            write.Bytes.assign(native.begin() + offset, native.begin() + offset + count);
            writeback.Writes.push_back(std::move(write));
            ++mRawCopyTiming.Spans;
            left -= count; inputLeft -= count; outputLeft -= count;
            input += count; output += count;
            if (!inputLeft) { input += inputGap; inputLeft = inputWidth; }
            if (!outputLeft) { output += outputGap; outputLeft = outputWidth; }
        }
        const auto commitStart = copyPhaseClock();
        if (!mPhysicalMemoryCommit(transfer.CompletionId, writeback.Writes))
            throw std::runtime_error("raw PICA copy writeback/completion failed");
        const auto copyEnd = copyPhaseClock();
        ++mRawCopyTiming.Copies;
        mRawCopyTiming.ReadbackBytes += native.size();
        mRawCopyTiming.WritebackBytes += native.size();
        mRawCopyTiming.ReadbackSeconds += phaseMilliseconds(readbackStart, encodeStart);
        mRawCopyTiming.EncodeSeconds += phaseMilliseconds(encodeStart, invalidateStart);
        mRawCopyTiming.InvalidateSeconds += phaseMilliseconds(invalidateStart, spanStart);
        mRawCopyTiming.SpanSeconds += phaseMilliseconds(spanStart, commitStart);
        mRawCopyTiming.CommitSeconds += phaseMilliseconds(commitStart, copyEnd);
        mRawTextureCopyWritebacks.insert_or_assign(
            std::pair{transfer.RenderTargetNamespace, transfer.OutputPhysicalAddress}, std::move(writeback));
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}
} // namespace Fast
#endif
