# OptiScaler-DLSS5 (DLSS 5 Neural Rendering) with TriAevum

2026-10-12. [OptiScaler-DLSS5](https://github.com/Skynizz/optiscaler-dlss5)
(GPL-3.0, unofficial) runs NVIDIA's DLSS 5 Neural Rendering model
(`nvngx_dlssnr.dll`, NGX feature 18, not redistributed) over the output of a
game's DLSS. It is a user-installed mod, not part of TriAevum; nothing of it
is packaged.

## How it attaches

- It loads as `winmm.dll` beside `TriAevum.exe` (the executable imports
  `WINMM.dll`) and intercepts NGX. TriAevum's DLSS runs through NRI's NGX
  upscaler on Vulkan; after each `NVSDK_NGX_VULKAN_EvaluateFeature` the mod
  runs the model on the same command buffer (`EvaluateAfterUpscaleVk`), with
  the colour, depth and motion vectors of TriAevum's DLSS parameters.
- The two NVIDIA extensions the model loads through (`VK_NVX_binary_import`,
  `VK_NVX_image_view_handle`) are already enabled by TriAevum for NGX; the
  mod adds `VK_KHR_buffer_device_address`.
- The pass runs before UI composition, so the HUD is never shown to the model.

## What TriAevum needed

The scene came out black, HUD intact, even at `TransferStrength=0`. NRI makes
every Vulkan image view usable only for its view type, so the DLSS output view
NRI hands NGX was storage-only; the mod samples that view (`gSource`, a
combined image sampler) to read the frame, which reads black, and composes the
result from that copy. `cmake/patches/oot3d_nri_storage_view_sampled.cmake`
keeps the sampled usage on a storage view whose texture is also a shader
resource (the image itself already allows it). Nothing else in TriAevum uses
the extra usage.

## Install (developer layout)

Beside `TriAevum.exe`: `OptiScaler.dll` renamed `winmm.dll`, `OptiScaler.ini`
with `[DlssNr] Enabled=true`, the mod's `nvngx.dll_dlssnr.dll` forwarder, and
`nvngx_dlssnr.dll`. In F1 set anti-aliasing to Upscaler with the DLSS provider
(the mod does nothing without the game's DLSS running). Insert opens the
OptiScaler menu (DLSS 5 tab); F6 cycles optimised / vanilla / off.

## Observations

- The model runs at the DLSS output size in the 3DS framebuffer's native
  orientation (log: `feature up at 720x1280`): it sees the frame rotated a
  quarter turn. It still works; whether an upright frame changes its result is
  untested.
- The output is R16G16B16A16 already tone-mapped and not flagged HDR, so the
  mod takes its display-referred path (`passthrough`).
- TriAevum's D3D12 NGX provider probe at startup also passes through the mod's
  D3D12 path once; harmless.

Lab (Kokiri savestate, warp to Hyrule Field, 1280x720, DLSS Quality): with the
patch the frame is the model's picture (cooler, more saturated grass and sky);
OptiScaler loaded with DLSS 5 off is identical to the game without it.
