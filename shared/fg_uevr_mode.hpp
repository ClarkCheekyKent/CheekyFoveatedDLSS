#pragma once
// Mode 0 is native stereo in UEVR's VR_RenderingMethod setting. Other modes
// cannot provide the simultaneous native stereo pair required by DLSS-FG.
// -2 means this is not a UEVR session (do not restrict other hosts).
// -1 means a UEVR session is attached but its rendering method is unknown.
#include <cstdint>
namespace cheeky::fg {
inline constexpr int no_uevr_host = -2;
inline constexpr int unknown_uevr_mode = -1;
[[nodiscard]] constexpr int uevr_mode_value(std::uint32_t mode) noexcept {
    return mode <= 3 ? static_cast<int>(mode) : unknown_uevr_mode;
}
[[nodiscard]] constexpr bool uevr_mode_supports_fg(int mode) noexcept {
    return mode == no_uevr_host || mode == 0;
}
[[nodiscard]] constexpr bool uevr_mode_pauses_fg(int mode) noexcept {
    return mode != no_uevr_host && mode != 0;
}
}
