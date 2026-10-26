#pragma once

void fg_latest_interval_validation() {
    using namespace cheeky::fg;
    struct Completion final : FrameCompletion {bool done{true};bool ready() const noexcept override{return done;}};
    BufferedPresenter presenter;
    for(std::uint64_t id:{1u,2u,3u}){
        BufferedPair pair;pair.native_id=id;pair.current_id=id+1;pair.interpolated=true;pair.generated_count=2;
        pair.progress=std::make_shared<BufferedPair::Progress>();
        for(auto& phase:pair.progress->phases){
            for(auto& image:phase.images)image=std::make_shared<TransportImage>();
            phase.completion=std::make_shared<Completion>();
        }
        presenter.ready.push_back(std::move(pair));
    }
    for(unsigned release:{0u,1u,2u,4u,5u}){
        presenter.active_release_point=release;
        check(presenter.newest_ready_pair(0.)->current_id==4,
            "Overlap release policies select the newest usable queued interval");
    }
    for(unsigned release:{3u,6u,7u}){
        presenter.active_release_point=release;
        check(presenter.newest_ready_pair(0.)->current_id==2,
            "Presentation-gated policies retain their ordered interval submissions");
    }
    presenter.active_release_point=2;
    auto& newest=presenter.ready.back();
    auto completion=std::static_pointer_cast<Completion>(newest.progress->phases[0].completion);
    completion->done=false;
    check(presenter.newest_ready_pair(0.)->current_id==3,"An unusable newest interval does not hide an older usable one");
    completion->done=true;auto eye=newest.progress->phases[0].images[1];newest.progress->phases[0].images[1].reset();
    check(presenter.newest_ready_pair(0.)->current_id==3,"Latest selection still requires a complete stereo publication");
    newest.progress->phases[0].images[1]=eye;
    check(presenter.newest_ready_pair(2.5)->current_id==4&&presenter.newest_ready_pair(3.5)==presenter.ready.end(),
        "Latest selection cannot rewind the displayed source timeline");
    auto active=presenter.ready.front();active.generated_index=1;
    auto active_completion=std::static_pointer_cast<Completion>(active.progress->phases[1].completion);active_completion->done=false;
    check(!presenter.fresh_content(active,true,true,1.25),"A newer interval cannot interrupt an active generated-frame sequence");
    active_completion->done=true;
    check(presenter.fresh_content(active,true,true,1.25),"The active sequence resumes when its own next phase is usable");
    std::cout<<"PASS latest-interval selection for overlap; presentation gates and active sequences preserved\n";
}

// Hold the software GPU before a simulated generation output. The production
// presenter must queue its XR uploads and release the game before GPU readiness.
void fg_shared_queue_validation() {
    using namespace cheeky::fg;using Microsoft::WRL::ComPtr;using Runtime=buffered_probe::Runtime;
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> game,other;D3D12_COMMAND_QUEUE_DESC desc{};
    check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))&&
        SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))&&
        SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))&&
        SUCCEEDED(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&game)))&&
        SUCCEEDED(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&other))),"Shared submission WARP queues");
    Runtime::device=device.Get();Runtime::chains.clear();Runtime::direct_states.clear();
    Runtime::ends=Runtime::image_releases=0;
    auto xr=Runtime::functions();xr.time_to_qpc=nullptr;
    xr.end_frame=[](XrSession,const XrFrameEndInfo*)->XrResult {++Runtime::ends;return XR_SUCCESS;};
    BufferedPresenter presenter;
    check(presenter.setup(h<XrSession>(71),xr,device.Get(),game.Get()),"Shared submission presenter");
    ComPtr<ID3D12Fence> blocker,ready;
    check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&blocker)))&&
        SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&ready))),"Shared submission fences");
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))&&
        SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))),"Shared output commands");
    D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width=texture.Height=16;texture.DepthOrArraySize=texture.MipLevels=1;texture.SampleDesc.Count=1;
    texture.Format=DXGI_FORMAT_B8G8R8A8_UNORM;texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12DescriptorHeap> rtv;D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rtv_desc.NumDescriptors=2;
    check(SUCCEEDED(device->CreateDescriptorHeap(&rtv_desc,IID_PPV_ARGS(&rtv))),"Shared output RTVs");
    BufferedPair pair;pair.ready=1;pair.native_id=1;pair.current_id=2;pair.interpolated=true;
    pair.progress=std::make_shared<BufferedPair::Progress>();
    auto& phase=pair.progress->phases[0];phase.submitted_at=TimingClock::now();
    auto handle=rtv->GetCPUDescriptorHandleForHeapStart();const float red[]{1,0,0,1};
    for(auto& image:phase.images){
        image=std::make_shared<TransportImage>();image->identity=100+unsigned(&image-phase.images.data());
        check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,
            D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&image->texture))),"Shared output image");
        device->CreateRenderTargetView(image->texture.Get(),nullptr,handle);
        list->ClearRenderTargetView(handle,red,0,nullptr);
        color_transition(list.Get(),image->texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_GENERIC_READ);
        image->state=D3D12_RESOURCE_STATE_GENERIC_READ;
        handle.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    check(SUCCEEDED(list->Close()),"Close shared output commands");
    struct Unblock {ID3D12Fence* fence;~Unblock(){fence->Signal(1);}} unblock{blocker.Get()};
    check(SUCCEEDED(game->Wait(blocker.Get(),1)),"Hold software GPU before generation");
    ID3D12CommandList* lists[]{list.Get()};game->ExecuteCommandLists(1,lists);
    check(SUCCEEDED(game->Signal(presenter.producer.fence.Get(),1))&&SUCCEEDED(game->Signal(ready.Get(),1)),"Submit capture and output fences");
    presenter.producer.serial=1;
    phase.completion=std::make_shared<FrameCompletion12>(ready.Get(),1,game.Get());
    presenter.generation_queue=other; // A later setting cannot change this phase's provenance.
    check(!presenter.capture_completed(pair.ready)&&!presenter.phase_ready(pair,0)&&
        presenter.phase_presentable(pair,0)&&presenter.fresh_content(pair,true,true,1.),
        "Submitted shared phase is presentable before capture/generation completes");
    auto shared=phase.completion;phase.completion=std::make_shared<FrameCompletion12>(ready.Get(),1,other.Get());
    check(!presenter.phase_presentable(pair,0),"An unfinished separate-queue phase still requires GPU readiness");
    phase.completion=shared;auto right=phase.images[1];phase.images[1].reset();
    check(!presenter.phase_presentable(pair,0),"Shared ordering still requires both published eyes");phase.images[1]=right;
    pair.generated=phase.images;
    for(auto& view:pair.generated_views){view.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;view.pose.orientation.w=1;view.fov={-.5f,.5f,.5f,-.5f};}
    presenter.inflight_id=presenter.submitted_native_id=presenter.awaiting_generated_submission=2;
    presenter.inflight_release_point=presenter.active_release_point=3;
    XrResult result{};bool closed{};
    check(presenter.render(pair,pair.generated,pair.generated_views,true,1,result,closed)&&closed&&
        Runtime::ends==1&&Runtime::image_releases==2&&presenter.uploads.serial==1&&
        !presenter.uploads.completed(1)&&!presenter.phase_ready(pair,0)&&!presenter.release_gate_active(),
        "XR uploads, image releases, EndFrame and game release precede GPU completion");
    check(pair.completed_at==TimingClock::time_point{},"Early submission is not mislabeled as observed GPU completion");
    check(SUCCEEDED(blocker->Signal(1)),"Release software GPU");presenter.uploads.drain();
    check(presenter.phase_ready(pair,0)&&presenter.uploads.completed(1),"Queued generation and XR uploads retire in order");
    presenter.stop();Runtime::chains.clear();Runtime::device=nullptr;
    std::cout<<"PASS shared-queue XR upload before GPU completion; separate-queue readiness preserved\n";
}
