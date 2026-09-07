# D3D12 SR array-output regression

After `scripts/build.ps1 -Configuration Release`, run:

```powershell
.\bin\Release\CheekyTests.exe --d3d12-composite
```

This executes the production SR compositor shader on the Windows WARP D3D12
device, without a game or NVIDIA runtime. It verifies the center uses the DLSS
texture, the periphery uses the input texture, output origins are respected,
and every other array slice and mip remains unchanged. Cases cover ordinary
2D textures and two-/four-slice mipmapped textures, including R11G11B10_FLOAT.
When installed, the D3D12 debug layer is enabled and errors fail the test.
The default CheekyTests run also checks the logged MSFS 3024x2836, 12-mip
resource description and the single-slice, single-mip private output contract.

This supports mip zero / slice zero of D3D NGX and Streamline resources. It
does not infer an eye's array slice from its viewport ID or add a nonzero-slice
API. The D3D resource contract currently consumed by the add-on exposes a
native resource and XY extent, not a slice selector. Additional slices in
the allocation remain untouched. This does not enable array outputs for NR.

For MSFS 2024 validation, restart the game with the rebuilt add-on and enter
the same VR mode. With NR off, enable fixed SR foveation and its red border.
Check both eyes, vary width/height, and verify the foveated GPU timing becomes
populated. The log reports `D3D12 SR array output accepted` when allocating
scratch resources for an array output. Repeat a flat-to-VR transition.
Compare with peripheral DLAA off first to isolate the separate PR #4
synchronization rejection. WARP tests do not validate NVIDIA evaluation or
the game's presentation path; those still require this in-game check.

The SR Streamline route also uses a private viewport (host ID | 0x20000000),
separate from the peripheral namespace. Some runtimes reject a second constants
submission for the same frame/viewport. Cropped options, tags, constants and
the evaluation input must all select that private viewport; host constants stay
untouched. Unit coverage exercises host IDs 0 and 32 from the MSFS trace,
preservation of unrelated inputs, and rejection of ambiguous/stale viewport
inputs. This models write-once constants; it does not execute Streamline itself.
In a fresh game log, verify `motion constants applied`, `prepare complete`,
and `original begin foveated=yes`. A failure now logs the constants result code.
# D3D12 gaze copy mapping

The normal test executable checks copy-chain translation, subresource isolation,
ambiguous destinations, destruction, expiry, reversed order, and rejection of
scaled copies. These tests validate the matching policy, not MSFS's rendering path.

For an in-game check, select simulated gaze in normal VR gameplay. Both eye
mapping rows should become stable with the `copy` route, and the alignment borders
should move. The submitted-texture-copy counter confirms observation of eligible
D3D12 copies/resolves; a rising count alone does not prove an eye route exists.
This implementation follows up to four equal-size mip-zero/slice-zero copies,
keeps at most 512 edges for 500 ms, and rejects routes reaching both eyes.
Shader blits, scaled copies and other array slices are not handled by this route.

## Streamline camera projection mapping

ABI 3 adds the OpenXR eye fields of view; update both the add-on and the layer DLL.
The fallback compares the unjittered Streamline perspective projection with both
XR eye frusta. It requires a unique match, matching full-eye output dimensions,
and two distinct snapshot display times. Symmetric or mismatched projections,
missing data, non-perspective matrices and stale/wrong-frame constants cannot
activate this route. Existing resource routes still take precedence.

Constants are cached per viewport and actual frame index (token pointers are reused),
with a 100 ms freshness limit, and exposed only during the corresponding SR prepare.
Matrix and frame-token ABI follow NVIDIA's
[Streamline guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md)
and [public frame token definition](https://github.com/NVIDIA-RTX/Streamline/blob/main/include/sl_core_types.h).
Tests cover handedness, reversed depth, distinct and ambiguous eyes, invalid matrices,
cache isolation/expiry, scoped context restoration, and rejection of the old snapshot ABI.

In MSFS normal VR gameplay, select simulated gaze and show the alignment borders.
Expected: `Snapshot ABI: Yes`, both eye mappings stable with `projection`, and moving
borders. If mapping remains waiting, capture 15 seconds of gameplay: bounded
`Gaze projection` lines show the Streamline and XR tangent comparisons. These tests
validate policy and ABI; they do not establish MSFS compatibility without a game test.
