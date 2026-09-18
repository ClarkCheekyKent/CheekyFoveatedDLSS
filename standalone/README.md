# Cheeky standalone and OptiScaler integration

These two packages use Cheeky's shared processing runtime and desktop settings
overlay. The standalone package loads through `dxgi.dll`; the OptiScaler package
loads as an ASI plugin through an existing OptiScaler installation. Neither needs
ReShade or the Cheeky UEVR plugin.

This is an experimental Windows x64 integration. Actual game support depends on
the graphics loading path and whether Cheeky can observe the game's DLSS calls.
Integration tests cover the loader, host and supported graphics paths; they do
not establish compatibility with every game or third-party hook combination.
They use fake NGX/Streamline inference with real graphics resources and GPU
readback; actual NVIDIA inference and headset behavior still need game testing.

## Standalone installation

1. Exit the game. Locate the directory containing the executable that renders
   the game (often `<game>\Binaries\Win64` in Unreal games).
2. Extract the **Standalone** package there, preserving this layout:

   ```text
   Game.exe
   dxgi.dll
   CheekyFoveatedDLSS/
     CheekyFoveatedDLSSHost.dll
     CheekyFoveatedDLSSRuntime.dll
   ```

3. Start the game with its D3D11 or D3D12 renderer and enable DLSS.

**If `dxgi.dll` already exists, do not overwrite it.** For an OptiScaler install,
use the ASI package below. For another DXGI mod, see optional chaining below.
A game that bypasses the local `dxgi.dll` will not load this package.

### Optional second DXGI mod

With the game closed, back up the other mod's `dxgi.dll`, then rename it to
`dxgi2.dll` and put Cheeky's `dxgi.dll` beside it. Keep the other mod's remaining
files in their original locations. Do not rename a second copy of Cheeky.

```text
Game.exe
dxgi.dll                 (Cheeky)
dxgi2.dll                (the other mod)
CheekyFoveatedDLSS/
```

On the first DXGI call, Cheeky loads `dxgi2.dll` from its own directory and
forwards the three factory exports, `DXGIGetDebugInterface1`, and
`DXGIDeclareAdapterRemovalSupport` through it. Missing exports and all other
exports use Windows DXGI directly: some mods expose unusable private-export
stubs when renamed. If the file is absent or cannot load, Cheeky continues using Windows DXGI.
`CheekyFoveatedDLSS-Loader.log` beside `dxgi.dll` records the result and Windows
error code (also sent to debug output). Restart after changing either DLL.

This is a loading mechanism, not a guarantee of compatibility with another mod.
The second mod must tolerate renaming. Factory creation, debug-interface and
adapter-removal-support calls that reenter Cheeky through another mod's hook
bypass the chain and use Windows DXGI. If that path also loops back, Cheeky
returns `DXGI_ERROR_INVALID_CALL` instead of overflowing the stack. This guard
covers VRPerfKit's five hooked DXGI exports; arbitrary private exports are not
covered. Luke Ross/R.E.A.L. VR compatibility still
requires testing in the specific game. To undo chaining, remove Cheeky's loader
and restore the other mod's original `dxgi.dll` filename.

## OptiScaler installation

1. Keep your working OptiScaler installation and close the game.
2. Extract the **OptiScaler** package beside the game executable. Its default
   layout is:

   ```text
   Game.exe
   OptiScaler.ini
   <existing OptiScaler loader DLL>
   OptiScaler/
     plugins/
       CheekyFoveatedDLSS.asi
       CheekyFoveatedDLSS/
         CheekyFoveatedDLSSHost.dll
         CheekyFoveatedDLSSRuntime.dll
   ```

3. Set `LoadAsiPlugins=true` in the existing `[Plugins]` section of
   `OptiScaler.ini`. If you set a custom `Path`, put the ASI and its sibling
   `CheekyFoveatedDLSS` folder in that directory instead. Preserve the rest of
   your OptiScaler configuration.
4. Start the game and use OptiScaler's **DLSS output backend**. Cheeky operates on
   DLSS evaluation calls; selecting FSR or XeSS output does not give those
   backends Cheeky's foveated DLSS processing.

This package contains no OptiScaler binaries and does not replace OptiScaler's
loader. The ASI uses the `InitializeASI` entry point from
[OptiScaler's plugin loader](https://github.com/optiscaler/OptiScaler/blob/master/OptiScaler/dllmain.cpp).
The folder and enable option follow
[OptiScaler's configuration](https://github.com/optiscaler/OptiScaler/blob/master/OptiScaler.ini).
Use the regular ASI filename: delayed loading can miss the game's initial D3D12
swap-chain creation.

## Settings and diagnostics

Press **F8** to open or close Cheeky's desktop overlay. Its tabs provide DLSS-SR,
gaze/stereo, DLSS-NR and diagnostics controls. Changes are saved automatically.
Pause the game when changing settings if it continues to react to keys while
the menu is open; some games poll input outside Windows message handling.

Settings are saved to `CheekyFoveatedDLSS.ini` inside the nested
`CheekyFoveatedDLSS` directory, alongside the host and runtime DLLs. This is also
where runtime logs and support reports are written. Settings are shared by the
runtime, but each installation has its own settings file.

Both hosts expose the shared settings, including D3D11 direct/transport, center
and peripheral SR, full/foveated NR before or after upscaling, gaze, calibration,
presets and diagnostics. The **All settings** tab provides the complete public
configuration field set. D3D11 NR uses the existing D3D12 transport path.
The overlay renders SDR, scRGB and HDR10 with the game's presentation color
space; HDR10 alpha blending is approximate.

DLSS-NR is experimental and off by default. Its compatible NVIDIA and Streamline
runtimes are supplied separately. For these packages, put `nvngx_dlssnr.dll`
beside the nested `CheekyFoveatedDLSSRuntime.dll` or the game executable; keep Streamline components in
the locations expected by the game. D3D11 requires **DX12 Transport** for NR.
See the [NR settings reference](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/blob/master/USAGE.md#experimental-dlss-nr-support)
for tuning and runtime compatibility notes.

If NR does not activate in D3D11, enable `D3D11D3D12Transport` in **All settings**.
The private DX12 path explicitly registers the loaded DLSS library's directory
when the game's NGX initialization paths are unavailable. A log entry such as
`D3D12 canonical create ... result=0xBAD0000B` means that private DLSS feature
creation failed before after-upscaling NR could run; it is not evidence of a
missing NR DLL. Include the nested runtime log when reporting this failure.

The resident runtime enforces one active Cheeky host per process. Install one
Cheeky loader for a game: standalone, OptiScaler ASI, UEVR plugin, or ReShade
add-on. Restart the game after changing loaders or DLLs. The loader, host and
runtime intentionally stay resident until game exit.

The desktop overlay is not an in-headset VR menu. OpenXR gaze and calibration
still use the separate Cheeky OpenXR layer; use its matching installer when
needed. This package does not turn a flat game into VR.
UEVR-specific AFW projection and rendering-mode callbacks remain available
through the Cheeky UEVR plugin. Use that integration for its AFW host controls.
Native OpenVR uses the game's existing OpenVR session and the built-in adapter;
it does not require the OpenXR layer.

## Loader behavior and current limits

- All 20 named exports and their ordinals from the tested Windows DXGI library
  are forwarded through optional `dxgi2.dll`, with the absolute System32 DXGI
  path as fallback. A missing Cheeky host does not disable DXGI forwarding.
- Cheeky host loading and graphics initialization run on a separate worker. Successful
  standalone factory creation waits up to ten seconds for this startup to
  finish before returning the factory to the game. Recursive factory creation
  by the host bypasses that wait. Failed DXGI calls retain their original error.
- ASI startup is asynchronous and duplicate `InitializeASI` calls are harmless.
  A D3D12 swap chain created before the host hooks are ready may not provide a
  usable presentation queue. In that case, restart with normal early ASI
  loading; the host must observe a supported swap-chain creation path.
- Chain selection is recorded in `CheekyFoveatedDLSS-Loader.log`; other loader
  startup diagnostics are sent to the Windows debug output. If no host
  log appears, check the nested DLL layout and whether the selected loader was
  loaded at all.
- Native NGX, Streamline, OptiScaler backend changes, multiple swap chains,
  HDR, and other injected overlays should be validated in the target game.

## Uninstall or update

Exit the game first. Remove the **Cheeky** loader you installed (`dxgi.dll` only
if it is Cheeky's, or `CheekyFoveatedDLSS.asi`) and its sibling
`CheekyFoveatedDLSS` directory. Keep that directory's INI if you want your settings
for a later install. Leave OptiScaler's loader and configuration in place.

Build packages from the repository with:

```powershell
.\scripts\build.ps1 -Configuration Release
.\scripts\package-standalone.ps1 -Configuration Release -Mode Both -Label my-build
```

Packaging verifies each ZIP entry against its staged SHA-256 hash and refuses
to overwrite an existing archive. `-BinaryRoot` can point to a CMake output
directory. `-IncludeOpenXRSetup` includes the separately built matching OpenXR
installer.
