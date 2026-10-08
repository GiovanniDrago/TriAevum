#include "fast/oot3d/graphics_settings_window.h"
#include "fast/oot3d/graphics_settings_runtime.h"
#include "fast/oot3d/settings_panel_widgets.h"

namespace Fast::Oot3d {
namespace {
bool DrawDirectionalShadows(GraphicsSettings& settings, const GraphicsCapabilities& capabilities) {
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
            "2048 (32 MiB)", "4096 (128 MiB)" };
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
        ImGui::TextWrapped("World-anchored virtual pages; unchanged pages are reused. Budget includes the static cache and composed atlas, and controls residency rather than near-field texel size.");
        if (ImGui::TreeNode("Advanced directional shadows")) {
            changed |= ImGui::DragFloat(
                "Shadow constant bias",
                &directionalShadows.DepthBiasConstant,
                0.05F, 0.0F, 16.0F, "%.2f");
            changed |= ImGui::DragFloat(
                "Shadow slope bias",
                &directionalShadows.DepthBiasSlope,
                0.05F, 0.0F, 16.0F, "%.2f");
            changed |= ImGui::SliderFloat(
                "Shadow normal offset",
                &directionalShadows.NormalOffset,
                0.0F, 8.0F, "%.2f texels");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Smooth-shaded models (characters) read the shadow this far along their "
                                  "shading normal, more where the light grazes them: removes the faceted "
                                  "self-shadowing of their triangles and of the joins between their pieces.");
            changed |= ImGui::SliderFloat(
                "Shadow terminator fade",
                &directionalShadows.TerminatorFade,
                0.0F, 1.0F, "%.2f N.L");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Where a smooth-shaded model turns from the light (N.L below this), its "
                                  "shadow gives way to the smooth light falloff instead of the facets of its "
                                  "triangles shadowing one another.");
            ImGui::TreePop();
        }
        if (!directionalShadowsReady) ImGui::EndDisabled();
    }
    if (!directionalShadowsReady) {
        ImGui::TextDisabled("Unavailable: %s",
            directionalShadowCapability.Reason.c_str());
    }
    if (GraphicsSettingsRuntime::Instance().NativePresentationOverrideActive())
        ImGui::TextWrapped("Inactive while the F2 native override is enabled.");
    ImGui::TextWrapped("Shadows apply to every scene and are saved automatically; the scene lights above are saved per scene.");
    return changed;
}

bool DrawPointShadows(GraphicsSettings& settings, const GraphicsCapabilities& capabilities) {
    bool changed = false;
    ImGui::SeparatorText("Point light shadows");
    auto& pointShadows = settings.Effects.PointShadows;
    const auto& capability = capabilities.Get(GraphicsCapability::NriDirectionalShadows);
    const char* const modes[] = { "Off", "Cube maps" };
    changed |= SettingsUi::ValidatedCombo<PointShadowMode>(
        "Point shadow mode", settings, capabilities, modes,
        [](auto& value, auto mode) { value.Effects.PointShadows.Mode = mode; },
        [](const auto& value) { return value.Effects.PointShadows.Mode; });
    if (pointShadows.Mode == PointShadowMode::CubeMaps) {
        if (!capability.Available) ImGui::BeginDisabled();
        constexpr uint32_t resolutions[] = { 256U, 512U, 1024U };
        int resolutionIndex = 1;
        for (size_t index = 0; index < std::size(resolutions); ++index)
            if (resolutions[index] == pointShadows.Resolution) resolutionIndex = static_cast<int>(index);
        const char* const resolutionNames[] = {
            "256 (1.5 MiB per light)", "512 (6.1 MiB per light)", "1024 (24 MiB per light)" };
        if (ImGui::Combo("Point shadow face resolution", &resolutionIndex, resolutionNames,
                         static_cast<int>(std::size(resolutionNames)))) {
            pointShadows.Resolution = resolutions[resolutionIndex];
            changed = true;
        }
        int maxLights = pointShadows.MaxLights;
        if (ImGui::SliderInt("Shadowed point lights", &maxLights, 1, 8)) {
            pointShadows.MaxLights = static_cast<uint8_t>(maxLights);
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The lights nearest the camera cast shadows; the others stay unshadowed.");
        float reach = pointShadows.Range * 100.0F;
        if (ImGui::SliderFloat("Point shadow reach", &reach, 25.0F, 100.0F, "%.0f%% of light radius",
                               ImGuiSliderFlags_AlwaysClamp)) {
            pointShadows.Range = reach / 100.0F;
            changed = true;
        }
        changed |= ImGui::SliderFloat("Point shadow strength", &pointShadows.Strength, 0.0F, 1.0F, "%.2f",
                                      ImGuiSliderFlags_AlwaysClamp);
        ImGui::TextWrapped("Navi, fairies and native local lights shadow only within their own radius. Rows of unchanged static lights are reused.");
        if (!capability.Available) ImGui::EndDisabled();
    }
    if (!capability.Available)
        ImGui::TextDisabled("Unavailable: %s", capability.Reason.c_str());
    return changed;
}
} // namespace

bool GraphicsSettingsPanel::DrawShadowSettings(GraphicsSettings& settings, const GraphicsCapabilities& capabilities) {
    // Both shadow kinds need the full scene lighting of the Modern model.
    auto shadowCapabilities = capabilities;
    if (!UsesFullSceneLighting(settings.Effects.SurfaceLighting))
        shadowCapabilities.Set(GraphicsCapability::NriDirectionalShadows, false,
                               "Choose the Modern lighting model in Lighting & GI.");
    bool changed = DrawDirectionalShadows(settings, shadowCapabilities);
    changed |= DrawPointShadows(settings, shadowCapabilities);
    return changed;
}
} // namespace Fast::Oot3d
