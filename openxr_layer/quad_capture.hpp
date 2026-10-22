#pragma once
// Fixed quad-view provider for D3D11 and D3D12. This does not
// implement Cheeky's per-view SR/NR/FG policies or advertise eye tracking.
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <d3d12.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <memory>
#include <functional>
#include "quad_role_mapping.hpp"
#include "quad_fg.hpp"

namespace cheeky::quad_capture {
using Microsoft::WRL::ComPtr;
inline std::recursive_mutex d3d11_dispatch_mutex;
constexpr auto configuration = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
inline bool is_quad(XrViewConfigurationType type) { return type == configuration; }

inline XrFovf inset_fov(const XrFovf& outer) {
    const float l=std::tan(outer.angleLeft),r=std::tan(outer.angleRight);
    const float d=std::tan(outer.angleDown),u=std::tan(outer.angleUp);
    const float w=(r-l)*.5f,h=(u-d)*.5f;
    const float x=std::clamp(-w*.5f,l,r-w),y=std::clamp(-h*.5f,d,u-h);
    return {std::atan(x),std::atan(x+w),std::atan(y+h),std::atan(y)};
}
inline std::array<float,4> inset_rect(const XrFovf& outer,const XrFovf& inner) {
    const float l=std::tan(outer.angleLeft),r=std::tan(outer.angleRight);
    const float d=std::tan(outer.angleDown),u=std::tan(outer.angleUp);
    return {(std::tan(inner.angleLeft)-l)/(r-l),(u-std::tan(inner.angleUp))/(u-d),
        (std::tan(inner.angleRight)-std::tan(inner.angleLeft))/(r-l),
        (std::tan(inner.angleUp)-std::tan(inner.angleDown))/(u-d)};
}
template<class T> XrResult enumerate(const std::vector<T>& values,uint32_t capacity,uint32_t* count,T* output) {
    if(!count || (capacity && !output))return XR_ERROR_VALIDATION_FAILURE;
    *count=static_cast<uint32_t>(values.size());
    if(!capacity)return XR_SUCCESS;
    if(capacity<values.size())return XR_ERROR_SIZE_INSUFFICIENT;
    std::copy(values.begin(),values.end(),output);return XR_SUCCESS;
}

struct Input {
    ID3D12Resource* texture{};
    XrSwapchainSubImage image{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    ID3D11Texture2D* texture11{};
    bool valid() const {
        if(!texture)return false;
        const auto desc=texture->GetDesc();const auto& r=image.imageRect;
        return desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&desc.SampleDesc.Count==1&&
            !(desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)&&image.imageArrayIndex<desc.DepthOrArraySize&&
            r.offset.x>=0&&r.offset.y>=0&&r.extent.width>0&&r.extent.height>0&&
            uint64_t(r.offset.x)+r.extent.width<=desc.Width&&uint64_t(r.offset.y)+r.extent.height<=desc.Height;
    }
};
inline constexpr char shader_source[]=R"HLSL(
Texture2DArray<float4> Context:register(t0);Texture2DArray<float4> Focus:register(t1);
SamplerState Linear:register(s0);
cbuffer Params:register(b0){float4 ContextUV;float4 FocusUV;float4 Inset;float2 Feather;float2 Pad;};
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};
V VS(uint id:SV_VertexID){V v;v.uv=float2((id<<1)&2,id&2);v.p=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;}
float4 PS(V v):SV_Target{
 float4 c=Context.SampleLevel(Linear,float3(ContextUV.xy+v.uv*ContextUV.zw,0),0);
 float2 f=(v.uv-Inset.xy)/Inset.zw;
 float2 edge=min(f,1-f)*Inset.zw;
 float blend=saturate(min(edge.x/Feather.x,edge.y/Feather.y));
 float4 center=Focus.SampleLevel(Linear,float3(FocusUV.xy+saturate(f)*FocusUV.zw,0),0);
 return lerp(c,center,blend);
})HLSL";

struct Compositor11 {
    struct Eye {
        XrSwapchain chain{};unsigned width{},height{};DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        std::vector<XrSwapchainImageD3D11KHR> images;
        uint32_t index{};bool acquired{},waited{};
    };
    XrSession session{};
    PFN_xrCreateSwapchain create{};PFN_xrDestroySwapchain destroy{};
    PFN_xrEnumerateSwapchainImages enumerate_images{};
    PFN_xrAcquireSwapchainImage acquire{};PFN_xrWaitSwapchainImage wait{};PFN_xrReleaseSwapchainImage release{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate,deferred;
    ComPtr<ID3D11Multithread> multithread;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth;
    std::unique_ptr<RoleMapping11> mapping;
    std::unique_ptr<QuadFg11> fg_guides;
    std::shared_ptr<cheeky::fg::D3D11Bridge> fg_bridge;
    std::array<XrSwapchain,4> source_chains{};
    std::vector<ComPtr<IUnknown>> recorded_views; // Alive until the command list has executed.
    // Invoked while the composite output is still acquired, before release.
    std::function<void(unsigned,XrSwapchain,unsigned,ID3D11Texture2D*,unsigned,unsigned,DXGI_FORMAT)> capture_output;
    std::array<Eye,2> eyes;
    std::array<XrViewConfigurationView,2> recommended{};
    bool initialize() {
        if(vs)return true;
        if(!device||!create||!destroy||!enumerate_images||!acquire||!wait||!release)return false;
        if(!mapping)mapping=std::make_unique<RoleMapping11>(device.Get());
        if(!fg_guides)fg_guides=std::make_unique<QuadFg11>(device.Get());
        device->GetImmediateContext(&immediate);
        if(FAILED(immediate.As(&multithread)))return false;
        multithread->SetMultithreadProtected(TRUE);
        ComPtr<ID3DBlob> vertex,pixel,error;
        if(FAILED(device->CreateDeferredContext(0,&deferred))||
           FAILED(D3DCompile(shader_source,sizeof(shader_source)-1,nullptr,nullptr,nullptr,"VS","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vertex,&error))||
           FAILED(D3DCompile(shader_source,sizeof(shader_source)-1,nullptr,nullptr,nullptr,"PS","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&pixel,&error))||
           FAILED(device->CreatePixelShader(pixel->GetBufferPointer(),pixel->GetBufferSize(),nullptr,&ps)))return false;
        D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=64;buffer.Usage=D3D11_USAGE_DEFAULT;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SAMPLER_DESC sample{};sample.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sample.AddressU=sample.AddressV=sample.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sample.MaxLOD=D3D11_FLOAT32_MAX;
        D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;
        D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthFunc=D3D11_COMPARISON_ALWAYS;
        // Publish the ready flag last, just as on the D3D12 path.
        return SUCCEEDED(device->CreateBuffer(&buffer,nullptr,&constants))&&
            SUCCEEDED(device->CreateSamplerState(&sample,&sampler))&&SUCCEEDED(device->CreateRasterizerState(&rs,&raster))&&
            SUCCEEDED(device->CreateDepthStencilState(&ds,&depth))&&
            SUCCEEDED(device->CreateVertexShader(vertex->GetBufferPointer(),vertex->GetBufferSize(),nullptr,&vs));
    }
    ~Compositor11(){for(auto& eye:eyes)if(eye.chain&&destroy)destroy(eye.chain);}
    bool output(unsigned eye,DXGI_FORMAT format) {
        auto& e=eyes[eye];if(e.chain)return e.format==format&&!e.images.empty();
        e.width=recommended[eye].recommendedImageRectWidth;e.height=recommended[eye].recommendedImageRectHeight;e.format=format;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format=format;info.sampleCount=1;info.width=e.width;info.height=e.height;info.faceCount=info.arraySize=info.mipCount=1;
        XrSwapchain chain{};
        if(XR_FAILED(create(session,&info,&chain)))return false;
        uint32_t count{};bool ok=XR_SUCCEEDED(enumerate_images(chain,0,&count,nullptr))&&count;
        if(ok){e.images.assign(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            ok=XR_SUCCEEDED(enumerate_images(chain,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(e.images.data())))&&count==e.images.size();}
        if(!ok){destroy(chain);e.images.clear();return false;}
        e.chain=chain;return true;
    }
    static bool valid(const Input& input) {
        if(!input.texture11)return false;
        D3D11_TEXTURE2D_DESC desc{};input.texture11->GetDesc(&desc);const auto& r=input.image.imageRect;
        return desc.SampleDesc.Count==1&&(desc.BindFlags&D3D11_BIND_SHADER_RESOURCE)&&input.image.imageArrayIndex<desc.ArraySize&&
            r.offset.x>=0&&r.offset.y>=0&&r.extent.width>0&&r.extent.height>0&&
            uint64_t(r.offset.x)+r.extent.width<=desc.Width&&uint64_t(r.offset.y)+r.extent.height<=desc.Height;
    }
    bool record(unsigned eye,const Input& context,const Input& focus,const XrFovf& outer,const XrFovf& inner,ID3D11Texture2D* target) {
        const auto inset=inset_rect(outer,inner);
        for(float f:inset)if(!std::isfinite(f))return false;
        if(!target||inset[0]<-.001f||inset[1]<-.001f||inset[2]<=0||inset[3]<=0||inset[0]+inset[2]>1.001f||inset[1]+inset[3]>1.001f)return false;
        float params[16]{};std::array<ComPtr<ID3D11ShaderResourceView>,2> sources;
        const Input* inputs[]{&context,&focus};
        for(unsigned i=0;i<2;++i){
            const auto& input=*inputs[i];D3D11_TEXTURE2D_DESC desc{};input.texture11->GetDesc(&desc);const auto& r=input.image.imageRect;
            params[i*4]=float(r.offset.x)/desc.Width;params[i*4+1]=float(r.offset.y)/desc.Height;
            params[i*4+2]=float(r.extent.width)/desc.Width;params[i*4+3]=float(r.extent.height)/desc.Height;
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=input.format;srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Texture2DArray.MipLevels=1;srv.Texture2DArray.FirstArraySlice=input.image.imageArrayIndex;srv.Texture2DArray.ArraySize=1;
            if(FAILED(device->CreateShaderResourceView(input.texture11,&srv,&sources[i])))return false;
        }
        std::copy(inset.begin(),inset.end(),params+8);params[12]=2.f/eyes[eye].width;params[13]=2.f/eyes[eye].height;
        ComPtr<ID3D11RenderTargetView> target_view;D3D11_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format=eyes[eye].format;rtv.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        if(FAILED(device->CreateRenderTargetView(target,&rtv,&target_view)))return false;
        // The recorded draw references these views until ExecuteCommandList.
        // Never let them die when they are unbound at the end of recording.
        recorded_views.insert(recorded_views.end(),{sources[0],sources[1],target_view});
        deferred->UpdateSubresource(constants.Get(),0,nullptr,params,0,0);
        deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        deferred->VSSetShader(vs.Get(),nullptr,0);deferred->PSSetShader(ps.Get(),nullptr,0);
        auto* buffer=constants.Get();deferred->PSSetConstantBuffers(0,1,&buffer);
        auto* sample=sampler.Get();deferred->PSSetSamplers(0,1,&sample);
        ID3D11ShaderResourceView* srvs[]{sources[0].Get(),sources[1].Get()};deferred->PSSetShaderResources(0,2,srvs);
        auto* target_raw=target_view.Get();deferred->OMSetRenderTargets(1,&target_raw,nullptr);
        deferred->OMSetDepthStencilState(depth.Get(),0);
        deferred->RSSetState(raster.Get());D3D11_VIEWPORT viewport{0,0,float(eyes[eye].width),float(eyes[eye].height),0,1};
        deferred->RSSetViewports(1,&viewport);deferred->Draw(3,0);
        ID3D11ShaderResourceView* empty[2]{};deferred->PSSetShaderResources(0,2,empty);deferred->OMSetRenderTargets(0,nullptr,nullptr);
        return true;
    }
    XrResult compose(const std::array<Input,4>& inputs,const XrCompositionLayerProjection& projection,std::array<XrCompositionLayerProjectionView,2>& views) {
        std::lock_guard dispatch(d3d11_dispatch_mutex);
        if(!initialize())return XR_ERROR_RUNTIME_FAILURE;
        for(const auto& input:inputs)if(!valid(input))return XR_ERROR_VALIDATION_FAILURE;
        if(!output(0,inputs[0].format)||!output(1,inputs[1].format))return XR_ERROR_RUNTIME_FAILURE;
        struct ContextLock {ID3D11Multithread* value;ContextLock(ID3D11Multithread* v):value(v){value->Enter();}~ContextLock(){value->Leave();}};
        XrResult result=XR_SUCCESS;
        for(unsigned eye=0;eye<2;++eye){
            auto& e=eyes[eye];
            if(!e.acquired){XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};result=acquire(e.chain,&a,&e.index);
                if(result!=XR_SUCCESS){if(XR_SUCCEEDED(result))result=XR_ERROR_RUNTIME_FAILURE;break;}e.acquired=true;}
            if(!e.waited){XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};w.timeout=XR_INFINITE_DURATION;
                result=wait(e.chain,&w);
                // A timeout is not a successful EndFrame: no runtime frame has
                // been ended yet. Retain this acquisition for a later retry.
                if(result!=XR_SUCCESS){if(XR_SUCCEEDED(result))result=XR_ERROR_RUNTIME_FAILURE;break;}e.waited=true;}
            if(e.index>=e.images.size()||!record(eye,inputs[eye],inputs[eye+2],projection.views[eye].fov,projection.views[eye+2].fov,e.images[e.index].texture)){
                result=XR_ERROR_VALIDATION_FAILURE;break;}
        }
        ComPtr<ID3D11CommandList> commands;
        // Finish even on failure to discard partial recording and reset private state.
        const auto finished=deferred->FinishCommandList(FALSE,&commands);
        if(result==XR_SUCCESS){
            if(FAILED(finished))result=XR_ERROR_RUNTIME_FAILURE;
            else {ContextLock lock(multithread.Get());immediate->ExecuteCommandList(commands.Get(),TRUE);immediate->Flush();
                if(FAILED(device->GetDeviceRemovedReason()))result=XR_ERROR_RUNTIME_FAILURE;}
        }
        commands.Reset();recorded_views.clear();
        for(unsigned eye=0;eye<2;++eye)if(eyes[eye].waited){
            if(result==XR_SUCCESS&&capture_output)capture_output(eye,eyes[eye].chain,eyes[eye].index,eyes[eye].images[eyes[eye].index].texture,
                eyes[eye].width,eyes[eye].height,eyes[eye].format);
            auto& e=eyes[eye];XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};const auto released=release(e.chain,&r);
            if(XR_FAILED(released))result=released;else e.acquired=e.waited=false;
            views[eye]=projection.views[eye];views[eye].next=nullptr;views[eye].subImage={eyes[eye].chain,{{0,0},{int32_t(eyes[eye].width),int32_t(eyes[eye].height)}},0};
        }
        return result;
    }
};
struct Compositor {
    struct Eye {
        XrSwapchain chain{};unsigned width{},height{};DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        std::vector<XrSwapchainImageD3D12KHR> images;
        ComPtr<ID3D12PipelineState> pipeline;
    };
    XrSession session{};
    PFN_xrCreateSwapchain create{};PFN_xrDestroySwapchain destroy{};
    PFN_xrEnumerateSwapchainImages enumerate_images{};
    PFN_xrAcquireSwapchainImage acquire{};PFN_xrWaitSwapchainImage wait{};PFN_xrReleaseSwapchainImage release{};
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D11Device> device11;std::unique_ptr<Compositor11> d3d11;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;uint64_t serial{};
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3DBlob> vs,ps;
    ComPtr<ID3D12DescriptorHeap> srvs,rtvs;
    std::array<Eye,2> eyes;
    std::array<XrViewConfigurationView,2> recommended{};
    // D3D12 FG: receives each composed eye after its GPU work completes,
    // before the image is released (still in RENDER_TARGET state).
    std::function<void(unsigned,XrSwapchain,unsigned,ID3D12Resource*,unsigned,unsigned,DXGI_FORMAT)> capture_output12;
    bool initialize() {
        if(device11){
            if(!d3d11){d3d11=std::make_unique<Compositor11>();d3d11->session=session;d3d11->device=device11;
                d3d11->create=create;d3d11->destroy=destroy;d3d11->enumerate_images=enumerate_images;
                d3d11->acquire=acquire;d3d11->wait=wait;d3d11->release=release;d3d11->recommended=recommended;}
            return d3d11->initialize();
        }
        if(root)return true;
        if(!device||!queue||!create||!destroy||!enumerate_images||!acquire||!wait||!release)return false;
        if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))||
           FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)))||
           FAILED(list->Close())||FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))))return false;
        const auto& shader=shader_source;
        ComPtr<ID3DBlob> error,blob;
        if(FAILED(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"VS","vs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error))||
           FAILED(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"PS","ps_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error)))return false;
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0};
        D3D12_ROOT_PARAMETER p[2]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;p[0].DescriptorTable={1,&range};
        p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[1].Constants={0,0,16};
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=2;signature.pParameters=p;signature.NumStaticSamplers=1;signature.pStaticSamplers=&sampler;
        if(FAILED(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)))return false;
        // Assign root last: its presence means every initialization step succeeded.
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=4;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&srvs))))return false;
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=2;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        return SUCCEEDED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&rtvs)))&&
            SUCCEEDED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    }
    bool idle() {
        if(!fence||fence->GetCompletedValue()>=serial)return !device||SUCCEEDED(device->GetDeviceRemovedReason());
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return false;
        const bool ok=SUCCEEDED(fence->SetEventOnCompletion(serial,event))&&WaitForSingleObject(event,10000)==WAIT_OBJECT_0;
        CloseHandle(event);return ok&&SUCCEEDED(device->GetDeviceRemovedReason());
    }
    ~Compositor(){idle();for(auto& eye:eyes)if(eye.chain&&destroy)destroy(eye.chain);}
    bool output(unsigned eye,DXGI_FORMAT format) {
        auto& e=eyes[eye];if(e.chain)return e.format==format;
        e.width=recommended[eye].recommendedImageRectWidth;e.height=recommended[eye].recommendedImageRectHeight;e.format=format;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format=format;info.sampleCount=1;info.width=e.width;info.height=e.height;info.faceCount=info.arraySize=info.mipCount=1;
        if(XR_FAILED(create(session,&info,&e.chain)))return false;
        uint32_t count{};
        if(XR_FAILED(enumerate_images(e.chain,0,&count,nullptr))||!count)return false;
        e.images.assign(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        if(XR_FAILED(enumerate_images(e.chain,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(e.images.data()))))return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root.Get();
        desc.VS={vs->GetBufferPointer(),vs->GetBufferSize()};desc.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;desc.RasterizerState.DepthClipEnable=TRUE;
        desc.SampleMask=UINT_MAX;desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;desc.NumRenderTargets=1;desc.RTVFormats[0]=format;desc.SampleDesc.Count=1;
        return SUCCEEDED(device->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&e.pipeline)));
    }
    static void barrier(ID3D12GraphicsCommandList* command,const Input& input,bool reading) {
        const auto desc=input.texture->GetDesc();
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=input.texture;
        b.Transition.Subresource=input.image.imageArrayIndex*desc.MipLevels;
        b.Transition.StateBefore=reading?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter=reading?D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_RENDER_TARGET;
        command->ResourceBarrier(1,&b);
    }
    bool record(unsigned eye,const Input& context,const Input& focus,const XrFovf& outer,const XrFovf& inner,ID3D12Resource* target) {
        if(!context.valid()||!focus.valid()||!target||!eyes[eye].pipeline)return false;
        const auto inset=inset_rect(outer,inner);
        for(float f:inset)if(!std::isfinite(f))return false;
        if(inset[0]<-.001f||inset[1]<-.001f||inset[2]<=0||inset[3]<=0||inset[0]+inset[2]>1.001f||inset[1]+inset[3]>1.001f)return false;
        const auto descriptor_stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        float constants[16]{};
        const Input* inputs[]{&context,&focus};
        for(unsigned i=0;i<2;++i){
            const auto& input=*inputs[i];const auto desc=input.texture->GetDesc();const auto& r=input.image.imageRect;
            constants[i*4]=float(r.offset.x)/float(desc.Width);constants[i*4+1]=float(r.offset.y)/desc.Height;
            constants[i*4+2]=float(r.extent.width)/float(desc.Width);constants[i*4+3]=float(r.extent.height)/desc.Height;
            auto handle=srvs->GetCPUDescriptorHandleForHeapStart();handle.ptr+=(eye*2+i)*descriptor_stride;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=input.format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2DArray.MipLevels=1;
            srv.Texture2DArray.FirstArraySlice=input.image.imageArrayIndex;srv.Texture2DArray.ArraySize=1;device->CreateShaderResourceView(input.texture,&srv,handle);
        }
        std::copy(inset.begin(),inset.end(),constants+8);constants[12]=2.f/eyes[eye].width;constants[13]=2.f/eyes[eye].height;
        const bool same=context.texture==focus.texture&&context.image.imageArrayIndex==focus.image.imageArrayIndex;
        barrier(list.Get(),context,true);if(!same)barrier(list.Get(),focus,true);
        auto rtv=rtvs->GetCPUDescriptorHandleForHeapStart();rtv.ptr+=eye*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_RENDER_TARGET_VIEW_DESC desc{};desc.Format=eyes[eye].format;desc.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;device->CreateRenderTargetView(target,&desc,rtv);
        ID3D12DescriptorHeap* heaps[]{srvs.Get()};list->SetDescriptorHeaps(1,heaps);list->SetGraphicsRootSignature(root.Get());list->SetPipelineState(eyes[eye].pipeline.Get());
        auto gpu=srvs->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=eye*2*descriptor_stride;list->SetGraphicsRootDescriptorTable(0,gpu);list->SetGraphicsRoot32BitConstants(1,16,constants,0);
        list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);D3D12_VIEWPORT viewport{0,0,float(eyes[eye].width),float(eyes[eye].height),0,1};
        D3D12_RECT scissor{0,0,LONG(eyes[eye].width),LONG(eyes[eye].height)};list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);
        barrier(list.Get(),context,false);if(!same)barrier(list.Get(),focus,false);return true;
    }
    XrResult compose(const std::array<Input,4>& inputs,const XrCompositionLayerProjection& projection,std::array<XrCompositionLayerProjectionView,2>& views) {
        if(device11)return initialize()?d3d11->compose(inputs,projection,views):XR_ERROR_RUNTIME_FAILURE;
        if(!initialize()||!idle())return XR_ERROR_RUNTIME_FAILURE;
        for(const auto& input:inputs)if(!input.valid())return XR_ERROR_VALIDATION_FAILURE;
        if(!output(0,inputs[0].format)||!output(1,inputs[1].format))return XR_ERROR_RUNTIME_FAILURE;
        if(FAILED(allocator->Reset())||FAILED(list->Reset(allocator.Get(),nullptr)))return XR_ERROR_RUNTIME_FAILURE;
        std::array<uint32_t,2> indices{};std::array<bool,2> acquired{},waited{};
        XrResult result=XR_SUCCESS;
        for(unsigned eye=0;eye<2;++eye){
            XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};result=acquire(eyes[eye].chain,&a,&indices[eye]);if(XR_FAILED(result))break;acquired[eye]=true;
            XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};w.timeout=XR_INFINITE_DURATION;result=wait(eyes[eye].chain,&w);if(result!=XR_SUCCESS)break;waited[eye]=true;
            if(indices[eye]>=eyes[eye].images.size()||!record(eye,inputs[eye],inputs[eye+2],projection.views[eye].fov,projection.views[eye+2].fov,eyes[eye].images[indices[eye]].texture)){result=XR_ERROR_VALIDATION_FAILURE;break;}
        }
        const HRESULT closed=list->Close();
        if(XR_SUCCEEDED(result)&&FAILED(closed))result=XR_ERROR_RUNTIME_FAILURE;
        if(result==XR_SUCCESS){ID3D12CommandList* commands[]{list.Get()};queue->ExecuteCommandLists(1,commands);
            if(FAILED(queue->Signal(fence.Get(),++serial))||!idle())result=XR_ERROR_RUNTIME_FAILURE;}
        for(unsigned eye=0;eye<2;++eye)if(acquired[eye]&&waited[eye]){
            if(result==XR_SUCCESS&&capture_output12)capture_output12(eye,eyes[eye].chain,indices[eye],eyes[eye].images[indices[eye]].texture,
                eyes[eye].width,eyes[eye].height,eyes[eye].format);
            XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};const auto released=release(eyes[eye].chain,&r);if(XR_FAILED(released))result=released;
            views[eye]=projection.views[eye];views[eye].next=nullptr;views[eye].subImage={eyes[eye].chain,{{0,0},{int32_t(eyes[eye].width),int32_t(eyes[eye].height)}},0};
        }
        return result;
    }
};
} // namespace cheeky::quad_capture
