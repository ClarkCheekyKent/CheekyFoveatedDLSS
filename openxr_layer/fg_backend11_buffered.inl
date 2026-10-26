// Included after BufferedPresenter is complete.
namespace cheeky::fg {
inline bool BufferedPresenter::setup11(XrSession s,const XrFns& f,ID3D11Device* dev,std::uint64_t generation,std::recursive_mutex* dispatch) noexcept {
    if(!dev||!f.create||!f.enumerate||!f.acquire||!f.wait_image||!f.release||!f.wait_frame||!f.begin_frame||!f.end_frame)return false;
    auto gpu=std::make_shared<Backend11>();
    if(!gpu->initialize(dev,generation,dispatch)){strcpy_s(diagnostic,gpu->diagnostic);return false;}
    xr=f;session=s;native11=std::move(gpu);backend=make_frame_backend11(*this);
    strcpy_s(diagnostic,"Native D3D11 shader FG: waiting for stereo frames");
    if(trace){char message[200]{};
        // SINGLETHREADED devices are not free-threaded: creating resources
        // from presenter threads would then be unsafe.
        sprintf_s(message,"FG native D3D11 backend: shader interpolation with shared pacing; no D3D12 bridge or NGX; device_flags=0x%x",
            dev->GetCreationFlags());trace(message);}
    return true;
}

inline bool BufferedPresenter::capture11(XrSwapchain sc,unsigned index,ID3D11Texture2D* source,unsigned width,unsigned height,unsigned array,DXGI_FORMAT declared) {
    if(!native11)return Presenter::capture11(sc,index,source,width,height,array,declared);
    if(bypass||!source)return false;
    std::lock_guard capture_lock(capture_mutex);
    // Quad views: the release hook also sees the four raw planes; only the
    // composed eyes (capture_composed11) and auxiliary layers are presented.
    if(quad_input&&std::find(quad_sources11.begin(),quad_sources11.end(),sc)!=quad_sources11.end())return false;
    {std::lock_guard lock(mx);if(stopping||degraded)return false;}
    const auto captured_at=TimingClock::now();
    AddElapsed elapsed{capture_cpu_accumulator};
    auto copy=native11->copy(source,declared);
    if(!copy){capture_overflow=true;return false;}
    direct_captures.erase(sc);captured[sc]=std::move(copy);capture_crop_tokens[sc]=0;
    capture_tokens[sc]=0;capture_frames[sc]=0;capture_times[sc]=captured_at;capture_ready[sc]={};return true;
}

inline bool BufferedPresenter::capture_composed11(XrSwapchain sc,ID3D11Texture2D* source,DXGI_FORMAT format) {
    if(!native11||bypass||!source)return false;
    const auto captured_at=TimingClock::now();auto copy=native11->copy(source,format);
    std::lock_guard lock(composed11_mutex);if(!copy){composed11.erase(sc);return false;}
    composed11[sc]={std::move(copy),captured_at};return true;
}

inline bool BufferedPresenter::prepare_native11(Native& job,const XrCompositionLayerProjection* projection,bool inference_slot) {
    // Hold capture_mutex.
    for(unsigned eye=0;eye<2;++eye){
        const auto& sub=projection->views[eye].subImage;Image full;TimingClock::time_point captured_at{};
        if(quad_input){std::lock_guard lock(composed11_mutex);const auto found=composed11.find(sub.swapchain);
            if(found!=composed11.end()){full=found->second.first;captured_at=found->second.second;}}
        else if(const auto found=captured.find(sub.swapchain);found!=captured.end()){full=found->second;captured_at=capture_times[sub.swapchain];}
        if(!full)return false;
        job.color[eye]=native11->crop(full,sub,native11_skip_identity_crop.load(std::memory_order_relaxed));if(!job.color[eye])return false;
        if(job.color[eye]!=full)++eye_crop_copies;
        if(!eye||captured_at<job.captured_at)job.captured_at=captured_at;
        job.views[eye]=projection->views[eye];
    }
    const bool good=subimages(job.layers,[&](XrSwapchainSubImage& sub){
        const auto found=captured.find(sub.swapchain);if(found==captured.end())return false;
        D3D11_TEXTURE2D_DESC d{};found->second->texture11->GetDesc(&d);
        if(sub.imageArrayIndex>=d.ArraySize||sub.imageRect.offset.x<0||sub.imageRect.offset.y<0||
            sub.imageRect.extent.width<=0||sub.imageRect.extent.height<=0||
            std::uint64_t(sub.imageRect.offset.x)+sub.imageRect.extent.width>d.Width||
            std::uint64_t(sub.imageRect.offset.y)+sub.imageRect.extent.height>d.Height)return false;
        if(std::none_of(job.auxiliary.begin(),job.auxiliary.end(),[&](const Auxiliary& a){return a.source==sub.swapchain;}))
            job.auxiliary.push_back({sub.swapchain,found->second});return true;
    });
    if(!good)return false;
    if(inference_slot){
        CheekyFgColorOptionsV1 options;
        if(optical::color_options(options)){
            auto payload=std::make_shared<FrameGuides11>();
            for(unsigned eye=0;eye<2;++eye){auto guide=std::make_shared<Backend11::Guides>();
                guide->source.meta=options.eyes[eye];D3D11_TEXTURE2D_DESC d{};job.color[eye]->texture11->GetDesc(&d);
                guide->source.meta.width=d.Width;guide->source.meta.height=d.Height;
                if(quad_input){auto& g=guide->source.meta;const auto& r=native11->optical_quad_regions[eye];
                    g.focus_width=r[2];g.focus_height=r[3];g.focus_x=r[0]+r[2]*.5f-.5f;g.focus_y=r[1]+r[3]*.5f-.5f;
                    g.focus_feather=2.f/std::max(1u,std::min(d.Width,d.Height));}
                guide->source.mapping_revision=guide->source.meta.configuration;payload->eyes[eye]=std::move(guide);}
            job.source_configuration=options.eyes[0].configuration;job.guide_token=job.id*2;
            job.guides=std::move(payload);job.policy={options.release_point,options.queue_priority,true};job.guides_valid=true;
            job.guide_state=CheekyFgState::waiting_for_history;return true;
        }
        std::array<std::shared_ptr<Backend11::Guides>,2> guides;
        if(native11->take_guides(guides)){
            auto payload=std::make_shared<FrameGuides11>();payload->eyes=std::move(guides);
            job.source_configuration=payload->eyes[0]->source.meta.configuration;
            job.guide_token=payload->eyes[0]->source.mapping_revision;
            const auto& g=payload->eyes[0]->source.meta;
            job.policy={};job.policy.optical_flow=cheeky_fg_uses_flow(g.center_method,g.peripheral_method,g.focus_width<.999f||g.focus_height<.999f);
            job.guides=std::move(payload);job.guides_valid=true;job.guide_state=CheekyFgState::waiting_for_history;
        }else job.guide_state=CheekyFgState::waiting_for_guides;
    }
    return true;
}

inline bool BufferedPresenter::upload11(Chain& chain,const Image& source) {
    // A deferred interpolation pass renders straight into the OpenXR image.
    const auto* draw=Backend11::deferred(source);
    if(!source||(!source->texture11&&!draw))return false;
    D3D11_TEXTURE2D_DESC d{};if(draw)d=draw->desc;else source->texture11->GetDesc(&d);
    if(chain.handle&&(chain.desc.Width!=d.Width||chain.desc.Height!=d.Height||chain.desc.Format!=d.Format||chain.desc.DepthOrArraySize!=d.ArraySize)){
        destroy_private_chain(chain.handle);chain={};
    }
    if(!chain.handle){
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};create.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        create.format=d.Format;create.width=d.Width;create.height=d.Height;create.arraySize=d.ArraySize;
        create.sampleCount=create.faceCount=create.mipCount=1;
        // Every runtime call that may touch the game's D3D11 context goes through
        // the dispatch gate: an ungated call racing the game's NGX evaluation
        // reproduced the NVIDIA driver-worker crash (--fg-shader11-ngx ungated).
        const auto gated=[&](auto&& call){return native11->runtime_submit(call);};
        if(XR_FAILED(gated([&]{return xr.create(session,&create,&chain.handle);}))){chain={};return false;}
        unsigned count{};
        if(XR_FAILED(gated([&]{return xr.enumerate(chain.handle,0,&count,nullptr);}))||!count){destroy_chain(chain.handle);chain={};return false;}
        chain.images11.assign(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if(XR_FAILED(gated([&]{return xr.enumerate(chain.handle,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(chain.images11.data()));}))){
            destroy_chain(chain.handle);chain={};return false;}
        chain.desc.Width=d.Width;chain.desc.Height=d.Height;chain.desc.Format=d.Format;chain.desc.DepthOrArraySize=UINT16(d.ArraySize);
    }
    if(chain.uploaded==source->identity)return true;
    unsigned index{};XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if(XR_FAILED(native11->runtime_submit([&]{return xr.acquire(chain.handle,&acquire,&index);})))return false;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
    const auto wait_begin=TimingClock::now();const auto wait_result=native11->runtime_submit([&]{return xr.wait_image(chain.handle,&wait);});
    {std::lock_guard lock(mx);present_image_wait.add(elapsed_ms(wait_begin));}
    const bool copied=XR_SUCCEEDED(wait_result)&&index<chain.images11.size()&&
        (draw?native11->draw_into(chain.images11[index].texture,source):native11->upload(chain.images11[index].texture,source));
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const bool released=XR_SUCCEEDED(native11->runtime_submit([&]{return xr.release(chain.handle,&release);}));
    if(released&&copied){chain.uploaded=source->identity;chain.last_uploaded_index=index;}
    return released&&copied;
}

struct BufferedBackend11 final : FrameBackend {
    using Native=BufferedNative;using Pair=BufferedPair;using Auxiliary=BufferedAuxiliary;
    using DirectRequest=BufferedDirectRequest;using Chain=BufferedChain;
    using CapturePlan=BufferedResources12::CapturePlan;using Slice=BufferedResources12::Slice;
    BufferedPresenter& owner;
    ApplicationTimer11 application_timer;
    explicit BufferedBackend11(BufferedPresenter& host):owner(host){}
    // Every stage is queued on the same immediate context. Preserve the native
    // D3D11 ordering contract: queued work is ready for ordered publication.
    bool ready(std::uint64_t) const override {return true;}
    void begin_application_frame(bool measure) override {
        if(!measure&&!application_timer.queries.next_sequence)return;
        owner.native11->submit([&](ID3D11DeviceContext* context){application_timer.begin(owner.native11->device.Get(),context,measure);return true;},false);
    }
    ApplicationTiming application_timing() const override {return application_timer.sample();}
    CaptureOutcome capture(const std::shared_ptr<BufferedNative>& frame,
        const XrCompositionLayerProjection* projection,bool inference_slot,
        const CheekyFgPair*,CheekyFgCompleteHooklessFrameFn) override {
        frame->direct_output_mode=0;
        frame->latency_diagnostics=false;frame->ready=0;
        const bool captured=owner.prepare_native11(*frame,projection,inference_slot);
        if(captured&&application_timer.open)owner.native11->submit([&](ID3D11DeviceContext* context){application_timer.end(context);return true;},false);
        return captured?CaptureOutcome::captured:CaptureOutcome::unavailable;
    }
    GenerationOutcome generate(const std::shared_ptr<BufferedNative>& previous,
        const std::shared_ptr<BufferedNative>& job,BufferedPair& pair,
        bool&,std::uint64_t&,
        std::shared_ptr<BufferedDirectRequest>&) override {
        GenerationOutcome outcome;auto& success=outcome.success;auto& attempted=outcome.attempted;auto& history_was_reset=outcome.history_reset;

        // Native D3D11 shader interpolation. Each guide snapshot keeps
        // its own reset flag; a reset pair only seeds history.
        if(previous&&previous->epoch==job->epoch&&previous->id+1==job->id&&previous->space==job->space&&
           job->guides_valid&&previous->guides_valid&&guides11(*job)&&guides11(*previous)&&
           cheeky::foveated_dlss::fg_same_source_history(previous->source_configuration,job->source_configuration)) {
            bool same_size=true;
            for(unsigned eye=0;eye<2;++eye){D3D11_TEXTURE2D_DESC a{},b{};
                previous->color[eye]->texture11->GetDesc(&a);job->color[eye]->texture11->GetDesc(&b);
                same_size&=a.Width==b.Width&&a.Height==b.Height&&a.Format==b.Format;}
            const auto& now=guides11(*job)->eyes;const auto& before=guides11(*previous)->eyes;
            history_was_reset=now[0]->source.meta.reset||now[1]->source.meta.reset||!same_size;
            attempted=true;
            if(!history_was_reset){
                pair.submitted_at=TimingClock::now();
                pair.generated_count=std::clamp(job->multiplier,2u,4u)-1;
                success=true;
                for(unsigned index=0;index<pair.generated_count&&success;++index){
                    pair.generated_index=index;
                    auto& output=index?pair.extra_generated[index-1]:pair.generated;
                    success=owner.native11->generate(previous->color,job->color,before,now,previous->views,job->views,output,
                        owner.direct11_requested.load(),float(index+1)/float(pair.generated_count+1));
                    if(success){
                        pair.submitted_at=TimingClock::now();
                        // announce_generated_phase reads pair.generated. Swap
                        // only the private local record while publishing G2/G3.
                        if(index)pair.generated.swap(pair.extra_generated[index-1]);
                        owner.announce_generated_phase(previous,job,pair,std::make_shared<FrameCompletion11>());
                        if(index)pair.generated.swap(pair.extra_generated[index-1]);
                    }
                }
                pair.generated_index=0;
                owner.inference_submitted(); // Releases the native credit gate, as the NGX worker callback does.
                if(!success){attempted=false;
                    if(!cheeky_fg_uses_flow(now[0]->source.meta.center_method,now[0]->source.meta.peripheral_method,true))
                        strcpy_s(owner.native11->diagnostic,"Native D3D11 shader FG: interpolation pass failed; native presentation continues");}
                else strcpy_s(owner.native11->diagnostic,"Native D3D11 FG: generating (no D3D12 bridge or NGX)");
            }
        }

        return outcome;
    }
    bool publish(const BufferedPair& pair,const std::array<Image,2>& images,
        std::array<XrCompositionLayerProjectionView,2>& views,FrameLayers& layers,bool) override {
        for(unsigned eye=0;eye<2;++eye){
            auto& chain=owner.scene_chains[eye];if(!owner.upload11(chain,images[eye])){owner.native11->cancel_gpu_timing();return false;}
            views[eye].type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;views[eye].next=nullptr;
            views[eye].subImage={chain.handle,{{0,0},{int(chain.desc.Width),int(chain.desc.Height)}},0};
        }
        for(const auto& a:pair.auxiliary)if(!owner.upload11(owner.auxiliary_chains[a.source],a.image))return false;
        if(!owner.subimages(layers,[&](XrSwapchainSubImage& sub){const auto i=owner.auxiliary_chains.find(sub.swapchain);
            if(i==owner.auxiliary_chains.end())return false;sub.swapchain=i->second.handle;return true;}))return false;
        return true;
    }
    void retire() override {
        owner.native11->submit([&](ID3D11DeviceContext* context){application_timer.reset(context);return true;},false);
        owner.native11->cancel_gpu_timing();
        owner.native11->flush();owner.native11->clear_guides();
        std::lock_guard lock(owner.composed11_mutex);owner.composed11.clear();
    }
    // Composed quad eyes are timestamped and replaced per release; retire()
    // drops them when the session stops.
    void discard_captures(bool) override {}
    void finish_generation() override {}
    void reset_generation() override {}
    // The bidirectional shader still samples previous-interval depth.
    void retire_guides(BufferedNative&) override {}
    void reset_statistics(bool) override {owner.native11->gpu_timings.reset();owner.native11->optical_timings.reset();owner.native11->capture_timings.reset();
        owner.native11->guide_timings.reset();owner.native11->upload_timings.reset();}
    GenerationStatus generation_status() const override {return {false,false,0,owner.native11->diagnostic};}
    void snapshot(CheekyFgPerformance& performance,CheekyFgOverhead& overhead,
        CheekyFgExperimentDiagnostics& experiments) const override {
        performance=owner.native11->gpu_timings.snapshot();overhead={};owner.worker.latency.snapshot(experiments);
        const auto optical=owner.native11->optical_timings.snapshot();
        performance.total_ms+=optical.total_ms;performance.total_last_ms+=optical.total_last_ms;
    }
    XrResult end_frame(const XrFrameEndInfo* end) override {
        return owner.native11->runtime_submit([&]{return owner.xr.end_frame(owner.session,end);});
    }
    bool service_direct_output(double,BufferedPair&) override {return false;}
    bool supports_direct_output() const override {return false;}
};

}
