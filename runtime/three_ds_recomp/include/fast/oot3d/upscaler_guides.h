#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace Fast::Oot3d {

// A temporal upscaler reconstructs at its colour input's extent, its render
// resolution. Its depth, motion and reactive guides keep the scene target's
// extent, which differs when a native display transfer resized the colour
// (OoT3D renders its top screen twice as wide and halves it). The guides are
// then resampled to the colour's extent: each output texel covers the source
// texels of its footprint, takes the depth and motion of the nearest of them
// and the largest reactive value.

struct UpscalerGuideSpan {
    uint32_t First = 0U;
    uint32_t Last = 0U;
};

// At most this many source texels per axis enter one output texel.
inline constexpr uint32_t kUpscalerGuideMaxSpan = 4U;

// The source texels [First, Last] that output texel `pixel` covers along one
// axis of an `extent`-texel output resampled from `sourceExtent` texels.
[[nodiscard]] UpscalerGuideSpan ResolveUpscalerGuideSpan(
    uint32_t pixel, uint32_t extent, uint32_t sourceExtent) noexcept;

[[nodiscard]] bool UpscalerGuidesNeedResample(
    uint32_t colorWidth, uint32_t colorHeight,
    uint32_t guideWidth, uint32_t guideHeight) noexcept;

// Projection jitter is applied in scene-target pixels; an upscaler takes it in
// pixels of its input.
[[nodiscard]] std::array<float, 2> UpscalerJitterInInputPixels(
    const std::array<float, 2>& targetPixels,
    uint32_t targetWidth, uint32_t targetHeight,
    uint32_t inputWidth, uint32_t inputHeight) noexcept;

// Push constants of the resample shader.
struct UpscalerGuideConstants {
    uint32_t Width = 0U;
    uint32_t Height = 0U;
    uint32_t SourceWidth = 0U;
    uint32_t SourceHeight = 0U;
};
static_assert(sizeof(UpscalerGuideConstants) == 16);

// Bindings: 0 depth (larger is farther), 1 motion, 2 reactive (sampled);
// 3 R32F depth, 4 RGBA16F motion, 5 R16F reactive (storage); 6 sampler.
[[nodiscard]] std::string BuildUpscalerGuideResampleShader();

} // namespace Fast::Oot3d
