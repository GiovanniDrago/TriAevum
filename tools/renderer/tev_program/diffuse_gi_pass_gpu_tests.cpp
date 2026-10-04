// GPU test of the M5 runtime pass of the diffuse screen-space GI
// (docs/TRIAEVUM_SCREEN_SPACE_GI.md, "M5"). The pass object
// (fast/renderer/diffuse_gi_pass.h) runs the precompiled kernels of
// builtin_pass.spvpack on synthetic sequences and is compared
//   - stage by stage with the FP32 twins, each fed with the GPU's own inputs
//     to that stage (gather, temporal accumulation, a-trous filter, output);
//   - end to end with the FP32 CPU chain;
//   - with the same kernels compiled at run time as the M2-M4 GPU twins
//     compile them (Vulkan 1.1, no optimization).
// It also covers history ownership (two views, a shared instance), every reset
// reason through the pass API, resize and screen-resource invalidation with a
// NaN-poisoned history, the declared VRAM, the barrier records, the neutral
// outputs, a padded poisoned viewport, odd extents with poisoned inputs and
// HDR, under the Khronos validation layer with synchronization validation when
// installed. Lines starting with DGI_GPU_M5 are quoted in the document.
#include "fast/renderer/diffuse_gi_gather.h"
#include "fast/renderer/diffuse_gi_hierarchy.h"
#include "fast/renderer/diffuse_gi_pass.h"
#include "fast/renderer/diffuse_gi_pass_plan.h"
#include "fast/renderer/diffuse_gi_pass_shader.h"
#include "fast/renderer/diffuse_gi_scene.h"
#include "fast/renderer/diffuse_gi_sequence.h"
#include "fast/renderer/diffuse_gi_temporal.h"
#include "fast/renderer/diffuse_gi_temporal_shader.h"
#include "fast/renderer/shaderc_compiler.h"

#include <vulkan/vulkan.h>
#include <shaderc/shaderc.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Fast::Renderer::DiffuseGi;
using namespace Fast::Renderer::DiffuseGi::Synthetic;

namespace {

void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
void Check(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("Vulkan result " + std::to_string(result));
}

uint32_t gErrors = 0, gSyncHazards = 0, gLoaderMessages = 0;
std::string gFirstMessage;

VKAPI_ATTR VkBool32 VKAPI_CALL OnMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (!(type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)) {
        ++gLoaderMessages; // environmental (third-party implicit layers)
        return VK_FALSE;
    }
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++gErrors;
        const std::string id = data->pMessageIdName ? data->pMessageIdName : "";
        if (id.find("SYNC-") != std::string::npos) ++gSyncHazards;
        if (gFirstMessage.empty()) gFirstMessage = data->pMessage ? data->pMessage : id;
    }
    return VK_FALSE;
}

// Vulkan 1.2 test device (the pack holds Vulkan 1.2 modules) with the
// validation layer and synchronization validation when installed.
struct Device {
    VkInstance Instance{};
    VkPhysicalDevice Physical{};
    VkDevice Handle{};
    VkQueue Queue{};
    uint32_t Family = 0;
    VkPhysicalDeviceMemoryProperties Memory{};
    VkDebugUtilsMessengerEXT Messenger{};
    VkCommandPool Pool{};
    VkCommandBuffer Command{};
    bool Validation = false;
    struct Buffer {
        VkBuffer Handle{};
        VkDeviceMemory Memory{};
        void* Mapped{};
        VkDeviceSize Size = 0;
    };
    struct Image {
        VkImage Handle{};
        VkDeviceMemory Memory{};
        VkImageView View{};
        VkFormat Format = VK_FORMAT_UNDEFINED;
        uint32_t Width = 0, Height = 0;
    };

    Device() {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> layers(count);
        vkEnumerateInstanceLayerProperties(&count, layers.data());
        for (const auto& l : layers)
            Validation = Validation || std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_2;
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME,
                                    VK_EXT_LAYER_SETTINGS_EXTENSION_NAME};
        const VkBool32 enabled = VK_TRUE;
        const VkLayerSettingEXT setting{"VK_LAYER_KHRONOS_validation", "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT,
                                        1, &enabled};
        VkLayerSettingsCreateInfoEXT settings{VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT};
        settings.settingCount = 1;
        settings.pSettings = &setting;
        VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
        VkDebugUtilsMessengerCreateInfoEXT messenger{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messenger.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger.pfnUserCallback = OnMessage;
        VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        features.enabledValidationFeatureCount = 1;
        features.pEnabledValidationFeatures = enables;
        features.pNext = &settings;
        settings.pNext = &messenger;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        if (Validation) {
            info.enabledLayerCount = 1;
            info.ppEnabledLayerNames = &layer;
            info.enabledExtensionCount = 3;
            info.ppEnabledExtensionNames = extensions;
            info.pNext = &features;
        }
        Check(vkCreateInstance(&info, nullptr, &Instance));
        if (Validation) {
            const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(Instance, "vkCreateDebugUtilsMessengerEXT"));
            Require(create != nullptr, "debug utils unavailable");
            Check(create(Instance, &messenger, nullptr, &Messenger));
        }
        Check(vkEnumeratePhysicalDevices(Instance, &count, nullptr));
        Require(count > 0, "no Vulkan device");
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(Instance, &count, devices.data()));
        Physical = devices[0];
        vkGetPhysicalDeviceMemoryProperties(Physical, &Memory);
        vkGetPhysicalDeviceQueueFamilyProperties(Physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(Physical, &count, queues.data());
        while (Family < count && !(queues[Family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++Family;
        Require(Family < count, "no compute queue");
        const float priority = 1.0F;
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = Family;
        q.queueCount = 1;
        q.pQueuePriorities = &priority;
        VkDeviceCreateInfo d{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        d.queueCreateInfoCount = 1;
        d.pQueueCreateInfos = &q;
        Check(vkCreateDevice(Physical, &d, nullptr, &Handle));
        vkGetDeviceQueue(Handle, Family, 0, &Queue);
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex = Family;
        Check(vkCreateCommandPool(Handle, &pool, nullptr, &Pool));
        VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command.commandPool = Pool;
        command.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(Handle, &command, &Command));
    }
    ~Device() {
        if (Pool) vkDestroyCommandPool(Handle, Pool, nullptr);
        if (Handle) vkDestroyDevice(Handle, nullptr);
        if (Messenger) {
            const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(Instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy) destroy(Instance, Messenger, nullptr);
        }
        if (Instance) vkDestroyInstance(Instance, nullptr);
    }
    uint32_t Type(uint32_t bits, VkMemoryPropertyFlags flags) const {
        for (uint32_t t = 0; t < Memory.memoryTypeCount; ++t)
            if ((bits & (1U << t)) && (Memory.memoryTypes[t].propertyFlags & flags) == flags) return t;
        throw std::runtime_error("no memory type");
    }
    Buffer MakeHost(VkDeviceSize size) {
        Buffer b;
        b.Size = std::max<VkDeviceSize>(size, 16);
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = b.Size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Check(vkCreateBuffer(Handle, &info, nullptr, &b.Handle));
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(Handle, b.Handle, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex =
            Type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Check(vkAllocateMemory(Handle, &alloc, nullptr, &b.Memory));
        Check(vkBindBufferMemory(Handle, b.Handle, b.Memory, 0));
        Check(vkMapMemory(Handle, b.Memory, 0, b.Size, 0, &b.Mapped));
        return b;
    }
    void Free(Buffer& b) {
        if (!b.Handle) return;
        vkUnmapMemory(Handle, b.Memory);
        vkDestroyBuffer(Handle, b.Handle, nullptr);
        vkFreeMemory(Handle, b.Memory, nullptr);
        b = {};
    }
    Image MakeImage(VkFormat format, uint32_t width, uint32_t height) {
        Image image;
        image.Format = format, image.Width = width, image.Height = height;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Check(vkCreateImage(Handle, &info, nullptr, &image.Handle));
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(Handle, image.Handle, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = Type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(Handle, &alloc, nullptr, &image.Memory));
        Check(vkBindImageMemory(Handle, image.Handle, image.Memory, 0));
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.Handle;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(vkCreateImageView(Handle, &view, nullptr, &image.View));
        return image;
    }
    void Free(Image& image) {
        if (!image.Handle) return;
        vkDestroyImageView(Handle, image.View, nullptr);
        vkDestroyImage(Handle, image.Handle, nullptr);
        vkFreeMemory(Handle, image.Memory, nullptr);
        image = {};
    }
    void Begin() {
        Check(vkResetCommandPool(Handle, Pool, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(Command, &begin));
    }
    void Submit() {
        Check(vkEndCommandBuffer(Command));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &Command;
        Check(vkQueueSubmit(Queue, 1, &submit, VK_NULL_HANDLE));
        Check(vkQueueWaitIdle(Queue));
    }
    void MemoryBarrier(VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst,
                       VkAccessFlags dstAccess) const {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(Command, src, dst, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
};

// FP16 encodings Vulkan allows for the RGBA16F store (round to nearest even,
// or toward zero).
uint16_t ToHalf(float value, bool nearestEven) {
    uint32_t x = 0;
    std::memcpy(&x, &value, 4);
    const uint32_t sign = (x >> 16) & 0x8000U;
    const uint32_t biased = (x >> 23) & 0xFFU;
    uint32_t mantissa = x & 0x7FFFFFU;
    if (biased == 0xFFU) return static_cast<uint16_t>(sign | 0x7C00U | (mantissa != 0U ? 0x200U : 0U));
    const int32_t exponent = static_cast<int32_t>(biased) - 127 + 15;
    if (exponent >= 31) return static_cast<uint16_t>(sign | (nearestEven ? 0x7C00U : 0x7BFFU));
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa |= 0x800000U;
        const uint32_t shift = static_cast<uint32_t>(14 - exponent);
        uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1U << shift) - 1U), halfway = 1U << (shift - 1U);
        if (nearestEven && (rest > halfway || (rest == halfway && (half & 1U) != 0U))) ++half;
        return static_cast<uint16_t>(sign | half);
    }
    uint32_t half = (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1FFFU;
    if (nearestEven && (rest > 0x1000U || (rest == 0x1000U && (half & 1U) != 0U))) ++half;
    return static_cast<uint16_t>(sign | half);
}

float FromHalf(uint16_t h) {
    const uint32_t sign = (h & 0x8000U) << 16, exponent = (h >> 10) & 0x1FU, mantissa = h & 0x3FFU;
    float value = 0.0F;
    if (exponent == 0U) value = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31U) value = mantissa != 0U ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    else value = std::ldexp(static_cast<float>(mantissa | 0x400U), static_cast<int>(exponent) - 25);
    return sign != 0U ? -value : value;
}

// SPIR-V of the M2-M4 GPU twins: compiled at run time, Vulkan 1.1, no optimization.
std::vector<uint32_t> CompileLikeTheTwins(const PassKernelSource& kernel) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
    const auto spirv = compiler.CompileGlslToSpv(kernel.Source, shaderc_compute_shader, kernel.Name, options);
    Require(spirv.GetCompilationStatus() == shaderc_compilation_status_success, spirv.GetErrorMessage());
    return {spirv.cbegin(), spirv.cend()};
}

// ---------------------------------------------------------------------------
// Harness: pass instances with their output images, adapter uploads and
// readback of every stage.

struct FrameData {
    const GatherInput* Input = nullptr;
    const std::vector<double>* Motion = nullptr;   // 2 per viewport pixel
    const std::vector<double>* Previous = nullptr; // 3 per viewport pixel
    const std::vector<uint32_t>* Ids = nullptr;
    float RadianceScale = 1.0F;
};

struct Readback {
    std::vector<float> Normal, Results, Aux, Geometry, Colour, Variance, Diag2, Filtered, History, A;
    std::vector<uint16_t> J;
};

struct Instance {
    DiffuseGiPass Pass;
    Device::Image J, A;
    PassImageState JState, AState;
    PassExtent Extent;
    PassSettings Settings;
    PassInputLayout Inputs;
};

struct Job {
    Instance* Target = nullptr;
    FrameData Data;
    PassFrame Frame;
    bool Clear = false;
    bool PoisonHistory = false;
    PassOutputAccess Completion;
    PassBarrierRecord Record;
    bool Ok = false;
    Readback Out;
};

class Runner {
  public:
    explicit Runner(Device& d) : D(d) {}

    bool Configure(Instance& in, const PassExtent& extent, const PassSettings& settings, const PassInputLayout& inputs) {
        if (in.J.Width != extent.BufferWidth || in.J.Height != extent.BufferHeight) {
            Check(vkDeviceWaitIdle(D.Handle));
            D.Free(in.J);
            D.Free(in.A);
            in.J = D.MakeImage(VK_FORMAT_R16G16B16A16_SFLOAT, extent.BufferWidth, extent.BufferHeight);
            in.A = D.MakeImage(VK_FORMAT_R32_SFLOAT, extent.BufferWidth, extent.BufferHeight);
            in.JState = in.AState = {};
        }
        in.Extent = extent, in.Settings = settings, in.Inputs = inputs;
        return in.Pass.Configure(extent, settings, inputs, Outputs(in));
    }
    static PassOutputs Outputs(const Instance& in) {
        return {{in.J.Handle, in.J.View, in.J.Format, in.J.Width, in.J.Height},
                {in.A.Handle, in.A.View, in.A.Format, in.A.Width, in.A.Height}};
    }
    void Release(Instance& in) {
        Check(vkDeviceWaitIdle(D.Handle));
        in.Pass.Shutdown();
        D.Free(in.J);
        D.Free(in.A);
    }

    // One command buffer: every job's upload and frame (or neutral output),
    // then the readback of every stage.
    void Run(const std::vector<Job*>& jobs) {
        std::vector<Device::Buffer> staging;
        D.Begin();
        for (Job* job : jobs) {
            Instance& in = *job->Target;
            if (!job->Clear) staging.push_back(Upload(in, job->Data));
            if (job->PoisonHistory)
                for (const PassResourceId id : {PassResourceId::History0, PassResourceId::History1})
                    vkCmdFillBuffer(D.Command, in.Pass.Buffer(id).Buffer, 0, VK_WHOLE_SIZE, 0x7FC00000U);
            job->Ok = job->Clear ? in.Pass.ClearOutputs(D.Command, in.JState, in.AState, job->Completion, job->Record)
                                 : in.Pass.Execute(D.Command, job->Frame, in.JState, in.AState, job->Completion,
                                                   job->Record);
        }
        D.MemoryBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT);
        struct Copy {
            std::vector<float>* Floats = nullptr;
            std::vector<uint16_t>* Halves = nullptr;
            VkDeviceSize Offset = 0, Bytes = 0;
        };
        std::vector<Copy> copies;
        VkDeviceSize total = 0;
        const auto reserve = [&](VkDeviceSize bytes) {
            const VkDeviceSize at = total;
            total += (bytes + 15U) / 16U * 16U;
            return at;
        };
        // Sizes first (one readback buffer for the whole run).
        struct Planned {
            Job* Owner;
            PassResourceId Id;
            std::vector<float>* Out;
            VkDeviceSize Offset;
        };
        std::vector<Planned> buffers;
        std::vector<std::pair<Job*, std::array<VkDeviceSize, 2>>> images;
        for (Job* job : jobs) {
            Instance& in = *job->Target;
            if (!in.Pass.Available()) continue;
            const uint32_t levels = in.Settings.Filter.Levels;
            const PassResourceId filtered = levels == 0U ? PassResourceId::Colour
                                            : ((levels - 1U) % 2U == 0U ? PassResourceId::Filter0
                                                                         : PassResourceId::Filter1);
            const PassResourceId history = in.Pass.LatestHistory() == 0U ? PassResourceId::History0
                                                                        : PassResourceId::History1;
            Readback& r = job->Out;
            for (const auto& [id, out] : std::vector<std::pair<PassResourceId, std::vector<float>*>>{
                     {PassResourceId::Normal, &r.Normal}, {PassResourceId::Results, &r.Results},
                     {PassResourceId::Aux, &r.Aux}, {PassResourceId::Geometry, &r.Geometry},
                     {PassResourceId::Colour, &r.Colour}, {PassResourceId::Variance, &r.Variance},
                     {PassResourceId::Diag2, &r.Diag2}, {filtered, &r.Filtered}, {history, &r.History}})
                buffers.push_back({job, id, out, reserve(in.Pass.Buffer(id).Size)});
            const VkDeviceSize texels = static_cast<VkDeviceSize>(in.Extent.BufferWidth) * in.Extent.BufferHeight;
            images.push_back({job, {reserve(8U * texels), reserve(4U * texels)}});
        }
        Device::Buffer readback = D.MakeHost(total);
        for (const Planned& p : buffers) {
            const auto view = p.Owner->Target->Pass.Buffer(p.Id);
            const VkBufferCopy region{0, p.Offset, view.Size};
            vkCmdCopyBuffer(D.Command, view.Buffer, readback.Handle, 1, &region);
        }
        for (auto& [job, offsets] : images) {
            Instance& in = *job->Target;
            const PassImageState source{VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                        VK_ACCESS_TRANSFER_READ_BIT};
            for (int k = 0; k < 2; ++k) {
                Device::Image& image = k == 0 ? in.J : in.A;
                PassImageState& state = k == 0 ? in.JState : in.AState;
                VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                barrier.srcAccessMask = state.Access;
                barrier.dstAccessMask = source.Access;
                barrier.oldLayout = state.Layout;
                barrier.newLayout = source.Layout;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image.Handle;
                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdPipelineBarrier(D.Command, state.Stages, source.Stages, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                state = source; // the pass must transition from the caller's state
                VkBufferImageCopy region{};
                region.bufferOffset = offsets[k];
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {image.Width, image.Height, 1};
                vkCmdCopyImageToBuffer(D.Command, image.Handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.Handle,
                                       1, &region);
            }
        }
        D.MemoryBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                        VK_ACCESS_HOST_READ_BIT);
        D.Submit();
        const auto* bytes = static_cast<const unsigned char*>(readback.Mapped);
        for (const Planned& p : buffers) {
            const VkDeviceSize size = p.Owner->Target->Pass.Buffer(p.Id).Size;
            p.Out->resize(size / 4U);
            std::memcpy(p.Out->data(), bytes + p.Offset, size);
        }
        for (auto& [job, offsets] : images) {
            const Instance& in = *job->Target;
            const size_t texels = static_cast<size_t>(in.Extent.BufferWidth) * in.Extent.BufferHeight;
            job->Out.J.resize(4U * texels);
            std::memcpy(job->Out.J.data(), bytes + offsets[0], 8U * texels);
            job->Out.A.resize(texels);
            std::memcpy(job->Out.A.data(), bytes + offsets[1], 4U * texels);
        }
        D.Free(readback);
        for (auto& b : staging) D.Free(b);
    }

  private:
    // Adapter writes: host staging -> pass input buffers (transfer writes,
    // made visible by the pass's leading barrier).
    Device::Buffer Upload(Instance& in, const FrameData& f) {
        const GatherInput& input = *f.Input;
        const size_t texels = static_cast<size_t>(input.BufferWidth) * input.BufferHeight;
        const size_t pixels = static_cast<size_t>(input.Viewport.Width) * input.Viewport.Height;
        std::vector<float> depth(input.Depth.begin(), input.Depth.end());
        std::vector<uint32_t> classes(input.Class.begin(), input.Class.end());
        std::vector<float> radiance(4 * texels, 0.0F), normals(4 * texels, 0.0F), motion(2 * pixels, 0.0F);
        for (size_t i = 0; i < texels; ++i)
            for (int c = 0; c < 3; ++c) {
                radiance[4 * i + c] = input.Radiance[3 * i + c] * f.RadianceScale;
                if (!input.FaceNormals.empty()) normals[4 * i + c] = input.FaceNormals[3 * i + c];
            }
        for (size_t i = 0; i < 2 * pixels && f.Motion != nullptr; ++i) motion[i] = static_cast<float>((*f.Motion)[i]);
        std::vector<float> previous;
        std::vector<uint32_t> ids;
        if (in.Pass.Buffer(PassResourceId::PreviousPosition).Size != 0U) {
            previous.assign(4 * pixels, 0.0F);
            for (size_t i = 0; i < pixels && f.Previous != nullptr; ++i)
                for (int c = 0; c < 3; ++c) previous[4 * i + c] = static_cast<float>((*f.Previous)[3 * i + c]);
        }
        if (in.Pass.Buffer(PassResourceId::SurfaceId).Size != 0U) {
            ids.assign(pixels, 0U);
            if (f.Ids != nullptr) ids = *f.Ids;
        }
        const std::vector<std::pair<PassResourceId, std::pair<const void*, size_t>>> parts{
            {PassResourceId::Depth, {depth.data(), depth.size() * 4}},
            {PassResourceId::Class, {classes.data(), classes.size() * 4}},
            {PassResourceId::Radiance, {radiance.data(), radiance.size() * 4}},
            {PassResourceId::Normal, {normals.data(), normals.size() * 4}},
            {PassResourceId::Motion, {motion.data(), motion.size() * 4}},
            {PassResourceId::PreviousPosition, {previous.data(), previous.size() * 4}},
            {PassResourceId::SurfaceId, {ids.data(), ids.size() * 4}}};
        size_t total = 0;
        for (const auto& [id, part] : parts) total += (part.second + 15U) / 16U * 16U;
        Device::Buffer staging = D.MakeHost(total);
        // Earlier frames' reads and writes of the inputs -> these transfer writes.
        D.MemoryBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        size_t offset = 0;
        for (const auto& [id, part] : parts) {
            if (part.second == 0U) continue;
            const auto target = in.Pass.Buffer(id);
            Require(target.Size == part.second, "adapter input size differs from the pass buffer");
            std::memcpy(static_cast<unsigned char*>(staging.Mapped) + offset, part.first, part.second);
            const VkBufferCopy region{offset, 0, part.second};
            vkCmdCopyBuffer(D.Command, staging.Handle, target.Buffer, 1, &region);
            offset += (part.second + 15U) / 16U * 16U;
        }
        return staging;
    }

    Device& D;
};

PassFrame MakeFrame(const GatherInput& input, uint64_t index) {
    PassFrame f;
    f.Camera = input.Camera;
    f.NormalSource = input.NormalSource;
    f.FrameIndex = index;
    return f;
}

PassExtent ExtentOf(const GatherInput& input) {
    PassExtent e;
    e.BufferWidth = input.BufferWidth;
    e.BufferHeight = input.BufferHeight;
    e.Viewport = input.Viewport;
    return e;
}

// ---------------------------------------------------------------------------
// Stage comparisons.

struct Stat {
    double Sum = 0.0, Max = 0.0;
    size_t Count = 0, Over = 0;
    void Add(double v, double threshold) {
        Sum += v, Max = std::max(Max, v), ++Count;
        Over += v > threshold ? 1U : 0U;
    }
    [[nodiscard]] double Mean() const { return Count == 0U ? 0.0 : Sum / static_cast<double>(Count); }
};

struct Totals {
    Stat GatherJ, GatherA, TemporalJ, TemporalA, History, FilterJ, FilterA;
    size_t ValidMismatch = 0, FlagMismatch = 0, Receivers = 0, NonFinite = 0, OutputMismatch = 0, OutputTexels = 0;
    size_t ReasonMismatch = 0, Frames = 0, GatherPixels = 0;
    double ChainSum = 0.0, ChainRef = 0.0;
};

double RelJ(const float* g, const Rgb& c) {
    const double scale = std::max({1.0, std::abs(c.R), std::abs(c.G), std::abs(c.B)});
    return std::max({std::abs(g[0] - c.R), std::abs(g[1] - c.G), std::abs(g[2] - c.B)}) / scale;
}

void CompareGather(const Readback& g, const std::vector<PixelGather>& cpu, Totals& t) {
    for (size_t i = 0; i < cpu.size(); ++i) {
        const float* r = &g.Results[4 * i];
        const float* a = &g.Aux[4 * i];
        for (int k = 0; k < 4; ++k) t.NonFinite += std::isfinite(r[k]) && std::isfinite(a[k]) ? 0U : 1U;
        if ((a[1] != 0.0F) != cpu[i].Valid) ++t.ValidMismatch;
        ++t.GatherPixels;
        if (!cpu[i].Valid) continue;
        t.GatherJ.Add(RelJ(r, cpu[i].J), 1.0e-3);
        t.GatherA.Add(std::abs(r[3] - cpu[i].ARaw), 1.0e-3);
    }
}

// The temporal twin fed with the GPU's raw gather and receiver geometry.
TemporalFrame TemporalInputOf(const Readback& g, const PassFrame& frame, const PassSettings& settings, uint32_t width,
                              uint32_t height, const FrameData& data) {
    TemporalFrame f;
    f.Width = width, f.Height = height;
    const size_t count = static_cast<size_t>(width) * height;
    f.FrameIndex = frame.FrameIndex;
    f.ViewId = frame.ViewId;
    f.CameraCut = frame.CameraCut;
    f.ExplicitReset = frame.ExplicitReset;
    f.PreExposure = frame.PreExposure;
    f.TransportKey = PassTransportKey(settings, frame.NormalSource);
    f.Depth.resize(count);
    f.Normal.resize(3 * count);
    f.Raw.resize(4 * count);
    for (size_t i = 0; i < count; ++i) {
        f.Depth[i] = g.Geometry[4 * i + 3];
        for (int c = 0; c < 3; ++c) f.Normal[3 * i + c] = g.Geometry[4 * i + c];
        for (int c = 0; c < 4; ++c) f.Raw[4 * i + c] = g.Results[4 * i + c];
    }
    f.ViewX = frame.Camera.ViewX;
    f.ViewY = frame.Camera.ViewY;
    f.JitterX = frame.JitterX, f.JitterY = frame.JitterY;
    f.CurrentToPrevious = frame.CurrentToPrevious;
    f.Motion = frame.Motion;
    f.MotionVectors = *data.Motion;
    f.Validation = frame.Validation;
    if (frame.Validation == HistoryValidation::PreviousPosition) f.PreviousPosition = *data.Previous;
    if (frame.SurfaceIds) f.SurfaceId = *data.Ids;
    return f;
}

void CompareTemporal(const Readback& g, const TemporalResult& t, const std::vector<double>& history, Totals& s) {
    for (size_t i = 0; i < t.Pixels.size(); ++i) {
        const TemporalPixel& p = t.Pixels[i];
        const uint32_t flags = static_cast<uint32_t>(g.Diag2[4 * i + 3]);
        s.FlagMismatch += (((flags & kTemporalDiagAccepted) != 0U) != p.HistoryAccepted) ||
                          (((flags & kTemporalDiagReceiver) != 0U) != p.Receiver);
        for (int c = 0; c < 4; ++c) s.NonFinite += std::isfinite(g.Colour[4 * i + c]) ? 0U : 1U;
        if (!p.Receiver) continue;
        ++s.Receivers;
        s.TemporalJ.Add(RelJ(&g.Colour[4 * i], p.J), 1.0e-3);
        s.TemporalA.Add(std::abs(g.Colour[4 * i + 3] - p.A), 1.0e-3);
        double h = 0.0;
        for (uint32_t k = 0; k < kHistoryValues; ++k)
            h = std::max(h, std::abs(g.History[kHistoryValues * i + k] - history[kHistoryValues * i + k]) /
                                std::max(1.0, std::abs(history[kHistoryValues * i + k])));
        s.History.Add(h, 1.0e-3);
    }
}

// The filter twin fed with the GPU's accumulated colour, variance and geometry.
std::vector<FilteredPixel> FilterTwin(const Readback& g, const PassFrame& frame, const FilterSettings& settings,
                                      uint32_t width, uint32_t height) {
    FilterInput in;
    in.Width = width, in.Height = height;
    in.ViewX = frame.Camera.ViewX, in.ViewY = frame.Camera.ViewY;
    const size_t count = static_cast<size_t>(width) * height;
    in.Depth.resize(count), in.Normal.resize(3 * count), in.J.resize(3 * count), in.A.resize(count);
    in.VarianceJ.resize(count), in.VarianceA.resize(count);
    for (size_t i = 0; i < count; ++i) {
        in.Depth[i] = g.Geometry[4 * i + 3];
        for (int c = 0; c < 3; ++c) {
            in.Normal[3 * i + c] = g.Geometry[4 * i + c];
            in.J[3 * i + c] = g.Colour[4 * i + c];
        }
        in.A[i] = g.Colour[4 * i + 3];
        in.VarianceJ[i] = g.Variance[2 * i];
        in.VarianceA[i] = g.Variance[2 * i + 1];
    }
    return FilterImage(in, settings, Precision::Float32);
}

void CompareFilter(const Readback& g, const std::vector<FilteredPixel>& f, Totals& s) {
    for (size_t i = 0; i < f.size(); ++i) {
        if (!f[i].Receiver) continue;
        s.FilterJ.Add(RelJ(&g.Filtered[4 * i], f[i].J), 1.0e-3);
        s.FilterA.Add(std::abs(g.Filtered[4 * i + 3] - f[i].ARaw), 1.0e-3);
    }
}

// Output kernel against its FP32 twin on the GPU's own filtered colour.
void CompareOutput(const Readback& g, const PassExtent& e, Totals& s) {
    const OutputShaderParams p = MakeOutputShaderParams(e, kOutputModeFinal);
    for (uint32_t y = 0; y < e.BufferHeight; ++y)
        for (uint32_t x = 0; x < e.BufferWidth; ++x) {
            const size_t t = static_cast<size_t>(y) * e.BufferWidth + x;
            const OutputTexel o = OutputTexelF32(p, x, y, g.Filtered.data(), g.Geometry.data());
            bool same = true;
            for (int c = 0; c < 4; ++c) {
                const uint16_t h = g.J[4 * t + c];
                same = same && (h == ToHalf(o.Illumination[c], true) || h == ToHalf(o.Illumination[c], false));
                s.NonFinite += std::isfinite(FromHalf(h)) ? 0U : 1U;
            }
            same = same && g.A[t] == o.Accessibility;
            s.NonFinite += std::isfinite(g.A[t]) ? 0U : 1U;
            s.OutputMismatch += same ? 0U : 1U;
            ++s.OutputTexels;
        }
}

size_t CountDifferent(const std::vector<float>& a, const std::vector<float>& b) {
    size_t n = a.size() == b.size() ? 0U : std::max(a.size(), b.size());
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) n += std::memcmp(&a[i], &b[i], 4) != 0 ? 1U : 0U;
    return n;
}
size_t CountDifferent(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
    size_t n = a.size() == b.size() ? 0U : std::max(a.size(), b.size());
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) n += a[i] != b[i] ? 1U : 0U;
    return n;
}

struct Twin {
    double MeanJ = 0.0, MaxJ = 0.0, MeanA = 0.0;
    size_t Pixels = 0, Over = 0, BitDifferent = 0, Values = 0;
};
// Pack modules against the runtime-compiled twin kernels on the same inputs.
void CompareCompilations(const Readback& pack, const Readback& twin, Twin& s) {
    for (const auto& [a, b] : std::vector<std::pair<const std::vector<float>*, const std::vector<float>*>>{
             {&pack.Results, &twin.Results}, {&pack.Colour, &twin.Colour}, {&pack.Filtered, &twin.Filtered},
             {&pack.History, &twin.History}, {&pack.A, &twin.A}}) {
        s.BitDifferent += CountDifferent(*a, *b);
        s.Values += a->size();
    }
    s.BitDifferent += CountDifferent(pack.J, twin.J);
    s.Values += pack.J.size();
    for (size_t i = 0; i + 3 < pack.Filtered.size(); i += 4) {
        const double scale = std::max(1.0, std::abs(static_cast<double>(twin.Filtered[i + 1])));
        const double dj = std::abs(pack.Filtered[i + 1] - twin.Filtered[i + 1]) / scale;
        const double da = std::abs(pack.Filtered[i + 3] - twin.Filtered[i + 3]);
        s.MeanJ += dj, s.MeanA += da, s.MaxJ = std::max(s.MaxJ, dj);
        s.Over += (dj > 1.0e-3 || da > 1.0e-3) ? 1U : 0U;
        ++s.Pixels;
    }
}

// Moving box in the corner room (the M3 sequence twin), camera translation and
// yaw, alternating projection jitter.
SequenceScene MovingBoxScene(uint32_t width, uint32_t height) {
    const Scene base = CornerRoomScene(width, height);
    SequenceScene scene{base.Name, base.Camera, base.Primitives, std::vector<uint32_t>(base.Primitives.size(), 0U)};
    scene.Primitives.push_back(Primitive::Box({0.0, 0.0, 0.0}, {0.35, 0.4, 0.35}, {0.05, 0.05, 0.05}));
    scene.Object.push_back(1U);
    return scene;
}
SequenceFrame MovingFrame(uint64_t k) {
    SequenceFrame f;
    f.Objects.resize(2);
    f.Objects[1].Position = {-2.3 + 0.08 * static_cast<double>(k), -0.6, -5.2};
    f.Camera.Position = {0.012 * static_cast<double>(k), 0.0, -0.01 * static_cast<double>(k)};
    f.Camera.Yaw = 0.003 * static_cast<double>(k);
    f.JitterX = (k % 2 == 0) ? 0.25 : -0.25;
    f.JitterY = (k % 2 == 0) ? -0.25 : 0.25;
    return f;
}

PassSettings SceneSettings(double radius, double thickness) {
    PassSettings s = DefaultPassSettings();
    s.Gather.Radius = radius;
    s.Gather.Thickness = thickness;
    s.Gather.Threads = 4;
    s.Temporal.Threads = 4;
    s.Filter.Threads = 4;
    return s;
}

std::vector<PixelGather> GatherTwin(const GatherInput& input, const PassSettings& s, uint64_t frame) {
    GatherSettings g = s.Gather;
    g.Rotations.Frame = frame;
    const PreparedGeometry g32 = PrepareGeometryF32(input);
    if (!PassUsesHierarchy(s)) return GatherImageF32(input, g);
    const DepthHierarchy h32 = BuildDepthHierarchy(input, g32, s.Hierarchy, true);
    GatherAcceleration acc;
    acc.Hierarchy = &h32;
    return GatherImageF32(input, g, acc);
}

void PrintTotals(const char* name, const Totals& t) {
    std::printf("DGI_GPU_M5 sequence=%s frames=%zu reset_reason_mismatch=%zu nonfinite=%zu | gather vs FP32 twin: "
                "meanJ=%.2e meanA=%.2e maxJ=%.2e maxA=%.2e over1e-3=%zu/%zu valid_mismatch=%zu | temporal (GPU raw in): "
                "J mean=%.2e max=%.2e A mean=%.2e history mean=%.2e max=%.2e decisions=%zu/%zu over1e-3=%zu | "
                "filter (GPU colour in): J mean=%.2e max=%.2e A mean=%.2e over1e-3=%zu/%zu | output: %zu/%zu texels "
                "differ from the twin | chain vs CPU chain (last frame; -1: not run) mean|dJ|/meanJ=%.2e\n",
                name, t.Frames, t.ReasonMismatch, t.NonFinite, t.GatherJ.Mean(), t.GatherA.Mean(), t.GatherJ.Max,
                t.GatherA.Max, std::max(t.GatherJ.Over, t.GatherA.Over), t.GatherJ.Count, t.ValidMismatch, t.TemporalJ.Mean(),
                t.TemporalJ.Max, t.TemporalA.Mean(), t.History.Mean(), t.History.Max, t.FlagMismatch, t.Receivers,
                t.TemporalJ.Over, t.FilterJ.Mean(), t.FilterJ.Max, t.FilterA.Mean(), t.FilterJ.Over, t.FilterJ.Count,
                t.OutputMismatch, t.OutputTexels, t.ChainRef > 0.0 ? t.ChainSum / t.ChainRef : -1.0);
}

void RequireTotals(const Totals& t) {
    Require(t.NonFinite == 0, "non-finite GPU value");
    Require(t.ReasonMismatch == 0, "the pass and the FP32 accumulator disagree on a reset");
    Require(t.ValidMismatch == 0, "GPU gather validity differs from the FP32 twin");
    // M4 FP32 model of the gather: means below 5e-4, isolated decision flips
    // beyond 1e-3 in at most 2% of pixels.
    Require(t.GatherJ.Mean() < 5.0e-4 && t.GatherA.Mean() < 5.0e-4, "gather beyond the M4 FP32 model");
    Require(t.GatherJ.Over <= t.GatherJ.Count / 50 && t.GatherA.Over <= t.GatherA.Count / 50,
            "more than 2% of gather pixels flip");
    // M3 model of the temporal kernel: decisions within 0.1%, mean below 1e-4.
    Require(t.FlagMismatch <= t.Receivers / 1000, "temporal decisions differ from the FP32 twin");
    Require(t.TemporalJ.Mean() < 1.0e-4 && t.TemporalA.Mean() < 1.0e-4 && t.History.Mean() < 1.0e-4,
            "temporal beyond the M3 FP32 model");
    // Filter on identical inputs: rounding only; isolated surface-test flips.
    Require(t.FilterJ.Mean() < 1.0e-4 && t.FilterA.Mean() < 1.0e-4, "filter beyond rounding");
    Require(t.FilterJ.Over <= t.FilterJ.Count / 1000 + 1, "filter decisions differ from the FP32 twin");
    Require(t.OutputMismatch == 0, "output texels differ from the FP16/FP32 twin");
}

} // namespace

int main() try {
    Device device;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.Physical, &properties);
    std::printf("DGI_GPU_M5 device=\"%s\" driver=0x%x api=%u.%u.%u validation_layer=%d sync_validation=%d\n",
                properties.deviceName, properties.driverVersion, VK_API_VERSION_MAJOR(properties.apiVersion),
                VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion),
                device.Validation, device.Validation);
    const auto start = std::chrono::steady_clock::now();
    Runner runner(device);
    const PassDevice passDevice{device.Physical, device.Handle, VK_NULL_HANDLE};

    // --- The precompiled pack: every kernel resolves from builtin_pass.spvpack
    // without compiling (no compiler, no disk cache configured).
    Fast::Renderer::CachedPassShaderCompiler shaders;
    {
        Instance probe;
        Require(probe.Pass.Initialize(passDevice, shaders), "pass initialization: " + probe.Pass.UnavailableReason());
        std::printf("DGI_GPU_M5 pack kernels=%u builtin_hits=%llu compiler_requests=%llu available_before_configure=%d "
                    "reason=\"%s\"\n",
                    probe.Pass.Stats().KernelModules, static_cast<unsigned long long>(shaders.BuiltinArtifactHits()),
                    static_cast<unsigned long long>(shaders.Stats().Requests), probe.Pass.Available(),
                    probe.Pass.UnavailableReason().c_str());
        Require(shaders.BuiltinArtifactHits() == kPassKernelCount && shaders.Stats().Requests == 0,
                "a kernel did not come from the precompiled pack");
        runner.Release(probe);
    }

    // --- Sequence twins: FlowAndDepth (the OoT3D adapter's mode) with the
    // pack and the runtime-compiled kernels in lockstep, then PreviousPosition
    // with surface ids.
    for (const HistoryValidation mode : {HistoryValidation::FlowAndDepth, HistoryValidation::PreviousPosition}) {
        const bool flow = mode == HistoryValidation::FlowAndDepth;
        const SequenceScene scene = MovingBoxScene(97, 61);
        const PassSettings settings = SceneSettings(2.5, 0.5);
        const PassInputLayout inputs{!flow, !flow};
        Instance pack, twin;
        Require(pack.Pass.Initialize(passDevice, shaders), pack.Pass.UnavailableReason());
        Require(twin.Pass.Initialize(passDevice, CompileLikeTheTwins), twin.Pass.UnavailableReason());
        TemporalAccumulator temporalTwin(Precision::Float32), chain(Precision::Float32);
        Totals totals;
        Twin compilations;
        const uint64_t frames = flow ? 24U : 16U;
        for (uint64_t k = 0; k < frames; ++k) {
            const RenderedFrame rendered = RenderSequenceFrame(scene, MovingFrame(k), MovingFrame(k == 0 ? 0 : k - 1));
            const GatherInput& input = rendered.Input;
            if (k == 0) {
                Require(runner.Configure(pack, ExtentOf(input), settings, inputs), pack.Pass.UnavailableReason());
                if (flow) Require(runner.Configure(twin, ExtentOf(input), settings, inputs), twin.Pass.UnavailableReason());
            }
            const float scale = k >= 12 ? 2.0F : 1.0F; // pre-exposed radiance
            FrameData data{&input, &rendered.MotionIncluded, &rendered.PreviousPosition, &rendered.SurfaceId, scale};
            PassFrame frame = MakeFrame(input, k);
            frame.PreExposure = scale;
            frame.JitterX = rendered.JitterX, frame.JitterY = rendered.JitterY;
            frame.CurrentToPrevious = rendered.CurrentToPrevious;
            frame.Validation = mode;
            frame.SurfaceIds = !flow;
            frame.CameraCut = k == 16;
            frame.ExplicitReset = k == 20;
            Job a{&pack, data, frame}, b{&twin, data, frame};
            std::vector<Job*> jobs{&a};
            if (flow) jobs.push_back(&b);
            runner.Run(jobs);
            Require(a.Ok && (!flow || b.Ok), "Execute refused: " + pack.Pass.UnavailableReason());
            const uint32_t w = input.Viewport.Width, h = input.Viewport.Height;
            // Gather twin (on the pre-exposed input).
            GatherInput scaled = input;
            for (float& r : scaled.Radiance) r *= scale;
            const std::vector<PixelGather> cpu = GatherTwin(scaled, settings, k);
            CompareGather(a.Out, cpu, totals);
            // Temporal twin on the GPU raw gather.
            const TemporalFrame tf = TemporalInputOf(a.Out, frame, settings, w, h, data);
            const TemporalResult tr = temporalTwin.Process(tf, settings.Temporal);
            if (tr.ResetReasons != pack.Pass.Stats().LastResetReasons) {
                ++totals.ReasonMismatch;
                std::printf("DGI_GPU_M5 reset mismatch frame=%llu pass=0x%02x twin=0x%02x\n",
                            static_cast<unsigned long long>(k), pack.Pass.Stats().LastResetReasons, tr.ResetReasons);
            }
            CompareTemporal(a.Out, tr, temporalTwin.History(), totals);
            // Filter twin on the GPU colour; output twin on the GPU filtered colour.
            CompareFilter(a.Out, FilterTwin(a.Out, frame, settings.Filter, w, h), totals);
            CompareOutput(a.Out, pack.Extent, totals);
            // CPU chain end to end.
            {
                const PreparedGeometry g32 = PrepareGeometryF32(scaled);
                TemporalFrame f = MakeSequenceTemporalFrame(rendered, g32, cpu);
                f.FrameIndex = k;
                f.PreExposure = scale;
                f.CameraCut = frame.CameraCut;
                f.ExplicitReset = frame.ExplicitReset;
                f.TransportKey = PassTransportKey(settings, frame.NormalSource);
                f.Validation = mode;
                if (flow) f.PreviousPosition.clear(), f.SurfaceId.clear();
                const TemporalResult ct = chain.Process(f, settings.Temporal);
                const auto filtered = FilterImage(MakeFilterInput(f, ct), settings.Filter, Precision::Float32);
                if (k == frames - 1)
                    for (size_t i = 0; i < filtered.size(); ++i) {
                        if (!filtered[i].Receiver || a.Out.Geometry[4 * i + 3] <= 0.0F) continue;
                        totals.ChainSum += std::abs(FromHalf(a.Out.J[4 * i + 1]) - filtered[i].J.G);
                        totals.ChainRef += std::abs(filtered[i].J.G);
                    }
            }
            if (flow) CompareCompilations(a.Out, b.Out, compilations);
            ++totals.Frames;
        }
        PrintTotals(flow ? "corner_moving_97x61_flow_and_depth" : "corner_moving_97x61_previous_position_ids", totals);
        RequireTotals(totals);
        Require(totals.ChainRef > 0.0 && totals.ChainSum / totals.ChainRef < 1.0e-2, "GPU chain deviates from the CPU chain");
        if (flow) {
            std::printf("DGI_GPU_M5 pack_vs_runtime_compiled frames=%llu values=%zu bit_different=%zu | filtered "
                        "meanJ=%.2e maxJ=%.2e meanA=%.2e pixels over1e-3=%zu/%zu\n",
                        static_cast<unsigned long long>(frames), compilations.Values, compilations.BitDifferent,
                        compilations.MeanJ / std::max<size_t>(compilations.Pixels, 1), compilations.MaxJ,
                        compilations.MeanA / std::max<size_t>(compilations.Pixels, 1), compilations.Over,
                        compilations.Pixels);
            Require(compilations.MeanJ / std::max<size_t>(compilations.Pixels, 1) < 1.0e-4 &&
                        compilations.Over <= compilations.Pixels / 100,
                    "pack modules deviate from the runtime-compiled twins");
        }
        runner.Release(pack);
        runner.Release(twin);
    }

    // --- Textured static scene with renderer face normals: the mean cache on
    // textured planes.
    {
        const Scene scene = CheckerRoomScene(101, 67);
        GatherInput input = Rasterize(scene);
        input.NormalSource = GeometricNormalSource::RendererFaceNormal;
        const PassSettings settings = SceneSettings(2.5, 0.5);
        Instance pass;
        Require(pass.Pass.Initialize(passDevice, shaders), pass.Pass.UnavailableReason());
        Require(runner.Configure(pass, ExtentOf(input), settings, {}), pass.Pass.UnavailableReason());
        const std::vector<double> motion(2U * input.Viewport.Width * input.Viewport.Height, 0.0);
        TemporalAccumulator temporalTwin(Precision::Float32);
        Totals totals;
        for (uint64_t k = 0; k < 10; ++k) {
            FrameData data{&input, &motion};
            const PassFrame frame = MakeFrame(input, k);
            Job job{&pass, data, frame};
            runner.Run({&job});
            Require(job.Ok, pass.Pass.UnavailableReason());
            CompareGather(job.Out, GatherTwin(input, settings, k), totals);
            const TemporalFrame tf = TemporalInputOf(job.Out, frame, settings, input.Viewport.Width,
                                                     input.Viewport.Height, data);
            const TemporalResult tr = temporalTwin.Process(tf, settings.Temporal);
            totals.ReasonMismatch += tr.ResetReasons != pass.Pass.Stats().LastResetReasons ? 1U : 0U;
            CompareTemporal(job.Out, tr, temporalTwin.History(), totals);
            CompareFilter(job.Out, FilterTwin(job.Out, frame, settings.Filter, input.Viewport.Width,
                                              input.Viewport.Height),
                          totals);
            CompareOutput(job.Out, pass.Extent, totals);
            ++totals.Frames;
        }
        PrintTotals("checker_101x67_face_normals_static", totals);
        RequireTotals(totals);
        runner.Release(pass);
    }

    // --- History ownership: two views interleaved in one command buffer equal
    // their solo runs bit for bit; one instance shared by two views resets at
    // every view change and then equals a first frame.
    {
        const SequenceScene moving = MovingBoxScene(64, 40);
        const Scene pillars = PillarsScene(56, 44);
        const GatherInput still = Rasterize(pillars);
        const std::vector<double> stillMotion(2U * 56U * 44U, 0.0);
        const PassSettings corner = SceneSettings(2.5, 0.5), pillarSettings = SceneSettings(3.0, 0.5);
        std::vector<RenderedFrame> frames;
        for (uint64_t k = 0; k < 6; ++k)
            frames.push_back(RenderSequenceFrame(moving, MovingFrame(k), MovingFrame(k == 0 ? 0 : k - 1)));
        const auto frameOf = [&](const RenderedFrame& r, uint64_t k, uint32_t view) {
            PassFrame f = MakeFrame(r.Input, k);
            f.ViewId = view;
            f.JitterX = r.JitterX, f.JitterY = r.JitterY;
            f.CurrentToPrevious = r.CurrentToPrevious;
            return f;
        };
        Instance x, y;
        for (Instance* in : {&x, &y}) Require(in->Pass.Initialize(passDevice, shaders), in->Pass.UnavailableReason());
        Require(runner.Configure(x, ExtentOf(frames[0].Input), corner, {}), x.Pass.UnavailableReason());
        Require(runner.Configure(y, ExtentOf(still), pillarSettings, {}), y.Pass.UnavailableReason());
        std::vector<Readback> together[2];
        for (uint64_t k = 0; k < 6; ++k) {
            Job a{&x, {&frames[k].Input, &frames[k].MotionIncluded}, frameOf(frames[k], k, 0)};
            PassFrame pf = MakeFrame(still, k);
            pf.ViewId = 1;
            Job b{&y, {&still, &stillMotion}, pf};
            runner.Run({&a, &b});
            together[0].push_back(a.Out), together[1].push_back(b.Out);
        }
        runner.Release(x);
        runner.Release(y);
        size_t differences = 0;
        {
            Instance solo;
            Require(solo.Pass.Initialize(passDevice, shaders), solo.Pass.UnavailableReason());
            Require(runner.Configure(solo, ExtentOf(frames[0].Input), corner, {}), solo.Pass.UnavailableReason());
            for (uint64_t k = 0; k < 6; ++k) {
                Job a{&solo, {&frames[k].Input, &frames[k].MotionIncluded}, frameOf(frames[k], k, 0)};
                runner.Run({&a});
                differences += CountDifferent(a.Out.J, together[0][k].J) + CountDifferent(a.Out.A, together[0][k].A) +
                               CountDifferent(a.Out.History, together[0][k].History);
            }
            runner.Release(solo);
        }
        {
            Instance solo;
            Require(solo.Pass.Initialize(passDevice, shaders), solo.Pass.UnavailableReason());
            Require(runner.Configure(solo, ExtentOf(still), pillarSettings, {}), solo.Pass.UnavailableReason());
            for (uint64_t k = 0; k < 6; ++k) {
                PassFrame pf = MakeFrame(still, k);
                pf.ViewId = 1;
                Job b{&solo, {&still, &stillMotion}, pf};
                runner.Run({&b});
                differences += CountDifferent(b.Out.J, together[1][k].J) + CountDifferent(b.Out.A, together[1][k].A) +
                               CountDifferent(b.Out.History, together[1][k].History);
            }
            runner.Release(solo);
        }
        // Shared instance alternating views 0 and 1 over the same frames.
        size_t viewResets = 0, firstFrameDifferences = 0;
        {
            Instance shared, fresh;
            Require(shared.Pass.Initialize(passDevice, shaders), shared.Pass.UnavailableReason());
            Require(runner.Configure(shared, ExtentOf(frames[0].Input), corner, {}), shared.Pass.UnavailableReason());
            for (uint64_t k = 0; k < 6; ++k) {
                Job a{&shared, {&frames[k].Input, &frames[k].MotionIncluded}, frameOf(frames[k], k, k % 2 == 0 ? 0 : 1)};
                runner.Run({&a});
                if (k == 0) continue;
                viewResets += (shared.Pass.Stats().LastResetReasons & kResetViewChange) != 0U ? 1U : 0U;
                // A fresh instance given the same frame (its first) produces the same outputs.
                Require(fresh.Pass.Initialize(passDevice, shaders), fresh.Pass.UnavailableReason());
                Require(runner.Configure(fresh, ExtentOf(frames[k].Input), corner, {}), fresh.Pass.UnavailableReason());
                Job f{&fresh, {&frames[k].Input, &frames[k].MotionIncluded}, frameOf(frames[k], k, k % 2 == 0 ? 0 : 1)};
                runner.Run({&f});
                firstFrameDifferences += CountDifferent(a.Out.J, f.Out.J) + CountDifferent(a.Out.A, f.Out.A);
                runner.Release(fresh);
            }
            runner.Release(shared);
        }
        std::printf("DGI_GPU_M5 ownership two_views_frames=6 bit_differences_vs_solo=%zu | shared_instance view_resets=%zu/5 "
                    "bit_differences_vs_first_frame=%zu\n",
                    differences, viewResets, firstFrameDifferences);
        Require(differences == 0, "interleaved views differ from their solo runs");
        Require(viewResets == 5 && firstFrameDifferences == 0, "a view change does not reset to a first frame");
    }

    // --- Lifecycle: every reset reason through the pass API, settings without
    // reallocation, neutral outputs, resize with a poisoned history, screen
    // invalidation, VRAM, barrier records.
    {
        const Scene scene = CornerRoomScene(97, 61);
        const GatherInput input = Rasterize(scene);
        const std::vector<double> motion(2U * 97U * 61U, 0.0);
        PassSettings settings = SceneSettings(2.5, 0.5);
        Instance in;
        Require(in.Pass.Initialize(passDevice, shaders), in.Pass.UnavailableReason());
        Require(runner.Configure(in, ExtentOf(input), settings, {}), in.Pass.UnavailableReason());
        uint64_t index = 0;
        const auto run = [&](PassFrame frame, const PassOutputAccess& completion = {}) {
            frame.FrameIndex = index++;
            Job job{&in, {&input, &motion}, frame};
            job.Completion = completion;
            runner.Run({&job});
            Require(job.Ok, "Execute refused: " + in.Pass.UnavailableReason());
            return job;
        };
        const PassFrame base = MakeFrame(input, 0);
        std::vector<std::pair<std::string, uint32_t>> reasons;
        const auto note = [&](const char* what) { reasons.push_back({what, in.Pass.Stats().LastResetReasons}); };
        Job first = run(base);
        note("first");
        const PassWorkload workload = PassFrameWorkload(settings);
        const PassBarrierRecord readable = first.Record;
        run(base);
        note("steady");
        PassFrame cut = base;
        cut.CameraCut = true;
        run(cut);
        note("cut");
        in.Pass.ResetHistory();
        run(base);
        note("reset_history");
        const uint64_t reallocations = in.Pass.Stats().Reallocations;
        settings.Gather.Radius = 2.0;
        const bool radiusReplaces = in.Pass.ReplacesResources(in.Extent, settings, {}, Runner::Outputs(in));
        Require(runner.Configure(in, ExtentOf(input), settings, {}), in.Pass.UnavailableReason());
        run(base);
        note("radius");
        settings.Filter.Levels = 2;
        const bool levelsReplace = in.Pass.ReplacesResources(in.Extent, settings, {}, Runner::Outputs(in));
        Require(runner.Configure(in, ExtentOf(input), settings, {}), in.Pass.UnavailableReason());
        Job levels = run(base);
        note("filter_levels_2");
        settings.Temporal.Enabled = false;
        Require(runner.Configure(in, ExtentOf(input), settings, {}), in.Pass.UnavailableReason());
        run(base);
        note("temporal_off");
        settings.Temporal.Enabled = true;
        settings.Filter.Levels = 4;
        Require(runner.Configure(in, ExtentOf(input), settings, {}), in.Pass.UnavailableReason());
        run(base);
        note("temporal_on");
        // Write-only completion: the completion transitions are elided.
        PassOutputAccess writeOnly;
        writeOnly.Illumination = kPassOutputWriteState;
        writeOnly.Accessibility = kPassOutputWriteState;
        Job elided = run(base, writeOnly);
        note("write_only");
        // Neutral outputs, then a reset.
        Job clear{&in};
        clear.Clear = true;
        runner.Run({&clear});
        Require(clear.Ok, in.Pass.UnavailableReason());
        size_t notNeutral = 0;
        for (size_t t = 0; t < clear.Out.A.size(); ++t) {
            notNeutral += clear.Out.A[t] != 1.0F ? 1U : 0U;
            for (int c = 0; c < 4; ++c) notNeutral += clear.Out.J[4 * t + c] != 0U ? 1U : 0U;
        }
        run(base);
        note("after_clear");
        const bool sameExtentNoRealloc = in.Pass.Stats().Reallocations == reallocations;
        // Resize: new images and buffers, history poisoned with NaN before the
        // first frame at the new extent.
        const Scene smallScene = CornerRoomScene(80, 48);
        const GatherInput small = Rasterize(smallScene);
        const std::vector<double> smallMotion(2U * 80U * 48U, 0.0);
        const bool resizeReplaces = in.Pass.ReplacesResources(ExtentOf(small), settings, {}, Runner::Outputs(in));
        Require(runner.Configure(in, ExtentOf(small), settings, {}), in.Pass.UnavailableReason());
        Job resized{&in, {&small, &smallMotion}, MakeFrame(small, index++)};
        resized.PoisonHistory = true;
        runner.Run({&resized});
        Require(resized.Ok, in.Pass.UnavailableReason());
        note("resize");
        const PassResourcePlan smallPlan = PlanPassResources(ExtentOf(small), settings, {},
                                                             static_cast<uint32_t>(properties.limits.minUniformBufferOffsetAlignment));
        const uint64_t smallBytes = in.Pass.Stats().BufferBytes, smallMemory = in.Pass.Stats().MemoryBytes;
        size_t resizeDifferences = 0, resizeNonFinite = 0;
        {
            Instance fresh;
            Require(fresh.Pass.Initialize(passDevice, shaders), fresh.Pass.UnavailableReason());
            Require(runner.Configure(fresh, ExtentOf(small), settings, {}), fresh.Pass.UnavailableReason());
            Job f{&fresh, {&small, &smallMotion}, resized.Frame};
            runner.Run({&f});
            resizeDifferences = CountDifferent(f.Out.J, resized.Out.J) + CountDifferent(f.Out.A, resized.Out.A) +
                                CountDifferent(f.Out.History, resized.Out.History);
            for (const uint16_t h : resized.Out.J) resizeNonFinite += std::isfinite(FromHalf(h)) ? 0U : 1U;
            runner.Release(fresh);
        }
        // Screen-resource invalidation.
        in.Pass.InvalidateScreenResources();
        const bool unavailable = !in.Pass.Available();
        const std::string invalidReason = in.Pass.UnavailableReason();
        PassBarrierRecord refused;
        PassImageState js = in.JState, as = in.AState;
        device.Begin();
        PassFrame refusedFrame = MakeFrame(small, index);
        const bool executedWhileInvalid = in.Pass.Execute(device.Command, refusedFrame, js, as, {}, refused);
        device.Submit();
        Require(runner.Configure(in, ExtentOf(small), settings, {}), in.Pass.UnavailableReason());
        Job again{&in, {&small, &smallMotion}, MakeFrame(small, index++)};
        runner.Run({&again});
        Require(again.Ok, in.Pass.UnavailableReason());
        note("after_invalidation");

        std::string list;
        for (const auto& [what, bits] : reasons) {
            char item[64];
            std::snprintf(item, sizeof(item), "%s%s=0x%02x", list.empty() ? "" : " ", what.c_str(), bits);
            list += item;
        }
        std::printf("DGI_GPU_M5 resets %s\n", list.c_str());
        std::printf("DGI_GPU_M5 lifecycle radius_replaces=%d filter_levels_replace=%d reallocations_without_extent_change=%d "
                    "resize_replaces=%d resize_vs_fresh_bit_differences=%zu resize_nonfinite=%zu clear_not_neutral=%zu "
                    "invalidated_available=%d reason=\"%s\" execute_while_invalid=%d dispatches=%u stats: executions=%llu "
                    "clears=%llu failures=%llu reset_frames=%llu invalidations=%llu reallocations=%llu\n",
                    radiusReplaces, levelsReplace, !sameExtentNoRealloc, resizeReplaces, resizeDifferences,
                    resizeNonFinite, notNeutral, !unavailable, invalidReason.c_str(), executedWhileInvalid,
                    refused.Dispatches, static_cast<unsigned long long>(in.Pass.Stats().Executions),
                    static_cast<unsigned long long>(in.Pass.Stats().Clears),
                    static_cast<unsigned long long>(in.Pass.Stats().Failures),
                    static_cast<unsigned long long>(in.Pass.Stats().ResetFrames),
                    static_cast<unsigned long long>(in.Pass.Stats().Invalidations),
                    static_cast<unsigned long long>(in.Pass.Stats().Reallocations));
        std::printf("DGI_GPU_M5 vram extent=80x48 pass_buffers=%llu plan=%llu (inputs %llu, frame %llu, history %llu, "
                    "static %llu) device_memory=%llu outputs=%llu\n",
                    static_cast<unsigned long long>(smallBytes), static_cast<unsigned long long>(smallPlan.PassBytes()),
                    static_cast<unsigned long long>(smallPlan.Bytes(PassLifetime::Input)),
                    static_cast<unsigned long long>(smallPlan.Bytes(PassLifetime::Frame)),
                    static_cast<unsigned long long>(smallPlan.Bytes(PassLifetime::Persistent)),
                    static_cast<unsigned long long>(smallPlan.Bytes(PassLifetime::Static)),
                    static_cast<unsigned long long>(smallMemory), static_cast<unsigned long long>(smallPlan.OutputBytes()));
        std::printf("DGI_GPU_M5 barriers readable: planned=%u emitted=%u elided=%u private=%u dispatches=%u (workload %u/%u) | "
                    "write_only: emitted=%u elided=%u state_general=%d | filter_levels_2 dispatches=%u | clear: dispatches=%u "
                    "private=%u emitted=%u\n",
                    readable.PlannedOutputTransitions, readable.EmittedOutputTransitions, readable.ElidedOutputTransitions,
                    readable.PrivateBarriers, readable.Dispatches, workload.PrivateBarriers, workload.Dispatches,
                    elided.Record.EmittedOutputTransitions, elided.Record.ElidedOutputTransitions,
                    elided.Record.ElidedOutputTransitions == 2U, levels.Record.Dispatches, clear.Record.Dispatches,
                    clear.Record.PrivateBarriers, clear.Record.EmittedOutputTransitions);
        const auto bits = [&](size_t i) { return reasons[i].second; };
        Require(bits(0) == kResetFirstFrame && bits(1) == 0U && bits(2) == kResetCameraCut && bits(3) == kResetExplicit &&
                    bits(4) == kResetTransport && bits(5) == 0U && bits(6) == kResetDisabled &&
                    bits(9) == kResetExplicit && bits(10) == (kResetResize | kResetExplicit) && bits(11) == kResetExplicit,
                "reset reasons through the pass API");
        Require(!radiusReplaces && !levelsReplace && sameExtentNoRealloc && resizeReplaces, "reallocation decisions");
        Require(resizeDifferences == 0 && resizeNonFinite == 0, "a resized pass read its replaced history");
        Require(notNeutral == 0, "ClearOutputs did not write neutral outputs");
        Require(unavailable && !executedWhileInvalid && refused.Dispatches == 0, "invalidated pass still executes");
        Require(smallBytes == smallPlan.PassBytes() && smallBytes == in.Pass.Resources().PassBytes(),
                "allocated buffers differ from the declared plan");
        Require(readable.PlannedOutputTransitions == 4 && readable.EmittedOutputTransitions == 4 &&
                    readable.ElidedOutputTransitions == 0 && readable.PrivateBarriers == workload.PrivateBarriers &&
                    readable.Dispatches == workload.Dispatches,
                "barrier record of a readable completion");
        Require(elided.Record.EmittedOutputTransitions == 2 && elided.Record.ElidedOutputTransitions == 2,
                "write-only completion did not elide its transitions");
        Require(clear.Record.Dispatches == 1 && clear.Record.PrivateBarriers == 2, "barrier record of ClearOutputs");
        runner.Release(in);
    }

    // --- Padded, poisoned viewport: equal to the unpadded run inside, neutral outside.
    {
        Scene padded = CornerRoomScene(83, 59);
        padded.PadLeft = 5, padded.PadTop = 3, padded.PadRight = 9, padded.PadBottom = 7;
        const GatherInput a = Rasterize(CornerRoomScene(83, 59)), b = Rasterize(padded);
        const std::vector<double> motion(2U * 83U * 59U, 0.0);
        const PassSettings settings = SceneSettings(2.5, 0.5);
        Instance plain, shifted;
        for (Instance* in : {&plain, &shifted}) Require(in->Pass.Initialize(passDevice, shaders), in->Pass.UnavailableReason());
        Require(runner.Configure(plain, ExtentOf(a), settings, {}), plain.Pass.UnavailableReason());
        Require(runner.Configure(shifted, ExtentOf(b), settings, {}), shifted.Pass.UnavailableReason());
        size_t inside = 0, outside = 0;
        for (uint64_t k = 0; k < 4; ++k) {
            Job p{&plain, {&a, &motion}, MakeFrame(a, k)}, s{&shifted, {&b, &motion}, MakeFrame(b, k)};
            runner.Run({&p, &s});
            for (uint32_t y = 0; y < b.BufferHeight; ++y)
                for (uint32_t x = 0; x < b.BufferWidth; ++x) {
                    const size_t t = static_cast<size_t>(y) * b.BufferWidth + x;
                    const bool in = x >= 5 && y >= 3 && x < 5 + 83 && y < 3 + 59;
                    if (in) {
                        const size_t u = static_cast<size_t>(y - 3) * 83 + (x - 5);
                        inside += std::memcmp(&s.Out.J[4 * t], &p.Out.J[4 * u], 8) != 0 || s.Out.A[t] != p.Out.A[u];
                    } else {
                        outside += s.Out.A[t] != 1.0F || s.Out.J[4 * t] != 0U || s.Out.J[4 * t + 1] != 0U ||
                                   s.Out.J[4 * t + 2] != 0U || s.Out.J[4 * t + 3] != 0U;
                    }
                }
        }
        std::printf("DGI_GPU_M5 padded_viewport frames=4 extent=97x69 viewport=83x59+5+3 inside_differences=%zu "
                    "outside_not_neutral=%zu\n",
                    inside, outside);
        Require(inside == 0 && outside == 0, "a padded viewport changes the result or leaks outside");
        runner.Release(plain);
        runner.Release(shifted);
    }

    // --- T16 through the pass: odd extents, partial workgroups, extreme level
    // counts, temporal off, hierarchy off, poisoned inputs.
    {
        size_t cases = 0, nonFinite = 0;
        for (const auto& [w, h] : std::vector<std::pair<uint32_t, uint32_t>>{{1, 1}, {7, 1}, {1, 7}, {13, 11}})
            for (const uint32_t levels : {0U, 1U, 8U})
                for (const bool temporal : {true, false})
                    for (const bool hierarchy : {true, false}) {
                        GatherInput input = Rasterize(PillarsScene(w, h));
                        const size_t texels = input.Depth.size();
                        std::vector<double> motion(2U * w * h, 0.0);
                        if (texels > 4) {
                            input.Radiance[3 * (texels / 2)] = std::numeric_limits<float>::quiet_NaN();
                            input.Depth[texels / 3] = std::numeric_limits<float>::quiet_NaN();
                            motion[0] = 1.0e30;
                            motion[motion.size() - 1] = std::numeric_limits<double>::quiet_NaN();
                        }
                        PassSettings s = SceneSettings(3.0, 0.5);
                        s.Filter.Levels = levels;
                        s.Temporal.Enabled = temporal;
                        if (!hierarchy) s.Gather.HierarchyFootprint = false, s.Gather.Cache = CacheMode::Off;
                        Instance in;
                        Require(in.Pass.Initialize(passDevice, shaders), in.Pass.UnavailableReason());
                        Require(runner.Configure(in, ExtentOf(input), s, {}), in.Pass.UnavailableReason());
                        for (uint64_t k = 0; k < 3; ++k) {
                            Job job{&in, {&input, &motion}, MakeFrame(input, k + (1ULL << 40))};
                            runner.Run({&job});
                            Require(job.Ok, in.Pass.UnavailableReason());
                            for (const uint16_t v : job.Out.J) nonFinite += std::isfinite(FromHalf(v)) ? 0U : 1U;
                            for (const float v : job.Out.A) nonFinite += std::isfinite(v) ? 0U : 1U;
                            for (const float v : job.Out.Filtered) nonFinite += std::isfinite(v) ? 0U : 1U;
                        }
                        runner.Release(in);
                        ++cases;
                    }
        std::printf("DGI_GPU_M5 t16 cases=%zu (extents 1x1, 7x1, 1x7, 13x11; filter levels 0, 1, 8; temporal on/off; "
                    "hierarchy on/off; NaN radiance and depth, motion 1e30 and NaN; frame index 2^40) nonfinite=%zu\n",
                    cases, nonFinite);
        Require(nonFinite == 0, "T16: non-finite output");
    }

    // --- HDR: radiance x8 gives J x8 (power-of-two scaling is exact in FP32;
    // FP16 rounding apart), A unchanged; radiance x1e6 stays finite in FP16.
    {
        const Scene scene = CornerRoomScene(97, 61);
        const GatherInput input = Rasterize(scene);
        const std::vector<double> motion(2U * 97U * 61U, 0.0);
        const PassSettings settings = SceneSettings(2.5, 0.5);
        std::array<Readback, 3> last;
        const std::array<float, 3> scales{1.0F, 8.0F, 1.0e6F};
        for (int run = 0; run < 3; ++run) {
            Instance in;
            Require(in.Pass.Initialize(passDevice, shaders), in.Pass.UnavailableReason());
            Require(runner.Configure(in, ExtentOf(input), settings, {}), in.Pass.UnavailableReason());
            for (uint64_t k = 0; k < 6; ++k) {
                Job job{&in, {&input, &motion, nullptr, nullptr, scales[run]}, MakeFrame(input, k)};
                runner.Run({&job});
                Require(job.Ok, in.Pass.UnavailableReason());
                last[run] = job.Out;
            }
            runner.Release(in);
        }
        double maxRel = 0.0, maxJ = 0.0;
        size_t compared = 0, aDifferent = 0, nonFinite = 0, saturated = 0;
        for (size_t t = 0; t < last[0].A.size(); ++t) {
            aDifferent += last[0].A[t] != last[1].A[t] || last[0].A[t] != last[2].A[t];
            for (int c = 0; c < 3; ++c) {
                const float j1 = FromHalf(last[0].J[4 * t + c]), j8 = FromHalf(last[1].J[4 * t + c]);
                const float big = FromHalf(last[2].J[4 * t + c]);
                nonFinite += std::isfinite(big) ? 0U : 1U;
                maxJ = std::max(maxJ, static_cast<double>(big));
                saturated += big == kOutputHalfMax ? 1U : 0U;
                if (j1 > 1.0e-3F) {
                    maxRel = std::max(maxRel, std::abs(j8 / (8.0 * j1) - 1.0));
                    ++compared;
                }
            }
            // Exact in FP32 before the store: the filtered colour scales by 8.
        }
        double fp32Rel = 0.0;
        for (size_t i = 0; i < last[0].Filtered.size(); ++i)
            if (std::abs(last[0].Filtered[i]) > 1.0e-6F && i % 4 != 3)
                fp32Rel = std::max(fp32Rel, std::abs(last[1].Filtered[i] / (8.0 * last[0].Filtered[i]) - 1.0));
        std::printf("DGI_GPU_M5 hdr x8: FP32 filtered max rel=%.2e, FP16 J max rel=%.2e over %zu channels (J > 1e-3), A "
                    "differences=%zu | x1e6: nonfinite=%zu max J=%.1f saturated channels=%zu\n",
                    fp32Rel, maxRel, compared, aDifferent, nonFinite, maxJ, saturated);
        Require(fp32Rel == 0.0, "x8 radiance does not scale the FP32 result exactly");
        Require(maxRel <= 1.0 / 1024.0 && aDifferent == 0, "x8 radiance changes the FP16 proportion or A");
        Require(nonFinite == 0 && maxJ <= kOutputHalfMax, "HDR output not finite");
    }

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("DGI_GPU_M5 validation_errors=%u sync_hazards=%u loader_messages=%u first=\"%s\" seconds=%.1f\n", gErrors,
                gSyncHazards, gLoaderMessages, gFirstMessage.substr(0, 200).c_str(), seconds);
    Require(gErrors == 0, "validation errors");
    std::printf("DGI_GPU_M5 passed\n");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
