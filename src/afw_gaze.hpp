#pragma once
#include "afw_compatibility.hpp"
#include "cheeky_gaze_abi.h"
#include <cmath>

namespace cheeky::foveated_dlss {
// Re-express a runtime eye's gaze ray in UE's projection for that same eye.
// Runtime and UE can use different FOV cropping; do not copy normalized UVs
// across those projections or average the two eyes into a fictitious 3D ray.
inline bool afw_project_gaze(const CheekyGazeViewV1& view, const GazeProjection& target,
    float u, float v, FoveationCenter& result) noexcept {
    if (!(view.flags & CHEEKY_GAZE_VIEW_FOV_VALID) || !std::isfinite(u) || !std::isfinite(v) ||
        u < 0.F || u > 1.F || v < 0.F || v > 1.F) return false;
    for (float angle : {view.fov_left, view.fov_right, view.fov_up, view.fov_down})
        if (!std::isfinite(angle) || std::abs(angle) >= 1.5707F) return false;
    GazeProjection source{std::tan(view.fov_left), std::tan(view.fov_right),
        std::tan(view.fov_up), std::tan(view.fov_down), true};
    float unused_u{}, unused_v{};
    if (!projection_forward_center(source, unused_u, unused_v) ||
        !projection_forward_center(target, unused_u, unused_v)) return false;
    const float ray_x = source.left + u * (source.right - source.left);
    const float ray_y = source.up - v * (source.up - source.down);
    result = {(ray_x - target.left) / (target.right - target.left),
        (target.up - ray_y) / (target.up - target.down), 1U};
    if (!std::isfinite(result.u) || !std::isfinite(result.v)) return false;
    result.u = std::clamp(result.u, 0.F, 1.F); result.v = std::clamp(result.v, 0.F, 1.F);
    return true;
}

struct AfwGazeBounds {
    float left{1.F}, top{1.F}, right{}, bottom{};
    void include(FoveationCenter center, float width, float height) noexcept {
        const float x = std::clamp(center.u - width * .5F, 0.F, 1.F - width);
        const float y = std::clamp(center.v - height * .5F, 0.F, 1.F - height);
        left = (std::min)(left, x); top = (std::min)(top, y);
        right = (std::max)(right, x + width); bottom = (std::max)(bottom, y + height);
    }
    void pad(float margin) noexcept {
        left = (std::max)(0.F, left - margin); top = (std::max)(0.F, top - margin);
        right = (std::min)(1.F, right + margin); bottom = (std::min)(1.F, bottom + margin);
    }
};
// Grow only within one settings/session epoch. Gaze motion must not toggle
// private DLSS dimensions by a pixel and recreate its temporal history.
inline void afw_gaze_axis(float lower, float upper, unsigned extent, unsigned quantum,
    unsigned& retained_size, unsigned& start) noexcept {
    const auto lo = static_cast<unsigned>(std::floor(std::clamp(lower, 0.F, 1.F) * extent));
    const auto hi = static_cast<unsigned>(std::ceil(std::clamp(upper, 0.F, 1.F) * extent));
    quantum = std::clamp(quantum, 1U, 64U);
    retained_size = (std::min)(extent, (std::max)(retained_size, ((hi - lo + quantum - 1) / quantum) * quantum));
    const auto minimum = hi > retained_size ? hi - retained_size : 0U;
    const auto maximum = (std::min)(lo, extent - retained_size);
    const auto desired = (minimum + maximum) / 2;
    const auto aligned = ((desired + quantum / 2) / quantum) * quantum;
    start = std::clamp(aligned, minimum, maximum); // Coverage takes precedence over quantization at an edge.
}
}
