# Parallel presentation: design

Status: design, branch `perf/aot-levers`. Nothing here is enabled by default.

## Goal

Overlap guest simulation with presentation while keeping every presented
image, every interpolation decision and every guest-visible effect identical
to the serial loop. The renderer (NRI/Vulkan backend) is not changed; only
who calls it, and when.

## Why, in the product mode

The product default is `native30_interpolated` with `FrameRate =
Interpolated2x`: a 30 Hz guest and 60 Hz presentation. Measured on the Field
checkpoint (host `fca3e07`, `mod-vfp-pgo`, no-present, unpaced, fixed 1/60 s
host delta; `analysis/interp_profile.py`), per guest update:

| Work | Share of the main thread |
| --- | ---: |
| Guest (title code, SVCs, synchronous PICA capture) | 40.7% |
| Presentation `Execute` (native frame + 2 samples) | 31.2% |
| Transition preparation (draw matching N-1 -> N) | 8.8% |
| Draw-plan build | 5.4% |
| Interpolated sampling | 1.9% |
| UI, audio, other | about 6% |

Each guest update executes the draw list three times: the non-presented native
frame that keeps the canonical target complete, the first interpolated sample
and the repeated sample on the following host frame. Guest and presentation
are summed today; overlapped, the frame costs their maximum (about 50% against
about 44% of today's frame), up to about 1.8-2x throughput when CPU-bound.

Interpolation already presents one guest frame late: samples show the
transition N-1 -> N only after frame N is complete. The split below uses
exactly that boundary.

## Stages

Per host frame, the loop in `RunOot3dNativeA32Window` does:

1. frame start: SDL events and input, ImGui `StartDraw`, `api.StartFrame`
   (the frame is open before the guest refresh, because dependency flushes
   record into it);
2. guest refresh (when due): guest execution, SVCs, audio, PICA drain, draw
   plans captured into the presentation scheduler's accumulator, dependency
   flushes for raw copies;
3. presentation: top/bottom transfer selection, `FinishFrame`, native frame,
   transition, samples, bottom frontend, N64 UI, ImGui `EndDraw`, screenshot,
   pacing, `window.EndFrame`.

Split:

- **Guest stage (G)**, main thread: 1 without the renderer calls, all of 2,
  and from 3 the parts that read guest state: transfer selection
  (`hostServices` framebuffer and LCD state, `displayTransfersByOutput`),
  `FinishFrame` (the accumulator is fed by G), `PublishNativeActorInteractions`
  (reads guest memory), UI lifecycle queries and UI primitive building, copy
  references to pin.
- **Render stage (R)**, worker thread: every call on `api`, the execution side
  of the presentation scheduler, `previousVisualFrame`/`latestVisualFrame`,
  `preparedVisualTransition`, `visualSampleCadence`, continuity tracker and
  epoch, temporal samples, `GrassInteractionBridge` sample selection, N64 UI
  rendering, screenshot, pacing and `window.EndFrame`.

G hands R one **presentation packet** per host frame, by value:

| Field | Source today |
| --- | --- |
| framebuffer extent | `window.GetWidth/Height` |
| optional new visual frame (moved) | `picaPresentationScheduler.FinishFrame` |
| selected top/bottom transfer copies, LCD black | `hostServices`, `displayTransfersByOutput` |
| native-frontend presentation flags | `uiLifecycleBridge` |
| UI primitive lists per subsystem | `uiLifecycleBridge.Build*Presentation`, ocarina text, free camera option |
| copy references to pin | `submissionQueue` and accumulator pending draws |
| presentation clock, host frame and guest frame counters | loop locals |
| screenshot request | `hostArgs`, `screenshotState` |

R keeps every piece of interpolation state it owns today and consumes packets
in order, so the sequence of renderer calls is the serial sequence.

## Presentation scheduler split

`Oot3dPicaPresentationScheduler` holds both the accumulator (`Capture`,
`FinishFrame`, `ViewDependencyWork`, pending draws) and execution state
(`Execute`, `PresentExisting`, snapshots, executed-draw tracking per
presentation). The accumulator moves to G; execution state to R. Savestates
capture both halves at a barrier.

## Feedback from R to G (synchronization points)

1. **Dependency flush (raw copies).** `flushNativeDependencies` executes a
   native prefix whose raw texture copies read and write guest memory through
   `SetPicaPhysicalMemoryAccess` and signal PPF interrupts. G enqueues the
   flush as an ordered command and blocks until R completes it; R runs the
   memory callbacks while G is blocked. The callbacks assert that they run
   only inside such a flush; anything else is a hard error, not a race.
2. **Savestate save/load, quick save/load.** Barrier: drain R, capture or
   restore both halves, resume.
3. **Graphics settings changes and resize.** They reconfigure the renderer:
   barrier, apply in R, resume. ImGui itself needs no barrier: in this host
   `Gui::ImGuiRenderDrawData`, `DrawGame` and `DrawFloatingWindows` are empty,
   so `StartDraw`/`EndDraw` are CPU-only ImGui state and stay in G.
4. **Deterministic captures (`--screenshot*`), probes, semantic traces.**
   Screenshots are ordered in R. Traces that record at presentation time run
   serially unless proven order-independent.
5. **Errors and shutdown.** Exceptions in R are rethrown in G at the next
   packet handoff or barrier; shutdown drains R.

Queue depth is one packet: G may run guest frame f+1 while R presents frame f.

## Latency

Pipelining adds one presentation frame of latency (16.7 ms at 60 Hz) on top of
the one-guest-frame delay interpolation already has. The option stays off by
default; the intended use is when the serial frame exceeds its budget. An
adaptive policy (enable while over budget) can follow once the trade-off is
measured.

## Qualification

- Deterministic captures in `Interpolated2x` (and `Original30`), serial
  against parallel: byte-identical framebuffers.
- `runtime.json` `visual_interpolation` and `frame_rate` counters identical
  (matched, interpolated and sample draws, samples presented, continuity
  epoch, cadence), plus the guest memory/process fingerprints and PCM.
- Sheik raw copy scene for the dependency-flush path.
- Throughput: ABBA in `Interpolated2x` with the unpaced fixed-delta harness.

## Phases

0. Interpolated-mode ABBA harness (unpaced, fixed 1/60 s delta, no-present).
1. Extract R as a `NativePresentationStage` object fed by an explicit packet,
   still called inline. Qualify: identical captures and counters.
2. Split the scheduler; snapshot copy references into the packet. Qualify.
3. Worker thread, depth-one queue, barriers, serial fallback, option
   `--parallel-presentation` (off by default). Qualify and measure.
4. Transition preparation off the critical path, if R becomes the bottleneck.

## Risks

- The loop is one 9,300-line function with shared locals; phase 1 is the bulk
  of the work and the main regression risk, which is why it is qualified
  serially before any thread exists.
- Hidden renderer or UI state touched from G would become a race. Phase 3
  starts with debug checks that `api` is only called from R.
- Latency, above.
