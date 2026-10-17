#include "dlssg_worker.hpp"
#include "fg_color_format.hpp"
#include <algorithm>
#include <limits>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <cmath>
#include <cwchar>
#include <deque>
#include <mutex>

namespace cheeky::fg {
namespace {
using namespace Microsoft::WRL;
std::recursive_mutex ngx_mutex;
// NGX can retain the initialization info. Keep it and its strings alive for
// the lifetime of this resident addon; never shut down the game's NGX context.
struct RuntimeInfo {
    std::wstring directory,cache;
    const wchar_t* paths[1]{};
    NVSDK_NGX_FeatureCommonInfo info{};
};
std::deque<RuntimeInfo> runtime_infos;
constexpr std::uint32_t mask=0xFFF00000;
inline bool ok(std::uint32_t code) noexcept{return (code&mask)!=0xBAD00000U;}
void barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
    if(a==b)return;D3D12_RESOURCE_BARRIER t{};t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition.pResource=r;t.Transition.StateBefore=a;t.Transition.StateAfter=b;t.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1,&t);
}
bool texture(ID3D12Device* device,ID3D12Resource* like,UINT width,UINT height,
             D3D12_RESOURCE_FLAGS flags,ComPtr<ID3D12Resource>& out){
    auto desc=like->GetDesc();desc.Width=width;desc.Height=height;desc.Flags=flags;
    desc.MipLevels=1;desc.DepthOrArraySize=1;desc.SampleDesc.Count=1;desc.SampleDesc.Quality=0;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&out)));
}
bool buffer(ID3D12Device* device,std::uint64_t bytes,D3D12_HEAP_TYPE type,
            D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state,ComPtr<ID3D12Resource>& out){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=type;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;
    d.Format=DXGI_FORMAT_UNKNOWN;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;
    return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&out)));
}
}
bool Worker::wait_idle() noexcept {
    if(!device||!fence||!serial)return true;
    while(fence->GetCompletedValue()<serial) {
        if(FAILED(device->GetDeviceRemovedReason()))return false;
        if(event&&SUCCEEDED(fence->SetEventOnCompletion(serial,event)))WaitForSingleObject(event,100);
        else Sleep(1);
    }
    return SUCCEEDED(device->GetDeviceRemovedReason());
}
void Worker::reset_features() noexcept {
    for(auto* array:{&features,&peripheral_features})for(auto& f:*array){
        if(f.handle&&release)release(f.handle);if(f.params&&destroy)destroy(f.params);f={};
    }
    focus.reset();configuration=0;center_width=center_height=peripheral_width=peripheral_height=0;
}
void Worker::reset() noexcept {
    std::lock_guard lock(ngx_mutex);wait_idle();reset_features();
    gpu_timings.reset();completion_wait={};submitted_at={};
    if(event)CloseHandle(event);event=nullptr;device.Reset();queue.Reset();allocator.Reset();list.Reset();fence.Reset();
    allocate=nullptr;destroy=nullptr;create=nullptr;evaluate=nullptr;release=nullptr;
    configured=false;serial=0;width=height=0;init=nullptr;capabilities=nullptr;
}
void Worker::report(const char* stage,std::uint32_t code) noexcept {
    sprintf_s(diagnostic,"%s: 0x%08x",stage,code);
    if(trace)trace(diagnostic);
}
bool Worker::record_output_copies(ID3D12GraphicsCommandList* commands,
    const std::array<ID3D12Resource*,2>& sources,const std::array<OutputCopy,2>& copies) noexcept {
    if(!commands||!copies[0].texture||!copies[1].texture||copies[0].texture==copies[1].texture)return false;
    // Validate the entire pair before recording any resource transitions.
    for(unsigned eye=0;eye<2;++eye) {
        if(!sources[eye]||copies[eye].texture==sources[0]||copies[eye].texture==sources[1])return false;
        const auto a=sources[eye]->GetDesc(),b=copies[eye].texture->GetDesc();
        if(a.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||a.Dimension!=b.Dimension||a.Width!=b.Width||a.Height!=b.Height||
           a.DepthOrArraySize!=b.DepthOrArraySize||a.MipLevels!=b.MipLevels||a.SampleDesc.Count!=b.SampleDesc.Count||
           a.SampleDesc.Quality!=b.SampleDesc.Quality||!compatible_color(a.Format,b.Format))return false;
    }
    for(unsigned eye=0;eye<2;++eye) {
        barrier(commands,sources[eye],D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(commands,copies[eye].texture,copies[eye].state,D3D12_RESOURCE_STATE_COPY_DEST);
        commands->CopyResource(copies[eye].texture,sources[eye]);
        barrier(commands,copies[eye].texture,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_GENERIC_READ);
        barrier(commands,sources[eye],D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    }
    return true;
}
bool Worker::probe(ID3D12Device* dev,const wchar_t* model_directory) noexcept {
    std::lock_guard lock(ngx_mutex);
    if(probe_attempted&&dev==probed_device.Get())return capability_ready&&!failed;
    probe_attempted=true;capability_ready=false;failed=false;probed_device=dev;
    if(!dev){failed=true;report("NGX missing D3D12 device",0);return false;}
    init=&NVSDK_NGX_D3D12_Init_with_ProjectID;
    capabilities=&NVSDK_NGX_D3D12_GetCapabilityParameters;
    allocate=&NVSDK_NGX_D3D12_AllocateParameters;
    destroy=&NVSDK_NGX_D3D12_DestroyParameters;
    create=&NVSDK_NGX_D3D12_CreateFeature;
    evaluate=&NVSDK_NGX_D3D12_EvaluateFeature;
    release=&NVSDK_NGX_D3D12_ReleaseFeature;
    wchar_t executable[32768]{},local_app[32768]{};
    GetModuleFileNameW(nullptr,executable,32768);
    auto* slash=wcsrchr(executable,L'\\');if(slash)*slash=0;
    GetEnvironmentVariableW(L"LOCALAPPDATA",local_app,32768);
    runtime_infos.emplace_back();auto& context=runtime_infos.back();
    context.directory=model_directory?model_directory:executable;
    context.cache=std::wstring(local_app)+L"\\CheekyFoveatedDLSS";
    CreateDirectoryW(context.cache.c_str(),nullptr);
    context.paths[0]=context.directory.c_str();
    context.info.PathListInfo.Path=context.paths;context.info.PathListInfo.Length=1;
    auto result=init("eec05a21-0120-42fa-a51a-eafae09e4cdc",NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        "CheekyFoveatedDLSS",context.cache.c_str(),dev,&context.info,NVSDK_NGX_Version_API);
    report("NGX SDK initialization",result);
    if(!ok(result)){failed=true;return false;}
    NgxParameters* caps{};result=capabilities(&caps);
    report("NGX capability parameters",result);
    if(!ok(result)||!caps){failed=true;return false;}
    int available{},needs_driver{};unsigned feature_result{},major{},minor{};
    const auto available_result=caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available,&available);
    caps->Get(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult,&feature_result);
    caps->Get(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver,&needs_driver);
    caps->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor,&major);
    caps->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor,&minor);
    destroy(caps);
    capability_ready=ok(available_result)&&available!=0;failed=!capability_ready;
    sprintf_s(diagnostic,"NGX FG available=%d query=0x%08x feature=0x%08x needs_driver=%d minimum_driver=%u.%u",
        available,unsigned(available_result),feature_result,needs_driver,major,minor);
    if(trace)trace(diagnostic);
    return capability_ready;
}
bool Worker::initialize(ID3D12Device* dev,ID3D12CommandQueue* q,unsigned w,unsigned h,DXGI_FORMAT fmt) noexcept {
    std::lock_guard lock(ngx_mutex);
    if(configured&&dev==device.Get()&&q==queue.Get()&&w==width&&h==height&&fmt==format)return true;
    reset();
    if(!dev||!q||!w||!h)return false;
    if(!probe(dev))return false;
    // reset() releases graphics objects but capability data remains cached.
    allocate=&NVSDK_NGX_D3D12_AllocateParameters;destroy=&NVSDK_NGX_D3D12_DestroyParameters;
    create=&NVSDK_NGX_D3D12_CreateFeature;evaluate=&NVSDK_NGX_D3D12_EvaluateFeature;
    release=&NVSDK_NGX_D3D12_ReleaseFeature;
    device=dev;queue=q;width=w;height=h;format=fmt;
    focus.color_format=force_fp16?DXGI_FORMAT_R16G16B16A16_FLOAT:inference_color_format(fmt);
    if(FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))||
       FAILED(dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)))||
       FAILED(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)))){reset();return false;}
    event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!event){reset();return false;}
    // Bootstrap record is closed; subsequent calls Reset it after the last submission retires.
    if(FAILED(list->Close())){reset();return false;}
    // Optional telemetry; unsupported queries must never disable generation.
    gpu_timings.initialize(dev,q);
    configured=true;return true;
}
bool Worker::generate(const CheekyFgPair& guides,const std::array<ID3D12Resource*,2>& colors,
    const std::array<ID3D12Resource*,2>& previous,const std::array<Camera,2>& cameras,
    std::array<ID3D12Resource*,2>& outputs,std::array<OutputCopy,2>* copies,
    void(*on_submitted)(void*) noexcept,void* submitted_context) noexcept {
    submitted_at={};
    std::lock_guard lock(ngx_mutex);
    if(!configured||guides.producer_queue!=queue.Get()||!colors[0]||!colors[1]||!previous[0]||!previous[1]||!cameras[0].valid||!cameras[1].valid)return false;
    if(!wait_idle())return false;
    if(configuration!=guides.eyes[0].configuration) {
        reset_features();configuration=guides.eyes[0].configuration;gpu_timings.initialize(device.Get(),queue.Get());completion_wait={};
    }
    if(FAILED(allocator->Reset())||FAILED(list->Reset(allocator.Get(),nullptr)))return false;
    gpu_timings.begin(list.Get());
    bool good=true;unsigned focused{};
    auto scaled=[](unsigned dimension,unsigned percent){return std::max(32u,(dimension*percent+50u)/100u);};
    // A foveated/scaled frame already has a final composition dispatch. Write
    // it into a leased publication image instead of a private scratch image
    // followed by another full stereo copy. The caller owns each destination
    // until the presenter and its GPU uploads have finished using it.
    bool direct=copies&&(*copies)[0].texture&&(*copies)[1].texture&&(*copies)[0].texture!=(*copies)[1].texture;
    for(unsigned eye=0;eye<2&&direct;++eye) {
        const auto& g=guides.eyes[eye];const auto d=(*copies)[eye].texture->GetDesc();
        const bool composition=g.focus_width<.999f||g.focus_height<.999f||
            scaled(width,g.resolution_percent)!=width||scaled(height,g.resolution_percent)!=height;
        direct=composition&&d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width==width&&d.Height==height&&
            d.DepthOrArraySize==1&&d.MipLevels==1&&d.SampleDesc.Count==1&&d.Format==focus.color_format&&
            (d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        for(unsigned source=0;source<2&&direct;++source)
            direct=(*copies)[eye].texture!=colors[source]&&(*copies)[eye].texture!=previous[source]&&
                (*copies)[eye].texture!=guides.eyes[source].depth&&(*copies)[eye].texture!=guides.eyes[source].motion;
    }
    auto transpose=[](Matrix m){Matrix result{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)result.m[r*4+c]=m.m[c*4+r];return result;};
    auto evaluate_pass=[&](Feature& f,const FocusInput& input,const Camera& camera,const CheekyFgEye& g,const FocusRect& rect) {
        const auto desc=input.color->GetDesc();const auto tw=unsigned(desc.Width),th=desc.Height;
        const auto guide_desc=input.depth->GetDesc();const auto gw=unsigned(guide_desc.Width),gh=guide_desc.Height;
        if(f.output&&(f.output->GetDesc().Width!=tw||f.output->GetDesc().Height!=th)) {
            if(f.handle)release(f.handle);if(f.params)destroy(f.params);f={};
        }
        if(!f.output&&(!texture(device.Get(),input.color.Get(),tw,th,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,f.output)||
            !buffer(device.Get(),8,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON,f.disable)||
            !buffer(device.Get(),8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,f.readback))) {
            report("NGX FG output allocation",unsigned(E_FAIL));failed=true;return false;
        }
        if(!f.params) {const auto result=allocate(&f.params);if(!ok(result)||!f.params){report("NGX FG parameters",result);failed=true;return false;}}
        bool created=false;auto* p=f.params;
        if(!f.handle) {
            p->Set("CreationNodeMask",1u);p->Set("VisibilityNodeMask",1u);p->Set("Width",tw);p->Set("Height",th);
            p->Set("DLSSG.BackbufferFormat",unsigned(desc.Format));p->Set("DLSSG.InternalWidth",gw);p->Set("DLSSG.InternalHeight",gh);p->Set("DLSSG.DynamicResolution",1u);
            const auto result=create(list.Get(),NVSDK_NGX_Feature_FrameGeneration,p,&f.handle);report("NGX FG CreateFeature",result);
            if(!ok(result)||!f.handle){failed=true;return false;}created=true;
        }
        Matrix crop=identity(),uncrop=identity();
        if(rect.active) {
            crop.m[0]=float(width)/rect.w;crop.m[5]=float(height)/rect.h;
            crop.m[3]=(float(width)-2*rect.x-rect.w)/rect.w;crop.m[7]=-(float(height)-2*rect.y-rect.h)/rect.h;
            if(!inverse(crop,uncrop))return false;
        }
        auto projection=multiply(crop,camera.view_to_clip),clip_prev=multiply(multiply(crop,camera.clip_to_prev),uncrop);
        Matrix unprojection{},prev_clip{};if(!inverse(projection,unprojection)||!inverse(clip_prev,prev_clip))return false;
        // SDK matrices use row vectors. Our camera math uses column vectors.
        auto proj=transpose(projection),unproj=transpose(unprojection),cp=transpose(clip_prev),pc=transpose(prev_clip),lens=identity();
        p->Set("DLSSG.CameraViewToClip",proj.m);p->Set("DLSSG.ClipToCameraView",unproj.m);
        p->Set("DLSSG.ClipToLensClip",lens.m);p->Set("DLSSG.ClipToPrevClip",cp.m);p->Set("DLSSG.PrevClipToClip",pc.m);
        p->Set("DLSSG.CameraNear",.05f);p->Set("DLSSG.CameraFar",10000.f);
        p->Set("DLSSG.CameraFOV",2.f*std::atan(1.f/std::max(.001f,std::abs(projection.m[5]))));
        p->Set("DLSSG.CameraAspectRatio",std::abs(projection.m[5]/projection.m[0]));
        const char* axes[]{"Pos","Right","Up","Fwd"};const float* vec[]{camera.pos,camera.right,camera.up,camera.forward};
        for(unsigned j=0;j<4;++j)for(unsigned k=0;k<3;++k){char name[64]{};sprintf_s(name,"DLSSG.Camera%s%s",axes[j],k==0?"X":k==1?"Y":"Z");p->Set(name,vec[j][k]);}
        const float rw=float(rect.w)/width,rh=float(rect.h)/height;
        p->Set("DLSSG.JitterOffsetX",g.jitter_x*gw/(std::max(1u,g.depth_width)*rw));
        p->Set("DLSSG.JitterOffsetY",g.jitter_y*gh/(std::max(1u,g.depth_height)*rh));
        // Prepared guides store UV displacement. NGX's frame-generation scale
        // converts it to pixels in the guide grid, including cropped/scaled inputs.
        p->Set("DLSSG.MvecScaleX",float(gw));p->Set("DLSSG.MvecScaleY",float(gh));
        p->Set("DLSSG.DepthInverted",g.depth_inverted);p->Set("DLSSG.CameraMotionIncluded",1u);
        p->Set("DLSSG.ColorBuffersHDR",desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?1u:0u);p->Set("DLSSG.MvecDilated",0u);
        p->Set("DLSSG.MvecInvalidValue",input.motion->GetDesc().Format==DXGI_FORMAT_R16G16_FLOAT?65504.f:(std::numeric_limits<float>::max)());
        p->Set("DLSSG.Reset",g.reset||created?1u:0u);p->Set("DLSSG.MultiFrameCount",1u);p->Set("DLSSG.MultiFrameIndex",1u);
        p->Set("DLSSG.Backbuffer",input.color.Get());p->Set("DLSSG.Depth",input.depth.Get());p->Set("DLSSG.MVecs",input.motion.Get());
        p->Set("DLSSG.OutputInterpolated",f.output.Get());p->Set("DLSSG.OutputDisableInterpolation",f.disable.Get());
        for(const char* key:{"DLSSG.BackbufferSubrectBaseX","DLSSG.BackbufferSubrectBaseY","DLSSG.InputBackbufferSubrectBaseX","DLSSG.InputBackbufferSubrectBaseY",
            "DLSSG.DepthSubrectBaseX","DLSSG.DepthSubrectBaseY","DLSSG.MVecsSubrectBaseX","DLSSG.MVecsSubrectBaseY","DLSSG.OutputInterpolatedSubrectBaseX","DLSSG.OutputInterpolatedSubrectBaseY"})p->Set(key,0u);
        for(const char* key:{"DLSSG.BackbufferSubrectWidth","DLSSG.InputBackbufferSubrectWidth","DLSSG.OutputInterpolatedSubrectWidth"})p->Set(key,tw);
        for(const char* key:{"DLSSG.BackbufferSubrectHeight","DLSSG.InputBackbufferSubrectHeight","DLSSG.OutputInterpolatedSubrectHeight"})p->Set(key,th);
        for(const char* key:{"DLSSG.DepthSubrectWidth","DLSSG.MVecsSubrectWidth"})p->Set(key,gw);
        for(const char* key:{"DLSSG.DepthSubrectHeight","DLSSG.MVecsSubrectHeight"})p->Set(key,gh);
        barrier(list.Get(),f.output.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(list.Get(),f.disable.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const auto result=evaluate(list.Get(),f.handle,p,nullptr);
        if(!ok(result)){report("NGX FG EvaluateFeature",result);failed=true;return false;}
        barrier(list.Get(),f.output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        barrier(list.Get(),f.disable.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(f.readback.Get(),0,f.disable.Get(),0,4);
        barrier(list.Get(),f.disable.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);return true;
    };
    bool foveated[2]{};
    // Diagnostic comparison: alternate processing order, never eye identity.
    // All features, guides, cameras, histories and publication slots remain
    // indexed by the original eye. This distinguishes a first-work delay from
    // a slow or differently laid-out left-eye input without changing queues.
    for(unsigned step=0;step<2&&good;++step) {
        const unsigned eye=(gpu_timings.first_eye+step)%2;
        const auto& g=guides.eyes[eye];const unsigned query=2+eye*6;gpu_timings.stamp(list.Get(),query);
        if(!g.depth||!g.motion||g.width!=width||g.height!=height){report("FG guide/output dimensions disagree",unsigned(E_INVALIDARG));failed=true;good=false;break;}
        source_depth_format=unsigned(g.depth->GetDesc().Format);source_motion_format=unsigned(g.motion->GetDesc().Format);
        if(depth_srv_format(DXGI_FORMAT(source_depth_format))==DXGI_FORMAT_UNKNOWN||motion_srv_format(DXGI_FORMAT(source_motion_format))==DXGI_FORMAT_UNKNOWN){
            sprintf_s(diagnostic,"FG guide conversion unsupported: depth=%u motion=%u; displaying native frames",source_depth_format,source_motion_format);
            if(trace)trace(diagnostic);failed=true;good=false;break;
        }
        FocusRect full_rect{0,0,width,height,0,false},rect=full_rect;
        rect.active=g.focus_width<.999f||g.focus_height<.999f;rect.feather=g.focus_feather;
        if(rect.active) {
            rect.w=std::clamp(unsigned(std::round(width*g.focus_width)),1u,width);rect.h=std::clamp(unsigned(std::round(height*g.focus_height)),1u,height);
            rect.x=unsigned(std::clamp((width-rect.w)*.5f+g.focus_x*width,0.f,float(width-rect.w)));
            rect.y=unsigned(std::clamp((height-rect.h)*.5f+g.focus_y*height,0.f,float(height-rect.h)));
            ++focused;
        }
        foveated[eye]=rect.active;peripheral_method=g.peripheral_method;
        center_width=scaled(rect.w,g.resolution_percent);center_height=scaled(rect.h,g.resolution_percent);
        peripheral_width=rect.active?scaled(width,g.peripheral_percent):0;peripheral_height=rect.active?scaled(height,g.peripheral_percent):0;
        Camera camera=cameras[eye];
        // Camera matrices already use the guide's native depth convention.
        // Converting reversed depth again here cancels the caller's conversion.
        auto* detail=gpu_timings.record_preparation?&gpu_timings:nullptr;
        if(!focus.prepare_inputs(device.Get(),list.Get(),eye,0,colors[eye],g,rect,center_width,center_height,32,32,input_color_state,detail)) {report("FG center guide conversion",unsigned(E_FAIL));failed=true;good=false;break;}
        if(rect.active&&!focus.prepare_inputs(device.Get(),list.Get(),eye,1,colors[eye],g,full_rect,peripheral_width,peripheral_height,g.depth_bits,g.motion_bits,input_color_state,detail)) {
            report("FG periphery guide conversion",unsigned(E_FAIL));failed=true;good=false;break;
        }
        if(!rect.active)for(unsigned phase=0;phase<4;++phase)gpu_timings.preparation_stamp(list.Get(),eye,1,phase);
        gpu_timings.stamp(list.Get(),query+1);gpu_timings.stamp(list.Get(),query+2);
        good=evaluate_pass(features[eye],focus.inputs[eye][0],camera,g,rect);gpu_timings.stamp(list.Get(),query+3);
        if(!good)break;gpu_timings.stamp(list.Get(),query+4);
        auto* base=features[eye].output.Get();
        if(rect.active) {
            if(g.peripheral_method==3) {
                good=evaluate_pass(peripheral_features[eye],focus.inputs[eye][1],camera,g,full_rect);
                base=peripheral_features[eye].output.Get();
            }else {
                good=focus.generate_periphery(device.Get(),list.Get(),eye,previous[eye],colors[eye],g,camera,input_color_state);
                base=focus.peripheral_output[eye].Get();
            }
        }
        if(good&&(rect.active||center_width!=width||center_height!=height)) {
            good=focus.compose_frame(device.Get(),list.Get(),eye,features[eye].output.Get(),base,rect,width,height,
                direct?(*copies)[eye].texture:nullptr,direct?(*copies)[eye].state:D3D12_RESOURCE_STATE_COMMON);
            outputs[eye]=direct?(*copies)[eye].texture:focus.full[eye].Get();
        }else outputs[eye]=base;
        if(good&&rect.active&&g.peripheral_method==2)good=focus.save_depth(device.Get(),list.Get(),eye);
        if(!good&&!failed){report("FG periphery/composition allocation",unsigned(E_FAIL));failed=true;}
        gpu_timings.stamp(list.Get(),query+5);
    }
    if(good)gpu_timings.stamp(list.Get(),1);
    if(good)gpu_timings.stamp(list.Get(),14);
    // Publish immutable output in the SAME GPU submission as inference. A copy
    // submitted only after the CPU completion wait can land behind a newer game
    // frame and delay a finished interpolation by another render interval.
    if(good&&copies&&!direct&&!record_output_copies(list.Get(),outputs,*copies)) {
        report("FG immutable output copy",unsigned(E_INVALIDARG));failed=true;good=false;
    }
    if(good){gpu_timings.stamp(list.Get(),15);gpu_timings.resolve(list.Get());}
    if(FAILED(list->Close())||!good)return false;
    ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
    if(copies)for(auto& copy:*copies)copy.state=D3D12_RESOURCE_STATE_GENERIC_READ;
    if(FAILED(queue->Signal(fence.Get(),++serial)))return false;
    submitted_at=TimingClock::now();
    // Notify only after commands AND their completion fence are queued. The
    // presenter may release the next native render without waiting for FG to
    // finish, but must not let it overtake a still-unsubmitted FG job.
    if(on_submitted)on_submitted(submitted_context);
    const auto wait_begin=TimingClock::now();const bool complete=wait_idle();completion_wait.add(elapsed_ms(wait_begin));if(!complete)return false;
    direct_publication=direct;gpu_timings.collect(focused,copies&&!direct);
    // Existing timestamp data only: distinguish consistently expensive work
    // from occasional long elapsed intervals without changing the GPU schedule.
    if(trace&&gpu_timings.valid&&(gpu_timings.samples%120)==0) {
        const auto& prep=gpu_timings.preparation;char message[384]{};
        sprintf_s(message,"FG preparation GPU ms: avg=%.3f median=%.3f p95=%.3f max=%.3f left_avg=%.3f right_avg=%.3f; processing_median=%.3f processing_p95=%.3f",
            prep.mean(),prep.percentile(50),prep.percentile(95),prep.percentile(100),
            gpu_timings.eye_preparation[0].mean(),gpu_timings.eye_preparation[1].mean(),
            gpu_timings.total.percentile(50),gpu_timings.total.percentile(95));trace(message);
        for(unsigned first=0;first<2;++first)if(gpu_timings.order_samples[first]) {
            const auto& prep=gpu_timings.preparation_by_first[first];
            const auto& eye=gpu_timings.eye_preparation_by_first[first];
            const auto& center=gpu_timings.center_dispatch_by_first[first];
            sprintf_s(message,"FG preparation order GPU ms: first=%s frames=%llu detail=%llu; prep_avg=%.3f prep_p95=%.3f left_avg=%.3f right_avg=%.3f; sampled_center_left=%.3f sampled_center_right=%.3f",
                first?"right":"left",static_cast<unsigned long long>(gpu_timings.order_samples[first]),
                static_cast<unsigned long long>(gpu_timings.order_detail_samples[first]),prep.mean(),prep.percentile(95),
                eye[0].mean(),eye[1].mean(),center[0].mean(),center[1].mean());trace(message);
        }
        if(gpu_timings.preparation_samples) {
            sprintf_s(message,"FG preparation sampled GPU ms: samples=%llu (1 in 17); avg=%.3f median=%.3f p95=%.3f; gaps_avg=%.3f",
                static_cast<unsigned long long>(gpu_timings.preparation_samples),gpu_timings.sampled_preparation.mean(),
                gpu_timings.sampled_preparation.percentile(50),gpu_timings.sampled_preparation.percentile(95),gpu_timings.preparation_gaps.mean());trace(message);
            for(unsigned pass=0;pass<4;++pass) {
                const auto& part=gpu_timings.preparation_parts[pass];const auto& input=focus.inputs[pass/2][pass%2];
                const auto guide=input.motion?input.motion->GetDesc():D3D12_RESOURCE_DESC{};
                sprintf_s(message,"FG preparation stage GPU ms: eye=%s pass=%s color=%ux%u guides=%llux%u; before/dispatch/after avg=%.3f/%.3f/%.3f last=%.3f/%.3f/%.3f p95=%.3f/%.3f/%.3f",
                    pass<2?"left":"right",pass%2?"periphery":"center",input.color?input.output_width:0,input.color?input.output_height:0,
                    static_cast<unsigned long long>(guide.Width),guide.Height,part[0].mean(),part[1].mean(),part[2].mean(),
                    part[0].last,part[1].last,part[2].last,part[0].percentile(95),part[1].percentile(95),part[2].percentile(95));trace(message);
            }
        }
    }
    bool enabled=true;
    for(unsigned eye=0;eye<2;++eye){
        focus.history[eye]=foveated[eye]&&guides.eyes[eye].peripheral_method==2;
        for(auto* f:{&features[eye],foveated[eye]&&guides.eyes[eye].peripheral_method==3?&peripheral_features[eye]:nullptr}) {
            if(!f)continue;
            if(!f->handle)continue;void* data{};if(FAILED(f->readback->Map(0,nullptr,&data))||!data)return false;
            enabled&=*static_cast<const unsigned char*>(data)==0;const D3D12_RANGE written{0,0};f->readback->Unmap(0,&written);
        }
    }
    return enabled;
}
}
