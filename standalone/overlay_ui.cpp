#include "overlay_ui.hpp"
#include "settings_io.hpp"
#include "version.h"
#include "cheeky_gaze_abi.h"
#include <imgui.h>
#include <shellapi.h>
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
#pragma comment(lib, "shell32.lib")
void report_openxr_menu_diagnostic(const char*) noexcept;
namespace cheeky::standalone {
using namespace cheeky::foveated_dlss;
static void test_control(const char* label) {
#ifdef CHEEKY_OVERLAY_TEST_DESKTOP
    extern void overlay_test_control(const char*);
    overlay_test_control(label);
#else
    (void)label;
#endif
}
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
    std::string diagnostic = "Menu command v5: " + std::string(action) +
        " accepted=" + (result ? "1" : "0") + " payload=" + std::string(payload);
    std::replace(diagnostic.begin(), diagnostic.end(), '\n', ';');
    report_openxr_menu_diagnostic(diagnostic.c_str());
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
    constexpr std::uint32_t values[]{0,5,10,11,12,13};
    constexpr const char* names[]{"Game default", "E (fastest)", "J", "K", "L", "M"};
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
void draw_sr(Settings& s, bool rr, bool quad) {
    ImGui::Checkbox(quad?"Enable quad-view DLSS-SR":"Enable foveated DLSS-SR", &s.enabled);
    test_control("Enabled");
    if(quad)ImGui::TextWrapped("Each assigned view is processed in full. Center controls apply to both center views; peripheral controls apply to both peripheral views.");
    ImGui::SeparatorText("Center quality");
    if (rr) { ImGui::TextUnformatted("Ray Reconstruction active"); rr_preset("Center RR preset",s.rr_center_preset); }
    else preset("Center preset", s.center_preset, true);
    if (!rr) {
        ImGui::Checkbox("Fix motion-vector blur", &s.fix_motion_blur);
        test_control("FixMotionBlur");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "DX12 DLSS-SR with input-resolution motion vectors.\n"
            "Copies the center motion region to zero offset on the GPU. Adds GPU work.");
        ImGui::TextDisabled("(Use if preset causes blurriness)");
    }
    slider("Center supersampling", s.center_supersampling, 1.0F, 2.0F, "%.2fx");
    ImGui::Checkbox(quad?"Override peripheral views":"Peripheral DLSS", &s.peripheral_dlaa_enabled);
    if (s.peripheral_dlaa_enabled) {
        if (rr) rr_preset("Peripheral RR preset",s.rr_peripheral_preset);
        else preset("Peripheral preset", s.peripheral_dlaa_preset, false);
        slider("Periphery scale", s.peripheral_dlaa_scale, .2F, 1.0F,"%.2fx");
        ImGui::TextWrapped("20%-100% of the game's DLSS input width and height; default 75%.");
        if(quad)ImGui::TextWrapped("Peripheral DLSS uses this processing scale over the full peripheral view. Off keeps the game's original peripheral DLSS.");
    }
    if(quad)return;
    if (rr) ImGui::TextWrapped("RR denoises both regions. Disabling peripheral DLSS uses input-resolution RR in the periphery.");
    ImGui::SeparatorText("Size and shape");
    slider("Fovea width", s.width, .2F, 1.0F);
    slider("Fovea height", s.height, .2F, 1.0F);
    slider("Roundness", s.roundness, 0.0F, 1.0F);
    ImGui::TextDisabled("Cosmetic only; no performance impact.");
    slider("Transition width", s.transition_width, 0.0F, .3F, "%.3f");
    ImGui::Checkbox("Show red alignment border", &s.alignment_border_enabled);
}

void draw_gaze(Settings& s) {
    combo("Foveation center", s.center_mode, "Fixed\0Runtime gaze (OpenXR / OpenVR / LibOVR)\0Simulated gaze\0");
    ImGui::Checkbox("Automatic stereo alignment", &s.auto_stereo_alignment);
    if (s.center_mode == FoveationCenterMode::openxr_gaze)
        ImGui::TextWrapped("Runtime gaze needs the Cheeky OpenXR layer or a supported OpenVR runtime. Fixed placement is used when tracking is unavailable.");
    if (!s.auto_stereo_alignment && s.center_mode == FoveationCenterMode::fixed)
        slider("Stereo X offset", s.x_offset, -1.0F, 1.0F);
    slider("Height offset",
        s.auto_stereo_alignment ? s.aligned_height_offset : s.height_offset, -1.0F, 1.0F);
    if (!s.auto_stereo_alignment && ImGui::TreeNode("Stereo mapping override")) {
        ImGui::Checkbox("Invert stereo eye order", &s.invert_stereo_x_offset);
        ImGui::TextWrapped("For packed layouts with reversed eye order; normally leave off.");
        ImGui::TreePop();
    }
    ImGui::TextWrapped("Height offset moves fixed and eye-tracked placement. Negative = up; positive = down.");
    if (ImGui::TreeNode("OpenXR input compatibility")) {
        ImGui::Checkbox("Independent OpenXR gaze (restart application required)", &s.independent_openxr_gaze);
        ImGui::TextWrapped("For hosts without OpenXR input only. After 90 frames, lets the layer attach gaze actions. A host that initializes input later cannot attach its own actions until restart.");
        ImGui::TreePop();
    }
    if (s.center_mode == FoveationCenterMode::simulated_gaze) {
        combo("Simulation pattern", s.simulation_pattern, "Figure eight (8 s)\0Slow sweep (20 s)\0Jump every 2 s\0Jump every 8 s\0Tracking loss\0Hold center\0");
        if (s.simulation_pattern == 2 || s.simulation_pattern == 3)
            ImGui::Checkbox("Show next jump target", &s.show_next_jump_target);
    }
    if (s.center_mode != FoveationCenterMode::fixed && ImGui::TreeNode("Advanced eye tracking")) {
        slider("Gaze smoothing", s.gaze_smoothing_ms, 0.0F, 100.0F, "%.0f ms");
        slider("Hold gaze on signal loss", s.gaze_hold_ms, 0.0F, 1000.0F, "%.0f ms");
        ImGui::TextDisabled("Covers blinks; longer loss returns to fixed placement.");
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

void draw_nr(Settings& s, bool quad) {
    ImGui::Checkbox(quad?"Enable DLSS-NR on center views":"Enable DLSS-NR", &s.nr_enabled);
    if(quad)ImGui::TextWrapped("NR covers each assigned center view in full. Peripheral views do not run NR. D3D11 requires DX12 Transport.");
    if(!quad){
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
            ImGui::TextDisabled("Cosmetic only; no performance impact.");
            slider("NR transition width", s.nr_transition_width, 0.0F, .3F, "%.3f");
        }
        ImGui::Checkbox("Show green alignment border", &s.nr_alignment_border_enabled);
    }
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
    if (!flag(gaze, "layer")) return "Eye tracking unavailable: no active OpenXR layer or supported OpenVR/LibOVR adapter. Using fixed placement.";
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
    case 4: return "LibOVR";
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
        diagnostic_line(gaze, "Registered DLSS handles", "views");
        diagnostic_line(gaze, "Sample age (ms)", "age_ms");
        const auto flags = static_cast<unsigned>(number(gaze, "status_flags"));
        for (const auto& item : {std::pair{"System supports eye tracking", CHEEKY_GAZE_STATUS_SYSTEM_SUPPORTED},
                 {"Session focused", CHEEKY_GAZE_STATUS_SESSION_FOCUSED},
                 {"Gaze input active", CHEEKY_GAZE_STATUS_ACTION_ACTIVE},
                 {"Tracking valid", CHEEKY_GAZE_STATUS_GAZE_VALID},
                 {"Submission mapping ready", CHEEKY_GAZE_STATUS_MAPPING_READY}})
            ImGui::TextWrapped("%s: %s", item.first, gaze.empty() ? "unavailable" : (flags & item.second) ? "Yes" : "No");
        const auto libovr = member(snapshot, "libovr");
        if (flag(libovr, "hooked")) {
            ImGui::SeparatorText("LibOVR runtime");
            diagnostic_line(libovr, "Runtime module", "module");
            diagnostic_line(libovr, "Layer header layout", "layout");
            diagnostic_line(libovr, "Frames observed", "frames");
            diagnostic_line(libovr, "Frames with stereo projection", "projection_frames");
            const auto pvr = member(libovr, "pvr");
            diagnostic_line(pvr, "Pimax PVR client", "client");
            diagnostic_line(pvr, "Game PVR session found", "session_captured");
            diagnostic_line(pvr, "Eye tracker supplying gaze", "gaze_valid");
            diagnostic_line(pvr, "Valid gaze samples", "valid_samples");
        }
        for (unsigned i = 0; i < 2; ++i) {
            const auto eye = array_object(member(gaze, "eyes"), i);
            ImGui::SeparatorText(i ? "Right eye" : "Left eye");
            diagnostic_line(eye, "Mapped", "mapped");
            if (flag(eye, "layout")) ImGui::TextUnformatted("Mapping: Manual stereo layout (unverified)");
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

void draw_view_details(std::string_view snapshot) {
    if (!ImGui::TreeNode("DLSS view activity")) return;
    const auto gaze = member(snapshot, "gaze");
    diagnostic_line(gaze, "Registered handles", "registered_views");
    diagnostic_line(gaze, "Receiving calls (last 2 seconds)", "receiving_views");
    diagnostic_line(gaze, "Ever evaluated", "evaluated_views");
    diagnostic_line(gaze, "Never evaluated", "never_evaluated_views");
    ImGui::TextWrapped("Counts describe DLSS handles and observed evaluation calls. They do not establish successful GPU frames or the number of OpenXR projection views. Focus/periphery roles remain unknown until verified.");
    const auto views = member(snapshot, "view_details");
    for (unsigned i = 0; i < 16; ++i) {
        const auto view = array_object(views, i);
        if (view.empty()) break;
        ImGui::PushID(static_cast<int>(i));
        // One scannable line per handle; the identifiers open on demand.
        const auto summary = plain(member(view, "eye")) + " | " + plain(member(view, "api")) + " | " +
            plain(member(view, "activity")) + " | " + std::to_string(static_cast<long long>(number(view, "output_width"))) +
            "x" + std::to_string(static_cast<long long>(number(view, "output_height")));
        if (!ImGui::TreeNode("handle", "%s", summary.c_str())) {ImGui::PopID();continue;}
        diagnostic_line(view, "ID", "id");
        diagnostic_line(view, "Registration generation", "generation");
        diagnostic_line(view, "Activity", "activity");
        diagnostic_line(view, "Evaluation calls", "evaluation_calls");
        if (plain(member(view, "last_evaluation_age_ms")) != "null")
            diagnostic_line(view, "Last evaluation age (ms)", "last_evaluation_age_ms");
        diagnostic_line(view, "API", "api");
        diagnostic_line(view, "Eye", "eye");
        diagnostic_line(view, "Mapping", "mapping");
        diagnostic_line(view, "Focus/periphery", "region");
        ImGui::Text("Input: %.0f x %.0f | Output: %.0f x %.0f",
            number(view, "input_width"), number(view, "input_height"),
            number(view, "output_width"), number(view, "output_height"));
        if (flag(view, "has_geometry")) {
            ImGui::Text("Last output crop: %.0f x %.0f at %.0f, %.0f",
                number(view, "crop_width"), number(view, "crop_height"),
                number(view, "crop_x"), number(view, "crop_y"));
            diagnostic_line(view, "Geometry observations", "evaluations");
        }
        ImGui::TreePop();
        ImGui::PopID();
    }
    if (number(snapshot, "view_details_total") > 16)
        ImGui::TextDisabled("Showing the first 16 registered handles.");
    ImGui::TreePop();
}

void draw_calibration_controls(OverlayUiState& r, const OverlayRuntime& runtime) {
    ImGui::SeparatorText("Stereo Eye Mapping");
    int method = r.draft.manual_stereo_mapping ? 4 : int(r.draft.eye_calibration_method);
    if (ImGui::Combo("Mapping method", &method,
            "Auto\0Standard corners\0Timing tolerant corners\0Full crop search\0Manual\0")) {
        r.draft.manual_stereo_mapping = method == 4;
        if (method != 4) r.draft.eye_calibration_method = static_cast<EyeCalibrationMethod>(method);
    }
    ImGui::BeginDisabled(!r.draft.manual_stereo_mapping);
    ImGui::Checkbox("First DLSS view is right eye", &r.draft.manual_stereo_right_first);
    ImGui::EndDisabled();
    if (r.draft.manual_stereo_mapping) {
        ImGui::TextWrapped("Manual assumes full-eye images with matching XR coordinates: no crop, flip or projection changes. Off means left eye first. Check both eyes and image edges with the red border.");
        return;
    }
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
}

void draw_quad_preferences(Settings& s) {
    // Independent of assignment mode: never enable manual mode or labels as a
    // side effect of changing the other control.
    ImGui::Checkbox("Show view numbers",&s.quad_show_numbers);
    ImGui::TextWrapped("Small numbers repeat across each view, including outside the center inset.");
    int mode=s.quad_manual_assignment?1:0;
    if(ImGui::Combo("Quad assignment mode",&mode,"Automatic\0Manual\0"))s.quad_manual_assignment=mode==1;
}

// Quad assignment replaces the stereo page only while four views are active.
// Stereo and flat sessions keep the preferences in a collapsed group below.
bool draw_quad_controls(OverlayUiState& r,const OverlayRuntime& runtime) {
    const auto quad=member(r.snapshot,"quad_views");
    if(!flag(quad,"active"))return false;
    ImGui::SeparatorText("Quad view assignment");
    draw_quad_preferences(r.draft);
    if(flag(quad,"app_composited"))ImGui::TextWrapped("Four DLSS views; the game composites them before OpenXR. Default order: Periphery Left, Center Left, Periphery Right, Center Right. Use Manual to change it.");
    diagnostic_line(quad,"Assignment","status");
    const auto sources=member(quad,"sources");
    for(unsigned i=0;i<8;++i){const auto source=array_object(sources,i);if(source.empty())break;
        const auto view_number=static_cast<unsigned>(number(source,"number"));
        std::string assignment_payload;
        const auto label="View "+std::to_string(view_number);int selected=static_cast<int>(number(source,"manual_role"));
        static constexpr const char* roles[]{"Unassigned","Periphery Left","Periphery Right","Center Left","Center Right"};
        ImGui::PushID(static_cast<int>(view_number));
        if(r.draft.quad_manual_assignment){
            if(ImGui::BeginCombo(label.c_str(),selected>=0&&selected<5?roles[selected]:roles[0])){
                for(int role=0;role<5;++role){bool taken{};
                    for(unsigned j=0;j<8&&role;++j){const auto other=array_object(sources,j);if(other.empty())break;
                        taken|=number(other,"number")!=view_number&&number(other,"manual_role")==role;}
                    ImGui::BeginDisabled(taken);
                    if(ImGui::Selectable(roles[role],selected==role)){
                        assignment_payload=plain(member(quad,"session"))+" "+std::to_string(view_number)+" "+std::to_string(role)+" "+
                            plain(member(source,"id"))+" "+plain(member(source,"generation"));
                    }
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
        }else diagnostic_line(source,label.c_str(),"role");
        ImGui::PopID();
        if(!assignment_payload.empty()){command(r,runtime,"quad_assign",assignment_payload);return true;}
    }
    if(ImGui::Button(r.draft.quad_manual_assignment?"Reset quad assignments":"Retry automatic assignment"))command(r,runtime,"quad_recalibrate");
    ImGui::TextWrapped("Manual roles are saved by view number and restored when the source count matches. Each role can be assigned once. Check the labels if the game's order changes.");
    return true;
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
    diagnostic_line(data, "Last recognized left view", "left_view");
    diagnostic_line(data, "Last recognized right view", "right_view");
    if (ImGui::TreeNode("Calibration timing")) {
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
        ImGui::TreePop();
    }
    ImGui::TextWrapped("Corner verification samples every 10 VR frames; timing recovery captures every frame while acquiring. Corrections count changes to an existing eye assignment. GPU time covers marker and copy commands; CPU time excludes lock waiting.");
    if (ImGui::Button("Reset calibration counters")) command(r, runtime, "calibration_reset");
    ImGui::TreePop();
}

// One row per feature, so the state of the whole pipeline is readable at a
// glance before opening any detail group.
void draw_diagnostics_summary(const OverlayUiState& r, const char* status) {
    const auto& s = r.draft;
    const auto& snapshot = r.snapshot;
    ImGui::SeparatorText("Summary");
    if (!ImGui::BeginTable("summary", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) return;
    ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch, 1.F);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 2.2F);
    const auto row = [](const char* label, const std::string& value) {
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", value.empty() ? "Unavailable" : value.c_str());
    };
    const auto yes_no = [](std::string_view object, const char* key) -> std::string {
        const auto value = plain(member(object, key));
        return value == "true" ? "Yes" : value == "false" ? "No" : value;
    };
    row("Runtime ready", yes_no(snapshot, "ready"));
    row("Overlay", status ? status : "");
    const auto present_ms = number(member(snapshot, "frame"), "present_ms");
    char frame[64]{};
    if (present_ms > 0) std::snprintf(frame, sizeof(frame), "%.2f ms (%.1f FPS)", present_ms, 1000 / present_ms);
    row("Frame time", present_ms > 0 ? frame : "Not sampled yet");
    std::string sr;
    for (unsigned i = 0; i < 2; ++i) {
        const auto api = array_object(member(snapshot, "apis"), i);
        if (number(api, "evaluations") == 0) continue;
        if (!sr.empty()) sr += "; ";
        sr += (i ? "D3D12: " : "D3D11: ") + plain(member(api, "state"));
    }
    const auto vulkan = member(snapshot, "vulkan");
    if (sr.empty() && number(vulkan, "evaluations") > 0) sr = "Vulkan: " + plain(member(vulkan, "state"));
    row("DLSS-SR", !s.enabled ? "Off" : sr.empty() ? "Waiting for the first DLSS evaluation" : sr);
    const auto nr_skip = plain(member(member(snapshot, "nr_details"), "skip_reason"));
    row("DLSS-NR", !s.nr_enabled ? "Off" : plain(member(snapshot, "nr")) + (nr_skip.empty() ? "" : " (" + nr_skip + ")"));
    const auto fg = member(snapshot, "fg");
    row("Frame generation", !s.fg.enabled ? "Off" : fg.empty() ? "Unavailable" :
        !flag(fg, "layer_present") ? "OpenXR layer not loaded" :
        !flag(fg, "compatible_session") ? "Waiting for a compatible OpenXR session" : plain(member(fg, "state")));
    const char* tracking = s.center_mode == FoveationCenterMode::fixed ? "Not in use (fixed placement)" :
        s.center_mode == FoveationCenterMode::simulated_gaze ? "Not in use (simulated gaze)" :
        !s.enabled && !(s.nr_enabled && s.nr_foveated) ? "Not evaluated (SR and foveated NR are off)" :
        gaze_warning(s, snapshot) ? "No" : "Yes";
    row("Eye tracking", tracking);
    row("Stereo alignment", alignment_name(static_cast<unsigned>(number(member(snapshot, "gaze"), "alignment"))));
    row("Eye mapping", plain(member(member(snapshot, "eye_calibration"), "status")));
    const auto quad = member(snapshot, "quad_views");
    if (flag(quad, "active")) row("Quad views", plain(member(quad, "status")));
    row("Native observer ready", yes_no(member(snapshot, "observer"), "ready"));
    ImGui::EndTable();
    if (std::string_view(tracking) == "No") warning(gaze_warning(s, snapshot));
    warning(alignment_warning(s, snapshot));
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
            ImGui::TextWrapped("Peripheral DLSS: %s", !s.peripheral_dlaa_enabled ? "Disabled" :
                motion_space == "Output-resolution" ? "Enabled (auto MV conversion)" :
                motion_space == "Input-resolution" ? "Enabled (direct MVs)" : "Enabled (waiting for compatible MV info)");
            ImGui::SeparatorText("DLSS-SR GPU timing");
            const bool quad=flag(member(snapshot,"quad_views"),"active");
            timing_line(api, quad?"Original DLSS":"Full DLSS", "native_ms");
            if(quad)timing_line(api, "Original peripheral DLSS", "native_peripheral_ms");
            if (s.enabled && number(api, "native_ms") <= 0)
                ImGui::TextWrapped("Turn off foveated DLSS-SR briefly to sample the full-frame baseline.");
            timing_line(api, "Foveated DLSS", "foveated_ms");
            if (s.peripheral_dlaa_enabled) {
                timing_line(api, "Peripheral DLSS", "peripheral_ms");
                if (!i && plain(member(api, "execution_path")) == "DX11 Direct") {
                    timing_line(api, "Peripheral preparation", "peripheral_preparation_ms");
                    timing_line(api, "Peripheral prep + DLSS total", "peripheral_total_ms");
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

void fg_percent(const char* label, std::uint32_t& value) {
    int index = static_cast<int>(value / 25) - 1;
    if (ImGui::Combo(label, &index, "25%\0" "50%\0" "75%\0" "100% native\0"))
        value = static_cast<std::uint32_t>(index + 1) * 25;
}

// NVIDIA Optical Flow output: one vector per grid x grid flow-input pixels.
void optical_grid(const char* label, std::uint32_t& value) {
    int index = value == 1 ? 2 : value == 2 ? 1 : 0;
    if (ImGui::Combo(label, &index, "4x4 blocks\0" "2x2 blocks\0" "1x1 (per pixel)\0"))
        value = index == 2 ? 1U : index == 1 ? 2U : 4U;
}

void draw_fg(Settings& settings, std::string_view snapshot, bool dx11) {
    // Game-composited quad views submit stereo, so they keep the stereo FG controls.
    const bool outer_guides=flag(member(snapshot,"quad_views"),"fg_outer_guides");
    const bool quad=flag(member(snapshot,"quad_views"),"active")&&!outer_guides;
    if(quad&&settings.fg.peripheral_method==0)settings.fg.peripheral_method=2;
    auto& s = settings.fg;
    const auto native_shader11=[&]{return dx11&&cheeky_fg_shader_only(s.center_method,s.peripheral_method,s.foveated||quad);};
    const auto fg = member(snapshot, "fg");
    ImGui::Checkbox("Enable Frame Generation", &s.enabled);
    test_control("FgEnabled");
    ImGui::Checkbox("Automatic FG multiplier",&s.automatic_multiplier);
    test_control("FgAutoMultiplier");
    ImGui::BeginDisabled(s.automatic_multiplier);
    int multiplier=int(s.multiplier)-2;
    if(ImGui::Combo("FG multiplier",&multiplier,"2x\0" "3x\0" "4x\0"))s.multiplier=unsigned(multiplier+2);
    test_control("FgMultiplier");
    ImGui::EndDisabled();
    if(s.automatic_multiplier){
        const auto automatic=member(fg,"automatic");
        if(!flag(automatic,"supported"))ImGui::TextWrapped("Auto requires the matching OpenXR layer update.");
        else ImGui::Text("Auto: %.0fx selected / %.0fx presenting (maximum %.0fx)%s",number(automatic,"selected"),number(automatic,"presented"),number(automatic,"maximum"),flag(automatic,"valid")?"":" - measuring");
        ImGui::TextWrapped("Adapts 2x-4x to rendering load. Increases after sustained overload; decreases after 1.5 seconds of headroom.");
    }
    if(s.multiplier>2||s.automatic_multiplier)ImGui::TextWrapped("DLSS requires multi-frame support from the GPU and runtime. Shader methods support all multipliers. Higher multipliers lower the game's target frame rate.");
    if (!flag(fg, "layer_present"))
        warning("OpenXR layer not loaded. Install the matching OpenXR support and restart the game.");
    else if (!flag(fg, "controls_supported"))
        warning("Update the OpenXR layer and restart the game.");
    else if (!s.enabled) ImGui::TextUnformatted("Status: Off");
    else if (!flag(fg, "compatible_session"))
        ImGui::TextWrapped(quad?"Status: Waiting for D3D11 quad-view FG support":"Status: Waiting for a running D3D11 or D3D12 OpenXR stereo session");
    else {
        diagnostic_line(fg, "Status", "state");
        const auto detail = plain(member(fg, "diagnostic"));
        if (plain(member(fg, "state")) != "Generating frames" && !detail.empty())
            ImGui::TextWrapped("%s", detail.c_str());
    }

    ImGui::SeparatorText(quad?"Quad view quality":"Foveation");
    // Type and method share one saved ID: 0 DLSS-G, 1-4 DLSS vectors,
    // 5-8 optical flow, 9-12 optical flow + depth, 13 previous frame.
    const auto method_combo=[](const char* prefix,unsigned& method,bool periphery,bool quad_view){
        constexpr const char* types[]{"DLSS-G","DLSS Vectors","Optiflow","Optiflow + Depth","Previous frame"};
        constexpr const char* methods[]{"Simple Warping","Bidirectional Interpolation",
            "Confidence-Guided Interpolation","Confidence-Guided Interpolation (Fast)"};
        const auto type_of=[](unsigned m){return m==0?0:m==cheeky_fg_previous?4:int((m-1)/4)+1;};
        const int first=periphery&&quad_view?1:0,count=periphery?5:4;
        const int interpolation=cheeky_fg_shader(method)?int((method-1)%4):2;
        char label[64]{};
        sprintf_s(label,"%s type",prefix);
        int selected=type_of(method)-first;
        if(ImGui::Combo(label,&selected,types+first,count-first)&&selected+first!=type_of(method)){
            const int type=selected+first;
            method=type==0?0u:type==4?cheeky_fg_previous:unsigned((type-1)*4+interpolation+1);
        }
        test_control(label);
        if(!cheeky_fg_shader(method))return;
        sprintf_s(label,"%s method",prefix);
        int index=int((method-1)%4);
        if(ImGui::Combo(label,&index,methods,4))method=method-(method-1)%4+unsigned(index);
        test_control(label);
    };
    method_combo("FG center",s.center_method,false,quad);
    if(s.center_method==0||((s.foveated||quad)&&s.peripheral_method==0)){
        int preset=int(s.dlssg_preset)-1;
        if(ImGui::Combo("DLSS-G preset",&preset,"Preset A\0Preset B\0Preset C\0"))s.dlssg_preset=unsigned(preset+1);
        test_control("FgDlssgPreset");
        if(s.dlssg_preset==2)ImGui::TextWrapped("Preset B needs separate scene and HUD inputs; without them it behaves like Preset A.");
        if(s.dlssg_preset==3)ImGui::TextWrapped("Preset C uses the faster Lite model and requires DLSS-G 310.9.16 or newer.");
    }
    if(cheeky_fg_cacheable(s.center_method))ImGui::Checkbox("Cache optical vector/confidence data (center)",&s.optical.center_cache);
    if(cheeky_fg_optical(s.center_method)){
        combo("Center optical flow preset",s.optical.center_preset,"Slow (quality)\0Medium\0Fast (performance)\0");
        fg_percent("Center optical flow resolution",s.optical.center_percent);
        optical_grid("Center optical flow vector grid",s.optical.center_grid);
    }
    fg_percent(quad?"Center FG scale":"FG scale", s.resolution_percent);
    if(!quad){
    ImGui::Checkbox("Foveated frame generation", &s.foveated);
    }
    if (s.foveated||quad) {
        if(!quad){
        ImGui::Checkbox("Match DLSS-SR foveation region", &s.match_sr_region);
        if (s.match_sr_region)
            ImGui::TextWrapped("Uses SR size, transition and per-eye placement, including gaze when available.");
        else {
            slider("FG region width", s.width, .2F, 1.F);
            slider("FG region height", s.height, .2F, 1.F);
            slider("FG horizontal offset", s.x, -.5F, .5F);
            slider("FG vertical offset", s.y, -1.F, 1.F);
            slider("FG transition width", s.feather, 0.F, .3F);
        }
        }
        fg_percent("FG periphery scale", s.peripheral_percent);
        method_combo("FG periphery",s.peripheral_method,true,quad);
        if(cheeky_fg_cacheable(s.peripheral_method))ImGui::Checkbox("Cache optical vector/confidence data (periphery)",&s.optical.peripheral_cache);
        if(cheeky_fg_optical(s.peripheral_method)){
            combo("Periphery optical flow preset",s.optical.peripheral_preset,"Slow (quality)\0Medium\0Fast (performance)\0");
            fg_percent("Periphery optical flow resolution",s.optical.peripheral_percent);
            optical_grid("Periphery optical flow vector grid",s.optical.peripheral_grid);
        }
        ImGui::Checkbox("Show blue FG region border", &s.show_border);
    }
    if (ImGui::TreeNode("Advanced settings")) {
        ImGui::Checkbox("Force NVIDIA Reflex off", &s.force_reflex_off);
        if(s.force_reflex_off)ImGui::TextWrapped("Disables NVIDIA Reflex sleep and its frame limiter. Applies even when frame generation is off.");
        char refresh_label[32]{};if(s.target_refresh_hz)sprintf_s(refresh_label,"%u Hz",s.target_refresh_hz);else strcpy_s(refresh_label,"Automatic");
        if(ImGui::BeginCombo("Headset target refresh",refresh_label)){
            for(unsigned hz:{0u,60u,72u,75u,80u,90u,100u,120u,144u,165u,180u,200u,240u,360u}){
                char label[32]{};if(hz)sprintf_s(label,"%u Hz",hz);else strcpy_s(label,"Automatic");
                if(ImGui::Selectable(label,s.target_refresh_hz==hz))s.target_refresh_hz=hz;
            }
            ImGui::EndCombo();
        }
        ImGui::TextWrapped("Used for game prediction and FPS targets; rendering proceeds when release conditions and buffer capacity allow it. Automatic uses a headset refresh query or the fastest runtime period observed this session. Set your headset's Hz explicitly if Automatic starts too low.");
        ImGui::Checkbox("Keep the GPU from falling behind (release Never)", &s.limit_gpu_ahead);
        if (s.limit_gpu_ahead)
            ImGui::TextWrapped("With release Never: the game waits while its previous frame has not yet been captured on the GPU, preventing a GPU backlog.");
        ImGui::Checkbox("Extra frame slots (more VRAM)", &s.extra_frame_slots);
        if (s.extra_frame_slots)
            ImGui::TextWrapped("Keeps two additional stereo capture slots available while frame generation is in flight. Uses more VRAM.");
        if(native_shader11())ImGui::TextWrapped("Native D3D11 records every stage on the game's immediate context, so the separate DX12 GPU queues do not apply.");
        else{
        const bool flow_queue=cheeky_fg_uses_flow(s.center_method,s.peripheral_method,s.foveated||quad);
        int queue_choice=flow_queue&&s.queue_priority==1u?1:0;
        ImGui::BeginDisabled(!flow_queue);
        if(ImGui::Combo("FG GPU queue",&queue_choice,quad?"Layer bridge queue\0Separate HIGH queue\0":"Shared game queue\0Separate HIGH queue\0"))
            s.queue_priority=queue_choice==1?1u:2u;
        ImGui::EndDisabled();
        if(!flow_queue)ImGui::TextWrapped("Separate HIGH is available only with NVIDIA Optical Flow selected for the center or active periphery. Other methods use the shared queue.");
        if(quad)ImGui::TextWrapped("DX11 quad input uses the layer's private DX12 bridge. Layer bridge queue shares its capture queue; separate queues isolate FG processing.");
        }
        // Display pipeline stages without renumbering persisted release IDs.
        // CPU submission can precede GPU capture completion. Keep its timed
        // variant adjacent; its deadline can overlap the GPU milestones.
        constexpr unsigned release_order[]{5,0,4,8,1,2,6,7,3};
        int release_choice=8;
        for(int i=0;i<int(std::size(release_order));++i)if(release_order[i]==s.release_point)release_choice=i;
        combo("Release game rendering", release_choice,
            "Never (buffer capacity only)\0After FG submission\0"
            "4 ms after FG submission\0After color capture completes\0After GPU preparation\0After FG completion\0"
            "After first generated frame submission\0After second generated frame submission\0After all generated frames submitted\0");
        s.release_point=release_order[release_choice];
        if (s.release_point == 8)
            ImGui::TextWrapped(native_shader11()?"Native D3D11 orders capture on its immediate context; this mode uses buffer capacity only.":
                "The next render starts after the GPU completes both-eye color capture and buffer capacity is available. Optical flow, frame generation and presentation may still be in flight.");
        else if (s.release_point == 5)
            ImGui::TextWrapped("The next render starts when buffer capacity allows it. Generation and presentation may still be in flight.");
        else if (s.release_point == 6 || s.release_point == 7)
            ImGui::TextWrapped("The next render starts after xrEndFrame returns for the selected generated frame. At 2x there is one generated frame.");
        else if (s.release_point == 3)
            ImGui::TextWrapped("The next render starts after xrEndFrame returns for the last generated frame.");
        else if (s.release_point != 3)
            ImGui::TextWrapped("Experimental overlap: capture order alone cannot prove frame identity. Use After all generated frames submitted for serialized matching.");
        if(native_shader11()){
            combo("FG output destination", s.direct_xr_output, "Private image + copy\0Direct OpenXR image (experimental)\0");
            ImGui::Checkbox("Reuse whole-image eye captures (experimental)", &s.native11_skip_identity_crop);
            ImGui::TextWrapped("Per-eye swapchains and quad views: use the captured eye directly instead of copying it again.");
        }
        else if(!dx11&&!quad)
            combo("FG output destination", s.direct_xr_output, "Compact intermediate\0Direct OpenXR image (experimental)\0");
        if(!native_shader11())combo("FG latency diagnostics", s.latency_diagnostics, "Off\0Calibrated timeline (1 in 17 jobs)\0");
        ImGui::TreePop();
    }
    // Settings stay editable while FG is off; measurements only exist while it runs.
    if (!s.enabled) return;
    const auto rows = [](std::string_view object,
        std::initializer_list<std::pair<const char*, const char*>> fields, bool milliseconds = false) {
        if (ImGui::BeginTable("values", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 1.4F);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.F);
            for (const auto& [label, key] : fields) {
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", label); ImGui::TableNextColumn();
                if (member(object, key).empty()) ImGui::TextUnformatted("Unavailable");
                else if (milliseconds) ImGui::Text("%.2f ms", number(object, key));
                else { auto value = plain(member(object, key));
                    if (value == "true") value = "Yes"; else if (value == "false") value = "No";
                    ImGui::TextWrapped("%s", value.c_str()); }
            }
            ImGui::EndTable();
        }
    };
    ImGui::SeparatorText("Performance");
    ImGui::Text("Total frames (native / generated / fallback): %.0f / %.0f / %.0f",
        number(fg, "native_frames"), number(fg, "generated_frames"), number(fg, "fallback_frames"));
    const auto cadence = member(fg, "cadence");
    if (flag(cadence, "valid")) {
        ImGui::Text("FPS (game / native / generated): %.1f / %.1f / %.1f", number(cadence,"game_fps"), number(cadence,"native_fps"), number(cadence,"generated_fps"));
        ImGui::Text("Unique FPS output: %.1f FPS", number(cadence,"unique_fps"));
    } else ImGui::TextUnformatted("Measuring presentation cadence...");
    ImGui::SeparatorText("Content age at OpenXR submission (average ms)");
    ImGui::TextWrapped("Capture-to-submit age, not rendering time or headset display latency.");
    ImGui::TextWrapped("Reference values for comparing modes; not a measurement of added FG delay.");
    ImGui::PushID("latency");
    const auto presentation=member(fg,"presentation");
    if(number(presentation,"native_samples")>0)rows(presentation, {{"Real frame", "native_age_ms"}}, true);
    if(number(presentation,"generated_samples")>0)rows(presentation, {
        {"Generated frame (at its interpolated time)", "generated_age_ms"},
        {"Newest input used for the generated frame", "newest_input_age_ms"}}, true);
    if(number(presentation,"native_samples")==0&&number(presentation,"generated_samples")==0)ImGui::TextUnformatted("No content-age samples yet.");
    ImGui::PopID();
    ImGui::SeparatorText("GPU timings, both eyes (average / last ms)");
    const auto gpu = member(fg, "performance");
    if (!flag(gpu, "supported")) ImGui::TextWrapped("Install the matching OpenXR layer update.");
    else if (flag(gpu, "gpu_valid")) {
        // Same stages as the UEVR menu, for every FG method (DLSS-G and shader), in pipeline order.
        const auto extra = member(fg, "overhead");
        const bool stages = flag(extra, "supported");
        const bool focused = number(gpu, "foveated_eyes") > 0;
        const bool resized = !focused && number(gpu, "center_width") > 0 &&
            (number(gpu, "center_width") != number(gpu, "output_width") || number(gpu, "center_height") != number(gpu, "output_height"));
        if(native_shader11()&&number(gpu,"output_width")>0)
            ImGui::Text("Output per eye: %.0f x %.0f",number(gpu,"output_width"),number(gpu,"output_height"));
        else if (number(gpu, "center_width") > 0) {
            ImGui::Text("%s: %.0f x %.0f", focused ? "Center FG input" : "Full-frame FG input", number(gpu, "center_width"), number(gpu, "center_height"));
            if (focused) ImGui::Text("Periphery input: %.0f x %.0f", number(gpu, "peripheral_width"), number(gpu, "peripheral_height"));
            ImGui::Text("Output per eye: %.0f x %.0f", number(gpu, "output_width"), number(gpu, "output_height"));
        }
        if (ImGui::BeginTable("fg_gpu_timings", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthStretch, 1.6F);
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch, 1.F);
            const auto row = [](const char* label, double mean, double last, bool sampled = true) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextWrapped("%s", label); ImGui::TableNextColumn();
                if (sampled) ImGui::Text("%.3f / %.3f", mean, last); else ImGui::TextUnformatted("Waiting for GPU samples");
            };
            double sum{}, sum_last{};
            // Adds a sampled overhead stage to its group subtotal.
            const auto stage = [&](const char* label, const char* samples, const char* mean, const char* last, double& group, double& group_last) {
                const bool sampled = number(extra, samples) > 0;
                row(label, number(extra, mean), number(extra, last), sampled);
                if (sampled) { group += number(extra, mean); group_last += number(extra, last); }
            };
            if (stages) {
                double capture{}, capture_last{};
                stage("Guide capture (per real frame)", "guide_samples", "guide_ms", "guide_last_ms", capture, capture_last);
                stage(native_shader11()?"Color capture / eye crop (per copy)":"Color capture + guide handoff (per real frame)", "capture_samples", "capture_ms", "capture_last_ms", capture, capture_last);
                if(!native_shader11())row("  Capture subtotal", capture, capture_last, number(extra,"guide_samples")>0&&number(extra,"capture_samples")>0);
                sum += capture; sum_last += capture_last;
            }
            if(native_shader11())row("Shader generation (center + periphery)",number(gpu,"total_ms"),number(gpu,"total_last_ms"));
            else {
            row("Color / guide preparation", number(gpu, "preparation_ms"), number(gpu, "preparation_last_ms"));
            row(focused ? "DLSS-FG / shader center" : "FG full frame", number(gpu, "center_ms"), number(gpu, "center_last_ms"));
            if (focused || resized) row(resized ? "Output resize" : "Periphery + output assembly", number(gpu, "periphery_ms"), number(gpu, "periphery_last_ms"));
            row(stages ? "  Generation subtotal" : "FG GPU total", number(gpu, "total_ms"), number(gpu, "total_last_ms"));
            }
            sum += number(gpu, "total_ms"); sum_last += number(gpu, "total_last_ms");
            if (stages) {
                double present{}, present_last{};
                if(!native_shader11())stage("Generated output copy (per generated frame)", "publication_samples", "publication_ms", "publication_last_ms", present, present_last);
                stage(native_shader11()?"OpenXR image copy (per copy)":"OpenXR uploads (per presentation)", "upload_samples", "upload_ms", "upload_last_ms", present, present_last);
                if(!native_shader11()){
                row("  Presentation subtotal", present, present_last, number(extra,"publication_samples")>0&&number(extra,"upload_samples")>0);
                sum += present; sum_last += present_last;
                row("Measured GPU stage sum", sum, sum_last, number(extra,"guide_samples")>0&&number(extra,"capture_samples")>0&&number(extra,"publication_samples")>0&&number(extra,"upload_samples")>0);
                }
            }
            ImGui::EndTable();
        }
        const auto batch=member(fg,"batch_timing");
        if(flag(batch,"supported")&&flag(batch,"valid")&&ImGui::BeginTable("fg_batch_timings",2,ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_RowBg)){
            const auto row=[](const char* label,double mean,double last){
                ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextWrapped("%s",label);
                ImGui::TableNextColumn();ImGui::Text("%.3f / %.3f",mean,last);
            };
            row("Whole batch incl. output copies (per real frame)",number(batch,"batch_ms"),number(batch,"batch_last_ms"));
            const bool separated=flag(batch,"optical_separated"),flow=number(batch,"optical_regions")>0;
            if(separated&&flow)row("Optical flow preparation + exposed waits (per real frame)",number(batch,"optical_ms"),number(batch,"optical_last_ms"));
            auto phases=member(batch,"phases");
            for(unsigned i=0;i<3;++i){const auto start=phases.find('{'),end=phases.find('}',start);
                if(start==phases.npos||end==phases.npos)break;
                const auto phase=phases.substr(start,end-start+1);char label[96]{};
                const auto index=unsigned(number(phase,"index"));
                sprintf_s(label,"Generated stereo frame %u%s",index,index==1&&flow&&!separated?" (includes optical flow)":"");
                row(label,number(phase,"ms"),number(phase,"last_ms"));phases.remove_prefix(end+1);
            }
            ImGui::EndTable();
            ImGui::TextWrapped("Batch and generated-frame times cover both eyes, including composition and output copies. Capture, XR uploads and pacing are excluded.");
            if(separated&&flow)ImGui::TextWrapped("Generated-frame times exclude optical flow preparation and exposed waits. Optical engine work overlapping interpolation is not counted twice.");
        }
        ImGui::TextWrapped(native_shader11()?"Shader time covers both eyes. Copy times are per copy. Runtime work is excluded.":
            "Stage samples use different frame cadences and can overlap. Their sum is not total frame time; compositor work is excluded.");
    } else if (!flag(gpu, "gpu_supported") && number(gpu, "gpu_samples") == 0)
        ImGui::TextWrapped("GPU timestamps unavailable or FG has not started.");
    else ImGui::TextWrapped("Waiting for a complete GPU timing sample.");
    if (ImGui::TreeNode("FG capture and presentation diagnostics")) {
        if(quad){const auto guides=member(member(snapshot,"quad_views"),"fg");
            ImGui::TextUnformatted("Frame source: Four-view OpenXR submission");
            diagnostic_line(guides,"Captured view guides","snapshots");diagnostic_line(guides,"Assembled guide pairs","guide_pairs");
            diagnostic_line(guides,"Capture stage (8 = ready)","capture_stage");diagnostic_line(guides,"Assembly stage (4 = ready)","assembly_stage");
        }else{
        diagnostic_line(fg, "Captured guides", "captured_guides");
        diagnostic_line(fg, "Guide matching", "match_reason");
        }
        diagnostic_line(member(fg,"direct_output"), "Direct output fallback", "reason_text");
        if (ImGui::TreeNode("CPU waits and pacing")) {
            ImGui::TextWrapped("Rolling averages; separate stages can overlap.");
            rows(member(fg, "pacing_timeline"), {{"Game waits for credit", "game_wait_credit_ms"},
                {"Game BeginFrame gate", "game_begin_gate_ms"}, {"Presenter waits for content", "presenter_content_ms"},
                {"Deferred game credit", "credit_deferred_ms"}, {"Clock lateness", "clock_late_ms"},
                {"Native xrEndFrame", "native_xr_end_ms"}, {"Generated xrEndFrame", "generated_xr_end_ms"},
                {"FG submit to completion", "fg_submit_to_complete_ms"}}, true);
            ImGui::TreePop();
        }
        if (ImGui::Button("Copy FG diagnostic JSON")) ImGui::SetClipboardText(std::string(fg).c_str());
        ImGui::TextWrapped("The support bundle includes the complete diagnostic snapshot.");
        ImGui::TreePop();
    }
}

bool begin_tab(const char* label) {
    const bool selected = ImGui::BeginTabItem(label);
    test_control(label);
    if (!selected) return false;
    // Keep the header and tabs reachable while long pages scroll independently.
    ImGui::BeginChild(label, ImVec2(0, 0), ImGuiChildFlags_None);
    return true;
}

void end_tab() { ImGui::EndChild(); ImGui::EndTabItem(); }

std::string menu_key_name(unsigned key) {
    wchar_t name[64]{};
    const auto scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
    const auto extended = key == VK_INSERT || key == VK_DELETE || key == VK_HOME || key == VK_END ||
        key == VK_PRIOR || key == VK_NEXT || key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN;
    const auto length = scan ? GetKeyNameTextW(static_cast<LONG>((scan << 16) | (extended ? 1U << 24 : 0)), name, 64) : 0;
    if (length) {
        char utf8[192]{};
        const auto bytes = WideCharToMultiByte(CP_UTF8, 0, name, length, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
        if (bytes) return std::string(utf8, bytes);
    }
    return "Key " + std::to_string(key);
}

// Whole-system load (sampled once per second) and output FPS, shown above the tabs.
void draw_system_load(std::string_view snapshot) {
    const auto load = member(snapshot, "system_load");
    const auto vram = member(member(snapshot, "gpu_memory"), "local");
    const auto fg=member(snapshot,"fg");
    const auto cadence = member(fg, "cadence");
    if (flag(load, "valid")) {
        ImGui::Text("CPU: %.0f%% total, busiest core %.0f%%, game %.0f%%", number(load, "cpu_total_percent"),
            number(load, "cpu_busiest_core_percent"), number(load, "cpu_game_percent"));
        if (flag(load, "gpu_valid"))
            ImGui::Text("GPU: %.0f%% busy, game %.0f%%, video encode %.0f%%", number(load, "gpu_busy_percent"), number(load, "gpu_game_percent"), number(load, "gpu_encode_percent"));
        else ImGui::TextUnformatted("GPU: utilization counters unavailable");
    }
    if (flag(vram, "valid"))
        ImGui::Text("VRAM (game): %.2f / %.2f GB budget", number(vram, "usage_bytes") / 1073741824.0, number(vram, "budget_bytes") / 1073741824.0);
    if (flag(fg,"active")&&flag(cadence, "valid"))
        ImGui::Text("FPS: %.1f game, %.1f unique output", number(cadence, "game_fps"), number(cadence, "unique_fps"));
    else if(const auto ms=number(member(snapshot,"frame"),"present_ms");ms>0)
        ImGui::Text("FPS: %.1f game present",1000./ms);
}

void draw_donate_button(OverlayUiState& state) {
    // Do not remove this donation button or change its original destination when
    // this project is forked or integrated into another project.
    constexpr auto url = L"https://linktr.ee/clarkcheekykent";
    const float font = ImGui::GetFontSize();
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(214, 61, 85, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(232, 77, 102, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(184, 43, 66, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, font * .3F);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(.73F, .5F));
    const bool clicked = ImGui::Button("Donate", ImVec2(font * 6.6F, font * 1.7F));
    test_control("Donate");
    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    auto* draw = ImGui::GetWindowDrawList();
    const float size = font * .95F;
    const ImVec2 origin(min.x + font * .55F, (min.y + max.y - size) * .5F);
    const auto point = [&](float x, float y) { return ImVec2(origin.x + x * size, origin.y + y * size); };
    draw->PathLineTo(point(.5F, .95F));
    draw->PathBezierCubicCurveTo(point(.35F, .8F), point(-.05F, .5F), point(.03F, .25F));
    draw->PathBezierCubicCurveTo(point(.08F, -.02F), point(.38F, -.06F), point(.5F, .2F));
    draw->PathBezierCubicCurveTo(point(.62F, -.06F), point(.92F, -.02F), point(.97F, .25F));
    draw->PathBezierCubicCurveTo(point(1.05F, .5F), point(.65F, .8F), point(.5F, .95F));
    draw->PathFillConcave(IM_COL32(255, 255, 255, 255));
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("https://linktr.ee/clarkcheekykent");
    if (clicked && reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        state.message = "Could not open the donation page in your browser.";
}

void draw_overlay_ui(OverlayUiState& r,const OverlayRuntime& runtime,InputState& input,const char* renderer,const char* status,bool& open) {
    refresh(r, runtime);
    if (const auto rebound = consume_menu_key_rebind(input)) {
        if (rebound != VK_ESCAPE) {
            r.menu_key_error = save_menu_key(input, rebound) ? "" : "Could not save the menu key.";
        }
    }
    const float menu_scale = ImGui::GetIO().FontGlobalScale;
    const auto display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2((std::min)(620.F * menu_scale, display.x * .9F),
        (std::min)(650.F * menu_scale, display.y * .9F)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(display.x * .5F, display.y * .5F), ImGuiCond_Appearing, ImVec2(.5F, .5F));
    ImGui::SetNextWindowSizeConstraints(ImVec2(420, 300), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("Cheeky Foveated DLSS###CheekyStandalone", &open, ImGuiWindowFlags_NoCollapse)) {
        const auto key_name = menu_key_name(input.menu_key.load());
        if (ImGui::BeginTable("header", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Support", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.6F);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("v" CHEEKY_VERSION " | %s | %s | %s to close",
                runtime.host_name ? runtime.host_name : "Standalone", renderer, key_name.c_str());
            draw_system_load(r.snapshot);
            ImGui::PopTextWrapPos();
            ImGui::TableNextColumn();
            draw_donate_button(r);
            ImGui::EndTable();
        }
        // Routine confirmations stay hidden; errors and action results still show.
        if (!r.message.empty() && r.message != "Settings applied" && !r.message.starts_with("Ready."))
            ImGui::TextWrapped("%s", r.message.c_str());
        ImGui::Separator();
        const auto previous = r.draft;
        if (ImGui::BeginTabBar("controls")) {
            if (begin_tab("General")) {
                ImGui::SeparatorText("Menu");
                ImGui::Text("Toggle key: %s", key_name.c_str());
                if (input.rebinding) {
                    ImGui::TextUnformatted("Press one key. Escape cancels; modifier keys cannot be bound.");
                    if (ImGui::Button("Cancel key change")) input.rebinding = false;
                } else {
                    if (ImGui::Button("Change menu key")) begin_menu_key_rebind(input);
                    ImGui::SameLine();
                    if (ImGui::Button("Reset to F8"))
                        r.menu_key_error = save_menu_key(input, VK_F8) ? "" : "Could not save the menu key.";
                }
                if (!r.menu_key_error.empty()) ImGui::TextWrapped("%s", r.menu_key_error.c_str());
                bool headset_pointer = input.headset_pointer.load();
                if (ImGui::Checkbox("VR controllers and hands control the menu", &headset_pointer))
                    r.menu_key_error = save_headset_pointer(input, headset_pointer) ? "" : "Could not save the menu pointer setting.";
                if (std::string_view(renderer) != "Vulkan") {
                    ImGui::SeparatorText("Compatibility");
                    ImGui::Checkbox("DX11 -> DX12 transport", &r.draft.d3d11_use_d3d12_transport);
                    ImGui::TextWrapped("Required for DLSS-NR in DX11 games.");
                    ImGui::Checkbox("Wait for quad-view composition (DX12)", &r.draft.d3d12_quad_compose_wait);
                    ImGui::TextWrapped("Off: DX12 quad-view games such as MSFS return from xrEndFrame once the four-view composition is submitted, instead of waiting for the GPU to finish it.");
                    ImGui::Checkbox("Use lower DLSS hook (DX12)", &r.draft.d3d12_lower_hook);
                    test_control("D3D12LowerHook");
                    ImGui::TextWrapped("Off selects the higher call. Restart the game after changing this.");
                }
                ImGui::SeparatorText("Status");
                ImGui::Text("Host: %s", runtime.host_name ? runtime.host_name : "Standalone");
                ImGui::Text("Renderer: %s", renderer);
                diagnostic_line(r.snapshot, "Runtime ready", "ready");
                diagnostic_line(r.snapshot, "NVIDIA DLSS override", "nvidia_override_status");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shows observed NVIDIA cached DLSS runtime use, including overrides and automatic updates.");
                if (std::string_view(renderer) != "Vulkan") {
                    ImGui::Text("Configured DX11 path: %s", fg_requires_d3d12(r.draft) ? "DX12 transport (required by DLSS FG)" : r.draft.d3d11_use_d3d12_transport ? "DX12 transport" : "DX11 direct");
                    const auto active_hook = member(r.snapshot, "d3d12_lower_hook_active");
                    ImGui::Text("Active DLSS hook: %s", active_hook.empty() ? "Waiting for runtime" : active_hook == "true" ? "Lower" : "Higher");
                    if (member(r.snapshot, "d3d12_hook_restart_required") == "true")
                        ImGui::TextUnformatted("DLSS hook change saved for next game restart.");
                }
                ImGui::TextWrapped("Overlay: %s", status);
                end_tab();
            }
            if (begin_tab("Stereo / Gaze")) {
                if(!draw_quad_controls(r,runtime)){
                draw_gaze_status(r.draft, r.snapshot);
                draw_calibration_controls(r, runtime);
                ImGui::SeparatorText("Placement");
                draw_gaze(r.draft);
                if (ImGui::Button("Reset gaze defaults")) command(r, runtime, "defaults_gaze");
                if (ImGui::TreeNode("Quad-view games")) {
                    ImGui::TextWrapped("No quad-view session is running. These preferences apply when a game submits four views.");
                    draw_quad_preferences(r.draft);
                    ImGui::TreePop();
                }
                }
                end_tab();
            }
            if (begin_tab("DLSS-SR")) {
                draw_sr(r.draft, member(array_object(member(r.snapshot,"apis"),1),"reconstruction_feature") == "13",flag(member(r.snapshot,"quad_views"),"active"));
                if (ImGui::Button("Reset SR defaults")) command(r, runtime, "defaults_sr");
                if (ImGui::CollapsingHeader("Performance##sr", ImGuiTreeNodeFlags_DefaultOpen))
                    draw_sr_performance(r.snapshot, r.draft);
                end_tab();
            }
            if (begin_tab("Frame Gen (Experimental)")) {
                draw_fg(r.draft, r.snapshot, std::string_view(renderer)=="D3D11");
                if (ImGui::Button("Reset FG defaults")) command(r, runtime, "defaults_fg");
                end_tab();
            }
            if (begin_tab("DLSS-NR")) {
                diagnostic_line(r.snapshot, "Runtime state", "nr");
                if (r.draft.nr_enabled) {
                    const auto reason = plain(member(member(r.snapshot, "nr_details"), "skip_reason"));
                    if (!reason.empty()) warning(reason.c_str());
                }
                draw_nr(r.draft,flag(member(r.snapshot,"quad_views"),"active"));
                if (ImGui::Button("Reset NR history / retry")) command(r, runtime, "reset_nr");
                ImGui::SameLine();
                if (ImGui::Button("Reset NR defaults")) command(r, runtime, "defaults_nr");
                if (ImGui::CollapsingHeader("Performance##nr", ImGuiTreeNodeFlags_DefaultOpen))
                    draw_nr_performance(r.snapshot, r.draft);
                end_tab();
            }
            if (begin_tab("Diagnostics")) {
                draw_diagnostics_summary(r, status);
                ImGui::SeparatorText("Support");
                if (ImGui::Button("Report an issue...")) command(r, runtime, "report_issue");
                ImGui::SameLine();
                if (ImGui::Button("Create support ZIP")) command(r, runtime, "report");
                ImGui::SameLine();
                if (ImGui::Button("Show support ZIP")) command(r, runtime, "show_report");
                if (ImGui::Button("Copy diagnostic snapshot")) ImGui::SetClipboardText(r.snapshot.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copies the complete JSON snapshot for a bug report.");
                ImGui::SeparatorText("Details");
                draw_gaze_details(r.snapshot);
                draw_calibration(r, runtime);
                draw_view_details(r.snapshot);
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
                    // Bounded, independently scrolling region: the raw JSON is
                    // tens of kilobytes and would otherwise bury the page.
                    if (ImGui::BeginChild("snapshot", ImVec2(0, 240.F * ImGui::GetIO().FontGlobalScale),
                            ImGuiChildFlags_Borders)) ImGui::TextWrapped("%s", r.snapshot.c_str());
                    ImGui::EndChild();
                    ImGui::TreePop();
                }
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
    const auto position = ImGui::GetWindowPos();
    const auto size = ImGui::GetWindowSize();
    r.menu_x = position.x; r.menu_y = position.y;
    r.menu_width = size.x; r.menu_height = size.y;
    ImGui::End();
}
}
