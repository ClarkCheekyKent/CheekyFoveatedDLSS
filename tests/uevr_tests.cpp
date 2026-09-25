#include "runtime_api.hpp"
#include "settings_io.hpp"
#include "processing_owner.hpp"
#include "late_attach_tests.hpp"
#include "runtime_search.hpp"
#include "frame_cadence.hpp"
#include "support_bundle.hpp"
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

using namespace cheeky::foveated_dlss;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void check(HRESULT hr, const char* why) { require(SUCCEEDED(hr), why); }
std::wstring directory;
std::string received;
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
bool add_present(UEVR_OnPresentCb f) { present = f; return true; }
bool add_reset(UEVR_OnDeviceResetCb f) { reset = f; return true; }
bool add_custom(UEVR_OnCustomEventCb f) { custom = f; return true; }
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
    Settings s; require(s.d3d12_lower_hook, "Lower hook is the default");
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
        if (afw || inactive_afw) api.vr = &vr_api;
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
        HMODULE plugin = LoadLibraryW(plugin_path.c_str()); require(plugin != nullptr, "Load actual UEVR plugin DLL");
        auto init = reinterpret_cast<UEVR_PluginInitializeFn>(GetProcAddress(plugin, "uevr_plugin_initialize"));
        require(init != nullptr, "Plugin entry export");
        require(!init(nullptr), "Reject invalid host API");
        version.minor=0; require(!init(&api), "Reject old UEVR API"); version.minor=39;
        require(init(&api), "Initialize real plugin via fake UEVR callbacks");
        require(init(&api), "Repeated host init is idempotent");
        require(present && custom && reset, "All callbacks registered");
        const auto runtime = GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll"); require(runtime != nullptr, "Runtime dependency loaded");
        auto get = reinterpret_cast<CheekyUEVRSnapshotFn>(GetProcAddress(runtime, "CheekyUEVR_Snapshot"));
        auto start = reinterpret_cast<CheekyUEVRStartFn>(GetProcAddress(runtime, "CheekyUEVR_Start"));
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
            require(observed.find("\"backend\":\"OpenVR\"") != observed.npos && field(observed, "frames") == 1,
                "Cached WaitGetPoses activates OpenVR calibration exactly once");
            for (unsigned i = 0; i < 5; ++i) present();
            require((*static_cast<void***>(cached_compositor))[2] == hooked_wait, "Repeated host ticks do not stack hooks");
            reinterpret_cast<Wait>(hooked_wait)(cached_compositor, nullptr, 0, nullptr, 0);
            require(field(snapshot(get), "frames") == 2, "No duplicate observation after repeated ticks");
            reset(); present();
            require((*static_cast<void***>(cached_compositor))[2] == hooked_wait, "Device reset can reattach without stacking hooks");
            reinterpret_cast<Wait>(hooked_wait)(cached_compositor, nullptr, 0, nullptr, 0);
            require(field(snapshot(get), "frames") == 3, "Observation continues after device reset");
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
        const auto calibration_start = received.find("\"eye_calibration\":{");
        require(calibration_start != received.npos, "Shared eye calibration diagnostics present");
        const auto calibration = received.substr(calibration_start, received.find('}', calibration_start) - calibration_start);
        require(calibration.find("\"backend\":\"Waiting for VR\"") != calibration.npos &&
            field(calibration, "graphics_api") == 0 && calibration.find("\"enabled\":true") != calibration.npos,
            "Shared eye calibration diagnostics enabled on attachment");
        command("1\n80\ncalibration_disable");
        require(received.find("\"status\":\"Disabled\"") != received.npos, "Calibration disable command");
        command("1\n81\ncalibration_enable");
        command("1\n82\ncalibration_reset");
        command("1\n86\ncalibration_recalibrate");
        require(received.find("Waiting for OpenVR or OpenXR") != received.npos, "Unavailable backend must not claim active calibration");
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
            require(received.find("transport is unavailable")!=received.npos,"DX11 transport rejected explicitly");
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
            received.find("\"continuous_validation\":false") != received.npos,
            "Calibration policy must reach the shared runtime");
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
        require(init(&api),"Reconnect existing runtime"); command("1\n6\nget");
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
