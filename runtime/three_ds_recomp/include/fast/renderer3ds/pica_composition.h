#pragma once

#include <cstdint>
#include <span>

namespace Fast::Renderer3ds {

enum class PicaCompositionDomain : uint8_t {
    Unknown = 0,
    Scene = 1,
    Ui = 2,
    // Two-dimensional content the title composites over the whole native
    // screen: movie frames, full-frame effect re-draws, screen-space
    // post-process. It is frame content rather than a placed UI element, so it
    // is rasterised over the presentation extent like the scene - the frame has
    // one owner and the title's own effect reaches the whole frame - instead of
    // being inset in the uniform native UI canvas. Placed UI keeps `Ui`: HUD,
    // menu layout and pointer mapping depend on that exact rectangle.
    Screen = 3,
};

enum class PicaCompositionLayer : uint8_t {
    Unknown = 0,
    OpaqueWorld = 1,
    TransparentWorld = 2,
    Atmosphere = 3,
    Ui = 4,
};

// Only proven world surfaces admit replacement illumination. Atmosphere owns
// its authored appearance; unknown ownership is not evidence of a lit surface.
constexpr bool PicaCompositionReceivesSurfaceLighting(PicaCompositionLayer layer) noexcept {
    return layer == PicaCompositionLayer::OpaqueWorld || layer == PicaCompositionLayer::TransparentWorld;
}

enum class PicaCompositionProvenance : uint8_t {
    Unknown = 0,
    NativeCmbDrawPass = 1,
    NativeControlFlow = 2,
    NativeUiLifecycle = 3,
};

struct PicaCompositionAttribution {
    PicaCompositionLayer Layer = PicaCompositionLayer::Unknown;
    PicaCompositionProvenance Provenance =
        PicaCompositionProvenance::Unknown;
    uint32_t SourcePc = 0;
    uint32_t NativeValue = 0;
    // Title-proven native projected/blob shadow, not inferred from material.
    bool NativeProjectedShadow = false;

    bool operator==(const PicaCompositionAttribution&) const = default;
};

inline constexpr uint32_t kPicaCompositionSequenceSchemaVersion = 3U;

struct PicaCompositionTargetReference {
    uint64_t RenderTargetNamespace = 0;
    uint32_t ColorPhysicalAddress = 0;
    uint32_t DepthPhysicalAddress = 0;
    uint16_t FramebufferWidth = 0;
    uint16_t FramebufferHeight = 0;
    uint8_t ColorFormat = 0;
    uint8_t DepthFormat = 0;

    bool operator==(const PicaCompositionTargetReference&) const = default;
};

struct PicaCompositionDrawReference {
    uint64_t SubmissionId = 0;
    PicaCompositionTargetReference Target;
    PicaCompositionDomain Domain = PicaCompositionDomain::Unknown;
    PicaCompositionAttribution Composition;
};

// Native memory visibility uses canonical raster results. Presentation may
// compose extensions and interpolate, but never writes those results to RAM.
enum class PicaCompositionPurpose : uint8_t {
    Presentation,
    NativeMemory,
};

// Complete ordered metadata for one selected PICA visual sample. The title
// frontend owns the span; a backend retaining the schedule copies only this
// compact metadata surface.
struct PicaCompositionSequenceView {
    uint32_t SchemaVersion = kPicaCompositionSequenceSchemaVersion;
    uint64_t SequenceId = 0;
    std::span<const PicaCompositionDrawReference> Draws;
    PicaCompositionPurpose Purpose = PicaCompositionPurpose::Presentation;
};

} // namespace Fast::Renderer3ds
