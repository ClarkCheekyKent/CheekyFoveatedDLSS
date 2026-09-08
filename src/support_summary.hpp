#pragma once

#include "cheeky_gaze_abi.h"
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace cheeky::foveated_dlss {
// Summarize the captured text, never re-sample live state while formatting.
inline std::string support_summary(const std::string& system,
                                   const std::string& diagnostics,
                                   const std::string& settings) {
    using Fields = std::map<std::string, std::string>;
    std::map<std::string, Fields> sections;
    auto parse = [&](const std::string& text) {
        std::istringstream in(text);
        std::string line, section;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.starts_with('[') && line.ends_with(']')) section = line.substr(1, line.size() - 2);
            else if (const auto equals = line.find('='); equals != std::string::npos)
                sections[section][line.substr(0, equals)] = line.substr(equals + 1);
        }
    };
    parse(diagnostics); parse(settings);
    const auto get = [&](const std::string& section, const std::string& key) {
        const auto s = sections.find(section);
        if (s == sections.end()) return std::string("unavailable");
        const auto f = s->second.find(key);
        return f == s->second.end() ? std::string("unavailable") : f->second;
    };
    const auto setting = [&](const char* key) { return get("CheekyFoveatedDLSS", key); };
    const bool nr = setting("nr_enabled") == "true";
    std::size_t longest{}, run{};
    for (const auto* text : {&system, &diagnostics, &settings}) {
        run = 0;
        for (const char c : *text) {
            run = c == '`' ? run + 1 : 0;
            if (run > longest) longest = run;
        }
    }
    const std::string fence(longest < 3 ? 3 : longest + 1, '`');
    std::ostringstream out;
    out << "### System\n\n" << fence << "text\n";
    std::istringstream sys(system);
    std::string line;
    std::set<std::string> adapters;
    bool keep_driver{};
    while (std::getline(sys, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("Adapter:")) {
            keep_driver = line.find("Microsoft Basic Render Driver") == std::string::npos && adapters.insert(line).second;
            if (keep_driver) out << line << '\n';
        } else if (line.starts_with("Driver:")) {
            if (keep_driver) out << line << '\n';
        } else if (const auto dll = line.find(".dll:"); dll != std::string::npos) {
            const auto name = line.substr(0, dll);
            if (name == "dxgi" || name == "d3d11") continue;
            if (name == "nvngx_dlssnr" && !nr) continue;
            if (name == "CheekyOpenXRLayer" && setting("center_mode") != "1" && setting("auto_stereo_alignment") != "true") continue;
            if (name.starts_with("sl.") && line.find(": loaded") == std::string::npos) continue;
            out << line.substr(0, line.find(';')) << '\n';
        } else if (!line.empty() && !line.starts_with("Relevant runtime")) out << line << '\n';
    }
    if (adapters.empty()) out << "Hardware GPU: not identified; see system.txt in ZIP\n";
    // Retain build identity without repeating the version/schema headers.
    const auto build = diagnostics.find("Build:");
    if (build != std::string::npos) out << diagnostics.substr(build, diagnostics.find('\n', build) - build) << '\n';
    out << fence << "\n\n### Feature status\n\n";
    for (const auto api : {"DX11", "DX12"}) {
        const auto evaluations = get(api, "evaluate_calls");
        out << "- " << api << ": " << (evaluations == "0" ? "no evaluations recorded" : get(api, "state_name")) << '\n';
    }
    out << "- DLSS-SR foveation: " << (setting("enabled") == "true" ? "enabled" : "disabled") << '\n'
        << "- Peripheral DLAA: " << (setting("peripheral_dlaa_enabled") == "true" ? "enabled" : "disabled") << '\n'
        << "- DLSS-NR: " << (nr ? get("DLSS-NR", "state") : "disabled") << "\n\n";
    if (setting("center_mode") == "1" && get("OpenXR", "using_gaze") == "false")
        out << "**Check:** OpenXR gaze is selected, but gaze was not being used at capture time. This snapshot alone does not establish why.\n\n";
    if (setting("auto_stereo_alignment") == "true" && get("OpenXR", "alignment_source") == "0")
        out << "**Check:** Automatic alignment is enabled, but the latest evaluated view reports manual fallback.\n\n";
    out << "### Relevant diagnostics\n\n" << fence << "text\n";
    auto row = [&](const std::string& section, const char* key, const char* label) {
        out << label << ": " << get(section, key) << '\n';
    };
    auto timing = [&](const std::string& section, const char* key, const char* label) {
        const auto value = get(section, key);
        out << label << ": " << (value == "0" ? "not sampled / unavailable" : value + " ms") << '\n';
    };
    for (const auto api : {"DX11", "DX12"}) {
        if (get(api, "evaluate_calls") == "0") continue;
        out << '[' << api << "]\n";
        row(api, "state_name", "State");
        if (std::string(api) == "DX12") row(api, "d3d12_ngx_route", "NGX route");
        else {
            row(api, "execution_path", "Execution path");
            row(api, "transport_status", "DX12 transport");
        }
        out << "Evaluations: " << get(api, "evaluate_calls") << "; active: " << get(api, "active_calls") << '\n';
        row(api, "last_result", "Last NGX result");
        if (get(api, "has_private_result") == "true") row(api, "last_private_result", "Last private NGX result");
        out << "Input: " << get(api, "received_input_width") << 'x' << get(api, "received_input_height")
            << "; output: " << get(api, "received_output_width") << 'x' << get(api, "received_output_height") << '\n'
            << "Motion vectors: " << get(api, "motion_vector_width") << 'x' << get(api, "motion_vector_height")
            << " (" << get(api, "motion_vector_space") << ")\n";
        row(api, "input_crop", "Input crop (x,y,w,h)");
        row(api, "output_crop", "Output crop (x,y,w,h)");
        timing(api, "foveated_dlss_gpu_ms", "Foveated DLSS GPU");
        if (setting("peripheral_dlaa_enabled") == "true") timing(api, "peripheral_dlaa_gpu_ms", "Peripheral DLAA GPU");
        timing(api, "native_dlss_gpu_ms", "Native DLSS GPU");
        if (nr) {
            timing(api, "full_dlss_nr_gpu_ms", "Full DLSS-NR GPU");
            timing(api, "foveated_dlss_nr_gpu_ms", "Foveated DLSS-NR GPU");
        }
        if (std::string(api) == "DX11" && setting("d3d11_use_d3d12_transport") == "true") timing(api, "transport_gpu_ms", "Transport GPU");
        out << '\n';
    }
    out << "[OpenXR]\n";
    row("OpenXR", "runtime_name", "Runtime");
    row("OpenXR", "layer_present", "Layer loaded");
    row("OpenXR", "abi_compatible", "Compatible ABI");
    row("OpenXR", "using_gaze", "Using gaze");
    const auto source_name = [](const std::string& source) {
        return source == "0" ? "Manual fallback" : source == "1" ? "Streamline" : source == "2" ? "OpenXR" : "Unknown";
    };
    out << "Alignment source: " << source_name(get("OpenXR", "alignment_source")) << '\n';
    row("OpenXR", "mapping_ambiguous", "Mapping ambiguous");
    row("OpenXR", "sample_age_ms", "Sample age (ms)");
    const auto flag_text = get("OpenXR", "status_flags");
    if (flag_text != "unavailable") {
        const auto flags = std::stoul(flag_text);
        for (const auto& flag : {
            std::pair{CHEEKY_GAZE_STATUS_LAYER_ACTIVE, "Layer active"},
            std::pair{CHEEKY_GAZE_STATUS_EXTENSION_ENABLED, "Eye-tracking extension enabled"},
            std::pair{CHEEKY_GAZE_STATUS_SYSTEM_SUPPORTED, "Eye tracking supported"},
            std::pair{CHEEKY_GAZE_STATUS_SESSION_FOCUSED, "Session focused"},
            std::pair{CHEEKY_GAZE_STATUS_ACTION_ACTIVE, "Gaze action active"},
            std::pair{CHEEKY_GAZE_STATUS_GAZE_VALID, "Gaze valid"},
            std::pair{CHEEKY_GAZE_STATUS_MAPPING_READY, "Mapping ready"},
            std::pair{CHEEKY_GAZE_STATUS_UNSUPPORTED_VIEW_CONFIG, "Unsupported view configuration"},
            std::pair{CHEEKY_GAZE_STATUS_AMBIGUOUS_RESOURCE, "Ambiguous resource"},
            std::pair{CHEEKY_GAZE_STATUS_SIMULATED, "Simulated gaze"}})
            out << flag.second << ": " << ((flags & flag.first) ? "yes" : "no") << '\n';
    }
    for (const auto eye : {"Eye 0", "Eye 1"}) {
        out << eye << ": mapped=" << get(eye, "resource_mapped")
            << "; packed stereo=" << get(eye, "packed_stereo_mapping")
            << "; center=" << get(eye, "center_u") << ',' << get(eye, "center_v")
            << "; alignment=" << source_name(get(eye, "alignment_source")) << '\n';
    }
    if (nr) {
        out << "\n[DLSS-NR]\n";
        for (const auto key : {"state", "route", "evaluation_calls", "failed_calls", "last_result", "region_width", "region_height", "working_width", "working_height"})
            row("DLSS-NR", key, key);
    }
    out << fence << "\n\n<details>\n<summary>All current add-on settings</summary>\n\n" << fence << "ini\n"
        << settings << '\n' << fence << "\n\n</details>\n\n"
        << "Full diagnostic counters, resource identifiers and logs are retained in the attached support ZIP.\n\n";
    return out.str();
}
}
