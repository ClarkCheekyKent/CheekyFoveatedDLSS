#include "loader.hpp"

#include <cwchar>

namespace {
// Windows 10/11 x64 DXGI exports. Names are resolved independently so a private
// export missing on an older OS does not prevent supported exports from working.
// Keep these indices identical to dxgi_exports.asm and dxgi.def ordinals.
constexpr const char* exports[] = {
    "ApplyCompatResolutionQuirking", "CompatString", "CompatValue", "DXGIDumpJournal",
    "PIXBeginCapture", "PIXEndCapture", "PIXGetCaptureState", "SetAppCompatStringPointer",
    "UpdateHMDEmulationStatus", "CreateDXGIFactory", "CreateDXGIFactory1", "CreateDXGIFactory2",
    "DXGID3D10CreateDevice", "DXGID3D10CreateLayeredDevice", "DXGID3D10GetLayeredDeviceSize",
    "DXGID3D10RegisterLayers", "DXGIDeclareAdapterRemovalSupport", "DXGIDisableVBlankVirtualization",
    "DXGIGetDebugInterface1", "DXGIReportAdapterConfiguration"
};
INIT_ONCE g_dxgi_once = INIT_ONCE_STATIC_INIT;
HMODULE g_dxgi{};

BOOL CALLBACK load_system_dxgi(PINIT_ONCE, PVOID, PVOID*) noexcept {
    wchar_t path[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(path, ARRAYSIZE(path));
    constexpr wchar_t suffix[] = L"\\dxgi.dll";
    if (length == 0 || length + ARRAYSIZE(suffix) > ARRAYSIZE(path)) return FALSE;
    wcscat_s(path, suffix);
    // An absolute system path prevents recursion into the game-local proxy.
    g_dxgi = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return g_dxgi != nullptr;
}

[[noreturn]] void missing_system_export() noexcept {
    OutputDebugStringW(L"Cheeky: a required system DXGI export could not be resolved.\n");
    // Private DXGI signatures vary, so inventing an HRESULT return here would
    // corrupt callers with another ABI. Match an unresolved import's hard failure.
    RaiseFailFastException(nullptr, nullptr, 0);
    TerminateProcess(GetCurrentProcess(), ERROR_PROC_NOT_FOUND);
    __assume(0);
}
}

extern "C" {
// Read by the MASM thunks. Interlocked publication avoids torn/stale writes.
__declspec(align(8)) void* cheeky_dxgi_targets[ARRAYSIZE(exports)]{};

FARPROC cheeky_dxgi_resolve(unsigned index) noexcept {
    const DWORD error = GetLastError();
    if (index >= ARRAYSIZE(exports) ||
        !InitOnceExecuteOnce(&g_dxgi_once, load_system_dxgi, nullptr, nullptr)) missing_system_export();
    const FARPROC target = GetProcAddress(g_dxgi, exports[index]);
    if (target == nullptr) missing_system_export();
    InterlockedExchangePointer(&cheeky_dxgi_targets[index], reinterpret_cast<void*>(target));
    SetLastError(error);
    return target;
}

HRESULT WINAPI cheeky_proxy_CreateDXGIFactory(REFIID iid, void** factory) noexcept {
    using Fn = HRESULT (WINAPI*)(REFIID, void**);
    const HRESULT result = reinterpret_cast<Fn>(cheeky_dxgi_resolve(9))(iid, factory);
    const DWORD error = GetLastError();
    if (SUCCEEDED(result)) cheeky_bootstrap_after_factory();
    SetLastError(error);
    return result;
}

HRESULT WINAPI cheeky_proxy_CreateDXGIFactory1(REFIID iid, void** factory) noexcept {
    using Fn = HRESULT (WINAPI*)(REFIID, void**);
    const HRESULT result = reinterpret_cast<Fn>(cheeky_dxgi_resolve(10))(iid, factory);
    const DWORD error = GetLastError();
    if (SUCCEEDED(result)) cheeky_bootstrap_after_factory();
    SetLastError(error);
    return result;
}

HRESULT WINAPI cheeky_proxy_CreateDXGIFactory2(UINT flags, REFIID iid, void** factory) noexcept {
    using Fn = HRESULT (WINAPI*)(UINT, REFIID, void**);
    const HRESULT result = reinterpret_cast<Fn>(cheeky_dxgi_resolve(11))(flags, iid, factory);
    const DWORD error = GetLastError();
    if (SUCCEEDED(result)) cheeky_bootstrap_after_factory();
    SetLastError(error);
    return result;
}
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        cheeky_bootstrap_attach(module);
        // Start early, but perform all DLL loading and graphics work on a worker.
        // In particular, never wait for that worker in this entry point.
        cheeky_bootstrap_start(CheekyBootstrapHost::standalone);
    }
    return TRUE;
}
