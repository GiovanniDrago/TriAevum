# Scene environment profiles: tooling for agents

Scene environment profiles (`environments/scene-XXXX.environment.json`, and
`scene-XXXX.night.environment.json` for a scene's night) are edited by people
in F1 and by agents as files. Both go through one codec,
`oot3d_scene_environment_codec.{h,cpp}`, used by the runtime, the F1 editor and
the tool below; the shape and ranges are documented in
`docs/schemas/triaevum.scene-environment.v5.schema.json`.

## triaevum_environment_tool

Developer builds (`TRIAEVUM_DEVELOPER_TOOLS`), target
`triaevum_environment_tool` (not built by default). It never starts the game.

```text
triaevum_environment_tool validate <file>...
triaevum_environment_tool format [--check] <file>...
triaevum_environment_tool explain <file>
```

- `validate` prints one JSON line per file:
  `{"file", "valid", "sceneId", "timeOfDay", "errors": [{"pointer", "message"}]}`.
  Every value is checked and every problem reported at its JSON pointer
  (`/placedLights/1/intensity: is 30; must be a number within 0..16`); a
  file named `scene-XXXX[.night].environment.json` must name that scene and
  time of day. Light ids are whole numbers 1..4294967295, unique in the
  profile.
- `format` rewrites a valid file in the runtime's canonical form (the bytes
  F1 saves: two-space indentation, numbers in their shortest exact form, a
  final newline); `--check` only reports files that are not canonical.
- `explain` prints the authored profile, what the renderer receives (key and
  fill directions toward their sources, ambient, native strengths, each
  visible placed light with its emission axis, colour times intensity, reach,
  beam or extents), what depends on the scene at run time (a key light that
  follows the scene's main light takes its direction, colour and brightness
  there) and notes: hidden lights, lights without shadows that light through
  walls, the eight point-shadow cube maps shared with Navi and the fairies,
  the 16 placed lights per scene, units and angle conventions.

Exit status: 0 success, 1 an invalid or non-canonical file, 2 usage.

## Hot reload

`TRIAEVUM_ENVIRONMENT_HOT_RELOAD=1` (developer builds) makes the running game
re-read the current scene's profile files when they change on disk, looking at
most every 250 ms (size and time of both the day and the night file). Each
outcome is a stderr line:

```text
TRIAEVUM_ENVIRONMENT_RELOAD scene=0x0055 time=day result=applied file=scene-0055.environment.json
```

- `applied`: the file validated and is now the profile in use.
- `kept-invalid`: it did not validate (`detail=` gives the first error); the
  profile in use stays, so a half-written file never reaches the renderer.
- `kept-missing`: the file is gone; the profile in use stays.
- `kept-unsaved`: F1 has unsaved edits; they are never replaced (save or
  reload in F1).

Creating or removing the night file switches which file the night uses.
The game's own saves are not picked up as changes.

## Identity

The renderer identifies a placed light by tag, time of day, scene and the
whole 32-bit light id (`PlacedLightIdentity`), so day and night lights and high
ids never share point-shadow history.

## Verification

- `triaevum_f1_settings_smoke` (9365 assertions): canonical text and exact
  round trip, a night profile refused as a day one, identities distinct across
  time of day, scene and the high bit, every error of a broken profile at its
  pointer (fractional id, out-of-range intensity, wrong-length colour,
  missing field, repeated id), `explain` showing the renderer's placed light
  and the scene-following key.
- The tool on the 26 profiles of the play configuration: all valid.
- Live, developer build, free camera above Kokiri Forest with hot reload: an
  invalid write is `kept-invalid`, the valid write 1.5 s later is `applied`
  and the green omni light turns red in the next frames (right third of the
  frame, mean RGB 89/126/22 -> 108/109/24), gizmo included.
