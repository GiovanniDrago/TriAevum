# Spec Kit On The Personal Fork

This repository uses GitHub Spec Kit for specification-driven development. The
generated project files are tracked; per-checkout state is ignored by
`.specify/.gitignore`.

## Pinned Tooling

The project was initialized with:

```text
specify-cli 1.2.3
integration: copilot
script type: sh
```

Install the pinned CLI with `uv`:

```sh
uv tool install specify-cli==1.2.3
specify --version
```

The GitHub Copilot integration installs skills under
`.github/skills/speckit-*/SKILL.md`. Shared templates, scripts, workflows, and
the constitution live under `.specify/`.

## Branch Model

- `main` mirrors the upstream `coccofresco/TriAevum` repository and is kept free
  of personal feature commits.
- `develop` is the integration branch for this fork and contains the Spec Kit
  setup.
- Feature work starts from `develop` on a dedicated branch.

The upstream remote is `origin`; the personal fork is `fork`. This keeps upstream
history available for rebases and comparisons.

## Workflow

Run the Spec Kit skills inside GitHub Copilot or another supported agent, one
step at a time, reviewing each artifact before continuing:

```text
/speckit-constitution   # only when project principles change
/speckit-specify        # define the next bounded change
/speckit-clarify        # optional, before planning
/speckit-plan
/speckit-tasks
/speckit-analyze        # optional consistency gate
/speckit-implement
/speckit-converge
```

Features are written under `specs/<feature>/`. Keep specifications, plans, and
tasks in version control. Do not commit local feature pointers, extension
configs, caches, or agent credentials.

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
specify integration upgrade copilot
```

Review the generated `.specify/` and `.github/skills/` diff before committing.
Do not change integration layouts or hand-edit generated skills without a
documented reason.
