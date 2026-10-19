#pragma once
#include <cstdint>

namespace cheeky::foveated_dlss {
// Switching foveated DLSS-SR ON/OFF can change the native color layout and
// NGX guide mapping without recreating the game's NGX-SR feature handles.
// This is a *presenter* configuration change, not a native pacing change:
// the OpenXR presenter must drain/reacquire its capture chain automatically.
constexpr std::uint64_t fg_sr_mode_configuration(std::uint64_t fg_settings, bool sr_foveation_enabled) noexcept {
    return (fg_settings ^ (sr_foveation_enabled ? 0xD1B54A32D192ED03ull : 0x94D049BB133111EBull))
        * 1099511628211ull;
}
// A game NGX-SR feature may be released/recreated when DLSS quality changes,
// even when the OpenXR color size and all FG Lua settings stay unchanged.
// Fold that lifecycle into the *guide* configuration, not the layer control
// configuration: there is no need to restart the OpenXR presenter.
constexpr std::uint64_t fg_source_configuration(std::uint64_t settings, std::uint64_t revision) noexcept {
    return (settings ^ (revision * 0x9e3779b97f4a7c15ull)) * 1099511628211ull;
}
constexpr bool fg_same_source_history(std::uint64_t previous, std::uint64_t current) noexcept {
    return previous != 0 && previous == current;
}
}
