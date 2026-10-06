# Save states

Date: 2026-10-05. Branch `qual/windows-next`.

## Contract

A save state holds the **game moment only**. It loads with any internal
resolution, output size, upscaler, effect or texture-pack setting, and with
either UI profile.

| Part of the file | Content | Depends on settings |
| --- | --- | --- |
| Guest | A32 process and memory, CTR host services, DSP, PICA frontend and submission state | no |
| Clocks | gameplay clock, presentation scheduler, frame pacing, TopScreen routing | no (TopScreen routing is applied only in the TopScreen profile) |
| Render targets | identities only (`native_pica_guest_framebuffers`); their pixels are in the guest memory above | no |
| `host_settings` (optional) | graphics, TopScreen, controls and game language | stored only by a "state + settings" save |

Since 2026-10-06 a state carries **no graphics resources**, like an Azahar state
after its rasterizer flush. Saving writes the game's render targets (the native
namespace) at their native extent and PICA formats (colour 0-4, depth D16, D24,
D24S8; tiled 8x8, `pica_framebuffer_encode.h`) into the saved copy of VRAM; the
live process is not written. GPU copies the game made are already in memory
(the memory capture resolves deferred writes). The texture cache, the
interpolation namespace, display images and the visual replay state are not
saved: on load the targets are decoded from memory and rebuilt at the current
resolution, textures are decoded from memory on first use, and presentation
starts afresh with the next guest frame. S09 mid-flashback: 78 MB instead of
240 MB at any output size; the restore pass captures are identical to the
previous format's (1280x720 and 1720x720). Azahar `.cst` files themselves are
not readable: they serialize Azahar's own kernel, service and JIT objects.

The simulation rate (`--gameplay-timing`: 30 Hz or Enhanced 60 Hz) is part of
the game moment, not a setting. A state saved at the other rate is refused
before anything is restored, with a message naming the rate to start with.

Renderer content is what the game keeps on the GPU between frames (incremental
UI surfaces, framebuffer copies used by menus and transitions). It is stored at
the guest framebuffer extent: render targets carry `render_scale_permille 0`
and no enhancement guides or shadow mask. On load, each target is recreated at
the current scale, its colour is enlarged bilinearly, depth and stencil take
the nearest sample, and the guides start neutral until the next draw writes
them. Copies and display images are stored at their guest extent; the copy
sampler maps spans through the image extent, and the next display transfer
recreates a display image at the current output extent.
`fast/renderer3ds/pica_guest_extent_snapshot.h` holds the title-neutral
resampling.

Older states (renderer content at a renderer's internal extent, with guides)
still load through the previous path, which restores them at their own scale.
States of 2026-10-05 (guest-extent renderer content, texture cache, display
images) load through the same previous path; the paragraph above describes it.

## Keys and F1

| Action | Key | F1 **Save states** |
| --- | --- | --- |
| Save the game moment to a new file | F5 | Save state (F5) |
| Save the moment and the settings | Shift+F5 | Save state + settings (Shift+F5) |
| Load the last state | F8 | Load last state (F8) |
| Load the last state and apply its settings | Shift+F8 | Load last state + settings (Shift+F8) |

- **Last state**: the last one saved, loaded, or chosen in the tab (from the
  folder listing or with **Choose a state file...**). It is remembered across
  sessions.
- **Folder**: `savestates/` beside the executable by default; **Browse...**
  chooses another one and **Use default folder** returns to it. The folder is
  created on the first save; a failure is reported, never silently redirected.
- **Files**: `state-YYYY-MM-DD_HH-MM-SS.oot3dsav`, with `+settings` when they
  carry settings. Saves never overwrite a file. Nothing is deleted
  automatically; the tab shows the number of states and their total size.
- **Feedback**: each action shows a short notice outside F1 and its outcome in
  the tab (failures in red, with the reason).
- Preferences live in `savestates.json` beside the configuration file.
- The browser is the in-game ImGui browser (works in fullscreen and on every
  platform), like the material editor's.

## Settings ("+ settings")

The settings block stores the graphics settings, the TopScreen configuration,
the controls and the game language, each serialized by its owner.

Shift+F8 applies them before the renderer content is restored, so that content
is rebuilt at the scale they select. Exceptions:

- the display mode (window mode, output size and VSync, as the presentation
  owner defines it, plus monitor and refresh rate) stays as it is: it belongs to
  the machine and to the display confirmation flow;
- graphics preferences are restored as stored, like the startup configuration
  load (`GraphicsSettingsRuntime::RestorePreferences`): an effect whose
  capability is missing at that moment (for example view metadata outside
  gameplay) stays inert instead of being turned off;
- the game language is recorded and takes effect at the next full start, as the
  Game tab already states;
- TopScreen settings apply in the TopScreen profile only;
- scene-environment profiles are authored files with their own Save and are not
  part of the block.

Shift+F8 on a state without settings loads the moment and says so.

## Failures

A failed save is reported once and dropped. A save requested while an ocarina
text continuation is pending is deferred to the next frame boundary.
`runtime.json` reports `savestates.save_failure_count`.

## Command line

- `--load-state FILE [--load-state-with-settings]`: initial load, optionally
  applying the stored settings.
- `--save-state FILE --save-state-frame N [--save-state-with-settings]`:
  automated save.
- `--savestate-action FRAME:save|save+settings|load|load+settings[:PATH]`
  (repeatable): queues the same request F5/F8 queue, at a zero-based run frame.
- `--quick-state FILE`: pins one file for scripted runs; every save writes it
  and F8 loads it, and the folder controls are disabled.

## Verification (developer launch, host-build, mod-vfp-pgo)

- 3840x2160 output, Kokiri: the old format failed (two 4320x3840 targets,
  547 MB each, above the 512 MiB budget) and the failure repeated every frame.
  Now: 223 MB file (colour targets 2.1 MB, display images 2.8 MB, decoded
  textures 126 MB), saved in 4.3 s.
- That state loads at 2560x1440 and at 3840x2160; frame 60 differs between the
  two outputs only by resolution detail (mean 2-3/255).
- `--load-state-with-settings` into another configuration: every stored graphics
  field applied (MXAO included) except the display mode; TopScreen and controls
  applied.
- Mailbox path at 4K: save, save+settings, load, load+settings, a missing file
  (reported), load; the default folder and the last state are persisted.
- `oot3d_native_a32_savestate_tests`: guest-extent round trip with settings,
  rejection of guides or renderer extents in a guest-extent target, refusal of
  the other simulation rate before guest memory changes, resampling, library
  (names, folder, last state, persistence, newest-first listing, fixed slot).
- F1 actual-widget smoke: the tab's buttons, listing, folder and file browser,
  default folder and status (8,639 assertions in total).

## Limits

- Until the game redraws, restored content is an enlargement of the guest
  extent, and content saved with scene effects resolved keeps those effects
  (for example a pause-menu background).
- Decoded native textures dominate the file size (about 126 MB in Kokiri).
- The notice drawn after a hotkey action is part of the presented frame.
