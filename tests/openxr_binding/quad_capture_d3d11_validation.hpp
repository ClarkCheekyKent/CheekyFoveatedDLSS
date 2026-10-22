#pragma once
#include <d3d11sdklayers.h>
namespace {
struct Quad11Chain {Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;bool acquired{},waited{};};
std::unordered_map<XrSwapchain,Quad11Chain> quad11_chains;
ID3D11Device* quad11_device{};
unsigned quad11_ends{},quad11_acquires{};
XrResult quad11_wait_result=XR_SUCCESS;
std::array<XrSwapchain,2> quad11_submitted{};
XrResult XRAPI_CALL quad11_create(XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* chain) {
    D3D11_TEXTURE2D_DESC desc{};desc.Width=info->width;desc.Height=info->height;desc.ArraySize=info->arraySize;desc.MipLevels=1;
    // OpenXR may expose a typed SRGB view backed by a typeless image.
    desc.Format=info->format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB?DXGI_FORMAT_R8G8B8A8_TYPELESS:static_cast<DXGI_FORMAT>(info->format);
    desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    if(info->usageFlags&XR_SWAPCHAIN_USAGE_SAMPLED_BIT)desc.BindFlags|=D3D11_BIND_SHADER_RESOURCE;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    check(SUCCEEDED(quad11_device->CreateTexture2D(&desc,nullptr,&texture)),"Quad11 swapchain texture");
    *chain=h<XrSwapchain>(quad_next_chain++);quad11_chains[*chain].texture=texture;return XR_SUCCESS;
}
XrResult XRAPI_CALL quad11_images(XrSwapchain chain,uint32_t capacity,uint32_t* count,XrSwapchainImageBaseHeader* images) {
    *count=1;if(!capacity)return XR_SUCCESS;
    check(images->type==XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR,"D3D11 session enumerated D3D12 images");
    reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)[0].texture=quad11_chains.at(chain).texture.Get();return XR_SUCCESS;
}
XrResult XRAPI_CALL quad11_acquire(XrSwapchain chain,const XrSwapchainImageAcquireInfo*,uint32_t* index) {
    auto& value=quad11_chains.at(chain);check(!value.acquired,"Quad11 double acquire");
    ++quad11_acquires;value.acquired=true;value.waited=false;*index=0;return XR_SUCCESS;
}
XrResult XRAPI_CALL quad11_wait(XrSwapchain chain,const XrSwapchainImageWaitInfo*) {
    auto& value=quad11_chains.at(chain);check(value.acquired&&!value.waited,"Quad11 wait order");
    if(quad11_wait_result==XR_SUCCESS)value.waited=true;return quad11_wait_result;
}
XrResult XRAPI_CALL quad11_release(XrSwapchain chain,const XrSwapchainImageReleaseInfo*) {
    auto& value=quad11_chains.at(chain);check(value.acquired&&value.waited,"Quad11 release order");
    value.acquired=value.waited=false;return XR_SUCCESS;
}
XrResult XRAPI_CALL quad11_destroy(XrSwapchain chain) {quad11_chains.erase(chain);return XR_SUCCESS;}
XrResult XRAPI_CALL quad11_end(XrSession,const XrFrameEndInfo* info) {
    ++quad11_ends;check(info->layerCount==2&&info->layers[0]->type==XR_TYPE_COMPOSITION_LAYER_QUAD,"Quad11 overlay lost");
    const auto* projection=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[1]);
    check(projection->viewCount==2,"Quad11 leaked four views to native runtime");
    for(unsigned eye=0;eye<2;++eye){const auto& v=projection->views[eye];quad11_submitted[eye]=v.subImage.swapchain;
        check(v.next==nullptr&&v.subImage.imageArrayIndex==0&&!quad11_chains.at(v.subImage.swapchain).acquired,"Quad11 output metadata/release");}
    return XR_SUCCESS;
}
void quad_capture_d3d11_validation() {
    using Microsoft::WRL::ComPtr;
    for(const auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}) {
        reset();quad11_ends=quad11_acquires=0;quad11_wait_result=XR_SUCCESS;
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
        auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context);
        if(hr==DXGI_ERROR_SDK_COMPONENT_MISSING)hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context);
        check(SUCCEEDED(hr),"Quad11 WARP device");quad11_device=device.Get();
        ComPtr<ID3D11InfoQueue> errors;device.As(&errors);if(errors)errors->ClearStoredMessages();
        auto& owner=instances.at(instance);owner.quad_capture_enabled=owner.quad_capture_prefer_quad=true;
        auto& d=owner.dispatch;d.enumerate_view_configurations=quad_test_configurations;d.enumerate_view_configuration_views=quad_test_recommended;
        d.begin_session=quad_test_begin;d.locate_views=quad_test_locate;d.create_swapchain=quad11_create;d.destroy_swapchain=quad11_destroy;
        d.enumerate_swapchain_images=quad11_images;d.acquire_swapchain_image=quad11_acquire;d.wait_swapchain_image=quad11_wait;
        d.release_swapchain_image=quad11_release;d.end_frame=quad11_end;
        SessionState session;session.session=h<XrSession>(2);session.instance=instance;session.system_id=1;session.graphics_api=11;session.graphics_device=device.Get();
        sessions.emplace(session.session,std::move(session));const auto handle=h<XrSession>(2);
        uint32_t count{};std::array<XrViewConfigurationType,2> configurations{};
        check(cheeky_xrEnumerateViewConfigurations(instance,1,2,&count,configurations.data())==XR_SUCCESS&&
            configurations[0]==cheeky::quad_capture::configuration,"DCS-style choose-first did not select quad");
        // The application creates its swapchain before beginning the session.
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        ci.format=format;ci.sampleCount=ci.faceCount=ci.mipCount=1;ci.width=ci.height=48;ci.arraySize=4;XrSwapchain source_chain{};
        check(cheeky_xrCreateSwapchain(handle,&ci,&source_chain)==XR_SUCCESS&&!(ci.usageFlags&XR_SWAPCHAIN_USAGE_SAMPLED_BIT),"Quad11 create mutates caller");
        D3D11_TEXTURE2D_DESC source_desc{};quad11_chains.at(source_chain).texture->GetDesc(&source_desc);
        check((source_desc.BindFlags&D3D11_BIND_SHADER_RESOURCE)!=0,"Pre-begin quad source is not sampleable");
        XrSwapchainImageD3D11KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        check(cheeky_xrEnumerateSwapchainImages(source_chain,1,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(&image))==XR_SUCCESS,"Quad11 application image enumeration");
        const std::array<unsigned char,4> colors[]{{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,0,255}};
        for(unsigned slice=0;slice<4;++slice){std::vector<unsigned char> pixels(48*48*4);
            for(unsigned y=0;y<48;++y)for(unsigned x=0;x<48;++x){
                const std::array<unsigned char,4> padding{255,0,255,255};const auto& color=x>=8&&x<40&&y>=4&&y<36?colors[slice]:padding;
                std::copy(color.begin(),color.end(),pixels.begin()+(y*48+x)*4);}
            context->UpdateSubresource(image.texture,slice,nullptr,pixels.data(),48*4,0);}
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};begin.primaryViewConfigurationType=configurations[0];
        check(cheeky_xrBeginSession(handle,&begin)==XR_SUCCESS&&quad_runtime_configuration==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,"D3D11 quad was rejected");
        auto compositor=sessions.at(handle).quad_capture;check(compositor&&compositor->d3d11,"No native D3D11 quad compositor");
        sessions.at(handle).quad_capture_correct_focus_fov=true;
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};locate.viewConfigurationType=begin.primaryViewConfigurationType;locate.displayTime=123;
        XrViewState state{XR_TYPE_VIEW_STATE};std::array<XrView,4> views;for(auto& v:views)v={XR_TYPE_VIEW};
        check(cheeky_xrLocateViews(handle,&locate,&state,4,&count,views.data())==XR_SUCCESS&&count==4,"Quad11 locate");
        uint32_t index{};XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        check(cheeky_xrAcquireSwapchainImage(source_chain,&acquire,&index)==XR_SUCCESS&&cheeky_xrWaitSwapchainImage(source_chain,&wait)==XR_SUCCESS&&
            cheeky_xrReleaseSwapchainImage(source_chain,&release)==XR_SUCCESS,"Quad11 source lifecycle");
        std::array<XrCompositionLayerProjectionView,4> submitted;
        for(unsigned i=0;i<4;++i){submitted[i]={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};submitted[i].pose=views[i].pose;
            submitted[i].fov=views[i%2].fov; // Reproduce DCS's incorrect focus FOV.
            submitted[i].subImage={source_chain,{{8,4},{32,32}},i};}
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.viewCount=4;projection.views=submitted.data();
        XrCompositionLayerQuad overlay{XR_TYPE_COMPOSITION_LAYER_QUAD};
        const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&overlay),reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=123;end.layerCount=2;end.layers=layers;
        D3D11_VIEWPORT viewport{3,4,19,21,.2f,.8f};context->RSSetViewports(1,&viewport);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        context->VSSetShader(compositor->d3d11->vs.Get(),nullptr,0);
        check(cheeky_xrEndFrame(handle,&end)==XR_SUCCESS&&quad11_ends==1,"Quad11 full application submission");
        D3D11_PRIMITIVE_TOPOLOGY topology{};context->IAGetPrimitiveTopology(&topology);D3D11_VIEWPORT restored{};UINT n=1;context->RSGetViewports(&n,&restored);
        ComPtr<ID3D11VertexShader> vertex;context->VSGetShader(&vertex,nullptr,nullptr);
        check(topology==D3D11_PRIMITIVE_TOPOLOGY_LINELIST&&restored.TopLeftX==3&&restored.Width==19&&vertex.Get()==compositor->d3d11->vs.Get(),"Quad11 changed game context state");
        check(projection.viewCount==4&&submitted[2].fov.angleLeft==views[0].fov.angleLeft&&submitted[2].subImage.imageArrayIndex==2,"Quad11 changed caller projection");
        for(unsigned eye=0;eye<2;++eye){auto texture=quad11_chains.at(quad11_submitted[eye]).texture;D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
            desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging)),"Quad11 readback texture");
            context->CopyResource(staging.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
            check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"Quad11 readback map");
            const auto* pixels=static_cast<const unsigned char*>(mapped.pData);const auto rect=cheeky::quad_capture::inset_rect(views[eye].fov,views[eye+2].fov);
            const auto x=unsigned((rect[0]+rect[2]*.5f)*32),y=unsigned((rect[1]+rect[3]*.5f)*32);const auto* center=pixels+y*mapped.RowPitch+x*4;
            check(pixels[eye?1:0]==255&&pixels[eye?0:1]==0&&pixels[2]==0,"Quad11 periphery/slice/source rectangle wrong");
            check(center[eye?0:2]==255&&center[eye?2:0]==0,"Quad11 focus/slice/FOV correction wrong");context->Unmap(staging.Get(),0);}
        end.displayTime=777;
        check(cheeky_xrEndFrame(handle,&end)==XR_SUCCESS,"Quad11 fixed inset cannot recover an unmatched display time");end.displayTime=123;
        const auto previous_ends=quad11_ends;
        quad11_wait_result=XR_TIMEOUT_EXPIRED;const auto before=quad11_acquires;
        sessions.at(handle).native_frame_begun=true;
        check(cheeky_xrEndFrame(handle,&end)==XR_ERROR_RUNTIME_FAILURE&&quad11_ends==previous_ends&&sessions.at(handle).native_frame_begun,
            "Quad11 wait timeout falsely completed an OpenXR frame");
        quad11_wait_result=XR_SUCCESS;
        check(cheeky_xrEndFrame(handle,&end)==XR_SUCCESS&&quad11_acquires==before+2&&!sessions.at(handle).native_frame_begun,
            "Quad11 wait retry double-acquired pending image or lost frame state");
        context->ClearState();context->Flush();
        if(errors)for(UINT64 i=0;i<errors->GetNumStoredMessages();++i){SIZE_T size{};errors->GetMessage(i,nullptr,&size);std::vector<char> storage(size);
            auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());errors->GetMessage(i,message,&size);
            if(message->Severity<=D3D11_MESSAGE_SEVERITY_ERROR){std::cerr<<message->pDescription<<'\n';check(false,"Quad11 D3D11 debug-layer error");}}
        sessions.clear();compositor.reset();swapchains.clear();quad11_chains.clear();quad11_device=nullptr;
    }
    std::cout<<"PASS D3D11 quad selection/session/submission, pre-begin sampleable images, array/crop/SRGB pixels, DCS focus FOV, context state and wait recovery (WARP)\n";
}
}
