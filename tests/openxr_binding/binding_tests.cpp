// Exercise production interceptors with a downstream runtime; no headset needed.
#include "../../openxr_layer/openxr_layer.cpp"
#include <iostream>
#include <stdexcept>
#include "../../standalone/menu_backend.hpp"
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
void menu_policy_test() {
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
}
int main() {
    try { run(); startup_test(); menu_policy_test(); custom_binding_menu_test(); std::cout << "PASS bindings, input, startup and flat sampled-menu policy\n"; return 0; }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
