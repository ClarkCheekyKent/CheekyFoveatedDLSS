#pragma once
#include <string>
#include <string_view>

namespace cheeky::foveated_dlss {
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
}
