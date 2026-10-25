#pragma once
#include <cstdint>
// Persisted method values: periphery 0 = previous, 3 = DLSS; center 0 = DLSS.
inline constexpr unsigned cheeky_fg_optical_flow = 4;
// Optical flow motion; captured DLSS depth only arbitrates occlusions.
inline constexpr unsigned cheeky_fg_optical_depth = 5;
inline bool cheeky_fg_optical(unsigned m) noexcept {return m==cheeky_fg_optical_flow||m==cheeky_fg_optical_depth;}
inline bool cheeky_fg_center_valid(unsigned m) noexcept {return m<=2||cheeky_fg_optical(m);}
inline bool cheeky_fg_periphery_valid(unsigned m) noexcept {return m<=5;}
inline bool cheeky_fg_image_only(unsigned center,unsigned periphery,bool foveated) noexcept {
    return center==4&&(!foveated||periphery==0||periphery==4);
}
inline bool cheeky_fg_uses_flow(unsigned center,unsigned periphery,bool foveated) noexcept {
    return cheeky_fg_optical(center)||(foveated&&cheeky_fg_optical(periphery));
}
struct CheekyFgOpticalSettings {
    std::uint32_t center_preset{1},peripheral_preset{2}; // 0 slow, 1 medium, 2 fast.
    std::uint32_t center_percent{100},peripheral_percent{50}; // Input dimensions, independent of FG output scale.
    std::uint32_t center_grid{4},peripheral_grid{4}; // One vector per grid x grid flow-input pixels: 1, 2 or 4.
};
