#include <Windows.h>
#include <dxgi1_2.h>
#include <cwchar>

namespace {
volatile LONG g_starts{}, g_kind{}, g_thread{}, g_recursive_factory{};
HMODULE g_module{};
volatile LONG g_chain_calls{};
}

// The loop fixture simulates a mod whose original function points back into
// Cheeky. Bound recursion so the unfixed loader fails a test without crashing.
thread_local unsigned fake_depth{};
struct FakeScope { FakeScope() { ++fake_depth; } ~FakeScope() { --fake_depth; } };
HMODULE factory_target() {
    wchar_t mode[2]{};
    if (GetEnvironmentVariableW(L"CHEEKY_TEST_CHAIN_LOOP", mode, ARRAYSIZE(mode))) {
        wchar_t path[32768]{};
        GetModuleFileNameW(g_module, path, ARRAYSIZE(path));
        auto slash = wcsrchr(path, L'\\');
        wcscpy_s(slash + 1, ARRAYSIZE(path) - (slash + 1 - path), L"dxgi.dll");
        return GetModuleHandleW(path);
    }
    wchar_t path[MAX_PATH]{};
    GetSystemDirectoryW(path, ARRAYSIZE(path));
    wcscat_s(path, L"\\dxgi.dll");
    return LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

HRESULT fake_factory(const char* name, REFIID iid, void** factory) {
    FakeScope scope;
    if (fake_depth > 8) return E_UNEXPECTED;
    InterlockedIncrement(&g_chain_calls);
    using Fn = HRESULT (WINAPI*)(REFIID, void**);
    return reinterpret_cast<Fn>(GetProcAddress(factory_target(), name))(iid, factory);
}
#pragma comment(linker, "/EXPORT:CreateDXGIFactory=FakeCreateDXGIFactory")
#pragma comment(linker, "/EXPORT:CreateDXGIFactory1=FakeCreateDXGIFactory1")
#pragma comment(linker, "/EXPORT:CreateDXGIFactory2=FakeCreateDXGIFactory2")
extern "C" HRESULT WINAPI FakeCreateDXGIFactory(REFIID iid, void** factory) {
    return fake_factory("CreateDXGIFactory", iid, factory);
}
extern "C" HRESULT WINAPI FakeCreateDXGIFactory1(REFIID iid, void** factory) {
    return fake_factory("CreateDXGIFactory1", iid, factory);
}
extern "C" HRESULT WINAPI FakeCreateDXGIFactory2(UINT flags, REFIID iid, void** factory) {
    FakeScope scope;
    if (fake_depth > 8) return E_UNEXPECTED;
    InterlockedIncrement(&g_chain_calls);
    using Fn = HRESULT (WINAPI*)(UINT, REFIID, void**);
    return reinterpret_cast<Fn>(GetProcAddress(factory_target(), "CreateDXGIFactory2"))(flags, iid, factory);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
    return 0x1234; // Distinguishes MASM forwarding from System32 fallback.
}

extern "C" __declspec(dllexport) bool CheekyHost_Start(unsigned kind) {
    InterlockedIncrement(&g_starts);
    InterlockedExchange(&g_kind, static_cast<LONG>(kind));
    InterlockedExchange(&g_thread, static_cast<LONG>(GetCurrentThreadId()));
    Sleep(40); // Make concurrent duplicate initialization observable.
    if (kind == 1) {
        wchar_t path[32768]{};
        if (!GetModuleFileNameW(g_module, path, ARRAYSIZE(path))) return false;
        auto slash = wcsrchr(path, L'\\'); if (!slash) return false; *slash = 0;
        slash = wcsrchr(path, L'\\'); if (!slash) return false;
        wcscpy_s(slash + 1, ARRAYSIZE(path) - static_cast<size_t>(slash + 1 - path), L"dxgi.dll");
        auto proxy = GetModuleHandleW(path);
        using FactoryFn = HRESULT (WINAPI*)(REFIID, void**);
        const auto factory = reinterpret_cast<FactoryFn>(GetProcAddress(proxy, "CreateDXGIFactory1"));
        IDXGIFactory1* value{};
        if (!factory || FAILED(factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&value)))) return false;
        value->Release();
        InterlockedExchange(&g_recursive_factory, 1);
    }
    return kind == 1 || kind == 2;
}

extern "C" __declspec(dllexport) unsigned CheekyFakeHost_Value(unsigned which) {
    if (which == 4) return static_cast<unsigned>(InterlockedCompareExchange(&g_chain_calls, 0, 0));
    volatile LONG* value = which == 0 ? &g_starts : which == 1 ? &g_kind : which == 2 ? &g_thread : &g_recursive_factory;
    return static_cast<unsigned>(InterlockedCompareExchange(value, 0, 0));
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_module = module;
    return TRUE;
}
