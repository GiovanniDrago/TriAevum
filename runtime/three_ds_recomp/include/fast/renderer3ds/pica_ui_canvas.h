#pragma once

#include <cstdint>
#include <algorithm>
#include <cmath>

namespace Fast::Renderer3ds {

struct PicaRasterCanvas {
    float X = 0;
    float Y = 0;
    float ScaleX = 1;
    float ScaleY = 1;
};

struct PicaCanvasClip {
    int32_t X = 0, Y = 0;
    uint32_t Width = 0, Height = 0;
};

inline PicaCanvasClip ClipToPicaCanvas(
    PicaCanvasClip scissor, const PicaRasterCanvas& canvas,
    uint32_t nativeWidth, uint32_t nativeHeight) noexcept {
    const auto x1 = std::max<int64_t>(scissor.X, static_cast<int64_t>(std::ceil(canvas.X)));
    const auto y1 = std::max<int64_t>(scissor.Y, static_cast<int64_t>(std::ceil(canvas.Y)));
    const auto x2 = std::min<int64_t>(int64_t(scissor.X) + scissor.Width,
        static_cast<int64_t>(std::floor(canvas.X + nativeWidth * canvas.ScaleX)));
    const auto y2 = std::min<int64_t>(int64_t(scissor.Y) + scissor.Height,
        static_cast<int64_t>(std::floor(canvas.Y + nativeHeight * canvas.ScaleY)));
    return {static_cast<int32_t>(x1), static_cast<int32_t>(y1),
            static_cast<uint32_t>(std::max<int64_t>(0, x2 - x1)),
            static_cast<uint32_t>(std::max<int64_t>(0, y2 - y1))};
}

} // namespace Fast::Renderer3ds
