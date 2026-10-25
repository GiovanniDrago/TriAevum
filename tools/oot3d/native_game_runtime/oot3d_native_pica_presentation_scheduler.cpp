#include "oot3d_native_pica_presentation_scheduler.h"

#include "oot3d_native_pica_composition.h"
#include "oot3d_native_pica_vulkan_bridge.h"
#include "oot3d_static_shadow_actor_catalog.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace Oot3dNativeGame {
namespace {

void SetError(std::string *error, std::string_view message) {
  if (error != nullptr) {
    *error = message;
  }
}

bool SameTransfer(const Oot3dPicaDisplayTransferSubmission &left,
                  const Oot3dPicaDisplayTransferSubmission &right) {
  return left.CompletionId == right.CompletionId &&
         left.InputPhysicalAddress == right.InputPhysicalAddress &&
         left.OutputPhysicalAddress == right.OutputPhysicalAddress;
}

class OptionalTimer final {
public:
  OptionalTimer(bool enabled, double &destination)
      : mEnabled(enabled), mDestination(destination),
        mStarted(enabled ? std::chrono::steady_clock::now()
                         : std::chrono::steady_clock::time_point{}) {
  }

  ~OptionalTimer() {
    if (mEnabled) {
      mDestination +=
          std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                        mStarted)
              .count();
    }
  }

private:
  bool mEnabled = false;
  double &mDestination;
  std::chrono::steady_clock::time_point mStarted;
};

} // namespace

void Oot3dPicaPresentationScheduler::Capture(Oot3dPicaVulkanDrawPlan plan) {
  ++mStats.ProducedGuestDraws;
  mAccumulator.Append(std::move(plan));
}

void Oot3dPicaPresentationScheduler::Capture(
    Oot3dPicaMemoryFillSubmission fill) {
  ++mStats.CapturedMemoryFills;
  mAccumulator.Append(std::move(fill));
}

void Oot3dPicaPresentationScheduler::Capture(
    Oot3dPicaDisplayTransferSubmission transfer) {
  ++mStats.CapturedDisplayTransfers;
  mAccumulator.Append(std::move(transfer));
}

std::optional<Oot3dPicaVisualFrame> Oot3dPicaPresentationScheduler::FinishFrame(
    const Oot3dPicaDisplayTransferSubmission &topTransfer) {
  auto frame = mAccumulator.Finish(topTransfer);
  mStats.CompletedVisualFrames += frame.has_value() ? 1U : 0U;
  return frame;
}

std::vector<Oot3dPicaVisualFrame> Oot3dPicaPresentationScheduler::FinishFrames(
    const Oot3dPicaDisplayTransferSubmission &topTransfer,
    std::span<const uint32_t> displayOutputs,
    std::span<const Oot3dPicaViewportState> continuationViewports) {
  auto frames = mAccumulator.FinishFrames(topTransfer, displayOutputs,
                                          continuationViewports);
  mStats.CompletedVisualFrames += frames.size();
  mStats.SupersededVisualFrames += frames.empty() ? 0U : frames.size() - 1U;
  return frames;
}

void Oot3dPicaPresentationScheduler::BeginPresentation(
    uint64_t presentationSerial) {
  if (mPresentationSerial == presentationSerial) {
    return;
  }
  mPresentationSerial = presentationSerial;
  for (auto& [targetNamespace, draws] : mExecutedDrawsThisPresentation) {
    draws.clear();
  }
}

bool Oot3dPicaPresentationScheduler::ViewDependencyWork(
    Oot3dPicaVisualFrameViewSample& output) {
  return mAccumulator.ViewDependencyWork(output);
}

Oot3dPicaPresentationSchedulerState
Oot3dPicaPresentationScheduler::CaptureState() const {
  return {mAccumulator.CaptureState(), mSnapshotCompletions};
}

bool Oot3dPicaPresentationScheduler::RestoreState(
    Oot3dPicaPresentationSchedulerState state) {
  if (!mAccumulator.RestoreState(std::move(state.Accumulator))) {
    return false;
  }
  mSnapshotCompletions = std::move(state.SnapshotCompletions);
  mPresentationSerial.reset();
  mExecutedDrawsThisPresentation.clear();
  return true;
}

bool Oot3dPicaPresentationScheduler::Execute(
    Oot3d::Renderer::PicaRenderBackend &backend,
    const Oot3dPicaVisualFrameViewSample &sample,
    uint64_t renderTargetNamespace, Oot3dPicaPresentationExecutionKind kind,
    bool present, std::string *error) {
  return ExecuteInternal(backend, sample, nullptr, renderTargetNamespace, kind,
                         present, error);
}

bool Oot3dPicaPresentationScheduler::ExecuteWithNetplay(
    Oot3d::Renderer::PicaRenderBackend &backend,
    const Oot3dPicaVisualFrameViewSample &sample,
    const NetplayOverlayDraw &overlay, uint64_t renderTargetNamespace,
    Oot3dPicaPresentationExecutionKind kind, bool present,
    std::string *error) {
  return ExecuteInternal(backend, sample, &overlay, renderTargetNamespace,
                         kind, present, error);
}

namespace {

std::optional<Oot3dPicaVisualDrawView> BuildNetplayOverlayView(
    const Oot3dPicaVisualDrawView &source, const NetplayOverlayDraw &overlay) {
  if (!overlay.Enabled || source.BasePlan == nullptr) return std::nullopt;
  const auto &draw = *source.BasePlan;
  if (draw.CompositionDomain != Oot3dPicaCompositionDomain::Scene ||
      !draw.Composition.NativeActorGeometry ||
      (draw.Composition.NativeActorIdentity & 0xFFFFFFFFULL) !=
          overlay.ActorAddress ||
      draw.VertexShader.TemporalProgram == nullptr) {
    return std::nullopt;
  }
  const auto &layout = draw.VertexShader.TemporalProgram->Hooks.Transform;
  if (!layout.Available()) return std::nullopt;
  Oot3dPicaVertexUniformState uniforms =
      source.VertexUniforms.has_value() ? *source.VertexUniforms
                                       : draw.VertexShader.Uniforms;
  const float dx = overlay.Remote.Position[0] - overlay.Local.Position[0];
  const float dy = overlay.Remote.Position[1] - overlay.Local.Position[1];
  const float dz = overlay.Remote.Position[2] - overlay.Local.Position[2];
  const float yaw = overlay.Remote.Yaw - overlay.Local.Yaw;
  const float c = std::cos(yaw);
  const float s = std::sin(yaw);
  const float tx = overlay.Remote.Position[0] -
                   (c * overlay.Local.Position[0] +
                    s * overlay.Local.Position[2]);
  const float tz = overlay.Remote.Position[2] -
                   (-s * overlay.Local.Position[0] +
                    c * overlay.Local.Position[2]);
  const std::array<std::array<float, 4>, 3> transform{{
      {c, 0.0F, s, tx},
      {0.0F, 1.0F, 0.0F, dy},
      {-s, 0.0F, c, tz},
  }};
  for (size_t row = 0; row < 3U; ++row) {
    const size_t index = layout.ModelFirstUniform + row;
    if (index >= uniforms.Floats.size()) return std::nullopt;
    const auto local = uniforms.Floats[index];
    std::array<float, 4> result{};
    for (size_t column = 0; column < 4U; ++column) {
      result[column] = transform[row][0] * local[0] +
                       transform[row][1] * local[1] +
                       transform[row][2] * local[2] + transform[row][3];
    }
    uniforms.Floats[index] = result;
  }
  Oot3dPicaVisualDrawView clone = source;
  clone.VertexUniforms = std::move(uniforms);
  clone.HostReplay = true;
  static bool trace = std::getenv("TRIAEVUM_NETPLAY_OVERLAY_TRACE") != nullptr;
  if (trace) {
    std::fprintf(stderr,
                 "TRIAEVUM_NETPLAY_OVERLAY actor=0x%08X local=(%.2f,%.2f,%.2f) "
                 "remote=(%.2f,%.2f,%.2f)\n",
                 overlay.ActorAddress, overlay.Local.Position[0],
                 overlay.Local.Position[1], overlay.Local.Position[2],
                 overlay.Remote.Position[0], overlay.Remote.Position[1],
                 overlay.Remote.Position[2]);
    trace = false;
  }
  return clone;
}

} // namespace

bool Oot3dPicaPresentationScheduler::ExecuteInternal(
    Oot3d::Renderer::PicaRenderBackend &backend,
    const Oot3dPicaVisualFrameViewSample &sample,
    const NetplayOverlayDraw *overlay, uint64_t renderTargetNamespace,
    Oot3dPicaPresentationExecutionKind kind, bool present,
    std::string *error) {
  OptionalTimer executeTimer(mTimings.Enabled, mTimings.ExecuteSeconds);
  if (!mPresentationSerial.has_value()) {
    SetError(error, "PICA presentation execution began without a host frame");
    return false;
  }
  if (sample.Draws.empty() && sample.DisplayTransfers.empty() &&
      kind == Oot3dPicaPresentationExecutionKind::DependencyFlush) {
    SetError(error, "PICA presentation sample is incomplete");
    return false;
  }

  {
    OptionalTimer validationTimer(mTimings.Enabled,
                                  mTimings.ValidationSeconds);
    std::unordered_set<uint64_t> sampleDraws;
    sampleDraws.reserve(sample.Draws.size());
    auto& executedDraws =
        mExecutedDrawsThisPresentation[renderTargetNamespace];
    for (const auto &draw : sample.Draws) {
      if (draw.BasePlan == nullptr ||
          !sampleDraws.insert(draw.BasePlan->SubmissionId).second ||
          executedDraws.contains(draw.BasePlan->SubmissionId)) {
        ++mStats.DuplicateDrawAttempts;
        SetError(error,
                 "PICA draw would execute more than once in one presentation");
        return false;
      }
    }
    executedDraws.insert(sampleDraws.begin(), sampleDraws.end());
  }
  mStats.SelectedDraws += sample.Draws.size();
  const bool presentation =
      kind != Oot3dPicaPresentationExecutionKind::DependencyFlush &&
      kind != Oot3dPicaPresentationExecutionKind::NativeFrame;

  // A left-out draw is neither published nor submitted, so the backend's
  // composition schedule matches what it executes.
  mCompositionDrawScratch.clear();
  mCompositionDrawScratch.reserve(sample.Draws.size());
  mOmittedDrawScratch.assign(sample.Draws.size(), false);
  for (size_t index = 0; index < sample.Draws.size(); ++index) {
    const auto &draw = *sample.Draws[index].BasePlan;
    const auto domain = static_cast<Oot3d::Renderer::PicaCompositionDomain>(
        static_cast<uint8_t>(draw.CompositionDomain));
    if (presentation && !mNativeGameplayHudVisible &&
        domain == Oot3d::Renderer::PicaCompositionDomain::Ui &&
        draw.Composition.SourcePc == kOot3dGameplayHudDrawEntry) {
      mOmittedDrawScratch[index] = true;
      ++mStats.OmittedHudDraws;
      continue;
    }
    const auto &framebuffer = draw.State.Framebuffer;
    mCompositionDrawScratch.push_back({
        .SubmissionId = draw.SubmissionId,
        .Target = {
            .RenderTargetNamespace = renderTargetNamespace,
            .ColorPhysicalAddress = framebuffer.ColorPhysicalAddress,
            .DepthPhysicalAddress = framebuffer.DepthPhysicalAddress,
            .FramebufferWidth = framebuffer.Width,
            .FramebufferHeight = framebuffer.Height,
            .ColorFormat = framebuffer.ColorFormat,
            .DepthFormat = framebuffer.DepthFormat,
        },
        .Domain = domain,
        .Composition = {
            .Layer = static_cast<Oot3d::Renderer::PicaCompositionLayer>(
                static_cast<uint8_t>(draw.Composition.Layer)),
            .Provenance =
                static_cast<Oot3d::Renderer::PicaCompositionProvenance>(
                    static_cast<uint8_t>(draw.Composition.Provenance)),
            .SourcePc = draw.Composition.SourcePc,
            .NativeValue = draw.Composition.NativeValue,
            .NativeProjectedShadow = draw.Composition.NativeProjectedShadow,
            .NativeActorGeometry = draw.Composition.NativeActorGeometry,
            .NativeActorIdentity = draw.Composition.NativeActorIdentity,
            .IsolatedLighting = draw.Composition.IsolatedLighting,
            .TemporalIdentity = draw.Composition.TemporalIdentity,
            .StaticShadowCacheEligible =
                Oot3dNativeGame::StaticShadowCacheEligibleActor(
                    draw.Composition.NativeActorGeometry,
                    draw.Composition.NativeActorIdentity),
        },
    });
  }
  uint64_t compositionSequenceId = mNextCompositionSequenceId++;
  if (mNextCompositionSequenceId == 0U) {
    mNextCompositionSequenceId = 1U;
  }
  {
    OptionalTimer timer(mTimings.Enabled,
                        mTimings.CompositionPublicationSeconds);
    if (!backend.PublishPicaCompositionSequence(
            {
                .SchemaVersion = Oot3d::Renderer::
                    kPicaCompositionSequenceSchemaVersion,
                .SequenceId = compositionSequenceId,
                .Draws = mCompositionDrawScratch,
                .Purpose =
                    presentation
                        ? Oot3d::Renderer::PicaCompositionPurpose::Presentation
                        : Oot3d::Renderer::PicaCompositionPurpose::NativeMemory,
            },
            error)) {
      ++mStats.CompositionPublicationFailures;
      return false;
    }
  }
  ++mStats.CompositionSequencesPublished;
  mStats.CompositionDrawReferencesPublished +=
      mCompositionDrawScratch.size();

  // Presentation work the backend needs ahead of native order (this sample's
  // shadow casters before their receivers) receives the draws it selects
  // first; every draw is still submitted below, in native order, exactly once.
  // Native-memory execution keeps its canonical sequence untouched.
  if (presentation) {
    OptionalTimer timer(mTimings.Enabled, mTimings.DrawPreparationSeconds);
    size_t published = 0;
    for (size_t index = 0; index < sample.Draws.size(); ++index) {
      if (mOmittedDrawScratch[index] ||
          !backend.WantsPicaSequenceDrawPreparation(
              mCompositionDrawScratch[published++])) {
        continue;
      }
      const auto &drawView = sample.Draws[index];
      const auto overrides = drawView.Overrides();
      if (!PrepareOot3dPicaVulkanSequenceDraw(backend, *drawView.BasePlan,
                                              error, renderTargetNamespace,
                                              &overrides)) {
        return false;
      }
      ++mStats.PreparedDraws;
    }
  }

  ++mStats.ExecutionLists;
  switch (kind) {
  case Oot3dPicaPresentationExecutionKind::CurrentFrame:
    ++mStats.CurrentFrameLists;
    break;
  case Oot3dPicaPresentationExecutionKind::InterpolatedFrame:
    ++mStats.InterpolatedFrameLists;
    break;
  case Oot3dPicaPresentationExecutionKind::RepeatedFrame:
    ++mStats.RepeatedFrameLists;
    break;
  case Oot3dPicaPresentationExecutionKind::NativeFrame:
    ++mStats.NativeFrameLists;
    break;
  case Oot3dPicaPresentationExecutionKind::DependencyFlush:
    ++mStats.DependencyFlushes;
    break;
  }
  mStats.InterpolatedDraws += sample.InterpolatedDraws;

  size_t fillIndex = 0;
  size_t transferIndex = 0;
  bool topSnapshotExecuted = false;
  const auto fills = sample.MemoryFills;
  const auto transfers = sample.DisplayTransfers;
  const auto submitFill = [&](const Oot3dPicaMemoryFillSubmission &fill) {
    OptionalTimer timer(mTimings.Enabled, mTimings.MemoryFillSeconds);
    if (!SubmitOot3dPicaVulkanMemoryFill(backend, fill, error,
                                         renderTargetNamespace)) {
      return false;
    }
    ++mStats.MemoryFillsExecuted;
    return true;
  };
  const auto submitTransfer = [&](const Oot3dPicaDisplayTransferSubmission
                                      &transfer) {
    OptionalTimer timer(mTimings.Enabled, mTimings.SnapshotTransferSeconds);
    // Real execution publishes byte visibility and discharges PPF, including
    // guarded deferred RAM. Replayed/interpolated/restored transfers recreate
    // only the GPU version; they never publish a second guest write.
    const bool materializeGuest =
        kind == Oot3dPicaPresentationExecutionKind::DependencyFlush &&
        transfer.Transfer.TextureCopyBytes != 0U && transfer.SignalInterrupt;
    if (!SubmitOot3dPicaVulkanDisplayTransfer(backend, transfer, false, error,
                                              renderTargetNamespace,
                                              materializeGuest)) {
      return false;
    }
    mSnapshotCompletions[MakeSnapshotTarget(transfer, renderTargetNamespace)] =
        transfer.CompletionId;
    ++mStats.DisplaySnapshotsExecuted;
    topSnapshotExecuted =
        topSnapshotExecuted || SameTransfer(transfer, sample.TopTransfer);
    return true;
  };

  for (size_t drawIndex = 0; drawIndex < sample.Draws.size(); ++drawIndex) {
    const auto &drawView = sample.Draws[drawIndex];
    const auto &draw = *drawView.BasePlan;
    while (transferIndex < transfers.size() &&
           transfers[transferIndex].AfterDrawSubmissionId < draw.SubmissionId) {
      if (!submitTransfer(transfers[transferIndex++])) {
        return false;
      }
    }
    while (fillIndex < fills.size() &&
           fills[fillIndex].BeforeDrawSubmissionId <= draw.SubmissionId) {
      if (!submitFill(fills[fillIndex++])) {
        return false;
      }
    }
    const auto overrides = drawView.Overrides();
    if (!mOmittedDrawScratch[drawIndex]) {
      OptionalTimer timer(mTimings.Enabled, mTimings.DrawSubmissionSeconds);
      Oot3dPicaVulkanDrawBridgeTiming bridgeTiming;
      const bool submitted = SubmitOot3dPicaVulkanDrawPlan(
          backend, draw, error, renderTargetNamespace, &overrides,
          mTimings.Enabled ? &bridgeTiming : nullptr);
      if (mTimings.Enabled) {
        mTimings.DrawUniformPackingSeconds +=
            bridgeTiming.UniformPackingSeconds;
        mTimings.DrawViewConstructionSeconds +=
            bridgeTiming.ViewConstructionSeconds;
        mTimings.DrawBackendSubmissionSeconds +=
            bridgeTiming.BackendSubmissionSeconds;
      }
      if (!submitted) {
        return false;
      }
      ++mStats.ExecutedDraws;
    }
    if (overlay != nullptr) {
      const auto clone = BuildNetplayOverlayView(drawView, *overlay);
      if (clone.has_value()) {
        const auto cloneOverrides = clone->Overrides();
        if (!SubmitOot3dPicaVulkanDrawPlan(
                backend, draw, error, renderTargetNamespace, &cloneOverrides,
                nullptr)) {
          return false;
        }
        ++mStats.ExecutedDraws;
      }
    }
    while (transferIndex < transfers.size() &&
           transfers[transferIndex].AfterDrawSubmissionId ==
               draw.SubmissionId) {
      if (!submitTransfer(transfers[transferIndex++])) {
        return false;
      }
    }
  }
  while (fillIndex < fills.size()) {
    if (!submitFill(fills[fillIndex++])) {
      return false;
    }
  }
  while (transferIndex < transfers.size()) {
    if (!submitTransfer(transfers[transferIndex++])) {
      return false;
    }
  }

  if (kind == Oot3dPicaPresentationExecutionKind::DependencyFlush) {
    if (present) {
      SetError(error, "PICA dependency flush cannot present a scanout");
      return false;
    }
    mAccumulator.CompleteDependencyWork();
    return true;
  }
  if (!topSnapshotExecuted) {
    if (!submitTransfer(sample.TopTransfer)) {
      return false;
    }
  } else {
    ++mStats.RedundantTopSnapshotCopiesAvoided;
  }
  if (present) {
    {
      OptionalTimer timer(mTimings.Enabled, mTimings.PresentTransferSeconds);
      if (!SubmitOot3dPicaVulkanDisplayTransfer(
              backend, sample.TopTransfer, true, error,
              renderTargetNamespace)) {
        return false;
      }
    }
    ++mStats.PresentedTransfers;
  }
  return true;
}

bool Oot3dPicaPresentationScheduler::PresentExisting(
    Oot3d::Renderer::PicaRenderBackend &backend,
    const Oot3dPicaDisplayTransferSubmission &transfer,
    uint64_t renderTargetNamespace, std::string *error) {
  OptionalTimer timer(mTimings.Enabled, mTimings.PresentExistingSeconds);
  if (!HasSnapshot(transfer, renderTargetNamespace)) {
    SetError(error, "PICA display snapshot has not been executed");
    return false;
  }
  if (!SubmitOot3dPicaVulkanDisplayTransfer(backend, transfer, true, error,
                                            renderTargetNamespace)) {
    return false;
  }
  ++mStats.PresentedTransfers;
  ++mStats.ReusedSnapshots;
  return true;
}

bool Oot3dPicaPresentationScheduler::HasSnapshot(
    const Oot3dPicaDisplayTransferSubmission &transfer,
    uint64_t renderTargetNamespace) const {
  const auto found = mSnapshotCompletions.find(
      MakeSnapshotTarget(transfer, renderTargetNamespace));
  return found != mSnapshotCompletions.end() &&
         found->second == transfer.CompletionId;
}

void Oot3dPicaPresentationScheduler::SetTimingEnabled(bool enabled) noexcept {
  if (mTimings.Enabled == enabled) {
    return;
  }
  mTimings = {};
  mTimings.Enabled = enabled;
}

void Oot3dPicaPresentationScheduler::Reset() {
  const bool timingEnabled = mTimings.Enabled;
  mAccumulator = {};
  mPresentationSerial.reset();
  mExecutedDrawsThisPresentation.clear();
  mCompositionDrawScratch.clear();
  mSnapshotCompletions.clear();
  mTimings = {};
  mTimings.Enabled = timingEnabled;
  ++mStats.Resets;
}

size_t Oot3dPicaPresentationScheduler::PendingDrawCount() const {
  return mAccumulator.PendingDrawCount();
}

bool Oot3dPicaPresentationScheduler::HasPendingRawCopyCompletion() const {
  return mAccumulator.HasPendingRawCopyCompletion();
}

const Oot3dPicaPresentationSchedulerStats &
Oot3dPicaPresentationScheduler::Stats() const noexcept {
  return mStats;
}

const Oot3dPicaPresentationSchedulerTimings &
Oot3dPicaPresentationScheduler::Timings() const noexcept {
  return mTimings;
}

Oot3dPicaPresentationScheduler::SnapshotTarget
Oot3dPicaPresentationScheduler::MakeSnapshotTarget(
    const Oot3dPicaDisplayTransferSubmission &transfer,
    uint64_t renderTargetNamespace) {
  return {renderTargetNamespace, transfer.OutputPhysicalAddress};
}

} // namespace Oot3dNativeGame
