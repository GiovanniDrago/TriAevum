#pragma once

#include "fast/oot3d/scene_view_runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Fast::Oot3d {

using SampleViewMatrix = std::array<float, 16>;

// The camera of the geometry actually rendered into one target during one
// presented frame: the captured native world-to-view of its opaque world
// draws (interpolated with the geometry for synthetic samples). The view most
// draws agree on wins, so a stray draw with its own view cannot move it.
class SampleViewAccumulator final {
  public:
    void Observe(uint64_t frame, const SampleViewMatrix& worldToView) noexcept;
    [[nodiscard]] std::optional<SampleViewMatrix> Resolve(uint64_t frame) const noexcept;
    // Distinct views observed in `frame` (diagnostics).
    [[nodiscard]] uint32_t DistinctViews(uint64_t frame) const noexcept;

  private:
    struct Candidate {
        SampleViewMatrix View{};
        uint32_t Draws = 0;
    };
    static constexpr size_t kCandidates = 4;
    uint64_t mFrame = 0;
    std::array<Candidate, kCandidates> mCandidates{};
    uint32_t mCount = 0;
};

// The guest projection with the rendered world-to-view. The latest guest
// camera can belong to another simulation step (it runs ahead of the replayed
// geometry) or another interpolation phase; effects that reconstruct or
// reproject positions from the rendered depth must use this view instead.
// The result carries no guest serial: its identity is its view.
[[nodiscard]] PerspectiveViewState ResolveSamplePerspective(const PerspectiveViewState& guest,
                                                            const SampleViewMatrix& worldToView) noexcept;

} // namespace Fast::Oot3d
