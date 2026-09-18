#include "loader.hpp"

#include <cwchar>

namespace {
HMODULE g_module{};
volatile LONG g_state{};
volatile LONG g_factory_wait_timed_out{};
CheekyBootstrapHost g_host{};
PVOID volatile g_worker{};
thread_local bool g_inside_worker{};

void report(const wchar_t* message) noexcept {
    OutputDebugStringW(message);
}

void enable_local_vulkan(HMODULE module) noexcept {
    // Publish before returning from DLL attach, before the game can create a
    // Vulkan instance. Only Kernel32 path/environment operations occur here;
    // the Vulkan loader loads the layer later, outside this entry point.
    wchar_t directory[32768]{}, existing[32768]{}, combined[32768]{};
    const DWORD length=GetModuleFileNameW(module,directory,ARRAYSIZE(directory));
    auto* filename=wcsrchr(directory,L'\\');
    constexpr wchar_t suffix[]=L"CheekyFoveatedDLSS\\Vulkan";
    if(!length || length>=ARRAYSIZE(directory) || !filename ||
        static_cast<size_t>(filename+1-directory)+ARRAYSIZE(suffix)>ARRAYSIZE(directory))return;
    wcscpy_s(filename+1,ARRAYSIZE(directory)-static_cast<size_t>(filename+1-directory),suffix);
    const auto attributes=GetFileAttributesW(directory);
    if(attributes==INVALID_FILE_ATTRIBUTES || !(attributes&FILE_ATTRIBUTE_DIRECTORY))return;
    // Respect and preserve an existing explicit search-path override as well
    // as other mods' additional implicit-layer directories.
    const wchar_t* variable=L"VK_IMPLICIT_LAYER_PATH";
    DWORD count=GetEnvironmentVariableW(variable,existing,ARRAYSIZE(existing));
    if(!count) {variable=L"VK_ADD_IMPLICIT_LAYER_PATH";count=GetEnvironmentVariableW(variable,existing,ARRAYSIZE(existing));}
    if(count>=ARRAYSIZE(existing))return;
    for(const wchar_t* part=existing;*part;) {
        const auto* end=wcschr(part,L';');const size_t size=end?static_cast<size_t>(end-part):wcslen(part);
        if(size==wcslen(directory) && _wcsnicmp(part,directory,size)==0)return;
        if(!end)break;part=end+1;
    }
    if(wcslen(directory)+count+2>ARRAYSIZE(combined))return;
    wcscpy_s(combined,directory);
    if(count){wcscat_s(combined,L";");wcscat_s(combined,existing);}
    if(!SetEnvironmentVariableW(variable,combined))report(L"Cheeky: could not activate the process-local Vulkan layer.\n");
}

DWORD WINAPI start_host(void*) noexcept {
    g_inside_worker = true;
    // Windows serializes thread startup against DllMain notifications. The
    // thread is never waited for from an entry point, so host loading cannot
    // run under the entry point's loader lock, including an ASI loader's lock.
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(g_module, path, ARRAYSIZE(path));
    if (length == 0 || length >= ARRAYSIZE(path)) {
        report(L"Cheeky: cannot locate bootstrap DLL.\n");
        InterlockedExchange(&g_state, 3);
        return 0;
    }
    wchar_t* filename = wcsrchr(path, L'\\');
    constexpr wchar_t suffix[] = L"CheekyFoveatedDLSS\\CheekyFoveatedDLSSHost.dll";
    if (filename == nullptr || static_cast<size_t>(filename + 1 - path) + ARRAYSIZE(suffix) > ARRAYSIZE(path)) {
        report(L"Cheeky: bootstrap path is too long.\n");
        InterlockedExchange(&g_state, 3);
        return 0;
    }
    wcscpy_s(filename + 1, ARRAYSIZE(path) - static_cast<size_t>(filename + 1 - path), suffix);

    // No current-working-directory or PATH search. The runtime and its private
    // dependencies live beneath this loader, including when it is an ASI.
    HMODULE host = LoadLibraryExW(path, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (host == nullptr) {
        report(L"Cheeky: missing or unloadable CheekyFoveatedDLSS/CheekyFoveatedDLSSHost.dll.\n");
        InterlockedExchange(&g_state, 3);
        return 0;
    }

    using StartFn = bool (*)(unsigned);
    const auto start = reinterpret_cast<StartFn>(GetProcAddress(host, "CheekyHost_Start"));
    // Retain the reference even on initialization failure. A host may already
    // have installed process-resident hooks before reporting its error.
    const bool started = start != nullptr && start(static_cast<unsigned>(g_host));
    if (!started) report(L"Cheeky: host initialization failed; see the Cheeky host log.\n");
    InterlockedExchange(&g_state, started ? 2 : 3);
    return 0;
}
}

void cheeky_bootstrap_attach(HMODULE module) noexcept {
    g_module = module;
    enable_local_vulkan(module);
}

void cheeky_bootstrap_start(CheekyBootstrapHost host) noexcept {
    if (InterlockedCompareExchange(&g_state, 1, 0) != 0) return;

    // Keep the worker's code mapped even if a host calls FreeLibrary straight
    // after InitializeASI. This neither loads a DLL nor waits for any thread.
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&cheeky_bootstrap_start), &pinned)) {
        InterlockedExchange(&g_state, 3);
        return;
    }
    g_host = host;
    const HANDLE worker = CreateThread(nullptr, 0, start_host, nullptr, 0, nullptr);
    if (worker == nullptr) {
        report(L"Cheeky: could not start the host worker.\n");
        InterlockedExchange(&g_state, 3);
        return;
    }
    // One process-resident handle permits a successful factory call to wait
    // for startup before the game creates its first swap chain.
    InterlockedExchangePointer(&g_worker, worker);
}

void cheeky_bootstrap_after_factory() noexcept {
    if (g_inside_worker || InterlockedCompareExchange(&g_state, 0, 0) != 1 ||
        InterlockedCompareExchange(&g_factory_wait_timed_out, 0, 0) != 0) return;
    const HANDLE worker = InterlockedCompareExchangePointer(&g_worker, nullptr, nullptr);
    if (worker != nullptr && WaitForSingleObject(worker, 10000) == WAIT_TIMEOUT) {
        InterlockedExchange(&g_factory_wait_timed_out, 1);
        report(L"Cheeky: graphics host startup timed out; DXGI remains available.\n");
    }
}

extern "C" __declspec(dllexport) unsigned CheekyBootstrap_Status() noexcept {
    return static_cast<unsigned>(InterlockedCompareExchange(&g_state, 0, 0));
}
