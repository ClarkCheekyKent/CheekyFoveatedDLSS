// A known red previous frame, blue current frame and green center distinguish
// real periphery algorithms and composition from a black/no-op dispatch.
void fg_periphery_validation(ID3D12Device* device,ID3D12CommandQueue* queue) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;UINT64 serial{};
    check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))),"Periphery allocator");
    check(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))),"Periphery list");
    check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))),"Periphery fence");
    auto execute=[&] {
        check(SUCCEEDED(list->Close()),"Periphery close");ID3D12CommandList* commands[]{list.Get()};queue->ExecuteCommandLists(1,commands);
        check(SUCCEEDED(queue->Signal(fence.Get(),++serial)),"Periphery signal");
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event!=nullptr,"Periphery event");
        check(SUCCEEDED(fence->SetEventOnCompletion(serial,event)),"Periphery completion");
        const auto result=WaitForSingleObject(event,10000);CloseHandle(event);check(result==WAIT_OBJECT_0,"Periphery wait");
    };
    auto begin=[&] {check(SUCCEEDED(allocator->Reset())&&SUCCEEDED(list->Reset(allocator.Get(),nullptr)),"Periphery reset");};
    ComPtr<ID3D12DescriptorHeap> rtvs;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=5;
    check(SUCCEEDED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs))),"Periphery RTV heap");
    const unsigned stride=device->GetDescriptorHandleIncrementSize(hd.Type);
    auto texture=[&](DXGI_FORMAT format,DXGI_FORMAT view,unsigned index,const float* clear) {
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
        d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=index>=3?32:64;d.DepthOrArraySize=d.MipLevels=1;
        d.Format=format;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> resource;check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,
            D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&resource))),"Periphery texture");
        auto handle=rtvs->GetCPUDescriptorHandleForHeapStart();handle.ptr+=stride*index;
        D3D12_RENDER_TARGET_VIEW_DESC vd{};vd.Format=view;vd.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(resource.Get(),&vd,handle);list->ClearRenderTargetView(handle,clear,0,nullptr);
        cheeky::fg::color_transition(list.Get(),resource.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);return resource;
    };
    const float red[]{1,0,0,1},blue[]{0,0,1,1},green[]{0,1,0,1},z[]{.5f,0,0,0},zero[4]{};
    auto previous=texture(DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,0,red);
    auto current=texture(DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,1,blue);
    auto center=texture(DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,2,green);
    auto depth=texture(DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_R32_FLOAT,3,z);
    auto motion=texture(DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R16G16_FLOAT,4,zero);
    // A guide patch catches wrong crop coordinates or motion normalization even
    // when uniform endpoint colors would conceal the error in final pixels.
    const D3D12_RECT patch{8,8,16,16};const float patch_depth[]{.75f,0,0,0},patch_motion[]{2,-4,0,0};
    auto paint=[&](ID3D12Resource* resource,unsigned index,const float* value){
        cheeky::fg::color_transition(list.Get(),resource,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto handle=rtvs->GetCPUDescriptorHandleForHeapStart();handle.ptr+=stride*index;
        list->ClearRenderTargetView(handle,value,1,&patch);
        cheeky::fg::color_transition(list.Get(),resource,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);
    };
    paint(depth.Get(),3,patch_depth);paint(motion.Get(),4,patch_motion);
    execute();begin();
    CheekyFgEye guides{};guides.depth=depth.Get();guides.motion=motion.Get();
    guides.width=guides.height=64;
    guides.depth_width=guides.depth_height=guides.motion_width=guides.motion_height=32;
    cheeky::fg::FocusPipeline pipeline;cheeky::fg::FocusRect full{0,0,64,64,0,false};
    cheeky::fg::FocusRect region{16,16,32,32,0,true};
    check(pipeline.prepare_inputs(device,list.Get(),0,0,current.Get(),guides,region,32,32),"Independent cropped center guides");
    check(pipeline.inputs[0][0].color->GetDesc().Width==32&&pipeline.inputs[0][0].depth->GetDesc().Width==16&&
        pipeline.inputs[0][0].motion->GetDesc().Height==16,"Center color and source-sized cropped guides must have independent grids");
    check(pipeline.prepare_inputs(device,list.Get(),0,1,current.Get(),guides,full,32,32,16,16),"Typeless depth / FP16 motion normalization");
    check(pipeline.inputs[0][1].depth->GetDesc().Format==DXGI_FORMAT_R16_FLOAT&&
        pipeline.inputs[0][1].motion->GetDesc().Format==DXGI_FORMAT_R16G16_FLOAT&&
        pipeline.inputs[0][1].depth->GetDesc().Width==16,"Periphery precision controls and source-sized guide grid");
    check(pipeline.save_depth(device,list.Get(),0),"Periphery depth history");
    auto& normalized=pipeline.inputs[0][0];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT guide_footprints[2]{};UINT64 guide_bytes[2]{};
    ID3D12Resource* normalized_resources[]{normalized.depth.Get(),normalized.motion.Get()};
    UINT64 guide_total{};
    for(unsigned i=0;i<2;++i){const auto desc=normalized_resources[i]->GetDesc();
        const auto offset=(guide_total+511)&~UINT64(511);
        device->GetCopyableFootprints(&desc,0,1,offset,&guide_footprints[i],nullptr,nullptr,&guide_bytes[i]);
        guide_total=offset+guide_bytes[i];}
    D3D12_HEAP_PROPERTIES guide_heap{};guide_heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC guide_buffer{};guide_buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;guide_buffer.Width=guide_total;
    guide_buffer.Height=guide_buffer.DepthOrArraySize=guide_buffer.MipLevels=1;guide_buffer.SampleDesc.Count=1;guide_buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> guide_readback;
    check(SUCCEEDED(device->CreateCommittedResource(&guide_heap,D3D12_HEAP_FLAG_NONE,&guide_buffer,D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,IID_PPV_ARGS(&guide_readback))),"Normalized guide readback");
    for(unsigned i=0;i<2;++i){auto* resource=normalized_resources[i];
        cheeky::fg::color_transition(list.Get(),resource,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=resource;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=guide_readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=guide_footprints[i];
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        cheeky::fg::color_transition(list.Get(),resource,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);}
    execute();pipeline.history[0]=true;
    void* guide_data{};check(SUCCEEDED(guide_readback->Map(0,nullptr,&guide_data)),"Map normalized guides");
    auto guide_pixel=[&](unsigned plane,unsigned x,unsigned y,unsigned components){return reinterpret_cast<const float*>(
        static_cast<const unsigned char*>(guide_data)+guide_footprints[plane].Offset+y*guide_footprints[plane].Footprint.RowPitch+x*components*sizeof(float));};
    check(*guide_pixel(0,0,0,1)==.75f&&*guide_pixel(0,15,15,1)==.5f,"Independent guide grid must sample the correct cropped source depth");
    const auto* vector=guide_pixel(1,0,0,2);const auto* outside=guide_pixel(1,15,15,2);
    check(vector[0]==.125f&&vector[1]==-.25f&&outside[0]==0&&outside[1]==0,
        "Cropped motion must retain normalized UV displacement instead of scaling with destination grid size");
    guide_readback->Unmap(0,nullptr);
    cheeky::fg::Camera camera{};camera.clip_to_prev=cheeky::fg::identity();
    cheeky::fg::Transport publication;check(publication.initialize(device,queue),"Direct composition publication pool");
    for(unsigned method=0;method<3;++method) {
        begin();guides.peripheral_method=method;
        // Supply no depth in simple warp and no guides at all in previous mode.
        guides.depth=method==2?depth.Get():nullptr;guides.motion=method?motion.Get():nullptr;
        check(pipeline.prepare_inputs(device,list.Get(),0,1,current.Get(),guides,full,32,32,16,16),"Prepare only inputs used by the periphery mode");
        check(!pipeline.inputs[0][1].color&&bool(pipeline.inputs[0][1].depth)==(method==2)&&
            bool(pipeline.inputs[0][1].motion)==(method!=0),"Custom periphery must omit unused color/depth/motion resources");
        check(pipeline.generate_periphery(device,list.Get(),0,previous.Get(),current.Get(),guides,camera),"Periphery dispatch");
        check(pipeline.compose_frame(device,list.Get(),0,center.Get(),pipeline.peripheral_output[0].Get(),region,64,64),"Center/periphery composition");
        auto* output=pipeline.full[0].Get();const auto desc=output->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};
        device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC rd{};
        rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=bytes;rd.Height=1;rd.DepthOrArraySize=rd.MipLevels=1;
        rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;ComPtr<ID3D12Resource> readback;
        check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))),"Periphery readback");
        cheeky::fg::color_transition(list.Get(),output,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=output;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        cheeky::fg::color_transition(list.Get(),output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);execute();
        void* data{};check(SUCCEEDED(readback->Map(0,nullptr,&data)),"Periphery map");
        auto pixel=[&](unsigned x,unsigned y){return reinterpret_cast<const unsigned short*>(static_cast<const unsigned char*>(data)+footprint.Offset+y*footprint.Footprint.RowPitch+x*8);};
        const auto* edge=pixel(2,2);const auto* middle=pixel(32,32);
        const bool center_green=middle[0]==0&&middle[1]==0x3c00&&middle[2]==0;
        const bool edge_correct=method<2?(edge[0]==0x3c00&&edge[1]==0&&edge[2]==0):
            (edge[0]==0x3800&&edge[1]==0&&edge[2]==0x3800);
        readback->Unmap(0,nullptr);
        check(center_green,"The center crop must remain green while the periphery changes");
        check(edge_correct,"Previous/simple modes must retain red; bidirectional mode must interpolate red and blue");

        // The output pool itself is the final UAV destination. Keep the first
        // generated image leased while the next job writes different pixels.
        auto pinned=publication.image(desc,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        check(bool(pinned),"Direct publication UAV");begin();
        check(!pipeline.compose_frame(device,list.Get(),0,center.Get(),pipeline.peripheral_output[0].Get(),region,64,64,center.Get()),
            "Reject a composition target that aliases its input before recording barriers");
        check(pipeline.compose_frame(device,list.Get(),0,center.Get(),pipeline.peripheral_output[0].Get(),region,64,64,
            pinned->texture.Get(),pinned->state),"Compose directly into a leased output");execute();
        pinned->state=D3D12_RESOURCE_STATE_GENERIC_READ;
        auto next=publication.image(desc,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto plain=publication.image(desc);
        check(next&&next!=pinned&&plain&&plain!=pinned&&plain!=next&&
            !(plain->texture->GetDesc().Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            "Pool must preserve live output leases and distinguish UAV from copy-only allocations");begin();
        check(pipeline.compose_frame(device,list.Get(),0,previous.Get(),current.Get(),region,64,64,
            next->texture.Get(),next->state),"Compose the next job into a distinct output");execute();
        next->state=D3D12_RESOURCE_STATE_GENERIC_READ;
        auto verify_publication=[&](const cheeky::fg::Image& image,bool original) {
            begin();from.pResource=image->texture.Get();list->CopyTextureRegion(&to,0,0,0,&from,nullptr);execute();
            check(SUCCEEDED(readback->Map(0,nullptr,&data)),"Direct publication readback");
            const auto* e=pixel(2,2);const auto* m=pixel(32,32);
            const bool correct=original?
                (m[0]==0&&m[1]==0x3c00&&m[2]==0&&e[1]==0&&
                    (method<2?(e[0]==0x3c00&&e[2]==0):(e[0]==0x3800&&e[2]==0x3800))):
                (m[0]==0x3c00&&m[1]==0&&m[2]==0&&e[0]==0&&e[1]==0&&e[2]==0x3c00);
            readback->Unmap(0,nullptr);check(correct,"Direct output pixels must survive newer generation jobs");
        };
        verify_publication(pinned,true);verify_publication(next,false);
        auto* retired=pinned.get();pinned.reset();
        auto reused=publication.image(desc,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        check(reused.get()==retired,"Retired direct output allocation may be reused");begin();
        check(pipeline.compose_frame(device,list.Get(),0,center.Get(),pipeline.peripheral_output[0].Get(),region,64,64,
            reused->texture.Get(),reused->state),"Rewrite a retired GENERIC_READ output via UAV");execute();
        reused->state=D3D12_RESOURCE_STATE_GENERIC_READ;verify_publication(reused,true);
    }
    // Compare the actual XR reconstruction with a complete-context reference.
    // Poison the skipped area with HDR NaNs so an accidental sample is visible.
    cheeky::fg::ColorDraw draw;cheeky::fg::ColorDrawBindings bindings;
    cheeky::fg::CompactLayout layout{64,64,13,9,32,32,32,32,32,32,.04f};
    auto make_output=[&](unsigned w,unsigned h,D3D12_RESOURCE_FLAGS flags){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
        d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=d.MipLevels=1;
        d.SampleDesc.Count=1;d.Format=pipeline.color_format;d.Flags=flags;ComPtr<ID3D12Resource> out;
        check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&out))),"Compact periphery output");return out;};
    auto reference=make_output(32,64,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto direct=make_output(32,64,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto reconstructed=make_output(64,64,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};const auto desc=reconstructed->GetDesc();
    device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC rd{};
    rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=bytes;rd.Height=rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))),"Compact reconstruction readback");
    auto reconstruct=[&](ID3D12Resource* atlas,bool packed=true){begin();bindings.used=0;
        cheeky::fg::color_transition(list.Get(),atlas,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cheeky::fg::color_transition(list.Get(),reconstructed.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
        cheeky::fg::ColorDraw::Constants c;if(packed)layout.constants(c.crop,c.spans,c.texel);D3D12_VIEWPORT viewport{0,0,64,64,0,1};
        check(draw.record(device,list.Get(),bindings,atlas,pipeline.color_format,0,reconstructed.Get(),pipeline.color_format,viewport,c),"Production XR compact reconstruction");
        cheeky::fg::color_transition(list.Get(),atlas,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        cheeky::fg::color_transition(list.Get(),reconstructed.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=reconstructed.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=fp;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        cheeky::fg::color_transition(list.Get(),reconstructed.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);execute();
        void* data{};check(SUCCEEDED(readback->Map(0,nullptr,&data)),"Compact reconstruction map");std::vector<unsigned short> pixels(64*64*4);
        for(unsigned y=0;y<64;++y)memcpy(pixels.data()+y*64*4,static_cast<unsigned char*>(data)+fp.Offset+y*fp.Footprint.RowPitch,64*8);
        readback->Unmap(0,nullptr);return pixels;};
    for(unsigned method=0;method<3;++method)for(float feather:{0.f,.04f,.16f}){
        layout.feather=feather;guides.peripheral_method=method;guides.depth=depth.Get();guides.motion=motion.Get();begin();
        check(pipeline.prepare_inputs(device,list.Get(),0,1,current.Get(),guides,full,32,32,16,16),"Compact periphery preparation");
        check(pipeline.generate_periphery(device,list.Get(),0,previous.Get(),current.Get(),guides,camera),"Complete context reference");
        auto* compact_center=pipeline.inputs[0][0].color.Get();
        check(pipeline.pack_frame(device,list.Get(),0,compact_center,pipeline.peripheral_output[0].Get(),reference.Get(),D3D12_RESOURCE_STATE_COMMON,layout,false,false),"Exact region copy packing");execute();
        const auto expected=reconstruct(reference.Get());begin();
        auto handle=rtvs->GetCPUDescriptorHandleForHeapStart();handle.ptr+=stride*2;
        device->CreateRenderTargetView(direct.Get(),nullptr,handle);
        cheeky::fg::color_transition(list.Get(),direct.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float poison[]{NAN,NAN,NAN,NAN};list->ClearRenderTargetView(handle,poison,0,nullptr);
        cheeky::fg::color_transition(list.Get(),direct.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);
        check(!pipeline.pack_frame(device,list.Get(),0,direct.Get(),reference.Get(),direct.Get(),D3D12_RESOURCE_STATE_COMMON,layout,false,false),"Reject atlas/source alias");
        check(pipeline.generate_periphery(device,list.Get(),0,previous.Get(),current.Get(),guides,camera,D3D12_RESOURCE_STATE_COMMON,direct.Get(),D3D12_RESOURCE_STATE_COMMON,&layout),"Direct compact periphery");
        check(pipeline.pack_frame(device,list.Get(),0,compact_center,direct.Get(),direct.Get(),D3D12_RESOURCE_STATE_COMMON,layout,false,true),"Copy center without touching direct context");execute();
        const auto actual=reconstruct(direct.Get());check(actual==expected,"Direct compact periphery must match all reconstructed HDR pixels, including feather/filter boundary and NaN-poisoned interior");
        for(auto v:actual)check((v&0x7c00)!=0x7c00,"No HDR NaNs from covered context");
    }
    std::cout<<"PASS direct compact periphery: all three custom modes, asymmetric crop, three feather widths, exact center/context copies, XR reconstruction matches every reference pixel with NaN-poisoned interior\n";
    auto final_image=make_output(64,64,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    for(unsigned mode:{0u,1u,2u})for(unsigned method=0;method<3;++method){guides.peripheral_method=method;begin();
        check(pipeline.prepare_inputs(device,list.Get(),0,1,current.Get(),guides,full,32,32,16,16),"Final-image periphery preparation");
        auto* small_center=pipeline.inputs[0][0].color.Get();
        cheeky::fg::FocusPipeline::FinalOutput final{small_center,{13,9,32,32,.04f,true},64,64,false,false};
        final.shading_mode=mode;
        check(pipeline.generate_periphery(device,list.Get(),0,previous.Get(),current.Get(),guides,camera,D3D12_RESOURCE_STATE_COMMON,
            final_image.Get(),D3D12_RESOURCE_STATE_COMMON,nullptr,&final),"Final-image assembly from center scratch");execute();
        const auto expected=reconstruct(final_image.Get(),false);begin();
        auto handle=rtvs->GetCPUDescriptorHandleForHeapStart();handle.ptr+=stride*2;device->CreateRenderTargetView(final_image.Get(),nullptr,handle);
        cheeky::fg::color_transition(list.Get(),final_image.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float poison[]{NAN,NAN,NAN,NAN};list->ClearRenderTargetView(handle,poison,0,nullptr);
        cheeky::fg::color_transition(list.Get(),final_image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);
        cheeky::fg::color_transition(list.Get(),small_center,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=small_center;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=final_image.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&to,13,9,0,&from,nullptr);
        cheeky::fg::color_transition(list.Get(),small_center,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        cheeky::fg::color_transition(list.Get(),final_image.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        final.center_direct=true;
        check(pipeline.generate_periphery(device,list.Get(),0,previous.Get(),current.Get(),guides,camera,D3D12_RESOURCE_STATE_COMMON,
            final_image.Get(),D3D12_RESOURCE_STATE_COMMON,nullptr,&final),"Final-image in-place center and periphery assembly");execute();
        check(reconstruct(final_image.Get(),false)==expected,"In-place HDR center/feather pixels must match center-scratch assembly without reading unwritten periphery");
    }
    std::cout<<"PASS final XR image assembly: in-place center matches scratch path for all custom periphery modes, asymmetric placement and feather; unwritten periphery poisoned with HDR NaNs\n";
    // A spatial pattern and CPU expansion reference catch block ownership,
    // interpolation halos and missing pixels that flat-color tests conceal.
    begin();const auto native_desc=previous->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT upload_fp{};UINT64 upload_bytes{};
    device->GetCopyableFootprints(&native_desc,0,1,0,&upload_fp,nullptr,nullptr,&upload_bytes);
    D3D12_HEAP_PROPERTIES upload_heap{};upload_heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    auto upload_desc=rd;upload_desc.Width=upload_bytes;ComPtr<ID3D12Resource> pattern;
    check(SUCCEEDED(device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&upload_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&pattern))),"Pattern upload");
    void* upload_data{};check(SUCCEEDED(pattern->Map(0,nullptr,&upload_data)),"Pattern map");
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){auto* p=static_cast<unsigned char*>(upload_data)+upload_fp.Offset+y*upload_fp.Footprint.RowPitch+x*4;
        p[0]=static_cast<unsigned char>((x*13+y*7)%256);p[1]=static_cast<unsigned char>((x*3+y*19)%256);p[2]=static_cast<unsigned char>((x*23+y*5)%256);p[3]=255;}
    pattern->Unmap(0,nullptr);
    for(auto* image:{previous.Get(),current.Get()}){cheeky::fg::color_transition(list.Get(),image,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=pattern.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=upload_fp;
        to.pResource=image;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        cheeky::fg::color_transition(list.Get(),image,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);}execute();
    auto read_pixels=[&](ID3D12Resource* image){begin();const auto d=image->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT f{};UINT64 size{};
        device->GetCopyableFootprints(&d,0,1,0,&f,nullptr,nullptr,&size);auto buffer=rd;buffer.Width=size;ComPtr<ID3D12Resource> rb;
        check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&rb))),"Pattern readback");
        cheeky::fg::color_transition(list.Get(),image,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=image;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=rb.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=f;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        cheeky::fg::color_transition(list.Get(),image,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);execute();
        void* data{};check(SUCCEEDED(rb->Map(0,nullptr,&data)),"Pattern readback map");std::vector<float> pixels(size_t(d.Width)*d.Height*4);
        for(unsigned y=0;y<d.Height;++y){const auto* row=reinterpret_cast<const unsigned short*>(static_cast<const unsigned char*>(data)+f.Offset+y*f.Footprint.RowPitch);
            for(unsigned x=0;x<d.Width*4;++x){const auto v=row[x];const unsigned exponent=(v>>10)&31,mantissa=v&1023;
                pixels[(size_t(y)*d.Width*4)+x]=exponent==31?NAN:std::ldexp(float(exponent?1024+mantissa:mantissa),exponent?int(exponent)-25:-24)*(v&0x8000?-1.f:1.f);}}
        rb->Unmap(0,nullptr);return pixels;};
    struct GridCase{unsigned w,h,gw,gh;};
    for(const auto shape:{GridCase{64,64,32,32},GridCase{128,128,32,32},GridCase{67,59,32,32},GridCase{73,61,41,35}})
    for(unsigned method=0;method<3;++method){guides.peripheral_method=method;begin();
        check(pipeline.prepare_inputs(device,list.Get(),0,1,current.Get(),guides,full,shape.gw,shape.gh,16,16),"Pattern coarse inputs");
        check(pipeline.generate_periphery(device,list.Get(),0,previous.Get(),current.Get(),guides,camera),"Pattern low-resolution reference");execute();
        const auto coarse=read_pixels(pipeline.peripheral_output[0].Get());
        auto image=make_output(shape.w,shape.h,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        for(unsigned mode:{1u,2u}){begin();auto handle=rtvs->GetCPUDescriptorHandleForHeapStart();handle.ptr+=stride*2;
            device->CreateRenderTargetView(image.Get(),nullptr,handle);cheeky::fg::color_transition(list.Get(),image.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
            const float poison[]{NAN,NAN,NAN,NAN};list->ClearRenderTargetView(handle,poison,0,nullptr);
            cheeky::fg::color_transition(list.Get(),image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);
            cheeky::fg::FocusPipeline::FinalOutput final{center.Get(),{13,9,32,32,.04f,true},shape.w,shape.h,false,false,mode};
            check(pipeline.generate_periphery(device,list.Get(),0,previous.Get(),current.Get(),guides,camera,D3D12_RESOURCE_STATE_COMMON,image.Get(),D3D12_RESOURCE_STATE_COMMON,nullptr,&final),"Pattern direct expansion");execute();
            const auto actual=read_pixels(image.Get());
            auto sample=[&](int x,int y,unsigned c){x=std::clamp(x,0,int(shape.gw)-1);y=std::clamp(y,0,int(shape.gh)-1);return coarse[(size_t(y)*shape.gw+x)*4+c];};
            for(unsigned y=0;y<shape.h;++y)for(unsigned x=0;x<shape.w;++x){const float u=(x+.5f)/shape.w,v=(y+.5f)/shape.h;
                const float sx=u*shape.gw-.5f,sy=v*shape.gh-.5f;const int bx=int(std::floor(sx)),by=int(std::floor(sy));const float fx=sx-bx,fy=sy-by;
                const float edge=std::min({u-13.f/shape.w,v-9.f/shape.h,45.f/shape.w-u,41.f/shape.h-v}),fade=std::clamp(edge/.04f,0.f,1.f);
                for(unsigned c=0;c<4;++c){const float periphery=mode==1?sample(int(u*shape.gw),int(v*shape.gh),c):
                    (1-fy)*((1-fx)*sample(bx,by,c)+fx*sample(bx+1,by,c))+fy*((1-fx)*sample(bx,by+1,c)+fx*sample(bx+1,by+1,c));
                    const float expected=periphery*(1-fade)+(c==1||c==3?1.f:0.f)*fade,value=actual[(size_t(y)*shape.w+x)*4+c];
                    check(std::isfinite(value)&&std::abs(value-expected)<.002f,"Coarse block/bilinear output must match independent low-resolution reference at every pixel, including odd grid/tile edges");}}
        }
    }
    std::cout<<"PASS coarse shading: patterned source, nonzero motion, all custom modes, 2x/4x/odd ratios, block and bilinear CPU references, feather and complete NaN-poisoned output coverage\n";
    check(SUCCEEDED(device->GetDeviceRemovedReason()),"Periphery pipeline device removal");
    std::cout << "PASS actual periphery pixels: previous/simple red, bidirectional purple, independent green center; source-sized guide crop and nonzero UV motion; unused inputs omitted\n";
    std::cout << "PASS direct composition: original pixels retained through a newer output, distinct UAV/copy pools, retired GENERIC_READ reuse and alias rejection\n";
}
