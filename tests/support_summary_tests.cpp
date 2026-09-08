#include "support_summary.hpp"
#include "support_prompts.hpp"
#include <iostream>

int run_support_summary_tests() {
    using cheeky::foveated_dlss::support_summary;
    const std::string system = "Game: Test.exe\nAdapter: Test GPU\nDriver: 1.2\nAdapter: Test GPU\nDriver: 1.2\nAdapter: Microsoft Basic Render Driver\nDriver: 0\nnvngx_dlss.dll: loaded, version 1; beside game=present\nnvngx_dlssnr.dll: loaded, version 2\ndxgi.dll: loaded\n";
    const std::string diagnostics = "[DX11]\nevaluate_calls=0\n[DX12]\nevaluate_calls=100\nactive_calls=100\nstate_name=Active\nd3d12_ngx_route=Core runtime\nfoveated_dlss_gpu_ms=0\nruntime_loaded=true\nxr_resource=123456\n[OpenXR]\nusing_gaze=false\nalignment_source=0\nstatus_flags=119\n[DLSS-NR]\nstate=Active\nroute=Native\n";
    const std::string settings = "[CheekyFoveatedDLSS]\nnr_enabled=false\nenabled=true\ncenter_mode=1\nauto_stereo_alignment=true\nwidth=0.22\n";
    const auto report = support_summary(system, diagnostics, settings);
    int failures{};
    auto check = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << "Support summary: " << message << '\n'; ++failures; }
    };
    check(report.find("DX11: no evaluations recorded") != std::string::npos, "collapse inactive API");
    check(report.find("[DX11]") == std::string::npos, "omit inactive detail");
    check(report.find("[DX12]") != std::string::npos && report.find("Core runtime") != std::string::npos, "retain active route");
    check(report.find("DLSS-NR: disabled") != std::string::npos && report.find("[DLSS-NR]") == std::string::npos, "collapse disabled NR");
    check(report.find("nvngx_dlssnr.dll") == std::string::npos, "omit disabled dependency");
    check(report.find("dxgi.dll") == std::string::npos && report.find("123456") == std::string::npos, "omit low-level noise");
    check(report.find("Adapter: Test GPU") == report.rfind("Adapter: Test GPU"), "deduplicate GPU");
    check(report.find("Microsoft Basic") == std::string::npos, "omit software adapter beside hardware");
    check(report.find("not sampled / unavailable") != std::string::npos, "zero is not measured cost");
    check(report.find("Session focused: no") != std::string::npos && report.find("Gaze valid: yes") != std::string::npos, "decode flags");
    check(report.find("gaze was not being used") != std::string::npos && report.find("manual fallback") != std::string::npos, "highlight requested/observed mismatch");
    check(report.find("<details>") != std::string::npos && report.find(settings) != std::string::npos, "retain every setting in details");
    auto enabled = settings;
    enabled.replace(enabled.find("nr_enabled=false"), 16, "nr_enabled=true");
    const auto enabled_report = support_summary(system, diagnostics, enabled);
    check(enabled_report.find("[DLSS-NR]") != std::string::npos && enabled_report.find("nvngx_dlssnr.dll") != std::string::npos, "retain enabled NR details and dependency");
    const auto prompts = cheeky::foveated_dlss::support_prompts("TestGame.exe", true, "Pimax OpenXR");
    check(prompts.problem.find("TestGame.exe") != std::string::npos && prompts.steps.find("Launch TestGame.exe in VR mode") != std::string::npos, "prefill game and detected VR");
    check(prompts.problem.find("**OpenXR runtime:** Pimax OpenXR") != std::string::npos && prompts.problem.find("**Headset:** Not detected") != std::string::npos, "do not mistake runtime for headset model");
    const auto unknown = cheeky::foveated_dlss::support_prompts("TestGame.exe", false, "");
    check(unknown.problem.find("confirm desktop or VR") != std::string::npos, "absence of OpenXR is not proof of desktop mode");
    return failures;
}
