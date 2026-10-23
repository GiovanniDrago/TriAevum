## TriAevum Parallel Task Metadata (Project Override)

This project requires task metadata that is precise enough for a parent agent to
schedule independent work without file conflicts.

For every generated task, include:

- **Files**: the exact repository-relative paths the task may create or modify;
- **Depends on**: the task IDs that must be complete first, or `none`;
- **Verify**: `static` or `game`;
- **Scenario** and **Expected**: required when `Verify: game`;
- the existing task ID, `[P]` marker when applicable, user-story label, and action.

Example:

```text
- [ ] T012 [P] [US1] Create entity model — Files: src/models/a.py, src/models/b.py; Depends on: T010; Verify: static
- [ ] T020 [US1] Fix P2 spawn — Files: tools/oot3d/native_game_runtime/oot3d_coop_dog.cpp; Depends on: T012; Verify: game; Scenario: coop-p2-smoke; Expected: dog spawns after Start and remains usable
```

Rules:

1. A task with unknown or unbounded file ownership must use `Files: TBD` and
   must not carry `[P]`.
2. `[P]` is valid only when all of these are true:
   - the task is in the current dependency phase;
   - its prerequisites are explicit;
   - its `Files` set is disjoint from every other `[P]` task in the same round;
   - it does not edit `tasks.md`, generated artifacts, build outputs, or shared
     configuration unless those files are explicitly listed.
3. Tasks that touch the same file, or whose file ownership cannot be
   determined, remain sequential even if the core template would mark them
   parallel.
4. Keep phase boundaries: Setup → Foundational → user stories → Polish.
5. Tests that must fail before implementation remain ordered before their
   implementation tasks.

Use `Verify: game` for any task that changes gameplay, rendering, UI, input,
coop runtime, or a scenario fixture. Use `Verify: static` only when the task
cannot affect runtime behaviour. If a task's verification class is uncertain,
choose `game`.

Do not invent file paths. If the plan or contracts do not determine the exact
paths, keep the task sequential and say so in the task description.
