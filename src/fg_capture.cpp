#include "fg_capture.hpp"
#include "settings.hpp"
#include "fg_snapshot.hpp"
#include "fg_source_lifecycle.hpp"
#include "quad_view_bridge.hpp"
#include "../shared/cheeky_quad_fg.hpp"
#include <d3d11.h>
#include "../shared/fg_uevr_mode.hpp"
#include "gaze_foveation.hpp"
#include "fg_sr_region.hpp"
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
// Two different concepts: desired FG setting vs. whether the current UEVR
// rendering method can supply native stereo. Never overwrite FgEnabled.
std::atomic<int> uevr_fg_rendering_mode{cheeky::fg::no_uevr_host};
std::atomic<std::uint64_t> uevr_fg_mode_epoch{1};
std::atomic<std::uint64_t> observed_fg_lists{},recognized_fg_guides{};
std::atomic<std::uint64_t> captured_guides{};
FgSnapshots::Match last_color_match;
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
struct ColorMetadata {CheekyFgEye eye;std::uint64_t configuration{},time{};};
ColorMetadata color_metadata[2];

// GPU recordings can outlive a runtime detach. Keep queue generations until
// their recorded work and every consumer lease have retired.
struct SnapshotStore {
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
    last_color_match={};
    if(!requested){
        state.generations.push_back(std::make_unique<FgSnapshots>());
        state.active=state.generations.back().get();
        trace_event("FG producer disabled: retiring prior guide generation; new captures blocked");
    } else {
        trace_event("FG producer enabled: starting fresh guide generation");
    }
    state.collect();
    pending_submissions=std::any_of(state.generations.begin(),state.generations.end(),
        [](const auto& p){return p->pending_submissions();});
}
bool enabled() noexcept {
    return current_settings().fg.enabled &&
        cheeky::fg::uevr_mode_supports_fg(uevr_fg_rendering_mode.load(std::memory_order_acquire));
}
std::uint64_t configuration(const Settings::FrameGeneration& s) noexcept {
    // Pacing policy is NOT a texture/NGX feature configuration. Hashing
    // release_point here stopped FG, invalidated stereo snapshots and reset
    // both NGX histories every time a release mode was changed live.
    // The policy is still latched separately on each acquired stereo pair.
    std::uint64_t hash=1469598103934665603ull;
    for(const auto n:{s.multiplier,s.dlssg_preset,unsigned(s.foveated),unsigned(s.match_sr_region),s.resolution_percent,s.peripheral_percent,
        s.optical.center_preset,s.optical.peripheral_preset,s.optical.center_percent,s.optical.peripheral_percent,s.optical.center_grid,s.optical.peripheral_grid,
        s.optical.center_validation,s.optical.peripheral_validation,
        s.center_method,s.peripheral_method,std::bit_cast<unsigned>(s.width),
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
    // D3D11/OpenComposite has no D3D12 presentation queue. Its private
    // transport publishes the actual guide queue on each evaluation.
    if(!q)return;
    store().collect();
    // The UEVR renderer accessor is only the provisional queue. Once a real
    // NGX guide submission identified its executing queue, do not overwrite
    // that proof on the next (per-frame) UEVR runtime tick. A different
    // device still forces the normal queue-generation transition.
    if(snapshots().has_discovered_submission_queue()&&snapshots().same_queue_device(q))return;
    if(!snapshots().initialize(q)&&store().generations.size()<8){
        snapshots().invalidate();
        auto next=std::make_unique<FgSnapshots>();
        if(next->initialize(q)){store().generations.push_back(std::move(next));store().active=store().generations.back().get();}
    }
} catch(...) {}
void fg_note_submission(ID3D12CommandQueue* q,ID3D12GraphicsCommandList* list) noexcept try {
    std::lock_guard lock(mutex);++observed_fg_lists;
    for(auto& p:store().generations)
        recognized_fg_guides+=p->submitted(q,list,p.get()==store().active);
    pending_submissions=std::any_of(store().generations.begin(),store().generations.end(),
        [](const auto& p){return p->pending_submissions();});
} catch(...) {}
void fg_retire_snapshots() noexcept try {std::lock_guard lock(mutex);synchronize_capture_enabled_locked(enabled());store().collect();} catch(...) {}
void fg_note_reset(ID3D12GraphicsCommandList* list) noexcept try {
    std::lock_guard lock(mutex);for(auto& p:store().generations)p->reset(list);
    pending_submissions=std::any_of(store().generations.begin(),store().generations.end(),
        [](const auto& p){return p->pending_submissions();});
} catch(...) {}
bool fg_recording_tagged(ID3D12GraphicsCommandList* list) noexcept{return FgSnapshots::tagged(list);}
bool fg_has_pending_submissions() noexcept{return pending_submissions.load(std::memory_order_relaxed);}
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
bool fg_is_registered_sr_feature(const NgxHandle* handle) noexcept try {
    if (!handle) return false;
    std::lock_guard lock(mutex);
    return feature_flags.contains(handle);
} catch (...) { return false; }
void fg_forget_feature(const NgxHandle* handle) noexcept try {
    std::uint64_t revision{};
    {
        std::lock_guard lock(mutex);
        if(feature_flags.erase(handle))revision=++game_sr_revision;
    }
    if(revision)trace_event("FG game DLSS-SR feature released: handle=%p revision=%llu (awaiting recreated guides)",
        static_cast<const void*>(handle),static_cast<unsigned long long>(revision));
} catch(...) {}
void fg_publish_shader11(ID3D11DeviceContext* context,const NgxHandle* handle,const NgxParameters* p) noexcept try {
    if(!enabled()||!context||!handle||!p)return;
    const auto settings=current_settings();
    if(!cheeky_fg_shader_only(settings.fg.center_method,settings.fg.peripheral_method,settings.fg.foveated))return;
    const auto layer=quad_view_module();const auto observe=layer?reinterpret_cast<CheekyFgShaderObserve11Fn>(GetProcAddress(layer,"CheekyFG_ObserveShaderD3D11V1")):nullptr;
    if(!observe)return;
    unsigned flags{};bool known{};
    {std::lock_guard lock(mutex);const auto it=feature_flags.find(handle);if(it!=feature_flags.end()){flags=it->second;known=true;}}
    if(!known&&!try_get_ngx_integer_bits(p,"DLSS.Feature.Create.Flags",flags))return;
    ID3D11Resource *depth{},*motion{};
    if(!ngx_succeeded(p->Get("Depth",&depth))||!ngx_succeeded(p->Get("MotionVectors",&motion))||!depth||!motion)return;
    ComPtr<ID3D11Texture2D> dt,mt;if(FAILED(depth->QueryInterface(IID_PPV_ARGS(&dt)))||FAILED(motion->QueryInterface(IID_PPV_ARGS(&mt))))return;
    D3D11_TEXTURE2D_DESC dd{},md{};dt->GetDesc(&dd);mt->GetDesc(&md);
    const unsigned w=ui(p,"OutWidth"),h=ui(p,"OutHeight");if(!w||!h)return;
    CheekyFgShaderSource11V1 source;source.context=context;source.depth=dt.Get();source.motion=mt.Get();
    auto& m=source.meta;m.width=w;m.height=h;m.feature=reinterpret_cast<uintptr_t>(handle);
    m.depth_width=ui(p,"DLSS.Render.Subrect.Dimensions.Width",dd.Width);
    m.depth_height=ui(p,"DLSS.Render.Subrect.Dimensions.Height",dd.Height);
    if(!m.depth_width)m.depth_width=ui(p,"Width",dd.Width);
    if(!m.depth_height)m.depth_height=ui(p,"Height",dd.Height);
    if(!m.depth_width)m.depth_width=dd.Width;
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
    // Native shader FG requires a calibrated physical eye and source crop.
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
        const auto size=sr.valid ? std::pair{sr.region_width,sr.region_height} :
            fg_sr_fallback_size(settings.width,settings.height,m.depth_width,m.depth_height,w,h);
        m.focus_width=size.first;
        m.focus_height=size.second;
        m.focus_feather=settings.transition_width;
        // Matching overrides manual FG offsets but retains stereo alignment.
        m.focus_x=base_u-.5f;
        m.focus_y=base_v-.5f;
    }
    m.resolution_percent=settings.fg.resolution_percent;
    m.peripheral_percent=settings.fg.peripheral_percent;m.peripheral_method=settings.fg.peripheral_method;
    m.center_method=settings.fg.center_method;
    m.optical=settings.fg.optical;
    // calibration_session 0 means OpenVR calibration (OpenComposite); the
    // layer binds it to its single running D3D11 OpenXR session.
    if(!assignment.calibrated||assignment.eye_index>1)return;
    source.session_generation=assignment.calibration_session;source.mapping_revision=assignment.mapping_revision;
    source.generation=stereo_view_generation(view_id);source.vertical_flip=assignment.vertical_flip;
    m.configuration=configuration(settings);
    for(unsigned eye=0;eye<2;++eye){
        if(!assignment.shared_source&&eye!=assignment.eye_index)continue;
        source.eye=eye;const auto& crop=assignment.source_crops[eye];
        if(!crop.valid)continue;
        source.crop[0]=crop.valid?crop.x:0;source.crop[1]=crop.valid?crop.y:0;
        source.crop[2]=crop.valid?crop.width:1;source.crop[3]=crop.valid?crop.height:1;
        {auto meta=m;meta.focus_width=std::min(1.f,m.focus_width/source.crop[2]);meta.focus_height=std::min(1.f,m.focus_height/source.crop[3]);
         meta.focus_x=(.5f+m.focus_x-source.crop[0])/source.crop[2]-.5f;
         meta.focus_y=(.5f+m.focus_y-source.crop[1])/source.crop[3]-.5f;if(source.vertical_flip)meta.focus_y=-meta.focus_y;
         meta.depth=nullptr;meta.motion=nullptr;
         std::lock_guard lock(mutex);color_metadata[eye]={meta,configuration(settings),GetTickCount64()};}
        if(!cheeky_fg_image_only(settings.fg.center_method,settings.fg.peripheral_method,settings.fg.foveated))observe(&source);
    }
}catch(...){}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetShaderOptionsV1(CheekyFgShaderOptionsV1* out) noexcept {
    if(!out||out->abi!=1||out->size!=sizeof(*out))return false;
    const auto s=current_settings();out->enabled=enabled();out->center_method=s.fg.center_method;
    out->peripheral_method=s.fg.peripheral_method;out->foveated=s.fg.foveated;return true;
}
void fg_publish_quad11(ID3D11DeviceContext* context,const NgxHandle* handle,const NgxParameters* p) noexcept try {
    if(!enabled()||!context||!handle||!p)return;
    const auto settings=current_settings();const auto quad=quad_view_snapshot();
    if(cheeky_fg_image_only(settings.fg.center_method,settings.fg.peripheral_method,settings.fg.foveated))return;
    const auto id=reinterpret_cast<uintptr_t>(handle),generation=stereo_view_generation(id);
    if(!quad_view_assignment(quad,id,generation))return;
    const auto layer=quad_view_module();const auto observe=layer?reinterpret_cast<CheekyQuadFgObserveFn>(GetProcAddress(layer,"CheekyQuadFG_ObserveD3D11V1")):nullptr;
    if(!observe)return;
    std::uint32_t flags{};bool known{};
    {std::lock_guard lock(mutex);const auto found=feature_flags.find(handle);if(found!=feature_flags.end()){flags=found->second;known=true;}}
    if(!known&&!try_get_ngx_integer_bits(p,"DLSS.Feature.Create.Flags",flags))return;
    ID3D11Resource* depth{};ID3D11Resource* motion{};
    if(!ngx_succeeded(p->Get("Depth",&depth))||!ngx_succeeded(p->Get("MotionVectors",&motion))||!depth||!motion)return;
    ComPtr<ID3D11Texture2D> dt,mt;if(FAILED(depth->QueryInterface(IID_PPV_ARGS(&dt)))||FAILED(motion->QueryInterface(IID_PPV_ARGS(&mt))))return;
    CheekyQuadFgSourceV1 source;source.context=context;source.depth=dt.Get();source.motion=mt.Get();source.generation=generation;
    auto& m=source.meta;m.feature=id;m.width=ui(p,"OutWidth");m.height=ui(p,"OutHeight");
    m.depth_width=ui(p,"DLSS.Render.Subrect.Dimensions.Width");m.depth_height=ui(p,"DLSS.Render.Subrect.Dimensions.Height");
    if(!m.depth_width)m.depth_width=ui(p,"Width");if(!m.depth_height)m.depth_height=ui(p,"Height");
    m.low_res_mv=(flags&2u)!=0;m.depth_inverted=(flags&8u)!=0;m.motion_jittered=(flags&4u)!=0;
    m.motion_width=m.low_res_mv?m.depth_width:m.width;m.motion_height=m.low_res_mv?m.depth_height:m.height;
    m.depth_base_x=ui(p,"DLSS.Input.Depth.Subrect.Base.X");m.depth_base_y=ui(p,"DLSS.Input.Depth.Subrect.Base.Y");
    m.motion_base_x=ui(p,"DLSS.Input.MV.Subrect.Base.X");m.motion_base_y=ui(p,"DLSS.Input.MV.Subrect.Base.Y");
    m.motion_scale_x=f(p,"MV.Scale.X",1);m.motion_scale_y=f(p,"MV.Scale.Y",1);m.jitter_x=f(p,"Jitter.Offset.X");m.jitter_y=f(p,"Jitter.Offset.Y");
    m.reset=ui(p,"Reset");m.resolution_percent=settings.fg.resolution_percent;m.peripheral_percent=settings.fg.peripheral_percent;
    m.center_method=settings.fg.center_method;
    m.optical=settings.fg.optical;
    m.peripheral_method=settings.fg.peripheral_method==0?2u:settings.fg.peripheral_method;
    m.focus_feather=settings.fg.feather;m.show_foveation_border=settings.fg.show_border;m.configuration=configuration(settings);
    source.queue_priority=settings.fg.queue_priority;source.release_point=settings.fg.release_point;
    observe(&source);
}catch(...){}
void fg_publish_ngx(ID3D12GraphicsCommandList* cmd,const NgxHandle* handle,
                    const NgxParameters* p,NgxResult result) noexcept try {
    if(!enabled()||!cmd||!p||!handle||!ngx_succeeded(result))return;
    // The layer's quad bridge owns all four guide identities. A game that
    // composites four views itself submits full-eye stereo color, so its two
    // peripheral views (full eye FOV, lower resolution) become the stereo pair.
    // TODO: composite the center views' guides into the inset (gaze + depth
    // registration) for full-resolution guides where the user looks.
    const auto quad=quad_view_snapshot();
    unsigned quad_eye=FgGuideIdentity::unknown;
    // Game-composited quad needs one peripheral guide per eye per frame;
    // report why a peripheral evaluation was not recorded.
    const auto quad_skip=[&](const char* reason,unsigned a=0,unsigned b=0,unsigned c=0){
        if(quad_eye>=2)return;static std::atomic<unsigned> skips{};const auto n=++skips;
        if(n<=16||n%600==0)trace_event("FG quad peripheral guide skipped eye=%u view=%llu reason=%s values=%u,%u,%u count=%u",
            quad_eye,static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(handle)),reason,a,b,c,n);};
    if(quad.active){
        if(!quad_is_app_snapshot(quad))return;
        const auto* role=quad_view_assignment(quad,reinterpret_cast<std::uintptr_t>(handle),
            stereo_view_generation(reinterpret_cast<std::uintptr_t>(handle)));
        if(!role||role->focus||role->eye_index>=2)return;
        quad_eye=role->eye_index;
        static std::atomic<unsigned> entered[2]{};const auto total=++entered[quad_eye]+entered[1-quad_eye];
        if(total<=4||total%600==0)trace_event("FG quad peripheral guides published left=%u right=%u (one per eye per frame expected)",
            entered[0].load(),entered[1].load());
    }
    const auto settings=current_settings();
    // There is no frame ticket. Only the ordered, immutable guide recordings
    // and the stereo pair completed at xrEndFrame associate a pair.
    std::uint32_t flags{};bool flags_known{};
    {std::lock_guard lock(mutex);if(auto entry=feature_flags.find(handle);entry!=feature_flags.end()){flags=entry->second;flags_known=true;}}
    // Late attachment can still recover an explicitly supplied declaration.
    // Never manufacture the depth convention when both sources are absent.
    if(!flags_known&&!try_get_ngx_integer_bits(p,"DLSS.Feature.Create.Flags",flags)){quad_skip("feature flags unknown");return;}
    auto* depth=read(p,"Depth");auto* motion=read(p,"MotionVectors");
    if(!depth||!motion){quad_skip("depth or motion missing");return;}
    const auto dd=depth->GetDesc(),md=motion->GetDesc();
    // MSFS 2024 passes a mipmapped depth array to one SR evaluation: two
    // slices in stereo, four for its first quad view (periphery left). DLSS
    // reads slice 0, and the FG guide preparation path samples mip 0 of slice
    // 0 explicitly; raw snapshots still reject arrays.
    if(dd.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||md.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
       !dd.DepthOrArraySize||md.DepthOrArraySize!=1||
       dd.SampleDesc.Count!=1||md.SampleDesc.Count!=1){
        quad_skip("unsupported depth/motion layout (depth slices, motion slices, depth dimension)",dd.DepthOrArraySize,md.DepthOrArraySize,unsigned(dd.Dimension));return;}
    const unsigned w=ui(p,"OutWidth"),h=ui(p,"OutHeight");
    if(!w||!h){quad_skip("output size missing");return;}
    CheekyFgEye m;
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
    if(quad_eye<2){
        // The peripheral output is upscaled by the game's own composition, so
        // the guides describe the submitted eye image, not the DLSS output.
        const auto frame=quad_app_frame();const auto& view=frame.views[quad_eye];
        if(frame.view_count<2||view.view_index!=quad_eye||!view.image_rect_width||!view.image_rect_height){
            quad_skip("submitted eye rect unknown (views, width, height)",frame.view_count,view.image_rect_width,view.image_rect_height);return;}
        m.width=view.image_rect_width;m.height=view.image_rect_height;
    }
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
    // Quad roles replace stereo calibration; a role change resets history.
    const FgGuideIdentity identity=quad_eye<2 ? FgGuideIdentity{quad_eye,(std::uint64_t(1)<<63)|quad.revision} :
        FgGuideIdentity{assignment.calibrated ? (assignment.shared_source?2u:assignment.eye_index) :
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
        const auto size=sr.valid ? std::pair{sr.region_width,sr.region_height} :
            fg_sr_fallback_size(settings.width,settings.height,m.depth_width,m.depth_height,w,h);
        m.focus_width=size.first;
        m.focus_height=size.second;
        m.focus_feather=settings.transition_width;
        // Matching overrides manual FG offsets but retains stereo alignment.
        m.focus_x=base_u-.5f;
        m.focus_y=base_v-.5f;
    }
    if(quad_eye<2&&settings.fg.foveated){
        // Cheeky does not crop game-composited quad views, so there is no SR
        // region to follow. Center the FG region in this eye's submitted image:
        // on OpenXR gaze when selected, otherwise on the stereo-forward point
        // (asymmetric eye FOVs put it off-center, mirrored between eyes).
        const auto frame=quad_app_frame();const auto& view=frame.views[quad_eye];
        const bool gaze=settings.center_mode==FoveationCenterMode::openxr_gaze&&(frame.status_flags&CHEEKY_GAZE_STATUS_GAZE_VALID)!=0;
        if(gaze||(view.flags&CHEEKY_GAZE_VIEW_FORWARD_VALID)){
            const float u=gaze?view.center_u:view.forward_u,v=gaze?view.center_v:view.forward_v;
            const float offset_x=settings.fg.match_sr_region?0.f:settings.fg.x,offset_y=settings.fg.match_sr_region?0.f:settings.fg.y;
            m.focus_x=std::clamp(u,0.f,1.f)-.5f+offset_x;m.focus_y=std::clamp(v,0.f,1.f)-.5f+offset_y;
        }
    }
    m.resolution_percent=settings.fg.resolution_percent;
    m.peripheral_percent=settings.fg.peripheral_percent;m.peripheral_method=settings.fg.peripheral_method;
    m.center_method=settings.fg.center_method;
    m.optical=settings.fg.optical;
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
    if(identity.eye<2){auto meta=m;meta.depth=meta.motion=meta.peripheral_depth=meta.peripheral_motion=nullptr;
        color_metadata[identity.eye]={meta,settings_configuration,GetTickCount64()};}
    if(cheeky_fg_image_only(settings.fg.center_method,settings.fg.peripheral_method,settings.fg.foveated))return;
    static std::uint64_t logged_mapping{};
    if(logged_mapping!=identity.mapping_revision){
        logged_mapping=identity.mapping_revision;
        trace_event("FG stereo identity revision=%llu calibrated=%u view=%llu physical_eye=%u (next complete pair resets history)",
            identity.mapping_revision,unsigned(assignment.calibrated),static_cast<unsigned long long>(view_id),identity.eye);
    }
    const bool recorded=snapshots().record(cmd,m,identity);
    if(!recorded)quad_skip("guide snapshot recording failed",dd.DepthOrArraySize,m.depth_width,m.depth_height);
    if(recorded){
        pending_submissions=true;++captured_guides;
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
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetCaptureHealth(CheekyFgCaptureHealth* out) noexcept {
    using namespace cheeky::foveated_dlss;
    if(!out||out->abi!=2||out->size!=sizeof(*out))return false;
    out->captured_guides=captured_guides;
    std::lock_guard captures(mutex);const auto& match=last_color_match;
    out->latest_evaluation=match.latest;out->consumed_evaluation=match.consumed;
    out->eligible=match.eligible;out->pending=match.pending;out->foreign=match.foreign;out->cancelled=match.cancelled;
    strcpy_s(out->match_reason,match.reason);return true;
}
// Each eye may release independently. Pin the submitted guide pair at color
// capture; the single xrEndFrame later validates and consumes this exact pair.
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetCaptureStamp(ID3D12Resource*,CheekyFgCaptureStamp* out) noexcept {
    using namespace cheeky::foveated_dlss;
    if(!out||out->abi!=1||out->size!=sizeof(*out)||!enabled())return false;
    std::lock_guard lock(mutex);
    out->token=snapshots().capture_token(out->token);out->render_frame=0;return true;
}
extern "C" __declspec(dllexport) std::uint64_t __cdecl CheekyFG_CompleteHooklessFrameV2(
    std::uint32_t use_for_fg,std::uint64_t color_token) noexcept {
    using namespace cheeky::foveated_dlss;
    if(!enabled())return 0;
    std::lock_guard lock(mutex);
    auto& guides=snapshots();
    const auto token=use_for_fg?guides.token(color_token):0;
    if(!token)guides.discard_interval(color_token);
    last_color_match=guides.last_match;
    if(!use_for_fg)last_color_match.reason="Native frame skipped at xrEndFrame";
    else if(token)last_color_match.reason="Matched at xrEndFrame";
    static ULONGLONG next_trace{};const auto now=GetTickCount64();
    if(now>=next_trace){next_trace=now+2000;const auto& m=last_color_match;
        trace_event("FG end-frame match token=%llu color_token=%llu eligible=%u pending=%u future=%u foreign=%u cancelled=%u latest_eval=%llu consumed=%llu failed=%llu reason=%s",
            token,color_token,m.eligible,m.pending,m.future,m.foreign,m.cancelled,m.latest,m.consumed,m.failed,m.reason);
    }
    return token;
}
extern "C" __declspec(dllexport) std::uint64_t __cdecl CheekyFG_CompleteHooklessFrameV1(
    std::uint32_t use_for_fg) noexcept {
    return CheekyFG_CompleteHooklessFrameV2(use_for_fg,0);
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
    if(!out||out->abi!=2||out->size!=sizeof(*out))return false;
    out->latency_diagnostics=cheeky::foveated_dlss::current_settings().fg.latency_diagnostics==1u?1u:0u;return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetRateOptionsV1(CheekyFgRateOptions* out) noexcept {
    if(!out||out->abi!=1||out->size<sizeof(*out))return false;
    out->multiplier=std::clamp(cheeky::foveated_dlss::current_settings().fg.multiplier,2u,4u);return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetRateOptionsV2(CheekyFgRateOptionsV2* out) noexcept {
    if(!out||out->abi!=2||out->size<sizeof(*out))return false;
    const auto& fg=cheeky::foveated_dlss::current_settings().fg;
    out->multiplier=std::clamp(fg.multiplier,2u,4u);out->automatic=fg.automatic_multiplier;return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetDlssgOptionsV1(CheekyFgDlssgOptionsV1* out) noexcept {
    if(!out||out->abi!=1||out->size<sizeof(*out))return false;
    out->preset=cheeky::foveated_dlss::current_settings().fg.dlssg_preset;return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetPacingOptionsV1(CheekyFgPacingOptions* out) noexcept {
    // Layers built before extra_frame_slots pass the shorter structure.
    if(!out||out->abi!=1||out->size<offsetof(CheekyFgPacingOptions,extra_frame_slots))return false;
    const auto& fg=cheeky::foveated_dlss::current_settings().fg;
    out->clock_mode=0; // Legacy follow/phase-lock settings no longer change the target clock.
    // Native D3D11 follows the shared release setting (default: after the
    // generated frame is submitted), like the D3D12 path.
    out->native11_release_point=fg.release_point<=8u?fg.release_point:3u;
    if(out->size>=offsetof(CheekyFgPacingOptions,dedicated_present_queue))out->extra_frame_slots=fg.extra_frame_slots?1u:0u;
    if(out->size>=offsetof(CheekyFgPacingOptions,gpu_waited_present))out->dedicated_present_queue=0;
    if(out->size>=offsetof(CheekyFgPacingOptions,limit_gpu_ahead))out->gpu_waited_present=0;
    if(out->size>=offsetof(CheekyFgPacingOptions,carry_missed_ticks))out->limit_gpu_ahead=fg.limit_gpu_ahead?1u:0u;
    if(out->size>=offsetof(CheekyFgPacingOptions,pacing_headroom))out->carry_missed_ticks=0;
    if(out->size>=offsetof(CheekyFgPacingOptions,target_refresh_hz))out->pacing_headroom=0;
    if(out->size>=sizeof(*out))out->target_refresh_hz=fg.target_refresh_hz;
    return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetNative11OptionsV1(CheekyFgNative11Options* out) noexcept {
    if(!out||out->abi!=1||out->size<sizeof(*out))return false;
    out->skip_identity_crop=cheeky::foveated_dlss::current_settings().fg.native11_skip_identity_crop?1u:0u;return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyQuad_GetComposeOptionsV1(CheekyQuadComposeOptions* out) noexcept {
    if(!out||out->abi!=1||out->size<sizeof(*out))return false;
    out->wait_gpu=cheeky::foveated_dlss::current_settings().d3d12_quad_compose_wait?1u:0u;return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetStorageOptionsV1(CheekyFgStorageOptions* out) noexcept {
    if(!out||out->abi!=1||out->size<sizeof(*out))return false;
    const auto settings=cheeky::foveated_dlss::current_settings();
    const auto& s=settings.fg;
    // Dynamic gaze uses existing full-eye native captures as previous/current
    // color history. No additional full-resolution history copy is allocated.
    out->compact=s.foveated&&!cheeky_fg_image_only(s.center_method,s.peripheral_method,s.foveated)&&
        settings.center_mode==cheeky::foveated_dlss::FoveationCenterMode::fixed &&
        !settings.eye_calibration_continuous;
    out->peripheral_percent=s.peripheral_percent;
    out->width=s.width;out->height=s.height;out->x=s.x;out->y=s.y;out->feather=s.feather;return true;
}
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_GetColorOptionsV1(CheekyFgColorOptionsV1* out) noexcept {
    using namespace cheeky::foveated_dlss;
    if(!out||out->abi!=2||out->size!=sizeof(*out))return false;
    const auto s=current_settings();const auto& f=s.fg;const auto key=configuration(s);
    out->enabled=enabled()&&cheeky_fg_image_only(f.center_method,f.peripheral_method,f.foveated);
    out->foveated=f.foveated;out->queue_priority=f.queue_priority;out->release_point=f.release_point;
    std::lock_guard lock(mutex);
    for(unsigned eye=0;eye<2;++eye){
        auto& g=out->eyes[eye];g={};
        const auto& cached=color_metadata[eye];
        if(cached.configuration==key&&GetTickCount64()-cached.time<500)g=cached.eye;
        else {
            g.focus_width=f.foveated?(f.match_sr_region?s.width:f.width):1.f;
            g.focus_height=f.foveated?(f.match_sr_region?s.height:f.height):1.f;
            g.focus_feather=f.match_sr_region?s.transition_width:f.feather;
            const float sx=(bool(eye==1)!=s.invert_stereo_x_offset)?-s.x_offset:s.x_offset;
            g.focus_x=fg_fallback_center(s.width,sx)-.5f+(f.match_sr_region?0.f:f.x);
            g.focus_y=fg_fallback_center(s.height,s.height_offset)-.5f+(f.match_sr_region?0.f:f.y);
        }
        g.center_method=f.center_method;g.peripheral_method=f.peripheral_method;g.optical=f.optical;
        g.resolution_percent=f.resolution_percent;g.peripheral_percent=f.peripheral_percent;
        g.show_foveation_border=f.foveated&&f.show_border;g.configuration=key;
    }
    return true;
}
// Read the unconsumed, actually submitted stereo guides for the current native
// interval. No "last good" crop is allowed here: a 2-pixel SR movement is
// enough to make compact color and NGX motion/depth spatially disagree.
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_PeekCaptureCropsV2(CheekyFgCaptureCropsV2* out) noexcept {
    using namespace cheeky::foveated_dlss;
    if(!out||out->abi!=2||out->size<sizeof(*out)||!enabled())return false;
    std::lock_guard lock(mutex);
    const auto settings=current_settings();
    if(settings.center_mode!=FoveationCenterMode::fixed||!settings.fg.enabled||!settings.fg.foveated)
        return false;
    CheekyFgCaptureCropsV2 sample;
    sample.token=out->token;
    // Reject any pending guide whose SR/FG settings revision is stale.
    const auto desired=fg_source_configuration(configuration(settings),game_sr_revision);
    if(!snapshots().peek_current_crops(sample,desired)||!sample.width||!sample.height)return false;
    *out=sample;
    return true;
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
extern "C" __declspec(dllexport) bool __cdecl CheekyFG_AcquirePair(CheekyFgPair* out) noexcept {
    using namespace cheeky::foveated_dlss;
    if(!out || out->abi!=CHEEKY_FG_PAIR_ABI || out->size!=sizeof(CheekyFgPair)||!enabled())return false;
    // Capture the persisted UEVR Lua setting alongside this guide pair.
    // Read settings before taking the snapshot mutex (consistent with enabled()).
    const auto settings=current_settings().fg;
    std::lock_guard lock(mutex);
    if(!snapshots().acquire(*out))return false;
    out->fg_queue_priority=settings.queue_priority <= 2u ? settings.queue_priority : 1u;
    out->release_point=settings.release_point<=8u?settings.release_point:0u;
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
    case CheekyFgState::waiting_for_history: return "Waiting for the previous real frame (interpolation needs a consecutive pair)";
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
        CheekyFgCaptureHealth health;
        if(CheekyFG_GetCaptureHealth(&health))strcpy_s(diagnostic.message,health.match_reason);
    }
    return diagnostic;
}
bool fg_performance(CheekyFgPerformance& out) noexcept {
    const auto layer=GetModuleHandleW(L"CheekyOpenXRLayer.dll");
    const auto query=layer?reinterpret_cast<CheekyFgPerformanceFn>(GetProcAddress(layer,"CheekyFG_GetPerformance")):nullptr;
    return query&&query(&out);
}
bool fg_batch_timings(CheekyFgBatchTimingsV2& out) noexcept {
    const auto layer=GetModuleHandleW(L"CheekyOpenXRLayer.dll");
    const auto query=layer?reinterpret_cast<CheekyFgBatchTimingsV2Fn>(GetProcAddress(layer,"CheekyFG_GetBatchTimingsV2")):nullptr;
    if(query)return query(&out);
    const auto legacy=layer?reinterpret_cast<CheekyFgBatchTimingsFn>(GetProcAddress(layer,"CheekyFG_GetBatchTimingsV1")):nullptr;
    CheekyFgBatchTimingsV1 prior;if(!legacy||!legacy(&prior))return false;
    static_cast<CheekyFgBatchTimingsV1&>(out)=prior;out.abi=2;out.size=sizeof(out);out.optical_separated=0;
    out.optical_ms=out.optical_last_ms=0;return true;
}
bool fg_cadence(CheekyFgCadence& out) noexcept {
    const auto layer=GetModuleHandleW(L"CheekyOpenXRLayer.dll");
    const auto query=layer?reinterpret_cast<CheekyFgCadenceFn>(GetProcAddress(layer,"CheekyFG_GetCadenceV1")):nullptr;
    return query&&query(&out);
}
bool fg_auto_status(CheekyFgAutoStatus& out) noexcept {
    const auto layer=GetModuleHandleW(L"CheekyOpenXRLayer.dll");
    const auto query=layer?reinterpret_cast<CheekyFgAutoStatusFn>(GetProcAddress(layer,"CheekyFG_GetAutoStatusV1")):nullptr;
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
    // Native/quad guide capture is measured by the layer, outside this DX12
    // stereo snapshot pool. Do not overwrite its samples with an empty pool.
    if(!out.guide_samples&&guide.samples){out.guide_samples=guide.samples;out.guide_ms=guide.mean;out.guide_last_ms=guide.last;}
    return supported;
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

}
