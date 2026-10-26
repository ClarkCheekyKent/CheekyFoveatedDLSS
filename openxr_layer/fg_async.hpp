#pragma once
#include "fg_frame_backend.hpp"
#include "fg_buffered_resources.hpp"
#include "fg_burst_capture.hpp"
#include "fg_native_submission_math.hpp"
#include "fg_display_clock.hpp"
#include "../src/fg_source_lifecycle.hpp"
#include <condition_variable>
#include <thread>
#include <atomic>
#include <optional>
#include <stdexcept>
#include <functional>

namespace cheeky::fg {
// The game owns capture/snapshot recording. Only the presentation thread calls
// the runtime frame functions. A separate worker waits for inference completion.
// Three native leases and two ready pairs bound latency and texture ownership.
struct BufferedPresenter final : Presenter, BufferedGpuResources {
    std::unique_ptr<FrameBackend> backend;
    BufferedPresenter();
    inline static thread_local const CheekyFgPair* quad_pair_for_call{};
    std::function<XrResult()> quad_native_fallback;
    XrResult end_quad_frame(const XrFrameEndInfo* end,const XrCompositionLayerProjection* projection,const CheekyFgPair& pair,
        std::function<XrResult()> fallback={}) noexcept {
        quad_native_fallback=std::move(fallback);
        const auto* prior=quad_pair_for_call;quad_pair_for_call=&pair;
        const auto result=end_frame(end,{},projection);quad_pair_for_call=prior;quad_native_fallback={};return result;
    }
    bool capture_completed(std::uint64_t ready) const {return backend->ready(ready);}
    std::condition_variable_any changed;
    std::mutex capture_mutex;
    std::thread presentation_thread,generation_thread,release_thread;
    FgBurstCapture burst;
    std::uint64_t burst_native{},burst_current{},burst_presentation{};
    bool burst_generated{};

    unsigned last_fg_priority_requested{UINT32_MAX};
    unsigned reported_queue_mode{2}; // Published under mx for presentation diagnostics.
    bool generation_high_priority{};
    static const char* queue_mode_name(unsigned mode) noexcept {
        return mode==1u?"separate-HIGH":"shared-game";
    }
    std::atomic<unsigned> latency_diagnostics{1};
    // Legacy diagnostic callers may still set a mode. Production follows
    // actual release conditions and buffer capacity, with no timer gate.
    std::atomic<unsigned> pacing_clock_mode{};
    StableDisplayClock display_clock; // Hold mx.
    std::atomic<unsigned> multiplier{2};
    std::atomic<bool> automatic_multiplier{};
    AutoMultiplier auto_multiplier; // Hold mx; decisions latch on a complete native interval.
    unsigned active_multiplier{2}; // Hold mx; set only when a complete interval is selected.
    CheekyFgAutoStatus automatic_status() const noexcept {
        CheekyFgAutoStatus out;out.supported=1;out.enabled=automatic_multiplier.load();
        const auto now=std::chrono::duration<double,std::milli>(TimingClock::now().time_since_epoch()).count();
        out.valid=auto_multiplier.valid&&now-auto_multiplier.last_sample_ms<=500;
        out.selected=auto_multiplier.selected;out.presented=pacing_active?active_multiplier:1;
        out.maximum=auto_multiplier.maximum;out.reason=auto_multiplier.reason;out.samples=auto_multiplier.samples;
        out.switches=auto_multiplier.switches;out.cpu_ms=auto_multiplier.cpu_ms;out.gpu_ms=auto_multiplier.gpu_ms;out.fg_ms=auto_multiplier.fg_ms;return out;
    }
    std::atomic<bool> native11_skip_identity_crop{}; // Runtime option; native D3D11 eye crops.
    double presented_native_ms{}; // Hold mx. Smoothed interval between distinct native submissions.
    TimingClock::time_point last_native_present{};
    std::uint64_t backlog_holds{},gpu_behind_holds{},gpu_behind_waits{};
    double native_clock_ms{}; // Hold mx. Credit period the pacing clock last used.
    // Native D3D11 frames carry no release policy (point 0: release the game
    // at FG submission). Nonzero applies that release point to them instead.
    std::atomic<bool> direct11_requested{};
    std::atomic<unsigned> native11_release_point{3}; // Matches the D3D12 default: after generated frame submission.
    CheekyFgTransportOptionsFn transport_options_query{};
    CheekyFgStorageOptions storage_options; // Protected by capture_mutex; latched at app WaitFrame.
    CheekyFgStaticEyeCropsV1 static_eye_crops; // Diagnostic only; NEVER used to pack a later native frame.
    // Protected by capture_mutex, like the captured images and their stamps.
    std::unordered_map<XrSwapchain,std::uint64_t> capture_crop_tokens;
    std::uint64_t color_stamp_frame{},color_stamp_token{}; // Hold capture_mutex; one anchor across this native frame's eye releases.
    CheekyFgCompleteHooklessFrameV2Fn hookless_complete_v2{}; // Optional test/runtime callback; otherwise resolve the additive export.
    std::uint64_t compact_current_lookups{},compact_current_misses{};
    std::uint64_t pose_sample_count{};
    CompactLayout eye_color_layout(unsigned eye,unsigned width,unsigned height,
                                   const CheekyFgCaptureCropsV2& current) const noexcept {
        if(quad_input)return {}; // Quad plane packing supplies its own explicit layout.
        const auto& s=storage_options;
        // Compact packing requires an exact pending stereo guide pair from
        // THIS capture interval. No cached previous-frame or global offset.
        if(s.compact&&eye<2&&(current.valid_mask&(1u<<eye))&&
            current.width==width&&current.height==height&&current.token){
            const auto& r=current.eye[eye];
            return compact_layout(width,height,true,r.width,r.height,r.x,r.y,s.peripheral_percent,s.feather);
        }
        return {}; // Missing current geometry: one ordinary full-eye copy.
    }
    std::atomic<std::uint64_t> direct_eye_copies{},eye_crop_copies{},capture_fallbacks{};
    CheekyFgExperimentDiagnostics experiment_state;
    CheekyFgMemory memory_state;
    std::uint64_t next_capture_memory{},next_worker_memory{},next_presenter_memory{};
    std::unordered_map<XrSwapchain,std::uint64_t> capture_tokens;
    std::unordered_map<XrSwapchain,std::uint64_t> capture_frames;
    std::unordered_map<XrSwapchain,TimingClock::time_point> capture_times;
    std::unordered_map<XrSwapchain,ColorReady> capture_ready;
    bool capture_overflow{};
    // Availability notifications and capture slots for real frames in
    // flight (pending, generating, history, presenting). The opt-in extra slots
    // let the game keep rendering while slower frame generation catches up, at
    // the cost of two more full stereo color copies of VRAM.
    std::atomic<bool> extra_frame_slots{};
    std::atomic<bool> limit_gpu_ahead{}; // Release Never: optional hold while the GPU is a frame behind.
    std::uint64_t carried_ticks{},clock_ticks{};
    // Hold mx. The previous frame is still on the GPU.
    bool gpu_behind_now() const {return latest_native&&!capture_completed(latest_native->ready);}
    std::size_t slot_count() const noexcept {return extra_frame_slots.load(std::memory_order_relaxed)?5u:3u;}
    // Captured real frames waiting for generation. Kept in order: interpolation
    // needs consecutive real frames, so a burst is worked off instead of skipped.
    std::size_t max_pending() const noexcept {return extra_frame_slots.load(std::memory_order_relaxed)?3u:2u;}
    std::array<std::shared_ptr<Native>,5> slots;
    void release_idle_slots() noexcept {
        // Hold mx. Only the slot itself owns these completed jobs; pending and
        // worker/history users retain the Native, and ready/upload users retain
        // their Images independently. Do not let an unused lease pin old pixels.
        for(auto& slot:slots)if(slot&&slot.use_count()==1&&capture_completed(slot->ready)){
            slot->color={};slot->color_ready={};slot->auxiliary.clear();slot->clear_guides();
        }
    }
    std::deque<std::shared_ptr<Native>> pending;
    std::deque<Pair> ready;
    std::optional<Pair> latest_native;
    std::array<BorderRect,2> last_fg_borders{}; // Protected by mx; latest FG region for native uploads.
    std::unordered_map<XrSwapchain,Chain> auxiliary_chains;
    Chain scene_chains[2]; // Native and generated presentations share each eye's destination.
    CheekyFgPerformance telemetry;
    CheekyFgBatchTimingsV1 batch_telemetry;
    std::weak_ptr<Pair::Progress> inflight_progress;
    CheekyFgBatchTimingsV1 batch_timing() const noexcept override {return batch_telemetry;}
    CheekyFgOverhead extra_gpu;
    // Unique native, generated interpolation time, newest FG input.
    std::array<TimingAverage,3> frame_ages;
    std::array<std::uint64_t,3> age_samples{};
    TimingAverage capture_to_completion,completion_to_native,completion_to_generated;
    // Native GPU-copy -> OpenXR handoff; retire only after the capture GPU fence
    // has published both calibrated timestamps. No GPU wait, timer or query here.
    struct NativeSubmissionProbe {
        std::array<ColorReady,2> color;
        ColorReady final_color;
        std::uint64_t submit_qpc{},submitted_tick{};
    };
    std::deque<NativeSubmissionProbe> pending_native_submissions;
    SignedTimingAverage native_ready_to_submit;
    TimingAverage native_hold,submit_ahead,native_capture_to_submit;
    std::uint64_t native_latency_samples{},native_latency_missing{},native_latency_held{},native_latency_early{};
    void collect_native_submission_probes() noexcept {
        // Called on the presenter thread without mx held. Reclaim producer
        // timestamps only after its queue fence retires; never block the GPU.
        collect_color_stamps();
        std::lock_guard lock(mx);
        while(!pending_native_submissions.empty()) {
            auto& p=pending_native_submissions.front();
            if(!p.color[0]||!p.color[1]||!p.final_color||!p.color[0]->calibrated||!p.color[1]->calibrated||!p.final_color->calibrated||
                GetTickCount64()-p.submitted_tick>2000){
                ++native_latency_missing;pending_native_submissions.pop_front();continue;
            }
            const auto left=p.color[0]->ready_qpc.load(std::memory_order_acquire);
            const auto right=p.color[1]->ready_qpc.load(std::memory_order_acquire);
            const auto final=p.final_color->ready_qpc.load(std::memory_order_acquire);
            if(!left||!right||!final)break;
            // The final assembly/crop GPU batch is ordered after both eye copies.
            // Require all three timestamps to avoid measuring a partial image.
            const double signed_ms=native_submission_delta_ms(p.submit_qpc,std::max(left,right),final,LatencyProbe::cpu_frequency());
            if(!std::isfinite(signed_ms)||std::abs(signed_ms)>1000.){++native_latency_missing;}
            else {
                native_ready_to_submit.add(signed_ms);
                if(signed_ms>=0){native_hold.add(signed_ms);++native_latency_held;}
                else {submit_ahead.add(-signed_ms);++native_latency_early;}
                ++native_latency_samples;
            }
            pending_native_submissions.pop_front();
        }
        // A dead/missing calibration must not pin a resource indefinitely.
        // Cap samples; the skip count is explicit rather than reporting zero ms.
        while(pending_native_submissions.size()>128){pending_native_submissions.pop_front();++native_latency_missing;}
    }
    std::array<TimingAverage,3> gpu_ready_ages;
    std::uint64_t gpu_ready_samples{},predicted_samples{},prediction_late{};
    std::uint64_t awaiting_generated_submission{},generated_gate_releases{},generated_gate_skips{};
    std::array<TimingAverage,4> generation_stages;
    std::uint64_t deferred_credits{},released_deferred_credits{},submitted_native_id{};
    bool credit_deferred{};
    unsigned inflight_release_point{},last_release_point{},active_release_point{};
    bool inflight_prepared{},release_policy_known{};
    bool inflight_generated_gate_released{};
    TimingAverage producer_cpu,pacing_wait;
    TimingAverage clock_late,credit_deferral,begin_gate_wait,presenter_content_wait;
    TimingAverage native_xr_end,generated_xr_end;
    TimingClock::time_point credit_deferred_since{};
    TimingClock::time_point release_delay_until{}; // Mode 4: 4 ms after FG submission; not a GPU fence wait.
    std::uint64_t last_generated_submit_tick{};
    TimingAverage app_image_wait,present_image_wait,runtime_begin,runtime_end,present_cpu;
    std::array<TimingAverage,3> app_calls;
    CadenceWindow cadence_window;
    std::uint64_t distinct_native{},runtime_submissions{};
    // Debug-only application->OpenXR submission trace. Monotonicity here only
    // proves what Cheeky handed to xrEndFrame, NOT what the headset displayed.
    XrTime presentation_trace_last_time{};
    double presentation_trace_last_position{-1.};
    std::uint64_t presentation_trace_events{},presentation_trace_anomalies{};
    double capture_cpu_accumulator{};
    struct ProducerElapsed {
        BufferedPresenter& owner;TimingClock::time_point begin{TimingClock::now()};
        ~ProducerElapsed();
    };
    char diagnostic[384]{"Buffered presenter waiting for stereo frames"};
    bool started{},stopping{},bypass{},fg_active{},pacing_active{},degraded{};
    bool native_frame_pending{}; // Hold mx; the pre-activation frame still owns runtime Begin/End.
    unsigned credits{},app_waiters{};
    std::uint64_t granted_credits{},consumed_credits{},capped_credits{}; // Pacing diagnostics: clock grants, game uses, grants lost at the 2-credit cap.
    std::uint64_t next_id{},waited{},begun{},ended{},ticks{},skipped{},epoch{},transition_until{};
    std::uint64_t last_credit_tick{},completed_pair_id{},inflight_id{};
    // Late-frame trace. Per native frame, CPU (QPC) timestamps from the game
    // thread, the hand-over and the worker; printed only when the runtime
    // skips a headset slot. Hold mx.
    struct FrameTrace {
        std::uint64_t id{};
        TimingClock::time_point wait_enter,wait_exit,begin_enter,begin_exit,end_enter,handed_off,worker_start,worker_end;
        double wait_before_ms{-1},wait_complete_ms{-1}; // Worker CPU fence waits inside generation.
    };
    std::array<FrameTrace,8> frame_traces;
    TimingClock::time_point game_wait_enter,game_wait_exit,game_begin_enter,game_begin_exit; // Game thread's current frame.
    std::uint64_t late_frames{};
    // Hold mx. Release-gate watchdog: when the gate has held the game for
    // 500 ms straight, something it waits for will never happen (e.g. a
    // generated frame lost in a pipeline reset); release it.
    TimingClock::time_point gate_held_since{};
    std::uint64_t gate_timeouts{};
    void release_gate_watchdog(TimingClock::time_point now) {
        if(!release_gate_active()){gate_held_since={};return;}
        if(gate_held_since==TimingClock::time_point{}){gate_held_since=now;return;}
        if(now-gate_held_since<std::chrono::milliseconds(500))return;
        ++gate_timeouts;
        if(trace){char message[256]{};
            sprintf_s(message,"FG release gate watchdog: game held %.0f ms; awaiting_generated=%llu inflight=%llu pending=%zu submitted_native=%llu; releasing (count=%llu)",
                std::chrono::duration<double,std::milli>(now-gate_held_since).count(),static_cast<unsigned long long>(awaiting_generated_submission),
                static_cast<unsigned long long>(inflight_id),pending.size(),static_cast<unsigned long long>(submitted_native_id),static_cast<unsigned long long>(gate_timeouts));
            trace(message);}
        if(awaiting_generated_submission)finish_generated_gate(awaiting_generated_submission,false);
        if(inflight_id){inflight_generated_gate_released=true;inflight_prepared=true;submitted_native_id=std::max(submitted_native_id,inflight_id);}
        for(const auto& job:pending)submitted_native_id=std::max(submitted_native_id,job->id);
        release_delay_until={};release_deferred_credit();gate_held_since={};changed.notify_all();
    }
    double last_content_wait_ms{}; // Presenter's wait for the content it is about to submit.
    FrameTrace& frame_trace(std::uint64_t id) noexcept {auto& t=frame_traces[id%frame_traces.size()];if(t.id!=id){t={};t.id=id;}return t;}
    const FrameTrace* find_trace(std::uint64_t id) const noexcept {const auto& t=frame_traces[id%frame_traces.size()];return id&&t.id==id?&t:nullptr;}
    static double span_ms(TimingClock::time_point a,TimingClock::time_point b) noexcept {
        return a==TimingClock::time_point{}||b==TimingClock::time_point{}||b<a?-1.:std::chrono::duration<double,std::milli>(b-a).count();
    }
    void describe_trace(char* out,std::size_t size,std::uint64_t id) const noexcept {
        const auto* t=find_trace(id);const auto* p=find_trace(id-1);
        if(!t){sprintf_s(out,size,"#%llu n/a",static_cast<unsigned long long>(id));return;}
        // credit=xrWaitFrame wait, gate=xrBeginFrame gate, game=go-ahead to
        // xrEndFrame, capture=xrEndFrame to hand-over, queue=hand-over to
        // worker, worker=worker start to generation done; of it, wait_prev=CPU
        // wait for the previous generation, wait_done=CPU wait for this one
        // (including the capture it waits for on the GPU).
        sprintf_s(out,size,"#%llu interval=%.1f credit=%.1f gate=%.1f game=%.1f capture=%.1f queue=%.1f worker=%.1f wait_prev=%.1f wait_done=%.1f",
            static_cast<unsigned long long>(id),p?span_ms(p->end_enter,t->end_enter):-1.,span_ms(t->wait_enter,t->wait_exit),
            span_ms(t->begin_enter,t->begin_exit),span_ms(t->wait_exit,t->end_enter),span_ms(t->end_enter,t->handed_off),
            span_ms(t->handed_off,t->worker_start),span_ms(t->worker_start,t->worker_end),t->wait_before_ms,t->wait_complete_ms);
    }
    TimingClock::time_point inflight_started{}; // Hold mx. When the worker took inflight_id.
    std::uint64_t stalled_natives{}; // Hold mx. Native frames shown because generation stalled.
    // Hold mx. Generation has run for over 100 ms (e.g. a driver session
    // rebuild). The presenter then shows a newer finished native frame
    // instead of freezing on the interval it was waiting for.
    bool stalled_native(double position) const {
        return inflight_id&&TimingClock::now()-inflight_started>std::chrono::milliseconds(100)&&
            latest_native&&capture_completed(latest_native->ready)&&double(latest_native->native_id)>position;
    }
    XrFrameState predicted{XR_TYPE_FRAME_STATE};
    XrTime app_display_time{};
    XrResult terminal{XR_SUCCESS},raw_result{XR_SUCCESS};
    bool stop_in_progress{}; // Protected by mx; set for the whole of stop().
    const XrFrameEndInfo* raw{};bool raw_done{};
    CheekyFgAcquireFn guide_acquire{};CheekyFgReleaseFn guide_release{};
    CheekyFgCaptureTokenFn guide_token{};
    CheekyFgCaptureStampFn guide_stamp{};

    ~BufferedPresenter() override {reset();}
    void set_native_frame_pending(bool pending_frame) noexcept override {
        std::lock_guard lock(mx);native_frame_pending=pending_frame;changed.notify_all();
    }
    void finish_application_frame() noexcept {
        // A following xrWaitFrame may overlap the preceding xrEndFrame.
        // Preserve its presenter until every accepted application frame ends.
        std::unique_lock lock(mx);changed.wait(lock,[&]{return stopping||waited==ended;});
    }
    void runtime_failure(const char* stage,XrResult result) noexcept {
        std::lock_guard lock(mx);
        terminal=result;stopping=true;state=CheekyFgState::presentation_failed;
        sprintf_s(diagnostic,"DLSS-FG %s failed: XrResult=%d; presenter stopped",stage,int(result));
        if(trace)trace(diagnostic);
        changed.notify_all();
    }

    // An application swapchain is being destroyed: drop captures keyed by its
    // handle so a reused handle never pairs with old images. The presenter
    // itself is retired at the next xrWaitFrame boundary.
    void forget_swapchain(XrSwapchain sc) noexcept {
        std::lock_guard lock(capture_mutex);
        captured.erase(sc);direct_captures.erase(sc);capture_plans.erase(sc);captured_descs.erase(sc);
        capture_tokens.erase(sc);capture_frames.erase(sc);capture_times.erase(sc);capture_ready.erase(sc);capture_crop_tokens.erase(sc);
        std::lock_guard composed(composed11_mutex);composed11.erase(sc);
    }
    // True when the workers stopped for a reason other than a runtime failure.
    bool interrupted() noexcept {std::lock_guard lock(mx);return stopping&&!XR_FAILED(terminal);}
    void set_quad_sources11(const std::array<XrSwapchain,4>& sources) {std::lock_guard lock(capture_mutex);quad_sources11=sources;}

    CheekyFgPerformance performance() const noexcept override {
        auto result=telemetry;result.asynchronous_presenter=1;result.queued_native=unsigned(pending.size());
        result.skipped_native=skipped;result.producer_cpu_ms=producer_cpu.mean();result.pacing_wait_ms=pacing_wait.mean();return result;
    }
    CheekyFgNativeSubmissionLatency native_submission_latency() const noexcept override {
        CheekyFgNativeSubmissionLatency t;t.supported=1;t.enabled=latency_diagnostics.load()!=0;
        t.gpu_samples=native_latency_samples;t.missing_stamps=native_latency_missing;
        t.held_samples=native_latency_held;t.early_samples=native_latency_early;
        t.pending=pending_native_submissions.size();
        t.gpu_ready_to_submit_ms=native_ready_to_submit.mean();t.gpu_ready_to_submit_last_ms=native_ready_to_submit.last;
        t.native_hold_ms=native_hold.mean();t.native_hold_last_ms=native_hold.last;
        t.submit_ahead_ms=submit_ahead.mean();t.submit_ahead_last_ms=submit_ahead.last;
        t.capture_to_submit_ms=native_capture_to_submit.mean();
        t.capture_to_submit_last_ms=native_capture_to_submit.last;
        return t;
    }
    CheekyFgPacingTimeline pacing_timeline() const noexcept override {
        CheekyFgPacingTimeline t;t.supported=1;
        t.generated_last_tick=last_generated_submit_tick;
        t.recent_generated=last_generated_submit_tick&&GetTickCount64()-last_generated_submit_tick<2000;
        t.deferred_events=deferred_credits;t.deferred_releases=released_deferred_credits;
        t.clock_late_ms=clock_late.mean();t.clock_late_last_ms=clock_late.last;
        t.credit_deferred_ms=credit_deferral.mean();t.credit_deferred_last_ms=credit_deferral.last;
        t.game_wait_credit_ms=pacing_wait.mean();t.game_wait_credit_last_ms=pacing_wait.last;
        t.game_begin_gate_ms=begin_gate_wait.mean();t.game_begin_gate_last_ms=begin_gate_wait.last;
        t.presenter_content_ms=presenter_content_wait.mean();t.presenter_content_last_ms=presenter_content_wait.last;
        t.native_xr_end_ms=native_xr_end.mean();t.native_xr_end_last_ms=native_xr_end.last;
        t.generated_xr_end_ms=generated_xr_end.mean();t.generated_xr_end_last_ms=generated_xr_end.last;
        t.fg_submit_to_complete_ms=generation_stages[3].mean();
        t.fg_submit_to_complete_last_ms=generation_stages[3].last;
        return t;
    }
    CheekyFgCadence cadence() const noexcept override {
        CheekyFgCadence result;result.supported=1;std::array<double,4> rates{};
        result.valid=cadence_window.rates(rates);result.game_fps=rates[0];result.native_fps=rates[1];result.generated_fps=rates[2];
        result.unique_fps=rates[1]+rates[2];result.submission_fps=rates[3];
        result.refresh_hz=display_clock.display_ms()>0?1000./display_clock.display_ms():0;
        result.application_period_ms=display_clock.display_ms()*(pacing_active?active_multiplier:1);
        result.native_target_fps=result.application_period_ms>0?1000./result.application_period_ms:0;
        result.app_image_wait_ms=app_image_wait.mean();result.present_image_wait_ms=present_image_wait.mean();
        result.begin_ms=runtime_begin.mean();result.end_ms=runtime_end.mean();result.present_cpu_ms=present_cpu.mean();
        result.app_begin_ms=app_calls[0].mean();result.app_end_ms=app_calls[1].mean();result.app_release_ms=app_calls[2].mean();return result;
    }

    CheekyFgMemory memory() const noexcept override {
        auto out=memory_state;out.supported=out.presenter_present=1;return out;
    }
    CheekyFgDirectOutput direct_output() const noexcept override {
        auto out=direct_stats;out.supported=1;out.requested=direct_output_mode;return out;
    }
    CheekyFgDirectOutputDetails direct_output_details() const noexcept override {auto out=direct_details;out.supported=1;return out;}

    CheekyFgPresentation presentation() const noexcept override {
        CheekyFgPresentation out;out.supported=1;
        out.native_samples=age_samples[0];out.generated_samples=age_samples[1];
        out.native_age_ms=frame_ages[0].mean();out.native_age_last_ms=frame_ages[0].last;
        out.generated_age_ms=frame_ages[1].mean();out.generated_age_last_ms=frame_ages[1].last;
        out.newest_input_age_ms=frame_ages[2].mean();out.newest_input_age_last_ms=frame_ages[2].last;
        return out;
    }
    void note_frame_age(const Pair& pair,bool generated,TimingClock::time_point submitted_at) noexcept {
        // Hold mx. Timestamps travel with immutable images, not with their pool
        // slots or the latest game frame.
        auto add=[&](unsigned index,TimingClock::time_point source) {
            if(source==TimingClock::time_point{}||source>submitted_at)return;
            frame_ages[index].add(std::chrono::duration<double,std::milli>(submitted_at-source).count());++age_samples[index];
        };
        if(generated) {
            if(pair.native_captured!=TimingClock::time_point{}&&pair.current_captured>=pair.native_captured)
                add(1,pair.native_captured+std::chrono::duration_cast<TimingClock::duration>((pair.current_captured-pair.native_captured)*pair.fraction()));
            add(2,pair.current_captured);
        }else add(0,pair.native_captured);
        if(pair.interpolated&&pair.completed_at!=TimingClock::time_point{}&&pair.completed_at<=submitted_at) {
            const auto ready_age=std::chrono::duration<double,std::milli>(submitted_at-pair.completed_at).count();
            if(generated&&pair.current_captured!=TimingClock::time_point{}&&pair.current_captured<=pair.completed_at) {
                // Same generated submissions enter both windows, so their sum
                // accounts for newest-input age without mixing populations.
                capture_to_completion.add(std::chrono::duration<double,std::milli>(pair.completed_at-pair.current_captured).count());
                completion_to_generated.add(ready_age);
                if(pair.handed_off_at>=pair.current_captured&&pair.worker_started_at>=pair.handed_off_at&&
                    pair.submitted_at>=pair.worker_started_at&&pair.completed_at>=pair.submitted_at) {
                    const std::array<TimingClock::time_point,5> boundaries{pair.current_captured,pair.handed_off_at,
                        pair.worker_started_at,pair.submitted_at,pair.completed_at};
                    for(unsigned i=0;i<generation_stages.size();++i)
                        generation_stages[i].add(std::chrono::duration<double,std::milli>(boundaries[i+1]-boundaries[i]).count());
                }
            }else if(!generated)completion_to_native.add(ready_age);
        }
    }
    void note_app_image_wait(double ms) noexcept override {std::lock_guard lock(mx);app_image_wait.add(ms);}
    void note_gpu_ready_age(const Pair& pair,std::uint64_t submitted_cpu,std::uint64_t display_cpu) noexcept {
        std::uint64_t ready{},captured=UINT64_MAX;
        for(const auto& stamp:pair.current_color_ready){
            if(!stamp)return;const auto qpc=stamp->ready_qpc.load();if(!qpc)return;
            ready=std::max(ready,qpc);captured=std::min(captured,stamp->captured_cpu);
        }
        const double scale=1000./LatencyProbe::cpu_frequency();
        if(!ready||captured>ready||submitted_cpu<ready)return;
        const double tail=double(ready-captured)*scale,submit=double(submitted_cpu-ready)*scale;
        if(tail>1000||submit>1000)return;
        gpu_ready_ages[0].add(tail);gpu_ready_ages[1].add(submit);++gpu_ready_samples;
        if(display_cpu){
            if(display_cpu<submitted_cpu){++prediction_late;return;}
            const double display=double(display_cpu-ready)*scale;
            if(display<=1000){gpu_ready_ages[2].add(display);++predicted_samples;}
        }
    }
    void finish_generated_gate(std::uint64_t current_id,bool submitted) noexcept {
        // Hold mx. Startup/FG failure has no generated gate; stale/hidden
        // content cancels it so runtime transitions cannot strand the game.
        if(!awaiting_generated_submission||current_id<awaiting_generated_submission)return;
        if(current_id==inflight_id)inflight_generated_gate_released=true;
        awaiting_generated_submission=0;if(submitted)++generated_gate_releases;else ++generated_gate_skips;
        release_deferred_credit();changed.notify_all();
    }
    void note_app_call(unsigned stage,double ms) noexcept override {std::lock_guard lock(mx);if(stage<app_calls.size())app_calls[stage].add(ms);}
    XrResult runtime_end_frame(const XrFrameEndInfo* end) {
        const auto start=TimingClock::now();
        const auto result=backend->end_frame(end);
#ifdef CHEEKY_FG_LIFETIME_DIAGNOSTICS
        if(XR_FAILED(result)&&trace&&end) {
            char message[384]{};
            sprintf_s(message,"FG-LIFETIME end_frame FAILED session=%p result=%d display_time=%lld layers=%u native=%llu current=%llu generated=%u",
                session,int(result),static_cast<long long>(end->displayTime),end->layerCount,
                static_cast<unsigned long long>(burst_native),static_cast<unsigned long long>(burst_current),unsigned(burst_generated));
            trace(message);
            // Only probe on failure: these read-only calls distinguish a dead
            // space from a dead swapchain without adding steady-state XR work.
            const auto chain=[&](XrSwapchain handle,unsigned layer,unsigned eye) {
                unsigned count{};const auto status=xr.enumerate?xr.enumerate(handle,0,&count,nullptr):XR_ERROR_FUNCTION_UNSUPPORTED;
                sprintf_s(message,"FG-LIFETIME submitted_chain layer=%u eye=%u chain=%p enumerate_result=%d images=%u",
                    layer,eye,handle,int(status),count);trace(message);
            };
            for(unsigned i=0;end->layers&&i<end->layerCount;++i) {
                const auto* layer=end->layers[i];if(!layer)continue;
                XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
                const auto status=xr.locate_space&&layer->space?xr.locate_space(layer->space,layer->space,end->displayTime,&location):XR_ERROR_FUNCTION_UNSUPPORTED;
                sprintf_s(message,"FG-LIFETIME submitted_layer index=%u type=%d space=%p self_locate_result=%d",
                    i,int(layer->type),layer->space,int(status));trace(message);
                switch(layer->type) {
                case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
                    const auto* p=reinterpret_cast<const XrCompositionLayerProjection*>(layer);
                    for(unsigned eye=0;p->views&&eye<p->viewCount;++eye)chain(p->views[eye].subImage.swapchain,i,eye);
                    break;
                }
                case XR_TYPE_COMPOSITION_LAYER_QUAD:chain(reinterpret_cast<const XrCompositionLayerQuad*>(layer)->subImage.swapchain,i,0);break;
                case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:chain(reinterpret_cast<const XrCompositionLayerCylinderKHR*>(layer)->subImage.swapchain,i,0);break;
                case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:chain(reinterpret_cast<const XrCompositionLayerCubeKHR*>(layer)->swapchain,i,0);break;
                case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:chain(reinterpret_cast<const XrCompositionLayerEquirectKHR*>(layer)->subImage.swapchain,i,0);break;
                case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:chain(reinterpret_cast<const XrCompositionLayerEquirect2KHR*>(layer)->subImage.swapchain,i,0);break;
                default:break;
                }
            }
        }
#endif
        std::lock_guard lock(mx);runtime_end.add(elapsed_ms(start));return result;
    }
    const char* diagnostic_text() const noexcept override {return diagnostic;}
    // Caller holds mx (or has joined the workers). Dropped jobs have never
    // submitted inference, so their raw snapshot leases can retire immediately.
    std::uint64_t passthrough_events{};
    std::uint64_t history_reset_events{};
    void discard_pending() noexcept {
        if(auto progress=inflight_progress.lock())progress->failed=true;
        for(auto& job:pending)job->clear_guides();pending.clear();
    }
    void stop() noexcept {
        {std::lock_guard lock(mx);stopping=true;stop_in_progress=true;changed.notify_all();}
        if(presentation_thread.joinable())presentation_thread.join();
        if(generation_thread.joinable())generation_thread.join();
        if(release_thread.joinable())release_thread.join();
        std::lock_guard capture_lock(capture_mutex);
        backend->retire();
        std::lock_guard lock(mx);discard_pending();ready.clear();latest_native.reset();last_fg_borders={};slots={};
        captured.clear();backend->discard_captures(true);capture_tokens.clear();capture_frames.clear();capture_times.clear();capture_ready.clear();color_stamp_frame=color_stamp_token=0;capture_overflow=false;capture_cpu_accumulator=0;started=false;credits=0;fg_active=pacing_active=false;
        if(raw)raw_result=XR_FAILED(terminal)?terminal:XR_ERROR_SESSION_NOT_RUNNING;
        raw=nullptr;raw_done=true;passthrough_events=history_reset_events=0;
        waited=begun=ended=0;ticks=transition_until=0;degraded=false;app_display_time=0;predicted={XR_TYPE_FRAME_STATE};++epoch;
        app_image_wait={};present_image_wait={};runtime_begin={};runtime_end={};present_cpu={};app_calls={};cadence_window={};
        clock_late={};credit_deferral={};begin_gate_wait={};presenter_content_wait={};
        native_xr_end={};generated_xr_end={};credit_deferred_since={};release_delay_until={};last_generated_submit_tick=0;
        reset_gpu_metrics();extra_gpu={};batch_telemetry={};inflight_progress.reset();
        frame_ages={};age_samples={};
        last_credit_tick=completed_pair_id=inflight_id=0;inflight_started={};stalled_natives=0;gate_held_since={};
        capture_to_completion={};completion_to_native={};completion_to_generated={};
        generation_stages={};deferred_credits=released_deferred_credits=submitted_native_id=0;credit_deferred=false;
        gpu_ready_ages={};gpu_ready_samples=predicted_samples=prediction_late=0;
        pending_native_submissions.clear();native_ready_to_submit={};native_hold={};submit_ahead={};native_capture_to_submit={};
        native_latency_samples=native_latency_missing=native_latency_held=native_latency_early=0;
        experiment_state={};direct_request.reset();direct_stats={};direct_wait={};
        direct_details={};logged_direct_details={};
        direct_blocked_target=0;
        awaiting_generated_submission=generated_gate_releases=generated_gate_skips=0;
        memory_state={};next_capture_memory=next_worker_memory=next_presenter_memory=0;
        inflight_release_point=last_release_point=active_release_point=0;inflight_prepared=release_policy_known=false;
        distinct_native=runtime_submissions=0;
        auto_multiplier={};active_multiplier=2;
        presented_native_ms=native_clock_ms=0;last_native_present={};display_clock={};
        backlog_holds=gpu_behind_holds=gpu_behind_waits=0;carried_ticks=clock_ticks=0;
        presentation_trace_last_time=0;presentation_trace_last_position=-1.;
        presentation_trace_events=presentation_trace_anomalies=0;
        stop_in_progress=false;changed.notify_all();
    }
    // Private chains can retire on the application thread after workers stop.
    std::function<void(XrSwapchain)> retire_chain;

    void invalidate_temporal() noexcept override {
        stop();backend->reset_generation();destroy_chains();
        std::lock_guard lock(mx);held_valid=false;generated_valid=false;frames=0;telemetry={};producer_cpu={};pacing_wait={};
    }
    void configure(std::uint64_t next) noexcept override {
        if(!next||configuration==next)return;
        if(configuration){
            if(trace)trace("DLSS-FG source/presenter configuration changed: draining and restarting without changing FG enabled state");
            invalidate_temporal();
        }
        std::lock_guard lock(mx);configuration=next;state=CheekyFgState::waiting_for_history;
    }
    bool start() noexcept {
        if(started)return true;
        stopping=false;terminal=XR_SUCCESS;raw_done=false;
        try {
            started=true;
            release_thread=std::thread([this]{release_loop();});
            generation_thread=std::thread([this]{generation_loop();});
            presentation_thread=std::thread([this]{presentation_loop();});
        } catch(...) {stopping=true;changed.notify_all();return false;}
        return true;
    }
    XrResult wait_frame(const XrFrameWaitInfo* info,XrFrameState* frame) noexcept override {
        if(bypass)return xr.wait_frame(session,info,frame);
        if(!frame||(info&&info->next))return XR_ERROR_VALIDATION_FAILURE;
        const auto runtime=GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll");
        const auto storage_query=runtime?reinterpret_cast<CheekyFgStorageOptionsFn>(GetProcAddress(runtime,"CheekyFG_GetStorageOptionsV1")):nullptr;
        CheekyFgStorageOptions storage;
        if(storage_query)storage_query(&storage);
        const auto crop_query=runtime?reinterpret_cast<CheekyFgStaticEyeCropsFn>(
            GetProcAddress(runtime,"CheekyFG_GetStaticEyeCropsV1")):nullptr;
        CheekyFgStaticEyeCropsV1 crops;
        if(!crop_query||!crop_query(&crops))crops.valid_mask=0;
        unsigned old_compact{},old_mask{};
        {std::lock_guard capture_lock(capture_mutex);
            old_compact=storage_options.compact;old_mask=static_eye_crops.valid_mask;
            storage_options=storage;static_eye_crops=crops;
        }
        if(trace&&(old_compact!=storage.compact||old_mask!=crops.valid_mask)){
            char message[180]{};
            sprintf_s(message,"FG native color storage compact=%u valid_physical_eyes=0x%x "
                "(dynamic gaze/continuous calibration uses existing full-eye color history)",
                storage.compact,crops.valid_mask);
            trace(message);
        }
        const auto query=transport_options_query?transport_options_query:runtime?
            reinterpret_cast<CheekyFgTransportOptionsFn>(GetProcAddress(runtime,"CheekyFG_GetTransportOptionsV1")):nullptr;
        CheekyFgTransportOptions options;
        const auto output_query=direct_output_query?direct_output_query:runtime?
            reinterpret_cast<CheekyFgDirectOutputFn>(GetProcAddress(runtime,"CheekyFG_GetDirectOutputOptionsV1")):nullptr;
        CheekyFgDirectOutput direct_options;
        if(output_query&&output_query(&direct_options)){
            const unsigned mode=backend->supports_direct_output() && direct_options.requested==1u;
            if(direct_output_mode.exchange(mode)!=mode)direct_blocked_target=0;
            // Native D3D11 renders generated frames straight into the OpenXR
            // image without the D3D12 direct-output handshake.
            if(direct11_requested.exchange(direct_options.requested==1u)!=(direct_options.requested==1u)&&native11&&trace)
                trace(direct_options.requested==1u?"FG native D3D11 output=direct OpenXR image":"FG native D3D11 output=private image + copy");
        }
        if(query&&query(&options)){
            const auto old_diagnostics=latency_diagnostics.exchange(options.latency_diagnostics==1u?1u:0u);
            if(old_diagnostics!=latency_diagnostics.load()){
                std::lock_guard lock(mx);
                pending_native_submissions.clear();native_ready_to_submit={};native_hold={};submit_ahead={};native_capture_to_submit={};
                native_latency_samples=native_latency_missing=native_latency_held=native_latency_early=0;
            }
            if(trace&&old_diagnostics!=latency_diagnostics)
                trace(latency_diagnostics?"FG latency diagnostics=1-in-17 (next native frame)":"FG latency diagnostics=OFF (next native frame)");
        }
        const auto native11_query=runtime&&native11?reinterpret_cast<CheekyFgNative11OptionsFn>(GetProcAddress(runtime,"CheekyFG_GetNative11OptionsV1")):nullptr;
        CheekyFgNative11Options native11_options;
        if(native11_query&&native11_query(&native11_options)&&native11_skip_identity_crop.exchange(native11_options.skip_identity_crop!=0)!=(native11_options.skip_identity_crop!=0)&&trace)
            trace(native11_options.skip_identity_crop?"FG native D3D11 whole-image eyes=reuse capture":"FG native D3D11 whole-image eyes=crop copy");
        const auto pacing_query=runtime?reinterpret_cast<CheekyFgPacingOptionsFn>(GetProcAddress(runtime,"CheekyFG_GetPacingOptionsV1")):nullptr;
        CheekyFgPacingOptions pacing;
        const auto rate_query=runtime?reinterpret_cast<CheekyFgRateOptionsFn>(GetProcAddress(runtime,"CheekyFG_GetRateOptionsV1")):nullptr;
        CheekyFgRateOptions rate;
        if(rate_query&&rate_query(&rate))multiplier=std::clamp(rate.multiplier,2u,4u);
        const auto automatic_query=runtime?reinterpret_cast<CheekyFgRateOptionsV2Fn>(GetProcAddress(runtime,"CheekyFG_GetRateOptionsV2")):nullptr;
        CheekyFgRateOptionsV2 automatic_options;
        if(automatic_query&&automatic_query(&automatic_options)){
            multiplier=std::clamp(automatic_options.multiplier,2u,4u);automatic_multiplier=automatic_options.automatic!=0;
        }
        if(pacing_query&&pacing_query(&pacing)){
            const bool extra=pacing.extra_frame_slots!=0;
            if(extra_frame_slots.exchange(extra)!=extra&&trace)trace(extra?"FG extra frame slots=ON (5 capture slots)":"FG extra frame slots=OFF (3 capture slots)");
            const bool gpu_ahead=pacing.limit_gpu_ahead!=0;
            if(limit_gpu_ahead.exchange(gpu_ahead)!=gpu_ahead&&trace)trace(gpu_ahead?"FG limit GPU ahead=ON":"FG limit GPU ahead=OFF");
            {std::lock_guard lock(mx);
                const auto before=display_clock.display_ns();
                display_clock.set_override(pacing.target_refresh_hz);
                if(before!=display_clock.display_ns())changed.notify_all();
            }
            const auto point=std::min(pacing.native11_release_point,7u);
            if(native11_release_point.exchange(point)!=point&&trace){char message[96]{};
                sprintf_s(message,"FG native D3D11 release point=%u (3 = after generated frame submission)",point);trace(message);}
        }
        std::unique_lock lock(mx);
        if(!start())return XR_ERROR_RUNTIME_FAILURE;
        const auto wait_begin=TimingClock::now();
        ++app_waiters;
        // Optional GPU backlog protection is an actual resource dependency,
        // independent of the nominal display target.
        const auto gpu_gate=[&]{return pacing_active&&
            limit_gpu_ahead.load(std::memory_order_relaxed)&&active_release_point==5u&&gpu_behind_now();};
        for(bool counted=false,capacity_counted=false;;) {
            if(stopping||XR_FAILED(terminal))break;
            const bool ordered=waited==begun&&!raw&&predicted.predictedDisplayPeriod>0;
            const bool gated=release_gate_active();
            const bool capacity=degraded||producer_has_capacity();
            const bool queued=pending.size()>=max_pending()||ready.size()>=max_pending();
            if(ordered&&!gated&&capacity&&!gpu_gate()){
                release_deferred_credit();++granted_credits;break;
            }
            if(ordered&&gated)mark_credit_deferred();
            if(ordered&&!capacity&&!capacity_counted){++capped_credits;capacity_counted=true;}
            if(ordered&&gpu_gate()&&!counted){++gpu_behind_waits;counted=true;}
            // Fence/lease retirement may not notify the CPU. Queue consumption
            // and release gates do notify; they need no periodic polling.
            if(ordered&&!gated&&((!capacity&&!queued)||gpu_gate()))changed.wait_for(lock,std::chrono::microseconds(500));
            else changed.wait(lock);
        }
        --app_waiters;
        pacing_wait.add(elapsed_ms(wait_begin));game_wait_enter=wait_begin;game_wait_exit=TimingClock::now();
        // Interrupted by a retirement or reset on another thread: let the stop
        // finish so the layer can retry against the current presenter.
        if(stopping&&!XR_FAILED(terminal))changed.wait(lock,[&]{return !stop_in_progress||XR_FAILED(terminal);});
        if(stopping||XR_FAILED(terminal))return XR_FAILED(terminal)?terminal:XR_ERROR_SESSION_NOT_RUNNING;
        credits=0;++consumed_credits;++waited;
        const auto next=frame->next;*frame=predicted;frame->next=next;
        // Predict a native frame at least two display intervals ahead; keep one display
        // time consistent across all engine stages and strictly increasing.
        const auto period=std::max<XrDuration>(1,display_clock.display_ns());
        const auto factor=pacing_active?active_multiplier:1u;
        frame->predictedDisplayTime=std::max(predicted.predictedDisplayTime+period*std::max(2u,factor),app_display_time+period*factor);
        if(period<=INT64_MAX/factor)frame->predictedDisplayPeriod=period*factor;
        app_display_time=frame->predictedDisplayTime;return XR_SUCCESS;
    }
    XrResult begin_frame(const XrFrameBeginInfo* info) noexcept override {
        if(bypass)return xr.begin_frame(session,info);
        if(info&&info->next)return XR_ERROR_VALIDATION_FAILURE;
        std::unique_lock capture_lock(capture_mutex);
        std::unique_lock lock(mx);
        if(stopping)return XR_FAILED(terminal)?terminal:XR_ERROR_SESSION_NOT_RUNNING;
        if(waited==begun)return XR_ERROR_CALL_ORDER_INVALID;
        // A host may forfeit an in-progress frame (OpenComposite does this at
        // startup). Waiting for its missing EndFrame deadlocks the render thread.
        const bool discarded=begun!=ended;
        if(discarded){
            ended=begun;++skipped;++epoch;discard_pending();ready.clear();latest_native.reset();
            awaiting_generated_submission=0;
            captured.clear();backend->discard_captures(false);capture_tokens.clear();capture_frames.clear();capture_crop_tokens.clear();
            capture_times.clear();capture_ready.clear();capture_overflow=false;capture_cpu_accumulator=0;
            if(trace)trace("FG xrBeginFrame discarded the preceding unended application frame");
            changed.notify_all();
        }
        capture_lock.unlock();
        // A pipelined xrWaitFrame may already have consumed a credit before
        // the previous end_frame enqueues its FG job. Gate begin_frame too.
        const auto gate_started=TimingClock::now();
        changed.wait(lock,[&]{return stopping||!release_gate_active();});
        begin_gate_wait.add(elapsed_ms(gate_started));game_begin_enter=gate_started;game_begin_exit=TimingClock::now();
        if(stopping)return XR_FAILED(terminal)?terminal:XR_ERROR_SESSION_NOT_RUNNING;
        ++begun;changed.notify_all();lock.unlock();capture_lock.lock();
        {std::lock_guard state_lock(mx);if(stopping)return XR_ERROR_SESSION_NOT_RUNNING;}
        backend->begin_application_frame(automatic_multiplier.load());
        return discarded?XR_FRAME_DISCARDED:XR_SUCCESS;
    }

    template<class Fn> static bool subimages(FrameLayers& layers,Fn&& visit) {
        for(auto& layer:layers.layers)if(!layer.scene)switch(layer.type) {
        case XR_TYPE_COMPOSITION_LAYER_PROJECTION:for(auto& v:layer.views)if(!visit(v.subImage))return false;break;
        case XR_TYPE_COMPOSITION_LAYER_QUAD:if(!visit(layer.quad.subImage))return false;break;
        case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:if(!visit(layer.cylinder.subImage))return false;break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:if(!visit(layer.equirect.subImage))return false;break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:if(!visit(layer.equirect2.subImage))return false;break;
        default:return false;
        }
        return true;
    }

    XrResult passthrough(const XrFrameEndInfo* end,const char* reason) noexcept {
        if(quad_native_fallback){const auto result=quad_native_fallback();if(XR_FAILED(result))return result;}
        std::unique_lock lock(mx);
        if(stopping)return XR_FAILED(terminal)?terminal:XR_ERROR_SESSION_NOT_RUNNING;
        ++passthrough_events;
        if(trace&&(passthrough_events<=3||passthrough_events%120==0)) {
            char message[256]{};
            sprintf_s(message,"DLSS-FG native passthrough: reason=%s count=%llu layers=%u degraded=%u; waiting for compatible stereo capture",
                reason,static_cast<unsigned long long>(passthrough_events),end?end->layerCount:0,unsigned(degraded));
            trace(message);
        }
        if(!degraded)state=CheekyFgState::unsupported_submission;
        ++epoch;raw=end;raw_done=false;credits=0;credit_deferred=false;awaiting_generated_submission=0;discard_pending();ready.clear();latest_native.reset();fg_active=pacing_active=false;changed.notify_all();
        // Unknown next chains cannot be copied generically. Their application
        // storage remains alive until the owning thread submits this frame.
        // Stop wakes application waits immediately, but this caller lends its
        // next/layers storage to the presentation thread until submission ends.
        changed.wait(lock,[&]{return raw_done;});
        return raw_result;
    }
    XrResult end_frame(const XrFrameEndInfo* end,const std::array<unsigned,2>&,
                       const XrCompositionLayerProjection* projection) noexcept override {
        const auto* external=quad_pair_for_call;
        if(bypass)return xr.end_frame(session,end);
        if(!end)return XR_ERROR_VALIDATION_FAILURE;
        ProducerElapsed elapsed{*this};const auto end_entered=TimingClock::now();
        std::uint64_t id{};bool use_raw{};
        {std::lock_guard lock(mx);if(stopping)return XR_FAILED(terminal)?terminal:XR_ERROR_SESSION_NOT_RUNNING;
            if(!started||begun==ended)return XR_ERROR_CALL_ORDER_INVALID;++native_count;id=++next_id;use_raw=degraded;
            auto& t=frame_trace(id);t.wait_enter=game_wait_enter;t.wait_exit=game_wait_exit;
            t.begin_enter=game_begin_enter;t.begin_exit=game_begin_exit;t.end_enter=end_entered;}
        const auto fg_runtime=GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll");
        const auto hookless_complete=fg_runtime&&!external?reinterpret_cast<CheekyFgCompleteHooklessFrameFn>(
            GetProcAddress(fg_runtime,"CheekyFG_CompleteHooklessFrameV1")):nullptr;
        if(use_raw){
            if(hookless_complete)hookless_complete(0);
            const auto result=passthrough(end,"presenter degraded");std::lock_guard lock(mx);if(!stopping)++ended;changed.notify_all();return result;
        }
        {std::lock_guard capture_lock(capture_mutex);if(capture_overflow){
            if(hookless_complete)hookless_complete(0);
            capture_overflow=false;std::lock_guard lock(mx);++skipped;++ended;changed.notify_all();return XR_SUCCESS;}}
        XrResult result=XR_SUCCESS;
        std::shared_ptr<Native> job;
        {
            std::lock_guard lock(mx);
            // Slots beyond the active count are released once idle, freeing their VRAM.
            for(std::size_t i=slot_count();i<slots.size();++i)if(slots[i]&&slots[i].use_count()==1&&capture_completed(slots[i]->ready))slots[i].reset();
            for(std::size_t i=0;i<slot_count();++i) {
                auto& slot=slots[i];
                if(!slot)slot=std::make_shared<Native>();
                if(slot.use_count()==1&&capture_completed(slot->ready)){job=slot;break;}
            }
        }
        const bool inference_slot=bool(job);
        // CaptureStamp is per-image. Some UEVR versions release a left/right
        // image separately or execute the final copy through opaque wrappers.
        // In hook-free mode only the completed xrEndFrame can claim a pair.
        if(!inference_slot&&hookless_complete)hookless_complete(0);
        if(!job){job=std::make_shared<Native>();std::lock_guard lock(mx);++skipped;}
        job->color={};job->color_ready={};job->final_color_ready.reset();job->auxiliary.clear();job->clear_guides();job->policy={};job->guides_valid=false;job->guide_token=0;job->source_configuration=0;job->render_frame=0;job->captured_at={};
        job->guide_state=CheekyFgState::waiting_for_guides;job->source_depth=job->source_motion=0;
        if(!projection||!job->layers.prepare(end,projection)||!job->layers.retain_spaces(session,xr.retain_space)){
            if(inference_slot&&hookless_complete)hookless_complete(0);
            result=passthrough(end,"unsupported OpenXR projection/layers");
        }
        else {
            std::unique_lock capture_lock(capture_mutex);
            job->id=id;
            job->multiplier=multiplier.load();
            job->automatic_multiplier=automatic_multiplier.load();
            const auto capture_result=backend->capture(job,projection,inference_slot,external,hookless_complete);
            if(capture_result==CaptureOutcome::busy){
                std::lock_guard lock(mx);++skipped;++ended;changed.notify_all();return XR_SUCCESS;
            }
            const bool good=capture_result==CaptureOutcome::captured;
            if(!good){capture_lock.unlock();result=passthrough(end,"incomplete native color capture / changed swapchain layout");}
            else {
                job->space=job->layers.scene_space();job->blend=end->environmentBlendMode;
                std::lock_guard lock(mx);job->id=id;job->epoch=epoch;job->handed_off_at=TimingClock::now();frame_trace(id).handed_off=job->handed_off_at;
                // Keep the debug overlay on native submissions too. Native
                // snapshots are immutable once published; use the last proven
                // FG region (one native frame behind for dynamic gaze).
                for(unsigned eye=0;eye<2;++eye)job->color[eye]->fg_border=last_fg_borders[eye];
                Pair native;native.native_id=native.current_id=id;native.ready=job->ready;
                native.native_color_ready=native.current_color_ready=job->color_ready;
                native.native_final_ready=native.current_final_ready=job->final_color_ready;
                native.native_captured=native.current_captured=job->captured_at;
                native.native=native.current=job->color;native.native_views=native.current_views=job->views;
                native.layers=job->layers;native.auxiliary=job->auxiliary;native.space=job->space;native.blend=job->blend;
                latest_native=std::move(native);
                if(native11&&job->guides_valid&&!job->policy.release_point)job->policy.release_point=std::min(native11_release_point.load(),7u);
                if(inference_slot){pending.push_back(job);while(pending.size()>max_pending()){pending.front()->clear_guides();pending.pop_front();++skipped;}}
                ++frames;held_valid=true;changed.notify_all();
            }
        }
        {std::lock_guard lock(mx);if(!stopping)++ended;changed.notify_all();}
        return result;
    }
    static std::array<XrCompositionLayerProjectionView,2> midpoint(
        const std::array<XrCompositionLayerProjectionView,2>& a,const std::array<XrCompositionLayerProjectionView,2>& b,float t=.5f) {
        auto result=b;
        for(unsigned eye=0;eye<2;++eye) {
            auto& p=result[eye].pose;for(unsigned k=0;k<3;++k)(&p.position.x)[k]=(1-t)*(&a[eye].pose.position.x)[k]+t*(&b[eye].pose.position.x)[k];
            auto x=a[eye].pose.orientation,y=b[eye].pose.orientation;
            if(x.x*y.x+x.y*y.y+x.z*y.z+x.w*y.w<0)y={-y.x,-y.y,-y.z,-y.w};
            x={x.x*(1-t),x.y*(1-t),x.z*(1-t),x.w*(1-t)};y={y.x*t,y.y*t,y.z*t,y.w*t};
            const float n=std::sqrt((x.x+y.x)*(x.x+y.x)+(x.y+y.y)*(x.y+y.y)+(x.z+y.z)*(x.z+y.z)+(x.w+y.w)*(x.w+y.w));
            p.orientation=n>0?XrQuaternionf{(x.x+y.x)/n,(x.y+y.y)/n,(x.z+y.z)/n,(x.w+y.w)/n}:b[eye].pose.orientation;
            for(unsigned k=0;k<4;++k)(&result[eye].fov.angleLeft)[k]=(1-t)*(&a[eye].fov.angleLeft)[k]+t*(&b[eye].fov.angleLeft)[k];
        }
        return result;
    }

    // Called by the generation thread as soon as both eye outputs are safe
    // for ordered publication: a submitted DX12 fence or D3D11 context order.
    void announce_generated_phase(const std::shared_ptr<Native>& previous,const std::shared_ptr<Native>& job,
                                  Pair& pair,std::shared_ptr<FrameCompletion> completion) {
        std::lock_guard lock(mx);
        if(stopping||degraded||job->epoch!=epoch||!previous||pair.direct_xr||(pair.progress&&pair.progress->failed))return;
        const bool first=!pair.progress;
        if(first){pair.progress=std::make_shared<Pair::Progress>();inflight_progress=pair.progress;}
        const unsigned index=pair.generated_index;
        auto& phase=pair.progress->phases[index];phase.images=pair.generated;
        phase.completion=std::move(completion);phase.submitted_at=pair.submitted_at;
        if(first){
            Pair published=pair;published.generated_index=0;published.interpolated=true;
            published.native_id=previous->id;published.native=previous->color;
            published.native_color_ready=previous->color_ready;published.native_final_ready=previous->final_color_ready;
            published.native_captured=previous->captured_at;published.native_views=previous->views;
            published.generated_views=midpoint(previous->views,job->views,published.fraction());
            ready.push_back(std::move(published));while(ready.size()>max_pending())ready.pop_front();
            if(active_release_point==3u||active_release_point==6u||active_release_point==7u)awaiting_generated_submission=job->id;
            if(!fg_active)transition_until=ticks+8;
        }
        changed.notify_all();
    }
    bool phase_ready(const Pair& pair,unsigned index) const {
        // Hold mx. Non-streaming backends retain their completed-batch path.
        if(!pair.progress)return true;
        if(pair.progress->failed||index>=pair.generated_count)return false;
        const auto& p=pair.progress->phases[index];
        return p.images[0]&&p.images[1]&&p.completion&&p.completion->ready();
    }
    bool phase_presentable(const Pair& pair,unsigned index) const {
        // Hold mx. The phase retains its actual submission queue, independent
        // of later queue-mode changes. On the game/binding queue, uploads are
        // ordered after the phase without waiting for GPU completion on CPU.
        if(!pair.progress)return true;
        if(pair.progress->failed||index>=pair.generated_count)return false;
        const auto& p=pair.progress->phases[index];
        return p.images[0]&&p.images[1]&&p.completion&&
            (p.completion->ordered_on(queue.Get())||p.completion->ready());
    }

    void generation_loop() noexcept {
        SetThreadDescription(GetCurrentThread(),L"Cheeky FG completion");
        std::shared_ptr<Native> previous;bool reset_history=true;
        unsigned measured_diagnostics=UINT32_MAX;std::uint64_t logged_samples{};
        try {for(;;) {
            std::shared_ptr<Native> job;
            {std::unique_lock lock(mx);changed.wait(lock,[&]{return stopping||!pending.empty();});if(stopping)break;
             job=pending.front();pending.pop_front();inflight_id=job->id;inflight_started=TimingClock::now();inflight_generated_gate_released=false;inflight_progress.reset();
             inflight_release_point=job->guides_valid?job->policy.release_point:0u;inflight_prepared=false;
             if(job->guides_valid){
                 if(active_release_point!=job->policy.release_point)cadence_window={};
                 active_release_point=job->policy.release_point;
             }changed.notify_all();
             }
            if(measured_diagnostics!=unsigned(job->latency_diagnostics)){
                measured_diagnostics=unsigned(job->latency_diagnostics);logged_samples=0;
                std::lock_guard lock(mx);gpu_ready_ages={};gpu_ready_samples=predicted_samples=prediction_late=0;
            }
            if(job->guides_valid) {
                const auto point=job->policy.release_point;
                const bool changed_policy=!release_policy_known||last_release_point!=point;
                if(changed_policy) {
                    backend->reset_statistics(false);logged_samples=0;
                    {std::lock_guard lock(mx);gpu_ready_ages={};gpu_ready_samples=predicted_samples=prediction_late=0;}
                    if(release_policy_known)backend->reset_statistics(true);
                    last_release_point=point;
                    if(trace)trace(point==5u?"FG native release=buffer capacity only (no FG release gate)":
                        point==3u?"FG native release=after generated frame OpenXR submission (Lua; histories retained)":
                        point==6u?"FG native release=after first generated frame OpenXR submission (Lua; histories retained)":
                        point==7u?"FG native release=after second generated frame OpenXR submission (Lua; histories retained)":
                        point==2u?"FG native release=after stereo GPU completion (Lua; histories retained)":
                        point==1u?"FG native release=after both-eye GPU preparation (Lua; histories retained)":
                        point==4u?"FG native release=4 ms after FG command submission (Lua; CPU timer, no GPU wait)":
                        "FG native release=after submission (Lua; default overlap)");
                }
                release_policy_known=true;
            }
            Pair pair;pair.native_id=pair.current_id=job->id;pair.native=pair.current=job->color;
            std::shared_ptr<DirectRequest> direct_job;
            struct CompleteRequest {BufferedPresenter& owner;std::shared_ptr<DirectRequest>& request;
                ~CompleteRequest(){if(request){std::lock_guard lock(owner.mx);request->complete=true;owner.changed.notify_all();}}} complete_request{*this,direct_job};
            pair.native_color_ready=pair.current_color_ready=job->color_ready;
            pair.native_final_ready=pair.current_final_ready=job->final_color_ready;
            pair.native_captured=pair.current_captured=job->captured_at;
            pair.handed_off_at=job->handed_off_at;pair.worker_started_at=TimingClock::now();
            pair.native_views=pair.current_views=job->views;pair.layers=job->layers;pair.auxiliary=job->auxiliary;
            pair.space=job->space;pair.blend=job->blend;pair.ready=job->ready;
            {
                const auto sample=backend->application_timing();
                const auto maximum=backend->maximum_multiplier(*job);
                const auto publication_cost=overhead();
                std::lock_guard lock(mx);
                auto_multiplier.mode(job->automatic_multiplier,job->multiplier,maximum);
                if(job->automatic_multiplier){
                    if(job->guides_valid&&previous&&previous->epoch==job->epoch&&previous->id+1==job->id&&telemetry.gpu_valid){
                        const double now=std::chrono::duration<double,std::milli>(TimingClock::now().time_since_epoch()).count();
                        const auto generated_ms=(job->policy.optical_flow?telemetry.total_ms:telemetry.total_last_ms)+publication_cost.publication_last_ms+publication_cost.upload_last_ms;
                        if(auto_multiplier.observe(now,sample,generated_ms,display_clock.display_ms(),xr_wait.mean()+present_cpu.mean())&&trace){char message[180]{};
                            sprintf_s(message,"FG Auto selected=%ux max=%ux render_ms=%.3f generated_ms=%.3f reason=%u (histories retained)",
                                auto_multiplier.selected,maximum,auto_multiplier.render_ms,auto_multiplier.fg_ms,auto_multiplier.reason);trace(message);}
                    }
                    job->multiplier=auto_multiplier.selected;
                }
            }
            const auto generate_started=TimingClock::now();const auto rebuilds_before=optical::rebuilds.load();
            worker_wait_before_ms=worker_wait_complete_ms=0;
            const auto generation=backend->generate(previous,job,pair,reset_history,logged_samples,direct_job);
            {std::lock_guard lock(mx);auto& t=frame_trace(job->id);t.worker_start=pair.worker_started_at;t.worker_end=TimingClock::now();
                t.wait_before_ms=worker_wait_before_ms;t.wait_complete_ms=worker_wait_complete_ms;}
            if(const auto generate_ms=elapsed_ms(generate_started);generate_ms>100.&&trace){char message[192]{};
                sprintf_s(message,"FG worker stall: native=%llu generate_ms=%.1f optical_flow_session_rebuilds=%llu",
                    static_cast<unsigned long long>(job->id),generate_ms,static_cast<unsigned long long>(optical::rebuilds.load()-rebuilds_before));trace(message);}
            const auto success=generation.success,attempted=generation.attempted,history_was_reset=generation.history_reset;
            if(success){pair.interpolated=true;pair.native_id=previous->id;pair.native=previous->color;
                pair.native_color_ready=previous->color_ready;
                pair.native_final_ready=previous->final_color_ready;
                pair.native_captured=previous->captured_at;
                pair.native_views=previous->views;pair.generated_views=midpoint(previous->views,job->views,pair.fraction());
                pair.completed_at=TimingClock::now();}
            // Completed NGX warm-up advances history even when it supplies no
            // interpolated output. An early return or failed batch does not.
            reset_history=!attempted;if(job->latency_diagnostics)collect_color_stamps();
            measure_worker_memory();
            {std::lock_guard lock(mx);if(stopping)break;
                // Previous-frame history needs colors/poses, not its original
                // guides. Release completed leases now instead of pinning raw
                // textures in reusable slots or temporal history indefinitely.
                // The D3D11 shader samples the previous interval's depth.
                backend->retire_guides(*job);
                if(success&&!pair.direct_xr)for(unsigned eye=0;eye<2;++eye)last_fg_borders[eye]=pair.generated[eye]->fg_border;
                if(pair.progress){pair.progress->complete=true;pair.progress->failed|=!success||job->epoch!=epoch||degraded;
                    if(pair.progress->failed){std::erase_if(ready,[&](const auto& p){return p.progress==pair.progress;});finish_generated_gate(job->id,false);}}
                if(success&&!pair.progress&&!(pair.direct_xr&&direct_job&&direct_job->queued)&&
                    (active_release_point==3u||active_release_point==6u||active_release_point==7u)&&job->epoch==epoch&&!degraded)awaiting_generated_submission=job->id;
                inflight_id=0;inflight_release_point=0;inflight_prepared=false;
                const auto status=backend->generation_status();
                CheekyFgPerformance gpu_performance;backend->snapshot(gpu_performance,extra_gpu,experiment_state);
                if(trace&&experiment_state.latency_valid&&experiment_state.latency_samples!=logged_samples){
                    const auto copies=experiments();
                    logged_samples=experiment_state.latency_samples;
                    const auto& d=experiment_state;char message[768]{};
                    sprintf_s(message,"FG calibrated latency sample=%llu rejected=%llu release=%u queue=%s mean_ms: queue_entry=%.3f capture_dependency=%.3f ready_to_fg=%.3f gpu_work=%.3f gpu_to_observed=%.3f enqueue_total=%.3f submit_total=%.3f; copies(full/direct/crop)=%llu/%llu/%llu uploads(copies/batches)=%llu/%llu",
                        d.latency_samples,d.latency_rejected,active_release_point,queue_mode_name(reported_queue_mode),
                        d.queue_entry.mean,d.capture_dependency.mean,d.ready_to_fg.mean,d.gpu_work.mean,d.gpu_to_observed.mean,d.enqueue_to_observed.mean,d.submit_to_observed.mean,
                        copies.full_color_copies,copies.direct_eye_copies,copies.eye_crop_copies,copies.upload_copies,copies.upload_batches);trace(message);
                }
                release_deferred_credit();
                if(job->epoch!=epoch||degraded){previous.reset();reset_history=true;continue;}
                if(pair.direct_xr){
                    // An early direct handoff is immutable and may already
                    // have been moved by the presenter while NGX retired.
                    if(!direct_job->queued){direct_job->success=success;if(success){++direct_stats.frames;direct_job->result=std::move(pair);}
                        else{++direct_stats.fallbacks;direct_stats.last_path=3;note_direct_failure_locked(CheekyFgDirectReason::generation);}}
                }
                else{
                    if(trace&&job->latency_diagnostics){
                        char publication[420]{};
                        sprintf_s(publication,
                            "FG XR-PAIR epoch=%llu native=%llu current=%llu interpolated=%u "
                            "render_frame=%llu guide_token=%llu reset=%u success=%u worker_serial=%llu",
                            static_cast<unsigned long long>(epoch),
                            static_cast<unsigned long long>(pair.native_id),
                            static_cast<unsigned long long>(pair.current_id),
                            pair.interpolated?1u:0u,
                            static_cast<unsigned long long>(job->render_frame),
                            static_cast<unsigned long long>(job->guide_token),
                            history_was_reset?1u:0u,success?1u:0u,
                            static_cast<unsigned long long>(status.serial));
                        trace(publication);
                    }
                    if(!pair.progress){ready.push_back(std::move(pair));while(ready.size()>max_pending())ready.pop_front();}
                }
                if(success)completed_pair_id=job->id;
                // Once real interpolation is available, briefly stop the
                // speculative native mailbox from running ahead of its first
                // usable pair. Inference/native failures release this priming.
                if(success&&!fg_active)transition_until=ticks+8;
                if(status.failed||!job->guides_valid)transition_until=0;
                if(status.failed)pacing_active=false;
                const auto xr_ms=telemetry.xr_wait_ms,period_ms=telemetry.xr_period_ms;
                const auto color_token=telemetry.color_token,guide_token=telemetry.guide_token,mismatches=telemetry.mismatched_guides;
                const auto color_frame=telemetry.color_render_frame,guide_frame=telemetry.guide_render_frame;
                telemetry=gpu_performance;telemetry.copy_wait_ms=0;telemetry.xr_wait_ms=xr_ms;telemetry.xr_period_ms=period_ms;
                telemetry.color_token=color_token;telemetry.guide_token=guide_token;telemetry.mismatched_guides=mismatches;
                telemetry.color_render_frame=color_frame;telemetry.guide_render_frame=guide_frame;
                telemetry.source_depth_format=job->source_depth;telemetry.source_motion_format=job->source_motion;
                state=success?CheekyFgState::generating:status.failed?(status.capability_ready?CheekyFgState::generation_failed:CheekyFgState::ngx_unavailable):
                    job->guides_valid?CheekyFgState::waiting_for_history:job->guide_state;
                if(job->guide_state==CheekyFgState::generation_failed&&!job->guides_valid)
                    sprintf_s(diagnostic,"FG guide snapshot unavailable: depth=%u motion=%u; native presentation continues",job->source_depth,job->source_motion);
                else if(job->guide_state==CheekyFgState::guide_frame_mismatch&&!job->guides_valid)
                    strcpy_s(diagnostic,"Waiting for matching color and guide frames. See the plugin frame-source diagnostics; native presentation continues.");
                else strcpy_s(diagnostic,status.diagnostic);changed.notify_all();}
            previous=std::move(job);
        }}catch(...) {std::lock_guard lock(mx);state=CheekyFgState::generation_failed;degraded=true;fg_active=pacing_active=false;
            if(auto p=inflight_progress.lock())p->failed=true;
            discard_pending();ready.clear();awaiting_generated_submission=0;inflight_id=0;inflight_release_point=0;inflight_prepared=false;++epoch;strcpy_s(diagnostic,"FG background processing failed; using original native submissions");changed.notify_all();}
        backend->finish_generation();
    }

    // Native D3D11 backend: private chain matching the frame, then one copy.
    // TODO(perf): render generated frames straight into the acquired image.

    void wake_producer_locked() {
        // An availability notification, not a timer credit. wait_frame may
        // proceed as soon as its dependencies clear even without this call.
        if(!stopping&&!raw){
            if(release_gate_active()||(pacing_active&&unsubmitted_native()))mark_credit_deferred();
            else credits=1;
        }
        changed.notify_all();
    }
    bool producer_has_capacity() const {
        // Hold mx. Backpressure depends on actual owned work, never elapsed
        // runtime intervals. Resource reuse remains fence-protected.
        if(pending.size()>=max_pending()||ready.size()>=max_pending())return false;
        for(std::size_t i=0;i<slot_count();++i)
            if(!slots[i]||(slots[i].use_count()==1&&capture_completed(slots[i]->ready)))return true;
        return false;
    }
    void release_loop() noexcept {
        SetThreadDescription(GetCurrentThread(),L"Cheeky FG release watchdog");
        try {
            std::unique_lock lock(mx);
            std::int64_t previous_target{};
            while(!stopping) {
                const auto now=TimingClock::now();
                const auto native_ns=display_clock.native_ns(pacing_active?active_multiplier:1u);
                native_clock_ms=double(native_ns)/1e6; // Diagnostic target, not a production timer.
                if(native_ns&&native_ns!=previous_target){
                    previous_target=native_ns;
                    if(trace){char message[180]{};sprintf_s(message,
                        "FG demand-driven production: refresh_hz=%.3f source=%s multiplier=%u native_target_ms=%.3f",
                        1000./display_clock.display_ms(),display_clock.source(),pacing_active?active_multiplier:1u,double(native_ns)/1e6);trace(message);}
                }
                // This thread only maintains explicit release deadlines and
                // the stall watchdog. It never schedules normal game frames.
                if(release_delay_until!=TimingClock::time_point{}&&now>=release_delay_until) {
                    release_delay_until={};
                    release_deferred_credit();
                    changed.notify_all();
                }
                release_gate_watchdog(now);
                const auto watchdog_wake=now+std::chrono::milliseconds(50);
                auto wake=release_delay_until!=TimingClock::time_point{}?
                    std::min(watchdog_wake,release_delay_until):watchdog_wake;
                changed.wait_until(lock,wake);
            }
        }catch(...) {std::lock_guard lock(mx);terminal=XR_ERROR_RUNTIME_FAILURE;stopping=true;changed.notify_all();}
    }
    // Release point 5 never holds the game: its jobs neither gate nor defer credits.
    bool unsubmitted_native() const noexcept {
        return (inflight_id>submitted_native_id&&inflight_release_point!=5u)||
            (!pending.empty()&&pending.back()->id>submitted_native_id&&pending.back()->policy.release_point!=5u);
    }
    bool release_gate_active() const noexcept {
        // Hold mx. Inspect pending jobs as well as the worker's latched policy
        // so pre-existing credits cannot let a render race worker pickup.
        if(stopping||raw||degraded)return false;
        if(awaiting_generated_submission)return true;
        if(release_delay_until!=TimingClock::time_point{}&&TimingClock::now()<release_delay_until)return true;
        if(inflight_id&&inflight_release_point!=5u&&(inflight_id>submitted_native_id||inflight_release_point==2u||
            (!inflight_generated_gate_released&&(inflight_release_point==3u||inflight_release_point==6u||inflight_release_point==7u))||
            (inflight_release_point==1u&&!inflight_prepared)))return true;
        return std::any_of(pending.begin(),pending.end(),[&](const auto& job){
            return job->guides_valid&&job->policy.release_point!=5u&&(job->policy.release_point!=0||job->id>submitted_native_id);
        });
    }
    bool generated_submission_pending() const noexcept {
        return awaiting_generated_submission||(inflight_id&&!inflight_generated_gate_released&&(inflight_release_point==3u||inflight_release_point==6u||inflight_release_point==7u))||
            std::any_of(pending.begin(),pending.end(),[](const auto& job){return job->guides_valid&&(job->policy.release_point==3u||job->policy.release_point==6u||job->policy.release_point==7u);});
    }
    void mark_credit_deferred() noexcept {
        // Hold mx. Measure a blocked release condition; repeated notifications
        // share one pending wake rather than banking extra renders.
        if(!credit_deferred){++deferred_credits;credit_deferred=true;credit_deferred_since=TimingClock::now();}
    }
    void release_deferred_credit() noexcept {
        // Hold mx. Wake the blocked render immediately when its actual
        // release condition clears. There is no bank of missed timer ticks.
        if(credit_deferred&&!stopping&&!raw&&!unsubmitted_native()&&!release_gate_active()) {
            credits=std::max(credits,1u);credit_deferred=false;++released_deferred_credits;
            if(credit_deferred_since!=TimingClock::time_point{})credit_deferral.add(elapsed_ms(credit_deferred_since));
            credit_deferred_since={};changed.notify_all();
        }
    }
    void inference_submitted() noexcept {
        std::lock_guard lock(mx);submitted_native_id=std::max(submitted_native_id,inflight_id);
        if(inflight_release_point==4u) {
            release_delay_until=TimingClock::now()+std::chrono::milliseconds(4);
            changed.notify_all(); // Install the explicit soft-release deadline.
        }
        release_deferred_credit();changed.notify_all();
    }
    void inference_prepared() noexcept {
        std::lock_guard lock(mx);inflight_prepared=true;release_deferred_credit();changed.notify_all();
    }
    void wake_producer() {std::lock_guard lock(mx);wake_producer_locked();}
    std::deque<Pair>::iterator newest_ready_pair(double position) {
        auto selected=ready.end();
        // The next pair in order, not the newest: a backlog is shown, not skipped,
        // so every real frame keeps its generated frames.
        for(auto it=ready.begin();it!=ready.end();++it){
            // A submitted phase covers its capture dependencies too. Shared
            // queue order permits upload before either fence finishes.
            const bool presentable=it->progress?phase_presentable(*it,0):capture_completed(it->ready);
            if(presentable&&double(it->native_id)>=position){selected=it;break;}
        }
        return selected;
    }
    bool render(Pair& pair,const std::array<Image,2>& images,
                const std::array<XrCompositionLayerProjectionView,2>& original_views,bool generated,XrTime time,
                XrResult& result,bool& closed) {
        // Streaming DX12 phases each own a stereo immutable-output fence.
        // Native pairs retain their original producer dependency.
        {std::lock_guard lock(mx);
            if(pair.progress){
                const unsigned index=generated?pair.generated_index:0u;
                if(!phase_presentable(pair,index))return false;
                if(generated){const auto& phase=pair.progress->phases[index];
                    pair.submitted_at=phase.submitted_at;
                    // Queued does not mean GPU-complete. Do not report this
                    // CPU submission as an observed completion timestamp.
                    pair.completed_at=phase.completion->ready()?TimingClock::now():TimingClock::time_point{};
                }
            }else if(!capture_completed(pair.ready))return false;
        }
        burst_native=pair.native_id;burst_current=pair.current_id;burst_generated=generated;
        burst_presentation=ticks;
        burst.begin_submission();
        if(latency_diagnostics.load()!=0)collect_native_submission_probes();
        auto views=original_views;auto layers=pair.layers;
        const bool published=backend->publish(pair,images,views,layers,generated);
        if(!published)return false;
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=pair.space;
        projection.layerFlags=layers.scene_flags;projection.viewCount=2;projection.views=views.data();
        auto headers=layers.replace_scene(projection);XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=time;
        end.environmentBlendMode=pair.blend;end.layerCount=unsigned(headers.size());end.layers=headers.data();
        std::uint64_t display_cpu{};
        if(generated&&xr.time_to_qpc){LARGE_INTEGER qpc{};
            if(XR_SUCCEEDED(xr.time_to_qpc(xr.time_instance,time,&qpc))&&qpc.QuadPart>0)display_cpu=std::uint64_t(qpc.QuadPart);}
        wake_producer();closed=true;const auto submitted_at=TimingClock::now();const auto submitted_cpu=LatencyProbe::cpu_now();
#ifdef CHEEKY_FG_SUBMISSION_OBSERVER
        CHEEKY_FG_SUBMISSION_OBSERVER(generated);
#endif
        result=runtime_end_frame(&end);
        {std::lock_guard lock(mx);(generated?generated_xr_end:native_xr_end).add(elapsed_ms(submitted_at));
            if(!generated&&XR_SUCCEEDED(result)&&latency_diagnostics.load()!=0) {
                // The native eyes have been submitted to the OpenXR runtime.
                // The GPU may still be working. Resolve their completed GPU
                // timestamps on a subsequent presenter iteration.
                pending_native_submissions.push_back({pair.native_color_ready,pair.native_final_ready,submitted_cpu,GetTickCount64()});
                if(pair.native_captured!=TimingClock::time_point{}&&pair.native_captured<=submitted_at)
                    native_capture_to_submit.add(std::chrono::duration<double,std::milli>(submitted_at-pair.native_captured).count());
            }}
        if(XR_SUCCEEDED(result)){std::lock_guard lock(mx);note_frame_age(pair,generated,submitted_at);
            if(generated){note_gpu_ready_age(pair,submitted_cpu,display_cpu);
                // 3 releases after the interval's last generated frame, 6 after its
                // first, 7 after its second (the only one at 2x).
                if(pair.generated_index+1==pair.generated_count||(active_release_point==6u&&pair.generated_index==0)||
                   (active_release_point==7u&&pair.generated_index==1))finish_generated_gate(pair.current_id,true);}}
        return XR_SUCCEEDED(result);
    }
    bool fresh_content(const Pair& active,bool have_active,bool generated_phase,double position) {
        // Hold mx. A pair whose native image was already displayed can still
        // contribute its interpolation; do not submit that native image twice.
        if(have_active&&active.progress&&active.progress->failed)return true; // Retire failed partial interval below.
        if(stalled_native(position))return true;
        if(have_active&&generated_phase)return phase_presentable(active,active.generated_index);
        if(have_active&&double(active.native_id)>position)return true;
        const auto selected=newest_ready_pair(position);
        if(selected!=ready.end()&&(double(selected->native_id)>position||
            (selected->interpolated&&double(selected->current_id)>position)))return true;
        return !fg_active&&ticks>=transition_until&&latest_native&&capture_completed(latest_native->ready)&&
            double(latest_native->native_id)>position;
    }
    bool await_content(const Pair& active,bool have_active,bool generated_phase,double position) {
        std::unique_lock lock(mx);
        bool waited_for_content=false;
        const auto started_wait=TimingClock::now();
        // Only fresh content is submitted. Until the first runtime frame the
        // game may itself wait for this presenter's first prediction, so the
        // first tick never waits for content.
        while(!stopping&&!raw&&ticks&&!(direct_request&&!direct_request->decided)&&!fresh_content(active,have_active,generated_phase,position)) {
            waited_for_content=true;
            // A generated-submission gate for an interval the presenter has
            // already passed (a stall showed its native frame) can never be
            // met; holding it would leave the game and this wait stuck.
            if(awaiting_generated_submission&&double(awaiting_generated_submission)<=position){
                if(trace)trace("FG generated gate cancelled: presenter already showed that native frame");
                finish_generated_gate(awaiting_generated_submission,false);continue;}
            // FG completion and new captures notify changed. Poll only an
            // outstanding producer fence (native-only startup/fallback), not
            // a quiet mailbox: idle fresh-only mode must not spin at 1 kHz.
            const bool producer_pending=(have_active&&generated_phase&&active.progress&&!phase_presentable(active,active.generated_index))||
                (!fg_active&&latest_native&&!capture_completed(latest_native->ready))||
                std::any_of(ready.begin(),ready.end(),[&](const auto& pair){return pair.progress?!phase_presentable(pair,0):!capture_completed(pair.ready);});
            // A running generation may stall; recheck so a newer native frame can be shown.
            if(producer_pending)changed.wait_for(lock,std::chrono::milliseconds(1));
            else if(inflight_id)changed.wait_for(lock,std::chrono::milliseconds(5));
            else changed.wait(lock);
        }
        if(waited_for_content)presenter_content_wait.add(elapsed_ms(started_wait));
        last_content_wait_ms=waited_for_content?elapsed_ms(started_wait):0.;
        return !stopping;
    }
    void presentation_loop() noexcept {
        SetThreadDescription(GetCurrentThread(),L"Cheeky OpenXR presentation");
        struct CompleteBorrowedFrame {
            BufferedPresenter& owner;
            ~CompleteBorrowedFrame() {
                std::lock_guard lock(owner.mx);
                if(owner.raw){owner.raw=nullptr;owner.raw_done=true;
                    owner.raw_result=XR_FAILED(owner.terminal)?owner.terminal:XR_ERROR_SESSION_NOT_RUNNING;}
                owner.changed.notify_all();
            }
        } complete_borrowed_frame{*this};
        Pair active;bool have_active{},generated_phase{};double last_position{};
        XrTime late_last_display{}; // Late-frame trace: previous runtime frame's display time.
        TimingClock::time_point next_refresh_query{};
        try {for(;;) {
            // Runtime frame ownership cannot transfer midway through the old
            // native frame. BeginFrame may discard an unfinished frame rather
            // than wait for its EndFrame, which would race our own EndFrame.
            // Wait before acquiring a runtime frame so stop cannot strand a wait.
            {std::unique_lock lock(mx);
                changed.wait(lock,[&]{return stopping||!native_frame_pending;});
                if(stopping)break;}
            if(!await_content(active,have_active,generated_phase,last_position))break;
            XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};XrFrameState frame{XR_TYPE_FRAME_STATE};
            const auto start=TimingClock::now();auto result=xr.wait_frame(session,&wait,&frame);const auto waited_ms=elapsed_ms(start);
            if(XR_FAILED(result)){runtime_failure("runtime xrWaitFrame",result);break;}
            const auto present_start=TimingClock::now();
            burst.log=trace;burst.poll_keys();
            XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};result=runtime_dispatch([&]{return xr.begin_frame(session,&begin);});
            {std::lock_guard lock(mx);runtime_begin.add(elapsed_ms(present_start));}
            if(XR_FAILED(result)){runtime_failure("runtime xrBeginFrame",result);break;}
            double refresh_hz{};
            if(xr.get_refresh_rate&&present_start>=next_refresh_query){
                next_refresh_query=present_start+std::chrono::seconds(1);
                float hz{};
                if(XR_SUCCEEDED(runtime_dispatch([&]{return xr.get_refresh_rate(session,&hz);})))refresh_hz=hz;
            }
            const XrFrameEndInfo* raw_frame{};bool stop_now{};
            {std::lock_guard lock(mx);raw_frame=raw;stop_now=stopping;
                if(have_active&&active.progress&&active.progress->failed){have_active=false;generated_phase=false;fg_active=false;active={};}
                if(awaiting_generated_submission&&(last_position>=double(awaiting_generated_submission)||frame.shouldRender!=XR_TRUE))
                    finish_generated_gate(awaiting_generated_submission,false);
                if(frame.shouldRender!=XR_TRUE&&direct_request&&!direct_request->decided){
                    note_direct_failure_locked(CheekyFgDirectReason::cancelled,0,direct_request.get());
                    direct_request->decided=true;direct_request.reset();++direct_stats.fallbacks;direct_stats.last_path=3;changed.notify_all();}
                predicted=frame;telemetry.xr_period_ms=double(frame.predictedDisplayPeriod)/1000000.;
                display_clock.observe_period(frame.predictedDisplayPeriod);
                display_clock.observe_refresh(refresh_hz);
                xr_wait.add(waited_ms);telemetry.xr_wait_ms=xr_wait.mean();++ticks;changed.notify_all();
            }
            Pair direct_pair;
            const bool have_direct=frame.shouldRender==XR_TRUE&&backend->service_direct_output(last_position,direct_pair);
            {std::lock_guard lock(mx);raw_frame=raw;stop_now=stopping;
                if(have_direct&&!raw_frame&&!stop_now&&!degraded){active=std::move(direct_pair);have_active=true;generated_phase=true;fg_active=pacing_active=true;active_multiplier=active.generated_count+1;
                    while(!ready.empty()&&ready.front().current_id<=active.current_id)ready.pop_front();}
                else if(!raw_frame&&!stop_now&&!degraded&&stalled_native(last_position)) {
                    active=*latest_native;have_active=true;generated_phase=false;
                    if(trace&&(++stalled_natives<=3||stalled_natives%60==0)){char message[160]{};
                        sprintf_s(message,"FG presenter: generation stalled %.0f ms; showing native=%llu (count=%llu)",
                            std::chrono::duration<double,std::milli>(TimingClock::now()-inflight_started).count(),
                            static_cast<unsigned long long>(active.native_id),static_cast<unsigned long long>(stalled_natives));trace(message);}
                }
                else if(!generated_phase&&!raw_frame&&!stop_now&&!degraded) {
                    auto selected=newest_ready_pair(last_position);
                    if(selected!=ready.end()) {
                        active=std::move(*selected);ready.erase(ready.begin(),std::next(selected));have_active=true;
                        changed.notify_all(); // A queued interval no longer consumes production capacity.
                        fg_active=active.interpolated;
                        if(active.interpolated&&double(active.native_id)<=last_position&&
                            double(active.current_id)>last_position)generated_phase=true;
                        // A transient skipped/reset pair must not accelerate
                        // native production to 1x and overload FG again.
                        if(fg_active){pacing_active=true;active_multiplier=active.generated_count+1;}
                    }
                    // Prioritize a usable interpolation pair. During NGX
                    // startup, keep showing new native frames even when every
                    // inference lease is busy. Never rewind to a stale pair.
                    if(!fg_active&&!generated_submission_pending()&&ticks>=transition_until&&latest_native&&capture_completed(latest_native->ready)&&
                       double(latest_native->native_id)>=last_position&&
                       (!have_active||latest_native->native_id>active.native_id)) {
                        active=*latest_native;have_active=true;
                    }
                }
            }
            bool submitted{},is_generated{},closed{},stale{};
            std::uint64_t trace_native_id{},trace_current_id{},trace_source_left{},trace_source_right{};
            bool trace_direct{};
            double trace_source_position{-1.};
            if(raw_frame&&!stop_now) {
                auto end=*raw_frame;end.displayTime=frame.predictedDisplayTime;
                result=runtime_end_frame(&end);submitted=closed=true;
                {std::lock_guard lock(mx);raw_result=result;raw=nullptr;raw_done=true;have_active=false;
                    generated_phase=false;last_position=0;changed.notify_all();}
                presentation_trace_last_time=0;presentation_trace_last_position=-1.;
                wake_producer();
            }else if(have_active&&!stop_now&&frame.shouldRender==XR_TRUE) {
                is_generated=generated_phase&&active.interpolated;
                auto images=is_generated?(active.generated_index?active.extra_generated[active.generated_index-1]:active.generated):active.native;
                if(is_generated&&active.progress){std::lock_guard lock(mx);images=active.progress->phases[active.generated_index].images;}
                const auto views=is_generated?midpoint(active.native_views,active.current_views,active.fraction()):active.native_views;
                stale=!is_generated&&double(active.native_id)==last_position;
                // Snapshot identity BEFORE the generated submission advances
                // active.native_id/current_views to the next real frame.
                trace_native_id=active.native_id;
                trace_current_id=active.current_id;
                trace_source_position=double(active.native_id)+(is_generated?active.fraction():0.);
                trace_source_left=images[0]?images[0]->identity:0;
                trace_source_right=images[1]?images[1]->identity:0;
                trace_direct=is_generated&&active.direct_xr;
                // A direct request may wake a tick only to fall back. Never use
                // that wake to resubmit the preceding native frame.
                if(!stale)submitted=render(active,images,views,is_generated,frame.predictedDisplayTime,result,closed);
                if(submitted) {
                    last_position=double(active.native_id)+(is_generated?active.fraction():0.);
                    if(is_generated&&++active.generated_index==active.generated_count){active.native=active.current;active.native_views=active.current_views;active.native_id=active.current_id;
                        active.native_color_ready=active.current_color_ready;
                        active.native_final_ready=active.current_final_ready;
                        active.native_captured=active.current_captured;generated_phase=false;}
                    else generated_phase=active.interpolated&&last_position<double(active.current_id);
                }
            }
            if(!closed) {
                XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};empty.displayTime=frame.predictedDisplayTime;
                empty.environmentBlendMode=have_active?active.blend:XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                wake_producer();
                result=runtime_end_frame(&empty);
            }
            // The runtime skipped a headset slot: print the late frame's
            // timeline next to its neighbours (rate-limited).
            if(trace&&!raw_frame&&frame.predictedDisplayPeriod>0&&late_last_display&&
               frame.predictedDisplayTime-late_last_display>frame.predictedDisplayPeriod*3/2) {
                std::lock_guard lock(mx);
                if(++late_frames<=30||late_frames%20==0) {
                    const auto late_id=trace_current_id?trace_current_id:trace_native_id;
                    char prev[256]{},late[256]{},next[256]{},message[1024]{};
                    describe_trace(prev,sizeof(prev),late_id-1);describe_trace(late,sizeof(late),late_id);describe_trace(next,sizeof(next),late_id+1);
                    const double step=double(frame.predictedDisplayTime-late_last_display)/1e6,period=double(frame.predictedDisplayPeriod)/1e6;
                    sprintf_s(message,"FG LATE FRAME: count=%llu skipped_slots=%.0f display_step_ms=%.1f shown=%s presenter_wait_ms=%.1f | prev %s | late %s | next %s",
                        static_cast<unsigned long long>(late_frames),step/period-1.,step,is_generated?"GEN":submitted?"NATIVE":"EMPTY",
                        last_content_wait_ms,prev,late,next);
                    trace(message);
                }
            }
            late_last_display=frame.predictedDisplayTime;
            // Capture each runtime frame, including empty submissions, so
            // silent skips cannot masquerade as a clean N/G/N cadence.
            // Use the existing FG Latency Diagnostics switch to avoid any
            // additional output or locking in ordinary gameplay.
            if(trace&&latency_diagnostics.load()!=0&&!raw_frame) {
                const bool valid=XR_SUCCEEDED(result);
                const bool backward_time=valid&&presentation_trace_last_time!=0&&
                    frame.predictedDisplayTime<=presentation_trace_last_time;
                const bool backward_content=valid&&submitted&&
                    trace_source_position<=presentation_trace_last_position;
                const bool gap=valid&&submitted&&
                    presentation_trace_last_position>=0&&trace_source_position>presentation_trace_last_position+1.01;
                const bool wrong_upload=valid&&submitted&&!trace_direct&&
                    (scene_chains[0].uploaded!=trace_source_left||
                     scene_chains[1].uploaded!=trace_source_right);
                if(backward_time||backward_content||wrong_upload)++presentation_trace_anomalies;
                if(valid){
                    presentation_trace_last_time=frame.predictedDisplayTime;
                    if(submitted)presentation_trace_last_position=trace_source_position;
                }
                ++presentation_trace_events;
                // Complete first 3,000 events (~25-40 s), then sample every
                // fourth event indefinitely. ALWAYS log anomalies and skips.
                const bool write=presentation_trace_events<=3000||
                    (presentation_trace_events%4)==0||backward_time||backward_content||
                    gap||wrong_upload||!submitted||!valid;
                if(write){
                    // The expected headset deadline is a prediction, not proof
                    // of compositor display. Negative lead means submitted late.
                    double predicted_lead_ms{-1.};
                    if(xr.time_to_qpc){
                        LARGE_INTEGER display_qpc{};
                        if(XR_SUCCEEDED(xr.time_to_qpc(xr.time_instance,frame.predictedDisplayTime,&display_qpc))&&
                           display_qpc.QuadPart>0){
                            const auto submit_qpc=LatencyProbe::cpu_now();
                            predicted_lead_ms=(double(display_qpc.QuadPart)-double(submit_qpc))*
                                1000./LatencyProbe::cpu_frequency();
                        }
                    }
                    // Generation publishes ready pairs under mx. Snapshot the
                    // queue and epoch rather than racing the producer thread.
                    std::uint64_t trace_epoch{},trace_ready{},trace_generated{};
                    {std::lock_guard lock(mx);
                        trace_epoch=epoch;trace_ready=ready.size();trace_generated=generated_count;}
                    char timeline[680]{};
                    sprintf_s(timeline,
                        "FG XR-TIMELINE event=%llu epoch=%llu xr_tick=%llu kind=%s native=%llu current=%llu "
                        "position=%.3f left_tex=%llu right_tex=%llu xr_left=%u xr_right=%u "
                        "upload_match=%u display_ns=%lld period_ns=%lld "
                        "should_render=%u xr_result=%ld forward=%u time_forward=%u gap=%u stale=%u "
                        "predicted_lead_ms=%.3f ready_pairs=%llu generated_total=%llu anomalies=%llu",
                        static_cast<unsigned long long>(presentation_trace_events),
                        static_cast<unsigned long long>(trace_epoch),static_cast<unsigned long long>(ticks),
                        submitted?(is_generated?"GEN":"NATIVE"):"EMPTY",
                        static_cast<unsigned long long>(trace_native_id),
                        static_cast<unsigned long long>(trace_current_id),trace_source_position,
                        static_cast<unsigned long long>(trace_source_left),
                        static_cast<unsigned long long>(trace_source_right),
                        scene_chains[0].last_uploaded_index,scene_chains[1].last_uploaded_index,
                        wrong_upload?0u:1u,
                        static_cast<long long>(frame.predictedDisplayTime),
                        static_cast<long long>(frame.predictedDisplayPeriod),
                        frame.shouldRender==XR_TRUE?1u:0u,static_cast<long>(result),
                        backward_content?0u:1u,backward_time?0u:1u,gap?1u:0u,
                        stale?1u:0u,predicted_lead_ms,static_cast<unsigned long long>(trace_ready),
                        static_cast<unsigned long long>(trace_generated),
                        static_cast<unsigned long long>(presentation_trace_anomalies));
                    trace(timeline);
                }
            }
            measure_presenter_memory();
            char age_log[512]{},stage_log[512]{},cadence_log[512]{},gpu_age_log[512]{},pacing_log[512]{},native_latency_log[512]{};
            {std::lock_guard lock(mx);
                if(is_generated&&submitted){++generated_count;last_generated_submit_tick=GetTickCount64();}
                if(submitted&&!is_generated&&!raw_frame){++distinct_native;
                    const auto now=TimingClock::now();
                    if(last_native_present!=TimingClock::time_point{}&&pacing_active){
                        // Gaps over 250 ms are pauses or transitions, not a rate.
                        const double interval=std::chrono::duration<double,std::milli>(now-last_native_present).count();
                        if(interval<250.)presented_native_ms=presented_native_ms>0?presented_native_ms*.9+interval*.1:interval;
                    }else if(!pacing_active)presented_native_ms=0;
                    last_native_present=now;}
                present_cpu.add(elapsed_ms(present_start));
                if(XR_SUCCEEDED(result)){++runtime_submissions;
                    const auto now_ms=std::chrono::duration<double,std::milli>(TimingClock::now().time_since_epoch()).count();
                    cadence_window.add(now_ms,{native_count,distinct_native,generated_count,runtime_submissions});}
                if(trace&&is_generated&&submitted&&(generated_count%120)==0) {
                    const auto c=cadence();
                    sprintf_s(cadence_log,"FG experiment cadence: release=%s queue=%s production=%s target_native_ms=%.3f valid=%u game=%.3f native=%.3f generated=%.3f unique=%.3f submissions=%.3f; xr_wait_ms=%.3f xr_end_ms=%.3f present_ms=%.3f; game_end_ms=%.3f last=%.3f capture_cpu_ms=%.3f render_wait_last=%.3f permits granted=%llu consumed=%llu capacity_waits=%llu",
                        active_release_point==5u?"never":active_release_point==3u?"generated-submission":active_release_point==6u?"first-generated-submission":active_release_point==7u?"second-generated-submission":active_release_point==2u?"completion":active_release_point==1u?"preparation":active_release_point==4u?"submit+4ms":"submission",
                        queue_mode_name(reported_queue_mode),
                        "demand",display_clock.display_ms()*(pacing_active?active_multiplier:1u),c.valid,c.game_fps,c.native_fps,c.generated_fps,c.unique_fps,c.submission_fps,
                        xr_wait.mean(),runtime_end.mean(),present_cpu.mean(),app_calls[1].mean(),app_calls[1].last,producer_cpu.mean(),pacing_wait.last,
                        static_cast<unsigned long long>(granted_credits),static_cast<unsigned long long>(consumed_credits),static_cast<unsigned long long>(capped_credits));
                    sprintf_s(age_log,"FG source-age CPU ms: generated_samples=%llu latest_capture_to_complete=%.3f complete_to_generated_submit=%.3f newest_input_age=%.3f complete_to_native_submit=%.3f",
                        static_cast<unsigned long long>(generated_count),capture_to_completion.mean(),completion_to_generated.mean(),
                        frame_ages[2].mean(),completion_to_native.mean());
                    sprintf_s(stage_log,"FG capture-path CPU ms: samples=%u capture_to_handoff=%.3f handoff_to_worker=%.3f worker_to_GPU_submit=%.3f GPU_submit_to_complete=%.3f sum=%.3f; credit_deferred=%llu released_after_submit_or_retire=%llu outstanding=%u",
                        generation_stages[0].count,generation_stages[0].mean(),generation_stages[1].mean(),generation_stages[2].mean(),
                        generation_stages[3].mean(),generation_stages[0].mean()+generation_stages[1].mean()+generation_stages[2].mean()+generation_stages[3].mean(),
                        static_cast<unsigned long long>(deferred_credits),static_cast<unsigned long long>(released_deferred_credits),unsigned(credit_deferred));
                    const auto time=pacing_timeline();
                    sprintf_s(pacing_log,"FG CPU pacing timeline: clock_late=%.3f deferred_credit=%.3f credit_wait=%.3f begin_gate=%.3f presenter_content=%.3f native_end=%.3f generated_end=%.3f GPU_submit_to_complete=%.3f; deferred=%llu releases=%llu backlog_holds=%llu gpu_behind_holds=%llu gpu_behind_waits=%llu carried_ticks=%llu clock_ticks=%llu",
                        time.clock_late_ms,time.credit_deferred_ms,time.game_wait_credit_ms,time.game_begin_gate_ms,
                        time.presenter_content_ms,time.native_xr_end_ms,time.generated_xr_end_ms,
                        time.fg_submit_to_complete_ms,time.deferred_events,time.deferred_releases,
                        static_cast<unsigned long long>(backlog_holds),static_cast<unsigned long long>(gpu_behind_holds),static_cast<unsigned long long>(gpu_behind_waits),
                        static_cast<unsigned long long>(carried_ticks),static_cast<unsigned long long>(clock_ticks));
                    const auto native_latency=native_submission_latency();
                    sprintf_s(native_latency_log,"FG NATIVE submit latency (GPU final stereo capture end to xrEndFrame ENTER): enabled=%u samples=%llu pending=%llu missing=%llu signed_ms=%.3f last=%.3f hold_after_ready=%.3f early_before_ready=%.3f CPU_capture_to_submit=%.3f; NO FG-off baseline",
                        native_latency.enabled,static_cast<unsigned long long>(native_latency.gpu_samples),
                        static_cast<unsigned long long>(native_latency.pending),static_cast<unsigned long long>(native_latency.missing_stamps),
                        native_latency.gpu_ready_to_submit_ms,native_latency.gpu_ready_to_submit_last_ms,
                        native_latency.native_hold_ms,native_latency.submit_ahead_ms,native_latency.capture_to_submit_ms);
                    sprintf_s(gpu_age_log,"FG GPU-ready latency ms: samples=%llu native_tail_excluded=%.3f generated_submit=%.3f predicted_display=%.3f predicted_samples=%llu late_targets=%llu gate_released=%llu gate_skipped=%llu",
                        gpu_ready_samples,gpu_ready_samples?gpu_ready_ages[0].mean():-1.,gpu_ready_samples?gpu_ready_ages[1].mean():-1.,
                        predicted_samples?gpu_ready_ages[2].mean():-1.,predicted_samples,prediction_late,
                        generated_gate_releases,generated_gate_skips);
                }
                if(have_active&&!submitted&&!stop_now&&frame.shouldRender==XR_TRUE&&!stale){++fallback_count;state=CheekyFgState::presentation_failed;
                    degraded=true;fg_active=pacing_active=false;awaiting_generated_submission=0;discard_pending();ready.clear();latest_native.reset();have_active=false;generated_phase=false;++epoch;
                    strcpy_s(diagnostic,"Private presentation failed; using coordinated original native submissions");}
                // Bounded semaphore-equivalent credits. Two prime credits let
                // the game overlap production with prior FG/compositor work.
                // GPU copies have been queued before the next game is woken.
                changed.notify_all();
                if(XR_FAILED(result)){runtime_failure("runtime xrEndFrame",result);break;}
            }
            if(age_log[0])trace(age_log);
            if(stage_log[0])trace(stage_log);
            if(cadence_log[0])trace(cadence_log);
            if(gpu_age_log[0])trace(gpu_age_log);
            if(pacing_log[0])trace(pacing_log);
            if(native_latency_log[0])trace(native_latency_log);
        }}catch(...) {runtime_failure("presentation exception",XR_ERROR_RUNTIME_FAILURE);}
    }
    #include "fg_buffered_entrypoints.inl"
};
}

namespace cheeky::fg {
std::unique_ptr<FrameBackend> make_frame_backend12(BufferedPresenter&);
std::unique_ptr<FrameBackend> make_frame_backend11(BufferedPresenter&);
}

#include "fg_backend12_buffered.inl"
#include "fg_backend11_buffered.inl"
#include "fg_buffered_backend.inl"
