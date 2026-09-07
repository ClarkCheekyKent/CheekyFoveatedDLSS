# ACC startup crash, 2026-09-07

The initial addon test crashed ACC and was followed by SteamVR compositor and
server watchdog aborts. The previous addon was restored (SHA256
`EF2FA8A0B7570AE4A578B63D70848F6B0DFB9568C2442AFB1FED7106C68F1242`),
the mock was set to off and its external-driver registration was removed.
Crash logs and dumps are retained locally in `build/openvr-crash-20260907`.

The game's first-chance access violation occurred in `vrclient_x64.dll` at
RVA `0xAE618`, writing to `0x3297C780`. The stack pointer was `0x673297C630`:
the write target is consistent with a truncated stack pointer. The caller at
addon RVA `0x4E99D` resolves, using the failed build's Release PDB, to
`CompositorHooks<1>::wait_hook + 0x4D`.

The initial implementation inline-hooked compositor implementation addresses.
SteamVR forwarding thunks can be shared by unrelated interface methods; routing
an unrelated call through the WaitGetPoses ABI can truncate a pointer to a
32-bit pose-array count. Treating a shared implementation address as a unique
method identity is unsafe. The revised implementation patches specific vtable
entries, preserves original slot values, and observes requested interfaces via
VR_GetGenericInterface instead of proactively requesting every compositor ABI.
Nested compatibility-wrapper submissions publish only the outer submission.

ReShade also intercepts OpenVR. Its source explicitly hooks only the first
compositor version encountered, to avoid compatibility-wrapper recursion.
Forcing interface requests from our worker interferes with that selection.

Validation so far: Release build and existing tests pass; a new offline test
checks shared-function isolation, full-width pointers and restoration ownership.
The expanded adapter probe checks IVRSystem_019 and _026 projection calls after
arming compositor wrappers, in addition to legacy WaitGetPoses and shutdown.
It could not run after the crash because the background runtime returned error
121 (SteamVR stopped). Earlier background-only probes did not cover this bug.

Follow-up: the user confirmed the restored ACC baseline starts. The expanded
probe passed against SteamVR 2.16.7 with the mock unregistered: compositor
_022/_027/_028/_029 table hooks armed, both IVRSystem_019 and _026 returned
matching valid projection bounds, and legacy WaitGetPoses plus shutdown
invalidation passed. Its background WaitGetPoses result was 103 (not a scene
application), so this does not establish scene submission correctness.

Further live validation: the revised addon starts in ACC. Diagnostics show
OpenVR 2.16.7, a focused session, packed left/right submission mapping with
thousands of DLSS matches and no mapping ambiguity. With the mock unregistered,
runtime gaze is unavailable as expected. The user then enabled addon simulated
gaze and confirmed visible motion; diagnostics show tracking valid, using gaze,
both eye samples and nonzero crop movement with mapping still unambiguous.

Pending: restore mock registration and test runtime-provided gaze with addon
simulation turned off, followed by dropout/recovery.
The server watchdog named cheeky_mock, but its exact relationship to the game
exception remains unproven. Do not claim that driver path is cleared by the
offline hook test.
