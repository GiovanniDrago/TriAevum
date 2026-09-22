#ifdef ENABLE_OOT3D_VULKAN
#include "fast/backends/gfx_vulkan.h"
#include "fast/renderer3ds/pica_framebuffer_encode.h"
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
        // Read the source back inside the very submission the copy already
        // waits on: the fence below orders it, so the readback needs no second
        // submit, no queue idle and no per-copy buffer. Bytes handed to the
        // encoder are identical to the previous immediate-command readback.
        const auto readbackBytes =
            static_cast<VkDeviceSize>(source->Width) * source->Height * 4U;
        if (mRawCopyReadbackBuffer.Size < readbackBytes) {
            DestroyBuffer(mRawCopyReadbackBuffer);
            mRawCopyReadbackBuffer =
                CreateReadbackBuffer(readbackBytes, &mRawCopyReadbackCoherent);
        }
        VkImageMemoryBarrier readbackToTransfer{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        readbackToTransfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        readbackToTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        readbackToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        readbackToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        readbackToTransfer.image = source->ColorImage;
        readbackToTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        readbackToTransfer.subresourceRange.levelCount = 1U;
        readbackToTransfer.subresourceRange.layerCount = 1U;
        readbackToTransfer.srcAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        readbackToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(
            command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &readbackToTransfer);
        VkBufferImageCopy readbackCopy{};
        readbackCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        readbackCopy.imageSubresource.layerCount = 1U;
        readbackCopy.imageExtent = {source->Width, source->Height, 1U};
        vkCmdCopyImageToBuffer(
            command, source->ColorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            mRawCopyReadbackBuffer.Buffer, 1U, &readbackCopy);
        VkImageMemoryBarrier readbackToStable = readbackToTransfer;
        readbackToStable.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        readbackToStable.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        readbackToStable.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        readbackToStable.dstAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(
            command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
            nullptr, 1, &readbackToStable);
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
            readbackRange.memory = mRawCopyReadbackBuffer.Memory;
            readbackRange.offset = 0;
            readbackRange.size = VK_WHOLE_SIZE;
            RequireCopyVk(vkInvalidateMappedMemoryRanges(mDevice, 1, &readbackRange),
                          "invalidate raw-copy readback");
        }
        const auto* readbackBytesPointer =
            static_cast<const uint8_t*>(mRawCopyReadbackBuffer.Mapped);
        if (readbackBytesPointer == nullptr) {
            throw std::runtime_error("raw PICA copy readback staging is not mapped");
        }
        const auto rgba = std::vector<uint8_t>(
            readbackBytesPointer,
            readbackBytesPointer + static_cast<size_t>(readbackBytes));
        const auto encodeStart = copyPhaseClock();
        const auto native = Renderer3ds::EncodePicaFramebuffer(rgba, source->Width, source->Height,
            source->Key.Width, source->Key.Height, source->Key.ColorFormat);
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
        mRawCopyTiming.ReadbackBytes += rgba.size();
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
