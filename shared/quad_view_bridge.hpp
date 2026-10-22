#pragma once
#include "cheeky_quad_views.h"
#include "cheeky_quad_fg.hpp"
#include "cheeky_gaze_abi.h"
#include "app_quad_views.hpp"
#include <Windows.h>
#include <atomic>
#include <sstream>
namespace cheeky::foveated_dlss {
inline HMODULE quad_view_module() noexcept {
    // Pin a successfully discovered layer; retry discovery after late injection.
    static std::atomic<HMODULE> retained{};
    auto module=retained.load();
    if(!module){HMODULE found{};if(GetModuleHandleExW(0,L"CheekyOpenXRLayer.dll",&found)){
        if(!retained.compare_exchange_strong(module,found))FreeLibrary(found);else module=found;
    }}
    return module;
}
// NGX can submit native driver work in addition to ordinary D3D11 API calls.
// Share one CPU dispatch gate with the quad presenter's runtime submissions.
struct QuadD3D11DispatchScope {
    void* ticket{};CheekyQuadReleaseD3D11V1Fn release{};
    QuadD3D11DispatchScope(void* context,bool enabled) noexcept {
        if(!enabled||!context)return;const auto module=quad_view_module();if(!module)return;
        const auto acquire=reinterpret_cast<CheekyQuadAcquireD3D11V1Fn>(GetProcAddress(module,"CheekyQuad_AcquireD3D11V1"));
        release=reinterpret_cast<CheekyQuadReleaseD3D11V1Fn>(GetProcAddress(module,"CheekyQuad_ReleaseD3D11V1"));
        if(acquire&&release)ticket=acquire(context);
    }
    ~QuadD3D11DispatchScope(){if(ticket)release(ticket);}
    QuadD3D11DispatchScope(const QuadD3D11DispatchScope&)=delete;
    QuadD3D11DispatchScope& operator=(const QuadD3D11DispatchScope&)=delete;
};
inline CheekyQuadViewsV1 quad_layer_view_snapshot() noexcept {
    CheekyQuadViewsV1 value{};value.version=CHEEKY_QUAD_VIEWS_VERSION;value.size=sizeof(value);
    const auto module=quad_view_module();
    const auto get=module?reinterpret_cast<CheekyQuadGetViewsV1Fn>(GetProcAddress(module,"CheekyQuad_GetViewsV1")):nullptr;
    if(!get||!get(&value))value.active=0;
    return value;
}
inline CheekyGazeSnapshotV1 quad_app_frame() noexcept {
    CheekyGazeSnapshotV1 frame{};const auto module=quad_view_module();
    const auto get=module?reinterpret_cast<CheekyOpenXRGetGazeSnapshotFn>(GetProcAddress(module,"CheekyOpenXR_GetGazeSnapshot")):nullptr;
    if(get)get(CHEEKY_GAZE_ABI_VERSION,&frame,sizeof(frame));return frame;
}
inline bool quad_is_app_snapshot(const CheekyQuadViewsV1& value) noexcept {
    return (value.session_generation&(uint64_t(1)<<63))!=0;
}
inline CheekyQuadViewsV1 quad_view_snapshot() noexcept {
    auto layer=quad_layer_view_snapshot();if(layer.active){app_quad_views().snapshot(0,0,GetTickCount64());return layer;}
    const auto frame=quad_app_frame();
    auto app=app_quad_views().snapshot(frame.session_generation,uint64_t(frame.predicted_display_time),GetTickCount64());
    return app.active?app:layer;
}
inline void quad_view_observe12(uint64_t id,uint64_t generation,unsigned width,unsigned height) noexcept {
    if(quad_layer_view_snapshot().active){app_quad_views().snapshot(0,0,GetTickCount64());return;}
    const auto frame=quad_app_frame();
    app_quad_views().observe(frame.session_generation,uint64_t(frame.predicted_display_time),GetTickCount64(),id,generation,width,height);
}
inline bool quad_view_blocks_fg() noexcept {
    // Also called by the layer while querying FG controls. Do not call back
    // into layer exports under its session/presenter locks.
    return app_quad_views().detected();
}
inline bool quad_view_observe11(const CheekyQuadSourceD3D11V1& source) noexcept {
    const auto module=quad_view_module();
    const auto observe=module?reinterpret_cast<CheekyQuadObserveD3D11V1Fn>(GetProcAddress(module,"CheekyQuad_ObserveD3D11V1")):nullptr;
    return observe&&observe(&source);
}
inline void quad_view_forget(uint64_t id,uint64_t generation) noexcept {
    app_quad_views().forget(id,generation);
    const auto module=quad_view_module();
    const auto forget=module?reinterpret_cast<CheekyQuadForgetSourceV1Fn>(GetProcAddress(module,"CheekyQuad_ForgetSourceV1")):nullptr;
    if(forget)forget(id,generation);
}
inline void quad_view_recalibrate() noexcept {
    app_quad_views().reset();
    const auto module=quad_view_module();
    const auto reset=module?reinterpret_cast<CheekyQuadRecalibrateV1Fn>(GetProcAddress(module,"CheekyQuad_RecalibrateV1")):nullptr;
    if(reset)reset();
}
inline void quad_view_configure(bool manual,bool show_numbers,const unsigned* roles=nullptr,unsigned source_count=0) noexcept {
    app_quad_views().configure(manual,show_numbers,roles,source_count);
    const auto module=quad_view_module();
    const auto configure=module?reinterpret_cast<CheekyQuadConfigureV1Fn>(GetProcAddress(module,"CheekyQuad_ConfigureV1")):nullptr;
    if(configure)configure(manual,show_numbers);
    if(roles){const auto save=module?reinterpret_cast<CheekyQuadSavedLayoutV1Fn>(GetProcAddress(module,"CheekyQuad_SetSavedLayoutV1")):nullptr;
        if(save){CheekyQuadSavedLayoutV1 layout{};layout.version=1;layout.size=sizeof(layout);layout.source_count=source_count;
            for(unsigned i=0;i<8;++i)layout.roles[i]=roles[i];save(&layout);}}
}
inline bool quad_view_assign(uint64_t session,unsigned number,unsigned role,uint64_t id,uint64_t generation) noexcept {
    if(session&(uint64_t(1)<<63))return app_quad_views().assign(session,number,role,id,generation);
    const auto module=quad_view_module();
    const auto assign=module?reinterpret_cast<CheekyQuadAssignV1Fn>(GetProcAddress(module,"CheekyQuad_AssignV1")):nullptr;
    return assign&&assign(session,number,role,id,generation);
}
inline const CheekyQuadViewV1* quad_view_assignment(const CheekyQuadViewsV1& snapshot,uint64_t id,uint64_t generation) noexcept {
    if(snapshot.active&&snapshot.status==2)for(const auto& view:snapshot.views)
        if((view.verified||snapshot.manual_mode||quad_is_app_snapshot(snapshot))&&view.dlss_view==id&&view.registration_generation==generation)return &view;
    return nullptr;
}
inline const char* quad_role_name(unsigned role) noexcept {
    static constexpr const char* names[]{"Unassigned","Periphery Left","Periphery Right","Center Left","Center Right"};
    return role<5?names[role]:names[0];
}
inline bool quad_fg_stats(CheekyQuadFgStatsV1& stats) noexcept {
    const auto module=quad_view_module();const auto get=module?reinterpret_cast<CheekyQuadFgStatsFn>(GetProcAddress(module,"CheekyQuadFG_GetStatsV1")):nullptr;
    return get&&get(&stats);
}
inline std::string quad_view_diagnostics_json() {
    const auto quad=quad_view_snapshot();std::ostringstream out;out<<std::boolalpha;
    const bool app=quad.active&&quad_is_app_snapshot(quad);
    out<<"{\"active\":"<<bool(quad.active)<<",\"app_composited\":"<<app<<",\"openxr_quad\":"<<bool(quad.active&&!app)<<",\"fg_blocked\":"<<app
        <<",\"manual\":"<<bool(quad.manual_mode)<<",\"show_numbers\":"<<bool(quad.show_numbers)
        <<",\"session\":\""<<quad.session_generation<<"\",\"revision\":"<<quad.revision
        <<",\"status\":\""<<(quad.active?(quad.manual_mode?(quad.status==2?"Manual assigned":"Manual incomplete"):
            quad.status==2?(app?"Default order":"Verified"):quad.status==3?"Unresolved":"Acquiring"):"Inactive")
        <<"\",\"confirmations\":"<<quad.confirmations<<",\"captures\":"<<quad.captures<<",\"rejected\":"<<quad.rejected<<",\"sources\":[";
    bool comma{};
    for(const auto& source:quad.sources)if(source.dlss_view){
        if(comma)out<<',';comma=true;unsigned assigned{};
        if(quad.status==2)for(const auto& view:quad.views)if(view.dlss_view==source.dlss_view&&view.registration_generation==source.registration_generation)assigned=view.view_index+1;
        out<<"{\"number\":"<<source.number<<",\"id\":\""<<source.dlss_view<<"\",\"generation\":\""<<source.registration_generation
            <<"\",\"manual_role\":"<<source.manual_role<<",\"role\":\""<<quad_role_name(assigned)<<"\",\"width\":"<<source.width<<",\"height\":"<<source.height<<'}';
    }
    out<<"],\"views\":[";
    for(unsigned i=0;i<4;++i){if(i)out<<',';const auto& view=quad.views[i];
        out<<"{\"index\":"<<i<<",\"role\":\""<<quad_role_name(i+1)<<"\",\"id\":\""<<view.dlss_view<<"\",\"generation\":\""<<view.registration_generation
            <<"\",\"verified\":"<<bool(view.verified)<<",\"resource\":\""<<view.submitted_resource<<"\",\"slice\":"<<view.array_slice<<'}';}
    out<<"],\"fg\":{";
    CheekyQuadFgStatsV1 stats;const bool supported=!app&&quad_fg_stats(stats);
    out<<"\"supported\":"<<supported<<",\"enabled\":"<<bool(stats.enabled)<<",\"observations\":"<<stats.observations<<",\"snapshots\":"<<stats.snapshots
        <<",\"frames\":"<<stats.frames<<",\"guide_pairs\":"<<stats.guide_pairs<<",\"capture_stage\":"<<stats.capture_stage<<",\"assembly_stage\":"<<stats.assembly_stage;
    out<<"}}";return out.str();
}
}
