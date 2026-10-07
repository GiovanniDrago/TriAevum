# Title intro at 2160p: above 60 fps

2026-10-06. Request: the title intro must average above 60 fps at 3840x2160
with no upscaling, and the screen may differ from the previous build by at most
3%. Developer launch (`dev-latest/avvia.cmd`: TopScreen, Enhanced-derived
settings with MXAO, virtual shadow maps, point-light cube shadows, per-pixel
PBR and grass, render scale 1.0, no anti-aliasing or upscaler), RTX 3060.

## Where the time went

Before: 35.5 fps, frame p50 25.8 ms, GPU busy 26.2 ms of every frame. The
CPU was not the limit. Half the render scale cut the GPU frame to 8.1 ms, so the
cost was per pixel. A GPU timeline (below) and one-feature-off runs located
it:

| Removed | GPU frame (ms) | Saving |
|---|---|---|
| nothing | 26.5 | |
| MXAO | 17.3 | 9.2: 4.7 pass, the rest guide writes in the scene and grass |
| grass | 20.2 | 6.3, nearly all inside the native passes |
| directional shadows | 24.0 | 2.5 (5 at the final density) |
| per-pixel lighting (Native) | no saving | |

Two causes multiplied every effect:

1. **The top screen was rendered twice per output pixel.** The game draws its
   top screen into a 480x400 target (stored rotated) and its display transfer
   box-filters it 2:1 onto the 240x400 LCD. `ResolveNativePicaRenderExtent`
   kept that ratio at host resolution: 4320x3840 for a 3840x2160 output, so
   every native draw, guide, grass blade and AO texel ran at 16.6 million
   samples for 8.3 million pixels.
2. **No draw kept early depth testing.** Every native fragment program ended
   with `gl_FragDepth = pica_depth`, and the grass program wrote its depth too.
   The driver therefore shaded every overdrawn fragment completely (lighting,
   shadow-map gathers, five guide outputs) before testing it.

## Changes

- **Top screen at the presented density** (`render_resolution_policy.h`,
  `pica_display_transfer.{h,cpp}`). From 480 presented lines the host renders
  the top target at the output's density (scale 1 = one sample per output
  pixel, both axes) and the guest's 2:1 transfer becomes a copy
  (`SourceAtPresentedDensity`). A render scale above 1 now supersamples both
  axes evenly. Below 480 lines (native-size output) the game's 2x sampling and
  filter are kept unchanged. This is the only change that alters pixels: the
  former vertical 2x supersampling is gone at high resolution.
- **Rasterized depth when it is the PICA depth**
  (`oot3d_native_pica_fragment_shader_gen.cpp`). With Z-buffering, scale -1
  and offset 0 the PICA depth `-z/w * -1 + 0` is bit-identical to the
  rasterized depth. Such programs no longer write `gl_FragDepth` (depth output
  `FixedFunction`, own state key), so the hardware tests depth before shading.
  In the intro 96% of draws qualify; decals with a depth offset and W-buffered
  draws keep the explicit write. Captures are bit-identical.
- **The same for grass** (`grass_shader_sources.cpp`, `grass_draw_state.h`,
  `interactive_grass_pass.cpp`). A second set of grass pipelines without the
  depth write is bound per batch when `GrassDepthIsRasterDepth`. Bit-identical.
- **Display images resized in flight** (`gfx_vulkan_pica.cpp`). At boot the
  game writes one display address first with a 1:1 crop and then with the 2:1
  transfer. At the new density those differ in size, and the replaced image was
  destroyed while the GPU still used it (device lost in 2-7 of 10 boots).
  Replaced images now wait for their frame slot's fence
  (`ReleaseRetiredNativePicaDisplayImages`); this was a latent defect for any
  resize with frames in flight.
- **GPU timeline** (`oot3d_vulkan_gpu_profiler.{h,cpp}`, developer only).
  `TRIAEVUM_GPU_TIMELINE=1` writes labelled timestamps (every profile scope,
  each render pass with its namespace and size, grass draw, shadow maps, display
  effect passes) and prints the mean GPU time per label every 300 frames. Each
  timestamp serializes the GPU slightly; use it to attribute time, not to
  report frame rates. Inert when unset.

## Result

Same launch and intro, measured on the wall clock (`TRIAEVUM_PACING_TRACE=1`,
warm-up 300 frames):

| Build | VSync | fps | frame p50 / p95 / p99 (ms) |
|---|---|---|---|
| before | on | 35.5 (38.6 without the present stall) | 25.8 / 33.3 / 36.6 |
| after | off | **87.8** | 9.9 / 18.1 / 20.3 |
| after | on | 52.0 (58.5 without the present stall) | 16.3 / 32.0 / 36.0 |

Every VSync run, before and after, contains one 4.4-5.1 s frame spent in
`present` (`phase_timing.slow_frames`) at an arbitrary frame; runs without
VSync never do (longest frame 38 ms). It is outside this change and
unexplained; the bracketed rates exclude it. With VSync the remaining misses
are the intro's first seconds (pipeline preparation and its heaviest shot:
GPU frames up to 20-25 ms between frames 180 and 420) and isolated 25 ms
frames; afterwards the GPU frame stays within 11-15 ms per 60-frame window
(`TRIAEVUM_GPU_GAP_TRACE=1` now also prints each window's longest GPU frame and
the frames above 16.7 ms).

GPU frame on fixed steps (timeline on): 26.5 ms before, 11.4 ms after (the
heaviest 5-second window 12.9 ms instead of 31.7). The exact changes alone
(depth, grass depth) give 21.2 ms at the old density.

Parametric (always-available) programs keep their explicit depth: they write
the same value, their built-in artifacts stay valid, and they cover a draw
whose specialized program uses the rasterized depth
(`PicaUberEffectSupports`). The two new grass programs and the changed grass
sources are in `builtin_pass.spvpack` (regenerated, `build_pass_artifacts
--check`). S17 (cold/warm shader preparation) passes: no draw waits, nothing
compiled at run time, longest frame 37 ms.

Placed UI and movie pictures are fitted in native coordinates with uniform
proportions (`ResolvePicaRasterCanvas`); at the presented density their X scale
is halved (`NativeTopScreenStorageSamplingX`), so HUD, menus and the Sheikah
Stone movie keep their proportions (a first version without this squeezed the
minimap and the movie; the battery caught it).

Screen difference against the previous build, five deterministic intro frames
(400-1600) at 3840x2160: mean absolute difference 0.19-0.79% (average 0.50%),
PSNR 33.5-37.7 dB. The difference is confined to polygon and blade edges, which
lose the vertical 2x supersampling; 3.6-15.4% of pixels change by more than 3%
in some channel.

## Screen audit (2026-10-07)

Question: is the top screen rendered once, at the intended resolution, and is
the bottom screen limited to what TopScreen needs? Developer launch, the play
profile above (3840x2160, render scale 1.0, Interpolated2x, TopScreen), real
time from a Kokiri gameplay save plus the 6,000-frame intro run;
`--pica-semantic-trace` for structure, `TRIAEVUM_GPU_TIMELINE=1` for cost (its
render-pass labels now carry the target namespace).

- **Mono.** The top LCD left and right addresses are equal in every frame and
  each guest frame has one top display transfer from one 480x400 target: no
  second eye is drawn.
- **Resolution.** The host top target is 2160x3840 (stored rotated): one
  sample per output pixel, without the game's 2:1 vertical sampling. The
  bottom target is 240x320, its native size.
- **Executions per guest frame.** The draw list runs three times at that
  resolution: the interpolated and the repeated presented samples (namespace
  2), and the non-presented canonical frame (namespace 1) that keeps the
  guest-visible target exact for raw copies and savestates. The canonical
  frame runs no scene effect, but its target carries every guide attachment
  (about 55 bytes per pixel, roughly 450 MB at this size). GPU per presented
  frame: presented top passes 5.36 ms, canonical 0.73 ms, of 13.3 ms.
- **Bottom screen.** The game still draws its complete touch screen: one
  clear and ten UI draws (about 950 vertices) per guest frame, transferred to
  the bottom LCD, which TopScreen never presents or reads: its relocated HUD
  copies native quads and save data from guest memory. The guest code that
  builds them is what TopScreen consumes. Host GPU cost 0.03 ms per presented
  frame (both namespaces).

### Canonical frame without guides

Native-memory execution (the canonical frame and raw-copy dependency
flushes) now renders with a colour-only attachment layout
(`ActivePicaAttachmentRequirements`, `gfx_vulkan_pica_attachments.cpp`): its
targets are created without guide storage, its passes use the canonical
render pass or one dynamic-rendering colour attachment, its pipelines are
built for that layout and its clears cover colour and depth only. The layout
follows the published sequence purpose, and a pass restarts if the layout of
its target changes. Presentation keeps the frame's instrumented layout and
grows guides on any target that lacks them, as before.

Verification, developer launch, play profile with AO off at 2560x1440 (AO's
temporal noise and the desktop's window clamp make the 4K profile differ from
run to run), scripted input, old/new/old builds: every capture and the
savestate (which holds the canonical colour targets) are bit-identical in
gameplay (11 captures), a pause opened and closed twice (11), and the Sheik
revelation near-copy checkpoint with 128 raw TextureCopy reads of the
canonical target (8). Real time at 3840x2160 with the full play profile,
two interleaved rounds: dedicated VRAM 2,080 -> 1,759-1,778 MiB; the
canonical pass stays at 0.75 ms of GPU per presented frame (canonical
shaders never wrote the guides; only their storage and clears went), frame
p95 32.0 -> 30.3-30.5 ms.

## Reproduce

Scripts are private (`I:/_triaevum_work/perf-levers/tmp/intro-fps`); the
procedure is: copy `dev-latest/config.json`, run the developer launch with
`--frames 2400 --benchmark-warmup-frames 300` (wall clock) or with
`--fixed-delta-seconds 0.016666666666666666` plus `--screenshot ...
--screenshot-start-frame 400 --screenshot-interval 300 --screenshot-sequence`
(captures), and compare the BMPs per pixel.
