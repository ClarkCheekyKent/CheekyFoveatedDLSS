#pragma once

#include <stdint.h>

#include "cheeky_gaze_abi.h"

// Gaze provider interface: for VR mods that own the headset session and render
// the stereo pair themselves (flat-to-VR mods, alternate-eye rendering), where
// Cheeky's OpenXR layer is not in the session. The mod exports these functions
// from any DLL loaded in the game process, for example its dxgi.dll proxy.
// Cheeky finds them by name, keeps that DLL loaded while it uses them, and
// releases it at shutdown. When Cheeky's OpenXR layer is loaded, the layer is
// used instead. See GAZE-PROVIDER.md.

// Required. The same contract as CheekyOpenXR_GetGazeSnapshot: fill a
// CheekyGazeSnapshotV1 (CHEEKY_GAZE_ABI_VERSION) and return nonzero, or return
// 0 for an unsupported version or a too-small buffer.
#define CHEEKY_GAZE_PROVIDER_SNAPSHOT_EXPORT "CheekyGazeProvider_GetGazeSnapshot"
typedef CheekyOpenXRGetGazeSnapshotFn CheekyGazeProviderGetGazeSnapshotFn;

// Optional. Which DLSS view renders which eye, for providers that know it
// exactly (e.g. one DLSS feature per eye, evaluated on alternate frames).
// Cheeky treats the pair as a calibrated stereo pair with a full-image mapping
// and skips marker calibration while the provider reports it. Cheeky sets
// structure_size; the provider fills the rest and returns nonzero, or returns 0
// while it has no pair (menus, flat screens).
#define CHEEKY_GAZE_PROVIDER_EYE_VIEWS_EXPORT "CheekyGazeProvider_GetEyeViews"
#define CHEEKY_GAZE_PROVIDER_EYE_VIEWS_VERSION 1U
typedef struct CheekyGazeProviderEyeViewsV1 {
    uint32_t version;         // CHEEKY_GAZE_PROVIDER_EYE_VIEWS_VERSION
    uint32_t structure_size;  // set by Cheeky
    uint64_t left_view;       // the NVSDK_NGX_Handle* the game evaluates for each eye
    uint64_t right_view;
    uint32_t output_width;    // DLSS output size of each eye
    uint32_t output_height;
    uint64_t session_generation;  // the gaze snapshot's session_generation
} CheekyGazeProviderEyeViewsV1;
typedef uint32_t(__cdecl* CheekyGazeProviderGetEyeViewsFn)(CheekyGazeProviderEyeViewsV1* views);
