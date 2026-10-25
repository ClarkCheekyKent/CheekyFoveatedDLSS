#pragma once
// ABI boundary between UEVR NGX interception and the OpenXR presentation layer.
// A returned pair owns COM references; receiver must call CheekyFG_ReleasePair.
#include <cstdint>
#include "fg_methods.hpp"
struct ID3D12Resource;
struct ID3D12CommandQueue;
struct IUnknown;

constexpr std::uint32_t CHEEKY_FG_PAIR_ABI = 0x4346473E; // Optical flow vector grids; install runtime and layer together.
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
    std::uint32_t show_foveation_border{}; // Blue border on FG generated output only (not history).
    std::uint32_t resolution_percent{100},peripheral_percent{50};
    std::uint32_t peripheral_method{2};
    std::uint32_t center_method{}; // 0 DLSS, 1 simple motion warp, 2 bidirectional interpolation.
    CheekyFgOpticalSettings optical;
    std::uint64_t configuration{};
    std::uint32_t prepared_guides{}; // Guides are prepared FG grids rather than raw snapshots.
    ID3D12Resource* peripheral_depth{};
    ID3D12Resource* peripheral_motion{};
};
struct CheekyFgPair {
    std::uint32_t abi{CHEEKY_FG_PAIR_ABI};
    std::uint32_t size{sizeof(CheekyFgPair)};
    CheekyFgEye eyes[2]{};
    ID3D12CommandQueue* producer_queue{};
    std::uint64_t capture_token{}; // Required input: token observed at color release.
    IUnknown* lease{}; // Retain until the consumer GPU has finished reading guides.
    std::uint64_t render_frame{}; // Producer frame serial (quad views); 0 for pairs matched at xrEndFrame.
    std::uint32_t fg_queue_priority{1}; // 0 separate NORMAL; 1 separate HIGH; 2 shared game queue.
    std::uint32_t release_point{}; // 0 submission, 1 GPU preparation, 2 completion, 3 generated XR, 4 submission + 4ms.
};
inline bool cheeky_fg_needs_ngx(const CheekyFgPair& pair) noexcept {
    for(const auto& g:pair.eyes)
        if(g.center_method==0||((g.focus_width<.999f||g.focus_height<.999f)&&g.peripheral_method==3))return true;
    return false;
}
// eyes[] is in physical OpenXR order. Evaluation ordinals retain capture order,
// so either eye may own the final evaluation identified by the color token.
inline bool cheeky_fg_pair_matches_color(const CheekyFgPair& pair,std::uint64_t token,std::uint64_t frame) noexcept {
    return token>=2&&pair.capture_token==token&&pair.eyes[0].render_frame==frame&&pair.eyes[1].render_frame==frame&&
        ((pair.eyes[0].evaluation==token-1&&pair.eyes[1].evaluation==token)||
         (pair.eyes[1].evaluation==token-1&&pair.eyes[0].evaluation==token));
}
using CheekyFgAcquireFn=bool(__cdecl*)(CheekyFgPair*);
using CheekyFgReleaseFn=void(__cdecl*)(CheekyFgPair*);
using CheekyFgCaptureTokenFn=std::uint64_t(__cdecl*)(ID3D12Resource*);
struct CheekyFgCaptureStamp {
    std::uint32_t abi{1},size{sizeof(CheekyFgCaptureStamp)};
    std::uint64_t token{},render_frame{};
};
using CheekyFgCaptureStampFn=bool(__cdecl*)(ID3D12Resource*,CheekyFgCaptureStamp*);
// Once per native xrEndFrame, AFTER both eye releases. True claims one
// complete stereo pair; false retires an unused/failed frame. This is the
// only way a captured guide pair is matched to the frame's color.
using CheekyFgCompleteHooklessFrameFn=std::uint64_t(__cdecl*)(std::uint32_t use_for_fg);
struct CheekyFgCaptureHealth {
    std::uint32_t abi{2},size{sizeof(CheekyFgCaptureHealth)};
    std::uint64_t captured_guides{},latest_evaluation{},consumed_evaluation{};
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
// Optional extension; older runtime/layer pairs retain 2x behavior.
struct CheekyFgRateOptions {
    std::uint32_t abi{1},size{sizeof(CheekyFgRateOptions)},multiplier{2};
};
using CheekyFgRateOptionsFn=bool(__cdecl*)(CheekyFgRateOptions*);
struct CheekyFgRateOptionsV2 {
    std::uint32_t abi{2},size{sizeof(CheekyFgRateOptionsV2)},multiplier{2},automatic{};
};
using CheekyFgRateOptionsV2Fn=bool(__cdecl*)(CheekyFgRateOptionsV2*);
struct CheekyFgAutoStatus {
    std::uint32_t abi{1},size{sizeof(CheekyFgAutoStatus)},supported{},enabled{},valid{};
    std::uint32_t selected{2},presented{2},maximum{4},reason{};
    std::uint64_t samples{},switches{};
    double cpu_ms{},gpu_ms{},fg_ms{};
};
using CheekyFgAutoStatusFn=bool(__cdecl*)(CheekyFgAutoStatus*);
// Optional status extension. Keeps the existing FG control/status ABI intact.
struct CheekyFgUevrModeV1 {
    std::uint32_t abi{1}, size{sizeof(CheekyFgUevrModeV1)};
    std::uint32_t attached{}, known{}, mode{0xFFFFFFFFu};
};
using CheekyFgUevrModeFn = bool(__cdecl*)(CheekyFgUevrModeV1*);
// Optional V2: monotonic mode-change epoch. The OpenXR layer can detect a
// Native -> non-native -> Native transition even if no xrWaitFrame occurred
// while the non-native mode was selected. V1 remains ABI-compatible.
struct CheekyFgUevrModeV2 {
    std::uint32_t abi{2}, size{sizeof(CheekyFgUevrModeV2)};
    std::uint32_t attached{}, known{}, mode{0xFFFFFFFFu};
    std::uint64_t transition_epoch{};
};
using CheekyFgUevrModeV2Fn = bool(__cdecl*)(CheekyFgUevrModeV2*);
enum class CheekyFgState : std::uint32_t {
    off, waiting_for_stereo, starting, waiting_for_history, waiting_for_guides,
    queue_mismatch, ngx_unavailable, generation_failed, generating, unsupported_submission,
    presenter_failed, presentation_failed, guide_frame_mismatch, paused_rendering_mode
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

// Optional extension: one native-pair batch and its generated stereo phases.
// Includes immutable output publication, excludes capture, XR upload and pacing.
// Phase 1 includes shared optical-flow preparation/estimation when present.
struct CheekyFgBatchTimingsV1 {
    std::uint32_t abi{1},size{sizeof(CheekyFgBatchTimingsV1)},supported{},valid{};
    std::uint32_t generated_count{},optical_regions{};
    std::uint64_t samples{};
    double batch_ms{},batch_last_ms{};
    double phase_ms[3]{},phase_last_ms[3]{};
};
using CheekyFgBatchTimingsFn=bool(__cdecl*)(CheekyFgBatchTimingsV1*);

// Separate optional export: v15's performance structure remains unchanged.
struct CheekyFgCadence {
    std::uint32_t abi{2},size{sizeof(CheekyFgCadence)},supported{},valid{};
    double game_fps{},native_fps{},generated_fps{},unique_fps{},submission_fps{};
    double refresh_hz{},native_target_fps{},application_period_ms{};
    double app_image_wait_ms{},present_image_wait_ms{},begin_ms{},end_ms{},present_cpu_ms{};
    double app_begin_ms{},app_end_ms{},app_release_ms{};
};
using CheekyFgCadenceFn=bool(__cdecl*)(CheekyFgCadence*);

// Separate optional ABI: CPU pacing timeline. None of these durations is input-to-photon latency.
// All entries are milliseconds; mean and last use a bounded 60-sample window.
struct CheekyFgPacingTimeline {
    std::uint32_t abi{1},size{sizeof(CheekyFgPacingTimeline)},supported{};
    std::uint32_t recent_generated{}; // Generated xrEndFrame completed within the last 2 seconds.
    std::uint64_t generated_last_tick{},deferred_events{},deferred_releases{};
    double clock_late_ms{},clock_late_last_ms{},credit_deferred_ms{},credit_deferred_last_ms{};
    double game_wait_credit_ms{},game_wait_credit_last_ms{},game_begin_gate_ms{},game_begin_gate_last_ms{};
    double presenter_content_ms{},presenter_content_last_ms{};
    double native_xr_end_ms{},native_xr_end_last_ms{},generated_xr_end_ms{},generated_xr_end_last_ms{};
    double fg_submit_to_complete_ms{},fg_submit_to_complete_last_ms{};
};
using CheekyFgPacingTimelineFn=bool(__cdecl*)(CheekyFgPacingTimeline*);

// Native color GPU-copy completion -> actual private xrEndFrame ENTER (not display).
// Signed delta >0: held after the native copy finished; <0: submitted before
// the copy finished (the GPU/runtime may still wait). This does NOT provide
// an FG-on/off delta because the native capture is bypassed with FG disabled.
struct CheekyFgNativeSubmissionLatency {
    std::uint32_t abi{1},size{sizeof(CheekyFgNativeSubmissionLatency)},supported{},enabled{};
    std::uint64_t gpu_samples{},missing_stamps{},held_samples{},early_samples{},pending{};
    double gpu_ready_to_submit_ms{},gpu_ready_to_submit_last_ms{}; // Signed.
    double native_hold_ms{},native_hold_last_ms{}; // Positive part only.
    double submit_ahead_ms{},submit_ahead_last_ms{}; // Positive magnitude of negative part.
    double capture_to_submit_ms{},capture_to_submit_last_ms{}; // CPU capture observation -> submit.
};
using CheekyFgNativeSubmissionLatencyFn=bool(__cdecl*)(CheekyFgNativeSubmissionLatency*);

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
// Generated content time uses its interpolation fraction between native capture times.
struct CheekyFgPresentation {
    std::uint32_t abi{2},size{sizeof(CheekyFgPresentation)},supported{};
    std::uint64_t native_samples{},generated_samples{};
    double native_age_ms{},native_age_last_ms{},generated_age_ms{},generated_age_last_ms{};
    double newest_input_age_ms{},newest_input_age_last_ms{};
};
using CheekyFgPresentationFn=bool(__cdecl*)(CheekyFgPresentation*);

// Additive experiments API: preserve the existing pair/status layouts.
struct CheekyFgTransportOptions {
    std::uint32_t abi{2},size{sizeof(CheekyFgTransportOptions)};
    std::uint32_t latency_diagnostics{1};
};
using CheekyFgTransportOptionsFn=bool(__cdecl*)(CheekyFgTransportOptions*);
// Separate export so the transport layout keeps its exact-size check.
struct CheekyFgPacingOptions {
    std::uint32_t abi{1},size{sizeof(CheekyFgPacingOptions)};
    std::uint32_t clock_mode{}; // 0 independent display clock; 1 presented native rate; 2 rate + runtime phase lock.
    std::uint32_t native11_release_point{3}; // Shared release setting: 0 FG submission ... 3 generated frame submission, 4 submission + 4 ms.
    std::uint32_t extra_frame_slots{}; // 1: 4 banked credits and 5 capture slots (more VRAM). Absent in older layers.
    std::uint32_t dedicated_present_queue{}; // 1: give the OpenXR runtime the layer's own D3D12 queue at session creation.
    std::uint32_t gpu_waited_present{}; // 1: hand generated frames to the runtime once queued; the upload waits on the GPU, not the presenter's CPU.
    std::uint32_t limit_gpu_ahead{}; // 1: Follow clock + release Never hold the game while its previous frame is not yet captured on the GPU.
    std::uint32_t adaptive_gpu_lead{}; // 1: with limit_gpu_ahead, release the game a self-tuned lead before that capture is predicted to finish.
};
using CheekyFgPacingOptionsFn=bool(__cdecl*)(CheekyFgPacingOptions*);
// Independent export: D3D12 quad-view composition in xrEndFrame.
struct CheekyQuadComposeOptions {
    std::uint32_t abi{1},size{sizeof(CheekyQuadComposeOptions)};
    std::uint32_t wait_gpu{1}; // 1 wait for the composition to execute (baseline); 0 only wait on an in-flight recording.
};
using CheekyQuadComposeOptionsFn=bool(__cdecl*)(CheekyQuadComposeOptions*);
// Independent export: native D3D11 FG capture options.
struct CheekyFgNative11Options {
    std::uint32_t abi{1},size{sizeof(CheekyFgNative11Options)};
    std::uint32_t skip_identity_crop{}; // 1 reuse a whole-image eye capture instead of copying it again.
};
using CheekyFgNative11OptionsFn=bool(__cdecl*)(CheekyFgNative11Options*);
// Independent export: older transport/pair layouts remain compatible.
struct CheekyFgDirectOutput {
    std::uint32_t abi{1},size{sizeof(CheekyFgDirectOutput)},supported{},requested{};
    std::uint32_t last_path{}; // 0 baseline, 1 direct NGX+assembly, 2 NGX scratch+direct assembly, 3 fallback.
    std::uint64_t attempts{},frames{},ngx_direct_eyes{},ngx_scratch_eyes{},fallbacks{},unsupported{},not_ready{};
    double wait_ms{},last_wait_ms{};
};
using CheekyFgDirectOutputFn=bool(__cdecl*)(CheekyFgDirectOutput*);
// Kept separate from the existing direct-output counters/options ABI.
enum class CheekyFgDirectReason : std::uint32_t {none,configuration,chain_missing,chain_usage,chain_extent,chain_format,
    native_not_ready,acquire,wait,image_format,image_usage,image_extent,cancelled,generation,queue_wait,release};
inline const char* cheeky_fg_direct_reason_name(CheekyFgDirectReason reason) noexcept {
    constexpr const char* names[]{"None","Requires foveated custom periphery","Scene swapchain missing","Scene swapchain is not writable",
        "Scene dimensions differ","Scene format differs","Preceding native frame not ready","XR image acquire failed","XR image wait failed",
        "XR backing format incompatible","XR backing image lacks writable flags","XR backing dimensions unsupported","Cancelled or hidden",
        "Generation unavailable","Binding queue wait failed","XR image release failed"};
    const auto n=static_cast<std::uint32_t>(reason);return n<sizeof(names)/sizeof(*names)?names[n]:"Unknown";
}
struct CheekyFgDirectOutputDetails {
    std::uint32_t abi{1},size{sizeof(CheekyFgDirectOutputDetails)},supported{};
    CheekyFgDirectReason reason{};
    std::uint32_t eye{},expected_format{},chain_format{},image_format{},image_flags{};
    std::uint32_t expected_width{},expected_height{},chain_width{},chain_height{},image_width{},image_height{};
    std::int32_t xr_result{};
    std::uint64_t rejections{};
};
using CheekyFgDirectOutputDetailsFn=bool(__cdecl*)(CheekyFgDirectOutputDetails*);
struct CheekyFgStorageOptions {
    std::uint32_t abi{1},size{sizeof(CheekyFgStorageOptions)};
    std::uint32_t compact{},peripheral_percent{25};
    float width{.55f},height{.45f},x{},y{},feather{.04f};
};
using CheekyFgStorageOptionsFn=bool(__cdecl*)(CheekyFgStorageOptions*);
// Snapshot of the actual per-physical-eye NGX crop, published by the guide
// producer. A missing eye must use full-color fallback, never global offsets.
struct CheekyFgStaticEyeCrop {float width{},height{},x{},y{};};
struct CheekyFgStaticEyeCropsV1 {
    std::uint32_t abi{1},size{sizeof(CheekyFgStaticEyeCropsV1)},valid_mask{};
    CheekyFgStaticEyeCrop eye[2]{};
};
using CheekyFgStaticEyeCropsFn=bool(__cdecl*)(CheekyFgStaticEyeCropsV1*);
// The guides belonging to ONE unconsumed native interval, not a cache of
// the preceding frame. The lookup can return the first physical eye before
// the second submits; it does not consume/lease either guide.
// The returned token must still match CompleteHooklessFrame/AcquirePair at
// xrEndFrame: capture can race submission on another thread.
struct CheekyFgCaptureCropsV2 {
    std::uint32_t abi{2},size{sizeof(CheekyFgCaptureCropsV2)};
    std::uint64_t token{},render_frame{}; // render_frame=0 in hook-free mode.
    std::uint32_t width{},height{},valid_mask{};
    CheekyFgStaticEyeCrop eye[2]{};
};
using CheekyFgCaptureCropsFn=bool(__cdecl*)(CheekyFgCaptureCropsV2*);
struct CheekyFgTimingValue {double mean{},last{};};
struct CheekyFgExperimentDiagnostics {
    std::uint32_t abi{2},size{sizeof(CheekyFgExperimentDiagnostics)},supported{};
    std::uint32_t latency_enabled{},latency_valid{};
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
