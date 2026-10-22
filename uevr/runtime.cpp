#include "vulkan_backend.hpp"
#include "runtime_api.hpp"
#include "settings_io.hpp"
#include "frame_cadence.hpp"
#include "gpu_memory.hpp"
#include "support_bundle.hpp"
#include "eye_calibration_capture.hpp"
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include "graphics_observer.hpp"
#include "processing_owner.hpp"
#include "runtime.hpp"
#include "diagnostics.hpp"
#include "d3d11_peripheral_dlaa.hpp"
#include "d3d11_d3d12_transport.hpp"
#include "eye_calibration.hpp"
#include "quad_view_bridge.hpp"
#include "gaze_foveation.hpp"
#include "openvr_gaze.hpp"
#include "libovr_gaze.hpp"
#include "dlss_nr.hpp"
#include "d3d12_ngx_dispatch.hpp"
#include "fg_capture.hpp"
#include "afw_compatibility.hpp"
#include "version.h"
#include <atomic>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <charconv>
#include <chrono>

namespace cheeky::foveated_dlss {
namespace {
// Intentionally process-resident: workers and game detours never reference the
// unloadable host adapter, nor call into Lua or UEVR.
struct State {
    std::mutex mutex, log_mutex;
    std::filesystem::path directory, config;
    HANDLE log{INVALID_HANDLE_VALUE}, save_event{}, owner{};
    bool started{}, graphics_ready{}, failed{};
    std::uint32_t renderer{};
    CheekyRuntimeHost host{CheekyRuntimeHost::uevr};
    void* observed_device{};
    void* observed_queue{};
    std::uint64_t revision{}, saved_revision{}, request{}, applied_request{};
    std::uint64_t attachment_sequence{};
    FrameCadence cadence;
    GpuMemory memory;
    CheekyFgMemory fg_memory;
    std::uint64_t next_fg_memory_sample{};
    std::uint64_t logged_memory_sample{};
    ULONGLONG next_timing_log{};
    std::string message{"Not initialized"};
    std::atomic<bool> save_requested{}, report_requested{};
    std::atomic<bool> report_busy{}, report_browser{};
    std::atomic<unsigned> report_open{};
    std::filesystem::path report_zip;
    std::string report_summary;
};
State& state() { static auto* instance = new State; return *instance; }
void sample_fg_memory(State& s,std::uint64_t now) {
    if(now<s.next_fg_memory_sample)return;
    CheekyFgMemory value;fg_memory_diagnostics(value);s.fg_memory=value;s.next_fg_memory_sample=now+1000;
}
std::string fg_memory_json(const CheekyFgMemory& value) {
    std::ostringstream out;out << std::boolalpha;
    out << "{\"layer_supported\":" << bool(value.supported) << ",\"presenter_present\":" << bool(value.presenter_present)
        << ",\"guides_supported\":" << bool(value.guides_supported) << ",\"capture_tick\":" << value.capture_tick
        << ",\"worker_tick\":" << value.worker_tick << ",\"presenter_tick\":" << value.presenter_tick << ",\"guide_tick\":" << value.guide_tick
        << ",\"categories\":{";
    std::uint64_t app_bytes{},runtime_bytes{},unknown{};
    for(unsigned i=0;i<fg_memory::count;++i){const auto& b=value.categories[i];
        if(i)out << ',';
        out << '"' << fg_memory::names[i] << "\":{\"bytes\":" << b.bytes << ",\"resources\":" << b.resources
            << ",\"reusable_bytes\":" << b.reusable_bytes << ",\"reusable_resources\":" << b.reusable_resources
            << ",\"unknown_resources\":" << b.unknown_resources << '}';
        if(i<fg_memory::swapchain_native)app_bytes+=b.bytes;else runtime_bytes+=b.bytes;
        unknown+=b.unknown_resources;
    }
    out << "},\"app_resource_bytes\":" << app_bytes << ",\"swapchain_estimated_bytes\":" << runtime_bytes
        << ",\"known_resource_bytes\":" << app_bytes+runtime_bytes << ",\"unknown_resources\":" << unknown << '}';return out.str();
}
// DllMain detach uses only these atomics, never state() construction or a lock.
std::atomic<bool> adapter_attached{};
std::atomic<std::uint64_t> active_attachment{};

const char* host_id(CheekyRuntimeHost host) noexcept {
    switch (host) {
    case CheekyRuntimeHost::uevr: return "uevr";
    case CheekyRuntimeHost::standalone: return "standalone";
    case CheekyRuntimeHost::optiscaler: return "optiscaler";
    }
    return "unknown";
}
const char* host_label(CheekyRuntimeHost host) noexcept {
    switch (host) {
    case CheekyRuntimeHost::uevr: return "UEVR plugin";
    case CheekyRuntimeHost::standalone: return "Standalone";
    case CheekyRuntimeHost::optiscaler: return "OptiScaler integration";
    }
    return "Unknown host";
}

std::string snapshot_locked(State& s) {
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::boolalpha << std::setprecision(6);
    const auto observer = native_observer_status();
    const auto gaze = gaze_diagnostics();
    const auto views = stereo_view_statistics();
    const auto details = stereo_view_details();
    const auto quad=quad_view_snapshot();const bool app_quad=quad.active&&quad_is_app_snapshot(quad);
    CheekyQuadFgStatsV1 quad_fg;const bool quad_guides=quad.active&&!app_quad&&quad_fg_stats(quad_fg);
    const auto receiving_views = std::count_if(details.begin(), details.end(),
        [](const auto& view) { return view.recently_evaluated; });
    const auto evaluated_views = std::count_if(details.begin(), details.end(),
        [](const auto& view) { return view.evaluation_calls != 0; });
    const auto nr = dlss_nr_snapshot();
    const auto attach = late_attach_status();
    const auto afw = afw_compatibility_status();
    const auto afw_projection = afw_stereo_projection();
    const auto afw_coverage = afw_coverage_status();
    const auto frame = diagnostic_snapshot(DiagnosticApi::d3d11);
    const auto gpu = gpu_timing_status();
    const auto fg = fg_status();
    s.memory.poll(GetTickCount64());
    sample_fg_memory(s,GetTickCount64());
    CheekyFgPerformance fg_time;const bool fg_time_supported=fg_performance(fg_time);
    CheekyFgCadence fg_rates;fg_cadence(fg_rates);
    CheekyFgPacingTimeline fg_pacing;fg_pacing_timeline(fg_pacing);
    CheekyFgNativeSubmissionLatency fg_native_latency;fg_native_submission_latency(fg_native_latency);
    CheekyFgOverhead fg_extra;fg_overhead(fg_extra);
    CheekyFgPresentation fg_present;fg_presentation(fg_present);
    CheekyFgExperimentDiagnostics fg_exp;fg_experiment_diagnostics(fg_exp);
    CheekyFgDirectOutput fg_direct;fg_direct_output_diagnostics(fg_direct);
    CheekyFgDirectOutputDetails fg_direct_details;fg_direct_output_details(fg_direct_details);
    CheekyFgDirectShading fg_shading;fg_direct_shading_diagnostics(fg_shading);
    CheekyFgCaptureHealth fg_capture;
    if(auto self=GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll"))
        if(auto query=reinterpret_cast<CheekyFgCaptureHealthFn>(GetProcAddress(self,"CheekyFG_GetCaptureHealth")))query(&fg_capture);
    out << "{\"protocol\":1,\"version\":\"" CHEEKY_VERSION "-" << host_id(s.host)
        << "\",\"host\":\"" << host_id(s.host)
        << "\",\"host_supports_afw_projection\":" << (s.host == CheekyRuntimeHost::uevr)
        << ",\"request\":" << s.request
        << ",\"revision\":" << s.revision << ",\"saved_revision\":" << s.saved_revision
        << ",\"applied_request\":" << s.applied_request
        << ",\"attached\":" << adapter_attached.load() << ",\"ready\":" << (s.started && s.graphics_ready)
        << ",\"processing\":" << current_settings().enabled
        << ",\"renderer\":" << s.renderer << ",\"message\":\"" << json_escape(s.message)
        << "\",\"settings\":" << settings_json(configured_settings())
        << ",\"setting_groups\":" << setting_groups_json();
    const auto& memory=s.memory;
    out << ",\"gpu_memory\":{\"supported\":" << bool(memory.adapter)
        << ",\"scope\":\"process\",\"adapter\":\"" << json_escape(memory.name) << "\",\"node\":" << memory.node
        << ",\"dedicated_capacity_bytes\":" << memory.dedicated_capacity << ",\"samples\":" << memory.samples
        << ",\"sampled_at_tick\":" << memory.sampled_at << ",\"sample_age_ms\":" << GetTickCount64()-memory.sampled_at
        << ",\"adapter_result\":" << static_cast<unsigned long>(memory.adapter_result);
    const auto memory_segment=[&](const char* name,const GpuMemory::Segment& m){
        out << ",\"" << name << "\":{\"valid\":" << m.valid << ",\"result\":" << static_cast<unsigned long>(m.result)
            << ",\"usage_bytes\":" << m.usage << ",\"budget_bytes\":" << m.budget << ",\"headroom_bytes\":" << m.headroom
            << ",\"over_budget_bytes\":" << m.over_budget << ",\"peak_bytes\":" << m.peak << ",\"over_budget_samples\":" << m.over_budget_samples << "}";
    };
    memory_segment("local",memory.local);memory_segment("nonlocal",memory.nonlocal);
    out << ",\"baseline\":{\"valid\":" << memory.baseline_valid << ",\"nonlocal_valid\":" << memory.baseline_nonlocal_valid
        << ",\"fg_enabled\":" << memory.baseline_fg_enabled << ",\"tick\":" << memory.baseline_at
        << ",\"local_bytes\":" << memory.baseline_local << ",\"nonlocal_bytes\":" << memory.baseline_nonlocal
        << ",\"local_delta_bytes\":" << (memory.baseline_valid&&memory.local.valid?std::int64_t(memory.local.usage)-std::int64_t(memory.baseline_local):0)
        << ",\"nonlocal_delta_bytes\":" << (memory.baseline_nonlocal_valid&&memory.nonlocal.valid?std::int64_t(memory.nonlocal.usage)-std::int64_t(memory.baseline_nonlocal):0) << "}"
        << ",\"fg_resources\":" << fg_memory_json(s.fg_memory) << "}"
        << ",\"fg\":{\"layer_present\":" << bool(fg.layer_present)
        << ",\"controls_supported\":" << bool(fg.controls_supported)
        << ",\"compatible_session\":" << bool(fg.compatible_session)
        << ",\"active\":" << bool(fg.active)
        << ",\"state\":\"" << fg_state_name(fg.state) << "\""
        << ",\"diagnostic\":\"" << json_escape(app_quad?"FG disabled: game composites four DLSS views before stereo OpenXR submission":fg_diagnostic().message) << "\""
        << ",\"native_frames\":" << fg.native_frames
        << ",\"generated_frames\":" << fg.generated_frames
        << ",\"fallback_frames\":" << fg.fallback_frames
        << ",\"frame_source\":\"" << json_escape(quad_guides?"Quad D3D11 raw-view capture":fg_capture.source) << "\""
        << ",\"frame_source_hook\":" << bool(fg_capture.plugin_hook)
        << ",\"frame_source_calls\":" << (quad_guides?quad_fg.frames:fg_capture.frame_calls)
        << ",\"captured_guides\":" << (quad_guides?quad_fg.snapshots:fg_capture.captured_guides)
        << ",\"missing_frame_guides\":" << fg_capture.missing_frame_guides
        << ",\"last_guide_frame\":" << fg_capture.last_guide_frame
        << ",\"color_copies\":" << fg_capture.color_copies
        << ",\"last_color_frame\":" << fg_capture.color_frame
        << ",\"latest_captured_frame\":" << fg_capture.latest_frame
        << ",\"eligible_guides\":" << fg_capture.eligible
        << ",\"pending_guides\":" << fg_capture.pending
        << ",\"foreign_guides\":" << fg_capture.foreign
        << ",\"match_reason\":\"" << json_escape(quad_guides?(quad_fg.assembly_stage==4?"Quad guide assembly ready":"Waiting for four current view guides"):fg_capture.match_reason) << "\""
        << ",\"performance\":{\"supported\":" << fg_time_supported
        << ",\"gpu_supported\":" << bool(fg_time.gpu_supported)
        << ",\"gpu_valid\":" << bool(fg_time.gpu_valid)
        << ",\"foveated_eyes\":" << fg_time.foveated_eyes
        << ",\"window_samples\":" << fg_time.window_samples
        << ",\"gpu_samples\":" << fg_time.gpu_samples
        << ",\"center_ms\":" << fg_time.center_ms << ",\"center_last_ms\":" << fg_time.center_last_ms
        << ",\"periphery_ms\":" << fg_time.periphery_ms << ",\"periphery_last_ms\":" << fg_time.periphery_last_ms
        << ",\"preparation_ms\":" << fg_time.preparation_ms << ",\"preparation_last_ms\":" << fg_time.preparation_last_ms
        << ",\"total_ms\":" << fg_time.total_ms << ",\"total_last_ms\":" << fg_time.total_last_ms
        << ",\"completion_wait_ms\":" << fg_time.completion_wait_ms
        << ",\"copy_wait_ms\":" << fg_time.copy_wait_ms
        << ",\"xr_wait_ms\":" << fg_time.xr_wait_ms
        << ",\"xr_period_ms\":" << fg_time.xr_period_ms
        << ",\"asynchronous_presenter\":" << bool(fg_time.asynchronous_presenter)
        << ",\"queued_native\":" << fg_time.queued_native << ",\"skipped_native\":" << fg_time.skipped_native
        << ",\"producer_cpu_ms\":" << fg_time.producer_cpu_ms << ",\"pacing_wait_ms\":" << fg_time.pacing_wait_ms
        << ",\"color_token\":" << fg_time.color_token << ",\"guide_token\":" << fg_time.guide_token
        << ",\"mismatched_guides\":" << fg_time.mismatched_guides
        << ",\"color_render_frame\":" << fg_time.color_render_frame << ",\"guide_render_frame\":" << fg_time.guide_render_frame
        << ",\"inference_color_format\":" << fg_time.inference_color_format << ",\"shared_gpu_queue\":" << fg_time.shared_gpu_queue
        << ",\"center_width\":" << fg_time.center_width << ",\"center_height\":" << fg_time.center_height
        << ",\"peripheral_width\":" << fg_time.peripheral_width << ",\"peripheral_height\":" << fg_time.peripheral_height
        << ",\"output_width\":" << fg_time.output_width << ",\"output_height\":" << fg_time.output_height
        << ",\"peripheral_method\":" << fg_time.peripheral_method
        << ",\"source_depth_format\":" << fg_time.source_depth_format
        << ",\"source_motion_format\":" << fg_time.source_motion_format << "}"
        << ",\"pacing_timeline\":{\"supported\":" << bool(fg_pacing.supported)
        << ",\"recent_generated\":" << bool(fg_pacing.recent_generated)
        << ",\"generated_last_tick\":" << fg_pacing.generated_last_tick
        << ",\"deferred_events\":" << fg_pacing.deferred_events << ",\"deferred_releases\":" << fg_pacing.deferred_releases
        << ",\"clock_late_ms\":" << fg_pacing.clock_late_ms << ",\"clock_late_last_ms\":" << fg_pacing.clock_late_last_ms
        << ",\"credit_deferred_ms\":" << fg_pacing.credit_deferred_ms << ",\"credit_deferred_last_ms\":" << fg_pacing.credit_deferred_last_ms
        << ",\"game_wait_credit_ms\":" << fg_pacing.game_wait_credit_ms << ",\"game_wait_credit_last_ms\":" << fg_pacing.game_wait_credit_last_ms
        << ",\"game_begin_gate_ms\":" << fg_pacing.game_begin_gate_ms << ",\"game_begin_gate_last_ms\":" << fg_pacing.game_begin_gate_last_ms
        << ",\"presenter_content_ms\":" << fg_pacing.presenter_content_ms << ",\"presenter_content_last_ms\":" << fg_pacing.presenter_content_last_ms
        << ",\"native_xr_end_ms\":" << fg_pacing.native_xr_end_ms << ",\"native_xr_end_last_ms\":" << fg_pacing.native_xr_end_last_ms
        << ",\"generated_xr_end_ms\":" << fg_pacing.generated_xr_end_ms << ",\"generated_xr_end_last_ms\":" << fg_pacing.generated_xr_end_last_ms
        << ",\"fg_submit_to_complete_ms\":" << fg_pacing.fg_submit_to_complete_ms << ",\"fg_submit_to_complete_last_ms\":" << fg_pacing.fg_submit_to_complete_last_ms << "}"
        << ",\"native_submission_latency\":{\"supported\":" << bool(fg_native_latency.supported)
        << ",\"enabled\":" << bool(fg_native_latency.enabled)
        << ",\"gpu_samples\":" << fg_native_latency.gpu_samples
        << ",\"missing_stamps\":" << fg_native_latency.missing_stamps
        << ",\"pending\":" << fg_native_latency.pending
        << ",\"held_samples\":" << fg_native_latency.held_samples
        << ",\"early_samples\":" << fg_native_latency.early_samples
        << ",\"gpu_ready_to_submit_ms\":" << fg_native_latency.gpu_ready_to_submit_ms
        << ",\"gpu_ready_to_submit_last_ms\":" << fg_native_latency.gpu_ready_to_submit_last_ms
        << ",\"native_hold_ms\":" << fg_native_latency.native_hold_ms
        << ",\"native_hold_last_ms\":" << fg_native_latency.native_hold_last_ms
        << ",\"submit_ahead_ms\":" << fg_native_latency.submit_ahead_ms
        << ",\"submit_ahead_last_ms\":" << fg_native_latency.submit_ahead_last_ms
        << ",\"capture_to_submit_ms\":" << fg_native_latency.capture_to_submit_ms
        << ",\"capture_to_submit_last_ms\":" << fg_native_latency.capture_to_submit_last_ms << "}"
        << ",\"cadence\":{\"supported\":" << bool(fg_rates.supported) << ",\"valid\":" << bool(fg_rates.valid)
        << ",\"game_fps\":" << fg_rates.game_fps << ",\"native_fps\":" << fg_rates.native_fps
        << ",\"generated_fps\":" << fg_rates.generated_fps << ",\"unique_fps\":" << fg_rates.unique_fps
        << ",\"repeat_fps\":" << fg_rates.repeat_fps << ",\"submission_fps\":" << fg_rates.submission_fps
        << ",\"refresh_hz\":" << fg_rates.refresh_hz << ",\"native_target_fps\":" << fg_rates.native_target_fps
        << ",\"application_period_ms\":" << fg_rates.application_period_ms
        << ",\"app_image_wait_ms\":" << fg_rates.app_image_wait_ms << ",\"present_image_wait_ms\":" << fg_rates.present_image_wait_ms
        << ",\"begin_ms\":" << fg_rates.begin_ms << ",\"end_ms\":" << fg_rates.end_ms
        << ",\"present_cpu_ms\":" << fg_rates.present_cpu_ms
        << ",\"app_begin_ms\":" << fg_rates.app_begin_ms << ",\"app_end_ms\":" << fg_rates.app_end_ms
        << ",\"app_release_ms\":" << fg_rates.app_release_ms << "}"
        << ",\"overhead\":{\"supported\":" << bool(fg_extra.supported)
        << ",\"guide_samples\":" << fg_extra.guide_samples << ",\"capture_samples\":" << fg_extra.capture_samples
        << ",\"publication_samples\":" << fg_extra.publication_samples << ",\"upload_samples\":" << fg_extra.upload_samples
        << ",\"guide_ms\":" << fg_extra.guide_ms << ",\"guide_last_ms\":" << fg_extra.guide_last_ms
        << ",\"capture_ms\":" << fg_extra.capture_ms << ",\"capture_last_ms\":" << fg_extra.capture_last_ms
        << ",\"publication_ms\":" << fg_extra.publication_ms << ",\"publication_last_ms\":" << fg_extra.publication_last_ms
        << ",\"upload_ms\":" << fg_extra.upload_ms << ",\"upload_last_ms\":" << fg_extra.upload_last_ms << "}"
        << ",\"direct_output\":{\"supported\":" << bool(fg_direct.supported) << ",\"requested\":" << bool(fg_direct.requested)
        << ",\"last_path\":" << fg_direct.last_path << ",\"attempts\":" << fg_direct.attempts << ",\"frames\":" << fg_direct.frames
        << ",\"ngx_direct_eyes\":" << fg_direct.ngx_direct_eyes << ",\"ngx_scratch_eyes\":" << fg_direct.ngx_scratch_eyes
        << ",\"fallbacks\":" << fg_direct.fallbacks << ",\"unsupported\":" << fg_direct.unsupported << ",\"not_ready\":" << fg_direct.not_ready
        << ",\"wait_ms\":" << fg_direct.wait_ms << ",\"last_wait_ms\":" << fg_direct.last_wait_ms
        << ",\"details_supported\":" << bool(fg_direct_details.supported) << ",\"reason\":" << unsigned(fg_direct_details.reason)
        << ",\"reason_text\":\"" << cheeky_fg_direct_reason_name(fg_direct_details.reason) << "\",\"eye\":" << fg_direct_details.eye
        << ",\"expected_format\":" << fg_direct_details.expected_format << ",\"chain_format\":" << fg_direct_details.chain_format
        << ",\"image_format\":" << fg_direct_details.image_format << ",\"image_flags\":" << fg_direct_details.image_flags
        << ",\"expected_width\":" << fg_direct_details.expected_width << ",\"expected_height\":" << fg_direct_details.expected_height
        << ",\"chain_width\":" << fg_direct_details.chain_width << ",\"chain_height\":" << fg_direct_details.chain_height
        << ",\"image_width\":" << fg_direct_details.image_width << ",\"image_height\":" << fg_direct_details.image_height
        << ",\"api_result\":" << fg_direct_details.xr_result
        << ",\"shading\":{\"supported\":" << bool(fg_shading.supported) << ",\"valid\":" << bool(fg_shading.valid)
        << ",\"requested_mode\":" << fg_shading.requested_mode << ",\"active_mode\":" << fg_shading.active_mode
        << ",\"grid_width\":" << fg_shading.grid_width << ",\"grid_height\":" << fg_shading.grid_height
        << ",\"output_width\":" << fg_shading.output_width << ",\"output_height\":" << fg_shading.output_height
        << ",\"coarse_frames\":" << fg_shading.coarse_frames << ",\"full_frames\":" << fg_shading.full_frames << "}}"
        << ",\"experiments\":{\"supported\":" << bool(fg_exp.supported)
        << ",\"color_capture_mode\":" << fg_exp.color_capture_mode << ",\"upload_mode\":" << fg_exp.upload_mode
        << ",\"latency_enabled\":" << bool(fg_exp.latency_enabled) << ",\"latency_valid\":" << bool(fg_exp.latency_valid)
        << ",\"full_color_copies\":" << fg_exp.full_color_copies << ",\"direct_eye_copies\":" << fg_exp.direct_eye_copies
        << ",\"eye_crop_copies\":" << fg_exp.eye_crop_copies << ",\"capture_fallbacks\":" << fg_exp.capture_fallbacks
        << ",\"upload_copies\":" << fg_exp.upload_copies << ",\"upload_batches\":" << fg_exp.upload_batches
        << ",\"latency_samples\":" << fg_exp.latency_samples << ",\"latency_rejected\":" << fg_exp.latency_rejected
        << ",\"display_time_supported\":" << bool(fg_exp.display_time_supported)
        << ",\"gpu_ready_samples\":" << fg_exp.gpu_ready_samples << ",\"predicted_samples\":" << fg_exp.predicted_samples
        << ",\"prediction_late\":" << fg_exp.prediction_late << ",\"generated_gate_releases\":" << fg_exp.generated_gate_releases
        << ",\"generated_gate_skips\":" << fg_exp.generated_gate_skips;
    const auto timing=[&](const char* name,const CheekyFgTimingValue& value){out << ",\"" << name << "\":{\"mean\":" << value.mean << ",\"last\":" << value.last << "}";};
    timing("queue_entry",fg_exp.queue_entry);timing("capture_dependency",fg_exp.capture_dependency);
    timing("ready_to_fg",fg_exp.ready_to_fg);timing("gpu_work",fg_exp.gpu_work);
    timing("gpu_to_observed",fg_exp.gpu_to_observed);timing("enqueue_to_observed",fg_exp.enqueue_to_observed);
    timing("submit_to_observed",fg_exp.submit_to_observed);
    timing("native_gpu_tail",fg_exp.native_gpu_tail);timing("gpu_ready_to_submit",fg_exp.gpu_ready_to_submit);
    timing("gpu_ready_to_display",fg_exp.gpu_ready_to_display);
    out << "}"
        << ",\"presentation\":{\"supported\":" << bool(fg_present.supported)
        << ",\"native_samples\":" << fg_present.native_samples << ",\"generated_samples\":" << fg_present.generated_samples
        << ",\"repeat_samples\":" << fg_present.repeat_samples << ",\"wait_samples\":" << fg_present.wait_samples
        << ",\"wait_arrivals\":" << fg_present.wait_arrivals << ",\"wait_timeouts\":" << fg_present.wait_timeouts
        << ",\"native_age_ms\":" << fg_present.native_age_ms << ",\"native_age_last_ms\":" << fg_present.native_age_last_ms
        << ",\"generated_age_ms\":" << fg_present.generated_age_ms << ",\"generated_age_last_ms\":" << fg_present.generated_age_last_ms
        << ",\"newest_input_age_ms\":" << fg_present.newest_input_age_ms << ",\"newest_input_age_last_ms\":" << fg_present.newest_input_age_last_ms
        << ",\"repeat_age_ms\":" << fg_present.repeat_age_ms << ",\"repeat_age_last_ms\":" << fg_present.repeat_age_last_ms
        << ",\"late_wait_ms\":" << fg_present.late_wait_ms << ",\"late_wait_last_ms\":" << fg_present.late_wait_last_ms
        << ",\"wait_budget_ms\":" << fg_present.wait_budget_ms << "}}"
        << ",\"d3d12_lower_hook_active\":" << d3d12_lower_hook_enabled()
        << ",\"d3d12_hook_restart_required\":" << d3d12_hook_restart_required(configured_settings().d3d12_lower_hook)
        << ",\"afw_experiment\":{\"enabled\":" << afw.enabled
        << ",\"core_calls\":" << afw.core_calls << ",\"lower_calls\":" << afw.lower_calls
        << ",\"missing_lower_calls\":" << afw.missing_lower_calls
        << ",\"standalone_lower_calls\":" << afw.standalone_lower_calls
        << ",\"rejected_core_reentry\":" << afw.rejected_core_reentry
        << ",\"runtime_candidates\":" << afw.runtime_candidates << ",\"runtime_selected\":" << afw.runtime_selected
        << ",\"warp_observer_ready\":" << afw.warp_observer_ready << ",\"warp_calls\":" << afw.warp_calls
        << ",\"last_warp_age_ms\":" << (afw.last_warp_age_ms == UINT64_MAX ? -1LL : static_cast<long long>(afw.last_warp_age_ms))
        << ",\"warp_metadata_supported\":" << afw.warp_metadata_supported
        << ",\"last_warp_source_eye\":" << (afw.last_warp_source_eye < 2 ? static_cast<int>(afw.last_warp_source_eye) : -1)
        << ",\"last_warp_mode\":" << (afw.last_warp_mode <= 3 ? static_cast<int>(afw.last_warp_mode) : -1)
        << ",\"source_left_calls\":" << afw.source_left_calls << ",\"source_right_calls\":" << afw.source_right_calls
        << ",\"coverage_enabled\":" << afw.coverage_enabled << ",\"rendering_mode_known\":" << afw.rendering_mode_known
        << ",\"rendering_mode\":" << (afw.rendering_mode_known ? static_cast<int>(afw.rendering_mode) : -1)
        << ",\"last_evaluation_eye\":" << (afw.last_evaluation_eye < 2 ? static_cast<int>(afw.last_evaluation_eye) : -1)
        << ",\"early_left_calls\":" << afw.early_left_calls << ",\"early_right_calls\":" << afw.early_right_calls
        << ",\"early_unknown_calls\":" << afw.early_unknown_calls
        << ",\"projection_valid\":" << afw_projection.valid
        << ",\"projection_width\":" << afw_projection.output_width << ",\"projection_height\":" << afw_projection.output_height
        << ",\"coverage_observed\":" << afw_coverage.observed << ",\"coverage_mode\":" << afw_coverage.mode
        << ",\"manual_coverage\":" << (afw_coverage.mode == 1U)
        << ",\"effective_width\":" << afw_coverage.width << ",\"effective_height\":" << afw_coverage.height
        << ",\"effective_x_offset\":" << afw_coverage.x_offset << ",\"effective_height_offset\":" << afw_coverage.height_offset
        << ",\"effective_center_scale\":" << afw_coverage.center_scale << '}'
        << ",\"eye_calibration\":" << eye_calibration_json()
        << ",\"libovr\":" << libovr_gaze_json()
        << ",\"support\":{\"busy\":" << s.report_busy.load() << ",\"zip\":\"" << json_escape(path_utf8(s.report_zip)) << "\"}"
        << ",\"gpu_timing\":{\"recorded\":" << gpu.recorded << ",\"submitted\":" << gpu.submitted
        << ",\"completed\":" << gpu.completed << ",\"published\":" << gpu.published
        << ",\"discarded\":" << gpu.discarded << ",\"failures\":" << gpu.failures
        << ",\"waiting_submission\":" << gpu.waiting_submission << ",\"waiting_gpu\":" << gpu.waiting_gpu
        << ",\"last_error\":" << gpu.last_error << '}'
        << ",\"frame\":{\"present_ms\":" << s.cadence.average_ms
        << ",\"sr_enabled_ms\":" << frame.foveated_frame_ms << ",\"sr_disabled_ms\":" << frame.native_frame_ms << '}'
        << ",\"late_attach\":{\"options_hooked\":" << attach.streamline_options_hooked
        << ",\"options_seen\":" << attach.streamline_options_seen
        << ",\"native_fallback\":" << attach.streamline_native_fallback
        << ",\"fallback_calls\":" << attach.streamline_fallback_calls << '}'
        << ",\"observer\":{\"ready\":" << observer.ready << ",\"submissions\":" << observer.submissions
        << ",\"copies\":" << observer.copies << ",\"resets\":" << observer.resets << ",\"destroyed\":" << observer.destroyed << '}'
        << ",\"gaze\":{\"layer\":" << gaze.layer_present << ",\"abi\":" << gaze.abi_compatible
        << ",\"input\":" << gaze_input_diagnostics_json(gaze.input)
        << ",\"using_gaze\":" << gaze.using_gaze << ",\"alignment\":" << gaze.alignment_source
        << ",\"afw_bilateral\":" << gaze.afw_bilateral << ",\"afw_fresh_sample\":" << gaze.afw_fresh_sample
        << ",\"ambiguous\":" << gaze.mapping_ambiguous << ",\"views\":" << views.active
        << ",\"registered_views\":" << details.size()
        << ",\"receiving_views\":" << receiving_views
        << ",\"evaluated_views\":" << evaluated_views
        << ",\"never_evaluated_views\":" << details.size() - evaluated_views
        << ",\"activity_window_ms\":2000"
        << ",\"submitted_copies\":" << gaze.submitted_copies
        << ",\"left_mapped\":" << gaze.views[0].resource_mapped << ",\"right_mapped\":" << gaze.views[1].resource_mapped
        << ",\"runtime\":\"" << json_escape(gaze.runtime_name) << "\",\"age_ms\":" << gaze.sample_age_ms
        << ",\"status_flags\":" << gaze.status_flags << ",\"reset_reason\":" << static_cast<unsigned>(gaze.last_reset_reason)
        << ",\"peak_views\":" << views.peak << ",\"seen_views\":" << views.seen << ",\"eyes\":[";
    for (unsigned i = 0; i < 2; ++i) {
        if (i) out << ',';
        const auto& v = gaze.views[i];
        out << "{\"center_u\":" << v.center_u << ",\"center_v\":" << v.center_v
            << ",\"view_id\":\"" << v.dlss_view_id << "\",\"stable_matches\":" << v.stable_matches
            << ",\"delta_x\":" << v.crop_delta_x << ",\"delta_y\":" << v.crop_delta_y
            << ",\"mapped\":" << v.resource_mapped << ",\"packed\":" << v.packed_stereo_mapping
            << ",\"layout\":" << v.layout_mapping
            << ",\"copy\":" << v.copy_mapping << ",\"projection\":" << v.projection_mapping
            << ",\"marker\":" << v.marker_mapping
            << ",\"alignment\":" << v.alignment_source
            << ",\"aligned_u\":" << v.aligned_u << ",\"aligned_v\":" << v.aligned_v
            << ",\"submitted_projection\":" << v.submitted_projection
            << ",\"fov_tangents\":[" << v.fov_tangents[0] << ',' << v.fov_tangents[1]
            << ',' << v.fov_tangents[2] << ',' << v.fov_tangents[3] << "]}";
    }
    out << "]},\"nr\":\"" << json_escape(dlss_nr_state_name(nr.state)) << "\",\"nr_details\":{"
        << "\"route\":\"" << json_escape(dlss_nr_route_name(nr.route)) << "\",\"candidates\":" << nr.candidate_calls
        << ",\"evaluations\":" << nr.evaluation_calls << ",\"failures\":" << nr.failed_calls << ",\"result\":" << nr.last_result
        << ",\"output_width\":" << nr.output_width << ",\"output_height\":" << nr.output_height
        << ",\"processing_order\":" << static_cast<unsigned>(nr.processing_order)
        << ",\"processing_width\":" << nr.processing_width << ",\"processing_height\":" << nr.processing_height
        << ",\"hdr_input\":" << (nr.hdr_input ? "true" : "false")
        << ",\"skip_reason\":\"" << json_escape(nr.skip_reason ? nr.skip_reason : "") << "\""
        << ",\"region_width\":" << nr.region_width << ",\"region_height\":" << nr.region_height
        << ",\"region_x\":" << nr.region_base_x << ",\"region_y\":" << nr.region_base_y
        << ",\"working_width\":" << nr.working_width << ",\"working_height\":" << nr.working_height
        << ",\"codec_creations\":" << nr.codec_creations
        << ",\"border_creations\":" << nr.border_creations
        << ",\"resource_rebinds\":" << nr.resource_rebinds
        << ",\"vram_bytes\":" << nr.intermediate_vram_bytes << "}"
        << ",\"nvidia_override_status\":\"" << diagnostic_dlss_override_status() << "\",\"apis\":[";
    for (unsigned index = 0; index < 2; ++index) {
        const auto d = diagnostic_snapshot(index ? DiagnosticApi::d3d12 : DiagnosticApi::d3d11);
        if (index) out << ',';
        out << "{\"state\":\"" << json_escape(diagnostic_state_name(d.state)) << "\",\"hook\":" << d.hook_discovered
            << ",\"creates\":" << d.create_calls << ",\"evaluations\":" << d.evaluate_calls << ",\"active\":" << d.active_calls
            << ",\"reconstruction_feature\":" << d.reconstruction_feature
            << ",\"input_width\":" << d.received_input_width << ",\"input_height\":" << d.received_input_height
            << ",\"output_width\":" << d.received_output_width << ",\"output_height\":" << d.received_output_height
            << ",\"foveated_ms\":" << d.foveated_dlss_gpu_ms << ",\"native_ms\":" << d.native_dlss_gpu_ms
            << ",\"native_peripheral_ms\":" << d.native_peripheral_dlss_gpu_ms
            << ",\"peripheral_ms\":" << d.peripheral_dlaa_gpu_ms << ",\"result\":" << d.last_result
            << ",\"transport_ms\":" << d.transport_gpu_ms
            << ",\"peripheral_preparation_ms\":" << (index ? 0.0F : d3d11_peripheral_dlaa_preparation_gpu_ms())
            << ",\"peripheral_total_ms\":" << (index ? 0.0F : d3d11_peripheral_dlaa_total_gpu_ms())
            << ",\"runtime_loaded\":" << d.runtime_loaded << ",\"streamline\":" << d.streamline_detected
            << ",\"direct_detour\":" << d.direct_detour_installed << ",\"has_private_result\":" << d.has_private_result
            << ",\"private_result\":" << d.last_private_result << ",\"nr_full_ms\":" << d.full_dlss_nr_gpu_ms
            << ",\"nr_foveated_ms\":" << d.foveated_dlss_nr_gpu_ms
            << ",\"before_nr_full_ms\":" << d.before_full_nr_gpu_ms
            << ",\"before_nr_foveated_ms\":" << d.before_foveated_nr_gpu_ms
            << ",\"before_pipeline_ms\":" << d.before_pipeline_gpu_ms
            << ",\"after_pipeline_ms\":" << d.after_pipeline_gpu_ms
            << ",\"motion_width\":" << d.motion_vector_width << ",\"motion_height\":" << d.motion_vector_height
            << ",\"motion_space\":\"" << motion_vector_space_name(d.motion_vector_space)
            << "\",\"execution_path\":\"" << json_escape(d3d11_execution_path_name(d.d3d11_execution_path))
            << "\",\"transport_status\":\"" << json_escape(d3d11_transport_status_name(d.d3d11_transport_status))
            << "\",\"ngx_route\":" << static_cast<unsigned>(d.d3d12_ngx_route)
            << ",\"crop\":{\"input_width\":" << d.passed_crop.input_width << ",\"input_height\":" << d.passed_crop.input_height
            << ",\"input_x\":" << d.passed_crop.input_base_x << ",\"input_y\":" << d.passed_crop.input_base_y
            << ",\"output_width\":" << d.passed_crop.output_width << ",\"output_height\":" << d.passed_crop.output_height
            << ",\"output_x\":" << d.passed_crop.output_base_x << ",\"output_y\":" << d.passed_crop.output_base_y << "}}";
    }
    const auto vk = vulkan_backend_status();
    out << "],\"vulkan\":{\"state\":\"" << json_escape(vk.reason)
        << "\",\"evaluations\":" << vk.calls << ",\"active\":" << vk.active
        << ",\"passthrough\":" << vk.passthrough << ",\"failed\":" << vk.failed
        << ",\"result\":" << vk.last_result << ",\"input_width\":" << vk.input_width
        << ",\"input_height\":" << vk.input_height << ",\"output_width\":" << vk.output_width
        << ",\"output_height\":" << vk.output_height << "},\"view_details\":[";
    // Bound the event size even in games that churn many view identities.
    for (std::size_t i = 0; i < (std::min)(details.size(), std::size_t{16}); ++i) {
        const auto& v = details[i];
        if (i) out << ',';
        out << "{\"id\":\"" << v.view_id << "\",\"eye\":\""
            << (v.has_eye_assignment ? (v.second_eye ? "Right" : "Left") : afw.coverage_enabled ? "Unknown (AFW source eye)" : "Unassigned")
            << "\",\"evaluations\":" << v.evaluations
            << ",\"generation\":" << v.generation
            << ",\"evaluation_calls\":" << v.evaluation_calls
            << ",\"activity\":\"" << (v.evaluation_calls ? (v.recently_evaluated ? "Receiving calls" : "Idle") : "Never evaluated") << '"'
            << ",\"last_evaluation_age_ms\":" << (v.evaluation_calls ? std::to_string(v.last_evaluation_age_ms) : "null")
            << ",\"api\":\"" << (v.graphics_api == 11 ? "DX11" : v.graphics_api == 12 ? "DX12" : v.graphics_api == 1 ? "Vulkan" : "Unknown") << '"'
            << ",\"mapping\":\"" << (v.quad_region ? (v.quad_manual ? "Manual quad assignment" : app_quad?"Default quad order":"Verified quad submission") : v.calibrated ? "Calibrated" : v.has_eye_assignment ? "Unverified eye assignment" : "Unassigned") << '"'
            << ",\"region\":\"" << (v.quad_region==1?"Periphery":v.quad_region==2?"Center":"Unknown") << "\",\"has_geometry\":" << v.has_geometry
            << ",\"input_width\":" << v.evaluation_input_width << ",\"input_height\":" << v.evaluation_input_height
            << ",\"output_width\":" << v.evaluation_output_width << ",\"output_height\":" << v.evaluation_output_height
            << ",\"crop_width\":" << v.crop.output_width << ",\"crop_height\":" << v.crop.output_height
            << ",\"crop_x\":" << v.crop.output_base_x << ",\"crop_y\":" << v.crop.output_base_y << '}';
    }
    out << "],\"view_details_total\":" << details.size() << ",\"quad_views\":"<<quad_view_diagnostics_json()<<'}'; return out.str();
}
DWORD WINAPI persistence_worker(void*) {
    auto& s = state();
    for (;;) {
        WaitForSingleObject(s.save_event, INFINITE);
        try {
            if (s.save_requested.exchange(false)) {
                Settings settings; std::uint64_t revision;
                { std::lock_guard lock(s.mutex); settings = configured_settings(); revision = s.revision; }
                std::string error;
                const auto success = write_settings_file(s.config, settings, error);
                std::lock_guard lock(s.mutex);
                if (success) s.saved_revision = revision;
                else { s.message = error; log_error(error.c_str()); }
            }
            if (s.report_requested.exchange(false)) {
                const auto calibration = eye_calibration_stats();
                const auto capture = request_calibration_images(calibration.enabled && calibration.runtime_active,
                    std::chrono::seconds(5), calibration.enabled ? "vr_not_active" : "calibration_disabled");
                auto images = collect_calibration_images(capture);
                std::string text, settings, summary;
                { std::lock_guard lock(s.mutex);
                    text = snapshot_locked(s); settings = serialize_settings(configured_settings());
                    const auto d = diagnostic_snapshot(s.renderer == 1 ? DiagnosticApi::d3d12 : DiagnosticApi::d3d11);
                    std::ostringstream details;
                    std::array<wchar_t, 32768> executable{};
                    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
                    const auto game = length && length < executable.size() ? path_utf8(std::filesystem::path(executable.data()).filename()) : "Unknown";
                    details << "Cheeky " CHEEKY_VERSION " " << host_label(s.host) << "\nGame: " << game << "\nRenderer: " << (s.renderer == 1 ? "DX12" : "DX11")
                        << "\nDLSS-SR: " << diagnostic_state_name(d.state) << "\nDLSS-NR: " << dlss_nr_state_name(dlss_nr_snapshot().state)
                        << "\nGPU ms (native / center / peripheral): " << d.native_dlss_gpu_ms << " / " << d.foveated_dlss_gpu_ms << " / " << d.peripheral_dlaa_gpu_ms
                        << "\n\nSettings:\n" << settings << "\nFull diagnostic snapshot and logs are in the attached ZIP.";
                    summary = details.str();
                }
                text.pop_back();
                text += ",\"stereo_capture\":" + images.diagnostics + '}';
                std::ofstream report(s.directory / L"CheekyFoveatedDLSS-diagnostics.json", std::ios::binary);
                report << text; report.close();
                if (!report) throw std::runtime_error("Could not write diagnostics JSON");
                const auto zip = create_runtime_support_bundle(s.directory, text, settings, summary, s.host, std::move(images.files));
                { std::lock_guard lock(s.mutex);
                    s.report_zip = zip; s.report_summary = summary;
                    s.message = "Support ZIP ready. Review the files, describe the problem on GitHub and attach the ZIP.";
                }
                if (s.report_browser.exchange(false)) s.report_open.fetch_or(3U);
                s.report_busy = false;
            }
            if (const auto action = s.report_open.exchange(0U)) {
                std::filesystem::path zip; std::string summary;
                { std::lock_guard lock(s.mutex); zip = s.report_zip; summary = s.report_summary; }
                if (!zip.empty()) {
                    bool ok = true;
                    if (action & 2U) {
                        const auto url = support_issue_url(zip, summary, s.host);
                        ok = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
                    }
                    if (action & 1U) {
                        const auto args = L"/select,\"" + zip.wstring() + L"\"";
                        ok = (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL)) > 32) && ok;
                    }
                    if (!ok) { std::lock_guard lock(s.mutex); s.message = "ZIP created, but browser or Explorer could not open. Use the displayed ZIP path."; }
                }
            }
        } catch (const std::exception& error) {
            s.report_busy = false; s.report_browser = false;
            std::lock_guard lock(s.mutex); s.message = std::string("Save/report failed: ") + error.what(); log_error(s.message.c_str());
        } catch (...) { s.report_busy = false; s.report_browser = false; log_error("Runtime persistence/report worker failed"); }
    }
}
void request_save(State& s) { s.save_requested = true; SetEvent(s.save_event); }
bool configure_graphics(State& s, std::uint32_t renderer, void* device, void* queue) {
    const auto previous_renderer = s.renderer;
    s.renderer = renderer;
    if (renderer > 2) {
        s.graphics_ready = false;
        s.memory.attach(renderer,nullptr,nullptr);
        s.message = "Unsupported renderer; processing paused";
        return false;
    }
    if (!device) {
        s.graphics_ready = false;
        s.memory.attach(renderer,nullptr,nullptr);
        s.observed_device = nullptr; s.observed_queue = nullptr;
        s.message = "Waiting for a graphics device";
        reset_gaze_foveation();
        return false;
    }
    if (previous_renderer == renderer && s.observed_device == device && s.observed_queue == queue && s.graphics_ready) return true;
    if (renderer == 1 && !initialize_native_observer(static_cast<ID3D12Device*>(device), static_cast<ID3D12CommandQueue*>(queue))) {
        s.graphics_ready = false;
        s.memory.attach(renderer,nullptr,nullptr);
        s.message = "D3D12 submission observer unavailable; processing paused";
        return false;
    }
    s.observed_device = device; s.observed_queue = queue; s.graphics_ready = true;
    s.memory.attach(renderer,device,queue);
    s.logged_memory_sample=0;
    s.fg_memory={};s.next_fg_memory_sample=0;
    s.message = "Ready. Enable DLSS in the game; complete future evaluations can be adopted after injection. Check diagnostics if processing is waiting.";
    return true;
}
}
void set_addon_modules(HMODULE, HMODULE) noexcept {}
void trace_event(const char* format, ...) noexcept {
    if (!format) return;
    auto& s = state(); std::lock_guard lock(s.log_mutex);
    if (s.log == INVALID_HANDLE_VALUE) return;
    char message[3072]{};
    va_list args; va_start(args, format); vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args); va_end(args);
    SYSTEMTIME now; GetLocalTime(&now); char line[3328]{};
    const auto n = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [T%lu] %s\r\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentThreadId(), message);
    if (n > 0) { DWORD written; WriteFile(s.log, line, static_cast<DWORD>((std::min)(static_cast<std::size_t>(n), sizeof(line)-1)), &written, nullptr); }
}
void close_trace_log() noexcept {} // The runtime and log stay resident until process exit.
void log_message(int level, const char* message) noexcept { trace_event("[%d] %s", level, message ? message : ""); }
void log_info(const char* message) noexcept { log_message(3, message); }
void log_warning(const char* message) noexcept { log_message(2, message); }
void log_error(const char* message) noexcept { log_message(1, message); }
}

using namespace cheeky::foveated_dlss;
extern "C" __declspec(dllexport) bool CheekyRuntime_Start(const CheekyRuntimeStart* input) {
    try {
        if (!input || input->size != sizeof(*input) || input->abi != cheeky_runtime_abi ||
            !input->config_directory || !*input->config_directory || !input->attachment) return false;
        *input->attachment = 0;
        if (input->renderer > 2 || static_cast<std::uint32_t>(input->host) > static_cast<std::uint32_t>(CheekyRuntimeHost::optiscaler)) return false;
        auto& s = state(); std::lock_guard lock(s.mutex);
        if (adapter_attached) { log_warning("Duplicate Cheeky host attachment rejected"); return false; }
        if (s.failed) return false;
        if (s.started && (s.host != input->host || s.directory != std::filesystem::path(input->config_directory))) {
            log_warning("Resident runtime cannot change host or configuration directory; restart the game");
            return false;
        }
        if (!s.started) {
            set_processing_allowed(false);
            s.host = input->host;
            fg_set_standalone_host(s.host != CheekyRuntimeHost::uevr);
            s.directory = input->config_directory;
            std::filesystem::create_directories(s.directory);
            s.config = s.directory / L"CheekyFoveatedDLSS.ini";
            s.log = CreateFileW((s.directory / runtime_log_filename(s.host)).c_str(), FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            HMODULE resident{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&CheekyRuntime_Start), &resident)) return false;
            trace_event("Cheeky " CHEEKY_VERSION " %s initializing; runtime remains resident until game exit", host_label(s.host));
            if (GetModuleHandleW(L"CheekyFoveatedDLSS.addon64") || !(s.owner = claim_processing_owner())) {
                s.message = "Another Cheeky integration is loaded. Keep one Cheeky integration and restart the game.";
                s.failed = true; log_error(s.message.c_str()); return false;
            }
            Settings settings;
            if (std::filesystem::exists(s.config)) {
                std::string error;
                if (!read_settings_file(s.config, settings, error)) {
                    s.message = error + "; fix or rename CheekyFoveatedDLSS.ini and restart";
                    s.failed = true; log_error(s.message.c_str()); return false;
                }
            }
            update_settings(settings); s.revision = 1;
            set_eye_calibration_learning(settings.eye_calibration_learned_method,
                settings.eye_calibration_learned_signature, settings.eye_calibration_learned_sessions);
            s.save_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!s.save_event) { s.failed = true; s.message = "Could not create persistence event"; return false; }
            HANDLE worker = CreateThread(nullptr, 0, persistence_worker, nullptr, 0, nullptr);
            if (!worker) { s.failed = true; s.message = "Could not create persistence worker"; return false; }
            CloseHandle(worker);
            enable_openvr_late_recovery(s.host != CheekyRuntimeHost::uevr);
            if (!start_interception()) { s.failed = true; s.message = "NGX interception initialization failed"; return false; }
            s.started = true;
        }
        configure_graphics(s, input->renderer, input->device, input->queue);
        active_attachment = ++s.attachment_sequence;
        *input->attachment = s.attachment_sequence;
        adapter_attached = true;
        if(s.host == CheekyRuntimeHost::uevr)fg_uevr_mode_attached();
        allow_afw_stereo_projection(s.host == CheekyRuntimeHost::uevr);
        eye_calibration_enable(!configured_settings().manual_stereo_mapping);
        s.cadence.reset();
        set_processing_allowed(s.graphics_ready);
        request_save(s);
        return true;
    } catch (...) { set_processing_allowed(false); return false; }
}
extern "C" __declspec(dllexport) bool CheekyUEVR_Start(const CheekyUEVRStart* input) {
    if (!input || input->size != sizeof(*input) || input->abi != cheeky_uevr_abi) return false;
    CheekyRuntimeStart generic;
    generic.config_directory = input->config_directory; generic.renderer = input->renderer;
    generic.device = input->device; generic.queue = input->queue; generic.attachment = input->attachment;
    generic.host = CheekyRuntimeHost::uevr;
    return CheekyRuntime_Start(&generic);
}
extern "C" __declspec(dllexport) void CheekyRuntime_Detach(std::uint64_t attachment) {
    if (!attachment || active_attachment.load(std::memory_order_acquire) != attachment) return;
    adapter_attached.store(false, std::memory_order_release);
    fg_abandon_frame_source();
    if(state().host == CheekyRuntimeHost::uevr)fg_uevr_mode_detached();
    allow_afw_stereo_projection(false);
    eye_calibration_suspend();
    set_processing_allowed(false);
}
extern "C" __declspec(dllexport) void CheekyUEVR_Detach(std::uint64_t attachment) { CheekyRuntime_Detach(attachment); }
extern "C" __declspec(dllexport) bool CheekyUEVR_AttachOpenVR(std::uint64_t attachment, void* compositor) {
    try {
        if (!attachment || !adapter_attached.load() || attachment != active_attachment.load()) return false;
        auto& s = state(); std::lock_guard lock(s.mutex);
        if (s.host != CheekyRuntimeHost::uevr || !adapter_attached.load() || attachment != active_attachment.load()) return false;
        return attach_openvr_compositor(compositor);
    } catch (...) { return false; }
}
extern "C" __declspec(dllexport) bool CheekyUEVR_PublishStereo(std::uint64_t attachment, const CheekyUEVRStereoProjection* input) {
    try {
        if (!input || input->size != sizeof(*input) || input->abi != 1) return false;
        auto& s = state(); std::lock_guard lock(s.mutex);
        if (!attachment || !adapter_attached.load() || attachment != active_attachment.load() || s.host != CheekyRuntimeHost::uevr) return false;
        publish_afw_stereo_projection(input->matrices, input->output_width, input->output_height,
            input->active == 1 && s.graphics_ready && s.renderer == 1);
        return true;
    } catch (...) { return false; }
}
extern "C" __declspec(dllexport) bool CheekyUEVR_PublishRenderingMode(std::uint64_t attachment, std::uint32_t mode) {
    try {
        auto& s = state(); std::lock_guard lock(s.mutex);
        if (!attachment || !adapter_attached.load() || attachment != active_attachment.load() || s.host != CheekyRuntimeHost::uevr) return false;
        const auto effective_mode=s.graphics_ready && s.renderer == 1 ? mode : UINT32_MAX;
        publish_afw_rendering_mode(effective_mode);
        fg_uevr_mode_published(effective_mode);
        return true;
    } catch (...) { return false; }
}
extern "C" __declspec(dllexport) void CheekyRuntime_Tick(std::uint64_t attachment, std::uint32_t renderer, void* device, void* queue) {
    try {
        if (!adapter_attached.load() || attachment != active_attachment.load()) return;
        auto& s = state(); std::lock_guard lock(s.mutex);
        if (!attachment || !adapter_attached.load() || attachment != active_attachment.load()) return;
        configure_graphics(s, renderer, device, queue);
        fg_set_uevr_queue(renderer == 1 ? static_cast<ID3D12CommandQueue*>(queue) : nullptr);
        if(!fg_requires_d3d12(current_settings()))retire_d3d11_fg_transport();
        if (!s.graphics_ready || renderer != 1) {
            const float empty[2][16]{};
            publish_afw_stereo_projection(empty, 0, 0, false);
        }
        eye_calibration_tick();
        static std::uint64_t saved_learning_revision{};
        const auto learning_revision = eye_calibration_learning_revision();
        if (learning_revision != saved_learning_revision) {
            saved_learning_revision = learning_revision; ++s.revision; request_save(s);
        }
        set_processing_allowed(s.started && s.graphics_ready);
        if (s.graphics_ready) {
            const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            const auto enabled = current_settings().enabled;
            const auto ms = s.cadence.sample(seconds, enabled);
            if (ms > 0) diagnostic_note_frame_rate(static_cast<float>(1000.0 / ms), enabled);
        } else s.cadence.reset();
        if (s.graphics_ready && renderer == 1) note_d3d12_present(nullptr);
        if(s.graphics_ready)s.memory.poll(GetTickCount64());
        if(s.graphics_ready&&s.memory.samples!=s.logged_memory_sample){
            s.logged_memory_sample=s.memory.samples;
            sample_fg_memory(s,GetTickCount64());
            const auto& m=s.memory;CheekyFgCadence fg_rates;fg_cadence(fg_rates);
            trace_event("GPU memory process tick=%llu node=%u fg_enabled=%u fg_active=%u queue_request=%u local_valid=%u local_usage=%llu local_budget=%llu local_headroom=%llu local_over=%llu local_peak=%llu local_over_samples=%llu nonlocal_valid=%u nonlocal_usage=%llu nonlocal_budget=%llu baseline_valid=%u baseline_fg=%u baseline_tick=%llu baseline_local=%llu game_fps=%.3f unique_fps=%.3f cadence_valid=%u present_fps=%.3f hr=0x%08lX/0x%08lX (bytes)",
                m.sampled_at,m.node,configured_settings().fg.enabled?1u:0u,fg_status().active,configured_settings().fg.queue_priority,
                m.local.valid?1u:0u,m.local.usage,m.local.budget,m.local.headroom,m.local.over_budget,m.local.peak,m.local.over_budget_samples,
                m.nonlocal.valid?1u:0u,m.nonlocal.usage,m.nonlocal.budget,m.baseline_valid?1u:0u,m.baseline_fg_enabled?1u:0u,
                m.baseline_at,m.baseline_local,fg_rates.game_fps,fg_rates.unique_fps,fg_rates.valid,s.cadence.average_ms>0?1000./s.cadence.average_ms:0.,
                static_cast<unsigned long>(m.local.result),static_cast<unsigned long>(m.nonlocal.result));
            trace_event("FG resource memory tick=%llu %s (allocation requirements; excludes NGX internals/driver overhead; not residency)",
                m.sampled_at,fg_memory_json(s.fg_memory).c_str());
        }
        if (s.graphics_ready && renderer == 1 && GetTickCount64() >= s.next_timing_log) {
            s.next_timing_log = GetTickCount64() + 5000;
            const auto gpu = gpu_timing_status();
            trace_event("GPU timing recorded=%llu submitted=%llu completed=%llu valid=%llu discarded=%llu waiting_submit=%u waiting_gpu=%u failures=%llu hr=0x%08X queue_submissions=%llu",
                gpu.recorded, gpu.submitted, gpu.completed, gpu.published, gpu.discarded,
                gpu.waiting_submission, gpu.waiting_gpu, gpu.failures, gpu.last_error, native_observer_status().submissions);
        }
        static bool was_down{};
        const bool down = (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_OEM_2) & 0x8000);
        if (down && !was_down) {
            auto settings = configured_settings(); settings.enabled = !settings.enabled; update_settings(settings); ++s.revision; request_save(s);
        }
        was_down = down;
        static bool nr_was_down{};
        const bool nr_down = (GetAsyncKeyState(VK_MENU) & 0x8000) &&
            (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_OEM_PERIOD) & 0x8000);
        if (nr_down && !nr_was_down) {
            auto settings = configured_settings();
            settings.nr_enabled = !settings.nr_enabled;
            update_settings(settings);
            ++s.revision;
            request_save(s);
            trace_event("DLSS-NR hotkey Alt+Shift+> toggled enabled=%s",
                settings.nr_enabled ? "yes" : "no");
        }
        nr_was_down = nr_down;
    } catch (...) { set_processing_allowed(false); }
}
extern "C" __declspec(dllexport) void CheekyUEVR_Tick(std::uint64_t attachment, std::uint32_t renderer, void* device, void* queue) {
    CheekyRuntime_Tick(attachment, renderer, device, queue);
}
extern "C" __declspec(dllexport) bool CheekyRuntime_Command(std::uint64_t attachment, const char* command) {
    try {
        if (!command || strnlen_s(command, 8193) > 8192 || !adapter_attached.load() || attachment != active_attachment.load()) return false;
        auto& s = state(); std::lock_guard lock(s.mutex);
        if (!attachment || !adapter_attached.load() || attachment != active_attachment.load()) return false;
        std::istringstream in(command); std::string protocol, request, action;
        if (!std::getline(in, protocol) || protocol != "1" || !std::getline(in, request) || !std::getline(in, action)) return false;
        std::uint64_t id{};
        const auto parsed = std::from_chars(request.data(), request.data()+request.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != request.data()+request.size()) return false;
        s.request = id;
        if (action == "get") return true;
        if(action=="quad_assign"){
            uint64_t session{},source_id{},generation{};unsigned number{},role{};
            if(!(in>>session>>number>>role>>source_id>>generation)||!number||number>8||role>4)return false;
            in>>std::ws;if(!in.eof())return false;
            const bool ok=quad_view_assign(session,number,role,source_id,generation);
            s.message=ok?"Quad view assignment updated":"Role already assigned or view changed; refresh and retry";
            if(ok){auto settings=configured_settings();const auto quad=quad_view_snapshot();
                settings.quad_saved_roles={};settings.quad_saved_source_count=0;
                for(const auto& source:quad.sources)if(source.dlss_view&&source.number&&source.number<=8){
                    settings.quad_saved_roles[source.number-1]=source.manual_role;++settings.quad_saved_source_count;}
                update_settings(settings);++s.revision;request_save(s);}
            return ok;
        }
        if(action=="quad_recalibrate"){auto settings=configured_settings();settings.quad_saved_roles={};settings.quad_saved_source_count=0;
            update_settings(settings);quad_view_recalibrate();++s.revision;request_save(s);return true;}
        if (action == "mark_vram_baseline") {
            const bool marked=s.memory.mark_baseline(GetTickCount64(),configured_settings().fg.enabled);
            s.message=marked?"Process GPU memory baseline marked":"GPU memory query unavailable; baseline not marked";
            trace_event("GPU memory baseline marked=%u tick=%llu fg_enabled=%u local=%llu nonlocal=%llu (process bytes)",
                marked?1u:0u,s.memory.baseline_at,s.memory.baseline_fg_enabled?1u:0u,s.memory.baseline_local,s.memory.baseline_nonlocal);
            return marked;
        }
        if (action == "calibration_enable" || action == "calibration_disable") {
            eye_calibration_enable(action == "calibration_enable" && !configured_settings().manual_stereo_mapping); return true;
        }
        if (action == "calibration_reset") { eye_calibration_reset_stats(); return true; }
        if (action == "calibration_forget") {
            set_eye_calibration_learning(0, 0, 0); eye_calibration_recalibrate();
            ++s.revision; request_save(s); return true;
        }
        if (action == "calibration_recalibrate") { eye_calibration_recalibrate(); return true; }
        if (action == "report" || action == "report_issue") {
            if (s.report_busy.exchange(true)) return true;
            s.report_browser = action == "report_issue"; s.report_requested = true;
            SetEvent(s.save_event); s.message = "Capturing stereo diagnostics and preparing support ZIP (up to 5 seconds)"; return true;
        }
        if (action == "show_report" || action == "open_issue") {
            if (s.report_zip.empty()) { s.message = "Create a support ZIP first"; return false; }
            s.report_open.fetch_or(action == "show_report" ? 1U : 2U); SetEvent(s.save_event); return true;
        }
        if (action == "save") { request_save(s); return true; }
        if (action == "reset_nr") { reset_dlss_nr(); return true; }
        auto settings = configured_settings();
        const bool was_manual_mapping = settings.manual_stereo_mapping;
        if (action == "defaults") {
            settings = Settings{};
        }
        else if (action.starts_with("defaults_")) {
            if (!reset_settings_group(settings, std::string_view(action).substr(9))) {
                s.message = "Unknown settings group"; return false;
            }
        }
        else if (action == "set") {
            std::string line; unsigned count{};
            while (std::getline(in, line)) {
                if (line.empty()) continue;
                const auto eq = line.find('=');
                if (line.starts_with("EyeCalibrationLearned")) {
                    s.message = "Learned calibration is read-only; use Reset learned calibration method"; return false;
                }
                if (eq == std::string::npos || !set_named_setting(settings, std::string_view(line).substr(0,eq), std::string_view(line).substr(eq+1))) {
                    s.message = "Rejected invalid settings transaction"; return false;
                }
                if (++count > 64) { s.message = "Too many settings"; return false; }
            }
            if (!count) return false;
        } else { s.message = "Unknown command"; return false; }
        update_settings(settings);
        if (was_manual_mapping != settings.manual_stereo_mapping)
            eye_calibration_enable(!settings.manual_stereo_mapping);
        ++s.revision; s.applied_request = id;
        s.message = "Settings applied"; request_save(s); return true;
    } catch (...) { return false; }
}
extern "C" __declspec(dllexport) bool CheekyUEVR_Command(std::uint64_t attachment, const char* command) {
    return CheekyRuntime_Command(attachment, command);
}
extern "C" __declspec(dllexport) bool CheekyRuntime_Snapshot(char* output, std::uint32_t capacity) {
    try {
        if (!output || !capacity) return false;
        output[0] = 0; auto& s = state(); std::lock_guard lock(s.mutex);
        const auto text = snapshot_locked(s);
        if (text.size() >= capacity) return false;
        memcpy(output, text.c_str(), text.size()+1); return true;
    } catch (...) { return false; }
}
extern "C" __declspec(dllexport) bool CheekyUEVR_Snapshot(char* output, std::uint32_t capacity) {
    return CheekyRuntime_Snapshot(output, capacity);
}

extern "C" __declspec(dllexport) void CheekyRuntime_VulkanFrame(void* device, void* queue) {
    CheekyRuntime_Tick(active_attachment.load(), 2, device, queue);
}
