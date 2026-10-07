# Developer warp menu (F1 > Dev Tools)

2026-10-07. Developer builds only: F1 > **Dev Tools** reproduces Ship of
Harkinian's Dev Tools > Warping column over OOT3D, so a developer can jump
between scenes, entrances and saved places.

## Build gate

`TRIAEVUM_DEVELOPER_TOOLS` (CMake option, default OFF) compiles the tab and its
runtime into `oot3d_native_game` (`TRIAEVUM_DEVELOPER_TOOLS=1`). Without it
nothing of the feature is compiled. `--product-info` reports
`capabilities.developer_tools`; `prepare_release.py` refuses a runtime that has
it and `audit_release.py` rejects a runtime contract that does not exclude it
(`test_release_audit.py` covers true and missing). A developer configures with
`-DTRIAEVUM_DEVELOPER_TOOLS=ON`.

## What mirrors SoH

| SoH (Dev Tools > Warping) | TriAevum F1 > Dev Tools |
|---|---|
| Better Debug Warp Screen (default on) | same checkbox: the Better list (scenes, then entrances), Child/Adult, Day/Night; off: the vanilla scene list, current time kept |
| Debug Warp Screen Translation | same checkbox: German or French names when the game runs in that language (the F1 font has no Japanese) |
| Debug Warp Screen lists (`sScenes`, `sBetterScenes`, `sBetterGrottos`) | the same lists and names, generated from SoH's `z_select.c` |
| Grotto entries (`Select_Grotto_LoadGame`) | listed, disabled: they need OOT3D's respawn fields (below) |
| Warp Points: name + "+", Warp, Boot, Delete | same; points stored with SoH's fields (`entranceId`, `roomNum`, `pos`, `rotY`, `bootToPoint`, `useSceneEntrance`) |
| Warp point arrival at the saved place (void-out respawn at `respawn[DOWN]`, damage skipped) | arrival at the point's entrance and scene; room and position not yet (below) |
| Boot to warp point (Boot Sequence) | the boot point is warped to when gameplay first starts in a session: OOT3D needs a loaded file, there is no debug save |

Not offered: the Master Quest toggle (OOT3D's Master Quest is a save-wide mode
with a mirrored world, not a per-scene dungeon swap) and SoH's in-game Debug
Warp Screen game state and its button combination (the request is an F1 menu).

## How a warp runs

`DevWarpRuntime` (`oot3d_dev_warp.{h,cpp}`) never builds a scene. At the guest
frame boundary where `NativeScenarioBootstrap` advances, during normal gameplay
(game mode 0, no title/demo cutscene index, the TopScreen free-camera
admission) and with the PlayState transition slot free, it sets the age the
native way, PlayState `linkAgeOnLoad` (`+0x5C00`: the N64 tail maps onto
OOT3D's at -0xC1E8, checked on five fields; the game applies it when the
scene ends and swaps the age equipment, like SoH's SwitchAge), and the time of
day at SaveContext `+0x0C` (noon or midnight; the scene derives night from it,
`+0x10` reads 1 afterwards). Writing the save age under a live player faults
the title. It then calls code.bin's own transition request
(`FUN_003348E8`, trigger 20) and effect preparation (`FUN_0035DA3C`), the
structural scenario contract (`oot3d_dev_warp_process.cpp`). Arrival is the
requested scene in the PlayState after the transition slot was seen busy.

## Catalog and validation

`tools/oot3d/dev_warp/generate_dev_warp_catalog.py <soh root> <out.inc>`
generates `oot3d_dev_warp_catalog.inc` from SoH's `z_select.c`,
`entrance_table.h` and `scene_table.h` (their SHA-256 are in the file header):
125 vanilla scenes, 50 Better scenes with 300 entrances, the grottos. Each
entrance carries the scene and spawn SoH expects.

At the first gameplay frame the runtime reads OOT3D's global entrance table
(`0x00543BB8`, 1586 rows of scene and spawn). A destination whose OOT3D row
differs is listed disabled, with the reason in its tooltip; nothing is guessed.
Against the EUR code.bin 391 of 397 entrance references match; the six others
are debug scenes OOT3D numbers differently (SRD Map, Treasure Chest Warp,
Hyrule Garden Game).

## Not yet: respawn fields

SoH places a warp point exactly through a void-out respawn and loads grottos
through the respawn return data. The decomp maps OOT3D's respawn state to
SaveContext `+0x4EC` (flag) and `+0x4F0` (RespawnData), but the live title
disagrees: after writing `respawn[DOWN]` there and calling the title's void-out
(`0x0036EBDC`), the requested entrance was the respawn entrance the game keeps
(`0x0211`), not the written one, and `+0x4EC` stayed 0; with the exit helper,
a written flag was ignored. The runtime therefore writes no respawn field:
warp points arrive at their entrance, grottos are disabled. Finding the real
fields (a memory diff around a genuine void-out) completes both.

## Storage

`developer-tools.json` beside the config: `WarpPoints` (SoH's format) and
`DebugWarpScreen` (Better, Translation, the selected scene and entrance, age,
night), written atomically; a read or write error is shown in the tab.

## Verification

`oot3d_dev_warp_tests` (fake guest, 22 checks): catalog against the table,
scene and vanilla warps (age on load, time kept or set), warp points to their
entrance with no respawn write, grottos refused, arrival, gameplay and
transition-slot admission, SoH-format persistence and the boot point.

Live, developer launch from the play quick save (Kokiri Forest), log lines
`TRIAEVUM_DEV_WARP` (stderr): Hyrule Field "Near Drawbridge" as adult at night
arrives in 70 guest frames as adult (`age 0`), `day_time 0x0000`, night flag
1, health unchanged, capture showing adult Link at the drawbridge; the same
warp keeping child and day arrives likewise; a boot warp point warps at the
first gameplay frame to its entrance (0x00EE). `TRIAEVUM_DEV_WARP=<scene>:
<entrance>:<child|adult>:<day|night>` requests a Better-list warp at the first
gameplay frame for such checks.
