#pragma once
#include "fg_transport.hpp"
#include "fg_burst_capture.hpp"
#include "fg_native_submission_math.hpp"
#include "../src/d3d12_native.hpp"
#include "../src/fg_source_lifecycle.hpp"
#include <condition_variable>
#include <thread>
#include <atomic>
#include <optional>
#include <stdexcept>

namespace cheeky::fg {
// The game owns capture/snapshot recording. Only the presentation thread calls
// the runtime frame functions. A separate worker waits for inference completion.
// Three native leases and two ready pairs bound latency and texture ownership.
struct BufferedPresenter final : Presenter {
    struct Slice {XrRect2Di rect{};unsigned layer{};};
    struct CapturePlan {D3D12_RESOURCE_DESC desc{};std::vector<Slice> slices;std::vector<unsigned> physical_eyes;bool full_required{};};
    struct DirectCapture {std::vector<std::pair<Slice,Image>> eyes;};
    static bool same_slice(const Slice& a,const Slice& b) noexcept {
        return a.layer==b.layer&&a.rect.offset.x==b.rect.offset.x&&a.rect.offset.y==b.rect.offset.y&&
            a.rect.extent.width==b.rect.extent.width&&a.rect.extent.height==b.rect.extent.height;
    }
    struct Auxiliary {XrSwapchain source{};Image image;};
    struct Native {
        std::uint64_t id{},ready{},epoch{};
        unsigned capture_mode{},upload_mode{},direct_output_mode{},direct_shading_mode{1};bool latency_diagnostics{};
        TimingClock::time_point captured_at{},handed_off_at{};
        std::array<Image,2> color;
        std::array<ColorReady,2> color_ready;
        ColorReady final_color_ready; // GPU end of the combined native-eye crop/assembly batch.
        std::array<XrCompositionLayerProjectionView,2> views;
        XrSpace space{};XrEnvironmentBlendMode blend{};
        FrameLayers layers;
        std::vector<Auxiliary> auxiliary;
        CheekyFgPair guides;
        ComPtr<IUnknown> guide_lease;
        std::array<ComPtr<ID3D12Resource>,2> guide_depth,guide_motion;
        std::array<ComPtr<ID3D12Resource>,2> peripheral_depth,peripheral_motion;
        // Only hook-free split-queue captures use this GPU-side dependency.
        // The producer queue signals it after the NGX guide command lists.
        ComPtr<ID3D12Fence> guide_sync_fence;
        std::uint64_t guide_sync_value{};
        void retain_guides(const CheekyFgPair& source) noexcept {
            guides=source;guide_lease=source.lease;
            for(unsigned eye=0;eye<2;++eye){guide_depth[eye]=source.eyes[eye].depth;guide_motion[eye]=source.eyes[eye].motion;
                peripheral_depth[eye]=source.eyes[eye].peripheral_depth;peripheral_motion[eye]=source.eyes[eye].peripheral_motion;}
            // Explicit members own the returned references. The metadata copy
            // must never be passed to the bridge's ReleasePair function.
            guides.lease=nullptr;guides.producer_queue=nullptr;
        }
        void clear_guides() noexcept {guides={};guide_depth={};guide_motion={};peripheral_depth={};peripheral_motion={};
            guide_lease.Reset();guide_sync_fence.Reset();guide_sync_value=0;}
        bool guides_valid{};
        CheekyFgState guide_state{CheekyFgState::waiting_for_guides};
        unsigned source_depth{},source_motion{};
        std::uint64_t guide_token{};
        std::uint64_t source_configuration{}; // Survives release of guide textures.
        std::uint64_t render_frame{};
    };
    struct Pair {
        std::uint64_t native_id{},current_id{},ready{};
        unsigned upload_mode{};
        TimingClock::time_point native_captured{},current_captured{},completed_at{},handed_off_at{},worker_started_at{},submitted_at{};
        std::array<Image,2> native,generated,current;
        std::array<ColorReady,2> native_color_ready,current_color_ready;
        ColorReady native_final_ready,current_final_ready;
        std::array<XrCompositionLayerProjectionView,2> native_views,generated_views,current_views;
        FrameLayers layers;
        std::vector<Auxiliary> auxiliary;
        XrSpace space{};XrEnvironmentBlendMode blend{};
        bool interpolated{},direct_xr{};
    };
    struct DirectRequest {
        std::uint64_t epoch{},native_id{};
        std::array<Image,2> previous;
        std::array<Worker::OutputCopy,2> targets;
        DXGI_FORMAT format{};unsigned width{},height{};
        bool decided{},allowed{},complete{},success{};
        unsigned deferred_ticks{};
        ComPtr<ID3D12Fence> fence;std::uint64_t serial{};
        Pair result;
    };
    std::shared_ptr<DirectRequest> direct_request; // Handshake guarded by mx; only presenter calls XR.
    CheekyFgDirectOutput direct_stats;
    CheekyFgDirectOutputDetails direct_details;
    CheekyFgDirectShading shading_stats;
    CheekyFgDirectOutputDetails logged_direct_details;
    TimingAverage direct_wait;
    std::atomic<unsigned> direct_output_mode{};
    std::atomic<unsigned> direct_shading_mode{1};
    std::atomic<std::uint64_t> direct_blocked_target{};
    static std::uint64_t direct_target_key(unsigned w,unsigned h,DXGI_FORMAT format) noexcept {
        return (std::uint64_t(w)<<32)|(std::uint64_t(h)<<8)|unsigned(format);
    }
    CheekyFgDirectOutputFn direct_output_query{};
    CheekyFgDirectShadingFn direct_shading_query{};
    struct Chain {
        XrSwapchain handle{};std::vector<XrSwapchainImageD3D12KHR> images;D3D12_RESOURCE_DESC desc{};
        std::uint64_t uploaded{}; // Content identity; GPU contexts retain sources only until their upload fence.
        unsigned last_uploaded_index{UINT32_MAX}; // Last image released to OpenXR for this chain.
        bool direct_writable{},linear_color{};
    };
    std::condition_variable_any changed;
    std::mutex capture_mutex;
    std::thread presentation_thread,generation_thread,pacing_thread;
    Transport producer,output,uploads;
    FgBurstCapture burst;
    std::uint64_t burst_native{},burst_current{},burst_presentation{};
    bool burst_generated{};
    // Default: dedicated HIGH-priority inference queue. Lua can also select a
    // separate NORMAL queue or share the application's original queue. The
    // OpenXR binding, guide capture and uploads always retain the original queue.
    ComPtr<ID3D12CommandQueue> generation_queue;
    ComPtr<ID3D12CommandQueue> fg_high_queue,fg_normal_queue;
    // Single monotonically increasing fence shared across hook-free frames.
    // It bridges a verified NGX execution queue to the OpenXR/FG queues
    // without depending on UEVR's private renderer/vtable layout.
    ComPtr<ID3D12Fence> guide_sync_fence;
    std::uint64_t guide_sync_serial{};
    bool guide_sync_reported{};
    bool accept_guide_queue(const CheekyFgPair& guides,Native& job,bool hookless) noexcept {
        if(cheeky::foveated_dlss::same_d3d12_queue(guides.producer_queue,queue.Get()))return true;
        // Legacy modes intentionally retain their established same-queue contract.
        if(!hookless||!guide_sync_fence||!device||!queue)return false;
        auto actual=cheeky::foveated_dlss::native_d3d12_interface(guides.producer_queue);
        if(!actual||actual->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT||
           queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return false;
        ComPtr<ID3D12Device> executing_device;
        if(FAILED(actual->GetDevice(IID_PPV_ARGS(&executing_device)))||
           !cheeky::foveated_dlss::same_d3d12_device(device.Get(),executing_device.Get()))return false;
        // The guide command lists were already observed as submitted on
        // `actual`. Its signal is ordered after those writes. The worker
        // enqueues Queue::Wait on its OWN queue before accessing their data.
        const auto value=guide_sync_serial+1;
        if(FAILED(actual->Signal(guide_sync_fence.Get(),value)))return false;
        guide_sync_serial=value;
        job.guide_sync_fence=guide_sync_fence;
        job.guide_sync_value=value;
        if(!guide_sync_reported&&trace){
            trace("DLSS-FG hook-free guide queue differs from OpenXR queue: validated same-device cross-queue GPU fence enabled");
            guide_sync_reported=true;
        }
        return true;
    }
    unsigned last_fg_priority_requested{UINT32_MAX};
    unsigned reported_queue_mode{1}; // Published under mx for presentation diagnostics.
    bool generation_high_priority{};
    static const char* queue_mode_name(unsigned mode) noexcept {
        return mode==2u?"shared-game":mode==0u?"separate-NORMAL":"separate-HIGH";
    }
    std::unordered_map<XrSwapchain,Image> captured;
    std::unordered_map<XrSwapchain,DirectCapture> direct_captures;
    std::unordered_map<XrSwapchain,CapturePlan> capture_plans;
    std::unordered_map<XrSwapchain,D3D12_RESOURCE_DESC> captured_descs;
    std::atomic<unsigned> color_capture_mode{},upload_mode{},latency_diagnostics{1};
    CheekyFgTransportOptionsFn transport_options_query{};
    CheekyFgStorageOptions storage_options; // Protected by capture_mutex; latched at app WaitFrame.
    CheekyFgStaticEyeCropsV1 static_eye_crops; // Diagnostic only; NEVER used to pack a later native frame.
    // Protected by capture_mutex, like the captured images and their stamps.
    std::unordered_map<XrSwapchain,std::uint64_t> capture_crop_tokens;
    std::uint64_t compact_current_lookups{},compact_current_misses{};
    std::uint64_t pose_sample_count{};
    CompactLayout eye_color_layout(unsigned eye,unsigned width,unsigned height,
                                   const CheekyFgCaptureCropsV2& current) const noexcept {
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
    std::array<std::shared_ptr<Native>,3> slots;
    void release_idle_slots() noexcept {
        // Hold mx. Only the slot itself owns these completed jobs; pending and
        // worker/history users retain the Native, and ready/upload users retain
        // their Images independently. Do not let an unused lease pin old pixels.
        for(auto& slot:slots)if(slot&&slot.use_count()==1&&producer.completed(slot->ready)){
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
    CheekyFgOverhead extra_gpu;
    // Unique native, generated midpoint, newest FG input, repeated native.
    std::array<TimingAverage,4> frame_ages;
    std::array<std::uint64_t,4> age_samples{};
    TimingAverage late_pair_wait;
    std::uint64_t late_wait_samples{},late_wait_arrivals{},late_wait_timeouts{};
    std::uint64_t late_ready_skips{},late_idle_skips{};
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
    bool inflight_prepared{},release_policy_known{},submit_repeat_frames{true},clock_pacing_active{};
    double late_wait_budget_ms{};
    TimingAverage producer_cpu,pacing_wait;
    TimingAverage clock_late,credit_deferral,begin_gate_wait,presenter_content_wait;
    TimingAverage native_xr_end,generated_xr_end;
    TimingClock::time_point credit_deferred_since{};
    TimingClock::time_point release_delay_until{}; // Mode 4: 4 ms after FG submission; not a GPU fence wait.
    std::uint64_t last_generated_submit_tick{};
    TimingAverage app_image_wait,present_image_wait,runtime_begin,runtime_end,present_cpu;
    std::array<TimingAverage,3> app_calls;
    CadenceWindow cadence_window;
    std::uint64_t distinct_native{},repeated_presentations{},runtime_submissions{};
    // Debug-only application->OpenXR submission trace. Monotonicity here only
    // proves what Cheeky handed to xrEndFrame, NOT what the headset displayed.
    XrTime presentation_trace_last_time{};
    double presentation_trace_last_position{-1.};
    std::uint64_t presentation_trace_events{},presentation_trace_anomalies{};
    double capture_cpu_accumulator{};
    struct ProducerElapsed {
        BufferedPresenter& owner;TimingClock::time_point begin{TimingClock::now()};
        ~ProducerElapsed(){std::lock_guard capture_lock(owner.capture_mutex);std::lock_guard lock(owner.mx);
            owner.producer_cpu.add(elapsed_ms(begin)+owner.capture_cpu_accumulator);owner.capture_cpu_accumulator=0;
            owner.release_idle_slots();
            const auto now=GetTickCount64();if(now>=owner.next_capture_memory){
                fg_memory::Collector collect(owner.device.Get());
                owner.memory_state.categories[fg_memory::capture]=owner.producer.memory(collect);
                owner.memory_state.capture_tick=now;owner.next_capture_memory=now+1000;
            }}
    };
    char diagnostic[384]{"Buffered presenter waiting for stereo frames"};
    bool started{},stopping{},bypass{},fg_active{},pacing_active{},degraded{};
    bool native_frame_pending{}; // Hold mx; the pre-activation frame still owns runtime Begin/End.
    unsigned credits{},app_waiters{};
    std::uint64_t next_id{},waited{},begun{},ended{},ticks{},skipped{},epoch{},transition_until{};
    std::uint64_t last_credit_tick{},completed_pair_id{},inflight_id{};
    XrFrameState predicted{XR_TYPE_FRAME_STATE};
    XrTime app_display_time{};
    XrResult terminal{XR_SUCCESS},raw_result{XR_SUCCESS};
    const XrFrameEndInfo* raw{};bool raw_done{};
    CheekyFgAcquireFn guide_acquire{};CheekyFgReleaseFn guide_release{};
    CheekyFgCaptureTokenFn guide_token{};
    CheekyFgCaptureStampFn guide_stamp{};

    ~BufferedPresenter() override {reset();}
    void set_native_frame_pending(bool pending_frame) noexcept override {
        std::lock_guard lock(mx);native_frame_pending=pending_frame;changed.notify_all();
    }
    void runtime_failure(const char* stage,XrResult result) noexcept {
        std::lock_guard lock(mx);
        terminal=result;stopping=true;state=CheekyFgState::presentation_failed;
        sprintf_s(diagnostic,"DLSS-FG %s failed: XrResult=%d; presenter stopped",stage,int(result));
        if(trace)trace(diagnostic);
        changed.notify_all();
    }
    bool setup(XrSession s,const XrFns& f,ID3D12Device* dev,ID3D12CommandQueue* q) noexcept override {
        if(!Presenter::setup(s,f,dev,q))return false;

        // Create the FG queue on the very same D3D12 device/node as the game.
        // The OpenXR runtime continues to own/use the queue supplied in the
        // graphics binding. Only NGX evaluation and its final publication copy
        // are submitted to this dedicated queue. A GPU-side wait in
        // generation_loop() orders it after the native snapshot/guide work.
        //
        // If HIGH priority is unavailable, retain the proven same-queue path;
        // never make the whole VR presenter fail because of a priority hint.
        const auto game_desc=q->GetDesc();
        D3D12_COMMAND_QUEUE_DESC fg_desc{};
        fg_desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        fg_desc.Priority=D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        fg_desc.Flags=D3D12_COMMAND_QUEUE_FLAG_NONE;
        fg_desc.NodeMask=game_desc.NodeMask;
        const HRESULT high_result=dev->CreateCommandQueue(&fg_desc,IID_PPV_ARGS(&fg_high_queue));
        fg_desc.Priority=D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        const HRESULT normal_result=dev->CreateCommandQueue(&fg_desc,IID_PPV_ARGS(&fg_normal_queue));
        // The default remains HIGH; the shared game queue is an explicit A/B
        // choice alongside separate NORMAL. Missing priorities fall back here.
        generation_queue=fg_high_queue.Get() ? fg_high_queue.Get() : fg_normal_queue.Get() ? fg_normal_queue.Get() : q;
        generation_high_priority=fg_high_queue.Get()!=nullptr&&generation_queue.Get()==fg_high_queue.Get();
        reported_queue_mode=generation_queue.Get()==q?2u:generation_high_priority?1u:0u;
        last_fg_priority_requested=UINT32_MAX;
        guide_sync_fence.Reset();guide_sync_serial=0;guide_sync_reported=false;
        // Failure merely disables split-queue FG; the native OpenXR path
        // remains usable, and matching queues need no additional fence.
        dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&guide_sync_fence));
        if(trace) {
            char message[240]{};
            sprintf_s(message,"DLSS-FG queue A/B: HIGH=%s(0x%08lx), separate NORMAL=%s(0x%08lx); active=%s; capture remains game queue",
                fg_high_queue?"available":"FAILED",static_cast<unsigned long>(high_result),
                fg_normal_queue?"available":"FAILED",static_cast<unsigned long>(normal_result),
                generation_high_priority?"HIGH":fg_normal_queue?"NORMAL":"shared fallback");
            trace(message);
        }
        return producer.initialize(dev,q)&&output.initialize(dev,q)&&uploads.initialize(dev,q);
    }
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
        CheekyFgCadence result;result.supported=1;std::array<double,5> rates{};
        result.valid=cadence_window.rates(rates);result.game_fps=rates[0];result.native_fps=rates[1];result.generated_fps=rates[2];
        result.unique_fps=rates[1]+rates[2];result.repeat_fps=rates[3];result.submission_fps=rates[4];
        result.refresh_hz=telemetry.xr_period_ms>0?1000./telemetry.xr_period_ms:0;
        result.application_period_ms=telemetry.xr_period_ms*(pacing_active?2:1);
        result.native_target_fps=result.application_period_ms>0?1000./result.application_period_ms:0;
        result.app_image_wait_ms=app_image_wait.mean();result.present_image_wait_ms=present_image_wait.mean();
        result.begin_ms=runtime_begin.mean();result.end_ms=runtime_end.mean();result.present_cpu_ms=present_cpu.mean();
        result.app_begin_ms=app_calls[0].mean();result.app_end_ms=app_calls[1].mean();result.app_release_ms=app_calls[2].mean();return result;
    }
    CheekyFgOverhead overhead() const noexcept override {
        auto result=extra_gpu;result.supported=1;
        const auto capture=producer.gpu_cost.summary(),upload=uploads.gpu_cost.summary();
        result.capture_samples=capture.samples;result.capture_ms=capture.mean;result.capture_last_ms=capture.last;
        result.upload_samples=upload.samples;result.upload_ms=upload.mean;result.upload_last_ms=upload.last;
        return result;
    }
    CheekyFgExperimentDiagnostics experiments() const noexcept override {
        auto out=experiment_state;out.supported=1;
        if(out.color_capture_mode!=color_capture_mode||out.upload_mode!=upload_mode||out.latency_enabled!=latency_diagnostics)out.latency_valid=0;
        out.color_capture_mode=color_capture_mode.load();out.upload_mode=upload_mode.load();
        out.latency_enabled=latency_diagnostics.load();out.full_color_copies=producer.full_copies.load();
        out.direct_eye_copies=direct_eye_copies.load();out.eye_crop_copies=eye_crop_copies.load();out.capture_fallbacks=capture_fallbacks.load();
        out.upload_copies=uploads.upload_copies.load();out.upload_batches=uploads.submissions.load();
        out.display_time_supported=xr.time_to_qpc!=nullptr;out.gpu_ready_samples=gpu_ready_samples;
        out.predicted_samples=predicted_samples;out.prediction_late=prediction_late;
        out.generated_gate_releases=generated_gate_releases;out.generated_gate_skips=generated_gate_skips;
        out.native_gpu_tail={gpu_ready_ages[0].mean(),gpu_ready_ages[0].last};
        out.gpu_ready_to_submit={gpu_ready_ages[1].mean(),gpu_ready_ages[1].last};
        out.gpu_ready_to_display={gpu_ready_ages[2].mean(),gpu_ready_ages[2].last};return out;
    }
    CheekyFgMemory memory() const noexcept override {
        auto out=memory_state;out.supported=out.presenter_present=1;return out;
    }
    CheekyFgDirectOutput direct_output() const noexcept override {
        auto out=direct_stats;out.supported=1;out.requested=direct_output_mode;return out;
    }
    CheekyFgDirectOutputDetails direct_output_details() const noexcept override {auto out=direct_details;out.supported=1;return out;}
    CheekyFgDirectShading direct_shading() const noexcept override {auto out=shading_stats;out.supported=1;out.requested_mode=direct_shading_mode;return out;}
    void measure_worker_memory() {
        // Single generation worker, after the prior GPU fence retired. Publish
        // only integer snapshots; diagnostic readers never walk live resources.
        const auto now=GetTickCount64();if(now<next_worker_memory)return;
        output.trim_unused(now);
        CheekyFgMemory value;fg_memory::Collector collect(device.Get());
        value.categories[fg_memory::generated]=output.memory(collect);
        for(const auto& eye:worker.focus.inputs)for(const auto& input:eye){
            collect.add(value.categories[fg_memory::prepared_color],input.color.Get());
            collect.add(value.categories[fg_memory::prepared_color],input.recentered_color.Get());
            collect.add(value.categories[fg_memory::prepared_motion],input.corrected_motion.Get());
            // Prepared captures are owned/countable by the producer snapshot
            // pool. These references are borrowed, not a second allocation.
            if(!input.borrowed_guides){collect.add(value.categories[fg_memory::prepared_depth],input.depth.Get());
                collect.add(value.categories[fg_memory::prepared_motion],input.motion.Get());}
        }
        for(const auto* features:{&worker.features,&worker.peripheral_features})for(const auto& f:*features){
            collect.add(value.categories[fg_memory::ngx_outputs],f.output.Get());
            collect.add(value.categories[fg_memory::ngx_outputs],f.disable.Get());
        }
        for(unsigned eye=0;eye<2;++eye){
            collect.add(value.categories[fg_memory::periphery],worker.focus.peripheral_output[eye].Get());
            collect.add(value.categories[fg_memory::periphery],worker.focus.previous_depth[eye].Get());
            collect.add(value.categories[fg_memory::composition],worker.focus.full[eye].Get());
        }
        std::lock_guard lock(mx);
        for(auto category:{fg_memory::generated,fg_memory::prepared_color,fg_memory::prepared_depth,
                fg_memory::prepared_motion,fg_memory::ngx_outputs,fg_memory::periphery,fg_memory::composition})
            memory_state.categories[category]=value.categories[category];
        memory_state.worker_tick=now;next_worker_memory=now+1000;
    }
    void measure_presenter_memory() {
        // Only the presentation thread creates/destroys/enumerates these chains.
        const auto now=GetTickCount64();if(now<next_presenter_memory)return;
        CheekyFgMemory value;fg_memory::Collector collect(device.Get());
        value.categories[fg_memory::upload_pool]=uploads.memory(collect);
        for(const auto& chain:scene_chains)for(const auto& image:chain.images)
            collect.add(value.categories[fg_memory::swapchain_native],image.texture);
        for(const auto& [sc,chain]:auxiliary_chains)for(const auto& image:chain.images)
            collect.add(value.categories[fg_memory::swapchain_auxiliary],image.texture);
        std::lock_guard lock(mx);
        for(auto category:{fg_memory::upload_pool,fg_memory::swapchain_native,fg_memory::swapchain_generated,fg_memory::swapchain_auxiliary})
            memory_state.categories[category]=value.categories[category];
        memory_state.presenter_tick=now;next_presenter_memory=now+1000;
    }
    CheekyFgPresentation presentation() const noexcept override {
        CheekyFgPresentation out;out.supported=1;
        out.native_samples=age_samples[0];out.generated_samples=age_samples[1];out.repeat_samples=age_samples[3];
        out.native_age_ms=frame_ages[0].mean();out.native_age_last_ms=frame_ages[0].last;
        out.generated_age_ms=frame_ages[1].mean();out.generated_age_last_ms=frame_ages[1].last;
        out.newest_input_age_ms=frame_ages[2].mean();out.newest_input_age_last_ms=frame_ages[2].last;
        out.repeat_age_ms=frame_ages[3].mean();out.repeat_age_last_ms=frame_ages[3].last;
        out.wait_samples=late_wait_samples;out.wait_arrivals=late_wait_arrivals;out.wait_timeouts=late_wait_timeouts;
        out.late_wait_ms=late_pair_wait.mean();out.late_wait_last_ms=late_pair_wait.last;out.wait_budget_ms=late_wait_budget_ms;
        return out;
    }
    void note_frame_age(const Pair& pair,bool generated,bool repeated,TimingClock::time_point submitted_at) noexcept {
        // Hold mx. Timestamps travel with immutable images, not with their pool
        // slots or the latest game frame. Repeats keep their original source.
        auto add=[&](unsigned index,TimingClock::time_point source) {
            if(source==TimingClock::time_point{}||source>submitted_at)return;
            frame_ages[index].add(std::chrono::duration<double,std::milli>(submitted_at-source).count());++age_samples[index];
        };
        if(generated) {
            if(pair.native_captured!=TimingClock::time_point{}&&pair.current_captured>=pair.native_captured)
                add(1,pair.native_captured+(pair.current_captured-pair.native_captured)/2);
            add(2,pair.current_captured);
        }else add(repeated?3u:0u,pair.native_captured);
        if(pair.interpolated&&pair.completed_at!=TimingClock::time_point{}&&pair.completed_at<=submitted_at&&!repeated) {
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
    void collect_color_stamps() {
        std::lock_guard capture_lock(capture_mutex);producer.collect(producer.fence->GetCompletedValue());
    }
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
        awaiting_generated_submission=0;if(submitted)++generated_gate_releases;else ++generated_gate_skips;
        release_deferred_credit();changed.notify_all();
    }
    void note_app_call(unsigned stage,double ms) noexcept override {std::lock_guard lock(mx);if(stage<app_calls.size())app_calls[stage].add(ms);}
    XrResult runtime_end_frame(const XrFrameEndInfo* end) {
        const auto start=TimingClock::now();const auto result=xr.end_frame(session,end);
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
    void discard_pending() noexcept {for(auto& job:pending)job->clear_guides();pending.clear();}
    void stop() noexcept {
        {std::lock_guard lock(mx);stopping=true;changed.notify_all();}
        if(presentation_thread.joinable())presentation_thread.join();
        if(generation_thread.joinable())generation_thread.join();
        if(pacing_thread.joinable())pacing_thread.join();
        std::lock_guard capture_lock(capture_mutex);
        producer.drain();output.drain();uploads.drain();worker.wait_idle();
        std::lock_guard lock(mx);discard_pending();ready.clear();latest_native.reset();last_fg_borders={};slots={};
        captured.clear();direct_captures.clear();capture_plans.clear();captured_descs.clear();capture_tokens.clear();capture_frames.clear();capture_times.clear();capture_ready.clear();capture_overflow=false;capture_cpu_accumulator=0;started=false;credits=0;fg_active=pacing_active=false;
        if(raw)raw_result=XR_FAILED(terminal)?terminal:XR_ERROR_SESSION_NOT_RUNNING;
        raw=nullptr;raw_done=true;passthrough_events=0;
        waited=begun=ended=0;ticks=transition_until=0;degraded=false;app_display_time=0;predicted={XR_TYPE_FRAME_STATE};++epoch;
        app_image_wait={};present_image_wait={};runtime_begin={};runtime_end={};present_cpu={};app_calls={};cadence_window={};
        clock_late={};credit_deferral={};begin_gate_wait={};presenter_content_wait={};
        native_xr_end={};generated_xr_end={};credit_deferred_since={};release_delay_until={};last_generated_submit_tick=0;
        producer.gpu_cost.reset();uploads.gpu_cost.reset();extra_gpu={};
        frame_ages={};age_samples={};late_pair_wait={};late_wait_samples=late_wait_arrivals=late_wait_timeouts=0;
        late_wait_budget_ms=0;last_credit_tick=completed_pair_id=inflight_id=0;
        late_ready_skips=late_idle_skips=0;capture_to_completion={};completion_to_native={};completion_to_generated={};
        generation_stages={};deferred_credits=released_deferred_credits=submitted_native_id=0;credit_deferred=false;
        gpu_ready_ages={};gpu_ready_samples=predicted_samples=prediction_late=0;
        pending_native_submissions.clear();native_ready_to_submit={};native_hold={};submit_ahead={};native_capture_to_submit={};
        native_latency_samples=native_latency_missing=native_latency_held=native_latency_early=0;
        experiment_state={};direct_request.reset();direct_stats={};direct_wait={};
        direct_details={};logged_direct_details={};
        direct_blocked_target=0;
        guide_sync_fence.Reset();guide_sync_serial=0;guide_sync_reported=false;
        awaiting_generated_submission=generated_gate_releases=generated_gate_skips=0;
        memory_state={};next_capture_memory=next_worker_memory=next_presenter_memory=0;
        inflight_release_point=last_release_point=active_release_point=0;inflight_prepared=release_policy_known=clock_pacing_active=false;submit_repeat_frames=true;
        distinct_native=repeated_presentations=runtime_submissions=0;
        presentation_trace_last_time=0;presentation_trace_last_position=-1.;
        presentation_trace_events=presentation_trace_anomalies=0;
        changed.notify_all();
    }
    void destroy_chains() noexcept {
        for(auto& c:scene_chains){if(c.handle)destroy_chain(c.handle);c={};}
        for(auto& [key,c]:auxiliary_chains)if(c.handle)destroy_chain(c.handle);
        auxiliary_chains.clear();
    }
    void reset() noexcept override {
        // Stop/join both workers and drain their GPU fences before dropping the
        // dedicated queue. A temporal reset only stops the workers; it keeps
        // the queue available for the next interpolation session.
        stop();destroy_chains();Presenter::reset();
        generation_queue.Reset();fg_high_queue.Reset();fg_normal_queue.Reset();generation_high_priority=false;
        last_fg_priority_requested=UINT32_MAX;
    }
    void invalidate_temporal() noexcept override {
        stop();worker.reset();worker.failed=false;destroy_chains();
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
            started=true;clock_pacing_active=true;
            pacing_thread=std::thread([this]{pacing_loop();});
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
        const auto shading_query=direct_shading_query?direct_shading_query:runtime?
            reinterpret_cast<CheekyFgDirectShadingFn>(GetProcAddress(runtime,"CheekyFG_GetDirectShadingOptionsV1")):nullptr;
        CheekyFgDirectShading shading_options;
        if(shading_query&&shading_query(&shading_options))direct_shading_mode=std::min(shading_options.requested_mode,2u);
        if(output_query&&output_query(&direct_options)){
            const unsigned mode=!interop11 && direct_options.requested==1u;
            if(direct_output_mode.exchange(mode)!=mode)direct_blocked_target=0;
        }
        if(query&&query(&options)){
            const auto old_color=color_capture_mode.exchange(options.color_capture_mode==1u?1u:0u);
            const auto old_upload=upload_mode.exchange(options.upload_mode==1u?1u:0u);
            const auto old_diagnostics=latency_diagnostics.exchange(options.latency_diagnostics==1u?1u:0u);
            if(old_diagnostics!=latency_diagnostics.load()){
                std::lock_guard lock(mx);
                pending_native_submissions.clear();native_ready_to_submit={};native_hold={};submit_ahead={};native_capture_to_submit={};
                native_latency_samples=native_latency_missing=native_latency_held=native_latency_early=0;
            }
            if(trace&&(old_color!=color_capture_mode||old_upload!=upload_mode||old_diagnostics!=latency_diagnostics)){
                char message[192]{};sprintf_s(message,"FG transport A/B: color=%s uploads=%s latency=%s (next native frame)",
                    color_capture_mode?"direct-eyes":"full-snapshot",upload_mode?"batched":"per-image",latency_diagnostics?"1-in-17":"OFF");trace(message);
            }
        }
        std::unique_lock lock(mx);
        if(!start())return XR_ERROR_RUNTIME_FAILURE;
        const auto wait_begin=TimingClock::now();
        ++app_waiters;
        changed.wait(lock,[&]{return stopping||XR_FAILED(terminal)||(credits&&waited==begun&&!raw&&!release_gate_active());});
        --app_waiters;
        pacing_wait.add(elapsed_ms(wait_begin));
        if(stopping||XR_FAILED(terminal))return XR_FAILED(terminal)?terminal:XR_ERROR_SESSION_NOT_RUNNING;
        --credits;++waited;
        const auto next=frame->next;*frame=predicted;frame->next=next;
        // Predict a native frame two display intervals ahead; keep one display
        // time consistent across all engine stages and strictly increasing.
        const auto period=std::max<XrDuration>(1,predicted.predictedDisplayPeriod);
        frame->predictedDisplayTime=std::max(predicted.predictedDisplayTime+2*period,app_display_time+period*(pacing_active?2:1));
        if(pacing_active&&period<=INT64_MAX/2)frame->predictedDisplayPeriod=period*2;
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
            captured.clear();direct_captures.clear();capture_tokens.clear();capture_frames.clear();capture_crop_tokens.clear();
            capture_times.clear();capture_ready.clear();capture_overflow=false;capture_cpu_accumulator=0;
            if(trace)trace("FG xrBeginFrame discarded the preceding unended application frame");
            changed.notify_all();
        }
        capture_lock.unlock();
        // A pipelined xrWaitFrame may already have consumed a credit before
        // the previous end_frame enqueues its FG job. Gate begin_frame too.
        const auto gate_started=TimingClock::now();
        changed.wait(lock,[&]{return stopping||!release_gate_active();});
        begin_gate_wait.add(elapsed_ms(gate_started));
        if(stopping)return XR_FAILED(terminal)?terminal:XR_ERROR_SESSION_NOT_RUNNING;
        ++begun;changed.notify_all();return discarded?XR_FRAME_DISCARDED:XR_SUCCESS;
    }
    bool capture(XrSwapchain sc,unsigned,ID3D12Resource* original,unsigned,unsigned,unsigned,
                 DXGI_FORMAT declared=DXGI_FORMAT_UNKNOWN) noexcept override {
        if(bypass||!original)return false;
        std::lock_guard capture_lock(capture_mutex);
        {std::lock_guard lock(mx);if(stopping||degraded)return false;}
        const auto captured_at=TimingClock::now();
        const auto captured_cpu=latency_diagnostics?LatencyProbe::cpu_now():0;
        AddElapsed elapsed{capture_cpu_accumulator};
        auto runtime=GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll");
        const auto stamp_fn=guide_stamp?guide_stamp:runtime?reinterpret_cast<CheekyFgCaptureStampFn>(GetProcAddress(runtime,"CheekyFG_GetCaptureStamp")):nullptr;
        CheekyFgCaptureStamp stamp;if(stamp_fn)stamp_fn(original,&stamp);
        // The legacy test callback has no production fallback: real generation
        // requires the RHI frame identity returned with the color stamp.
        if(guide_token&&!stamp_fn){stamp.token=guide_token(original);stamp.render_frame=stamp.token/2;}
        const auto token=stamp.token;
        // Look up the *current* unconsumed, submitted guide pair at color
        // release, rather than using the preceding WaitFrame's crop cache.
        // No GPU work and no lease/consumption is performed by this query.
        CheekyFgCaptureCropsV2 current_crops;
        current_crops.token=token;
        current_crops.render_frame=stamp.render_frame;
        const auto peek=runtime?reinterpret_cast<CheekyFgCaptureCropsFn>(
            GetProcAddress(runtime,"CheekyFG_PeekCaptureCropsV2")):nullptr;
        if(storage_options.compact){
            if(!peek||!peek(&current_crops))current_crops.valid_mask=0;
            if(current_crops.valid_mask)++compact_current_lookups;
            else ++compact_current_misses;
            const auto total=compact_current_lookups+compact_current_misses;
            if(trace&&(total<=2||total%240==0)){
                char message[224]{};
                sprintf_s(message,"FG same-interval crop lookup ok=%llu fallback=%llu token=%llu eye_mask=0x%x (no prior-frame crop reuse)",
                    static_cast<unsigned long long>(compact_current_lookups),
                    static_cast<unsigned long long>(compact_current_misses),
                    static_cast<unsigned long long>(current_crops.token),current_crops.valid_mask);
                trace(message);
            }
        }
        const auto format=capture_format(original->GetDesc().Format,declared);if(format==DXGI_FORMAT_UNKNOWN)return false;
        std::uint64_t profile_frame{};{std::lock_guard lock(mx);profile_frame=next_id+1;}
        auto* c=producer.begin(profile_frame);if(!c){capture_overflow=true;return false;}
        const auto ready_stamp=captured_cpu?producer.mark_color_ready(*c,captured_cpu):ColorReady{};
        const auto desc=original->GetDesc();captured_descs[sc]=desc;
        const auto plan=capture_plans.find(sc);
        const bool direct=color_capture_mode==1u&&plan!=capture_plans.end()&&!plan->second.full_required&&
            !plan->second.slices.empty()&&plan->second.desc.Width==desc.Width&&plan->second.desc.Height==desc.Height&&
            plan->second.desc.DepthOrArraySize==desc.DepthOrArraySize&&plan->second.desc.Format==desc.Format;
        DirectCapture eyes;Image copy;
        if(direct){
            for(std::size_t n=0;n<plan->second.slices.size();++n){
                const auto slice=plan->second.slices[n];
                const unsigned eye=n<plan->second.physical_eyes.size()?plan->second.physical_eyes[n]:2u;
                const auto layout=eye_color_layout(eye,slice.rect.extent.width,slice.rect.extent.height,current_crops);
                auto image=layout.packed()?producer.packed_region(*c,original,xr_image_state(),slice.rect,slice.layer,format,layout):
                    producer.region_copy(*c,original,xr_image_state(),slice.rect,slice.layer,format);
                if(!image){capture_overflow=true;std::uint64_t discarded{};producer.execute(*c,discarded);return false;}
                eyes.eyes.push_back({slice,std::move(image)});++direct_eye_copies;
            }
        }else {
            copy=producer.copy(*c,original,xr_image_state(),format);
            if(color_capture_mode==1u&&(plan==capture_plans.end()||!plan->second.full_required))++capture_fallbacks;
            if(!copy){capture_overflow=true;c->list->Close();return false;}
        }
        std::uint64_t ticket{};if(!producer.execute(*c,ticket))return false;
        if(direct){captured.erase(sc);direct_captures[sc]=std::move(eyes);}
        else {direct_captures.erase(sc);captured[sc]=std::move(copy);}
        // A compact image only belongs to the exact guide interval returned
        // by PeekCaptureCrops. Full-eye fallback has no crop-token dependency.
        capture_crop_tokens[sc]=direct&&current_crops.valid_mask?current_crops.token:0;
        capture_tokens[sc]=token;capture_frames[sc]=stamp.render_frame;capture_times[sc]=captured_at;capture_ready[sc]=ready_stamp;return true;
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
        if(bypass)return xr.end_frame(session,end);
        if(!end)return XR_ERROR_VALIDATION_FAILURE;
        ProducerElapsed elapsed{*this};
        std::uint64_t id{};bool use_raw{};
        {std::lock_guard lock(mx);if(stopping)return XR_FAILED(terminal)?terminal:XR_ERROR_SESSION_NOT_RUNNING;
            if(!started||begun==ended)return XR_ERROR_CALL_ORDER_INVALID;++native_count;id=++next_id;use_raw=degraded;}
        const auto fg_runtime=GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll");
        const auto hookless_complete=fg_runtime?reinterpret_cast<CheekyFgCompleteHooklessFrameFn>(
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
            for(auto& slot:slots) {
                if(!slot)slot=std::make_shared<Native>();
                if(slot.use_count()==1&&producer.completed(slot->ready)){job=slot;break;}
            }
        }
        const bool inference_slot=bool(job);
        // CaptureStamp is per-image. Some UEVR versions release a left/right
        // image separately or execute the final copy through opaque wrappers.
        // In hook-free mode only the completed xrEndFrame can claim a pair.
        if(!inference_slot&&hookless_complete)hookless_complete(0);
        if(!job){job=std::make_shared<Native>();std::lock_guard lock(mx);++skipped;}
        job->color={};job->color_ready={};job->final_color_ready.reset();job->auxiliary.clear();job->clear_guides();job->guides_valid=false;job->guide_token=0;job->source_configuration=0;job->render_frame=0;job->captured_at={};
        job->guide_state=CheekyFgState::waiting_for_guides;job->source_depth=job->source_motion=0;
        if(!projection||!job->layers.prepare(end,projection)||!job->layers.retain_spaces(session,xr.retain_space)){
            if(inference_slot&&hookless_complete)hookless_complete(0);
            result=passthrough(end,"unsupported OpenXR projection/layers");
        }
        else {
            std::lock_guard capture_lock(capture_mutex);
            std::unordered_map<XrSwapchain,CapturePlan> next_plans;
            for(unsigned eye=0;eye<2;++eye){const auto& sub=projection->views[eye].subImage;
                auto& plan=next_plans[sub.swapchain];plan.desc=captured_descs[sub.swapchain];
                Slice slice{sub.imageRect,sub.imageArrayIndex};
                auto existing=std::find_if(plan.slices.begin(),plan.slices.end(),
                    [&](const auto& x){return same_slice(x,slice);});
                if(existing==plan.slices.end()){
                    plan.slices.push_back(slice);plan.physical_eyes.push_back(eye);
                }else if(plan.physical_eyes[std::size_t(existing-plan.slices.begin())]!=eye){
                    // Same image subrect is used for both eyes; cannot give it
                    // two different packed layouts. Copy the full texture.
                    plan.full_required=true;
                }
            }
            subimages(job->layers,[&](XrSwapchainSubImage& sub){next_plans[sub.swapchain].full_required=true;return true;});
            capture_plans=std::move(next_plans);
            job->capture_mode=color_capture_mode.load();job->upload_mode=upload_mode.load();job->latency_diagnostics=latency_diagnostics!=0;
            job->direct_output_mode=direct_output_mode.load();
            job->direct_shading_mode=direct_shading_mode.load();
            auto* c=producer.begin(id,true);
            if(!c){
                if(inference_slot&&hookless_complete)hookless_complete(0);
                std::lock_guard lock(mx);++skipped;++ended;changed.notify_all();return XR_SUCCESS;
            }
            bool good=true;std::uint64_t expected_token{},expected_frame{};bool tokens_match=true;
            if(good)for(unsigned eye=0;eye<2;++eye) {
                const auto& sub=projection->views[eye].subImage;
                const auto direct=direct_captures.find(sub.swapchain);
                if(direct!=direct_captures.end()){
                    const Slice requested{sub.imageRect,sub.imageArrayIndex};
                    for(const auto& [slice,image]:direct->second.eyes)if(same_slice(slice,requested)){job->color[eye]=image;break;}
                    if(!job->color[eye])++capture_fallbacks; // Changed layout: coordinated native passthrough below.
                }else {
                    const auto found=captured.find(sub.swapchain);
                    if(found!=captured.end()){
                        // Full-swapchain capture is an ordinary full-eye
                        // fallback, never repack it using stale cached crop.
                        // The FG worker samples its full resolution directly.
                        job->color[eye]=producer.crop(*c,found->second,sub.imageRect,sub.imageArrayIndex);
                    }
                    if(job->color[eye])++eye_crop_copies;
                }
                if(!job->color[eye]){good=false;break;}
                const auto token=capture_tokens[sub.swapchain];
                const auto frame=capture_frames[sub.swapchain];
                const auto captured_at=capture_times[sub.swapchain];
                job->color_ready[eye]=capture_ready[sub.swapchain];
                if(!eye||captured_at<job->captured_at)job->captured_at=captured_at;
                if(!eye){expected_token=token;expected_frame=frame;}else tokens_match&=token==expected_token&&frame==expected_frame;
                producer.freeze(*c,job->color[eye]);job->views[eye]=projection->views[eye];
            }
            if(good)good=subimages(job->layers,[&](XrSwapchainSubImage& sub){
                const auto found=captured.find(sub.swapchain);if(found==captured.end())return false;
                const auto d=found->second->texture->GetDesc();
                if(sub.imageArrayIndex>=d.DepthOrArraySize||sub.imageRect.offset.x<0||sub.imageRect.offset.y<0||
                    sub.imageRect.extent.width<=0||sub.imageRect.extent.height<=0||
                    std::uint64_t(sub.imageRect.offset.x)+sub.imageRect.extent.width>d.Width||
                    std::uint64_t(sub.imageRect.offset.y)+sub.imageRect.extent.height>d.Height)return false;
                if(std::none_of(job->auxiliary.begin(),job->auxiliary.end(),[&](const Auxiliary& a){return a.source==sub.swapchain;}))
                    job->auxiliary.push_back({sub.swapchain,found->second});return true;
            });
            if(good) {
                if(inference_slot&&hookless_complete){
                    // Both final stereo colors are now captured. Stereo guides are claimed at the final OpenXR handoff, independent
                    // of the native-frame release policy.
                    const auto completed=hookless_complete(1);
                    if(completed){expected_token=completed;expected_frame=0;tokens_match=true;}
                    else if(!expected_frame){expected_token=0;tokens_match=false;}
                }
                // When a guide interval changed between xrReleaseSwapchainImage
                // and xrEndFrame, never evaluate FG with the now-wrong packed
                // crop. Full-eye copies are unaffected and remain eligible.
                if(tokens_match&&expected_token)for(unsigned eye=0;eye<2;++eye){
                    if(!job->color[eye]||!job->color[eye]->layout.packed())continue;
                    const auto sc=projection->views[eye].subImage.swapchain;
                    if(capture_crop_tokens[sc]!=expected_token){
                        tokens_match=false;
                        if(trace){
                            char message[224]{};
                            sprintf_s(message,"FG compact capture interval changed eye=%u packed_guide=%llu matched_guide=%llu; skipping FG pair",
                                eye,static_cast<unsigned long long>(capture_crop_tokens[sc]),
                                static_cast<unsigned long long>(expected_token));
                            trace(message);
                        }
                        break;
                    }
                }
                auto runtime=GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll");
                auto acquire=guide_acquire?guide_acquire:runtime?reinterpret_cast<CheekyFgAcquireFn>(GetProcAddress(runtime,"CheekyFG_AcquirePair")):nullptr;
                auto release=guide_release?guide_release:runtime?reinterpret_cast<CheekyFgReleaseFn>(GetProcAddress(runtime,"CheekyFG_ReleasePair")):nullptr;
                CheekyFgPair guides;guides.capture_token=expected_token;guides.render_frame=expected_frame;
                // render_frame is deliberately zero for hook-free mode 3.
                // The nonzero color token still identifies the immutable pair.
                if(inference_slot&&(!tokens_match||!expected_token))job->guide_state=CheekyFgState::guide_frame_mismatch;
                if(inference_slot&&tokens_match&&expected_token&&acquire&&release&&acquire(&guides)) {
                    // Hook-free mode 3 can work with a different *verified*
                    // native guide queue. Explicit Signal/Wait orders the
                    // guide writes before FG reads; same-device alone is NOT
                    // sufficient. Other release modes still require one queue.
                    job->guides_valid=accept_guide_queue(guides,*job,expected_frame==0);
                    if(!job->guides_valid)job->guide_state=CheekyFgState::queue_mismatch;
                    if(!cheeky_fg_pair_matches_color(guides,expected_token,expected_frame)){
                        job->guides_valid=false;job->guide_state=CheekyFgState::guide_frame_mismatch;
                    }
                    for(unsigned eye=0;eye<2&&job->guides_valid;++eye) {
                        auto g=guides.eyes[eye];const auto d=job->color[eye]->texture->GetDesc();
                        if(!g.depth||!g.motion){job->guides_valid=false;job->guide_state=CheekyFgState::generation_failed;break;}
                        job->source_depth=unsigned(g.depth->GetDesc().Format);job->source_motion=unsigned(g.motion->GetDesc().Format);
                        const auto& layout=job->color[eye]->layout;
                        if(g.width!=(layout.width?layout.width:d.Width)||g.height!=(layout.height?layout.height:d.Height)){job->guides_valid=false;job->guide_state=CheekyFgState::generation_failed;break;}
                        if(layout.packed()){
                            // Metadata is published on the preceding guide evaluations.
                            // If geometry changed since capture, do not feed NGX a high-
                            // resolution crop partly supplied by low-res context.
                            const auto expected=compact_layout(layout.width,layout.height,true,
                                g.focus_width,g.focus_height,g.focus_x,g.focus_y,
                                g.peripheral_percent,layout.feather);
                            if(!expected.packed()||layout.x!=expected.x||layout.y!=expected.y||
                               layout.w!=expected.w||layout.h!=expected.h){
                                job->guides_valid=false;
                                job->guide_state=CheekyFgState::generation_failed;
                                if(trace){
                                    char message[250]{};
                                    sprintf_s(message,"FG compact capture/guide crop disagreement eye=%u "
                                        "packed=%u,%u,%u,%u matched=%u,%u,%u,%u; skipping FG pair",
                                        eye,layout.x,layout.y,layout.w,layout.h,
                                        expected.x,expected.y,expected.w,expected.h);
                                    trace(message);
                                }
                                break;
                            }
                        }
                    }
                    if(job->guides_valid){
                        // The plugin has ALREADY captured immutable guides.
                        // Retain its lease through inference rather than
                        // converting another full guide set at handoff.
                        // Worker preparation now crops/normalizes once using
                        // the original offsets, scales and jitter correction.
                        job->retain_guides(guides);job->guide_token=expected_token;
                        job->source_configuration=guides.eyes[0].configuration;job->render_frame=expected_frame;
                    }
                    release(&guides);
                }else if(inference_slot&&tokens_match&&expected_token)job->guide_state=CheekyFgState::guide_frame_mismatch;
                {std::lock_guard lock(mx);telemetry.color_token=expected_token;telemetry.guide_token=job->guide_token;
                    telemetry.color_render_frame=expected_frame;telemetry.guide_render_frame=job->guides_valid?expected_frame:0;
                    if(inference_slot&&job->guide_state==CheekyFgState::guide_frame_mismatch)++telemetry.mismatched_guides;}
                if(job->latency_diagnostics)
                    job->final_color_ready=producer.mark_color_ready(*c,LatencyProbe::cpu_now());
                good=producer.execute(*c,job->ready);
                if(!good&&inference_slot&&hookless_complete)hookless_complete(0);
            }else if(c){
                if(inference_slot&&hookless_complete)hookless_complete(0);
                // A changed layout can fail after one eye was recorded. Queue
                // those valid partial transitions so tracked pool states stay
                // consistent before falling back to the original native frame.
                std::uint64_t discarded{};producer.execute(*c,discarded);
            }
            if(!good)result=passthrough(end,"incomplete native color capture / changed swapchain layout");
            else {
                job->space=job->layers.scene_space();job->blend=end->environmentBlendMode;
                std::lock_guard lock(mx);job->id=id;job->epoch=epoch;job->handed_off_at=TimingClock::now();
                // Keep the debug overlay on native submissions too. Native
                // snapshots are immutable once published; use the last proven
                // FG region (one native frame behind for dynamic gaze).
                for(unsigned eye=0;eye<2;++eye)job->color[eye]->fg_border=last_fg_borders[eye];
                Pair native;native.upload_mode=job->upload_mode;native.native_id=native.current_id=id;native.ready=job->ready;
                native.native_color_ready=native.current_color_ready=job->color_ready;
                native.native_final_ready=native.current_final_ready=job->final_color_ready;
                native.native_captured=native.current_captured=job->captured_at;
                native.native=native.current=job->color;native.native_views=native.current_views=job->views;
                native.layers=job->layers;native.auxiliary=job->auxiliary;native.space=job->space;native.blend=job->blend;
                latest_native=std::move(native);
                if(inference_slot){pending.push_back(job);while(pending.size()>2){pending.front()->clear_guides();pending.pop_front();++skipped;}}
                ++frames;held_valid=true;changed.notify_all();
            }
        }
        {std::lock_guard lock(mx);if(!stopping)++ended;changed.notify_all();}
        return result;
    }
    static std::array<XrCompositionLayerProjectionView,2> midpoint(
        const std::array<XrCompositionLayerProjectionView,2>& a,const std::array<XrCompositionLayerProjectionView,2>& b) {
        auto result=b;
        for(unsigned eye=0;eye<2;++eye) {
            auto& p=result[eye].pose;for(unsigned k=0;k<3;++k)(&p.position.x)[k]=.5f*((&a[eye].pose.position.x)[k]+(&b[eye].pose.position.x)[k]);
            auto x=a[eye].pose.orientation,y=b[eye].pose.orientation;
            if(x.x*y.x+x.y*y.y+x.z*y.z+x.w*y.w<0)y={-y.x,-y.y,-y.z,-y.w};
            const float n=std::sqrt((x.x+y.x)*(x.x+y.x)+(x.y+y.y)*(x.y+y.y)+(x.z+y.z)*(x.z+y.z)+(x.w+y.w)*(x.w+y.w));
            p.orientation=n>0?XrQuaternionf{(x.x+y.x)/n,(x.y+y.y)/n,(x.z+y.z)/n,(x.w+y.w)/n}:b[eye].pose.orientation;
            for(unsigned k=0;k<4;++k)(&result[eye].fov.angleLeft)[k]=.5f*((&a[eye].fov.angleLeft)[k]+(&b[eye].fov.angleLeft)[k]);
        }
        return result;
    }
    void generation_loop() noexcept {
        SetThreadDescription(GetCurrentThread(),L"Cheeky FG completion");
        std::shared_ptr<Native> previous;bool reset_history=true,last_repeat_policy=true;
        unsigned measured_capture=UINT32_MAX,measured_upload=UINT32_MAX,measured_diagnostics=UINT32_MAX;std::uint64_t logged_samples{};
        try {for(;;) {
            std::shared_ptr<Native> job;
            {std::unique_lock lock(mx);changed.wait(lock,[&]{return stopping||!pending.empty();});if(stopping)break;
             job=pending.back();inflight_id=job->id;
             inflight_release_point=job->guides_valid?job->guides.release_point:0u;inflight_prepared=false;
             if(job->guides_valid){
                 if(submit_repeat_frames!=(job->guides.submit_repeats!=0)||active_release_point!=job->guides.release_point)cadence_window={};
                 submit_repeat_frames=job->guides.submit_repeats!=0;active_release_point=job->guides.release_point;
             }changed.notify_all();
             skipped+=pending.size()-1;
             for(auto& dropped:pending)if(dropped!=job)dropped->clear_guides();pending.clear();}
            if(measured_diagnostics!=unsigned(job->latency_diagnostics)){
                measured_diagnostics=unsigned(job->latency_diagnostics);logged_samples=0;
                std::lock_guard lock(mx);gpu_ready_ages={};gpu_ready_samples=predicted_samples=prediction_late=0;
            }
            if(measured_capture!=job->capture_mode||measured_upload!=job->upload_mode){
                measured_capture=job->capture_mode;measured_upload=job->upload_mode;logged_samples=0;
                worker.latency.clear_statistics();
                if(worker.device&&worker.queue)worker.gpu_timings.initialize(worker.device.Get(),worker.queue.Get());
                std::lock_guard lock(mx);cadence_window={};gpu_ready_ages={};gpu_ready_samples=predicted_samples=prediction_late=0;
            }
            if(job->guides_valid) {
                const auto point=job->guides.release_point;
                const bool changed_policy=!release_policy_known||last_release_point!=point;
                if(changed_policy) {
                    worker.latency.clear_statistics();logged_samples=0;
                    {std::lock_guard lock(mx);gpu_ready_ages={};gpu_ready_samples=predicted_samples=prediction_late=0;}
                    if(release_policy_known&&worker.device&&worker.queue)
                        worker.gpu_timings.initialize(worker.device.Get(),worker.queue.Get());
                    last_release_point=point;
                    if(trace)trace(point==3u?"FG native release=after generated frame OpenXR submission (Lua; histories retained)":
                        point==2u?"FG native release=after stereo GPU completion (Lua; histories retained)":
                        point==1u?"FG native release=after both-eye GPU preparation (Lua; histories retained)":
                        point==4u?"FG native release=4 ms after FG command submission (Lua; CPU timer, no GPU wait)":
                        "FG native release=after submission (Lua; default overlap)");
                }
                if(!release_policy_known||last_repeat_policy!=(job->guides.submit_repeats!=0)) {
                    worker.latency.clear_statistics();logged_samples=0;
                    {std::lock_guard lock(mx);gpu_ready_ages={};gpu_ready_samples=predicted_samples=prediction_late=0;}
                    last_repeat_policy=job->guides.submit_repeats!=0;
                    if(!changed_policy&&worker.device&&worker.queue)
                        worker.gpu_timings.initialize(worker.device.Get(),worker.queue.Get());
                    if(trace)trace(last_repeat_policy?"FG repeat submissions=ON (independent native clock)":
                        "FG repeat submissions=OFF (fresh content only; independent native clock)");
                }
                release_policy_known=true;
            }
            Pair pair;pair.upload_mode=job->upload_mode;pair.native_id=pair.current_id=job->id;pair.native=pair.current=job->color;
            std::shared_ptr<DirectRequest> direct_job;
            struct CompleteRequest {BufferedPresenter& owner;std::shared_ptr<DirectRequest>& request;
                ~CompleteRequest(){if(request){std::lock_guard lock(owner.mx);request->complete=true;owner.changed.notify_all();}}} complete_request{*this,direct_job};
            pair.native_color_ready=pair.current_color_ready=job->color_ready;
            pair.native_final_ready=pair.current_final_ready=job->final_color_ready;
            pair.native_captured=pair.current_captured=job->captured_at;
            pair.handed_off_at=job->handed_off_at;pair.worker_started_at=TimingClock::now();
            pair.native_views=pair.current_views=job->views;pair.layers=job->layers;pair.auxiliary=job->auxiliary;
            pair.space=job->space;pair.blend=job->blend;pair.ready=job->ready;
            bool success{};
            bool attempted{},history_was_reset{};
            if(previous&&previous->epoch==job->epoch&&previous->id+1==job->id&&previous->space==job->space&&job->guides_valid&&
               cheeky::foveated_dlss::fg_same_source_history(previous->source_configuration,job->source_configuration)&&
               previous->color[0]->texture->GetDesc().Width==job->color[0]->texture->GetDesc().Width&&
               previous->color[0]->texture->GetDesc().Height==job->color[0]->texture->GetDesc().Height) {
                // The snapshot is recorded on the game queue. Queue::Wait is
                // GPU-side, so neither the game nor the FG CPU worker needs to
                // block while capture finishes. It also covers the earlier
                // game-queue guide writes that precede the snapshot signal.
                // Lua setting travels with this native stereo pair. Switch queues
                // only on the single generation worker after the previous fence
                // has retired; recreating NGX handles prevents cross-queue history
                // hazards and makes the A/B measurements independent.
                const unsigned requested_priority=job->guides.fg_queue_priority<=2u?job->guides.fg_queue_priority:1u;
                ID3D12CommandQueue* requested=requested_priority==2u?queue.Get():requested_priority==0u?fg_normal_queue.Get():fg_high_queue.Get();
                ID3D12CommandQueue* selected=requested ? requested : (fg_high_queue.Get()?fg_high_queue.Get():fg_normal_queue.Get()?fg_normal_queue.Get():queue.Get());
                if(selected!=generation_queue.Get()) {
                    worker.wait_idle();worker.reset();generation_queue=selected;
                    generation_high_priority=fg_high_queue.Get()!=nullptr&&selected==fg_high_queue.Get();
                    reset_history=true;logged_samples=0;
                    {std::lock_guard lock(mx);cadence_window={};gpu_ready_ages={};gpu_ready_samples=predicted_samples=prediction_late=0;
                        generation_stages={};capture_to_completion={};completion_to_native={};completion_to_generated={};
                        frame_ages={};age_samples={};pacing_wait={};producer_cpu={};}
                    if(trace)trace(generation_high_priority?"DLSS-FG queue mode=HIGH (Lua; NGX histories reset)":
                        selected==fg_normal_queue.Get()?"DLSS-FG queue mode=separate NORMAL (Lua; NGX histories reset)":
                        "DLSS-FG queue mode=shared game queue (Lua; NGX histories reset)");
                }
                {std::lock_guard lock(mx);reported_queue_mode=selected==queue.Get()?2u:generation_high_priority?1u:0u;}
                if(last_fg_priority_requested!=requested_priority) {
                    last_fg_priority_requested=requested_priority;
                    if(trace) {
                        char msg[176]{};
                        sprintf_s(msg,"DLSS-FG queue request=%s selected=%s fallback=%u",
                            queue_mode_name(requested_priority),
                            generation_queue.Get()==fg_high_queue.Get()&&fg_high_queue.Get()?"HIGH":
                            generation_queue.Get()==fg_normal_queue.Get()&&fg_normal_queue.Get()?"separate NORMAL":"game queue",
                            selected!=requested?1u:0u);
                        trace(msg);
                    }
                }
                auto d=job->color[0]->texture->GetDesc();worker.trace=trace;worker.input_color_state=D3D12_RESOURCE_STATE_GENERIC_READ;
                const auto& color_layout=job->color[0]->layout;
                if(worker.initialize(device.Get(),generation_queue.Get(),color_layout.width?color_layout.width:unsigned(d.Width),color_layout.height?color_layout.height:d.Height,d.Format)) {
                    for(unsigned eye=0;eye<2;++eye){worker.color_layouts[eye]=job->color[eye]->layout;worker.previous_layouts[eye]=previous->color[eye]->layout;}
                    // The OpenXR capture has its own producer.fence, but it
                    // does not cover guide writes on a DIFFERENT game queue.
                    // Bridge that dependency before DLSS-G prepares the guides.
                    const auto guides_ready=job->guide_sync_value ?
                        generation_queue->Wait(job->guide_sync_fence.Get(),job->guide_sync_value) : S_OK;
                    if(FAILED(guides_ready)){worker.failed=true;worker.report("FG guide cross-queue GPU wait",unsigned(guides_ready));
                        throw std::runtime_error("FG guide cross-queue GPU wait failed");}
                    const auto ready_result=worker.latency.enqueue_wait(device.Get(),generation_queue.Get(),producer.fence.Get(),job->ready,job->latency_diagnostics);
                    if(FAILED(ready_result)){worker.failed=true;worker.report("FG capture dependency queue wait",unsigned(ready_result));}
                    if(FAILED(ready_result))throw std::runtime_error("FG capture dependency queue wait failed");
                    auto guides=job->guides;guides.producer_queue=generation_queue.Get();
                    if(previous->guide_token+2!=job->guide_token)reset_history=true;
                    // Hook-free mode 3 deliberately has no native-frame ID.
                    // Preserve history when the capture tokens are consecutive;
                    // reset on any transition between tagged and untagged mode.
                    if((previous->render_frame||job->render_frame)&&
                       previous->render_frame+1!=job->render_frame)reset_history=true;
                    history_was_reset=reset_history;
                    if(reset_history)for(auto& eye:guides.eyes)eye.reset=1;
                    if(trace && ((++pose_sample_count%60u)==1u)) {
                        auto angle=[](const XrQuaternionf& a,const XrQuaternionf& b){
                            const double dot=std::abs(double(a.x)*b.x+double(a.y)*b.y+double(a.z)*b.z+double(a.w)*b.w);
                            return 2.0*180.0/3.141592653589793*std::acos(std::clamp(dot,0.0,1.0));
                        };
                        for(unsigned eye=0;eye<2;++eye){
                            const auto& a=previous->views[eye].pose;
                            const auto& b=job->views[eye].pose;
                            const double dx=b.position.x-a.position.x,dy=b.position.y-a.position.y,dz=b.position.z-a.position.z;
                            char message[360]{};
                            sprintf_s(message,"FG POSE-DELTA native_prev=%llu native_next=%llu eye=%u "
                                "source_dt_ms=%.3f position_delta_m=%.6f angle_deg=%.5f "
                                "current_pose_xyz=%.4f,%.4f,%.4f (OpenXR submitted poses, not game render poses)",
                                static_cast<unsigned long long>(previous->id),static_cast<unsigned long long>(job->id),eye,
                                std::chrono::duration<double,std::milli>(job->captured_at-previous->captured_at).count(),std::sqrt(dx*dx+dy*dy+dz),
                                angle(a.orientation,b.orientation),b.position.x,b.position.y,b.position.z);
                            trace(message);
                        }
                    }
                    std::array<Camera,2> cameras;for(unsigned eye=0;eye<2;++eye)
                        cameras[eye]=camera(job->views[eye],&previous->views[eye],guides.eyes[eye].depth_inverted!=0);
                    const bool camera_pair_valid=std::all_of(cameras.begin(),cameras.end(),[](const auto& c){return c.valid;});

                    std::array<ID3D12Resource*,2> input{job->color[0]->texture.Get(),job->color[1]->texture.Get()},
                        prior{previous->color[0]->texture.Get(),previous->color[1]->texture.Get()},result{};
                    auto publication=d;publication.Format=worker.focus.color_format;
                    std::array<Worker::OutputCopy,2> copies;
                    bool available=true;
                    // Debug border is drawn by the final OpenXR upload. Avoid
                    // bypassing that upload via experimental direct XR output.
                    const bool border_upload=std::any_of(std::begin(guides.eyes),std::end(guides.eyes),[](const auto& g){return g.show_foveation_border!=0;});
                    const bool blocked_direct=job->direct_output_mode&&(border_upload||direct_blocked_target==direct_target_key(worker.width,worker.height,worker.focus.color_format));
                    if(blocked_direct){std::lock_guard lock(mx);++direct_stats.fallbacks;++direct_stats.unsupported;direct_stats.last_path=3;}
                    if(camera_pair_valid&&job->direct_output_mode&&!blocked_direct){
                        const bool supported=std::all_of(std::begin(guides.eyes),std::end(guides.eyes),[](const auto& g){
                            return g.peripheral_method<=2&&(g.focus_width<.999f||g.focus_height<.999f);});
                        if(supported){
                            direct_job=std::make_shared<DirectRequest>();direct_job->epoch=job->epoch;direct_job->native_id=previous->id;
                            direct_job->previous=previous->color;direct_job->format=worker.focus.color_format;direct_job->width=worker.width;direct_job->height=worker.height;
                            std::unique_lock lock(mx);++direct_stats.attempts;direct_request=direct_job;changed.notify_all();
                            changed.wait(lock,[&]{return stopping||job->epoch!=epoch||direct_job->decided;});
                            if(direct_job->allowed){copies=direct_job->targets;pair.direct_xr=true;
                                for(unsigned eye=0;eye<2;++eye){copies[eye].shading_mode=job->direct_shading_mode;pair.generated[eye]=std::make_shared<TransportImage>();
                                    pair.generated[eye]->texture=copies[eye].texture;pair.generated[eye]->state=copies[eye].state;}}
                        }else{std::lock_guard lock(mx);++direct_stats.unsupported;++direct_stats.fallbacks;direct_stats.last_path=3;
                            note_direct_failure_locked(CheekyFgDirectReason::configuration);}
                    }
                    for(unsigned eye=0;eye<2&&!pair.direct_xr&&camera_pair_valid;++eye) {
                        const auto& g=guides.eyes[eye];
                        const auto layout=compact_layout(worker.width,worker.height,job->color[eye]->layout.packed(),g.focus_width,g.focus_height,
                            g.focus_x,g.focus_y,g.peripheral_percent,g.focus_feather,g.resolution_percent);
                        publication.Width=layout.stored_width();publication.Height=layout.stored_height();
                        pair.generated[eye]=output.image(publication,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                        if(!pair.generated[eye]){available=false;break;}
                        pair.generated[eye]->layout=layout;
                        pair.generated[eye]->fg_border=fg_border_rect(worker.width,worker.height,g.focus_width,g.focus_height,
                            g.focus_x,g.focus_y,g.show_foveation_border!=0);
                        copies[eye]={pair.generated[eye]->texture.Get(),pair.generated[eye]->state,layout};
                    }
                    if(available&&camera_pair_valid) {
                        success=worker.generate(guides,input,prior,cameras,result,&copies,[](void* context) noexcept {
                            static_cast<BufferedPresenter*>(context)->inference_submitted();
                        },this,[](void* context) noexcept {
                            static_cast<BufferedPresenter*>(context)->inference_prepared();
                        });attempted=!worker.failed&&worker.temporal_evaluation_completed;
                        pair.submitted_at=worker.submitted_at;
                        for(unsigned eye=0;eye<2;++eye) {
                            pair.generated[eye]->state=copies[eye].state;
                            pair.generated[eye]->presentation_format=srgb_color(d.Format)&&publication.Format==DXGI_FORMAT_R8G8B8A8_UNORM?
                                DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:publication.Format;
                        }
                        if(pair.direct_xr){std::lock_guard lock(mx);
                            direct_job->fence=worker.fence;direct_job->serial=worker.serial;
                            if(success){const bool coarse=worker.focus.coarse_output[0]&&worker.focus.coarse_output[1];
                                shading_stats.valid=1;shading_stats.active_mode=coarse?job->direct_shading_mode:0u;
                                shading_stats.grid_width=coarse?worker.peripheral_width:worker.width;
                                shading_stats.grid_height=coarse?worker.peripheral_height:worker.height;
                                shading_stats.output_width=worker.width;shading_stats.output_height=worker.height;
                                if(coarse)++shading_stats.coarse_frames;else ++shading_stats.full_frames;}
                            for(const auto& f:worker.features)if(f.direct)++direct_stats.ngx_direct_eyes;else ++direct_stats.ngx_scratch_eyes;
                            direct_stats.last_path=worker.features[0].direct&&worker.features[1].direct?1u:2u;}
                    }
                }
            }
            if(success){pair.interpolated=true;pair.native_id=previous->id;pair.native=previous->color;
                pair.native_color_ready=previous->color_ready;
                pair.native_final_ready=previous->final_color_ready;
                pair.native_captured=previous->captured_at;
                pair.native_views=previous->views;pair.generated_views=midpoint(previous->views,job->views);
                pair.completed_at=TimingClock::now();}
            // Completed NGX warm-up advances history even when it supplies no
            // interpolated output. An early return or failed batch does not.
            reset_history=!attempted;if(job->latency_diagnostics)collect_color_stamps();
            measure_worker_memory();
            {std::lock_guard lock(mx);if(stopping)break;
                // Previous-frame history needs colors/poses, not its original
                // guides. Release completed leases now instead of pinning raw
                // textures in reusable slots or temporal history indefinitely.
                if(!worker.fence||worker.fence->GetCompletedValue()>=worker.serial)job->clear_guides();
                if(success&&!pair.direct_xr)for(unsigned eye=0;eye<2;++eye)last_fg_borders[eye]=pair.generated[eye]->fg_border;
                if(success&&active_release_point==3u&&job->epoch==epoch&&!degraded)awaiting_generated_submission=job->id;
                inflight_id=0;inflight_release_point=0;inflight_prepared=false;
                worker.latency.snapshot(experiment_state);
                experiment_state.color_capture_mode=job->capture_mode;experiment_state.upload_mode=job->upload_mode;
                if(trace&&experiment_state.latency_valid&&experiment_state.latency_samples!=logged_samples){
                    logged_samples=experiment_state.latency_samples;
                    const auto& d=experiment_state;char message[768]{};
                    sprintf_s(message,"FG calibrated latency sample=%llu rejected=%llu color=%u uploads=%u release=%u repeats=%u queue=%s mean_ms: queue_entry=%.3f capture_dependency=%.3f ready_to_fg=%.3f gpu_work=%.3f gpu_to_observed=%.3f enqueue_total=%.3f submit_total=%.3f; copies(full/direct/crop)=%llu/%llu/%llu uploads(copies/batches)=%llu/%llu",
                        d.latency_samples,d.latency_rejected,job->capture_mode,job->upload_mode,active_release_point,submit_repeat_frames?1u:0u,queue_mode_name(reported_queue_mode),
                        d.queue_entry.mean,d.capture_dependency.mean,d.ready_to_fg.mean,d.gpu_work.mean,d.gpu_to_observed.mean,d.enqueue_to_observed.mean,d.submit_to_observed.mean,
                        producer.full_copies.load(),direct_eye_copies.load(),eye_crop_copies.load(),uploads.upload_copies.load(),uploads.submissions.load());trace(message);
                }
                release_deferred_credit();
                if(job->epoch!=epoch||degraded){previous.reset();reset_history=true;continue;}
                if(pair.direct_xr){direct_job->success=success;if(success){++direct_stats.frames;direct_job->result=std::move(pair);}
                    else{++direct_stats.fallbacks;direct_stats.last_path=3;note_direct_failure_locked(CheekyFgDirectReason::generation);}}
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
                            static_cast<unsigned long long>(worker.serial));
                        trace(publication);
                    }
                    ready.push_back(std::move(pair));while(ready.size()>2)ready.pop_front();
                }
                if(success)completed_pair_id=job->id;
                // Once real interpolation is available, briefly stop the
                // speculative native mailbox from running ahead of its first
                // usable pair. Inference/native failures release this priming.
                if(success&&!fg_active)transition_until=ticks+8;
                if(worker.failed||!job->guides_valid)transition_until=0;
                if(worker.failed)pacing_active=false;
                const auto xr_ms=telemetry.xr_wait_ms,period_ms=telemetry.xr_period_ms;
                const auto color_token=telemetry.color_token,guide_token=telemetry.guide_token,mismatches=telemetry.mismatched_guides;
                const auto color_frame=telemetry.color_render_frame,guide_frame=telemetry.guide_render_frame;
                telemetry=Presenter::performance();telemetry.copy_wait_ms=0;telemetry.xr_wait_ms=xr_ms;telemetry.xr_period_ms=period_ms;
                telemetry.color_token=color_token;telemetry.guide_token=guide_token;telemetry.mismatched_guides=mismatches;
                telemetry.color_render_frame=color_frame;telemetry.guide_render_frame=guide_frame;
                telemetry.inference_color_format=unsigned(worker.focus.color_format);
                telemetry.shared_gpu_queue=generation_queue.Get()==queue.Get()?1u:0u;
                if(worker.gpu_timings.valid) {
                    extra_gpu.publication_samples=worker.gpu_timings.samples;
                    extra_gpu.publication_ms=worker.gpu_timings.publication.mean();
                    extra_gpu.publication_last_ms=worker.gpu_timings.publication.last;
                }
                telemetry.source_depth_format=job->source_depth;telemetry.source_motion_format=job->source_motion;
                state=success?CheekyFgState::generating:worker.failed?(worker.capability_ready?CheekyFgState::generation_failed:CheekyFgState::ngx_unavailable):
                    job->guides_valid?CheekyFgState::waiting_for_history:job->guide_state;
                if(job->guide_state==CheekyFgState::generation_failed&&!job->guides_valid)
                    sprintf_s(diagnostic,"FG guide snapshot unavailable: depth=%u motion=%u; native presentation continues",job->source_depth,job->source_motion);
                else if(job->guide_state==CheekyFgState::guide_frame_mismatch&&!job->guides_valid)
                    strcpy_s(diagnostic,"Waiting for matching color and guide frames. See the plugin frame-source diagnostics; native presentation continues.");
                else strcpy_s(diagnostic,worker.diagnostic);changed.notify_all();}
            previous=std::move(job);
        }}catch(...) {std::lock_guard lock(mx);state=CheekyFgState::generation_failed;degraded=true;fg_active=pacing_active=false;
            discard_pending();ready.clear();awaiting_generated_submission=0;inflight_id=0;inflight_release_point=0;inflight_prepared=false;++epoch;strcpy_s(diagnostic,"FG background processing failed; using original native submissions");changed.notify_all();}
        worker.wait_idle();output.drain();
    }
    bool ensure_chain(Chain& chain,const Image& image,bool scene=false) {
        auto d=image->texture->GetDesc();
        if(image->presentation_format!=DXGI_FORMAT_UNKNOWN)d.Format=image->presentation_format;
        if(scene){
            if(image->layout.width){d.Width=image->layout.width;d.Height=image->layout.height;}
            const bool srgb=srgb_color(d.Format);d.Format=inference_color_format(d.Format);
            if(srgb&&d.Format==DXGI_FORMAT_R8G8B8A8_UNORM)d.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        }
        const auto target_key=direct_target_key(unsigned(d.Width),d.Height,inference_color_format(d.Format));
        const bool writable=scene&&direct_output_mode==1u&&direct_blocked_target!=target_key;
        if(writable)d.Format=inference_color_format(d.Format);
        if(chain.handle&&(chain.desc.Width!=d.Width||chain.desc.Height!=d.Height||chain.desc.Format!=d.Format||chain.desc.DepthOrArraySize!=d.DepthOrArraySize||chain.direct_writable!=writable)) {
            uploads.drain();destroy_chain(chain.handle);chain={};
        }
        if(chain.handle)return true;
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};create.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        if(writable)create.usageFlags|=XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;
        create.format=d.Format;create.width=unsigned(d.Width);create.height=d.Height;create.arraySize=d.DepthOrArraySize;
        create.sampleCount=create.faceCount=create.mipCount=1;
        const auto created=xr.create(session,&create,&chain.handle);
        if(XR_FAILED(created)){
            if(!writable)return false;chain.handle={};direct_blocked_target=target_key;
            {std::lock_guard lock(mx);DirectRequest request;request.width=unsigned(d.Width);request.height=d.Height;request.format=d.Format;
                note_direct_failure_locked(CheekyFgDirectReason::chain_usage,0,&request,&chain,nullptr,created);}
            if(trace)trace("FG direct XR: runtime rejected writable scene image; falling back to compact uploads");
            return ensure_chain(chain,image,scene);
        }
        if(!enumerate_images(chain.handle,chain.images))return false;
        next_presenter_memory=0; // Publish newly allocated chains on this presentation, including during startup.
        chain.desc=d;chain.direct_writable=writable;chain.linear_color=writable&&d.Format==DXGI_FORMAT_R8G8B8A8_UNORM;return true;
    }
    bool upload_image(Chain& chain,const Image& source,bool scene=false) {
        if(!ensure_chain(chain,source,scene))return false;
        // xrEndFrame uses the most recently released image. Resubmitting that
        // image needs no acquire/copy/release when its immutable content has
        // not changed. A new content ID on every pool lease prevents recycled
        // pointers from being mistaken for the previously uploaded pixels.
        if(chain.uploaded==source->identity)return true;
        unsigned index{};XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if(XR_FAILED(xr.acquire(chain.handle,&acquire,&index)))return false;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
        const auto wait_begin=TimingClock::now();const auto wait_result=xr.wait_image(chain.handle,&wait);
        {std::lock_guard lock(mx);present_image_wait.add(elapsed_ms(wait_begin));}
        bool copied=false;
        if(XR_SUCCEEDED(wait_result)&&index<chain.images.size()){
            auto* target=chain.images[index].texture;
            if(!burst.recording_frame&&!burst.strict)
                copied=uploads.upload(source,target,ticks,chain.desc.Format,chain.linear_color,xr_image_state());
            else {
                auto* context=uploads.begin(ticks);
                if(!context){uploads.drain();context=uploads.begin(ticks);}
                if(context&&uploads.record_upload(*context,source,target,chain.desc.Format,chain.linear_color,xr_image_state())){
                    if(scene&&burst.recording_frame){
                        const unsigned eye=&chain==&scene_chains[0]?0u:1u;
                        burst.record(device.Get(),context->list.Get(),source->texture.Get(),source->state,
                            "source",eye,index,burst_native,burst_current,burst_generated,burst_presentation);
                        burst.record(device.Get(),context->list.Get(),target,xr_image_state(),
                            "openxr",eye,index,burst_native,burst_current,burst_generated,burst_presentation);
                    }
                    std::uint64_t ticket{};copied=uploads.execute(*context,ticket);
                    if(copied){burst.commit(uploads.fence.Get(),ticket);if(burst.strict)burst.wait_upload(uploads.fence.Get(),ticket);}
                    else burst.abandon();
                }else {if(context)context->list->Close();burst.abandon();}
            }
        }
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const bool released=XR_SUCCEEDED(release_image(chain.handle,index,&release));
        if(released&&copied){chain.uploaded=source->identity;chain.last_uploaded_index=index;}
        return released&&copied;
    }
    bool upload_batch(const std::vector<std::pair<Chain*,Image>>& requests) {
        struct Item {Chain* chain;Image source;unsigned index{};bool acquired{},waited{};};
        std::vector<Item> items;
        for(const auto& [chain,source]:requests){
            if(!chain->handle&&!ensure_chain(*chain,source))return false;
            if(chain->uploaded!=source->identity)items.push_back({chain,source});
        }
        if(items.empty())return true;
        bool good=true;
        for(auto& item:items){
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if(XR_FAILED(xr.acquire(item.chain->handle,&acquire,&item.index))){good=false;break;}
            item.acquired=true;
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
            const auto started=TimingClock::now();const auto result=xr.wait_image(item.chain->handle,&wait);
            {std::lock_guard lock(mx);present_image_wait.add(elapsed_ms(started));}
            item.waited=XR_SUCCEEDED(result);
            if(!item.waited||item.index>=item.chain->images.size()){good=false;break;}
        }
        if(good){
            auto* c=uploads.begin(ticks);if(!c){uploads.drain();c=uploads.begin(ticks);}
            good=c!=nullptr;
            if(c){
                for(auto& item:items)if(!uploads.record_upload(*c,item.source,item.chain->images[item.index].texture,item.chain->desc.Format,item.chain->linear_color,xr_image_state())){good=false;break;}
                if(good&&burst.recording_frame)for(auto& item:items){
                    if(item.chain!=&scene_chains[0]&&item.chain!=&scene_chains[1])continue;
                    const unsigned eye=item.chain==&scene_chains[0]?0u:1u;
                    burst.record(device.Get(),c->list.Get(),item.source->texture.Get(),item.source->state,
                        "source",eye,item.index,burst_native,burst_current,burst_generated,burst_presentation);
                    burst.record(device.Get(),c->list.Get(),item.chain->images[item.index].texture,xr_image_state(),
                        "openxr",eye,item.index,burst_native,burst_current,burst_generated,burst_presentation);
                }
                if(good){std::uint64_t ticket{};good=uploads.execute(*c,ticket);
                    if(good){burst.commit(uploads.fence.Get(),ticket);if(burst.strict)burst.wait_upload(uploads.fence.Get(),ticket);}
                    else burst.abandon();}
                else {c->list->Close();burst.abandon();}
            }
        }
        const bool submitted=good;
        // All copies MUST be queued before any image is released: the runtime
        // may insert its own fence at ReleaseSwapchainImage.
        for(auto& item:items)if(item.acquired&&item.waited){
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            const bool released=XR_SUCCEEDED(release_image(item.chain->handle,item.index,&release));
            if(submitted&&released){item.chain->uploaded=item.source->identity;item.chain->last_uploaded_index=item.index;}
            good&=released;
        }
        return good;
    }
    #include "fg_direct_xr.inl"
    void wake_producer_locked() {
        if(clock_pacing_active){changed.notify_all();return;}
        // Early late-selection wake and ordinary submission share one credit.
        if(last_credit_tick==ticks)return;
        last_credit_tick=ticks;
        if(!stopping&&!raw&&(!pacing_active||(ticks%2)==0)) {
            if(release_gate_active()||(pacing_active&&unsubmitted_native())) {
                // Default: hold until FG submission. The opt-in completion
                // policy holds through both eyes' ordinary completion fence.
                // Coalesce pacing ticks while either policy blocks production.
                mark_credit_deferred();
            }else credits=std::min(2u,credits+(ticks==1?2u:1u));
        }
        changed.notify_all();
    }
    void pacing_loop() noexcept {
        SetThreadDescription(GetCurrentThread(),L"Cheeky native pacing");
        try {
            std::unique_lock lock(mx);
            // The first game credit needs a real runtime prediction. Do not
            // let the independent clock expose an uninitialized frame state.
            changed.wait(lock,[&]{return stopping||telemetry.xr_period_ms>0;});
            auto next=TimingClock::now();
            while(!stopping) {
                const auto now=TimingClock::now();
                // A deferred native credit may become available between
                // display clock ticks. Wake exactly at the soft-release
                // deadline instead of adding a whole XR tick of latency.
                if(release_delay_until!=TimingClock::time_point{}&&now>=release_delay_until) {
                    release_delay_until={};
                    release_deferred_credit();
                    changed.notify_all(); // Existing wait/begin credits may also be gated.
                }
                if(now>=next) {
                    clock_late.add(std::chrono::duration<double,std::milli>(now-next).count());
                    const double display_ms=telemetry.xr_period_ms>0?telemetry.xr_period_ms:1000./120.;
                    const auto period=std::chrono::duration_cast<TimingClock::duration>(
                        std::chrono::duration<double,std::milli>(std::max(1.,display_ms*(pacing_active?2.:1.))));
                    if(!raw) {
                        if(release_gate_active()||(pacing_active&&unsubmitted_native())) {
                            mark_credit_deferred();
                        }else if(credit_deferred)release_deferred_credit();
                        else credits=std::min(2u,credits+1);
                        changed.notify_all();
                    }
                    next+=period;if(next<=now)next=now+period;
                }
                const auto wake=release_delay_until!=TimingClock::time_point{}?
                    std::min(next,release_delay_until):next;
                changed.wait_until(lock,wake);
            }
        }catch(...) {std::lock_guard lock(mx);terminal=XR_ERROR_RUNTIME_FAILURE;stopping=true;changed.notify_all();}
    }
    bool unsubmitted_native() const noexcept {
        return inflight_id>submitted_native_id||(!pending.empty()&&pending.back()->id>submitted_native_id);
    }
    bool release_gate_active() const noexcept {
        // Hold mx. Inspect pending jobs as well as the worker's latched policy
        // so pre-existing credits cannot let a render race worker pickup.
        if(stopping||raw||degraded)return false;
        if(awaiting_generated_submission)return true;
        if(release_delay_until!=TimingClock::time_point{}&&TimingClock::now()<release_delay_until)return true;
        if(inflight_id&&(inflight_id>submitted_native_id||inflight_release_point==2u||inflight_release_point==3u||
            (inflight_release_point==1u&&!inflight_prepared)))return true;
        return std::any_of(pending.begin(),pending.end(),[&](const auto& job){
            return job->guides_valid&&(job->guides.release_point!=0||job->id>submitted_native_id);
        });
    }
    bool generated_submission_pending() const noexcept {
        return awaiting_generated_submission||(inflight_id&&inflight_release_point==3u)||
            std::any_of(pending.begin(),pending.end(),[](const auto& job){return job->guides_valid&&job->guides.release_point==3u;});
    }
    void mark_credit_deferred() noexcept {
        // Hold mx. A missed clock tick is coalesced; measure the actual held duration.
        if(!credit_deferred){++deferred_credits;credit_deferred=true;credit_deferred_since=TimingClock::now();}
    }
    void release_deferred_credit() noexcept {
        // Hold mx. Coalesce missed ticks rather than bursting queued renders.
        if(credit_deferred&&!stopping&&!raw&&!unsubmitted_native()&&!release_gate_active()) {
            credits=std::min(2u,credits+1);credit_deferred=false;++released_deferred_credits;
            if(credit_deferred_since!=TimingClock::time_point{})credit_deferral.add(elapsed_ms(credit_deferred_since));
            credit_deferred_since={};changed.notify_all();
        }
    }
    void inference_submitted() noexcept {
        std::lock_guard lock(mx);submitted_native_id=std::max(submitted_native_id,inflight_id);
        if(inflight_release_point==4u) {
            release_delay_until=TimingClock::now()+std::chrono::milliseconds(4);
            changed.notify_all(); // Wake the pacing clock to install the deadline.
        }
        release_deferred_credit();
    }
    void inference_prepared() noexcept {
        std::lock_guard lock(mx);inflight_prepared=true;release_deferred_credit();changed.notify_all();
    }
    void wake_producer() {std::lock_guard lock(mx);wake_producer_locked();}
    static TimingClock::time_point pair_wait_deadline(TimingClock::time_point start,XrDuration period) noexcept {
        // Match the reference's heuristic: at most 10 ms, reserving 4 ms of
        // the XR period for uploads/rendering/submission. Never a GPU deadline.
        const auto budget=period>0?std::min<XrDuration>(10000000,std::max<XrDuration>(0,period-4000000)):10000000;
        return start+std::chrono::nanoseconds(budget);
    }
    std::deque<Pair>::iterator newest_ready_pair(double position) {
        auto selected=ready.end();
        for(auto it=ready.begin();it!=ready.end();++it)
            if(producer.completed(it->ready)&&double(it->native_id)>=position)selected=it;
        return selected;
    }
    void wait_for_new_pair(TimingClock::time_point start,XrDuration period,double position=0) {
        std::unique_lock lock(mx);
        if(!fg_active||stopping||raw||degraded||state!=CheekyFgState::generating)return;
        const auto baseline=completed_pair_id,generation=epoch;
        const auto deadline=pair_wait_deadline(start,period);
        late_wait_budget_ms=std::chrono::duration<double,std::milli>(deadline-start).count();
        const auto available=newest_ready_pair(position);
        // Our ready queue owns pairs which have not yet been selected. Unlike
        // a reference host re-reading its active shared-memory publication,
        // a forward pair here is already useful NEW content. Do not wait for
        // yet another native render when that pair can be presented now.
        if(available!=ready.end()&&(double(available->native_id)>position||
            (available->interpolated&&double(available->current_id)>position))){++late_ready_skips;return;}
        // Snapshot BEFORE releasing the game credit. An already-published pair
        // cannot falsely satisfy this wait; it remains selectable on timeout.
        wake_producer_locked();
        // Unlike the reference's per-frame transport token, our game credit is
        // released only at native cadence. Do not burn the selection budget
        // when no newer render/inference can arrive before the next credit.
        if(inflight_id<=baseline&&(pending.empty()||pending.back()->id<=baseline)&&
            waited==ended&&!(credits&&app_waiters)){++late_idle_skips;return;}
        const auto waiting=TimingClock::now();if(waiting>=deadline)return;
        changed.wait_until(lock,deadline,[&]{return stopping||raw||degraded||epoch!=generation||
            state!=CheekyFgState::generating||completed_pair_id>baseline||(direct_request&&!direct_request->decided);});
        late_pair_wait.add(elapsed_ms(waiting));++late_wait_samples;
        if(completed_pair_id>baseline&&epoch==generation)++late_wait_arrivals;
        else if(!stopping&&!raw&&!degraded&&epoch==generation&&TimingClock::now()>=deadline)++late_wait_timeouts;
    }
    bool render(Pair& pair,const std::array<Image,2>& images,
                const std::array<XrCompositionLayerProjectionView,2>& original_views,bool generated,bool repeated,XrTime time,
                XrResult& result,bool& closed) {
        // An interpolated pair is published only after the worker's single
        // inference + immutable-copy fence completes. Native pairs retain the
        // producer fence; both paths therefore refer to the original job here.
        if(!producer.completed(pair.ready))return false;
        burst_native=pair.native_id;burst_current=pair.current_id;burst_generated=generated;
        burst_presentation=ticks;
        burst.begin_submission();
        if(latency_diagnostics.load()!=0)collect_native_submission_probes();
        auto views=original_views;
        std::vector<std::pair<Chain*,Image>> batch;
        for(unsigned eye=0;eye<2;++eye) {
            auto& chain=scene_chains[eye];
            if(generated&&pair.direct_xr){}else if(pair.upload_mode==1u){if(!ensure_chain(chain,images[eye],true))return false;batch.push_back({&chain,images[eye]});}
            else if(!upload_image(chain,images[eye],true))return false;
            views[eye].type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;views[eye].next=nullptr;
            views[eye].subImage={chain.handle,{{0,0},{int(chain.desc.Width),int(chain.desc.Height)}},0};
        }
        auto layers=pair.layers;
        for(auto& a:pair.auxiliary){
            if(pair.upload_mode==1u){auto& chain=auxiliary_chains[a.source];if(!ensure_chain(chain,a.image))return false;batch.push_back({&chain,a.image});}
            else if(!upload_image(auxiliary_chains[a.source],a.image))return false;
        }
        if(pair.upload_mode==1u&&!upload_batch(batch))return false;
        uploads.close_profile_frame(ticks);
        if(!subimages(layers,[&](XrSwapchainSubImage& sub){const auto i=auxiliary_chains.find(sub.swapchain);
            if(i==auxiliary_chains.end())return false;sub.swapchain=i->second.handle;return true;}))return false;
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
            if(!generated&&!repeated&&XR_SUCCEEDED(result)&&latency_diagnostics.load()!=0) {
                // The native eyes have been submitted to the OpenXR runtime.
                // The GPU may still be working. Resolve their completed GPU
                // timestamps on a subsequent presenter iteration.
                pending_native_submissions.push_back({pair.native_color_ready,pair.native_final_ready,submitted_cpu,GetTickCount64()});
                if(pair.native_captured!=TimingClock::time_point{}&&pair.native_captured<=submitted_at)
                    native_capture_to_submit.add(std::chrono::duration<double,std::milli>(submitted_at-pair.native_captured).count());
            }}
        if(XR_SUCCEEDED(result)){std::lock_guard lock(mx);note_frame_age(pair,generated,repeated,submitted_at);
            if(generated){note_gpu_ready_age(pair,submitted_cpu,display_cpu);finish_generated_gate(pair.current_id,true);}}
        return XR_SUCCEEDED(result);
    }
    bool fresh_content(const Pair& active,bool have_active,bool generated_phase,double position) {
        // Hold mx. A pair whose native image was already displayed can still
        // contribute its interpolation; do not submit that native image twice.
        if(have_active&&(generated_phase||double(active.native_id)>position))return true;
        const auto selected=newest_ready_pair(position);
        if(selected!=ready.end()&&(double(selected->native_id)>position||
            (selected->interpolated&&double(selected->current_id)>position)))return true;
        return !fg_active&&ticks>=transition_until&&latest_native&&producer.completed(latest_native->ready)&&
            double(latest_native->native_id)>position;
    }
    bool await_content(const Pair& active,bool have_active,bool generated_phase,double position,bool& allow_repeats) {
        std::unique_lock lock(mx);
        bool waited_for_content=false;
        const auto started_wait=TimingClock::now();
        while(!stopping&&!raw&&!submit_repeat_frames&&!(direct_request&&!direct_request->decided)&&!fresh_content(active,have_active,generated_phase,position)) {
            waited_for_content=true;
            // FG completion and new captures notify changed. Poll only an
            // outstanding producer fence (native-only startup/fallback), not
            // a quiet mailbox: idle fresh-only mode must not spin at 1 kHz.
            const bool producer_pending=(!fg_active&&latest_native&&!producer.completed(latest_native->ready))||
                std::any_of(ready.begin(),ready.end(),[&](const auto& pair){return !producer.completed(pair.ready);});
            if(producer_pending)changed.wait_for(lock,std::chrono::milliseconds(1));
            else changed.wait(lock);
        }
        if(waited_for_content)presenter_content_wait.add(elapsed_ms(started_wait));
        allow_repeats=submit_repeat_frames;return !stopping;
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
        try {for(;;) {
            // Runtime frame ownership cannot transfer midway through the old
            // native frame. BeginFrame may discard an unfinished frame rather
            // than wait for its EndFrame, which would race our own EndFrame.
            // Wait before acquiring a runtime frame so stop cannot strand a wait.
            {std::unique_lock lock(mx);
                changed.wait(lock,[&]{return stopping||!native_frame_pending;});
                if(stopping)break;}
            bool allow_repeats{};
            if(!await_content(active,have_active,generated_phase,last_position,allow_repeats))break;
            XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};XrFrameState frame{XR_TYPE_FRAME_STATE};
            const auto start=TimingClock::now();auto result=xr.wait_frame(session,&wait,&frame);const auto waited_ms=elapsed_ms(start);
            if(XR_FAILED(result)){runtime_failure("runtime xrWaitFrame",result);break;}
            const auto present_start=TimingClock::now();
            burst.log=trace;burst.poll_keys();
            XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};result=xr.begin_frame(session,&begin);
            {std::lock_guard lock(mx);runtime_begin.add(elapsed_ms(present_start));}
            if(XR_FAILED(result)){runtime_failure("runtime xrBeginFrame",result);break;}
            const XrFrameEndInfo* raw_frame{};bool stop_now{};
            {std::lock_guard lock(mx);raw_frame=raw;stop_now=stopping;
                if(awaiting_generated_submission&&(last_position>=double(awaiting_generated_submission)||frame.shouldRender!=XR_TRUE))
                    finish_generated_gate(awaiting_generated_submission,false);
                if(frame.shouldRender!=XR_TRUE&&direct_request&&!direct_request->decided){
                    note_direct_failure_locked(CheekyFgDirectReason::cancelled,0,direct_request.get());
                    direct_request->decided=true;direct_request.reset();++direct_stats.fallbacks;direct_stats.last_path=3;changed.notify_all();}
                predicted=frame;telemetry.xr_period_ms=double(frame.predictedDisplayPeriod)/1000000.;
                xr_wait.add(waited_ms);telemetry.xr_wait_ms=xr_wait.mean();++ticks;changed.notify_all();
            }
            if(allow_repeats&&!generated_phase&&frame.shouldRender==XR_TRUE)wait_for_new_pair(present_start,frame.predictedDisplayPeriod,last_position);
            Pair direct_pair;
            const bool have_direct=frame.shouldRender==XR_TRUE&&service_direct_output(last_position,direct_pair);
            {std::lock_guard lock(mx);raw_frame=raw;stop_now=stopping;
                if(have_direct&&!raw_frame&&!stop_now&&!degraded){active=std::move(direct_pair);have_active=true;generated_phase=true;fg_active=pacing_active=true;
                    while(!ready.empty()&&ready.front().current_id<=active.current_id)ready.pop_front();}
                else if(!generated_phase&&!raw_frame&&!stop_now&&!degraded) {
                    auto selected=newest_ready_pair(last_position);
                    if(selected!=ready.end()) {
                        active=std::move(*selected);ready.erase(ready.begin(),std::next(selected));have_active=true;
                        fg_active=active.interpolated;
                        if(!allow_repeats&&active.interpolated&&double(active.native_id)<=last_position&&
                            double(active.current_id)>last_position)generated_phase=true;
                        // A transient skipped/reset pair must not accelerate
                        // native production to 1x and overload FG again.
                        if(fg_active)pacing_active=true;
                    }
                    // Prioritize a usable interpolation pair. During NGX
                    // startup, keep showing new native frames even when every
                    // inference lease is busy. Never rewind to a stale pair.
                    if(!fg_active&&!generated_submission_pending()&&ticks>=transition_until&&latest_native&&producer.completed(latest_native->ready)&&
                       double(latest_native->native_id)>=last_position&&
                       (!have_active||latest_native->native_id>active.native_id)) {
                        active=*latest_native;have_active=true;
                    }
                }
            }
            bool submitted{},is_generated{},closed{},repeated{};
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
                const auto& images=is_generated?active.generated:active.native;
                const auto& views=is_generated?active.generated_views:active.native_views;
                repeated=!is_generated&&double(active.native_id)==last_position;
                // Snapshot identity BEFORE the generated submission advances
                // active.native_id/current_views to the next real frame.
                trace_native_id=active.native_id;
                trace_current_id=active.current_id;
                trace_source_position=double(active.native_id)+(is_generated?.5:0.);
                trace_source_left=images[0]?images[0]->identity:0;
                trace_source_right=images[1]?images[1]->identity:0;
                trace_direct=is_generated&&active.direct_xr;
                // A direct request may wake a tick only to fall back. Fresh-only
                // mode must not use that wake to resubmit the preceding native.
                if(allow_repeats||!repeated)submitted=render(active,images,views,is_generated,repeated,frame.predictedDisplayTime,result,closed);
                if(submitted) {
                    last_position=double(active.native_id)+(is_generated?.5:0.);
                    if(is_generated){active.native=active.current;active.native_views=active.current_views;active.native_id=active.current_id;
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
            // Capture each runtime frame, including empty submissions, so
            // silent skips cannot masquerade as a clean N/G/N cadence.
            // Use the existing FG Latency Diagnostics switch to avoid any
            // additional output or locking in ordinary gameplay.
            if(trace&&latency_diagnostics.load()!=0&&!raw_frame) {
                const bool valid=XR_SUCCEEDED(result);
                const bool backward_time=valid&&presentation_trace_last_time!=0&&
                    frame.predictedDisplayTime<=presentation_trace_last_time;
                const bool backward_content=valid&&submitted&&!repeated&&
                    trace_source_position<=presentation_trace_last_position;
                const bool gap=valid&&submitted&&!repeated&&
                    presentation_trace_last_position>=0&&trace_source_position>presentation_trace_last_position+1.01;
                const bool wrong_upload=valid&&submitted&&!trace_direct&&
                    (scene_chains[0].uploaded!=trace_source_left||
                     scene_chains[1].uploaded!=trace_source_right);
                if(backward_time||backward_content||wrong_upload)++presentation_trace_anomalies;
                if(valid){
                    presentation_trace_last_time=frame.predictedDisplayTime;
                    if(submitted&&!repeated)presentation_trace_last_position=trace_source_position;
                }
                ++presentation_trace_events;
                // Complete first 3,000 events (~25-40 s), then sample every
                // fourth event indefinitely. ALWAYS log anomalies and skips.
                const bool write=presentation_trace_events<=3000||
                    (presentation_trace_events%4)==0||backward_time||backward_content||
                    gap||wrong_upload||!submitted||repeated||!valid;
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
                        "position=%.1f left_tex=%llu right_tex=%llu xr_left=%u xr_right=%u "
                        "upload_match=%u display_ns=%lld period_ns=%lld "
                        "should_render=%u xr_result=%ld forward=%u time_forward=%u gap=%u repeats=%u "
                        "predicted_lead_ms=%.3f ready_pairs=%llu generated_total=%llu anomalies=%llu",
                        static_cast<unsigned long long>(presentation_trace_events),
                        static_cast<unsigned long long>(trace_epoch),static_cast<unsigned long long>(ticks),
                        submitted?(is_generated?"GEN":repeated?"REPEAT":"NATIVE"):"EMPTY",
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
                        repeated?1u:0u,predicted_lead_ms,static_cast<unsigned long long>(trace_ready),
                        static_cast<unsigned long long>(trace_generated),
                        static_cast<unsigned long long>(presentation_trace_anomalies));
                    trace(timeline);
                }
            }
            measure_presenter_memory();
            char age_log[512]{},stage_log[512]{},cadence_log[512]{},gpu_age_log[512]{},pacing_log[512]{},native_latency_log[512]{};
            {std::lock_guard lock(mx);
                if(is_generated&&submitted){++generated_count;last_generated_submit_tick=GetTickCount64();}
                if(submitted&&!is_generated&&!raw_frame){if(repeated)++repeated_presentations;else ++distinct_native;}
                present_cpu.add(elapsed_ms(present_start));
                if(XR_SUCCEEDED(result)){++runtime_submissions;
                    const auto now_ms=std::chrono::duration<double,std::milli>(TimingClock::now().time_since_epoch()).count();
                    cadence_window.add(now_ms,{native_count,distinct_native,generated_count,repeated_presentations,runtime_submissions});}
                if(trace&&is_generated&&submitted&&(generated_count%120)==0) {
                    const auto c=cadence();
                    sprintf_s(cadence_log,"FG experiment cadence: release=%s repeats=%s queue=%s clock=independent valid=%u game=%.3f native=%.3f generated=%.3f unique=%.3f repeated=%.3f submissions=%.3f; xr_wait_ms=%.3f xr_end_ms=%.3f present_ms=%.3f",
                        active_release_point==3u?"generated-submission":active_release_point==2u?"completion":active_release_point==1u?"preparation":active_release_point==4u?"submit+4ms":"submission",
                        submit_repeat_frames?"ON":"OFF",queue_mode_name(reported_queue_mode),c.valid,c.game_fps,c.native_fps,c.generated_fps,c.unique_fps,c.repeat_fps,c.submission_fps,
                        xr_wait.mean(),runtime_end.mean(),present_cpu.mean());
                    sprintf_s(age_log,"FG source-age CPU ms: generated_samples=%llu latest_capture_to_complete=%.3f complete_to_generated_submit=%.3f newest_input_age=%.3f complete_to_native_submit=%.3f; late_waits=%llu arrivals=%llu timeouts=%llu ready_skips=%llu idle_skips=%llu",
                        static_cast<unsigned long long>(generated_count),capture_to_completion.mean(),completion_to_generated.mean(),
                        frame_ages[2].mean(),completion_to_native.mean(),static_cast<unsigned long long>(late_wait_samples),
                        static_cast<unsigned long long>(late_wait_arrivals),static_cast<unsigned long long>(late_wait_timeouts),
                        static_cast<unsigned long long>(late_ready_skips),static_cast<unsigned long long>(late_idle_skips));
                    sprintf_s(stage_log,"FG capture-path CPU ms: samples=%u capture_to_handoff=%.3f handoff_to_worker=%.3f worker_to_GPU_submit=%.3f GPU_submit_to_complete=%.3f sum=%.3f; credit_deferred=%llu released_after_submit_or_retire=%llu outstanding=%u",
                        generation_stages[0].count,generation_stages[0].mean(),generation_stages[1].mean(),generation_stages[2].mean(),
                        generation_stages[3].mean(),generation_stages[0].mean()+generation_stages[1].mean()+generation_stages[2].mean()+generation_stages[3].mean(),
                        static_cast<unsigned long long>(deferred_credits),static_cast<unsigned long long>(released_deferred_credits),unsigned(credit_deferred));
                    const auto time=pacing_timeline();
                    sprintf_s(pacing_log,"FG CPU pacing timeline: clock_late=%.3f deferred_credit=%.3f credit_wait=%.3f begin_gate=%.3f presenter_content=%.3f native_end=%.3f generated_end=%.3f GPU_submit_to_complete=%.3f; deferred=%llu releases=%llu",
                        time.clock_late_ms,time.credit_deferred_ms,time.game_wait_credit_ms,time.game_begin_gate_ms,
                        time.presenter_content_ms,time.native_xr_end_ms,time.generated_xr_end_ms,
                        time.fg_submit_to_complete_ms,time.deferred_events,time.deferred_releases);
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
                if(have_active&&!submitted&&!stop_now&&frame.shouldRender==XR_TRUE&&(allow_repeats||!repeated)){++fallback_count;state=CheekyFgState::presentation_failed;
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
};
}
