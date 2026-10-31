#pragma once
// Universal DLSS 5: DLSS-NR on each submitted eye image, inside the buffered
// D3D12 frame-generation pipeline. Per native stereo pair, on the generation
// queue and before frame generation:
//   captured eyes -> optical flow -> motion vectors -> DLSS-NR -> FG -> submit.
// The NR outputs replace the pair's native color, so they are presented and
// become the next interval's FG history. The captured eyes are kept for the
// next optical flow, which therefore never compares NR output with raw color.
// When FG's center also uses optical flow, FG borrows this flow instead of
// estimating it again (Worker::shared_center_flow). FG's periphery flow is
// estimated here too, so the flow engine runs it during DLSS-NR instead of
// after it (Worker::shared_peripheral_flow).
#include "fg_optical.hpp"
#include "fg_transport.hpp"
#include "fg_color_format.hpp"
#include "fg_matrices.hpp"
#include "stereo_alignment.hpp"
#include "../shared/cheeky_nr_universal.hpp"
#include "../shared/cheeky_fg_bridge.hpp"
#include <array>
#include <algorithm>
#include <cmath>
namespace cheeky::fg {

struct UniversalNr12 {
    struct Input {
        ID3D12Device* device{};
        ID3D12CommandQueue* queue{};
        Transport* pool{}; // Allocates the NR images.
        std::array<Image,2> previous_raw,current_raw; // GENERIC_READ captured eyes.
        std::array<Camera,2> cameras;
        StereoAlignment alignment;
        const CheekyFgPair* guides{};
        bool reset{};
        // The previous native frame has no NR image: also copy it into NR's
        // image format, so FG never mixes typed views within one interval.
        bool convert_previous{};
    };
    struct Output {
        std::array<Image,2> color; // NR output, or an unmodified copy where NR failed.
        std::array<Image,2> previous_copy; // Previous native as the same image format, when it had none.
        std::array<bool,2> evaluated{},shared{};
        std::array<OpticalConstants,2> flow_constants{};
        std::array<bool,2> periphery_shared{}; // FG's periphery flow, still to be consumed.
        std::array<OpticalConstants,2> periphery_constants{};
        bool game_motion{},game_depth{};
        bool projection_alignment{};
        const char* reason{};
    };

    optical::Session12 flow[2],periphery[2];
    ComPtr<ID3D12Resource> vectors[2];
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    // Lists, in submission order: image copies (run while the optical flow
    // engine estimates), then each eye's vectors and DLSS-NR, each after only
    // that eye's flow. Eye 0's NR then overlaps eye 1's flow.
    static constexpr unsigned list_count=3;
    ComPtr<ID3D12CommandAllocator> allocators[list_count];ComPtr<ID3D12GraphicsCommandList> lists[list_count];
    ID3D12GraphicsCommandList* list{}; // The list being recorded.
    ComPtr<ID3D12Fence> fence;HANDLE event{};std::uint64_t serial{};
    UniversalNr12()=default;UniversalNr12(const UniversalNr12&)=delete;UniversalNr12& operator=(const UniversalNr12&)=delete;
    ~UniversalNr12(){wait();if(event)CloseHandle(event);}

    static bool options(CheekyNrUniversalOptionsV1& out) noexcept {
        const auto runtime=GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll");
        const auto get=runtime?reinterpret_cast<CheekyNrGetUniversalOptionsV1Fn>(GetProcAddress(runtime,"CheekyNR_GetUniversalOptionsV1")):nullptr;
        if(!get)return false;
        if(!get(&out)){
            out={};out.abi=1;out.size=CHEEKY_NR_UNIVERSAL_OPTIONS_V1_SIZE;
            if(!get(&out))return false;
        }
        return out.enabled!=0;
    }
    static void report(const CheekyNrUniversalStatusV1& status,const Output& output) noexcept {
        const auto runtime=GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll");
        const auto put=runtime?reinterpret_cast<CheekyNrReportUniversalV1Fn>(GetProcAddress(runtime,"CheekyNR_ReportUniversalV1")):nullptr;
        if(put)put(&status);
        const auto alignment=runtime?reinterpret_cast<CheekyNrReportUniversalAlignmentV1Fn>(
            GetProcAddress(runtime,"CheekyNR_ReportUniversalAlignmentV1")):nullptr;
        if(alignment){CheekyNrUniversalAlignmentV1 value;
            value.projection_alignment=output.projection_alignment&&status.evaluated[0]&&status.evaluated[1]&&!status.fallback?1u:0u;
            alignment(&value);}
    }
    // The non-sRGB member of the captured format's family, if DLSS-NR can
    // write it through a UAV. Copies between family members keep the bits.
    static DXGI_FORMAT nr_format(ID3D12Device* dev,DXGI_FORMAT captured) noexcept {
        DXGI_FORMAT f=DXGI_FORMAT_UNKNOWN;
        switch(color_family(captured)){
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:f=DXGI_FORMAT_R8G8B8A8_UNORM;break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:f=DXGI_FORMAT_B8G8R8A8_UNORM;break;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:f=DXGI_FORMAT_R10G10B10A2_UNORM;break;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:f=DXGI_FORMAT_R16G16B16A16_FLOAT;break;
        default:return DXGI_FORMAT_UNKNOWN;
        }
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{f};
        if(FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)))||
           !(support.Support1&D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)||
           !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))return DXGI_FORMAT_UNKNOWN;
        return f;
    }
    bool wait() noexcept {
        if(!fence||fence->GetCompletedValue()>=serial)return true;
        if(!event||FAILED(fence->SetEventOnCompletion(serial,event)))return false;
        return WaitForSingleObject(event,10000)==WAIT_OBJECT_0;
    }
    bool commands(ID3D12Device* dev,ID3D12CommandQueue* q) noexcept {
        if(device.Get()!=dev||queue.Get()!=q){
            wait();for(auto& a:allocators)a.Reset();for(auto& l:lists)l.Reset();list=nullptr;fence.Reset();serial=0;device=dev;queue=q;
            for(auto& v:vectors)v.Reset();
        }
        if(!event&&!(event=CreateEventW(nullptr,FALSE,FALSE,nullptr)))return false;
        if(!fence&&FAILED(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))))return false;
        for(unsigned i=0;i<list_count;++i)if(!allocators[i]&&(FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocators[i])))||
           FAILED(dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators[i].Get(),nullptr,IID_PPV_ARGS(&lists[i])))||
           FAILED(lists[i]->Close())))return false;
        // Only this stage's previous lists must retire before their allocators
        // are reused; the next pair normally arrives long after.
        if(!wait())return false;
        for(unsigned i=0;i<list_count;++i)if(FAILED(allocators[i]->Reset())||FAILED(lists[i]->Reset(allocators[i].Get(),nullptr))){close_all();return false;}
        list=lists[0].Get();return true;
    }
    // A failed run leaves no list recording, so the next commands() can reset.
    void close_all() noexcept {for(auto& l:lists)if(l)l->Close();}
    bool vector_texture(unsigned eye,unsigned w,unsigned h) noexcept {
        if(vectors[eye]){const auto d=vectors[eye]->GetDesc();if(d.Width==w&&d.Height==h)return true;vectors[eye].Reset();}
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=d.MipLevels=1;
        d.Format=DXGI_FORMAT_R32G32_FLOAT;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&vectors[eye])));
    }
    static void transition(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) noexcept {
        optical::Session12::barrier(l,r,a,b);
    }
    // An image in DLSS-NR's format holding an exact copy of `source`.
    Image copy_image(Transport& pool,const Image& source,DXGI_FORMAT format,unsigned w,unsigned h) noexcept {
        auto desc=source->texture->GetDesc();desc.Width=w;desc.Height=h;desc.DepthOrArraySize=desc.MipLevels=1;desc.Format=format;
        auto image=pool.image(desc,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);if(!image)return {};
        transition(list,source->texture.Get(),source->state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(list,image->texture.Get(),image->state,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=source->texture.Get();to.pResource=image->texture.Get();
        from.Type=to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;const D3D12_BOX box{0,0,0,w,h,1};
        list->CopyTextureRegion(&to,0,0,0,&from,&box);
        transition(list,image->texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_GENERIC_READ);
        transition(list,source->texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,source->state);
        image->state=D3D12_RESOURCE_STATE_GENERIC_READ;image->layout=source->layout;image->fg_border=source->fg_border;
        image->presentation_feather=source->presentation_feather;
        image->alignment_borders=source->alignment_borders;
        const auto captured=source->texture->GetDesc().Format;
        image->presentation_format=source->presentation_format!=DXGI_FORMAT_UNKNOWN?source->presentation_format:
            srgb_color(captured)?captured:DXGI_FORMAT_UNKNOWN;
        return image;
    }
    // Normalized eye rectangle (x,y,w,h) of extent (fw,fh) around center (u,v).
    static std::array<float,4> centered(float fw,float fh,float u,float v) noexcept {
        fw=std::clamp(fw,0.f,1.f);fh=std::clamp(fh,0.f,1.f);
        return {std::clamp(u-fw*.5f,0.f,1.f-fw),std::clamp(v-fh*.5f,0.f,1.f-fh),fw,fh};
    }
    // FG's own center output rectangle, exactly as the worker derives it.
    static std::array<float,4> fg_center(const CheekyFgEye& g,unsigned width,unsigned height) noexcept {
        const bool focused=g.focus_width<.999f||g.focus_height<.999f;
        if(!focused||!width||!height)return {0,0,1,1};
        const unsigned rw=std::clamp(unsigned(std::round(width*g.focus_width)),1u,width);
        const unsigned rh=std::clamp(unsigned(std::round(height*g.focus_height)),1u,height);
        const unsigned rx=unsigned(std::clamp((width-rw)*.5f+g.focus_x*width,0.f,float(width-rw)));
        const unsigned ry=unsigned(std::clamp((height-rh)*.5f+g.focus_y*height,0.f,float(height-rh)));
        return {float(rx)/width,float(ry)/height,float(rw)/width,float(rh)/height};
    }
    static std::array<float,4> bounds(const std::array<float,4>& a,const std::array<float,4>& b) noexcept {
        const float x=std::min(a[0],b[0]),y=std::min(a[1],b[1]);
        return {x,y,std::max(a[0]+a[2],b[0]+b[2])-x,std::max(a[1]+a[3],b[1]+b[3])-y};
    }

    bool run(const Input& in,const CheekyNrUniversalOptionsV1& opts,Output& out) noexcept {
        out={};
        const auto runtime=GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll");
        const auto evaluate=runtime?reinterpret_cast<CheekyNrEvaluateUniversalV1Fn>(GetProcAddress(runtime,"CheekyNR_EvaluateUniversalV1")):nullptr;
        if(!evaluate){out.reason="CheekyFoveatedDLSSRuntime.dll has no Universal DLSS 5 support; update it";return false;}
        if(!in.device||!in.queue||!in.pool||!in.guides){out.reason="Universal DLSS 5 inputs unavailable";return false;}
        for(unsigned eye=0;eye<2;++eye)if(!in.current_raw[eye]||!in.previous_raw[eye]||!in.current_raw[eye]->texture||!in.previous_raw[eye]->texture){
            out.reason="Universal DLSS 5 needs both captured eyes";return false;}
        for(unsigned eye=0;eye<2;++eye)if(in.current_raw[eye]->layout.packed()||in.previous_raw[eye]->layout.packed()){
            out.reason="Universal DLSS 5 needs full-resolution eye captures";return false;}
        const auto captured=in.current_raw[0]->texture->GetDesc();
        const auto format=nr_format(in.device,captured.Format);
        if(format==DXGI_FORMAT_UNKNOWN){out.reason="Universal DLSS 5 cannot write this headset image format";return false;}
        if(!commands(in.device,in.queue)){out.reason="Universal DLSS 5 command allocation failed";return false;}
        struct EyeWork {unsigned w{},h{};OpticalConstants c{};bool flow{};ID3D12Resource* motion{};ID3D12Resource* depth{};
            unsigned motion_rect[4]{},depth_rect[4]{};float motion_scale[2]{1,1};bool game_motion{},game_depth{},depth_inverted{};
            bool has_center{};float center[2]{.5f,.5f};};
        std::array<EyeWork,2> work{};
        out.projection_alignment=opts.automatic_alignment&&in.alignment.valid;
        for(unsigned eye=0;eye<2;++eye){
            auto& e=work[eye];const auto& raw=in.current_raw[eye];const auto& g=in.guides->eyes[eye];
            e.w=raw->layout.width?raw->layout.width:unsigned(raw->texture->GetDesc().Width);
            e.h=raw->layout.height?raw->layout.height:raw->texture->GetDesc().Height;
            const auto center=universal_eye_center(opts,g,in.alignment,eye);
            e.has_center=true;e.center[0]=center[0];e.center[1]=center[1];
            if(out.projection_alignment){const auto projected=aligned_eye_center(in.alignment,eye,opts.aligned_height_offset);
                out.projection_alignment=std::abs(center[0]-projected[0])<1.e-5f&&std::abs(center[1]-projected[1])<1.e-5f;}
            // Raw game guides are snapshots of the eye's DLSS inputs (full eye <-> active rect).
            const bool raw_guides=!g.prepared_guides;
            if(opts.motion==1&&raw_guides&&g.motion){
                const auto d=g.motion->GetDesc();
                const unsigned mw=g.motion_width?g.motion_width:unsigned(d.Width)-std::min(unsigned(d.Width),g.motion_base_x);
                const unsigned mh=g.motion_height?g.motion_height:d.Height-std::min(d.Height,g.motion_base_y);
                if(mw&&mh){e.game_motion=true;e.motion=g.motion;e.motion_rect[0]=g.motion_base_x;e.motion_rect[1]=g.motion_base_y;
                    e.motion_rect[2]=mw;e.motion_rect[3]=mh;e.motion_scale[0]=g.motion_scale_x/mw;e.motion_scale[1]=g.motion_scale_y/mh;}
            }
            if(opts.depth!=2&&raw_guides&&g.depth){
                const auto d=g.depth->GetDesc();
                const unsigned dw=g.depth_width?g.depth_width:unsigned(d.Width)-std::min(unsigned(d.Width),g.depth_base_x);
                const unsigned dh=g.depth_height?g.depth_height:d.Height-std::min(d.Height,g.depth_base_y);
                if(dw&&dh){e.game_depth=true;e.depth=g.depth;e.depth_rect[0]=g.depth_base_x;e.depth_rect[1]=g.depth_base_y;
                    e.depth_rect[2]=dw;e.depth_rect[3]=dh;e.depth_inverted=g.depth_inverted!=0;}
            }
            if(e.game_motion)continue;
            // Flow window: the NR region (with the usual halo), widened to FG's
            // center when FG shares this flow.
            const bool share=opts.share_optical&&cheeky_fg_optical(g.center_method);
            auto rect=opts.foveated?centered(opts.width,opts.height,e.center[0],e.center[1]):std::array<float,4>{0,0,1,1};
            auto flow_eye=g;flow_eye.width=e.w;flow_eye.height=e.h;
            if(share)rect=bounds(rect,fg_center(g,e.w,e.h));
            else{flow_eye.optical.center_preset=opts.optical_preset;flow_eye.optical.center_percent=opts.optical_percent;flow_eye.optical.center_grid=opts.optical_grid;}
            if(!vector_texture(eye,std::max(64u,(e.w+1)/2),std::max(64u,(e.h+1)/2))){out.reason="Universal DLSS 5 vector allocation failed";out.shared={};close_all();return false;}
            const auto vd=vectors[eye]->GetDesc();
            e.c=optical::constants(flow_eye,0,rect.data(),in.previous_raw[eye]->layout,raw->layout,unsigned(vd.Width),vd.Height);
            e.c.texel[3]=0; // Fast mode, no vector cache; FG builds its own cache if it borrows.
            if(in.cameras[eye].valid)optical::pose_mapping(e.c,in.cameras[eye].previous_pose,in.cameras[eye].previous_fov,
                in.cameras[eye].pose,in.cameras[eye].view_fov,.5f);
            const auto old_format=in.previous_raw[eye]->texture->GetDesc().Format,now_format=raw->texture->GetDesc().Format;
            // Submits its input preparation and the flow. The queue waits for it
            // only right before this eye's vectors, so both eyes' flows run
            // back-to-back on the optical flow engine.
            if(!flow[eye].prepare(in.device,in.queue,in.previous_raw[eye]->texture.Get(),raw->texture.Get(),
                share?g.optical.center_preset:opts.optical_preset,e.c,in.reset||g.reset!=0,D3D12_RESOURCE_STATE_GENERIC_READ,old_format,now_format,true)){
                out.reason="Universal DLSS 5 optical flow failed";continue;
            }
            e.flow=true;out.shared[eye]=share;out.flow_constants[eye]=e.c;
            out.flow_constants[eye].sizes[0]=float(flow[eye].width);out.flow_constants[eye].sizes[1]=float(flow[eye].height);
            e.motion=vectors[eye].Get();e.motion_rect[2]=unsigned(vd.Width);e.motion_rect[3]=vd.Height;
        }
        // FG's periphery flows queue behind both NR flows on the flow engine
        // and finish while DLSS-NR runs. FG consumes them before its reader.
        for(unsigned eye=0;eye<2&&opts.share_optical;++eye){
            const auto& g=in.guides->eyes[eye];
            if(!cheeky_fg_optical(g.peripheral_method)||(g.focus_width>=.999f&&g.focus_height>=.999f))continue;
            const float full[]{0,0,1,1};
            auto c=optical::constants(g,1,full,in.previous_raw[eye]->layout,in.current_raw[eye]->layout,
                std::max(32u,(work[eye].w*g.peripheral_percent+50)/100),std::max(32u,(work[eye].h*g.peripheral_percent+50)/100));
            if(in.cameras[eye].valid)optical::pose_mapping(c,in.cameras[eye].previous_pose,in.cameras[eye].previous_fov,
                in.cameras[eye].pose,in.cameras[eye].view_fov,.5f);
            const auto& old_image=in.previous_raw[eye]->texture;const auto& now_image=in.current_raw[eye]->texture;
            if(!periphery[eye].prepare(in.device,in.queue,old_image.Get(),now_image.Get(),g.optical.peripheral_preset,c,in.reset||g.reset!=0,
                D3D12_RESOURCE_STATE_GENERIC_READ,old_image->GetDesc().Format,now_image->GetDesc().Format,true))continue;
            out.periphery_shared[eye]=true;out.periphery_constants[eye]=c;
            out.periphery_constants[eye].sizes[0]=float(periphery[eye].width);out.periphery_constants[eye].sizes[1]=float(periphery[eye].height);
        }
        // List 0: image copies only; it runs on 3D while the flows estimate.
        for(unsigned eye=0;eye<2&&in.convert_previous;++eye){
            const auto& p=in.previous_raw[eye];
            out.previous_copy[eye]=copy_image(*in.pool,p,format,p->layout.width?p->layout.width:unsigned(p->texture->GetDesc().Width),
                p->layout.height?p->layout.height:p->texture->GetDesc().Height);
        }
        for(unsigned eye=0;eye<2;++eye){
            out.color[eye]=copy_image(*in.pool,in.current_raw[eye],format,work[eye].w,work[eye].h);
            if(!out.color[eye]){out.reason="Universal DLSS 5 image allocation failed";break;}
        }
        bool good=out.color[0]&&out.color[1]&&(!in.convert_previous||(out.previous_copy[0]&&out.previous_copy[1]));
        bool submitted=true;
        const auto submit=[&](unsigned index){
            if(FAILED(lists[index]->Close())){submitted=false;return;}
            ID3D12CommandList* l[]{lists[index].Get()};in.queue->ExecuteCommandLists(1,l);
        };
        submit(0);
        // Lists 1 and 2: each eye after only its own flow.
        for(unsigned eye=0;eye<2;++eye){
            auto& e=work[eye];list=lists[1+eye].Get();
            if(e.flow&&!flow[eye].consume()){e.flow=false;e.motion=nullptr;out.shared[eye]=false;out.reason="Universal DLSS 5 optical flow wait failed";}
            if(e.flow&&!flow[eye].nr_vectors(list,vectors[eye].Get(),e.c,false)){
                e.flow=false;e.motion=nullptr;out.shared[eye]=false;out.reason="Universal DLSS 5 vector conversion failed";}
            if(e.flow)transition(list,vectors[eye].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if(good&&e.motion){
                CheekyNrUniversalEyeV1 nr;nr.list=list;nr.eye=eye;nr.reset=in.reset||in.guides->eyes[eye].reset?1u:0u;
                nr.color=out.color[eye]->texture.Get();nr.color_state=D3D12_RESOURCE_STATE_GENERIC_READ;nr.width=e.w;nr.height=e.h;
                nr.hdr=format==DXGI_FORMAT_R16G16B16A16_FLOAT?1u:0u;
                nr.motion=e.motion;nr.motion_state=e.game_motion?D3D12_RESOURCE_STATE_COMMON:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                nr.motion_x=e.motion_rect[0];nr.motion_y=e.motion_rect[1];nr.motion_width=e.motion_rect[2];nr.motion_height=e.motion_rect[3];
                nr.motion_uv_scale_x=e.motion_scale[0];nr.motion_uv_scale_y=e.motion_scale[1];
                if(e.depth){nr.depth=e.depth;nr.depth_state=D3D12_RESOURCE_STATE_COMMON;nr.depth_x=e.depth_rect[0];nr.depth_y=e.depth_rect[1];
                    nr.depth_width=e.depth_rect[2];nr.depth_height=e.depth_rect[3];nr.depth_inverted=e.depth_inverted?1u:0u;}
                nr.has_center=e.has_center?1u:0u;nr.center_u=e.center[0];nr.center_v=e.center[1];
                nr.game_motion=e.game_motion?1u:0u;nr.shared_flow=out.shared[eye]?1u:0u;
                out.evaluated[eye]=evaluate(&nr);
                out.game_motion|=e.game_motion;out.game_depth|=e.game_depth&&out.evaluated[eye];
                if(!out.evaluated[eye]&&!out.reason)out.reason="DLSS-NR rejected this eye; see the DLSS-NR runtime state";
            }
            if(e.flow)transition(list,vectors[eye].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
            submit(1+eye);
        }
        list=lists[0].Get();
        if(FAILED(in.queue->Signal(fence.Get(),++serial))){--serial;}
        if(!submitted){out={};out.reason="Universal DLSS 5 recording failed";return false;}
        if(!good){out.color={};out.previous_copy={};out.shared={};out.periphery_shared={};}
        return good;
    }
};
}
