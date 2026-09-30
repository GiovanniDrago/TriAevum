#include "fast/oot3d/launchpad_albedo_pass.h"
#ifdef ENABLE_OOT3D_VULKAN
#include "fast/oot3d/nri_interop_context.h"
#include "fast/oot3d/resource_state_tracker.h"
#ifdef ENABLE_OOT3D_IMMERSE
#include "fast/immerse/launchpad_albedo_shader.h"
#endif
#if defined(ENABLE_OOT3D_NRI) && defined(ENABLE_OOT3D_IMMERSE)
#include "nri_interop_internal.h"
#endif
#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace Fast::Oot3d {
#if defined(ENABLE_OOT3D_NRI) && defined(ENABLE_OOT3D_IMMERSE)
namespace {
enum : size_t { kInitProgram, kDownsampleProgram, kFuseProgram, kMainProgram, kProgramCount };
enum : uint32_t { kSceneBinding, kSourceBinding, kDestinationBinding, kSamplerBinding, kLevelsBinding, kRangeCount };
constexpr uint32_t kLevelSlots = Immerse::kLaunchpadAlbedoMaxLevels;

// One dispatch: its program, set, source, destination and extents.
struct AlbedoStep {
    size_t Program = kInitProgram;
    nri::DescriptorSet* Set = nullptr;
    VkImage Destination = VK_NULL_HANDLE;
    uint32_t Width = 0, Height = 0, SourceWidth = 0, SourceHeight = 0;
    bool Horizontal = false;
};
} // namespace
#endif

struct LaunchpadAlbedoPass::Impl {
    VkDevice Device = VK_NULL_HANDLE;
    NriInteropContext* Interop = nullptr;
#if defined(ENABLE_OOT3D_NRI) && defined(ENABLE_OOT3D_IMMERSE)
    nri::CoreInterface* Core = nullptr;
    nri::PipelineLayout* Layout = nullptr;
    std::array<nri::Pipeline*, kProgramCount> Pipelines{};
    nri::Descriptor* Sampler = nullptr;
    nri::DescriptorPool* Pool = nullptr;
    std::vector<nri::DescriptorSet*> Sets;
    std::vector<AlbedoStep> Steps;
#endif
    // Pyramid levels (RG16F, level 0 at half resolution), the horizontal
    // halves of each step, the fusion and the full-resolution R16F estimate.
    std::vector<NriOwnedTexture2D> Levels;
    std::vector<NriOwnedTexture2D> Halves;
    NriOwnedTexture2D Fused;
    NriOwnedTexture2D Output;
    uint32_t LowestLevel = 0;
    std::array<std::array<uint32_t, 2>, 10> LevelExtents{};
    VkImage Scene = VK_NULL_HANDLE;
    VkFormat SceneFormat = VK_FORMAT_UNDEFINED;
    uint32_t Width = 0, Height = 0;
    bool Ready = false;
    ResourceStateTracker States;
    EffectPassBarrierExecution LastBarriers;
#if defined(ENABLE_OOT3D_NRI) && defined(ENABLE_OOT3D_IMMERSE)
    std::string Reason = "iMMERSE LAUNCHPAD albedo is not initialized";
#else
    std::string Reason = "iMMERSE LAUNCHPAD albedo was not compiled into this build";
#endif
};

LaunchpadAlbedoPass::LaunchpadAlbedoPass() : mImpl(std::make_unique<Impl>()) {
}
LaunchpadAlbedoPass::~LaunchpadAlbedoPass() {
    Shutdown();
}

bool LaunchpadAlbedoPass::Initialize(VkPhysicalDevice physical, VkDevice device, NriInteropContext& interop) {
    Shutdown();
#if !defined(ENABLE_OOT3D_NRI) || !defined(ENABLE_OOT3D_IMMERSE)
    (void)physical;
    (void)device;
    (void)interop;
    return false;
#else
    mImpl->Device = device;
    mImpl->Interop = &interop;
    if (!interop.Available()) {
        mImpl->Reason = interop.UnavailableReason();
        return false;
    }
    try {
        constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        for (const VkFormat format : { VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16_SFLOAT }) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
            if ((properties.optimalTilingFeatures & required) != required)
                throw std::runtime_error("LAUNCHPAD albedo requires filterable storage RG16F and R16F");
        }
        auto* nriDevice = NriInteropAccess::Device(interop);
        mImpl->Core = NriInteropAccess::Core(interop);
        if (nriDevice == nullptr || mImpl->Core == nullptr)
            throw std::runtime_error("LAUNCHPAD albedo NRI device unavailable");
        const std::array<nri::DescriptorRangeDesc, kRangeCount> ranges{
            { { 0, 1, nri::DescriptorType::TEXTURE, nri::StageBits::COMPUTE_SHADER },
              { 2, 1, nri::DescriptorType::TEXTURE, nri::StageBits::COMPUTE_SHADER },
              { 3, 1, nri::DescriptorType::STORAGE_TEXTURE, nri::StageBits::COMPUTE_SHADER },
              { 4, 1, nri::DescriptorType::SAMPLER, nri::StageBits::COMPUTE_SHADER },
              { 5, kLevelSlots, nri::DescriptorType::TEXTURE, nri::StageBits::COMPUTE_SHADER,
                nri::DescriptorRangeBits::ARRAY } }
        };
        nri::DescriptorSetDesc set{};
        set.ranges = ranges.data();
        set.rangeNum = uint32_t(ranges.size());
        nri::RootConstantDesc root{};
        root.size = sizeof(Immerse::LaunchpadAlbedoConstants);
        root.shaderStages = nri::StageBits::COMPUTE_SHADER;
        nri::PipelineLayoutDesc layout{};
        layout.descriptorSets = &set;
        layout.descriptorSetNum = 1;
        layout.rootConstants = &root;
        layout.rootConstantNum = 1;
        layout.shaderStages = nri::StageBits::COMPUTE_SHADER;
        layout.flags = nri::PipelineLayoutBits::IGNORE_GLOBAL_SPIRV_OFFSETS;
        if (mImpl->Core->CreatePipelineLayout(*nriDevice, layout, mImpl->Layout) != nri::Result::SUCCESS)
            throw std::runtime_error("LAUNCHPAD albedo pipeline layout failed");
        const std::array<std::string, kProgramCount> sources{
            Immerse::BuildLaunchpadAlbedoInitShader(), Immerse::BuildLaunchpadAlbedoDownsampleShader(),
            Immerse::BuildLaunchpadAlbedoFuseShader(), Immerse::BuildLaunchpadAlbedoMainShader() };
        constexpr std::array<const char*, kProgramCount> names{
            "immerse_launchpad_albedo_init.comp", "immerse_launchpad_albedo_downsample.comp",
            "immerse_launchpad_albedo_fuse.comp", "immerse_launchpad_albedo_main.comp" };
        for (size_t i = 0; i < sources.size(); ++i) {
            const auto spirv = interop.Shaders().Resolve(sources[i], Renderer::SpirvStage::Compute, names[i]);
            nri::ComputePipelineDesc pipeline{};
            pipeline.pipelineLayout = mImpl->Layout;
            pipeline.shader.stage = nri::StageBits::COMPUTE_SHADER;
            pipeline.shader.bytecode = spirv.data();
            pipeline.shader.size = spirv.size() * sizeof(uint32_t);
            if (mImpl->Core->CreateComputePipeline(*nriDevice, pipeline, mImpl->Pipelines[i]) != nri::Result::SUCCESS)
                throw std::runtime_error("LAUNCHPAD albedo pipeline failed");
        }
        nri::SamplerDesc sampler{};
        sampler.filters = { nri::Filter::LINEAR, nri::Filter::LINEAR, nri::Filter::NEAREST };
        sampler.addressModes = { nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE,
                                 nri::AddressMode::CLAMP_TO_EDGE };
        if (mImpl->Core->CreateSampler(*nriDevice, sampler, mImpl->Sampler) != nri::Result::SUCCESS)
            throw std::runtime_error("LAUNCHPAD albedo sampler failed");
        mImpl->Ready = true;
        mImpl->Reason.clear();
        return true;
    } catch (const std::exception& error) {
        mImpl->Reason = error.what();
        return false;
    }
#endif
}

bool LaunchpadAlbedoPass::Configure(uint32_t width, uint32_t height, VkImage sceneColor, VkFormat sceneFormat,
                                    uint32_t sceneWidth, uint32_t sceneHeight) {
    if (!Available() || width < 2 || height < 2 || sceneColor == VK_NULL_HANDLE || sceneWidth == 0 || sceneHeight == 0)
        return false;
    if (mImpl->Width == width && mImpl->Height == height && mImpl->Scene == sceneColor &&
        mImpl->SceneFormat == sceneFormat)
        return true;
#if !defined(ENABLE_OOT3D_NRI) || !defined(ENABLE_OOT3D_IMMERSE)
    (void)sceneFormat;
    (void)sceneWidth;
    (void)sceneHeight;
    return false;
#else
    if (mImpl->Width != 0 && !mImpl->Interop->CompleteRecordedWork())
        return false;
    InvalidateScreenResources();
    try {
        // Level k of the original is (W/2 >> k) x (H/2 >> k).
        const uint32_t halfWidth = width / 2U, halfHeight = height / 2U;
        mImpl->LowestLevel = Immerse::LaunchpadAlbedoLowestLevel(width, height);
        const auto extent = [](uint32_t size, uint32_t shift) { return std::max(1U, size >> shift); };
        mImpl->Levels.resize(mImpl->LowestLevel + 1U);
        mImpl->Halves.resize(mImpl->LowestLevel + 1U);
        for (uint32_t k = 0; k <= mImpl->LowestLevel; ++k) {
            mImpl->LevelExtents[k] = { extent(halfWidth, k), extent(halfHeight, k) };
            if (!mImpl->Interop->CreateOwnedTexture2D(mImpl->LevelExtents[k][0], mImpl->LevelExtents[k][1],
                                                      VK_FORMAT_R16G16_SFLOAT, mImpl->Levels[k]) ||
                (k != 0 && !mImpl->Interop->CreateOwnedTexture2D(extent(halfWidth, k), extent(halfHeight, k - 1U),
                                                                 VK_FORMAT_R16G16_SFLOAT, mImpl->Halves[k])))
                throw std::runtime_error("LAUNCHPAD albedo pyramid allocation failed");
        }
        if (!mImpl->Interop->CreateOwnedTexture2D(halfWidth, halfHeight, VK_FORMAT_R16G16_SFLOAT, mImpl->Fused) ||
            !mImpl->Interop->CreateOwnedTexture2D(width, height, VK_FORMAT_R16_SFLOAT, mImpl->Output))
            throw std::runtime_error("LAUNCHPAD albedo allocation failed");
        if (!mImpl->Interop->WrapTexture(sceneColor, sceneFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_USAGE_SAMPLED_BIT,
                                         sceneWidth, sceneHeight))
            throw std::runtime_error("LAUNCHPAD albedo scene colour wrapping failed");
        const auto view = [&](VkImage image, bool storage) {
            auto* descriptor = NriInteropAccess::TextureView(*mImpl->Interop, image, storage);
            if (!descriptor)
                throw std::runtime_error("LAUNCHPAD albedo view unavailable");
            return descriptor;
        };
        auto* scene = view(sceneColor, false);
        auto* level0 = view(mImpl->Levels[0].Image, false);
        std::array<nri::Descriptor*, kLevelSlots> levels{};
        for (uint32_t k = 0; k < kLevelSlots; ++k)
            levels[k] = k <= mImpl->LowestLevel ? view(mImpl->Levels[k].Image, false) : level0;
        // Steps in execution order; unused bindings hold level 0.
        struct Planned {
            size_t Program;
            nri::Descriptor* Source;
            VkImage Destination;
            uint32_t Width, Height, SourceWidth, SourceHeight;
            bool Horizontal;
        };
        const auto& levelExtents = mImpl->LevelExtents;
        std::vector<Planned> planned;
        planned.push_back({ kInitProgram, level0, mImpl->Levels[0].Image, levelExtents[0][0], levelExtents[0][1],
                            width, height, false });
        for (uint32_t k = 1; k <= mImpl->LowestLevel; ++k) {
            // Horizontal half into (W >> k, H >> (k - 1)), then the vertical one.
            const uint32_t halfStepWidth = levelExtents[k][0], halfStepHeight = levelExtents[k - 1U][1];
            planned.push_back({ kDownsampleProgram, view(mImpl->Levels[k - 1U].Image, false), mImpl->Halves[k].Image,
                                halfStepWidth, halfStepHeight, levelExtents[k - 1U][0], levelExtents[k - 1U][1],
                                true });
            planned.push_back({ kDownsampleProgram, view(mImpl->Halves[k].Image, false), mImpl->Levels[k].Image,
                                levelExtents[k][0], levelExtents[k][1], halfStepWidth, halfStepHeight, false });
        }
        planned.push_back({ kFuseProgram, level0, mImpl->Fused.Image, halfWidth, halfHeight, halfWidth, halfHeight,
                            false });
        planned.push_back({ kMainProgram, view(mImpl->Fused.Image, false), mImpl->Output.Image, width, height,
                            halfWidth, halfHeight, false });
        nri::DescriptorPoolDesc pool{};
        pool.descriptorSetMaxNum = uint32_t(planned.size());
        pool.textureMaxNum = uint32_t(planned.size()) * (2U + kLevelSlots);
        pool.storageTextureMaxNum = uint32_t(planned.size());
        pool.samplerMaxNum = uint32_t(planned.size());
        auto* device = NriInteropAccess::Device(*mImpl->Interop);
        mImpl->Sets.assign(planned.size(), nullptr);
        if (mImpl->Core->CreateDescriptorPool(*device, pool, mImpl->Pool) != nri::Result::SUCCESS ||
            mImpl->Core->AllocateDescriptorSets(*mImpl->Pool, *mImpl->Layout, 0, mImpl->Sets.data(),
                                                uint32_t(planned.size()), 0) != nri::Result::SUCCESS)
            throw std::runtime_error("LAUNCHPAD albedo descriptor allocation failed");
        for (size_t i = 0; i < planned.size(); ++i) {
            const auto& step = planned[i];
            auto* set = mImpl->Sets[i];
            auto* source = step.Source;
            auto* destination = view(step.Destination, true);
            const std::array<nri::UpdateDescriptorRangeDesc, kRangeCount> updates{
                { { set, kSceneBinding, 0, &scene, 1 },
                  { set, kSourceBinding, 0, &source, 1 },
                  { set, kDestinationBinding, 0, &destination, 1 },
                  { set, kSamplerBinding, 0, &mImpl->Sampler, 1 },
                  { set, kLevelsBinding, 0, levels.data(), kLevelSlots } }
            };
            mImpl->Core->UpdateDescriptorRanges(updates.data(), uint32_t(updates.size()));
            mImpl->Steps.push_back({ step.Program, set, step.Destination, step.Width, step.Height,
                                     step.SourceWidth, step.SourceHeight, step.Horizontal });
        }
        mImpl->Scene = sceneColor;
        mImpl->SceneFormat = sceneFormat;
        mImpl->Width = width;
        mImpl->Height = height;
        mImpl->Reason.clear();
        return true;
    } catch (const std::exception& error) {
        mImpl->Reason = error.what();
        InvalidateScreenResources();
        return false;
    }
#endif
}

bool LaunchpadAlbedoPass::Execute(uint32_t frameSlot, bool linearSource) {
    mImpl->LastBarriers = {};
    if (mImpl->Width == 0)
        return false;
#if !defined(ENABLE_OOT3D_NRI) || !defined(ENABLE_OOT3D_IMMERSE)
    (void)frameSlot;
    (void)linearSource;
    return false;
#else
    auto* command = NriInteropAccess::CommandBuffer(*mImpl->Interop, frameSlot);
    if (!command || mImpl->Steps.empty())
        return false;
    Immerse::LaunchpadAlbedoConstants push;
    push.LowestLevel = mImpl->LowestLevel;
    push.LinearSource = linearSource ? 1U : 0U;
    push.LevelExtents = mImpl->LevelExtents;
    const auto transition = [&](VkImage image, ResourceAccess access) {
        const auto change = mImpl->States.PlanTransition(reinterpret_cast<uintptr_t>(image), { access, 0 });
        if (!NriInteropAccess::CmdTextureBarrier(*mImpl->Interop, frameSlot, image, change))
            return false;
        mImpl->States.Commit(change);
        mImpl->LastBarriers.RecordPrivateTransition(change);
        return true;
    };
    mImpl->Core->CmdSetDescriptorPool(*command, *mImpl->Pool);
    mImpl->Core->CmdSetPipelineLayout(*command, nri::BindPoint::COMPUTE, *mImpl->Layout);
    for (const auto& step : mImpl->Steps) {
        if (!transition(step.Destination, ResourceAccess::ComputeWrite))
            return false;
        push.Width = step.Width;
        push.Height = step.Height;
        push.SourceWidth = step.SourceWidth;
        push.SourceHeight = step.SourceHeight;
        push.Horizontal = step.Horizontal ? 1U : 0U;
        mImpl->Core->CmdSetPipeline(*command, *mImpl->Pipelines[step.Program]);
        const nri::SetDescriptorSetDesc descriptors{ 0, step.Set };
        mImpl->Core->CmdSetDescriptorSet(*command, descriptors);
        const nri::SetRootConstantsDesc constants{ 0, &push, sizeof(push) };
        mImpl->Core->CmdSetRootConstants(*command, constants);
        mImpl->Core->CmdDispatch(*command, { (step.Width + 7) / 8, (step.Height + 7) / 8, 1 });
        if (!transition(step.Destination, ResourceAccess::ShaderRead))
            return false;
    }
    return true;
#endif
}

void LaunchpadAlbedoPass::InvalidateScreenResources() {
#if defined(ENABLE_OOT3D_NRI) && defined(ENABLE_OOT3D_IMMERSE)
    if (mImpl->Core && mImpl->Pool)
        mImpl->Core->DestroyDescriptorPool(mImpl->Pool);
    mImpl->Pool = nullptr;
    mImpl->Sets.clear();
    mImpl->Steps.clear();
#endif
    if (mImpl->Interop) {
        for (auto& texture : mImpl->Levels)
            mImpl->Interop->DestroyOwnedTexture(texture.Image);
        for (auto& texture : mImpl->Halves)
            mImpl->Interop->DestroyOwnedTexture(texture.Image);
        mImpl->Interop->DestroyOwnedTexture(mImpl->Fused.Image);
        mImpl->Interop->DestroyOwnedTexture(mImpl->Output.Image);
    }
    mImpl->Levels.clear();
    mImpl->Halves.clear();
    mImpl->Fused = {};
    mImpl->Output = {};
    mImpl->LowestLevel = 0;
    mImpl->LevelExtents = {};
    mImpl->Scene = VK_NULL_HANDLE;
    mImpl->SceneFormat = VK_FORMAT_UNDEFINED;
    mImpl->Width = mImpl->Height = 0;
    mImpl->States.Clear();
    mImpl->LastBarriers = {};
}
void LaunchpadAlbedoPass::Shutdown() {
    InvalidateScreenResources();
#if defined(ENABLE_OOT3D_NRI) && defined(ENABLE_OOT3D_IMMERSE)
    if (mImpl->Core) {
        if (mImpl->Sampler)
            mImpl->Core->DestroyDescriptor(mImpl->Sampler);
        for (auto* pipeline : mImpl->Pipelines)
            if (pipeline)
                mImpl->Core->DestroyPipeline(pipeline);
        if (mImpl->Layout)
            mImpl->Core->DestroyPipelineLayout(mImpl->Layout);
    }
#endif
    *mImpl = {};
}
bool LaunchpadAlbedoPass::Available() const {
    return mImpl->Ready;
}
VkImage LaunchpadAlbedoPass::OutputImage() const {
    return mImpl->Output.Image;
}
const EffectPassBarrierExecution& LaunchpadAlbedoPass::LastBarrierExecution() const {
    return mImpl->LastBarriers;
}
const std::string& LaunchpadAlbedoPass::UnavailableReason() const {
    return mImpl->Reason;
}
} // namespace Fast::Oot3d
#endif
