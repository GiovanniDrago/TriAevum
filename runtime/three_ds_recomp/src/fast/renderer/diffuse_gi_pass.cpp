#include "fast/renderer/diffuse_gi_pass.h"

// Vulkan execution of the M5 runtime pass. Plumbing follows the repository's
// pass objects (compute pipelines resolved from the precompiled pack through
// the pass shader compiler, explicit producer -> consumer barriers, caller-
// owned output state, Configure / Execute / invalidation lifecycle); the
// algorithm is the module's kernels unchanged.

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace Fast::Renderer::DiffuseGi {
namespace {

constexpr uint32_t kGroup = 8;
constexpr VkDeviceSize kUpdateLimit = 65536; // vkCmdUpdateBuffer

enum LayoutIndex : uint32_t { LayoutGather, LayoutPack, LayoutTemporal, LayoutFilter, LayoutOutput, LayoutCount };

// Descriptor types of each set layout, binding by binding.
constexpr VkDescriptorType kUbo = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
constexpr VkDescriptorType kDyn = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
constexpr VkDescriptorType kSsbo = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
constexpr VkDescriptorType kImg = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
// Gather family (normal, hierarchy, M4 gather): 0 gather block, 1-6 M2
// buffers, 7 M4 block (dynamic: the level), 8 cells, 9 SH, 10 guide table,
// 11 guide records, 12 noise (9-12 unused by the pass: placeholder).
constexpr std::array<VkDescriptorType, 13> kGatherTypes{kUbo,  kSsbo, kSsbo, kSsbo, kSsbo, kSsbo, kSsbo,
                                                        kDyn,  kSsbo, kSsbo, kSsbo, kSsbo, kSsbo};
constexpr std::array<VkDescriptorType, 8> kPackTypes{kUbo, kSsbo, kSsbo, kSsbo, kSsbo, kSsbo, kSsbo, kSsbo};
constexpr std::array<VkDescriptorType, 12> kTemporalTypes{kUbo,  kSsbo, kSsbo, kSsbo, kSsbo, kSsbo,
                                                          kSsbo, kSsbo, kSsbo, kSsbo, kSsbo, kSsbo};
constexpr std::array<VkDescriptorType, 6> kFilterTypes{kDyn, kSsbo, kSsbo, kSsbo, kSsbo, kSsbo};
constexpr std::array<VkDescriptorType, 5> kOutputTypes{kDyn, kSsbo, kSsbo, kImg, kImg};
constexpr uint32_t kMinStorageBuffersPerStage = 11; // gather and temporal sets

uint32_t Groups(uint32_t n) noexcept { return (n + kGroup - 1U) / kGroup; }

LayoutIndex KernelLayout(PassKernel kernel) noexcept {
    switch (kernel) {
    case PassKernel::Pack: return LayoutPack;
    case PassKernel::Temporal: return LayoutTemporal;
    case PassKernel::Filter: return LayoutFilter;
    case PassKernel::Output: return LayoutOutput;
    default: return LayoutGather;
    }
}

void Check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed (VkResult " + std::to_string(result) + ")");
}

struct OwnedBuffer {
    VkBuffer Handle = VK_NULL_HANDLE;
    VkDeviceMemory Memory = VK_NULL_HANDLE;
    VkDeviceSize Size = 0;
    VkDeviceSize MemorySize = 0;
};

} // namespace

struct DiffuseGiPass::Impl {
    VkPhysicalDevice Physical = VK_NULL_HANDLE;
    VkDevice Device = VK_NULL_HANDLE;
    VkPipelineCache Cache = VK_NULL_HANDLE;
    VkPhysicalDeviceLimits Limits{};
    VkPhysicalDeviceMemoryProperties Memory{};
    std::array<VkDescriptorSetLayout, LayoutCount> SetLayouts{};
    std::array<VkPipelineLayout, LayoutCount> PipelineLayouts{};
    std::array<VkPipeline, kPassKernelCount> Pipelines{};
    std::array<OwnedBuffer, kPassResourceCount> Buffers{};
    VkDescriptorPool Pool = VK_NULL_HANDLE;
    VkDescriptorSet SetGather = VK_NULL_HANDLE, SetPack = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 2> SetTemporal{}; // [k]: history k in, 1 - k out
    std::array<VkDescriptorSet, 3> SetFilter{};   // level 0; odd levels; even levels >= 2
    std::array<VkDescriptorSet, 3> SetOutput{};   // colour source: Colour, Filter0, Filter1
    bool Initialized = false, Configured = false;
    PassExtent Extent;
    PassSettings Settings;
    PassInputLayout Inputs;
    PassOutputs Outputs;
    PassResourcePlan Plan;
    PassParameterLayout Params;
    TemporalHistoryState History;
    uint32_t Current = 0; // history buffer written by the last frame
    PassFramePlan Frame;
    PassStats Stats;
    std::string Reason = "not initialized";

    [[nodiscard]] uint32_t Alignment() const noexcept {
        return static_cast<uint32_t>(std::max<VkDeviceSize>(Limits.minUniformBufferOffsetAlignment, 16));
    }

    void DestroyBuffer(PassResourceId id) noexcept {
        OwnedBuffer& b = Buffers[static_cast<size_t>(id)];
        if (b.Handle != VK_NULL_HANDLE) vkDestroyBuffer(Device, b.Handle, nullptr);
        if (b.Memory != VK_NULL_HANDLE) vkFreeMemory(Device, b.Memory, nullptr);
        b = {};
    }

    void CreateBuffer(PassResourceId id, VkDeviceSize size) {
        OwnedBuffer& b = Buffers[static_cast<size_t>(id)];
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     (id == PassResourceId::Parameters ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                                                       : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(vkCreateBuffer(Device, &info, nullptr, &b.Handle), "diffuse GI buffer creation");
        b.Size = size;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(Device, b.Handle, &requirements);
        uint32_t type = Memory.memoryTypeCount;
        for (const VkMemoryPropertyFlags wanted : {VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
                                                   VkMemoryPropertyFlags(0)}) {
            for (uint32_t t = 0; t < Memory.memoryTypeCount && type == Memory.memoryTypeCount; ++t)
                if ((requirements.memoryTypeBits & (1U << t)) != 0U &&
                    (Memory.memoryTypes[t].propertyFlags & wanted) == wanted)
                    type = t;
            if (type != Memory.memoryTypeCount) break;
        }
        if (type == Memory.memoryTypeCount) throw std::runtime_error("no memory type for a diffuse GI buffer");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        Check(vkAllocateMemory(Device, &allocation, nullptr, &b.Memory), "diffuse GI memory allocation");
        b.MemorySize = requirements.size;
        Check(vkBindBufferMemory(Device, b.Handle, b.Memory, 0), "diffuse GI memory binding");
    }

    void DestroySets() noexcept {
        if (Pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(Device, Pool, nullptr);
        Pool = VK_NULL_HANDLE;
        SetGather = SetPack = VK_NULL_HANDLE;
        SetTemporal = {};
        SetFilter = {};
        SetOutput = {};
    }

    void ReleaseResources() noexcept {
        DestroySets();
        for (uint32_t id = 0; id < kPassResourceCount; ++id) DestroyBuffer(static_cast<PassResourceId>(id));
        Configured = false;
        Stats.BufferBytes = Stats.MemoryBytes = 0;
    }

    void DestroyPipelines() noexcept {
        for (VkPipeline& p : Pipelines) {
            if (p != VK_NULL_HANDLE) vkDestroyPipeline(Device, p, nullptr);
            p = VK_NULL_HANDLE;
        }
        for (VkPipelineLayout& l : PipelineLayouts) {
            if (l != VK_NULL_HANDLE) vkDestroyPipelineLayout(Device, l, nullptr);
            l = VK_NULL_HANDLE;
        }
        for (VkDescriptorSetLayout& l : SetLayouts) {
            if (l != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(Device, l, nullptr);
            l = VK_NULL_HANDLE;
        }
    }

    template <size_t N>
    VkDescriptorSetLayout MakeSetLayout(const std::array<VkDescriptorType, N>& types) {
        std::array<VkDescriptorSetLayoutBinding, N> bindings{};
        for (uint32_t i = 0; i < N; ++i) bindings[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = static_cast<uint32_t>(N);
        info.pBindings = bindings.data();
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        Check(vkCreateDescriptorSetLayout(Device, &info, nullptr, &layout), "diffuse GI set layout");
        return layout;
    }

    [[nodiscard]] bool Allocated(PassResourceId id) const noexcept {
        return Buffers[static_cast<size_t>(id)].Handle != VK_NULL_HANDLE;
    }

    // Storage binding of a buffer, or the placeholder where the pass has none.
    [[nodiscard]] VkDescriptorBufferInfo Storage(PassResourceId id) const noexcept {
        const PassResourceId used = Allocated(id) ? id : PassResourceId::Placeholder;
        return {Buffers[static_cast<size_t>(used)].Handle, 0, VK_WHOLE_SIZE};
    }
    [[nodiscard]] VkDescriptorBufferInfo Block(uint32_t offset, VkDeviceSize range) const noexcept {
        return {Buffers[static_cast<size_t>(PassResourceId::Parameters)].Handle, offset, range};
    }

    void WriteSet(VkDescriptorSet set, const VkDescriptorType* types, const std::vector<VkDescriptorBufferInfo>& buffers,
                  const std::vector<VkDescriptorImageInfo>& images) {
        std::vector<VkWriteDescriptorSet> writes;
        uint32_t binding = 0;
        for (const VkDescriptorBufferInfo& info : buffers) {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = binding;
            w.descriptorCount = 1;
            w.descriptorType = types[binding];
            w.pBufferInfo = &info;
            writes.push_back(w);
            ++binding;
        }
        for (const VkDescriptorImageInfo& info : images) {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = binding;
            w.descriptorCount = 1;
            w.descriptorType = types[binding];
            w.pImageInfo = &info;
            writes.push_back(w);
            ++binding;
        }
        vkUpdateDescriptorSets(Device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    void BuildSets() {
        DestroySets();
        using enum PassResourceId;
        const bool filter = Allocated(Filter0);
        const uint32_t filterSets = filter ? 3U : 0U;
        const uint32_t outputSets = filter ? 3U : 1U;
        const uint32_t sets = 2U + 2U + filterSets + outputSets;
        const std::array<VkDescriptorPoolSize, 4> sizes{{
            {kUbo, 4},
            {kDyn, 1U + filterSets + outputSets},
            {kSsbo, 11U + 7U + 2U * 11U + 5U * filterSets + 2U * outputSets},
            {kImg, 2U * outputSets},
        }};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = sets;
        pool.poolSizeCount = static_cast<uint32_t>(sizes.size());
        pool.pPoolSizes = sizes.data();
        Check(vkCreateDescriptorPool(Device, &pool, nullptr, &Pool), "diffuse GI descriptor pool");
        const auto allocate = [&](LayoutIndex layout) {
            VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            info.descriptorPool = Pool;
            info.descriptorSetCount = 1;
            info.pSetLayouts = &SetLayouts[layout];
            VkDescriptorSet set = VK_NULL_HANDLE;
            Check(vkAllocateDescriptorSets(Device, &info, &set), "diffuse GI descriptor set");
            return set;
        };
        const VkDescriptorBufferInfo gatherBlock = Block(Params.Gather, sizeof(GatherShaderParams));
        SetGather = allocate(LayoutGather);
        WriteSet(SetGather, kGatherTypes.data(),
                 {gatherBlock, Storage(Depth), Storage(Class), Storage(Radiance), Storage(Normal), Storage(Results),
                  Storage(Aux), Block(0, sizeof(M4ShaderParams)), Storage(Cells), Storage(Placeholder),
                  Storage(Placeholder), Storage(Placeholder), Storage(Placeholder)},
                 {});
        SetPack = allocate(LayoutPack);
        WriteSet(SetPack, kPackTypes.data(),
                 {gatherBlock, Storage(Depth), Storage(Class), Storage(Radiance), Storage(Normal), Storage(Results),
                  Storage(Aux), Storage(Geometry)},
                 {});
        for (uint32_t k = 0; k < 2; ++k) {
            SetTemporal[k] = allocate(LayoutTemporal);
            WriteSet(SetTemporal[k], kTemporalTypes.data(),
                     {Block(Params.Temporal, sizeof(TemporalShaderParams)), Storage(Results), Storage(Geometry),
                      Storage(Motion), Storage(PreviousPosition), Storage(SurfaceId),
                      Storage(k == 0 ? History0 : History1), Storage(k == 0 ? History1 : History0), Storage(Colour),
                      Storage(Variance), Storage(Diag), Storage(Diag2)},
                     {});
        }
        const VkDescriptorBufferInfo filterBlock = Block(0, sizeof(FilterShaderParams));
        if (filter) {
            const std::array<std::array<PassResourceId, 4>, 3> wiring{{
                {Colour, Variance, Filter0, FilterVariance0},
                {Filter0, FilterVariance0, Filter1, FilterVariance1},
                {Filter1, FilterVariance1, Filter0, FilterVariance0},
            }};
            for (uint32_t k = 0; k < 3; ++k) {
                SetFilter[k] = allocate(LayoutFilter);
                WriteSet(SetFilter[k], kFilterTypes.data(),
                         {filterBlock, Storage(Geometry), Storage(wiring[k][0]), Storage(wiring[k][1]),
                          Storage(wiring[k][2]), Storage(wiring[k][3])},
                         {});
            }
        }
        const VkDescriptorImageInfo illumination{VK_NULL_HANDLE, Outputs.Illumination.View, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo accessibility{VK_NULL_HANDLE, Outputs.Accessibility.View, VK_IMAGE_LAYOUT_GENERAL};
        const std::array<PassResourceId, 3> sources{Colour, Filter0, Filter1};
        for (uint32_t k = 0; k < outputSets; ++k) {
            SetOutput[k] = allocate(LayoutOutput);
            WriteSet(SetOutput[k], kOutputTypes.data(),
                     {Block(0, sizeof(OutputShaderParams)), Storage(sources[k]), Storage(Geometry)},
                     {illumination, accessibility});
        }
    }

    // --- Recording helpers.
    void Barrier(VkCommandBuffer command, VkPipelineStageFlags srcStages, VkAccessFlags src,
                 VkPipelineStageFlags dstStages, VkAccessFlags dst, PassBarrierRecord& record) const {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = src;
        barrier.dstAccessMask = dst;
        vkCmdPipelineBarrier(command, srcStages, dstStages, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        ++record.PrivateBarriers;
    }
    void ComputeBarrier(VkCommandBuffer command, PassBarrierRecord& record) const {
        Barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, record);
    }
    // Producers of this frame (adapter compute or transfer writes, the previous
    // frame's history writes) -> reads; earlier reads of the parameters -> update.
    void UpdateParameters(VkCommandBuffer command, uint32_t offset, const void* data, uint32_t size,
                          PassBarrierRecord& record) const {
        Barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                    VK_ACCESS_UNIFORM_READ_BIT,
                record);
        vkCmdUpdateBuffer(command, Buffers[static_cast<size_t>(PassResourceId::Parameters)].Handle, offset, size, data);
        Barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_UNIFORM_READ_BIT, record);
    }
    void Dispatch(VkCommandBuffer command, PassKernel kernel, VkDescriptorSet set, const uint32_t* dynamicOffset,
                  uint32_t width, uint32_t height, PassBarrierRecord& record) const {
        const LayoutIndex layout = KernelLayout(kernel);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, Pipelines[static_cast<size_t>(kernel)]);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, PipelineLayouts[layout], 0, 1, &set,
                                dynamicOffset != nullptr ? 1U : 0U, dynamicOffset);
        vkCmdDispatch(command, std::max(Groups(width), 1U), std::max(Groups(height), 1U), 1);
        ++record.Dispatches;
    }
    static void Transition(VkCommandBuffer command, VkImage image, const PassImageState& from,
                           const PassImageState& to) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = from.Access;
        barrier.dstAccessMask = to.Access;
        barrier.oldLayout = from.Layout;
        barrier.newLayout = to.Layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, from.Stages != 0U ? from.Stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             to.Stages != 0U ? to.Stages : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
    }
    // Output kernel between its two graph-visible transitions.
    void RecordOutput(VkCommandBuffer command, VkDescriptorSet set, uint32_t offset, PassImageState& illumination,
                      PassImageState& accessibility, const PassOutputAccess& completion,
                      PassBarrierRecord& record) const {
        record.PlannedOutputTransitions += 4;
        Transition(command, Outputs.Illumination.Image, illumination, kPassOutputWriteState);
        Transition(command, Outputs.Accessibility.Image, accessibility, kPassOutputWriteState);
        record.EmittedOutputTransitions += 2;
        Dispatch(command, PassKernel::Output, set, &offset, Extent.BufferWidth, Extent.BufferHeight, record);
        const auto complete = [&](VkImage image, PassImageState& state, const PassImageState& target) {
            if (target == kPassOutputWriteState) {
                ++record.ElidedOutputTransitions;
            } else {
                Transition(command, image, kPassOutputWriteState, target);
                ++record.EmittedOutputTransitions;
            }
            state = target;
        };
        complete(Outputs.Illumination.Image, illumination, completion.Illumination);
        complete(Outputs.Accessibility.Image, accessibility, completion.Accessibility);
    }

    bool Fail(const std::string& reason) {
        Reason = reason;
        ++Stats.Failures;
        return false;
    }
};

DiffuseGiPass::DiffuseGiPass() : mImpl(std::make_unique<Impl>()) {}
DiffuseGiPass::~DiffuseGiPass() { Shutdown(); }

bool DiffuseGiPass::Initialize(const PassDevice& device, CachedPassShaderCompiler& shaders) {
    return Initialize(device, [&shaders](const PassKernelSource& kernel) {
        return shaders.Resolve(kernel.Source, SpirvStage::Compute, kernel.Name);
    });
}

bool DiffuseGiPass::Initialize(const PassDevice& device, const PassShaderResolver& resolver) {
    Shutdown();
    Impl& m = *mImpl;
    m.Physical = device.Physical;
    m.Device = device.Device;
    m.Cache = device.PipelineCache;
    if (m.Physical == VK_NULL_HANDLE || m.Device == VK_NULL_HANDLE) {
        m.Reason = "no Vulkan device";
        return false;
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(m.Physical, &properties);
    m.Limits = properties.limits;
    vkGetPhysicalDeviceMemoryProperties(m.Physical, &m.Memory);
    if (m.Limits.maxPerStageDescriptorStorageBuffers < kMinStorageBuffersPerStage) {
        m.Reason = "the device binds fewer than 11 storage buffers per stage";
        return false;
    }
    for (const VkFormat format : {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT}) {
        VkFormatProperties support{};
        vkGetPhysicalDeviceFormatProperties(m.Physical, format, &support);
        if ((support.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0U) {
            m.Reason = "the output formats are not storage images on this device";
            return false;
        }
    }
    try {
        m.SetLayouts[LayoutGather] = m.MakeSetLayout(kGatherTypes);
        m.SetLayouts[LayoutPack] = m.MakeSetLayout(kPackTypes);
        m.SetLayouts[LayoutTemporal] = m.MakeSetLayout(kTemporalTypes);
        m.SetLayouts[LayoutFilter] = m.MakeSetLayout(kFilterTypes);
        m.SetLayouts[LayoutOutput] = m.MakeSetLayout(kOutputTypes);
        for (uint32_t l = 0; l < LayoutCount; ++l) {
            VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            info.setLayoutCount = 1;
            info.pSetLayouts = &m.SetLayouts[l];
            Check(vkCreatePipelineLayout(m.Device, &info, nullptr, &m.PipelineLayouts[l]), "diffuse GI pipeline layout");
        }
        for (const PassKernelSource& kernel : BuildDiffuseGiPassKernels()) {
            const std::vector<uint32_t> words = resolver(kernel);
            if (words.size() < 5U || words[0] != 0x07230203U)
                throw std::runtime_error(std::string("no SPIR-V for ") + kernel.Name);
            VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            moduleInfo.codeSize = words.size() * sizeof(uint32_t);
            moduleInfo.pCode = words.data();
            VkShaderModule module = VK_NULL_HANDLE;
            Check(vkCreateShaderModule(m.Device, &moduleInfo, nullptr, &module), "diffuse GI shader module");
            VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                          module, "main", nullptr};
            info.layout = m.PipelineLayouts[KernelLayout(kernel.Kernel)];
            const VkResult result = vkCreateComputePipelines(m.Device, m.Cache, 1, &info, nullptr,
                                                             &m.Pipelines[static_cast<size_t>(kernel.Kernel)]);
            vkDestroyShaderModule(m.Device, module, nullptr);
            Check(result, kernel.Name);
            ++m.Stats.KernelModules;
        }
    } catch (const std::exception& error) {
        m.DestroyPipelines();
        m.Stats.KernelModules = 0;
        m.Reason = error.what();
        return false;
    }
    m.Initialized = true;
    m.Reason = "not configured";
    return true;
}

bool DiffuseGiPass::ReplacesResources(const PassExtent& extent, const PassSettings& settings,
                                      const PassInputLayout& inputs, const PassOutputs& outputs) const {
    const Impl& m = *mImpl;
    if (!m.Configured) return false;
    if (outputs.Illumination.View != m.Outputs.Illumination.View ||
        outputs.Accessibility.View != m.Outputs.Accessibility.View ||
        outputs.Illumination.Image != m.Outputs.Illumination.Image ||
        outputs.Accessibility.Image != m.Outputs.Accessibility.Image)
        return true;
    const PassResourcePlan plan = PlanPassResources(extent, settings, inputs, m.Alignment());
    for (uint32_t id = 0; id < kPassResourceCount; ++id)
        if (plan.Buffers[id].Bytes != m.Buffers[id].Size) return true;
    return false;
}

bool DiffuseGiPass::Configure(const PassExtent& extent, const PassSettings& settings, const PassInputLayout& inputs,
                              const PassOutputs& outputs) {
    Impl& m = *mImpl;
    if (!m.Initialized) return m.Fail("not initialized");
    if (const char* e = ValidatePassExtent(extent); *e != '\0') return m.Fail(e);
    if (const char* e = ValidatePassSettings(settings); *e != '\0') return m.Fail(e);
    const auto checkOutput = [&](const PassImageBinding& image, VkFormat format) {
        return image.Image != VK_NULL_HANDLE && image.View != VK_NULL_HANDLE && image.Format == format &&
               image.Width == extent.BufferWidth && image.Height == extent.BufferHeight;
    };
    if (!checkOutput(outputs.Illumination, VK_FORMAT_R16G16B16A16_SFLOAT) ||
        !checkOutput(outputs.Accessibility, VK_FORMAT_R32_SFLOAT))
        return m.Fail("output images must be R16G16B16A16_SFLOAT and R32_SFLOAT at the buffer extent");
    const PassResourcePlan plan = PlanPassResources(extent, settings, inputs, m.Alignment());
    for (const PassResource& r : plan.Buffers)
        if (r.Allocated && r.Id != PassResourceId::Parameters && r.Bytes > m.Limits.maxStorageBufferRange)
            return m.Fail(std::string("buffer '") + r.Name + "' exceeds the device's storage buffer range");
    if (Groups(extent.BufferWidth) > m.Limits.maxComputeWorkGroupCount[0] ||
        Groups(extent.BufferHeight) > m.Limits.maxComputeWorkGroupCount[1])
        return m.Fail("extent exceeds the device's dispatch limits");
    const PassParameterLayout params = MakePassParameterLayout(m.Alignment());
    if (params.Size > kUpdateLimit) return m.Fail("parameter blocks exceed one command-stream update");

    const bool outputsChanged = !m.Configured || outputs.Illumination.View != m.Outputs.Illumination.View ||
                                outputs.Accessibility.View != m.Outputs.Accessibility.View ||
                                outputs.Illumination.Image != m.Outputs.Illumination.Image ||
                                outputs.Accessibility.Image != m.Outputs.Accessibility.Image;
    bool replaced = false, historyReplaced = false;
    try {
        for (uint32_t index = 0; index < kPassResourceCount; ++index) {
            const PassResource& r = plan.Buffers[index];
            const auto id = static_cast<PassResourceId>(index);
            if (r.Bytes == m.Buffers[index].Size) continue;
            m.DestroyBuffer(id);
            if (r.Allocated) m.CreateBuffer(id, r.Bytes);
            replaced = true;
            historyReplaced = historyReplaced || r.Lifetime == PassLifetime::Persistent;
        }
        m.Extent = extent;
        m.Settings = settings;
        m.Inputs = inputs;
        m.Outputs = outputs;
        m.Plan = plan;
        m.Params = params;
        if (replaced || outputsChanged) m.BuildSets();
    } catch (const std::exception& error) {
        m.ReleaseResources();
        if (m.History.HasHistory()) m.History.Invalidate();
        return m.Fail(error.what());
    }
    // Replaced history buffers hold no history: the next frame resets
    // (kResetExplicit, resource invalidation), whatever the extents. Before
    // the first frame there is nothing to invalidate (kResetFirstFrame).
    if (historyReplaced && m.History.HasHistory()) m.History.Invalidate();
    m.Stats.BufferBytes = m.Stats.MemoryBytes = 0;
    for (const OwnedBuffer& b : m.Buffers) {
        m.Stats.BufferBytes += b.Size;
        m.Stats.MemoryBytes += b.MemorySize;
    }
    ++m.Stats.Configurations;
    if (replaced) ++m.Stats.Reallocations;
    m.Configured = true;
    m.Reason.clear();
    return true;
}

DiffuseGiPass::BufferView DiffuseGiPass::Buffer(PassResourceId id) const noexcept {
    if (id == PassResourceId::Count) return {};
    const OwnedBuffer& b = mImpl->Buffers[static_cast<size_t>(id)];
    return {b.Handle, b.Size};
}

uint32_t DiffuseGiPass::LatestHistory() const noexcept { return mImpl->Current; }

bool DiffuseGiPass::Execute(VkCommandBuffer command, const PassFrame& frame, PassImageState& illumination,
                            PassImageState& accessibility, const PassOutputAccess& completion,
                            PassBarrierRecord& record) {
    Impl& m = *mImpl;
    record = {};
    if (!m.Configured) return m.Fail(m.Initialized ? "not configured" : "not initialized");
    if (command == VK_NULL_HANDLE) return m.Fail("no command buffer");
    PassFramePlan plan;
    if (const char* e = PlanPassFrame(m.Extent, m.Settings, m.Inputs, m.Params, m.History, frame, plan); *e != '\0')
        return m.Fail(e);

    using enum PassKernel;
    const ViewportRect& v = m.Extent.Viewport;
    m.UpdateParameters(command, 0, plan.Parameters.data(), m.Params.Size, record);
    m.Dispatch(command, Normal, m.SetGather, &m.Params.M4[0], v.Width, v.Height, record);
    for (uint32_t l = 0; l < plan.HierarchyLevels; ++l) {
        m.ComputeBarrier(command, record);
        m.Dispatch(command, l == 0 ? HierarchyLevel0 : HierarchyReduce, m.SetGather, &m.Params.M4[l],
                   plan.LevelExtent[l][0], plan.LevelExtent[l][1], record);
    }
    m.ComputeBarrier(command, record);
    m.Dispatch(command, Gather, m.SetGather, &m.Params.M4[0], v.Width, v.Height, record);
    m.ComputeBarrier(command, record);
    m.Dispatch(command, Pack, m.SetPack, nullptr, v.Width, v.Height, record);
    m.ComputeBarrier(command, record);
    m.Dispatch(command, Temporal, m.SetTemporal[m.Current], nullptr, v.Width, v.Height, record);
    for (uint32_t l = 0; l < plan.FilterLevels; ++l) {
        m.ComputeBarrier(command, record);
        const VkDescriptorSet set = m.SetFilter[l == 0 ? 0 : (l % 2 == 1 ? 1 : 2)];
        m.Dispatch(command, Filter, set, &m.Params.Filter[l], v.Width, v.Height, record);
    }
    m.ComputeBarrier(command, record);
    const VkDescriptorSet output =
        m.SetOutput[plan.FilterLevels == 0 ? 0 : ((plan.FilterLevels - 1U) % 2U == 0U ? 1 : 2)];
    m.RecordOutput(command, output, m.Params.Output, illumination, accessibility, completion, record);

    m.History.Commit(plan.Temporal);
    m.Current = 1U - m.Current;
    ++m.Stats.Executions;
    m.Stats.LastResetReasons = plan.ResetReasons;
    if (plan.ResetReasons != 0U) ++m.Stats.ResetFrames;
    for (uint32_t bit = 0; bit < 8; ++bit)
        if ((plan.ResetReasons & (1U << bit)) != 0U) ++m.Stats.ResetsByReason[bit];
    m.Frame = std::move(plan);
    return true;
}

bool DiffuseGiPass::ClearOutputs(VkCommandBuffer command, PassImageState& illumination, PassImageState& accessibility,
                                 const PassOutputAccess& completion, PassBarrierRecord& record) {
    Impl& m = *mImpl;
    record = {};
    if (!m.Configured) return m.Fail(m.Initialized ? "not configured" : "not initialized");
    if (command == VK_NULL_HANDLE) return m.Fail("no command buffer");
    const OutputShaderParams neutral = MakeOutputShaderParams(m.Extent, kOutputModeNeutral);
    m.UpdateParameters(command, m.Params.OutputNeutral, &neutral, sizeof(neutral), record);
    m.RecordOutput(command, m.SetOutput[0], m.Params.OutputNeutral, illumination, accessibility, completion, record);
    // A frame without GI breaks the one-frame motion chain: the next frame resets.
    m.History.Invalidate();
    ++m.Stats.Clears;
    return true;
}

void DiffuseGiPass::ResetHistory() noexcept {
    mImpl->History.Invalidate();
    ++mImpl->Stats.HistoryResets;
}

void DiffuseGiPass::InvalidateScreenResources() noexcept {
    Impl& m = *mImpl;
    m.ReleaseResources();
    m.History.Invalidate();
    ++m.Stats.Invalidations;
    if (m.Initialized) m.Reason = "screen resources invalidated";
}

void DiffuseGiPass::Shutdown() noexcept {
    Impl& m = *mImpl;
    if (m.Device != VK_NULL_HANDLE) {
        m.ReleaseResources();
        m.DestroyPipelines();
    }
    m.Initialized = false;
    m.Configured = false;
    m.Stats.KernelModules = 0;
    m.Reason = "not initialized";
}

bool DiffuseGiPass::Available() const noexcept { return mImpl->Initialized && mImpl->Configured; }
const std::string& DiffuseGiPass::UnavailableReason() const noexcept { return mImpl->Reason; }
const PassStats& DiffuseGiPass::Stats() const noexcept { return mImpl->Stats; }
const PassResourcePlan& DiffuseGiPass::Resources() const noexcept { return mImpl->Plan; }
const PassParameterLayout& DiffuseGiPass::ParameterLayout() const noexcept { return mImpl->Params; }
const PassFramePlan& DiffuseGiPass::LastFrame() const noexcept { return mImpl->Frame; }

} // namespace Fast::Renderer::DiffuseGi
