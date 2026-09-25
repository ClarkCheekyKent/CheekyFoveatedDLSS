#pragma once
#include <cstdint>

// Process-local ABI between the standalone DXGI host and the OpenXR API layer.
// texture is AddRef'd by acquire and must be released by the caller.
struct CheekyOpenXRMenuFrame {
    std::uint32_t size{sizeof(CheekyOpenXRMenuFrame)};
    std::uint32_t graphics_api{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t format{};
    float display_width{};
    float display_height{};
    void* device{};
    void* queue{};
    void* texture{};
};

using CheekyOpenXRMenuAcquire = bool(__cdecl*)(CheekyOpenXRMenuFrame*);
// On success returns an AddRef'd D3D11 texture and a keyed mutex held at key 1.
// The consumer releases key 0 after copying, then releases both COM references.
using CheekyOpenXRMenuShared11 = std::int32_t(__cdecl*)(void*, void*, void**, void**);
using CheekyOpenXRMenuPointer = void(__cdecl*)(float, float, bool, bool);
using CheekyOpenXRMenuCopy11 = bool(__cdecl*)(void*, void*);
using CheekyOpenXRMenuExecute11 = bool(__cdecl*)(void*, void*);
using CheekyOpenXRMenuSubmit12 = bool(__cdecl*)(void*, void*, void*, void*, std::uint64_t);
using CheekyOpenXRMenuReport = void(__cdecl*)(std::uint32_t);
using CheekyOpenXRMenuDiagnosticFn = void(__cdecl*)(const char*);

enum CheekyOpenXRMenuStatus : std::uint32_t {
    cheeky_xr_menu_submitted,
    cheeky_xr_menu_graphics_mismatch,
    cheeky_xr_menu_device_mismatch,
    cheeky_xr_menu_format_unavailable,
    cheeky_xr_menu_swapchain_failed,
    cheeky_xr_menu_tracking_unavailable,
    cheeky_xr_menu_image_failed,
    cheeky_xr_menu_copy_failed,
    cheeky_xr_menu_end_frame_rejected,
};
