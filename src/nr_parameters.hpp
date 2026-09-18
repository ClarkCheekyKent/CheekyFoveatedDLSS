#pragma once
#include "ngx_abi.hpp"
#include "dlss_nr_contract.hpp"
#include <algorithm>
#include <cstring>
#include <atomic>
namespace cheeky::foveated_dlss {
inline std::atomic<std::uint64_t> requested_nr_reset_generation{};
inline void set_model_tuning(NgxParameters* parameters, const Settings& settings) noexcept {
    // Match the reference forwarder's creation-time tuning contract. Keep these
    // in FeatureKey as well, without depending on live retuning support.
    parameters->Set("DLSSNR.Hint.Render.Preset", settings.nr_preset);
    parameters->Set("DLSSNR.Intensity", settings.nr_intensity);
    parameters->Set("DLSSNR.LocalToneStrength", settings.nr_local_tone_strength);
    parameters->Set("DLSSNR.LocalStructureStrength", settings.nr_local_structure_strength);
    parameters->Set("DLSSNR.SkinStructureStrength", settings.nr_skin_structure_strength);
    // The driver distinguishes integer values from pointer values even on x64.
    parameters->Set("DLSSNR.ControlMask", static_cast<void*>(nullptr));
    parameters->Set("DLSSNR.UseAutoMask", settings.nr_automatic_mask ? 1U : 0U);
    parameters->Set("DLSSNR.Style", settings.nr_style);
    parameters->Set("DLSSNR.UICorrection", settings.nr_ui_correction ? 1U : 0U);
}

inline NgxResult neural_scaling_ratio_callback(NgxParameters* const parameters) noexcept {
    if (parameters == nullptr) return 0xBAD00005U;
    float scale{1.0F};
    if (!ngx_succeeded(parameters->Get("DLSSNR.Scale", &scale))) scale = 1.0F;
    parameters->Set("DLSSNR.ScalingRatio", std::clamp(scale, 0.1F, 1.0F));
    return 1U;
}

[[nodiscard]] inline std::uint64_t nr_settings_signature(
    const Settings& settings,
    const NrRegion& region
) noexcept {
    std::uint64_t signature = 1469598103934665603ULL;
    const auto append = [&signature](const std::uint32_t value) noexcept {
        signature ^= value;
        signature *= 1099511628211ULL;
    };
    append(static_cast<std::uint32_t>(settings.nr_processing_order));
    append(settings.nr_style);
    for (const auto value : {
            settings.nr_working_scale,
            settings.nr_intensity,
            settings.nr_local_tone_strength,
            settings.nr_local_structure_strength,
            settings.nr_skin_structure_strength,
            settings.nr_paper_white_scale,
            settings.nr_hdr_transfer_strength,
            settings.nr_color_strength,
            settings.nr_motion_scale_x_multiplier,
            settings.nr_motion_scale_y_multiplier,
            region.shape_width,
            region.shape_height,
            region.roundness,
            region.transition,
        }) {
        std::uint32_t bits{};
        std::memcpy(&bits, &value, sizeof(bits));
        append(bits);
    }
    append(settings.nr_foveated ? 1U : 0U);
    append(settings.nr_automatic_mask ? 1U : 0U);
    append(settings.nr_ui_correction ? 1U : 0U);
    append(settings.nr_depth_convention);
    append(region.width);
    append(region.height);
    return signature;
}

}
