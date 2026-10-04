#include "oot3d_native_a32_window.h"
#include "three_ds_sdl_controller.h"
#include "oot3d_prebackend_budget.h"
#include "oot3d_cpu_phase_probe.h"
#include "fast/renderer/frame_time_distribution.h"
#include "fast/renderer/slow_frame_samples.h"
#include "fast/renderer3ds/pica_program_preparation.h"
#include "fast/renderer3ds/slow_step_trace.h"
#include "oot3d_native_preparation_screen.h"
#include "oot3d_game_language_panel.h"
#include "oot3d_graphics_settings_schedule.h"
#include "oot3d_scene_environment.h"
#ifdef OOT3D_WHOLE_AOT_PRODUCT_MODE
#include "oot3d_native_crash_diagnostics.h"
#endif

#include "oot3d_a32_provenance.h"
#include "oot3d_demo_host_context.h"
#include "oot3d_demo_host_io.h"
#include "oot3d_demo_host_screenshot.h"
#include "oot3d_demo_host_window_timing.h"
#include "oot3d_legacy_ui_adapters.h"
#include "oot3d_n64_ui_renderer.h"
#include "oot3d_native_a32_ctr_host.h"
#include "oot3d_native_a32_dsp_hle.h"
#include "oot3d_native_a32_dsp_memory_adapter.h"
#include "oot3d_native_a32_process_image.h"
#include "oot3d_native_a32_registry.h"
#include "oot3d_native_a32_savestate.h"
#include "oot3d_native_a32_scene_view.h"
#include "oot3d_native_actor_interactions.h"
#include "fast/oot3d/grass_interaction_bridge.h"
#include "oot3d_native_a32_timing_probe.h"
#include "oot3d_native_compiled_functions.h"
#include "oot3d_native_controls_settings_panel.h"
#include "oot3d_native_frame_rate.h"
#include "oot3d_native_game_bootstrap.h"
#include "oot3d_native_mass_aot.h"
#include "oot3d_native_pcm_diagnostics.h"
#include "oot3d_native_presentation_pacer.h"
#include "oot3d_native_pica_frontend.h"
#include "oot3d_native_pica_composition.h"
#include "oot3d_native_pica_presentation_scheduler.h"
#include "oot3d_native_render_command_stream.h"
#include "oot3d_native_pica_semantic_trace.h"
#include "oot3d_native_pica_submission.h"
#include "oot3d_native_pica_visual_frame.h"
#include "oot3d_native_pica_visual_savestate.h"
#include "oot3d_native_pica_vulkan_bridge.h"
#include "oot3d_native_pica_vulkan_plan.h"
#include "oot3d_native_player_temporal_bridge.h"
#include "oot3d_native_scenario_bootstrap.h"
#include "oot3d_native_temporal_event_ledger.h"
#include "oot3d_native_true_aot_blocks.h"
#include "oot3d_native_ui_lifecycle_bridge.h"
#include "oot3d_native_ui_texture_provider.h"
#include "oot3d_top_screen_ocarina_text_runtime.h"
#include "oot3d_native_whole_aot_runtime.h"
#ifdef OOT3D_NATIVE_DIRECT_AOT_PLUGIN
#include "oot3d_native_direct_aot.h"
#endif
#include "oot3d_top_screen_camera_guest.h"
#include "oot3d_top_screen_controls.h"
#include "oot3d_top_screen_frontend_composition.h"
#include "oot3d_top_screen_gameplay_actions.h"
#include "oot3d_top_screen_gameplay_action_consumer.h"
#include "oot3d_top_screen_item_guest.h"
#include "oot3d_top_screen_item_dispatch.h"
#include "oot3d_top_screen_input_cadence.h"
#include "oot3d_top_screen_items_hint.h"
#include "oot3d_top_screen_settings_panel.h"
#include "oot3d_top_screen_texture_overrides.h"
#include "oot3d_native_hd_font_runtime.h"
#include "oot3d_top_screen_texture_runtime.h"
#include "oot3d_typed_gameplay_bridge.h"

#include "fast/Fast3dGui.h"
#include "fast/Fast3dWindow.h"
#include "fast/oot3d/graphics_settings_persistence.h"
#include "fast/oot3d/graphics_settings_runtime.h"
#include "fast/oot3d/graphics_settings_window.h"
#include "fast/oot3d/perspective_fov_policy.h"
#include "fast/oot3d/presentation_pacing_policy.h"
#include "fast/oot3d/title_render_backend.h"
#include "ship/Context.h"
#include "ship/audio/Audio.h"
#include "ship/audio/AudioPlayer.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/controller/controldevice/controller/mapping/keyboard/KeyboardScancodes.h"
#include "ship/controller/physicaldevice/ConnectedPhysicalDeviceManager.h"
#include "ship/controller/physicaldevice/GlobalSDLDeviceSettings.h"
#include "ship/window/gui/Gui.h"

#include <SDL2/SDL.h>
#include <imgui.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include "oot3d_native_a32_window_shared.h"

using namespace Oot3dA32Window;

namespace {
namespace CpuProbe = Oot3dNativeGame::CpuPhaseProbe;

#if defined(__SWITCH__)
size_t gSwitchBlockHookCount = 0U;
bool gSwitchBlockHooksSorted = false;
bool gSwitchBlockHooksContainProcessEntry = false;
uint64_t gSwitchRuntimeHostFrames = 0U;
uint32_t gSwitchRuntimeGuestFrames = 0U;
std::string gSwitchUiProfile = "unknown";
std::string gSwitchGameplayTiming = "unknown";
std::string gSwitchControlProfile = "unknown";
uint32_t gSwitchPresentationRateHz = 0U;
bool gSwitchVisualInterpolation = false;
bool gSwitchTopScreen211Assets = false;
uint32_t gSwitchWholeAotBlockBudget = 0U;
uint64_t gSwitchWholeAotBlockLimitExits = 0U;
bool gSwitchPicaGeometryCacheEnabled = false;
uint64_t gSwitchPicaGeometryPersistentDraws = 0U;
uint64_t gSwitchPicaGeometryPersistentUploads = 0U;
uint64_t gSwitchPicaGeometryRegistryEntries = 0U;
bool gSwitchPicaGeometryCacheObserved = false;

void WriteSwitchBootStage(const char* stage) {
  std::ofstream stream("sdmc:/switch/oot3dre/boot-status.txt",
                       std::ios::trunc);
  if (!stream) {
    return;
  }
  stream << "status=launching\n"
         << "stage=" << stage << '\n'
         << "whole_aot_functions=12419\n"
         << "host_boundaries=3\n"
         << "residual_a32=0\n"
         << "block_hook_count=" << gSwitchBlockHookCount << '\n'
         << "block_hooks_sorted=" << (gSwitchBlockHooksSorted ? 1 : 0)
         << '\n'
         << "block_hooks_contain_process_entry="
         << (gSwitchBlockHooksContainProcessEntry ? 1 : 0) << '\n'
         << "host_frames=" << gSwitchRuntimeHostFrames << '\n'
         << "guest_frames=" << gSwitchRuntimeGuestFrames << '\n'
         << "ui_profile=" << gSwitchUiProfile << '\n'
         << "gameplay_timing=" << gSwitchGameplayTiming << '\n'
         << "presentation_rate_hz=" << gSwitchPresentationRateHz << '\n'
         << "visual_interpolation=" << (gSwitchVisualInterpolation ? 1 : 0)
         << '\n'
         << "control_profile=" << gSwitchControlProfile << '\n'
         << "topscreen_211_assets=" << (gSwitchTopScreen211Assets ? 1 : 0)
         << '\n'
         << "whole_aot_block_budget=" << gSwitchWholeAotBlockBudget << '\n'
         << "whole_aot_block_limit_exits="
         << gSwitchWholeAotBlockLimitExits << '\n'
         << "pica_geometry_cache_enabled="
         << (gSwitchPicaGeometryCacheEnabled ? 1 : 0) << '\n'
         << "pica_geometry_persistent_draws="
         << gSwitchPicaGeometryPersistentDraws << '\n'
         << "pica_geometry_persistent_uploads="
         << gSwitchPicaGeometryPersistentUploads << '\n'
         << "pica_geometry_registry_entries="
         << gSwitchPicaGeometryRegistryEntries << '\n';
}

#endif


uint64_t HashDiagnosticBytes(std::span<const uint8_t> bytes) noexcept {
  uint64_t hash = 14695981039346656037ULL;
  for (const uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

bool IsNativeFrontendOpaqueTargetInitializationDraw(
    const Oot3dNativeGame::Oot3dPicaDrawSubmission &draw) {
  const auto &state = draw.State;
  const auto &blend = state.OutputMerger.Blend;
  const auto &depth = state.OutputMerger.Depth;
  const bool hasTexture =
      std::any_of(state.Textures.begin(), state.Textures.end(),
                  [](const auto &texture) { return texture.Enabled; });
  // Native UI targets include rotated/padded storage outside the logical
  // viewport, so framebuffer dimensions cannot establish whether this copy
  // quad initializes the frontend canvas. The replacement blend/depth state
  // identifies that operation; its association with CommonBackground00's
  // target AND viewport is checked by the caller, preserving the scene clear.
  return !hasTexture && state.VertexInput.Indexed &&
         state.VertexInput.VertexCount == 4U &&
         state.OutputMerger.ColorWriteMask == 0x0FU &&
         state.OutputMerger.LogicOperation ==
             Oot3dNativeGame::Oot3dPicaLogicOperation::Copy &&
         blend.Enabled &&
         blend.SourceColor == Oot3dNativeGame::Oot3dPicaBlendFactor::One &&
         blend.DestinationColor ==
             Oot3dNativeGame::Oot3dPicaBlendFactor::Zero &&
         blend.SourceAlpha == Oot3dNativeGame::Oot3dPicaBlendFactor::One &&
         blend.DestinationAlpha ==
             Oot3dNativeGame::Oot3dPicaBlendFactor::Zero &&
         depth.TestEnabled && depth.WriteEnabled &&
         depth.Compare == Oot3dNativeGame::Oot3dPicaCompareFunction::Always;
}



void WriteA32BlockTrace(const std::filesystem::path &path,
                        const NativeWidescreenProjectionState &runtime) {
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("cannot open A32 block trace: " + path.string());
  }
  stream << nlohmann::json({
                               {"format", "oot3d_a32_block_trace_v2"},
                               {"records", runtime.BlockTrace.size()},
                               {"records_observed",
                                runtime.BlockTraceRecordsObserved},
                               {"truncated", runtime.BlockTraceTruncated},
                           })
                .dump()
         << '\n';
  for (const auto &record : runtime.BlockTrace) {
    stream
        << nlohmann::json(
               {
                   {"host_frame", record.HostFrame},
                   {"pc", record.Pc},
                   {"memory_write_generation", record.MemoryWriteGeneration},
                   {"memory_write_fingerprint", record.MemoryWriteFingerprint},
                   {"fast_read_mismatch_count", record.FastReadMismatchCount},
                   {"last_fast_read_mismatch_address",
                    record.LastFastReadMismatchAddress},
                   {"last_fast_read_mismatch_value",
                    record.LastFastReadMismatchValue},
                   {"last_checked_read_mismatch_value",
                    record.LastCheckedReadMismatchValue},
                   {"r", record.Registers},
                   {"cpsr", record.Cpsr},
                   {"fpscr", record.Fpscr},
                   {"thread_pointer", record.ThreadPointer},
                   {"vfp", record.Vfp},
                   {"exclusive_address", record.ExclusiveAddress},
                   {"exclusive_token", record.ExclusiveToken},
                   {"exclusive_size", record.ExclusiveSize},
                   {"exclusive_valid", record.ExclusiveValid},
               })
               .dump()
        << '\n';
  }
  if (!stream) {
    throw std::runtime_error("cannot write A32 block trace: " + path.string());
  }
}

class ScopedA32BlockTraceWriter final {
 public:
  ScopedA32BlockTraceWriter(
      const std::filesystem::path &path,
      const NativeWidescreenProjectionState &runtime)
      : Path(path), Runtime(runtime) {}

  ~ScopedA32BlockTraceWriter() {
    if (Written || Path.empty()) {
      return;
    }
    try {
      Write();
    } catch (const std::exception &exception) {
      std::cerr << "oot3d_native_game: failed to preserve A32 fault trace: "
                << exception.what() << '\n';
    }
  }

  void Write() {
    if (Written || Path.empty()) {
      return;
    }
    WriteA32BlockTrace(Path, Runtime);
    Written = true;
  }

 private:
  std::filesystem::path Path;
  const NativeWidescreenProjectionState &Runtime;
  bool Written = false;
};

double SecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
      .count();
}

struct NativeFramePhaseTiming {
  double GuestSeconds = 0.0;
  double GuestInsidePicaSubmitSeconds = 0.0;
  double GuestClockSeconds = 0.0;
  double FrameStartSeconds = 0.0;
  double HostFrameStartSeconds = 0.0;
  double InputPollSeconds = 0.0;
  double RendererFrameStartSeconds = 0.0;
  double DspMixSeconds = 0.0;
  double AudioOutputSeconds = 0.0;
  double PicaSubmitSeconds = 0.0;
  double PicaPlanSeconds = 0.0;
  double PicaBackendSeconds = 0.0;
  double PicaDiagnosticsSeconds = 0.0;
  double VisualPresentationSeconds = 0.0;
  double VisualReplaySeconds = 0.0;
  double PresentSeconds = 0.0;
  double PacingWaitSeconds = 0.0;
  std::array<double, 15> Values() const {
    return {GuestSeconds, FrameStartSeconds, HostFrameStartSeconds, InputPollSeconds,
      RendererFrameStartSeconds, DspMixSeconds, AudioOutputSeconds, PicaSubmitSeconds,
      PicaPlanSeconds, PicaBackendSeconds, PicaDiagnosticsSeconds,
      VisualPresentationSeconds, VisualReplaySeconds, PresentSeconds, PacingWaitSeconds};
  }
};

struct NativeAudioOutputDiagnostics {
  void Observe(uint32_t hostFrame, int32_t before, int32_t after,
               uint64_t requestedFrames, int32_t desiredBuffered) {
    const auto now = std::chrono::steady_clock::now();
    if (HasSubmitTime) {
      const double gapSeconds =
          std::chrono::duration<double>(now - LastSubmitTime).count();
      MaximumSubmitGapSeconds = std::max(MaximumSubmitGapSeconds, gapSeconds);
      if (gapSeconds >= 0.05 && SubmitGapEvents.size() < 64U) {
        SubmitGapEvents.push_back({
            {"host_frame", hostFrame},
            {"gap_seconds", gapSeconds},
            {"buffered_before", before},
        });
      }
    }
    LastSubmitTime = now;
    HasSubmitTime = true;
    ++SubmitCalls;
    RequestedFrames += requestedFrames;
    if (!HasBufferObservation) {
      MinimumBufferedBefore = before;
      MaximumBufferedBefore = before;
      MinimumBufferedAfter = after;
      MaximumBufferedAfter = after;
      HasBufferObservation = true;
    } else {
      MinimumBufferedBefore = std::min(MinimumBufferedBefore, before);
      MaximumBufferedBefore = std::max(MaximumBufferedBefore, before);
      MinimumBufferedAfter = std::min(MinimumBufferedAfter, after);
      MaximumBufferedAfter = std::max(MaximumBufferedAfter, after);
    }
    const bool wasPrimed = Primed;
    if (after >= desiredBuffered) {
      Primed = true;
    }
    if (wasPrimed && before == 0) {
      ++StarvationObservations;
      StarvationEvents.push_back({
          {"host_frame", hostFrame},
          {"buffered_before", before},
          {"buffered_after", after},
          {"requested_frames", requestedFrames},
      });
    }
    const uint64_t growth =
        after > before ? static_cast<uint64_t>(after - before) : 0U;
    if (growth < requestedFrames) {
      EstimatedShortfallFrames += requestedFrames - growth;
      ++ShortGrowthSubmissions;
    }
  }

  bool HasBufferObservation = false;
  bool HasSubmitTime = false;
  bool Primed = false;
  std::chrono::steady_clock::time_point LastSubmitTime{};
  double MaximumSubmitGapSeconds = 0.0;
  uint64_t SubmitCalls = 0;
  uint64_t RequestedFrames = 0;
  uint64_t ShortGrowthSubmissions = 0;
  uint64_t EstimatedShortfallFrames = 0;
  uint64_t StarvationObservations = 0;
  int32_t MinimumBufferedBefore = 0;
  int32_t MaximumBufferedBefore = 0;
  int32_t MinimumBufferedAfter = 0;
  int32_t MaximumBufferedAfter = 0;
  nlohmann::json StarvationEvents = nlohmann::json::array();
  nlohmann::json SubmitGapEvents = nlohmann::json::array();
};

struct NativeInputConsumerDiagnostics {
  uint64_t Samples = 0;
  uint64_t ReadFailures = 0;
  uint64_t HeldFrames = 0;
  uint64_t PressedFrames = 0;
  uint64_t ReleasedFrames = 0;
  uint32_t LastHeld = 0;
  uint32_t LastPressed = 0;
  uint32_t LastReleased = 0;
  uint32_t LastRawCurrent = 0;
  uint32_t LastRawPrevious = 0;
  bool HasPrevious = false;
  nlohmann::json Transitions = nlohmann::json::array();

  void Observe(Oot3dNativeGame::NativeA32Memory &memory, uint32_t hostFrame) {
    uint32_t held = 0;
    uint32_t pressed = 0;
    uint32_t released = 0;
    uint32_t rawCurrent = 0;
    uint32_t rawPrevious = 0;
    if (!memory.Read32(kInputGlobalState + kInputHeldOffset, &held) ||
        !memory.Read32(kInputGlobalState + kInputPressedOffset, &pressed) ||
        !memory.Read32(kInputGlobalState + kInputReleasedOffset, &released) ||
        !memory.Read32(kInputGlobalState + kInputRawCurrentOffset,
                       &rawCurrent) ||
        !memory.Read32(kInputGlobalState + kInputRawPreviousOffset,
                       &rawPrevious)) {
      ++ReadFailures;
      return;
    }
    ++Samples;
    HeldFrames += held != 0U ? 1U : 0U;
    PressedFrames += pressed != 0U ? 1U : 0U;
    ReleasedFrames += released != 0U ? 1U : 0U;
    const bool changed = !HasPrevious || held != LastHeld ||
                         pressed != LastPressed || released != LastReleased ||
                         rawCurrent != LastRawCurrent ||
                         rawPrevious != LastRawPrevious;
    if (changed && Transitions.size() < 64U) {
      Transitions.push_back({
          {"host_frame", hostFrame},
          {"held", held},
          {"pressed", pressed},
          {"released", released},
          {"raw_current", rawCurrent},
          {"raw_previous", rawPrevious},
      });
    }
    LastHeld = held;
    LastPressed = pressed;
    LastReleased = released;
    LastRawCurrent = rawCurrent;
    LastRawPrevious = rawPrevious;
    HasPrevious = true;
  }
};





Oot3dNativeGame::NativeA32ProcessRunResult
RunUntilGuestWait(Oot3dNativeGame::NativeA32Process &process,
                  uint32_t wholeAotBlockBudget) {
  static const bool diagnoseWait = std::getenv("TRIAEVUM_DIAGNOSE_GUEST_WAIT") != nullptr;
  const auto waitStart = diagnoseWait ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
  // A block-limit exit unwinds the native whole-AOT call chain. The caller
  // resumes it immediately: this is a stack-safety boundary, not an SDL event
  // pump or guest scheduling boundary.
  auto result = process.Run(wholeAotBlockBudget);
  while (result.Kind == Oot3dNativeGame::NativeA32ProcessRunKind::Yielded) {
    // Low-priority workers may poll indefinitely. Once the foreground is
    // waiting, let the host deliver its timers, HID and GPU completions.
    // The worker stays Ready and resumes after those events, not next boot.
    if (process.OnlyBackgroundThreadsReady()) return result;
    if (diagnoseWait && std::chrono::steady_clock::now() - waitStart > std::chrono::seconds(20)) {
      for (uint32_t id = 0; id < process.ThreadCount(); ++id) {
        const auto* thread = process.ThreadState(id);
        if (thread != nullptr)
          std::fprintf(stderr, "GUEST_WAIT_TIMEOUT thread=%u pc=%08x lr=%08x sp=%08x r0=%08x r1=%08x r2=%08x\n",
                       id, thread->r[15], thread->r[14], thread->r[13],
                       thread->r[0], thread->r[1], thread->r[2]);
      }
      throw std::runtime_error("guest did not reach a wait boundary within diagnostic deadline");
    }
    result = process.Run(wholeAotBlockBudget);
  }
  return result;
}

void RequireRunnableGuest(
    const Oot3dNativeGame::NativeA32ProcessRunResult &result) {
  if (result.Kind == Oot3dNativeGame::NativeA32ProcessRunKind::Faulted) {
    throw std::runtime_error("native A32 process faulted: " + result.Error);
  }
}

Oot3dNativeGame::NativeGuestClockDeadlineResolution AdvanceGuestClockTo(
    Oot3dNativeGame::NativeA32Process &process,
    Oot3dNativeGame::NativeA32CtrHostServices &hostServices,
    Oot3dNativeGame::NativeA32ProcessRunResult &processResult,
    uint64_t deadline, uint32_t wholeAotBlockBudget) {
  auto resolution = Oot3dNativeGame::ResolveNativeGuestClockDeadline(
      hostServices.SystemTicks(), deadline);
  if (resolution.OvershootTicks != 0U) {
    return resolution;
  }
  while (processResult.Kind !=
         Oot3dNativeGame::NativeA32ProcessRunKind::Terminated) {
    const auto wakeTick = hostServices.NextSleepWakeTick();
    if (!wakeTick.has_value() || *wakeTick > deadline) {
      break;
    }
    const uint64_t currentTick = hostServices.SystemTicks();
    hostServices.AdvanceSystemTicks(
        *wakeTick > currentTick ? *wakeTick - currentTick : 0U);
    processResult = RunUntilGuestWait(process, wholeAotBlockBudget);
    RequireRunnableGuest(processResult);
    resolution = Oot3dNativeGame::ResolveNativeGuestClockDeadline(
        hostServices.SystemTicks(), deadline);
    if (resolution.OvershootTicks != 0U) {
      return resolution;
    }
  }
  resolution = Oot3dNativeGame::ResolveNativeGuestClockDeadline(
      hostServices.SystemTicks(), deadline);
  if (resolution.AdvanceTicks != 0U) {
    hostServices.AdvanceSystemTicks(resolution.AdvanceTicks);
  }
  return resolution;
}

std::vector<Oot3dNativeGame::Oot3dPicaPhysicalMemoryRegion>
BuildPicaMemoryRegions(
    const Oot3dNativeGame::NativeA32ProcessImageManifest &manifest) {
  std::vector<Oot3dNativeGame::Oot3dPicaPhysicalMemoryRegion> regions{
      {0x20000000U, manifest.LinearHeapBaseAddress, manifest.LinearHeapSize}};
  for (const auto &region : manifest.SystemRegions) {
    if (region.Name == "ctr_vram") {
      regions.push_back({0x18000000U, region.Address, region.MappedSize});
      break;
    }
  }
  return regions;
}



} // namespace

void RunOot3dNativeA32Window(const Oot3dNativeGameLaunch &launch) {
  if (launch.Host.Renderer != "opengl" &&
      launch.Host.Renderer != "nri" &&
      launch.Host.Renderer != "vulkan") {
    throw std::runtime_error(
        "the native process requires the OpenGL or NRI/Vulkan backend");
  }

  auto topScreenConfigRuntime =
      std::make_shared<Oot3dNativeGame::TopScreenUiConfigRuntime>(
          launch.TopScreenConfigPath, launch.TopScreenConfig);
  auto controlConfigRuntime =
      std::make_shared<Oot3dNativeGame::NativeControlConfigRuntime>(
          launch.ControlConfigPath, launch.ControlConfig);
  Oot3dNativeGame::TopScreenUiConfig activeTopScreenConfig =
      topScreenConfigRuntime->Snapshot().Config;
#if defined(__SWITCH__)
  gSwitchUiProfile = Oot3dNativeGame::Oot3dUiProfileName(launch.UiProfile);
  gSwitchGameplayTiming =
      Oot3dNativeGame::GameplayTimingModeName(launch.GameplayTiming);
  gSwitchControlProfile = Oot3dNativeGame::NativeControlProfileName(
      controlConfigRuntime->Snapshot().Config.Profile);
  gSwitchPresentationRateHz = launch.PresentationRateHz;
  gSwitchWholeAotBlockBudget = launch.WholeAotBlockBudget;
  gSwitchPicaGeometryCacheEnabled =
      !launch.DisableOpenGlPicaGeometryCache;
#endif

  std::string error;
  const auto manifest = Oot3dNativeGame::LoadNativeA32ProcessImageManifest(
      launch.A32ProcessManifestPath, &error);
  if (!manifest.has_value()) {
    throw std::runtime_error(error);
  }
#if defined(__SWITCH__)
  WriteSwitchBootStage("manifest_loaded");
#endif

  std::optional<Oot3dNativeGame::NativeScenarioCatalog> scenarioCatalog;
  std::optional<Oot3dNativeGame::NativeScenarioBootstrap> scenarioBootstrap;
  if (!launch.ScenarioId.empty()) {
    scenarioCatalog =
        Oot3dNativeGame::LoadNativeScenarioCatalog(launch.ScenarioCatalogPath);
    const auto *recipe = Oot3dNativeGame::FindNativeScenarioRecipe(
        *scenarioCatalog, launch.ScenarioId);
    if (recipe == nullptr) {
      throw std::runtime_error("unknown structural scenario: " +
                               launch.ScenarioId);
    }
    scenarioBootstrap.emplace(scenarioCatalog->Runtime, *recipe);
    std::cout << "oot3d_native_game: structural scenario " << recipe->Id
              << " selects " << recipe->ScenePath << " entrance "
              << recipe->EntranceIndex << '\n';
  }

  Oot3dNativeGame::TopScreenTextureOverridePack topScreenTextureOverridePack;
  Oot3dNativeGame::NativeHdFontRuntime hdFonts;
  bool hdFontsLoaded = false;
  if (!launch.TopScreenTextureOverridePackPath.empty()) {
    const auto fontPath = launch.TopScreenTextureOverridePackPath.parent_path() / "font_coverage.zip";
    if (std::filesystem::is_regular_file(fontPath)) {
      hdFontsLoaded = hdFonts.Load(fontPath, &error);
      if (!hdFontsLoaded) std::cerr << "HD font coverage unavailable; re-run Forge: " << error << '\n';
    }
  }
  const Oot3dNativeGame::TopScreenTextureOverridePack
      *topScreenTextureOverridePackPointer = nullptr;
  if (!launch.TopScreenTextureOverridePackPath.empty()) {
    if (!topScreenTextureOverridePack.Load(
            launch.TopScreenTextureOverridePackPath, &error)) {
      throw std::runtime_error("cannot load TopScreen texture overrides: " +
                               error);
    }
    topScreenTextureOverridePackPointer = &topScreenTextureOverridePack;
#if defined(__SWITCH__)
    gSwitchTopScreen211Assets =
        topScreenTextureOverridePack.FindProfileTexture(
            Oot3dNativeGame::kTopScreen211MenuAtlasSemantic) != nullptr &&
        topScreenTextureOverridePack.FindProfileTexture(
            "oot3d/topscreen/2.1.1/font_atlas") != nullptr;
#endif
  }

  Oot3dNativeGame::Oot3dNativePicaFrontend picaFrontend;
  picaFrontend.SetDiagnosticHistoryEnabled(false);
  Oot3dNativeGame::Oot3dNativePicaCompositionTracker
      picaCompositionTracker;
  Oot3dNativeGame::Oot3dPicaCompositionDomain picaCompositionDomain =
      Oot3dNativeGame::Oot3dPicaCompositionDomain::Unknown;
  Oot3dNativeGame::NativeA32CtrHostConfig hostConfig;
  const auto languageSettings = std::make_shared<Oot3dNativeGame::GameLanguageSettings>(
      std::filesystem::path(launch.Host.ConfigurationPath).parent_path() / "game_language.json",
      Oot3dNativeGame::DetectGameLanguages(manifest->RomFsImagePath,
          manifest->RomFsImageOffset, manifest->RomFsImageSize));
  hostConfig.SystemLanguage = languageSettings->SystemId();
  hostConfig.ResourceLimitValues = manifest->ResourceLimitValues;
  hostConfig.ResourceCurrentValues = manifest->ResourceCurrentValues;
  hostConfig.LinearHeapBaseAddress = manifest->LinearHeapBaseAddress;
  hostConfig.LinearHeapSize = manifest->LinearHeapSize;
  hostConfig.HeapBaseAddress = manifest->HeapBaseAddress;
  hostConfig.HeapSize = manifest->HeapSize;
  hostConfig.RomFsImagePath = manifest->RomFsImagePath;
  hostConfig.RomFsImageOffset = manifest->RomFsImageOffset;
  hostConfig.RomFsImageSize = manifest->RomFsImageSize;
  hostConfig.PicaFrontend = &picaFrontend;
  hostConfig.PicaCompositionDomain = &picaCompositionDomain;
  hostConfig.PicaCompositionProvider = &picaCompositionTracker;
  hostConfig.SaveDataDirectory =
      launch.SaveDataDirectory.empty()
          ? launch.A32ProcessManifestPath.parent_path() /
                "oot3d_native_savedata"
          : launch.SaveDataDirectory;
  const std::filesystem::path saveDataDirectory = hostConfig.SaveDataDirectory;
  const std::filesystem::path quickStatePath =
      launch.QuickStatePath.empty() ? saveDataDirectory / "quick.oot3dsav"
                                    : launch.QuickStatePath;
  const Oot3dNativeGame::NativeA32SavestateCompatibility savestateCompatibility{
      manifest->CodeBinSha256,
      manifest->ProcessName,
      std::string(oot3d::recomp::kA32SourceSnapshotId),
  };
  hostConfig.DetailedSvcDiagnostics = launch.ExtendedDiagnostics;
  hostConfig.ProfileRuntime = launch.ProfileA32Runtime;
  Oot3dNativeGame::NativeA32CtrHostServices hostServices(std::move(hostConfig));
  const auto frameRateContract =
      Oot3dNativeGame::ResolveNativeFrameRateContract(
          launch.GameplayTiming, launch.PresentationRateHz);
#if defined(__SWITCH__)
  gSwitchVisualInterpolation = frameRateContract.VisualInterpolationAllowed;
#endif
  Oot3dNativeGame::NativeFrameRatePolicy frameRatePolicy(frameRateContract);
  Oot3dNativeGame::NativeA32SceneViewProbe sceneViewProbe;
  Oot3dNativeGame::NativeA32PlayerTimingProbe playerTimingProbe;
  Oot3dNativeGame::NativeTemporalEventLedger temporalEventLedger;
  Oot3dNativeGame::NativeA32PlayerTemporalBridge playerTemporalBridge;
  NativeWidescreenProjectionState widescreenProjection;
  widescreenProjection.HdFonts = hdFontsLoaded ? &hdFonts : nullptr;
  widescreenProjection.FrameRatePolicy = &frameRatePolicy;
  widescreenProjection.SceneViewProbe = &sceneViewProbe;
  widescreenProjection.PlayerTimingProbe = &playerTimingProbe;
  widescreenProjection.TemporalEventLedger = &temporalEventLedger;
  if (frameRateContract.Mode ==
      Oot3dNativeGame::GameplayTimingMode::Enhanced60) {
    widescreenProjection.PlayerTemporalBridge = &playerTemporalBridge;
  }
  widescreenProjection.CollectExtendedDiagnostics = launch.ExtendedDiagnostics;
  widescreenProjection.ProfileA32Blocks = launch.ProfileA32Blocks;
  widescreenProjection.TraceA32Blocks = !launch.A32BlockTracePath.empty();
  ScopedA32BlockTraceWriter blockTraceWriter(
      launch.A32BlockTracePath, widescreenProjection);
  std::vector<uint32_t> blockEntryHooks{
      kMtx4x4BuildFrustum,
      kMtx4x4BuildOrthographicProjectionRotated,
      kGameStateUpdate,
      Oot3dNativeGame::kOot3dPlayerUpdateEntry,
      Oot3dNativeGame::kOot3dFramePacingBeginEntry,
      Oot3dNativeGame::kOot3dFramePacingDecisionEntry,
      Oot3dNativeGame::kOot3dFramePacingCommitEntry,
      Oot3dNativeGame::kOot3dMeshCommandPacketSubmitEntry,
  };
  const auto picaCompositionHooks =
      Oot3dNativeGame::Oot3dNativePicaCompositionHookPcs();
  blockEntryHooks.insert(blockEntryHooks.end(),
                         picaCompositionHooks.begin(),
                         picaCompositionHooks.end());
  if (widescreenProjection.PlayerTemporalBridge != nullptr) {
    const auto temporalHooks =
        Oot3dNativeGame::NativeA32PlayerTemporalHookPcs();
    blockEntryHooks.insert(blockEntryHooks.end(), temporalHooks.begin(),
                           temporalHooks.end());
  }
  const auto typedGameplayObservableExitHooks =
      Oot3dNativeGame::Oot3dTypedGameplayObservableExitPoints();
#if !defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
  blockEntryHooks.insert(blockEntryHooks.end(),
                         typedGameplayObservableExitHooks.begin(),
                         typedGameplayObservableExitHooks.end());
#endif
  const auto &uiLifecycleEntries =
      Oot3dNativeGame::Oot3dNativeUiLifecycleBridge::GuestEntryPoints();
  blockEntryHooks.insert(blockEntryHooks.end(), uiLifecycleEntries.begin(),
                         uiLifecycleEntries.end());
  if (launch.UiProfile == Oot3dNativeGame::Oot3dUiProfile::TopScreen) {
    const std::array topScreenHooks{
        Oot3dNativeGame::kTopScreenQuestMaterializedHook,
        Oot3dNativeGame::kTopScreenQuestSubmitModelsHook,
        Oot3dNativeGame::kTopScreenQuestDrawHook,
        0x00300588U,
        0x002FEABCU,
        0x00311364U,
        kPauseProjectionPrepare,
        kPauseUiDraw,
        kPauseUiDrawReturn,
        kPauseRendererVisibilityNativeDraw,
        kPauseIconBuild};
    // Texture payloads are patched when the native pause owner has loaded
    // them, before its update can submit HUD or pause draws.
    blockEntryHooks.push_back(kPauseUiUpdate);
    blockEntryHooks.insert(blockEntryHooks.end(), topScreenHooks.begin(),
                           topScreenHooks.end());
    const std::array topScreenSourcePortHooks{
        kPauseTouchButtonsDrawCall,
        kPauseTouchButtonsTouchBranch,
        kPauseLowerScreenBlock,
        kPauseDungeonMapCursorInput,
        kPauseUiNativeUpdateCall,
        kPauseProjectionPrepareCall,
        kPauseAlternateViewportDrawCall,
        kPauseConditionalDrawCall,
        kPauseRoutedCommandCallA,
        kPauseRoutedCommandCallB,
        kPauseOverlayViewportDrawCall,
        kPauseAlternateRendererUpdateCall,
        kPauseSceneSevenDrawCall,
        kPauseSceneViewportDrawCall,
        kPauseTouchCoordinateUpdateCall,
        kPauseRendererVisibilityCall,
        kPauseControllerDrawCall,
        kTopScreenCameraNormal1Scalar,
        kTopScreenCameraFovScalar,
        kTopScreenCameraUpdateEntry,
        kTopScreenCameraUpdatePatchSite,
        kTopScreenGameplayCompositionPrefix,
    };
    blockEntryHooks.insert(blockEntryHooks.end(),
                           topScreenSourcePortHooks.begin(),
                           topScreenSourcePortHooks.end());
    for (const auto &contract :
         Oot3dNativeGame::TopScreenVerifiedControlFlowContracts()) {
      blockEntryHooks.push_back(contract.Entry);
    }
    for (const auto &contract :
         Oot3dNativeGame::TopScreenVerifiedFloatLoadContracts()) {
      blockEntryHooks.push_back(contract.Entry);
    }
    const auto itemHooks = Oot3dNativeGame::TopScreenItemDispatchEntries();
    blockEntryHooks.insert(blockEntryHooks.end(), itemHooks.begin(), itemHooks.end());
    blockEntryHooks.push_back(Oot3dNativeGame::kTopScreenInputUpdateBoundary);
  }
  if (launch.ExtendedDiagnostics) {
    const std::array diagnosticHooks{
        kEnMagInit,
        kEnMagUpdate,
        kEnMagDraw,
        kPauseUiUpdate,
        kPauseUiDraw,
        kFileSelectUpdate,
        kFileSelectInit,
        kFileChooseInit,
        kPlayTransitionUpdate,
        kFileSelectActivate,
        kBlockingQueuePop,
        kBlockingQueuePush,
        kRendererCommandQueueProcessPending,
        kBlockingQueueTryPush,
        kTaskQueueTakeFirstFromPriority,
        kTaskQueueEnqueuePriorityNode,
        kTaskQueueWorkerDrainPriorityNodes,
        kNngxP3dCallback,
        kNngxCommonInterruptHandler,
        kNngxCmdlistCompletionDispatch,
        kRendererGlobalCallbackAfterLookup,
    };
    blockEntryHooks.insert(blockEntryHooks.end(), diagnosticHooks.begin(),
                           diagnosticHooks.end());
  }
  std::sort(blockEntryHooks.begin(), blockEntryHooks.end());
  if (hdFontsLoaded) blockEntryHooks.push_back(Oot3dNativeGame::kNativeQbfA4Blit);
  std::sort(blockEntryHooks.begin(), blockEntryHooks.end());
  blockEntryHooks.erase(
      std::unique(blockEntryHooks.begin(), blockEntryHooks.end()),
      blockEntryHooks.end());
#if defined(__SWITCH__)
  gSwitchBlockHookCount = blockEntryHooks.size();
  gSwitchBlockHooksSorted =
      std::is_sorted(blockEntryHooks.begin(), blockEntryHooks.end());
  gSwitchBlockHooksContainProcessEntry = std::binary_search(
      blockEntryHooks.begin(), blockEntryHooks.end(), 0x00100000U);
#endif
#if defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
  const bool enableCompiledFunctions = true;
#else
  const bool enableCompiledFunctions = !launch.DisableCompiledFunctions;
#endif
  const bool wholeAotAvailable =
      Oot3dNativeGame::Oot3dWholeAotAvailable();
#if defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
  if (!wholeAotAvailable) {
    throw std::runtime_error(
        "whole-AOT product mode requires the audited generated backend");
  }
  const bool enableWholeAot = true;
#else
  const bool enableWholeAot = enableCompiledFunctions &&
                              !launch.DisableWholeAot &&
                              wholeAotAvailable;
#endif
  if (enableCompiledFunctions && !launch.DisableWholeAot &&
      !wholeAotAvailable) {
    std::cerr
        << "Warning: whole-AOT was requested but is not linked. "
           "Guest execution will use substantially slower fallback paths.\n";
  }
#if defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
  const bool enableMassAot = false;
#else
  const bool enableMassAot = enableCompiledFunctions &&
                             !launch.DisableMassAot &&
                             Oot3dNativeGame::Oot3dMassAotAvailable();
#endif
#if defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
  const bool enableTrueAot = false;
#else
  const bool enableTrueAot =
      enableCompiledFunctions && !launch.DisableTrueAotBlocks;
#endif
#if defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
  const bool enableManualCompiledFunctions = true;
#else
  const bool enableManualCompiledFunctions =
      !launch.DisableManualCompiledFunctions;
#endif
  const auto trueAotEntries = Oot3dNativeGame::Oot3dTrueAotBlockEntryPoints();
  const auto compiledFunctionEntries =
      Oot3dNativeGame::Oot3dCompiledFunctionEntryPoints(
          enableWholeAot,
          enableCompiledFunctions && enableManualCompiledFunctions);
  const auto typedGameplayEntries =
      Oot3dNativeGame::Oot3dTypedGameplayEntryPoints();
  std::vector<uint32_t> nativeCandidateEntries;
  if (enableMassAot) {
    const auto massAotEntries = Oot3dNativeGame::Oot3dMassAotBlockEntryPoints();
    nativeCandidateEntries.insert(nativeCandidateEntries.end(),
                                  massAotEntries.begin(), massAotEntries.end());
    Oot3dNativeGame::ResetOot3dMassAotStats();
  }
  if (enableCompiledFunctions) {
    nativeCandidateEntries.insert(nativeCandidateEntries.end(),
                                  typedGameplayEntries.begin(),
                                  typedGameplayEntries.end());
    nativeCandidateEntries.insert(nativeCandidateEntries.end(),
                                  compiledFunctionEntries.begin(),
                                  compiledFunctionEntries.end());
    Oot3dNativeGame::ResetOot3dTypedGameplayStats();
    Oot3dNativeGame::ResetOot3dCompiledFunctionStats();
    Oot3dNativeGame::SetOot3dCompiledFunctionProfilingEnabled(
        launch.ProfileA32Runtime);
  }
  if (enableTrueAot) {
    nativeCandidateEntries.insert(nativeCandidateEntries.end(),
                                  trueAotEntries.begin(), trueAotEntries.end());
    Oot3dNativeGame::ResetOot3dTrueAotBlockStats();
  }
  std::vector<uint32_t> topScreenSourcePortBlocks;
  std::vector<uint32_t> wholeAotObservableExitBlocks;
#if !defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
  wholeAotObservableExitBlocks.push_back(
      Oot3dNativeGame::kOot3dMeshCommandPacketSubmitEntry);
  wholeAotObservableExitBlocks.insert(
      wholeAotObservableExitBlocks.end(), picaCompositionHooks.begin(),
      picaCompositionHooks.end());
#endif
#if !defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
  if (enableCompiledFunctions && !launch.DisableTypedGameplay) {
    wholeAotObservableExitBlocks.insert(
        wholeAotObservableExitBlocks.end(),
        typedGameplayObservableExitHooks.begin(),
        typedGameplayObservableExitHooks.end());
  }
#endif
  if (launch.UiProfile == Oot3dNativeGame::Oot3dUiProfile::TopScreen) {
    topScreenSourcePortBlocks = {
        kPauseTouchButtonsDrawCall,       kPauseTouchButtonsTouchBranch,
        kPauseLowerScreenBlock,           kPauseDungeonMapCursorInput,
        kPauseUiNativeUpdateCall,         kPauseProjectionPrepareCall,
        kPauseAlternateViewportDrawCall,  kPauseConditionalDrawCall,
        kPauseRoutedCommandCallA,         kPauseRoutedCommandCallB,
        kPauseOverlayViewportDrawCall,    kPauseAlternateRendererUpdateCall,
        kPauseSceneSevenDrawCall,         kPauseSceneViewportDrawCall,
        kPauseTouchCoordinateUpdateCall,  kPauseRendererVisibilityCall,
        kPauseControllerDrawCall,         kTopScreenCameraNormal1Scalar,
        kTopScreenCameraUpdateEntry,      kTopScreenCameraUpdatePatchSite,
        kTopScreenTitleLogoFadeHoldPrefix};
    for (const auto &contract :
         Oot3dNativeGame::TopScreenVerifiedControlFlowContracts()) {
      topScreenSourcePortBlocks.push_back(contract.Entry);
    }
    for (const auto &contract :
         Oot3dNativeGame::TopScreenVerifiedFloatLoadContracts()) {
      topScreenSourcePortBlocks.push_back(contract.Entry);
    }
    const auto itemEntries = Oot3dNativeGame::TopScreenItemDispatchEntries();
    topScreenSourcePortBlocks.insert(topScreenSourcePortBlocks.end(),
                                    itemEntries.begin(), itemEntries.end());
    topScreenSourcePortBlocks.push_back(kTopScreenGameplayCompositionPrefix);
    topScreenSourcePortBlocks.push_back(Oot3dNativeGame::kTopScreenInputUpdateBoundary);
    // These source ports are reached from inside large AOT regions. Make
    // their native entries observable so execution returns to the dispatcher
    // before applying the typed replacement. Keep this list sorted because
    // the whole-AOT block-entry callback uses binary_search.
    const std::array topScreenObservableExitBlocks{
        kTopScreenCameraNormal1Scalar,
        kTopScreenCameraUpdateEntry,
        kTopScreenGameplayCompositionPrefix,
        kTopScreenTitleLogoFadeHoldPrefix,
    };
#if !defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
    wholeAotObservableExitBlocks.insert(
        wholeAotObservableExitBlocks.end(),
        topScreenObservableExitBlocks.begin(),
        topScreenObservableExitBlocks.end());
#else
    wholeAotObservableExitBlocks.push_back(kTopScreenCameraUpdateEntry);
    wholeAotObservableExitBlocks.push_back(kTopScreenCameraNormal1Scalar);
#endif
    wholeAotObservableExitBlocks.push_back(Oot3dNativeGame::kTopScreenInputUpdateBoundary);
    wholeAotObservableExitBlocks.push_back(kPauseRendererVisibilityNativeDraw);
    wholeAotObservableExitBlocks.insert(wholeAotObservableExitBlocks.end(),
                                       itemEntries.begin(), itemEntries.end());
    nativeCandidateEntries.insert(nativeCandidateEntries.end(),
                                  topScreenSourcePortBlocks.begin(),
                                  topScreenSourcePortBlocks.end());
  }
  std::sort(wholeAotObservableExitBlocks.begin(),
            wholeAotObservableExitBlocks.end());
  wholeAotObservableExitBlocks.erase(
      std::unique(wholeAotObservableExitBlocks.begin(),
                  wholeAotObservableExitBlocks.end()),
      wholeAotObservableExitBlocks.end());
  std::sort(nativeCandidateEntries.begin(), nativeCandidateEntries.end());
  nativeCandidateEntries.erase(
      std::unique(nativeCandidateEntries.begin(), nativeCandidateEntries.end()),
      nativeCandidateEntries.end());
  Oot3dNativeGame::ConfigureOot3dNativeA32Candidates(
      nativeCandidateEntries);
  Oot3dNativeGame::NativeA32Process process(
      Oot3dNativeGame::Oot3dNativeA32Registry(), hostServices);
  widescreenProjection.TraceMemory = &process.Memory();
  process.Memory().EnableWriteTraceFingerprint(
      widescreenProjection.TraceA32Blocks);
  Oot3dNativeGame::Oot3dNativeUiLifecycleBridge uiLifecycleBridge(
      process.Memory());
  uiLifecycleBridge.SetTopScreenConfig(activeTopScreenConfig);
  widescreenProjection.UiLifecycleBridge = &uiLifecycleBridge;
  widescreenProjection.PicaCompositionDomain = &picaCompositionDomain;
  widescreenProjection.PicaCompositionTracker = &picaCompositionTracker;
  widescreenProjection.TopScreenUiProfile =
      launch.UiProfile == Oot3dNativeGame::Oot3dUiProfile::TopScreen;
  widescreenProjection.TopScreenConfig = activeTopScreenConfig;
  const auto nativeBlockEntryPcs =
      launch.ProfileA32Blocks || widescreenProjection.TraceA32Blocks
          ? std::span<const uint32_t>{}
          : std::span<const uint32_t>{blockEntryHooks};
  NativeCandidateDispatchState nativeCandidateDispatch{
      &ApplyNativeWidescreenProjectionPolicy,
      &widescreenProjection,
      nativeBlockEntryPcs,
      Oot3dNativeGame::BuildOot3dMassAotPcFilter(nativeBlockEntryPcs,
                                                 nativeBlockEntryPcs.empty()),
      Oot3dNativeGame::BuildOot3dMassAotPcFilter(
          wholeAotObservableExitBlocks, false),
      &process.Memory(),
      {static_cast<float>(frameRateContract.NativeUpdateRate),
       &frameRatePolicy.GameplayClock().Context()},
      enableCompiledFunctions && !launch.DisableTypedGameplay,
      enableMassAot,
      enableCompiledFunctions,
      enableWholeAot,
      enableManualCompiledFunctions,
      enableTrueAot,
      launch.ProfileA32Runtime,
  };
  nativeCandidateDispatch.TopScreenUiProfile =
      launch.UiProfile == Oot3dNativeGame::Oot3dUiProfile::TopScreen;
  widescreenProjection.TopScreenInput =
      &nativeCandidateDispatch.TopScreenInput;
  widescreenProjection.TopScreenInputClock =
      &nativeCandidateDispatch.TopScreenInputClock;
  widescreenProjection.TopScreenItems = &nativeCandidateDispatch.TopScreenItems;
  nativeCandidateDispatch.TopScreenItems.TraceEnabled = launch.ExtendedDiagnostics;
  nativeCandidateDispatch.UiLifecycleBridge = &uiLifecycleBridge;
  nativeCandidateDispatch.PicaCompositionDomain = &picaCompositionDomain;
  nativeCandidateDispatch.TopScreenPauseProjection =
      &uiLifecycleBridge.TopScreenPauseProjection();
  nativeCandidateDispatch.TopScreenConfig = &activeTopScreenConfig;
  Oot3dNativeGame::ApplyTopScreenFreeCameraConfig(
      nativeCandidateDispatch.TopScreenCamera.Camera, activeTopScreenConfig);
  std::uint64_t appliedTopScreenConfigRevision =
      topScreenConfigRuntime->Snapshot().Revision;
  const auto applyTopScreenConfigSnapshot = [&]() {
    const auto snapshot = topScreenConfigRuntime->Snapshot();
    if (snapshot.Revision == appliedTopScreenConfigRevision) {
      return;
    }
    activeTopScreenConfig = snapshot.Config;
    appliedTopScreenConfigRevision = snapshot.Revision;
    uiLifecycleBridge.SetTopScreenConfig(activeTopScreenConfig);
    widescreenProjection.TopScreenConfig = activeTopScreenConfig;
    Oot3dNativeGame::ApplyTopScreenFreeCameraConfig(
        nativeCandidateDispatch.TopScreenCamera.Camera,
        activeTopScreenConfig);
    if (!activeTopScreenConfig.FreeCameraEnabled) {
      nativeCandidateDispatch.TopScreenCameraInput.Reset();
    }
  };
  const auto setTopScreenSessionMinimapVisible = [&](bool visible) {
    activeTopScreenConfig.MinimapVisible = visible;
    uiLifecycleBridge.SetTopScreenConfig(activeTopScreenConfig);
    widescreenProjection.TopScreenConfig = activeTopScreenConfig;
  };
  widescreenProjection.TopScreenPauseTargetRoute =
      &nativeCandidateDispatch.TopScreenPauseRoute;
  widescreenProjection.TopScreenTouchCoordinateRoute =
      &nativeCandidateDispatch.TopScreenTouchCoordinateRoute;
  widescreenProjection.WholeAotObservableExitPcs =
      wholeAotObservableExitBlocks;
  process.SetTimingEnabled(launch.ProfileA32Runtime);
#if !defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
  // Native candidates are experimental reconstruction aids.  The product
  // executes the audited whole-AOT graph and never substitutes source-native
  // or typed gameplay implementations for guest functions.
  if (!nativeCandidateEntries.empty()) {
    process.SetNativeBlockCallback(&ExecuteNativeCandidate,
                                   &nativeCandidateDispatch);
  }
#else
  if (nativeCandidateDispatch.TopScreenUiProfile) {
    process.SetNativeBlockCallback(&ExecuteProductTopScreenHook,
                                   &nativeCandidateDispatch);
  }
#endif
  if (!Oot3dNativeGame::MountNativeA32ProcessImage(
          process, *manifest, std::filesystem::path{}, &error)) {
    throw std::runtime_error(error);
  }
#if defined(__SWITCH__)
  WriteSwitchBootStage("process_image_mounted");
#endif
  const auto applySelectedUiProfile = [&]() {
    if (launch.UiProfile != Oot3dNativeGame::Oot3dUiProfile::TopScreen) {
      return;
    }
    Oot3dNativeGame::TopScreenLayoutApplyStats layoutStats;
    if (!Oot3dNativeGame::ApplyTopScreenVerifiedLayout(process.Memory(),
                                                       &layoutStats, &error)) {
      throw std::runtime_error(
          "TopScreen source-port layout rejected the process image: " + error);
    }
    Oot3dNativeGame::TopScreenRuntimeGeometryApplyStats geometryStats;
    if (layoutStats.WordsChanged != 0U &&
        !Oot3dNativeGame::RebindTopScreenVerifiedRuntimeGeometry(
            process.Memory(), &geometryStats, &error)) {
      throw std::runtime_error(
          "TopScreen source-port runtime geometry rejected the process "
          "image: " +
          error);
    }
    std::cout << "TopScreen source-port layout: contracts="
              << layoutStats.ContractsChecked
              << " words=" << layoutStats.WordsWritten
              << " changed_words=" << layoutStats.WordsChanged
              << " rebound_streams=" << geometryStats.StreamsRebound
              << " rebound_quads=" << geometryStats.QuadsRebound << '\n';
  };

  const auto picaMemoryRegions = BuildPicaMemoryRegions(*manifest);
  const Oot3dNativeGame::Oot3dPicaPhysicalMemoryView picaMemoryView(
      process.Memory(), picaMemoryRegions);
  std::optional<Oot3dNativeGame::TopScreenTextureOverrideRuntime>
      topScreenTextureOverrideRuntime;
  if (topScreenTextureOverridePackPointer != nullptr) {
    topScreenTextureOverrideRuntime.emplace(topScreenTextureOverridePack,
                                            picaMemoryView);
    widescreenProjection.TopScreenTextureOverrides =
        &*topScreenTextureOverrideRuntime;
  }
  Oot3dNativeGame::Oot3dNativePicaSubmissionQueue submissionQueue(
      picaMemoryView, true);
  // Generated title code links its own checked memory helpers. Only a module
  // advertising the observer contract can preserve GPU/CPU byte ownership.
#if defined(OOT3D_NATIVE_DIRECT_AOT_PLUGIN)
  const bool copyWritesObservable =
      Oot3dNativeGame::Oot3dWholeAotPluginObservesMemoryWrites();
#else
  constexpr bool copyWritesObservable = true;
#endif
  if (copyWritesObservable) {
    process.Memory().SetWriteObserver(
        [&](uint32_t address, size_t size) {
          if (!picaMemoryView.SynchronizeGuestWrite(process.Memory(), address, size))
            throw std::runtime_error("cannot make a guest write visible through its physical aliases");
          submissionQueue.NotifyGuestWrite(address, size);
        });
    submissionQueue.BindGuestWriteObservation(
        [&](uint32_t address, size_t size) {
          return process.Memory().ObserveWrites(address, size);
        });
  }
  struct CopyWriteObserverLifetime {
    Oot3dNativeGame::NativeA32Memory& Memory;
    ~CopyWriteObserverLifetime() { Memory.SetWriteObserver({}); }
  } copyWriteObserverLifetime{process.Memory()};
  if (hdFontsLoaded) submissionQueue.SetTextureSnapshotTransform(
      [&](const Oot3dNativeGame::Oot3dPicaTextureState& texture,
          Oot3dNativeGame::Oot3dPicaResourceSnapshot& resource) {
        hdFonts.Resolve(picaMemoryView, texture, resource);
      });
  if (topScreenTextureOverrideRuntime.has_value()) {
    submissionQueue.SetTexturePayloadTransform(
        [&](const Oot3dNativeGame::Oot3dPicaTextureState &texture,
            std::span<std::uint8_t> payload) {
          topScreenTextureOverrideRuntime->Transform(texture, payload);
        });
  }
  submissionQueue.SetRuntimeProfilingEnabled(launch.ProfileA32Runtime);
  picaFrontend.SetPacketSink(&submissionQueue);

  Args hostArgs = launch.Host;
  hostArgs.AudioSampleRate = Oot3dNativeGame::NativeA32DspHle::NativeSampleRate;
  hostArgs.AudioSampleLength =
      Oot3dNativeGame::NativeA32DspHle::SamplesPerFrame;
  hostArgs.AudioDesiredBuffered = std::max<int32_t>(
      hostArgs.AudioDesiredBuffered,
      static_cast<int32_t>((static_cast<uint64_t>(hostArgs.AudioSampleRate) *
                                kMinimumHostAudioPrebufferMilliseconds +
                            999U) /
                           1000U));
  const auto sceneEnvironment = std::make_shared<Oot3dNativeGame::SceneEnvironmentRuntime>(
      std::filesystem::path(launch.Host.ConfigurationPath).parent_path() / "environments",
      [](const Fast::Renderer3ds::PicaEnvironmentLighting& environment) {
        Fast::Oot3d::GraphicsSettingsRuntime::Instance().PublishSceneEnvironment(environment);
      });
  std::vector<std::shared_ptr<Fast::Oot3d::GraphicsSettingsPanelTab>>
      applicationSettingsTabs;
  applicationSettingsTabs.push_back(Oot3dNativeGame::CreateGameLanguagePanel(languageSettings));
  applicationSettingsTabs.push_back(Oot3dNativeGame::CreateSceneEnvironmentPanel(sceneEnvironment));
  applicationSettingsTabs.push_back(
      Oot3dNativeGame::CreateNativeControlsSettingsPanel(
          controlConfigRuntime,
          launch.UiProfile ==
                  Oot3dNativeGame::Oot3dUiProfile::TopScreen
              ? topScreenConfigRuntime
              : nullptr));
  if (launch.UiProfile ==
      Oot3dNativeGame::Oot3dUiProfile::TopScreen) {
    applicationSettingsTabs.push_back(
        Oot3dNativeGame::CreateTopScreenSettingsPanel(
            topScreenConfigRuntime));
  }
  Fast::Oot3d::InstallGraphicsSettingsPanelTabs(
      std::move(applicationSettingsTabs));
  InitContextForDemo(hostArgs);
#if defined(__SWITCH__)
  WriteSwitchBootStage("host_context_initialized");
#endif
#if defined(OOT3D_WHOLE_AOT_PRODUCT_MODE) && defined(_WIN32)
  std::string crashDiagnosticsError;
  if (!Oot3dNativeGame::InstallWholeAotCrashDiagnostics(
          saveDataDirectory / "crashes", &crashDiagnosticsError)) {
    throw std::runtime_error("whole-AOT crash diagnostics failed: " +
                             crashDiagnosticsError);
  }
#endif
  auto &window = GetActiveFast3dWindowForDemo();
  // Interactive presentation is paced by NativeRealtimeRefreshPacer, not by a
  // second software limiter in SDL. Bounded probes retain their existing policy.
  if (hostArgs.ThroughputBenchmark || hostArgs.FrameLimit == 0U) {
    window.SetTargetFps(0);
  }
  auto &api = GetActiveRenderingApiForDemo(window);
  auto *const programPreparation =
      dynamic_cast<Fast::Renderer3ds::PicaProgramPreparationBackend *>(&api);
  const bool alwaysAvailablePrograms =
      programPreparation != nullptr &&
      programPreparation->UsesAlwaysAvailablePrograms();
  if (programPreparation != nullptr)
    programPreparation->RegisterTitlePassPrograms(
        Oot3dNativeGame::NativePicaTitlePassPrograms());
  if (programPreparation != nullptr &&
      (launch.PicaParametricTev || alwaysAvailablePrograms)) {
    programPreparation->PrepareNativePicaPrograms(
        Oot3dNativeGame::NativePicaVertexProgramFamily());
  }
  // Initial preparation: from here until the renderer's startup work is ready
  // and the first frames run within budget, the window shows its progress.
  Oot3dNativeGame::NativePreparationScreen preparationScreen(
      std::chrono::steady_clock::now());
  api.SetNativePicaGeometryCacheEnabled(
      !launch.DisableOpenGlPicaGeometryCache);
  auto *const titleRenderBackend =
      dynamic_cast<Fast::Oot3d::TitleRenderBackend *>(&api);
  const auto gui =
      Ship::Context::GetRawInstance()->GetWindow()->GetGui();
  if (gui == nullptr) {
    throw std::runtime_error("native PICA host has no renderer GUI");
  }
#if defined(__SWITCH__)
  WriteSwitchBootStage("opengl_renderer_initialized");
#endif
  Oot3dNativeGame::Oot3dNativeA32UiTextureProvider nativeUiTextureProvider(
      picaMemoryView, topScreenTextureOverridePackPointer);
  if (hdFontsLoaded) nativeUiTextureProvider.SetGeneratedTextureTransform(
      [&](const Oot3dNativeGame::Oot3dPicaTextureState& texture,
          Oot3dNativeGame::Oot3dPicaResourceSnapshot& resource) {
        hdFonts.Resolve(picaMemoryView, texture, resource);
      });
  Oot3dNativeGame::TopScreenOcarinaTextRuntime ocarinaText(process.Memory(), nativeUiTextureProvider);
  nativeCandidateDispatch.OcarinaText = &ocarinaText;
  widescreenProjection.OcarinaText = &ocarinaText;
  oot3d::ui::Fast3dUiRenderBackend uiRenderBackend(api);
  oot3d::ui::N64UiFast3dRenderer n64UiRenderer(uiRenderBackend,
                                               &ocarinaText);
  std::array<std::vector<oot3d::ui::UiPrimitive>,
             oot3d::ui::kUiSubsystemCount>
      lastTopScreenPrimitivesBySubsystem;
  window.SetTextureFilter(Fast::FILTER_LINEAR);
  const auto inputTimeline = Oot3dNativeGame::NativeA32InputTimeline::LoadFile(
      launch.Host.InputTimelinePath);
  auto graphicsSettingsSchedule =
      Oot3dNativeGame::GraphicsSettingsSchedule::LoadFile(
          launch.Host.GraphicsSettingsSchedulePath);
#if defined(__SWITCH__)
  WriteSwitchBootStage("ui_runtime_initialized");
#endif

  const auto updateScenePresentation = [&]() {
    widescreenProjection.Projection.Presentation =
        Fast::Oot3d::ResolveScenePresentationPolicy(
            std::max<uint32_t>(1, window.GetWidth()),
            std::max<uint32_t>(1, window.GetHeight()));
    widescreenProjection.Projection.FovMultiplier =
        Fast::Oot3d::GraphicsSettingsRuntime::Instance()
            .Snapshot()
            .FovMultiplier;
  };
  updateScenePresentation();
  process.SetBlockEntryCallback(
      &ApplyNativeWidescreenProjectionPolicy, &widescreenProjection,
      launch.ProfileA32Blocks || widescreenProjection.TraceA32Blocks
          ? std::vector<uint32_t>{}
          : blockEntryHooks);
#if defined(__SWITCH__)
  WriteSwitchBootStage("guest_callbacks_initialized");
#endif

  NativeFramePhaseTiming phaseTiming;
  Oot3dNativeGame::NativeA32InputDiagnostics inputDiagnostics;
  NativeInputConsumerDiagnostics inputConsumerDiagnostics;
  NativeControlPollingState nativeControlPollingState;
  Oot3dNativeGame::Oot3dPicaVulkanShaderSourceCache shaderSourceCache;
  shaderSourceCache.TevMode = launch.PicaParametricTev
      ? Oot3dNativeGame::Oot3dPicaTevMode::Parametric
      : Oot3dNativeGame::Oot3dPicaTevMode::Specialized;
  shaderSourceCache.UberFragments = alwaysAvailablePrograms;
  Oot3dNativeGame::Oot3dPicaSemanticTraceWriter picaSemanticTrace(
      launch.PicaSemanticTracePath);
  picaSemanticTrace.BeginSession(
      launch.Host.Renderer,
      Oot3dNativeGame::GameplayTimingModeName(launch.GameplayTiming),
      Oot3dNativeGame::Oot3dUiProfileName(launch.UiProfile));
  Oot3dNativeGame::NativeA32DspHle dspHle(
      Oot3dNativeGame::BuildNativeA32DspPhysicalRegions(*manifest));
  auto *context = Ship::Context::GetRawInstance();
  auto audio = context != nullptr ? context->GetAudio() : nullptr;
  auto audioPlayer = audio != nullptr ? audio->GetAudioPlayer() : nullptr;
#if defined(__SWITCH__)
  WriteSwitchBootStage("audio_runtime_initialized");
#endif
  auto phaseStart = std::chrono::steady_clock::now();
  Oot3dNativeGame::NativeA32ProcessRunResult processResult;
  uint32_t frameCount = 0;
  uint32_t runFrameCount = 0;
  uint64_t presentationFrameCount = 0;
  uint64_t refreshTickRemainder = 0;
  uint64_t nextVblankTick = 0;
  uint64_t lateVblankDeadlineCount = 0;
  uint64_t maximumVblankDeadlineOvershootTicks = 0;
  uint64_t savestateSaveCount = 0;
  uint64_t savestateLoadCount = 0;
  uint64_t savestateSaveBytes = 0;
  uint64_t savestateLoadBytes = 0;
  double savestateSaveSeconds = 0.0;
  double savestateLoadSeconds = 0.0;
  double savestateCaptureSeconds = 0.0;
  double savestateEncodeSeconds = 0.0;
  double savestateWriteSeconds = 0.0;
  double savestateReadSeconds = 0.0;
  double savestateDecodeSeconds = 0.0;
  double savestateRestoreSeconds = 0.0;
  uint64_t legacySavestateClockRestoreCount = 0;
  uint64_t legacySavestatePresentationSchedulerRestoreCount = 0;
  uint64_t legacySavestateFrameRatePolicyRestoreCount = 0;
  uint64_t legacySavestateTopScreenTemporalRestoreCount = 0;
  uint64_t legacySavestatePicaVisualReplayRestoreCount = 0;
  uint64_t legacySavestatePicaTextureCacheRestoreCount = 0;
  uint64_t legacySavestatePicaColorTargetRestoreCount = 0;
  uint64_t legacySavestatePicaPresentationRestoreCount = 0;
  uint64_t savestatePicaTexturesCaptured = 0;
  uint64_t savestatePicaTextureBytesCaptured = 0;
  uint64_t savestatePicaTexturesRestored = 0;
  uint64_t savestatePicaTextureBytesRestored = 0;
  uint64_t savestatePicaColorTargetsCaptured = 0;
  uint64_t savestatePicaColorTargetBytesCaptured = 0;
  uint64_t savestatePicaColorTargetsRestored = 0;
  uint64_t savestatePicaColorTargetBytesRestored = 0;
  uint64_t savestatePicaDisplayImagesCaptured = 0;
  uint64_t savestatePicaDisplayImageBytesCaptured = 0;
  uint64_t savestatePicaDisplayImagesRestored = 0;
  uint64_t savestatePicaDisplayImageBytesRestored = 0;
  uint64_t savestatePicaVisualReplayBytesCaptured = 0;
  uint64_t savestatePicaVisualReplayBytesRestored = 0;
  uint64_t lastSavestateSemanticFingerprint = 0;
  Oot3dNativeGame::NativePresentationScheduler presentationScheduler(
      frameRateContract.SimulationRateHz,
      frameRateContract.Mode ==
              Oot3dNativeGame::GameplayTimingMode::Enhanced60
          ? Oot3dNativeGame::NativeSimulationCatchUpPolicy::
                PreserveBoundedDebt
          : Oot3dNativeGame::NativeSimulationCatchUpPolicy::DropExcess);
  Oot3dNativeGame::NativeVisualSampleCadence visualSampleCadence;
  Oot3dNativeGame::Oot3dPicaPresentationScheduler
      picaPresentationScheduler;
  Oot3dNativeGame::Oot3dPicaVisualContinuityTracker
      visualContinuityTracker;
  Oot3dNativeGame::Oot3dPicaPreparedVisualTransition
      preparedVisualTransition;
  std::optional<Oot3dNativeGame::Oot3dPicaVisualFrame>
      latestVisualFrame;
  std::optional<Oot3dNativeGame::Oot3dPicaVisualFrame>
      previousVisualFrame;
  bool latestVisualTransitionContinuous = false;
  uint64_t visualContinuityEpoch = 1U;
  std::map<uint32_t, Oot3dNativeGame::Oot3dPicaDisplayTransferSubmission>
      displayTransfersByOutput;
  uint64_t lastSubmittedDrawId = 0;
  // Diagnostic: raw TextureCopy destinations watched for guest access while
  // extended diagnostics are on. Empty otherwise, so normal runs are untouched.
  std::vector<std::pair<uint32_t, uint32_t>> rawCopyWatchRanges;
  // Instrument control: TRIAEVUM_WATCH_GUEST_RANGE=0xSTART:0xSIZE watches an
  // arbitrary guest range, so a run can show that typed accesses from the
  // precompiled title module really are counted instead of assumed.
  bool watchReportRequested = false;
  if (const char* watchRange = std::getenv("TRIAEVUM_WATCH_GUEST_RANGE")) {
    unsigned long long start = 0, size = 0;
    const int parsed = std::sscanf(watchRange, "%llx:%llx", &start, &size);
    std::fprintf(stderr,
                 "TRIAEVUM_WATCH_GUEST_RANGE raw='%s' parsed=%d start=0x%llX size=0x%llX\n",
                 watchRange, parsed, start, size);
    if (parsed == 2 && size != 0) {
      rawCopyWatchRanges.emplace_back(static_cast<uint32_t>(start),
                                      static_cast<uint32_t>(start + size));
      process.Memory().SetRangeWatch(rawCopyWatchRanges);
      watchReportRequested = true;
      std::fprintf(stderr, "TRIAEVUM_WATCH_GUEST_RANGE active=%d ranges=%zu\n",
                   process.Memory().RangeWatchActive() ? 1 : 0,
                   process.Memory().WatchedRangeCount());
    }
  }
  uint64_t lastSelectedTopTransferCompletionId = 0;
  const auto restoreSavestateTiming =
      [&](const Oot3dNativeGame::NativeA32SavestateRuntimeState &runtime) {
        if (!runtime.GameplayClockAvailable) {
          frameRatePolicy.ResetGameplayClock();
          ++legacySavestateClockRestoreCount;
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
          ++legacySavestatePresentationSchedulerRestoreCount;
        } else if (!presentationScheduler.Restore(
                       runtime.PresentationScheduler, &error)) {
          throw std::runtime_error(
              "savestate presentation scheduler restore failed: " + error);
        }
        if (!runtime.FrameRatePolicyTemporalStateAvailable) {
          frameRatePolicy.ResetTemporalState();
          ++legacySavestateFrameRatePolicyRestoreCount;
        } else if (!frameRatePolicy.RestoreTemporalState(
                       runtime.FrameRatePolicyTemporalState, &error)) {
          throw std::runtime_error(
              "savestate frame-rate policy restore failed: " + error);
        }
      };
  const auto restoreSavestateTopScreenTemporalState =
      [&](const Oot3dNativeGame::NativeA32SavestateRuntimeState &runtime) {
        if (launch.UiProfile !=
            Oot3dNativeGame::Oot3dUiProfile::TopScreen) {
          return;
        }
        if (!runtime.TopScreenTemporalStateAvailable ||
            !runtime.TopScreenTemporalState.ProfileActive) {
          ++legacySavestateTopScreenTemporalRestoreCount;
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
      };
  const auto restoreSavestatePicaTextureCache =
      [&](const Oot3dNativeGame::NativeA32SavestateRuntimeState &runtime) {
        if (!runtime.PicaTextureCacheAvailable) {
          ++legacySavestatePicaTextureCacheRestoreCount;
          return;
        }
        if (!api.RestorePicaTextureCache(
                runtime.PicaTextureCache, &error)) {
          throw std::runtime_error(
              "savestate native PICA texture restore failed: " + error);
        }
        for (const auto &texture : runtime.PicaTextureCache) {
          savestatePicaTextureBytesRestored +=
              texture.PixelBytes.size();
        }
        savestatePicaTexturesRestored +=
            runtime.PicaTextureCache.size();
      };
  const auto restoreSavestatePicaColorTargets =
      [&](const Oot3dNativeGame::NativeA32SavestateRuntimeState &runtime) {
        if (!runtime.PicaColorTargetsAvailable) {
          ++legacySavestatePicaColorTargetRestoreCount;
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
          savestatePicaColorTargetBytesRestored +=
              target.ColorRgba8.size();
        }
        submissionQueue.RestoreGpuColorRenderTargetOwnership(
            restoredAddresses);
        savestatePicaColorTargetsRestored += restoredAddresses.size();
      };
  const auto restoreSavestatePicaPresentationState =
      [&](const Oot3dNativeGame::NativeA32SavestateRuntimeState &runtime) {
        if (!runtime.PicaPresentationStateAvailable) {
          ++legacySavestatePicaPresentationRestoreCount;
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
          savestatePicaDisplayImageBytesRestored +=
              image.ColorRgba8.size();
        }
        savestatePicaDisplayImagesRestored +=
            runtime.PicaPresentationState.DisplayImages.size();
      };
  const auto restoreSavestatePicaVisualReplayState =
      [&](const Oot3dNativeGame::NativeA32SavestateRuntimeState &runtime) {
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
          ++legacySavestatePicaVisualReplayRestoreCount;
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
        savestatePicaVisualReplayBytesRestored +=
            runtime.PicaVisualReplayState.size();
      };
  if (!launch.LoadStatePath.empty()) {
#if defined(__SWITCH__)
    WriteSwitchBootStage("initial_savestate_loading");
#endif
    Oot3dNativeGame::NativeA32SavestateRuntimeState restoredRuntime;
    Oot3dNativeGame::NativeA32SavestateIoResult ioResult;
    if (!Oot3dNativeGame::LoadNativeA32State(
            launch.LoadStatePath, savestateCompatibility, restoredRuntime,
            process, hostServices, picaFrontend, submissionQueue, dspHle,
            &ioResult, &error)) {
      throw std::runtime_error("native savestate load failed: " + error);
    }
    Oot3dNativeGame::ResetOot3dTypedGameplayTransientState();
    hdFonts.Reset();
    const auto restoredGravity = hostServices.HidRuntimeProfile().LastAccelerometer;
    nativeControlPollingState.VirtualMotion.RestoreGravity(
        {static_cast<float>(restoredGravity[0]), static_cast<float>(restoredGravity[1]),
         static_cast<float>(restoredGravity[2])});
    nativeControlPollingState.PendingMouseDeltaX = 0;
    nativeControlPollingState.PendingMouseDeltaY = 0;
    nativeControlPollingState.PendingInputSeconds = 0.0;
    picaCompositionTracker.Reset();
    frameCount = restoredRuntime.FrameCount;
    refreshTickRemainder = restoredRuntime.RefreshTickRemainder;
    nextVblankTick = restoredRuntime.NextVblankTick;
    processResult.Kind = static_cast<Oot3dNativeGame::NativeA32ProcessRunKind>(
        restoredRuntime.ProcessRunKind);
    restoreSavestateTiming(restoredRuntime);
    applySelectedUiProfile();
    if (!api.ResetPicaState(&error)) {
      throw std::runtime_error(
          "native PICA reset before initial savestate restore failed: " +
          error);
    }
    if (titleRenderBackend != nullptr &&
        !titleRenderBackend->ResetTitleState(&error)) {
      throw std::runtime_error(
          "OOT3D renderer reset before initial savestate restore failed: " +
          error);
    }
    restoreSavestatePicaTextureCache(restoredRuntime);
    restoreSavestatePicaColorTargets(restoredRuntime);
    restoreSavestatePicaPresentationState(restoredRuntime);
    restoreSavestatePicaVisualReplayState(restoredRuntime);
    restoreSavestateTopScreenTemporalState(restoredRuntime);
    ++savestateLoadCount;
    savestateLoadBytes += ioResult.FileBytes;
    savestateLoadSeconds += SecondsSince(phaseStart);
    savestateReadSeconds += ioResult.IoSeconds;
    savestateDecodeSeconds += ioResult.DecodeSeconds;
    savestateRestoreSeconds += ioResult.RestoreSeconds;
    std::cout << "oot3d_native_game: loaded initial state at frame "
              << frameCount << " from " << launch.LoadStatePath.string()
              << '\n';
  } else {
    applySelectedUiProfile();
#if defined(__SWITCH__)
    WriteSwitchBootStage("initial_guest_dispatch");
#endif
    processResult =
        RunUntilGuestWait(process, launch.WholeAotBlockBudget);
#if defined(__SWITCH__)
    WriteSwitchBootStage("initial_guest_wait");
#endif
    phaseTiming.GuestSeconds += SecondsSince(phaseStart);
    RequireRunnableGuest(processResult);
  }
  uint64_t drawCount = 0;
  uint64_t refreshesWithNativeDraws = 0;
  uint64_t displayTransferCount = 0;
  uint64_t recordedCompletionCount = 0;
  uint64_t completedP3dCount = 0;
  uint64_t completedPpfCount = 0;
  uint64_t picaDrainPassCount = 0;
  uint64_t maximumPicaDrainPassesPerRefresh = 0;
  uint64_t selectedTopTransferCount = 0;
  uint64_t nativeBottomFrontendPresentationCount = 0;
  uint64_t nativeMenuBackdropDrawOmissionCount = 0;
  uint64_t nativeMenuClearDrawOmissionCount = 0;
  uint64_t nativeFrontendPresentationActiveCount = 0;
  uint64_t nativeFrontendBottomTransferHitCount = 0;
  uint64_t visualSnapshotCount = 0;
  uint64_t visualTransitionCount = 0;
  uint64_t visualMatchedDrawCount = 0;
  uint64_t visualStrictUniqueMatchedDrawCount = 0;
  uint64_t visualStrictOrdinalMatchedDrawCount = 0;
  uint64_t visualStructuralOrdinalMatchedDrawCount = 0;
  uint64_t visualPipelineOrdinalMatchedDrawCount = 0;
  uint64_t visualChangedContinuousDrawCount = 0;
  uint64_t visualChangedPerInstanceVertexDrawCount = 0;
  uint64_t visualChangedPerVertexDrawCount = 0;
  Oot3dNativeGame::Oot3dPicaVisualInterpolationTiming visualInterpolationTiming;
  uint64_t visualAmbiguousPreviousDrawCount = 0;
  uint64_t visualAmbiguousCurrentDrawCount = 0;
  uint64_t visualUnmatchedPreviousDrawCount = 0;
  uint64_t visualUnmatchedCurrentDrawCount = 0;
  uint64_t visualSamplesPresented = 0;
  uint64_t visualDirectFramesPresented = 0;
  uint64_t visualReusedSnapshotPresentations = 0;
  uint64_t visualDiscontinuityCount = 0;
  uint64_t visualNamespaceResyncCount = 0;
  // Output addresses of selected top-screen transfers (the display
  // framebuffers): an earlier snapshot into one ends a superseded guest frame.
  std::vector<uint32_t> topDisplayOutputs;
  uint64_t visualSampleDrawCount = 0;
  uint64_t visualInterpolatedDrawCount = 0;
  uint64_t visualInterpolatedPerInstanceVertexDrawCount = 0;
  uint64_t visualInterpolatedPerVertexDrawCount = 0;
  uint64_t vblankCount = 0;
  uint64_t dspFramesMixed = 0;
  uint64_t dspFramesSuppressed = 0;
  uint64_t dspSamplesMixed = 0;
  uint64_t dspNonzeroSamples = 0;
  uint64_t dspPcmFnv1a64 = 14695981039346656037ULL;
  int32_t dspPeakMagnitude = 0;
  nlohmann::json displayTransferEvents = nlohmann::json::array();
  nlohmann::json drawEvents = nlohmann::json::array();
  nlohmann::json visualTransitionEvents = nlohmann::json::array();
  WindowDemoTimingState timing;
  auto presentationPacingPolicy =
      Fast::Oot3d::ResolvePresentationPacingPolicy(
          Fast::Oot3d::GraphicsSettingsRuntime::Instance()
              .Snapshot()
              .FrameRate);
  const auto resolveFrameCompositionPolicy =
      [&](const Fast::Oot3d::PresentationPacingPolicy& pacing) {
        auto policy = pacing.Composition;
        policy.NativeRateHz = frameRateContract.SimulationRateHz;
        if (!frameRateContract.VisualInterpolationAllowed) {
          policy.Interpolation =
              Fast::Oot3d::NativeVisualInterpolationMode::Disabled;
          policy.FixedSampleMultiplier = 1U;
        }
        return policy;
      };
  auto frameCompositionPolicy =
      resolveFrameCompositionPolicy(presentationPacingPolicy);
  visualSampleCadence.Configure(
      frameCompositionPolicy.FixedMultiplier()
          ? frameCompositionPolicy.FixedSampleMultiplier
          : 0U);
  Oot3dNativeGame::NativeRealtimeRefreshPacer realtimePacer(
      hostArgs.FrameLimit == 0U && !hostArgs.ThroughputBenchmark,
      presentationPacingPolicy.TargetRateHz);
  realtimePacer.Configure(presentationPacingPolicy.Enabled &&
                              !hostArgs.ThroughputBenchmark,
                          presentationPacingPolicy.TargetRateHz);
  bool visualInterpolationEnabled =
      Oot3dNativeGame::ShouldUseNativeVisualInterpolation(
          frameRateContract,
          frameCompositionPolicy.InterpolationEnabled());
  // The two values above belong to the renderer. The guest detects policy
  // changes against its own copies.
  auto guestCompositionPolicy = frameCompositionPolicy;
  bool guestInterpolationEnabled = visualInterpolationEnabled;
  auto lastPresentationTime = std::chrono::steady_clock::now();
  NativeAudioOutputDiagnostics audioOutputDiagnostics;
  Oot3dNativeGame::NativePcmContinuityDiagnostics pcmContinuityDiagnostics;
  std::vector<int16_t> pcmDumpSamples;
  FramebufferScreenshotState screenshotState;
  uint64_t lastPublishedSceneViewSerial = 0;
  uint64_t publishedSceneViewCount = 0;
  // Renderer work of the loop, in program order. Inline: runs at submission.
  // Declared after every object its commands touch, so destruction drains it
  // first.
  Oot3dNativeGame::NativeRenderCommandStream renderStream;
  const auto publishNativeSceneView = [&]() {
    // Guest memory is read here; the scene environment, the duplicate check
    // and the publish belong to the renderer and run in its order.
    auto snapshot = sceneViewProbe.Capture(process.Memory());
    renderStream.Submit([&, snapshot] {
    sceneEnvironment->ObserveScene(snapshot ? snapshot->SceneId : std::nullopt);
    if (!snapshot.has_value() ||
        snapshot->Serial == lastPublishedSceneViewSerial) {
      return;
    }
    const Fast::Oot3d::TitleSceneViewSubmission view{
        snapshot->GuestFunction,
        snapshot->GuestReturnAddress,
        snapshot->Left,
        snapshot->Right,
        snapshot->Bottom,
        snapshot->Top,
        snapshot->NearPlane,
        snapshot->FarPlane,
        snapshot->Eye,
        snapshot->At,
        snapshot->CameraAvailable,
        // Rooms share their scene's coordinates; another scene is another world.
        snapshot->SceneId.has_value() ? uint64_t{*snapshot->SceneId} + 1U : 0U,
    };
    if (titleRenderBackend != nullptr &&
        titleRenderBackend->PublishSceneView(view)) {
      lastPublishedSceneViewSerial = snapshot->Serial;
      ++publishedSceneViewCount;
    }
    });
  };
  picaPresentationScheduler.SetTimingEnabled(launch.ExtendedDiagnostics);
  // Guest memory and guest interrupts belong to the guest. The renderer
  // reaches them only from a command the guest waits for (inline or Run);
  // anything else would race the running guest, so it fails instead.
  const auto requireStoppedGuest = [&]() {
    if (renderStream.InCommand() && !renderStream.InSynchronousCommand()) {
      throw std::runtime_error(
          "renderer reached guest state while the guest was running");
    }
  };
  api.SetPicaPhysicalMemoryAccess(
      [&](uint32_t address, std::span<uint8_t> bytes) {
        requireStoppedGuest();
        return picaMemoryView.Read(address, bytes);
      },
      [&](std::span<const Fast::Renderer3ds::PicaPhysicalMemoryWrite> writes) {
        requireStoppedGuest();
        // The writeback is the renderer writing the copy itself, not a guest or
        // service access; keep it out of the guest-access counters. Scope, not
        // a pair of calls: an early return must not leave the watch suppressed.
        {
        const Oot3dNativeGame::NativeA32Memory::RangeWatchSuppression suppressed(
            process.Memory());
        for (const auto& write : writes) {
          if (!picaMemoryView.ForEachGuestAlias(write.Address, write.Bytes.size(),
              [&](uint32_t address, size_t size) { return process.Memory().IsWritable(address, size); }))
            return false;
        }
        for (const auto& write : writes) {
          if (!picaMemoryView.ForEachGuestAlias(write.Address, write.Bytes.size(),
              [&](uint32_t address, size_t) { return process.Memory().WriteBytes(address, write.Bytes); }))
            return false;
        }
        }
        return true;
      },
      [&](std::span<const Fast::Renderer3ds::PicaRawCopySpan> spans,
          Fast::Renderer3ds::PicaRenderBackend::PhysicalMemoryRead reader) {
        requireStoppedGuest();
        std::vector<std::pair<uint32_t, size_t>> ranges;
        for (const auto& span : spans) {
          if (!picaMemoryView.ForEachGuestAlias(span.DestinationAddress, span.ByteCount,
              [&](uint32_t address, size_t size) { ranges.emplace_back(address, size); return true; }))
            return false;
        }
        return process.Memory().DeferWrites(ranges,
            [&, reader = std::move(reader)](uint32_t address, std::span<uint8_t> bytes) {
              const auto physical = picaMemoryView.TranslateGuest(address, bytes.size());
              if (!physical) return false;
              // The bytes live in a renderer-owned GPU copy: a guest read
              // reaches it after every renderer command before it.
              if (renderStream.InCommand() ||
                  renderStream.CurrentMode() ==
                      Oot3dNativeGame::NativeRenderCommandStream::Mode::Inline) {
                return reader(*physical, bytes);
              }
              bool read = false;
              renderStream.Run([&] { read = reader(*physical, bytes); });
              return read;
            });
      },
      [&](uint64_t) {
        requireStoppedGuest();
        if (!hostServices.SignalPicaInterrupt(process.Memory(), Oot3dNativeGame::Oot3dPicaInterruptId::Ppf)) return false;
        ++completedPpfCount;
        ++recordedCompletionCount;
        return true;
      },
      [&]() {
        requireStoppedGuest();
        return process.Memory().MaterializeDeferredWrites();
      });
  struct PhysicalMemoryAccessLifetime {
    Fast::Renderer3ds::PicaRenderBackend& Backend;
    ~PhysicalMemoryAccessLifetime() { Backend.SetPicaPhysicalMemoryAccess({}, {}, {}, {}, {}); }
  } physicalMemoryAccessLifetime{api};
  std::optional<Fast::Oot3d::NativeFrameTemporalSample> capturedTemporalSample;
  std::vector<Fast::Renderer3ds::PicaTextureCopyReference> pinnedCopyReferences;
  // Renderer-side state written only by render commands (or at a drain).
  // Inline mode points the timings at the loop's own phase timing.
  NativeFramePhaseTiming workerPhaseTiming;
  NativeFramePhaseTiming* rendererTiming = &phaseTiming;
  std::string renderError;
  // A renderer frame that acquired no image skips its drawing commands. In
  // worker mode the guest learns it at its next frame begin and synchronizes.
  bool rendererFrameSkipped = false;
  std::atomic<bool> rendererFrameWasInactive{false};
  // Copy references still owned by the guest side (submission queue and the
  // accumulator), snapshotted where the serial loop read them.
  std::vector<Fast::Renderer3ds::PicaTextureCopyReference> guestCopyReferences;
  const auto collectGuestCopyReferences = [&]() {
    std::vector<Fast::Renderer3ds::PicaTextureCopyReference> references;
    submissionQueue.CollectLiveCopyReferences(references);
    for (const auto& draw : submissionQueue.PendingDraws()) {
      for (const auto& resource : draw.Resources) {
        references.insert(references.end(), resource.CopyReferences.begin(),
                          resource.CopyReferences.end());
      }
    }
    for (const auto& plan : picaPresentationScheduler.PendingDraws()) {
      Oot3dNativeGame::CollectOot3dPicaPlanCopyReferences(plan, references);
    }
    return references;
  };
  const auto pinGpuCopyResources =
      [&](const Oot3dNativeGame::Oot3dPicaVisualFrameViewSample& sample) {
        pinnedCopyReferences = guestCopyReferences;
        if (previousVisualFrame) {
          Oot3dNativeGame::CollectOot3dPicaFrameCopyReferences(
              *previousVisualFrame, pinnedCopyReferences);
        }
        if (latestVisualFrame) {
          Oot3dNativeGame::CollectOot3dPicaFrameCopyReferences(
              *latestVisualFrame, pinnedCopyReferences);
        }
        // The active sample may already have left the accumulator, or may be
        // the new frame before it replaces latestVisualFrame.
        for (const auto& draw : sample.Draws) {
          if (draw.BasePlan != nullptr) {
            Oot3dNativeGame::CollectOot3dPicaPlanCopyReferences(
                *draw.BasePlan, pinnedCopyReferences);
          }
        }
        if (!api.SetPinnedPicaTextureCopies(pinnedCopyReferences, &renderError)) {
          throw std::runtime_error("native PICA copy retention failed: " + renderError);
        }
      };
  Oot3dNativeGame::Oot3dPicaVisualFrameViewSample dependencySampleScratch;
  const auto flushNativeDependencies = [&]() {
    // The accumulator is guest-owned, so whether there is dependency work is
    // known here; only real work waits for the renderer.
    if (!picaPresentationScheduler.ViewDependencyWork(dependencySampleScratch))
      return false;
    // Synchronous: raw copies read and write guest memory and signal PPF
    // while the guest waits.
    bool flushed = false;
    renderStream.Run([&] {
      // Without an acquired image the flush waits; PPF arrives later, as it
      // may on hardware.
      if (rendererFrameSkipped) return;
      guestCopyReferences = collectGuestCopyReferences();
      pinGpuCopyResources(dependencySampleScratch);
      if (!picaPresentationScheduler.Execute(
              api, dependencySampleScratch, kNativeDependencyRenderTargetNamespace,
              Oot3dNativeGame::Oot3dPicaPresentationExecutionKind::DependencyFlush,
              false, &renderError)) {
        throw std::runtime_error("native PICA dependency flush failed: " + renderError);
      }
      flushed = true;
    });
    return flushed;
  };
  const auto executeVisualSample =
      [&](const Oot3dNativeGame::Oot3dPicaVisualFrameViewSample &sample,
          Oot3dNativeGame::Oot3dPicaPresentationExecutionKind kind,
          bool present,
          const Fast::Oot3d::NativeFrameTemporalSample& temporalSample,
          uint64_t renderTargetNamespace =
              kVisualInterpolationRenderTargetNamespace) {
        const auto started = std::chrono::steady_clock::now();
        (void)api.PublishPicaFrameTemporalSample(temporalSample);
        Fast::Oot3d::GrassInteractionBridge::Instance().SelectSample(temporalSample);
        pinGpuCopyResources(sample);
        if (!picaPresentationScheduler.Execute(
                api, sample, renderTargetNamespace, kind,
                present, &renderError)) {
          throw std::runtime_error(
              "native PICA scheduled presentation failed: " + renderError);
        }
        const double elapsed = SecondsSince(started);
        if (present) {
          capturedTemporalSample = temporalSample;
        }
        rendererTiming->PicaBackendSeconds += elapsed;
        rendererTiming->VisualReplaySeconds += elapsed;
      };
  Oot3dNativeGame::Oot3dPicaVisualFrameViewSample visualFrameSampleScratch;
  // Declared after every renderer-owned local the commands reference: leaving
  // the loop (normally or by exception) stops the worker before they die.
  struct RenderStreamStop {
    Oot3dNativeGame::NativeRenderCommandStream& Stream;
    ~RenderStreamStop() {
      try {
        Stream.SetMode(Oot3dNativeGame::NativeRenderCommandStream::Mode::Inline);
      } catch (...) {
      }
    }
  } renderStreamStop{renderStream};
  std::vector<Oot3dNativeGame::Oot3dPicaDrawSubmission> drainedDraws;
  bool quickSaveKeyWasDown = false;
  bool quickLoadKeyWasDown = false;
  bool quickSavePending = false;
  bool automatedStateSaved = false;
  std::optional<uint32_t> automatedStateSavedAtRunFrame;
  std::optional<uint32_t> automatedStateSavedAtGuestFrame;

  const auto saveState = [&](const std::filesystem::path &path) {
    // Barrier: the savestate captures renderer-owned replay state.
    renderStream.Drain();
    // Host-owned continuation registers must never escape into a standalone savestate.
    if (ocarinaText.Pending()) return false;
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
      std::cerr
          << "oot3d_native_game: native PICA visual replay capture failed: "
          << error << '\n';
      return false;
    }
    runtime.PicaVisualReplayStateAvailable = true;
    savestatePicaVisualReplayBytesCaptured +=
        runtime.PicaVisualReplayState.size();
    if (!api.CapturePicaTextureCache(
            runtime.PicaTextureCache, &error)) {
      std::cerr
          << "oot3d_native_game: native PICA texture capture failed: "
          << error << '\n';
      return false;
    }
    runtime.PicaTextureCacheAvailable = true;
    for (const auto &texture : runtime.PicaTextureCache) {
      savestatePicaTextureBytesCaptured += texture.PixelBytes.size();
    }
    savestatePicaTexturesCaptured += runtime.PicaTextureCache.size();
    if (!api.CapturePicaColorTargets(
            runtime.PicaColorTargets, &error)) {
      std::cerr
          << "oot3d_native_game: native PICA color capture failed: "
          << error << '\n';
      return false;
    }
    runtime.PicaColorTargetsAvailable = true;
    for (const auto &target : runtime.PicaColorTargets) {
      savestatePicaColorTargetBytesCaptured +=
          target.ColorRgba8.size();
    }
    savestatePicaColorTargetsCaptured += runtime.PicaColorTargets.size();
    if (!api.CapturePicaPresentationState(
            runtime.PicaPresentationState, &error)) {
      std::cerr
          << "oot3d_native_game: native PICA presentation capture failed: "
          << error << '\n';
      return false;
    }
    runtime.PicaPresentationStateAvailable = true;
    for (const auto &image :
         runtime.PicaPresentationState.DisplayImages) {
      savestatePicaDisplayImageBytesCaptured +=
          image.ColorRgba8.size();
    }
    savestatePicaDisplayImagesCaptured +=
        runtime.PicaPresentationState.DisplayImages.size();
    Oot3dNativeGame::NativeA32SavestateIoResult ioResult;
    if (!Oot3dNativeGame::SaveNativeA32State(
            path, savestateCompatibility, runtime, process, hostServices,
            picaFrontend, submissionQueue, dspHle, &ioResult, &error)) {
      std::cerr << "oot3d_native_game: savestate save failed: " << error
                << '\n';
      return false;
    }
    ++savestateSaveCount;
    savestateSaveBytes += ioResult.FileBytes;
    lastSavestateSemanticFingerprint = ioResult.SemanticFingerprint;
    savestateSaveSeconds += SecondsSince(started);
    savestateCaptureSeconds += ioResult.CaptureSeconds;
    savestateEncodeSeconds += ioResult.EncodeSeconds;
    savestateWriteSeconds += ioResult.IoSeconds;
    std::cout << "oot3d_native_game: saved state at frame " << frameCount
              << " to " << path.string() << '\n';
    return true;
  };
  const auto loadState = [&](const std::filesystem::path &path) {
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
      return false;
    }
    Oot3dNativeGame::ResetOot3dTypedGameplayTransientState();
    picaCompositionTracker.Reset();
    const auto restoredGravity = hostServices.HidRuntimeProfile().LastAccelerometer;
    nativeControlPollingState.VirtualMotion.RestoreGravity(
        {static_cast<float>(restoredGravity[0]), static_cast<float>(restoredGravity[1]),
         static_cast<float>(restoredGravity[2])});
    nativeControlPollingState.PendingMouseDeltaX = 0;
    nativeControlPollingState.PendingMouseDeltaY = 0;
    nativeControlPollingState.PendingInputSeconds = 0.0;
    applySelectedUiProfile();
    if (!api.ResetPicaState(&error)) {
      throw std::runtime_error(
          "native PICA reset after savestate load failed: " + error);
    }
    if (titleRenderBackend != nullptr &&
        !titleRenderBackend->ResetTitleState(&error)) {
      throw std::runtime_error(
          "OOT3D renderer reset after savestate load failed: " + error);
    }
    restoreSavestatePicaTextureCache(runtime);
    restoreSavestatePicaColorTargets(runtime);
    restoreSavestatePicaPresentationState(runtime);
    if (audioPlayer != nullptr && audioPlayer->IsInitialized()) {
      audioPlayer->Flush();
    }
    frameCount = runtime.FrameCount;
    refreshTickRemainder = runtime.RefreshTickRemainder;
    nextVblankTick = runtime.NextVblankTick;
    processResult = {};
    processResult.Kind = static_cast<Oot3dNativeGame::NativeA32ProcessRunKind>(
        runtime.ProcessRunKind);
    restoreSavestateTiming(runtime);
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
    restoreSavestateTopScreenTemporalState(runtime);
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
    restoreSavestatePicaVisualReplayState(runtime);
    pcmContinuityDiagnostics = {};
    realtimePacer.ResetDeadline();
    lastPresentationTime = std::chrono::steady_clock::now();
    ++savestateLoadCount;
    savestateLoadBytes += ioResult.FileBytes;
    savestateLoadSeconds += SecondsSince(started);
    savestateReadSeconds += ioResult.IoSeconds;
    savestateDecodeSeconds += ioResult.DecodeSeconds;
    savestateRestoreSeconds += ioResult.RestoreSeconds;
    std::cout << "oot3d_native_game: loaded state at frame " << frameCount
              << " from " << path.string() << '\n';
    return true;
  };

  auto benchmarkMeasurementStart = std::chrono::steady_clock::now();
  auto benchmarkMeasurementEnd = benchmarkMeasurementStart;
  uint64_t benchmarkMeasuredFrames = 0U;
  std::array<double, 15> benchmarkPhaseSeconds{};
  double benchmarkNestedGuestSeconds = 0.0;
  double benchmarkGuestClockSeconds = 0.0;
  double benchmarkUnattributedSeconds = 0.0;
  Fast::Renderer::FrameTimeDistribution preBackendTimes;
  Fast::Renderer::FrameTimeDistribution preBackendEnvelopeTimes;
  uint64_t invalidPreBackendIntervals = 0;
  Fast::Renderer::SlowFrameSamples<15> slowFrames;
  const char* pacingTrace = std::getenv("TRIAEVUM_PACING_TRACE");
  const bool measurePacing = pacingTrace && std::string_view(pacingTrace) == "1";
  auto benchmarkFrameTimes = (hostArgs.ThroughputBenchmark || measurePacing)
      ? std::make_unique<Fast::Renderer::FrameTimeDistribution>() : nullptr;
  // Measurement starts after the initial preparation and the warm-up frames.
  bool benchmarkMeasurementStarted = false;
  auto lastCompletedFrameTime = std::chrono::steady_clock::now();
  const char* cpuProbeEnvironment = std::getenv("TRIAEVUM_PROFILE_CPU_LEDGER");
  const bool cpuProbeRequested = CpuProbe::BuildEnabled && cpuProbeEnvironment &&
      std::string_view(cpuProbeEnvironment) == "1";
  CpuProbe::Totals benchmarkCpuPhases{};

  uint32_t guestFrameBeginWidth = 0;
  uint32_t guestFrameBeginHeight = 0;
  uint64_t guestFrameBeginSettings = 0;
  bool guestFrameBeginNativePresentation = false;
  const char* parallelPresentationEnvironment =
      std::getenv("TRIAEVUM_PARALLEL_PRESENTATION");
  // Harnesses override the launch choice: "0" serial, "1" parallel.
  const bool parallelPresentation =
      parallelPresentationEnvironment != nullptr &&
              (std::string_view(parallelPresentationEnvironment) == "0" ||
               std::string_view(parallelPresentationEnvironment) == "1")
          ? std::string_view(parallelPresentationEnvironment) == "1"
          : launch.ParallelPresentation;
  auto* const fast3dGui = dynamic_cast<Fast::Fast3dGui*>(gui.get());
  if (parallelPresentation) {
    // Renderer timings accumulate apart and merge once the stream drains.
    rendererTiming = &workerPhaseTiming;
    renderStream.SetMode(
        Oot3dNativeGame::NativeRenderCommandStream::Mode::Worker);
    // The ImGui overlay (and the display composition it triggers) renders
    // in renderer order, from a copy of each frame's draw data.
    if (fast3dGui != nullptr) {
      fast3dGui->SetRendererRouting(
          {[&](std::function<void()> work) { renderStream.Run(std::move(work)); },
           [&](std::function<void()> work) {
             renderStream.Submit(std::move(work));
           }});
    }
  }
  // The routing refers to this loop's stream: removed on every exit.
  struct GuiRendererRoutingReset {
    Fast::Fast3dGui* Gui;
    ~GuiRendererRoutingReset() {
      if (Gui != nullptr) Gui->SetRendererRouting({});
    }
  } guiRendererRoutingReset{parallelPresentation ? fast3dGui : nullptr};

#if defined(__SWITCH__)
  WriteSwitchBootStage("runtime_loop");
#endif
  while (window.IsRunning() &&
         processResult.Kind !=
             Oot3dNativeGame::NativeA32ProcessRunKind::Terminated) {
    const auto phaseBeforeFrame = phaseTiming.Values();
    const double nestedGuestBeforeFrame = phaseTiming.GuestInsidePicaSubmitSeconds;
    const double guestClockBeforeFrame = phaseTiming.GuestClockSeconds;
    widescreenProjection.CurrentHostFrame =
        static_cast<uint32_t>(presentationFrameCount);
    WindowDemoFrameTiming frameTiming;
    if (!PrepareNextWindowDemoFrame(hostArgs, window, timing, frameTiming)) {
      continue;
    }
    const auto requestedPresentationPolicy =
        Fast::Oot3d::ResolvePresentationPacingPolicy(
            Fast::Oot3d::GraphicsSettingsRuntime::Instance()
                .Snapshot()
                .FrameRate);
    if (Fast::Oot3d::PresentationPacingPolicyChanged(
            presentationPacingPolicy, requestedPresentationPolicy)) {
      const auto requestedCompositionPolicy =
          resolveFrameCompositionPolicy(requestedPresentationPolicy);
      const bool previousInterpolationEnabled = guestInterpolationEnabled;
      const bool requestedInterpolationEnabled =
          Oot3dNativeGame::ShouldUseNativeVisualInterpolation(
              frameRateContract,
              requestedCompositionPolicy.InterpolationEnabled());
      const bool compositionModeChanged =
          guestCompositionPolicy.Interpolation !=
              requestedCompositionPolicy.Interpolation ||
          guestCompositionPolicy.FixedSampleMultiplier !=
              requestedCompositionPolicy.FixedSampleMultiplier;
      presentationPacingPolicy = requestedPresentationPolicy;
      guestCompositionPolicy = requestedCompositionPolicy;
      guestInterpolationEnabled = requestedInterpolationEnabled;
      renderStream.Submit([&, requestedPresentationPolicy,
                           requestedCompositionPolicy,
                           requestedInterpolationEnabled,
                           previousInterpolationEnabled,
                           compositionModeChanged] {
      frameCompositionPolicy = requestedCompositionPolicy;
      realtimePacer.Configure(requestedPresentationPolicy.Enabled &&
                                  !hostArgs.ThroughputBenchmark,
                              requestedPresentationPolicy.TargetRateHz);
      visualSampleCadence.Configure(
          frameCompositionPolicy.FixedMultiplier()
              ? frameCompositionPolicy.FixedSampleMultiplier
              : 0U);
      visualInterpolationEnabled = requestedInterpolationEnabled;
      if (compositionModeChanged) {
        ++visualContinuityEpoch;
      }
      if (previousInterpolationEnabled != visualInterpolationEnabled) {
        preparedVisualTransition.Reset();
        visualContinuityTracker = {};
        previousVisualFrame.reset();
        latestVisualFrame.reset();
        latestVisualTransitionContinuous = false;
        visualSampleCadence.Reset();
      }
      });
    }
    const auto presentationTime = std::chrono::steady_clock::now();
    renderStream.Submit([&, presentationTime] {
      realtimePacer.ObservePresentation(presentationTime);
    });
    const double presentationElapsedSeconds =
        hostArgs.FixedDeltaSeconds > 0.0
            ? hostArgs.FixedDeltaSeconds
            : std::chrono::duration<double>(presentationTime -
                                            lastPresentationTime)
                  .count();
    lastPresentationTime = presentationTime;
    const auto presentationStateBeforeFrame = presentationScheduler.CaptureState();
    const auto presentationStep =
        presentationScheduler.Advance(presentationElapsedSeconds);
    const uint32_t guestRefreshesDue = presentationStep.GuestRefreshesDue;
    const float visualInterpolationAlpha = static_cast<float>(
        std::clamp(presentationStep.InterpolationAlpha, 0.0, 1.0));
    applyTopScreenConfigSnapshot();
    uiLifecycleBridge.BeginHostFrame(
        static_cast<uint32_t>(presentationFrameCount));
    if (topScreenTextureOverrideRuntime.has_value()) {
      topScreenTextureOverrideRuntime->Observe(uiLifecycleBridge);
    }

    const uint32_t width = std::max<uint32_t>(1, window.GetWidth());
    const uint32_t height = std::max<uint32_t>(1, window.GetHeight());
    hdFonts.SetOutputHeight(height);
    updateScenePresentation();
    phaseStart = std::chrono::steady_clock::now();
    Fast::Renderer3ds::SlowStepTrace hostStartSteps("host_frame_start");
    window.GetMouseStateManager()->StartFrame();
    hostStartSteps.Mark("mouse");
    gui->StartDraw();
    hostStartSteps.Mark("gui_start_draw");
    renderStream.Submit([&] { window.StartFrame(); });
    hostStartSteps.Mark("render_start_frame");
    phaseTiming.HostFrameStartSeconds += SecondsSince(phaseStart);
    const auto inputPhaseStart = std::chrono::steady_clock::now();
    const bool quickSaveKeyDown = window.IsKeyDown(Ship::KbScancode::LUS_KB_F5);
    const bool quickLoadKeyDown = window.IsKeyDown(Ship::KbScancode::LUS_KB_F8);
    quickSavePending =
        quickSavePending || (quickSaveKeyDown && !quickSaveKeyWasDown);
    const bool quickLoadRequested = quickLoadKeyDown && !quickLoadKeyWasDown;
    quickSaveKeyWasDown = quickSaveKeyDown;
    quickLoadKeyWasDown = quickLoadKeyDown;
    bool nativeFrontendTouchEnabled = ResolveNativeA32TouchInputEnabled(
        process.Memory(), uiLifecycleBridge,
        nativeCandidateDispatch.TopScreenUiProfile);
    uint32_t topScreenNativeGameMode = UINT32_MAX;
    const bool topScreenGameplayInputMode =
        process.Memory().Read32(kNativeGameMode, &topScreenNativeGameMode) &&
        Oot3dNativeGame::IsTopScreenGameplayInputMode(topScreenNativeGameMode);
    picaCompositionDomain =
        topScreenGameplayInputMode
            ? Oot3dNativeGame::Oot3dPicaCompositionDomain::Scene
            : Oot3dNativeGame::Oot3dPicaCompositionDomain::Unknown;
    bool topScreenStartRoutingEligible =
        uiLifecycleBridge.NativeGameplayPresentationActive() &&
        topScreenGameplayInputMode;
    Oot3dNativeGame::NativeAimProfileTransform aimProfileTransform;
    if (nativeCandidateDispatch.TopScreenUiProfile) {
      const auto topScreenAim =
          Oot3dNativeGame::ResolveTopScreenCStickAimPolicy(
              activeTopScreenConfig);
      aimProfileTransform.RightStickScale = topScreenAim.SpeedMultiplier;
      aimProfileTransform.RightStickSmoothingCoefficient =
          Oot3dNativeGame::ResolveTopScreenCStickSmoothingCoefficient(
              activeTopScreenConfig.FreeCameraSmoothing);
      aimProfileTransform.RightStickInvertX = topScreenAim.InvertX;
      aimProfileTransform.RightStickInvertY = topScreenAim.InvertY;
    }
    const bool hostGuiVisible =
        gui->GetMenuOrMenubarVisible() || gui->GetAnyGuiWindowVisible();
    auto physicalInputFrame =
        PollNativeA32Input(window, nativeFrontendTouchEnabled,
                           hostGuiVisible,
                           nativeCandidateDispatch.TopScreenUiProfile,
                           activeTopScreenConfig.FreeCameraEnabled,
                           *controlConfigRuntime, aimProfileTransform,
                           presentationElapsedSeconds,
                           guestRefreshesDue != 0U,
                           nativeControlPollingState);
    if (physicalInputFrame.Exit) {
      window.Close();
      break;
    }
    // Developer qualification: scheduled settings take the F1 commit path
    // (applied, not persisted) before this frame is rendered.
    for (const auto *step : graphicsSettingsSchedule.TakeDue(runFrameCount)) {
      auto &graphicsSettings = Fast::Oot3d::GraphicsSettingsRuntime::Instance();
      const auto current = graphicsSettings.Snapshot();
      const auto decoded = Fast::Oot3d::DeserializeGraphicsSettings(
          Oot3dNativeGame::MergeScheduledGraphics(
              Fast::Oot3d::SerializeGraphicsSettings(current), step->Graphics),
          current);
      const auto applied = graphicsSettings.Apply(decoded.Value, false);
      std::fprintf(stderr,
                   "TRIAEVUM_GRAPHICS_SETTINGS_SCHEDULE frame=%u accepted=%d "
                   "decode_issues=%zu apply_issues=%zu\n",
                   step->Frame, applied.Accepted() ? 1 : 0,
                   decoded.Issues.size(), applied.Issues.size());
      if (decoded.UnsupportedFutureVersion || !applied.Accepted())
        throw std::runtime_error(
            "graphics settings schedule step at frame " +
            std::to_string(step->Frame) + " was rejected");
    }
    if (!inputTimeline.Enabled()) {
      const uint32_t start = Oot3dNativeGame::NativeA32HidButtonMask(
          Oot3dNativeGame::NativeA32HidButton::Start);
      const bool physicalStartHeld =
          (physicalInputFrame.Hid.Buttons & start) != 0U;
      Oot3dNativeGame::ObserveNativeA32PolledButton(
          physicalStartHeld, nativeCandidateDispatch.StartButtonLatch);
      Oot3dNativeGame::ReleaseTopScreenStartRoutingLatch(
          physicalStartHeld, nativeCandidateDispatch.TopScreenStartRouting);
    }
    Oot3dNativeGame::NativeA32InputFrame inputFrame;
    phaseTiming.InputPollSeconds += SecondsSince(inputPhaseStart);
    const auto rendererFrameStart = std::chrono::steady_clock::now();
    CpuProbe::Scope cpuRendererStart(CpuProbe::Phase::RendererStart);
    const auto beginRendererFrame = [&, width, height,
                                     presentationSerial =
                                         presentationFrameCount] {
      api.UpdateFramebufferParameters(0, width, height, 1, false, true, true,
                                      true);
      if (titleRenderBackend != nullptr) {
        // Display settings are window operations: applied only by a frame
        // begun on the window's thread.
        titleRenderBackend->SetPresentationSettingsHeld(
            !renderStream.InSynchronousCommand());
      }
      api.StartFrame();
      rendererFrameSkipped = !api.HasActiveFrame();
      if (rendererFrameSkipped) {
        rendererFrameWasInactive.store(true);
        return;
      }
      picaPresentationScheduler.BeginPresentation(presentationSerial);
      api.StartDrawToFramebuffer(0, 1.0F);
      api.SetClearColor(0.0F, 0.0F, 0.0F, 1.0F);
      api.ClearFramebuffer(true, true);
      api.SetViewport(0, 0, static_cast<int>(width), static_cast<int>(height));
      api.SetScissor(0, 0, static_cast<int>(width), static_cast<int>(height));
    };
    // A frame begin may recreate targets (and so read back GPU copies into
    // guest memory) after a resize or a graphics settings change, may apply
    // display settings (window operations), and an inactive frame must not
    // advance the guest: those begin synchronously, on this thread.
    // Otherwise the guest goes on while the renderer finishes the previous
    // frame.
    const auto renderSettings = Fast::Oot3d::GraphicsSettingsRuntime::Instance()
                                    .SnapshotWithRevision()
                                    .Revision;
    const bool renderNativePresentation =
        Fast::Oot3d::GraphicsSettingsRuntime::Instance()
            .NativePresentationOverrideActive();
    const bool presentationTransactionActive =
        Fast::Oot3d::GraphicsSettingsRuntime::Instance()
            .PresentationStatus()
            .Phase != Fast::Oot3d::PresentationTransactionPhase::Idle;
    const bool previousRendererFrameInactive =
        rendererFrameWasInactive.exchange(false);
    const bool synchronousFrameBegin =
        renderStream.CurrentMode() ==
            Oot3dNativeGame::NativeRenderCommandStream::Mode::Inline ||
        presentationTransactionActive || previousRendererFrameInactive ||
        width != guestFrameBeginWidth ||
        height != guestFrameBeginHeight ||
        renderSettings != guestFrameBeginSettings ||
        renderNativePresentation != guestFrameBeginNativePresentation;
    guestFrameBeginWidth = width;
    guestFrameBeginHeight = height;
    guestFrameBeginSettings = renderSettings;
    guestFrameBeginNativePresentation = renderNativePresentation;
    bool rendererFrameActive = true;
    if (synchronousFrameBegin) {
      renderStream.Run(beginRendererFrame);
      // The renderer is idle after Run: its frame state is readable.
      rendererFrameActive = !rendererFrameSkipped;
    } else {
      renderStream.Submit(beginRendererFrame);
    }
    if (!rendererFrameActive) {
      // A minimized or changing surface is not a guest/PICA failure. Keep events
      // and UI balanced, but do not consume guest time without an acquired frame.
      if (!presentationScheduler.Restore(presentationStateBeforeFrame, &error)) {
        throw std::runtime_error("cannot restore suspended presentation clock: " + error);
      }
      gui->EndDraw();
      renderStream.Submit([&] { window.EndFrame(); }, true);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      renderStream.Submit([&] { realtimePacer.ResetDeadline(); });
      lastPresentationTime = std::chrono::steady_clock::now();
      continue;
    }
    const auto preparationStatus = programPreparation != nullptr
        ? programPreparation->PreparationStatus()
        : Fast::Renderer3ds::PicaPreparationStatus{};
    if (preparationScreen.HoldsGuest(preparationStatus,
                                     std::chrono::steady_clock::now())) {
      // The guest waits for the initial preparation: its clock does not run
      // and the frame shows the progress instead of a game image.
      if (!presentationScheduler.Restore(presentationStateBeforeFrame, &error)) {
        throw std::runtime_error("cannot restore the presentation clock during preparation: " + error);
      }
      preparationScreen.Draw(preparationStatus);
      gui->EndDraw();
      renderStream.Submit([&] { window.EndFrame(); }, true);
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      renderStream.Submit([&] { realtimePacer.ResetDeadline(); });
      lastPresentationTime = std::chrono::steady_clock::now();
      lastCompletedFrameTime = lastPresentationTime;
      continue;
    }
    phaseTiming.RendererFrameStartSeconds += SecondsSince(rendererFrameStart);
    cpuRendererStart.Stop();
    phaseTiming.FrameStartSeconds += SecondsSince(phaseStart);
    const uint32_t guestRefreshIterations =
        std::max<uint32_t>(1U, guestRefreshesDue);
    for (uint32_t guestRefreshIndex = 0;
         guestRefreshIndex < guestRefreshIterations; ++guestRefreshIndex) {
      const bool advanceGuest = guestRefreshIndex < guestRefreshesDue;
      const bool presentHostFrame =
          guestRefreshIndex + 1U == guestRefreshIterations;
      if (advanceGuest) {
        if (guestRefreshIndex != 0U) {
          nativeFrontendTouchEnabled = ResolveNativeA32TouchInputEnabled(
              process.Memory(), uiLifecycleBridge,
              nativeCandidateDispatch.TopScreenUiProfile);
          topScreenNativeGameMode = UINT32_MAX;
          const bool refreshedTopScreenGameplayInputMode =
              process.Memory().Read32(kNativeGameMode,
                                      &topScreenNativeGameMode) &&
              Oot3dNativeGame::IsTopScreenGameplayInputMode(
                  topScreenNativeGameMode);
          topScreenStartRoutingEligible =
              uiLifecycleBridge.NativeGameplayPresentationActive() &&
              refreshedTopScreenGameplayInputMode;
          picaCompositionDomain =
              refreshedTopScreenGameplayInputMode
                  ? Oot3dNativeGame::Oot3dPicaCompositionDomain::Scene
                  : Oot3dNativeGame::Oot3dPicaCompositionDomain::Unknown;
        }
        // Re-arm after any state restore: RestoreState replaces the memory
        // image, and the watch is diagnostic state that must not disappear with
        // it. Cheap when the trapped page set is unchanged.
        if (!rawCopyWatchRanges.empty() &&
            !process.Memory().RangeWatchActive()) {
          process.Memory().SetRangeWatch(rawCopyWatchRanges);
        }
        inputFrame = ResolveNativeA32GuestInput(
            physicalInputFrame, inputTimeline, frameCount, runFrameCount,
            nativeFrontendTouchEnabled, topScreenStartRoutingEligible,
            nativeCandidateDispatch.TopScreenUiProfile,
            nativeCandidateDispatch.StartButtonLatch,
            nativeCandidateDispatch.TopScreenStartRouting);
        inputDiagnostics.Observe(inputFrame);
        if (inputFrame.Exit) {
          window.Close();
          break;
        }
      }
      if (advanceGuest) {
        const uint32_t topScreenButtons = inputFrame.Hid.Buttons;
        const uint32_t topScreenPressed =
            topScreenButtons &
            ~nativeCandidateDispatch.PreviousTopScreenButtons;
        const auto topScreenButton =
            [](Oot3dNativeGame::NativeA32HidButton value) {
              return Oot3dNativeGame::NativeA32HidButtonMask(value);
            };
        Oot3dNativeGame::TopScreenDpadActionState topScreenDpadActions;
        Oot3dNativeGame::TopScreenOcarinaState ocarinaOwner;
        const bool ocarinaOwnsDpad = nativeCandidateDispatch.TopScreenUiProfile &&
            Oot3dNativeGame::ReadTopScreenOcarinaState(process.Memory(), &ocarinaOwner) &&
            ocarinaOwner.Active;
        if (nativeCandidateDispatch.TopScreenUiProfile &&
            topScreenStartRoutingEligible && !ocarinaOwnsDpad) {
          bool childLink = false;
          std::string childLinkError;
          if (!Oot3dNativeGame::ReadTopScreenChildLink(
                  process.Memory(), &childLink, &childLinkError)) {
            throw std::runtime_error("TopScreen Link age decode failed: " +
                                     childLinkError);
          }
          Oot3dNativeGame::TopScreenDpadPhysicalState dpadPhysical;
          dpadPhysical.ChildLink = childLink;
          constexpr std::array<Oot3dNativeGame::NativeA32HidButton, 4>
              kDpadButtons{Oot3dNativeGame::NativeA32HidButton::DpadUp,
                           Oot3dNativeGame::NativeA32HidButton::DpadDown,
                           Oot3dNativeGame::NativeA32HidButton::DpadLeft,
                           Oot3dNativeGame::NativeA32HidButton::DpadRight};
          for (std::size_t direction = 0U;
               direction < kDpadButtons.size(); ++direction) {
            const auto mask = topScreenButton(kDpadButtons[direction]);
            dpadPhysical.Pressed[direction] =
                (topScreenPressed & mask) != 0U;
            dpadPhysical.Held[direction] =
                (topScreenButtons & mask) != 0U;
          }
          topScreenDpadActions = Oot3dNativeGame::ResolveTopScreenDpadActions(
              activeTopScreenConfig, dpadPhysical);
        }
        const bool zrHeld = ThreeDsRecomp::Input::IsButtonHeld(
            inputFrame, ThreeDsRecomp::Input::Button::Zr);
        const bool zlHeld = ThreeDsRecomp::Input::IsButtonHeld(
            inputFrame, ThreeDsRecomp::Input::Button::Zl);
        Oot3dNativeGame::TopScreenExtendedInputFrame sampledTopScreenInput = {
            .ZrHeld =
                zrHeld ||
                topScreenDpadActions.IsHeld(
                    Oot3dNativeGame::TopScreenDpadAction::ItemZr),
            .ZlHeld =
                zlHeld ||
                topScreenDpadActions.IsHeld(
                    Oot3dNativeGame::TopScreenDpadAction::ItemZl),
            .DpadLeftHeld =
                (inputFrame.Hid.Buttons &
                 Oot3dNativeGame::NativeA32HidButtonMask(
                     Oot3dNativeGame::NativeA32HidButton::DpadLeft)) != 0U,
            .DpadRightHeld =
                (inputFrame.Hid.Buttons &
                 Oot3dNativeGame::NativeA32HidButtonMask(
                     Oot3dNativeGame::NativeA32HidButton::DpadRight)) != 0U};
        sampledTopScreenInput.DpadUpHeld =
            (inputFrame.Hid.Buttons & topScreenButton(
                Oot3dNativeGame::NativeA32HidButton::DpadUp)) != 0U;
        if (ocarinaOwnsDpad) {
          inputFrame.Hid.Buttons &= ~(topScreenButton(Oot3dNativeGame::NativeA32HidButton::DpadLeft) |
                                     (ocarinaOwner.Page == 12 ? topScreenButton(
                                         Oot3dNativeGame::NativeA32HidButton::DpadUp) : 0U) |
                                     topScreenButton(Oot3dNativeGame::NativeA32HidButton::DpadRight));
        }
        sampledTopScreenInput.XHeld =
            (topScreenButtons &
             topScreenButton(Oot3dNativeGame::NativeA32HidButton::X)) != 0U;
        sampledTopScreenInput.YHeld =
            (topScreenButtons &
             topScreenButton(Oot3dNativeGame::NativeA32HidButton::Y)) != 0U;
        sampledTopScreenInput.GameplayDpadActionsOwned = true;
        sampledTopScreenInput.RestorationLayout =
            activeTopScreenConfig.HudLayout ==
            Oot3dNativeGame::TopScreenHudLayout::Restoration;
        nativeCandidateDispatch.TopScreenInputClock.ObserveGuest(sampledTopScreenInput);
        if (nativeCandidateDispatch.TopScreenUiProfile &&
            activeTopScreenConfig.FreeCameraEnabled &&
            nativeControlPollingState.GameplayMouseOwned) {
          nativeCandidateDispatch.TopScreenCameraInput.Observe(
              inputFrame.CStick);
        } else if (inputFrame.CStick.Kind ==
                   Oot3dNativeGame::NativeFreeCameraInputKind::Absolute) {
          nativeCandidateDispatch.TopScreenCameraInput.Observe(
              inputFrame.CStick);
        } else {
          nativeCandidateDispatch.TopScreenCameraInput.Reset();
        }
        if (nativeCandidateDispatch.TopScreenUiProfile) {
          const uint32_t buttons = inputFrame.Hid.Buttons;
          const uint32_t pressed =
              buttons & ~nativeCandidateDispatch.PreviousTopScreenButtons;
          const auto button = [](Oot3dNativeGame::NativeA32HidButton value) {
            return Oot3dNativeGame::NativeA32HidButtonMask(value);
          };
          const bool gameplayViewPressed = topScreenDpadActions.WasPressed(
              Oot3dNativeGame::TopScreenDpadAction::View);
          const bool gameplayOcarinaPressed =
              topScreenDpadActions.WasPressed(
                  Oot3dNativeGame::TopScreenDpadAction::Ocarina);
          const bool leftShoulderHeld =
              (buttons &
               button(Oot3dNativeGame::NativeA32HidButton::L)) != 0U;
          const bool rightShoulderHeld =
              (buttons &
               button(Oot3dNativeGame::NativeA32HidButton::R)) != 0U;
          const bool selectPressed =
              (pressed &
               button(Oot3dNativeGame::NativeA32HidButton::Select)) != 0U;
          if (topScreenDpadActions.WasPressed(
                  Oot3dNativeGame::TopScreenDpadAction::MinimapToggle) ||
              (selectPressed &&
               activeTopScreenConfig.SelectAction ==
                   Oot3dNativeGame::TopScreenSelectAction::MinimapToggle)) {
            setTopScreenSessionMinimapVisible(
                !activeTopScreenConfig.MinimapVisible);
          }
          if (selectPressed &&
              activeTopScreenConfig.SelectAction ==
                  Oot3dNativeGame::TopScreenSelectAction::MinimapToggle) {
            inputFrame.Hid.Buttons &=
                ~button(Oot3dNativeGame::NativeA32HidButton::Select);
          }

          if (gameplayViewPressed || gameplayOcarinaPressed) {
            ++nativeCandidateDispatch.TopScreenGameplayDpadAttempts;
            Oot3dNativeGame::TopScreenGameplayDpadResult dpadResult;
            std::string dpadError;
            if (!Oot3dNativeGame::PrepareTopScreenGameplayDpadGuest(
                    process.Memory(),
                    {.ViewPressed = gameplayViewPressed,
                     .OcarinaPressed = gameplayOcarinaPressed,
                     .LeftShoulderHeld = leftShoulderHeld,
                     .RightShoulderHeld = rightShoulderHeld},
                    &dpadResult, &dpadError)) {
              throw std::runtime_error(
                  "TopScreen gameplay D-pad prepare failed: " + dpadError);
            }
            nativeCandidateDispatch.TopScreenNaviViewActivations +=
                dpadResult.NaviViewActivated ? 1U : 0U;
            if (dpadResult.OcarinaQueryRequired) {
              ++nativeCandidateDispatch.TopScreenOcarinaQueries;
              uint32_t nativeEligibility = 0U;
              constexpr std::array<std::uint32_t, 0> kNoArguments{};
              if (!process.InvokeFunctionWithResult(
                      kTopScreenOcarinaEligibility, kNoArguments,
                      kTopScreenDirectCallReturn, &nativeEligibility,
                      &dpadError) ||
                  !Oot3dNativeGame::CommitTopScreenGameplayOcarinaGuest(
                      process.Memory(), nativeEligibility, &dpadResult,
                      &dpadError)) {
                throw std::runtime_error(
                    "TopScreen ocarina D-pad route failed: " + dpadError);
              }
              nativeCandidateDispatch.TopScreenOcarinaActivations +=
                  dpadResult.OcarinaActivated ? 1U : 0U;
            }
          }
          std::string gameplayActionError;
          if (const auto sound = uiLifecycleBridge.TakeOcarinaGuideSound(); sound != 0U) {
            if (!Oot3dNativeGame::PlayTopScreenUiSound(
                    process, sound, kTopScreenDirectCallReturn, &gameplayActionError)) {
              throw std::runtime_error("TopScreen ocarina guide sound failed: " + gameplayActionError);
            }
          }
          if (!Oot3dNativeGame::ConsumeTopScreenGameplayActions(
                  process, topScreenDpadActions,
                  nativeCandidateDispatch.TopScreenInput.ZrPressed ||
                      nativeCandidateDispatch.TopScreenInput.ZlPressed,
                  kTopScreenDirectCallReturn,
                  &nativeCandidateDispatch.TopScreenGameplayActionRuntime,
                  &nativeCandidateDispatch.TopScreenGameplayActions,
                  &gameplayActionError)) {
            throw std::runtime_error(
                "TopScreen gameplay action failed: " +
                gameplayActionError);
          }
          nativeCandidateDispatch.PreviousTopScreenButtons = buttons;
          nativeCandidateDispatch.TopScreenItems.DirectItemId =
              nativeCandidateDispatch.TopScreenGameplayActionRuntime.DirectItem.ActiveItemId;
          Oot3dNativeGame::TopScreenPauseDrawInputs pauseInputs;
          if (!Oot3dNativeGame::ReadTopScreenPauseDrawInputs(process.Memory(),
                                                             &pauseInputs)) {
            nativeCandidateDispatch.TopScreenPauseAotBarrier = true;
          } else {
            const auto closePlan =
                Oot3dNativeGame::ResolveTopScreenPauseStartClose(
                    pauseInputs,
                    (pressed &
                     button(Oot3dNativeGame::NativeA32HidButton::Start)) != 0U);
            if (closePlan.Page !=
                Oot3dNativeGame::TopScreenPauseClosePage::None) {
              std::string closeError;
              if (!ApplyTopScreenPauseClose(process, closePlan, &closeError)) {
                throw std::runtime_error(
                    "TopScreen START pause-close transaction failed: " +
                    closeError);
              }
              Oot3dNativeGame::MarkTopScreenStartCloseGestureConsumed(
                  nativeCandidateDispatch.TopScreenStartRouting);
              inputFrame.Hid.Buttons &=
                  ~button(Oot3dNativeGame::NativeA32HidButton::Start);
              ++nativeCandidateDispatch.TopScreenPauseStartCloseCalls;
              nativeCandidateDispatch.TopScreenPauseLastClosedPage =
                  closePlan.Page;
              if (!Oot3dNativeGame::ReadTopScreenPauseDrawInputs(
                      process.Memory(), &pauseInputs)) {
                throw std::runtime_error("TopScreen pause state could not be "
                                         "read after START close");
              }
            }
            if (activeTopScreenConfig.ExitItemsToSaveScreen) {
              const auto systemOpenPlan =
                  Oot3dNativeGame::ResolveTopScreenPauseBSystemOpen(
                      pauseInputs,
                      (pressed &
                       button(Oot3dNativeGame::NativeA32HidButton::B)) != 0U,
                      nativeCandidateDispatch.TopScreenPauseSystemOpen);
              if (systemOpenPlan.Page !=
                  Oot3dNativeGame::TopScreenPauseClosePage::None) {
                std::string systemOpenError;
                if (!ApplyTopScreenPauseSystemOpen(
                        process, systemOpenPlan, &systemOpenError)) {
                  throw std::runtime_error(
                      "TopScreen B system-menu transaction failed: " +
                      systemOpenError);
                }
                ++nativeCandidateDispatch.TopScreenPauseSystemOpenCalls;
                nativeCandidateDispatch.TopScreenPauseSystemSourcePage =
                    systemOpenPlan.Page;
                if (!Oot3dNativeGame::ReadTopScreenPauseDrawInputs(
                        process.Memory(), &pauseInputs)) {
                  throw std::runtime_error("TopScreen pause state could not be "
                                           "read after B system-menu open");
                }
              }
            }
            const bool sceneRuntimeRoute = pauseInputs.HasScene &&
                                           pauseInputs.SceneMode == 3U &&
                                           (pauseInputs.RuntimeMode == 1U ||
                                            pauseInputs.RuntimeMode == 2U);
            nativeCandidateDispatch.TopScreenPauseAotBarrier =
                sceneRuntimeRoute ||
                Oot3dNativeGame::ResolveTopScreenPauseRouteActive(
                    pauseInputs,
                    &nativeCandidateDispatch.TopScreenAotPauseRoute);
          }
        }
        if (scenarioBootstrap.has_value() && !scenarioBootstrap->Finished()) {
          std::string scenarioError;
          if (!scenarioBootstrap->Advance(
                  process, sceneViewProbe.Stats().LastPlayStateAddress,
                  frameCount, &scenarioError)) {
            std::cerr << "oot3d_native_game: structural scenario failed: "
                      << scenarioError << '\n';
            if (launch.ScenarioStrict) {
              window.Close();
              break;
            }
          }
          if (launch.ScenarioAutoExit && scenarioBootstrap->Complete()) {
            window.Close();
            break;
          }
        }
        refreshTickRemainder += kCtrArm11TicksPerSecond;
        nextVblankTick += refreshTickRemainder / kGuestDisplayRefreshRate;
        refreshTickRemainder %= kGuestDisplayRefreshRate;
        const auto clockStart = std::chrono::steady_clock::now();
        const auto clockResolution = AdvanceGuestClockTo(
            process, hostServices, processResult, nextVblankTick,
            launch.WholeAotBlockBudget);
        const double clockSeconds = SecondsSince(clockStart);
        phaseTiming.GuestClockSeconds += clockSeconds;
        phaseTiming.GuestSeconds += clockSeconds;
        if (clockResolution.OvershootTicks != 0U) {
          ++lateVblankDeadlineCount;
          maximumVblankDeadlineOvershootTicks =
              std::max(maximumVblankDeadlineOvershootTicks,
                       clockResolution.OvershootTicks);
        }
        const auto hidUpdate = hostServices.AdvanceHidToCurrentTick(
            process.Memory(), inputFrame.Hid);
        if (hidUpdate.Status ==
            Oot3dNativeGame::NativeA32CtrHidUpdateStatus::Failed) {
          throw std::runtime_error(
              "native HID sample could not be written to shared memory");
        }
        if (hidUpdate.EventsSignaled &&
            processResult.Kind !=
                Oot3dNativeGame::NativeA32ProcessRunKind::Terminated) {
          phaseStart = std::chrono::steady_clock::now();
          processResult =
              RunUntilGuestWait(process, launch.WholeAotBlockBudget);
          phaseTiming.GuestSeconds += SecondsSince(phaseStart);
          RequireRunnableGuest(processResult);
        }
        const uint32_t dspAudioFrames =
            hostServices.TakePendingDspAudioFrames();
        std::vector<int16_t> audioSamples;
        audioSamples.reserve(static_cast<size_t>(dspAudioFrames) * 320U);
        for (uint32_t audioFrame = 0; audioFrame < dspAudioFrames;
             ++audioFrame) {
          const size_t frameSampleOffset = audioSamples.size();
          const auto dspMixStart = std::chrono::steady_clock::now();
          CpuProbe::Scope cpuDsp(CpuProbe::Phase::Dsp);
          if (!dspHle.ProcessFrame(process.Memory(), audioSamples, &error)) {
            throw std::runtime_error("native DSP HLE mix failed: " + error);
          }
          phaseTiming.DspMixSeconds += SecondsSince(dspMixStart);
          cpuDsp.Stop();
          pcmContinuityDiagnostics.ObserveFrame(
              dspFramesMixed, std::span<const int16_t>(audioSamples)
                                  .subspan(frameSampleOffset));
          ++dspFramesMixed;
          dspFramesSuppressed += launch.DisableAudio ? 1U : 0U;
          if (!hostServices.SignalDspAudioFrame()) {
            break;
          }
          phaseStart = std::chrono::steady_clock::now();
          processResult =
              RunUntilGuestWait(process, launch.WholeAotBlockBudget);
          phaseTiming.GuestSeconds += SecondsSince(phaseStart);
          RequireRunnableGuest(processResult);
        }
        for (const int16_t sample : audioSamples) {
          const int32_t magnitude = std::abs(static_cast<int32_t>(sample));
          dspPeakMagnitude = std::max(dspPeakMagnitude, magnitude);
          dspNonzeroSamples += sample != 0 ? 1U : 0U;
          const uint16_t bits = static_cast<uint16_t>(sample);
          dspPcmFnv1a64 ^= static_cast<uint8_t>(bits);
          dspPcmFnv1a64 *= 1099511628211ULL;
          dspPcmFnv1a64 ^= static_cast<uint8_t>(bits >> 8U);
          dspPcmFnv1a64 *= 1099511628211ULL;
        }
        dspSamplesMixed += audioSamples.size() / 2U;
        if (!launch.AudioPcmDumpPath.empty()) {
          pcmDumpSamples.insert(pcmDumpSamples.end(), audioSamples.begin(),
                                audioSamples.end());
        }
        if (!launch.DisableAudio && audioPlayer != nullptr &&
            audioPlayer->IsInitialized() && !audioSamples.empty()) {
          const auto audioOutputStart = std::chrono::steady_clock::now();
          CpuProbe::Scope cpuAudio(CpuProbe::Phase::AudioOutput);
          const int32_t bufferedBefore = audioPlayer->Buffered();
          audioPlayer->Play(
              reinterpret_cast<const uint8_t *>(audioSamples.data()),
              audioSamples.size() * sizeof(int16_t));
          audioOutputDiagnostics.Observe(
              frameCount, bufferedBefore, audioPlayer->Buffered(),
              audioSamples.size() / 2U, audioPlayer->GetDesiredBuffered());
          phaseTiming.AudioOutputSeconds += SecondsSince(audioOutputStart);
        }
        if (!hostServices.SignalVBlank(process.Memory())) {
          throw std::runtime_error(
              "native VBlank could not enter the GSP relay queue");
        }
        ++vblankCount;
      }
      if (advanceGuest) {
        // PPF unblocks the current guest refresh, not the next presentation.
        // The dependency capture keeps its plans for the complete visual frame.
        (void)flushNativeDependencies();
        phaseStart = std::chrono::steady_clock::now();
        processResult =
            RunUntilGuestWait(process, launch.WholeAotBlockBudget);
        phaseTiming.GuestSeconds += SecondsSince(phaseStart);
        RequireRunnableGuest(processResult);

        phaseStart = std::chrono::steady_clock::now();
        const auto picaSubmitStart = phaseStart;
        CpuProbe::Scope cpuQueue(CpuProbe::Phase::PicaQueue);
        size_t drainPassesThisRefresh = 0;
        bool refreshHadNativeDraws = false;
        while (drainPassesThisRefresh < kMaximumPicaDrainPassesPerRefresh) {
          publishNativeSceneView();
          submissionQueue.TakePendingDraws(drainedDraws);
          auto &draws = drainedDraws;
          auto completions = submissionQueue.TakePendingCompletions();
          auto displayTransfers = submissionQueue.TakePendingDisplayTransfers();
          auto memoryFills = submissionQueue.TakePendingMemoryFills();
          if (draws.empty() && completions.empty() &&
              displayTransfers.empty() && memoryFills.empty()) {
            break;
          }
          ++drainPassesThisRefresh;
          ++picaDrainPassCount;
          refreshHadNativeDraws = refreshHadNativeDraws || !draws.empty();
          const bool suppressTopScreenFrontendBackdrop =
              Oot3dNativeGame::ShouldSuppressTopScreenFrontendBackdrop(
                  launch.UiProfile,
                  uiLifecycleBridge.NativeFrontendPresentationActive());
          const auto commonBackgroundGuestAddress =
              suppressTopScreenFrontendBackdrop
                  ? uiLifecycleBridge.NativePauseSharedTextureGuestAddress(
                        oot3d::ui::UiPauseSharedTextureSlot::CommonBackground00)
                  : std::nullopt;
          const auto commonBackgroundPhysicalAddress =
              commonBackgroundGuestAddress.has_value()
                  ? picaMemoryView.TranslateGuest(*commonBackgroundGuestAddress,
                                                  1U)
                  : std::nullopt;
          std::vector<Oot3dNativeGame::TopScreenFrontendCanvas>
              frontendBackdropCanvases;
          const auto rememberFrontendBackdropCanvas =
              [&](const Oot3dNativeGame::Oot3dPicaDecodedDrawState& state) {
                if (state.Framebuffer.ColorPhysicalAddress != 0U &&
                    std::none_of(frontendBackdropCanvases.begin(),
                                 frontendBackdropCanvases.end(),
                                 [&](const auto& canvas) {
                                   return canvas.Matches(state);
                                 })) {
                  frontendBackdropCanvases.push_back(
                      {state.Framebuffer.ColorPhysicalAddress, state.Viewport});
                }
              };
          if (commonBackgroundPhysicalAddress.has_value()) {
            for (const auto &draw : draws) {
              const bool usesCommonBackground = std::any_of(
                  draw.State.Textures.begin(), draw.State.Textures.end(),
                  [&](const auto &texture) {
                    return texture.Enabled &&
                           texture.PhysicalAddress ==
                               *commonBackgroundPhysicalAddress;
                  });
              if (usesCommonBackground) {
                rememberFrontendBackdropCanvas(draw.State);
              }
            }
          }
          bool signaledPicaCompletion = false;
          size_t completionIndex = 0;
          size_t memoryFillIndex = 0;
          size_t displayTransferIndex = 0;
          const auto submitMemoryFill =
              [&](const Oot3dNativeGame::Oot3dPicaMemoryFillSubmission &fill) {
                picaSemanticTrace.RecordMemoryFill(
                    presentationFrameCount, frameCount, fill);
                picaPresentationScheduler.Capture(fill);
                if (fill.CompletionId != 0U) {
                  if (!fill.Interrupt.has_value() ||
                      !hostServices.SignalPicaInterrupt(process.Memory(),
                                                        *fill.Interrupt)) {
                    throw std::runtime_error(
                        "native PICA memory fill completion "
                        "could not enter the GSP relay queue");
                  }
                  if (*fill.Interrupt ==
                      Oot3dNativeGame::Oot3dPicaInterruptId::P3d) {
                    ++completedP3dCount;
                  } else if (*fill.Interrupt ==
                             Oot3dNativeGame::Oot3dPicaInterruptId::Ppf) {
                    ++completedPpfCount;
                  }
                  ++recordedCompletionCount;
                  signaledPicaCompletion = true;
                }
              };
          const auto submitDisplayTransfer =
              [&](const Oot3dNativeGame::Oot3dPicaDisplayTransferSubmission
                      &transfer) {
                const auto topFramebuffer = hostServices.TopFramebuffer();
                const bool selectedForTop = topFramebuffer.has_value() &&
                                            (transfer.Transfer.OutputAddress ==
                                                 topFramebuffer->AddressLeft ||
                                             transfer.Transfer.OutputAddress ==
                                                 topFramebuffer->AddressRight);
                picaSemanticTrace.RecordDisplayTransfer(
                    presentationFrameCount, frameCount, transfer,
                    selectedForTop, suppressTopScreenFrontendBackdrop);
                if (launch.ExtendedDiagnostics) {
                  if (displayTransferEvents.size() == 64U) {
                    displayTransferEvents.erase(displayTransferEvents.begin());
                  }
                  displayTransferEvents.push_back({
                      {"input_address", transfer.Transfer.InputAddress},
                      {"output_address", transfer.Transfer.OutputAddress},
                      {"input_physical_address", transfer.InputPhysicalAddress},
                      {"output_physical_address",
                       transfer.OutputPhysicalAddress},
                      {"input_size", transfer.Transfer.InputSize},
                      {"output_size", transfer.Transfer.OutputSize},
                      {"flags", transfer.Transfer.Flags},
                      {"texture_copy_bytes", transfer.Transfer.TextureCopyBytes},
                      {"after_draw_submission_id",
                       transfer.AfterDrawSubmissionId},
                      {"signals_guest_interrupt", transfer.SignalInterrupt},
                      {"selected_for_top_at_submission", selectedForTop},
                      {"topscreen_frontend_backdrop_suppression_active",
                       suppressTopScreenFrontendBackdrop},
                  });
                  if (transfer.Transfer.TextureCopyBytes != 0U) {
                    // Same span the backend writes: byte count plus the per-row
                    // gap, so every byte the copy materializes is inside the
                    // watch. For a raw copy OutputSize carries the native
                    // width/height pair in 16-byte units (see the bridge).
                    const uint32_t copySize =
                        transfer.Transfer.TextureCopyBytes & ~15U;
                    const uint32_t copyGap =
                        (transfer.Transfer.OutputSize >> 16U) * 16U;
                    const uint32_t copyWidth =
                        copyGap == 0U
                            ? copySize
                            : (transfer.Transfer.OutputSize & 0xFFFFU) * 16U;
                    if (copySize != 0U && copyWidth != 0U) {
                      const uint64_t copyEnd =
                          uint64_t{transfer.OutputPhysicalAddress} + copySize +
                          uint64_t{(copySize - 1U) / copyWidth} * copyGap;
                      const auto copySpan = static_cast<size_t>(
                          std::min<uint64_t>(copyEnd, uint64_t{1} << 32) -
                          transfer.OutputPhysicalAddress);
                      // The memory object is indexed by guest addresses: the
                      // transfer names a physical destination, so it has to be
                      // translated before anything can be trapped. A span that
                      // does not translate is skipped rather than watched at the
                      // wrong address.
                      const auto watchGuest = picaMemoryView.Translate(
                          transfer.OutputPhysicalAddress, copySpan);
                      if (watchGuest.has_value()) {
                        const std::pair<uint32_t, uint32_t> watched{
                            *watchGuest,
                            *watchGuest + static_cast<uint32_t>(copySpan)};
                        if (std::find(rawCopyWatchRanges.begin(),
                                      rawCopyWatchRanges.end(),
                                      watched) == rawCopyWatchRanges.end()) {
                          rawCopyWatchRanges.push_back(watched);
                          process.Memory().SetRangeWatch(rawCopyWatchRanges);
                        } else if (!process.Memory().RangeWatchActive()) {
                          process.Memory().SetRangeWatch(rawCopyWatchRanges);
                        }
                      }
                    }
                  }
                }
                picaPresentationScheduler.Capture(transfer);
                if (transfer.SignalInterrupt && transfer.Transfer.TextureCopyBytes == 0U) {
                  if (!hostServices.SignalPicaInterrupt(
                          process.Memory(),
                          Oot3dNativeGame::Oot3dPicaInterruptId::Ppf)) {
                    throw std::runtime_error(
                        "native PICA display transfer completion could not "
                        "enter the GSP relay queue");
                  }
                  ++completedPpfCount;
                  ++recordedCompletionCount;
                  signaledPicaCompletion = true;
                }
                displayTransfersByOutput[transfer.Transfer.OutputAddress] =
                    transfer;
                ++displayTransferCount;
              };
          while (displayTransferIndex < displayTransfers.size() &&
                 displayTransfers[displayTransferIndex].AfterDrawSubmissionId <=
                     lastSubmittedDrawId) {
            submitDisplayTransfer(displayTransfers[displayTransferIndex++]);
          }
          for (auto &draw : draws) {
            while (memoryFillIndex < memoryFills.size() &&
                   memoryFills[memoryFillIndex].BeforeDrawSubmissionId <=
                       draw.Id) {
              submitMemoryFill(memoryFills[memoryFillIndex++]);
            }
            const bool usesCommonBackground =
                commonBackgroundPhysicalAddress.has_value() &&
                std::any_of(draw.State.Textures.begin(),
                            draw.State.Textures.end(),
                            [&](const auto &texture) {
                              return texture.Enabled &&
                                     texture.PhysicalAddress ==
                                         *commonBackgroundPhysicalAddress;
                            });
            const bool targetsFrontendBackdrop =
                std::any_of(frontendBackdropCanvases.begin(),
                            frontendBackdropCanvases.end(),
                            [&](const auto& canvas) {
                              return canvas.Matches(draw.State);
                            });
            const bool isOpaqueTargetInitialization =
                IsNativeFrontendOpaqueTargetInitializationDraw(draw);
            const bool suppressNativeFrontendBackdropDraw =
                suppressTopScreenFrontendBackdrop &&
                targetsFrontendBackdrop &&
                (usesCommonBackground || isOpaqueTargetInitialization);
            Oot3dNativeGame::Oot3dPicaVulkanDrawPlan plan;
            auto drawPhaseStart = std::chrono::steady_clock::now();
            if (!Oot3dNativeGame::
                    BuildOot3dPicaVulkanDrawPlanAndConsumeResources(
                        draw, plan, &error, &shaderSourceCache)) {
              throw std::runtime_error("native PICA Vulkan plan failed: " +
                                       error);
            }
            phaseTiming.PicaPlanSeconds += SecondsSince(drawPhaseStart);
            picaSemanticTrace.RecordDraw(
                presentationFrameCount, frameCount, draw, plan,
                suppressNativeFrontendBackdropDraw, usesCommonBackground,
                isOpaqueTargetInitialization);
            if (suppressNativeFrontendBackdropDraw) {
              nativeMenuBackdropDrawOmissionCount +=
                  usesCommonBackground ? 1U : 0U;
              nativeMenuClearDrawOmissionCount +=
                  isOpaqueTargetInitialization ? 1U : 0U;
            }
            if (launch.ExtendedDiagnostics) {
              drawPhaseStart = std::chrono::steady_clock::now();
              if (drawEvents.size() == 128U) {
                drawEvents.erase(drawEvents.begin());
              }
              nlohmann::json textures = nlohmann::json::array();
              for (size_t slot = 0; slot < draw.State.Textures.size();
                   ++slot) {
                const auto &texture = draw.State.Textures[slot];
                if (!texture.Enabled) {
                  continue;
                }
                const auto planTexture = std::find_if(
                    plan.Textures.begin(), plan.Textures.end(),
                    [slot](const auto &candidate) {
                      return candidate.Slot == slot;
                    });
                const std::span<const uint8_t> nativeBytes =
                    planTexture != plan.Textures.end()
                        ? std::span<const uint8_t>(planTexture->NativeBytes)
                        : std::span<const uint8_t>();
                textures.push_back({
                    {"slot", slot},
                    {"width", texture.Width},
                    {"height", texture.Height},
                    {"physical_address", texture.PhysicalAddress},
                    {"format", texture.Format},
                    {"type", texture.Type},
                    {"native_byte_count", nativeBytes.size()},
                    {"native_byte_hash", HashDiagnosticBytes(nativeBytes)},
                    {"native_nonzero_bytes",
                     std::count_if(nativeBytes.begin(), nativeBytes.end(),
                                   [](const uint8_t byte) {
                                     return byte != 0U;
                                   })},
                });
              }
              nlohmann::json vertexBindings = nlohmann::json::array();
              for (const auto &binding : plan.VertexBindings) {
                constexpr size_t kDiagnosticByteLimit = 256U;
                const auto bindingBytes = binding.ResolvedBytes();
                const size_t byteCount =
                    std::min(bindingBytes.size(), kDiagnosticByteLimit);
                vertexBindings.push_back({
                    {"binding", binding.Binding},
                    {"byte_stride", binding.ByteStride},
                    {"per_instance",
                     binding.InputRate ==
                         Oot3dNativeGame::Oot3dPicaVertexInputRate::
                             PerInstance},
                    {"byte_count", bindingBytes.size()},
                    {"prefix_bytes",
                     std::vector<uint8_t>(bindingBytes.begin(),
                                          bindingBytes.begin() + byteCount)},
                });
              }
              nlohmann::json defaultAttributes = nlohmann::json::array();
              for (size_t attributeIndex = 0;
                   attributeIndex < draw.State.VertexInput.AttributeCount;
                   ++attributeIndex) {
                if (!draw.State.VertexInput.Attributes[attributeIndex]
                         .Default) {
                  continue;
                }
                defaultAttributes.push_back({
                    {"attribute", attributeIndex},
                    {"input_register",
                     draw.State.ShaderInterface
                         .InputRegisterByAttribute[attributeIndex]},
                    {"value", draw.Packet.DefaultAttributes[attributeIndex]},
                });
              }
              constexpr std::array<uint16_t, 6> kTevStageRegisters{
                  0x0C0U, 0x0C8U, 0x0D0U, 0x0D8U, 0x0F0U, 0x0F8U};
              nlohmann::json tevStages = nlohmann::json::array();
              for (const uint16_t base : kTevStageRegisters) {
                tevStages.push_back({
                    {"base_register", base},
                    {"sources", draw.Packet.Registers[base]},
                    {"modifiers", draw.Packet.Registers[base + 1U]},
                    {"operations", draw.Packet.Registers[base + 2U]},
                    {"constant", draw.Packet.Registers[base + 3U]},
                    {"scales", draw.Packet.Registers[base + 4U]},
                });
              }
              drawEvents.push_back({
                  {"frame", frameCount},
                  {"submission_id", draw.Id},
                  {"command_list_address", draw.Packet.CommandListAddress},
                  {"command_list_offset_words",
                   draw.Packet.CommandListOffsetWords},
                  {"topscreen_frontend_backdrop_suppressed",
                   suppressNativeFrontendBackdropDraw},
                  {"native_frontend_background_texture", usesCommonBackground},
                  {"native_frontend_target_initialization",
                   isOpaqueTargetInitialization},
                  {"vertex_count", draw.State.VertexInput.VertexCount},
                  {"vertex_offset", draw.State.VertexInput.VertexOffset},
                  {"minimum_vertex_index", draw.MinimumVertexIndex},
                  {"maximum_vertex_index", draw.MaximumVertexIndex},
                  {"indexed", draw.State.VertexInput.Indexed},
                  {"indices_are_16_bit",
                   draw.State.VertexInput.IndicesAre16Bit},
                  {"attribute_count", draw.State.VertexInput.AttributeCount},
                  {"vulkan_base_vertex", plan.BaseVertex},
                  {"vulkan_vertex_bindings", plan.VertexBindings.size()},
                  {"vertex_shader_key", plan.VertexShader.StateKey},
                  {"fragment_shader_key", plan.FragmentShader.StateKey},
                  {"vertex_shader_source",
                   std::string(plan.ResolvedVertexShaderSource())},
                  {"fragment_shader_source",
                   std::string(plan.ResolvedFragmentShaderSource())},
                  {"vertex_uniform_floats", plan.VertexShader.Uniforms.Floats},
                  {"shader_output_mask", draw.State.ShaderInterface.OutputMask},
                  {"shader_output_total",
                   draw.Packet.Registers[0x04FU] & 7U},
                  {"geometry_shader_enabled",
                   draw.State.ShaderInterface.GeometryShaderEnabled},
                  {"geometry_shader_mode",
                   draw.Packet.Registers[0x229U] & 3U},
                  {"shader_output_maps",
                   {draw.Packet.Registers[0x050U],
                    draw.Packet.Registers[0x051U],
                    draw.Packet.Registers[0x052U],
                    draw.Packet.Registers[0x053U],
                    draw.Packet.Registers[0x054U],
                    draw.Packet.Registers[0x055U],
                    draw.Packet.Registers[0x056U]}},
                  {"framebuffer",
                   {{"color_address",
                     draw.State.Framebuffer.ColorPhysicalAddress},
                    {"depth_address",
                     draw.State.Framebuffer.DepthPhysicalAddress},
                    {"width", draw.State.Framebuffer.Width},
                    {"height", draw.State.Framebuffer.Height},
                    {"color_format", draw.State.Framebuffer.ColorFormat},
                    {"depth_format", draw.State.Framebuffer.DepthFormat},
                    {"flipped", draw.State.Framebuffer.Flipped}}},
                  {"viewport",
                   {{"half_width", draw.State.Viewport.HalfWidth},
                    {"half_height", draw.State.Viewport.HalfHeight},
                    {"depth_range", draw.State.Viewport.DepthRange},
                    {"near_plane", draw.State.Viewport.NearPlane},
                    {"z_buffering", draw.State.Viewport.ZBuffering},
                    {"corner_x", draw.State.Viewport.CornerX},
                    {"corner_y", draw.State.Viewport.CornerY}}},
                  {"scissor",
                   {{"mode", static_cast<uint32_t>(draw.State.Scissor.Mode)},
                    {"x1", draw.State.Scissor.X1},
                    {"y1", draw.State.Scissor.Y1},
                    {"x2", draw.State.Scissor.X2},
                    {"y2", draw.State.Scissor.Y2}}},
                  {"topology", static_cast<uint32_t>(draw.State.Topology)},
                  {"cull_mode", static_cast<uint32_t>(draw.State.CullMode)},
                  {"output_merger",
                   {{"fragment_operation_mode",
                     plan.State.OutputMerger.FragmentOperationMode},
                    {"color_write_mask",
                     plan.State.OutputMerger.ColorWriteMask},
                    {"logic_operation",
                     static_cast<uint32_t>(
                         plan.State.OutputMerger.LogicOperation)},
                    {"blend_enabled", plan.State.OutputMerger.Blend.Enabled},
                    {"blend_color_equation",
                     static_cast<uint32_t>(
                         plan.State.OutputMerger.Blend.ColorEquation)},
                    {"blend_source_color",
                     static_cast<uint32_t>(
                         plan.State.OutputMerger.Blend.SourceColor)},
                    {"blend_destination_color",
                     static_cast<uint32_t>(
                         plan.State.OutputMerger.Blend.DestinationColor)},
                    {"depth_test_enabled",
                     plan.State.OutputMerger.Depth.TestEnabled},
                    {"depth_write_enabled",
                     plan.State.OutputMerger.Depth.WriteEnabled},
                    {"depth_compare",
                     static_cast<uint32_t>(
                         plan.State.OutputMerger.Depth.Compare)}}},
                  {"textures", std::move(textures)},
                  {"vertex_bindings", std::move(vertexBindings)},
                  {"default_attributes", std::move(defaultAttributes)},
                  {"tev_stages", std::move(tevStages)},
                  {"combiner_buffer_color", draw.Packet.Registers[0x0FDU]},
              });
              phaseTiming.PicaDiagnosticsSeconds +=
                  SecondsSince(drawPhaseStart);
            }
            if (!suppressNativeFrontendBackdropDraw) {
              picaPresentationScheduler.Capture(std::move(plan));
            }
            while (completionIndex < completions.size() &&
                   completions[completionIndex].AfterDrawSubmissionId ==
                       draw.Id) {
              const auto &completion = completions[completionIndex++];
              if (!hostServices.SignalPicaInterrupt(process.Memory(),
                                                    completion.Interrupt)) {
                throw std::runtime_error(
                    "native PICA draw completion could not "
                    "enter the GSP relay queue");
              }
              if (completion.Interrupt ==
                  Oot3dNativeGame::Oot3dPicaInterruptId::P3d) {
                ++completedP3dCount;
              } else if (completion.Interrupt ==
                         Oot3dNativeGame::Oot3dPicaInterruptId::Ppf) {
                ++completedPpfCount;
              }
              ++recordedCompletionCount;
              signaledPicaCompletion = true;
            }
            lastSubmittedDrawId = draw.Id;
            while (
                displayTransferIndex < displayTransfers.size() &&
                displayTransfers[displayTransferIndex].AfterDrawSubmissionId ==
                    draw.Id) {
              submitDisplayTransfer(displayTransfers[displayTransferIndex++]);
            }
            ++drawCount;
          }
          if (completionIndex != completions.size()) {
            throw std::runtime_error(
                "native PICA completion did not match a submitted draw");
          }
          while (memoryFillIndex < memoryFills.size()) {
            submitMemoryFill(memoryFills[memoryFillIndex++]);
          }
          if (displayTransferIndex != displayTransfers.size()) {
            throw std::runtime_error("native PICA display transfer did not "
                                     "match a submitted draw");
          }
          signaledPicaCompletion =
              flushNativeDependencies() || signaledPicaCompletion;
          if (!signaledPicaCompletion) {
            break;
          }
          phaseStart = std::chrono::steady_clock::now();
          processResult =
              RunUntilGuestWait(process, launch.WholeAotBlockBudget);
          const double resumedGuestSeconds = SecondsSince(phaseStart);
          phaseTiming.GuestSeconds += resumedGuestSeconds;
          phaseTiming.GuestInsidePicaSubmitSeconds += resumedGuestSeconds;
          RequireRunnableGuest(processResult);
        }
        if (drainPassesThisRefresh == kMaximumPicaDrainPassesPerRefresh &&
            (!submissionQueue.PendingDraws().empty() ||
             !submissionQueue.PendingCompletions().empty() ||
             !submissionQueue.PendingDisplayTransfers().empty() ||
             !submissionQueue.PendingMemoryFills().empty())) {
          throw std::runtime_error("native PICA completion chain exceeded "
                                   "the per-refresh drain limit");
        }
        maximumPicaDrainPassesPerRefresh = std::max<uint64_t>(
            maximumPicaDrainPassesPerRefresh, drainPassesThisRefresh);
        refreshesWithNativeDraws += refreshHadNativeDraws ? 1U : 0U;
        phaseTiming.PicaSubmitSeconds += SecondsSince(picaSubmitStart);
      }
      if (presentHostFrame) {
        CpuProbe::Scope cpuVisual(CpuProbe::Phase::VisualPresentation);
        const auto visualPresentationStart = std::chrono::steady_clock::now();
        constexpr auto presentationRenderTargetNamespace =
            kVisualInterpolationRenderTargetNamespace;
        const bool lcdForceBlack = hostServices.LcdForceBlack();
        const auto topFramebuffer = hostServices.TopFramebuffer();
        const Oot3dNativeGame::Oot3dPicaDisplayTransferSubmission
            *traceSelectedTopTransfer = nullptr;
        if (topFramebuffer.has_value()) {
          auto traceSelected =
              displayTransfersByOutput.find(topFramebuffer->AddressLeft);
          if (traceSelected == displayTransfersByOutput.end() &&
              topFramebuffer->AddressRight != topFramebuffer->AddressLeft) {
            traceSelected =
                displayTransfersByOutput.find(topFramebuffer->AddressRight);
          }
          if (traceSelected != displayTransfersByOutput.end()) {
            traceSelectedTopTransfer = &traceSelected->second;
          }
        }
        picaSemanticTrace.RecordPresentationSelection(
            presentationFrameCount, frameCount, lcdForceBlack,
            topFramebuffer.has_value()
                ? std::optional<uint32_t>(topFramebuffer->AddressLeft)
                : std::nullopt,
            topFramebuffer.has_value()
                ? std::optional<uint32_t>(topFramebuffer->AddressRight)
                : std::nullopt,
            traceSelectedTopTransfer);
        if (!lcdForceBlack) {
          if (topFramebuffer.has_value()) {
            auto selected =
                displayTransfersByOutput.find(topFramebuffer->AddressLeft);
            if (selected == displayTransfersByOutput.end() &&
                topFramebuffer->AddressRight != topFramebuffer->AddressLeft) {
              selected =
                  displayTransfersByOutput.find(topFramebuffer->AddressRight);
            }
            if (selected != displayTransfersByOutput.end()) {
              // Guest side: transfer selection, frame completion and the
              // guest-memory reads the renderer needs. The rest runs as one
              // render command, in renderer order.
              const auto topSelection = selected->second;
              std::shared_ptr<Oot3dNativeGame::Oot3dPicaVisualFrame>
                  completedFrame;
              // Guest frames completed before the selected one while the host
              // fell behind: canonical work and interpolation history only.
              std::shared_ptr<std::vector<Oot3dNativeGame::Oot3dPicaVisualFrame>>
                  supersededFrames;
              std::vector<Fast::Oot3d::GrassInteractor> frameActors;
              bool suppressTopScreenBackdrop = false;
              if (topSelection.CompletionId !=
                  lastSelectedTopTransferCompletionId) {
                lastSelectedTopTransferCompletionId =
                    topSelection.CompletionId;
                ++selectedTopTransferCount;
                auto finishedFrames = picaPresentationScheduler.FinishFrames(
                    topSelection, topDisplayOutputs);
                if (std::find(topDisplayOutputs.begin(), topDisplayOutputs.end(),
                              topSelection.OutputPhysicalAddress) == topDisplayOutputs.end()) {
                  if (topDisplayOutputs.size() == 4U)
                    topDisplayOutputs.erase(topDisplayOutputs.begin());
                  topDisplayOutputs.push_back(topSelection.OutputPhysicalAddress);
                }
                std::optional<Oot3dNativeGame::Oot3dPicaVisualFrame> finished;
                if (!finishedFrames.empty()) {
                  finished = std::move(finishedFrames.back());
                  finishedFrames.pop_back();
                  if (!finishedFrames.empty())
                    supersededFrames = std::make_shared<
                        std::vector<Oot3dNativeGame::Oot3dPicaVisualFrame>>(
                        std::move(finishedFrames));
                }
                if (finished.has_value()) {
                  frameActors = Oot3dNativeGame::CollectNativeActorInteractions(
                      process.Memory(),
                      sceneViewProbe.Stats().LastPlayStateAddress,
                      finished->Sequence);
                  ++visualSnapshotCount;
                  suppressTopScreenBackdrop =
                      Oot3dNativeGame::ShouldSuppressTopScreenFrontendBackdrop(
                          launch.UiProfile,
                          uiLifecycleBridge.NativeFrontendPresentationActive());
                  completedFrame =
                      std::make_shared<Oot3dNativeGame::Oot3dPicaVisualFrame>(
                          std::move(*finished));
                }
              }
              renderStream.Submit([&, topSelection, completedFrame, supersededFrames,
                                   frameActors, suppressTopScreenBackdrop,
                                   presentationElapsedSeconds,
                                   visualInterpolationAlpha, frameCount,
                                   guestReferences =
                                       collectGuestCopyReferences()] {
              if (rendererFrameSkipped) return;
              guestCopyReferences = guestReferences;
              bool presentedVisualSample = false;
              bool selectedNewVisualFrame = false;
              {
                if (completedFrame != nullptr && supersededFrames != nullptr) {
                  for (auto &superseded : *supersededFrames) {
                    auto &sample = visualFrameSampleScratch;
                    (void)Oot3dNativeGame::ViewOot3dPicaVisualFrame(
                        superseded, sample, true);
                    const auto nativeTemporalSample =
                        Fast::Oot3d::BuildNativeFrameTemporalSample(
                            frameCompositionPolicy,
                            Fast::Oot3d::NativeFrameTemporalSampleKind::
                                Authoritative,
                            superseded.Sequence, superseded.Sequence,
                            visualContinuityEpoch, 1.0F,
                            static_cast<float>(presentationElapsedSeconds));
                    executeVisualSample(
                        sample,
                        Oot3dNativeGame::Oot3dPicaPresentationExecutionKind::
                            NativeFrame,
                        false, nativeTemporalSample,
                        kNativeDependencyRenderTargetNamespace);
                    if (visualInterpolationEnabled) {
                      preparedVisualTransition.Reset();
                      if (latestVisualFrame.has_value())
                        previousVisualFrame = std::move(latestVisualFrame);
                      latestVisualFrame = std::move(superseded);
                    }
                  }
                }
                if (completedFrame != nullptr) {
                  std::optional<Oot3dNativeGame::Oot3dPicaVisualFrame>
                      currentVisualFrame(std::move(*completedFrame));
                  Fast::Oot3d::GrassInteractionBridge::Instance().PublishFrame(
                      currentVisualFrame->Sequence, frameActors);
                  selectedNewVisualFrame = true;
                  auto &sample = visualFrameSampleScratch;
                  // Keep the canonical target complete independently of the
                  // delayed visual target. Raw-copy prefixes have already
                  // executed; only the native suffix remains here.
                  (void)Oot3dNativeGame::ViewOot3dPicaVisualFrame(
                      *currentVisualFrame, sample, true);
                  const auto nativeTemporalSample =
                      Fast::Oot3d::BuildNativeFrameTemporalSample(
                          frameCompositionPolicy,
                          Fast::Oot3d::NativeFrameTemporalSampleKind::
                              Authoritative,
                          currentVisualFrame->Sequence,
                          currentVisualFrame->Sequence, visualContinuityEpoch,
                          1.0F, static_cast<float>(presentationElapsedSeconds));
                  executeVisualSample(
                      sample,
                      Oot3dNativeGame::Oot3dPicaPresentationExecutionKind::
                          NativeFrame,
                      false, nativeTemporalSample,
                      kNativeDependencyRenderTargetNamespace);
                  Oot3dNativeGame::ComposeTopScreenFrontendFrame(
                      *currentVisualFrame, suppressTopScreenBackdrop);
                  if (!visualInterpolationEnabled) {
                    if (Oot3dNativeGame::ViewOot3dPicaVisualFrame(
                            *currentVisualFrame, sample)) {
                      const auto temporalSample =
                          Fast::Oot3d::BuildNativeFrameTemporalSample(
                              frameCompositionPolicy,
                              Fast::Oot3d::NativeFrameTemporalSampleKind::
                                  Authoritative,
                              currentVisualFrame->Sequence,
                              currentVisualFrame->Sequence,
                              visualContinuityEpoch, 1.0F,
                              static_cast<float>(
                                  presentationElapsedSeconds));
                      executeVisualSample(
                          sample,
                          Oot3dNativeGame::
                              Oot3dPicaPresentationExecutionKind::CurrentFrame,
                          true, temporalSample);
                      ++visualDirectFramesPresented;
                      ++visualSamplesPresented;
                      visualSampleDrawCount += sample.Draws.size();
                      presentedVisualSample = true;
                    }
                    // Direct gameplay modes retain no cross-frame matching
                    // history. The scheduler keeps only the executed scanout
                    // snapshot needed if presentation temporarily outruns
                    // simulation.
                    preparedVisualTransition.Reset();
                    previousVisualFrame.reset();
                    latestVisualFrame.reset();
                    latestVisualTransitionContinuous = false;
                  } else {
                    bool resynchronizeVisualNamespace =
                        !latestVisualFrame.has_value();
                    if (latestVisualFrame.has_value()) {
                      const bool transitionPrepared =
                          preparedVisualTransition.Prepare(
                              *latestVisualFrame, *currentVisualFrame,
                              &visualInterpolationTiming);
                      const auto &transition =
                          preparedVisualTransition.Stats();
                      const float sampleAlpha =
                          visualSampleCadence.Resolve(
                              true, visualInterpolationAlpha);
                      const bool visualSampleReady =
                          transitionPrepared &&
                          preparedVisualTransition.Sample(
                              *latestVisualFrame, *currentVisualFrame,
                              sampleAlpha, sample,
                              &visualInterpolationTiming);
                      ++visualTransitionCount;
                      visualMatchedDrawCount += transition.MatchedDraws;
                      visualStrictUniqueMatchedDrawCount +=
                          transition.StrictUniqueMatchedDraws;
                      visualStrictOrdinalMatchedDrawCount +=
                          transition.StrictOrdinalMatchedDraws;
                      visualStructuralOrdinalMatchedDrawCount +=
                          transition.StructuralOrdinalMatchedDraws;
                      visualPipelineOrdinalMatchedDrawCount +=
                          transition.PipelineOrdinalMatchedDraws;
                      visualChangedContinuousDrawCount +=
                          transition.ChangedContinuousDraws;
                      visualChangedPerInstanceVertexDrawCount +=
                          transition.ChangedPerInstanceVertexDraws;
                      visualChangedPerVertexDrawCount +=
                          transition.ChangedPerVertexDraws;
                      visualAmbiguousPreviousDrawCount +=
                          transition.AmbiguousPreviousDraws;
                      visualAmbiguousCurrentDrawCount +=
                          transition.AmbiguousCurrentDraws;
                      visualUnmatchedPreviousDrawCount +=
                          transition.UnmatchedPreviousDraws;
                      visualUnmatchedCurrentDrawCount +=
                          transition.UnmatchedCurrentDraws;
                      const double continuityThreshold =
                          visualContinuityTracker.CurrentThreshold();
                      const bool continuousTransition =
                          transitionPrepared &&
                          visualContinuityTracker.Accept(transition);
                      latestVisualTransitionContinuous = continuousTransition;
                      visualDiscontinuityCount +=
                          continuousTransition ? 0U : 1U;
                      if (launch.ExtendedDiagnostics) {
                        if (visualTransitionEvents.size() == 1024U) {
                          visualTransitionEvents.erase(
                              visualTransitionEvents.begin());
                        }
                        visualTransitionEvents.push_back({
                            {"host_frame", frameCount},
                            {"previous_sequence",
                             latestVisualFrame->Sequence},
                            {"current_sequence",
                             currentVisualFrame->Sequence},
                            {"matched_draws", transition.MatchedDraws},
                            {"strict_unique_matched_draws",
                             transition.StrictUniqueMatchedDraws},
                            {"strict_ordinal_matched_draws",
                             transition.StrictOrdinalMatchedDraws},
                            {"structural_ordinal_matched_draws",
                             transition.StructuralOrdinalMatchedDraws},
                            {"pipeline_ordinal_matched_draws",
                             transition.PipelineOrdinalMatchedDraws},
                            {"changed_per_instance_vertex_draws",
                             transition.ChangedPerInstanceVertexDraws},
                            {"changed_per_vertex_draws",
                             transition.ChangedPerVertexDraws},
                            {"unmatched_previous_draws",
                             transition.UnmatchedPreviousDraws},
                            {"unmatched_current_draws",
                             transition.UnmatchedCurrentDraws},
                            {"top_target_matched_draws",
                             transition.TopTargetMatchedDraws},
                            {"matched_draw_coverage",
                             transition.MatchedDrawCoverage},
                            {"median_normalized_continuous_delta",
                             transition.MedianNormalizedContinuousDelta},
                            {"p90_normalized_continuous_delta",
                             transition.P90NormalizedContinuousDelta},
                            {"continuity_threshold", continuityThreshold},
                            {"continuous", continuousTransition},
                        });
                      }
                      if (continuousTransition && visualSampleReady) {
                        const auto temporalSample =
                            Fast::Oot3d::BuildNativeFrameTemporalSample(
                                frameCompositionPolicy,
                                Fast::Oot3d::NativeFrameTemporalSampleKind::
                                    Transition,
                                latestVisualFrame->Sequence,
                                currentVisualFrame->Sequence,
                                visualContinuityEpoch, sampleAlpha,
                                static_cast<float>(
                                    presentationElapsedSeconds));
                        executeVisualSample(
                            sample,
                            Oot3dNativeGame::
                                Oot3dPicaPresentationExecutionKind::
                                    InterpolatedFrame,
                            true, temporalSample);
                        ++visualSamplesPresented;
                        visualSampleDrawCount += sample.Draws.size();
                        visualInterpolatedDrawCount +=
                            sample.InterpolatedDraws;
                        visualInterpolatedPerInstanceVertexDrawCount +=
                            sample.InterpolatedPerInstanceVertexDraws;
                        visualInterpolatedPerVertexDrawCount +=
                            sample.InterpolatedPerVertexDraws;
                        presentedVisualSample = true;
                      } else {
                        resynchronizeVisualNamespace = true;
                      }
                    }
                    if (resynchronizeVisualNamespace &&
                        Oot3dNativeGame::ViewOot3dPicaVisualFrame(
                            *currentVisualFrame, sample)) {
                      preparedVisualTransition.Reset();
                      ++visualContinuityEpoch;
                      const auto temporalSample =
                          Fast::Oot3d::BuildNativeFrameTemporalSample(
                              frameCompositionPolicy,
                              Fast::Oot3d::NativeFrameTemporalSampleKind::
                                  Resynchronized,
                              currentVisualFrame->Sequence,
                              currentVisualFrame->Sequence,
                              visualContinuityEpoch, 1.0F,
                              static_cast<float>(
                                  presentationElapsedSeconds));
                      executeVisualSample(
                          sample,
                          Oot3dNativeGame::
                              Oot3dPicaPresentationExecutionKind::CurrentFrame,
                          true, temporalSample);
                      ++visualNamespaceResyncCount;
                      ++visualSamplesPresented;
                      visualSampleDrawCount += sample.Draws.size();
                      presentedVisualSample = true;
                    }
                    if (latestVisualFrame.has_value()) {
                      previousVisualFrame = std::move(latestVisualFrame);
                    } else {
                      latestVisualTransitionContinuous = false;
                    }
                    latestVisualFrame = std::move(currentVisualFrame);
                  }
                }
              }
              if (!selectedNewVisualFrame && !presentedVisualSample &&
                  visualInterpolationEnabled &&
                  latestVisualTransitionContinuous &&
                  previousVisualFrame.has_value() &&
                  latestVisualFrame.has_value() &&
                  preparedVisualTransition.Ready()) {
                auto &sample = visualFrameSampleScratch;
                const float sampleAlpha =
                    visualSampleCadence.Resolve(
                        false, visualInterpolationAlpha);
                if (preparedVisualTransition.Sample(
                        *previousVisualFrame, *latestVisualFrame, sampleAlpha,
                        sample, &visualInterpolationTiming)) {
                  const auto temporalSample =
                      Fast::Oot3d::BuildNativeFrameTemporalSample(
                          frameCompositionPolicy,
                          Fast::Oot3d::NativeFrameTemporalSampleKind::
                              Transition,
                          previousVisualFrame->Sequence,
                          latestVisualFrame->Sequence,
                          visualContinuityEpoch, sampleAlpha,
                          static_cast<float>(presentationElapsedSeconds));
                  executeVisualSample(
                      sample,
                      Oot3dNativeGame::
                          Oot3dPicaPresentationExecutionKind::RepeatedFrame,
                      true, temporalSample);
                  ++visualSamplesPresented;
                  visualSampleDrawCount += sample.Draws.size();
                  visualInterpolatedDrawCount += sample.InterpolatedDraws;
                  visualInterpolatedPerInstanceVertexDrawCount +=
                      sample.InterpolatedPerInstanceVertexDraws;
                  visualInterpolatedPerVertexDrawCount +=
                      sample.InterpolatedPerVertexDraws;
                  presentedVisualSample = true;
                }
              }
              if (!presentedVisualSample &&
                  picaPresentationScheduler.HasSnapshot(
                      topSelection,
                      presentationRenderTargetNamespace)) {
                const auto started = std::chrono::steady_clock::now();
                if (!picaPresentationScheduler.PresentExisting(
                        api, topSelection,
                        presentationRenderTargetNamespace, &renderError)) {
                  throw std::runtime_error(
                      "native top-screen scheduled scanout failed: " +
                      renderError);
                }
                rendererTiming->VisualReplaySeconds += SecondsSince(started);
                ++visualReusedSnapshotPresentations;
              }
              });
            }
          }

          const bool presentNativeBottomFrontend =
              Oot3dNativeGame::ShouldPresentNativeBottomFrontend(
                  launch.UiProfile,
                  uiLifecycleBridge.NativeFrontendPresentationActive());
          const auto bottomFramebuffer = hostServices.BottomFramebuffer();
          if (presentNativeBottomFrontend &&
              bottomFramebuffer.has_value()) {
            ++nativeFrontendPresentationActiveCount;
            auto selectedBottom =
                displayTransfersByOutput.find(bottomFramebuffer->AddressLeft);
            if (selectedBottom == displayTransfersByOutput.end() &&
                bottomFramebuffer->AddressRight != 0U &&
                bottomFramebuffer->AddressRight !=
                    bottomFramebuffer->AddressLeft) {
              selectedBottom = displayTransfersByOutput.find(
                  bottomFramebuffer->AddressRight);
            }
            nativeFrontendBottomTransferHitCount +=
                selectedBottom != displayTransfersByOutput.end() ? 1U : 0U;
            if (selectedBottom != displayTransfersByOutput.end()) {
              renderStream.Submit(
                  [&, bottomSelection = selectedBottom->second] {
                    if (rendererFrameSkipped ||
                        !picaPresentationScheduler.HasSnapshot(
                            bottomSelection,
                            presentationRenderTargetNamespace)) {
                      return;
                    }
                    if (!picaPresentationScheduler.PresentExisting(
                            api, bottomSelection,
                            presentationRenderTargetNamespace, &renderError)) {
                      throw std::runtime_error(
                          "native bottom-screen frontend presentation "
                          "failed: " + renderError);
                    }
                    ++nativeBottomFrontendPresentationCount;
                  });
            }
          }
        }
        phaseTiming.VisualPresentationSeconds +=
            SecondsSince(visualPresentationStart);
      }
      frameCount += advanceGuest ? 1U : 0U;
    }

    if (launch.ExtendedDiagnostics) {
      inputConsumerDiagnostics.Observe(process.Memory(), frameCount);
    }

    for (std::size_t subsystemIndex = 0;
         subsystemIndex < oot3d::ui::kUiSubsystemCount; ++subsystemIndex) {
      const auto subsystem =
          static_cast<oot3d::ui::UiSubsystem>(subsystemIndex);
      const bool topScreenProfile =
          launch.UiProfile == Oot3dNativeGame::Oot3dUiProfile::TopScreen;
      if (topScreenProfile &&
          subsystem != oot3d::ui::UiSubsystem::GameplayHud &&
          subsystem != oot3d::ui::UiSubsystem::TouchControls) {
        continue;
      }
      const bool topScreenPresentation = topScreenProfile;
      if (!topScreenPresentation && !uiLifecycleBridge.Runtime()
                                         .PlanFrame(subsystem)
                                         .run_host_presentation) {
        continue;
      }
      auto primitives =
          topScreenPresentation
              ? uiLifecycleBridge.BuildTopScreenPresentation(subsystem)
              : uiLifecycleBridge.BuildShadowPresentation(subsystem);
      if (topScreenPresentation &&
          subsystem == oot3d::ui::UiSubsystem::GameplayHud) {
        Oot3dNativeGame::AppendTopScreenFreeCameraOptionPresentation(
            nativeCandidateDispatch.TopScreenCamera.Camera, primitives);
      }
      if (topScreenPresentation && subsystem == oot3d::ui::UiSubsystem::TouchControls) {
        ocarinaText.Append(primitives);
      }
      if (topScreenPresentation) {
        lastTopScreenPrimitivesBySubsystem[subsystemIndex] = primitives;
      }
      const auto canvasMode =
          topScreenPresentation
              ? oot3d::ui::N64UiCanvasMode::NativeTopScreen400x240
              : oot3d::ui::N64UiCanvasMode::Widescreen16x9;
      // Texture payloads are read from guest memory here; the renderer
      // only uploads them.
      auto preparedTextures = n64UiRenderer.PrepareTextures(primitives);
      renderStream.Submit([&, primitives = std::move(primitives), width,
                           height, canvasMode,
                           prepared = std::move(preparedTextures)]() mutable {
        if (rendererFrameSkipped) {
          n64UiRenderer.Stage(std::move(prepared));
          return;
        }
        if (!n64UiRenderer.Render(primitives, width, height, canvasMode,
                                  std::move(prepared), &renderError)) {
          throw std::runtime_error("integrated N64 UI presentation failed: " +
                                   renderError);
        }
      });
    }

    phaseStart = std::chrono::steady_clock::now();
    CpuProbe::Scope cpuPresent(CpuProbe::Phase::Present);
    Fast::Renderer3ds::SlowStepTrace presentSteps("present");
    // The first frames render under the preparation screen.
    preparationScreen.Draw(preparationStatus);
    gui->EndDraw();
    presentSteps.Mark("gui_end_draw");
    renderStream.Submit([&, width, height,
                         presentationSerial = presentationFrameCount] {
      if (rendererFrameSkipped) {
        capturedTemporalSample.reset();
        window.EndFrame();
        realtimePacer.ResetDeadline();
        return;
      }
      MaybeWriteFramebufferScreenshot(
          hostArgs, api, width, height,
          static_cast<uint32_t>(presentationSerial), screenshotState,
          capturedTemporalSample ? &*capturedTemporalSample : nullptr);
      capturedTemporalSample.reset();
      const auto waitStart = std::chrono::steady_clock::now();
      CpuProbe::Scope cpuPacing(CpuProbe::Phase::Pacing);
      realtimePacer.WaitForNextRefresh();
      cpuPacing.Stop();
      rendererTiming->PacingWaitSeconds += SecondsSince(waitStart);
      window.EndFrame();
    }, true);
    presentSteps.Mark("render_end_frame");
    picaSemanticTrace.RecordFrameBoundary(
        presentationFrameCount, frameCount, guestRefreshesDue != 0U);
    phaseTiming.PresentSeconds += SecondsSince(phaseStart);
    cpuPresent.Stop();
    ++presentationFrameCount;
    ++runFrameCount;
#if defined(__SWITCH__)
    gSwitchRuntimeHostFrames = runFrameCount;
    gSwitchRuntimeGuestFrames = frameCount;
    // Write once at the first frame and at most once more when a persistent
    // geometry is first observed. Periodic receipt writes would put
    // synchronous SD filesystem work directly in the presentation loop.
    if (runFrameCount == 1U || !gSwitchPicaGeometryCacheObserved) {
      const auto wholeAotStats =
          Oot3dNativeGame::GetOot3dCompiledFunctionStats();
      gSwitchWholeAotBlockLimitExits =
          wholeAotStats.WholeAotBlockLimitExits;
      const auto picaBackend = api.GetNativePicaBackendStats();
      gSwitchPicaGeometryCacheEnabled =
          picaBackend.GeometryCacheEnabled;
      gSwitchPicaGeometryPersistentDraws =
          picaBackend.GeometryPersistentDraws;
      gSwitchPicaGeometryPersistentUploads =
          picaBackend.GeometryPersistentUploads;
      gSwitchPicaGeometryRegistryEntries =
          picaBackend.GeometryRegistryEntries;
      gSwitchPicaGeometryCacheObserved =
          gSwitchPicaGeometryPersistentDraws != 0U &&
          gSwitchPicaGeometryPersistentUploads != 0U &&
          gSwitchPicaGeometryRegistryEntries != 0U;
      if (runFrameCount == 1U || gSwitchPicaGeometryCacheObserved) {
        WriteSwitchBootStage("runtime_running");
      }
    }
#endif
    const auto completedFrameTime = std::chrono::steady_clock::now();
    if (!preparationScreen.Done()) {
      preparationScreen.ObserveFrame(
          std::chrono::duration<double, std::milli>(completedFrameTime - lastCompletedFrameTime).count(),
          completedFrameTime);
      if (preparationScreen.Done()) {
        std::fprintf(stderr,
                     "TRIAEVUM_INITIAL_PREPARATION parts=%zu startup_seconds=%.3f wait_seconds=%.3f "
                     "first_frames=%u first_frames_seconds=%.3f\n",
                     preparationScreen.Parts(), preparationScreen.StartupSeconds(),
                     preparationScreen.WaitSeconds(),
                     preparationScreen.FirstFrames(), preparationScreen.FirstFramesSeconds());
      }
    }
    lastCompletedFrameTime = completedFrameTime;
    if (!benchmarkMeasurementStarted && preparationScreen.Done() &&
        runFrameCount >= hostArgs.BenchmarkWarmupFrames) {
      benchmarkMeasurementStart = completedFrameTime;
      benchmarkMeasurementEnd = completedFrameTime;
      benchmarkMeasurementStarted = true;
      if (cpuProbeRequested) CpuProbe::Thread.StartAt(CpuProbe::Read());
    } else if (benchmarkMeasurementStarted &&
               runFrameCount > hostArgs.BenchmarkWarmupFrames) {
      const auto currentPhases = phaseTiming.Values();
      std::array<double, 15> delta{};
      for (size_t i=0; i<delta.size(); ++i) {
        delta[i]=currentPhases[i]-phaseBeforeFrame[i];
        benchmarkPhaseSeconds[i]+=delta[i];
      }
      const double nestedGuest=phaseTiming.GuestInsidePicaSubmitSeconds-nestedGuestBeforeFrame;
      benchmarkNestedGuestSeconds+=nestedGuest;
      benchmarkGuestClockSeconds+=phaseTiming.GuestClockSeconds-guestClockBeforeFrame;
      const Oot3dNativeGame::PreBackendBudget budget{
        delta[0],delta[7],nestedGuest,delta[8],delta[2],delta[3],delta[5],delta[6]};
      if(budget.Valid()) preBackendTimes.RecordMilliseconds(budget.Total()*1000.0);
      else ++invalidPreBackendIntervals;
      // Present includes pacing. Visual presentation includes backend/replay.
      // Remove each inclusive scope once; keep unclassified host/UI work visible.
      const double envelope=std::chrono::duration<double>(completedFrameTime-benchmarkMeasurementEnd).count()
          -delta[4]-delta[11]-delta[13];
      preBackendEnvelopeTimes.RecordMilliseconds(envelope*1000.0);
      benchmarkUnattributedSeconds+=envelope-budget.Total();
      if (benchmarkFrameTimes) {
        auto phases = phaseTiming.Values();
        for (size_t i = 0; i < phases.size(); ++i)
          phases[i] = (phases[i] - phaseBeforeFrame[i]) * 1000.0;
        slowFrames.Record(runFrameCount, std::chrono::duration<double, std::milli>(
            completedFrameTime - benchmarkMeasurementEnd).count(), phases);
        benchmarkFrameTimes->RecordMilliseconds(
            std::chrono::duration<double, std::milli>(
                completedFrameTime - benchmarkMeasurementEnd).count());
      }
      benchmarkMeasurementEnd = completedFrameTime;
      ++benchmarkMeasuredFrames;
      if (cpuProbeRequested) benchmarkCpuPhases = CpuProbe::Thread.Snapshot();
    }
    if (quickLoadRequested) {
      if (loadState(quickStatePath)) {
        quickSavePending = false;
      }
    } else {
      if (quickSavePending && saveState(quickStatePath)) {
        quickSavePending = false;
      }
      if (Oot3dNativeGame::ShouldCaptureNativeA32AutomatedState(
              launch.SaveStateFrameAvailable, automatedStateSaved,
              runFrameCount, launch.SaveStateFrame) &&
          saveState(launch.SaveStatePath)) {
        automatedStateSaved = true;
        automatedStateSavedAtRunFrame = runFrameCount - 1U;
        automatedStateSavedAtGuestFrame = frameCount;
      }
    }
    ApplyWindowDemoFrameLimit(hostArgs, window, runFrameCount);
  }
  // Every renderer-owned value below is read after the last command ran.
  renderStream.SetMode(Oot3dNativeGame::NativeRenderCommandStream::Mode::Inline);
  if (rendererTiming != &phaseTiming) {
    phaseTiming.PicaBackendSeconds += workerPhaseTiming.PicaBackendSeconds;
    phaseTiming.VisualReplaySeconds += workerPhaseTiming.VisualReplaySeconds;
    phaseTiming.PacingWaitSeconds += workerPhaseTiming.PacingWaitSeconds;
    rendererTiming = &phaseTiming;
  }
  CpuProbe::Thread.Active = false;

#if defined(__SWITCH__)
  WriteSwitchBootStage(
      processResult.Kind ==
              Oot3dNativeGame::NativeA32ProcessRunKind::Terminated
          ? "runtime_guest_terminated"
          : "runtime_window_closed");
#endif

  if (!launch.AudioPcmDumpPath.empty()) {
    Oot3dNativeGame::WriteStereoPcm16Wave(
        launch.AudioPcmDumpPath,
        Oot3dNativeGame::NativeA32DspHle::NativeSampleRate, pcmDumpSamples);
  }

  blockTraceWriter.Write();
  picaSemanticTrace.Finish();

  if (!launch.Host.OutputPath.empty()) {
    nlohmann::json topFramebuffer = nullptr;
    if (const auto top = hostServices.TopFramebuffer(); top.has_value()) {
      topFramebuffer = {
          {"address_left", top->AddressLeft},
          {"address_right", top->AddressRight},
          {"stride", top->Stride},
          {"format", top->Format},
          {"buffer_index", top->BufferIndex},
      };
    }
    nlohmann::json bottomFramebuffer = nullptr;
    if (const auto bottom = hostServices.BottomFramebuffer();
        bottom.has_value()) {
      bottomFramebuffer = {
          {"address_left", bottom->AddressLeft},
          {"address_right", bottom->AddressRight},
          {"stride", bottom->Stride},
          {"format", bottom->Format},
          {"buffer_index", bottom->BufferIndex},
      };
    }
    nlohmann::json recentSvcEvents = nlohmann::json::array();
    nlohmann::json filesystemSvcEvents = nlohmann::json::array();
    nlohmann::json ipcSvcEvents = nlohmann::json::array();
    const auto &svcEvents = hostServices.SvcEvents();
    const size_t firstSvcEvent =
        svcEvents.size() > 32U ? svcEvents.size() - 32U : 0U;
    for (size_t index = firstSvcEvent; index < svcEvents.size(); ++index) {
      const auto &event = svcEvents[index];
      recentSvcEvents.push_back({
          {"immediate", event.Immediate},
          {"pc", event.Pc},
          {"thread_id", event.ThreadId},
          {"handled", event.Handled},
          {"name", event.Name},
          {"detail", event.Detail},
      });
    }
    size_t firstFilesystemEvent = 0;
    size_t filesystemEventCount = 0;
    for (size_t index = svcEvents.size(); index > 0; --index) {
      const auto &event = svcEvents[index - 1U];
      if (event.Name.find("fs:USER") == std::string::npos &&
          event.Name.find("file:romfs") == std::string::npos &&
          event.Name.find("file:savedata") == std::string::npos) {
        continue;
      }
      firstFilesystemEvent = index - 1U;
      if (++filesystemEventCount == 64U) {
        break;
      }
    }
    for (size_t index = firstFilesystemEvent; index < svcEvents.size();
         ++index) {
      const auto &event = svcEvents[index];
      if (event.Name.find("fs:USER") == std::string::npos &&
          event.Name.find("file:romfs") == std::string::npos &&
          event.Name.find("file:savedata") == std::string::npos) {
        continue;
      }
      filesystemSvcEvents.push_back({
          {"immediate", event.Immediate},
          {"pc", event.Pc},
          {"thread_id", event.ThreadId},
          {"handled", event.Handled},
          {"name", event.Name},
          {"detail", event.Detail},
      });
    }
    size_t firstIpcEvent = 0;
    size_t ipcEventCount = 0;
    for (size_t index = svcEvents.size(); index > 0; --index) {
      const auto &event = svcEvents[index - 1U];
      if (event.Immediate != 0x32U ||
          event.Name.find("file:romfs:Read") != std::string::npos ||
          event.Name.find("gsp::Gpu:FlushDataCache") != std::string::npos) {
        continue;
      }
      firstIpcEvent = index - 1U;
      if (++ipcEventCount == 128U) {
        break;
      }
    }
    for (size_t index = firstIpcEvent; index < svcEvents.size(); ++index) {
      const auto &event = svcEvents[index];
      if (event.Immediate != 0x32U ||
          event.Name.find("file:romfs:Read") != std::string::npos ||
          event.Name.find("gsp::Gpu:FlushDataCache") != std::string::npos) {
        continue;
      }
      ipcSvcEvents.push_back({
          {"pc", event.Pc},
          {"thread_id", event.ThreadId},
          {"handled", event.Handled},
          {"name", event.Name},
          {"detail", event.Detail},
      });
    }
    std::map<uint32_t, const Oot3dNativeGame::NativeA32CtrSvcEvent *>
        lastSvcByThread;
    for (const auto &event : svcEvents) {
      lastSvcByThread[event.ThreadId] = &event;
    }
    nlohmann::json lastThreadSvcEvents = nlohmann::json::array();
    for (const auto &[threadId, event] : lastSvcByThread) {
      lastThreadSvcEvents.push_back({
          {"thread_id", threadId},
          {"immediate", event->Immediate},
          {"pc", event->Pc},
          {"handled", event->Handled},
          {"name", event->Name},
          {"detail", event->Detail},
      });
    }
    nlohmann::json threads = nlohmann::json::array();
    for (uint32_t threadId = 0; threadId < process.ThreadCount(); ++threadId) {
      const auto *state = process.ThreadState(threadId);
      nlohmann::json commandBufferWords = nlohmann::json::array();
      nlohmann::json registers = nlohmann::json::array();
      nlohmann::json stackWords = nlohmann::json::array();
      nlohmann::json threadArgumentWords = nlohmann::json::array();
      if (state != nullptr) {
        for (uint32_t value : state->r) {
          registers.push_back(value);
        }
        for (uint32_t offset = 0; offset < 0x40U; offset += 4U) {
          uint32_t value = 0;
          if (!process.Memory().Read32(state->r[13] + offset, &value)) {
            break;
          }
          stackWords.push_back(value);
        }
        const uint32_t commandBuffer = state->thread_pointer + 0x80U;
        for (uint32_t offset = 0; offset < 0x30U; offset += 4U) {
          uint32_t value = 0;
          if (!process.Memory().Read32(commandBuffer + offset, &value)) {
            break;
          }
          commandBufferWords.push_back(value);
        }
      }
      const uint32_t threadArgument =
          process.ThreadArgument(threadId).value_or(0U);
      for (uint32_t offset = 0; threadArgument != 0U && offset < 0x20U;
           offset += 4U) {
        uint32_t value = 0;
        if (!process.Memory().Read32(threadArgument + offset, &value)) {
          break;
        }
        threadArgumentWords.push_back(value);
      }
      threads.push_back({
          {"id", threadId},
          {"entry_address", process.ThreadEntryAddress(threadId).value_or(0U)},
          {"argument", process.ThreadArgument(threadId).value_or(0U)},
          {"priority", process.ThreadPriority(threadId).value_or(0U)},
          {"status", static_cast<uint32_t>(process.ThreadStatus(threadId))},
          {"pc", state != nullptr ? state->r[15] : 0U},
          {"registers", std::move(registers)},
          {"stack", std::move(stackWords)},
          {"command_buffer", std::move(commandBufferWords)},
          {"argument_words", std::move(threadArgumentWords)},
      });
    }
    nlohmann::json projectionCallsites = nlohmann::json::array();
    for (const auto &[key, stats] :
         widescreenProjection.Projection.Callsites) {
      projectionCallsites.push_back({
          {"function", key.first},
          {"return_address", key.second},
          {"calls", stats.Calls},
          {"first_left", stats.FirstLeft},
          {"first_right", stats.FirstRight},
          {"last_frustum", stats.LastFrustum},
      });
    }
    std::vector<std::pair<uint32_t, uint64_t>> sampledBlocks(
        widescreenProjection.BlockSamples.begin(),
        widescreenProjection.BlockSamples.end());
    std::sort(sampledBlocks.begin(), sampledBlocks.end(),
              [](const auto &left, const auto &right) {
                return left.second != right.second ? left.second > right.second
                                                   : left.first < right.first;
              });
    uint64_t totalBlockSampleCount = 0;
    for (const auto &sampledBlock : sampledBlocks) {
      totalBlockSampleCount += sampledBlock.second;
    }
    constexpr size_t kMaximumReportedA32Blocks = 4096U;
    if (sampledBlocks.size() > kMaximumReportedA32Blocks) {
      sampledBlocks.resize(kMaximumReportedA32Blocks);
    }
    nlohmann::json sampledBlockEntries = nlohmann::json::array();
    uint64_t blockSampleCount = 0;
    for (const auto &[pc, samples] : sampledBlocks) {
      blockSampleCount += samples;
      sampledBlockEntries.push_back({
          {"pc", pc},
          {"samples", samples},
          {"estimated_entries", samples * 64U},
      });
    }
    const auto readQueueState = [&](uint32_t address) {
      nlohmann::json words = nlohmann::json::array();
      std::array<uint32_t, 13> values{};
      size_t valueCount = 0;
      for (uint32_t offset = 0; offset < 0x34U; offset += 4U) {
        uint32_t value = 0;
        if (!process.Memory().Read32(address + offset, &value)) {
          break;
        }
        values[valueCount++] = value;
        words.push_back({{"offset", offset}, {"value", value}});
      }
      const auto valueAt = [&](size_t index) {
        return index < valueCount ? values[index] : 0U;
      };
      return nlohmann::json{
          {"address", address},
          {"storage", valueAt(0)},
          {"item_semaphore", valueAt(1)},
          {"free_semaphore", valueAt(3)},
          {"capacity", valueAt(8)},
          {"read_index", valueAt(9)},
          {"count", valueAt(10)},
          {"active_consumers", valueAt(11)},
          {"active_producers", valueAt(12)},
          {"words", std::move(words)},
      };
    };
    const auto readRendererCommandState = [&]() {
      nlohmann::json words = nlohmann::json::array();
      for (uint32_t offset = 0x168U; offset <= 0x188U; offset += 4U) {
        uint32_t value = 0;
        if (!process.Memory().Read32(kRendererCommandQueue + offset, &value)) {
          break;
        }
        words.push_back({{"offset", offset}, {"value", value}});
      }
      return words;
    };
    const auto sceneViewSnapshot =
        sceneViewProbe.Capture(process.Memory());
    const auto playerTimingSnapshot =
        playerTimingProbe.Capture(process.Memory());
    const auto serializeTopScreenPrimitives = [](const auto &primitives) {
      nlohmann::json serialized = nlohmann::json::array();
      for (const auto &primitive : primitives) {
        serialized.push_back(
            {{"role", static_cast<std::uint32_t>(primitive.role)},
             {"owner_address", primitive.owner_address},
             {"descriptor_address", primitive.descriptor_address},
             {"source_quad", primitive.source_quad},
             {"texture", primitive.texture.semantic_name},
             {"destination",
              {primitive.destination.x, primitive.destination.y,
               primitive.destination.width, primitive.destination.height}},
             {"uv",
              {primitive.uv.x, primitive.uv.y, primitive.uv.width,
               primitive.uv.height}},
             {"color",
              {primitive.color.red, primitive.color.green,
               primitive.color.blue, primitive.color.alpha}},
             {"layer", primitive.layer},
             {"visible", primitive.visible}});
      }
      return serialized;
    };
    nlohmann::json topScreenPresentations = nlohmann::json::object();
    for (std::size_t subsystemIndex = 0;
         subsystemIndex < oot3d::ui::kUiSubsystemCount; ++subsystemIndex) {
      const auto subsystem =
          static_cast<oot3d::ui::UiSubsystem>(subsystemIndex);
      topScreenPresentations[oot3d::ui::UiSubsystemName(subsystem)] =
          serializeTopScreenPrimitives(
              lastTopScreenPrimitivesBySubsystem[subsystemIndex]);
    }
    const auto topScreenGameplayPrimitiveAudit =
        serializeTopScreenPrimitives(lastTopScreenPrimitivesBySubsystem[
            static_cast<std::size_t>(oot3d::ui::UiSubsystem::GameplayHud)]);
    const auto graphicsSettings =
        Fast::Oot3d::GraphicsSettingsRuntime::Instance().Snapshot();
    const auto &directionalShadows =
        graphicsSettings.Effects.DirectionalShadows;
    const auto &realtimePacingStats = realtimePacer.Stats();
    const double benchmarkHostSeconds =
        benchmarkMeasuredFrames == 0U
            ? 0.0
            : std::chrono::duration<double>(benchmarkMeasurementEnd -
                                            benchmarkMeasurementStart)
                  .count();
    WriteJsonFile(
        launch.Host.OutputPath,
        {
            {"schema", "oot3d_native_a32_vulkan_runtime_v1"},
            {"structural_scenario", scenarioBootstrap.has_value()
                                        ? scenarioBootstrap->ToJson()
                                        : nlohmann::json(nullptr)},
            {"frames", frameCount},
            {"run_frames", runFrameCount},
            {"guest_refresh_frames", frameCount},
            {"presentation_frames", presentationFrameCount},
            {"pica_composition",
             [&]() {
               const auto &stats = picaCompositionTracker.Stats();
               return nlohmann::json{
                   {"cmb_opaque_pass_entries", stats.CmbOpaquePassEntries},
                   {"cmb_transparent_pass_entries",
                    stats.CmbTransparentPassEntries},
                   {"cmb_pass_exits", stats.CmbPassExits},
                   {"invalid_cmb_pass_entries", stats.InvalidCmbPassEntries},
                   {"invalid_cmb_return_addresses",
                    stats.InvalidCmbReturnAddresses},
                   {"ui_scope_entries", stats.UiScopeEntries},
                   {"ui_scope_exits", stats.UiScopeExits},
                   {"ui_command_spans", stats.UiCommandSpans},
                   {"atmosphere_scope_entries", stats.AtmosphereScopeEntries},
                   {"atmosphere_scope_exits", stats.AtmosphereScopeExits},
                   {"atmosphere_packet_spans", stats.AtmospherePacketSpans},
                   {"direct_atmosphere_spans", stats.DirectAtmosphereSpans},
                   {"direct_atmosphere_bytes", stats.DirectAtmosphereBytes},
                   {"nested_atmosphere_scope_entries",
                    stats.NestedAtmosphereScopeEntries},
                   {"invalid_atmosphere_command_ranges",
                    stats.InvalidAtmosphereCommandRanges},
                   {"recovered_atmosphere_packet_spans",
                    stats.RecoveredAtmospherePacketSpans},
                   {"invalid_atmosphere_return_addresses",
                    stats.InvalidAtmosphereReturnAddresses},
                   {"ignored_unrelated_command_list_executions",
                    stats.IgnoredUnrelatedCommandListExecutions},
                   {"atmosphere_selector_read_failures",
                    stats.AtmosphereSelectorReadFailures},
                   {"primitive_packet_entries",
                    stats.PrimitivePacketEntries},
                   {"primitive_packet_exits", stats.PrimitivePacketExits},
                   {"primitive_packet_spans", stats.PrimitivePacketSpans},
                   {"primitive_packet_bytes", stats.PrimitivePacketBytes},
                   {"nested_primitive_packet_entries",
                    stats.NestedPrimitivePacketEntries},
                   {"invalid_primitive_packet_return_addresses",
                    stats.InvalidPrimitivePacketReturnAddresses},
                   {"invalid_primitive_packet_ranges",
                    stats.InvalidPrimitivePacketRanges},
                   {"unmatched_primitive_packet_exits",
                    stats.UnmatchedPrimitivePacketExits},
                   {"mesh_packet_entries", stats.MeshPacketEntries},
                   {"recorded_packet_spans", stats.RecordedPacketSpans},
                   {"coalesced_packet_spans", stats.CoalescedPacketSpans},
                   {"unattributed_packet_spans",
                    stats.UnattributedPacketSpans},
                   {"invalid_packet_spans", stats.InvalidPacketSpans},
                   {"overlapping_packet_spans",
                    stats.OverlappingPacketSpans},
                   {"consumed_packet_spans", stats.ConsumedPacketSpans},
                   {"partially_overlapping_submissions",
                    stats.PartiallyOverlappingSubmissions},
                   {"evicted_packet_spans", stats.EvictedPacketSpans},
                   {"read_failures", stats.ReadFailures},
               };
             }()},
            {"topscreen_gameplay_primitives",
             topScreenGameplayPrimitiveAudit},
            {"topscreen_presentations", std::move(topScreenPresentations)},
            {"vblanks", vblankCount},
            {"savestates",
             {{"quick_path", quickStatePath.string()},
              {"initial_load_path", launch.LoadStatePath.string()},
              {"automated_save_path", launch.SaveStatePath.string()},
              {"automated_save_frame",
               launch.SaveStateFrameAvailable
                   ? nlohmann::json(launch.SaveStateFrame)
                   : nlohmann::json(nullptr)},
              {"automated_save_actual_run_frame",
               automatedStateSavedAtRunFrame.has_value()
                   ? nlohmann::json(*automatedStateSavedAtRunFrame)
                   : nlohmann::json(nullptr)},
              {"automated_save_actual_guest_frame",
               automatedStateSavedAtGuestFrame.has_value()
                   ? nlohmann::json(*automatedStateSavedAtGuestFrame)
                   : nlohmann::json(nullptr)},
              {"save_count", savestateSaveCount},
              {"load_count", savestateLoadCount},
              {"save_bytes", savestateSaveBytes},
              {"load_bytes", savestateLoadBytes},
              {"save_seconds", savestateSaveSeconds},
              {"load_seconds", savestateLoadSeconds},
              {"capture_seconds", savestateCaptureSeconds},
              {"encode_seconds", savestateEncodeSeconds},
              {"write_seconds", savestateWriteSeconds},
              {"read_seconds", savestateReadSeconds},
              {"decode_seconds", savestateDecodeSeconds},
              {"restore_seconds", savestateRestoreSeconds},
              {"last_semantic_fingerprint",
               std::to_string(lastSavestateSemanticFingerprint)},
              {"legacy_gameplay_clock_restores",
               legacySavestateClockRestoreCount},
              {"legacy_presentation_scheduler_restores",
               legacySavestatePresentationSchedulerRestoreCount},
              {"legacy_frame_rate_policy_restores",
               legacySavestateFrameRatePolicyRestoreCount},
              {"legacy_topscreen_temporal_restores",
               legacySavestateTopScreenTemporalRestoreCount},
              {"legacy_pica_visual_replay_restores",
               legacySavestatePicaVisualReplayRestoreCount},
              {"legacy_pica_texture_cache_restores",
               legacySavestatePicaTextureCacheRestoreCount},
              {"legacy_pica_color_target_restores",
               legacySavestatePicaColorTargetRestoreCount},
              {"legacy_pica_presentation_restores",
               legacySavestatePicaPresentationRestoreCount},
              {"pica_textures_captured",
               savestatePicaTexturesCaptured},
              {"pica_texture_bytes_captured",
               savestatePicaTextureBytesCaptured},
              {"pica_textures_restored",
               savestatePicaTexturesRestored},
              {"pica_texture_bytes_restored",
               savestatePicaTextureBytesRestored},
              {"pica_visual_replay_bytes_captured",
               savestatePicaVisualReplayBytesCaptured},
              {"pica_visual_replay_bytes_restored",
               savestatePicaVisualReplayBytesRestored},
              {"pica_color_targets_captured",
               savestatePicaColorTargetsCaptured},
              {"pica_color_target_bytes_captured",
               savestatePicaColorTargetBytesCaptured},
              {"pica_color_targets_restored",
               savestatePicaColorTargetsRestored},
              {"pica_color_target_bytes_restored",
               savestatePicaColorTargetBytesRestored},
              {"pica_display_images_captured",
               savestatePicaDisplayImagesCaptured},
              {"pica_display_image_bytes_captured",
               savestatePicaDisplayImageBytesCaptured},
              {"pica_display_images_restored",
               savestatePicaDisplayImagesRestored},
              {"pica_display_image_bytes_restored",
               savestatePicaDisplayImageBytesRestored}}},
             {"realtime_pacing",
             {{"enabled", realtimePacer.Enabled()},
              {"multimedia_scheduling_active", realtimePacer.MultimediaSchedulingActive()},
              {"high_resolution_wait_available",
               realtimePacer.HighResolutionWaitAvailable()},
              {"basis", "realtime_when_no_frame_limit"},
              {"waits", realtimePacingStats.Waits},
              {"deadline_misses",
               realtimePacingStats.DeadlineMisses},
              {"carried_deadline_debt",
               realtimePacingStats.CarriedDeadlineDebt},
              {"deadline_resyncs",
               realtimePacingStats.DeadlineResyncs},
              {"sleep_seconds", realtimePacingStats.SleepSeconds},
              {"spin_seconds", realtimePacingStats.SpinSeconds},
              {"host_loop_seconds", realtimePacer.ElapsedSeconds()},
              {"presentation_intervals",
               realtimePacingStats.PresentationIntervals},
              {"mean_interval_seconds",
               realtimePacer.MeanIntervalSeconds()},
              {"minimum_interval_seconds",
               realtimePacingStats.MinimumIntervalSeconds},
              {"maximum_interval_seconds",
               realtimePacingStats.MaximumIntervalSeconds},
              {"rms_interval_error_seconds",
               realtimePacer.RmsIntervalErrorSeconds()},
              {"maximum_interval_error_seconds",
               realtimePacingStats.MaximumIntervalErrorSeconds},
              {"maximum_lateness_seconds",
               realtimePacingStats.MaximumLatenessSeconds},
              {"target_refresh_hz",
               realtimePacer.TargetRateHz() == 0U
                   ? nlohmann::json(nullptr)
                   : nlohmann::json(realtimePacer.TargetRateHz())}}},
            {"guest_clock",
             {{"system_ticks", hostServices.SystemTicks()},
              {"next_vblank_tick", nextVblankTick},
              {"late_vblank_deadlines", lateVblankDeadlineCount},
              {"maximum_vblank_deadline_overshoot_ticks",
               maximumVblankDeadlineOvershootTicks},
              {"maximum_vblank_deadline_overshoot_seconds",
               static_cast<double>(maximumVblankDeadlineOvershootTicks) /
                   static_cast<double>(kCtrArm11TicksPerSecond)}}},
            {"initial_preparation",
             {{"parts", preparationScreen.Parts()},
              {"startup_seconds", preparationScreen.StartupSeconds()},
              {"wait_seconds", preparationScreen.WaitSeconds()},
              {"first_frames", preparationScreen.FirstFrames()},
              {"first_frames_seconds", preparationScreen.FirstFramesSeconds()},
              {"completed", preparationScreen.Done()},
              {"note", "The guest waits for the startup preparation; the first frames render under the "
                       "preparation screen until they run within budget. The benchmark window starts after it."}}},
            {"benchmark_window",
             {{"warmup_frames_requested",
               hostArgs.BenchmarkWarmupFrames},
              {"warmup_frames_completed",
               std::min<uint64_t>(runFrameCount,
                                  hostArgs.BenchmarkWarmupFrames)},
              {"measured_frames", benchmarkMeasuredFrames},
              {"thread_cpu_ledger", [&]() -> nlohmann::json {
                nlohmann::json phases = nlohmann::json::object();
                double windowCpu = 0.0;
                for (size_t i = 0; i < benchmarkCpuPhases.size(); ++i) {
                  const auto& p = benchmarkCpuPhases[i];
                  windowCpu += p.User + p.Kernel;
                  phases[CpuProbe::Names[i]] = {
                      {"wall_seconds", p.Wall}, {"thread_cycles", p.Cycles},
                      {"intervals", p.Intervals}};
                }
                return {{"requested", cpuProbeRequested},
                    {"available_in_build", CpuProbe::BuildEnabled},
                    {"clock", "GetThreadTimes_and_QueryThreadCycleTime"},
                    {"read_failures", CpuProbe::Thread.ReadFailures},
                    {"window_thread_cpu_seconds", windowCpu},
                    {"scope", "exclusive_main_thread_same_post_warmup_window"},
                    {"warning", "quantized_CPU_valid_only_for_whole_window; off_CPU_includes_preemption_not_just_waits; phase_cycles_are_not_seconds"},
                    {"phases", std::move(phases)}};
              }()},
              {"pre_backend_cpu", [&]() -> nlohmann::json {
                const auto& p=benchmarkPhaseSeconds;
                const Oot3dNativeGame::PreBackendBudget b{
                  p[0],p[7],benchmarkNestedGuestSeconds,p[8],p[2],p[3],p[5],p[6]};
                return {{"scope","guest_including_gsp_pica_plus_plan_capture_host_audio_before_nri_execute"},
                  {"samples",preBackendTimes.Count()}, {"invalid_intervals",invalidPreBackendIntervals},
                  {"guest_seconds",b.Guest}, {"plan_seconds",b.Plan},
                  {"guest_clock_included_seconds",benchmarkGuestClockSeconds},
                  {"queue_capture_seconds",b.QueueAndCapture()},
                  {"nested_guest_subtracted_seconds",b.GuestInsideSubmission},
                  {"host_seconds",b.Host}, {"input_seconds",b.Input},
                  {"dsp_seconds",b.Dsp}, {"audio_output_seconds",b.AudioOutput},
                  {"total_seconds",b.Total()},
                  {"mean_ms",preBackendTimes.MeanMs()},
                  {"host_envelope_mean_ms",preBackendEnvelopeTimes.MeanMs()},
                  {"host_envelope_p95_upper_ms",preBackendEnvelopeTimes.Count()?nlohmann::json(*preBackendEnvelopeTimes.QuantileUpperMs(.95)):nlohmann::json(nullptr)},
                  {"unattributed_host_ui_seconds",benchmarkUnattributedSeconds},
                  {"p95_upper_ms",preBackendTimes.Count()?nlohmann::json(*preBackendTimes.QuantileUpperMs(.95)):nlohmann::json(nullptr)},
                  {"maximum_ms",preBackendTimes.MaximumMs()}};
              }()},
              {"frame_times", [&]() -> nlohmann::json {
                 if (!benchmarkFrameTimes || !benchmarkFrameTimes->Count()) return nullptr;
                 const auto& times = *benchmarkFrameTimes;
                 return {{"samples", times.Count()}, {"invalid_samples", times.Invalid()},
                         {"quantile_resolution_ms", times.ResolutionMs},
                         {"overflow_samples", times.Overflow()},
                         {"p50_upper_ms", *times.QuantileUpperMs(0.50)},
                         {"p95_upper_ms", *times.QuantileUpperMs(0.95)},
                         {"p99_upper_ms", *times.QuantileUpperMs(0.99)},
                         {"maximum_ms", times.MaximumMs()}, {"mean_ms", times.MeanMs()},
                         {"over_60hz_budget", times.Over60Hz()},
                         {"over_30hz_budget", times.Over30Hz()}};
               }()},
              {"host_seconds",
               benchmarkMeasuredFrames == 0U
                   ? nlohmann::json(nullptr)
                   : nlohmann::json(benchmarkHostSeconds)},
              {"frames_per_second",
               benchmarkMeasuredFrames == 0U || benchmarkHostSeconds <= 0.0
                   ? nlohmann::json(nullptr)
                   : nlohmann::json(
                         static_cast<double>(benchmarkMeasuredFrames) /
                         benchmarkHostSeconds)},
              {"fixed_delta_seconds",
               hostArgs.FixedDeltaSeconds > 0.0
                   ? nlohmann::json(hostArgs.FixedDeltaSeconds)
                   : nlohmann::json(nullptr)},
              {"throughput_mode", hostArgs.ThroughputBenchmark},
              {"vsync",
               hostArgs.ThroughputBenchmark ? false
                                            : graphicsSettings.VSync},
              {"sdl_frame_limiter_enabled",
               !hostArgs.ThroughputBenchmark &&
                   window.GetTargetFps() > 0},
              {"pacing_enabled", realtimePacer.Enabled()},
              {"parallel_presentation", parallelPresentation}}},
            {"frame_rate",
             [&]() {
                const auto &contract = frameRatePolicy.Contract();
                const auto &stats = frameRatePolicy.Stats();
                const auto &time =
                    frameRatePolicy.GameplayClock().Context();
                const auto &presentationStats = presentationScheduler.Stats();
                const auto &cadenceStats = visualSampleCadence.Stats();
                const auto &playerTemporalStats =
                    playerTemporalBridge.Stats();
                return nlohmann::json{
                    {"mode",
                     Oot3dNativeGame::GameplayTimingModeName(contract.Mode)},
                    {"implementation_status",
                     contract.Mode ==
                             Oot3dNativeGame::GameplayTimingMode::Enhanced60
                         ? "experimental_a32_compatibility"
                         : "baseline"},
                    {"original_simulation_hz",
                     Oot3dNativeGame::kOot3dOriginalSimulationRateHz},
                   {"simulation_hz", contract.SimulationRateHz},
                   {"presentation_hz",
                    contract.PresentationRateUnlimited
                        ? nlohmann::json(nullptr)
                        : nlohmann::json(contract.PresentationRateHz)},
                   {"presentation_unlimited",
                    contract.PresentationRateUnlimited},
                   {"presentation_frames",
                    presentationStats.PresentationFrames},
                   {"guest_refreshes_scheduled",
                    presentationStats.GuestRefreshes},
                   {"deferred_guest_refresh_observations",
                    presentationStats.DeferredGuestRefreshObservations},
                   {"guest_refreshes_dropped",
                    presentationStats.DroppedGuestRefreshes},
                   {"batched_presentation_frames",
                    presentationStats.BatchedPresentationFrames},
                   {"maximum_guest_refresh_batch",
                    presentationStats.MaximumGuestRefreshBatch},
                   {"guest_refresh_batch_limit",
                    Oot3dNativeGame::kMaximumGuestRefreshesPerPresentation},
                   {"maximum_deferred_guest_refreshes",
                    presentationStats.MaximumDeferredGuestRefreshes},
                   {"deferred_guest_refresh_limit",
                    Oot3dNativeGame::kMaximumDeferredGuestRefreshes},
                    {"step_seconds", contract.StepSeconds},
                    {"native_update_rate", contract.NativeUpdateRate},
                    {"native_step_scale", contract.NativeStepScale},
                    {"logical_frame_delta", contract.LogicalFrameDelta},
                    {"simulation_tick", time.SimulationTick},
                    {"previous_logical_frame",
                     time.PreviousLogicalFrame},
                    {"current_logical_frame", time.CurrentLogicalFrame},
                    {"logical_frame_index", time.LogicalFrameIndex},
                    {"crossed_logical_frame", time.CrossedLogicalFrame},
                    {"a32_update_rate", contract.A32UpdateRate},
                    {"a32_update_rate_exact", contract.A32UpdateRateExact},
                    {"visual_interpolation_allowed",
                     contract.VisualInterpolationAllowed},
                    {"visual_interpolation_active",
                     visualInterpolationEnabled},
                    {"visual_interpolation_mode",
                     Fast::Oot3d::NativeVisualInterpolationModeName(
                         frameCompositionPolicy.Interpolation)},
                    {"visual_sample_multiplier",
                     frameCompositionPolicy.FixedSampleMultiplier},
                    {"visual_sample_cadence",
                     {{"fixed_multiplier",
                       visualSampleCadence.FixedSampleMultiplier()},
                      {"new_source_frames",
                       cadenceStats.NewSourceFrames},
                      {"repeated_samples",
                       cadenceStats.RepeatedSamples},
                      {"clamped_repeated_samples",
                       cadenceStats.ClampedRepeatedSamples},
                      {"last_sample_ordinal",
                       cadenceStats.SampleOrdinal}}},
                    {"visual_presentation_rate_hz",
                     frameCompositionPolicy.TargetPresentationRateHz != 0U
                         ? nlohmann::json(
                               frameCompositionPolicy.TargetPresentationRateHz)
                         : nlohmann::json(nullptr)},
                    {"visual_continuity_epoch", visualContinuityEpoch},
                    {"compatibility_island",
                     contract.Mode ==
                             Oot3dNativeGame::GameplayTimingMode::Enhanced60
                         ? nlohmann::json("a32_gameplay_owner_graph")
                         : nlohmann::json(nullptr)},
                    {"player_temporal_bridge",
                     {{"enabled",
                       widescreenProjection.PlayerTemporalBridge != nullptr},
                      {"collision_entries",
                       playerTemporalStats.CollisionEntries},
                      {"collision_returns",
                       playerTemporalStats.CollisionReturns},
                      {"logical_frame_returns",
                       playerTemporalStats.LogicalFrameReturns},
                      {"intermediate_returns",
                       playerTemporalStats.IntermediateReturns},
                      {"reconciliations",
                       playerTemporalStats.FloorTimerReconciliations},
                      {"immediate_floor_type_changes",
                       playerTemporalStats.ImmediateFloorTypeChanges},
                      {"native_mutation_mismatches",
                       playerTemporalStats.NativeMutationMismatches},
                      {"ledge_increment_paths",
                       playerTemporalStats.LedgeIncrementPaths},
                      {"ledge_wire_overflow_reconciliations",
                       playerTemporalStats
                           .LedgeWireOverflowReconciliations},
                      {"immediate_ledge_type_changes",
                       playerTemporalStats.ImmediateLedgeTypeChanges},
                      {"ledge_timer_resets",
                       playerTemporalStats.LedgeTimerResets},
                      {"ledge_mutation_mismatches",
                       playerTemporalStats.LedgeMutationMismatches},
                      {"dynamic_wall_gate_evaluations",
                       playerTemporalStats.DynamicWallGateEvaluations},
                      {"high_ledge_gate_evaluations",
                       playerTemporalStats.HighLedgeGateEvaluations},
                      {"low_ledge_gate_evaluations",
                       playerTemporalStats.LowLedgeGateEvaluations},
                      {"ledge_gates_reached",
                       playerTemporalStats.LedgeGatesReached},
                      {"unmatched_returns",
                       playerTemporalStats.UnmatchedReturns},
                      {"unmatched_ledge_increment_paths",
                       playerTemporalStats.UnmatchedLedgeIncrementPaths},
                      {"read_failures", playerTemporalStats.ReadFailures},
                      {"write_failures", playerTemporalStats.WriteFailures}}},
                    {"game_state_updates_observed",
                     stats.GameStateUpdatesObserved},
                   {"update_rate_writes", stats.UpdateRateWrites},
                   {"frame_pacing_commits_observed",
                    stats.FramePacingCommitsObserved},
                   {"frame_pacing_frames_observed",
                    stats.FramePacingFramesObserved},
                   {"frame_pacing_writes", stats.FramePacingWrites},
                   {"frame_pacing_decisions_observed",
                    stats.FramePacingDecisionsObserved},
                   {"frame_pacing_deadline_writes",
                    stats.FramePacingDeadlineWrites},
                   {"read_failures", stats.ReadFailures},
                   {"write_failures", stats.WriteFailures},
                   {"last_game_state_address", stats.LastGameStateAddress},
                   {"last_time_state_address", stats.LastTimeStateAddress},
                   {"last_observed_update_rate",
                    stats.HasObservedUpdateRate
                        ? nlohmann::json(stats.LastObservedUpdateRate)
                        : nlohmann::json(nullptr)},
                   {"last_frame_pacing_state_address",
                    stats.LastFramePacingStateAddress},
                   {"last_observed_maximum_interval",
                    stats.HasObservedFramePacing
                        ? nlohmann::json(stats.LastObservedMaximumInterval)
                        : nlohmann::json(nullptr)},
                   {"last_observed_interval",
                    stats.HasObservedFramePacing
                        ? nlohmann::json(stats.LastObservedInterval)
                        : nlohmann::json(nullptr)},
                   {"last_observed_deadline",
                    stats.HasObservedFramePacing
                        ? nlohmann::json(stats.LastObservedDeadline)
                        : nlohmann::json(nullptr)},
                   {"last_observed_vsync_counter",
                    stats.HasObservedFramePacing
                        ? nlohmann::json(stats.LastObservedVsyncCounter)
                        : nlohmann::json(nullptr)},
               };
             }()},
            {"temporal_event_ledger",
             [&]() {
               const auto &stats = temporalEventLedger.Stats();
               nlohmann::json events = nlohmann::json::array();
               for (const auto &event : temporalEventLedger.Events()) {
                 events.push_back({
                     {"sequence", event.Sequence},
                     {"simulation_tick", event.SimulationTick},
                     {"logical_frame", event.LogicalFrame},
                     {"logical_frame_index", event.LogicalFrameIndex},
                     {"kind",
                      std::string(
                          Oot3dNativeGame::NativeTemporalEventKindName(
                              event.Kind))},
                     {"subject_address", event.SubjectAddress},
                     {"previous_value", event.PreviousValue},
                     {"current_value", event.CurrentValue},
                 });
               }
               return nlohmann::json{
                   {"enabled", launch.ExtendedDiagnostics},
                   {"source", "a32_player_semantic_transition_probe"},
                   {"player_observations", stats.PlayerObservations},
                   {"player_identity_changes",
                    stats.PlayerIdentityChanges},
                   {"events_observed", stats.EventsObserved},
                   {"events_recorded", stats.EventsRecorded},
                   {"events_dropped", stats.EventsDropped},
                   {"baseline_fingerprint",
                    stats.BaselineFingerprint},
                   {"event_fingerprint", stats.EventFingerprint},
                   {"events", std::move(events)},
               };
             }()},
            {"player_timing",
             [&]() {
               const auto &stats = playerTimingProbe.Stats();
               nlohmann::json state = nullptr;
               if (playerTimingSnapshot.has_value()) {
                 const auto &player = *playerTimingSnapshot;
                 state = {
                     {"player_address", player.PlayerAddress},
                     {"play_state_address", player.PlayStateAddress},
                     {"time_state_address", player.TimeStateAddress},
                     {"world_position",
                      {player.WorldX, player.WorldY, player.WorldZ}},
                     {"velocity",
                      {player.VelocityX, player.VelocityY, player.VelocityZ}},
                     {"actor_speed", player.ActorSpeed},
                     {"player_speed", player.PlayerSpeed},
                     {"floor_height", player.FloorHeight},
                     {"underwater_timer", player.UnderwaterTimer},
                     {"melee_weapon_action_timer",
                      player.MeleeWeaponActionTimer},
                     {"melee_weapon_combo_state",
                      player.MeleeWeaponComboState},
                     {"item_action_state_or_burn_timer",
                      player.ItemActionStateOrBurnTimer},
                     {"ledge_climb_type", player.LedgeClimbType},
                     {"ledge_climb_delay_timer",
                      player.LedgeClimbDelayTimer},
                     {"textbox_button_cooldown_timer",
                      player.TextboxButtonCooldownTimer},
                     {"damage_flicker_animation_counter",
                      player.DamageFlickerAnimationCounter},
                     {"damage_run_timer", player.DamageRunTimer},
                     {"item_action_cooldown_timer",
                      player.ItemActionCooldownTimer},
                     {"collision_sfx_cooldown_timer",
                      player.CollisionSfxCooldownTimer},
                     {"invincibility_timer", player.InvincibilityTimer},
                     {"floor_type_timer", player.FloorTypeTimer},
                     {"previous_floor_type", player.PreviousFloorType},
                     {"respawn_damage_state",
                      player.RespawnDamageState},
                     {"random_turn_state", player.RandomTurnState},
                     {"random_turn_timer", player.RandomTurnTimer},
                     {"attention_persistence_counter",
                      player.AttentionPersistenceCounter},
                     {"fairy_revive_grace_timer",
                      player.FairyReviveGraceTimer},
                     {"shape_yaw", player.ShapeYaw},
                     {"background_check_flags", player.BackgroundCheckFlags},
                     {"action_function", player.ActionFunction},
                     {"state_flags_1", player.StateFlags1},
                     {"state_flags_2", player.StateFlags2},
                     {"animation_resource", player.AnimationResource},
                     {"animation_frame", player.AnimationFrame},
                     {"animation_play_speed", player.AnimationPlaySpeed},
                     {"animation_start_frame", player.AnimationStartFrame},
                     {"animation_end_frame", player.AnimationEndFrame},
                     {"animation_mode", player.AnimationMode},
                     {"time_state_update_rate", player.TimeStateUpdateRate},
                 };
               }
               return nlohmann::json{
                   {"source", "code_bin_player_update_layout"},
                   {"update_entries_observed", stats.UpdateEntriesObserved},
                   {"snapshots_captured", stats.SnapshotsCaptured},
                   {"read_failures", stats.ReadFailures},
                   {"last_error", playerTimingProbe.LastError()},
                   {"state", std::move(state)},
                   {"events_truncated",
                    widescreenProjection.PlayerTimingEventsTruncated},
                   {"events",
                    std::move(widescreenProjection.PlayerTimingEvents)},
               };
             }()},
            {"native_scene_view",
             [&]() {
               const auto &stats = sceneViewProbe.Stats();
               nlohmann::json state = nullptr;
               if (sceneViewSnapshot.has_value()) {
                 const auto &view = *sceneViewSnapshot;
                 state = {
                     {"serial", view.Serial},
                     {"play_state_address", view.PlayStateAddress},
                     {"guest_function", view.GuestFunction},
                     {"guest_return_address", view.GuestReturnAddress},
                     {"frustum",
                      {view.Left, view.Right, view.Bottom, view.Top,
                       view.NearPlane, view.FarPlane}},
                     {"eye", view.Eye},
                     {"at", view.At},
                     {"camera_available", view.CameraAvailable},
                 };
               }
               return nlohmann::json{
                   {"source",
                    "code_bin_mtx4x4_frustum_and_playstate_view"},
                   {"perspective_observations",
                    stats.PerspectiveObservations},
                   {"invalid_perspective_observations",
                    stats.InvalidPerspectiveObservations},
                   {"snapshots_captured", stats.SnapshotsCaptured},
                   {"read_failures", stats.ReadFailures},
                   {"published", publishedSceneViewCount},
                   {"last_error", sceneViewProbe.LastError()},
                   {"state", std::move(state)},
               };
             }()},
            {"hid_input",
             [&]() {
               const auto hid = hostServices.HidRuntimeProfile();
               const auto controls =
                   controlConfigRuntime->Snapshot().Config;
               return nlohmann::json{
                   {"controls_config",
                    controlConfigRuntime->Path().generic_string()},
                   {"controls_profile",
                    Oot3dNativeGame::NativeControlProfileName(
                        controls.Profile)},
                   {"native_aim_source",
                    Oot3dNativeGame::NativeMotionSourceName(
                        controls.NativeAimSource)},
                   {"free_camera_source",
                    Oot3dNativeGame::NativeMotionSourceName(
                        controls.FreeCameraSource)},
                   {"gameplay_mouse_owned",
                    nativeControlPollingState.GameplayMouseOwned},
                   {"mouse_polling", {
                       {"eligible", nativeControlPollingState.MouseEligiblePolls},
                       {"released", nativeControlPollingState.MouseReleasedPolls},
                       {"host_ui", nativeControlPollingState.MouseHostUiPolls},
                       {"native_ui", nativeControlPollingState.MouseNativeUiPolls},
                       {"capture_transitions", nativeControlPollingState.MouseCaptureTransitions},
                       {"movement", nativeControlPollingState.MouseMovementPolls}}},
                   {"free_camera_input",
                    {{"pending_x",
                      nativeCandidateDispatch.TopScreenCameraInput.Peek().X},
                     {"pending_y",
                      nativeCandidateDispatch.TopScreenCameraInput.Peek().Y},
                     {"relative_samples_observed",
                      nativeCandidateDispatch.TopScreenCameraInput
                          .RelativeSamplesObserved()},
                     {"relative_samples_consumed",
                      nativeCandidateDispatch.TopScreenCameraInput
                          .RelativeSamplesConsumed()}}},
                   {"timeline_enabled", inputTimeline.Enabled()},
                   {"timeline_path",
                    inputTimeline.SourcePath().generic_string()},
                   {"timeline_segments", inputTimeline.SegmentCount()},
                   {"timeline_frame_origin",
                    Oot3dNativeGame::NativeA32InputTimelineFrameOriginName(
                        inputTimeline.FrameOrigin())},
                   {"sampled_host_frames", inputDiagnostics.SampledFrameCount},
                   {"scripted_host_frames",
                    inputDiagnostics.ScriptedFrameCount},
                   {"active_segment_host_frames",
                    inputDiagnostics.ActiveSegmentFrameCount},
                   {"button_host_frames", inputDiagnostics.ButtonFrameCount},
                   {"circle_pad_host_frames",
                    inputDiagnostics.CirclePadFrameCount},
                   {"touch_host_frames", inputDiagnostics.TouchFrameCount},
                   {"start_edges_observed",
                    nativeCandidateDispatch.StartButtonLatch.ObservedPresses},
                   {"start_short_presses_recovered",
                    nativeCandidateDispatch.StartButtonLatch
                        .RecoveredShortPresses},
                   {"start_edge_pending",
                    nativeCandidateDispatch.StartButtonLatch.PendingPressed},
                   {"shared_memory_mapped", hid.SharedMemoryMapped},
                   {"samples_written", hid.SamplesWritten},
                   {"accelerometer_samples_written",
                    hid.AccelerometerSamplesWritten},
                   {"gyroscope_samples_written",
                    hid.GyroscopeSamplesWritten},
                   {"event_batches_signaled", hid.EventBatchesSignaled},
                   {"last_sample_tick", hid.LastSampleTick},
                   {"last_buttons", hid.LastButtons},
                   {"last_additions", hid.LastAdditions},
                   {"last_removals", hid.LastRemovals},
                   {"last_circle_pad_x", hid.LastCirclePadX},
                   {"last_circle_pad_y", hid.LastCirclePadY},
                   {"next_pad_index", hid.NextPadIndex},
                   {"last_accelerometer", hid.LastAccelerometer},
                   {"last_gyroscope", hid.LastGyroscope},
                   {"next_accelerometer_index",
                    hid.NextAccelerometerIndex},
                   {"next_gyroscope_index",
                    hid.NextGyroscopeIndex},
                   {"native_consumer",
                    {{"samples", inputConsumerDiagnostics.Samples},
                     {"read_failures", inputConsumerDiagnostics.ReadFailures},
                     {"held_frames", inputConsumerDiagnostics.HeldFrames},
                     {"pressed_frames", inputConsumerDiagnostics.PressedFrames},
                     {"released_frames",
                      inputConsumerDiagnostics.ReleasedFrames},
                     {"last_held", inputConsumerDiagnostics.LastHeld},
                     {"last_pressed", inputConsumerDiagnostics.LastPressed},
                     {"last_released", inputConsumerDiagnostics.LastReleased},
                     {"last_raw_current",
                      inputConsumerDiagnostics.LastRawCurrent},
                     {"last_raw_previous",
                      inputConsumerDiagnostics.LastRawPrevious},
                     {"transitions",
                      std::move(inputConsumerDiagnostics.Transitions)}}},
               };
             }()},
            {"dsp_audio",
             {{"enabled", !launch.DisableAudio},
              {"host_output_enabled", !launch.DisableAudio},
              {"guest_dsp_processing_enabled", true},
              {"frames_mixed", dspFramesMixed},
              {"frames_processed", dspFramesMixed},
              {"frames_suppressed", dspFramesSuppressed},
              {"stereo_samples_mixed", dspSamplesMixed},
              {"nonzero_channel_samples", dspNonzeroSamples},
              {"peak_magnitude", dspPeakMagnitude},
              {"pcm_fnv1a64", dspPcmFnv1a64},
              {"pcm_continuity", pcmContinuityDiagnostics.ToJson()},
              {"pcm_dump",
               {{"enabled", !launch.AudioPcmDumpPath.empty()},
                {"path", launch.AudioPcmDumpPath.string()},
                {"channel_samples", pcmDumpSamples.size()}}},
              {"output_initialized",
               audioPlayer != nullptr && audioPlayer->IsInitialized()},
              {"output_sample_rate",
               audioPlayer != nullptr ? audioPlayer->GetSampleRate() : 0},
              {"output_frame_samples",
               audioPlayer != nullptr ? audioPlayer->GetSampleLength() : 0},
              {"output_queue",
               {{"observed", audioOutputDiagnostics.HasBufferObservation},
                {"primed", audioOutputDiagnostics.Primed},
                {"submit_calls", audioOutputDiagnostics.SubmitCalls},
                {"requested_frames", audioOutputDiagnostics.RequestedFrames},
                {"maximum_submit_gap_seconds",
                 audioOutputDiagnostics.MaximumSubmitGapSeconds},
                {"submit_gap_events",
                 std::move(audioOutputDiagnostics.SubmitGapEvents)},
                {"short_growth_submissions",
                 audioOutputDiagnostics.ShortGrowthSubmissions},
                {"estimated_shortfall_frames",
                 audioOutputDiagnostics.EstimatedShortfallFrames},
                {"starvation_observations",
                 audioOutputDiagnostics.StarvationObservations},
                {"starvation_events",
                 std::move(audioOutputDiagnostics.StarvationEvents)},
                {"minimum_buffered_before",
                 audioOutputDiagnostics.MinimumBufferedBefore},
                {"maximum_buffered_before",
                 audioOutputDiagnostics.MaximumBufferedBefore},
                {"minimum_buffered_after",
                 audioOutputDiagnostics.MinimumBufferedAfter},
                {"maximum_buffered_after",
                 audioOutputDiagnostics.MaximumBufferedAfter},
                {"desired_buffered", audioPlayer != nullptr
                                         ? audioPlayer->GetDesiredBuffered()
                                         : 0}}}}},
            {"draws_submitted", drawCount},
            {"refreshes_with_native_draws", refreshesWithNativeDraws},
            {"display_transfers_submitted", displayTransferCount},
            {"gpu_completions_queued", 0},
            {"pica_completions_recorded", recordedCompletionCount},
            {"p3d_completions_delivered", completedP3dCount},
            {"ppf_completions_delivered", completedPpfCount},
            {"pending_gpu_completions", 0},
            {"pica_drain_passes", picaDrainPassCount},
            {"maximum_pica_drain_passes_per_refresh",
             maximumPicaDrainPassesPerRefresh},
            {"pending_draws", submissionQueue.PendingDraws().size()},
            {"pending_completion_markers",
             submissionQueue.PendingCompletions().size()},
            {"pending_display_transfers",
             submissionQueue.PendingDisplayTransfers().size()},
            {"process_run_kind", static_cast<uint32_t>(processResult.Kind)},
            {"ui_profile",
             Oot3dNativeGame::Oot3dUiProfileName(launch.UiProfile)},
            {"graphics_settings",
             {{"configuration_path", launch.Host.ConfigurationPath},
              {"preset", static_cast<std::uint32_t>(
                             graphicsSettings.Preset)},
              {"frame_rate_mode",
               static_cast<std::uint32_t>(
                   graphicsSettings.FrameRate)},
              {"fov_multiplier", graphicsSettings.FovMultiplier},
              {"toon_mode",
               static_cast<std::uint32_t>(
                   graphicsSettings.Effects.Toon)},
              {"directional_shadows",
               {{"enabled",
                 directionalShadows.Mode ==
                     Fast::Oot3d::DirectionalShadowMode::
                         VirtualShadowMaps},
                {"mode",
                 static_cast<std::uint32_t>(
                     directionalShadows.Mode)},
                {"resolution", directionalShadows.Resolution},
                {"maximum_distance",
                 directionalShadows.MaximumDistance},
                {"depth_bias_constant",
                 directionalShadows.DepthBiasConstant},
                {"depth_bias_slope",
                 directionalShadows.DepthBiasSlope},
                {"strength", directionalShadows.Strength},
                {"pcf_radius", directionalShadows.PcfRadius}}}}},
            {"topscreen_config",
             launch.TopScreenConfigPath.empty()
                 ? nlohmann::json(nullptr)
                 : nlohmann::json(launch.TopScreenConfigPath.string())},
            {"topscreen_settings",
             {{"source",
               launch.TopScreenConfigPath.empty()
                   ? nlohmann::json("built_in_defaults")
                   : nlohmann::json("external_json")},
              {"hud_layout",
               Oot3dNativeGame::TopScreenHudLayoutName(
                   activeTopScreenConfig.HudLayout)},
              {"hud_scale",
               std::round(static_cast<double>(
                              activeTopScreenConfig.HudScale) *
                          100.0) /
                   100.0},
              {"hud_margin_x", activeTopScreenConfig.HudMarginX},
              {"hud_margin_y", activeTopScreenConfig.HudMarginY},
              {"magic_bar_y", activeTopScreenConfig.MagicBarY},
              {"minimap_visible", activeTopScreenConfig.MinimapVisible},
              {"render_hud", activeTopScreenConfig.RenderHud},
              {"render_dpad_icons",
               activeTopScreenConfig.RenderDpadIcons},
              {"render_items_hint",
               activeTopScreenConfig.RenderItemsHint},
              {"select_action",
               Oot3dNativeGame::TopScreenSelectActionName(
                   activeTopScreenConfig.SelectAction)},
              {"exit_items_to_save_screen",
               activeTopScreenConfig.ExitItemsToSaveScreen},
              {"camera_zoom_percent",
               activeTopScreenConfig.CameraZoomPercent},
              {"camera_fov_percent",
               activeTopScreenConfig.CameraFovPercent},
              {"dpad_child",
               nlohmann::json::array(
                   {Oot3dNativeGame::TopScreenDpadActionName(
                        activeTopScreenConfig.ChildDpad[0]),
                    Oot3dNativeGame::TopScreenDpadActionName(
                        activeTopScreenConfig.ChildDpad[1]),
                    Oot3dNativeGame::TopScreenDpadActionName(
                        activeTopScreenConfig.ChildDpad[2]),
                    Oot3dNativeGame::TopScreenDpadActionName(
                        activeTopScreenConfig.ChildDpad[3])})},
              {"dpad_adult",
               nlohmann::json::array(
                   {Oot3dNativeGame::TopScreenDpadActionName(
                        activeTopScreenConfig.AdultDpad[0]),
                    Oot3dNativeGame::TopScreenDpadActionName(
                        activeTopScreenConfig.AdultDpad[1]),
                    Oot3dNativeGame::TopScreenDpadActionName(
                        activeTopScreenConfig.AdultDpad[2]),
                    Oot3dNativeGame::TopScreenDpadActionName(
                        activeTopScreenConfig.AdultDpad[3])})},
              {"free_camera_enabled",
               activeTopScreenConfig.FreeCameraEnabled},
              {"free_camera_speed_level",
               activeTopScreenConfig.FreeCameraSpeedLevel},
              {"free_camera_smoothing",
               Oot3dNativeGame::TopScreenFreeCameraSmoothingName(
                   activeTopScreenConfig.FreeCameraSmoothing)},
              {"free_camera_invert_x",
               activeTopScreenConfig.FreeCameraInvertX},
              {"free_camera_invert_y",
               activeTopScreenConfig.FreeCameraInvertY},
              {"c_stick_aim_speed_level",
               activeTopScreenConfig.CStickAimSpeedLevel},
              {"c_stick_aim_invert_x",
               activeTopScreenConfig.CStickAimInvertX},
              {"c_stick_aim_invert_y",
               activeTopScreenConfig.CStickAimInvertY},
              {"gameplay_actions",
               {{"attempts",
                 nativeCandidateDispatch.TopScreenGameplayActions.Attempts},
                {"eligible",
                 nativeCandidateDispatch.TopScreenGameplayActions.Eligible},
                {"equipment_changes",
                 nativeCandidateDispatch.TopScreenGameplayActions
                     .EquipmentChanges},
                {"player_refreshes",
                 nativeCandidateDispatch.TopScreenGameplayActions
                     .PlayerRefreshes},
                {"direct_item_assignments",
                 nativeCandidateDispatch.TopScreenGameplayActions
                     .DirectItemAssignments},
                {"direct_item_triggers",
                 nativeCandidateDispatch.TopScreenGameplayActions
                     .DirectItemTriggers},
                {"direct_item_clears",
                 nativeCandidateDispatch.TopScreenGameplayActions
                     .DirectItemClears}}},
              {"guest_save_preferences_used", false},
              {"gameplay_preference_chords_enabled", false}}},
            {"process_state_fingerprint", process.StateFingerprint()},
            {"memory_state_fingerprint", process.Memory().StateFingerprint()},
            {"memory_content_fingerprint",
             process.Memory().ContentFingerprint()},
            {"system_ticks", hostServices.SystemTicks()},
            {"lcd_force_black", hostServices.LcdForceBlack()},
            {"top_framebuffer", std::move(topFramebuffer)},
            {"bottom_framebuffer", std::move(bottomFramebuffer)},
            {"display_transfer_events", std::move(displayTransferEvents)},
            {"draw_events", std::move(drawEvents)},
            {"recent_svc_events", std::move(recentSvcEvents)},
            {"filesystem_svc_events", std::move(filesystemSvcEvents)},
            {"ipc_svc_events", std::move(ipcSvcEvents)},
            {"last_svc_by_thread", std::move(lastThreadSvcEvents)},
            {"threads", std::move(threads)},
            {"screenshot_written", screenshotState.Written},
            {"visual_interpolation",
             {{"snapshot_replay_enabled", visualInterpolationEnabled},
              {"cross_frame_history_enabled", visualInterpolationEnabled},
              {"direct_current_frames_presented",
               visualDirectFramesPresented},
              {"reused_snapshot_presentations",
               visualReusedSnapshotPresentations},
              {"selected_top_transfers", selectedTopTransferCount},
              {"complete_snapshots", visualSnapshotCount},
              {"transitions", visualTransitionCount},
              {"matched_draws", visualMatchedDrawCount},
              {"strict_unique_matched_draws",
               visualStrictUniqueMatchedDrawCount},
              {"strict_ordinal_matched_draws",
               visualStrictOrdinalMatchedDrawCount},
              {"structural_ordinal_matched_draws",
               visualStructuralOrdinalMatchedDrawCount},
              {"pipeline_ordinal_matched_draws",
               visualPipelineOrdinalMatchedDrawCount},
              {"changed_continuous_draws", visualChangedContinuousDrawCount},
              {"changed_per_instance_vertex_draws",
               visualChangedPerInstanceVertexDrawCount},
              {"changed_per_vertex_draws", visualChangedPerVertexDrawCount},
              {"ambiguous_previous_draws", visualAmbiguousPreviousDrawCount},
              {"ambiguous_current_draws", visualAmbiguousCurrentDrawCount},
              {"unmatched_previous_draws", visualUnmatchedPreviousDrawCount},
              {"unmatched_current_draws", visualUnmatchedCurrentDrawCount},
              {"samples_presented", visualSamplesPresented},
              {"discontinuities", visualDiscontinuityCount},
              {"namespace_resynchronizations", visualNamespaceResyncCount},
              {"sample_draws", visualSampleDrawCount},
              {"interpolated_draws", visualInterpolatedDrawCount},
              {"interpolated_per_instance_vertex_draws",
               visualInterpolatedPerInstanceVertexDrawCount},
              {"interpolated_per_vertex_draws",
               visualInterpolatedPerVertexDrawCount},
              {"matching_seconds",
               visualInterpolationTiming.MatchingNanoseconds * 1.0e-9},
              {"analysis_seconds",
               visualInterpolationTiming.AnalysisNanoseconds * 1.0e-9},
              {"sampling_seconds",
               visualInterpolationTiming.SamplingNanoseconds * 1.0e-9},
              {"pending_draws",
               picaPresentationScheduler.PendingDrawCount()},
              {"execution_scheduler",
               [&]() {
                 const auto &stats = picaPresentationScheduler.Stats();
                 const auto &timings = picaPresentationScheduler.Timings();
                 const double measuredSeconds =
                     timings.ValidationSeconds +
                     timings.CompositionPublicationSeconds +
                     timings.DrawPreparationSeconds +
                     timings.DrawSubmissionSeconds +
                     timings.MemoryFillSeconds +
                     timings.SnapshotTransferSeconds +
                     timings.PresentTransferSeconds;
                 return nlohmann::json{
                     {"produced_guest_draws", stats.ProducedGuestDraws},
                     {"captured_memory_fills", stats.CapturedMemoryFills},
                     {"captured_display_transfers",
                      stats.CapturedDisplayTransfers},
                     {"completed_visual_frames",
                      stats.CompletedVisualFrames},
                     {"superseded_visual_frames",
                      stats.SupersededVisualFrames},
                     {"selected_draws", stats.SelectedDraws},
                     {"executed_draws", stats.ExecutedDraws},
                     {"prepared_draws", stats.PreparedDraws},
                     {"interpolated_draws", stats.InterpolatedDraws},
                     {"execution_lists", stats.ExecutionLists},
                     {"current_frame_lists", stats.CurrentFrameLists},
                     {"native_frame_lists", stats.NativeFrameLists},
                     {"interpolated_frame_lists",
                      stats.InterpolatedFrameLists},
                     {"repeated_frame_lists", stats.RepeatedFrameLists},
                     {"dependency_flushes", stats.DependencyFlushes},
                     {"memory_fills_executed",
                      stats.MemoryFillsExecuted},
                     {"display_snapshots_executed",
                      stats.DisplaySnapshotsExecuted},
                     {"presented_transfers", stats.PresentedTransfers},
                     {"reused_snapshots", stats.ReusedSnapshots},
                     {"duplicate_draw_attempts",
                      stats.DuplicateDrawAttempts},
                     {"redundant_top_snapshot_copies_avoided",
                      stats.RedundantTopSnapshotCopiesAvoided},
                     {"composition_sequences_published",
                      stats.CompositionSequencesPublished},
                     {"composition_draw_references_published",
                      stats.CompositionDrawReferencesPublished},
                     {"composition_publication_failures",
                      stats.CompositionPublicationFailures},
                     {"resets", stats.Resets},
                     {"timing_enabled", timings.Enabled},
                     {"execute_seconds", timings.ExecuteSeconds},
                     {"validation_seconds", timings.ValidationSeconds},
                     {"composition_publication_seconds",
                      timings.CompositionPublicationSeconds},
                     {"draw_preparation_seconds",
                      timings.DrawPreparationSeconds},
                     {"draw_submission_seconds",
                      timings.DrawSubmissionSeconds},
                     {"draw_uniform_packing_seconds",
                      timings.DrawUniformPackingSeconds},
                     {"draw_view_construction_seconds",
                      timings.DrawViewConstructionSeconds},
                     {"draw_backend_submission_seconds",
                      timings.DrawBackendSubmissionSeconds},
                     {"memory_fill_seconds", timings.MemoryFillSeconds},
                     {"snapshot_transfer_seconds",
                      timings.SnapshotTransferSeconds},
                     {"present_transfer_seconds",
                      timings.PresentTransferSeconds},
                     {"present_existing_seconds",
                      timings.PresentExistingSeconds},
                     {"execute_overhead_seconds",
                      std::max(0.0,
                               timings.ExecuteSeconds - measuredSeconds)},
                 };
               }()},
              {"recent_transitions", std::move(visualTransitionEvents)}}},
            {"enmag_trace",
             {{"init_calls", widescreenProjection.EnMagInitCalls},
              {"update_calls", widescreenProjection.EnMagUpdateCalls},
              {"draw_calls", widescreenProjection.EnMagDrawCalls},
              {"events", std::move(widescreenProjection.EnMagEvents)}}},
            {"ui_lifecycle_trace",
             {{"call_counts", widescreenProjection.UiLifecycleCallCounts},
              {"events", std::move(widescreenProjection.UiLifecycleEvents)}}},
            {"game_state_trace",
             {{"update_calls", widescreenProjection.GameStateUpdateCalls},
              {"events", std::move(widescreenProjection.GameStateEvents)}}},
            {"n64_integrated_ui",
             [&]() {
               const auto &bridge = uiLifecycleBridge.Stats();
               const auto &ui = uiLifecycleBridge.Runtime().Stats();
               const auto &renderer = n64UiRenderer.Stats();
               const auto &nativeTextures = nativeUiTextureProvider.Stats();
               nlohmann::json subsystemCalls = nlohmann::json::object();
               for (std::size_t index = 0;
                    index < bridge.calls_by_subsystem.size(); ++index) {
                 const auto subsystem =
                     static_cast<oot3d::ui::UiSubsystem>(index);
                 subsystemCalls[oot3d::ui::UiSubsystemName(subsystem)] =
                     bridge.calls_by_subsystem[index];
               }
               nlohmann::json phaseCalls = nlohmann::json::object();
               for (std::size_t index = 0; index < bridge.calls_by_phase.size();
                    ++index) {
                 const auto phase = static_cast<oot3d::ui::UiSeamPhase>(index);
                 phaseCalls[oot3d::ui::UiSeamPhaseName(phase)] =
                     bridge.calls_by_phase[index];
               }
               return nlohmann::json{
                   {"selected_profile",
                    Oot3dNativeGame::Oot3dUiProfileName(launch.UiProfile)},
                   {"topscreen_profile_active",
                    nativeCandidateDispatch.TopScreenUiProfile},
                   {"topscreen_gameplay_composition_observed",
                    nativeCandidateDispatch.TopScreenLowerCompositionSkips !=
                        0U},
                   {"backend_profile",
                    uiLifecycleBridge.Runtime().Profile().id},
                   {"matched_entries", bridge.matched_entries},
                   {"topscreen_quest_hook_calls", bridge.topscreen_quest_hook_calls},
                   {"topscreen_quest_transforms", bridge.topscreen_quest_transforms},
                   {"topscreen_quest_hook_failures", bridge.topscreen_quest_hook_failures},
                   {"guest_routed_entries", bridge.guest_routed_entries},
                   {"host_routed_entries", bridge.host_routed_entries},
                   {"state_captures", bridge.state_captures},
                   {"fields_read", bridge.fields_read},
                   {"fields_missing", bridge.fields_missing},
                   {"last_guest_entry", bridge.last_guest_entry},
                   {"calls_by_subsystem", std::move(subsystemCalls)},
                   {"calls_by_phase", std::move(phaseCalls)},
                   {"observed_states", ui.observed_states},
                   {"known_file_select_states", ui.known_file_select_states},
                   {"active_file_select_states", ui.active_file_select_states},
                   {"selected_slot_known", ui.selected_slot_known},
                   {"selected_slot", ui.selected_slot},
                   {"controller_state_known", ui.controller_state_known},
                   {"controller_state", ui.controller_state},
                   {"known_name_entry_states", ui.known_name_entry_states},
                   {"active_name_entry_states", ui.active_name_entry_states},
                   {"name_entry_controller_state_known",
                    ui.name_entry_controller_state_known},
                   {"name_entry_controller_state",
                    ui.name_entry_controller_state},
                   {"name_entry_keyboard_page_known",
                    ui.name_entry_keyboard_page_known},
                   {"name_entry_keyboard_page", ui.name_entry_keyboard_page},
                   {"name_entry_cursor_known", ui.name_entry_cursor_known},
                   {"name_entry_cursor_column", ui.name_entry_cursor_column},
                   {"name_entry_cursor_row", ui.name_entry_cursor_row},
                   {"name_entry_length_known", ui.name_entry_length_known},
                   {"name_entry_length", ui.name_entry_length},
                   {"name_entry_action_choice_known",
                    ui.name_entry_action_choice_known},
                   {"name_entry_action_choice", ui.name_entry_action_choice},
                   {"name_entry_confirmation_choice_known",
                    ui.name_entry_confirmation_choice_known},
                   {"name_entry_confirmation_choice",
                    ui.name_entry_confirmation_choice},
                   {"name_entry_save_commit_timer_known",
                    ui.name_entry_save_commit_timer_known},
                   {"name_entry_save_commit_timer",
                    ui.name_entry_save_commit_timer},
                   {"shadow_presentation_frames",
                    bridge.shadow_presentation_frames},
                   {"topscreen_ocarina_frames", bridge.topscreen_ocarina_frames},
                   {"topscreen_ocarina_text_builds", ocarinaText.Builds()},
                   {"topscreen_ocarina_text_releases", ocarinaText.Releases()},
                   {"topscreen_ocarina_primitives", bridge.topscreen_ocarina_primitives},
                   {"topscreen_ocarina_failures", bridge.topscreen_ocarina_failures},
                   {"topscreen_ocarina_error", bridge.topscreen_ocarina_error},
                   {"shadow_presentation_primitives",
                    bridge.shadow_presentation_primitives},
                   {"topscreen_native_touch_copy_frames",
                    bridge.topscreen_native_touch_copy_frames},
                   {"topscreen_native_touch_copy_primitives",
                    bridge.topscreen_native_touch_copy_primitives},
                   {"topscreen_native_touch_visible_primitives",
                    bridge.topscreen_native_touch_visible_primitives},
                   {"topscreen_horse_stamina_visible_frames",
                    bridge.topscreen_horse_stamina_visible_frames},
                   {"topscreen_horse_stamina_visible_primitives",
                    bridge.topscreen_horse_stamina_visible_primitives},
                   {"native_bottom_frontend_presentation_frames",
                    nativeBottomFrontendPresentationCount},
                   {"native_bottom_overlay_frames", 0U},
                   {"native_menu_backdrop_draws_omitted",
                    nativeMenuBackdropDrawOmissionCount},
                   {"native_menu_clear_draws_omitted",
                    nativeMenuClearDrawOmissionCount},
                   {"native_frontend_composition_draws", 0U},
                   {"native_frontend_presentation_active_frames",
                    nativeFrontendPresentationActiveCount},
                   {"native_frontend_bottom_transfer_hits",
                    nativeFrontendBottomTransferHitCount},
                   {"native_frontend_composed_output_hits", 0U},
                   {"renderer_frames", renderer.frames},
                   {"renderer_primitives_drawn", renderer.primitives_drawn},
                   {"renderer_texture_uploads", renderer.texture_uploads},
                   {"renderer_texture_cache_hits", renderer.texture_cache_hits},
                   {"renderer_native_texture_resolves",
                    renderer.native_texture_resolves},
                   {"renderer_native_texture_failures",
                    renderer.native_texture_failures},
                   {"native_texture_provider_resolves",
                    nativeTextures.resolve_calls},
                   {"native_texture_provider_physical_reads",
                    nativeTextures.physical_reads},
                   {"native_texture_provider_translated_guest_reads",
                    nativeTextures.translated_guest_reads},
                   {"native_texture_provider_bytes_read",
                    nativeTextures.bytes_read},
                   {"native_texture_provider_decode_failures",
                    nativeTextures.decode_failures},
                   {"native_texture_provider_override_checks",
                    nativeTextures.override_checks},
                   {"native_texture_provider_overrides_applied",
                    nativeTextures.overrides_applied},
                   {"native_texture_provider_overrides_already_applied",
                    nativeTextures.overrides_already_applied},
                   {"native_texture_provider_override_no_matches",
                    nativeTextures.override_no_matches},
                   {"topscreen_texture_override_pack_loaded",
                    topScreenTextureOverridePackPointer != nullptr},
                   {"hd_font_pack_loaded", hdFontsLoaded},
                   {"hd_font_native_blits", hdFonts.Blits},
                   {"hd_font_atlas_rebuilds", hdFonts.Rebuilds},
                   {"hd_font_texture_replacements", hdFonts.Replacements},
                   {"topscreen_texture_override_pica",
                    [&]() {
                      if (!topScreenTextureOverrideRuntime.has_value()) {
                        return nlohmann::json(nullptr);
                      }
                      const auto &overrides =
                          topScreenTextureOverrideRuntime->Stats();
                      nlohmann::json targets = nlohmann::json::array();
                      for (const auto &target :
                           topScreenTextureOverrideRuntime->TargetStats()) {
                        targets.push_back({
                            {"physical_address", target.physical_address},
                            {"guest_surface_address",
                             target.guest_surface_address},
                            {"byte_count", target.byte_count},
                            {"semantic_names", target.semantic_names},
                            {"last_width", target.last_width},
                            {"last_height", target.last_height},
                            {"last_format", target.last_format},
                            {"last_payload_hash", target.last_payload_hash},
                            {"payload_checks", target.payload_checks},
                            {"applied", target.applied},
                            {"already_applied", target.already_applied},
                            {"no_match", target.no_match},
                        });
                      }
                      return nlohmann::json{
                          {"observe_calls", overrides.observe_calls},
                          {"identities_observed",
                           overrides.identities_observed},
                          {"target_bindings", overrides.target_bindings},
                          {"payload_checks", overrides.payload_checks},
                          {"applied", overrides.applied},
                          {"already_applied", overrides.already_applied},
                          {"no_match", overrides.no_match},
                          {"targets", std::move(targets)},
                      };
                    }()},
               };
             }()},
            {"renderer_queue_events",
             std::move(widescreenProjection.RendererQueueEvents)},
            {"renderer_queue_call_counts",
             widescreenProjection.RendererQueueCallCounts},
            {"renderer_command_queue_state",
             {{"submission", readQueueState(kRendererCommandQueue)},
              {"completion", readQueueState(kRendererCommandQueue + 0xB4U)},
              {"command_words", readRendererCommandState()}}},
            {"widescreen_projection",
             {{"native_aspect",
               widescreenProjection.Projection.Presentation.NativeAspect},
              {"output_aspect",
               widescreenProjection.Projection.Presentation.OutputAspect},
              {"aspect_extension",
               static_cast<std::uint32_t>(
                   widescreenProjection.Projection.Presentation.Extension)},
              {"horizontal_expansion",
               widescreenProjection.Projection.Presentation
                   .HorizontalFovExpansion},
              {"vertical_expansion",
               widescreenProjection.Projection.Presentation
                   .VerticalFovExpansion},
              {"fov_multiplier",
               widescreenProjection.Projection.FovMultiplier},
              {"frustum_calls",
               widescreenProjection.Projection.FrustumCalls},
              {"orthographic_calls",
               widescreenProjection.Projection.OrthographicCalls},
              {"callsites", std::move(projectionCallsites)},
              {"topscreen_pause_projection",
               {{"calls", widescreenProjection.TopScreenPauseProjectionCalls},
                {"failures",
                 widescreenProjection.TopScreenPauseProjectionFailures},
                {"events", std::move(widescreenProjection
                                         .TopScreenPauseProjectionEvents)}}}}},
            {"a32_block_profile",
             {{"enabled", launch.ProfileA32Blocks},
              {"sample_rate_denominator", 64},
              {"block_entries", widescreenProjection.BlockEntries},
              {"total_samples", totalBlockSampleCount},
              {"reported_samples", blockSampleCount},
              {"blocks", std::move(sampledBlockEntries)}}},
            {"compiled_functions",
              [&]() {
                const auto stats =
                    Oot3dNativeGame::GetOot3dCompiledFunctionStats();
                nlohmann::json externalTargets = nlohmann::json::array();
                for (const auto& target :
                     Oot3dNativeGame::GetOot3dWholeAotExternalTargets()) {
                  externalTargets.push_back({
                      {"entry", target.Entry},
                      {"calls", target.Calls},
                      {"manual_compiled_calls", target.ManualCompiledCalls},
                      {"timing_samples", target.TimingSamples},
                      {"estimated_seconds",
                       target.TimingSampleNanoseconds * 64.0e-9},
                  });
                }
                const auto typedGameplayStats =
                    Oot3dNativeGame::GetOot3dTypedGameplayStats();
               const auto massAotStats =
                   Oot3dNativeGame::GetOot3dMassAotStats();
               return nlohmann::json{
                   {"enabled", !launch.DisableCompiledFunctions},
                   {"typed_gameplay_entry_points",
                    Oot3dNativeGame::Oot3dTypedGameplayEntryPoints().size()},
                   {"typed_gameplay_calls", typedGameplayStats.Calls},
                   {"typed_gameplay_angle_calls",
                    typedGameplayStats.AngleCalls},
                   {"typed_gameplay_actor_calls",
                    typedGameplayStats.ActorCalls},
                   {"typed_gameplay_enko_blink_block_calls",
                    typedGameplayStats.EnKoBlinkBlockCalls},
                   {"typed_gameplay_enko_blink_logical_advances",
                    typedGameplayStats.EnKoBlinkLogicalAdvances},
                   {"typed_gameplay_enko_blink_intermediate_holds",
                    typedGameplayStats.EnKoBlinkIntermediateHolds},
                   {"typed_gameplay_enko_blink_timer_advances",
                    typedGameplayStats.EnKoBlinkTimerAdvances},
                   {"typed_gameplay_enko_blink_sequence_advances",
                    typedGameplayStats.EnKoBlinkSequenceAdvances},
                   {"typed_gameplay_enko_blink_rng_dispatches",
                    typedGameplayStats.EnKoBlinkRngDispatches},
                   {"typed_gameplay_enko_blink_rng_returns",
                    typedGameplayStats.EnKoBlinkRngReturns},
                   {"typed_gameplay_enkanban_phase_block_calls",
                    typedGameplayStats.EnKanbanPhaseBlockCalls},
                   {"typed_gameplay_enkanban_phase_logical_advances",
                    typedGameplayStats.EnKanbanPhaseLogicalAdvances},
                   {"typed_gameplay_enkanban_phase_intermediate_holds",
                    typedGameplayStats.EnKanbanPhaseIntermediateHolds},
                   {"typed_gameplay_enkanban_state0_countdown_block_calls",
                    typedGameplayStats.EnKanbanState0CountdownBlockCalls},
                   {"typed_gameplay_enkanban_state0_countdown_logical_advances",
                    typedGameplayStats
                        .EnKanbanState0CountdownLogicalAdvances},
                   {"typed_gameplay_enkanban_state0_countdown_intermediate_holds",
                    typedGameplayStats
                        .EnKanbanState0CountdownIntermediateHolds},
                   {"typed_gameplay_enkanban_actor_flag_countdown_block_calls",
                    typedGameplayStats
                        .EnKanbanActorFlagCountdownBlockCalls},
                   {"typed_gameplay_enkanban_actor_flag_countdown_logical_advances",
                    typedGameplayStats
                        .EnKanbanActorFlagCountdownLogicalAdvances},
                   {"typed_gameplay_enkanban_actor_flag_countdown_intermediate_holds",
                    typedGameplayStats
                        .EnKanbanActorFlagCountdownIntermediateHolds},
                   {"typed_gameplay_enkanban_actor_flag_clears",
                    typedGameplayStats.EnKanbanActorFlagClears},
                   {"typed_gameplay_enkanban_interaction_cooldown_block_calls",
                    typedGameplayStats
                        .EnKanbanInteractionCooldownBlockCalls},
                   {"typed_gameplay_enkanban_interaction_cooldown_logical_advances",
                    typedGameplayStats
                        .EnKanbanInteractionCooldownLogicalAdvances},
                   {"typed_gameplay_enkanban_interaction_cooldown_intermediate_holds",
                    typedGameplayStats
                        .EnKanbanInteractionCooldownIntermediateHolds},
                   {"typed_gameplay_enkanban_interaction_cooldown_ready_passes",
                    typedGameplayStats
                        .EnKanbanInteractionCooldownReadyPasses},
                   {"typed_gameplay_enkanban_interaction_cooldown_blocked_passes",
                    typedGameplayStats
                        .EnKanbanInteractionCooldownBlockedPasses},
                   {"typed_gameplay_enkanban_draw_gate_ramp_block_calls",
                    typedGameplayStats.EnKanbanDrawGateRampBlockCalls},
                   {"typed_gameplay_enkanban_draw_gate_ramp_logical_advances",
                    typedGameplayStats.EnKanbanDrawGateRampLogicalAdvances},
                   {"typed_gameplay_enkanban_draw_gate_ramp_intermediate_holds",
                    typedGameplayStats
                        .EnKanbanDrawGateRampIntermediateHolds},
                   {"typed_gameplay_enkanban_draw_gate_ramp_inactive_passes",
                    typedGameplayStats.EnKanbanDrawGateRampInactivePasses},
                   {"typed_gameplay_enkanban_draw_gate_ramp_increase_steps",
                    typedGameplayStats.EnKanbanDrawGateRampIncreaseSteps},
                   {"typed_gameplay_enkanban_draw_gate_ramp_decrease_steps",
                    typedGameplayStats.EnKanbanDrawGateRampDecreaseSteps},
                   {"typed_gameplay_enkanban_draw_gate_ramp_clamps",
                    typedGameplayStats.EnKanbanDrawGateRampClamps},
                   {"typed_gameplay_enkanban_oscillator_axis_block_calls",
                    typedGameplayStats.EnKanbanOscillatorAxisBlockCalls},
                   {"typed_gameplay_enkanban_oscillator_x_block_calls",
                    typedGameplayStats.EnKanbanOscillatorXBlockCalls},
                   {"typed_gameplay_enkanban_oscillator_y_block_calls",
                    typedGameplayStats.EnKanbanOscillatorYBlockCalls},
                   {"typed_gameplay_enkanban_oscillator_rate_adjusted_steps",
                    typedGameplayStats
                        .EnKanbanOscillatorRateAdjustedSteps},
                   {"typed_gameplay_enkanban_oscillator_ground_resets",
                    typedGameplayStats.EnKanbanOscillatorGroundResets},
                   {"typed_gameplay_enkanban_oscillator_velocity_clamps",
                    typedGameplayStats.EnKanbanOscillatorVelocityClamps},
                   {"typed_gameplay_enkanban_piece_lifetime_block_calls",
                    typedGameplayStats.EnKanbanPieceLifetimeBlockCalls},
                   {"typed_gameplay_enkanban_piece_lifetime_logical_decrements",
                    typedGameplayStats
                        .EnKanbanPieceLifetimeLogicalDecrements},
                   {"typed_gameplay_enkanban_piece_lifetime_intermediate_holds",
                    typedGameplayStats
                        .EnKanbanPieceLifetimeIntermediateHolds},
                   {"typed_gameplay_enkanban_piece_lifetime_state_transitions",
                    typedGameplayStats
                        .EnKanbanPieceLifetimeStateTransitions},
                   {"typed_gameplay_enkanban_piece_lifetime_zero_crossing_transitions",
                    typedGameplayStats
                        .EnKanbanPieceLifetimeZeroCrossingTransitions},
                   {"typed_gameplay_enkanban_piece_lifetime_existing_zero_transitions",
                    typedGameplayStats
                        .EnKanbanPieceLifetimeExistingZeroTransitions},
                   {"typed_gameplay_enkanban_ripple_gate_calls",
                    typedGameplayStats.EnKanbanRippleGateCalls},
                   {"typed_gameplay_enkanban_ripple_dispatches",
                    typedGameplayStats.EnKanbanRippleDispatches},
                   {"typed_gameplay_enkanban_ripple_intermediate_suppressions",
                    typedGameplayStats
                        .EnKanbanRippleIntermediateSuppressions},
                   {"typed_gameplay_actor_update_all_context_freeze_block_calls",
                    typedGameplayStats
                        .ActorUpdateAllContextFreezeBlockCalls},
                   {"typed_gameplay_actor_update_all_context_freeze_logical_advances",
                    typedGameplayStats
                        .ActorUpdateAllContextFreezeLogicalAdvances},
                   {"typed_gameplay_actor_update_all_context_freeze_intermediate_holds",
                    typedGameplayStats
                        .ActorUpdateAllContextFreezeIntermediateHolds},
                   {"typed_gameplay_actor_update_all_instance_freeze_block_calls",
                    typedGameplayStats
                        .ActorUpdateAllInstanceFreezeBlockCalls},
                   {"typed_gameplay_actor_update_all_instance_freeze_logical_advances",
                    typedGameplayStats
                        .ActorUpdateAllInstanceFreezeLogicalAdvances},
                   {"typed_gameplay_actor_update_all_instance_freeze_intermediate_holds",
                    typedGameplayStats
                        .ActorUpdateAllInstanceFreezeIntermediateHolds},
                   {"typed_gameplay_actor_update_all_instance_freeze_gate_passes",
                    typedGameplayStats
                        .ActorUpdateAllInstanceFreezeGatePasses},
                   {"typed_gameplay_actor_update_all_instance_freeze_gate_skips",
                    typedGameplayStats
                        .ActorUpdateAllInstanceFreezeGateSkips},
                   {"typed_gameplay_actor_update_all_effect_timer_block_calls",
                    typedGameplayStats.ActorUpdateAllEffectTimerBlockCalls},
                   {"typed_gameplay_actor_update_all_effect_timer_logical_field_advances",
                    typedGameplayStats
                        .ActorUpdateAllEffectTimerLogicalFieldAdvances},
                   {"typed_gameplay_actor_update_all_effect_timer_intermediate_field_holds",
                    typedGameplayStats
                        .ActorUpdateAllEffectTimerIntermediateFieldHolds},
                   {"typed_gameplay_actor_init_callback_dispatches",
                    typedGameplayStats.ActorInitCallbackDispatches},
                   {"typed_gameplay_actor_init_callback_returns",
                    typedGameplayStats.ActorInitCallbackReturns},
                   {"typed_gameplay_actor_update_callback_dispatches",
                    typedGameplayStats.ActorUpdateCallbackDispatches},
                   {"typed_gameplay_actor_destroy_callback_dispatches",
                    typedGameplayStats.ActorDestroyCallbackDispatches},
                   {"typed_gameplay_actor_resource_cleanup_dispatches",
                    typedGameplayStats.ActorResourceCleanupDispatches},
                   {"typed_gameplay_actor_destroy_returns",
                    typedGameplayStats.ActorDestroyReturns},
                   {"typed_gameplay_audio_request_reference_acquire_dispatches",
                    typedGameplayStats
                        .AudioRequestReferenceAcquireDispatches},
                   {"typed_gameplay_audio_request_status_query_dispatches",
                    typedGameplayStats.AudioRequestStatusQueryDispatches},
                   {"typed_gameplay_audio_request_reference_cleanup_dispatches",
                    typedGameplayStats
                        .AudioRequestReferenceCleanupDispatches},
                   {"typed_gameplay_audio_request_callback_returns",
                    typedGameplayStats.AudioRequestCallbackReturns},
                   {"typed_gameplay_pause_ui_alpha_pause_state_dispatches",
                    typedGameplayStats.PauseUiAlphaPauseStateDispatches},
                   {"typed_gameplay_pause_ui_alpha_first_step_dispatches",
                    typedGameplayStats.PauseUiAlphaFirstStepDispatches},
                   {"typed_gameplay_pause_ui_alpha_tail_step_dispatches",
                    typedGameplayStats.PauseUiAlphaTailStepDispatches},
                   {"typed_gameplay_pause_ui_alpha_returns",
                    typedGameplayStats.PauseUiAlphaReturns},
                   {"typed_gameplay_dyna_interaction_reset_matches",
                    typedGameplayStats.DynaInteractionResetMatches},
                   {"typed_gameplay_dyna_interaction_reset_misses",
                    typedGameplayStats.DynaInteractionResetMisses},
                   {"typed_gameplay_player_release_lock_on_calls",
                    typedGameplayStats.PlayerReleaseLockOnCalls},
                   {"typed_gameplay_actor_update_record_initialize_calls",
                    typedGameplayStats.ActorUpdateRecordInitializeCalls},
                   {"typed_gameplay_actor_update_record_clear_calls",
                    typedGameplayStats.ActorUpdateRecordClearCalls},
                   {"typed_gameplay_record_initializer_memzero_dispatches",
                    typedGameplayStats.RecordInitializerMemzeroDispatches},
                   {"typed_gameplay_record_initializer_returns",
                    typedGameplayStats.RecordInitializerReturns},
                   {"typed_gameplay_player_calls",
                    typedGameplayStats.PlayerCalls},
                   {"typed_gameplay_math_calls", typedGameplayStats.MathCalls},
                   {"typed_gameplay_animation_calls",
                    typedGameplayStats.AnimationCalls},
                   {"typed_gameplay_renderer_calls",
                    typedGameplayStats.RendererCalls},
                   {"typed_gameplay_cutscene_calls",
                    typedGameplayStats.CutsceneCalls},
                   {"typed_gameplay_camera_calls",
                    typedGameplayStats.CameraCalls},
                   {"typed_gameplay_camera_mode_frame_countdown_block_calls",
                    typedGameplayStats.CameraModeFrameCountdownBlockCalls},
                   {"typed_gameplay_camera_mode_frame_countdown_logical_advances",
                    typedGameplayStats
                        .CameraModeFrameCountdownLogicalAdvances},
                   {"typed_gameplay_camera_mode_frame_countdown_intermediate_holds",
                    typedGameplayStats
                        .CameraModeFrameCountdownIntermediateHolds},
                   {"typed_gameplay_camera_special5_timer_block_calls",
                    typedGameplayStats.CameraSpecial5TimerBlockCalls},
                   {"typed_gameplay_camera_special5_timer_logical_decrements",
                    typedGameplayStats
                        .CameraSpecial5TimerLogicalDecrements},
                   {"typed_gameplay_camera_special5_timer_intermediate_holds",
                    typedGameplayStats
                        .CameraSpecial5TimerIntermediateHolds},
                   {"typed_gameplay_camera_special5_timer_zero_transitions",
                    typedGameplayStats.CameraSpecial5TimerZeroTransitions},
                   {"typed_gameplay_camera_special5_timer_terminal_continues",
                    typedGameplayStats
                        .CameraSpecial5TimerTerminalContinues},
                   {"typed_gameplay_camera_floor_miss_counter_block_calls",
                    typedGameplayStats.CameraFloorMissCounterBlockCalls},
                   {"typed_gameplay_camera_floor_miss_counter_logical_advances",
                    typedGameplayStats
                        .CameraFloorMissCounterLogicalAdvances},
                   {"typed_gameplay_camera_floor_miss_counter_intermediate_holds",
                    typedGameplayStats
                        .CameraFloorMissCounterIntermediateHolds},
                   {"typed_gameplay_camera_interface_delay_block_calls",
                    typedGameplayStats.CameraInterfaceDelayBlockCalls},
                   {"typed_gameplay_camera_interface_delay_logical_advances",
                    typedGameplayStats
                        .CameraInterfaceDelayLogicalAdvances},
                   {"typed_gameplay_camera_interface_delay_intermediate_holds",
                    typedGameplayStats
                        .CameraInterfaceDelayIntermediateHolds},
                   {"typed_gameplay_camera_water_distortion_timer_block_calls",
                    typedGameplayStats
                        .CameraWaterDistortionTimerBlockCalls},
                   {"typed_gameplay_camera_water_distortion_timer_logical_advances",
                    typedGameplayStats
                        .CameraWaterDistortionTimerLogicalAdvances},
                   {"typed_gameplay_camera_water_distortion_timer_intermediate_holds",
                    typedGameplayStats
                        .CameraWaterDistortionTimerIntermediateHolds},
                   {"typed_gameplay_camera_water_distortion_fractional_samples",
                    typedGameplayStats
                        .CameraWaterDistortionFractionalSamples},
                   {"typed_gameplay_camera_water_distortion_flag4_samples",
                    typedGameplayStats.CameraWaterDistortionFlag4Samples},
                   {"typed_gameplay_camera_water_distortion_flag8_samples",
                    typedGameplayStats.CameraWaterDistortionFlag8Samples},
                   {"typed_gameplay_camera_water_distortion_custom_samples",
                    typedGameplayStats.CameraWaterDistortionCustomSamples},
                   {"typed_gameplay_camera_water_distortion_sample_failures",
                    typedGameplayStats.CameraWaterDistortionSampleFailures},
                   {"typed_gameplay_camera_quake_callback_calls",
                    typedGameplayStats.CameraQuakeCallbackCalls},
                   {"typed_gameplay_camera_quake_logical_advances",
                    typedGameplayStats.CameraQuakeLogicalAdvances},
                   {"typed_gameplay_camera_quake_intermediate_holds",
                    typedGameplayStats.CameraQuakeIntermediateHolds},
                   {"typed_gameplay_camera_quake_random_samples",
                    typedGameplayStats.CameraQuakeRandomSamples},
                   {"typed_gameplay_camera_quake_signal_reuses",
                    typedGameplayStats.CameraQuakeSignalReuses},
                   {"typed_gameplay_camera_quake_signal_cache_misses",
                    typedGameplayStats.CameraQuakeSignalCacheMisses},
                   {"typed_gameplay_camera_quake_helper_dispatches",
                    typedGameplayStats.CameraQuakeHelperDispatches},
                   {"typed_gameplay_camera_quake_return_dispatches",
                    typedGameplayStats.CameraQuakeReturnDispatches},
                   {"typed_gameplay_camera_quake_failures",
                    typedGameplayStats.CameraQuakeFailures},
                   {"typed_gameplay_cutscene_normal_frame_block_calls",
                    typedGameplayStats.CutsceneNormalFrameBlockCalls},
                   {"typed_gameplay_cutscene_logical_frame_advances",
                    typedGameplayStats.CutsceneLogicalFrameAdvances},
                   {"typed_gameplay_cutscene_intermediate_holds",
                    typedGameplayStats.CutsceneIntermediateHolds},
                   {"typed_gameplay_cutscene_command_dispatches",
                    typedGameplayStats.CutsceneCommandDispatches},
                   {"typed_gameplay_cutscene_actor_cue_interpolation_calls",
                    typedGameplayStats.CutsceneActorCueInterpolationCalls},
                   {"typed_gameplay_cutscene_actor_cue_fractional_samples",
                    typedGameplayStats.CutsceneActorCueFractionalSamples},
                   {"typed_gameplay_cutscene_camera_binding_observations",
                    typedGameplayStats.CutsceneCameraBindingObservations},
                   {"typed_gameplay_cutscene_camera_rejected_observations",
                    typedGameplayStats.CutsceneCameraRejectedObservations},
                   {"typed_gameplay_cutscene_camera_fractional_samples",
                    typedGameplayStats.CutsceneCameraFractionalSamples},
                   {"typed_gameplay_cutscene_camera_curve_samples",
                    typedGameplayStats.CutsceneCameraCurveSamples},
                   {"typed_gameplay_cutscene_camera_sample_failures",
                    typedGameplayStats.CutsceneCameraSampleFailures},
                   {"typed_gameplay_actor_cutscene_camera_binding_observations",
                    typedGameplayStats
                        .ActorCutsceneCameraBindingObservations},
                   {"typed_gameplay_actor_cutscene_camera_priming_observations",
                    typedGameplayStats
                        .ActorCutsceneCameraPrimingObservations},
                   {"typed_gameplay_actor_cutscene_camera_rejected_observations",
                    typedGameplayStats
                        .ActorCutsceneCameraRejectedObservations},
                   {"typed_gameplay_actor_cutscene_camera_intermediate_holds",
                    typedGameplayStats.ActorCutsceneCameraIntermediateHolds},
                   {"typed_gameplay_actor_cutscene_camera_fractional_samples",
                    typedGameplayStats.ActorCutsceneCameraFractionalSamples},
                   {"typed_gameplay_actor_cutscene_camera_curve_samples",
                    typedGameplayStats.ActorCutsceneCameraCurveSamples},
                   {"typed_gameplay_actor_cutscene_camera_sample_failures",
                    typedGameplayStats.ActorCutsceneCameraSampleFailures},
                   {"typed_gameplay_fishing_state_block_calls",
                    typedGameplayStats.FishingStateBlockCalls},
                   {"typed_gameplay_fishing_state_logical_advances",
                    typedGameplayStats.FishingStateLogicalAdvances},
                   {"typed_gameplay_fishing_state_intermediate_holds",
                    typedGameplayStats.FishingStateIntermediateHolds},
                   {"typed_gameplay_damage_run_timer_block_calls",
                    typedGameplayStats.DamageRunTimerBlockCalls},
                   {"typed_gameplay_damage_run_timer_logical_advances",
                    typedGameplayStats.DamageRunTimerLogicalAdvances},
                   {"typed_gameplay_damage_run_timer_intermediate_holds",
                    typedGameplayStats.DamageRunTimerIntermediateHolds},
                   {"typed_gameplay_invincibility_timer_block_calls",
                    typedGameplayStats.InvincibilityTimerBlockCalls},
                   {"typed_gameplay_invincibility_timer_logical_advances",
                    typedGameplayStats.InvincibilityTimerLogicalAdvances},
                   {"typed_gameplay_invincibility_timer_intermediate_holds",
                    typedGameplayStats.InvincibilityTimerIntermediateHolds},
                   {"typed_gameplay_invincibility_timer_positive_action_holds",
                    typedGameplayStats
                        .InvincibilityTimerPositiveActionHolds},
                   {"typed_gameplay_damage_flicker_counter_block_calls",
                    typedGameplayStats.DamageFlickerCounterBlockCalls},
                   {"typed_gameplay_damage_flicker_counter_logical_advances",
                    typedGameplayStats
                        .DamageFlickerCounterLogicalAdvances},
                   {"typed_gameplay_damage_flicker_counter_intermediate_holds",
                    typedGameplayStats
                        .DamageFlickerCounterIntermediateHolds},
                   {"typed_gameplay_respawn_damage_block_calls",
                    typedGameplayStats.RespawnDamageBlockCalls},
                   {"typed_gameplay_respawn_damage_logical_advances",
                    typedGameplayStats.RespawnDamageLogicalAdvances},
                   {"typed_gameplay_respawn_damage_intermediate_holds",
                    typedGameplayStats.RespawnDamageIntermediateHolds},
                   {"typed_gameplay_respawn_damage_audio_dispatches",
                    typedGameplayStats.RespawnDamageAudioDispatches},
                   {"typed_gameplay_random_turn_timer_decrement_block_calls",
                    typedGameplayStats
                        .RandomTurnTimerDecrementBlockCalls},
                   {"typed_gameplay_random_turn_timer_refresh_block_calls",
                    typedGameplayStats.RandomTurnTimerRefreshBlockCalls},
                   {"typed_gameplay_random_turn_timer_logical_advances",
                    typedGameplayStats.RandomTurnTimerLogicalAdvances},
                   {"typed_gameplay_random_turn_timer_intermediate_holds",
                    typedGameplayStats.RandomTurnTimerIntermediateHolds},
                   {"typed_gameplay_random_turn_timer_rng_dispatches",
                    typedGameplayStats.RandomTurnTimerRngDispatches},
                   {"typed_gameplay_random_turn_timer_rng_intermediate_suppressions",
                    typedGameplayStats
                        .RandomTurnTimerRngIntermediateSuppressions},
                   {"typed_gameplay_attention_persistence_block_calls",
                    typedGameplayStats.AttentionPersistenceBlockCalls},
                   {"typed_gameplay_attention_persistence_logical_advances",
                    typedGameplayStats
                        .AttentionPersistenceLogicalAdvances},
                   {"typed_gameplay_attention_persistence_intermediate_holds",
                    typedGameplayStats
                        .AttentionPersistenceIntermediateHolds},
                   {"typed_gameplay_attention_persistence_saturation_holds",
                    typedGameplayStats
                        .AttentionPersistenceSaturationHolds},
                   {"typed_gameplay_common_countdown_block_calls",
                    typedGameplayStats.CommonCountdownBlockCalls},
                   {"typed_gameplay_common_countdown_logical_field_advances",
                    typedGameplayStats.CommonCountdownLogicalFieldAdvances},
                   {"typed_gameplay_common_countdown_logical_field_intermediate_holds",
                    typedGameplayStats
                        .CommonCountdownLogicalFieldIntermediateHolds},
                   {"typed_gameplay_fairy_revive_grace_timer_advances",
                    typedGameplayStats.FairyReviveGraceTimerAdvances},
                   {"typed_gameplay_underwater_timer_reset_block_calls",
                    typedGameplayStats.UnderwaterTimerResetBlockCalls},
                   {"typed_gameplay_underwater_timer_increment_block_calls",
                    typedGameplayStats.UnderwaterTimerIncrementBlockCalls},
                   {"typed_gameplay_underwater_timer_logical_advances",
                    typedGameplayStats.UnderwaterTimerLogicalAdvances},
                   {"typed_gameplay_underwater_timer_intermediate_holds",
                    typedGameplayStats.UnderwaterTimerIntermediateHolds},
                   {"typed_gameplay_melee_action_timer_block_calls",
                    typedGameplayStats.MeleeActionTimerBlockCalls},
                   {"typed_gameplay_melee_action_timer_logical_advances",
                    typedGameplayStats.MeleeActionTimerLogicalAdvances},
                   {"typed_gameplay_melee_action_timer_intermediate_holds",
                    typedGameplayStats.MeleeActionTimerIntermediateHolds},
                   {"typed_gameplay_melee_action_combo_clears",
                    typedGameplayStats.MeleeActionComboClears},
                   {"typed_gameplay_melee_weapon_tip_combo_block_calls",
                    typedGameplayStats.MeleeWeaponTipComboBlockCalls},
                   {"typed_gameplay_melee_weapon_tip_combo_logical_advances",
                    typedGameplayStats
                        .MeleeWeaponTipComboLogicalAdvances},
                   {"typed_gameplay_melee_weapon_tip_combo_intermediate_holds",
                    typedGameplayStats
                        .MeleeWeaponTipComboIntermediateHolds},
                   {"typed_gameplay_retained_aot_fallbacks",
                    typedGameplayStats.RetainedAotFallbacks},
                   {"typed_gameplay_read_failures",
                    typedGameplayStats.ReadFailures},
                   {"typed_gameplay_write_failures",
                    typedGameplayStats.WriteFailures},
                   {"mass_aot_enabled",
                    !launch.DisableCompiledFunctions &&
                        !launch.DisableMassAot &&
                        Oot3dNativeGame::Oot3dMassAotAvailable()},
                   {"mass_aot_entry_points",
                    Oot3dNativeGame::Oot3dMassAotBlockEntryPoints().size()},
                   {"mass_aot_excluded_boundary_entries",
                    Oot3dNativeGame::Oot3dMassAotExcludedBoundaryCount()},
                   {"mass_aot_calls", massAotStats.Calls},
                   {"mass_aot_blocks", massAotStats.Blocks},
                   {"mass_aot_region_exits", massAotStats.RegionExits},
                   {"mass_aot_svc_exits", massAotStats.SvcExits},
                   {"mass_aot_block_limit_exits", massAotStats.BlockLimitExits},
                   {"mass_aot_memory_faults", massAotStats.MemoryFaults},
                   {"mass_aot_missing_blocks", massAotStats.MissingBlocks},
                   {"mass_aot_fallback_exits", massAotStats.FallbackExits},
                   {"mass_aot_unsupported_exits",
                    massAotStats.UnsupportedExits},
                   {"whole_aot_available", wholeAotAvailable},
                   {"whole_aot_requested",
                    !launch.DisableCompiledFunctions &&
                        !launch.DisableWholeAot},
                   {"whole_aot_enabled", enableWholeAot},
                   {"manual_compiled_functions_enabled",
                    !launch.DisableCompiledFunctions &&
                    enableManualCompiledFunctions},
                   {"entry_points",
                    Oot3dNativeGame::Oot3dCompiledFunctionEntryPoints(
                        enableWholeAot,
                        enableCompiledFunctions &&
                            enableManualCompiledFunctions)
                        .size()},
                   {"calls", stats.Calls},
                   {"bytes_processed", stats.BytesProcessed},
                   {"retained_arm_fallbacks", stats.RetainedArmFallbacks},
                   {"memcpy_overlap_fallbacks", stats.MemcpyOverlapFallbacks},
                   {"memcpy_overlap_bytes", stats.MemcpyOverlapBytes},
                   {"memcpy_destination_after_source_fallbacks",
                    stats.MemcpyDestinationAfterSourceFallbacks},
                    {"memcpy_destination_before_source_fallbacks",
                     stats.MemcpyDestinationBeforeSourceFallbacks},
                    {"mtx3x4_fast_calls", stats.Mtx3x4FastCalls},
                    {"mtx3x4_soft_calls", stats.Mtx3x4SoftCalls},
                    {"mtx3x4_validation_calls",
                     stats.Mtx3x4ValidationCalls},
                    {"mtx3x4_value_mismatches",
                     stats.Mtx3x4ValueMismatches},
                    {"mtx3x4_flag_mismatches",
                     stats.Mtx3x4FlagMismatches},
                    {"whole_aot_calls", stats.WholeAotCalls},
                   {"whole_aot_direct_calls", stats.WholeAotDirectCalls},
                   {"whole_aot_indirect_calls", stats.WholeAotIndirectCalls},
                   {"whole_aot_resolved_indirect_calls",
                    stats.WholeAotResolvedIndirectCalls},
                    {"whole_aot_external_calls", stats.WholeAotExternalCalls},
                    {"whole_aot_external_targets",
                     std::move(externalTargets)},
                    {"whole_aot_svc_exits", stats.WholeAotSvcExits},
                   {"whole_aot_block_budget", launch.WholeAotBlockBudget},
                   {"whole_aot_block_limit_exits",
                    stats.WholeAotBlockLimitExits},
                   {"whole_aot_memory_faults", stats.WholeAotMemoryFaults},
                   {"whole_aot_unsupported_exits",
                    stats.WholeAotUnsupportedExits},
               };
             }()},
            {"true_aot_blocks",
             [&]() {
               const auto stats = Oot3dNativeGame::GetOot3dTrueAotBlockStats();
               return nlohmann::json{
                   {"enabled", !launch.DisableCompiledFunctions &&
                                   !launch.DisableTrueAotBlocks},
                   {"entry_points",
                    Oot3dNativeGame::Oot3dTrueAotBlockEntryPoints().size()},
                   {"calls", stats.Calls},
                   {"iterations", stats.Iterations},
                   {"memory_faults", stats.MemoryFaults},
               };
             }()},
            {"a32_runtime_profile",
             [&]() {
               const auto timing = process.TimingStats();
               const auto ctrTiming = hostServices.RuntimeProfile();
               const auto picaSubmissionTiming =
                   submissionQueue.RuntimeProfile();
               const auto picaBackend = api.GetNativePicaBackendStats();
               constexpr double nanosecondsToSeconds = 1.0e-9;
               const uint64_t candidateSampleNanoseconds =
                   nativeCandidateDispatch.TypedGameplaySampleNanoseconds +
                   nativeCandidateDispatch.CompiledSampleNanoseconds +
                   nativeCandidateDispatch.TrueAotSampleNanoseconds +
                   nativeCandidateDispatch.UnhandledSampleNanoseconds;
               nlohmann::json svcByImmediate = nlohmann::json::array();
               for (size_t immediate = 0;
                    immediate < timing.SvcCallsByImmediate.size();
                    ++immediate) {
                 if (timing.SvcCallsByImmediate[immediate] == 0U) {
                   continue;
                 }
                 svcByImmediate.push_back({
                     {"immediate", immediate},
                     {"calls", timing.SvcCallsByImmediate[immediate]},
                     {"seconds", timing.SvcNanosecondsByImmediate[immediate] *
                                     nanosecondsToSeconds},
                 });
               }
               const auto &ipcNameCounts = hostServices.IpcNameCounts();
               nlohmann::json ipcByName = nlohmann::json::array();
               for (const auto &[name, calls] : ipcNameCounts) {
                 ipcByName.push_back({
                     {"name", name},
                     {"calls", calls},
                 });
               }
               const uint64_t measuredTriggerNanoseconds =
                   ctrTiming.CommandListReadNanoseconds +
                   ctrTiming.PicaFrontendSubmitNanoseconds;
               const uint64_t triggerPostNanoseconds =
                   ctrTiming.TriggerCommandQueueNanoseconds >
                           measuredTriggerNanoseconds
                       ? ctrTiming.TriggerCommandQueueNanoseconds -
                             measuredTriggerNanoseconds
                       : 0U;
               return nlohmann::json{
                   {"enabled", launch.ProfileA32Runtime},
                   {"sample_rate_denominator", kA32RuntimeSampleDenominator},
                   {"process_run_calls", timing.ProcessRunCalls},
                   {"dispatch_calls", timing.DispatchCalls},
                   {"dispatch_seconds",
                    timing.DispatchNanoseconds * nanosecondsToSeconds},
                   {"svc_calls", timing.SvcCalls},
                   {"svc_seconds",
                    timing.SvcNanoseconds * nanosecondsToSeconds},
                   {"svc_event_count", hostServices.SvcEventCount()},
                   {"svc_by_immediate", std::move(svcByImmediate)},
                   {"ipc_by_name", std::move(ipcByName)},
                   {"romfs_stream_open_calls",
                    ctrTiming.RomFsStreamOpenCalls},
                   {"romfs_read_calls", ctrTiming.RomFsReadCalls},
                   {"romfs_read_bytes", ctrTiming.RomFsReadBytes},
                   {"romfs_read_seconds",
                    ctrTiming.RomFsReadNanoseconds * nanosecondsToSeconds},
                   {"gsp_trigger_command_queue_calls",
                    ctrTiming.TriggerCommandQueueCalls},
                   {"gsp_trigger_command_queue_seconds",
                    ctrTiming.TriggerCommandQueueNanoseconds *
                        nanosecondsToSeconds},
                   {"gsp_command_packets", ctrTiming.CommandPackets},
                   {"gsp_command_list_bytes", ctrTiming.CommandListBytes},
                   {"gsp_command_list_read_seconds",
                    ctrTiming.CommandListReadNanoseconds *
                        nanosecondsToSeconds},
                   {"gsp_pica_frontend_submit_seconds",
                    ctrTiming.PicaFrontendSubmitNanoseconds *
                        nanosecondsToSeconds},
                   {"gsp_trigger_post_seconds",
                    triggerPostNanoseconds * nanosecondsToSeconds},
                   {"pica_submission_draw_calls",
                    picaSubmissionTiming.DrawCalls},
                   {"pica_submission_total_seconds",
                    picaSubmissionTiming.TotalNanoseconds *
                        nanosecondsToSeconds},
                   {"pica_submission_packet_copy_seconds",
                    picaSubmissionTiming.PacketCopyNanoseconds *
                        nanosecondsToSeconds},
                   {"pica_submission_decode_seconds",
                    picaSubmissionTiming.DecodeNanoseconds *
                        nanosecondsToSeconds},
                   {"pica_submission_vertex_capture_seconds",
                    picaSubmissionTiming.VertexCaptureNanoseconds *
                        nanosecondsToSeconds},
                   {"pica_submission_texture_capture_seconds",
                    picaSubmissionTiming.TextureCaptureNanoseconds *
                        nanosecondsToSeconds},
                   {"pica_submission_enqueue_seconds",
                    picaSubmissionTiming.EnqueueNanoseconds *
                        nanosecondsToSeconds},
                   {"pica_submission_index_bytes",
                    picaSubmissionTiming.IndexBytes},
                   {"pica_submission_vertex_bytes",
                    picaSubmissionTiming.VertexBytes},
                   {"pica_submission_texture_bytes",
                    picaSubmissionTiming.TextureBytes},
                   {"pica_submission_texture_snapshot_cache_hits",
                    picaSubmissionTiming.TextureSnapshotCacheHits},
                   {"pica_submission_texture_snapshot_cache_misses",
                    picaSubmissionTiming.TextureSnapshotCacheMisses},
                   {"pica_submission_texture_snapshot_compared_bytes",
                    picaSubmissionTiming.TextureSnapshotComparedBytes},
                   {"pica_submission_texture_snapshot_copied_bytes",
                    picaSubmissionTiming.TextureSnapshotCopiedBytes},
                   {"pica_submission_geometry_snapshot_cache_hits",
                    picaSubmissionTiming.GeometrySnapshotCacheHits},
                   {"pica_submission_geometry_snapshot_cache_misses",
                    picaSubmissionTiming.GeometrySnapshotCacheMisses},
                   {"pica_submission_geometry_snapshot_version_hits",
                    picaSubmissionTiming.GeometrySnapshotVersionHits},
                   {"pica_submission_geometry_snapshot_version_misses",
                    picaSubmissionTiming.GeometrySnapshotVersionMisses},
                   {"pica_submission_geometry_snapshot_compared_bytes",
                    picaSubmissionTiming.GeometrySnapshotComparedBytes},
                   {"pica_submission_geometry_snapshot_copied_bytes",
                    picaSubmissionTiming.GeometrySnapshotCopiedBytes},
                   {"pica_backend_geometry_registry_hits",
                    picaBackend.GeometryRegistryHits},
                   {"pica_backend_stats_available",
                    picaBackend.Available},
                   {"pica_backend_geometry_cache_enabled",
                    picaBackend.GeometryCacheEnabled},
                   {"pica_backend_geometry_registry_misses",
                    picaBackend.GeometryRegistryMisses},
                   {"pica_backend_geometry_registry_updates",
                    picaBackend.GeometryRegistryUpdates},
                   {"pica_backend_geometry_registry_evictions",
                    picaBackend.GeometryRegistryEvictions},
                   {"pica_backend_geometry_registry_entries",
                    picaBackend.GeometryRegistryEntries},
                   {"pica_backend_geometry_persistent_draws",
                    picaBackend.GeometryPersistentDraws},
                   {"pica_backend_geometry_streaming_draws",
                    picaBackend.GeometryStreamingDraws},
                   {"pica_backend_geometry_persistent_uploads",
                    picaBackend.GeometryPersistentUploads},
                   {"pica_backend_geometry_persistent_upload_bytes",
                    picaBackend.GeometryPersistentUploadBytes},
                   {"pica_backend_geometry_streaming_uploads",
                    picaBackend.GeometryStreamingUploads},
                   {"pica_backend_geometry_streaming_upload_bytes",
                    picaBackend.GeometryStreamingUploadBytes},
                   {"pica_backend_uniform_uploads",
                    picaBackend.UniformUploads},
                   {"pica_backend_uniform_upload_bytes",
                    picaBackend.UniformUploadBytes},
                   {"fallback_calls", timing.FallbackCalls},
                   {"fallback_seconds",
                    timing.FallbackNanoseconds * nanosecondsToSeconds},
                   {"candidate_calls", nativeCandidateDispatch.Calls},
                   {"topscreen_source_port_calls",
                    nativeCandidateDispatch.TopScreenSourcePortCalls},
                   {"topscreen_title_logo_fade_hold_calls",
                    nativeCandidateDispatch.TopScreenTitleLogoFadeHoldCalls},
                   {"topscreen_lower_composition_skips",
                    nativeCandidateDispatch.TopScreenLowerCompositionSkips},
                   {"topscreen_pause_target_observations",
                    widescreenProjection.TopScreenPauseTargetObservations},
                   {"topscreen_pause_target_commands",
                    widescreenProjection.TopScreenPauseTargetCommands},
                   {"topscreen_pause_target_rewrites",
                    widescreenProjection.TopScreenPauseTargetRewrites},
                   {"topscreen_pause_start_close_calls",
                    nativeCandidateDispatch.TopScreenPauseStartCloseCalls},
                   {"topscreen_pause_system_open_calls",
                    nativeCandidateDispatch.TopScreenPauseSystemOpenCalls},
                   {"topscreen_aim_projectile_cycle_attempts",
                    nativeCandidateDispatch
                        .TopScreenAimProjectileCycleAttempts},
                   {"topscreen_aim_projectile_cycle_updates",
                    nativeCandidateDispatch
                        .TopScreenAimProjectileCycleUpdates},
                   {"topscreen_aim_projectile_cycle_sound_calls",
                    nativeCandidateDispatch
                        .TopScreenAimProjectileCycleSoundCalls},
                   {"topscreen_gameplay_dpad_attempts",
                    nativeCandidateDispatch.TopScreenGameplayDpadAttempts},
                   {"topscreen_navi_view_activations",
                    nativeCandidateDispatch.TopScreenNaviViewActivations},
                   {"topscreen_ocarina_queries",
                    nativeCandidateDispatch.TopScreenOcarinaQueries},
                   {"topscreen_ocarina_activations",
                    nativeCandidateDispatch.TopScreenOcarinaActivations},
                   {"topscreen_pause_last_closed_page",
                    static_cast<uint32_t>(
                        nativeCandidateDispatch.TopScreenPauseLastClosedPage)},
                   {"topscreen_pause_system_source_page",
                    static_cast<uint32_t>(nativeCandidateDispatch
                                              .TopScreenPauseSystemSourcePage)},
                   {"topscreen_pause_child_suppressions",
                    widescreenProjection.TopScreenPauseChildSuppressions},
                   {"topscreen_pause_child_native_draws",
                    widescreenProjection.TopScreenPauseChildNativeDraws},
                   {"topscreen_pause_page_composition_calls",
                    widescreenProjection.TopScreenPausePageCompositionCalls},
                   {"topscreen_pause_page_redraws",
                    widescreenProjection.TopScreenPausePageRedraws},
                   {"topscreen_pause_page_redraw_skips",
                    widescreenProjection.TopScreenPausePageRedrawSkips},
                   {"topscreen_pause_last_route_active",
                    widescreenProjection.LastTopScreenPauseRouteActive},
                   {"topscreen_pause_last_children_suppressed",
                    widescreenProjection
                        .LastTopScreenPauseChildrenSuppressed},
                   {"topscreen_pause_last_inputs",
                    widescreenProjection.HasTopScreenPauseDrawInputs
                        ? nlohmann::json{
                              {"has_scene",
                               widescreenProjection
                                   .LastTopScreenPauseDrawInputs.HasScene},
                              {"scene_mode",
                               widescreenProjection
                                   .LastTopScreenPauseDrawInputs.SceneMode},
                              {"scene_variant",
                               widescreenProjection
                                   .LastTopScreenPauseDrawInputs.SceneVariant},
                              {"scene_sequence",
                               widescreenProjection
                                   .LastTopScreenPauseDrawInputs.SceneSequence},
                              {"runtime_mode",
                               widescreenProjection
                                   .LastTopScreenPauseDrawInputs.RuntimeMode},
                              {"native_transition_active",
                               widescreenProjection.LastTopScreenPauseDrawInputs
                                   .NativeTransitionActive},
                              {"alternate_path_ready",
                               widescreenProjection.LastTopScreenPauseDrawInputs
                                   .AlternatePathReady},
                              {"alternate_path_visible",
                               widescreenProjection.LastTopScreenPauseDrawInputs
                                   .AlternatePathVisible},
                              {"suppression_override",
                               widescreenProjection.LastTopScreenPauseDrawInputs
                                   .SuppressionOverride},
                              {"any_page_gate_active",
                               widescreenProjection.LastTopScreenPauseDrawInputs
                                   .AnyPageGateActive},
                              {"gear_state",
                               widescreenProjection.LastTopScreenPauseDrawInputs
                                   .ChildStates[0]},
                              {"items_state",
                               widescreenProjection.LastTopScreenPauseDrawInputs
                                   .ChildStates[1]},
                              {"dungeon_map_state",
                               widescreenProjection.LastTopScreenPauseDrawInputs
                                   .ChildStates[2]},
                              {"system_state",
                               widescreenProjection.LastTopScreenPauseDrawInputs
                                   .ChildStates[3]},
                              {"selector_state",
                               widescreenProjection.LastTopScreenPauseDrawInputs
                                    .ChildStates[4]},
                              {"touch_button_state",
                               widescreenProjection.LastTopScreenPauseDrawInputs
                                   .ChildStates[5]},
                          }
                        : nlohmann::json(nullptr)},
                   {"topscreen_pause_route_state",
                    {{"previous_runtime_mode",
                      nativeCandidateDispatch.TopScreenPauseRoute
                          .PreviousRuntimeMode},
                     {"transition_phase",
                      nativeCandidateDispatch.TopScreenPauseRoute
                          .TransitionPhase},
                     {"remaining_calls",
                      nativeCandidateDispatch.TopScreenPauseRoute
                          .RemainingCalls}}},
                   {"topscreen_temporal_state",
                    {{"viewport_draw_phase",
                      nativeCandidateDispatch.TopScreenViewportDrawPhase},
                     {"scene_viewport_draw_phase",
                      nativeCandidateDispatch.TopScreenSceneViewportDrawPhase},
                     {"overlay_viewport_draw_phase",
                      nativeCandidateDispatch.TopScreenOverlayViewportDrawPhase},
                     {"touch_coordinate_update_phase",
                      nativeCandidateDispatch
                          .TopScreenTouchCoordinateUpdatePhase},
                     {"alternate_renderer_restore",
                      nativeCandidateDispatch.TopScreenAlternateRendererRestore
                          .has_value()},
                     {"aot_route_previous_runtime_mode",
                      nativeCandidateDispatch.TopScreenAotPauseRoute
                          .PreviousRuntimeMode},
                     {"aot_route_transition_phase",
                      nativeCandidateDispatch.TopScreenAotPauseRoute
                          .TransitionPhase},
                     {"aot_route_remaining_calls",
                      nativeCandidateDispatch.TopScreenAotPauseRoute
                          .RemainingCalls},
                     {"touch_runtime_scene_latch",
                      nativeCandidateDispatch.TopScreenTouchCoordinateRoute
                          .RuntimeSceneLatch},
                     {"renderer_visibility_fade_step",
                      nativeCandidateDispatch.TopScreenRendererVisibilityRoute
                          .FadeStep},
                     {"renderer_visibility_delay_calls",
                      nativeCandidateDispatch.TopScreenRendererVisibilityRoute
                          .DelayCalls},
                     {"renderer_visibility_conflict",
                      nativeCandidateDispatch.TopScreenRendererVisibilityRoute
                          .RendererConflict},
                     {"pause_controller_routed_latch",
                      nativeCandidateDispatch.TopScreenPauseController
                          .RoutedRendererLatched},
                     {"pause_controller_draw_phase",
                      nativeCandidateDispatch
                          .TopScreenPauseControllerDrawPhase},
                     {"pending_items_selection",
                      nativeCandidateDispatch.TopScreenItems.PendingSelection},
                     {"pending_target",
                      widescreenProjection.PendingTopScreenPauseTarget
                          .has_value()},
                     {"pending_target_renderer",
                      widescreenProjection.PendingTopScreenPauseRenderer},
                     {"draw_native_transition_latched",
                      widescreenProjection.TopScreenPauseDrawRouting
                          .NativeTransitionLatched},
                     {"draw_suppression_delay_armed",
                      widescreenProjection.TopScreenPauseDrawRouting
                          .SuppressionDelayArmed},
                     {"draw_suppression_delay_commands",
                      widescreenProjection.TopScreenPauseDrawRouting
                          .SuppressionDelayCommands},
                     {"page_redraw_delay_calls",
                      widescreenProjection.TopScreenPausePageRedraw.DelayCalls},
                     {"page_redraw_active",
                      widescreenProjection.TopScreenPausePageRedrawActive},
                     {"pending_child_restore",
                      widescreenProjection.PendingTopScreenPauseChildRestore
                          .has_value()}}},
                   {"topscreen_camera_normal1_calls",
                    nativeCandidateDispatch.TopScreenCameraNormal1Calls},
                   {"topscreen_camera_update_calls",
                    nativeCandidateDispatch.TopScreenCameraUpdateCalls},
                   {"topscreen_camera_active_calls",
                    nativeCandidateDispatch.TopScreenCameraActiveCalls},
                   {"topscreen_camera_enabled",
                    nativeCandidateDispatch.TopScreenCamera.Camera.Enabled},
                   {"topscreen_camera_n64_style_zoom",
                    nativeCandidateDispatch.TopScreenCamera.Camera
                        .N64StyleZoom},
                   {"topscreen_camera_last_blockers",
                    nativeCandidateDispatch.TopScreenCamera
                        .LastOwnershipBlockers},
                   {"topscreen_camera_last_ownership",
                    {{"game_mode",
                      nativeCandidateDispatch.TopScreenCamera
                          .LastOwnershipInput.GameMode},
                     {"entrance_index",
                      nativeCandidateDispatch.TopScreenCamera
                          .LastOwnershipInput.EntranceIndex},
                     {"cutscene_index",
                      nativeCandidateDispatch.TopScreenCamera
                          .LastOwnershipInput.CutsceneIndex},
                     {"scene",
                      nativeCandidateDispatch.TopScreenCamera
                          .LastOwnershipInput.Scene},
                     {"is_main_camera",
                      nativeCandidateDispatch.TopScreenCamera
                          .LastOwnershipInput.IsMainCamera},
                     {"has_player",
                      nativeCandidateDispatch.TopScreenCamera
                          .LastOwnershipInput.HasPlayer},
                     {"player_state_flags_1",
                      nativeCandidateDispatch.TopScreenCamera
                          .LastOwnershipInput.PlayerStateFlags1},
                     {"player_state_flags_2",
                      nativeCandidateDispatch.TopScreenCamera
                          .LastOwnershipInput.PlayerStateFlags2},
                     {"status",
                      nativeCandidateDispatch.TopScreenCamera
                          .LastOwnershipInput.CameraStatus},
                     {"setting",
                      nativeCandidateDispatch.TopScreenCamera
                          .LastOwnershipInput.CameraSetting},
                     {"right_stick_x",
                      nativeCandidateDispatch.TopScreenCamera
                          .LastOwnershipInput.RightStickX},
                     {"right_stick_y",
                      nativeCandidateDispatch.TopScreenCamera
                          .LastOwnershipInput.RightStickY}}},
                   {"topscreen_item_query_calls",
                    nativeCandidateDispatch.TopScreenItems.QueryCalls},
                   {"topscreen_item_query_trace", [&] {
                      auto rows = nlohmann::json::array();
                      for (const auto& row : nativeCandidateDispatch.TopScreenItems.QueryTrace)
                        rows.push_back({{"entry", row.Entry}, {"return_pc", row.ReturnPc},
                            {"flags", row.SuppressionFlags}, {"native", row.NativeResult},
                            {"result", row.Result}, {"zr_pressed", row.Input.ZrPressed},
                            {"zl_pressed", row.Input.ZlPressed}, {"zr_held", row.Input.ZrHeld},
                            {"zl_held", row.Input.ZlHeld}});
                      return rows;
                    }()},
                   {"topscreen_input_native_updates",
                    nativeCandidateDispatch.TopScreenInputClock.Updates},
                   {"topscreen_input_zr_press_updates",
                    nativeCandidateDispatch.TopScreenInputClock.ZrPressUpdates},
                   {"topscreen_input_zl_press_updates",
                    nativeCandidateDispatch.TopScreenInputClock.ZlPressUpdates},
                   {"topscreen_items_selection_updates",
                    nativeCandidateDispatch.TopScreenItems.SelectionUpdates},
                   {"topscreen_item_query_true_results",
                    nativeCandidateDispatch.TopScreenItems.QueryTrueResults},
                   {"topscreen_slot_item_attempts",
                    nativeCandidateDispatch.TopScreenItems.SlotAttempts},
                   {"topscreen_slot_item_overrides",
                    nativeCandidateDispatch.TopScreenItems.SlotOverrides},
                   {"topscreen_slot_item_native_fallbacks",
                    nativeCandidateDispatch.TopScreenItems.SlotNativeFallbacks},
                   {"topscreen_item_query_entries",
                    nativeCandidateDispatch.TopScreenItems.QueryEntries},
                   {"topscreen_slot_item_entries",
                    nativeCandidateDispatch.TopScreenItems.SlotEntries},
                   {"topscreen_items_selection_begins",
                    nativeCandidateDispatch.TopScreenItems.SelectionBegins},
                   {"topscreen_items_selection_completions",
                    nativeCandidateDispatch.TopScreenItems.SelectionCompletions},
                   {"candidate_samples", nativeCandidateDispatch.Samples},
                   {"candidate_estimated_seconds",
                    candidateSampleNanoseconds * kA32RuntimeSampleDenominator *
                        nanosecondsToSeconds},
                   {"typed_gameplay_samples",
                    nativeCandidateDispatch.TypedGameplaySamples},
                   {"typed_gameplay_estimated_seconds",
                    nativeCandidateDispatch.TypedGameplaySampleNanoseconds *
                        kA32RuntimeSampleDenominator * nanosecondsToSeconds},
                   {"compiled_samples",
                    nativeCandidateDispatch.CompiledSamples},
                   {"compiled_estimated_seconds",
                    nativeCandidateDispatch.CompiledSampleNanoseconds *
                        kA32RuntimeSampleDenominator * nanosecondsToSeconds},
                   {"true_aot_samples", nativeCandidateDispatch.TrueAotSamples},
                   {"true_aot_estimated_seconds",
                    nativeCandidateDispatch.TrueAotSampleNanoseconds *
                        kA32RuntimeSampleDenominator * nanosecondsToSeconds},
                   {"unhandled_samples",
                    nativeCandidateDispatch.UnhandledSamples},
                   {"unhandled_estimated_seconds",
                    nativeCandidateDispatch.UnhandledSampleNanoseconds *
                        kA32RuntimeSampleDenominator * nanosecondsToSeconds},
               };
             }()},
            {"phase_timing",
             {{"slow_frames", [&]() {
                 constexpr const char* names[]{"guest", "frame_start", "host_frame_start", "input_poll",
                   "renderer_frame_start", "dsp_mix", "audio_output", "pica_submit", "pica_plan",
                   "pica_backend", "pica_diagnostics", "visual_presentation", "visual_replay", "present", "pacing_wait"};
                 auto samples = nlohmann::json::array();
                 for (const auto& sample : slowFrames.Samples()) {
                   nlohmann::json phases;
                   for (size_t i = 0; i < std::size(names); ++i) phases[names[i]] = sample.PhaseMilliseconds[i];
                   samples.push_back({{"frame", sample.Frame}, {"total_ms", sample.Milliseconds}, {"phases_ms", phases}});
                 }
                 return samples;
               }()},
              {"slow_frame_note", "Nested phases overlap; do not sum parent and child timers. Measured benchmark frames only."},
              {"guest_seconds", phaseTiming.GuestSeconds},
              {"frame_start_seconds", phaseTiming.FrameStartSeconds},
              {"host_frame_start_seconds", phaseTiming.HostFrameStartSeconds},
              {"input_poll_seconds", phaseTiming.InputPollSeconds},
              {"renderer_frame_start_seconds", phaseTiming.RendererFrameStartSeconds},
              {"dsp_mix_seconds", phaseTiming.DspMixSeconds},
              {"audio_output_seconds", phaseTiming.AudioOutputSeconds},
              {"pica_submit_seconds", phaseTiming.PicaSubmitSeconds},
              {"pica_plan_seconds", phaseTiming.PicaPlanSeconds},
              {"pica_backend_seconds", phaseTiming.PicaBackendSeconds},
              {"pica_diagnostics_seconds", phaseTiming.PicaDiagnosticsSeconds},
              {"visual_presentation_seconds",
               phaseTiming.VisualPresentationSeconds},
              {"visual_replay_seconds", phaseTiming.VisualReplaySeconds},
              {"present_seconds", phaseTiming.PresentSeconds},
              {"pacing_wait_seconds", phaseTiming.PacingWaitSeconds}}},
            {"shader_source_cache",
             {{"vertex_entries", shaderSourceCache.VertexSources.size()},
              {"vertex_structural_entries",
               shaderSourceCache.VertexStructuralStates.size()},
              {"vertex_hits", shaderSourceCache.VertexHits},
              {"vertex_misses", shaderSourceCache.VertexMisses},
              {"fragment_entries", shaderSourceCache.FragmentSources.size()},
              {"fragment_hits", shaderSourceCache.FragmentHits},
              {"fragment_misses", shaderSourceCache.FragmentMisses}}},
            {"authority", "oot3d_a32_process_pica_vulkan"},
        });
  }

  if (launch.ExtendedDiagnostics &&
      (process.Memory().RangeWatchActive() || watchReportRequested)) {
    const auto& watched = process.Memory().WatchCounters();
    std::fprintf(
        stderr,
        "TRIAEVUM_RAW_COPY_HOST_ACCESS active=%d ranges=%zu typed_reads=%llu typed_writes=%llu "
        "byte_reads=%llu pointer_reads=%llu pointer_writes=%llu byte_writes=%llu "
        "host_byte_writes=%llu fills=%llu suppressed_byte_writes=%llu first_read=0x%08X "
        "first_size=%u first_write=0x%08X first_write_size=%u\n",
        process.Memory().RangeWatchActive() ? 1 : 0,
        process.Memory().WatchedRangeCount(),
        static_cast<unsigned long long>(watched.TypedReads),
        static_cast<unsigned long long>(watched.TypedWrites),
        static_cast<unsigned long long>(watched.ByteReads),
        static_cast<unsigned long long>(watched.PointerReads),
        static_cast<unsigned long long>(watched.PointerWrites),
        static_cast<unsigned long long>(watched.ByteWrites),
        static_cast<unsigned long long>(watched.HostByteWrites),
        static_cast<unsigned long long>(watched.Fills),
        static_cast<unsigned long long>(watched.SuppressedByteWrites),
        watched.FirstReadAddress,
        static_cast<unsigned>(watched.FirstReadSize),
        watched.FirstWriteAddress,
        static_cast<unsigned>(watched.FirstWriteSize));
  }

  if (launch.ScenarioStrict &&
      (!scenarioBootstrap.has_value() || !scenarioBootstrap->Complete())) {
    const std::string detail =
        scenarioBootstrap.has_value()
            ? scenarioBootstrap->Stats().Error
            : "structural scenario bootstrap is unavailable";
    throw std::runtime_error("structural scenario did not complete capture" +
                             (detail.empty() ? std::string() : ": " + detail));
  }
}
