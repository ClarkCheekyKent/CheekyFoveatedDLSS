#pragma once
#include "../shared/cheeky_quad_fg.hpp"
#include "../shared/cheeky_quad_views.h"
#include "../shared/fg_guide_layout.hpp"
#include "fg_d3d11.hpp"
#include "fg_timing11.hpp"
#include <d3dcompiler.h>
#include <atomic>
#include <array>
#include <memory>
#include <algorithm>
#include <cmath>
#include <bit>

namespace cheeky::quad_capture {
// Snapshots the four explicitly assigned views on the game's immediate
// context. Only immutable, leased full-eye guide atlases reach the FG worker.
class QuadFg11 {
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Pair11 {Ptr<ID3D11Texture2D> depth,motion;Ptr<ID3D11ShaderResourceView> depth_srv,motion_srv;
        Ptr<ID3D11UnorderedAccessView> depth_uav,motion_uav;unsigned width{},height{};};
    struct Source {uint64_t id{},generation{},interval{};unsigned evaluations{};CheekyQuadFgSourceV1 metadata;Pair11 image;};
    struct Frame {std::array<Pair11,2> image;std::array<Ptr<ID3D12Resource>,2> depth,motion;};
    struct Lease final:IUnknown {std::atomic<ULONG> refs{1};std::shared_ptr<Frame> frame;
        explicit Lease(std::shared_ptr<Frame> f):frame(std::move(f)){}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{if(!out)return E_POINTER;*out=nullptr;
            if(id!=__uuidof(IUnknown))return E_NOINTERFACE;*out=static_cast<IUnknown*>(this);AddRef();return S_OK;}
        ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}ULONG STDMETHODCALLTYPE Release()override{const auto n=--refs;if(!n)delete this;return n;}};
    Ptr<ID3D11Device> device;Ptr<ID3D11DeviceContext> context;Ptr<ID3D11Multithread> protection;
    // Commands go straight to the game's immediate context inside our own
    // ID3DDeviceContextState (no deferred contexts / command lists): the NVIDIA
    // driver crashed on its worker thread with quad views + FG, and this
    // FG-only work was recorded on a deferred context.
    Ptr<ID3D11DeviceContext1> context1;Ptr<ID3DDeviceContextState> state;
    // Caller holds Guard. Swaps our pipeline state in for the scope's lifetime.
    struct StateScope {QuadFg11& owner;Ptr<ID3DDeviceContextState> previous;
        explicit StateScope(QuadFg11& o):owner(o){owner.context1->SwapDeviceContextState(owner.state.Get(),&previous);}
        ~StateScope(){owner.context1->SwapDeviceContextState(previous.Get(),nullptr);}};
    Ptr<ID3D11ComputeShader> snapshot_shader,atlas_shader;Ptr<ID3D11Buffer> constants;
    std::mutex mutex;std::atomic<bool> enabled{},atlas_ready{};
    std::array<Source,8> sources;std::array<std::shared_ptr<Frame>,4> frames;
    uint64_t interval{1},serial{},last_configuration{},last_built_interval{};
    CheekyQuadFgStatsV1 diagnostics;
    cheeky::fg::StageTimings11 guide_timings;
    struct Guard {ID3D11Multithread* p;explicit Guard(ID3D11Multithread* v):p(v){p->Enter();}~Guard(){p->Leave();}};
    struct Constants {unsigned depth_base[2]{},depth_size[2]{},motion_base[2]{},motion_size[2]{},size[2]{};
        float scale[2]{},jitter[2]{},pad[2]{},inset[4]{};};
    static constexpr char shaders[]=R"(
Texture2D<float> Depth:register(t0);Texture2D<float2> Motion:register(t1);
Texture2D<float> CenterDepth:register(t2);Texture2D<float2> CenterMotion:register(t3);
RWTexture2D<float> OutDepth:register(u0);RWTexture2D<float4> OutMotion:register(u1);
cbuffer Params:register(b0){uint2 DepthBase;uint2 DepthSize;uint2 MotionBase;uint2 MotionSize;uint2 Size;
 float2 Scale;float2 Jitter;float2 Pad;float4 Inset;};
[numthreads(8,8,1)]void Snapshot(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=Size))return;float2 uv=(float2(id.xy)+.5)/Size;
 OutDepth[id.xy]=Depth.Load(int3(DepthBase+min(uint2(uv*DepthSize),DepthSize-1),0));
 float2 mv=Motion.Load(int3(MotionBase+min(uint2(uv*MotionSize),MotionSize-1),0));
 OutMotion[id.xy]=float4(all(isfinite(mv))&&all(abs(mv)<65504)?mv*Scale-Jitter:float2(65504,65504),0,0);
}
[numthreads(8,8,1)]void Atlas(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=Size))return;float2 uv=(float2(id.xy)+.5)/Size;float2 local=(uv-Inset.xy)/Inset.zw;
 float depth;float2 mv;
 if(all(local>=0)&&all(local<1)){
  uint w,h;CenterDepth.GetDimensions(w,h);depth=CenterDepth.Load(int3(min(uint2(local*uint2(w,h)),uint2(w,h)-1),0));
  CenterMotion.GetDimensions(w,h);mv=CenterMotion.Load(int3(min(uint2(local*uint2(w,h)),uint2(w,h)-1),0));
  if(all(abs(mv)<65504))mv*=Inset.zw;
 }else{uint w,h;Depth.GetDimensions(w,h);depth=Depth.Load(int3(min(uint2(uv*uint2(w,h)),uint2(w,h)-1),0));
  Motion.GetDimensions(w,h);mv=Motion.Load(int3(min(uint2(uv*uint2(w,h)),uint2(w,h)-1),0));}
 OutDepth[id.xy]=depth;OutMotion[id.xy]=float4(mv,0,0);
})";
    bool prepare(){
        if(snapshot_shader&&atlas_shader&&state&&constants)return true;
        if(!state){Ptr<ID3D11Device1> device1;const auto level=device->GetFeatureLevel();
            if(FAILED(context.As(&context1))||FAILED(device.As(&device1))||
               FAILED(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&state)))return false;}
        for(unsigned i=0;i<2;++i){auto& shader=i?atlas_shader:snapshot_shader;if(shader)continue;Ptr<ID3DBlob> code;
            if(FAILED(D3DCompile(shaders,sizeof(shaders)-1,nullptr,nullptr,nullptr,i?"Atlas":"Snapshot","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,nullptr))||
                FAILED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader)))return false;}
        if(!constants){D3D11_BUFFER_DESC b{};b.ByteWidth=sizeof(Constants);b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            if(FAILED(device->CreateBuffer(&b,nullptr,&constants)))return false;}
        return true;
    }
    bool allocate(Pair11& p,unsigned w,unsigned h,bool shared=false,ID3D12Device* d12=nullptr,Ptr<ID3D12Resource>* depth12=nullptr,Ptr<ID3D12Resource>* motion12=nullptr){
        if(p.depth&&p.width==w&&p.height==h&&(!shared||(depth12&&*depth12&&motion12&&*motion12)))return true;
        p={};if(depth12)depth12->Reset();if(motion12)motion12->Reset();
        D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=1;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        if(shared)d.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
        for(unsigned i=0;i<2;++i){auto& texture=i?p.motion:p.depth;auto& srv=i?p.motion_srv:p.depth_srv;auto& uav=i?p.motion_uav:p.depth_uav;
            // RG32 sharing is rejected by the DCS adapter. Preserve both
            // motion components at float32 precision in a shareable RGBA32.
            d.Format=i?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_R32_FLOAT;
            if(FAILED(device->CreateTexture2D(&d,nullptr,&texture))||FAILED(device->CreateShaderResourceView(texture.Get(),nullptr,&srv))||
                FAILED(device->CreateUnorderedAccessView(texture.Get(),nullptr,&uav)))return false;
            if(shared){Ptr<IDXGIResource1> resource;HANDLE handle{};
                if(!d12||FAILED(texture.As(&resource))||FAILED(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle)))return false;
                const auto hr=d12->OpenSharedHandle(handle,IID_PPV_ARGS(i?motion12->ReleaseAndGetAddressOf():depth12->ReleaseAndGetAddressOf()));CloseHandle(handle);
                if(FAILED(hr))return false;}
        }
        p.width=w;p.height=h;return true;
    }
    // Caller holds a StateScope.
    bool record(ID3D11ComputeShader* shader,const Constants& c,const std::array<ID3D11ShaderResourceView*,4>& srvs,Pair11& target){
        auto* commands=context.Get();
        return guide_timings.measure(device.Get(),commands,interval,6,[&]{
        commands->UpdateSubresource(constants.Get(),0,nullptr,&c,0,0);auto* cb=constants.Get();
        commands->CSSetShader(shader,nullptr,0);commands->CSSetConstantBuffers(0,1,&cb);commands->CSSetShaderResources(0,4,srvs.data());
        ID3D11UnorderedAccessView* uavs[]{target.depth_uav.Get(),target.motion_uav.Get()};commands->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
        commands->Dispatch((c.size[0]+7)/8,(c.size[1]+7)/8,1);
        ID3D11ShaderResourceView* empty[4]{};ID3D11UnorderedAccessView* empty_uav[2]{};
        commands->CSSetShaderResources(0,4,empty);commands->CSSetUnorderedAccessViews(0,2,empty_uav,nullptr);
        return true;});
    }
    bool execute(){return SUCCEEDED(device->GetDeviceRemovedReason());}
public:
    struct NativeResult {
        std::array<Ptr<ID3D11Texture2D>,2> depth,motion;
        std::array<Ptr<ID3D11ShaderResourceView>,2> depth_srv,motion_srv;
        std::array<CheekyFgEye,2> meta;
        bool valid{};
    };
    NativeResult build_native(const CheekyQuadViewsV1& roles,const XrCompositionLayerProjection& projection,
        const std::array<std::array<float,4>,2>& insets) {
        NativeResult result;if(!enabled||!protection||roles.status!=2||!roles.active)return result;
        Guard guard(protection.Get());std::lock_guard lock(mutex);
        std::array<Source*,4> selected{};
        for(unsigned i=0;i<4;++i){const auto& role=roles.views[i];
            for(auto& source:sources)if(source.id==role.dlss_view&&source.generation==role.registration_generation)selected[i]=&source;
            const auto* source=selected[i];
            if(!source||source->interval!=interval||source->evaluations!=1||
                source->metadata.meta.width!=unsigned(projection.views[i].subImage.imageRect.extent.width)||
                source->metadata.meta.height!=unsigned(projection.views[i].subImage.imageRect.extent.height))return result;
            if(i&&(source->metadata.meta.configuration!=selected[0]->metadata.meta.configuration||
                source->metadata.meta.depth_inverted!=selected[0]->metadata.meta.depth_inverted))return result;
        }
        auto configuration=selected[0]->metadata.meta.configuration^roles.revision;
        std::array<Pair11,2> target;
        for(unsigned eye=0;eye<2;++eye){const auto& inset=insets[eye];
            for(float value:inset)if(!std::isfinite(value))return result;
            if(inset[2]<=0||inset[3]<=0||inset[0]<0||inset[1]<0||inset[0]+inset[2]>1.001f||inset[1]+inset[3]>1.001f)return result;
            const unsigned w=std::max(selected[eye]->image.width,unsigned(std::ceil(selected[eye+2]->image.width/inset[2]))),
                h=std::max(selected[eye]->image.height,unsigned(std::ceil(selected[eye+2]->image.height/inset[3])));
            if(w>16384||h>16384||!allocate(target[eye],w,h))return result;
            configuration=(configuration^w^(uint64_t(h)<<32))*1099511628211ull;
            for(float value:inset)configuration=(configuration^std::bit_cast<unsigned>(value))*1099511628211ull;
            for(unsigned part:{eye,eye+2})configuration=(configuration^selected[part]->id^selected[part]->generation)*1099511628211ull;
        }
        const bool reset=configuration!=last_configuration||last_built_interval+1!=interval;
        if(!prepare())return result;
        {StateScope scope{*this};
        for(unsigned eye=0;eye<2;++eye){const auto& outer=selected[eye]->image;const auto& center=selected[eye+2]->image;
            Constants c;c.size[0]=target[eye].width;c.size[1]=target[eye].height;std::copy(insets[eye].begin(),insets[eye].end(),c.inset);
            record(atlas_shader.Get(),c,{outer.depth_srv.Get(),outer.motion_srv.Get(),center.depth_srv.Get(),center.motion_srv.Get()},target[eye]);
            auto& g=result.meta[eye];g=selected[eye+2]->metadata.meta;g.depth=g.motion=g.peripheral_depth=g.peripheral_motion=nullptr;
            g.focus_width=insets[eye][2];g.focus_height=insets[eye][3];
            g.focus_x=insets[eye][0]+g.focus_width*.5f-.5f;g.focus_y=insets[eye][1]+g.focus_height*.5f-.5f;
            g.focus_feather=2.f/std::min(target[eye].width,target[eye].height);g.configuration=configuration;
            g.reset=reset||selected[eye]->metadata.meta.reset||selected[eye+2]->metadata.meta.reset;
            result.depth[eye]=target[eye].depth;result.motion[eye]=target[eye].motion;
            result.depth_srv[eye]=target[eye].depth_srv;result.motion_srv[eye]=target[eye].motion_srv;
        }}
        if(!execute())return {};
        last_configuration=configuration;last_built_interval=interval;result.valid=true;atlas_ready=true;++diagnostics.guide_pairs;return result;
    }
    struct Result {CheekyFgPair pair;Ptr<IUnknown> lease;};
    explicit QuadFg11(ID3D11Device* d):device(d){device->GetImmediateContext(&context);context.As(&protection);
        if(protection)protection->SetMultithreadProtected(TRUE);}
    void enable(bool e){if(enabled.exchange(e)!=e)guide_timings.reset();if(!e)atlas_ready=false;}
    bool ready()const noexcept{return enabled&&atlas_ready;}
    cheeky::gpu::Summary guide_cost() const {return guide_timings.summary();}
    CheekyQuadFgStatsV1 stats(){std::lock_guard lock(mutex);auto value=diagnostics;value.enabled=enabled;return value;}
    void finish(){if(!protection)return;Guard guard(protection.Get());std::lock_guard lock(mutex);guide_timings.collect(context.Get());++interval;
        if(!enabled){sources={};frames={};last_configuration=last_built_interval=0;}}
    bool observe(const CheekyQuadFgSourceV1& input){
        if(!enabled||!protection||input.context!=context.Get()||!input.meta.feature||!input.generation||!input.depth||!input.motion)return false;
        Guard guard(protection.Get());std::lock_guard lock(mutex);++diagnostics.observations;diagnostics.capture_stage=1;if(!prepare())return false;
        diagnostics.capture_stage=2;
        auto* depth=static_cast<ID3D11Texture2D*>(input.depth);auto* motion=static_cast<ID3D11Texture2D*>(input.motion);
        D3D11_TEXTURE2D_DESC dd{},md{};depth->GetDesc(&dd);motion->GetDesc(&md);
        if(dd.ArraySize!=1||md.ArraySize!=1||dd.SampleDesc.Count!=1||md.SampleDesc.Count!=1)return false;
        const auto& g=input.meta;
        diagnostics.capture_stage=3;
        const auto bounds=[](unsigned base,unsigned size,unsigned total){return size&&uint64_t(base)+size<=total;};
        if(!bounds(g.depth_base_x,g.depth_width,dd.Width)||!bounds(g.depth_base_y,g.depth_height,dd.Height)||
            !bounds(g.motion_base_x,g.motion_width,md.Width)||!bounds(g.motion_base_y,g.motion_height,md.Height))return false;
        diagnostics.capture_stage=4;const auto df=cheeky::fg::guide_depth_format(dd.Format),mf=cheeky::fg::guide_motion_format(md.Format);
        if(df==DXGI_FORMAT_UNKNOWN||mf==DXGI_FORMAT_UNKNOWN)return false;
        auto source=std::find_if(sources.begin(),sources.end(),[&](const auto& s){return s.id==g.feature;});
        if(source==sources.end())source=std::find_if(sources.begin(),sources.end(),[](const auto& s){return !s.id;});
        if(source==sources.end())source=std::min_element(sources.begin(),sources.end(),[](const auto& a,const auto& b){return a.interval<b.interval;});
        bool reset=g.reset||source->id!=g.feature||source->generation!=input.generation||source->interval+1!=interval;
        diagnostics.capture_stage=5;if(!allocate(source->image,g.depth_width,g.depth_height))return false;
        diagnostics.capture_stage=6;
        Ptr<ID3D11ShaderResourceView> ds,ms;D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=1;
        view.Format=df;if(FAILED(device->CreateShaderResourceView(depth,&view,&ds)))return false;
        view.Format=mf;if(FAILED(device->CreateShaderResourceView(motion,&view,&ms)))return false;
        Constants c;c.depth_base[0]=g.depth_base_x;c.depth_base[1]=g.depth_base_y;c.depth_size[0]=g.depth_width;c.depth_size[1]=g.depth_height;
        c.motion_base[0]=g.motion_base_x;c.motion_base[1]=g.motion_base_y;c.motion_size[0]=g.motion_width;c.motion_size[1]=g.motion_height;
        c.size[0]=g.depth_width;c.size[1]=g.depth_height;c.scale[0]=g.motion_scale_x/g.motion_width;c.scale[1]=g.motion_scale_y/g.motion_height;
        if(g.motion_jittered){if(!reset&&source->metadata.meta.motion_jittered){
            c.jitter[0]=(source->metadata.meta.jitter_x-g.jitter_x)/g.motion_width;c.jitter[1]=(source->metadata.meta.jitter_y-g.jitter_y)/g.motion_height;
        }else reset=true;}
        diagnostics.capture_stage=7;{StateScope scope{*this};record(snapshot_shader.Get(),c,{ds.Get(),ms.Get(),nullptr,nullptr},source->image);}
        if(!execute())return false;
        source->evaluations=source->interval==interval?source->evaluations+1:1;
        source->id=g.feature;source->generation=input.generation;source->interval=interval;source->metadata=input;
        source->metadata.meta.reset=reset;diagnostics.capture_stage=8;++diagnostics.snapshots;return true;
    }
    Result build(const CheekyQuadViewsV1& roles,const XrCompositionLayerProjection& projection,
        const std::array<unsigned,2>& widths,const std::array<unsigned,2>& heights,
        const std::array<std::array<float,4>,2>& insets,cheeky::fg::D3D11Bridge& bridge){
        Result result;atlas_ready=false;if(!enabled||!protection||roles.status!=2||!roles.active)return result;
        // Bridge methods take their mutex before entering the immediate context.
        // Bridge upload waits retire outside this mutex. Brief CPU contention
        // must not discard a valid four-view interval and reset FG history.
        auto dispatch=bridge.lock_dispatch();std::unique_lock bridge_lock(bridge.mutex);
        Guard guard(protection.Get());std::lock_guard lock(mutex);
        ++diagnostics.frames;diagnostics.assembly_stage=1;
        std::array<Source*,4> selected{};
        for(unsigned i=0;i<4;++i){const auto& role=roles.views[i];for(auto& s:sources)if(s.id==role.dlss_view&&s.generation==role.registration_generation)selected[i]=&s;
            const auto* s=selected[i];if(!s||s->interval!=interval||s->evaluations!=1||s->metadata.meta.width!=unsigned(projection.views[i].subImage.imageRect.extent.width)||
                s->metadata.meta.height!=unsigned(projection.views[i].subImage.imageRect.extent.height))return result;
            if(i&&(s->metadata.meta.configuration!=selected[0]->metadata.meta.configuration||s->metadata.meta.depth_inverted!=selected[0]->metadata.meta.depth_inverted))return result;}
        std::shared_ptr<Frame> frame;
        for(auto& slot:frames){if(!slot)slot=std::make_shared<Frame>();if(slot.use_count()==1){frame=slot;break;}}
        if(!frame)return result;
        diagnostics.assembly_stage=2;
        auto configuration=selected[0]->metadata.meta.configuration^roles.revision;
        for(unsigned eye=0;eye<2;++eye){const auto& inset=insets[eye];
            for(const auto f:inset)if(!std::isfinite(f))return result;
            if(inset[2]<=0||inset[3]<=0||inset[0]<0||inset[1]<0||inset[0]+inset[2]>1.001f||inset[1]+inset[3]>1.001f)return result;
            const unsigned w=std::max(selected[eye]->image.width,unsigned(std::ceil(selected[eye+2]->image.width/inset[2]))),
                h=std::max(selected[eye]->image.height,unsigned(std::ceil(selected[eye+2]->image.height/inset[3])));
            if(!w||!h||w>16384||h>16384||!allocate(frame->image[eye],w,h,true,bridge.device12.Get(),&frame->depth[eye],&frame->motion[eye]))return result;
            configuration=(configuration^w^uint64_t(h)<<32)*1099511628211ull;
            for(const auto f:inset)configuration=(configuration^std::bit_cast<unsigned>(f))*1099511628211ull;
        }
        bool reset=configuration!=last_configuration||last_built_interval+1!=interval;
        if(!prepare())return result;
        {StateScope scope{*this};
        for(unsigned eye=0;eye<2;++eye){auto& target=frame->image[eye];const auto& outer=selected[eye]->image;const auto& center=selected[eye+2]->image;
            Constants c;c.size[0]=target.width;c.size[1]=target.height;std::copy(insets[eye].begin(),insets[eye].end(),c.inset);
            record(atlas_shader.Get(),c,{outer.depth_srv.Get(),outer.motion_srv.Get(),center.depth_srv.Get(),center.motion_srv.Get()},target);
            auto& g=result.pair.eyes[eye];g=selected[eye+2]->metadata.meta;g.depth=frame->depth[eye].Get();g.motion=frame->motion[eye].Get();
            g.width=widths[eye];g.height=heights[eye];g.depth_width=g.motion_width=target.width;g.depth_height=g.motion_height=target.height;
            g.depth_base_x=g.depth_base_y=g.motion_base_x=g.motion_base_y=0;g.motion_scale_x=float(target.width);g.motion_scale_y=float(target.height);
            g.jitter_x=g.jitter_y=g.jitter_correction_x=g.jitter_correction_y=0;g.motion_jittered=0;g.prepared_guides=0;
            g.focus_width=insets[eye][2];g.focus_height=insets[eye][3];g.focus_x=insets[eye][0]+g.focus_width*.5f-.5f;g.focus_y=insets[eye][1]+g.focus_height*.5f-.5f;
            g.focus_feather=2.f/std::min(widths[eye],heights[eye]);
            g.peripheral_method=std::min(g.peripheral_method,2u);g.peripheral_depth=g.peripheral_motion=nullptr;
            g.reset=reset||selected[eye]->metadata.meta.reset||selected[eye+2]->metadata.meta.reset;g.configuration=configuration;
        }}
        diagnostics.assembly_stage=3;if(!execute()||!bridge.from11())return {};
        last_configuration=configuration;last_built_interval=interval;const auto token=(++serial)*2;
        auto& pair=result.pair;pair.capture_token=token;pair.render_frame=serial;pair.producer_queue=bridge.queue.Get();
        const auto& options=selected[0]->metadata;pair.fg_queue_priority=options.queue_priority;pair.release_point=options.release_point;
        for(unsigned i=0;i<2;++i){pair.eyes[i].render_frame=serial;pair.eyes[i].evaluation=token-1+i;}
        result.lease.Attach(new Lease(std::move(frame)));pair.lease=result.lease.Get();diagnostics.assembly_stage=4;++diagnostics.guide_pairs;atlas_ready=true;return result;
    }
};
}
