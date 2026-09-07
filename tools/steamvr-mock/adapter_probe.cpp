#include <Windows.h>
#include <openvr.h>
#include <MinHook.h>
#include "openvr_gaze.hpp"
#include <cstdarg>
#include <cstdio>
namespace cheeky::foveated_dlss {
void trace_event(const char* format,...) noexcept {
    va_list args; va_start(args,format); std::vprintf(format,args); va_end(args); std::puts("");
}
}
int main() {
    using namespace cheeky::foveated_dlss;
    vr::EVRInitError error{};
    if (!vr::VR_Init(&error,vr::VRApplication_Background)) {
        std::printf("Runtime init failed: %s\n",vr::VR_GetVRInitErrorAsEnglishDescription(error)); return 1;
    }
    if (MH_Initialize()!=MH_OK) return 2;
    poll_openvr_hooks();
    auto* legacy=vr::VR_GetGenericInterface("IVRCompositor_022",&error);
    if (!legacy) return 3;
    // Exercise the other compositor wrappers as well, then call unrelated
    // IVRSystem methods that may share their forwarding implementations.
    for (const auto* version:{"IVRCompositor_029","IVRCompositor_028","IVRCompositor_027"})
        vr::VR_GetGenericInterface(version,&error);
    for (const auto* version:{"IVRSystem_019","IVRSystem_026"}) {
        auto* system=vr::VR_GetGenericInterface(version,&error);
        if (!system) return 5;
        using Projection=void (*)(void*,vr::EVREye,float*,float*,float*,float*);
        const auto projection=reinterpret_cast<Projection>((*static_cast<void***>(system))[2]);
        float left{},right{},top{},bottom{};
        projection(system,vr::Eye_Left,&left,&right,&top,&bottom);
        std::printf("Unrelated %s projection after compositor hooks: %f %f %f %f\n",version,left,right,top,bottom);
        if (!(left<right && top<bottom)) return 6;
    }
    using Wait=vr::EVRCompositorError (*)(void*,vr::TrackedDevicePose_t*,unsigned,vr::TrackedDevicePose_t*,unsigned);
    auto wait=reinterpret_cast<Wait>((*static_cast<void***>(legacy))[2]);
    vr::TrackedDevicePose_t pose{};
    const auto result=wait(legacy,&pose,1,nullptr,0);
    CheekyGazeSnapshotV1 snapshot{};
    const bool captured=read_openvr_gaze(Settings{},nullptr,snapshot);
    std::printf("Legacy WaitGetPoses result=%d (background app may lack scene focus), captured=%d backend=0x%x frame=%lld runtime=%s\n",
        result,captured,snapshot.status_flags,static_cast<long long>(snapshot.predicted_display_time),snapshot.runtime_name);
    // Exercise the hooked runtime shutdown before removing detours.
    vr::VR_Shutdown();
    CheekyGazeSnapshotV1 after{};
    const bool cleared=!read_openvr_gaze(Settings{},nullptr,after);
    stop_openvr_hooks(); MH_Uninitialize();
    if (!captured || !(snapshot.status_flags&CHEEKY_GAZE_STATUS_OPENVR) || !cleared) return 4;
    std::puts("PASS: actual SteamVR legacy hook and shutdown invalidation. This does not validate scene texture mapping.");
}
