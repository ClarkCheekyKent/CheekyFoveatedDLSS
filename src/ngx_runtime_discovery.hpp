#pragma once
#include <Windows.h>
#include <string>
#include <string_view>

namespace cheeky::foveated_dlss {
// The same pinned callback owner supplies private SR transport and the NR
// parameter allocator. Implemented by hooks.cpp in processing-runtime builds.
HMODULE find_loaded_ngx_core_runtime() noexcept;

inline bool ngx_identity_equal(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i != a.size(); ++i) {
        auto lower = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

// OptiScaler can load NVIDIA's core as nvngx.dll rather than _nvngx.dll. The
// filename alone is unsafe: upscaler-replacement proxies also use nvngx.dll.
// These version fields distinguish the supported NVIDIA binary; this is a
// compatibility classification, not a cryptographic authenticity guarantee.
inline bool is_nvidia_ngx_core_alias_identity(std::wstring_view path,
    std::wstring_view company, std::wstring_view original, std::wstring_view product,
    bool complete_ngx_exports, bool has_proxy_exports) noexcept {
    const auto name = path.substr(path.find_last_of(L"/\\") + 1);
    return ngx_identity_equal(name, L"nvngx.dll") &&
        ngx_identity_equal(company, L"NVIDIA Corporation") &&
        (ngx_identity_equal(original, L"nvngx.dll") || ngx_identity_equal(original, L"_nvngx.dll")) &&
        ngx_identity_equal(product, L"NGX") && complete_ngx_exports && !has_proxy_exports;
}

// NVIDIA OTA snippets keep the NGX exports but use generated filenames. Match
// the SR model directory specifically; NR, RR, FG and Streamline are different
// runtimes and must never share SR's private callbacks.
inline bool is_dlss_sr_runtime_path(std::wstring_view path) {
    std::wstring normalized;
    normalized.reserve(path.size());
    for (auto c : path) {
        if (c == L'/') c = L'\\';
        else if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
        if (c == L'\\' && !normalized.empty() && normalized.back() == c) continue;
        normalized.push_back(c);
    }
    const auto name = std::wstring_view(normalized).substr(normalized.find_last_of(L'\\') + 1);
    if (name == L"nvngx_dlss.dll") return true;
    return (name.ends_with(L".bin") || name.ends_with(L".dll")) &&
        normalized.find(L"\\ngx\\models\\dlss\\versions\\") != std::wstring::npos;
}
// RR owns a separate callback set even when both model DLLs are loaded.
inline bool is_dlss_rr_runtime_path(std::wstring_view path) {
    std::wstring normalized(path);
    for (auto& c : normalized) {
        if (c == L'/') c = L'\\';
        else if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    }
    const auto name = std::wstring_view(normalized).substr(normalized.find_last_of(L'\\') + 1);
    return name == L"nvngx_dlssd.dll" ||
        ((name.ends_with(L".bin") || name.ends_with(L".dll")) &&
         normalized.find(L"\\ngx\\models\\dlssd\\versions\\") != std::wstring::npos);
}

}
