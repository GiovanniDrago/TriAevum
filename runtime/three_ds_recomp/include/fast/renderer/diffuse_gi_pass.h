#pragma once

// Diffuse screen-space GI, milestone M5 (Stage A): the runtime pass object
// (docs/TRIAEVUM_SCREEN_SPACE_GI.md, "M5"). It runs the module's GPU pipeline
// (diffuse_gi_pass_plan.h) with the precompiled kernels of builtin_pass.spvpack
// on the renderer's device: pass-owned buffers (adapter inputs, frame
// transients, per-view histories) and two caller-owned output images,
//   DiffuseIllumination  R16G16B16A16_SFLOAT  J.rgb (before the receiver albedo), receiver flag
//   accessibility        R32_SFLOAT           A clamped to [0, 1]
// both at the buffer extent. Title-neutral raw Vulkan (no NRI, PICA or title
// vocabulary); the renderer's provider maps its graph contract onto it.
//
// Lifecycle (one instance per view and composed surface: the history belongs
// to the instance):
//   Initialize   pipelines from the precompiled kernels (or a test resolver)
//   Configure    extent, settings, optional inputs, output images; replaces
//                resources only when their sizes or the outputs change
//   Execute      records one frame; adapter inputs must be written (compute
//                shader or transfer writes recorded earlier on the same queue)
//   ClearOutputs records neutral outputs (J = 0, A = 1) for a frame without GI
//                and invalidates the history (the next frame resets)
//   ResetHistory the next frame resets (explicit)
//   InvalidateScreenResources / Shutdown release resources
// Configure (when ReplacesResources), InvalidateScreenResources and Shutdown
// destroy Vulkan objects: the caller first completes every command buffer
// that recorded this pass (the renderer's recorded-work retirement boundary).
// A Configure that changes only parameters destroys nothing.

#include "fast/renderer/diffuse_gi_pass_plan.h"
#include "fast/renderer/shaderc_compiler.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Fast::Renderer::DiffuseGi {

struct PassDevice {
    VkPhysicalDevice Physical = VK_NULL_HANDLE;
    VkDevice Device = VK_NULL_HANDLE;
    VkPipelineCache PipelineCache = VK_NULL_HANDLE; // optional
};

// SPIR-V of a kernel from its exact source text (tests compare resolvers).
using PassShaderResolver = std::function<std::vector<uint32_t>(const PassKernelSource& kernel)>;

struct PassImageBinding {
    VkImage Image = VK_NULL_HANDLE;
    VkImageView View = VK_NULL_HANDLE; // storage-capable view of mip 0, layer 0
    VkFormat Format = VK_FORMAT_UNDEFINED;
    uint32_t Width = 0, Height = 0;
};

struct PassOutputs {
    PassImageBinding Illumination;  // R16G16B16A16_SFLOAT, STORAGE usage, buffer extent
    PassImageBinding Accessibility; // R32_SFLOAT, STORAGE usage, buffer extent
};

// Vulkan state of an output image. The caller owns it (its graph state
// tracker): the pass transitions from it and stores the completion state.
struct PassImageState {
    VkImageLayout Layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags Stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags Access = 0;
    bool operator==(const PassImageState&) const = default;
};

// Completion states the caller's graph decides: readable by later passes, or
// left in the dispatch state (GENERAL, compute shader write) when nothing
// reads the output, which elides the completion transition.
struct PassOutputAccess {
    PassImageState Illumination{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_ACCESS_SHADER_READ_BIT};
    PassImageState Accessibility{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_ACCESS_SHADER_READ_BIT};
};
// The dispatch state of both outputs.
inline constexpr PassImageState kPassOutputWriteState{VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                                      VK_ACCESS_SHADER_WRITE_BIT};

// Barrier execution record of one Execute or ClearOutputs.
struct PassBarrierRecord {
    uint32_t PlannedOutputTransitions = 0; // dispatch + completion per output image
    uint32_t EmittedOutputTransitions = 0;
    uint32_t ElidedOutputTransitions = 0;  // completion already equal to the dispatch state
    uint32_t PrivateBarriers = 0;          // memory barriers on pass-owned buffers
    uint32_t Dispatches = 0;
};

struct PassStats {
    uint64_t Configurations = 0;   // successful Configure calls
    uint64_t Reallocations = 0;    // Configure calls that replaced resources
    uint64_t Executions = 0;
    uint64_t Clears = 0;
    uint64_t Failures = 0;         // Execute or ClearOutputs refused (nothing recorded)
    uint64_t ResetFrames = 0;      // executed frames that reset the history
    std::array<uint64_t, 8> ResetsByReason{}; // index = bit of kReset* (first frame ... disabled)
    uint64_t HistoryResets = 0;    // ResetHistory calls
    uint64_t Invalidations = 0;    // InvalidateScreenResources calls
    uint32_t LastResetReasons = 0;
    uint64_t BufferBytes = 0;      // sum of the pass-owned VkBuffer sizes (= PassResourcePlan::PassBytes)
    uint64_t MemoryBytes = 0;      // device memory allocated for them (alignment included)
    uint32_t KernelModules = 0;    // pipelines created
};

class DiffuseGiPass final {
  public:
    DiffuseGiPass();
    ~DiffuseGiPass();
    DiffuseGiPass(const DiffuseGiPass&) = delete;
    DiffuseGiPass& operator=(const DiffuseGiPass&) = delete;

    // Pipelines from the precompiled pack, resolved like every built-in pass.
    bool Initialize(const PassDevice& device, CachedPassShaderCompiler& shaders);
    bool Initialize(const PassDevice& device, const PassShaderResolver& resolver);

    // True when Configure with these arguments would destroy or replace
    // Vulkan objects (the caller then completes its recorded work first).
    [[nodiscard]] bool ReplacesResources(const PassExtent& extent, const PassSettings& settings,
                                         const PassInputLayout& inputs, const PassOutputs& outputs) const;
    bool Configure(const PassExtent& extent, const PassSettings& settings, const PassInputLayout& inputs,
                   const PassOutputs& outputs);

    struct BufferView {
        VkBuffer Buffer = VK_NULL_HANDLE;
        VkDeviceSize Size = 0; // 0 when not allocated
    };
    // Adapter inputs (Depth ... SurfaceId) and, for diagnostics and tests, any
    // other pass buffer. Contents are valid after the commands that wrote them.
    [[nodiscard]] BufferView Buffer(PassResourceId id) const noexcept;
    // Index (0 or 1) of the history buffer the last frame wrote.
    [[nodiscard]] uint32_t LatestHistory() const noexcept;

    bool Execute(VkCommandBuffer command, const PassFrame& frame, PassImageState& illumination,
                 PassImageState& accessibility, const PassOutputAccess& completion, PassBarrierRecord& record);
    bool ClearOutputs(VkCommandBuffer command, PassImageState& illumination, PassImageState& accessibility,
                      const PassOutputAccess& completion, PassBarrierRecord& record);
    void ResetHistory() noexcept;
    void InvalidateScreenResources() noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] bool Available() const noexcept;           // initialized and configured
    [[nodiscard]] const std::string& UnavailableReason() const noexcept;
    [[nodiscard]] const PassStats& Stats() const noexcept;
    [[nodiscard]] const PassResourcePlan& Resources() const noexcept;
    [[nodiscard]] const PassParameterLayout& ParameterLayout() const noexcept;
    [[nodiscard]] const PassFramePlan& LastFrame() const noexcept; // constants of the last executed frame

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace Fast::Renderer::DiffuseGi
