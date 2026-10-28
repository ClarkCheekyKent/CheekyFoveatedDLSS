#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_VULKAN

#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <filesystem>
#include <d3dcompiler.h>
#define VK_NO_PROTOTYPES
#include "../third_party/vulkan/include/vulkan/vulkan_core.h"
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include "cheeky_gaze_abi.h"
#include "openxr_startup_log.hpp"
#include "gaze_math.hpp"
#include "eye_calibration.hpp"
#include "projection_selection.hpp"
#include "fg_async.hpp"
#include "fg_restart_policy.hpp"
#include "quad_capture.hpp"
#include "../shared/openxr_menu_bridge.hpp"
#include "../standalone/menu_backend.hpp"
#include "menu_geometry.hpp"
#include "../src/realvr_runtime.hpp"
#include "../src/d3d12_native.hpp"
#include <wrl/client.h>
#include <deque>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <mutex>
#include <shared_mutex>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <type_traits>

namespace {

constexpr char layer_name[] = "XR_APILAYER_CHEEKY_foveated_dlss";
// Latch the first configuration supplied by the consumer for this DLL lifetime.
// Attachment cannot be undone, so changing this option requires an app restart.
std::atomic<int> independent_gaze_option{-1};

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
    PFN_xrConvertTimeToWin32PerformanceCounterKHR time_to_qpc{};
    PFN_xrGetDisplayRefreshRateFB get_refresh_rate{};
    PFN_xrGetInstanceProcAddr get_instance_proc_addr{};
    PFN_xrDestroyInstance destroy_instance{};
    PFN_xrGetInstanceProperties get_instance_properties{};
    PFN_xrGetSystem get_system{};
    PFN_xrGetSystemProperties get_system_properties{};
    PFN_xrEnumerateViewConfigurations enumerate_view_configurations{};
    PFN_xrEnumerateViewConfigurationViews enumerate_view_configuration_views{};
    PFN_xrGetViewConfigurationProperties get_view_configuration_properties{};
    PFN_xrEnumerateEnvironmentBlendModes enumerate_environment_blend_modes{};
    PFN_xrGetVisibilityMaskKHR get_visibility_mask{};
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
    PFN_xrGetCurrentInteractionProfile get_current_profile{};
    PFN_xrPathToString path_to_string{};
    PFN_xrGetActionStatePose get_action_state_pose{};
    PFN_xrGetActionStateBoolean get_action_state_boolean{};
    PFN_xrGetActionStateFloat get_action_state_float{};
    PFN_xrGetActionStateVector2f get_action_state_vector2f{};
    PFN_xrCreateActionSpace create_action_space{};
    PFN_xrCreateReferenceSpace create_reference_space{};
    PFN_xrDestroySpace destroy_space{};
    PFN_xrLocateSpace locate_space{};
    PFN_xrLocateSpaces locate_spaces{};
    PFN_xrLocateViews locate_views{};
    PFN_xrCreateSwapchain create_swapchain{};
    PFN_xrEnumerateSwapchainFormats enumerate_swapchain_formats{};
    PFN_xrDestroySwapchain destroy_swapchain{};
    PFN_xrEnumerateSwapchainImages enumerate_swapchain_images{};
    PFN_xrAcquireSwapchainImage acquire_swapchain_image{};
    PFN_xrWaitSwapchainImage wait_swapchain_image{};
    PFN_xrReleaseSwapchainImage release_swapchain_image{};
    PFN_xrWaitFrame wait_frame{};
    PFN_xrBeginFrame begin_frame{};
    PFN_xrEndFrame end_frame{};
};

void populate_dispatch(
    Dispatch& dispatch,
    const XrInstance instance,
    const PFN_xrGetInstanceProcAddr gipa
) noexcept {
    dispatch.get_instance_proc_addr = gipa;
    dispatch.time_to_qpc=load_function<PFN_xrConvertTimeToWin32PerformanceCounterKHR>(gipa,instance,"xrConvertTimeToWin32PerformanceCounterKHR");
    dispatch.get_refresh_rate=load_function<PFN_xrGetDisplayRefreshRateFB>(gipa,instance,"xrGetDisplayRefreshRateFB");
#define CHEEKY_LOAD(field, name) \
    dispatch.field = load_function<PFN_xr##name>(gipa, instance, "xr" #name)
    CHEEKY_LOAD(destroy_instance, DestroyInstance);
    CHEEKY_LOAD(get_instance_properties, GetInstanceProperties);
    CHEEKY_LOAD(get_system, GetSystem);
    CHEEKY_LOAD(get_system_properties, GetSystemProperties);
    CHEEKY_LOAD(locate_spaces, LocateSpaces);
    CHEEKY_LOAD(enumerate_view_configurations, EnumerateViewConfigurations);
    CHEEKY_LOAD(enumerate_view_configuration_views, EnumerateViewConfigurationViews);
    CHEEKY_LOAD(get_view_configuration_properties, GetViewConfigurationProperties);
    CHEEKY_LOAD(enumerate_environment_blend_modes, EnumerateEnvironmentBlendModes);
    CHEEKY_LOAD(get_visibility_mask, GetVisibilityMaskKHR);
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
    CHEEKY_LOAD(get_current_profile, GetCurrentInteractionProfile);
    CHEEKY_LOAD(path_to_string, PathToString);
    CHEEKY_LOAD(get_action_state_pose, GetActionStatePose);
    CHEEKY_LOAD(get_action_state_boolean, GetActionStateBoolean);
    CHEEKY_LOAD(get_action_state_float, GetActionStateFloat);
    CHEEKY_LOAD(get_action_state_vector2f, GetActionStateVector2f);
    CHEEKY_LOAD(create_action_space, CreateActionSpace);
    CHEEKY_LOAD(create_reference_space, CreateReferenceSpace);
    CHEEKY_LOAD(destroy_space, DestroySpace);
    CHEEKY_LOAD(locate_space, LocateSpace);
    CHEEKY_LOAD(locate_views, LocateViews);
    CHEEKY_LOAD(create_swapchain, CreateSwapchain);
    CHEEKY_LOAD(enumerate_swapchain_formats, EnumerateSwapchainFormats);
    CHEEKY_LOAD(destroy_swapchain, DestroySwapchain);
    CHEEKY_LOAD(enumerate_swapchain_images, EnumerateSwapchainImages);
    CHEEKY_LOAD(acquire_swapchain_image, AcquireSwapchainImage);
    CHEEKY_LOAD(wait_swapchain_image, WaitSwapchainImage);
    CHEEKY_LOAD(release_swapchain_image, ReleaseSwapchainImage);
    CHEEKY_LOAD(wait_frame, WaitFrame);
    CHEEKY_LOAD(begin_frame, BeginFrame);
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
    ULONGLONG input_log_tick{};
    std::array<ULONGLONG, 3> pose_log_ticks{};
    struct InputSample { XrAction action; XrPath path; XrStructureType type; ULONGLONG tick; };
    std::vector<InputSample> input_samples; // bounded, session-owned diagnostic throttle
    bool diagnostic_menu_visible{};
    struct Menu {
        XrSwapchain swapchain{XR_NULL_HANDLE};
        std::uint32_t acquired_index{};
        bool acquired{},waited{};
        std::vector<XrSwapchainImageD3D11KHR> images11;
        std::vector<XrSwapchainImageD3D12KHR> images12;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> convert_context11;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> convert_vertex11;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> convert_pixel11;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> convert_sampler11;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> convert_raster11;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> convert_depth11;
        Microsoft::WRL::ComPtr<ID3D11BlendState> convert_blend11;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> convert_source11;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> convert_source_view11;
        std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> convert_targets11;
        std::vector<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>> allocators;
        std::vector<std::uint64_t> fence_values;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> convert_root;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> convert_pipeline;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> convert_rtv_heap;
        std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> convert_srv_heaps;
        std::uint32_t convert_rtv_stride{};
        std::uint64_t fence_value{};
        std::uint32_t width{}, height{}, format{}, target_format{};
        float display_width{}, display_height{};
        bool anchored{};
        bool cylinder{};
        unsigned strips{1};
        XrPosef pose{};
        XrExtent2Df size{};
    } menu, cached_menu; // Updates never acquire the currently published chain.
    Menu cursor_menu; // Static pixels; only the quad pose changes per headset tick.
    Microsoft::WRL::ComPtr<ID3D12Resource> cursor_upload;
    bool cursor_ready{}, cursor_failed{}, cursor_controller_drag{};
    struct LocatedProjection {
        XrTime time{};
        XrSpace space{XR_NULL_HANDLE};
        std::array<XrPosef, 2> poses{};
        bool valid{};
    };
    std::array<LocatedProjection, 8> located_history{};
    unsigned located_cursor{};
    std::array<XrFovf, 2> submitted_fov{};
    std::array<XrQuaternionf, 2> submitted_rotation_delta{};
    XrTime submitted_projection_time{};
    bool submitted_projection_valid{}, using_submitted_projection{};
    cheeky::openxr_calibration::Frame calibration;
    unsigned graphics_api{};
    void* graphics_device{};
    void* graphics_queue{};
    XrSession session{XR_NULL_HANDLE};
    XrInstance instance{XR_NULL_HANDLE};
    XrSystemId system_id{XR_NULL_SYSTEM_ID};
    XrSpace gaze_space{XR_NULL_HANDLE};
    std::array<XrSpace, 2> menu_aim_spaces{};
    XrSpace calibration_local_space{XR_NULL_HANDLE};
    XrViewConfigurationType view_configuration{};
    XrSessionState state{XR_SESSION_STATE_UNKNOWN};
    std::uint64_t generation{};
    bool system_supported{};
    bool action_attached{};
    bool running{};
    bool menu_submission_disabled{};
    bool menu_cylinder_enabled{};
    int menu_pointer_hand{-1};
    ULONGLONG menu_pointer_log_tick{};
    struct HostMenuHand {
        XrPath profile{};
        XrSpace space{};
        XrAction trigger{};
        XrPath trigger_subaction{};
        XrActionType trigger_type{XR_ACTION_TYPE_MAX_ENUM};
        bool initialized{};
    };
    std::array<HostMenuHand, 2> host_menu_hands{};
    std::uint32_t max_layers{16};
    XrTime menu_recenter_time{};
    bool fallback_setup_attempted{};
    XrTime last_fallback_sync_time{};
    // Successful EndFrame count. A host that renders stereo frames without ever
    // touching the action system may receive our standalone gaze attachment.
    unsigned rendered_frames{};
    CheekyGazeInputDiagnosticsV1 input{
        CHEEKY_GAZE_INPUT_DIAGNOSTICS_VERSION, sizeof(CheekyGazeInputDiagnosticsV1), 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        CHEEKY_GAZE_RESULT_NOT_CALLED, CHEEKY_GAZE_RESULT_NOT_CALLED,
        CHEEKY_GAZE_RESULT_NOT_CALLED, CHEEKY_GAZE_RESULT_NOT_CALLED,
        CHEEKY_GAZE_RESULT_NOT_CALLED, CHEEKY_GAZE_RESULT_NOT_CALLED};
    bool action_active{};
    bool gaze_valid{};
    bool simulated{};
    bool next_jump_valid{};
    std::array<float, 2> next_jump_u{}, next_jump_v{};
    XrTime simulation_start{};
    unsigned simulation_pattern{};
    bool unsupported_view_configuration{};
    std::shared_ptr<cheeky::quad_capture::Compositor> quad_capture;
    std::uint64_t quad_capture_locates{}, quad_capture_frames{};
    bool quad_capture_correct_focus_fov{};
    std::map<XrTime,std::array<XrFovf,4>> quad_capture_fovs;
    bool ambiguous_resource{};
    XrTime predicted_display_time{};
    XrTime sample_time{};
    std::shared_ptr<cheeky::fg::Presenter> fg;
    CheekyFgState fg_state{CheekyFgState::off};
    bool fg_requested{};
    // An application swapchain was destroyed. Retire the presenter at the next
    // xrWaitFrame boundary, never from the destroying thread: another thread
    // may be blocked inside the presenter's xrWaitFrame at that moment.
    bool fg_retire_pending{};
    // A WaitFrame issued before activation still belongs to the runtime.
    // Preserve that ownership through its matching BeginFrame and EndFrame.
    std::uint64_t native_waits_pending{};
    bool native_frame_begun{};
    std::uint64_t fg_uevr_mode_epoch{}; // Last observed UEVR mode-transition epoch.
    std::uint32_t gaze_location_flags{};
    std::array<float, CHEEKY_GAZE_MAX_VIEWS> center_u{};
    std::array<float, CHEEKY_GAZE_MAX_VIEWS> center_v{};
    std::array<XrFovf, CHEEKY_GAZE_MAX_VIEWS> eye_fov{};
    bool eye_fov_valid{};
    bool forward_valid{};
    std::array<float, 2> forward_u{}, forward_v{};
    std::array<SubmittedView, CHEEKY_GAZE_MAX_VIEWS> submitted_views{};
};

struct SwapchainState {
    XrSwapchain swapchain{XR_NULL_HANDLE};
    XrSession session{XR_NULL_HANDLE};
    XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    std::vector<std::uint64_t> resource_identities;
    std::vector<void*> calibration_images;
    std::deque<std::pair<std::uint32_t, bool>> acquired_images;
    std::uint32_t acquired_index{};
    std::uint32_t released_index{};
    bool has_acquired{};
    bool has_released{};
};

struct InstanceState {
    bool quad_capture_enabled{};
    bool quad_capture_prefer_quad{};
    bool msfs_version_bridge{};
    XrInstance instance{XR_NULL_HANDLE};
    Dispatch dispatch{};
    std::string runtime_name;
    bool extension_enabled{};
    bool cylinder_enabled{};
    XrActionSet action_set{XR_NULL_HANDLE};
    XrActionSet menu_action_set{XR_NULL_HANDLE};
    XrAction menu_aim_action{XR_NULL_HANDLE};
    XrAction menu_click_action{XR_NULL_HANDLE};
    std::array<XrPath, 2> menu_hand_paths{};
    std::array<XrPath, 5> menu_profiles{};
    std::array<bool, 5> menu_bindings_submitted{};
    // Applications may suggest bindings before their graphics session exists.
    // Later menu setup must augment that successful suggestion, not replace it.
    std::array<std::vector<XrActionSuggestedBinding>, 5> host_menu_profile_bindings;
    std::array<bool, 5> host_menu_profile_has_next{};
    bool menu_graphics_enabled{};
    XrAction gaze_action{XR_NULL_HANDLE};
    XrPath gaze_path{XR_NULL_PATH};
    XrPath gaze_profile{XR_NULL_PATH};
    bool gaze_binding_submitted{};
    bool host_action_sets_created{};
    bool il2_input_compatibility{};
    XrResult binding_result{static_cast<XrResult>(CHEEKY_GAZE_RESULT_NOT_CALLED)};
};

std::atomic<bool> simulated_gaze_enabled{};
std::atomic<unsigned> simulation_pattern{};
std::mutex state_mutex;

// Private swapchains of a retired native D3D11 presenter. Virtual Desktop
// stopped answering xrWaitFrame (DCS froze) when they were destroyed right
// after the presenter's final frame; the D3D11->D3D12 bridge only destroys
// after a GPU drain. Destroy them from the application's own xrEndFrame
// once the runtime has displayed about a second of later frames.
namespace {
// Logs application frame calls right after FG is disabled (transition trace).
std::atomic<int> fg_transition_trace{};
bool trace_transition() noexcept {return fg_transition_trace.load(std::memory_order_relaxed)>0&&fg_transition_trace.fetch_sub(1)>0;}
struct RetiredChain {XrSession session;XrSwapchain chain;PFN_xrDestroySwapchain destroy;unsigned frames;};
std::mutex retired_chain_mutex;
std::vector<RetiredChain> retired_chains;
void retire_private_chain(XrSession session,XrSwapchain chain,PFN_xrDestroySwapchain destroy) noexcept {
    if(!chain||!destroy)return;
    try{std::lock_guard lock(retired_chain_mutex);retired_chains.push_back({session,chain,destroy,90});}
    catch(...){destroy(chain);}
}
void age_retired_chains(XrSession session,bool all) noexcept {
    std::vector<RetiredChain> due;
    {std::lock_guard lock(retired_chain_mutex);
        std::erase_if(retired_chains,[&](RetiredChain& r){
            if(r.session!=session||(!all&&--r.frames))return false;
            due.push_back(r);return true;});}
    for(const auto& r:due)r.destroy(r.chain);
}
}

std::mutex menu_mutex;
std::unordered_map<XrInstance, InstanceState> instances;
std::unordered_map<XrSession, SessionState> sessions;
std::unordered_map<XrSwapchain, SwapchainState> swapchains;
// Frame submissions can outlive the application's reference space after recenter.
// Keep independent runtime spaces for queued frames; application destruction
// still forwards immediately and preserves the runtime's return value.
struct FrameSpace {
    XrSpace handle{}; PFN_xrDestroySpace destroy{};
    ~FrameSpace(){if(handle&&destroy)destroy(handle);}
};
struct FrameSpaceRecord {
    XrSession session{}; XrInstance instance{}; PFN_xrDestroySpace destroy{};
    PFN_xrCreateReferenceSpace reference_create{}; PFN_xrCreateActionSpace action_create{};
    XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    XrActionSpaceCreateInfo action{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    bool cloneable{};
    std::weak_ptr<FrameSpace> retained;
};
std::unordered_map<XrSpace, std::shared_ptr<FrameSpaceRecord>> frame_spaces;
std::atomic<std::uint64_t> next_session_generation{1U};
std::atomic<std::uint64_t> swapchain_generation{1U};

void report_menu_status(const CheekyOpenXRMenuStatus status) noexcept {
    const auto host = GetModuleHandleW(L"CheekyFoveatedDLSSHost.dll");
    const auto report = host ? reinterpret_cast<CheekyOpenXRMenuReport>(
        GetProcAddress(host, "CheekyOpenXRMenuReportStatus")) : nullptr;
    if (report) report(status);
}

void report_menu_diagnostic(const char* const message) noexcept {
    const auto host = GetModuleHandleW(L"CheekyFoveatedDLSSHost.dll");
    const auto report = host ? reinterpret_cast<CheekyOpenXRMenuDiagnosticFn>(
        GetProcAddress(host, "CheekyOpenXRMenuLogDiagnostic")) : nullptr;
    if (report) report(message);
}

bool menu_copy_compatible(const DXGI_FORMAT source, const DXGI_FORMAT target) noexcept {
    if (source == target) return true;
    const auto in_group = [](const DXGI_FORMAT value, const DXGI_FORMAT a,
        const DXGI_FORMAT b) { return value == a || value == b; };
    return (in_group(source, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) &&
        in_group(target, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) ||
        (in_group(source, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) &&
        in_group(target, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB));
}

bool initialize_menu_converter11(SessionState::Menu& menu, ID3D11Device* device) {
    static constexpr char vertex_shader[] = R"(
        struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
        Output main(uint id : SV_VertexID) {
            Output result;
            result.uv = float2((id << 1) & 2, id & 2);
            result.position = float4(result.uv * float2(2, -2) + float2(-1, 1), 0, 1);
            return result;
        })";
    static constexpr char pixel_shader[] = R"(
        Texture2D source_texture : register(t0);
        SamplerState source_sampler : register(s0);
        float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
            return source_texture.Sample(source_sampler, uv);
        })";
    Microsoft::WRL::ComPtr<ID3DBlob> vertex, pixel, errors;
    if (FAILED(D3DCompile(vertex_shader, sizeof(vertex_shader) - 1, nullptr, nullptr, nullptr,
            "main", "vs_5_0", 0, 0, &vertex, &errors)) ||
        FAILED(D3DCompile(pixel_shader, sizeof(pixel_shader) - 1, nullptr, nullptr, nullptr,
            "main", "ps_5_0", 0, 0, &pixel, &errors)) ||
        FAILED(device->CreateDeferredContext(0, &menu.convert_context11)) ||
        FAILED(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(),
            nullptr, &menu.convert_vertex11)) ||
        FAILED(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(),
            nullptr, &menu.convert_pixel11))) return false;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    D3D11_BLEND_DESC blend{};
    auto& blend_target = blend.RenderTarget[0];
    blend_target.SrcBlend = D3D11_BLEND_ONE;
    blend_target.DestBlend = D3D11_BLEND_ZERO;
    blend_target.BlendOp = D3D11_BLEND_OP_ADD;
    blend_target.SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_target.DestBlendAlpha = D3D11_BLEND_ZERO;
    blend_target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateSamplerState(&sampler, &menu.convert_sampler11)) ||
        FAILED(device->CreateRasterizerState(&raster, &menu.convert_raster11)) ||
        FAILED(device->CreateDepthStencilState(&depth, &menu.convert_depth11)) ||
        FAILED(device->CreateBlendState(&blend, &menu.convert_blend11))) return false;
    menu.convert_targets11.resize(menu.images11.size());
    D3D11_RENDER_TARGET_VIEW_DESC target_view{};
    target_view.Format = static_cast<DXGI_FORMAT>(menu.target_format);
    target_view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    for (std::size_t index{}; index < menu.images11.size(); ++index)
        if (FAILED(device->CreateRenderTargetView(menu.images11[index].texture,
            &target_view, &menu.convert_targets11[index]))) return false;
    return true;
}

bool render_menu_converter11(SessionState::Menu& menu, const CheekyOpenXRMenuFrame& frame,
    const std::uint32_t index, const HMODULE host, const bool shared_device) {
    if (index >= menu.convert_targets11.size() || !menu.convert_context11) return false;
    auto* device = static_cast<ID3D11Device*>(frame.device);
    auto* source = static_cast<ID3D11Texture2D*>(frame.texture);
    if (menu.convert_source11.Get() != source) {
        menu.convert_source_view11.Reset();
        menu.convert_source11 = source;
        if (FAILED(device->CreateShaderResourceView(source, nullptr,
            &menu.convert_source_view11))) return false;
    }
    auto* context = menu.convert_context11.Get();
    context->ClearState();
    auto* target = menu.convert_targets11[index].Get();
    auto* view = menu.convert_source_view11.Get();
    auto* sampler = menu.convert_sampler11.Get();
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(menu.width),
        static_cast<float>(menu.height), 0, 1};
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(menu.convert_blend11.Get(), nullptr, 0xFFFFFFFFU);
    context->OMSetDepthStencilState(menu.convert_depth11.Get(), 0);
    context->RSSetState(menu.convert_raster11.Get());
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(menu.convert_vertex11.Get(), nullptr, 0);
    context->PSSetShader(menu.convert_pixel11.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &view);
    context->PSSetSamplers(0, 1, &sampler);
    context->Draw(3, 0);
    Microsoft::WRL::ComPtr<ID3D11CommandList> list;
    if (FAILED(context->FinishCommandList(FALSE, &list))) return false;
    if (shared_device) {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate;
        device->GetImmediateContext(&immediate);
        immediate->ExecuteCommandList(list.Get(), TRUE);
        immediate->Flush();
        return SUCCEEDED(device->GetDeviceRemovedReason());
    }
    const auto execute = reinterpret_cast<CheekyOpenXRMenuExecute11>(
        GetProcAddress(host, "CheekyOpenXRMenuExecute11"));
    return execute && execute(list.Get(), source);
}

bool initialize_menu_converter12(SessionState::Menu& menu, ID3D12Device* device) {
    const auto fail = [](const char* stage, HRESULT result) {
        char message[160]{};
        std::snprintf(message, sizeof(message), "OpenXR menu: D3D12 converter %s failed (0x%08lX)",
            stage, static_cast<unsigned long>(result));
        report_menu_diagnostic(message);
        return false;
    };
    static constexpr char vertex_shader[] = R"(
        struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
        Output main(uint id : SV_VertexID) {
            Output result;
            result.uv = float2((id << 1) & 2, id & 2);
            result.position = float4(result.uv * float2(2, -2) + float2(-1, 1), 0, 1);
            return result;
        })";
    static constexpr char pixel_shader[] = R"(
        Texture2D source_texture : register(t0);
        SamplerState source_sampler : register(s0);
        float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
            return source_texture.Sample(source_sampler, uv);
        })";
    Microsoft::WRL::ComPtr<ID3DBlob> vertex, pixel, errors, root_blob;
    auto result = D3DCompile(vertex_shader, sizeof(vertex_shader) - 1, nullptr, nullptr, nullptr,
        "main", "vs_5_1", 0, 0, &vertex, &errors);
    if (FAILED(result)) return fail("vertex shader", result);
    result = D3DCompile(pixel_shader, sizeof(pixel_shader) - 1, nullptr, nullptr, nullptr,
        "main", "ps_5_1", 0, 0, &pixel, &errors);
    if (FAILED(result)) return fail("pixel shader", result);
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxAnisotropy = 1;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC root{};
    root.NumParameters = 1;
    root.pParameters = &parameter;
    root.NumStaticSamplers = 1;
    root.pStaticSamplers = &sampler;
    root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    result = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, &errors);
    if (FAILED(result)) return fail("root serialization", result);
    result = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
        IID_PPV_ARGS(&menu.convert_root));
    if (FAILED(result)) return fail("root signature", result);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = menu.convert_root.Get();
    pipeline.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    pipeline.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    auto& blend_target = pipeline.BlendState.RenderTarget[0];
    blend_target.SrcBlend = D3D12_BLEND_ONE;
    blend_target.DestBlend = D3D12_BLEND_ZERO;
    blend_target.BlendOp = D3D12_BLEND_OP_ADD;
    blend_target.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend_target.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend_target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend_target.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend_target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable = TRUE;
    pipeline.DepthStencilState.DepthEnable = FALSE;
    pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    pipeline.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    pipeline.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    pipeline.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline.DepthStencilState.BackFace = pipeline.DepthStencilState.FrontFace;
    pipeline.SampleMask = UINT_MAX;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = static_cast<DXGI_FORMAT>(menu.target_format);
    pipeline.SampleDesc.Count = 1;
    result = device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&menu.convert_pipeline));
    if (FAILED(result)) return fail("pipeline", result);
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = static_cast<UINT>(menu.images12.size());
    result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&menu.convert_rtv_heap));
    if (FAILED(result)) return fail("RTV heap", result);
    menu.convert_rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto rtv = menu.convert_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    menu.convert_srv_heaps.resize(menu.images12.size());
    // OpenXR images can use typeless backing resources. A default RTV would
    // inherit that typeless format and can remove the D3D12 device.
    D3D12_RENDER_TARGET_VIEW_DESC target_view{};
    target_view.Format = static_cast<DXGI_FORMAT>(menu.target_format);
    target_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    for (std::size_t index{}; index < menu.images12.size(); ++index) {
        if (!menu.images12[index].texture) return fail("missing runtime texture", E_POINTER);
        if (index == 0) {
            char message[160]{};
            std::snprintf(message, sizeof(message), "OpenXR menu: D3D12 backing format %u, explicit RTV format %u",
                static_cast<unsigned>(menu.images12[index].texture->GetDesc().Format), menu.target_format);
            report_menu_diagnostic(message);
        }
        device->CreateRenderTargetView(menu.images12[index].texture, &target_view, rtv);
        result = device->GetDeviceRemovedReason();
        if (FAILED(result)) return fail("render-target view", result);
        rtv.ptr += menu.convert_rtv_stride;
        heap = {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 1;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&menu.convert_srv_heaps[index]));
        if (FAILED(result)) return fail("SRV heap", result);
    }
    return true;
}

// FG presenters submit runtime frames from their own thread, more than one per
// app frame. The app thread copies the menu and publishes its ready layer here;
// the presenter's runtime xrEndFrame appends it to every frame it submits.
struct PublishedMenu {
    PFN_xrEndFrame next{};
    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::uint32_t max_layers{};
    bool cylinder{};
    XrCompositionLayerCylinderKHR cylinder_layer{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
    std::vector<XrCompositionLayerQuad> strips;
    XrPosef panel_pose{};
    XrExtent2Df panel_size{};
    std::uint32_t panel_width{}, panel_strips{1};
    float display_width{}, display_height{};
    XrSwapchain cursor_swapchain{XR_NULL_HANDLE};
    XrCompositionLayerQuad cursor_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    bool cursor_visible{}, cursor_controller_drag{};
};
void update_presenter_menu_cursor(XrSession, const XrFrameEndInfo*, PublishedMenu&);
std::mutex published_menu_mutex;
// Shared while a presenter submits; exclusive before a menu swapchain is destroyed.
std::shared_mutex published_menu_lifetime;
std::unordered_map<XrSession, PublishedMenu> published_menus;

void register_presenter_end_frame(const XrSession session, const PFN_xrEndFrame next) {
    std::lock_guard lock(published_menu_mutex);
    auto& published = published_menus[session];
    published = {};
    published.next = next;
}

void unpublish_menu(const XrSession session) {
    std::lock_guard lock(published_menu_mutex);
    const auto found = published_menus.find(session);
    if (found != published_menus.end()) found->second.swapchain = XR_NULL_HANDLE;
}

XRAPI_ATTR XrResult XRAPI_CALL presenter_end_frame_with_menu(
    const XrSession session, const XrFrameEndInfo* const info) {
    std::shared_lock lifetime(published_menu_lifetime);
    PublishedMenu menu;
    {
        std::lock_guard lock(published_menu_mutex);
        const auto found = published_menus.find(session);
        if (found == published_menus.end() || !found->second.next) return XR_ERROR_HANDLE_INVALID;
        menu = found->second;
    }
    if (info && menu.swapchain) update_presenter_menu_cursor(session, info, menu);
    const auto count = menu.cylinder ? 1U : static_cast<std::uint32_t>(menu.strips.size());
    if (!info || menu.swapchain == XR_NULL_HANDLE || !count || (info->layerCount && !info->layers) ||
        info->layerCount + count > menu.max_layers) return menu.next(session, info);
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    if (info->layerCount) layers.assign(info->layers, info->layers + info->layerCount);
    if (menu.cylinder) layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menu.cylinder_layer));
    else for (const auto& strip : menu.strips) layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&strip));
    if (menu.cursor_visible && menu.cursor_swapchain && layers.size() < menu.max_layers)
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menu.cursor_layer));
    auto forwarded = *info;
    forwarded.layerCount = static_cast<std::uint32_t>(layers.size());
    forwarded.layers = layers.data();
    return menu.next(session, &forwarded);
}

void destroy_menu(SessionState::Menu& menu, const Dispatch& dispatch) noexcept {
    if (menu.swapchain != XR_NULL_HANDLE) {
        // Wait out a presenter submission that may still reference this swapchain.
        std::unique_lock lifetime(published_menu_lifetime);
        std::lock_guard lock(published_menu_mutex);
        for (auto& published : published_menus) {
            if (published.second.swapchain == menu.swapchain) published.second.swapchain = XR_NULL_HANDLE;
            if (published.second.cursor_swapchain == menu.swapchain) {
                published.second.cursor_swapchain = XR_NULL_HANDLE; published.second.cursor_visible = false;
            }
        }
    }
    // Resize/session teardown only: allocators and static cursor uploads must
    // outlive their last queued copy. Ordinary menu refresh never waits here.
    if (menu.fence && menu.fence_value && menu.fence->GetCompletedValue() < menu.fence_value) {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if (!menu.images12.empty() && menu.images12[0].texture)
            menu.images12[0].texture->GetDevice(IID_PPV_ARGS(&device));
        const auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        while(menu.fence->GetCompletedValue()<menu.fence_value) {
            if(device&&FAILED(device->GetDeviceRemovedReason()))break;
            if(event&&SUCCEEDED(menu.fence->SetEventOnCompletion(menu.fence_value,event)))WaitForSingleObject(event,100);
            else Sleep(1);
        }
        if(event)CloseHandle(event);
    }
    if (menu.swapchain != XR_NULL_HANDLE && dispatch.destroy_swapchain)
        static_cast<void>(dispatch.destroy_swapchain(menu.swapchain));
    menu = {};
}

XrVector3f rotate_menu_vector(const XrQuaternionf& q, const XrVector3f v) noexcept {
    const XrVector3f t{
        2.F * (q.y * v.z - q.z * v.y),
        2.F * (q.z * v.x - q.x * v.z),
        2.F * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + q.y * t.z - q.z * t.y,
        v.y + q.w * t.y + q.z * t.x - q.x * t.z,
        v.z + q.w * t.z + q.x * t.y - q.y * t.x};
}

bool anchor_menu(SessionState& session, const Dispatch& dispatch,
    const CheekyOpenXRMenuFrame& frame, const XrTime time) {
    auto& menu = session.menu;
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    // The runtime only knows stereo when the layer composes an app's quad views.
    locate.viewConfigurationType = session.quad_capture ?
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO : session.view_configuration;
    locate.displayTime = time;
    locate.space = session.calibration_local_space;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    std::uint32_t view_count{};
    if (XR_FAILED(dispatch.locate_views(session.session, &locate, &view_state,
        static_cast<std::uint32_t>(views.size()), &view_count, views.data())) ||
        view_count < 2 || !(view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) ||
        !(view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
        report_menu_status(cheeky_xr_menu_tracking_unavailable);
        return false;
    }
    const auto& head = views[0].pose;
    const auto head_forward = rotate_menu_vector(head.orientation, {0, 0, -1});
    const float yaw = std::atan2(-head_forward.x, -head_forward.z);
    menu.pose.orientation = {0, std::sin(yaw * .5F), 0, std::cos(yaw * .5F)};
    const auto forward = rotate_menu_vector(menu.pose.orientation, {0, 0, -1});
    menu.pose.position = {(views[0].pose.position.x + views[1].pose.position.x) * .5F + 1.5F * forward.x,
        (views[0].pose.position.y + views[1].pose.position.y) * .5F + 1.5F * forward.y,
        (views[0].pose.position.z + views[1].pose.position.z) * .5F + 1.5F * forward.z};
    const float meters_per_pixel = (std::min)(1.2F / 620.F,
        (cheeky::xr_menu::radius * cheeky::xr_menu::max_angle) / frame.display_width);
    menu.size = {frame.display_width * meters_per_pixel, frame.display_height * meters_per_pixel};
    menu.anchored = true;
    return true;
}

bool initialize_menu(SessionState& session, const Dispatch& dispatch,
    const CheekyOpenXRMenuFrame& frame, const XrTime time) {
    auto& menu = session.menu;
    if (menu.swapchain != XR_NULL_HANDLE && (menu.width != frame.width ||
        menu.height != frame.height || menu.format != frame.format)) destroy_menu(menu, dispatch);
    if (menu.swapchain != XR_NULL_HANDLE) return menu.anchored || anchor_menu(session, dispatch, frame, time);
    if (!dispatch.enumerate_swapchain_formats || !dispatch.create_swapchain ||
        !dispatch.enumerate_swapchain_images || !dispatch.locate_views ||
        session.calibration_local_space == XR_NULL_HANDLE ||
        frame.width < 160 || frame.height < 100 ||
        frame.display_width <= 0 || frame.display_height <= 0) {
        report_menu_status(cheeky_xr_menu_tracking_unavailable); return false;
    }
    std::uint32_t format_count{};
    if (XR_FAILED(dispatch.enumerate_swapchain_formats(session.session, 0, &format_count, nullptr)) ||
        !format_count || format_count > 1024) {
        report_menu_status(cheeky_xr_menu_format_unavailable); return false;
    }
    std::vector<std::int64_t> formats(format_count);
    if (XR_FAILED(dispatch.enumerate_swapchain_formats(session.session, format_count, &format_count, formats.data()))) {
        report_menu_status(cheeky_xr_menu_format_unavailable); return false;
    }
    XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    create.usageFlags = cheeky::xr_menu::swapchain_usage;
    create.sampleCount = 1;
    create.width = frame.width;
    create.height = frame.height;
    create.faceCount = 1;
    create.arraySize = 1;
    create.mipCount = 1;
    const DXGI_FORMAT candidates[]{static_cast<DXGI_FORMAT>(frame.format),
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM};
    // Prefer an exact copy, then a format in the same DXGI typeless family,
    // then a format that can receive the menu through a shader conversion.
    for (unsigned pass{}; pass < 3 && menu.swapchain == XR_NULL_HANDLE; ++pass)
        for (const auto candidate : candidates) {
            if ((pass == 0 && candidate != frame.format) ||
                (pass == 1 && (candidate == frame.format ||
                    !menu_copy_compatible(static_cast<DXGI_FORMAT>(frame.format), candidate))) ||
                (pass == 2 && menu_copy_compatible(static_cast<DXGI_FORMAT>(frame.format), candidate)) ||
                std::find(formats.begin(), formats.end(), static_cast<std::int64_t>(candidate)) == formats.end()) continue;
            create.format = static_cast<std::int64_t>(candidate);
            XrSwapchain created{XR_NULL_HANDLE};
            if (XR_SUCCEEDED(dispatch.create_swapchain(session.session, &create, &created))) {
                menu.swapchain = created;
                menu.target_format = static_cast<std::uint32_t>(candidate);
                break;
            }
        }
    if (menu.swapchain == XR_NULL_HANDLE) {
        report_menu_status(cheeky_xr_menu_swapchain_failed); return false;
    }
    menu.width = frame.width;
    menu.height = frame.height;
    menu.format = frame.format;
    {
        char message[160]{};
        std::snprintf(message, sizeof(message),
            "OpenXR menu: swapchain source format %u, target format %u, API D3D%u",
            frame.format, menu.target_format, session.graphics_api);
        report_menu_diagnostic(message);
    }
    std::uint32_t count{};
    if (XR_FAILED(dispatch.enumerate_swapchain_images(menu.swapchain, 0, &count, nullptr)) || !count || count > 16) {
        report_menu_status(cheeky_xr_menu_image_failed);
        destroy_menu(menu, dispatch); return false;
    }
    if (session.graphics_api == 11) {
        menu.images11.resize(count);
        for (auto& image : menu.images11) image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
        if (XR_FAILED(dispatch.enumerate_swapchain_images(menu.swapchain, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(menu.images11.data())))) {
            report_menu_status(cheeky_xr_menu_image_failed);
            destroy_menu(menu, dispatch); return false;
        }
        if (!menu_copy_compatible(static_cast<DXGI_FORMAT>(frame.format),
            static_cast<DXGI_FORMAT>(menu.target_format)) &&
            !initialize_menu_converter11(menu, static_cast<ID3D11Device*>(frame.device))) {
            report_menu_status(cheeky_xr_menu_copy_failed);
            destroy_menu(menu, dispatch); return false;
        }
    } else if (session.graphics_api == 12) {
        menu.images12.resize(count);
        for (auto& image : menu.images12) image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
        if (XR_FAILED(dispatch.enumerate_swapchain_images(menu.swapchain, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(menu.images12.data())))) {
            report_menu_status(cheeky_xr_menu_image_failed);
            destroy_menu(menu, dispatch); return false;
        }
        auto* device = static_cast<ID3D12Device*>(frame.device);
        menu.allocators.resize(count);
        menu.fence_values.resize(count);
        for (auto& allocator : menu.allocators) if (FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) {
            report_menu_diagnostic("OpenXR menu: D3D12 command allocator creation failed");
            session.menu_submission_disabled = true;
            report_menu_status(cheeky_xr_menu_copy_failed);
            destroy_menu(menu, dispatch); return false;
        }
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            menu.allocators[0].Get(), nullptr, IID_PPV_ARGS(&menu.command_list))) ||
            FAILED(menu.command_list->Close()) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&menu.fence)))) {
            report_menu_diagnostic("OpenXR menu: D3D12 command list or fence creation failed");
            session.menu_submission_disabled = true;
            report_menu_status(cheeky_xr_menu_copy_failed);
            destroy_menu(menu, dispatch); return false;
        }
        if (!menu_copy_compatible(static_cast<DXGI_FORMAT>(frame.format),
            static_cast<DXGI_FORMAT>(menu.target_format)) &&
            !initialize_menu_converter12(menu, device)) {
            session.menu_submission_disabled = true;
            report_menu_status(cheeky_xr_menu_copy_failed);
            destroy_menu(menu, dispatch); return false;
        }
    } else { destroy_menu(menu, dispatch); return false; }

    report_menu_diagnostic("OpenXR menu: non-blocking refresh; previous released menu retained while update is pending");
    return menu.anchored || anchor_menu(session, dispatch, frame, time);
}

// User SteamVR bindings can omit actions added by a layer. Read the host's
// already-bound aim/trigger as a fallback; never replace bindings, resync action
// sets, or consume/modify game input. Used only when the layer action is inactive.
bool sample_host_menu_hand(SessionState& session, const InstanceState& instance,
    unsigned hand, XrTime time, XrSpaceLocation& location, XrActionStateBoolean& click) {
    const auto& d = instance.dispatch;
    if (!d.get_current_profile || !d.string_to_path || !d.create_action_space || !d.locate_space)
        return false;
    XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (XR_FAILED(d.get_current_profile(session.session, instance.menu_hand_paths[hand], &profile)) ||
        profile.interactionProfile == XR_NULL_PATH) return false;
    auto& cached = session.host_menu_hands[hand];
    if (!cached.initialized || cached.profile != profile.interactionProfile) {
        if (cached.space && d.destroy_space) d.destroy_space(cached.space);
        cached = {}; cached.initialized = true; cached.profile = profile.interactionProfile;
        const char* prefix = hand ? "/user/hand/right" : "/user/hand/left";
        XrPath aim_path{}, trigger_path{}, select_path{};
        d.string_to_path(instance.instance, (std::string(prefix) + "/input/aim/pose").c_str(), &aim_path);
        d.string_to_path(instance.instance, (std::string(prefix) + "/input/trigger/value").c_str(), &trigger_path);
        d.string_to_path(instance.instance, (std::string(prefix) + "/input/select/click").c_str(), &select_path);
        XrAction aim{};
        for (unsigned i = 0; i < instance.menu_profiles.size(); ++i) {
            if (instance.menu_profiles[i] != profile.interactionProfile) continue;
            for (const auto& binding : instance.host_menu_profile_bindings[i]) {
                if (binding.binding == aim_path) aim = binding.action;
                if (binding.binding == trigger_path || binding.binding == select_path) cached.trigger = binding.action;
            }
        }
        cached.trigger_subaction = instance.menu_hand_paths[hand];
        if (aim) {
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            info.action = aim; info.subactionPath = instance.menu_hand_paths[hand];
            info.poseInActionSpace.orientation.w = 1.F;
            auto result = d.create_action_space(session.session, &info, &cached.space);
            if (result == XR_ERROR_PATH_UNSUPPORTED) {
                info.subactionPath = XR_NULL_PATH;
                result = d.create_action_space(session.session, &info, &cached.space);
            }
            char message[256]{};
            std::snprintf(message, sizeof(message), "Menu host fallback hand=%u profile=%llu aim=%p space=%p trigger=%p result=%d",
                hand, static_cast<unsigned long long>(cached.profile), aim, cached.space, cached.trigger, result);
            report_menu_diagnostic(message);
        }
    }
    bool valid_pose = false;
    if (cached.space) valid_pose = XR_SUCCEEDED(d.locate_space(cached.space,
        session.calibration_local_space, time, &location)) &&
        (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
        (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    if (!cached.trigger) return valid_pose;
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = cached.trigger; get.subactionPath = cached.trigger_subaction;
    XrResult result = XR_ERROR_ACTION_TYPE_MISMATCH;
    if (d.get_action_state_float && cached.trigger_type != XR_ACTION_TYPE_BOOLEAN_INPUT) {
        XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
        result = d.get_action_state_float(session.session, &get, &value);
        if (result == XR_ERROR_PATH_UNSUPPORTED) {
            get.subactionPath = cached.trigger_subaction = XR_NULL_PATH;
            result = d.get_action_state_float(session.session, &get, &value);
        }
        if (XR_SUCCEEDED(result)) {
            cached.trigger_type = XR_ACTION_TYPE_FLOAT_INPUT;
            click.isActive = value.isActive;
            click.currentState = value.currentState >= .55F;
        }
    }
    if (d.get_action_state_boolean && (cached.trigger_type == XR_ACTION_TYPE_BOOLEAN_INPUT ||
        result == XR_ERROR_ACTION_TYPE_MISMATCH)) {
        XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
        result = d.get_action_state_boolean(session.session, &get, &value);
        if (result == XR_ERROR_PATH_UNSUPPORTED) {
            get.subactionPath = cached.trigger_subaction = XR_NULL_PATH;
            result = d.get_action_state_boolean(session.session, &get, &value);
        }
        if (XR_SUCCEEDED(result)) { cached.trigger_type = XR_ACTION_TYPE_BOOLEAN_INPUT; click = value; }
    }
    return valid_pose;
}

struct MenuPointerSample {float u{},v{};bool down{},active{};};
MenuPointerSample send_menu_pointer(SessionState& session, const Dispatch& dispatch,
    const XrTime time, const CheekyOpenXRMenuFrame& frame, CheekyOpenXRMenuPointer pointer_sink = nullptr,
    const SessionState::Menu* displayed = nullptr) {
    const auto host = GetModuleHandleW(L"CheekyFoveatedDLSSHost.dll");
    const auto send = pointer_sink ? pointer_sink : host ? reinterpret_cast<CheekyOpenXRMenuPointer>(
        GetProcAddress(host, "CheekyOpenXRMenuSetPointer")) : nullptr;
    if (!send || !dispatch.locate_space || !dispatch.get_action_state_boolean || frame.display_width <= 0 || frame.display_height <= 0) return {};
    const auto instance = instances.find(session.instance);
    if (instance == instances.end() || instance->second.menu_click_action == XR_NULL_HANDLE ||
        !session.action_attached) { send(0, 0, false, false); return {}; }
    const auto& menu = displayed ? *displayed : session.menu;
    const auto& orientation = menu.pose.orientation;
    const XrQuaternionf inverse{-orientation.x, -orientation.y, -orientation.z, orientation.w};
    struct Pointer { bool hit{}, down{}; float x{}, y{}; };
    std::array<Pointer, 2> pointers{};
    std::array<XrSpaceLocationFlags, 2> pose_flags{};
    std::array<XrActionStateBoolean, 2> clicks{};
    std::array<bool, 2> host_fallback{};
    for (const unsigned hand : {1U, 0U}) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
        get_info.action = instance->second.menu_click_action;
        get_info.subactionPath = instance->second.menu_hand_paths[hand];
        auto& click = clicks[hand];
        click.type = XR_TYPE_ACTION_STATE_BOOLEAN;
        dispatch.get_action_state_boolean(session.session, &get_info, &click);
        auto located = session.menu_aim_spaces[hand] == XR_NULL_HANDLE ? XR_ERROR_HANDLE_INVALID :
            dispatch.locate_space(session.menu_aim_spaces[hand], session.calibration_local_space, time, &location);
        if (!click.isActive || XR_FAILED(located) || !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
            !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            XrSpaceLocation fallback_location{XR_TYPE_SPACE_LOCATION};
            XrActionStateBoolean fallback_click{XR_TYPE_ACTION_STATE_BOOLEAN};
            const bool fallback_pose = sample_host_menu_hand(session, instance->second, hand, time,
                fallback_location, fallback_click);
            if (fallback_pose && (XR_FAILED(located) ||
                !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
                !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))) {
                location = fallback_location; located = XR_SUCCESS; host_fallback[hand] = true;
            }
            if (!click.isActive && fallback_click.isActive) { click = fallback_click; host_fallback[hand] = true; }
        }
        const bool down = click.isActive && click.currentState;
        pose_flags[hand] = location.locationFlags;
        if (XR_FAILED(located) ||
            !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
            !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) continue;
        const XrVector3f position{
            location.pose.position.x - menu.pose.position.x,
            location.pose.position.y - menu.pose.position.y,
            location.pose.position.z - menu.pose.position.z};
        const auto origin = rotate_menu_vector(inverse, position);
        const auto world_direction = rotate_menu_vector(location.pose.orientation, {0, 0, -1});
        const auto direction = rotate_menu_vector(inverse, world_direction);
        float u{}, v{};
        if (!cheeky::xr_menu::hit(origin, direction, menu.size.width, menu.size.height,
            menu.width, menu.strips, menu.cylinder, u, v)) continue;
        pointers[hand] = {true, down, u * frame.display_width, v * frame.display_height};
    }
    int selected = session.menu_pointer_hand;
    if (selected < 0 || !pointers[selected].hit) {
        selected = -1;
        for (const int hand : {1, 0}) if (pointers[hand].hit && pointers[hand].down) { selected = hand; break; }
        if (selected < 0) for (const int hand : {1, 0}) if (pointers[hand].hit) { selected = hand; break; }
    }
    MenuPointerSample sample;
    if (selected >= 0) {
        const auto& p = pointers[selected];
        sample = {p.x / frame.display_width, p.y / frame.display_height, p.down, true};
        send(p.x, p.y, p.down, true);
        session.menu_pointer_hand = p.down ? selected : -1;
    } else { session.menu_pointer_hand = -1; send(0, 0, false, false); }
    const auto now = GetTickCount64();
    if (now - session.menu_pointer_log_tick >= 1000) {
        session.menu_pointer_log_tick = now;
        char diagnostic[320]{};
        std::snprintf(diagnostic, sizeof(diagnostic),
            "XR pointer v5: hand=%d left(pose=%llu active=%u down=%u hit=%u xy=%.0f,%.0f) right(pose=%llu active=%u down=%u hit=%u xy=%.0f,%.0f) host_fallback=%d,%d",
            selected, static_cast<unsigned long long>(pose_flags[0]), clicks[0].isActive, clicks[0].currentState,
            unsigned(pointers[0].hit), pointers[0].x, pointers[0].y,
            static_cast<unsigned long long>(pose_flags[1]), clicks[1].isActive, clicks[1].currentState,
            unsigned(pointers[1].hit), pointers[1].x, pointers[1].y, host_fallback[0], host_fallback[1]);
        report_menu_diagnostic(diagnostic);
    }
    return sample;
}

// Copies the open F8 menu into its swapchain, then hands the frame to `end`
// (the runtime, the FG presenter or the quad compositor). Without a presenter
// the menu layer follows the app's layers here; a presenter instead takes the
// published layer into each runtime frame it submits.
// A timeout leaves the image acquired. Poll that same image on the next menu
// update; never acquire another one or release an image whose wait timed out.
XrResult poll_menu_image(SessionState::Menu& menu, const Dispatch& dispatch) {
    if (!dispatch.acquire_swapchain_image || !dispatch.wait_swapchain_image || !dispatch.release_swapchain_image)
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!menu.acquired) {
        const XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        const auto result = dispatch.acquire_swapchain_image(menu.swapchain, &acquire, &menu.acquired_index);
        if (XR_FAILED(result)) return result;
        menu.acquired = true; menu.waited = false;
    }
    if (!menu.waited) {
        const XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, 0};
        const auto result = dispatch.wait_swapchain_image(menu.swapchain, &wait);
        if (result != XR_SUCCESS && result != XR_SESSION_LOSS_PENDING) return result;
        menu.waited = true;
    }
    return XR_SUCCESS;
}

XrResult release_menu_image(SessionState::Menu& menu, const Dispatch& dispatch) {
    if (!menu.acquired || !menu.waited) return XR_ERROR_CALL_ORDER_INVALID;
    const XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const auto result = dispatch.release_swapchain_image(menu.swapchain, &release);
    if (XR_SUCCEEDED(result)) menu.acquired = menu.waited = false;
    return result;
}

#include "menu_cursor.inl"

template <class EndFrame>
XrResult submit_menu_layers(const XrFrameEndInfo* info, SessionState& state,
    const bool presenter, const SessionState::Menu& menu, EndFrame&& end) {
    const auto count = menu.cylinder ? 1u : menu.strips;
    if (!menu.swapchain || !count || info->layerCount + count > state.max_layers) return end(info);
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    if (info->layerCount && info->layers) layers.assign(info->layers, info->layers + info->layerCount);
    XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
    std::vector<XrCompositionLayerQuad> strips(menu.cylinder ? 0U : menu.strips);
    if (menu.cylinder) {
        cylinder.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        cylinder.space = state.calibration_local_space;
        cylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        cylinder.subImage.swapchain = menu.swapchain;
        cylinder.subImage.imageRect.extent = {static_cast<std::int32_t>(menu.width), static_cast<std::int32_t>(menu.height)};
        cylinder.pose = menu.pose;
        const auto offset = rotate_menu_vector(menu.pose.orientation, {0, 0, cheeky::xr_menu::radius});
        cylinder.pose.position.x += offset.x;
        cylinder.pose.position.z += offset.z;
        cylinder.radius = cheeky::xr_menu::radius;
        cylinder.centralAngle = menu.size.width / cylinder.radius;
        cylinder.aspectRatio = menu.size.width / menu.size.height;
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinder));
    } else {
        // Runtimes without cylinders receive a single flat full-image panel.
        // Geometry and pointer hit-testing both use the same one-strip policy.
        for (unsigned i{}; i < menu.strips; ++i) {
            const auto geometry = cheeky::xr_menu::strip(menu.size.width, menu.width, i, menu.strips);
            auto& quad = strips[i];
            quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            quad.space = state.calibration_local_space;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = menu.swapchain;
            quad.subImage.imageRect.offset.x = static_cast<std::int32_t>(geometry.left);
            quad.subImage.imageRect.extent = {static_cast<std::int32_t>(geometry.right - geometry.left), static_cast<std::int32_t>(menu.height)};
            quad.pose = menu.pose;
            const auto offset = rotate_menu_vector(menu.pose.orientation, geometry.center);
            quad.pose.position.x += offset.x;
            quad.pose.position.z += offset.z;
            const float sn = std::sin(-geometry.angle * .5F), cs = std::cos(-geometry.angle * .5F);
            quad.pose.orientation.y = menu.pose.orientation.y * cs + menu.pose.orientation.w * sn;
            quad.pose.orientation.w = menu.pose.orientation.w * cs - menu.pose.orientation.y * sn;
            quad.size = {geometry.width, menu.size.height};
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
        }
    }
    PublishedMenu live;
    live.swapchain=menu.swapchain;live.max_layers=state.max_layers;live.cylinder=menu.cylinder;live.strips=strips;
    set_menu_cursor_geometry(live,menu);
    {
        std::lock_guard lock(state_mutex);
        const auto instance=instances.find(state.instance);
        if(instance!=instances.end())update_menu_cursor_locked(state,instance->second.dispatch,info,live);
    }
    if (presenter) {
        {
            std::lock_guard lock(published_menu_mutex);
            auto& published = published_menus[state.session];
            published.swapchain = menu.swapchain;
            published.max_layers = state.max_layers;
            published.cylinder = menu.cylinder;
            published.cylinder_layer = cylinder;
            published.strips = strips;
            set_menu_cursor_geometry(published, menu);
            published.cursor_swapchain=live.cursor_swapchain;published.cursor_layer=live.cursor_layer;
            published.cursor_visible=live.cursor_visible;published.cursor_controller_drag=live.cursor_controller_drag;
        }
        const auto result = end(info);
        report_menu_status(XR_SUCCEEDED(result) ? cheeky_xr_menu_submitted : cheeky_xr_menu_end_frame_rejected);
        return result;
    }
    unpublish_menu(state.session);
    if(live.cursor_visible&&layers.size()<state.max_layers)
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&live.cursor_layer));
    auto forwarded = *info;
    forwarded.layerCount = static_cast<std::uint32_t>(layers.size());
    forwarded.layers = layers.data();
    const auto result = end(&forwarded);
    report_menu_status(XR_SUCCEEDED(result) ? cheeky_xr_menu_submitted : cheeky_xr_menu_end_frame_rejected);
    return result;
}

template <class EndFrame>
XrResult submit_menu_frame(const XrFrameEndInfo* info, const Dispatch& dispatch,
    SessionState& state, const bool presenter, EndFrame&& end) {
    // Quad sessions reach the runtime as stereo after composition.
    if (!info || !state.running ||
        (state.view_configuration != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO && !state.quad_capture) ||
        (state.graphics_api != 11 && state.graphics_api != 12))
        return end(info);
    const auto keep_previous = [&] {
        return state.cached_menu.swapchain ? submit_menu_layers(info, state, presenter, state.cached_menu, end) : end(info);
    };
    const auto host = GetModuleHandleW(L"CheekyFoveatedDLSSHost.dll");
    const auto acquire = host ? reinterpret_cast<CheekyOpenXRMenuAcquire>(
        GetProcAddress(host, "CheekyOpenXRMenuAcquireFrame")) : nullptr;
    if (!acquire) return end(info);
    const auto try_acquire = reinterpret_cast<CheekyOpenXRMenuTryAcquire>(
        GetProcAddress(host, "CheekyOpenXRMenuTryAcquireFrame"));
    CheekyOpenXRMenuFrame frame{};
    const auto acquired = try_acquire ? try_acquire(&frame) : acquire(&frame) ? S_OK : S_FALSE;
    if (acquired == E_PENDING) return keep_previous();
    if (acquired != S_OK) {
        { std::lock_guard lock(state_mutex); state.diagnostic_menu_visible = false; }
        state.menu.anchored = false; // Reopening places the panel in front of the current head pose.
        state.cached_menu.anchored = false;
        state.menu_pointer_hand = -1;
        state.menu_submission_disabled = false;
        unpublish_menu(state.session);
        return end(info);
    }
    { std::lock_guard lock(state_mutex); state.diagnostic_menu_visible = true; }
    if (state.menu_submission_disabled) {
        if (frame.texture) static_cast<IUnknown*>(frame.texture)->Release();
        unpublish_menu(state.session);
        return end(info);
    }
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> shared_mutex;
    if (frame.texture && frame.graphics_api == 11 && state.graphics_api == 11 &&
        frame.device != state.graphics_device) {
        const auto share = reinterpret_cast<CheekyOpenXRMenuShared11>(
            GetProcAddress(host, "CheekyOpenXRMenuAcquireShared11"));
        void* texture{};
        void* mutex{};
        const auto hr = share ? share(frame.texture, state.graphics_device, &texture, &mutex) : E_NOINTERFACE;
        if (hr != S_OK) {
            if (hr != S_FALSE) {
                char message[128]{};
                std::snprintf(message, sizeof(message), "OpenXR menu: shared D3D11 handoff failed (0x%08lX)",
                    static_cast<unsigned long>(hr));
                report_menu_diagnostic(message);
                state.menu_submission_disabled = true;
                report_menu_status(cheeky_xr_menu_copy_failed);
            }
            static_cast<IUnknown*>(frame.texture)->Release();
            return end(info);
        }
        static_cast<IUnknown*>(frame.texture)->Release();
        frame.texture = texture;
        frame.device = state.graphics_device;
        shared_mutex.Attach(static_cast<IDXGIKeyedMutex*>(mutex));
    }
    const auto release_source = [&frame, &shared_mutex] {
        if (shared_mutex) static_cast<void>(shared_mutex->ReleaseSync(0));
        static_cast<IUnknown*>(frame.texture)->Release();
    };
    // MSFS's mirror and XR devices are one D3D12 device behind a proxy
    // (Streamline): pointers differ, but resources and queues interoperate.
    const bool same_device = frame.device == state.graphics_device ||
        (frame.graphics_api == 12 && state.graphics_api == 12 && frame.device && state.graphics_device &&
         cheeky::foveated_dlss::same_d3d12_device(static_cast<ID3D12Device*>(frame.device),
             static_cast<ID3D12Device*>(state.graphics_device)));
    if (!frame.texture || frame.graphics_api != state.graphics_api ||
        !same_device ||
        (state.graphics_api == 12 && (!frame.queue || !state.graphics_queue))) {
        report_menu_status(frame.graphics_api != state.graphics_api ? cheeky_xr_menu_graphics_mismatch :
            !same_device ? cheeky_xr_menu_device_mismatch : cheeky_xr_menu_copy_failed);
        static bool reported_d3d12_devices{};
        if (frame.graphics_api == 12 && state.graphics_api == 12 && frame.device != state.graphics_device &&
            frame.device && state.graphics_device && !std::exchange(reported_d3d12_devices, true)) {
            // A proxy (Streamline, ReShade) can wrap one of the two; record whether
            // they are one underlying device before the menu can bridge them.
            char message[192]{};
            std::snprintf(message, sizeof(message), "OpenXR menu: D3D12 mirror device %p, XR device %p, same underlying device %u",
                frame.device, state.graphics_device, unsigned(cheeky::foveated_dlss::same_d3d12_device(
                    static_cast<ID3D12Device*>(frame.device), static_cast<ID3D12Device*>(state.graphics_device))));
            report_menu_diagnostic(message);
        }
        if (frame.texture) release_source();
        return end(info);
    }
    // Build the menu converter on the XR device itself, never on its proxy:
    // its command lists execute on the runtime's queue.
    frame.device = state.graphics_device;
    if (state.menu_recenter_time && info->displayTime >= state.menu_recenter_time) {
        state.menu.anchored = false;
        state.menu_recenter_time = 0;
        report_menu_diagnostic("OpenXR menu: reanchoring after runtime recenter");
    }
    if (!initialize_menu(state, dispatch, frame, info->displayTime)) {
        release_source(); return end(info);
    }
    auto& menu = state.menu;
    menu.display_width = frame.display_width; menu.display_height = frame.display_height;
    const auto available_layers = state.max_layers > info->layerCount ? state.max_layers - info->layerCount : 0U;
    if (!available_layers) { release_source(); return end(info); }
    menu.cylinder = state.menu_cylinder_enabled;
    menu.strips = menu.cylinder ? 1U : cheeky::xr_menu::fallback_layers(available_layers);
    if (menu.fence_value == 0 && state.graphics_api == 12)
        report_menu_diagnostic(menu.cylinder ? "OpenXR menu policy: native cylinder, sampled texture" :
            "OpenXR menu policy: single full-image quad, sampled texture");
    // Pointer sampling follows presentation, independently of background refresh.
    const auto available = poll_menu_image(menu, dispatch);
    if (available == XR_TIMEOUT_EXPIRED) { release_source(); return keep_previous(); }
    if (available != XR_SUCCESS) {
        report_menu_status(cheeky_xr_menu_image_failed);
        release_source(); return keep_previous();
    }
    const auto index = menu.acquired_index;
    // Runtime availability does not imply our previous queue submission has
    // completed yet. A busy allocator is backpressure, not a permanent copy
    // failure: never poison the headset menu until it is reopened for this.
    if (state.graphics_api == 12 && index < menu.fence_values.size() && menu.fence &&
        menu.fence->GetCompletedValue() != UINT64_MAX &&
        menu.fence->GetCompletedValue() < menu.fence_values[index]) {
        release_source();
        return keep_previous();
    }
    bool copied{};
    const char* copy_failure_stage = "image, allocator or fence not ready";
    if (state.graphics_api == 11 && index < menu.images11.size()) {
        if (menu_copy_compatible(static_cast<DXGI_FORMAT>(frame.format),
            static_cast<DXGI_FORMAT>(menu.target_format))) {
            if (shared_mutex && menu.images11[index].texture) {
                Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
                auto* device = static_cast<ID3D11Device*>(frame.device);
                device->GetImmediateContext(&context);
                context->CopyResource(menu.images11[index].texture, static_cast<ID3D11Texture2D*>(frame.texture));
                context->Flush();
                copied = SUCCEEDED(device->GetDeviceRemovedReason());
            } else {
                const auto copy = reinterpret_cast<CheekyOpenXRMenuCopy11>(
                    GetProcAddress(host, "CheekyOpenXRMenuCopy11"));
                if (copy && menu.images11[index].texture)
                    copied = copy(menu.images11[index].texture, frame.texture);
            }
        } else copied = render_menu_converter11(menu, frame, index, host, shared_mutex != nullptr);
    } else if (state.graphics_api == 12 && index < menu.images12.size() &&
        index < menu.allocators.size() && menu.command_list && menu.fence &&
        menu.fence->GetCompletedValue() >= menu.fence_values[index]) {
        auto* target = menu.images12[index].texture;
        auto* source = static_cast<ID3D12Resource*>(frame.texture);
        auto* list = menu.command_list.Get();
        copy_failure_stage = "command reset failed";
        if (target && SUCCEEDED(menu.allocators[index]->Reset()) &&
            SUCCEEDED(list->Reset(menu.allocators[index].Get(), nullptr))) {
            const bool convert = !menu_copy_compatible(static_cast<DXGI_FORMAT>(frame.format),
                static_cast<DXGI_FORMAT>(menu.target_format));
            if (menu.fence_value == 0) {
                char message[192]{};
                std::snprintf(message, sizeof(message),
                    "OpenXR menu: first D3D12 %s, image %u, queues %s",
                    convert ? "conversion" : "copy", index,
                    frame.queue == state.graphics_queue ? "shared" : "separate");
                report_menu_diagnostic(message);
            }
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = target;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            barrier.Transition.StateAfter = convert ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COPY_DEST;
            list->ResourceBarrier(1, &barrier);
            if (convert) {
                D3D12_RESOURCE_BARRIER source_barrier{};
                source_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                source_barrier.Transition.pResource = source;
                source_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                source_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                source_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                list->ResourceBarrier(1, &source_barrier);
                auto* device = static_cast<ID3D12Device*>(frame.device);
                auto* heap = menu.convert_srv_heaps[index].Get();
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = static_cast<DXGI_FORMAT>(frame.format);
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(source, &srv, heap->GetCPUDescriptorHandleForHeapStart());
                const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(menu.width),
                    static_cast<float>(menu.height), 0, 1};
                const D3D12_RECT scissor{0, 0, static_cast<LONG>(menu.width), static_cast<LONG>(menu.height)};
                auto rtv = menu.convert_rtv_heap->GetCPUDescriptorHandleForHeapStart();
                rtv.ptr += SIZE_T(index) * menu.convert_rtv_stride;
                list->SetPipelineState(menu.convert_pipeline.Get());
                list->SetGraphicsRootSignature(menu.convert_root.Get());
                list->SetDescriptorHeaps(1, &heap);
                list->SetGraphicsRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
                list->RSSetViewports(1, &viewport);
                list->RSSetScissorRects(1, &scissor);
                list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                list->DrawInstanced(3, 1, 0, 0);
                std::swap(source_barrier.Transition.StateBefore, source_barrier.Transition.StateAfter);
                list->ResourceBarrier(1, &source_barrier);
            } else list->CopyResource(target, source);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(1, &barrier);
            copy_failure_stage = "command list close failed";
            if (SUCCEEDED(list->Close())) {
                const auto value = menu.fence_value + 1;
                const auto submit = reinterpret_cast<CheekyOpenXRMenuSubmit12>(
                    GetProcAddress(host, "CheekyOpenXRMenuSubmit12"));
                const auto try_submit = reinterpret_cast<CheekyOpenXRMenuTrySubmit12>(
                    GetProcAddress(host, "CheekyOpenXRMenuTrySubmit12"));
                copy_failure_stage = submit ? "queue submission failed" : "host submission entry unavailable";
                if (try_submit) {
                    const auto result = try_submit(frame.queue, state.graphics_queue, list, menu.fence.Get(), value);
                    if (result == E_PENDING || result == S_FALSE) { release_source(); return keep_previous(); }
                    copied = result == S_OK;
                } else copied = submit && submit(frame.queue, state.graphics_queue, list, menu.fence.Get(), value);
                if (copied) menu.fence_values[index] = menu.fence_value = value;
            }
        }
    }
    release_source();
    const auto released = release_menu_image(menu, dispatch);
    if (XR_FAILED(released) || !copied) {
        if (!copied && state.graphics_api == 12) {
            report_menu_diagnostic(copy_failure_stage);
            state.menu_submission_disabled = true;
        }
        report_menu_status(copied ? cheeky_xr_menu_image_failed : cheeky_xr_menu_copy_failed);
        return keep_previous();
    }
    // Preserve the released/displayed chain while the other one is acquired
    // and polled next frame. Its descriptors and GPU contexts travel with it.
    std::swap(state.menu, state.cached_menu);
    state.menu.pose = state.cached_menu.pose;
    state.menu.size = state.cached_menu.size;
    state.menu.anchored = state.cached_menu.anchored;
    return submit_menu_layers(info, state, presenter, state.cached_menu, end);
}

struct SnapshotSlot {
    std::atomic<std::uint32_t> readers{};
    CheekyGazeSnapshotV1 snapshot{};
    CheekyGazeInputDiagnosticsV1 input{};
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
            if (session->using_submitted_projection) target.flags |= CHEEKY_GAZE_VIEW_SUBMITTED_PROJECTION;
            if (session->forward_valid) target.flags |= CHEEKY_GAZE_VIEW_FORWARD_VALID;
            target.forward_u = session->forward_u[index];
            target.forward_v = session->forward_v[index];
            if (session->eye_fov_valid) {
                const auto& fov = session->eye_fov[index];
                target.fov_left = fov.angleLeft; target.fov_right = fov.angleRight;
                target.fov_up = fov.angleUp; target.fov_down = fov.angleDown;
                target.flags |= CHEEKY_GAZE_VIEW_FOV_VALID;
            }
            if (session->next_jump_valid) target.flags |= CHEEKY_GAZE_VIEW_NEXT_JUMP_VALID;
            target.next_jump_u = session->next_jump_u[index];
            target.next_jump_v = session->next_jump_v[index];
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
    auto input = session ? session->input : CheekyGazeInputDiagnosticsV1{};
    input.version = CHEEKY_GAZE_INPUT_DIAGNOSTICS_VERSION;
    input.structure_size = sizeof(input);
    input.session_generation = session ? session->generation : 0;
    input.action_attached = session && session->action_attached;
    input.host_action_sets_created = instance && instance->host_action_sets_created;
    input.binding_submitted = instance && instance->gaze_binding_submitted;
    input.binding_result = instance ? instance->binding_result : CHEEKY_GAZE_RESULT_NOT_CALLED;
    snapshot_slots[target].input = input;
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

enum class ExtensionAvailability { present, absent, unknown };

void log_startup(const char* format, ...) noexcept {
    // Startup only; failure to write diagnostics must never affect OpenXR.
    try {
        static std::mutex log_mutex;
        std::lock_guard lock(log_mutex);
        char message[1024]{};
        va_list args;
        va_start(args, format);
        vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
        va_end(args);
        char line[1200]{};
        _snprintf_s(line, sizeof(line), _TRUNCATE, "tick=%llu thread=%lu %s",
            GetTickCount64(), GetCurrentThreadId(), message);
        OutputDebugStringA(line);
        const auto path = cheeky::openxr_startup_log_path();
        if (path.empty()) return;
        const auto file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written{};
        WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
        CloseHandle(file);
    } catch (...) {}
}

[[nodiscard]] ExtensionAvailability extension_availability(
    const PFN_xrGetInstanceProcAddr gipa,
    const char* name = XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME
) noexcept {
    PFN_xrVoidFunction function{};
    const auto lookup = gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &function);
    if (XR_FAILED(lookup) || !function) {
        log_startup("probe extension=%s stage=lookup result=%d function_present=%u availability=unknown\n",
            name, lookup, function ? 1U : 0U);
        return ExtensionAvailability::unknown;
    }
    const auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(function);
    std::uint32_t count{};
    const auto count_result = enumerate(nullptr, 0U, &count, nullptr);
    if (XR_FAILED(count_result)) {
        log_startup("probe extension=%s stage=count result=%d availability=unknown\n", name, count_result);
        return ExtensionAvailability::unknown;
    }
    if (count == 0U) {
        log_startup("probe extension=%s stage=count result=%d availability=absent\n", name, count_result);
        return ExtensionAvailability::absent;
    }
    std::vector<XrExtensionProperties> properties(
        count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES}
    );
    const auto list_result = enumerate(nullptr, count, &count, properties.data());
    if (XR_FAILED(list_result)) {
        log_startup("probe extension=%s stage=list result=%d availability=unknown\n", name, list_result);
        return ExtensionAvailability::unknown;
    }
    const auto found = std::any_of(
        properties.begin(), properties.end(), [name](const auto& property) {
            return std::strcmp(
                property.extensionName,
                name
            ) == 0;
        }
    );
    log_startup("probe extension=%s stage=list result=%d availability=%s\n", name, list_result,
        found ? "present" : "absent");
    return found ? ExtensionAvailability::present : ExtensionAvailability::absent;
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

std::vector<XrActionSuggestedBinding> menu_bindings_for_profile(
    const InstanceState& state, const std::size_t profile_index) {
    std::vector<XrActionSuggestedBinding> bindings;
    if (!state.menu_graphics_enabled || state.menu_aim_action == XR_NULL_HANDLE || state.menu_click_action == XR_NULL_HANDLE ||
        profile_index >= state.menu_profiles.size()) return bindings;
    const char* hands[]{"/user/hand/left", "/user/hand/right"};
    const char* click = profile_index == 0 ? "/input/select/click" : "/input/trigger/value";
    for (const auto* hand : hands) {
        XrPath aim{}, select{};
        if (XR_SUCCEEDED(state.dispatch.string_to_path(state.instance,
            (std::string(hand) + "/input/aim/pose").c_str(), &aim)) &&
            XR_SUCCEEDED(state.dispatch.string_to_path(state.instance,
            (std::string(hand) + click).c_str(), &select))) {
            bindings.push_back({state.menu_aim_action, aim});
            bindings.push_back({state.menu_click_action, select});
        }
    }
    return bindings;
}

bool create_menu_actions(InstanceState& state) noexcept {
    const auto& dispatch = state.dispatch;
    if (!dispatch.create_action_set || !dispatch.create_action || !dispatch.string_to_path ||
        !dispatch.destroy_action || !dispatch.destroy_action_set) return false;
    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    static_cast<void>(strcpy_s(set_info.actionSetName, "cheeky_menu"));
    static_cast<void>(strcpy_s(set_info.localizedActionSetName, "Cheeky Menu"));
    if (XR_FAILED(dispatch.create_action_set(state.instance, &set_info, &state.menu_action_set))) return false;
    const char* hands[]{"/user/hand/left", "/user/hand/right"};
    for (std::size_t i{}; i < 2; ++i) if (XR_FAILED(dispatch.string_to_path(
        state.instance, hands[i], &state.menu_hand_paths[i]))) return false;
    const char* profiles[]{"/interaction_profiles/khr/simple_controller",
        "/interaction_profiles/oculus/touch_controller",
        "/interaction_profiles/valve/index_controller",
        "/interaction_profiles/htc/vive_controller",
        "/interaction_profiles/microsoft/motion_controller"};
    for (std::size_t i{}; i < state.menu_profiles.size(); ++i)
        static_cast<void>(dispatch.string_to_path(state.instance, profiles[i], &state.menu_profiles[i]));
    XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
    action_info.countSubactionPaths = 2;
    action_info.subactionPaths = state.menu_hand_paths.data();
    action_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
    static_cast<void>(strcpy_s(action_info.actionName, "cheeky_menu_aim"));
    static_cast<void>(strcpy_s(action_info.localizedActionName, "Cheeky Menu Aim"));
    if (XR_FAILED(dispatch.create_action(state.menu_action_set, &action_info, &state.menu_aim_action))) return false;
    action_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    static_cast<void>(strcpy_s(action_info.actionName, "cheeky_menu_select"));
    static_cast<void>(strcpy_s(action_info.localizedActionName, "Cheeky Menu Select"));
    return XR_SUCCEEDED(dispatch.create_action(state.menu_action_set, &action_info, &state.menu_click_action));
}

void ensure_menu_bindings(InstanceState& state) {
    if (!state.menu_graphics_enabled || !state.dispatch.suggest_bindings ||
        state.menu_action_set == XR_NULL_HANDLE) return;
    for (std::size_t i{}; i < state.menu_profiles.size(); ++i) {
        if (state.menu_bindings_submitted[i] || state.menu_profiles[i] == XR_NULL_PATH) continue;
        // Unknown extension chains cannot be retained by pointer or replayed
        // without their semantics. Preserve the host suggestion in that case.
        if (state.host_menu_profile_has_next[i]) continue;
        auto bindings = state.host_menu_profile_bindings[i];
        const auto menu = menu_bindings_for_profile(state, i);
        if (menu.empty()) continue;
        for (const auto& binding : menu) {
            if (std::none_of(bindings.begin(), bindings.end(), [&](const auto& existing) {
                return existing.action == binding.action && existing.binding == binding.binding;
            })) bindings.push_back(binding);
        }
        const XrInteractionProfileSuggestedBinding suggestion{
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr,
            state.menu_profiles[i], static_cast<std::uint32_t>(bindings.size()), bindings.data()};
        if (XR_SUCCEEDED(state.dispatch.suggest_bindings(state.instance, &suggestion)))
            state.menu_bindings_submitted[i] = true;
    }
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
    instance.binding_result = result;
    if (XR_SUCCEEDED(result)) instance.gaze_binding_submitted = true;
    return result;
}

// Cache inspected modules; proxy filenames alone must never identify RealVR.
bool realvr_present() noexcept {
    static std::array<HMODULE, 4> inspected{};
    static bool detected{};
    if (detected) return true;
    const wchar_t* names[]{L"dxgi2.dll", L"dxgi.dll", L"RealVR64.dll", L"ReShade64.dll"};
    for (unsigned i = 0; i < inspected.size(); ++i) {
        const auto module = GetModuleHandleW(names[i]);
        if (!module || inspected[i] == module) continue;
        inspected[i] = module;
        if (cheeky::foveated_dlss::is_realvr_runtime(module)) detected = true;
    }
    return detected;
}

bool il2_host() noexcept {
    // IL-2's native OpenXR path can render without creating any input actions.
    // Match only its process basename, never an application/runtime substring.
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) return false;
    const auto slash = std::wcsrchr(path, L'\\');
    return _wcsicmp(slash ? slash + 1 : path, L"IL2Series.exe") == 0;
}

// Defer action spaces until attachment for runtimes that reject earlier
// creation. Both attachment routes use this compatibility ordering.
void create_post_attach_spaces_locked(
    InstanceState& instance, SessionState& state, bool gaze_attached, bool menu_attached
) noexcept {
    const auto& d = instance.dispatch;
    if (d.create_action_space == nullptr) return;
    if (gaze_attached && state.gaze_space == XR_NULL_HANDLE && state.system_supported &&
        instance.gaze_action != XR_NULL_HANDLE) {
        XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        space_info.action = instance.gaze_action;
        space_info.poseInActionSpace.orientation.w = 1.0F;
        state.input.space_result = d.create_action_space(
            state.session, &space_info, &state.gaze_space);
    }
    if (!menu_attached || instance.menu_aim_action == XR_NULL_HANDLE) return;
    for (std::size_t hand{}; hand < state.menu_aim_spaces.size(); ++hand) {
        if (state.menu_aim_spaces[hand] != XR_NULL_HANDLE) continue;
        XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        space_info.action = instance.menu_aim_action;
        space_info.subactionPath = instance.menu_hand_paths[hand];
        space_info.poseInActionSpace.orientation.w = 1.F;
        static_cast<void>(d.create_action_space(
            state.session, &space_info, &state.menu_aim_spaces[hand]));
    }
}

// Optional startup grace period, not proof that the host will never use input.
// Only explicit opt-in permits the generic fallback to claim attachment.
constexpr unsigned standalone_gaze_frame_delay = 90U;

// The standalone fallback exists for hosts that drive no OpenXR input at all:
// an identified RealVR bridge (nobody owns its menu and gaze actions) or an
// ordinary application that presents stereo frames through this layer yet
// has explicitly opted in and has not created, attached or synchronized actions.
// Hosts that initialize input after fallback attachment require a restart.
[[nodiscard]] bool standalone_gaze_allowed_locked(
    const InstanceState& instance, const SessionState& state
) noexcept {
    if (state.input.realvr_detected || instance.il2_input_compatibility) return true;
    if (instance.host_action_sets_created || state.input.host_attach_calls ||
        state.input.host_sync_calls) return false;
    return independent_gaze_option.load(std::memory_order_acquire) == 1 &&
        state.rendered_frames >= standalone_gaze_frame_delay;
}

// Called under state_mutex before reading gaze, once per distinct display time.
// Once a host has synchronized input, it owns synchronization for that session:
// an extra gaze-only sync could deactivate controllers or consume input changes.
void poll_independent_gaze_locked(InstanceState& instance, SessionState& state, XrTime time) {
    state.input.realvr_detected = realvr_present();
    if (!state.running || state.state != XR_SESSION_STATE_FOCUSED ||
        state.input.host_sync_calls || time <= 0) return;
    if (!standalone_gaze_allowed_locked(instance, state)) return;
    // Gaze capability must not require an existing action space: a strict
    // runtime rejects xrCreateActionSpace before attach, so the space can only
    // be created after the standalone attachment below.
    const bool gaze_capable = state.system_supported && instance.gaze_action != XR_NULL_HANDLE;
    // Preserve controller bindings for the existing RealVR/IL-2 paths only.
    // The new generic opt-in attaches gaze, never controller actions.
    const bool menu_available = (state.input.realvr_detected || instance.il2_input_compatibility) &&
        instance.menu_graphics_enabled && instance.menu_action_set != XR_NULL_HANDLE &&
        instance.menu_aim_action != XR_NULL_HANDLE && instance.menu_click_action != XR_NULL_HANDLE;
    if (!gaze_capable && !menu_available) return;
    const auto& d = instance.dispatch;
    if (!state.action_attached) {
        // Attachment is irreversible for this session. Leave hosts that created
        // actions in control, and never repeatedly attach after a runtime failure.
        if (instance.host_action_sets_created || state.input.host_attach_calls ||
            state.fallback_setup_attempted || !d.attach_action_sets) return;
        state.fallback_setup_attempted = true;
        // An unanswerable binding must not produce a useless attachment.
        if (gaze_capable) static_cast<void>(ensure_gaze_binding_locked(instance));
        if (menu_available) ensure_menu_bindings(instance);
        std::array<XrActionSet, 2> sets{};
        unsigned count{};
        if (gaze_capable && instance.gaze_binding_submitted) sets[count++] = instance.action_set;
        if (menu_available) sets[count++] = instance.menu_action_set;
        if (!count) return;
        XrSessionActionSetsAttachInfo info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        info.countActionSets = count;
        info.actionSets = sets.data();
        ++state.input.fallback_attach_calls;
        state.input.attach_result = d.attach_action_sets(state.session, &info);
        state.action_attached = XR_SUCCEEDED(state.input.attach_result);
        if (state.action_attached) create_post_attach_spaces_locked(instance, state,
            gaze_capable && instance.gaze_binding_submitted, menu_available);
        if (menu_available) report_menu_diagnostic(state.action_attached ?
            "OpenXR menu: compatibility controller actions attached" : "OpenXR menu: compatibility controller action attachment failed");
    }
    // The attachment above may have created the space; recompute readiness. With
    // no usable set (failed binding, attach or space) there is nothing to sync.
    const bool gaze_ready = gaze_capable && state.gaze_space != XR_NULL_HANDLE &&
        instance.gaze_binding_submitted;
    if (!state.action_attached || !d.sync_actions || state.last_fallback_sync_time == time) return;
    state.last_fallback_sync_time = time;
    std::array<XrActiveActionSet, 2> active{};
    unsigned count{};
    if (gaze_ready) active[count++] = {instance.action_set, XR_NULL_PATH};
    if (menu_available) active[count++] = {instance.menu_action_set, XR_NULL_PATH};
    if (!count) return;
    XrActionsSyncInfo info{XR_TYPE_ACTIONS_SYNC_INFO};
    info.countActiveActionSets = count;
    info.activeActionSets = active.data();
    ++state.input.fallback_sync_calls;
    state.input.sync_result = d.sync_actions(state.session, &info);
    if (state.input.sync_result != XR_SUCCESS) {
        state.action_active = false;
        state.gaze_valid = false;
    }
}

XrQuaternionf multiply_rotation(const XrQuaternionf& a, const XrQuaternionf& b) noexcept {
    return {a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
}
bool normalize_rotation(XrQuaternionf& q) noexcept {
    const float length = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (!std::isfinite(length) || length < 0.0001F) return false;
    q = {q.x/length, q.y/length, q.z/length, q.w/length};
    return true;
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
CheekyOpenXR_SetIndependentGaze(const std::uint32_t enabled) {
    int unset = -1;
    independent_gaze_option.compare_exchange_strong(unset, enabled ? 1 : 0,
        std::memory_order_acq_rel);
}

extern "C" __declspec(dllexport) void __cdecl
CheekyOpenXR_SetSimulationPattern(const std::uint32_t pattern) {
    simulation_pattern.store((std::min)(pattern, 5U), std::memory_order_release);
}

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

extern "C" __declspec(dllexport) std::uint32_t __cdecl
CheekyOpenXR_GetGazeInputDiagnostics(std::uint32_t version, void* output, std::uint32_t size) {
    if (version != CHEEKY_GAZE_INPUT_DIAGNOSTICS_VERSION || !output ||
        size < sizeof(CheekyGazeInputDiagnosticsV1)) return 0;
    for (;;) {
        const auto index = active_snapshot_slot.load(std::memory_order_acquire);
        auto& slot = snapshot_slots[index];
        slot.readers.fetch_add(1U, std::memory_order_acquire);
        if (index != active_snapshot_slot.load(std::memory_order_acquire)) {
            slot.readers.fetch_sub(1U, std::memory_order_release);
            continue;
        }
        std::memcpy(output, &slot.input, sizeof(slot.input));
        slot.readers.fetch_sub(1U, std::memory_order_release);
        return 1;
    }
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrCreateActionSet(
    XrInstance instance, const XrActionSetCreateInfo* info, XrActionSet* action_set) {
    std::lock_guard lock(state_mutex);
    const auto it = instances.find(instance);
    if (it == instances.end()) return XR_ERROR_HANDLE_INVALID;
    auto& state = it->second;
    if (!state.dispatch.create_action_set) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = state.dispatch.create_action_set(instance, info, action_set);
    log_startup("input-v2 create_action_set name=%s priority=%u result=%d set=%p\n",
        info ? info->actionSetName : "null", info ? info->priority : 0, result,
        XR_SUCCEEDED(result) && action_set ? *action_set : XR_NULL_HANDLE);
    if (XR_SUCCEEDED(result)) state.host_action_sets_created = true;
    return result;
}

// Forward declarations for entry points returned by the layer GIPA.
extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrGetActionStateBoolean(
    XrSession, const XrActionStateGetInfo*, XrActionStateBoolean*);
extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrGetActionStateFloat(
    XrSession, const XrActionStateGetInfo*, XrActionStateFloat*);
extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrGetActionStateVector2f(
    XrSession, const XrActionStateGetInfo*, XrActionStateVector2f*);
extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrGetActionStatePose(
    XrSession, const XrActionStateGetInfo*, XrActionStatePose*);
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
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace*);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrCreateActionSpace(XrSession, const XrActionSpaceCreateInfo*, XrSpace*);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrDestroySpace(XrSpace);
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
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrWaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrBeginFrame(XrSession session, const XrFrameBeginInfo* info);
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrEndFrame(
    XrSession, const XrFrameEndInfo*
);
}

namespace {

#include "quad_capture.inl"

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
    CHEEKY_INTERCEPT("xrEnumerateViewConfigurations", cheeky_xrEnumerateViewConfigurations)
    CHEEKY_INTERCEPT("xrEnumerateViewConfigurationViews", cheeky_xrEnumerateViewConfigurationViews)
    CHEEKY_INTERCEPT("xrGetViewConfigurationProperties", cheeky_xrGetViewConfigurationProperties)
    CHEEKY_INTERCEPT("xrEnumerateEnvironmentBlendModes", cheeky_xrEnumerateEnvironmentBlendModes)
    CHEEKY_INTERCEPT("xrGetVisibilityMaskKHR", cheeky_xrGetVisibilityMaskKHR)
    CHEEKY_INTERCEPT("xrCreateSession", cheeky_xrCreateSession)
    CHEEKY_INTERCEPT("xrDestroySession", cheeky_xrDestroySession)
    CHEEKY_INTERCEPT("xrCreateReferenceSpace", cheeky_xrCreateReferenceSpace)
    CHEEKY_INTERCEPT("xrCreateActionSpace", cheeky_xrCreateActionSpace)
    CHEEKY_INTERCEPT("xrDestroySpace", cheeky_xrDestroySpace)
    CHEEKY_INTERCEPT("xrPollEvent", cheeky_xrPollEvent)
    CHEEKY_INTERCEPT("xrCreateActionSet", cheeky_xrCreateActionSet)
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
    CHEEKY_INTERCEPT("xrGetActionStatePose", cheeky_xrGetActionStatePose)
    CHEEKY_INTERCEPT("xrGetActionStateBoolean", cheeky_xrGetActionStateBoolean)
    CHEEKY_INTERCEPT("xrGetActionStateFloat", cheeky_xrGetActionStateFloat)
    CHEEKY_INTERCEPT("xrGetActionStateVector2f", cheeky_xrGetActionStateVector2f)
    CHEEKY_INTERCEPT("xrLocateViews", cheeky_xrLocateViews)
    CHEEKY_INTERCEPT("xrLocateSpaces", cheeky_xrLocateSpaces)
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
    CHEEKY_INTERCEPT("xrWaitFrame", cheeky_xrWaitFrame)
    CHEEKY_INTERCEPT("xrBeginFrame", cheeky_xrBeginFrame)
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
    const bool capability_query=name&&(!std::strcmp(name,"xrEnumerateInstanceExtensionProperties")||
        !std::strcmp(name,"xrEnumerateViewConfigurations")||!std::strcmp(name,"xrEnumerateViewConfigurationViews")||
        !std::strcmp(name,"xrGetViewConfigurationProperties")||!std::strcmp(name,"xrEnumerateEnvironmentBlendModes")||!std::strcmp(name,"xrBeginSession"));
    if (is_intercepted_name(name, *function)) {
        if(capability_query)log_startup("QUAD-CAPTURE proc_resolve instance=%p name=%s route=layer result=0\n",instance,name);
        return XR_SUCCESS;
    }

    std::lock_guard lock(state_mutex);
    const auto iterator = instances.find(instance);
    if (iterator == instances.end() ||
        iterator->second.dispatch.get_instance_proc_addr == nullptr) {
        if(capability_query)log_startup("QUAD-CAPTURE proc_resolve instance=%p name=%s route=none result=%d\n",instance,name,int(XR_ERROR_HANDLE_INVALID));
        return XR_ERROR_HANDLE_INVALID;
    }
    const auto result=iterator->second.dispatch.get_instance_proc_addr(
        instance, name, function
    );
    if(capability_query)log_startup("QUAD-CAPTURE proc_resolve instance=%p name=%s route=downstream result=%d\n",instance,name,int(result));
    return result;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrCreateApiLayerInstance(
    const XrInstanceCreateInfo* const info,
    const XrApiLayerCreateInfo* const layer_info,
    XrInstance* const instance
) {
    log_startup("instance_create begin thread=%lu\n", GetCurrentThreadId());
#ifdef CHEEKY_FG_LIFETIME_DIAGNOSTICS
    log_startup("FG-LIFETIME diagnostic build active: space lifecycle, failed EndFrame handles, FG feature dimensions\n");
#endif
    if (info == nullptr || layer_info == nullptr || instance == nullptr ||
        layer_info->nextInfo == nullptr ||
        layer_info->nextInfo->nextGetInstanceProcAddr == nullptr ||
        layer_info->nextInfo->nextCreateApiLayerInstance == nullptr) {
        log_startup("instance_create invalid_arguments result=%d\n", XR_ERROR_INITIALIZATION_FAILED);
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    const auto next_gipa = layer_info->nextInfo->nextGetInstanceProcAddr;
    const bool msfs_capture_host=msfs_quad_version_bridge_host();
    const bool prefer_quad_host=msfs_capture_host||dcs_quad_capture_host();
    bool quad_requested = info->applicationInfo.apiVersion >= XR_MAKE_VERSION(1,1,0);
    for (uint32_t i=0;i<info->enabledExtensionCount;++i) {
        log_startup("QUAD-CAPTURE requested extension=%s\n",info->enabledExtensionNames[i]);
        quad_requested |= std::strcmp(info->enabledExtensionNames[i],XR_VARJO_QUAD_VIEWS_EXTENSION_NAME)==0;
    }
    log_startup("QUAD-CAPTURE provider=fixed-d3d11-d3d12 enabled=%u api=%llu prefer_quad=%u\n",unsigned(quad_requested),
        static_cast<unsigned long long>(info->applicationInfo.apiVersion),unsigned(quad_requested&&prefer_quad_host));
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
    const auto gaze_availability = already_enabled ? ExtensionAvailability::present :
        extension_availability(next_gipa);
    bool inject_extension = !already_enabled && gaze_availability != ExtensionAvailability::absent;
    if (already_enabled) log_startup("probe extension=%s availability=application_requested\n", XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    if (gaze_availability == ExtensionAvailability::unknown) {
        log_startup("eye-gaze extension probe unanswered; trying extension at instance creation\n");
    }
    bool cylinder_enabled{};
    for (std::uint32_t i{}; i < info->enabledExtensionCount; ++i)
        cylinder_enabled |= std::strcmp(info->enabledExtensionNames[i], XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME) == 0;
    auto cylinder_availability = cylinder_enabled ? ExtensionAvailability::present :
        extension_availability(next_gipa, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    // Cyberpunk/OFXR/SteamVR: a rejected speculative creation followed by a
    // retry correlates with duplicate client state and a missing input manifest.
    // Cylinders are optional menu geometry; retain the quad fallback when the
    // downstream layer cannot confirm support. Never remove host requests.
    if (!cylinder_enabled && cylinder_availability == ExtensionAvailability::unknown &&
        std::strcmp(info->applicationInfo.applicationName, "CyberpunkVRPort") == 0) {
        cylinder_availability = ExtensionAvailability::absent;
        log_startup("Cyberpunk compatibility: unconfirmed optional cylinder skipped; quad menu retained\n");
    }
    bool inject_cylinder = !cylinder_enabled && cylinder_availability != ExtensionAvailability::absent;
    bool time_enabled{},refresh_enabled{};
    for(std::uint32_t i=0;i<info->enabledExtensionCount;++i){
        time_enabled|=std::strcmp(info->enabledExtensionNames[i],XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME)==0;
        refresh_enabled|=std::strcmp(info->enabledExtensionNames[i],XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)==0;
    }
    // Optional telemetry only. Never speculate or add a failed instance retry
    // for clock conversion; an unsupported runtime still gets submit timings.
    const bool inject_time=!time_enabled&&extension_availability(next_gipa,
        XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME)==ExtensionAvailability::present;
    const bool inject_refresh=!refresh_enabled&&extension_availability(next_gipa,
        XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)==ExtensionAvailability::present;
    if (cylinder_enabled) log_startup("probe extension=%s availability=application_requested\n", XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    if (cylinder_availability == ExtensionAvailability::unknown) {
        log_startup("cylinder extension probe unanswered; trying extension at instance creation\n");
    }
    // Try subsets of only speculative requests, preferring gaze over cylinders:
    // both, gaze only, cylinder only, neither. Confirmed/application extensions
    // remain requested on every attempt. Other errors stop immediately.
    constexpr int gaze_bit = 2, cylinder_bit = 1;
    const int unknown = (gaze_availability == ExtensionAvailability::unknown ? gaze_bit : 0) |
        (cylinder_availability == ExtensionAvailability::unknown ? cylinder_bit : 0);
    XrResult result = XR_ERROR_EXTENSION_NOT_PRESENT;
    unsigned attempt{};
    const bool allow_msfs_version_bridge=msfs_capture_host&&
        XR_VERSION_MAJOR(info->applicationInfo.apiVersion)==1&&XR_VERSION_MINOR(info->applicationInfo.apiVersion)==1;
    bool msfs_version_bridge{};
    XrVersion downstream_api=info->applicationInfo.apiVersion;
    std::vector<const char*> promoted_extensions;
    for (int selection = unknown; selection >= 0; --selection) {
        if ((selection & ~unknown) != 0) continue;
        inject_extension = !already_enabled && (gaze_availability == ExtensionAvailability::present ||
            (selection & gaze_bit) != 0);
        inject_cylinder = !cylinder_enabled && (cylinder_availability == ExtensionAvailability::present ||
            (selection & cylinder_bit) != 0);
        std::vector<const char*> extensions;
        if (info->enabledExtensionCount != 0U) {
            extensions.assign(info->enabledExtensionNames, info->enabledExtensionNames + info->enabledExtensionCount);
        }
        // This layer implements fixed quad views over the runtime's stereo
        // configuration. Its extension must not be requested of that runtime.
        std::erase_if(extensions,[](const char* name){return std::strcmp(name,XR_VARJO_QUAD_VIEWS_EXTENSION_NAME)==0;});
        if (inject_extension) extensions.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
        if (inject_cylinder) extensions.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
        if(inject_time)extensions.push_back(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
        if(inject_refresh)extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        extensions.insert(extensions.end(),promoted_extensions.begin(),promoted_extensions.end());
        XrInstanceCreateInfo forwarded_info = *info;
        forwarded_info.applicationInfo.apiVersion = downstream_api;
        forwarded_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        forwarded_info.enabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
        XrApiLayerCreateInfo next_layer_info = *layer_info;
        next_layer_info.nextInfo = layer_info->nextInfo->next;
        ++attempt;
        log_startup("instance_create attempt=%u gaze_requested=%u cylinder_requested=%u reason=%s\n",
            attempt, unsigned(already_enabled || inject_extension), unsigned(cylinder_enabled || inject_cylinder),
            attempt == 1 ? "initial" : "extension_not_present");
        result = next_create(&forwarded_info, &next_layer_info, instance);
        log_startup("instance_create attempt=%u result=%d\n", attempt, result);
        if(result==XR_ERROR_API_VERSION_UNSUPPORTED&&allow_msfs_version_bridge&&!msfs_version_bridge){
            msfs_version_bridge=true;
            downstream_api=XR_MAKE_VERSION(1,0,XR_VERSION_PATCH(info->applicationInfo.apiVersion));
            // Enable available equivalents of promoted functionality without
            // changing any extensions explicitly requested by the application.
            for(const char* promoted:{XR_EXT_LOCAL_FLOOR_EXTENSION_NAME,XR_EXT_PALM_POSE_EXTENSION_NAME}){
                const bool requested=std::any_of(extensions.begin(),extensions.end(),[promoted](const char* name){return std::strcmp(name,promoted)==0;});
                if(!requested&&extension_availability(next_gipa,promoted)==ExtensionAvailability::present){
                    extensions.push_back(promoted);promoted_extensions.push_back(promoted);
                }
            }
            forwarded_info.applicationInfo.apiVersion=downstream_api;
            forwarded_info.enabledExtensionCount=static_cast<uint32_t>(extensions.size());
            forwarded_info.enabledExtensionNames=extensions.empty()?nullptr:extensions.data();
            ++attempt;
            log_startup("QUAD-CAPTURE version_bridge app_api=1.1.%u downstream_api=1.0.%u quad_provider=enabled reason=runtime_api_version_unsupported\n",
                XR_VERSION_PATCH(info->applicationInfo.apiVersion),XR_VERSION_PATCH(downstream_api));
            result=next_create(&forwarded_info,&next_layer_info,instance);
            log_startup("instance_create attempt=%u result=%d version_bridge=1\n",attempt,result);
        }
        if (result != XR_ERROR_EXTENSION_NOT_PRESENT) break;
    }
    log_startup("instance_create end result=%d gaze_enabled=%u cylinder_enabled=%u\n", result,
        unsigned(XR_SUCCEEDED(result) && (already_enabled || inject_extension)),
        unsigned(XR_SUCCEEDED(result) && (cylinder_enabled || inject_cylinder)));
    if (XR_FAILED(result)) return result;

    InstanceState state{};
    state.instance = *instance;
    state.quad_capture_enabled = quad_requested;
    state.quad_capture_prefer_quad = quad_requested&&prefer_quad_host;
    state.msfs_version_bridge = msfs_version_bridge;
    state.il2_input_compatibility = il2_host();
    if (state.il2_input_compatibility) {
        log_startup("input_compatibility host=IL2Series.exe policy=guarded_independent_input; host actions reserve attachment, host sync disables fallback\n");
    }
    state.extension_enabled = already_enabled || inject_extension;
    state.cylinder_enabled = cylinder_enabled || inject_cylinder;
    populate_dispatch(state.dispatch, *instance, next_gipa);
    if(!time_enabled&&!inject_time)state.dispatch.time_to_qpc=nullptr;
    if(!refresh_enabled&&!inject_refresh)state.dispatch.get_refresh_rate=nullptr;
    log_startup("FG display clock conversion=%s\n",state.dispatch.time_to_qpc?"available (predicted target only)":"unavailable; GPU-ready submission metric retained");
    if (state.dispatch.get_instance_properties != nullptr) {
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(state.dispatch.get_instance_properties(
                *instance, &properties
            ))) {
            state.runtime_name = properties.runtimeName;
        }
    }
    static_cast<void>(create_gaze_action(state));
    static_cast<void>(create_menu_actions(state));

    {
        std::lock_guard lock(state_mutex);
        instances.emplace(*instance, std::move(state));
        publish_snapshot_locked(nullptr);
    }
    return result;
}

std::shared_ptr<void> retain_frame_space(XrSession session,XrSpace source,XrSpace& target) {
    target=XR_NULL_HANDLE;std::shared_ptr<FrameSpaceRecord> record;
    { std::lock_guard lock(state_mutex);const auto found=frame_spaces.find(source);
      if(found==frame_spaces.end()) {
          // This layer-owned menu space is destroyed only after its presenter
          // joins. It already has the required session lifetime.
          const auto owner=sessions.find(session);
          if(owner!=sessions.end()&&source&&source==owner->second.calibration_local_space){
              static const auto session_owned=std::make_shared<int>(0);target=source;return session_owned;
          }
          return {};
      }
      record=found->second;if(record->session!=session||!record->cloneable)return {};
      if(auto retained=record->retained.lock()){target=retained->handle;return retained;} }
    auto retained=std::make_shared<FrameSpace>();retained->destroy=record->destroy;
    const auto result=record->reference_create
        ?record->reference_create(session,&record->reference,&retained->handle)
        :record->action_create(session,&record->action,&retained->handle);
    if(XR_FAILED(result)||!retained->handle)return {};
    { std::lock_guard lock(state_mutex);const auto found=frame_spaces.find(source);
      if(found==frame_spaces.end()||found->second!=record)return {};
      if(auto previous=record->retained.lock()){target=previous->handle;return previous;}
      record->retained=retained;target=retained->handle; }
    return retained;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrCreateReferenceSpace(
    XrSession session, const XrReferenceSpaceCreateInfo* info, XrSpace* space) {
    Dispatch d{}; XrInstance instance{};
    { std::lock_guard lock(state_mutex); const auto* owner=find_instance_for_session_locked(session);
      if(!owner)return XR_ERROR_HANDLE_INVALID; d=owner->dispatch;instance=owner->instance; }
    if(!d.create_reference_space)return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result=d.create_reference_space(session,info,space);
    if(XR_SUCCEEDED(result)&&space&&info){
        auto record=std::make_shared<FrameSpaceRecord>();record->session=session;record->instance=instance;
        record->destroy=d.destroy_space;record->reference_create=d.create_reference_space;
        record->reference=*info;record->reference.next=nullptr;record->cloneable=!info->next&&d.destroy_space;
        std::lock_guard lock(state_mutex);frame_spaces[*space]=std::move(record);
    }
#ifdef CHEEKY_FG_LIFETIME_DIAGNOSTICS
    log_startup("FG-LIFETIME create_reference_space session=%p space=%p type=%d result=%d\n",
        session,XR_SUCCEEDED(result)&&space?*space:XR_NULL_HANDLE,info?int(info->referenceSpaceType):-1,int(result));
#endif
    return result;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrCreateActionSpace(
    XrSession session, const XrActionSpaceCreateInfo* info, XrSpace* space) {
    Dispatch d{}; XrInstance instance{};
    { std::lock_guard lock(state_mutex); const auto* owner=find_instance_for_session_locked(session);
      if(!owner)return XR_ERROR_HANDLE_INVALID; d=owner->dispatch;instance=owner->instance; }
    if(!d.create_action_space)return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result=d.create_action_space(session,info,space);
    if(XR_SUCCEEDED(result)&&space&&info){
        auto record=std::make_shared<FrameSpaceRecord>();record->session=session;record->instance=instance;
        record->destroy=d.destroy_space;record->action_create=d.create_action_space;
        record->action=*info;record->action.next=nullptr;record->cloneable=!info->next&&d.destroy_space;
        std::lock_guard lock(state_mutex);frame_spaces[*space]=std::move(record);
    }
#ifdef CHEEKY_FG_LIFETIME_DIAGNOSTICS
    log_startup("FG-LIFETIME create_action_space session=%p space=%p result=%d\n",
        session,XR_SUCCEEDED(result)&&space?*space:XR_NULL_HANDLE,int(result));
#endif
    return result;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrDestroySpace(XrSpace space) {
    PFN_xrDestroySpace destroy{};XrSession session{};
    { std::lock_guard lock(state_mutex);const auto found=frame_spaces.find(space);
      if(found!=frame_spaces.end()){destroy=found->second->destroy;session=found->second->session;}
      // Extension-created spaces may not pass either create hook. Preserve
      // downstream dispatch; unsupported space creation stays on the native path.
      else for(const auto& entry:instances)if(entry.second.dispatch.destroy_space){destroy=entry.second.dispatch.destroy_space;break;} }
    if(!destroy)return XR_ERROR_FUNCTION_UNSUPPORTED;
#ifdef CHEEKY_FG_LIFETIME_DIAGNOSTICS
    log_startup("FG-LIFETIME destroy_space ENTER session=%p space=%p tracked=%u\n",session,space,unsigned(session!=XR_NULL_HANDLE));
#endif
    const auto result=destroy(space);
    if(XR_SUCCEEDED(result)){std::lock_guard lock(state_mutex);frame_spaces.erase(space);}
#ifdef CHEEKY_FG_LIFETIME_DIAGNOSTICS
    log_startup("FG-LIFETIME destroy_space RETURN space=%p result=%d\n",space,int(result));
#endif
    return result;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrDestroyInstance(
    const XrInstance instance
) {
    std::lock_guard menu_lock(menu_mutex);
    std::vector<std::shared_ptr<cheeky::fg::Presenter>> retired_presenters;
    Dispatch dispatch{};
    XrAction action{XR_NULL_HANDLE};
    XrActionSet action_set{XR_NULL_HANDLE};
    XrAction menu_aim_action{XR_NULL_HANDLE};
    XrAction menu_click_action{XR_NULL_HANDLE};
    XrActionSet menu_action_set{XR_NULL_HANDLE};
    {
        std::lock_guard lock(state_mutex);
        const auto iterator = instances.find(instance);
        if (iterator == instances.end()) return XR_ERROR_HANDLE_INVALID;
        dispatch = iterator->second.dispatch;
        action = iterator->second.gaze_action;
        action_set = iterator->second.action_set;
        menu_aim_action = iterator->second.menu_aim_action;
        menu_click_action = iterator->second.menu_click_action;
        menu_action_set = iterator->second.menu_action_set;
        for (auto session_it = sessions.begin(); session_it != sessions.end();) {
            if (session_it->second.instance == instance) {
                destroy_menu(session_it->second.menu, dispatch);
                destroy_menu(session_it->second.cached_menu, dispatch);
                destroy_menu(session_it->second.cursor_menu, dispatch);
                session_it->second.calibration.destroy(cheeky::openxr_calibration::bridge());
                if(session_it->second.fg)retired_presenters.push_back(std::move(session_it->second.fg));
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
        std::erase_if(frame_spaces,[&](const auto& entry){return entry.second->instance==instance;});
        instances.erase(iterator);
        publish_snapshot_locked(nullptr);
    }
    for(auto& presenter:retired_presenters)presenter->invalidate_temporal();
    retired_presenters.clear();
    if (action != XR_NULL_HANDLE && dispatch.destroy_action != nullptr) {
        static_cast<void>(dispatch.destroy_action(action));
    }
    if (action_set != XR_NULL_HANDLE && dispatch.destroy_action_set != nullptr) {
        static_cast<void>(dispatch.destroy_action_set(action_set));
    }
    if (menu_aim_action != XR_NULL_HANDLE && dispatch.destroy_action)
        static_cast<void>(dispatch.destroy_action(menu_aim_action));
    if (menu_click_action != XR_NULL_HANDLE && dispatch.destroy_action)
        static_cast<void>(dispatch.destroy_action(menu_click_action));
    if (menu_action_set != XR_NULL_HANDLE && dispatch.destroy_action_set)
        static_cast<void>(dispatch.destroy_action_set(menu_action_set));
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
    const auto result = instance_state->dispatch.create_session(instance, info, session);
    if (XR_FAILED(result)) return result;

    SessionState session_state{};
    session_state.session = *session;
    session_state.instance = instance;
    session_state.system_id = info->systemId;
    session_state.menu_cylinder_enabled = instance_state->cylinder_enabled;
    if (instance_state->dispatch.get_system_properties) {
        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_SUCCEEDED(instance_state->dispatch.get_system_properties(instance, info->systemId, &properties)) &&
            properties.graphicsProperties.maxLayerCount)
            session_state.max_layers = properties.graphicsProperties.maxLayerCount;
    }
    if (instance_state->dispatch.create_reference_space) {
        XrReferenceSpaceCreateInfo local{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        local.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        local.poseInReferenceSpace.orientation.w = 1.F;
        static_cast<void>(instance_state->dispatch.create_reference_space(
            *session, &local, &session_state.calibration_local_space));
    }
    for (auto* binding = static_cast<const XrBaseInStructure*>(info->next); binding; binding = binding->next) {
        if (binding->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            session_state.graphics_api = 11;
            session_state.graphics_device = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(binding)->device;
        }
        if (binding->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) session_state.graphics_api = 100;
        if (binding->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            session_state.graphics_api = 12;
            session_state.graphics_device = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(binding)->device;
            session_state.graphics_queue = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(binding)->queue;
        }
    }
    // graphics_queue stays the game's queue: captures and the layer's own
    // game-ordered work keep using it.
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
    // Defer action spaces until attachment for runtime compatibility.
    // VDXR rejects xrCreateActionSpace before xrAttachSessionActionSets and
    // leaves the handle null, which silently disables gaze in xrLocateViews
    // for the whole session. The gaze and menu aim spaces are created by
    // create_post_attach_spaces_locked from the two attach routes instead.
    if (session_state.graphics_api == 11 || session_state.graphics_api == 12) {
        std::lock_guard lock(state_mutex);
        const auto found = instances.find(instance);
        if (found != instances.end()) found->second.menu_graphics_enabled = true;
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
    std::lock_guard menu_lock(menu_mutex);
    std::shared_ptr<cheeky::fg::Presenter> retired_fg;
    Dispatch dispatch{};
    XrSpace gaze_space{XR_NULL_HANDLE};
    XrSpace calibration_local_space{XR_NULL_HANDLE};
    std::array<XrSpace, 2> menu_aim_spaces{};
    std::array<XrSpace, 2> host_menu_spaces{};
    {
        std::lock_guard lock(state_mutex);
        const auto session_it = sessions.find(session);
        if (session_it == sessions.end()) return XR_ERROR_HANDLE_INVALID;
        const auto instance_it = instances.find(session_it->second.instance);
        if (instance_it == instances.end()) return XR_ERROR_HANDLE_INVALID;
        dispatch = instance_it->second.dispatch;
        destroy_menu(session_it->second.menu, dispatch);
        destroy_menu(session_it->second.cached_menu, dispatch);
        destroy_menu(session_it->second.cursor_menu, dispatch);
        gaze_space = session_it->second.gaze_space;
        calibration_local_space = session_it->second.calibration_local_space;
        menu_aim_spaces = session_it->second.menu_aim_spaces;
        for (unsigned hand = 0; hand < 2; ++hand) host_menu_spaces[hand] = session_it->second.host_menu_hands[hand].space;
        session_it->second.calibration.destroy(cheeky::openxr_calibration::bridge());
        retired_fg=std::move(session_it->second.fg);
        std::erase_if(frame_spaces,[&](const auto& entry){return entry.second->session==session;});
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
    if(retired_fg)retired_fg->invalidate_temporal();
    retired_fg.reset();
    age_retired_chains(session,true);
    if (gaze_space != XR_NULL_HANDLE && dispatch.destroy_space != nullptr) {
        static_cast<void>(dispatch.destroy_space(gaze_space));
    }
    if (calibration_local_space != XR_NULL_HANDLE && dispatch.destroy_space)
        static_cast<void>(dispatch.destroy_space(calibration_local_space));
    if (dispatch.destroy_space) for (const auto space : menu_aim_spaces)
        if (space != XR_NULL_HANDLE) static_cast<void>(dispatch.destroy_space(space));
    if (dispatch.destroy_space) for (const auto space : host_menu_spaces)
        if (space != XR_NULL_HANDLE) static_cast<void>(dispatch.destroy_space(space));
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
    if (result == XR_SUCCESS && event_data &&
        event_data->type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
        const auto* changed = reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(event_data);
#ifdef CHEEKY_FG_LIFETIME_DIAGNOSTICS
        log_startup("FG-LIFETIME reference_space_change session=%p type=%d time=%lld pose_valid=%u\n",
            changed->session,int(changed->referenceSpaceType),static_cast<long long>(changed->changeTime),unsigned(changed->poseValid));
#endif
        if (changed->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
            std::lock_guard menu_lock(menu_mutex);
            std::lock_guard state_lock(state_mutex);
            const auto found = sessions.find(changed->session);
            if (found != sessions.end()) found->second.menu_recenter_time = changed->changeTime;
        }
    }
    if (XR_SUCCEEDED(result) && event_data != nullptr &&
        event_data->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(
            event_data
        );
        std::lock_guard lock(state_mutex);
        const auto iterator = sessions.find(changed->session);
        if (iterator != sessions.end()) {
            iterator->second.state = changed->state;
            if (changed->state != XR_SESSION_STATE_FOCUSED) {
                iterator->second.action_active = false;
                iterator->second.gaze_valid = false;
                iterator->second.gaze_location_flags = 0;
            }
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
    std::shared_ptr<cheeky::fg::Presenter> retired_fg;
    std::shared_ptr<cheeky::quad_capture::Compositor> quad;
    XrSessionBeginInfo forwarded{};
    log_startup("QUAD-CAPTURE begin_request session=%p app_configuration=%d\n",session,info?int(info->primaryViewConfigurationType):-1);
    const auto rejected=[&](XrResult result){log_startup("QUAD-CAPTURE begin_rejected session=%p app_configuration=%d result=%d stage=provider_preflight\n",
        session,info?int(info->primaryViewConfigurationType):-1,int(result));return result;};
    {
        std::lock_guard lock(state_mutex);
        auto* instance = find_instance_for_session_locked(session);
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.begin_session;
        if(info && cheeky::quad_capture::is_quad(info->primaryViewConfigurationType)) {
            auto& owner=sessions.at(session);
            if(!instance->quad_capture_enabled)return rejected(XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);
            if(owner.graphics_api!=12&&owner.graphics_api!=11)return rejected(XR_ERROR_GRAPHICS_DEVICE_INVALID);
            quad=std::make_shared<cheeky::quad_capture::Compositor>();
            quad->session=session;
            if(owner.graphics_api==11)quad->device11=static_cast<ID3D11Device*>(owner.graphics_device);
            else {quad->device=static_cast<ID3D12Device*>(owner.graphics_device);quad->queue=static_cast<ID3D12CommandQueue*>(owner.graphics_queue);}
            const auto& d=instance->dispatch;
            quad->create=d.create_swapchain;quad->destroy=d.destroy_swapchain;quad->enumerate_images=d.enumerate_swapchain_images;
            quad->acquire=d.acquire_swapchain_image;quad->wait=d.wait_swapchain_image;quad->release=d.release_swapchain_image;
            for(auto& view:quad->recommended)view.type=XR_TYPE_VIEW_CONFIGURATION_VIEW;
            uint32_t count{};
            if(!d.enumerate_view_configuration_views)return rejected(XR_ERROR_FUNCTION_UNSUPPORTED);
            const auto result=d.enumerate_view_configuration_views(instance->instance,owner.system_id,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&count,quad->recommended.data());
            if(XR_FAILED(result))return rejected(result);
            if(count!=2)return rejected(XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);
            forwarded=*info;forwarded.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        }
    }
    if(quad&&!quad->initialize())return rejected(XR_ERROR_RUNTIME_FAILURE);
    const auto result = next == nullptr
        ? XR_ERROR_FUNCTION_UNSUPPORTED
        : next(session, quad ? &forwarded : info);
    log_startup("QUAD-CAPTURE begin session=%p app_configuration=%d runtime_configuration=%d result=%d\n",session,
        info?int(info->primaryViewConfigurationType):-1,quad?int(forwarded.primaryViewConfigurationType):info?int(info->primaryViewConfigurationType):-1,int(result));
    if (XR_SUCCEEDED(result) && info != nullptr) {
        std::lock_guard lock(state_mutex);
        const auto iterator = sessions.find(session);
        if (iterator != sessions.end()) {
            retired_fg=std::move(iterator->second.fg);
            iterator->second.fg_requested=false;
            iterator->second.fg_state=CheekyFgState::off;
            iterator->second.running = true;
            iterator->second.native_waits_pending = 0;
            iterator->second.native_frame_begun = false;
            iterator->second.last_fallback_sync_time = 0;
            iterator->second.view_configuration =
                info->primaryViewConfigurationType;
            iterator->second.quad_capture = std::move(quad);
            iterator->second.quad_capture_locates = iterator->second.quad_capture_frames = 0;
            iterator->second.quad_capture_correct_focus_fov = bool(iterator->second.quad_capture)&&dcs_quad_capture_host();
            iterator->second.quad_capture_fovs.clear();
            if(iterator->second.quad_capture)log_startup("QUAD-CAPTURE compositor graphics_api=%u focus_fov_correction=%u; D3D11 quad FG guide path available\n",
                iterator->second.graphics_api,unsigned(iterator->second.quad_capture_correct_focus_fov));
            iterator->second.unsupported_view_configuration =
                info->primaryViewConfigurationType !=
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            publish_snapshot_locked(&iterator->second);
        }
    }
    // Worker retirement can release resources through callbacks taking state_mutex.
    // A restarted session needs setup() again, including its queue bridge fence.
    if(retired_fg)retired_fg->invalidate_temporal();
    return result;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrEndSession(
    const XrSession session
) {
    PFN_xrEndSession next{};
    std::shared_ptr<cheeky::fg::Presenter> presenter;
    {
        std::lock_guard lock(state_mutex);
        auto* instance = find_instance_for_session_locked(session);
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.end_session;
        auto& session_state = sessions.at(session);
        session_state.calibration.destroy(cheeky::openxr_calibration::bridge());
        presenter=std::move(session_state.fg);
        session_state.fg_requested=false;
        session_state.fg_state=CheekyFgState::off;
        session_state.native_waits_pending=0;
        session_state.native_frame_begun=false;
        session_state.running = false;
        session_state.quad_capture_fovs.clear();
        session_state.action_active = false;
        session_state.gaze_valid = false;
        publish_snapshot_locked(&session_state);
    }
    if(presenter)presenter->invalidate_temporal();
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
    std::vector<XrActionSuggestedBinding> menu_bindings;
    std::size_t menu_profile_index{5};
    {
        std::lock_guard lock(state_mutex);
        const auto iterator = instances.find(instance);
        if (iterator == instances.end()) return XR_ERROR_HANDLE_INVALID;
        next = iterator->second.dispatch.suggest_bindings;
        gaze_action = iterator->second.gaze_action;
        gaze_path = iterator->second.gaze_path;
        gaze_profile = iterator->second.gaze_profile;
        for (std::size_t i{}; i < iterator->second.menu_profiles.size(); ++i)
            if (suggested->interactionProfile == iterator->second.menu_profiles[i]) {
                menu_bindings = menu_bindings_for_profile(iterator->second, i);
                menu_profile_index = i;
                break;
            }
    }
    if (next == nullptr) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const bool gaze_profile_match = gaze_action != XR_NULL_HANDLE &&
        suggested->interactionProfile == gaze_profile;
    if (!gaze_profile_match && menu_bindings.empty() && menu_profile_index >= 5) {
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
    if (gaze_profile_match) menu_bindings.push_back({gaze_action, gaze_path});
    for (const auto& binding : menu_bindings) {
        const auto present = std::any_of(bindings.begin(), bindings.end(), [&](const auto& existing) {
            return existing.action == binding.action && existing.binding == binding.binding;
        });
        if (!present) bindings.push_back(binding);
    }
    auto merged = *suggested;
    merged.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size());
    merged.suggestedBindings = bindings.data();
    const auto result = next(instance, &merged);
    log_startup("input-v2 suggest profile=%llu host=%u merged=%u menu_profile=%zu result=%d\n",
        static_cast<unsigned long long>(suggested->interactionProfile),
        suggested->countSuggestedBindings, merged.countSuggestedBindings, menu_profile_index, result);
    if (XR_SUCCEEDED(result)) {
        std::lock_guard lock(state_mutex);
        const auto iterator = instances.find(instance);
        if (iterator != instances.end()) {
            if (gaze_profile_match) {
                iterator->second.gaze_binding_submitted = true;
                iterator->second.binding_result = result;
            }
            if (menu_profile_index < iterator->second.menu_bindings_submitted.size()) {
                auto& host = iterator->second.host_menu_profile_bindings[menu_profile_index];
                host.clear();
                if (suggested->suggestedBindings && suggested->countSuggestedBindings)
                    host.assign(suggested->suggestedBindings,
                        suggested->suggestedBindings + suggested->countSuggestedBindings);
                iterator->second.host_menu_profile_has_next[menu_profile_index] = suggested->next != nullptr;
                iterator->second.menu_bindings_submitted[menu_profile_index] = !menu_bindings.empty();
            }
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
    XrActionSet menu_action_set{XR_NULL_HANDLE};
    {
        std::lock_guard lock(state_mutex);
        auto* instance = find_instance_for_session_locked(session);
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.attach_action_sets;
        ++sessions.at(session).input.host_attach_calls;
        layer_action_set = instance->action_set;
        menu_action_set = instance->menu_graphics_enabled ? instance->menu_action_set : XR_NULL_HANDLE;
        static_cast<void>(ensure_gaze_binding_locked(*instance));
        ensure_menu_bindings(*instance);
    }
    if (next == nullptr) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (layer_action_set == XR_NULL_HANDLE && menu_action_set == XR_NULL_HANDLE) return next(session, attach_info);

    std::vector<XrActionSet> action_sets;
    if (attach_info->countActionSets != 0U &&
        attach_info->actionSets != nullptr) {
        action_sets.assign(
            attach_info->actionSets,
            attach_info->actionSets + attach_info->countActionSets
        );
    }
    if (layer_action_set != XR_NULL_HANDLE &&
        std::find(action_sets.begin(), action_sets.end(), layer_action_set) == action_sets.end()) {
        action_sets.push_back(layer_action_set);
    }
    if (menu_action_set != XR_NULL_HANDLE &&
        std::find(action_sets.begin(), action_sets.end(), menu_action_set) == action_sets.end())
        action_sets.push_back(menu_action_set);
    auto merged = *attach_info;
    merged.countActionSets = static_cast<std::uint32_t>(action_sets.size());
    merged.actionSets = action_sets.data();
    const auto result = next(session, &merged);
    log_startup("input-v2 attach session=%p host=%u merged=%u gaze=%p menu=%p result=%d\n",
        session, attach_info->countActionSets, merged.countActionSets,
        layer_action_set, menu_action_set, result);
    {
        std::lock_guard lock(state_mutex);
        const auto iterator = sessions.find(session);
        if (iterator != sessions.end()) {
            auto& state = iterator->second;
            state.input.attach_result = result;
            if (XR_SUCCEEDED(result)) {
                state.action_attached = true;
                // Our action sets were merged into this host attachment, so the
                // gaze and menu aim spaces can now be created. Without this
                // a strict runtime leaves gaze_space null and gaze never routes
                // for hosts that drive their own input.
                auto* instance = find_instance_for_session_locked(session);
                if (instance != nullptr) create_post_attach_spaces_locked(*instance, state,
                    layer_action_set != XR_NULL_HANDLE, menu_action_set != XR_NULL_HANDLE);
            }
            publish_snapshot_locked(&state);
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
    XrActionSet menu_action_set{XR_NULL_HANDLE};
    XrAction gaze_action{XR_NULL_HANDLE};
    bool attached{};
    Dispatch diagnostic_dispatch{};
    XrInstance diagnostic_instance{XR_NULL_HANDLE};
    std::array<XrPath, 2> diagnostic_hands{};
    XrSessionState diagnostic_state{};
    bool diagnostic_due{}, diagnostic_menu{};
    {
        std::lock_guard lock(state_mutex);
        auto* instance = find_instance_for_session_locked(session);
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.sync_actions;
        get_pose = instance->dispatch.get_action_state_pose;
        action_set = instance->action_set;
        menu_action_set = instance->menu_graphics_enabled ? instance->menu_action_set : XR_NULL_HANDLE;
        gaze_action = instance->gaze_action;
        attached = sessions.at(session).action_attached;
        ++sessions.at(session).input.host_sync_calls;
        auto& tracked = sessions.at(session);
        const auto now = GetTickCount64();
        diagnostic_due = now - tracked.input_log_tick >= 2000;
        if (diagnostic_due) {
            tracked.input_log_tick = now;
            diagnostic_dispatch = instance->dispatch;
            diagnostic_instance = instance->instance;
            diagnostic_hands = instance->menu_hand_paths;
            diagnostic_state = tracked.state;
            diagnostic_menu = tracked.diagnostic_menu_visible;
        }
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
    if (attached && menu_action_set != XR_NULL_HANDLE) {
        const auto present = std::any_of(active_sets.begin(), active_sets.end(), [&](const auto& active) {
            return active.actionSet == menu_action_set;
        });
        if (!present) active_sets.push_back({menu_action_set, XR_NULL_PATH});
    }
    auto merged = *sync_info;
    merged.countActiveActionSets = static_cast<std::uint32_t>(active_sets.size());
    merged.activeActionSets = active_sets.data();
    const auto result = next(session, &merged);

    bool active{};
    if (diagnostic_due) {
        log_startup("input-v2 sync session=%p state=%d host=%u merged=%u menu_visible=%u menu_active=%u result=%d\n",
            session, diagnostic_state, sync_info->countActiveActionSets, merged.countActiveActionSets,
            unsigned(diagnostic_menu), unsigned(attached && menu_action_set != XR_NULL_HANDLE), result);
        for (unsigned hand = 0; hand < 2; ++hand) {
            XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
            const auto hr = diagnostic_dispatch.get_current_profile && diagnostic_hands[hand] != XR_NULL_PATH
                ? diagnostic_dispatch.get_current_profile(session, diagnostic_hands[hand], &profile)
                : XR_ERROR_FUNCTION_UNSUPPORTED;
            char name[XR_MAX_PATH_LENGTH]{}; uint32_t size{};
            const auto name_hr = XR_SUCCEEDED(hr) && profile.interactionProfile != XR_NULL_PATH && diagnostic_dispatch.path_to_string
                ? diagnostic_dispatch.path_to_string(diagnostic_instance, profile.interactionProfile,
                    sizeof(name), &size, name) : XR_ERROR_PATH_INVALID;
            log_startup("input-v2 profile hand=%u result=%d path=%llu name_result=%d name=%s\n",
                hand, hr, static_cast<unsigned long long>(profile.interactionProfile), name_hr, name);
        }
    }
    XrResult pose_result{static_cast<XrResult>(CHEEKY_GAZE_RESULT_NOT_CALLED)};
    if (result == XR_SUCCESS && attached && gaze_action != XR_NULL_HANDLE &&
        get_pose != nullptr) {
        XrActionStateGetInfo state_info{XR_TYPE_ACTION_STATE_GET_INFO};
        state_info.action = gaze_action;
        XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
        pose_result = get_pose(session, &state_info, &state);
        if (XR_SUCCEEDED(pose_result)) {
            active = state.isActive == XR_TRUE;
        }
    }
    {
        std::lock_guard lock(state_mutex);
        const auto iterator = sessions.find(session);
        if (iterator != sessions.end()) {
            iterator->second.input.sync_result = result;
            iterator->second.input.pose_result = pose_result;
            // Simulated samples come from LocateViews, independently of the
            // physical gaze action. Controller sync must not erase them.
            const bool preserve_simulation = result == XR_SUCCESS &&
                iterator->second.simulated && simulated_gaze_enabled.load(std::memory_order_acquire);
            if (!preserve_simulation) {
                iterator->second.action_active = active;
                if (!active) iterator->second.gaze_valid = false;
            }
            publish_snapshot_locked(&iterator->second);
        }
    }
    return result;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrGetActionStatePose(
    XrSession session, const XrActionStateGetInfo* info, XrActionStatePose* output) {
    PFN_xrGetActionStatePose next{};
    bool due{}; unsigned hand = 2;
    {
        std::lock_guard lock(state_mutex);
        const auto it = sessions.find(session);
        auto* instance = find_instance_for_session_locked(session);
        if (it == sessions.end() || !instance) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.get_action_state_pose;
        if (info) for (unsigned i = 0; i < 2; ++i)
            if (info->subactionPath == instance->menu_hand_paths[i]) hand = i;
        const auto now = GetTickCount64();
        due = now - it->second.pose_log_ticks[hand] >= 2000;
        if (due) it->second.pose_log_ticks[hand] = now;
    }
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = next(session, info, output);
    if (due) log_startup("input-v2 host_pose session=%p hand=%u action=%p result=%d active=%u\n",
        session, hand, info ? info->action : XR_NULL_HANDLE, result,
        unsigned(XR_SUCCEEDED(result) && output && output->isActive == XR_TRUE));
    return result;
}

template<class Output, class Fn>
XrResult diagnostic_input(XrSession session, const XrActionStateGetInfo* info,
    Output* output, Fn Dispatch::* member, XrStructureType type, const char* label) {
    Fn next{}; bool due{}; unsigned hand = 2;
    {
        std::lock_guard lock(state_mutex);
        auto it = sessions.find(session);
        auto* instance = find_instance_for_session_locked(session);
        if (it == sessions.end() || !instance) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.*member;
        if (info) {
            for (unsigned i = 0; i < 2; ++i)
                if (info->subactionPath == instance->menu_hand_paths[i]) hand = i;
            auto& samples = it->second.input_samples;
            const auto now = GetTickCount64();
            auto sample = std::find_if(samples.begin(), samples.end(), [&](const auto& s) {
                return s.action == info->action && s.path == info->subactionPath && s.type == type;
            });
            if (sample == samples.end()) {
                if (samples.size() < 128) { samples.push_back({info->action, info->subactionPath, type, now}); due = true; }
            } else if (now - sample->tick >= 2000) { sample->tick = now; due = true; }
        }
    }
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto result = next(session, info, output); // exactly one query; no rewriting host output
    if (due) {
        double x{}, y{}; unsigned active{}, changed{};
        if (XR_SUCCEEDED(result) && output) {
            active = output->isActive; changed = output->changedSinceLastSync;
            if constexpr (std::is_same_v<Output, XrActionStateVector2f>) {
                x = output->currentState.x; y = output->currentState.y;
            } else x = output->currentState;
        }
        log_startup("input-v3 host_%s session=%p hand=%u action=%p result=%d active=%u changed=%u x=%.4f y=%.4f\n",
            label, session, hand, info->action, result, active, changed, x, y);
    }
    return result;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrGetActionStateBoolean(
    XrSession s, const XrActionStateGetInfo* i, XrActionStateBoolean* o) {
    return diagnostic_input(s, i, o, &Dispatch::get_action_state_boolean, XR_TYPE_ACTION_STATE_BOOLEAN, "button");
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrGetActionStateFloat(
    XrSession s, const XrActionStateGetInfo* i, XrActionStateFloat* o) {
    return diagnostic_input(s, i, o, &Dispatch::get_action_state_float, XR_TYPE_ACTION_STATE_FLOAT, "trigger");
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrGetActionStateVector2f(
    XrSession s, const XrActionStateGetInfo* i, XrActionStateVector2f* o) {
    return diagnostic_input(s, i, o, &Dispatch::get_action_state_vector2f, XR_TYPE_ACTION_STATE_VECTOR2F, "stick");
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
    bool emulating_quad{};
    XrAction gaze_action{XR_NULL_HANDLE};
    XrSpace gaze_space{XR_NULL_HANDLE};
    XrSpace calibration_local_space{XR_NULL_HANDLE};
    bool action_attached{};
    {
        std::lock_guard lock(state_mutex);
        auto* instance = find_instance_for_session_locked(session);
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.locate_views;
        get_pose = instance->dispatch.get_action_state_pose;
        locate_space = instance->dispatch.locate_space;
        gaze_action = instance->gaze_action;
        auto& session_state = sessions.at(session);
        emulating_quad = session_state.quad_capture && locate_info && cheeky::quad_capture::is_quad(locate_info->viewConfigurationType);
        if (locate_info && locate_info->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
            poll_independent_gaze_locked(*instance, session_state, locate_info->displayTime);
        gaze_space = session_state.gaze_space;
        calibration_local_space = session_state.calibration_local_space;
        action_attached = session_state.running && session_state.action_attached &&
            session_state.state == XR_SESSION_STATE_FOCUSED &&
            session_state.input.sync_result == XR_SUCCESS;
    }
    if (next == nullptr) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (emulating_quad)return quad_capture_locate(session,locate_info,view_state,capacity,count,views,next);
    const auto result = next(
        session, locate_info, view_state, capacity, count, views
    );
    if (XR_FAILED(result) || locate_info == nullptr || count == nullptr ||
        views == nullptr || capacity < CHEEKY_GAZE_MAX_VIEWS ||
        *count != CHEEKY_GAZE_MAX_VIEWS) {
        return result;
    }

    // App views may be head-relative. Query a stationary space for motion,
    // using the downstream entry point without changing the application's views.
    XrViewState motion_state{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> motion_views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    std::uint32_t motion_count{};
    bool motion_valid{};
    if (calibration_local_space != XR_NULL_HANDLE) {
        XrViewLocateInfo motion_info{XR_TYPE_VIEW_LOCATE_INFO};
        motion_info.viewConfigurationType = locate_info->viewConfigurationType;
        motion_info.displayTime = locate_info->displayTime;
        motion_info.space = calibration_local_space;
        motion_valid = XR_SUCCEEDED(next(session, &motion_info, &motion_state,
            2, &motion_count, motion_views.data())) && motion_count == 2 &&
            (motion_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    }

    // The runtime's recommended optics can differ from the application's
    // submitted projection. Apply the last submitted FOV and relative eye
    // rotation to the current tracked poses; never reuse an old absolute pose.
    std::array<XrView, 2> effective_views{views[0], views[1]};
    bool submitted_projection{};
    {
        std::lock_guard lock(state_mutex);
        auto it = sessions.find(session);
        if (it != sessions.end()) {
            auto& state = it->second;
            auto& located = state.located_history[state.located_cursor++ % state.located_history.size()];
            if (cheeky::openxr_calibration::observe_pose) {
                const auto& q = motion_views[0].pose.orientation;
                const float xyzw[]{q.x, q.y, q.z, q.w};
                cheeky::openxr_calibration::observe_pose(state.generation,
                    reinterpret_cast<std::uint64_t>(calibration_local_space), locate_info->displayTime, xyzw,
                    motion_valid);
            }
            located.time = locate_info->displayTime;
            located.space = locate_info->space;
            located.poses = {views[0].pose, views[1].pose};
            located.valid = view_state && (view_state->viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
            submitted_projection = state.submitted_projection_valid &&
                locate_info->displayTime >= state.submitted_projection_time &&
                locate_info->displayTime - state.submitted_projection_time <= 250000000;
            if (submitted_projection) for (unsigned eye = 0; eye < 2; ++eye) {
                effective_views[eye].fov = state.submitted_fov[eye];
                effective_views[eye].pose.orientation = multiply_rotation(views[eye].pose.orientation,
                    state.submitted_rotation_delta[eye]);
            }
        }
    }
    cheeky::gaze_math::Pose forward_pose{};
    std::array<float, 2> forward_u{}, forward_v{};
    bool forward_valid = view_state != nullptr &&
        (view_state->viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0 &&
        cheeky::gaze_math::stereo_forward_pose(convert_pose(views[0].pose), convert_pose(views[1].pose), forward_pose);
    if (forward_valid) {
        for (unsigned i = 0; i < 2; ++i) {
            const auto& f = effective_views[i].fov;
            forward_valid &= cheeky::gaze_math::project_gaze_to_view(forward_pose,
                convert_pose(effective_views[i].pose), {f.angleLeft, f.angleRight, f.angleUp, f.angleDown},
                forward_u[i], forward_v[i]);
        }
    }

    bool action_active{};
    XrResult pose_result{static_cast<XrResult>(CHEEKY_GAZE_RESULT_NOT_CALLED)};
    XrResult locate_result{static_cast<XrResult>(CHEEKY_GAZE_RESULT_NOT_CALLED)};
    XrSpaceLocation gaze_location{XR_TYPE_SPACE_LOCATION};
    XrEyeGazeSampleTimeEXT sample_time{XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT};
    gaze_location.next = &sample_time;
    if (action_attached && gaze_action != XR_NULL_HANDLE &&
        gaze_space != XR_NULL_HANDLE && get_pose != nullptr &&
        locate_space != nullptr) {
        XrActionStateGetInfo state_info{XR_TYPE_ACTION_STATE_GET_INFO};
        state_info.action = gaze_action;
        XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
        pose_result = get_pose(session, &state_info, &state);
        if (XR_SUCCEEDED(pose_result) &&
            state.isActive == XR_TRUE) {
            action_active = true;
            locate_result = locate_space(
                gaze_space,
                locate_info->space,
                locate_info->displayTime,
                &gaze_location
            );
            if (XR_FAILED(locate_result)) gaze_location.locationFlags = 0;
        }
    }

    cheeky::gaze_math::Pose next_jump_pose{};
    bool next_jump_valid{};
    std::array<float, 2> next_jump_u{}, next_jump_v{};
    const unsigned pattern = simulation_pattern.load(std::memory_order_acquire);
    const bool simulated = simulated_gaze_enabled.load(std::memory_order_acquire);
    if (simulated && view_state != nullptr &&
        (view_state->viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0) {
        double elapsed{};
        {
            std::lock_guard lock(state_mutex);
            auto& state = sessions.at(session);
            if (!state.simulated || state.simulation_pattern != pattern) state.simulation_start = locate_info->displayTime;
            state.simulation_pattern = pattern;
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
            const auto pose = cheeky::gaze_math::simulated_gaze_pose(head, elapsed, pattern);
            next_jump_valid = pattern == 2U || pattern == 3U;
            if (next_jump_valid) next_jump_pose = cheeky::gaze_math::simulated_gaze_pose(
                head, cheeky::gaze_math::next_simulated_jump_time(elapsed, pattern), pattern);
            gaze_location.pose.orientation = {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
            gaze_location.locationFlags = cheeky::gaze_math::simulated_gaze_valid(elapsed, pattern)
                ? XR_SPACE_LOCATION_ORIENTATION_VALID_BIT : 0;
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
                effective_views[index].fov.angleLeft,
                effective_views[index].fov.angleRight,
                effective_views[index].fov.angleUp,
                effective_views[index].fov.angleDown,
            };
            if (next_jump_valid) next_jump_valid &= cheeky::gaze_math::project_gaze_to_view(
                next_jump_pose, convert_pose(effective_views[index].pose), fov, next_jump_u[index], next_jump_v[index]);
            projections_valid &= cheeky::gaze_math::project_gaze_to_view(
                gaze_pose,
                convert_pose(effective_views[index].pose),
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
            state.eye_fov_valid = !state.unsupported_view_configuration;
            state.forward_valid = forward_valid && !state.unsupported_view_configuration;
            state.forward_u = forward_u; state.forward_v = forward_v;
            state.using_submitted_projection = submitted_projection;
            for (unsigned i = 0; i < CHEEKY_GAZE_MAX_VIEWS; ++i) state.eye_fov[i] = effective_views[i].fov;
            state.next_jump_valid = next_jump_valid;
            state.next_jump_u = next_jump_u; state.next_jump_v = next_jump_v;
            state.simulated = simulated;
            state.sample_time = sample_time.time;
            state.input.pose_result = pose_result;
            state.input.locate_result = locate_result;
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
    bool quad{};
    {
        std::lock_guard lock(state_mutex);
        auto* instance = find_instance_for_session_locked(session);
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.create_swapchain;
        const auto& owner=sessions.at(session);
        // Applications can create their color swapchains before BeginSession.
        // DCS must receive sampleable images on that normal startup path too.
        quad = bool(owner.quad_capture)||(instance->quad_capture_enabled&&instance->quad_capture_prefer_quad&&
            (owner.graphics_api==11||owner.graphics_api==12));
    }
    if (next == nullptr) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrSwapchainCreateInfo forwarded{};
    if(quad&&create_info){forwarded=*create_info;
        if(forwarded.usageFlags&XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)forwarded.usageFlags|=XR_SWAPCHAIN_USAGE_SAMPLED_BIT;}
    const auto result = next(session, quad&&create_info?&forwarded:create_info, swapchain);
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
    std::shared_ptr<cheeky::fg::Presenter> fg_to_forget;
    std::shared_ptr<cheeky::quad_capture::Compositor> quad_to_invalidate;
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
            fg_to_forget=session_it->second.fg;
            quad_to_invalidate=session_it->second.quad_capture;
            if(fg_to_forget)session_it->second.fg_retire_pending=true;
            auto& calibration = session_it->second.calibration;
            for (const auto& region : calibration.history) {
                if (region.swapchain == reinterpret_cast<std::uint64_t>(swapchain)) {
                    calibration.destroy(cheeky::openxr_calibration::bridge());
                    break;
                }
            }
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
    if(quad_to_invalidate&&quad_to_invalidate->d3d11&&quad_to_invalidate->d3d11->mapping)quad_to_invalidate->d3d11->mapping->reset();
    if(fg_to_forget) {
        // OpenComposite destroys overlay swapchains from the game thread while
        // its render thread waits in xrWaitFrame (ACC loading into a map).
        // Stopping the presenter here returned XR_ERROR_SESSION_NOT_RUNNING
        // to that wait, and OpenComposite aborts on any failed wait.
        if(const auto buffered=std::dynamic_pointer_cast<cheeky::fg::BufferedPresenter>(fg_to_forget))
            buffered->forget_swapchain(swapchain);
        if(fg_to_forget->interop11)fg_to_forget->interop11->forget(swapchain);
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
    std::vector<void*> calibration_images(*count);
    if (*count != 0U && images[0].type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
        const auto* typed = reinterpret_cast<const XrSwapchainImageD3D11KHR*>(
            images
        );
        for (std::uint32_t index{}; index < *count; ++index) {
            identities[index] = canonical_resource_identity(typed[index].texture);
            calibration_images[index] = typed[index].texture;
        }
    } else if (*count != 0U &&
               images[0].type == XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) {
        const auto* typed = reinterpret_cast<const XrSwapchainImageD3D12KHR*>(
            images
        );
        for (std::uint32_t index{}; index < *count; ++index) {
            identities[index] = canonical_resource_identity(typed[index].texture);
            calibration_images[index] = typed[index].texture;
        }
    }

    if(*count!=0U && images[0].type==XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) {
        const auto* typed=reinterpret_cast<const XrSwapchainImageVulkanKHR*>(images);
        for(std::uint32_t i=0;i<*count;++i)identities[i]=reinterpret_cast<std::uint64_t>(typed[i].image);
    }
    std::lock_guard lock(state_mutex);
    const auto swapchain_it = swapchains.find(swapchain);
    if (swapchain_it != swapchains.end()) {
        swapchain_it->second.resource_identities = std::move(identities);
        swapchain_it->second.calibration_images = std::move(calibration_images);
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
            swapchain_it->second.acquired_images.emplace_back(*index, false);
            swapchain_it->second.acquired_index = swapchain_it->second.acquired_images.front().first;
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
    std::shared_ptr<cheeky::fg::Presenter> presenter;
    {
        std::lock_guard lock(state_mutex);
        const auto swapchain_it = swapchains.find(swapchain);
        if (swapchain_it == swapchains.end()) return XR_ERROR_HANDLE_INVALID;
        auto* instance = find_instance_for_session_locked(
            swapchain_it->second.session
        );
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.wait_swapchain_image;
        const auto owner=sessions.find(swapchain_it->second.session);
        if(owner!=sessions.end())presenter=owner->second.fg;
    }
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const auto wait_start=cheeky::fg::TimingClock::now();
    const auto result = next(swapchain, wait_info);
    if(presenter)presenter->note_app_image_wait(cheeky::fg::elapsed_ms(wait_start));
    if (result == XR_SUCCESS || result == XR_SESSION_LOSS_PENDING) {
        std::lock_guard lock(state_mutex);
        auto it = swapchains.find(swapchain);
        if (it != swapchains.end()) for (auto& image : it->second.acquired_images) {
            if (!image.second) { image.second = true; break; }
        }

    }
    return result;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrReleaseSwapchainImage(
    const XrSwapchain swapchain,
    const XrSwapchainImageReleaseInfo* const release_info
) {
    PFN_xrReleaseSwapchainImage next{};
    std::shared_ptr<cheeky::fg::Presenter> fg_presenter;
    cheeky::fg::AppFrameCallTimer app_timer{fg_presenter,2};
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
        auto& chain = swapchain_it->second;
        auto owner = sessions.find(session);
        if (next && owner != sessions.end() && !chain.acquired_images.empty() && chain.acquired_images.front().second &&
            acquired_index < chain.calibration_images.size() &&
            (chain.create_info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)) {
            owner->second.calibration.before_release(cheeky::openxr_calibration::bridge(),
                reinterpret_cast<std::uint64_t>(swapchain), acquired_index, chain.calibration_images[acquired_index],
                owner->second.graphics_queue, chain.create_info.width, chain.create_info.height);
        }
    }
    if (next == nullptr) return XR_ERROR_FUNCTION_UNSUPPORTED;
    // Copy the completed OpenXR color image before giving its ownership back.
    // The GPU-owned copy remains stable while UEVR renders another frame.
    ID3D12Resource* fg_source{};
    ID3D11Texture2D* fg_source11{};
    unsigned fg_width{}, fg_height{}, fg_array{};
    DXGI_FORMAT fg_format{DXGI_FORMAT_UNKNOWN};
    {
        std::lock_guard fg_lock(state_mutex);
        const auto chain = swapchains.find(swapchain);
        const auto owner = sessions.find(session);
        bool quad_capture_ready=true;
        if(owner!=sessions.end()&&owner->second.quad_capture&&owner->second.fg){
            std::lock_guard presenter_lock(owner->second.fg->mx);
            const auto buffered=std::dynamic_pointer_cast<cheeky::fg::BufferedPresenter>(owner->second.fg);
            quad_capture_ready=buffered&&buffered->started;
        }
        if (chain != swapchains.end() && owner != sessions.end() && owner->second.fg &&
            quad_capture_ready &&
            (!owner->second.quad_capture||!owner->second.native_frame_begun) &&
            // D3D12 quad captures the composed eyes at xrEndFrame instead.
            !(owner->second.quad_capture&&owner->second.graphics_api==12) &&
            (chain->second.create_info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
            has_acquired && acquired_index < chain->second.calibration_images.size()) {
            fg_presenter=owner->second.fg;
            if(owner->second.graphics_api==11) {
                fg_source11=static_cast<ID3D11Texture2D*>(chain->second.calibration_images[acquired_index]);
                if(fg_source11)fg_source11->AddRef();
            } else fg_source=static_cast<ID3D12Resource*>(chain->second.calibration_images[acquired_index]);
            if (fg_source) fg_source->AddRef();
            fg_width=chain->second.create_info.width;
            fg_height=chain->second.create_info.height;
            fg_array=chain->second.create_info.arraySize;
            fg_format=static_cast<DXGI_FORMAT>(chain->second.create_info.format);
        }
    }
    if (fg_presenter && fg_source) {
        fg_presenter->capture(swapchain,acquired_index,fg_source,fg_width,fg_height,fg_array,fg_format);
        fg_source->Release();
    }
    if (fg_presenter && fg_source11) {
        fg_presenter->capture11(swapchain,acquired_index,fg_source11,fg_width,fg_height,fg_array,fg_format);
        fg_source11->Release();
    }

    const auto submit=[&]{return next(swapchain,release_info);};
    const auto result = fg_presenter?fg_presenter->runtime_dispatch(submit):submit();
    {
        std::lock_guard lock(state_mutex);
        auto owner = sessions.find(session);
        if (owner != sessions.end()) owner->second.calibration.after_release(
            reinterpret_cast<std::uint64_t>(swapchain), acquired_index, XR_SUCCEEDED(result));
    }
    if (XR_SUCCEEDED(result) && has_acquired) {
        std::lock_guard lock(state_mutex);
        const auto swapchain_it = swapchains.find(swapchain);
        if (swapchain_it != swapchains.end()) {
            swapchain_it->second.released_index = acquired_index;
            swapchain_it->second.has_released = true;
            auto& chain = swapchain_it->second;
            if (!chain.acquired_images.empty()) chain.acquired_images.pop_front();
            chain.has_acquired = !chain.acquired_images.empty();
            if (chain.has_acquired) chain.acquired_index = chain.acquired_images.front().first;
            const auto session_it = sessions.find(session);
            if (session_it != sessions.end()) {
                update_view_resource_locked(session_it->second, swapchain);
                publish_snapshot_locked(&session_it->second);
            }
        }
    }
    return result;
}

// Caller holds state_mutex; returned resources retire outside that lock.
std::shared_ptr<cheeky::fg::Presenter> update_fg_at_frame_boundary(
    SessionState& owner, const Dispatch& d, bool requested,bool native_shader11=false) {
    const bool supported=owner.running && (owner.graphics_api==12 || owner.graphics_api==11) &&
        (owner.view_configuration==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO || owner.quad_capture);
    if(owner.quad_capture&&owner.quad_capture->d3d11&&owner.quad_capture->d3d11->fg_guides)
        owner.quad_capture->d3d11->fg_guides->enable(requested&&supported&&bool(owner.fg));
    if (!requested || !supported) {
        if(owner.fg){log_startup("DLSS-FG disabling session=%p; draining private GPU work\n",owner.session);fg_transition_trace=48;}
        owner.fg_requested=false;
        owner.fg_state=requested?CheekyFgState::waiting_for_stereo:CheekyFgState::off;
        return std::move(owner.fg);
    }
    if (!owner.fg_requested) {
        owner.fg_requested=true;
        log_startup("DLSS-FG enabling session=%p device=%p queue=%p; OpenXR color state=RENDER_TARGET\n",
            owner.session,owner.graphics_device,owner.graphics_queue);
        cheeky::fg::XrFns fn{};
        fn.time_instance=owner.instance;fn.time_to_qpc=d.time_to_qpc;fn.locate_space=d.locate_space;fn.retain_space=retain_frame_space;
        fn.get_refresh_rate=d.get_refresh_rate;
        fn.create=d.create_swapchain;fn.destroy=d.destroy_swapchain;
        fn.enumerate=d.enumerate_swapchain_images;fn.acquire=d.acquire_swapchain_image;
        fn.wait_image=d.wait_swapchain_image;fn.release=d.release_swapchain_image;
        fn.wait_frame=d.wait_frame;fn.begin_frame=d.begin_frame;
        // The presenter's runtime frames carry the F8 menu's published layer.
        register_presenter_end_frame(owner.session,d.end_frame);fn.end_frame=presenter_end_frame_with_menu;
        if(owner.graphics_api==11&&native_shader11){
            // Same buffered pacing as DLSS-G; native D3D11 GPU stages, no bridge.
            auto candidate=std::make_shared<cheeky::fg::BufferedPresenter>();
            candidate->quad_input=bool(owner.quad_capture);
            if(owner.quad_capture&&owner.quad_capture->d3d11)owner.quad_capture->d3d11->fg_bridge.reset();
            candidate->trace=[](const char* message) noexcept {log_startup("%s\n",message);};
            candidate->retire_chain=[session=owner.session,destroy=d.destroy_swapchain](XrSwapchain chain) noexcept {
                retire_private_chain(session,chain,destroy);};
            if(candidate->setup11(owner.session,fn,static_cast<ID3D11Device*>(owner.graphics_device),owner.generation,
                &cheeky::quad_capture::d3d11_dispatch_mutex)){
                candidate->set_native_frame_pending(owner.native_waits_pending!=0||owner.native_frame_begun);
                owner.fg=std::move(candidate);owner.fg_state=CheekyFgState::waiting_for_history;
                log_startup("Shader FG buffered presenter initialized with native D3D11 backend; no D3D12 bridge session=%p\n",owner.session);
            }else {owner.fg_requested=false;owner.fg_state=CheekyFgState::presenter_failed;
                log_startup("Shader FG native D3D11 initialization failed: %s\n",candidate->diagnostic_text());}
            if(owner.quad_capture&&owner.quad_capture->d3d11&&owner.quad_capture->d3d11->fg_guides)
                owner.quad_capture->d3d11->fg_guides->enable(bool(owner.fg));
            return {};
        }
        auto candidate=std::make_shared<cheeky::fg::BufferedPresenter>();
        // D3D12 quad presents the composed eyes as stereo; only the D3D11
        // bridge sends raw quad planes.
        candidate->quad_input=bool(owner.quad_capture)&&owner.graphics_api==11;
        candidate->trace=[](const char* message) noexcept {log_startup("%s\n",message);};
        auto* device12=owner.graphics_api==12?static_cast<ID3D12Device*>(owner.graphics_device):nullptr;
        auto* queue12=owner.graphics_api==12?static_cast<ID3D12CommandQueue*>(owner.graphics_queue):nullptr;
        if(owner.graphics_api==11) {
            auto bridge=std::make_shared<cheeky::fg::D3D11Bridge>();
            bridge->dispatch_mutex=&cheeky::quad_capture::d3d11_dispatch_mutex;
            if(bridge->initialize(static_cast<ID3D11Device*>(owner.graphics_device),candidate->quad_input)) {
                device12=bridge->device12.Get();queue12=bridge->queue.Get();
                candidate->interop11=std::move(bridge);
                if(owner.quad_capture&&owner.quad_capture->d3d11)owner.quad_capture->d3d11->fg_bridge=candidate->interop11;
                if(candidate->quad_input)log_startup("QUAD-FG bridge native D3D11 shared textures; deferred copies with protected copy/signal/flush; NGX/runtime dispatch serialized; device_flags=0x%x\n",static_cast<ID3D11Device*>(owner.graphics_device)->GetCreationFlags());
            }
        }
        if (candidate->setup(owner.session,fn,device12,queue12)) {
            candidate->set_native_frame_pending(owner.native_waits_pending!=0||owner.native_frame_begun);
            owner.fg=std::move(candidate);
            owner.fg_state=CheekyFgState::waiting_for_history;
            log_startup("DLSS-FG presenter initialized session=%p\n",owner.session);
        } else {
            owner.fg_requested=false;
            owner.fg_state=CheekyFgState::presenter_failed;
            log_startup("DLSS-FG presenter initialization failed session=%p\n",owner.session);
        }
    }
    if(owner.quad_capture&&owner.quad_capture->d3d11&&owner.quad_capture->d3d11->fg_guides)
        owner.quad_capture->d3d11->fg_guides->enable(bool(owner.fg));
    return {};
}

XrResult wait_frame_once(XrSession session,const XrFrameWaitInfo* info,XrFrameState* state,bool retry);
extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrWaitFrame(
    const XrSession session, const XrFrameWaitInfo* const info,
    XrFrameState* const state) {
    return wait_frame_once(session,info,state,true);
}
XrResult wait_frame_once(const XrSession session,const XrFrameWaitInfo* const info,
    XrFrameState* const state,const bool retry) {
    PFN_xrWaitFrame downstream{};
    PFN_xrBeginFrame downstream_begin{};
    PFN_xrEndFrame downstream_end{};
    std::shared_ptr<cheeky::fg::Presenter> presenter;
    std::shared_ptr<cheeky::fg::Presenter> retired;
    bool replacing_source_presenter{};
    bool native_wait_reserved{};
    XrTime native_time_floor{}; // Last display time a retired presenter gave the application.
    // Read the runtime setting before taking the layer lock. Changes are
    // latched here, so one application frame keeps the same cadence throughout.
    CheekyFgControl control;
    const auto runtime=GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll");
    const auto get_control=runtime?reinterpret_cast<CheekyFgControlFn>(GetProcAddress(runtime,"CheekyFG_GetControl")):nullptr;
    const bool requested=get_control && get_control(&control) && control.enabled;
    CheekyFgShaderOptionsV1 shader_options;
    const auto get_shader_options=runtime?reinterpret_cast<CheekyFgShaderOptionsFn>(GetProcAddress(runtime,"CheekyFG_GetShaderOptionsV1")):nullptr;
    const bool native_shader11=get_shader_options&&get_shader_options(&shader_options)&&shader_options.enabled&&
        cheeky_fg_shader_only(shader_options.center_method,shader_options.peripheral_method,shader_options.foveated!=0);
    CheekyFgUevrModeV1 uevr_mode{};
    const auto get_uevr_mode=runtime?reinterpret_cast<CheekyFgUevrModeFn>(
        GetProcAddress(runtime,"CheekyFG_GetUevrModeV1")):nullptr;
    const bool paused_for_uevr_mode=get_uevr_mode && get_uevr_mode(&uevr_mode) &&
        uevr_mode.attached && (!uevr_mode.known || uevr_mode.mode!=0);
    CheekyFgUevrModeV2 uevr_mode_v2{};
    const auto get_uevr_mode_v2=runtime?reinterpret_cast<CheekyFgUevrModeV2Fn>(
        GetProcAddress(runtime,"CheekyFG_GetUevrModeV2")):nullptr;
    const bool mode_epoch_valid=get_uevr_mode_v2 && get_uevr_mode_v2(&uevr_mode_v2) &&
        uevr_mode_v2.attached && uevr_mode_v2.transition_epoch!=0;
    // Also gate on V2, in case V1 and V2 were read during a transition.
    const bool mode_allows_fg=!mode_epoch_valid ||
        (uevr_mode_v2.known && uevr_mode_v2.mode==0);
    const bool fg_requested_now=requested && mode_allows_fg;
    std::shared_ptr<cheeky::fg::BufferedPresenter> finishing_quad;
    {
        std::lock_guard lock(state_mutex);const auto found=sessions.find(session);
        if(found!=sessions.end()&&found->second.quad_capture&&found->second.fg){
            const auto& active=found->second.fg;std::lock_guard fg_lock(active->mx);
            if(!fg_requested_now||cheeky::fg::replace_presenter_for_configuration(requested,active->configuration,control.configuration))
                finishing_quad=std::dynamic_pointer_cast<cheeky::fg::BufferedPresenter>(active);
        }
    }
    // Keep the old owner visible to the render thread until its pipelined
    // xrEndFrame finishes. Waiting while holding state_mutex would deadlock it.
    if(finishing_quad)finishing_quad->finish_application_frame();
    static bool warned_missing_mode_epoch{};
    if(get_uevr_mode && !get_uevr_mode_v2 && !warned_missing_mode_epoch){
        warned_missing_mode_epoch=true;
        log_startup("DLSS-FG UEVR transition counter unavailable; update CheekyFoveatedDLSSRuntime.dll to enable automatic mode reactivation\n");
    }
    {
        std::lock_guard lock(state_mutex);
        const auto* instance=find_instance_for_session_locked(session);
        if (!instance) return XR_ERROR_HANDLE_INVALID;
        downstream=instance->dispatch.wait_frame;
        downstream_begin=instance->dispatch.begin_frame;downstream_end=instance->dispatch.end_frame;
        auto it=sessions.find(session);
        if (it!=sessions.end()) {
            auto& owner=it->second;
            // UEVR can switch Native -> AFW/AER/Sequential -> Native while
            // the game stops calling xrWaitFrame. Merely checking the *current*
            // mode misses that transition and leaves the old presenter alive.
            // The producer records every published mode transition in V2.
            if(state && mode_epoch_valid){
                const auto next_epoch=uevr_mode_v2.transition_epoch;
                if(owner.fg_uevr_mode_epoch==0)
                    log_startup("DLSS-FG UEVR mode epoch tracking active epoch=%llu mode=%u\n",
                        static_cast<unsigned long long>(next_epoch),uevr_mode_v2.mode);
                if(owner.fg && owner.fg_uevr_mode_epoch!=0 &&
                   owner.fg_uevr_mode_epoch!=next_epoch){
                    log_startup("DLSS-FG UEVR mode transition epoch %llu -> %llu (mode=%u): replacing presenter at xrWaitFrame; FgEnabled preserved\n",
                        static_cast<unsigned long long>(owner.fg_uevr_mode_epoch),
                        static_cast<unsigned long long>(next_epoch),uevr_mode_v2.mode);
                    retired=std::move(owner.fg);
                    owner.fg_requested=false;
                    owner.fg_state=CheekyFgState::waiting_for_history;
                    replacing_source_presenter=true;
                }
                owner.fg_uevr_mode_epoch=next_epoch;
            }
            if (state && owner.fg_retire_pending) {
                owner.fg_retire_pending=false;
                if(owner.fg){
                    log_startup("DLSS-FG application swapchain destroyed: replacing presenter at xrWaitFrame (keeping FG enabled)\n");
                    retired=std::move(owner.fg);
                    owner.fg_requested=false;
                    owner.fg_state=CheekyFgState::waiting_for_history;
                    replacing_source_presenter=true;
                }
            }
            if (state && fg_requested_now && owner.fg) {
                // A foveated-SR toggle changes both the capture layout and the
                // guide history.  An in-place temporal reset stops the threads,
                // but does NOT run BufferedPresenter::setup() again. The user
                // disabling and re-enabling FG works because it constructs a
                // completely fresh presenter. Do precisely that here, without
                // changing the FG enabled setting or introducing private UEVR
                // hooks. Other (pacing-only) changes are excluded from the hash.
                std::uint64_t active_configuration{};
                {
                    std::lock_guard fg_lock(owner.fg->mx);
                    active_configuration=owner.fg->configuration;
                }
                if (cheeky::fg::replace_presenter_for_configuration(
                        requested,active_configuration,control.configuration)) {
                    log_startup("DLSS-FG source configuration changed 0x%llx -> 0x%llx: replacing presenter at xrWaitFrame (keeping FG enabled)\n",
                        static_cast<unsigned long long>(active_configuration),
                        static_cast<unsigned long long>(control.configuration));
                    retired=std::move(owner.fg);
                    owner.fg_requested=false; // Reuse the proven manual-enable setup path.
                    owner.fg_state=CheekyFgState::waiting_for_history;
                    replacing_source_presenter=true;
                }
            }
            if (state) {
                auto disabled=update_fg_at_frame_boundary(owner,instance->dispatch,fg_requested_now,native_shader11);
                if(disabled && !retired)retired=std::move(disabled);
                if((paused_for_uevr_mode || (mode_epoch_valid && !mode_allows_fg)) && !fg_requested_now)
                    owner.fg_state=CheekyFgState::paused_rendering_mode;
            }
            if (owner.running && state) presenter=owner.fg;
            if(presenter&&owner.quad_capture&&owner.quad_capture->d3d11){
                const auto buffered=std::dynamic_pointer_cast<cheeky::fg::BufferedPresenter>(presenter);
                std::lock_guard fg_lock(presenter->mx);
                // The native D3D11 backend builds its quad guides per presented
                // frame and shows native frames until they arrive.
                if(buffered&&!buffered->started&&!buffered->native11){
                    if(!owner.quad_capture->d3d11->fg_guides->ready()){
                        presenter->state=CheekyFgState::waiting_for_guides;
                        strcpy_s(buffered->diagnostic,"Preparing four-view guides; native presentation remains active");presenter.reset();
                    }else buffered->diagnostic[0]=0;
                }
            }
            if (!presenter && downstream && state) {
                ++owner.native_waits_pending;
                native_wait_reserved=true;
            }
        }
    }
    // Never drain GPU work under state_mutex: callbacks that release captured
    // color resources also take it. Retire the old presenter fully before the
    // new presenter's first xrWaitFrame, so its GPU fences and private OpenXR
    // swapchains are gone before a new frame is submitted.
    if(retired) {
        if(const auto buffered=std::dynamic_pointer_cast<cheeky::fg::BufferedPresenter>(retired)){
            std::lock_guard fg_lock(buffered->mx);native_time_floor=buffered->app_display_time;}
        if(replacing_source_presenter)retired->reset();
        else retired->invalidate_temporal();
    }
    retired.reset();
    if (presenter) {presenter->configure(control.configuration);const auto r=presenter->wait_frame(info,state);
        if(trace_transition())log_startup("FG-TRANSITION xrWaitFrame presenter thread=%lu result=%d\n",GetCurrentThreadId(),int(r));
        // Another thread retired or reset this presenter while the application
        // waited in it. That is not a session failure: wait again against the
        // session's current presenter (or the runtime) instead.
        const auto buffered=std::dynamic_pointer_cast<cheeky::fg::BufferedPresenter>(presenter);
        if(r==XR_ERROR_SESSION_NOT_RUNNING&&retry&&buffered&&buffered->interrupted()){
            log_startup("DLSS-FG xrWaitFrame interrupted by a presenter reset on another thread; waiting again\n");
            presenter.reset();return wait_frame_once(session,info,state,false);
        }
        return r;}
    const bool traced=trace_transition();
    if(traced)log_startup("FG-TRANSITION xrWaitFrame enter native thread=%lu reserved=%u\n",GetCurrentThreadId(),unsigned(native_wait_reserved));
    auto result=downstream ? downstream(session,info,state) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if(traced)log_startup("FG-TRANSITION xrWaitFrame return native thread=%lu result=%d time=%lld floor=%lld\n",GetCurrentThreadId(),int(result),
        state?static_cast<long long>(state->predictedDisplayTime):0ll,static_cast<long long>(native_time_floor));
    // The retired presenter paced the application ahead of the runtime. A
    // native frame that is not later than its last frame is skipped by the
    // application (DCS calls xrWaitFrame again without xrBeginFrame), and the
    // runtime then blocks that wait until a BeginFrame which never comes.
    // Consume such runtime frames here, empty, until the clock passes it.
    // Do not return a stale prediction after an arbitrary number of skips:
    // DCS skips its BeginFrame, then VDXR blocks the next WaitFrame forever.
    for(unsigned skipped=0;XR_SUCCEEDED(result)&&state&&native_time_floor&&state->predictedDisplayTime<=native_time_floor&&
            downstream_begin&&downstream_end;++skipped){
        XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};result=downstream_begin(session,&begin);if(XR_FAILED(result))break;
        XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};empty.displayTime=state->predictedDisplayTime;
        empty.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;result=downstream_end(session,&empty);if(XR_FAILED(result))break;
        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};result=downstream(session,info?info:&wait,state);
        log_startup("FG native resume: skipped runtime frame before the last presented time (%u) result=%d time=%lld floor=%lld\n",
            skipped+1,int(result),static_cast<long long>(state->predictedDisplayTime),static_cast<long long>(native_time_floor));
    }
    if (native_wait_reserved && XR_FAILED(result)) {
        std::lock_guard lock(state_mutex);
        const auto owner=sessions.find(session);
        if (owner!=sessions.end()) {
            if(owner->second.native_waits_pending)--owner->second.native_waits_pending;
            if(owner->second.fg)owner->second.fg->set_native_frame_pending(
                owner->second.native_waits_pending!=0||owner->second.native_frame_begun);
        }
    }
    return result;
}

extern "C" __declspec(dllexport) bool __cdecl CheekyFG_ObserveShaderD3D11V1(const CheekyFgShaderSource11V1* input) noexcept {
    if(!input||input->abi!=1||input->size!=sizeof(*input)||!input->context)return false;
    try{
        std::shared_ptr<cheeky::fg::BufferedPresenter> presenter;
        auto source=*input;
        {std::lock_guard lock(state_mutex);
            // Session 0: the eye calibration came from OpenVR (an OpenComposite
            // game such as ACC), which has no OpenXR generation. Bind it to the
            // only running D3D11 session; with several, the owner is ambiguous.
            unsigned matches{};
            for(const auto& [handle,owner]:sessions)if(owner.running&&owner.graphics_api==11&&
                (owner.generation==input->session_generation||!input->session_generation)){
                presenter=std::dynamic_pointer_cast<cheeky::fg::BufferedPresenter>(owner.fg);
                source.session_generation=owner.generation;++matches;}
            if(matches!=1)return false;}
        return presenter&&presenter->native11&&presenter->native11->observe(source);
    }catch(...){return false;}
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetStatus(CheekyFgStatus* out) noexcept {
    if (!out || out->abi!=CHEEKY_FG_CONTROL_ABI || out->size!=sizeof(*out)) return false;
    CheekyFgStatus status;
    status.layer_present=1;status.controls_supported=1;
    std::lock_guard lock(state_mutex);
    for (const auto& [handle,owner]:sessions) {
        if ((owner.graphics_api!=12 && owner.graphics_api!=11) || !owner.running ||
            (owner.view_configuration!=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO&&!owner.quad_capture)) continue;
        status.compatible_session=1;
        status.state=owner.fg_state;
        if (!owner.fg) continue;
        std::lock_guard fg_lock(owner.fg->mx);
        status.active=1;status.state=owner.fg->state;
        if(owner.quad_capture){const auto buffered=std::dynamic_pointer_cast<cheeky::fg::BufferedPresenter>(owner.fg);
            status.active=buffered&&buffered->started;}
        status.native_frames=owner.fg->native_count;
        status.generated_frames=owner.fg->generated_count;
        status.fallback_frames=owner.fg->fallback_count;
        break;
    }
    *out=status;
    return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetDiagnostic(CheekyFgDiagnostic* out) noexcept {
    if(!out||out->abi!=CHEEKY_FG_CONTROL_ABI||out->size!=sizeof(*out))return false;
    out->message[0]=0;
    std::lock_guard lock(state_mutex);
    for(const auto& [handle,owner]:sessions) {
        if(!owner.fg)continue;
        std::lock_guard fg_lock(owner.fg->mx);
        strcpy_s(out->message,owner.fg->diagnostic_text());return true;
    }
    return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetPerformance(CheekyFgPerformance* out) noexcept {
    if(!out||out->abi!=CHEEKY_FG_CONTROL_ABI||out->size!=sizeof(*out))return false;
    *out=CheekyFgPerformance{};
    std::lock_guard lock(state_mutex);
    for(const auto& [handle,owner]:sessions) {
        if(!owner.fg)continue;
        std::lock_guard fg_lock(owner.fg->mx);
        *out=owner.fg->performance();return true;
    }
    return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetBatchTimingsV1(CheekyFgBatchTimingsV1* out) noexcept {
    if(!out||out->abi!=1||out->size!=sizeof(*out))return false;
    *out=CheekyFgBatchTimingsV1{};
    std::lock_guard lock(state_mutex);
    for(const auto& [handle,owner]:sessions){if(!owner.fg)continue;
        std::lock_guard fg_lock(owner.fg->mx);const auto timing=owner.fg->batch_timing();
        *out=static_cast<const CheekyFgBatchTimingsV1&>(timing);out->abi=1;out->size=sizeof(*out);
        if(timing.optical_separated&&timing.generated_count){out->phase_ms[0]+=timing.optical_ms;out->phase_last_ms[0]+=timing.optical_last_ms;}
        return true;}
    return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetBatchTimingsV2(CheekyFgBatchTimingsV2* out) noexcept {
    if(!out||out->abi!=2||out->size!=sizeof(*out))return false;
    *out=CheekyFgBatchTimingsV2{};std::lock_guard lock(state_mutex);
    for(const auto& [handle,owner]:sessions){if(!owner.fg)continue;
        std::lock_guard fg_lock(owner.fg->mx);*out=owner.fg->batch_timing();return true;}
    return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetCadenceV1(CheekyFgCadence* out) noexcept {
    if(!out||out->abi!=2||out->size!=sizeof(*out))return false;
    *out=CheekyFgCadence{};std::lock_guard lock(state_mutex);
    for(const auto& [handle,owner]:sessions){if(!owner.fg)continue;
        std::lock_guard fg_lock(owner.fg->mx);*out=owner.fg->cadence();return true;}
    return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetAutoStatusV1(CheekyFgAutoStatus* out) noexcept {
    if(!out||out->abi!=1||out->size!=sizeof(*out))return false;
    *out=CheekyFgAutoStatus{};out->supported=1;out->presented=1;std::lock_guard lock(state_mutex);
    for(const auto& [handle,owner]:sessions){
        const auto buffered=std::dynamic_pointer_cast<cheeky::fg::BufferedPresenter>(owner.fg);if(!buffered)continue;
        std::lock_guard fg_lock(buffered->mx);*out=buffered->automatic_status();return true;
    }
    return true;
}

extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetPacingTimelineV1(CheekyFgPacingTimeline* out) noexcept {
    if(!out||out->abi!=1||out->size!=sizeof(*out))return false;
    *out=CheekyFgPacingTimeline{};std::lock_guard lock(state_mutex);
    for(const auto& [handle,owner]:sessions){if(!owner.fg)continue;
        std::lock_guard fg_lock(owner.fg->mx);*out=owner.fg->pacing_timeline();return true;}
    return true;
}

extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetNativeSubmissionLatencyV1(CheekyFgNativeSubmissionLatency* out) noexcept {
    if(!out||out->abi!=1||out->size!=sizeof(*out))return false;
    *out=CheekyFgNativeSubmissionLatency{};std::lock_guard lock(state_mutex);
    for(const auto& [handle,owner]:sessions){if(!owner.fg)continue;
        std::lock_guard fg_lock(owner.fg->mx);*out=owner.fg->native_submission_latency();return true;}
    return true;
}

extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetOverheadV1(CheekyFgOverhead* out) noexcept {
    if(!out||out->abi!=1||out->size!=sizeof(*out))return false;
    *out=CheekyFgOverhead{};std::lock_guard lock(state_mutex);
    for(const auto& [handle,owner]:sessions){if(!owner.fg)continue;
        std::lock_guard fg_lock(owner.fg->mx);*out=owner.fg->overhead();
        if(owner.quad_capture&&owner.quad_capture->d3d11&&owner.quad_capture->d3d11->fg_guides){
            const auto guide=owner.quad_capture->d3d11->fg_guides->guide_cost();
            out->guide_samples=guide.samples;out->guide_ms=guide.mean;out->guide_last_ms=guide.last;
        }
        return true;}
    return true;
}

extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetPresentationV1(CheekyFgPresentation* out) noexcept {
    if(!out||out->abi!=2||out->size!=sizeof(*out))return false;
    *out=CheekyFgPresentation{};std::lock_guard lock(state_mutex);
    for(const auto& [handle,owner]:sessions){if(!owner.fg)continue;
        std::lock_guard fg_lock(owner.fg->mx);*out=owner.fg->presentation();return true;}
    return true;
}

extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetExperimentDiagnosticsV1(CheekyFgExperimentDiagnostics* out) noexcept {
    if(!out||out->abi!=2||out->size!=sizeof(*out))return false;
    *out=CheekyFgExperimentDiagnostics{};std::lock_guard lock(state_mutex);
    for(const auto& [handle,owner]:sessions){if(!owner.fg)continue;
        std::lock_guard fg_lock(owner.fg->mx);*out=owner.fg->experiments();return true;}
    return true;
}

extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetMemoryV1(CheekyFgMemory* out) noexcept {
    if(!out||out->abi!=1||out->size!=sizeof(*out))return false;
    *out=CheekyFgMemory{};out->supported=1;std::lock_guard lock(state_mutex);
    for(const auto& [handle,owner]:sessions){if(!owner.fg)continue;
        std::lock_guard fg_lock(owner.fg->mx);*out=owner.fg->memory();return true;}
    return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetDirectOutputDiagnosticsV1(CheekyFgDirectOutput* out) noexcept {
    if(!out||out->abi!=1||out->size!=sizeof(*out))return false;
    *out=CheekyFgDirectOutput{};std::lock_guard lock(state_mutex);
    for(const auto& [handle,owner]:sessions){if(!owner.fg)continue;
        std::lock_guard fg_lock(owner.fg->mx);*out=owner.fg->direct_output();return true;}
    return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetDirectOutputDetailsV1(CheekyFgDirectOutputDetails* out) noexcept {
    if(!out||out->abi!=1||out->size!=sizeof(*out))return false;
    *out=CheekyFgDirectOutputDetails{};std::lock_guard lock(state_mutex);
    for(const auto& [handle,owner]:sessions){if(!owner.fg)continue;
        std::lock_guard fg_lock(owner.fg->mx);*out=owner.fg->direct_output_details();return true;}
    return true;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrBeginFrame(
    const XrSession session, const XrFrameBeginInfo* info) {
    PFN_xrBeginFrame next{};
    std::shared_ptr<cheeky::fg::Presenter> presenter;
    cheeky::fg::AppFrameCallTimer app_timer{presenter,0};
    bool native_begin{};
    { std::lock_guard lock(state_mutex);
      const auto* instance = find_instance_for_session_locked(session);
      if (!instance) return XR_ERROR_HANDLE_INVALID;
      next = instance->dispatch.begin_frame;
      const auto owner=sessions.find(session);
      if(owner!=sessions.end()) {
          native_begin=owner->second.native_waits_pending!=0;
          if(!native_begin)presenter=owner->second.fg;
      } }
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    // Some hosts render DLSS before BeginFrame and call it just before submitting.
    // Preserve the source markers collected since the previous EndFrame.
    const auto result=presenter?presenter->begin_frame(info):next(session, info);
    if(trace_transition())log_startup("FG-TRANSITION xrBeginFrame thread=%lu path=%s result=%d\n",GetCurrentThreadId(),
        presenter?"presenter":native_begin?"native-reserved":"native",int(result));
    if (native_begin && XR_SUCCEEDED(result)) {
        std::lock_guard lock(state_mutex);
        const auto owner=sessions.find(session);
        if(owner!=sessions.end()) {
            if(owner->second.native_waits_pending)--owner->second.native_waits_pending;
            owner->second.native_frame_begun=true;
            if(owner->second.fg)owner->second.fg->set_native_frame_pending(true);
        }
    }
    return result;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrEndFrame(
    const XrSession session,
    const XrFrameEndInfo* const frame_end_info
) {
    struct AgeRetiredChains {XrSession session;~AgeRetiredChains(){age_retired_chains(session,false);}} age_retired{session};
    std::shared_ptr<cheeky::quad_capture::Compositor> quad;
    std::shared_ptr<cheeky::fg::Presenter> quad_presenter;
    Dispatch quad_dispatch{};
    {
        std::lock_guard lock(state_mutex);
        const auto found=sessions.find(session);
        if(found!=sessions.end()){
            quad=found->second.quad_capture;
            if(quad&&!found->second.native_frame_begun)quad_presenter=found->second.fg;
            quad_dispatch=instances.at(found->second.instance).dispatch;
            if(!quad && ++found->second.quad_capture_frames<=8 && frame_end_info && frame_end_info->layers){
                for(unsigned i=0;i<frame_end_info->layerCount;++i){
                    const auto* layer=frame_end_info->layers[i];
                    if(layer&&layer->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION)
                        log_startup("QUAD-CAPTURE native submit configuration=%d layer=%u input_views=%u\n",int(found->second.view_configuration),i,
                            reinterpret_cast<const XrCompositionLayerProjection*>(layer)->viewCount);
                }
            }
        }
    }
    if(quad){
        std::unique_lock menu_lock(menu_mutex);
        SessionState* menu_session{};
        {std::lock_guard lock(state_mutex);const auto found=sessions.find(session);if(found!=sessions.end())menu_session=&found->second;}
        const auto compose=[&](const XrFrameEndInfo* info){
            // The menu is ready; composition and FG pacing must not hold up recenter or teardown.
            if(menu_lock.owns_lock())menu_lock.unlock();
            return quad_capture_end(session,info,quad_dispatch,quad,quad_presenter);};
        const auto result=menu_session?submit_menu_frame(frame_end_info,quad_dispatch,*menu_session,bool(quad_presenter),compose)
                                      :compose(frame_end_info);
        if(trace_transition())log_startup("FG-TRANSITION xrEndFrame quad thread=%lu path=%s result=%d\n",GetCurrentThreadId(),
            quad_presenter?"presenter":"native",int(result));
        if(XR_SUCCEEDED(result)){std::lock_guard lock(state_mutex);auto found=sessions.find(session);if(found!=sessions.end()){
            found->second.native_frame_begun=false;if(found->second.fg)found->second.fg->set_native_frame_pending(found->second.native_waits_pending!=0);}}
        return result;
    }
    PFN_xrEndFrame next{};
    std::shared_ptr<cheeky::fg::Presenter> presenter;
    cheeky::fg::AppFrameCallTimer app_timer{presenter,1};
    {
        std::lock_guard lock(state_mutex);
        auto* instance = find_instance_for_session_locked(session);
        if (instance == nullptr) return XR_ERROR_HANDLE_INVALID;
        next = instance->dispatch.end_frame;
    }
    if (next == nullptr) return XR_ERROR_FUNCTION_UNSUPPORTED;

    std::unique_lock menu_lock(menu_mutex);
    SessionState* menu_session{};
    Dispatch menu_dispatch{};
    {
        std::lock_guard state_lock(state_mutex);
        const auto found = sessions.find(session);
        if (found != sessions.end()) {
            menu_session = &found->second;
            menu_dispatch = instances.at(found->second.instance).dispatch;
        }
    }
    // Privately owned swapchains let the layer hold native A until real B
    // arrives, then display A / DLSS-G(A,B) / B in chronological order.
    std::array<unsigned,2> release_indices{};
    const XrCompositionLayerProjection* fg_projection{};
    bool fg_compatible{};
    bool native_end{};
    {
        std::lock_guard fg_lock(state_mutex);
        const auto it=sessions.find(session);
        native_end=it!=sessions.end() && it->second.native_frame_begun;
        if(!native_end && it!=sessions.end() && it->second.running && it->second.fg &&
            it->second.view_configuration==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO){
            presenter=it->second.fg;
            const auto selected=cheeky::openxr::select_projection(frame_end_info,[session](XrSwapchain sc){
                const auto ch=swapchains.find(sc);
                return ch!=swapchains.end() && ch->second.session==session &&
                    ch->second.create_info.width==4 && ch->second.create_info.height==4;
            });
            if(selected.projection && !selected.ambiguous && !selected.unsupported &&
               frame_end_info) {
                fg_compatible=true;
                for(unsigned eye=0;eye<2;++eye){
                    const auto sc=selected.projection->views[eye].subImage.swapchain;
                    const auto ch=swapchains.find(sc);
                    if(ch==swapchains.end()||!ch->second.has_released){fg_compatible=false;break;}
                    release_indices[eye]=ch->second.released_index;
                }
                if(fg_compatible) {
                    presenter=it->second.fg;
                    fg_projection=selected.projection;
                }
            }
        }
    }
    const auto downstream=[&](const XrFrameEndInfo* info){
        return presenter?presenter->end_frame(info,release_indices,fg_projection):next(session,info);};
    const auto result = menu_session ? submit_menu_frame(frame_end_info,menu_dispatch,*menu_session,bool(presenter),downstream)
                                     : downstream(frame_end_info);
    menu_lock.unlock();
    std::unique_lock lock(state_mutex);
    if(native_end && XR_SUCCEEDED(result)) {
        const auto owner=sessions.find(session);
        if(owner!=sessions.end()) {
            owner->second.native_frame_begun=false;
            if(owner->second.fg)owner->second.fg->set_native_frame_pending(owner->second.native_waits_pending!=0);
        }
    }
    const auto selected = cheeky::openxr::select_projection(frame_end_info, [session](XrSwapchain handle) {
        const auto it = swapchains.find(handle);
        return it != swapchains.end() && it->second.session == session &&
            it->second.create_info.width == 4 && it->second.create_info.height == 4 &&
            it->second.create_info.arraySize == 1;
    });
    {
        auto owner = sessions.find(session);
        if (owner != sessions.end()) {
            const auto generation = owner->second.generation;
            owner->second.submitted_projection_valid = false;
            SessionState::LocatedProjection located{};
            if (frame_end_info) for (const auto& candidate : owner->second.located_history)
                if (candidate.valid && candidate.time == frame_end_info->displayTime) located = candidate;
            const auto* projection = selected.projection;
            bool valid = XR_SUCCEEDED(result) && projection && located.valid &&
                !selected.ambiguous && !selected.unsupported;
            XrQuaternionf space_rotation{0, 0, 0, 1};
            if (valid && projection->space != located.space) {
                auto* instance = find_instance_for_session_locked(session);
                const auto locate = instance ? instance->dispatch.locate_space : nullptr;
                XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
                // Runtime callbacks must not run under our state mutex.
                lock.unlock();
                valid = locate && XR_SUCCEEDED(locate(projection->space, located.space,
                    frame_end_info->displayTime, &location)) &&
                    (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
                lock.lock();
                space_rotation = location.pose.orientation;
            }
            std::array<XrQuaternionf, 2> deltas{};
            std::array<XrFovf, 2> fovs{};
            if (valid) valid = normalize_rotation(space_rotation);
            for (unsigned eye = 0; valid && eye < 2; ++eye) {
                auto tracked = located.poses[eye].orientation;
                auto submitted = projection->views[eye].pose.orientation;
                const auto& fov = projection->views[eye].fov;
                const float width = std::tan(fov.angleRight) - std::tan(fov.angleLeft);
                const float height = std::tan(fov.angleUp) - std::tan(fov.angleDown);
                valid = normalize_rotation(tracked) && normalize_rotation(submitted) &&
                    std::isfinite(width) && std::isfinite(height) && width > 0.0001F && height > 0.0001F;
                if (!valid) break;
                const XrQuaternionf inverse{-tracked.x, -tracked.y, -tracked.z, tracked.w};
                deltas[eye] = multiply_rotation(inverse, multiply_rotation(space_rotation, submitted));
                valid = normalize_rotation(deltas[eye]);
                fovs[eye] = fov;
            }
            owner = sessions.find(session);
            if (valid && owner != sessions.end() && owner->second.generation == generation) {
                owner->second.submitted_fov = fovs;
                owner->second.submitted_rotation_delta = deltas;
                owner->second.submitted_projection_time = frame_end_info->displayTime;
                owner->second.submitted_projection_valid = true;
            }
        }
    }
    {
        auto owner = sessions.find(session);
        if (owner != sessions.end()) {
            std::array<cheeky::openxr_calibration::Region, 2> regions{};
            std::array<std::uint32_t, 2> released{};
            bool valid = XR_SUCCEEDED(result) && selected.projection &&
                owner->second.view_configuration == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            if (valid) {
                const auto* projection = selected.projection;
                for (unsigned eye = 0; eye < 2; ++eye) {
                    const auto& image = projection->views[eye].subImage;
                    regions[eye] = {reinterpret_cast<std::uint64_t>(image.swapchain), image.imageArrayIndex,
                        image.imageRect.offset.x, image.imageRect.offset.y, image.imageRect.extent.width, image.imageRect.extent.height};
                    auto chain = swapchains.find(image.swapchain);
                    if (chain == swapchains.end() || chain->second.session != session || !chain->second.has_released)
                        valid = false;
                    else released[eye] = chain->second.released_index;
                }
            }
            owner->second.calibration.end(cheeky::openxr_calibration::bridge(), valid ? regions : decltype(regions){}, released, valid);
            // Validate the submitted pair before closing its core record and
            // arming the next sample. End-to-end intervals cover both early and
            // late BeginFrame hosts; the first interval is an unstamped warm-up.
            owner->second.calibration.begin(cheeky::openxr_calibration::bridge(),
                owner->second.generation, owner->second.graphics_api);
        }
    }
    if (XR_FAILED(result)) return result;

    {
        const auto session_it = sessions.find(session);
        if (session_it != sessions.end()) {
            auto& state = session_it->second;
            ++state.rendered_frames;
            state.ambiguous_resource = selected.ambiguous;
            state.unsupported_view_configuration = selected.unsupported ||
                state.view_configuration != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            if (const auto* projection = selected.projection) {
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
            }
            if (!selected.projection) {
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
