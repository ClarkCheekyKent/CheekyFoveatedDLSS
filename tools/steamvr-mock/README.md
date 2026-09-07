# SteamVR mock gaze experiment

This is a development-only build using the actual `EyeTrackingOutput` helper
from [Sboy's CustomHeadsetOpenVR](https://github.com/sboys3/CustomHeadsetOpenVR),
pinned to `21185da63a9e4307ae876b3702a43a43044d451c` and its OpenVR submodule.
It replaces the full provider with a small gaze-only provider. It does not load
the headset replacement, compositor shader, or hooking parts of that project.
The upstream license and OpenVR license are packaged alongside the binaries.

The Cheeky add-on and OpenXR layer are unchanged. This experiment first checks
whether SteamVR exposes driver-generated gaze from the existing Quest 3 device
through the native OpenVR foveation API. It does not yet add OpenVR support to
Cheeky, and it does not run code on the Quest.

## Prepared on this machine

- Driver: `build/steamvr-mock/package/driver`
- Reader: `build/steamvr-mock/package/probe/gaze-probe.exe`
- Driver name in SteamVR: `cheeky_mock`
- Driver is registered alongside Virtual Desktop and starts with `mode=0` (off).
- Original registration is backed up in `package/openvrpaths-before.vrpath`.
- The package contains the exact upstream helper patch as `sboy-helper.patch`.
- Build and helper tests passed. Live Quest 3 / Virtual Desktop testing on SteamVR
  2.16.7 passed on 2026-09-07: mock off returned 0/100 valid samples; center and
  sweep each returned 100/100 valid samples. Sweep left-eye NDC X range was 0.4726.
  Dropout returned 70 valid and 30 invalid samples, including recovery to valid.
  Logs are `probe-20260907-105044.txt`, `probe-20260907-105121.txt`,
  `probe-20260907-105152.txt`, and `probe-20260907-105230.txt` in the package.

## Baseline, then enable

1. Connect Quest 3 through Virtual Desktop and start SteamVR. Leave the headset
   awake. No game is needed for the initial background reader attempt.
2. Run `./tools/steamvr-mock/control.ps1 probe` from the repository root.
   Expect `valid=0` throughout with the mock off. Save this baseline.
3. Run `./tools/steamvr-mock/control.ps1 center`, then `probe` again.
   Expect valid, steady per-eye centers. The centers need not be numerically zero.
4. Run `./tools/steamvr-mock/control.ps1 sweep`, then `probe` again.
   Expect valid, changing values for both eyes and a nonzero left-X range.
5. Optional: use `dropout` to generate six seconds of motion followed by two
   seconds of invalid gaze. Use `off` to explicitly publish invalid input.

Every probe saves a timestamped log in the package directory. `status` prints
registration, mode, and recent matching driver log messages. A successful
component update alone does not prove application gaze acquisition works.
If the background reader cannot acquire gaze, investigate runtime requirements
before treating that as proof of a driver failure.

The mock waits for the Quest 3 model string and refuses initial attachment if
the device already advertises eye tracking. It logs component creation/update
errors. Component attachment and native API acquisition through Virtual Desktop
were confirmed in the live test above.
Changing modes is live within 250 ms; restarting SteamVR resets component state.
To fully remove the experiment, exit SteamVR and run
`./tools/steamvr-mock/control.ps1 unregister`. This removes only this driver entry.

## Rebuild

Clone the upstream repository into `build/steamvr-mock/CustomHeadsetOpenVR`,
check out the pinned revision above, and initialize its `ThirdParty/openvr`
submodule. Run `./tools/steamvr-mock/build.ps1` with Visual C++ v143 installed.
The script patches only the cloned eye tracking helper, builds the mock, reader,
and helper tests, then runs tests. It does not invoke upstream deployment steps.
Exit SteamVR before rebuilding a loaded driver.

Tests use a fake OpenVR input interface to exercise the modified helper's
coordinate conversion, invalidation/recovery, and stale samples. They also load
the actual driver DLL and verify its factory/interface contract. They do not
establish runtime compatibility or eye tracker latency.
