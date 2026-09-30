#include "fast/oot3d/launchpad_normals_pass.h"
#ifdef ENABLE_OOT3D_VULKAN
#include "fast/oot3d/nri_interop_context.h"
#include "fast/oot3d/resource_state_tracker.h"
#ifdef ENABLE_OOT3D_IMMERSE
#include "fast/immerse/launchpad_normals_shader.h"
#endif
#if defined(ENABLE_OOT3D_NRI) && defined(ENABLE_OOT3D_IMMERSE)
#include "nri_interop_internal.h"
#endif
#include <algorithm>
#include <array>
#include <stdexcept>

namespace Fast::Oot3d {
#if defined(ENABLE_OOT3D_NRI) && defined(ENABLE_OOT3D_IMMERSE)
namespace {
enum : size_t { kGbufferProgram, kWideProgram, kNarrowProgram, kProgramCount };
} // namespace
#endif

struct LaunchpadNormalsPass::Impl {
    VkDevice Device = VK_NULL_HANDLE;
    NriInteropContext* Interop = nullptr;
#if defined(ENABLE_OOT3D_NRI) && defined(ENABLE_OOT3D_IMMERSE)
    nri::CoreInterface* Core = nullptr;
    nri::PipelineLayout* Layout = nullptr;
    std::array<nri::Pipeline*, kProgramCount> Pipelines{};
    nri::Descriptor* Sampler = nullptr;
    nri::DescriptorPool* Pool = nullptr;
    std::array<nri::DescriptorSet*, kProgramCount> Sets{};
#endif
    // Half-height packed surfaces (RG32UI) and the full-resolution guide.
    NriOwnedTexture2D Gbuffer;
    NriOwnedTexture2D Wide;
    NriOwnedTexture2D Output;
    std::array<VkImage, 2> Inputs{};
    std::array<VkFormat, 2> Formats{};
    uint32_t Width = 0, Height = 0;
    bool Ready = false;
    ResourceStateTracker States;
    EffectPassBarrierExecution LastBarriers;
#if defined(ENABLE_OOT3D_NRI) && defined(ENABLE_OOT3D_IMMERSE)
    std::string Reason = "iMMERSE LAUNCHPAD normals are not initialized";
#else
    std::string Reason = "iMMERSE LAUNCHPAD normals were not compiled into this build";
#endif
};

LaunchpadNormalsPass::LaunchpadNormalsPass() : mImpl(std::make_unique<Impl>()) {
}
LaunchpadNormalsPass::~LaunchpadNormalsPass() {
    Shutdown();
}

bool LaunchpadNormalsPass::Initialize(VkPhysicalDevice physical, VkDevice device, NriInteropContext& interop) {
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
        constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        for (const VkFormat format : { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32_UINT }) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
            if ((properties.optimalTilingFeatures & required) != required)
                throw std::runtime_error("LAUNCHPAD normals require sampled/storage RGBA16F and RG32UI");
        }
        auto* nriDevice = NriInteropAccess::Device(interop);
        mImpl->Core = NriInteropAccess::Core(interop);
        if (nriDevice == nullptr || mImpl->Core == nullptr)
            throw std::runtime_error("LAUNCHPAD normals NRI device unavailable");
        const std::array<nri::DescriptorRangeDesc, 5> ranges{
            { { 0, 1, nri::DescriptorType::TEXTURE, nri::StageBits::COMPUTE_SHADER },
              { 1, 1, nri::DescriptorType::TEXTURE, nri::StageBits::COMPUTE_SHADER },
              { 2, 1, nri::DescriptorType::TEXTURE, nri::StageBits::COMPUTE_SHADER },
              { 3, 1, nri::DescriptorType::STORAGE_TEXTURE, nri::StageBits::COMPUTE_SHADER },
              { 4, 1, nri::DescriptorType::SAMPLER, nri::StageBits::COMPUTE_SHADER } }
        };
        nri::DescriptorSetDesc set{};
        set.ranges = ranges.data();
        set.rangeNum = uint32_t(ranges.size());
        nri::RootConstantDesc root{};
        root.size = sizeof(Immerse::LaunchpadNormalsConstants);
        root.shaderStages = nri::StageBits::COMPUTE_SHADER;
        nri::PipelineLayoutDesc layout{};
        layout.descriptorSets = &set;
        layout.descriptorSetNum = 1;
        layout.rootConstants = &root;
        layout.rootConstantNum = 1;
        layout.shaderStages = nri::StageBits::COMPUTE_SHADER;
        layout.flags = nri::PipelineLayoutBits::IGNORE_GLOBAL_SPIRV_OFFSETS;
        if (mImpl->Core->CreatePipelineLayout(*nriDevice, layout, mImpl->Layout) != nri::Result::SUCCESS)
            throw std::runtime_error("LAUNCHPAD normals pipeline layout failed");
        const std::array<std::string, kProgramCount> sources{ Immerse::BuildLaunchpadGbufferShader(),
                                                              Immerse::BuildLaunchpadSmoothWideShader(),
                                                              Immerse::BuildLaunchpadSmoothNarrowShader() };
        constexpr std::array<const char*, kProgramCount> names{ "immerse_launchpad_gbuffer.comp",
                                                                "immerse_launchpad_smooth_wide.comp",
                                                                "immerse_launchpad_smooth_narrow.comp" };
        for (size_t i = 0; i < sources.size(); ++i) {
            const auto spirv = interop.Shaders().Resolve(sources[i], Renderer::SpirvStage::Compute, names[i]);
            nri::ComputePipelineDesc pipeline{};
            pipeline.pipelineLayout = mImpl->Layout;
            pipeline.shader.stage = nri::StageBits::COMPUTE_SHADER;
            pipeline.shader.bytecode = spirv.data();
            pipeline.shader.size = spirv.size() * sizeof(uint32_t);
            if (mImpl->Core->CreateComputePipeline(*nriDevice, pipeline, mImpl->Pipelines[i]) != nri::Result::SUCCESS)
                throw std::runtime_error("LAUNCHPAD normals pipeline failed");
        }
        nri::SamplerDesc sampler{};
        sampler.filters = { nri::Filter::NEAREST, nri::Filter::NEAREST, nri::Filter::NEAREST };
        sampler.addressModes = { nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE,
                                 nri::AddressMode::CLAMP_TO_EDGE };
        if (mImpl->Core->CreateSampler(*nriDevice, sampler, mImpl->Sampler) != nri::Result::SUCCESS)
            throw std::runtime_error("LAUNCHPAD normals sampler failed");
        mImpl->Ready = true;
        mImpl->Reason.clear();
        return true;
    } catch (const std::exception& error) {
        mImpl->Reason = error.what();
        return false;
    }
#endif
}

bool LaunchpadNormalsPass::Configure(uint32_t width, uint32_t height, VkImage depth, VkFormat depthFormat,
                                     VkImage normal, VkFormat normalFormat) {
    const std::array inputs{ depth, normal };
    const std::array formats{ depthFormat, normalFormat };
    if (!Available() || width == 0 || height == 0 || depth == VK_NULL_HANDLE || normal == VK_NULL_HANDLE)
        return false;
    if (mImpl->Width == width && mImpl->Height == height && mImpl->Inputs == inputs && mImpl->Formats == formats)
        return true;
#if !defined(ENABLE_OOT3D_NRI) || !defined(ENABLE_OOT3D_IMMERSE)
    return false;
#else
    // Descriptor sets are rebuilt only once recorded work that may still use
    // them has completed.
    if (mImpl->Width != 0 && !mImpl->Interop->CompleteRecordedWork())
        return false;
    try {
        if (mImpl->Width != width || mImpl->Height != height) {
            InvalidateScreenResources();
            const uint32_t halfHeight = Immerse::LaunchpadHalfHeight(height);
            if (!mImpl->Interop->CreateOwnedTexture2D(width, halfHeight, VK_FORMAT_R32G32_UINT, mImpl->Gbuffer) ||
                !mImpl->Interop->CreateOwnedTexture2D(width, halfHeight, VK_FORMAT_R32G32_UINT, mImpl->Wide) ||
                !mImpl->Interop->CreateOwnedTexture2D(width, height, VK_FORMAT_R16G16B16A16_SFLOAT, mImpl->Output))
                throw std::runtime_error("LAUNCHPAD normals allocation failed");
        } else if (mImpl->Pool) {
            mImpl->Core->DestroyDescriptorPool(mImpl->Pool);
            mImpl->Pool = nullptr;
            mImpl->Sets = {};
        }
        std::array<nri::Descriptor*, 2> sources{};
        for (size_t i = 0; i < inputs.size(); ++i) {
            if (!mImpl->Interop->WrapTexture(inputs[i], formats[i], VK_IMAGE_TYPE_2D, VK_IMAGE_USAGE_SAMPLED_BIT,
                                             width, height))
                throw std::runtime_error("LAUNCHPAD normals input wrapping failed");
            sources[i] = NriInteropAccess::TextureView(*mImpl->Interop, inputs[i], false);
            if (!sources[i])
                throw std::runtime_error("LAUNCHPAD normals input view unavailable");
        }
        nri::DescriptorPoolDesc pool{};
        pool.descriptorSetMaxNum = kProgramCount;
        pool.textureMaxNum = 3 * kProgramCount;
        pool.storageTextureMaxNum = kProgramCount;
        pool.samplerMaxNum = kProgramCount;
        auto* device = NriInteropAccess::Device(*mImpl->Interop);
        if (mImpl->Core->CreateDescriptorPool(*device, pool, mImpl->Pool) != nri::Result::SUCCESS ||
            mImpl->Core->AllocateDescriptorSets(*mImpl->Pool, *mImpl->Layout, 0, mImpl->Sets.data(), kProgramCount,
                                                0) != nri::Result::SUCCESS)
            throw std::runtime_error("LAUNCHPAD normals descriptor allocation failed");
        const auto view = [&](VkImage image, bool storage) {
            auto* descriptor = NriInteropAccess::TextureView(*mImpl->Interop, image, storage);
            if (!descriptor)
                throw std::runtime_error("LAUNCHPAD normals view unavailable");
            return descriptor;
        };
        // Binding 2 holds the packed surfaces a program reads. The G-buffer
        // program reads none; it binds the wide result so the unused descriptor
        // still has a valid layout.
        const std::array<std::pair<nri::Descriptor*, nri::Descriptor*>, kProgramCount> pairs{ {
            { view(mImpl->Wide.Image, false), view(mImpl->Gbuffer.Image, true) },
            { view(mImpl->Gbuffer.Image, false), view(mImpl->Wide.Image, true) },
            { view(mImpl->Wide.Image, false), view(mImpl->Output.Image, true) },
        } };
        for (size_t i = 0; i < kProgramCount; ++i) {
            auto* set = mImpl->Sets[i];
            auto* surfaces = pairs[i].first;
            auto* destination = pairs[i].second;
            const std::array<nri::UpdateDescriptorRangeDesc, 5> updates{ { { set, 0, 0, &sources[0], 1 },
                                                                           { set, 1, 0, &sources[1], 1 },
                                                                           { set, 2, 0, &surfaces, 1 },
                                                                           { set, 3, 0, &destination, 1 },
                                                                           { set, 4, 0, &mImpl->Sampler, 1 } } };
            mImpl->Core->UpdateDescriptorRanges(updates.data(), uint32_t(updates.size()));
        }
        mImpl->Width = width;
        mImpl->Height = height;
        mImpl->Inputs = inputs;
        mImpl->Formats = formats;
        mImpl->Reason.clear();
        return true;
    } catch (const std::exception& error) {
        mImpl->Reason = error.what();
        InvalidateScreenResources();
        return false;
    }
#endif
}

bool LaunchpadNormalsPass::Execute(uint32_t frameSlot, const PerspectiveViewState& view, bool wBuffer,
                                   PicaSurfaceCoordinates coordinates) {
    mImpl->LastBarriers = {};
    if (mImpl->Width == 0 || view.NearPlane <= 0 || view.FarPlane <= view.NearPlane)
        return false;
#if !defined(ENABLE_OOT3D_NRI) || !defined(ENABLE_OOT3D_IMMERSE)
    (void)frameSlot;
    (void)coordinates;
    (void)wBuffer;
    return false;
#else
    auto* command = NriInteropAccess::CommandBuffer(*mImpl->Interop, frameSlot);
    if (!command || std::any_of(mImpl->Sets.begin(), mImpl->Sets.end(), [](auto* set) { return set == nullptr; }))
        return false;
    Immerse::LaunchpadNormalsConstants push;
    push.Width = mImpl->Width;
    push.Height = mImpl->Height;
    const auto zero = coordinates.StorageToViewNdc({ 0, 0 });
    const auto x = coordinates.StorageToViewNdc({ 1, 0 });
    const auto y = coordinates.StorageToViewNdc({ 0, 1 });
    push.ViewX = { (x[0] - zero[0]) / view.Projection[0], (y[0] - zero[0]) / view.Projection[0],
                   (zero[0] + view.Projection[8]) / view.Projection[0], 0 };
    push.ViewY = { (x[1] - zero[1]) / view.Projection[5], (y[1] - zero[1]) / view.Projection[5],
                   (zero[1] + view.Projection[9]) / view.Projection[5], 0 };
    push.Depth = { wBuffer ? view.NearPlane : view.NearPlane * view.FarPlane, view.FarPlane,
                   view.FarPlane - view.NearPlane, wBuffer ? 1.0F : 0.0F };
    const float side = float(std::min(mImpl->Width, mImpl->Height));
    push.Scale = { view.NearPlane, Immerse::kLaunchpadWideRadius * side, Immerse::kLaunchpadNarrowRadius * side, 0 };
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
    const auto dispatch = [&](size_t program, uint32_t width, uint32_t height) {
        push.Iteration = uint32_t(program);
        mImpl->Core->CmdSetPipeline(*command, *mImpl->Pipelines[program]);
        const nri::SetDescriptorSetDesc descriptors{ 0, mImpl->Sets[program] };
        mImpl->Core->CmdSetDescriptorSet(*command, descriptors);
        const nri::SetRootConstantsDesc constants{ 0, &push, sizeof(push) };
        mImpl->Core->CmdSetRootConstants(*command, constants);
        mImpl->Core->CmdDispatch(*command, { (width + 7) / 8, (height + 7) / 8, 1 });
    };
    const uint32_t halfHeight = Immerse::LaunchpadHalfHeight(mImpl->Height);
    if (!transition(mImpl->Gbuffer.Image, ResourceAccess::ComputeWrite))
        return false;
    dispatch(kGbufferProgram, mImpl->Width, halfHeight);
    if (!transition(mImpl->Gbuffer.Image, ResourceAccess::ShaderRead) ||
        !transition(mImpl->Wide.Image, ResourceAccess::ComputeWrite))
        return false;
    dispatch(kWideProgram, mImpl->Width, halfHeight);
    if (!transition(mImpl->Wide.Image, ResourceAccess::ShaderRead) ||
        !transition(mImpl->Output.Image, ResourceAccess::ComputeWrite))
        return false;
    dispatch(kNarrowProgram, mImpl->Width, mImpl->Height);
    return transition(mImpl->Output.Image, ResourceAccess::ShaderRead);
#endif
}

void LaunchpadNormalsPass::InvalidateScreenResources() {
#if defined(ENABLE_OOT3D_NRI) && defined(ENABLE_OOT3D_IMMERSE)
    if (mImpl->Core && mImpl->Pool)
        mImpl->Core->DestroyDescriptorPool(mImpl->Pool);
    mImpl->Pool = nullptr;
    mImpl->Sets = {};
#endif
    if (mImpl->Interop) {
        mImpl->Interop->DestroyOwnedTexture(mImpl->Gbuffer.Image);
        mImpl->Interop->DestroyOwnedTexture(mImpl->Wide.Image);
        mImpl->Interop->DestroyOwnedTexture(mImpl->Output.Image);
    }
    mImpl->Gbuffer = {};
    mImpl->Wide = {};
    mImpl->Output = {};
    mImpl->Width = mImpl->Height = 0;
    mImpl->Inputs = {};
    mImpl->Formats = {};
    mImpl->States.Clear();
    mImpl->LastBarriers = {};
}
void LaunchpadNormalsPass::Shutdown() {
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
bool LaunchpadNormalsPass::Available() const {
    return mImpl->Ready;
}
VkImage LaunchpadNormalsPass::OutputImage() const {
    return mImpl->Output.Image;
}
VkImageView LaunchpadNormalsPass::OutputView() const {
    return mImpl->Output.SampledView;
}
VkFormat LaunchpadNormalsPass::OutputFormat() const {
    return VK_FORMAT_R16G16B16A16_SFLOAT;
}
const EffectPassBarrierExecution& LaunchpadNormalsPass::LastBarrierExecution() const {
    return mImpl->LastBarriers;
}
const std::string& LaunchpadNormalsPass::UnavailableReason() const {
    return mImpl->Reason;
}
} // namespace Fast::Oot3d
#endif
