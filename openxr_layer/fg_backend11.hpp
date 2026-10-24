#pragma once
#include "fg_transport.hpp"
#include "fg_matrices.hpp"
#include "fg_timing11.hpp"
#include "fg_optical.hpp"
#include "../shared/cheeky_fg_shader11.hpp"
#include "../shared/fg_interpolation_shader.hpp"
#include "../shared/fg_guide_layout.hpp"
#include <d3dcompiler.h>
#include <cmath>
#include <mutex>

namespace cheeky::fg {
// Native D3D11 GPU stages behind BufferedPresenter's pacing: color capture,
// guide snapshots, shader interpolation and copies into private OpenXR
// swapchains. No D3D12 device, bridge or NGX. Commands are issued directly on
// the application's immediate context under ID3D11Multithread (plus the quad
// NGX dispatch mutex when set). Shader work uses a private pipeline state;
// texture copies leave the game's pipeline bindings untouched. No deferred contexts or command lists:
// the NVIDIA driver crashed on its worker thread with quad views + FG, and the
// FG-only D3D11 work was the part recorded on deferred contexts.
// One immediate context orders every stage, so no fences are needed: a frame
// is "ready" as soon as its commands are queued.
struct Backend11 {
    optical::Session11 optical[2][2];
    std::array<float,4> optical_quad_regions[2]; // Latched by quad capture before this frame's metadata.
    struct Guides {
        ComPtr<ID3D11Texture2D> depth,motion;
        ComPtr<ID3D11ShaderResourceView> depth_srv,motion_srv;
        CheekyFgShaderSource11V1 source; // Metadata only; borrowed pointers are cleared.
        unsigned observations{};
    };
    struct Constants {
        float rect[4]{},offset[4]{},dimensions[4]{},motion[4]{},options[4]{},jitter[4]{},
            atlas_crop[4]{},atlas_spans[4]{},atlas_texel[4]{},prior_crop[4]{},prior_spans[4]{};
        Matrix previous{identity()};
    };
    struct SnapshotConstants {
        unsigned depth_base[2]{},depth_size[2]{},motion_base[2]{},motion_size[2]{},size[2]{};
        float scale[2]{},jitter[2]{},flip[2]{},crop[4]{};
    };
    static constexpr char snapshot_hlsl[]=R"(
Texture2D<float> Depth:register(t0);Texture2D<float2> Motion:register(t1);
RWTexture2D<float> OutDepth:register(u0);RWTexture2D<float2> OutMotion:register(u1);
cbuffer Params:register(b0){uint2 DepthBase;uint2 DepthSize;uint2 MotionBase;uint2 MotionSize;uint2 Size;
 float2 Scale;float2 Jitter;float2 Flip;float4 Crop;};
[numthreads(8,8,1)] void Main(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=Size))return;float2 uv=(float2(id.xy)+.5)/Size;
 if(Flip.x>.5)uv.y=1-uv.y;uv=Crop.xy+uv*Crop.zw;
 OutDepth[id.xy]=Depth.Load(int3(DepthBase+min(uint2(uv*DepthSize),DepthSize-1),0));
 float2 mv=Motion.Load(int3(MotionBase+min(uint2(uv*MotionSize),MotionSize-1),0));
 bool valid=all(isfinite(mv))&&all(abs(mv)<65504);
 mv=(mv*Scale-Jitter)/Crop.zw;if(Flip.x>.5)mv.y=-mv.y;
 OutMotion[id.xy]=valid?mv:float2(65504,65504);
})";
    struct Guard {ID3D11Multithread* p;explicit Guard(ID3D11Multithread* value):p(value){p->Enter();}~Guard(){p->Leave();}};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3DDeviceContextState> state; // Our own pipeline state on the immediate context.
    ComPtr<ID3D11Multithread> protection;
    std::recursive_mutex* dispatch{}; // Shared with the game's NGX evaluation in stereo and quad modes.
    ShaderTimings11 gpu_timings;
    ShaderTimings11 optical_timings;
    StageTimings11 capture_timings,guide_timings,upload_timings;
    std::atomic<std::uint64_t> guide_interval{1};
    ComPtr<ID3D11ComputeShader> snapshot;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11SamplerState> sampler;
    std::uint64_t session_generation{};
    std::mutex pool_mutex;
    std::vector<Image> pool;
    std::mutex guide_mutex;
    std::array<std::shared_ptr<Guides>,2> pending_guides{},last_guides{};
    char diagnostic[384]{"Native D3D11 shader FG: waiting for stereo guides"};

    bool initialize(ID3D11Device* dev,std::uint64_t generation,std::recursive_mutex* serialize) noexcept {
        if(!dev||dev->GetFeatureLevel()<D3D_FEATURE_LEVEL_11_0)return false;
        device=dev;session_generation=generation;dispatch=serialize;dev->GetImmediateContext(&context);
        if(FAILED(context.As(&protection)))return false;
        protection->SetMultithreadProtected(TRUE);
        ComPtr<ID3D11Device1> device1;const auto level=dev->GetFeatureLevel();
        if(FAILED(context.As(&context1))||FAILED(dev->QueryInterface(IID_PPV_ARGS(&device1)))||
           FAILED(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&state)))return false;
        ComPtr<ID3DBlob> code,error;
        auto compile=[&](const char* source,const char* entry,const char* profile){
            code.Reset();error.Reset();
            if(SUCCEEDED(D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error)))return true;
            if(error)strncpy_s(diagnostic,static_cast<const char*>(error->GetBufferPointer()),_TRUNCATE);return false;};
        if(!compile(snapshot_hlsl,"Main","cs_5_0")||FAILED(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&snapshot)))return false;
        if(!compile(interpolation_shader,"RegionsVS","vs_5_0")||FAILED(dev->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vertex)))return false;
        if(!compile(interpolation_shader,"RegionsPS","ps_5_0")||FAILED(dev->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&pixel)))return false;
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
        if(FAILED(dev->CreateRasterizerState(&rd,&rasterizer)))return false;
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        return SUCCEEDED(dev->CreateSamplerState(&sd,&sampler));
    }
    // Copies do not change pipeline bindings. In particular, the presentation
    // thread must not switch the game's entire context state just to upload
    // a texture. Keep the dispatch and immediate-context protection for all work.
    template<class Record> bool submit(Record&& record,bool flush) {
        std::unique_lock<std::recursive_mutex> serialized;if(dispatch)serialized=std::unique_lock(*dispatch);
        Guard guard(protection.Get());
        gpu_timings.collect(context.Get());
        optical_timings.collect(context.Get());
        capture_timings.collect(context.Get());guide_timings.collect(context.Get());upload_timings.collect(context.Get());
        const bool recorded=record(context.Get());
        if(flush)context->Flush();
        return recorded&&SUCCEEDED(device->GetDeviceRemovedReason());
    }
    // Only draws/dispatches need private shader state. Restore before submit()
    // flushes, including when recording exits by exception.
    template<class Record> bool submit_shader(Record&& record,bool flush) {
        return submit([&](ID3D11DeviceContext* commands){
            struct StateScope {
                Backend11& owner;ComPtr<ID3DDeviceContextState> previous;
                explicit StateScope(Backend11& value):owner(value){owner.context1->SwapDeviceContextState(owner.state.Get(),&previous);}
                ~StateScope(){owner.context1->SwapDeviceContextState(previous.Get(),nullptr);}
            } scope{*this};
            return record(commands);
        },flush);
    }
    // Runtime calls made from presenter threads. A D3D11 runtime may queue work
    // (e.g. a fence signal) on the application's immediate context during
    // xrReleaseSwapchainImage/xrEndFrame and rely on the application to flush
    // it. Our threads are not the application, so flush here: otherwise the
    // runtime's next xrWaitFrame can wait forever once the game stops
    // submitting (disabling frame generation hung DCS on Virtual Desktop).
    template<class Submit> auto runtime_submit(Submit&& submit) {
        std::unique_lock<std::recursive_mutex> serialized;if(dispatch)serialized=std::unique_lock(*dispatch);
        auto result=submit();flush();return result;
    }
    void flush() noexcept {if(context&&protection){Guard guard(protection.Get());context->Flush();}}
    void cancel_gpu_timing() {submit([&](ID3D11DeviceContext* commands){gpu_timings.cancel(commands);return true;},false);}
    // The single immediate context retains every referenced texture until its
    // queued GPU work finishes, so an image is reusable once only the pool owns it.
    Image image(const D3D11_TEXTURE2D_DESC& wanted,bool srv) {
        static std::atomic<std::uint64_t> next_identity{};
        std::lock_guard lock(pool_mutex);
        for(auto& p:pool)if(p.use_count()==1){D3D11_TEXTURE2D_DESC d{};p->texture11->GetDesc(&d);
            if(d.Width==wanted.Width&&d.Height==wanted.Height&&d.ArraySize==wanted.ArraySize&&d.Format==wanted.Format&&
               d.BindFlags==wanted.BindFlags&&d.MipLevels==wanted.MipLevels){
                p->presentation_format=DXGI_FORMAT_UNKNOWN;p->layout={};p->fg_border={};p->identity=++next_identity;return p;}}
        if(pool.size()>=48){const auto free=std::find_if(pool.begin(),pool.end(),[](const Image& p){return p.use_count()==1;});
            if(free==pool.end())return {};pool.erase(free);}
        auto out=std::make_shared<TransportImage>();
        if(FAILED(device->CreateTexture2D(&wanted,nullptr,&out->texture11)))return {};
        if(srv){D3D11_SHADER_RESOURCE_VIEW_DESC v{};v.Format=wanted.Format;v.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;v.Texture2D.MipLevels=1;
            if(FAILED(device->CreateShaderResourceView(out->texture11.Get(),&v,&out->srv11)))return {};}
        out->identity=++next_identity;pool.push_back(out);return out;
    }
    // Full private copy of a released application/compositor image.
    Image copy(ID3D11Texture2D* source,DXGI_FORMAT declared) {
        if(!source)return {};D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
        const auto format=capture_format(d.Format,declared);if(format==DXGI_FORMAT_UNKNOWN||d.SampleDesc.Count!=1)return {};
        // Single images are sampleable, so a whole-image eye needs no crop copy.
        const bool single=d.ArraySize==1&&d.MipLevels==1;
        d.Format=format;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=single?D3D11_BIND_SHADER_RESOURCE:0;d.CPUAccessFlags=0;d.MiscFlags=0;
        auto out=image(d,single);if(!out)return {};
        return submit([&](ID3D11DeviceContext* c){return capture_timings.measure(device.Get(),c,0,1,[&]{
            c->CopyResource(out->texture11.Get(),source);return true;});},false)?out:Image{};
    }
    // One eye's subimage as an independent, sampleable texture.
    Image crop(const Image& full,const XrSwapchainSubImage& sub,bool reuse_whole=false) {
        if(!full||!full->texture11)return {};D3D11_TEXTURE2D_DESC d{};full->texture11->GetDesc(&d);const auto& r=sub.imageRect;
        if(r.offset.x<0||r.offset.y<0||r.extent.width<=0||r.extent.height<=0||sub.imageArrayIndex>=d.ArraySize||
            std::uint64_t(r.offset.x)+r.extent.width>d.Width||std::uint64_t(r.offset.y)+r.extent.height>d.Height)return {};
        // Per-eye swapchains and composed quad eyes: the capture already is the eye.
        if(reuse_whole&&full->srv11&&d.ArraySize==1&&d.MipLevels==1&&r.offset.x==0&&r.offset.y==0&&
            unsigned(r.extent.width)==d.Width&&unsigned(r.extent.height)==d.Height)return full;
        D3D11_TEXTURE2D_DESC e{};e.Width=unsigned(r.extent.width);e.Height=unsigned(r.extent.height);e.MipLevels=e.ArraySize=1;
        e.Format=d.Format;e.SampleDesc.Count=1;e.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        auto out=image(e,true);if(!out)return {};
        D3D11_BOX box{unsigned(r.offset.x),unsigned(r.offset.y),0,unsigned(r.offset.x+r.extent.width),unsigned(r.offset.y+r.extent.height),1};
        return submit([&](ID3D11DeviceContext* c){return capture_timings.measure(device.Get(),c,0,1,[&]{
            c->CopySubresourceRegion(out->texture11.Get(),0,0,0,0,full->texture11.Get(),
            D3D11CalcSubresource(0,sub.imageArrayIndex,d.MipLevels),&box);return true;});},false)?out:Image{};
    }
    // Called from the outer game NGX evaluation, before it can reuse its guides.
    bool observe(const CheekyFgShaderSource11V1& input) {
        if(input.eye>1||input.context!=context.Get()||input.session_generation!=session_generation||!input.generation||
            !input.mapping_revision||!input.depth||!input.motion||!input.meta.feature)return false;
        if(!cheeky_fg_shader_only(input.meta.center_method,input.meta.peripheral_method,
            input.meta.focus_width<.999f||input.meta.focus_height<.999f))return false;
        std::shared_ptr<Guides> old;
        {std::lock_guard lock(guide_mutex);auto& slot=pending_guides[input.eye];if(slot){++slot->observations;return false;}old=last_guides[input.eye];}
        auto next=std::make_shared<Guides>();next->source=input;next->observations=1;
        auto& g=next->source.meta;D3D11_TEXTURE2D_DESC dd{},md{};input.depth->GetDesc(&dd);input.motion->GetDesc(&md);
        if(dd.ArraySize!=1||md.ArraySize!=1||dd.SampleDesc.Count!=1||md.SampleDesc.Count!=1)return false;
        auto bounds=[](unsigned base,unsigned size,unsigned total){return size&&std::uint64_t(base)+size<=total;};
        if(!bounds(g.depth_base_x,g.depth_width,dd.Width)||!bounds(g.depth_base_y,g.depth_height,dd.Height)||
            !bounds(g.motion_base_x,g.motion_width,md.Width)||!bounds(g.motion_base_y,g.motion_height,md.Height))return false;
        for(float v:input.crop)if(!std::isfinite(v))return false;
        if(input.crop[0]<0||input.crop[1]<0||input.crop[2]<=0||input.crop[3]<=0||
            input.crop[0]+input.crop[2]>1.0001f||input.crop[1]+input.crop[3]>1.0001f)return false;
        const auto df=guide_depth_format(dd.Format),mf=guide_motion_format(md.Format);
        if(df==DXGI_FORMAT_UNKNOWN||mf==DXGI_FORMAT_UNKNOWN)return false;
        const bool same=old&&old->source.meta.feature==g.feature&&old->source.generation==input.generation&&
            old->source.mapping_revision==input.mapping_revision&&old->source.meta.configuration==g.configuration&&
            old->source.meta.depth_width==g.depth_width&&old->source.meta.depth_height==g.depth_height&&
            old->source.meta.motion_width==g.motion_width&&old->source.meta.motion_height==g.motion_height&&
            old->source.meta.motion_jittered==g.motion_jittered&&
            old->source.vertical_flip==input.vertical_flip&&std::equal(std::begin(input.crop),std::end(input.crop),old->source.crop)&&
            old->source.meta.depth_inverted==g.depth_inverted;
        g.reset=g.reset||!same;
        SnapshotConstants c;c.depth_base[0]=g.depth_base_x;c.depth_base[1]=g.depth_base_y;
        c.depth_size[0]=g.depth_width;c.depth_size[1]=g.depth_height;
        c.motion_base[0]=g.motion_base_x;c.motion_base[1]=g.motion_base_y;c.motion_size[0]=g.motion_width;c.motion_size[1]=g.motion_height;
        c.size[0]=std::max(1u,unsigned(std::round(g.depth_width*input.crop[2])));
        c.size[1]=std::max(1u,unsigned(std::round(g.depth_height*input.crop[3])));
        c.scale[0]=g.motion_scale_x/g.motion_width;c.scale[1]=g.motion_scale_y/g.motion_height;
        for(float scale:c.scale)if(!std::isfinite(scale))return false;
        if(g.motion_jittered){if(same&&old->source.meta.motion_jittered){
            c.jitter[0]=(old->source.meta.jitter_x-g.jitter_x)/g.motion_width;
            c.jitter[1]=(old->source.meta.jitter_y-g.jitter_y)/g.motion_height;
        }else g.reset=1;}
        c.flip[0]=float(input.vertical_flip);std::copy(std::begin(input.crop),std::end(input.crop),c.crop);
        auto writable=[&](DXGI_FORMAT format,ComPtr<ID3D11Texture2D>& texture,ComPtr<ID3D11ShaderResourceView>& srv,ComPtr<ID3D11UnorderedAccessView>& uav){
            D3D11_TEXTURE2D_DESC d{};d.Width=c.size[0];d.Height=c.size[1];d.MipLevels=d.ArraySize=1;d.Format=format;d.SampleDesc.Count=1;
            d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
            return SUCCEEDED(device->CreateTexture2D(&d,nullptr,&texture))&&SUCCEEDED(device->CreateShaderResourceView(texture.Get(),nullptr,&srv))&&
                SUCCEEDED(device->CreateUnorderedAccessView(texture.Get(),nullptr,&uav));};
        ComPtr<ID3D11UnorderedAccessView> depth_uav,motion_uav;
        if(!writable(DXGI_FORMAT_R32_FLOAT,next->depth,next->depth_srv,depth_uav)||
            !writable(DXGI_FORMAT_R32G32_FLOAT,next->motion,next->motion_srv,motion_uav))return false;
        // Depth textures need not have been created with SHADER_RESOURCE binding.
        // Copy them into private sampleable allocations before normalization.
        auto readable=[&](ID3D11Texture2D* original,D3D11_TEXTURE2D_DESC d,DXGI_FORMAT format,ComPtr<ID3D11Texture2D>& texture,ComPtr<ID3D11ShaderResourceView>& srv){
            d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.MiscFlags=0;d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=0;
            if(d.Format==DXGI_FORMAT_D32_FLOAT)d.Format=DXGI_FORMAT_R32_TYPELESS;
            if(d.Format==DXGI_FORMAT_D16_UNORM)d.Format=DXGI_FORMAT_R16_TYPELESS;
            if(d.Format==DXGI_FORMAT_D24_UNORM_S8_UINT)d.Format=DXGI_FORMAT_R24G8_TYPELESS;
            if(d.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT)d.Format=DXGI_FORMAT_R32G8X24_TYPELESS;
            if(FAILED(device->CreateTexture2D(&d,nullptr,&texture)))return false;
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};v.Format=format;v.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;v.Texture2D.MipLevels=1;
            return SUCCEEDED(device->CreateShaderResourceView(texture.Get(),&v,&srv));};
        ComPtr<ID3D11Texture2D> depth,motion;ComPtr<ID3D11ShaderResourceView> depth_srv,motion_srv;
        if(!readable(input.depth,dd,df,depth,depth_srv)||!readable(input.motion,md,mf,motion,motion_srv))return false;
        ComPtr<ID3D11Buffer> constants;D3D11_BUFFER_DESC b{};b.ByteWidth=sizeof(c);b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{&c};if(FAILED(device->CreateBuffer(&b,&initial,&constants)))return false;
        if(!submit_shader([&](ID3D11DeviceContext* commands){
            return guide_timings.measure(device.Get(),commands,guide_interval.load(),2,[&]{
            commands->CopyResource(depth.Get(),input.depth);commands->CopyResource(motion.Get(),input.motion);
            auto* cb=constants.Get();ID3D11ShaderResourceView* srvs[]{depth_srv.Get(),motion_srv.Get()};
            ID3D11UnorderedAccessView* uavs[]{depth_uav.Get(),motion_uav.Get()};
            commands->CSSetShader(snapshot.Get(),nullptr,0);commands->CSSetConstantBuffers(0,1,&cb);
            commands->CSSetShaderResources(0,2,srvs);commands->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
            commands->Dispatch((c.size[0]+7)/8,(c.size[1]+7)/8,1);
            ID3D11ShaderResourceView* no_srvs[2]{};ID3D11UnorderedAccessView* no_uavs[2]{};
            commands->CSSetShaderResources(0,2,no_srvs);commands->CSSetUnorderedAccessViews(0,2,no_uavs,nullptr);
            return true;});},false))return false;
        // Only immutable snapshots cross from the game evaluation to xrEndFrame.
        next->source.context=nullptr;next->source.depth=next->source.motion=nullptr;
        std::lock_guard lock(guide_mutex);auto& slot=pending_guides[input.eye];if(slot){++slot->observations;return false;}
        slot=std::move(next);return true;
    }
    // Quad views: full-eye guide atlases assembled by QuadFg11::build_native.
    void adopt_quad_guides(unsigned eye,ID3D11Texture2D* depth,ID3D11Texture2D* motion,
        ID3D11ShaderResourceView* depth_srv,ID3D11ShaderResourceView* motion_srv,const CheekyFgEye& meta) {
        if(eye>1)return;auto guide=std::make_shared<Guides>();guide->source.meta=meta;guide->observations=1;
        guide->source.mapping_revision=meta.configuration;
        guide->depth=depth;guide->depth_srv=depth_srv;guide->motion=motion;guide->motion_srv=motion_srv;
        std::lock_guard lock(guide_mutex);pending_guides[eye]=std::move(guide);
    }
    // Claims this interval's stereo guides. Ambiguous intervals claim nothing.
    bool take_guides(std::array<std::shared_ptr<Guides>,2>& out) {
        std::lock_guard lock(guide_mutex);auto taken=std::move(pending_guides);pending_guides={};++guide_interval;
        if(!taken[0]||!taken[1]||taken[0]->observations!=1||taken[1]->observations!=1||
            taken[0]->source.meta.configuration!=taken[1]->source.meta.configuration||
            taken[0]->source.mapping_revision!=taken[1]->source.mapping_revision)return false;
        last_guides=taken;out=std::move(taken);return true;
    }
    void clear_guides() {std::lock_guard lock(guide_mutex);pending_guides={};last_guides={};}
    // One eye's interpolation pass, kept until it is drawn. With direct output
    // the pass renders straight into the acquired OpenXR image at
    // publication, which saves the private texture and its full-eye copy.
    struct DeferredDraw {
        Image previous,current;
        std::shared_ptr<Guides> old_guides,new_guides;
        ComPtr<ID3D11Buffer> constants;
        ComPtr<ID3D11Texture2D> optical_color[2];
        ComPtr<ID3D11ShaderResourceView> optical_view[2];
        D3D11_TEXTURE2D_DESC desc{};
        ShaderTimings11::Ticket timing;unsigned timing_part{};
    };
    void record_draw(ID3D11DeviceContext* commands,const DeferredDraw& draw,ID3D11RenderTargetView* target) {
        auto* cb=draw.constants.Get();auto* ss=sampler.Get();
        ID3D11ShaderResourceView* srvs[]{draw.previous->srv11.Get(),draw.current->srv11.Get(),
            draw.new_guides->motion_srv.Get(),draw.new_guides->depth_srv.Get(),draw.old_guides->depth_srv.Get(),nullptr,
            draw.optical_view[0].Get(),draw.optical_view[1].Get()};
        commands->OMSetDepthStencilState(nullptr,0);
        commands->OMSetBlendState(nullptr,nullptr,~0u);commands->RSSetState(rasterizer.Get());
        commands->IASetInputLayout(nullptr);commands->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->VSSetShader(vertex.Get(),nullptr,0);
        commands->PSSetConstantBuffers(0,1,&cb);commands->PSSetSamplers(0,1,&ss);commands->PSSetShaderResources(0,8,srvs);
        commands->OMSetRenderTargets(1,&target,nullptr);
        const D3D11_VIEWPORT viewport{0,0,float(draw.desc.Width),float(draw.desc.Height),0,1};
        commands->RSSetViewports(1,&viewport);commands->PSSetShader(pixel.Get(),nullptr,0);commands->Draw(3,0);
        ID3D11ShaderResourceView* empty[8]{};commands->PSSetShaderResources(0,8,empty);commands->OMSetRenderTargets(0,nullptr,nullptr);
    }
    static const DeferredDraw* deferred(const Image& image) noexcept {
        return image?static_cast<const DeferredDraw*>(image->pending11.get()):nullptr;
    }
    // Interpolates previous (A) -> current (B) for both eyes. Deferred output
    // only prepares each pass; draw_into() renders it at publication.
    bool generate(const std::array<Image,2>& previous,const std::array<Image,2>& current,
        const std::array<std::shared_ptr<Guides>,2>& old_guides,const std::array<std::shared_ptr<Guides>,2>& new_guides,
        const std::array<XrCompositionLayerProjectionView,2>& previous_views,const std::array<XrCompositionLayerProjectionView,2>& current_views,
        std::array<Image,2>& out,bool deferred_output=false,float fraction=.5f) {
        // Prepare both eyes first; then issue all commands in one submission.
        std::array<std::shared_ptr<DeferredDraw>,2> draws;std::array<ComPtr<ID3D11RenderTargetView>,2> targets;
        const auto& meta=new_guides[0]->source.meta;
        const unsigned focused_eyes=(meta.focus_width<.999f||meta.focus_height<.999f)?2u:0u;
        const bool uses_flow=cheeky_fg_uses_flow(meta.center_method,meta.peripheral_method,focused_eyes!=0);
        ShaderTimings11::Ticket optical_timing;bool optical_measured=false;
        if(uses_flow){D3D11_TEXTURE2D_DESC d{};current[0]->texture11->GetDesc(&d);
            submit([&](ID3D11DeviceContext* c){optical_timing=optical_timings.reserve(device.Get(),1,d.Width,d.Height,focused_eyes,meta.peripheral_method,meta.configuration);
                optical_measured=optical_timings.begin(c,optical_timing,0);return true;},false);}
        struct CancelMeasurement {Backend11& backend;bool armed;~CancelMeasurement(){if(armed)backend.submit([&](ID3D11DeviceContext* c){backend.optical_timings.cancel(c);return true;},false);}} cancel{*this,uses_flow};
        for(unsigned eye=0;eye<2;++eye){
            const auto& now=*new_guides[eye];const auto& g=now.source.meta;
            D3D11_TEXTURE2D_DESC d{};current[eye]->texture11->GetDesc(&d);
            auto cam=camera(current_views[eye],&previous_views[eye],g.depth_inverted!=0);if(!cam.valid)return false;
            Constants c;c.previous=cam.clip_to_prev;c.options[0]=float(g.peripheral_method);c.options[1]=fraction;c.options[2]=g.reset?0.f:1.f;
            c.dimensions[0]=float(d.Width);c.dimensions[1]=float(d.Height);
            c.dimensions[2]=g.resolution_percent*.01f;
            const bool focused=g.focus_width<.999f||g.focus_height<.999f;
            const auto& crop=now.source.crop;
            c.rect[2]=focused?std::clamp(g.focus_width/crop[2],.001f,1.f):1.f;
            c.rect[3]=focused?std::clamp(g.focus_height/crop[3],.001f,1.f):1.f;
            float center_x=(.5f+g.focus_x-crop[0])/crop[2],center_y=(.5f+g.focus_y-crop[1])/crop[3];
            if(now.source.vertical_flip)center_y=1-center_y;
            c.rect[0]=std::clamp(center_x-c.rect[2]*.5f,0.f,1-c.rect[2]);c.rect[1]=std::clamp(center_y-c.rect[3]*.5f,0.f,1-c.rect[3]);
            c.offset[0]=g.focus_feather;c.offset[2]=float(g.show_foveation_border);c.motion[2]=float(g.center_method);c.motion[3]=focused?1.f:0.f;
            auto& draw=draws[eye];draw=std::make_shared<DeferredDraw>();
            draw->previous=previous[eye];draw->current=current[eye];draw->old_guides=old_guides[eye];draw->new_guides=new_guides[eye];
            for(unsigned pass=0;pass<2;++pass){
                if((pass?g.peripheral_method:g.center_method)!=4||(pass&&!focused))continue;
                auto meta=g;meta.width=d.Width;meta.height=d.Height;
                const float full[4]{0,0,1,1};const auto* rect=pass?full:c.rect;
                const auto scale=pass?g.peripheral_percent:g.resolution_percent;
                auto params=optical::constants(meta,pass,rect,previous[eye]->layout,current[eye]->layout,
                    std::max(32u,unsigned(std::round(d.Width*rect[2]*scale*.01f))),
                    std::max(32u,unsigned(std::round(d.Height*rect[3]*scale*.01f))));
                const auto quality=pass?g.optical.peripheral_preset:g.optical.center_preset;
                if(!submit_shader([&](ID3D11DeviceContext* context){
                    return optical[eye][pass].render(context,previous[eye]->srv11.Get(),current[eye]->srv11.Get(),
                        previous[eye]->identity,current[eye]->identity,quality,params,fraction,g.reset!=0,
                        draw->optical_color[pass],draw->optical_view[pass]);},true)){
                    strcpy_s(diagnostic,optical[eye][pass].diagnostic);return false;
                }
            }
            // Render-target output works for every swapchain color format,
            // including BGRA and sRGB, which typed UAVs do not.
            D3D11_TEXTURE2D_DESC e{};e.Width=d.Width;e.Height=d.Height;e.MipLevels=e.ArraySize=1;e.Format=d.Format;e.SampleDesc.Count=1;
            e.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;draw->desc=e;
            D3D11_BUFFER_DESC b{};b.ByteWidth=sizeof(c);b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;D3D11_SUBRESOURCE_DATA initial{&c};
            if(FAILED(device->CreateBuffer(&b,&initial,&draw->constants)))return false;
            if(deferred_output){
                static std::atomic<std::uint64_t> next_identity{1ull<<62};
                out[eye]=std::make_shared<TransportImage>();out[eye]->identity=++next_identity;out[eye]->pending11=draw;
                continue;
            }
            out[eye]=image(e,true);if(!out[eye])return false;
            if(FAILED(device->CreateRenderTargetView(out[eye]->texture11.Get(),nullptr,&targets[eye])))return false;
        }
        if(optical_measured)submit([&](ID3D11DeviceContext* c){optical_timings.end(c,optical_timing,0);return true;},false);
        cancel.armed=false;
        const unsigned focused=(meta.focus_width<.999f||meta.focus_height<.999f)?2u:0u;
        if(deferred_output)return submit([&](ID3D11DeviceContext*){
            auto timing=gpu_timings.reserve(device.Get(),2,draws[0]->desc.Width,draws[0]->desc.Height,focused,meta.peripheral_method,meta.configuration);
            for(unsigned eye=0;eye<2;++eye){draws[eye]->timing=timing;draws[eye]->timing_part=eye;}
            return true;},false);
        return submit_shader([&](ID3D11DeviceContext* commands){
            auto timing=gpu_timings.reserve(device.Get(),1,draws[0]->desc.Width,draws[0]->desc.Height,focused,meta.peripheral_method,meta.configuration);
            const bool measured=gpu_timings.begin(commands,timing,0);
            for(unsigned eye=0;eye<2;++eye)record_draw(commands,*draws[eye],targets[eye].Get());
            if(measured)gpu_timings.end(commands,timing,0);
            return true;},true);
    }
    // Renders a deferred pass into an acquired OpenXR image of the same size.
    bool draw_into(ID3D11Texture2D* target,const Image& source) {
        const auto* draw=deferred(source);if(!target||!draw)return false;
        D3D11_TEXTURE2D_DESC t{};target->GetDesc(&t);
        if(t.Width!=draw->desc.Width||t.Height!=draw->desc.Height||!(t.BindFlags&D3D11_BIND_RENDER_TARGET))return false;
        D3D11_RENDER_TARGET_VIEW_DESC view{};view.Format=draw->desc.Format;
        view.ViewDimension=t.ArraySize>1?D3D11_RTV_DIMENSION_TEXTURE2DARRAY:D3D11_RTV_DIMENSION_TEXTURE2D;
        if(t.ArraySize>1){view.Texture2DArray.ArraySize=1;}
        ComPtr<ID3D11RenderTargetView> rtv;if(FAILED(device->CreateRenderTargetView(target,&view,&rtv)))return false;
        return submit_shader([&](ID3D11DeviceContext* commands){
            const bool measured=gpu_timings.begin(commands,draw->timing,draw->timing_part);
            record_draw(commands,*draw,rtv.Get());
            if(measured)gpu_timings.end(commands,draw->timing,draw->timing_part);
            return true;},true);
    }
    // Copies an immutable frame into an acquired OpenXR image (presentation thread).
    bool upload(ID3D11Texture2D* target,const Image& source) {
        if(!target||!source||!source->texture11)return false;
        return submit([&](ID3D11DeviceContext* c){return upload_timings.measure(device.Get(),c,0,1,[&]{
            c->CopyResource(target,source->texture11.Get());return true;});},true);
    }
};
}
