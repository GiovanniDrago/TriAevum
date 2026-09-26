# Surface lighting ownership and brightness preservation

## Contract

Per-pixel lighting replaces surface illumination, not the authored appearance of
sky, backgrounds, atmosphere or UI. Native, NativeMemory, Authentic and F2 remain
canonical. Native gameplay/environment updates continue; disabling native light
contributions is a presentation decision, not a guest-state mutation.

The implementation separates:

- native appearance and native alpha/coverage;
- material reflectance and baked vertex/texture tint;
- a scene-brightness reference independent of enabled native direct sources;
- effective direct illumination, shadows and ambient occlusion.

Classification comes from typed native composition ownership. It must not use
texture names, material hashes, screen position, draw distance or scene-specific
corrections. Distant world geometry remains a surface, not a background exemption.
Unknown ownership must not be silently promoted to a relightable surface.

## Implementation sequence

1. Carry composition-layer ownership into shader admission. Preserve atmospheric
   appearance instead of applying the world surface-lighting replacement. Verify
   shared-program cache transitions between world and atmosphere.
2. Add an explicit brightness-reference policy to the scene Environment owner.
   Native source gains may all be zero without discarding the reference. Resolve
   the low-frequency lighting budget on the CPU, not with framebuffer readback or
   camera-dependent automatic exposure. Version and document persistence.
3. Use the same resolved lighting inputs for Per-pixel, PBR and Grass. Keep
   reflectance application single-owned, specialize inactive source work, and
   avoid repeated preparation/upload of unchanged illumination.
4. Qualify with deterministic scene captures, native regression captures,
   production GPU lighting probes, real F1 widgets and structural diagnostics.

Each implementation increment records its verification here and is committed
separately. Pre-existing worktree changes are excluded from these commits.

## Brightness interpretation

Preservation means maintaining the environment's reference light level and
palette while allowing new light directions and shadows. It cannot promise
pixel-identical world shading after replacing those directions. Sky/background
appearance has a stricter invariant: changing surface-light settings must not
change it at the same atmosphere state and presentation settings.

Do not multiply already-lit native RGB by another complete lighting result, add
an arbitrary minimum brightness, bake modified textures, or use global exposure
to conceal a missing sky contribution. Authored dark channels remain dark.

## Acceptance and evidence

- Original direct-light gains zero: sky/background retain their authored color.
- Brightness reference remains usable without evaluating original direct lights.
- Key/fill, dynamic point groups and material controls remain independent.
- Atmosphere, alpha, depth/stencil, native draw order and UI remain protected.
- Native/F2 and native-memory outputs remain unchanged.
- Room/time/environment transitions update the reference without camera pumping.
- Performance claims require steady-state measurements, not capture wall time.

Initial read-only investigation: `I:/_triaevum_work/lighting-rationalization/`.
The archived Kokiri traces contain 17 atmospheric draws in the Scene domain;
world-lighting admission previously tested only that domain. A fixed sky region
in the archived Native capture has mean linear luminance 0.744; the authored-key
capture has zero there while its terrain receives light. The Native capture is
at a different frame, so this is diagnostic evidence, not an exact parity test.

## Development record

2026-09-26: implementation scoped; baseline worktree snapshots retained privately
for selective commits. No implementation or new runtime qualification is claimed
by this initial record.

### Appearance increment

Surface replacement now requires an explicit OpaqueWorld or TransparentWorld
owner, both when preparing lighting and when resolving the pipeline variant.
Atmosphere and unknown ownership keep their native appearance.

The first runtime probe exposed a second, upstream defect: the configured sky
producer queues models rather than emitting their CMB packets immediately.
Native CMB pass 0/1 alone consequently mislabeled those later packets as world.
The title adapter now carries the producer attribution on the exact eight-byte
native queue slot. It observes `0031487C` (model in r2, slot in r1); the native
`002FAD2C`, `002FAE10` and `003FE374` consumers identify that slot with r8+r4*8
at their CMB calls. These registers were verified against the native ARM code
and the read-only reconstructed producer. Model identity is deliberately not
the key: another queue can submit the same model without inheriting its role.
Slot replacement and a new gameplay frame invalidate old ownership.

Verification:

- 42 shader-pipeline/instrumentation tests passed, including world/atmosphere/
  unknown admission using the same program contract.
- Native composition regression passed, including deferred opaque/transparent
  consumption, the same renderer in another slot, slot overwrite and frame reuse.
- Deterministic Kokiri source sample 111550, host frame 30, 1280x720:
  sky ROI `(620,5)-(780,30)` changed from black to the exact Native RGB values
  with every native light gain zero and the authored key enabled. All 4,000
  ROI pixels match; mean linear luminance is 0.7385199023.
- Native framebuffer SHA-256 remains
  `a12f2ecfc6bc30d7e5f2d78be74b309f3f0d92b5c177745edb6a79a836ac3f89`
  before and after both the admission and deferred-ownership changes.
- These are renderer framebuffer readbacks, not desktop screenshots. Grass
  remained enabled in both captures. World brightness is intentionally not
  claimed fixed by this increment; its independent reference is the next step.

### Independent brightness reference

F1 Environment now owns **Preserve scene brightness**. It retains the native
ambient/material reference independently of all three native source gains.
Authored unlit primary color also remains a base appearance, not a light gain.
Added ambient, key/fill and point lights remain additive and independently
controllable. This deliberately preserves a baseline, not total irradiance:
there is no image histogram, guessed light direction, automatic exposure,
arbitrary minimum RGB or promise of identical world pixels.

The CPU resolves the direction-independent ambient contribution once per
surface input. With preservation enabled, the old ambient-light loop gain is
zero; the retained reference travels in the shared environment ambient input
used by Per-pixel, PBR and the Grass surface-lighting atlas. No original direct
source is required to recover it. Disabling preservation restores authored-only
lighting, including intentional black.

Environment schema 4 persists `preserveSceneBrightness`. New profiles enable
it. Versions 1–3 keep their authored-only choices and are not rewritten on load.
The native ambient slider is inactive while the reference owns that baseline;
its saved gain is retained. Native/Native+PBR, Authentic and F2 continue to bypass
the authored profile.

Verification:

- 24 production GPU source/reference cases pass, including all native gains
  zero, retained unlit appearance, authored-light response on that appearance,
  dark material channels, updated native ambient and disabled point groups.
- Production PBR GPU probes pass for triangle lists, indexed strips, alpha/base
  invariance, tangent frames, normal/ORM/emission/height and seam boundaries.
- 42 pipeline/instrumentation tests and 6,648 real F1 widget assertions pass.
- At sample 111550, the fixed `(0,0)-(800,650)` framebuffer region measures
  mean linear luminance 0.302808 Native versus 0.304710 with native gains,
  authored ambient and key/fill all zero and the reference enabled. With the
  authored key enabled it measures 0.322381. These regions are manual probes,
  not semantic world masks or a claim of brightness parity in every scene.
- Sky remains exact in both reference captures. All runs use private profiles;
  textures, material packs, active profiles and the running user session are
  untouched.
