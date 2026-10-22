#pragma once
#include <stdint.h>
#define CHEEKY_QUAD_VIEWS_VERSION 1U
// Separate from the two-eye gaze ABI. View order is OpenXR peripheral L/R,
// focus L/R; roles require pixel verification or an explicit manual layout.
typedef struct CheekyQuadViewV1 {
    uint64_t dlss_view, registration_generation, submitted_resource;
    uint32_t view_index, eye_index, focus, verified;
    uint32_t array_slice, width, height;
} CheekyQuadViewV1;
typedef struct CheekyQuadSourceV1 {
    uint64_t dlss_view, registration_generation;
    uint32_t number, manual_role, width, height;
} CheekyQuadSourceV1;
typedef struct CheekyQuadViewsV1 {
    uint32_t version, size, active, graphics_api;
    uint64_t session_generation, revision, captures, rejected;
    uint32_t status, confirmations; // 0 unavailable, 1 acquiring, 2 verified, 3 unresolved
    uint32_t manual_mode, show_numbers;
    CheekyQuadViewV1 views[4];
    CheekyQuadSourceV1 sources[8];
} CheekyQuadViewsV1;
typedef struct CheekyQuadSourceD3D11V1 {
    uint32_t version, size;
    uint64_t dlss_view, registration_generation;
    void* context;
    void* texture;
    uint32_t x, y, width, height, array_slice;
} CheekyQuadSourceD3D11V1;
typedef uint32_t(__cdecl* CheekyQuadGetViewsV1Fn)(CheekyQuadViewsV1*);
typedef uint32_t(__cdecl* CheekyQuadObserveD3D11V1Fn)(const CheekyQuadSourceD3D11V1*);
typedef void(__cdecl* CheekyQuadForgetSourceV1Fn)(uint64_t, uint64_t);
typedef void(__cdecl* CheekyQuadRecalibrateV1Fn)();
typedef void(__cdecl* CheekyQuadConfigureV1Fn)(uint32_t, uint32_t);
typedef uint32_t(__cdecl* CheekyQuadAssignV1Fn)(uint64_t, uint32_t, uint32_t, uint64_t, uint64_t);
typedef struct CheekyQuadSavedLayoutV1 {
    uint32_t version,size,source_count,roles[8];
} CheekyQuadSavedLayoutV1;
typedef uint32_t(__cdecl* CheekyQuadSavedLayoutV1Fn)(const CheekyQuadSavedLayoutV1*);
typedef void*(__cdecl* CheekyQuadAcquireD3D11V1Fn)(void*);
typedef void(__cdecl* CheekyQuadReleaseD3D11V1Fn)(void*);
