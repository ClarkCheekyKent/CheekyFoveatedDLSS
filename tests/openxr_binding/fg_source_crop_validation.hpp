#pragma once

namespace source_crop_test {
inline CheekyFgCaptureCropsV2 current;
inline CheekyFgColorOptionsV1 color;
inline bool __cdecl peek(CheekyFgCaptureCropsV2* out){
    if(out->token&&out->token!=current.token)return false;
    *out=current;return true;
}
inline bool __cdecl color_options(CheekyFgColorOptionsV1* out){*out=color;return true;}
}
void fg_source_crop_validation(bool hardware=false) {
    using namespace cheeky::fg;using Microsoft::WRL::ComPtr;
    CheekyFgEye g;g.width=2040;g.height=2080;g.full_eye_crop_compatibility=1;
    g.submitted_crop.valid=1;g.submitted_crop.revision=7;
    g.submitted_crop.rect[2]=1544.f/2040;
    const XrRect2Di left{{0,0},{1544,2080}},right{{2535,0},{1545,2080}};
    auto l=source_eye_region(g,left,4080,2080);
    check(l.active&&l.source.offset.x==0&&l.source.extent.width==2040&&l.visible.extent.width==1544,
        "Left symmetric crop recovers full source eye without changing visible width");
    g.submitted_crop.rect[0]=495.f/2040;g.submitted_crop.rect[2]=1545.f/2040;
    auto r=source_eye_region(g,right,4080,2080);
    check(r.active&&r.source.offset.x==2040&&r.visible.offset.x==495&&r.visible.extent.width==1545,
        "Right symmetric crop preserves truncation and source-relative offset");
    XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    view.pose.orientation.w=1;
    view.fov={-.7f,.7f,.7f,-.7f};
    const auto expanded=full_source_view(view,r.visible,2040,2080);
    const float step=(std::tan(view.fov.angleRight)-std::tan(view.fov.angleLeft))/1545;
    check(std::abs(std::tan(expanded.fov.angleLeft)+step*495-std::tan(view.fov.angleLeft))<.00001f&&
        std::abs(std::tan(expanded.fov.angleRight)-std::tan(view.fov.angleRight))<.00001f,
        "Processing projection expands source coverage while preserving visible rays");
    check(view.fov.angleLeft==-.7f,"Original presentation FOV remains unchanged");
    const auto processing=source_processing_view(view,r),unchanged=source_processing_view(view,{});
    check(processing.fov.angleLeft==expanded.fov.angleLeft&&unchanged.fov.angleLeft==view.fov.angleLeft,
        "FG and Universal NR use the expanded projection only for verified source eyes");
    StereoAlignment alignment;alignment.valid=alignment.gaze_valid=true;
    alignment.u={.5f,.5f};alignment.v={.5f,.5f};
    alignment.gaze_u={.25f,.75f};alignment.gaze_v={.25f,.75f};
    source_eye_alignment(alignment,0,l);source_eye_alignment(alignment,1,r);
    check(std::abs(alignment.u[0]-772.f/2040)<.00001f&&
        std::abs(alignment.u[1]-(495.f+772.5f)/2040)<.00001f&&
        std::abs(alignment.gaze_u[0]-386.f/2040)<.00001f&&
        std::abs(alignment.gaze_u[1]-(495.f+1158.75f)/2040)<.00001f&&
        alignment.v[0]==.5f&&alignment.gaze_v[1]==.75f,
        "Stereo alignment and gaze map from each visible crop into its full source eye");
    auto invalid=g;invalid.submitted_crop.valid=0;
    check(!source_eye_region(invalid,right,4080,2080).active,"Missing mapping is never inferred from atlas layout");
    invalid=g;invalid.submitted_crop.rect[2]=.5f;
    check(!source_eye_region(invalid,right,4080,2080).active,"Scaled submissions are rejected");
    check(!source_eye_region(g,right,4000,2080).active,"Source eye must fit the actual texture");
    invalid=g;invalid.full_eye_crop_compatibility=0;
    check(!source_eye_region(invalid,right,4080,2080).active,"Compatibility defaults to the existing path");
    invalid=g;invalid.submitted_crop.rect[0]=std::numeric_limits<float>::quiet_NaN();
    check(!source_eye_region(invalid,right,4080,2080).active,"Nonfinite mapping rejected");
    auto changed=r;++changed.revision;
    check(!(changed==r),"Mapping revision invalidates temporal source compatibility");
    {
        // UEVR Asymmetrical: 2694x2080 source eyes in a 5388x2080 atlas,
        // bounds 0..0.7571413 and 0.24285871..1 truncated to whole pixels.
        auto a=g;a.width=2694;a.submitted_crop.rect[0]=0;a.submitted_crop.rect[2]=1.f;
        const XrRect2Di al{{0,0},{2039,2080}},ar{{3348,0},{2040,2080}};
        check(!source_eye_region(a,al,5388,2080).active,
            "An assumed full-canvas crop contradicts the submitted size and is rejected");
        a.submitted_crop.rect[2]=2039.f/2694;
        const auto lo=source_eye_region(a,al,5388,2080);
        check(lo.active&&lo.source.offset.x==0&&lo.source.extent.width==2694&&
            lo.visible.offset.x==0&&lo.visible.extent.width==2039,"Asymmetric left native crop maps to its source eye");
        a.submitted_crop.rect[0]=654.f/2694;a.submitted_crop.rect[2]=2040.f/2694;
        const auto ro=source_eye_region(a,ar,5388,2080);
        check(ro.active&&ro.source.offset.x==2694&&ro.source.extent.width==2694&&
            ro.visible.offset.x==654&&ro.visible.extent.width==2040,"Asymmetric right native crop maps to its source eye");
    }
    for(unsigned scale:{50u,75u,100u})for(bool focused:{false,true}){
        g.focus_width=focused?.5f:1;g.focus_height=focused?.25f:1;g.resolution_percent=scale;g.peripheral_percent=50;
        const auto grid=guide_grid(g,1360,1387,2040,2080,0);
        check(grid.width<=grid.color_width&&grid.height<=grid.color_height&&grid.width>0&&grid.height>0,
            "Scaled prepared guide grids retain the full-source coordinate contract");
    }

    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> adapter;ComPtr<ID3D12Device> device;
    check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))),"Source crop DXGI factory");
    if(hardware){
        for(unsigned i=0;;++i){ComPtr<IDXGIAdapter1> candidate;
            if(factory->EnumAdapters1(i,&candidate)==DXGI_ERROR_NOT_FOUND)break;
            DXGI_ADAPTER_DESC1 desc{};candidate->GetDesc1(&desc);
            if(desc.VendorId==0x10de&&!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){adapter=candidate;break;}}
    }else check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))),"Source crop WARP adapter");
    check(adapter&&SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"Source crop D3D12 device");
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};
    check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"Source crop queue");
    auto texture=[&](unsigned width,unsigned height,DXGI_FORMAT format){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;
        d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=format;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> out;
        check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,
            IID_PPV_ARGS(&out))),"Source crop texture");return out;
    };
    auto atlas=texture(4080,2080,DXGI_FORMAT_R8G8B8A8_UNORM);
    auto depth=texture(1360,1387,DXGI_FORMAT_R32_FLOAT),motion=texture(2040,2080,DXGI_FORMAT_R32G32_FLOAT);
    ComPtr<ID3D12DescriptorHeap> rtvs;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=3;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
    check(SUCCEEDED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs)))&&
        SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))&&
        SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands))),
        "Initialize source crop pixels");
    auto handle=rtvs->GetCPUDescriptorHandleForHeapStart();
    const unsigned stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for(auto* resource:{atlas.Get(),depth.Get(),motion.Get()}){
        device->CreateRenderTargetView(resource,nullptr,handle);const float value[]{resource==depth.Get()?.5f:0.f,0,0,1};
        commands->ClearRenderTargetView(handle,value,0,nullptr);handle.ptr+=stride;
    }
    check(SUCCEEDED(commands->Close()),"Close source initialization");ID3D12CommandList* lists[]{commands.Get()};queue->ExecuteCommandLists(1,lists);
    using buffered_probe::Runtime;
    Runtime::device=device.Get();Runtime::chains.clear();Runtime::guides={};Runtime::snapshots=nullptr;
    Runtime::guides.producer_queue=queue.Get();Runtime::capture_token=2;
    for(unsigned eye=0;eye<2;++eye){
        auto& e=Runtime::guides.eyes[eye];e=g;e.focus_width=.5f;e.focus_height=.25f;e.resolution_percent=100;
        e.center_method=3;e.peripheral_method=1;e.configuration=1;e.depth=depth.Get();e.motion=motion.Get();
        e.depth_width=1360;e.depth_height=1387;e.motion_width=2040;e.motion_height=2080;
        e.submitted_crop.rect[0]=eye?495.f/2040:0;e.submitted_crop.rect[2]=(eye?1545.f:1544.f)/2040;
    }
    BufferedPresenter p;check(p.setup(h<XrSession>(900),Runtime::functions(),device.Get(),queue.Get()),"Source crop presenter");
    p.trace=[](const char* text) noexcept {std::cout<<text<<'\n';};
    p.guide_stamp=Runtime::stamp;p.guide_acquire=Runtime::acquire_guides;p.guide_release=Runtime::release_guides;
    p.capture_crops_query=source_crop_test::peek;
    p.storage_options.full_eye_crop_compatibility=1;p.storage_options.peripheral_percent=50;
    BufferedBackend12 backend(p);
    const auto sc=h<XrSwapchain>(901);
    std::array<XrCompositionLayerProjectionView,2> views{view,view};
    for(unsigned eye=0;eye<2;++eye)views[eye].subImage={sc,eye?right:left,0};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.views=views.data();projection.viewCount=2;
    std::shared_ptr<BufferedNative> previous;
    for(unsigned index=0;index<4;++index){
        const unsigned mode=std::min(index,2u);
        auto& current=source_crop_test::current;current={};current.token=Runtime::capture_token;
        current.width=2040;current.height=2080;current.valid_mask=3;
        for(unsigned eye=0;eye<2;++eye){
            const auto& e=Runtime::guides.eyes[eye];current.source_crop[eye]=e.submitted_crop;
            current.eye[eye]={e.focus_width,e.focus_height,e.focus_x,e.focus_y};
        }
        p.storage_options.compact=mode==2;p.storage_options.full_eye_compact_capture=mode==2;
        if(mode){BufferedPresenter::CapturePlan plan;plan.desc=atlas->GetDesc();
            plan.slices={{left,0},{right,0}};plan.physical_eyes={0,1};p.capture_plans[sc]=plan;}
        check(p.capture(sc,0,atlas.Get(),4080,2080,1,DXGI_FORMAT_R8G8B8A8_UNORM),"Capture symmetric source atlas");
        auto job=std::make_shared<BufferedNative>();job->id=index+1;
        job->alignment.valid=job->alignment.gaze_valid=true;
        job->alignment.u={.5f,.5f};job->alignment.gaze_u={.25f,.75f};
        check(backend.capture(job,&projection,true,nullptr,nullptr)==CaptureOutcome::captured&&job->guides_valid,
            "Full-source color and matched guides pass consumer validation");
        p.producer.drain();
        check(std::abs(job->alignment.u[0]-772.f/2040)<.00001f&&
            std::abs(job->alignment.u[1]-(495.f+772.5f)/2040)<.00001f&&
            std::abs(job->alignment.gaze_u[1]-(495.f+1158.75f)/2040)<.00001f,
            "Captured full-source eyes retain per-frame alignment in source coordinates");
        for(unsigned eye=0;eye<2;++eye){
            const auto& image=job->color[eye];check(image->source_eye.active&&image->source_eye.visible.extent.width==int(eye?1545:1544),
                "Captured eyes retain distinct visible widths");
            check(image->layout.packed()==(mode==2),"Compact toggle selects full-source packing only when requested");
            check((image->layout.width?image->layout.width:image->texture->GetDesc().Width)==2040,
                "Logical color width stays source-sized in both storage modes");
        }
        if(index==0){
            UniversalNr12 nr;
            check(nr.commands(device.Get(),queue.Get()),"Initialize Universal NR copy commands");
            auto copied=nr.copy_image(p.output,job->color[1],DXGI_FORMAT_R8G8B8A8_UNORM,2040,2080);
            check(bool(copied),"Copy full-source eye for Universal NR");
            const bool retained=copied->source_eye==job->color[1]->source_eye;
            check(SUCCEEDED(nr.lists[0]->Close()),"Close Universal NR copy");
            ID3D12CommandList* copy_lists[]{nr.lists[0].Get()};
            queue->ExecuteCommandLists(1,copy_lists);
            nr.lists[1]->Close();nr.lists[2]->Close();
            check(SUCCEEDED(queue->Signal(nr.fence.Get(),++nr.serial))&&nr.wait(),"Complete Universal NR copy");
            check(retained,"Universal NR copies must retain source-eye crop and revision");
            auto nr_images=job->color;nr_images[1]=copied;
            auto nr_views=views;BufferedPair nr_pair;FrameLayers nr_layers;
            check(backend.publish(nr_pair,nr_images,nr_views,nr_layers,false)&&
                nr_views[1].subImage.imageRect.offset.x==495&&
                nr_views[1].subImage.imageRect.extent.width==1545&&nr_views[1].fov.angleLeft==views[1].fov.angleLeft,
                "Universal NR copies present the original visible crop and FOV");
            p.uploads.drain();
        }
        if(index==1||index==3){
            bool reset=true;std::uint64_t samples{};std::shared_ptr<BufferedDirectRequest> direct;
            BufferedPair generated;job->direct_output_mode=1;
            const auto outcome=backend.generate(previous,job,generated,reset,samples,direct);
            p.worker.wait_idle();
            check(outcome.attempted&&outcome.success&&!generated.direct_xr,
                "DLSS Vectors generates both full-source eyes without waiting for direct XR output");
            for(unsigned eye=0;eye<2;++eye)
                check(generated.generated[eye]->source_eye==job->color[eye]->source_eye,
                    "Generated images inherit immutable source/visible geometry");
            check(p.direct_output_details().reason==CheekyFgDirectReason::source_crop,"Direct output explains full-eye fallback");
            auto moved=job->color[1]->source_eye;++job->color[1]->source_eye.revision;
            reset=false;
            const auto rejected=backend.generate(previous,job,generated,reset,samples,direct);
            check(!rejected.attempted&&reset,"Mapping changes invalidate history before interpolation resumes");
            job->color[1]->source_eye=moved;
        }
        BufferedPair pair;FrameLayers layers;auto presented=views;
        check(backend.publish(pair,job->color,presented,layers,false),"Publish source eyes through private chains");
        for(unsigned eye=0;eye<2;++eye){
            check(presented[eye].subImage.imageRect.extent.width==int(eye?1545:1544)&&
                presented[eye].subImage.imageRect.offset.x==int(eye?495:0)&&
                presented[eye].fov.angleLeft==views[eye].fov.angleLeft,"Presentation restores original crop and FOV");
        }
        p.uploads.drain();Runtime::capture_token+=2;previous=job;
    }
    source_crop_test::current.token=Runtime::capture_token;
    check(p.capture(sc,0,atlas.Get(),4080,2080,1,DXGI_FORMAT_R8G8B8A8_UNORM),"Capture before mapping changes");
    ++Runtime::guides.eyes[1].submitted_crop.revision;
    auto stale=std::make_shared<BufferedNative>();stale->id=5;
    check(backend.capture(stale,&projection,true,nullptr,nullptr)==CaptureOutcome::captured&&!stale->guides_valid,
        "A guide mapping revision changed after capture rejects the pair without losing native color");
    p.producer.drain();
    for(bool enabled:{false,true}){
        p.storage_options.full_eye_crop_compatibility=enabled;
        Runtime::capture_token+=2;source_crop_test::current.token=Runtime::capture_token;
        for(unsigned eye=0;eye<2;++eye){
            auto& e=Runtime::guides.eyes[eye];e.full_eye_crop_compatibility=enabled;e.submitted_crop={};
            source_crop_test::current.source_crop[eye]={};
        }
        check(p.capture(sc,0,atlas.Get(),4080,2080,1,DXGI_FORMAT_R8G8B8A8_UNORM),"Capture native fallback without verified mapping");
        auto native=std::make_shared<BufferedNative>();native->id=enabled?7:6;
        check(backend.capture(native,&projection,true,nullptr,nullptr)==CaptureOutcome::captured&&!native->guides_valid,
            "Off or uncalibrated compatibility never bypasses strict guide size validation");
        for(unsigned eye=0;eye<2;++eye)
            check(!native->color[eye]->source_eye.active&&native->color[eye]->texture->GetDesc().Width==(eye?1545:1544),
                "Unverified mapping retains the original cropped native color");
        check(std::strstr(native->guide_failure,enabled?"no verified":"is off")!=nullptr,
            "Native fallback explicitly explains unverified or disabled crop mapping");
        p.producer.drain();
    }
    p.guide_stamp=Runtime::missing_stamp;p.color_stamp_frame=p.color_stamp_token=0;
    Runtime::capture_token+=2;source_crop_test::current.token=Runtime::capture_token;
    for(unsigned eye=0;eye<2;++eye){
        auto& crop=source_crop_test::current.source_crop[eye];crop.valid=1;crop.revision=7;
        crop.rect[0]=eye?495.f/2040:0;crop.rect[2]=(eye?1545.f:1544.f)/2040;
    }
    check(p.capture(sc,0,atlas.Get(),4080,2080,1,DXGI_FORMAT_R8G8B8A8_UNORM),"Capture unstamped native source");
    auto unstamped=std::make_shared<BufferedNative>();unstamped->id=8;
    check(backend.capture(unstamped,&projection,true,nullptr,nullptr)==CaptureOutcome::captured&&!unstamped->guides_valid&&
        !unstamped->color[0]->source_eye.active&&!unstamped->color[1]->source_eye.active,
        "An unanchored crop peek never authorizes source-eye recovery");
    p.producer.drain();
    p.storage_options.full_eye_crop_compatibility=1;
    p.color_options_query=source_crop_test::color_options;
    auto& color=source_crop_test::color;color={};color.enabled=1;
    for(unsigned eye=0;eye<2;++eye){
        auto& e=color.eyes[eye];e=Runtime::guides.eyes[eye];e.full_eye_crop_compatibility=1;
        e.center_method=cheeky_fg_optical_flow;e.focus_width=e.focus_height=1;
        e.submitted_crop=source_crop_test::current.source_crop[eye];e.configuration=1;
    }
    std::shared_ptr<BufferedNative> previous_color;
    for(unsigned index=0;index<2;++index){
        check(p.capture(sc,0,atlas.Get(),4080,2080,1,DXGI_FORMAT_R8G8B8A8_UNORM),"Capture color-only source");
        auto job=std::make_shared<BufferedNative>();job->id=9+index;
        check(backend.capture(job,&projection,true,nullptr,nullptr)==CaptureOutcome::captured&&job->guides_valid,
            "Color-only full-source mapping works without a guide token");
        for(unsigned eye=0;eye<2;++eye)
            check(job->color[eye]->source_eye.active&&job->color[eye]->texture->GetDesc().Width==2040,
                "Color-only capture retains full source dimensions for both eyes");
        if(hardware&&previous_color){
            bool reset=true;std::uint64_t samples{};std::shared_ptr<BufferedDirectRequest> direct;
            BufferedPair generated;
            const auto outcome=backend.generate(previous_color,job,generated,reset,samples,direct);
            check(outcome.success&&outcome.attempted,"Physical Optiflow generates full-source color-only eyes");
            for(unsigned eye=0;eye<2;++eye)
                check(generated.generated[eye]->source_eye==job->color[eye]->source_eye,
                    "Physical Optiflow output retains each visible crop");
        }
        auto presented=views;BufferedPair pair;FrameLayers layers;
        check(backend.publish(pair,job->color,presented,layers,false)&&
            presented[1].subImage.imageRect.offset.x==495&&presented[1].subImage.imageRect.extent.width==1545,
            "Color-only presentation preserves the visible crop");
        p.producer.drain();p.uploads.drain();previous_color=job;
    }
    check(p.capture(sc,0,atlas.Get(),4080,2080,1,DXGI_FORMAT_R8G8B8A8_UNORM),"Capture color before calibration changes");
    ++color.eyes[0].submitted_crop.revision;++color.eyes[1].submitted_crop.revision;
    auto changed_color=std::make_shared<BufferedNative>();changed_color->id=11;
    check(backend.capture(changed_color,&projection,true,nullptr,nullptr)==CaptureOutcome::captured&&!changed_color->guides_valid,
        "Color-only direct capture rejects a changed calibration revision");
    p.producer.drain();
    color.eyes[0].submitted_crop.valid=0;
    check(p.capture(sc,0,atlas.Get(),4080,2080,1,DXGI_FORMAT_R8G8B8A8_UNORM),"Capture color without verified mapping");
    auto missing_color=std::make_shared<BufferedNative>();missing_color->id=12;
    check(backend.capture(missing_color,&projection,true,nullptr,nullptr)==CaptureOutcome::captured&&!missing_color->guides_valid,
        "Color-only unequal unverified eye sizes never enter generation");
    p.producer.drain();
    p.reset();check(Runtime::chains.empty(),"Crop test releases private chains");
    std::cout<<"PASS full-source crop geometry, projection, scaled guides, atlas fallback, direct/compact capture and original presentation bounds\n";
}
