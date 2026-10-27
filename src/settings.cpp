#include "settings.hpp"
#include "quad_view_bridge.hpp"
#include "reflex_override_state.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <deque>
#include <mutex>
#include <Windows.h>

namespace cheeky::foveated_dlss {
namespace {

std::mutex settings_mutex;
Settings::FrameGeneration fg_settings;
std::atomic<bool> processing_allowed{true};

std::atomic<bool> enabled{true};
std::atomic<bool> d3d12_lower_hook{true};
std::atomic<bool> d3d11_use_d3d12_transport{false};
std::atomic<bool> d3d12_quad_compose_wait{true};
std::atomic<bool> peripheral_dlaa_enabled{true};
std::atomic<std::uint32_t> peripheral_dlaa_scale_bits{0x3F400000U};
std::atomic<std::uint32_t> center_preset{};
std::atomic<bool> fix_motion_blur{};
std::atomic<std::uint32_t> rr_center_preset{}, rr_peripheral_preset{};
std::atomic<std::uint32_t> center_supersampling_bits{0x3F800000U};
std::atomic<bool> afw_manual_coverage{false};
std::atomic<bool> afw_automatic_coverage{false};
std::atomic<std::uint32_t> afw_warp_margin_bits{0x3D4CCCCDU};
std::atomic<std::uint32_t> peripheral_dlaa_preset{5U};
std::atomic<std::uint32_t> width_bits{0x3F0CCCCDU};
std::atomic<std::uint32_t> height_bits{0x3EE66666U};
std::atomic<std::uint32_t> x_offset_bits{0U};
std::atomic<std::uint32_t> height_offset_bits{0xBEE66666U};
std::atomic<bool> invert_stereo_x_offset{false};
std::atomic<bool> auto_stereo_alignment{true};
std::atomic<bool> eye_calibration_continuous{false};
std::atomic<unsigned> calibration_method{};
unsigned learned_method{}, learned_sessions{};
std::uint64_t learned_signature{};
std::atomic<std::uint64_t> learning_revision{};
std::atomic<std::uint32_t> aligned_height_offset_bits{};
std::atomic<std::uint32_t> roundness_bits{};
std::atomic<std::uint32_t> transition_bits{0x3D23D70AU};
std::atomic<bool> alignment_border_enabled{false};
std::atomic<std::uint32_t> center_mode{};
std::atomic<bool> manual_stereo_mapping{false};
std::atomic<bool> manual_stereo_right_first{false};
std::atomic<bool> quad_manual_assignment{false},quad_show_numbers{false};
std::array<unsigned,8> quad_saved_roles{};unsigned quad_saved_source_count{}; // settings_mutex
std::atomic<bool> independent_openxr_gaze{false};

std::atomic<std::uint32_t> simulation_pattern{};
std::atomic<bool> show_next_jump_target{true};
std::atomic<std::uint32_t> gaze_smoothing_ms_bits{0x41A00000U};
std::atomic<std::uint32_t> gaze_hold_ms_bits{0x43C80000U};
std::atomic<std::uint32_t> gaze_quantization_pixels{8U};
std::atomic<std::uint32_t> gaze_jump_reset_ratio_bits{0x3E000000U};
std::atomic<bool> nr_enabled{false};
std::atomic<NrProcessingOrder> nr_order{NrProcessingOrder::after_upscaling};
std::atomic<bool> nr_foveated{true};
std::atomic<bool> nr_use_sr_foveation{false};
std::atomic<bool> nr_alignment_border_enabled{false};
std::atomic<std::uint32_t> nr_width_bits{0x3F0F5C29U};
std::atomic<std::uint32_t> nr_height_bits{0x3F0F5C29U};
std::atomic<std::uint32_t> nr_roundness_bits{};
std::atomic<std::uint32_t> nr_transition_bits{0x3DA3D70AU};
std::atomic<std::uint32_t> nr_working_scale_bits{0x3F800000U};
std::atomic<std::uint32_t> nr_preset{};
std::atomic<std::uint32_t> nr_style{};
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
    std::uint64_t generation{};
    std::uint64_t first_evaluation{};
    std::uint64_t evaluation_calls{};
    std::uint64_t last_evaluation_ms{};
    unsigned graphics_api{};
    std::uint32_t evaluation_input_width{}, evaluation_input_height{};
    std::uint32_t evaluation_output_width{}, evaluation_output_height{};
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
std::uint64_t registration_generation{};
std::uint64_t stereo_mapping_revision{1};
struct Calibration {
    std::uint64_t left{}, right{}, sequence{}, session_generation{};
    bool vertical_flip{};
    std::array<StereoSourceCrop, 2> source_crops{};
    std::uint64_t verified_ms{};
    EyeCalibrationMethod method{EyeCalibrationMethod::full};
} calibration;
bool calibration_live() {
    // Verified identity survives missing markers. View destruction, session
    // changes and explicit disable clear it; fresh evidence may replace it.
    return calibration.left && calibration.right;
}
StereoEyeAssignment calibrated_assignment(std::uint64_t view) {
    if (!calibration_live()) return {};
    auto crops = calibration.source_crops;
    const auto now = GetTickCount64();
    if (eye_calibration_continuous_validation(calibration.method) &&
        (now < calibration.verified_ms || now - calibration.verified_ms > 2500))
        for (auto& crop : crops) crop.valid = false;
    if (view == calibration.left) return {0, true, true, calibration.session_generation, calibration.vertical_flip,
        calibration.left == calibration.right, crops, stereo_mapping_revision};
    if (view == calibration.right) return {1, true, true, calibration.session_generation, calibration.vertical_flip, false, crops, stereo_mapping_revision};
    return {};
}

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

Settings configured_settings() noexcept {
    std::lock_guard lock(settings_mutex);
    Settings settings{};
    settings.fg = fg_settings;
    settings.enabled = enabled.load(std::memory_order_acquire);
    settings.d3d12_lower_hook = d3d12_lower_hook.load(std::memory_order_acquire);
    settings.d3d11_use_d3d12_transport =
        d3d11_use_d3d12_transport.load(std::memory_order_acquire);
    settings.d3d12_quad_compose_wait = d3d12_quad_compose_wait.load(std::memory_order_acquire);
    settings.peripheral_dlaa_enabled =
        peripheral_dlaa_enabled.load(std::memory_order_acquire);
    settings.peripheral_dlaa_scale = load_float(peripheral_dlaa_scale_bits);
    settings.center_supersampling = load_float(center_supersampling_bits);
    settings.afw_manual_coverage = afw_manual_coverage.load(std::memory_order_acquire);
    settings.afw_automatic_coverage = afw_automatic_coverage.load(std::memory_order_acquire);
    settings.afw_warp_margin = load_float(afw_warp_margin_bits);
    settings.rr_center_preset = rr_center_preset.load(std::memory_order_acquire);
    settings.rr_peripheral_preset = rr_peripheral_preset.load(std::memory_order_acquire);
    settings.center_preset =
        center_preset.load(std::memory_order_acquire);
    settings.fix_motion_blur = fix_motion_blur.load(std::memory_order_acquire);
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
    settings.auto_stereo_alignment = auto_stereo_alignment.load(std::memory_order_acquire);
    settings.eye_calibration_continuous = eye_calibration_continuous.load(std::memory_order_acquire);
    settings.eye_calibration_method = static_cast<EyeCalibrationMethod>(calibration_method.load());
    settings.eye_calibration_learned_method = learned_method;
    settings.eye_calibration_learned_signature = learned_signature;
    settings.eye_calibration_learned_sessions = learned_sessions;
    settings.aligned_height_offset = load_float(aligned_height_offset_bits);
    settings.center_mode = static_cast<FoveationCenterMode>(
        center_mode.load(std::memory_order_acquire)
    );
    settings.show_next_jump_target = show_next_jump_target.load(std::memory_order_acquire);
    settings.simulation_pattern = simulation_pattern.load(std::memory_order_acquire);
    settings.manual_stereo_mapping = manual_stereo_mapping.load(std::memory_order_acquire);
    settings.manual_stereo_right_first = manual_stereo_right_first.load(std::memory_order_acquire);
    settings.quad_manual_assignment=quad_manual_assignment.load();settings.quad_show_numbers=quad_show_numbers.load();
    settings.quad_saved_roles=quad_saved_roles;settings.quad_saved_source_count=quad_saved_source_count;
    settings.independent_openxr_gaze = independent_openxr_gaze.load(std::memory_order_acquire);
    settings.gaze_smoothing_ms = load_float(gaze_smoothing_ms_bits);
    settings.gaze_hold_ms = load_float(gaze_hold_ms_bits);
    settings.gaze_quantization_pixels = gaze_quantization_pixels.load(
        std::memory_order_acquire
    );
    settings.gaze_jump_reset_ratio = load_float(
        gaze_jump_reset_ratio_bits
    );
    settings.nr_processing_order = nr_order.load(std::memory_order_acquire);
    settings.nr_enabled = nr_enabled.load(std::memory_order_acquire);
    settings.nr_foveated = nr_foveated.load(std::memory_order_acquire);
    settings.nr_use_sr_foveation =
        nr_use_sr_foveation.load(std::memory_order_acquire);
    settings.nr_alignment_border_enabled =
        nr_alignment_border_enabled.load(std::memory_order_acquire);
    settings.nr_width = load_float(nr_width_bits);
    settings.nr_height = load_float(nr_height_bits);
    settings.nr_roundness = load_float(nr_roundness_bits);
    settings.nr_transition_width = load_float(nr_transition_bits);
    settings.nr_working_scale = load_float(nr_working_scale_bits);
    settings.nr_preset = nr_preset.load(std::memory_order_acquire);
    settings.nr_style = nr_style.load(std::memory_order_acquire);
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

Settings current_settings() noexcept {
    auto settings = configured_settings();
    if (!processing_allowed.load(std::memory_order_acquire)) {
        settings.enabled = false;
        settings.nr_enabled = false;
        settings.fg.enabled = false;
    }
    return settings;
}

void update_settings(const Settings& settings) noexcept {
    std::unique_lock lock(settings_mutex);
    fg_settings = settings.fg;
    reflex_override::requested.store(settings.fg.force_reflex_off,std::memory_order_release);
    fg_settings.multiplier=std::clamp(fg_settings.multiplier,2u,4u);
    const auto bounded = [](float value, float lo, float hi, float fallback) {
        return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
    };
    fg_settings.width = bounded(fg_settings.width, 0.2F, 1.0F, 0.55F);
    fg_settings.height = bounded(fg_settings.height, 0.2F, 1.0F, 0.45F);
    fg_settings.x = bounded(fg_settings.x, -0.5F, 0.5F, 0.0F);
    fg_settings.y = bounded(fg_settings.y, -1.0F, 1.0F, 0.0F);
    fg_settings.feather = bounded(fg_settings.feather, 0.0F, 0.3F, 0.04F);
    auto fg_percent=[](std::uint32_t n) {return n==25||n==50||n==75||n==100?n:100u;};
    fg_settings.resolution_percent=fg_percent(fg_settings.resolution_percent);
    fg_settings.peripheral_percent=fg_percent(fg_settings.peripheral_percent);
    if(!cheeky_fg_periphery_valid(fg_settings.peripheral_method))fg_settings.peripheral_method=1;
    fg_settings.queue_priority=fg_settings.queue_priority==1u?1u:2u;
    if(!cheeky_fg_center_valid(fg_settings.center_method))fg_settings.center_method=0;
    fg_settings.optical.center_preset=std::min(fg_settings.optical.center_preset,2u);
    fg_settings.optical.peripheral_preset=std::min(fg_settings.optical.peripheral_preset,2u);
    fg_settings.optical.center_percent=fg_percent(fg_settings.optical.center_percent);
    fg_settings.optical.peripheral_percent=fg_percent(fg_settings.optical.peripheral_percent);
    auto fg_grid=[](std::uint32_t n) {return n==1||n==2||n==4?n:4u;};
    fg_settings.optical.center_grid=fg_grid(fg_settings.optical.center_grid);
    fg_settings.optical.peripheral_grid=fg_grid(fg_settings.optical.peripheral_grid);
    d3d12_lower_hook.store(settings.d3d12_lower_hook, std::memory_order_release);
    enabled.store(settings.enabled, std::memory_order_release);
    d3d11_use_d3d12_transport.store(
        settings.d3d11_use_d3d12_transport,
        std::memory_order_release
    );
    d3d12_quad_compose_wait.store(settings.d3d12_quad_compose_wait, std::memory_order_release);
    peripheral_dlaa_enabled.store(
        settings.peripheral_dlaa_enabled,
        std::memory_order_release
    );
    store_float(
        peripheral_dlaa_scale_bits,
        std::isfinite(settings.peripheral_dlaa_scale)?std::clamp(settings.peripheral_dlaa_scale, 0.20F, 1.0F):0.75F
    );
    const auto valid_preset = [](const std::uint32_t value) noexcept {
        return value == 5U || value == 10U || value == 11U || value == 12U || value == 13U;
    };
    store_float(center_supersampling_bits, std::isfinite(settings.center_supersampling)
        ? std::clamp(settings.center_supersampling, 1.0F, 2.0F) : 1.0F);
    afw_manual_coverage.store(settings.afw_manual_coverage, std::memory_order_release);
    afw_automatic_coverage.store(settings.afw_automatic_coverage, std::memory_order_release);
    store_float(afw_warp_margin_bits, std::isfinite(settings.afw_warp_margin)
        ? std::clamp(settings.afw_warp_margin, 0.F, 0.25F) : 0.05F);
    const auto valid_rr = [](unsigned v) { return v == 0 || v == 4 || v == 5 || v == 6; };
    rr_center_preset.store(valid_rr(settings.rr_center_preset) ? settings.rr_center_preset : 0);
    rr_peripheral_preset.store(valid_rr(settings.rr_peripheral_preset) ? settings.rr_peripheral_preset : 0);
    fix_motion_blur.store(settings.fix_motion_blur, std::memory_order_release);
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
    auto_stereo_alignment.store(settings.auto_stereo_alignment, std::memory_order_release);
    eye_calibration_continuous.store(settings.eye_calibration_continuous, std::memory_order_release);
    calibration_method.store(unsigned(settings.eye_calibration_method) <= 3 ? unsigned(settings.eye_calibration_method) : 0);
    store_float(aligned_height_offset_bits, std::clamp(settings.aligned_height_offset, -1.0F, 1.0F));
    center_mode.store(
        static_cast<std::uint32_t>(settings.center_mode) <= 2U
            ? static_cast<std::uint32_t>(settings.center_mode)
            : 0U,
        std::memory_order_release
    );
    manual_stereo_mapping.store(settings.manual_stereo_mapping, std::memory_order_release);
    manual_stereo_right_first.store(settings.manual_stereo_right_first, std::memory_order_release);
    quad_manual_assignment.store(settings.quad_manual_assignment);quad_show_numbers.store(settings.quad_show_numbers);
    quad_saved_roles=settings.quad_saved_roles;quad_saved_source_count=std::min(settings.quad_saved_source_count,8u);
    independent_openxr_gaze.store(settings.independent_openxr_gaze, std::memory_order_release);
    show_next_jump_target.store(settings.show_next_jump_target, std::memory_order_release);
    simulation_pattern.store(std::min(settings.simulation_pattern, 5U), std::memory_order_release);
    store_float(
        gaze_smoothing_ms_bits,
        std::clamp(settings.gaze_smoothing_ms, 0.0F, 100.0F)
    );
    store_float(
        gaze_hold_ms_bits,
        std::clamp(settings.gaze_hold_ms, 0.0F, 1000.0F)
    );
    gaze_quantization_pixels.store(
        std::clamp(settings.gaze_quantization_pixels, 1U, 64U),
        std::memory_order_release
    );
    store_float(
        gaze_jump_reset_ratio_bits,
        std::clamp(settings.gaze_jump_reset_ratio, 0.01F, 1.0F)
    );
    nr_order.store(nr_processing_order(static_cast<std::uint32_t>(settings.nr_processing_order)), std::memory_order_release);
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
    nr_style.store((std::min)(settings.nr_style, 2U), std::memory_order_release);
    // NR 310.8 saturates the final model blend at one. Accept legacy saved
    // values above one, but expose the effective value to both integrations.
    store_float(nr_intensity_bits, std::clamp(settings.nr_intensity, 0.0F, 1.0F));
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
    lock.unlock();quad_view_configure(settings.quad_manual_assignment,settings.quad_show_numbers,settings.quad_saved_roles.data(),settings.quad_saved_source_count);
}

void set_processing_allowed(bool allowed) noexcept {
    processing_allowed.store(allowed, std::memory_order_release);
    reflex_override::processing.store(allowed,std::memory_order_release);
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
    stereo_views.back().generation = ++registration_generation;
    peak_stereo_view_count = (std::max)(
        peak_stereo_view_count,
        static_cast<std::uint32_t>(stereo_views.size())
    );
}

void unregister_stereo_view(const std::uint64_t view_id) noexcept {
    if (view_id == 0U) return;
    quad_view_forget(view_id,stereo_view_generation(view_id));
    std::lock_guard lock(stereo_views_mutex);
    if (view_id == calibration.left || view_id == calibration.right) { calibration = {}; ++stereo_mapping_revision; }
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
    const auto quad=quad_view_snapshot();if(quad.active)return quad.status==2;
    std::lock_guard lock(stereo_views_mutex);
    if (calibration_live()) return calibration.left != calibration.right;
    return eye_roles[0].view_id != 0U && eye_roles[1].view_id != 0U;
}

StereoEyeAssignment stereo_eye_assignment(
    const std::uint64_t view_id
) noexcept {
    const auto quad=quad_view_snapshot();
    std::lock_guard lock(stereo_views_mutex);
    if(quad.active){StereoEyeAssignment result;result.mapping_revision=quad.revision;
        for(const auto& view:stereo_views)if(view.view_id==view_id){
            if(const auto* role=quad_view_assignment(quad,view_id,view.generation)){
                result.assigned=true;result.eye_index=role->eye_index;result.quad_assigned=true;
                result.quad_manual=quad.manual_mode!=0;result.quad_view_index=role->view_index;
                result.calibration_session=quad.session_generation;}
            break;}
        return result;}
    const auto corrected = calibrated_assignment(view_id);
    if (corrected.assigned) return corrected;
    StereoEyeAssignment fallback;fallback.mapping_revision=stereo_mapping_revision;
    if (eye_roles[0].view_id == 0U || eye_roles[1].view_id == 0U) return fallback;
    for (std::uint32_t index{}; index < 2U; ++index) {
        if (eye_roles[index].view_id == view_id) { fallback.eye_index=index;fallback.assigned=true;return fallback; }
    }
    return fallback;
}

StereoViewStatistics stereo_view_statistics() noexcept {
    std::lock_guard lock(stereo_views_mutex);
    return {
        static_cast<std::uint32_t>(stereo_views.size()),
        peak_stereo_view_count,
        static_cast<std::uint32_t>(seen_stereo_views.size()),
    };
}

std::uint64_t stereo_view_generation(std::uint64_t view_id) noexcept {
    std::lock_guard lock(stereo_views_mutex);
    for (const auto& view : stereo_views) {
        if (view.view_id == view_id) return view.generation;
    }
    return 0;
}

bool publish_stereo_calibration(std::uint64_t left, std::uint64_t right,
    std::uint64_t left_generation, std::uint64_t right_generation,
    std::uint64_t sequence, std::uint64_t captured_ms, bool* corrected,
    std::uint64_t session_generation, bool vertical_flip, bool shared_source,
    const std::array<StereoSourceCrop, 2>* source_crops, EyeCalibrationMethod method) noexcept {
    if (corrected) *corrected = false;
    if (!left || !right || (left == right) != shared_source || !left_generation || !right_generation) return false;
    std::lock_guard lock(stereo_views_mutex);
    const auto now = GetTickCount64();
    constexpr std::uint64_t lifetime_ms = 1000;
    if (captured_ms > now || now - captured_ms > lifetime_ms || sequence <= calibration.sequence) return false;
    bool found_left{}, found_right{};
    for (const auto& view : stereo_views) {
        found_left |= view.view_id == left && view.generation == left_generation;
        found_right |= view.view_id == right && view.generation == right_generation;
    }
    if (!found_left || !found_right) return false;
    const auto previous_eye = [](std::uint64_t view_id) {
        const auto corrected_role = calibrated_assignment(view_id);
        if (corrected_role.assigned) return int(corrected_role.eye_index);
        if (eye_roles[0].view_id && eye_roles[1].view_id) {
            for (int eye = 0; eye < 2; ++eye) {
                if (eye_roles[eye].view_id == view_id) return eye;
            }
        }
        return -1;
    };
    const int previous_left = previous_eye(left), previous_right = previous_eye(right);
    if (corrected) {
        *corrected = shared_source ? calibration.left != left || calibration.right != right :
            (calibration.left && calibration.left == calibration.right) || (previous_left >= 0 && previous_left != 0) ||
            (previous_right >= 0 && previous_right != 1);
    }
    if (calibration.left!=left || calibration.right!=right || calibration.session_generation!=session_generation ||
        calibration.vertical_flip!=vertical_flip) ++stereo_mapping_revision;
    calibration = {left, right, sequence, session_generation, vertical_flip};
    calibration.method = method;
    if (source_crops) { calibration.source_crops = *source_crops; calibration.verified_ms = captured_ms; }
    return true;
}
void invalidate_stereo_crop() noexcept {
    std::lock_guard lock(stereo_views_mutex);
    for (auto& crop : calibration.source_crops) crop.valid = false;
}
void set_eye_calibration_learning(unsigned method, std::uint64_t signature, unsigned sessions) noexcept {
    std::lock_guard lock(settings_mutex);
    if (!method || method > 3 || !signature || !sessions) { method = 0; signature = 0; sessions = 0; }
    sessions = (std::min)(sessions, 2U);
    if (learned_method == method && learned_signature == signature && learned_sessions == sessions) return;
    learned_method = method; learned_signature = signature; learned_sessions = sessions;
    ++learning_revision;
}
std::uint64_t eye_calibration_learning_revision() noexcept { return learning_revision.load(); }
EyeCalibrationMethod eye_calibration_selected_method() noexcept {
    return static_cast<EyeCalibrationMethod>(calibration_method.load());
}
bool eye_calibration_continuous_validation(EyeCalibrationMethod method) noexcept {
    return method != EyeCalibrationMethod::full || eye_calibration_continuous.load(std::memory_order_acquire);
}
void clear_stereo_calibration() noexcept {
    std::lock_guard lock(stereo_views_mutex);
    ++stereo_mapping_revision;
    calibration = {};
}

std::vector<StereoViewDetail> stereo_view_details() {
    const auto quad=quad_view_snapshot();
    std::lock_guard lock(stereo_views_mutex);
    const auto now = GetTickCount64();
    std::vector<StereoViewDetail> details;
    details.reserve(stereo_views.size());
    for (const auto& view : stereo_views) {
        const auto corrected = calibrated_assignment(view.view_id);
        details.push_back({
            view.view_id,
            corrected.assigned ? corrected.eye_index == 1 : view.second_eye,
            corrected.assigned || view.has_eye_assignment,
            view.evaluations,
            view.render_width,
            view.render_height,
            view.output_width,
            view.output_height,
            view.crop,
            view.has_geometry,
            view.generation,
            view.evaluation_calls,
            view.evaluation_calls ? now - view.last_evaluation_ms : 0,
            view.evaluation_calls && now - view.last_evaluation_ms <= 2000,
            view.graphics_api,
            corrected.calibrated,
            view.evaluation_input_width, view.evaluation_input_height,
            view.evaluation_output_width, view.evaluation_output_height,
        });
        if(quad.active){auto& detail=details.back();const auto* role=quad_view_assignment(quad,view.view_id,view.generation);
            detail.calibrated=false;detail.has_eye_assignment=role!=nullptr;
            detail.second_eye=role&&role->eye_index==1;
            if(role){detail.quad_region=role->focus?2u:1u;detail.quad_view_index=role->view_index;detail.quad_manual=quad.manual_mode!=0;}}
    }
    return details;
}

void note_stereo_view_evaluation(std::uint64_t view_id, unsigned graphics_api,
    std::uint32_t input_width, std::uint32_t input_height,
    std::uint32_t output_width, std::uint32_t output_height) noexcept {
    std::lock_guard lock(stereo_views_mutex);
    for (auto& view : stereo_views) {
        if (view.view_id != view_id) continue;
        ++view.evaluation_calls;
        view.last_evaluation_ms = GetTickCount64();
        view.graphics_api = graphics_api;
        view.evaluation_input_width = input_width;
        view.evaluation_input_height = input_height;
        view.evaluation_output_width = output_width;
        view.evaluation_output_height = output_height;
        break;
    }
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
    const auto quad=quad_view_snapshot();
    if(quad.active){auto result=settings;result.quad_view_mode=true;result.quad_view_region=0;
        const auto* role=quad_view_assignment(quad,view_id,stereo_view_generation(view_id));
        if(role)result.quad_view_region=role->focus?2u:1u;
        result.width=result.height=1.0F;result.x_offset=result.height_offset=result.aligned_height_offset=0;
        result.roundness=result.transition_width=0;result.alignment_border_enabled=false;
        result.auto_stereo_alignment=false;result.center_mode=FoveationCenterMode::fixed;
        result.next_jump_visible=false;result.eye_independent_coverage=false;
        result.afw_manual_coverage=result.afw_automatic_coverage=false;result.afw_mask={};result.afw_nr_mask={};
        result.nr_foveated=result.nr_use_sr_foveation=result.nr_alignment_border_enabled=false;
        result.nr_enabled=settings.nr_enabled&&role&&role->focus;
        // Quad FG is captured/presented by the layer, independently of SR transport.
        result.fg.enabled=false;
        if(!role){result.enabled=false;result.peripheral_dlaa_enabled=false;}
        else if(role->focus){result.peripheral_dlaa_enabled=false;}
        else {result.enabled=settings.enabled&&settings.peripheral_dlaa_enabled;
            result.center_preset=settings.peripheral_dlaa_preset;result.rr_center_preset=settings.rr_peripheral_preset;
            result.center_supersampling=1.0F;result.d3d11_use_d3d12_transport=false;}
        return result;}
    if (settings.eye_independent_coverage) return settings;
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
        return result;
    }

    if (calibration_live()) {
        const auto corrected = calibrated_assignment(view_id);
        matched_view->has_eye_assignment = corrected.assigned;
        matched_view->second_eye = corrected.eye_index == 1;
        result.x_offset = corrected.assigned && !corrected.shared_source
            ? ((matched_view->second_eye != settings.invert_stereo_x_offset) ? -settings.x_offset : settings.x_offset)
            : 0.0F;
        return result;
    }

    const auto sequence = ++stereo_evaluation_sequence;
    if (!matched_view->first_evaluation) matched_view->first_evaluation = sequence;
    // Manual eye order applies to a stereo pair, not every DLSS feature. A
    // desktop/mirror view interleaved with the eyes must not rotate the two
    // fallback roles through the LRU cache. Geometry selects the pair only;
    // the user's manual policy still supplies its eye identity.
    if (settings.manual_stereo_mapping && stereo_views.size() > 2U) {
        StereoView* pair[2]{};
        unsigned pairs{};
        for (auto& first : stereo_views) {
            if (!first.has_geometry || !first.output_width || !first.output_height ||
                !first.first_evaluation) continue;
            for (auto& second : stereo_views) {
                if (second.generation <= first.generation || !second.has_geometry ||
                    !second.first_evaluation || first.output_width != second.output_width ||
                    first.output_height != second.output_height) continue;
                pair[0] = &first; pair[1] = &second; ++pairs;
            }
        }
        if (pairs == 1U) {
            if (pair[1]->first_evaluation < pair[0]->first_evaluation) std::swap(pair[0], pair[1]);
            for (unsigned eye = 0; eye < 2U; ++eye)
                eye_roles[eye].view_id = pair[eye]->view_id;
            for (auto& view : stereo_views) {
                view.has_eye_assignment = &view == pair[0] || &view == pair[1];
                view.second_eye = &view == pair[1];
            }
            if (!matched_view->has_eye_assignment) {
                result.x_offset = 0.0F;
                return result;
            }
        }
    }
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
        return result;
    }
    const bool negative = matched_view->second_eye !=
        settings.invert_stereo_x_offset;
    result.x_offset = negative ? -settings.x_offset : settings.x_offset;
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
