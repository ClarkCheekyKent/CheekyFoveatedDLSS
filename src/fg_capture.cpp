#include "fg_capture.hpp"
#include "settings.hpp"
#include "fg_snapshot.hpp"
#include "fg_frame_phase.hpp"
#include "uevr_frame_source.hpp"
#include "runtime.hpp"
#include <Windows.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <bit>
#include <unordered_map>

using Microsoft::WRL::ComPtr;
namespace cheeky::foveated_dlss {
namespace {
std::mutex mutex;
// Set by UEVR's executing RHI command, not by an engine-tick/global counter.
thread_local std::uint64_t render_frame{};
std::atomic<std::uint64_t> source_epoch{1};
thread_local std::uint64_t ticket_epoch{};
std::mutex source_mutex;
using EnqueueRenderPoses=void(*)(void*,std::uint32_t);
void* original_enqueue{};
int source_hook{-1};
std::atomic<bool> source_abandoned{};
CheekyFgUnregisterHookFn unregister_source{};
std::atomic<std::uint64_t> source_calls{},last_guide_frame{},captured_guides{},missing_frame_guides{},color_copies{};
std::atomic<std::uint64_t> last_color_frame{};
FgSnapshots::Match last_color_match;
char source_message[128]{"Plugin frame source has not attached"};
using RhiCommand=void*(*)(void*,void*,void*,void*,void*,void*,void*,void*,void*,void*,void*,void*);
struct RhiHook {void* original{};int id{-1};std::uintptr_t begin{},end{};};
std::array<RhiHook,8> rhi_hooks;
unsigned rhi_hook_count{};
std::atomic<int> selected_rhi{-1};
std::atomic<std::uint64_t> rhi_serial{};
thread_local bool rhi_ticket{};
thread_local FgFramePhase frame_phase;
std::atomic<unsigned> phase_traces{};
template<unsigned Index> void* rhi_command(void* a,void* b,void* c,void* d,void* e,void* f,void* g,void* h,void* i,void* j,void* k,void* l) {
    if(!source_abandoned.load()&&selected_rhi.load()==int(Index)){
        const auto epoch=source_epoch.load();if(ticket_epoch!=epoch||!rhi_ticket)frame_phase={};
        render_frame=0x100000000ull+(++rhi_serial);ticket_epoch=epoch;rhi_ticket=true;
        frame_phase.begin(render_frame);
    }
    return reinterpret_cast<RhiCommand>(rhi_hooks[Index].original)(a,b,c,d,e,f,g,h,i,j,k,l);
}
template<std::size_t... I> auto rhi_detours(std::index_sequence<I...>){return std::array<RhiCommand,sizeof...(I)>{&rhi_command<unsigned(I)>...};}
void enqueue_render_poses(void* runtime,std::uint32_t frame) {
    // This is the frame argument of the actual UEVR render-pose command, on
    // the calling render/RHI thread. Never read a latest game-thread counter.
    if(!source_abandoned.load()&&selected_rhi.load()<0&&rhi_hook_count){
        void* stack[16]{};const auto count=CaptureStackBackTrace(0,16,stack,nullptr);
        for(unsigned n=0;n<count&&selected_rhi.load()<0;++n)for(unsigned slot=0;slot<rhi_hook_count;++slot){
            auto ip=reinterpret_cast<std::uintptr_t>(stack[n]);const auto& hook=rhi_hooks[slot];
            if(hook.id>=0&&ip>=hook.begin&&ip<hook.end){
                selected_rhi=int(slot);++source_epoch;render_frame=0;rhi_ticket=false;
                trace_event("FG RHI frame source selected entry=%p; stamp precedes original command execution",reinterpret_cast<void*>(hook.begin));break;
            }
        }
    }
    if(source_abandoned.load())render_frame=0;
    else if(selected_rhi.load()<0){render_frame=std::uint64_t(frame)+1;ticket_epoch=source_epoch.load();rhi_ticket=false;}
    else if(!rhi_ticket||ticket_epoch!=source_epoch.load())render_frame=0;
    // enqueue_render_poses can run AFTER the original RHI command. Once its
    // dispatcher is selected, retain the entry ticket instead of overwriting
    // it here with a frame number that arrives after DLSS has already run.
    ++source_calls;
    const auto original=reinterpret_cast<EnqueueRenderPoses>(original_enqueue);
    if(original)original(runtime,frame);
}
// Creation and evaluation need not use the same parameter object. In
// particular, reversed depth and jittered motion are feature creation flags.
std::unordered_map<const NgxHandle*,std::uint32_t> feature_flags;
// GPU recordings can outlive a runtime detach. Keep queue generations until
// their recorded work and every consumer lease have retired.
struct SnapshotStore {
    FgColorProvenance colors;
    std::vector<std::unique_ptr<FgSnapshots>> generations;
    FgSnapshots* active{};
    SnapshotStore(){generations.push_back(std::make_unique<FgSnapshots>());active=generations.back().get();}
    void collect(){std::erase_if(generations,[&](auto& p){p->retire();return p.get()!=active&&p->idle();});}
};
SnapshotStore& store(){static auto* value=new SnapshotStore;return *value;}
FgSnapshots& snapshots(){return *store().active;}
bool enabled() noexcept {
    return current_settings().fg.enabled;
}
std::uint64_t configuration(const Settings::FrameGeneration& s) noexcept {
    std::uint64_t hash=1469598103934665603ull;
    for(const auto n:{unsigned(s.foveated),s.resolution_percent,s.peripheral_percent,
        s.peripheral_method,s.depth_bits,s.motion_bits,std::bit_cast<unsigned>(s.width),
        std::bit_cast<unsigned>(s.height),std::bit_cast<unsigned>(s.x),std::bit_cast<unsigned>(s.y),std::bit_cast<unsigned>(s.feather)})
        hash=(hash^n)*1099511628211ull;
    return hash;
}
ID3D12Resource* read(const NgxParameters* p,const char* n) noexcept {
    ID3D12Resource* r{};if (p) p->Get(n,&r);return r;
}
unsigned ui(const NgxParameters* p,const char* n,unsigned fallback=0) noexcept {
    unsigned u{};return p && ngx_succeeded(p->Get(n,&u))?u:fallback;
}
float f(const NgxParameters* p,const char* n,float fallback=0) noexcept {
    float v{};return p && ngx_succeeded(p->Get(n,&v))?v:fallback;
}
}
void fg_set_uevr_queue(ID3D12CommandQueue* q) noexcept try {
    std::lock_guard lock(mutex);
    if(!q){snapshots().invalidate();store().colors.invalidate();return;}
    store().collect();
    if(!snapshots().initialize(q)&&store().generations.size()<8){
        snapshots().invalidate();
        store().colors.invalidate();
        auto next=std::make_unique<FgSnapshots>();
        if(next->initialize(q)){store().generations.push_back(std::move(next));store().active=store().generations.back().get();}
    }
} catch(...) {}
void fg_note_submission(ID3D12CommandQueue* q,ID3D12GraphicsCommandList* list) noexcept try {
    std::lock_guard lock(mutex);for(auto& p:store().generations)p->submitted(q,list);
    store().colors.submitted(q,list,snapshots());
} catch(...) {}
void fg_retire_snapshots() noexcept try {std::lock_guard lock(mutex);store().collect();} catch(...) {}
void fg_note_reset(ID3D12GraphicsCommandList* list) noexcept try {
    std::lock_guard lock(mutex);for(auto& p:store().generations)p->reset(list);
    store().colors.reset(list);
} catch(...) {}
void fg_note_color_copy(ID3D12GraphicsCommandList* list,ID3D12Resource* color) noexcept try {
    // Most copies (including our guide snapshots) are unrelated. Inspect the
    // registration before taking the mutex; snapshot recording already owns it.
    if(!enabled()||!FgColorProvenance::registered(color))return;
    const auto color_frame=ticket_epoch!=source_epoch.load()?0:rhi_ticket?frame_phase.color():render_frame;
    ++color_copies;
    std::lock_guard lock(mutex);store().colors.observe(list,color,snapshots(),color_frame);
    if(rhi_ticket&&phase_traces.fetch_add(1)<48)
        trace_event("FG color phase copy=%p command=%p render_entry=%llu color_content=%llu evaluations=%u previous_complete=%llu",
            color,list,render_frame,color_frame,frame_phase.evaluations,frame_phase.previous);
} catch(...) {}
bool fg_recording_tagged(ID3D12GraphicsCommandList* list) noexcept{return FgSnapshots::tagged(list);}
void fg_abandon_frame_source() noexcept {source_abandoned=true;++source_epoch;}
void fg_remember_feature(const NgxHandle* handle,const NgxParameters* p) noexcept try {
    std::uint32_t flags{};if(!handle||!try_get_ngx_integer_bits(p,"DLSS.Feature.Create.Flags",flags))return;
    std::lock_guard lock(mutex);feature_flags[handle]=flags;
} catch(...) {}
void fg_forget_feature(const NgxHandle* handle) noexcept try {
    std::lock_guard lock(mutex);feature_flags.erase(handle);
} catch(...) {}
void fg_publish_ngx(ID3D12GraphicsCommandList* cmd,const NgxHandle* handle,
                    const NgxParameters* p,NgxResult result) noexcept try {
    if(!enabled()||!cmd||!p||!handle||!ngx_succeeded(result))return;
    if(!render_frame||ticket_epoch!=source_epoch.load()){++missing_frame_guides;return;}
    // A failed/unsupported capture still advances rendering. Do not let it
    // leave the preceding frame eligible as the new color's identity.
    if(rhi_ticket)frame_phase.evaluated();
    std::uint32_t flags{};bool flags_known{};
    {std::lock_guard lock(mutex);if(auto entry=feature_flags.find(handle);entry!=feature_flags.end()){flags=entry->second;flags_known=true;}}
    // Late attachment can still recover an explicitly supplied declaration.
    // Never manufacture the depth convention when both sources are absent.
    if(!flags_known&&!try_get_ngx_integer_bits(p,"DLSS.Feature.Create.Flags",flags))return;
    auto* depth=read(p,"Depth");auto* motion=read(p,"MotionVectors");
    if(!depth||!motion)return;
    const auto dd=depth->GetDesc(),md=motion->GetDesc();
    if(dd.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||md.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
       dd.DepthOrArraySize!=1||md.DepthOrArraySize!=1||dd.SampleDesc.Count!=1||md.SampleDesc.Count!=1)return;
    const unsigned w=ui(p,"OutWidth"),h=ui(p,"OutHeight");
    if(!w||!h)return;
    CheekyFgEye m;
    m.render_frame=render_frame;
    m.depth=depth;m.motion=motion;
    m.feature=reinterpret_cast<std::uintptr_t>(handle);m.width=w;m.height=h;
    m.depth_width=ui(p,"DLSS.Render.Subrect.Dimensions.Width",static_cast<unsigned>(dd.Width));
    m.depth_height=ui(p,"DLSS.Render.Subrect.Dimensions.Height",dd.Height);
    if(!m.depth_width)m.depth_width=ui(p,"Width",static_cast<unsigned>(dd.Width));
    if(!m.depth_height)m.depth_height=ui(p,"Height",dd.Height);
    if(!m.depth_width)m.depth_width=static_cast<unsigned>(dd.Width);
    if(!m.depth_height)m.depth_height=dd.Height;
    const bool low_res=(flags & 2U)!=0||md.Width<w||md.Height<h;
    m.motion_width=low_res?m.depth_width:w;m.motion_height=low_res?m.depth_height:h;
    m.depth_base_x=ui(p,"DLSS.Input.Depth.Subrect.Base.X");
    m.depth_base_y=ui(p,"DLSS.Input.Depth.Subrect.Base.Y");
    m.motion_base_x=ui(p,"DLSS.Input.MV.Subrect.Base.X");
    m.motion_base_y=ui(p,"DLSS.Input.MV.Subrect.Base.Y");
    m.jitter_x=f(p,"Jitter.Offset.X");m.jitter_y=f(p,"Jitter.Offset.Y");
    m.motion_jittered=(flags&4U)!=0;
    m.motion_scale_x=f(p,"MV.Scale.X",1);m.motion_scale_y=f(p,"MV.Scale.Y",1);
    m.depth_inverted=(flags & 8U)!=0;
    m.low_res_mv=(flags & 2U)!=0;
    m.reset=ui(p,"Reset");
    const auto& settings=current_settings();
    m.focus_width=settings.fg.foveated ? settings.fg.width : 1.0f;
    m.focus_height=settings.fg.foveated ? settings.fg.height : 1.0f;
    m.focus_x=settings.fg.x;
    m.focus_y=settings.fg.y;
    m.focus_feather=settings.fg.feather;
    m.resolution_percent=settings.fg.resolution_percent;
    m.peripheral_percent=settings.fg.peripheral_percent;m.peripheral_method=settings.fg.peripheral_method;
    m.depth_bits=settings.fg.depth_bits;m.motion_bits=settings.fg.motion_bits;
    m.configuration=configuration(settings.fg);
    std::lock_guard lock(mutex);
    if(snapshots().record(cmd,m)){++captured_guides;last_guide_frame=render_frame;}
} catch(...) {}
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_AttachUEVRFrameSource(void* backend,
    CheekyFgRegisterHookFn install,CheekyFgUnregisterHookFn remove) noexcept try {
    using namespace cheeky::foveated_dlss;
    std::lock_guard lock(source_mutex);
    if(source_hook>=0&&!source_abandoned.load())return true;
    // On a complete plugin unload UEVR clears its own inline hooks after
    // FreeLibrary. The resident runtime must not reuse that old hook ID or
    // trampoline when a fresh adapter attaches later.
    source_hook=-1;
    rhi_hooks={};rhi_hook_count=0;selected_rhi=-1;phase_traces=0;
    if(!backend||!install||!remove){strcpy_s(source_message,"UEVR did not provide the native hook API");return false;}
    const auto target=uevr_frames::locate(static_cast<HMODULE>(backend));
    if(!target.function){strcpy_s(source_message,"Installed UEVR runtime layout is not supported by the plugin frame hook");trace_event("FG frame source: %s",source_message);return false;}
    const auto image=uevr_frames::image(static_cast<HMODULE>(backend));
    const auto roots=uevr_frames::rhi_dispatchers(image);
    static const auto detours=rhi_detours(std::make_index_sequence<8>{});
    for(const auto& root:roots){auto& hook=rhi_hooks[rhi_hook_count];
        hook.begin=reinterpret_cast<std::uintptr_t>(backend)+root.begin;hook.end=reinterpret_cast<std::uintptr_t>(backend)+root.end;
        hook.id=install(reinterpret_cast<void*>(hook.begin),reinterpret_cast<void*>(detours[rhi_hook_count]),&hook.original);
        if(hook.id<0){for(unsigned n=0;n<rhi_hook_count;++n)remove(rhi_hooks[n].id);rhi_hook_count=0;
            strcpy_s(source_message,"UEVR could not install a verified RHI command hook");return false;}
        ++rhi_hook_count;
    }
    const auto id=install(target.function,reinterpret_cast<void*>(&enqueue_render_poses),&original_enqueue);
    if(id<0){for(unsigned n=0;n<rhi_hook_count;++n)remove(rhi_hooks[n].id);rhi_hook_count=0;
        strcpy_s(source_message,"UEVR could not install the plugin frame hook");return false;}
    source_hook=id;unregister_source=remove;source_abandoned=false;++source_epoch;strcpy_s(source_message,"Plugin hook: UEVR render-pose frame argument");
    trace_event("FG frame source attached: backend=%p table_rva=0x%X target=%p hook=%d rhi_entries=%u",backend,target.table_rva,target.function,id,rhi_hook_count);return true;
} catch(...){return false;}
extern "C" __declspec(dllexport) void __cdecl CheekyFG_DetachUEVRFrameSource() noexcept {
    using namespace cheeky::foveated_dlss;
    std::lock_guard lock(source_mutex);
    source_abandoned=true;++source_epoch;
    if(source_hook>=0&&unregister_source)unregister_source(source_hook);
    for(unsigned n=0;n<rhi_hook_count;++n)if(rhi_hooks[n].id>=0&&unregister_source)unregister_source(rhi_hooks[n].id);
    rhi_hook_count=0;selected_rhi=-1;
    source_hook=-1;unregister_source=nullptr;++source_epoch;render_frame=0;
    strcpy_s(source_message,"Plugin frame source detached");
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetCaptureHealth(CheekyFgCaptureHealth* out) noexcept {
    using namespace cheeky::foveated_dlss;
    if(!out||out->abi!=1||out->size!=sizeof(*out))return false;
    std::lock_guard lock(source_mutex);out->plugin_hook=source_hook>=0&&!source_abandoned.load();
    out->frame_calls=source_calls;out->last_guide_frame=last_guide_frame;out->captured_guides=captured_guides;
    out->missing_frame_guides=missing_frame_guides;out->color_copies=color_copies;
    strcpy_s(out->source,selected_rhi.load()>=0?"Plugin hook: RHI stereo phases":source_message);
    std::lock_guard captures(mutex);const auto& match=last_color_match;
    out->color_frame=last_color_frame;out->match_frame=match.frame;out->latest_frame=match.latest_frame;
    out->latest_evaluation=match.latest;out->consumed_evaluation=match.consumed;
    out->eligible=match.eligible;out->pending=match.pending;out->foreign=match.foreign;out->cancelled=match.cancelled;
    strcpy_s(out->match_reason,match.reason);return true;
}
extern "C" __declspec(dllexport) void __cdecl CheekyFG_SetRenderFrameV1(std::uint64_t frame) noexcept {
    cheeky::foveated_dlss::render_frame=frame;
    cheeky::foveated_dlss::ticket_epoch=cheeky::foveated_dlss::source_epoch.load();
    cheeky::foveated_dlss::rhi_ticket=false;
    cheeky::foveated_dlss::frame_phase={};
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetCaptureStamp(ID3D12Resource* color,CheekyFgCaptureStamp* out) noexcept {
    using namespace cheeky::foveated_dlss;
    if(!out||out->abi!=1||out->size!=sizeof(*out)||!enabled())return false;
    std::lock_guard lock(mutex);out->token=store().colors.token(color);out->render_frame=store().colors.frame(color);
    last_color_frame=out->render_frame;last_color_match=snapshots().inspect(out->render_frame);
    if(out->token)last_color_match.reason="Matched";
    else if(!out->render_frame)last_color_match.reason="Color copy has no completed stereo interval";
    static ULONGLONG next_trace{};const auto now=GetTickCount64();
    if(now>=next_trace){next_trace=now+2000;const auto& m=last_color_match;
        trace_event("FG match color_frame=%llu token=%llu last_guide_frame=%llu latest_frame=%llu eligible=%u pending=%u foreign=%u cancelled=%u latest_eval=%llu consumed=%llu failed=%llu reason=%s",
            out->render_frame,out->token,last_guide_frame.load(),m.latest_frame,m.eligible,m.pending,m.foreign,m.cancelled,m.latest,m.consumed,m.failed,m.reason);}
    return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetControl(CheekyFgControl* out) noexcept {
    if (!out || out->abi != CHEEKY_FG_CONTROL_ABI || out->size != sizeof(*out)) return false;
    out->enabled = cheeky::foveated_dlss::current_settings().fg.enabled;
    out->configuration=cheeky::foveated_dlss::configuration(cheeky::foveated_dlss::current_settings().fg);
    {
        using namespace cheeky::foveated_dlss;
        std::lock_guard lock(mutex);
        static bool previous{};
        static std::uint64_t previous_configuration{};
        if (previous != bool(out->enabled)||previous_configuration!=out->configuration) {
            snapshots().invalidate();
            store().colors.invalidate();
            previous=out->enabled != 0;
            previous_configuration=out->configuration;
        }
    }
    return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetTransportOptionsV1(CheekyFgTransportOptions* out) noexcept {
    if(!out||out->abi!=1||out->size!=sizeof(*out))return false;
    const auto settings=cheeky::foveated_dlss::current_settings().fg;
    out->color_capture_mode=settings.color_capture_mode==1u?1u:0u;
    out->upload_mode=settings.upload_mode==1u?1u:0u;
    out->latency_diagnostics=settings.latency_diagnostics==1u?1u:0u;return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetGuideMemoryV1(CheekyFgMemory* out) noexcept {
    if(!out||out->abi!=1||out->size!=sizeof(*out))return false;
    using namespace cheeky::foveated_dlss;
    *out=CheekyFgMemory{};std::lock_guard lock(mutex);cheeky::fg_memory::Collector collect;
    for(const auto& generation:store().generations)generation->memory(*out,collect);
    out->guides_supported=1;out->guide_tick=GetTickCount64();return true;
}
extern "C" __declspec(dllexport) std::uint64_t __cdecl CheekyFG_GetCaptureToken(ID3D12Resource* color) noexcept {
    using namespace cheeky::foveated_dlss;
    if(!enabled())return 0;
    std::lock_guard lock(mutex);return store().colors.token(color);
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_AcquirePair(CheekyFgPair* out) noexcept {
    using namespace cheeky::foveated_dlss;
    if(!out || out->abi!=CHEEKY_FG_PAIR_ABI || out->size!=sizeof(CheekyFgPair)||!enabled())return false;
    // Capture the persisted UEVR Lua setting alongside this guide pair.
    // Read settings before taking the snapshot mutex (consistent with enabled()).
    const auto settings=current_settings().fg;
    const auto mode=settings.diagnostic_mode;
    std::lock_guard lock(mutex);
    if(!snapshots().acquire(*out))return false;
    out->diagnostic_mode=mode;
    out->enable_frame_id=settings.backbuffer_frame_id ? 1u : 0u;
    out->fg_queue_priority=settings.queue_priority <= 2u ? settings.queue_priority : 1u;
    out->release_point=settings.release_point<=3u?settings.release_point:0u;
    out->submit_repeats=settings.submit_repeats?1u:0u;
    return true;
}
extern "C" __declspec(dllexport) void __cdecl CheekyFG_ReleasePair(CheekyFgPair* p) noexcept {
    if(!p||p->abi!=CHEEKY_FG_PAIR_ABI)return;
    for(auto& eye:p->eyes){if(eye.depth)eye.depth->Release();if(eye.motion)eye.motion->Release();eye={};}
    if(p->producer_queue)p->producer_queue->Release();p->producer_queue=nullptr;
    if(p->lease)p->lease->Release();p->lease=nullptr;
}

namespace cheeky::foveated_dlss {
CheekyFgStatus fg_status() noexcept {
    CheekyFgStatus status;
    const auto layer = GetModuleHandleW(L"CheekyOpenXRLayer.dll");
    status.layer_present = layer != nullptr;
    const auto query = layer ? reinterpret_cast<CheekyFgStatusFn>(GetProcAddress(layer, "CheekyFG_GetStatus")) : nullptr;
    if (query && query(&status)) status.controls_supported = true;
    return status;
}
const char* fg_state_name(CheekyFgState value) noexcept {
    switch (value) {
    case CheekyFgState::off: return "Off";
    case CheekyFgState::waiting_for_stereo: return "Waiting for a running D3D12 OpenXR stereo session";
    case CheekyFgState::starting: return "Starting at the next frame";
    case CheekyFgState::waiting_for_history: return "Waiting for frame history";
    case CheekyFgState::waiting_for_guides: return "Waiting for stereo NGX depth and motion vectors";
    case CheekyFgState::queue_mismatch: return "NGX and OpenXR command queues do not match";
    case CheekyFgState::ngx_unavailable: return "NVIDIA NGX frame generation unavailable";
    case CheekyFgState::generation_failed: return "Frame generation warming up or failed; displaying native frames";
    case CheekyFgState::generating: return "Generating frames";
    case CheekyFgState::unsupported_submission: return "Unsupported OpenXR frame layout (see OpenXR log)";
    case CheekyFgState::presenter_failed: return "Could not initialize the D3D12 presenter";
    case CheekyFgState::presentation_failed: return "FG presentation unavailable; displaying native frames";
    case CheekyFgState::guide_frame_mismatch: return "Waiting for matching color and guide frames";
    }
    return "Unknown";
}
CheekyFgDiagnostic fg_diagnostic() noexcept {
    CheekyFgDiagnostic diagnostic;
    const auto layer=GetModuleHandleW(L"CheekyOpenXRLayer.dll");
    const auto query=layer?reinterpret_cast<CheekyFgDiagnosticFn>(GetProcAddress(layer,"CheekyFG_GetDiagnostic")):nullptr;
    if(query)query(&diagnostic);
    if(fg_status().state==CheekyFgState::guide_frame_mismatch){
        CheekyFgCaptureHealth health;CheekyFG_GetCaptureHealth(&health);
        if(!health.plugin_hook)strcpy_s(diagnostic.message,health.source);
    }
    return diagnostic;
}
bool fg_performance(CheekyFgPerformance& out) noexcept {
    const auto layer=GetModuleHandleW(L"CheekyOpenXRLayer.dll");
    const auto query=layer?reinterpret_cast<CheekyFgPerformanceFn>(GetProcAddress(layer,"CheekyFG_GetPerformance")):nullptr;
    return query&&query(&out);
}
bool fg_cadence(CheekyFgCadence& out) noexcept {
    const auto layer=GetModuleHandleW(L"CheekyOpenXRLayer.dll");
    const auto query=layer?reinterpret_cast<CheekyFgCadenceFn>(GetProcAddress(layer,"CheekyFG_GetCadenceV1")):nullptr;
    return query&&query(&out);
}
bool fg_overhead(CheekyFgOverhead& out) noexcept {
    const auto layer=GetModuleHandleW(L"CheekyOpenXRLayer.dll");
    const auto query=layer?reinterpret_cast<CheekyFgOverheadFn>(GetProcAddress(layer,"CheekyFG_GetOverheadV1")):nullptr;
    const bool supported=query&&query(&out);
    std::lock_guard lock(mutex);snapshots().collect_completed();const auto guide=snapshots().capture_gpu();
    out.guide_samples=guide.samples;out.guide_ms=guide.mean;out.guide_last_ms=guide.last;return supported;
}
bool fg_presentation(CheekyFgPresentation& out) noexcept {
    const auto layer=GetModuleHandleW(L"CheekyOpenXRLayer.dll");
    const auto query=layer?reinterpret_cast<CheekyFgPresentationFn>(GetProcAddress(layer,"CheekyFG_GetPresentationV1")):nullptr;
    return query&&query(&out);
}
bool fg_experiment_diagnostics(CheekyFgExperimentDiagnostics& out) noexcept {
    const auto layer=GetModuleHandleW(L"CheekyOpenXRLayer.dll");
    const auto query=layer?reinterpret_cast<CheekyFgExperimentDiagnosticsFn>(GetProcAddress(layer,"CheekyFG_GetExperimentDiagnosticsV1")):nullptr;
    return query&&query(&out);
}
bool fg_memory_diagnostics(CheekyFgMemory& out) noexcept {
    if(out.abi!=1||out.size!=sizeof(out))return false;
    out=CheekyFgMemory{};
    const auto layer=GetModuleHandleW(L"CheekyOpenXRLayer.dll");
    const auto query=layer?reinterpret_cast<CheekyFgMemoryFn>(GetProcAddress(layer,"CheekyFG_GetMemoryV1")):nullptr;
    const bool supported=query&&query(&out);
    CheekyFgMemory guides;CheekyFG_GetGuideMemoryV1(&guides);
    out.categories[fg_memory::guide_depth]=guides.categories[fg_memory::guide_depth];
    out.categories[fg_memory::guide_motion]=guides.categories[fg_memory::guide_motion];
    out.guides_supported=guides.guides_supported;out.guide_tick=guides.guide_tick;return supported;
}
}
