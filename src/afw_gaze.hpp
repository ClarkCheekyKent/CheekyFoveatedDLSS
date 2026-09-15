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
// Fixed allocation budget from configuration/projection, independent of gaze.
inline unsigned afw_gaze_size(float fraction, unsigned extent, unsigned quantum) noexcept {
    quantum = std::clamp(quantum, 1U, 64U);
    const auto pixels = static_cast<unsigned>(std::ceil(std::clamp(fraction, .001F, 1.F) * extent));
    return (std::min)(extent, ((pixels + quantum - 1) / quantum) * quantum);
}
inline unsigned afw_gaze_start(float center, unsigned extent, unsigned size, unsigned quantum) noexcept {
    quantum = std::clamp(quantum, 1U, 64U);
    const auto maximum = extent - size;
    const auto desired = static_cast<unsigned>(std::lround(std::clamp(center * extent - size * .5F, 0.F, float(maximum))));
    return (std::min)(maximum, ((desired + quantum / 2) / quantum) * quantum);
}
// The same budget is used for either source eye, including asymmetric FOVs.
inline AfwMaskExtent afw_gaze_budget(const Settings& settings, const AfwStereoProjection& projection) noexcept {
    AfwMaskExtent budget{};
    for (unsigned source = 0; source < 2; ++source) {
        FoveationMask mask{};
        for (unsigned eye = 0; eye < 2; ++eye)
            afw_mask_include(mask, projection.centers[eye], settings.afw_gaze_width,
                settings.afw_gaze_height, settings.afw_warp_margin, &projection, eye,
                settings.afw_source_eye < 2 ? source : UINT32_MAX);
        const auto b = afw_mask_extent(mask, &projection, settings.afw_source_eye < 2 ? source : UINT32_MAX);
        budget.width = (std::max)(budget.width, b.width);
        budget.height = (std::max)(budget.height, b.height);
    }
    return budget;
}
}
