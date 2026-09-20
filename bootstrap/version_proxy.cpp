#include "loader.hpp"
#include <cwchar>

namespace {
// Keep the order aligned with version.def and version_exports.asm.
constexpr const char* exports[] = {
    "GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW", "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW", "GetFileVersionInfoW",
    "VerFindFileA", "VerFindFileW", "VerInstallFileA", "VerInstallFileW",
    "VerLanguageNameA", "VerLanguageNameW", "VerQueryValueA", "VerQueryValueW"
};
INIT_ONCE once = INIT_ONCE_STATIC_INIT;
HMODULE system_version{};

BOOL CALLBACK load_system_version(PINIT_ONCE, PVOID, PVOID*) noexcept {
    wchar_t path[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(path, ARRAYSIZE(path));
    constexpr wchar_t suffix[] = L"\\version.dll";
    if (!length || length + ARRAYSIZE(suffix) > ARRAYSIZE(path)) return FALSE;
    wcscat_s(path, suffix);
    system_version = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return system_version != nullptr;
}
}

extern "C" {
__declspec(align(8)) void* cheeky_version_targets[ARRAYSIZE(exports)]{};

FARPROC cheeky_version_resolve(unsigned index) noexcept {
    const auto error = GetLastError();
    FARPROC target{};
    if (index < ARRAYSIZE(exports) && InitOnceExecuteOnce(&once, load_system_version, nullptr, nullptr))
        target = GetProcAddress(system_version, exports[index]);
    if (!target) {
        OutputDebugStringW(L"Cheeky: a required System32 VERSION export could not be resolved.\n");
        RaiseFailFastException(nullptr, nullptr, 0);
        TerminateProcess(GetCurrentProcess(), ERROR_PROC_NOT_FOUND);
        __assume(0);
    }
    InterlockedExchangePointer(&cheeky_version_targets[index], reinterpret_cast<void*>(target));
    SetLastError(error);
    return target;
}
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        cheeky_bootstrap_attach(module);
        cheeky_bootstrap_start(CheekyBootstrapHost::standalone);
        // Version queries may occur under the loader lock. Never wait for the
        // host from an export; graphics hooks are installed by the shared worker.
    }
    return TRUE;
}
