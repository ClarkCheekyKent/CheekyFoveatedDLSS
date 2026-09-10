#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <dxgiformat.h>

namespace cheeky::foveated_dlss {
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
// Markers only need exact zero/one values; no lossy float-to-half conversion.
inline void calibration_encode_marker(unsigned char* p, DXGI_FORMAT format, unsigned candidate) {
    if (format == DXGI_FORMAT_R11G11B10_FLOAT) {
        // Exact 1.0: exponent 15, zero mantissa; this format has no alpha.
        const std::uint32_t value = (candidate ? 0x3c0U << 11 : 0x3c0U) | (0x1e0U << 22);
        std::memcpy(p, &value, sizeof(value));
    } else if (format == DXGI_FORMAT_R32G32B32A32_FLOAT) {
        const CalibrationPixel value{candidate ? 0.0F : 1.0F, candidate ? 1.0F : 0.0F, 1, 1};
        std::memcpy(p, &value, 16);
    } else if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        const std::uint16_t value[]{std::uint16_t(candidate ? 0 : 0x3c00),
                                    std::uint16_t(candidate ? 0x3c00 : 0), 0x3c00, 0x3c00};
        std::memcpy(p, value, 8);
    } else if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        const std::uint32_t value = (candidate ? 1023U << 10 : 1023U) | (1023U << 20) | (3U << 30);
        std::memcpy(p, &value, 4);
    } else {
        const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                          format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
        p[bgra ? 2 : 0] = candidate ? 0 : 255;
        p[1] = candidate ? 255 : 0;
        p[bgra ? 0 : 2] = 255;
        p[3] = 255;
    }
}
inline float calibration_similarity(CalibrationPixel p, unsigned candidate) {
    if (!std::isfinite(p.r) || !std::isfinite(p.g) || !std::isfinite(p.b))
        return 0;
    const float peak = (std::max)({p.r, p.g, p.b});
    if (peak < 0.05F)
        return 0;
    const float contrast = candidate ? (std::min)(p.g, p.b) - p.r : (std::min)(p.r, p.b) - p.g;
    return std::clamp(contrast / peak, 0.0F, 1.0F);
}
inline int calibration_classify(float a, float b) {
    if (!std::isfinite(a) || !std::isfinite(b))
        return -1;
    if ((std::max)(a, b) < 0.55F || std::abs(a - b) < 0.25F)
        return -1;
    return a > b ? 0 : 1;
}
} // namespace cheeky::foveated_dlss
