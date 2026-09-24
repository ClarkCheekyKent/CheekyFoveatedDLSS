// Cheeky local adaptation of the pinned ImGui DX11/DX12 pixel shaders.
// 0 = SDR UNORM, 1 = scRGB (203-nit white), 2 = Rec.2020/PQ HDR10,
// 3 = SDR sRGB render target (hardware applies the sRGB transfer).
#pragma once
#include <string>
extern thread_local int cheeky_overlay_color_mode;
inline constexpr char cheeky_overlay_color_shader[] = R"(
float3 CheekyOverlayColor(float3 color) {
#if CHEEKY_COLOR_MODE == 0
    return color;
#else
    float3 linear_color = lerp(color / 12.92, pow((color + 0.055) / 1.055, 2.4), step(0.04045, color));
#if CHEEKY_COLOR_MODE == 1
    return linear_color * (203.0 / 80.0);
#elif CHEEKY_COLOR_MODE == 3
    return linear_color;
#else
    // Rec.709 -> Rec.2020, followed by ST.2084 at a 203 cd/m2 UI white.
    float3 rec2020 = mul(float3x3(
        0.627404, 0.329283, 0.043313,
        0.069097, 0.919540, 0.011362,
        0.016391, 0.088013, 0.895595), linear_color);
    float3 powered = pow(saturate(rec2020 * (203.0 / 10000.0)), 2610.0 / 16384.0);
    return pow((3424.0 / 4096.0 + (2413.0 / 128.0) * powered) /
        (1.0 + (2392.0 / 128.0) * powered), 2523.0 / 32.0);
#endif
#endif
}
)";
