#pragma once
#include "eye_calibration_bridge.h"
#include <Windows.h>
#include <array>
#include <cstdint>

namespace cheeky::openxr_calibration {
// Called under the layer's state mutex. Retain the exporting module: a cached
// callback must survive a host adapter unload. Disabled cores do no GPU work.
inline const CheekyEyeCalibrationBridgeV1* bridge() noexcept {
    static CheekyEyeCalibrationBridgeV1 api;
    static HMODULE retained{};
    if (retained)
        return &api;
    for (const wchar_t* name : std::array<const wchar_t*, 3>{L"CheekyFoveatedDLSSRuntime.dll",
                                                             L"CheekyFoveatedDLSS.addon64", nullptr}) {
        const auto module = GetModuleHandleW(name);
        if (!module)
            continue;
        const auto get = reinterpret_cast<CheekyGetEyeCalibrationBridgeFn>(
            GetProcAddress(module, "CheekyEyeCalibration_GetBridge"));
        CheekyEyeCalibrationBridgeV1 candidate;
        if (!get || !get(&candidate) || !candidate.begin || !candidate.capture || !candidate.result ||
            !candidate.destroy)
            continue;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(get),
                                &retained))
            continue;
        api = candidate;
        return &api;
    }
    return nullptr;
}

struct Region {
    std::uint64_t swapchain{};
    std::uint32_t slice{};
    std::int32_t x{}, y{}, width{}, height{};
    bool operator==(const Region&) const = default;
    bool valid() const noexcept {
        return swapchain && x >= 0 && y >= 0 && width > 0 && height > 0;
    }
};

// Copy only small patches before Release. EndFrame supplies the physical eye
// labels and confirms which released image was actually used. If layouts change
// beyond a permutation, reject this frame and learn the new rectangles.
struct Frame {
    std::array<Region, 2> history{}, captured{};
    std::array<std::uint64_t, 2> tickets{};
    std::array<std::uint32_t, 2> indices{};
    std::array<bool, 2> released{};
    std::uint64_t generation{};
    std::uint32_t graphics_api{};
    bool active{};

    void begin(const CheekyEyeCalibrationBridgeV1* api, std::uint64_t gen, std::uint32_t graphics) noexcept {
        generation = gen;
        graphics_api = graphics;
        tickets = {};
        released = {};
        captured = {};
        active = api && (graphics == 11 || graphics == 12) && api->begin(gen, graphics);
    }
    void before_release(const CheekyEyeCalibrationBridgeV1* api, std::uint64_t swapchain, std::uint32_t index,
                        void* texture, void* queue, std::uint32_t width, std::uint32_t height) noexcept {
        if (!active || !api || !texture || !width || !height)
            return;
        for (unsigned slot = 0; slot < 2; ++slot) {
            const auto& r = history[slot];
            if (!r.valid() || r.swapchain != swapchain || std::uint64_t(r.x) + r.width > width ||
                std::uint64_t(r.y) + r.height > height)
                continue;
            captured[slot] = r;
            indices[slot] = index;
            released[slot] = false;
            tickets[slot] = api->capture(generation, texture, queue, graphics_api, slot, r.slice,
                                         float(r.x) / width, float(r.y) / height,
                                         float(r.x + r.width) / width, float(r.y + r.height) / height);
        }
    }
    void after_release(std::uint64_t swapchain, std::uint32_t index, bool success) noexcept {
        for (unsigned slot = 0; slot < 2; ++slot)
            if (captured[slot].swapchain == swapchain && indices[slot] == index)
                released[slot] = success;
    }
    void end(const CheekyEyeCalibrationBridgeV1* api, const std::array<Region, 2>& views,
             const std::array<std::uint32_t, 2>& last_released, bool success) noexcept {
        bool valid = success && views[0].valid() && views[1].valid() && views[0] != views[1];
        std::array<unsigned, 2> eyes{{2, 2}};
        for (unsigned slot = 0; slot < 2; ++slot) {
            for (unsigned eye = 0; eye < 2; ++eye)
                if (captured[slot] == views[eye] && indices[slot] == last_released[eye])
                    eyes[slot] = eye;
            valid = valid && tickets[slot] && released[slot] && eyes[slot] < 2;
        }
        valid = valid && eyes[0] != eyes[1];
        if (active && api)
            for (unsigned slot = 0; slot < 2; ++slot)
                api->result(tickets[slot], valid ? 0 : -1, eyes[slot]);
        // Never retain geometry from a failed or ambiguous submission.
        history = success ? views : std::array<Region, 2>{};
        active = false;
    }
    void destroy(const CheekyEyeCalibrationBridgeV1* api) noexcept {
        if (api && generation)
            api->destroy(generation);
        *this = {};
    }
};
} // namespace cheeky::openxr_calibration
