#pragma once
#include <cstdint>

// Process-local, versioned bridge. No OpenXR or STL types cross the DLL boundary.
// The layer retains the provider module before caching these callbacks.
struct CheekyEyeCalibrationBridgeV1 {
    std::uint32_t size{sizeof(CheekyEyeCalibrationBridgeV1)};
    std::uint32_t version{1};
    bool(__cdecl* begin)(std::uint64_t session_generation, std::uint32_t graphics_api) noexcept {};
    std::uint64_t(__cdecl* capture)(std::uint64_t session_generation, void* texture, void* queue,
                                    std::uint32_t graphics_api, std::uint32_t eye, std::uint32_t array_slice,
                                    float u0, float v0, float u1, float v1) noexcept {};
    void(__cdecl* result)(std::uint64_t ticket, int result, std::uint32_t eye) noexcept {};
    void(__cdecl* destroy)(std::uint64_t session_generation) noexcept {};
};
using CheekyGetEyeCalibrationBridgeFn = bool(__cdecl*)(CheekyEyeCalibrationBridgeV1*) noexcept;
