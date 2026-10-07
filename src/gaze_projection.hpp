#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace cheeky::foveated_dlss {
struct GazeProjection {
    // Signed view-space tangents, independent of left/right handed depth.
    float left{}, right{}, up{}, down{};
    bool valid{};
};

inline bool projection_forward_center(const GazeProjection& p, float& u, float& v) {
    if (!p.valid || !std::isfinite(p.left) || !std::isfinite(p.right) ||
        !std::isfinite(p.up) || !std::isfinite(p.down) ||
        p.left >= 0.F || p.right <= 0.F || p.down >= 0.F || p.up <= 0.F) return false;
    u = -p.left / (p.right - p.left);
    v = p.up / (p.up - p.down);
    return std::isfinite(u) && std::isfinite(v);
}

inline GazeProjection gaze_projection_from_matrix(const float* m) {
    for (unsigned i = 0; i < 16; ++i) if (!std::isfinite(m[i])) return {};
    // Streamline uses row vectors and row-major, unjittered matrices.
    // Accept only conventional perspective projections, not arbitrary transforms.
    for (const auto i : {1, 2, 3, 4, 6, 7, 12, 13, 15})
        if (std::abs(m[i]) > 0.0001F) return {};
    if (m[0] <= 0 || m[5] <= 0 || std::abs(std::abs(m[11]) - 1.F) > 0.0001F) return {};
    const float sign = m[11];
    GazeProjection result{(-1.F - m[8] * sign) / m[0],
        (1.F - m[8] * sign) / m[0], (1.F - m[9] * sign) / m[5],
        (-1.F - m[9] * sign) / m[5], true};
    result.valid = result.left < 0 && result.right > 0 && result.down < 0 && result.up > 0 &&
        std::isfinite(result.left) && std::isfinite(result.right) &&
        std::isfinite(result.up) && std::isfinite(result.down);
    return result;
}

inline bool gaze_projection_matches(const GazeProjection& a, const GazeProjection& b) {
    if (!a.valid || !b.valid) return false;
    const auto close = [](float x, float y) {
        return std::isfinite(x) && std::isfinite(y) &&
            std::abs(x - y) <= 0.001F * (1.F + (std::max)(std::abs(x), std::abs(y)));
    };
    return close(a.left, b.left) && close(a.right, b.right) &&
        close(a.up, b.up) && close(a.down, b.down);
}

struct GazeProjectionMatch { unsigned count{}; unsigned index{UINT32_MAX}; };
inline GazeProjectionMatch match_gaze_projection_eyes(const GazeProjection& camera,
    const std::array<GazeProjection, 2>& eyes) {
    GazeProjectionMatch result{};
    if (!eyes[0].valid || !eyes[1].valid) return result;
    for (unsigned i = 0; i < 2; ++i) if (gaze_projection_matches(camera, eyes[i])) {
        ++result.count; result.index = i;
    }
    return result;
}

// One render can feed both eyes: ControlVR's alternate-eye mode draws a single
// frustum spanning both eye frusta and crops each eye from it, so the output
// has the union's aspect ratio and neither eye's own. Inputs are the published
// eye angles (radians, left/down negative) and the head forward in eye UV.
struct GazeEyeFrustum {
    float left{}, right{}, up{}, down{};
    float forward_u{}, forward_v{};
    bool forward_valid{};
};
struct BinocularUnionFrustum {
    std::array<float, 2> yaw{}, pitch{}; // Each eye's forward direction, removed to reach head space.
    float left{}, right{}, up{}, down{}; // Union tangents in head space.
    bool valid{};
};
// Angle of a UV coordinate across a planar projection spanning the two angles.
inline float gaze_uv_angle(float t, float first, float second) {
    const auto a = std::tan(first), b = std::tan(second);
    return std::atan(a + t * (b - a));
}
inline BinocularUnionFrustum binocular_union_frustum(const std::array<GazeEyeFrustum, 2>& eyes,
    std::uint32_t output_width, std::uint32_t output_height) {
    constexpr float limit = 1.55F; // Keep every bound short of a right angle.
    if (output_width == 0U || output_height == 0U) return {};
    const float output_aspect = static_cast<float>(output_width) / static_cast<float>(output_height);
    BinocularUnionFrustum frustum{};
    float left = limit, right = -limit, up = -limit, down = limit;
    for (unsigned i = 0; i < 2; ++i) {
        const auto& e = eyes[i];
        for (const auto value : {e.left, e.right, e.up, e.down, e.forward_u, e.forward_v})
            if (!std::isfinite(value)) return {};
        if (!(e.left < 0.F && e.right > 0.F && e.up > 0.F && e.down < 0.F) ||
            std::abs(e.left) >= limit || std::abs(e.right) >= limit || std::abs(e.up) >= limit || std::abs(e.down) >= limit)
            return {};
        // An eye's own frustum matching the output is an ordinary per-eye render.
        const float eye_aspect = (std::tan(e.right) - std::tan(e.left)) / (std::tan(e.up) - std::tan(e.down));
        if (std::abs(eye_aspect / output_aspect - 1.F) <= 0.02F) return {};
        frustum.yaw[i] = e.forward_valid ? gaze_uv_angle(e.forward_u, e.left, e.right) : 0.F;
        frustum.pitch[i] = e.forward_valid ? gaze_uv_angle(e.forward_v, e.up, e.down) : 0.F;
        left = (std::min)(left, e.left - frustum.yaw[i]);
        right = (std::max)(right, e.right - frustum.yaw[i]);
        up = (std::max)(up, e.up - frustum.pitch[i]);
        down = (std::min)(down, e.down - frustum.pitch[i]);
    }
    if (!(left > -limit && right < limit && up < limit && down > -limit && left < 0.F && right > 0.F && up > 0.F && down < 0.F))
        return {};
    frustum.left = std::tan(left); frustum.right = std::tan(right);
    frustum.up = std::tan(up); frustum.down = std::tan(down);
    const float union_aspect = (frustum.right - frustum.left) / (frustum.up - frustum.down);
    frustum.valid = std::abs(union_aspect / output_aspect - 1.F) <= 0.01F;
    return frustum;
}
// Maps a UV in one eye's image to the union output. Each eye renders from its
// own position, so a binocular gaze averages both eyes' mapped points.
inline bool map_eye_uv_to_union(const BinocularUnionFrustum& frustum, const GazeEyeFrustum& eye, unsigned index,
    float u, float v, float& union_u, float& union_v) {
    if (!frustum.valid || index > 1U || !std::isfinite(u) || !std::isfinite(v)) return false;
    const auto horizontal = std::tan(gaze_uv_angle(u, eye.left, eye.right) - frustum.yaw[index]);
    const auto vertical = std::tan(gaze_uv_angle(v, eye.up, eye.down) - frustum.pitch[index]);
    union_u = (horizontal - frustum.left) / (frustum.right - frustum.left);
    union_v = (frustum.up - vertical) / (frustum.up - frustum.down);
    return std::isfinite(union_u) && std::isfinite(union_v);
}

class GazeProjectionCache {
    struct Entry { std::uint32_t viewport; std::uintptr_t frame; std::uint64_t time;
        GazeProjection projection; };
    std::vector<Entry> entries_;
public:
    void record(std::uint32_t viewport, std::uintptr_t frame, std::uint64_t now, GazeProjection projection) {
        std::erase_if(entries_, [=](const auto& e) { return e.viewport == viewport; });
        if (entries_.size() >= 32) entries_.erase(entries_.begin());
        entries_.push_back({viewport, frame, now, projection});
    }
    GazeProjection find(std::uint32_t viewport, std::uintptr_t frame, std::uint64_t now) const {
        for (const auto& e : entries_)
            if (frame && e.viewport == viewport && e.frame == frame && now >= e.time && now - e.time <= 100)
                return e.projection;
        return {};
    }
    void forget(std::uint32_t viewport) {
        std::erase_if(entries_, [=](const auto& e) { return e.viewport == viewport; });
    }
};

struct GazeProjectionContext { std::uint64_t view{}; GazeProjection projection{}; };
inline thread_local GazeProjectionContext active_gaze_projection{};
class ScopedGazeProjection {
    GazeProjectionContext previous_;
public:
    ScopedGazeProjection(std::uint64_t view, GazeProjection projection) : previous_(active_gaze_projection) {
        active_gaze_projection = {view, projection};
    }
    ~ScopedGazeProjection() { active_gaze_projection = previous_; }
    ScopedGazeProjection(const ScopedGazeProjection&) = delete;
    ScopedGazeProjection& operator=(const ScopedGazeProjection&) = delete;
};
} // namespace cheeky::foveated_dlss
