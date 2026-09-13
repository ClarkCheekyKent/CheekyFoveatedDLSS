#pragma once
#include "settings.hpp"
#include <algorithm>
#include <cmath>

namespace cheeky::foveated_dlss {
// Apply only to evaluation-local copies; never rewrite saved preferences.
// Manual coverage bounds BOTH possible mirrored horizontal crops. This is an
// uncertainty envelope, not a claim that opposite-eye UVs correspond in 3D.
// Padding is a user-controlled heuristic for warp donors/disocclusion, not a
// reprojection guarantee. No DLSS-handle or evaluation-parity eye guesses.
inline Settings afw_experiment_settings(Settings settings) noexcept {
    const auto finite = [](float value, float fallback, float lo, float hi) {
        return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
    };
    const float width = finite(settings.width, .7F, .2F, 1.F);
    const float height = finite(settings.height, .7F, .2F, 1.F);
    if (settings.afw_manual_coverage) {
        const float margin = finite(settings.afw_warp_margin, .05F, 0.F, .25F);
        const float x = finite(settings.x_offset, 0.F, -1.F, 1.F);
        const float y = finite(settings.height_offset, 0.F, -1.F, 1.F);
        settings.width = (std::min)(1.F, width + std::abs(x) * (1.F - width) + 2.F * margin);
        // Clip padding at the image boundary; retain the union's true center.
        const float top = (std::max)(0.F, (1.F - height) * (y + 1.F) * .5F - margin);
        const float bottom = (std::min)(1.F, (1.F - height) * (y + 1.F) * .5F + height + margin);
        settings.height = bottom - top;
        settings.height_offset = settings.height < 1.F
            ? std::clamp(2.F * top / (1.F - settings.height) - 1.F, -1.F, 1.F) : 0.F;
        // Rounding the bounding rectangle would remove covered corner pixels.
        settings.roundness = 0.F;
    } else {
        settings.width = (std::max)(width, .70F);
        settings.height = (std::max)(height, .70F);
        settings.height_offset = 0.F;
    }
    settings.x_offset = settings.aligned_height_offset = 0.F;
    settings.auto_stereo_alignment = settings.invert_stereo_x_offset = false;
    settings.center_mode = FoveationCenterMode::fixed;
    settings.center_supersampling = finite(settings.center_supersampling, 1.F, 1.F, 2.F);
    settings.next_jump_visible = false;
    settings.nr_enabled = false;
    return settings;
}
}
