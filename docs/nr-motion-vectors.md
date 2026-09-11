# NR motion-vector units

`DlssNrFrame.motion_uv_scale_x/y` convert stored current-to-previous vectors
into UV displacement over the full view. Streamline supplies this convention
directly. Native NGX and the DX11 transport divide NGX `MV.Scale` by the original
render dimensions, independently of the motion texture resolution.

NVIDIA's [Streamline DLSS adapter](https://github.com/NVIDIA-RTX/Streamline/blob/main/source/plugins/sl.dlss/dlssEntry.cpp#L670)
performs the opposite conversion: normalized scale times render dimensions.

NR normalizes its supplied `MVecScale` by the declared motion subrect size.
Consequently, for either axis:

```
processing_pixel_scale = motion_uv_scale * processing_extent
MVecScale = processing_pixel_scale * motion_subrect_extent / region_extent
```

The user motion multiplier applies to the UV scale. The processing extent is
the full render extent in Before mode and the full display extent in After
mode. The motion subrect extent is the actual integer extent sent to NR,
including when the DX11 transport has already copied a crop into a private
texture. Use the original full render dimensions to normalize the transport's
NGX scale, not the copied depth or motion dimensions.

Color working resolution does not enter this formula: resizing color does
not resize the guide or change its displacement units. Crop-origin compensation
divides the crop movement in processing pixels by `processing_pixel_scale`,
producing an offset in stored-vector units.

## Runtime evidence

Verified against installed NVIDIA NR 310.8.0.0, SHA-256
`e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e`.
Image addresses below assume preferred image base `0x180000000`:

- `0x18001a8ba` reads `DLSSNR.MVecScaleX` into parameter offset `0xd8`;
  Y goes into `0xdc`. Both default to one.
- `0x180022278` through `0x1800222a7` select the declared motion subrect
  extents, falling back to backing resource extents when zero.
- `0x18002257e` through `0x18002259e` divide Y and X scales by those effective
  extents. The normalized factors are passed to `0x18003f490`.

This independently corroborates the host-side normalization described in
[MLX-DLSS's preprocessor research](https://github.com/iamwavecut/MLX-DLSS/blob/main/docs/research/2026-08-31-dlssnr-first-frame-preprocessor.md).
The previous working-resolution multiplier and direct pass-through of native
and Streamline scales predated PR 29; old behavior is not the reference.

Contract tests check scene displacement across processing orders, guide sizes,
crops, working scales, signed normalized vectors, and moving crop origins.
Native/Streamline integration tests observe the actual NR parameter calls.
These checks do not replace an in-game visual comparison of temporal quality.

## Exact guide preparation and jitter

NR now receives private motion and depth textures at its working size. The
sampling position is calculated from the full-view crop origin and pixel
center, not by stretching an enclosing integer guide subrect. A transport copy
retains the original full guide extent and subtracts its copied origin. Integer
wide-product comparisons avoid floating-point floor errors at texel boundaries.
Motion is point sampled; invalid vector sentinels are preserved.

Private motion values are already normalized to the NR crop. The runtime
MVecScale is therefore the working extent; NR divides by the identical private
motion extent. The earlier formula above describes the original guide-domain
conversion; it is not the scale written for the new normalized private texture.

Per-view history retains the jitter of the last successful NR evaluation.
For current-to-previous screen-axis jitter, the added UV displacement is
`(before - motion_vectors_jittered) * (previous_jitter - current_jitter)`.
It adds jitter for unjittered vectors with Before color, leaves already-jittered
Before vectors alone, and removes jitter from jittered vectors with After color.
Crop-origin displacement and jitter are independent of user motion multipliers.
Reset/failure breaks history, and non-finite jitter is rejected rather than sent
to the runtime. Native NGX, DX11 transport and Streamline all carry these fields.

Both private guides live in the existing six-entry NR resource cache and use
its recording/replay/fence lifetime. Source identities are included in cache
matching so descriptors referenced by pending recordings are never rewritten.
The guides cost 12 bytes per working pixel (RG32F motion plus R32F depth); active
intermediate VRAM diagnostics include that cost. No new unbounded pool is used.
