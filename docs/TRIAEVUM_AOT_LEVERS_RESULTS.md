# CPU/AOT levers: implementation and measurement

Branch `perf/aot-levers`, based on `807b66e`. Developer work only: the
release catalogue, installed modules and Forge are unchanged.

## Measurement protocol

- Field checkpoint `J:/TriAevum-verify-20260910/win-field`, neutral input,
  `native30_no_interpolation`, `--throughput-benchmark`, 900 updates per run,
  last 720 measured, alternating ABBA pairs (`measure_prebackend.py`,
  `summarize_aot_abba.py`).
- `--no-present` (commit "add a no-present developer throughput mode"). The
  test display is limited to 30 Hz, and earlier runs on this branch were paced
  by it through the frame fence, not by game pacing, VSync or GPU clocks.
  Results measured before this mode are not throughput results.
- GPU clocks locked at 1800-2100 MHz for the measured campaigns.
- The host carried unrelated background load (about 4-6 busy logical cores,
  mostly idle agent sessions). A/A spread under that load is about +/-5% per
  pair; single-pair differences below that are not claimed.
- Control module: current generator, 256 affinity shards, no instrumentation.
  Every comparison keeps the same host executable in both arms unless stated.

## Results

| Lever | Change | Pre-NRI ratio (median of 4 pairs) | Guest ratio | Fingerprint | Decision |
| --- | --- | ---: | ---: | --- | --- |
| Host helper fast memory | exact single-page `ReadBytes`/`WriteBytes` forms, product skips overlay stub | 0.999 (0.94-1.04) | - | identical | kept: exact, no measurable gain |
| PGO | instrumentation profile from 7 non-Field workloads, `-fprofile-instr-use` | **0.864** (0.836-0.871) | **0.822** | identical | **candidate for promotion** |
| Audio kernels, memory-region lowering | `--memory-region` for `00466E2C`, `004A022C`, `004A0338` (views, local registers, local budget) | 0.996 (0.98-1.03) | 0.997 | identical | rejected: no gain; hand-written natives of the same shape are not pursued |

PGO whole-frame ratio 0.901 (about 110 to 122 fps in this configuration).

### PGO qualification

| Check | Result |
| --- | --- |
| Field, 360 updates, captures at 120/240 | both framebuffers byte-identical to the control; memory content/state, process state, 34,200 draws, 720 transfers, empty queues and PCM FNV identical |
| Sheik raw copy, 400 updates, captures at 120/240/360 | all three framebuffers byte-identical; memory/process state, 41,671 draws, 1,118 transfers, empty queues and PCM identical |
| Cross-check with history | memory fingerprints equal the 2026-09-27 ABI-3 qualification (`6672342775686176555`, `17615798149623760801`) |
| Untrained scene (`native-child-field`, ABBA, no-present) | pre-NRI **0.872** (0.868-0.880), guest **0.834**, whole frame 0.908; fingerprint identical |
| Startup | process CPU including startup 10.55-10.94 s (PGO) versus 11.36-13.42 s (control) per run; no load-time penalty observed |

The PGO module is 197,840,896 bytes versus 87,777,792 for the control. This is
the only open release question besides builder support: the profile and its
receipt become a versioned developer input of the precompiled title module.

### PGO training set

`practice-windmill`, `practice-meadow`, `temple-sword`, `ocarina-final-abi`,
`win-coldboot`, `practice-tomb-forward` (all `J:/TriAevum-verify-20260910`)
and Sheik raw copy with `native_cutscene_dialogue_advance.json`, 900 updates
each. `win-items` was excluded: the checkpoint no longer restores on this host
("display transfer source has no Vulkan render target"). The measured Field
scene is deliberately not in the training set. Merged profile SHA-256
`7804e25b3d311100fd8b72525cd2c7820b482705e782e67bf75616366eac28ce`.

## Reproduction

```powershell
# instrumented module, profile, profiled module
build_aot_header_experiment.py --generated <gen> --output <gen-module> ... --pgo-generate
collect_aot_profile.py --invocation <template> --executable <host> --plugin <gen-module dll> \
  --workload <checkpoint>[::<timeline>] ... --llvm-root <llvm> --output <profile-dir>
build_aot_header_experiment.py --generated <gen> --output <use-module> ... --pgo-use <profile-dir>/title.profdata
measure_prebackend.py --invocation <template> --executable <host> --baseline-plugin <control> \
  --comparison-plugin <candidate> --runs 8 --frames 900 --no-present
```

Private evidence: `I:/_triaevum_work/perf-levers/` (runs, profiles, modules).
Its `DISPOSABLES.md` lists every artifact and its removal condition.

## Not yet done

- Release-builder PGO support and a decision on the module size.
- PICA plan/queue off the guest critical path; block-entry contract changes;
  further call-free leaf functions.
