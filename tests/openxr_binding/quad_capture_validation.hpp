#pragma once
namespace {
struct QuadFakeChain {Microsoft::WRL::ComPtr<ID3D12Resource> texture;bool acquired{},waited{};};
std::unordered_map<XrSwapchain,QuadFakeChain> quad_fake_chains;
ID3D12Device* quad_test_device{};
unsigned quad_next_chain=5000,quad_runtime_begins{},quad_runtime_ends{};
XrViewConfigurationType quad_runtime_configuration{};
XrResult XRAPI_CALL quad_test_configurations(XrInstance,XrSystemId,uint32_t capacity,uint32_t* count,XrViewConfigurationType* types) {
    return cheeky::quad_capture::enumerate(std::vector<XrViewConfigurationType>{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO},capacity,count,types);
}
XrResult XRAPI_CALL quad_test_recommended(XrInstance,XrSystemId,XrViewConfigurationType type,uint32_t capacity,uint32_t* count,XrViewConfigurationView* views) {
    check(type==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,"Quad enum leaked to stereo runtime");
    *count=2;if(!capacity)return XR_SUCCESS;if(capacity<2)return XR_ERROR_SIZE_INSUFFICIENT;
    for(unsigned i=0;i<2;++i){views[i].recommendedImageRectWidth=views[i].recommendedImageRectHeight=32;
        views[i].maxImageRectWidth=views[i].maxImageRectHeight=64;views[i].recommendedSwapchainSampleCount=views[i].maxSwapchainSampleCount=1;}
    return XR_SUCCESS;
}
XrResult XRAPI_CALL quad_test_locate(XrSession,const XrViewLocateInfo* info,XrViewState* state,uint32_t capacity,uint32_t* count,XrView* views) {
    check(info->viewConfigurationType==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO&&capacity==2,"Quad locate must use stereo runtime");
    *count=2;state->viewStateFlags=XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT;
    for(unsigned i=0;i<2;++i){views[i].pose.orientation.w=1;views[i].pose.position.x=i?.032f:-.032f;
        views[i].fov={i?-.6f:-.9f,i?.9f:.6f,.8f,-.7f};}return XR_SUCCESS;
}
XrResult XRAPI_CALL quad_test_begin(XrSession,const XrSessionBeginInfo* info) {
    ++quad_runtime_begins;quad_runtime_configuration=info->primaryViewConfigurationType;return XR_SUCCESS;
}
XrResult XRAPI_CALL quad_test_create(XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* chain) {
    using Microsoft::WRL::ComPtr;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=info->width;desc.Height=info->height;
    desc.DepthOrArraySize=desc.MipLevels=1;desc.Format=static_cast<DXGI_FORMAT>(info->format);desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> texture;check(SUCCEEDED(quad_test_device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&texture))),"Quad runtime output allocation");
    *chain=h<XrSwapchain>(quad_next_chain++);quad_fake_chains[*chain].texture=texture;return XR_SUCCESS;
}
XrResult XRAPI_CALL quad_test_images(XrSwapchain chain,uint32_t capacity,uint32_t* count,XrSwapchainImageBaseHeader* images) {
    *count=1;if(!capacity)return XR_SUCCESS;
    reinterpret_cast<XrSwapchainImageD3D12KHR*>(images)[0].texture=quad_fake_chains.at(chain).texture.Get();return XR_SUCCESS;
}
XrResult XRAPI_CALL quad_test_acquire(XrSwapchain chain,const XrSwapchainImageAcquireInfo*,uint32_t* index) {
    auto& c=quad_fake_chains.at(chain);check(!c.acquired,"Quad output double acquire");c.acquired=true;c.waited=false;*index=0;return XR_SUCCESS;
}
XrResult XRAPI_CALL quad_test_wait(XrSwapchain chain,const XrSwapchainImageWaitInfo*) {
    auto& c=quad_fake_chains.at(chain);check(c.acquired,"Quad wait before acquire");c.waited=true;return XR_SUCCESS;
}
XrResult XRAPI_CALL quad_test_release(XrSwapchain chain,const XrSwapchainImageReleaseInfo*) {
    auto& c=quad_fake_chains.at(chain);check(c.acquired&&c.waited,"Quad release before wait");c.acquired=c.waited=false;return XR_SUCCESS;
}
XrResult XRAPI_CALL quad_test_destroy(XrSwapchain chain) {quad_fake_chains.erase(chain);return XR_SUCCESS;}
std::array<XrSwapchain,2> quad_submitted_chains{};
XrResult XRAPI_CALL quad_test_end(XrSession,const XrFrameEndInfo* info) {
    ++quad_runtime_ends;check(info->layerCount==2,"Quad auxiliary layer lost");
    check(info->layers[0]->type==XR_TYPE_COMPOSITION_LAYER_QUAD,"Quad auxiliary layer replaced");
    const auto* projection=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[1]);
    check(projection->viewCount==2,"Quad projection not composed to stereo");
    for(unsigned eye=0;eye<2;++eye){const auto& view=projection->views[eye];quad_submitted_chains[eye]=view.subImage.swapchain;
        check(view.next==nullptr&&view.subImage.imageArrayIndex==0,"Quad output depth/slice metadata invalid");
        check(!quad_fake_chains.at(view.subImage.swapchain).acquired,"Quad output submitted before release");}
    return XR_SUCCESS;
}
void quad_capture_validation() {
    using Microsoft::WRL::ComPtr;
    reset();auto& d=instances.at(instance).dispatch;
    d.enumerate_view_configurations=quad_test_configurations;d.enumerate_view_configuration_views=quad_test_recommended;
    uint32_t count{};
    check(cheeky_xrEnumerateViewConfigurations(instance,1,0,&count,nullptr)==XR_SUCCESS&&count==1,"Quad capability appears without enable");
    instances.at(instance).quad_capture_enabled=true;
    check(cheeky_xrEnumerateViewConfigurations(instance,1,0,&count,nullptr)==XR_SUCCESS&&count==2,"Quad capability not exposed");
    XrViewConfigurationType short_buffer{};
    check(cheeky_xrEnumerateViewConfigurations(instance,1,1,&count,&short_buffer)==XR_ERROR_SIZE_INSUFFICIENT,"Quad enum short-buffer contract");
    std::array<XrViewConfigurationType,2> types{};
    check(cheeky_xrEnumerateViewConfigurations(instance,1,2,&count,types.data())==XR_SUCCESS&&types[1]==cheeky::quad_capture::configuration,"Quad enum missing configuration");
    instances.at(instance).quad_capture_prefer_quad=true;
    check(cheeky_xrEnumerateViewConfigurations(instance,1,2,&count,types.data())==XR_SUCCESS&&
        types[0]==cheeky::quad_capture::configuration&&types[1]==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,"Quad preference must retain stereo fallback");
    instances.at(instance).quad_capture_prefer_quad=false;
    d.enumerate_view_configurations=[](XrInstance,XrSystemId,uint32_t,uint32_t*,XrViewConfigurationType*) -> XrResult {return XR_ERROR_RUNTIME_FAILURE;};
    count=777;types[0]=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO;
    check(cheeky_xrEnumerateViewConfigurations(instance,1,2,&count,types.data())==XR_ERROR_RUNTIME_FAILURE&&count==777&&
        types[0]==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO,"Negotiation diagnostics changed a failed enumeration's result/output");
    d.enumerate_view_configurations=quad_test_configurations;
    check(cheeky_xrEnumerateViewConfigurationViews(instance,1,cheeky::quad_capture::configuration,0,&count,nullptr)==XR_SUCCESS&&count==4,"Quad view size query");
    std::array<XrViewConfigurationView,4> recommended;for(auto& v:recommended)v={XR_TYPE_VIEW_CONFIGURATION_VIEW};
    check(cheeky_xrEnumerateViewConfigurationViews(instance,1,cheeky::quad_capture::configuration,3,&count,recommended.data())==XR_ERROR_SIZE_INSUFFICIENT,"Quad view short-buffer query");
    check(cheeky_xrEnumerateViewConfigurationViews(instance,1,cheeky::quad_capture::configuration,4,&count,recommended.data())==XR_SUCCESS,"Quad recommendations");
    for(const auto& v:recommended)check(v.recommendedImageRectWidth==16&&v.recommendedImageRectHeight==16,"Quad recommendation dimensions");
    ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;ComPtr<ID3D12Device> device;
    check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))&&SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))&&
        SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"Quad WARP device");
    quad_test_device=device.Get();ComPtr<ID3D12InfoQueue> errors;device.As(&errors);if(errors)errors->ClearStoredMessages();
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};
    check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"Quad WARP queue");
    d.begin_session=quad_test_begin;d.locate_views=quad_test_locate;d.create_swapchain=quad_test_create;d.destroy_swapchain=quad_test_destroy;
    d.enumerate_swapchain_images=quad_test_images;d.acquire_swapchain_image=quad_test_acquire;d.wait_swapchain_image=quad_test_wait;d.release_swapchain_image=quad_test_release;d.end_frame=quad_test_end;
    SessionState session;session.session=h<XrSession>(2);session.instance=instance;session.system_id=1;session.graphics_api=12;
    session.graphics_device=device.Get();session.graphics_queue=queue.Get();sessions.emplace(session.session,std::move(session));
    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};begin.primaryViewConfigurationType=cheeky::quad_capture::configuration;
    check(cheeky_xrBeginSession(h<XrSession>(2),&begin)==XR_SUCCESS&&quad_runtime_configuration==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,"Quad begin-session translation");
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};locate.viewConfigurationType=cheeky::quad_capture::configuration;locate.displayTime=123;
    XrViewState state{XR_TYPE_VIEW_STATE};std::array<XrView,4> views;for(auto& view:views)view={XR_TYPE_VIEW};
    check(cheeky_xrLocateViews(h<XrSession>(2),&locate,&state,0,&count,nullptr)==XR_SUCCESS&&count==4,"Quad locate count query");
    check(cheeky_xrLocateViews(h<XrSession>(2),&locate,&state,3,&count,views.data())==XR_ERROR_SIZE_INSUFFICIENT,"Quad locate short buffer");
    check(cheeky_xrLocateViews(h<XrSession>(2),&locate,&state,4,&count,views.data())==XR_SUCCESS&&count==4,"Quad located views");
    for(unsigned eye=0;eye<2;++eye){check(views[eye].pose.position.x==views[eye+2].pose.position.x,"Quad eye pose changed");
        const auto rect=cheeky::quad_capture::inset_rect(views[eye].fov,views[eye+2].fov);
        check(rect[0]>=0&&rect[1]>=0&&std::abs(rect[2]-.5f)<.0001f&&std::abs(rect[3]-.5f)<.0001f,"Quad inset FOV geometry");}
    auto compositor=sessions.at(h<XrSession>(2)).quad_capture;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=32;desc.DepthOrArraySize=4;desc.MipLevels=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> source;check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&source))),"Quad source array");
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))&&
        SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))),"Quad source recording");
    ComPtr<ID3D12DescriptorHeap> rtvs;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=4;
    check(SUCCEEDED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs))),"Quad source RTVs");
    const float colors[4][4]{{1,0,0,1},{0,1,0,1},{0,0,1,1},{1,1,0,1}};
    for(unsigned i=0;i<4;++i){auto handle=rtvs->GetCPUDescriptorHandleForHeapStart();handle.ptr+=i*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};rtv.Format=desc.Format;rtv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;rtv.Texture2DArray.FirstArraySlice=i;rtv.Texture2DArray.ArraySize=1;
        device->CreateRenderTargetView(source.Get(),&rtv,handle);list->ClearRenderTargetView(handle,colors[i],0,nullptr);}
    check(SUCCEEDED(list->Close()),"Quad source close");ID3D12CommandList* commands[]{list.Get()};queue->ExecuteCommandLists(1,commands);
    check(SUCCEEDED(queue->Signal(compositor->fence.Get(),++compositor->serial))&&compositor->idle(),"Quad source ready");
    SwapchainState chain;chain.swapchain=h<XrSwapchain>(4000);chain.session=h<XrSession>(2);chain.has_released=true;
    chain.create_info.format=desc.Format;chain.calibration_images={source.Get()};swapchains[chain.swapchain]=chain;
    std::array<XrCompositionLayerProjectionView,4> submitted;
    for(unsigned i=0;i<4;++i){submitted[i]={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};submitted[i].pose=views[i].pose;submitted[i].fov=views[i].fov;
        submitted[i].subImage={chain.swapchain,{{0,0},{32,32}},i};}
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.viewCount=4;projection.views=submitted.data();
    XrCompositionLayerQuad overlay{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&overlay),reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=123;end.layerCount=2;end.layers=layers;
    check(cheeky_xrEndFrame(h<XrSession>(2),&end)==XR_SUCCESS&&quad_runtime_ends==1,"Quad end-frame composition");
    check(projection.viewCount==4&&submitted[2].subImage.imageArrayIndex==2,"Quad caller submission modified");
    for(unsigned eye=0;eye<2;++eye){
        const auto texture=quad_fake_chains.at(quad_submitted_chains[eye]).texture;const auto output_desc=texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};device->GetCopyableFootprints(&output_desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_HEAP_PROPERTIES read_heap{};read_heap.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;check(SUCCEEDED(device->CreateCommittedResource(&read_heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))),"Quad readback");
        check(SUCCEEDED(allocator->Reset())&&SUCCEEDED(list->Reset(allocator.Get(),nullptr)),"Quad readback reset");
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=texture.Get();b.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1,&b);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=texture.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);check(SUCCEEDED(list->Close()),"Quad readback close");queue->ExecuteCommandLists(1,commands);
        check(SUCCEEDED(queue->Signal(compositor->fence.Get(),++compositor->serial))&&compositor->idle(),"Quad readback completion");
        void* mapped{};check(SUCCEEDED(readback->Map(0,nullptr,&mapped)),"Quad readback map");const auto* pixels=static_cast<const unsigned char*>(mapped);
        const auto rect=cheeky::quad_capture::inset_rect(views[eye].fov,views[eye+2].fov);
        const auto x=unsigned((rect[0]+rect[2]*.5f)*32),y=unsigned((rect[1]+rect[3]*.5f)*32);
        const auto* focus=pixels+y*footprint.Footprint.RowPitch+x*4;
        check(pixels[eye?1:0]==255&&pixels[eye?0:1]==0,"Quad peripheral pixels incorrect");
        check(focus[eye?0:2]==255&&focus[eye?2:0]==0,"Quad focus pixels incorrect");readback->Unmap(0,nullptr);
    }
    if(errors)for(UINT64 i=0;i<errors->GetNumStoredMessages();++i){SIZE_T length{};errors->GetMessage(i,nullptr,&length);std::vector<char> storage(length);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());errors->GetMessage(i,message,&length);
        if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<message->pDescription<<'\n';check(false,"Quad D3D12 debug-layer error");}}
    sessions.clear();compositor.reset();swapchains.clear();quad_fake_chains.clear();quad_test_device=nullptr;
    std::cout<<"PASS quad capability, buffer contracts, stereo session translation, fixed four-view poses and GPU array-slice composition (WARP)\n";
}
}
