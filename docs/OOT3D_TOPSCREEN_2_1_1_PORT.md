# TopScreen 2.1.1 source port

## Objective

Port the behavior of the official TopScreen 2.1.1 release into typed host
code without executing its IPS or injected ARM payload. The profile remains
selectable at runtime, the default OoT3D presentation remains unchanged, and
save data stays binary-compatible with the original game.

This document supersedes `OOT3D_TOPSCREEN_MOD_SEMANTIC_PORT.md` for current
work. That document remains the historical record for the 1.2 baseline and
for behavior inherited unchanged by 2.1.1.

## Binary evidence

| Evidence | Digest |
| --- | --- |
| Official `topscreen211.zip` | SHA-256 `e0c143c872ccf4ad72033caab768753b147a4033d260a701204a63ee6bc16df4` |
| Official `topscreen211.zip` | MD5 `2457743e14ffe02bfe64adc083f900a1` |
| EUR `code.ips` | SHA-256 `c834030be2fbbfd93db26180333f112e1abcfd85050b09252a8449f06bf90400` |
| Reconstructed EUR payload at `0x005c7000` | SHA-256 `e25bacdae7331714cb8be89044e07f973c3f9701a30bc84e6e282a15aaf89ade` |

The reconstructed payload is 111,157 bytes. Its maintained Ghidra export has
266 functions. `OOT3DHud` public source is useful corroboration for names and
intent, but it is not binary-identical and is never authoritative over the
official archive.

`tools/oot3d/decomp_support/scripts/analyze_topscreen_mod.py` reproduces the
IPS inventory and payload extraction. The archive, Ghidra project, export and
payload are offline evidence; none is a runtime dependency.

## Architecture

```text
official IPS/CTXB evidence (offline only)
    -> typed TopScreen policy and geometry       ui_topscreen
    -> OoT3D guest-state/action adapters         runtime consumer
    -> UI lifecycle and application input        native runtime
    -> generic UiPrimitive + texture identity    UI presentation contract
    -> ordinary native frame composition         NRI renderer
```

The boundaries are mandatory:

- `tools/oot3d/ui_topscreen` owns configuration, pure control/action policy,
  camera math, temporal hint state and geometry. New policy code does not know
  Vulkan, NRI, framebuffers or renderer internals.
- `oot3d_top_screen_runtime_consumer` is the isolated application adapter. It
  owns new 2.1.1 guest addresses, native state reads/writes and calls to
  original OoT3D actions.
- `Oot3dNativeUiLifecycleBridge` owns observation and temporal UI state. It
  requests typed state from the consumer and emits generic UI primitives.
- `Oot3dNativeA32UiTextureProvider` resolves profile texture identities. It
  does not alter draw ordering or backend state.
- NRI/Vulkan remains title-profile agnostic. No TopScreen condition, address,
  texture name or layout rule may be added to the renderer.
- Preferences live only in external JSON and the F1 host settings panel. They
  are not stored in `SaveContext`, guest configuration bytes or save files.
- TopScreen never promotes or copies the lower framebuffer. It reconstructs
  only the required native elements on the top-screen UI canvas.

This follows `OOT3D_NRI_FIDELITY_EXTENSION_ARCHITECTURE.md`: gameplay, input
and UI policy remain outside the renderer while final composition consumes
stable, generic contracts.

## Implemented surface

| Area | Current source port |
| --- | --- |
| Configuration | Typed `oot3d_topscreen_ui_v2`, strict ranges, atomic external persistence and v1 migration |
| HUD | Normal/Restoration layouts, scale, margins, magic-bar offset, HUD/D-pad visibility and minimap session visibility |
| D-pad | Exact 14-action ABI, separate child/adult mappings and official defaults |
| Native actions | View, Ocarina, minimap, boots, sword, all-boots cycle, tunic, shield, Boomerang and Slingshot routes |
| Item input | ZR/ZL item lanes, native item-query compatibility and direct child-item lifetime handling |
| Camera | Zoom percentage, FOV percentage, free-camera speed/inversion and C-stick aiming speed/inversion |
| Filtering | Five official smoothing levels, exact coefficient table, half-unit deadband, update only on guest refresh |
| Pause behavior | Configurable Select action and configurable Items-to-Save B behavior; START reopens the last page (0x005E21C8); on an open Items or Gear page SELECT shows the native item description over the page (0x005E3D3C, 0x005D3344..0x005D3410) |
| Items hint | Native selected-model bounds, 90-update delay, 12-update fade, 90-update pulse and exact atlas geometry |
| Assets | Original-size localized CTXB replacements plus named 2.1.1 menu/font profile assets in O3TU v2 |
| Version choice | The title's normal / Master Quest choice on the upper screen with the frontend menus, native input unchanged (`TRIAEVUM_TOPSCREEN_FRONTEND_SKY.md`, #28) |
| Compatibility | O3TU v1 remains readable; default OoT3D profile and original saves remain unchanged |

Equipment and direct-item routes deliberately call or mutate the same native
OoT3D contracts recovered from the payload. They are gameplay intents handled
by the application adapter, not renderer substitutions.

### Product hooks

Product builds (`OOT3D_WHOLE_AOT_PRODUCT_MODE`) execute the precompiled
whole-AOT graph through pause routes. `ExecuteProductTopScreenHook` accepts the
sorted list `ProductTopScreenBlocks` (every TopScreen source-port block), and
those blocks are observable exits. The graph reports only the starts of its
basic blocks, and blocks end after branches and calls, so a site in the middle
of a block would never be reached.

`tools/oot3d/native_a32_runtime/whole_aot_observed_sites.json` lists the hook
and resume pcs that are not natural block starts (touch-button draw predicate
0x0042F5CC, the page draw calls 0x00419838/50/74/8C, 0x0041952C, 0x004198B0,
0x0041E984, 0x0041EB28, the scene sites 0x00300520..0x0030055C, the FOV scalar
0x00478EB8 and its resume, the control-flow and float-load contracts, and the
planned layout sites). `whole_aot_program.py` starts a block at each of them,
so a title module generated from it reaches all of them. The product runtime
checks the title module's entry points at startup and refuses one that lacks
any TopScreen site, instead of silently skipping it.

The developer interpreter runs the block policy at every dispatcher entry
before the hook; `ExecuteProductTopScreenHook` does the same, except for the
entry that its own observable exit left, whose policy already ran in compiled
code. A hook that branches to another hooked entry (0x0041E984 resumes at the
input boundary 0x0041E988) therefore still runs that entry's policy.

The camera update entry 0x002D84C4 is observed whenever the TopScreen profile
is active, because it resolves the FOV owner. 2.1.1 stores the play view's
FOV address (`camera->play + 0x198`, its hook at 0x002D92B4) and 005DF388
scales and clamps the value that 0x00478EB8 loads from that address, at every
percentage.

One 2.1.1 patch is deliberately not ported. 2.1.1 branches 0x0041EB40 to
0x0041EC10 and so skips the pause scene dim (a translucent grey submitted
through `EnvironmentRenderer_SubmitViewportConfig`). At 400x240 the page covers
the scene; with wider output the scene shows beside the page, so the port keeps
the native dim and those strips stay dark during the pause. The site stays in
the observed-sites catalogue; an extra block start is harmless.

### SingleScreen 3.0 Items page rebase

The Items model draw and shortcut-column adapters follow SingleScreen 3.0
build 25aeb. Task 2's developer run verified circle-pad shortcut navigation,
reordering and page exits. Claude accepted Task 2b's D-pad navigation, tap
reorder and L/R hints in game; the pause run emitted no gameplay D-pad or
Items assignment attempts. Claude's Task 2c developer run verified in-page
assignment and held-X clearing. Task 2d adds the pre-picker A-hold cursor and
recovers the ring material; its consumer changes and visual checks are below.
The remaining profile uses the 2.1.1 port
except where stated below. The 3.0 binary and oracle are offline evidence
only; no payload or mod-private guest globals are installed or executed.

Task 1b corrected the earlier assumption that the oracle showed six native
inventory columns. Native quads 0/1 cover the five-column inventory panel;
3.0 adds a four-row shortcut column. Its background, icon producer and input
are separate contracts. Restoring the old zeroed Items/Gear extents restores
BACK-button quads, not an inventory column.

- At 0x0041EE74, the native-call sequence recovered from 0x005E3CA0 /
  0x005E3728 uploads projection 0.6, draws ranges 0..1, quad 2, 6..11,
  12..24 and 28..36, then restores 0.8. Ranges 3..5 and 25..27 are omitted.
  Grid X uses +4; ranges 6..11 and 28..36 use -40; 12..24 stays in place.
- Quad 2 starts with zero width in the native data. ExecuteItemsUpdate at
  0x0041E9A4 reproduces 0x005E460C -> 0x005E3CFC..0x005E3D9C after native
  0x0042DAEC: rectangle (-44,0,100,200), atlas rectangle (-1,0,100,200).
  Inactive Items gets zero extent; buffer dimensions normalize the UVs.
  These 20 position/UV words persist for the native upload. The selected
  stream is a separate copy (0x00348BE4), so draw-time base edits are too
  late. The update hook issues MOV R0,R4 and the native 0x0042DF3C call,
  returning to block start 0x0041E9AC; 0x0041E9A8 is not a block start.
  Claude integrated this boundary in Task 1b. The draw offset still
  follows 0x005CB13C using the selected stream and animated grid origin.
- 0x005CD8C8 narrows the native model's index count and index pointer;
  0x005CAFCC applies its local offset through six matrix lanes. Those eight
  words are restored after the draw, on skipped ranges, failure, and reset.
  Assignment origins stay intact.
- The upper call is 0x005E3728(1,1), with X/Y scales 1/1. The 0.85 X scale
  and +36 offset belong to the compact lower path. Shared page projection
  at 0x0041EE1C remains -0.005 / 0.8. The writer bind/upload/accumulate
  transaction stays contiguous.
- Assignment ranges follow 0x005CE1D0: split at X 256, +4 for the grid
  side, zero for the right side. Native missing-stream/type-7 fallbacks,
  visibility gates and the update-keyed countdown remain.
- The native icon-group/cursor portion of 0x005E3264 uses projection 0.58
  for grid cursors and 0.6 for right-column cursors. 0x005D6020 supplies
  the assignment-cursor owner-X/hidden-quad search. The native tail remains.
  The shortcut producer 0x005E2DF4 is represented by generic host primitives,
  with native item textures and the named 3.0 UI atlas.
- Gear has a model-only interceptor at block start 0x00424C34, reproducing
  the 0x005D5AE4 ranges 0..24, 28..29 and 32..40. It skips BACK quads
  25..27/30..31 and resumes the native tail at 0x00424C44. Claude integrated
  this adapter and verified the BACK-fragment correction in Task 1b.
- The ten old BACK-extent zeroing patches remain removed; other Items/Gear
  layout words match the 3.0 byte image. The extra left pause edge remains;
  the right edge still has zero width while Items is active.
- The hint consumer scans visible cursor quads as in 0x005CB298 and uses
  upper-screen coordinates: native X+84 for projection 0.58, X+80 for 0.6.
  Its 2.1.1 atlas/timing remains until the separate atlas task.

Task 1b's focused runtime tests start from the actual zero-width inset
contract, keep base and uploaded geometry separate, verify the post-update
MOV/continuation, inactive/invalid-buffer cases, and inspect inset positions
and UVs after upload. Draw-time model state is restored on completion,
skipped ranges, reset and failure. They also check Gear's ranges, native
continuation and lower/inactive pass behavior.
The profile tests retain coverage for native extents and layout rebinding.
Task 1 previously also passed the Items-hint and lifecycle-bridge tests.

Task 2 adds the shortcut contracts from 0x005E460C, 0x005E2DF4 and
0x005E2D24. The validated host permutation selects actions [9,1,2,8]:
Minimap, View, Ocarina, All Boots. Map/View are always present; Ocarina
requires ownership; the boots row requires both special boots and is dimmed
for child Link. Native 0x005CBC18 dims RGB to 0.3 while retaining alpha 1.
The selected/grabbed icon grows from 42 to 52 pixels, and reordering retains
the six-update movement, swap and white pulse. View uses (452,73,47,30) and
the boots badge (320,3,40,40) in custom_ui01, which is 512x256. Native item
crops use the 512x512 item atlas. All output is ordinary UiPrimitive data;
there is no renderer or texture-hash special case.

The existing 0x0041E9A4 update intercepts input after sampling, before native
Items navigation. Both grid-left and assignment-right lead into the extra
column. Vertical motion wraps within its four rows. A horizontal exit leaves
one native step to enter the grid or assignments; native previous-column
bookkeeping is retained. A short A tap selects a row to move, a second tap
swaps it, and B cancels. Row-order changes notify the host for config Preview;
no guest preference bytes or save data are written.

Holding A for 15 subsequent updates opens the in-page D-pad assignment
picker from 0x005D2014. Task 2c replaces the former TakeShortcutAction()
request; the accepted user decision keeps this interaction in the pause page.
Assignments Preview the current age's host mapping. Saving remains an explicit
F1 action, with no guest preference bytes or save writes.

The new cursor interceptor starts at 0x00434B98 (two NOPs before the BL at
0x00434BA0), calls native 0x00446E98, then 0x002F7AF4 with X=-39 and
Y=row*48+5, and resumes at 0x00434BA4. It uses the same native-call sequence
and caller restoration as the draw adapters. Claude integrated the entry and continuation in the dispatch and both
window_setup hook lists. Codex's original full irblocks.py check was blocked
by file permissions (coord/task2-blocks.log); the Task 2b brief records
Claude's successful check of every Task 2 hook, resume and native call PC.
0x00434BA0 is not a hook. Task 2b adds no hook, resume or native call PCs.

Four focused targets passed after Task 2's final changes: wide Items runtime,
Items hint/presentation, native UI lifecycle bridge, and mod profile. New
coverage includes all 24 permutations, both column entrances/exits, vertical
wrap, hold threshold and action eligibility, reorder/config notifications,
caller preservation and native call ordering, icon crops/brightness, and
inactive presentation. Picker reachability, config integration and visual
parity remain acceptance checks for Task 2c. The new pack restores native I/II; the separate
L/R tab-button atlas regression is addressed by Task 2b below.

Task 2b replaces the old tab hints with 3.0 0x005D4A0C, called after the
page redraw by 0x005E0E6C. Both backing and glyph models use custom_ui01
(0x005D1644), exposed as oot3d/topscreen/3.0/ui_atlas (512x256).
Backings use (401,196,18,18), drawn at (65.5,206) and (293.5,206), size
41x42. Rects 20/21 at 0x0060A568 are (462,108,37,28) and
(240,117,37,28); their 1.05 centre scaling places 38.85x29.4 glyphs at
(65.575,208.3) and (295.575,208.3). Only a held L/R glyph dims to RGB
0.55, retaining alpha 1. Items, Gear and Map show the hints (oracle
f2700/f2800/f3000); System/Save suppresses them. Generic primitives use
the existing lifecycle redraw gate and profile texture, with no new hook,
renderer rule or dependency on retired native cursor/equipment atlases.

The Task 2b D-pad failure originates before the shortcut boundary: the
window input owner resolves gameplay D-pad actions while pause is open.
With the child mapping, its ItemZr/ItemZl actions become ZR/ZL assignment
input. The supplied six left presses caused six assignment begins and
completions; three down presses also reached gameplay action handling.
The required Claude-owned integration gates ResolveTopScreenDpadActions
in UpdateGuestServicesForRefresh on a successful read of 0x0050AF68 == 2
(the native gameplay controller state), retaining the existing profile,
scene and Ocarina checks. Physical D-pad and ZR/ZL inputs remain intact.
Task 2b runtime tests protect both native direction words through ordinary
grid navigation, shortcut entry and exit. They do not validate the window
integration or replace a game run. Claude subsequently applied this guard and
accepted D-pad navigation and L/R hints in game.

Task 2c implements the age-local assignment policy in
`oot3d_top_screen_dpad_picker.h` and uses the existing Items update sequence:

- 0x005F3BA0 calls the picker update in the same update that opens it. Opening
  lasts eight updates; confirmation lasts twelve; closing lasts six. Raw held
  edges select physical D-pad up/down/left/right, in that priority. Circle-pad
  repeats do not assign directions. Grid/column input is suspended and all four
  native edge/repeat words are consumed while the modal owns input.
- 0x005D0604 replaces the chosen direction, swapping the first duplicate action
  with its previous occupant. Choosing the direction already holding the action
  unassigns it. A/B cancels without a config change. X clears all four mappings
  after 45 subsequent held updates and requires release before another clear.
- 0x005D1FE4 uses native sound 0x0037547C, bank 4: open 0x01000497,
  assign 0x0100048D, unassign 0x0100048F, cancel/clear 0x01000496. The call
  supplies volume/frequency pointer 0x0054AC20 and reverb pointer 0x0054AC24.
- The 0x005E9348 presentation uses projection 0.8 (native X+40): dimmer,
  156x172 nine-slice panel centred at (200,120), 58px D-pad cross, 28px
  mappings and 30px central action. Native custom_ui01 remains 512x256.
  Item icons use the native 512x512 item atlas. Confirmation grows the target
  icon and moves a fading action copy with the recovered ease-out curve.
- The clear-hold cursor follows 0x005CCA90 / 0x005CBD64: 24 black annulus
  quads, up to 24 yellow quads including a fractional last segment, and a 13px
  X glyph. Its radial crop belongs to native OcarinaPage; the X glyph belongs
  to custom_ui01. Claude integrated generic free corners and alpha-only texture
  sampling. Task 2d additionally recovers the missing post-filter alpha scale
  of 2, exposed as `TopScreenDpadPickerHoldCursor::TextureAlphaScale`.
- Lifecycle capture cancels on page/age transitions even when Items update is
  skipped. Lifecycle ResetAfterStateLoad and runtime Reset cancel without
  publishing changes; mappings and callbacks remain host-owned. The ordinary
  Items hint is suppressed while the picker is active.

The integrated host Preview wiring seeds SetDpadAssignments(child, adult),
installs SetDpadAssignmentChanged(bool child,
const TopScreenDpadAssignments&) to copy the latest config, replace only that
age's array and call Preview, then synchronizes both arrays on config snapshots.
Local assignments are stored before the callback so synchronous Preview does
not cancel confirmation. A different external mapping/order cancels stale UI.
No configuration schema, asset importer or new source target is required.

Task 2d traces the ring context at `[0x0060F414-0x888]` to the 48-quad
construction at 0x005D173C. It uses a copy of native template 0x004D2BD0,
modified at 0x005D1708/0C to RGB REPLACE and alpha scale 2. Native
0x003146E4 packs those fields into TEV stage 0: RGB takes primary/previous
colour; alpha is `clamp(2 * vertex.a * filtered_texture.a, 0, 1)`.
0x005CCAE4 gets native slot 5 (OcarinaPage), and 0x005CCB1C rebinds it with
linear min/mag filtering and clamp-to-edge. It does not use custom_ui01 for
the band. The top-left UVs remain outer U=423/512, inner U=428/512,
V=189.5/256 and 192.5/256. Native 0x005CBC68 writes the complementary GPU V.

The A-hold ring uses the same mesh/material. 0x005CD470 exposes progress only
for held updates 2..14 as `HoldFrames / 15`; update 15 opens the picker.
0x005E34A4 places its centre at the selected native cursor right edge plus
12.133333 minus 2 pixels, and at the vertical midpoint, on the shortcut
projection. Bounds are read from the live selected stream. Its 13px A glyph
uses custom_ui01 `(46,89,32,32)` through normal texture modulation. The
lifecycle exposes `TopScreenItemsShortcutCursor()` alongside the existing
`TopScreenDpadPickerCursor()` after `BuildTopScreenPresentation(GameplayHud)`.
Rendering advances no hold timer. Release, navigation, reordering, modal
opening, page/age/System transitions, source detachment and savestate reset
suppress stale A output. Unavailable actions can show hold progress but do
not open, matching the native availability check at the end of the hold.

REQUEST to the Claude-owned window/renderer integration: consume both cursor
outputs with native OcarinaPage, `texture_alpha_only=true`, and layer 37,
before their layer-38 X/A glyphs. Preserve the recovered quad order and
triangles [0,1,2,2,1,3]. Add a generic post-filter alpha-scale field and clamp
the result before blending; transport `TextureAlphaScale` through that field.
Do not multiply cached alpha texels (which clamps before filtering) or set
vertex alpha to 2 (the current UI shader wraps values outside -0.51..1.51).
Keep native vertex alpha at 1 and use linear/clamp sampling. No texture
identity, UV, or alpha-only flag change is needed. The clear-ring correction and rendered A ring await
that integration and comparison with the 3.0 oracle.

There are no new Task 2c or Task 2d hook, resume or native-call PCs. The picker reuses
hook/sequence return 0x0041E9A4, native sound entry 0x0037547C, displaced native
call 0x0042DF3C and native return 0x0041E9AC. 0x005D2014, 0x005D29E4,
0x005D0604, 0x005E9348 and other 3.0 producer PCs above are offline evidence,
never executed payload. The unavailable irblocks checker was not retried.

Focused policy/runtime tests cover both ages, every row action/direction,
duplicate swap/unassign, input ownership, cancel/clear timing, callback
reentrancy, config changes, page/age/reset cleanup and native call restoration.
Presentation tests check frame/cross/crops, confirmation animation, exact
ring geometry/material, the A-hold interval and live placement, and lifecycle
cleanup. The original legend also has a third entry, Y View Model. Its submode (0x005D24FC / 0x005D2274 /
0x005D21E8, transform wrapper 0x005D2734 at native 0x00425880) is a separate
follow-up: no Y legend or handler is advertised in this assignment-only port.
Rendered parity, the complete original modal including that viewer, and
end-to-end host Preview persistence are not established by these tests.

For one bounded comparison, set TRIAEVUM_TOPSCREEN_ITEMS_TRACE=1. The
runtime emits at most eight SS3_ITEMS and eight SS3_GEAR records with model
bindings, base/selected inset widths, update count, cursor state and Gear
ranges. Reuse the 560-frame pause timeline from the Task 1b evidence and compare Items f100,
Gear f300 and Map f420. After wiring the new sites, the background should
widen and Gear should lose the BACK fragment. With Task 2 wiring, compare the
shortcut icons and each column transition as well. Claude owns game/emulator validation and the
integration files, as recorded in
I:/_triaevum_work/singlescreen30/coord/HUD_COORD.md.

Dungeon Map shares the full viewport and page projection: Map state
0x00506CE8 must be nonzero and shared map mode 0x004FDABC must equal 1.
World Map stays centred. Verified scalar contracts widen both the dungeon
panel and title to 272 pixels and move the side columns; six native icon
coordinate hooks set R2 to -30 while retaining the ownership branches.
Dungeon Map omits both extra pause edges but keeps the tab hints.

Scene mode 5 (the file select and the other frontend screens) has no pause
edges: 2.1.1 0x005D2078 leaves through 0x005D18F0 (scene owner byte +0x100
== 5) before its edge producer 0x005D0374. The record-33 site 0x00419838
keeps the mode-5 viewport but no longer adds the 1.2 payload's edges; they
showed as stone strips beside the file select once 29d0375 made the site
reachable in product builds. Azahar captures of 2.1.1 show the file select's
sky to the screen edges.

The Save (System) page overlays gameplay on the upper screen. 2.1.1 replaces
the queue-5 draw at 0x0030055C: after the original 0x004228D4 it calls its
compositor before the top transfer. While System is active (0x0050A530) the
compositor sets viewport (0, 0, 480, 400), saves the pause root's
backdrop skip (0x005043F4) and touch (0x0050AF68), writes 1 and 0, calls
`PauseUi_Draw` on the gameplay target and restores both values and the
viewport. `TopScreenSystemPageRuntime` (pause transaction unit) runs the same
native calls and resumes at 0x00300560. System joins the shared wide
predicate, so 0x0041EE1C applies the same projection. The page keeps the
native framebuffer plan rather than PageRedraw's lower-target swap, draws no
pause edges or tab hints, and its later lower-tail draw uses the Tail
suppression set. The payload's Mod Options affordance (0x005DCA80,
0x005DF178) is not drawn; mod settings live in F1.

Exits that run guest continuations are conditional: 0x0041E988 leaves the
graph only while the pause transaction or song text has work, and the page
draw return 0x00419830 only while the item description is drawing or may
start. The page-child restore at 0x00419830 runs before that exit, because a
resumed graph skips the entry it left from.

The item description is the native message 0x005C1878, which the native page
draws under the relocated page. `TopScreenItemDescriptionRuntime` redraws it
after the page with the payload's native calls (material bind, the payload's
two matrices from guest stack scratch, viewport X 0 or 300 by cursor row).
While a page is open SELECT belongs to the description; it reaches the
minimap and Save routes only in the gameplay pause state.

## Deliberate host adaptations

- The mod's in-game settings overlay and preference chords are represented by
  the F1 panel and external JSON. This keeps gameplay inputs available and
  avoids mod-private guest save/config bytes.
- Items-hint coordinates now follow the 3.0 cursor projection on the
  400x240 upper canvas before becoming generic primitives.
- `custom_menu00.ctxb` supplies the Items-hint atlas. `custom_font00.ctxb` is
  retained as a named profile asset, but host F1 text uses the host UI font.
- Preferences changed for the current session, such as minimap visibility,
  update typed host state only unless the user explicitly saves the JSON.

## Validation state

The following is proven at this checkpoint:

- policy, geometry, configuration and input unit tests pass;
- native Items-owner memory traversal is exercised with a typed fixture;
- the complete game target links with the isolated runtime consumer;
- whole-AOT closure remains 12,419 compiled functions, three host boundaries
  and zero residual A32 entries;
- no NRI, Vulkan or renderer source is changed by the 2.1.1 port.

This does not yet claim full visual parity. Runtime acceptance still requires
a controlled game comparison for HUD layouts, the Items hint, each configured
D-pad action, camera/FOV transitions and localized CTXB selection. Any defect
found there must be corrected in policy, consumer or lifecycle code, not with
a renderer special case.

## Focused verification

```powershell
C:\devkitPro\msys2\usr\bin\cmake.exe --build /i/oot3dre_work/whole-aot-product-consumer --target oot3d_top_screen_items_hint_tests oot3d_native_ui_lifecycle_bridge_tests oot3d_top_screen_mod_profile_tests oot3d_native_a32_input_tests -j 6
I:\oot3dre_work\whole-aot-product-consumer\oot3d_top_screen_items_hint_tests.exe
I:\oot3dre_work\whole-aot-product-consumer\oot3d_native_ui_lifecycle_bridge_tests.exe
I:\oot3dre_work\whole-aot-product-consumer\oot3d_top_screen_mod_profile_tests.exe
I:\oot3dre_work\whole-aot-product-consumer\oot3d_native_a32_input_tests.exe
```

The product gate is:

```powershell
C:\devkitPro\msys2\usr\bin\cmake.exe --build /i/oot3dre_work/whole-aot-product-consumer --target oot3d_native_game -j 6
```
