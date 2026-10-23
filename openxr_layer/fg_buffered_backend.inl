// Included after BufferedPresenter is complete.
namespace cheeky::fg {
// These host-bound adapters leave the existing capture hooks and diagnostic
// fixtures intact while giving all three pacing threads one backend contract.
// Backend selection happens before any pacing thread is started.

inline std::unique_ptr<FrameBackend> make_frame_backend12(BufferedPresenter& host){return std::make_unique<BufferedBackend12>(host);}
inline std::unique_ptr<FrameBackend> make_frame_backend11(BufferedPresenter& host){return std::make_unique<BufferedBackend11>(host);}
inline BufferedPresenter::BufferedPresenter():backend(make_frame_backend12(*this)){}

inline CheekyFgOverhead BufferedPresenter::overhead() const noexcept {
    auto result=extra_gpu;result.supported=1;
    if(native11){
        const auto capture=native11->capture_timings.summary(),guide=native11->guide_timings.summary(),upload=native11->upload_timings.summary();
        result.capture_samples=capture.samples;result.capture_ms=capture.mean;result.capture_last_ms=capture.last;
        result.guide_samples=guide.samples;result.guide_ms=guide.mean;result.guide_last_ms=guide.last;
        result.upload_samples=upload.samples;result.upload_ms=upload.mean;result.upload_last_ms=upload.last;
        return result;
    }
    const auto capture=producer.gpu_cost.summary(),upload=uploads.gpu_cost.summary();
    result.capture_samples=capture.samples;result.capture_ms=capture.mean;result.capture_last_ms=capture.last;
    result.upload_samples=upload.samples;result.upload_ms=upload.mean;result.upload_last_ms=upload.last;
    return result;
}

inline CheekyFgExperimentDiagnostics BufferedPresenter::experiments() const noexcept {
    auto out=experiment_state;out.supported=1;
    if(out.latency_enabled!=latency_diagnostics)out.latency_valid=0;
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

inline void BufferedPresenter::destroy_private_chain(XrSwapchain chain) {
    if(native11&&retire_chain)retire_chain(chain);else destroy_chain(chain);
}

inline void BufferedPresenter::destroy_chains() noexcept {
    if(native11)native11->flush(); // As D3D11Bridge::forget does via drain().
    for(auto& c:scene_chains){if(c.handle)destroy_private_chain(c.handle);c={};}
    for(auto& [key,c]:auxiliary_chains)if(c.handle)destroy_private_chain(c.handle);
    auxiliary_chains.clear();
}

inline void BufferedPresenter::reset() noexcept {
    // Stop/join both workers and drain their GPU fences before dropping the
    // dedicated queue. A temporal reset only stops the workers; it keeps
    // the queue available for the next interpolation session.
    stop();destroy_chains();Presenter::reset();
    generation_queue.Reset();fg_high_queue.Reset();fg_normal_queue.Reset();generation_high_priority=false;
    last_fg_priority_requested=UINT32_MAX;
}

inline std::recursive_mutex* BufferedPresenter::dispatch_mutex11() const noexcept {
    return native11?native11->dispatch:Presenter::dispatch_mutex11();
}

inline BufferedPresenter::ProducerElapsed::~ProducerElapsed() {std::lock_guard capture_lock(owner.capture_mutex);std::lock_guard lock(owner.mx);
            owner.producer_cpu.add(elapsed_ms(begin)+owner.capture_cpu_accumulator);owner.capture_cpu_accumulator=0;
            owner.release_idle_slots();
            const auto now=GetTickCount64();if(now>=owner.next_capture_memory){
                fg_memory::Collector collect(owner.device.Get());
                owner.memory_state.categories[fg_memory::capture]=owner.producer.memory(collect);
                owner.memory_state.capture_tick=now;owner.next_capture_memory=now+1000;
            }}

inline void BufferedPresenter::reset_gpu_metrics() {
    producer.gpu_cost.reset();uploads.gpu_cost.reset();
    guide_sync_fence.Reset();guide_sync_serial=0;guide_sync_reported=false;
}
}
