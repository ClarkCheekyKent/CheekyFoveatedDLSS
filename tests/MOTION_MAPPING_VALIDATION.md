# D3D12 motion mapping validation

Branch: `fix/gaze-visual-artifacts`
Current PR base: `e44282eed38e966b3368e2e8d39ee831d8d8fbd0` (upstream `master`).

The standalone fix preserves declared motion-vector mapping, region rejection,
history invalidation, and peripheral motion flags. Native D3D12 preparation
validates regions before custom evaluation; the before-upscaling input scope is
not needed on master. Issue reporting and NR-before-upscaling are not included.
The small scoped crop override used by the backend regression fixture is retained
so tests exercise the same exact crop positions through production preparation.

Standalone verification uses `build/gaze-standalone-verify`. The comparison
builds and headset observations below are historical evidence from the stacked
implementation; they do not identify the standalone binary.

Historical comparison baseline: `1c89ed8e5c7aa3fe33e6fe3845fcc12b6f84d1bd` (`feature/neural-render-before-upscaling`).

The mapping correction and the headset artifact report have separate acceptance criteria.
The tests use actual D3D12 backend preparation/history/parameter restoration on WARP,
with fake NVIDIA feature callbacks. They do not establish visual quality in a headset.

## Standalone automated checks (2026-09-07)

Release add-on build, all three CTest targets, and the D3D12 WARP compositor
checks passed on the standalone master-based implementation:

```powershell
cmake -S . -B build/gaze-standalone-verify -G "Visual Studio 18 2026" -A x64
cmake --build build/gaze-standalone-verify --config Release --parallel 4
ctest --test-dir build/gaze-standalone-verify -C Release --output-on-failure
.\build\gaze-standalone-verify\bin\Release\CheekyTests.exe --d3d12-composite
```

## Historical automated checks

Release add-on build succeeded; all three CTest targets passed on 2026-09-07.

```powershell
cmake -S . -B build/motion-verify -G "Visual Studio 18 2026" -A x64
cmake --build build/motion-verify --config Release --parallel 4
ctest --test-dir build/motion-verify -C Release --output-on-failure
```

- `CheekyGazeTests`: existing gaze, dispatch, NR contract, and support tests.
  The CMake test target now enables `/EHsc`, required by its existing exception-cleanup test.
- `CheekyMotionRegionTests`: signed/unsigned reads (including zero and the sign bit), missing
  declarations/resources, packed stereo bases, explicit high resolution, equal dimensions,
  both interpretations fitting, boundary fits, out-of-bounds and checked arithmetic.
- `CheekyMotionBackendTests`: real backend feature keys, crop-motion compensation,
  declaration preservation, rejection before custom evaluation, parameter/output restoration,
  skipped/failed history reset on resume, peripheral rejection and explicit high-resolution
  flags after peripheral resampling. Legitimate flag, size, quality and preset changes recreate features.

The 4936x1189 fixture uses input crop Y=304/312 and output crop Y=456/468.
Its 729-pixel output crop height fits the alternative high-resolution interpretation at 304
and exceeds the texture at 312. The declared low-resolution region is valid at both positions.
Restoring the old distance/fit heuristic temporarily makes the regression fail with
`4936x1189 gaze movement changed declared low MV mapping` (exit 1).
The mutation was removed before the final build.

## Comparison builds

- Baseline add-on: `build/motion-baseline-build/bin/Release/CheekyFoveatedDLSS.addon64`
- Patched add-on: `build/motion-verify/bin/Release/CheekyFoveatedDLSS.addon64`

Both use the installed MSVC v145 compiler in Release mode. Baseline sources were extracted
from the commit above into `build/motion-baseline-source`; the working branch was not switched.
The installed add-on was subsequently verified by SHA-256 to match the patched build.

## Assetto Corsa EVO headset comparison

Use the same scene/replay, headset runtime, render resolution, DLSS quality/preset, peripheral
DLAA configuration, crop size, gaze smoothing/quantization and reset ratio for both binaries.
Close the game before swapping the add-on and retain a separate trace for each run.
Keep the OpenXR layer and every other component the same.

Run each row for baseline and patch, separately with NR before upscaling and NR after upscaling.
Observe both eyes, including packed-eye boundary positions.

| Scenario | NR before upscaling | NR after upscaling |
| --- | --- | --- |
| Stationary gaze | Pending | Pending |
| Slow boundary crossings, including Y=304/312 where reproducible | Pending | Pending |
| Rapid gaze jumps | Pending | Pending |
| Both eyes checked for temporary mismatched scene content | Pending | Pending |

Inspect `D3D12 MV` and `D3D12 peripheral MV` records by view identity. For unchanged game flags,
space must remain constant; invalid rectangles must produce a rejection, never reinterpretation.
`D3D12 feature key` reports old/new configuration fields; crop-only movement must not recreate
features. `VR gaze history reset` reports displacement, crop dimensions and effective
thresholds. A nonzero large-jump reset count is expected and is not itself a failure.

## Observed headset result (2026-09-07)

The user reported substantially improved stability and believes the temporary mismatched
scene content is completely gone. The installed binary's SHA-256 matched the patched build:
`4B2FB16B589F18D6D78CA208375A549F0287CF248526966CFC8AD84A10DCBD8E`.

The session log from 15:28:52 through 15:35:22 contained 288 motion-region records,
all declared input-space with flags `0x2B` and valid regions. Both center SR and
peripheral DLAA were represented for both view IDs. Only four initial private feature
creations occurred, with no subsequent recreations or logged evaluation failures.
One startup gaze-association rejection and gaze-jump/reacquisition resets were recorded;
these do not indicate recurrence of the corrected motion-space switching.

The log is rate-limited and cannot establish visual quality for every frame. The full
scenario-by-rendering-order matrix above has not been individually confirmed; its pending
entries do not negate the user's positive result for the tested session. If artifacts recur,
retain the per-eye traces and investigate their remaining cause separately.
