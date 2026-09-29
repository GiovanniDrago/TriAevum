#include "fast/oot3d/brixelizer_scene.h"

#include <bit>

#ifdef ENABLE_OOT3D_VULKAN

#if defined(ENABLE_OOT3D_BRIXELIZER_GI) && __has_include(<FidelityFX/host/backends/vk/ffx_vk.h>) && \
    __has_include(<FidelityFX/host/ffx_brixelizer.h>)
#define OOT3D_HAS_FFX_BRIXELIZER 1
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_brixelizer.h>
#include <FidelityFX/host/ffx_brixelizergi.h>
#else
#define OOT3D_HAS_FFX_BRIXELIZER 0
#endif

#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Fast::Oot3d {

#if OOT3D_HAS_FFX_BRIXELIZER
namespace {

// Budgets of one update. Static placements beyond the creation budget are
// submitted as dynamic this frame and promoted on a later one.
constexpr uint32_t kStaticCreationsPerUpdate = 256;
constexpr uint64_t kDynamicTrianglesPerUpdate = 262144;
// A static placement unseen for this many native frames leaves the field
// (about 20 s at 30 Hz): long enough to keep what the camera turned away
// from, short enough for moved scenery not to linger.
constexpr uint64_t kEvictAfterCollections = 600;
constexpr VkDeviceSize kVertexArenaBytes = 96ULL << 20;
constexpr VkDeviceSize kIndexArenaBytes = 64ULL << 20;
constexpr uint32_t kMaxReferences = 8U << 20;
constexpr uint32_t kTriangleSwapBytes = 64U << 20;
constexpr uint32_t kMaxBricksPerBake = 1U << 13;

// First-fit range allocator with coalescing.
class RangeAllocator {
  public:
    explicit RangeAllocator(VkDeviceSize size = 0) { Reset(size); }
    void Reset(VkDeviceSize size) {
        mFree.clear();
        if (size != 0) mFree.emplace(0, size);
        mUsed = 0;
    }
    std::optional<VkDeviceSize> Allocate(VkDeviceSize size, VkDeviceSize alignment) {
        size = (size + alignment - 1) / alignment * alignment;
        for (auto range = mFree.begin(); range != mFree.end(); ++range) {
            const VkDeviceSize start = (range->first + alignment - 1) / alignment * alignment;
            const VkDeviceSize end = range->first + range->second;
            if (start + size > end) continue;
            const VkDeviceSize before = start - range->first;
            const VkDeviceSize after = end - (start + size);
            const VkDeviceSize first = range->first;
            mFree.erase(range);
            if (before != 0) mFree.emplace(first, before);
            if (after != 0) mFree.emplace(start + size, after);
            // The alignment padding belongs to the block and returns with it.
            mUsed += before + size;
            mBlocks[start] = { first, before + size };
            return start;
        }
        return std::nullopt;
    }
    void Free(VkDeviceSize offset) {
        const auto block = mBlocks.find(offset);
        if (block == mBlocks.end()) return;
        VkDeviceSize start = block->second.first;
        VkDeviceSize size = block->second.second;
        mBlocks.erase(block);
        mUsed -= size;
        auto next = mFree.lower_bound(start);
        if (next != mFree.end() && start + size == next->first) {
            size += next->second;
            next = mFree.erase(next);
        }
        if (next != mFree.begin()) {
            auto previous = std::prev(next);
            if (previous->first + previous->second == start) {
                start = previous->first;
                size += previous->second;
                mFree.erase(previous);
            }
        }
        mFree.emplace(start, size);
    }
    [[nodiscard]] VkDeviceSize Used() const noexcept { return mUsed; }

  private:
    std::map<VkDeviceSize, VkDeviceSize> mFree;
    // allocation offset -> (block start, block size)
    std::unordered_map<VkDeviceSize, std::pair<VkDeviceSize, VkDeviceSize>> mBlocks;
    VkDeviceSize mUsed = 0;
};

struct Buffer {
    VkBuffer Handle = VK_NULL_HANDLE;
    VkDeviceMemory Memory = VK_NULL_HANDLE;
    VkBufferCreateInfo Info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    void* Mapped = nullptr;
};

struct Image {
    VkImage Handle = VK_NULL_HANDLE;
    VkDeviceMemory Memory = VK_NULL_HANDLE;
    VkImageCreateInfo Info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
};

void RowMajor3x4(const Renderer3ds::ShadowMatrix& columnMajor, float (&out)[12]) {
    for (uint32_t row = 0; row < 3; ++row)
        for (uint32_t col = 0; col < 4; ++col) out[row * 4 + col] = columnMajor[col * 4 + row];
}

} // namespace
#endif

struct BrixelizerScene::Impl {
    BrixelizerSceneStats Stats;
    std::string Reason = "Brixelizer is not initialized";
    bool Initialized = false;
#if OOT3D_HAS_FFX_BRIXELIZER
    VkPhysicalDevice Physical = VK_NULL_HANDLE;
    VkDevice Device = VK_NULL_HANDLE;
    uint32_t FrameSlots = 2;
    std::vector<unsigned char> BackendScratch;
    VkDeviceContext DeviceContext{};
    FfxInterface Backend{};
    std::unique_ptr<FfxBrixelizerContext> Context;
    // About 8 MB: never on a thread stack.
    std::unique_ptr<FfxBrixelizerBakedUpdateDescription> Baked;
    bool ContextCreated = false;

    Image Atlas;
    bool AtlasReady = false;
    Buffer BrickAabbs;
    std::array<Buffer, FFX_BRIXELIZER_MAX_CASCADES> AabbTrees;
    std::array<Buffer, FFX_BRIXELIZER_MAX_CASCADES> BrickMaps;
    Buffer GpuScratch;
    Buffer Vertices;
    Buffer Indices;
    uint32_t VertexBufferIndex = 0;
    uint32_t IndexBufferIndex = 0;
    RangeAllocator VertexArena;
    RangeAllocator IndexArena;

    struct MeshResidency {
        std::shared_ptr<const Renderer3ds::PicaGiTriangleMesh> Mesh;
        VkDeviceSize VertexOffset = 0;
        VkDeviceSize IndexOffset = 0;
        uint32_t References = 0;
    };
    std::unordered_map<const Renderer3ds::PicaGiTriangleMesh*, MeshResidency> Meshes;
    struct StaticPlacement {
        FfxBrixelizerInstanceID Id = FFX_BRIXELIZER_INVALID_ID;
        const Renderer3ds::PicaGiTriangleMesh* Mesh = nullptr;
        uint64_t LastSeen = 0;
    };
    std::unordered_map<uint64_t, StaticPlacement> Static;
    std::unordered_map<uint64_t, uint64_t> Candidates; // signature -> last collection seen

    // Collection of one native frame.
    uint64_t SourceFrame = 0;
    uint64_t ExecutionKey = 0;
    bool CollectionOpen = false;
    uint64_t Collections = 0;
    std::vector<Renderer3ds::PicaGiInstance> Pending;

    uint64_t UpdateIndex = 0;
    std::deque<std::pair<uint64_t, std::function<void()>>> Retirements;

    uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(Physical, &properties);
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            if ((bits & (1U << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("Brixelizer: no suitable memory type");
    }
    Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) {
        Buffer buffer;
        buffer.Info.size = size;
        // FidelityFX copies from and into its resources (readback, clears).
        buffer.Info.usage = usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer.Info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(Device, &buffer.Info, nullptr, &buffer.Handle) != VK_SUCCESS)
            throw std::runtime_error("Brixelizer: buffer creation failed");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(Device, buffer.Handle, &requirements);
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits,
            hostVisible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(Device, &allocate, nullptr, &buffer.Memory) != VK_SUCCESS ||
            vkBindBufferMemory(Device, buffer.Handle, buffer.Memory, 0) != VK_SUCCESS)
            throw std::runtime_error("Brixelizer: buffer memory allocation failed");
        if (hostVisible && vkMapMemory(Device, buffer.Memory, 0, VK_WHOLE_SIZE, 0, &buffer.Mapped) != VK_SUCCESS)
            throw std::runtime_error("Brixelizer: buffer mapping failed");
        return buffer;
    }
    void Destroy(Buffer& buffer) {
        if (buffer.Handle != VK_NULL_HANDLE) vkDestroyBuffer(Device, buffer.Handle, nullptr);
        if (buffer.Memory != VK_NULL_HANDLE) vkFreeMemory(Device, buffer.Memory, nullptr);
        buffer = {};
    }
    void CreateAtlas() {
        const uint32_t size = FFX_BRIXELIZER_STATIC_CONFIG_SDF_ATLAS_SIZE;
        Atlas.Info.imageType = VK_IMAGE_TYPE_3D;
        Atlas.Info.format = VK_FORMAT_R8_UNORM;
        Atlas.Info.extent = { size, size, size };
        Atlas.Info.mipLevels = 1;
        Atlas.Info.arrayLayers = 1;
        Atlas.Info.samples = VK_SAMPLE_COUNT_1_BIT;
        Atlas.Info.tiling = VK_IMAGE_TILING_OPTIMAL;
        Atlas.Info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        Atlas.Info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Atlas.Info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(Device, &Atlas.Info, nullptr, &Atlas.Handle) != VK_SUCCESS)
            throw std::runtime_error("Brixelizer: SDF atlas creation failed");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(Device, Atlas.Handle, &requirements);
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(Device, &allocate, nullptr, &Atlas.Memory) != VK_SUCCESS ||
            vkBindImageMemory(Device, Atlas.Handle, Atlas.Memory, 0) != VK_SUCCESS)
            throw std::runtime_error("Brixelizer: SDF atlas memory allocation failed");
    }
    FfxResource BufferResource(const Buffer& buffer, const wchar_t* name, uint32_t stride = 4) const {
        auto description = ffxGetBufferResourceDescriptionVK(buffer.Handle, buffer.Info, FFX_RESOURCE_USAGE_UAV);
        description.stride = stride;
        return ffxGetResourceVK(reinterpret_cast<void*>(buffer.Handle), description, name,
                                FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    void Retire(std::function<void()> work) {
        // Updates of the frame slots in flight may still read the resource.
        Retirements.emplace_back(UpdateIndex + FrameSlots + 1, std::move(work));
    }
    void RunRetirements() {
        while (!Retirements.empty() && Retirements.front().first <= UpdateIndex) {
            Retirements.front().second();
            Retirements.pop_front();
        }
    }
    MeshResidency* Upload(const std::shared_ptr<const Renderer3ds::PicaGiTriangleMesh>& mesh) {
        const auto found = Meshes.find(mesh.get());
        if (found != Meshes.end()) return &found->second;
        const VkDeviceSize vertexBytes = mesh->Positions.size() * sizeof(mesh->Positions[0]);
        const VkDeviceSize indexBytes = mesh->Indices.size() * sizeof(uint32_t);
        const auto vertex = VertexArena.Allocate(vertexBytes, 64);
        if (!vertex) return nullptr;
        const auto index = IndexArena.Allocate(indexBytes, 64);
        if (!index) {
            VertexArena.Free(*vertex);
            return nullptr;
        }
        std::memcpy(static_cast<uint8_t*>(Vertices.Mapped) + *vertex, mesh->Positions.data(), vertexBytes);
        std::memcpy(static_cast<uint8_t*>(Indices.Mapped) + *index, mesh->Indices.data(), indexBytes);
        return &Meshes.emplace(mesh.get(), MeshResidency{ mesh, *vertex, *index, 0 }).first->second;
    }
    void Release(const Renderer3ds::PicaGiTriangleMesh* mesh) {
        const auto found = Meshes.find(mesh);
        if (found == Meshes.end() || --found->second.References != 0) return;
        const VkDeviceSize vertex = found->second.VertexOffset, index = found->second.IndexOffset;
        Meshes.erase(found);
        Retire([this, vertex, index] {
            VertexArena.Free(vertex);
            IndexArena.Free(index);
        });
    }
    FfxBrixelizerInstanceDescription Describe(const MeshResidency& residency,
                                              const Renderer3ds::PicaGiInstance& instance) const {
        FfxBrixelizerInstanceDescription description{};
        description.maxCascade = kCascadeCount - 1;
        const float pad = kBaseVoxelSize * 0.25F;
        for (uint32_t axis = 0; axis < 3; ++axis) {
            description.aabb.min[axis] = instance.WorldBounds.Minimum[axis] - pad;
            description.aabb.max[axis] = instance.WorldBounds.Maximum[axis] + pad;
        }
        RowMajor3x4(instance.WorldFromLocal, description.transform);
        description.indexFormat = FFX_INDEX_TYPE_UINT32;
        description.indexBuffer = IndexBufferIndex;
        description.indexBufferOffset = static_cast<uint32_t>(residency.IndexOffset);
        description.triangleCount = static_cast<uint32_t>(residency.Mesh->Indices.size() / 3);
        description.vertexBuffer = VertexBufferIndex;
        description.vertexStride = sizeof(residency.Mesh->Positions[0]);
        description.vertexBufferOffset = static_cast<uint32_t>(residency.VertexOffset);
        description.vertexCount = static_cast<uint32_t>(residency.Mesh->Positions.size());
        description.vertexFormat = FFX_SURFACE_FORMAT_R32G32B32_FLOAT;
        return description;
    }
    void DeleteStatic(uint64_t signature) {
        const auto found = Static.find(signature);
        if (found == Static.end()) return;
        (void)ffxBrixelizerDeleteInstances(Context.get(), &found->second.Id, 1);
        Release(found->second.Mesh);
        Static.erase(found);
    }
#endif
};

BrixelizerScene::BrixelizerScene() : mImpl(std::make_unique<Impl>()) {}
BrixelizerScene::~BrixelizerScene() { Shutdown(); }

bool BrixelizerScene::Initialize(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t frameSlots) {
#if !OOT3D_HAS_FFX_BRIXELIZER
    (void)physicalDevice;
    (void)device;
    (void)frameSlots;
    mImpl->Reason = "FidelityFX Brixelizer is not built into this renderer";
    return false;
#else
    if (mImpl->Initialized) return true;
    auto& impl = *mImpl;
    impl.Physical = physicalDevice;
    impl.Device = device;
    impl.FrameSlots = std::max(frameSlots, 1U);
    try {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, VK_FORMAT_R8_UNORM, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0U)
            throw std::runtime_error("R8 storage images are unavailable");
        constexpr size_t contexts = FFX_BRIXELIZER_CONTEXT_COUNT + FFX_BRIXELIZER_GI_CONTEXT_COUNT;
        impl.BackendScratch.assign(ffxGetScratchMemorySizeVK(physicalDevice, contexts), 0);
        impl.DeviceContext = { device, physicalDevice, vkGetDeviceProcAddr };
        if (ffxGetInterfaceVK(&impl.Backend, ffxGetDeviceVK(&impl.DeviceContext), impl.BackendScratch.data(),
                              impl.BackendScratch.size(), contexts) != FFX_OK)
            throw std::runtime_error("FidelityFX Vulkan backend initialization failed");

        impl.CreateAtlas();
        constexpr VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        impl.BrickAabbs = impl.CreateBuffer(FFX_BRIXELIZER_BRICK_AABBS_SIZE, storage, false);
        for (uint32_t i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; ++i) {
            impl.AabbTrees[i] = impl.CreateBuffer(FFX_BRIXELIZER_CASCADE_AABB_TREE_SIZE, storage, false);
            impl.BrickMaps[i] = impl.CreateBuffer(FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE, storage, false);
        }
        impl.Vertices = impl.CreateBuffer(kVertexArenaBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        impl.Indices = impl.CreateBuffer(kIndexArenaBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        impl.VertexArena.Reset(kVertexArenaBytes);
        impl.IndexArena.Reset(kIndexArenaBytes);

        impl.Context = std::make_unique<FfxBrixelizerContext>();
        FfxBrixelizerContextDescription description{};
        description.numCascades = kCascadeCount;
        // Readback buffers feed the (delayed) brick statistics.
        description.flags = static_cast<FfxBrixelizerContextFlags>(
            FFX_BRIXELIZER_CONTEXT_FLAG_DEBUG_CONTEXT_READBACK_BUFFERS |
            FFX_BRIXELIZER_CONTEXT_FLAG_DEBUG_CASCADE_READBACK_BUFFERS);
        float voxelSize = kBaseVoxelSize;
        for (uint32_t i = 0; i < kCascadeCount; ++i) {
            description.cascadeDescs[i].flags =
                static_cast<FfxBrixelizerCascadeFlag>(FFX_BRIXELIZER_CASCADE_STATIC | FFX_BRIXELIZER_CASCADE_DYNAMIC);
            description.cascadeDescs[i].voxelSize = voxelSize;
            voxelSize *= 2.0F;
        }
        description.backendInterface = impl.Backend;
        if (ffxBrixelizerContextCreate(&description, impl.Context.get()) != FFX_OK)
            throw std::runtime_error("Brixelizer context creation failed");
        impl.ContextCreated = true;

        std::array<FfxBrixelizerBufferDescription, 2> buffers{};
        buffers[0].buffer = impl.BufferResource(impl.Vertices, L"Brixelizer vertex arena");
        buffers[0].outIndex = &impl.VertexBufferIndex;
        buffers[1].buffer = impl.BufferResource(impl.Indices, L"Brixelizer index arena");
        buffers[1].outIndex = &impl.IndexBufferIndex;
        if (ffxBrixelizerRegisterBuffers(impl.Context.get(), buffers.data(), 2) != FFX_OK)
            throw std::runtime_error("Brixelizer geometry arena registration failed");
        impl.Initialized = true;
        impl.Reason.clear();
        return true;
    } catch (const std::exception& exception) {
        impl.Reason = exception.what();
        Shutdown();
        impl.Reason = exception.what();
        return false;
    }
#endif
}

void BrixelizerScene::Shutdown() {
#if OOT3D_HAS_FFX_BRIXELIZER
    auto& impl = *mImpl;
    if (impl.Device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(impl.Device);
    if (impl.ContextCreated) (void)ffxBrixelizerContextDestroy(impl.Context.get());
    impl.ContextCreated = false;
    impl.Context.reset();
    for (auto& retirement : impl.Retirements) retirement.second();
    impl.Retirements.clear();
    impl.Meshes.clear();
    impl.Static.clear();
    impl.Candidates.clear();
    impl.Pending.clear();
    impl.Destroy(impl.GpuScratch);
    impl.Destroy(impl.Vertices);
    impl.Destroy(impl.Indices);
    impl.Destroy(impl.BrickAabbs);
    for (auto& buffer : impl.AabbTrees) impl.Destroy(buffer);
    for (auto& buffer : impl.BrickMaps) impl.Destroy(buffer);
    if (impl.Atlas.Handle != VK_NULL_HANDLE) vkDestroyImage(impl.Device, impl.Atlas.Handle, nullptr);
    if (impl.Atlas.Memory != VK_NULL_HANDLE) vkFreeMemory(impl.Device, impl.Atlas.Memory, nullptr);
    impl.Atlas = {};
    impl.AtlasReady = false;
    impl.Initialized = false;
    impl.Device = VK_NULL_HANDLE;
    impl.Stats = {};
    impl.Reason = "Brixelizer is not initialized";
#endif
}

bool BrixelizerScene::AcceptsExecution(uint64_t sourceFrame, uint64_t executionKey) {
#if OOT3D_HAS_FFX_BRIXELIZER
    auto& impl = *mImpl;
    if (!impl.Initialized || sourceFrame == 0) return false;
    if (sourceFrame != impl.SourceFrame) {
        impl.SourceFrame = sourceFrame;
        impl.ExecutionKey = executionKey;
        impl.CollectionOpen = true;
        impl.Pending.clear();
    }
    return impl.CollectionOpen && impl.ExecutionKey == executionKey;
#else
    (void)sourceFrame;
    (void)executionKey;
    return false;
#endif
}

void BrixelizerScene::Submit(const Renderer3ds::PicaGiInstance& instance) {
#if OOT3D_HAS_FFX_BRIXELIZER
    if (mImpl->CollectionOpen && instance.Valid()) mImpl->Pending.push_back(instance);
#else
    (void)instance;
#endif
}

bool BrixelizerScene::Update(VkCommandBuffer commandBuffer, uint32_t frameSlot,
                             const std::array<float, 3>& center) {
#if !OOT3D_HAS_FFX_BRIXELIZER
    (void)commandBuffer;
    (void)frameSlot;
    (void)center;
    return false;
#else
    (void)frameSlot;
    auto& impl = *mImpl;
    if (!impl.Initialized || !impl.CollectionOpen) return false;
    // The collected native frame is consumed once; interpolated samples of
    // it reuse the field.
    impl.CollectionOpen = false;
    ++impl.Collections;
    ++impl.UpdateIndex;
    impl.RunRetirements();
    auto& stats = impl.Stats;
    stats.CollectedInstances = impl.Pending.size();
    stats.RejectedInstances = 0;
    stats.DynamicInstances = 0;
    stats.DynamicTriangles = 0;

    if (!impl.AtlasReady) {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = impl.Atlas.Handle;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        impl.AtlasReady = true;
    }

    // Placements of this native frame: static reuse, promotion or dynamic.
    std::unordered_set<uint64_t> seen;
    std::vector<FfxBrixelizerInstanceDescription> dynamic;
    uint32_t created = 0;
    const auto submitDynamic = [&](const Renderer3ds::PicaGiInstance& instance) {
        const uint64_t triangles = instance.Mesh->Indices.size() / 3;
        if (stats.DynamicTriangles + triangles > kDynamicTrianglesPerUpdate) {
            ++stats.RejectedInstances;
            return;
        }
        const bool resident = impl.Meshes.contains(instance.Mesh.get());
        auto* residency = impl.Upload(instance.Mesh);
        if (residency == nullptr) {
            ++stats.RejectedInstances;
            return;
        }
        // A transient upload is referenced once and released after the update.
        if (!resident) ++residency->References;
        auto description = impl.Describe(*residency, instance);
        description.flags = FFX_BRIXELIZER_INSTANCE_FLAG_DYNAMIC;
        dynamic.push_back(description);
        stats.DynamicTriangles += triangles;
    };
    std::vector<const Renderer3ds::PicaGiTriangleMesh*> transientMeshes;
    for (const auto& instance : impl.Pending) {
        const uint64_t signature = instance.RigidSignature;
        if (signature != 0 && !seen.insert(signature).second) continue;
        if (signature != 0) {
            const auto placed = impl.Static.find(signature);
            if (placed != impl.Static.end()) {
                placed->second.LastSeen = impl.Collections;
                continue;
            }
            // Promoted when also seen in the previous native frame.
            const auto candidate = impl.Candidates.find(signature);
            const bool stable = candidate != impl.Candidates.end() && candidate->second + 1 == impl.Collections;
            impl.Candidates[signature] = impl.Collections;
            if (stable && created < kStaticCreationsPerUpdate) {
                auto* residency = impl.Upload(instance.Mesh);
                if (residency != nullptr) {
                    Impl::StaticPlacement placement;
                    placement.Mesh = instance.Mesh.get();
                    placement.LastSeen = impl.Collections;
                    auto description = impl.Describe(*residency, instance);
                    description.flags = FFX_BRIXELIZER_INSTANCE_FLAG_NONE;
                    description.outInstanceID = &placement.Id;
                    if (ffxBrixelizerCreateInstances(impl.Context.get(), &description, 1) == FFX_OK) {
                        ++residency->References;
                        impl.Static.emplace(signature, placement);
                        impl.Candidates.erase(signature);
                        ++created;
                        ++stats.StaticInstancesCreated;
                        continue;
                    }
                    if (residency->References == 0) {
                        ++residency->References;
                        impl.Release(instance.Mesh.get());
                    }
                }
            }
        }
        const bool wasResident = impl.Meshes.contains(instance.Mesh.get());
        submitDynamic(instance);
        if (!wasResident && impl.Meshes.contains(instance.Mesh.get()))
            transientMeshes.push_back(instance.Mesh.get());
    }
    impl.Pending.clear();
    // Eviction of placements unseen for a long time, and of stale candidates.
    std::vector<uint64_t> evicted;
    for (const auto& [signature, placement] : impl.Static)
        if (placement.LastSeen + kEvictAfterCollections < impl.Collections) evicted.push_back(signature);
    for (const uint64_t signature : evicted) {
        impl.DeleteStatic(signature);
        ++stats.StaticInstancesEvicted;
    }
    std::erase_if(impl.Candidates, [&](const auto& entry) { return entry.second + 2 < impl.Collections; });

    if (!dynamic.empty() &&
        ffxBrixelizerCreateInstances(impl.Context.get(), dynamic.data(), static_cast<uint32_t>(dynamic.size())) !=
            FFX_OK) {
        impl.Reason = "Brixelizer rejected the dynamic instances";
        dynamic.clear();
    }
    stats.DynamicInstances = static_cast<uint32_t>(dynamic.size());

    FfxBrixelizerStats ffxStats{};
    FfxBrixelizerUpdateDescription update{};
    update.resources.sdfAtlas = ffxGetResourceVK(
        reinterpret_cast<void*>(impl.Atlas.Handle),
        ffxGetImageResourceDescriptionVK(impl.Atlas.Handle, impl.Atlas.Info, FFX_RESOURCE_USAGE_UAV),
        L"Brixelizer SDF atlas", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    update.resources.brickAABBs = impl.BufferResource(impl.BrickAabbs, L"Brixelizer brick AABBs");
    for (uint32_t i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; ++i) {
        update.resources.cascadeResources[i].aabbTree =
            impl.BufferResource(impl.AabbTrees[i], L"Brixelizer cascade AABB tree");
        update.resources.cascadeResources[i].brickMap =
            impl.BufferResource(impl.BrickMaps[i], L"Brixelizer cascade brick map");
    }
    update.frameIndex = static_cast<uint32_t>(impl.UpdateIndex);
    std::copy(center.begin(), center.end(), update.sdfCenter);
    update.populateDebugAABBsFlags = FFX_BRIXELIZER_POPULATE_AABBS_NONE;
    update.maxReferences = kMaxReferences;
    update.triangleSwapSize = kTriangleSwapBytes;
    update.maxBricksPerBake = kMaxBricksPerBake;
    size_t scratchBytes = 0;
    update.outScratchBufferSize = &scratchBytes;
    update.outStats = &ffxStats;
    if (!impl.Baked) impl.Baked = std::make_unique<FfxBrixelizerBakedUpdateDescription>();
    auto& baked = *impl.Baked;
    bool updated = false;
    try {
        if (ffxBrixelizerBakeUpdate(impl.Context.get(), &update, &baked) != FFX_OK)
            throw std::runtime_error("Brixelizer update baking failed");
        if (impl.GpuScratch.Handle == VK_NULL_HANDLE || impl.GpuScratch.Info.size < scratchBytes) {
            if (impl.GpuScratch.Handle != VK_NULL_HANDLE) {
                auto old = impl.GpuScratch;
                impl.Retire([&impl, old]() mutable { impl.Destroy(old); });
                impl.GpuScratch = {};
            }
            impl.GpuScratch = impl.CreateBuffer(std::max<VkDeviceSize>(scratchBytes + scratchBytes / 4, 1U << 20),
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        }
        if (ffxBrixelizerUpdate(impl.Context.get(), &baked,
                                impl.BufferResource(impl.GpuScratch, L"Brixelizer scratch"),
                                ffxGetCommandListVK(commandBuffer)) != FFX_OK)
            throw std::runtime_error("Brixelizer update failed");
        updated = true;
    } catch (const std::exception& exception) {
        impl.Reason = exception.what();
    }
    // Transient dynamic geometry is released after this update retires.
    for (const auto* mesh : transientMeshes) impl.Release(mesh);

    ++stats.Updates;
    stats.StaticInstances = static_cast<uint32_t>(impl.Static.size());
    stats.ResidentMeshes = static_cast<uint32_t>(impl.Meshes.size());
    stats.ArenaVertexBytes = impl.VertexArena.Used();
    stats.ArenaIndexBytes = impl.IndexArena.Used();
    stats.GpuScratchBytes = impl.GpuScratch.Info.size;
    stats.FreeBricks = ffxStats.contextStats.freeBricks;
    stats.StaticBricksAllocated = ffxStats.staticCascadeStats.bricksAllocated;
    stats.DynamicBricksAllocated = ffxStats.dynamicCascadeStats.bricksAllocated;
    return updated;
#endif
}

void BrixelizerScene::Reset() {
#if OOT3D_HAS_FFX_BRIXELIZER
    auto& impl = *mImpl;
    if (!impl.Initialized) return;
    std::vector<uint64_t> placements;
    placements.reserve(impl.Static.size());
    for (const auto& entry : impl.Static) placements.push_back(entry.first);
    for (const uint64_t signature : placements) impl.DeleteStatic(signature);
    impl.Candidates.clear();
    impl.Pending.clear();
    impl.CollectionOpen = false;
    impl.SourceFrame = 0;
#endif
}

bool BrixelizerScene::Available() const noexcept { return mImpl->Initialized; }
const BrixelizerSceneStats& BrixelizerScene::Stats() const noexcept { return mImpl->Stats; }
const std::string& BrixelizerScene::UnavailableReason() const noexcept { return mImpl->Reason; }

} // namespace Fast::Oot3d

#endif
