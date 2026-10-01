#pragma once

#include "fast/oot3d/graphics_settings.h"
#include "fast/oot3d/scene_presentation_policy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Fast::Oot3d {

struct RenderExtent {
    uint32_t Width = 1;
    uint32_t Height = 1;
};

// The smallest internal scale: an upscaler's Ultra Performance renders a third
// of the output per axis. The user's own render scale stays within 0.5-2.
inline constexpr float kMinimumUpscalerRenderScale = 1.0F / 3.0F;

// An upscaler renders its output divided by its quality's factor, so its
// output is the display extent, never larger.
[[nodiscard]] inline float ResolveInternalResolutionScale(
    const GraphicsSettings& settings) {
    if (settings.AntiAliasing == AntiAliasingMode::Upscaler)
        return std::clamp(1.0F / UpscalerScalingFactor(settings.UpscalerMode),
                          kMinimumUpscalerRenderScale, 1.0F);
    return std::clamp(settings.InternalResolutionScale, 0.5F, 2.0F);
}

[[nodiscard]] inline RenderExtent ScaleRenderExtent(
    RenderExtent extent, float scale) {
    const float sanitized = std::isfinite(scale)
        ? std::clamp(scale, kMinimumUpscalerRenderScale, 2.0F) : 1.0F;
    return {
        std::max(1U, static_cast<uint32_t>(std::lround(
                         static_cast<float>(extent.Width) * sanitized))),
        std::max(1U, static_cast<uint32_t>(std::lround(
                         static_cast<float>(extent.Height) * sanitized)))};
}

[[nodiscard]] inline RenderExtent ResolveNativePicaRenderExtent(
    RenderExtent guestExtent, RenderExtent outputExtent,
    float internalScale) {
    RenderExtent base = guestExtent;
    if (guestExtent.Width == kNativeTopScreenPhysicalWidth &&
        guestExtent.Height == kNativeTopScreenPhysicalHeight &&
        outputExtent.Width != 0U && outputExtent.Height != 0U) {
        // The top LCD is stored rotated: physical X is twice logical height.
        base = {outputExtent.Height * 2U, outputExtent.Width};
    }
    return ScaleRenderExtent(base, internalScale);
}

} // namespace Fast::Oot3d
