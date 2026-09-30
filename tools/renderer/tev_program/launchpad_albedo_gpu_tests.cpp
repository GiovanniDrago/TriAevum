#include "fast/immerse/launchpad_albedo_shader.h"
#include "vulkan_test_device.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

using Fast::Immerse::LaunchpadAlbedoConstants;

namespace {
constexpr uint32_t W = 512, H = 288, Levels = Fast::Immerse::kLaunchpadAlbedoMaxLevels;
enum : uint32_t { kInit, kDownsample, kFuse, kMain, kProgramCount };

struct Image {
    VkImage Handle{};
    VkImageView View{};
    VkDeviceMemory Memory{};
    uint32_t Width = 0, Height = 0;
};

float Half(uint16_t bits) {
    const uint32_t exponent = (bits >> 10U) & 31U, mantissa = bits & 1023U;
    const float value = exponent == 0U ? std::ldexp(float(mantissa), -24)
                                       : std::ldexp(float(mantissa | 1024U), int(exponent) - 25);
    return (bits & 0x8000U) != 0U ? -value : value;
}

// Production albedo programs, dispatched in LaunchpadAlbedoPass's order.
struct AlbedoFixture {
    Gpu Device{ VkPhysicalDeviceFeatures{ .shaderStorageImageExtendedFormats = VK_TRUE } };
    uint32_t Lowest = Fast::Immerse::LaunchpadAlbedoLowestLevel(W, H);
    Image Scene, Fused, Output;
    std::vector<Image> Pyramid, Halves;
    Gpu::Buffer Upload{}, Result{};
    VkSampler Sampler{};
    VkDescriptorSetLayout SetLayout{};
    VkPipelineLayout Layout{};
    VkDescriptorPool Pool{};
    std::array<VkPipeline, kProgramCount> Pipelines{};
    VkCommandPool Commands{};
    VkCommandBuffer Command{};
    struct Step {
        uint32_t Program;
        const Image* Source;
        const Image* Destination;
        bool Horizontal;
        VkDescriptorSet Set{};
    };
    std::vector<Step> Steps;

    Image MakeImage(VkFormat format, uint32_t width, uint32_t height) {
        Image image;
        image.Width = width;
        image.Height = height;
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

    AlbedoFixture() {
        Scene = MakeImage(VK_FORMAT_R8G8B8A8_UNORM, W, H);
        for (uint32_t k = 0; k <= Lowest; ++k) {
            Pyramid.push_back(MakeImage(VK_FORMAT_R16G16_SFLOAT, std::max(1U, (W / 2) >> k), std::max(1U, (H / 2) >> k)));
            Halves.push_back(k == 0 ? Image{}
                                    : MakeImage(VK_FORMAT_R16G16_SFLOAT, std::max(1U, (W / 2) >> k),
                                                std::max(1U, (H / 2) >> (k - 1))));
        }
        Fused = MakeImage(VK_FORMAT_R16G16_SFLOAT, W / 2, H / 2);
        Output = MakeImage(VK_FORMAT_R16_SFLOAT, W, H);
        Upload = Device.MakeBuffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Result = Device.MakeBuffer(W * H * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        Check(vkCreateSampler(Device.Device, &sampler, nullptr, &Sampler));
        const std::array<std::pair<uint32_t, VkDescriptorType>, 5> kinds{ {
            { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE }, { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE },
            { 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE }, { 4, VK_DESCRIPTOR_TYPE_SAMPLER },
            { 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE } } };
        std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
        for (size_t i = 0; i < bindings.size(); ++i) {
            bindings[i].binding = kinds[i].first;
            bindings[i].descriptorType = kinds[i].second;
            bindings[i].descriptorCount = kinds[i].first == 5 ? Levels : 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo setLayout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        setLayout.bindingCount = uint32_t(bindings.size());
        setLayout.pBindings = bindings.data();
        Check(vkCreateDescriptorSetLayout(Device.Device, &setLayout, nullptr, &SetLayout));
        const VkPushConstantRange push{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(LaunchpadAlbedoConstants) };
        VkPipelineLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &SetLayout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        Check(vkCreatePipelineLayout(Device.Device, &layout, nullptr, &Layout));
        const std::array sources{ Fast::Immerse::BuildLaunchpadAlbedoInitShader(),
                                  Fast::Immerse::BuildLaunchpadAlbedoDownsampleShader(),
                                  Fast::Immerse::BuildLaunchpadAlbedoFuseShader(),
                                  Fast::Immerse::BuildLaunchpadAlbedoMainShader() };
        for (uint32_t program = 0; program < kProgramCount; ++program) {
            const auto shader = Device.Shader(sources[program], shaderc_compute_shader);
            VkComputePipelineCreateInfo pipeline{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            pipeline.layout = Layout;
            pipeline.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                               VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr };
            Check(vkCreateComputePipelines(Device.Device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &Pipelines[program]));
            vkDestroyShaderModule(Device.Device, shader, nullptr);
        }
        Steps.push_back({ kInit, &Pyramid[0], &Pyramid[0], false });
        for (uint32_t k = 1; k <= Lowest; ++k) {
            Steps.push_back({ kDownsample, &Pyramid[k - 1], &Halves[k], true });
            Steps.push_back({ kDownsample, &Halves[k], &Pyramid[k], false });
        }
        Steps.push_back({ kFuse, &Pyramid[0], &Fused, false });
        Steps.push_back({ kMain, &Fused, &Output, false });
        const std::array<VkDescriptorPoolSize, 3> sizes{ {
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, uint32_t(Steps.size()) * (2 + Levels) },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, uint32_t(Steps.size()) },
            { VK_DESCRIPTOR_TYPE_SAMPLER, uint32_t(Steps.size()) } } };
        VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool.maxSets = uint32_t(Steps.size());
        pool.poolSizeCount = uint32_t(sizes.size());
        pool.pPoolSizes = sizes.data();
        Check(vkCreateDescriptorPool(Device.Device, &pool, nullptr, &Pool));
        for (auto& step : Steps) {
            VkDescriptorSetAllocateInfo allocation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocation.descriptorPool = Pool;
            allocation.descriptorSetCount = 1;
            allocation.pSetLayouts = &SetLayout;
            Check(vkAllocateDescriptorSets(Device.Device, &allocation, &step.Set));
            const auto sampled = [](const Image& image) {
                return VkDescriptorImageInfo{ VK_NULL_HANDLE, image.View, VK_IMAGE_LAYOUT_GENERAL };
            };
            const VkDescriptorImageInfo scene = sampled(Scene), source = sampled(*step.Source);
            const VkDescriptorImageInfo destination{ VK_NULL_HANDLE, step.Destination->View, VK_IMAGE_LAYOUT_GENERAL };
            const VkDescriptorImageInfo samplerInfo{ Sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
            std::array<VkDescriptorImageInfo, Levels> levels{};
            for (uint32_t k = 0; k < Levels; ++k)
                levels[k] = sampled(Pyramid[std::min(k, Lowest)]);
            std::array<VkWriteDescriptorSet, 5> writes{};
            const std::array<const VkDescriptorImageInfo*, 5> infos{ &scene, &source, &destination, &samplerInfo,
                                                                     levels.data() };
            for (size_t i = 0; i < writes.size(); ++i) {
                writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[i].dstSet = step.Set;
                writes[i].dstBinding = kinds[i].first;
                writes[i].descriptorCount = kinds[i].first == 5 ? Levels : 1;
                writes[i].descriptorType = kinds[i].second;
                writes[i].pImageInfo = infos[i];
            }
            vkUpdateDescriptorSets(Device.Device, uint32_t(writes.size()), writes.data(), 0, nullptr);
        }
        VkCommandPoolCreateInfo commands{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        commands.queueFamilyIndex = Device.Family;
        Check(vkCreateCommandPool(Device.Device, &commands, nullptr, &Commands));
        VkCommandBufferAllocateInfo command{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        command.commandPool = Commands;
        command.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(Device.Device, &command, &Command));
    }

    void Barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags source, VkAccessFlags target,
                 VkPipelineStageFlags sourceStage, VkPipelineStageFlags targetStage) {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = source;
        barrier.dstAccessMask = target;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(Command, sourceStage, targetStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    // Scene colour (display-encoded RGB in 0..1) to the albedo estimate's mean.
    std::vector<float> Run(const std::vector<std::array<float, 3>>& color) {
        auto* texels = static_cast<std::array<uint8_t, 4>*>(Upload.Mapped);
        for (size_t i = 0; i < color.size(); ++i)
            texels[i] = { uint8_t(std::lround(color[i][0] * 255)), uint8_t(std::lround(color[i][1] * 255)),
                          uint8_t(std::lround(color[i][2] * 255)), 255 };
        Check(vkResetCommandPool(Device.Device, Commands, 0));
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        Check(vkBeginCommandBuffer(Command, &begin));
        // Every image stays in GENERAL; compute-to-compute barriers order them.
        std::vector<VkImage> images{ Scene.Handle, Fused.Handle, Output.Handle };
        for (uint32_t k = 0; k <= Lowest; ++k) {
            images.push_back(Pyramid[k].Handle);
            if (k != 0)
                images.push_back(Halves[k].Handle);
        }
        for (VkImage image : images)
            Barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { W, H, 1 };
        vkCmdCopyBufferToImage(Command, Upload.Handle, Scene.Handle, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        VkMemoryBarrier memory{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        memory.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(Command, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
        LaunchpadAlbedoConstants constants;
        constants.LowestLevel = Lowest;
        for (uint32_t k = 0; k <= Lowest; ++k)
            constants.LevelExtents[k] = { Pyramid[k].Width, Pyramid[k].Height };
        for (const auto& step : Steps) {
            constants.Width = step.Destination->Width;
            constants.Height = step.Destination->Height;
            constants.SourceWidth = step.Program == kInit ? W : step.Source->Width;
            constants.SourceHeight = step.Program == kInit ? H : step.Source->Height;
            constants.Horizontal = step.Horizontal ? 1 : 0;
            vkCmdBindPipeline(Command, VK_PIPELINE_BIND_POINT_COMPUTE, Pipelines[step.Program]);
            vkCmdBindDescriptorSets(Command, VK_PIPELINE_BIND_POINT_COMPUTE, Layout, 0, 1, &step.Set, 0, nullptr);
            vkCmdPushConstants(Command, Layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
            vkCmdDispatch(Command, (constants.Width + 7) / 8, (constants.Height + 7) / 8, 1);
            memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(Command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &memory, 0, nullptr, 0, nullptr);
        }
        memory.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(Command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                             &memory, 0, nullptr, 0, nullptr);
        vkCmdCopyImageToBuffer(Command, Output.Handle, VK_IMAGE_LAYOUT_GENERAL, Result.Handle, 1, &copy);
        memory.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        memory.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(Command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &memory, 0,
                             nullptr, 0, nullptr);
        Check(vkEndCommandBuffer(Command));
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &Command;
        Check(vkQueueSubmit(Device.Queue, 1, &submit, VK_NULL_HANDLE));
        Check(vkQueueWaitIdle(Device.Queue));
        std::vector<float> values(size_t(W) * H);
        const auto* halves = static_cast<const uint16_t*>(Result.Mapped);
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = Half(halves[i]);
            Require(std::isfinite(values[i]) && values[i] >= 0, "nonfinite or negative albedo estimate");
        }
        return values;
    }

    ~AlbedoFixture() {
        vkDestroyCommandPool(Device.Device, Commands, nullptr);
        for (auto pipeline : Pipelines)
            vkDestroyPipeline(Device.Device, pipeline, nullptr);
        vkDestroyPipelineLayout(Device.Device, Layout, nullptr);
        vkDestroyDescriptorPool(Device.Device, Pool, nullptr);
        vkDestroyDescriptorSetLayout(Device.Device, SetLayout, nullptr);
        vkDestroySampler(Device.Device, Sampler, nullptr);
        Device.Free(Upload);
        Device.Free(Result);
        std::vector<Image> images{ Scene, Fused, Output };
        images.insert(images.end(), Pyramid.begin(), Pyramid.end());
        images.insert(images.end(), Halves.begin() + 1, Halves.end());
        for (const auto& image : images) {
            vkDestroyImageView(Device.Device, image.View, nullptr);
            vkDestroyImage(Device.Device, image.Handle, nullptr);
            vkFreeMemory(Device.Device, image.Memory, nullptr);
        }
    }
};

// Mean of the estimate over a column band, away from the border.
double BandMean(const std::vector<float>& values, uint32_t from, uint32_t to) {
    double sum = 0;
    size_t count = 0;
    for (uint32_t y = 16; y + 16 < H; ++y)
        for (uint32_t x = from; x < to; ++x, ++count)
            sum += values[size_t(y) * W + x];
    return sum / double(count);
}
} // namespace

int main() try {
    AlbedoFixture gpu;
    // A flat colour is its own albedo: the estimate is uniform.
    const auto flat = gpu.Run(std::vector<std::array<float, 3>>(size_t(W) * H, { .45F, .40F, .30F }));
    const auto [low, high] = std::minmax_element(flat.begin(), flat.end());
    std::cout << "flat colour: estimate " << *low << ".." << *high << '\n';
    Require(*high - *low < .01F * *high, "a flat colour is not uniform");
    // A fine checker under a lighting ramp: the ramp is equalized away while
    // the texture contrast stays.
    std::vector<std::array<float, 3>> lit(size_t(W) * H);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const float light = .25F + .6F * float(x) / float(W - 1);
            const float texture = ((x / 4U + y / 4U) % 2U) == 0U ? .6F : 1.0F;
            lit[size_t(y) * W + x] = { light * texture, light * texture, light * texture };
        }
    const auto equalized = gpu.Run(lit);
    const double left = BandMean(equalized, 32, 96), right = BandMean(equalized, W - 96, W - 32);
    const double inputRatio = (.25 + .6 * 448.0 / 511.0) / (.25 + .6 * 64.0 / 511.0);
    double contrast = 0;
    for (uint32_t x = 200; x < 312; ++x)
        contrast = std::max(contrast, double(std::abs(equalized[size_t(H / 2) * W + x] -
                                                      equalized[size_t(H / 2) * W + x + 4])));
    // The estimate is normalized around the original's HDR mid-grey (about
    // 0.025), so the checker contrast is measured against the local mean.
    const double centre = BandMean(equalized, 200, 316);
    std::cout << "lighting ramp " << inputRatio << "x: estimate right/left " << right / left
              << ", checker contrast " << contrast / centre << " of the mean\n";
    Require(right / left < 1.0 + .35 * (inputRatio - 1.0), "the lighting ramp survives the equalization");
    Require(contrast > .3 * centre, "the texture contrast is lost");
    std::cout << "LAUNCHPAD albedo GPU: uniform colour, lighting equalization and texture contrast passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
