#pragma once

namespace buffered_probe {
using Microsoft::WRL::ComPtr;
struct Runtime {
    static inline ID3D12Device* device{};
    static inline std::unordered_map<XrSwapchain,ComPtr<ID3D12Resource>> chains;
    static inline std::atomic<unsigned> waits{},begins{},ends{},scenes{},generated{},raw_frames{},errors{};
    static inline std::atomic<unsigned> age_logs{};
    static inline std::atomic<unsigned> path_logs{};
    static inline std::atomic<bool> paused{},reject_private{};
    static inline DWORD owner{};
    static inline unsigned phase{},next_chain{};
    static inline ID3D12Resource* last_native{};
    static inline const std::uint64_t* upload_serial{};
    static inline std::uint64_t required_upload_serial{};
    static inline unsigned image_releases{};
    static inline CheekyFgPair guides;
    static inline CheekyFgTransportOptions options;
    static inline std::uint64_t predicted_qpc{};
    static inline std::atomic<bool> block_generated_end{},generated_end_entered{},allow_generated_end{};
    static bool __cdecl transport_options(CheekyFgTransportOptions* out){*out=options;return true;}
    static inline std::uint64_t capture_token{};
    static inline cheeky::foveated_dlss::FgSnapshots* snapshots{};
    static XrResult XRAPI_PTR wait(XrSession,const XrFrameWaitInfo*,XrFrameState* state) {
        Sleep(8);while(paused)Sleep(1);if(!owner)owner=GetCurrentThreadId();if(owner!=GetCurrentThreadId()||phase)++errors;
        phase=1;const auto tick=++waits;state->shouldRender=XR_TRUE;
        predicted_qpc=cheeky::fg::LatencyProbe::cpu_now()+std::uint64_t(cheeky::fg::LatencyProbe::cpu_frequency()/120.);
        state->predictedDisplayTime=XrTime(tick)*8333333;state->predictedDisplayPeriod=8333333;return XR_SUCCESS;
    }
    static XrResult XRAPI_PTR begin(XrSession,const XrFrameBeginInfo*) {
        if(owner!=GetCurrentThreadId()||phase!=1)++errors;phase=2;++begins;return XR_SUCCESS;
    }
    static XrResult XRAPI_PTR end(XrSession,const XrFrameEndInfo* frame) {
        if(owner!=GetCurrentThreadId()||phase!=2)++errors;phase=0;++ends;
        if(frame->layerCount) {
            if(frame->layerCount!=3){++errors;return XR_ERROR_LAYER_INVALID;}
            const auto* scene=reinterpret_cast<const XrCompositionLayerProjection*>(frame->layers[1]);
            const auto* quad=reinterpret_cast<const XrCompositionLayerQuad*>(frame->layers[2]);
            const auto sc=scene->views[0].subImage.swapchain;
            if(sc==h<XrSwapchain>(200)){++raw_frames;return XR_SUCCESS;}
            if(!chains.contains(sc)||quad->size.width!=1||quad->subImage.swapchain==h<XrSwapchain>(201))++errors;
            else {
                ++scenes;auto* tex=chains.at(sc).Get();
                if(tex->GetDesc().Format==DXGI_FORMAT_R16G16B16A16_FLOAT||tex->GetDesc().Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB){
                    ++generated;if(block_generated_end){generated_end_entered=true;while(!allow_generated_end)Sleep(1);}
                }
                else last_native=tex;
            }
        }
        return XR_SUCCESS;
    }
    static XrResult XRAPI_PTR create(XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* handle) {
        if(reject_private)return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=info->width;d.Height=info->height;
        d.DepthOrArraySize=UINT16(info->arraySize);d.MipLevels=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT(info->format);
        d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> texture;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&texture))))return XR_ERROR_RUNTIME_FAILURE;
        *handle=h<XrSwapchain>(1000+next_chain++);chains[*handle]=std::move(texture);return XR_SUCCESS;
    }
    static XrResult XRAPI_PTR enumerate(XrSwapchain sc,unsigned capacity,unsigned* count,XrSwapchainImageBaseHeader* data) {
        *count=1;if(capacity)reinterpret_cast<XrSwapchainImageD3D12KHR*>(data)[0].texture=chains.at(sc).Get();return XR_SUCCESS;
    }
    static std::uint64_t __cdecl token(ID3D12Resource*){return snapshots?snapshots->token():capture_token;}
    static bool __cdecl stamp(ID3D12Resource*,CheekyFgCaptureStamp* out){
        out->render_frame=capture_token/2;out->token=snapshots?snapshots->token(out->render_frame):capture_token;return true;
    }
    static bool __cdecl missing_stamp(ID3D12Resource*,CheekyFgCaptureStamp* out){out->token=out->render_frame=0;return true;}
    static bool __cdecl acquire_guides(CheekyFgPair* out) {
        if(snapshots){
            const bool acquired=snapshots->acquire(*out);
            if(acquired){out->release_point=guides.release_point;out->submit_repeats=guides.submit_repeats;
                out->fg_queue_priority=guides.fg_queue_priority;}
            return acquired;
        }
        const auto expected=out->capture_token;*out=guides;out->capture_token=expected;
        out->render_frame=expected/2;out->eyes[0].evaluation=expected-1;out->eyes[1].evaluation=expected;
        for(auto& eye:out->eyes)eye.render_frame=out->render_frame;return true;
    }
    static void __cdecl release_guides(CheekyFgPair* pair) {
        if(pair->lease){for(auto& eye:pair->eyes){eye.depth->Release();eye.motion->Release();}pair->producer_queue->Release();pair->lease->Release();}
    }
    static cheeky::fg::XrFns functions() {
        cheeky::fg::XrFns f;f.wait_frame=wait;f.begin_frame=begin;f.end_frame=end;f.create=create;f.enumerate=enumerate;
        f.time_instance=h<XrInstance>(1);f.time_to_qpc=[](XrInstance,XrTime,LARGE_INTEGER* qpc){qpc->QuadPart=predicted_qpc;return XR_SUCCESS;};
        f.destroy=[](XrSwapchain sc){chains.erase(sc);return XR_SUCCESS;};
        f.acquire=[](XrSwapchain,const XrSwapchainImageAcquireInfo*,unsigned* i){*i=0;return XR_SUCCESS;};
        f.wait_image=[](XrSwapchain,const XrSwapchainImageWaitInfo*){return XR_SUCCESS;};
        f.release=[](XrSwapchain,const XrSwapchainImageReleaseInfo*){
            if(upload_serial&&*upload_serial<required_upload_serial)++errors;
            ++image_releases;return XR_SUCCESS;};return f;
    }
    static void trace(const char* message) noexcept {
        if(std::strstr(message,"FG calibrated latency"))std::cout<<message<<'\n';
        if(std::strstr(message,"FG source-age CPU ms:")){++age_logs;std::cout<<message<<'\n';}
        if(std::strstr(message,"FG capture-path CPU ms:")){++path_logs;std::cout<<message<<'\n';}
    }
};
}

void fg_repeat_upload_validation(ID3D12Device* device) {
    using namespace buffered_probe;using Microsoft::WRL::ComPtr;
    Runtime::device=device;
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};
    check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"Repeat queue");
    cheeky::fg::BufferedPresenter p;check(p.setup(h<XrSession>(3),Runtime::functions(),device,queue.Get()),"Repeat presenter");
    // HIGH priority is a property of the GPU command queue, not the CPU
    // generation thread. On supported devices, only FG uses the new queue;
    // all captures and runtime-visible uploads remain on the original queue.
    check(p.generation_queue.Get()!=nullptr,"FG GPU queue selected");
    check(p.producer.queue.Get()==queue.Get()&&p.uploads.queue.Get()==queue.Get(),
        "Capture and OpenXR uploads retain the game GPU queue");
    if(p.generation_high_priority) {
        const auto desc=p.generation_queue->GetDesc();
        check(p.generation_queue.Get()!=queue.Get()&&desc.Type==D3D12_COMMAND_LIST_TYPE_DIRECT&&
            desc.Priority==D3D12_COMMAND_QUEUE_PRIORITY_HIGH&&desc.NodeMask==queue->GetDesc().NodeMask,
            "FG executes on a separate HIGH-priority direct queue on the same GPU node");
    } else check(p.generation_queue.Get()==(p.fg_normal_queue?p.fg_normal_queue.Get():queue.Get()),
        "Unsupported HIGH priority uses separate NORMAL before the shared fallback");
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
    check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))&&
        SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands))),"Repeat commands");
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
    d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=16;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> source;check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,
        D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&source))),"Repeat source");
    ComPtr<ID3D12DescriptorHeap> rtv;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
    check(SUCCEEDED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv))),"Repeat RTV");
    device->CreateRenderTargetView(source.Get(),nullptr,rtv->GetCPUDescriptorHandleForHeapStart());
    auto execute=[&]{check(SUCCEEDED(commands->Close()),"Repeat close");ID3D12CommandList* lists[]{commands.Get()};queue->ExecuteCommandLists(1,lists);
        // The test alone waits for readback/reset. Production repeat handling
        // must not submit GPU work or wait for allocator retirement.
        p.uploads.drain();ComPtr<ID3D12Fence> fence;check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))),"Repeat fence");
        check(SUCCEEDED(queue->Signal(fence.Get(),1)),"Repeat signal");HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        check(event&&SUCCEEDED(fence->SetEventOnCompletion(1,event))&&WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"Repeat wait");CloseHandle(event);
        check(SUCCEEDED(allocator->Reset())&&SUCCEEDED(commands->Reset(allocator.Get(),nullptr)),"Repeat reset");};
    const float red[]{1,0,0,1},blue[]{0,0,1,1};commands->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(),red,0,nullptr);execute();
    auto snapshot=[&]{auto* c=p.producer.begin();check(c!=nullptr,"Repeat snapshot commands");
        auto image=p.producer.copy(*c,source.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);check(bool(image),"Repeat snapshot");
        p.producer.freeze(*c,image);std::uint64_t ticket{};check(p.producer.execute(*c,ticket),"Repeat snapshot submit");return image;};
    auto old=snapshot();cheeky::fg::BufferedPresenter::Chain chain;
    // The cross-queue wait must retire only after the native snapshot signal.
    // This also tests the harmless same-queue fallback on devices without HIGH.
    ComPtr<ID3D12Fence> fg_ready;
    check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fg_ready))),"FG queue fence");
    check(SUCCEEDED(p.generation_queue->Wait(p.producer.fence.Get(),p.producer.serial))&&
        SUCCEEDED(p.generation_queue->Signal(fg_ready.Get(),1)),"FG GPU wait and signal");
    HANDLE fg_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    check(fg_event&&SUCCEEDED(fg_ready->SetEventOnCompletion(1,fg_event))&&
        WaitForSingleObject(fg_event,10000)==WAIT_OBJECT_0,"FG queue GPU fence retirement");
    CloseHandle(fg_event);
    check(p.upload_image(chain,old),"First image upload");const auto first=p.uploads.serial;
    for(unsigned i=0;i<8;++i)check(p.upload_image(chain,old),"Repeat last released image");
    check(p.uploads.serial==first,"Repeated presentations must not enqueue another GPU image upload");
    commands->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(),blue,0,nullptr);execute();
    auto read_pixel=[&](unsigned b,unsigned r){
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};
        const auto actual=chain.images[0].texture->GetDesc();
        device->GetCopyableFootprints(&actual,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;
        bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES read_heap{};read_heap.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> rb;
        check(SUCCEEDED(device->CreateCommittedResource(&read_heap,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&rb))),"Repeat readback");
        auto* texture=chain.images[0].texture;
        cheeky::fg::color_transition(commands.Get(),texture,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=texture;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=rb.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        cheeky::fg::color_transition(commands.Get(),texture,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);execute();
        void* data{};check(SUCCEEDED(rb->Map(0,nullptr,&data)),"Repeat pixel map");const auto* pixel=static_cast<unsigned char*>(data);
        const bool good=pixel[0]==b&&pixel[1]==0&&pixel[2]==r;rb->Unmap(0,nullptr);check(good,"Repeated/new uploads must preserve the correct actual pixels");};
    check(p.upload_image(chain,old)&&p.uploads.serial==first,"Reusing the engine texture must not change its old immutable presentation");read_pixel(0,255);
    auto next=snapshot();check(next!=old&&p.upload_image(chain,next)&&p.uploads.serial==first+1,"New content on the same engine texture must upload again");
    check(p.upload_image(chain,next)&&p.uploads.serial==first+1,"New image's repeats must also avoid GPU uploads");read_pixel(255,0);

    // Exercise the exact publication recorder used inside NGX's command list,
    // with distinct stereo pixels and destinations that have already been used.
    auto* capture=p.producer.begin();check(capture!=nullptr,"Publication source context");
    auto generated_red=p.producer.copy(*capture,old->texture.Get(),old->state);
    auto generated_blue=p.producer.copy(*capture,next->texture.Get(),next->state);
    std::uint64_t capture_ready{};
    check(generated_red&&generated_blue&&p.producer.execute(*capture,capture_ready),"Publication source snapshots");
    std::array<cheeky::fg::Image,2> published{p.output.image(d),p.output.image(d)};
    check(published[0]&&published[1]&&published[0]!=published[1],"Distinct immutable stereo publication targets");
    std::array<ID3D12Resource*,2> generated{generated_red->texture.Get(),generated_blue->texture.Get()};
    std::array<cheeky::fg::Worker::OutputCopy,2> copies;
    for(unsigned eye=0;eye<2;++eye)copies[eye]={published[eye]->texture.Get(),published[eye]->state};
    auto aliased=copies;aliased[1].texture=generated[0];
    check(!cheeky::fg::Worker::record_output_copies(commands.Get(),generated,aliased),"Reject an aliased publication before recording either eye");
    check(cheeky::fg::Worker::record_output_copies(commands.Get(),generated,copies),"Record stereo publication with inference commands");execute();
    for(auto& image:published)image->state=D3D12_RESOURCE_STATE_GENERIC_READ;
    check(p.upload_image(chain,published[0]),"Published left eye");read_pixel(0,255);
    check(p.upload_image(chain,published[1]),"Published right eye");read_pixel(255,0);
    // Drop the runtime's source pin before simulating reuse of retired output
    // allocations. Live/leased immutable publication images are never rewritten.
    p.uploads.drain();p.xr.destroy(chain.handle);chain={};
    std::swap(generated[0],generated[1]);
    for(unsigned eye=0;eye<2;++eye)copies[eye].state=published[eye]->state;
    check(cheeky::fg::Worker::record_output_copies(commands.Get(),generated,copies),"Reuse retired GENERIC_READ publication allocations");execute();
    check(p.upload_image(chain,published[0]),"Reused left publication");read_pixel(255,0);
    check(p.upload_image(chain,published[1]),"Reused right publication");read_pixel(0,255);
    // Batch both eyes and UI, then change just one image. Unchanged content
    // must issue neither copies nor a new ExecuteCommandLists call.
    cheeky::fg::BufferedPresenter::Chain right_chain,ui_chain;
    auto before=p.uploads.serial;
    Runtime::upload_serial=&p.uploads.serial;Runtime::required_upload_serial=before+1;
    const auto released_before=Runtime::image_releases;
    check(p.upload_batch({{&chain,old},{&right_chain,next},{&ui_chain,old}})&&p.uploads.serial==before+1,
        "Stereo and UI uploads share exactly one GPU batch");read_pixel(0,255);
    check(Runtime::errors==0&&Runtime::image_releases==released_before+3,"All copies are submitted before each acquired stereo/UI image is released");
    Runtime::upload_serial=nullptr;Runtime::required_upload_serial=0;
    std::swap(chain,right_chain);read_pixel(255,0);std::swap(chain,right_chain);
    before=p.uploads.serial;
    check(p.upload_batch({{&chain,old},{&right_chain,next},{&ui_chain,old}})&&p.uploads.serial==before,
        "Unchanged batched stereo/UI issues no GPU work");
    check(p.upload_batch({{&chain,next},{&right_chain,next},{&ui_chain,old}})&&p.uploads.serial==before+1,
        "One changed image creates only one upload batch");read_pixel(255,0);
    p.uploads.drain();p.xr.destroy(right_chain.handle);p.xr.destroy(ui_chain.handle);right_chain={};ui_chain={};

    // The game really uses side-by-side color. Copy distinct halves directly
    // from its reused render target and verify both eye pixels on the GPU.
    const D3D12_RECT left_rect{0,0,8,16},right_rect{8,0,16,16};
    commands->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(),red,1,&left_rect);
    commands->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(),blue,1,&right_rect);execute();
    const auto scene=h<XrSwapchain>(500);
    cheeky::fg::BufferedPresenter::CapturePlan plan;plan.desc=source->GetDesc();
    plan.slices={{{{0,0},{8,16}},0},{{{8,0},{8,16}},0}};p.capture_plans[scene]=plan;p.color_capture_mode=1;
    const auto full_before=p.producer.full_copies.load();
    check(p.capture(scene,0,source.Get(),16,16,1,DXGI_FORMAT_B8G8R8A8_UNORM),"Direct side-by-side capture");
    p.producer.drain();check(p.capture_ready.at(scene)&&p.capture_ready.at(scene)->ready_qpc>0,
        "Existing capture GPU timestamp resolves into calibrated QPC without an extra GPU query");
    auto eyes=p.direct_captures.at(scene).eyes;
    check(eyes.size()==2&&p.direct_eye_copies==2&&p.producer.full_copies==full_before&&
        eyes[0].second->texture->GetDesc().Width==8,"Direct eyes eliminate the full color snapshot");
    check(p.upload_image(chain,eyes[0].second),"Direct left upload");read_pixel(0,255);
    check(p.upload_image(chain,eyes[1].second),"Direct right upload");read_pixel(255,0);
    commands->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(),red,0,nullptr);execute();
    check(p.upload_image(chain,eyes[1].second),"Immutable direct capture survives engine reuse");read_pixel(255,0);
    p.capture_plans[scene].desc.Width=99;
    check(p.capture(scene,0,source.Get(),16,16,1,DXGI_FORMAT_B8G8R8A8_UNORM)&&
        !p.direct_captures.contains(scene)&&p.captured.contains(scene)&&p.capture_fallbacks>0,
        "Changed source layout safely falls back to the full snapshot");
    commands->Close();p.xr.destroy(chain.handle);chain={};old.reset();next.reset();p.reset();
    check(Runtime::chains.empty(),"Repeat test must release private swapchains and source leases");
    std::cout<<"PASS repeat upload reuse: unchanged images enqueue zero copies, reused engine source preserves old red/new blue GPU pixels\n";
    std::cout<<"PASS inference-batch publication: distinct red/blue stereo output pixels, source restoration, retired allocation reuse and alias rejection\n";
    std::cout<<"PASS direct side-by-side eye capture: distinct pixels, no full snapshot, immutable engine reuse and layout fallback; batched stereo/UI uploads and unchanged-image skipping\n";
}

void fg_latency_validation(ID3D12Device* device) {
    using Microsoft::WRL::ComPtr;using cheeky::fg::LatencyProbe;
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC desc{};
    check(SUCCEEDED(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue))),"Latency test queue");
    LatencyProbe probe;check(probe.initialize(device,queue.Get()),"Latency query resources");
    ComPtr<ID3D12Fence> dependency,completion;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&dependency)))&&
        SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&completion)))&&
        SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))&&
        SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))),"Latency test objects");
    for(unsigned pass=0;pass<2;++pass){
        if(pass){check(SUCCEEDED(allocator->Reset())&&SUCCEEDED(list->Reset(allocator.Get(),nullptr)),"Latency test reset");probe.clear_statistics();}
        // Simulate an upstream render dependency, and independently simulate
        // delay before the worker observes an already completed GPU fence.
        std::thread release;
        if(!pass)release=std::thread([dependency]{Sleep(80);dependency->Signal(1);});
        const auto result=probe.enqueue_wait(device,queue.Get(),dependency.Get(),1,true);
        if(release.joinable())release.join();
        check(SUCCEEDED(result)&&probe.active,"Calibrated probe around producer dependency");
        probe.begin(list.Get());probe.end(list.Get());check(SUCCEEDED(list->Close()),"Latency work close");
        ID3D12CommandList* commands[]{list.Get()};queue->ExecuteCommandLists(1,commands);
        check(SUCCEEDED(queue->Signal(completion.Get(),pass+1)),"Latency final signal");probe.submitted(completion.Get(),pass+1);
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        const bool complete=event&&SUCCEEDED(completion->SetEventOnCompletion(pass+1,event))&&WaitForSingleObject(event,10000)==WAIT_OBJECT_0;
        if(event)CloseHandle(event);check(complete,"Latency final fence");if(pass)Sleep(20);
        const auto observed=LatencyProbe::cpu_now();check(probe.collect(queue.Get(),observed),"Valid GPU/QPC calibration");
        CheekyFgExperimentDiagnostics timing;probe.snapshot(timing);
        const double sum=timing.queue_entry.last+timing.capture_dependency.last+timing.ready_to_fg.last+timing.gpu_work.last+timing.gpu_to_observed.last;
        check(timing.latency_samples==1&&std::abs(sum-timing.enqueue_to_observed.last)<.25,
            "Calibrated stages account for the same end-to-end interval");
        if(!pass)check(timing.capture_dependency.last>50&&timing.gpu_work.last<20,
            "Upstream GPU dependency is separated from actual FG work");
        else check(timing.gpu_to_observed.last>=15&&timing.capture_dependency.last<10,
            "CPU observation delay is separated from capture dependency");
        std::cout<<"PASS calibrated timeline: dependency="<<timing.capture_dependency.last<<" GPU="<<timing.gpu_work.last
            <<" observed="<<timing.gpu_to_observed.last<<" total="<<timing.enqueue_to_observed.last<<" ms\n";
    }
    check(SUCCEEDED(probe.enqueue_wait(device,queue.Get(),dependency.Get(),1,false))&&!probe.active&&!probe.enabled,
        "Diagnostics OFF retains only the original GPU dependency wait");probe.reset();
}

void fg_presentation_timing_validation(ID3D12Device* device) {
    using P=cheeky::fg::BufferedPresenter;using Clock=cheeky::fg::TimingClock;
    using namespace std::chrono;
    const auto now=Clock::now();
    {
        P corrected;P::Pair sampled;const auto cpu=cheeky::fg::LatencyProbe::cpu_now();
        const auto tick=std::uint64_t(cheeky::fg::LatencyProbe::cpu_frequency()/1000.);
        for(auto& stamp:sampled.current_color_ready){stamp=std::make_shared<cheeky::fg::ColorReadyStamp>();
            stamp->captured_cpu=cpu-50*tick;stamp->ready_qpc=cpu-20*tick;}
        corrected.note_gpu_ready_age(sampled,cpu,cpu+10*tick);auto exp=corrected.experiments();
        check(exp.gpu_ready_samples==1&&exp.predicted_samples==1&&std::abs(exp.native_gpu_tail.last-30)<.05&&
            std::abs(exp.gpu_ready_to_submit.last-20)<.05&&std::abs(exp.gpu_ready_to_display.last-30)<.05,
            "GPU-ready latency excludes the native GPU tail and distinguishes measured submission from predicted display");
        sampled.current_color_ready[1]->ready_qpc=cpu-15*tick;
        corrected.note_gpu_ready_age(sampled,cpu,cpu-tick);exp=corrected.experiments();
        check(exp.gpu_ready_samples==2&&exp.predicted_samples==1&&exp.prediction_late==1&&std::abs(exp.gpu_ready_to_submit.last-15)<.05,
            "Use the latest of both native eyes; missed prediction targets cannot become claimed display measurements");
        corrected.pacing_active=true;corrected.ticks=2;corrected.awaiting_generated_submission=7;corrected.submitted_native_id=7;
        corrected.wake_producer();check(corrected.release_gate_active()&&corrected.credits==0&&corrected.credit_deferred,
            "Generated submission gate holds native credits after FG GPU retirement");
        corrected.finish_generated_gate(6,true);check(corrected.credits==0,"An older generated submission cannot release a newer native gate");
        corrected.finish_generated_gate(7,true);check(corrected.credits==1&&corrected.generated_gate_releases==1&&!corrected.release_gate_active(),
            "Matching generated OpenXR submission releases exactly one coalesced native credit");
    }
    check(P::pair_wait_deadline(now,8333333)-now==nanoseconds(4333333)&&
        P::pair_wait_deadline(now,10000000)-now==milliseconds(6)&&
        P::pair_wait_deadline(now,16666667)-now==milliseconds(10)&&
        P::pair_wait_deadline(now,4000000)==now,
        "Late FG wait must cap at 10 ms and reserve 4 ms of the actual headset period");
    P p;P::Pair pair;pair.native_captured=now-milliseconds(40);pair.current_captured=now-milliseconds(20);
    pair.interpolated=true;pair.completed_at=now-milliseconds(5);
    pair.handed_off_at=now-milliseconds(19);pair.worker_started_at=now-milliseconds(17);pair.submitted_at=now-milliseconds(15);
    p.note_frame_age(pair,false,false,now);p.note_frame_age(pair,true,false,now);p.note_frame_age(pair,false,true,now+milliseconds(10));
    auto measured=p.presentation();
    check(measured.supported&&measured.native_samples==1&&measured.generated_samples==1&&measured.repeat_samples==1&&
        std::abs(measured.native_age_ms-40)<.001&&std::abs(measured.generated_age_ms-30)<.001&&
        std::abs(measured.newest_input_age_ms-20)<.001&&std::abs(measured.repeat_age_ms-50)<.001,
        "Submission age must follow actual native captures, generated midpoint and unchanged repeated content");
    check(std::abs(p.capture_to_completion.mean()-15)<.001&&std::abs(p.completion_to_generated.mean()-5)<.001&&
        std::abs(p.completion_to_native.mean()-5)<.001&&
        std::abs(p.capture_to_completion.mean()+p.completion_to_generated.mean()-measured.newest_input_age_ms)<.001,
        "Capture-to-completion plus completed-frame residence must account for newest FG input age");
    double stages{};for(auto& stage:p.generation_stages)stages+=stage.mean();
    check(std::abs(stages-p.capture_to_completion.mean())<.001&&p.generation_stages[0].count==1,
        "Capture/handoff/record/GPU completion stages must account for the exact same submitted frame population");
    pair.native_captured={};pair.current_captured=now+milliseconds(1);
    p.note_frame_age(pair,false,false,now);p.note_frame_age(pair,true,false,now);
    measured=p.presentation();
    check(measured.native_samples==1&&measured.generated_samples==1&&p.age_samples[2]==1,
        "Missing/future capture timestamps must never become valid frame-age samples");

    p.fg_active=p.pacing_active=true;p.state=CheekyFgState::generating;p.ticks=2;p.completed_pair_id=7;p.app_waiters=1;
    std::atomic<bool> finished{};
    std::thread waiter([&]{p.wait_for_new_pair(Clock::now(),50000000);finished=true;});
    {std::unique_lock lock(p.mx);
        const bool woke=p.changed.wait_for(lock,seconds(1),[&]{return p.credits>0;});
        check(woke&&!finished&&p.late_wait_samples==0,
            "An already-complete baseline pair must not satisfy the new-pair wait");
        p.changed.notify_all();p.completed_pair_id=8;p.changed.notify_all();}
    waiter.join();measured=p.presentation();
    check(finished&&measured.wait_samples==1&&measured.wait_arrivals==1&&measured.wait_timeouts==0,
        "New FG publication must wake the presenter before timeout");
    const auto credits=p.credits;p.wake_producer();
    check(credits==1&&p.credits==credits,"Early wake plus submission must grant only one game credit per XR tick");
    ++p.ticks;p.credits=0;p.app_waiters=0;
    p.wait_for_new_pair(Clock::now(),8333333);
    check(p.presentation().wait_samples==1,"No incoming render or inference must not cause an artificial wait");
    p.inflight_id=9;p.wait_for_new_pair(Clock::now(),4500000);measured=p.presentation();
    check(measured.wait_samples==2&&measured.wait_timeouts==1&&measured.late_wait_last_ms>=.45,
        "An unavailable new pair must expire its bounded budget and preserve normal fallback selection");
    p.wait_for_new_pair(Clock::now()-milliseconds(50),8333333);
    check(p.presentation().wait_samples==2,"A spent submission budget must not add any late wait");
    p.state=CheekyFgState::waiting_for_guides;p.wait_for_new_pair(Clock::now(),16666667);
    check(p.presentation().wait_samples==2,"Unavailable FG must continue native presentation without an artificial wait");
    check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&p.producer.fence))),"Late selection completed producer fence");
    p.state=CheekyFgState::generating;++p.ticks;p.completed_pair_id=9;p.inflight_id=10;p.credits=0;
    P::Pair available;available.native_id=8;available.current_id=9;available.interpolated=true;p.ready.push_back(available);
    p.wait_for_new_pair(Clock::now(),8333333,7.5);
    check(p.presentation().wait_samples==2&&p.late_ready_skips==1&&p.credits==0,
        "A forward completed pair must bypass waiting for another native render and preserve upload-before-game-wake order");
    p.wait_for_new_pair(Clock::now(),8333333,8);
    check(p.presentation().wait_samples==2&&p.late_ready_skips==2,
        "A ready interpolation after the already-presented native image is useful new content");
    check(p.newest_ready_pair(9.5)==p.ready.end(),"Ready pairs must not rewind past the displayed interpolation");
    p.wait_for_new_pair(Clock::now(),4500000,9.5);
    check(p.presentation().wait_samples==3&&p.presentation().wait_timeouts==2,
        "A stale completed pair must not suppress the wait for a newer in-flight pair");
    {
        P ordered;ordered.pacing_active=true;ordered.ticks=2;ordered.inflight_id=5;ordered.submitted_native_id=4;
        ordered.wake_producer();
        check(ordered.credits==0&&ordered.credit_deferred&&ordered.deferred_credits==1,
            "A newer game render must not overtake an older native FG job awaiting GPU submission");
        ordered.inference_submitted();
        check(ordered.credits==1&&!ordered.credit_deferred&&ordered.released_deferred_credits==1&&ordered.submitted_native_id==5,
            "Enqueueing FG must release pacing without waiting for GPU completion");
        ordered.wake_producer();check(ordered.credits==1,"Submission callback and ordinary wake must not duplicate a pacing credit");
        ordered.ticks=4;ordered.credits=0;
        auto next=std::make_shared<P::Native>();next->id=6;ordered.pending.push_back(next);
        ordered.wake_producer();ordered.inference_submitted();
        check(ordered.credits==0&&ordered.credit_deferred,"An older submitted job must not release a newer pending job's backpressure");
        ordered.pending.clear();ordered.inflight_id=0;ordered.release_deferred_credit();
        check(ordered.credits==1&&!ordered.credit_deferred,"Dropped or invalid jobs must release pacing on retirement rather than deadlock native fallback");
        ordered.ticks=6;ordered.credits=0;ordered.inflight_id=7;ordered.wake_producer();ordered.stopping=true;ordered.inference_submitted();
        check(ordered.credits==0,"Shutdown must not release a deferred render credit");
    }
    {
        P held;held.started=true;held.credits=2;held.pacing_active=true;held.ticks=2;
        auto job=std::make_shared<P::Native>();job->id=5;job->guides_valid=true;job->guides.release_point=2;
        held.pending.push_back(job);held.wake_producer();
        check(held.release_gate_active()&&held.credits==2&&held.credit_deferred,
            "Completion policy must block pre-existing credits before worker pickup");
        held.pending.clear();held.inflight_id=5;held.inflight_release_point=2;held.submitted_native_id=4;
        held.inference_submitted();
        check(held.release_gate_active()&&held.credit_deferred&&held.released_deferred_credits==0,
            "FG submission must not release a completion-held native frame");
        held.ticks=4;held.wake_producer();
        check(held.credit_deferred&&held.credits==2,"Ordinary pacing must not bypass the completion hold");
        std::atomic<bool> returned{};XrResult result{};XrFrameState state{XR_TYPE_FRAME_STATE};
        std::thread waiter([&]{result=held.wait_frame(nullptr,&state);returned=true;});
        bool waiting{};
        for(unsigned i=0;i<1000&&!waiting;++i){
            {std::lock_guard lock(held.mx);waiting=held.app_waiters!=0;}
            if(!waiting)Sleep(1);
        }
        const bool blocked=waiting&&!returned;
        {std::lock_guard lock(held.mx);held.inflight_id=0;held.inflight_release_point=0;
            held.release_deferred_credit();held.changed.notify_all();}
        waiter.join();
        check(blocked&&XR_SUCCEEDED(result)&&held.released_deferred_credits==1,
            "xrWaitFrame with an existing credit must wait until stereo FG retirement");
        // Reproduce a wait credit consumed before the previous frame handed off.
        held.waited=2;held.begun=held.ended=1;held.inflight_id=6;held.inflight_release_point=2;
        std::atomic<bool> entered{};returned=false;
        std::thread beginner([&]{entered=true;result=held.begin_frame(nullptr);returned=true;});
        while(!entered)Sleep(1);Sleep(10);const bool begin_blocked=!returned;
        {std::lock_guard lock(held.mx);held.inflight_id=0;held.inflight_release_point=0;held.changed.notify_all();}
        beginner.join();
        check(begin_blocked&&XR_SUCCEEDED(result)&&held.begun==2,
            "A pipelined pre-consumed wait credit must not bypass the hold at xrBeginFrame");
        job->guides_valid=false;held.pending.push_back(job);
        check(!held.release_gate_active(),"Invalid guides must preserve native fallback");
        held.pending.clear();held.inflight_id=7;held.inflight_release_point=2;held.degraded=true;
        check(!held.release_gate_active(),"Degraded/native fallback must release the completion gate");
        held.degraded=false;held.stopping=true;const auto credits=held.credits;
        check(held.wait_frame(nullptr,&state)==XR_ERROR_SESSION_NOT_RUNNING&&held.credits==credits,
            "Shutdown must wake completion-held frames without consuming a credit");
    }
    {
        P prepared;prepared.inflight_id=9;prepared.inflight_release_point=1;
        prepared.credits=0;prepared.credit_deferred=true;
        check(prepared.release_gate_active(),"Preparation gate blocks before submission");
        prepared.inference_submitted();
        check(prepared.release_gate_active()&&prepared.credits==0,"CPU submission is not GPU preparation completion");
        prepared.inference_prepared();
        check(!prepared.release_gate_active()&&prepared.credits==1&&prepared.inflight_id==9,
            "GPU preparation releases producer while stereo FG remains in flight");
        prepared.inference_prepared();check(prepared.credits==1,"Preparation callback cannot duplicate credits");
        prepared.inflight_release_point=2;prepared.credit_deferred=true;
        prepared.inference_prepared();check(prepared.release_gate_active(),"Completion policy cannot release at preparation");
    }
    {
        P clock;clock.clock_pacing_active=true;clock.pacing_active=true;clock.telemetry.xr_period_ms=8.333333;
        clock.submit_repeat_frames=false;
        std::thread pacer([&]{clock.pacing_loop();});
        bool delivered{};{std::unique_lock lock(clock.mx);delivered=clock.changed.wait_for(lock,seconds(1),[&]{return clock.credits>0;});
            clock.stopping=true;clock.changed.notify_all();}
        pacer.join();check(delivered&&clock.ticks==0,"Native clock must deliver credits without any XR or repeat submissions");
    }
    std::cout<<"PASS bounded late FG wait: baseline race, completion wake, timeout, refresh budget and single pacing credit; native/generated/repeat capture ages\n";
    std::cout<<"PASS ready-pair late selection: useful completed pairs bypass new-publication waits, stale pairs cannot rewind, source age decomposes into completion and residence\n";
    std::cout<<"PASS FG submission backpressure: next render waits for older FG enqueue, not GPU completion; retirement/fallback releases, no duplicate credits or shutdown wake\n";
    std::cout<<"PASS FG completion gate: pending/in-flight holds, submit and pacing paths, existing/pipelined credits, retirement, fallback and shutdown\n";
    std::cout<<"PASS FG preparation milestone and independent native clock without XR submissions\n";
}

void fg_buffered_validation(ID3D12Device* device,const wchar_t* models=nullptr,unsigned method=2) {
    using namespace buffered_probe;
    using Microsoft::WRL::ComPtr;
    fg_presentation_timing_validation(device);fg_latency_validation(device);
    {
        cheeky::gpu::Groups groups;
        groups.add(1,.2);groups.commit();check(groups.summary().samples==0,"Do not publish partially measured capture batches");
        groups.add(1,.3,true);groups.commit();auto value=groups.summary();
        check(value.samples==1&&std::abs(value.mean-.5)<.000001,"Capture timing must sum all GPU batches in one native frame");
        groups.add(2,.1,false,2);groups.commit();check(groups.summary().samples==1,"Guide timing needs both completed eyes");
        groups.add(2,.2,false,2);groups.commit();value=groups.summary();
        check(value.samples==2&&std::abs(value.last-.3)<.000001,"Guide timing must sum both eye snapshots");
    }
    {
        cheeky::fg::CadenceWindow measured;std::array<double,5> rates{};
        for(unsigned tick=0;tick<=120;++tick)measured.add(tick*1000./120,
            {tick/4,tick/4,tick/4,tick-2*(tick/4),tick});
        check(measured.rates(rates)&&std::abs(rates[0]-30)<.001&&std::abs(rates[1]+rates[2]-60)<.001&&
            std::abs(rates[3]-60)<.001&&std::abs(rates[4]-120)<.001,
            "120 runtime submissions with repeated pixels must report 60 unique FPS, not 120");
        measured.add(2000,{30,30,30,180,240});
        check(measured.rates(rates)&&std::abs(rates[0]-15)<.001&&std::abs(rates[1]+rates[2]-30)<.001,
            "Measured unique cadence follows wall time and content changes, not headset refresh metadata");
    }
    Runtime::device=device;Runtime::chains.clear();Runtime::waits=Runtime::begins=Runtime::ends=0;
    Runtime::scenes=Runtime::generated=Runtime::raw_frames=Runtime::errors=Runtime::age_logs=Runtime::path_logs=0;Runtime::phase=Runtime::owner=Runtime::next_chain=0;
    Runtime::paused=Runtime::reject_private=false;Runtime::last_native=nullptr;Runtime::snapshots=nullptr;
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"Buffered producer queue");
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
    check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))&&
        SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands))),"Buffered test commands");
    ComPtr<ID3D12Fence> complete,blocked;UINT64 serial{};
    check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&complete)))&&
        SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&blocked))),"Buffered test fences");
    auto execute=[&] {
        check(SUCCEEDED(commands->Close()),"Buffered test close");ID3D12CommandList* lists[]{commands.Get()};queue->ExecuteCommandLists(1,lists);
        check(SUCCEEDED(queue->Signal(complete.Get(),++serial)),"Buffered test signal");
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event!=nullptr,"Buffered test event");
        const bool done=SUCCEEDED(complete->SetEventOnCompletion(serial,event))&&WaitForSingleObject(event,10000)==WAIT_OBJECT_0;
        CloseHandle(event);check(done,"Buffered test wait");
    };
    ComPtr<ID3D12DescriptorHeap> rtv;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=3;
    check(SUCCEEDED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv))),"Buffered RTV heap");
    const auto stride=device->GetDescriptorHandleIncrementSize(hd.Type);const unsigned width=models?512:64;
    auto texture=[&](unsigned index,DXGI_FORMAT format,DXGI_FORMAT view,const float* clear) {
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=index?width/2:width;
        d.Format=format;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> result;
        check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&result))),"Buffered source texture");
        auto slot=rtv->GetCPUDescriptorHandleForHeapStart();slot.ptr+=index*stride;
        D3D12_RENDER_TARGET_VIEW_DESC v{};v.Format=view;v.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(result.Get(),&v,slot);commands->ClearRenderTargetView(slot,clear,0,nullptr);
        if(index)cheeky::fg::color_transition(commands.Get(),result.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);
        return result;
    };
    const float red[]{1,0,0,1},z[]{.5f,0,0,0},zero[4]{};
    auto color=texture(0,DXGI_FORMAT_B8G8R8A8_TYPELESS,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,red);
    auto depth=texture(1,DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_R32_FLOAT,z);
    auto motion=texture(2,DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R16G16_FLOAT,zero);execute();
    cheeky::fg::BufferedPresenter p;check(p.setup(h<XrSession>(2),Runtime::functions(),device,queue.Get()),"Buffered presenter setup");
    Runtime::options={};p.transport_options_query=Runtime::transport_options;
    cheeky::foveated_dlss::FgSnapshots snapshots;
    if(models) {
        p.trace=Runtime::trace;
        check(p.worker.probe(device,models),p.worker.diagnostic);
        Runtime::guides={};Runtime::guides.producer_queue=queue.Get();
        for(auto& eye:Runtime::guides.eyes){eye.depth=depth.Get();eye.motion=motion.Get();eye.width=eye.height=width;
            eye.depth_width=eye.depth_height=eye.motion_width=eye.motion_height=width/2;
            eye.focus_width=.55f;eye.focus_height=.45f;eye.peripheral_method=method;eye.configuration=123;}
        p.guide_acquire=Runtime::acquire_guides;p.guide_release=Runtime::release_guides;p.guide_stamp=Runtime::stamp;
        check(snapshots.initialize(queue.Get()),"Real FG guide snapshot queue");Runtime::snapshots=&snapshots;
    }
    bool snapshot_active=models!=nullptr;
    auto snapshot_inputs=[&] {
        if(!snapshot_active)return;
        auto* c=p.producer.begin();if(!c)return; // Exercise the bounded busy fallback, too.
        for(auto eye:Runtime::guides.eyes) {
            eye.render_frame=Runtime::capture_token/2;
            for(auto* r:{eye.depth,eye.motion})cheeky::fg::color_transition(c->list.Get(),r,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            snapshots.record(c->list.Get(),eye);
            for(auto* r:{eye.depth,eye.motion})cheeky::fg::color_transition(c->list.Get(),r,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        }
        std::uint64_t ticket{};check(p.producer.execute(*c,ticket),"Real guide snapshot submission");snapshots.submitted(queue.Get(),c->list.Get());
    };
    std::array<XrCompositionLayerProjectionView,2> views{},helper_views{};
    for(unsigned i=0;i<2;++i){views[i]={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};views[i].pose.orientation.w=1;
        views[i].fov={-.7f,.7f,.7f,-.7f};views[i].subImage={h<XrSwapchain>(200),{{0,0},{int(width),int(width)}},0};
        helper_views[i]=views[i];helper_views[i].subImage={h<XrSwapchain>(201),{{0,0},{4,4}},0};}
    XrCompositionLayerProjection scene{XR_TYPE_COMPOSITION_LAYER_PROJECTION},helper{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    scene.space=helper.space=h<XrSpace>(1);scene.views=views.data();helper.views=helper_views.data();scene.viewCount=helper.viewCount=2;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};quad.space=scene.space;quad.pose.orientation.w=1;quad.size={1,1};
    quad.subImage={h<XrSwapchain>(201),{{0,0},{int(width),int(width)}},0};
    const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&helper),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&scene),reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};XrFrameState state{XR_TYPE_FRAME_STATE};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=3;end.layers=layers;
    // Block the GPU, then verify multiple application submissions still return.
    // Signal before assertions so a failed check cannot strand queue teardown.
    queue->Wait(blocked.Get(),1);double maximum_handoff{};bool accepted=true;
    // Native Wait/Begin now deliberately enforce the submission milestone.
    // Release this synthetic GPU blockage independently of that app loop;
    // continue measuring only capture/end-frame's nonblocking handoff.
    struct InitialRelease {std::thread thread;~InitialRelease(){if(thread.joinable())thread.join();}} initial_release{
        std::thread([gate=blocked]{Sleep(80);gate->Signal(1);})};
    for(unsigned frame=0;frame<7;++frame) {
        accepted&=XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin));end.displayTime=state.predictedDisplayTime;Runtime::capture_token+=2;snapshot_inputs();
        const auto start=cheeky::fg::TimingClock::now();
        p.capture(h<XrSwapchain>(200),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        p.capture(h<XrSwapchain>(201),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        accepted&=XR_SUCCEEDED(p.end_frame(&end,{},&scene));maximum_handoff=std::max(maximum_handoff,cheeky::fg::elapsed_ms(start));
        quad.size.width=99;Sleep(1);quad.size.width=1;
    }
    blocked->Signal(1);
    initial_release.thread.join();
    check(accepted,"Buffered submissions must succeed while producer GPU work is blocked");
    check(maximum_handoff<40,"Game handoff must not wait for the blocked producer fence or inference completion");
    for(unsigned frame=0;frame<(models?1000u:5u);++frame) {
        check(XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin)),"Buffered app frame loop");
        end.displayTime=state.predictedDisplayTime;Runtime::capture_token+=2;snapshot_inputs();
        check(p.capture(h<XrSwapchain>(200),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
            p.capture(h<XrSwapchain>(201),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB),"Buffered capture reuse");
        check(XR_SUCCEEDED(p.end_frame(&end,{},&scene)),"Buffered handoff");quad.size.width=99;Sleep(1);quad.size.width=1;
        if(models&&frame>=24&&Runtime::generated>=4)break;
    }
    if(!models) {
        {std::lock_guard lock(p.mx);p.submit_repeat_frames=false;p.changed.notify_all();}
        for(unsigned frame=0;frame<8;++frame) {
            check(XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin)),"No-repeat native clock progression");
            end.displayTime=state.predictedDisplayTime;Runtime::capture_token+=2;
            check(p.capture(h<XrSwapchain>(200),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
                p.capture(h<XrSwapchain>(201),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
                XR_SUCCEEDED(p.end_frame(&end,{},&scene)),"No-repeat native content");
        }
        Sleep(50);const auto submitted=Runtime::ends.load();Sleep(40);
        check(Runtime::ends==submitted,"No new content must mean no further XR frame submissions, including empty frames");
        {std::lock_guard lock(p.mx);p.submit_repeat_frames=true;p.changed.notify_all();}
        Sleep(40);check(Runtime::ends>submitted,"Repeat submissions can resume live");
        std::cout<<"PASS no-repeat native fallback: fresh images advance; stalled content produces zero XR submissions; live re-enable\n";
    }
    for(unsigned capture_mode:{0u,1u})for(unsigned upload_mode:{0u,1u}){
        Runtime::options.color_capture_mode=capture_mode;Runtime::options.upload_mode=upload_mode;
        const auto direct_before=p.direct_eye_copies.load();
        for(unsigned frame=0;frame<(models?36u:8u);++frame){
            check(XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin)),"Live transport dropdown frame");
            end.displayTime=state.predictedDisplayTime;Runtime::capture_token+=2;snapshot_inputs();
            check(p.capture(h<XrSwapchain>(200),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
                p.capture(h<XrSwapchain>(201),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
                XR_SUCCEEDED(p.end_frame(&end,{},&scene)),"Live optimized capture/upload handoff");
        }
        {std::unique_lock lock(p.mx);check(p.changed.wait_for(lock,std::chrono::seconds(3),[&]{return p.inflight_id==0&&p.pending.empty();}),"Transport experiment retirement");
            const auto exp=p.experiments();check(exp.color_capture_mode==capture_mode&&exp.upload_mode==upload_mode,"Dropdown modes reach actual presenter");
            if(models)check(exp.latency_enabled&&exp.latency_valid&&exp.latency_samples>0&&exp.gpu_ready_samples>0&&exp.predicted_samples>0,
                "Physical FG records valid GPU-ready ages and calibrated timelines in every transport mode");}
        check(!capture_mode||p.direct_eye_copies>direct_before,"Live projection learns direct eye capture layout");
        std::cout<<"PASS live transport experiment: capture="<<capture_mode<<" upload="<<upload_mode<<"\n";
    }
    if(models)for(bool diagnostics:{false,true}){
        Runtime::options.latency_diagnostics=diagnostics?1u:0u;
        for(unsigned frame=0;frame<6;++frame){
            check(XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin)),"Live diagnostic overhead toggle");
            end.displayTime=state.predictedDisplayTime;Runtime::capture_token+=2;snapshot_inputs();
            check(p.capture(h<XrSwapchain>(200),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
                p.capture(h<XrSwapchain>(201),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
                XR_SUCCEEDED(p.end_frame(&end,{},&scene)),"Diagnostic toggle keeps generating");
        }
        std::unique_lock lock(p.mx);check(p.changed.wait_for(lock,std::chrono::seconds(3),[&]{return p.inflight_id==0&&p.pending.empty();}),"Diagnostic-toggle retirement");
        const auto exp=p.experiments();check(bool(exp.latency_enabled)==diagnostics&&bool(p.worker.latency.enabled)==diagnostics&&
            (!diagnostics?!exp.latency_valid:(exp.latency_valid&&exp.latency_samples>0)),"Diagnostic OFF/ON applies to real FG without stale timings");
    }
    if(!models){
        const auto raw_before=Runtime::raw_frames.load();
        views[1].subImage.imageRect.extent.width=int(width/2);
        check(XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin)),"Changed-layout frame");
        check(p.capture(h<XrSwapchain>(200),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
            p.capture(h<XrSwapchain>(201),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
            XR_SUCCEEDED(p.end_frame(&end,{},&scene)),"Changed projection uses original native passthrough safely");
        check(Runtime::raw_frames>raw_before,"Uncaptured eye viewport never displays stale pixels");
        views[1].subImage.imageRect.extent.width=int(width);
        // First restored frame also falls back, then the learned layout works.
        for(unsigned frame=0;frame<3;++frame){
            check(XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin)),"Restored-layout frame");
            check(p.capture(h<XrSwapchain>(200),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
                p.capture(h<XrSwapchain>(201),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
                XR_SUCCEEDED(p.end_frame(&end,{},&scene)),"Restored projection layout recovers direct capture");
        }
    }
    if(models) {
        p.guide_stamp=Runtime::missing_stamp;
        for(unsigned frame=0;frame<3;++frame){
            check(XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin)),"Transient guide miss frame");
            check(state.predictedDisplayPeriod==16666666,"A transient guide miss must preserve established 2x native pacing");
            end.displayTime=state.predictedDisplayTime;Runtime::capture_token+=2;snapshot_inputs();
            p.capture(h<XrSwapchain>(200),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
            p.capture(h<XrSwapchain>(201),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
            check(XR_SUCCEEDED(p.end_frame(&end,{},&scene)),"Native fallback during missing guides");
        }
        p.guide_stamp=Runtime::stamp;
        const auto before=Runtime::generated.load();std::uint64_t skipped_before{};
        {std::lock_guard lock(p.mx);skipped_before=p.skipped;}
        const auto steady_begin=cheeky::fg::TimingClock::now();
        const unsigned steady_frames=method==1?132u:60u;
        for(unsigned frame=0;frame<steady_frames;++frame) {
            check(XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin)),"Steady buffered frame loop");
            end.displayTime=state.predictedDisplayTime;Runtime::capture_token+=2;snapshot_inputs();
            check(p.capture(h<XrSwapchain>(200),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
                p.capture(h<XrSwapchain>(201),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
                XR_SUCCEEDED(p.end_frame(&end,{},&scene)),"Steady buffered submission");
        }
        Sleep(25);
        const auto generated=Runtime::generated.load()-before;
        {std::lock_guard lock(p.mx);const auto measured=p.cadence();
            check(measured.supported&&measured.valid&&measured.unique_fps>0&&measured.generated_fps>0&&
                std::abs(measured.native_target_fps-60)<.01&&std::abs(measured.refresh_hz-120)<.01,
                "Real buffered output exposes measured unique FPS separately from the 60/120 pacing targets");}
        std::uint64_t skipped_after{};{std::lock_guard lock(p.mx);skipped_after=p.skipped;}
        std::cout<<"Steady buffered mode="<<method<<" native FPS="<<1000.*steady_frames/cheeky::fg::elapsed_ms(steady_begin)
            <<" generated="<<generated<<'/'<<steady_frames<<" native, busy skips="<<skipped_after-skipped_before<<'\n';
        check(generated>=steady_frames*2/3&&skipped_after-skipped_before<15,"Steady pipeline must keep generating without serializing the game or exhausting its ring");
        if(method==1)check(Runtime::age_logs>0&&Runtime::path_logs>0,"Physical presenter must emit completed-frame residence and handoff/record/completion diagnostics through its ordinary trace callback");
        if(method==1) {
            std::uint64_t deferred_before{},released_before{};
            {std::lock_guard lock(p.mx);deferred_before=p.deferred_credits;released_before=p.released_deferred_credits;}
            check(SUCCEEDED(queue->Wait(blocked.Get(),2)),"Queue-pressure injection");
            // Block only this test queue. A guaranteed joined CPU releaser
            // prevents the test's game/presenter waits from stranding the GPU.
            struct Releaser {std::thread thread;~Releaser(){if(thread.joinable())thread.join();}} release{
                std::thread([gate=blocked]{Sleep(60);gate->Signal(2);})};
            for(unsigned frame=0;frame<8;++frame) {
                check(XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin)),"Pressured native frame loop");
                end.displayTime=state.predictedDisplayTime;Runtime::capture_token+=2;snapshot_inputs();
                check(p.capture(h<XrSwapchain>(200),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
                    p.capture(h<XrSwapchain>(201),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
                    XR_SUCCEEDED(p.end_frame(&end,{},&scene)),"Pressured native handoff");
            }
            release.thread.join();
            {std::lock_guard lock(p.mx);check(p.deferred_credits>deferred_before&&p.released_deferred_credits>released_before,
                "A real blocked inference queue must defer render-ahead and release it through the submission/retirement path");}
            std::cout<<"PASS physical GPU queue pressure: unsent FG backpressure exercised, game resumed without fence waits on its thread\n";
            Runtime::guides.release_point=2;
            auto submit_native=[&] {
                end.displayTime=state.predictedDisplayTime;Runtime::capture_token+=2;snapshot_inputs();
                check(p.capture(h<XrSwapchain>(200),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
                    p.capture(h<XrSwapchain>(201),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
                    XR_SUCCEEDED(p.end_frame(&end,{},&scene)),"Completion-held native handoff");
            };
            // Apply the live option to actual captured guide pairs first.
            for(unsigned frame=0;frame<2;++frame){
                check(XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin)),"Enable live completion hold");
                submit_native();
            }
            check(XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin)),"Prepare completion-gate pressure frame");
            ComPtr<ID3D12Fence> gate;
            check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)))&&
                SUCCEEDED(p.generation_queue->Wait(gate.Get(),1)),"Block only the FG queue until the timed release");
            Releaser completion_release{std::thread([gate]{Sleep(80);gate->Signal(1);})};
            submit_native();
            const auto held_result=p.wait_frame(&wait,&state);
            const bool gpu_released=gate->GetCompletedValue()>=1;
            bool retired{};{std::lock_guard lock(p.mx);retired=p.inflight_id==0&&p.pending.empty();}
            completion_release.thread.join();
            check(XR_SUCCEEDED(held_result)&&gpu_released&&retired,
                "Next native credit must wait for actual FG GPU completion, not just command submission");
            check(XR_SUCCEEDED(p.begin_frame(&begin)),"Resume after FG GPU completion");submit_native();
            Runtime::guides.release_point=0;
            check(XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin)),"Disable live completion hold");
            submit_native();
            for(unsigned point:{0u,1u,2u,3u})for(bool repeats:{true,false}) {
                Runtime::guides.release_point=point;Runtime::guides.submit_repeats=repeats?1u:0u;
                std::uint64_t repeat_baseline{};
                for(unsigned frame=0;frame<40;++frame){
                    check(XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin)),"Live release/repeat experiment frame");
                    submit_native();
                    if(frame==5){std::lock_guard lock(p.mx);repeat_baseline=p.repeated_presentations;}
                }
                {std::unique_lock lock(p.mx);
                    check(p.changed.wait_for(lock,std::chrono::seconds(2),[&]{return p.inflight_id==0&&p.pending.empty();}),"Experiment GPU retirement");
                    check(p.active_release_point==point&&p.submit_repeat_frames==repeats,"Live experiment settings reach presenter");
                    if(point==1)check(p.worker.prepared_serial>0&&p.worker.prepared_serial<p.worker.serial&&
                        p.worker.fence->GetCompletedValue()>=p.worker.serial,"Both-eye GPU preparation fence precedes final stereo fence");
                    if(!repeats)check(p.repeated_presentations==repeat_baseline,"Fresh-only mode must skip duplicate native prefixes before generated frames too");
                    if(point==3)check(p.generated_gate_releases>0,"Generated submission policy reaches actual OpenXR presentation");
                }
                if(!repeats){
                    Sleep(50);const auto submitted=Runtime::ends.load();Sleep(40);
                    check(Runtime::ends==submitted,"Fresh-only FG must stop runtime frame calls while input is paused");
                }
                std::cout<<"PASS physical experiment: release="<<point<<" repeat_submissions="<<repeats<<"\n";
            }
            // Prove that GPU completion alone cannot release policy 3: stall
            // the real presenter's generated xrEndFrame after copies queued.
            Runtime::guides.release_point=3;Runtime::guides.submit_repeats=0;
            check(XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin)),"Prepare generated submission gate");
            Runtime::allow_generated_end=false;Runtime::generated_end_entered=false;Runtime::block_generated_end=true;
            submit_native();std::atomic<bool> next_returned{};XrResult next_result=XR_ERROR_RUNTIME_FAILURE;
            std::thread next_wait([&]{next_result=p.wait_frame(&wait,&state);next_returned=true;});
            const auto until=GetTickCount64()+3000;
            while(!Runtime::generated_end_entered&&GetTickCount64()<until)Sleep(1);
            bool fence_complete{},gate_holding{};
            {std::lock_guard lock(p.mx);fence_complete=p.worker.fence&&p.worker.fence->GetCompletedValue()>=p.worker.serial;
                gate_holding=p.awaiting_generated_submission>0&&p.inflight_id==0&&!next_returned;}
            Runtime::allow_generated_end=true;Runtime::block_generated_end=false;next_wait.join();
            check(Runtime::generated_end_entered&&fence_complete&&gate_holding&&XR_SUCCEEDED(next_result),
                "After generated submission must hold past GPU completion until generated xrEndFrame returns");
            check(XR_SUCCEEDED(p.begin_frame(&begin)),"Resume game after generated submission");
            Runtime::guides.release_point=0;Runtime::guides.submit_repeats=1;submit_native();
            // Execute real NGX on each selected queue. In particular, a shared
            // queue must not wait on a future producer signal or strand either
            // GPU-preparation or generated-submission release policies.
            Runtime::guides.submit_repeats=0;
            for(unsigned mode:{2u,0u,1u})for(unsigned point:{0u,1u,3u}) {
                Runtime::guides.fg_queue_priority=mode;Runtime::guides.release_point=point;
                const auto generated_before=Runtime::generated.load();
                for(unsigned frame=0;frame<36;++frame){
                    check(XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin)),"Live GPU queue switch frame");
                    submit_native();
                }
                {std::unique_lock lock(p.mx);
                    check(p.changed.wait_for(lock,std::chrono::seconds(3),[&]{return p.inflight_id==0&&p.pending.empty();}),"Live GPU queue switch retirement");
                    auto* requested=mode==2u?queue.Get():mode==0u?p.fg_normal_queue.Get():p.fg_high_queue.Get();
                    check(requested&&p.generation_queue.Get()==requested&&p.worker.queue.Get()==requested,
                        "Requested shared/NORMAL/HIGH queue actually executes NGX");
                    check(p.reported_queue_mode==mode&&p.telemetry.shared_gpu_queue==(mode==2u?1u:0u)&&
                        p.producer.queue.Get()==queue.Get()&&p.uploads.queue.Get()==queue.Get(),"Queue diagnostics and capture/upload ownership match actual execution");
                    const auto exp=p.experiments();
                    check(exp.latency_valid&&exp.latency_samples>0&&exp.gpu_ready_samples>0,
                        "Calibrated timeline remains valid after shared/separate queue switches");
                }
                check(Runtime::generated>generated_before+20,"Live queue switching preserves real generated output");
                std::cout<<"PASS physical GPU queue switch: mode="<<mode<<" release="<<point<<"\n";
            }
            Runtime::guides.release_point=0;Runtime::guides.submit_repeats=1;
            // Policy changes clear GPU timing windows. Refill both alternating
            // execution orders and the 17-frame detailed sampling cadence.
            for(unsigned frame=0;frame<36;++frame){
                check(XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin)),"Resume normal overlap after live OFF");
                submit_native();
            }
            std::cout<<"PASS physical GPU completion gate: live ON/OFF, blocked FG fence holds the next native frame until stereo work retires\n";
            std::cout<<"PASS physical generated-submission gate: GPU finished, generated xrEndFrame held, next game frame blocked until submission returned\n";
        }
    }
    Sleep(40);
    if(models) {
        snapshots.collect_completed();const auto guide=snapshots.capture_gpu();
        check(guide.samples>0&&guide.mean>0,"Physical GPU guide-copy profiling must use already-retired capture fences");
        std::cout<<"Guide snapshot GPU="<<guide.mean<<" ms/native (both eyes)\n";
    }
    {std::lock_guard lock(p.mx);const auto extra=p.overhead();
        check(extra.supported&&extra.capture_samples>0&&extra.upload_samples>0&&extra.capture_ms>0&&extra.upload_ms>0,
            "Actual capture/handoff and presentation-upload GPU queries must retire without adding CPU waits");
        if(models)check(extra.publication_samples>0&&extra.publication_ms==0,
            "Foveated FG must compose directly into its leased output without a second full-frame publication copy");
        std::cout<<"GPU stages: capture/handoff="<<extra.capture_ms<<" ms/native, OpenXR upload="<<extra.upload_ms
            <<" ms/presentation, generated copy="<<extra.publication_ms<<" ms/generated\n";
        const auto age=p.presentation();
        check(age.native_samples>0&&age.native_age_ms>0,"Real immutable native captures must retain their timestamps until runtime submission");
        if(models)check(age.generated_samples>0&&age.generated_age_ms>=age.newest_input_age_ms&&p.late_ready_skips>0&&
            std::abs(p.capture_to_completion.mean()+p.completion_to_generated.mean()-age.newest_input_age_ms)<.001,
            "Real NGX presentation must bypass waits for usable ready pairs and account for newest-input age with completion plus residence");
        if(models){double stages{};for(auto& stage:p.generation_stages)stages+=stage.mean();
            check(p.generation_stages[0].count>0&&std::abs(stages-p.capture_to_completion.mean())<.001,
                "Actual handoff, worker, CPU recording and completion intervals must account for color capture to completed FG");}
        std::cout<<"Submission age: native="<<age.native_age_ms<<", generated midpoint="<<age.generated_age_ms
            <<", newest input="<<age.newest_input_age_ms<<" ms; late waits="<<age.wait_samples
            <<" arrived="<<age.wait_arrivals<<" timeouts="<<age.wait_timeouts<<'\n';
    }
    p.stop();snapshot_active=false;
    check(Runtime::errors==0&&Runtime::waits==Runtime::begins&&Runtime::begins==Runtime::ends,
        "Exactly one background owner must balance runtime Wait/Begin/End and retain helper/UI descriptions");
    check(Runtime::owner!=GetCurrentThreadId()&&Runtime::scenes>0,"Presentation must run independently of the game");
    // Submission backpressure can now prevent startup ring exhaustion. The
    // explicitly occupied inference slots below exercise busy publication.
    if(models){
        check(Runtime::generated>0,"Real NGX output must reach the buffered runtime presentation loop");
        check(p.output.serial==0,"Generated snapshots must share the inference submission; no copy may be queued after its CPU completion wait");
        check(p.worker.direct_publication&&!p.worker.focus.full[0]&&!p.worker.focus.full[1],
            "Real foveated FG must write its leased publication images without allocating a private full-frame intermediate");
        const auto& timing=p.worker.gpu_timings;double covered=timing.preparation_gaps.mean();
        check(timing.preparation_samples>0&&timing.sampled_preparation.mean()>0,
            "Real buffered FG must retire sampled preparation boundaries through the existing inference fence");
        for(const auto& pass:timing.preparation_parts)for(const auto& phase:pass)covered+=phase.mean();
        check(std::abs(covered-timing.sampled_preparation.mean())<.001,
            "Sampled before/dispatch/after intervals must account for the sampled preparation scope without double counting");
        check(timing.order_samples[0]>0&&timing.order_samples[1]>0&&
            timing.order_detail_samples[0]>0&&timing.order_detail_samples[1]>0,
            "Buffered comparison must sample both left-first and right-first execution orders");
        check(p.worker.source_depth_format==unsigned(depth->GetDesc().Format)&&
              p.worker.source_motion_format==unsigned(motion->GetDesc().Format),
            "Buffered FG must consume the original leased guide snapshots without an intermediate full-guide conversion");
    }
    // Read a runtime image after the asynchronous upload queue has drained.
    auto* image=Runtime::last_native;check(image!=nullptr,"Buffered native image");
    const auto desc=image->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};
    device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
    D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=bytes;rd.Height=1;
    rd.DepthOrArraySize=rd.MipLevels=rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> readback;
    check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))),"Buffered readback");
    allocator->Reset();commands->Reset(allocator.Get(),nullptr);
    cheeky::fg::color_transition(commands.Get(),image,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=image;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
    commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    cheeky::fg::color_transition(commands.Get(),image,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);execute();
    void* data{};readback->Map(0,nullptr,&data);const auto* pixel=static_cast<unsigned char*>(data)+footprint.Offset;
    const bool red_image=pixel[0]==0&&pixel[1]==0&&pixel[2]==255;readback->Unmap(0,nullptr);check(red_image,"Asynchronous native upload must preserve actual scene pixels");
    std::cout<<"PASS buffered "<<(models?"physical NGX":"WARP native")<<": independent frame owner, blocked-GPU handoff max "<<maximum_handoff
        <<" ms, native submissions="<<Runtime::scenes<<" generated="<<Runtime::generated<<" skipped="<<p.skipped<<"; owned helper/UI and native pixels\n";
    // Occupy every inference lease. Native presentation must keep advancing
    // through the independent mailbox instead of freezing behind FG startup.
    Runtime::owner=0;std::array<std::shared_ptr<cheeky::fg::BufferedPresenter::Native>,3> busy;
    for(unsigned i=0;i<3;++i){busy[i]=std::make_shared<cheeky::fg::BufferedPresenter::Native>();p.slots[i]=busy[i];}
    const auto native_before=Runtime::scenes.load();
    for(unsigned frame=0;frame<8;++frame) {
        check(XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin)),"Native mailbox frame loop");
        end.displayTime=state.predictedDisplayTime;Runtime::capture_token+=2;snapshot_inputs();
        check(p.capture(h<XrSwapchain>(200),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
            p.capture(h<XrSwapchain>(201),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)&&
            XR_SUCCEEDED(p.end_frame(&end,{},&scene)),"Busy inference must permit native-only publication");
    }
    Sleep(25);p.stop();busy={};
    check(p.skipped>0,"Explicitly occupied inference slots must skip FG publication without blocking native handoff");
    check(Runtime::scenes>native_before&&Runtime::errors==0,"Native images must reach the headset while every inference lease is occupied");
    std::cout<<"PASS independent native mailbox while every FG transport lease is busy\n";
    // Restart and forward an unknown extension while its stack data is alive.
    Runtime::owner=0;
    check(XR_SUCCEEDED(p.wait_frame(&wait,&state))&&XR_SUCCEEDED(p.begin_frame(&begin)),"Buffered restart");
    XrBaseInStructure unknown{XR_TYPE_INSTANCE_CREATE_INFO};quad.next=&unknown;end.displayTime=state.predictedDisplayTime;Runtime::capture_token+=2;snapshot_inputs();
    check(XR_SUCCEEDED(p.end_frame(&end,{},&scene)),"Unsupported extension must use coordinated original native submission");quad.next=nullptr;
    p.stop();check(Runtime::raw_frames>0&&Runtime::errors==0&&Runtime::waits==Runtime::ends,"Native fallback and restart must preserve runtime frame ownership");
    p.reset();check(Runtime::chains.empty(),"Buffered shutdown must destroy private swapchains after workers stop");
    Runtime::owner=0;Runtime::reject_private=true;
    cheeky::fg::BufferedPresenter failed;
    check(failed.setup(h<XrSession>(2),Runtime::functions(),device,queue.Get()),"Buffered failure setup");
    const auto raw_before=Runtime::raw_frames.load();
    for(unsigned frame=0;frame<10&&Runtime::raw_frames==raw_before;++frame) {
        check(XR_SUCCEEDED(failed.wait_frame(&wait,&state))&&XR_SUCCEEDED(failed.begin_frame(&begin)),"Buffered failure frame loop");
        failed.capture(h<XrSwapchain>(200),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        failed.capture(h<XrSwapchain>(201),0,color.Get(),width,width,1,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        end.displayTime=state.predictedDisplayTime;Runtime::capture_token+=2;snapshot_inputs();
        check(XR_SUCCEEDED(failed.end_frame(&end,{},&scene)),"Private chain failure must recover original native submissions");
    }
    check(Runtime::raw_frames>raw_before,"Private swapchain failure must reach original native presentation");
    Runtime::paused=true;
    {std::lock_guard lock(failed.mx);failed.credits=0;
        // Pausing the runtime no longer pauses native credits. Hold an
        // already-waited frame before Begin so this second Wait is reliably
        // blocked on call order while testing cancellation by Stop.
        ++failed.waited;}
    XrResult interrupted=XR_SUCCESS;
    std::thread waiting([&]{XrFrameState f{XR_TYPE_FRAME_STATE};interrupted=failed.wait_frame(&wait,&f);});
    bool blocked_wait{};
    for(unsigned attempt=0;attempt<100&&!blocked_wait;++attempt){
        {std::lock_guard lock(failed.mx);blocked_wait=failed.app_waiters>0;}if(!blocked_wait)Sleep(1);}
    std::thread release_wait([]{Sleep(5);Runtime::paused=false;});
    failed.stop();waiting.join();release_wait.join();
    check(blocked_wait&&interrupted==XR_ERROR_SESSION_NOT_RUNNING&&Runtime::errors==0&&Runtime::waits==Runtime::ends,
        "Stop must wake a blocked application wait and close the outstanding runtime frame exactly once");
    failed.reset();Runtime::reject_private=false;
    Runtime::snapshots=nullptr;
    std::cout<<"PASS buffered private-swapchain failure recovery, restart, blocked application wait cancellation and balanced shutdown\n";
    fg_repeat_upload_validation(device);
}
