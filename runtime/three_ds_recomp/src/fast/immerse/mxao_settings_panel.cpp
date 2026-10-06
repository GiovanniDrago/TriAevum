// iMMERSE module of TriAevum (see LICENSE.md in this directory).
// iMMERSE MXAO, Copyright (c) Pascal Gilcher. All rights reserved.
// Not covered by TriAevum's GPL: this file stays under the original iMMERSE
// licence (LICENSE-iMMERSE.txt) and is used under the author's explicit
// permission, granted exclusively to TriAevum.
//
// The MXAO controls of the F1 Lighting tab: the original's options, preset
// names and descriptions. GraphicsSettingsPanel::DrawAmbientOcclusionSettings
// calls them when MXAO is the ambient occlusion provider, and the credits at
// the foot of Lighting > Ambient Occlusion.
#include "fast/oot3d/graphics_settings_window.h"
#include "fast/oot3d/settings_panel_widgets.h"
#include <type_traits>

namespace Fast::Oot3d {
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
    ImGui::TextWrapped("AO texture relief bends only the renderer normals.");
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

void GraphicsSettingsPanel::DrawMxaoCredits() {
    ImGui::SeparatorText("Credits");
    ImGui::TextWrapped("MXAO: iMMERSE by Pascal Gilcher (martysmods.com). Thanks to the author.");
    ImGui::TextWrapped("Copyright (c) Pascal Gilcher, all rights reserved. Licence exception: used and modified "
                       "with the author's permission, granted to TriAevum only; it stays under its original "
                       "licence, outside TriAevum's GPL.");
}
} // namespace Fast::Oot3d
