# TriAevum Agent Instructions

TriAevum is a native Ocarina of Time 3D runtime and renderer. The repository
contains host source, build scripts, documentation, and generated process
scaffolding. It never contains ROMs, decrypted title source, game assets, saves,
or private runtime data.

## Spec Kit And OpenCode

Spec Kit is configured for the OpenCode integration. Process commands live in
`.opencode/commands/` and are invoked as dotted commands:

```text
/speckit.specify
/speckit.plan
/speckit.tasks
/speckit.implement
/speckit.converge
```

The project-local preset `presets/triaevum-parallel/` adds mandatory task
metadata and parallel subagent execution. Its installed copy is under
`.specify/presets/triaevum-parallel/`; do not hand-edit generated command files.

## Parallel Implementation

When `/speckit.implement` finds independent `[P]` tasks with disjoint file
ownership:

- use the OpenCode `subagent` tool with `background: true`;
- launch at most four lanes per round;
- give each subagent a closed file allow-list and the relevant spec/plan/task
  context;
- do not let subagents edit `tasks.md`, commit, push, or change branches;
- the parent is the single writer for `tasks.md`, Git operations, integrated
  build/tests, and final verification.

If subagents are unavailable, stop and report instead of silently running
parallelizable tasks sequentially. Do not mark a task complete until the parent
has verified its diff and checks.

## Runtime Gameplay Verification

Use the `game-tester` subagent for any task marked `Verify: game` or any task
that changes gameplay, rendering, UI, input, coop runtime, or a game-test
fixture. The parent must invoke it through the `subagent` tool with
`agent: "game-tester"` and wait for a structured report.

The verifier loads `game-test-session` and uses `tools/game_test/run_case.py`
with the package/profile/state supplied through environment variables. It
captures the framebuffer, inspects `runtime.json` and `coop_probe.jsonl`, and
reports PASS, FAIL, or BLOCKED. A task cannot be marked complete without a PASS
report containing evidence paths. Game runs are serialized; do not launch
multiple instances against the same display, package, or save directory.

See `docs/GAME_TESTING.md` for the scenario and input contracts.

## Build And Verification

- The Linux developer helper is `scripts/build-and-run-linux.sh`. It requires a
  user-supplied decrypted ROM and network access on the first configure.
- Never place a ROM, save, build directory, cache, or personal path in the
  repository. Use the helper's environment variables or `XDG` directories.
- Shell changes: run `bash -n <script>`.
- Python changes: run `python3 -m py_compile <script>`.
- C++ changes: build the affected CMake/Ninja target and run
  `git diff --check`.
- Runtime verification requires a legal user-supplied ROM. Report the scene and
  action tested; a compile alone is not runtime verification.

## Safety And Compatibility

- Preserve upstream licences, third-party notices, and source offers.
- Do not modify `main` history or add personal feature commits to it.
- Validate guest actor pointers, actor-list membership, model context, and
  lifecycle before host-to-guest calls. Fail closed on invalid state.
- Do not add secrets, tokens, personal contact data, or machine-specific paths.
- Keep optional features functional when title data, ROM data, or GPU features
  are unavailable.
