#pragma once
// ABI boundary between UEVR NGX interception and the OpenXR presentation layer.
// A returned pair owns COM references; receiver must call CheekyFG_ReleasePair.
#include <cstdint>
struct ID3D12Resource;
struct ID3D12CommandQueue;
struct IUnknown;

constexpr std::uint32_t CHEEKY_FG_PAIR_ABI = 0x43464739; // CFG9: release milestones and repeat-submission experiment
struct CheekyFgEye {
    ID3D12Resource* depth{};
    ID3D12Resource* motion{};
    std::uint64_t feature{};
    std::uint64_t evaluation{};
    std::uint64_t render_frame{};
    std::uint32_t width{}, height{};
    std::uint32_t depth_width{}, depth_height{};
    std::uint32_t motion_width{}, motion_height{};
    std::uint32_t depth_base_x{}, depth_base_y{};
    std::uint32_t motion_base_x{}, motion_base_y{};
    float motion_scale_x{1}, motion_scale_y{1};
    float jitter_x{}, jitter_y{};
    float jitter_correction_x{},jitter_correction_y{};
    std::uint32_t motion_jittered{};
    std::uint32_t depth_inverted{}, reset{}, low_res_mv{};
    float focus_width{1},focus_height{1},focus_x{},focus_y{},focus_feather{0.04f};
    std::uint32_t resolution_percent{100},peripheral_percent{50};
    std::uint32_t peripheral_method{2},depth_bits{32},motion_bits{32};
    std::uint64_t configuration{};
};
struct CheekyFgPair {
    std::uint32_t abi{CHEEKY_FG_PAIR_ABI};
    std::uint32_t size{sizeof(CheekyFgPair)};
    CheekyFgEye eyes[2]{};
    ID3D12CommandQueue* producer_queue{};
    std::uint64_t capture_token{}; // Required input: token observed at color release.
    IUnknown* lease{}; // Retain until the consumer GPU has finished reading guides.
    std::uint64_t render_frame{}; // Required identity received with the color stamp.
    std::uint32_t diagnostic_mode{}; // 0 normal, 1 prepare-first, 2 left-only, 3 right-only, 4 split-lists.
    std::uint32_t enable_frame_id{1}; // Optional NGX BackbufferFrameID A/B; applied to each feature.
    std::uint32_t fg_queue_priority{1}; // 0 separate NORMAL; 1 separate HIGH; 2 shared game queue.
    std::uint32_t release_point{}; // 0 FG submission, 1 GPU preparation, 2 stereo completion, 3 generated XR submission.
    std::uint32_t submit_repeats{1}; // Resubmit unchanged scene content at runtime frame opportunities.
};
using CheekyFgAcquireFn=bool(__cdecl*)(CheekyFgPair*);
using CheekyFgReleaseFn=void(__cdecl*)(CheekyFgPair*);
using CheekyFgCaptureTokenFn=std::uint64_t(__cdecl*)(ID3D12Resource*);
struct CheekyFgCaptureStamp {
    std::uint32_t abi{1},size{sizeof(CheekyFgCaptureStamp)};
    std::uint64_t token{},render_frame{};
};
using CheekyFgCaptureStampFn=bool(__cdecl*)(ID3D12Resource*,CheekyFgCaptureStamp*);
using CheekyFgRegisterHookFn=int(*)(void*,void*,void**);
using CheekyFgUnregisterHookFn=void(*)(int);
using CheekyFgAttachFrameSourceFn=bool(__cdecl*)(void*,CheekyFgRegisterHookFn,CheekyFgUnregisterHookFn);
using CheekyFgDetachFrameSourceFn=void(__cdecl*)();
struct CheekyFgCaptureHealth {
    std::uint32_t abi{1},size{sizeof(CheekyFgCaptureHealth)},plugin_hook{};
    std::uint64_t frame_calls{},last_guide_frame{},captured_guides{},missing_frame_guides{},color_copies{};
    char source[128]{};
    std::uint64_t color_frame{},match_frame{},latest_frame{},latest_evaluation{},consumed_evaluation{};
    std::uint32_t eligible{},pending{},foreign{},cancelled{};
    char match_reason[128]{};
};
using CheekyFgCaptureHealthFn=bool(__cdecl*)(CheekyFgCaptureHealth*);

constexpr std::uint32_t CHEEKY_FG_CONTROL_ABI = 2;
struct CheekyFgControl {
    std::uint32_t abi{CHEEKY_FG_CONTROL_ABI}, size{sizeof(CheekyFgControl)};
    std::uint32_t enabled{};
    std::uint64_t configuration{};
};
using CheekyFgControlFn = bool(__cdecl*)(CheekyFgControl*);
enum class CheekyFgState : std::uint32_t {
    off, waiting_for_stereo, starting, waiting_for_history, waiting_for_guides,
    queue_mismatch, ngx_unavailable, generation_failed, generating, unsupported_submission,
    presenter_failed, presentation_failed, guide_frame_mismatch
};
struct CheekyFgStatus {
    std::uint32_t abi{CHEEKY_FG_CONTROL_ABI}, size{sizeof(CheekyFgStatus)};
    std::uint32_t layer_present{}, controls_supported{}, compatible_session{}, active{};
    CheekyFgState state{CheekyFgState::off};
    std::uint64_t native_frames{}, generated_frames{}, fallback_frames{};
};
using CheekyFgStatusFn = bool(__cdecl*)(CheekyFgStatus*);
struct CheekyFgDiagnostic {
    std::uint32_t abi{CHEEKY_FG_CONTROL_ABI},size{sizeof(CheekyFgDiagnostic)};
    char message[384]{};
};
using CheekyFgDiagnosticFn=bool(__cdecl*)(CheekyFgDiagnostic*);
// Optional extension: older plugin/layer combinations retain the status ABI.
// GPU values are milliseconds for both eyes in one generated stereo pair.
struct CheekyFgPerformance {
    std::uint32_t abi{CHEEKY_FG_CONTROL_ABI},size{sizeof(CheekyFgPerformance)};
    std::uint32_t gpu_supported{},gpu_valid{},foveated_eyes{},window_samples{};
    std::uint64_t gpu_samples{};
    double center_ms{},center_last_ms{},periphery_ms{},periphery_last_ms{};
    double preparation_ms{},preparation_last_ms{},total_ms{},total_last_ms{};
    double completion_wait_ms{},copy_wait_ms{},xr_wait_ms{},xr_period_ms{};
    std::uint32_t center_width{},center_height{},peripheral_width{},peripheral_height{};
    std::uint32_t output_width{},output_height{},peripheral_method{2};
    std::uint32_t source_depth_format{},source_motion_format{};
    std::uint32_t asynchronous_presenter{},queued_native{};
    std::uint64_t skipped_native{};
    double producer_cpu_ms{},pacing_wait_ms{};
    std::uint64_t color_token{},guide_token{},mismatched_guides{};
    std::uint64_t color_render_frame{},guide_render_frame{};
    std::uint32_t inference_color_format{},shared_gpu_queue{};
};
using CheekyFgPerformanceFn=bool(__cdecl*)(CheekyFgPerformance*);

// Separate optional export: v15's performance structure remains unchanged.
struct CheekyFgCadence {
    std::uint32_t abi{1},size{sizeof(CheekyFgCadence)},supported{},valid{};
    double game_fps{},native_fps{},generated_fps{},unique_fps{},repeat_fps{},submission_fps{};
    double refresh_hz{},native_target_fps{},application_period_ms{};
    double app_image_wait_ms{},present_image_wait_ms{},begin_ms{},end_ms{},present_cpu_ms{};
    double app_begin_ms{},app_end_ms{},app_release_ms{};
};
using CheekyFgCadenceFn=bool(__cdecl*)(CheekyFgCadence*);

// Optional GPU stages outside the original processing timestamp interval.
struct CheekyFgOverhead {
    std::uint32_t abi{1},size{sizeof(CheekyFgOverhead)},supported{};
    std::uint64_t guide_samples{},capture_samples{},publication_samples{},upload_samples{};
    double guide_ms{},guide_last_ms{},capture_ms{},capture_last_ms{};
    double publication_ms{},publication_last_ms{},upload_ms{},upload_last_ms{};
};
using CheekyFgOverheadFn=bool(__cdecl*)(CheekyFgOverhead*);

// Optional presentation telemetry; previous export layouts remain unchanged.
// CPU color-capture observation -> xrEndFrame entry, NOT motion-to-photon.
// Generated content time is the midpoint of its two native capture times.
struct CheekyFgPresentation {
    std::uint32_t abi{1},size{sizeof(CheekyFgPresentation)},supported{};
    std::uint64_t native_samples{},generated_samples{},repeat_samples{};
    std::uint64_t wait_samples{},wait_arrivals{},wait_timeouts{};
    double native_age_ms{},native_age_last_ms{},generated_age_ms{},generated_age_last_ms{};
    double newest_input_age_ms{},newest_input_age_last_ms{},repeat_age_ms{},repeat_age_last_ms{};
    double late_wait_ms{},late_wait_last_ms{},wait_budget_ms{};
};
using CheekyFgPresentationFn=bool(__cdecl*)(CheekyFgPresentation*);

// Additive experiments API: preserve the existing pair/status layouts.
struct CheekyFgTransportOptions {
    std::uint32_t abi{1},size{sizeof(CheekyFgTransportOptions)};
    std::uint32_t color_capture_mode{},upload_mode{},latency_diagnostics{1};
};
using CheekyFgTransportOptionsFn=bool(__cdecl*)(CheekyFgTransportOptions*);
struct CheekyFgStorageOptions {
    std::uint32_t abi{1},size{sizeof(CheekyFgStorageOptions)};
    std::uint32_t compact{},peripheral_percent{25};
    float width{.55f},height{.45f},x{},y{},feather{.04f};
};
using CheekyFgStorageOptionsFn=bool(__cdecl*)(CheekyFgStorageOptions*);
struct CheekyFgTimingValue {double mean{},last{};};
struct CheekyFgExperimentDiagnostics {
    std::uint32_t abi{1},size{sizeof(CheekyFgExperimentDiagnostics)},supported{};
    std::uint32_t color_capture_mode{},upload_mode{},latency_enabled{},latency_valid{};
    std::uint64_t full_color_copies{},direct_eye_copies{},eye_crop_copies{},capture_fallbacks{};
    std::uint64_t upload_copies{},upload_batches{},latency_samples{},latency_rejected{};
    CheekyFgTimingValue queue_entry,capture_dependency,ready_to_fg,gpu_work,gpu_to_observed,enqueue_to_observed,submit_to_observed;
    std::uint32_t display_time_supported{};
    std::uint64_t gpu_ready_samples{},predicted_samples{},prediction_late{},generated_gate_releases{},generated_gate_skips{};
    CheekyFgTimingValue native_gpu_tail,gpu_ready_to_submit,gpu_ready_to_display;
};
using CheekyFgExperimentDiagnosticsFn=bool(__cdecl*)(CheekyFgExperimentDiagnostics*);

// Resource allocation requirements, not residency or total NGX memory. Separate
// export so existing performance/status/pair ABI layouts remain unchanged.
namespace cheeky::fg_memory {
enum Category : unsigned {capture,generated,upload_pool,guide_depth,guide_motion,
    prepared_color,prepared_depth,prepared_motion,ngx_outputs,periphery,composition,
    swapchain_native,swapchain_generated,swapchain_auxiliary,count};
inline constexpr const char* names[count]{"capture","generated","upload_pool","guide_depth","guide_motion",
    "prepared_color","prepared_depth","prepared_motion","ngx_outputs","periphery","composition",
    "swapchain_native","swapchain_generated","swapchain_auxiliary"};
}
struct CheekyFgMemoryBucket {
    std::uint64_t bytes{},resources{},reusable_bytes{},reusable_resources{},unknown_resources{};
};
struct CheekyFgMemory {
    std::uint32_t abi{1},size{sizeof(CheekyFgMemory)},supported{},presenter_present{},guides_supported{};
    std::uint64_t capture_tick{},worker_tick{},presenter_tick{},guide_tick{};
    CheekyFgMemoryBucket categories[cheeky::fg_memory::count]{};
};
using CheekyFgMemoryFn=bool(__cdecl*)(CheekyFgMemory*);
