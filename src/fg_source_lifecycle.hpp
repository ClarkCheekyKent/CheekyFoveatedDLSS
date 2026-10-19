#pragma once
#include <cstdint>

namespace cheeky::foveated_dlss {
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
