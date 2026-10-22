# GitHub Copilot Instructions For TriAevum

TriAevum is a native Ocarina of Time 3D runtime and renderer. The repository
contains host source, build scripts, documentation, and generated process
scaffolding. It never contains ROMs, decrypted title source, game assets, saves,
or private runtime data.

## Before Changing Code

1. Read the relevant README section and the active specification under `specs/`.
2. Check whether the change is host-side C++ or requires title/guest data.
3. For non-trivial behaviour changes, use the Spec Kit flow instead of editing
   directly: `/speckit-specify`, `/speckit-plan`, `/speckit-tasks`,
   `/speckit-implement`, `/speckit-converge`.

## Build And Verification

- The Linux developer helper is `scripts/build-and-run-linux.sh`. It requires a
  user-supplied decrypted ROM and network access on the first configure.
- Never place a ROM, save, build directory, cache, or personal path in the
  repository. Use the helper's environment variables or `XDG` directories.
- For shell changes run `bash -n <script>`.
- For Python changes run `python3 -m py_compile <script>`.
- For C++ changes build the affected target with the repository's CMake/Ninja
  configuration and run `git diff --check`.
- Runtime verification requires a legal user-supplied ROM. Report the scene and
  action tested; do not claim runtime verification from a compile alone.

## Safety And Compatibility

- Preserve upstream licences, third-party notices, and source offers.
- Do not modify `main` history or add personal feature commits to it.
- Validate guest actor pointers, actor-list membership, model context, and
  lifecycle before host-to-guest calls. Fail closed on invalid state.
- Do not add secrets, tokens, personal contact data, or machine-specific paths.
- Keep optional features functional when title data, ROM data, or GPU features
  are unavailable.

## Spec Kit

Spec Kit is pinned to `specify-cli 1.2.3`; see `docs/SPEC_KIT.md`. Generated
agent skills live in `.github/skills/` and shared scaffolding in `.specify/`.
Do not hand-edit generated skills; customize through reviewed templates or
presets.
