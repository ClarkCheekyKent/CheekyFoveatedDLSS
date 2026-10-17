#pragma once
inline cheeky::fg::Worker::Evaluate fg_probe_real_evaluate{};
inline bool fg_probe_native_depth{},fg_probe_sdr{},fg_probe_legacy_mvec{};
inline NVSDK_NGX_Result NVSDK_CONV fg_probe_evaluate(ID3D12GraphicsCommandList* commands,const NVSDK_NGX_Handle* feature,
    const NVSDK_NGX_Parameter* params,PFN_NVSDK_NGX_ProgressCallback callback) {
    void* projection{};unsigned inverted{},hdr{};
    check(NVSDK_NGX_SUCCEED(params->Get("DLSSG.CameraViewToClip",&projection))&&projection,"Actual NGX camera matrix");
    params->Get("DLSSG.DepthInverted",&inverted);params->Get("DLSSG.ColorBuffersHDR",&hdr);
    const auto* m=static_cast<const float*>(projection);
    fg_probe_native_depth|=inverted&&m[10]>=0&&m[14]>0;
    fg_probe_sdr|=hdr==0;
    check(inverted&&m[10]>=0&&m[14]>0,"NGX must receive reversed-Z exactly once, including cropped projections");
    ID3D12Resource* depth{};params->Get("DLSSG.Depth",&depth);
    float mx{},my{};params->Get("DLSSG.MvecScaleX",&mx);params->Get("DLSSG.MvecScaleY",&my);
    check(depth&&mx==float(depth->GetDesc().Width)&&my==float(depth->GetDesc().Height),
        "Normalized motion must be scaled to the actual NGX guide dimensions");
    if(fg_probe_legacy_mvec){
        auto* editable=const_cast<NVSDK_NGX_Parameter*>(params);
        editable->Set("DLSSG.MvecScaleX",1.f);editable->Set("DLSSG.MvecScaleY",1.f);}
    return fg_probe_real_evaluate(commands,feature,params,callback);
}
// Opt-in integration probe. Uses the production worker, the physical GPU and
// real NVIDIA inference, without starting a game or an OpenXR session.
inline void fg_hardware_probe(cheeky::fg::Worker& worker,ID3D12Device* device,bool bgra=false,bool foveated=false,
    unsigned method=2,unsigned scale=100,unsigned peripheral_scale=50,bool game_guides=false,unsigned width=512,unsigned height=512,
    bool motion_test=false,bool full_guides=false) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};
    check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"Hardware probe queue");
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))),"Probe allocator");
    check(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))),"Probe list");
    ComPtr<ID3D12DescriptorHeap> rtvs;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=4;
    check(SUCCEEDED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs))),"Probe RTV heap");
    const auto stride=device->GetDescriptorHandleIncrementSize(hd.Type);
    ComPtr<ID3D12Resource> color,right_color,depth,motion;
    auto make=[&](DXGI_FORMAT format,unsigned index,const float* clear,ComPtr<ID3D12Resource>& texture) {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;
        if(game_guides&&!full_guides&&(index==1||index==2)){d.Width=width/2;d.Height=height/2;}
        d.DepthOrArraySize=d.MipLevels=1;d.Format=game_guides&&index==1?DXGI_FORMAT_R32_TYPELESS:format;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&texture))),"Probe texture");
        auto slot=rtvs->GetCPUDescriptorHandleForHeapStart();slot.ptr+=stride*index;
        D3D12_RENDER_TARGET_VIEW_DESC view{};view.Format=format;view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(texture.Get(),&view,slot);list->ClearRenderTargetView(slot,clear,0,nullptr);
        cheeky::fg::color_transition(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);
    };
    const float red[]{1,.1f,.05f,1},z[]{motion_test?.04999525f:.5f,0,0,0},zero[]{motion_test?-16.f/width:0.f,0,0,0};
    const float blue[]{.05f,.1f,1,1};
    const auto color_format=bgra?DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM;
    make(color_format,0,red,color);make(DXGI_FORMAT_R32_FLOAT,1,z,depth);make(game_guides?DXGI_FORMAT_R16G16_FLOAT:DXGI_FORMAT_R32G32_FLOAT,2,zero,motion);
    make(color_format,3,blue,right_color);
    check(SUCCEEDED(list->Close()),"Probe close");ID3D12CommandList* commands[]{list.Get()};queue->ExecuteCommandLists(1,commands);
    ComPtr<ID3D12Fence> fence;check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))),"Probe fence");
    check(SUCCEEDED(queue->Signal(fence.Get(),1)),"Probe signal");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event!=nullptr,"Probe event");
    check(SUCCEEDED(fence->SetEventOnCompletion(1,event)),"Probe completion event");
    const auto waited=WaitForSingleObject(event,10000);CloseHandle(event);check(waited==WAIT_OBJECT_0,"Probe initialization completion");
    check(worker.initialize(device,queue.Get(),width,height,color_format),worker.diagnostic);
    fg_probe_real_evaluate=worker.evaluate;worker.evaluate=fg_probe_evaluate;fg_probe_native_depth=fg_probe_sdr=false;
    CheekyFgPair guides{};guides.producer_queue=queue.Get();
    for(auto& eye:guides.eyes) {
        eye.depth=depth.Get();eye.motion=motion.Get();eye.width=width;eye.height=height;
        eye.depth_width=eye.motion_width=game_guides&&!full_guides?width/2:width;
        eye.depth_height=eye.motion_height=game_guides&&!full_guides?height/2:height;
        if(foveated) {eye.focus_width=.55f;eye.focus_height=.45f;}
        eye.peripheral_method=method;eye.resolution_percent=scale;eye.peripheral_percent=peripheral_scale;
        eye.depth_inverted=1;
        if(motion_test){eye.motion_scale_x=float(width);eye.motion_scale_y=float(height);}
        if(game_guides&&!full_guides)eye.depth_bits=eye.motion_bits=16;
    }
    XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    view.pose.orientation.w=1;view.fov={-.7f,.7f,.7f,-.7f};
    const auto camera=cheeky::fg::camera(view,&view,true);
    std::array<cheeky::fg::Camera,2> cameras{camera,camera};
    std::array<ID3D12Resource*,2> colors{color.Get(),motion_test?color.Get():right_color.Get()},outputs{};
    ComPtr<ID3D12Resource> upload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT upload_footprint{};UINT64 upload_bytes{};
    if(motion_test){
        auto d=color->GetDesc();device->GetCopyableFootprints(&d,0,1,0,&upload_footprint,nullptr,nullptr,&upload_bytes);
        D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=upload_bytes;rd.Height=1;rd.DepthOrArraySize=rd.MipLevels=rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_UPLOAD;
        check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload))),"Moving-scene upload");
    }
    unsigned last_frame{};
    auto move_scene=[&](unsigned frame){
        last_frame=frame;void* mapped{};D3D12_RANGE empty{};check(SUCCEEDED(upload->Map(0,&empty,&mapped)),"Moving-scene map");
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
            auto* pixel=static_cast<unsigned char*>(mapped)+upload_footprint.Offset+y*upload_footprint.Footprint.RowPitch+x*4;
            const bool bar=x>=96+frame*16&&x<144+frame*16;
            pixel[0]=pixel[1]=pixel[2]=bar?255:0;pixel[3]=255;
        }upload->Unmap(0,nullptr);allocator->Reset();list->Reset(allocator.Get(),nullptr);
        cheeky::fg::color_transition(list.Get(),color.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=upload_footprint;
        to.pResource=color.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        cheeky::fg::color_transition(list.Get(),color.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        list->Close();queue->ExecuteCommandLists(1,commands);
        auto now=view,previous_view=view;const float step=2*std::tan(.7f)*16/width;
        now.pose.position.x=-float(frame)*step;previous_view.pose.position.x=-float(frame?frame-1:0)*step;
        cameras.fill(cheeky::fg::camera(now,&previous_view,true));
    };
    bool generated{};
    for(unsigned frame=0;frame<6&&!worker.failed;++frame) {
        for(auto& eye:guides.eyes)eye.reset=frame==0;
        if(motion_test)move_scene(frame);
        generated=worker.generate(guides,colors,colors,cameras,outputs);
        std::cout << "Hardware evaluation " << frame << ": " << (generated?"generated":"warmup/failure") << '\n';
        if(generated)break;
    }
    check(generated,worker.diagnostic);
    check(fg_probe_native_depth&&(worker.force_fp16||fg_probe_sdr),"Actual SDK camera/HDR parameters match the native depth and color formats");
    if(motion_test)for(unsigned frame=last_frame+1;frame<=12;++frame){move_scene(frame);check(worker.generate(guides,colors,colors,cameras,outputs),"Moving-scene interpolation");}
    else if(width>512)for(unsigned frame=0;frame<128;++frame)check(worker.generate(guides,colors,colors,cameras,outputs),"Steady benchmark generation");
    const auto& timing=worker.gpu_timings;
    check(timing.supported()&&timing.valid&&timing.samples>0,"Physical GPU timing sample must resolve after the ordinary fence wait");
    check(timing.center.last>0&&timing.total.last>=timing.center.last&&
        timing.foveated_eyes==(foveated?2u:0u),"GPU timing must distinguish foveated and full-frame generation");
    if(foveated)check(timing.periphery.last>0,"Foveated composition must have a real GPU timestamp interval");
    check(bool(worker.peripheral_features[0].handle)==(foveated&&method==3),"DLSS periphery must run its own NGX feature; custom modes must not");
    check(worker.center_width==std::max(32u,(unsigned(std::round(width*(foveated?.55f:1.f)))*scale+50u)/100u),"FG scale must change actual inference dimensions");
    const auto& center_input=worker.focus.inputs[0][0];
    const auto guide_width=unsigned(std::round(double(guides.eyes[0].depth_width)*worker.center_width/width));
    check(center_input.depth->GetDesc().Width==guide_width&&center_input.motion->GetDesc().Width==guide_width,
        "NGX must accept cropped source-resolution guides independently of output color");
    if(foveated){const auto& input=worker.focus.inputs[0][1];
        check(bool(input.color)==(method==3)&&bool(input.depth)==(method>=2)&&bool(input.motion)==(method!=0),
            "Only resources used by the periphery algorithm may be prepared");
        check(bool(worker.focus.previous_depth[0])==(method==2),"Only bidirectional warp may copy depth history");}
    std::cout << "Actual inputs: center=" << worker.center_width << 'x' << worker.center_height
        << " periphery=" << worker.peripheral_width << 'x' << worker.peripheral_height
        << " method=" << method << " source-depth=" << worker.source_depth_format << " source-motion=" << worker.source_motion_format << '\n';
    std::cout << "Actual center guides=" << center_input.depth->GetDesc().Width << 'x' << center_input.depth->GetDesc().Height;
    if(worker.focus.inputs[0][1].motion)std::cout << " periphery guides=" << worker.focus.inputs[0][1].motion->GetDesc().Width << 'x' << worker.focus.inputs[0][1].motion->GetDesc().Height;
    std::cout << '\n';
    std::cout << "GPU stereo ms (last): center=" << timing.center.last << " periphery=" << timing.periphery.last
        << " preparation=" << timing.preparation.last << " total=" << timing.total.last
        << "; completion wait CPU=" << worker.completion_wait.last << '\n';
    std::cout<<"GPU stereo ms (average): center="<<timing.center.mean()<<" periphery="<<timing.periphery.mean()
        <<" preparation="<<timing.preparation.mean()<<" total="<<timing.total.mean()<<" input format="<<unsigned(center_input.color->GetDesc().Format)<<'\n';
    check(timing.preparation_samples>0&&timing.sampled_preparation.mean()>0,"Hardware preparation detail samples");
    check(timing.preparation_samples<=(timing.samples+16)/17,"Preparation detail queries must be sampled, not recorded every frame");
    double accounted=timing.preparation_gaps.mean();
    for(unsigned pass=0;pass<4;++pass) {
        const auto& part=timing.preparation_parts[pass];for(const auto& phase:part)accounted+=phase.mean();
        std::cout<<"Sampled preparation eye="<<pass/2<<" pass="<<pass%2<<" before/dispatch/after ms="
            <<part[0].mean()<<'/'<<part[1].mean()<<'/'<<part[2].mean()<<'\n';
    }
    check(std::abs(accounted-timing.sampled_preparation.mean())<.001,"Preparation stages must account for their enclosing GPU scope");
    std::cout<<"Preparation detail samples="<<timing.preparation_samples<<" sampled total="<<timing.sampled_preparation.mean()
        <<" gaps="<<timing.preparation_gaps.mean()<<" ms\n";
    check(timing.order_samples[0]>0&&timing.order_samples[1]>0,"Both eye execution orders must be exercised");
    // Verify the interpolated texture actually contains the non-black scene.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};
    const auto desc=outputs[0]->GetDesc();device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT right_footprint{};UINT64 right_bytes{};
    const UINT64 right_offset=(bytes+511)&~UINT64(511);
    const auto right_desc=outputs[1]->GetDesc();device->GetCopyableFootprints(&right_desc,0,1,right_offset,&right_footprint,nullptr,nullptr,&right_bytes);
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb{};rb.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rb.Width=right_offset+right_bytes;rb.Height=1;
    rb.DepthOrArraySize=rb.MipLevels=1;rb.SampleDesc.Count=1;rb.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&rb,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))),"Probe readback");
    check(SUCCEEDED(allocator->Reset())&&SUCCEEDED(list->Reset(allocator.Get(),nullptr)),"Probe readback recording");
    cheeky::fg::color_transition(list.Get(),outputs[0],D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=outputs[0];from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
    list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    cheeky::fg::color_transition(list.Get(),outputs[0],D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    cheeky::fg::color_transition(list.Get(),outputs[1],D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
    from.pResource=outputs[1];to.PlacedFootprint=right_footprint;
    list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    cheeky::fg::color_transition(list.Get(),outputs[1],D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    check(SUCCEEDED(list->Close()),"Probe readback close");queue->ExecuteCommandLists(1,commands);
    check(SUCCEEDED(queue->Signal(fence.Get(),2)),"Probe readback signal");
    event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event!=nullptr,"Probe readback event");
    check(SUCCEEDED(fence->SetEventOnCompletion(2,event)),"Probe readback completion");
    const auto completed=WaitForSingleObject(event,10000);CloseHandle(event);check(completed==WAIT_OBJECT_0,"Probe readback wait");
    void* data{};check(SUCCEEDED(readback->Map(0,nullptr,&data)),"Probe map");
    const bool floating=desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT;
    const auto* pixel=static_cast<const unsigned char*>(data)+footprint.Offset+height/2*footprint.Footprint.RowPitch+width/2*(floating?8:4);
    if(motion_test){
        double sum{},weight{};const auto* row=static_cast<const unsigned char*>(data)+footprint.Offset+height/2*footprint.Footprint.RowPitch;
        for(unsigned x=0;x<width;++x){const auto value=row[x*4];sum+=(x+.5)*value;weight+=value;}
        const double midpoint=120+last_frame*16-8,measured=sum/std::max(1.,weight);
        std::cout<<"Moving-head reprojection: NGX scale="<<(fg_probe_legacy_mvec?"legacy identity":"guide dimensions")<<" expected center="<<midpoint<<" actual="<<measured<<" error="<<std::abs(measured-midpoint)<<" pixels\n";
        readback->Unmap(0,nullptr);check(weight>1000&&std::abs(measured-midpoint)<(fg_probe_legacy_mvec?3.:.25),"Generated scene must lie at the midpoint of the two rendered head poses");return;
    }
    std::cout << "Generated center format=" << unsigned(desc.Format) << " bytes: " << unsigned(pixel[0]) << ',' << unsigned(pixel[1]) << ',' << unsigned(pixel[2]) << ',' << unsigned(pixel[3]) << '\n';
    const bool nonblack=floating?(*reinterpret_cast<const unsigned short*>(pixel)>=0x3800):
        (pixel[0]>128&&pixel[0]>pixel[1]*2);
    if(bgra&&!floating)check(std::abs(int(pixel[1])-89)<=5&&std::abs(int(pixel[2])-63)<=5,
        "sRGB source must retain encoded colors when converted to RGBA8 inference/output");
    const auto* edge=static_cast<const unsigned char*>(data)+footprint.Offset+2*footprint.Footprint.RowPitch+2*(floating?8:4);
    const bool edge_nonblack=floating?(*reinterpret_cast<const unsigned short*>(edge)>=0x3800):edge[0]>128;
    const auto* right_pixel=static_cast<const unsigned char*>(data)+right_footprint.Offset+height/2*right_footprint.Footprint.RowPitch+width/2*(floating?8:4);
    const bool right_blue=floating?reinterpret_cast<const unsigned short*>(right_pixel)[2]>=0x3800:
        right_pixel[2]>128&&right_pixel[2]>right_pixel[0]*2;
    check(right_blue,"Alternating execution order must preserve the distinct blue right-eye output");
    readback->Unmap(0,nullptr);check(nonblack&&edge_nonblack,"Actual NGX output and generated periphery must contain the red scene");
    std::cout << "PASS alternating eye processing retains red left / blue right output pixels\n";
    std::cout << "PASS physical GPU NGX inference and non-black generated pixels\n";
}
