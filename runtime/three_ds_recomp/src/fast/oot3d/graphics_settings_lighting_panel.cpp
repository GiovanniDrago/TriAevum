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
    const bool aoReady = capabilities.Has(GraphicsCapability::NriInterop) &&
                         capabilities.Has(GraphicsCapability::SampledDepth) &&
                         capabilities.Has(GraphicsCapability::NormalGuide) &&
                         capabilities.Has(GraphicsCapability::ValidViewMetadata);
    const char* const aoModes[] = {"Off", "Multiscale GTAO", "FidelityFX CACAO"};
    changed |= SettingsUi::ValidatedCombo<AmbientOcclusionMode>(
        "Ambient occlusion", settings, capabilities, aoModes,
        [](auto& value, auto mode) { value.Effects.AmbientOcclusion = mode; },
        [](const auto& value) { return value.Effects.AmbientOcclusion; });
    const bool cacao = settings.Effects.AmbientOcclusion == AmbientOcclusionMode::Cacao;
    const bool ao = settings.Effects.AmbientOcclusion != AmbientOcclusionMode::Off;
    if (ao) {
        ImGui::BeginDisabled(!aoReady || (cacao && !capabilities.Has(GraphicsCapability::FidelityFxCacao)));
        int quality = settings.Effects.AoQuality;
        const char* const qualityNames[] = {"Low", "Medium"};
        if (ImGui::Combo("AO quality", &quality, qualityNames, 2)) {
            settings.Effects.AoQuality = static_cast<uint8_t>(quality);
            changed = true;
        }
        changed |= ImGui::SliderFloat(cacao ? "AO radius" : "Broad radius", &settings.Effects.AoRadius,
                                      0.05F, 100.0F, "%.2f", ImGuiSliderFlags_Logarithmic);
        if (!cacao)
            changed |= ImGui::SliderFloat("Fine radius", &settings.Effects.AoDetailRadius,
                                          0.01F, settings.Effects.AoRadius, "%.2f", ImGuiSliderFlags_Logarithmic);
        changed |= ImGui::SliderFloat(cacao ? "Detail strength" : "Fine strength", &settings.Effects.AoDetailStrength,
                                      0.0F, 10.0F, "%.2f");
        changed |= ImGui::SliderFloat(cacao ? "AO strength" : "Broad strength", &settings.Effects.AoStrength,
                                      0.0F, 10.0F, "%.2f");
        if (!cacao)
            ImGui::TextWrapped("Separate gains for fine contacts and additional broad occlusion. Radii use native scene units.");
        if (ImGui::TreeNode("Advanced AO")) {
            changed |= ImGui::SliderFloat("Shadow power",
                                          &settings.Effects.AoShadowPower,
                                          0.1F, 10.0F, "%.2f");
            changed |= ImGui::SliderFloat("Shadow clamp",
                                          &settings.Effects.AoShadowClamp,
                                          0.0F, 1.0F, "%.3f");
            if (cacao) {
                changed |= ImGui::SliderFloat("Horizon angle threshold", &settings.Effects.AoHorizonAngleThreshold,
                                              0.0F, 0.2F, "%.3f");
                int blurPasses = static_cast<int>(settings.Effects.AoBlurPassCount);
                if (ImGui::SliderInt("Blur passes", &blurPasses, 0, 8)) {
                    settings.Effects.AoBlurPassCount = static_cast<uint32_t>(blurPasses);
                    changed = true;
                }
            }
            changed |= ImGui::DragFloat("Fade start",
                                        &settings.Effects.AoFadeOutFrom,
                                        1.0F, 0.0F, 100000.0F, "%.1f");
            changed |= ImGui::DragFloat("Fade end",
                                        &settings.Effects.AoFadeOutTo,
                                        1.0F, 0.01F, 100000.0F, "%.1f");
            changed |= ImGui::SliderFloat("Sharpness",
                                          &settings.Effects.AoSharpness,
                                          0.0F, 1.0F, "%.3f");
            ImGui::TreePop();
        }
        ImGui::EndDisabled();
    }
    if (!aoReady) {
        ImGui::TextWrapped("AO unavailable: NRI, depth, normals or camera metadata missing.");
    }
    if (!capabilities.Has(GraphicsCapability::FidelityFxCacao))
        ImGui::TextWrapped("CACAO unavailable: %s", capabilities.Get(GraphicsCapability::FidelityFxCacao).Reason.c_str());

    return changed;
}
} // namespace Fast::Oot3d
