#pragma once
#include <cstdint>
// Persisted method values: periphery 0 = previous, 3 = DLSS; center 0 = DLSS.
inline constexpr unsigned cheeky_fg_optical_flow = 4;
inline bool cheeky_fg_center_valid(unsigned m) noexcept {return m<=2||m==4;}
inline bool cheeky_fg_periphery_valid(unsigned m) noexcept {return m<=4;}
inline bool cheeky_fg_image_only(unsigned center,unsigned periphery,bool foveated) noexcept {
    return center==4&&(!foveated||periphery==0||periphery==4);
}
inline bool cheeky_fg_uses_flow(unsigned center,unsigned periphery,bool foveated) noexcept {
    return center==4||(foveated&&periphery==4);
}
struct CheekyFgOpticalSettings {
    std::uint32_t center_preset{1},peripheral_preset{2}; // 0 slow, 1 medium, 2 fast.
    std::uint32_t center_percent{100},peripheral_percent{50}; // Input dimensions, independent of FG output scale.
};
