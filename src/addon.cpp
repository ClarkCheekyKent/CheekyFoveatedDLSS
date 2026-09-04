#include "backend.hpp"
#include "d3d11_d3d12_transport.hpp"
#include "d3d11_peripheral_dlaa.hpp"
#include "diagnostics.hpp"
#include "dlss_nr.hpp"
#include "runtime.hpp"
#include "settings.hpp"

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace cheeky::foveated_dlss {
namespace {

using LogMessageFn = void (*)(void*, int, const char*);

std::atomic<HMODULE> addon_module{};
std::atomic<LogMessageFn> reshade_log{};
std::atomic<HANDLE> trace_log{};
SRWLOCK trace_log_lock = SRWLOCK_INIT;

constexpr char config_section[] = "CheekyFoveatedDLSS";
std::atomic<bool> toggle_hotkey_down{};

struct DiagnosticDisplayCache {
    DiagnosticSnapshot snapshot{};
    double next_refresh{};
    bool valid{};
};

[[nodiscard]] const DiagnosticSnapshot& displayed_diagnostics(
    const DiagnosticApi api
) noexcept {
    static std::array<DiagnosticDisplayCache, 2U> caches{};
    auto& cache = caches[api == DiagnosticApi::d3d12 ? 1U : 0U];
    const auto now = ImGui::GetTime();
    if (!cache.valid || now >= cache.next_refresh) {
        cache.snapshot = diagnostic_snapshot(api);
        cache.next_refresh = now + 0.25;
        cache.valid = true;
    }
    return cache.snapshot;
}

struct DiagnosticTable {
    bool open{};

    explicit DiagnosticTable(const char* const id) {
        constexpr auto flags = ImGuiTableFlags_SizingStretchProp |
            ImGuiTableFlags_RowBg;
        open = ImGui::BeginTable(id, 2, flags);
        if (!open) return;
        ImGui::TableSetupColumn(
            "Item", ImGuiTableColumnFlags_WidthStretch, 0.48F
        );
        ImGui::TableSetupColumn(
            "Value", ImGuiTableColumnFlags_WidthStretch, 0.52F
        );
    }

    ~DiagnosticTable() {
        if (open) ImGui::EndTable();
    }

    [[nodiscard]] explicit operator bool() const noexcept { return open; }
};

void diagnostic_row(const char* const label, const char* const format, ...) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    va_list arguments;
    va_start(arguments, format);
    ImGui::TextV(format, arguments);
    va_end(arguments);
}

[[nodiscard]] float pixel_percentage(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t original_width,
    const std::uint32_t original_height
) noexcept {
    const auto original_pixels =
        static_cast<double>(original_width) * original_height;
    if (original_pixels <= 0.0) return 0.0F;
    return static_cast<float>(
        static_cast<double>(width) * height * 100.0 / original_pixels
    );
}

void timing_row(const char* const label, const float milliseconds) {
    if (milliseconds > 0.0F) {
        diagnostic_row(label, "%.3f ms", milliseconds);
    } else {
        diagnostic_row(label, "Not sampled yet");
    }
}

void draw_api_diagnostics(
    const char* const label,
    const DiagnosticApi api
) {
    const auto& data = displayed_diagnostics(api);
    const auto settings = current_settings();
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);

    ImGui::SeparatorText("Status");
    if (DiagnosticTable table{"status"}) {
        diagnostic_row("State", "%s", diagnostic_state_name(data.state));
        if (api == DiagnosticApi::d3d11) {
            diagnostic_row(
                "Current route", "%s",
                d3d11_execution_path_name(data.d3d11_execution_path)
            );
        }
        const auto views = stereo_view_statistics();
        diagnostic_row(
            "Views", "Active %u | Peak %u | Seen %u",
            views.active,
            views.peak,
            views.seen
        );
        diagnostic_row(
            "Calls", "Create %llu | Evaluate %llu | Foveated %llu",
            static_cast<unsigned long long>(data.create_calls),
            static_cast<unsigned long long>(data.evaluate_calls),
            static_cast<unsigned long long>(data.active_calls)
        );
    }

    if (ImGui::TreeNode("View details")) {
        const auto views = stereo_view_details();
        const auto assigned_views = static_cast<std::size_t>(std::count_if(
            views.begin(),
            views.end(),
            [](const StereoViewDetail& view) {
                return view.has_eye_assignment;
            }
        ));
        for (std::size_t index{}; index < views.size(); ++index) {
            const auto& view = views[index];
            const bool negative = view.second_eye !=
                settings.invert_stereo_x_offset;
            const auto effective_x = assigned_views < 2U ||
                    !view.has_eye_assignment
                ? 0.0F
                : negative ? -settings.x_offset : settings.x_offset;
            ImGui::PushID(static_cast<int>(index));
            ImGui::SeparatorText("View");
            ImGui::Text(
                "#%u | ID 0x%016llX | Role %s | X offset %+.2f",
                static_cast<unsigned>(index + 1U),
                static_cast<unsigned long long>(view.view_id),
                !view.has_eye_assignment
                    ? "Idle"
                    : view.second_eye ? "B" : "A",
                effective_x
            );
            ImGui::Text(
                "Evaluations: %llu",
                static_cast<unsigned long long>(view.evaluations)
            );
            if (view.has_geometry) {
                ImGui::Text(
                    "Input: %u x %u | Crop: %u x %u at %u,%u",
                    view.render_width,
                    view.render_height,
                    view.crop.input_width,
                    view.crop.input_height,
                    view.crop.input_base_x,
                    view.crop.input_base_y
                );
                ImGui::Text(
                    "Output: %u x %u | Crop: %u x %u at %u,%u",
                    view.output_width,
                    view.output_height,
                    view.crop.output_width,
                    view.crop.output_height,
                    view.crop.output_base_x,
                    view.crop.output_base_y
                );
            } else {
                ImGui::TextDisabled("No foveated geometry recorded yet.");
            }
            ImGui::PopID();
        }
        if (views.empty()) {
            ImGui::TextDisabled("No view handles are currently active.");
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Interception details")) {
        if (DiagnosticTable table{"interception_details"}) {
            diagnostic_row(
                "NGX runtime loaded", "%s",
                data.runtime_loaded ? "Yes" : "No"
            );
            if (api == DiagnosticApi::d3d12) {
                diagnostic_row(
                    "Streamline detected", "%s",
                    data.streamline_detected ? "Yes" : "No"
                );
                diagnostic_row(
                    "Active NGX route", "%s",
                    d3d12_ngx_route_name(data.d3d12_ngx_route)
                );
            }
            diagnostic_row(
                "NGX exports discovered", "%s",
                data.hook_discovered ? "Yes" : "No"
            );
            diagnostic_row(
                "Direct export detour", "%s",
                data.direct_detour_installed ? "Installed" : "Not installed"
            );
            if (data.has_private_result) {
                diagnostic_row(
                    "Private foveated result", "0x%08X",
                    data.last_private_result
                );
                diagnostic_row(
                    "Final/fallback result", "0x%08X", data.last_result
                );
            } else {
                diagnostic_row("Last NGX result", "0x%08X", data.last_result);
            }
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void draw_resolution_performance(
    const char* const api_name,
    const DiagnosticSnapshot& data
) {
    ImGui::PushID(api_name);
    ImGui::SeparatorText("Resolution");
    if (DiagnosticTable table{"resolution"}) {
        diagnostic_row("Graphics API", "%s", api_name);
        diagnostic_row(
            "Original DLSS", "%u x %u -> %u x %u",
            data.received_input_width,
            data.received_input_height,
            data.received_output_width,
            data.received_output_height
        );
        const auto live_settings = current_settings();
        if (data.motion_vector_width != 0U &&
            data.motion_vector_height != 0U) {
            diagnostic_row(
                "Motion vectors", "%u x %u (%s)",
                data.motion_vector_width,
                data.motion_vector_height,
                motion_vector_space_name(data.motion_vector_space)
            );
            diagnostic_row(
                "Peripheral DLAA", "%s",
                !live_settings.peripheral_dlaa_enabled
                    ? "Disabled"
                    : data.motion_vector_space == MotionVectorSpace::output
                        ? "Enabled (auto MV conversion)"
                        : data.motion_vector_space == MotionVectorSpace::input
                            ? "Enabled (direct MVs)"
                            : "Enabled (compatibility unknown)"
            );
        } else {
            diagnostic_row("Motion vectors", "Not sampled yet");
            diagnostic_row(
                "Peripheral DLAA", "%s",
                live_settings.peripheral_dlaa_enabled
                    ? "Enabled (waiting for MV info)"
                    : "Disabled"
            );
        }
        if (data.passed_crop.input_width != 0U &&
            data.passed_crop.input_height != 0U) {
            diagnostic_row(
                "Foveated input", "%u x %u (%.1f%% of original) at %u,%u",
                data.passed_crop.input_width,
                data.passed_crop.input_height,
                pixel_percentage(
                    data.passed_crop.input_width,
                    data.passed_crop.input_height,
                    data.received_input_width,
                    data.received_input_height
                ),
                data.passed_crop.input_base_x,
                data.passed_crop.input_base_y
            );
            diagnostic_row(
                "Foveated output", "%u x %u (%.1f%% of original) at %u,%u",
                data.passed_crop.output_width,
                data.passed_crop.output_height,
                pixel_percentage(
                    data.passed_crop.output_width,
                    data.passed_crop.output_height,
                    data.received_output_width,
                    data.received_output_height
                ),
                data.passed_crop.output_base_x,
                data.passed_crop.output_base_y
            );
        } else {
            diagnostic_row("Foveated crop", "Not sampled yet");
        }
    }
    ImGui::PopID();
}

void draw_frame_performance(const DiagnosticSnapshot& data) {
    ImGui::SeparatorText("Frame Rate (250 ms average)");
    ImGui::TextDisabled(
        "Uses ReShade's FPS counter; keep the Home overlay open while sampling."
    );
    if (DiagnosticTable table{"frame_performance"}) {
        if (data.native_frame_ms > 0.0F) {
            diagnostic_row(
                "Non-foveated FPS", "%.1f FPS (%.2f ms)",
                1000.0F / data.native_frame_ms,
                data.native_frame_ms
            );
        } else {
            diagnostic_row("Non-foveated FPS", "Not sampled yet");
        }
        if (data.foveated_frame_ms > 0.0F) {
            diagnostic_row(
                "Foveated FPS", "%.1f FPS (%.2f ms)",
                1000.0F / data.foveated_frame_ms,
                data.foveated_frame_ms
            );
        } else {
            diagnostic_row("Foveated FPS", "Not sampled yet");
        }
        if (data.native_frame_ms > 0.0F &&
            data.foveated_frame_ms > 0.0F) {
            const auto native_fps = 1000.0F / data.native_frame_ms;
            const auto foveated_fps = 1000.0F / data.foveated_frame_ms;
            const auto fps_gain = foveated_fps - native_fps;
            diagnostic_row(
                "Foveated FPS gain", "%+.1f FPS (%+.1f%%)",
                fps_gain,
                fps_gain * 100.0F / native_fps
            );
            diagnostic_row(
                "Frame-time change", "%+.2f ms",
                data.foveated_frame_ms - data.native_frame_ms
            );
        } else {
            diagnostic_row("Foveated FPS gain", "Not sampled yet");
            diagnostic_row("Frame-time change", "Not sampled yet");
        }
    }
}

void draw_d3d11_gpu_performance(
    const DiagnosticSnapshot& data,
    const Settings& settings
) {
    ImGui::SeparatorText("DLSS GPU Timing (250 ms average)");
    if (DiagnosticTable table{"dlss_timing"}) {
        timing_row("Full DLSS call", data.native_dlss_gpu_ms);
        timing_row("Foveated DLSS call", data.foveated_dlss_gpu_ms);
        if (settings.peripheral_dlaa_enabled) {
            const bool direct =
                data.d3d11_execution_path == D3D11ExecutionPath::dx11_direct;
            if (direct) {
                timing_row(
                    "Peripheral preparation",
                    d3d11_peripheral_dlaa_preparation_gpu_ms()
                );
            }
            timing_row("Peripheral DLAA call", data.peripheral_dlaa_gpu_ms);
            if (direct) {
                timing_row(
                    "Peripheral prep + DLAA total",
                    d3d11_peripheral_dlaa_total_gpu_ms()
                );
            }
        }
        if (data.native_dlss_gpu_ms > 0.0F &&
            data.foveated_dlss_gpu_ms > 0.0F) {
            const auto savings =
                data.native_dlss_gpu_ms - data.foveated_dlss_gpu_ms;
            const auto savings_percent =
                savings * 100.0F / data.native_dlss_gpu_ms;
            diagnostic_row(
                "Foveated savings", "%.3f ms (%.1f%%)",
                savings, savings_percent
            );
        } else {
            diagnostic_row("Foveated savings", "Not sampled yet");
        }
    }

    if (!settings.d3d11_use_d3d12_transport) return;

    ImGui::SeparatorText("DX11 -> DX12 Transport");
    if (DiagnosticTable table{"transport_timing"}) {
        timing_row("Total time with transport", data.transport_gpu_ms);
        const auto nr_gpu_ms = settings.nr_foveated
            ? data.foveated_dlss_nr_gpu_ms
            : data.full_dlss_nr_gpu_ms;
        const auto peripheral_gpu_ms = settings.peripheral_dlaa_enabled
            ? data.peripheral_dlaa_gpu_ms
            : 0.0F;
        const bool has_nr_time = !settings.nr_enabled || nr_gpu_ms > 0.0F;
        const bool has_peripheral_time =
            !settings.peripheral_dlaa_enabled || peripheral_gpu_ms > 0.0F;
        if (data.transport_gpu_ms > 0.0F &&
            data.foveated_dlss_gpu_ms > 0.0F &&
            has_nr_time && has_peripheral_time) {
            diagnostic_row(
                "Transport overhead (excludes DLSS)", "%.3f ms",
                (std::max)(
                    0.0F,
                    data.transport_gpu_ms - data.foveated_dlss_gpu_ms -
                        peripheral_gpu_ms -
                        (settings.nr_enabled ? nr_gpu_ms : 0.0F)
                )
            );
        } else {
            diagnostic_row(
                "Transport overhead (excludes DLSS)", "Not sampled yet"
            );
        }
        diagnostic_row(
            "Transport status", "%s",
            d3d11_transport_status_name(data.d3d11_transport_status)
        );
    }
}

void draw_d3d12_gpu_performance(
    const DiagnosticSnapshot& data,
    const Settings& settings
) {
    if (!settings.peripheral_dlaa_enabled) return;
    ImGui::SeparatorText("DLSS GPU Timing (250 ms average)");
    if (DiagnosticTable table{"d3d12_dlss_timing"}) {
        timing_row("Peripheral DLAA call", data.peripheral_dlaa_gpu_ms);
    }
}

void draw_performance() {
    const auto& d3d11 = displayed_diagnostics(DiagnosticApi::d3d11);
    const auto& d3d12 = displayed_diagnostics(DiagnosticApi::d3d12);
    const bool d3d11_active = d3d11.evaluate_calls != 0U;
    const bool d3d12_active = d3d12.evaluate_calls != 0U;

    if (!d3d11_active && !d3d12_active) {
        ImGui::TextDisabled("Waiting for the first DLSS evaluation.");
        return;
    }

    if (d3d11_active) {
        draw_resolution_performance("Direct3D 11", d3d11);
    }
    if (d3d12_active) {
        draw_resolution_performance("Direct3D 12", d3d12);
    }

    // Frame timing is API-independent and is stored in the shared D3D11 slot.
    draw_frame_performance(d3d11);
    const auto settings = current_settings();
    if (d3d11_active) {
        draw_d3d11_gpu_performance(d3d11, settings);
    }
    if (d3d12_active) {
        draw_d3d12_gpu_performance(d3d12, settings);
    }
}

void draw_nr_performance() {
    const auto data = dlss_nr_snapshot();
    const auto& d3d11 = displayed_diagnostics(DiagnosticApi::d3d11);
    const auto& d3d12 = displayed_diagnostics(DiagnosticApi::d3d12);
    const auto& timing = data.route == DlssNrRoute::d3d11_transport
        ? d3d11
        : d3d12;
    ImGui::SeparatorText("Status");
    if (DiagnosticTable table{"nr_status"}) {
        diagnostic_row("State", "%s", dlss_nr_state_name(data.state));
        diagnostic_row("Current route", "%s", dlss_nr_route_name(data.route));
        diagnostic_row(
            "Calls", "Candidate %llu | Evaluated %llu | Failed %llu",
            static_cast<unsigned long long>(data.candidate_calls),
            static_cast<unsigned long long>(data.evaluation_calls),
            static_cast<unsigned long long>(data.failed_calls)
        );
        diagnostic_row("Last NGX result", "0x%08X", data.last_result);
    }

    ImGui::SeparatorText("Resolution");
    if (DiagnosticTable table{"nr_resolution"}) {
        if (data.output_width == 0U || data.output_height == 0U) {
            diagnostic_row("DLSS-SR output", "Not sampled yet");
            diagnostic_row("DLSS-NR region", "Not sampled yet");
            diagnostic_row("DLSS-NR working size", "Not sampled yet");
        } else {
            diagnostic_row(
                "DLSS-SR output", "%u x %u",
                data.output_width, data.output_height
            );
            diagnostic_row(
                "DLSS-NR region", "%u x %u (%.1f%%) at %u,%u",
                data.region_width,
                data.region_height,
                pixel_percentage(
                    data.region_width,
                    data.region_height,
                    data.output_width,
                    data.output_height
                ),
                data.region_base_x,
                data.region_base_y
            );
            diagnostic_row(
                "DLSS-NR working size", "%u x %u",
                data.working_width, data.working_height
            );
        }
        if (data.intermediate_vram_bytes != 0U) {
            diagnostic_row(
                "Intermediate VRAM", "%.1f MiB",
                static_cast<double>(data.intermediate_vram_bytes) /
                    (1024.0 * 1024.0)
            );
        } else {
            diagnostic_row("Intermediate VRAM", "Not allocated yet");
        }
    }

    ImGui::SeparatorText("DLSS-NR GPU Timing (250 ms average)");
    ImGui::TextDisabled(
        "Measured with D3D12 timestamps on the active processing path."
    );
    if (DiagnosticTable table{"nr_gpu_timing"}) {
        timing_row("Full DLSS-NR call", timing.full_dlss_nr_gpu_ms);
        timing_row(
            "Foveated DLSS-NR call", timing.foveated_dlss_nr_gpu_ms
        );
        if (timing.full_dlss_nr_gpu_ms > 0.0F &&
            timing.foveated_dlss_nr_gpu_ms > 0.0F) {
            const auto savings = timing.full_dlss_nr_gpu_ms -
                timing.foveated_dlss_nr_gpu_ms;
            diagnostic_row(
                "Foveated savings", "%.3f ms (%.1f%%)",
                savings,
                savings * 100.0F / timing.full_dlss_nr_gpu_ms
            );
        } else {
            diagnostic_row("Foveated savings", "Not sampled yet");
        }
    }
}

void load_settings_from_reshade() noexcept {
    auto settings = current_settings();
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "Enabled", settings.enabled
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "D3D11D3D12Transport",
        settings.d3d11_use_d3d12_transport
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "PeripheralDlaa",
        settings.peripheral_dlaa_enabled
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "PeripheralDlaaScale",
        settings.peripheral_dlaa_scale
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "CenterPreset",
        settings.center_preset
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "PeripheralDlaaPreset",
        settings.peripheral_dlaa_preset
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "Width", settings.width
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "Height", settings.height
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "XOffset", settings.x_offset
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "HeightOffset", settings.height_offset
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "InvertStereoXOffset",
        settings.invert_stereo_x_offset
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "Roundness", settings.roundness
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr,
        config_section,
        "TransitionWidth",
        settings.transition_width
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "AlignmentBorder",
        settings.alignment_border_enabled
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrEnabled", settings.nr_enabled
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrFoveated", settings.nr_foveated
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrUseSrFoveation",
        settings.nr_use_sr_foveation
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrAlignmentBorder",
        settings.nr_alignment_border_enabled
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrWidth", settings.nr_width
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrHeight", settings.nr_height
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrXOffset", settings.nr_x_offset
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrHeightOffset", settings.nr_height_offset
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrInvertStereoXOffset",
        settings.nr_invert_stereo_x_offset
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrRoundness", settings.nr_roundness
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrTransitionWidth",
        settings.nr_transition_width
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrWorkingScale", settings.nr_working_scale
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrPreset", settings.nr_preset
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrIntensity", settings.nr_intensity
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrLocalToneStrength",
        settings.nr_local_tone_strength
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrLocalStructureStrength",
        settings.nr_local_structure_strength
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrSkinStructureStrength",
        settings.nr_skin_structure_strength
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrAutomaticMask", settings.nr_automatic_mask
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrUiCorrection", settings.nr_ui_correction
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrPaperWhiteScale",
        settings.nr_paper_white_scale
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrHdrTransferStrength",
        settings.nr_hdr_transfer_strength
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrColorStrength", settings.nr_color_strength
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrDepthConvention",
        settings.nr_depth_convention
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrMotionScaleXMultiplier",
        settings.nr_motion_scale_x_multiplier
    ));
    static_cast<void>(reshade::get_config_value(
        nullptr, config_section, "NrMotionScaleYMultiplier",
        settings.nr_motion_scale_y_multiplier
    ));
    update_settings(settings);
}

void save_settings_to_reshade(const Settings& settings) noexcept {
    reshade::set_config_value(
        nullptr, config_section, "Enabled", settings.enabled
    );
    reshade::set_config_value(
        nullptr, config_section, "D3D11D3D12Transport",
        settings.d3d11_use_d3d12_transport
    );
    reshade::set_config_value(
        nullptr, config_section, "PeripheralDlaa",
        settings.peripheral_dlaa_enabled
    );
    reshade::set_config_value(
        nullptr, config_section, "PeripheralDlaaScale",
        settings.peripheral_dlaa_scale
    );
    reshade::set_config_value(
        nullptr, config_section, "CenterPreset",
        settings.center_preset
    );
    reshade::set_config_value(
        nullptr, config_section, "PeripheralDlaaPreset",
        settings.peripheral_dlaa_preset
    );
    reshade::set_config_value(
        nullptr, config_section, "Width", settings.width
    );
    reshade::set_config_value(
        nullptr, config_section, "Height", settings.height
    );
    reshade::set_config_value(
        nullptr, config_section, "XOffset", settings.x_offset
    );
    reshade::set_config_value(
        nullptr, config_section, "HeightOffset", settings.height_offset
    );
    reshade::set_config_value(
        nullptr, config_section, "InvertStereoXOffset",
        settings.invert_stereo_x_offset
    );
    reshade::set_config_value(
        nullptr, config_section, "Roundness", settings.roundness
    );
    reshade::set_config_value(
        nullptr,
        config_section,
        "TransitionWidth",
        settings.transition_width
    );
    reshade::set_config_value(
        nullptr, config_section, "AlignmentBorder",
        settings.alignment_border_enabled
    );
    reshade::set_config_value(
        nullptr, config_section, "NrEnabled", settings.nr_enabled
    );
    reshade::set_config_value(
        nullptr, config_section, "NrFoveated", settings.nr_foveated
    );
    reshade::set_config_value(
        nullptr, config_section, "NrUseSrFoveation",
        settings.nr_use_sr_foveation
    );
    reshade::set_config_value(
        nullptr, config_section, "NrAlignmentBorder",
        settings.nr_alignment_border_enabled
    );
    reshade::set_config_value(nullptr, config_section, "NrWidth", settings.nr_width);
    reshade::set_config_value(nullptr, config_section, "NrHeight", settings.nr_height);
    reshade::set_config_value(nullptr, config_section, "NrXOffset", settings.nr_x_offset);
    reshade::set_config_value(
        nullptr, config_section, "NrHeightOffset", settings.nr_height_offset
    );
    reshade::set_config_value(
        nullptr, config_section, "NrInvertStereoXOffset",
        settings.nr_invert_stereo_x_offset
    );
    reshade::set_config_value(
        nullptr, config_section, "NrRoundness", settings.nr_roundness
    );
    reshade::set_config_value(
        nullptr, config_section, "NrTransitionWidth",
        settings.nr_transition_width
    );
    reshade::set_config_value(
        nullptr, config_section, "NrWorkingScale", settings.nr_working_scale
    );
    reshade::set_config_value(nullptr, config_section, "NrPreset", settings.nr_preset);
    reshade::set_config_value(
        nullptr, config_section, "NrIntensity", settings.nr_intensity
    );
    reshade::set_config_value(
        nullptr, config_section, "NrLocalToneStrength",
        settings.nr_local_tone_strength
    );
    reshade::set_config_value(
        nullptr, config_section, "NrLocalStructureStrength",
        settings.nr_local_structure_strength
    );
    reshade::set_config_value(
        nullptr, config_section, "NrSkinStructureStrength",
        settings.nr_skin_structure_strength
    );
    reshade::set_config_value(
        nullptr, config_section, "NrAutomaticMask", settings.nr_automatic_mask
    );
    reshade::set_config_value(
        nullptr, config_section, "NrUiCorrection", settings.nr_ui_correction
    );
    reshade::set_config_value(
        nullptr, config_section, "NrPaperWhiteScale",
        settings.nr_paper_white_scale
    );
    reshade::set_config_value(
        nullptr, config_section, "NrHdrTransferStrength",
        settings.nr_hdr_transfer_strength
    );
    reshade::set_config_value(
        nullptr, config_section, "NrColorStrength", settings.nr_color_strength
    );
    reshade::set_config_value(
        nullptr, config_section, "NrDepthConvention",
        settings.nr_depth_convention
    );
    reshade::set_config_value(
        nullptr, config_section, "NrMotionScaleXMultiplier",
        settings.nr_motion_scale_x_multiplier
    );
    reshade::set_config_value(
        nullptr, config_section, "NrMotionScaleYMultiplier",
        settings.nr_motion_scale_y_multiplier
    );
}

void draw_sr_controls(Settings& settings, bool& changed) {
    static bool size_drafts_initialized{};
    static bool editing_width{};
    static bool editing_height{};
    static bool editing_peripheral_scale{};
    static float width_draft{};
    static float height_draft{};
    static float peripheral_scale_draft{};
    if (!size_drafts_initialized) {
        width_draft = settings.width;
        height_draft = settings.height;
        peripheral_scale_draft = settings.peripheral_dlaa_scale;
        size_drafts_initialized = true;
    }
    const auto preset_combo = [&changed](
        const char* const label,
        std::uint32_t& value,
        const bool allow_game_default
    ) {
        static constexpr std::uint32_t values[]{
            0U, 5U, 11U, 12U, 13U
        };
        static constexpr const char* labels[]{
            "Game/default",
            "E (Fastest)",
            "K",
            "L",
            "M",
        };
        const int first = allow_game_default ? 0 : 1;
        int selected{};
        const auto count = static_cast<int>(std::size(values));
        for (int index = first; index < count; ++index) {
            if (values[index] == value) {
                selected = index - first;
                break;
            }
        }
        if (ImGui::Combo(label, &selected, labels + first, count - first)) {
            value = values[first + selected];
            changed = true;
        }
    };
    changed |= ImGui::Checkbox("Enable foveated DLSS-SR", &settings.enabled);
    ImGui::SameLine();
    ImGui::TextDisabled("(Alt+Shift+/)");
    ImGui::BeginDisabled(!settings.enabled);
    preset_combo("Center preset", settings.center_preset, true);
    changed |= ImGui::Checkbox(
        "Peripheral DLAA",
        &settings.peripheral_dlaa_enabled
    );
    if (!editing_peripheral_scale) {
        peripheral_scale_draft = settings.peripheral_dlaa_scale;
    }
    ImGui::BeginDisabled(!settings.peripheral_dlaa_enabled);
    preset_combo(
        "Peripheral preset",
        settings.peripheral_dlaa_preset,
        false
    );
    if (ImGui::SliderFloat(
        "Periphery scale",
        &peripheral_scale_draft,
        0.20F,
        1.0F,
        "%.2f",
        ImGuiSliderFlags_AlwaysClamp
    )) {
        editing_peripheral_scale = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        settings.peripheral_dlaa_scale = peripheral_scale_draft;
        editing_peripheral_scale = false;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled(
        "Downscale periphery even more from original resolution"
    );
    if (!editing_width) width_draft = settings.width;
    if (ImGui::SliderFloat(
        "Fovea width",
        &width_draft,
        0.20F,
        1.0F,
        "%.2f",
        ImGuiSliderFlags_AlwaysClamp
    )) {
        editing_width = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        settings.width = width_draft;
        editing_width = false;
        changed = true;
    }
    if (!editing_height) height_draft = settings.height;
    if (ImGui::SliderFloat(
        "Fovea height",
        &height_draft,
        0.20F,
        1.0F,
        "%.2f",
        ImGuiSliderFlags_AlwaysClamp
    )) {
        editing_height = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        settings.height = height_draft;
        editing_height = false;
        changed = true;
    }
    if (has_multiple_stereo_views()) {
        ImGui::TextDisabled(
            "Applies equal and opposite X offsets to the two stereo views."
        );
        changed |= ImGui::SliderFloat(
            "Stereo X offset",
            &settings.x_offset,
            -1.0F,
            1.0F,
            "%.2f",
            ImGuiSliderFlags_AlwaysClamp
        );
        changed |= ImGui::Checkbox(
            "Invert stereo eye order",
            &settings.invert_stereo_x_offset
        );
    }
    changed |= ImGui::SliderFloat(
        "Height offset",
        &settings.height_offset,
        -1.0F,
        1.0F,
        "%.2f",
        ImGuiSliderFlags_AlwaysClamp
    );
    changed |= ImGui::SliderFloat(
        "Roundness",
        &settings.roundness,
        0.0F,
        1.0F,
        "%.2f",
        ImGuiSliderFlags_AlwaysClamp
    );
    changed |= ImGui::SliderFloat(
        "Transition width",
        &settings.transition_width,
        0.0F,
        0.30F,
        "%.3f",
        ImGuiSliderFlags_AlwaysClamp
    );
    changed |= ImGui::Checkbox(
        "Show 5 px red alignment border",
        &settings.alignment_border_enabled
    );
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (ImGui::Button("Reset DLSS-SR defaults", ImVec2(0.0F, 0.0F))) {
        const Settings defaults{};
        settings.enabled = defaults.enabled;
        settings.peripheral_dlaa_enabled = defaults.peripheral_dlaa_enabled;
        settings.peripheral_dlaa_scale = defaults.peripheral_dlaa_scale;
        settings.center_preset = defaults.center_preset;
        settings.peripheral_dlaa_preset = defaults.peripheral_dlaa_preset;
        peripheral_scale_draft = defaults.peripheral_dlaa_scale;
        editing_peripheral_scale = false;
        settings.width = defaults.width;
        settings.height = defaults.height;
        settings.x_offset = defaults.x_offset;
        settings.height_offset = defaults.height_offset;
        settings.invert_stereo_x_offset = defaults.invert_stereo_x_offset;
        settings.roundness = defaults.roundness;
        settings.transition_width = defaults.transition_width;
        settings.alignment_border_enabled = defaults.alignment_border_enabled;
        width_draft = defaults.width;
        height_draft = defaults.height;
        editing_width = false;
        editing_height = false;
        changed = true;
    }
}

void draw_nr_controls(Settings& settings, bool& changed) {
    struct GeometryDrafts {
        bool initialized{};
        bool editing_width{};
        bool editing_height{};
        bool editing_working_scale{};
        float width{};
        float height{};
        float working_scale{};
    };
    static GeometryDrafts drafts;
    if (!drafts.initialized) {
        drafts.width = settings.nr_width;
        drafts.height = settings.nr_height;
        drafts.working_scale = settings.nr_working_scale;
        drafts.initialized = true;
    }
    const auto deferred_slider = [&changed](
        const char* const label,
        float& committed,
        float& draft,
        bool& editing,
        const float minimum,
        const float maximum,
        const char* const format
    ) {
        if (!editing) draft = committed;
        if (ImGui::SliderFloat(
                label,
                &draft,
                minimum,
                maximum,
                format,
                ImGuiSliderFlags_AlwaysClamp
            )) {
            editing = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            committed = draft;
            editing = false;
            changed = true;
        }
    };

    changed |= ImGui::Checkbox("Foveated DLSS-NR", &settings.nr_foveated);
    if (settings.nr_foveated) {
        changed |= ImGui::Checkbox(
            "Use DLSS-SR foveation values",
            &settings.nr_use_sr_foveation
        );
        if (settings.nr_use_sr_foveation) {
            ImGui::TextDisabled(
                "Width, height, offsets, roundness, and transition follow DLSS-SR."
            );
        } else {
            deferred_slider(
                "NR fovea width",
                settings.nr_width,
                drafts.width,
                drafts.editing_width,
                0.20F,
                1.0F,
                "%.2f"
            );
            deferred_slider(
                "NR fovea height",
                settings.nr_height,
                drafts.height,
                drafts.editing_height,
                0.20F,
                1.0F,
                "%.2f"
            );
            if (has_multiple_stereo_views()) {
                ImGui::TextDisabled(
                    "Applies equal and opposite NR X offsets to both stereo views."
                );
                changed |= ImGui::SliderFloat(
                    "NR stereo X offset", &settings.nr_x_offset,
                    -1.0F, 1.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp
                );
                changed |= ImGui::Checkbox(
                    "Invert NR stereo eye order",
                    &settings.nr_invert_stereo_x_offset
                );
            }
            changed |= ImGui::SliderFloat(
                "NR height offset", &settings.nr_height_offset,
                -1.0F, 1.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp
            );
            changed |= ImGui::SliderFloat(
                "NR roundness", &settings.nr_roundness,
                0.0F, 1.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp
            );
            changed |= ImGui::SliderFloat(
                "NR transition width", &settings.nr_transition_width,
                0.0F, 0.30F, "%.3f", ImGuiSliderFlags_AlwaysClamp
            );
        }
    }
    changed |= ImGui::Checkbox(
        "Show 5 px green alignment border",
        &settings.nr_alignment_border_enabled
    );

    ImGui::SeparatorText("Neural rendering");
    deferred_slider(
        "Working scale",
        settings.nr_working_scale,
        drafts.working_scale,
        drafts.editing_working_scale,
        0.10F,
        1.0F,
        "%.2f"
    );
    int preset = static_cast<int>(settings.nr_preset);
    if (ImGui::Combo(
            "DLSS-NR preset",
            &preset,
            "Default\0Preset A\0Preset B\0Preset C\0Preset D\0Preset E\0Preset F\0Preset G\0"
        )) {
        settings.nr_preset = static_cast<std::uint32_t>(preset);
        changed = true;
    }
    changed |= ImGui::SliderFloat(
        "Intensity", &settings.nr_intensity,
        0.0F, 2.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp
    );
    if (ImGui::TreeNode("Advanced DLSS-NR tuning")) {
        changed |= ImGui::SliderFloat(
            "Local tone strength", &settings.nr_local_tone_strength,
            0.0F, 2.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp
        );
        changed |= ImGui::SliderFloat(
            "Local structure strength", &settings.nr_local_structure_strength,
            0.0F, 2.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp
        );
        changed |= ImGui::SliderFloat(
            "Skin structure strength", &settings.nr_skin_structure_strength,
            0.0F, 2.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp
        );
        changed |= ImGui::Checkbox(
            "Automatic mask", &settings.nr_automatic_mask
        );
        changed |= ImGui::Checkbox(
            "UI correction", &settings.nr_ui_correction
        );
        changed |= ImGui::SliderFloat(
            "Paper white scale", &settings.nr_paper_white_scale,
            0.01F, 8.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp
        );
        changed |= ImGui::SliderFloat(
            "HDR transfer strength", &settings.nr_hdr_transfer_strength,
            0.0F, 2.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp
        );
        changed |= ImGui::SliderFloat(
            "Color strength", &settings.nr_color_strength,
            0.0F, 2.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp
        );
        int depth = static_cast<int>(settings.nr_depth_convention);
        if (ImGui::Combo(
                "Depth convention", &depth,
                "Use game NGX flags\0Normal depth\0Reversed depth\0"
            )) {
            settings.nr_depth_convention = static_cast<std::uint32_t>(depth);
            changed = true;
        }
        changed |= ImGui::SliderFloat(
            "Motion scale X multiplier", &settings.nr_motion_scale_x_multiplier,
            -4.0F, 4.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp
        );
        changed |= ImGui::SliderFloat(
            "Motion scale Y multiplier", &settings.nr_motion_scale_y_multiplier,
            -4.0F, 4.0F, "%.2f", ImGuiSliderFlags_AlwaysClamp
        );
        ImGui::TreePop();
    }
    if (ImGui::Button("Reset DLSS-NR history / retry runtime")) {
        reset_dlss_nr();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset DLSS-NR defaults")) {
        const Settings defaults{};
        settings.nr_enabled = defaults.nr_enabled;
        settings.nr_foveated = defaults.nr_foveated;
        settings.nr_use_sr_foveation = defaults.nr_use_sr_foveation;
        settings.nr_alignment_border_enabled =
            defaults.nr_alignment_border_enabled;
        settings.nr_width = defaults.nr_width;
        settings.nr_height = defaults.nr_height;
        settings.nr_x_offset = defaults.nr_x_offset;
        settings.nr_height_offset = defaults.nr_height_offset;
        settings.nr_invert_stereo_x_offset = defaults.nr_invert_stereo_x_offset;
        settings.nr_roundness = defaults.nr_roundness;
        settings.nr_transition_width = defaults.nr_transition_width;
        settings.nr_working_scale = defaults.nr_working_scale;
        settings.nr_preset = defaults.nr_preset;
        settings.nr_intensity = defaults.nr_intensity;
        settings.nr_local_tone_strength = defaults.nr_local_tone_strength;
        settings.nr_local_structure_strength = defaults.nr_local_structure_strength;
        settings.nr_skin_structure_strength = defaults.nr_skin_structure_strength;
        settings.nr_automatic_mask = defaults.nr_automatic_mask;
        settings.nr_ui_correction = defaults.nr_ui_correction;
        settings.nr_paper_white_scale = defaults.nr_paper_white_scale;
        settings.nr_hdr_transfer_strength = defaults.nr_hdr_transfer_strength;
        settings.nr_color_strength = defaults.nr_color_strength;
        settings.nr_depth_convention = defaults.nr_depth_convention;
        settings.nr_motion_scale_x_multiplier =
            defaults.nr_motion_scale_x_multiplier;
        settings.nr_motion_scale_y_multiplier =
            defaults.nr_motion_scale_y_multiplier;
        drafts.width = defaults.nr_width;
        drafts.height = defaults.nr_height;
        drafts.working_scale = defaults.nr_working_scale;
        drafts.editing_width = false;
        drafts.editing_height = false;
        drafts.editing_working_scale = false;
        reset_dlss_nr();
        changed = true;
    }
}

void draw_settings_overlay(reshade::api::effect_runtime*) {
    auto settings = current_settings();
    diagnostic_note_frame_rate(
        ImGui::GetIO().Framerate,
        settings.enabled || settings.nr_enabled
    );
    bool changed{};

    ImGui::TextUnformatted("Changes apply live to the next DLSS evaluation.");
    ImGui::TextDisabled("DX12 Transport enables DX12 features for DX11 games.");
    int d3d11_path = settings.d3d11_use_d3d12_transport ? 1 : 0;
    if (ImGui::Combo(
            "DX11 game processing path",
            &d3d11_path,
            "DX11 Direct\0DX12 Transport\0"
        )) {
        settings.d3d11_use_d3d12_transport = d3d11_path == 1;
        changed = true;
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("DLSS-SR", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::TreeNodeEx(
                "Controls##dlss_sr",
                ImGuiTreeNodeFlags_DefaultOpen
            )) {
            draw_sr_controls(settings, changed);
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx(
                "Performance##dlss_sr",
                ImGuiTreeNodeFlags_DefaultOpen
            )) {
            draw_performance();
            ImGui::TreePop();
        }
    }

    if (ImGui::CollapsingHeader(
            "DLSS-NR (Experimental)", ImGuiTreeNodeFlags_DefaultOpen
        )) {
        changed |= ImGui::Checkbox(
            "Enable DLSS-NR (DLSS 5)", &settings.nr_enabled
        );
        ImGui::TextDisabled(
            "Requires nvngx_dlssnr.dll beside this add-on and a DX12 processing path."
        );
        if (settings.nr_enabled) {
            if (ImGui::TreeNodeEx(
                    "Controls##dlss_nr",
                    ImGuiTreeNodeFlags_DefaultOpen
                )) {
                draw_nr_controls(settings, changed);
                ImGui::TreePop();
            }
            if (ImGui::TreeNodeEx(
                    "Performance##dlss_nr",
                    ImGuiTreeNodeFlags_DefaultOpen
                )) {
                draw_nr_performance();
                ImGui::TreePop();
            }
        }
    }

    if (changed) {
        update_settings(settings);
        save_settings_to_reshade(settings);
    }

    if (ImGui::CollapsingHeader(
            "Diagnostics",
            ImGuiTreeNodeFlags_DefaultOpen
    )) {
        const auto& d3d11 = displayed_diagnostics(DiagnosticApi::d3d11);
        const auto& d3d12 = displayed_diagnostics(DiagnosticApi::d3d12);
        const bool d3d11_active = d3d11.evaluate_calls != 0U;
        const bool d3d12_active = d3d12.evaluate_calls != 0U;
        if (d3d11_active) {
            draw_api_diagnostics("Direct3D 11", DiagnosticApi::d3d11);
        }
        if (d3d11_active && d3d12_active) ImGui::Spacing();
        if (d3d12_active) {
            draw_api_diagnostics("Direct3D 12", DiagnosticApi::d3d12);
        }
        if (!d3d11_active && !d3d12_active) {
            ImGui::TextDisabled("Waiting for the first DLSS evaluation.");
        }
    }
}

void on_present(
    reshade::api::command_queue* const queue,
    reshade::api::swapchain*,
    const reshade::api::rect*,
    const reshade::api::rect*,
    std::uint32_t,
    const reshade::api::rect*
) {
    if (queue != nullptr && queue->get_device() != nullptr &&
        queue->get_device()->get_api() == reshade::api::device_api::d3d12) {
        note_d3d12_present(
            reinterpret_cast<ID3D12CommandQueue*>(queue->get_native())
        );
    }
    auto settings = current_settings();
    const bool down =
        (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 &&
        (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 &&
        (GetAsyncKeyState(VK_OEM_2) & 0x8000) != 0;
    const bool was_down = toggle_hotkey_down.exchange(
        down, std::memory_order_acq_rel
    );
    if (down && !was_down) {
        settings.enabled = !settings.enabled;
        update_settings(settings);
        save_settings_to_reshade(settings);
        trace_event(
            "Foveated DLSS hotkey Alt+Shift+/ toggled enabled=%s",
            settings.enabled ? "yes" : "no"
        );
    }
}

void on_execute_command_list(
    reshade::api::command_queue* const queue,
    reshade::api::command_list* const command_list
) {
    if (queue == nullptr || command_list == nullptr ||
        queue->get_device() == nullptr ||
        queue->get_device()->get_api() != reshade::api::device_api::d3d12) {
        return;
    }
    note_d3d12_command_list_submission(
        reinterpret_cast<ID3D12CommandQueue*>(queue->get_native()),
        reinterpret_cast<ID3D12GraphicsCommandList*>(command_list->get_native())
    );
}

}  // namespace

void set_addon_modules(
    const HMODULE addon,
    const HMODULE reshade
) noexcept {
    addon_module.store(addon, std::memory_order_release);
    reshade_log.store(
        reshade == nullptr
            ? nullptr
            : reinterpret_cast<LogMessageFn>(GetProcAddress(
                reshade,
                "ReShadeLogMessage"
            )),
        std::memory_order_release
    );

    std::array<wchar_t, MAX_PATH> path{};
    const auto length = GetModuleFileNameW(
        addon,
        path.data(),
        static_cast<DWORD>(path.size())
    );
    if (length != 0U && length < path.size()) {
        wchar_t* filename = path.data();
        for (DWORD index{}; index < length; ++index) {
            if (path[index] == L'\\' || path[index] == L'/') {
                filename = path.data() + index + 1U;
            }
        }
        constexpr wchar_t log_name[] = L"CheekyFoveatedDLSS.log";
        const auto prefix = static_cast<std::size_t>(filename - path.data());
        if (prefix + std::size(log_name) <= path.size()) {
            std::memcpy(
                filename,
                log_name,
                sizeof(log_name)
            );
            const auto handle = CreateFileW(
                path.data(),
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                nullptr
            );
            if (handle != INVALID_HANDLE_VALUE) {
                trace_log.store(handle, std::memory_order_release);
            }
        }
    }
}

void trace_event(const char* const format, ...) noexcept {
    if (format == nullptr) return;
    const auto handle = trace_log.load(std::memory_order_acquire);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return;

    std::array<char, 2048U> message{};
    va_list arguments;
    va_start(arguments, format);
    const auto message_length = std::vsnprintf(
        message.data(),
        message.size(),
        format,
        arguments
    );
    va_end(arguments);
    if (message_length < 0) return;

    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::array<char, 2304U> line{};
    const auto line_length = std::snprintf(
        line.data(),
        line.size(),
        "%02u:%02u:%02u.%03u [T%lu] %s\r\n",
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds,
        static_cast<unsigned long>(GetCurrentThreadId()),
        message.data()
    );
    if (line_length <= 0) return;
    const auto bytes = static_cast<DWORD>((std::min)(
        static_cast<std::size_t>(line_length),
        line.size() - 1U
    ));

    AcquireSRWLockExclusive(&trace_log_lock);
    DWORD written{};
    static_cast<void>(WriteFile(handle, line.data(), bytes, &written, nullptr));
    static_cast<void>(FlushFileBuffers(handle));
    ReleaseSRWLockExclusive(&trace_log_lock);
}

void close_trace_log() noexcept {
    const auto handle = trace_log.exchange(nullptr, std::memory_order_acq_rel);
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(handle);
        CloseHandle(handle);
    }
}

void log_message(const int level, const char* const message) noexcept {
    trace_event("ReShade log level=%d: %s", level, message == nullptr ? "" : message);
    const auto logger = reshade_log.load(std::memory_order_acquire);
    const auto module = addon_module.load(std::memory_order_acquire);
    if (logger != nullptr && module != nullptr && message != nullptr) {
        logger(module, level, message);
    }
}

void log_info(const char* const message) noexcept {
    log_message(3, message);
}

void log_warning(const char* const message) noexcept {
    log_message(2, message);
}

void log_error(const char* const message) noexcept {
    log_message(1, message);
}

}  // namespace cheeky::foveated_dlss

extern "C" __declspec(dllexport) const char* NAME = "Cheeky Foveated DLSS";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Configurable foveated DLSS Super Resolution and DLSS-NR for Direct3D 11 and 12.";

extern "C" __declspec(dllexport) bool AddonInit(
    const HMODULE addon,
    const HMODULE reshade
) {
    using namespace cheeky::foveated_dlss;
    set_addon_modules(addon, reshade);
    trace_event("AddonInit begin module=%p reshade=%p", addon, reshade);
    load_settings_from_reshade();
    if (!start_interception()) {
        log_error("Failed to start NGX interception.");
        return false;
    }
    reshade::register_overlay(nullptr, &draw_settings_overlay);
    reshade::register_event<reshade::addon_event::execute_command_list>(
        &on_execute_command_list
    );
    reshade::register_event<reshade::addon_event::present>(&on_present);
    log_info("Foveated DLSS-SR and DLSS-NR interception started for D3D11 and D3D12.");
    trace_event("AddonInit complete");
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(
    HMODULE,
    HMODULE
) {
    using namespace cheeky::foveated_dlss;
    reshade::unregister_event<reshade::addon_event::present>(&on_present);
    reshade::unregister_event<reshade::addon_event::execute_command_list>(
        &on_execute_command_list
    );
    reshade::unregister_overlay(nullptr, &draw_settings_overlay);
    stop_interception();
    release_d3d11_d3d12_transport();
    release_d3d11_resources();
    release_d3d12_resources();
    release_dlss_nr_resources();
    log_info("Foveated DLSS-SR and DLSS-NR interception stopped.");
    close_trace_log();
}

BOOL APIENTRY DllMain(
    const HMODULE module,
    const DWORD reason,
    LPVOID
) {
    using namespace cheeky::foveated_dlss;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (!reshade::register_addon(module)) return FALSE;
    } else if (reason == DLL_PROCESS_DETACH) {
        reshade::unregister_addon(module);
    }
    return TRUE;
}
