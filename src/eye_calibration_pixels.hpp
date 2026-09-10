#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <dxgiformat.h>

namespace cheeky::foveated_dlss {
inline constexpr unsigned calibration_marker_size = 40;
inline constexpr unsigned calibration_sample_size = 60;
inline constexpr int calibration_sample_margin =
    (int(calibration_marker_size) - int(calibration_sample_size)) / 2;
inline constexpr float calibration_pattern_min_score = .90F;
inline constexpr float calibration_pattern_min_gap = .15F;
static_assert(calibration_marker_size == 40 && calibration_sample_size == 60,
              "The 5x5 pattern and bounded search use source-pixel coordinates");
struct CalibrationPixel {
    float r{}, g{}, b{}, a{1};
};
inline unsigned calibration_pixel_bytes(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 16;
    default:
        return 0;
    }
}
inline float calibration_half(std::uint16_t h) {
    const int exponent = (h >> 10) & 31;
    const int mantissa = h & 1023;
    const float value = exponent == 0    ? std::ldexp(float(mantissa), -24)
                        : exponent == 31 ? (mantissa ? std::numeric_limits<float>::quiet_NaN()
                                                     : std::numeric_limits<float>::infinity())
                                         : std::ldexp(1.0F + mantissa / 1024.0F, exponent - 15);
    return h & 0x8000 ? -value : value;
}
inline CalibrationPixel calibration_decode(const unsigned char* p, DXGI_FORMAT format) {
    if (format == DXGI_FORMAT_R11G11B10_FLOAT) {
        std::uint32_t n;
        std::memcpy(&n, p, sizeof(n));
        // Unsigned 5-bit exponents use the same bias as half-float. Align
        // the 6/6/5-bit mantissas to half's 10 bits, retaining NaN/Inf.
        return {calibration_half(std::uint16_t((n & 0x7ffU) << 4)),
                calibration_half(std::uint16_t(((n >> 11) & 0x7ffU) << 4)),
                calibration_half(std::uint16_t((n >> 22) << 5)), 1};
    }
    if (format == DXGI_FORMAT_R32G32B32A32_FLOAT) {
        CalibrationPixel result;
        std::memcpy(&result, p, sizeof(result));
        return result;
    }
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        std::uint16_t h[4];
        std::memcpy(h, p, sizeof(h));
        return {calibration_half(h[0]), calibration_half(h[1]), calibration_half(h[2]),
                calibration_half(h[3])};
    }
    if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        std::uint32_t n;
        std::memcpy(&n, p, 4);
        return {float(n & 1023) / 1023, float((n >> 10) & 1023) / 1023, float((n >> 20) & 1023) / 1023,
                float(n >> 30) / 3};
    }
    const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                      format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    return {p[bgra ? 2 : 0] / 255.0F, p[1] / 255.0F, p[bgra ? 0 : 2] / 255.0F, p[3] / 255.0F};
}
// Balanced, asymmetric 5x5 codes. Each cell is 8x8 source pixels.
inline bool calibration_pattern_bit(unsigned candidate, unsigned x, unsigned y) {
    constexpr const char* codes[]{
        "11010" "00101" "11000" "10101" "00110",
        "10111" "00100" "00110" "11001" "01001"};
    return codes[candidate][y * 5 + x] == '1';
}
inline void calibration_encode_pattern(unsigned char* p, DXGI_FORMAT format, unsigned candidate,
                                       unsigned x, unsigned y) {
    const bool light = calibration_pattern_bit(candidate, x / 8, y / 8);
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        const auto v = std::uint16_t(light ? 0x3c00 : 0);
        const std::uint16_t values[]{v, v, v, 0x3c00};
        std::memcpy(p, values, 8);
    } else if (format == DXGI_FORMAT_R32G32B32A32_FLOAT) {
        const float v = light ? 1.F : 0.F;
        const CalibrationPixel value{v, v, v, 1};
        std::memcpy(p, &value, 16);
    } else if (format == DXGI_FORMAT_R11G11B10_FLOAT) {
        const std::uint32_t v = light ? 0x3c0U | (0x3c0U << 11) | (0x1e0U << 22) : 0;
        std::memcpy(p, &v, 4);
    } else if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        const std::uint32_t v = (light ? 0x3fffffffU : 0) | (3U << 30);
        std::memcpy(p, &v, 4);
    } else {
        p[0] = p[1] = p[2] = light ? 255 : 0;
        p[3] = 255;
    }
}
// Normalize only the tiny readback, then search +/-8 source pixels in 2px steps.
// Mirrored templates tolerate reversed bounds; the marker's corner determines
// the submitted image orientation using the existing top/bottom checks.
inline float calibration_pattern_score(const void* data, unsigned pitch, unsigned width,
                                        unsigned height, DXGI_FORMAT format, unsigned candidate,
                                        bool submitted) {
    if (!data || !width || !height || candidate > 1 || !calibration_pixel_bytes(format)) return 0;
    const unsigned side = submitted ? calibration_sample_size : calibration_marker_size;
    std::array<float, calibration_sample_size * calibration_sample_size> luma{};
    const auto bytes = calibration_pixel_bytes(format);
    for (unsigned y = 0; y < side; ++y)
        for (unsigned x = 0; x < side; ++x) {
            const auto px = (std::min)(width - 1, unsigned((x + .5F) * width / side));
            const auto py = (std::min)(height - 1, unsigned((y + .5F) * height / side));
            const auto p = calibration_decode(static_cast<const unsigned char*>(data) + py * pitch + px * bytes, format);
            if (!std::isfinite(p.r) || !std::isfinite(p.g) || !std::isfinite(p.b)) return 0;
            luma[y * side + x] = (p.r + p.g + p.b) / 3.F;
        }
    float best{};
    const int origin = submitted ? 10 : 0, radius = submitted ? 8 : 0;
    for (unsigned mirror = 0; mirror < (submitted ? 4U : 1U); ++mirror)
        for (int dy = -radius; dy <= radius; dy += 2)
            for (int dx = -radius; dx <= radius; dx += 2) {
                std::array<float, 25> values{}, signs{};
                float sum{}, square{}, dot{}, sign_sum{}, high{}, low{};
                unsigned highs{}, lows{};
                for (unsigned y = 0; y < 5; ++y)
                    for (unsigned x = 0; x < 5; ++x) {
                        const int cx = origin + dx + int(x * 8) + 4;
                        const int cy = origin + dy + int(y * 8) + 4;
                        const float v = (luma[(cy - 1) * side + cx - 1] + luma[(cy - 1) * side + cx + 1] +
                                         luma[(cy + 1) * side + cx - 1] + luma[(cy + 1) * side + cx + 1]) * .25F;
                        const bool light = calibration_pattern_bit(candidate, mirror & 1 ? 4 - x : x,
                                                                    mirror & 2 ? 4 - y : y);
                        const float sign = light ? 1.F : -1.F;
                        values[y * 5 + x] = v; signs[y * 5 + x] = sign;
                        sum += v; square += v * v; dot += v * sign; sign_sum += sign;
                        if (light) { high += v; ++highs; } else { low += v; ++lows; }
                    }
                high /= highs; low /= lows;
                if (high - low < .04F) continue; // Flat/clipped patches carry no usable code.
                const float variance = square - sum * sum / 25.F;
                if (variance <= 1e-6F) continue;
                unsigned correct{};
                for (unsigned i = 0; i < 25; ++i)
                    correct += (values[i] - (high + low) * .5F) * signs[i] > 0;
                if (correct < 24) continue;
                const float score = (dot - sum * sign_sum / 25.F) /
                    std::sqrt(variance * (25.F - sign_sum * sign_sum / 25.F));
                best = (std::max)(best, std::clamp(score, 0.F, 1.F));
            }
    return best;
}
inline int calibration_pattern_classify(float a, float b) {
    if (!std::isfinite(a) || !std::isfinite(b) || (std::max)(a, b) < calibration_pattern_min_score ||
        std::abs(a - b) < calibration_pattern_min_gap) return -1;
    return a > b ? 0 : 1;
}
} // namespace cheeky::foveated_dlss
