#include "fg_capture.hpp"
#include "settings.hpp"
#include "fg_snapshot.hpp"
#include "fg_frame_phase.hpp"
#include "fg_source_lifecycle.hpp"
#include "../shared/fg_uevr_mode.hpp"
#include "gaze_foveation.hpp"
#include "fg_sr_region.hpp"
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
std::atomic<bool> pending_submissions{};
std::atomic<bool> standalone_host{};
// Two different concepts: desired FG setting vs. whether the current UEVR
// rendering method can supply native stereo. Never overwrite FgEnabled.
std::atomic<int> uevr_fg_rendering_mode{cheeky::fg::no_uevr_host};
std::atomic<std::uint64_t> uevr_fg_mode_epoch{1};
std::atomic<std::uint64_t> observed_fg_lists{},recognized_fg_guides{};
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
// Guarded by mutex. This is *not* the UEVR RHI frame-source epoch.
// Bump only for tracked game NGX-SR feature create/release, never for FG's
// internal features or for a live pacing-mode switch.
std::uint64_t game_sr_revision{1};
// No resource is retained here. This is only geometry from the last valid
// physical-eye NGX evaluation. Guarded by `mutex`.
CheekyFgStaticEyeCropsV1 static_eye_crops;
std::uint64_t static_eye_crop_configuration{},static_eye_crop_mapping{};

// GPU recordings can outlive a runtime detach. Keep queue generations until
// their recorded work and every consumer lease have retired.
struct SnapshotStore {
    FgColorProvenance colors;
    std::vector<std::unique_ptr<FgSnapshots>> generations;
    FgSnapshots* active{};
    bool capture_was_enabled{};
    SnapshotStore(){generations.push_back(std::make_unique<FgSnapshots>());active=generations.back().get();}
    void collect(){std::erase_if(generations,[&](auto& p){p->retire();return p.get()!=active&&p->idle();});}
};
SnapshotStore& store(){static auto* value=new SnapshotStore;return *value;}
FgSnapshots& snapshots(){return *store().active;}
// FG-off is a producer lifecycle boundary, not just a presentation toggle.
// Move the old generation aside so its snapshot textures can be destroyed
// once the executing command lists and any consumers have retired. Never
// release in-flight D3D12 resources synchronously at a Lua toggle.
void synchronize_capture_enabled_locked(bool requested) {
    auto& state=store();
    if(state.capture_was_enabled==requested)return;
    state.capture_was_enabled=requested;
    state.active->invalidate();
    state.colors.invalidate();
    last_color_match={};
    last_color_frame=0;
    if(!requested){
        state.generations.push_back(std::make_unique<FgSnapshots>());
        state.active=state.generations.back().get();
        trace_event("FG producer disabled: retiring prior guide generation; new captures blocked");
    } else {
        trace_event("FG producer enabled: starting fresh guide generation");
    }
    state.collect();
    pending_submissions=std::any_of(state.generations.begin(),state.generations.end(),
        [](const auto& p){return p->pending_submissions();})||state.colors.has_pending();
}
bool enabled() noexcept {
    return current_settings().fg.enabled &&
        cheeky::fg::uevr_mode_supports_fg(uevr_fg_rendering_mode.load(std::memory_order_acquire));
}
bool hookless_selected(const Settings::FrameGeneration& s) noexcept {
    return standalone_host.load(std::memory_order_acquire) || s.hookless_mode3;
}
bool hookless_selected() noexcept {return hookless_selected(current_settings().fg);}
std::uint64_t configuration(const Settings::FrameGeneration& s) noexcept {
    // Pacing policy is NOT a texture/NGX feature configuration. Hashing
    // release_point here stopped FG, invalidated stereo snapshots and reset
    // both NGX histories every time a release mode was changed live.
    // The policy is still latched separately on each acquired stereo pair.
    std::uint64_t hash=1469598103934665603ull;
    for(const auto n:{unsigned(s.foveated),unsigned(s.match_sr_region),s.compact_storage,s.guide_capture_mode,s.resolution_percent,s.peripheral_percent,
        s.peripheral_method,s.depth_bits,s.motion_bits,unsigned(hookless_selected(s)),std::bit_cast<unsigned>(s.width),
        std::bit_cast<unsigned>(s.height),std::bit_cast<unsigned>(s.x),std::bit_cast<unsigned>(s.y),std::bit_cast<unsigned>(s.feather)})
        hash=(hash^n)*1099511628211ull;
    return hash;
}
// Unlike the FG-specific settings, the main SR foveation toggle changes
// both OpenXR capture geometry and the guide source used by DLSS-G. The
// presenter uses this key to retire the old pipeline at the next xrWaitFrame.
std::uint64_t configuration(const Settings& s) noexcept {
    auto hash=fg_sr_mode_configuration(configuration(s.fg),s.enabled);
    if(s.fg.foveated)hash=(hash^unsigned(s.center_mode))*1099511628211ull;
    if(s.fg.match_sr_region&&s.fg.foveated){
        // SR's static region can change independently of FG. Gaze placement is
        // sampled per eye rather than hashed (it moves every frame).
        for(const auto n:{std::bit_cast<unsigned>(s.width),std::bit_cast<unsigned>(s.height),
                std::bit_cast<unsigned>(s.x_offset),std::bit_cast<unsigned>(s.height_offset),
                std::bit_cast<unsigned>(s.transition_width),unsigned(s.center_mode),
                unsigned(s.auto_stereo_alignment)})hash=(hash^n)*1099511628211ull;
    }
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
// A mode transition is also a producer-lifecycle transition. Retire the
// previous immutable snapshot generation when leaving native stereo; the
// old generation remains alive until GPU submissions and leases retire.
void fg_set_standalone_host(bool value) noexcept { standalone_host.store(value,std::memory_order_release); }
void fg_uevr_mode_attached() noexcept { fg_uevr_mode_published(0xFFFFFFFFu); }
void fg_uevr_mode_published(std::uint32_t mode) noexcept try {
    const int next=cheeky::fg::uevr_mode_value(mode);
    const int previous=uevr_fg_rendering_mode.exchange(next,std::memory_order_acq_rel);
    if(previous==next)return;
    // A monotonic counter, unlike current mode, records transitions that the
    // OpenXR presenter may not have observed while it was not being called.
    const auto epoch=uevr_fg_mode_epoch.fetch_add(1,std::memory_order_acq_rel)+1;
    std::lock_guard lock(mutex);
    synchronize_capture_enabled_locked(enabled());
    trace_event("FG UEVR rendering mode changed %d -> %d epoch=%llu: %s (FgEnabled preference preserved)",
        previous,next,static_cast<unsigned long long>(epoch),cheeky::fg::uevr_mode_supports_fg(next)?"Native Stereo / FG allowed":"FG paused; non-native/unknown mode");
} catch(...) {}
void fg_uevr_mode_detached() noexcept { fg_uevr_mode_published(0xFFFFFFFFu); }
void fg_set_uevr_queue(ID3D12CommandQueue* q) noexcept try {
    std::lock_guard lock(mutex);
    synchronize_capture_enabled_locked(enabled());
    if(!enabled()){store().collect();return;}
    if(!q){
        // D3D11/OpenComposite has no D3D12 presentation queue. Its private
        // transport publishes the actual guide queue on each evaluation.
        if(hookless_selected())return;
        snapshots().invalidate();store().colors.invalidate();return;
    }
    store().collect();
    // In hook-free mode 3 the UEVR renderer accessor is only the provisional
    // queue. Once a real NGX guide submission identified its executing queue,
    // do not overwrite that proof on the next (per-frame) UEVR runtime tick.
    // A different device still forces the normal queue-generation transition.
    if(hookless_selected()&&snapshots().has_discovered_submission_queue()&&
       snapshots().same_queue_device(q))return;
    if(!snapshots().initialize(q)&&store().generations.size()<8){
        snapshots().invalidate();
        store().colors.invalidate();
        auto next=std::make_unique<FgSnapshots>();
        if(next->initialize(q)){store().generations.push_back(std::move(next));store().active=store().generations.back().get();}
    }
} catch(...) {}
void fg_note_submission(ID3D12CommandQueue* q,ID3D12GraphicsCommandList* list) noexcept try {
    std::lock_guard lock(mutex);++observed_fg_lists;
    const bool hookless=hookless_selected();
    for(auto& p:store().generations)
        recognized_fg_guides+=p->submitted(q,list,hookless&&p.get()==store().active);
    if(!hookless)store().colors.submitted(q,list,snapshots());
    pending_submissions=std::any_of(store().generations.begin(),store().generations.end(),
        [](const auto& p){return p->pending_submissions();})||store().colors.has_pending();
} catch(...) {}
void fg_retire_snapshots() noexcept try {std::lock_guard lock(mutex);synchronize_capture_enabled_locked(enabled());store().collect();} catch(...) {}
void fg_note_reset(ID3D12GraphicsCommandList* list) noexcept try {
    std::lock_guard lock(mutex);for(auto& p:store().generations)p->reset(list);
    store().colors.reset(list);
    pending_submissions=std::any_of(store().generations.begin(),store().generations.end(),
        [](const auto& p){return p->pending_submissions();})||store().colors.has_pending();
} catch(...) {}
void fg_note_color_copy(ID3D12GraphicsCommandList* list,ID3D12Resource* color) noexcept try {
    // Most copies (including our guide snapshots) are unrelated. Inspect the
    // registration before taking the mutex; snapshot recording already owns it.
    // Hook-free matching happens at xrEndFrame, after the complete stereo submission.
    // Do not require private UEVR color-copy queues or resource identities.
    if(!enabled()||hookless_selected()||!FgColorProvenance::registered(color))return;
    const bool hookless=false;
    const auto color_frame=hookless?0:ticket_epoch!=source_epoch.load()?0:rhi_ticket?frame_phase.color():render_frame;
    ++color_copies;
    std::lock_guard lock(mutex);store().colors.observe(list,color,snapshots(),color_frame,hookless);
    pending_submissions=true;
    if(!hookless&&rhi_ticket&&phase_traces.fetch_add(1)<48)
        trace_event("FG color phase copy=%p command=%p render_entry=%llu color_content=%llu evaluations=%u previous_complete=%llu",
            color,list,render_frame,color_frame,frame_phase.evaluations,frame_phase.previous);
} catch(...) {}
bool fg_recording_tagged(ID3D12GraphicsCommandList* list) noexcept{return FgSnapshots::tagged(list);}
bool fg_has_pending_submissions() noexcept{return pending_submissions.load(std::memory_order_relaxed);}
void fg_abandon_frame_source() noexcept {source_abandoned=true;++source_epoch;}
void fg_remember_feature(const NgxHandle* handle,const NgxParameters* p) noexcept try {
    std::uint32_t flags{};if(!handle||!try_get_ngx_integer_bits(p,"DLSS.Feature.Create.Flags",flags))return;
    std::uint64_t revision{};
    {
        std::lock_guard lock(mutex);
        feature_flags[handle]=flags;
        revision=++game_sr_revision;
    }
    trace_event("FG game DLSS-SR feature created: handle=%p revision=%llu (FG history refresh on next complete stereo pair)",
        static_cast<const void*>(handle),static_cast<unsigned long long>(revision));
} catch(...) {}
void fg_forget_feature(const NgxHandle* handle) noexcept try {
    std::uint64_t revision{};
    {
        std::lock_guard lock(mutex);
        if(feature_flags.erase(handle))revision=++game_sr_revision;
    }
    if(revision)trace_event("FG game DLSS-SR feature released: handle=%p revision=%llu (awaiting recreated guides)",
        static_cast<const void*>(handle),static_cast<unsigned long long>(revision));
} catch(...) {}
void fg_publish_ngx(ID3D12GraphicsCommandList* cmd,const NgxHandle* handle,
                    const NgxParameters* p,NgxResult result) noexcept try {
    if(!enabled()||!cmd||!p||!handle||!ngx_succeeded(result))return;
    const auto settings=current_settings();
    const bool hookless=hookless_selected(settings.fg);
    if(!hookless&&(!render_frame||ticket_epoch!=source_epoch.load())){++missing_frame_guides;return;}
    // Hook-free mode has no frame ticket. Only the ordered, immutable guide
    // recordings and final color-copy enqueue are used to associate a pair.
    if(!hookless&&rhi_ticket)frame_phase.evaluated();
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
    m.render_frame=hookless?0:render_frame;
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
    m.focus_width=settings.fg.foveated ? settings.fg.width : 1.0f;
    m.focus_height=settings.fg.foveated ? settings.fg.height : 1.0f;
    m.focus_feather=settings.fg.feather;
    m.show_foveation_border=settings.fg.foveated&&settings.fg.show_border?1u:0u;
    // Each DLSS view maps to a different optical center. FG offsets are
    // relative to that stereo-aligned center, even with SR-size matching OFF.
    const auto view_id=static_cast<DlssViewId>(reinterpret_cast<std::uintptr_t>(handle));
    // Query the known calibration without changing SR eye-role counters.
    const auto assignment=stereo_eye_assignment(view_id);
    // Preserve the existing ordinal fallback before calibration. Once known,
    // use physical eyes; a verified shared source cannot form two FG guides.
    const FgGuideIdentity identity{assignment.calibrated ? (assignment.shared_source?2u:assignment.eye_index) :
        FgGuideIdentity::unknown,assignment.mapping_revision};
    const float signed_sr_x=assignment.assigned&&!assignment.shared_source
        ? ((bool(assignment.eye_index==1)!=settings.invert_stereo_x_offset) ? -settings.x_offset : settings.x_offset)
        : 0.f;
    const auto sr=fg_sr_prepared_for(view_id,w,h);
    const float base_u=sr.valid ? sr.center_u : fg_fallback_center(settings.width,signed_sr_x);
    const float base_v=sr.valid ? sr.center_v : fg_fallback_center(settings.height,settings.height_offset);
    m.focus_x=base_u-.5f+settings.fg.x;
    m.focus_y=base_v-.5f+settings.fg.y;
    if(settings.fg.foveated&&settings.fg.match_sr_region){
        // Use the exact SR crop that was actually prepared, including its
        // pixel rounding. Never invoke the SR gaze solver again from FG:
        // doing so advances the same temporal/crop state twice per eye.
        m.focus_width=sr.valid ? sr.region_width : settings.width;
        m.focus_height=sr.valid ? sr.region_height : settings.height;
        m.focus_feather=settings.transition_width;
        // Matching overrides manual FG offsets but retains stereo alignment.
        m.focus_x=base_u-.5f;
        m.focus_y=base_v-.5f;
    }
    m.resolution_percent=settings.fg.resolution_percent;
    m.peripheral_percent=settings.fg.peripheral_percent;m.peripheral_method=settings.fg.peripheral_method;
    m.depth_bits=settings.fg.depth_bits;m.motion_bits=settings.fg.motion_bits;
    m.guide_capture_mode=settings.fg.guide_capture_mode==1u?1u:0u;
    // Include the primary foveated-SR ON/OFF state, even if the game keeps
    // both NGX-SR feature handles alive while the mod changes rendering paths.
    const auto settings_configuration=configuration(settings);
    std::lock_guard lock(mutex);
    synchronize_capture_enabled_locked(settings.fg.enabled);
    if(!settings.fg.enabled)return;
    // Capture the source revision under the SAME lock as release/create and
    // snapshot recording. A stereo pair spanning a DLSS quality change is
    // rejected by FgSnapshots::acquire rather than mixing old/new guides.
    m.configuration=fg_source_configuration(settings_configuration,game_sr_revision);
    static std::uint64_t logged_mapping{};
    if(logged_mapping!=identity.mapping_revision){
        logged_mapping=identity.mapping_revision;
        trace_event("FG stereo identity revision=%llu calibrated=%u view=%llu physical_eye=%u (next complete pair resets history)",
            identity.mapping_revision,unsigned(assignment.calibrated),static_cast<unsigned long long>(view_id),identity.eye);
    }
    if(snapshots().record(cmd,m,identity)){
        pending_submissions=true;++captured_guides;last_guide_frame=m.render_frame;
        if(static_eye_crop_configuration!=m.configuration||static_eye_crop_mapping!=identity.mapping_revision){
            static_eye_crops.valid_mask=0;
            static_eye_crop_configuration=m.configuration;
            static_eye_crop_mapping=identity.mapping_revision;
        }
        // Only a verified physical-eye assignment can become a compact crop.
        if(identity.eye<2){
            static_eye_crops.eye[identity.eye]={m.focus_width,m.focus_height,m.focus_x,m.focus_y};
            static_eye_crops.valid_mask|=(1u<<identity.eye);
        }
    }
} catch(...) {}
}
// Called by the adapter before installing any UEVR-private inline hooks and
// after Lua settings changes. In hook-free mode the adapter never probes UEVR.
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_NeedsUEVRFrameSourceV1() noexcept {
    return !cheeky::foveated_dlss::hookless_selected();
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_AttachUEVRFrameSource(void* backend,
    CheekyFgRegisterHookFn install,CheekyFgUnregisterHookFn remove) noexcept try {
    using namespace cheeky::foveated_dlss;
    std::lock_guard lock(source_mutex);
    if(hookless_selected()){
        strcpy_s(source_message,"Hook-free FG: no UEVR frame hooks");
        return true; // Do not even inspect the installed UEVR runtime layout.
    }
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
    strcpy_s(source_message,hookless_selected()?"Hook-free FG: no UEVR frame hooks":"Plugin frame source detached");
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetCaptureHealth(CheekyFgCaptureHealth* out) noexcept {
    using namespace cheeky::foveated_dlss;
    if(!out||out->abi!=1||out->size!=sizeof(*out))return false;
    std::lock_guard lock(source_mutex);out->plugin_hook=source_hook>=0&&!source_abandoned.load();
    out->frame_calls=source_calls;out->last_guide_frame=last_guide_frame;out->captured_guides=captured_guides;
    out->missing_frame_guides=missing_frame_guides;out->color_copies=color_copies;
    strcpy_s(out->source,hookless_selected()?"Hook-free FG: capture-ordered stereo pairs":
        selected_rhi.load()>=0?"Plugin hook: RHI stereo phases":source_message);
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
    const bool hookless=hookless_selected();
    if(hookless){
        // Each eye may release its OpenXR image independently. The authoritative
        // association is the single xrEndFrame AFTER both releases, not here.
        out->token=out->render_frame=0;
        return true;
    }
    std::lock_guard lock(mutex);out->token=store().colors.token(color,&snapshots(),false);out->render_frame=store().colors.frame(color);
    last_color_frame=out->render_frame;
    last_color_match=hookless?snapshots().last_match:snapshots().inspect(out->render_frame);
    if(out->token)last_color_match.reason="Matched";
    else if(!hookless&&!out->render_frame)last_color_match.reason="Color copy has no completed stereo interval";
    static ULONGLONG next_trace{};const auto now=GetTickCount64();
    if(now>=next_trace){next_trace=now+2000;const auto& m=last_color_match;
        trace_event("FG match color_frame=%llu token=%llu last_guide_frame=%llu latest_frame=%llu eligible=%u pending=%u foreign=%u cancelled=%u latest_eval=%llu consumed=%llu failed=%llu reason=%s",
            out->render_frame,out->token,last_guide_frame.load(),m.latest_frame,m.eligible,m.pending,m.foreign,m.cancelled,m.latest,m.consumed,m.failed,m.reason);}
    return true;
}
extern "C" __declspec(dllexport) std::uint64_t __cdecl CheekyFG_CompleteHooklessFrameV1(
    std::uint32_t use_for_fg) noexcept {
    using namespace cheeky::foveated_dlss;
    if(!enabled()||!hookless_selected())return 0;
    std::lock_guard lock(mutex);
    auto& guides=snapshots();
    const auto token=use_for_fg?guides.token(0,true):0;
    if(!token)guides.discard_hookless_interval();
    last_color_frame=0;
    last_color_match=guides.last_match;
    if(!use_for_fg)last_color_match.reason="Hook-free native frame skipped at xrEndFrame";
    else if(token)last_color_match.reason="Matched at xrEndFrame";
    static ULONGLONG next_trace{};const auto now=GetTickCount64();
    if(now>=next_trace){next_trace=now+2000;const auto& m=last_color_match;
        trace_event("FG end-frame match token=%llu eligible=%u pending=%u foreign=%u cancelled=%u latest_eval=%llu consumed=%llu failed=%llu reason=%s",
            token,m.eligible,m.pending,m.foreign,m.cancelled,m.latest,m.consumed,m.failed,m.reason);
    }
    return token;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetControl(CheekyFgControl* out) noexcept {
    if (!out || out->abi != CHEEKY_FG_CONTROL_ABI || out->size != sizeof(*out)) return false;
    using namespace cheeky::foveated_dlss;
    const auto settings=current_settings();
    out->enabled=settings.fg.enabled && cheeky::fg::uevr_mode_supports_fg(uevr_fg_rendering_mode.load(std::memory_order_acquire));
    out->configuration=configuration(settings);
    // This transition does not require users to uncheck/recheck FG. The
    // OpenXR layer observes the changed key on its next xrWaitFrame and
    // drains/rebuilds the presenter, preserving the FG enable request.
    static std::atomic<int> previous_sr{-1};
    const int sr=settings.enabled?1:0;
    const int before=previous_sr.exchange(sr,std::memory_order_relaxed);
    if(before>=0&&before!=sr&&settings.fg.enabled)
        trace_event("FG main DLSS-SR foveation toggled %s -> %s: requesting automatic FG presenter rebuild",
            before?"ON":"OFF",sr?"ON":"OFF");
    {
        using namespace cheeky::foveated_dlss;
        std::lock_guard lock(mutex);
        synchronize_capture_enabled_locked(bool(out->enabled));
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
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetUevrModeV1(CheekyFgUevrModeV1* out) noexcept {
    if(!out || out->abi!=1 || out->size!=sizeof(*out))return false;
    const int mode=cheeky::foveated_dlss::uevr_fg_rendering_mode.load(std::memory_order_acquire);
    out->attached=mode!=cheeky::fg::no_uevr_host;
    out->known=mode>=0;
    out->mode=mode>=0?static_cast<std::uint32_t>(mode):0xFFFFFFFFu;
    return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetUevrModeV2(CheekyFgUevrModeV2* out) noexcept {
    if(!out || out->abi!=2 || out->size!=sizeof(*out))return false;
    using namespace cheeky::foveated_dlss;
    // Snapshot both mode and epoch in a consistent order. The mode publisher
    // updates the epoch immediately after changing the mode.
    for(unsigned n=0;n<4;++n){
        const auto before=uevr_fg_mode_epoch.load(std::memory_order_acquire);
        const auto mode=uevr_fg_rendering_mode.load(std::memory_order_acquire);
        const auto after=uevr_fg_mode_epoch.load(std::memory_order_acquire);
        if(before!=after)continue;
        out->attached=mode!=cheeky::fg::no_uevr_host;
        out->known=mode>=0;
        out->mode=mode>=0?static_cast<std::uint32_t>(mode):0xFFFFFFFFu;
        out->transition_epoch=after;
        return true;
    }
    // Fail closed on an unstable concurrent rendering-mode transition.
    out->attached=1;out->known=0;out->mode=0xFFFFFFFFu;
    out->transition_epoch=uevr_fg_mode_epoch.load(std::memory_order_acquire);
    return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetTransportOptionsV1(CheekyFgTransportOptions* out) noexcept {
    if(!out||out->abi!=1||out->size!=sizeof(*out))return false;
    const auto settings=cheeky::foveated_dlss::current_settings().fg;
    out->color_capture_mode=settings.color_capture_mode==1u?1u:0u;
    out->upload_mode=settings.upload_mode==1u?1u:0u;
    out->latency_diagnostics=settings.latency_diagnostics==1u?1u:0u;return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetStorageOptionsV1(CheekyFgStorageOptions* out) noexcept {
    if(!out||out->abi!=1||out->size<sizeof(*out))return false;
    const auto settings=cheeky::foveated_dlss::current_settings();
    const auto& s=settings.fg;
    // Dynamic gaze uses existing full-eye native captures as previous/current
    // color history. No additional full-resolution history copy is allocated.
    out->compact=s.compact_storage==1u&&s.foveated&&
        settings.center_mode==cheeky::foveated_dlss::FoveationCenterMode::fixed &&
        !settings.eye_calibration_continuous;
    out->peripheral_percent=s.peripheral_percent;
    out->width=s.width;out->height=s.height;out->x=s.x;out->y=s.y;out->feather=s.feather;return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetStaticEyeCropsV1(CheekyFgStaticEyeCropsV1* out) noexcept {
    if(!out||out->abi!=1||out->size<sizeof(*out))return false;
    std::lock_guard lock(cheeky::foveated_dlss::mutex);
    const auto settings=cheeky::foveated_dlss::current_settings();
    const auto desired=cheeky::foveated_dlss::fg_source_configuration(
        cheeky::foveated_dlss::configuration(settings),cheeky::foveated_dlss::game_sr_revision);
    *out=cheeky::foveated_dlss::static_eye_crops;
    // Cache is unavailable immediately after a configuration/calibration
    // transition. Full-eye fallback is safe during those startup frames.
    if(settings.center_mode!=cheeky::foveated_dlss::FoveationCenterMode::fixed||
       desired!=cheeky::foveated_dlss::static_eye_crop_configuration)out->valid_mask=0;
    return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetDirectOutputOptionsV1(CheekyFgDirectOutput* out) noexcept {
    if(!out||out->abi!=1||out->size!=sizeof(*out))return false;
    const auto settings=cheeky::foveated_dlss::current_settings().fg;
    out->requested=settings.enabled&&settings.direct_xr_output==1u;return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetDirectShadingOptionsV1(CheekyFgDirectShading* out) noexcept {
    if(!out||out->abi!=1||out->size!=sizeof(*out))return false;
    out->requested_mode=cheeky::foveated_dlss::current_settings().fg.direct_peripheral_shading;return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetGuideMemoryV1(CheekyFgMemory* out) noexcept {
    if(!out||out->abi!=1||out->size!=sizeof(*out))return false;
    using namespace cheeky::foveated_dlss;
    *out=CheekyFgMemory{};std::lock_guard lock(mutex);
    synchronize_capture_enabled_locked(enabled());
    store().collect();
    cheeky::fg_memory::Collector collect;
    for(const auto& generation:store().generations)generation->memory(*out,collect);
    static std::uint64_t next_diagnostic{};const auto now=GetTickCount64();
    if(now>=next_diagnostic){
        std::uint64_t prepared{},raw{},fallbacks{},equivalent{};unsigned slots{},leased{};
        for(const auto& generation:store().generations)generation->guide_diagnostics(prepared,raw,fallbacks,slots,leased,equivalent);
        const auto actual=out->categories[cheeky::fg_memory::guide_depth].bytes+out->categories[cheeky::fg_memory::guide_motion].bytes;
        const auto d=snapshots().submission_stats();
        trace_event("FG guide capture prepared_eyes=%llu raw_eyes=%llu fallbacks=%llu allocated_eye_slots=%u consumer_leased_eyes=%u actual_bytes=%llu equivalent_raw_snapshot_bytes=%llu pending_submissions=%u submitted_unretired=%u retired=%u foreign=%u cancelled=%u latest_pending_eval=%llu latest_pending_recording=%llu observed_submit_lists=%llu recognized_guide_submits=%llu execution_queue_discovered=%u (worker prepared rows exclude borrowed guides)",
            prepared,raw,fallbacks,slots,leased,actual,equivalent,d.pending,d.submitted,d.retired,d.foreign,d.cancelled,d.pending_id,d.pending_recording,observed_fg_lists.load(),recognized_fg_guides.load(),
            unsigned(snapshots().has_discovered_submission_queue()));next_diagnostic=now+1000;
    }
    out->guides_supported=1;out->guide_tick=GetTickCount64();return true;
}
extern "C" __declspec(dllexport) std::uint64_t __cdecl CheekyFG_GetCaptureToken(ID3D12Resource* color) noexcept {
    using namespace cheeky::foveated_dlss;
    if(!enabled())return 0;
    if(hookless_selected())return 0; // Only xrEndFrame may claim a hook-free stereo pair.
    std::lock_guard lock(mutex);return store().colors.token(color,&snapshots(),false);
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
    out->enable_frame_id=!hookless_selected(settings)&&settings.backbuffer_frame_id ? 1u : 0u;
    out->fg_queue_priority=settings.queue_priority <= 2u ? settings.queue_priority : 1u;
    out->release_point=settings.release_point<=4u?settings.release_point:0u;
    out->submit_repeats=settings.submit_repeats?1u:0u;
    return true;
}
extern "C" __declspec(dllexport) void __cdecl CheekyFG_ReleasePair(CheekyFgPair* p) noexcept {
    if(!p||p->abi!=CHEEKY_FG_PAIR_ABI)return;
    for(auto& eye:p->eyes){for(auto* resource:{eye.depth,eye.motion,eye.peripheral_depth,eye.peripheral_motion})if(resource)resource->Release();eye={};}
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
    case CheekyFgState::paused_rendering_mode: return "Paused - UEVR Native Stereo required";
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
bool fg_pacing_timeline(CheekyFgPacingTimeline& out) noexcept {
    const auto layer=GetModuleHandleW(L"CheekyOpenXRLayer.dll");
    const auto query=layer?reinterpret_cast<CheekyFgPacingTimelineFn>(GetProcAddress(layer,"CheekyFG_GetPacingTimelineV1")):nullptr;
    return query&&query(&out);
}
bool fg_native_submission_latency(CheekyFgNativeSubmissionLatency& out) noexcept {
    const auto layer=GetModuleHandleW(L"CheekyOpenXRLayer.dll");
    const auto query=layer?reinterpret_cast<CheekyFgNativeSubmissionLatencyFn>(GetProcAddress(layer,"CheekyFG_GetNativeSubmissionLatencyV1")):nullptr;
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
bool fg_direct_output_diagnostics(CheekyFgDirectOutput& out) noexcept {
    const auto layer=GetModuleHandleW(L"CheekyOpenXRLayer.dll");
    const auto query=layer?reinterpret_cast<CheekyFgDirectOutputFn>(GetProcAddress(layer,"CheekyFG_GetDirectOutputDiagnosticsV1")):nullptr;
    return query&&query(&out);
}
bool fg_direct_output_details(CheekyFgDirectOutputDetails& out) noexcept {
    const auto layer=GetModuleHandleW(L"CheekyOpenXRLayer.dll");
    const auto query=layer?reinterpret_cast<CheekyFgDirectOutputDetailsFn>(GetProcAddress(layer,"CheekyFG_GetDirectOutputDetailsV1")):nullptr;
    return query&&query(&out);
}
bool fg_direct_shading_diagnostics(CheekyFgDirectShading& out) noexcept {
    const auto layer=GetModuleHandleW(L"CheekyOpenXRLayer.dll");
    const auto query=layer?reinterpret_cast<CheekyFgDirectShadingFn>(GetProcAddress(layer,"CheekyFG_GetDirectShadingDiagnosticsV1")):nullptr;
    return query&&query(&out);
}
}
