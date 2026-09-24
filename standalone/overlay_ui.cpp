#include "overlay_ui.hpp"
#include "settings_io.hpp"
#include "version.h"
#include "cheeky_gaze_abi.h"
#include <imgui.h>
#include <array>
#include <algorithm>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <locale>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <utility>
namespace cheeky::standalone {
using namespace cheeky::foveated_dlss;
// Snapshot is emitted by our runtime. This reader only extracts a direct
// member, respecting nesting and escaped strings so similarly named diagnostic
// values cannot accidentally become settings.
std::string_view member(std::string_view object, std::string_view name) {
    auto whitespace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t i = object.find('{');
    if (i == object.npos) return {};
    ++i;
    while (i < object.size()) {
        while (i < object.size() && (whitespace(object[i]) || object[i] == ',')) ++i;
        if (i == object.size() || object[i] != '"') return {};
        const auto key_start = ++i;
        while (i < object.size() && object[i] != '"') { if (object[i] == '\\') ++i; ++i; }
        if (i == object.size()) return {};
        const auto key = object.substr(key_start, i++ - key_start);
        while (i < object.size() && whitespace(object[i])) ++i;
        if (i == object.size() || object[i++] != ':') return {};
        while (i < object.size() && whitespace(object[i])) ++i;
        const auto start = i;
        int depth{}; bool quoted{}, escaped{};
        for (; i < object.size(); ++i) {
            const auto c = object[i];
            if (quoted) { if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == '"') quoted = false; continue; }
            if (c == '"') { quoted = true; continue; }
            if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') { if (!depth) break; --depth; }
            else if (c == ',' && !depth) break;
        }
        auto value = object.substr(start, i - start);
        while (!value.empty() && whitespace(value.back())) value.remove_suffix(1);
        if (key == name) return value;
        if (i == object.size() || object[i] == '}') return {};
    }
    return {};
}

std::string plain(std::string_view value) {
    if (value.size() < 2 || value.front() != '"') return std::string(value);
    std::string out;
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        if (value[i] == '\\' && i + 2 < value.size()) {
            const char c = value[++i];
            if (c == 'n') out += '\n'; else if (c == 't') out += '\t';
            else if (c == 'r') out += '\r'; else out += c;
        } else out += value[i];
    }
    return out;
}

void refresh(OverlayUiState& r, const OverlayRuntime& runtime, bool force = false) {
    const auto now = GetTickCount64();
    if (!runtime.snapshot || (!force && (now < r.next_snapshot || ImGui::IsAnyItemActive()))) return;
    std::array<char, 32768> buffer{};
    if (!runtime.snapshot(buffer.data(), static_cast<std::uint32_t>(buffer.size()))) {
        r.message = "Runtime snapshot unavailable"; return;
    }
    r.snapshot = buffer.data(); r.next_snapshot = now + 250;
    const auto settings = member(r.snapshot, "settings");
    if (!settings.empty()) {
        auto draft = r.draft;
        bool valid = true;
#define CHEEKY_SETTING(name, field) { const auto value = member(settings, name); if (!value.empty()) valid &= set_named_setting(draft, name, value); }
#include "settings_fields.inc"
#undef CHEEKY_SETTING
        if (valid) r.draft = draft;
    }
    r.message = plain(member(r.snapshot, "message"));
}

bool command(OverlayUiState& r, const OverlayRuntime& runtime, std::string_view action, std::string_view payload = {}) {
    if (!runtime.command || !runtime.attachment) return false;
    const auto text = "1\n" + std::to_string(++r.request) + "\n" + std::string(action) + "\n" + std::string(payload);
    const bool result = runtime.command(runtime.attachment, text.c_str());
    refresh(r, runtime, true);
    if (!result && r.message.empty()) r.message = "Runtime rejected the command";
    return result;
}

template<class T> auto scalar(T value) {
    if constexpr (std::is_enum_v<T>) return static_cast<std::uint32_t>(value);
    else return value;
}

void commit(OverlayUiState& r, const OverlayRuntime& runtime, const Settings& previous) {
    std::ostringstream out; out.imbue(std::locale::classic()); out.precision(9);
#define CHEEKY_SETTING(name, field) if (!std::string_view(name).starts_with("EyeCalibrationLearned") && previous.field != r.draft.field) out << name << '=' << scalar(r.draft.field) << '\n';
#include "settings_fields.inc"
#undef CHEEKY_SETTING
    const auto payload = out.str();
    if (!payload.empty()) command(r, runtime, "set", payload);
}

void slider(const char* label, float& value, float low, float high, const char* format = "%.2f") {
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##value", &value, low, high, format, ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopID();
}

template<class T> void combo(const char* label, T& value, const char* names) {
    int index = static_cast<int>(value);
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##value", &index, names)) value = static_cast<T>(index);
    ImGui::PopID();
}

void preset(const char* label, std::uint32_t& value, bool game_default) {
    constexpr std::uint32_t values[]{0,5,11,12,13};
    constexpr const char* names[]{"Game default", "E (fastest)", "K", "L", "M"};
    const int first = game_default ? 0 : 1;
    int selected{};
    for (int i = first; i < static_cast<int>(std::size(values)); ++i) if (values[i] == value) selected = i - first;
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##value", &selected, names + first, static_cast<int>(std::size(values)) - first)) value = values[first + selected];
    ImGui::PopID();
}

void rr_preset(const char* label, std::uint32_t& value) {
    constexpr unsigned values[]{0,4,5,6};
    constexpr const char* names[]{"Game default", "D", "E", "F"};
    int selected{};
    for (int i=0;i<4;++i) if (value==values[i]) selected=i;
    if (ImGui::Combo(label,&selected,names,4)) value=values[selected];
}
void draw_sr(Settings& s, bool rr) {
    ImGui::Checkbox("Enable foveated DLSS-SR", &s.enabled);
    ImGui::SeparatorText("Center quality");
    if (rr) { ImGui::TextUnformatted("Ray Reconstruction active"); rr_preset("Center RR preset",s.rr_center_preset); }
    else preset("Center preset", s.center_preset, true);
    slider("Center supersampling", s.center_supersampling, 1.0F, 2.0F, "%.2fx");
    ImGui::Checkbox("Peripheral DLAA", &s.peripheral_dlaa_enabled);
    if (s.peripheral_dlaa_enabled) {
        if (rr) rr_preset("Peripheral RR preset",s.rr_peripheral_preset);
        else preset("Peripheral preset", s.peripheral_dlaa_preset, false);
        slider("Periphery scale", s.peripheral_dlaa_scale, .2F, 1.0F);
    }
    if (rr) ImGui::TextWrapped("RR denoises both regions. Disabling peripheral DLAA uses input-resolution RR in the periphery.");
    ImGui::SeparatorText("Size and shape");
    slider("Fovea width", s.width, .2F, 1.0F);
    slider("Fovea height", s.height, .2F, 1.0F);
    slider("Roundness", s.roundness, 0.0F, 1.0F);
    slider("Transition width", s.transition_width, 0.0F, .3F, "%.3f");
    ImGui::Checkbox("Show red alignment border", &s.alignment_border_enabled);
}

void draw_gaze(Settings& s) {
    combo("Foveation center", s.center_mode, "Fixed\0Runtime gaze (OpenXR / OpenVR)\0Simulated gaze\0");
    ImGui::Checkbox("Automatic stereo alignment", &s.auto_stereo_alignment);
    if (s.center_mode == FoveationCenterMode::openxr_gaze)
        ImGui::TextWrapped("Runtime gaze needs the Cheeky OpenXR layer or a supported OpenVR runtime. Fixed placement is used when tracking is unavailable.");
    if (!s.auto_stereo_alignment && s.center_mode == FoveationCenterMode::fixed)
        slider("Stereo X offset", s.x_offset, -1.0F, 1.0F);
    slider(s.center_mode == FoveationCenterMode::fixed ? "Height offset" : "Fallback height offset",
        s.auto_stereo_alignment ? s.aligned_height_offset : s.height_offset, -1.0F, 1.0F);
    if (ImGui::TreeNode("Stereo mapping override")) {
        ImGui::Checkbox("Invert stereo eye order", &s.invert_stereo_x_offset);
        ImGui::TextWrapped("For packed layouts with reversed eye order; normally leave off.");
        ImGui::TreePop();
    }
    if (s.center_mode == FoveationCenterMode::simulated_gaze) {
        combo("Simulation pattern", s.simulation_pattern, "Figure eight (8 s)\0Slow sweep (20 s)\0Jump every 2 s\0Jump every 8 s\0Tracking loss\0Hold center\0");
        if (s.simulation_pattern == 2 || s.simulation_pattern == 3)
            ImGui::Checkbox("Show next jump target", &s.show_next_jump_target);
    }
    if (s.center_mode != FoveationCenterMode::fixed && ImGui::TreeNode("Advanced eye tracking")) {
        slider("Gaze smoothing", s.gaze_smoothing_ms, 0.0F, 100.0F, "%.0f ms");
        int pixels = static_cast<int>(s.gaze_quantization_pixels);
        ImGui::TextUnformatted("Crop origin quantization");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##quantization", &pixels, 1, 64, "%d px", ImGuiSliderFlags_AlwaysClamp)) s.gaze_quantization_pixels = static_cast<std::uint32_t>(pixels);
        slider("Jump reset threshold", s.gaze_jump_reset_ratio, .01F, 1.0F, "%.3f crop");
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("AFW coverage")) {
        ImGui::Checkbox("Automatic AFW coverage", &s.afw_automatic_coverage);
        ImGui::Checkbox("Manual AFW coverage", &s.afw_manual_coverage);
        if (s.afw_automatic_coverage || s.afw_manual_coverage)
            slider("AFW warp margin", s.afw_warp_margin, 0.0F, .25F, "%.3f");
        ImGui::TreePop();
    }
}

void draw_nr(Settings& s) {
    ImGui::Checkbox("Enable DLSS-NR", &s.nr_enabled);
    ImGui::SeparatorText("Size and shape");
    ImGui::Checkbox("Foveated DLSS-NR", &s.nr_foveated);
    if (s.nr_foveated) {
        ImGui::Checkbox("Use DLSS-SR size and shape", &s.nr_use_sr_foveation);
        if (s.nr_use_sr_foveation) {
            ImGui::TextWrapped("Width, height, roundness and transition follow the DLSS-SR settings, even with SR disabled.");
        } else {
            slider("NR fovea width", s.nr_width, .2F, 1.0F);
            slider("NR fovea height", s.nr_height, .2F, 1.0F);
            slider("NR roundness", s.nr_roundness, 0.0F, 1.0F);
            slider("NR transition width", s.nr_transition_width, 0.0F, .3F, "%.3f");
        }
        ImGui::Checkbox("Show green alignment border", &s.nr_alignment_border_enabled);
    }
    ImGui::SeparatorText("Neural rendering");
    combo("Rendering order", s.nr_processing_order, "After upscaling\0Before upscaling (experimental)\0");
    slider("Working scale", s.nr_working_scale, .1F, 1.0F);
    combo("DLSS-NR style", s.nr_style, "Standard\0Natural\0Cinematic\0");
    slider("Intensity", s.nr_intensity, 0.0F, 1.0F);
    if (ImGui::TreeNode("Advanced neural rendering")) {
        slider("Local tone strength", s.nr_local_tone_strength, 0.0F, 2.0F);
        slider("Local structure strength", s.nr_local_structure_strength, 0.0F, 2.0F);
        ImGui::Checkbox("Automatic mask", &s.nr_automatic_mask);
        if (s.nr_automatic_mask)
            slider("Skin structure strength", s.nr_skin_structure_strength, 0.0F, 2.0F);
        ImGui::Checkbox("UI correction", &s.nr_ui_correction);
        slider("Paper white scale", s.nr_paper_white_scale, .01F, 8.0F);
        slider("HDR transfer strength", s.nr_hdr_transfer_strength, 0.0F, 2.0F);
        slider("Color strength", s.nr_color_strength, 0.0F, 2.0F);
        combo("Depth convention", s.nr_depth_convention, "Game NGX flags\0Normal depth\0Reversed depth\0");
        slider("Motion scale X multiplier", s.nr_motion_scale_x_multiplier, -4.0F, 4.0F);
        slider("Motion scale Y multiplier", s.nr_motion_scale_y_multiplier, -4.0F, 4.0F);
        ImGui::TreePop();
    }
}

void diagnostic_line(std::string_view object, const char* label, const char* key) {
    auto value = plain(member(object, key));
    if (value == "true") value = "Yes";
    else if (value == "false") value = "No";
    ImGui::TextWrapped("%s: %s", label, value.empty() ? "unavailable" : value.c_str());
}

// Arrays contain nested objects (including strings with braces). Never split
// on commas: crop and per-eye diagnostics have their own members and arrays.
std::string_view array_object(std::string_view array, unsigned index) {
    int depth{};
    bool quoted{}, escaped{};
    std::size_t start{};
    for (std::size_t i = 0; i < array.size(); ++i) {
        const char c = array[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') quoted = true;
        else if (c == '{') { if (depth++ == 0) start = i; }
        else if (c == '}' && depth > 0 && --depth == 0) {
            if (index-- == 0) return array.substr(start, i - start + 1);
        }
    }
    return {};
}

double number(std::string_view object, const char* key) {
    const auto text = member(object, key);
    double result{};
    if (text.empty()) return 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(result) ? result : 0;
}

bool flag(std::string_view object, const char* key) { return member(object, key) == "true"; }

const char* gaze_warning(const Settings& s, std::string_view snapshot) {
    if (s.center_mode != FoveationCenterMode::openxr_gaze ||
        (!s.enabled && !(s.nr_enabled && s.nr_foveated))) return nullptr;
    const auto gaze = member(snapshot, "gaze");
    if (gaze.empty()) return "Waiting for eye-tracking diagnostics.";
    if (!flag(gaze, "layer")) return "Eye tracking unavailable: no active OpenXR layer or supported OpenVR adapter. Using fixed placement.";
    if (!flag(gaze, "abi")) return "Eye tracking unavailable: update the OpenXR layer to match this runtime. Using fixed placement.";
    const auto flags = static_cast<unsigned>(number(gaze, "status_flags"));
    if (!(flags & CHEEKY_GAZE_STATUS_SYSTEM_SUPPORTED)) return "Eye tracking not detected. Using fixed placement.";
    if (flags & CHEEKY_GAZE_STATUS_UNSUPPORTED_VIEW_CONFIG) return "Eye tracking unavailable for this stereo layout. Using fixed placement.";
    if (!(flags & CHEEKY_GAZE_STATUS_SESSION_FOCUSED)) return "VR session is not focused. Using fixed fallback.";
    if (!(flags & CHEEKY_GAZE_STATUS_GAZE_VALID)) return "No valid eye-tracking signal. Using fixed fallback.";
    if (!flag(gaze, "using_gaze")) {
        if (flag(gaze, "ambiguous")) return "Eye mapping is ambiguous. Waiting for a reliable left/right eye assignment.";
        return "Waiting for a fresh eye-tracking sample or stable eye mapping.";
    }
    return nullptr;
}

const char* alignment_warning(const Settings& s, std::string_view snapshot) {
    if (!s.auto_stereo_alignment || (!s.enabled && !(s.nr_enabled && s.nr_foveated))) return nullptr;
    const auto gaze = member(snapshot, "gaze");
    if (gaze.empty()) return "Waiting for automatic stereo alignment diagnostics.";
    // Fixed foveation in an ordinary flat game does not require stereo data.
    if (number(gaze, "views") < 2 && !flag(gaze, "layer") && s.center_mode == FoveationCenterMode::fixed)
        return nullptr;
    // Projection alignment can work without an eye tracker, and calibrated
    // mappings can resolve ambiguous runtime resources. Trust the used source.
    if (number(gaze, "alignment") != 0) return nullptr;
    return "Automatic stereo alignment has no usable projection or eye mapping. Using manual fallback placement.";
}

void warning(const char* text) {
    if (!text) return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, .65F, .25F, 1.0F));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

const char* alignment_name(unsigned source) {
    switch (source) {
    case 1: return "Streamline projection";
    case 2: return "OpenXR";
    case 3: return "OpenVR";
    default: return "Manual fallback";
    }
}

void draw_gaze_status(const Settings& s, std::string_view snapshot) {
    const auto gaze = member(snapshot, "gaze");
    ImGui::SeparatorText("Tracking and alignment status");
    if (s.center_mode == FoveationCenterMode::fixed)
        ImGui::TextWrapped("Eye Tracking Ready: Not in use (fixed placement selected).");
    else if (s.center_mode == FoveationCenterMode::simulated_gaze)
        ImGui::TextWrapped("Eye Tracking Ready: Not in use (simulated gaze selected).");
    else if (!s.enabled && !(s.nr_enabled && s.nr_foveated))
        ImGui::TextWrapped("Eye Tracking Ready: Not evaluated (SR and foveated NR are off).");
    else if (const auto reason = gaze_warning(s, snapshot)) {
        ImGui::TextUnformatted("Eye Tracking Ready: No");
        warning(reason);
    } else ImGui::TextUnformatted("Eye Tracking Ready: Yes");
    ImGui::TextWrapped("Latest alignment: %s", alignment_name(static_cast<unsigned>(number(gaze, "alignment"))));
    warning(alignment_warning(s, snapshot));
}

void draw_gaze_details(std::string_view snapshot) {
    const auto gaze = member(snapshot, "gaze");
    if (ImGui::TreeNode("Eye tracking details")) {
        diagnostic_line(gaze, "Runtime", "runtime");
        diagnostic_line(gaze, "Runtime adapter loaded", "layer");
        diagnostic_line(gaze, "Compatible gaze ABI", "abi");
        diagnostic_line(gaze, "Mapping ambiguity", "ambiguous");
        diagnostic_line(gaze, "Active stereo views", "views");
        diagnostic_line(gaze, "Sample age (ms)", "age_ms");
        const auto flags = static_cast<unsigned>(number(gaze, "status_flags"));
        for (const auto& item : {std::pair{"System supports eye tracking", CHEEKY_GAZE_STATUS_SYSTEM_SUPPORTED},
                 {"Session focused", CHEEKY_GAZE_STATUS_SESSION_FOCUSED},
                 {"Gaze input active", CHEEKY_GAZE_STATUS_ACTION_ACTIVE},
                 {"Tracking valid", CHEEKY_GAZE_STATUS_GAZE_VALID},
                 {"Submission mapping ready", CHEEKY_GAZE_STATUS_MAPPING_READY}})
            ImGui::TextWrapped("%s: %s", item.first, gaze.empty() ? "unavailable" : (flags & item.second) ? "Yes" : "No");
        for (unsigned i = 0; i < 2; ++i) {
            const auto eye = array_object(member(gaze, "eyes"), i);
            ImGui::SeparatorText(i ? "Right eye" : "Left eye");
            diagnostic_line(eye, "Mapped", "mapped");
            diagnostic_line(eye, "DLSS view", "view_id");
            diagnostic_line(eye, "Stable matches", "stable_matches");
            ImGui::TextWrapped("Alignment: %s", alignment_name(static_cast<unsigned>(number(eye, "alignment"))));
            ImGui::Text("Aligned center: %.4f, %.4f", number(eye, "aligned_u"), number(eye, "aligned_v"));
            if (flags & CHEEKY_GAZE_STATUS_GAZE_VALID)
                ImGui::Text("Gaze center: %.4f, %.4f", number(eye, "center_u"), number(eye, "center_v"));
            ImGui::Text("Crop delta: %.0f, %.0f px", number(eye, "delta_x"), number(eye, "delta_y"));
        }
        ImGui::TreePop();
    }
}

void draw_calibration_controls(OverlayUiState& r, const OverlayRuntime& runtime) {
    ImGui::SeparatorText("Eye calibration");
    bool enabled = flag(member(r.snapshot, "eye_calibration"), "enabled");
    if (ImGui::Checkbox("Automatic eye calibration (this session)", &enabled))
        command(r, runtime, enabled ? "calibration_enable" : "calibration_disable");
    ImGui::BeginDisabled(!enabled);
    int method = int(r.draft.eye_calibration_method);
    if (ImGui::Combo("Calibration method", &method,
            "Auto\0Standard corners\0Timing tolerant corners\0Full crop search\0"))
        r.draft.eye_calibration_method = static_cast<EyeCalibrationMethod>(method);
    const auto learned = unsigned(number(member(r.snapshot, "settings"), "EyeCalibrationLearnedMethod"));
    ImGui::Text("Learned starting method: %s", learned ? eye_calibration_method_name(static_cast<EyeCalibrationMethod>(learned)) : "Not learned yet");
    if (learned == 2 && number(member(r.snapshot, "settings"), "EyeCalibrationLearnedSessions") < 2)
        ImGui::TextWrapped("Timing preference needs confirmation on another launch; Auto will start with standard corners.");
    diagnostic_line(member(r.snapshot, "eye_calibration"), "Active method", "active_method");
    if (ImGui::Button("Reset learned calibration method")) command(r, runtime, "calibration_forget");
    if (r.draft.eye_calibration_method == EyeCalibrationMethod::full ||
        (r.draft.eye_calibration_method == EyeCalibrationMethod::automatic &&
         plain(member(member(r.snapshot, "eye_calibration"), "active_method")) == "Full crop search")) {
        int mode = r.draft.eye_calibration_continuous ? 0 : 1;
        if (ImGui::Combo("Recalibration", &mode,
                "Continuously validate\0Only on view or dimension changes\0"))
            r.draft.eye_calibration_continuous = mode == 0;
        if (!r.draft.eye_calibration_continuous)
            ImGui::TextWrapped("Keeps the learned alignment without validation markers until views, dimensions, submission bounds, or the VR session change. Eye swaps or image crop changes within unchanged views are not detected.");
    }
    if (ImGui::Button("Recalibrate now")) command(r, runtime, "calibration_recalibrate");
    ImGui::EndDisabled();
}

void draw_calibration(OverlayUiState& r, const OverlayRuntime& runtime) {
    if (!ImGui::TreeNode("Eye calibration diagnostics")) return;
    // Commands refresh r.snapshot, so consume all views before sending one.
    const auto data = member(r.snapshot, "eye_calibration");
    diagnostic_line(data, "Backend", "backend");
    const auto api = number(data, "graphics_api");
    ImGui::Text("Graphics API: %s", api == 12 ? "D3D12" : api == 11 ? "D3D11" : "Waiting for DLSS");
    diagnostic_line(data, "Status", "status");
    diagnostic_line(data, "Corrections applied", "corrections");
    diagnostic_line(data, "Confirmed mapping updates", "applied");
    ImGui::Text("Valid / completed samples: %.0f / %.0f", number(data, "valid"), number(data, "completed"));
    ImGui::Text("Skipped / in flight: %.0f / %.0f", number(data, "skipped"), number(data, "in_flight"));
    ImGui::Text("Full calibration attempts / successes: %.0f / %.0f",
        number(data,"full_calibration_attempts"),number(data,"full_calibration_successes"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Attempts count full stereo capture cycles, including retries; successes count accepted full crop acquisitions. Counts reset with calibration counters.");
    ImGui::Text("Recalibration requests: %.0f; verification failures: %.0f / %.0f",
        number(data,"recalibration_requests"),number(data,"verification_failure_streak"),number(member(data,"placement_search"),"verification_failure_limit"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Requests count transitions from a locked mapping back to full search. Verification failures are counted samples, normally one per 10 VR frames; motion-excused misses reset the streak. Ambiguity or source/geometry changes can trigger immediately.");
    diagnostic_line(data,"Last full calibration reason","full_calibration_reason");
    const auto last_failure=member(data,"last_rejection");
    diagnostic_line(last_failure,"Last marker failure","marker_failure_detail");
    if (number(last_failure,"sequence")>0 && ImGui::TreeNode("Last failure details")) {
        ImGui::Text("Sample %.0f; DLSS evaluations %.0f; source mask %.0f",
            number(last_failure,"sequence"),number(last_failure,"evaluations"),number(last_failure,"source_mask"));
        diagnostic_line(last_failure,"Shared source assumed","shared_source_assumed");
        diagnostic_line(last_failure,"Motion unreliable","motion_unreliable");
        ImGui::Text("Measured marker shift: %.1f / %.1f px (X / Y)",
            number(last_failure,"marker_error_x_px"),number(last_failure,"marker_error_y_px"));
        const auto patches=member(last_failure,"patch_search_diagnostics");
        for(unsigned i=4;i<12;++i) {
            const auto patch=array_object(patches,i);
            if(number(patch,"positions")<=0) continue;
            ImGui::Text("Patch %u: best sampled score %.3f; bits %.0f/25; contrast %.3f",i,
                number(patch,"best_sampled_score"),number(patch,"bits_at_best_score"),number(patch,"contrast_at_best_score"));
            ImGui::Text("  Positions %.0f; best coarse bits %.0f/25; low contrast %.0f",
                number(patch,"positions"),number(patch,"best_coarse_bits"),number(patch,"low_contrast_positions"));
        }
        ImGui::TextWrapped("Scores are sampled candidates, not exhaustive maxima. -1 means no candidate was scored. A high score alone does not establish the correct eye pair or crop. Shift values stay zero when recognition failed before geometry could be checked.");
        ImGui::TreePop();
    }
    ImGui::Text("CPU work: %.2f us/frame", number(data, "cpu_us_per_frame"));
    ImGui::Text("Verification worker: %.2f us/frame (background)",number(data,"verification_cpu_us_per_frame"));
    ImGui::Text("Verification worker (last / peak sample): %.2f / %.2f ms",
        number(data,"verification_last_ms"),number(data,"verification_peak_ms"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Worker elapsed time for completed local searches, including failures and patch conversion. Separate from graphics-thread CPU work; includes worker scheduling delays.");
    ImGui::Text("Verification attempts (exact / nearby / broad): %.0f / %.0f / %.0f",
        number(data,"verification_exact"),number(data,"verification_nearby"),number(data,"verification_broad"));
    if (number(data, "capture_timing_samples") > 0) {
        ImGui::Text("Image acquisition total (L / R): %.1f / %.1f ms", number(data,"capture_total_left_ms"),number(data,"capture_total_right_ms"));
        ImGui::Text("Capture allocation / record (L / R): %.1f / %.1f ms", number(data,"capture_setup_left_ms"),number(data,"capture_setup_right_ms"));
        ImGui::Text("Readback elapsed (L / R): %.1f / %.1f ms", number(data,"capture_wait_left_ms"),number(data,"capture_wait_right_ms"));
        ImGui::Text("Map calls CPU (L / R): %.1f / %.1f ms", number(data,"capture_map_left_ms"),number(data,"capture_map_right_ms"));
        ImGui::Text("CPU image allocation / copy (L / R): %.1f / %.1f ms", number(data,"capture_copy_left_ms"),number(data,"capture_copy_right_ms"));
        ImGui::Text("Image acquisition peak: %.1f ms",number(data,"max_capture_ms"));
        ImGui::Text("Peak capture: eye %s, %.0fx%.0f, sample %.0f, %.0f Map polls",
            number(data,"peak_capture_eye")==0 ? "L" : "R", number(data,"peak_capture_width"),
            number(data,"peak_capture_height"),number(data,"peak_capture_sequence"),number(data,"peak_capture_map_polls"));
        ImGui::Text("At peak - allocation / record: %.1f ms; readback: %.1f ms",
            number(data,"peak_capture_setup_ms"),number(data,"peak_capture_wait_ms"));
        ImGui::Text("At peak - CPU copy: %.1f ms; other: %.1f ms; Map CPU: %.1f ms",
            number(data,"peak_capture_copy_ms"),number(data,"peak_capture_other_ms"),number(data,"peak_capture_map_ms"));
        ImGui::TextWrapped("Readback elapsed includes GPU queue and polling delay; Map CPU overlaps it. -1 means unavailable.");
    }
    if (number(data, "search_timing_samples") > 0) {
        ImGui::Text("Full-image search (last L / R): %.1f / %.1f ms",
            number(data, "search_left_ms"), number(data, "search_right_ms"));
        ImGui::Text("Full-image search (peak per eye): %.1f ms", number(data, "max_search_ms"));
    } else ImGui::TextUnformatted("Full-image search: no completed timing yet");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Worker elapsed time per eye, including unsuccessful searches. Eyes run concurrently; do not add their times. Excludes image readback/conversion and final verification. -1 means unavailable.");
    if (number(data, "gpu_samples") > 0) ImGui::Text("GPU marker / copy work: %.2f us", number(data, "gpu_us"));
    else diagnostic_line(data, "GPU marker / copy work", "gpu_timing_status");
    ImGui::Text("Readback latency: %.2f VR frames", number(data, "latency_frames"));
    diagnostic_line(data, "Last recognized left view", "left_view");
    diagnostic_line(data, "Last recognized right view", "right_view");
    ImGui::TextWrapped("Corner verification samples every 10 VR frames; timing recovery captures every frame while acquiring. Corrections count changes to an existing eye assignment. GPU time covers marker and copy commands; CPU time excludes lock waiting.");
    if (ImGui::Button("Reset calibration counters")) command(r, runtime, "calibration_reset");
    ImGui::TreePop();
}

void timing_line(std::string_view data, const char* label, const char* key) {
    const auto ms = number(data, key);
    if (ms > 0) ImGui::TextWrapped("%s: %.3f ms", label, ms);
    else ImGui::TextWrapped("%s: Not sampled yet", label);
}

void savings_line(std::string_view data, const char* full, const char* foveated) {
    const auto baseline = number(data, full), optimized = number(data, foveated);
    if (baseline > 0 && optimized > 0)
        ImGui::TextWrapped("Foveated savings: %.3f ms (%.1f%%)", baseline - optimized, (baseline - optimized) * 100 / baseline);
    else ImGui::TextWrapped("Foveated savings: Sample both full and foveated modes to compare.");
}

void resolution_line(std::string_view data, const char* label, const char* width, const char* height) {
    const auto w = number(data, width), h = number(data, height);
    if (w > 0 && h > 0) ImGui::TextWrapped("%s: %.0f x %.0f", label, w, h);
    else ImGui::TextWrapped("%s: Not sampled yet", label);
}

void region_line(std::string_view region, std::string_view original, const char* label,
    const char* width, const char* height, const char* x, const char* y,
    const char* original_width, const char* original_height) {
    const auto w = number(region, width), h = number(region, height);
    const auto ow = number(original, original_width), oh = number(original, original_height);
    if (w > 0 && h > 0 && ow > 0 && oh > 0)
        ImGui::TextWrapped("%s: %.0f x %.0f (%.1f%% of original) at %.0f,%.0f",
            label, w, h, 100 * w * h / (ow * oh), number(region, x), number(region, y));
    else resolution_line(region, label, width, height);
}

void draw_sr_performance(std::string_view snapshot, const Settings& s) {
    ImGui::SeparatorText("Frame rate (250 ms average)");
    const auto frame = member(snapshot, "frame");
    for (const auto& item : {std::pair{"Current presentation", "present_ms"},
             {"Non-foveated FPS (SR off)", "sr_disabled_ms"}, {"Foveated FPS (SR on)", "sr_enabled_ms"}}) {
        const auto ms = number(frame, item.second);
        if (ms > 0) ImGui::TextWrapped("%s: %.1f FPS (%.2f ms)", item.first, 1000 / ms, ms);
        else ImGui::TextWrapped("%s: Not sampled yet", item.first);
    }
    const auto native_ms = number(frame, "sr_disabled_ms"), foveated_ms = number(frame, "sr_enabled_ms");
    if (native_ms > 0 && foveated_ms > 0) {
        // Match the addon's comparison: FPS gain is relative to SR off.
        // It is different from the percentage reduction in frame/GPU time.
        const auto native_fps = 1000 / native_ms, foveated_fps = 1000 / foveated_ms;
        const auto gain = foveated_fps - native_fps;
        ImGui::TextWrapped("Foveated FPS gain: %+.1f FPS (%+.1f%%)", gain, 100 * gain / native_fps);
        ImGui::TextWrapped("Frame-time change: %+.2f ms (%+.1f%%)",
            foveated_ms - native_ms, 100 * (foveated_ms - native_ms) / native_ms);
    } else {
        ImGui::TextWrapped("Foveated FPS gain: Not sampled yet");
        ImGui::TextWrapped("Frame-time change: Not sampled yet");
        ImGui::TextWrapped("Sample SR both on and off for about two seconds each to compare. Keep the scene and other settings the same.");
    }
    ImGui::TextWrapped("Frame comparisons use presentation cadence and retain the last sample from each mode.");
    ImGui::TextWrapped("GPU timings are 250 ms averages. Comparisons retain the last sampled value for each mode.");
    bool active{};
    for (unsigned i = 0; i < 2; ++i) {
        const auto api = array_object(member(snapshot, "apis"), i);
        if (number(api, "evaluations") == 0) continue;
        active = true;
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::CollapsingHeader(i ? "Direct3D 12" : "Direct3D 11", ImGuiTreeNodeFlags_DefaultOpen)) {
            diagnostic_line(api, "State", "state");
            resolution_line(api, "DLSS input", "input_width", "input_height");
            resolution_line(api, "DLSS output", "output_width", "output_height");
            region_line(member(api, "crop"), api, "Center input", "input_width", "input_height",
                "input_x", "input_y", "input_width", "input_height");
            region_line(member(api, "crop"), api, "Center output", "output_width", "output_height",
                "output_x", "output_y", "output_width", "output_height");
            resolution_line(api, "Motion vectors", "motion_width", "motion_height");
            diagnostic_line(api, "Motion-vector space", "motion_space");
            const auto motion_space = plain(member(api, "motion_space"));
            ImGui::TextWrapped("Peripheral DLAA: %s", !s.peripheral_dlaa_enabled ? "Disabled" :
                motion_space == "Output-resolution" ? "Enabled (auto MV conversion)" :
                motion_space == "Input-resolution" ? "Enabled (direct MVs)" : "Enabled (waiting for compatible MV info)");
            ImGui::SeparatorText("DLSS-SR GPU timing");
            timing_line(api, "Full DLSS call", "native_ms");
            if (s.enabled && number(api, "native_ms") <= 0)
                ImGui::TextWrapped("Turn off foveated DLSS-SR briefly to sample the full-frame baseline.");
            timing_line(api, "Foveated DLSS call", "foveated_ms");
            if (s.peripheral_dlaa_enabled) {
                timing_line(api, "Peripheral DLAA call", "peripheral_ms");
                if (!i && plain(member(api, "execution_path")) == "DX11 Direct") {
                    timing_line(api, "Peripheral preparation", "peripheral_preparation_ms");
                    timing_line(api, "Peripheral prep + DLAA total", "peripheral_total_ms");
                }
            }
            savings_line(api, "native_ms", "foveated_ms");
            if (!i) {
                diagnostic_line(api, "Execution path", "execution_path");
                if (s.d3d11_use_d3d12_transport) {
                    diagnostic_line(api, "Transport status", "transport_status");
                    timing_line(api, "Total time with transport", "transport_ms");
                    const auto transport_ms = number(api, "transport_ms");
                    const auto sr_ms = number(api, s.enabled ? "foveated_ms" : "native_ms");
                    const auto peripheral_ms = s.enabled && s.peripheral_dlaa_enabled ? number(api, "peripheral_ms") : 0;
                    const auto nr_ms = !s.nr_enabled ? 0 : number(api,
                        s.nr_processing_order == NrProcessingOrder::before_upscaling
                            ? (s.nr_foveated ? "before_nr_foveated_ms" : "before_nr_full_ms")
                            : (s.nr_foveated ? "nr_foveated_ms" : "nr_full_ms"));
                    if (transport_ms > 0 && sr_ms > 0 && (!s.nr_enabled || nr_ms > 0) &&
                        (!(s.enabled && s.peripheral_dlaa_enabled) || peripheral_ms > 0))
                        ImGui::TextWrapped("Transport overhead (excludes DLSS): %.3f ms",
                            (std::max)(0.0, transport_ms - sr_ms - peripheral_ms - nr_ms));
                    else ImGui::TextWrapped("Transport overhead (excludes DLSS): Not sampled yet");
                }
            }
        }
        ImGui::PopID();
    }
    if (!active) ImGui::TextWrapped("Waiting for the first Direct3D DLSS evaluation. GPU timings are unavailable for Vulkan.");
}

void draw_nr_performance(std::string_view snapshot, const Settings& s) {
    ImGui::TextWrapped("GPU timings are 250 ms averages. Comparisons retain the last sampled value for each mode.");
    for (unsigned i = 0; i < 2; ++i) {
        const auto api = array_object(member(snapshot, "apis"), i);
        if (number(api, "evaluations") == 0) continue;
        ImGui::SeparatorText(i ? "Direct3D 12 NR timing" : "DX11 -> DX12 NR timing");
        if (s.nr_processing_order == NrProcessingOrder::before_upscaling) {
            timing_line(api, "Full NR + preparation", "before_nr_full_ms");
            timing_line(api, "Foveated NR + preparation", "before_nr_foveated_ms");
            timing_line(api, "Total intercepted pipeline", "before_pipeline_ms");
            savings_line(api, "before_nr_full_ms", "before_nr_foveated_ms");
        } else {
            timing_line(api, "Full DLSS-NR call", "nr_full_ms");
            timing_line(api, "Foveated DLSS-NR call", "nr_foveated_ms");
            timing_line(api, "Total intercepted pipeline", "after_pipeline_ms");
            savings_line(api, "nr_full_ms", "nr_foveated_ms");
        }
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::TreeNode("Other processing order timings")) {
            if (s.nr_processing_order == NrProcessingOrder::before_upscaling) {
                timing_line(api, "After: full DLSS-NR call", "nr_full_ms");
                timing_line(api, "After: foveated DLSS-NR call", "nr_foveated_ms");
                timing_line(api, "After: total intercepted pipeline", "after_pipeline_ms");
            } else {
                timing_line(api, "Before: full NR + preparation", "before_nr_full_ms");
                timing_line(api, "Before: foveated NR + preparation", "before_nr_foveated_ms");
                timing_line(api, "Before: total intercepted pipeline", "before_pipeline_ms");
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::SeparatorText("DLSS-NR status and resolution");
    {
        diagnostic_line(snapshot, "State", "nr");
        const auto nr = member(snapshot, "nr_details");
        diagnostic_line(nr, "Current route", "route");
        if (!plain(member(nr, "skip_reason")).empty()) diagnostic_line(nr, "Skipped", "skip_reason");
        diagnostic_line(nr, "Candidates", "candidates");
        diagnostic_line(nr, "Evaluations", "evaluations");
        diagnostic_line(nr, "Failures", "failures");
        if (!member(nr, "result").empty())
            ImGui::Text("Last NGX result: 0x%08X", static_cast<std::uint32_t>(number(nr, "result")));
        ImGui::TextWrapped("Rendering order: %s", number(nr, "processing_order") == 1 ? "Before upscaling" : "After upscaling");
        resolution_line(nr, "Processing resolution", "processing_width", "processing_height");
        resolution_line(nr, "DLSS-SR output", "output_width", "output_height");
        region_line(nr, nr, "DLSS-NR region", "region_width", "region_height", "region_x", "region_y",
            "processing_width", "processing_height");
        resolution_line(nr, "DLSS-NR working size", "working_width", "working_height");
        if (number(nr, "vram_bytes") > 0)
            ImGui::Text("Intermediate VRAM: %.1f MiB", number(nr, "vram_bytes") / (1024 * 1024));
        else ImGui::TextUnformatted("Intermediate VRAM: Not allocated yet");
    }
}

template<class T> void raw_setting(const char* name, T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        ImGui::Checkbox(name, &value);
    } else {
        ImGui::PushID(name);
        ImGui::TextUnformatted(name);
        ImGui::SetNextItemWidth(-1);
        if constexpr (std::is_floating_point_v<T>) ImGui::InputFloat("##value", &value, 0.0F, 0.0F, "%.6g");
        else {
            std::uint32_t number = static_cast<std::uint32_t>(value);
            if (ImGui::InputScalar("##value", ImGuiDataType_U32, &number)) value = static_cast<T>(number);
        }
        ImGui::PopID();
    }
}

bool begin_tab(const char* label) {
    if (!ImGui::BeginTabItem(label)) return false;
    // Keep the header and tabs reachable while long pages scroll independently.
    ImGui::BeginChild(label, ImVec2(0, 0), ImGuiChildFlags_None);
    return true;
}

void end_tab() { ImGui::EndChild(); ImGui::EndTabItem(); }

void draw_overlay_ui(OverlayUiState& r,const OverlayRuntime& runtime,const char* renderer,const char* status,bool& open) {
    refresh(r, runtime);
    ImGui::SetNextWindowSize(ImVec2(620, 650), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(420, 300), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("Cheeky Foveated DLSS###CheekyStandalone", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextDisabled("v" CHEEKY_VERSION " | %s | %s | F8 to close", runtime.host_name ? runtime.host_name : "Standalone", renderer);
        ImGui::TextWrapped("Changes apply when you release a control and are saved automatically.");
        if (!r.message.empty()) ImGui::TextWrapped("%s", r.message.c_str());
        ImGui::Separator();
        const auto previous = r.draft;
        if (ImGui::BeginTabBar("controls")) {
            if (begin_tab("Stereo / Gaze")) {
                draw_gaze_status(r.draft, r.snapshot);
                draw_calibration_controls(r, runtime);
                ImGui::SeparatorText("Placement");
                draw_gaze(r.draft);
                if (ImGui::Button("Reset gaze defaults")) command(r, runtime, "defaults_gaze");
                end_tab();
            }
            if (begin_tab("DLSS-SR")) {
                ImGui::Checkbox("Use lower DLSS hook (DX12)", &r.draft.d3d12_lower_hook);
                ImGui::TextWrapped("Off selects the higher call. Restart the game after changing this.");
                ImGui::Text("Active DLSS hook: %s", member(r.snapshot, "d3d12_lower_hook_active") == "true" ? "Lower" : "Higher");
                if (member(r.snapshot, "d3d12_hook_restart_required") == "true")
                    ImGui::TextUnformatted("DLSS hook change saved for next game restart.");
                draw_sr(r.draft, member(array_object(member(r.snapshot,"apis"),1),"reconstruction_feature") == "13");
                if (ImGui::Button("Reset SR defaults")) command(r, runtime, "defaults_sr");
                if (ImGui::CollapsingHeader("Performance##sr", ImGuiTreeNodeFlags_DefaultOpen))
                    draw_sr_performance(r.snapshot, r.draft);
                end_tab();
            }
            if (begin_tab("DLSS-NR")) {
                if (member(r.snapshot, "renderer") == "0") {
                    ImGui::Checkbox("DX11 -> DX12 transport", &r.draft.d3d11_use_d3d12_transport);
                    ImGui::TextWrapped("Required for DLSS-NR in DX11 games. Changing NR does not change this setting.");
                }
                diagnostic_line(r.snapshot, "Runtime state", "nr");
                if (r.draft.nr_enabled) {
                    const auto reason = plain(member(member(r.snapshot, "nr_details"), "skip_reason"));
                    if (!reason.empty()) warning(reason.c_str());
                }
                draw_nr(r.draft);
                if (ImGui::Button("Reset NR history / retry")) command(r, runtime, "reset_nr");
                if (ImGui::Button("Reset NR defaults")) command(r, runtime, "defaults_nr");
                if (ImGui::CollapsingHeader("Performance##nr", ImGuiTreeNodeFlags_DefaultOpen))
                    draw_nr_performance(r.snapshot, r.draft);
                end_tab();
            }
            if (begin_tab("Diagnostics")) {
                diagnostic_line(r.snapshot, "Runtime ready", "ready");
                diagnostic_line(r.snapshot, "NR state", "nr");
                diagnostic_line(member(r.snapshot, "frame"), "Frame time (ms)", "present_ms");
                diagnostic_line(member(r.snapshot, "observer"), "Native observer ready", "ready");
                diagnostic_line(member(r.snapshot, "nr_details"), "NR skip reason", "skip_reason");
                ImGui::TextWrapped("Overlay: %s", status);
                draw_gaze_details(r.snapshot);
                draw_calibration(r, runtime);
                if (ImGui::Button("Report an issue...")) command(r, runtime, "report_issue");
                if (ImGui::Button("Create support ZIP")) command(r, runtime, "report");
                if (ImGui::Button("Show support ZIP")) command(r, runtime, "show_report");
                if (ImGui::Button("Copy diagnostic snapshot")) ImGui::SetClipboardText(r.snapshot.c_str());
                if (ImGui::TreeNode("DX12 GPU timing collection")) {
                    ImGui::TextWrapped("These counters cover the DX12 collector only. DX11 direct SR and eye calibration use separate timers.");
                    const auto gpu = member(r.snapshot, "gpu_timing");
                    diagnostic_line(gpu, "Published samples", "published");
                    diagnostic_line(gpu, "Waiting for submission", "waiting_submission");
                    diagnostic_line(gpu, "Waiting for GPU", "waiting_gpu");
                    diagnostic_line(gpu, "Discarded samples", "discarded");
                    diagnostic_line(gpu, "Failures", "failures");
                    diagnostic_line(gpu, "Last error", "last_error");
                    ImGui::TreePop();
                }
                if (ImGui::TreeNode("Full diagnostic snapshot")) {
                    ImGui::TextWrapped("%s", r.snapshot.c_str());
                    ImGui::TreePop();
                }
                end_tab();
            }
            if (begin_tab("All settings")) {
                ImGui::TextWrapped("Advanced values use the same keys as the configuration file. The runtime validates and clamps each transaction.");
#define CHEEKY_SETTING(name, field) if (!std::string_view(name).starts_with("EyeCalibrationLearned")) raw_setting(name, r.draft.field);
#include "settings_fields.inc"
#undef CHEEKY_SETTING
                end_tab();
            }
            ImGui::EndTabBar();
        }
        // Geometry/model controls can allocate expensive GPU resources. Keep a
        // local draft during dragging; commit only after the active edit ends.
        if (!ImGui::IsAnyItemActive()) {
            Settings configured = previous;
            const auto current = member(r.snapshot, "settings");
#define CHEEKY_SETTING(name, field) { const auto value = member(current, name); if (!value.empty()) set_named_setting(configured, name, value); }
#include "settings_fields.inc"
#undef CHEEKY_SETTING
            commit(r, runtime, configured);
        }
    }
    ImGui::End();
}
}
