#include "settings_io.hpp"
#include "../shared/fg_dlssg_preset.hpp"
#include <Windows.h>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <type_traits>

namespace cheeky::foveated_dlss {
namespace {
template<class T> bool parse(std::string_view s, T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        if (s == "true" || s == "1") { value = true; return true; }
        if (s == "false" || s == "0") { value = false; return true; }
        return false;
    } else if constexpr (std::is_enum_v<T>) {
        std::uint32_t n{};
        constexpr auto maximum = std::is_same_v<T, EyeCalibrationMethod> ? 3U : std::is_same_v<T, NrProcessingOrder> ? 1U : 2U;
        if (!parse(s, n) || n > maximum) return false;
        value = static_cast<T>(n); return true;
    } else {
        T n{};
        const auto r = std::from_chars(s.data(), s.data() + s.size(), n);
        if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return false;
        if constexpr (std::is_floating_point_v<T>) if (!std::isfinite(n)) return false;
        value = n; return true;
    }
}
template<class T> auto number(T n) {
    if constexpr (std::is_enum_v<T>) return static_cast<std::uint32_t>(n);
    else return n;
}
std::string_view trim(std::string_view s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == s.npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
}
bool set_named_setting(Settings& s, std::string_view key, std::string_view value) {
    if(key=="FgDlssgPreset") {unsigned n{};if(!parse(value,n)||!cheeky::fg::valid_dlssg_preset(n))return false;s.fg.dlssg_preset=n;return true;}
    if(key=="FgMultiplier") {unsigned n{};if(!parse(value,n)||n<2||n>4)return false;s.fg.multiplier=n;return true;}
    if(key=="FgCenterMethod") {unsigned method{};if(!parse(value,method)||!cheeky_fg_center_valid(method))return false;s.fg.center_method=method;return true;}
    if(key=="FgPeripheralMethod") {unsigned method{};if(!parse(value,method)||!cheeky_fg_periphery_valid(method))return false;s.fg.peripheral_method=method;return true;}
    if(key=="FgOpticalCenterPreset"||key=="FgOpticalPeripheralPreset") {unsigned n{};if(!parse(value,n)||n>2)return false;(key=="FgOpticalCenterPreset"?s.fg.optical.center_preset:s.fg.optical.peripheral_preset)=n;return true;}
    if(key=="FgOpticalCenterPercent"||key=="FgOpticalPeripheralPercent") {unsigned n{};if(!parse(value,n)||(n!=25&&n!=50&&n!=75&&n!=100))return false;(key=="FgOpticalCenterPercent"?s.fg.optical.center_percent:s.fg.optical.peripheral_percent)=n;return true;}
    if(key=="FgOpticalCenterGrid"||key=="FgOpticalPeripheralGrid") {unsigned n{};if(!parse(value,n)||(n!=1&&n!=2&&n!=4))return false;(key=="FgOpticalCenterGrid"?s.fg.optical.center_grid:s.fg.optical.peripheral_grid)=n;return true;}
    if(key=="FgOpticalCenterValidation"||key=="FgOpticalPeripheralValidation") {unsigned n{};if(!parse(value,n)||n>2)return false;(key=="FgOpticalCenterValidation"?s.fg.optical.center_validation:s.fg.optical.peripheral_validation)=n;return true;}
    if(key=="QuadSavedSourceCount"){unsigned count{};if(!parse(value,count)||count>8)return false;s.quad_saved_source_count=count;return true;}
    if(key.size()==13&&key.starts_with("QuadView")&&key.ends_with("Role")&&key[8]>='1'&&key[8]<='8'){
        unsigned role{};if(!parse(value,role)||role>4)return false;s.quad_saved_roles[key[8]-'1']=role;return true;}
    if (key == "FgTargetRefreshHz") {
        std::uint32_t hz{};if(!parse(value,hz)||(hz!=0&&(hz<30||hz>360)))return false;
        s.fg.target_refresh_hz=hz;return true;
    }
    if (key == "FgPacingClock") {
        std::uint32_t mode{}; if (!parse(value, mode) || mode > 2U) return false;
        s.fg.pacing_clock=mode; return true;
    }
    // Retired: native D3D11 now follows FgReleasePoint. Accept older files.
    if (key == "FgNative11Release") return true;
    // Retired options now fixed by the runtime; older files and menus may still send them.
    for (const auto retired : {"FgHooklessMode3", "FgBackbufferFrameId", "FgDepthBits", "FgMotionBits", "FgSubmitRepeats", "FgGuideCaptureMode",
            "FgCompactStorage", "FgColorCaptureMode", "FgUploadMode", "FgDirectPeripheralShading", "FgDiagnosticMode",
            "D3D11D3D12SharedGuides", "D3D11D3D12SingleBuffer", "HookRescanOnModuleChange",
            "FgDedicatedPresentQueue", "FgGpuWaitedPresent"})
        if (key == retired) return true;
    if (key == "FgQueuePriority") {
        std::uint32_t mode{}; if (!parse(value, mode) || mode > 2U) return false;
        s.fg.queue_priority=mode==1u?1u:2u; return true;
    }
    if (key == "FgLatencyDiagnostics" || key == "FgDirectXrOutput") {
        std::uint32_t mode{}; if (!parse(value, mode) || mode > 1U) return false;
        if(key == "FgDirectXrOutput") s.fg.direct_xr_output=mode;
        else s.fg.latency_diagnostics=mode;
        return true;
    }
    if (key == "FgWaitForCompletion") { // Migrate the previous diagnostic checkbox.
        bool hold{}; if (!parse(value, hold)) return false;
        s.fg.release_point = hold ? 2U : 0U; return true;
    }
    if (key == "FgReleasePoint") {
        std::uint32_t point{}; if (!parse(value, point) || point > 8U) return false;
        s.fg.release_point = point; return true;
    }
#define CHEEKY_SETTING(name, field) if (key == name) return parse(value, s.field);
#include "settings_fields.inc"
#undef CHEEKY_SETTING
    return false;
}
std::string_view setting_group(std::string_view key) {
    if(key.starts_with("Quad"))return "gaze";
    if (key.starts_with("Fg")) return "fg";
    if (key.starts_with("Nr")) return "nr";
    for (const auto gaze : {"XOffset", "HeightOffset", "InvertStereoXOffset", "CenterMode",
            "AutoStereoAlignment", "EyeCalibrationContinuous", "EyeCalibrationMethod", "EyeCalibrationLearnedMethod", "EyeCalibrationLearnedSignature", "EyeCalibrationLearnedSessions", "AlignedHeightOffset", "ShowNextJumpTarget", "SimulationPattern",
            "ManualStereoMapping", "ManualStereoRightFirst", "ManualQuadViewAssignment", "ShowQuadViewNumbers", "IndependentOpenXRGaze",
            "GazeSmoothingMs", "GazeHoldMs", "GazeQuantizationPixels", "GazeJumpResetRatio",
            "AfwManualCoverage", "AfwAutomaticCoverage", "AfwWarpMargin"})
        if (key == gaze) return "gaze";
    return "sr";
}
bool reset_settings_group(Settings& s, std::string_view group) {
    if (group != "sr" && group != "nr" && group != "gaze" && group != "fg") return false;
    const Settings defaults;
#define CHEEKY_SETTING(name, field) if (!std::string_view(name).starts_with("EyeCalibrationLearned") && setting_group(name) == group) s.field = defaults.field;
#include "settings_fields.inc"
#undef CHEEKY_SETTING
    return true;
}
std::string setting_groups_json() {
    std::ostringstream out; out << '{'; bool first = true;
#define CHEEKY_SETTING(name, field) if (!first) out << ','; first = false; out << '"' << name << "\":\"" << setting_group(name) << '"';
#include "settings_fields.inc"
#undef CHEEKY_SETTING
    out << '}'; return out.str();
}
std::string serialize_settings(const Settings& s) {
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(9);
    out << "[CheekyFoveatedDLSS]\nSchemaVersion=1\n";
#define CHEEKY_SETTING(name, field) out << name << '=' << number(s.field) << '\n';
#include "settings_fields.inc"
#undef CHEEKY_SETTING
    return out.str();
}
std::string settings_json(const Settings& s) {
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(9) << std::boolalpha;
    out << '{'; bool first = true;
#define CHEEKY_SETTING(name, field) if (!first) out << ','; first = false; out << '"' << name << "\":" << number(s.field);
#include "settings_fields.inc"
#undef CHEEKY_SETTING
    out << '}'; return out.str();
}
std::string json_escape(std::string_view s) {
    std::string out; constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out;
}
bool read_settings_file(const std::filesystem::path& path, Settings& s, std::string& error) {
    std::ifstream in(path);
    if (!in) { error = "Cannot read settings file"; return false; }
    auto candidate = s;
    candidate.fg = Settings{}.fg;
    candidate.d3d12_lower_hook = true; // Older files use the new lower-hook default.
    candidate.manual_stereo_mapping = false;
    candidate.manual_stereo_right_first = false;
    candidate.quad_manual_assignment=false;candidate.quad_show_numbers=false;
    candidate.quad_saved_roles={};candidate.quad_saved_source_count=0;
    candidate.independent_openxr_gaze = false;
    candidate.fix_motion_blur = false; // Compatibility copies are always opt-in.
    // Older files predate this additive key and use After, even when loaded
    // over a currently configured Before setting. Commit only after validation.
    candidate.nr_processing_order = NrProcessingOrder::after_upscaling;
    candidate.nr_style = 0U; // Older files used the hardcoded Standard style.
    candidate.afw_manual_coverage = false; // Older AFW builds used the centered safety region.
    candidate.afw_automatic_coverage = false;
    candidate.afw_warp_margin = Settings{}.afw_warp_margin;
    std::string line; bool section = false, explicit_release_point = false;
    while (std::getline(in, line)) {
        const auto text = trim(line);
        if (text.empty() || text.front() == ';' || text.front() == '#') continue;
        if (text.front() == '[') { section = text == "[CheekyFoveatedDLSS]"; continue; }
        if (!section) continue;
        const auto split = text.find('='); if (split == text.npos) continue;
        const auto key = trim(text.substr(0, split)), value = trim(text.substr(split + 1));
        if (key == "FgWaitForCompletion" && explicit_release_point) continue;
        if (key == "FgReleasePoint") explicit_release_point = true;
        if (key == "SchemaVersion") {
            if (value != "1") { error = "Unsupported settings schema"; return false; }
        } else {
            // Unknown future keys are ignored, but malformed known values reject
            // the file as a transaction instead of applying half a configuration.
            bool known = key == "FgWaitForCompletion";
#define CHEEKY_SETTING(name, field) known |= key == name;
#include "settings_fields.inc"
#undef CHEEKY_SETTING
            if (known && !set_named_setting(candidate, key, value)) {
                error = "Invalid setting: " + std::string(key); return false;
            }
        }
    }
    if (in.bad()) { error = "Settings read failed"; return false; }
    s = candidate; error.clear(); return true;
}
bool write_settings_file(const std::filesystem::path& path, const Settings& s, std::string& error) {
    auto temporary = path; temporary += L".tmp";
    { std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
      out << serialize_settings(s); out.flush();
      if (!out) { error = "Cannot write settings file"; return false; } }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Cannot replace settings file"; return false;
    }
    error.clear(); return true;
}
}
