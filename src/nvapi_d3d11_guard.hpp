#pragma once
#include <Windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <atomic>

namespace cheeky::foveated_dlss::nvapi_guard {
// NVAPI D3D11 entry points that record into a context enter the NVIDIA driver
// directly, without the D3D11 runtime's device lock. Frame generation uses the
// game's immediate context from the OpenXR layer's presenter thread under that
// lock (ID3D11Multithread), so an unlocked NVAPI call from the game's render
// thread races it. UE4 toggles NvAPI_D3D11_SetDepthBoundsTest around every
// deferred light; in --fg-shader11-nvapi that race hung or crashed the
// driver's worker within one frame, matching the ACC dumps, and never did
// with the lock held. Only short, non-blocking recording calls are wrapped.
// The implementations themselves are detoured, so pointers the game cached
// before installation are covered too.
using QueryInterfaceFn=void*(__cdecl*)(unsigned);
using DepthBoundsFn=int(__cdecl*)(IUnknown*,unsigned,float,float);
using OverlapFn=int(__cdecl*)(IUnknown*);
constexpr unsigned set_depth_bounds_id=0x7AAF7A04u,begin_uav_overlap_id=0x65B93CA8u,end_uav_overlap_id=0x2216A357u;
inline std::atomic<DepthBoundsFn> original_depth_bounds{};
inline std::atomic<OverlapFn> original_begin_overlap{},original_end_overlap{};
inline std::atomic<bool> installed{};
inline std::atomic<unsigned long long> guarded_calls{};

// Holds the immediate context's D3D11 lock for a device or immediate context.
// Deferred contexts record privately and need no lock.
struct DeviceLock {
    Microsoft::WRL::ComPtr<ID3D11Multithread> lock;
    explicit DeviceLock(IUnknown* target) noexcept {
        if(!target)return;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;Microsoft::WRL::ComPtr<ID3D11Device> device;
        if(SUCCEEDED(target->QueryInterface(IID_PPV_ARGS(&context)))){
            if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
        }else if(SUCCEEDED(target->QueryInterface(IID_PPV_ARGS(&device))))device->GetImmediateContext(&context);
        if(context&&SUCCEEDED(context.As(&lock))){lock->Enter();guarded_calls.fetch_add(1,std::memory_order_relaxed);}
        else lock.Reset();
    }
    ~DeviceLock(){if(lock)lock->Leave();}
    DeviceLock(const DeviceLock&)=delete;
    DeviceLock& operator=(const DeviceLock&)=delete;
};
inline int __cdecl set_depth_bounds(IUnknown* target,unsigned enable,float minimum,float maximum) {
    DeviceLock lock(target);return original_depth_bounds.load(std::memory_order_acquire)(target,enable,minimum,maximum);
}
inline int __cdecl begin_uav_overlap(IUnknown* target) {
    DeviceLock lock(target);return original_begin_overlap.load(std::memory_order_acquire)(target);
}
inline int __cdecl end_uav_overlap(IUnknown* target) {
    DeviceLock lock(target);return original_end_overlap.load(std::memory_order_acquire)(target);
}
template<class Function> bool detour(void* target,Function replacement,std::atomic<Function>& original) noexcept {
    if(!target)return false;void* trampoline{};
    if(MH_CreateHook(target,reinterpret_cast<void*>(replacement),&trampoline)!=MH_OK)return false;
    original.store(reinterpret_cast<Function>(trampoline),std::memory_order_release);
    const auto enabled=MH_EnableHook(target);
    if(enabled==MH_OK||enabled==MH_ERROR_ENABLED)return true;
    original.store(nullptr,std::memory_order_release);MH_RemoveHook(target);return false;
}
// Call after MH_Initialize, outside the loader lock, once nvapi64.dll is
// loaded. Games cache the entry points, so the detours and this module stay
// resident. Returns true once all available entry points are guarded.
inline bool install() noexcept {
    if(installed.load(std::memory_order_acquire))return true;
    const auto nvapi=GetModuleHandleW(L"nvapi64.dll");if(!nvapi)return false;
    const auto query=reinterpret_cast<QueryInterfaceFn>(GetProcAddress(nvapi,"nvapi_QueryInterface"));if(!query)return false;
    HMODULE resident{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&set_depth_bounds),&resident))return false;
    // A missing entry point (older driver) is simply not used by the game.
    const auto depth_bounds=query(set_depth_bounds_id),begin_overlap=query(begin_uav_overlap_id),end_overlap=query(end_uav_overlap_id);
    const bool good=(!depth_bounds||detour(depth_bounds,&set_depth_bounds,original_depth_bounds))&&
        (!begin_overlap||detour(begin_overlap,&begin_uav_overlap,original_begin_overlap))&&
        (!end_overlap||detour(end_overlap,&end_uav_overlap,original_end_overlap));
    installed.store(true,std::memory_order_release); // Never retry a partial install.
    return good;
}
}
