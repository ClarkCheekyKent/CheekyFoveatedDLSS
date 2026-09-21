#pragma once
#include <Windows.h>
#include <winver.h>
#include <array>
#include <vector>
#include <cwchar>

namespace cheeky::foveated_dlss {
// Use the installed module's product identity, not a generic proxy filename.
// Reading VERSIONINFO does not execute or inspect private R.E.A.L. VR code.
inline bool is_realvr_runtime(HMODULE module) noexcept {
    try {
        std::array<wchar_t, 32768> path{};
        const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (!module || !length || length >= path.size()) return false;
        DWORD unused{};
        const auto size = GetFileVersionInfoSizeW(path.data(), &unused);
        if (!size || size > 1024U * 1024U) return false;
        std::vector<unsigned char> bytes(size);
        if (!GetFileVersionInfoW(path.data(), 0, size, bytes.data())) return false;
        struct Translation { WORD language, codepage; };
        Translation* translations{}; UINT count{};
        if (!VerQueryValueW(bytes.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &count)) return false;
        for (unsigned i = 0; i < count / sizeof(Translation); ++i) {
            wchar_t key[96]{};
            swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\ProductName", translations[i].language, translations[i].codepage);
            wchar_t* value{}; UINT chars{};
            if (VerQueryValueW(bytes.data(), key, reinterpret_cast<void**>(&value), &chars) &&
                chars == std::size(L"R.E.A.L. VR") && value &&
                std::wmemcmp(value, L"R.E.A.L. VR", std::size(L"R.E.A.L. VR")) == 0) return true;
        }
    } catch (...) {}
    return false;
}
}
