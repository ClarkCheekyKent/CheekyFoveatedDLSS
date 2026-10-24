#include "dlssg_worker.hpp"
#include "fg_ngx_capture.hpp"
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
NgxEvalBurst ngx_burst;
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
    return wait_fence(serial);
}
bool Worker::wait_fence(std::uint64_t value) noexcept {
    if(!device||!fence||!value)return true;
    while(fence->GetCompletedValue()<value) {
        if(FAILED(device->GetDeviceRemovedReason()))return false;
        if(event&&SUCCEEDED(fence->SetEventOnCompletion(value,event)))WaitForSingleObject(event,100);
        else Sleep(1);
    }
    return SUCCEEDED(device->GetDeviceRemovedReason());
}
void Worker::reset_features() noexcept {
    temporal_evaluation_completed=false;
    for(auto* array:{&features,&peripheral_features})for(auto& f:*array){
        if(f.handle&&release)release(f.handle);if(f.params&&destroy)destroy(f.params);f={};
    }
    focus.reset();configuration=0;center_width=center_height=peripheral_width=peripheral_height=0;
}
void Worker::reset() noexcept {
    std::lock_guard lock(ngx_mutex);wait_idle();reset_features();
    latency.reset();gpu_timings.reset();completion_wait={};submitted_at={};
    if(event)CloseHandle(event);event=nullptr;device.Reset();queue.Reset();allocator.Reset();second_allocator.Reset();list.Reset();second_list.Reset();fence.Reset();
    allocate=nullptr;destroy=nullptr;create=nullptr;evaluate=nullptr;release=nullptr;
    configured=false;serial=prepared_serial=0;width=height=0;init=nullptr;capabilities=nullptr;
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
    max_generated_frames=1;
    unsigned maximum{};
    if(ok(caps->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,&maximum)))max_generated_frames=std::max(1u,maximum);
    destroy(caps);
    capability_ready=ok(available_result)&&available!=0;failed=!capability_ready;
    sprintf_s(diagnostic,"NGX FG available=%d query=0x%08x feature=0x%08x needs_driver=%d minimum_driver=%u.%u",
        available,unsigned(available_result),feature_result,needs_driver,major,minor);
    if(trace)trace(diagnostic);
    return capability_ready;
}
bool Worker::initialize(ID3D12Device* dev,ID3D12CommandQueue* q,unsigned w,unsigned h,DXGI_FORMAT fmt,bool require_ngx) noexcept {
    std::lock_guard lock(ngx_mutex);
    if(configured&&dev==device.Get()&&q==queue.Get()&&w==width&&h==height&&fmt==format&&require_ngx==ngx_required)return true;
    reset();
    if(!dev||!q||!w||!h)return false;
    if(require_ngx&&!probe(dev))return false;
    ngx_required=require_ngx;failed=false;
    if(!require_ngx)strcpy_s(diagnostic,"Shader FG: D3D12 compute (no NVIDIA inference)");
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
    // Mode comes from the UEVR Lua settings attached to the guide pair.
    // Until the first pair arrives the worker remains in normal mode.
    if(trace)trace("FG diagnostic mode=normal (awaiting UEVR Lua setting; FG queue retained)");
    // Optional telemetry; unsupported queries must never disable generation.
    gpu_timings.initialize(dev,q);
    configured=true;return true;
}
bool Worker::generate(const CheekyFgPair& guides,const std::array<ID3D12Resource*,2>& colors,
    const std::array<ID3D12Resource*,2>& previous,const std::array<Camera,2>& cameras,
    std::array<ID3D12Resource*,2>& outputs,std::array<OutputCopy,2>* copies,
    void(*on_submitted)(void*) noexcept,void* submitted_context,void(*on_prepared)(void*) noexcept,
    unsigned frame_count,unsigned frame_index) noexcept {
    submitted_at={};prepared_serial=0;temporal_evaluation_completed=false;
    std::lock_guard lock(ngx_mutex);
    if(frame_count<1||frame_count>3||frame_index<1||frame_index>frame_count)return false;
    if(cheeky_fg_needs_ngx(guides)&&frame_count>max_generated_frames){
        sprintf_s(diagnostic,"DLSS %ux FG unavailable: this GPU/runtime supports at most %ux; select a supported multiplier or shader methods",
            frame_count+1,max_generated_frames+1);if(trace)trace(diagnostic);return false;
    }
    const float fraction=float(frame_index)/float(frame_count+1);
    const bool last_output=frame_index==frame_count;
    if(!configured||(cheeky_fg_needs_ngx(guides)&&!ngx_required)||guides.eyes[0].center_method>2||guides.eyes[1].center_method>2||guides.producer_queue!=queue.Get()||!colors[0]||!colors[1]||!previous[0]||!previous[1]||!cameras[0].valid||!cameras[1].valid)return false;
    if(!wait_idle())return false;
    ngx_burst.trace=trace;
    ngx_burst.start(guides.render_frame,guides.capture_token);
    if(configuration!=guides.eyes[0].configuration) {
        const bool previously_failed=failed;
        reset_features();configuration=guides.eyes[0].configuration;gpu_timings.initialize(device.Get(),queue.Get());completion_wait={};
        // A quality-mode transition can briefly invalidate the game's guides.
        // A new, validated stereo pair is a safe point to recreate FG features
        // and clear a previous transient NGX error. Do not mask device removal.
        if(device&&SUCCEEDED(device->GetDeviceRemovedReason()))failed=false;
        if(trace) {
            char message[192]{};
            sprintf_s(message,"FG NGX source/configuration changed: rebuilding FG features, transient_error_cleared=%u",
                unsigned(previously_failed&&!failed));
            trace(message);
        }
    }
    focus.color_layouts=color_layouts;focus.previous_layouts=previous_layouts;
    focus.interpolation_fraction=fraction;
    if(FAILED(allocator->Reset())||FAILED(list->Reset(allocator.Get(),nullptr)))return false;
    // Release point 1 records both eyes' preparation on list A and NGX and
    // composition on list B, with a GPU milestone between them.
    const bool two_lists=guides.release_point==1u;
    // Lazily allocate the second list. Both allocators are reused only after
    // the previous frame's final fence retires.
    if(two_lists) {
        if(!second_allocator && FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&second_allocator)))) {
            if(trace)trace("FG preparation gate: cannot allocate second command allocator");
            list->Close();return false;
        }
        if(!second_list) {
            if(FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,
                    second_allocator.Get(),nullptr,IID_PPV_ARGS(&second_list)))) {
                if(trace)trace("FG preparation gate: cannot create second command list");
                list->Close();return false;
            }
            if(FAILED(second_list->Close())) {list->Close();return false;}
        }
        if(FAILED(second_allocator->Reset())||FAILED(second_list->Reset(second_allocator.Get(),nullptr))) {
            if(trace)trace("FG preparation gate: cannot reset second command list");
            list->Close();return false;
        }
    }
    // The lambdas and timestamp recording follow the active command list.
    // A mode switch is safe because the previous job was fully retired above.
    ID3D12GraphicsCommandList* commands=list.Get();
    bool cut_to_second_list=false;
    latency.begin(commands);gpu_timings.begin(commands);
    bool good=true;unsigned focused{};
    const bool crop_diagnostic=trace&&((++crop_diagnostic_frame%60u)==1u);
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
        direct=(composition||(*copies)[eye].layout.packed())&&d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width==((*copies)[eye].layout.packed()?(*copies)[eye].layout.stored_width():width)&&d.Height==((*copies)[eye].layout.packed()?(*copies)[eye].layout.stored_height():height)&&
            d.DepthOrArraySize==1&&d.MipLevels==1&&d.SampleDesc.Count==1&&writable_color(d.Format,focus.color_format)&&
            (d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)&&(!(*copies)[eye].final_xr||(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET));
        for(unsigned source=0;source<2&&direct;++source)
            direct=(*copies)[eye].texture!=colors[source]&&(*copies)[eye].texture!=previous[source]&&
                (*copies)[eye].texture!=guides.eyes[source].depth&&(*copies)[eye].texture!=guides.eyes[source].motion;
    }
    if(copies&&!direct&&((*copies)[0].layout.packed()||(*copies)[1].layout.packed()||(*copies)[0].final_xr||(*copies)[1].final_xr)){list->Close();if(two_lists)second_list->Close();return false;}
    // wait_idle above retired every earlier use of these private scratch images.
    if(direct&&direct_periphery)for(unsigned eye=0;eye<2;++eye)
        if(((*copies)[eye].layout.packed()||(*copies)[eye].final_xr)&&guides.eyes[eye].peripheral_method<=2){
            focus.peripheral_output[eye].Reset();if((*copies)[eye].final_xr)focus.full[eye].Reset();}
    std::array<bool,2> moving_crop_reset{};
    std::array<FocusRect,2> previous_center_crops{};
    std::array<float,2> motion_crop_dx{},motion_crop_dy{};
    auto transpose=[](Matrix m){Matrix result{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)result.m[r*4+c]=m.m[c*4+r];return result;};
    auto configure_pass=[&](Feature& f,const FocusInput& input,const Camera& camera,const CheekyFgEye& g,const FocusRect& rect,unsigned eye=0,bool center_pass=true) {
        const auto desc=input.color->GetDesc();const auto tw=unsigned(desc.Width),th=desc.Height;
        const auto guide_desc=input.depth->GetDesc();const auto gw=unsigned(guide_desc.Width),gh=guide_desc.Height;
        if(f.width!=tw||f.height!=th) {
#ifdef CHEEKY_FG_LIFETIME_DIAGNOSTICS
            if(trace) {
                char message[384]{};
                sprintf_s(message,"FG-FEATURE resize worker=%p feature=%p eye=%u pass=%s old=%ux%u new=%ux%u old_handle=%p format=%u guide=%ux%u config=%llu crop=%u,%u,%u,%u",
                    this,&f,eye,center_pass?"center":"periphery",f.width,f.height,tw,th,f.handle,unsigned(desc.Format),gw,gh,
                    static_cast<unsigned long long>(configuration),rect.x,rect.y,rect.w,rect.h);
                trace(message);
            }
#endif
            if(f.handle)release(f.handle);if(f.params)destroy(f.params);f={};f.width=tw;f.height=th;
        }
        f.eye=eye;f.offset_x=f.offset_y=0;f.direct=false;
        const bool final_xr=direct&&copies&&(*copies)[eye].final_xr;
        // With crop motion, NGX produces a view between the two crop origins.
        // It must be resampled before publication, so it cannot write directly
        // into the atlas/XR destination that represents the current origin.
        const bool recenter_output=center_pass&&(motion_crop_dx[eye]!=0.f||motion_crop_dy[eye]!=0.f);
        if(!recenter_output&&direct&&copies&&((*copies)[eye].layout.packed()||final_xr)&&!(final_xr?f.xr_direct_rejected:f.direct_rejected)){const auto& destination=(*copies)[eye];const auto d=destination.texture->GetDesc();
            const auto& layout=destination.layout;
            f.offset_x=final_xr?rect.x:0;f.offset_y=final_xr?rect.y:layout.packed()&&!center_pass?layout.center_height:0;
            f.direct=writable_color(d.Format,desc.Format)&&(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)&&
                (final_xr?(tw==rect.w&&th==rect.h):layout.packed()?(tw==(center_pass?layout.center_width:layout.context_width)&&th==(center_pass?layout.center_height:layout.context_height)):
                    (tw==d.Width&&th==d.Height));
        }
        if((!f.disable&&!buffer(device.Get(),8,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON,f.disable))||
           (!f.readback&&!buffer(device.Get(),8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,f.readback))||
           (!f.direct&&!f.output&&!texture(device.Get(),input.color.Get(),tw,th,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,f.output))) {
            report("NGX FG output allocation",unsigned(E_FAIL));failed=true;return false;
        }
        f.target=f.direct?(*copies)[eye].texture:f.output.Get();
        if(f.direct)f.output.Reset(); // Prior GPU work retired before configure_pass.
        if(!f.params) {const auto result=allocate(&f.params);if(!ok(result)||!f.params){report("NGX FG parameters",result);failed=true;return false;}}
        bool created=false;auto* p=f.params;
        p->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount,frame_count);
        p->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex,frame_index);
        if(!f.handle) {
            p->Set("CreationNodeMask",1u);p->Set("VisibilityNodeMask",1u);p->Set("Width",tw);p->Set("Height",th);
            p->Set("DLSSG.BackbufferFormat",unsigned(desc.Format));p->Set("DLSSG.InternalWidth",gw);p->Set("DLSSG.InternalHeight",gh);p->Set("DLSSG.DynamicResolution",1u);
            const auto result=create(commands,NVSDK_NGX_Feature_FrameGeneration,p,&f.handle);report("NGX FG CreateFeature",result);
#ifdef CHEEKY_FG_LIFETIME_DIAGNOSTICS
            if(trace) {
                char message[256]{};
                sprintf_s(message,"FG-FEATURE create worker=%p feature=%p eye=%u pass=%s size=%ux%u handle=%p result=0x%08X",
                    this,&f,eye,center_pass?"center":"periphery",tw,th,f.handle,unsigned(result));trace(message);
            }
#endif
            if(!ok(result)||!f.handle){failed=true;return false;}created=true;
        }
        Matrix crop=identity(),uncrop=identity(),previous_crop=identity();
        if(rect.active) {
            crop.m[0]=float(width)/rect.w;crop.m[5]=float(height)/rect.h;
            crop.m[3]=(float(width)-2*rect.x-rect.w)/rect.w;crop.m[7]=-(float(height)-2*rect.y-rect.h)/rect.h;
            if(!inverse(crop,uncrop))return false;
            if(center_pass&&!moving_crop_reset[eye]){
                const auto& before=previous_center_crops[eye];
                previous_crop.m[0]=float(width)/before.w;
                previous_crop.m[5]=float(height)/before.h;
                previous_crop.m[3]=(float(width)-2*before.x-before.w)/before.w;
                previous_crop.m[7]=-(float(height)-2*before.y-before.h)/before.h;
            }else previous_crop=crop;
        }
        auto projection=multiply(crop,camera.view_to_clip),clip_prev=multiply(multiply(previous_crop,camera.clip_to_prev),uncrop);
        if(!finite_matrix(projection)||!finite_matrix(clip_prev))return false;
        if(center_pass&&crop_diagnostic){
            char message[640]{};
            sprintf_s(message,"FG CROP-DIAG frame=%llu eye=%u active=%u eye_wh=%ux%u crop_xywh=%u,%u,%u,%u "
                "color_in=%ux%u mv=%ux%u depth=%ux%u jitter=%.5f,%.5f "
                "crop_scale=%.5f,%.5f crop_shift=%.5f,%.5f clip_prev_shift=%.5f,%.5f reset=%u",
                static_cast<unsigned long long>(guides.render_frame),eye,unsigned(rect.active),
                width,height,rect.x,rect.y,rect.w,rect.h,tw,th,g.motion_width,g.motion_height,
                g.depth_width,g.depth_height,g.jitter_x,g.jitter_y,
                crop.m[0],crop.m[5],crop.m[3],crop.m[7],clip_prev.m[3],clip_prev.m[7],g.reset);
            trace(message);
        }
        Matrix unprojection{},prev_clip{};if(!inverse(projection,unprojection)||!inverse(clip_prev,prev_clip))return false;
        // SDK matrices use row vectors. Our camera math uses column vectors.
        // NGX parameter maps retain matrix pointers rather than copying their
        // contents. Preparation and evaluation may be separate phases/lists.
        f.view_to_clip=transpose(projection);f.clip_to_camera=transpose(unprojection);
        f.clip_to_prev=transpose(clip_prev);f.prev_clip_to_clip=transpose(prev_clip);f.lens=identity();
        p->Set("DLSSG.CameraViewToClip",f.view_to_clip.m);p->Set("DLSSG.ClipToCameraView",f.clip_to_camera.m);
        p->Set("DLSSG.ClipToLensClip",f.lens.m);p->Set("DLSSG.ClipToPrevClip",f.clip_to_prev.m);p->Set("DLSSG.PrevClipToClip",f.prev_clip_to_clip.m);
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
        p->Set("DLSSG.Reset",g.reset||created||(center_pass&&moving_crop_reset[eye])?1u:0u);
        p->Set("DLSSG.Backbuffer",input.color.Get());p->Set("DLSSG.Depth",input.depth.Get());p->Set("DLSSG.MVecs",input.motion.Get());
        p->Set("DLSSG.OutputInterpolated",f.target);p->Set("DLSSG.OutputDisableInterpolation",f.disable.Get());
        for(const char* key:{"DLSSG.BackbufferSubrectBaseX","DLSSG.BackbufferSubrectBaseY","DLSSG.InputBackbufferSubrectBaseX","DLSSG.InputBackbufferSubrectBaseY",
            "DLSSG.DepthSubrectBaseX","DLSSG.DepthSubrectBaseY","DLSSG.MVecsSubrectBaseX","DLSSG.MVecsSubrectBaseY","DLSSG.OutputInterpolatedSubrectBaseX","DLSSG.OutputInterpolatedSubrectBaseY"})p->Set(key,0u);
        p->Set("DLSSG.OutputInterpolatedSubrectBaseY",f.direct?f.offset_y:0u);
        p->Set("DLSSG.OutputInterpolatedSubrectBaseX",f.direct?f.offset_x:0u);
        p->Set("DLSSG.EvalFlags",f.direct?unsigned(NVSDK_NGX_DLSSG_EvalFlags_UpdateOnlyInsideExtents):0u);
        for(const char* key:{"DLSSG.BackbufferSubrectWidth","DLSSG.InputBackbufferSubrectWidth","DLSSG.OutputInterpolatedSubrectWidth"})p->Set(key,tw);
        for(const char* key:{"DLSSG.BackbufferSubrectHeight","DLSSG.InputBackbufferSubrectHeight","DLSSG.OutputInterpolatedSubrectHeight"})p->Set(key,th);
        for(const char* key:{"DLSSG.DepthSubrectWidth","DLSSG.MVecsSubrectWidth"})p->Set(key,gw);
        for(const char* key:{"DLSSG.DepthSubrectHeight","DLSSG.MVecsSubrectHeight"})p->Set(key,gh);
        return true;
    };
    auto dispatch_pass=[&](Feature& f,unsigned eye,bool center_pass) {
        // Detailed GPU timestamp boundaries: pre-UAV transitions, NGX commands,
        // post-UAV transitions and disable-interpolation readback copy.
        if(center_pass)gpu_timings.ngx_stamp(commands,eye,0);
        barrier(commands,f.target,f.direct?(*copies)[f.eye].state:D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(commands,f.disable.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if(center_pass)gpu_timings.ngx_stamp(commands,eye,1);
        if(ngx_burst.active){
            const unsigned pass=center_pass?0u:1u;
            const auto& inputs=focus.inputs[eye][pass];
            ngx_burst.record(device.Get(),commands,inputs.color.Get(),D3D12_RESOURCE_STATE_COMMON,"input_color",eye,pass);
            ngx_burst.record(device.Get(),commands,inputs.depth.Get(),D3D12_RESOURCE_STATE_COMMON,"input_depth",eye,pass);
            ngx_burst.record(device.Get(),commands,inputs.motion.Get(),D3D12_RESOURCE_STATE_COMMON,"input_motion",eye,pass);
        }
        auto result=evaluate(commands,f.handle,f.params,nullptr);
        if(!ok(result)&&f.direct){
            // Some NGX versions reject subrect/shared destinations. Keep only a
            // compact private fallback, allocated on demand after rejection.
            if((*copies)[eye].final_xr)f.xr_direct_rejected=true;else f.direct_rejected=true;
            report("NGX direct output rejected; using compact scratch",result);
            D3D12_RESOURCE_BARRIER uav{};uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;uav.UAV.pResource=f.target;commands->ResourceBarrier(1,&uav);
            barrier(commands,f.target,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);(*copies)[f.eye].state=D3D12_RESOURCE_STATE_COMMON;
            uav.UAV.pResource=f.disable.Get();commands->ResourceBarrier(1,&uav);
            if(!f.output&&!texture(device.Get(),focus.inputs[f.eye][center_pass?0:1].color.Get(),f.width,f.height,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,f.output)){failed=true;return false;}
            f.direct=false;f.target=f.output.Get();f.params->Set("DLSSG.OutputInterpolated",f.target);
            f.params->Set("DLSSG.OutputInterpolatedSubrectBaseY",0u);f.params->Set("DLSSG.OutputInterpolatedSubrectBaseX",0u);
            f.params->Set("DLSSG.EvalFlags",0u);f.params->Set("DLSSG.Reset",1u);
            barrier(commands,f.target,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            result=evaluate(commands,f.handle,f.params,nullptr);
        }
        if(center_pass)gpu_timings.ngx_stamp(commands,eye,2);
        if(!ok(result)){report("NGX FG EvaluateFeature",result);failed=true;return false;}
        barrier(commands,f.target,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        if(f.direct)(*copies)[f.eye].state=D3D12_RESOURCE_STATE_COMMON;
        if(ngx_burst.active)
            ngx_burst.record(device.Get(),commands,f.target,D3D12_RESOURCE_STATE_COMMON,
                "output_interpolated",eye,center_pass?0u:1u,f.offset_x,f.offset_y,f.width,f.height,f.direct);
        barrier(commands,f.disable.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->CopyBufferRegion(f.readback.Get(),0,f.disable.Get(),0,4);
        barrier(commands,f.disable.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        if(center_pass)gpu_timings.ngx_stamp(commands,eye,3);
        return true;
    };

    auto evaluate_pass=[&](Feature& f,const FocusInput& input,const Camera& camera,const CheekyFgEye& g,const FocusRect& rect,unsigned eye) {
        return configure_pass(f,input,camera,g,rect,eye,false)&&dispatch_pass(f,eye,false);
    };
    bool foveated[2]{};
    std::array<FocusRect,2> center_rects{};
    std::array<unsigned,2> center_output_width{},center_output_height{};
    // Ordering is preparation -> NGX -> composition per eye. With the
    // preparation gate both guides are prepared first, then both center NGX
    // features are evaluated, then both eyes are composed.
    const bool prepare_first=two_lists;
    for(unsigned phase=0;phase<(prepare_first?3u:1u)&&good;++phase) {
        for(unsigned step=0;step<2&&good;++step) {
            const unsigned eye=(gpu_timings.first_eye+step)%2;
            const auto& g=guides.eyes[eye];const unsigned query=2+eye*6;
            const Camera& camera=cameras[eye];
            if(!prepare_first||phase==0) {
                gpu_timings.stamp(commands,query);
                if(!g.depth||!g.motion||g.width!=width||g.height!=height){report("FG guide/output dimensions disagree",unsigned(E_INVALIDARG));failed=true;good=false;break;}
                source_depth_format=unsigned(g.depth->GetDesc().Format);source_motion_format=unsigned(g.motion->GetDesc().Format);
                if(depth_srv_format(DXGI_FORMAT(source_depth_format))==DXGI_FORMAT_UNKNOWN||motion_srv_format(DXGI_FORMAT(source_motion_format))==DXGI_FORMAT_UNKNOWN){
                    sprintf_s(diagnostic,"FG guide conversion unsupported: depth=%u motion=%u; displaying native frames",source_depth_format,source_motion_format);
                    if(trace)trace(diagnostic);failed=true;good=false;break;
                }
                FocusRect full_rect{0,0,width,height,0,false},rect=full_rect;
                rect.active=g.focus_width<.999f||g.focus_height<.999f;rect.feather=g.focus_feather;
                if(rect.active) {
                    rect.w=std::clamp(unsigned(std::round(width*g.focus_width)),1u,width);
                    rect.h=std::clamp(unsigned(std::round(height*g.focus_height)),1u,height);
                    rect.x=unsigned(std::clamp((width-rect.w)*.5f+g.focus_x*width,0.f,float(width-rect.w)));
                    rect.y=unsigned(std::clamp((height-rect.h)*.5f+g.focus_y*height,0.f,float(height-rect.h)));
                    ++focused;
                }
                center_rects[eye]=rect;foveated[eye]=rect.active;peripheral_method=g.peripheral_method;
                const auto& last=features[eye];
                // An evaluation that skipped NGX cannot provide a previous crop.
                // A non-overlapping jump has no useful temporal pixels to reuse.
                const bool compatible=last.evaluated_crop_valid&&last.evaluated_crop.active==rect.active&&
                    last.evaluated_crop.w==rect.w&&last.evaluated_crop.h==rect.h&&
                    (!rect.active||(std::abs(double(rect.x)-last.evaluated_crop.x)<rect.w&&
                                    std::abs(double(rect.y)-last.evaluated_crop.y)<rect.h));
                const bool frame_backwards=guides.render_frame&&last.evaluated_frame&&
                    guides.render_frame<=last.evaluated_frame;
                moving_crop_reset[eye]=g.reset||!compatible||frame_backwards;
                if(g.center_method==0&&!moving_crop_reset[eye]&&rect.active){
                    previous_center_crops[eye]=last.evaluated_crop;
                    motion_crop_dx[eye]=(float(rect.x)-last.evaluated_crop.x)/rect.w;
                    motion_crop_dy[eye]=(float(rect.y)-last.evaluated_crop.y)/rect.h;
                }
                center_output_width[eye]=scaled(rect.w,g.resolution_percent);
                center_output_height[eye]=scaled(rect.h,g.resolution_percent);
                center_width=center_output_width[eye];center_height=center_output_height[eye];
                peripheral_width=rect.active?scaled(width,g.peripheral_percent):0;
                peripheral_height=rect.active?scaled(height,g.peripheral_percent):0;
                auto* detail=gpu_timings.record_preparation?&gpu_timings:nullptr;
                if(!focus.prepare_inputs(device.Get(),commands,eye,0,colors[eye],g,rect,center_width,center_height,input_color_state,detail,motion_crop_dx[eye],motion_crop_dy[eye])) {
                    report("FG center guide conversion",unsigned(E_FAIL));failed=true;good=false;break;
                }
                if(rect.active&&!focus.prepare_inputs(device.Get(),commands,eye,1,colors[eye],g,full_rect,peripheral_width,peripheral_height,input_color_state,detail)) {
                    report("FG periphery guide conversion",unsigned(E_FAIL));failed=true;good=false;break;
                }
                if(!rect.active)for(unsigned part=0;part<4;++part)gpu_timings.preparation_stamp(commands,eye,1,part);
                // Feature creation can record GPU commands. With the preparation
                // gate, configure both feature instances before either evaluates.
                if(prepare_first&&g.center_method==0&&!configure_pass(features[eye],focus.inputs[eye][0],camera,g,rect,eye)) {
                    good=false;break;
                }
                gpu_timings.stamp(commands,query+1);
            }
            if(prepare_first&&phase==0)continue;
            const auto& rect=center_rects[eye];
            if(!prepare_first||phase==1) {
                gpu_timings.stamp(commands,query+2);
                if(g.center_method) {
                    const auto& last=features[eye];
                    focus.center_history[eye]=focus.center_history[eye]&&!g.reset&&last.evaluated_crop_valid&&
                        last.evaluated_crop.x==rect.x&&last.evaluated_crop.y==rect.y&&last.evaluated_crop.w==rect.w&&last.evaluated_crop.h==rect.h;
                    good=focus.generate_periphery(device.Get(),commands,eye,previous[eye],colors[eye],g,camera,input_color_state,
                        nullptr,D3D12_RESOURCE_STATE_COMMON,nullptr,nullptr,&rect);
                    for(unsigned mark=0;mark<4;++mark)gpu_timings.ngx_stamp(commands,eye,mark);
                } else {
                    good=prepare_first?dispatch_pass(features[eye],eye,true):
                        (configure_pass(features[eye],focus.inputs[eye][0],camera,g,rect,eye)&&dispatch_pass(features[eye],eye,true));
                }
                gpu_timings.stamp(commands,query+3);
                if(!good)break;
            }
            if(prepare_first&&phase==1)continue;
            gpu_timings.stamp(commands,query+4);
            auto* center=g.center_method?focus.center_output[eye].Get():features[eye].target;
            if(motion_crop_dx[eye]!=0.f||motion_crop_dy[eye]!=0.f){
                // For a static full-eye point P and crop origins A -> B, NGX
                // interpolates (P-A)/size -> (P-B)/size. Its output at t belongs
                // at lerp(A,B,t). Sampling at uv+(1-t)*(B-A)/size re-expresses that
                // image in B without changing the real scene's interpolation.
                if(!focus.recenter_frame(device.Get(),commands,eye,center,(1-fraction)*motion_crop_dx[eye],(1-fraction)*motion_crop_dy[eye])){
                    report("FG moving-crop output recenter",unsigned(E_FAIL));failed=true;good=false;break;
                }
                center=focus.inputs[eye][0].recentered_color.Get();
            }
            auto* base=center;
            bool custom_context_direct=false;
            const bool final_xr=direct&&(*copies)[eye].final_xr;
            if(final_xr){
                FocusPipeline::FinalOutput final{center,rect,width,height,features[eye].direct,
                    focus.color_format==DXGI_FORMAT_R8G8B8A8_UNORM&&srgb_color(format)};
                good=focus.generate_periphery(device.Get(),commands,eye,previous[eye],colors[eye],g,camera,input_color_state,
                    (*copies)[eye].texture,(*copies)[eye].state,nullptr,&final);
                (*copies)[eye].state=D3D12_RESOURCE_STATE_COMMON;outputs[eye]=(*copies)[eye].texture;
            }else if(rect.active) {
                if(g.peripheral_method==3) {
                    good=evaluate_pass(peripheral_features[eye],focus.inputs[eye][1],camera,g,FocusRect{0,0,width,height,0,false},eye);
                    base=peripheral_features[eye].target;
                }else {
                    custom_context_direct=direct&&direct_periphery&&(*copies)[eye].layout.packed();
                    auto* target=custom_context_direct?(*copies)[eye].texture:nullptr;
                    good=focus.generate_periphery(device.Get(),commands,eye,previous[eye],colors[eye],g,camera,input_color_state,
                        target,custom_context_direct?(*copies)[eye].state:D3D12_RESOURCE_STATE_COMMON,
                        custom_context_direct?&(*copies)[eye].layout:nullptr);
                    base=custom_context_direct?target:focus.peripheral_output[eye].Get();
                    if(good&&custom_context_direct)(*copies)[eye].state=D3D12_RESOURCE_STATE_COMMON;
                }
            }
            if(final_xr){}else if(good&&copies&&(*copies)[eye].layout.packed()) {
                const bool center_direct=features[eye].direct;
                const bool context_direct=custom_context_direct||(g.peripheral_method==3&&peripheral_features[eye].direct);
                good=focus.pack_frame(device.Get(),commands,eye,center,base,(*copies)[eye].texture,(*copies)[eye].state,
                    (*copies)[eye].layout,center_direct,context_direct);
                outputs[eye]=(*copies)[eye].texture;(*copies)[eye].state=D3D12_RESOURCE_STATE_COMMON;
            }else if(good&&(rect.active||center_output_width[eye]!=width||center_output_height[eye]!=height)) {
                good=focus.compose_frame(device.Get(),commands,eye,center,base,rect,width,height,
                    direct?(*copies)[eye].texture:nullptr,direct?(*copies)[eye].state:D3D12_RESOURCE_STATE_COMMON);
                outputs[eye]=direct?(*copies)[eye].texture:focus.full[eye].Get();
                if(direct)(*copies)[eye].state=D3D12_RESOURCE_STATE_GENERIC_READ;
            } else outputs[eye]=base;
            if(good&&last_output&&rect.active&&g.peripheral_method==2)good=focus.save_depth(device.Get(),commands,eye);
            if(good&&last_output&&g.center_method==2)good=focus.save_depth(device.Get(),commands,eye,0);
            if(!good&&!failed){report("FG periphery/composition allocation",unsigned(E_FAIL));failed=true;}
            gpu_timings.stamp(commands,query+5);
        }
        if(good&&two_lists&&phase==0) {
            // Both eyes' GPU preparation is on A; NGX/composition is on B.
            // Record B before submitting either list to avoid a CPU-created
            // gap between preparation and inference.
            if(FAILED(commands->Close())) {good=false;break;}
            commands=second_list.Get();cut_to_second_list=true;
        }
    }
    if(good&&direct)for(auto& copy:*copies){
        const auto state=copy.final_xr?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_GENERIC_READ;
        barrier(commands,copy.texture,copy.state,state);copy.state=state;
    }
    if(good)gpu_timings.stamp(commands,1);
    if(good)gpu_timings.stamp(commands,14);
    // Publish immutable output in the SAME GPU submission as inference. A copy
    // submitted only after the CPU completion wait can land behind a newer game
    // frame and delay a finished interpolation by another render interval.
    if(good&&copies&&!direct&&!record_output_copies(commands,outputs,*copies)) {
        report("FG immutable output copy",unsigned(E_INVALIDARG));failed=true;good=false;
    }
    if(good){gpu_timings.stamp(commands,15);gpu_timings.resolve(commands);latency.end(commands);}
    // No list is submitted on failure. That guarantees the caller may safely
    // release captured resources without an untracked in-flight GPU packet.
    const HRESULT close_result=commands->Close();
    if(FAILED(close_result)||!good) {
        ngx_burst.abandon();
        if(FAILED(close_result))report("FG command list close",unsigned(close_result));
        // A failure before the cut leaves list B recording. Close it too
        // before reusing its allocator on the next frame. Neither list has
        // been submitted, so nothing may still reference captured inputs.
        if(two_lists && !cut_to_second_list && second_list)second_list->Close();
        return false;
    }
    if(two_lists && cut_to_second_list) {
        // Two actual ExecuteCommandLists calls on the SAME HIGH-priority queue,
        // strictly ordered, no CPU fence wait and no inter-list GPU signal.
        ID3D12CommandList* first[]{list.Get()};
        ID3D12CommandList* second[]{second_list.Get()};
        queue->ExecuteCommandLists(1,first);
        prepared_serial=++serial;
        if(FAILED(queue->Signal(fence.Get(),prepared_serial)))return false;
        queue->ExecuteCommandLists(1,second);
        if(trace && gpu_timings.samples==0)trace("FG preparation gate: both eyes on list A; GPU milestone before NGX list B");
    } else {
        ID3D12CommandList* lists[]{commands};queue->ExecuteCommandLists(1,lists);
    }
    if(copies)for(auto& copy:*copies)copy.state=copy.final_xr?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_GENERIC_READ;
    if(FAILED(queue->Signal(fence.Get(),++serial))){ngx_burst.abandon();return false;}
    ngx_burst.commit(fence.Get(),serial);
    // Only submitted evaluations advance NGX history. The pair can fail after
    // recording EvaluateFeature (e.g. during final composition), in which case
    // the command list is discarded and these crops must not be published.
    for(unsigned eye=0;last_output&&eye<2;++eye){
        auto& done=features[eye];
        done.evaluated_crop=center_rects[eye];done.evaluated_crop_valid=true;
        done.evaluated_frame=guides.render_frame;
    }
    submitted_at=TimingClock::now();latency.submitted(fence.Get(),serial);
    // Notify only after commands AND their completion fence are queued. The
    // presenter may release the next native render without waiting for FG to
    // finish, but must not let it overtake a still-unsubmitted FG job.
    if(on_submitted)on_submitted(submitted_context);
    const auto wait_begin=TimingClock::now();
    if(two_lists) {
        if(!wait_fence(prepared_serial))return false;
        if(on_prepared)on_prepared(submitted_context);
    }
    const bool complete=wait_idle();const auto observed_cpu=LatencyProbe::cpu_now();
    completion_wait.add(elapsed_ms(wait_begin));if(!complete)return false;
    latency.collect(queue.Get(),observed_cpu);
    direct_publication=direct;gpu_timings.collect(focused,copies&&!direct);
    // Existing timestamp data only: distinguish consistently expensive work
    // from occasional long elapsed intervals without changing the GPU schedule.
    if(trace&&gpu_timings.valid&&(gpu_timings.samples%120)==0) {
        // The 4.2ms counter includes NGX's GPU commands and surrounding UAV
        // barriers/copy; the detailed queries isolate those three regions.
        for(unsigned eye=0;eye<2;++eye) {
            const auto& phase=gpu_timings.ngx_parts[eye];
            char detail_msg[512]{};
            sprintf_s(detail_msg,
                "FG NGX breakdown GPU ms: eye=%s samples=%llu "
                "before_avg=%.3f before_p95=%.3f ngx_avg=%.3f ngx_p50=%.3f ngx_p95=%.3f "
                "after_avg=%.3f after_p95=%.3f",
                eye?"right":"left",
                static_cast<unsigned long long>(gpu_timings.samples),
                phase[0].mean(),phase[0].percentile(95),
                phase[1].mean(),phase[1].percentile(50),phase[1].percentile(95),
                phase[2].mean(),phase[2].percentile(95));
            trace(detail_msg);
        }
        {
            const auto& first=gpu_timings.ngx_by_position[0];
            const auto& second=gpu_timings.ngx_by_position[1];
            char order_msg[480]{};
            sprintf_s(order_msg,
                "FG NGX breakdown order GPU ms: samples=%llu "
                "first_before=%.3f first_ngx=%.3f first_ngx_p95=%.3f first_after=%.3f "
                "second_before=%.3f second_ngx=%.3f second_ngx_p95=%.3f second_after=%.3f",
                static_cast<unsigned long long>(gpu_timings.samples),
                first[0].mean(),first[1].mean(),first[1].percentile(95),first[2].mean(),
                second[0].mean(),second[1].mean(),second[1].percentile(95),second[2].mean());
            trace(order_msg);
        }
        const auto& prep=gpu_timings.preparation;char message[384]{};
        sprintf_s(message,"FG preparation GPU ms: avg=%.3f median=%.3f p95=%.3f max=%.3f left_avg=%.3f right_avg=%.3f; processing_median=%.3f processing_p95=%.3f",
            prep.mean(),prep.percentile(50),prep.percentile(95),prep.percentile(100),
            gpu_timings.eye_preparation[0].mean(),gpu_timings.eye_preparation[1].mean(),
            gpu_timings.total.percentile(50),gpu_timings.total.percentile(95));trace(message);
        // Eye identity alone cannot identify a queue stall: evaluation order
        // alternates every generated stereo pair, while all eye inputs and
        // feature histories remain unchanged. GPU stamps have already been
        // resolved on the normal completion fence; this is CPU-only logging.
        {
            const auto& left=gpu_timings.eye_center[0];
            const auto& right=gpu_timings.eye_center[1];
            const auto& first=gpu_timings.center_by_position[0];
            const auto& second=gpu_timings.center_by_position[1];
            char eye_message[640]{};
            sprintf_s(eye_message,
                "FG NGX eye GPU ms: frames=%llu left_avg=%.3f left_p50=%.3f left_p95=%.3f left_max=%.3f "
                "right_avg=%.3f right_p50=%.3f right_p95=%.3f right_max=%.3f "
                "first_avg=%.3f first_p95=%.3f second_avg=%.3f second_p95=%.3f "
                "stereo_avg=%.3f stereo_p50=%.3f stereo_p95=%.3f",
                static_cast<unsigned long long>(gpu_timings.samples),
                left.mean(),left.percentile(50),left.percentile(95),left.percentile(100),
                right.mean(),right.percentile(50),right.percentile(95),right.percentile(100),
                first.mean(),first.percentile(95),second.mean(),second.percentile(95),
                gpu_timings.center.mean(),gpu_timings.center.percentile(50),gpu_timings.center.percentile(95));
            trace(eye_message);
        }
        for(unsigned first=0;first<2;++first)if(gpu_timings.order_samples[first]) {
            const auto& by_eye=gpu_timings.eye_center_by_first[first];
            char order_message[480]{};
            sprintf_s(order_message,
                "FG NGX order GPU ms: first=%s frames=%llu "
                "left_avg=%.3f left_p50=%.3f left_p95=%.3f "
                "right_avg=%.3f right_p50=%.3f right_p95=%.3f "
                "first_eye_avg=%.3f second_eye_avg=%.3f",
                first?"right":"left",
                static_cast<unsigned long long>(gpu_timings.order_samples[first]),
                by_eye[0].mean(),by_eye[0].percentile(50),by_eye[0].percentile(95),
                by_eye[1].mean(),by_eye[1].percentile(50),by_eye[1].percentile(95),
                by_eye[first].mean(),by_eye[1-first].mean());
            trace(order_message);
        }
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
        if(last_output){focus.history[eye]=foveated[eye]&&guides.eyes[eye].peripheral_method==2;
            focus.center_history[eye]=guides.eyes[eye].center_method==2;}
        for(auto* f:{&features[eye],foveated[eye]&&guides.eyes[eye].peripheral_method==3?&peripheral_features[eye]:nullptr}) {
            if(!f)continue;
            if(!f->handle)continue;void* data{};if(FAILED(f->readback->Map(0,nullptr,&data))||!data)return false;
            enabled&=*static_cast<const unsigned char*>(data)==0;const D3D12_RANGE written{0,0};f->readback->Unmap(0,&written);
        }
    }
    temporal_evaluation_completed=true;
    return enabled;
}
}
