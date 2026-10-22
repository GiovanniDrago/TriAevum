# Spec Kit On The Personal Fork

This repository uses GitHub Spec Kit for specification-driven development. The
generated project files are tracked; per-checkout state is ignored by
`.specify/.gitignore`.

## Pinned Tooling

The project was initialized with:

```text
specify-cli 1.2.3
integration: opencode
script type: sh
```

Install the pinned CLI with `uv`:

```sh
uv tool install specify-cli==1.2.3
specify --version
```

The OpenCode integration installs process commands under
`.opencode/commands/`. Shared templates, scripts, workflows, and the constitution
live under `.specify/`.

## Branch Model

- `main` mirrors the upstream `coccofresco/TriAevum` repository and is kept free
  of personal feature commits.
- `develop` is the integration branch for this fork and contains the Spec Kit
  setup.
- Feature work starts from `develop` on a dedicated branch.

`origin` is the personal fork and is the default remote for fetch, push, and
GitHub CLI operations. `upstream` is the original `coccofresco/TriAevum`
repository, kept for read-only synchronization and comparisons; its push URL is
deliberately disabled.

## Workflow

Run the Spec Kit commands inside OpenCode, one step at a time, reviewing each
artifact before continuing:

```text
/speckit.constitution   # only when project principles change
/speckit.specify        # define the next bounded change
/speckit.clarify        # optional, before planning
/speckit.plan
/speckit.tasks
/speckit.analyze        # optional consistency gate
/speckit.implement
/speckit.converge
```

Features are written under `specs/<feature>/`. Keep specifications, plans, and
tasks in version control. Do not commit local feature pointers, extension
configs, caches, or agent credentials.

## Parallel Task Execution

The project-local preset `presets/triaevum-parallel/` is installed into
`.specify/presets/triaevum-parallel/` and adds mandatory rules to
`/speckit.tasks` and `/speckit.implement`.

- `/speckit.tasks` must emit exact `Files:` and `Depends on:` metadata.
- Only tasks with disjoint file ownership may be marked `[P]`.
- `/speckit.implement` must launch OpenCode subagents for eligible `[P]` tasks,
  at most four lanes per round.
- Subagents only edit their listed files. They must not edit `tasks.md`, commit,
  push, or change branches.
- The parent agent owns `tasks.md`, Git, integrated build/tests, and final
  verification.
- If subagents are unavailable, implementation stops instead of silently
  running parallelizable tasks in sequence.

The preset source is versioned separately from the installed copy so Spec Kit
upgrades can be reviewed. Reinstall it with:

```sh
specify preset update triaevum-parallel --dev ./presets/triaevum-parallel
```

`specify integration status` may report the two preset-modified command files
(`speckit.tasks` and `speckit.implement`) as modified managed files. This is
expected: the preset deliberately prepends reviewed orchestration rules to the
core commands. Review the diff before upgrading or refreshing the integration.

## Reproducibility Rules

- Do not commit ROMs (`*.cci`, `*.3ds`, `*.z64`, `*.n64`, `*.v64`), saves,
  runtime profiles, logs, build outputs, or caches.
- Do not hard-code `/home/<user>` paths, GPU models, controller names, display
  servers, or ROM locations in tracked files.
- Use `$XDG_DATA_HOME`, `$HOME`, and the helper's documented environment
  variables for local state.
- Keep the source tree runnable from a clean clone with a user-supplied
  decrypted ROM and the public release inputs.
- Preserve upstream licences, third-party notices, and source offers.

## Updating Spec Kit

Pin and review every update:

```sh
uv tool install --force specify-cli==<version>
specify integration upgrade opencode
```

Review the generated `.specify/`, `.opencode/commands/`, and preset diff before
committing. Do not hand-edit generated commands or preset installations; update
`presets/triaevum-parallel/` and reinstall the preset instead.
