#include "fast/oot3d/graphics_settings_window.h"
#include "fast/oot3d/graphics_settings_runtime.h"
#include "fast/oot3d/settings_panel_widgets.h"
#include <type_traits>

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
        int relief = static_cast<int>(settings.Effects.AoRelief);
        const char* const reliefNames[] = {"Off", "Subtle", "Medium", "Strong"};
        if (ImGui::Combo("AO texture relief", &relief, reliefNames, 4)) {
            settings.Effects.AoRelief = static_cast<AoTextureRelief>(relief);
            changed = true;
        }
        ImGui::TextWrapped("Relief from each world material's colour texture, added to the scene normals the AO "
                           "reads (reflections and GI read the same normals).");
        ImGui::EndDisabled();
    }
    if (!aoReady) {
        ImGui::TextWrapped("AO unavailable: NRI, depth, normals or camera metadata missing.");
    }
    if (!capabilities.Has(GraphicsCapability::FidelityFxCacao))
        ImGui::TextWrapped("CACAO unavailable: %s", capabilities.Get(GraphicsCapability::FidelityFxCacao).Reason.c_str());
    if (!capabilities.Has(GraphicsCapability::ImmerseMxao))
        ImGui::TextWrapped("MXAO unavailable: %s", capabilities.Get(GraphicsCapability::ImmerseMxao).Reason.c_str());

    return changed;
}

bool GraphicsSettingsPanel::DrawMxaoSettings(EffectsSettings& effects) {
    bool changed = false;
    const auto combo = [&](const char* label, auto& field, const char* const* names, int count,
                           const char* tooltip) {
        int index = static_cast<int>(field);
        // Every entry visible: a scrolled list hides the first ones.
        if (ImGui::Combo(label, &index, names, count, count)) {
            field = static_cast<std::remove_reference_t<decltype(field)>>(index);
            changed = true;
        }
        if (tooltip != nullptr && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip);
    };
    const char* const qualityNames[] = {"Low", "Medium", "High", "Very high", "Ultra", "Extreme", "IDGAF"};
    combo("MXAO quality", effects.MxaoQuality, qualityNames, 7,
          "Global quality control, main performance knob. Higher radii might require higher quality.");
    const char* const shadingRates[] = {"Full rate", "Half rate", "Quarter rate"};
    combo("MXAO shading rate", effects.MxaoShadingRate, shadingRates, 3,
          "Full: every pixel each frame. Half / Quarter: 50% / 25% of the pixels each frame; the others keep "
          "their previous estimate, which trails moving content.");
    const char* const methodNames[] = {"Ground truth", "Solid angle", "Visibility bitmask",
                                       "Visibility bitmask, solid angle"};
    combo("MXAO method", effects.MxaoMethod, methodNames, 4,
          "Ground truth: high contrast. Solid angle: smoother. Visibility bitmask: highest quality, slower. "
          "Bitmask with solid angle: like the bitmask, smoother.");
    const char* const normalNames[] = {"From depth (original)", "Renderer normals", "Face normals"};
    combo("MXAO normals", effects.MxaoNormals, normalNames, 3,
          "From depth: the plane of the depth samples MXAO tests, as the original computes it. Renderer normals: "
          "interpolated vertex normals, PBR normal maps and texture relief (like Launchpad's smooth normals); "
          "where they tilt away from the surface, as across sharp corners, the surface occludes itself and "
          "darkens. Face normals (default): the exact plane of each visible triangle, the depth plane without "
          "its reconstruction across silhouettes.");
    changed |= ImGui::Checkbox("Increase radius with distance", &effects.MxaoRadiusGrowsWithDistance);
    if (effects.MxaoRadiusGrowsWithDistance)
        changed |= ImGui::SliderFloat("MXAO sample radius", &effects.MxaoScreenRadius, 0.5F, 10.0F, "%.2f");
    else
        changed |= ImGui::SliderFloat("MXAO radius", &effects.AoRadius, 0.05F, 100.0F, "%.2f",
                                      ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(effects.MxaoRadiusGrowsWithDistance
            ? "The original's sample radius: a constant screen radius (3% of the width per unit), so the "
              "world radius grows with distance."
            : "World radius in native scene units: higher means more large-scale occlusion with less "
              "fine-scale detail.");
    changed |= ImGui::SliderFloat("MXAO amount", &effects.AoStrength, 0.0F, 2.0F, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Intensity of the AO. Above 1 the original switches to a gamma response.");
    int filter = effects.MxaoFilter;
    if (ImGui::SliderInt("MXAO filter quality", &filter, 0, 2)) {
        effects.MxaoFilter = static_cast<uint8_t>(filter);
        changed = true;
    }
    changed |= ImGui::Checkbox("MXAO temporal accumulation", &effects.MxaoTemporal);
    if (effects.MxaoTemporal)
        ImGui::TextWrapped("Each frame samples other directions and the scene's motion vectors carry the "
                           "result over frames: a lower quality preset gives a stable image.");
    ImGui::TextWrapped("iMMERSE MXAO by Pascal Gilcher (martysmods.com), used with the author's permission. "
                       "AO texture relief bends only the renderer normals.");
    if (ImGui::TreeNode("Advanced AO")) {
        changed |= ImGui::Checkbox("Original fade", &effects.MxaoOriginalFade);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The original's exponential Fade Out Distance, over depth normalized by the far "
                              "plane, instead of the fade start and end.");
        if (effects.MxaoOriginalFade) {
            changed |= ImGui::SliderFloat("Fade out distance", &effects.MxaoFadeDistance, 0.0F, 1.0F, "%.3f");
        } else {
            changed |= ImGui::DragFloat("Fade start", &effects.AoFadeOutFrom, 1.0F, 0.0F, 100000.0F, "%.1f");
            changed |= ImGui::DragFloat("Fade end", &effects.AoFadeOutTo, 1.0F, 0.01F, 100000.0F, "%.1f");
        }
        ImGui::TreePop();
    }
    const char* const debugViews[] = {"Off", "Ambient occlusion", "Unfiltered occlusion", "Normals in use",
                                      "Renderer normals", "Depth normals", "Normal disagreement", "Depth",
                                      "Edge weight", "Fade", "Temporal history", "Face normals",
                                      "Face vs depth normals"};
    combo("MXAO debug view", effects.MxaoDebug, debugViews, 13,
          "Replaces the image. Ambient occlusion is the original's Show Raw AO; the others show the depth and "
          "normals the renderer passes and the estimate's terms. Normals: view space (X right, Y up, Z toward "
          "the viewer). Normal disagreement (renderer vs depth): black = equal, white = 90 degrees apart. Face "
          "vs depth normals: black = equal, white = 10 degrees or more apart. Not saved.");
    return changed;
}
} // namespace Fast::Oot3d
