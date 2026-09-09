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
    Settings s; require(set_named_setting(s, "Width", "0.72"), "Parse setting");
    require(!set_named_setting(s, "Width", "nan"), "Reject NaN");
    require(!set_named_setting(s, "Width", "0.7trailing"), "Reject trailing garbage");
    require(!set_named_setting(s, "Enabled", "maybe"), "Reject bad bool");
    require(!set_named_setting(s, "CenterMode", "99"), "Reject enum");
    require(!set_named_setting(s, "GazeQuantizationPixels", "-1"), "Reject negative unsigned");
    s.enabled = false; s.nr_motion_scale_x_multiplier = -1.25f;
    std::string error; const auto path = root / "roundtrip.ini";
    require(write_settings_file(path, s, error), "Write settings");
    Settings r; require(read_settings_file(path, r, error), "Read settings");
    require(serialize_settings(r) == serialize_settings(s), "Roundtrip all persisted fields");
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
        bool hardware{};
        for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--hardware") hardware = true;
        const std::string mode = argc > 1 ? argv[1] : "";
        const bool late = mode.starts_with("--late-");
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
        const auto plugin_path = bin / "CheekyFoveatedDLSS.dll";
        if (late) prepare_late_attach_test(bin,device11.Get(),device.Get(),queue.Get(),mode.ends_with("-c"),mode.starts_with("--late-streamline"));
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
        require(received.find("\"eye_calibration\":{\"backend\":\"Waiting for VR\",\"graphics_api\":0,\"enabled\":true") != received.npos,
            "Shared eye calibration diagnostics enabled on attachment");
        command("1\n80\ncalibration_disable");
        require(received.find("\"status\":\"Disabled\"") != received.npos, "Calibration disable command");
        command("1\n81\ncalibration_enable");
        command("1\n82\ncalibration_reset");
        require(received.find("Waiting for OpenVR or OpenXR") != received.npos, "Unavailable backend must not claim active calibration");
        if (late) {
            command("1\n2\nset\nEnabled=true\nPeripheralDlaa=false\nAutoStereoAlignment=false\nCenterMode=0\nNrEnabled=false");
            verify_late_attach_test(get, command);
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
        command("1\n2\nset\nWidth=0.65\nHeight=0.45\nEnabled=false");
        require(std::abs(field(received,"Width")-0.65)<0.0001 && received.find("\"Enabled\":false") != received.npos, "Settings bridge transaction");
        const auto revision=field(received,"revision");
        command("1\n3\nset\nWidth=0.4\nHeight=nan");
        require(field(received,"revision")==revision && std::abs(field(received,"Width")-0.65)<0.0001, "Invalid transaction is atomic");
        command("1\n4\nset\nEnabled=true");
        command("1\n40\nset\nNrIntensity=0.4\nGazeSmoothingMs=60");
        command("1\n41\ndefaults_nr");
        require(std::abs(field(received,"Width")-0.65)<0.0001 && field(received,"NrIntensity")==1 && field(received,"GazeSmoothingMs")==60,
            "NR reset preserves SR and gaze");
        command("1\n42\nset\nNrIntensity=0.4");
        command("1\n43\ndefaults_gaze");
        require(std::abs(field(received,"Width")-0.65)<0.0001 && std::abs(field(received,"NrIntensity")-0.4)<0.0001 && field(received,"GazeSmoothingMs")==20,
            "Gaze reset preserves SR and NR");
        command("1\n44\ndefaults_sr");
        require(std::abs(field(received,"Width")-0.55)<0.0001 && std::abs(field(received,"NrIntensity")-0.4)<0.0001,
            "SR reset preserves NR");
        const auto reset_revision = field(received,"revision");
        command("1\n45\ndefaults_typo");
        require(field(received,"revision") == reset_revision, "Unknown reset group is atomic");
        command("1\n46\nset\nWidth=0.65\nNrIntensity=1");
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
        detach(1);
        require(snapshot(get).find("\"attached\":true")!=std::string::npos,"Stale attachment cannot detach reloaded plugin");
        puts("UEVR settings, ABI, bridge, native observation, persistence, unload/reload tests passed");
        return 0;
    } catch (const std::exception& e) { fprintf(stderr,"UEVR TEST FAILED: %s\n",e.what()); return 1; }
}
