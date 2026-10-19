#pragma once
#include "fg_transport.hpp"
#include "../src/d3d12_native.hpp"
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
    struct CapturePlan {D3D12_RESOURCE_DESC desc{};std::vector<Slice> slices;bool full_required{};};
    struct DirectCapture {std::vector<std::pair<Slice,Image>> eyes;};
    static bool same_slice(const Slice& a,const Slice& b) noexcept {
        return a.layer==b.layer&&a.rect.offset.x==b.rect.offset.x&&a.rect.offset.y==b.rect.offset.y&&
            a.rect.extent.width==b.rect.extent.width&&a.rect.extent.height==b.rect.extent.height;
    }
    struct Auxiliary {XrSwapchain source{};Image image;};
    struct Native {
        std::uint64_t id{},ready{},epoch{};
        unsigned capture_mode{},upload_mode{};bool latency_diagnostics{};
        TimingClock::time_point captured_at{},handed_off_at{};
        std::array<Image,2> color;
        std::array<ColorReady,2> color_ready;
        std::array<XrCompositionLayerProjectionView,2> views;
        XrSpace space{};XrEnvironmentBlendMode blend{};
        FrameLayers layers;
        std::vector<Auxiliary> auxiliary;
        CheekyFgPair guides;
        ComPtr<IUnknown> guide_lease;
        std::array<ComPtr<ID3D12Resource>,2> guide_depth,guide_motion;
        void retain_guides(const CheekyFgPair& source) noexcept {
            guides=source;guide_lease=source.lease;
            for(unsigned eye=0;eye<2;++eye){guide_depth[eye]=source.eyes[eye].depth;guide_motion[eye]=source.eyes[eye].motion;}
            // Explicit members own the returned references. The metadata copy
            // must never be passed to the bridge's ReleasePair function.
            guides.lease=nullptr;guides.producer_queue=nullptr;
        }
        void clear_guides() noexcept {guides={};guide_depth={};guide_motion={};guide_lease.Reset();}
        bool guides_valid{};
        CheekyFgState guide_state{CheekyFgState::waiting_for_guides};
        unsigned source_depth{},source_motion{};
        std::uint64_t guide_token{};
        std::uint64_t render_frame{};
    };
    struct Pair {
        std::uint64_t native_id{},current_id{},ready{};
        unsigned upload_mode{};
        TimingClock::time_point native_captured{},current_captured{},completed_at{},handed_off_at{},worker_started_at{},submitted_at{};
        std::array<Image,2> native,generated,current;
        std::array<ColorReady,2> current_color_ready;
        std::array<XrCompositionLayerProjectionView,2> native_views,generated_views,current_views;
        FrameLayers layers;
        std::vector<Auxiliary> auxiliary;
        XrSpace space{};XrEnvironmentBlendMode blend{};
        bool interpolated{};
    };
    struct Chain {
        XrSwapchain handle{};std::vector<XrSwapchainImageD3D12KHR> images;D3D12_RESOURCE_DESC desc{};
        std::uint64_t uploaded{}; // Content identity; GPU contexts retain sources only until their upload fence.
    };
    std::condition_variable_any changed;
    std::mutex capture_mutex;
    std::thread presentation_thread,generation_thread,pacing_thread;
    Transport producer,output,uploads;
    // Default: dedicated HIGH-priority inference queue. Lua can also select a
    // separate NORMAL queue or share the application's original queue. The
    // OpenXR binding, guide capture and uploads always retain the original queue.
    ComPtr<ID3D12CommandQueue> generation_queue;
    ComPtr<ID3D12CommandQueue> fg_high_queue,fg_normal_queue;
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
    TimingAverage app_image_wait,present_image_wait,runtime_begin,runtime_end,present_cpu;
    std::array<TimingAverage,3> app_calls;
    CadenceWindow cadence_window;
    std::uint64_t distinct_native{},repeated_presentations{},runtime_submissions{};
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
    void measure_worker_memory() {
        // Single generation worker, after the prior GPU fence retired. Publish
        // only integer snapshots; diagnostic readers never walk live resources.
        const auto now=GetTickCount64();if(now<next_worker_memory)return;
        CheekyFgMemory value;fg_memory::Collector collect(device.Get());
        value.categories[fg_memory::generated]=output.memory(collect);
        for(const auto& eye:worker.focus.inputs)for(const auto& input:eye){
            collect.add(value.categories[fg_memory::prepared_color],input.color.Get());
            collect.add(value.categories[fg_memory::prepared_depth],input.depth.Get());
            collect.add(value.categories[fg_memory::prepared_motion],input.motion.Get());
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
        std::lock_guard lock(mx);runtime_end.add(elapsed_ms(start));return result;
    }
    const char* diagnostic_text() const noexcept override {return diagnostic;}
    // Caller holds mx (or has joined the workers). Dropped jobs have never
    // submitted inference, so their raw snapshot leases can retire immediately.
    void discard_pending() noexcept {for(auto& job:pending)job->clear_guides();pending.clear();}
    void stop() noexcept {
        {std::lock_guard lock(mx);stopping=true;changed.notify_all();}
        if(presentation_thread.joinable())presentation_thread.join();
        if(generation_thread.joinable())generation_thread.join();
        if(pacing_thread.joinable())pacing_thread.join();
        std::lock_guard capture_lock(capture_mutex);
        producer.drain();output.drain();uploads.drain();worker.wait_idle();
        std::lock_guard lock(mx);discard_pending();ready.clear();latest_native.reset();slots={};
        captured.clear();direct_captures.clear();capture_plans.clear();captured_descs.clear();capture_tokens.clear();capture_frames.clear();capture_times.clear();capture_ready.clear();capture_overflow=false;capture_cpu_accumulator=0;started=false;credits=0;fg_active=pacing_active=false;raw=nullptr;raw_done=true;
        waited=begun=ended=0;ticks=transition_until=0;degraded=false;app_display_time=0;predicted={XR_TYPE_FRAME_STATE};++epoch;
        app_image_wait={};present_image_wait={};runtime_begin={};runtime_end={};present_cpu={};app_calls={};cadence_window={};
        producer.gpu_cost.reset();uploads.gpu_cost.reset();extra_gpu={};
        frame_ages={};age_samples={};late_pair_wait={};late_wait_samples=late_wait_arrivals=late_wait_timeouts=0;
        late_wait_budget_ms=0;last_credit_tick=completed_pair_id=inflight_id=0;
        late_ready_skips=late_idle_skips=0;capture_to_completion={};completion_to_native={};completion_to_generated={};
        generation_stages={};deferred_credits=released_deferred_credits=submitted_native_id=0;credit_deferred=false;
        gpu_ready_ages={};gpu_ready_samples=predicted_samples=prediction_late=0;experiment_state={};
        awaiting_generated_submission=generated_gate_releases=generated_gate_skips=0;
        memory_state={};next_capture_memory=next_worker_memory=next_presenter_memory=0;
        inflight_release_point=last_release_point=active_release_point=0;inflight_prepared=release_policy_known=clock_pacing_active=false;submit_repeat_frames=true;
        distinct_native=repeated_presentations=runtime_submissions=0;
    }
    void destroy_chains() noexcept {
        for(auto& c:scene_chains){if(c.handle)xr.destroy(c.handle);c={};}
        for(auto& [key,c]:auxiliary_chains)if(c.handle)xr.destroy(c.handle);
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
        if(configuration)invalidate_temporal();
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
        {std::lock_guard capture_lock(capture_mutex);storage_options=storage;}
        const auto query=transport_options_query?transport_options_query:runtime?
            reinterpret_cast<CheekyFgTransportOptionsFn>(GetProcAddress(runtime,"CheekyFG_GetTransportOptionsV1")):nullptr;
        CheekyFgTransportOptions options;
        if(query&&query(&options)){
            const auto old_color=color_capture_mode.exchange(options.color_capture_mode==1u?1u:0u);
            const auto old_upload=upload_mode.exchange(options.upload_mode==1u?1u:0u);
            const auto old_diagnostics=latency_diagnostics.exchange(options.latency_diagnostics==1u?1u:0u);
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
        std::unique_lock lock(mx);
        if(waited==begun)return XR_ERROR_CALL_ORDER_INVALID;
        // A pipelined xrWaitFrame may already have consumed a credit before
        // the previous end_frame enqueues its FG job. Gate begin_frame too.
        changed.wait(lock,[&]{return stopping||(begun==ended&&!release_gate_active());});
        if(stopping)return XR_ERROR_SESSION_NOT_RUNNING;
        ++begun;changed.notify_all();return XR_SUCCESS;
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
            for(auto slice:plan->second.slices){
                const auto& s=storage_options;
                const auto layout=compact_layout(slice.rect.extent.width,slice.rect.extent.height,s.compact!=0,s.width,s.height,s.x,s.y,s.peripheral_percent,s.feather);
                auto image=layout.packed()?producer.packed_region(*c,original,D3D12_RESOURCE_STATE_RENDER_TARGET,slice.rect,slice.layer,format,layout):
                    producer.region_copy(*c,original,D3D12_RESOURCE_STATE_RENDER_TARGET,slice.rect,slice.layer,format);
                if(!image){capture_overflow=true;std::uint64_t discarded{};producer.execute(*c,discarded);return false;}
                eyes.eyes.push_back({slice,std::move(image)});++direct_eye_copies;
            }
        }else {
            copy=producer.copy(*c,original,D3D12_RESOURCE_STATE_RENDER_TARGET,format);
            if(color_capture_mode==1u&&(plan==capture_plans.end()||!plan->second.full_required))++capture_fallbacks;
            if(!copy){capture_overflow=true;c->list->Close();return false;}
        }
        std::uint64_t ticket{};if(!producer.execute(*c,ticket))return false;
        if(direct){captured.erase(sc);direct_captures[sc]=std::move(eyes);}
        else {direct_captures.erase(sc);captured[sc]=std::move(copy);}
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
    XrResult passthrough(const XrFrameEndInfo* end) noexcept {
        std::unique_lock lock(mx);if(!degraded)state=CheekyFgState::unsupported_submission;
        ++epoch;raw=end;raw_done=false;credits=0;credit_deferred=false;awaiting_generated_submission=0;discard_pending();ready.clear();latest_native.reset();fg_active=pacing_active=false;changed.notify_all();
        // Unknown next chains cannot be copied generically. Their application
        // storage remains alive until the owning thread submits this frame.
        changed.wait(lock,[&]{return stopping||raw_done;});
        return stopping?XR_ERROR_SESSION_NOT_RUNNING:raw_result;
    }
    XrResult end_frame(const XrFrameEndInfo* end,const std::array<unsigned,2>&,
                       const XrCompositionLayerProjection* projection) noexcept override {
        if(bypass)return xr.end_frame(session,end);
        if(!end)return XR_ERROR_VALIDATION_FAILURE;
        ProducerElapsed elapsed{*this};
        std::uint64_t id{};bool use_raw{};
        {std::lock_guard lock(mx);if(stopping)return XR_ERROR_SESSION_NOT_RUNNING;
            if(!started||begun==ended)return XR_ERROR_CALL_ORDER_INVALID;++native_count;id=++next_id;use_raw=degraded;}
        if(use_raw){const auto result=passthrough(end);std::lock_guard lock(mx);++ended;changed.notify_all();return result;}
        {std::lock_guard capture_lock(capture_mutex);if(capture_overflow){
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
        if(!job){job=std::make_shared<Native>();std::lock_guard lock(mx);++skipped;}
        job->color={};job->color_ready={};job->auxiliary.clear();job->clear_guides();job->guides_valid=false;job->guide_token=0;job->render_frame=0;job->captured_at={};
        job->guide_state=CheekyFgState::waiting_for_guides;job->source_depth=job->source_motion=0;
        if(!projection||!job->layers.prepare(end,projection))result=passthrough(end);
        else {
            std::lock_guard capture_lock(capture_mutex);
            std::unordered_map<XrSwapchain,CapturePlan> next_plans;
            for(unsigned eye=0;eye<2;++eye){const auto& sub=projection->views[eye].subImage;
                auto& plan=next_plans[sub.swapchain];plan.desc=captured_descs[sub.swapchain];
                Slice slice{sub.imageRect,sub.imageArrayIndex};
                if(std::none_of(plan.slices.begin(),plan.slices.end(),[&](const auto& x){return same_slice(x,slice);}))plan.slices.push_back(slice);
            }
            subimages(job->layers,[&](XrSwapchainSubImage& sub){next_plans[sub.swapchain].full_required=true;return true;});
            capture_plans=std::move(next_plans);
            job->capture_mode=color_capture_mode.load();job->upload_mode=upload_mode.load();job->latency_diagnostics=latency_diagnostics!=0;
            auto* c=producer.begin(id,true);
            if(!c){std::lock_guard lock(mx);++skipped;++ended;changed.notify_all();return XR_SUCCESS;}
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
                        const auto& s=storage_options;const auto layout=compact_layout(sub.imageRect.extent.width,sub.imageRect.extent.height,
                            s.compact!=0,s.width,s.height,s.x,s.y,s.peripheral_percent,s.feather);
                        if(layout.packed()){
                            job->color[eye]=producer.packed_region(*c,found->second->texture.Get(),found->second->state,sub.imageRect,sub.imageArrayIndex,
                                found->second->texture->GetDesc().Format,layout);c->images.push_back(found->second);
                        }else job->color[eye]=producer.crop(*c,found->second,sub.imageRect,sub.imageArrayIndex);
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
                auto runtime=GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll");
                auto acquire=guide_acquire?guide_acquire:runtime?reinterpret_cast<CheekyFgAcquireFn>(GetProcAddress(runtime,"CheekyFG_AcquirePair")):nullptr;
                auto release=guide_release?guide_release:runtime?reinterpret_cast<CheekyFgReleaseFn>(GetProcAddress(runtime,"CheekyFG_ReleasePair")):nullptr;
                CheekyFgPair guides;guides.capture_token=expected_token;guides.render_frame=expected_frame;
                if(inference_slot&&(!tokens_match||!expected_token||!expected_frame))job->guide_state=CheekyFgState::guide_frame_mismatch;
                if(inference_slot&&tokens_match&&expected_token&&expected_frame&&acquire&&release&&acquire(&guides)) {
                    job->guides_valid=cheeky::foveated_dlss::native_d3d12_interface(guides.producer_queue)==
                        cheeky::foveated_dlss::native_d3d12_interface(queue.Get());
                    if(!job->guides_valid)job->guide_state=CheekyFgState::queue_mismatch;
                    if(guides.capture_token!=expected_token||guides.eyes[1].evaluation!=expected_token||
                       guides.eyes[0].evaluation+1!=expected_token||guides.eyes[0].render_frame!=expected_frame||
                       guides.eyes[1].render_frame!=expected_frame){job->guides_valid=false;job->guide_state=CheekyFgState::guide_frame_mismatch;}
                    for(unsigned eye=0;eye<2&&job->guides_valid;++eye) {
                        auto g=guides.eyes[eye];const auto d=job->color[eye]->texture->GetDesc();
                        if(!g.depth||!g.motion){job->guides_valid=false;job->guide_state=CheekyFgState::generation_failed;break;}
                        job->source_depth=unsigned(g.depth->GetDesc().Format);job->source_motion=unsigned(g.motion->GetDesc().Format);
                        const auto& layout=job->color[eye]->layout;
                        if(g.width!=(layout.width?layout.width:d.Width)||g.height!=(layout.height?layout.height:d.Height)){job->guides_valid=false;job->guide_state=CheekyFgState::generation_failed;break;}
                    }
                    if(job->guides_valid){
                        // The plugin has ALREADY captured immutable guides.
                        // Retain its lease through inference rather than
                        // converting another full guide set at handoff.
                        // Worker preparation now crops/normalizes once using
                        // the original offsets, scales and jitter correction.
                        job->retain_guides(guides);job->guide_token=expected_token;job->render_frame=expected_frame;
                    }
                    release(&guides);
                }else if(inference_slot&&tokens_match&&expected_token)job->guide_state=CheekyFgState::guide_frame_mismatch;
                {std::lock_guard lock(mx);telemetry.color_token=expected_token;telemetry.guide_token=job->guide_token;
                    telemetry.color_render_frame=expected_frame;telemetry.guide_render_frame=job->guides_valid?expected_frame:0;
                    if(inference_slot&&job->guide_state==CheekyFgState::guide_frame_mismatch)++telemetry.mismatched_guides;}
                good=producer.execute(*c,job->ready);
            }else if(c){
                // A changed layout can fail after one eye was recorded. Queue
                // those valid partial transitions so tracked pool states stay
                // consistent before falling back to the original native frame.
                std::uint64_t discarded{};producer.execute(*c,discarded);
            }
            if(!good)result=passthrough(end);
            else {
                job->space=projection->space;job->blend=end->environmentBlendMode;
                std::lock_guard lock(mx);job->id=id;job->epoch=epoch;job->handed_off_at=TimingClock::now();
                Pair native;native.upload_mode=job->upload_mode;native.native_id=native.current_id=id;native.ready=job->ready;
                native.current_color_ready=job->color_ready;
                native.native_captured=native.current_captured=job->captured_at;
                native.native=native.current=job->color;native.native_views=native.current_views=job->views;
                native.layers=job->layers;native.auxiliary=job->auxiliary;native.space=job->space;native.blend=job->blend;
                latest_native=std::move(native);
                if(inference_slot){pending.push_back(job);while(pending.size()>2){pending.front()->clear_guides();pending.pop_front();++skipped;}}
                ++frames;held_valid=true;changed.notify_all();
            }
        }
        {std::lock_guard lock(mx);++ended;changed.notify_all();}
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
            pair.current_color_ready=job->color_ready;
            pair.native_captured=pair.current_captured=job->captured_at;
            pair.handed_off_at=job->handed_off_at;pair.worker_started_at=TimingClock::now();
            pair.native_views=pair.current_views=job->views;pair.layers=job->layers;pair.auxiliary=job->auxiliary;
            pair.space=job->space;pair.blend=job->blend;pair.ready=job->ready;
            bool success{};
            bool attempted{};
            if(previous&&previous->epoch==job->epoch&&previous->id+1==job->id&&previous->space==job->space&&job->guides_valid&&
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
                    const auto ready_result=worker.latency.enqueue_wait(device.Get(),generation_queue.Get(),producer.fence.Get(),job->ready,job->latency_diagnostics);
                    if(FAILED(ready_result)){worker.failed=true;worker.report("FG capture dependency queue wait",unsigned(ready_result));}
                    if(FAILED(ready_result))throw std::runtime_error("FG capture dependency queue wait failed");
                    auto guides=job->guides;guides.producer_queue=generation_queue.Get();
                    if(previous->guide_token+2!=job->guide_token)reset_history=true;
                    if(previous->render_frame+1!=job->render_frame)reset_history=true;
                    if(reset_history)for(auto& eye:guides.eyes)eye.reset=1;
                    std::array<Camera,2> cameras;for(unsigned eye=0;eye<2;++eye)
                        cameras[eye]=camera(job->views[eye],&previous->views[eye],guides.eyes[eye].depth_inverted!=0);
                    std::array<ID3D12Resource*,2> input{job->color[0]->texture.Get(),job->color[1]->texture.Get()},
                        prior{previous->color[0]->texture.Get(),previous->color[1]->texture.Get()},result{};
                    auto publication=d;publication.Format=worker.focus.color_format;
                    std::array<Worker::OutputCopy,2> copies;
                    bool available=true;
                    for(unsigned eye=0;eye<2;++eye) {
                        const auto& g=guides.eyes[eye];
                        const auto layout=compact_layout(worker.width,worker.height,job->color[eye]->layout.packed(),g.focus_width,g.focus_height,
                            g.focus_x,g.focus_y,g.peripheral_percent,g.focus_feather,g.resolution_percent);
                        publication.Width=layout.stored_width();publication.Height=layout.stored_height();
                        pair.generated[eye]=output.image(publication,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                        if(!pair.generated[eye]){available=false;break;}
                        pair.generated[eye]->layout=layout;
                        copies[eye]={pair.generated[eye]->texture.Get(),pair.generated[eye]->state,layout};
                    }
                    if(available) {
                        success=worker.generate(guides,input,prior,cameras,result,&copies,[](void* context) noexcept {
                            static_cast<BufferedPresenter*>(context)->inference_submitted();
                        },this,[](void* context) noexcept {
                            static_cast<BufferedPresenter*>(context)->inference_prepared();
                        });attempted=!worker.failed;
                        pair.submitted_at=worker.submitted_at;
                        for(unsigned eye=0;eye<2;++eye) {
                            pair.generated[eye]->state=copies[eye].state;
                            pair.generated[eye]->presentation_format=srgb_color(d.Format)&&publication.Format==DXGI_FORMAT_R8G8B8A8_UNORM?
                                DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:publication.Format;
                        }
                    }
                }
            }
            if(success){pair.interpolated=true;pair.native_id=previous->id;pair.native=previous->color;
                pair.native_captured=previous->captured_at;
                pair.native_views=previous->views;pair.generated_views=midpoint(previous->views,job->views);
                pair.completed_at=TimingClock::now();}
            reset_history=!attempted;if(job->latency_diagnostics)collect_color_stamps();
            measure_worker_memory();
            {std::lock_guard lock(mx);if(stopping)break;
                // Previous-frame history needs colors/poses, not its original
                // guides. Release completed leases now instead of pinning raw
                // textures in reusable slots or temporal history indefinitely.
                if(!worker.fence||worker.fence->GetCompletedValue()>=worker.serial)job->clear_guides();
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
                ready.push_back(std::move(pair));while(ready.size()>2)ready.pop_front();
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
        if(chain.handle&&(chain.desc.Width!=d.Width||chain.desc.Height!=d.Height||chain.desc.Format!=d.Format||chain.desc.DepthOrArraySize!=d.DepthOrArraySize)) {
            uploads.drain();xr.destroy(chain.handle);chain={};
        }
        if(chain.handle)return true;
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};create.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        create.format=d.Format;create.width=unsigned(d.Width);create.height=d.Height;create.arraySize=d.DepthOrArraySize;
        create.sampleCount=create.faceCount=create.mipCount=1;
        if(XR_FAILED(xr.create(session,&create,&chain.handle)))return false;
        unsigned count{};if(XR_FAILED(xr.enumerate(chain.handle,0,&count,nullptr))||!count)return false;
        chain.images.resize(count);for(auto& i:chain.images)i={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
        if(XR_FAILED(xr.enumerate(chain.handle,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(chain.images.data()))))return false;
        chain.desc=d;return true;
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
        const bool copied=XR_SUCCEEDED(wait_result)&&index<chain.images.size()&&
            uploads.upload(source,chain.images[index].texture,ticks,chain.desc.Format);
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const bool released=XR_SUCCEEDED(xr.release(chain.handle,&release));
        if(released&&copied)chain.uploaded=source->identity;
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
                for(auto& item:items)if(!uploads.record_upload(*c,item.source,item.chain->images[item.index].texture,item.chain->desc.Format)){good=false;break;}
                if(good){std::uint64_t ticket{};good=uploads.execute(*c,ticket);}
                else c->list->Close();
            }
        }
        const bool submitted=good;
        // All copies MUST be queued before any image is released: the runtime
        // may insert its own fence at ReleaseSwapchainImage.
        for(auto& item:items)if(item.acquired&&item.waited){
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            const bool released=XR_SUCCEEDED(xr.release(item.chain->handle,&release));
            if(submitted&&released)item.chain->uploaded=item.source->identity;
            good&=released;
        }
        return good;
    }
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
                if(!credit_deferred)++deferred_credits;credit_deferred=true;
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
                if(now>=next) {
                    const double display_ms=telemetry.xr_period_ms>0?telemetry.xr_period_ms:1000./120.;
                    const auto period=std::chrono::duration_cast<TimingClock::duration>(
                        std::chrono::duration<double,std::milli>(std::max(1.,display_ms*(pacing_active?2.:1.))));
                    if(!raw) {
                        if(release_gate_active()||(pacing_active&&unsubmitted_native())) {
                            if(!credit_deferred)++deferred_credits;credit_deferred=true;
                        }else if(credit_deferred)release_deferred_credit();
                        else credits=std::min(2u,credits+1);
                        changed.notify_all();
                    }
                    next+=period;if(next<=now)next=now+period;
                }
                changed.wait_until(lock,next);
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
        if(inflight_id&&(inflight_id>submitted_native_id||inflight_release_point>=2u||
            (inflight_release_point==1u&&!inflight_prepared)))return true;
        return std::any_of(pending.begin(),pending.end(),[&](const auto& job){
            return job->guides_valid&&(job->guides.release_point!=0||job->id>submitted_native_id);
        });
    }
    bool generated_submission_pending() const noexcept {
        return awaiting_generated_submission||(inflight_id&&inflight_release_point==3u)||
            std::any_of(pending.begin(),pending.end(),[](const auto& job){return job->guides_valid&&job->guides.release_point==3u;});
    }
    void release_deferred_credit() noexcept {
        // Hold mx. Coalesce missed ticks rather than bursting queued renders.
        if(credit_deferred&&!stopping&&!raw&&!unsubmitted_native()&&!release_gate_active()) {
            credits=std::min(2u,credits+1);credit_deferred=false;++released_deferred_credits;changed.notify_all();
        }
    }
    void inference_submitted() noexcept {
        std::lock_guard lock(mx);submitted_native_id=std::max(submitted_native_id,inflight_id);
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
            state!=CheekyFgState::generating||completed_pair_id>baseline;});
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
        if(generated&&pair.current_color_ready[0])collect_color_stamps();
        auto views=original_views;
        std::vector<std::pair<Chain*,Image>> batch;
        for(unsigned eye=0;eye<2;++eye) {
            auto& chain=scene_chains[eye];
            if(pair.upload_mode==1u){if(!ensure_chain(chain,images[eye],true))return false;batch.push_back({&chain,images[eye]});}
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
        result=runtime_end_frame(&end);
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
        while(!stopping&&!raw&&!submit_repeat_frames&&!fresh_content(active,have_active,generated_phase,position)) {
            // FG completion and new captures notify changed. Poll only an
            // outstanding producer fence (native-only startup/fallback), not
            // a quiet mailbox: idle fresh-only mode must not spin at 1 kHz.
            const bool producer_pending=(!fg_active&&latest_native&&!producer.completed(latest_native->ready))||
                std::any_of(ready.begin(),ready.end(),[&](const auto& pair){return !producer.completed(pair.ready);});
            if(producer_pending)changed.wait_for(lock,std::chrono::milliseconds(1));
            else changed.wait(lock);
        }
        allow_repeats=submit_repeat_frames;return !stopping;
    }
    void presentation_loop() noexcept {
        SetThreadDescription(GetCurrentThread(),L"Cheeky OpenXR presentation");
        Pair active;bool have_active{},generated_phase{};double last_position{};
        try {for(;;) {
            bool allow_repeats{};
            if(!await_content(active,have_active,generated_phase,last_position,allow_repeats))break;
            XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};XrFrameState frame{XR_TYPE_FRAME_STATE};
            const auto start=TimingClock::now();auto result=xr.wait_frame(session,&wait,&frame);const auto waited_ms=elapsed_ms(start);
            if(XR_FAILED(result)){std::lock_guard lock(mx);terminal=result;stopping=true;changed.notify_all();break;}
            const auto present_start=TimingClock::now();
            XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};result=xr.begin_frame(session,&begin);
            {std::lock_guard lock(mx);runtime_begin.add(elapsed_ms(present_start));}
            if(XR_FAILED(result)){std::lock_guard lock(mx);terminal=result;stopping=true;changed.notify_all();break;}
            const XrFrameEndInfo* raw_frame{};bool stop_now{};
            {std::lock_guard lock(mx);raw_frame=raw;stop_now=stopping;
                if(awaiting_generated_submission&&(last_position>=double(awaiting_generated_submission)||frame.shouldRender!=XR_TRUE))
                    finish_generated_gate(awaiting_generated_submission,false);
                predicted=frame;telemetry.xr_period_ms=double(frame.predictedDisplayPeriod)/1000000.;
                xr_wait.add(waited_ms);telemetry.xr_wait_ms=xr_wait.mean();++ticks;changed.notify_all();
            }
            if(allow_repeats&&!generated_phase&&frame.shouldRender==XR_TRUE)wait_for_new_pair(present_start,frame.predictedDisplayPeriod,last_position);
            {std::lock_guard lock(mx);raw_frame=raw;stop_now=stopping;
                if(!generated_phase&&!raw_frame&&!stop_now&&!degraded) {
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
            if(raw_frame&&!stop_now) {
                auto end=*raw_frame;end.displayTime=frame.predictedDisplayTime;
                result=runtime_end_frame(&end);submitted=closed=true;
                {std::lock_guard lock(mx);raw_result=result;raw=nullptr;raw_done=true;have_active=false;
                    generated_phase=false;last_position=0;changed.notify_all();}
                wake_producer();
            }else if(have_active&&!stop_now&&frame.shouldRender==XR_TRUE) {
                is_generated=generated_phase&&active.interpolated;
                const auto& images=is_generated?active.generated:active.native;
                const auto& views=is_generated?active.generated_views:active.native_views;
                repeated=!is_generated&&double(active.native_id)==last_position;
                submitted=render(active,images,views,is_generated,repeated,frame.predictedDisplayTime,result,closed);
                if(submitted) {
                    last_position=double(active.native_id)+(is_generated?.5:0.);
                    if(is_generated){active.native=active.current;active.native_views=active.current_views;active.native_id=active.current_id;
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
            measure_presenter_memory();
            char age_log[512]{},stage_log[512]{},cadence_log[512]{},gpu_age_log[512]{};
            {std::lock_guard lock(mx);
                if(is_generated&&submitted)++generated_count;
                if(submitted&&!is_generated&&!raw_frame){if(repeated)++repeated_presentations;else ++distinct_native;}
                present_cpu.add(elapsed_ms(present_start));
                if(XR_SUCCEEDED(result)){++runtime_submissions;
                    const auto now_ms=std::chrono::duration<double,std::milli>(TimingClock::now().time_since_epoch()).count();
                    cadence_window.add(now_ms,{native_count,distinct_native,generated_count,repeated_presentations,runtime_submissions});}
                if(trace&&is_generated&&submitted&&(generated_count%120)==0) {
                    const auto c=cadence();
                    sprintf_s(cadence_log,"FG experiment cadence: release=%s repeats=%s queue=%s clock=independent valid=%u game=%.3f native=%.3f generated=%.3f unique=%.3f repeated=%.3f submissions=%.3f; xr_wait_ms=%.3f xr_end_ms=%.3f present_ms=%.3f",
                        active_release_point==3u?"generated-submission":active_release_point==2u?"completion":active_release_point==1u?"preparation":"submission",
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
                    sprintf_s(gpu_age_log,"FG GPU-ready latency ms: samples=%llu native_tail_excluded=%.3f generated_submit=%.3f predicted_display=%.3f predicted_samples=%llu late_targets=%llu gate_released=%llu gate_skipped=%llu",
                        gpu_ready_samples,gpu_ready_samples?gpu_ready_ages[0].mean():-1.,gpu_ready_samples?gpu_ready_ages[1].mean():-1.,
                        predicted_samples?gpu_ready_ages[2].mean():-1.,predicted_samples,prediction_late,
                        generated_gate_releases,generated_gate_skips);
                }
                if(have_active&&!submitted&&!stop_now&&frame.shouldRender==XR_TRUE){++fallback_count;state=CheekyFgState::presentation_failed;
                    degraded=true;fg_active=pacing_active=false;awaiting_generated_submission=0;discard_pending();ready.clear();latest_native.reset();have_active=false;generated_phase=false;++epoch;
                    strcpy_s(diagnostic,"Private presentation failed; using coordinated original native submissions");}
                // Bounded semaphore-equivalent credits. Two prime credits let
                // the game overlap production with prior FG/compositor work.
                // GPU copies have been queued before the next game is woken.
                changed.notify_all();
                if(XR_FAILED(result)){terminal=result;stopping=true;changed.notify_all();break;}
            }
            if(age_log[0])trace(age_log);
            if(stage_log[0])trace(stage_log);
            if(cadence_log[0])trace(cadence_log);
            if(gpu_age_log[0])trace(gpu_age_log);
        }}catch(...) {std::lock_guard lock(mx);terminal=XR_ERROR_RUNTIME_FAILURE;stopping=true;changed.notify_all();}
    }
};
}
