Cheeky native Vulkan - fixed DLSS-SR preview

1. Close the game. Back up your existing CheekyFoveatedDLSS folder.
2. Extract this ZIP beside RDR2.exe (or the actual rendering executable).
   Replace Cheeky's dxgi.dll and host/runtime pair together. Keep your
   existing dxgi2.dll and Luke Ross files. The included INI replaces
   Cheeky's settings with a centered fixed SR test preset.
3. Launch in Vulkan with in-game DLSS enabled. The red foveation outline
   is enabled; NR and peripheral DLAA are off. F8 opens Cheeky's menu.
   Start by checking the border and changing SR width/height modestly.

This is an interim build, not a completed Vulkan release. Native compute
and NVIDIA SR inference have been exercised with private test images;
compatibility with RDR2 and R.E.A.L. VR still needs your game test.
The drop-in loader and six real SR frames, including a width change, have
passed readback checks. A shutdown hang in the direct-driver test harness
is still being investigated; clean game exit is not yet verified.
The Vulkan F8 renderer is included but its full presentation tests are
still in progress. Keep NR/peripheral DLAA off for this first test.

Logs are inside CheekyFoveatedDLSS:
  CheekyFoveatedDLSS-Standalone.log
  CheekyFoveatedDLSS-Host.log
The DXGI loader activates the bundled Vulkan layer for this process.
There is no install script or registry registration. Keep the bundled
CheekyFoveatedDLSS/Vulkan/Cheeky.json file in place.
No Vulkan-to-DirectX translation or extra copy through DX12 is used.

Undo: close the game and restore the previous Cheeky dxgi.dll and folder.
There is nothing to unregister. Your other mods are not removed.
