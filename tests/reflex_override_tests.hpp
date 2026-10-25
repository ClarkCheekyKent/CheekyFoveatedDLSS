#pragma once
#include "../src/reflex_override.hpp"
#include "settings_io.hpp"
#include <stdexcept>

namespace cheeky::tests {
inline LONG reflex_sleeps{},reflex_modes{};
inline int reflex_mode_result{};
inline foveated_dlss::reflex_override::SleepMode driver_mode{};
__declspec(noinline) inline int __cdecl fake_reflex_sleep(IUnknown*) {
    InterlockedIncrement(&reflex_sleeps);return 17;
}
__declspec(noinline) inline int __cdecl fake_reflex_mode(IUnknown*,void* params) {
    InterlockedIncrement(&reflex_modes);
    if(!params)return -5;
    if(reflex_mode_result)return reflex_mode_result;
    std::memcpy(&driver_mode,params,sizeof(driver_mode));return 0;
}
inline int __cdecl fake_reflex_status(IUnknown*,foveated_dlss::reflex_override::SleepStatus* status) {
    status->low_latency=driver_mode.low_latency;return 0;
}
inline void reflex_override_tests() {
    using namespace foveated_dlss;
    namespace r=reflex_override;
    auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    Settings settings;
    require(!settings.fg.force_reflex_off,"Reflex override defaults off");
    require(set_named_setting(settings,"FgForceReflexOff","true")&&settings.fg.force_reflex_off&&
        !set_named_setting(settings,"FgForceReflexOff","invalid")&&
        serialize_settings(settings).find("FgForceReflexOff=1")!=std::string::npos,"Reflex setting persistence and validation");
    // Cache the driver's entry points before installing the actual inline hooks.
    r::SleepFn volatile cached_sleep=&fake_reflex_sleep;
    r::SetModeFn volatile cached_mode=&fake_reflex_mode;
    require(MH_Initialize()==MH_OK,"Initialize Reflex hook fixture");
    require(r::install_at(reinterpret_cast<void*>(&fake_reflex_sleep),reinterpret_cast<void*>(&fake_reflex_mode),&fake_reflex_status),"Install Reflex hooks");
    auto* device=reinterpret_cast<IUnknown*>(std::uintptr_t{0x1234});
    r::SleepMode requested{};requested.low_latency=requested.boost=requested.optimize_markers=requested.minimum_queue=1;requested.interval_us=12345;
    r::running=true;r::processing=true;r::requested=false;
    require(cached_mode(device,&requested)==0&&driver_mode.low_latency==1&&cached_sleep(device)==17,"Unchecked cached calls pass through");
    r::requested=true;
    require(cached_sleep(device)==0&&reflex_sleeps==1&&driver_mode.low_latency==0&&driver_mode.boost==0&&driver_mode.interval_us==0,"Live enable disables driver pacing and skips cached Sleep");
    requested.interval_us=19000;
    require(cached_mode(device,&requested)==0&&requested.low_latency==1&&requested.interval_us==19000&&driver_mode.interval_us==0,"Forced options never mutate caller memory");
    r::requested=false;
    require(cached_sleep(device)==17&&driver_mode.low_latency==1&&driver_mode.boost==1&&driver_mode.interval_us==19000,"Live disable restores latest game options");
    r::requested=true;r::processing=false;
    require(cached_sleep(device)==17,"Suspended processing leaves Reflex alone");
    r::processing=true;reflex_mode_result=-104;
    require(cached_sleep(device)==17&&r::last_status==-104,"Driver failure is reported and Sleep passes through");
    reflex_mode_result=0;
    require(cached_sleep(device)==0,"Failed mode application can recover on the next call");
    r::running=false;
    require(cached_sleep(device)==17&&driver_mode.low_latency==1,"Stopping interception restores game options");
    // Late injection: recover the existing mode even without seeing SetSleepMode.
    r::devices={};r::running=true;
    require(cached_sleep(device)==0&&driver_mode.low_latency==0,"Late-attached override disables an already enabled driver");
    r::requested=false;
    require(cached_sleep(device)==17&&driver_mode.low_latency==1,"Late-attached override restores prior enable state");
    require(cached_mode(device,nullptr)==-5,"Invalid calls retain driver error behavior");
    MH_DisableHook(reinterpret_cast<void*>(&fake_reflex_sleep));MH_DisableHook(reinterpret_cast<void*>(&fake_reflex_mode));
    MH_RemoveHook(reinterpret_cast<void*>(&fake_reflex_sleep));MH_RemoveHook(reinterpret_cast<void*>(&fake_reflex_mode));MH_Uninitialize();
    r::running=false;
}
}
