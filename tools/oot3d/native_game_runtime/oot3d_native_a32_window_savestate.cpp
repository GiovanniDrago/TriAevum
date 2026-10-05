// Native A32 window session: savestate save, load and the restore of renderer and title state.
#include "oot3d_native_a32_window_session.h"

namespace Oot3dA32Window {

void NativeA32WindowSession::RestoreSavestateTiming(const Oot3dNativeGame::NativeA32SavestateRuntimeState &runtime) {
  auto &frameRatePolicy = *frameRatePolicyStorage;
  auto &presentationScheduler = *presentationSchedulerStorage;
  if (!runtime.GameplayClockAvailable) {
    frameRatePolicy.ResetGameplayClock();
    ++savestateStats.LegacyClockRestoreCount;
    std::cerr
        << "oot3d_native_game: legacy savestate has no gameplay clock; "
           "starting at a quiescent logical-frame boundary\n";
  } else {
    if (!frameRatePolicy.RestoreGameplayClock(
            runtime.GameplayTiming, runtime.GameplayClock)) {
      throw std::runtime_error(
          "savestate gameplay clock restore failed: " +
          frameRatePolicy.LastError());
    }
  }
  if (!runtime.PresentationSchedulerAvailable) {
    presentationScheduler.Reset();
    ++savestateStats.LegacyPresentationSchedulerRestoreCount;
  } else if (!presentationScheduler.Restore(
                 runtime.PresentationScheduler, &error)) {
    throw std::runtime_error(
        "savestate presentation scheduler restore failed: " + error);
  }
  if (!runtime.FrameRatePolicyTemporalStateAvailable) {
    frameRatePolicy.ResetTemporalState();
    ++savestateStats.LegacyFrameRatePolicyRestoreCount;
  } else if (!frameRatePolicy.RestoreTemporalState(
                 runtime.FrameRatePolicyTemporalState, &error)) {
    throw std::runtime_error(
        "savestate frame-rate policy restore failed: " + error);
  }
}

void NativeA32WindowSession::RestoreSavestateTopScreenTemporalState(const Oot3dNativeGame::NativeA32SavestateRuntimeState &runtime) {
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  if (launch.UiProfile !=
      Oot3dNativeGame::Oot3dUiProfile::TopScreen) {
    return;
  }
  if (!runtime.TopScreenTemporalStateAvailable ||
      !runtime.TopScreenTemporalState.ProfileActive) {
    ++savestateStats.LegacyTopScreenTemporalRestoreCount;
    return;
  }
  const auto &state = runtime.TopScreenTemporalState;
  if (state.ProjectionAvailable) {
    uiLifecycleBridge.TopScreenPauseProjection() = state.Projection;
    uiLifecycleBridge.TopScreenPauseProjection().AlternatePage =
        activeTopScreenConfig.MinimapVisible;
  }
  widescreenProjection.TopScreenPausePageRedrawActive =
      state.PausePageRedrawActive;
  widescreenProjection.TopScreenPausePageRedraw.DelayCalls =
      state.PausePageRedrawDelayCalls;
  widescreenProjection.TopScreenPauseDrawRouting
      .NativeTransitionLatched =
      state.PauseDrawNativeTransitionLatched;
  widescreenProjection.TopScreenPauseDrawRouting
      .SuppressionDelayArmed =
      state.PauseDrawSuppressionDelayArmed;
  widescreenProjection.TopScreenPauseDrawRouting
      .SuppressionDelayCommands =
      state.PauseDrawSuppressionDelayCommands;
  nativeCandidateDispatch.TopScreenPauseRoute.PreviousRuntimeMode =
      state.PauseRoutePreviousRuntimeMode;
  nativeCandidateDispatch.TopScreenPauseRoute.TransitionPhase =
      state.PauseRouteTransitionPhase;
  nativeCandidateDispatch.TopScreenPauseRoute.RemainingCalls =
      state.PauseRouteRemainingCalls;
  nativeCandidateDispatch.TopScreenAotPauseRoute.PreviousRuntimeMode =
      state.AotPauseRoutePreviousRuntimeMode;
  nativeCandidateDispatch.TopScreenAotPauseRoute.TransitionPhase =
      state.AotPauseRouteTransitionPhase;
  nativeCandidateDispatch.TopScreenAotPauseRoute.RemainingCalls =
      state.AotPauseRouteRemainingCalls;
  nativeCandidateDispatch.TopScreenTouchCoordinateRoute
      .RuntimeSceneLatch = state.TouchCoordinateRuntimeSceneLatch;
}

void NativeA32WindowSession::RestoreSavestatePicaTextureCache(const Oot3dNativeGame::NativeA32SavestateRuntimeState &runtime) {
  auto &api = *apiPointer;
  if (!runtime.PicaTextureCacheAvailable) {
    ++savestateStats.LegacyPicaTextureCacheRestoreCount;
    return;
  }
  if (!api.RestorePicaTextureCache(
          runtime.PicaTextureCache, &error)) {
    throw std::runtime_error(
        "savestate native PICA texture restore failed: " + error);
  }
  for (const auto &texture : runtime.PicaTextureCache) {
    savestateStats.PicaTextureBytesRestored +=
        texture.PixelBytes.size();
  }
  savestateStats.PicaTexturesRestored +=
      runtime.PicaTextureCache.size();
}

void NativeA32WindowSession::RestoreSavestatePicaColorTargets(const Oot3dNativeGame::NativeA32SavestateRuntimeState &runtime) {
  auto &submissionQueue = *submissionQueueStorage;
  auto &api = *apiPointer;
  if (!runtime.PicaColorTargetsAvailable) {
    ++savestateStats.LegacyPicaColorTargetRestoreCount;
    return;
  }
  if (!api.RestorePicaColorTargets(
          runtime.PicaColorTargets, &error)) {
    throw std::runtime_error(
        "savestate native PICA color restore failed: " + error);
  }
  std::vector<uint32_t> restoredAddresses;
  restoredAddresses.reserve(runtime.PicaColorTargets.size());
  for (const auto &target : runtime.PicaColorTargets) {
    restoredAddresses.push_back(target.ColorPhysicalAddress);
    savestateStats.PicaColorTargetBytesRestored +=
        target.ColorRgba8.size();
  }
  submissionQueue.RestoreGpuColorRenderTargetOwnership(
      restoredAddresses);
  savestateStats.PicaColorTargetsRestored += restoredAddresses.size();
}

void NativeA32WindowSession::RestoreSavestatePicaPresentationState(const Oot3dNativeGame::NativeA32SavestateRuntimeState &runtime) {
  auto &api = *apiPointer;
  if (!runtime.PicaPresentationStateAvailable) {
    ++savestateStats.LegacyPicaPresentationRestoreCount;
    return;
  }
  if (!api.RestorePicaPresentationState(
          runtime.PicaPresentationState, &error)) {
    throw std::runtime_error(
        "savestate native PICA presentation restore failed: " +
        error);
  }
  for (const auto &image :
       runtime.PicaPresentationState.DisplayImages) {
    savestateStats.PicaDisplayImageBytesRestored +=
        image.ColorRgba8.size();
  }
  savestateStats.PicaDisplayImagesRestored +=
      runtime.PicaPresentationState.DisplayImages.size();
}

void NativeA32WindowSession::RestoreSavestatePicaVisualReplayState(const Oot3dNativeGame::NativeA32SavestateRuntimeState &runtime) {
  if (!runtime.PicaVisualReplayStateAvailable) {
    picaPresentationScheduler.Reset();
    visualContinuityTracker = {};
    preparedVisualTransition.Reset();
    latestVisualFrame.reset();
    previousVisualFrame.reset();
    latestVisualTransitionContinuous = false;
    visualSampleCadence.Reset();
    displayTransfersByOutput.clear();
    lastSubmittedDrawId = 0U;
    lastSelectedTopTransferCompletionId = 0U;
    ++visualContinuityEpoch;
    ++savestateStats.LegacyPicaVisualReplayRestoreCount;
    return;
  }
  Oot3dNativeGame::Oot3dPicaVisualReplayState state;
  if (!Oot3dNativeGame::DecodeOot3dPicaVisualReplayState(
          runtime.PicaVisualReplayState, state, &error) ||
      !picaPresentationScheduler.RestoreState(
          std::move(state.Scheduler)) ||
      !visualContinuityTracker.RestoreState(
          std::move(state.Continuity))) {
    throw std::runtime_error(
        "savestate native PICA visual replay restore failed: " +
        error);
  }
  previousVisualFrame = std::move(state.PreviousFrame);
  latestVisualFrame = std::move(state.LatestFrame);
  latestVisualTransitionContinuous =
      state.LatestTransitionContinuous;
  preparedVisualTransition.Reset();
  visualSampleCadence.Reset();
  if (latestVisualTransitionContinuous &&
      previousVisualFrame.has_value() &&
      latestVisualFrame.has_value()) {
    latestVisualTransitionContinuous =
        preparedVisualTransition.Prepare(*previousVisualFrame,
                                         *latestVisualFrame);
  }
  ++visualContinuityEpoch;
  displayTransfersByOutput =
      std::move(state.DisplayTransfersByOutput);
  lastSelectedTopTransferCompletionId =
      state.LastSelectedTopTransferCompletionId;
  lastSubmittedDrawId = state.LastSubmittedDrawId;
  savestateStats.PicaVisualReplayBytesRestored +=
      runtime.PicaVisualReplayState.size();
}

NativeA32WindowSession::SaveStateOutcome
NativeA32WindowSession::FailSave(std::string message) {
  std::cerr << "oot3d_native_game: " << message << '\n';
  ++savestateStats.SaveFailureCount;
  lastSavestateMessage = std::move(message);
  return SaveStateOutcome::Failed;
}

NativeA32WindowSession::SaveStateOutcome
NativeA32WindowSession::SaveState(const std::filesystem::path &path, bool withSettings) {
  auto &hostServices = *hostServicesStorage;
  auto &frameRatePolicy = *frameRatePolicyStorage;
  auto &process = *processStorage;
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  auto &submissionQueue = *submissionQueueStorage;
  auto &api = *apiPointer;
  auto &ocarinaText = *ocarinaTextStorage;
  auto &dspHle = *dspHleStorage;
  auto &presentationScheduler = *presentationSchedulerStorage;
  // Barrier: the savestate captures renderer-owned replay state.
  renderStream.Drain();
  // Host-owned continuation registers must never escape into a standalone savestate.
  if (ocarinaText.Pending()) return SaveStateOutcome::Deferred;
  const auto started = std::chrono::steady_clock::now();
  Oot3dNativeGame::NativeA32SavestateRuntimeState runtime;
  runtime.FrameCount = frameCount;
  runtime.RefreshTickRemainder = refreshTickRemainder;
  runtime.NextVblankTick = nextVblankTick;
  runtime.ProcessRunKind = static_cast<uint32_t>(processResult.Kind);
  runtime.GameplayClockAvailable = true;
  runtime.GameplayTiming = frameRateContract.Mode;
  runtime.GameplayClock =
      frameRatePolicy.GameplayClock().CaptureState();
  runtime.PresentationSchedulerAvailable = true;
  runtime.PresentationScheduler = presentationScheduler.CaptureState();
  runtime.FrameRatePolicyTemporalStateAvailable = true;
  runtime.FrameRatePolicyTemporalState =
      frameRatePolicy.CaptureTemporalState();
  runtime.TopScreenTemporalStateAvailable = true;
  auto &topScreenState = runtime.TopScreenTemporalState;
  topScreenState.ProjectionAvailable = true;
  topScreenState.Projection = uiLifecycleBridge.TopScreenPauseProjection();
  topScreenState.ProfileActive =
      launch.UiProfile == Oot3dNativeGame::Oot3dUiProfile::TopScreen;
  topScreenState.PausePageRedrawActive =
      widescreenProjection.TopScreenPausePageRedrawActive;
  topScreenState.PausePageRedrawDelayCalls =
      widescreenProjection.TopScreenPausePageRedraw.DelayCalls;
  topScreenState.PauseDrawNativeTransitionLatched =
      widescreenProjection.TopScreenPauseDrawRouting
          .NativeTransitionLatched;
  topScreenState.PauseDrawSuppressionDelayArmed =
      widescreenProjection.TopScreenPauseDrawRouting.SuppressionDelayArmed;
  topScreenState.PauseDrawSuppressionDelayCommands =
      widescreenProjection.TopScreenPauseDrawRouting
          .SuppressionDelayCommands;
  topScreenState.PauseRoutePreviousRuntimeMode =
      nativeCandidateDispatch.TopScreenPauseRoute.PreviousRuntimeMode;
  topScreenState.PauseRouteTransitionPhase =
      nativeCandidateDispatch.TopScreenPauseRoute.TransitionPhase;
  topScreenState.PauseRouteRemainingCalls =
      nativeCandidateDispatch.TopScreenPauseRoute.RemainingCalls;
  topScreenState.AotPauseRoutePreviousRuntimeMode =
      nativeCandidateDispatch.TopScreenAotPauseRoute.PreviousRuntimeMode;
  topScreenState.AotPauseRouteTransitionPhase =
      nativeCandidateDispatch.TopScreenAotPauseRoute.TransitionPhase;
  topScreenState.AotPauseRouteRemainingCalls =
      nativeCandidateDispatch.TopScreenAotPauseRoute.RemainingCalls;
  topScreenState.TouchCoordinateRuntimeSceneLatch =
      nativeCandidateDispatch.TopScreenTouchCoordinateRoute
          .RuntimeSceneLatch;
  Oot3dNativeGame::Oot3dPicaVisualReplayState visualReplayState;
  visualReplayState.Scheduler =
      picaPresentationScheduler.CaptureState();
  visualReplayState.Continuity =
      visualContinuityTracker.CaptureState();
  visualReplayState.PreviousFrame = previousVisualFrame;
  visualReplayState.LatestFrame = latestVisualFrame;
  visualReplayState.LatestTransitionContinuous =
      latestVisualTransitionContinuous;
  visualReplayState.DisplayTransfersByOutput =
      displayTransfersByOutput;
  visualReplayState.LastSelectedTopTransferCompletionId =
      lastSelectedTopTransferCompletionId;
  visualReplayState.LastSubmittedDrawId = lastSubmittedDrawId;
  if (!Oot3dNativeGame::EncodeOot3dPicaVisualReplayState(
          visualReplayState, runtime.PicaVisualReplayState, &error)) {
    return FailSave("native PICA visual replay capture failed: " + error);
  }
  runtime.PicaVisualReplayStateAvailable = true;
  savestateStats.PicaVisualReplayBytesCaptured +=
      runtime.PicaVisualReplayState.size();
  if (!api.CapturePicaTextureCache(
          runtime.PicaTextureCache, &error)) {
    return FailSave("native PICA texture capture failed: " + error);
  }
  runtime.PicaTextureCacheAvailable = true;
  for (const auto &texture : runtime.PicaTextureCache) {
    savestateStats.PicaTextureBytesCaptured += texture.PixelBytes.size();
  }
  savestateStats.PicaTexturesCaptured += runtime.PicaTextureCache.size();
  if (!api.CapturePicaColorTargets(
          runtime.PicaColorTargets, &error)) {
    return FailSave("native PICA color capture failed: " + error);
  }
  runtime.PicaColorTargetsAvailable = true;
  for (const auto &target : runtime.PicaColorTargets) {
    savestateStats.PicaColorTargetBytesCaptured +=
        target.ColorRgba8.size();
  }
  savestateStats.PicaColorTargetsCaptured += runtime.PicaColorTargets.size();
  if (!api.CapturePicaPresentationState(
          runtime.PicaPresentationState, &error)) {
    return FailSave("native PICA presentation capture failed: " + error);
  }
  runtime.PicaPresentationStateAvailable = true;
  for (const auto &image :
       runtime.PicaPresentationState.DisplayImages) {
    savestateStats.PicaDisplayImageBytesCaptured +=
        image.ColorRgba8.size();
  }
  savestateStats.PicaDisplayImagesCaptured +=
      runtime.PicaPresentationState.DisplayImages.size();
  if (withSettings) {
    runtime.HostSettingsAvailable = true;
    runtime.HostSettingsJson =
        Oot3dNativeGame::CaptureSavestateSettings(SavestateSettingsOwners());
  }
  Oot3dNativeGame::NativeA32SavestateIoResult ioResult;
  if (!Oot3dNativeGame::SaveNativeA32State(
          path, savestateCompatibility, runtime, process, hostServices,
          picaFrontend, submissionQueue, dspHle, &ioResult, &error)) {
    return FailSave("savestate save failed: " + error);
  }
  ++savestateStats.SaveCount;
  savestateStats.SaveBytes += ioResult.FileBytes;
  savestateStats.LastSemanticFingerprint = ioResult.SemanticFingerprint;
  savestateStats.SaveSeconds += SecondsSince(started);
  savestateStats.CaptureSeconds += ioResult.CaptureSeconds;
  savestateStats.EncodeSeconds += ioResult.EncodeSeconds;
  savestateStats.WriteSeconds += ioResult.IoSeconds;
  std::cout << "oot3d_native_game: saved state at frame " << frameCount
            << " to " << path.string() << '\n';
  lastSavestateMessage.clear();
  return SaveStateOutcome::Saved;
}

bool NativeA32WindowSession::LoadState(const std::filesystem::path &path,
                                       bool applySettings) {
  auto &hostServices = *hostServicesStorage;
  auto &process = *processStorage;
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  auto &submissionQueue = *submissionQueueStorage;
  auto &api = *apiPointer;
  auto &ocarinaText = *ocarinaTextStorage;
  auto &n64UiRenderer = *n64UiRendererStorage;
  auto &dspHle = *dspHleStorage;
  auto &realtimePacer = *realtimePacerStorage;
  // Barrier: loading resets renderer-owned state.
  renderStream.Drain();
  const auto started = std::chrono::steady_clock::now();
  Oot3dNativeGame::NativeA32SavestateRuntimeState runtime;
  Oot3dNativeGame::NativeA32SavestateIoResult ioResult;
  if (!Oot3dNativeGame::LoadNativeA32State(
          path, savestateCompatibility, runtime, process, hostServices,
          picaFrontend, submissionQueue, dspHle, &ioResult, &error)) {
    std::cerr << "oot3d_native_game: savestate load failed: " << error
              << '\n';
    lastSavestateMessage = error;
    return false;
  }
  // Settings first: the renderer restore below rebuilds the saved content at
  // the scale they select.
  ApplyLoadedSavestateSettings(runtime, applySettings);
  Oot3dNativeGame::ResetOot3dTypedGameplayTransientState();
  picaCompositionTracker.Reset();
  const auto restoredGravity = hostServices.HidRuntimeProfile().LastAccelerometer;
  nativeControlPollingState.VirtualMotion.RestoreGravity(
      {static_cast<float>(restoredGravity[0]), static_cast<float>(restoredGravity[1]),
       static_cast<float>(restoredGravity[2])});
  nativeControlPollingState.PendingMouseDeltaX = 0;
  nativeControlPollingState.PendingMouseDeltaY = 0;
  nativeControlPollingState.PendingInputSeconds = 0.0;
  ApplySelectedUiProfile();
  if (!api.ResetPicaState(&error)) {
    throw std::runtime_error(
        "native PICA reset after savestate load failed: " + error);
  }
  if (titleRenderBackend != nullptr &&
      !titleRenderBackend->ResetTitleState(&error)) {
    throw std::runtime_error(
        "OOT3D renderer reset after savestate load failed: " + error);
  }
  RestoreSavestatePicaTextureCache(runtime);
  RestoreSavestatePicaColorTargets(runtime);
  RestoreSavestatePicaPresentationState(runtime);
  if (audioPlayer != nullptr && audioPlayer->IsInitialized()) {
    audioPlayer->Flush();
  }
  frameCount = runtime.FrameCount;
  refreshTickRemainder = runtime.RefreshTickRemainder;
  nextVblankTick = runtime.NextVblankTick;
  processResult = {};
  processResult.Kind = static_cast<Oot3dNativeGame::NativeA32ProcessRunKind>(
      runtime.ProcessRunKind);
  RestoreSavestateTiming(runtime);
  picaFrontend.SetPacketSink(&submissionQueue);
  uiLifecycleBridge.ResetAfterStateLoad(frameCount);
  hdFonts.Reset();
  ocarinaText.Reset();
  uiLifecycleBridge.SetTopScreenConfig(activeTopScreenConfig);
  widescreenProjection.TopScreenPauseDrawRouting = {};
  widescreenProjection.TopScreenPausePageRedraw = {};
  widescreenProjection.TopScreenPausePageRedrawActive = false;
  widescreenProjection.TopScreenPausePageCompositionCalls = 0U;
  widescreenProjection.TopScreenPausePageRedraws = 0U;
  widescreenProjection.TopScreenPausePageRedrawSkips = 0U;
  widescreenProjection.PendingTopScreenPauseChildRestore.reset();
  nativeCandidateDispatch.TopScreenInput = {};
  nativeCandidateDispatch.TopScreenInputClock.Reset();
  nativeCandidateDispatch.TopScreenGameplayActionRuntime = {};
  nativeCandidateDispatch.PreviousTopScreenButtons = 0U;
  nativeCandidateDispatch.StartButtonLatch = {};
  nativeCandidateDispatch.TopScreenStartRouting = {};
  nativeCandidateDispatch.TopScreenPauseSystemOpen = {};
  nativeCandidateDispatch.TopScreenViewportDrawPhase = 0U;
  nativeCandidateDispatch.TopScreenViewportDrawArgument = 0U;
  nativeCandidateDispatch.TopScreenSceneViewportDrawPhase = 0U;
  nativeCandidateDispatch.TopScreenSceneViewportDrawArgument = 0U;
  nativeCandidateDispatch.TopScreenOverlayViewportDrawPhase = 0U;
  nativeCandidateDispatch.TopScreenTouchCoordinateUpdatePhase = 0U;
  nativeCandidateDispatch.TopScreenAlternateRendererRestore.reset();
  nativeCandidateDispatch.TopScreenPauseRoute = {};
  nativeCandidateDispatch.TopScreenTouchCoordinateRoute = {};
  nativeCandidateDispatch.TopScreenRendererVisibilityRoute = {};
  nativeCandidateDispatch.TopScreenGameOverDrawPhase = 0U;
  nativeCandidateDispatch.TopScreenPauseController = {};
  nativeCandidateDispatch.TopScreenPauseControllerDrawPhase = 0U;
  nativeCandidateDispatch.TopScreenPauseControllerVisibleRenderers = {};
  nativeCandidateDispatch.TopScreenItems.PendingSelection = 0U;
  nativeCandidateDispatch.TopScreenItems.PreviousSelectionButtons = 0U;
  nativeCandidateDispatch.TopScreenItems.PreviousSelectionPressed = 0U;
  nativeCandidateDispatch.TopScreenCamera = {};
  nativeCandidateDispatch.TopScreenCameraInput.Reset();
  Oot3dNativeGame::ApplyTopScreenFreeCameraConfig(
      nativeCandidateDispatch.TopScreenCamera.Camera,
      activeTopScreenConfig);
  nativeCandidateDispatch.TopScreenExtendedCameraActive = false;
  RestoreSavestateTopScreenTemporalState(runtime);
  sceneViewProbe.Reset();
  sceneEnvironment->ObserveScene(std::nullopt);
  Fast::Oot3d::GrassInteractionBridge::Instance().Reset();
  temporalEventLedger.Reset();
  lastPublishedSceneViewSerial = 0;
  n64UiRenderer.Release();
  picaPresentationScheduler.Reset();
  visualContinuityTracker = {};
  preparedVisualTransition.Reset();
  latestVisualFrame.reset();
  previousVisualFrame.reset();
  latestVisualTransitionContinuous = false;
  visualFrameSampleScratch = {};
  displayTransfersByOutput.clear();
  drainedDraws.clear();
  lastSubmittedDrawId = 0;
  lastSelectedTopTransferCompletionId = 0;
  ++visualContinuityEpoch;
  RestoreSavestatePicaVisualReplayState(runtime);
  pcmContinuityDiagnostics = {};
  realtimePacer.ResetDeadline();
  lastPresentationTime = std::chrono::steady_clock::now();
  ++savestateStats.LoadCount;
  savestateStats.LoadBytes += ioResult.FileBytes;
  savestateStats.LoadSeconds += SecondsSince(started);
  savestateStats.ReadSeconds += ioResult.IoSeconds;
  savestateStats.DecodeSeconds += ioResult.DecodeSeconds;
  savestateStats.RestoreSeconds += ioResult.RestoreSeconds;
  std::cout << "oot3d_native_game: loaded state at frame " << frameCount
            << " from " << path.string() << '\n';
  return true;
}

void NativeA32WindowSession::ApplyLoadedSavestateSettings(
    const Oot3dNativeGame::NativeA32SavestateRuntimeState &runtime,
    bool applySettings) {
  lastSavestateMessage.clear();
  if (!applySettings) return;
  if (!runtime.HostSettingsAvailable) {
    lastSavestateMessage = "this state has no saved settings; current settings kept";
    std::cout << "oot3d_native_game: savestate settings: none stored\n";
    return;
  }
  const auto applied = Oot3dNativeGame::ApplySavestateSettings(
      runtime.HostSettingsJson, SavestateSettingsOwners());
  for (const auto &line : applied.Lines) {
    std::cout << "oot3d_native_game: savestate settings: " << line << '\n';
    lastSavestateMessage += (lastSavestateMessage.empty() ? "" : "\n") + line;
  }
  if (topScreenConfigRuntime != nullptr) ApplyTopScreenConfigSnapshot();
}

Oot3dNativeGame::SavestateSettingsOwners
NativeA32WindowSession::SavestateSettingsOwners() const {
  return {launch.UiProfile == Oot3dNativeGame::Oot3dUiProfile::TopScreen
              ? topScreenConfigRuntime
              : nullptr,
          controlConfigRuntime, languageSettings};
}

void NativeA32WindowSession::ServiceSavestateRequests() {
  auto request = savestateLibrary->TakeRequest();
  if (!request.has_value()) return;
  const auto report = [&](bool success, std::string message) {
    std::cout << "oot3d_native_game: savestate request "
              << (success ? "succeeded" : "failed") << ": " << message << '\n';
    Fast::Oot3d::PostHostNotice(message, !success);
    savestateLibrary->Report(success, std::move(message));
  };
  const auto fileName = [](const std::filesystem::path &path) {
    const auto text = path.filename().u8string();
    return std::string(text.begin(), text.end());
  };
  if (request->Kind == Oot3dNativeGame::SavestateRequestKind::Save) {
    const auto path = savestateLibrary->NewStatePath(request->WithSettings);
    std::error_code directoryError;
    std::filesystem::create_directories(path.parent_path(), directoryError);
    if (directoryError) {
      report(false, "Save state failed: cannot create the folder (" +
                        directoryError.message() + ")");
      return;
    }
    switch (SaveState(path, request->WithSettings)) {
    case SaveStateOutcome::Deferred:
      savestateLibrary->Request(*request);  // Retried at the next boundary.
      return;
    case SaveStateOutcome::Failed:
      report(false, "Save state failed: " + lastSavestateMessage);
      return;
    case SaveStateOutcome::Saved:
      savestateLibrary->SetLastState(path);
      report(true, std::string(request->WithSettings ? "State and settings saved: "
                                                     : "State saved: ") +
                       fileName(path));
      return;
    }
    return;
  }
  const auto path = request->Path.empty() ? savestateLibrary->LastState()
                                          : std::optional(request->Path);
  if (!path.has_value()) {
    report(false, "No save state to load yet");
    return;
  }
  std::error_code existsError;
  if (!std::filesystem::is_regular_file(*path, existsError)) {
    report(false, "Save state not found: " + fileName(*path));
    return;
  }
  if (!LoadState(*path, request->WithSettings)) {
    report(false, "Load failed: " + lastSavestateMessage);
    return;
  }
  savestateLibrary->SetLastState(*path);
  std::string message = (request->WithSettings ? "State and settings loaded: "
                                               : "State loaded: ") +
                        fileName(*path);
  if (!lastSavestateMessage.empty()) message += "\n" + lastSavestateMessage;
  report(true, std::move(message));
}

} // namespace Oot3dA32Window
