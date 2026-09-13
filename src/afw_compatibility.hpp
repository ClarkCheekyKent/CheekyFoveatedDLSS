#pragma once
#include "settings.hpp"
#include <algorithm>

namespace cheeky::foveated_dlss {
// A deliberately generous, eye-independent region for the routing experiment.
// Apply to evaluation-local copies only; never rewrite the user's preferences.
inline Settings afw_experiment_settings(Settings settings) noexcept {
    settings.width = (std::max)(settings.width, 0.70F);
    settings.height = (std::max)(settings.height, 0.70F);
    settings.x_offset = settings.height_offset = settings.aligned_height_offset = 0.F;
    settings.auto_stereo_alignment = settings.invert_stereo_x_offset = false;
    settings.center_mode = FoveationCenterMode::fixed;
    settings.center_supersampling = 1.F;
    settings.next_jump_visible = false;
    settings.nr_enabled = false;
    return settings;
}
}
