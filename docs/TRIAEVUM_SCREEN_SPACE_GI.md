# Screen-space diffuse GI (clean-room implementation)

Status: **M1 done** (radiometric contract, deterministic angular reference,
T01-T04). Nothing is wired into the renderer yet: no setting, no graph pass, no
shader. Design input is only the written specification "Specifica per una GI
diffusa in screen space" v1.2 (2026-09-30), its cited public sources and the
synthetic scenes in the tests. Milestones follow the specification: M1 adapter
and angular integrator; M2 gathering and masks (T05-T09); M3 accumulation and
filter (T11-T16); M4 cache and guiding (after T10); M5 artistic integration.

## M1 deliverables

| File | Content |
|---|---|
| `runtime/three_ds_recomp/include/fast/renderer/diffuse_gi_reference.h`, `src/fast/renderer/diffuse_gi_reference.cpp` | Title-neutral, Vulkan-free: input declaration and validation, radiometric helpers, section geometry, analytic sector weights (double and FP32), Gauss-Legendre reference quadrature, cumulative C, synthetic angular fields, sector estimator (double and FP32), continuous reference integrator |
| `runtime/three_ds_recomp/tests/renderer_diffuse_gi_reference_tests.cpp` | T01-T04 and their support tests; prints `DGI_REPORT` lines |
| `runtime/three_ds_recomp/tests/CMakeLists.txt` | Test target `renderer_diffuse_gi_reference_tests` |

The module belongs to the generic renderer layer (no 3DS, PICA or OOT3D
vocabulary). It is compiled only into its test target: `renderer/*.cpp` is an
explicit list, so the game build is unchanged until M2 adds it to
`Source_Files__RendererExtensions`.

## Conventions and parameters (decisions)

- **Measure.** v is the unit vector from the receiver to the camera;
  `SectionFrame::FromView` completes it deterministically (e1 = normalize(h x v)
  with h = X unless |v.x| >= 0.9, then Y; e2 = v x e1). phi in [0, pi), theta in
  [-pi, pi), omega = v cos theta + t(phi) sin theta, d omega = |sin theta| d theta d phi.
- **Weights.** w_j(phi) = (1/pi) integral over the sector of max(0, n.omega) |sin theta|.
  With a = n.v and b = n.t(phi), n.omega = a cos theta + b sin theta. No source
  cosine and no 1/r^2: the receiver cosine is already the solid-angle weight.
- **Own closed forms** (verified against the quadrature, see the report):
  - per piece where sin theta has one sign s and the lobe is positive:
    integral_p^q (a cos t + b sin t) sin t dt = 1/2 [b (D - sin D cos S) + a sin D sin S],
    D = q - p, S = q + p, evaluated as b ((D - sin D) + 2 sin D sin^2(S/2)) + a sin D sin S
    with a Taylor series for D - sin D on small D (no cancellation in FP32);
  - the lobe is [g - pi/2, g + pi/2], g = atan2(b, a), wrapped into [-pi, pi];
  - section mass M_phi = (r/pi)(cos g' + g' sin g'), r = |(a, b)|,
    g' = min(|g|, pi - |g|). M_phi = 0 only when n is normal to the section plane;
    then every contribution is zero and nothing is divided.
- **Sectors** are uniform in theta over [-pi, pi) (baseline); the count N is
  independent of the section count S. A sector takes the field at its centre
  direction (deterministic centre rule). The continuous reference integrates
  the same section without sectors to measure quantization.
- **Estimator.** uniform p = 1/pi, phi_s = pi (s + u)/S with a declared rotation
  u in [0, 1) (u = 0.5 in the deterministic mode; per-pixel/per-frame u is the
  production randomization, from M2). J is never renormalized by the lit or
  visible fraction; open directions take the declared fallback (zero: no local
  GI, any environment enters separately).
- **Outputs.** J raw (never clamped); A raw (can exceed 1 for few sections:
  a grazing normal gives up to pi/2 for one section) and A = clamp(A raw, 0, 1);
  C = valid / scheduled candidates (unobservable directions count as open for A,
  lower C only: optimistic AO fallback, diagnostic only).
- **Inputs are declared, never inferred.** `InputDeclaration` +
  `ValidateInputDeclaration` reject an unknown depth encoding, forward axis,
  motion direction/units/jitter, radiance profile or encoding, receiver albedo
  encoding, exposure, a geometric normal outside view space, a source that is
  not outgoing radiance, or indistinguishable sky/unavailable pixels.
  `LdrApproximation` is a separate profile and is never calibrated radiance.
- **Radiometry.** `SourceRadianceFromIrradiance` = rho_s E / pi, applied once by
  the adapter; the source albedo is never an input of the transport.
  `ComposeIndirect` = rho_d J, applied once; `ComposeNative` implements
  C_out = L_direct,d + L_spec + L_e + A L_ambient,d + rho_d J_local.

## TriAevum adapter contract (what the renderer provides before UI)

GI runs in the display graph at the native pre-UI boundary, like AO, before
TAA/SMAA/upscalers, scanout and native UI. There is no bloom or tone mapping:
the PICA scene colour is display-encoded RGBA8. Buffers below are at the guide
target extent (`effectGuideTarget`, twice the presented width for OoT3D's top
screen) and in physical PICA storage orientation.

| Contract item | TriAevum source | Declared convention | Gap |
|---|---|---|---|
| Depth and camera | `NativeDepth` (D32_SFLOAT preferred), `NativeSceneView` / `PerspectiveViewState` (Projection, WorldToClip, Eye, At, Near, Far), `TemporalViewState` (current/previous WorldToClip) | Perspective window depth 0 at near, 1 at far (guest D3D form, w = -z), view forward -Z; W-buffer targets (`WBuffering`) are linear n + d (f - n). Positions from storage UV via `PicaSurfaceCoordinates::StorageToViewNdc` and the projection, as GTAO does | Scene units are native OoT3D world units; their metric scale is not established. R and tau are declared in those units |
| Geometric normal | Not stored | Reconstruct from depth (view space) in M2 | Required: depth-derived geometric normal |
| Shading normal | `NormalGuide` RGBA8, 0.5 n + 0.5, view space (unrotated camera basis), alpha coverage; PBR mapped / authored / quaternion / derivative normal | Not used for integration in the first version | Policy and tests for below-surface directions before use |
| Source radiance | `DiffuseAlbedoGuide` RGBA16 UNORM, low byte of each channel: sRGB of albedo x per-pixel direct light (shadowed native directionals, key, local lights, Navi/fairies); per-pixel lighting modes only | Outgoing radiance including the source albedo, display-encoded, clipped at 1: **LdrApproximation** | Native HDR profile needs a linear float direct-radiance output (e.g. RGBA16F) written by the same per-pixel lighting before its clamp |
| Receiver albedo | Same guide, high byte (TEV with unit lighting: texture x material) | sRGB 8-bit | Textures may carry baked light (see compatibility path) |
| Motion | `MotionVectors` RGBA16F: rg = previous UV - current UV in storage UV of the same target; b = disocclusion; rigid/grass object motion from `RigidMotionGuide` | CurrentToPrevious, NormalizedViewportUv, jitter Included (both UVs jittered) | Previous surface position only through motion; validation of history is weaker than the specification's ideal (M3) |
| Validity | Albedo-guide alpha = validity x overlay transmission (0 on clear, HUD, replacing surfaces); depth >= 1 = no geometry | Sky = clear/no depth; unavailable = alpha 0 | Two-sided surfaces are not marked |
| Frame state | `mFrameCounter`, `TemporalViewState` (jitter, `CameraCut`, `ResetReason`, history epoch), world identity resets, `SceneSurfaceKey` per composed surface, guide extent | Exposure: none (fixed) | Interpolated presentations (Interpolated2x) need per-sample history like Brixelizer GI |

**Baked lighting (compatibility path, section 8).** OoT3D bakes fill and
ambient light into vertex colours and textures; native fragment ambient is
almost zero in the forest. The native colour cannot be split exactly into
albedo, direct and baked light. The declared compatibility path is therefore:
source = the per-pixel *direct* radiance only (never the native colour, never
ambient or fill), receiver albedo = TEV with unit lighting, and
`C_out = native colour + rho_d J` (additive local bounce, A optionally on the
native ambient-like part only through an explicit, labelled artistic control).
It exists only in Per-pixel and Per-pixel + PBR lighting; Native, Authentic and
F2 keep GI off. Any reduction of existing light is exposed as an artistic
control, never hidden in the estimator.

## Integration plan (M2-M5, not implemented)

- **Graph.** A new provider behind the existing `GlobalIllumination` display
  pass (stage `AfterOpaque`, after `Guides` and `Motion`, before `Composite`),
  reading `NativeSceneView`, `NativeDepth`, `NormalGuide`,
  `DiffuseAlbedoGuide`, `MotionVectors`, writing `DiffuseIllumination`
  (J, RGBA16F; the composite already multiplies by the albedo guide once).
  New transient resources for A, C and raw J (diagnostics) and two persistent
  history resources (J and moments) per composed surface, owned like other
  temporal histories. Optional effects stay inert when Off: no guide MRT, no
  pass, no allocation.
- **Settings owner.** `Graphics.Effects.GlobalIllumination.Mode` gains a
  screen-space value next to `BrixelizerGi` (one owner: the graphics settings
  runtime; one F1 control in Lighting > Global illumination, capability-safe
  Off, actual-widget smoke). Separate controls for sections, radial candidates,
  sectors, R, tau (scene units), temporal and filter; validation mode disables
  gain, highlight clamp and fade.
- **Shaders.** Runtime-compiled through the pass shader cache during M2-M3
  (fast iteration, CPU reference in this module, GPU tests in
  `tools/renderer/tev_program` like `gtao_gpu_tests`); precompiled into
  `builtin_pass_binaries.h` (`build_pass_artifacts`, `--check`) at M5.

## Implementation manifest (section 14)

| Component | Public source (consulted 2026-10-01) | Used | Own derivation | Rejected alternatives |
|---|---|---|---|---|
| Section parameterization, measure, weights, estimator | Specification sections 2, 3, 12 (formulas 1-5) | Definitions of J, A, omega, d omega, w_j, F, J hat | Closed form of the piecewise integral, lobe wrapping, M_phi closed form, FP32-stable form | Unsplit Gauss-Legendre across kinks (2.7e-4 error at order 256, not converged); hemisphere-slice sectors with 1/N weights and a source cosine as in [B1] Algorithm 1 (the specification uses the solid-angle measure and no source cosine) |
| Sector visibility family | [B1] Therrien, Levesque, Gilet, arXiv:2301.11376v2 (HTML), sections 3.1, 3.3, 4.3, Algorithm 1 | Principle only: N sectors per slice, thickness intervals, newly occupied sectors (M2) | Full-circle theta sectors of the specification | None imported |
| Slice integration of AO | [B2] Jimenez et al., ATVI-TR-16-01 | Not retrieved: the URL reset the connection twice on 2026-10-01; no equation taken from it | All slice formulas above derived independently | - |
| Gauss-Legendre rule | Standard numerical analysis (Legendre recurrence, Newton) | Nodes/weights computed at run time in double | - | Tabulated nodes |
| Synthetic scenes | Own: caps, half-spaces, view band | Known integrals: cap about n: sin^2 alpha; cap at angle beta above the horizon: sin^2 alpha cos beta; half-space: (1 + cos beta)/2; band: sin^2 theta2 - sin^2 theta1 | Section breakpoints of a cap: theta = delta +- acos(cos alpha / R) | - |

No code, table, noise or asset of any implementation was consulted or
imported; no shader source of any third-party AO/GI implementation was read.

## Reproducible report (M1)

```text
I:\_triaevum_work\perf-levers\build-host.cmd -j 4 renderer_diffuse_gi_reference_tests
I:\_triaevum_work\perf-levers\host-build\three_ds_recomp_runtime\tests\renderer_diffuse_gi_reference_tests.exe
```

clang-cl 22.1.6 Release, Windows 11, 2026-10-01: 7/7 tests pass in under 1 s.
Values from the `DGI_REPORT` lines:

- **Weights** (9,472 sector cases, 8 normal tilts x 37 sections x 32 sectors):
  analytic double vs reference quadrature max abs error 2.8e-17; quadrature
  order needed at most 16 (tolerance 1e-14 relative / 1e-17 absolute, orders
  doubled from 1 to 256, pieces split at theta = 0 and the lobe edges); FP32
  analytic max abs error 2.3e-8; M_phi closed form vs sector sum 1.7e-16.
  Integral over phi of all sector weights: 1 within 6.4e-15 (7 normals incl.
  back-facing and grazing).
- **T01** (constant RGB (0.25, 1, 3.5), N = 32): normal along v exact for one
  section (< 1e-12); max relative error of J and A by section count S:

  | tilt | S=1 | S=2 | S=4 | S=8 | S=16 | S=64 |
  |---|---|---|---|---|---|---|
  | 30 deg | 1.9e-2 | 2.9e-3 | 5.2e-6 | 1.7e-11 | 2.2e-16 | 2.2e-16 |
  | 60 deg | 3.1e-2 | 4.4e-2 | 1.5e-3 | 2.2e-6 | 7.1e-10 | 2.5e-16 |
  | 85 deg | 8.6e-3 | 1.4e-1 | 2.0e-2 | 1.3e-3 | 4.2e-4 | 1.3e-9 |
  | 120 deg | 3.1e-2 | 4.4e-2 | 1.5e-3 | 2.2e-6 | 7.1e-10 | 2.5e-16 |

  Mean of 4,096 single-section estimates with golden-ratio rotations: at most
  2.8e-5 (single estimates deviate up to 0.91, as allowed). Continuous
  reference: at most 3.5e-14.
- **T02** (light (2, 1, 0.5)): continuous reference error at most 2.1e-7
  (caps of 15 deg and tilted 20 deg; 8,192 outer panels, last correction
  5.7e-7), 1e-15 elsewhere. Sector estimator with S = 128, max relative error:
  15 deg cap 7.6e-2 (N = 32), 2.3e-2 (64), 2.0e-3 (128), 9.4e-3 (256);
  half-space at 170 deg (J = 0.0076) 2.0e-1, 1.2e-2, 2.5e-2, 7.4e-3; all other
  regions below 1e-2 from N = 64. Sector-aligned band about v: exact for a
  single section (8.2e-16). A 45 deg cap gives J = 0.5 L (no renormalization).
- **T03**: after `Reset()`, zero source and zero fallback give J = 0 exactly and
  finite A and C, including a grazing normal with zero-mass sections; source
  albedo at fixed radiance leaves J bit-identical; rho_s E fixed gives the same
  J within 1e-15; rho_d acts once (doubling it doubles L_ind exactly).
- **T04** (FP32; sun cap 5e4 + sky + ground, N = 32, S = 32): J raw
  (167.9, 134.4, 101.2), no clamp; FP32 vs double 1.0e-6; scaling by 2, 8, 3 and
  1000 preserves the proportion within 2.9e-7; at 1e30 and 1e-30 the x8
  proportion is exact; no non-finite value.

Quantization and normalization limits recorded for M2: with the centre rule
the sector error is not monotone in N for small or grazing regions (it depends
on where sector centres fall), so production must choose N with scenes of the
expected angular size, or use the continuous intervals of the occupied
segments (planned for the M2 masks).

## Open points (as requirements or tests)

1. The adapter must provide a linear, unclamped direct-radiance output for the
   native HDR profile; until then only `LdrApproximation` is declared.
2. The metric scale of native OoT3D units must be measured (test: known
   geometry such as a door height reconstructed from depth) before R and tau
   defaults are published.
3. A depth-derived geometric normal must reject self-intersections without
   bias on flat surfaces with RGBA8 guides (M2 test on a sloped plane).
4. Sources and receivers must stay absent outside Per-pixel lighting modes, and
   GI Off must stay byte-identical (deterministic capture, M2/M5).
