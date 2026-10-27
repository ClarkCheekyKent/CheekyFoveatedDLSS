// D3D12 capture, interpolation and publication behind the shared pacing core.
#include "fg_application_timing.hpp"
#include "../src/d3d12_native.hpp"
namespace cheeky::fg {
inline bool BufferedPresenter::accept_guide_queue(const CheekyFgPair& guides,FrameGuides12& payload,bool hookless) noexcept {
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
    payload.sync_fence=guide_sync_fence;
    payload.sync_value=value;
    if(!guide_sync_reported&&trace){
        trace("DLSS-FG hook-free guide queue differs from OpenXR queue: validated same-device cross-queue GPU fence enabled");
        guide_sync_reported=true;
    }
    return true;
}

inline bool BufferedPresenter::setup(XrSession s,const XrFns& f,ID3D12Device* dev,ID3D12CommandQueue* q) noexcept {
    if(!Presenter::setup(s,f,dev,q))return false;

    // All methods start on the game queue. Only an optical-flow job explicitly
    // selecting HIGH may allocate/use the separate inference queue below.
    generation_queue=q;generation_high_priority=false;reported_queue_mode=2;
    last_fg_priority_requested=UINT32_MAX;
    guide_sync_fence.Reset();guide_sync_serial=0;guide_sync_reported=false;
    // Failure merely disables split-queue FG; the native OpenXR path
    // remains usable, and matching queues need no additional fence.
    dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&guide_sync_fence));
    // Capture and XR uploads use the original game/binding queue.
    return producer.initialize(dev,q)&&output.initialize(dev,q)&&uploads.initialize(dev,q);
}

inline void BufferedPresenter::measure_worker_memory() {
    // Single generation worker, after the prior GPU fence retired. Publish
    // only integer snapshots; diagnostic readers never walk live resources.
    const auto now=GetTickCount64();if(now<next_worker_memory)return;
    output.trim_unused(now);
    CheekyFgMemory value;fg_memory::Collector collect(device.Get());
    value.categories[fg_memory::generated]=output.memory(collect);
    for(const auto* pipeline:{&worker.focus,&worker.ngx_phases[0].focus,&worker.ngx_phases[1].focus}){
        for(const auto& eye:pipeline->inputs)for(const auto& input:eye){
            collect.add(value.categories[fg_memory::prepared_color],input.color.Get());
            collect.add(value.categories[fg_memory::prepared_color],input.recentered_color.Get());
            collect.add(value.categories[fg_memory::prepared_motion],input.corrected_motion.Get());
            // Prepared captures are owned/countable by the producer snapshot
            // pool. These references are borrowed, not a second allocation.
            if(!input.borrowed_guides){collect.add(value.categories[fg_memory::prepared_depth],input.depth.Get());
                collect.add(value.categories[fg_memory::prepared_motion],input.motion.Get());}
        }
        for(unsigned eye=0;eye<2;++eye){
            collect.add(value.categories[fg_memory::periphery],pipeline->peripheral_output[eye].Get());
            collect.add(value.categories[fg_memory::periphery],pipeline->previous_depth[eye].Get());
            collect.add(value.categories[fg_memory::prepared_color],pipeline->center_output[eye].Get());
            collect.add(value.categories[fg_memory::prepared_depth],pipeline->previous_center_depth[eye].Get());
            collect.add(value.categories[fg_memory::composition],pipeline->full[eye].Get());
        }
    }
    for(const auto* features:{&worker.features,&worker.peripheral_features,
        &worker.ngx_phases[0].features,&worker.ngx_phases[0].peripheral_features,
        &worker.ngx_phases[1].features,&worker.ngx_phases[1].peripheral_features})for(const auto& f:*features){
        collect.add(value.categories[fg_memory::ngx_outputs],f.output.Get());
        collect.add(value.categories[fg_memory::ngx_outputs],f.disable.Get());
    }
    std::lock_guard lock(mx);
    for(auto category:{fg_memory::generated,fg_memory::prepared_color,fg_memory::prepared_depth,
            fg_memory::prepared_motion,fg_memory::ngx_outputs,fg_memory::periphery,fg_memory::composition})
        memory_state.categories[category]=value.categories[category];
    memory_state.worker_tick=now;next_worker_memory=now+1000;
}

inline void BufferedPresenter::measure_presenter_memory() {
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

inline void BufferedPresenter::collect_color_stamps() {
    if(native11)return; // No GPU color timestamps on the native D3D11 backend.
    std::lock_guard capture_lock(capture_mutex);producer.collect(producer.fence->GetCompletedValue());
}

inline bool BufferedPresenter::capture_quad_planes(const XrCompositionLayerProjection& raw,const XrCompositionLayerProjection& logical,
        const std::array<std::array<float,4>,2>& insets,const CheekyFgPair& pair) {
    if(raw.views[0].subImage.swapchain==raw.views[1].subImage.swapchain)return false;
    std::lock_guard capture_lock(capture_mutex);
    std::array<Image,2> outer,center,packed;std::array<CompactLayout,2> layouts;
    for(unsigned eye=0;eye<2;++eye){const auto o=captured.find(raw.views[eye].subImage.swapchain),c=captured.find(raw.views[eye+2].subImage.swapchain);
        if(o==captured.end()||c==captured.end())return false;outer[eye]=o->second;center[eye]=c->second;
        const auto& logical_rect=logical.views[eye].subImage.imageRect;
        const auto& inset=insets[eye];auto layout=compact_layout(unsigned(logical_rect.extent.width),unsigned(logical_rect.extent.height),true,
            inset[2],inset[3],inset[0]+inset[2]*.5f-.5f,inset[1]+inset[3]*.5f-.5f,1,0);
        if(!layout.packed())return false;
        layout.center_width=unsigned(raw.views[eye+2].subImage.imageRect.extent.width);layout.center_height=unsigned(raw.views[eye+2].subImage.imageRect.extent.height);
        layout.context_width=unsigned(raw.views[eye].subImage.imageRect.extent.width);layout.context_height=unsigned(raw.views[eye].subImage.imageRect.extent.height);
        if(!layout.center_width||!layout.center_height||!layout.context_width||!layout.context_height||layout.stored_width()>16384||layout.stored_height()>16384)return false;
        layouts[eye]=layout;
    }
    uint64_t frame{};{std::lock_guard lock(mx);if(stopping||degraded)return false;frame=next_id+1;}
    auto* c=producer.begin(frame);if(!c)return false;
    bool good=true;for(unsigned eye=0;eye<2&&good;++eye){packed[eye]=producer.quad_regions(*c,outer[eye],center[eye],raw.views[eye].subImage,raw.views[eye+2].subImage,layouts[eye]);good=bool(packed[eye]);}
    uint64_t ready{};if(!producer.execute(*c,ready))return false;if(!good)return false;
    for(unsigned eye=0;eye<2;++eye){const auto& sub=logical.views[eye].subImage;
        direct_captures[sub.swapchain].eyes={{{sub.imageRect,sub.imageArrayIndex},packed[eye]}};
        auto desc=packed[eye]->texture->GetDesc();desc.Width=layouts[eye].width;desc.Height=layouts[eye].height;captured_descs[sub.swapchain]=desc;
        capture_tokens[sub.swapchain]=capture_crop_tokens[sub.swapchain]=pair.capture_token;capture_frames[sub.swapchain]=pair.render_frame;
        capture_times[sub.swapchain]=std::min(capture_times[raw.views[eye].subImage.swapchain],capture_times[raw.views[eye+2].subImage.swapchain]);
    }
    return true;
}

inline bool BufferedPresenter::capture(XrSwapchain sc,unsigned,ID3D12Resource* original,unsigned,unsigned,unsigned,
                 DXGI_FORMAT declared) noexcept {
    if(bypass||!original)return false;
    std::lock_guard capture_lock(capture_mutex);
    {std::lock_guard lock(mx);if(stopping||degraded)return false;}
    const auto captured_at=TimingClock::now();
    const auto captured_cpu=latency_diagnostics?LatencyProbe::cpu_now():0;
    AddElapsed elapsed{capture_cpu_accumulator};
    auto runtime=GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll");
    const auto stamp_fn=guide_stamp?guide_stamp:runtime&&!quad_input?reinterpret_cast<CheekyFgCaptureStampFn>(GetProcAddress(runtime,"CheekyFG_GetCaptureStamp")):nullptr;
    std::uint64_t profile_frame{};{std::lock_guard lock(mx);profile_frame=next_id+1;}
    if(color_stamp_frame!=profile_frame){color_stamp_frame=profile_frame;color_stamp_token=0;}
    CheekyFgCaptureStamp stamp;stamp.token=color_stamp_token;
    if(stamp_fn&&!stamp_fn(original,&stamp))stamp={};
    if(stamp.token&&!stamp.render_frame&&!color_stamp_token)color_stamp_token=stamp.token;
    // The legacy test callback has no production fallback. Production keeps
    // either a tagged RHI identity or the hook-free color-bound guide token.
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
    if(storage_options.compact&&!quad_input){
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
    auto* c=producer.begin(profile_frame);if(!c){capture_overflow=true;return false;}
    const auto ready_stamp=captured_cpu?producer.mark_color_ready(*c,captured_cpu):ColorReady{};
    const auto desc=original->GetDesc();captured_descs[sc]=desc;
    const auto plan=capture_plans.find(sc);
    const bool direct=!quad_input&&plan!=capture_plans.end()&&!plan->second.full_required&&
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
        if(plan==capture_plans.end()||!plan->second.full_required)++capture_fallbacks;
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

inline bool BufferedPresenter::ensure_chain(Chain& chain,const Image& image,bool scene) {
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

inline bool BufferedPresenter::upload_image(Chain& chain,const Image& source,bool scene) {
    // One image through the same batched path as presentation.
    return ensure_chain(chain,source,scene)&&upload_batch({{&chain,source}});
}

inline bool BufferedPresenter::upload_batch(const std::vector<std::pair<Chain*,Image>>& requests) {
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
            // Shared-queue phases were submitted before their announcement;
            // these uploads follow their output copies through queue order.
            // Separate HIGH phases must already be GPU-ready at selection.
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

struct BufferedBackend12 final : FrameBackend {
    using Native=BufferedNative;using Pair=BufferedPair;using Auxiliary=BufferedAuxiliary;
    using DirectRequest=BufferedDirectRequest;using Chain=BufferedChain;
    using CapturePlan=BufferedResources12::CapturePlan;using Slice=BufferedResources12::Slice;
    BufferedPresenter& owner;
    ApplicationTimer12 application_timer;
    explicit BufferedBackend12(BufferedPresenter& host):owner(host){}
    bool ready(std::uint64_t ticket) const override {return owner.producer.completed(ticket);}
    void begin_application_frame(bool measure) override {application_timer.begin(owner.producer,measure);}
    ApplicationTiming application_timing() const override {return application_timer.sample();}
    unsigned maximum_multiplier(const BufferedNative& frame) const override {
        const auto* payload=guides12(frame);
        return payload&&cheeky_fg_needs_ngx(payload->pair)?std::min(4u,owner.worker.max_generated_frames+1):4u;
    }
    CaptureOutcome capture(const std::shared_ptr<BufferedNative>& job,
        const XrCompositionLayerProjection* projection,bool inference_slot,
        const CheekyFgPair* external,CheekyFgCompleteHooklessFrameFn hookless_complete) override {
        bool good=true;

        std::unordered_map<XrSwapchain,CapturePlan> next_plans;
        for(unsigned eye=0;eye<2;++eye){const auto& sub=projection->views[eye].subImage;
            auto& plan=next_plans[sub.swapchain];plan.desc=owner.captured_descs[sub.swapchain];
            Slice slice{sub.imageRect,sub.imageArrayIndex};
            auto existing=std::find_if(plan.slices.begin(),plan.slices.end(),
                [&](const auto& x){return owner.same_slice(x,slice);});
            if(existing==plan.slices.end()){
                plan.slices.push_back(slice);plan.physical_eyes.push_back(eye);
            }else if(plan.physical_eyes[std::size_t(existing-plan.slices.begin())]!=eye){
                // Same image subrect is used for both eyes; cannot give it
                // two different packed layouts. Copy the full texture.
                plan.full_required=true;
            }
        }
        owner.subimages(job->layers,[&](XrSwapchainSubImage& sub){next_plans[sub.swapchain].full_required=true;return true;});
        owner.capture_plans=std::move(next_plans);
        job->latency_diagnostics=owner.latency_diagnostics!=0;
        job->direct_output_mode=owner.quad_input?0u:owner.direct_output_mode.load();
        auto* c=owner.producer.begin(job->id,true);
        if(!c){
            if(inference_slot&&hookless_complete)hookless_complete(0);
            return CaptureOutcome::busy;
        }
        good=true;std::uint64_t expected_token{},expected_frame{};bool tokens_match=true;
        if(good)for(unsigned eye=0;eye<2;++eye) {
            const auto& sub=projection->views[eye].subImage;
            const auto direct=owner.direct_captures.find(sub.swapchain);
            if(direct!=owner.direct_captures.end()){
                const Slice requested{sub.imageRect,sub.imageArrayIndex};
                for(const auto& [slice,image]:direct->second.eyes)if(owner.same_slice(slice,requested)){job->color[eye]=image;break;}
                if(!job->color[eye])++owner.capture_fallbacks; // Changed layout: coordinated native passthrough below.
            }else {
                const auto found=owner.captured.find(sub.swapchain);
                if(found!=owner.captured.end()){
                    // Full-swapchain capture is an ordinary full-eye
                    // fallback, never repack it using stale cached crop.
                    // The FG worker samples its full resolution directly.
                    job->color[eye]=owner.producer.crop(*c,found->second,sub.imageRect,sub.imageArrayIndex);
                }
                if(job->color[eye])++owner.eye_crop_copies;
            }
            if(!job->color[eye]){good=false;break;}
            const auto token=owner.capture_tokens[sub.swapchain];
            const auto frame=owner.capture_frames[sub.swapchain];
            const auto captured_at=owner.capture_times[sub.swapchain];
            job->color_ready[eye]=owner.capture_ready[sub.swapchain];
            if(!eye||captured_at<job->captured_at)job->captured_at=captured_at;
            if(!eye){expected_token=token;expected_frame=frame;}else tokens_match&=token==expected_token&&frame==expected_frame;
            owner.producer.freeze(*c,job->color[eye]);job->views[eye]=projection->views[eye];
        }
        if(good)good=owner.subimages(job->layers,[&](XrSwapchainSubImage& sub){
            const auto found=owner.captured.find(sub.swapchain);if(found==owner.captured.end())return false;
            const auto d=found->second->texture->GetDesc();
            if(sub.imageArrayIndex>=d.DepthOrArraySize||sub.imageRect.offset.x<0||sub.imageRect.offset.y<0||
                sub.imageRect.extent.width<=0||sub.imageRect.extent.height<=0||
                std::uint64_t(sub.imageRect.offset.x)+sub.imageRect.extent.width>d.Width||
                std::uint64_t(sub.imageRect.offset.y)+sub.imageRect.extent.height>d.Height)return false;
            if(std::none_of(job->auxiliary.begin(),job->auxiliary.end(),[&](const Auxiliary& a){return a.source==sub.swapchain;}))
                job->auxiliary.push_back({sub.swapchain,found->second});return true;
        });
        if(good) {
            if(external){expected_token=external->capture_token;expected_frame=external->render_frame;tokens_match=expected_token!=0;}
            const auto runtime=GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll");
            const auto complete_v2=external?nullptr:owner.hookless_complete_v2?owner.hookless_complete_v2:
                runtime?reinterpret_cast<CheekyFgCompleteHooklessFrameV2Fn>(GetProcAddress(runtime,"CheekyFG_CompleteHooklessFrameV2")):nullptr;
            if(inference_slot&&(hookless_complete||complete_v2)){
                // Both final stereo colors are now captured. Stereo guides are claimed at the final OpenXR handoff, independent
                // of the native-frame release policy.
                // A first eye may have been copied before the second guide
                // existed. Only zero/unavailable stamps may adopt the native
                // capture's anchor; two disagreeing nonzero stamps still fail.
                if(complete_v2&&!expected_frame&&owner.color_stamp_frame==job->id&&owner.color_stamp_token){
                    expected_token=owner.color_stamp_token;tokens_match=true;
                    for(unsigned eye=0;eye<2;++eye){const auto sc=projection->views[eye].subImage.swapchain;
                        const auto t=owner.capture_tokens[sc],f=owner.capture_frames[sc];
                        tokens_match&=!f&&(!t||t==expected_token);}
                }
                const auto completed=complete_v2?complete_v2(tokens_match?1u:0u,expected_token):hookless_complete(1);
                if(completed){expected_token=completed;expected_frame=0;tokens_match=true;}
                else if(!expected_frame){expected_token=0;tokens_match=false;}
            }
            // When a guide interval changed between xrReleaseSwapchainImage
            // and xrEndFrame, never evaluate FG with the now-wrong packed
            // crop. Full-eye copies are unaffected and remain eligible.
            if(tokens_match&&expected_token)for(unsigned eye=0;eye<2;++eye){
                if(!job->color[eye]||!job->color[eye]->layout.packed())continue;
                const auto sc=projection->views[eye].subImage.swapchain;
                if(owner.capture_crop_tokens[sc]!=expected_token){
                    tokens_match=false;
                    if(owner.trace){
                        char message[224]{};
                        sprintf_s(message,"FG compact capture interval changed eye=%u packed_guide=%llu matched_guide=%llu; skipping FG pair",
                            eye,static_cast<unsigned long long>(owner.capture_crop_tokens[sc]),
                            static_cast<unsigned long long>(expected_token));
                        owner.trace(message);
                    }
                    break;
                }
            }
            auto acquire=owner.guide_acquire?owner.guide_acquire:runtime?reinterpret_cast<CheekyFgAcquireFn>(GetProcAddress(runtime,"CheekyFG_AcquirePair")):nullptr;
            auto release=owner.guide_release?owner.guide_release:runtime?reinterpret_cast<CheekyFgReleaseFn>(GetProcAddress(runtime,"CheekyFG_ReleasePair")):nullptr;
            CheekyFgPair guides;guides.capture_token=expected_token;guides.render_frame=expected_frame;
            CheekyFgColorOptionsV1 color_options;const bool color_only=optical::color_options(color_options);
            const bool acquired=!color_only&&inference_slot&&tokens_match&&expected_token&&
                (external?(guides=*external,true):(acquire&&release&&acquire(&guides)));
            // render_frame is deliberately zero for hook-free mode 3.
            // The nonzero color token still identifies the immutable pair.
            if(inference_slot&&(!tokens_match||!expected_token))job->guide_state=CheekyFgState::guide_frame_mismatch;
            if(acquired) {
                // Hook-free mode 3 can work with a different *verified*
                // native guide queue. Explicit Signal/Wait orders the
                // guide writes before FG reads; same-device alone is NOT
                // sufficient. Other release modes still require one queue.
                auto payload=std::make_shared<FrameGuides12>();
                job->guides_valid=owner.accept_guide_queue(guides,*payload,expected_frame==0);
                if(!job->guides_valid)job->guide_state=CheekyFgState::queue_mismatch;
                if(!cheeky_fg_pair_matches_color(guides,expected_token,expected_frame)){
                    job->guides_valid=false;job->guide_state=CheekyFgState::guide_frame_mismatch;
                }
                for(unsigned eye=0;eye<2&&job->guides_valid;++eye) {
                    auto g=guides.eyes[eye];const auto d=job->color[eye]->texture->GetDesc();
                    // Prepared guides omit textures that neither region's method reads.
                    if(!g.prepared_guides&&(!g.depth||!g.motion)){job->guides_valid=false;job->guide_state=CheekyFgState::generation_failed;break;}
                    job->source_depth=g.depth?unsigned(g.depth->GetDesc().Format):0;job->source_motion=g.motion?unsigned(g.motion->GetDesc().Format):0;
                    const auto& layout=job->color[eye]->layout;
                    if(g.width!=(layout.width?layout.width:d.Width)||g.height!=(layout.height?layout.height:d.Height)){job->guides_valid=false;job->guide_state=CheekyFgState::generation_failed;break;}
                    if(layout.packed()){
                        // Metadata is published on the preceding guide evaluations.
                        // If geometry changed since capture, do not feed NGX a high-
                        // resolution crop partly supplied by low-res context.
                        const auto expected=compact_layout(layout.width,layout.height,true,
                            g.focus_width,g.focus_height,g.focus_x,g.focus_y,
                            external?1u:g.peripheral_percent,layout.feather);
                        if(!expected.packed()||layout.x!=expected.x||layout.y!=expected.y||
                           layout.w!=expected.w||layout.h!=expected.h){
                            job->guides_valid=false;
                            job->guide_state=CheekyFgState::generation_failed;
                            if(owner.trace){
                                char message[250]{};
                                sprintf_s(message,"FG compact capture/guide crop disagreement eye=%u "
                                    "packed=%u,%u,%u,%u matched=%u,%u,%u,%u; skipping FG pair",
                                    eye,layout.x,layout.y,layout.w,layout.h,
                                    expected.x,expected.y,expected.w,expected.h);
                                owner.trace(message);
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
                    payload->retain(guides);job->guides=std::move(payload);job->policy=FrameGuides12::policy_of(guides);job->guide_token=expected_token;
                    job->source_configuration=guides.eyes[0].configuration;job->render_frame=expected_frame;
                }
                if(!external)release(&guides);
            }else {
                if(complete_v2&&expected_token)complete_v2(0,expected_token);
                if(inference_slot&&tokens_match&&expected_token)job->guide_state=CheekyFgState::guide_frame_mismatch;
            }
            if(color_only&&inference_slot){
                auto payload=std::make_shared<FrameGuides12>();
                for(unsigned eye=0;eye<2;++eye){auto g=color_options.eyes[eye];const auto d=job->color[eye]->texture->GetDesc();
                    const auto& layout=job->color[eye]->layout;g.width=layout.width?layout.width:unsigned(d.Width);g.height=layout.height?layout.height:d.Height;
                    payload->pair.eyes[eye]=g;}
                payload->pair.capture_token=job->id*2;payload->pair.fg_queue_priority=color_options.queue_priority;payload->pair.release_point=color_options.release_point;
                job->guides=std::move(payload);job->source_configuration=color_options.eyes[0].configuration;
                job->guide_token=job->id*2;job->render_frame=0;job->guides_valid=true;job->guide_state=CheekyFgState::waiting_for_history;
                job->policy={color_options.release_point,color_options.queue_priority,true};
            }
            {std::lock_guard lock(owner.mx);owner.telemetry.color_token=expected_token;owner.telemetry.guide_token=job->guide_token;
                owner.telemetry.color_render_frame=expected_frame;owner.telemetry.guide_render_frame=job->guides_valid?expected_frame:0;
                if(inference_slot&&job->guide_state==CheekyFgState::guide_frame_mismatch)++owner.telemetry.mismatched_guides;}
            if(job->latency_diagnostics)
                job->final_color_ready=owner.producer.mark_color_ready(*c,LatencyProbe::cpu_now());
            application_timer.end(c->list.Get());
            good=owner.producer.execute(*c,job->ready);
            if(good)application_timer.submitted(job->ready);
            else application_timer.cancel();
            if(!good&&inference_slot&&hookless_complete)hookless_complete(0);
        }else if(c){
            if(inference_slot&&hookless_complete)hookless_complete(0);
            // A changed layout can fail after one eye was recorded. Queue
            // those valid partial transitions so tracked pool states stay
            // consistent before falling back to the original native frame.
            std::uint64_t discarded{};owner.producer.execute(*c,discarded);
        }

        return good?CaptureOutcome::captured:CaptureOutcome::unavailable;
    }
    GenerationOutcome generate(const std::shared_ptr<BufferedNative>& previous,
        const std::shared_ptr<BufferedNative>& job,BufferedPair& pair,
        bool& reset_history,std::uint64_t& logged_samples,
        std::shared_ptr<BufferedDirectRequest>& direct_job) override {
        pair.generated_count=std::clamp(job->multiplier,2u,4u)-1;
        GenerationOutcome total{true,true,false};
        for(unsigned index=0;index<pair.generated_count;++index){
            pair.generated_index=index;
            if(index)pair.generated.swap(pair.extra_generated[index-1]);
            const auto result=generate_one(previous,job,pair,reset_history,logged_samples,direct_job);
            if(index)pair.generated.swap(pair.extra_generated[index-1]);
            total.success&=result.success;total.attempted&=result.attempted;total.history_reset|=result.history_reset;
            if(!result.attempted)break;
        }
        pair.generated_index=0;
        if(!total.success)owner.worker.wait_idle();
        return total;
    }
    GenerationOutcome generate_one(const std::shared_ptr<BufferedNative>& previous,
        const std::shared_ptr<BufferedNative>& job,BufferedPair& pair,
        bool& reset_history,std::uint64_t& logged_samples,
        std::shared_ptr<BufferedDirectRequest>& direct_job) {
        GenerationOutcome outcome;auto& success=outcome.success;auto& attempted=outcome.attempted;auto& history_was_reset=outcome.history_reset;
        const auto* job_guides=guides12(*job);
        if(previous&&previous->epoch==job->epoch&&previous->id+1==job->id&&previous->space==job->space&&job->guides_valid&&job_guides&&
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
            const bool uses_flow=std::any_of(std::begin(job_guides->pair.eyes),std::end(job_guides->pair.eyes),[](const auto& g){
                return cheeky_fg_uses_flow(g.center_method,g.peripheral_method,g.focus_width<.999f||g.focus_height<.999f);
            });
            const unsigned requested_priority=job->policy.queue_priority==1u&&uses_flow?1u:2u;
            if(requested_priority==1u&&!owner.fg_high_attempted){
                owner.fg_high_attempted=true;
                auto desc=owner.queue->GetDesc();desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
                desc.Priority=D3D12_COMMAND_QUEUE_PRIORITY_HIGH;desc.Flags=D3D12_COMMAND_QUEUE_FLAG_NONE;
                const auto hr=owner.device->CreateCommandQueue(&desc,IID_PPV_ARGS(&owner.fg_high_queue));
                if(FAILED(hr)&&owner.trace)owner.trace("Optical-flow HIGH queue unavailable; using game queue");
            }
            ID3D12CommandQueue* requested=requested_priority==1u?owner.fg_high_queue.Get():owner.queue.Get();
            ID3D12CommandQueue* selected=requested?requested:owner.queue.Get();
            if(selected!=owner.generation_queue.Get()) {
                owner.worker.wait_idle();owner.worker.reset();owner.generation_queue=selected;
                owner.generation_high_priority=owner.fg_high_queue.Get()!=nullptr&&selected==owner.fg_high_queue.Get();
                reset_history=true;logged_samples=0;
                {std::lock_guard lock(owner.mx);owner.cadence_window={};owner.gpu_ready_ages={};owner.gpu_ready_samples=owner.predicted_samples=owner.prediction_late=0;
                    owner.generation_stages={};owner.capture_to_completion={};owner.completion_to_native={};owner.completion_to_generated={};
                    owner.frame_ages={};owner.age_samples={};owner.pacing_wait={};owner.producer_cpu={};}
                if(owner.trace)owner.trace(owner.generation_high_priority?"DLSS-FG queue mode=HIGH (Lua; NGX histories reset)":
                    "DLSS-FG queue mode=shared game queue (Lua; NGX histories reset)");
            }
            {std::lock_guard lock(owner.mx);owner.reported_queue_mode=selected==owner.queue.Get()?2u:1u;}
            if(owner.last_fg_priority_requested!=requested_priority) {
                owner.last_fg_priority_requested=requested_priority;
                if(owner.trace) {
                    char msg[176]{};
                    sprintf_s(msg,"DLSS-FG queue request=%s selected=%s fallback=%u",
                        owner.queue_mode_name(requested_priority),
                        owner.generation_queue.Get()==owner.fg_high_queue.Get()&&owner.fg_high_queue.Get()?"HIGH":
                        "game queue",
                        requested_priority==1u&&selected==owner.queue.Get()?1u:0u);
                    owner.trace(msg);
                }
            }
            auto d=job->color[0]->texture->GetDesc();owner.worker.trace=owner.trace;owner.worker.input_color_state=D3D12_RESOURCE_STATE_GENERIC_READ;
            const auto& color_layout=job->color[0]->layout;
            if(owner.worker.initialize(owner.device.Get(),owner.generation_queue.Get(),color_layout.width?color_layout.width:unsigned(d.Width),color_layout.height?color_layout.height:d.Height,d.Format,cheeky_fg_needs_ngx(job_guides->pair))) {
                for(unsigned eye=0;eye<2;++eye){owner.worker.color_layouts[eye]=job->color[eye]->layout;owner.worker.previous_layouts[eye]=previous->color[eye]->layout;}
                // The OpenXR capture has its own producer.fence, but it
                // does not cover guide writes on a DIFFERENT game queue.
                // Bridge that dependency before DLSS-G prepares the guides.
                const auto guides_ready=job_guides->sync_value ?
                    owner.generation_queue->Wait(job_guides->sync_fence.Get(),job_guides->sync_value) : S_OK;
                if(FAILED(guides_ready)){owner.worker.failed=true;owner.worker.report("FG guide cross-queue GPU wait",unsigned(guides_ready));
                    throw std::runtime_error("FG guide cross-queue GPU wait failed");}
                const bool queued_ngx=!pair.direct_xr&&Worker::can_batch_ngx(job_guides->pair);
                const bool continuing=pair.generated_index>0&&
                    (!cheeky_fg_needs_ngx(job_guides->pair)||queued_ngx);
                // The first phase already submitted this capture dependency.
                // Preserve its calibrated latency bracket across the batch.
                const auto ready_result=continuing?S_OK:owner.worker.latency.enqueue_wait(owner.device.Get(),owner.generation_queue.Get(),owner.producer.fence.Get(),job->ready,job->latency_diagnostics);
                if(FAILED(ready_result)){owner.worker.failed=true;owner.worker.report("FG capture dependency queue wait",unsigned(ready_result));}
                if(FAILED(ready_result))throw std::runtime_error("FG capture dependency queue wait failed");
                auto guides=job_guides->pair;guides.producer_queue=owner.generation_queue.Get();
                const bool token_gap=previous->guide_token+2!=job->guide_token;
                if(token_gap)reset_history=true;
                // Hook-free mode 3 deliberately has no native-frame ID.
                // Preserve history when the capture tokens are consecutive;
                // reset on any transition between tagged and untagged mode.
                const bool frame_gap=(previous->render_frame||job->render_frame)&&previous->render_frame+1!=job->render_frame;
                if(frame_gap)reset_history=true;
                const bool source_reset=guides.eyes[0].reset||guides.eyes[1].reset;
                history_was_reset=reset_history||source_reset;
                if(history_was_reset&&owner.trace){const auto count=++owner.history_reset_events;
                    if(count<=4||(count&(count-1))==0){char message[240]{};
                        sprintf_s(message,"FG HISTORY reset count=%llu frame=%llu worker_restart=%u token_gap=%u frame_gap=%u source_reset=%u",
                            count,job->render_frame,unsigned(reset_history&&!token_gap&&!frame_gap),unsigned(token_gap),unsigned(frame_gap),unsigned(source_reset));owner.trace(message);}}
                if(reset_history)for(auto& eye:guides.eyes)eye.reset=1;
                if(owner.trace && ((++owner.pose_sample_count%60u)==1u)) {
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
                            std::chrono::duration<double,std::milli>(job->captured_at-previous->captured_at).count(),std::sqrt(dx*dx+dy*dy+dz*dz),
                            angle(a.orientation,b.orientation),b.position.x,b.position.y,b.position.z);
                        owner.trace(message);
                    }
                }
                std::array<Camera,2> cameras;for(unsigned eye=0;eye<2;++eye)
                    cameras[eye]=camera(job->views[eye],&previous->views[eye],guides.eyes[eye].depth_inverted!=0);
                const bool camera_pair_valid=std::all_of(cameras.begin(),cameras.end(),[](const auto& c){return c.valid;});

                std::array<ID3D12Resource*,2> input{job->color[0]->texture.Get(),job->color[1]->texture.Get()},
                    prior{previous->color[0]->texture.Get(),previous->color[1]->texture.Get()},result{};
                auto publication=d;publication.Format=owner.worker.focus.color_format;
                std::array<Worker::OutputCopy,2> copies;
                bool available=true;
                // Debug border is drawn by the final OpenXR upload. Avoid
                // bypassing that upload via experimental direct XR output.
                const bool border_upload=std::any_of(std::begin(guides.eyes),std::end(guides.eyes),[](const auto& g){return g.show_foveation_border!=0;});
                const bool blocked_direct=job->direct_output_mode&&(border_upload||owner.direct_blocked_target==owner.direct_target_key(owner.worker.width,owner.worker.height,owner.worker.focus.color_format));
                if(blocked_direct){std::lock_guard lock(owner.mx);++owner.direct_stats.fallbacks;++owner.direct_stats.unsupported;owner.direct_stats.last_path=3;}
                if(camera_pair_valid&&pair.generated_count==1&&job->direct_output_mode&&!blocked_direct){
                    const bool supported=std::all_of(std::begin(guides.eyes),std::end(guides.eyes),[](const auto& g){
                        return (g.peripheral_method<=2||cheeky_fg_optical(g.peripheral_method))&&(g.focus_width<.999f||g.focus_height<.999f);});
                    if(supported){
                        direct_job=std::make_shared<DirectRequest>();direct_job->epoch=job->epoch;direct_job->native_id=previous->id;
                        direct_job->previous=previous->color;direct_job->format=owner.worker.focus.color_format;direct_job->width=owner.worker.width;direct_job->height=owner.worker.height;
                        std::unique_lock lock(owner.mx);++owner.direct_stats.attempts;owner.direct_request=direct_job;owner.changed.notify_all();
                        owner.changed.wait(lock,[&]{return owner.stopping||job->epoch!=owner.epoch||direct_job->decided;});
                        if(direct_job->allowed){copies=direct_job->targets;pair.direct_xr=true;
                            for(unsigned eye=0;eye<2;++eye){pair.generated[eye]=std::make_shared<TransportImage>();
                                pair.generated[eye]->texture=copies[eye].texture;pair.generated[eye]->state=copies[eye].state;}}
                    }else{std::lock_guard lock(owner.mx);++owner.direct_stats.unsupported;++owner.direct_stats.fallbacks;owner.direct_stats.last_path=3;
                        owner.note_direct_failure_locked(CheekyFgDirectReason::configuration);}
                }
                for(unsigned eye=0;eye<2&&!pair.direct_xr&&camera_pair_valid;++eye) {
                    const auto& g=guides.eyes[eye];
                    const auto layout=compact_layout(owner.worker.width,owner.worker.height,job->color[eye]->layout.packed(),g.focus_width,g.focus_height,
                        g.focus_x,g.focus_y,g.peripheral_percent,g.focus_feather,g.resolution_percent);
                    publication.Width=layout.stored_width();publication.Height=layout.stored_height();
                    pair.generated[eye]=owner.output.image(publication,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                    if(!pair.generated[eye]){available=false;break;}
                    pair.generated[eye]->layout=layout;
                    pair.generated[eye]->fg_border=fg_border_rect(owner.worker.width,owner.worker.height,g.focus_width,g.focus_height,
                        g.focus_x,g.focus_y,g.show_foveation_border!=0);
                    copies[eye]={pair.generated[eye]->texture.Get(),pair.generated[eye]->state,layout};
                }
                if(available&&camera_pair_valid) {
                    const bool last=pair.generated_index+1==pair.generated_count;
                    bool announced=false;
                    auto publish_state=[&] {
                        pair.submitted_at=owner.worker.submitted_at;
                        for(unsigned eye=0;eye<2;++eye){
                            pair.generated[eye]->state=copies[eye].state;
                            pair.generated[eye]->presentation_format=srgb_color(d.Format)&&publication.Format==DXGI_FORMAT_R8G8B8A8_UNORM?
                                DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:publication.Format;
                        }
                    };
                    // Each generated phase hands over its immutable stereo output
                    // before retirement. Eligible multi-frame intervals queue
                    // every index before the final CPU completion wait.
                    // Shared-queue uploads can follow these submitted commands
                    // immediately. Separate queues retain the readiness check.
                    auto announce=[&]() noexcept {
                        publish_state();
                        if(pair.direct_xr){
                            std::lock_guard lock(owner.mx);
                            direct_job->fence=owner.worker.fence;direct_job->serial=owner.worker.serial;
                            direct_job->queued=true;direct_job->success=!owner.stopping&&!owner.degraded&&job->epoch==owner.epoch;
                            if(direct_job->success){
                                auto published=pair;published.interpolated=true;
                                published.native_id=previous->id;published.native=previous->color;
                                published.native_color_ready=previous->color_ready;published.native_final_ready=previous->final_color_ready;
                                published.native_captured=previous->captured_at;published.native_views=previous->views;
                                published.generated_views=BufferedPresenter::midpoint(previous->views,job->views,published.fraction());
                                direct_job->result=std::move(published);++owner.direct_stats.frames;
                                owner.arm_generated_gate(job->id);
                            }
                            for(const auto& f:owner.worker.features)if(f.direct)++owner.direct_stats.ngx_direct_eyes;else ++owner.direct_stats.ngx_scratch_eyes;
                            owner.direct_stats.last_path=owner.worker.features[0].direct&&owner.worker.features[1].direct?1u:2u;
                            direct_job->complete=true;owner.changed.notify_all();
                        }else owner.announce_generated_phase(previous,job,pair,
                            std::make_shared<FrameCompletion12>(owner.worker.fence.Get(),owner.worker.serial,owner.worker.queue.Get()));
                        announced=true;
                    };
                    success=owner.worker.generate(guides,input,prior,cameras,result,&copies,last?+[](void* context) noexcept {
                        static_cast<BufferedPresenter*>(context)->inference_submitted();
                    }:nullptr,&owner,last?+[](void* context) noexcept {
                        static_cast<BufferedPresenter*>(context)->inference_prepared();
                    }:nullptr,pair.generated_count,pair.generated_index+1,+[](void* context) noexcept {
                        (*static_cast<decltype(announce)*>(context))();
                    },&announce);attempted=!owner.worker.failed&&owner.worker.temporal_evaluation_completed;
                    // Once announced, images/fence/format are immutable to the
                    // presenter; do not rewrite shared metadata after its wake.
                    if(!announced)publish_state();
                    if(pair.direct_xr&&!announced){std::lock_guard lock(owner.mx);
                        direct_job->fence=owner.worker.fence;direct_job->serial=owner.worker.serial;
                        for(const auto& f:owner.worker.features)if(f.direct)++owner.direct_stats.ngx_direct_eyes;else ++owner.direct_stats.ngx_scratch_eyes;
                        owner.direct_stats.last_path=owner.worker.features[0].direct&&owner.worker.features[1].direct?1u:2u;}
                }
            }
        }

        return outcome;
    }
    bool publish(const BufferedPair& pair,const std::array<Image,2>& images,
        std::array<XrCompositionLayerProjectionView,2>& views,FrameLayers& layers,bool generated) override {
        std::vector<std::pair<Chain*,Image>> batch;
        for(unsigned eye=0;eye<2;++eye) {
            auto& chain=owner.scene_chains[eye];
            if(!(generated&&pair.direct_xr)){if(!owner.ensure_chain(chain,images[eye],true))return false;batch.push_back({&chain,images[eye]});}
            views[eye].type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;views[eye].next=nullptr;
            views[eye].subImage={chain.handle,{{0,0},{int(chain.desc.Width),int(chain.desc.Height)}},0};
        }
        for(auto& a:pair.auxiliary){
            auto& chain=owner.auxiliary_chains[a.source];if(!owner.ensure_chain(chain,a.image))return false;batch.push_back({&chain,a.image});
        }
        if(!owner.upload_batch(batch))return false;
        owner.uploads.close_profile_frame(owner.ticks);
        if(!owner.subimages(layers,[&](XrSwapchainSubImage& sub){const auto i=owner.auxiliary_chains.find(sub.swapchain);
            if(i==owner.auxiliary_chains.end())return false;sub.swapchain=i->second.handle;return true;}))return false;
        return true;
    }
    void retire() override {
        owner.producer.drain();owner.output.drain();owner.uploads.drain();owner.worker.wait_idle();
        application_timer.reset(owner.producer);
    }
    void discard_captures(bool session) override {
        owner.direct_captures.clear();if(session){owner.capture_plans.clear();owner.captured_descs.clear();}
    }
    void finish_generation() override {owner.worker.wait_idle();owner.output.drain();}
    void reset_generation() override {owner.worker.reset();owner.worker.failed=false;}
    void retire_guides(BufferedNative& frame) override {
        if(!owner.worker.fence||owner.worker.fence->GetCompletedValue()>=owner.worker.serial)frame.clear_guides();
    }
    void reset_statistics(bool reset_gpu) override {
        auto& worker=owner.worker;worker.latency.clear_statistics();
        if(reset_gpu&&worker.device&&worker.queue){worker.wait_idle();worker.initialize_timings();}
    }
    GenerationStatus generation_status() const override {
        const auto& worker=owner.worker;return {worker.failed,worker.capability_ready,worker.serial,worker.diagnostic};
    }
    void snapshot(CheekyFgPerformance& performance,CheekyFgOverhead& overhead,
        CheekyFgExperimentDiagnostics& experiments) const override {
        performance=owner.Presenter::performance();owner.worker.latency.snapshot(experiments);
        owner.batch_telemetry=owner.worker.batch_timings.snapshot();
        performance.inference_color_format=unsigned(owner.worker.focus.color_format);
        performance.shared_gpu_queue=owner.generation_queue.Get()==owner.queue.Get()?1u:0u;
        if(owner.worker.gpu_timings.valid){
            overhead.publication_samples=owner.worker.gpu_timings.samples;
            overhead.publication_ms=owner.worker.gpu_timings.publication.mean();
            overhead.publication_last_ms=owner.worker.gpu_timings.publication.last;
        }
    }
    XrResult end_frame(const XrFrameEndInfo* end) override {
        return owner.runtime_dispatch([&]{return owner.xr.end_frame(owner.session,end);});
    }
    bool service_direct_output(double position,BufferedPair& result) override {return owner.service_direct_output(position,result);}
    bool supports_direct_output() const override {return !owner.interop11;}
};

}
