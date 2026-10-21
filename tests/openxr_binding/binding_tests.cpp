// Exercise production interceptors with a downstream runtime; no headset needed.
#include <d3d12.h>
void fg_barrier_observer(ID3D12Resource*,D3D12_RESOURCE_STATES,D3D12_RESOURCE_STATES);
void fg_submission_observer(bool);
#define CHEEKY_FG_BARRIER_OBSERVER fg_barrier_observer
#define CHEEKY_FG_SUBMISSION_OBSERVER fg_submission_observer
#include "../../openxr_layer/openxr_layer.cpp"
#include <iostream>
#include <stdexcept>
#include "../../standalone/menu_backend.hpp"
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include "../../src/fg_snapshot.hpp"
namespace {
bool observe_fg_barriers{};
bool fg_observed_generated{};
unsigned fg_barrier_errors{};
std::unordered_map<ID3D12Resource*,D3D12_RESOURCE_STATES> fg_expected_states;
}
void fg_submission_observer(bool generated) {fg_observed_generated=generated;}
void fg_barrier_observer(ID3D12Resource* texture,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    if(!observe_fg_barriers)return;
    auto [it,inserted]=fg_expected_states.try_emplace(texture,D3D12_RESOURCE_STATE_COMMON);
    if(it->second!=from) {
        ++fg_barrier_errors;
        std::cerr << "FG resource-state contract mismatch: expected=" << it->second << " actual=" << from << '\n';
    }
    it->second=to;
}
namespace {
template<class T> T h(uintptr_t n) { return reinterpret_cast<T>(n); }
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::unordered_map<XrPath, std::vector<XrActionSuggestedBinding>> runtime_bindings;
XrResult suggestion_result = XR_SUCCESS;
unsigned suggestion_calls{};
std::vector<XrActionSet> attached_sets;
std::vector<XrActiveActionSet> synchronized_sets;
XrResult sync_result = XR_SUCCESS, pose_result = XR_SUCCESS;
XrBool32 pose_active = XR_TRUE;
unsigned pose_calls{};
unsigned input_calls{};
XrResult input_result = XR_SUCCESS;
template<class T> XrResult XRAPI_CALL input(XrSession, const XrActionStateGetInfo*, T* output) {
    ++input_calls;
    if (XR_SUCCEEDED(input_result)) {
        output->isActive = XR_TRUE; output->changedSinceLastSync = XR_TRUE; output->lastChangeTime = 123;
        if constexpr (std::is_same_v<T, XrActionStateVector2f>) output->currentState = {0.25f, -0.75f};
        else output->currentState = 1;
    }
    return input_result;
}
template<class T, class Fn> void test_input(Fn fn, XrStructureType type) {
    for (const auto status : {XR_SUCCESS, XR_ERROR_RUNTIME_FAILURE}) {
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO}; get.action = h<XrAction>(201); get.subactionPath = 401;
        T output{type}; output.lastChangeTime = 456;
        input_result = status; const auto before = input_calls;
        check(fn(h<XrSession>(2), &get, &output) == status, "input result changed");
        check(input_calls == before + 1, "input queried twice");
        check(output.lastChangeTime == (XR_SUCCEEDED(status) ? 123 : 456), "input output modified");
        if (XR_SUCCEEDED(status)) {
            check(output.isActive && output.changedSinceLastSync, "input flags lost");
            if constexpr (std::is_same_v<T, XrActionStateVector2f>)
                check(output.currentState.x == .25f && output.currentState.y == -.75f, "stick values lost");
            else check(output.currentState == 1, "button/trigger value lost");
        }
    }
}
XrResult XRAPI_CALL sync(XrSession, const XrActionsSyncInfo* info) {
    synchronized_sets.assign(info->activeActionSets, info->activeActionSets + info->countActiveActionSets);
    return sync_result;
}
XrResult XRAPI_CALL pose(XrSession, const XrActionStateGetInfo*, XrActionStatePose* output) {
    ++pose_calls;
    if (XR_SUCCEEDED(pose_result)) output->isActive = pose_active;
    return pose_result;
}
XrResult XRAPI_CALL suggest(XrInstance, const XrInteractionProfileSuggestedBinding* info) {
    ++suggestion_calls;
    if (XR_SUCCEEDED(suggestion_result))
        runtime_bindings[info->interactionProfile] = std::vector<XrActionSuggestedBinding>(
            info->suggestedBindings, info->suggestedBindings + info->countSuggestedBindings);
    return suggestion_result;
}
XrResult XRAPI_CALL path(XrInstance, const char* name, XrPath* result) {
    uint64_t value = 1469598103934665603ULL;
    for (; *name; ++name) value = (value ^ static_cast<unsigned char>(*name)) * 1099511628211ULL;
    *result = value; return XR_SUCCESS;
}
XrResult XRAPI_CALL session_create(XrInstance, const XrSessionCreateInfo*, XrSession* result) {
    *result = h<XrSession>(2); return XR_SUCCESS;
}
XrResult XRAPI_CALL attach(XrSession, const XrSessionActionSetsAttachInfo* info) {
    attached_sets.assign(info->actionSets, info->actionSets + info->countActionSets);
    return XR_SUCCESS;
}
constexpr XrPath profile = 100;
const XrInstance instance = h<XrInstance>(1);
const XrAction host_action = h<XrAction>(201);
void reset() {
    sessions.clear(); instances.clear(); runtime_bindings.clear(); attached_sets.clear();
    suggestion_result = XR_SUCCESS; suggestion_calls = 0;
    InstanceState state{}; state.instance = instance;
    state.dispatch.suggest_bindings = suggest; state.dispatch.string_to_path = path;
    state.dispatch.create_session = session_create; state.dispatch.attach_action_sets = attach;
    state.dispatch.sync_actions = sync; state.dispatch.get_action_state_pose = pose;
    state.dispatch.get_action_state_boolean = input<XrActionStateBoolean>;
    state.dispatch.get_action_state_float = input<XrActionStateFloat>;
    state.dispatch.get_action_state_vector2f = input<XrActionStateVector2f>;
    state.menu_hand_paths = {401, 402};
    state.menu_action_set = h<XrActionSet>(10);
    state.menu_aim_action = h<XrAction>(20); state.menu_click_action = h<XrAction>(21);
    state.menu_profiles[0] = profile;
    instances.emplace(instance, std::move(state));
}
XrResult host_suggest(XrAction action = host_action, const void* next = nullptr) {
    const XrActionSuggestedBinding binding{action, 301};
    XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    info.next = next; info.interactionProfile = profile;
    info.countSuggestedBindings = 1; info.suggestedBindings = &binding;
    return cheeky_xrSuggestInteractionProfileBindings(instance, &info);
}
void create_graphics_session(bool d3d12) {
    XrGraphicsBindingD3D11KHR binding11{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    XrGraphicsBindingD3D12KHR binding12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO}; info.systemId = 1;
    info.next = d3d12 ? static_cast<void*>(&binding12) : static_cast<void*>(&binding11);
    XrSession session{};
    check(cheeky_xrCreateSession(instance, &info, &session) == XR_SUCCESS, "session creation");
}
void attach_host() {
    const auto host_set = h<XrActionSet>(11);
    XrSessionActionSetsAttachInfo info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    info.countActionSets = 1; info.actionSets = &host_set;
    check(cheeky_xrAttachSessionActionSets(h<XrSession>(2), &info) == XR_SUCCESS, "host attach");
    check(attached_sets.size() == 2 && attached_sets[0] == host_set, "host action set preserved");
}
bool contains(XrAction action) {
    const auto& bindings = runtime_bindings[profile];
    return std::any_of(bindings.begin(), bindings.end(), [&](auto& v) { return v.action == action; });
}
void run() {
    for (bool d3d12 : {false, true}) {
        reset(); check(host_suggest() == XR_SUCCESS, "early suggestion accepted");
        check(runtime_bindings[profile].size() == 1, "headless suggestion unchanged");
        create_graphics_session(d3d12); attach_host();
        check(contains(host_action), "early host bindings erased by menu setup");
        check(runtime_bindings[profile].size() == 5, "host plus four menu bindings");
        const auto calls = suggestion_calls; ensure_menu_bindings(instances.at(instance));
        check(suggestion_calls == calls, "already installed bindings submitted twice");
        reset(); create_graphics_session(d3d12); host_suggest(); attach_host();
        check(contains(host_action) && runtime_bindings[profile].size() == 5, "late merge lost bindings");
        reset(); host_suggest(); const auto replacement = h<XrAction>(202);
        host_suggest(replacement); create_graphics_session(d3d12); attach_host();
        check(contains(replacement) && !contains(host_action), "replacement resurrected stale binding");
        reset(); host_suggest(); suggestion_result = XR_ERROR_RUNTIME_FAILURE;
        check(XR_FAILED(host_suggest(replacement)), "failed suggestion propagated");
        suggestion_result = XR_SUCCESS; create_graphics_session(d3d12); attach_host();
        check(contains(host_action) && !contains(replacement), "failed suggestion replaced good cache");
        reset(); XrBaseInStructure unknown{static_cast<XrStructureType>(1999999999), nullptr};
        host_suggest(host_action, &unknown); create_graphics_session(d3d12); attach_host();
        check(runtime_bindings[profile].size() == 1 && contains(host_action), "unknown chain overwritten");
        reset(); create_graphics_session(d3d12); attach_host();
        check(runtime_bindings[profile].size() == 4 && !contains(host_action), "instance leaked old bindings");
        const XrActiveActionSet host_set{h<XrActionSet>(11), 401};
        test_input<XrActionStateBoolean>(cheeky_xrGetActionStateBoolean, XR_TYPE_ACTION_STATE_BOOLEAN);
        test_input<XrActionStateFloat>(cheeky_xrGetActionStateFloat, XR_TYPE_ACTION_STATE_FLOAT);
        test_input<XrActionStateVector2f>(cheeky_xrGetActionStateVector2f, XR_TYPE_ACTION_STATE_VECTOR2F);
        XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
        sync_info.countActiveActionSets = 1; sync_info.activeActionSets = &host_set;
        for (const auto status : {XR_SUCCESS, XR_SESSION_NOT_FOCUSED, XR_ERROR_RUNTIME_FAILURE}) {
            sync_result = status;
            check(cheeky_xrSyncActions(h<XrSession>(2), &sync_info) == status, "sync result changed by diagnostics");
            check(synchronized_sets.size() == 2 && synchronized_sets[0].actionSet == host_set.actionSet &&
                synchronized_sets[0].subactionPath == 401, "host sync lost set or subaction path");
        }
        for (const auto status : {XR_SUCCESS, XR_ERROR_RUNTIME_FAILURE}) {
            for (const auto active : {XR_FALSE, XR_TRUE}) {
                XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO}; get.action = host_action; get.subactionPath = 401;
                XrActionStatePose output{XR_TYPE_ACTION_STATE_POSE}; output.isActive = XR_TRUE;
                pose_result = status; pose_active = active; const auto calls_before = pose_calls;
                check(cheeky_xrGetActionStatePose(h<XrSession>(2), &get, &output) == status, "pose result changed");
                check(pose_calls == calls_before + 1, "pose called more than once");
                check(output.isActive == (XR_SUCCEEDED(status) ? active : XR_TRUE), "pose output modified");
            }
        }
    }
}
unsigned instance_attempts{};
XrResult XRAPI_CALL unavailable_gipa(XrInstance, const char*, PFN_xrVoidFunction* output) {
    *output = nullptr; return XR_ERROR_FUNCTION_UNSUPPORTED;
}
XrResult XRAPI_CALL create_without_cylinders(const XrInstanceCreateInfo* info,
    const XrApiLayerCreateInfo*, XrInstance* output) {
    ++instance_attempts;
    bool gaze{};
    for (unsigned i = 0; i < info->enabledExtensionCount; ++i) {
        if (!std::strcmp(info->enabledExtensionNames[i], XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME))
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        gaze |= !std::strcmp(info->enabledExtensionNames[i], XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    }
    check(gaze, "gaze request lost with cylinder workaround");
    *output = h<XrInstance>(999); return XR_SUCCESS;
}
void startup_test() {
    for (bool cyberpunk : {true, false}) {
        instances.clear(); sessions.clear(); instance_attempts = 0;
        XrApiLayerNextInfo next{};
        next.nextGetInstanceProcAddr = unavailable_gipa;
        next.nextCreateApiLayerInstance = create_without_cylinders;
        XrApiLayerCreateInfo layer{}; layer.nextInfo = &next;
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(info.applicationInfo.applicationName, cyberpunk ? "CyberpunkVRPort" : "OtherApp");
        XrInstance instance{};
        check(cheeky_xrCreateApiLayerInstance(&info, &layer, &instance) == XR_SUCCESS, "startup failed");
        check(instance_attempts == (cyberpunk ? 1U : 2U), "unexpected speculative startup attempt");
        check(!instances.at(instance).cylinder_enabled, "unsupported cylinder enabled");
        check(instances.at(instance).extension_enabled, "gaze extension disabled");
    }
}
bool clock_advertised{},clock_requested{};
XrResult XRAPI_CALL clock_extensions(const char*,std::uint32_t capacity,std::uint32_t* count,XrExtensionProperties* props){
    *count=clock_advertised?1u:0u;
    if(capacity&&*count){strcpy_s(props[0].extensionName,XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);props[0].extensionVersion=1;}
    return XR_SUCCESS;
}
XrResult XRAPI_CALL clock_convert(XrInstance,XrTime time,LARGE_INTEGER* qpc){qpc->QuadPart=time;return XR_SUCCESS;}
XrResult XRAPI_CALL clock_gipa(XrInstance,const char* name,PFN_xrVoidFunction* out){
    *out=nullptr;
    if(!std::strcmp(name,"xrEnumerateInstanceExtensionProperties"))*out=reinterpret_cast<PFN_xrVoidFunction>(clock_extensions);
    if(!std::strcmp(name,"xrConvertTimeToWin32PerformanceCounterKHR"))*out=reinterpret_cast<PFN_xrVoidFunction>(clock_convert);
    return *out?XR_SUCCESS:XR_ERROR_FUNCTION_UNSUPPORTED;
}
XrResult XRAPI_CALL clock_create(const XrInstanceCreateInfo* info,const XrApiLayerCreateInfo*,XrInstance* out){
    clock_requested=false;
    for(unsigned i=0;i<info->enabledExtensionCount;++i)clock_requested|=!std::strcmp(info->enabledExtensionNames[i],XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
    *out=h<XrInstance>(999);return XR_SUCCESS;
}
void clock_extension_test(){
    for(bool supported:{false,true}){
        instances.clear();sessions.clear();clock_advertised=supported;
        XrApiLayerNextInfo next{};next.nextGetInstanceProcAddr=clock_gipa;next.nextCreateApiLayerInstance=clock_create;
        XrApiLayerCreateInfo layer{};layer.nextInfo=&next;XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};XrInstance created{};
        check(XR_SUCCEEDED(cheeky_xrCreateApiLayerInstance(&info,&layer,&created)),"Clock extension instance creation");
        check(clock_requested==supported&&bool(instances.at(created).dispatch.time_to_qpc)==supported,
            "Predicted display conversion is enabled only with confirmed extension support; a stray function pointer is insufficient");
    }
    std::cout<<"PASS optional display clock extension: confirmed support only; unavailable runtimes retain measured submission latency\n";
}
void menu_policy_test() {
    cheeky::standalone::MenuPointerQueue queue;
    queue.push({10, 20, true, true});
    queue.push({30, 40, true, true});
    queue.push({50, 60, true, true});
    queue.push({70, 80, false, true});
    queue.push({90, 100, false, true});
    queue.push({110, 120, false, true});
    check(queue.count == 4, "Only motion may coalesce between pointer edges");
    auto event = queue.pop();
    check(event.down && event.x == 10, "Queued press moved from its original target");
    event = queue.pop();
    check(event.down && event.x == 50, "Drag motion did not coalesce");
    event = queue.pop();
    check(!event.down && event.x == 70, "Queued release moved from its original target");
    event = queue.pop();
    check(!event.down && event.x == 110, "Hover motion did not coalesce");
    queue.push({10, 20, true, true});
    queue.push({30, 40, true, true});
    check(queue.pop().x == 10, "Press after draining the queue lost its original position");
    using cheeky::standalone::controller_pointer_owns;
    check(!controller_pointer_owns(true, false, false, 10000, 9999), "hover stole desktop mouse");
    check(controller_pointer_owns(true, false, true, 10000, 9999), "controller click cannot take ownership");
    check(controller_pointer_owns(true, true, false, 10000, 9999), "controller drag lost ownership");
    check(!controller_pointer_owns(false, true, false, 10000, 9999), "ray loss did not release");
    check(!controller_pointer_owns(true, false, true, 10000, 1, true), "controller stole held mouse drag");
    using cheeky::standalone::openxr_menu_owns;
    check(!openxr_menu_owns(false, true, 10000, 0), "loaded layer stole native OpenVR host");
    check(!openxr_menu_owns(false, true, 10000, 1000), "stale XR request stole host");
    check(openxr_menu_owns(false, true, 10000, 9999), "active XR menu not selected");
    check(openxr_menu_owns(true, true, 20000, 9999), "stall enabled a second menu backend");
    check(!openxr_menu_owns(false, false, 10000, 9999), "missing layer selected");
    using namespace cheeky::xr_menu;
    check(fallback_layers(0) == 0, "menu exceeded exhausted layer budget");
    for (unsigned budget : {1U, 2U, 12U, 16U, 64U}) {
        check(fallback_layers(budget) == 1, "fallback still emits cropped repeated layers");
        const auto panel = strip(1.2f, 621, 0, fallback_layers(budget));
        check(panel.left == 0 && panel.right == 621 && panel.width == 1.2f,
            "fallback crops or distorts source image");
        check(panel.angle == 0 && panel.center.x == 0 && panel.center.z == 0, "flat panel geometry");
        float u{}, v{};
        check(hit({0,0,1.5f}, {0,0,-1}, 1.2f, .8f, 621, 1, false, u, v) &&
            std::abs(u-.5f) < .001f && std::abs(v-.5f) < .001f, "flat menu pointer center");
        check(!hit({2,0,1.5f}, {0,0,-1}, 1.2f, .8f, 621, 1, false, u, v), "pointer outside panel");
    }
    check((swapchain_usage & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) != 0 &&
          (swapchain_usage & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0 &&
          (swapchain_usage & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0,
          "menu texture usage missing");
}
}
namespace {
float fallback_trigger[2]{};
bool fallback_tracking = true;
XrPath fallback_profile = profile;
unsigned fallback_creates{}, fallback_destroys{};
cheeky::standalone::MenuPointer received_pointer;
void __cdecl pointer_sink(float x, float y, bool down, bool active) noexcept {
    received_pointer = {x, y, down, active};
}
XrResult XRAPI_CALL fallback_current_profile(XrSession, XrPath, XrInteractionProfileState* output) {
    output->interactionProfile = fallback_profile; return XR_SUCCESS;
}
XrResult XRAPI_CALL fallback_space(XrSession, const XrActionSpaceCreateInfo* info, XrSpace* space) {
    check(info->action == h<XrAction>(510) || info->action == h<XrAction>(511), "Fallback selected wrong aim action");
    check(info->subactionPath == 401 || info->subactionPath == 402, "Fallback lost hand subaction");
    ++fallback_creates; *space = h<XrSpace>(1000 + info->subactionPath - 401); return XR_SUCCESS;
}
XrResult XRAPI_CALL fallback_destroy(XrSpace) { ++fallback_destroys; return XR_SUCCESS; }
XrResult XRAPI_CALL fallback_locate(XrSpace space, XrSpace, XrTime, XrSpaceLocation* output) {
    output->locationFlags = 0;
    if (fallback_tracking && (space == h<XrSpace>(1000) || space == h<XrSpace>(1001))) {
        output->locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        output->pose.orientation.w = 1.F; output->pose.position = {0, 0, 1.5F};
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL unbound_menu_click(XrSession, const XrActionStateGetInfo*, XrActionStateBoolean* output) {
    output->isActive = XR_FALSE; output->currentState = XR_FALSE; return XR_SUCCESS;
}
XrResult XRAPI_CALL bound_host_trigger(XrSession, const XrActionStateGetInfo* info, XrActionStateFloat* output) {
    check(info->action == h<XrAction>(500) || info->action == h<XrAction>(501), "Fallback selected wrong trigger");
    check(info->subactionPath == 401 || info->subactionPath == 402, "Fallback trigger lost subaction");
    output->isActive = XR_TRUE; output->currentState = fallback_trigger[info->subactionPath - 401]; return XR_SUCCESS;
}
void custom_binding_menu_test() {
    reset();
    auto& inst = instances.at(instance);
    inst.dispatch.get_current_profile = fallback_current_profile;
    inst.dispatch.create_action_space = fallback_space;
    inst.dispatch.destroy_space = fallback_destroy;
    inst.dispatch.locate_space = fallback_locate;
    inst.dispatch.get_action_state_boolean = unbound_menu_click;
    inst.dispatch.get_action_state_float = bound_host_trigger;
    for (unsigned hand = 0; hand < 2; ++hand) {
        const std::string prefix = hand ? "/user/hand/right" : "/user/hand/left";
        XrPath aim{}, trigger{};
        path(instance, (prefix + "/input/aim/pose").c_str(), &aim);
        path(instance, (prefix + "/input/trigger/value").c_str(), &trigger);
        inst.host_menu_profile_bindings[0].push_back({h<XrAction>(510+hand), aim});
        inst.host_menu_profile_bindings[0].push_back({h<XrAction>(500+hand), trigger});
    }
    SessionState state{}; state.instance = instance; state.session = h<XrSession>(2); state.action_attached = true;
    state.menu_aim_spaces = {h<XrSpace>(900), h<XrSpace>(901)};
    state.menu.pose.orientation.w = 1.F; state.menu.size = {2.F, 2.F}; state.menu.width = 800; state.menu.strips = 1;
    CheekyOpenXRMenuFrame frame{}; frame.display_width = 800; frame.display_height = 600;
    const auto bindings_before = suggestion_calls;
    fallback_creates = fallback_destroys = 0;
    fallback_trigger[0] = 0; fallback_trigger[1] = .9F;
    send_menu_pointer(state, inst.dispatch, 123, frame, pointer_sink);
    check(received_pointer.active && received_pointer.down && state.menu_pointer_hand == 1,
        "Custom binding without Cheeky actions must still aim/select using host");
    check(std::abs(received_pointer.x-400) < .01F && std::abs(received_pointer.y-300) < .01F,
        "Fallback ray-to-menu coordinates wrong");
    check(fallback_creates == 2, "Expected one fallback aim space per hand");
    fallback_trigger[1] = 0;
    send_menu_pointer(state, inst.dispatch, 124, frame, pointer_sink);
    check(received_pointer.active && !received_pointer.down, "Fallback trigger release lost");
    fallback_trigger[0] = .9F;
    send_menu_pointer(state, inst.dispatch, 125, frame, pointer_sink);
    check(received_pointer.down && state.menu_pointer_hand == 0, "Left-hand trigger must select");
    check(fallback_creates == 2 && suggestion_calls == bindings_before,
        "Fallback recreated spaces or overwrote user bindings");
    fallback_tracking = false;
    send_menu_pointer(state, inst.dispatch, 126, frame, pointer_sink);
    check(!received_pointer.active && !received_pointer.down, "Tracking loss must release pointer");
    fallback_profile = 999;
    send_menu_pointer(state, inst.dispatch, 127, frame, pointer_sink);
    check(!received_pointer.active && fallback_destroys == 2, "Profile change must destroy stale fallback spaces");
    fallback_profile = profile; fallback_tracking = true;
    std::cout << "PASS custom SteamVR binding: host fallback aim, both triggers, release, ray loss, profile cleanup\n";
}
void fg_native_handoff_test() {
    reset();swapchains.clear();
    const auto session=h<XrSession>(2);
    SessionState owner{};owner.session=session;owner.instance=instance;owner.running=true;
    sessions.emplace(session,std::move(owner));
    static std::mutex runtime_mx;
    static std::condition_variable runtime_cv;
    static unsigned waits,begins,ends,discarded;
    static bool cancel,second_wait_entered,fail_begin,fail_end;
    waits=begins=ends=discarded=0;cancel=second_wait_entered=false;fail_begin=fail_end=true;
    auto& d=instances.at(instance).dispatch;
    d.wait_frame=[](XrSession,const XrFrameWaitInfo*,XrFrameState*) {return XR_ERROR_RUNTIME_FAILURE;};
    XrFrameState failed_wait{XR_TYPE_FRAME_STATE};
    check(cheeky_xrWaitFrame(session,nullptr,&failed_wait)==XR_ERROR_RUNTIME_FAILURE &&
        sessions.at(session).native_waits_pending==0,"Failed native wait must not retain frame ownership");
    d.wait_frame=[](XrSession,const XrFrameWaitInfo*,XrFrameState* frame) {
        std::unique_lock lock(runtime_mx);
        if(waits==1){second_wait_entered=true;runtime_cv.notify_all();}
        if(!runtime_cv.wait_for(lock,std::chrono::seconds(2),[]{return cancel||waits==begins;})||cancel)
            return XR_ERROR_RUNTIME_FAILURE;
        ++waits;frame->predictedDisplayTime=1000+waits*100;frame->predictedDisplayPeriod=10000000;
        frame->shouldRender=XR_FALSE;return XR_SUCCESS;
    };
    d.begin_frame=[](XrSession,const XrFrameBeginInfo*) {
        std::unique_lock lock(runtime_mx);
        if(fail_begin){fail_begin=false;return XR_ERROR_RUNTIME_FAILURE;}
        // A runtime may discard the prior unended frame, rather than block.
        // The presenter must not rely on BeginFrame waiting for native EndFrame.
        if(cancel)return XR_ERROR_RUNTIME_FAILURE;
        const bool discard=begins!=ends;
        if(discard)++discarded;
        ++begins;runtime_cv.notify_all();return discard?XR_FRAME_DISCARDED:XR_SUCCESS;
    };
    d.end_frame=[](XrSession,const XrFrameEndInfo*) {
        std::lock_guard lock(runtime_mx);
        if(fail_end){fail_end=false;return XR_ERROR_RUNTIME_FAILURE;}
        ++ends;runtime_cv.notify_all();return XR_SUCCESS;
    };
    XrFrameState first{XR_TYPE_FRAME_STATE};
    check(XR_SUCCEEDED(cheeky_xrWaitFrame(session,nullptr,&first)),"Native pre-activation wait");
    auto presenter=std::make_shared<cheeky::fg::BufferedPresenter>();
    presenter->session=session;presenter->xr.wait_frame=d.wait_frame;
    presenter->xr.begin_frame=d.begin_frame;presenter->xr.end_frame=d.end_frame;
    sessions.at(session).fg=presenter;
    presenter->set_native_frame_pending(true);
    XrResult result=XR_ERROR_RUNTIME_FAILURE;XrFrameState second{XR_TYPE_FRAME_STATE};
    std::thread next_wait([&]{result=presenter->wait_frame(nullptr,&second);});
    bool premature_wait{};
    {std::unique_lock lock(runtime_mx);premature_wait=runtime_cv.wait_for(lock,std::chrono::milliseconds(50),[]{return second_wait_entered;});}
    const auto failed_begin_result=cheeky_xrBeginFrame(session,nullptr);
    const bool begin_owner_retained=sessions.at(session).native_waits_pending==1;
    const auto begin_result=cheeky_xrBeginFrame(session,nullptr);
    // Widen the handoff race: the native renderer is still working while
    // the application's next WaitFrame is already inside the new presenter.
    {std::unique_lock lock(runtime_mx);premature_wait|=runtime_cv.wait_for(lock,std::chrono::milliseconds(50),[]{return second_wait_entered;});}
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=first.predictedDisplayTime;
    const auto failed_end_result=cheeky_xrEndFrame(session,&end);
    const bool end_owner_retained=sessions.at(session).native_frame_begun;
    {std::unique_lock lock(runtime_mx);premature_wait|=runtime_cv.wait_for(lock,std::chrono::milliseconds(50),[]{return second_wait_entered;});}
    const auto end_result=cheeky_xrEndFrame(session,&end);
    next_wait.join();
    {std::lock_guard lock(runtime_mx);cancel=true;runtime_cv.notify_all();}
    presenter->invalidate_temporal();
    check(failed_begin_result==XR_ERROR_RUNTIME_FAILURE && begin_owner_retained,
        "Failed native begin must retain ownership for retry after activation");
    check(failed_end_result==XR_ERROR_RUNTIME_FAILURE && end_owner_retained,
        "Failed native end must retain ownership for retry after activation");
    check(!premature_wait,"Presenter must not acquire a runtime frame before native EndFrame completes");
    check(second_wait_entered&&discarded==0,"Runtime frame handoff must not discard the older native frame");
    check(XR_SUCCEEDED(begin_result)&&XR_SUCCEEDED(end_result),"Old native Begin/End must retain downstream ownership");
    check(XR_SUCCEEDED(result),"Forwarding the old Begin/End must release the new presenter wait");
    check(presenter->begun==0&&presenter->ended==0,"Old native frame must not consume presenter frame counters");
    sessions.clear();presenter.reset();
    std::cout<<"PASS pipelined native WaitFrame -> FG enable -> native BeginFrame/EndFrame handoff\n";
}

void fg_lifecycle_retirement_test() {
    reset();swapchains.clear();
    struct RetirementProbe final : cheeky::fg::Presenter {
        bool retired{},outside_state_lock{};
        void invalidate_temporal() noexcept override {
            retired=true;
            std::thread callback([&]{outside_state_lock=state_mutex.try_lock();
                if(outside_state_lock)state_mutex.unlock();});
            callback.join();
        }
    };
    const auto session=h<XrSession>(2);
    SessionState owner{};owner.session=session;owner.instance=instance;
    owner.fg_requested=true;owner.native_waits_pending=1;owner.native_frame_begun=true;
    auto first=std::make_shared<RetirementProbe>();owner.fg=first;
    sessions.emplace(session,std::move(owner));
    auto& dispatch=instances.at(instance).dispatch;
    dispatch.begin_session=[](XrSession,const XrSessionBeginInfo*) {return XR_SUCCESS;};
    dispatch.end_session=[](XrSession) {return XR_SUCCESS;};
    dispatch.destroy_swapchain=[](XrSwapchain) {return XR_SUCCESS;};
    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};begin.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    check(XR_SUCCEEDED(cheeky_xrBeginSession(session,&begin)) && first->retired && first->outside_state_lock,
        "Session restart must retire the old presenter outside the layer state lock");
    auto& live=sessions.at(session);
    check(live.running&&!live.fg&&!live.fg_requested&&!live.native_waits_pending&&!live.native_frame_begun,
        "Session restart must reacquire presenter setup and clear old frame ownership");
    auto second=std::make_shared<RetirementProbe>();live.fg=second;live.fg_requested=true;
    check(XR_SUCCEEDED(cheeky_xrEndSession(session)) && second->retired && second->outside_state_lock &&
        !live.fg&&!live.fg_requested&&!live.running,"Session end must detach the stopped presenter");
    auto third=std::make_shared<RetirementProbe>();live.fg=third;live.fg_requested=true;live.running=true;
    const auto chain=h<XrSwapchain>(70);SwapchainState swapchain;swapchain.session=session;
    swapchains.emplace(chain,std::move(swapchain));
    check(XR_SUCCEEDED(cheeky_xrDestroySwapchain(chain)) && third->retired && third->outside_state_lock &&
        !live.fg&&!live.fg_requested,"Swapchain replacement must rebuild setup instead of reusing a stopped presenter");
    sessions.clear();
    std::cout<<"PASS FG session restart/end and swapchain retirement outside the state lock\n";
}

void fg_borrowed_frame_shutdown_test() {
    static std::mutex runtime_mutex;
    static std::condition_variable runtime_changed;
    static bool in_end,release_end;
    static std::atomic<unsigned> waits{};
    in_end=release_end=false;waits=0;
    cheeky::fg::BufferedPresenter presenter;
    presenter.session=h<XrSession>(2);
    presenter.xr.wait_frame=[](XrSession,const XrFrameWaitInfo*,XrFrameState* state) {
        Sleep(1);state->predictedDisplayTime=1000+100*++waits;
        state->predictedDisplayPeriod=10000000;state->shouldRender=XR_FALSE;return XR_SUCCESS;
    };
    presenter.xr.begin_frame=[](XrSession,const XrFrameBeginInfo*) {return XR_SUCCESS;};
    presenter.xr.end_frame=[](XrSession,const XrFrameEndInfo* end) {
        if(end->next==h<void*>(1)) {
            std::unique_lock lock(runtime_mutex);in_end=true;runtime_changed.notify_all();
            runtime_changed.wait(lock,[]{return release_end;});
        }
        return XR_SUCCESS;
    };
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    check(XR_SUCCEEDED(presenter.wait_frame(nullptr,&frame)),"Borrowed frame runtime bootstrap");
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.next=h<void*>(1);end.displayTime=frame.predictedDisplayTime;
    std::atomic<bool> returned{};XrResult result=XR_ERROR_RUNTIME_FAILURE;
    std::thread sender([&]{result=presenter.passthrough(&end,"test extension chain");returned=true;});
    bool entered{};
    {std::unique_lock lock(runtime_mutex);entered=runtime_changed.wait_for(lock,std::chrono::seconds(2),[]{return in_end;});}
    std::thread stopper([&]{presenter.stop();});
    bool stopping{};
    for(unsigned i=0;i<2000&&!stopping;++i){
        {std::lock_guard lock(presenter.mx);stopping=presenter.stopping;}
        if(!stopping)Sleep(1);
    }
    Sleep(20);const bool returned_early=returned;
    {std::lock_guard lock(runtime_mutex);release_end=true;runtime_changed.notify_all();}
    stopper.join();sender.join();
    check(entered&&stopping&&!returned_early&&result==XR_SUCCESS,
        "Stop must retain borrowed next/layers storage until the runtime EndFrame returns");
    cheeky::fg::BufferedPresenter gated;gated.xr=presenter.xr;gated.set_native_frame_pending(true);
    const auto before=waits.load();bool started{};
    {std::lock_guard lock(gated.mx);started=gated.start();}
    gated.stop();
    check(started&&waits==before,"Stopping a pending native handoff must not acquire or strand a runtime frame");
    cheeky::fg::BufferedPresenter failed;failed.started=true;failed.stopping=true;
    failed.terminal=XR_ERROR_CALL_ORDER_INVALID;
    check(failed.wait_frame(nullptr,&frame)==XR_ERROR_CALL_ORDER_INVALID &&
        failed.begin_frame(nullptr)==XR_ERROR_CALL_ORDER_INVALID &&
        failed.end_frame(&end,{},nullptr)==XR_ERROR_CALL_ORDER_INVALID &&
        failed.passthrough(&end,"late shutdown request")==XR_ERROR_CALL_ORDER_INVALID,
        "All application frame entry points must preserve the terminal runtime error without publishing another borrowed frame");
    std::cout<<"PASS borrowed OpenXR frame lifetime during shutdown and pending handoff cancellation\n";
}

void fg_toggle_test() {
    reset();
    const auto session=h<XrSession>(2);
    auto& d=instances.at(instance).dispatch;
    static std::atomic<unsigned> waits{},begins{},ends{};
    waits=begins=ends=0;
    d.wait_frame=[](XrSession,const XrFrameWaitInfo*,XrFrameState* state) {
        Sleep(1);const auto tick=++waits;state->predictedDisplayPeriod=100;state->predictedDisplayTime=1000+100*tick;
        state->shouldRender=XR_TRUE;return XR_SUCCESS;
    };
    d.begin_frame=[](XrSession,const XrFrameBeginInfo*) {++begins;return XR_SUCCESS;};
    d.end_frame=[](XrSession,const XrFrameEndInfo*) {++ends;return XR_SUCCESS;};
    d.create_swapchain=[](XrSession,const XrSwapchainCreateInfo*,XrSwapchain*) {return XR_ERROR_RUNTIME_FAILURE;};
    d.destroy_swapchain=[](XrSwapchain) {return XR_SUCCESS;};
    d.enumerate_swapchain_images=[](XrSwapchain,uint32_t,uint32_t*,XrSwapchainImageBaseHeader*) {return XR_ERROR_RUNTIME_FAILURE;};
    d.acquire_swapchain_image=[](XrSwapchain,const XrSwapchainImageAcquireInfo*,uint32_t*) {return XR_ERROR_RUNTIME_FAILURE;};
    d.wait_swapchain_image=[](XrSwapchain,const XrSwapchainImageWaitInfo*) {return XR_ERROR_RUNTIME_FAILURE;};
    d.release_swapchain_image=[](XrSwapchain,const XrSwapchainImageReleaseInfo*) {return XR_ERROR_RUNTIME_FAILURE;};
    SessionState owner{};owner.session=session;owner.instance=instance;
    owner.running=true;owner.graphics_api=11;owner.view_configuration=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    update_fg_at_frame_boundary(owner,d,true);
    check(!owner.fg && owner.fg_state==CheekyFgState::presenter_failed,"DX11 without a usable graphics device must preserve native presentation");
    owner.graphics_api=12;
    update_fg_at_frame_boundary(owner,d,true);
    check(!owner.fg && owner.fg_state==CheekyFgState::presenter_failed,"Presenter failure must be reported");
    update_fg_at_frame_boundary(owner,d,false);
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    check(SUCCEEDED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"FG test D3D12 device");
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(SUCCEEDED(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue))),"FG test queue");
    owner.graphics_device=device.Get();owner.graphics_queue=queue.Get();
    sessions.emplace(session,std::move(owner));
    auto& live=sessions.at(session);
    update_fg_at_frame_boundary(live,d,true);
    check(live.fg!=nullptr,"Saved toggle must create presenter without an environment variable");
    auto first=live.fg;
    update_fg_at_frame_boundary(live,d,true);
    check(live.fg==first,"Repeated enable must preserve frame history");
    first->configure(101);first->held_valid=true;first->native_count=7;first->generated_count=2;
    first->configure(101);
    check(first->held_valid,"Unchanged FG quality controls must preserve history");
    first->worker.failed=true;first->configure(102);
    check(!first->held_valid&&!first->worker.failed&&first->native_count==7&&first->generated_count==2,
        "Changing FG quality must clear stale resources/failures while preserving lifetime counters");
    first->held_valid=true;first->frames=1;
    XrFrameWaitInfo info{XR_TYPE_FRAME_WAIT_INFO};XrFrameState state{XR_TYPE_FRAME_STATE};
    check(XR_SUCCEEDED(first->wait_frame(&info,&state)) && waits>=1 && begins>=1 &&
        state.predictedDisplayPeriod==100 && first->fallback_count==0,"Enabling FG without a generated frame must preserve native cadence");
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};empty.displayTime=state.predictedDisplayTime;
    check(XR_SUCCEEDED(first->begin_frame(&begin))&&XR_SUCCEEDED(first->end_frame(&empty,{},nullptr)),
        "Buffered application frames must close through the presentation owner");
    CheekyFgStatus status;
    check(CheekyFG_GetStatus(&status) && status.active && status.compatible_session &&
        status.fallback_frames==0,"FG status must expose real presenter counters");
    status.abi=999;
    check(!CheekyFG_GetStatus(&status),"Reject mismatched FG status ABI");
    CheekyFgPerformance performance;
    check(CheekyFG_GetPerformance(&performance)&&!performance.gpu_valid&&performance.gpu_samples==0&&
        std::abs(performance.xr_period_ms-.0001)<1e-9,"FG performance must preserve raw headset period before any cadence scaling");
    performance.abi=999;
    check(!CheekyFG_GetPerformance(&performance),"Reject mismatched FG performance ABI");
    CheekyFgCadence cadence;
    check(CheekyFG_GetCadenceV1(&cadence)&&cadence.supported,"Optional measured-cadence export");
    cadence.abi=999;check(!CheekyFG_GetCadenceV1(&cadence),"Reject mismatched cadence ABI");
    CheekyFgNativeSubmissionLatency native_latency;
    check(CheekyFG_GetNativeSubmissionLatencyV1(&native_latency)&&native_latency.supported,
        "Optional native GPU-ready-to-OpenXR submit diagnostic export");
    native_latency.abi=999;
    check(!CheekyFG_GetNativeSubmissionLatencyV1(&native_latency),"Reject mismatched native latency ABI");
    native_latency={};--native_latency.size;
    check(!CheekyFG_GetNativeSubmissionLatencyV1(&native_latency)&&
        !CheekyFG_GetNativeSubmissionLatencyV1(nullptr),"Reject invalid native latency size/pointer");
    check(std::abs(cheeky::fg::native_submission_delta_ms(150000,100000,125000,1000000.)-25.)<1e-9,
        "Native GPU-ready timestamp chooses the later stereo eye");
    check(std::abs(cheeky::fg::native_submission_delta_ms(125000,150000,175000,1000000.)+50.)<1e-9,
        "Native GPU-ready timestamp preserves early OpenXR submission as negative time");
    cheeky::fg::SignedTimingAverage signed_native_samples;
    signed_native_samples.add(-15.);signed_native_samples.add(5.);
    check(std::abs(signed_native_samples.mean()+5.)<1e-9,"Native signed timing window keeps negative samples");
    CheekyFgPresentation presentation;
    check(CheekyFG_GetPresentationV1(&presentation)&&presentation.supported,"Optional capture-age and late-wait export");
    presentation.abi=999;check(!CheekyFG_GetPresentationV1(&presentation),"Reject mismatched presentation telemetry ABI");
    presentation={};--presentation.size;
    check(!CheekyFG_GetPresentationV1(&presentation)&&!CheekyFG_GetPresentationV1(nullptr),"Reject invalid presentation size/pointer");
    CheekyFgExperimentDiagnostics experiments;
    check(CheekyFG_GetExperimentDiagnosticsV1(&experiments)&&experiments.supported,"Optional transport diagnostics export");
    experiments.abi=999;check(!CheekyFG_GetExperimentDiagnosticsV1(&experiments),"Reject mismatched experiment ABI");
    experiments={};--experiments.size;
    check(!CheekyFG_GetExperimentDiagnosticsV1(&experiments)&&!CheekyFG_GetExperimentDiagnosticsV1(nullptr),"Reject invalid experiment size/pointer");
    CheekyFgDirectOutput direct;
    check(CheekyFG_GetDirectOutputDiagnosticsV1(&direct)&&direct.supported,"Direct output diagnostics export");
    direct.abi=999;check(!CheekyFG_GetDirectOutputDiagnosticsV1(&direct),"Reject mismatched direct output ABI");
    direct={};--direct.size;
    check(!CheekyFG_GetDirectOutputDiagnosticsV1(&direct)&&!CheekyFG_GetDirectOutputDiagnosticsV1(nullptr),"Reject invalid direct output size/pointer");
    CheekyFgDirectOutputDetails details;
    check(CheekyFG_GetDirectOutputDetailsV1(&details)&&details.supported,"Detailed direct-output rejection export");
    details.abi=999;check(!CheekyFG_GetDirectOutputDetailsV1(&details),"Reject mismatched direct detail ABI");
    auto retired=update_fg_at_frame_boundary(live,d,false);
    check(!live.fg && retired==first,"Disable must retire the presenter at the frame boundary");
    first.reset();retired.reset();
    waits=begins=ends=0;
    check(XR_SUCCEEDED(cheeky_xrWaitFrame(session,&info,&state)) && waits==1 && !begins && !ends &&
        state.predictedDisplayPeriod==100,"Disabled FG must restore the ordinary application cadence");
    update_fg_at_frame_boundary(live,d,true);
    check(live.fg && !live.fg->held_valid && live.fg->frames==0,"Re-enable must start with fresh history");
    live.running=false;
    retired=update_fg_at_frame_boundary(live,d,true);
    check(!live.fg,"Session stop must retire FG");
    retired.reset();sessions.clear();
    std::cout << "PASS FG live enable/disable/re-enable, cadence, session stop, failures and status ABI\n";
}

#include "fg_periphery_validation.hpp"
#include "fg_async_validation.hpp"
#include "fg_compact_validation.hpp"
#include "fg_prepared_validation.hpp"
#include "fg_snapshot_validation.hpp"
#include "fg_d3d11_validation.hpp"
void fg_copy_validation_test(bool include_buffered=true) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D12Debug> debug;
    const auto debug_result=D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if(FAILED(debug_result)) {
        std::cerr << "D3D12GetDebugInterface HRESULT=0x" << std::hex << unsigned(debug_result) << std::dec << '\n';
        const auto core=GetModuleHandleW(L"D3D12Core.dll");
        wchar_t path[32768]{};if(core)GetModuleFileNameW(core,path,32768);
        std::wcerr << L"Core module: " << path << L'\n';
    }
    if(debug)debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;
    check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))),"FG validation WARP adapter");
    ComPtr<ID3D12Device> device;
    check(SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"FG validation device");
    ComPtr<ID3D12InfoQueue> messages;
    device.As(&messages);
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"FG validation queue");
    auto color=[&](DXGI_FORMAT format=DXGI_FORMAT_R8G8B8A8_UNORM) {
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=64;d.Height=64;d.DepthOrArraySize=1;d.MipLevels=1;d.Format=format;
        d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> result;
        check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,
            D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&result))),"FG mock OpenXR render target");
        return result;
    };
    auto source=color(DXGI_FORMAT_B8G8R8A8_TYPELESS);
    std::array<ComPtr<ID3D12Resource>,2> targets{color(DXGI_FORMAT_B8G8R8A8_TYPELESS),color(DXGI_FORMAT_B8G8R8A8_TYPELESS)};
    // A red scene lets the test distinguish a real copy from an uninitialized
    // or black allocation, including typeless <-> sRGB typed copies.
    ComPtr<ID3D12CommandAllocator> test_allocator;ComPtr<ID3D12GraphicsCommandList> test_list;
    ComPtr<ID3D12Fence> test_fence;UINT64 test_serial{};
    check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&test_allocator))) &&
        SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,test_allocator.Get(),nullptr,IID_PPV_ARGS(&test_list))) &&
        SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&test_fence))),"Pixel test command objects");
    auto execute_test=[&] {
        check(SUCCEEDED(test_list->Close()),"Pixel test close");
        ID3D12CommandList* commands[]{test_list.Get()};queue->ExecuteCommandLists(1,commands);
        check(SUCCEEDED(queue->Signal(test_fence.Get(),++test_serial)),"Pixel test signal");
        const HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event!=nullptr,"Pixel test event");
        const bool completed=SUCCEEDED(test_fence->SetEventOnCompletion(test_serial,event))&&WaitForSingleObject(event,10000)==WAIT_OBJECT_0;
        CloseHandle(event);check(completed,"Pixel test GPU completion");
    };
    ComPtr<ID3D12DescriptorHeap> rtv_heap;D3D12_DESCRIPTOR_HEAP_DESC rh{};rh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rh.NumDescriptors=1;
    check(SUCCEEDED(device->CreateDescriptorHeap(&rh,IID_PPV_ARGS(&rtv_heap))),"Pixel test RTV heap");
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};rtv.Format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;rtv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
    const auto rtv_handle=rtv_heap->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(source.Get(),&rtv,rtv_handle);
    const float red[]{1,0,0,1};test_list->ClearRenderTargetView(rtv_handle,red,0,nullptr);execute_test();
    cheeky::fg::XrFns xr;
    xr.create=[](XrSession,const XrSwapchainCreateInfo*,XrSwapchain*) {return XR_ERROR_RUNTIME_FAILURE;};
    xr.destroy=[](XrSwapchain) {return XR_SUCCESS;};
    xr.enumerate=[](XrSwapchain,uint32_t,uint32_t*,XrSwapchainImageBaseHeader*) {return XR_ERROR_RUNTIME_FAILURE;};
    xr.acquire=[](XrSwapchain,const XrSwapchainImageAcquireInfo*,uint32_t* index) {*index=0;return XR_SUCCESS;};
    xr.wait_image=[](XrSwapchain,const XrSwapchainImageWaitInfo*) {return XR_SUCCESS;};
    xr.release=[](XrSwapchain,const XrSwapchainImageReleaseInfo*) {return XR_SUCCESS;};
    xr.wait_frame=[](XrSession,const XrFrameWaitInfo*,XrFrameState*) {return XR_SUCCESS;};
    xr.begin_frame=[](XrSession,const XrFrameBeginInfo*) {return XR_SUCCESS;};
    xr.end_frame=[](XrSession,const XrFrameEndInfo*) {return XR_SUCCESS;};
    cheeky::fg::Presenter presenter;
    const auto sc=h<XrSwapchain>(200);
    check(presenter.setup(h<XrSession>(2),xr,device.Get(),queue.Get()),"FG copy presenter setup");
    // This test exercises copies and presentation, not NVIDIA inference on WARP.
    presenter.worker.probed_device=device;presenter.worker.probe_attempted=true;
    presenter.worker.capability_ready=true;
    if(messages)messages->ClearStoredMessages();
    fg_expected_states.clear();fg_barrier_errors=0;observe_fg_barriers=true;
    for(auto* texture:{source.Get(),targets[0].Get(),targets[1].Get()})
        fg_expected_states[texture]=D3D12_RESOURCE_STATE_RENDER_TARGET;
    check(!presenter.capture(sc,0,source.Get(),64,64,1),"Typeless capture requires its declared OpenXR format");
    check(presenter.capture(sc,0,source.Get(),64,64,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB),"FG capture typeless resource with typed OpenXR format");
    XrRect2Di rect{{0,0},{64,64}};
    auto copy=presenter.subrect(sc,0,rect,0);
    check(copy!=nullptr,"FG capture crop");
    check(copy->GetDesc().Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,"Owned color must preserve the declared typed format");
    std::array<ComPtr<ID3D12Resource>,2> inputs{copy,copy};
    for(unsigned i=0;i<2;++i) {
        presenter.real_chains[i]=h<XrSwapchain>(300+i);
        XrSwapchainImageD3D12KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};image.texture=targets[i].Get();
        presenter.real_images[i].push_back(image);
    }
    std::array<unsigned,2> indices{};
    check(presenter.upload(inputs,presenter.real_chains,presenter.real_images,indices),"FG upload into acquired OpenXR render targets");
    // Feed the production EndFrame interceptor a typical UEVR layout: VD
    // helper projection, scene, game UI quad and cylindrical framework UI.
    reset();swapchains.clear();
    static unsigned composed{},synthetic_composed{};composed=synthetic_composed=0;
    presenter.xr.end_frame=[](XrSession,const XrFrameEndInfo* end) {
        if(!end->layerCount)return XR_SUCCESS;
        check(end->layerCount==4,"FG must preserve all UEVR composition layers");
        const XrStructureType expected[]{XR_TYPE_COMPOSITION_LAYER_PROJECTION,XR_TYPE_COMPOSITION_LAYER_PROJECTION,
            XR_TYPE_COMPOSITION_LAYER_QUAD,XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
        for(unsigned i=0;i<4;++i)check(end->layers[i]->type==expected[i],"FG changed UEVR composition order");
        const auto* dummy=reinterpret_cast<const XrCompositionLayerProjection*>(end->layers[0]);
        const auto* scene=reinterpret_cast<const XrCompositionLayerProjection*>(end->layers[1]);
        const auto* quad=reinterpret_cast<const XrCompositionLayerQuad*>(end->layers[2]);
        check(dummy->views[1].subImage.imageRect.offset.x==2,"VD helper views must be copied");
        check(quad->size.width==1 && quad->subImage.swapchain==h<XrSwapchain>(202),"UI quad must survive the extra slot");
        const auto scene_chain=scene->views[0].subImage.swapchain;
        check(scene_chain==h<XrSwapchain>(200)||scene_chain==h<XrSwapchain>(300)||scene_chain==h<XrSwapchain>(400),"Scene must use original or private FG swapchains");
        if(scene_chain==h<XrSwapchain>(400))++synthetic_composed;
        ++composed;return XR_SUCCESS;
    };
    presenter.xr.wait_frame=[](XrSession,const XrFrameWaitInfo*,XrFrameState* state) {
        state->shouldRender=XR_TRUE;state->predictedDisplayTime=300;state->predictedDisplayPeriod=100;return XR_SUCCESS;
    };
    auto& dispatch=instances.at(instance).dispatch;dispatch.end_frame=presenter.xr.end_frame;
    SessionState owner{};owner.instance=instance;owner.session=h<XrSession>(2);owner.running=true;
    owner.graphics_api=12;owner.view_configuration=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    owner.fg=std::shared_ptr<cheeky::fg::Presenter>(&presenter,[](auto*){});
    sessions.emplace(owner.session,std::move(owner));
    for(unsigned i=0;i<2;++i) {
        SwapchainState chain{};chain.session=h<XrSession>(2);chain.has_released=true;chain.released_index=0;
        chain.create_info={XR_TYPE_SWAPCHAIN_CREATE_INFO};chain.create_info.width=i?4:64;
        chain.create_info.height=i?4:64;chain.create_info.arraySize=1;
        swapchains.emplace(h<XrSwapchain>(200+i),std::move(chain));
    }
    std::array<XrCompositionLayerProjectionView,2> scene_views{},dummy_views{};
    for(unsigned i=0;i<2;++i) {
        scene_views[i]={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};scene_views[i].pose.orientation.w=1;
        scene_views[i].subImage={sc,{{0,0},{64,64}},0};
        dummy_views[i]={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};dummy_views[i].pose.orientation.w=1;
        dummy_views[i].subImage={h<XrSwapchain>(201),{{int(i*2),0},{2,4}},0};
    }
    XrCompositionLayerProjection scene{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    scene.space=h<XrSpace>(1);scene.viewCount=2;scene.views=scene_views.data();
    XrCompositionLayerProjection dummy{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    dummy.space=scene.space;dummy.viewCount=2;dummy.views=dummy_views.data();
    dummy.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};quad.space=scene.space;
    quad.pose.orientation.w=1;quad.size={1,2};quad.subImage={h<XrSwapchain>(202),{{0,0},{64,64}},0};
    XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};cylinder.space=scene.space;
    cylinder.pose.orientation.w=1;cylinder.radius=2;cylinder.centralAngle=1;cylinder.aspectRatio=1;
    cylinder.subImage={h<XrSwapchain>(203),{{0,0},{64,64}},0};
    const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&dummy),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&scene),reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinder)};
    XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};frame.displayTime=100;frame.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    frame.layerCount=4;frame.layers=layers;
    check(XR_SUCCEEDED(cheeky_xrEndFrame(h<XrSession>(2),&frame)) && presenter.held_valid && presenter.native_count==1,
        "UEVR's extra UI/helper layers must reach FG and establish history");
    check(presenter.capture(sc,0,source.Get(),64,64,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB),"Capture second UEVR scene frame");
    frame.displayTime=200;
    check(XR_SUCCEEDED(cheeky_xrEndFrame(h<XrSession>(2),&frame)) && presenter.native_count==2 && composed==2,
        "Multi-layer native frame must reach the presenter");
    // Simulate the application's stack structures being reused after EndFrame.
    quad.size.width=99;dummy_views[1].subImage.imageRect.offset.x=91;
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    check(XR_SUCCEEDED(presenter.wait_frame(&wait,&frame_state)) && composed==2 && presenter.fallback_count==0 &&
        frame_state.predictedDisplayPeriod==100,"Missing NGX guides must preserve native presentation and cadence");
    // Supply a synthetic scene fixture independently of NVIDIA NGX, and
    // verify its separate private chains preserve the same helper/UI layers.
    presenter.generated=presenter.held;presenter.generated_views=presenter.held_views;presenter.generated_valid=true;
    for(unsigned i=0;i<2;++i) {
        presenter.fg_chains[i]=h<XrSwapchain>(400+i);
        presenter.fg_images[i]=presenter.real_images[i];
    }
    check(XR_SUCCEEDED(presenter.wait_frame(&wait,&frame_state)) && composed==3 && synthetic_composed==1 &&
        presenter.generated_count==1 && presenter.fallback_count==0,"Generated scene slot must composite the same saved UI/helper layers");
    presenter.xr.end_frame=[](XrSession,const XrFrameEndInfo*) {return XR_SUCCESS;};
    XrBaseInStructure unknown{XR_TYPE_INSTANCE_CREATE_INFO};quad.next=&unknown;
    check(XR_SUCCEEDED(presenter.end_frame(&frame,indices,&scene)) && !presenter.held_valid && presenter.native_count==3 &&
        presenter.state==CheekyFgState::unsupported_submission,"Unsupported chains must report a reason without resetting the native counter");
    quad.next=nullptr;quad.size.width=1;dummy_views[1].subImage.imageRect.offset.x=2;
    static std::array<ID3D12Resource*,2> runtime_images{};runtime_images={targets[0].Get(),targets[1].Get()};
    static unsigned creates{};creates=0;
    auto typed_xr=xr;
    typed_xr.create=[](XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* chain) {
        if(info->format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        *chain=h<XrSwapchain>(500+creates++);return XR_SUCCESS;
    };
    typed_xr.enumerate=[](XrSwapchain chain,uint32_t capacity,uint32_t* count,XrSwapchainImageBaseHeader* images) {
        *count=1;if(capacity)reinterpret_cast<XrSwapchainImageD3D12KHR*>(images)[0].texture=runtime_images[uintptr_t(chain)-500];
        return XR_SUCCESS;
    };
    cheeky::fg::Presenter typed_presenter;
    check(typed_presenter.setup(h<XrSession>(2),typed_xr,device.Get(),queue.Get()),"Typed OpenXR presenter");
    check(typed_presenter.upload(inputs,typed_presenter.real_chains,typed_presenter.real_images,indices) && creates==2,
        "Private swapchains must request the typed format and accept typeless runtime images");
    check(typed_presenter.upload(inputs,typed_presenter.real_chains,typed_presenter.real_images,indices) && creates==2,
        "Typed swapchains must be reusable with typeless resources");
    static unsigned native_ends{},blank_ends{},wait_calls{},begin_calls{};
    native_ends=blank_ends=wait_calls=begin_calls=0;
    auto failed_xr=typed_xr;
    failed_xr.create=[](XrSession,const XrSwapchainCreateInfo*,XrSwapchain*) {return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;};
    failed_xr.end_frame=[](XrSession,const XrFrameEndInfo* end) {
        if(!end->layerCount)++blank_ends;
        else {
            const auto* scene=reinterpret_cast<const XrCompositionLayerProjection*>(end->layers[1]);
            check(end->layerCount==4 && scene->views[0].subImage.swapchain==h<XrSwapchain>(200),"Failure must submit the original scene and UI");
            ++native_ends;
        }
        return XR_SUCCESS;
    };
    failed_xr.wait_frame=[](XrSession,const XrFrameWaitInfo*,XrFrameState* state) {
        ++wait_calls;state->shouldRender=XR_TRUE;state->predictedDisplayTime=400;state->predictedDisplayPeriod=100;return XR_SUCCESS;
    };
    failed_xr.begin_frame=[](XrSession,const XrFrameBeginInfo*) {++begin_calls;return XR_SUCCESS;};
    cheeky::fg::Presenter failed_presenter;
    check(failed_presenter.setup(h<XrSession>(2),failed_xr,device.Get(),queue.Get()) &&
        failed_presenter.frame_layers.prepare(&frame,&scene),"Failure fixture setup");
    failed_presenter.blend=frame.environmentBlendMode;
    bool used_private=true;
    check(XR_SUCCEEDED(failed_presenter.submit(&frame,200,&inputs,&scene_views,false,&used_private)) && !used_private &&
        native_ends==1 && blank_ends==0,"Failed native upload must close the frame using original layers, never blank");
    failed_presenter.generated=inputs;failed_presenter.generated_views=scene_views;failed_presenter.generated_valid=true;
    check(XR_SUCCEEDED(failed_presenter.wait_frame(&wait,&frame_state)) && native_ends==2 && !blank_ends &&
        wait_calls==2 && begin_calls==1 && failed_presenter.fallback_count==1 && failed_presenter.generated_count==0,
        "Failed extra-slot upload must end that slot with the saved native scene");
    typed_presenter.frame_layers.prepare(&frame,&scene);typed_presenter.blend=frame.environmentBlendMode;
    typed_presenter.xr.end_frame=[](XrSession,const XrFrameEndInfo* end) {
        const auto* scene=reinterpret_cast<const XrCompositionLayerProjection*>(end->layers[1]);
        if(scene->views[0].subImage.swapchain!=h<XrSwapchain>(200))return XR_ERROR_LAYER_INVALID;
        ++native_ends;return XR_SUCCESS;
    };
    check(XR_SUCCEEDED(typed_presenter.submit(&frame,200,&inputs,&scene_views,false,&used_private)) && !used_private &&
        native_ends==3,"Runtime rejection of a private layer must retry the original native frame");
    typed_presenter.reset();failed_presenter.reset();
    std::cout << "PASS typed OpenXR formats over typeless resources; creation/upload/submission failures preserve native frames and balance frame calls\n";
    sessions.clear();swapchains.clear();
    std::cout << "PASS UEVR helper + scene + quad + cylinder routing, owned extra-slot layers and rejection status\n";
    presenter.invalidate_temporal();presenter.reset();
    const auto target_desc=targets[0]->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 readback_bytes{};
    device->GetCopyableFootprints(&target_desc,0,1,0,&footprint,nullptr,nullptr,&readback_bytes);
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=readback_bytes;
    buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES read_heap{};read_heap.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> readback;
    check(SUCCEEDED(device->CreateCommittedResource(&read_heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,IID_PPV_ARGS(&readback))),"Pixel readback allocation");
    check(SUCCEEDED(test_allocator->Reset())&&SUCCEEDED(test_list->Reset(test_allocator.Get(),nullptr)),"Pixel readback command reset");
    cheeky::fg::color_transition(test_list.Get(),targets[0].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=targets[0].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
    test_list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    cheeky::fg::color_transition(test_list.Get(),targets[0].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    execute_test();void* pixels{};check(SUCCEEDED(readback->Map(0,nullptr,&pixels)),"Pixel readback map");
    const auto* pixel=static_cast<const unsigned char*>(pixels)+footprint.Offset;
    const bool red_preserved=pixel[0]==0 && pixel[1]==0 && pixel[2]==255 && pixel[3]==255;readback->Unmap(0,nullptr);
    check(red_preserved,"Captured scene pixel must survive typed/typeless copies without black output or swapped channels");
    std::cout << "PASS actual BGRA scene pixels preserved through capture/crop/private OpenXR upload\n";
    unsigned errors{};
    for(UINT64 i=0;messages && i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
        SIZE_T bytes{};messages->GetMessage(i,nullptr,&bytes);std::vector<unsigned char> storage(bytes);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if(SUCCEEDED(messages->GetMessage(i,message,&bytes)) &&
            message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) {
            ++errors;std::cerr << "D3D12 ERROR: " << message->pDescription << '\n';
        }
    }
    observe_fg_barriers=false;
    fg_periphery_validation(device.Get(),queue.Get());
    // The buffered pipeline uses multiple threads; the legacy state observer is
    // deliberately single-threaded and is only used by the copy checks above.
    observe_fg_barriers=false;
    fg_snapshot_validation(device.Get());
    fg_compact_validation(device.Get());
    fg_prepared_validation(device.Get());
    if(include_buffered)fg_buffered_validation(device.Get());
    fg_d3d11_validation();
    for(auto* texture:{source.Get(),targets[0].Get(),targets[1].Get()})
        check(fg_expected_states[texture]==D3D12_RESOURCE_STATE_RENDER_TARGET,"OpenXR image must be restored to RENDER_TARGET");
    check(!fg_barrier_errors,"FG copy path violated known OpenXR resource-state contracts");
    check(!errors,"FG GPU copy path must produce zero D3D12 validation errors");
    check(SUCCEEDED(device->GetDeviceRemovedReason()),"FG GPU validation device removed");
    presenter.invalidate_temporal();
    presenter.worker.capability_ready=false;presenter.worker.failed=true;
    const auto serial_before=presenter.serial,native_before=presenter.native_count;
    check(!presenter.capture(sc,0,source.Get(),64,64,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) &&
        presenter.serial==serial_before&&presenter.copies.empty(),"Unavailable NGX must skip all capture GPU work");
    check(XR_SUCCEEDED(presenter.end_frame(&frame,indices,&scene)) &&
        presenter.native_count==native_before+1&&presenter.serial==serial_before,
        "Unavailable NGX must forward the original native frame without GPU work");
    std::cout << "PASS FG GPU capture/crop/upload and OpenXR RENDER_TARGET state contracts (WARP); debug layer="
        << (debug?"enabled":"unavailable") << '\n';
}
}
#include "fg_hardware_probe.hpp"
#include "fg_hardening_validation.hpp"
#ifdef CHEEKY_FG_LIFETIME_DIAGNOSTICS
void diagnostic_lifetime_test() {
    reset();traced_spaces.clear();
    const auto session=h<XrSession>(9001);SessionState owner{};owner.instance=instance;owner.session=session;sessions.emplace(session,std::move(owner));
    auto& d=instances.at(instance).dispatch;
    d.create_reference_space=[](XrSession,const XrReferenceSpaceCreateInfo*,XrSpace* out)->XrResult{*out=h<XrSpace>(9002);return XR_SUCCESS;};
    d.create_action_space=[](XrSession,const XrActionSpaceCreateInfo*,XrSpace* out)->XrResult{*out=h<XrSpace>(9003);return XR_SUCCESS;};
    d.destroy_space=[](XrSpace space)->XrResult{return space==h<XrSpace>(9002)?XR_ERROR_RUNTIME_FAILURE:XR_SUCCESS;};
    XrSpace space{};XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    check(cheeky_xrCreateReferenceSpace(session,&reference,&space)==XR_SUCCESS&&traced_spaces.contains(space),"Diagnostic reference create forwards and tracks");
    check(cheeky_xrDestroySpace(space)==XR_ERROR_RUNTIME_FAILURE&&traced_spaces.contains(space),"Failed destroy preserves tracking and result");
    XrActionSpaceCreateInfo action{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    check(cheeky_xrCreateActionSpace(session,&action,&space)==XR_SUCCESS&&traced_spaces.contains(space),"Diagnostic action create forwards and tracks");
    check(cheeky_xrDestroySpace(space)==XR_SUCCESS&&!traced_spaces.contains(space),"Successful destroy removes tracking");
    check(cheeky_xrDestroySpace(h<XrSpace>(9004))==XR_SUCCESS,"Untracked extension space still forwards");
    cheeky::fg::BufferedPresenter p;static std::string log;log.clear();
    p.trace=[](const char* text) noexcept {log+=text;log+='\n';};
    p.xr.end_frame=[](XrSession,const XrFrameEndInfo*)->XrResult{return XR_ERROR_HANDLE_INVALID;};
    p.xr.locate_space=[](XrSpace,XrSpace,XrTime,XrSpaceLocation*)->XrResult{return XR_ERROR_HANDLE_INVALID;};
    p.xr.enumerate=[](XrSwapchain,unsigned,unsigned* count,XrSwapchainImageBaseHeader*)->XrResult{*count=3;return XR_SUCCESS;};
    XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};view.subImage.swapchain=h<XrSwapchain>(9005);
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=space;projection.views=&view;projection.viewCount=1;
    const auto* header=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.layers=&header;end.layerCount=1;
    check(p.runtime_end_frame(&end)==XR_ERROR_HANDLE_INVALID,"Diagnostic probes preserve original EndFrame result");
    check(log.find("self_locate_result=-12")!=log.npos&&log.find("enumerate_result=0")!=log.npos,"Failure log distinguishes invalid space from live chain");
    sessions.clear();traced_spaces.clear();
    std::cout<<"PASS diagnostic space lifecycle forwarding and EndFrame handle probes\n";
}
#endif
int main(int argc,char** argv) {
    std::cout.setf(std::ios::unitbuf);
    try {
#ifdef CHEEKY_FG_LIFETIME_DIAGNOSTICS
        diagnostic_lifetime_test();
        if(argc>1&&std::string(argv[1])=="--fg-lifetime")return 0;
#endif
        if(argc>1&&std::string(argv[1])=="--fg-native-handoff") {fg_native_handoff_test();return 0;}
        if(argc>1&&std::string(argv[1])=="--fg-lifecycle") {fg_lifecycle_retirement_test();fg_borrowed_frame_shutdown_test();return 0;}
        if(argc>1&&std::string(argv[1])=="--fg-borrowed-shutdown") {fg_borrowed_frame_shutdown_test();return 0;}
        if(argc>1&&std::string(argv[1])=="--fg-hardening") {fg_hardening_validation();return 0;}
        if(argc>2 && (std::string(argv[1])=="--fg-capability" || std::string(argv[1])=="--fg-generate" || std::string(argv[1])=="--fg-buffered"||std::string(argv[1])=="--fg-motion"||std::string(argv[1])=="--fg-crop-motion")) {
            Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
            check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))),"DXGI factory");
            for(UINT i=0;;++i) {
                Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
                if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)break;
                DXGI_ADAPTER_DESC1 description{};adapter->GetDesc1(&description);
                if(description.VendorId!=0x10de || (description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))continue;
                std::wcout << L"GPU: " << description.Description << L'\n';
                Microsoft::WRL::ComPtr<ID3D12Device> device;
                check(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))),"NVIDIA D3D12 device");
                const int count=MultiByteToWideChar(CP_UTF8,0,argv[2],-1,nullptr,0);
                std::wstring directory(count,L'\0');MultiByteToWideChar(CP_UTF8,0,argv[2],-1,directory.data(),count);
                cheeky::fg::Worker worker;worker.trace=[](const char* line) noexcept {std::cout << line << '\n';};
                worker.force_fp16=argc>9&&std::string(argv[9])=="fp16";
                fg_probe_legacy_mvec=argc>3&&std::string(argv[3])=="identity-mv";
                const bool ready=worker.probe(device.Get(),directory.c_str());
                std::cout << "Frame generation capability: " << (ready?"AVAILABLE":"UNAVAILABLE") << '\n';
                if(ready&&std::string(argv[1])=="--fg-buffered")fg_buffered_validation(device.Get(),directory.c_str(),argc>3?unsigned(std::stoul(argv[3])):2u,argc>4&&std::string(argv[4])=="prepared");
                if(ready&&std::string(argv[1])=="--fg-motion")fg_hardware_probe(worker,device.Get(),true,argc>3&&std::string(argv[3])=="foveated",1,100,50,false,512,512,true);
                if(ready&&std::string(argv[1])=="--fg-crop-motion")fg_hardware_probe(worker,device.Get(),false,true,1,
                    argc>3?unsigned(std::stoul(argv[3])):100u,50,false,512,512,true,false,
                    argc>5&&std::string(argv[5])=="atlas",false,false,argc>5&&std::string(argv[5])=="xr",false,1,
                    true,argc>4?unsigned(std::stoul(argv[4])):0u,argc>6&&std::string(argv[6])=="scene");
                if(ready&&std::string(argv[1])=="--fg-generate")fg_hardware_probe(worker,device.Get(),argc>3&&std::string(argv[3])=="bgra",argc>4&&std::string(argv[4])=="foveated",
                    argc>5?unsigned(std::stoul(argv[5])):2u,argc>6?unsigned(std::stoul(argv[6])):100u,
                    argc>7?unsigned(std::stoul(argv[7])):50u,argc>8&&(std::string(argv[8])=="game-guides"||std::string(argv[8])=="full-guides"),
                    argc>10?unsigned(std::stoul(argv[10])):512u,argc>11?unsigned(std::stoul(argv[11])):512u,
                    false,argc>8&&std::string(argv[8])=="full-guides",argc>12&&std::string(argv[12])=="compact",argc>13&&std::string(argv[13])=="prepared",
                    argc>14&&std::string(argv[14])=="periphery-baseline",argc>14&&(std::string(argv[14])=="final-xr"||std::string(argv[14])=="final-xr-typeless"),
                    argc>14&&std::string(argv[14])=="final-xr-typeless",argc>15&&std::string(argv[15])=="full-rate"?0u:argc>15&&std::string(argv[15])=="filtered"?2u:1u);
                return ready?0:2;
            }
            throw std::runtime_error("No NVIDIA adapter found");
        }
        if(argc>1 && std::string(argv[1])=="--fg-d3d11") {fg_d3d11_validation();return 0;}
        if(argc>1 && std::string(argv[1])=="--fg-gpu-validation") {fg_copy_validation_test();return 0;}
        if(argc>1 && std::string(argv[1])=="--fg-crop-gpu-validation") {fg_copy_validation_test(false);return 0;}
        run(); startup_test(); clock_extension_test(); menu_policy_test(); custom_binding_menu_test(); fg_native_handoff_test(); fg_lifecycle_retirement_test(); fg_borrowed_frame_shutdown_test(); fg_toggle_test(); std::cout << "PASS bindings, input, startup and flat sampled-menu policy\n"; return 0;
    }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
