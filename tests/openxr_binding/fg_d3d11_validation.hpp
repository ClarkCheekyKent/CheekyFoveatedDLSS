#pragma once
namespace fg11_probe {
using Microsoft::WRL::ComPtr;
inline ID3D11Device* device{};
inline std::unordered_map<XrSwapchain,ComPtr<ID3D11Texture2D>> chains;
inline unsigned sequence{}, releases{};
XrResult XRAPI_CALL create(XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* out) {
    D3D11_TEXTURE2D_DESC desc{};desc.Width=info->width;desc.Height=info->height;
    desc.ArraySize=info->arraySize;desc.MipLevels=1;desc.SampleDesc.Count=1;
    desc.Format=static_cast<DXGI_FORMAT>(info->format);desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    if(FAILED(device->CreateTexture2D(&desc,nullptr,&texture)))return XR_ERROR_RUNTIME_FAILURE;
    *out=h<XrSwapchain>(100000+(++sequence));chains[*out]=std::move(texture);return XR_SUCCESS;
}
XrResult XRAPI_CALL enumerate(XrSwapchain chain,unsigned capacity,unsigned* count,XrSwapchainImageBaseHeader* images) {
    *count=1;if(!capacity)return XR_SUCCESS;
    if(!images || images[0].type!=XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR)return XR_ERROR_VALIDATION_FAILURE;
    reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)[0].texture=chains.at(chain).Get();return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy(XrSwapchain chain) {chains.erase(chain);return XR_SUCCESS;}
}

void fg_d3d11_validation() {
    using namespace fg11_probe;
    ComPtr<ID3D11Device> d11;ComPtr<ID3D11DeviceContext> context;
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,&context)),"FG D3D11 WARP device");
    device=d11.Get();releases=0;
    Dispatch dispatch;
    dispatch.create_swapchain=create;dispatch.enumerate_swapchain_images=enumerate;dispatch.destroy_swapchain=destroy;
    dispatch.acquire_swapchain_image=[](XrSwapchain,const XrSwapchainImageAcquireInfo*,unsigned* index){*index=0;return XR_SUCCESS;};
    dispatch.wait_swapchain_image=[](XrSwapchain,const XrSwapchainImageWaitInfo*){return XR_SUCCESS;};
    dispatch.release_swapchain_image=[](XrSwapchain,const XrSwapchainImageReleaseInfo*){++releases;return XR_SUCCESS;};
    dispatch.wait_frame=[](XrSession,const XrFrameWaitInfo*,XrFrameState*){return XR_SUCCESS;};
    dispatch.begin_frame=[](XrSession,const XrFrameBeginInfo*){return XR_SUCCESS;};
    dispatch.end_frame=[](XrSession,const XrFrameEndInfo*){return XR_SUCCESS;};
    SessionState owner;owner.session=h<XrSession>(909);owner.running=true;owner.graphics_api=11;
    owner.graphics_device=d11.Get();owner.view_configuration=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    update_fg_at_frame_boundary(owner,dispatch,true);
    auto p=std::dynamic_pointer_cast<cheeky::fg::BufferedPresenter>(owner.fg);
    check(p&&p->interop11&&p->queue&&p->device,"D3D11 OpenXR session creates a D3D12 FG bridge");
    const auto source_chain=h<XrSwapchain>(99999);
    {
        auto& bridge=*p->interop11;
        D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=8;desc.ArraySize=desc.MipLevels=1;
        desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> source;check(SUCCEEDED(d11->CreateTexture2D(&desc,nullptr,&source)),"Interop cycle source");
        ComPtr<ID3D12Fence> blocked;check(SUCCEEDED(bridge.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&blocked))),"Interop cycle gate");
        check(bridge.capture(source_chain,source.Get(),[&](ID3D12Resource*){return SUCCEEDED(bridge.queue->Wait(blocked.Get(),1));}),
            "Capture may enqueue private reads while their queue is blocked");
        bool reused{};
        const bool busy=bridge.capture(source_chain,source.Get(),[&](ID3D12Resource*){reused=true;return true;});
        ComPtr<ID3D11Fence> progress;check(SUCCEEDED(bridge.device11->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&progress))),"D3D11 progress fence");
        check(SUCCEEDED(bridge.context->Signal(progress.Get(),1)),"Game context submits work independently of FG");bridge.context->Flush();
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        const bool independent=event&&SUCCEEDED(progress->SetEventOnCompletion(1,event))&&WaitForSingleObject(event,1000)==WAIT_OBJECT_0;
        if(event)CloseHandle(event);
        blocked->Signal(1);bridge.drain();bridge.forget(source_chain);
        check(!busy&&!reused,"A busy shared source skips capture without overwriting an in-flight read");
        check(independent,"Blocked FG work must never stall later commands on the game's D3D11 context");
        std::cout<<"PASS DX11 interop dependency cycle: game context progresses while private FG queue is blocked\n";
    }
    for(const auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_TYPELESS}) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=64;desc.ArraySize=2;desc.MipLevels=1;
        desc.SampleDesc.Count=1;desc.Format=format;desc.Usage=D3D11_USAGE_DEFAULT;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> source;
        check(SUCCEEDED(d11->CreateTexture2D(&desc,nullptr,&source)),"Non-shareable native D3D11 stereo color");
        auto clear=[&](unsigned eye,const float* color) {
            D3D11_RENDER_TARGET_VIEW_DESC view{};view.Format=format==DXGI_FORMAT_B8G8R8A8_TYPELESS?DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:format;
            view.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DARRAY;view.Texture2DArray.FirstArraySlice=eye;view.Texture2DArray.ArraySize=1;
            ComPtr<ID3D11RenderTargetView> target;
            check(SUCCEEDED(d11->CreateRenderTargetView(source.Get(),&view,&target)),"D3D11 eye RTV");
            context->ClearRenderTargetView(target.Get(),color);
        };
        const float red[]{1,0,0,1},blue[]{0,0,1,1},green[]{0,1,0,1};clear(0,red);clear(1,blue);
        const auto declared=format==DXGI_FORMAT_B8G8R8A8_TYPELESS?DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:format;
        check(p->capture11(source_chain,0,source.Get(),64,64,2,declared),"D3D11 OpenXR release captures stereo color");
        auto captured=p->captured.at(source_chain);
        clear(0,green);clear(1,green); // Native image reuse must not alter held FG input.
        for(unsigned eye=0;eye<2;++eye) {
            auto* batch=p->producer.begin();check(batch!=nullptr,"D3D11 eye crop batch");
            const XrRect2Di rect{{0,0},{64,64}};
            const auto layout=cheeky::fg::compact_layout(64,64,true,.5f,.5f,0,0,25,.04f);
            auto image=p->producer.packed_region(*batch,captured->texture.Get(),captured->state,rect,eye,declared,layout);
            std::uint64_t ticket{};
            check(image&&p->producer.execute(*batch,ticket),"D3D11 captured eye compact layout");
            check(p->upload_image(p->scene_chains[eye],image,true),"Compact FG output uploads into a D3D11 OpenXR image");
            auto target=chains.at(p->scene_chains[eye].handle);
            D3D11_TEXTURE2D_DESC read_desc{};target->GetDesc(&read_desc);
            read_desc.Usage=D3D11_USAGE_STAGING;read_desc.BindFlags=read_desc.MiscFlags=0;read_desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> read;
            check(SUCCEEDED(d11->CreateTexture2D(&read_desc,nullptr,&read)),"D3D11 output readback");
            context->CopyResource(read.Get(),target.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(SUCCEEDED(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped)),"D3D11 FG output GPU completion");
            for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
                const auto pixel=reinterpret_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch+x*4;
                const unsigned red_channel=(read_desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM || read_desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)?2:0;
                const bool correct=pixel[eye?2-red_channel:red_channel]>250&&pixel[1]<3&&pixel[eye?red_channel:2-red_channel]<3;
                if(!correct)std::fprintf(stderr,"DX11 bridge pixel format=%u eye=%u x=%u y=%u BGRA/RGBA=%u,%u,%u,%u\n",format,eye,x,y,pixel[0],pixel[1],pixel[2],pixel[3]);
                check(correct,
                    "D3D11 FG output retains immutable left red/right blue across API fences and compact assembly");
            }
            context->Unmap(read.Get(),0);
        }
        p->interop11->forget(source_chain);
    }
    check(releases==4,"D3D11 private images release exactly once after upload");
    p.reset();owner.fg.reset();check(chains.empty(),"D3D11 FG private chains retire on shutdown");device=nullptr;
    std::cout<<"PASS D3D11 OpenXR FG bridge: non-shareable stereo arrays, RGBA and typeless BGRA/sRGB, immutable capture, compact output GPU pixels and shutdown\n";
}
