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
covered pixels. Uncovered triangles keep casting. Layers are never peeled
again after the first surface; inner roofs at every height remain occluders.
The renderer selects the maximum-dot sector for each shadow keyframe and
degenerates masked triangles in a cull-none vertex shader. The mesh checksum,
sector, and raster bias enter its static-page content identity.

## Cache format, version 1

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

## Validation

Owning-boundary targets: `renderer3ds_room_shadow_shell_tests`,
`oot3d_native_room_shadow_shell_tests`, `oot3d_pica_scene_frame_tests`,
`triaevum_environment_tests`, `triaevum_f1_settings_smoke --scene-environment`.
The adapter test optionally accepts an extracted ROM scene directory for
read-only CMB validation. GPU and in-game visual checks are separate; the
implementation work does not launch the game. A Forge prebuild mode is deferred.

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
