# iMMERSE module: licence exception

This directory, `runtime/three_ds_recomp/src/fast/immerse/`, is the iMMERSE
module of TriAevum. It is an explicit exception to TriAevum's licensing: the
GNU General Public License that covers TriAevum's own code (`LICENSE`,
`LICENSE_SCOPE.md`) does not apply to anything in this directory.

## Origin

The module ports MXAO from `Shaders/MartysMods_MXAO.fx` of
[iMMERSE](https://github.com/martymcmodding/iMMERSE), revision
`f57d3afa1ebe5d1fd6152d4f6fb9a2e75bd1d1cb`, by Pascal Gilcher
([Marty's Mods](https://martysmods.com)).

Copyright (c) Pascal Gilcher. All rights reserved.

## Terms

- All rights in MXAO and in this module remain with its original author,
  Pascal Gilcher.
- The module stays under the original iMMERSE licence, reproduced unchanged in
  `LICENSE-iMMERSE.txt`. Nothing in TriAevum relicenses it. The changes made to
  integrate it into TriAevum are part of the module, under the same terms, and
  grant no further right.
- TriAevum integrates and modifies MXAO under the explicit permission of its
  author, granted exclusively to the TriAevum project. The permission is not
  transferable and extends to no one else: receiving TriAevum, its source, or a
  copy or fork of either gives no right to use, modify or redistribute this
  module. Anyone else needs the author's own permission.
- Credit: iMMERSE MXAO by Pascal Gilcher (martysmods.com). The F1 Lighting tab
  shows it with the MXAO controls, and `THIRD_PARTY_NOTICES.md` records it for
  source and binary distributions.

## Boundary

Everything in this directory, and nothing outside it, is under these terms.

| File | Contents |
| --- | --- |
| `mxao_shader.{h,cpp}` | The ported MXAO programs (GLSL) and their constants. |
| `mxao_pass.cpp` | Their NRI execution: resources, descriptor sets, dispatches, the original's parameter mapping. |
| `mxao_pass.spvpack`, `mxao_pass_binaries.{h,cpp}` | The precompiled programs and their embedding. |
| `mxao_settings_panel.cpp` | The MXAO controls of the F1 Lighting tab: the original's options, presets and descriptions. |
| `immerse.cmake` | Adds the module to the runtime library. |
| `tests/` | GPU tests and `build_immerse_pass_artifacts`, the generator of `mxao_pass.spvpack`. |
| `README.md` | How the port works and how it was validated. |
| `LICENSE.md`, `LICENSE-iMMERSE.txt` | This exception and the original licence. |

TriAevum's GPL code reaches the module only through integration points that
TriAevum owns and that name MXAO only to select it: the interface
`include/fast/oot3d/mxao_pass.h`, the panel method
`GraphicsSettingsPanel::DrawMxaoSettings`, the provider choice
`AmbientOcclusionMode::Mxao` with its settings, persistence and capability,
and the effect graph wiring in the Vulkan backend.

The module is compiled only with `THREE_DS_RECOMP_ENABLE_IMMERSE`, on by
default with NRI. With the option off TriAevum builds without it, and the
directory can then be removed: `fast/oot3d/immerse_unavailable.cpp` reports
MXAO as not compiled, the provider stays unselectable, and every other feature
builds, runs and is tested. A fork or redistribution without the author's
permission must be built that way and must not include this directory.

Binary packages built with the module carry this file as
`LICENSES/iMMERSE-MXAO.md` and the original licence as
`LICENSES/iMMERSE-LICENSE.txt`.
