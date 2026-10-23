---
name: game-capture
description: Capture and inspect TriAevum framebuffer evidence without desktop screenshots or ROM-derived artifacts in the repository.
license: GPL-3.0-only
compatibility: opencode
metadata:
  audience: game-tester
  workflow: runtime-verification
---
## Capture

The runtime writes the framebuffer directly:

```text
--screenshot FILE.bmp
--screenshot-sequence
--screenshot-start-frame N
--screenshot-interval N
```

The harness uses one final framebuffer capture by default and converts it to
PNG when Pillow is available:

```text
artifacts/game-tests/<run>/final.bmp
artifacts/game-tests/<run>/final.png
```

## Inspect

Use the `read` tool on the PNG. Report what is visible:

- expected scene and UI;
- Link position/state;
- P2 dog presence, position, animation, color;
- HUD, dialogue, transition, or error overlays;
- whether the screenshot matches the scenario expectation.

Do not infer visual success from `exit_code == 0`. Do not commit screenshots or
publish ROM-derived frames.

## Optional deterministic comparison

If a private reference image is available outside the repository, compare
dimensions and a perceptual/pixel difference with a local script. A reference
image is never committed. If no reference exists, use explicit visual
observations and the runtime probe.
