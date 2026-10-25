#pragma once
#include "reflex_override_state.hpp"
#include <Windows.h>
#include <Unknwn.h>
#include <MinHook.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace cheeky::foveated_dlss::reflex_override {
// NVAPI DX11/DX12 Reflex ABI. NvBool is one byte, not a Windows BOOL.
// https://docs.nvidia.com/nvapi/nvapi_8h_source.html
struct SleepMode {
    std::uint32_t version{0x1002c};
    std::uint8_t low_latency{}, boost{};
    std::uint16_t padding{};
    std::uint32_t interval_us{};
    std::uint8_t optimize_markers{}, minimum_queue{};
    std::uint8_t reserved[30]{};
};
struct SleepStatus {
    std::uint32_t version{0x10088};
    std::uint8_t low_latency{}, fullscreen_vrr{}, cpl_vsync{}, reserved[126]{};
};
static_assert(sizeof(SleepMode)==44 && sizeof(SleepStatus)==136);
using SleepFn=int(__cdecl*)(IUnknown*);
using SetModeFn=int(__cdecl*)(IUnknown*,void*);
using GetStatusFn=int(__cdecl*)(IUnknown*,SleepStatus*);
using QueryFn=void*(__cdecl*)(unsigned);
inline std::atomic<SleepFn> original_sleep{};
inline std::atomic<SetModeFn> original_set_mode{};
inline GetStatusFn original_get_status{};
inline std::atomic<bool> attempted{};
inline std::atomic<std::uint64_t> suppressed_calls{};
inline std::atomic<int> last_status{};
struct DeviceState {
    IUnknown* device{}; // Identity only; never retain a game's graphics device.
    std::array<std::uint8_t,256> requested_mode{};
    bool known{}, applied{};
};
inline std::array<DeviceState,16> devices{};
inline std::mutex device_mutex;
inline DeviceState* device_state(IUnknown* device) noexcept {
    for(auto& state:devices)if(state.device==device)return &state;
    for(auto& state:devices)if(!state.device){state.device=device;return &state;}
    return nullptr;
}
inline unsigned mode_size(const void* params) noexcept {
    if(!params)return 0;
    std::uint32_t version{};std::memcpy(&version,params,sizeof(version));
    const auto size=version&0xffffu;
    return (version>>16)>=1 && size>=sizeof(SleepMode) && size<=256 ? size : 0;
}
inline void disable_mode(void* params) noexcept {
    // Clear the Reflex-owned frame limit too: otherwise the driver can move
    // the omitted sleep into another API call. Other game limiters are untouched.
    auto* bytes=static_cast<std::uint8_t*>(params);
    bytes[4]=bytes[5]=bytes[12]=bytes[13]=0;
    const std::uint32_t zero{};std::memcpy(bytes+8,&zero,sizeof(zero));
}
inline int __cdecl set_mode(IUnknown* device,void* params) {
    const auto original=original_set_mode.load(std::memory_order_acquire);
    const auto size=mode_size(params);
    if(!device||!size)return original(device,params);
    std::lock_guard lock(device_mutex);
    auto* state=device_state(device);if(!state)return original(device,params);
    std::array<std::uint8_t,256> copy{};std::memcpy(copy.data(),params,size);
    const bool force=active();if(force)disable_mode(copy.data());
    const int result=original(device,force?copy.data():params);
    last_status.store(result,std::memory_order_relaxed);
    if(result==0){
        std::memcpy(state->requested_mode.data(),params,size);state->known=true;state->applied=force;
    }
    return result;
}
inline int __cdecl sleep(IUnknown* device) {
    const auto original=original_sleep.load(std::memory_order_acquire);
    bool bypass{};
    if(device){
        std::lock_guard lock(device_mutex);
        const bool force=active();
        DeviceState* state{};
        for(auto& candidate:devices)if(candidate.device==device){state=&candidate;break;}
        if(force&&!state)state=device_state(device);
        if(state&&(force||state->applied)){
            if(!state->known){
                // Late injection can miss the initial SetSleepMode. Recover
                // the current enable state; subsequent calls save all options.
                SleepStatus status{};
                const int result=original_get_status?original_get_status(device,&status):-3;
                last_status.store(result,std::memory_order_relaxed);
                if(result==0){SleepMode mode{};mode.low_latency=status.low_latency;
                    std::memcpy(state->requested_mode.data(),&mode,sizeof(mode));state->known=true;}
            }
            if(state->known&&state->applied!=force){
                auto mode=state->requested_mode;if(force)disable_mode(mode.data());
                const int result=original_set_mode.load(std::memory_order_acquire)(device,mode.data());
                last_status.store(result,std::memory_order_relaxed);
                if(result==0)state->applied=force;
            }
            bypass=force&&state->applied;
        }
    }
    // Never hold our mutex across a real driver sleep.
    if(bypass){suppressed_calls.fetch_add(1,std::memory_order_relaxed);return 0;}
    return original(device);
}
// Direct detours cover pointers cached before injection. Keep trampolines
// resident like nvapi_d3d11_guard; inactive overrides pass calls through.
inline bool install_at(void* sleep_target,void* mode_target,GetStatusFn get_status) noexcept {
    void* sleep_trampoline{};void* mode_trampoline{};
    if(!sleep_target||!mode_target)return false;
    if(MH_CreateHook(sleep_target,reinterpret_cast<void*>(&sleep),&sleep_trampoline)!=MH_OK)return false;
    if(MH_CreateHook(mode_target,reinterpret_cast<void*>(&set_mode),&mode_trampoline)!=MH_OK){MH_RemoveHook(sleep_target);return false;}
    original_sleep.store(reinterpret_cast<SleepFn>(sleep_trampoline),std::memory_order_release);
    original_set_mode.store(reinterpret_cast<SetModeFn>(mode_trampoline),std::memory_order_release);
    original_get_status=get_status;
    if(MH_QueueEnableHook(sleep_target)!=MH_OK||MH_QueueEnableHook(mode_target)!=MH_OK){
        MH_RemoveHook(sleep_target);MH_RemoveHook(mode_target);return false;
    }
    if(MH_ApplyQueued()!=MH_OK)return false;
    installed.store(true,std::memory_order_release);return true;
}
inline bool install() noexcept {
    if(attempted.load(std::memory_order_acquire))return installed.load(std::memory_order_acquire);
    const auto module=GetModuleHandleW(L"nvapi64.dll");if(!module)return false;
    const auto query=reinterpret_cast<QueryFn>(GetProcAddress(module,"nvapi_QueryInterface"));if(!query)return false;
    attempted.store(true,std::memory_order_release);
    const auto sleep_target=query(0x852cd1d2u),mode_target=query(0xac1ca9e0u);
    if(!sleep_target||!mode_target)return false;
    HMODULE resident{};
    constexpr DWORD pin=GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN;
    if(!GetModuleHandleExW(pin,reinterpret_cast<LPCWSTR>(&sleep),&resident)||
       !GetModuleHandleExW(pin,reinterpret_cast<LPCWSTR>(query),&resident)||
       !GetModuleHandleExW(pin,reinterpret_cast<LPCWSTR>(sleep_target),&resident)||
       !GetModuleHandleExW(pin,reinterpret_cast<LPCWSTR>(mode_target),&resident))return false;
    return install_at(sleep_target,mode_target,reinterpret_cast<GetStatusFn>(query(0xaef96ca1u)));
}
}
