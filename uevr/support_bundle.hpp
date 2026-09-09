#pragma once
#include "support_zip.hpp"
#include "version.h"
#include <Windows.h>
#include <sstream>
#include <atomic>

namespace cheeky::foveated_dlss {
inline std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
inline std::wstring support_issue_url(const std::filesystem::path& zip, const std::string& summary) {
    auto encode = [](const std::string& value) {
        constexpr wchar_t hex[] = L"0123456789ABCDEF";
        std::wstring out;
        for (unsigned char c : value) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') out += c;
            else { out += L'%'; out += hex[c >> 4]; out += hex[c & 15]; }
        }
        return out;
    };
    return L"https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/issues/new?template=bug_report.yml&title=UEVR%20bug%20report&environment="
        + encode("Cheeky " CHEEKY_VERSION " UEVR plugin") + L"&report=" + encode(path_utf8(zip.filename()))
        + L"&diagnostics=" + encode(summary) + L"&problem=Describe%20the%20problem%20here.&steps=Describe%20how%20to%20reproduce%20it.";
}
inline std::filesystem::path create_uevr_support_bundle(const std::filesystem::path& directory,
    const std::string& snapshot, const std::string& settings, const std::string& summary) {
    std::vector<SupportFile> files{{"diagnostics.json", snapshot}, {"settings.ini", settings},
        {"issue-report.md", "# Cheeky UEVR support report\n\n" + summary + "\n\nFull diagnostics: diagnostics.json\nReview the files before sharing; logs may contain personal paths.\n"}};
    std::ostringstream manifest;
    manifest << "Cheeky " CHEEKY_VERSION " UEVR support bundle\nLogs are limited to the latest 2 MiB per file.\n";
    for (const auto* name : {"CheekyFoveatedDLSS-UEVR.log", "log.txt", "config.txt"}) {
        const auto source = directory / name;
        const auto file = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { manifest << name << ": unavailable (Windows error " << GetLastError() << ")\n"; continue; }
        struct CloseFile { HANDLE handle; ~CloseFile() { CloseHandle(handle); } } close{file};
        LARGE_INTEGER end{}, start{};
        if (!GetFileSizeEx(file, &end)) { manifest << name << ": size unavailable\n"; continue; }
        const auto count = static_cast<DWORD>((std::min)(end.QuadPart, LONGLONG{2 * 1024 * 1024}));
        start.QuadPart = end.QuadPart - count;
        if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) { manifest << name << ": seek failed\n"; continue; }
        std::string contents(count, '\0'); DWORD read{};
        if (!ReadFile(file, contents.data(), count, &read, nullptr)) { manifest << name << ": read failed\n"; continue; }
        contents.resize(read);
        manifest << name << ": captured " << contents.size() << " of " << end.QuadPart << " bytes\n";
        files.push_back({name, std::move(contents)});
    }
    files.push_back({"README.txt", manifest.str()});
    const auto folder = directory / L"support";
    std::filesystem::create_directories(folder);
    static std::atomic<std::uint64_t> sequence{};
    const auto stem = L"Cheeky-UEVR-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence);
    const auto partial = folder / (stem + L".partial"), zip = folder / (stem + L".zip");
    write_support_zip(partial, files);
    std::filesystem::rename(partial, zip);
    return zip;
}
}
