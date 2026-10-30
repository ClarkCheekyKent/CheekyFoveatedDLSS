#pragma once
#include "../third_party/nvof/nvOpticalFlowD3D11.h"
#include "../third_party/nvof/nvOpticalFlowD3D12.h"
#include "../shared/fg_optical_shader.hpp"
#include "../shared/fg_precompiled.hpp"
#include "fg_matrices.hpp"
#include "../shared/cheeky_fg_bridge.hpp"
#include "../shared/cheeky_fg_shader11.hpp"
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <mutex>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <cstdio>
#include <vector>
#include <memory>
namespace cheeky::fg {
namespace optical {
using Microsoft::WRL::ComPtr;
inline bool color_options(CheekyFgColorOptionsV1& options){
    const auto runtime=GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll");
    const auto get=runtime?reinterpret_cast<CheekyFgColorOptionsFn>(GetProcAddress(runtime,"CheekyFG_GetColorOptionsV1")):nullptr;
    return get&&get(&options)&&options.enabled;
}
// Sessions rebuilt after their first initialization (each stalls generation).
inline std::atomic<std::uint64_t> rebuilds{};
inline HMODULE module(){static HMODULE dll=LoadLibraryExW(L"nvofapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);return dll;}
inline NV_OF_INIT_PARAMS parameters(unsigned w,unsigned h,unsigned preset,unsigned grid){
    NV_OF_INIT_PARAMS p{};p.width=w;p.height=h;p.outGridSize=NV_OF_OUTPUT_VECTOR_GRID_SIZE(grid);
    p.mode=NV_OF_MODE_OPTICALFLOW;
    p.perfLevel=preset==0?NV_OF_PERF_LEVEL_SLOW:preset==1?NV_OF_PERF_LEVEL_MEDIUM:NV_OF_PERF_LEVEL_FAST;
    p.enableOutputCost=NV_OF_TRUE;p.predDirection=NV_OF_PRED_DIRECTION_BOTH;
    p.inputBufferFormat=NV_OF_BUFFER_FORMAT_ABGR8;return p;
}
inline DXGI_FORMAT input_format(){return DXGI_FORMAT_B8G8R8A8_UNORM;}
inline bool cached(const OpticalConstants& c){return c.texel[3]>=4&&c.texel[3]<8;}
template<class Count,class Formats> bool supports_format(NvOFHandle handle,Count count,Formats formats,NV_OF_BUFFER_USAGE usage,DXGI_FORMAT format){
    unsigned n{};if(count(handle,usage,NV_OF_MODE_OPTICALFLOW,&n)!=NV_OF_SUCCESS||!n)return false;
    std::vector<DXGI_FORMAT> values(n);if(formats(handle,usage,NV_OF_MODE_OPTICALFLOW,values.data())!=NV_OF_SUCCESS)return false;
    return std::find(values.begin(),values.end(),format)!=values.end();
}
inline bool supported(NvOFHandle handle,PFNNVOFGETCAPS caps,unsigned w,unsigned h){
    const NV_OF_CAPS names[]{NV_OF_CAPS_WIDTH_MIN,NV_OF_CAPS_HEIGHT_MIN,NV_OF_CAPS_WIDTH_MAX,NV_OF_CAPS_HEIGHT_MAX};
    unsigned values[4]{};for(unsigned i=0;i<4;++i){unsigned count=1;if(caps(handle,names[i],&values[i],&count)!=NV_OF_SUCCESS)return false;}
    return w>=values[0]&&h>=values[1]&&w<=values[2]&&h<=values[3];
}
inline bool supports_grid(NvOFHandle handle,PFNNVOFGETCAPS caps,unsigned grid){
    unsigned count{};if(caps(handle,NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,nullptr,&count)!=NV_OF_SUCCESS||!count||count>8)return grid==4;
    unsigned values[8]{};if(caps(handle,NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,values,&count)!=NV_OF_SUCCESS)return grid==4;
    return std::find(values,values+count,grid)!=values+count;
}
inline OpticalConstants constants(const CheekyFgEye& g,unsigned pass,const float* rect,
    const CompactLayout& old_layout,const CompactLayout& layout,unsigned outw,unsigned outh){
    OpticalConstants c;std::copy_n(rect,4,c.output_rect);std::copy_n(rect,4,c.flow_rect);
    // Include a halo so trajectories can cross the center/periphery boundary.
    // Head turns move content tens of pixels per native frame at low rates.
    // At an eye edge the window slides inward instead of shrinking: its size
    // sets the flow session's size, and a new size rebuilds the session.
    if(pass==0){const float halo_x=std::max(.05f,32.f/std::max(1u,g.width)),halo_y=std::max(.05f,32.f/std::max(1u,g.height));
        c.flow_rect[2]=std::min(1.f,rect[2]+2*halo_x);c.flow_rect[3]=std::min(1.f,rect[3]+2*halo_y);
        c.flow_rect[0]=std::clamp(rect[0]-halo_x,0.f,1.f-c.flow_rect[2]);c.flow_rect[1]=std::clamp(rect[1]-halo_y,0.f,1.f-c.flow_rect[3]);}
    const auto percent=pass?g.optical.peripheral_percent:g.optical.center_percent;
    const auto grid=pass?g.optical.peripheral_grid:g.optical.center_grid;
    c.sizes[0]=float(std::max(64u,(unsigned(std::ceil(g.width*c.flow_rect[2]*percent*.01f))+3u)&~3u));
    c.sizes[1]=float(std::max(64u,(unsigned(std::ceil(g.height*c.flow_rect[3]*percent*.01f))+3u)&~3u));
    c.sizes[2]=float(outw);c.sizes[3]=float(outh);
    layout.constants(c.crop,c.spans,c.texel);float unused[4];old_layout.constants(c.old_crop,c.old_spans,unused);
    c.texel[0]=float(grid==1||grid==2?grid:4u);
    c.texel[1]=float(4u*std::min(2u,pass?g.optical.peripheral_validation:g.optical.center_validation));
    const auto method=pass?g.peripheral_method:g.center_method;
    c.texel[3]=float(cheeky_fg_interpolation(method));
    if(cheeky_fg_cacheable(method))c.texel[3]+=4;return c;
}
// Head-rotation reprojection between the previous, current and submitted
// in-between cameras. The in-between pose matches the presenter's midpoint().
inline void pose_mapping(OpticalConstants& c,const XrPosef& previous,const XrFovf& previous_fov,
    const XrPosef& current,const XrFovf& current_fov,float t) noexcept {
    const auto options=unsigned(c.texel[1])&~1u;c.texel[1]=float(options);
    if(!finite_pose_orientation(previous)||!finite_pose_orientation(current)||
       !finite_camera_fov(previous_fov)||!finite_camera_fov(current_fov)||!std::isfinite(t))return;
    struct Q{float x,y,z,w;};
    auto normal=[](Q q){const float n=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);return Q{q.x/n,q.y/n,q.z/n,q.w/n};};
    auto conj=[](Q q){return Q{-q.x,-q.y,-q.z,q.w};};
    auto mul=[](Q a,Q b){return Q{a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};};
    const auto& po=previous.orientation;const auto& co=current.orientation;
    const Q old_q=normal({po.x,po.y,po.z,po.w});Q now_q=normal({co.x,co.y,co.z,co.w});
    Q mid_q=now_q;if(old_q.x*now_q.x+old_q.y*now_q.y+old_q.z*now_q.z+old_q.w*now_q.w<0)mid_q={-now_q.x,-now_q.y,-now_q.z,-now_q.w};
    mid_q=normal({old_q.x*(1-t)+mid_q.x*t,old_q.y*(1-t)+mid_q.y*t,old_q.z*(1-t)+mid_q.z*t,old_q.w*(1-t)+mid_q.w*t});
    const Q now_to_old=mul(conj(old_q),now_q),mid_to_now=mul(conj(now_q),mid_q);
    auto tangents=[](const XrFovf& f,float* out){out[0]=std::tan(f.angleLeft);out[1]=std::tan(f.angleRight);out[2]=std::tan(f.angleUp);out[3]=std::tan(f.angleDown);};
    XrFovf mid_fov{previous_fov.angleLeft*(1-t)+current_fov.angleLeft*t,previous_fov.angleRight*(1-t)+current_fov.angleRight*t,
        previous_fov.angleUp*(1-t)+current_fov.angleUp*t,previous_fov.angleDown*(1-t)+current_fov.angleDown*t};
    tangents(previous_fov,c.old_tangents);tangents(current_fov,c.now_tangents);tangents(mid_fov,c.mid_tangents);
    const float a[]{now_to_old.x,now_to_old.y,now_to_old.z,now_to_old.w},b[]{mid_to_now.x,mid_to_now.y,mid_to_now.z,mid_to_now.w};
    for(float v:a)if(!std::isfinite(v))return;for(float v:b)if(!std::isfinite(v))return;
    std::copy_n(a,4,c.now_to_old);std::copy_n(b,4,c.mid_to_now);c.texel[1]=float(options|1u);
}
// Fixed optical shaders ship as DXBC. Creating a session (including while the
// D3D11 context is held) must never invoke the HLSL optimizer on the game thread.
inline bool compile(const char* entry,const char* profile,ComPtr<ID3DBlob>& code){
    code.Reset();const auto bytes=bytecode::optical(entry,profile);
    if(bytes.empty()||FAILED(D3DCreateBlob(bytes.size(),&code)))return false;
    std::memcpy(code->GetBufferPointer(),bytes.data(),bytes.size());return true;
}
// Private D3D11 device for the optical-flow engine. Flow queued on the game's
// device counted toward its frame-latency throttle (its Present blocked until
// our OFA work retired) and ordered the game's next frame after it. The game
// context and this device order their work through two shared fences.
struct FlowDevice11 {
    ComPtr<ID3D11Device5> device;ComPtr<ID3D11DeviceContext4> context,game_context;ComPtr<ID3D11Device1> game_device;
    ComPtr<ID3D11Fence> game_fence,game_view; // Signaled by the game context; game_view is this device's handle.
    ComPtr<ID3D11Fence> flow_fence,flow_view; // Signaled by this context; flow_view is the game device's handle.
    std::uint64_t game_value{}; // Game immediate-context guard.
    std::uint64_t flow_value{}; // mx.
    std::mutex mx; // This device's immediate context. Taken after, never before, the game context guard.
    char reason[160]{};
    bool create(ID3D11Device* dev,ID3D11DeviceContext* ctx) noexcept {
        auto fail=[&](const char* what,HRESULT hr){snprintf(reason,sizeof(reason),"%s (0x%08lx)",what,static_cast<unsigned long>(hr));
            game_fence.Reset();game_view.Reset();flow_fence.Reset();flow_view.Reset();context.Reset();device.Reset();return false;};
        ComPtr<ID3D11Device5> game5;HRESULT hr=dev->QueryInterface(IID_PPV_ARGS(&game5));if(FAILED(hr))return fail("game device has no D3D11 fences",hr);
        if(FAILED(hr=ctx->QueryInterface(IID_PPV_ARGS(&game_context)))||FAILED(hr=game5.As(&game_device)))return fail("game context has no D3D11 fences",hr);
        ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;
        if(FAILED(hr=dev->QueryInterface(IID_PPV_ARGS(&dxgi)))||FAILED(hr=dxgi->GetAdapter(&adapter)))return fail("adapter lookup",hr);
        const D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;ComPtr<ID3D11Device> created;ComPtr<ID3D11DeviceContext> immediate;
        if(FAILED(hr=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,&level,1,D3D11_SDK_VERSION,&created,nullptr,&immediate)))return fail("private device",hr);
        if(FAILED(hr=created.As(&device))||FAILED(hr=immediate.As(&context)))return fail("private device has no D3D11 fences",hr);
        auto open=[&](ID3D11Device5* owner,ID3D11Device5* other,ComPtr<ID3D11Fence>& fence,ComPtr<ID3D11Fence>& view){HANDLE handle{};
            HRESULT result=owner->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence));
            if(SUCCEEDED(result))result=fence->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle);
            if(SUCCEEDED(result)){result=other->OpenSharedFence(handle,IID_PPV_ARGS(&view));CloseHandle(handle);}
            return result;};
        if(FAILED(hr=open(game5.Get(),device.Get(),game_fence,game_view)))return fail("shared game fence",hr);
        if(FAILED(hr=open(device.Get(),game5.Get(),flow_fence,flow_view)))return fail("shared flow fence",hr);
        return true;
    }
    // A texture of this device and the game device's view of the same memory.
    bool shared(D3D11_TEXTURE2D_DESC d,ComPtr<ID3D11Texture2D>& own,ComPtr<ID3D11Texture2D>& game) noexcept {
        d.MiscFlags|=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;ComPtr<IDXGIResource1> resource;HANDLE handle{};
        if(FAILED(device->CreateTexture2D(&d,nullptr,&own))||FAILED(own.As(&resource))||
           FAILED(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle)))return false;
        const bool ok=SUCCEEDED(game_device->OpenSharedResource1(handle,IID_PPV_ARGS(&game)));CloseHandle(handle);return ok;
    }
};
struct Error {
    char diagnostic[384]{"NVIDIA Optical Flow: waiting for color history"};
    bool fail(const char* stage,unsigned status=0){sprintf_s(diagnostic,"NVIDIA Optical Flow: %s (0x%x); native presentation continues",stage,status);return false;}
};
// The owning backend serializes every call. The driver DLL stays resident.
struct Session11 : Error {
    NV_OF_D3D11_API_FUNCTION_LIST api{};NvOFHandle handle{};
    ComPtr<ID3D11Device> device;ComPtr<ID3D11ComputeShader> prepare_shader,interpolate_shader,cache_shader;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> constants_buffer;
    struct Output {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        ComPtr<ID3D11UnorderedAccessView> target;
        unsigned width{},height{};
    };
    std::vector<std::shared_ptr<Output>> outputs;
    std::atomic<unsigned> output_allocations{},output_reuses{};
    std::array<ComPtr<ID3D11Texture2D>,8> textures;
    std::array<ComPtr<ID3D11ShaderResourceView>,8> views;
    std::array<NvOFGPUBufferHandle,6> buffers{};
    ComPtr<ID3D11UnorderedAccessView> input_uavs[2],cache_uavs[2];
    bool cache_ready{};unsigned cache_builds{};
    unsigned width{},height{},preset{},grid{4};std::uint64_t old_id{},new_id{};
    float crop[4]{};bool history{},initialized{};
    // Private flow (flow set): the engine runs on flow->device. textures[0..1]
    // are the game device's views of shared inputs; vectors live in flow sets.
    FlowDevice11* flow{};
    ComPtr<ID3D11Texture2D> own_inputs[2];
    std::atomic<std::uint64_t> inputs_consumed{}; // Flow fence value after the engine read the inputs.
    // One pair's vectors/costs. The game context reads a set at publication
    // while the engine already estimates the next pair into another one.
    struct FlowSet {
        std::array<ComPtr<ID3D11Texture2D>,4> own,textures;std::array<ComPtr<ID3D11ShaderResourceView>,4> views;
        std::array<NvOFGPUBufferHandle,4> buffers{};
        std::array<ComPtr<ID3D11Texture2D>,2> cache;std::array<ComPtr<ID3D11ShaderResourceView>,2> cache_views;
        std::array<ComPtr<ID3D11UnorderedAccessView>,2> cache_uavs;
        bool cache_ready{}; // Game context guard; reset only while no draw holds the set.
        std::atomic<std::uint64_t> ready{},released{}; // Flow fence after estimation; game fence after the last read.
    };
    std::vector<std::shared_ptr<FlowSet>> sets;
    std::shared_ptr<FlowSet> current;
    // Created by initialize() (game guard, then flow->mx): game-device calls
    // never run while only flow->mx is held. Current pair, a newer pair and two
    // pairs with unpublished phases.
    static constexpr unsigned flow_sets=4;
    bool create_set(){
        auto s=std::make_shared<FlowSet>();
        for(unsigned i=0;i<4;++i){D3D11_TEXTURE2D_DESC d{};d.Width=(width+grid-1)/grid;d.Height=(height+grid-1)/grid;d.ArraySize=d.MipLevels=1;d.SampleDesc.Count=1;
            d.Format=i<2?DXGI_FORMAT_R16G16_SINT:DXGI_FORMAT_R8_UINT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            if(!flow->shared(d,s->own[i],s->textures[i])||FAILED(device->CreateShaderResourceView(s->textures[i].Get(),nullptr,&s->views[i])))return fail("shared flow buffer");
            if(const auto status=api.nvOFRegisterResourceD3D11(handle,s->own[i].Get(),&s->buffers[i])){
                for(unsigned j=0;j<i;++j)api.nvOFUnregisterResourceD3D11(s->buffers[j]);return fail("flow buffer registration",status);}
        }
        sets.push_back(std::move(s));return true;
    }
    std::shared_ptr<FlowSet> acquire_set(){
        for(auto& s:sets)if(s.use_count()==1)return s;
        fail("every flow set is still held by unpublished frames");return {};
    }
    // Rebuilding a session (driver init, registrations, shader compiles)
    // stalls generation for seconds. The current session samples any flow
    // window at its own size; it is rebuilt only once a different size has
    // been requested for about a second, not for a transient full-eye frame.
    unsigned size_mismatch{};
    bool keep_size(unsigned w,unsigned h) noexcept {
        const bool close=w<=width&&h<=height&&3ull*w*h>=2ull*width*height;
        size_mismatch=close?0u:size_mismatch+1u;
        return size_mismatch<90u;
    }
    Session11()=default;Session11(const Session11&)=delete;Session11& operator=(const Session11&)=delete;
    ~Session11(){reset();}
    // Hold flow->mx when flow is set. Draws keep their sets' game views alive.
    void reset(){
        for(auto& b:buffers)if(b){api.nvOFUnregisterResourceD3D11(b);b=nullptr;}
        for(auto& s:sets)for(auto& b:s->buffers)if(b){api.nvOFUnregisterResourceD3D11(b);b=nullptr;}
        if(handle){api.nvOFDestroy(handle);handle=nullptr;}
        sets.clear();current.reset();for(auto& t:own_inputs)t.Reset();inputs_consumed=0;
        for(auto& t:textures)t.Reset();for(auto& v:views)v.Reset();for(auto& v:input_uavs)v.Reset();for(auto& v:cache_uavs)v.Reset();
        cache_shader.Reset();cache_ready=false;cache_builds=0;constants_buffer.Reset();outputs.clear();
        device.Reset();prepare_shader.Reset();interpolate_shader.Reset();sampler.Reset();old_id=new_id=0;history=false;initialized=false;
    }
    bool initialize(ID3D11Device* dev,ID3D11DeviceContext* context,unsigned w,unsigned h,unsigned quality,unsigned cells){
        if(initialized&&device.Get()==dev&&preset==quality&&grid==cells&&keep_size(w,h))return true;
        std::unique_lock<std::mutex> engine;if(flow)engine=std::unique_lock(flow->mx);
        if(initialized)++rebuilds;size_mismatch=0;
        reset();auto dll=module();if(!dll)return fail("driver API unavailable",GetLastError());
        using Create=decltype(&NvOFAPICreateInstanceD3D11);auto create=reinterpret_cast<Create>(GetProcAddress(dll,"NvOFAPICreateInstanceD3D11"));
        if(!create)return fail("D3D11 API unavailable");
        auto status=create(NV_OF_API_VERSION,&api);if(status)return fail("D3D11 API version",status);
        status=flow?api.nvCreateOpticalFlowD3D11(flow->device.Get(),flow->context.Get(),&handle):api.nvCreateOpticalFlowD3D11(dev,context,&handle);
        if(status)return fail("D3D11 device unsupported",status);
        device=dev;width=w;height=h;preset=quality;grid=cells;
        if(!supported(handle,api.nvOFGetCaps,w,h))return fail("flow dimensions unsupported");
        if(!supports_grid(handle,api.nvOFGetCaps,cells))return fail("vector grid unsupported by this GPU; use 4x4",cells);
        const auto format=input_format();
        if(!supports_format(handle,api.nvOFGetSurfaceFormatCountD3D11,api.nvOFGetSurfaceFormatD3D11,NV_OF_BUFFER_USAGE_INPUT,format))return fail("RGB input unsupported");
        auto params=parameters(w,h,quality,cells);status=api.nvOFInit(handle,&params);if(status)return fail("session initialization",status);
        for(unsigned i=0;i<6;++i){
            D3D11_TEXTURE2D_DESC d{};d.Width=i<2?w:(w+cells-1)/cells;d.Height=i<2?h:(h+cells-1)/cells;d.ArraySize=d.MipLevels=1;d.SampleDesc.Count=1;
            d.Format=i<2?format:i<4?DXGI_FORMAT_R16G16_SINT:DXGI_FORMAT_R8_UINT;
            d.BindFlags=D3D11_BIND_SHADER_RESOURCE|(i<2?D3D11_BIND_UNORDERED_ACCESS:0);
            if(flow){if(i>=2)continue;
                if(!flow->shared(d,own_inputs[i],textures[i])||FAILED(dev->CreateShaderResourceView(textures[i].Get(),nullptr,&views[i]))||
                   FAILED(dev->CreateUnorderedAccessView(textures[i].Get(),nullptr,&input_uavs[i])))return fail("shared input");
                status=api.nvOFRegisterResourceD3D11(handle,own_inputs[i].Get(),&buffers[i]);if(status)return fail("buffer registration",status);continue;}
            if(FAILED(dev->CreateTexture2D(&d,nullptr,&textures[i]))||FAILED(dev->CreateShaderResourceView(textures[i].Get(),nullptr,&views[i])))return fail("buffer allocation");
            if(i<2&&FAILED(dev->CreateUnorderedAccessView(textures[i].Get(),nullptr,&input_uavs[i])))return fail("input view");
            status=api.nvOFRegisterResourceD3D11(handle,textures[i].Get(),&buffers[i]);if(status)return fail("buffer registration",status);
        }
        if(flow)for(unsigned i=0;i<flow_sets;++i)if(!create_set())return false;
        ComPtr<ID3DBlob> code;if(!compile("Prepare","cs_5_0",code)||FAILED(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&prepare_shader)))return fail("input shader");
        code.Reset();if(!compile("Interpolate","cs_5_0",code)||FAILED(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&interpolate_shader)))return fail("interpolation shader");
        D3D11_SAMPLER_DESC s{};s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;s.MaxLOD=D3D11_FLOAT32_MAX;
        if(FAILED(dev->CreateSamplerState(&s,&sampler)))return fail("sampler");initialized=true;return true;
    }
    bool bind(ID3D11DeviceContext* ctx,const OpticalConstants& c){
        if(!constants_buffer){D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(c);d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            if(FAILED(device->CreateBuffer(&d,nullptr,&constants_buffer)))return fail("constants");}
        // UpdateSubresource captures each value in immediate-context order.
        ctx->UpdateSubresource(constants_buffer.Get(),0,nullptr,&c,0,0);
        auto* b=constants_buffer.Get();auto* s=sampler.Get();ctx->CSSetConstantBuffers(0,1,&b);ctx->CSSetSamplers(0,1,&s);return true;
    }
    static void unbind(ID3D11DeviceContext* ctx){ID3D11ShaderResourceView* srv[9]{};ID3D11UnorderedAccessView* uav[2]{};ctx->CSSetShaderResources(0,9,srv);ctx->CSSetUnorderedAccessViews(0,2,uav,nullptr);}
    // Prepare all eye/region inputs before flushing once and executing flow.
    bool prepare(ID3D11DeviceContext* ctx,ID3D11ShaderResourceView* old,ID3D11ShaderResourceView* now,
        std::uint64_t previous_id,std::uint64_t current_id,unsigned quality,OpticalConstants& c,bool& needed){
        ID3D11Device* dev{};ctx->GetDevice(&dev);ComPtr<ID3D11Device> owned;owned.Attach(dev);
        if(!initialize(dev,ctx,unsigned(c.sizes[0]),unsigned(c.sizes[1]),quality,unsigned(c.texel[0])))return false;
        c.sizes[0]=float(width);c.sizes[1]=float(height);
        needed=old_id!=previous_id||new_id!=current_id;
        if(needed){
            cache_ready=false;
            // The engine may still read the previous pair's inputs.
            if(flow)flow->game_context->Wait(flow->flow_view.Get(),inputs_consumed.load());
            if(!bind(ctx,c))return false;
            ID3D11ShaderResourceView* srvs[]{old,now};ID3D11UnorderedAccessView* uavs[]{input_uavs[0].Get(),input_uavs[1].Get()};
            ctx->CSSetShader(prepare_shader.Get(),nullptr,0);ctx->CSSetShaderResources(0,2,srvs);ctx->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
            ctx->Dispatch((width+7)/8,(height+7)/8,1);unbind(ctx);
        }
        return true;
    }
    bool execute(std::uint64_t previous_id,std::uint64_t current_id,const OpticalConstants& c,bool reset_history){
        if(old_id!=previous_id||new_id!=current_id){
            NV_OF_EXECUTE_INPUT_PARAMS in{};in.inputFrame=buffers[0];in.referenceFrame=buffers[1];
            // Temporal hints describe the preceding interval, whose current
            // endpoint must be this interval's previous endpoint.
            in.disableTemporalHints=(!history||reset_history||new_id!=previous_id||memcmp(crop,c.flow_rect,sizeof(crop)))?NV_OF_TRUE:NV_OF_FALSE;
            NV_OF_EXECUTE_OUTPUT_PARAMS out{};out.outputBuffer=buffers[2];out.bwdOutputBuffer=buffers[3];out.outputCostBuffer=buffers[4];out.bwdOutputCostBuffer=buffers[5];
            // Private flow (hold flow->mx): write a set no publication still reads.
            std::shared_ptr<FlowSet> set;
            if(flow){set=acquire_set();if(!set)return false;flow->context->Wait(flow->game_view.Get(),set->released.load());
                out.outputBuffer=set->buffers[0];out.bwdOutputBuffer=set->buffers[1];out.outputCostBuffer=set->buffers[2];out.bwdOutputCostBuffer=set->buffers[3];}
            auto status=api.nvOFExecute(handle,&in,&out);if(status)return fail("flow execution",status);
            if(set){set->cache_ready=false;set->ready=0;current=std::move(set);}
            old_id=previous_id;new_id=current_id;std::copy_n(c.flow_rect,4,crop);history=true;
        }
        return true;
    }
    // A lease protects deferred G1/G2/G3 consumers from output reuse. Once a
    // draw is enqueued, the immediate context orders its reads before reuse.
    std::shared_ptr<Output> output(unsigned w,unsigned h,bool pooled){
        if(pooled)for(auto& p:outputs)if(p.use_count()==1&&p->width==w&&p->height==h){++output_reuses;return p;}
        auto p=std::make_shared<Output>();p->width=w;p->height=h;
        D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.ArraySize=d.MipLevels=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        if(FAILED(device->CreateTexture2D(&d,nullptr,&p->texture))||FAILED(device->CreateShaderResourceView(p->texture.Get(),nullptr,&p->view))||
           FAILED(device->CreateUnorderedAccessView(p->texture.Get(),nullptr,&p->target))){fail("generated image");return {};}
        ++output_allocations;
        if(pooled){
            if(outputs.size()>=12){const auto free=std::find_if(outputs.begin(),outputs.end(),[](const auto& p){return p.use_count()==1;});
                if(free!=outputs.end())outputs.erase(free);}
            if(outputs.size()<12)outputs.push_back(p);
        }
        return p;
    }
    // depth is the captured R32 depth snapshot when c.flags[2] enables it.
    bool reconstruct(ID3D11DeviceContext* ctx,ID3D11ShaderResourceView* old,ID3D11ShaderResourceView* now,ID3D11ShaderResourceView* depth,
        OpticalConstants c,float fraction,ComPtr<ID3D11Texture2D>& result,ComPtr<ID3D11ShaderResourceView>& result_view,
        std::shared_ptr<Output>* lease=nullptr,FlowSet* set=nullptr){
        auto* dev=device.Get();
        // A flow set (private flow) carries its own vectors and packed cache.
        auto* field_views=set?set->views.data():views.data()+2;
        auto* cache_textures=set?set->cache.data():textures.data()+6;auto* cache_views=set?set->cache_views.data():views.data()+6;
        auto* cache_targets=set?set->cache_uavs.data():cache_uavs;bool& packed=set?set->cache_ready:cache_ready;
        if(cached(c)&&!packed){
            if(!cache_shader){ComPtr<ID3DBlob> code;
                if(!compile("CacheVectors","cs_5_0",code)||FAILED(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&cache_shader)))return fail("vector cache shader");}
            for(unsigned i=0;i<2;++i)if(!cache_textures[i]){
                D3D11_TEXTURE2D_DESC d{};d.Width=(width+grid-1)/grid;d.Height=(height+grid-1)/grid;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
                d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
                if(FAILED(dev->CreateTexture2D(&d,nullptr,&cache_textures[i]))||FAILED(dev->CreateShaderResourceView(cache_textures[i].Get(),nullptr,&cache_views[i]))||
                   FAILED(dev->CreateUnorderedAccessView(cache_textures[i].Get(),nullptr,&cache_targets[i])))return fail("vector cache allocation");
            }
            if(!bind(ctx,c))return false;
            ID3D11ShaderResourceView* fields[]{field_views[0].Get(),field_views[1].Get(),field_views[2].Get(),field_views[3].Get()};
            ID3D11UnorderedAccessView* targets[]{cache_targets[0].Get(),cache_targets[1].Get()};
            ctx->CSSetShader(cache_shader.Get(),nullptr,0);ctx->CSSetShaderResources(2,4,fields);ctx->CSSetUnorderedAccessViews(0,2,targets,nullptr);
            ctx->Dispatch(((width+grid-1)/grid+7)/8,((height+grid-1)/grid+7)/8,1);unbind(ctx);packed=true;++cache_builds;
        }
        auto generated=output(unsigned(c.sizes[2]),unsigned(c.sizes[3]),lease!=nullptr);if(!generated)return false;
        result=generated->texture;result_view=generated->view;if(lease)*lease=generated;
        c.flags[0]=fraction;if(!bind(ctx,c))return false;
        ID3D11ShaderResourceView* srvs[]{old,now,field_views[0].Get(),field_views[1].Get(),field_views[2].Get(),field_views[3].Get(),depth,cache_views[0].Get(),cache_views[1].Get()};auto* uav=generated->target.Get();
        ctx->CSSetShader(interpolate_shader.Get(),nullptr,0);ctx->CSSetShaderResources(0,9,srvs);ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
        ctx->Dispatch((generated->width+7)/8,(generated->height+7)/8,1);unbind(ctx);
        strcpy_s(diagnostic,"NVIDIA Optical Flow: generating");return true;
    }
    bool render(ID3D11DeviceContext* ctx,ID3D11ShaderResourceView* old,ID3D11ShaderResourceView* now,ID3D11ShaderResourceView* depth,
        std::uint64_t previous_id,std::uint64_t current_id,unsigned quality,OpticalConstants c,float fraction,bool reset_history,
        ComPtr<ID3D11Texture2D>& result,ComPtr<ID3D11ShaderResourceView>& result_view){
        bool needed{};
        if(!prepare(ctx,old,now,previous_id,current_id,quality,c,needed))return false;
        if(needed){ctx->Flush();if(!execute(previous_id,current_id,c,reset_history))return false;}
        return reconstruct(ctx,old,now,depth,c,fraction,result,result_view);
    }
};
struct Session12 : Error {
    NV_OF_D3D12_API_FUNCTION_LIST api{};NvOFHandle handle{};
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;std::uint64_t serial{};HANDLE event{};
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> prepare_shader,interpolate_shader,cache_shader;
    ComPtr<ID3D12DescriptorHeap> heap;unsigned stride{};
    std::array<ComPtr<ID3D12Resource>,8> textures;std::array<NvOFGPUBufferHandle,6> buffers{};
    unsigned width{},height{},preset{},grid{4};float crop[4]{};bool history{},initialized{};
    bool cache_ready{};unsigned cache_builds{};
    unsigned descriptor_set{}; // Generated frame within the interval; renders may be in flight together.
    // Rebuilding a session (driver init, registrations, shader compiles)
    // stalls generation for seconds. The current session samples any flow
    // window at its own size; it is rebuilt only once a different size has
    // been requested for about a second, not for a transient full-eye frame.
    unsigned size_mismatch{};
    bool keep_size(unsigned w,unsigned h) noexcept {
        const bool close=w<=width&&h<=height&&3ull*w*h>=2ull*width*height;
        size_mismatch=close?0u:size_mismatch+1u;
        return size_mismatch<90u;
    }
    Session12()=default;Session12(const Session12&)=delete;Session12& operator=(const Session12&)=delete;
    ~Session12(){reset();}
    bool wait(){if(!fence||fence->GetCompletedValue()>=serial)return true;
        if(FAILED(fence->SetEventOnCompletion(serial,event)))return false;return WaitForSingleObject(event,10000)==WAIT_OBJECT_0;}
    void reset(){
        wait();if(queue&&fence){queue->Signal(fence.Get(),++serial);wait();}
        for(auto& b:buffers)if(b){NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 p{b};api.nvOFUnregisterResourceD3D12(&p);b=nullptr;}
        // The driver faults when a registered texture outlives nvOFDestroy.
        for(auto& t:textures)t.Reset();
        if(handle){api.nvOFDestroy(handle);handle=nullptr;}
        device.Reset();queue.Reset();allocator.Reset();list.Reset();fence.Reset();root.Reset();heap.Reset();prepare_shader.Reset();interpolate_shader.Reset();cache_shader.Reset();cache_ready=false;cache_builds=0;
        if(event)CloseHandle(event);event=nullptr;serial=0;history=false;initialized=false;
    }
    static void barrier(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
        if(!r||a==b)return;D3D12_RESOURCE_BARRIER t{};t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;t.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};l->ResourceBarrier(1,&t);
    }
    bool texture(unsigned w,unsigned h,DXGI_FORMAT format,bool writable,ComPtr<ID3D12Resource>& out){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.MipLevels=d.DepthOrArraySize=1;d.SampleDesc.Count=1;d.Format=format;d.Flags=writable?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
        return SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&out)));
    }
    bool initialize(ID3D12Device* dev,ID3D12CommandQueue* q,unsigned w,unsigned h,unsigned quality,unsigned cells){
        if(initialized&&device.Get()==dev&&queue.Get()==q&&preset==quality&&grid==cells&&keep_size(w,h))return true;
        if(initialized)++rebuilds;size_mismatch=0;
        reset();auto dll=module();if(!dll)return fail("driver API unavailable",GetLastError());
        using Create=decltype(&NvOFAPICreateInstanceD3D12);auto create=reinterpret_cast<Create>(GetProcAddress(dll,"NvOFAPICreateInstanceD3D12"));if(!create)return fail("D3D12 API unavailable");
        auto status=create(NV_OF_API_VERSION,&api);if(status)return fail("D3D12 API version",status);
        status=api.nvCreateOpticalFlowD3D12(dev,&handle);if(status)return fail("D3D12 device unsupported",status);
        device=dev;queue=q;width=w;height=h;preset=quality;grid=cells;
        if(!supported(handle,api.nvOFGetCaps,w,h))return fail("flow dimensions unsupported");
        if(!supports_grid(handle,api.nvOFGetCaps,cells))return fail("vector grid unsupported by this GPU; use 4x4",cells);
        const auto format=input_format();
        if(!supports_format(handle,api.nvOFGetSurfaceFormatCountD3D12,api.nvOFGetSurfaceFormatD3D12,NV_OF_BUFFER_USAGE_INPUT,format))return fail("RGB input unsupported");
        auto p=parameters(w,h,quality,cells);status=api.nvOFInit(handle,&p);if(status)return fail("session initialization",status);
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event||FAILED(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)))||FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))||FAILED(dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))))return fail("command resources");
        list->Close();
        for(unsigned i=0;i<6;++i){
            const auto fmt=i<2?format:i<4?DXGI_FORMAT_R16G16_SINT:DXGI_FORMAT_R8_UINT;
            if(!texture(i<2?w:(w+cells-1)/cells,i<2?h:(h+cells-1)/cells,fmt,i<2,textures[i]))return fail("buffer allocation");
            NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 r{};r.resource=textures[i].Get();r.hOFGpuBuffer=&buffers[i];r.inputFencePoint={fence.Get(),serial};r.outputFencePoint={fence.Get(),++serial};
            status=api.nvOFRegisterResourceD3D12(handle,&r);if(status){--serial;return fail("buffer registration",status);}if(!wait())return fail("registration fence");
        }
        D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,9,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,9}};
        D3D12_ROOT_PARAMETER rp[2]{};rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[0].DescriptorTable={2,ranges};rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;rp[1].Constants={0,0,sizeof(OpticalConstants)/4};
        D3D12_STATIC_SAMPLER_DESC s{};s.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;s.MaxLOD=D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=2;rs.pParameters=rp;rs.NumStaticSamplers=1;rs.pStaticSamplers=&s;
        ComPtr<ID3DBlob> blob,error;if(FAILED(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error))||FAILED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))))return fail("root signature");
        for(unsigned i=0;i<2;++i){ComPtr<ID3DBlob> code;if(!compile(i?"Interpolate":"Prepare","cs_5_1",code))return fail("shader compilation");D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=root.Get();ps.CS={code->GetBufferPointer(),code->GetBufferSize()};if(FAILED(dev->CreateComputePipelineState(&ps,IID_PPV_ARGS(i?&interpolate_shader:&prepare_shader))))return fail("compute pipeline");}
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=11*5;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if(FAILED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))))return fail("descriptor heap");stride=dev->GetDescriptorHandleIncrementSize(hd.Type);initialized=true;return true;
    }
    // Eleven descriptors per set: nine SRVs and two UAVs. Set four owns cache preparation.
    void bind(ID3D12GraphicsCommandList* commands,unsigned base,ID3D12Resource* old,ID3D12Resource* now,ID3D12Resource* output,OpticalConstants c,bool preparing,DXGI_FORMAT old_format,DXGI_FORMAT now_format,ID3D12Resource* depth=nullptr,bool caching=false){
        auto cpu=heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=base*stride;
        ID3D12Resource* resources[]{old,now,textures[2].Get(),textures[3].Get(),textures[4].Get(),textures[5].Get(),depth,(caching||preparing)?nullptr:textures[6].Get(),(caching||preparing)?nullptr:textures[7].Get()};
        for(unsigned i=0;i<9;++i){D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=i==0?old_format:i==1?now_format:i<4?DXGI_FORMAT_R16G16_SINT:i<6?DXGI_FORMAT_R8_UINT:i==6?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2D.MipLevels=1;device->CreateShaderResourceView(resources[i],&v,cpu);cpu.ptr+=stride;}
        for(unsigned i=0;i<2;++i){auto* r=caching?textures[6+i].Get():preparing?textures[i].Get():i==0?output:nullptr;D3D12_UNORDERED_ACCESS_VIEW_DESC v{};v.Format=r?r->GetDesc().Format:DXGI_FORMAT_R8G8B8A8_UNORM;v.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(r,nullptr,&v,cpu);cpu.ptr+=stride;}
        auto* h=heap.Get();commands->SetDescriptorHeaps(1,&h);commands->SetComputeRootSignature(root.Get());commands->SetPipelineState(caching?cache_shader.Get():preparing?prepare_shader.Get():interpolate_shader.Get());auto gpu=heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=base*stride;commands->SetComputeRootDescriptorTable(0,gpu);commands->SetComputeRoot32BitConstants(1,sizeof(c)/4,&c,0);
    }
    bool prepare(ID3D12Device* dev,ID3D12CommandQueue* q,ID3D12Resource* old,ID3D12Resource* now,unsigned quality,OpticalConstants c,bool reset_history,
        D3D12_RESOURCE_STATES color_state,DXGI_FORMAT old_format,DXGI_FORMAT now_format,bool defer_completion=false){
        // Only this session's previous submission must retire before its
        // allocator is reused. The flow engine waits on the queue-side fence
        // below, which follows every earlier render that sampled our vectors,
        // so the CPU never waits for unrelated (game) work on a shared queue.
        if(!initialize(dev,q,unsigned(c.sizes[0]),unsigned(c.sizes[1]),quality,unsigned(c.texel[0]))||!wait())return false;
        c.sizes[0]=float(width);c.sizes[1]=float(height);cache_ready=false;
        if(FAILED(allocator->Reset())||FAILED(list->Reset(allocator.Get(),nullptr)))return fail("command reset");
        barrier(list.Get(),old,color_state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);if(now!=old)barrier(list.Get(),now,color_state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(unsigned i=0;i<2;++i)barrier(list.Get(),textures[i].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        bind(list.Get(),0,old,now,nullptr,c,true,old_format,now_format);list->Dispatch((width+7)/8,(height+7)/8,1);
        for(unsigned i=0;i<2;++i)barrier(list.Get(),textures[i].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        barrier(list.Get(),old,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,color_state);if(now!=old)barrier(list.Get(),now,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,color_state);
        if(FAILED(list->Close()))return fail("preparation recording");auto* l=static_cast<ID3D12CommandList*>(list.Get());queue->ExecuteCommandLists(1,&l);
        const auto ready=++serial;if(FAILED(queue->Signal(fence.Get(),ready)))return fail("preparation fence");
        NV_OF_FENCE_POINT start{fence.Get(),ready},done{fence.Get(),++serial};
        NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{};in.inputFrame=buffers[0];in.referenceFrame=buffers[1];in.numFencePoints=1;in.fencePoint=&start;
        in.disableTemporalHints=(!history||reset_history||memcmp(crop,c.flow_rect,sizeof(crop)))?NV_OF_TRUE:NV_OF_FALSE;
        NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{};out.outputBuffer=buffers[2];out.bwdOutputBuffer=buffers[3];out.outputCostBuffer=buffers[4];out.bwdOutputCostBuffer=buffers[5];out.fencePoint=&done;
        auto status=api.nvOFExecuteD3D12(handle,&in,&out);if(status){--serial;return fail("flow execution",status);}
        if(!defer_completion&&!consume())return false;
        std::copy_n(c.flow_rect,4,crop);history=true;strcpy_s(diagnostic,"NVIDIA Optical Flow: generating");return true;
    }
    // Queue after consume(), once per native pair. Later interpolation phases
    // reuse the same packed fields without estimation, filtering passes or CPU waits.
    bool prepare_vectors(ID3D12GraphicsCommandList* commands,OpticalConstants c){
        if(c.texel[3]<3.5||cache_ready)return true;
        if(!cache_shader){ComPtr<ID3DBlob> code;if(!compile("CacheVectors","cs_5_1",code))return fail("vector cache shader");
            D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root.Get();p.CS={code->GetBufferPointer(),code->GetBufferSize()};
            if(FAILED(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&cache_shader))))return fail("vector cache pipeline");}
        for(unsigned i=6;i<8;++i)if(!textures[i]&&!texture((width+grid-1)/grid,(height+grid-1)/grid,DXGI_FORMAT_R32G32B32A32_FLOAT,true,textures[i]))return fail("vector cache allocation");
        c.sizes[0]=float(width);c.sizes[1]=float(height);
        for(unsigned i=2;i<6;++i)barrier(commands,textures[i].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(unsigned i=6;i<8;++i)barrier(commands,textures[i].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        bind(commands,44,nullptr,nullptr,nullptr,c,false,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,nullptr,true);
        commands->Dispatch(((width+grid-1)/grid+7)/8,((height+grid-1)/grid+7)/8,1);
        for(unsigned i=6;i<8;++i)barrier(commands,textures[i].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        for(unsigned i=2;i<6;++i)barrier(commands,textures[i].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        cache_ready=true;++cache_builds;return true;
    }
    // GPU-side dependency only. The caller first submits earlier independent
    // work, then consumes this result immediately before recording its reader.
    bool consume(){return SUCCEEDED(queue->Wait(fence.Get(),serial))||fail("optical flow dependency");}
    // depth (R32_FLOAT, COMMON state) is read when c.flags[2] enables it.
    bool render(ID3D12GraphicsCommandList* commands,ID3D12Resource* old,ID3D12Resource* now,ID3D12Resource* depth,OpticalConstants c,float fraction,DXGI_FORMAT format,
        D3D12_RESOURCE_STATES color_state,DXGI_FORMAT old_format,DXGI_FORMAT now_format,ComPtr<ID3D12Resource>& output){
        if(!handle||!prepare_vectors(commands,c))return false;
        c.sizes[0]=float(width);c.sizes[1]=float(height);
        if(!output||output->GetDesc().Width!=unsigned(c.sizes[2])||output->GetDesc().Height!=unsigned(c.sizes[3])||output->GetDesc().Format!=format){output.Reset();if(!texture(unsigned(c.sizes[2]),unsigned(c.sizes[3]),format,true,output))return fail("generated image");}
        barrier(commands,old,color_state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);if(now!=old)barrier(commands,now,color_state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(unsigned i=2;i<6;++i)barrier(commands,textures[i].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(commands,output.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(commands,depth,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(cached(c))for(unsigned i=6;i<8;++i)barrier(commands,textures[i].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        c.flags[0]=fraction;bind(commands,11*(1+descriptor_set),old,now,output.Get(),c,false,old_format,now_format,depth);commands->Dispatch((unsigned(c.sizes[2])+7)/8,(unsigned(c.sizes[3])+7)/8,1);
        barrier(commands,output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        barrier(commands,depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        if(cached(c))for(unsigned i=6;i<8;++i)barrier(commands,textures[i].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        for(unsigned i=2;i<6;++i)barrier(commands,textures[i].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        barrier(commands,old,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,color_state);if(now!=old)barrier(commands,now,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,color_state);return true;
    }
};
}
}
