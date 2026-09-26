#include "fast/oot3d/graphics_settings_runtime.h"
#include "fast/oot3d/settings_panel_widgets.h"
#include "oot3d_scene_environment.h"
#include <imgui.h>

namespace Oot3dNativeGame {
namespace {
void DrawDirectionalShadows() {
    using namespace Fast::Oot3d;
    auto& runtime = GraphicsSettingsRuntime::Instance();
    auto settings = runtime.Snapshot();
    auto capabilities = runtime.Capabilities();
    if (settings.Effects.SurfaceLighting == SurfaceLightingMode::Native)
        capabilities.Set(GraphicsCapability::NriDirectionalShadows, false,
                         "Choose Per-pixel or Per-pixel + PBR in Lighting.");
    bool changed = false;
    ImGui::SeparatorText("Directional shadows");
    auto& directionalShadows = settings.Effects.DirectionalShadows;
    const auto& directionalShadowCapability =
        capabilities.Get(GraphicsCapability::NriDirectionalShadows);
    const bool directionalShadowsReady =
        directionalShadowCapability.Available;
    const char* const directionalShadowModes[] = {
        "Off", "Virtual Shadow Maps" };
    changed |= SettingsUi::ValidatedCombo<DirectionalShadowMode>(
        "Shadow mode", settings, capabilities, directionalShadowModes,
        [](auto& value, auto mode) { value.Effects.DirectionalShadows.Mode = mode; },
        [](const auto& value) { return value.Effects.DirectionalShadows.Mode; });
    if (directionalShadows.Mode ==
        DirectionalShadowMode::VirtualShadowMaps) {
        if (!directionalShadowsReady) ImGui::BeginDisabled();
        constexpr uint32_t resolutions[] = {
            2048U, 4096U };
        int resolutionIndex = 0;
        for (size_t index = 0; index < std::size(resolutions);
             ++index) {
            if (resolutions[index] ==
                directionalShadows.Resolution) {
                resolutionIndex = static_cast<int>(index);
                break;
            }
        }
        const char* const resolutionNames[] = {
            "2048 (16 MiB)", "4096 (64 MiB)" };
        if (ImGui::Combo("Shadow atlas budget", &resolutionIndex,
                         resolutionNames,
                         static_cast<int>(
                             std::size(resolutionNames)))) {
            directionalShadows.Resolution =
                resolutions[resolutionIndex];
            changed = true;
        }
        changed |= ImGui::DragFloat(
            "Shadow distance",
            &directionalShadows.MaximumDistance,
            100.0F, 50.0F, 65536.0F, "%.0f", ImGuiSliderFlags_AlwaysClamp);
        changed |= ImGui::SliderFloat(
            "Shadow strength", &directionalShadows.Strength,
            0.0F, 4.0F, "%.2fx");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Above 1x amplifies partial occlusion. Visibility saturates at zero instead of producing negative light.");
        int pcfRadius = directionalShadows.PcfRadius;
        if (ImGui::SliderInt("PCF radius", &pcfRadius, 0, 2)) {
            directionalShadows.PcfRadius =
                static_cast<uint8_t>(pcfRadius);
            changed = true;
        }
        ImGui::TextWrapped("World-anchored virtual pages; unchanged pages are reused. Atlas budget controls residency, not near-field texel size.");
        if (ImGui::TreeNode("Advanced directional shadows")) {
            changed |= ImGui::DragFloat(
                "Shadow constant bias",
                &directionalShadows.DepthBiasConstant,
                0.05F, 0.0F, 16.0F, "%.2f");
            changed |= ImGui::DragFloat(
                "Shadow slope bias",
                &directionalShadows.DepthBiasSlope,
                0.05F, 0.0F, 16.0F, "%.2f");
            ImGui::TreePop();
        }
        if (!directionalShadowsReady) ImGui::EndDisabled();
    }
    if (!directionalShadowsReady) {
        ImGui::TextDisabled("Unavailable: %s",
            directionalShadowCapability.Reason.c_str());
    }
    if (runtime.NativePresentationOverrideActive())
        ImGui::TextWrapped("Inactive while the F2 native override is enabled.");
    ImGui::TextWrapped("Shadow quality is global and saved automatically. Scene lights use the scene profile below.");
    if (changed) {
        settings.Preset = GraphicsPreset::Custom;
        runtime.Apply(std::move(settings));
    }
}

class EnvironmentPanel final : public Fast::Oot3d::GraphicsSettingsPanelTab {
public:
  explicit EnvironmentPanel(std::shared_ptr<SceneEnvironmentRuntime> runtime)
      : mRuntime(std::move(runtime)) {}
  const char *Label() const noexcept override { return "Environment"; }
  void Draw() override {
    DrawDirectionalShadows();
    ImGui::SeparatorText("Scene environment");
    const auto state = mRuntime->Snapshot();
    if (!state.SceneId) {
      ImGui::TextWrapped("No native gameplay scene is available. Environment "
                         "overrides are inactive.");
      return;
    }
    ImGui::Text("Native scene: 0x%04X (all rooms)", unsigned(*state.SceneId));
    ImGui::TextWrapped("Profile: %s", state.Path.string().c_str());
    auto value = state.Profile;
    auto &graphics = Fast::Oot3d::GraphicsSettingsRuntime::Instance();
    const auto settings = graphics.Snapshot();
    const bool supported =
        graphics.Capabilities()
            .Get(Fast::Oot3d::GraphicsCapability::NativeSurfaceLighting)
            .Available;
    ImGui::BeginDisabled(!supported && !value.Enabled);
    bool changed = ImGui::Checkbox("Use scene environment", &value.Enabled);
    ImGui::EndDisabled();
    if (!supported ||
        settings.Effects.SurfaceLighting ==
            Fast::Oot3d::SurfaceLightingMode::Native ||
        settings.Preset == Fast::Oot3d::GraphicsPreset::Authentic ||
        graphics.NativePresentationOverrideActive())
      ImGui::TextWrapped(
          "Inactive: choose Per-pixel or Per-pixel + PBR in Lighting, with F2 "
          "native override off and a supported renderer.");
    ImGui::BeginDisabled(!value.Enabled || !supported);
    changed |= ImGui::Checkbox("Preserve scene brightness", &value.PreserveSceneBrightness);
    ImGui::TextWrapped(
        "Keep the native ambient reference and authored unlit color when native lights are off. "
        "Added lights still add illumination; this is not automatic exposure.");
    changed |= ImGui::ColorEdit3("Ambient color", value.AmbientColor.data(),
                                 ImGuiColorEditFlags_Float);
    changed |=
        ImGui::SliderFloat("Ambient intensity", &value.AmbientIntensity, 0.0F,
                           2.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::TextWrapped(
        "Ambient is uniform, without direction. Colors are linear RGB.");
    changed |= ImGui::SliderFloat("Native scene direct contribution",
                                  &value.NativeDirectStrength, 0.0F, 2.0F,
                                  "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::SliderFloat("Native local contribution",
                                  &value.NativeLocalStrength, 0.0F, 2.0F,
                                  "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    ImGui::TextWrapped(
        "Scene direct uses the game's two environment directions on world and actors. "
        "Local keeps other native per-draw lights separate. Neither controls the added key/fill or fairy lights.");
    ImGui::BeginDisabled(value.PreserveSceneBrightness);
    changed |= ImGui::SliderFloat("Native ambient contribution",
                                  &value.NativeAmbientStrength, 0.0F, 2.0F,
                                  "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    changed |=
        ImGui::SliderFloat("Native fog contribution", &value.FogStrength, 0.0F,
                           1.0F, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    ImGui::TextUnformatted("Additional environment lights");
    if (ImGui::BeginTabBar("Scene lights")) {
      const char *labels[] = {"Key light", "Fill light"};
      for (size_t i = 0; i < value.Lights.size(); ++i) {
        if (!ImGui::BeginTabItem(labels[i]))
          continue;
        auto &light = value.Lights[i];
        ImGui::PushID(static_cast<int>(i));
        changed |= ImGui::ColorEdit3("Light color", light.Color.data(),
                                     ImGuiColorEditFlags_Float);
        changed |=
            ImGui::SliderFloat("Light intensity", &light.Intensity, 0.0F, 4.0F,
                               "%.2f", ImGuiSliderFlags_AlwaysClamp);
        changed |=
            ImGui::SliderFloat("Azimuth", &light.Azimuth, -180.0F, 180.0F,
                               "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
        changed |=
            ImGui::SliderFloat("Elevation", &light.Elevation, -90.0F, 90.0F,
                               "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
        ImGui::TextWrapped(
            i == 0 ? "Key light casts directional shadows when Shadow mode is "
                     "enabled above. It lights actors and static world surfaces."
                   : "Fill light is unshadowed.");
        ImGui::PopID();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::TextWrapped("Direction points toward the source: azimuth 0 = +Z, 90 "
                       "= +X; elevation 90 = overhead. Painted texture "
                       "lighting remains in the albedo.");
    ImGui::EndDisabled();
    ImGui::SeparatorText("Dynamic scene point lights");
    ImGui::TextWrapped("Replace Navi and fairy vertex-light contributions with spatial scene lights. "
                       "Native positions, colors and ranges are retained. These controls are independent "
                       "of Use scene environment and affect only Per-pixel / Per-pixel + PBR.");
    const char *pointLabels[] = {"Navi point lights", "Fairy point lights"};
    for (size_t i = 0; i < value.PointLights.size(); ++i) {
      auto &light = value.PointLights[i];
      ImGui::PushID(pointLabels[i]);
      ImGui::BeginDisabled(!supported && !light.Enabled);
      changed |= ImGui::Checkbox(pointLabels[i], &light.Enabled);
      ImGui::EndDisabled();
      ImGui::BeginDisabled(!supported || !light.Enabled);
      changed |= ImGui::SliderFloat(i == 0 ? "Navi intensity" : "Fairy intensity", &light.Intensity, 0.0F, 8.0F,
                                    "%.2fx", ImGuiSliderFlags_AlwaysClamp);
      changed |= ImGui::SliderFloat(i == 0 ? "Navi radius scale" : "Fairy radius scale", &light.RadiusScale, 0.1F, 8.0F,
                                    "%.2fx", ImGuiSliderFlags_AlwaysClamp);
      changed |= ImGui::ColorEdit3(i == 0 ? "Navi color tint" : "Fairy color tint", light.ColorTint.data(),
                                   ImGuiColorEditFlags_Float);
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    if (changed)
      mRuntime->Preview(value);
    if (ImGui::Button("Save scene environment"))
      mRuntime->Save();
    ImGui::SameLine();
    if (ImGui::Button("Reload scene environment"))
      mRuntime->Reload();
    if (ImGui::Button("Use native environment")) {
      value.Enabled = false;
      mRuntime->Preview(value);
    }
    const auto status = mRuntime->Snapshot();
    if (!status.Error.empty())
      ImGui::TextWrapped("Environment error: %s", status.Error.c_str());
    if (status.Dirty)
      ImGui::TextWrapped(
          "Unsaved live preview. Save before leaving this scene.");
    else
      ImGui::TextUnformatted(
          status.FileExists ? "Scene profile saved."
                            : "No scene profile file. Save to create one.");
  }

private:
  std::shared_ptr<SceneEnvironmentRuntime> mRuntime;
};
} // namespace
std::shared_ptr<Fast::Oot3d::GraphicsSettingsPanelTab>
CreateSceneEnvironmentPanel(std::shared_ptr<SceneEnvironmentRuntime> runtime) {
  return std::make_shared<EnvironmentPanel>(std::move(runtime));
}
} // namespace Oot3dNativeGame
