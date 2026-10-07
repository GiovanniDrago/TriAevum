# Placed scene lights (F1 > Lighting > Lights & Shadows > Scene lights)

2026-10-07. Lights a user adds to a scene from F1, beside the scene
environment's key and fill lights. They belong to the scene's environment
profile, so a scene with a night profile keeps separate lights by night
(creating the night profile copies them).

## Editing

**Add light at Link** creates a light where Link stands, 50 units above his
feet, facing his direction and tilted 30 degrees down; **Move to Link** and
**Face Link's direction** bring an existing light back. The position (world
units, Y up) and the rotation can then be set freely. At most 16 lights per
profile. The controls follow the type:

| Type | Position | Rotation | Shape and size | Shadows |
|---|---|---|---|---|
| Sun | no | yaw, pitch (the light's direction) | none | never (the key light keeps the directional shadows) |
| Spot | yes | yaw, pitch (the beam) | reach, beam angle, beam softness | yes |
| Point | yes | no | reach | yes |
| Surface | yes | yaw, pitch, roll (the emitting face) | rectangle (width, height) or disc (radius), one or both sides, reach | yes |
| Omni | yes | no | reach | yes |

Every light has a name, visibility, colour (linear RGB) and intensity (0 to 16).
Yaw 0 faces +Z and 90 faces +X (the game's own yaw); pitch 90 points up. Edits
preview live; **Save scene environment** keeps them, **Reload** discards them.
They apply while **Use scene environment** applies (Modern lighting, F2 off).

## Rendering, by step

1. **Now.** Point and omni lights are drawn through the scene point-light term
   the renderer already has for Navi and the fairies (`1 - d^2/r^2` falloff
   over the reach), after the native scene points in the same per-draw slots
   (32 in all). A light with **Cast shadows** competes for a point-shadow cube
   map (Point shadows: Cube maps; 8 rows shared with Navi and the fairies).
   Sun, spot and surface lights are kept, edited and saved, not drawn yet; the
   panel says so. Point currently uses the omni falloff.
2. **Next.** Shader terms for the physical point falloff (inverse square with
   a smooth cut at the reach), spot cones, surface lights (rectangle and disc,
   one or two sides) and unshadowed suns. Changing the surface-lighting shader
   changes every lit pipeline, so the precompiled shader pack
   (`expanded-union.o3ps`) has to be regenerated with it.
3. **Then.** Shadows for spot and surface lights.

## Storage

`placedLights` in the profile JSON (`environments/scene-XXXX[.night].environment.json`),
optional within version 5, so runtimes that predate it ignore the lights:

```json
"placedLights": [
  {"id": 1, "name": "Light 1", "type": "omni", "visible": true,
   "color": [1.0, 0.8, 0.6], "intensity": 1.5,
   "position": [-504.0, 430.0, -1224.0], "rotation": [90.0, -30.0, 0.0],
   "castShadows": true, "radius": 300.0, "width": 100.0, "height": 100.0,
   "shape": "rectangle", "twoSided": false, "beamAngle": 45.0,
   "beamSoftness": 0.2, "range": 300.0}
]
```

`type` is `sun`, `spot`, `point`, `surface` or `omni`; `shape` is `rectangle`
or `disc`. `id` is unique in the profile and names the light's shadow row.
Every field is required in an entry and range-checked; an invalid entry makes
the profile invalid, as any other out-of-range value does.

## Verification

`triaevum_f1_settings_smoke`: a light added at Link's placement lands 50 units
above him with his yaw and is drawn as a point light; spot is not drawn and
shows its beam controls; surface shows its shape, size and sides; sun offers no
shadows; omni with shadows publishes a shadowing light; save writes
`placedLights`; duplicate gives a new identity; delete stops drawing; reload
restores the saved light.
