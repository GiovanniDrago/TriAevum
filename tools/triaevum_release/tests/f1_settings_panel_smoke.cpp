#include "fast/oot3d/graphics_settings_window.h"
#include "fast/oot3d/graphics_settings_runtime.h"
#include "fast/oot3d/graphics_settings_persistence.h"
#include "oot3d_native_controls_settings_panel.h"
#include "oot3d_top_screen_settings_panel.h"
#include "oot3d_game_language_panel.h"
#include "oot3d_scene_environment.h"
#include "fast/MouseCapturePolicy.h"
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
#include <chrono>
#include <numbers>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

using namespace Fast::Oot3d;
namespace {
struct Item { std::string Label; ImRect Rect; ImGuiID Seed = 0; bool Disabled = false; bool Popup = false; };
struct RawBindingSource final : ThreeDsRecomp::Input::HostButtonSource {
    ThreeDsRecomp::Input::HostBinding Held;
    bool IsKeyboardKeyHeld(ThreeDsRecomp::Input::KeyboardKey key) const noexcept override { return key == Held.KeyboardPrimary; }
    bool IsMouseButtonHeld(ThreeDsRecomp::Input::MouseButton button) const noexcept override { return button == Held.Mouse; }
    bool IsGamepadButtonHeld(ThreeDsRecomp::Input::GamepadButton button) const noexcept override { return button == Held.Gamepad; }
};
std::map<ImGuiID, Item> items;
int assertions = 0;
void Check(bool condition, const std::string& what) {
    ++assertions;
    if (!condition) throw std::runtime_error(what);
}
struct MemoryStore final : GraphicsSettingsPersistencePort {
    nlohmann::json Root;
    int Stores = 0;
    bool Fail = false;
    bool LoadRoot(nlohmann::json& root) override { root = Root; return true; }
    bool StoreGraphics(const nlohmann::json& value) override {
        ++Stores;
        if (Fail) return false;
        Root["Graphics"] = value;
        return true;
    }
};
GraphicsSettingsPanel panel;
std::string loggedPanelText;
bool updateTextureObservations = false;
bool showPanel = true;
constexpr uint64_t firstTextureHash = 0xA100U;
constexpr uint64_t secondTextureHash = 0xB200U;
ImVec2 size(760.0F, 680.0F);
void Frame(bool logText = false) {
    if (updateTextureObservations) {
        auto& catalog = TextureCatalogRuntime::Instance();
        catalog.Observe(firstTextureHash, 0x1000U, 32, 32, 0);
        catalog.Observe(secondTextureHash, 0x2000U, 64, 64, 0);
    }
    items.clear();
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(std::max(1280.0F, size.x + 16.0F),
                            std::max(720.0F, size.y + 16.0F));
    io.DeltaTime = 1.0F / 60.0F;
    ImGui::NewFrame();
    if (showPanel) {
        ImGui::SetNextWindowPos(ImVec2(8, 8));
        ImGui::SetNextWindowSize(size);
        ImGui::Begin("F1 test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        if (logText) ImGui::LogToBuffer(0);
        panel.Draw();
        if (logText) {
            loggedPanelText = GImGui->LogBuffer.c_str();
            ImGui::LogFinish();
        }
        ImGui::End();
    }
    DrawDisplayConfirmation();
    ImGui::Render();
    Check(GImGui->DisabledStackSize == 0, "unbalanced disabled scope");
    Check(GImGui->ColorStack.Size == 0 && GImGui->StyleVarStack.Size == 0,
          "unbalanced style scope");
    for (auto* list : ImGui::GetDrawData()->CmdLists) {
        for (const auto& v : list->VtxBuffer)
            if (!std::isfinite(v.pos.x) || !std::isfinite(v.pos.y))
                throw std::runtime_error("non-finite UI geometry");
    }
}
Item Find(const char* label) {
    std::optional<Item> found;
    for (const auto& [id, item] : items)
        if ((item.Label == label || ImHashStr(label, 0, item.Seed) == id) &&
            item.Rect.GetHeight() > 0.0F) {
            if (item.Popup) return item;
            found = item;
        }
    if (found) return *found;
    std::string available;
    for (const auto& [id, item] : items) available += item.Label + " | ";
    throw std::runtime_error(std::string("Missing UI item: ") + label + " (available: " + available + ")");
}
void Click(const char* label) {
    Frame();
    const auto item = Find(label);
    Check(!item.Disabled, std::string("disabled UI item: ") + label);
    const ImVec2 p(item.Rect.Min.x + std::min(12.0F, item.Rect.GetWidth() / 2.0F),
                   (item.Rect.Min.y + item.Rect.Max.y) / 2.0F);
    const auto bounds = item.Popup ? ImGui::GetIO().DisplaySize : ImVec2(size.x + 8.0F, size.y + 8.0F);
    Check(p.y < bounds.y && p.x < bounds.x, std::string("clipped UI action: ") + label);
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(p.x, p.y);
    Frame();
    io.AddMouseButtonEvent(0, true);
    Frame();
    io.AddMouseButtonEvent(0, false);
    Frame();
    Frame();
}
void Select(const char* combo, const char* option) {
    std::cout << "Select " << combo << " -> " << option << std::endl;
    const auto beforeClick = Find(combo);
    Click(combo);
    if (GImGui->OpenPopupStack.empty())
        std::cerr << "target " << beforeClick.Rect.Min.x << "," << beforeClick.Rect.Min.y
                  << " -> " << beforeClick.Rect.Max.x << "," << beforeClick.Rect.Max.y
                  << "; hovered " << (GImGui->HoveredWindow ? GImGui->HoveredWindow->Name : "none")
                  << "; active id " << GImGui->ActiveId << "\n";
    Check(!GImGui->OpenPopupStack.empty(), std::string("combo did not open: ") + combo);
    Click(option);
}
void EditScalar(const char* label, const char* value) {
    auto& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    Click(label);
    Check(GImGui->TempInputId != 0, std::string("scalar input did not open: ") + label);
    io.AddKeyEvent(ImGuiKey_A, true); Frame();
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false); Frame();
    io.AddInputCharactersUTF8(value); Frame();
    io.AddKeyEvent(ImGuiKey_Enter, true); Frame();
    io.AddKeyEvent(ImGuiKey_Enter, false); Frame(); Frame();
}
void EditText(const char* label, const char* value) {
    Click(label);
    auto& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_A, true); Frame();
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false); Frame();
    io.AddKeyEvent(ImGuiKey_Backspace, true); Frame();
    io.AddKeyEvent(ImGuiKey_Backspace, false); Frame();
    io.AddInputCharactersUTF8(value); Frame(); Frame();
}
void ClickTexture(uint64_t hash, bool reorderDuringClick = false) {
    std::cout << "Select texture " << std::hex << hash << std::dec << std::endl;
    Frame();
    char hashText[20]{};
    std::snprintf(hashText, sizeof(hashText), "%016llX", static_cast<unsigned long long>(hash));
    std::optional<Item> target;
    for (const auto& [id, item] : items)
        if (item.Popup && item.Label.find(hashText) != std::string::npos)
            target = item;
    if (!target) {
        std::string visible;
        for (const auto& [id, item] : items) if (item.Popup) visible += item.Label + " | ";
        throw std::runtime_error(std::string("Texture ") + hashText + " absent from open catalog: " + visible);
    }
    const ImVec2 p(target->Rect.Min.x + 12.0F, target->Rect.GetCenter().y);
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(p.x, p.y);
    Frame();
    if (reorderDuringClick) {
        // Move the second texture to the top of the live ranking while the
        // pointer is already over its row in the open popup.
        for (int observation = 0; observation < 1000; ++observation)
            TextureCatalogRuntime::Instance().Observe(secondTextureHash, 0x2000U, 64, 64, 0);
    }
    io.AddMouseButtonEvent(0, true);
    Frame();
    io.AddMouseButtonEvent(0, false);
    Frame(); Frame();
    Check(GImGui->OpenPopupStack.empty(), "texture selection lost while observations changed");
}
void AllCapabilities(bool available) {
    auto& runtime = GraphicsSettingsRuntime::Instance();
    for (unsigned value = 0; value <= static_cast<unsigned>(kLastGraphicsCapability); ++value)
        runtime.SetCapability(static_cast<GraphicsCapability>(value), available, "test capability");
}

void CheckControlPersistence() {
    using namespace Oot3dNativeGame;
    const auto root = std::filesystem::temp_directory_path() /
        ("triaevum-controls-smoke-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Check(std::filesystem::create_directory(root), "could not create isolated config fixture");
    struct Cleanup { std::filesystem::path Root; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(Root, ec); } } cleanup{root};
    auto controls = std::make_shared<NativeControlConfigRuntime>(root / "controls.json", NativeControlDefaults());
    auto top = std::make_shared<TopScreenUiConfigRuntime>(root / "topscreen.json", TopScreenUiConfig{});
    InstallGraphicsSettingsPanelTabs({CreateNativeControlsSettingsPanel(controls, top)});
    size = ImVec2(760, 680);
    Click("Controls");
    Click("Devices");
    Click("Keyboard");
    Click("Revert changes");
    Check(controls->Snapshot().Config.KeyboardEnabled, "cannot undo before first save");
    Click("Keyboard");
    Click("Camera");
    // The previous panel may have saved this section collapsed in ImGui.
    Frame();
    bool freeCameraVisible = false;
    for (const auto& [id, item] : items) freeCameraVisible |= item.Label == "Enabled##freecam";
    if (!freeCameraVisible) Click("Free camera");
    Click("Enabled##freecam");
    Click("Save controls");
    NativeControlConfig saved;
    TopScreenUiConfig savedTop;
    Check(LoadNativeControlConfig(root / "controls.json", &saved) && !saved.KeyboardEnabled,
          "Save controls did not persist live input");
    Check(LoadTopScreenUiConfig(root / "topscreen.json", &savedTop) && savedTop.FreeCameraEnabled,
          "Save controls did not persist camera settings");
    Click("Enabled##freecam");
    auto externalHud = top->Snapshot().Config;
    externalHud.HudScale = 0.65F;
    externalHud.HudMarginX = 8;
    top->Preview(externalHud);
    Click("Devices");
    Click("Keyboard");
    Click("Revert changes");
    Check(controls->Snapshot().Config == saved, "Revert did not restore saved bindings");
    Check(top->Snapshot().Config.FreeCameraEnabled && top->Snapshot().Config.HudScale == 0.65F &&
          top->Snapshot().Config.HudMarginX == 8, "Revert overwrote unrelated live HUD configuration");
    Click("Camera");
    Click("Enabled##freecam");
    Click("Devices");
    Click("Keyboard");
    const auto live = controls->Snapshot().Config;
    const auto liveTop = top->Snapshot().Config;
    std::ofstream(root / "topscreen.json") << "invalid json";
    Click("Revert changes");
    Check(controls->Snapshot().Config == live && top->Snapshot().Config == liveTop,
          "failed Revert partially modified live configuration");
    Frame(true);
    Check(loggedPanelText.find("parse") != std::string::npos, "Revert error was not visible");
    std::filesystem::create_directory(root / "not-a-file");
    auto blocked = std::make_shared<NativeControlConfigRuntime>(root / "not-a-file", NativeControlDefaults());
    InstallGraphicsSettingsPanelTabs({CreateNativeControlsSettingsPanel(blocked, nullptr)});
    Frame(); Frame();
    Click("Save controls");
    Frame(true);
    Check(loggedPanelText.find("cannot") != std::string::npos, "Save error was not visible");
    InstallGraphicsSettingsPanelTabs({});
}

void CheckSceneEnvironment() {
    using namespace Oot3dNativeGame;
    const auto root = std::filesystem::temp_directory_path() /
        ("triaevum-environment-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    struct Cleanup { std::filesystem::path Root; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(Root, ec); } } cleanup{root};
    auto& graphics = GraphicsSettingsRuntime::Instance();
    AllCapabilities(true);
    auto settings = graphics.Snapshot();
    settings.Preset = GraphicsPreset::Custom;
    settings.Effects.SurfaceLighting = SurfaceLightingMode::PerPixelPbr;
    graphics.Apply(settings);
    auto environment = std::make_shared<SceneEnvironmentRuntime>(root, [&](const auto& value) {
        graphics.PublishSceneEnvironment(value);
    });
    environment->ObserveScene(0x55);
    InstallGraphicsSettingsPanelTabs({CreateSceneEnvironmentPanel(environment)});
    size = ImVec2(760, 1450);
    Frame(); Frame(); Click("Environment");
    Select("Shadow mode", "Virtual Shadow Maps");
    EditScalar("Shadow strength", "4");
    Check(graphics.Snapshot().Effects.DirectionalShadows.Strength == 4.0F,
          "shadow strength above one did not apply live");
    EditScalar("Shadow strength", "0");
    Check(graphics.Snapshot().Effects.DirectionalShadows.Strength == 0.0F,
          "zero shadow strength did not apply live");
    graphics.SetCapability(GraphicsCapability::NriDirectionalShadows, false, "test unavailable");
    Frame();
    Select("Shadow mode", "Off");
    Check(graphics.Snapshot().Effects.DirectionalShadows.Mode == DirectionalShadowMode::Off,
          "cannot disable unsupported shadows");
    Click("Shadow mode");
    Check(Find("Virtual Shadow Maps").Disabled, "unsupported shadow mode selectable");
    Click("Off");
    AllCapabilities(true);
    Select("Shadow mode", "Virtual Shadow Maps");
    settings = graphics.Snapshot();
    settings.Effects.SurfaceLighting = SurfaceLightingMode::Native;
    graphics.Apply(settings);
    Frame();
    Check(graphics.Snapshot().Effects.DirectionalShadows.Mode == DirectionalShadowMode::VirtualShadowMaps &&
          graphics.SnapshotForRendering().Value.Effects.DirectionalShadows.Mode == DirectionalShadowMode::Off,
          "native lighting did not bypass VSM while preserving its preference");
    Click("Shadow mode");
    Check(Find("Virtual Shadow Maps").Disabled, "native lighting can enable VSM");
    settings.Effects.SurfaceLighting = SurfaceLightingMode::NativePbr;
    graphics.Apply(settings);
    Frame();
    Check(Find("Virtual Shadow Maps").Disabled &&
              graphics.SnapshotForRendering().Value.Effects.DirectionalShadows.Mode == DirectionalShadowMode::Off &&
              graphics.Snapshot().Effects.DirectionalShadows.Mode == DirectionalShadowMode::VirtualShadowMaps,
          "Native PBR enabled VSM or erased its saved preference");
    Click("Off");
    settings = graphics.Snapshot();
    settings.Effects.SurfaceLighting = SurfaceLightingMode::PerPixel;
    graphics.Apply(settings);
    Frame();
    Select("Shadow mode", "Virtual Shadow Maps");
    Check(graphics.SnapshotForRendering().Value.Effects.DirectionalShadows.Mode == DirectionalShadowMode::VirtualShadowMaps,
          "per-pixel lighting did not enable VSM");
    graphics.ToggleNativePresentationOverride();
    Check(graphics.SnapshotForRendering().Value.Effects.DirectionalShadows.Mode == DirectionalShadowMode::Off &&
          graphics.Snapshot().Effects.DirectionalShadows.Mode == DirectionalShadowMode::VirtualShadowMaps,
          "F2 did not bypass VSM without erasing its preference");
    Check(graphics.SnapshotForRendering().Value.Effects.SurfaceLighting == SurfaceLightingMode::Native &&
              graphics.Snapshot().Effects.SurfaceLighting == SurfaceLightingMode::PerPixel,
          "F2 did not force Native lighting while preserving the configured model");
    graphics.ToggleNativePresentationOverride();
    Check(graphics.SnapshotForRendering().Value.Effects.SurfaceLighting == SurfaceLightingMode::PerPixel,
          "F2 did not restore per-pixel lighting");
    settings = graphics.Snapshot();
    settings.Effects.SurfaceLighting = SurfaceLightingMode::PerPixelPbr;
    graphics.Apply(settings);
    Check(graphics.SnapshotForRendering().Value.Effects.DirectionalShadows.Mode == DirectionalShadowMode::VirtualShadowMaps,
          "per-pixel PBR lighting did not restore VSM");
    Select("Point shadow mode", "Cube maps");
    Check(graphics.SnapshotForRendering().Value.Effects.PointShadows.Mode == PointShadowMode::CubeMaps,
          "point shadows did not enable live");
    Select("Point shadow face resolution", "1024 (24 MiB per light)");
    EditScalar("Shadowed point lights", "6");
    EditScalar("Point shadow reach", "50");
    EditScalar("Point shadow strength", "0.25");
    {
        const auto points = graphics.Snapshot().Effects.PointShadows;
        Check(points.Resolution == 1024U && points.MaxLights == 6U && std::abs(points.Range - 0.5F) < 1.0e-5F &&
                  std::abs(points.Strength - 0.25F) < 1.0e-5F,
              "point shadow controls did not apply live");
    }
    settings = graphics.Snapshot();
    settings.Effects.SurfaceLighting = SurfaceLightingMode::Native;
    graphics.Apply(settings);
    Frame();
    Check(graphics.Snapshot().Effects.PointShadows.Mode == PointShadowMode::CubeMaps &&
              graphics.SnapshotForRendering().Value.Effects.PointShadows.Mode == PointShadowMode::Off,
          "native lighting did not bypass point shadows while preserving their preference");
    Click("Point shadow mode");
    Check(Find("Cube maps").Disabled, "native lighting can enable point shadows");
    Click("Off");
    Check(graphics.Snapshot().Effects.PointShadows.Mode == PointShadowMode::Off, "cannot disable unsupported point shadows");
    settings = graphics.Snapshot();
    settings.Effects.SurfaceLighting = SurfaceLightingMode::PerPixelPbr;
    graphics.Apply(settings);
    Frame();
    Click("Use scene environment");
    Check(Find("Native ambient contribution").Disabled, "scene reference did not own the ambient baseline");
    Click("Preserve scene brightness");
    EditScalar("Ambient intensity", "0.12");
    EditScalar("Native scene direct contribution", "0.25");
    EditScalar("Native local contribution", "0.7");
    EditScalar("Native ambient contribution", "0.15");
    EditScalar("Native fog contribution", "0.4");
    EditScalar("Light intensity", "1.4");
    EditScalar("Azimuth", "90");
    EditScalar("Elevation", "30");
    const auto rendered = graphics.SnapshotForRendering().Environment;
    Check(rendered.Enabled && rendered.AmbientIntensity == 0.12F &&
          rendered.NativeDirectStrength == 0.25F && rendered.NativeLocalStrength == 0.7F &&
          rendered.NativeAmbientStrength == 0.15F &&
          rendered.FogStrength == 0.4F && rendered.Lights[0].Intensity == 1.4F,
          "scene controls did not reach the live rendering snapshot");
    Check(std::abs(rendered.Lights[0].DirectionTowardSource[0] - std::sqrt(0.75F)) < 0.0001F &&
          std::abs(rendered.Lights[0].DirectionTowardSource[1] - 0.5F) < 0.0001F &&
          std::abs(rendered.Lights[0].DirectionTowardSource[2]) < 0.0001F,
          "scene light direction does not follow the displayed world axes");
    using KeyDirection = Fast::Renderer3ds::PicaEnvironmentKeyDirection;
    Click("Key light direction"); Click("Ambient light orientation");
    Check(graphics.SnapshotForRendering().Environment.KeyDirection == KeyDirection::SceneAmbient,
          "key light did not follow the ambient light orientation");
    Click("Key light direction"); Click("Main scene light");
    EditScalar("Azimuth offset", "30");
    EditScalar("Elevation offset", "-10");
    EditScalar("Fallback elevation", "30");
    {
        const auto keyed = graphics.SnapshotForRendering().Environment;
        const float degree = std::numbers::pi_v<float> / 180.0F;
        Check(keyed.KeyDirection == KeyDirection::SceneMain &&
                  std::abs(keyed.KeyAzimuthOffset - 30.0F * degree) < 0.0001F &&
                  std::abs(keyed.KeyElevationOffset + 10.0F * degree) < 0.0001F &&
                  keyed.WorldUp == std::array{0.0F, 1.0F, 0.0F} &&
                  keyed.WorldAzimuthZero == std::array{0.0F, 0.0F, 1.0F} &&
                  std::abs(keyed.Lights[0].DirectionTowardSource[1] - 0.5F) < 0.0001F,
              "key light offsets, axes or fallback did not reach the rendering snapshot");
    }
    Click("Fill light"); EditScalar("Light intensity", "0.3");
    EditScalar("Navi intensity", "2.5");
    EditScalar("Fairy radius scale", "1.75");
    Click("Fairy point lights");
    Click("Preserve scene brightness");
    Check(graphics.SnapshotForRendering().Environment.PreserveSceneBrightness &&
              Find("Native ambient contribution").Disabled,
          "scene brightness reference did not apply independently of native source gains");
    Click("Save scene environment");
    const auto saved = environment->Snapshot();
    Check(saved.FileExists && !saved.Dirty && saved.Error.empty(), "scene environment was not saved");
    Check(saved.Profile.KeyDirection == KeyDirection::SceneMain && saved.Profile.KeyAzimuthOffset == 30.0F &&
              saved.Profile.KeyElevationOffset == -10.0F,
          "the saved profile lost the key light direction");
    Click("Use native environment");
    Check(!graphics.SnapshotForRendering().Environment.Enabled, "native environment restore was not immediate");
    Check(graphics.SnapshotForRendering().Environment.PointLights[0].Intensity == 2.5F &&
              !graphics.SnapshotForRendering().Environment.PointLights[1].Enabled,
          "using native environment erased independent scene point-light controls");
    const auto inactiveEnvironment = graphics.SnapshotForRendering().Environment;
    auto inactiveEdit = environment->Snapshot().Profile;
    inactiveEdit.AmbientIntensity = 1.7F;
    inactiveEdit.Lights[0].Azimuth = -75.0F;
    environment->Preview(inactiveEdit);
    Check(graphics.SnapshotForRendering().Environment == inactiveEnvironment,
          "disabled profile edits changed rendering inputs and invalidated history");
    Click("Reload scene environment");
    Check(environment->Snapshot().Profile == saved.Profile, "scene profile reload lost authored settings");
    settings = graphics.Snapshot();
    settings.Effects.SurfaceLighting = SurfaceLightingMode::NativePbr;
    graphics.Apply(settings);
    const auto nativePbrEnvironment = graphics.SnapshotForRendering().Environment;
    Check(!nativePbrEnvironment.Enabled && environment->Snapshot().Profile.Enabled,
          "Native PBR applied the scene environment or erased its profile");
    auto nativePbrEdit = saved.Profile;
    nativePbrEdit.NativeDirectStrength = 0;
    nativePbrEdit.NativeLocalStrength = 0;
    nativePbrEdit.PointLights[0].Enabled = !nativePbrEdit.PointLights[0].Enabled;
    environment->Preview(nativePbrEdit);
    Check(graphics.SnapshotForRendering().Environment == nativePbrEnvironment,
          "Native PBR consumed scene or fairy override edits");
    environment->Preview(saved.Profile);
    settings.Effects.SurfaceLighting = SurfaceLightingMode::PerPixelPbr;
    graphics.Apply(settings);
    graphics.ToggleNativePresentationOverride();
    Check(!graphics.SnapshotForRendering().Environment.Enabled && environment->Snapshot().Profile.Enabled,
          "F2 did not bypass scene relighting without erasing its profile");
    graphics.ToggleNativePresentationOverride();
    Check(graphics.SnapshotForRendering().Environment.Enabled, "F2 did not restore the authored environment");
    environment->ObserveScene(0x56);
    Check(!graphics.SnapshotForRendering().Environment.Enabled, "previous scene's environment leaked across a transition");
    environment->ObserveScene(0x55);
    Check(environment->Snapshot().Profile == saved.Profile, "returning to scene did not restore its file");
    const auto backup = root / "saved.json";
    std::filesystem::rename(saved.Path, backup);
    std::filesystem::create_directory(saved.Path);
    Check(!environment->Save() && !environment->Snapshot().Error.empty(), "profile save failure was hidden");
    std::filesystem::remove(saved.Path);
    { std::ofstream file(saved.Path); file << "{invalid"; }
    Check(!environment->Reload() && environment->Snapshot().Profile == saved.Profile,
          "malformed reload replaced the last valid live environment");
    environment->ObserveScene(0x56);
    environment->ObserveScene(0x55);
    Check(!graphics.SnapshotForRendering().Environment.Enabled && !environment->Snapshot().Error.empty(),
          "invalid destination profile inherited another scene's lights");
    std::filesystem::remove(saved.Path);
    std::filesystem::rename(backup, saved.Path);
    Click("Reload scene environment");
    nlohmann::json originalProfile;
    { std::ifstream file(saved.Path); file >> originalProfile; }
    for (int version : {1, 2, 3}) {
        auto legacy = originalProfile;
        legacy["version"] = version;
        if (version < 3) legacy.erase("nativeLocalStrength");
        legacy.erase("preserveSceneBrightness");
        if (version == 1) legacy.erase("pointLights");
        { std::ofstream file(saved.Path); file << legacy; }
        Click("Reload scene environment");
        const auto migrated = environment->Snapshot();
        Check(migrated.Error.empty() && !migrated.Dirty &&
                  migrated.Profile.NativeDirectStrength == 0.25F &&
                  migrated.Profile.NativeLocalStrength == (version < 3 ? 0.25F : 0.7F) &&
                  !migrated.Profile.PreserveSceneBrightness,
              "legacy profile lost its native direct/local gain");
        Check(migrated.Profile.PointLights[1].Enabled == (version == 1),
              "legacy profile lost point-group defaults or saved Off");
        nlohmann::json unchanged;
        { std::ifstream file(saved.Path); file >> unchanged; }
        Check(unchanged == legacy, "loading a legacy profile rewrote the user's file");
    }
    graphics.SetCapability(GraphicsCapability::NativeSurfaceLighting, false, "test unavailable");
    Frame(); Click("Use native environment");
    Check(!environment->Snapshot().Profile.Enabled, "capability loss prevented the native off control");
    Click("Navi point lights");
    Check(!graphics.SnapshotForRendering().Environment.PointLights[0].Enabled,
          "capability loss prevented disabling Navi point lights");
    environment->ObserveScene(std::nullopt);
    AllCapabilities(true);
}

void CheckPbrMaterialEditor() {
    using namespace ::Oot3d::Renderer;
    auto& packs = AzaharTexturePackRuntime::Instance();
    auto& catalog = TextureCatalogRuntime::Instance();
    const auto root = std::filesystem::temp_directory_path() /
        ("triaevum-material-editor-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto packRoot = root / "pack";
    const auto inputs = root / "inputs";
    std::filesystem::create_directories(packRoot);
    std::filesystem::create_directory(inputs);
    struct Cleanup {
        std::filesystem::path Root;
        ~Cleanup() {
            auto& runtime = AzaharTexturePackRuntime::Instance();
            runtime.Configure({});
            runtime.WaitForPendingLoads();
            TextureCatalogRuntime::Instance().Clear();
            std::error_code error;
            std::filesystem::remove_all(Root, error);
        }
    } cleanup{root};
    const auto image = [&](const char* name, std::array<uint8_t, 4> pixel, uint32_t width = 2) {
        std::vector<uint8_t> pixels(width * 2 * 4);
        for (size_t offset = 0; offset < pixels.size(); offset += 4)
            std::copy(pixel.begin(), pixel.end(), pixels.begin() + offset);
        WriteAzaharTexturePng(inputs / name, width, 2, pixels);
    };
    image("normal.png", {128, 128, 255, 255});
    image("orm.png", {255, 51, 0, 255});
    image("base.png", {201, 127, 51, 255});
    image("emissive.png", {25, 35, 45, 255});
    image("height.png", {64, 64, 64, 255});
    image("occlusion.png", {17, 23, 29, 255});
    image("roughness.png", {41, 63, 47, 255});
    image("metallic.png", {97, 101, 129, 255});
    image("wrong-size.png", {7, 11, 13, 255}, 4);
    {
        std::ofstream other(packRoot / "000000000000DCBA.material.json");
        other << R"({"schema":"triaevum.pbr-material","version":1,"specularLevel":0.25})";
    }
    packs.Configure({.LoadCustomTextures = true, .LoadDirectory = packRoot});
    packs.WaitForPendingLoads();
    catalog.Clear();
    // Catalog/FNV identity deliberately differs from the Azahar/native-byte identity.
    catalog.Observe(0x12345678, 0x1000, 32, 32, 0);
    catalog.AssociateMaterial(0x12345678, 32, 32, 0, 0xABCD, packs.Snapshot().PackUsesNewHash);
    size = ImVec2(1000, 1350);
    Click("Material editor");
    Click("Create from texture");
    Click("Texture catalog selection");
    ClickTexture(0x12345678);
    Click("Create / edit selected texture");
    Click("Material");
    EditScalar("Normal strength", "3.5");
    EditScalar("Glossiness strength", "0.6");
    EditScalar("Minimum roughness", "0.4");
    EditScalar("Dielectric reflectance", "1.5");
    EditScalar("Specular weight", "0.4");
    EditScalar("Screen-space reflection weight", "1.5");
    EditScalar("Height depth (UV)", "0.05");
    Click("UV seam softening");
    Click("Enable UV seam softening");
    EditScalar("Seam strength", "0.75");
    EditScalar("Seam width (% edge)", "12.5");
    Click("Soften normals");
    Click("Soften emission");
    Click("UV seam softening");
    Click("Save material");
    packs.WaitForPendingLoads();
    Frame();
    const auto descriptor = packRoot / "000000000000ABCD.material.json";
    Check(std::filesystem::is_regular_file(descriptor) &&
              !std::filesystem::exists(packRoot / "0000000012345678.material.json"),
          "creation used the catalog hash instead of the native texture's Azahar identity");
    const auto resolved = [&](uint64_t hash) {
        (void)packs.ResolveMaterialOrQueue(hash, packs.Generation());
        packs.WaitForPendingLoads();
        return packs.ResolveMaterialOrQueue(hash, packs.Generation());
    };
    auto material = resolved(0xABCD);
    Check(material.Material && material.Material->Parameters.NormalScale == 3.5F &&
              material.Material->Parameters.GlossinessStrength == 0.6F &&
              material.Material->Parameters.MinimumRoughness == 0.4F &&
              material.Material->Parameters.SpecularLevel == 1.5F &&
              material.Material->Parameters.SpecularWeight == 0.4F &&
              material.Material->Parameters.ScreenSpaceReflectionWeight == 1.5F &&
              material.Material->Parameters.HeightScale == 0.05F,
          "saved editor parameters did not reach the material loader");
    Check(material.Material->Parameters.SeamSoftening &&
              material.Material->Parameters.SeamStrength == 0.75F &&
              material.Material->Parameters.SeamWidth == 0.125F &&
              material.Material->Parameters.SeamChannels == 62U,
          "saved seam controls did not reach the material loader");
    Click("UV seam softening");
    EditScalar("Seam strength", "0");
    Click("Soften height");
    Click("UV seam softening");
    EditScalar("Dielectric reflectance", "0.5");
    EditScalar("Specular weight", "0");
    EditScalar("Screen-space reflection weight", "2");
    material = resolved(0xABCD);
    Check(material.Preview && material.Preview->SpecularLevel == 0.5F &&
              material.Preview->SpecularWeight == 0.0F &&
              material.Preview->ScreenSpaceReflectionWeight == 2.0F &&
              material.Material->Parameters.SpecularWeight == 0.4F &&
              material.Material->Parameters.ScreenSpaceReflectionWeight == 1.5F &&
              material.Material->Parameters.SpecularLevel == 1.5F,
          "selected material preview either failed or wrote unsaved values to disk");
    Check(material.Preview->SeamStrength == 0 && material.Preview->SeamChannels == 30U &&
              material.Material->Parameters.SeamStrength == 0.75F &&
              material.Material->Parameters.SeamChannels == 62U,
          "live seam preview changed persisted channel selection or strength");
    const auto other = resolved(0xDCBA);
    Check(other.Material && !other.Preview && other.Material->Parameters.SpecularLevel == 0.25F &&
              other.Material->Parameters.SpecularWeight == 1.0F &&
              other.Material->Parameters.ScreenSpaceReflectionWeight == 1.0F,
          "editing one material changed another material");
    Select("Existing PBR material", "000000000000DCBA");
    Click("Keep editing");
    Click("Save material");
    packs.WaitForPendingLoads();
    Frame();
    Select("Material view", "Roughness");
    Check(resolved(0xABCD).Preview->DebugView == Fast::Renderer3ds::PicaPbrDebugView::Roughness,
          "selected-material diagnostic preview was not published");
    auto& graphics = GraphicsSettingsRuntime::Instance();
    graphics.ToggleNativePresentationOverride();
    Frame();
    Check(graphics.SnapshotForRendering().Value.Effects.SurfaceLighting == SurfaceLightingMode::Native,
          "F2 did not bypass material shading");
    graphics.ToggleNativePresentationOverride();
    Frame();
    Check(resolved(0xABCD).Preview->SpecularLevel == 0.5F &&
              resolved(0xABCD).Preview->SpecularWeight == 0.0F &&
              resolved(0xABCD).Preview->ScreenSpaceReflectionWeight == 2.0F,
          "F2 lost the authored material preview");
    Select("Material view", "Shaded");
    AllCapabilities(false);
    Frame();
    Check(!Find("Minimum roughness").Disabled, "material authoring disappeared when renderer capabilities were unavailable");
    Click("UV seam softening");
    Check(!Find("Enable UV seam softening").Disabled,
          "seam Off control became inaccessible after capability loss");
    Click("Enable UV seam softening");
    Check(!resolved(0xABCD).Preview->SeamSoftening,
          "seam Off control did not disable the material preview");
    Click("UV seam softening");
    AllCapabilities(true);
    Click("Images");
    Click("Base color");
    Click("Browse image");
    EditText("Folder", inputs.string().c_str());
    Click("Open folder");
    Click("base.png");
    packs.WaitForPendingLoads();
    Frame();
    const auto loadImage = [&](const char* path) {
        EditText("Image path", (inputs / path).string().c_str());
        Click("Load image");
        packs.WaitForPendingLoads();
        Frame(); Frame();
    };
    Click("Normal"); loadImage("normal.png");
    Click("Emissive"); loadImage("emissive.png");
    Click("Height"); loadImage("height.png");
    Click("ORM"); loadImage("orm.png");
    Click("Save material");
    packs.WaitForPendingLoads();
    Frame();
    material = resolved(0xABCD);
    Check(material.Material->ColorReplacement && (*material.Material->ColorReplacement->Rgba8)[0] == 201 &&
              (*material.Material->Textures[0]->Rgba8)[2] == 255 &&
              (*material.Material->Textures[1]->Rgba8)[1] == 51 &&
              (*material.Material->Textures[2]->Rgba8)[2] == 45 &&
              (*material.Material->Textures[3]->Rgba8)[0] == 64,
          "an imported PBR slot did not survive save and loader publication");
    Select("ORM input", "Separate channels");
    Click("Occlusion"); loadImage("occlusion.png"); Select("Source channel", "R");
    Click("Roughness"); loadImage("roughness.png"); Select("Source channel", "G");
    Click("Metallic"); loadImage("metallic.png"); Select("Source channel", "B");
    Click("Preview packed ORM");
    Click("Save material");
    packs.WaitForPendingLoads();
    Frame();
    material = resolved(0xABCD);
    const auto packed = material.Material->Textures[1];
    Check(packed && (*packed->Rgba8)[0] == 17 && (*packed->Rgba8)[1] == 63 &&
              (*packed->Rgba8)[2] == 129 && (*packed->Rgba8)[3] == 255,
          "separate ORM channels were swizzled, gamma-converted, or not exported as one image");
    auto readJson = [&] {
        std::ifstream file(descriptor);
        return nlohmann::json::parse(file);
    };
    const auto saved = readJson();
    Check(std::filesystem::is_regular_file(descriptor.parent_path() / saved["orm"]["texture"].get<std::string>()),
          "saved descriptor does not reference the packed ORM export");
    loadImage("wrong-size.png");
    Click("Save material");
    Check(readJson() == saved, "mismatched ORM dimensions replaced the last valid material");
    loadImage("metallic.png");
    {
        std::ofstream broken(inputs / "broken.png");
        broken << "not an image";
    }
    loadImage("broken.png");
    Click("Save material");
    Check(readJson() == saved, "failed image import corrupted the material descriptor");
    loadImage("metallic.png");
    Click("Material");
    EditScalar("Dielectric reflectance", "0.8");
    std::filesystem::rename(descriptor, descriptor.string() + ".backup");
    std::filesystem::create_directory(descriptor);
    Click("Save material");
    Check(resolved(0xABCD).Preview->SpecularLevel == 0.8F &&
              std::filesystem::is_regular_file(descriptor.string() + ".backup"),
          "failed descriptor write discarded the live draft");
    std::filesystem::remove(descriptor);
    std::filesystem::rename(descriptor.string() + ".backup", descriptor);
    Click("Save material");
    packs.WaitForPendingLoads();
    Frame();
    Check(resolved(0xABCD).Material->Parameters.SpecularLevel == 0.8F,
          "retrying material save did not commit the retained draft");
    Click("Reset material parameters");
    Check(resolved(0xABCD).Preview->SpecularWeight == 1.0F &&
              resolved(0xABCD).Preview->ScreenSpaceReflectionWeight == 1.0F,
          "reset did not restore neutral material reflection weights");
    Check(!resolved(0xABCD).Preview->SeamSoftening,
          "reset left auxiliary seam blending enabled");
    Select("Existing PBR material", "000000000000DCBA");
    Click("Discard and select");
    Check(resolved(0xABCD).Material->Parameters.SpecularLevel == 0.8F && !resolved(0xABCD).Preview,
          "discarding a draft changed the saved material or left its preview active");
    Select("Existing PBR material", "000000000000ABCD");
    packs.WaitForPendingLoads();
    Frame();
    Check(resolved(0xABCD).Preview->MinimumRoughness == 0.4F,
          "selecting an existing material lost its authored roughness floor");
    Click("Renderer");
    Click("Lighting");
    size = ImVec2(760, 1100);
    Frame(); Frame();
}

void CheckGameSurfaceMouseResume() {
    Fast::MouseCapturePolicy policy;
    Check(policy.Request(true), "initial mouse capture denied");
    policy.Release();
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(100, 100);
    for (int frame = 0; frame < 3; ++frame) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(640, 480));
        ImGui::Begin("Main Game", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground);
        ImGui::End();
        ImGui::Render();
    }
    Check(io.WantCaptureMouse, "game viewport did not reproduce generic ImGui capture");
    Check(!policy.Request(true) && !policy.ResumeClick(true), "a real F1 window allowed recapture");
    Check(policy.ResumeClick(false) && policy.Request(true),
          "game-surface-only ImGui capture blocked resuming gameplay");
    Check(policy.ConsumeClickRelease(), "recapture click would leak a game action");
}
}

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb,
                               const ImGuiLastItemData*) {
    if (id == 0) return;
    items[id].Rect = bb;
    items[id].Popup = (ctx->CurrentWindow->Flags & ImGuiWindowFlags_Popup) != 0;
    items[id].Seed = ctx->CurrentWindow->IDStack.empty()
        ? ctx->CurrentWindow->ID : ctx->CurrentWindow->IDStack.back();
    items[id].Disabled = (ctx->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
    items[id].Label = label ? label : "";
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID id) {
    auto it = items.find(id);
    return it == items.end() ? nullptr : it->second.Label.c_str();
}

int main() try {
    PresentationSettingsTransaction delayedConfirmation(500);
    PresentationSettingsValue previousDisplay;
    auto nextDisplay = previousDisplay;
    nextDisplay.Window = WindowMode::Borderless;
    Check(delayedConfirmation.Begin(previousDisplay, nextDisplay), "display transaction did not begin");
    Check(delayedConfirmation.MarkApplied(nextDisplay, 100, true), "display transaction did not apply");
    Check(!delayedConfirmation.Advance(100000), "hidden confirmation expired before it could be seen");
    Check(delayedConfirmation.ConfirmationVisible(100000), "visible confirmation did not start the timer");
    Check(!delayedConfirmation.ConfirmationVisible(100100), "redrawing the confirmation renewed its timer");
    Check(!delayedConfirmation.Advance(100499) && delayedConfirmation.Advance(100500),
          "visible confirmation lost the automatic rollback deadline");
    auto store = std::make_shared<MemoryStore>();
    auto initial = GraphicsSettingsService::Preset(GraphicsPreset::Custom);
    store->Root["Graphics"] = SerializeGraphicsSettings(initial);
    InstallGraphicsSettingsPersistencePort(store);
    auto& runtime = GraphicsSettingsRuntime::Instance();
    AllCapabilities(true);
    auto controls = std::make_shared<Oot3dNativeGame::NativeControlConfigRuntime>(
        std::filesystem::path{}, Oot3dNativeGame::NativeControlPreset(
            Oot3dNativeGame::NativeControlProfile::KeyboardMouse));
    auto topScreen = std::make_shared<Oot3dNativeGame::TopScreenUiConfigRuntime>(
        std::filesystem::path{}, Oot3dNativeGame::TopScreenUiConfig{});
    InstallGraphicsSettingsPanelTabs({
        Oot3dNativeGame::CreateNativeControlsSettingsPanel(controls, topScreen),
        Oot3dNativeGame::CreateTopScreenSettingsPanel(topScreen)});
    ImGui::CreateContext();
    GImGui->TestEngineHookItems = true;
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int width, height;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    Frame(); Frame();

    auto requestedDisplay = runtime.Snapshot();
    requestedDisplay.OutputWidth = 3840;
    requestedDisplay.OutputHeight = 2160;
    runtime.Apply(requestedDisplay);
    runtime.AcknowledgePresentationApplied(requestedDisplay);
    runtime.PublishDisplayMetrics({1920, 1080, 960, 540, 0.5F, WindowMode::Windowed});
    Frame(true);
    Check(loggedPanelText.find("Output framebuffer: 1920 x 1080") != std::string::npos &&
          loggedPanelText.find("Scene image: 960 x 540") != std::string::npos,
          "F1 labels the requested resolution as the actual renderer extent");
    Check(runtime.Snapshot().OutputWidth == 3840, "observed extent overwrote the requested resolution");
    runtime.Apply(initial);
    runtime.AcknowledgePresentationApplied(initial);
    Frame();

    Check(!runtime.NativePresentationOverrideActive(), "F2 must start inactive");
    for (const auto toon : {ToonMode::Off, ToonMode::PostProcessPreview, ToonMode::PicaMaterial}) {
        for (const auto reflections : {ReflectionMode::Off, ReflectionMode::HiZ, ReflectionMode::FidelityFxSssr}) {
            auto configured = runtime.Snapshot();
            configured.Preset = GraphicsPreset::Custom;
            configured.Grass.Quality = GrassQuality::Custom;
            configured.Grass.Generation.InstancesPerSquareMeter = 1048.8F;
            configured.Effects.Toon = toon;
            configured.Effects.ToonStyle.OutlineEnabled = true;
            configured.Effects.AmbientOcclusion = AmbientOcclusionMode::Gtao;
            configured.Effects.Reflections = reflections;
            configured.FrameRate = FrameRateMode::Interpolated2x;
            configured.FovMultiplier = 1.1F;
            runtime.Apply(configured);
            const auto before = runtime.SnapshotWithRevision();
            const auto serialized = SerializeGraphicsSettings(before.Value);
            const auto storedBefore = store->Root;
            const int savesBefore = store->Stores;
            runtime.ToggleNativePresentationOverride();
            const auto effective = runtime.SnapshotForRendering();
            auto expectedOff = before.Value;
            expectedOff.Grass.Quality = GrassQuality::Off;
            expectedOff.Effects.Toon = ToonMode::Off;
            expectedOff.Effects.ToonStyle.OutlineEnabled = false;
            expectedOff.Effects.AmbientOcclusion = AmbientOcclusionMode::Off;
            expectedOff.Effects.Reflections = ReflectionMode::Off;
            Check(runtime.NativePresentationOverrideActive() && effective.Revision > before.Revision,
                  "F2 must invalidate frame settings");
            Check(SerializeGraphicsSettings(effective.Value) == SerializeGraphicsSettings(expectedOff),
                  "F2 must disable only the requested effects");
            Check(SerializeGraphicsSettings(runtime.Snapshot()) == serialized,
                  "F2 overwrote configured values");
            Frame(true);
            Check(loggedPanelText.find("F2 override ACTIVE") != std::string::npos,
                  "active F2 status missing from real panel");
            Check(store->Stores == savesBefore && store->Root == storedBefore,
                  "F2 or opening F1 persisted temporary Off values");
            auto edited = runtime.Snapshot();
            edited.Effects.ToonStyle.OutlineWidth = 4.5F;
            edited.Grass.Generation.InstancesPerSquareMeter = 2048.0F;
            runtime.Apply(edited);
            Check(runtime.SnapshotForRendering().Value.Grass.Quality == GrassQuality::Off,
                  "editing configured effects bypassed F2");
            const auto editedJson = SerializeGraphicsSettings(runtime.Snapshot());
            Check(store->Root["Graphics"] == editedJson, "editing under F2 saved effective settings");
            const int savesAfterEdit = store->Stores;
            runtime.ToggleNativePresentationOverride();
            Check(!runtime.NativePresentationOverrideActive() &&
                      SerializeGraphicsSettings(runtime.SnapshotForRendering().Value) == editedJson,
                  "F2 did not restore latest edited settings exactly");
            Check(store->Stores == savesAfterEdit, "F2 restore wrote a profile");
        }
    }
    Frame(true);
    Check(loggedPanelText.find("F2 override inactive") != std::string::npos,
          "inactive F2 status missing from real panel");
    runtime.Apply(initial);
    Frame();

    Select("Preset", "Authentic");
    Check(runtime.Snapshot().Preset == GraphicsPreset::Authentic, "preset identity lost");
    Click("Antialiasing");
    Select("Mode", "FXAA");
    Check(runtime.Snapshot().AntiAliasing == AntiAliasingMode::Fxaa, "cannot customize Authentic");
    Check(runtime.Snapshot().Preset == GraphicsPreset::Custom, "manual edits must become Custom");
    {
        auto withPack = runtime.Snapshot();
        withPack.TexturePacks.Azahar.LoadCustomTextures = true;
        withPack.TexturePacks.Azahar.LoadDirectory = "pack";
        runtime.Apply(withPack);
        Frame();
    }
    Select("Preset", "Enhanced");
    Check(runtime.Snapshot().Preset == GraphicsPreset::Enhanced &&
              runtime.Snapshot().Effects.AmbientOcclusion == AmbientOcclusionMode::Mxao &&
              runtime.Snapshot().Effects.MxaoNormals == MxaoNormalSource::Depth &&
              runtime.Snapshot().TexturePacks.Azahar.LoadCustomTextures &&
              runtime.Snapshot().TexturePacks.Azahar.LoadDirectory == "pack",
          "Enhanced is not the captured profile or replaced the texture pack");
    Select("Preset", "Toon");
    const auto beforeCustom = runtime.Snapshot();
    Select("Preset", "Custom");
    const auto expected = GraphicsSettingsService::PresetForCurrent(GraphicsPreset::Custom, beforeCustom);
    Check(SerializeGraphicsSettings(runtime.Snapshot()) == SerializeGraphicsSettings(expected),
          "Custom changed user settings");
    Check(runtime.Snapshot().Effects.Toon == ToonMode::PostProcessPreview, "Custom reset effects");
    Click("Toon");
    auto custom = runtime.Snapshot();
    custom.Effects.ToonStyle.LightBandLevels[1] = 0.22F;
    runtime.Apply(custom);
    Frame();
    Click("Custom light band profile");
    Check(runtime.Snapshot().Effects.ToonStyle.LightBandLevels[1] == 0.22F, "enabling custom bands reset values");
    Click("Custom light band profile");
    Click("Custom light band profile");
    Check(runtime.Snapshot().Effects.ToonStyle.LightBandLevels[1] == 0.22F, "toggling custom bands reset values");
    auto bands = runtime.Snapshot().Effects.ToonStyle;
    ResizeToonLightBandProfile(bands, 6);
    Check(bands.LightBands == 6 && bands.LightBandLevels.front() == 0.0F &&
          bands.LightBandLevels[5] == 1.0F, "band resizing endpoints");
    for (size_t i = 1; i < 5; ++i)
        Check(bands.LightBandThresholds[i] > bands.LightBandThresholds[i-1], "threshold order");

    size.y = 1100.0F;
    Frame(); Frame();
    Click("Custom light band profile");
    Click("Outline");
    if (!runtime.Snapshot().Effects.ToonStyle.OutlineEnabled) Click("Enable depth outline");
    EditScalar("Outline width (1080p)", "9.5");
    EditScalar("Outline softness", "0.25");
    Check(runtime.Snapshot().Effects.ToonStyle.OutlineWidth == 9.5F,
          "outline width not applied");
    Check(runtime.Snapshot().Effects.ToonStyle.OutlineSoftness == 0.25F,
          "outline softness not applied");

    Click("Lighting");
    Select("Lighting model", "Per-pixel");
    Check(runtime.Snapshot().Effects.SurfaceLighting == SurfaceLightingMode::PerPixel,
          "per-pixel lighting selection not applied");
    Select("Lighting model", "Per-pixel + PBR");
    Check(runtime.Snapshot().Effects.SurfaceLighting == SurfaceLightingMode::PerPixelPbr,
          "PBR lighting selection not applied");
    Select("Lighting model", "Native + PBR");
    Check(runtime.Snapshot().Effects.SurfaceLighting == SurfaceLightingMode::NativePbr,
          "native PBR selection not applied");
    const auto persistedNativePbr = DeserializeGraphicsSettings(store->Root["Graphics"]);
    Check(persistedNativePbr.Value.Effects.SurfaceLighting == SurfaceLightingMode::NativePbr,
          "native PBR lighting selection lost on reload");
    runtime.ToggleNativePresentationOverride();
    Check(runtime.SnapshotForRendering().Value.Effects.SurfaceLighting == SurfaceLightingMode::Native,
          "F2 did not bypass native PBR");
    runtime.ToggleNativePresentationOverride();
    Check(runtime.SnapshotForRendering().Value.Effects.SurfaceLighting == SurfaceLightingMode::NativePbr,
          "F2 did not restore native PBR");
    CheckPbrMaterialEditor();
    runtime.SetCapability(GraphicsCapability::NativeSurfaceLighting, false, "test unavailable");
    Frame();
    Select("Lighting model", "Native");
    Check(runtime.Snapshot().Effects.SurfaceLighting == SurfaceLightingMode::Native,
          "cannot restore native lighting after capability loss");
    Click("Lighting model");
    Check(Find("Per-pixel").Disabled && Find("Per-pixel + PBR").Disabled && Find("Native + PBR").Disabled,
          "unsupported lighting model selectable");
    Click("Native");
    AllCapabilities(true);
    Frame();
    Select("Global illumination", "Brixelizer GI");
    Select("GI resolution", "Quarter");
    EditScalar("GI diffuse intensity", "2.5");
    EditScalar("GI native scene light", "0.4");
    EditScalar("GI key light", "1.5");
    EditScalar("GI dynamic lights", "0.5");
    EditScalar("GI sky light", "0.6");
    {
        const auto gi = runtime.Snapshot().Effects.GlobalIllumination;
        Check(gi.SceneLight == 0.4F && gi.KeyLight == 1.5F && gi.DynamicLights == 0.5F && gi.SkyLight == 0.6F,
              "GI source controls did not apply");
    }
    Click("GI specular reflections");
    EditScalar("GI specular intensity", "0.75");
    {
        const auto gi = runtime.Snapshot().Effects.GlobalIllumination;
        Check(gi.Mode == GlobalIlluminationMode::BrixelizerGi &&
                  gi.Resolution == GlobalIlluminationResolution::Quarter && gi.DiffuseIntensity == 2.5F,
              "global illumination controls did not apply");
        Check(gi.Specular && gi.SpecularIntensity == 0.75F, "GI specular controls did not apply");
        const auto persistedSpecular = DeserializeGraphicsSettings(store->Root["Graphics"]);
        Check(persistedSpecular.Value.Effects.GlobalIllumination.Specular &&
                  persistedSpecular.Value.Effects.GlobalIllumination.SpecularIntensity == 0.75F,
              "GI specular selection lost on reload");
        Check(runtime.SnapshotForRendering().Value.Effects.GlobalIllumination.Mode == GlobalIlluminationMode::Off,
              "global illumination rendered without per-pixel lighting");
        const auto persistedGi = DeserializeGraphicsSettings(store->Root["Graphics"]);
        Check(persistedGi.Value.Effects.GlobalIllumination.Mode == GlobalIlluminationMode::BrixelizerGi &&
                  persistedGi.Value.Effects.GlobalIllumination.DiffuseIntensity == 2.5F,
              "global illumination selection lost on reload");
    }
    Select("Lighting model", "Per-pixel");
    Check(runtime.SnapshotForRendering().Value.Effects.GlobalIllumination.Mode == GlobalIlluminationMode::BrixelizerGi,
          "per-pixel lighting did not activate the stored global illumination");
    runtime.ToggleNativePresentationOverride();
    Check(runtime.SnapshotForRendering().Value.Effects.GlobalIllumination.Mode == GlobalIlluminationMode::Off,
          "F2 did not bypass global illumination");
    runtime.ToggleNativePresentationOverride();
    runtime.SetCapability(GraphicsCapability::BrixelizerGlobalIllumination, false, "provider unavailable");
    Frame();
    Select("Global illumination", "Off");
    Check(runtime.Snapshot().Effects.GlobalIllumination.Mode == GlobalIlluminationMode::Off,
          "cannot disable global illumination after capability loss");
    Click("Global illumination");
    Check(Find("Brixelizer GI").Disabled, "unsupported global illumination selectable");
    Click("Off");
    Select("Lighting model", "Native");
    AllCapabilities(true);
    runtime.SetCapability(GraphicsCapability::ValidViewMetadata, false, "test unavailable");
    Frame();
    Select("Ambient occlusion", "Off");
    Check(runtime.Snapshot().Effects.AmbientOcclusion == AmbientOcclusionMode::Off, "cannot disable AO");
    AllCapabilities(true);
    Select("Ambient occlusion", "Multiscale GTAO");
    Select("AO quality", "Medium");
    Check(runtime.Snapshot().Effects.AoQuality == 1, "AO quality not applied");
    EditScalar("Broad radius", "8");
    EditScalar("Fine radius", "1.25");
    EditScalar("Fine strength", "0");
    EditScalar("Broad strength", "2");
    const auto ao = runtime.Snapshot().Effects;
    Check(ao.AoRadius == 8.0F && ao.AoDetailRadius == 1.25F &&
              ao.AoDetailStrength == 0.0F && ao.AoStrength == 2.0F,
          "AO fine/broad controls are coupled or were not applied");
    Select("Ambient occlusion", "FidelityFX CACAO");
    Click("Advanced AO");
    EditScalar("Horizon angle threshold", "0.12");
    EditScalar("Blur passes", "5");
    Check(runtime.Snapshot().Effects.AmbientOcclusion == AmbientOcclusionMode::Cacao &&
              runtime.Snapshot().Effects.AoHorizonAngleThreshold == 0.12F &&
              runtime.Snapshot().Effects.AoBlurPassCount == 5,
          "original CACAO controls did not apply");
    Select("Ambient occlusion", "iMMERSE MXAO");
    Select("MXAO quality", "High");
    Select("MXAO method", "Solid angle");
    EditScalar("MXAO radius", "2.5");
    EditScalar("MXAO amount", "1.5");
    EditScalar("MXAO filter quality", "2");
    const auto mxao = runtime.Snapshot().Effects;
    Check(mxao.AmbientOcclusion == AmbientOcclusionMode::Mxao && mxao.MxaoQuality == 2 &&
              mxao.MxaoMethod == MxaoEstimator::SolidAngle && mxao.AoRadius == 2.5F && mxao.AoStrength == 1.5F &&
              mxao.MxaoFilter == 2 && mxao.AoHorizonAngleThreshold == 0.12F,
          "MXAO controls did not apply or disturbed the CACAO controls");
    Click("MXAO temporal accumulation");
    Check(runtime.Snapshot().Effects.MxaoTemporal && runtime.Snapshot().Effects.MxaoQuality == 2 &&
              runtime.Snapshot().Effects.MxaoFilter == 2,
          "MXAO temporal accumulation did not apply or disturbed the MXAO controls");
    // The original's remaining options and the debug views.
    Select("MXAO quality", "IDGAF");
    Select("MXAO shading rate", "Quarter rate");
    Select("MXAO normals", "Renderer normals");
    Click("Increase radius with distance");
    EditScalar("MXAO sample radius", "4");
    Click("Original fade");
    EditScalar("Fade out distance", "0.5");
    Select("MXAO debug view", "Normal disagreement");
    {
        const auto options = runtime.Snapshot().Effects;
        Check(options.MxaoQuality == 6 && options.MxaoShadingRate == 2 &&
                  options.MxaoNormals == MxaoNormalSource::Renderer && options.MxaoRadiusGrowsWithDistance &&
                  options.MxaoScreenRadius == 4.0F && options.MxaoOriginalFade && options.MxaoFadeDistance == 0.5F &&
                  options.MxaoDebug == MxaoDebugView::NormalDisagreement && options.AoRadius == 2.5F &&
                  options.MxaoTemporal,
              "the original MXAO options did not apply or disturbed the other MXAO controls");
    }
    Select("MXAO debug view", "Off");
    Click("Original fade");
    Click("Increase radius with distance");
    Select("MXAO normals", "From depth (original)");
    Select("MXAO shading rate", "Full rate");
    Select("MXAO quality", "High");
    {
        const auto restored = runtime.Snapshot().Effects;
        Check(restored.MxaoDebug == MxaoDebugView::Off, "the MXAO debug view cannot be turned off");
        Check(restored.MxaoNormals == MxaoNormalSource::Depth, "MXAO depth normals cannot be restored");
        Check(!restored.MxaoRadiusGrowsWithDistance, "MXAO increase radius with distance cannot be cleared");
        Check(!restored.MxaoOriginalFade, "the MXAO original fade cannot be cleared");
        Check(restored.MxaoShadingRate == 0, "the MXAO full shading rate cannot be restored");
    }
    Select("AO texture relief", "Strong");
    Check(runtime.Snapshot().Effects.AoRelief == AoTextureRelief::Strong &&
              runtime.Snapshot().Effects.MxaoQuality == 2,
          "AO texture relief did not apply or disturbed the MXAO controls");
    Select("Ambient occlusion", "Multiscale GTAO");
    Select("AO texture relief", "Subtle");
    Check(runtime.Snapshot().Effects.AoRelief == AoTextureRelief::Subtle &&
              runtime.Snapshot().Effects.AmbientOcclusion == AmbientOcclusionMode::Gtao,
          "AO texture relief is not shared by the AO providers");
    Select("AO texture relief", "Off");
    Select("Ambient occlusion", "iMMERSE MXAO");
    Check(runtime.Snapshot().Effects.AoRelief == AoTextureRelief::Off && runtime.Snapshot().Effects.MxaoTemporal,
          "cannot disable AO texture relief, or another provider dropped MXAO accumulation");
    runtime.SetCapability(GraphicsCapability::ImmerseMxao, false, "provider unavailable");
    Frame();
    Select("Ambient occlusion", "Off");
    Click("Ambient occlusion");
    Check(Find("iMMERSE MXAO").Disabled && !Find("Multiscale GTAO").Disabled,
          "MXAO capability incorrectly disabled GTAO or allowed unsupported MXAO");
    Click("Off");
    runtime.SetCapability(GraphicsCapability::ImmerseMxao, true);
    Frame();
    runtime.SetCapability(GraphicsCapability::FidelityFxCacao, false, "provider unavailable");
    Frame();
    Select("Ambient occlusion", "Off");
    Click("Ambient occlusion");
    Check(Find("FidelityFX CACAO").Disabled && !Find("Multiscale GTAO").Disabled,
          "CACAO capability incorrectly disabled GTAO or allowed unsupported CACAO");
    Click("Multiscale GTAO");
    Check(runtime.Snapshot().Effects.AoDetailRadius == 1.25F,
          "switching AO provider erased the GTAO fine radius");
    runtime.SetCapability(GraphicsCapability::NormalGuide, false, "test unavailable");
    Frame();
    Select("Ambient occlusion", "Off");
    Check(runtime.Snapshot().Effects.AmbientOcclusion == AmbientOcclusionMode::Off,
          "cannot disable AO after normal-guide capability loss");
    AllCapabilities(true);
    Click("Reflections");
    Select("Reflection provider", "FidelityFX SSSR");
    runtime.SetCapability(GraphicsCapability::NormalGuide, false, "test unavailable");
    Frame();
    Select("Reflection provider", "Off");
    Check(runtime.Snapshot().Effects.Reflections == ReflectionMode::Off, "cannot disable reflections");
    AllCapabilities(true);
    Click("Antialiasing");
    Select("Mode", "MSAA");
    Select("Samples", "4x");
    Check(runtime.Snapshot().MsaaSamples == 4, "MSAA selection lost");
    Select("Mode", "NRI Upscaler");
    Select("Provider", "NVIDIA DLSS");
    Check(runtime.Snapshot().Upscaler == UpscalerProvider::Dlss, "provider not applied");
    Select("DLSS model", "CNN (E/F)");
    Check(runtime.Snapshot().UpscalerDlssModel == DlssModel::Cnn, "DLSS model not applied");
    Select("DLSS model", "Default");
    Check(std::none_of(items.begin(), items.end(), [](const auto& pair) { return pair.second.Label == "Sharpness"; }),
          "unused DLSS sharpness exposed");

    Click("Grass");
    Click("Generation");
    EditScalar("Blades per square metre", "4096");
    Check(runtime.Snapshot().Grass.Generation.InstancesPerSquareMeter == 4096.0F,
          "4096 grass density not applied");
    Click("Appearance");
    EditScalar("Curvature", "1.8");
    EditScalar("Tip droop", "0.8");
    EditScalar("Shape irregularity", "0.9");
    EditScalar("Blade twist", "150");
    const auto shape = runtime.Snapshot().Grass.Appearance;
    Check(std::abs(shape.BladeCurvature - 1.8F) < 0.001F &&
          std::abs(shape.BladeDroop - 0.8F) < 0.001F &&
          std::abs(shape.ShapeVariation - 0.9F) < 0.001F &&
          shape.BladeTwistDegrees == 150.0F, "grass shape controls not applied");
    EditScalar("Rim fade start (m)", "3");
    EditScalar("Rim fade end (m)", "12");
    Check(runtime.Snapshot().Grass.Appearance.ToonRimFadeStart == 300.0F &&
          runtime.Snapshot().Grass.Appearance.ToonRimFadeEnd == 1200.0F, "grass rim metre conversion failed");
    Click("Enable nearby rim");
    Check(!runtime.Snapshot().Grass.Appearance.ToonRimEnabled, "grass rim disable failed");
    Click("Enable nearby rim");
    Check(runtime.Snapshot().Grass.Appearance.ToonRimEnabled, "grass rim enable failed");
    Click("Performance");
    EditScalar("Draw distance", "20000");
    EditScalar("Density falloff distance", "5000");
    EditScalar("Final fade range", "0.25");
    EditScalar("Density fade softness", "0.30");
    EditScalar("LOD transition range", "0.35");
    EditScalar("Distant tuft quantity", "2.5");
    EditScalar("Distant tuft spread", "1.8");
    Check(runtime.Snapshot().Grass.DrawDistance == 20000.0F &&
          runtime.Snapshot().Grass.LodReferenceDistance == 5000.0F,
          "grass visibility and density distances not independent");
    EditScalar("Near blade segments", "8");
    EditScalar("Far blade segments", "2");
    EditScalar("Segment reduction begins", "120");
    EditScalar("Minimum segments reached", "650");
    EditScalar("Segment transition spread", "0.75");
    const auto grassLod = runtime.Snapshot().Grass;
    Check(grassLod.Appearance.BladeSegments == 8U && grassLod.FarBladeSegments == 2U &&
          grassLod.SegmentLodStartDistance == 120.0F && grassLod.SegmentLodEndDistance == 650.0F,
          "independent grass segment LOD controls not applied");
    Check(grassLod.DrawFadeFraction == 0.25F && grassLod.DensityFadeFraction == 0.30F &&
          grassLod.TuftTransitionFraction == 0.35F && grassLod.FarTuftDensity == 2.5F &&
          grassLod.FarTuftSpread == 1.8F && grassLod.SegmentLodSoftness == 0.75F,
          "grass transition, tuft quantity/spread controls not applied");
    size.y = 680.0F;
    for (const char* section : {"Generation", "Appearance", "Performance", "Wind", "Interaction", "Sources"})
        Click(section);
    updateTextureObservations = true;
    Frame();
    Click("Texture catalog selection");
    ClickTexture(secondTextureHash, true);
    Click("Add selected texture source");
    Check(runtime.Snapshot().Grass.Rules.size() == 1 &&
          runtime.Snapshot().Grass.Rules.front().Target.Rgba8Hash == secondTextureHash,
          "texture selection did not reach the grass consumer");
    Click("Renderer");
    Click("Reflections");
    Click("Reflection material assignments");
    Click("Texture catalog selection");
    ClickTexture(firstTextureHash);
    Click("Assign selected as water");
    Check(runtime.Snapshot().Effects.ReflectionMaterials.size() == 1 &&
          runtime.Snapshot().Effects.ReflectionMaterials.front().Target.ContentHash == firstTextureHash,
          "texture selection did not reach the reflection consumer");
    for (uint64_t index = 0; index < 120; ++index)
        TextureCatalogRuntime::Instance().Observe(0xC000U + index, 0x3000U, 16, 16, 0);
    Click("Texture catalog selection");
    auto* popup = ImGui::FindWindowByName("##Combo_00");
    Check(popup != nullptr && popup->Active, "texture popup is not active");
    ImGui::GetIO().AddMousePosEvent(popup->Pos.x + 24.0F, popup->Pos.y + 24.0F);
    Frame();
    ImGui::GetIO().AddMouseWheelEvent(0.0F, -100.0F);
    Frame(); Frame();
    ClickTexture(0xC000U + 119U);
    Click("Assign selected as metal");
    const auto reflectionRules = runtime.Snapshot().Effects.ReflectionMaterials;
    Check(std::any_of(reflectionRules.begin(), reflectionRules.end(), [](const auto& rule) {
        return rule.Target.ContentHash == 0xC000U + 119U;
    }), "catalog truncated entries after the first 96");
    Click("Grass");
    updateTextureObservations = false;
    auto display = runtime.Snapshot();
    display.Window = WindowMode::Borderless;
    runtime.Apply(display);
    runtime.AcknowledgePresentationApplied(display);
    showPanel = false;
    runtime.PublishDisplayMetrics({1920, 1080, 1280, 720, 2.0F / 3.0F, WindowMode::Borderless});
    Frame();
    Click("Keep display settings");
    Check(runtime.PresentationStatus().Phase == PresentationTransactionPhase::Idle,
          "display confirmation not accessible with F1 closed");
    showPanel = true;
    const auto confirmedDisplay = runtime.Snapshot();
    auto rejectedDisplay = confirmedDisplay;
    rejectedDisplay.Window = WindowMode::ExclusiveFullscreen;
    Check(runtime.Apply(rejectedDisplay).Accepted(), "display request rejected before backend");
    const std::string displayFailure = "Test monitor: exclusive mode unsupported";
    Check(runtime.RejectPresentationApply(rejectedDisplay, displayFailure),
          "backend display failure not accepted");
    Check(GetPresentationSettings(runtime.Snapshot()) == GetPresentationSettings(confirmedDisplay),
          "failed display request did not restore previous settings");
    Frame(true);
    Check(loggedPanelText.find("Display change reverted: " + displayFailure) != std::string::npos,
          "backend failure not visible in F1 outside Renderer");
    Check(!runtime.RejectPresentationApply(rejectedDisplay, "stale failure"),
          "stale display failure accepted");
    Check(!runtime.RejectPresentationApply(confirmedDisplay, "idle failure"),
          "idle display failure accepted");
    Check(!runtime.AcknowledgePresentationApplied(confirmedDisplay),
          "duplicate recovery acknowledgement accepted");
    Check(runtime.LastPresentationRejection() == displayFailure,
          "stale response erased the useful display failure");
    Check(runtime.Apply(rejectedDisplay).Accepted(), "display retry not staged");
    Check(runtime.RollbackPresentation(), "pending display retry not reverted");
    Check(runtime.AcknowledgePresentationApplied(confirmedDisplay),
          "display retry rollback not acknowledged");
    Check(runtime.LastPresentationRejection() == displayFailure,
          "rollback acknowledgement erased the useful display failure");
    Check(runtime.Apply(rejectedDisplay).Accepted(), "second display retry not staged");
    Check(!runtime.RejectPresentationApply(confirmedDisplay, "older request failed"),
          "older display request replaced a newer one");
    Check(!runtime.AcknowledgePresentationApplied(confirmedDisplay),
          "older display acknowledgement replaced a newer one");
    Check(runtime.LastPresentationRejection() == displayFailure,
          "retry lost display failure before backend success");
    Check(runtime.AcknowledgePresentationApplied(rejectedDisplay),
          "successful display retry not acknowledged");
    Check(runtime.LastPresentationRejection().empty(), "successful retry retained old error");
    Frame(true);
    Check(loggedPanelText.find("Display change reverted:") == std::string::npos,
          "F1 retained error after successful retry");
    Click("Revert display settings");
    Check(runtime.AcknowledgePresentationApplied(confirmedDisplay),
          "smoke test could not restore its original display");
    Click("Textures");
    Find("Apply folders");
    Click("Controls");
    for (const char* section : {"Bindings", "Shortcuts", "Camera"}) Click(section);
    Click("Enabled##freecam");
    Check(topScreen->Snapshot().Config.FreeCameraEnabled, "central camera control not connected");
    Click("##Free-camera source");
    Frame(true);
    Check(std::none_of(items.begin(), items.end(), [](const auto& pair) {
              return pair.second.Popup && (pair.second.Label == "Controller Gyroscope" ||
                  pair.second.Label == "Controller Accelerometer" ||
                  pair.second.Label == "Controller gyro + accelerometer");
          }),
          "free-camera source exposes motion sensors");
    Click("Right Stick");
    Check(controls->Snapshot().Config.FreeCameraSource == Oot3dNativeGame::NativeMotionSource::RightStick,
          "free-camera stick choice not connected");
    auto cameraExternal = topScreen->Snapshot().Config;
    cameraExternal.HudMarginX = 7;
    topScreen->Preview(cameraExternal);
    Frame();
    Click("Enabled##freecam");
    Check(!topScreen->Snapshot().Config.FreeCameraEnabled && topScreen->Snapshot().Config.HudMarginX == 7,
          "central camera control overwrote external HUD state");
    Click("Free camera");
    Click("Aiming");
    Select("##Aim source", "Mouse");
    EditScalar("##Mouse aim sensitivity", "0.61");
    Check(std::abs(controls->Snapshot().Config.MouseAimDegreesPerPixel - 0.61F) < 0.001F,
          "aiming sensitivity not connected");
    Click("Devices");
    Click("Keyboard");
    Check(!controls->Snapshot().Config.KeyboardEnabled, "control preview not connected");
    Click("Controller touchpad");
    Check(!controls->Snapshot().Config.ControllerTouchpadEnabled, "touchpad disable not connected");
    Click("Controller touchpad");
    Check(controls->Snapshot().Config.ControllerTouchpadEnabled, "touchpad enable not connected");
    controls->ObserveDevices({
        {11, "same-model", "Twin pad", false, false, "first", true},
        {12, "same-model", "Twin pad", false, false, "second", false},
    });
    Frame();
    Select("##Active controller", "Twin pad [12]##12");
    Check(controls->Snapshot().Config.PreferredControllerGuid == "same-model" &&
          controls->Snapshot().Config.PreferredControllerSerial == "second",
          "controller selection discarded physical identity");
    auto external = controls->Snapshot().Config;
    external.MouseAimDegreesPerPixel = 0.77F;
    controls->Preview(external);
    Frame();
    Click("Keyboard");
    Check(controls->Snapshot().Config.MouseAimDegreesPerPixel == 0.77F, "stale draft overwrote external controls");
    Click("Bindings");
    using Action = Oot3dNativeGame::NativeControlAction;
    using Key = Oot3dNativeGame::NativeKeyboardKey;
    const auto movementIndex = static_cast<size_t>(Action::MoveForward);
    // Old content-proportional columns shrink progressively, not just on resize.
    EditText("##ActionFilter", "move");
    for (const auto width : {520.0F, 760.0F, 1100.0F, 520.0F}) {
        size = ImVec2(width, 680.0F);
        Frame(); Frame(); Frame();
        const auto primary = Find("##primary:Move forward").Rect;
        const auto alternate = Find("##alternate:Move forward").Rect;
        Check(primary.GetWidth() > 100.0F && alternate.GetWidth() > 100.0F,
              "binding fields became too narrow");
        Check(primary.Max.x < alternate.Min.x && alternate.Max.x < size.x + 8.0F,
              "binding columns overlap or leave the window");
        for (int frame = 0; frame < 120; ++frame) Frame();
        const auto after = Find("##primary:Move forward").Rect;
        Check(std::abs(after.Min.x - primary.Min.x) < 1.0F &&
              std::abs(after.GetWidth() - primary.GetWidth()) < 1.0F,
              "binding columns drifted across idle frames");
    }
    size = ImVec2(760.0F, 680.0F); Frame(); Frame();
    Select("##primary:Move forward", "Unassigned");
    Check(controls->Snapshot().Config.Bindings[movementIndex].KeyboardPrimary == Key::None,
          "clear key did not update runtime");
    Click("##primary:Move forward");
    EditText("##BindingSearch", "numpad 8");
    Click("Numpad 8");
    Check(controls->Snapshot().Config.Bindings[movementIndex].KeyboardPrimary == Key::Numpad8,
          "searchable key assignment did not update runtime");
    Click("##primary:Move forward");
    Check(GImGui->OpenPopupStack.Size > 0, "key popup failed to reopen");
    Click("W");
    Check(controls->Snapshot().Config.Bindings[movementIndex].KeyboardPrimary == Key::W,
          "key popup retained stale search on reopen");
    Click("Mouse");
    Select("##mouse:Move forward", "Back");
    Check(controls->Snapshot().Config.Bindings[movementIndex].Mouse == Oot3dNativeGame::NativeMouseButton::Back &&
          controls->Snapshot().Config.Bindings[movementIndex].KeyboardPrimary == Key::W,
          "mouse assignment changed the wrong device");
    Select("##mouse:Move forward", "Unassigned");
    Click("Controller");
    const auto beforeSwap = controls->Snapshot().Config;
    Click("Swap shoulders / triggers");
    const auto swapped = controls->Snapshot().Config;
    using Button = Oot3dNativeGame::NativeGamepadButton;
    Check(swapped.Bindings[static_cast<size_t>(Action::L)].Gamepad == Button::LeftTrigger &&
          swapped.Bindings[static_cast<size_t>(Action::R)].Gamepad == Button::RightTrigger &&
          swapped.Bindings[static_cast<size_t>(Action::Zl)].Gamepad == Button::LeftShoulder &&
          swapped.Bindings[static_cast<size_t>(Action::Zr)].Gamepad == Button::RightShoulder,
          "full controller swap failed to move both item bindings");
    Click("Swap shoulders / triggers");
    Check(controls->Snapshot().Config.Bindings == beforeSwap.Bindings,
          "second controller swap did not restore original bindings");
    Click("##gamepad:Move forward");
    EditText("##BindingSearch", "right stick");
    Click("Right Stick");
    Check(controls->Snapshot().Config.Bindings[movementIndex].Gamepad == Button::RightStick &&
          controls->Snapshot().Config.Bindings[movementIndex].KeyboardPrimary == Key::W,
          "controller assignment changed the wrong device");
    Select("##gamepad:Move forward", "Unassigned");
    EditText("##ActionFilter", "no such control");
    Frame(true);
    Check(loggedPanelText.find("No matching controls") != std::string::npos,
          "empty binding search not reported");
    EditText("##ActionFilter", "");
    Find("##gamepad:Move forward");
    EditText("##ActionFilter", "Move forward");
    RawBindingSource rawBinding;
    using CapturePhase = ThreeDsRecomp::Input::BindingCapturePhase;
    Click("##gamepad:Move forward"); Click("Listen...");
    Check(controls->BindingCaptureStatus().Phase == CapturePhase::Release, "controller Listen did not start capture");
    controls->ObserveBindingCapture(rawBinding, false); Frame();
    rawBinding.Held.Gamepad = Button::LeftTrigger;
    controls->ObserveBindingCapture(rawBinding, false); Frame(); Frame();
    Check(controls->Snapshot().Config.Bindings[movementIndex].Gamepad == Button::LeftTrigger &&
          !controls->Snapshot().Config.ControllerEnabled, "Listen did not assign a disabled controller source");
    Click("Keyboard");
    rawBinding.Held = {};
    Click("##alternate:Move forward"); Click("Listen...");
    controls->ObserveBindingCapture(rawBinding, false); Frame();
    rawBinding.Held.KeyboardPrimary = Key::P;
    controls->ObserveBindingCapture(rawBinding, false); Frame(); Frame();
    Check(controls->Snapshot().Config.Bindings[movementIndex].KeyboardSecondary == Key::P &&
          controls->Snapshot().Config.Bindings[movementIndex].KeyboardPrimary == Key::W,
          "Listen assigned wrong keyboard slot");
    Click("Mouse");
    rawBinding.Held = {};
    rawBinding.Held.Mouse = Oot3dNativeGame::NativeMouseButton::Left;
    const auto beforeCapture = controls->Snapshot().Config;
    Click("##mouse:Move forward"); Click("Listen...");
    controls->ObserveBindingCapture(rawBinding, false); Frame();
    Check(controls->Snapshot().Config == beforeCapture && controls->BindingCaptureStatus().Phase == CapturePhase::Release,
          "Listen captured its opening mouse click");
    rawBinding.Held = {};
    controls->ObserveBindingCapture(rawBinding, false); Frame();
    rawBinding.Held.Mouse = Oot3dNativeGame::NativeMouseButton::Forward;
    controls->ObserveBindingCapture(rawBinding, false); Frame(); Frame();
    Check(controls->Snapshot().Config.Bindings[movementIndex].Mouse == Oot3dNativeGame::NativeMouseButton::Forward,
          "Listen did not assign the mouse side button");
    const auto beforeCancel = controls->Snapshot().Config;
    Click("##mouse:Move forward"); Click("Listen...");
    Click("Cancel");
    Check(controls->Snapshot().Config == beforeCancel, "cancelling Listen modified a binding");
    Click("Presets...");
    const auto beforePreset = controls->Snapshot().Config;
    Select("##Preset", "Controller");
    Check(controls->Snapshot().Config == beforePreset, "preset selection applied without confirmation");
    Click("Cancel");
    Check(controls->Snapshot().Config == beforePreset, "cancel changed the control profile");
    Click("Presets...");
    Click("Apply preset");
    Check(controls->Snapshot().Config.Profile == Oot3dNativeGame::NativeControlProfile::Controller,
          "confirmed preset not applied");
    Check(controls->Snapshot().Config.PreferredControllerGuid == beforePreset.PreferredControllerGuid &&
          controls->Snapshot().Config.PreferredControllerSerial == beforePreset.PreferredControllerSerial &&
          controls->Snapshot().Config.GyroscopeBiasDegreesPerSecond == beforePreset.GyroscopeBiasDegreesPerSecond &&
          controls->Snapshot().Config.AccelerometerNeutral == beforePreset.AccelerometerNeutral,
          "preset discarded controller identity or calibration");
    Click("TopScreen 2.1.1");
    Click("Render HUD");
    Check(!topScreen->Snapshot().Config.RenderHud, "TopScreen preview not connected");
    auto externalTop = topScreen->Snapshot().Config;
    externalTop.HudScale = 0.75F;
    topScreen->Preview(externalTop);
    Frame();
    Click("Render HUD");
    Check(topScreen->Snapshot().Config.HudScale == 0.75F, "stale draft overwrote external TopScreen");
    Click("HUD");
    size = ImVec2(520, 400);
    Frame(); Frame();
    Find("Save TopScreen");
    Click("Controls");
    for (const char* section : {"Bindings", "Camera", "Devices", "Shortcuts"}) {
        Click(section);
        Frame(); Frame();
        const auto saveRect = Find("Save controls").Rect;
        const auto revertRect = Find("Revert changes").Rect;
        Check(saveRect.Min.y > 8.0F && saveRect.Max.y < size.y + 8.0F &&
              revertRect.Max.x < size.x + 8.0F && saveRect.Max.x < revertRect.Min.x,
              "compact Controls footer is clipped or overlapping");
        for (const auto* window : GImGui->Windows)
            if (window->Active && (window->Flags & ImGuiWindowFlags_ChildWindow))
                Check(window->ScrollMax.x == 0.0F, "Controls contents overflow horizontally");
    }

    auto pending = runtime.Snapshot();
    const int storesBefore = store->Stores;
    for (int i = 0; i < 30; ++i) {
        pending.FovMultiplier = 1.0F + i * 0.01F;
        runtime.Apply(pending, false);
    }
    Check(store->Stores == storesBefore, "drag wrote settings on every step");
    runtime.SavePending();
    Check(store->Stores == storesBefore + 1, "deferred save not coalesced");
    size = ImVec2(760, 680);
    Click("Renderer");
    Click("Display");
    Frame();
    const auto fov = Find("Global scene FOV");
    const int beforeDrag = store->Stores;
    auto& io = ImGui::GetIO();
    const float y = (fov.Rect.Min.y + fov.Rect.Max.y) * 0.5F;
    io.AddMousePosEvent(fov.Rect.Min.x + 30.0F, y);
    Frame();
    io.AddMouseButtonEvent(0, true);
    Frame();
    for (int step = 0; step < 10; ++step) {
        io.AddMousePosEvent(fov.Rect.Min.x + 35.0F + step * 5.0F, y);
        Frame();
    }
    Check(store->Stores == beforeDrag, "live UI drag wrote to disk");
    io.AddMouseButtonEvent(0, false);
    Frame(); Frame();
    Check(store->Stores == beforeDrag + 1, "UI slider release did not save once");
    store->Fail = true;
    pending.FovMultiplier = 1.1F;
    runtime.Apply(pending);
    Check(runtime.SaveState() == GraphicsSettingsSaveState::Failed, "save failure hidden");
    store->Fail = false;
    Frame();
    Click("Retry saving graphics");
    Check(runtime.SaveState() == GraphicsSettingsSaveState::Saved, "save retry failed");
    CheckControlPersistence();
    CheckGameSurfaceMouseResume();
    const auto languagePath = std::filesystem::temp_directory_path() /
        ("triaevum-language-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    const std::vector<Oot3dNativeGame::GameLanguage> languages{{"en","English",1},{"it","Italiano",4}};
    auto language = std::make_shared<Oot3dNativeGame::GameLanguageSettings>(languagePath,languages);
    InstallGraphicsSettingsPanelTabs({Oot3dNativeGame::CreateGameLanguagePanel(language)});
    Frame(); Frame();
    Click("Game"); Frame();
    Select("Game language", "Italiano");
    Check(language->Selected() == "it", "language widget did not persist choice");
    Check(language->SystemId() == 1, "language selection mutated the running guest");
    Frame(true);
    Check(loggedPanelText.find("Restart required") != std::string::npos, "language restart notice missing");
    Oot3dNativeGame::GameLanguageSettings restarted(languagePath,languages);
    Check(restarted.SystemId() == 4, "next boot did not select Italian CFG language");
    Check(!restarted.Select("de"), "language absent from ROM was accepted");
    Oot3dNativeGame::GameLanguageSettings differentRom(languagePath,{{"en","English",1}});
    Check(differentRom.SystemId() == 1, "unsupported old language leaked into another ROM");
    std::filesystem::remove(languagePath);
    CheckSceneEnvironment();
    InstallGraphicsSettingsPanelTabs({});
    ImGui::DestroyContext();
    std::cout << "F1 UI smoke passed: " << assertions << " assertions, real renderer/Controls/TopScreen widgets\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
}
