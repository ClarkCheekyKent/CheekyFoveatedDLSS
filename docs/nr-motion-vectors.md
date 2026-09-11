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
