# TopScreen textures in the base installation

TriAevum's TopScreen profile is the typed port of **SingleScreen 3.0**
(the successor of TopScreen / Single Screen Experience, by M-1 / rlgcarrot,
<https://gamebanana.com/mods/695893>). Since 2026-10-17 SingleScreen 3.0 is
integrated into TriAevum with its author's permission: the package carries its
low-resolution graphics, so installing needs nothing beyond the ROM. TopScreen
2.1.1 is no longer used (its archive is no longer published either).

## User flow

1. Select the supported decrypted `.3ds` or `.cci` ROM in Forge as before.
2. Forge verifies the precompiled module and the original game data.
3. Forge installs the SingleScreen 3.0 UI atlas and Link animations shipped in
   the package. No download, no archive, no network access.
4. The launch profile receives the installed pack and the game is ready.

## What ships, what does not

Shipped, unchanged, from the official `singlescreen_30_25aeb.zip` (28,883,512
bytes, SHA-256 `5668640fec8dc2b47b4516fd0b871c4d7d90ca1c2203acc2a22adb59aaab5993`),
in `resources/topscreen/singlescreen-3.0/` (repository:
`runtime/triaevum_resources/topscreen/singlescreen-3.0/`):

| File | Bytes | SHA-256 | Use |
|---|---|---|---|
| `custom_ui01.ctxb` | 131,144 | `20290e417c56973cd19daa1f61a6c3963f075d61776db3d996954ee3296d07d8` | 3.0 UI atlas (profile texture `oot3d/topscreen/3.0/ui_atlas`) |
| `zelda_link_ext.zar` | 186,404 | `06d60e05c493e86677497eb935b49951d613249c43f896816ae898bb8b005902` | Roll Dash and landing animations |

The files are identical in the archive's EUR, USA and JPN folders. Their notice
is `LICENSES/SingleScreen-3.0.md`; the release audit accepts them only at these
paths with these hashes (`topscreen_singlescreen_asset` role).

Not shipped:
- the archive's IPS patch, ExHeader and ARM payload: TriAevum reimplements the
  behavior as typed host code and never applies them;
- the **high-resolution graphics**, the HD UI atlas
  (`tex1_512x256_1FAF9B5B2ADF6038_13_mip0.png`, 4096x2048) and the HD fonts
  (`ltn16.qbf`, `sys8.qbf`), until their author consents. Their hashes and the
  archive's are forbidden in the package.

## The HD graphics, prepared but not distributed

Forge imports the HD graphics only from an archive the user places beside
`TriAevumForge.exe`, under `mods/` beside it, or in `data/downloads/`, verified
by size and SHA-256; it never downloads one. With the archive present it adds:
- `ui_atlas_hd.png`, passed to the game with `--topscreen-ui-atlas-hd`; it
  applies to the exact native atlas bytes, and a user texture pack that replaces
  the atlas wins;
- `font_coverage.zip`: the archive's `ltn16.qbf` (EUR `message/eu`, USA
  `message/us`) paired with the ROM's font (64x64 coverage cells, density 4,
  native metrics). `sys8.qbf` is not an A4 coverage font and stays native.

Without the archive the game uses the native atlas resolution and native fonts.
When the author consents, shipping them is a packaging change: the same import
reads them from the package instead of the archive.

## Ownership and files

- `tools/triaevum_release/topscreen_assets.py`: verifies the shipped assets,
  builds the O3TU pack, installs the animations, imports the optional HD
  graphics; content-addressed cache with atomic writes.
- `supported_revisions.json`: the archive contract (name, size, hash) for the
  optional HD import; no download URL.
- `tools/oot3d/decomp_support/scripts/build_topscreen_texture_override_pack.py`:
  `build_texture_pack(singlescreen_ui_atlas=...)` and O3TUv2 output.
- `precompiled_titles.py` prepares the pack before transactional activation;
  `forge.py` writes the launch arguments and the runtime receipt.
- Runtime consumers: `tools/oot3d/ui_topscreen/oot3d_top_screen_texture_overrides.*`,
  `tools/oot3d/native_game_runtime/oot3d_native_ui_texture_provider.*`. The
  Items hint (SELECT ?) and the D-pad cycle marks draw from the shipped
  custom_ui01 atlas; nothing reads the retired 2.1.1 menu atlas.

Private outputs live in `data/mods/topscreen/<source-key>/`:
`atlas_overrides.o3tu`, `zelda_link_ext.zar`, `import.json`, and with the HD
archive `ui_atlas_hd.png`, `font_coverage.zip` and `fonts_import.json`.
