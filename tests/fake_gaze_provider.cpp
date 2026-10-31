// Gaze provider fixture (shared/cheeky_gaze_provider.h) for CheekyTests --gaze-provider.
#include "cheeky_gaze_provider.h"

#include <Windows.h>

#include <cstring>

namespace {
volatile LONG64 g_left = 0x7101, g_right = 0x7102, g_display_time{};
}

extern "C" __declspec(dllexport) uint32_t __cdecl CheekyGazeProvider_GetGazeSnapshot(
    uint32_t requested_version, void* output, uint32_t output_size) {
    if (requested_version != CHEEKY_GAZE_ABI_VERSION || output == nullptr || output_size < sizeof(CheekyGazeSnapshotV1))
        return 0;
    CheekyGazeSnapshotV1 snapshot{};
    snapshot.abi_version = CHEEKY_GAZE_ABI_VERSION;
    snapshot.structure_size = sizeof(snapshot);
    snapshot.status_flags = CHEEKY_GAZE_STATUS_LAYER_ACTIVE | CHEEKY_GAZE_STATUS_EXTENSION_ENABLED |
        CHEEKY_GAZE_STATUS_SYSTEM_SUPPORTED | CHEEKY_GAZE_STATUS_SESSION_FOCUSED | CHEEKY_GAZE_STATUS_ACTION_ACTIVE |
        CHEEKY_GAZE_STATUS_GAZE_VALID | CHEEKY_GAZE_STATUS_MAPPING_READY;
    snapshot.view_count = 2;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    snapshot.sequence = static_cast<uint64_t>(InterlockedIncrement64(&g_display_time));
    snapshot.publication_qpc = static_cast<uint64_t>(now.QuadPart);
    snapshot.predicted_display_time = snapshot.sample_time = static_cast<int64_t>(snapshot.sequence);
    snapshot.session_generation = 7;
    snapshot.swapchain_generation = 1;
    strcpy_s(snapshot.runtime_name, "Cheeky test gaze provider");
    for (uint32_t eye = 0; eye < 2; ++eye) {
        auto& view = snapshot.views[eye];
        view.structure_size = sizeof(view);
        view.view_index = eye;
        view.flags = CHEEKY_GAZE_VIEW_ORIENTATION_VALID | CHEEKY_GAZE_VIEW_ORIENTATION_TRACKED |
            CHEEKY_GAZE_VIEW_FOV_VALID | CHEEKY_GAZE_VIEW_FORWARD_VALID;
        view.center_u = view.next_jump_u = .4F;
        view.center_v = view.next_jump_v = .6F;
        view.image_rect_width = 2000;
        view.image_rect_height = 1600;
        view.fov_left = view.fov_down = -.8F;
        view.fov_right = view.fov_up = .8F;
        view.forward_u = view.forward_v = .5F;
    }
    std::memcpy(output, &snapshot, sizeof(snapshot));
    return 1;
}

extern "C" __declspec(dllexport) uint32_t __cdecl CheekyGazeProvider_GetEyeViews(CheekyGazeProviderEyeViewsV1* views) {
    if (views == nullptr || views->structure_size < sizeof(CheekyGazeProviderEyeViewsV1)) return 0;
    views->version = CHEEKY_GAZE_PROVIDER_EYE_VIEWS_VERSION;
    views->left_view = static_cast<uint64_t>(InterlockedCompareExchange64(&g_left, 0, 0));
    views->right_view = static_cast<uint64_t>(InterlockedCompareExchange64(&g_right, 0, 0));
    views->output_width = 2000;
    views->output_height = 1600;
    views->session_generation = 7;
    return 1;
}

// Test control: which view the provider reports for each eye.
extern "C" __declspec(dllexport) void __cdecl CheekyFakeGazeProvider_SetEyeViews(uint64_t left, uint64_t right) {
    InterlockedExchange64(&g_left, static_cast<LONG64>(left));
    InterlockedExchange64(&g_right, static_cast<LONG64>(right));
}
