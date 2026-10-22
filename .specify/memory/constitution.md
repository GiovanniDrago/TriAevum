# TriAevum Constitution

## Core Principles

### I. Source-Only Reproducibility (NON-NEGOTIABLE)

The repository contains source code, build scripts, documentation, manifests, and
legal notices only. Never commit ROMs, decrypted title source, game assets,
saves, runtime profiles, caches, logs, build outputs, credentials, or
machine-specific paths.

Build and developer scripts must use documented environment variables, `XDG`
directories, or `$HOME`-relative defaults. A clean clone plus a user-supplied
decrypted ROM and the public release inputs must be sufficient to reproduce the
documented workflow.

### II. Upstream Compatibility And Licensing

The `main` branch mirrors the upstream project. Personal or experimental work
lives on separate branches and must not rewrite upstream history. Preserve all
upstream licences, third-party notices, source offers, and attribution.

Do not relicence third-party code or redistribute proprietary runtime binaries,
fonts, ROM-derived content, or other assets unless their licence explicitly
allows it. Document every external runtime dependency and its acquisition path.

### III. Guest-Memory Safety

Native runtime changes must validate guest pointers, ownership, and lifecycle
before reading, writing, or calling into guest code. Never drive rendering,
animation, or actor callbacks from an actor that is not present in the current
actor lists or whose model context is not initialized.

Host-to-guest calls must have explicit arguments, a bounded execution policy,
and deterministic failure handling. When validation fails, fail closed and leave
the game in a recoverable state instead of retrying indefinitely or mutating
unverified memory.

### IV. Verification Before Merge

Every non-trivial change is verified with the strongest checks available in the
environment:

- build the affected native target;
- run syntax/static checks for changed shell and Python files;
- exercise the affected runtime path with a legal user-supplied ROM when possible;
- record any verification that could not be performed and the reason.

Do not present a change as runtime-verified solely because it compiled. Runtime
claims must identify the scene, action, and observed result.

### V. Small, Reviewable Changes

Use a specification for features that change user-visible behaviour or cross
runtime boundaries. Keep implementation plans and tasks reviewable, avoid
unrelated refactors, and preserve existing public interfaces unless the
specification explicitly requires a change.

Feature branches are short-lived and merge through a reviewable commit history.

## Additional Constraints

- The supported Linux developer workflow is documented and must not assume one
  user's home directory, GPU model, controller, ROM path, or display server.
- Optional rendering, personalization, and co-op features must degrade safely
  when their inputs, title data, or GPU capabilities are unavailable.
- Do not add secrets, tokens, personal contact data, or machine identifiers to
  tracked files, logs, fixtures, or examples.
- Generated title source and ROM-derived assets remain outside the repository;
  reference them through user-provided paths only.

## Development Workflow

- `main` tracks upstream and is not used for personal feature commits.
- `develop` is the integration branch for the personal fork.
- Feature work starts from `develop` and uses the Spec Kit flow:
  `/speckit-specify`, `/speckit-plan`, `/speckit-tasks`, `/speckit-implement`,
  `/speckit-converge`.
- Pull requests must state which checks were run and which runtime verification
  remains manual.
- Changes touching guest memory, actor lifecycle, rendering, or build scripts
  require an explicit review of failure paths and cleanup.
- Independent tasks marked `[P]` with disjoint file ownership must run through
  the configured subagent lanes. The parent agent remains the only writer of
  `tasks.md` and the only actor allowed to commit, push, or run integrated
  verification. If subagents are unavailable, stop rather than silently
  serializing parallel work.

## Governance

This constitution supersedes informal practices for the repository. Amendments
require a pull request that explains the reason, updates dependent templates or
documentation, and records the version change.

All reviews must verify compliance. Complexity or an exception to a principle
must be justified in the specification or plan and approved by the maintainer.

**Version**: 1.0.0 | **Ratified**: 2026-10-22 | **Last Amended**: 2026-10-22
