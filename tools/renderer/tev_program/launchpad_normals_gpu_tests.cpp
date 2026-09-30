#include "fast/immerse/launchpad_normals_shader.h"
#include "vulkan_test_device.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <cstdlib>
#include <string>

using Fast::Immerse::LaunchpadNormalsConstants;

namespace {
struct Image {
    VkImage Handle{};
    VkImageView View{};
    VkDeviceMemory Memory{};
};

enum : uint32_t { kDepth, kNormals, kGbuffer, kWide, kOutput, kImageCount };
enum : uint32_t { kGbufferProgram, kWideProgram, kNarrowProgram, kProgramCount };

using Vec3 = std::array<float, 3>;
using Texel = std::array<float, 4>;
constexpr float kPi = 3.14159265358979323846F;

float Dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Vec3 Normalize(Vec3 v) {
    const float length = std::sqrt(Dot(v, v));
    return { v[0] / length, v[1] / length, v[2] / length };
}
float Degrees(const Vec3& a, const Vec3& b) {
    return std::acos(std::clamp(Dot(Normalize(a), Normalize(b)), -1.0F, 1.0F)) * 180.0F / kPi;
}
float Half(uint16_t bits) {
    const uint32_t exponent = (bits >> 10U) & 31U, mantissa = bits & 1023U;
    const float value = exponent == 0U   ? std::ldexp(float(mantissa), -24)
                        : exponent == 31U ? std::numeric_limits<float>::quiet_NaN()
                                          : std::ldexp(float(mantissa | 1024U), int(exponent) - 25);
    return (bits & 0x8000U) != 0U ? -value : value;
}

// A synthetic native frame: view depth (0 = sky) and the RGBA8 guide.
struct Scene {
    std::vector<float> Depth;
    std::vector<std::array<uint8_t, 4>> Guide;
};

// Production LAUNCHPAD smoothing on native RGBA8 normals. The odd extents
// exercise partial workgroups and the odd half height of the wide iteration.
struct LaunchpadFixture {
    uint32_t Width, Height, HalfHeight;
    Gpu Device{ VkPhysicalDeviceFeatures{ .shaderStorageImageExtendedFormats = VK_TRUE } };
    VkQueryPool Timestamps{};
    float TimestampPeriod = 1;
    static constexpr uint32_t kMaxRepeats = 32;
    // GPU milliseconds of the half-height (G-buffer and wide) and narrow
    // dispatches of the last run: the mean of the second half of its repeats,
    // once clocks have settled.
    std::array<double, 2> LastMilliseconds{};
    std::array<Image, kImageCount> Images{};
    Gpu::Buffer Depths{}, Normals{}, Result{};
    VkSampler Sampler{};
    VkDescriptorSetLayout SetLayout{};
    VkDescriptorPool Pool{};
    // Sets pair the packed surfaces (binding 2) with an output (binding 3), as
    // in LaunchpadNormalsPass: the G-buffer program does not read binding 2.
    static constexpr std::array<std::array<uint32_t, 2>, kProgramCount> kPairs{ { { kWide, kGbuffer },
                                                                                   { kGbuffer, kWide },
                                                                                   { kWide, kOutput } } };
    std::array<VkDescriptorSet, kProgramCount> Sets{};
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

    LaunchpadFixture(uint32_t width, uint32_t height)
        : Width(width), Height(height), HalfHeight(Fast::Immerse::LaunchpadHalfHeight(height)) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(Device.Physical, &properties);
        TimestampPeriod = properties.limits.timestampPeriod;
        VkQueryPoolCreateInfo queries{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queries.queryCount = 2 * kMaxRepeats + 1;
        Check(vkCreateQueryPool(Device.Device, &queries, nullptr, &Timestamps));
        Images = { MakeImage(VK_FORMAT_R32_SFLOAT, Width, Height), MakeImage(VK_FORMAT_R8G8B8A8_UNORM, Width, Height),
                   MakeImage(VK_FORMAT_R32G32_UINT, Width, HalfHeight),
                   MakeImage(VK_FORMAT_R32G32_UINT, Width, HalfHeight),
                   MakeImage(VK_FORMAT_R16G16B16A16_SFLOAT, Width, Height) };
        Depths = Device.MakeBuffer(Width * Height * sizeof(float), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Normals = Device.MakeBuffer(Width * Height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Result = Device.MakeBuffer(Width * Height * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
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
        const std::array<VkDescriptorPoolSize, 3> sizes{ { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * kProgramCount },
                                                           { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kProgramCount },
                                                           { VK_DESCRIPTOR_TYPE_SAMPLER, kProgramCount } } };
        VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool.maxSets = kProgramCount;
        pool.poolSizeCount = static_cast<uint32_t>(sizes.size());
        pool.pPoolSizes = sizes.data();
        Check(vkCreateDescriptorPool(Device.Device, &pool, nullptr, &Pool));
        std::array<VkDescriptorSetLayout, kProgramCount> layouts{};
        layouts.fill(SetLayout);
        VkDescriptorSetAllocateInfo allocation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocation.descriptorPool = Pool;
        allocation.descriptorSetCount = kProgramCount;
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
        const VkPushConstantRange push{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(LaunchpadNormalsConstants) };
        VkPipelineLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &SetLayout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        Check(vkCreatePipelineLayout(Device.Device, &layout, nullptr, &Layout));
        const std::array sources{ Fast::Immerse::BuildLaunchpadGbufferShader(),
                                  Fast::Immerse::BuildLaunchpadSmoothWideShader(),
                                  Fast::Immerse::BuildLaunchpadSmoothNarrowShader() };
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
    void Dispatch(uint32_t program, LaunchpadNormalsConstants constants, uint32_t height) {
        constants.Iteration = program;
        vkCmdBindPipeline(Command, VK_PIPELINE_BIND_POINT_COMPUTE, Pipelines[program]);
        vkCmdBindDescriptorSets(Command, VK_PIPELINE_BIND_POINT_COMPUTE, Layout, 0, 1, &Sets[program], 0, nullptr);
        vkCmdPushConstants(Command, Layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        vkCmdDispatch(Command, (Width + 7) / 8, (height + 7) / 8, 1);
    }

    std::vector<Texel> Run(const Scene& scene, const LaunchpadNormalsConstants& constants, uint32_t repeats = 1) {
        repeats = std::clamp<uint32_t>(repeats, 1, kMaxRepeats);
        auto* encodedDepth = static_cast<float*>(Depths.Mapped);
        const bool wBuffer = constants.Depth[3] != 0;
        for (size_t i = 0; i < scene.Depth.size(); ++i) {
            const float z = scene.Depth[i];
            encodedDepth[i] = z <= 0 ? 1.0F : wBuffer ? (z - 1) / 99 : (100 - 100 / z) / 99;
        }
        std::memcpy(Normals.Mapped, scene.Guide.data(), scene.Guide.size() * 4);
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
        for (uint32_t i : { kGbuffer, kWide, kOutput })
            Barrier(Images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        const auto toRead = [&](uint32_t image) {
            Barrier(Images[image], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        };
        vkCmdResetQueryPool(Command, Timestamps, 0, 2 * repeats + 1);
        vkCmdWriteTimestamp(Command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, Timestamps, 0);
        for (uint32_t repeat = 0; repeat < repeats; ++repeat) {
            if (repeat != 0) {
                for (uint32_t image : { kGbuffer, kWide })
                    Barrier(Images[image], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
                Barrier(Images[kOutput], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            }
            Dispatch(kGbufferProgram, constants, HalfHeight);
            toRead(kGbuffer);
            Dispatch(kWideProgram, constants, HalfHeight);
            toRead(kWide);
            vkCmdWriteTimestamp(Command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, Timestamps, 2 * repeat + 1);
            Dispatch(kNarrowProgram, constants, Height);
            vkCmdWriteTimestamp(Command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, Timestamps, 2 * repeat + 2);
        }
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
        std::vector<uint64_t> ticks(2 * repeats + 1);
        Check(vkGetQueryPoolResults(Device.Device, Timestamps, 0, 2 * repeats + 1, ticks.size() * sizeof(uint64_t),
                                    ticks.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        LastMilliseconds = {};
        const uint32_t first = repeats / 2;
        for (uint32_t repeat = first; repeat < repeats; ++repeat)
            for (uint32_t i = 0; i < 2; ++i)
                LastMilliseconds[i] += double(ticks[2 * repeat + i + 1] - ticks[2 * repeat + i]) * TimestampPeriod *
                                       1.0e-6 / double(repeats - first);
        std::vector<Texel> values(scene.Depth.size());
        const auto* halves = static_cast<const uint16_t*>(Result.Mapped);
        for (size_t i = 0; i < values.size(); ++i)
            for (size_t c = 0; c < 4; ++c) {
                values[i][c] = Half(halves[i * 4 + c]);
                Require(std::isfinite(values[i][c]), "nonfinite smoothed guide");
            }
        return values;
    }

    ~LaunchpadFixture() {
        vkDestroyQueryPool(Device.Device, Timestamps, nullptr);
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

// Fixture extent; the odd defaults exercise partial workgroups and an odd half
// height, the benchmark uses the reference 2560x1440.
uint32_t W = 641, H = 481;

// Rays of the fixture projection: view X / depth = 2u - 1, Y / depth = 2v - 1.
Vec3 Ray(uint32_t x, uint32_t y) {
    return { 2 * (x + .5F) / W - 1, 2 * (y + .5F) / H - 1, 1 };
}
// Normals are given in the renderer's mirrored view space (depth positive, a
// surface facing the camera has negative Z); the guide stores native view
// space, where that Z is positive.
std::array<uint8_t, 4> Encode(const Vec3& mirrored, uint8_t coverage = 255) {
    const Vec3 n = Normalize(mirrored);
    const auto channel = [](float v) { return static_cast<uint8_t>(std::lround((v * .5F + .5F) * 255)); };
    return { channel(n[0]), channel(n[1]), channel(-n[2]), coverage };
}
// Both decode back into the mirrored view space of the fixture geometry.
Vec3 DecodeGuide(const std::array<uint8_t, 4>& guide) {
    return { guide[0] / 127.5F - 1, guide[1] / 127.5F - 1, 1 - guide[2] / 127.5F };
}
Vec3 DecodeOutput(const Texel& texel) {
    return { texel[0] * 2 - 1, texel[1] * 2 - 1, 1 - texel[2] * 2 };
}

// Each pixel sees the nearest of the given planes {normal, distance} whose
// front side it enters (a convex solid), or the background plane.
Scene Planes(const std::vector<std::pair<Vec3, float>>& planes, bool convex = true) {
    Scene scene;
    scene.Depth.resize(W * H);
    scene.Guide.resize(W * H);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const Vec3 ray = Ray(x, y);
            float enter = 0, exit = std::numeric_limits<float>::max();
            Vec3 normal{ 0, 0, -1 };
            for (const auto& [n, d] : planes) {
                const float along = Dot(n, ray);
                const float t = d / along;
                if (along < 0) {
                    if (t > enter) {
                        enter = t;
                        normal = n;
                    }
                } else if (convex) {
                    exit = std::min(exit, t);
                }
            }
            const size_t i = size_t(y) * W + x;
            const bool hit = enter > 0 && enter <= exit;
            scene.Depth[i] = hit ? enter : 40.0F;
            scene.Guide[i] = Encode(hit ? normal : Vec3{ 0, 0, -1 });
        }
    return scene;
}
// Plane through depth `z` on the view axis with the given normal.
std::pair<Vec3, float> Plane(Vec3 normal, float z) {
    normal = Normalize(normal);
    return { normal, normal[2] * z };
}

// A cylinder around the view Y axis built from flat facets, as low-poly
// geometry reaches the renderer: planar depth and one normal per facet.
constexpr float kCenter = 6, kRadius = 3;
Scene Prism(uint32_t facets) {
    std::vector<std::pair<Vec3, float>> planes;
    for (uint32_t k = 0; k < facets; ++k) {
        const float angle = 2 * kPi * (k + .5F) / facets;
        const Vec3 n{ std::sin(angle), 0, -std::cos(angle) };
        // Chord plane: dot(n, P - centre) = R cos(half facet angle).
        planes.push_back({ n, n[2] * kCenter + kRadius * std::cos(kPi / facets) });
    }
    return Planes(planes);
}
// Normal of the round cylinder at the point a pixel sees on the prism.
Vec3 CylinderNormal(uint32_t x, uint32_t y, float depth) {
    const Vec3 ray = Ray(x, y);
    return Normalize({ ray[0] * depth, 0, depth - kCenter });
}

struct Statistics {
    float MeanError = 0, MaxJump = 0;
    size_t Samples = 0;
};
// Error against the round cylinder and the largest normal step between
// horizontal neighbours, inside the silhouette.
Statistics Measure(const Scene& scene, const std::function<Vec3(size_t)>& normal) {
    Statistics statistics;
    double error = 0;
    for (uint32_t y = H / 4; y < 3 * H / 4; ++y)
        for (uint32_t x = 1; x + 1 < W; ++x) {
            const size_t i = size_t(y) * W + x;
            const Vec3 ray = Ray(x, y);
            // Stay clear of the silhouette, where depth jumps to the background.
            if (std::abs(ray[0]) > .45F)
                continue;
            error += Degrees(normal(i), CylinderNormal(x, y, scene.Depth[i]));
            statistics.MaxJump = std::max(statistics.MaxJump, Degrees(normal(i), normal(i + 1)));
            ++statistics.Samples;
        }
    statistics.MeanError = float(error / double(std::max<size_t>(statistics.Samples, 1)));
    return statistics;
}

float MaxDeviation(const Scene& scene, const std::vector<Texel>& output) {
    float deviation = 0;
    for (size_t i = 0; i < output.size(); ++i)
        deviation = std::max(deviation, Degrees(DecodeOutput(output[i]), DecodeGuide(scene.Guide[i])));
    return deviation;
}
float MaxDifference(const std::vector<Texel>& a, const std::vector<Texel>& b) {
    float difference = 0;
    for (size_t i = 0; i < a.size(); ++i)
        for (size_t c = 0; c < 4; ++c)
            difference = std::max(difference, std::abs(a[i][c] - b[i][c]));
    return difference;
}

LaunchpadNormalsConstants FixtureConstants() {
    LaunchpadNormalsConstants constants;
    constants.Width = W;
    constants.Height = H;
    constants.ViewX = { 2, 0, -1, 0 };
    constants.ViewY = { 0, 2, -1, 0 };
    constants.Depth = { 100, 100, 99, 0 };
    const float side = float(std::min(W, H));
    constants.Scale = { 1, Fast::Immerse::kLaunchpadWideRadius * side, Fast::Immerse::kLaunchpadNarrowRadius * side,
                        0 };
    return constants;
}

// GPU time of both iterations on a faceted cylinder at the reference extent.
int Benchmark() {
    W = 2560;
    H = 1440;
    LaunchpadFixture gpu(W, H);
    const auto constants = FixtureConstants();
    const auto prism = Prism(48);
    // Grass-like guides: every pixel's normal tilted at random by up to 25
    // degrees, so neighbouring threads project different tangent directions.
    auto noisy = prism;
    uint32_t state = 12345U;
    const auto random = [&state] {
        state = state * 1664525U + 1013904223U;
        return float(state >> 8U) / float(1U << 24U) * 2.0F - 1.0F;
    };
    for (auto& guide : noisy.Guide) {
        Vec3 n = DecodeGuide(guide);
        n = Normalize({ n[0] + .45F * random(), n[1] + .45F * random(), n[2] });
        guide = Encode(n);
    }
    const std::array<std::pair<const char*, const Scene*>, 2> scenes{ { { "faceted cylinder", &prism },
                                                                        { "noisy normals", &noisy } } };
    for (const auto& [name, scene] : scenes) {
        gpu.Run(*scene, constants, LaunchpadFixture::kMaxRepeats);
        std::cout << "LAUNCHPAD normals 2560x1440, " << name << ": G-buffer + wide " << gpu.LastMilliseconds[0]
                  << " ms, narrow " << gpu.LastMilliseconds[1] << " ms (mean of the last 16 of 32 repeats)\n";
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) try {
    if (argc > 1 && std::string(argv[1]) == "--benchmark")
        return Benchmark();
    LaunchpadFixture gpu(W, H);
    const auto constants = FixtureConstants();

    // Planes keep their normal: every neighbour agrees, so nothing leans.
    const auto facing = Planes({ Plane({ 0, 0, -1 }, 8) });
    Require(MaxDeviation(facing, gpu.Run(facing, constants)) < .1F, "a facing plane changes its normal");
    const auto oblique = Planes({ Plane({ .45F, -.2F, -1 }, 8) });
    Require(MaxDeviation(oblique, gpu.Run(oblique, constants)) < .1F, "an oblique plane changes its normal");

    // A 50-degree ridge exceeds the 45-degree tolerance: the crease stays.
    const float ridge = 25 * kPi / 180;
    const auto crease = Planes({ Plane({ -std::sin(ridge), 0, -std::cos(ridge) }, 5),
                                 Plane({ std::sin(ridge), 0, -std::cos(ridge) }, 5) });
    Require(MaxDeviation(crease, gpu.Run(crease, constants)) < .1F, "a hard crease is smoothed");

    // Faceted cylinders: facets 7.5 and 15 degrees apart become a continuous
    // surface closer to the round one.
    for (const uint32_t facets : { 48U, 24U }) {
        const auto prism = Prism(facets);
        const auto smoothed = gpu.Run(prism, constants);
        const auto before = Measure(prism, [&](size_t i) { return DecodeGuide(prism.Guide[i]); });
        const auto after = Measure(prism, [&](size_t i) { return DecodeOutput(smoothed[i]); });
        std::cout << facets << " facets: mean error " << before.MeanError << " -> " << after.MeanError
                  << " degrees, largest step " << before.MaxJump << " -> " << after.MaxJump << " degrees over "
                  << after.Samples << " pixels\n";
        Require(after.Samples > 10000, "prism fixture misses the screen");
        // Measured on an RTX 3060: steps 7.7 -> 0.5 and 15.0 -> 0.8 degrees,
        // mean error 1.9 -> 0.5 and 3.8 -> 2.2 degrees.
        Require(after.MaxJump < .25F * before.MaxJump, "facet edges are not smoothed");
        Require(after.MeanError < .75F * before.MeanError, "smoothing does not approach the round surface");
        Require(MaxDifference(smoothed, gpu.Run(prism, constants)) == 0, "smoothing depends on previous contents");
        auto wBuffer = constants;
        wBuffer.Depth = { 1, 100, 99, 1 };
        Require(MaxDifference(smoothed, gpu.Run(prism, wBuffer)) < .002F,
                "W-buffer and perspective reconstruction disagree");
    }

    // Uncovered texels and sky keep the native guide; coverage passes through.
    auto partial = Prism(48);
    for (uint32_t y = 100; y < 200; ++y)
        for (uint32_t x = 250; x < 400; ++x)
            partial.Guide[size_t(y) * W + x][3] = x < 320 ? 0 : 200;
    for (uint32_t y = 0; y < 40; ++y)
        for (uint32_t x = 0; x < W; ++x)
            partial.Depth[size_t(y) * W + x] = 0;
    const auto kept = gpu.Run(partial, constants);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const size_t i = size_t(y) * W + x;
            const auto& guide = partial.Guide[i];
            if (guide[3] == 0 || partial.Depth[i] == 0) {
                for (size_t c = 0; c < 4; ++c)
                    Require(std::abs(kept[i][c] - guide[c] / 255.0F) < 5.0e-4F, "an uncovered texel was changed");
            } else {
                Require(std::abs(kept[i][3] - guide[3] / 255.0F) < 5.0e-4F, "coverage was not preserved");
            }
        }
    std::cout << "LAUNCHPAD normals GPU: planes, crease, faceted cylinders, coverage, sky, W-buffer, odd extents and "
                 "repeatability passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
