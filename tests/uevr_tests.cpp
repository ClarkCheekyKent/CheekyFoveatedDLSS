#include "runtime_api.hpp"
#include "settings_io.hpp"
#include "processing_owner.hpp"
#include "late_attach_tests.hpp"
#include "runtime_search.hpp"
#include "frame_cadence.hpp"
#include "gpu_memory.hpp"
#include "support_bundle.hpp"
#include "../shared/cheeky_fg_bridge.hpp"
#include "mock_ngx_parameters.hpp"
#include <uevr/API.h>
#include <d3d12.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <string>
#include <vector>
#include "../src/fg_source_lifecycle.hpp"

using namespace cheeky::foveated_dlss;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void check(HRESULT hr, const char* why) { require(SUCCEEDED(hr), why); }
std::wstring directory;
std::string received, loader_received;
unsigned present_registrations{}, reset_registrations{}, custom_registrations{};
UEVR_OnPresentCb present{};
UEVR_OnDeviceResetCb reset{};
UEVR_OnCustomEventCb custom{};
void* cached_compositor{};
bool openvr_active{};
bool hmd_active{true};
std::string rendering_mode{"3"};
void get_mod_value(const char* key, char* value, unsigned size) {
    if (std::string(key) == "VR_RenderingMethod") strncpy_s(value, size, rendering_mode.c_str(), _TRUNCATE);
}
bool is_hmd_active() { return hmd_active; }
unsigned hmd_extent() { return 256; }
void get_projection(UEVR_Eye eye, UEVR_Matrix4x4f* out) {
    *out = {};
    out->m[0][0] = out->m[1][1] = out->m[2][3] = 1.F; out->m[3][2] = 10.F;
    out->m[2][0] = eye == 0 ? .4F : -.2F;
}
bool is_openvr() { return openvr_active; }
UEVR_IVRCompositor get_compositor() { return reinterpret_cast<UEVR_IVRCompositor>(cached_compositor); }
bool add_present(UEVR_OnPresentCb f) { ++present_registrations; present = f; return true; }
bool add_reset(UEVR_OnDeviceResetCb f) { ++reset_registrations; reset = f; return true; }
bool add_custom(UEVR_OnCustomEventCb f) { ++custom_registrations; custom = f; return true; }
bool remove_callback(void* f) {
    if (f == reinterpret_cast<void*>(present)) present = nullptr;
    if (f == reinterpret_cast<void*>(reset)) reset = nullptr;
    if (f == reinterpret_cast<void*>(custom)) custom = nullptr;
    return true;
}
unsigned get_dir(wchar_t* out, unsigned capacity) {
    if (out && capacity > directory.size()) wcscpy_s(out, capacity, directory.c_str());
    return static_cast<unsigned>(directory.size());
}
void dispatch(const char* event, const char* data) {
    if (std::string(event) == "cheeky.foveated_dlss.snapshot.v1") received = data;
    if (std::string(event) == "cheeky.foveated_dlss.load_state.v1") loader_received = data;
}
void log(const char* format, ...) { va_list args; va_start(args, format); vprintf(format, args); va_end(args); puts(""); }
double field(const std::string& json, const std::string& name) {
    auto p = json.find('"' + name + "\":"); require(p != json.npos, "Missing JSON field");
    p += name.size() + 3; return std::stod(json.substr(p));
}
void command(const char* text) { custom("cheeky.foveated_dlss.command.v1", text); present(); }
std::string snapshot(CheekyUEVRSnapshotFn fn) {
    std::vector<char> text(cheeky_uevr_message_capacity);
    require(fn(text.data(), static_cast<unsigned>(text.size())), "Snapshot export failed"); return text.data();
}
void wait_gpu(ID3D12Device* device, ID3D12CommandQueue* queue) {
    ComPtr<ID3D12Fence> fence; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Fence");
    check(queue->Signal(fence.Get(), 1), "Signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); require(event != nullptr, "Fence event");
    check(fence->SetEventOnCompletion(1, event), "SetEventOnCompletion");
    auto result = WaitForSingleObject(event, 10000); CloseHandle(event); require(result == WAIT_OBJECT_0, "GPU timeout");
}
void settings_tests(const std::filesystem::path& root) {
    {
        Settings fg;
        require(!fg.fg.automatic_multiplier&&set_named_setting(fg,"FgAutoMultiplier","true")&&fg.fg.automatic_multiplier&&
            !set_named_setting(fg,"FgAutoMultiplier","invalid"),"Auto FG is opt-in and validates boolean values");
        require(fg.fg.multiplier==2&&set_named_setting(fg,"FgMultiplier","3")&&fg.fg.multiplier==3&&
            set_named_setting(fg,"FgMultiplier","4")&&!set_named_setting(fg,"FgMultiplier","1")&&
            !set_named_setting(fg,"FgMultiplier","5")&&!set_named_setting(fg,"FgMultiplier","-1"),"FG multiplier validates 2x/3x/4x");
        require(!fg.fg.enabled && set_named_setting(fg,"FgEnabled","true") && fg.fg.enabled,
            "FG is opt-in and configurable");
        require(fg.fg.match_sr_region && !fg.fg.show_border &&
            fg.fg.width == fg.width && fg.fg.height == fg.height,
            "FG follows SR by default, uses matching manual dimensions, and hides debug border");
        require(set_named_setting(fg,"FgMatchSrRegion","false") && !fg.fg.match_sr_region &&
            set_named_setting(fg,"FgMatchSrRegion","true") && fg.fg.match_sr_region &&
            set_named_setting(fg,"FgShowBorder","true") && fg.fg.show_border &&
            set_named_setting(fg,"FgShowBorder","false") && !fg.fg.show_border &&
            !set_named_setting(fg,"FgShowBorder","invalid"),
            "FG match-SR and blue-border options parse independently");
        require(fg.fg.release_point==3&&fg.fg.resolution_percent==100&&
            fg.fg.peripheral_percent==50&&fg.fg.peripheral_method==1,"FG uses the tested generated-submission/fresh-only and quality defaults");
        require(set_named_setting(fg,"FgWaitForCompletion","false")&&fg.fg.release_point==0&&set_named_setting(fg,"FgWaitForCompletion","true")&&
            fg.fg.release_point==2&&!set_named_setting(fg,"FgWaitForCompletion","invalid"),
            "Legacy completion checkbox still selects both policies and validates boolean values");
        require(set_named_setting(fg,"FgWidth","0.37") && set_named_setting(fg,"FgYOffset","-0.12") &&
            !set_named_setting(fg,"FgEnabled","invalid") && !set_named_setting(fg,"FgWidth","nan"),
            "FG settings validate types");
        require(set_named_setting(fg,"FgReleasePoint","1")&&fg.fg.release_point==1&&
            set_named_setting(fg,"FgReleasePoint","3")&&set_named_setting(fg,"FgReleasePoint","4")&&fg.fg.release_point==4&&
            set_named_setting(fg,"FgReleasePoint","5")&&fg.fg.release_point==5&&!set_named_setting(fg,"FgReleasePoint","6")&&!set_named_setting(fg,"FgReleasePoint","-1"),"Release points validate");
        require(set_named_setting(fg,"FgReleasePoint","3"),"Select generated submission for persistence");
        for(const auto key:{"FgHooklessMode3","FgBackbufferFrameId","FgDepthBits","FgMotionBits","FgSubmitRepeats","FgGuideCaptureMode","FgCompactStorage",
                "FgColorCaptureMode","FgUploadMode","FgDirectPeripheralShading","FgDiagnosticMode",
                "D3D11D3D12SharedGuides","D3D11D3D12SingleBuffer","HookRescanOnModuleChange"})
            require(set_named_setting(fg,key,"0")&&set_named_setting(fg,key,"invalid")&&
                setting_groups_json().find(std::string("\"")+key+"\"")==std::string::npos,"Retired FG options are accepted from older menus but not persisted");
        require(fg.fg.queue_priority==2&&set_named_setting(fg,"FgQueuePriority","0")&&
            set_named_setting(fg,"FgQueuePriority","1")&&set_named_setting(fg,"FgQueuePriority","2")&&
            !set_named_setting(fg,"FgQueuePriority","3")&&!set_named_setting(fg,"FgQueuePriority","-1")&&fg.fg.queue_priority==2,
            "Shared game queue is the default; separate GPU queues remain selectable");
        require(fg.fg.latency_diagnostics==0,"Latency timeline defaults off");
        require(fg.fg.direct_xr_output==0,"Direct XR experiment defaults off");
        for(const auto key:{"FgLatencyDiagnostics","FgDirectXrOutput"})
            require(!set_named_setting(fg,key,"2")&&!set_named_setting(fg,key,"-1")&&set_named_setting(fg,key,"1"),"Transport dropdown bounds validate");
        require(fg.fg.center_method==0&&set_named_setting(fg,"FgCenterMethod","1")&&set_named_setting(fg,"FgCenterMethod","2")&&
            !set_named_setting(fg,"FgCenterMethod","3")&&!set_named_setting(fg,"FgCenterMethod","-1"),"Center FG defaults to DLSS and validates shader methods");
        std::string error;const auto path=root/"fg.ini";
        require(set_named_setting(fg,"FgResolutionPercent","75")&&set_named_setting(fg,"FgPeripheralPercent","25")&&
            set_named_setting(fg,"FgPeripheralMethod","3"),"FG quality and periphery controls parse");
        require(write_settings_file(path,fg,error),"Save FG settings");
        Settings restored;
        require(read_settings_file(path,restored,error) && restored.fg.enabled && restored.fg.width==0.37f &&
            restored.fg.y==-0.12f&&restored.fg.resolution_percent==75&&restored.fg.peripheral_percent==25&&
            restored.fg.automatic_multiplier&&restored.fg.multiplier==4&&restored.fg.center_method==2&&restored.fg.peripheral_method==3&&
            restored.fg.release_point==3&&restored.fg.latency_diagnostics==1&&restored.fg.queue_priority==2&&restored.fg.direct_xr_output==1,
            "FG settings survive restart");
        require(reset_settings_group(restored,"sr") && restored.fg.enabled,"SR reset must preserve FG");
        require(reset_settings_group(restored,"fg") && !restored.fg.enabled&&restored.fg.release_point==3&&
            !restored.fg.automatic_multiplier&&restored.fg.multiplier==2&&restored.fg.center_method==0&&restored.fg.peripheral_percent==50&&restored.fg.peripheral_method==1&&restored.fg.queue_priority==2,
            "FG reset restores the tested preset and opt-out");
        {std::ofstream out(path);out<<"[CheekyFoveatedDLSS]\nWidth=0.4\n";}
        restored=fg;
        require(read_settings_file(path,restored,error) && !restored.fg.enabled&&restored.fg.release_point==3,
            "Old profiles inherit the tested FG preset and default FG off");
        {std::ofstream out(path);out<<"[CheekyFoveatedDLSS]\nFgWaitForCompletion=1\n";}
        require(read_settings_file(path,restored,error)&&restored.fg.release_point==2,"Migrate old completion checkbox with fresh-only default");
        {std::ofstream out(path);out<<"[CheekyFoveatedDLSS]\nFgReleasePoint=1\nFgWaitForCompletion=1\n";}
        require(read_settings_file(path,restored,error)&&restored.fg.release_point==1,"Explicit release point overrides legacy key");
        fg.fg.width=5;fg.fg.height=-1;fg.fg.feather=3;
        update_settings(fg);
        require(current_settings().fg.enabled && current_settings().fg.width==1 && current_settings().fg.height==0.2f &&
            current_settings().fg.feather==0.3f,"Runtime clamps FG geometry");
        set_processing_allowed(false);
        require(!current_settings().fg.enabled && configured_settings().fg.enabled,"Detach pauses FG and preserves preference");
        set_processing_allowed(true);update_settings(Settings{});
    }
    Settings s; require(s.d3d12_lower_hook, "Lower hook is the default");
    require(!s.manual_stereo_mapping && !s.manual_stereo_right_first && !s.independent_openxr_gaze,
        "Gaze compatibility must be opt-in");
    for (const auto key : {"ManualStereoMapping", "ManualStereoRightFirst", "IndependentOpenXRGaze"})
        require(set_named_setting(s, key, "true"), "Parse gaze compatibility option");
    require(!s.fix_motion_blur, "Motion blur compatibility is opt-in");
    require(set_named_setting(s, "FixMotionBlur", "true") && s.fix_motion_blur, "Enable motion blur fix");
    require(!set_named_setting(s, "FixMotionBlur", "invalid"), "Reject malformed motion blur setting");
    s.center_preset = s.peripheral_dlaa_preset = 10U;
    require(set_named_setting(s, "D3D12LowerHook", "false") && !s.d3d12_lower_hook, "Select higher hook");
    require(!set_named_setting(s, "D3D12LowerHook", "invalid"), "Reject invalid hook toggle");
    require(set_named_setting(s, "Width", "0.2"), "Parse setting");
    require(!set_named_setting(s, "Width", "nan"), "Reject NaN");
    require(!set_named_setting(s, "Width", "0.7trailing"), "Reject trailing garbage");
    require(!set_named_setting(s, "Enabled", "maybe"), "Reject bad bool");
    require(!set_named_setting(s, "CenterMode", "99"), "Reject enum");
    require(!set_named_setting(s, "GazeQuantizationPixels", "-1"), "Reject negative unsigned");
    s.height = s.nr_width = s.nr_height = .2F;
    s.enabled = false; s.nr_motion_scale_x_multiplier = -1.25f;
    require(set_named_setting(s, "NrStyle", "2"), "Parse Cinematic style");
    require(set_named_setting(s, "AfwManualCoverage", "true") && set_named_setting(s, "AfwWarpMargin", "0.125"),
        "Parse AFW manual coverage settings");
    require(set_named_setting(s, "AfwAutomaticCoverage", "true"), "Parse AFW automatic coverage setting");
    require(!Settings{}.eye_calibration_continuous, "Default calibration must stop after acquisition");
    require(set_named_setting(s, "EyeCalibrationMethod", "3") &&
        !set_named_setting(s, "EyeCalibrationMethod", "4"), "Validate calibration override enum");
    s.eye_calibration_learned_method = 2; s.eye_calibration_learned_sessions = 2;
    s.eye_calibration_learned_signature = 0xfedcba9876543210ULL;
    std::string error; const auto path = root / "roundtrip.ini";
    require(write_settings_file(path, s, error), "Write settings");
    Settings r; require(read_settings_file(path, r, error), "Read settings");
    require(r.fix_motion_blur && r.center_preset == 10U && r.peripheral_dlaa_preset == 10U,
        "Preset J and motion blur fix survive settings roundtrip");
    require(r.manual_stereo_mapping && r.manual_stereo_right_first && r.independent_openxr_gaze,
        "Gaze opt-ins survive per-application settings roundtrip");
    auto reset_gaze = r;
    require(reset_settings_group(reset_gaze, "gaze") && !reset_gaze.manual_stereo_mapping &&
        !reset_gaze.manual_stereo_right_first && !reset_gaze.independent_openxr_gaze,
        "Gaze reset restores opt-outs");
    update_settings(r);
    require(current_settings().fix_motion_blur && current_settings().center_preset == 10U &&
        current_settings().peripheral_dlaa_preset == 10U, "Runtime accepts J and motion blur fix");
    auto reset_sr = r;
    require(reset_settings_group(reset_sr, "sr") && !reset_sr.fix_motion_blur,
        "SR reset disables compatibility copies");
    require(serialize_settings(r) == serialize_settings(s), "Roundtrip all persisted fields");
    require(r.eye_calibration_method == EyeCalibrationMethod::full && r.eye_calibration_learned_method == 2 &&
        r.eye_calibration_learned_signature == 0xfedcba9876543210ULL && r.eye_calibration_learned_sessions == 2,
        "Persist learned calibration without losing 64-bit signature precision");
    for (const auto order : {"0", "1"}) {
        require(set_named_setting(s, "NrProcessingOrder", order), "Parse NR rendering order");
        s.nr_working_scale = 0.37f;
        require(write_settings_file(path, s, error) && read_settings_file(path, r, error), "Persist NR order");
        require(r.nr_processing_order == s.nr_processing_order && r.nr_working_scale == 0.37f,
            "Rendering order roundtrip changed working scale");
        require(settings_json(r).find(std::string("\"NrProcessingOrder\":") + order) != std::string::npos,
            "Runtime/support settings omit rendering order");
        require(serialize_settings(r).find("SchemaVersion=1\n") != std::string::npos, "Additive key changed schema");
    }
    for (const auto value : {"-1", "2", "3", "1.5", "nan", "invalid"}) {
        const auto before = serialize_settings(r);
        require(!set_named_setting(r, "NrProcessingOrder", value) && serialize_settings(r) == before,
            "Invalid rendering order changed settings");
        { std::ofstream out(path); out << "[CheekyFoveatedDLSS]\nWidth=0.2\nNrProcessingOrder=" << value << '\n'; }
        require(!read_settings_file(path, r, error) && serialize_settings(r) == before,
            "Invalid NR order file was not rejected atomically");
    }
    { std::ofstream out(path); out << "[CheekyFoveatedDLSS]\nSchemaVersion=1\nAfwDepthCoverage=1\nNrWorkingScale=0.37\n"; }
    require(read_settings_file(path, r, error) && r.nr_processing_order == NrProcessingOrder::after_upscaling &&
        r.nr_working_scale == 0.37f, "Missing NR order must default to After");
    require(r.nr_style == 0U, "Legacy settings must restore Standard style");
    require(r.d3d12_lower_hook, "Legacy settings default to lower hook even over a higher-hook draft");
    require(!r.fix_motion_blur, "Legacy settings cannot inherit compatibility copies");
    require(!r.manual_stereo_mapping && !r.manual_stereo_right_first && !r.independent_openxr_gaze,
        "Files without gaze opt-ins cannot inherit previously enabled compatibility modes");
    require(serialize_settings(r).find("AfwDepthCoverage") == std::string::npos,
        "Retired depth setting is ignored when loading older files and omitted on save");
    require(!r.afw_manual_coverage && !r.afw_automatic_coverage && r.afw_warp_margin == .05F,
        "Legacy settings restore centered AFW mode even over existing manual settings");
    for (const auto key : {"AfwManualCoverage", "AfwAutomaticCoverage", "AfwWarpMargin"})
        require(setting_group(key) == "gaze", "Shared AFW coverage belongs to the Stereo/gaze reset group");
    auto reset_afw = s;
    require(reset_settings_group(reset_afw, "sr") && reset_afw.afw_manual_coverage,
        "SR reset must preserve shared AFW coverage used by NR");
    require(reset_settings_group(reset_afw, "gaze") && !reset_afw.afw_manual_coverage &&
        !reset_afw.afw_automatic_coverage && reset_afw.afw_warp_margin == .05F,
        "Stereo/gaze reset restores all shared AFW controls");
    require(setting_groups_json().find("\"NrProcessingOrder\":\"nr\"") != std::string::npos,
        "Rendering order is missing from NR group metadata");
    r = s;
    { std::ofstream out(path); out << "[CheekyFoveatedDLSS]\nWidth=0.4\nHeight=nan\n"; }
    require(!read_settings_file(path, r, error) && r.width == s.width, "Atomic malformed file rejection");
    { std::ofstream out(path); out << "[CheekyFoveatedDLSS]\nSchemaVersion=99\n"; }
    require(!read_settings_file(path, r, error), "Unknown schema rejection");
    update_settings(s); set_processing_allowed(false);
    require(!current_settings().enabled && configured_settings().width == s.width, "Detach preserves configured settings");
    set_processing_allowed(true);
    Settings a, b; a.width = a.height = 0.3f; b.width = b.height = 0.8f;
    update_settings(a); std::atomic<bool> running{true}, coherent{true};
    std::thread writer([&] { for (int i=0;i<20000;++i) update_settings(i%2 ? a : b); running=false; });
    while (running) { const auto value = current_settings(); if (value.width != value.height) coherent=false; }
    writer.join(); require(coherent, "Coherent concurrent settings snapshots");
}
}
int main(int argc, char** argv) {
    // A live release-mode switch retains the source revision. A DLSS-SR
    // quality change (new source revision) cannot interpolate from old guides.
    constexpr auto a=fg_source_configuration(0xabcdu,2u);
    constexpr auto b=fg_source_configuration(0xabcdu,3u);
    static_assert(a!=b && fg_same_source_history(a,a) &&
        !fg_same_source_history(a,b) && !fg_same_source_history(0,a));
    try {
        wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const auto bin = std::filesystem::path(exe).parent_path();
        // Windows reuses PIDs. Keep fixtures from earlier runs from masquerading
        // as the files and reports produced by this invocation.
        const auto run = std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(std::chrono::system_clock::now().time_since_epoch().count());
        const auto root = bin / "uevr-test-data" / run;
        std::filesystem::create_directories(root); directory = root.wstring();
        settings_tests(root);
        const auto issue = support_issue_url(L"report #&.zip", "state & details\n");
        require(issue.starts_with(L"https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/issues/new?") &&
            issue.find(L"report=report%20%23%26.zip") != issue.npos && issue.find(L"diagnostics=state%20%26%20details%0A") != issue.npos,
            "Support issue link encodes user-visible summary and ZIP filename");
        {
            const auto primary = root / "runtime", executable = root / "game";
            std::filesystem::create_directories(primary); std::filesystem::create_directories(executable);
            const auto fixture = bin / "test-fixtures" / "nvngx_dlss.dll";
            std::filesystem::copy_file(fixture, executable / "cheeky-loader-test.dll");
            auto loaded = load_runtime_library(L"cheeky-loader-test.dll", primary.c_str(), executable.c_str());
            require(loaded.module && loaded.error == 0 && std::filesystem::path(loaded.path.data()).parent_path() == executable,
                "Optional runtime loads from executable directory when absent beside runtime");
            FreeLibrary(loaded.module);
            std::filesystem::copy_file(fixture, primary / "cheeky-loader-test.dll");
            loaded = load_runtime_library(L"cheeky-loader-test.dll", primary.c_str(), executable.c_str());
            require(loaded.module && std::filesystem::path(loaded.path.data()).parent_path() == primary, "Runtime directory takes precedence");
            FreeLibrary(loaded.module);
            loaded = load_runtime_library(L"cheeky-missing-test.dll", primary.c_str(), executable.c_str());
            require(!loaded.module && loaded.error != 0, "Missing optional runtime reports loader failure");
            FrameCadence cadence;
            for (unsigned i = 0; i <= 100; ++i) cadence.sample(10 + i * 0.01, true);
            require(std::abs(cadence.average_ms - 10) < 0.001, "Present cadence averages real intervals");
            cadence.sample(12, true);
            require(cadence.average_ms == 0, "Pause clears stale present cadence");
            for (unsigned i = 1; i <= 100; ++i) cadence.sample(12 + i * 0.02, true);
            require(std::abs(cadence.average_ms - 20) < 0.001, "Cadence recovers after pause");
            cadence.sample(14.02, false);
            require(cadence.average_ms == 0, "SR toggle starts a fresh cadence window");
        }
        const bool conflict_mode = argc > 1 && std::string(argv[1]) == "--conflict";
        bool hardware{}, inactive_afw{}, higher_hook{};
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--higher-hook") higher_hook = true;
            if (std::string(argv[i]) == "--hardware") hardware = true;
            if (std::string(argv[i]) == "--inactive-afw") inactive_afw = true;
        }
        const std::string mode = argc > 1 ? argv[1] : "";
        const bool late = mode.starts_with("--late-");
        const bool afw = mode.starts_with("--afw-");
        const bool realvr = mode.starts_with("--realvr-") || mode.starts_with("--lower-");
        const bool openvr_late = mode.starts_with("--openvr-late-");
        const bool dx11 = mode == "--dx11" || (late && mode.find("dx11")!=mode.npos);
        HANDLE conflict = conflict_mode ? claim_processing_owner() : nullptr;
        require(!conflict_mode || conflict, "Create conflicting owner");
        ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        ComPtr<IDXGIAdapter> warp; check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
        ComPtr<ID3D12Device> device; check(D3D12CreateDevice(hardware ? nullptr : warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12 device");
        if (hardware) puts("Testing native D3D12 hooks on the default hardware adapter (no game).");
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue; check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "Queue");
        {
            GpuMemory::Segment segment;DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            info.CurrentUsage=1024;info.Budget=2048;segment.update(S_OK,info);
            require(segment.valid&&segment.headroom==1024&&segment.over_budget==0&&segment.peak==1024,"Memory budget headroom");
            info.CurrentUsage=4096;segment.update(S_OK,info);
            require(segment.headroom==0&&segment.over_budget==2048&&segment.over_budget_samples==1&&segment.peak==4096,
                "Over-budget memory must not underflow headroom");
            segment.update(E_FAIL,{});
            require(!segment.valid&&segment.usage==0&&segment.peak==4096,"Failed memory queries are unavailable, not valid zero usage");
            GpuMemory memory;memory.attach(1,device.Get(),queue.Get());
            const auto expected=device->GetAdapterLuid();
            require(memory.adapter&&memory.luid.LowPart==expected.LowPart&&memory.luid.HighPart==expected.HighPart,"Memory adapter must match the actual render device");
            require(memory.poll(1000)&&memory.samples==1&&!memory.poll(1500)&&memory.samples==1,"GPU memory queries are throttled to one per second");
            require(memory.mark_baseline(1600,false)==memory.local.valid&&memory.baseline_at==1600&&!memory.baseline_fg_enabled,
                "Baseline performs a fresh query and preserves FG state");
            if(hardware)require(memory.local.valid&&memory.nonlocal.valid,"Hardware DXGI local and nonlocal memory queries succeed");
            memory.attach(2,nullptr,nullptr);
            require(!memory.adapter&&!memory.baseline_valid&&memory.samples==0,"Device changes discard stale memory samples and baselines");
        }
        UEVR_PluginVersion version{2,39,0};
        UEVR_PluginFunctions functions{};
        functions.get_persistent_dir = get_dir; functions.dispatch_lua_event = dispatch; functions.remove_callback = remove_callback;
        functions.log_info = log; functions.log_error = log; functions.log_warn = log;
        UEVR_PluginCallbacks callbacks{};
        callbacks.on_present = add_present; callbacks.on_device_reset = add_reset; callbacks.on_custom_event = add_custom;
        UEVR_RendererData renderer{UEVR_RENDERER_D3D12, device.Get(), nullptr, queue.Get()};
        ComPtr<ID3D11Device> device11;
        if (dx11) {
            check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device11,nullptr,nullptr),"DX11 device");
            renderer = {UEVR_RENDERER_D3D11, device11.Get(), nullptr, nullptr};
        }
        UEVR_PluginInitializeParam api{}; api.version = &version; api.functions = &functions; api.callbacks = &callbacks; api.renderer = &renderer;
        UEVR_VRData vr_api{}; vr_api.is_openvr = is_openvr;
        vr_api.is_hmd_active = is_hmd_active; vr_api.get_ue_projection_matrix = get_projection;
        vr_api.get_mod_value = get_mod_value;
        vr_api.get_hmd_width = vr_api.get_hmd_height = hmd_extent;
        if (afw || inactive_afw || mode == "--fg-controls") api.vr = &vr_api;
        if (inactive_afw) rendering_mode = "0";
        UEVR_OpenVRData openvr_api{}; openvr_api.get_vr_compositor = get_compositor;
        void* original_wait{};
        if (openvr_late) {
            auto fixture = LoadLibraryW((bin / "test-fixtures" / "openvr_api.dll").c_str());
            require(fixture != nullptr, "Load mock OpenVR before Cheeky");
            auto configure = reinterpret_cast<void (*)(unsigned)>(GetProcAddress(fixture, "CheekyFakeOpenVR_SetVersion"));
            auto get_interface = reinterpret_cast<void* (*)(const char*, int*)>(GetProcAddress(fixture, "VR_GetGenericInterface"));
            require(configure && get_interface, "OpenVR fixture exports");
            const auto abi = static_cast<unsigned>(std::stoul(mode.substr(mode.size() - 3)));
            configure(abi);
            int error{};
            cached_compositor = get_interface(("IVRCompositor_" + mode.substr(mode.size() - 3)).c_str(), &error);
            require(cached_compositor && !error, "Host caches compositor before Cheeky loads");
            original_wait = (*static_cast<void***>(cached_compositor))[2];
            api.vr = &vr_api; api.openvr = &openvr_api;
        }
        auto plugin_path = bin / "CheekyFoveatedDLSS.dll";
        if ((late && !dx11) || afw || realvr) {
            // Isolate the optional fake NR runtime from ordinary host fixtures
            // and from other concurrently running test processes.
            const auto isolated = root / "nr-hooks";
            const auto runtime_dir = isolated / "CheekyFoveatedDLSS";
            std::filesystem::create_directories(runtime_dir);
            std::filesystem::copy_file(plugin_path, isolated / plugin_path.filename());
            std::filesystem::copy_file(bin / "CheekyFoveatedDLSS" / "CheekyFoveatedDLSSRuntime.dll",
                runtime_dir / "CheekyFoveatedDLSSRuntime.dll");
            std::filesystem::copy_file(bin / "test-fixtures" / "nvngx_dlss.dll", runtime_dir / "nvngx_dlssnr.dll");
            plugin_path = isolated / plugin_path.filename();
        }
        std::filesystem::path late_ngx_path;
        for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--ota-runtime") {
            require(late, "OTA runtime option requires a late-attachment test");
            late_ngx_path = root / "NVIDIA/NGX/models/dlss/versions/20318464/files/160_E658700.bin";
            std::filesystem::create_directories(late_ngx_path.parent_path());
            std::filesystem::copy_file(bin / "test-fixtures/nvngx_dlss.dll", late_ngx_path);
        }
        if (late) prepare_late_attach_test(bin,device11.Get(),device.Get(),queue.Get(),mode.ends_with("-c"),mode.starts_with("--late-streamline"),late_ngx_path);
        if (afw || realvr) prepare_afw_test(bin,root,device.Get(),queue.Get(),mode);
        if (inactive_afw) {
            require(late, "Inactive AFW fixture requires a late-attachment route");
            const auto afw_path = root / "PDAFWPlugin.dll";
            std::filesystem::copy_file(bin / "test-fixtures/nvngx_dlss.dll", afw_path);
            require(LoadLibraryW(afw_path.c_str()) != nullptr, "Load inactive AFW before Cheeky");
        }
        if (higher_hook) {
            Settings initial; initial.d3d12_lower_hook = false; std::string error;
            require(write_settings_file(root / "CheekyFoveatedDLSS.ini", initial, error), "Save higher-hook startup setting");
        }
        if (mode == "--loader-missing") {
            const auto isolated = root / "missing-runtime";
            std::filesystem::create_directories(isolated);
            std::filesystem::copy_file(plugin_path, isolated / plugin_path.filename());
            plugin_path = isolated / plugin_path.filename();
        }
        HMODULE plugin = LoadLibraryW(plugin_path.c_str()); require(plugin != nullptr, "Load actual UEVR plugin DLL");
        auto init = reinterpret_cast<UEVR_PluginInitializeFn>(GetProcAddress(plugin, "uevr_plugin_initialize"));
        require(init != nullptr, "Plugin entry export");
        require(!init(nullptr), "Reject invalid host API");
        version.minor=0; require(!init(&api), "Reject old UEVR API"); version.minor=39;
        require(init(&api), "Initialize real plugin via fake UEVR callbacks");
        require(init(&api), "Repeated host init is idempotent");
        require(present && custom && reset, "All callbacks registered");
        require(present_registrations == 1 && reset_registrations == 1 && custom_registrations == 1,
            "Repeated initialization must not register duplicate callbacks");
        present(); reset();
        require(GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll") == nullptr && loader_received == "disabled",
            "Plugin stays dormant without loading runtime");
        custom("load", "true"); custom("cheeky.foveated_dlss.load.v1", "invalid"); present();
        require(GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll") == nullptr,
            "Foreign and malformed events must not load Cheeky");
        custom("cheeky.foveated_dlss.load.v1", "true");
        require(GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll") == nullptr,
            "Custom callback must defer loading to present");
        present();
        if (mode == "--loader-missing") {
            require(loader_received == "failed" && GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll") == nullptr,
                "Missing runtime reports failure without leaving pointers active");
            reset(); present();
            const auto destination = plugin_path.parent_path() / "CheekyFoveatedDLSS" / "CheekyFoveatedDLSSRuntime.dll";
            std::filesystem::create_directories(destination.parent_path());
            std::filesystem::copy_file(bin / "test-fixtures" / "nvngx_dlss.dll", destination);
            custom("cheeky.foveated_dlss.load.v1", "false"); present();
            custom("cheeky.foveated_dlss.load.v1", "true"); present(); reset();
            require(loader_received == "failed" && GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll") == nullptr,
                "Incomplete runtime exports leave no dangling callbacks");
            std::filesystem::copy_file(bin / "CheekyFoveatedDLSS" / "CheekyFoveatedDLSSRuntime.dll",
                destination, std::filesystem::copy_options::overwrite_existing);
            custom("cheeky.foveated_dlss.load.v1", "false"); present();
            custom("cheeky.foveated_dlss.load.v1", "true"); present();
            require(loader_received == "enabled", "Explicit retry can recover after package repair");
        }
        const auto runtime = GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll"); require(runtime != nullptr, "Runtime dependency loaded");
        auto get = reinterpret_cast<CheekyUEVRSnapshotFn>(GetProcAddress(runtime, "CheekyUEVR_Snapshot"));
        auto start = reinterpret_cast<CheekyUEVRStartFn>(GetProcAddress(runtime, "CheekyUEVR_Start"));
        if (mode == "--fg-controls") {
            const auto control_fn=reinterpret_cast<CheekyFgControlFn>(GetProcAddress(runtime,"CheekyFG_GetControl"));
            require(control_fn!=nullptr,"FG controls export missing");
            CheekyFgControl control;
            require(control_fn(&control) && !control.enabled,"FG starts disabled");
            control.abi=999;
            require(!control_fn(&control),"FG control rejects ABI mismatch");
            control=CheekyFgControl{};
            // FG only activates in UEVR Native Stereo (VR_RenderingMethod=0).
            rendering_mode="0"; present();
            command("1\n901\nset\nFgEnabled=true\nFgWidth=0.37\nFgFeather=0.08\nFgReleasePoint=3\nFgLatencyDiagnostics=0\nFgQueuePriority=2\nFgDirectXrOutput=1");
            const auto direct_fn=reinterpret_cast<CheekyFgDirectOutputFn>(GetProcAddress(runtime,"CheekyFG_GetDirectOutputOptionsV1"));
            CheekyFgDirectOutput direct;require(direct_fn&&direct_fn(&direct)&&direct.requested&&
                snapshot(get).find("\"direct_output\":{")!=std::string::npos,"Direct XR Lua option reaches runtime export and diagnostics");
            direct.abi=999;require(!direct_fn(&direct)&&!direct_fn(nullptr),"Direct output options validate ABI and pointer");
            command("1\n906\nset\nFgDirectPeripheralShading=2");
            require(!GetProcAddress(runtime,"CheekyFG_GetDirectShadingOptionsV1")&&
                snapshot(get).find("\"FgDirectPeripheralShading\"")==std::string::npos,"Retired shading option has no runtime export");
            const auto transport_fn=reinterpret_cast<CheekyFgTransportOptionsFn>(GetProcAddress(runtime,"CheekyFG_GetTransportOptionsV1"));
            CheekyFgTransportOptions options;require(transport_fn&&transport_fn(&options)&&options.latency_diagnostics==0,
                "Lua transport options reach real runtime bridge");
            options.size=0;require(!transport_fn(&options),"Transport options reject incorrect ABI size");
            require(control_fn(&control) && control.enabled && std::abs(field(snapshot(get),"FgWidth")-0.37)<0.00001,
                "Lua protocol controls real FG runtime");
            const auto mode_fn=reinterpret_cast<CheekyFgUevrModeFn>(GetProcAddress(runtime,"CheekyFG_GetUevrModeV1"));
            CheekyFgUevrModeV1 mode_status;
            require(mode_fn && mode_fn(&mode_status) && mode_status.attached && mode_status.known && mode_status.mode==0,
                "FG mode reports Native Stereo before mode transitions");
            const auto mode_v2_fn=reinterpret_cast<CheekyFgUevrModeV2Fn>(
                GetProcAddress(runtime,"CheekyFG_GetUevrModeV2"));
            CheekyFgUevrModeV2 mode_v2;
            require(mode_v2_fn && mode_v2_fn(&mode_v2) && mode_v2.mode==0 &&
                mode_v2.transition_epoch!=0,"FG mode V2 exports the transition epoch");
            const auto initial_mode_epoch=mode_v2.transition_epoch;
            // Native -> AFW -> Native can occur without any OpenXR xrWaitFrame.
            rendering_mode="3";present();
            rendering_mode="0";present();
            require(mode_v2_fn(&mode_v2) && mode_v2.mode==0 &&
                mode_v2.transition_epoch>=initial_mode_epoch+2,
                "Native -> AFW -> Native leaves a detectable transition epoch");
            for(const auto method : {1U,2U,3U}){
                rendering_mode=std::to_string(method);present();
                require(control_fn(&control) && !control.enabled && mode_fn(&mode_status) &&
                    mode_status.known && mode_status.mode==method &&
                    snapshot(get).find("\"FgEnabled\":true")!=std::string::npos,
                    "Non-native rendering modes pause producer without changing saved FG preference");
                require(mode_v2_fn(&mode_v2) && mode_v2.mode==method,
                    "FG mode V2 observes paused method");
                const auto previous_epoch=mode_v2.transition_epoch;
                rendering_mode="0";present();
                require(control_fn(&control) && control.enabled && mode_fn(&mode_status) && mode_status.mode==0,
                    "Returning to Native Stereo automatically reactivates FG");
                require(mode_v2_fn(&mode_v2) && mode_v2.transition_epoch>previous_epoch,
                    "Returning to Native Stereo advances the presenter epoch");
            }
            rendering_mode="garbage";present();
            require(control_fn(&control)&&!control.enabled&&mode_fn(&mode_status)&&!mode_status.known,
                "Unknown UEVR rendering mode fails closed");
            rendering_mode="0";present();
            require(control_fn(&control)&&control.enabled,"Native Stereo recovers from unknown mode");
            // Release policy is scheduling-only: do not restart FG or invalidate NGX history.
            const auto before_release_configuration=control.configuration;
            command("1\n907\nset\nFgReleasePoint=4");
            require(control_fn(&control)&&control.enabled&&control.configuration==before_release_configuration&&
                snapshot(get).find("\"FgReleasePoint\":4")!=std::string::npos,
                "4ms soft release is live without resetting FG or its capture configuration");
            command("1\n908\nset\nFgReleasePoint=3");
            require(control_fn(&control)&&control.enabled&&control.configuration==before_release_configuration,
                "Switching release modes preserves an enabled FG pipeline and its temporal configuration");
            const auto rate_fn=reinterpret_cast<CheekyFgRateOptionsFn>(GetProcAddress(runtime,"CheekyFG_GetRateOptionsV1"));
            CheekyFgRateOptions rate;
            require(rate_fn&&rate_fn(&rate)&&rate.multiplier==2,"Older configurations retain 2x default");
            command("1\n909\nset\nFgMultiplier=4");
            require(rate_fn(&rate)&&rate.multiplier==4&&control_fn(&control)&&control.configuration!=before_release_configuration,
                "Multiplier reaches layer export and retires the old presenter configuration");
            rate.abi=999;require(!rate_fn(&rate),"FG rate export validates ABI");
            command("1\n910\nset\nFgMultiplier=2");
            require(control_fn(&control),"Read current fixed multiplier configuration");const auto fixed_configuration=control.configuration;
            const auto automatic_fn=reinterpret_cast<CheekyFgRateOptionsV2Fn>(GetProcAddress(runtime,"CheekyFG_GetRateOptionsV2"));
            CheekyFgRateOptionsV2 automatic;
            command("1\n911\nset\nFgAutoMultiplier=true");
            require(automatic_fn&&automatic_fn(&automatic)&&automatic.automatic&&automatic.multiplier==2&&
                control_fn(&control)&&control.configuration==fixed_configuration,"Auto toggle retains the source configuration and temporal history");
            automatic.abi=1;require(!automatic_fn(&automatic),"Rate V2 rejects old ABI");
            command("1\n912\nset\nFgAutoMultiplier=false");
            command("1\n903\nset\nEnabled=false");
            command("1\n905\nset\nFgReleasePoint=0");
            require(snapshot(get).find("\"FgReleasePoint\":0")!=std::string::npos,
                "Lua protocol restores overlapping native rendering");
            CheekyFgCaptureHealth health;
            auto capture_health=reinterpret_cast<CheekyFgCaptureHealthFn>(GetProcAddress(runtime,"CheekyFG_GetCaptureHealth"));
            require(capture_health&&capture_health(&health)&&
                !GetProcAddress(runtime,"CheekyFG_AttachUEVRFrameSource")&&!GetProcAddress(runtime,"CheekyFG_NeedsUEVRFrameSourceV1"),
                "UEVR matches FG guides at xrEndFrame without frame-source hooks");
            custom("cheeky.foveated_dlss.load.v1","false");present();
            require(control_fn(&control) && !control.enabled && snapshot(get).find("\"FgEnabled\":true")!=std::string::npos,
                "Host detach pauses FG without discarding saved preference");
            custom("cheeky.foveated_dlss.load.v1","true");present();
            require(control_fn(&control) && control.enabled,"Host re-enable restores FG preference");
            command("1\n902\ndefaults_fg");
            direct=CheekyFgDirectOutput{};require(direct_fn(&direct)&&!direct.requested&&
                snapshot(get).find("\"FgDirectXrOutput\":0")!=std::string::npos,"FG reset disables the direct XR experiment");
            options=CheekyFgTransportOptions{};require(transport_fn(&options)&&options.latency_diagnostics==0&&
                snapshot(get).find("\"FgReleasePoint\":3")!=std::string::npos,"Tested defaults reset live settings");
            require(control_fn(&control) && !control.enabled,"FG reset restores live opt-out");
            std::puts("PASS FG protocol, control ABI, host detach/re-enable and FG group reset");
            return 0;
        }
        if (mode == "--loader-lifecycle" || mode == "--loader-missing") {
            const auto before = field(snapshot(get), "Width");
            custom("cheeky.foveated_dlss.command.v1", "1\n999\nset\nWidth=0.777");
            custom("cheeky.foveated_dlss.load.v1", "false");
            require(snapshot(get).find("\"attached\":true") != std::string::npos,
                "Event callback must defer detachment");
            present();
            require(loader_received == "disabled" && snapshot(get).find("\"processing\":false") != std::string::npos,
                "Disable detaches processing");
            require(GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll") == runtime,
                "Hook runtime stays resident after disable");
            custom("cheeky.foveated_dlss.load.v1", "true"); present();
            require(loader_received == "enabled" && field(snapshot(get), "Width") == before,
                "Re-enable must discard commands from the old attachment");
            auto stale_detach = reinterpret_cast<CheekyUEVRDetachFn>(GetProcAddress(runtime, "CheekyUEVR_Detach"));
            custom("cheeky.foveated_dlss.load.v1", "true"); present();
            stale_detach(1);
            require(snapshot(get).find("\"attached\":true") != std::string::npos,
                "Repeated enable is idempotent and stale detach cannot disable new attachment");
            // Requests may arrive from a Lua/game thread while render/reset callbacks run.
            std::thread events([] { for (int i = 0; i < 200; ++i)
                custom("cheeky.foveated_dlss.load.v1", i % 2 ? "true" : "false"); });
            for (int i = 0; i < 20; ++i) { present(); reset(); }
            events.join();
            custom("cheeky.foveated_dlss.load.v1", "false"); present();
            require(loader_received == "disabled", "Final concurrent request is applied");
            puts("PASS: deferred loader lifecycle, failure recovery and event isolation");
            return 0;
        }
        if (openvr_late) {
            auto attach = reinterpret_cast<CheekyUEVRAttachOpenVRFn>(GetProcAddress(runtime, "CheekyUEVR_AttachOpenVR"));
            require(attach && !attach(0, cached_compositor), "Reject unowned compositor attachment");
            // Host reports no active OpenVR session first. Do not touch its
            // cached interface until it becomes active, then attach without a
            // second host VR_GetGenericInterface request.
            present();
            require((*static_cast<void***>(cached_compositor))[2] == original_wait, "Inactive OpenVR stays untouched");
            openvr_active = true;
            for (unsigned i = 0; i < 20 && (*static_cast<void***>(cached_compositor))[2] == original_wait; ++i) {
                present(); Sleep(25);
            }
            auto hooked_wait = (*static_cast<void***>(cached_compositor))[2];
            require(hooked_wait != original_wait, "Late attachment hooks cached compositor without a new host request");
            using Wait = int (*)(void*, void*, unsigned, void*, unsigned);
            require(reinterpret_cast<Wait>(hooked_wait)(cached_compositor, nullptr, 0, nullptr, 0) == 0, "Original WaitGetPoses return preserved");
            auto observed = snapshot(get);
            const auto calibration_frames=[&](const std::string& json){
                const auto start=json.find("\"eye_calibration\":{");
                require(start!=json.npos,"OpenVR calibration diagnostics present");
                return field(json.substr(start,json.find('}',start)-start),"frames");
            };
            require(observed.find("\"backend\":\"OpenVR\"") != observed.npos && calibration_frames(observed) == 1,
                "Cached WaitGetPoses activates OpenVR calibration exactly once");
            for (unsigned i = 0; i < 5; ++i) present();
            require((*static_cast<void***>(cached_compositor))[2] == hooked_wait, "Repeated host ticks do not stack hooks");
            reinterpret_cast<Wait>(hooked_wait)(cached_compositor, nullptr, 0, nullptr, 0);
            require(calibration_frames(snapshot(get)) == 2, "No duplicate observation after repeated ticks");
            reset(); present();
            require((*static_cast<void***>(cached_compositor))[2] == hooked_wait, "Device reset can reattach without stacking hooks");
            reinterpret_cast<Wait>(hooked_wait)(cached_compositor, nullptr, 0, nullptr, 0);
            require(calibration_frames(snapshot(get)) == 3, "Observation continues after device reset");
            puts("PASS: cached OpenVR compositor late attachment");
            return 0;
        }
        CheekyUEVRStart bad; bad.abi=999; require(!start(&bad), "Runtime ABI rejection");
        command("1\n1\nget");
        require(!received.empty(), "Native-to-Lua event bridge");
        if (conflict) {
            require(received.find("Another Cheeky integration") != received.npos, "Duplicate-owner rejection visible");
            require(received.find("\"ready\":false") != received.npos, "Conflicting runtime must stay disabled");
            present=nullptr; custom=nullptr; reset=nullptr; FreeLibrary(plugin); CloseHandle(conflict);
            puts("UEVR ownership conflict test passed"); return 0;
        }
        require(received.find("\"ready\":true") != received.npos, "Renderer initialized");
        require(received.find("\"gpu_memory\":{\"supported\":true,\"scope\":\"process\"")!=received.npos,
            "Actual runtime snapshot reports process GPU memory on the render adapter");
        command("1\n79\nmark_vram_baseline");
        const auto memory_start=received.find("\"gpu_memory\":{");
        const auto memory_json=received.substr(memory_start,received.find(",\"fg\":{",memory_start)-memory_start);
        const bool memory_valid=memory_json.find("\"local\":{\"valid\":true")!=memory_json.npos;
        require(memory_json.find(memory_valid?"\"baseline\":{\"valid\":true":"\"baseline\":{\"valid\":false")!=memory_json.npos,
            "Lua baseline command reflects actual DXGI query availability");
        const auto calibration_start = received.find("\"eye_calibration\":{");
        require(calibration_start != received.npos, "Shared eye calibration diagnostics present");
        const auto calibration = received.substr(calibration_start, received.find('}', calibration_start) - calibration_start);
        require(calibration.find("\"backend\":\"Waiting for VR\"") != calibration.npos &&
            field(calibration, "graphics_api") == 0 && calibration.find("\"enabled\":true") != calibration.npos,
            "Shared eye calibration diagnostics enabled on attachment");
        command("1\n80\ncalibration_disable");
        require(received.find("\"status\":\"Disabled\"") != received.npos, "Calibration disable command");
        command("1\n81\ncalibration_enable");
        command("1\n83\nset\nManualStereoMapping=true");
        require(received.find("\"status\":\"Disabled\"") != received.npos, "Manual mapping disables automatic calibration");
        command("1\n84\ncalibration_enable");
        require(received.find("\"status\":\"Disabled\"") != received.npos, "Manual mapping cannot enable marker calibration");
        command("1\n85\nset\nManualStereoMapping=false\nEyeCalibrationMethod=0");
        require(received.find("Waiting for OpenVR, OpenXR or LibOVR") != received.npos, "Automatic mapping resumes calibration");
        command("1\n82\ncalibration_reset");
        command("1\n86\ncalibration_recalibrate");
        require(received.find("Waiting for OpenVR, OpenXR or LibOVR") != received.npos, "Unavailable backend must not claim active calibration");
        if (late) {
            const auto active_hook = higher_hook ? "\"d3d12_lower_hook_active\":false" : "\"d3d12_lower_hook_active\":true";
            require(snapshot(get).find(active_hook) != std::string::npos, "Saved hook path was not applied at startup");
            command(higher_hook ? "1\n70\nset\nD3D12LowerHook=true" : "1\n70\nset\nD3D12LowerHook=false");
            require(snapshot(get).find(active_hook) != std::string::npos &&
                snapshot(get).find("\"d3d12_hook_restart_required\":true") != std::string::npos,
                "Hook toggle must save a restart request without changing live ownership");
            command(higher_hook ? "1\n71\nset\nD3D12LowerHook=false" : "1\n71\nset\nD3D12LowerHook=true");
            require(snapshot(get).find("\"d3d12_hook_restart_required\":false") != std::string::npos,
                "Restoring the active selection clears the restart request");
            if (inactive_afw) {
                present();
                require(snapshot(get).find("\"afw_experiment\":{\"enabled\":true") != std::string::npos &&
                    snapshot(get).find("\"warp_calls\":0") != std::string::npos,
                    "Inactive fixture detects AFW without executing frame warp");
                if (!dx11) require(snapshot(get).find("\"coverage_enabled\":false") != std::string::npos &&
                    snapshot(get).find("\"rendering_mode_known\":true,\"rendering_mode\":0") != std::string::npos,
                    "Native Stereo explicitly disables AFW coverage before standalone DX12 evaluation");
            }
            command("1\n2\nset\nEnabled=true\nPeripheralDlaa=false\nAutoStereoAlignment=false\nCenterMode=0\nNrEnabled=false");
            // Keep the host mode fresh just as real present callbacks do.
            verify_late_attach_test(get, command, inactive_afw ? present : nullptr,
                inactive_afw && !dx11 ? +[](unsigned mode) { rendering_mode = std::to_string(mode); } : nullptr);
            if (!late_ngx_path.empty()) require(snapshot(get).find("Active (NVIDIA cached runtime)") != std::string::npos,
                "Native and Streamline cached evaluations publish the override source");
            return 0;
        }
        if (realvr) {
            verify_realvr_test(get,command);
            return 0;
        }
        if (afw) {
            const auto publish_mode = reinterpret_cast<CheekyUEVRPublishRenderingModeFn>(GetProcAddress(runtime, "CheekyUEVR_PublishRenderingMode"));
            require(publish_mode && !publish_mode(0, 0) && !publish_mode(999, 0), "Unowned AFW mode publication rejected");
            rendering_mode = "0"; present();
            require(snapshot(get).find("\"coverage_enabled\":false") != std::string::npos &&
                snapshot(get).find("\"rendering_mode\":0") != std::string::npos, "Native Stereo restores ordinary coverage without removing hooks");
            rendering_mode = "garbage"; present();
            require(snapshot(get).find("\"coverage_enabled\":true") != std::string::npos &&
                snapshot(get).find("\"rendering_mode_known\":false") != std::string::npos, "Malformed mode retains conservative AFW coverage");
            rendering_mode = "3"; present();
            auto publish = reinterpret_cast<CheekyUEVRPublishStereoFn>(GetProcAddress(runtime, "CheekyUEVR_PublishStereo"));
            CheekyUEVRStereoProjection invalid;
            require(publish && !publish(0, &invalid) && !publish(999, &invalid), "Unowned projection publication rejected");
            invalid.abi = 9; require(!publish(1, &invalid), "Unknown projection ABI rejected");
            require(snapshot(get).find("\"projection_valid\":true") != std::string::npos, "Adapter publishes public UEVR projections");
            hmd_active = false; present();
            require(snapshot(get).find("\"projection_valid\":false") != std::string::npos, "Inactive host HMD clears projection data");
            hmd_active = true; present(); reset();
            require(snapshot(get).find("\"projection_valid\":false") != std::string::npos, "Device reset invalidates automatic coverage immediately");
            present();
            verify_afw_test(get, command);
            present = nullptr; custom = nullptr; reset = nullptr;
            FreeLibrary(plugin);
            require(snapshot(get).find("\"projection_valid\":false") != std::string::npos,
                "Adapter unload immediately suppresses cached AFW projections");
            plugin = LoadLibraryW(plugin_path.c_str()); require(plugin != nullptr, "Reload AFW adapter");
            init = reinterpret_cast<UEVR_PluginInitializeFn>(GetProcAddress(plugin, "uevr_plugin_initialize"));
            require(init(&api), "Reconnect AFW adapter");
            custom("cheeky.foveated_dlss.load.v1", "true"); present();
            hmd_active = false; present(); hmd_active = true;
            require(snapshot(get).find("\"projection_valid\":false") != std::string::npos,
                "Reattachment does not inherit another adapter generation's projections");
            invalid.abi = 1;
            require(!publish(1, &invalid), "Old adapter generation cannot publish after reconnect");
            require(!publish_mode(1, 0), "Old adapter cannot disable new attachment's AFW coverage");
            present();
            require(snapshot(get).find("\"projection_valid\":true") != std::string::npos,
                "Reconnected AFW adapter publishes fresh projections");
            return 0;
        }
        if (dx11) {
            command("1\n10\nset\nD3D11D3D12Transport=true");
            require(received.find("Settings applied")!=received.npos,"UEVR accepts DX11 transport");
            reset(); present();
            require(snapshot(get).find("\"D3D11D3D12Transport\":true") != std::string::npos,
                "UEVR graphics recovery preserves transport preference");
            command("1\n11\ndefaults");
            require(received.find("Settings applied")!=received.npos,"DX11 defaults remain usable");
        }
        std::uint64_t duplicate_attachment = 99;
        CheekyUEVRStart duplicate; duplicate.config_directory=directory.c_str(); duplicate.attachment=&duplicate_attachment;
        require(!start(&duplicate) && duplicate_attachment==0, "Duplicate runtime attachment rejected");
        auto detach = reinterpret_cast<CheekyUEVRDetachFn>(GetProcAddress(runtime,"CheekyUEVR_Detach"));
        detach(duplicate_attachment);
        require(snapshot(get).find("\"attached\":true")!=std::string::npos,"Failed attachment cannot detach owner");
        reset();
        require(snapshot(get).find("\"processing\":false")!=std::string::npos,"Device reset pauses processing");
        present();
        require(snapshot(get).find("\"ready\":true")!=std::string::npos,"Renderer recovery");
        command("1\n2\nset\nWidth=0.65\nHeight=0.45\nEnabled=false\nEyeCalibrationContinuous=false");
        require(received.find("\"EyeCalibrationContinuous\":false") != received.npos &&
            received.find("\"continuous_validation\":true") != received.npos,
            "Auto corner calibration must keep validating while the full-search retention preference is disabled");
        require(std::abs(field(received,"Width")-0.65)<0.0001 && received.find("\"Enabled\":false") != received.npos, "Settings bridge transaction");
        const auto revision=field(received,"revision");
        command("1\n3\nset\nWidth=0.4\nHeight=nan");
        require(field(received,"revision")==revision && std::abs(field(received,"Width")-0.65)<0.0001, "Invalid transaction is atomic");
        command("1\n4\nset\nEnabled=true");
        command("1\n83\nset\nNrProcessingOrder=1\nNrWorkingScale=0.37");
        require(field(received,"NrProcessingOrder") == 1 && std::abs(field(received,"NrWorkingScale")-0.37)<0.0001,
            "Before rendering order command/acknowledgement");
        command("1\n84\nset\nNrProcessingOrder=0");
        require(field(received,"NrProcessingOrder") == 0 && std::abs(field(received,"NrWorkingScale")-0.37)<0.0001,
            "After rendering order changed working scale");
        const auto order_revision = field(received,"revision");
        const auto order_ack = field(received,"applied_request");
        for (const auto value : {"-1", "2", "3", "1.5", "nan"}) {
            const auto invalid = std::string("1\n85\nset\nNrWorkingScale=0.5\nNrProcessingOrder=") + value;
            command(invalid.c_str());
            require(field(received,"revision") == order_revision && field(received,"applied_request") == order_ack &&
                field(received,"NrProcessingOrder") == 0 && std::abs(field(received,"NrWorkingScale")-0.37)<0.0001,
                "Invalid order command must reject the whole transaction");
        }
        command("1\n40\nset\nNrProcessingOrder=1\nNrIntensity=0.4\nGazeSmoothingMs=60");
        command("1\n41\ndefaults_nr");
        require(std::abs(field(received,"Width")-0.65)<0.0001 && field(received,"NrIntensity")==1 && field(received,"NrProcessingOrder")==0 && field(received,"GazeSmoothingMs")==60,
            "NR reset preserves SR and gaze");
        command("1\n42\nset\nNrIntensity=0.4\nNrProcessingOrder=1");
        command("1\n43\ndefaults_gaze");
        require(std::abs(field(received,"Width")-0.65)<0.0001 && std::abs(field(received,"NrIntensity")-0.4)<0.0001 && field(received,"GazeSmoothingMs")==20,
            "Gaze reset preserves SR and NR");
        require(field(received,"NrProcessingOrder")==1, "Gaze reset changed NR order");
        require(received.find("\"EyeCalibrationContinuous\":false") != received.npos,
            "Gaze defaults must restore continuous calibration validation");
        command("1\n44\ndefaults_sr");
        require(std::abs(field(received,"Width")-0.55)<0.0001 && std::abs(field(received,"NrIntensity")-0.4)<0.0001,
            "SR reset preserves NR");
        require(field(received,"NrProcessingOrder")==1, "SR reset changed NR order");
        const auto reset_revision = field(received,"revision");
        command("1\n45\ndefaults_typo");
        require(field(received,"revision") == reset_revision, "Unknown reset group is atomic");
        command("1\n46\nset\nWidth=0.65\nNrIntensity=1\nNrWorkingScale=0.37\nEyeCalibrationContinuous=false");
        require(received.find("\"setting_groups\":{") != received.npos && received.find("\"nr_details\":{") != received.npos &&
            received.find("\"frame\":{") != received.npos, "Expanded diagnostic snapshot bridge");
        for (unsigned i = 0; i < 35; ++i) { Sleep(10); present(); }
        require(field(snapshot(get), "present_ms") > 0, "Host presents feed the exported frame cadence");
        if (!dx11) {
        const auto before = snapshot(get);
        ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Allocator");
        ComPtr<ID3D12GraphicsCommandList> list;
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"List");
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=64; desc.Height=32;
        desc.DepthOrArraySize=1; desc.MipLevels=1; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1;
        ComPtr<ID3D12Resource> source,destination;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_SOURCE,nullptr,IID_PPV_ARGS(&source)),"Source");
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&destination)),"Destination");
        list->CopyResource(destination.Get(),source.Get());
        D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=source.Get(); dst.pResource=destination.Get();
        src.Type=dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        check(list->Close(),"Close list"); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1,lists); wait_gpu(device.Get(),queue.Get());
        const auto after=snapshot(get);
        require(field(after,"submissions")>field(before,"submissions"),"Actual native ExecuteCommandLists observation");
        require(field(after,"copies")>=field(before,"copies")+2,"Actual native CopyResource/CopyTextureRegion observation");
        require(field(after,"submitted_copies")==field(before,"submitted_copies")+2,"Copy edges publish on submission");
        ComPtr<ID3D12CommandQueue> second_queue;
        check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&second_queue)),"Second queue");
        second_queue->ExecuteCommandLists(1,lists); wait_gpu(device.Get(),second_queue.Get());
        require(field(snapshot(get),"submitted_copies")==field(after,"submitted_copies")+2,"Resubmission on a second queue retains copy edges");
        check(allocator->Reset(),"Allocator reset"); check(list->Reset(allocator.Get(),nullptr),"List reset");
        list->CopyResource(destination.Get(),source.Get()); check(list->Close(),"Close discarded list");
        const auto before_discard=field(snapshot(get),"submitted_copies");
        check(list->Reset(allocator.Get(),nullptr),"Discard unsubmitted recording"); check(list->Close(),"Close reset list");
        queue->ExecuteCommandLists(1,lists); wait_gpu(device.Get(),queue.Get());
        require(field(snapshot(get),"submitted_copies")==before_discard,"Reset discards unsubmitted copy edges");
        require(field(snapshot(get),"resets")>field(before,"resets"),"Actual command-list reset observation");
        list.Reset(); source.Reset(); destination.Reset();
        require(field(snapshot(get),"destroyed")>=field(before,"destroyed")+3,"Resource and list lifetime sentinels");
        }
        command("1\n5\nreport");
        for (int i=0;i<500 && !std::filesystem::exists(root/"CheekyFoveatedDLSS-diagnostics.json");++i) Sleep(10);
        require(std::filesystem::exists(root/"CheekyFoveatedDLSS-diagnostics.json"),"Asynchronous report");
        for (int i=0;i<500 && snapshot(get).find("\"busy\":true")!=std::string::npos;++i) Sleep(10);
        require(snapshot(get).find("\"busy\":false")!=std::string::npos,"Support ZIP worker completed");
        bool zip_found{};
        if (std::filesystem::exists(root/"support")) for (const auto& entry : std::filesystem::directory_iterator(root/"support"))
            if (entry.path().extension()==".zip" && entry.file_size()>0) zip_found=true;
        require(zip_found,"Headless report creates a support ZIP without opening applications");
        for (int i=0;i<500 && field(snapshot(get),"saved_revision")<field(snapshot(get),"revision");++i) Sleep(10);
        Settings saved; std::string error; require(read_settings_file(root/"CheekyFoveatedDLSS.ini",saved,error),"Persisted runtime config");
        require(std::abs(saved.width-0.65f)<0.0001f && saved.enabled,"Persisted configured state");
        require(!saved.eye_calibration_continuous, "Calibration policy must be saved per game");
        require(saved.nr_processing_order == NrProcessingOrder::before_upscaling && saved.nr_working_scale == 0.37f,
            "Asynchronous persistence lost NR rendering order or scale");
        // Reproduce UEVR clearing callbacks before FreeLibrary.
        present=nullptr; custom=nullptr; reset=nullptr;
        FreeLibrary(plugin);
        require(GetModuleHandleW(L"CheekyFoveatedDLSS.dll")==nullptr,"Thin plugin really unloads");
        require(snapshot(get).find("\"attached\":false")!=std::string::npos,"Unload detaches resident runtime");
        require(snapshot(get).find("\"processing\":false")!=std::string::npos,"Unload disables processing");
        require(snapshot(get).find("\"status\":\"Disabled\"")!=std::string::npos,"Unload suspends calibration without loader-lock GPU work");
        plugin=LoadLibraryW(plugin_path.c_str()); require(plugin!=nullptr,"Reload plugin");
        init=reinterpret_cast<UEVR_PluginInitializeFn>(GetProcAddress(plugin,"uevr_plugin_initialize"));
        require(init(&api),"Reconnect existing runtime");
        custom("cheeky.foveated_dlss.load.v1", "true"); present(); command("1\n6\nget");
        require(received.find("\"attached\":true")!=received.npos && std::abs(field(received,"Width")-0.65)<0.0001,"Reload retains settings");
        require(received.find("\"EyeCalibrationContinuous\":false") != received.npos,
            "Reconnect must retain the saved calibration policy");
        require(field(received,"NrProcessingOrder")==1 && std::abs(field(received,"NrWorkingScale")-0.37)<0.0001,
            "Reconnect lost rendering order or scale");
        detach(1);
        require(snapshot(get).find("\"attached\":true")!=std::string::npos,"Stale attachment cannot detach reloaded plugin");
        puts("UEVR settings, ABI, bridge, native observation, persistence, unload/reload tests passed");
        return 0;
    } catch (const std::exception& e) { fprintf(stderr,"UEVR TEST FAILED: %s\n",e.what()); return 1; }
}
