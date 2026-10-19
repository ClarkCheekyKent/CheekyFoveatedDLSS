#pragma once

// Exercise the actual runtime exports, native D3D12 observer and intercepted
// NGX calls. A shader need not propagate anything: the executing RHI ticket is
// the authority at guide evaluation and at the final color-copy submission.
void fg_runtime_frame_validation(HMODULE runtime,const std::filesystem::path& bin,ID3D12Device* device,ID3D12CommandQueue* queue) {
    auto stamp=reinterpret_cast<CheekyFgCaptureStampFn>(GetProcAddress(runtime,"CheekyFG_GetCaptureStamp"));
    auto acquire=reinterpret_cast<CheekyFgAcquireFn>(GetProcAddress(runtime,"CheekyFG_AcquirePair"));
    auto release=reinterpret_cast<CheekyFgReleaseFn>(GetProcAddress(runtime,"CheekyFG_ReleasePair"));
    auto set_frame=reinterpret_cast<void(__cdecl*)(std::uint64_t)>(GetProcAddress(runtime,"CheekyFG_SetRenderFrameV1"));
    auto health=reinterpret_cast<CheekyFgCaptureHealthFn>(GetProcAddress(runtime,"CheekyFG_GetCaptureHealth"));
    require(stamp&&acquire&&release&&set_frame,"Explicit RHI frame bridge exports");
    CheekyFgCaptureHealth status;
    if(health&&health(&status)&&!status.plugin_hook){
        std::printf("Frame source: %s\n",status.source);
    }
    require(health&&health(&status)&&status.plugin_hook&&fg_test_hook_installs==2,
        "Plugin installs the frame source through the existing backend hook API");
    auto executable=uevr_frames::image(GetModuleHandleW(nullptr));
    require(!uevr_frames::frame_method(executable,reinterpret_cast<std::uintptr_t>((*reinterpret_cast<void***>(&fg_test_runtime))[13]))&&
        !uevr_frames::locate(GetModuleHandleW(L"kernel32.dll")).function,
        "Frame discovery rejects an unrelated virtual method and an unsupported module");
    auto ngx=LoadLibraryW((bin/"test-fixtures/nvngx_dlss.dll").c_str());require(ngx!=nullptr,"Frame fixture NGX runtime");
    // Hook installation runs on a worker. Wait for the actual completion
    // record instead of assuming a fixed sleep is enough on a loaded PC.
    bool evaluate_hooked{};const auto hook_deadline=GetTickCount64()+10000;
    do {
        Sleep(25);present();std::ifstream log(std::filesystem::path(directory)/"CheekyFoveatedDLSS-UEVR.log");
        const std::string text((std::istreambuf_iterator<char>(log)),{});
        evaluate_hooked=text.find("Direct detour installed export=NVSDK_NGX_D3D12_EvaluateFeature ")!=std::string::npos;
    }while(!evaluate_hooked&&GetTickCount64()<hook_deadline);
    require(evaluate_hooked,"Actual D3D12 NGX evaluation hook is ready");
    using Create=NgxResult(*)(ID3D12GraphicsCommandList*,unsigned,NgxParameters*,NgxHandle**);
    using Evaluate=NgxResult(*)(ID3D12GraphicsCommandList*,const NgxHandle*,const NgxParameters*,NgxProgressCallback);
    auto create=reinterpret_cast<Create>(GetProcAddress(ngx,"NVSDK_NGX_D3D12_CreateFeature"));
    auto evaluate=reinterpret_cast<Evaluate>(GetProcAddress(ngx,"NVSDK_NGX_D3D12_EvaluateFeature"));require(create&&evaluate,"Frame NGX exports");
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Frame allocator");
    check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"Frame commands");
    auto texture=[&](DXGI_FORMAT format){
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
        d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=64;d.Format=format;
        d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> result;check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&result)),"Frame texture");return result;
    };
    auto depth=texture(DXGI_FORMAT_R32_FLOAT),motion=texture(DXGI_FORMAT_R32G32_FLOAT),color=texture(DXGI_FORMAT_R8G8B8A8_UNORM),output=texture(DXGI_FORMAT_R8G8B8A8_UNORM);
    ComPtr<ID3D12DescriptorHeap> rtvs;D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap_desc.NumDescriptors=3;
    check(device->CreateDescriptorHeap(&heap_desc,IID_PPV_ARGS(&rtvs)),"Frame fixture RTV heap");
    const auto rtv_stride=device->GetDescriptorHandleIncrementSize(heap_desc.Type);unsigned rtv_index{};
    for(auto* resource:{depth.Get(),motion.Get(),color.Get()}){
        auto rtv=rtvs->GetCPUDescriptorHandleForHeapStart();rtv.ptr+=rtv_index++*rtv_stride;device->CreateRenderTargetView(resource,nullptr,rtv);
    }
    auto write_pixels=[&](ID3D12Resource* resource,unsigned index,float value){
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=resource;
        b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;list->ResourceBarrier(1,&b);
        auto rtv=rtvs->GetCPUDescriptorHandleForHeapStart();rtv.ptr+=index*rtv_stride;const float values[]{value,value,value,1};
        list->ClearRenderTargetView(rtv,values,0,nullptr);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);
    };
    MockNgxParameters p;p.Set("Width",64u);p.Set("Height",64u);p.Set("OutWidth",64u);p.Set("OutHeight",64u);
    p.Set("Depth",depth.Get());p.Set("MotionVectors",motion.Get());p.Set("Color",color.Get());p.Set("Output",output.Get());
    p.Set("MV.Scale.X",1.f);p.Set("MV.Scale.Y",1.f);p.Set("Reset",0u);
    MockNgxParameters creation;creation.Set("Width",64u);creation.Set("Height",64u);creation.Set("OutWidth",64u);creation.Set("OutHeight",64u);
    creation.Set("DLSS.Feature.Create.Flags",14); // Signed creation flags, absent from evaluation parameters.
    NgxHandle* handle{};require(ngx_succeeded(create(list.Get(),1,&creation,&handle))&&handle,"Frame feature creation");
    auto submit=[&]{check(list->Close(),"Frame close");ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);wait_gpu(device,queue);};
    auto begin=[&]{check(allocator->Reset(),"Frame allocator reset");check(list->Reset(allocator.Get(),nullptr),"Frame list reset");};
    auto guides=[&](unsigned frame){
        fg_test_render_frame(frame);
        p.Set("Jitter.Offset.X",frame==77?.25f:.75f);
        require(ngx_succeeded(evaluate(list.Get(),handle,&p,nullptr))&&ngx_succeeded(evaluate(list.Get(),handle,&p,nullptr)),"Actual intercepted stereo guide evaluations");
    };
    CheekyFgCaptureStamp s;require(stamp(output.Get(),&s)&&!s.token,"First color registration does not infer a frame");
    guides(77);submit();
    begin();
    auto copy=[&]{
        D3D12_RESOURCE_BARRIER barriers[2]{};for(auto& b:barriers){b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;}
        barriers[0].Transition.pResource=color.Get();barriers[0].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[1].Transition.pResource=output.Get();barriers[1].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(2,barriers);list->CopyResource(output.Get(),color.Get());
        for(auto& b:barriers)std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(2,barriers);
    };
    copy();submit();begin();guides(78);submit(); // Newer guides exist BEFORE old color is acquired.
    require(stamp(output.Get(),&s)&&s.token&&s.render_frame==77,"Color retains the executed RHI frame despite newer DLSS inputs");
    CheekyFgPair pair;pair.capture_token=s.token;pair.render_frame=78;
    require(!acquire(&pair),"Matching evaluation counters cannot override a different RHI frame identity");
    pair.render_frame=77;require(acquire(&pair)&&pair.eyes[0].render_frame==77&&pair.eyes[1].render_frame==77,"Both guides belong to the color's RHI frame");
    require(pair.release_point==3&&pair.submit_repeats==0,"Lua generated-submission gate reaches the real captured stereo pair");
    require(pair.eyes[0].depth_inverted&&pair.eyes[1].depth_inverted&&pair.eyes[0].motion_jittered&&pair.eyes[1].motion_jittered,
        "Signed creation flags survive a separate evaluation parameter object");release(&pair);
    begin();copy();submit();require(stamp(output.Get(),&s)&&s.render_frame==78,"Next color uses its own RHI frame");
    pair={};pair.capture_token=s.token;pair.render_frame=78;require(acquire(&pair),"Next matching frame lease");
    require(std::abs(pair.eyes[0].jitter_correction_x+.5f/64)<.00001f&&std::abs(pair.eyes[1].jitter_correction_x+.5f/64)<.00001f,
        "Jittered native motion is corrected against the prior same-eye frame");release(&pair);
    begin();guides(79);copy(); // Both guide captures and final color await the SAME enqueue.
    require(stamp(output.Get(),&s)&&s.render_frame==78,"An unsubmitted color recording cannot publish frame 79");
    submit();require(stamp(output.Get(),&s)&&s.token&&s.render_frame==79,"Same-command-list guides and color resolve their exact frame after enqueue");
    pair={};pair.capture_token=s.token;pair.render_frame=79;require(acquire(&pair),"Same-list matching guide lease");release(&pair);
    fg_test_render_frame(80);begin();copy();submit();
    require(stamp(output.Get(),&s)&&!s.token&&s.render_frame==80&&health(&status)&&status.color_frame==80&&status.last_guide_frame==79&&
        std::string(status.match_reason)=="No guides with the color frame ID",
        "Color/guide ID mismatch reports the actual frame numbers and rejection reason");
    set_frame(0);begin();copy();submit(); // No marker: never invent a newer frame.
    pair={};pair.capture_token=s.token;pair.render_frame=79;require(!acquire(&pair),"Missing RHI marker cannot reuse consumed guides");
    require(health(&status)&&status.frame_calls>=3&&status.last_guide_frame==79&&status.captured_guides>=6,
        "Actual inline-hook frame calls reach the resident runtime and matching guide capture");
    fg_test_render_body=[&]{
        const float value=float(fg_test_command_frame-9000)*.1f;
        write_pixels(depth.Get(),0,value);write_pixels(motion.Get(),1,value);
        require(ngx_succeeded(evaluate(list.Get(),handle,&p,nullptr))&&ngx_succeeded(evaluate(list.Get(),handle,&p,nullptr)),
            "RHI command executes the real intercepted stereo evaluations before its late pose marker");
        write_pixels(color.Get(),2,value);
    };
    begin();fg_test_execute_frame(9000);copy();submit(); // Discover which verified RHI entry supplies the marker.
    for(unsigned frame=9001;frame<=9002;++frame){
        begin();fg_test_execute_frame(frame);copy();submit();
        require(stamp(output.Get(),&s)&&s.token&&s.render_frame>=0x100000000ull,"Command entry supplies the color's exact execution ticket");
        pair={};pair.capture_token=s.token;pair.render_frame=s.render_frame;
        require(acquire(&pair)&&pair.eyes[0].render_frame==s.render_frame&&pair.eyes[1].render_frame==s.render_frame,
            "Late enqueue_render_poses must not retag color one frame ahead of guides");release(&pair);
    }
    // UEVR can enter the next render command before copying the preceding
    // completed color. Its new entry ticket must not relabel those old pixels.
    begin();fg_test_execute_frame(9003);submit();
    require(health(&status),"Completed guide frame before next RHI entry");
    const auto completed_frame=status.last_guide_frame;
    auto check_pixels=[&](const CheekyFgPair& captured,float expected){
        // Read actual final color and both immutable depth snapshots. Ticket-
        // only assertions cannot verify the temporal contents of these copies.
        ComPtr<ID3D12CommandAllocator> read_allocator;ComPtr<ID3D12GraphicsCommandList> read_list;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&read_allocator)),"Phase read allocator");
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,read_allocator.Get(),nullptr,IID_PPV_ARGS(&read_list)),"Phase read commands");
        ID3D12Resource* resources[]{output.Get(),captured.eyes[0].depth,captured.eyes[1].depth};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[3]{};UINT64 bytes{};
        for(unsigned i=0;i<3;++i){auto desc=resources[i]->GetDesc();UINT64 size{};
            device->GetCopyableFootprints(&desc,0,1,bytes,&footprints[i],nullptr,nullptr,&size);
            bytes=(size+bytes+D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1)&~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1);
        }
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC bd{};
        bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"Phase readback");
        for(unsigned i=0;i<3;++i){
            D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=resources[i];
            b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=i?D3D12_RESOURCE_STATE_COMMON:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;read_list->ResourceBarrier(1,&b);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=resources[i];from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprints[i];
            read_list->CopyTextureRegion(&to,0,0,0,&from,nullptr);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);read_list->ResourceBarrier(1,&b);
        }
        check(read_list->Close(),"Phase readback close");ID3D12CommandList* reads[]{read_list.Get()};queue->ExecuteCommandLists(1,reads);wait_gpu(device,queue);
        void* mapped{};D3D12_RANGE range{0,SIZE_T(bytes)};check(readback->Map(0,&range,&mapped),"Phase readback map");
        const auto* pixels=static_cast<const unsigned char*>(mapped);
        require(std::abs(float(pixels[footprints[0].Offset])/255-expected)<.005f&&
            std::abs(*reinterpret_cast<const float*>(pixels+footprints[1].Offset)-expected)<.0001f&&
            std::abs(*reinterpret_cast<const float*>(pixels+footprints[2].Offset)-expected)<.0001f,
            "Actual copied color and both guide snapshots must contain the same temporal frame");
        D3D12_RANGE empty{};readback->Unmap(0,&empty);
    };
    fg_test_before_render_body=[&]{
        copy();submit();
        require(stamp(output.Get(),&s)&&s.token&&s.render_frame==completed_frame,
            "Next RHI entry must preserve the completed color's guide ticket before new DLSS work");
        pair={};pair.capture_token=s.token;pair.render_frame=s.render_frame;
        require(acquire(&pair)&&pair.eyes[0].render_frame==completed_frame&&pair.eyes[1].render_frame==completed_frame,
            "Early color copy retains its completed stereo pair");check_pixels(pair,.3f);release(&pair);begin();
    };
    begin();fg_test_execute_frame(9004);copy();submit();fg_test_before_render_body={};
    require(stamp(output.Get(),&s)&&s.token&&s.render_frame!=completed_frame,
        "Color copied after new stereo evaluations uses the new completed pair");
    pair={};pair.capture_token=s.token;pair.render_frame=s.render_frame;require(acquire(&pair),"New completed color lease");check_pixels(pair,.4f);release(&pair);
    // Empty/partial intervals cannot keep borrowing an older completed frame.
    // An invalid observation must also clear an existing resource color tag.
    fg_test_render_body={};begin();fg_test_execute_frame(9005);submit();
    begin();fg_test_execute_frame(9006);copy();submit();
    require(stamp(output.Get(),&s)&&!s.token&&!s.render_frame,"An empty render interval cannot reuse older guide content");
    fg_test_render_body=[&]{require(ngx_succeeded(evaluate(list.Get(),handle,&p,nullptr)),"Partial stereo fixture");copy();};
    begin();fg_test_execute_frame(9007);submit();
    require(stamp(output.Get(),&s)&&!s.token&&!s.render_frame,"A partial stereo interval clears the prior color provenance");
    fg_test_render_body={};require(health(&status)&&std::string(status.source)=="Plugin hook: RHI stereo phases",
        "Frame source reports the selected original RHI command entry");
    std::puts("PASS late pose-marker regression: RHI entry stamps both guides and later color with one ticket; all 12 arguments forwarded");
    std::puts("PASS next-entry color regression: early old color and later new color match both guide tickets AND GPU pixels; empty/partial intervals rejected");
    std::puts("PASS plugin-owned inline frame hook, original forwarding, native observer, delayed color/guide matching and mismatched-frame rejection");
}
