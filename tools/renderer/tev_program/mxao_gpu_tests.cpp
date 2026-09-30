#include "fast/immerse/mxao_shader.h"
#include "vulkan_test_device.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

using Fast::Immerse::MxaoConstants;

namespace {
struct Image {
    VkImage Handle{};
    VkImageView View{};
    VkDeviceMemory Memory{};
};

enum : uint32_t { kDepth, kNormals, kTiled, kRaw, kFiltered, kOutput, kImageCount };
enum : uint32_t { kDeinterleave, kOcclusion, kFilter, kResolve, kProgramCount };

// Production MXAO kernels on native RGB8 normals. Odd extents exercise the
// padded tile grid and partial workgroups of every program.
struct MxaoFixture {
    static constexpr uint32_t Width = 129, Height = 97;
    Gpu Device{ VkPhysicalDeviceFeatures{ .shaderStorageImageExtendedFormats = VK_TRUE } };
    std::array<Image, kImageCount> Images{};
    Gpu::Buffer Depths{}, Normals{}, Result{};
    VkSampler Sampler{};
    VkDescriptorSetLayout SetLayout{};
    VkDescriptorPool Pool{};
    // Sets pair an input (binding 2) with an output (binding 3), as in MxaoPass.
    static constexpr std::array<std::array<uint32_t, 2>, 5> kPairs{ { { kDepth, kTiled },
                                                                      { kTiled, kRaw },
                                                                      { kRaw, kFiltered },
                                                                      { kRaw, kOutput },
                                                                      { kFiltered, kOutput } } };
    std::array<VkDescriptorSet, 5> Sets{};
    VkPipelineLayout Layout{};
    std::array<VkPipeline, kProgramCount> Pipelines{};
    VkCommandPool Commands{};
    VkCommandBuffer Command{};

    Image MakeImage(VkFormat format, uint32_t width, uint32_t height) {
        Image image;
        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = { width, height, 1 };
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        Check(vkCreateImage(Device.Device, &info, nullptr, &image.Handle));
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(Device.Device, image.Handle, &requirements);
        VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = Device.Type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(Device.Device, &allocation, nullptr, &image.Memory));
        Check(vkBindImageMemory(Device.Device, image.Handle, image.Memory, 0));
        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = image.Handle;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        Check(vkCreateImageView(Device.Device, &view, nullptr, &image.View));
        return image;
    }

    MxaoFixture() {
        const uint32_t tiledWidth = Fast::Immerse::MxaoTiledExtent(Width);
        const uint32_t tiledHeight = Fast::Immerse::MxaoTiledExtent(Height);
        Images = { MakeImage(VK_FORMAT_R32_SFLOAT, Width, Height), MakeImage(VK_FORMAT_R8G8B8A8_UNORM, Width, Height),
                   MakeImage(VK_FORMAT_R32_SFLOAT, tiledWidth, tiledHeight),
                   MakeImage(VK_FORMAT_R16G16_SFLOAT, Width, Height), MakeImage(VK_FORMAT_R16G16_SFLOAT, Width, Height),
                   MakeImage(VK_FORMAT_R32_SFLOAT, Width, Height) };
        Depths = Device.MakeBuffer(Width * Height * sizeof(float), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Normals = Device.MakeBuffer(Width * Height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Result = Device.MakeBuffer(Width * Height * sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        Check(vkCreateSampler(Device.Device, &sampler, nullptr, &Sampler));
        std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
        for (uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[i].descriptorType = i == 4   ? VK_DESCRIPTOR_TYPE_SAMPLER
                                         : i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                  : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }
        VkDescriptorSetLayoutCreateInfo setLayout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        setLayout.bindingCount = static_cast<uint32_t>(bindings.size());
        setLayout.pBindings = bindings.data();
        Check(vkCreateDescriptorSetLayout(Device.Device, &setLayout, nullptr, &SetLayout));
        const std::array<VkDescriptorPoolSize, 3> sizes{ { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 15 },
                                                           { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 },
                                                           { VK_DESCRIPTOR_TYPE_SAMPLER, 5 } } };
        VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool.maxSets = static_cast<uint32_t>(Sets.size());
        pool.poolSizeCount = static_cast<uint32_t>(sizes.size());
        pool.pPoolSizes = sizes.data();
        Check(vkCreateDescriptorPool(Device.Device, &pool, nullptr, &Pool));
        std::array<VkDescriptorSetLayout, 5> layouts{};
        layouts.fill(SetLayout);
        VkDescriptorSetAllocateInfo allocation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocation.descriptorPool = Pool;
        allocation.descriptorSetCount = static_cast<uint32_t>(Sets.size());
        allocation.pSetLayouts = layouts.data();
        Check(vkAllocateDescriptorSets(Device.Device, &allocation, Sets.data()));
        for (uint32_t set = 0; set < Sets.size(); ++set) {
            std::array<VkDescriptorImageInfo, 5> images{};
            std::array<VkWriteDescriptorSet, 5> writes{};
            for (uint32_t binding = 0; binding < 5; ++binding) {
                const uint32_t index = binding == 2 ? kPairs[set][0] : binding == 3 ? kPairs[set][1] : binding;
                images[binding].imageView = binding == 4 ? VK_NULL_HANDLE : Images[index].View;
                images[binding].imageLayout =
                    binding == 3 ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                images[binding].sampler = binding == 4 ? Sampler : VK_NULL_HANDLE;
                writes[binding] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[binding].dstSet = Sets[set];
                writes[binding].dstBinding = binding;
                writes[binding].descriptorCount = 1;
                writes[binding].descriptorType = bindings[binding].descriptorType;
                writes[binding].pImageInfo = &images[binding];
            }
            vkUpdateDescriptorSets(Device.Device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
        const VkPushConstantRange push{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(MxaoConstants) };
        VkPipelineLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &SetLayout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        Check(vkCreatePipelineLayout(Device.Device, &layout, nullptr, &Layout));
        const std::array sources{ Fast::Immerse::BuildMxaoDeinterleaveShader(), Fast::Immerse::BuildMxaoOcclusionShader(),
                                  Fast::Immerse::BuildMxaoFilterShader(false), Fast::Immerse::BuildMxaoFilterShader(true) };
        for (uint32_t program = 0; program < kProgramCount; ++program) {
            const auto shader = Device.Shader(sources[program], shaderc_compute_shader);
            VkComputePipelineCreateInfo pipeline{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            pipeline.layout = Layout;
            pipeline.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                               nullptr,
                               0,
                               VK_SHADER_STAGE_COMPUTE_BIT,
                               shader,
                               "main",
                               nullptr };
            Check(vkCreateComputePipelines(Device.Device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &Pipelines[program]));
            vkDestroyShaderModule(Device.Device, shader, nullptr);
        }
        VkCommandPoolCreateInfo commands{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        commands.queueFamilyIndex = Device.Family;
        Check(vkCreateCommandPool(Device.Device, &commands, nullptr, &Commands));
        VkCommandBufferAllocateInfo command{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        command.commandPool = Commands;
        command.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(Device.Device, &command, &Command));
    }

    void Barrier(const Image& image, VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags from,
                 VkAccessFlags to, VkPipelineStageFlags source, VkPipelineStageFlags destination) {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = from;
        barrier.dstAccessMask = to;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.Handle;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(Command, source, destination, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    void WrittenToRead(uint32_t image) {
        Barrier(Images[image], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    void Dispatch(uint32_t program, uint32_t set, MxaoConstants constants, uint32_t flags, uint32_t width,
                  uint32_t height, uint32_t group) {
        constants.Flags |= flags;
        vkCmdBindPipeline(Command, VK_PIPELINE_BIND_POINT_COMPUTE, Pipelines[program]);
        vkCmdBindDescriptorSets(Command, VK_PIPELINE_BIND_POINT_COMPUTE, Layout, 0, 1, &Sets[set], 0, nullptr);
        vkCmdPushConstants(Command, Layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        vkCmdDispatch(Command, (width + group - 1) / group, (height + group - 1) / group, 1);
    }

    std::vector<float> Run(const std::vector<float>& depth, const MxaoConstants& constants, uint32_t filter,
                           uint8_t coverage = 255, float slope = 0) {
        auto* encodedDepth = static_cast<float*>(Depths.Mapped);
        auto* encodedNormals = static_cast<std::array<uint8_t, 4>*>(Normals.Mapped);
        const float length = std::sqrt(1 + slope * slope);
        const std::array<uint8_t, 4> normal = { static_cast<uint8_t>(std::round((.5F + .5F * slope / length) * 255)),
                                                128, static_cast<uint8_t>(std::round((.5F + .5F / length) * 255)),
                                                coverage };
        for (size_t i = 0; i < depth.size(); ++i) {
            encodedDepth[i] = constants.Depth[3] != 0 ? (depth[i] - 1) / 99 : (100 - 100 / depth[i]) / 99;
            encodedNormals[i] = normal;
        }
        Check(vkResetCommandPool(Device.Device, Commands, 0));
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        Check(vkBeginCommandBuffer(Command, &begin));
        for (uint32_t i : { kDepth, kNormals }) {
            Barrier(Images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { Width, Height, 1 };
            vkCmdCopyBufferToImage(Command, i == kDepth ? Depths.Handle : Normals.Handle, Images[i].Handle,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            Barrier(Images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        for (uint32_t i : { kTiled, kRaw, kFiltered, kOutput })
            Barrier(Images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        const uint32_t tiledWidth = Fast::Immerse::MxaoTiledExtent(Width);
        const uint32_t tiledHeight = Fast::Immerse::MxaoTiledExtent(Height);
        Dispatch(kDeinterleave, 0, constants, 0, tiledWidth, tiledHeight, 8);
        WrittenToRead(kTiled);
        Dispatch(kOcclusion, 1, constants, 0, tiledWidth, tiledHeight, 16);
        WrittenToRead(kRaw);
        if (filter == 2) {
            Dispatch(kFilter, 2, constants, 0, Width, Height, 8);
            WrittenToRead(kFiltered);
        }
        Dispatch(kResolve, filter == 2 ? 4 : 3, constants,
                 filter == 0 ? Fast::Immerse::kMxaoFilterCopy : Fast::Immerse::kMxaoFilterMirrored, Width, Height, 8);
        Barrier(Images[kOutput], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { Width, Height, 1 };
        vkCmdCopyImageToBuffer(Command, Images[kOutput].Handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, Result.Handle, 1,
                               &copy);
        VkMemoryBarrier host{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(Command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0,
                             nullptr, 0, nullptr);
        Check(vkEndCommandBuffer(Command));
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &Command;
        Check(vkQueueSubmit(Device.Queue, 1, &submit, VK_NULL_HANDLE));
        Check(vkQueueWaitIdle(Device.Queue));
        std::vector<float> values(depth.size());
        std::memcpy(values.data(), Result.Mapped, values.size() * sizeof(float));
        for (float value : values)
            Require(std::isfinite(value) && value >= 0 && value <= 1, "nonfinite or out-of-range MXAO visibility");
        return values;
    }

    ~MxaoFixture() {
        vkDestroyCommandPool(Device.Device, Commands, nullptr);
        for (auto pipeline : Pipelines)
            vkDestroyPipeline(Device.Device, pipeline, nullptr);
        vkDestroyPipelineLayout(Device.Device, Layout, nullptr);
        vkDestroyDescriptorPool(Device.Device, Pool, nullptr);
        vkDestroyDescriptorSetLayout(Device.Device, SetLayout, nullptr);
        vkDestroySampler(Device.Device, Sampler, nullptr);
        Device.Free(Depths);
        Device.Free(Normals);
        Device.Free(Result);
        for (const auto& image : Images) {
            vkDestroyImageView(Device.Device, image.View, nullptr);
            vkDestroyImage(Device.Device, image.Handle, nullptr);
            vkFreeMemory(Device.Device, image.Memory, nullptr);
        }
    }
};

float Difference(const std::vector<float>& a, const std::vector<float>& b) {
    float difference = 0;
    for (size_t i = 0; i < a.size(); ++i)
        difference = std::max(difference, std::abs(a[i] - b[i]));
    return difference;
}
float Minimum(const std::vector<float>& values) {
    return *std::min_element(values.begin(), values.end());
}
} // namespace

int main() try {
    MxaoFixture gpu;
    MxaoConstants constants;
    constants.Width = MxaoFixture::Width;
    constants.Height = MxaoFixture::Height;
    constants.Flags = 1; // Medium
    constants.ViewX = { 2, 0, -1, 0 };
    constants.ViewY = { 0, 2, -1, 0 };
    constants.Depth = { 100, 100, 99, 0 };
    constants.RadiusFade = { 3, std::log(4.0F) / 3.0F, 50, 100 };
    constants.Response = { 1, 1.0F / 100.0F, 4.0F / (255.0F * 255.0F), 0 };
    const std::vector<float> flat(MxaoFixture::Width * MxaoFixture::Height, 10), identity(flat.size(), 1);
    auto sloped = flat;
    for (uint32_t y = 0; y < MxaoFixture::Height; ++y)
        for (uint32_t x = 0; x < MxaoFixture::Width; ++x)
            sloped[y * MxaoFixture::Width + x] = 10 / (1 - .3F * (2 * (x + .5F) / MxaoFixture::Width - 1));
    auto scene = flat;
    for (uint32_t y = 15; y < 80; ++y)
        for (uint32_t x = 15; x < 110; ++x) {
            if (x < 40)
                scene[y * MxaoFixture::Width + x] = 9.8F;
            else if (x > 75)
                scene[y * MxaoFixture::Width + x] = 8.5F;
        }
    for (uint32_t method = 0; method < 4; ++method) {
        auto variant = constants;
        variant.Method = method;
        for (uint32_t filter = 0; filter < 3; ++filter) {
            // Failed by 0.163 (ground truth, slice-count normalization) and by
            // one truncated half-precision step (stored visibility) before.
            Require(Difference(gpu.Run(flat, variant, filter), identity) < .0002F, "RGB8 empty plane darkens");
            Require(Difference(gpu.Run(sloped, variant, filter, 255, .3F), identity) < .001F,
                    "RGB8 sloped plane darkens");
            const auto occluded = gpu.Run(scene, variant, filter);
            std::cout << "method " << method << " filter " << filter << " minimum visibility " << Minimum(occluded)
                      << '\n';
            Require(Minimum(occluded) < .9F, "steps are not occluded");
            Require(Difference(occluded, gpu.Run(scene, variant, filter)) == 0, "MXAO depends on previous contents");
        }
        Require(Difference(gpu.Run(scene, variant, 1, 0), identity) == 0, "missing scene coverage darkens");
        auto disabled = variant;
        disabled.Response[0] = 0;
        Require(Difference(gpu.Run(scene, disabled, 1), identity) == 0, "zero amount alters visibility");
        auto faded = variant;
        faded.RadiusFade[2] = 1;
        faded.RadiusFade[3] = 2;
        Require(Difference(gpu.Run(scene, faded, 1), identity) == 0, "faded distance still darkens");
        auto deeper = variant;
        deeper.Response[0] = 2;
        const auto base = gpu.Run(scene, variant, 1), gamma = gpu.Run(scene, deeper, 1);
        for (size_t i = 0; i < base.size(); ++i)
            Require(gamma[i] <= base[i] + 1.0e-6F, "an amount above 1 lightens occlusion");
        auto wBuffer = variant;
        wBuffer.Depth = { 1, 100, 99, 1 };
        Require(Difference(base, gpu.Run(scene, wBuffer, 1)) < .002F, "W-buffer and perspective reconstruction disagree");
    }
    std::cout << "MXAO GPU: RGB8 planes, occlusion for four methods and three filters, coverage, amount, fade, "
                 "W-buffer, odd extents and repeatability passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
