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
#include "oot3d_native_a32_window_stats.h"
#include "oot3d_native_a32_window_summary.h"

using namespace Oot3dA32Window;

namespace {

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
  NativeA32SavestateStats savestateStats;
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
      };
  const auto restoreSavestateTopScreenTemporalState =
      [&](const Oot3dNativeGame::NativeA32SavestateRuntimeState &runtime) {
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
      };
  const auto restoreSavestatePicaTextureCache =
      [&](const Oot3dNativeGame::NativeA32SavestateRuntimeState &runtime) {
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
      };
  const auto restoreSavestatePicaColorTargets =
      [&](const Oot3dNativeGame::NativeA32SavestateRuntimeState &runtime) {
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
      };
  const auto restoreSavestatePicaPresentationState =
      [&](const Oot3dNativeGame::NativeA32SavestateRuntimeState &runtime) {
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
    ++savestateStats.LoadCount;
    savestateStats.LoadBytes += ioResult.FileBytes;
    savestateStats.LoadSeconds += SecondsSince(phaseStart);
    savestateStats.ReadSeconds += ioResult.IoSeconds;
    savestateStats.DecodeSeconds += ioResult.DecodeSeconds;
    savestateStats.RestoreSeconds += ioResult.RestoreSeconds;
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
  NativeA32PicaSubmissionStats picaStats;
  NativeA32VisualStats visualStats;
  Oot3dNativeGame::Oot3dPicaVisualInterpolationTiming visualInterpolationTiming;
  // Output addresses of selected top-screen transfers (the display
  // framebuffers): an earlier snapshot into one ends a superseded guest frame.
  std::vector<uint32_t> topDisplayOutputs;
  uint64_t vblankCount = 0;
  NativeA32DspMixStats dspStats;
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
        ++picaStats.CompletedPpfCount;
        ++picaStats.RecordedCompletionCount;
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
    savestateStats.PicaVisualReplayBytesCaptured +=
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
      savestateStats.PicaTextureBytesCaptured += texture.PixelBytes.size();
    }
    savestateStats.PicaTexturesCaptured += runtime.PicaTextureCache.size();
    if (!api.CapturePicaColorTargets(
            runtime.PicaColorTargets, &error)) {
      std::cerr
          << "oot3d_native_game: native PICA color capture failed: "
          << error << '\n';
      return false;
    }
    runtime.PicaColorTargetsAvailable = true;
    for (const auto &target : runtime.PicaColorTargets) {
      savestateStats.PicaColorTargetBytesCaptured +=
          target.ColorRgba8.size();
    }
    savestateStats.PicaColorTargetsCaptured += runtime.PicaColorTargets.size();
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
      savestateStats.PicaDisplayImageBytesCaptured +=
          image.ColorRgba8.size();
    }
    savestateStats.PicaDisplayImagesCaptured +=
        runtime.PicaPresentationState.DisplayImages.size();
    Oot3dNativeGame::NativeA32SavestateIoResult ioResult;
    if (!Oot3dNativeGame::SaveNativeA32State(
            path, savestateCompatibility, runtime, process, hostServices,
            picaFrontend, submissionQueue, dspHle, &ioResult, &error)) {
      std::cerr << "oot3d_native_game: savestate save failed: " << error
                << '\n';
      return false;
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
    ++savestateStats.LoadCount;
    savestateStats.LoadBytes += ioResult.FileBytes;
    savestateStats.LoadSeconds += SecondsSince(started);
    savestateStats.ReadSeconds += ioResult.IoSeconds;
    savestateStats.DecodeSeconds += ioResult.DecodeSeconds;
    savestateStats.RestoreSeconds += ioResult.RestoreSeconds;
    std::cout << "oot3d_native_game: loaded state at frame " << frameCount
              << " from " << path.string() << '\n';
    return true;
  };

  NativeA32BenchmarkWindow benchmark;
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
              dspStats.FramesMixed, std::span<const int16_t>(audioSamples)
                                  .subspan(frameSampleOffset));
          ++dspStats.FramesMixed;
          dspStats.FramesSuppressed += launch.DisableAudio ? 1U : 0U;
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
          dspStats.PeakMagnitude = std::max(dspStats.PeakMagnitude, magnitude);
          dspStats.NonzeroSamples += sample != 0 ? 1U : 0U;
          const uint16_t bits = static_cast<uint16_t>(sample);
          dspStats.PcmFnv1a64 ^= static_cast<uint8_t>(bits);
          dspStats.PcmFnv1a64 *= 1099511628211ULL;
          dspStats.PcmFnv1a64 ^= static_cast<uint8_t>(bits >> 8U);
          dspStats.PcmFnv1a64 *= 1099511628211ULL;
        }
        dspStats.SamplesMixed += audioSamples.size() / 2U;
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
          ++picaStats.PicaDrainPassCount;
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
                    ++picaStats.CompletedP3dCount;
                  } else if (*fill.Interrupt ==
                             Oot3dNativeGame::Oot3dPicaInterruptId::Ppf) {
                    ++picaStats.CompletedPpfCount;
                  }
                  ++picaStats.RecordedCompletionCount;
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
                  ++picaStats.CompletedPpfCount;
                  ++picaStats.RecordedCompletionCount;
                  signaledPicaCompletion = true;
                }
                displayTransfersByOutput[transfer.Transfer.OutputAddress] =
                    transfer;
                ++picaStats.DisplayTransferCount;
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
              picaStats.NativeMenuBackdropDrawOmissionCount +=
                  usesCommonBackground ? 1U : 0U;
              picaStats.NativeMenuClearDrawOmissionCount +=
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
                ++picaStats.CompletedP3dCount;
              } else if (completion.Interrupt ==
                         Oot3dNativeGame::Oot3dPicaInterruptId::Ppf) {
                ++picaStats.CompletedPpfCount;
              }
              ++picaStats.RecordedCompletionCount;
              signaledPicaCompletion = true;
            }
            lastSubmittedDrawId = draw.Id;
            while (
                displayTransferIndex < displayTransfers.size() &&
                displayTransfers[displayTransferIndex].AfterDrawSubmissionId ==
                    draw.Id) {
              submitDisplayTransfer(displayTransfers[displayTransferIndex++]);
            }
            ++picaStats.DrawCount;
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
        picaStats.MaximumPicaDrainPassesPerRefresh = std::max<uint64_t>(
            picaStats.MaximumPicaDrainPassesPerRefresh, drainPassesThisRefresh);
        picaStats.RefreshesWithNativeDraws += refreshHadNativeDraws ? 1U : 0U;
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
                ++picaStats.SelectedTopTransferCount;
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
                  ++visualStats.SnapshotCount;
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
                      ++visualStats.DirectFramesPresented;
                      ++visualStats.SamplesPresented;
                      visualStats.SampleDrawCount += sample.Draws.size();
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
                      ++visualStats.TransitionCount;
                      visualStats.MatchedDrawCount += transition.MatchedDraws;
                      visualStats.StrictUniqueMatchedDrawCount +=
                          transition.StrictUniqueMatchedDraws;
                      visualStats.StrictOrdinalMatchedDrawCount +=
                          transition.StrictOrdinalMatchedDraws;
                      visualStats.StructuralOrdinalMatchedDrawCount +=
                          transition.StructuralOrdinalMatchedDraws;
                      visualStats.PipelineOrdinalMatchedDrawCount +=
                          transition.PipelineOrdinalMatchedDraws;
                      visualStats.ChangedContinuousDrawCount +=
                          transition.ChangedContinuousDraws;
                      visualStats.ChangedPerInstanceVertexDrawCount +=
                          transition.ChangedPerInstanceVertexDraws;
                      visualStats.ChangedPerVertexDrawCount +=
                          transition.ChangedPerVertexDraws;
                      visualStats.AmbiguousPreviousDrawCount +=
                          transition.AmbiguousPreviousDraws;
                      visualStats.AmbiguousCurrentDrawCount +=
                          transition.AmbiguousCurrentDraws;
                      visualStats.UnmatchedPreviousDrawCount +=
                          transition.UnmatchedPreviousDraws;
                      visualStats.UnmatchedCurrentDrawCount +=
                          transition.UnmatchedCurrentDraws;
                      const double continuityThreshold =
                          visualContinuityTracker.CurrentThreshold();
                      const bool continuousTransition =
                          transitionPrepared &&
                          visualContinuityTracker.Accept(transition);
                      latestVisualTransitionContinuous = continuousTransition;
                      visualStats.DiscontinuityCount +=
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
                        ++visualStats.SamplesPresented;
                        visualStats.SampleDrawCount += sample.Draws.size();
                        visualStats.InterpolatedDrawCount +=
                            sample.InterpolatedDraws;
                        visualStats.InterpolatedPerInstanceVertexDrawCount +=
                            sample.InterpolatedPerInstanceVertexDraws;
                        visualStats.InterpolatedPerVertexDrawCount +=
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
                      ++visualStats.NamespaceResyncCount;
                      ++visualStats.SamplesPresented;
                      visualStats.SampleDrawCount += sample.Draws.size();
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
                  ++visualStats.SamplesPresented;
                  visualStats.SampleDrawCount += sample.Draws.size();
                  visualStats.InterpolatedDrawCount += sample.InterpolatedDraws;
                  visualStats.InterpolatedPerInstanceVertexDrawCount +=
                      sample.InterpolatedPerInstanceVertexDraws;
                  visualStats.InterpolatedPerVertexDrawCount +=
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
                ++visualStats.ReusedSnapshotPresentations;
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
            ++picaStats.NativeFrontendPresentationActiveCount;
            auto selectedBottom =
                displayTransfersByOutput.find(bottomFramebuffer->AddressLeft);
            if (selectedBottom == displayTransfersByOutput.end() &&
                bottomFramebuffer->AddressRight != 0U &&
                bottomFramebuffer->AddressRight !=
                    bottomFramebuffer->AddressLeft) {
              selectedBottom = displayTransfersByOutput.find(
                  bottomFramebuffer->AddressRight);
            }
            picaStats.NativeFrontendBottomTransferHitCount +=
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
                    ++picaStats.NativeBottomFrontendPresentationCount;
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
      benchmark.MeasurementStart = completedFrameTime;
      benchmark.MeasurementEnd = completedFrameTime;
      benchmarkMeasurementStarted = true;
      if (cpuProbeRequested) CpuProbe::Thread.StartAt(CpuProbe::Read());
    } else if (benchmarkMeasurementStarted &&
               runFrameCount > hostArgs.BenchmarkWarmupFrames) {
      const auto currentPhases = phaseTiming.Values();
      std::array<double, 15> delta{};
      for (size_t i=0; i<delta.size(); ++i) {
        delta[i]=currentPhases[i]-phaseBeforeFrame[i];
        benchmark.PhaseSeconds[i]+=delta[i];
      }
      const double nestedGuest=phaseTiming.GuestInsidePicaSubmitSeconds-nestedGuestBeforeFrame;
      benchmark.NestedGuestSeconds+=nestedGuest;
      benchmark.GuestClockSeconds+=phaseTiming.GuestClockSeconds-guestClockBeforeFrame;
      const Oot3dNativeGame::PreBackendBudget budget{
        delta[0],delta[7],nestedGuest,delta[8],delta[2],delta[3],delta[5],delta[6]};
      if(budget.Valid()) benchmark.PreBackendTimes.RecordMilliseconds(budget.Total()*1000.0);
      else ++benchmark.InvalidPreBackendIntervals;
      // Present includes pacing. Visual presentation includes backend/replay.
      // Remove each inclusive scope once; keep unclassified host/UI work visible.
      const double envelope=std::chrono::duration<double>(completedFrameTime-benchmark.MeasurementEnd).count()
          -delta[4]-delta[11]-delta[13];
      benchmark.PreBackendEnvelopeTimes.RecordMilliseconds(envelope*1000.0);
      benchmark.UnattributedSeconds+=envelope-budget.Total();
      if (benchmarkFrameTimes) {
        auto phases = phaseTiming.Values();
        for (size_t i = 0; i < phases.size(); ++i)
          phases[i] = (phases[i] - phaseBeforeFrame[i]) * 1000.0;
        benchmark.SlowFrames.Record(runFrameCount, std::chrono::duration<double, std::milli>(
            completedFrameTime - benchmark.MeasurementEnd).count(), phases);
        benchmarkFrameTimes->RecordMilliseconds(
            std::chrono::duration<double, std::milli>(
                completedFrameTime - benchmark.MeasurementEnd).count());
      }
      benchmark.MeasurementEnd = completedFrameTime;
      ++benchmark.MeasuredFrames;
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

  WriteNativeA32WindowRuntimeSummary({launch,
      controlConfigRuntime, activeTopScreenConfig, scenarioBootstrap, hdFonts,
      hdFontsLoaded, quickStatePath, hostServices, frameRatePolicy,
      sceneViewProbe, playerTimingProbe, temporalEventLedger, playerTemporalBridge,
      widescreenProjection, enableCompiledFunctions, wholeAotAvailable, enableWholeAot,
      enableManualCompiledFunctions, process, uiLifecycleBridge, nativeCandidateDispatch,
      submissionQueue, hostArgs, window, api,
      nativeUiTextureProvider, ocarinaText, n64UiRenderer, inputTimeline,
      phaseTiming, inputDiagnostics, inputConsumerDiagnostics, nativeControlPollingState,
      shaderSourceCache, audioPlayer, processResult, frameCount,
      runFrameCount, presentationFrameCount, nextVblankTick, lateVblankDeadlineCount,
      maximumVblankDeadlineOvershootTicks, savestateStats, presentationScheduler, visualSampleCadence,
      visualContinuityEpoch, picaStats, visualStats, visualInterpolationTiming,
      vblankCount, dspStats, displayTransferEvents, drawEvents,
      visualTransitionEvents, timing, frameCompositionPolicy, realtimePacer,
      visualInterpolationEnabled, audioOutputDiagnostics, pcmContinuityDiagnostics, pcmDumpSamples,
      screenshotState, publishedSceneViewCount, automatedStateSavedAtRunFrame, automatedStateSavedAtGuestFrame,
      benchmark, benchmarkFrameTimes, cpuProbeRequested, benchmarkCpuPhases,
      picaCompositionTracker, topScreenTextureOverridePackPointer, topScreenTextureOverrideRuntime, preparationScreen,
      lastTopScreenPrimitivesBySubsystem, picaPresentationScheduler, parallelPresentation});

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
