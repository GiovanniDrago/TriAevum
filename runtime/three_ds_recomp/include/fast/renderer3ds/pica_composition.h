#pragma once

#include <cstdint>
#include <span>

namespace Fast::Renderer3ds {

enum class PicaCompositionDomain : uint8_t {
    Unknown = 0,
    Scene = 1,
    Ui = 2,
    // Two-dimensional content the title composites over the whole native
    // screen: a full-frame redraw of the copied frame, a full-screen wash, a
    // movie frame. It is frame content rather than a placed UI element, so it
    // reaches every output pixel instead of being inset in the uniform native
    // UI canvas (PicaScreenContent says how). Placed UI keeps `Ui`: HUD, menu
    // layout and pointer mapping depend on that exact rectangle.
    Screen = 3,
};

// What screen-space frame content shows, which decides its mapping onto a
// target wider or narrower than the native screen.
enum class PicaScreenContent : uint8_t {
    None = 0,
    // The rendered frame itself - a redraw of a copy of rendered output, or a
    // wash over it: it takes the scene's own mapping, so a redraw lands exactly
    // on the pixels it copied.
    RenderedFrame = 1,
    // A picture of its own, such as a movie frame: it covers the target
    // without distortion, native proportions kept and the excess centred.
    OwnPicture = 2,
};

// What a UI-attributed draw rasterises, as far as frame content is concerned.
struct PicaUiFrameContentEvidence {
    // The draw's viewport covers its whole colour target.
    bool CoversWholeTarget = false;
    // This frame's scene drew into that colour target.
    bool SceneDrewOnTarget = false;
    // Its projected primitives cover the whole viewport.
    bool GeometryCoversViewport = false;
    // It samples no texture: a solid or vertex-coloured wash.
    bool Untextured = false;
    // It samples at least one texture, and each is owned by an ordered GPU
    // copy of rendered output.
    bool SamplesOnlyRenderedCopies = false;
};

struct PicaUiFrameContent {
    PicaCompositionDomain Domain = PicaCompositionDomain::Unknown;
    PicaScreenContent Content = PicaScreenContent::None;

    bool operator==(const PicaUiFrameContent&) const = default;
};

// A UI-attributed draw is screen-space frame content only when it covers the
// frame itself: its geometry covers the whole colour target the scene drew
// into. Placed UI - HUD, minimap, dialogue, menus - also draws over the scene
// with a whole-target viewport, but its primitives cover part of it, so it
// stays `Ui`. Covering content that washes the frame or redraws a copy of
// rendered output shows the rendered frame; anything else is its own picture.
[[nodiscard]] constexpr PicaUiFrameContent ResolvePicaUiFrameContent(
    PicaCompositionDomain declared,
    const PicaUiFrameContentEvidence& evidence) noexcept {
    if (declared != PicaCompositionDomain::Ui || !evidence.CoversWholeTarget ||
        !evidence.SceneDrewOnTarget || !evidence.GeometryCoversViewport) {
        return {declared, PicaScreenContent::None};
    }
    return {PicaCompositionDomain::Screen,
            evidence.Untextured || evidence.SamplesOnlyRenderedCopies
                ? PicaScreenContent::RenderedFrame
                : PicaScreenContent::OwnPicture};
}

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
    // Native actor draw ownership, independent of skinning and material.
    bool NativeActorGeometry = false;
    uint64_t NativeActorIdentity = 0;

    bool operator==(const PicaCompositionAttribution&) const = default;
};

inline constexpr uint32_t kPicaCompositionSequenceSchemaVersion = 4U;

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
