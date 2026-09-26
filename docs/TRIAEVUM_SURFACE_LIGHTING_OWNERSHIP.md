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
