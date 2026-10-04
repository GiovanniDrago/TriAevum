#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "fast/renderer3ds/pica_composition.h"
#include "fast/renderer3ds/pica_ui_canvas.h"

namespace Oot3d::Renderer {

struct UiPresentationRect {
    float X = 0.0F;
    float Y = 0.0F;
    float Width = 0.0F;
    float Height = 0.0F;

    [[nodiscard]] bool Valid() const noexcept {
        return Width > 0.0F && Height > 0.0F;
    }
};

struct UiPresentationViewport {
    std::uint32_t X = 0U;
    std::uint32_t Y = 0U;
    std::uint32_t Width = 0U;
    std::uint32_t Height = 0U;

    [[nodiscard]] bool Valid() const noexcept {
        return Width > 0U && Height > 0U;
    }
};

struct UiPresentationPoint {
    float X = 0.0F;
    float Y = 0.0F;
    bool Inside = false;
};

[[nodiscard]] inline UiPresentationRect FitUiPresentation(
    std::uint32_t targetWidth, std::uint32_t targetHeight, float logicalWidth,
    float logicalHeight) noexcept {
    if (targetWidth == 0U || targetHeight == 0U ||
        !(logicalWidth > 0.0F) || !(logicalHeight > 0.0F)) {
        return {};
    }

    float presentWidth = static_cast<float>(targetWidth);
    float presentHeight = presentWidth * logicalHeight / logicalWidth;
    if (presentHeight > static_cast<float>(targetHeight)) {
        presentHeight = static_cast<float>(targetHeight);
        presentWidth = presentHeight * logicalWidth / logicalHeight;
    }
    return {
        (static_cast<float>(targetWidth) - presentWidth) * 0.5F,
        (static_cast<float>(targetHeight) - presentHeight) * 0.5F,
        presentWidth,
        presentHeight,
    };
}

[[nodiscard]] inline UiPresentationRect FillUiPresentation(
    std::uint32_t targetWidth, std::uint32_t targetHeight, float logicalWidth,
    float logicalHeight) noexcept {
    if (targetWidth == 0U || targetHeight == 0U ||
        !(logicalWidth > 0.0F) || !(logicalHeight > 0.0F)) {
        return {};
    }

    float presentWidth = static_cast<float>(targetWidth);
    float presentHeight = presentWidth * logicalHeight / logicalWidth;
    if (presentHeight < static_cast<float>(targetHeight)) {
        presentHeight = static_cast<float>(targetHeight);
        presentWidth = presentHeight * logicalWidth / logicalHeight;
    }
    return {
        (static_cast<float>(targetWidth) - presentWidth) * 0.5F,
        (static_cast<float>(targetHeight) - presentHeight) * 0.5F,
        presentWidth,
        presentHeight,
    };
}

[[nodiscard]] inline UiPresentationViewport FitUiPresentationViewport(
    std::uint32_t targetWidth, std::uint32_t targetHeight, float logicalWidth,
    float logicalHeight) noexcept {
    const UiPresentationRect fitted =
        FitUiPresentation(targetWidth, targetHeight, logicalWidth,
                          logicalHeight);
    if (!fitted.Valid()) {
        return {};
    }

    const std::uint32_t width = std::clamp(
        static_cast<std::uint32_t>(std::lround(fitted.Width)), 1U,
        targetWidth);
    const std::uint32_t height = std::clamp(
        static_cast<std::uint32_t>(std::lround(fitted.Height)), 1U,
        targetHeight);
    return {
        (targetWidth - width) / 2U,
        (targetHeight - height) / 2U,
        width,
        height,
    };
}

[[nodiscard]] inline UiPresentationPoint MapUiPresentationPoint(
    std::uint32_t targetWidth, std::uint32_t targetHeight, float logicalWidth,
    float logicalHeight, float targetX, float targetY) noexcept {
    const UiPresentationRect fitted =
        FitUiPresentation(targetWidth, targetHeight, logicalWidth,
                          logicalHeight);
    if (!fitted.Valid() || !std::isfinite(targetX) ||
        !std::isfinite(targetY) || targetX < fitted.X ||
        targetY < fitted.Y || targetX >= fitted.X + fitted.Width ||
        targetY >= fitted.Y + fitted.Height) {
        return {};
    }

    return {
        (targetX - fitted.X) * logicalWidth / fitted.Width,
        (targetY - fitted.Y) * logicalHeight / fitted.Height,
        true,
    };
}

// Native framebuffer coordinates include the LCD rotation/sampling convention.
// Fit in that coordinate system, not in host logical screen coordinates.
// Ownership, not an orthographic matrix or shader mode, distinguishes UI.
// Title-side: it applies this title's presentation convention and placement
// policy, so it stays out of the title-neutral renderer boundary.
[[nodiscard]] inline Fast::Renderer3ds::PicaRasterCanvas ResolvePicaRasterCanvas(
    Fast::Renderer3ds::PicaCompositionDomain domain,
    Fast::Renderer3ds::PicaScreenContent screenContent,
    std::uint32_t nativeWidth, std::uint32_t nativeHeight,
    std::uint32_t targetWidth, std::uint32_t targetHeight) noexcept {
    if (!nativeWidth || !nativeHeight || !targetWidth || !targetHeight) {
        return {};
    }
    if (domain == Fast::Renderer3ds::PicaCompositionDomain::Ui) {
        const auto fit = FitUiPresentation(
            targetWidth, targetHeight, static_cast<float>(nativeWidth),
            static_cast<float>(nativeHeight));
        return {fit.X, fit.Y, fit.Width / nativeWidth, fit.Height / nativeHeight};
    }
    if (domain == Fast::Renderer3ds::PicaCompositionDomain::Screen &&
        screenContent == Fast::Renderer3ds::PicaScreenContent::OwnPicture) {
        // A picture of its own (a movie frame) covers the presentation extent
        // without distortion: native proportions kept, the excess centred, so
        // nothing behind it shows around it.
        const auto cover = FillUiPresentation(
            targetWidth, targetHeight, static_cast<float>(nativeWidth),
            static_cast<float>(nativeHeight));
        return {cover.X, cover.Y, cover.Width / nativeWidth,
                cover.Height / nativeHeight};
    }
    // The scene, and screen content that shows the rendered frame: a redraw of
    // the copied frame must land on the pixels it copied (the copy holds the
    // scene as this mapping rasterised it), and a wash reaches every pixel.
    return {0, 0, static_cast<float>(targetWidth) / nativeWidth,
            static_cast<float>(targetHeight) / nativeHeight};
}

} // namespace Oot3d::Renderer
