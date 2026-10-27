#pragma once

void fg_compact_validation(ID3D12Device* device) {
    using namespace cheeky::fg;using Microsoft::WRL::ComPtr;
    buffered_probe::Runtime::device=device;
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};
    check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"Compact queue");
    BufferedPresenter presenter;check(presenter.setup(h<XrSession>(9),buffered_probe::Runtime::functions(),device,queue.Get()),"Compact presenter");
    Transport transport;check(transport.initialize(device,queue.Get()),"Compact transport");
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=128;
    desc.DepthOrArraySize=2;desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> source;
    check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&source))),"Compact array source");
    ColorDrawBindings bindings;
    auto* capture=transport.begin();check(capture!=nullptr,"Compact capture context");
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;ComPtr<ID3D12DescriptorHeap> rtv;
    check(SUCCEEDED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv))),"Compact clear heap");
    D3D12_RENDER_TARGET_VIEW_DESC view{};view.Format=desc.Format;view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray.FirstArraySlice=1;view.Texture2DArray.ArraySize=1;device->CreateRenderTargetView(source.Get(),&view,rtv->GetCPUDescriptorHandleForHeapStart());
    const float blue[]{0,0,.25f,1},red[]{.5f,0,0,1};D3D12_RECT center{32,32,96,96};
    capture->list->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(),blue,0,nullptr);
    capture->list->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(),red,1,&center);
    const auto layout=compact_layout(128,128,true,.5f,.5f,0,0,25,0);
    check(layout.packed()&&layout.stored_width()==64&&layout.stored_height()==96,"Compact physical shape includes atlas padding");
    auto native=transport.packed_region(*capture,source.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,{{0,0},{128,128}},1,desc.Format,layout);
    check(bool(native),"Pack native array slice");transport.freeze(*capture,native);std::uint64_t ticket{};
    check(transport.execute(*capture,ticket),"Submit compact native capture");transport.drain();
    BufferedPresenter::Chain chain;
    check(presenter.upload_image(chain,native,true),"Reconstruct native compact image into scene XR");presenter.uploads.drain();
    const auto handle=chain.handle;check(chain.desc.Width==128&&chain.desc.Height==128&&chain.desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        "Scene XR retains logical dimensions and converts BGRA to stable RGBA sRGB");
    auto verify=[&](unsigned x,unsigned y,bool center_pixel){
        const auto target=chain.images[0].texture;const auto td=target->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};
        device->GetCopyableFootprints(&td,0,1,0,&fp,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=bytes;rd.Height=1;
        rd.DepthOrArraySize=rd.MipLevels=rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES rh{};rh.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> readback;
        check(SUCCEEDED(device->CreateCommittedResource(&rh,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))),"Compact pixel readback");
        auto* c=transport.begin();color_transition(c->list.Get(),target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=target;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        b.pResource=readback.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=fp;c->list->CopyTextureRegion(&b,0,0,0,&a,nullptr);
        color_transition(c->list.Get(),target,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        check(transport.execute(*c,ticket),"Compact readback submit");transport.drain();
        void* data{};check(SUCCEEDED(readback->Map(0,nullptr,&data)),"Compact readback map");
        const auto* pixel=static_cast<unsigned char*>(data)+y*fp.Footprint.RowPitch+x*4;
        const bool correct=center_pixel?(std::abs(int(pixel[0])-188)<=2&&pixel[1]==0&&pixel[2]==0):
            (pixel[0]==0&&pixel[1]==0&&std::abs(int(pixel[2])-137)<=2);
        readback->Unmap(0,nullptr);check(correct,"Compact reconstruction preserves center/context colors, array slice and sRGB exactly once");
    };
    verify(64,64,true);verify(0,0,false);verify(127,127,false);
    // Generated atlas uses a different resource format but must keep the same
    // scene XR destination; its compact packing runs without a full-size image.
    FocusPipeline focus;focus.color_format=DXGI_FORMAT_R8G8B8A8_UNORM;check(focus.initialize(device),"Compact compute shaders");
    auto gd=native->texture->GetDesc();gd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    auto generated=transport.image(gd,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);generated->layout=layout;
    // Use the atlas's two regions as independent read-only views through the
    // production preparation path, retaining native center density and context.
    CheekyFgEye g;g.width=g.height=128;g.peripheral_method=cheeky_fg_previous;
    // Test publication packing from known, uniformly colored region textures.
    auto make_color=[&](unsigned size,const float* color){
        auto d=gd;d.Width=d.Height=size;auto image=transport.image(d,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto* c=transport.begin();D3D12_RENDER_TARGET_VIEW_DESC v{};v.Format=d.Format;v.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(image->texture.Get(),&v,rtv->GetCPUDescriptorHandleForHeapStart());
        color_transition(c->list.Get(),image->texture.Get(),image->state,D3D12_RESOURCE_STATE_RENDER_TARGET);
        c->list->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(),color,0,nullptr);
        color_transition(c->list.Get(),image->texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);
        c->images.push_back(image);check(transport.execute(*c,ticket),"Compact region clear");transport.drain();return image;
    };
    const float encoded_red[]{188.f/255,0,0,1},encoded_blue[]{0,0,137.f/255,1};
    auto central=make_color(64,encoded_red),context=make_color(32,encoded_blue);
    auto* c=transport.begin();
    check(focus.pack_frame(device,c->list.Get(),0,central->texture.Get(),context->texture.Get(),generated->texture.Get(),generated->state,layout,false,false),"Pack generated center and context");
    transport.freeze(*c,generated);c->images.push_back(central);c->images.push_back(context);
    check(transport.execute(*c,ticket),"Submit generated compact publication");transport.drain();
    generated->presentation_format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    check(presenter.upload_image(chain,generated,true)&&chain.handle==handle,"Native and generated compact frames share XR destinations");presenter.uploads.drain();
    verify(64,64,true);verify(0,0,false);verify(127,127,false);
    const auto before=presenter.uploads.serial;check(presenter.upload_image(chain,generated,true)&&presenter.uploads.serial==before,"Unchanged compact image skips upload");
    check(native.use_count()==2,"Completed XR upload does not pin a native capture beyond its pool/client owners");
    presenter.xr.destroy(chain.handle);chain={};presenter.reset();
    std::cout<<"PASS compact native/generated GPU pixels, array-slice capture, BGRA/RGBA sRGB, atlas boundaries, shared XR and early release\n";
}
