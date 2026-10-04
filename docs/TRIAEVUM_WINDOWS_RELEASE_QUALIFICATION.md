# TriAevum: Windows qualification of the line after alpha.2c

Status: inventory and battery design, 2026-10-04. Branch `qual/windows-next`
(from `perf/aot-levers` `cf4d331`). This record lists every development
intended since the last good release, how each one is proven to work on
Windows, and what is open. It is not a release approval.

## 1. Scope and decisions

- Baseline: `v0.6.0-alpha.2c` (`739f03e`, 2026-09-12), the last published
  release that was not withdrawn. `v0.6.0-alpha.3` and `v0.6.0-alpha.3b` were
  withdrawn (`a9b4477`); their regressions are recorded and corrected in
  `TRIAEVUM_ALPHA2C_TO_ALPHA3_RECONSTRUCTION.md` (issues 45/49, 51, 53 and the
  title-neutral renderer gate).
- Line under qualification: 286 commits, `v0.6.0-alpha.2c..qual/windows-next`:
  142 of `feat/azahar-static-aot-nri` (to `807b66e`, 2026-09-27) and 144 of
  `perf/aot-levers` (2026-09-28 to 2026-10-04).
- Platform: Windows only. Linux, Steam Deck, macOS and Android stay out of this
  qualification; their Python release suites still run because they share code.
- Global illumination (Brixelizer GI and the screen-space GI) is not part of the
  next release. Both stay in the source as experimental paths: off by default,
  not offered by a release build, their tests kept (section 6).
- Outside the line: the uncommitted Azahar platform / static AOT work of the
  main checkout and its preservation branch `feat/azahar-static-aot-nri-recovered`;
  the macOS (#16) and Linux port proposals.
- Open decision for the user: whether the iMMERSE MXAO module (all rights
  reserved, used under the author's exclusive permission) ships in a binary that
  also links GPL-2.0+ code (`LICENSE_SCOPE.md`, "Exception: the iMMERSE module").

## 2. Sources of intended developments

| Source | What it holds | Treatment |
| --- | --- | --- |
| Commit history (286) | features, fixes, diagnostics, refactors | grouped below by area |
| Recovery snapshots of 2026-09-30 | `recovery/omp-2026-09-28` (`88f935a`, main-checkout work reverted by a worktree reset), `recovery/renderer-2026-09-26` (`a33d1f6`) | 87 % and 95 % of their lines are in the line (restored by `e9dc221` and later work); the remainder is the concurrent Azahar/AOT workstream (section 4.R) |
| Open pull requests | #6, #11, #13, #14, #15, #16, #18 (999sian, PabloVSouza) | integrated selectively on 2026-09-09/10, before alpha.2c, as replacement implementations (`TRIAEVUM_CONTRIBUTIONS.md`); their branches stay open, so their own lines are mostly absent (0-69 %) while their behaviour is in the line. What was deliberately not adopted is listed in section 4.R. #50 (high refresh presets) and the closed #57 are proposals outside the line |
| GitHub issues | user reports since alpha.2c | a fix claimed by a commit or document needs a scenario check (section 4) |
| Qualification documents | `TRIAEVUM_TOPSCREEN_ISSUES_25_32_39.md`, `TRIAEVUM_ISSUES_40_43.md`, `TRIAEVUM_ISSUES_45_46.md`, `TRIAEVUM_ISSUE_48_TITLE_START.md`, `TRIAEVUM_ISSUE44_HD_FONTS.md`, `TRIAEVUM_RUNTIME_PROGRESS_QUALIFICATION.md` | their open acceptance items become battery entries |

Line survival (lines a commit added that still exist anywhere in the tree,
moves and splits included) was measured for every commit: below 70 % only for
generated shader headers replaced by binary packs (`86bb942`), deliberate
reverts (LAUNCHPAD `21bf907`, shadow receiver history `c31bd43`), the split
F1/F12 frontend kept on `feature/deferred-f1-f12-menus` (`a946f34`), GI
iterations, and the TextureCopy GPU decode of 2026-09-23 (`85fefa8`, `0765e0c`,
`602d10e`, `6cc19cc`: 18-27 %), which section 4.I checks.

## 3. Battery layers

| Layer | Content | Runner |
| --- | --- | --- |
| L1 build | clean configure and build of the product (`TriAevum.exe`), the title module and every test target; an expected target list, so a test that is not built fails the battery | battery runner |
| L2 unit | every host test executable, the GPU test project (with its generator `--check` modes), the Python suites (`tools/triaevum_release`, `tools/oot3d/native_a32_runtime`, `tools/renderer/tev_program`, other tool suites) | battery runner |
| L3 F1 | actual-widget smoke (`triaevum_f1_settings_smoke`) | battery runner |
| L4 identity | deterministic Kokiri captures on eight renderer profiles against the reference set, `runtime.json` structural diagnostics, GI-off identity | `qualify/baseline` gate, kit |
| L5 scenarios | savestate or save-file scenarios driven by recorded input, with framebuffer and runtime assertions (section 5) | battery runner, scenario files |
| L6 package | release build options, Forge installation into a deep path without compilation, release audit and layout | battery runner |
| L7 user | short manual checklist for what cannot be automated (feel of input devices, audio, long play) | this document |

Every entry below names the layer and the check that proves it. Status values:
`pass` (proven on this line), `open` (no proof yet), `fail`, `lost` (intended,
absent from the line), `experimental` (excluded from the release).

## 4. Inventory

### 4.A Release, Forge and packaging (Windows)

| ID | Development | Evidence | Check | Status |
| --- | --- | --- | --- | --- |
| A1 | Forge counts embedded renderer shader artifacts during preparation | `5d13fa7` | L2 release suite; L6 install | open |
| A2 | Atomic writes keep temporary paths inside the legacy limit (Forge in a deep root, #53) | `4a09933`, reconstruction 3.1 | L2 `test_common` path capacity; L6 install into a padded root | open |
| A3 | Forge prepares the renderer's startup programs during installation | `6d2f40a` | L6 install, then first launch reports prepared programs | open |
| A4 | USA hint movies get the names the EUR module opens | `e5fbc5a` | L2 Forge suite; L5 movie scenario on a USA install (if a USA input exists) | open |
| A5 | Developer PGO profile accepted for title modules (developer path only) | `41cc7b5` | L2; L6 confirms the release module build options | open |
| A6 | iMMERSE module isolated with its own licence, release layout maps both licence files | `b94b3b8` | L2 release audit tests; L6 audit | pass (gate `verify-immerse`) |
| A7 | Source-archive UI contract reads the CMake area files | `27e5a10` | L2 | pass |

### 4.B Startup, shader preparation and stutter

| ID | Development | Evidence | Check | Status |
| --- | --- | --- | --- | --- |
| B1 | Precompiled native, temporal, toon, uber, caster and pass families; no shader compilation in the product | `247f234`..`4d0e39b`, `86bb942`, `271b9a5` | L2 generator `--check`; L5 runtime reports zero runtime compilations | open |
| B2 | Pipeline identity excludes dormant state; shared normalized pipelines; bounded retirement | `801a359`, `d42143c`, `aaa3530`, `a9d9441`, `fd9195a` | L2 pipeline tests; L4 | pass (L4) |
| B3 | Background program preparation, always-available programs, hybrid default | `4151f1a`, `a430ab8`, `271b9a5`, `44b59e3` | L4; L5 cold-cache scenario: no first-use stall counters | open |
| B4 | Pipelines prepared in the background are persisted and reused | `038dcf9`, `b644be7`, `390d918` | L5 second launch: every effect pipeline from the cache | open |
| B5 | Initial preparation before gameplay | `743d05a` | L5 boot scenario: preparation completes before the first gameplay frame | open |
| B6 | Startup stalls reduced; swapchain fence ownership retired | `18bc3c0`, `e6490d7` | L5 timing receipts | open |

### 4.C Renderer core

| ID | Development | Evidence | Check | Status |
| --- | --- | --- | --- | --- |
| C1 | Parametric PICA TEV, lighting, fog, alpha, texture selection, procedural texture with GPU parity | `12b2c97`..`054bb5b` | L2 GPU parity tests | open |
| C2 | Offline-translated native vertex family; title-owned precompiled vertex programs | `8727eb4`, `91d4c5b` | L2 `build_vertex_artifacts --check` (needs the private SHBIN copies) | open |
| C3 | Descriptor pools grow on demand; live shader contracts promoted after replay | `29b8bdb`, `8079f6c` | L2; L5 long gameplay scenario | open |
| C4 | Shared 3DS renderer stays title-neutral (boundary gate) | `ba1f200`, reconstruction 3.4 | L1 `verify_renderer_3ds_cross_title_boundary` | open |
| C5 | Scene completion exposed only before native UI; title and menu draw domains (#48) | `d363d36`, `16be838` | L5 title Start scenario | open |
| C6 | Per-draw allocation, register-hash and lighting-decode levers | `83c7be4`, `2c545df`, `754ccb3`, `8cdf3ca` | L4 identity | pass (L4) |
| C7 | Effects reconstruct depth with the draws' projection; one shared geometry guide | `0dfc904`, `567281a` | L4; L2 effect-graph tests | pass |
| C8 | Toon: uniform appearance, native raster initialization outside toon lighting, offline outline coverage | `8735f46`, `ebd6ba6`, `8c4b6b1`, `6ad3d83` | L4 `cfg-gg-outline*`; L2 `oot3d_toon_outline_gpu_tests` (one failing case, section 7) | open |

### 4.D Presentation and pacing

| ID | Development | Evidence | Check | Status |
| --- | --- | --- | --- | --- |
| D1 | Bounded presentation catch-up debt, isolated pacing waits, one interactive limiter, native clocks | `6891f89`, `b361f02`, `ab6b485` | L2 presentation clock tests; L5 timing scenario | open |
| D2 | One visual frame per guest frame when the host falls behind | `b09e3cb` | L5 throttled scenario | open |
| D3 | Parallel presentation (render command stream, worker) on by default | `fc5408f`..`f5fff67` | L4 identity; L5 scene change, F1 toggle, resize and fullscreen under the worker | open |
| D4 | Fullscreen lifecycle (the alpha.2c fix) and display changes | alpha.2c, #3 | L5 fullscreen/windowed toggles | open |
| D5 | Temporal jitter phase counts frames that drew the scene; temporal history across graph invocations | `e550c18`, `b49fa74`, `4120c4e` | L4; L5 pause/unpause scenario | open |

### 4.E UI composition, HUD and TopScreen

| ID | Development | Evidence | Check | Status |
| --- | --- | --- | --- | --- |
| E1 | Native HUD keeps its aspect with exact UI composition ownership | `0b2d848` | L5 HUD scenario at 4:3, 16:9, 21:9 (#25) | open |
| E2 | Centered UI composition; minimap layout persists (and survives savestate reload, #32) | `1f5ef90`, issues doc 25/32/39 | L5 minimap marker scenario, fresh entry and reload | open |
| E3 | Visions UI ownership through shared primitive drawing | `d108659` | L5 Visions scenario (ultrawide and 4:3) | open |
| E4 | Consolidated F1 settings; F1 toggles from the key event (#51); Steam Input pads pass through | `a946f34`, `816fe91` | L3; L5 F1 tap scenario | open |
| E5 | TopScreen #40 stamina/timer overlap, #42 mounted B counter, #43 boots hotkey repeat (recovered after the 2026-09-17 reset) | `3f84692`, `71a654e` | L2 TopScreen profile tests; L5 Epona and boots scenarios | open |
| E6 | Game Over choices restored (#46): directional/A, save-yes, mouse/touch | `f89d3fb`, issues doc 45/46 | L5 Game Over scenarios (the last two never qualified) | open |
| E7 | Native UI kept out of the upscalers; TAA/SMAA resolved before native UI | `85c780f`, `cd6e550`, `d39aa14` | L4; L5 DLSS/FSR scenario: UI pixels at output resolution | open |
| E8 | F2 native lighting toggle and Navi fix | `e9dc221` | L5 | open |

### 4.F Fonts

| ID | Development | Evidence | Check | Status |
| --- | --- | --- | --- | --- |
| F1 | HD native QBF fonts in the TopScreen at 480p and above, coverage atlas, Forge preparation from the TopScreen archive (#44) | `3d38ef1`, `d28ba12`, `b58a83f`, `8f802cc` | L2 `test_qbf_font`, `verify_hd_fonts.py`; L5 dialogue scenario (acceptance list of the #44 document) | open |

### 4.G Movies

| ID | Development | Evidence | Check | Status |
| --- | --- | --- | --- | --- |
| G1 | Native movie decoder continuations and dispatch tables recovered in offline AOT; Sheikah Stone plays, completes or cancels and returns to gameplay | `7ee14a1`, `460960d`, `aa5f36d`, `522f185`, `1f5ef90` | L5 Sheikah Stone scenario | open |

### 4.H Input

| ID | Development | Evidence | Check | Status |
| --- | --- | --- | --- | --- |
| H1 | Shared SDL controller routing with physical device identity | `e7913b4` | L2 `three_ds_sdl_controller_tests`, input suites | open |
| H2 | Azahar motion and touch in the shared controller input; motion only for aiming; automatic motion aiming in the preset; stick + gyro aiming with mouse ownership | `46c8132`, `59cfc5c`, `c182ca1`, `e84ad86` | L2; L7 device checklist | open |
| H3 | Mouse capture policy | — | L2 `mouse_capture_policy_tests` | pass |

### 4.I TextureCopy and cutscenes (#45, #49)

| ID | Development | Evidence | Check | Status |
| --- | --- | --- | --- | --- |
| I1 | Ordered raw TextureCopy reaches the Sheik revelation; completion obligation survives a savestate | `0bcd22d`, `e9a14a7` | L2 visual savestate tests; L5 revelation scenario from the mid-flashback checkpoint (native 30 and interpolated 2x) | open |
| I2 | Raw copy readback cost cut; GPU encode; guest-access watch | `99e2005`, `4f58878`, `7dd875c`, `e91a360` | L2; L5 revelation timing | open |
| I3 | Copied textures decoded on the GPU (2026-09-23 work, 18-27 % of its lines in the line) | `0765e0c`, `602d10e`, `6cc19cc`, `c5b6bb0` | establish whether a later commit replaced it on purpose; L5 revelation | open |
| I4 | Canonical GPU-copy visibility preserved across title execution | `bcec0f6` | L5 revelation; L2 | open |

### 4.J Lighting, shadows, materials, AO, AA and upscalers

| ID | Development | Evidence | Check | Status |
| --- | --- | --- | --- | --- |
| J1 | Scene lit by native environment directionals; queued atmosphere appearance preserved; native brightness independent of light gains | `6e80d2e`, `6c07177`, `5b530d5`, `bc4f8c9`, `b0772ec` | L4; L5 day/night scenario | open |
| J2 | Key light from the scene's environment lights, its colour from the main light; continuous light under animated time; shadow keyframes follow a moving light | `9669831`, `d1691e8`, `0a3a357`, `5a83492` | L5 night/dawn scenario (`intro-lights` anchor) | open |
| J3 | Directional shadows: current sample's casters, alpha-tested casters keep their cutout; point lights cast cube shadows | `ab1b32a`, `e4930f7`, `d92fe95` | L4 `cfg-points`; L5 | pass (L4) |
| J4 | Grass VSM shadows, actor shadow cache | `e9dc221` | L4; L5 | open |
| J5 | Editable PBR packs and native UV seam softening | `c9138cb` | L2; L5 | open |
| J6 | AO providers: FidelityFX CACAO, multiscale GTAO; native AO normals and texture relief | `19a1296`, `5d59274`, `567281a` | L2 AO guide tests; L4; L5 | open |
| J7 | MXAO (iMMERSE module): temporal, depth/face normals, options, debug views | `7d2a889`, `d39aa14`, `72c9eea`, `93fcc1f`, `e543598`, `1c71763` | L2 `mxao_gpu_tests`; L4 `cfg-su-native` | pass (L4), release decision open |
| J8 | Upscaler output is the display; Ultra Performance renders 1/3; selectable DLSS model, the Enhanced preset uses the transformer | `af6529a`, `8abc0eb`, `7cc023f` | L2 `test_dlss_runtime_validation`; L5 | open |
| J9 | Scene changes no longer recreate the temporal upscaler | `2f53274` | L5 house-entry scenario: one upscaler instance per stream | open |
| J10 | Scene effects use the rendered sample camera; SSSR per surface; reflections analyse environment terms once | `db2ddd8`, `a025d4d` | L4 | open |
| J11 | Savestate restore keeps colour targets at the saved scale | `481290d` | L5 savestate scenario at a non-native scale | open |

### 4.K Grass

| ID | Development | Evidence | Check | Status |
| --- | --- | --- | --- | --- |
| K1 | Exact pooled spacing columns, visibility ordering, parallel visibility, stable selection, moving interaction cells | `80ee461`, `33d241f`, `d619428`, `48bd779`, `94bee2f` | L2 grass tests; L4 | open |
| K2 | Exact grass motion vectors | `a179c21` | L2 `grass_motion_gpu_tests`; L4 | pass (L4) |

### 4.L Audio

| ID | Development | Evidence | Check | Status |
| --- | --- | --- | --- | --- |
| AU1 | Deferred PCM and ADPCM buffers observed completely | `aada562` | L2 `oot3d_native_audio_tests` (needs ROM-derived fixtures, section 7); L7 | open |

### 4.N AOT title module

| ID | Development | Evidence | Check | Status |
| --- | --- | --- | --- | --- |
| N1 | Verified memory epochs and replay baseline preserved; contracts tested; synchronous AOT work attributed | `46a2731`, `ffccab0`, `8d891cf` | L2 AOT runtime suites | open |
| N2 | Host title helpers with exact single-page memory paths; inline host-float VFP; frequent SVC names without streams | `ad030db`, `b6ceda7`, `fca3e07` | L4 identity with the module that ships; module build options recorded by L6 | open |
| N3 | Opt-in memory-region pilot and rejected AOT experiments stay inactive | `df0d82f`, diagnostics of 2026-09-16/17 | L6 release build shows them off | open |

### 4.O Diagnostics and developer tools (must be inert in the product)

GPU gap trace (`5b0ee4a`), graphics settings schedule (`758e0a1`), no-present
throughput mode (`ae8d00c`), environment light trace (`e2f1ed4`), GI dumps
(`ef413b8`), AOT sampling and cohorts. Check: L4 identity with every developer
variable unset; L6 confirms none is enabled by the release configuration.

### 4.P Global illumination (experimental, excluded)

Brixelizer GI phases 1-5 and fixes (`e805e9b`..`7871fab`), the GI setting and
F1 control (`1a55980`), the screen-space GI M1-M8 (`ccda246`..`cf4d331`). Check:
L2 GI suites keep running in developer builds; L6 proves the release build
offers no GI (section 6). Pre-existing finding: with either GI mode on, a shared
pass samples an image in `GENERAL` layout with a `SHADER_READ_ONLY_OPTIMAL`
descriptor (validation `imageLayout-00344`, `None-09600`); not a release item
while GI is excluded.

### 4.Q Reorganization (behaviour-preserving)

SPIR-V binary packs, prunes of unused sources, topic splits of the window,
Vulkan backends, CTR host, TopScreen profile and audio, CMake area files,
unreferenced product code dropped (`86bb942`..`fc473e0`). Check: L4 identity
(gates p3* to p5m8), L1 builds, L2. Status: pass.

### 4.R Integrated contributions, not-adopted parts and lost work

No commit, pull request or recovery snapshot holds HUD/UI code that is missing
from the line: every HUD/UI commit since alpha.2c survives at 91-100 % and the
open pull requests were integrated as replacement implementations. HUD/UI work
reported lost is therefore either work that was never committed (a reset
before a commit) or a behaviour regression of code that is still present. The
L5 HUD/UI scenarios detect the second; the user's list of missing behaviours
drives the first.

| ID | Development | Evidence | Check | Status |
| --- | --- | --- | --- | --- |
| R1 | Product-mode TopScreen item getters, slot resolution and Items-page ZR/ZL assignment (#39) | PR #13, integrated as the item dispatch module (`7617c8a`, `TRIAEVUM_TOPSCREEN_PRODUCT_ITEM_DISPATCH.md`) | L2 `oot3d_top_screen_mod_profile_tests`; L5 S04 (assignment, bow ammunition, hookshot) | open |
| R2 | PR #13's press latch, not adopted: a ZL/ZR tap shorter than a guest refresh, or released before the game polls, can be lost; the document leaves it to the input owner | `TRIAEVUM_TOPSCREEN_PRODUCT_ITEM_DISPATCH.md` | L5 S04 with one-frame taps | open |
| R3 | Ocarina song guide and D-pad ownership in a dedicated title UI module (PR #18's evidence; its heuristics not adopted) | `TRIAEVUM_TOPSCREEN_OCARINA_OWNER.md` | L2 TopScreen ocarina tests; L5 S07 | open |
| R4 | Display-change rejection reason in the F1 display status (PR #14, `dd7539e`) | `TRIAEVUM_CONTRIBUTIONS.md` | L3 actual-widget lifecycle | open |
| R5 | Remainder of the 2026-09-28 / 2026-09-26 recovery snapshots | `88f935a` (13 % absent), `a33d1f6` (5 %) | the absent lines are the concurrent Azahar/AOT workstream (AOT redesign notes, SHA-256 and A32 runtime of the module) and renderer pieces of its uncommitted grass/VSM work (shadow history release, VSM page lookup, interop adapter): they belong to that workstream, not to this line | out of the line |

## 5. Scenarios (L5)

Each scenario names its input (savestate or save file, never distributed), the
recorded input, the profiles it runs under and its assertions. Inputs are
private and read only; outputs go to a scratch directory.

| ID | Scenario | Input | Covers |
| --- | --- | --- | --- |
| S01 | Boot, title, Start during the title presentation, file select, load a save | none (fresh boot) and a practice save | B5, C5, #48 |
| S02 | Kokiri walk, eight renderer profiles (the L4 gate) | `qualify/ref-kokiri2` | C, J, K, Q |
| S03 | HUD at 4:3, 16:9 and 21:9; minimap markers on fresh entry and after reload | practice saves with map/compass | E1, E2, #25, #32 |
| S04 | Pause menu pages, item numbers, map screen, ZL/ZR assignment and use, one-frame taps | practice saves (adult with bow/hookshot) | E2, R1, R2, #39 |
| S05 | Epona: stamina and timer, mounted B counter | issue 40-43 checkpoints | E5, #40, #42 |
| S06 | Boots hotkey repeat | `boots-held` checkpoint | E5, #43 |
| S07 | Ocarina performance UI, song browsing | practice save with ocarina | R3 |
| S08 | Game Over: continue/quit by directional/A, save-yes, mouse/touch | Game Over checkpoints | E6, #46 |
| S09 | Temple of Time revelation and the mid-flashback savestate, native 30 and interpolated 2x | issue 45/46 checkpoints | I1-I4, #45, #49 |
| S10 | Sheikah Stone movie: play, complete, cancel, return | Kokiri state near the stone | G1 |
| S11 | Dialogue with HD fonts at 480p and above, TopScreen on and off | practice saves | F1, #44 |
| S12 | F1 tap toggle, F2 native lighting, capability-safe Off controls | any gameplay state | E4, E8 |
| S13 | Fullscreen and windowed toggles, display change rejection | any | D4, R4 |
| S14 | DLSS and FSR profiles: UI at output resolution, house entry keeps one upscaler | `play-kokiri` style state at a house door | E7, J8, J9 |
| S15 | Night/dawn light and shadows | `intro-lights` night anchor | J2 |
| S16 | Savestate save/restore at a non-native scale | Kokiri | J11 |
| S17 | Cold and warm cache start: preparation, no runtime compilation, persisted pipelines | fresh cache directory | B1, B3-B5 |
| S18 | Slow host: one visual frame per guest frame | Kokiri with a throttled host | D2 |

## 6. Global illumination out of the release

Requirement: a release build offers no GI mode; the GI code stays in the tree
and in developer builds. Design (to implement): a build option for the
experimental GI paths, off in the release configuration; when off, both GI
capabilities report "experimental, not part of this build", the settings layer
normalizes any persisted GI mode to Off (it already does for an unavailable
capability) and F1 shows the capability-safe Off control with that reason. The
release audit checks the option and the default configuration.

## 7. Known red items before the battery's first run

From the last full host-test run of this line (2026-10-04, M8 gate): 88 of 96
test executables pass.

| Item | Observation | Next |
| --- | --- | --- |
| `sdl_video_driver_policy_tests` | crashes (exit 139) | investigate |
| `three_ds_recomp_tests` | `EventSystem.CallEventUpdatesCallerMetadata`, `CallEventIncrementsCaller` fail | investigate |
| `triaevum_native_module_loader_tests` | exit 1 | investigate |
| `oot3d_toon_outline_gpu_tests` | `IndexedVertexDecoderMatchesExpandedStreamForEveryTopology`: Vulkan failure -13 in the shared compute fixture (pre-existing on this host) | investigate on this host |
| `oot3d_native_a32_process_tests` | asserts the interpreter dispatcher's registry, which whole-AOT product mode does not use (reconstruction section 4) | make it configuration-aware or retire the assertion |
| `oot3d_native_audio_tests` | needs ROM-derived fixtures | point it at the private extracted inputs |
| `oot3d_native_mass_aot_tests`, `oot3d_native_pica_frontend_tests` | not built in this configuration | build in the battery's configuration or record why not |
