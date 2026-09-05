#include "settings.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>

namespace cheeky::foveated_dlss {
namespace {

std::atomic<bool> enabled{true};
std::atomic<bool> d3d11_use_d3d12_transport{false};
std::atomic<bool> peripheral_dlaa_enabled{true};
std::atomic<std::uint32_t> peripheral_dlaa_scale_bits{0x3F400000U};
std::atomic<std::uint32_t> center_preset{};
std::atomic<std::uint32_t> peripheral_dlaa_preset{5U};
std::atomic<std::uint32_t> width_bits{0x3F0CCCCDU};
std::atomic<std::uint32_t> height_bits{0x3EE66666U};
std::atomic<std::uint32_t> x_offset_bits{0x3F19999AU};
std::atomic<std::uint32_t> height_offset_bits{0xBEE66666U};
std::atomic<bool> invert_stereo_x_offset{false};
std::atomic<std::uint32_t> roundness_bits{};
std::atomic<std::uint32_t> transition_bits{0x3D23D70AU};
std::atomic<bool> alignment_border_enabled{false};
std::atomic<std::uint32_t> center_mode{};
std::atomic<std::uint32_t> simulation_pattern{};
std::atomic<bool> show_next_jump_target{true};
std::atomic<std::uint32_t> gaze_smoothing_ms_bits{0x41A00000U};
std::atomic<std::uint32_t> gaze_quantization_pixels{8U};
std::atomic<std::uint32_t> gaze_jump_reset_ratio_bits{0x3E000000U};
std::atomic<bool> nr_enabled{false};
std::atomic<bool> nr_foveated{true};
std::atomic<bool> nr_use_sr_foveation{false};
std::atomic<bool> nr_alignment_border_enabled{false};
std::atomic<std::uint32_t> nr_width_bits{0x3F0F5C29U};
std::atomic<std::uint32_t> nr_height_bits{0x3F0F5C29U};
std::atomic<std::uint32_t> nr_x_offset_bits{0x3E9EB852U};
std::atomic<std::uint32_t> nr_height_offset_bits{0xBEE147AEU};
std::atomic<bool> nr_invert_stereo_x_offset{false};
std::atomic<std::uint32_t> nr_roundness_bits{};
std::atomic<std::uint32_t> nr_transition_bits{0x3DA3D70AU};
std::atomic<std::uint32_t> nr_working_scale_bits{0x3F4CCCCDU};
std::atomic<std::uint32_t> nr_preset{};
std::atomic<std::uint32_t> nr_intensity_bits{0x3F800000U};
std::atomic<std::uint32_t> nr_local_tone_bits{0x3F800000U};
std::atomic<std::uint32_t> nr_local_structure_bits{0x3F800000U};
std::atomic<std::uint32_t> nr_skin_structure_bits{0x3F800000U};
std::atomic<bool> nr_automatic_mask{false};
std::atomic<bool> nr_ui_correction{false};
std::atomic<std::uint32_t> nr_paper_white_bits{0x3F800000U};
std::atomic<std::uint32_t> nr_hdr_transfer_bits{0x3F800000U};
std::atomic<std::uint32_t> nr_color_strength_bits{0x3F800000U};
std::atomic<std::uint32_t> nr_depth_convention{};
std::atomic<std::uint32_t> nr_motion_scale_x_bits{0x3F800000U};
std::atomic<std::uint32_t> nr_motion_scale_y_bits{0x3F800000U};

struct StereoView {
    std::uint64_t view_id{};
    bool second_eye{};
    bool has_eye_assignment{};
    std::uint64_t evaluations{};
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    CropGeometry crop{};
    bool has_geometry{};
};

std::mutex stereo_views_mutex;
std::deque<StereoView> stereo_views;
std::deque<StereoView> seen_stereo_views;
std::uint32_t peak_stereo_view_count{};

struct EyeRole {
    std::uint64_t view_id{};
    std::uint64_t last_evaluation{};
};

EyeRole eye_roles[2]{};
std::uint64_t stereo_evaluation_sequence{};

void store_float(std::atomic<std::uint32_t>& destination, float value) noexcept {
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    destination.store(bits, std::memory_order_release);
}

[[nodiscard]] float load_float(
    const std::atomic<std::uint32_t>& source
) noexcept {
    const auto bits = source.load(std::memory_order_acquire);
    float value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}  // namespace

Settings current_settings() noexcept {
    Settings settings{};
    settings.enabled = enabled.load(std::memory_order_acquire);
    settings.d3d11_use_d3d12_transport =
        d3d11_use_d3d12_transport.load(std::memory_order_acquire);
    settings.peripheral_dlaa_enabled =
        peripheral_dlaa_enabled.load(std::memory_order_acquire);
    settings.peripheral_dlaa_scale = load_float(peripheral_dlaa_scale_bits);
    settings.center_preset =
        center_preset.load(std::memory_order_acquire);
    settings.peripheral_dlaa_preset =
        peripheral_dlaa_preset.load(std::memory_order_acquire);
    settings.width = load_float(width_bits);
    settings.height = load_float(height_bits);
    settings.x_offset = load_float(x_offset_bits);
    settings.height_offset = load_float(height_offset_bits);
    settings.invert_stereo_x_offset =
        invert_stereo_x_offset.load(std::memory_order_acquire);
    settings.roundness = load_float(roundness_bits);
    settings.transition_width = load_float(transition_bits);
    settings.alignment_border_enabled =
        alignment_border_enabled.load(std::memory_order_acquire);
    settings.center_mode = static_cast<FoveationCenterMode>(
        center_mode.load(std::memory_order_acquire)
    );
    settings.show_next_jump_target = show_next_jump_target.load(std::memory_order_acquire);
    settings.simulation_pattern = simulation_pattern.load(std::memory_order_acquire);
    settings.gaze_smoothing_ms = load_float(gaze_smoothing_ms_bits);
    settings.gaze_quantization_pixels = gaze_quantization_pixels.load(
        std::memory_order_acquire
    );
    settings.gaze_jump_reset_ratio = load_float(
        gaze_jump_reset_ratio_bits
    );
    settings.nr_enabled = nr_enabled.load(std::memory_order_acquire);
    settings.nr_foveated = nr_foveated.load(std::memory_order_acquire);
    settings.nr_use_sr_foveation =
        nr_use_sr_foveation.load(std::memory_order_acquire);
    settings.nr_alignment_border_enabled =
        nr_alignment_border_enabled.load(std::memory_order_acquire);
    settings.nr_width = load_float(nr_width_bits);
    settings.nr_height = load_float(nr_height_bits);
    settings.nr_x_offset = load_float(nr_x_offset_bits);
    settings.nr_height_offset = load_float(nr_height_offset_bits);
    settings.nr_invert_stereo_x_offset =
        nr_invert_stereo_x_offset.load(std::memory_order_acquire);
    settings.nr_roundness = load_float(nr_roundness_bits);
    settings.nr_transition_width = load_float(nr_transition_bits);
    settings.nr_working_scale = load_float(nr_working_scale_bits);
    settings.nr_preset = nr_preset.load(std::memory_order_acquire);
    settings.nr_intensity = load_float(nr_intensity_bits);
    settings.nr_local_tone_strength = load_float(nr_local_tone_bits);
    settings.nr_local_structure_strength = load_float(nr_local_structure_bits);
    settings.nr_skin_structure_strength = load_float(nr_skin_structure_bits);
    settings.nr_automatic_mask = nr_automatic_mask.load(std::memory_order_acquire);
    settings.nr_ui_correction = nr_ui_correction.load(std::memory_order_acquire);
    settings.nr_paper_white_scale = load_float(nr_paper_white_bits);
    settings.nr_hdr_transfer_strength = load_float(nr_hdr_transfer_bits);
    settings.nr_color_strength = load_float(nr_color_strength_bits);
    settings.nr_depth_convention =
        nr_depth_convention.load(std::memory_order_acquire);
    settings.nr_motion_scale_x_multiplier = load_float(nr_motion_scale_x_bits);
    settings.nr_motion_scale_y_multiplier = load_float(nr_motion_scale_y_bits);
    return settings;
}

void update_settings(const Settings& settings) noexcept {
    enabled.store(settings.enabled, std::memory_order_release);
    d3d11_use_d3d12_transport.store(
        settings.d3d11_use_d3d12_transport,
        std::memory_order_release
    );
    peripheral_dlaa_enabled.store(
        settings.peripheral_dlaa_enabled,
        std::memory_order_release
    );
    store_float(
        peripheral_dlaa_scale_bits,
        std::clamp(settings.peripheral_dlaa_scale, 0.20F, 1.0F)
    );
    const auto valid_preset = [](const std::uint32_t value) noexcept {
        return value == 5U || value == 11U || value == 12U || value == 13U;
    };
    center_preset.store(
        settings.center_preset == 0U || valid_preset(settings.center_preset)
            ? settings.center_preset
            : 0U,
        std::memory_order_release
    );
    peripheral_dlaa_preset.store(
        valid_preset(settings.peripheral_dlaa_preset)
            ? settings.peripheral_dlaa_preset
            : 5U,
        std::memory_order_release
    );
    store_float(width_bits, std::clamp(settings.width, 0.20F, 1.0F));
    store_float(height_bits, std::clamp(settings.height, 0.20F, 1.0F));
    store_float(x_offset_bits, std::clamp(settings.x_offset, -1.0F, 1.0F));
    store_float(
        height_offset_bits,
        std::clamp(settings.height_offset, -1.0F, 1.0F)
    );
    invert_stereo_x_offset.store(
        settings.invert_stereo_x_offset,
        std::memory_order_release
    );
    store_float(roundness_bits, std::clamp(settings.roundness, 0.0F, 1.0F));
    store_float(
        transition_bits,
        std::clamp(settings.transition_width, 0.0F, 0.30F)
    );
    alignment_border_enabled.store(
        settings.alignment_border_enabled,
        std::memory_order_release
    );
    center_mode.store(
        static_cast<std::uint32_t>(settings.center_mode) <= 2U
            ? static_cast<std::uint32_t>(settings.center_mode)
            : 0U,
        std::memory_order_release
    );
    show_next_jump_target.store(settings.show_next_jump_target, std::memory_order_release);
    simulation_pattern.store(std::min(settings.simulation_pattern, 5U), std::memory_order_release);
    store_float(
        gaze_smoothing_ms_bits,
        std::clamp(settings.gaze_smoothing_ms, 0.0F, 100.0F)
    );
    gaze_quantization_pixels.store(
        std::clamp(settings.gaze_quantization_pixels, 1U, 64U),
        std::memory_order_release
    );
    store_float(
        gaze_jump_reset_ratio_bits,
        std::clamp(settings.gaze_jump_reset_ratio, 0.01F, 1.0F)
    );
    nr_enabled.store(settings.nr_enabled, std::memory_order_release);
    nr_foveated.store(settings.nr_foveated, std::memory_order_release);
    nr_use_sr_foveation.store(
        settings.nr_use_sr_foveation,
        std::memory_order_release
    );
    nr_alignment_border_enabled.store(
        settings.nr_alignment_border_enabled,
        std::memory_order_release
    );
    store_float(nr_width_bits, std::clamp(settings.nr_width, 0.20F, 1.0F));
    store_float(nr_height_bits, std::clamp(settings.nr_height, 0.20F, 1.0F));
    store_float(
        nr_x_offset_bits,
        std::clamp(settings.nr_x_offset, -1.0F, 1.0F)
    );
    store_float(
        nr_height_offset_bits,
        std::clamp(settings.nr_height_offset, -1.0F, 1.0F)
    );
    nr_invert_stereo_x_offset.store(
        settings.nr_invert_stereo_x_offset,
        std::memory_order_release
    );
    store_float(
        nr_roundness_bits,
        std::clamp(settings.nr_roundness, 0.0F, 1.0F)
    );
    store_float(
        nr_transition_bits,
        std::clamp(settings.nr_transition_width, 0.0F, 0.30F)
    );
    store_float(
        nr_working_scale_bits,
        std::clamp(settings.nr_working_scale, 0.10F, 1.0F)
    );
    nr_preset.store((std::min)(settings.nr_preset, 7U), std::memory_order_release);
    store_float(nr_intensity_bits, std::clamp(settings.nr_intensity, 0.0F, 2.0F));
    store_float(
        nr_local_tone_bits,
        std::clamp(settings.nr_local_tone_strength, 0.0F, 2.0F)
    );
    store_float(
        nr_local_structure_bits,
        std::clamp(settings.nr_local_structure_strength, 0.0F, 2.0F)
    );
    store_float(
        nr_skin_structure_bits,
        std::clamp(settings.nr_skin_structure_strength, 0.0F, 2.0F)
    );
    nr_automatic_mask.store(settings.nr_automatic_mask, std::memory_order_release);
    nr_ui_correction.store(settings.nr_ui_correction, std::memory_order_release);
    store_float(
        nr_paper_white_bits,
        std::clamp(settings.nr_paper_white_scale, 0.01F, 8.0F)
    );
    store_float(
        nr_hdr_transfer_bits,
        std::clamp(settings.nr_hdr_transfer_strength, 0.0F, 2.0F)
    );
    store_float(
        nr_color_strength_bits,
        std::clamp(settings.nr_color_strength, 0.0F, 2.0F)
    );
    nr_depth_convention.store(
        (std::min)(settings.nr_depth_convention, 2U),
        std::memory_order_release
    );
    store_float(
        nr_motion_scale_x_bits,
        std::clamp(settings.nr_motion_scale_x_multiplier, -4.0F, 4.0F)
    );
    store_float(
        nr_motion_scale_y_bits,
        std::clamp(settings.nr_motion_scale_y_multiplier, -4.0F, 4.0F)
    );
}

void register_stereo_view(const std::uint64_t view_id) noexcept {
    if (view_id == 0U) return;
    std::lock_guard lock(stereo_views_mutex);
    for (const auto& view : stereo_views) {
        if (view.view_id == view_id) return;
    }
    bool seen_before{};
    for (const auto& seen_view : seen_stereo_views) {
        if (seen_view.view_id != view_id) continue;
        seen_before = true;
        break;
    }
    if (!seen_before) {
        seen_stereo_views.push_back({view_id});
    }
    stereo_views.push_back({view_id});
    peak_stereo_view_count = (std::max)(
        peak_stereo_view_count,
        static_cast<std::uint32_t>(stereo_views.size())
    );
}

void unregister_stereo_view(const std::uint64_t view_id) noexcept {
    if (view_id == 0U) return;
    std::lock_guard lock(stereo_views_mutex);
    for (auto iterator = stereo_views.begin();
         iterator != stereo_views.end(); ++iterator) {
        if (iterator->view_id != view_id) continue;
        stereo_views.erase(iterator);
        break;
    }
    for (auto& role : eye_roles) {
        if (role.view_id == view_id) role = {};
    }
}

bool has_multiple_stereo_views() noexcept {
    std::lock_guard lock(stereo_views_mutex);
    return eye_roles[0].view_id != 0U && eye_roles[1].view_id != 0U;
}

StereoEyeAssignment stereo_eye_assignment(
    const std::uint64_t view_id
) noexcept {
    std::lock_guard lock(stereo_views_mutex);
    if (eye_roles[0].view_id == 0U || eye_roles[1].view_id == 0U) return {};
    for (std::uint32_t index{}; index < 2U; ++index) {
        if (eye_roles[index].view_id == view_id) return {index, true};
    }
    return {};
}

StereoViewStatistics stereo_view_statistics() noexcept {
    std::lock_guard lock(stereo_views_mutex);
    return {
        static_cast<std::uint32_t>(stereo_views.size()),
        peak_stereo_view_count,
        static_cast<std::uint32_t>(seen_stereo_views.size()),
    };
}

std::vector<StereoViewDetail> stereo_view_details() {
    std::lock_guard lock(stereo_views_mutex);
    std::vector<StereoViewDetail> details;
    details.reserve(stereo_views.size());
    for (const auto& view : stereo_views) {
        details.push_back({
            view.view_id,
            view.second_eye,
            view.has_eye_assignment,
            view.evaluations,
            view.render_width,
            view.render_height,
            view.output_width,
            view.output_height,
            view.crop,
            view.has_geometry,
        });
    }
    return details;
}

void note_stereo_view_geometry(
    const std::uint64_t view_id,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const CropGeometry& crop
) noexcept {
    std::lock_guard lock(stereo_views_mutex);
    for (auto& view : stereo_views) {
        if (view.view_id != view_id) continue;
        ++view.evaluations;
        view.render_width = render_width;
        view.render_height = render_height;
        view.output_width = output_width;
        view.output_height = output_height;
        view.crop = crop;
        view.has_geometry = true;
        break;
    }
}

Settings settings_for_view(
    const Settings& settings,
    const std::uint64_t view_id
) noexcept {
    auto result = settings;
    std::lock_guard lock(stereo_views_mutex);
    StereoView* matched_view{};
    for (auto& view : stereo_views) {
        if (view.view_id == view_id) {
            matched_view = &view;
            break;
        }
    }
    if (matched_view == nullptr) {
        result.x_offset = 0.0F;
        result.nr_x_offset = 0.0F;
        return result;
    }

    const auto sequence = ++stereo_evaluation_sequence;
    std::size_t role_index = 2U;
    for (std::size_t index{}; index < 2U; ++index) {
        if (eye_roles[index].view_id == view_id) {
            role_index = index;
            break;
        }
    }
    if (role_index == 2U) {
        if (eye_roles[0].view_id == 0U) {
            role_index = 0U;
        } else if (eye_roles[1].view_id == 0U) {
            role_index = 1U;
        } else {
            role_index = eye_roles[0].last_evaluation <=
                    eye_roles[1].last_evaluation
                ? 0U
                : 1U;
            for (auto& view : stereo_views) {
                if (view.view_id != eye_roles[role_index].view_id) continue;
                view.has_eye_assignment = false;
                break;
            }
        }
        eye_roles[role_index].view_id = view_id;
    }
    eye_roles[role_index].last_evaluation = sequence;
    matched_view->second_eye = role_index == 1U;
    matched_view->has_eye_assignment = true;

    if (eye_roles[0].view_id == 0U || eye_roles[1].view_id == 0U) {
        result.x_offset = 0.0F;
        result.nr_x_offset = 0.0F;
        return result;
    }
    const bool negative = matched_view->second_eye !=
        settings.invert_stereo_x_offset;
    result.x_offset = negative ? -settings.x_offset : settings.x_offset;
    const bool nr_negative = matched_view->second_eye !=
        settings.nr_invert_stereo_x_offset;
    result.nr_x_offset = nr_negative
        ? -settings.nr_x_offset
        : settings.nr_x_offset;
    return result;
}

FoveationParameters foveation_parameters(const Settings& settings) noexcept {
    return {
        settings.width,
        settings.height,
        settings.x_offset,
        settings.height_offset,
        settings.roundness,
        settings.transition_width,
    };
}

bool calculate_crop(
    const Settings& settings,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const std::uint32_t output_origin_x,
    const std::uint32_t output_origin_y,
    CropGeometry& crop
) noexcept {
    return settings.enabled && calculate_foveation_geometry(
        foveation_parameters(settings),
        render_width,
        render_height,
        output_width,
        output_height,
        output_origin_x,
        output_origin_y,
        crop
    );
}

}  // namespace cheeky::foveated_dlss
