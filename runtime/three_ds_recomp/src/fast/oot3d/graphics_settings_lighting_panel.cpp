#include "fast/oot3d/graphics_settings_window.h"
#include "fast/oot3d/graphics_settings_runtime.h"
#include "fast/oot3d/settings_panel_widgets.h"

namespace Fast::Oot3d {
using SettingsUi::EnumCombo;

bool GraphicsSettingsPanel::DrawLightingSettings(GraphicsSettings& settings, const GraphicsCapabilities& capabilities) {
    bool changed = false;
    ImGui::SeparatorText("Surface lighting");
    const char* const surfaceLightingModes[] = {"Native", "Per-pixel", "Per-pixel + PBR", "Native + PBR"};
    changed |= SettingsUi::ValidatedCombo<SurfaceLightingMode>(
        "Lighting model", settings, capabilities, surfaceLightingModes,
        [](auto& value, auto mode) { value.Effects.SurfaceLighting = mode; },
        [](const auto& value) { return value.Effects.SurfaceLighting; });
    const bool nativePbr = settings.Effects.SurfaceLighting == SurfaceLightingMode::NativePbr;
    ImGui::TextWrapped(nativePbr
        ? "Uses native scene lights only on PBR materials. Other surfaces and projected shadows stay native. Scene environment overrides are inactive."
        : "Uses native scene lights and selected local lights. Unsupported native materials keep their original shading.");
    if (UsesPbrMaterials(settings.Effects.SurfaceLighting)) {
        ImGui::TextWrapped(nativePbr
            ? "PBR uses Azahar material packs configured in Textures. Surfaces without a supported material keep original native shading."
            : "PBR uses Azahar material packs configured in Textures. Materials without a pack use native per-pixel lighting.");
        ImGui::TextWrapped("Edit individual materials, maps and PBR response in the Material editor tab.");
    }

    ImGui::SeparatorText("Ambient occlusion");
    const bool cacaoReady = capabilities.Has(GraphicsCapability::NriInterop) &&
                            capabilities.Has(GraphicsCapability::SampledDepth) &&
                            capabilities.Has(GraphicsCapability::ValidViewMetadata);
    const char* const aoModes[] = {"Off", "FidelityFX CACAO"};
    changed |= SettingsUi::ValidatedCombo<AmbientOcclusionMode>(
        "Ambient occlusion", settings, capabilities, aoModes,
        [](auto& value, auto mode) { value.Effects.AmbientOcclusion = mode; },
        [](const auto& value) { return value.Effects.AmbientOcclusion; });
    const bool ao = settings.Effects.AmbientOcclusion == AmbientOcclusionMode::Cacao;
    if (ao) {
        ImGui::BeginDisabled(!cacaoReady);
        int quality = settings.Effects.AoQuality;
        const char* const qualityNames[] = {"Low", "Medium"};
        if (ImGui::Combo("AO quality", &quality, qualityNames, 2)) {
            settings.Effects.AoQuality = static_cast<uint8_t>(quality);
            changed = true;
        }
        changed |= ImGui::SliderFloat("AO radius", &settings.Effects.AoRadius,
                                      0.05F, 100.0F, "%.2f",
                                      ImGuiSliderFlags_Logarithmic);
        changed |= ImGui::SliderFloat("AO strength", &settings.Effects.AoStrength,
                                      0.0F, 10.0F, "%.2f");
        if (ImGui::TreeNode("Advanced CACAO")) {
            changed |= ImGui::SliderFloat("Shadow power",
                                          &settings.Effects.AoShadowPower,
                                          0.1F, 10.0F, "%.2f");
            changed |= ImGui::SliderFloat("Shadow clamp",
                                          &settings.Effects.AoShadowClamp,
                                          0.0F, 1.0F, "%.3f");
            changed |= ImGui::SliderFloat(
                "Horizon threshold",
                &settings.Effects.AoHorizonAngleThreshold,
                0.0F, 1.0F, "%.3f");
            changed |= ImGui::DragFloat("Fade start",
                                        &settings.Effects.AoFadeOutFrom,
                                        1.0F, 0.0F, 100000.0F, "%.1f");
            changed |= ImGui::DragFloat("Fade end",
                                        &settings.Effects.AoFadeOutTo,
                                        1.0F, 0.01F, 100000.0F, "%.1f");
            int blurPasses = settings.Effects.AoBlurPassCount;
            if (ImGui::SliderInt("Blur passes", &blurPasses, 0, 8)) {
                settings.Effects.AoBlurPassCount =
                    static_cast<uint8_t>(blurPasses);
                changed = true;
            }
            changed |= ImGui::SliderFloat("Sharpness",
                                          &settings.Effects.AoSharpness,
                                          0.0F, 1.0F, "%.3f");
            changed |= ImGui::SliderFloat("Detail strength",
                                          &settings.Effects.AoDetailStrength,
                                          0.0F, 10.0F, "%.2f");
            ImGui::TreePop();
        }
        ImGui::EndDisabled();
    }
    if (!cacaoReady) {
        ImGui::TextWrapped("CACAO unavailable: NRI, depth or camera metadata missing.");
    }

    return changed;
}
} // namespace Fast::Oot3d
