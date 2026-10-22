# Cheeky standalone and OptiScaler integration

These two packages use Cheeky's shared processing runtime and desktop settings
overlay. The standalone package offers `dxgi.dll` and `version.dll` loaders; the OptiScaler package
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
2. Copy the `CheekyFoveatedDLSS` folder and **one** loader from the Standalone ZIP:
   - `dxgi.dll`: default choice; also supports another mod as `dxgi2.dll`.
   - `version.dll`: use when another mod must keep its own `dxgi.dll`, or when
     the game's loading path needs this alternative. Keep the other mod unchanged.

   Do not copy both Cheeky loaders or rename one to the other's filename.
   Example using the DXGI loader:

   ```text
   Game.exe
   dxgi.dll
   CheekyFoveatedDLSS/
     CheekyFoveatedDLSSHost.dll
     CheekyFoveatedDLSSRuntime.dll
   ```

3. For Vulkan, also copy `CheekyVulkanLayer.dll` beside the game executable and
   keep the included `CheekyFoveatedDLSS/Vulkan` folder. The loader enables the
   layer for the game process; no separate layer installer is needed.
4. Start the game with its D3D11, D3D12 or Vulkan renderer and enable DLSS.

**Do not overwrite another mod's loader.** Both choices use the same shared
folder and desktop controls. The game must load the selected local DLL for Cheeky to
start. For OptiScaler, use the ASI package below.

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

Press **F8** by default to open or close Cheeky's desktop overlay. The **General** tab
lets you change the menu key and shows compatibility controls and runtime status.
Its other tabs provide DLSS-SR, gaze/stereo, DLSS-FG, DLSS-NR and diagnostics controls.
Changes are saved automatically. The menu key is saved in `CheekyOverlay.ini`
beside the host DLL; use **Reset to F8** in General to restore the default.
Pause the game when changing settings if it continues to react to keys while
the menu is open; some games poll input outside Windows message handling.
An open menu stays visible when the game loses desktop focus, as long as the
game continues presenting frames. Desktop keyboard/mouse capture and the menu
hotkey apply only while the game has focus; switching apps releases the cursor.

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

The **DLSS-SR** tab compares SR on/off FPS, FPS gain (including percent), and
frame-time change using the same comparison formulas as the add-on. Sample
both modes for about two seconds each in the same scene; each mode retains its
last sample. Standalone uses presentation cadence, while the add-on uses
ReShade's FPS counter. GPU-time savings are a separate comparison. SR and NR
GPU timings, resolutions, and crop pixel percentages live in their respective tabs.

DLSS-NR is experimental and off by default. Its compatible NVIDIA and Streamline
runtimes are supplied separately. For these packages, put `nvngx_dlssnr.dll`
beside the nested `CheekyFoveatedDLSSRuntime.dll` or the game executable; keep Streamline components in
the locations expected by the game. D3D11 requires **DX12 Transport** for NR.
See the [NR settings reference](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/blob/master/USAGE.md#experimental-dlss-nr-support)
for tuning and runtime compatibility notes.

Select `D3D11D3D12Transport` manually in the **DLSS-NR** tab
or **All settings**. The NR toggle and hotkey do not change the transport setting.
The private DX12 path explicitly registers the loaded DLSS library's directory
when the game's NGX initialization paths are unavailable. A log entry such as
`D3D12 canonical create ... result=0xBAD0000B` means that private DLSS feature
creation failed before after-upscaling NR could run; it is not evidence of a
missing NR DLL. Include the nested runtime log when reporting this failure.

The resident runtime enforces one active Cheeky host per process. Install one
Cheeky loader for a game: standalone, OptiScaler ASI, UEVR plugin, or ReShade
add-on. Restart the game after changing loaders or DLLs. The loader, host and
runtime intentionally stay resident until game exit.

For native OpenVR/SteamVR games using D3D11 or D3D12, the F8 menu also appears
as a floating panel in the headset, including the mouse cursor. Point a VR
controller at it to use SteamVR's laser pointer, click/drag and scroll controls.
The stage has a gentle curve and stays anchored in the room. Wide canvases use
up to 120 degrees of curvature; larger canvases scale down to fit.
Drag the title bar or empty window space
with either input to move the same window in both views; resize from its border
or corner. The desktop canvas is placed in front of you on first opening, with
the same left/right window placement as the desktop. Closing and reopening
preserves its position; moving or resizing the menu does not recenter the canvas.
Point away from the UI surface to use the desktop mouse again. F8 still opens the menu in this preview;
the close button works with either input. This headset panel needs testing
in real games. Gaze and simulated gaze remain active while Cheeky's own menu
captures controller input, provided scene frames and gaze samples remain valid;
the SteamVR dashboard still blocks that exception. OpenXR and Vulkan headset
menus are not supported yet. OpenXR
gaze and calibration still use the separate Cheeky OpenXR layer; use its matching
installer when needed. This package does not turn a flat game into VR.
UEVR-specific AFW projection and rendering-mode callbacks remain available
through the Cheeky UEVR plugin. Use that integration for its AFW host controls.
Native OpenVR uses the game's existing OpenVR session and the built-in adapter;
it does not require the OpenXR layer. LibOVR games, including R.E.A.L. VR with
`PreferredAPI2=1` on Pimax, likewise use the game's loaded LibOVR runtime and,
for Pimax eye tracking, its existing PVR session; see
[LibOVR games and Pimax eye tracking](../USAGE.md#libovr-games-and-pimax-eye-tracking).

## Experimental OpenXR DLSS Frame Generation

Standalone and OptiScaler expose the same 2x DLSS-FG processing and OpenXR
presenter as the UEVR package. Install the matching **Cheeky OpenXR support**
from the package's `OpenXR/CheekyOpenXRSetup.exe`, restart the game, and use a
**D3D11 or D3D12 OpenXR stereo** session with native DLSS depth and motion-vector guides.
In F8, open **DLSS-FG** and enable **DLSS Frame Generation (2x)**. FG is off by
default and does not require the Cheeky UEVR plugin or its Lua script.

For **ACC in DX11 through OpenComposite**, keep the game in DX11 and enable its
DLSS upscaler. FG automatically uses Cheeky's private DX12 transport while
enabled, including when foveated SR is off. Full-eye depth and motion guides
are captured independently of the SR crop. The OpenXR session remains D3D11;
shared textures and GPU fences carry scene captures and generated output
between APIs. The matching updated OpenXR installer is required. The direct-XR
output experiment uses compact intermediates on D3D11. This path has GPU
integration coverage; ACC and headset compatibility still require game testing.

Standalone automatically uses hook-free guide matching at `xrEndFrame`; the
UEVR-specific `FgHooklessMode3` and `FgBackbufferFrameId` preferences do not
override that behavior. Each native frame must supply exactly two submitted
eye evaluations. Missing, partial or ambiguous guide intervals retain native
presentation and recover on a later complete stereo frame. Keep **Release game
rendering** at **After generated frame submission** (the default) for serialized
matching. Other release modes allow experimental overlap without native frame
identity proof.

The tab includes full/foveated FG, matching the SR region and gaze placement,
manual region controls, center/peripheral scales, peripheral methods and guide
precision, queue scheduling, capture/storage/output experiments, and reset to FG
defaults. Status, generated frame counts, measured unique output FPS and GPU
timings use the shared presenter diagnostics. Unique FPS excludes repeats.

This path requires the same NVIDIA DLSS-G capability as UEVR. Vulkan,
OpenVR, LibOVR, alternate-eye rendering and flat-screen FG presentation are not
supported. A game that supplies a different guide layout may remain on native
frames; actual NVIDIA inference and headset pacing need game testing.

## Loader behavior and current limits

- The DXGI loader forwards the five public entry points described above through
  optional `dxgi2.dll`; the other exports use System32 DXGI. The VERSION loader
  forwards all 17 exports to System32 VERSION and does not load `dxgi2.dll`.
  A missing Cheeky host does not disable system function forwarding.
- Cheeky host loading and graphics initialization run on a separate worker. Successful
  standalone factory creation waits up to ten seconds for this startup to
  finish before returning the factory to the game. Recursive factory creation
  by the host bypasses that wait. Failed DXGI calls retain their original error.
- VERSION and ASI startup are asynchronous; duplicate `InitializeASI` calls are harmless.
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

Exit the game first. Remove the **Cheeky** loader you installed (`dxgi.dll`,
`version.dll`, or `CheekyFoveatedDLSS.asi`, only if it is Cheeky's) and its sibling
`CheekyFoveatedDLSS` directory. Keep that directory's INI if you want your settings
for a later install. Leave OptiScaler's loader and configuration in place.

Build and package standalone in one command, including the matching OpenXR
installer:

```powershell
.\scripts\build-standalone-package.ps1 -Label my-build
```

This builds Release, runs the tests, builds the OpenXR installer, and creates
the verified ZIP in `bin/Release`. Use `-Mode OptiScaler` or `-Mode Both` for
the ASI package or both packages. Like the UEVR wrapper, it accepts `-SkipTests`
and `-IsccPath` for a custom Inno Setup compiler location.

To package binaries that have already been built, use
`.\scripts\package-standalone.ps1 -Configuration Release -Mode Standalone -Label my-build`.
Packaging verifies each ZIP entry against its staged SHA-256 hash and refuses
to overwrite an existing archive. `-BinaryRoot` can point to a CMake output
directory. `-IncludeOpenXRSetup` includes the separately built matching OpenXR
installer.

## Experimental DX12 Ray Reconstruction

Native NGX RR now has separate center/periphery processing and RR preset menus
(Game default, D, E, F). SR preset selections are retained when switching modes.
RR denoises the periphery; disabling Peripheral DLSS keeps RR at input resolution
there. NR is available before and after RR. Unsupported guide contracts retain
native full-frame RR and NR. Validation across games and headsets remains limited.
