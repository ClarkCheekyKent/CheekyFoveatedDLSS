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
HMODULE g_proxy{}, g_chain{};
INIT_ONCE g_chain_once = INIT_ONCE_STATIC_INIT;
thread_local bool g_loading_chain{};
// A chained mod may hook these proxy exports and keep a trampoline back here.
// Nested calls bypass the chain; a second bounce fails instead of overflowing.
thread_local unsigned g_forward_depth{};

struct ForwardCallScope {
    ForwardCallScope() noexcept { ++g_forward_depth; }
    ~ForwardCallScope() { --g_forward_depth; }
};

BOOL CALLBACK load_chain(PINIT_ONCE, PVOID, PVOID*) noexcept {
    // Reentrant exports during the second proxy's DllMain use System32 directly.
    g_loading_chain = true;
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(g_proxy, path, ARRAYSIZE(path));
    auto filename = wcsrchr(path, L'\\');
    if (length && length < ARRAYSIZE(path) && filename) {
        const size_t remaining = ARRAYSIZE(path) - (filename + 1 - path);
        if (remaining >= ARRAYSIZE(L"CheekyFoveatedDLSS-Loader.log")) {
            wcscpy_s(filename + 1, remaining, L"dxgi2.dll");
            const DWORD attributes = GetFileAttributesW(path);
            const wchar_t* message = L"dxgi2.dll absent; forwarding to System32 DXGI";
            DWORD error = 0;
            if (attributes != INVALID_FILE_ATTRIBUTES || GetLastError() != ERROR_FILE_NOT_FOUND) {
                const HMODULE candidate = LoadLibraryExW(path, nullptr,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
                error = candidate ? 0 : GetLastError();
                if (candidate && candidate != g_proxy && !GetProcAddress(candidate, "CheekyBootstrap_Status")) {
                    g_chain = candidate;
                    message = L"dxgi2.dll loaded; forwarding its exports, with System32 fallback for missing exports";
                } else {
                    message = candidate ? L"dxgi2.dll is another Cheeky loader; ignored" :
                        L"dxgi2.dll failed to load; forwarding to System32 DXGI";
                }
                // Retain loaded DLLs: third-party initialization may install hooks.
            }
            wchar_t line[512]{};
            swprintf_s(line, L"Cheeky: %s (Win32=%lu)\r\n", message, error);
            OutputDebugStringW(line);
            wcscpy_s(filename + 1, remaining, L"CheekyFoveatedDLSS-Loader.log");
            const HANDLE log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (log != INVALID_HANDLE_VALUE) {
                const wchar_t bom = 0xfeff;
                DWORD written{};
                WriteFile(log, &bom, sizeof(bom), &written, nullptr);
                WriteFile(log, line, static_cast<DWORD>(wcslen(line) * sizeof(wchar_t)), &written, nullptr);
                CloseHandle(log);
            }
        }
    }
    g_loading_chain = false;
    return TRUE;
}

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
    const bool reentrant = g_loading_chain;
    if (!reentrant) InitOnceExecuteOnce(&g_chain_once, load_chain, nullptr, nullptr);
    // Some proxies advertise private DXGI exports but leave their forwarding
    // pointers null when renamed. Only chain the supported public entry points;
    // Windows' own D3D11 runtime calls private helpers such as CompatValue.
    const bool chainable = (index >= 9 && index <= 11) || index == 16 || index == 18;
    FARPROC target = !reentrant && g_chain && chainable ? GetProcAddress(g_chain, exports[index]) : nullptr;
    if (!target) target = GetProcAddress(g_dxgi, exports[index]);
    if (target == nullptr) missing_system_export();
    // Do not permanently bypass the chain when resolving during its initialization.
    if (!reentrant) InterlockedExchangePointer(&cheeky_dxgi_targets[index], reinterpret_cast<void*>(target));
    SetLastError(error);
    return target;
}

HRESULT WINAPI cheeky_proxy_CreateDXGIFactory(REFIID iid, void** factory) noexcept {
    ForwardCallScope scope;
    if (g_forward_depth > 2) {
        if (factory) *factory = nullptr;
        return static_cast<HRESULT>(0x887A0001L); // DXGI_ERROR_INVALID_CALL
    }
    using Fn = HRESULT (WINAPI*)(REFIID, void**);
    const FARPROC target = g_forward_depth > 1 ? GetProcAddress(g_dxgi, "CreateDXGIFactory") : cheeky_dxgi_resolve(9);
    if (!target) missing_system_export();
    const HRESULT result = reinterpret_cast<Fn>(target)(iid, factory);
    const DWORD error = GetLastError();
    if (SUCCEEDED(result) && !g_loading_chain && g_forward_depth == 1) cheeky_bootstrap_after_factory();
    SetLastError(error);
    return result;
}

HRESULT WINAPI cheeky_proxy_CreateDXGIFactory1(REFIID iid, void** factory) noexcept {
    ForwardCallScope scope;
    if (g_forward_depth > 2) {
        if (factory) *factory = nullptr;
        return static_cast<HRESULT>(0x887A0001L);
    }
    using Fn = HRESULT (WINAPI*)(REFIID, void**);
    const FARPROC target = g_forward_depth > 1 ? GetProcAddress(g_dxgi, "CreateDXGIFactory1") : cheeky_dxgi_resolve(10);
    if (!target) missing_system_export();
    const HRESULT result = reinterpret_cast<Fn>(target)(iid, factory);
    const DWORD error = GetLastError();
    if (SUCCEEDED(result) && !g_loading_chain && g_forward_depth == 1) cheeky_bootstrap_after_factory();
    SetLastError(error);
    return result;
}

HRESULT WINAPI cheeky_proxy_CreateDXGIFactory2(UINT flags, REFIID iid, void** factory) noexcept {
    ForwardCallScope scope;
    if (g_forward_depth > 2) {
        if (factory) *factory = nullptr;
        return static_cast<HRESULT>(0x887A0001L);
    }
    using Fn = HRESULT (WINAPI*)(UINT, REFIID, void**);
    const FARPROC target = g_forward_depth > 1 ? GetProcAddress(g_dxgi, "CreateDXGIFactory2") : cheeky_dxgi_resolve(11);
    if (!target) missing_system_export();
    const HRESULT result = reinterpret_cast<Fn>(target)(flags, iid, factory);
    const DWORD error = GetLastError();
    if (SUCCEEDED(result) && !g_loading_chain && g_forward_depth == 1) cheeky_bootstrap_after_factory();
    SetLastError(error);
    return result;
}

HRESULT WINAPI cheeky_proxy_DXGIDeclareAdapterRemovalSupport() noexcept {
    ForwardCallScope scope;
    if (g_forward_depth > 2) return static_cast<HRESULT>(0x887A0001L);
    using Fn = HRESULT (WINAPI*)();
    const FARPROC target = g_forward_depth > 1 ? GetProcAddress(g_dxgi, "DXGIDeclareAdapterRemovalSupport") : cheeky_dxgi_resolve(16);
    if (!target) missing_system_export();
    return reinterpret_cast<Fn>(target)();
}

HRESULT WINAPI cheeky_proxy_DXGIGetDebugInterface1(UINT flags, REFIID iid, void** output) noexcept {
    ForwardCallScope scope;
    if (g_forward_depth > 2) {
        if (output) *output = nullptr;
        return static_cast<HRESULT>(0x887A0001L);
    }
    using Fn = HRESULT (WINAPI*)(UINT, REFIID, void**);
    const FARPROC target = g_forward_depth > 1 ? GetProcAddress(g_dxgi, "DXGIGetDebugInterface1") : cheeky_dxgi_resolve(18);
    if (!target) missing_system_export();
    return reinterpret_cast<Fn>(target)(flags, iid, output);
}
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_proxy = module;
        cheeky_bootstrap_attach(module);
        // Start early, but perform all DLL loading and graphics work on a worker.
        // In particular, never wait for that worker in this entry point.
        cheeky_bootstrap_start(CheekyBootstrapHost::standalone);
    }
    return TRUE;
}
