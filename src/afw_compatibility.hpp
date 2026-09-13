#pragma once
#include "settings.hpp"
#include "gaze_projection.hpp"
#include <algorithm>
#include <cmath>

namespace cheeky::foveated_dlss {
struct AfwStereoProjection {
    bool valid{};
    unsigned output_width{}, output_height{};
    std::array<FoveationCenter, 2> centers{};
    std::array<GazeProjection, 2> projections{};
    std::uint64_t generation{};
    std::uint64_t age_ms{UINT64_MAX};
};
class AfwProjectionCache {
    AfwStereoProjection value_{};
    std::uint64_t published_{};
public:
    void publish(const float (&matrices)[2][16], unsigned width, unsigned height, bool active, std::uint64_t now) noexcept {
        value_ = {}; published_ = now;
        if (!active || width < 32 || height < 32 || width > 16384 || height > 16384) return;
        for (unsigned eye = 0; eye < 2; ++eye) {
            value_.projections[eye] = gaze_projection_from_matrix(matrices[eye]);
            if (!projection_forward_center(value_.projections[eye],
                    value_.centers[eye].u, value_.centers[eye].v)) return;
        }
        value_.output_width = width; value_.output_height = height; value_.valid = true;
    }
    AfwStereoProjection snapshot(std::uint64_t now) const noexcept {
        auto value = value_;
        value.age_ms = now >= published_ ? now - published_ : UINT64_MAX;
        value.valid &= value.age_ms <= 250;
        return value;
    }
};
// Copied projection data only. No callback into the unloadable host adapter.
void publish_afw_stereo_projection(const float (&matrices)[2][16], unsigned width,
    unsigned height, bool active) noexcept;
void allow_afw_stereo_projection(bool allowed) noexcept;
[[nodiscard]] AfwStereoProjection afw_stereo_projection() noexcept;
struct AfwCoverageStatus {
    bool observed{};
    unsigned mode{}; // 0 centered, 1 manual union, 2 automatic union, 3 bilateral gaze
    float width{.7F}, height{.7F}, x_offset{}, height_offset{}, center_scale{1.F};
};
void note_afw_coverage(const Settings& settings, bool automatic_applied, bool gaze_applied = false) noexcept;
[[nodiscard]] AfwCoverageStatus afw_coverage_status() noexcept;

inline bool afw_projection_matches_output(const AfwStereoProjection& projection,
    unsigned width, unsigned height, unsigned x = 0, unsigned y = 0) noexcept {
    // Offsets locate a complete eye in a larger allocation. They do not change
    // its projection; the backend independently validates the resource bounds.
    return projection.valid && width == projection.output_width && height == projection.output_height &&
        static_cast<std::uint64_t>(x) + width <= 16384 && static_cast<std::uint64_t>(y) + height <= 16384;
}
inline void afw_mask_include(FoveationMask& mask, FoveationCenter center, float width, float height, float margin) noexcept {
    if (mask.count >= 4) return;
    const float x = std::clamp(center.u - width * .5F, 0.F, 1.F - width);
    const float y = std::clamp(center.v - height * .5F, 0.F, 1.F - height);
    auto& bounds = mask.bounds[mask.count++];
    // Retain the un-clipped shape at screen edges, avoiding squeezed ellipses.
    bounds[0] = x - margin; bounds[1] = y - margin;
    bounds[2] = x + width + margin; bounds[3] = y + height + margin;
}
// Apply only to evaluation-local copies; never rewrite saved preferences.
// Manual coverage bounds BOTH possible mirrored horizontal crops. This is an
// uncertainty envelope, not a claim that opposite-eye UVs correspond in 3D.
// Padding is a user-controlled heuristic for warp donors/disocclusion, not a
// reprojection guarantee. No DLSS-handle or evaluation-parity eye guesses.
inline Settings afw_coverage_settings(Settings settings, const AfwStereoProjection* projection = nullptr) noexcept {
    const auto finite = [](float value, float fallback, float lo, float hi) {
        return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
    };
    const float width = finite(settings.width, .7F, .2F, 1.F);
    const float height = finite(settings.height, .7F, .2F, 1.F);
    settings.afw_gaze_width = width; settings.afw_gaze_height = height;
    settings.afw_warp_margin = finite(settings.afw_warp_margin, .05F, 0.F, .25F);
    const float depth_margin = settings.afw_depth_coverage ? finite(settings.afw_depth_margin, 0.F, 0.F, 1.F) : 0.F;
    settings.afw_warp_margin = (std::min)(1.F, settings.afw_warp_margin + depth_margin);
    const float manual_x = settings.x_offset;
    settings.afw_mask = {};
    settings.x_offset = 0.F;
    if (settings.afw_automatic_coverage && projection && projection->valid) {
        const float margin = settings.afw_warp_margin;
        const float bias = .5F * finite(settings.aligned_height_offset, 0.F, -1.F, 1.F);
        float left = 1.F, top = 1.F, right = 0.F, bottom = 0.F;
        for (const auto& center : projection->centers) {
            afw_mask_include(settings.afw_mask, {center.u, center.v + bias, 1}, width, height, margin);
            const float x = std::clamp(center.u - width * .5F, 0.F, 1.F - width);
            const float y = std::clamp(center.v + bias - height * .5F, 0.F, 1.F - height);
            left = (std::min)(left, x); top = (std::min)(top, y);
            right = (std::max)(right, x + width); bottom = (std::max)(bottom, y + height);
        }
        left = (std::max)(0.F, left - margin); top = (std::max)(0.F, top - margin);
        right = (std::min)(1.F, right + margin); bottom = (std::min)(1.F, bottom + margin);
        settings.width = right - left; settings.height = bottom - top;
        settings.x_offset = settings.width < 1.F ? std::clamp(2.F * left / (1.F - settings.width) - 1.F, -1.F, 1.F) : 0.F;
        settings.height_offset = settings.height < 1.F ? std::clamp(2.F * top / (1.F - settings.height) - 1.F, -1.F, 1.F) : 0.F;
    } else if (!settings.afw_automatic_coverage && settings.afw_manual_coverage) {
        const float margin = settings.afw_warp_margin;
        const float x = finite(manual_x, 0.F, -1.F, 1.F);
        const float y = finite(settings.height_offset, 0.F, -1.F, 1.F);
        for (const float sign : {-1.F, 1.F})
            afw_mask_include(settings.afw_mask, {.5F + sign * x * (1.F - width) * .5F,
                .5F + y * (1.F - height) * .5F, 1}, width, height, margin);
        settings.width = (std::min)(1.F, width + std::abs(x) * (1.F - width) + 2.F * margin);
        // Clip padding at the image boundary; retain the union's true center.
        const float top = (std::max)(0.F, (1.F - height) * (y + 1.F) * .5F - margin);
        const float bottom = (std::min)(1.F, (1.F - height) * (y + 1.F) * .5F + height + margin);
        settings.height = bottom - top;
        settings.height_offset = settings.height < 1.F
            ? std::clamp(2.F * top / (1.F - settings.height) - 1.F, -1.F, 1.F) : 0.F;
    } else {
        settings.width = (std::min)(1.F, (std::max)(width, .70F) + 2.F * depth_margin);
        settings.height = (std::min)(1.F, (std::max)(height, .70F) + 2.F * depth_margin);
        settings.height_offset = 0.F;
    }
    settings.aligned_height_offset = 0.F;
    settings.eye_independent_coverage = true;
    settings.auto_stereo_alignment = settings.invert_stereo_x_offset = false;
    settings.center_supersampling = finite(settings.center_supersampling, 1.F, 1.F, 2.F);
    settings.next_jump_visible = false;
    return settings;
}
inline Settings afw_experiment_settings(Settings settings, const AfwStereoProjection* projection = nullptr) noexcept {
    auto nr = settings;
    if (!settings.nr_use_sr_foveation) { nr.width = settings.nr_width; nr.height = settings.nr_height; }
    nr = afw_coverage_settings(nr, projection);
    settings = afw_coverage_settings(settings, projection);
    settings.afw_nr = {nr.width, nr.height, nr.x_offset, nr.height_offset, nr.afw_gaze_width, nr.afw_gaze_height};
    settings.afw_nr_mask = nr.afw_mask;
    return settings;
}
inline std::uint64_t afw_nr_gaze_view(std::uint64_t view) noexcept {
    // Windows native NGX handles do not occupy this private coordinator namespace.
    return view ^ 0x4000000000000000ULL;
}
}
