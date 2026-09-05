#pragma once

#include <stdint.h>

#define CHEEKY_GAZE_ABI_VERSION 1U
#define CHEEKY_GAZE_MAX_VIEWS 2U
#define CHEEKY_GAZE_RUNTIME_NAME_SIZE 128U

#define CHEEKY_GAZE_STATUS_SIMULATED (1U << 9U)

#define CHEEKY_GAZE_STATUS_LAYER_ACTIVE (1U << 0U)
#define CHEEKY_GAZE_STATUS_EXTENSION_ENABLED (1U << 1U)
#define CHEEKY_GAZE_STATUS_SYSTEM_SUPPORTED (1U << 2U)
#define CHEEKY_GAZE_STATUS_SESSION_FOCUSED (1U << 3U)
#define CHEEKY_GAZE_STATUS_ACTION_ACTIVE (1U << 4U)
#define CHEEKY_GAZE_STATUS_GAZE_VALID (1U << 5U)
#define CHEEKY_GAZE_STATUS_MAPPING_READY (1U << 6U)
#define CHEEKY_GAZE_STATUS_UNSUPPORTED_VIEW_CONFIG (1U << 7U)
#define CHEEKY_GAZE_STATUS_AMBIGUOUS_RESOURCE (1U << 8U)

#define CHEEKY_GAZE_VIEW_ORIENTATION_VALID (1U << 0U)
#define CHEEKY_GAZE_VIEW_POSITION_VALID (1U << 1U)
#define CHEEKY_GAZE_VIEW_ORIENTATION_TRACKED (1U << 2U)
#define CHEEKY_GAZE_VIEW_POSITION_TRACKED (1U << 3U)
#define CHEEKY_GAZE_VIEW_RESOURCE_VALID (1U << 4U)

typedef struct CheekyGazeViewV1 {
    uint32_t structure_size;
    uint32_t view_index;
    uint32_t flags;
    uint32_t array_index;
    float center_u;
    float center_v;
    int32_t image_rect_x;
    int32_t image_rect_y;
    uint32_t image_rect_width;
    uint32_t image_rect_height;
    uint64_t resource_identity;
    uint64_t swapchain_identity;
} CheekyGazeViewV1;

typedef struct CheekyGazeSnapshotV1 {
    uint32_t abi_version;
    uint32_t structure_size;
    uint32_t status_flags;
    uint32_t view_count;
    uint64_t sequence;
    uint64_t publication_qpc;
    int64_t predicted_display_time;
    int64_t sample_time;
    uint64_t session_generation;
    uint64_t swapchain_generation;
    char runtime_name[CHEEKY_GAZE_RUNTIME_NAME_SIZE];
    CheekyGazeViewV1 views[CHEEKY_GAZE_MAX_VIEWS];
} CheekyGazeSnapshotV1;

typedef uint32_t(__cdecl* CheekyOpenXRGetGazeSnapshotFn)(
    uint32_t requested_version,
    void* output,
    uint32_t output_size
);

