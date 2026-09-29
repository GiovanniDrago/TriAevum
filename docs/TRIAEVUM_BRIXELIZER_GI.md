# FidelityFX Brixelizer GI: design

Status: phases 1-5 done (diffuse GI, history and resets, F1 live qualification, specular GI as a reflection source). Owner surface: F1 **Lighting**.

## Goal

Add dynamic diffuse (and later specular) global illumination to the NRI
renderer with AMD FidelityFX Brixelizer (sparse distance fields built from
scene triangles) and Brixelizer GI (screen probes, a world-space radiance and
SH irradiance cache, temporal denoising). It must follow
`OOT3D_NRI_FIDELITY_EXTENSION_ARCHITECTURE.md`: optional, declared in the
compiled effect graph, inert when disabled, and never part of the canonical
PICA path.

## Source and license

FidelityFX SDK v1.1.4, already pinned for SSSR
(`cmake/dependencies/oot3d-sssr.cmake`): `sdk/src/components/brixelizer`
(Brixelizer 1.0) and `sdk/src/components/brixelizergi` (Brixelizer GI 1.0.1),
with GLSL shaders for the Vulkan backend under
`sdk/src/backends/vk/shaders/brixelizer{,gi}`. MIT; the donor notices travel
with the generated shader headers exactly as for SSSR.

## Scope and product surface

- **Modes.** Per-pixel and Per-pixel + PBR only, like directional shadows.
  Native lighting, Authentic, F2 and native-memory rendering never run it; the
  stored preference survives them and their effective snapshots disable it.
- **F1 Lighting > Global illumination:** Off (default) / Brixelizer GI;
  quality (internal resolution 1/2 or 1/4, cascade count); diffuse intensity;
  specular on/off (phase 5). Off remains selectable when capabilities are
  missing; the reason is shown. One owner: the graphics settings runtime.
- **Off is inert.** No context, no geometry export, no guide MRT, no graph pass,
  no allocation; framebuffers byte-identical to the current build.

## Inputs (Brixelizer GI dispatch)

| GI input | Source in this renderer |
|---|---|
| depth, history depth | native depth; previous frame retained by the graph (history read, as VSM) |
| normal, history normal | normal guide through `NormalSpacePass` to world space; `normalsUnpackMul/Add` 2/-1 |
| roughness | material guide G, perceptual (`isRoughnessPerceptual = true`, channel 1) |
| motion vectors | `MotionVectors` (graph resource, same producer as TAA/SSSR) |
| prevLitOutput | previous linear scene color (`linear_scene_color`), before UI |
| environment map | the IBL environment already prepared for SSSR, or black with intensity 0 |
| noise texture | fixed blue-noise texture generated at build time (no media download) |
| view/projection, previous | `NativeSceneView` captured camera; canonical negative-Z projection |
| SDF atlas, brick AABBs, cascade trees/maps | Brixelizer context outputs |

The depth convention (W-buffer, inverted or not) is resolved explicitly per
frame from the captured projection, as GTAO and SSSR already do; it selects
the `FFX_BRIXELIZER_GI_OPTION_DEPTH_INVERTED` permutation.

## Scene geometry for Brixelizer

Brixelizer voxelizes triangles from GPU vertex/index buffers
(RGBA16F/RGBA32F positions with a stride, 16/32-bit indices, per-instance
AABB and transform; static instances persist, dynamic instances are
resubmitted every update).

PICA vertex data is not world-space: positions are produced by the draw's
vertex program. The renderer already reproduces them for shadows: virtual
shadow maps replay native draws with the captured view-to-world transform and
the typed transform/skeleton hook layout (`PicaVertexShaderHookLayout`,
`BuildPicaShadowCaster`). GI geometry uses the same contract:

1. **Admission.** Identical to shadow casters: opaque, depth-writing,
   non-alpha-tested native *scene-domain* geometry with a typed transform
   hook. UI, frontend, transparent and unknown programs are excluded (no
   guessing, no per-scene lists).
2. **Position export.** A world-position export variant of the native vertex
   program (typed hook, separate cache key, never the canonical shader) runs
   once per admitted draw version and writes RGBA32F world positions into a
   persistent GI geometry arena; indices are copied once per geometry
   version. It changes no native attachment.
3. **Instances.** Rigid geometry (stable geometry identity, content version
   and quantized pose, the VSM reuse rule) becomes a Brixelizer *static*
   instance and persists across frames; its removal or change deletes it.
   Skinned/animated or unproven content becomes a *dynamic* instance,
   resubmitted each update under a per-frame triangle budget.
4. **Cascades.** Camera-centred cascades scrolled by Brixelizer; world units
   follow the VSM clipmap scale (native units).

The Brixelizer update runs once per *native* frame (a new guest frame), not
per interpolated sample: samples reuse the SDF, since geometry positions are
the native frame's. GI dispatch runs per presented sample (its own history).

## Composition

Diffuse GI is irradiance; it needs diffuse albedo, which the canonical guides
do not carry (the ambient guide's RGB is the ambient response consumed by AO).
A new auxiliary output **DiffuseAlbedoGuide** (RGBA8, location 7, declared only
when GI is active) carries it. Locations 5 and 6 already hold the fog and
outline-geometry guides, so it is the eighth colour attachment (the common
`maxColorAttachments`); the instrumented ABI keeps seven attachments whenever
GI is off, so existing pipelines, manifests and prewarm are unchanged. Writers:
per-pixel lit draws re-evaluate the native TEV region with unit lighting
(primary 1, secondary 0) and store the result, i.e. texture times material
colour without the vertex-baked lighting; PBR draws store `pbr_albedo`
(returned to sRGB); other opaque, RGB-replacing scene draws store zero so a
surface in front removes GI; translucent draws leave it. Alpha is validity. The composer (the same fused scene composite as AO and
reflections, before UI) adds

`color += coverage * albedo * diffuseGI * intensity * aoVisibility`

where `coverage` is the existing scene-coverage weight (clear/HUD zero,
fractional under translucent overlays). Native draw order, depth, blending and
UI composition are unchanged. Specular GI (phase 5) composes like SSSR through
the material guide (F0, roughness, reflection gain), filling SSSR misses only.

## Scheduling (effect graph)

- `BrixelizerUpdate` (compute, after the native opaque prefix of a native
  frame; reads the GI geometry arena; writes the SDF resources, persistent).
- `BrixelizerGi` (compute, after motion and normal-space; reads depth,
  normals, material, motion, previous lit colour, SDF; writes DiffuseGI,
  SpecularGI; owns its internal histories).
- Composite reads DiffuseGI and DiffuseAlbedoGuide. All transitions are
  graph-declared; the FFX backend's own barriers stay inside its dispatch, as
  with SSSR.

## Phases

1. **Build.** Compile Brixelizer and Brixelizer GI host components and their
   Vulkan shader permutations with the pinned `FidelityFX_SC`; add both effects
   to the shared FFX Vulkan backend. Windows only at first (Linux needs the
   shader bundle to carry the new headers). Acceptance: libraries link; a GPU
   probe creates and destroys both contexts on the device.
2. **Geometry.** Admission, position-export variant, geometry arena, static
   and dynamic instances, Brixelizer update pass, debug visualization
   (Brixelizer's SDF view) under a developer switch. Acceptance: SDF debug view
   matches the scene; zero validation errors; Off byte-identical.
3. **Diffuse GI.** Dispatch, DiffuseAlbedoGuide, composition, F1 controls.
   Acceptance: deterministic captures (On differs only in scene domain, UI
   pixels unchanged), Off byte-identical, validation clean, GPU cost measured.
4. **Interpolation and history.** Correct behavior with Interpolated2x samples
   (SDF per native frame, GI per sample), resize, settings changes,
   savestate/load, scene transitions (history and instances reset).
5. **Specular GI.** Composition with SSSR.

## Phase 1 result (2026-09-29)

- `THREE_DS_RECOMP_ENABLE_BRIXELIZER_GI` (Windows, requires SSSR, default
  ON) compiles 31 Brixelizer and 19 Brixelizer GI Vulkan shaders (four
  variants each; GI with its depth-inverted, disable-specular and
  disable-denoiser permutations), the host components
  `oot3d_ffx_brixelizer` / `oot3d_ffx_brixelizergi`, and adds both effects to
  the shared FidelityFX Vulkan backend. The runtime links them and defines
  `ENABLE_OOT3D_BRIXELIZER_GI`; nothing calls them yet.
- `oot3d_brixelizer_gi_context_probe` creates and destroys a four-cascade
  Brixelizer context and a 50%-resolution GI context through one backend
  interface on the RTX 3060.
- Under the Khronos validation layer the probe exposed a donor defect: the
  per-effect bindless descriptor pool lacks `FREE_DESCRIPTOR_SET_BIT` although
  its sets are freed individually, and its `maxSets` counts pool sizes (3)
  while four sets are allocated. The checked, idempotent
  `patches/oot3d_fidelityfx_vk_bindless_pool.cmake` corrects both; the probe is
  then validation-clean. Only bindless effects (Brixelizer) use that pool.
- The runtime with the rebuilt backend is identical to the Interpolated2x
  baseline (Field, Sheik copy).

## Phase 2 result (2026-09-29)

- `renderer3ds/pica_gi_geometry` (shared 3DS layer) turns one native draw into
  a triangle-list mesh with the shadow-caster contract (`VertexGeometry::Pose`):
  rigid draws keep a model-space mesh shared per geometry version plus the
  draw's model-to-world transform and a quantized placement signature; skinned
  or non-homogeneous draws carry world positions. Strips become lists,
  degenerate joints are dropped, unknown programs yield nothing.
  `pica_gi_geometry_tests` covers these cases.
- `oot3d/brixelizer_scene` owns the context, the 512^3 R8 atlas, brick and
  cascade buffers, host-visible vertex/index arenas registered once, and the
  instance lifecycle: a rigid placement seen in two consecutive native frames
  becomes static; others are dynamic each update (triangle budget); static
  placements unseen for 600 native frames are evicted; `ResetPicaState`
  clears the field. The ~8 MB baked update description lives on the heap
  (it overflowed the render thread's stack).
- The PICA extension graph declares `SceneDistanceField` (Observer, AfterOpaque,
  reads PicaSceneFrame and NativeSceneView). Geometry is collected at the draw
  record site from the first non-synthetic execution of each native frame; the
  update runs once per frame before the display effects.
- Developer switch until the F1 control (phase 3): `OOT3D_GRAPHICS_BRIXELIZER=1`
  with Per-pixel or Per-pixel + PBR lighting (replaced in phase 3 by the
  setting below). Diagnostics:
  `TRIAEVUM_SCENE_DISTANCE_FIELD` lines on stderr every 150 updates.
- The renderer's Vulkan 1.2 profile now also enables descriptorBindingPartiallyBound,
  runtimeDescriptorArray and non-uniform indexing of storage/sampled/storage-image
  arrays (required by Brixelizer's shaders); arena and scratch buffers carry
  transfer usage for FidelityFX's readback copies. Under the Khronos layer the
  Field run then reports zero validation errors.
- Field (Interpolated2x, Per-pixel): 64 admitted draws per native frame; 68 static
  and 22 dynamic instances after 150 updates, 81 static after 300; arenas 205/154
  KiB; free bricks 262,144 -> 197,770. GPU scratch is 268 MiB with the initial
  budgets (to reduce). With the switch off, Field and Sheik copy stay identical
  to the Interpolated2x baseline.
- Debug view: `OOT3D_GRAPHICS_BRIXELIZER_DEBUG=<directory>` renders
  Brixelizer's ray-marched debug visualization (mode from
  `OOT3D_GRAPHICS_BRIXELIZER_DEBUG_MODE`, default 3 = normals) with the scene
  camera every 150 updates into a private RGBA16F image and writes it as
  `sdf-u<update>-m<mode>.ppm` once the GPU has finished (read back after the
  frame slots retire). It uses the FidelityFX clip matrices of the other
  FidelityFX passes, so the image is in the storage orientation of the scene
  guides (rotate by 90 degrees to compare with a capture). Ray marching must
  use the *merged* cascades: static, dynamic and merged cascades are indexed
  [0, n), [n, 2n) and [2n, 3n); the static range alone shows only part of the
  field. In Field the merged view matches the capture (gate, walls, benches,
  steps, Link with shield and legs).
- GPU cost (timestamp queries around the update, RTX 3060, no validation
  layer): Field about 0.69 ms per update in steady state (windowed means of
  updates 151-450), one update per native frame; the first full bake costs
  35 ms once. Sheik copy about 2.9 ms per update (dense fluted interior).
  The update is not yet split or skipped when nothing changes.
- Budgets sized from measured peaks: 2M references (peaks 77k Field, 192k
  Sheik copy), 4 MiB triangle swap (peaks 145 KB / 371 KB), 16k bricks per
  bake. Bricks per bake is a throttle: a voxel whose brick cannot be
  allocated stays uninitialized and is baked by a later update of the
  cascade, so the Sheik copy's 35k-brick first bake spreads over updates.
  `overflows` in the diagnostic line counts updates over the reference or
  swap budget (zero in both scenes). GPU scratch 268 -> 93 MiB.
- Diagnostics line: `TRIAEVUM_SCENE_DISTANCE_FIELD ... peak_references
  peak_triangles (swap bytes) peak_bricks overflows gpu_ms_mean gpu_ms_max`.
- Qualification: Field under the Khronos validation layer with debug view and
  timestamps, zero errors; with the switch off, Field and Sheik copy identical
  to the Interpolated2x baseline (contract and captures).

## Phase 3 progress

- Settings (graphics settings runtime is the single owner):
  `Graphics.Effects.GlobalIllumination {Mode Off|BrixelizerGI, Resolution
  Half|Quarter, DiffuseIntensity 0..4}`; an older file without the object
  loads Off and is rewritten. Validation turns the mode Off (with the reason)
  without NRI, sampled depth, normal/material guides, motion vectors, view
  metadata or the `BrixelizerGlobalIllumination` capability, which the
  renderer reports from `BrixelizerScene::Supported` (compiled SDK, R8
  storage images, descriptor indexing). The rendering snapshot disables GI
  outside Per-pixel / Per-pixel + PBR lighting and under F2/Authentic; the
  stored preference survives. The scene distance field now follows the
  setting (the environment switch is gone).
- F1 Lighting > Global illumination: mode (capability-safe Off), GI
  resolution, GI diffuse intensity, with the unavailability reason. The
  actual-widget smoke covers selection, persistence, the per-pixel and F2
  rendering gates and capability loss (6,833 assertions).
- DiffuseAlbedoGuide (phase 3b): `PicaColorAttachment::DiffuseAlbedoGuide`
  (index 7) and `PicaAuxiliaryOutput::DiffuseAlbedoGuide`; the extension graph
  requests it (with the normal and material guides) only for GI, and
  `ColorAttachmentCount()` is 8 only then (7 otherwise). The image is grown on
  demand by `PrepareNativePicaTargetAttachments` (not part of creation-time
  target storage), cleared to zero, and bound through dynamic rendering only:
  the legacy render pass keeps seven attachments and the GI capability
  requires dynamic rendering. Grass gains a third pipeline for the extended
  layout (mask 0: it leaves the albedo below it) and selects its pipeline
  from the frame's active bindings rather than the target's allocated
  storage. The instrumented fragment variant (`PICA_DIFFUSE_ALBEDO_GUIDE_V1`)
  writes it for opaque, RGB-replacing, depth-writing scene draws; manifest
  entries carry `writes_diffuse_albedo_guide` (optional on read).
- Verification: unit suites (effect graph 47, foundation 296, pipeline cache
  42, manifest 8) pass; GI off stays identical to the Interpolated2x baseline
  (Field, Sheik copy); GI on (8 attachments, validation layer: zero errors)
  produces captures identical to Per-pixel without GI, since nothing reads the
  guide yet.
- `kLastGraphicsCapability` replaces two loops that ended at a hard-coded
  enumerator (a new capability was silently missing from the permissive
  load-time set).

## Phase 3c/3d result (2026-09-29)

- Graph: `EffectResource::DiffuseIllumination` (transient RGBA16F) and the
  display pass `GlobalIllumination` (after Motion, before Composite; reads
  NativeSceneView, SceneColor, NativeDepth, NormalGuide, MaterialGuide,
  MotionVectors). GI declares Motion and the diffuse albedo and normal guides,
  and forces the dedicated scene composite, so the scanout's fused path is
  unchanged. The composer runs at the native pre-UI boundary like AO and
  reflections (the pre-UI trigger now includes GI).
- Provider: `BrixelizerGiPass`, one instance per composed scene surface (at
  most `BrixelizerScene::kMaxGiSurfaces` = 3, sharing the field's FidelityFX
  backend, which is sized for them). A prepare compute writes, per sample,
  R32F depth, world normals and the sRGB-decoded scene colour into ping-pong
  images, so depth, normal and lit colour have one-frame histories. The GI
  context runs at most once per presented frame and surface into the
  instance's own output; each composition copies it into the graph image (a
  surface composed twice in one frame no longer advances the history twice
  or recycles the backend's per-dispatch views while they are in flight).
  Black environment cube and 16 white-noise layers are generated in code.
  Depth is the native perspective depth: the guest projection is already the
  D3D [0, 1] form (`f/(n-f)`, `w = -z`). W-buffer surfaces are not traced.
- Without GI for a sample (no field data yet, W-buffer, a failed dispatch) the
  declared output is cleared to zero irradiance, so the graph stays complete
  and the composite adds nothing.
- Composite: `color += albedo * irradiance * intensity * aoVisibility` in
  linear light where the albedo guide is valid (alpha). The ambient guide's
  coverage is written only by the AO instrumentation, so GI uses the albedo
  validity; overlay attenuation followed (see "Translucent overlays"). Developer view `OOT3D_GRAPHICS_GI_DEBUG`: 1 irradiance, 2 albedo,
  3 coverage, 4 added light.
- Instrumentation: GI requests the normal guide and the temporal vertex
  instrumentation (object motion). A draw without the typed frontend vertex
  program (frames restored from a visual savestate, which does not store it)
  is now resolved without temporal instrumentation instead of failing the
  presentation; this also fixes TAA in the Sheik copy.
- Measured (RTX 3060, 1600x960 window, 1920x1600 guide target, Half):
  GI dispatch 2.85 ms mean / 3.5 ms p95 in Field, 2.70 / 3.49 in the Sheik
  copy (`global_illumination_ms`); frame GPU time 3.37 -> 6.75 ms with the
  field update. Validation (core) clean in both. GI off stays identical to the
  Interpolated2x baseline. Field daylight: a warm bounce from the grass onto
  walls and Link (+2 levels mean); UI pixels change only where translucent
  HUD elements show the scene.

## Phase 4 result (2026-09-29): history and resets

Brixelizer GI keeps world-space state (screen probes matched by world
position, a radiance cache attached to the field's bricks) besides its
screen histories, so resets follow the world, not the camera:

| Event | Field | GI instances |
|---|---|---|
| Camera cut, projection change, continuity break (resync, interpolation mode) | kept | kept; the context rejects history by world position and normal, the real previous sample stays the history |
| Another scene (`TitleSceneViewSubmission::WorldIdentity`, from the PlayState scene id; rooms share their scene's coordinates) | `Reset` at the next field update | `ResetHistory`: recorded work completed, every instance dropped |
| Savestate load (`ResetPicaState`) | `Reset` | dropped (after the device wait) |
| Resize, internal resolution, MSAA, render-target reconfiguration | kept | dropped with the screen resources |
| GI resolution setting | kept | the surface's instance is replaced |
| GI disabled (field pass leaves the extension graph) | `Reset` | `ResetHistory`, so re-enabling never traces a world seen before |
| Surface not composed for a while | kept | kept up to `kMaxGiSurfaces`, least recently used retired |

Per-sample behavior under Interpolated2x is unchanged: the field updates
once per native frame, GI traces once per presented frame and surface.
stderr `TRIAEVUM_GLOBAL_ILLUMINATION frame= instances_created= retired=
world_resets=` reports lifecycle events.

Measured: structural scenario `link_info_entry_00bb` (Kokiri Forest anchor,
native transition into Link's house, GI Half, 1600x960): one world reset at
the transition (frame 81); the field went from the forest's placements to
the interior's (59 created, 14 resident, none left from the forest, which
would otherwise have stayed until the 600-frame eviction). Interior GI
renders; core validation clean (300 frames). Navi flight (Kokiri, no scene
change): no reset.

Live settings changes (`--graphics-settings-schedule`, which applies a
partial Graphics block at a run frame through the F1 commit call, not
persisted): Field, Interpolated2x, GI Half on, Off at frame 200, on again at
350, Quarter at 500. The disable reset and both instance replacements run
(`instances_created=3 retired=1 world_resets=1`); core validation clean over
650 frames. Every capture while GI is off (220-345) is byte-identical to a
run with GI never enabled, so the pipelines, guides and targets return to
the canonical ones; after re-enabling and after the resolution change the
GI contribution starts from zero and accumulates again (no stale history).

## Phase 5 result (2026-09-29): specular GI

- Settings: `GlobalIllumination.Specular` (off by default) and
  `SpecularIntensity` (0-2), persisted without a schema change (absent fields
  keep the defaults and are rewritten); F1 Lighting > Global illumination:
  "GI specular reflections" and "GI specular intensity" (actual-widget smoke:
  6,872 assertions).
- Specular GI is a **reflection source**. When no screen-space provider is
  selected, the display plan routes the `Reflections` pass to it: the GI
  pass also writes `SpecularIllumination` (transient RGBA16F; the context is
  created without `DISABLE_SPECULAR`), and `Reflections` resolves it through
  the existing material resolve (reflectivity, reflection gain and split-sum
  BRDF from the material guide) into `ReflectionColor`, which the composite
  mixes with the GI specular intensity like any reflection. No HiZ pyramid or
  scene-colour read is declared for it. Instances are keyed by specular too;
  toggling it replaces the surface's instance.
- With **HiZ** reflections, GI specular fills the screen-space misses: HiZ
  stores hit confidence x material weight in alpha, and the material resolve
  (`ReflectionResolveLayering`) then runs in place over the HiZ output,
  adding GI radiance only for the weight the hits leave uncovered
  (`a = max(w_gi, a_hiz)`, encoded to the HiZ output's sRGB). The plan
  declares `SpecularIllumination` as a Reflections read and orders the pass
  after GlobalIllumination (`ReflectionMissesFromGlobalIllumination`).
  Polished-Link check: HiZ alone hits little of the body (mean weight 0.5 in
  the crop), with the fill the whole material reflects (21.8); core
  validation clean (Field 400 frames, Sheik copy 600).
- **FidelityFX SSSR** keeps the term: its intersect shades misses inside the
  shader (`lerp(environment, hit, confidence)`) and its denoiser carries no
  confidence, so filling its misses would need a fork of SSSR and its
  denoiser; GI specular is not traced with it.
- Misses: the GI specular trace has no environment of its own; the SDK fills
  missed rays with probe irradiance along the reflected direction (the
  environment intensity stays 0, so no sky is invented). Without GI for a
  sample the reflection is not composed (a cleared image would darken every
  reflective surface).
- Materials: the material guide and the texture material rules also apply
  when only GI specular is enabled. With no configured material, only draws
  whose native TEV reads the specular colour are reflective (calibrated
  guide); the Field and the Sheik copy have none, so specular GI changes
  nothing there until a material rule or PBR pack makes a surface
  reflective. Checked with a polished rule on Link's texture: the reflection
  follows Link exactly.
- Guide fix found on the way (affects every reflection provider and TAA
  reactivity): opaque guide writers have the material attachment in their
  write mask, but draws without a material of their own had no location-2
  output, and guides are cleared only with the native colour. Under
  Interpolated2x the synthetic sample therefore kept the previous sample's
  reflectivity and reactivity where Link had just been (a displaced dark
  silhouette on the grass). Such draws now write the neutral baseline
  (`PicaShaderInstrumentationFeature::MaterialGuideBaseline`, applied only
  while a material consumer is active); effects-off captures stay
  byte-identical to the Interpolated2x baseline (Field and Sheik copy).
- Measured (Field, 1920x1600 target, Half): GI dispatch 2.80 -> 3.35 ms with
  specular, resolve 0.13 ms; frame GPU 6.47 -> 7.14 ms. Core validation clean
  (Field 400 frames with the polished rule, Sheik copy 600 frames). The
  reflection debug views (probe `--debug-view 1/3/4`: material, radiance,
  weight) apply to the GI source as well.

## Translucent overlays (2026-09-29)

GI is light on the opaque surface below an overlay, so an overlay transmits
only its native destination factor of it. With GI enabled, the scene-domain
guide instrumentation (the mechanism AO uses for its ambient coverage)
includes the diffuse albedo guide: blended scene overlays write
`pica_diffuse_albedo_guide = vec4(0, 0, 0, pica_color.a)` through an
alpha-only coverage blend (source factor zero, destination factor the draw's
native `DestRgb`), and opaque overlays that do not write depth write alpha 0.
The albedo guide's alpha is therefore validity x transmission, and the
composite's existing `coverage = albedo.a` attenuates GI under water, flames
and other overlays. Output flag `SceneDomainAlbedoOverlay` (pipeline
manifest and key). Field check (GI debug view 3): fractional coverage only
on the moat water and the torch flames; core validation clean (Field 300,
Sheik copy 600 frames).

## Screen-space effects under interpolation (GTAO, SSSR, TAA, GI)

Measured in the Sheik copy cutscene (Interpolated2x, camera pan, per presented
sample): the camera the scene effects used (`LatestPerspective()`, published
by the guest camera hook once per native step) disagreed with the camera the
geometry was rendered with. Static frames agree exactly; during the pan the
guest eye was 8-19 native units away on real samples (the guest camera is one
simulation step ahead of the replayed geometry) and 10-25 units on synthetic
samples (the geometry is interpolated half a step); a camera cut moved the
guest camera 323 units one sample before the geometry. Every consumer that
reconstructs or reprojects positions from the rendered depth (GTAO, SSSR,
motion vectors / TAA / upscalers, grass, the distance-field centre and GI)
therefore sampled a displaced world.

Fix (sample view): each target accumulates the captured native world-to-view
of its opaque world draws for the presented frame (`SampleViewAccumulator`,
majority view; interpolated with the geometry for synthetic samples), and
every `NativeSceneView` publication uses `ResolveSamplePerspective`: the guest
projection with that view (WorldToClip, eye, look direction). A target without
such draws keeps the guest camera. The resolved perspective carries no guest
serial, so interpolated samples of one native step are distinct perspectives
and the previous-view history advances per sample. With effects off, Field and
Sheik copy stay identical to the Interpolated2x baseline.

Second defect found on the way: in scenes that compose more than one scene
surface per frame (a native copy boundary plus the presentation, Sheik copy),
FidelityFX SSSR reconfigured its single context and destroyed resources the
command buffer being recorded still referenced; the driver then faulted in
`vkQueueSubmit` (reproducible with SSSR in the Sheik copy, also before these
changes). SSSR now keeps one instance (context, output, temporal history) per
composed surface, retiring the least recently used one only when this frame
does not use it. The same single-configuration pattern (reconfigure, then destroy or update in
place) exists in GTAO, CACAO, HiZ, linear scene colour, motion vectors,
reflection resolve, TAA, SMAA, scanout and the transient image arena, and the
backend releases every effect-graph image client when the transient
allocation plan changes. `vkDeviceWaitIdle` does not cover commands still
being recorded in the current frame. Retirement contract:
`NriInteropContext::CompleteRecordedWork()` is the single boundary every such
reconfiguration calls; the backend installs it as "submit and complete the
recorded frame prefix (`FlushGpuTextureCopyPrefix`), wait for the device,
continue recording in the same frame". GTAO + SSSR in the Sheik copy now runs
to completion with core validation clean; effects-off Field and Sheik copy
remain identical to the baseline.

GTAO reconstructs view-space positions from depth and the projection only, so
the camera mismatch never affected it (old/new captures are bit-identical);
its interpolation failure was the mid-frame reconfiguration fault above.

## Risks

- **Geometry fidelity.** Programs without a typed transform hook are not
  voxelized; their surfaces neither occlude nor bounce. This is visible as
  missing bounce, never as wrong native output.
- **Cost.** Brixelizer update and GI dispatch are GPU compute; the RTX 3060
  budget must be measured (target: GI at 1/2 resolution within ~2 ms at
  1440p). Dynamic instances are budgeted.
- **Radiance feedback.** GI feeds on the previous lit output; interpolated
  samples and UI must not enter it (linear scene colour before UI only).
- **Portability.** The GI shaders need Vulkan 1.2 and subgroup/int64-free
  paths as compiled by the SDK; unsupported devices report the capability as
  unavailable instead of falling back silently.
