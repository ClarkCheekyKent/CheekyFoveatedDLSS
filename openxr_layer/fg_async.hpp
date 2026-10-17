#pragma once
#include "fg_transport.hpp"
#include "../src/d3d12_native.hpp"
#include <condition_variable>
#include <thread>
#include <atomic>
#include <optional>

namespace cheeky::fg {
// The game owns capture/snapshot recording. Only the presentation thread calls
// the runtime frame functions. A separate worker waits for inference completion.
// Three native leases and two ready pairs bound latency and texture ownership.
struct BufferedPresenter final : Presenter {
    struct Auxiliary {XrSwapchain source{};Image image;};
    struct Native {
        std::uint64_t id{},ready{},epoch{};
        std::array<Image,2> color;
        std::array<XrCompositionLayerProjectionView,2> views;
        XrSpace space{};XrEnvironmentBlendMode blend{};
        FrameLayers layers;
        std::vector<Auxiliary> auxiliary;
        FocusPipeline snapshot;
        CheekyFgPair guides;
        bool guides_valid{};
        CheekyFgState guide_state{CheekyFgState::waiting_for_guides};
        unsigned source_depth{},source_motion{};
        std::uint64_t guide_token{};
        std::uint64_t render_frame{};
    };
    struct Pair {
        std::uint64_t native_id{},current_id{},ready{};
        std::array<Image,2> native,generated,current;
        std::array<XrCompositionLayerProjectionView,2> native_views,generated_views,current_views;
        FrameLayers layers;
        std::vector<Auxiliary> auxiliary;
        XrSpace space{};XrEnvironmentBlendMode blend{};
        bool interpolated{};
    };
    struct Chain {
        XrSwapchain handle{};std::vector<XrSwapchainImageD3D12KHR> images;D3D12_RESOURCE_DESC desc{};
        Image uploaded; // Pins immutable content until this chain is updated/destroyed.
    };
    std::condition_variable_any changed;
    std::mutex capture_mutex;
    std::thread presentation_thread,generation_thread;
    Transport producer,output,uploads;
    FocusPipeline snapshot_programs;
    ComPtr<ID3D12CommandQueue> generation_queue;
    std::unordered_map<XrSwapchain,Image> captured;
    std::unordered_map<XrSwapchain,std::uint64_t> capture_tokens;
    std::unordered_map<XrSwapchain,std::uint64_t> capture_frames;
    bool capture_overflow{};
    std::array<std::shared_ptr<Native>,3> slots;
    std::deque<std::shared_ptr<Native>> pending;
    std::deque<Pair> ready;
    std::optional<Pair> latest_native;
    std::unordered_map<XrSwapchain,Chain> auxiliary_chains;
    Chain scene_chains[2][2];
    CheekyFgPerformance telemetry;
    TimingAverage producer_cpu,pacing_wait;
    TimingAverage app_image_wait,present_image_wait,runtime_begin,runtime_end,present_cpu;
    std::array<TimingAverage,3> app_calls;
    CadenceWindow cadence_window;
    std::uint64_t distinct_native{},repeated_presentations{},runtime_submissions{};
    double capture_cpu_accumulator{};
    struct ProducerElapsed {
        BufferedPresenter& owner;TimingClock::time_point begin{TimingClock::now()};
        ~ProducerElapsed(){std::lock_guard capture_lock(owner.capture_mutex);std::lock_guard lock(owner.mx);
            owner.producer_cpu.add(elapsed_ms(begin)+owner.capture_cpu_accumulator);owner.capture_cpu_accumulator=0;}
    };
    char diagnostic[384]{"Buffered presenter waiting for stereo frames"};
    bool started{},stopping{},bypass{},fg_active{},pacing_active{},degraded{};
    unsigned credits{},app_waiters{};
    std::uint64_t next_id{},waited{},begun{},ended{},ticks{},skipped{},epoch{},transition_until{};
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
        // Match the reference: CPU completion is asynchronous, but FG GPU work
        // uses the game's queue. A competing direct queue adds scheduling jitter
        // inside the FG timestamp range and races the next game render for time.
        generation_queue=q;
        return producer.initialize(dev,q)&&output.initialize(dev,q)&&uploads.initialize(dev,q)&&snapshot_programs.initialize(dev);
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
    void note_app_image_wait(double ms) noexcept override {std::lock_guard lock(mx);app_image_wait.add(ms);}
    void note_app_call(unsigned stage,double ms) noexcept override {std::lock_guard lock(mx);if(stage<app_calls.size())app_calls[stage].add(ms);}
    XrResult runtime_end_frame(const XrFrameEndInfo* end) {
        const auto start=TimingClock::now();const auto result=xr.end_frame(session,end);
        std::lock_guard lock(mx);runtime_end.add(elapsed_ms(start));return result;
    }
    const char* diagnostic_text() const noexcept override {return diagnostic;}
    void stop() noexcept {
        {std::lock_guard lock(mx);stopping=true;changed.notify_all();}
        if(presentation_thread.joinable())presentation_thread.join();
        if(generation_thread.joinable())generation_thread.join();
        std::lock_guard capture_lock(capture_mutex);
        producer.drain();output.drain();uploads.drain();worker.wait_idle();
        std::lock_guard lock(mx);pending.clear();ready.clear();latest_native.reset();slots={};
        captured.clear();capture_tokens.clear();capture_frames.clear();capture_overflow=false;capture_cpu_accumulator=0;started=false;credits=0;fg_active=pacing_active=false;raw=nullptr;raw_done=true;
        waited=begun=ended=0;ticks=transition_until=0;degraded=false;app_display_time=0;predicted={XR_TYPE_FRAME_STATE};++epoch;
        app_image_wait={};present_image_wait={};runtime_begin={};runtime_end={};present_cpu={};app_calls={};cadence_window={};
        distinct_native=repeated_presentations=runtime_submissions=0;
    }
    void destroy_chains() noexcept {
        for(auto& mode:scene_chains)for(auto& c:mode){if(c.handle)xr.destroy(c.handle);c={};}
        for(auto& [key,c]:auxiliary_chains)if(c.handle)xr.destroy(c.handle);
        auxiliary_chains.clear();
    }
    void reset() noexcept override {
        stop();destroy_chains();Presenter::reset();
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
            started=true;
            generation_thread=std::thread([this]{generation_loop();});
            presentation_thread=std::thread([this]{presentation_loop();});
        } catch(...) {stopping=true;changed.notify_all();return false;}
        return true;
    }
    XrResult wait_frame(const XrFrameWaitInfo* info,XrFrameState* frame) noexcept override {
        if(bypass)return xr.wait_frame(session,info,frame);
        if(!frame||(info&&info->next))return XR_ERROR_VALIDATION_FAILURE;
        std::unique_lock lock(mx);
        if(!start())return XR_ERROR_RUNTIME_FAILURE;
        const auto wait_begin=TimingClock::now();
        ++app_waiters;
        changed.wait(lock,[&]{return stopping||XR_FAILED(terminal)||(credits&&waited==begun&&!raw);});
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
        changed.wait(lock,[&]{return stopping||begun==ended;});
        if(stopping)return XR_ERROR_SESSION_NOT_RUNNING;
        ++begun;changed.notify_all();return XR_SUCCESS;
    }
    bool capture(XrSwapchain sc,unsigned,ID3D12Resource* original,unsigned,unsigned,unsigned,
                 DXGI_FORMAT declared=DXGI_FORMAT_UNKNOWN) noexcept override {
        if(bypass||!original)return false;
        std::lock_guard capture_lock(capture_mutex);
        {std::lock_guard lock(mx);if(stopping||degraded)return false;}
        AddElapsed elapsed{capture_cpu_accumulator};
        auto runtime=GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll");
        const auto stamp_fn=guide_stamp?guide_stamp:runtime?reinterpret_cast<CheekyFgCaptureStampFn>(GetProcAddress(runtime,"CheekyFG_GetCaptureStamp")):nullptr;
        CheekyFgCaptureStamp stamp;if(stamp_fn)stamp_fn(original,&stamp);
        // The legacy test callback has no production fallback: real generation
        // requires the RHI frame identity returned with the color stamp.
        if(guide_token&&!stamp_fn){stamp.token=guide_token(original);stamp.render_frame=stamp.token/2;}
        const auto token=stamp.token;
        const auto format=capture_format(original->GetDesc().Format,declared);if(format==DXGI_FORMAT_UNKNOWN)return false;
        auto* c=producer.begin();if(!c){capture_overflow=true;return false;}
        auto copy=producer.copy(*c,original,D3D12_RESOURCE_STATE_RENDER_TARGET,format);
        if(!copy){capture_overflow=true;c->list->Close();return false;}
        std::uint64_t ticket{};if(!producer.execute(*c,ticket))return false;
        captured[sc]=std::move(copy);capture_tokens[sc]=token;capture_frames[sc]=stamp.render_frame;return true;
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
        ++epoch;raw=end;raw_done=false;credits=0;pending.clear();ready.clear();latest_native.reset();fg_active=pacing_active=false;changed.notify_all();
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
                if(!slot){slot=std::make_shared<Native>();auto& s=slot->snapshot;
                    s.root=snapshot_programs.root;s.prepare=snapshot_programs.prepare;
                    s.peripheral=snapshot_programs.peripheral;s.compose=snapshot_programs.compose;}
                if(slot.use_count()==1&&producer.completed(slot->ready)){job=slot;break;}
            }
        }
        const bool inference_slot=bool(job);
        if(!job){job=std::make_shared<Native>();std::lock_guard lock(mx);++skipped;}
        job->color={};job->auxiliary.clear();job->guides={};job->guides_valid=false;job->guide_token=0;job->render_frame=0;
        job->guide_state=CheekyFgState::waiting_for_guides;job->source_depth=job->source_motion=0;
        if(!projection||!job->layers.prepare(end,projection))result=passthrough(end);
        else {
            std::lock_guard capture_lock(capture_mutex);
            auto* c=producer.begin();
            if(!c){std::lock_guard lock(mx);++skipped;++ended;changed.notify_all();return XR_SUCCESS;}
            bool good=true;std::uint64_t expected_token{},expected_frame{};bool tokens_match=true;
            if(good)for(unsigned eye=0;eye<2;++eye) {
                const auto& sub=projection->views[eye].subImage;
                const auto found=captured.find(sub.swapchain);
                job->color[eye]=found==captured.end()?Image{}:producer.crop(*c,found->second,sub.imageRect,sub.imageArrayIndex);
                if(!job->color[eye]){good=false;break;}
                const auto token=capture_tokens[sub.swapchain];
                const auto frame=capture_frames[sub.swapchain];
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
                    if(guides.lease)c->leases.emplace_back(guides.lease);
                    for(unsigned eye=0;eye<2&&job->guides_valid;++eye) {
                        auto g=guides.eyes[eye];const auto d=job->color[eye]->texture->GetDesc();
                        if(!g.depth||!g.motion){job->guides_valid=false;job->guide_state=CheekyFgState::generation_failed;break;}
                        job->source_depth=unsigned(g.depth->GetDesc().Format);job->source_motion=unsigned(g.motion->GetDesc().Format);
                        if(g.width!=d.Width||g.height!=d.Height){job->guides_valid=false;job->guide_state=CheekyFgState::generation_failed;break;}
                        // Normalize immutable evaluation-time snapshots into
                        // UV guides owned by this native transport lease.
                        g.peripheral_method=2;FocusRect full{0,0,unsigned(d.Width),d.Height,0,false};
                        job->guides_valid=job->snapshot.prepare_inputs(device.Get(),c->list.Get(),eye,1,
                            job->color[eye]->texture.Get(),g,full,unsigned(d.Width),d.Height);
                        if(!job->guides_valid){job->guide_state=CheekyFgState::generation_failed;break;}
                        auto& owned=job->snapshot.inputs[eye][1];auto& meta=job->guides.eyes[eye];meta=guides.eyes[eye];
                        const auto guide=owned.depth->GetDesc();
                        meta.jitter_x*=float(guide.Width)/std::max(1u,meta.depth_width);
                        meta.jitter_y*=float(guide.Height)/std::max(1u,meta.depth_height);
                        meta.depth=owned.depth.Get();meta.motion=owned.motion.Get();
                        meta.depth_width=meta.motion_width=unsigned(guide.Width);meta.depth_height=meta.motion_height=guide.Height;
                        meta.depth_base_x=meta.depth_base_y=meta.motion_base_x=meta.motion_base_y=0;
                        meta.motion_scale_x=float(guide.Width);meta.motion_scale_y=float(guide.Height);
                        meta.jitter_correction_x=meta.jitter_correction_y=0;meta.motion_jittered=0;
                        c->borrowed.emplace_back(g.depth);c->borrowed.emplace_back(g.motion);
                    }
                    if(job->guides_valid){job->guide_token=expected_token;job->render_frame=expected_frame;}
                    release(&guides);
                }else if(inference_slot&&tokens_match&&expected_token)job->guide_state=CheekyFgState::guide_frame_mismatch;
                {std::lock_guard lock(mx);telemetry.color_token=expected_token;telemetry.guide_token=job->guide_token;
                    telemetry.color_render_frame=expected_frame;telemetry.guide_render_frame=job->guides_valid?expected_frame:0;
                    if(inference_slot&&job->guide_state==CheekyFgState::guide_frame_mismatch)++telemetry.mismatched_guides;}
                good=producer.execute(*c,job->ready);
            }else if(c)c->list->Close();
            if(!good)result=passthrough(end);
            else {
                job->space=projection->space;job->blend=end->environmentBlendMode;
                std::lock_guard lock(mx);job->id=id;job->epoch=epoch;
                Pair native;native.native_id=native.current_id=id;native.ready=job->ready;
                native.native=native.current=job->color;native.native_views=native.current_views=job->views;
                native.layers=job->layers;native.auxiliary=job->auxiliary;native.space=job->space;native.blend=job->blend;
                latest_native=std::move(native);
                if(inference_slot){pending.push_back(job);while(pending.size()>2){pending.pop_front();++skipped;}}
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
        std::shared_ptr<Native> previous;bool reset_history=true;
        try {for(;;) {
            std::shared_ptr<Native> job;
            {std::unique_lock lock(mx);changed.wait(lock,[&]{return stopping||!pending.empty();});if(stopping)break;
             job=pending.back();skipped+=pending.size()-1;pending.clear();}
            Pair pair;pair.native_id=pair.current_id=job->id;pair.native=pair.current=job->color;
            pair.native_views=pair.current_views=job->views;pair.layers=job->layers;pair.auxiliary=job->auxiliary;
            pair.space=job->space;pair.blend=job->blend;pair.ready=job->ready;
            bool success{};
            bool attempted{};
            if(previous&&previous->epoch==job->epoch&&previous->id+1==job->id&&previous->space==job->space&&job->guides_valid&&
               previous->color[0]->texture->GetDesc().Width==job->color[0]->texture->GetDesc().Width&&
               previous->color[0]->texture->GetDesc().Height==job->color[0]->texture->GetDesc().Height) {
                generation_queue->Wait(producer.fence.Get(),job->ready);
                auto d=job->color[0]->texture->GetDesc();worker.trace=trace;worker.input_color_state=D3D12_RESOURCE_STATE_GENERIC_READ;
                if(worker.initialize(device.Get(),generation_queue.Get(),unsigned(d.Width),d.Height,d.Format)) {
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
                        pair.generated[eye]=output.image(publication);
                        if(!pair.generated[eye]){available=false;break;}
                        copies[eye]={pair.generated[eye]->texture.Get(),pair.generated[eye]->state};
                    }
                    if(available) {
                        success=worker.generate(guides,input,prior,cameras,result,&copies);attempted=!worker.failed;
                        for(unsigned eye=0;eye<2;++eye) {
                            pair.generated[eye]->state=copies[eye].state;
                            pair.generated[eye]->presentation_format=srgb_color(d.Format)&&publication.Format==DXGI_FORMAT_R8G8B8A8_UNORM?
                                DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:publication.Format;
                        }
                    }
                }
            }
            if(success){pair.interpolated=true;pair.native_id=previous->id;pair.native=previous->color;
                pair.native_views=previous->views;pair.generated_views=midpoint(previous->views,job->views);}
            reset_history=!attempted;
            {std::lock_guard lock(mx);if(stopping)break;
                if(job->epoch!=epoch||degraded){previous.reset();reset_history=true;continue;}
                ready.push_back(std::move(pair));while(ready.size()>2)ready.pop_front();
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
                telemetry.inference_color_format=unsigned(worker.focus.color_format);telemetry.shared_gpu_queue=1;
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
            pending.clear();ready.clear();++epoch;strcpy_s(diagnostic,"FG background processing failed; using original native submissions");}
        worker.wait_idle();output.drain();
    }
    bool ensure_chain(Chain& chain,const Image& image) {
        auto d=image->texture->GetDesc();
        if(image->presentation_format!=DXGI_FORMAT_UNKNOWN)d.Format=image->presentation_format;
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
    bool upload_image(Chain& chain,const Image& source) {
        if(!ensure_chain(chain,source))return false;
        // xrEndFrame uses the most recently released image. Resubmitting that
        // image needs no acquire/copy/release when its immutable content has
        // not changed. Pinning the source prevents transport-pool reuse from
        // making pointer equality accidentally identify newer pixels.
        if(chain.uploaded==source)return true;
        unsigned index{};XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if(XR_FAILED(xr.acquire(chain.handle,&acquire,&index)))return false;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
        const auto wait_begin=TimingClock::now();const auto wait_result=xr.wait_image(chain.handle,&wait);
        {std::lock_guard lock(mx);present_image_wait.add(elapsed_ms(wait_begin));}
        const bool copied=XR_SUCCEEDED(wait_result)&&index<chain.images.size()&&
            uploads.upload(source,chain.images[index].texture);
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const bool released=XR_SUCCEEDED(xr.release(chain.handle,&release));
        if(released&&copied)chain.uploaded=source;
        return released&&copied;
    }
    void wake_producer() {
        std::lock_guard lock(mx);
        if(!stopping&&!raw&&(!pacing_active||(ticks%2)==0))credits=std::min(2u,credits+(ticks==1?2u:1u));
        changed.notify_all();
    }
    bool render(Pair& pair,const std::array<Image,2>& images,
                const std::array<XrCompositionLayerProjectionView,2>& original_views,bool generated,XrTime time,
                XrResult& result,bool& closed) {
        // An interpolated pair is published only after the worker's single
        // inference + immutable-copy fence completes. Native pairs retain the
        // producer fence; both paths therefore refer to the original job here.
        if(!producer.completed(pair.ready))return false;
        auto views=original_views;
        for(unsigned eye=0;eye<2;++eye) {
            auto& chain=scene_chains[generated?1:0][eye];if(!upload_image(chain,images[eye]))return false;
            views[eye].type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;views[eye].next=nullptr;
            const auto d=images[eye]->texture->GetDesc();views[eye].subImage={chain.handle,{{0,0},{int(d.Width),int(d.Height)}},0};
        }
        auto layers=pair.layers;
        for(auto& a:pair.auxiliary)if(!upload_image(auxiliary_chains[a.source],a.image))return false;
        if(!subimages(layers,[&](XrSwapchainSubImage& sub){const auto i=auxiliary_chains.find(sub.swapchain);
            if(i==auxiliary_chains.end())return false;sub.swapchain=i->second.handle;return true;}))return false;
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=pair.space;
        projection.layerFlags=layers.scene_flags;projection.viewCount=2;projection.views=views.data();
        auto headers=layers.replace_scene(projection);XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=time;
        end.environmentBlendMode=pair.blend;end.layerCount=unsigned(headers.size());end.layers=headers.data();
        wake_producer();closed=true;result=runtime_end_frame(&end);return XR_SUCCEEDED(result);
    }
    void presentation_loop() noexcept {
        SetThreadDescription(GetCurrentThread(),L"Cheeky OpenXR presentation");
        Pair active;bool have_active{},generated_phase{};double last_position{};
        try {for(;;) {
            {std::lock_guard lock(mx);if(stopping)break;}
            XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};XrFrameState frame{XR_TYPE_FRAME_STATE};
            const auto start=TimingClock::now();auto result=xr.wait_frame(session,&wait,&frame);const auto waited_ms=elapsed_ms(start);
            if(XR_FAILED(result)){std::lock_guard lock(mx);terminal=result;stopping=true;changed.notify_all();break;}
            const auto present_start=TimingClock::now();
            XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};result=xr.begin_frame(session,&begin);
            {std::lock_guard lock(mx);runtime_begin.add(elapsed_ms(present_start));}
            if(XR_FAILED(result)){std::lock_guard lock(mx);terminal=result;stopping=true;changed.notify_all();break;}
            const XrFrameEndInfo* raw_frame{};bool stop_now{};
            {std::lock_guard lock(mx);raw_frame=raw;stop_now=stopping;
                predicted=frame;telemetry.xr_period_ms=double(frame.predictedDisplayPeriod)/1000000.;
                xr_wait.add(waited_ms);telemetry.xr_wait_ms=xr_wait.mean();++ticks;
                if(!generated_phase&&!raw_frame&&!stop_now&&!degraded) {
                    auto selected=ready.end();
                    for(auto it=ready.begin();it!=ready.end();++it) {
                        const bool complete=producer.completed(it->ready);
                        if(complete&&double(it->native_id)>=last_position)selected=it;
                    }
                    if(selected!=ready.end()) {
                        active=std::move(*selected);ready.erase(ready.begin(),std::next(selected));have_active=true;
                        fg_active=active.interpolated;
                        // A transient skipped/reset pair must not accelerate
                        // native production to 1x and overload FG again.
                        if(fg_active)pacing_active=true;
                    }
                    // Prioritize a usable interpolation pair. During NGX
                    // startup, keep showing new native frames even when every
                    // inference lease is busy. Never rewind to a stale pair.
                    if(!fg_active&&ticks>=transition_until&&latest_native&&producer.completed(latest_native->ready)&&
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
                submitted=render(active,images,views,is_generated,frame.predictedDisplayTime,result,closed);
                if(submitted) {
                    repeated=!is_generated&&double(active.native_id)==last_position;
                    last_position=double(active.native_id)+(is_generated?.5:0.);
                    if(is_generated){active.native=active.current;active.native_views=active.current_views;active.native_id=active.current_id;generated_phase=false;}
                    else generated_phase=active.interpolated&&last_position<double(active.current_id);
                }
            }
            if(!closed) {
                XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};empty.displayTime=frame.predictedDisplayTime;
                empty.environmentBlendMode=have_active?active.blend:XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                wake_producer();
                result=runtime_end_frame(&empty);
            }
            {std::lock_guard lock(mx);
                if(is_generated&&submitted)++generated_count;
                if(submitted&&!is_generated&&!raw_frame){if(repeated)++repeated_presentations;else ++distinct_native;}
                present_cpu.add(elapsed_ms(present_start));
                if(XR_SUCCEEDED(result)){++runtime_submissions;
                    const auto now_ms=std::chrono::duration<double,std::milli>(TimingClock::now().time_since_epoch()).count();
                    cadence_window.add(now_ms,{native_count,distinct_native,generated_count,repeated_presentations,runtime_submissions});}
                if(have_active&&!submitted&&!stop_now&&frame.shouldRender==XR_TRUE){++fallback_count;state=CheekyFgState::presentation_failed;
                    degraded=true;fg_active=pacing_active=false;pending.clear();ready.clear();latest_native.reset();have_active=false;generated_phase=false;++epoch;
                    strcpy_s(diagnostic,"Private presentation failed; using coordinated original native submissions");}
                // Bounded semaphore-equivalent credits. Two prime credits let
                // the game overlap production with prior FG/compositor work.
                // GPU copies have been queued before the next game is woken.
                changed.notify_all();
                if(XR_FAILED(result)){terminal=result;stopping=true;changed.notify_all();break;}
            }
        }}catch(...) {std::lock_guard lock(mx);terminal=XR_ERROR_RUNTIME_FAILURE;stopping=true;changed.notify_all();}
    }
};
}
