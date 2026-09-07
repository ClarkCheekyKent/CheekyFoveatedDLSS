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
