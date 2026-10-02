# Animated lighting: continuous light and shadow under every day/night change

Date: 2026-10-02. Baseline: `2f53274`, branch `perf/aot-levers`.
Status: **strategy, nothing implemented**. The measurements below are real;
every design value marked *initial* is a starting point to settle by
measurement in its phase.

The [NRI architecture](OOT3D_NRI_FIDELITY_EXTENSION_ARCHITECTURE.md) remains
authoritative. This plan extends the optional per-pixel lighting, the scene
environment profiles and the paged virtual shadow maps described there; it
changes nothing in Native lighting, Authentic or the F2 override.

## 1. Purpose

The game changes its lighting over time in many ways: the day/night cycle, the
Sun's Song, cutscenes that set or advance the time, scene changes that pick a
day or a night variant, weather, lightning, light changes between rooms. The
per-pixel renderer must show all of them with:

1. **Fluid motion.** Light and shadow move in every presented frame, also
   where the original moves in steps.
2. **No pops.** No visible jump of light or shadow except where the game itself
   cuts (a camera cut, a fade, a scene change).
3. **Constant cost.** A moving light never costs more per frame than a fixed
   budget.
4. **Intrinsic coverage.** Nothing in the renderer knows *why* the light
   changed. Every present and future day/night change is covered because it
   can only reach the renderer through one signal (section 4).

Decisions already taken by the user (2026-10-02):

- Motion must be fluid even where the original is stepped.
- A light below the horizon casts no shadow and its direct light is attenuated.
  Section 5.3 narrows the attenuation to sky lights, with the evidence.
- The existing scene profiles are disposable; they are recreated in the new
  profile standard (section 5.8).

## 2. Requirements

| # | Requirement | Verified by |
|---|---|---|
| R1 | While the native light moves, the light direction used for shading changes in every presented frame, with no speed discontinuity above the threshold of section 7 | light-direction series |
| R2 | While the light moves, the shadows move in every presented frame | shadow blend series, edge tracking in consecutive captures |
| R3 | No direction jump of light or shadow unless the game cut (section 4.2, class C) | jump count against the cut list |
| R4 | The shadow-casting light is never below the horizon | below-horizon frame count = 0 |
| R5 | Shadow map work per presented frame never exceeds the page budget, at any light speed | pages per frame, GPU time on moving frames |
| R6 | Day to night and night to day are continuous at any speed (normal time, Sun's Song, time-lapse cutscenes) | probes of section 7 |
| R7 | A fast native event (lightning, flash) is not smeared | colour series around the event |
| R8 | Native lighting, Authentic and F2 are byte-identical to today; the feature is inert when per-pixel lighting is off | deterministic captures |
| R9 | A scene profile works for every light state of its scene (day, night, cutscene, weather) without per-state authoring | profile probes at several times |

## 3. Evidence

### 3.1 Measured: the title intro

Boot without input, user configuration at 1280x720, 5,400 presented frames
(`qualify/intro-lights` in the work directory: `boot_probe.py`,
`boot1-light-trace.txt`, `boot1-shadow-series.json`, `boot1-sheet.png`,
`boot1-flip.png`). The title scene is `0x6B` and has no profile, so the native
environment lights drive per-pixel lighting and the virtual shadow map.

- The scene has two environment directionals: `L0` (sun) and `L1` (moon),
  **exactly opposite** at every frame. Directions are signed bytes; colours
  change by one unit every few frames.
- **Stepped motion.** The main light moves in 74 of 900 guest frames, in steps
  up to 0.68 degrees, and stays still for 14 frames (median) between steps.
- **Pop at dawn.** The shadow light is "the brightest". The sun becomes
  brighter than the moon while it is 20 degrees *below* the horizon: in one
  frame the shadow direction turns by 180 degrees (dot product -1.0, presented
  frames 584 and 1184).
- **Shadows from below the ground.** After that flip the shadow light stays
  below the horizon for 1,317 of 3,800 frames (about 22 of 63 seconds).
- **Full shadow-map redraw at every step.** Each direction change redraws 215
  of 220 resident pages, static ones included: GPU frame 6.5 -> 9.8 ms mean,
  15.5 ms maximum, on 273 frames. Interpolated presentation creates two or
  three intermediate directions per native step, each a full redraw.
- A cutscene time skip moves the direction by 15 degrees in one frame (a cut).
- The next demo scene (`0x17`) has a light **from below** on purpose: an orange
  light, direction `(-63, -127, -31)`, under a lava-lit room.

### 3.2 The engine's light state machine

From `code.bin` (EUR), the environment update around `0045DFD8..0045EEE0`
(`r4` = environment context, `sl` = save context). Addresses and offsets are
read from the disassembly; the role of each field is inferred from its use and
is confirmed against a live trace in P0:

| Address | Fact |
|---|---|
| `0045DFD8..0045DFE0` | The clock (`save context + 0xC`, 16 bits) advances every frame by twice a value loaded just before (the time speed) |
| `0045E068..0045E078` | **Time mode** when `env + 0x20 == 0` and `env + 0xA7 == 0xFF`; otherwise **settings mode** (`0045EA58`) |
| `0045E0A4..0045E430` | Time mode: colours are blended between the entries of a time table (`env + 0x21` selects it; `env + 0x22` and `+0x23` are read in the same loop, a second table and its enable) and truncated to bytes |
| `0045E438..0045E4A0` | Time mode: the sun direction is recomputed from the clock: `x = -sin(t) * 120`, `y = cos(t) * 120`, `z = cos(t) * 20`, **truncated to signed bytes** at `env + 0xB5..0xB7` |
| `0045E4A4..0045E4C0` | Time mode: the second light is the exact negation (`env + 0xBB..0xBD`) |
| `0045EA58..` | Settings mode: light setting `env + 0xA5` (28-byte records: ambient, light 1 direction and colour, light 2 direction and colour, fog), optionally blended from `env + 0xA6` with the weight at `env + 0xC8` when `env + 0xA4` is set |

Consequences:

- **Every** lighting change goes through this one update and ends in the same
  small state: two directionals (direction, colour), ambient, fog. The adapter
  already captures the two directionals from their live records
  (`NativeA32LightingProbe::CaptureSceneLights`).
- The steps of the sun are a truncation, not a property of the source: the
  clock has 65,536 positions per turn (0.0055 degrees).
- The engine itself knows whether its lights are **sky lights** (time mode) or
  **authored fixed lights** (settings mode). No heuristic can replace this: a
  fixed pair such as `(73,73,73)` / `(-73,-73,-73)` is also exactly opposite.

The read-only decompilation evidence (`I:/oot3decomp`, never copied) confirms
that cutscenes carry set-time and lighting commands; both act on the state
above.

### 3.3 Offline smoothing prototype

`qualify/intro-lights/smooth_prototype.py` on the recorded trace (guest frames
301-1201 and 1908-2423):

| Reconstruction | Frames moving | Step per frame | Largest change of step | Distance from the ideal path |
|---|---|---|---|---|
| Native bytes | 74 of 900 | up to 0.68 deg | 0.68 deg | - |
| Critically damped spring, 1.5 rad/s | 892 of 900 | 0.032 deg | 0.0016 deg | 1.30 deg |
| Critically damped spring, 3 rad/s | 892 of 900 | 0.033 deg | 0.0055 deg | 0.65 deg |
| Critically damped spring, 6 rad/s | 892 of 900 | 0.034 deg | 0.022 deg | 0.32 deg |
| Ramp between steps | 892 of 900 | 0.033 deg | 0.042 deg | 0.24 deg |

A filter makes the motion uniform at the price of a lag that grows with speed.
The exact clock (3.2) gives the same uniformity with no lag; the filter is
therefore the fallback, not the main path (5.2).

### 3.4 What the renderer does today

- `ResolvePicaDirectionalShadowLightSelection` and
  `FindPicaSceneMainDirectional`: the shadow light is the brightest directional,
  wherever it points.
- `BuildVirtualShadowLayout`: any direction change larger than float roundoff
  invalidates every page.
- `PicaEnvironmentLighting`: a profile adds a key light with a constant colour
  and intensity; the user's two profiles mute the native direct light, so only
  the native ambient (30% / 50%) follows the game.
- `ResolvePicaEnvironmentKeyDirection` (commit `9669831`): the key direction can
  follow the main scene light, so it inherits the flip and the steps.
- Scene lights are paired by source identity for interpolated presentation.
- Native light changes do not reset temporal history; only a profile change
  does (`mFrameEnvironment`).

## 4. Governing principle: one signal, classified by shape

### 4.1 The single choke point

The renderer consumes one typed snapshot of the native light state per guest
frame. Section 3.2 shows that every mechanism of the game writes that state.
The design therefore never asks what happened in the game; it classifies the
**shape** of the signal. A new or unlisted gameplay situation cannot escape,
because it has no other way to change the lights.

### 4.2 Signal classes

| Class | Shape of the signal | Treatment |
|---|---|---|
| S | Still: nothing changes | One shadow layout, single sampling; today's cost |
| A | Slow continuous motion of direction | Exact or reconstructed continuous direction (5.2); shadow keyframes with continuous dissolve (5.5) |
| B | Fast continuous motion (several degrees per second) | Same path; the shadow page set shrinks so the budget and the spacing hold (5.5) |
| C | Cut: a jump beyond what motion explains, or a declared discontinuity | Snap every filter; rebuild the shadow layout in that frame (as today) |
| D | Energy only: colours, ambient, fog change, direction fixed | Colours follow, dequantised when stepped, untouched when fast (5.2); no shadow work |
| E | Handover: the shadow-casting light changes identity | Continuous by the horizon model for sky lights (5.3); timed fade otherwise (5.4) |

A change can belong to several classes at once (dawn is A + D + E).

Cut detection uses evidence, never the cause:

- scene identity or environment context changed;
- presentation continuity epoch changed (state load, renderer target reset);
- the engine switched between time mode and settings mode;
- the direction turned by more than `cut angle` in one guest frame (*initial*:
  8 degrees; the Sun's Song probe of section 7 must stay below it);
- a colour changed by more than `event step` units in one guest frame
  (*initial*: 3): passed through unfiltered, not a cut for the shadow map.

### 4.3 Coverage of the game's day/night and light changes

"Measured" means traced in section 3.1. "Expected" means derived from the
engine structure of section 3.2 and to be confirmed by the probe named in
section 7; none of these rows needs its own code.

| Situation | Engine path | Classes | Status |
|---|---|---|---|
| Time passing in open areas | time mode, clock advances | A + D + E at dawn and dusk | expected (P-field) |
| Sun's Song where time flows | time mode, clock advances fast | B + D + E | expected (P-song-flow) |
| Sun's Song where time is fixed | fade, scene reload at the new time | C | expected (P-song-fixed) |
| Title intro, attract demos, credits time-lapses | time mode driven by cutscene | A/B + D + E, C at camera cuts | **measured** |
| Cutscene set-time command | clock rewritten | C (or A if below the cut angle) | measured once (15 degree skip) |
| Cutscene lighting command, with or without blend | settings mode, blend weight | D, A for blended directions, C without blend | expected (P-intro-new-game) |
| Entering or leaving an area that picks a day or night variant | scene load | C | expected (P-town) |
| Gate and drawbridge sequences at dusk and dawn | time mode plus cutscene | A + D + E, C at cuts | expected (P-field) |
| Fixed-time scenes (towns, interiors, dungeons) | settings mode, constant | S | measured (Kokiri, Link's house) |
| Light settings changing between rooms | settings mode, blend | D, A | expected (P-interior) |
| Weather onset and clearing | time mode, table blend | D | expected (P-weather) |
| Lightning, flashes, boss effects | colour adjustments | D with event pass-through | expected (P-intro-new-game) |
| Warps, death, continue, loading a save | scene load | C | expected (P-town) |
| Loading a TriAevum state | continuity epoch | C | measured mechanism (state load exists) |
| Pause, dialogue, menus | clock stops | S | expected |

## 5. Design

### 5.1 Typed scene light snapshot (OOT3D adapter)

`PicaSceneDirectionalLight` gains, per light:

- `DirectionTowardSource`: the best native direction available (5.2);
- `SkyLight`: true when the engine is in time mode (3.2). Title semantics stay
  in the adapter; the shared renderer sees only the typed flag;
- unchanged: `SourceIdentity`, `Color`.

`PicaSceneDirectionalLights` gains a `ContinuityEpoch` that the adapter
advances on scene change, environment re-initialisation and mode switch, so the
renderer's cut detection (4.2) does not read title state.

The title already publishes its axes (`WorldUp`, `WorldAzimuthZero`) for the
profile; they move to the snapshot so that scenes without a profile have them.

### 5.2 Continuous reconstruction

The published snapshot is the continuous one. Everything downstream (surface
lighting, shadow map, grass, GI) reads the same values; nothing downstream
filters again.

**Sky lights: exact direction from the clock.** In time mode the adapter
recomputes the direction of 3.2 in floating point from the 16-bit clock, and
checks it: truncating its own result must reproduce the three native bytes.
If the check fails (another writer changed the record), it publishes the native
bytes and the dequantiser below applies. No lag, no parameter, exact at any
clock speed: classes A and B share one path.

**Everything else: dequantiser.** Fixed-mode directions that blend between
light settings, and all colours, are integer signals. Per channel:

- the output is a critically damped spring toward the native value;
- its rate follows the cadence of the native steps (*initial*: time constant =
  1.5 step intervals, at least 3 rad/s), so the lag stays about one and a half
  quantisation steps at any speed instead of growing with it;
- the output never leaves a band of 1.5 units around the native value;
- a change above `event step` (4.2) passes through at once: lightning stays
  sharp, a blend that already moves every frame is not delayed;
- a cut (4.2) snaps.

The filter runs once per guest frame with the fixed guest time step, so runs
are deterministic. Its state is not saved: a state load is a cut.

**Presented frames.** The existing identity-paired interpolation samples the
continuous snapshots for each presented frame; a sky direction interpolates the
clock angle, not the truncated vector.

### 5.3 Horizon model

With `up` the published world up and `e` the elevation of a light
(`sin e = dot(direction, up)`):

- **Shadow eligibility (all lights).** A light with `e <= 0` never casts the
  shadow map.
- **Shadow strength (all lights).** `shadow(e) = smoothstep(sin 1 deg, sin 8 deg, sin e)`
  (*initial*): shadows lengthen and fade toward the horizon instead of ending.
- **Direct light (sky lights only).** `direct(e) = smoothstep(sin -1 deg, sin 4 deg, sin e)`
  (*initial*) multiplies the light's direct term.

Why the attenuation is limited to sky lights: a sun below the horizon is
hidden by the ground, but fixed-mode scenes place lights below the horizon on
purpose (the lava glow of scene `0x17` in 3.1, the common fill from below).
Removing those would darken interiors against the art. The engine's own mode
flag separates the two cases exactly (3.2).

Effect on the day/night handover: sun and moon are opposite, so exactly one is
above the horizon. As the sun sets its shadows fade to zero at the horizon,
where the moon's start from zero. Dawn and dusk are continuous at any speed
with no special case; the brightness crossover that causes today's flip no
longer selects anything.

### 5.4 Main light and handover

- `effective energy = luminance(colour) * direct(e)` for a sky light,
  `luminance(colour)` otherwise; zero for any light with `e <= 0` as a shadow
  candidate.
- The shadow-casting light is the eligible light with the largest effective
  energy, with hysteresis: a challenger takes over only above the incumbent by
  a margin (*initial*: 10%).
- When the identity changes outside a cut, the shadow strength fades out, the
  identity swaps at zero, and it fades in (*initial*: 0.4 s in total, guest
  time). For the sky pair the horizon model already holds the strength at zero
  at the swap, so the fade is invisible; the rule matters for fixed-mode pairs
  whose colours blend across each other.
- `FindPicaSceneMainDirectional` stays the single owner of the rule, shared by
  surface lighting, the shadow pass and the profile (5.8).

### 5.5 Shadow keyframes

A light that moves in every frame would invalidate the whole virtual shadow map
in every frame (3.1: +3 to +9 ms). Instead:

**Layouts.** Up to three shadow layouts exist per target: two *displayed*
(`K[n-1]`, `K[n]`) and one *in construction* (`K[n+1]`). Each is a complete
virtual shadow layout for one fixed light direction, with its static cache.

**Construction under a fixed budget.** Each presented frame renders at most
`B` pages of `K[n+1]` (*initial*: 16; one page measured at about 0.015 ms),
in the existing residency priority order (near to far). The layout's direction
is the light direction when its construction starts.

**Continuous dissolve.** Receivers sample `K[n-1]` and `K[n]` and blend with a
weight that advances every presented frame from 0 to 1 over the construction
time of `K[n+1]`. When it reaches 1, `K[n+1]` is complete: the layouts rotate
and the next construction starts. The displayed shadow therefore follows the
light continuously, one to two construction periods behind (about 0.5 s and
0.5 degrees at the measured intro speed).

**Spacing law.** With `P` selected pages, light speed `v` and frame time `dt`:
construction time `T = ceil(P / B) * dt`, keyframe spacing `delta = v * T`.
The page set is chosen so that `delta <= delta_max` (*initial*: 0.5 degrees):

- slow motion (class A): the full set fits; spacing about 0.25 degrees at the
  measured intro speed;
- fast motion (class B): the set shrinks from the finest levels outward. Near
  shadows fall back to the coarser resident levels the virtual shadow map
  already provides, so shadows soften while the light sweeps and sharpen when
  it slows. Cost stays at `B` pages per frame by construction;
- if even the coarsest set exceeds `delta_max`, the spacing grows; the
  dissolve then reads as a blur of the sweep.

**Still light (class S).** When the weight reaches 1 and the light has not
moved, no construction starts; receivers sample one layout, as today.

**Cut (class C).** All layouts are dropped and one is rebuilt in that frame, as
today. Cuts coincide with camera cuts, fades or scene loads.

**Dynamic casters.** Each displayed layout recomposes its dynamic pages every
frame, as one layout does today; two layouts double that small cost (9 pages
per frame measured on a still light).

**Memory.** Up to three pairs of atlases (static cache and composed depth)
instead of one: at 2048, 96 MiB against 32 MiB; less if pages without dynamic
casters are sampled from the static cache. At 4096 it would be 384 MiB against
128: *initial* rule, a moving light uses 2048 layouts; a still light keeps the
configured size. To settle in P2.

**Why not fewer layouts.** With two layouts, display and construction must
alternate, and the shadow would move in visible bursts. The alternative that
reaches two layouts without bursts is listed in section 10.

### 5.6 Sampling at the presentation rate

Light direction, colours, horizon factors, handover fade and dissolve weight
are all evaluated per presented frame. The guest rate (and the interpolation
multiplier) does not show in the motion.

### 5.7 Temporal consumers

- **TAA / DLSS / FSR.** Native light changes keep not resetting history. A
  colour event (4.2) above a threshold raises the reactive mask for that frame
  so a flash does not trail; to verify in P4.
- **MXAO temporal.** Independent of light; no change.
- **Brixelizer GI.** Its history weight drops with the change of key and
  ambient energy between frames, and resets on a cut; P4, GI is optional and
  off in the reference configuration.
- **Grass.** Consumes each draw's surface-lighting uniforms and the shadow
  grid; it follows without its own logic. The shadow grid samples the same two
  layouts and weight.
- **Point-light shadows.** Unaffected.
- **Fog and sky.** Native, already animated; the profile's fog strength scales
  them.

### 5.8 Profile standard, version 6

A profile no longer replaces the scene's light with a constant one. It
**regrades the native lights**, so it inherits every native change and one
profile serves every state of its scene (R9).

```json
{
  "schema": "triaevum.scene-environment",
  "version": 6,
  "sceneId": 85,
  "enabled": true,
  "sources": [
    {"gain": 2.0, "tint": [1.0, 0.96, 0.9]},
    {"gain": 1.2, "tint": [0.8, 0.88, 1.0]}
  ],
  "shadowDirection": {"mode": "source", "azimuthOffset": 0.0, "elevationOffset": 0.0},
  "ambient": {"nativeGain": 1.0, "addedColor": [1.0, 1.0, 1.0], "addedIntensity": 0.0},
  "nativeLocalGain": 1.0,
  "fogStrength": 1.0,
  "addedLight": null,
  "pointLights": [
    {"enabled": true, "intensity": 1.0, "radiusScale": 1.0, "colorTint": [1.0, 1.0, 1.0]},
    {"enabled": true, "intensity": 1.0, "radiusScale": 1.0, "colorTint": [1.0, 1.0, 1.0]}
  ]
}
```

- `sources[i]` regrades native environment light `i`: in time mode the sun
  (0) and the moon (1), so day and night have independent looks; in settings
  mode light 1 and light 2. The light's colour is `native * tint * gain`,
  after the horizon model.
- `shadowDirection` applies to whichever light casts the shadows (5.4):
  `source` with a fixed azimuth and elevation offset, or `otherAxis` (the axis
  of the other light, on the caster's side; today's `scene_ambient`). The
  energy always follows the native elevation; the offset direction is clamped
  above a minimum elevation (*initial*: 3 degrees) so an offset never pushes
  the caster below the ground.
- `ambient.nativeGain` scales the native ambient (which follows the game);
  `added*` is an optional constant term.
- `addedLight` is the optional constant light for art direction (today's
  authored key in its fixed form): direction, colour, intensity, shadowless.
  It does not follow the game, by definition.
- Versions 1 to 5 still load, as today, and behave as they do today; saving
  writes version 6. The user's two profiles are recreated, not migrated.

F1 **Environment** remains the only owner. The Key light tab becomes **Scene
lights** (per-source gain and tint, shadow direction) and **Added light**.
New profiles start from gains of one: enabling a profile changes nothing until
the user moves a control.

### 5.9 Ownership and inertness

- Title addresses, the time-mode flag and the clock formula live in the OOT3D
  adapter (`oot3d_native_a32_lighting`). The dequantiser, horizon model, main
  light rule, handover and shadow keyframes are reusable mechanisms in the
  shared 3DS renderer and the shadow pass.
- Native lighting, Authentic, F2 and native-memory rendering never read the
  snapshot; they stay byte-identical (R8).
- Settings: `delta_max`, the page budget and the horizon angles are renderer
  constants, not user settings, until a measurement shows a need.

## 6. Phases

Each phase ends with its metrics of section 7 measured before and after, the
deterministic captures, the structural diagnostics, and the F1 smoke test where
F1 changes.

### P0. Instruments and probes

- Promote the temporary light trace to a diagnostic
  (`TRIAEVUM_SCENE_LIGHT_TRACE`): native bytes, mode flag, clock, published
  values, class decided for the frame.
- Add to the frame diagnostics: light direction used, shadow weight, pages
  built for the layout in construction, displayed layouts.
- Resolve the save-context address and the time-speed value read at
  `0045DFD8`; confirm the mode flag against the trace.
- Build the probes of section 7 that need input or a state (field, Sun's Song,
  town, interior, new-game intro).
- **Exit:** every row of 4.3 has a recorded trace and its class.

### P1. Continuous light

- Snapshot fields of 5.1; exact sky direction and dequantiser of 5.2.
- Horizon model (5.3), main light and handover (5.4), in
  `FindPicaSceneMainDirectional`, surface lighting and the shadow selection.
- Tests: dequantiser unit tests on the recorded trace (uniform speed, bounded
  band, event pass-through, cut snap); horizon and handover unit tests;
  composition tests for the adapter's clock check.
- **Exit:** R1, R3, R4, R6 for light; R7; R8. The shadow map still redraws on
  every change (cost unchanged or worse: the light now moves every frame). P1
  is therefore not shippable alone on moving scenes; P2 follows directly.

### P2. Continuous shadows

- Three layouts per target, budgeted construction, receivers with two layouts
  and a weight, spacing law and page-set selection, memory rule (5.5).
- Grass shadow grid on the same layouts.
- Tests: layout rotation and budget unit tests; still-light captures identical
  to the single-layout path; structural diagnostics for the declared histories.
- **Exit:** R2, R5, R6 for shadows; GPU time on moving frames within the noise
  of still frames plus the measured two-layout sampling cost.

### P3. Profile standard

- Version 6 schema, loader for 1-5 unchanged, F1 tabs, panel smoke test.
- Recreate the Kokiri and Link's house profiles with the user.
- **Exit:** R9; a profile probed at day, night and a cutscene state.

### P4. Events and temporal consumers

- Reactive mask on colour events; GI history response; DLSS/FSR checks on
  lightning and on the dawn/dusk sweep.
- New-game intro probe (lightning, cutscene light settings).
- **Exit:** R7 on real events; no trailing in captures after a flash.

### P5. Optional refinements

- Warped single-layout lookup (section 10) if the dissolve shows double edges
  on long shadows or the memory rule is too tight.
- Per-light-setting art direction, only if regrading by source is not enough.

## 7. Verification

### Probes

| Probe | Scenario | Covers |
|---|---|---|
| P-boot | Boot, no input, title intro (exists) | A, D, E at dawn, C at cuts |
| P-field | State in an open area, time running through dusk and dawn | A, D, E both ways |
| P-song-flow | Sun's Song in an open area | B, E at speed |
| P-song-fixed | Sun's Song in a fixed-time area | C |
| P-town | Enter and leave a town at day and at night | C, S |
| P-interior | A room change with different light settings | D, A in settings mode |
| P-weather | A weather change | D |
| P-intro-new-game | New game opening, scripted menu input | D events, settings-mode cutscene lighting |
| P-state | Load a state taken at another time of day | C |

### Metrics and thresholds

| Metric | Today (P-boot) | Target |
|---|---|---|
| Guest frames in which a moving light moves | 74 of 900 | all |
| Largest change of angular step between frames | 0.68 deg | <= 0.01 deg outside cuts |
| Direction jumps outside cuts | 2 (the 180 degree flips) | 0 |
| Frames with the shadow light below the horizon | 1,317 of 3,800 | 0 |
| Pages rendered in one frame by a moving light | 215 of 220 | <= budget (16) plus dynamic pages |
| GPU frame time on moving frames against still frames | 9.8 against 6.5 ms, 15.5 max | within noise plus two-layout sampling |
| Shadow edge position across consecutive captures | steps | monotonic, even |
| Native / Authentic / F2 captures | - | byte-identical to the baseline |

All timing comparisons use interleaved rounds; the GPU is shared on the
development machine.

## 8. Cost budget

| Item | Cost |
|---|---|
| Snapshot, clock direction, dequantiser | negligible CPU, once per guest frame |
| Layout construction while the light moves | at most 16 pages per frame, about 0.25 ms GPU (*initial*) |
| Second displayed layout while the light moves | double PCF sampling in receivers and the grass grid; to measure in P2 |
| Dynamic casters while the light moves | twice today's dynamic pages (about 9 per frame measured) |
| Still light | exactly today's cost |
| Memory at 2048 | up to 96 MiB against 32 MiB |

## 9. Risks

| Risk | Mitigation |
|---|---|
| The dissolve shows a double edge at the tip of long shadows | Spacing law keeps the offset inside the filter width near the caster; the horizon fade removes the longest shadows; P5 warp |
| Two-layout sampling costs more than expected at 4K | Measure in P2; sample the second layout only where the first reports an occluder nearby; stochastic choice resolved by the upscaler as a fallback |
| Another writer changes the sky record (cutscene) | The clock check of 5.2 falls back to the native bytes and the dequantiser |
| The Sun's Song step exceeds the cut angle | P-song-flow sets the threshold; the clock speed read in P0 bounds it |
| A fixed-mode scene relies on a light from below as its main light | It still lights; it only never casts the shadow map (5.3). Reviewed per probe, not per scene |
| Memory at 4096 | Moving lights use 2048 layouts (5.5) |
| Shadows trail the light by about half a second | Invisible at measured speeds (0.5 deg); class B shortens the construction time |

## 10. Alternatives considered

- **Redraw the whole shadow map every frame.** Exact and simplest; +3 to +9 ms
  per frame measured. Rejected by R5.
- **Hold the map and swap at intervals.** Constant cost but stepped shadows.
  Rejected by R2.
- **Two layouts with alternating construction and dissolve.** Shadows move in
  bursts. Rejected by R2.
- **One layout with a warped lookup.** The receiver shifts its lookup by the
  occluder distance times the angle between the layout's direction and the
  light's; two layouts suffice (one displayed and warped, one in construction).
  Lower memory and single sampling, but approximate at depth discontinuities.
  Kept as the P5 refinement, to be judged against real double edges.
- **Filter only (no clock).** Works (3.3) but lags in proportion to speed and
  needs tuning for the Sun's Song. Kept as the fallback path of 5.2.
- **Profiles per light setting or per time of day.** Covers only the states
  someone authored. Rejected as the base by R9; possible later as art
  direction (P5).
- **Attenuate every light below the horizon.** Removes authored interior
  lighting (3.1). Rejected; the engine's mode flag decides (5.3).

## 11. Open points to settle by measurement

1. `cut angle`, `event step`, hysteresis margin, handover time (4.2, 5.4).
2. Horizon angles for `shadow(e)` and `direct(e)` (5.3), judged on dusk and
   dawn captures.
3. Page budget and `delta_max` (5.5), judged on edge tracking and GPU time.
4. Layout size rule at 4096 (5.5).
5. Whether colours need the dequantiser at all once gains are applied (5.2).
