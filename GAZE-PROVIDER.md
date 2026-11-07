# Gaze provider interface for VR mods

Cheeky normally gets gaze from its OpenXR layer (or its OpenVR / LibOVR
adapters) and works out which DLSS view is which eye with marker calibration.
A VR mod that owns the headset session itself — a flat-to-VR mod that renders
the stereo pair through the game, often one eye per frame — can't use the
OpenXR layer: its session isn't one the layer sits in front of, and marker
calibration can't tell alternating single-eye renders apart on D3D12. The mod
already knows both answers, so it can give them to Cheeky directly.

The interface is two C exports from any DLL loaded in the game process, for
example the mod's `dxgi.dll` proxy. Declarations are in
`shared/cheeky_gaze_provider.h`.

## `CheekyGazeProvider_GetGazeSnapshot` (required)

```c
uint32_t __cdecl CheekyGazeProvider_GetGazeSnapshot(uint32_t requested_version, void* output, uint32_t output_size);
```

The same contract as the layer's `CheekyOpenXR_GetGazeSnapshot`
(`shared/cheeky_gaze_abi.h`): fill a `CheekyGazeSnapshotV1` for
`CHEEKY_GAZE_ABI_VERSION` and return nonzero; return 0 for an unsupported
version or a too-small buffer. One view per eye: the gaze point in that eye's
rendered image (`center_u` right, `center_v` down, 0..1), the eye's rendered
FOV and its forward point. Cheeky takes a new sample only when
`predicted_display_time` is nonzero and has changed since the last call, so
advance it with every new gaze sample. Clear `CHEEKY_GAZE_STATUS_GAZE_VALID`
when there is no gaze (tracking lost, or the user chose fixed foveation);
Cheeky then uses its fixed placement.

## `CheekyGazeProvider_GetEyeViews` (optional)

```c
uint32_t __cdecl CheekyGazeProvider_GetEyeViews(CheekyGazeProviderEyeViewsV1* views);
```

Which DLSS view renders which eye: the `NVSDK_NGX_Handle*` the game evaluates
for each eye, as an integer, plus the DLSS output size and the snapshot's
`session_generation`. Cheeky sets `structure_size`; fill the rest, set
`version` to `CHEEKY_GAZE_PROVIDER_EYE_VIEWS_VERSION` and return nonzero.
Return 0 while there is no stereo pair (menus, flat screens).

Cheeky calls it at most every 250 ms and publishes the pair as a calibrated
stereo pair with a full-image mapping, which replaces marker calibration for
those views. A published pair stays verified for 2.5 s, so when the provider
stops reporting, Cheeky falls back to its own calibration shortly after.

## Discovery and lifetime

- Cheeky looks for the exports in every loaded module, when it first needs
  gaze and then at most twice a second while none is found (only after
  modules were loaded or unloaded). A provider can load before or after Cheeky.
- The first module found is used. Cheeky takes a reference to it while it uses
  the exports and releases it when gaze foveation shuts down.
- When `CheekyOpenXRLayer.dll` is loaded, the layer is used and the provider is
  ignored.
- Both functions may be called from render threads, so they must be fast and
  thread-safe. They are never called under the loader lock.
