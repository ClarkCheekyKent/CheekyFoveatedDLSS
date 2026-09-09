#include "eye_calibration.hpp"
#include "openvr_gaze.hpp"
#include "openvr_gaze_math.hpp"
#include "openvr_vtable_hook.hpp"
#include "gaze_math.hpp"
#include "diagnostics.hpp"
#include "runtime.hpp"
#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <openvr.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace cheeky::foveated_dlss {
namespace {
using Microsoft::WRL::ComPtr;
using GetInterface = void* (__cdecl*)(const char*,vr::EVRInitError*);
using Shutdown = void (__cdecl*)();
using Wait = vr::EVRCompositorError (*)(void*,vr::TrackedDevicePose_t*,std::uint32_t,vr::TrackedDevicePose_t*,std::uint32_t);
using Submit = vr::EVRCompositorError (*)(void*,vr::EVREye,const vr::Texture_t*,const vr::VRTextureBounds_t*,vr::EVRSubmitFlags);
using SubmitArray = vr::EVRCompositorError (*)(void*,vr::EVREye,const vr::Texture_t*,std::uint32_t,const vr::VRTextureBounds_t*,vr::EVRSubmitFlags);
std::mutex state_mutex;
GetInterface get_interface{};
Shutdown original_shutdown{};
HMODULE api_module{};
std::vector<void*> hooks;
std::mutex hook_mutex;
std::vector<OpenVRVtableHook> vtable_hooks;
std::vector<HMODULE> retained_modules;
std::atomic<bool> stopping{};
bool runtime_stopping{}; // protected by state_mutex
CheekyGazeSnapshotV1 snapshot{};
std::uint64_t generation=0x8000000000000001ULL, frame{}, simulation_start{};
thread_local unsigned wait_depth{};
thread_local unsigned submit_depth{};
std::uint32_t pattern{};
bool simulate{};
struct Submitted {
    ComPtr<IUnknown> identity;
    CheekyGazeViewV1 view{};
    ULONGLONG observed{};
};
std::array<std::vector<Submitted>,2> submitted;
using Retired = decltype(submitted);
std::uint64_t qpc() { LARGE_INTEGER n{}; QueryPerformanceCounter(&n); return static_cast<std::uint64_t>(n.QuadPart); }
void clear_state(Retired& retired) {
    snapshot={}; retired.swap(submitted); ++generation; simulation_start=0;
}
void shutdown_hook() {
    // Exclude runtime queries without holding our mutex across runtime cleanup,
    // which can invoke ReShade resource destruction callbacks.
    {
        Retired retired;
        std::lock_guard lock(state_mutex);
        runtime_stopping=true;
        clear_state(retired);
    }
    original_shutdown();
    std::lock_guard lock(state_mutex);
    runtime_stopping=false;
}
void observe_frame(bool focused) {
    if (stopping.load()) return;
    Retired retired; // Release resources after state_mutex, outside coordinator lock order.
    std::lock_guard lock(state_mutex);
    if (runtime_stopping) return;
    vr::EVRInitError error{};
    auto* system=static_cast<vr::IVRSystem*>(get_interface(vr::IVRSystem_Version,&error));
    if (!system || error!=vr::VRInitError_None) { clear_state(retired); return; }
    const auto now=GetTickCount64();
    snapshot={};
    snapshot.abi_version=CHEEKY_GAZE_ABI_VERSION;
    snapshot.structure_size=sizeof(snapshot);
    snapshot.view_count=2;
    snapshot.sequence=++frame;
    // Opaque frame stamp for shared mapping/freshness policy; not an XrTime.
    snapshot.predicted_display_time=static_cast<std::int64_t>(frame);
    snapshot.sample_time=snapshot.predicted_display_time;
    snapshot.publication_qpc=qpc();
    snapshot.session_generation=generation;
    snapshot.swapchain_generation=generation;
    snapshot.status_flags=CHEEKY_GAZE_STATUS_LAYER_ACTIVE|CHEEKY_GAZE_STATUS_OPENVR;
    if (focused && system->IsInputAvailable()) snapshot.status_flags|=CHEEKY_GAZE_STATUS_SESSION_FOCUSED;
    const char* runtime=system->GetRuntimeVersion();
    sprintf_s(snapshot.runtime_name,"SteamVR / OpenVR %s",runtime ? runtime : "");
    vr::HmdVector2_t ndc[2]{};
    bool valid=!simulate && system->GetEyeTrackedFoveationCenter(&ndc[0],&ndc[1]);
    vr::ETrackedPropertyError property_error{};
    if (valid || system->GetBoolTrackedDeviceProperty(0,vr::Prop_SupportsXrEyeGazeInteraction_Bool,&property_error))
        snapshot.status_flags|=CHEEKY_GAZE_STATUS_SYSTEM_SUPPORTED;
    float ray[3]{0,0,-1}, next_ray[3]{};
    bool next_valid=false;
    if (simulate) {
        snapshot.status_flags|=CHEEKY_GAZE_STATUS_SIMULATED;
        if (!simulation_start) simulation_start=now;
        const double elapsed=(now-simulation_start)/1000.0;
        auto direction=[](const gaze_math::Pose& pose,float* result) {
            const auto& q=pose.orientation;
            result[0]=-2*(q.x*q.z+q.w*q.y);
            result[1]=-2*(q.y*q.z-q.w*q.x);
            result[2]=-(1-2*(q.x*q.x+q.y*q.y));
        };
        direction(gaze_math::simulated_gaze_pose({},elapsed,pattern),ray);
        next_valid=pattern==2 || pattern==3;
        if (next_valid) direction(gaze_math::simulated_gaze_pose({},gaze_math::next_simulated_jump_time(elapsed,pattern),pattern),next_ray);
        valid=gaze_math::simulated_gaze_valid(elapsed,pattern);
    }
    bool mapped=true;
    for (unsigned eye=0;eye<2;++eye) {
        auto& history=submitted[eye];
        for (auto& entry:history) if (now-entry.observed>500) retired[eye].push_back(std::move(entry));
        history.erase(std::remove_if(history.begin(),history.end(),[](const Submitted& s){return !s.identity;}),history.end());
        auto& view=snapshot.views[eye];
        if (!history.empty()) view=history.back().view;
        else mapped=false;
        view.structure_size=sizeof(view); view.view_index=eye;
        float left{},right{},top{},bottom{};
        const auto e=static_cast<vr::EVREye>(eye);
        system->GetProjectionRaw(e,&left,&right,&top,&bottom);
        const auto transform=system->GetEyeToHeadTransform(e);
        const float forward[3]{0,0,-1};
        if (openvr_project_direction(transform.m,left,right,top,bottom,forward,view.forward_u,view.forward_v)) {
            view.flags|=CHEEKY_GAZE_VIEW_FOV_VALID|CHEEKY_GAZE_VIEW_FORWARD_VALID;
            view.fov_left=std::atan(left); view.fov_right=std::atan(right);
            view.fov_up=-std::atan(top); view.fov_down=-std::atan(bottom);
        } else valid=false;
        const bool projected=simulate
            ? openvr_project_direction(transform.m,left,right,top,bottom,ray,view.center_u,view.center_v)
            : openvr_ndc_center(ndc[eye].v[0],ndc[eye].v[1],view.center_u,view.center_v);
        valid=valid && projected;
        if (simulate && next_valid && openvr_project_direction(transform.m,left,right,top,bottom,next_ray,view.next_jump_u,view.next_jump_v))
            view.flags|=CHEEKY_GAZE_VIEW_NEXT_JUMP_VALID;
    }
    if (mapped) snapshot.status_flags|=CHEEKY_GAZE_STATUS_MAPPING_READY;
    if (valid && focused) {
        snapshot.status_flags|=CHEEKY_GAZE_STATUS_GAZE_VALID|CHEEKY_GAZE_STATUS_ACTION_ACTIVE;
        for (auto& view:snapshot.views) view.flags|=CHEEKY_GAZE_VIEW_ORIENTATION_VALID;
    }
}
void observe_submit(vr::EVREye eye,const vr::Texture_t* texture,const vr::VRTextureBounds_t* bounds,
    std::uint32_t slice,vr::EVRSubmitFlags flags,vr::EVRCompositorError result) {
    if (stopping.load() || eye<0 || eye>1) return;
    Submitted entry;
    std::vector<Submitted> retired;
    bool valid=result==vr::VRCompositorError_None && texture && texture->handle && slice==0;
    std::uint32_t width{},height{};
    // Reject nonstandard layouts rather than interpreting their payload as a 2D image.
    valid=valid && (static_cast<unsigned>(flags)&~static_cast<unsigned>(vr::Submit_TextureWithPose|vr::Submit_TextureWithDepth|vr::Submit_FrameDiscontinuity))==0;
    if (valid && texture->eType==vr::TextureType_DirectX) {
        ComPtr<ID3D11Texture2D> resource;
        valid=SUCCEEDED(static_cast<IUnknown*>(texture->handle)->QueryInterface(IID_PPV_ARGS(&resource)));
        if (valid) {
            D3D11_TEXTURE2D_DESC desc{}; resource->GetDesc(&desc);
            valid=desc.ArraySize==1 && desc.SampleDesc.Count==1;
            width=desc.Width; height=desc.Height;
            resource.As(&entry.identity);
        }
    } else if (valid && texture->eType==vr::TextureType_DirectX12) {
        const auto* data=static_cast<const vr::D3D12TextureData_t*>(texture->handle);
        valid=data->m_pResource!=nullptr;
        if (valid) {
            const auto desc=data->m_pResource->GetDesc();
            valid=desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize==1 &&
                desc.SampleDesc.Count==1 && desc.Width<=UINT32_MAX;
            width=static_cast<std::uint32_t>(desc.Width); height=desc.Height;
            data->m_pResource->QueryInterface(IID_PPV_ARGS(&entry.identity));
        }
    } else valid=false;
    const vr::VRTextureBounds_t full{0,0,1,1};
    const auto& b=bounds ? *bounds : full;
    auto& view=entry.view;
    valid=valid && entry.identity && openvr_bounds(b.uMin,b.uMax,width,view.image_rect_x,view.image_rect_width) &&
        openvr_bounds(b.vMin,b.vMax,height,view.image_rect_y,view.image_rect_height);
    std::lock_guard lock(state_mutex);
    if (runtime_stopping) return;
    auto& history=submitted[eye];
    if (!valid) { if (!history.empty()) { retired.swap(history); ++generation; } return; }
    view.structure_size=sizeof(view); view.view_index=eye;
    view.resource_identity=reinterpret_cast<std::uint64_t>(entry.identity.Get());
    view.swapchain_identity=view.resource_identity;
    view.flags=CHEEKY_GAZE_VIEW_RESOURCE_VALID;
    entry.observed=GetTickCount64();
    if (!history.empty()) {
        const auto& previous=history.back().view;
        if (previous.image_rect_x!=view.image_rect_x || previous.image_rect_y!=view.image_rect_y ||
            previous.image_rect_width!=view.image_rect_width || previous.image_rect_height!=view.image_rect_height) {
            retired.swap(history); ++generation;
        }
    }
    const bool first=history.empty();
    history.erase(std::remove_if(history.begin(),history.end(),[&](const Submitted& s){return s.view.resource_identity==view.resource_identity;}),history.end());
    if (history.size()>=4) { retired.push_back(std::move(history.front())); history.erase(history.begin()); }
    history.push_back(std::move(entry));
    if (first) trace_event("OpenVR submit eye=%u resource=0x%llx rect=%d,%d %ux%u",static_cast<unsigned>(eye),
        static_cast<unsigned long long>(view.resource_identity),view.image_rect_x,view.image_rect_y,view.image_rect_width,view.image_rect_height);
}
// Separate original slot values for each ABI version. Do not deduplicate by
// implementation address: unrelated methods can share the same forwarding thunk.
template<unsigned N> struct CompositorHooks {
    static inline Wait wait{};
    static inline Submit submit{};
    static inline SubmitArray array{};
    static vr::EVRCompositorError wait_hook(void* self,vr::TrackedDevicePose_t* r,std::uint32_t nr,vr::TrackedDevicePose_t* g,std::uint32_t ng) {
        ++wait_depth;
        const auto result=wait(self,r,nr,g,ng);
        if (--wait_depth==0) {
            observe_frame(result==vr::VRCompositorError_None && (!r || nr==0 || r[0].bPoseIsValid));
            if (result == vr::VRCompositorError_None) eye_calibration_frame();
            eye_calibration_tick();
        }
        return result;
    }
    static vr::EVRCompositorError submit_hook(void* self,vr::EVREye eye,const vr::Texture_t* texture,const vr::VRTextureBounds_t* bounds,vr::EVRSubmitFlags flags) {
        ++submit_depth;
        std::uint64_t calibration_ticket{};
        if (submit_depth == 1 && eye_calibration_enabled()) {
            const auto supported_flags = vr::Submit_TextureWithPose | vr::Submit_TextureWithDepth | vr::Submit_FrameDiscontinuity;
            if (texture && texture->handle && texture->eType == vr::TextureType_DirectX &&
                (static_cast<unsigned>(flags) & ~static_cast<unsigned>(supported_flags)) == 0) {
                ComPtr<ID3D11Texture2D> image;
                if (SUCCEEDED(static_cast<IUnknown*>(texture->handle)->QueryInterface(IID_PPV_ARGS(&image)))) {
                    const vr::VRTextureBounds_t full{0, 0, 1, 1};
                    const auto& b = bounds ? *bounds : full;
                    // Enqueue the readback before forwarding Submit. The runtime
                    // may consume/reuse the image after the original call.
                    calibration_ticket = eye_calibration_submit(image.Get(), unsigned(eye), b.uMin, b.vMin, b.uMax, b.vMax);
                } else eye_calibration_unsupported_submit();
            } else eye_calibration_unsupported_submit();
        }
        const auto result=submit(self,eye,texture,bounds,flags);
        eye_calibration_result(calibration_ticket, int(result));
        if (--submit_depth==0) observe_submit(eye,texture,bounds,0,flags,result); return result;
    }
    static vr::EVRCompositorError array_hook(void* self,vr::EVREye eye,const vr::Texture_t* texture,std::uint32_t slice,const vr::VRTextureBounds_t* bounds,vr::EVRSubmitFlags flags) {
        eye_calibration_unsupported_submit();
        ++submit_depth;
        const auto result=array(self,eye,texture,slice,bounds,flags);
        if (--submit_depth==0) observe_submit(eye,texture,bounds,slice,flags,result); return result;
    }
};
bool install(void* target,void* detour,void** original) {
    if (std::find(hooks.begin(),hooks.end(),target)!=hooks.end()) return true;
    if (MH_CreateHook(target,detour,original)!=MH_OK) return false;
    // Keep implementation code mapped until hooks have been removed, even after VR shutdown.
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(target),&module)) {
        MH_RemoveHook(target); return false;
    }
    if (MH_EnableHook(target)!=MH_OK) { MH_RemoveHook(target); FreeLibrary(module); return false; }
    retained_modules.push_back(module); hooks.push_back(target); return true;
}
bool install_slot(void** slot,void* detour,void** original) {
    for (const auto& hook:vtable_hooks) if (hook.slot==slot) return true;
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(slot),&module)) return false;
    OpenVRVtableHook hook;
    if (!hook.install(slot,detour,original)) { FreeLibrary(module); return false; }
    retained_modules.push_back(module); vtable_hooks.push_back(hook); return true;
}
template<unsigned N> void arm(const char* version,void* object) {
    auto** vtable=*static_cast<void***>(object);
    const auto slot=openvr_submit_slot(version);
    const auto before=vtable_hooks.size();
    using H=CompositorHooks<N>;
    install_slot(&vtable[2],reinterpret_cast<void*>(&H::wait_hook),reinterpret_cast<void**>(&H::wait));
    install_slot(&vtable[slot],reinterpret_cast<void*>(&H::submit_hook),reinterpret_cast<void**>(&H::submit));
    if constexpr (N<2) install_slot(&vtable[slot+1],reinterpret_cast<void*>(&H::array_hook),reinterpret_cast<void**>(&H::array));
    if (vtable_hooks.size()!=before) trace_event("OpenVR compositor table hooks armed interface=%s",version);
}
void* interface_hook(const char* version,vr::EVRInitError* error) {
    auto* object=get_interface(version,error);
    if (!object || stopping.load() || !openvr_submit_slot(version)) return object;
    std::lock_guard lock(hook_mutex);
    if (stopping.load()) return object;
    if (!std::strcmp(version,"IVRCompositor_029")) arm<0>(version,object);
    else if (!std::strcmp(version,"IVRCompositor_028")) arm<1>(version,object);
    else if (!std::strcmp(version,"IVRCompositor_027")) arm<2>(version,object);
    else if (!std::strcmp(version,"IVRCompositor_022")) arm<3>(version,object);
    return object;
}
}
void poll_openvr_hooks() noexcept {
    if (stopping.load()) return;
    if (!api_module) {
        if (!GetModuleHandleExW(0,L"openvr_api.dll",&api_module)) return;
    }
    std::lock_guard lock(hook_mutex);
    if (!original_shutdown) {
        auto* target=reinterpret_cast<void*>(GetProcAddress(api_module,"VR_ShutdownInternal"));
        if (!target || !install(target,reinterpret_cast<void*>(&shutdown_hook),reinterpret_cast<void**>(&original_shutdown))) return;
    }
    if (!get_interface) {
        auto* target=reinterpret_cast<void*>(GetProcAddress(api_module,"VR_GetGenericInterface"));
        if (target) install(target,reinterpret_cast<void*>(&interface_hook),reinterpret_cast<void**>(&get_interface));
    }
    // Observe the interfaces requested by the application. Proactively querying
    // every version can also force ReShade to select the wrong first interface.
}
void stop_openvr_hooks() noexcept {
    stopping.store(true);
    eye_calibration_stop();
    std::lock_guard hooks_lock(hook_mutex);
    for (auto it=vtable_hooks.rbegin();it!=vtable_hooks.rend();++it) it->restore();
    for (auto* target:hooks) MH_DisableHook(target);
    Retired retired;
    std::lock_guard lock(state_mutex);
    clear_state(retired);
    // Trampolines are removed by the shared MinHook owner immediately after this.
    // Retain target modules through that teardown; process exit releases them.
}
bool read_openvr_gaze(const Settings& settings,IUnknown* resource,CheekyGazeSnapshotV1& output) noexcept {
    std::lock_guard lock(state_mutex);
    const bool next_simulate=settings.center_mode==FoveationCenterMode::simulated_gaze;
    if (next_simulate!=simulate || pattern!=settings.simulation_pattern) simulation_start=0;
    simulate=next_simulate; pattern=settings.simulation_pattern;
    if (snapshot.abi_version!=CHEEKY_GAZE_ABI_VERSION) return false;
    output=snapshot;
    if (snapshot.swapchain_generation!=generation)
        output.status_flags&=~CHEEKY_GAZE_STATUS_MAPPING_READY;
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    const auto current_qpc=qpc();
    if (current_qpc<output.publication_qpc ||
        static_cast<double>(current_qpc-output.publication_qpc)/frequency.QuadPart>0.050)
        output.status_flags&=~(CHEEKY_GAZE_STATUS_GAZE_VALID|CHEEKY_GAZE_STATUS_SESSION_FOCUSED);
    ComPtr<IUnknown> identity;
    if (resource) resource->QueryInterface(IID_PPV_ARGS(&identity));
    const auto id=reinterpret_cast<std::uint64_t>(identity.Get());
    const auto now=GetTickCount64();
    // Select an observed member of the game's rotating render-target set for
    // exact matching. Other-eye metadata remains available to existing routes.
    for (unsigned eye=0;eye<2;++eye) {
        for (const auto& entry:submitted[eye]) {
            if (id && entry.view.resource_identity==id && now-entry.observed<=500) {
                auto& v=output.views[eye];
                v.resource_identity=entry.view.resource_identity;
                v.swapchain_identity=entry.view.swapchain_identity;
                v.image_rect_x=entry.view.image_rect_x; v.image_rect_y=entry.view.image_rect_y;
                v.image_rect_width=entry.view.image_rect_width; v.image_rect_height=entry.view.image_rect_height;
                v.flags|=CHEEKY_GAZE_VIEW_RESOURCE_VALID;
            }
        }
    }
    return true;
}
}
