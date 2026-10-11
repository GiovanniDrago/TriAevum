# Native room overlays and grass placement

Round 35, 2026-10-11. Native decals previously failed grass-source admission,
but had no way to suppress placements from the surface underneath them. This
produced grass through the Graveyard stone path.

## Capture and coverage

Grass now observes room overlays during the existing composition-sequence
preparation pass. This is necessary because grass can be inserted at the opaque
boundary before native order reaches the translucent decals. Preparation only
captures immutable CPU data; it does not submit or reorder native draws.

`grass_scene_bridge` keeps overlays separate from its grass-source index. A
source can use only later overlays in the same frame, composition sequence,
render-target namespace and color framebuffer. Configured, eligible grass
sources do not become masks. Actor geometry, projected shadows, stencil draws,
skinned geometry, unsupported UV programs and non-alpha-over blends are excluded.

`grass_overlay_mask` prepares world triangles once per cached overlay set. For
each source triangle it requires overlapping bounds, normals within one degree,
and all overlay vertices within **one world unit (1 cm)** of the source plane.
Distance is measured along the source normal, so this also works on slopes.
Roots are tested with triangle barycentrics, not the overlay's bounding rectangle.
Surfaces further above the plane and crossing walls do not mask the source.

The mask evaluates native TEV alpha, including texture-zero RGBA operands,
primary alpha, per-stage constants, previous/buffer alpha, operands, scales and
8-bit stage quantization. Native alpha-test comparisons use the resulting byte.
Opaque overlays cover fully where their alpha test passes; opaque overlays
without an alpha test cover fully without requiring a texture. Alpha-over
overlays use their output alpha after the alpha test. Unsupported dependencies
(fragment lighting, other texture units/procedural sources, previous RGB or
DOT3_RGBA) decline the mask instead of assuming opaque coverage.

Primary alpha is recovered from the typed native CMB surface program and its
captured uniforms/vertex input. The translated CMB instructions 77–115 in
`tools/oot3d/native_pica_frontend/oot3d_translated_vertex_programs.h` establish
the contract: lit alpha is material diffuse alpha times the sum of enabled light
diffuse alphas, without the normal dot product; lit vertex tint multiplies it;
unlit vertex tint replaces material alpha. The registry applies native vertex
`min(abs(alpha), 1)` before interpolation and fragment quantization.

Coverage scales the existing acceptance probability by `1 - alpha`. Separate
draws multiply their survival probabilities; triangles within one draw form a
union, avoiding double attenuation at shared edges. Alpha zero leaves candidate
placement unchanged, alpha one removes the root, and soft edges thin the grass.
The existing random sequence is retained. Blades can still lean or spread across
an edge from an uncovered root.

Texture sampling uses immutable **native base-level pixels**, the native UV
transform/wrap and nearest/bilinear filtering. It deliberately remains independent
of camera-dependent mip selection. Replacement-pack alpha and screen-space
occlusion/scissor coverage are not reconstructed. These limits should be checked
if a future overlay depends on them; native draw appearance is never modified.

## Lifetime and cost

Geometry and primary alpha are cached in `grass_geometry_registry`. Overlay-set
versions include source and overlay geometry/content, world transforms and alpha
material/texture state. Native order controls set membership. The version feeds
the existing async/static and world-placement keys, including removal, so stale
placements cannot survive a changed overlay set. An unchanged set shares its
prepared triangles and does not rerun extraction. No overlay returns the original
source version and uses the existing extractor path without per-root mask work.

Actors are excluded up front. Changes to a room overlay's UV transform, vertex
positions or rigid world transform mark that instance animated and remove its
mask until room removal/reset. Small reconstruction roundoff is tolerated for
the world transform. The first observed pose can mask until motion is detected.
Animation history is bounded to 4,096 instances; prepared sets to 256 sources.
Material/texture-alpha changes invalidate and rebuild static placement.

Grass Off bypasses capture and masking. The feature adds **zero GPU passes,
draws, texture uploads or readbacks**. The GPU receives the existing grass data
with fewer roots. Exact GPU savings and total live CPU frame cost require a lab
run; CPU capture still has a cost even when the resulting overlays do not
intersect a source. No claim of zero total-frame overhead is made.

### Offline Graveyard measurement

The private fixture was decoded from `scene/spot02_1_info.zsi`; no ROM-derived
payload is checked in. The optional test `Oot3dGrassOverlayBenchmark.OfflineRoomFixture`
reads the path in `TRIAEVUM_GRASS_MASK_FIXTURE`. It uses the actual room triangles,
UVs, native texture channels and trace order/blend categories. It uses the user's
density (1048.8/m²), spacing (0.6), first rule's green levels and an eight-million
candidate cap. Primary alpha is assumed one and alpha-test reference 128; this
is a CPU workload measurement, **not a replay of captured shader uniforms or the
runtime's inherited rule selection**.

Five extraction repetitions, median, local Release build:

| Measurement | Result |
| --- | ---: |
| Source triangles | 991 |
| Broad-phase overlay triangles | 2,541 |
| Source triangles with coplanar candidates | 241 |
| Source/overlay triangle candidate pairs | 1,282 |
| Roots before / after | 542,260 / 490,943 |
| Cold overlay preparation | 0.548 ms |
| Extraction before / after | 70.032 / 67.409 ms |
| Warm set lookup | 0.544 µs |
| Republish 19 prepared meshes + lookup | 3.148 µs |
| Empty-overlay lookup | 0.018 µs |

Reduced spacing/output work offsets mask evaluation in this fixture. The warm
republish measurement excludes native uniform decoding and geometry-registry
lookup; use the runtime capture diagnostic for that cost. These numbers do not
establish a frame-rate improvement or a no-overlay frame-time guarantee.

## Graveyard identification and lab checklist

Offline texture inspection and triangle counts identify these room draws in
`round35_graveyard_trace.jsonl` (frame 2983):

| Trace ordinal | Native texture ID | Room texture | Triangles |
| --- | --- | --- | ---: |
| 9 | `c09d75caf61cc19c` | `s2_yuka_01`, grass ground | 991 |
| 18 | `c018f0023fa65419` | `s2_tsuchi_02`, dirt overlay | 10 |
| 19 | `78795509fbe0ffeb` | `s2_tsuchi_01`, dirt/stone-path atlas | 205 |

Draw 20 (`dd5aab0feaa345b3`) was not identified in this room CMB. Runtime capture
can determine its eligibility; it is not hardcoded. None of these asset names,
IDs or scene IDs are used by the feature.

Enable `TRIAEVUM_GRASS_MASK_DIAGNOSTICS=1` for the lab. A `pair` line gives native
ordinals, submissions, geometry identities, source native/observed texture hashes,
**configured rule ID and original configured Rgba8Hash**, and the mask's native
and observed hashes. This also resolves sources admitted through texture twins.
`prepare` lines give cold triangle count/version/time; `frame` lines report the
previous frame's capture attempts, captures and CPU time. `excluded` lines explain
unsupported alpha/geometry/UV paths or detected animation. Pair counts are broad
phase; the extractor still applies the plane and barycentric tests.

1. In Claude's lab, load scene 0x53 room 1, child Link at night, from
   `state-2026-10-11_16-21-37.oot3dsav`, with the supplied Custom grass settings.
2. Confirm the diagnostic pairs source draw 9 with path draw 19 and records the
   actual configured rule ID/hash. Check for exclusions on these two draws.
3. Capture grass On/Off at the same camera. Slab interiors should have no roots;
   alpha-zero gaps and the grass beside the atlas should remain populated; soft
   edges should thin. Confirm the native pavement appearance is identical.
4. Move/rotate the camera and cross the path. Check slopes, intersections and
   room re-entry for stale masks or rebuilding. With `OOT3D_GRASS_DIAGNOSTICS=1`,
   `[grass-build]` should settle after static placement is cached.
5. Change/remove a source rule and toggle grass Off/On. Check that the overlay
   never becomes a source and Off has no mask-capture work.
6. Compare warm capture CPU and grass CPU/GPU timings against baseline, including
   a scene without ground overlays. Collect medians/tails with diagnostics off
   for final frame-time comparisons. Do not use the offline numbers as frame times.

## Validation

Built `TriAevum.exe` and `oot3d_graphics_foundation_tests`. The grass-related suite
plus overlay fixture passed 107 tests; its optional private benchmark was skipped
in the generic run and passed when explicitly supplied the local fixture.
Coverage includes opaque/alpha-edged masks, native TEV/primary/constant alpha,
alpha testing, bilinear/wrapped UVs, slopes, walls, plane tolerance, ordering,
scope/sequence isolation, animation exclusion, geometry alpha capture, and
static/world-cache invalidation/restoration. Standalone `grass_spacing_tests`
(36 brute-force reference cases) and `texture_source_cache_tests` also passed.
No game execution or visual acceptance was performed by Codex.
