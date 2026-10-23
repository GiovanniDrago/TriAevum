## TriAevum Parallel Subagent Execution (Project Override)

This section is **mandatory** and applies before the core implementation
workflow. The parent agent is the orchestrator. It owns `tasks.md`, Git
operations, integration, and final verification.

### Non-negotiable rule

If any pending tasks are parallelizable, you **MUST** launch OpenCode
subagents for those tasks. Use the `subagent` tool with `background: true`.

If the `subagent` tool is unavailable, stop and report:

> Parallel task execution requires the OpenCode subagent tool. No parallel
> tasks were started.

Do not silently execute parallel tasks sequentially and do not claim that
subagents were used when they were not.

### Scheduling algorithm

1. Read `tasks.md`, `spec.md`, `plan.md`, and the project constitution.
2. Build a dependency graph using:
   - phase barriers: Setup → Foundational → user stories → Polish;
   - explicit `Depends on: Txxx` fields;
   - test-before-implementation ordering;
   - file mutexes: tasks that touch the same file cannot run concurrently.
3. A task is eligible for the current round only when:
   - its checkbox is unchecked;
   - all dependencies are checked;
   - it is marked `[P]`;
   - its `Files` set is non-empty and disjoint from every other eligible task
     in the round.
4. If `Files` is missing or `TBD`, the task is sequential.
5. Respect a maximum of **4 concurrent subagents**. Group sequential tasks in
   the same lane rather than launching a new agent for every task.
6. Never start a task from the next phase until the current phase's checkpoint
   has been verified.

### Subagent brief

For each lane, call the `subagent` tool with:

- `agent: "general"`
- `description`: a short label containing the task IDs
- `background: true`
- `prompt`: a self-contained brief containing the following

```text
You are implementing one bounded lane of TriAevum task T### (and any explicitly
listed sequential follow-up tasks in this lane).

Read:
- <absolute path to spec.md>
- <absolute path to plan.md>
- <absolute path to tasks.md>
- <relevant contracts/data-model/research files>

Allowed files:
- <exact repository-relative paths>

Forbidden:
- Do not edit any file outside Allowed files.
- Do not edit tasks.md.
- Do not run git add, git commit, git push, or change branches.
- Do not touch ROMs, saves, caches, generated title source, build outputs, or
  personal/runtime data.
- Do not start work belonging to another lane.

Implement the task exactly as specified. Follow the repository constitution and
the existing code style. Run only checks that are local to your lane when
possible; the parent will run integrated build/tests.

Report exactly:
STATUS: DONE | FAILED | BLOCKED
TASKS: T###, T###
FILES CHANGED: ...
CHECKS: commands run and results
BLOCKERS: ...
```

Launch all lanes of the round in parallel. Do not wait for one lane before
starting the next lane.

### Runtime gameplay verification gate

After the static implementation of a round is integrated, check the tasks for
`Verify: game`. Also treat a task as runtime-affecting when its `Files` include
any of:

```text
tools/oot3d/native_game_runtime/
tools/oot3d/ui_*/
tools/three_ds/input/
tools/game_test/
coop_controls
coop runtime
```

When a runtime-affecting task exists:

1. Load the `game-tester` subagent through the `subagent` tool. Use
   `agent: "game-tester"`, `background: true`, and the task's `Scenario` (or
   `coop-p2-smoke` when no scenario is named).
2. Give the subagent the task IDs, expected result, package/profile/state
   environment variable names, and the exact files changed.
3. Wait for the subagent to finish. Do not launch two game instances against
   the same display, package, save directory, or GPU cache.
4. Require a `STATUS: PASS` report with a screenshot path, runtime JSON/probe
   evidence, and the observed result.
5. If the report is `FAIL`, `BLOCKED`, or lacks evidence, do not mark the task
   `[X]` and do not declare the feature complete. Record the failure and ask
   for the missing input when appropriate.
6. Only the parent agent updates `tasks.md` and performs Git operations after
   the gameplay evidence is accepted.

The game-tester is a verifier, not a source editor. It must not edit source,
`tasks.md`, or Git state.

### Parent verification gate

After every lane finishes:

1. Inspect the diff and confirm that each subagent stayed inside its allowed
   files.
2. If a lane edited an unallowed file, stop, revert or repair the change, and
   report the violation before continuing.
3. Run the integrated build and the relevant tests for the round.
4. Only after the checks pass, mark the completed tasks `[X]` in `tasks.md`.
5. Commit and push only from the parent, after verification.
6. If a lane fails or is blocked, do not mark its tasks complete and do not
   advance to the next round. Retry once with a narrower brief or ask the user
   for a decision.

### Progress and completion

- `tasks.md` has a single writer: the parent.
- A round is complete only when all lanes report success and the parent's
  verification passes.
- Continue with the next eligible round until no parallel work remains.
- Then execute the remaining sequential tasks using the core implementation
  workflow.
- Report each round: task IDs, subagent descriptions, files changed, checks,
  and any skipped parallelism with the reason.
