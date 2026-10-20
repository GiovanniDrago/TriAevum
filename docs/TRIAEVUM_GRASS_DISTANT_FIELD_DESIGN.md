# Grass: Distant Field

Date: 2026-10-20. Status: implemented, product default (start 7 m). Base:
`release/0.8.0-beta.1.2` after visible-budget thinning (33c9e31).

## Goal

Collapse the total grass cost while increasing the perceived cover at
distance: no thinning with distance, no bare hills, no budget-dependent gaps.
Near grass (blades, interaction, shadows, outlines) is unchanged.

## What it replaced

With the product preset every root up to the 500 m draw distance went through
the whole per-root pipeline (resident anchor, selection, compaction, a 4-vertex
tuft with the full blade vertex shader and a 4-silhouette cutout loop). Beyond
the LOD reference (22.8 m) retention fell as 0.5 / n^2, which is the thinning
visible on hills, and distant tufts were 2-3 pixels tall. Kokiri's ledge drew
146-203 thousand roots, Hyrule Field 160-270 thousand.

## Mechanism

Beyond `DistantFieldStartDistance` (T0, default 700 units = 7 m) the blades
stop, and every accepted surface triangle is drawn once as a thin grass slab:

- **Blades** keep their whole pipeline up to T0. Their draw distance becomes
  T0 and their draw fade covers the blend band [T0 (1 - b), T0], b =
  `DistantFieldBlendFraction` (0.25). Selection, residency and the visible
  budget therefore only see the near disc. Until every active placement has its
  field, the blades keep the full draw distance (no hole while a field builds).
- **Field data** (`grass_distant_field.{h,cpp}`) is derived from the
  placement's accepted roots, never from the mask directly, so it inherits
  every placement decision (mask levels, active rectangles, room overlays,
  slope, spacing, candidate capacity) and adds no surface: a triangle without
  roots has no slab. Per triangle a barycentric grid (25-unit edge, at most 128
  per side) holds coverage, mean surface colour and mean blade height, each a
  cone-kernel estimate (radius 37.5) over the roots around the grid vertex in
  world space, whatever triangle they grew on, so values are continuous across
  triangle edges. Coverage is normalised by the candidate density times the
  candidate-capacity scale times the share surviving the spacing test. Built
  on a background thread per placement version (Kokiri 1 M roots 150 ms,
  Hyrule Field 4.2 M roots 740 ms).
- **Slab** (`BuildGrassFieldVertexShader/FragmentShader`): one unindexed
  triangle per field triangle, lifted along the smoothed vertex normal by the
  tallest blade height. The vertex stage lights each corner with the native
  vertex's own surface-lighting texel and the coarse shadow grid (the corner's
  nearest root's reference). The fragment stage walks the view ray down
  through the slab and stops at the first world-anchored column it meets:
  a column stands in a lattice cell with probability equal to the local
  coverage, with a random height scaled by the local blade height and a
  tapered cross-section; cells are one blade spacing near T0 and coarser
  octaves once a column would be under 1.5 pixels (stable coarse-cell dither
  between octaves). A grazing ray takes up to four samples per height layer,
  so cover saturates as it does with real blades, while seen from above bare
  ground between the columns stays visible. Colour, toon response, lit
  ceiling, native fog and auxiliary guides use the blades' own functions,
  extracted from the blade shader text so they cannot drift. Depth is the
  hit's (declared `depth_greater` where the PICA depth is the raster depth),
  motion is the hit's rigid camera motion.
- **Draw order**: the field draws after the blades in the same grass provider
  pass (`DrawPreparedField`), with its own GPU scope `grass_field`.

## Changes from the design

- Coverage from the roots instead of the rule's mask LUT: the mask alone
  missed room overlays (stone paths in the Graveyard) and spacing.
- No coverage amplification. A first version boosted coverage to match the
  old tufts; it filled Hyrule Field's paths and dirt patches. The real cause
  was undersampling at grazing angles, fixed by the extra crossings.
- Colour weights the root-to-tip mix by the cube of the hit height: the
  first column a ray meets is hit near its top far more often than a blade's
  visible surface is, which made the field 10-20 levels brighter than tufts.
- The GPU selection's budget cut (added by 33c9e31) walked 1,024 histogram
  buckets in one thread, twice per frame (~0.25 ms). It is now a 256-lane scan.
- The blend band is measured where a column stands, not where the ray enters
  the slab top (fix after the first release of the field). A third-person
  camera above the field looking along it enters the slab far nearer than it
  lands: with the fade evaluated at the entry, rays landing beyond T0 carried
  the band's reduced density (or were discarded under T0 (1 - b)), so the
  columns beyond T0 were only reached through the tops of nearer slabs. The
  field was hollow under its top layer: a bright carpet of column tops floating
  one slab height above bare ground, doubling the fading blades below it
  ("grass repeated and shifted into the air", Kokiri Forest from the user's
  save state, worse with `DistantFieldHeightScale` 2, gone at 0.25). Every
  sample lies on the entry ray at entry distance + descent, so the fragment now
  evaluates the fade per sample at that distance, discards only when even the
  ground sample (entry + slab / minimum slope) is under the blend start, and
  the vertex stage keeps every slab whose longest descent can still reach the
  band.
- The march leaves its entry triangle with the entry's coverage, so a slab's
  columns stood up to slab / minimum slope (180 units) past the grass over
  neighbouring surfaces without roots (Kokiri's stream beyond the signpost was
  covered). Clipping the march to the prism is not an option: the prisms have
  no side faces, so the columns of the neighbouring grass triangle a ray
  crosses into are only reachable through this march. Instead each field
  carries a world cell table (`GrassFieldCellEntry`, binding 12): an
  open-addressing hash over the 25-unit cells that contain roots, each with
  the same cone-kernel estimate evaluated at its roots' mean position, so it is
  continuous with the triangle grids. A sample over its entry triangle (XZ
  footprint) keeps the interpolated grid coverage; a sample that has crossed
  out reads the cell holding it or the cell below (a sample stands up to a slab
  height above its surface cell), and 0 where no cell holds roots. Two probes
  per outside sample. A placement wider than the key (2048 x 1024 x 2048 cells,
  512 x 256 x 512 m) gets no table and keeps the entry coverage.

## Settings

`Graphics.Grass.Performance`: `DistantFieldEnabled` (default true),
`DistantFieldStartDistance` (700, 200-50,000), `DistantFieldBlendFraction`
(0.25, 0.02-0.9), `DistantFieldHeightScale` (1, 0.25-2), `DistantFieldSteps`
(6, 2-16). Optional keys: profiles without them get the defaults. F1 > Grass >
Performance > Distant field owns them; Off restores the previous behaviour
exactly (blade draw distance, residency, no field pass).

Diagnostics: `TRIAEVUM_GRASS_FIELD_DEBUG=1` colours the slab top by coverage /
height / blend ramp, `2` grey coverage, `3` grey height, `4` march hit (red)
or miss (blue). `[grass-field] built` lines (with `OOT3D_GRASS_DIAGNOSTICS`)
report triangles, grid size, roots, coverage statistics and build time.
`TRIAEVUM_GPU_TIMELINE=1` now splits grass compute into static upload,
selection, compaction setup and dispatch.

## Verification (dev launch, RTX 3060, 1280x720, product profile)

Free-camera lab (`grass_cam.py`, 8 yaws), Kokiri ledge at 2.2 m orbit and
Hyrule Field wide orbit (7 m out, 4 m up). Region colour means against the
field-off captures stay within 0-11 levels per channel (most within 3) with the
same contrast; Hyrule Field paths keep their width; partly covered dirt
patches read greener (their root density is real, the old thinning showed the
ground through it).

Paired runs on the same build, ABBA per scene, median grass GPU time over the
six steady yaws (1,074 frames each run):

| Scene | Field | Grass GPU ms | Selection+compaction | Blades | Field | Visible roots | Grass VRAM MiB |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Kokiri ledge | off | 2.06 / 2.15 | 0.48 / 0.49 | 1.49 / 1.56 | - | 203,309 | 90 |
| Kokiri ledge | on | 1.29 / 1.32 | 0.31 / 0.32 | 0.80 / 0.84 | 0.12 | 19,999 | 90 |
| Hyrule Field wide | off | 1.68 / 1.64 | 0.50 / 0.50 | 1.02 / 1.02 | - | 260,152 | 99 |
| Hyrule Field wide | on | 0.86 / 0.87 | 0.22 / 0.22 | 0.16 / 0.17 | 0.32 | 5,197 | 48 |

Grass GPU time falls 38% on the near-heavy Kokiri ledge and 48% on Hyrule
Field's wide views; visible roots fall by 90-98%. The remaining cost is the
near disc's blades (fragment-bound) and a fixed ~0.2 ms of selection passes.
Both "off" rows already include the parallel budget cut.

Tests: `oot3d_grass_distant_field_tests.cpp` (grid indexing, full / half /
bare lattices, candidate-capacity and spacing normalisation, shader
composition, settings persistence), foundation suite 428/428, SPIR-V pack 97
artefacts, `grass_motion_gpu_tests` (GPU cut equals the CPU rule), product
defaults test, F1 actual-widget smoke (11,061 assertions). Not yet run:
release-build / Forge package, Linux AMD, 4K, the 30 Hz intro ABBA.

## Not changed

Native PICA state, draw order and targets; placement masks and root
acceptance; near-band blades, interaction, actor shadows and outlines; UI and
presentation domains; the placement disk cache format.
