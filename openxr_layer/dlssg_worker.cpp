#include "dlssg_worker.hpp"
#include "fg_ngx_capture.hpp"
#include "fg_color_format.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
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
bool Worker::wait_idle(const char* site) noexcept {
    return wait_fence(serial,site);
}
bool Worker::wait_fence(std::uint64_t value,const char* site) noexcept {
    if(!device||!fence||!value)return true;
    if(fence->GetCompletedValue()>=value)return SUCCEEDED(device->GetDeviceRemovedReason());
    const auto started=std::chrono::steady_clock::now();
    while(fence->GetCompletedValue()<value) {
        if(FAILED(device->GetDeviceRemovedReason()))return false;
        if(event&&SUCCEEDED(fence->SetEventOnCompletion(value,event)))WaitForSingleObject(event,100);
        else Sleep(1);
    }
    const double waited=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    (std::strcmp(site,"generation complete")==0?worker_wait_complete_ms:worker_wait_before_ms)+=waited;
    if(waited>5.&&trace&&(++long_cpu_waits<=30||long_cpu_waits%50==0)){char message[160]{};
        sprintf_s(message,"FG worker CPU wait: site=%s waited_ms=%.1f count=%llu",site,waited,static_cast<unsigned long long>(long_cpu_waits));trace(message);}
    return SUCCEEDED(device->GetDeviceRemovedReason());
}
void Worker::reset_features() noexcept {
    temporal_evaluation_completed=false;
    // Borrowed temporal handles belong to the first phase. Retire the whole
    // interval before destroying any phase's descriptors or parameters.
    for(auto& phase:ngx_phases){
        for(auto* array:{&phase.features,&phase.peripheral_features})for(auto& f:*array){
            if(f.handle&&!f.borrowed_handle&&release)release(f.handle);
            if(f.params&&destroy)destroy(f.params);f={};
        }
        phase.focus.reset();
    }
    for(auto* array:{&features,&peripheral_features})for(auto& f:*array){
        if(f.handle&&release)release(f.handle);if(f.params&&destroy)destroy(f.params);f={};
    }
    focus.reset();ngx_fallback.reset();configuration=0;center_width=center_height=peripheral_width=peripheral_height=0;
}
bool Worker::can_batch_ngx(const CheekyFgPair& guides) noexcept {
    // Mixed optical regions borrow phase 1's sessions and immutable flow
    // fields; NGX parameter/preparation storage remains private per phase.
    return cheeky_fg_needs_ngx(guides);
}
void Worker::reset() noexcept {
    std::lock_guard lock(ngx_mutex);wait_idle();reset_features();
    latency.reset();gpu_timings.reset();for(auto& t:phase_timings)t.reset();batch_timings.reset();completion_wait={};submitted_at={};
    if(event)CloseHandle(event);event=nullptr;device.Reset();queue.Reset();allocator.Reset();second_allocator.Reset();optical_allocator.Reset();list.Reset();second_list.Reset();optical_list.Reset();for(auto& a:batch_allocators)a.Reset();for(auto& l:batch_lists)l.Reset();fence.Reset();
    for(auto& a:batch_second_allocators)a.Reset();for(auto& l:batch_second_lists)l.Reset();
    for(auto& a:optical_consumer_allocators)a.Reset();for(auto& l:optical_consumer_lists)l.Reset();
    for(auto& eye:optical_pending)for(auto& pending:eye)pending=false;
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
    initialize_timings();
    configured=true;return true;
}
bool Worker::generate(const CheekyFgPair& guides,const std::array<ID3D12Resource*,2>& colors,
    const std::array<ID3D12Resource*,2>& previous,const std::array<Camera,2>& cameras,
    std::array<ID3D12Resource*,2>& outputs,std::array<OutputCopy,2>* copies,
    void(*on_submitted)(void*) noexcept,void* submitted_context,void(*on_prepared)(void*) noexcept,
    unsigned frame_count,unsigned frame_index,void(*on_output_submitted)(void*) noexcept,void* output_context) noexcept {
    submitted_at={};prepared_serial=0;temporal_evaluation_completed=false;
    std::lock_guard lock(ngx_mutex);
    if(frame_count<1||frame_count>3||frame_index<1||frame_index>frame_count)return false;
    if(cheeky_fg_needs_ngx(guides)&&frame_count>max_generated_frames){
        sprintf_s(diagnostic,"DLSS %ux FG unavailable: this GPU/runtime supports at most %ux; select a supported multiplier or shader methods",
            frame_count+1,max_generated_frames+1);if(trace)trace(diagnostic);return false;
    }
    const float fraction=float(frame_index)/float(frame_count+1);
    const bool last_output=frame_index==frame_count;
    // Every method may publish its queued stereo output. NGX additionally
    // resolves its suppression flag on the GPU before the ready fence.
    const bool early_output=on_output_submitted&&copies&&
        (*copies)[0].texture&&(*copies)[1].texture;
    // Queue all indices in order without a GPU-to-CPU round trip. NGX uses
    // private phase bindings below while retaining the same temporal handles.
    // CPU suppression callers retain their old path. Preparation-gated jobs
    // queue every phase before observing their preparation/completion fences.
    const bool batch_ngx=early_output&&can_batch_ngx(guides);
    const bool batched=!cheeky_fg_needs_ngx(guides)||batch_ngx;
    const bool continuing=batched&&frame_index>1;
    if(!configured||(cheeky_fg_needs_ngx(guides)&&!ngx_required)||!cheeky_fg_center_valid(guides.eyes[0].center_method)||!cheeky_fg_center_valid(guides.eyes[1].center_method)||guides.producer_queue!=queue.Get()||!colors[0]||!colors[1]||!previous[0]||!previous[1]||!cameras[0].valid||!cameras[1].valid)return false;
    if(!continuing&&!wait_idle("previous generation"))return false;
    // Optical preparation and split interpolation prefixes can already be in
    // flight when a later recording/allocation fails. Retire all engines before
    // the caller releases their input and publication leases on that path.
    struct RetireFailure {
        Worker& worker;bool active{};
        ~RetireFailure(){if(active){
            for(auto& eye:worker.focus.optical)for(auto& session:eye)session.wait();
            const auto value=worker.serial+1;
            if(SUCCEEDED(worker.queue->Signal(worker.fence.Get(),value))){worker.serial=value;worker.wait_idle();}
        }}
    } retire_failure{*this};
    ngx_burst.trace=trace;
    ngx_burst.start(guides.render_frame,guides.capture_token,frame_count,frame_index);
    if(configuration!=guides.eyes[0].configuration) {
        const bool previously_failed=failed;
        reset_features();configuration=guides.eyes[0].configuration;initialize_timings();completion_wait={};
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
    auto& phase_focus=batch_ngx&&continuing?ngx_phases[frame_index-2].focus:this->focus;
    auto& phase_features=batch_ngx&&continuing?ngx_phases[frame_index-2].features:this->features;
    auto& phase_peripheral_features=batch_ngx&&continuing?ngx_phases[frame_index-2].peripheral_features:this->peripheral_features;
    if(batch_ngx&&continuing){
        auto borrow=[&](Feature& phase,const Feature& temporal){
            if(phase.handle!=temporal.handle||phase.width!=temporal.width||phase.height!=temporal.height){
                if(phase.params&&destroy)destroy(phase.params);phase={};
            }
            phase.handle=temporal.handle;phase.borrowed_handle=true;
            // DLSS writes this native-pair decision at the first index. Later
            // indices can leave it untouched (verified on the real runtime).
            // Share the flag through the interval: a phase-private buffer can
            // retain warm-up's 1 forever and incorrectly suppress G2/G3.
            // All evaluations, fallback reads and transitions use one queue.
            phase.disable=temporal.disable;
            phase.width=temporal.width;phase.height=temporal.height;
            phase.direct_rejected|=temporal.direct_rejected;phase.xr_direct_rejected|=temporal.xr_direct_rejected;
            phase.evaluated_crop=temporal.evaluated_crop;phase.evaluated_crop_valid=temporal.evaluated_crop_valid;
            phase.evaluated_frame=temporal.evaluated_frame;
        };
        for(unsigned eye=0;eye<2;++eye){
            borrow(phase_features[eye],this->features[eye]);borrow(phase_peripheral_features[eye],this->peripheral_features[eye]);
            // Previous-native depth is immutable until the LAST index saves
            // the new history, after all earlier queue consumers have run.
            phase_focus.previous_depth[eye]=this->focus.previous_depth[eye];
            phase_focus.previous_center_depth[eye]=this->focus.previous_center_depth[eye];
            phase_focus.previous_center_depth_rect[eye]=this->focus.previous_center_depth_rect[eye];
            phase_focus.history[eye]=this->focus.history[eye];phase_focus.center_history[eye]=this->focus.center_history[eye];
            for(unsigned pass=0;pass<2;++pass){
                phase_focus.borrowed_optical[eye][pass]=&this->focus.optical_session(eye,pass);
                phase_focus.optical_constants[eye][pass]=this->focus.optical_constants[eye][pass];
            }
        }
        phase_focus.color_format=this->focus.color_format;
    }
    // Each generated phase owns separate query storage until the final fence.
    // The legacy HUD aggregates their completed samples, rather than overwriting
    // phase 1's optical-flow interval with the last interpolation phase.
    auto& phase_timing=phase_timings[frame_index-1];
    if(batch_ngx&&frame_index==1&&phase_timing.samples==0&&trace){char message[192]{};
        sprintf_s(message,"DLSS-FG queued stereo phases: count=%u; private phase bindings, ordered shared temporal handles, CPU completion wait only after final phase",frame_count);
        trace(message);
    }
    phase_focus.color_layouts=color_layouts;phase_focus.previous_layouts=previous_layouts;
    phase_focus.interpolation_fraction=fraction;phase_focus.descriptor_set=batched?frame_index-1:0;
    bool optical_timing_started=false;
    unsigned optical_regions{};
    for(const auto& g:guides.eyes){
        optical_regions+=cheeky_fg_optical(g.center_method)?1u:0u;
        if(g.focus_width<.999f||g.focus_height<.999f)optical_regions+=cheeky_fg_optical(g.peripheral_method)?1u:0u;
    }
    // The two-list preparation milestone has its own conservative flow order;
    // do not split its second list into deferred optical-consumer lists.
    const bool defer_optical=batched&&guides.release_point!=1u;
    if(frame_index==1)for(auto& eye:optical_pending)for(auto& pending:eye)pending=false;
    // NVOFA submits work outside this command list. Prepare once per native
    // pair, then reuse the same forward/backward fields at all 2x/3x/4x phases.
    if(frame_index==1)for(unsigned eye=0;eye<2;++eye){
        const auto& g=guides.eyes[eye];const bool focused=g.focus_width<.999f||g.focus_height<.999f;
        for(unsigned pass=0;pass<2;++pass){
            if(!cheeky_fg_optical(pass?g.peripheral_method:g.center_method)||(pass&&!focused))continue;
            const unsigned rw=pass||!focused?width:std::clamp(unsigned(std::round(width*g.focus_width)),1u,width);
            const unsigned rh=pass||!focused?height:std::clamp(unsigned(std::round(height*g.focus_height)),1u,height);
            const unsigned rx=pass||!focused?0:unsigned(std::clamp((width-rw)*.5f+g.focus_x*width,0.f,float(width-rw)));
            const unsigned ry=pass||!focused?0:unsigned(std::clamp((height-rh)*.5f+g.focus_y*height,0.f,float(height-rh)));
            const float rect[]{float(rx)/width,float(ry)/height,float(rw)/width,float(rh)/height};
            const unsigned scale=pass?g.peripheral_percent:g.resolution_percent;
            auto c=optical::constants(g,pass,rect,previous_layouts[eye],color_layouts[eye],
                std::max(32u,(rw*scale+50)/100),std::max(32u,(rh*scale+50)/100));
            optical::pose_mapping(c,cameras[eye].previous_pose,cameras[eye].previous_fov,cameras[eye].pose,cameras[eye].view_fov,fraction);
            phase_focus.optical_constants[eye][pass]=c;
            if(!optical_timing_started){
                // A dedicated list, retired by the previous frame's completion
                // wait, so the main allocator is free without a CPU drain.
                if(!optical_allocator&&(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&optical_allocator)))||
                   FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,optical_allocator.Get(),nullptr,IID_PPV_ARGS(&optical_list)))||
                   FAILED(optical_list->Close())))return false;
                if(!wait_fence(serial,"optical list reuse (previous generation on GPU)")||FAILED(optical_allocator->Reset())||FAILED(optical_list->Reset(optical_allocator.Get(),nullptr)))return false;
                latency.begin(optical_list.Get());phase_timing.begin(optical_list.Get());
                batch_timings.begin(device.Get(),optical_list.Get(),frame_count,optical_regions);
                batch_timings.optical_prefix.begin(device.Get(),optical_list.Get());
                if(FAILED(optical_list->Close()))return false;ID3D12CommandList* start[]{optical_list.Get()};queue->ExecuteCommandLists(1,start);
                retire_failure.active=true;
                optical_timing_started=true;
            }
            auto& session=phase_focus.optical_session(eye,pass);
            if(!session.prepare(device.Get(),queue.Get(),previous[eye],colors[eye],pass?g.optical.peripheral_preset:g.optical.center_preset,
                c,g.reset!=0,input_color_state,previous[eye]->GetDesc().Format,colors[eye]->GetDesc().Format,defer_optical)){
                strcpy_s(diagnostic,session.diagnostic);failed=true;return false;
            }
            optical_pending[eye][pass]=defer_optical;
        }
    }
    if(continuing&&!batch_allocators[frame_index-2]){
        auto& a=batch_allocators[frame_index-2];auto& l=batch_lists[frame_index-2];
        if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)))||
           FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&l)))||FAILED(l->Close())){a.Reset();l.Reset();return false;}
    }
    auto& phase_allocator=continuing?batch_allocators[frame_index-2]:allocator;
    auto& phase_list=continuing?batch_lists[frame_index-2]:list;
    auto& phase_second_allocator=continuing?batch_second_allocators[frame_index-2]:second_allocator;
    auto& phase_second_list=continuing?batch_second_lists[frame_index-2]:second_list;
    if(FAILED(phase_allocator->Reset())||FAILED(phase_list->Reset(phase_allocator.Get(),nullptr)))return false;
    // Release point 1 records both eyes' preparation on list A and NGX and
    // composition on list B, with a GPU milestone between them.
    const bool two_lists=guides.release_point==1u;
    // Lazily allocate the second list. Both allocators are reused only after
    // the previous frame's final fence retires.
    if(two_lists) {
        if(!phase_second_allocator && FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&phase_second_allocator)))) {
            if(trace)trace("FG preparation gate: cannot allocate second command allocator");
            phase_list->Close();return false;
        }
        if(!phase_second_list) {
            if(FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,
                    phase_second_allocator.Get(),nullptr,IID_PPV_ARGS(&phase_second_list)))) {
                if(trace)trace("FG preparation gate: cannot create second command list");
                phase_list->Close();return false;
            }
            if(FAILED(phase_second_list->Close())) {phase_list->Close();return false;}
        }
        if(FAILED(phase_second_allocator->Reset())||FAILED(phase_second_list->Reset(phase_second_allocator.Get(),nullptr))) {
            if(trace)trace("FG preparation gate: cannot reset second command list");
            phase_list->Close();return false;
        }
    }
    // The lambdas and timestamp recording follow the active command list.
    // A mode switch is safe because the previous job was fully retired above.
    ID3D12GraphicsCommandList* commands=phase_list.Get();
    unsigned optical_consumer_count{};
    auto consume_optical=[&](unsigned eye,unsigned pass)->bool {
        if(!optical_pending[eye][pass])return true;
        if(optical_consumer_count>=optical_consumer_lists.size())return false;
        auto& wait_timing=batch_timings.optical_waits[batch_timings.optical_wait_count++];
        wait_timing.begin(device.Get(),commands);
        if(FAILED(commands->Close()))return false;
        ID3D12CommandList* prefix[]{commands};queue->ExecuteCommandLists(1,prefix);
        retire_failure.active=true;
        if(!phase_focus.optical_session(eye,pass).consume()){failed=true;return false;}
        optical_pending[eye][pass]=false;
        auto& a=optical_consumer_allocators[optical_consumer_count];auto& l=optical_consumer_lists[optical_consumer_count++];
        if(!a&&(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)))||
            FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&l)))||FAILED(l->Close()))){a.Reset();l.Reset();return false;}
        if(FAILED(a->Reset())||FAILED(l->Reset(a.Get(),nullptr)))return false;
        commands=l.Get();
        if(!phase_focus.optical_session(eye,pass).prepare_vectors(commands,phase_focus.optical_constants[eye][pass]))return false;
        wait_timing.end(commands);return true;
    };
    if(optical_timing_started){
        // With the preparation gate all flow waits are already on the queue.
        // Cache packing belongs to optical preparation, not generated frame 1.
        if(!defer_optical)for(unsigned eye=0;eye<2;++eye)for(unsigned pass=0;pass<2;++pass){
            const auto& g=guides.eyes[eye];
            if(!cheeky_fg_optical(pass?g.peripheral_method:g.center_method)||(pass&&g.focus_width>=.999f&&g.focus_height>=.999f))continue;
            if(!phase_focus.optical_session(eye,pass).prepare_vectors(commands,phase_focus.optical_constants[eye][pass]))return false;
        }
        batch_timings.optical_prefix.end(commands);
    }
    bool cut_to_second_list=false;
    if(!optical_timing_started){if(!continuing)latency.begin(commands);phase_timing.begin(commands);}
    if(frame_index==1&&!optical_timing_started)batch_timings.begin(device.Get(),commands,frame_count,optical_regions);
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
            d.DepthOrArraySize==1&&d.MipLevels==1&&d.SampleDesc.Count==1&&writable_color(d.Format,phase_focus.color_format)&&
            (d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)&&(!(*copies)[eye].final_xr||(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET));
        for(unsigned source=0;source<2&&direct;++source)
            direct=(*copies)[eye].texture!=colors[source]&&(*copies)[eye].texture!=previous[source]&&
                (*copies)[eye].texture!=guides.eyes[source].depth&&(*copies)[eye].texture!=guides.eyes[source].motion;
    }
    if(copies&&!direct&&((*copies)[0].layout.packed()||(*copies)[1].layout.packed()||(*copies)[0].final_xr||(*copies)[1].final_xr)){phase_list->Close();if(two_lists)phase_second_list->Close();return false;}
    // wait_idle above retired every earlier use of these private scratch images.
    // Not while earlier frames of this interval may still sample them.
    if(direct&&direct_periphery&&!continuing)for(unsigned eye=0;eye<2;++eye)
        if(((*copies)[eye].layout.packed()||(*copies)[eye].final_xr)&&(guides.eyes[eye].peripheral_method==cheeky_fg_previous||cheeky_fg_dlss_vectors(guides.eyes[eye].peripheral_method))){
            phase_focus.peripheral_output[eye].Reset();if((*copies)[eye].final_xr)phase_focus.full[eye].Reset();}
    std::array<bool,2> moving_crop_reset{};
    std::array<FocusRect,2> previous_center_crops{};
    std::array<float,2> motion_crop_dx{},motion_crop_dy{};
    auto transpose=[](Matrix m){Matrix result{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)result.m[r*4+c]=m.m[c*4+r];return result;};
    auto configure_pass=[&](Feature& f,const FocusInput& input,const Camera& camera,const CheekyFgEye& g,const FocusRect& rect,unsigned eye=0,bool center_pass=true) {
        const auto desc=input.color->GetDesc();const auto tw=unsigned(desc.Width),th=desc.Height;
        const auto guide_desc=input.depth->GetDesc();const auto gw=unsigned(guide_desc.Width),gh=guide_desc.Height;
        if(f.width!=tw||f.height!=th) {
            if(f.borrowed_handle){report("NGX indexed phase changed dimensions",unsigned(E_INVALIDARG));failed=true;return false;}
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
        if(!recenter_output&&direct&&copies&&!(final_xr&&cheeky_fg_optical(g.peripheral_method))&&((*copies)[eye].layout.packed()||final_xr)&&!(final_xr?f.xr_direct_rejected:f.direct_rejected)){const auto& destination=(*copies)[eye];const auto d=destination.texture->GetDesc();
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
        // A later index owns a separate parameter object, so populate the
        // complete binding even when it borrows an existing temporal handle.
        p->Set("CreationNodeMask",1u);p->Set("VisibilityNodeMask",1u);p->Set("Width",tw);p->Set("Height",th);
        p->Set("DLSSG.BackbufferFormat",unsigned(desc.Format));p->Set("DLSSG.InternalWidth",gw);p->Set("DLSSG.InternalHeight",gh);p->Set("DLSSG.DynamicResolution",1u);
        if(!f.handle) {
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
        if(center_pass)phase_timing.ngx_stamp(commands,eye,0);
        barrier(commands,f.target,f.direct?(*copies)[f.eye].state:D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(commands,f.disable.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if(center_pass)phase_timing.ngx_stamp(commands,eye,1);
        if(ngx_burst.active){
            const unsigned pass=center_pass?0u:1u;
            const auto& inputs=phase_focus.inputs[eye][pass];
            if(trace){unsigned reset{};f.params->Get("DLSSG.Reset",&reset);char message[480]{};
                sprintf_s(message,"FG NGX CAPTURE binding: frame=%llu token=%llu phase=%u/%u eye=%u pass=%u reset=%u handle=%p params=%p color=%p depth=%p motion=%p output=%p",
                    static_cast<unsigned long long>(guides.render_frame),static_cast<unsigned long long>(guides.capture_token),
                    frame_index,frame_count,eye,pass,reset,f.handle,f.params,inputs.color.Get(),inputs.depth.Get(),inputs.motion.Get(),f.target);trace(message);}
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
            if(!f.output&&!texture(device.Get(),phase_focus.inputs[f.eye][center_pass?0:1].color.Get(),f.width,f.height,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,f.output)){failed=true;return false;}
            f.direct=false;f.target=f.output.Get();f.params->Set("DLSSG.OutputInterpolated",f.target);
            f.params->Set("DLSSG.OutputInterpolatedSubrectBaseY",0u);f.params->Set("DLSSG.OutputInterpolatedSubrectBaseX",0u);
            f.params->Set("DLSSG.EvalFlags",0u);f.params->Set("DLSSG.Reset",1u);
            barrier(commands,f.target,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            result=evaluate(commands,f.handle,f.params,nullptr);
        }
        if(center_pass)phase_timing.ngx_stamp(commands,eye,2);
        if(!ok(result)){report("NGX FG EvaluateFeature",result);failed=true;return false;}
        barrier(commands,f.target,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        if(f.direct)(*copies)[f.eye].state=D3D12_RESOURCE_STATE_COMMON;
        if(ngx_burst.active)
            ngx_burst.record(device.Get(),commands,f.target,D3D12_RESOURCE_STATE_COMMON,
                "output_interpolated",eye,center_pass?0u:1u,f.offset_x,f.offset_y,f.width,f.height,f.direct);
        barrier(commands,f.disable.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->CopyBufferRegion(f.readback.Get(),0,f.disable.Get(),0,4);
        barrier(commands,f.disable.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        if(ngx_burst.active)ngx_burst.record(device.Get(),commands,f.disable.Get(),D3D12_RESOURCE_STATE_COMMON,
            "disable_interpolation",eye,center_pass?0u:1u);
        if(center_pass)phase_timing.ngx_stamp(commands,eye,3);
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
            const unsigned eye=(phase_timing.first_eye+step)%2;
            const auto& g=guides.eyes[eye];const unsigned query=2+eye*6;
            const Camera& camera=cameras[eye];
            if(!prepare_first||phase==0) {
                phase_timing.stamp(commands,query);
                const bool image_only=cheeky_fg_image_only(g.center_method,g.peripheral_method,g.focus_width<.999f||g.focus_height<.999f);
                // Prepared guides omit textures their methods do not read;
                // prepare_inputs validates those per pass.
                const bool raw_guides=!image_only&&!g.prepared_guides;
                if((raw_guides&&(!g.depth||!g.motion))||g.width!=width||g.height!=height){report("FG guide/output dimensions disagree",unsigned(E_INVALIDARG));failed=true;good=false;break;}
                source_depth_format=g.depth?unsigned(g.depth->GetDesc().Format):0;source_motion_format=g.motion?unsigned(g.motion->GetDesc().Format):0;
                if(raw_guides&&
                   (depth_srv_format(DXGI_FORMAT(source_depth_format))==DXGI_FORMAT_UNKNOWN||motion_srv_format(DXGI_FORMAT(source_motion_format))==DXGI_FORMAT_UNKNOWN)){
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
                const auto& last=phase_features[eye];
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
                auto* detail=phase_timing.record_preparation?&phase_timing:nullptr;
                if(!phase_focus.prepare_inputs(device.Get(),commands,eye,0,colors[eye],g,rect,center_width,center_height,input_color_state,detail,motion_crop_dx[eye],motion_crop_dy[eye])) {
                    report("FG center guide conversion",unsigned(E_FAIL));failed=true;good=false;break;
                }
                if(rect.active&&!phase_focus.prepare_inputs(device.Get(),commands,eye,1,colors[eye],g,full_rect,peripheral_width,peripheral_height,input_color_state,detail)) {
                    report("FG periphery guide conversion",unsigned(E_FAIL));failed=true;good=false;break;
                }
                if(!rect.active)for(unsigned part=0;part<4;++part)phase_timing.preparation_stamp(commands,eye,1,part);
                // Feature creation can record GPU commands. With the preparation
                // gate, configure both feature instances before either evaluates.
                if(prepare_first&&g.center_method==0&&!configure_pass(phase_features[eye],phase_focus.inputs[eye][0],camera,g,rect,eye)) {
                    good=false;break;
                }
                phase_timing.stamp(commands,query+1);
            }
            if(prepare_first&&phase==0)continue;
            const auto& rect=center_rects[eye];
            if(!prepare_first||phase==1) {
                if(cheeky_fg_optical(g.center_method)&&!consume_optical(eye,0)){good=false;break;}
                phase_timing.stamp(commands,query+2);
                if(g.center_method) {
                    const auto& last=phase_features[eye];
                    // Gaze movement changes the depth crop's origin. Preserve
                    // history and sample it with its saved rectangle; newly
                    // uncovered pixels fall back to the warped current frame.
                    phase_focus.center_history[eye]=phase_focus.center_history[eye]&&!g.reset&&last.evaluated_crop_valid&&
                        last.evaluated_crop.w==rect.w&&last.evaluated_crop.h==rect.h;
                    good=phase_focus.generate_periphery(device.Get(),commands,eye,previous[eye],colors[eye],g,camera,input_color_state,
                        nullptr,D3D12_RESOURCE_STATE_COMMON,nullptr,nullptr,&rect);
                    for(unsigned mark=0;mark<4;++mark)phase_timing.ngx_stamp(commands,eye,mark);
                } else {
                    good=prepare_first?dispatch_pass(phase_features[eye],eye,true):
                        (configure_pass(phase_features[eye],phase_focus.inputs[eye][0],camera,g,rect,eye)&&dispatch_pass(phase_features[eye],eye,true));
                }
                phase_timing.stamp(commands,query+3);
                if(!good)break;
            }
            if(prepare_first&&phase==1)continue;
            if(rect.active&&cheeky_fg_optical(g.peripheral_method)&&!consume_optical(eye,1)){good=false;break;}
            phase_timing.stamp(commands,query+4);
            auto* center=g.center_method?phase_focus.center_output[eye].Get():phase_features[eye].target;
            if(motion_crop_dx[eye]!=0.f||motion_crop_dy[eye]!=0.f){
                // For a static full-eye point P and crop origins A -> B, NGX
                // interpolates (P-A)/size -> (P-B)/size. Its output at t belongs
                // at lerp(A,B,t). Sampling at uv+(1-t)*(B-A)/size re-expresses that
                // image in B without changing the real scene's interpolation.
                if(!phase_focus.recenter_frame(device.Get(),commands,eye,center,(1-fraction)*motion_crop_dx[eye],(1-fraction)*motion_crop_dy[eye])){
                    report("FG moving-crop output recenter",unsigned(E_FAIL));failed=true;good=false;break;
                }
                center=phase_focus.inputs[eye][0].recentered_color.Get();
            }
            auto* base=center;
            bool custom_context_direct=false;
            const bool final_xr=direct&&(*copies)[eye].final_xr;
            if(final_xr){
                FocusPipeline::FinalOutput final{center,rect,width,height,phase_features[eye].direct,
                    phase_focus.color_format==DXGI_FORMAT_R8G8B8A8_UNORM&&srgb_color(format)};
                good=phase_focus.generate_periphery(device.Get(),commands,eye,previous[eye],colors[eye],g,camera,input_color_state,
                    (*copies)[eye].texture,(*copies)[eye].state,nullptr,&final);
                (*copies)[eye].state=D3D12_RESOURCE_STATE_COMMON;outputs[eye]=(*copies)[eye].texture;
            }else if(rect.active) {
                if(g.peripheral_method==0) {
                    good=evaluate_pass(phase_peripheral_features[eye],phase_focus.inputs[eye][1],camera,g,FocusRect{0,0,width,height,0,false},eye);
                    base=phase_peripheral_features[eye].target;
                }else {
                    custom_context_direct=direct&&direct_periphery&&(*copies)[eye].layout.packed()&&!cheeky_fg_optical(g.peripheral_method);
                    auto* target=custom_context_direct?(*copies)[eye].texture:nullptr;
                    good=phase_focus.generate_periphery(device.Get(),commands,eye,previous[eye],colors[eye],g,camera,input_color_state,
                        target,custom_context_direct?(*copies)[eye].state:D3D12_RESOURCE_STATE_COMMON,
                        custom_context_direct?&(*copies)[eye].layout:nullptr);
                    base=custom_context_direct?target:phase_focus.peripheral_output[eye].Get();
                    if(good&&custom_context_direct)(*copies)[eye].state=D3D12_RESOURCE_STATE_COMMON;
                }
            }
            if(final_xr){}else if(good&&copies&&(*copies)[eye].layout.packed()) {
                const bool center_direct=phase_features[eye].direct;
                const bool context_direct=custom_context_direct||(g.peripheral_method==0&&phase_peripheral_features[eye].direct);
                good=phase_focus.pack_frame(device.Get(),commands,eye,center,base,(*copies)[eye].texture,(*copies)[eye].state,
                    (*copies)[eye].layout,center_direct,context_direct);
                outputs[eye]=(*copies)[eye].texture;(*copies)[eye].state=D3D12_RESOURCE_STATE_COMMON;
            }else if(good&&(rect.active||center_output_width[eye]!=width||center_output_height[eye]!=height)) {
                good=phase_focus.compose_frame(device.Get(),commands,eye,center,base,rect,width,height,
                    direct?(*copies)[eye].texture:nullptr,direct?(*copies)[eye].state:D3D12_RESOURCE_STATE_COMMON);
                outputs[eye]=direct?(*copies)[eye].texture:phase_focus.full[eye].Get();
                if(direct)(*copies)[eye].state=D3D12_RESOURCE_STATE_GENERIC_READ;
            } else outputs[eye]=base;
            if(good&&last_output&&rect.active&&cheeky_fg_depth_history(g.peripheral_method))good=phase_focus.save_depth(device.Get(),commands,eye);
            if(good&&last_output&&cheeky_fg_depth_history(g.center_method))good=phase_focus.save_depth(device.Get(),commands,eye,0);
            if(!good&&!failed){report("FG periphery/composition allocation",unsigned(E_FAIL));failed=true;}
            phase_timing.stamp(commands,query+5);
        }
        if(good&&two_lists&&phase==0) {
            // Both eyes' GPU preparation is on A; NGX/composition is on B.
            // Record B before submitting either list to avoid a CPU-created
            // gap between preparation and inference.
            if(FAILED(commands->Close())) {good=false;break;}
            commands=phase_second_list.Get();cut_to_second_list=true;
        }
    }
    if(good&&direct)for(auto& copy:*copies){
        const auto state=copy.final_xr?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_GENERIC_READ;
        barrier(commands,copy.texture,copy.state,state);copy.state=state;
    }
    if(good)phase_timing.stamp(commands,1);
    if(good)phase_timing.stamp(commands,14);
    // Publish immutable output in the SAME GPU submission as inference. A copy
    // submitted only after the CPU completion wait can land behind a newer game
    // frame and delay a finished interpolation by another render interval.
    if(good&&copies&&!direct&&!record_output_copies(commands,outputs,*copies)) {
        report("FG immutable output copy",unsigned(E_INVALIDARG));failed=true;good=false;
    }
    if(good&&early_output&&cheeky_fg_needs_ngx(guides)){
        std::array<ID3D12Resource*,4> disabled{};
        for(unsigned eye=0;eye<2;++eye){
            if(guides.eyes[eye].center_method==0)disabled[eye*2]=phase_features[eye].disable.Get();
            if(foveated[eye]&&guides.eyes[eye].peripheral_method==0)disabled[eye*2+1]=phase_peripheral_features[eye].disable.Get();
        }
        // Either eye/pass suppresses the entire stereo result. Both outputs
        // then contain native color instead of exposing invalid interpolation.
        for(unsigned eye=0;eye<2&&good;++eye)if(!ngx_fallback.record(device.Get(),commands,frame_index-1,eye,
            colors[eye],input_color_state,color_layouts[eye],(*copies)[eye].texture,(*copies)[eye].layout,disabled,
            (*copies)[eye].final_xr?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_GENERIC_READ,phase_focus.color_format)){
            report("FG queued DLSS native fallback",unsigned(E_FAIL));failed=true;good=false;
        }
    }
    if(good&&ngx_burst.active&&copies)for(unsigned eye=0;eye<2;++eye)
        ngx_burst.record(device.Get(),commands,(*copies)[eye].texture,
            (*copies)[eye].final_xr?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_GENERIC_READ,
            "published_stereo",eye,0);
    if(good){phase_timing.stamp(commands,15);phase_timing.resolve(commands);if(!batched||last_output)latency.end(commands);
        if(last_output)batch_timings.end(commands);}
    // A split optical prefix is already submitted. Its retirement guard keeps
    // captured resources alive if recording a later consumer fails.
    const HRESULT close_result=commands->Close();
    if(FAILED(close_result)||!good) {
        ngx_burst.abandon();
        if(FAILED(close_result))report("FG command list close",unsigned(close_result));
        // A failure before the cut leaves list B recording. Close it too
        // before reusing its allocator on the next frame. Neither list has
        // been submitted, so nothing may still reference captured inputs.
        if(two_lists && !cut_to_second_list && phase_second_list)phase_second_list->Close();
        return false;
    }
    if(two_lists && cut_to_second_list) {
        // Two actual ExecuteCommandLists calls on the SAME HIGH-priority queue,
        // strictly ordered, no CPU fence wait and no inter-list GPU signal.
        ID3D12CommandList* first[]{phase_list.Get()};
        ID3D12CommandList* second[]{phase_second_list.Get()};
        retire_failure.active=true;
        queue->ExecuteCommandLists(1,first);
        prepared_serial=++serial;
        if(FAILED(queue->Signal(fence.Get(),prepared_serial)))return false;
        queue->ExecuteCommandLists(1,second);
        if(trace && phase_timing.samples==0)trace("FG preparation gate: both eyes on list A; GPU milestone before NGX list B");
    } else {
        retire_failure.active=true;
        ID3D12CommandList* lists[]{commands};queue->ExecuteCommandLists(1,lists);
    }
    if(copies)for(auto& copy:*copies)copy.state=copy.final_xr?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_GENERIC_READ;
    if(FAILED(queue->Signal(fence.Get(),++serial))){ngx_burst.abandon();return false;}
    ngx_burst.commit(fence.Get(),serial);
    // Only submitted evaluations advance NGX history. The pair can fail after
    // recording EvaluateFeature (e.g. during final composition), in which case
    // the command list is discarded and these crops must not be published.
    for(unsigned eye=0;last_output&&eye<2;++eye){
        auto& done=this->features[eye];
        done.evaluated_crop=center_rects[eye];done.evaluated_crop_valid=true;
        done.evaluated_frame=guides.render_frame;
    }
    submitted_at=TimingClock::now();latency.submitted(fence.Get(),serial);
    // The output, all final transitions, GPU suppression fallback and stereo
    // completion fence are now queued. The presenter can track this phase's
    // readiness while this worker retires NGX resources/history on the CPU.
    if(early_output)on_output_submitted(output_context);
    // Notify only after commands AND their completion fence are queued. The
    // presenter may release the next native render without waiting for FG to
    // finish, but must not let it overtake a still-unsubmitted FG job.
    if(on_submitted)on_submitted(submitted_context);
    // Earlier frames of a batched interval are retired by the last frame's wait.
    if(batched&&!last_output){retire_failure.active=false;direct_publication=direct;temporal_evaluation_completed=true;return true;}
    const auto wait_begin=TimingClock::now();
    if(two_lists) {
        if(!wait_fence(prepared_serial,"prepared guides"))return false;
        if(on_prepared)on_prepared(submitted_context);
    }
    // Includes the capture this generation waits for on the GPU (the game's frame).
    const bool complete=wait_idle("generation complete");const auto observed_cpu=LatencyProbe::cpu_now();
    completion_wait.add(elapsed_ms(wait_begin));if(!complete)return false;
    latency.collect(queue.Get(),observed_cpu);
    retire_failure.active=false;
    direct_publication=direct;
    const unsigned collect_count=batched?frame_count:1u;
    if((batched&&last_output)||(!batched&&frame_index==1))batch_timings.collect_optical(this->gpu_timings.frequency);
    for(unsigned i=0;i<collect_count;++i){auto& t=batched?phase_timings[i]:phase_timing;
        if(batch_timings.optical_valid)t.collect(focused,copies&&!direct,(batched?i==0:frame_index==1)?batch_timings.optical_current:0);
        this->gpu_timings.append_latest(t);}
    // Non-batched NGX calls also retain independent phase queries; only the
    // last call closes the native-pair span.
    if(last_output)batch_timings.collect(this->gpu_timings.frequency,phase_timings);
    if(batch_ngx&&last_output&&continuing)for(unsigned eye=0;eye<2;++eye){
        // Carry only completed temporal state into the next native interval;
        // phase-private preparation/output bindings stay with their allocators.
        this->focus.previous_depth[eye]=phase_focus.previous_depth[eye];
        this->focus.previous_center_depth[eye]=phase_focus.previous_center_depth[eye];
        this->focus.previous_center_depth_rect[eye]=phase_focus.previous_center_depth_rect[eye];
    }
    // Existing timestamp data only: distinguish consistently expensive work
    // from occasional long elapsed intervals without changing the GPU schedule.
    if(trace&&phase_timing.valid&&(phase_timing.samples%120)==0) {
        // The 4.2ms counter includes NGX's GPU commands and surrounding UAV
        // barriers/copy; the detailed queries isolate those three regions.
        for(unsigned eye=0;eye<2;++eye) {
            const auto& phase=phase_timing.ngx_parts[eye];
            char detail_msg[512]{};
            sprintf_s(detail_msg,
                "FG NGX breakdown GPU ms: eye=%s samples=%llu "
                "before_avg=%.3f before_p95=%.3f ngx_avg=%.3f ngx_p50=%.3f ngx_p95=%.3f "
                "after_avg=%.3f after_p95=%.3f",
                eye?"right":"left",
                static_cast<unsigned long long>(phase_timing.samples),
                phase[0].mean(),phase[0].percentile(95),
                phase[1].mean(),phase[1].percentile(50),phase[1].percentile(95),
                phase[2].mean(),phase[2].percentile(95));
            trace(detail_msg);
        }
        {
            const auto& first=phase_timing.ngx_by_position[0];
            const auto& second=phase_timing.ngx_by_position[1];
            char order_msg[480]{};
            sprintf_s(order_msg,
                "FG NGX breakdown order GPU ms: samples=%llu "
                "first_before=%.3f first_ngx=%.3f first_ngx_p95=%.3f first_after=%.3f "
                "second_before=%.3f second_ngx=%.3f second_ngx_p95=%.3f second_after=%.3f",
                static_cast<unsigned long long>(phase_timing.samples),
                first[0].mean(),first[1].mean(),first[1].percentile(95),first[2].mean(),
                second[0].mean(),second[1].mean(),second[1].percentile(95),second[2].mean());
            trace(order_msg);
        }
        const auto& prep=phase_timing.preparation;char message[384]{};
        sprintf_s(message,"FG preparation GPU ms: avg=%.3f median=%.3f p95=%.3f max=%.3f left_avg=%.3f right_avg=%.3f; processing_median=%.3f processing_p95=%.3f",
            prep.mean(),prep.percentile(50),prep.percentile(95),prep.percentile(100),
            phase_timing.eye_preparation[0].mean(),phase_timing.eye_preparation[1].mean(),
            phase_timing.total.percentile(50),phase_timing.total.percentile(95));trace(message);
        // Eye identity alone cannot identify a queue stall: evaluation order
        // alternates every generated stereo pair, while all eye inputs and
        // feature histories remain unchanged. GPU stamps have already been
        // resolved on the normal completion fence; this is CPU-only logging.
        {
            const auto& left=phase_timing.eye_center[0];
            const auto& right=phase_timing.eye_center[1];
            const auto& first=phase_timing.center_by_position[0];
            const auto& second=phase_timing.center_by_position[1];
            char eye_message[640]{};
            sprintf_s(eye_message,
                "FG NGX eye GPU ms: frames=%llu left_avg=%.3f left_p50=%.3f left_p95=%.3f left_max=%.3f "
                "right_avg=%.3f right_p50=%.3f right_p95=%.3f right_max=%.3f "
                "first_avg=%.3f first_p95=%.3f second_avg=%.3f second_p95=%.3f "
                "stereo_avg=%.3f stereo_p50=%.3f stereo_p95=%.3f",
                static_cast<unsigned long long>(phase_timing.samples),
                left.mean(),left.percentile(50),left.percentile(95),left.percentile(100),
                right.mean(),right.percentile(50),right.percentile(95),right.percentile(100),
                first.mean(),first.percentile(95),second.mean(),second.percentile(95),
                phase_timing.center.mean(),phase_timing.center.percentile(50),phase_timing.center.percentile(95));
            trace(eye_message);
        }
        for(unsigned first=0;first<2;++first)if(phase_timing.order_samples[first]) {
            const auto& by_eye=phase_timing.eye_center_by_first[first];
            char order_message[480]{};
            sprintf_s(order_message,
                "FG NGX order GPU ms: first=%s frames=%llu "
                "left_avg=%.3f left_p50=%.3f left_p95=%.3f "
                "right_avg=%.3f right_p50=%.3f right_p95=%.3f "
                "first_eye_avg=%.3f second_eye_avg=%.3f",
                first?"right":"left",
                static_cast<unsigned long long>(phase_timing.order_samples[first]),
                by_eye[0].mean(),by_eye[0].percentile(50),by_eye[0].percentile(95),
                by_eye[1].mean(),by_eye[1].percentile(50),by_eye[1].percentile(95),
                by_eye[first].mean(),by_eye[1-first].mean());
            trace(order_message);
        }
        for(unsigned first=0;first<2;++first)if(phase_timing.order_samples[first]) {
            const auto& prep=phase_timing.preparation_by_first[first];
            const auto& eye=phase_timing.eye_preparation_by_first[first];
            const auto& center=phase_timing.center_dispatch_by_first[first];
            sprintf_s(message,"FG preparation order GPU ms: first=%s frames=%llu detail=%llu; prep_avg=%.3f prep_p95=%.3f left_avg=%.3f right_avg=%.3f; sampled_center_left=%.3f sampled_center_right=%.3f",
                first?"right":"left",static_cast<unsigned long long>(phase_timing.order_samples[first]),
                static_cast<unsigned long long>(phase_timing.order_detail_samples[first]),prep.mean(),prep.percentile(95),
                eye[0].mean(),eye[1].mean(),center[0].mean(),center[1].mean());trace(message);
        }
        if(phase_timing.preparation_samples) {
            sprintf_s(message,"FG preparation sampled GPU ms: samples=%llu (1 in 17); avg=%.3f median=%.3f p95=%.3f; gaps_avg=%.3f",
                static_cast<unsigned long long>(phase_timing.preparation_samples),phase_timing.sampled_preparation.mean(),
                phase_timing.sampled_preparation.percentile(50),phase_timing.sampled_preparation.percentile(95),phase_timing.preparation_gaps.mean());trace(message);
            for(unsigned pass=0;pass<4;++pass) {
                const auto& part=phase_timing.preparation_parts[pass];const auto& input=phase_focus.inputs[pass/2][pass%2];
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
        if(last_output){this->focus.history[eye]=foveated[eye]&&cheeky_fg_depth_history(guides.eyes[eye].peripheral_method);
            this->focus.center_history[eye]=cheeky_fg_depth_history(guides.eyes[eye].center_method);}
        for(auto* f:{&phase_features[eye],foveated[eye]&&guides.eyes[eye].peripheral_method==0?&phase_peripheral_features[eye]:nullptr}) {
            if(!f)continue;
            if(!f->handle)continue;void* data{};if(FAILED(f->readback->Map(0,nullptr,&data))||!data)return false;
            enabled&=*static_cast<const unsigned char*>(data)==0;const D3D12_RANGE written{0,0};f->readback->Unmap(0,&written);
        }
    }
    temporal_evaluation_completed=true;
    // An early published suppressed phase already contains safe native color.
    // Legacy callers retain the original CPU suppression/warm-up behavior.
    return enabled||early_output;
}
}
