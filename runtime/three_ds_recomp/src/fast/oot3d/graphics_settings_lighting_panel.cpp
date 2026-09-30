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

    ImGui::SeparatorText("Global illumination");
    const char* const giModes[] = {"Off", "Brixelizer GI"};
    changed |= SettingsUi::ValidatedCombo<GlobalIlluminationMode>(
        "Global illumination", settings, capabilities, giModes,
        [](auto& value, auto mode) { value.Effects.GlobalIllumination.Mode = mode; },
        [](const auto& value) { return value.Effects.GlobalIllumination.Mode; });
    auto& globalIllumination = settings.Effects.GlobalIllumination;
    if (globalIllumination.Mode != GlobalIlluminationMode::Off) {
        int resolution = static_cast<int>(globalIllumination.Resolution);
        const char* const resolutionNames[] = {"Half", "Quarter"};
        if (ImGui::Combo("GI resolution", &resolution, resolutionNames, 2)) {
            globalIllumination.Resolution = static_cast<GlobalIlluminationResolution>(resolution);
            changed = true;
        }
        changed |= ImGui::SliderFloat("GI diffuse intensity", &globalIllumination.DiffuseIntensity,
                                      0.0F, 8.0F, "%.2f");
        changed |= ImGui::SliderFloat("GI native scene light", &globalIllumination.SceneLight, 0.0F, 4.0F, "%.2f");
        changed |= ImGui::SliderFloat("GI key light", &globalIllumination.KeyLight, 0.0F, 4.0F, "%.2f");
        changed |= ImGui::SliderFloat("GI dynamic lights", &globalIllumination.DynamicLights, 0.0F, 4.0F, "%.2f");
        changed |= ImGui::SliderFloat("GI sky light", &globalIllumination.SkyLight, 0.0F, 4.0F, "%.2f");
        ImGui::TextWrapped("GI bounces the direct light of the native scene directionals, the added key light "
                           "and dynamic lights (native local lights, Navi, fairies), and adds the sky light that "
                           "open surfaces receive (the native sky colour). Ambient and fill light are baked into "
                           "OoT's colours and are never bounced.");
        ImGui::TextWrapped("Intensity, the strongest source and switching a source off apply at once; other "
                           "changes of the source mix settle over a few seconds.");
        changed |= ImGui::Checkbox("GI specular reflections", &globalIllumination.Specular);
        if (globalIllumination.Specular) {
            changed |= ImGui::SliderFloat("GI specular intensity", &globalIllumination.SpecularIntensity,
                                          0.0F, 2.0F, "%.2f");
            if (settings.Effects.Reflections != ReflectionMode::Off)
                ImGui::TextWrapped("Screen-space reflections are enabled and provide the reflections; "
                                   "GI specular applies when they are off.");
        }
        if (!UsesFullSceneLighting(settings.Effects.SurfaceLighting))
            ImGui::TextWrapped("Active only with Per-pixel or Per-pixel + PBR lighting.");
    }
    if (!capabilities.Has(GraphicsCapability::BrixelizerGlobalIllumination))
        ImGui::TextWrapped("Brixelizer GI unavailable: %s",
                           capabilities.Get(GraphicsCapability::BrixelizerGlobalIllumination).Reason.c_str());

    ImGui::SeparatorText("Ambient occlusion");
    const bool aoReady = capabilities.Has(GraphicsCapability::NriInterop) &&
                         capabilities.Has(GraphicsCapability::SampledDepth) &&
                         capabilities.Has(GraphicsCapability::NormalGuide) &&
                         capabilities.Has(GraphicsCapability::ValidViewMetadata);
    const char* const aoModes[] = {"Off", "Multiscale GTAO", "FidelityFX CACAO", "iMMERSE MXAO"};
    changed |= SettingsUi::ValidatedCombo<AmbientOcclusionMode>(
        "Ambient occlusion", settings, capabilities, aoModes,
        [](auto& value, auto mode) { value.Effects.AmbientOcclusion = mode; },
        [](const auto& value) { return value.Effects.AmbientOcclusion; });
    const bool cacao = settings.Effects.AmbientOcclusion == AmbientOcclusionMode::Cacao;
    const bool mxao = settings.Effects.AmbientOcclusion == AmbientOcclusionMode::Mxao;
    const bool ao = settings.Effects.AmbientOcclusion != AmbientOcclusionMode::Off;
    if (mxao) {
        ImGui::BeginDisabled(!aoReady || !capabilities.Has(GraphicsCapability::ImmerseMxao));
        changed |= DrawMxaoSettings(settings.Effects);
        ImGui::EndDisabled();
    } else if (ao) {
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
    if (ao) {
        ImGui::BeginDisabled(!aoReady);
        const char* const normalSources[] = {"Native", "Smoothed (LAUNCHPAD)", "Texture detail (LAUNCHPAD)",
                                             "Smoothed + texture detail (LAUNCHPAD)"};
        changed |= SettingsUi::ValidatedCombo<AoNormalSource>(
            "AO normals", settings, capabilities, normalSources,
            [](auto& value, auto source) { value.Effects.AoNormals = source; },
            [](const auto& value) { return value.Effects.AoNormals; });
        auto& effects = settings.Effects;
        if (TexturesAoNormals(effects.AoNormals)) {
            int quality = effects.AoTextureDetailQuality;
            const char* const qualityNames[] = {"Low", "Medium", "High", "Very high", "Ultra"};
            if (ImGui::Combo("Texture detail quality", &quality, qualityNames, 5)) {
                effects.AoTextureDetailQuality = static_cast<uint8_t>(quality);
                changed = true;
            }
            changed |= ImGui::SliderFloat("Texture detail radius", &effects.AoTextureDetailRadius, 0.0F, 1.0F,
                                          "%.2f");
            changed |= ImGui::SliderFloat("Texture detail intensity", &effects.AoTextureDetailIntensity, 0.0F,
                                          1.0F, "%.2f");
        }
        ImGui::EndDisabled();
        if (effects.AoNormals != AoNormalSource::Native)
            ImGui::TextWrapped("Smoothing softens faceted geometry and keeps creases; texture detail adds surface "
                               "relief from the scene's textures. From iMMERSE LAUNCHPAD by Pascal Gilcher "
                               "(martysmods.com), used with the author's permission.");
    }
    if (!aoReady) {
        ImGui::TextWrapped("AO unavailable: NRI, depth, normals or camera metadata missing.");
    }
    if (!capabilities.Has(GraphicsCapability::FidelityFxCacao))
        ImGui::TextWrapped("CACAO unavailable: %s", capabilities.Get(GraphicsCapability::FidelityFxCacao).Reason.c_str());
    if (!capabilities.Has(GraphicsCapability::ImmerseMxao))
        ImGui::TextWrapped("MXAO unavailable: %s", capabilities.Get(GraphicsCapability::ImmerseMxao).Reason.c_str());
    if (!capabilities.Has(GraphicsCapability::ImmerseNormals))
        ImGui::TextWrapped("LAUNCHPAD normals unavailable: %s",
                           capabilities.Get(GraphicsCapability::ImmerseNormals).Reason.c_str());

    return changed;
}

bool GraphicsSettingsPanel::DrawMxaoSettings(EffectsSettings& effects) {
    bool changed = false;
    int quality = effects.MxaoQuality;
    const char* const qualityNames[] = {"Low", "Medium", "High", "Very high", "Ultra", "Extreme"};
    if (ImGui::Combo("MXAO quality", &quality, qualityNames, 6)) {
        effects.MxaoQuality = static_cast<uint8_t>(quality);
        changed = true;
    }
    int method = static_cast<int>(effects.MxaoMethod);
    const char* const methodNames[] = {"Ground truth", "Solid angle", "Visibility bitmask",
                                       "Visibility bitmask, solid angle"};
    if (ImGui::Combo("MXAO method", &method, methodNames, 4)) {
        effects.MxaoMethod = static_cast<MxaoEstimator>(method);
        changed = true;
    }
    changed |= ImGui::SliderFloat("MXAO radius", &effects.AoRadius, 0.05F, 100.0F, "%.2f",
                                  ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::SliderFloat("MXAO amount", &effects.AoStrength, 0.0F, 2.0F, "%.2f");
    int filter = effects.MxaoFilter;
    if (ImGui::SliderInt("MXAO filter quality", &filter, 0, 2)) {
        effects.MxaoFilter = static_cast<uint8_t>(filter);
        changed = true;
    }
    ImGui::TextWrapped("iMMERSE MXAO by Pascal Gilcher (martysmods.com), used with the author's permission. "
                       "The radius uses native scene units; above 1 the amount deepens occlusion.");
    if (ImGui::TreeNode("Advanced AO")) {
        changed |= ImGui::DragFloat("Fade start", &effects.AoFadeOutFrom, 1.0F, 0.0F, 100000.0F, "%.1f");
        changed |= ImGui::DragFloat("Fade end", &effects.AoFadeOutTo, 1.0F, 0.01F, 100000.0F, "%.1f");
        ImGui::TreePop();
    }
    return changed;
}
} // namespace Fast::Oot3d
