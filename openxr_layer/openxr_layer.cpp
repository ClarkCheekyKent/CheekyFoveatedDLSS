#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12

#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include "cheeky_gaze_abi.h"
#include "gaze_math.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr char layer_name[] = "XR_APILAYER_CHEEKY_foveated_dlss";

template <typename T>
[[nodiscard]] T load_function(
    const PFN_xrGetInstanceProcAddr gipa,
    const XrInstance instance,
    const char* const name
) noexcept {
    PFN_xrVoidFunction function{};
    return gipa != nullptr &&
            XR_SUCCEEDED(gipa(instance, name, &function))
        ? reinterpret_cast<T>(function)
        : nullptr;
}

struct Dispatch {
    PFN_xrGetInstanceProcAddr get_instance_proc_addr{};
    PFN_xrDestroyInstance destroy_instance{};
    PFN_xrGetInstanceProperties get_instance_properties{};
    PFN_xrGetSystem get_system{};
    PFN_xrGetSystemProperties get_system_properties{};
    PFN_xrCreateSession create_session{};
    PFN_xrDestroySession destroy_session{};
    PFN_xrPollEvent poll_event{};
    PFN_xrBeginSession begin_session{};
    PFN_xrEndSession end_session{};
    PFN_xrCreateActionSet create_action_set{};
    PFN_xrDestroyActionSet destroy_action_set{};
    PFN_xrCreateAction create_action{};
    PFN_xrDestroyAction destroy_action{};
    PFN_xrStringToPath string_to_path{};
    PFN_xrSuggestInteractionProfileBindings suggest_bindings{};
    PFN_xrAttachSessionActionSets attach_action_sets{};
    PFN_xrSyncActions sync_actions{};
    PFN_xrGetActionStatePose get_action_state_pose{};
    PFN_xrCreateActionSpace create_action_space{};
    PFN_xrDestroySpace destroy_space{};
    PFN_xrLocateSpace locate_space{};
    PFN_xrLocateViews locate_views{};
    PFN_xrCreateSwapchain create_swapchain{};
    PFN_xrDestroySwapchain destroy_swapchain{};
    PFN_xrEnumerateSwapchainImages enumerate_swapchain_images{};
    PFN_xrAcquireSwapchainImage acquire_swapchain_image{};
    PFN_xrWaitSwapchainImage wait_swapchain_image{};
    PFN_xrReleaseSwapchainImage release_swapchain_image{};
    PFN_xrEndFrame end_frame{};
};

void populate_dispatch(
    Dispatch& dispatch,
    const XrInstance instance,
    const PFN_xrGetInstanceProcAddr gipa
) noexcept {
    dispatch.get_instance_proc_addr = gipa;
#define CHEEKY_LOAD(field, name) \
    dispatch.field = load_function<PFN_xr##name>(gipa, instance, "xr" #name)
    CHEEKY_LOAD(destroy_instance, DestroyInstance);
    CHEEKY_LOAD(get_instance_properties, GetInstanceProperties);
    CHEEKY_LOAD(get_system, GetSystem);
    CHEEKY_LOAD(get_system_properties, GetSystemProperties);
    CHEEKY_LOAD(create_session, CreateSession);
    CHEEKY_LOAD(destroy_session, DestroySession);
    CHEEKY_LOAD(poll_event, PollEvent);
    CHEEKY_LOAD(begin_session, BeginSession);
    CHEEKY_LOAD(end_session, EndSession);
    CHEEKY_LOAD(create_action_set, CreateActionSet);
    CHEEKY_LOAD(destroy_action_set, DestroyActionSet);
    CHEEKY_LOAD(create_action, CreateAction);
    CHEEKY_LOAD(destroy_action, DestroyAction);
    CHEEKY_LOAD(string_to_path, StringToPath);
    dispatch.suggest_bindings = load_function<PFN_xrSuggestInteractionProfileBindings>(
        gipa, instance, "xrSuggestInteractionProfileBindings"
    );
    dispatch.attach_action_sets = load_function<PFN_xrAttachSessionActionSets>(
        gipa, instance, "xrAttachSessionActionSets"
    );
    CHEEKY_LOAD(sync_actions, SyncActions);
    CHEEKY_LOAD(get_action_state_pose, GetActionStatePose);
    CHEEKY_LOAD(create_action_space, CreateActionSpace);
    CHEEKY_LOAD(destroy_space, DestroySpace);
    CHEEKY_LOAD(locate_space, LocateSpace);
    CHEEKY_LOAD(locate_views, LocateViews);
    CHEEKY_LOAD(create_swapchain, CreateSwapchain);
    CHEEKY_LOAD(destroy_swapchain, DestroySwapchain);
    CHEEKY_LOAD(enumerate_swapchain_images, EnumerateSwapchainImages);
    CHEEKY_LOAD(acquire_swapchain_image, AcquireSwapchainImage);
    CHEEKY_LOAD(wait_swapchain_image, WaitSwapchainImage);
    CHEEKY_LOAD(release_swapchain_image, ReleaseSwapchainImage);
    CHEEKY_LOAD(end_frame, EndFrame);
#undef CHEEKY_LOAD
}

struct SubmittedView {
    XrSwapchain swapchain{XR_NULL_HANDLE};
    XrRect2Di rect{};
    std::uint32_t array_index{};
    std::uint64_t resource_identity{};
    bool valid{};
};

struct SessionState {
    XrSession session{XR_NULL_HANDLE};
    XrInstance instance{XR_NULL_HANDLE};
    XrSystemId system_id{XR_NULL_SYSTEM_ID};
    XrSpace gaze_space{XR_NULL_HANDLE};
    XrViewConfigurationType view_configuration{};
    XrSessionState state{XR_SESSION_STATE_UNKNOWN};
    std::uint64_t generation{};
    bool system_supported{};
    bool action_attached{};
    bool action_active{};
    bool gaze_valid{};
    bool simulated{};
    XrTime simulation_start{};
    bool unsupported_view_configuration{};
    bool ambiguous_resource{};
    XrTime predicted_display_time{};
    XrTime sample_time{};
    std::uint32_t gaze_location_flags{};
    std::array<float, CHEEKY_GAZE_MAX_VIEWS> center_u{};
    std::array<float, CHEEKY_GAZE_MAX_VIEWS> center_v{};
    std::array<SubmittedView, CHEEKY_GAZE_MAX_VIEWS> submitted_views{};
};

struct SwapchainState {
    XrSwapchain swapchain{XR_NULL_HANDLE};
    XrSession session{XR_NULL_HANDLE};
    XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    std::vector<std::uint64_t> resource_identities;
    std::uint32_t acquired_index{};
    std::uint32_t released_index{};
    bool has_acquired{};
    bool has_released{};
};

struct InstanceState {
    XrInstance instance{XR_NULL_HANDLE};
    Dispatch dispatch{};
    std::string runtime_name;
    bool extension_enabled{};
    XrActionSet action_set{XR_NULL_HANDLE};
    XrAction gaze_action{XR_NULL_HANDLE};
    XrPath gaze_path{XR_NULL_PATH};
    XrPath gaze_profile{XR_NULL_PATH};
    bool gaze_binding_submitted{};
};

std::atomic<bool> simulated_gaze_enabled{};
std::mutex state_mutex;
std::unordered_map<XrInstance, InstanceState> instances;
std::unordered_map<XrSession, SessionState> sessions;
std::unordered_map<XrSwapchain, SwapchainState> swapchains;
std::atomic<std::uint64_t> next_session_generation{1U};
std::atomic<std::uint64_t> swapchain_generation{1U};

struct SnapshotSlot {
    std::atomic<std::uint32_t> readers{};
    CheekyGazeSnapshotV1 snapshot{};
};

std::array<SnapshotSlot, 2U> snapshot_slots{};
std::atomic<std::uint32_t> active_snapshot_slot{};
std::uint64_t publication_sequence{};

[[nodiscard]] std::uint64_t query_qpc() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

void initialize_snapshot(CheekyGazeSnapshotV1& snapshot) noexcept {
    snapshot = {};
    snapshot.abi_version = CHEEKY_GAZE_ABI_VERSION;
    snapshot.structure_size = sizeof(snapshot);
    for (auto& view : snapshot.views) {
        view.structure_size = sizeof(view);
    }
}

void publish_snapshot_locked(const SessionState* const session) noexcept {
    CheekyGazeSnapshotV1 snapshot{};
    initialize_snapshot(snapshot);
    snapshot.status_flags = CHEEKY_GAZE_STATUS_LAYER_ACTIVE;
    snapshot.sequence = ++publication_sequence;
    snapshot.publication_qpc = query_qpc();
    snapshot.swapchain_generation = swapchain_generation.load(
        std::memory_order_acquire
    );

    const InstanceState* instance{};
    if (session != nullptr) {
        const auto instance_it = instances.find(session->instance);
        if (instance_it != instances.end()) instance = &instance_it->second;
    } else if (!instances.empty()) {
        instance = &instances.begin()->second;
    }
    if (instance != nullptr) {
        if (instance->extension_enabled) {
            snapshot.status_flags |= CHEEKY_GAZE_STATUS_EXTENSION_ENABLED;
        }
        static_cast<void>(strncpy_s(
            snapshot.runtime_name,
            instance->runtime_name.c_str(),
            _TRUNCATE
        ));
    }

    if (session != nullptr) {
        if (session->simulated) snapshot.status_flags |= CHEEKY_GAZE_STATUS_SIMULATED;
        snapshot.session_generation = session->generation;
        snapshot.predicted_display_time = session->predicted_display_time;
        snapshot.sample_time = session->sample_time;
        snapshot.view_count = CHEEKY_GAZE_MAX_VIEWS;
        if (session->system_supported) {
            snapshot.status_flags |= CHEEKY_GAZE_STATUS_SYSTEM_SUPPORTED;
        }
        if (session->state == XR_SESSION_STATE_FOCUSED) {
            snapshot.status_flags |= CHEEKY_GAZE_STATUS_SESSION_FOCUSED;
        }
        if (session->action_active) {
            snapshot.status_flags |= CHEEKY_GAZE_STATUS_ACTION_ACTIVE;
        }
        if (session->gaze_valid) {
            snapshot.status_flags |= CHEEKY_GAZE_STATUS_GAZE_VALID;
        }
        if (session->unsupported_view_configuration) {
            snapshot.status_flags |=
                CHEEKY_GAZE_STATUS_UNSUPPORTED_VIEW_CONFIG;
        }
        if (session->ambiguous_resource) {
            snapshot.status_flags |= CHEEKY_GAZE_STATUS_AMBIGUOUS_RESOURCE;
        }

        bool all_resources = true;
        for (std::uint32_t index{}; index < CHEEKY_GAZE_MAX_VIEWS; ++index) {
            const auto& source = session->submitted_views[index];
            auto& target = snapshot.views[index];
            target.view_index = index;
            target.center_u = session->center_u[index];
            target.center_v = session->center_v[index];
            target.flags = session->gaze_location_flags & 0xFU;
            target.array_index = source.array_index;
            target.image_rect_x = source.rect.offset.x;
            target.image_rect_y = source.rect.offset.y;
            target.image_rect_width = source.rect.extent.width;
            target.image_rect_height = source.rect.extent.height;
            target.resource_identity = source.resource_identity;
            target.swapchain_identity = static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(source.swapchain)
            );
            if (source.valid && source.resource_identity != 0U) {
                target.flags |= CHEEKY_GAZE_VIEW_RESOURCE_VALID;
            } else {
                all_resources = false;
            }
        }
        if (all_resources && !session->unsupported_view_configuration) {
            snapshot.status_flags |= CHEEKY_GAZE_STATUS_MAPPING_READY;
        }
    }

    const auto active = active_snapshot_slot.load(std::memory_order_acquire);
    const auto target = 1U - active;
    if (snapshot_slots[target].readers.load(std::memory_order_acquire) != 0U) {
        return;
    }
    snapshot_slots[target].snapshot = snapshot;
    active_snapshot_slot.store(target, std::memory_order_release);
}

[[nodiscard]] std::uint64_t canonical_resource_identity(
    IUnknown* const resource
) noexcept {
    if (resource == nullptr) return 0U;
    IUnknown* identity{};
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&identity))) ||
        identity == nullptr) {
        return 0U;
    }
    const auto value = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(identity)
    );
    identity->Release();
    return value;
}

[[nodiscard]] bool extension_available(
    const PFN_xrGetInstanceProcAddr gipa
) noexcept {
    const auto enumerate = load_function<PFN_xrEnumerateInstanceExtensionProperties>(
        gipa, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties"
    );
    if (enumerate == nullptr) return false;
    std::uint32_t count{};
    if (XR_FAILED(enumerate(nullptr, 0U, &count, nullptr)) || count == 0U) {
        return false;
    }
    std::vector<XrExtensionProperties> properties(
        count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES}
    );
    if (XR_FAILED(enumerate(nullptr, count, &count, properties.data()))) {
        return false;
    }
    return std::any_of(
        properties.begin(), properties.end(), [](const auto& property) {
            return std::strcmp(
                property.extensionName,
                XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME
            ) == 0;
        }
    );
}

[[nodiscard]] bool create_gaze_action(InstanceState& state) noexcept {
    const auto& dispatch = state.dispatch;
    if (!state.extension_enabled || dispatch.create_action_set == nullptr ||
        dispatch.create_action == nullptr || dispatch.string_to_path == nullptr) {
        return false;
    }

    XrActionSetCreateInfo action_set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    static_cast<void>(strcpy_s(
        action_set_info.actionSetName, "cheeky_eye_gaze"
    ));
    static_cast<void>(strcpy_s(
        action_set_info.localizedActionSetName, "Cheeky Eye Gaze"
    ));
    if (XR_FAILED(dispatch.create_action_set(
            state.instance, &action_set_info, &state.action_set
        ))) {
        return false;
    }

    XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
    action_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
    static_cast<void>(strcpy_s(
        action_info.actionName, "cheeky_gaze_pose"
    ));
    static_cast<void>(strcpy_s(
        action_info.localizedActionName, "Cheeky Gaze Pose"
    ));
    if (XR_FAILED(dispatch.create_action(
            state.action_set, &action_info, &state.gaze_action
        ))) {
        dispatch.destroy_action_set(state.action_set);
        state.action_set = XR_NULL_HANDLE;
        return false;
    }

    if (XR_FAILED(dispatch.string_to_path(
            state.instance,
            "/user/eyes_ext/input/gaze_ext/pose",
            &state.gaze_path
        )) || XR_FAILED(dispatch.string_to_path(
            state.instance,
            "/interaction_profiles/ext/eye_gaze_interaction",
            &state.gaze_profile
        ))) {
        dispatch.destroy_action(state.gaze_action);
        dispatch.destroy_action_set(state.action_set);
        state.gaze_action = XR_NULL_HANDLE;
        return false;
    }
    return true;
}

[[nodiscard]] InstanceState* find_instance_for_session_locked(
    const XrSession session
) noexcept {
    const auto session_it = sessions.find(session);
    if (session_it == sessions.end()) return nullptr;
    const auto instance_it = instances.find(session_it->second.instance);
    return instance_it == instances.end() ? nullptr : &instance_it->second;
}

[[nodiscard]] XrResult ensure_gaze_binding_locked(
    InstanceState& instance
) noexcept {
    if (instance.gaze_binding_submitted ||
        instance.gaze_action == XR_NULL_HANDLE ||
        instance.dispatch.suggest_bindings == nullptr) {
        return XR_SUCCESS;
    }
    const XrActionSuggestedBinding binding{
        instance.gaze_action, instance.gaze_path
    };
    const XrInteractionProfileSuggestedBinding info{
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
        nullptr,
        instance.gaze_profile,
        1U,
        &binding,
    };
    const auto result = instance.dispatch.suggest_bindings(
        instance.instance, &info
    );
    if (XR_SUCCEEDED(result)) instance.gaze_binding_submitted = true;
    return result;
}

[[nodiscard]] cheeky::gaze_math::Pose convert_pose(
    const XrPosef& pose
) noexcept {
    return {
        {pose.orientation.x, pose.orientation.y, pose.orientation.z,
         pose.orientation.w},
        {pose.position.x, pose.position.y, pose.position.z},
    };
}

void update_view_resource_locked(
    SessionState& session,
    const XrSwapchain swapchain
) noexcept {
    const auto swapchain_it = swapchains.find(swapchain);
    if (swapchain_it == swapchains.end()) return;
    const auto& state = swapchain_it->second;
    const auto index = state.has_acquired
        ? state.acquired_index
        : state.released_index;
    if (index >= state.resource_identities.size()) return;
    for (auto& view : session.submitted_views) {
        if (view.swapchain != swapchain) continue;
        view.resource_identity = state.resource_identities[index];
        view.valid = view.resource_identity != 0U;
    }
}

}  // namespace

extern "C" __declspec(dllexport) void __cdecl
CheekyOpenXR_SetSimulatedGaze(const std::uint32_t enabled) {
    simulated_gaze_enabled.store(enabled != 0U, std::memory_order_release);
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl
CheekyOpenXR_GetGazeSnapshot(
    const std::uint32_t requested_version,
    void* const output,
    const std::uint32_t output_size
) {
    if (requested_version != CHEEKY_GAZE_ABI_VERSION || output == nullptr ||
        output_size < sizeof(CheekyGazeSnapshotV1)) {
        return 0U;
    }
    for (;;) {
        const auto slot_index = active_snapshot_slot.load(
            std::memory_order_acquire
        );
        auto& slot = snapshot_slots[slot_index];
        slot.readers.fetch_add(1U, std::memory_order_acquire);
        if (slot_index != active_snapshot_slot.load(std::memory_order_acquire)) {
            slot.readers.fetch_sub(1U, std::memory_order_release);
            continue;
        }
        std::memcpy(output, &slot.snapshot, sizeof(slot.snapshot));
        slot.readers.fetch_sub(1U, std::memory_order_release);
        return 1U;
    }
}

// Forward declarations for entry points returned by the layer GIPA.
extern "C" {
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrGetInstanceProcAddr(
    XrInstance, const char*, PFN_xrVoidFunction*
);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrCreateApiLayerInstance(
    const XrInstanceCreateInfo*, const XrApiLayerCreateInfo*, XrInstance*
);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrDestroyInstance(XrInstance);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrGetSystem(
    XrInstance, const XrSystemGetInfo*, XrSystemId*
);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrGetSystemProperties(
    XrInstance, XrSystemId, XrSystemProperties*
);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrCreateSession(
    XrInstance, const XrSessionCreateInfo*, XrSession*
);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrDestroySession(XrSession);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrPollEvent(
    XrInstance, XrEventDataBuffer*
);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrBeginSession(
    XrSession, const XrSessionBeginInfo*
);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrEndSession(XrSession);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrSuggestInteractionProfileBindings(
    XrInstance, const XrInteractionProfileSuggestedBinding*
);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrAttachSessionActionSets(
    XrSession, const XrSessionActionSetsAttachInfo*
);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrSyncActions(
    XrSession, const XrActionsSyncInfo*
);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrLocateViews(
    XrSession, const XrViewLocateInfo*, XrViewState*, std::uint32_t,
    std::uint32_t*, XrView*
);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrCreateSwapchain(
    XrSession, const XrSwapchainCreateInfo*, XrSwapchain*
);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrDestroySwapchain(XrSwapchain);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrEnumerateSwapchainImages(
    XrSwapchain, std::uint32_t, std::uint32_t*, XrSwapchainImageBaseHeader*
);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrAcquireSwapchainImage(
    XrSwapchain, const XrSwapchainImageAcquireInfo*, std::uint32_t*
);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrWaitSwapchainImage(
    XrSwapchain, const XrSwapchainImageWaitInfo*
);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrReleaseSwapchainImage(
    XrSwapchain, const XrSwapchainImageReleaseInfo*
);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrEndFrame(
    XrSession, const XrFrameEndInfo*
);
}

namespace {

[[nodiscard]] bool is_intercepted_name(
    const char* const name,
    PFN_xrVoidFunction& function
) noexcept {
    if (name == nullptr) return false;
#define CHEEKY_INTERCEPT(openxr_name, layer_name) \
    if (std::strcmp(name, openxr_name) == 0) { \
        function = reinterpret_cast<PFN_xrVoidFunction>(layer_name); \
        return true; \
    }
    CHEEKY_INTERCEPT("xrGetInstanceProcAddr", cheeky_xrGetInstanceProcAddr)
    CHEEKY_INTERCEPT("xrDestroyInstance", cheeky_xrDestroyInstance)
    CHEEKY_INTERCEPT("xrGetSystem", cheeky_xrGetSystem)
    CHEEKY_INTERCEPT("xrGetSystemProperties", cheeky_xrGetSystemProperties)
    CHEEKY_INTERCEPT("xrCreateSession", cheeky_xrCreateSession)
    CHEEKY_INTERCEPT("xrDestroySession", cheeky_xrDestroySession)
    CHEEKY_INTERCEPT("xrPollEvent", cheeky_xrPollEvent)
    CHEEKY_INTERCEPT("xrBeginSession", cheeky_xrBeginSession)
    CHEEKY_INTERCEPT("xrEndSession", cheeky_xrEndSession)
    CHEEKY_INTERCEPT(
        "xrSuggestInteractionProfileBindings",
        cheeky_xrSuggestInteractionProfileBindings
    )
    CHEEKY_INTERCEPT(
        "xrAttachSessionActionSets", cheeky_xrAttachSessionActionSets
    )
    CHEEKY_INTERCEPT("xrSyncActions", cheeky_xrSyncActions)
    CHEEKY_INTERCEPT("xrLocateViews", cheeky_xrLocateViews)
    CHEEKY_INTERCEPT("xrCreateSwapchain", cheeky_xrCreateSwapchain)
    CHEEKY_INTERCEPT("xrDestroySwapchain", cheeky_xrDestroySwapchain)
    CHEEKY_INTERCEPT(
        "xrEnumerateSwapchainImages", cheeky_xrEnumerateSwapchainImages
    )
    CHEEKY_INTERCEPT(
        "xrAcquireSwapchainImage", cheeky_xrAcquireSwapchainImage
    )
    CHEEKY_INTERCEPT("xrWaitSwapchainImage", cheeky_xrWaitSwapchainImage)
    CHEEKY_INTERCEPT(
        "xrReleaseSwapchainImage", cheeky_xrReleaseSwapchainImage
    )
    CHEEKY_INTERCEPT("xrEndFrame", cheeky_xrEndFrame)
#undef CHEEKY_INTERCEPT
    return false;
}

}  // namespace

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrGetInstanceProcAddr(
    const XrInstance instance,
    const char* const name,
    PFN_xrVoidFunction* const function
) {
    if (function == nullptr) return XR_ERROR_VALIDATION_FAILURE;
    *function = nullptr;
    if (is_intercepted_name(name, *function)) return XR_SUCCESS;

    std::lock_guard lock(state_mutex);
    const auto iterator = instances.find(instance);
    if (iterator == instances.end() ||
        iterator->second.dispatch.get_instance_proc_addr == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    return iterator->second.dispatch.get_instance_proc_addr(
        instance, name, function
    );
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrCreateApiLayerInstance(
    const XrInstanceCreateInfo* const info,
    const XrApiLayerCreateInfo* const layer_info,
    XrInstance* const instance
) {
    if (info == nullptr || layer_info == nullptr || instance == nullptr ||
        layer_info->nextInfo == nullptr ||
        layer_info->nextInfo->nextGetInstanceProcAddr == nullptr ||
        layer_info->nextInfo->nextCreateApiLayerInstance == nullptr) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    const auto next_gipa = layer_info->nextInfo->nextGetInstanceProcAddr;
    const auto next_create = layer_info->nextInfo->nextCreateApiLayerInstance;
    bool already_enabled{};
    for (std::uint32_t index{}; index < info->enabledExtensionCount; ++index) {
        if (std::strcmp(
                info->enabledExtensionNames[index],
                XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME
            ) == 0) {
            already_enabled = true;
            break;
        }
    }
    const bool inject_extension = !already_enabled && extension_available(next_gipa);
    std::vector<const char*> extensions;
    XrInstanceCreateInfo forwarded_info = *info;
    if (inject_extension) {
        if (info->enabledExtensionCount != 0U &&
            info->enabledExtensionNames != nullptr) {
            extensions.assign(
                info->enabledExtensionNames,
                info->enabledExtensionNames + info->enabledExtensionCount
            );
        }
        extensions.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
        forwarded_info.enabledExtensionCount = static_cast<std::uint32_t>(
            extensions.size()
        );
        forwarded_info.enabledExtensionNames = extensions.data();
    }

    XrApiLayerCreateInfo next_layer_info = *layer_info;
    next_layer_info.nextInfo = layer_info->nextInfo->next;
    const auto result = next_create(
        &forwarded_info, &next_layer_info, instance
    );
    if (XR_FAILED(result)) return result;

    InstanceState state{};
    state.instance = *instance;
    state.extension_enabled = already_enabled || inject_extension;
    populate_dispatch(state.dispatch, *instance, next_gipa);
    if (state.dispatch.get_instance_properties != nullptr) {
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(state.dispatch.get_instance_properties(
                *instance, &properties
            ))) {
            state.runtime_name = properties.runtimeName;
        }
    }
    static_cast<void>(create_gaze_action(state));

    {
        std::lock_guard lock(state_mutex);
        instances.emplace(*instance, std::move(state));
        publish_snapshot_locked(nullptr);
    }
    return result;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrDestroyInstance(
    const XrInstance instance
) {
    Dispatch dispatch{};
    XrAction action{XR_NULL_HANDLE};
    XrActionSet action_set{XR_NULL_HANDLE};
    {
        std::lock_guard lock(state_mutex);
        const auto iterator = instances.find(instance);
        if (iterator == instances.end()) return XR_ERROR_HANDLE_INVALID;
        dispatch = iterator->second.dispatch;
        action = iterator->second.gaze_action;
        action_set = iterator->second.action_set;
        for (auto session_it = sessions.begin(); session_it != sessions.end();) {
            if (session_it->second.instance == instance) {
                session_it = sessions.erase(session_it);
            } else {
                ++session_it;
            }
        }
        for (auto swapchain_it = swapchains.begin();
             swapchain_it != swapchains.end();) {
            if (sessions.find(swapchain_it->second.session) == sessions.end()) {
                swapchain_it = swapchains.erase(swapchain_it);
            } else {
                ++swapchain_it;
            }
        }
        instances.erase(iterator);
        publish_snapshot_locked(nullptr);
    }
    if (action != XR_NULL_HANDLE && dispatch.destroy_action != nullptr) {
        static_cast<void>(dispatch.destroy_action(action));
    }
    if (action_set != XR_NULL_HANDLE && dispatch.destroy_action_set != nullptr) {
        static_cast<void>(dispatch.destroy_action_set(action_set));
    }
    return dispatch.destroy_instance == nullptr
        ? XR_ERROR_FUNCTION_UNSUPPORTED
        : dispatch.destroy_instance(instance);
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrGetSystem(
    const XrInstance instance,
    const XrSystemGetInfo* const info,
    XrSystemId* const system_id
) {
    PFN_xrGetSystem next{};
    {
        std::lock_guard lock(state_mutex);
        const auto iterator = instances.find(instance);
        if (iterator == instances.end()) return XR_ERROR_HANDLE_INVALID;
        next = iterator->second.dispatch.get_system;
    }
    return next == nullptr
        ? XR_ERROR_FUNCTION_UNSUPPORTED
        : next(instance, info, system_id);
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrGetSystemProperties(
    const XrInstance instance,
    const XrSystemId system_id,
    XrSystemProperties* const properties
) {
    PFN_xrGetSystemProperties next{};
    {
        std::lock_guard lock(state_mutex);
        const auto iterator = instances.find(instance);
        if (iterator == instances.end()) return XR_ERROR_HANDLE_INVALID;
        next = iterator->second.dispatch.get_system_properties;
    }
    return next == nullptr
        ? XR_ERROR_FUNCTION_UNSUPPORTED
        : next(instance, system_id, properties);
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrCreateSession(
    const XrInstance instance,
    const XrSessionCreateInfo* const info,
    XrSession* const session
) {
    InstanceState* instance_state{};
    {
        std::lock_guard lock(state_mutex);
        const auto iterator = instances.find(instance);
        if (iterator == instances.end()) return XR_ERROR_HANDLE_INVALID;
        instance_state = &iterator->second;
    }
    if (instance_state->dispatch.create_session == nullptr) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    const auto result = instance_state->dispatch.create_session(
        instance, info, session
    );
    if (XR_FAILED(result)) return result;

    SessionState session_state{};
    session_state.session = *session;
    session_state.instance = instance;
    session_state.system_id = info->systemId;
    session_state.generation = next_session_generation.fetch_add(
        1U, std::memory_order_relaxed
    );
    if (instance_state->extension_enabled &&
        instance_state->dispatch.get_system_properties != nullptr) {
        XrSystemEyeGazeInteractionPropertiesEXT gaze_properties{
            XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT
        };
        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
        properties.next = &gaze_properties;
        if (XR_SUCCEEDED(instance_state->dispatch.get_system_properties(
                instance, info->systemId, &properties
            ))) {
            session_state.system_supported =
                gaze_properties.supportsEyeGazeInteraction == XR_TRUE;
        }
    }
    if (session_state.system_supported &&
        instance_state->gaze_action != XR_NULL_HANDLE &&
        instance_state->dispatch.create_action_space != nullptr) {
        XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        space_info.action = instance_state->gaze_action;
        space_info.poseInActionSpace.orientation.w = 1.0F;
        static_cast<void>(instance_state->dispatch.create_action_space(
            *session, &space_info, &session_state.gaze_space
        ));
    }

    {
        std::lock_guard lock(state_mutex);
        sessions.emplace(*session, session_state);
        publish_snapshot_locked(&sessions.at(*session));
    }
    return result;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrDestroySession(
    const XrSession session
) {
    Dispatch dispatch{};
    XrSpace gaze_space{XR_NULL_HANDLE};
    {
        std::lock_guard lock(state_mutex);
        const auto session_it = sessions.find(session);
        if (session_it == sessions.end()) return XR_ERROR_HANDLE_INVALID;
        const auto instance_it = instances.find(session_it->second.instance);
        if (instance_it == instances.end()) return XR_ERROR_HANDLE_INVALID;
        dispatch = instance_it->second.dispatch;
        gaze_space = session_it->second.gaze_space;
        sessions.erase(session_it);
        for (auto iterator = swapchains.begin(); iterator != swapchains.end();) {
            if (iterator->second.session == session) {
                iterator = swapchains.erase(iterator);
            } else {
                ++iterator;
            }
        }
        swapchain_generation.fetch_add(1U, std::memory_order_release);
        publish_snapshot_locked(nullptr);
    }
    if (gaze_space != XR_NULL_HANDLE && dispatch.destroy_space != nullptr) {
        static_cast<void>(dispatch.destroy_space(gaze_space));
    }
    return dispatch.destroy_session == nullptr
        ? XR_ERROR_FUNCTION_UNSUPPORTED
        : dispatch.destroy_session(session);
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrPollEvent(
    const XrInstance instance,
    XrEventDataBuffer* const event_data
) {
    PFN_xrPollEvent next{};
    {
        std::lock_guard lock(state_mutex);
        const auto iterator = instances.find(instance);
        if (iterator == instances.end()) return XR_ERROR_HANDLE_INVALID;
        next = iterator->second.dispatch.poll_event;
    }
    if (next == nullptr) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = next(instance, event_data);
    if (XR_SUCCEEDED(result) && event_data != nullptr &&
        event_data->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(
            event_data
        );
        std::lock_guard lock(state_mutex);
        const auto iterator = sessions.find(changed->session);
        if (iterator != sessions.end()) {
            iterator->second.state = changed->state;
            if (changed->state == XR_SESSION_STATE_STOPPING ||
                changed->state == XR_SESSION_STATE_LOSS_PENDING ||
                changed->state == XR_SESSION_STATE_EXITING) {
                iterator->second.action_active = false;
                iterator->second.gaze_valid = false;
                for (auto& view : iterator->second.submitted_views) view = {};
                swapchain_generation.fetch_add(
                    1U, std::memory_order_release
                );
            }
            publish_snapshot_locked(&iterator->second);
        }
    }
    return result;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrBeginSession(
    const XrSession session,
    const XrSessionBeginInfo* const info
) {
    PFN_xrBeginSession next{};
    {
        std::lock_guard lock(state_mutex);
        auto* instance = find_instance_for_session_locked(session);
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.begin_session;
    }
    const auto result = next == nullptr
        ? XR_ERROR_FUNCTION_UNSUPPORTED
        : next(session, info);
    if (XR_SUCCEEDED(result) && info != nullptr) {
        std::lock_guard lock(state_mutex);
        const auto iterator = sessions.find(session);
        if (iterator != sessions.end()) {
            iterator->second.view_configuration =
                info->primaryViewConfigurationType;
            iterator->second.unsupported_view_configuration =
                info->primaryViewConfigurationType !=
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            publish_snapshot_locked(&iterator->second);
        }
    }
    return result;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrEndSession(
    const XrSession session
) {
    PFN_xrEndSession next{};
    {
        std::lock_guard lock(state_mutex);
        auto* instance = find_instance_for_session_locked(session);
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.end_session;
        auto& session_state = sessions.at(session);
        session_state.action_active = false;
        session_state.gaze_valid = false;
        publish_snapshot_locked(&session_state);
    }
    return next == nullptr
        ? XR_ERROR_FUNCTION_UNSUPPORTED
        : next(session);
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
cheeky_xrSuggestInteractionProfileBindings(
    const XrInstance instance,
    const XrInteractionProfileSuggestedBinding* const suggested
) {
    if (suggested == nullptr) return XR_ERROR_VALIDATION_FAILURE;
    PFN_xrSuggestInteractionProfileBindings next{};
    XrAction gaze_action{XR_NULL_HANDLE};
    XrPath gaze_path{XR_NULL_PATH};
    XrPath gaze_profile{XR_NULL_PATH};
    {
        std::lock_guard lock(state_mutex);
        const auto iterator = instances.find(instance);
        if (iterator == instances.end()) return XR_ERROR_HANDLE_INVALID;
        next = iterator->second.dispatch.suggest_bindings;
        gaze_action = iterator->second.gaze_action;
        gaze_path = iterator->second.gaze_path;
        gaze_profile = iterator->second.gaze_profile;
    }
    if (next == nullptr) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (gaze_action == XR_NULL_HANDLE ||
        suggested->interactionProfile != gaze_profile) {
        return next(instance, suggested);
    }

    std::vector<XrActionSuggestedBinding> bindings;
    if (suggested->countSuggestedBindings != 0U &&
        suggested->suggestedBindings != nullptr) {
        bindings.assign(
            suggested->suggestedBindings,
            suggested->suggestedBindings + suggested->countSuggestedBindings
        );
    }
    const auto present = std::any_of(
        bindings.begin(), bindings.end(), [&](const auto& binding) {
            return binding.action == gaze_action && binding.binding == gaze_path;
        }
    );
    if (!present) bindings.push_back({gaze_action, gaze_path});
    auto merged = *suggested;
    merged.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size());
    merged.suggestedBindings = bindings.data();
    const auto result = next(instance, &merged);
    if (XR_SUCCEEDED(result)) {
        std::lock_guard lock(state_mutex);
        const auto iterator = instances.find(instance);
        if (iterator != instances.end()) {
            iterator->second.gaze_binding_submitted = true;
        }
    }
    return result;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrAttachSessionActionSets(
    const XrSession session,
    const XrSessionActionSetsAttachInfo* const attach_info
) {
    if (attach_info == nullptr) return XR_ERROR_VALIDATION_FAILURE;
    PFN_xrAttachSessionActionSets next{};
    XrActionSet layer_action_set{XR_NULL_HANDLE};
    {
        std::lock_guard lock(state_mutex);
        auto* instance = find_instance_for_session_locked(session);
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.attach_action_sets;
        layer_action_set = instance->action_set;
        static_cast<void>(ensure_gaze_binding_locked(*instance));
    }
    if (next == nullptr) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (layer_action_set == XR_NULL_HANDLE) return next(session, attach_info);

    std::vector<XrActionSet> action_sets;
    if (attach_info->countActionSets != 0U &&
        attach_info->actionSets != nullptr) {
        action_sets.assign(
            attach_info->actionSets,
            attach_info->actionSets + attach_info->countActionSets
        );
    }
    if (std::find(action_sets.begin(), action_sets.end(), layer_action_set) ==
        action_sets.end()) {
        action_sets.push_back(layer_action_set);
    }
    auto merged = *attach_info;
    merged.countActionSets = static_cast<std::uint32_t>(action_sets.size());
    merged.actionSets = action_sets.data();
    const auto result = next(session, &merged);
    if (XR_SUCCEEDED(result)) {
        std::lock_guard lock(state_mutex);
        const auto iterator = sessions.find(session);
        if (iterator != sessions.end()) {
            iterator->second.action_attached = true;
            publish_snapshot_locked(&iterator->second);
        }
    }
    return result;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrSyncActions(
    const XrSession session,
    const XrActionsSyncInfo* const sync_info
) {
    if (sync_info == nullptr) return XR_ERROR_VALIDATION_FAILURE;
    PFN_xrSyncActions next{};
    PFN_xrGetActionStatePose get_pose{};
    XrActionSet action_set{XR_NULL_HANDLE};
    XrAction gaze_action{XR_NULL_HANDLE};
    bool attached{};
    {
        std::lock_guard lock(state_mutex);
        auto* instance = find_instance_for_session_locked(session);
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.sync_actions;
        get_pose = instance->dispatch.get_action_state_pose;
        action_set = instance->action_set;
        gaze_action = instance->gaze_action;
        attached = sessions.at(session).action_attached;
    }
    if (next == nullptr) return XR_ERROR_FUNCTION_UNSUPPORTED;

    std::vector<XrActiveActionSet> active_sets;
    if (sync_info->countActiveActionSets != 0U &&
        sync_info->activeActionSets != nullptr) {
        active_sets.assign(
            sync_info->activeActionSets,
            sync_info->activeActionSets + sync_info->countActiveActionSets
        );
    }
    if (attached && action_set != XR_NULL_HANDLE) {
        const auto present = std::any_of(
            active_sets.begin(), active_sets.end(), [&](const auto& active) {
                return active.actionSet == action_set;
            }
        );
        if (!present) active_sets.push_back({action_set, XR_NULL_PATH});
    }
    auto merged = *sync_info;
    merged.countActiveActionSets = static_cast<std::uint32_t>(active_sets.size());
    merged.activeActionSets = active_sets.data();
    const auto result = next(session, &merged);

    bool active{};
    if (XR_SUCCEEDED(result) && attached && gaze_action != XR_NULL_HANDLE &&
        get_pose != nullptr) {
        XrActionStateGetInfo state_info{XR_TYPE_ACTION_STATE_GET_INFO};
        state_info.action = gaze_action;
        XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
        if (XR_SUCCEEDED(get_pose(session, &state_info, &state))) {
            active = state.isActive == XR_TRUE;
        }
    }
    {
        std::lock_guard lock(state_mutex);
        const auto iterator = sessions.find(session);
        if (iterator != sessions.end()) {
            iterator->second.action_active = active;
            if (!active) iterator->second.gaze_valid = false;
            publish_snapshot_locked(&iterator->second);
        }
    }
    return result;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrLocateViews(
    const XrSession session,
    const XrViewLocateInfo* const locate_info,
    XrViewState* const view_state,
    const std::uint32_t capacity,
    std::uint32_t* const count,
    XrView* const views
) {
    PFN_xrLocateViews next{};
    PFN_xrGetActionStatePose get_pose{};
    PFN_xrLocateSpace locate_space{};
    XrAction gaze_action{XR_NULL_HANDLE};
    XrSpace gaze_space{XR_NULL_HANDLE};
    bool action_attached{};
    {
        std::lock_guard lock(state_mutex);
        auto* instance = find_instance_for_session_locked(session);
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.locate_views;
        get_pose = instance->dispatch.get_action_state_pose;
        locate_space = instance->dispatch.locate_space;
        gaze_action = instance->gaze_action;
        const auto& session_state = sessions.at(session);
        gaze_space = session_state.gaze_space;
        action_attached = session_state.action_attached;
    }
    if (next == nullptr) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = next(
        session, locate_info, view_state, capacity, count, views
    );
    if (XR_FAILED(result) || locate_info == nullptr || count == nullptr ||
        views == nullptr || capacity < CHEEKY_GAZE_MAX_VIEWS ||
        *count != CHEEKY_GAZE_MAX_VIEWS) {
        return result;
    }

    bool action_active{};
    XrSpaceLocation gaze_location{XR_TYPE_SPACE_LOCATION};
    XrEyeGazeSampleTimeEXT sample_time{XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT};
    gaze_location.next = &sample_time;
    if (action_attached && gaze_action != XR_NULL_HANDLE &&
        gaze_space != XR_NULL_HANDLE && get_pose != nullptr &&
        locate_space != nullptr) {
        XrActionStateGetInfo state_info{XR_TYPE_ACTION_STATE_GET_INFO};
        state_info.action = gaze_action;
        XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
        if (XR_SUCCEEDED(get_pose(session, &state_info, &state)) &&
            state.isActive == XR_TRUE) {
            action_active = true;
            static_cast<void>(locate_space(
                gaze_space,
                locate_info->space,
                locate_info->displayTime,
                &gaze_location
            ));
        }
    }

    const bool simulated = simulated_gaze_enabled.load(std::memory_order_acquire);
    if (simulated && view_state != nullptr &&
        (view_state->viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0) {
        double elapsed{};
        {
            std::lock_guard lock(state_mutex);
            auto& state = sessions.at(session);
            if (!state.simulated) state.simulation_start = locate_info->displayTime;
            elapsed = static_cast<double>(locate_info->displayTime - state.simulation_start) * 1e-9;
        }
        // Average the eye orientations (same hemisphere) for a head-relative pose.
        auto head = convert_pose(views[0].pose);
        auto right = convert_pose(views[1].pose).orientation;
        auto& q = head.orientation;
        const float dot = q.x*right.x + q.y*right.y + q.z*right.z + q.w*right.w;
        const float sign = dot < 0.0F ? -1.0F : 1.0F;
        q = {q.x + sign*right.x, q.y + sign*right.y, q.z + sign*right.z, q.w + sign*right.w};
        const float length = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
        if (length > 0.0001F) {
            q = {q.x/length, q.y/length, q.z/length, q.w/length};
            const auto pose = cheeky::gaze_math::simulated_gaze_pose(head, elapsed);
            gaze_location.pose.orientation = {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
            gaze_location.locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            sample_time.time = locate_info->displayTime;
            action_active = true;
        }
    } else if (simulated) {
        action_active = false;
        gaze_location.locationFlags = 0;
    }

    const bool orientation_valid =
        (gaze_location.locationFlags &
         XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
    std::array<float, CHEEKY_GAZE_MAX_VIEWS> projected_u{};
    std::array<float, CHEEKY_GAZE_MAX_VIEWS> projected_v{};
    bool projections_valid = orientation_valid;
    if (orientation_valid) {
        const auto gaze_pose = convert_pose(gaze_location.pose);
        for (std::uint32_t index{}; index < CHEEKY_GAZE_MAX_VIEWS; ++index) {
            const cheeky::gaze_math::Fov fov{
                views[index].fov.angleLeft,
                views[index].fov.angleRight,
                views[index].fov.angleUp,
                views[index].fov.angleDown,
            };
            projections_valid &= cheeky::gaze_math::project_gaze_to_view(
                gaze_pose,
                convert_pose(views[index].pose),
                fov,
                projected_u[index],
                projected_v[index]
            );
        }
    }

    {
        std::lock_guard lock(state_mutex);
        const auto iterator = sessions.find(session);
        if (iterator != sessions.end()) {
            auto& state = iterator->second;
            state.unsupported_view_configuration =
                locate_info->viewConfigurationType !=
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            state.predicted_display_time = locate_info->displayTime;
            state.simulated = simulated;
            state.sample_time = sample_time.time;
            state.action_active = action_active;
            state.gaze_location_flags = static_cast<std::uint32_t>(
                gaze_location.locationFlags
            );
            state.gaze_valid = action_active && projections_valid &&
                !state.unsupported_view_configuration;
            if (state.gaze_valid) {
                state.center_u = projected_u;
                state.center_v = projected_v;
            }
            publish_snapshot_locked(&state);
        }
    }
    return result;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrCreateSwapchain(
    const XrSession session,
    const XrSwapchainCreateInfo* const create_info,
    XrSwapchain* const swapchain
) {
    PFN_xrCreateSwapchain next{};
    {
        std::lock_guard lock(state_mutex);
        auto* instance = find_instance_for_session_locked(session);
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.create_swapchain;
    }
    if (next == nullptr) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = next(session, create_info, swapchain);
    if (XR_SUCCEEDED(result) && create_info != nullptr && swapchain != nullptr) {
        std::lock_guard lock(state_mutex);
        SwapchainState state{};
        state.swapchain = *swapchain;
        state.session = session;
        state.create_info = *create_info;
        swapchains.emplace(*swapchain, std::move(state));
        swapchain_generation.fetch_add(1U, std::memory_order_release);
        const auto session_it = sessions.find(session);
        if (session_it != sessions.end()) {
            publish_snapshot_locked(&session_it->second);
        }
    }
    return result;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrDestroySwapchain(
    const XrSwapchain swapchain
) {
    PFN_xrDestroySwapchain next{};
    XrSession session{XR_NULL_HANDLE};
    {
        std::lock_guard lock(state_mutex);
        const auto swapchain_it = swapchains.find(swapchain);
        if (swapchain_it == swapchains.end()) return XR_ERROR_HANDLE_INVALID;
        session = swapchain_it->second.session;
        auto* instance = find_instance_for_session_locked(session);
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.destroy_swapchain;
        const auto session_it = sessions.find(session);
        if (session_it != sessions.end()) {
            for (auto& view : session_it->second.submitted_views) {
                if (view.swapchain == swapchain) view = {};
            }
        }
        swapchains.erase(swapchain_it);
        swapchain_generation.fetch_add(1U, std::memory_order_release);
        if (session_it != sessions.end()) {
            publish_snapshot_locked(&session_it->second);
        }
    }
    return next == nullptr
        ? XR_ERROR_FUNCTION_UNSUPPORTED
        : next(swapchain);
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrEnumerateSwapchainImages(
    const XrSwapchain swapchain,
    const std::uint32_t capacity,
    std::uint32_t* const count,
    XrSwapchainImageBaseHeader* const images
) {
    PFN_xrEnumerateSwapchainImages next{};
    {
        std::lock_guard lock(state_mutex);
        const auto swapchain_it = swapchains.find(swapchain);
        if (swapchain_it == swapchains.end()) return XR_ERROR_HANDLE_INVALID;
        auto* instance = find_instance_for_session_locked(
            swapchain_it->second.session
        );
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.enumerate_swapchain_images;
    }
    if (next == nullptr) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = next(swapchain, capacity, count, images);
    if (XR_FAILED(result) || count == nullptr || images == nullptr ||
        capacity < *count) {
        return result;
    }

    std::vector<std::uint64_t> identities(*count);
    if (*count != 0U && images[0].type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
        const auto* typed = reinterpret_cast<const XrSwapchainImageD3D11KHR*>(
            images
        );
        for (std::uint32_t index{}; index < *count; ++index) {
            identities[index] = canonical_resource_identity(typed[index].texture);
        }
    } else if (*count != 0U &&
               images[0].type == XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) {
        const auto* typed = reinterpret_cast<const XrSwapchainImageD3D12KHR*>(
            images
        );
        for (std::uint32_t index{}; index < *count; ++index) {
            identities[index] = canonical_resource_identity(typed[index].texture);
        }
    }

    std::lock_guard lock(state_mutex);
    const auto swapchain_it = swapchains.find(swapchain);
    if (swapchain_it != swapchains.end()) {
        swapchain_it->second.resource_identities = std::move(identities);
        const auto session_it = sessions.find(swapchain_it->second.session);
        if (session_it != sessions.end()) {
            update_view_resource_locked(session_it->second, swapchain);
            publish_snapshot_locked(&session_it->second);
        }
    }
    return result;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrAcquireSwapchainImage(
    const XrSwapchain swapchain,
    const XrSwapchainImageAcquireInfo* const acquire_info,
    std::uint32_t* const index
) {
    PFN_xrAcquireSwapchainImage next{};
    {
        std::lock_guard lock(state_mutex);
        const auto swapchain_it = swapchains.find(swapchain);
        if (swapchain_it == swapchains.end()) return XR_ERROR_HANDLE_INVALID;
        auto* instance = find_instance_for_session_locked(
            swapchain_it->second.session
        );
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.acquire_swapchain_image;
    }
    if (next == nullptr) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = next(swapchain, acquire_info, index);
    if (XR_SUCCEEDED(result) && index != nullptr) {
        std::lock_guard lock(state_mutex);
        const auto swapchain_it = swapchains.find(swapchain);
        if (swapchain_it != swapchains.end()) {
            swapchain_it->second.acquired_index = *index;
            swapchain_it->second.has_acquired = true;
            const auto session_it = sessions.find(swapchain_it->second.session);
            if (session_it != sessions.end()) {
                update_view_resource_locked(session_it->second, swapchain);
                publish_snapshot_locked(&session_it->second);
            }
        }
    }
    return result;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrWaitSwapchainImage(
    const XrSwapchain swapchain,
    const XrSwapchainImageWaitInfo* const wait_info
) {
    PFN_xrWaitSwapchainImage next{};
    {
        std::lock_guard lock(state_mutex);
        const auto swapchain_it = swapchains.find(swapchain);
        if (swapchain_it == swapchains.end()) return XR_ERROR_HANDLE_INVALID;
        auto* instance = find_instance_for_session_locked(
            swapchain_it->second.session
        );
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.wait_swapchain_image;
    }
    return next == nullptr
        ? XR_ERROR_FUNCTION_UNSUPPORTED
        : next(swapchain, wait_info);
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrReleaseSwapchainImage(
    const XrSwapchain swapchain,
    const XrSwapchainImageReleaseInfo* const release_info
) {
    PFN_xrReleaseSwapchainImage next{};
    XrSession session{XR_NULL_HANDLE};
    std::uint32_t acquired_index{};
    bool has_acquired{};
    {
        std::lock_guard lock(state_mutex);
        const auto swapchain_it = swapchains.find(swapchain);
        if (swapchain_it == swapchains.end()) return XR_ERROR_HANDLE_INVALID;
        session = swapchain_it->second.session;
        acquired_index = swapchain_it->second.acquired_index;
        has_acquired = swapchain_it->second.has_acquired;
        auto* instance = find_instance_for_session_locked(session);
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.release_swapchain_image;
    }
    if (next == nullptr) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = next(swapchain, release_info);
    if (XR_SUCCEEDED(result) && has_acquired) {
        std::lock_guard lock(state_mutex);
        const auto swapchain_it = swapchains.find(swapchain);
        if (swapchain_it != swapchains.end()) {
            swapchain_it->second.released_index = acquired_index;
            swapchain_it->second.has_released = true;
            swapchain_it->second.has_acquired = false;
            const auto session_it = sessions.find(session);
            if (session_it != sessions.end()) {
                update_view_resource_locked(session_it->second, swapchain);
                publish_snapshot_locked(&session_it->second);
            }
        }
    }
    return result;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrEndFrame(
    const XrSession session,
    const XrFrameEndInfo* const frame_end_info
) {
    PFN_xrEndFrame next{};
    {
        std::lock_guard lock(state_mutex);
        auto* instance = find_instance_for_session_locked(session);
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.end_frame;
    }
    if (next == nullptr) return XR_ERROR_FUNCTION_UNSUPPORTED;

    const auto result = next(session, frame_end_info);
    if (XR_FAILED(result)) return result;

    if (frame_end_info != nullptr) {
        std::lock_guard lock(state_mutex);
        const auto session_it = sessions.find(session);
        if (session_it != sessions.end()) {
            auto& state = session_it->second;
            bool found_projection{};
            state.ambiguous_resource = false;
            for (std::uint32_t layer_index{};
                 layer_index < frame_end_info->layerCount; ++layer_index) {
                const auto* layer = frame_end_info->layers[layer_index];
                if (layer == nullptr ||
                    layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    continue;
                }
                const auto* projection =
                    reinterpret_cast<const XrCompositionLayerProjection*>(layer);
                if (projection->viewCount != CHEEKY_GAZE_MAX_VIEWS) {
                    state.unsupported_view_configuration = true;
                    continue;
                }
                found_projection = true;
                for (std::uint32_t view_index{};
                     view_index < CHEEKY_GAZE_MAX_VIEWS; ++view_index) {
                    const auto& sub_image =
                        projection->views[view_index].subImage;
                    auto& submitted = state.submitted_views[view_index];
                    submitted.swapchain = sub_image.swapchain;
                    submitted.rect = sub_image.imageRect;
                    submitted.array_index = sub_image.imageArrayIndex;
                    if (sub_image.imageArrayIndex != 0U) {
                        state.ambiguous_resource = true;
                    }
                    submitted.resource_identity = 0U;
                    submitted.valid = false;
                    update_view_resource_locked(state, sub_image.swapchain);
                }
                break;
            }
            if (!found_projection) {
                for (auto& view : state.submitted_views) view = {};
            }
            if (state.ambiguous_resource) {
                for (auto& view : state.submitted_views) view.valid = false;
            }
            publish_snapshot_locked(&state);
        }
    }
    return result;
}

extern "C" __declspec(dllexport) XRAPI_ATTR XrResult XRAPI_CALL
xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* const loader_info,
    const char* const requested_layer_name,
    XrNegotiateApiLayerRequest* const request
) {
    if (loader_info == nullptr || request == nullptr ||
        loader_info->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loader_info->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loader_info->structSize < sizeof(XrNegotiateLoaderInfo) ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        request->structSize < sizeof(XrNegotiateApiLayerRequest) ||
        loader_info->minInterfaceVersion >
            XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader_info->maxInterfaceVersion <
            XR_CURRENT_LOADER_API_LAYER_VERSION ||
        requested_layer_name == nullptr ||
        std::strcmp(requested_layer_name, layer_name) != 0) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    for (auto& slot : snapshot_slots) {
        initialize_snapshot(slot.snapshot);
        slot.snapshot.status_flags = CHEEKY_GAZE_STATUS_LAYER_ACTIVE;
        slot.snapshot.publication_qpc = query_qpc();
    }
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = (std::min)(
        loader_info->maxApiVersion, XR_CURRENT_API_VERSION
    );
    request->getInstanceProcAddr = cheeky_xrGetInstanceProcAddr;
    request->createApiLayerInstance = cheeky_xrCreateApiLayerInstance;
    return XR_SUCCESS;
}

BOOL WINAPI DllMain(
    const HINSTANCE module,
    const DWORD reason,
    LPVOID
) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        for (auto& slot : snapshot_slots) {
            initialize_snapshot(slot.snapshot);
            slot.snapshot.status_flags = CHEEKY_GAZE_STATUS_LAYER_ACTIVE;
            slot.snapshot.publication_qpc = query_qpc();
        }
    }
    return TRUE;
}
