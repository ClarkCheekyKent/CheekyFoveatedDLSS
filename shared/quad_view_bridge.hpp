#pragma once
#include "cheeky_quad_views.h"
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
inline CheekyQuadViewsV1 quad_view_snapshot() noexcept {
    CheekyQuadViewsV1 value{};value.version=CHEEKY_QUAD_VIEWS_VERSION;value.size=sizeof(value);
    const auto module=quad_view_module();
    const auto get=module?reinterpret_cast<CheekyQuadGetViewsV1Fn>(GetProcAddress(module,"CheekyQuad_GetViewsV1")):nullptr;
    if(!get||!get(&value))value.active=0;
    return value;
}
inline bool quad_view_observe11(const CheekyQuadSourceD3D11V1& source) noexcept {
    const auto module=quad_view_module();
    const auto observe=module?reinterpret_cast<CheekyQuadObserveD3D11V1Fn>(GetProcAddress(module,"CheekyQuad_ObserveD3D11V1")):nullptr;
    return observe&&observe(&source);
}
inline void quad_view_forget(uint64_t id,uint64_t generation) noexcept {
    const auto module=quad_view_module();
    const auto forget=module?reinterpret_cast<CheekyQuadForgetSourceV1Fn>(GetProcAddress(module,"CheekyQuad_ForgetSourceV1")):nullptr;
    if(forget)forget(id,generation);
}
inline void quad_view_recalibrate() noexcept {
    const auto module=quad_view_module();
    const auto reset=module?reinterpret_cast<CheekyQuadRecalibrateV1Fn>(GetProcAddress(module,"CheekyQuad_RecalibrateV1")):nullptr;
    if(reset)reset();
}
inline void quad_view_configure(bool manual,bool show_numbers) noexcept {
    const auto module=quad_view_module();
    const auto configure=module?reinterpret_cast<CheekyQuadConfigureV1Fn>(GetProcAddress(module,"CheekyQuad_ConfigureV1")):nullptr;
    if(configure)configure(manual,show_numbers);
}
inline bool quad_view_assign(uint64_t session,unsigned number,unsigned role,uint64_t id,uint64_t generation) noexcept {
    const auto module=quad_view_module();
    const auto assign=module?reinterpret_cast<CheekyQuadAssignV1Fn>(GetProcAddress(module,"CheekyQuad_AssignV1")):nullptr;
    return assign&&assign(session,number,role,id,generation);
}
inline const CheekyQuadViewV1* quad_view_assignment(const CheekyQuadViewsV1& snapshot,uint64_t id,uint64_t generation) noexcept {
    if(snapshot.active&&snapshot.status==2)for(const auto& view:snapshot.views)
        if((view.verified||snapshot.manual_mode)&&view.dlss_view==id&&view.registration_generation==generation)return &view;
    return nullptr;
}
inline const char* quad_role_name(unsigned role) noexcept {
    static constexpr const char* names[]{"Unassigned","Periphery Left","Periphery Right","Center Left","Center Right"};
    return role<5?names[role]:names[0];
}
inline std::string quad_view_diagnostics_json() {
    const auto quad=quad_view_snapshot();std::ostringstream out;out<<std::boolalpha;
    out<<"{\"active\":"<<bool(quad.active)<<",\"manual\":"<<bool(quad.manual_mode)<<",\"show_numbers\":"<<bool(quad.show_numbers)
        <<",\"session\":\""<<quad.session_generation<<"\",\"revision\":"<<quad.revision
        <<",\"status\":\""<<(quad.active?(quad.manual_mode?(quad.status==2?"Manual assigned":"Manual incomplete"):
            quad.status==2?"Verified":quad.status==3?"Unresolved":"Acquiring"):"Inactive")
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
    out<<"]}";return out.str();
}
}
