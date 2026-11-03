#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "version.h"

namespace cheeky::startup {
using Log = void (*)(const char*) noexcept;
// Logging must not change the error observed by a caller or another injector.
struct PreserveError {
    DWORD value{GetLastError()};
    ~PreserveError() { SetLastError(value); }
};
inline void host_log(const char* message) noexcept {
    PreserveError error;
    using Report = void (__cdecl*)(const char*);
    static const auto report = [] {
        const auto host = GetModuleHandleW(L"CheekyFoveatedDLSSHost.dll");
        return host ? reinterpret_cast<Report>(GetProcAddress(host, "CheekyOpenXRMenuLogDiagnostic")) : nullptr;
    }();
    if (report) report(message);
}
struct Step {
    Log log;
    const char* name;
    ULONGLONG began{GetTickCount64()};
    bool ended{};
    Step(Log sink, const char* stage) noexcept : log(sink), name(stage) {
        PreserveError error;
        char line[256]{};
        snprintf(line, sizeof(line), "STARTUP stage=%s begin", name);
        log(line);
    }
    Step(const Step&) = delete;
    Step& operator=(const Step&) = delete;
    void end(const char* domain = "status", unsigned long result = 0) noexcept {
        PreserveError error;
        char line[320]{};
        snprintf(line, sizeof(line), "STARTUP stage=%s end %s=0x%08lX elapsed_ms=%llu",
            name, domain, result, GetTickCount64() - began);
        ended = true;
        log(line);
    }
    ~Step() {
        if (!ended) {
            PreserveError error;
            char line[320]{};
            snprintf(line, sizeof(line), "STARTUP stage=%s interrupted elapsed_ms=%llu",
                name, GetTickCount64() - began);
            log(line);
        }
    }
};
template<class F> auto status(Log log, const char* stage, const char* domain, F&& operation) {
    Step step(log, stage);
    const auto result = operation();
    step.end(domain, static_cast<unsigned long>(result));
    return result;
}
template<class F> auto win32(Log log, const char* stage, F&& operation) {
    Step step(log, stage);
    const auto result = operation();
    const auto error = result ? ERROR_SUCCESS : GetLastError();
    step.end("win32", error);
    return result;
}
inline void identity(Log log, const char* component, HMODULE module) noexcept {
    PreserveError error;
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = base ? reinterpret_cast<const IMAGE_DOS_HEADER*>(base) : nullptr;
    const auto* image = dos && dos->e_magic == IMAGE_DOS_SIGNATURE
        ? reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew) : nullptr;
    char line[768]{};
    snprintf(line, sizeof(line), "STARTUP session=%lu-%08lX%08lX component=%s version=%s compiled=%s %s image_timestamp=%08lX image_size=%lu",
        GetCurrentProcessId(), created.dwHighDateTime, created.dwLowDateTime, component,
        CHEEKY_VERSION, __DATE__, __TIME__, image ? image->FileHeader.TimeDateStamp : 0,
        image ? image->OptionalHeader.SizeOfImage : 0);
    log(line);
    // The CodeView GUID and age identify the exact matching PDB, including for
    // signed release binaries. No file hashing or module enumeration is needed.
    if (!image) return;
    const auto& directory = image->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    const auto size = image->OptionalHeader.SizeOfImage;
    if (!directory.VirtualAddress || directory.VirtualAddress >= size || directory.Size > size - directory.VirtualAddress) return;
    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + directory.VirtualAddress);
    for (unsigned i = 0; i < directory.Size / sizeof(*entries); ++i) {
        const auto& entry = entries[i];
        if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || entry.SizeOfData < 24 ||
            entry.AddressOfRawData >= size || entry.SizeOfData > size - entry.AddressOfRawData) continue;
        const auto* data = base + entry.AddressOfRawData;
        if (std::memcmp(data, "RSDS", 4)) continue;
        GUID guid{}; DWORD age{};
        std::memcpy(&guid, data + 4, sizeof(guid)); std::memcpy(&age, data + 20, sizeof(age));
        snprintf(line, sizeof(line), "STARTUP component=%s pdb_guid=%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X pdb_age=%lu",
            component, guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
            guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7], age);
        log(line);
        break;
    }
}
} // namespace cheeky::startup
