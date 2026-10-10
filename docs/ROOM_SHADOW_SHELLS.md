# Interior shells pass light

`shadowCasterFaces` accepts `native`, `both`, and `interiorShells`. The base
environment uses `interiorShells`; existing `shadowCasterCulling` values keep
their meaning (`true` = native, `false` = both). The new field takes precedence.
F1 exposes these choices under Lighting > Lights & Shadows.

The OOT3D adapter snapshots the installed room command lists and bounded CMB
payloads on the guest thread. One lazy worker parses rigid bind geometry,
computes shell masks, and writes a local cache. Alpha-tested/blended geometry
stays native. Unsupported skinned or non-triangle opaque geometry makes that
room ineligible for replacement. No worker or room reads start while this
mode or directional shadows are disabled.

Installed room mesh headers differ from raw ZSI files: the title relocates
`mesh+4` (the entry-table pointer) to an absolute guest address. Command
arguments and CMB offsets in that table remain relative to the command-list
base. The adapter explicitly distinguishes installed memory from raw files
and validates the relocated table against the same bounded room span.

Draws do not identify individual rooms, so replacement is atomic across all
loaded rooms. Pending, malformed, unsupported, or failed GPU uploads retain
the previous two-sided native caster path. Immutable room snapshots travel
with the scene's light binding. Contact shadows still use native geometry.
Actors and alpha-tested draws remain native two-sided directional casters.

Opaque CMB draws commonly keep the PICA blend unit enabled with ADD and
ONE/ZERO for both RGB and alpha. This is opaque replacement and qualifies for
room shells; the enable bit alone does not imply translucency. Replacement
requires scene-domain opaque CMB ownership, without actor/projected-shadow or
isolated-lighting ownership. Unknown, UI, atmosphere, transparent-pass,
alpha-tested, and genuinely blended draws keep their native geometry.

The shared builder welds exact world positions. For sector `s` in `[0,63]`,
its Y-up Fibonacci direction is:

```
y = (s + 0.5) / 64
a = s * pi * (3 - sqrt(5))
d = (sqrt(1-y*y)*cos(a), y, sqrt(1-y*y)*sin(a))
```

Each direction projects the whole room into a square 512-by-512 raster with
a one-texel border. The greatest `dot(position,d)` owns each covered pixel
centre. A natively back-culled triangle is a shell only if its world-space
winding normal has `dot(normal,d) < -0.25` and it owns at least half of its
covered pixels **and the space on its front side is enclosed**. Uncovered
triangles keep casting. Layers are never peeled again after the first surface;
inner roofs at every height remain occluders.

Enclosure is computed once per triangle, independently of the 64 light
directions, and ANDed into their masks. Its constants are part of criterion
version 2:

- Cast **32** deterministic, uniform solid-angle Fibonacci rays over the
  hemisphere around the triangle's winding/front normal. Room shells face
  inward, so this is the space light enters after passing the shell.
- Start at the centroid plus **0.01 world units** along that normal, exclude
  the source triangle, and accept intersections at distances greater than
  `1e-6` and no farther than **4000 world units**.
- Require **at least 24 hits out of 32 (0.75)**. Stop counting early only when
  the final pass/fail result is already determined.
- Intersect the room's opaque, non-alpha-tested triangles with either winding,
  including double-sided triangles, through a deterministic median-split BVH
  with at most eight triangles per leaf. Rays and triangle intersections use
  double precision; there is no dependency on runtime visibility or actors.

For sample `i`, local hemisphere height is `(i+0.5)/32` and azimuth is
`i*pi*(3-sqrt(5))`. A tangent basis around the front normal rotates these
directions into world space. This keeps a lone wall over a ground plane
casting from behind: the open sky prevents 75% enclosure. Closed room walls
and ceilings qualify; a broad opening can disqualify the wall facing it.
A small doorway can still leave enough of the hemisphere enclosed to qualify.

The renderer selects the maximum-dot sector for each shadow keyframe and
degenerates masked triangles in a cull-none vertex shader. The mesh checksum,
sector, and raster bias enter its static-page content identity.

## Cache format/criterion, version 2

Location: `${TRIAEVUM_RENDERER_CACHE_DIR}/shadow-shells/<16-digit-lowercase-fingerprint>.bin`.
An unset/empty directory disables persistence, but still allows an in-memory
build. Cache contents are ROM-derived and local. Only installed room entries
and one active worker build are retained; stale queued builds are discarded.

All fields are little-endian. The 32-byte header is `u32 magic` (`RSSH`),
`u32 version`, `u64 room fingerprint`, `u32 sector count`, `u32 raster size`,
`u32 vertex count`, `u32 triangle count`. It is followed by float32 XYZ welded
positions, three u32 indices per triangle, a u64 mask per triangle, and a u64
checksum of all preceding bytes. Both checksum and room fingerprint use the
renderer FNV-1a basis **1469598103934665603** (intentionally nonstandard) and
prime 1099511628211. The room fingerprint covers file bytes `[0x10,0x90)`.

Reads validate version, key, exact byte length, checksum, finite coordinates,
counts, and indices. Runtime disk loads require the production raster size
512; smaller test rasters cannot be reused by the runtime. Bad/missing files
are rebuilt; writes use the renderer's atomic replacement helper.
Version 2 keeps the byte layout but adds the enclosure criterion. Version 1
files are rejected even with a valid checksum and rebuilt at the same path.

## Validation

Owning-boundary targets: `renderer3ds_room_shadow_shell_tests`,
`oot3d_native_room_shadow_shell_tests`, `oot3d_pica_scene_frame_tests`,
`triaevum_environment_tests`, `triaevum_f1_settings_smoke --scene-environment`.
The adapter test optionally accepts an extracted ROM scene directory for
read-only CMB validation. GPU and in-game visual checks are separate; the
implementation work does not launch the game. A Forge prebuild mode is deferred.
Builder tests include closed ceilings/walls, a lone wall on a ground plane,
a box with an open side, the 4000-unit range, two-sided enclosure occluders,
non-occluding alpha/transparent geometry, first-layer/majority behavior, and
rejection of checksummed version 1 caches.

`oot3d_native_room_shadow_shell_tests --benchmark-room <room_info.zsi>` reads
a raw RomFS room and performs five fresh production-resolution builds,
reporting minimum/median/maximum milliseconds and checking deterministic output.
Timing includes welding, BVH construction, enclosure rays, all sector masks,
and cache encoding; it excludes file I/O, CMB parsing, and cache hits.

Local optimized host build, 2026-10-10, five consecutive builds per room
(room benchmarks run sequentially):

| RomFS room | Triangles | Builder ms, median (min–max) |
| --- | ---: | ---: |
| Deku Tree, `ydan_0_info.zsi` (`3e6fb0db85447539`) | 1,596 | 211.0 (204.7–213.7) |
| Zora's Domain, `spot07_1_info.zsi` (`bc5756509000714f`) | 3,887 | 321.6 (319.1–322.5) |

All five outputs per room had identical content hashes. Geometry and shell
selection in the runtime renderer are unchanged; only cached mask construction
and acceptance of the new criterion version changed.

`TRIAEVUM_ROOM_SHADOW_SHELL` stderr events cover enable changes, room requests,
capture errors, cache hits/misses, build completion (triangle count and
milliseconds), and cache-write success/failure with the filesystem error.
The shadow pass reports native-caster replacement counts, shell-triangle
counts, or its fallback reason at most once every two seconds. `native_casters`
counts actual replacements; `native_candidates` and `native_drawable` count
casters before and after native pipeline admission. `room_candidates` counts
replaceable architecture. Each drawable caster belongs to one class: room,
unknown domain, other domain, actor, other pass, alpha test, or translucent
blend. The `reject_*` fields count the latter six; separate alpha-program and
pipeline rejection counts cover admission failures. `snapshots`,
`ready_snapshots`, and `missing_snapshots` count publication on all drawable
casters, independently of classification. `rooms` and `scope` describe the
first published snapshot, even when no room caster qualifies. They no longer
report a false publication failure when geometry is rejected. Missing,
pending, or mixed architecture snapshots still prevent replacement.

Publication regression tests cover gameplay capture through CMB command spans
and frontend packets, draw-plan shader-cache reuse and interpolation
(`oot3d_native_pica_vulkan_plan_tests`), presentation preparation/submission
(`oot3d_native_pica_vulkan_bridge_tests`), and scene-frame selection. The shared
shell tests exercise native ONE/ZERO replacement and every rejection class.

Developer-lab
graphics acknowledgments also include decode/apply issue details and the
effective lighting/shadow settings; an accepted correction is not a rejected
command. The optional adapter test `--guest-room <file>` accepts a u32 guest
base followed by an installed room memory span, for offline savestate checks.
