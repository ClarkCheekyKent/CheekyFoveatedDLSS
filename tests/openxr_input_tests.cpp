#include <Windows.h>
#include "../third_party/openxr/include/openxr/openxr.h"
#include "../third_party/openxr/include/openxr/openxr_loader_negotiation.h"
#include "cheeky_gaze_abi.h"
#include "gaze_foveation.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T handle(std::uintptr_t n) { return reinterpret_cast<T>(n); }
struct Runtime {
    inline static bool extension = true, supported = true, focused = true, active = true;
    inline static bool attached{}, synced{}, injected{};
    inline static XrResult attach_result = XR_SUCCESS, sync_result = XR_SUCCESS,
        binding_result = XR_SUCCESS, locate_result = XR_SUCCESS, space_result = XR_SUCCESS;
    inline static unsigned attaches{}, syncs{}, bindings{}, locates{}, created{};
    inline static std::vector<XrActionSet> attached_sets;
    inline static std::vector<XrActiveActionSet> active_sets;
    static void reset() {
        extension = supported = focused = active = true;
        attached = synced = injected = false;
        attach_result = sync_result = binding_result = locate_result = space_result = XR_SUCCESS;
        attaches = syncs = bindings = locates = created = 0;
        attached_sets.clear(); active_sets.clear();
    }
    static XrResult XRAPI_CALL create(const XrInstanceCreateInfo* info,
        const XrApiLayerCreateInfo*, XrInstance* instance) {
        for (unsigned i = 0; i < info->enabledExtensionCount; ++i)
            injected |= !strcmp(info->enabledExtensionNames[i], XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
        *instance = handle<XrInstance>(1); return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL get(XrInstance, const char* name, PFN_xrVoidFunction* out) {
        *out = nullptr;
#define FN(n, ...) if (!strcmp(name, n)) { *out = reinterpret_cast<PFN_xrVoidFunction>(+__VA_ARGS__); return XR_SUCCESS; }
        FN("xrEnumerateInstanceExtensionProperties", [](const char*, uint32_t capacity, uint32_t* count, XrExtensionProperties* props) {
            *count = extension ? 1 : 0;
            if (extension && capacity) strcpy_s(props[0].extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
            return XR_SUCCESS;
        });
        FN("xrGetInstanceProperties", [](XrInstance, XrInstanceProperties* p) { strcpy_s(p->runtimeName, "Gaze mock"); return XR_SUCCESS; });
        FN("xrDestroyInstance", [](XrInstance) { return XR_SUCCESS; });
        FN("xrCreateSession", [](XrInstance, const XrSessionCreateInfo*, XrSession* s) { *s = handle<XrSession>(2); return XR_SUCCESS; });
        FN("xrDestroySession", [](XrSession) { return XR_SUCCESS; });
        FN("xrBeginSession", [](XrSession, const XrSessionBeginInfo*) { return XR_SUCCESS; });
        FN("xrEndSession", [](XrSession) { return XR_SUCCESS; });
        FN("xrGetSystemProperties", [](XrInstance, XrSystemId, XrSystemProperties* p) {
            auto* gaze = static_cast<XrSystemEyeGazeInteractionPropertiesEXT*>(p->next);
            if (gaze) gaze->supportsEyeGazeInteraction = supported; return XR_SUCCESS;
        });
        FN("xrCreateActionSet", [](XrInstance, const XrActionSetCreateInfo*, XrActionSet* set) {
            *set = handle<XrActionSet>(10 + created++); return XR_SUCCESS;
        });
        FN("xrDestroyActionSet", [](XrActionSet) { return XR_SUCCESS; });
        FN("xrCreateAction", [](XrActionSet, const XrActionCreateInfo*, XrAction* a) { *a = handle<XrAction>(20); return XR_SUCCESS; });
        FN("xrDestroyAction", [](XrAction) { return XR_SUCCESS; });
        FN("xrStringToPath", [](XrInstance, const char* path, XrPath* p) {
            *p = strstr(path, "interaction_profiles") ? 31 : 30; return XR_SUCCESS;
        });
        FN("xrSuggestInteractionProfileBindings", [](XrInstance, const XrInteractionProfileSuggestedBinding* info) {
            ++bindings;
            require(info->countSuggestedBindings == 1 && info->suggestedBindings[0].action == handle<XrAction>(20), "Gaze binding missing");
            return binding_result;
        });
        FN("xrCreateActionSpace", [](XrSession, const XrActionSpaceCreateInfo*, XrSpace* space) {
            if (XR_SUCCEEDED(space_result)) *space = handle<XrSpace>(40); return space_result;
        });
        FN("xrDestroySpace", [](XrSpace) { return XR_SUCCESS; });
        FN("xrAttachSessionActionSets", [](XrSession, const XrSessionActionSetsAttachInfo* info) {
            ++attaches;
            if (attached) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
            if (XR_FAILED(attach_result)) return attach_result;
            attached_sets.assign(info->actionSets, info->actionSets + info->countActionSets);
            attached = true; return XR_SUCCESS;
        });
        FN("xrSyncActions", [](XrSession, const XrActionsSyncInfo* info) {
            ++syncs;
            require(attached, "Sync occurred before attach");
            active_sets.assign(info->activeActionSets, info->activeActionSets + info->countActiveActionSets);
            if (!focused) { synced = false; return XR_SESSION_NOT_FOCUSED; }
            synced = sync_result == XR_SUCCESS; return sync_result;
        });
        FN("xrGetActionStatePose", [](XrSession, const XrActionStateGetInfo*, XrActionStatePose* p) {
            p->isActive = attached && synced && focused && active; return XR_SUCCESS;
        });
        FN("xrLocateSpace", [](XrSpace, XrSpace, XrTime time, XrSpaceLocation* p) {
            ++locates;
            // Deliberately set flags even on failure: the layer must reject them.
            p->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            p->pose.orientation.w = 1;
            static_cast<XrEyeGazeSampleTimeEXT*>(p->next)->time = time;
            return locate_result;
        });
        FN("xrLocateViews", [](XrSession, const XrViewLocateInfo*, XrViewState* state, uint32_t cap, uint32_t* count, XrView* views) {
            *count = 2;
            if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
            state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT;
            for (unsigned i = 0; i < 2; ++i) { views[i].pose.orientation.w = 1; views[i].fov = {-0.8F,0.8F,0.8F,-0.8F}; }
            return XR_SUCCESS;
        });
        FN("xrPollEvent", [](XrInstance, XrEventDataBuffer* buffer) {
            XrEventDataSessionStateChanged event{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
            event.session = handle<XrSession>(2);
            event.state = focused ? XR_SESSION_STATE_FOCUSED : XR_SESSION_STATE_VISIBLE;
            memcpy(buffer, &event, sizeof(event)); return XR_SUCCESS;
        });
#undef FN
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
};
struct Layer {
    HMODULE module{};
    XrInstance instance{};
    XrSession session{};
    PFN_xrGetInstanceProcAddr get{};
    template<class T> T fn(const char* name) {
        PFN_xrVoidFunction f{};
        require(get(instance, name, &f) == XR_SUCCESS && f, "Missing layer function");
        return reinterpret_cast<T>(f);
    }
    Layer() {
        module = LoadLibraryW(L"CheekyOpenXRLayer.dll"); require(module != nullptr, "Missing layer DLL");
        const auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(GetProcAddress(module, "xrNegotiateLoaderApiLayerInterface"));
        XrNegotiateLoaderInfo loader{};
        loader.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO; loader.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
        loader.structSize = sizeof(loader); loader.minInterfaceVersion = loader.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
        loader.maxApiVersion = XR_CURRENT_API_VERSION;
        XrNegotiateApiLayerRequest request{};
        request.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST; request.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION;
        request.structSize = sizeof(request);
        require(negotiate && negotiate(&loader, "XR_APILAYER_CHEEKY_foveated_dlss", &request) == XR_SUCCESS, "Negotiation failed");
        get = request.getInstanceProcAddr;
        XrApiLayerNextInfo next{}; next.nextGetInstanceProcAddr = Runtime::get; next.nextCreateApiLayerInstance = Runtime::create;
        XrApiLayerCreateInfo layer{}; layer.nextInfo = &next;
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        require(request.createApiLayerInstance(&info, &layer, &instance) == XR_SUCCESS, "Create instance failed");
        XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO}; create.systemId = 1;
        require(fn<PFN_xrCreateSession>("xrCreateSession")(instance, &create, &session) == XR_SUCCESS, "Create session failed");
        begin(); focus(true);
    }
    void begin() {
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO}; begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        require(fn<PFN_xrBeginSession>("xrBeginSession")(session, &begin) == XR_SUCCESS, "Begin failed");
    }
    void focus(bool value) {
        Runtime::focused = value; XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        require(fn<PFN_xrPollEvent>("xrPollEvent")(instance, &event) == XR_SUCCESS, "Focus event failed");
    }
    CheekyGazeSnapshotV1 locate(XrTime time) {
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO}; info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        info.displayTime = time; info.space = handle<XrSpace>(50);
        XrViewState state{XR_TYPE_VIEW_STATE}; XrView views[2]{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}; uint32_t count{};
        require(fn<PFN_xrLocateViews>("xrLocateViews")(session, &info, &state, 2, &count, views) == XR_SUCCESS, "Locate views failed");
        CheekyGazeSnapshotV1 result{};
        const auto read = reinterpret_cast<CheekyOpenXRGetGazeSnapshotFn>(GetProcAddress(module, "CheekyOpenXR_GetGazeSnapshot"));
        require(read(CHEEKY_GAZE_ABI_VERSION, &result, sizeof(result)) != 0, "Snapshot failed"); return result;
    }
    CheekyGazeInputDiagnosticsV1 diagnostics() {
        CheekyGazeInputDiagnosticsV1 result{};
        const auto read = reinterpret_cast<CheekyOpenXRGetGazeInputDiagnosticsFn>(GetProcAddress(module, "CheekyOpenXR_GetGazeInputDiagnostics"));
        require(read && read(CHEEKY_GAZE_INPUT_DIAGNOSTICS_VERSION, &result, sizeof(result)), "Input diagnostics failed");
        return result;
    }
    XrActionSet create_host_set() {
        XrActionSet set{}; XrActionSetCreateInfo info{XR_TYPE_ACTION_SET_CREATE_INFO};
        require(fn<PFN_xrCreateActionSet>("xrCreateActionSet")(instance, &info, &set) == XR_SUCCESS, "Host set failed"); return set;
    }
    XrResult attach(XrActionSet set) {
        XrSessionActionSetsAttachInfo info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO}; info.countActionSets = 1; info.actionSets = &set;
        return fn<PFN_xrAttachSessionActionSets>("xrAttachSessionActionSets")(session, &info);
    }
    void sync(XrActionSet set) {
        XrActiveActionSet active{set, 55}; XrActionsSyncInfo info{XR_TYPE_ACTIONS_SYNC_INFO}; info.countActiveActionSets = 1; info.activeActionSets = &active;
        require(fn<PFN_xrSyncActions>("xrSyncActions")(session, &info) == XR_SUCCESS, "Host sync failed");
    }
    ~Layer() {
        fn<PFN_xrDestroySession>("xrDestroySession")(session);
        fn<PFN_xrDestroyInstance>("xrDestroyInstance")(instance);
        FreeLibrary(module);
    }
};
bool valid(const CheekyGazeSnapshotV1& s) { return (s.status_flags & CHEEKY_GAZE_STATUS_GAZE_VALID) != 0; }
}
int run_openxr_input_tests() {
    HMODULE realvr{};
    try {
        Runtime::reset();
        { Layer layer; require(!valid(layer.locate(1)) && !Runtime::attaches && !Runtime::syncs,
            "Ordinary apps must not independently attach/sync"); }
        Runtime::reset();
        {
            Layer layer; const auto host = layer.create_host_set();
            require(layer.attach(host) == XR_SUCCESS, "Ordinary host attach failed");
            layer.sync(host);
            require(valid(layer.locate(1)) && Runtime::syncs == 1 && Runtime::attaches == 1,
                "Ordinary host must retain merged input without fallback calls");
            require(!layer.diagnostics().realvr_detected && !layer.diagnostics().fallback_sync_calls,
                "Ordinary host must not enable RealVR fallback");
        }
        // Load the existing version-resource fixture under a supported proxy name.
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
        const auto bin = std::filesystem::path(executable).parent_path();
        const auto dir = bin / "test-fixtures" / "gaze-input";
        std::filesystem::create_directories(dir);
        std::filesystem::copy_file(bin / "test-fixtures" / "CheekyFakeRealVR.dll", dir / "RealVR64.dll", std::filesystem::copy_options::overwrite_existing);
        realvr = LoadLibraryW((dir / "RealVR64.dll").c_str()); require(realvr != nullptr, "RealVR fixture missing");
        Runtime::reset();
        {
            Layer layer;
            require(Runtime::injected, "Layer must enable gaze extension when app did not request it");
            require(valid(layer.locate(1)), "No-input RealVR must acquire live gaze");
            require(Runtime::bindings == 1 && Runtime::attaches == 1 && Runtime::syncs == 1, "Initial binding/attach/sync counts");
            layer.locate(1); require(Runtime::syncs == 1, "Repeated LocateViews must not sync twice at the same display time");
            layer.locate(2); require(Runtime::attaches == 1 && Runtime::syncs == 2, "Attach once, sync each frame");
            const auto d = layer.diagnostics();
            require(d.realvr_detected && d.action_attached && d.fallback_attach_calls == 1 && d.fallback_sync_calls == 2 && !d.host_sync_calls && d.pose_result == XR_SUCCESS, "Fallback diagnostics missing");
            const auto live = cheeky::foveated_dlss::gaze_diagnostics();
            require(live.input.session_generation == d.session_generation && live.input.fallback_sync_calls == 2,
                "Support diagnostics must be available without a DLSS evaluation");
            const auto json = cheeky::foveated_dlss::gaze_input_diagnostics_json(d);
            require(json.find("\"fallback_sync_calls\":2") != std::string::npos, "Support JSON missing input state");
            layer.focus(false); require(!valid(layer.locate(3)) && Runtime::syncs == 2, "Unfocused session must clear gaze and stop polling");
            layer.focus(true); require(valid(layer.locate(4)) && Runtime::attaches == 1, "Focus recovery must reuse attachment");
            Runtime::active = false; require(!valid(layer.locate(5)), "Inactive provider must not become synthetic gaze"); Runtime::active = true;
            Runtime::sync_result = XR_SESSION_NOT_FOCUSED; require(!valid(layer.locate(6)), "Positive NOT_FOCUSED result must not pass gaze");
            Runtime::sync_result = XR_ERROR_RUNTIME_FAILURE; require(!valid(layer.locate(7)), "Failed sync must clear old gaze");
            Runtime::sync_result = XR_SUCCESS; Runtime::locate_result = XR_ERROR_RUNTIME_FAILURE;
            require(!valid(layer.locate(8)), "Failed space locate must reject stale flags"); Runtime::locate_result = XR_SUCCESS;
            require(valid(layer.locate(9)), "Transient failures must recover");
            layer.fn<PFN_xrEndSession>("xrEndSession")(layer.session);
            const auto calls = Runtime::syncs; require(!valid(layer.locate(10)) && Runtime::syncs == calls, "Stopped session must not poll or publish gaze");
            layer.begin(); require(valid(layer.locate(11)) && Runtime::attaches == 1, "Session restart must reuse action attachment");
            const auto host = layer.create_host_set();
            require(layer.attach(host) == XR_ERROR_ACTIONSETS_ALREADY_ATTACHED, "Late attachment error must be forwarded, never fake success");
        }
        Runtime::reset();
        {
            Layer layer; const auto host = layer.create_host_set();
            layer.locate(1); require(Runtime::attaches == 0, "Host-created action sets must reserve attachment for host");
            require(layer.attach(host) == XR_SUCCESS && Runtime::attached_sets.size() == 2 && Runtime::attached_sets[0] == host,
                "Host attachment must preserve sets and append gaze");
            require(valid(layer.locate(2)), "Attached host without sync should receive fallback gaze");
            layer.sync(host);
            require(Runtime::active_sets.size() == 2 && Runtime::active_sets[0].actionSet == host && Runtime::active_sets[0].subactionPath == 55,
                "Host active sets and subaction paths must be preserved");
            const auto calls = Runtime::syncs; layer.locate(3); layer.locate(4);
            require(Runtime::syncs == calls, "After host sync, independent sync must stop to preserve controllers");
            require(layer.diagnostics().host_attach_calls == 1 && layer.diagnostics().host_sync_calls == 1, "Host input diagnostics missing");
        }
        for (unsigned failure = 0; failure < 5; ++failure) {
            Runtime::reset();
            if (failure == 0) Runtime::extension = false;
            if (failure == 1) Runtime::supported = false;
            if (failure == 2) Runtime::binding_result = XR_ERROR_PATH_UNSUPPORTED;
            if (failure == 3) Runtime::attach_result = XR_ERROR_RUNTIME_FAILURE;
            if (failure == 4) Runtime::space_result = XR_ERROR_RUNTIME_FAILURE;
            Layer layer; require(!valid(layer.locate(1)) && !valid(layer.locate(2)) && !Runtime::syncs,
                "Unsupported/failed setup must never synchronize or publish gaze");
            require(Runtime::attaches <= 1 && Runtime::bindings <= 1, "Setup failures must not retry every frame");
            const auto d = layer.diagnostics();
            if (failure == 2) require(d.binding_result == XR_ERROR_PATH_UNSUPPORTED, "Binding failure diagnostic");
            if (failure == 3) require(d.attach_result == XR_ERROR_RUNTIME_FAILURE, "Attach failure diagnostic");
            if (failure == 4) require(d.space_result == XR_ERROR_RUNTIME_FAILURE, "Space failure diagnostic");
        }
        FreeLibrary(realvr);
        std::cout << "PASS: OpenXR independent RealVR gaze, host actions, lifecycle and failure diagnostics\n";
        return 0;
    } catch (const std::exception& e) {
        if (realvr) FreeLibrary(realvr);
        std::cerr << "OpenXR input: " << e.what() << '\n'; return 1;
    }
}
