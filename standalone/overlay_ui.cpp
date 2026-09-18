#include "overlay_ui.hpp"
#include "settings_io.hpp"
#include "version.h"
#include <imgui.h>
#include <array>
#include <cfloat>
#include <locale>
#include <sstream>
#include <string_view>
#include <type_traits>
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
#define CHEEKY_SETTING(name, field) if (previous.field != r.draft.field) out << name << '=' << scalar(r.draft.field) << '\n';
#include "settings_fields.inc"
#undef CHEEKY_SETTING
    const auto payload = out.str();
    if (!payload.empty()) command(r, runtime, "set", payload);
}

void slider(const char* label, float& value, float low, float high, const char* format = "%.2f") {
    ImGui::SliderFloat(label, &value, low, high, format, ImGuiSliderFlags_AlwaysClamp);
}

template<class T> void combo(const char* label, T& value, const char* names) {
    int index = static_cast<int>(value);
    if (ImGui::Combo(label, &index, names)) value = static_cast<T>(index);
}

void preset(const char* label, std::uint32_t& value, bool game_default) {
    constexpr std::uint32_t values[]{0,5,11,12,13};
    constexpr const char* names[]{"Game default", "E (fastest)", "K", "L", "M"};
    const int first = game_default ? 0 : 1;
    int selected{};
    for (int i = first; i < static_cast<int>(std::size(values)); ++i) if (values[i] == value) selected = i - first;
    if (ImGui::Combo(label, &selected, names + first, static_cast<int>(std::size(values)) - first)) value = values[first + selected];
}

void draw_sr(Settings& s) {
    ImGui::Checkbox("Enable foveated DLSS-SR", &s.enabled);
    ImGui::BeginDisabled(!s.enabled);
    preset("Center preset", s.center_preset, true);
    slider("Center supersampling", s.center_supersampling, 1.0F, 2.0F, "%.2fx");
    ImGui::Checkbox("Peripheral DLAA", &s.peripheral_dlaa_enabled);
    ImGui::BeginDisabled(!s.peripheral_dlaa_enabled);
    preset("Peripheral preset", s.peripheral_dlaa_preset, false);
    slider("Periphery scale", s.peripheral_dlaa_scale, .2F, 1.0F);
    ImGui::EndDisabled();
    slider("Fovea width", s.width, .2F, 1.0F);
    slider("Fovea height", s.height, .2F, 1.0F);
    slider("Roundness", s.roundness, 0.0F, 1.0F);
    slider("Transition width", s.transition_width, 0.0F, .3F, "%.3f");
    ImGui::Checkbox("Show red alignment border", &s.alignment_border_enabled);
    ImGui::EndDisabled();
}

void draw_gaze(Settings& s) {
    combo("Foveation center", s.center_mode, "Fixed\0Runtime gaze (OpenXR / OpenVR)\0Simulated gaze\0");
    ImGui::Checkbox("Automatic stereo alignment", &s.auto_stereo_alignment);
    ImGui::TextWrapped("Runtime gaze needs the Cheeky OpenXR layer or a supported OpenVR runtime. Fixed placement is used when tracking is unavailable.");
    slider("Stereo X offset", s.x_offset, -1.0F, 1.0F);
    slider("Height offset", s.auto_stereo_alignment ? s.aligned_height_offset : s.height_offset, -1.0F, 1.0F);
    ImGui::Checkbox("Invert stereo eye order", &s.invert_stereo_x_offset);
    if (s.center_mode == FoveationCenterMode::simulated_gaze) {
        combo("Simulation pattern", s.simulation_pattern, "Figure eight (8 s)\0Slow sweep (20 s)\0Jump every 2 s\0Jump every 8 s\0Tracking loss\0Hold center\0");
        ImGui::Checkbox("Show next jump target", &s.show_next_jump_target);
    }
    slider("Gaze smoothing", s.gaze_smoothing_ms, 0.0F, 100.0F, "%.0f ms");
    int pixels = static_cast<int>(s.gaze_quantization_pixels);
    if (ImGui::SliderInt("Crop origin quantization", &pixels, 1, 64, "%d px", ImGuiSliderFlags_AlwaysClamp)) s.gaze_quantization_pixels = static_cast<std::uint32_t>(pixels);
    slider("Jump reset threshold", s.gaze_jump_reset_ratio, .01F, 1.0F, "%.3f crop");
    if (ImGui::TreeNode("AFW coverage")) {
        ImGui::Checkbox("Automatic AFW coverage", &s.afw_automatic_coverage);
        ImGui::Checkbox("Manual AFW coverage", &s.afw_manual_coverage);
        slider("AFW warp margin", s.afw_warp_margin, 0.0F, .3F, "%.3f");
        ImGui::TreePop();
    }
}

void draw_nr(Settings& s) {
    ImGui::Checkbox("Enable DLSS-NR", &s.nr_enabled);
    ImGui::BeginDisabled(!s.nr_enabled);
    ImGui::Checkbox("Foveated DLSS-NR", &s.nr_foveated);
    ImGui::Checkbox("Use DLSS-SR size and shape", &s.nr_use_sr_foveation);
    ImGui::BeginDisabled(!s.nr_foveated || s.nr_use_sr_foveation);
    slider("NR fovea width", s.nr_width, .2F, 1.0F);
    slider("NR fovea height", s.nr_height, .2F, 1.0F);
    slider("NR roundness", s.nr_roundness, 0.0F, 1.0F);
    slider("NR transition width", s.nr_transition_width, 0.0F, .3F, "%.3f");
    ImGui::EndDisabled();
    ImGui::Checkbox("Show green alignment border", &s.nr_alignment_border_enabled);
    combo("Rendering order", s.nr_processing_order, "After upscaling\0Before upscaling (experimental)\0");
    slider("Working scale", s.nr_working_scale, .1F, 1.0F);
    combo("DLSS-NR style", s.nr_style, "Standard\0Natural\0Cinematic\0");
    slider("Intensity", s.nr_intensity, 0.0F, 1.0F);
    if (ImGui::TreeNode("Advanced neural rendering")) {
        slider("Local tone strength", s.nr_local_tone_strength, 0.0F, 2.0F);
        slider("Local structure strength", s.nr_local_structure_strength, 0.0F, 2.0F);
        ImGui::Checkbox("Automatic mask", &s.nr_automatic_mask);
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
    ImGui::EndDisabled();
}

void diagnostic_line(std::string_view object, const char* label, const char* key) {
    const auto value = plain(member(object, key));
    ImGui::TextWrapped("%s: %s", label, value.empty() ? "unavailable" : value.c_str());
}

template<class T> void raw_setting(const char* name, T& value) {
    if constexpr (std::is_same_v<T, bool>) ImGui::Checkbox(name, &value);
    else if constexpr (std::is_floating_point_v<T>) ImGui::InputFloat(name, &value, 0.0F, 0.0F, "%.6g");
    else {
        std::uint32_t number = static_cast<std::uint32_t>(value);
        if (ImGui::InputScalar(name, ImGuiDataType_U32, &number)) value = static_cast<T>(number);
    }
}

void draw_overlay_ui(OverlayUiState& r,const OverlayRuntime& runtime,const char* renderer,const char* status,bool& open) {
    refresh(r, runtime);
    ImGui::SetNextWindowSize(ImVec2(620, 650), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(420, 300), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("Cheeky Foveated DLSS###CheekyStandalone", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextDisabled("v" CHEEKY_VERSION " | %s | %s | F8 to close", runtime.host_name ? runtime.host_name : "Standalone", renderer);
        ImGui::TextWrapped("Changes apply when you release a control and are saved automatically.");
        ImGui::TextWrapped("%s", r.message.c_str());
        ImGui::Separator();
        const auto previous = r.draft;
        if (ImGui::BeginTabBar("controls")) {
            if (ImGui::BeginTabItem("DLSS-SR")) {
                draw_sr(r.draft);
                if (ImGui::Button("Reset SR defaults")) command(r, runtime, "defaults_sr");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Gaze / stereo")) {
                draw_gaze(r.draft);
                if (ImGui::Button("Reset gaze defaults")) command(r, runtime, "defaults_gaze");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("DLSS-NR")) {
                draw_nr(r.draft);
                if (ImGui::Button("Reset NR history / retry")) command(r, runtime, "reset_nr");
                ImGui::SameLine();
                if (ImGui::Button("Reset NR defaults")) command(r, runtime, "defaults_nr");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Diagnostics")) {
                diagnostic_line(r.snapshot, "Runtime ready", "ready");
                diagnostic_line(r.snapshot, "NR state", "nr");
                const auto gaze = member(r.snapshot, "gaze");
                diagnostic_line(gaze, "Gaze source", "runtime");
                diagnostic_line(gaze, "Gaze driving foveation", "using_gaze");
                diagnostic_line(gaze, "Active stereo views", "views");
                diagnostic_line(member(r.snapshot, "frame"), "Frame time (ms)", "present_ms");
                diagnostic_line(member(r.snapshot, "observer"), "Native observer ready", "ready");
                diagnostic_line(member(r.snapshot, "nr_details"), "NR skip reason", "skip_reason");
                ImGui::TextWrapped("Overlay: %s", status);
                if (ImGui::Button("Report an issue...")) command(r, runtime, "report_issue");
                ImGui::SameLine();
                if (ImGui::Button("Create support ZIP")) command(r, runtime, "report");
                ImGui::SameLine();
                if (ImGui::Button("Show support ZIP")) command(r, runtime, "show_report");
                if (ImGui::Button("Copy diagnostic snapshot")) ImGui::SetClipboardText(r.snapshot.c_str());
                if (ImGui::Button("Enable eye calibration")) command(r, runtime, "calibration_enable");
                ImGui::SameLine();
                if (ImGui::Button("Disable eye calibration")) command(r, runtime, "calibration_disable");
                if (ImGui::Button("Reset calibration counters")) command(r, runtime, "calibration_reset");
                if (ImGui::TreeNode("Full diagnostic snapshot")) {
                    ImGui::TextWrapped("%s", r.snapshot.c_str());
                    ImGui::TreePop();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("All settings")) {
                ImGui::TextWrapped("Advanced values use the same keys as the configuration file. The runtime validates and clamps each transaction.");
                ImGui::BeginChild("fields", ImVec2(0, 0), ImGuiChildFlags_None);
#define CHEEKY_SETTING(name, field) raw_setting(name, r.draft.field);
#include "settings_fields.inc"
#undef CHEEKY_SETTING
                ImGui::EndChild();
                ImGui::EndTabItem();
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
