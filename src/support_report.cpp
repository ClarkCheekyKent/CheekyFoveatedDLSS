#include "support_report.hpp"
#include "support_zip.hpp"
#include "support_summary.hpp"
#include "support_prompts.hpp"
#include "diagnostics.hpp"
#include "eye_calibration.hpp"
#include "dlss_nr.hpp"
#include "gaze_foveation.hpp"
#include "settings.hpp"
#include "version.h"

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include <shellapi.h>
#include <dxgi.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <sstream>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "version.lib")

namespace cheeky::foveated_dlss {
namespace {
namespace fs = std::filesystem;
constexpr wchar_t issue_base[] =
    L"https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/issues/new?template=bug_report.yml";
struct PreparedReport { fs::path zip; std::string markdown; };
std::future<PreparedReport> pending;
fs::path last_zip;
std::string last_markdown;
std::string status;
std::string pending_summary, last_summary;
SupportPrompts pending_prompts, last_prompts;

std::wstring url_encode(const std::string& text) {
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring result;
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            result += static_cast<wchar_t>(c);
        } else {
            result += L'%'; result += hex[c >> 4]; result += hex[c & 15];
        }
    }
    return result;
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

fs::path module_path(HMODULE module) {
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) throw std::runtime_error("Cannot locate module");
    path.resize(size);
    return path;
}

std::string file_version(const fs::path& path) {
    DWORD unused{};
    const auto size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (!size) return "unavailable";
    std::vector<char> data(size);
    VS_FIXEDFILEINFO* info{};
    UINT length{};
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) ||
        length < sizeof(VS_FIXEDFILEINFO)) return "unavailable";
    std::ostringstream out;
    out << HIWORD(info->dwFileVersionMS) << '.' << LOWORD(info->dwFileVersionMS)
        << '.' << HIWORD(info->dwFileVersionLS) << '.' << LOWORD(info->dwFileVersionLS);
    return out.str();
}

std::string settings_text(const Settings& s) {
    std::ostringstream out;
    out << std::boolalpha << "[CheekyFoveatedDLSS]\n";
    out << "enabled=" << s.enabled << '\n';
    out << "d3d11_use_d3d12_transport=" << s.d3d11_use_d3d12_transport << '\n';
    out << "peripheral_dlaa_enabled=" << s.peripheral_dlaa_enabled << '\n';
    out << "peripheral_dlaa_scale=" << s.peripheral_dlaa_scale << '\n';
    out << "center_preset=" << s.center_preset << '\n';
    out << "peripheral_dlaa_preset=" << s.peripheral_dlaa_preset << '\n';
    out << "width=" << s.width << '\n';
    out << "height=" << s.height << '\n';
    out << "x_offset=" << s.x_offset << '\n';
    out << "height_offset=" << s.height_offset << '\n';
    out << "invert_stereo_x_offset=" << s.invert_stereo_x_offset << '\n';
    out << "auto_stereo_alignment=" << s.auto_stereo_alignment << '\n';
    out << "aligned_height_offset=" << s.aligned_height_offset << '\n';
    out << "roundness=" << s.roundness << '\n';
    out << "transition_width=" << s.transition_width << '\n';
    out << "alignment_border_enabled=" << s.alignment_border_enabled << '\n';
    out << "center_mode=" << static_cast<unsigned>(s.center_mode) << '\n';
    out << "simulation_pattern=" << s.simulation_pattern << '\n';
    out << "show_next_jump_target=" << s.show_next_jump_target << '\n';
    out << "gaze_smoothing_ms=" << s.gaze_smoothing_ms << '\n';
    out << "gaze_quantization_pixels=" << s.gaze_quantization_pixels << '\n';
    out << "gaze_jump_reset_ratio=" << s.gaze_jump_reset_ratio << '\n';
    out << "nr_enabled=" << s.nr_enabled << '\n';
    out << "nr_foveated=" << s.nr_foveated << '\n';
    out << "nr_use_sr_foveation=" << s.nr_use_sr_foveation << '\n';
    out << "nr_alignment_border_enabled=" << s.nr_alignment_border_enabled << '\n';
    out << "nr_width=" << s.nr_width << '\n';
    out << "nr_height=" << s.nr_height << '\n';
    out << "nr_roundness=" << s.nr_roundness << '\n';
    out << "nr_transition_width=" << s.nr_transition_width << '\n';
    out << "nr_working_scale=" << s.nr_working_scale << '\n';
    out << "nr_preset=" << s.nr_preset << '\n';
    out << "nr_intensity=" << s.nr_intensity << '\n';
    out << "nr_local_tone_strength=" << s.nr_local_tone_strength << '\n';
    out << "nr_local_structure_strength=" << s.nr_local_structure_strength << '\n';
    out << "nr_skin_structure_strength=" << s.nr_skin_structure_strength << '\n';
    out << "nr_automatic_mask=" << s.nr_automatic_mask << '\n';
    out << "nr_ui_correction=" << s.nr_ui_correction << '\n';
    out << "nr_paper_white_scale=" << s.nr_paper_white_scale << '\n';
    out << "nr_hdr_transfer_strength=" << s.nr_hdr_transfer_strength << '\n';
    out << "nr_color_strength=" << s.nr_color_strength << '\n';
    out << "nr_depth_convention=" << s.nr_depth_convention << '\n';
    out << "nr_motion_scale_x_multiplier=" << s.nr_motion_scale_x_multiplier << '\n';
    out << "nr_motion_scale_y_multiplier=" << s.nr_motion_scale_y_multiplier << '\n';
    return out.str();
}

std::string diagnostics_text() {
    std::ostringstream out;
    out << std::boolalpha << "Report schema: 1\nAdd-on: " << CHEEKY_VERSION
        << "\nBuild: " << __DATE__ << ' ' << __TIME__ << '\n';
    for (const auto api : {DiagnosticApi::d3d11, DiagnosticApi::d3d12}) {
        const auto d = diagnostic_snapshot(api);
        out << "\n[" << (api == DiagnosticApi::d3d11 ? "DX11" : "DX12") << "]\n";
        out << "runtime_loaded=" << d.runtime_loaded << '\n';
        out << "streamline_detected=" << d.streamline_detected << '\n';
        out << "hook_discovered=" << d.hook_discovered << '\n';
        out << "direct_detour_installed=" << d.direct_detour_installed << '\n';
        out << "create_calls=" << d.create_calls << '\n';
        out << "evaluate_calls=" << d.evaluate_calls << '\n';
        out << "active_calls=" << d.active_calls << '\n';
        out << "received_input_width=" << d.received_input_width << '\n';
        out << "received_input_height=" << d.received_input_height << '\n';
        out << "received_output_width=" << d.received_output_width << '\n';
        out << "received_output_height=" << d.received_output_height << '\n';
        out << "motion_vector_width=" << d.motion_vector_width << '\n';
        out << "motion_vector_height=" << d.motion_vector_height << '\n';
        out << "motion_vector_space=" << motion_vector_space_name(d.motion_vector_space) << '\n';
        out << "transport_gpu_ms=" << d.transport_gpu_ms << '\n';
        out << "foveated_dlss_gpu_ms=" << d.foveated_dlss_gpu_ms << '\n';
        out << "peripheral_dlaa_gpu_ms=" << d.peripheral_dlaa_gpu_ms << '\n';
        out << "full_dlss_nr_gpu_ms=" << d.full_dlss_nr_gpu_ms << '\n';
        out << "foveated_dlss_nr_gpu_ms=" << d.foveated_dlss_nr_gpu_ms << '\n';
        out << "native_dlss_gpu_ms=" << d.native_dlss_gpu_ms << '\n';
        out << "foveated_frame_ms=" << d.foveated_frame_ms << '\n';
        out << "native_frame_ms=" << d.native_frame_ms << '\n';
        out << "has_private_result=" << d.has_private_result << '\n';
        out << "last_private_result=" << d.last_private_result << '\n';
        out << "last_result=0x" << std::hex << d.last_result << std::dec << '\n';
        out << "state=" << static_cast<unsigned>(d.state) << '\n';
        out << "d3d11_execution_path=" << static_cast<unsigned>(d.d3d11_execution_path) << '\n';
        out << "d3d11_transport_status=" << static_cast<unsigned>(d.d3d11_transport_status) << '\n';
        out << "d3d12_ngx_route=" << d3d12_ngx_route_name(d.d3d12_ngx_route) << '\n';
        out << "state_name=" << diagnostic_state_name(d.state) << '\n'
            << "execution_path=" << d3d11_execution_path_name(d.d3d11_execution_path) << '\n'
            << "transport_status=" << d3d11_transport_status_name(d.d3d11_transport_status) << '\n';
        const auto& c = d.passed_crop;
        out << "input_crop=" << c.input_base_x << ',' << c.input_base_y << ','
            << c.input_width << ',' << c.input_height << '\n'
            << "output_crop=" << c.output_base_x << ',' << c.output_base_y << ','
            << c.output_width << ',' << c.output_height << '\n';
    }
    out << "\n[Eye calibration]\n" << eye_calibration_json() << '\n';
    const auto g = gaze_diagnostics();
    out << "\n[OpenXR]\n";
    out << "runtime_name=" << g.runtime_name << '\n';
    out << "submitted_copies=" << g.submitted_copies << '\n';
    out << "alignment_source=" << g.alignment_source << '\n';
    out << "status_flags=" << g.status_flags << '\n';
    out << "sample_age_ms=" << g.sample_age_ms << '\n';
    out << "layer_present=" << g.layer_present << '\n';
    out << "abi_compatible=" << g.abi_compatible << '\n';
    out << "using_gaze=" << g.using_gaze << '\n';
    out << "mapping_ambiguous=" << g.mapping_ambiguous << '\n';
    out << "last_reset_reason=" << static_cast<unsigned>(g.last_reset_reason) << '\n';
    for (std::size_t eye = 0; eye < g.views.size(); ++eye) {
        const auto& v = g.views[eye];
        out << "\n[Eye " << eye << "]\n";
        out << "center_u=" << v.center_u << '\n';
        out << "center_v=" << v.center_v << '\n';
        out << "dlss_view_id=" << v.dlss_view_id << '\n';
        out << "stable_matches=" << v.stable_matches << '\n';
        out << "crop_delta_x=" << v.crop_delta_x << '\n';
        out << "crop_delta_y=" << v.crop_delta_y << '\n';
        out << "xr_resource=" << v.xr_resource << '\n';
        out << "xr_x=" << v.xr_x << '\n';
        out << "xr_width=" << v.xr_width << '\n';
        out << "candidate_view=" << v.candidate_view << '\n';
        out << "candidate_x=" << v.candidate_x << '\n';
        out << "has_candidate=" << v.has_candidate << '\n';
        out << "resource_mapped=" << v.resource_mapped << '\n';
        out << "packed_stereo_mapping=" << v.packed_stereo_mapping << '\n';
        out << "xr_y=" << v.xr_y << '\n';
        out << "xr_height=" << v.xr_height << '\n';
        out << "xr_array=" << v.xr_array << '\n';
        out << "candidate_resource=" << v.candidate_resource << '\n';
        out << "candidate_y=" << v.candidate_y << '\n';
        out << "candidate_width=" << v.candidate_width << '\n';
        out << "candidate_height=" << v.candidate_height << '\n';
        out << "copy_mapping=" << v.copy_mapping << '\n';
        out << "projection_mapping=" << v.projection_mapping << '\n';
        out << "marker_mapping=" << v.marker_mapping << '\n';
        out << "alignment_source=" << v.alignment_source << '\n';
        out << "aligned_u=" << v.aligned_u << '\n';
        out << "aligned_v=" << v.aligned_v << '\n';
    }
    const auto nr = dlss_nr_snapshot();
    out << "\n[DLSS-NR]\n";
    out << "state=" << dlss_nr_state_name(nr.state) << '\n';
    out << "route=" << dlss_nr_route_name(nr.route) << '\n';
    out << "candidate_calls=" << nr.candidate_calls << '\n';
    out << "evaluation_calls=" << nr.evaluation_calls << '\n';
    out << "failed_calls=" << nr.failed_calls << '\n';
    out << "last_result=" << nr.last_result << '\n';
    out << "output_width=" << nr.output_width << '\n';
    out << "output_height=" << nr.output_height << '\n';
    out << "region_base_x=" << nr.region_base_x << '\n';
    out << "region_base_y=" << nr.region_base_y << '\n';
    out << "region_width=" << nr.region_width << '\n';
    out << "region_height=" << nr.region_height << '\n';
    out << "working_width=" << nr.working_width << '\n';
    out << "working_height=" << nr.working_height << '\n';
    out << "intermediate_vram_bytes=" << nr.intermediate_vram_bytes << '\n';
    return out.str();
}

// Only a tail is read, including when a log is still open in the game.
void collect_log(std::vector<SupportFile>& files, std::ostringstream& manifest,
                 const fs::path& source, const std::string& name) {
    const HANDLE file = CreateFileW(source.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        manifest << name << ": unavailable (Windows error " << GetLastError() << ")\n";
        return;
    }
    LARGE_INTEGER size{};
    constexpr LONGLONG limit = 4 * 1024 * 1024;
    if (!GetFileSizeEx(file, &size)) {
        manifest << name << ": size unavailable\n";
        CloseHandle(file); return;
    }
    LARGE_INTEGER start{};
    start.QuadPart = (std::max)(0LL, size.QuadPart - limit);
    if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) {
        manifest << name << ": seek failed\n";
        CloseHandle(file); return;
    }
    std::string contents(static_cast<std::size_t>(size.QuadPart - start.QuadPart), '\0');
    DWORD read{};
    const bool ok = ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr) != FALSE;
    CloseHandle(file);
    if (!ok) { manifest << name << ": read failed\n"; return; }
    contents.resize(read);
    manifest << name << ": " << read << " bytes; "
             << (start.QuadPart ? "truncated to last 4 MiB" : "complete at capture") << '\n';
    files.push_back({name, std::move(contents)});
}

// Use a fence longer than any backtick run in captured text, so logs cannot
// accidentally terminate a code block and alter the report's Markdown structure.
std::string code_block(const std::string& text) {
    std::size_t longest{}, run{};
    for (const char c : text) {
        run = c == '`' ? run + 1 : 0;
        longest = (std::max)(longest, run);
    }
    const std::string fence((std::max)(std::size_t{3}, longest + 1), '`');
    return fence + "text\n" + text + "\n" + fence + "\n\n";
}

std::string issue_markdown(const std::vector<SupportFile>& files) {
    auto contents = [&](const char* name) {
        for (const auto& file : files) if (file.name == name) return file.contents;
        return std::string{};
    };
    std::string report = support_summary(contents("system.txt"), contents("diagnostics.txt"), contents("settings.ini"));
    report += "### Capture details and log availability\n\n" + code_block(contents("README.txt"));
    report += "### Recent log excerpts\n\nFull captured logs are in the attached ZIP. "
              "Excerpts below contain up to the last 2 KiB of each available log.\n\n";
    bool has_logs{};
    for (const auto& file : files) {
        if (!file.name.ends_with(".log")) continue;
        has_logs = true;
        auto excerpt = file.contents.substr(file.contents.size() > 2048 ? file.contents.size() - 2048 : 0);
        if (file.contents.size() > 2048) {
            const auto newline = excerpt.find('\n');
            if (newline != std::string::npos) excerpt.erase(0, newline + 1);
            excerpt = "[earlier log content omitted]\n" + excerpt;
        }
        report += "#### " + file.name + "\n\n" + code_block(excerpt.empty() ? "(empty)" : excerpt);
    }
    if (!has_logs) report += "No logs were available; see capture details above.\n";
    return report;
}

PreparedReport create_report(const fs::path& addon, const fs::path& game,
                       std::string settings, std::string diagnostics) {
    std::array<wchar_t, 32768> temp{};
    const auto size = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
    if (!size || size >= temp.size()) throw std::runtime_error("Cannot locate temporary directory");
    const fs::path root = fs::path(temp.data()) / L"CheekySupport";
    fs::create_directories(root);
    SYSTEMTIME now{};
    GetSystemTime(&now);
    wchar_t name[100]{};
    swprintf_s(name, L"Cheeky-report-%04u%02u%02u-%02u%02u%02u-%03u-%lu",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        now.wMilliseconds, GetCurrentProcessId());
    const fs::path folder = root / name;
    if (!fs::create_directory(folder)) throw std::runtime_error("Report folder already exists; retry");
    std::vector<SupportFile> files{{"settings.ini", std::move(settings)},
                                 {"diagnostics.txt", std::move(diagnostics)}};
    std::ostringstream manifest;
    manifest << "Capture time (UTC): " << utf8(name) << "\n"
        << "Review before attaching: logs may contain personal paths or identifiers.\n"
        << "Only add-on settings are exported; ReShade.ini is not included.\n"
        << "The crash log is optional and may belong to an earlier game session.\n\n";
    collect_log(files, manifest, addon.parent_path() / L"CheekyFoveatedDLSS.log", "CheekyFoveatedDLSS.log");
    collect_log(files, manifest, game.parent_path() / L"ReShade.log", "ReShade-game.log");
    if (addon.parent_path() != game.parent_path())
        collect_log(files, manifest, addon.parent_path() / L"ReShade.log", "ReShade-addon.log");
    collect_log(files, manifest, fs::path(temp.data()) / L"CheekyFoveatedDLSS_crash.log", "CheekyFoveatedDLSS_crash.log");

    std::ostringstream system;
    system << "Game: " << utf8(game.filename().wstring()) << "\nGame version: " << file_version(game)
           << "\nAdd-on version: " << CHEEKY_VERSION << "\nArchitecture: 64-bit\n";
    using RtlVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const auto rtl = reinterpret_cast<RtlVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    RTL_OSVERSIONINFOW os{};
    os.dwOSVersionInfoSize = sizeof(os);
    if (rtl && rtl(&os) == 0)
        system << "Windows: " << os.dwMajorVersion << '.' << os.dwMinorVersion << '.' << os.dwBuildNumber << '\n';
    IDXGIFactory* factory{};
    if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter* adapter{};
        for (UINT i = 0; factory->EnumAdapters(i, &adapter) == S_OK; ++i) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                system << "Adapter: " << utf8(desc.Description) << " (vendor=" << desc.VendorId
                    << ", device=" << desc.DeviceId << ", VRAM=" << desc.DedicatedVideoMemory << ")\n";
                LARGE_INTEGER version{};
                if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
                    system << "Driver: " << HIWORD(version.HighPart) << '.' << LOWORD(version.HighPart)
                        << '.' << HIWORD(version.LowPart) << '.' << LOWORD(version.LowPart) << '\n';
            }
            adapter->Release();
        }
        factory->Release();
    }
    system << "\nRelevant runtime files (loaded status and on-disk presence are separate):\n";
    for (const auto dll : {L"nvngx_dlss.dll", L"nvngx_dlssnr.dll", L"CheekyOpenXRLayer.dll",
                           L"ReShade64.dll", L"dxgi.dll", L"d3d11.dll", L"sl.interposer.dll", L"sl.dlss.dll"}) {
        system << utf8(dll) << ": ";
        if (const auto module = GetModuleHandleW(dll)) {
            system << "loaded, version " << file_version(module_path(module));
        } else {
            system << "not loaded";
        }
        std::error_code error;
        const bool by_game = fs::exists(game.parent_path() / dll, error);
        error.clear();
        const bool by_addon = fs::exists(addon.parent_path() / dll, error);
        system << "; beside game=" << (by_game ? "present" : "absent")
               << "; beside add-on=" << (by_addon ? "present" : "absent") << '\n';
    }
    files.push_back({"system.txt", system.str()});
    files.push_back({"README.txt", manifest.str()});
    auto markdown = issue_markdown(files);
    {
        std::ofstream preview(folder / L"issue-report.md", std::ios::binary);
        preview.exceptions(std::ios::failbit | std::ios::badbit);
        preview.write(markdown.data(), markdown.size());
        preview.close();
    }
    files.push_back({"issue-report.md", markdown});
    const auto partial = folder / L"report.partial";
    const auto zip = folder / (std::wstring(name) + L".zip");
    write_support_zip(partial, files);
    fs::rename(partial, zip);
    return {zip, std::move(markdown)};
}

bool open_target(const wchar_t* target, const wchar_t* args = nullptr) {
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target, args, nullptr, SW_SHOWNORMAL)) > 32;
}

void show_zip() {
    const auto args = L"/select,\"" + last_zip.wstring() + L"\"";
    if (!open_target(L"explorer.exe", args.c_str())) status = "Could not open Explorer. ZIP path shown below.";
}

std::wstring issue_url() {
    auto url = std::wstring(issue_base) + L"&title=Bug%20report&report=" +
        url_encode(utf8(last_zip.filename().wstring())) + L"&environment=" + url_encode(last_summary) +
        L"&problem=" + url_encode(last_prompts.problem) + L"&steps=" + url_encode(last_prompts.steps);
    // Settings and diagnostics fit in a normal issue URL. Keep the potentially
    // huge log excerpts and capture manifest in the ZIP/copyable report only.
    const auto end = last_markdown.find("### Capture details and log availability");
    const auto details = last_markdown.substr(0, end);
    const auto populated = url + L"&diagnostics=" + url_encode(details);
    if (populated.size() <= 7800) return populated;
    // Exceptional oversized hardware/runtime descriptions must not produce a
    // broken GitHub URL. Make the fallback visible in the field and overlay.
    return url + L"&diagnostics=" + url_encode(
        "This report exceeds the issue-link size limit. Use Copy detailed report in the add-on and paste it here.");
}

void open_issue() {
    const auto url = issue_url();
    if (url.find(L"exceeds%20the%20issue-link") != std::wstring::npos)
        status = "Report exceeds the link size limit. Use Copy detailed report and paste into Diagnostics and settings; attach the ZIP.";
    if (!open_target(url.c_str())) status = "Could not open browser. Use Copy issue link and open it manually.";
}
}

void draw_support_report(HMODULE addon) {
    try {
        if (pending.valid() && pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto report = pending.get();
            last_zip = std::move(report.zip);
            last_markdown = std::move(report.markdown);
            last_summary = pending_summary;
            last_prompts = pending_prompts;
            status = "Diagnostics and settings are prefilled on GitHub. Review them, then drag the selected ZIP into Support ZIP.";
            open_issue();
            show_zip();
        }
        ImGui::SeparatorText("Report a problem");
        ImGui::TextWrapped("Open a GitHub issue with system information, diagnostics and settings filled in, and prepare a ZIP of the logs. Review before submitting; logs may contain personal paths.");
        ImGui::BeginDisabled(pending.valid());
        const bool clicked = ImGui::Button("Report an issue...");
        ImGui::EndDisabled();
        if (clicked) {
            const auto addon_file = module_path(addon);
            const auto game_file = module_path(nullptr);
            auto settings = settings_text(current_settings());
            auto diagnostics = diagnostics_text();
            const auto gaze = gaze_diagnostics();
            const bool openxr_activity = gaze.layer_present && gaze.abi_compatible &&
                ((gaze.status_flags & CHEEKY_GAZE_STATUS_SESSION_FOCUSED) != 0U ||
                 std::any_of(gaze.views.begin(), gaze.views.end(),
                     [](const auto& view) { return view.resource_mapped; }));
            pending_prompts = support_prompts(utf8(game_file.filename().wstring()),
                openxr_activity, gaze.runtime_name);
            pending_summary = std::string("Add-on: ") + CHEEKY_VERSION +
                "\nGame: " + utf8(game_file.filename().wstring()) +
                "\nDX11: " + diagnostic_state_name(diagnostic_snapshot(DiagnosticApi::d3d11).state) +
                "\nDX12: " + diagnostic_state_name(diagnostic_snapshot(DiagnosticApi::d3d12).state);
            pending = std::async(std::launch::async, create_report, addon_file, game_file,
                                 std::move(settings), std::move(diagnostics));
            status = "Preparing support ZIP...";
        }
        if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
        if (!last_zip.empty()) {
            ImGui::TextWrapped("%s", utf8(last_zip.wstring()).c_str());
            if (ImGui::Button("Show ZIP")) show_zip();
            ImGui::SameLine();
            if (ImGui::Button("Copy detailed report")) {
                ImGui::SetClipboardText(last_markdown.c_str());
                status = "Detailed report copied. Paste into Diagnostics and settings on GitHub.";
            }
            if (ImGui::Button("Review report")) {
                const auto preview = last_zip.parent_path() / L"issue-report.md";
                if (!open_target(preview.c_str())) status = "Could not open report. Find issue-report.md beside the ZIP.";
            }
            ImGui::SameLine();
            if (ImGui::Button("Open GitHub issue")) open_issue();
            ImGui::SameLine();
            if (ImGui::Button("Copy issue link")) ImGui::SetClipboardText(utf8(issue_url()).c_str());
        }
    } catch (const std::exception& error) {
        status = std::string("Report failed: ") + error.what() + ". Click Report an issue to retry.";
    }
}

void finish_support_report() noexcept {
    if (pending.valid()) { try { pending.get(); } catch (...) {} }
}
}
