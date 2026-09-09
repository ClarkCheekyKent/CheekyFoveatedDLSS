#pragma once
#include <Windows.h>
#include <array>
#include <cwchar>

namespace cheeky::foveated_dlss {
struct RuntimeLibrarySearch {
    HMODULE module{};
    DWORD error{ERROR_MOD_NOT_FOUND};
    std::array<wchar_t, 32768> path{};
};
// Absolute module directories only; never consult the process working directory.
inline RuntimeLibrarySearch load_runtime_library(const wchar_t* filename,
    const wchar_t* primary, const wchar_t* executable,
    void (*report)(const wchar_t*, DWORD) = nullptr) noexcept {
    RuntimeLibrarySearch result;
    const wchar_t* directories[]{primary, executable};
    for (unsigned i = 0; i < 2; ++i) {
        const auto directory = directories[i];
        if (!directory || !*directory) continue;
        if (i == 1 && primary && _wcsicmp(primary, executable) == 0) continue;
        if (std::wcslen(directory) + std::wcslen(filename) + 2 > result.path.size()) {
            result.error = ERROR_FILENAME_EXCED_RANGE;
            continue;
        }
        swprintf_s(result.path.data(), result.path.size(), L"%s\\%s", directory, filename);
        result.module = LoadLibraryExW(result.path.data(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        result.error = result.module ? ERROR_SUCCESS : GetLastError();
        if (report) report(result.path.data(), result.error);
        if (result.module) break;
    }
    return result;
}
}
