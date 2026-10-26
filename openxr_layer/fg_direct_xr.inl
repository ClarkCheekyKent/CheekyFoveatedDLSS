// Included in cheeky::fg after BufferedPresenter is complete.
inline void BufferedPresenter::note_direct_failure_locked(CheekyFgDirectReason reason,unsigned eye,const DirectRequest* request,
    const Chain* chain,const D3D12_RESOURCE_DESC* image,std::int32_t api_result) {
    const auto count=direct_details.rejections+1;direct_details={};auto& d=direct_details;
    d.supported=1;d.reason=reason;d.eye=eye;d.rejections=count;d.xr_result=api_result;
    if(request){d.expected_format=unsigned(request->format);d.expected_width=request->width;d.expected_height=request->height;}
    if(chain){d.chain_format=unsigned(chain->desc.Format);d.chain_width=unsigned(chain->desc.Width);d.chain_height=chain->desc.Height;}
    if(image){d.image_format=unsigned(image->Format);d.image_flags=unsigned(image->Flags);d.image_width=unsigned(image->Width);d.image_height=image->Height;}
    const auto& old=logged_direct_details;
    if(trace&&(old.reason!=d.reason||old.image_format!=d.image_format||old.image_flags!=d.image_flags||old.xr_result!=d.xr_result||
        old.chain_format!=d.chain_format||old.expected_width!=d.expected_width||old.expected_height!=d.expected_height)){
        char message[512]{};sprintf_s(message,"FG direct XR fallback: reason=%u (%s) eye=%u expected=%ux%u format=%u chain=%ux%u format=%u backing=%ux%u format=%u flags=0x%x result=0x%08x",
            unsigned(reason),cheeky_fg_direct_reason_name(reason),eye,d.expected_width,d.expected_height,d.expected_format,
            d.chain_width,d.chain_height,d.chain_format,d.image_width,d.image_height,d.image_format,d.image_flags,unsigned(api_result));
        trace(message);logged_direct_details=d;
    }
}

inline bool BufferedPresenter::service_direct_output(double position,Pair& result) {
    std::shared_ptr<DirectRequest> request;
    {
        std::lock_guard lock(mx);request=direct_request;
        if(!request||request->decided)return false;
        bool supported=true,eligible=!stopping&&!raw&&!degraded&&request->epoch==epoch&&
            position==double(request->native_id)&&direct_output_mode==1u;
        CheekyFgDirectReason reason{};unsigned bad_eye{};
        for(unsigned eye=0;eye<2;++eye){const auto& c=scene_chains[eye];
            if(reason==CheekyFgDirectReason::none){
                if(!c.handle)reason=CheekyFgDirectReason::chain_missing;
                else if(!c.direct_writable)reason=CheekyFgDirectReason::chain_usage;
                else if(c.desc.Width!=request->width||c.desc.Height!=request->height)reason=CheekyFgDirectReason::chain_extent;
                else if(c.desc.Format!=request->format)reason=CheekyFgDirectReason::chain_format;
                if(reason!=CheekyFgDirectReason::none)bad_eye=eye;
            }
            supported&=c.handle&&c.direct_writable&&c.desc.Width==request->width&&c.desc.Height==request->height&&c.desc.Format==request->format;
            eligible&=c.uploaded==request->previous[eye]->identity;
        }
        // The last pair's generated submission can release the next game
        // render one tick before its newest native image is shown. Present
        // that native first, then acquire its successor's generated target.
        if(supported&&!eligible&&!stopping&&!raw&&!degraded&&request->epoch==epoch&&
            direct_output_mode==1u&&position<double(request->native_id)&&request->deferred_ticks++<4)return false;
        if(!eligible||!supported){
            request->decided=true;direct_request.reset();++direct_stats.fallbacks;
            if(!supported)++direct_stats.unsupported;else ++direct_stats.not_ready;
            if(reason==CheekyFgDirectReason::none)reason=stopping||raw||degraded||request->epoch!=epoch||direct_output_mode!=1u?
                CheekyFgDirectReason::cancelled:CheekyFgDirectReason::native_not_ready;
            note_direct_failure_locked(reason,bad_eye,request.get(),&scene_chains[bad_eye]);
            direct_stats.last_path=3;changed.notify_all();return false;
        }
    }
    std::array<unsigned,2> indices{};std::array<bool,2> acquired{},waited{};bool good=true;
    CheekyFgDirectReason failure{};unsigned bad_eye{};D3D12_RESOURCE_DESC backing{};std::int32_t api_result{};
    for(unsigned eye=0;eye<2;++eye){auto& c=scene_chains[eye];
        bad_eye=eye;
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        api_result=xr.acquire(c.handle,&acquire,&indices[eye]);
        if(XR_FAILED(api_result)){failure=CheekyFgDirectReason::acquire;good=false;break;}acquired[eye]=true;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
        api_result=xr.wait_image(c.handle,&wait);waited[eye]=XR_SUCCEEDED(api_result);
        if(!waited[eye]){failure=CheekyFgDirectReason::wait;good=false;break;}
        if(indices[eye]>=c.images.size()){failure=CheekyFgDirectReason::image_extent;good=false;break;}
        auto* texture=c.images[indices[eye]].texture;const auto d=texture->GetDesc();backing=d;
        if(!writable_color(d.Format,request->format))failure=CheekyFgDirectReason::image_format;
        else if(!(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)||!(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))failure=CheekyFgDirectReason::image_usage;
        else if(d.Width!=request->width||d.Height!=request->height||d.DepthOrArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1)failure=CheekyFgDirectReason::image_extent;
        if(failure!=CheekyFgDirectReason::none){good=false;break;}
        request->targets[eye]={texture,D3D12_RESOURCE_STATE_RENDER_TARGET,{},true};
    }
    const auto start=TimingClock::now();
    {
        std::unique_lock lock(mx);
        good&=!stopping&&!raw&&!degraded&&epoch==request->epoch;
        request->allowed=good;request->decided=true;direct_request.reset();changed.notify_all();
        // Wait for the worker to publish its final queued fence/result. The
        // binding queue waits for GPU completion below, including on shutdown.
        if(good)changed.wait(lock,[&]{return request->complete;});
        if(good){direct_wait.add(elapsed_ms(start));direct_stats.wait_ms=direct_wait.mean();direct_stats.last_wait_ms=direct_wait.last;}
        else{++direct_stats.fallbacks;++direct_stats.unsupported;direct_stats.last_path=3;
            note_direct_failure_locked(failure==CheekyFgDirectReason::none?CheekyFgDirectReason::cancelled:failure,bad_eye,request.get(),&scene_chains[bad_eye],&backing,api_result);
            if(failure==CheekyFgDirectReason::image_format||failure==CheekyFgDirectReason::image_usage||failure==CheekyFgDirectReason::image_extent)
                direct_blocked_target=direct_target_key(request->width,request->height,request->format);}
        good&=request->success&&!stopping&&!raw&&!degraded&&epoch==request->epoch;
    }
    // Order the binding queue after inference even when inference used a
    // separate queue. Release lets the runtime enqueue work on that queue.
    if(request->fence){const auto hr=release_queue()->Wait(request->fence.Get(),request->serial);if(FAILED(hr)){
        good=false;std::lock_guard lock(mx);note_direct_failure_locked(CheekyFgDirectReason::queue_wait,0,request.get(),nullptr,nullptr,hr);}}
    for(unsigned eye=0;eye<2;++eye)if(acquired[eye]){
        scene_chains[eye].uploaded=0;
        if(waited[eye]){XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            const auto released=xr.release(scene_chains[eye].handle,&release);good&=XR_SUCCEEDED(released);
            if(XR_FAILED(released)){std::lock_guard lock(mx);note_direct_failure_locked(CheekyFgDirectReason::release,eye,request.get(),&scene_chains[eye],nullptr,released);}}
        else good=false;
    }
    if(good){result=std::move(request->result);return true;}
    if(request->allowed){std::lock_guard lock(mx);finish_generated_gate(request->result.current_id,false);}
    return false;
}
