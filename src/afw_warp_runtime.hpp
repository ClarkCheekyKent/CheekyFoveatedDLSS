#pragma once
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#pragma comment(lib, "bcrypt.lib")

namespace cheeky::foveated_dlss {
// Both official beta 6 release variants ship this exact DLL. An unfamiliar
// build retains opaque call observation and SR routing, without layout reads.
inline bool known_afw_warp_file(const wchar_t* path) noexcept {
    if (!path) return false;
    const auto file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart != 565248) { CloseHandle(file); return false; }
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::array<unsigned char, 8192> bytes{};
    DWORD read{};
    while (ok) {
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) { ok = false; break; }
        if (!read) break;
        ok = BCryptHashData(hash, bytes.data(), read, 0) >= 0;
    }
    std::array<unsigned char, 32> digest{};
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    constexpr std::array<unsigned char, 32> expected{
        0x5f,0x3c,0xfc,0x38,0x90,0x3a,0xc4,0x38,0x24,0x1a,0x3c,0x47,0xcb,0x4b,0xc3,0xac,
        0x32,0x9c,0xb3,0x9b,0x05,0x6e,0x33,0xa6,0x3c,0x20,0x04,0x5f,0xe1,0x53,0x36,0xf9};
    return ok && digest == expected;
}
inline bool known_afw_warp_runtime(HMODULE module) noexcept {
    if (!module) return false;
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    return length && length < path.size() && known_afw_warp_file(path.data());
}
}
