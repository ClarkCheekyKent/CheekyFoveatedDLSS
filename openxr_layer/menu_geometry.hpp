#pragma once
#include "../third_party/openxr/include/openxr/openxr.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cheeky::xr_menu {
inline constexpr float radius = 3.F;
inline constexpr float max_angle = 2.09439510239F;

struct Strip {
    XrVector3f center{};
    float angle{}, width{};
    std::uint32_t left{}, right{};
};

inline Strip strip(float width, std::uint32_t pixels, unsigned index, unsigned count) {
    Strip s;
    s.left = pixels * index / count;
    s.right = pixels * (index + 1) / count;
    if (count == 1) { s.width = width; return s; }
    const float a = (float(s.left) / pixels - .5F) * width / radius;
    const float b = (float(s.right) / pixels - .5F) * width / radius;
    s.angle = (a + b) * .5F;
    s.center = {radius * (std::sin(a) + std::sin(b)) * .5F, 0,
        radius * (1.F - (std::cos(a) + std::cos(b)) * .5F)};
    s.width = 2.F * radius * std::sin((b - a) * .5F);
    return s;
}

// Coordinates are relative to the upright stage's center, with +Z toward the viewer.
inline bool hit(XrVector3f o, XrVector3f d, float width, float height,
    std::uint32_t pixels, unsigned strips, bool cylinder, float& u, float& v) {
    if (cylinder) {
        const float z = o.z - radius;
        const float a = d.x * d.x + d.z * d.z;
        const float b = o.x * d.x + z * d.z;
        const float c = o.x * o.x + z * z - radius * radius;
        const float disc = b * b - a * c;
        if (a < 1e-8F || disc < 0) return false;
        // The visible surface faces inward: choose the exit from the cylinder.
        const float t = (-b + std::sqrt(disc)) / a;
        if (t <= 0 || t > 10) return false;
        const float angle = std::atan2(o.x + t * d.x, radius - (o.z + t * d.z));
        const float y = o.y + t * d.y;
        if (std::abs(angle) > width / (2.F * radius) || std::abs(y) > height * .5F) return false;
        u = .5F + angle * radius / width;
        v = .5F - y / height;
        return true;
    }
    float nearest = 11.F;
    bool found = false;
    for (unsigned i = 0; i < strips; ++i) {
        const auto s = strip(width, pixels, i, strips);
        const float sn = std::sin(s.angle), cs = std::cos(s.angle);
        const float denom = -sn * d.x + cs * d.z;
        if (denom >= -1e-5F) continue;
        const float t = (-sn * (s.center.x - o.x) + cs * (s.center.z - o.z)) / denom;
        if (t <= 0 || t > 10 || t >= nearest) continue;
        const float x = cs * (o.x + t * d.x - s.center.x) + sn * (o.z + t * d.z - s.center.z);
        const float y = o.y + t * d.y;
        if (std::abs(x) > s.width * .5F || std::abs(y) > height * .5F) continue;
        u = (s.left + (x / s.width + .5F) * (s.right - s.left)) / pixels;
        v = .5F - y / height;
        nearest = t;
        found = true;
    }
    return found;
}
}
