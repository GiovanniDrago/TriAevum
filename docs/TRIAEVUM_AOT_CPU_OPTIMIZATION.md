# AOT CPU optimization development

Branch: `perf/aot-cpu-hotpaths`, created from `d654122` on 2026-09-27.

## Baseline and scope

The working tree already contained 93 modified tracked files and 17 untracked
paths. Graphics/UI work outside the CPU dependency set remains unstaged and is
not part of these commits. The initial tracked diff is preserved privately at
`I:/_triaevum_work/cpu-dynarmic/branch-baseline.patch`.

The CPU baseline includes memory-observation ABI 3, native per-page write
certificates, alias-safe immutable resource snapshots, corrected AOT state
boundaries, and developer-only replay/qualification tools. These changes and
their completed verification precede this branch; see
[real-input replay](TRIAEVUM_AOT_REAL_INVOCATION_REPLAY.md). They are not a newly
qualified major speedup. The original `TriAevum.exe` and release catalogue are
not replaced by this development work.

The matched reference is the existing ABI-3 module
`I:/_triaevum_work/cpu-dynarmic/v3-control/plugin/triaevum_title_aot.dll`, SHA-256
`5d3777a22304e2715c4e15502fbb1db23c14ca6b9037a7a289000cef9341d6c4`, with the isolated
`TriAevum-cpu-pages-v3b.exe` host. Earlier broad register residency, support-only
ThinLTO and function ordering did not establish a qualified improvement; they
are not defaults for this work.

## Implementation sequence

1. Separate normal typed memory access and block-entry checks from diagnostic,
   checked-memory and observer slow paths. Constant PCs should reach the native
   optimizer without an out-of-line hash calculation.
2. Evaluate flat read-page lookup without dropping write certificates, aliases,
   deferred reads, watched-page traps or precise fault behavior. Change the
   memory ABI if the compiled object layout changes.
3. Implement and qualify exact hardware VFP paths. Preserve guest FPSCR,
   rounding, NaN payload/default-NaN rules, input/output subnormal behavior,
   exception flags, non-fused multiply-accumulate and host FP state.
4. Evaluate, rather than assume, a static Dynarmic-IR backend and legal
   contained-function deduplication. Runtime JIT cache serialization, an
   interpreter fallback and user-install compilation are not product options.

Each implementation boundary has its own commit and evidence entry below.
Correctness qualification and performance qualification remain separate.

## Invariants and acceptance

- Keep block-budget accounting, skip-first notification, filtered/unfiltered
  observers, register/flag reload and observable-exit behavior unchanged.
- Preserve exact reads/writes across page boundaries, protected/unmapped pages,
  watch traps, physical aliases, deferred device writes and pointer escape.
  Same-value writes still invalidate relevant ownership/certificates.
- Keep native PICA ordering, transfers, immutable snapshot bytes and the
  renderer/UI composition contract unchanged.
- A header change requires rebuilding every affected AOT shard with dependency
  identities that include the changed header. No mixed-layout module may load.
- Run owning-boundary checks and actual compiled-module smoke probes. Compare
  deterministic framebuffers, memory/state fingerprints and structural workload
  diagnostics in field and raw-copy scenarios.
- Measure matched candidates with fixed workload, warmup, A/A and ABBA controls.
  Failed noise gates do not establish a speedup or justify repeated sampling
  until a favorable percentage appears.
- No ROM-derived source, IR, captures, savestates, binaries, caches or SDKs are
  committed. Evidence stays private and documentation identifies exact inputs.

## Source-guided investigation

Inspected `fearkov/3dsrecomp` revision
`298836bf1a69ca55d1d36c94989f5c71b69e223b`:
[ABI/helpers](https://github.com/fearkov/3dsrecomp/blob/298836bf1a69ca55d1d36c94989f5c71b69e223b/abi/recomp.h),
[generator](https://github.com/fearkov/3dsrecomp/blob/298836bf1a69ca55d1d36c94989f5c71b69e223b/src/codegen/mod.rs),
[verification](https://github.com/fearkov/3dsrecomp/blob/298836bf1a69ca55d1d36c94989f5c71b69e223b/src/verify.rs).
Small direct-memory helpers and block-local budget checks are useful patterns;
its interpreter-relative performance and NaN-tolerant verification are not a
performance/fidelity oracle for this runtime.

Private investigation: `I:/_triaevum_work/cpu-dynarmic/upstream-study/`.
`findings.json`, `current-codegen.json`, `probe-results.json` and
`probe-commands.json` record the source identities, real disassembly and probes.
The existing intrusive RIP profile has 1,971 samples inside the title module:
613 memory helpers, 293 block entry, 332 scalar FP. Of the block-entry samples,
150 are in prologue/budget/filter, 114 in exact sorted-hook search, five in the
callback/state-transfer segment and 24 at common return. These are not exclusive
CPU-time measurements.

An executed standalone frontend probe translated five real entry blocks
(62 ARM instructions) with Dynarmic v6K, CPSR `0x10`, FPSCR `0`. Its own IR passes
reduced 432 operations to 288, GPR reads 106 to 57, and GPR writes 56 to 36, with
memory-operation counts preserved and donor IR verification completed. It did
not execute guest code, emit a static title module or compare speed against our
optimized machine code. Get/Set elimination must respect observable exits;
the donor itself disables it with `check_halt_on_memory_access`.

A separate executed semantic probe reproduced plain-C addition plus result-FZ
handling and found mismatches against the exact local VFP helper for inexact
flags, positive rounding, subnormal input with FZ and default NaN. This was an
independent reproduction of the arithmetic strategy, not execution of the
upstream library. Hardware acceleration must not copy that weaker contract.

Instruction-address containment found 429 candidates across 12,439 selected
functions, covering 29,102 of 963,498 per-function instruction occurrences.
Containment alone does not prove that entry, return and observation contracts
permit merging the generated function bodies.

## Development record

### Baseline preservation

The initial CPU-only commit preserves the existing ABI-3 and replay work before
new hot-path changes. Historical verification and its explicit process-test
limitation are recorded in the replay document. Unrelated graphics/UI edits,
private evidence and generated title inputs are excluded from the commit.

Fresh branch-baseline checks: the 36 `test_whole_aot_cpp.py` tests pass.
The real generated-DLL boundary probe passes nested return/flags, observer
reload, snapshotless unwind, callee memory fault, conditional return and compiled
page epochs. Receipt: `branch-baseline-boundaries/receipt.json` under the private
CPU evidence root. This does not remove the historical full-process-test limit.
