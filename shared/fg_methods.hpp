#pragma once
#include <cstdint>
// IDs follow the combined menu in both regions. Previous frame is periphery-only.
inline constexpr unsigned cheeky_fg_dlss = 0;
inline constexpr unsigned cheeky_fg_dlss_vectors_simple = 1;
inline constexpr unsigned cheeky_fg_dlss_vectors_bidirectional = 2;
inline constexpr unsigned cheeky_fg_dlss_vectors_confidence = 3;
inline constexpr unsigned cheeky_fg_optical_simple = 4;
inline constexpr unsigned cheeky_fg_optical_bidirectional = 5;
inline constexpr unsigned cheeky_fg_optical_flow = 6; // Confidence-guided reconstruction.
inline constexpr unsigned cheeky_fg_optical_simple_depth = 7;
inline constexpr unsigned cheeky_fg_optical_bidirectional_depth = 8;
inline constexpr unsigned cheeky_fg_optical_depth = 9;
inline constexpr unsigned cheeky_fg_previous = 10;
enum class CheekyFgVectorSource { dlss, optical };
enum class CheekyFgInterpolation { simple, bidirectional, confidence };
inline bool cheeky_fg_optical(unsigned m) noexcept {return m>=4&&m<=9;}
inline CheekyFgVectorSource cheeky_fg_vector_source(unsigned m) noexcept {
    return cheeky_fg_optical(m)?CheekyFgVectorSource::optical:CheekyFgVectorSource::dlss;
}
inline CheekyFgInterpolation cheeky_fg_interpolation(unsigned m) noexcept {
    return static_cast<CheekyFgInterpolation>(cheeky_fg_optical(m)?(m-4)%3:(m>=1&&m<=3?m-1:2));
}
inline bool cheeky_fg_depth(unsigned m) noexcept {return m==2||m==3||(m>=7&&m<=9);}
inline bool cheeky_fg_dlss_vectors(unsigned m) noexcept {return m>=1&&m<=3;}
inline bool cheeky_fg_shader(unsigned m) noexcept {return m>=1&&m<=9;}
inline bool cheeky_fg_depth_history(unsigned m) noexcept {return m==2||m==3;}
inline bool cheeky_fg_center_valid(unsigned m) noexcept {return m<=9;}
inline bool cheeky_fg_periphery_valid(unsigned m) noexcept {return m<=10;}
inline bool cheeky_fg_image_only(unsigned center,unsigned periphery,bool foveated) noexcept {
    return cheeky_fg_optical(center)&&!cheeky_fg_depth(center)&&
        (!foveated||periphery==cheeky_fg_previous||(cheeky_fg_optical(periphery)&&!cheeky_fg_depth(periphery)));
}
inline bool cheeky_fg_uses_flow(unsigned center,unsigned periphery,bool foveated) noexcept {
    return cheeky_fg_optical(center)||(foveated&&cheeky_fg_optical(periphery));
}
struct CheekyFgOpticalSettings {
    std::uint32_t center_preset{1},peripheral_preset{2}; // 0 slow, 1 medium, 2 fast.
    std::uint32_t center_percent{100},peripheral_percent{50}; // Input dimensions, independent of FG output scale.
    std::uint32_t center_grid{4},peripheral_grid{4}; // One vector per grid x grid flow-input pixels: 1, 2 or 4.
};
