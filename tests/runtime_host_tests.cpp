#include "runtime_host_api.hpp"
#include "runtime_api.hpp"
#include "processing_owner.hpp"
#include "mock_ngx_parameters.hpp"
#include <Windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T proc(HMODULE module, const char* name) {
    const auto address = GetProcAddress(module, name);
    require(address != nullptr, name);
    return reinterpret_cast<T>(address);
}
std::string snapshot(CheekyRuntimeSnapshotFn get) {
    std::vector<char> bytes(cheeky_runtime_message_capacity);
    require(get(bytes.data(), static_cast<std::uint32_t>(bytes.size())), "Snapshot export");
    return bytes.data();
}
bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
double field(const std::string& text, const char* name) {
    const auto token = std::string("\"") + name + "\":";
    const auto pos = text.find(token);
    require(pos != std::string::npos, "Missing numeric field");
    return std::stod(text.substr(pos + token.size()));
}
void verify_transport(CheekyRuntimeCommandFn command, CheekyRuntimeSnapshotFn get,
    std::uint64_t attachment, ID3D11Device* device, ID3D11DeviceContext* context, HMODULE ngx,
    const std::filesystem::path& log_path,bool depth24=false,bool backpressure=false) {
    using namespace cheeky::foveated_dlss;
    using Init = NgxResult (*)(unsigned long long, const wchar_t*, ID3D11Device*, const void*, unsigned);
    using Create = NgxResult (*)(ID3D11DeviceContext*, unsigned, NgxParameters*, NgxHandle**);
    using Evaluate = NgxResult (*)(ID3D11DeviceContext*, const NgxHandle*, const NgxParameters*, NgxProgressCallback);
    using Release = NgxResult (*)(NgxHandle*);
    for (unsigned i = 0; i < 200 && !contains(snapshot(get), "\"direct_detour\":true"); ++i) Sleep(25);
    require(contains(snapshot(get), "\"direct_detour\":true"), "NGX detours installed for transport fixture");
    require(ngx_succeeded(proc<Init>(ngx, "NVSDK_NGX_D3D11_Init")(42, L".", device, nullptr, 1)), "Record DX11 initialization for private transport");
    // Standalone/ASI runtime DLLs are nested away from the game's DLSS DLL.
    // The core must receive an explicit feature path when Init was recovered
    // or the game's Init supplied no FeatureCommonInfo.
    proc<void(*)(bool)>(GetModuleHandleW(L"_nvngx.dll"), "CheekyFakeRequireFeaturePath")(true);
    MockNgxParameters parameters;
    parameters.Set("Width", 128U); parameters.Set("Height", 128U);
    parameters.Set("OutWidth", 256U); parameters.Set("OutHeight", 256U);
    parameters.Set("DLSS.Feature.Create.Flags", 2U); parameters.Set("PerfQualityValue", 2U);
    parameters.Set("MV.Scale.X", 1.F); parameters.Set("MV.Scale.Y", 1.F);
    const char* names[]{"Color", "Depth", "MotionVectors", "Output"};
    std::array<ComPtr<ID3D11Texture2D>, 4> textures;
    for (unsigned i = 0; i < textures.size(); ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = i == 3 ? 256U : 128U;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = i == 1 ? DXGI_FORMAT_R32_FLOAT : i == 2 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if(depth24 && i==1){desc.Format=DXGI_FORMAT_R24G8_TYPELESS;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_DEPTH_STENCIL;}
        require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &textures[i])), "Transport game texture");
        parameters.Set(names[i], static_cast<ID3D11Resource*>(textures[i].Get()));
    }
    ComPtr<ID3D11DepthStencilView> depth_target;
    if(depth24) {
        D3D11_DEPTH_STENCIL_VIEW_DESC desc{};desc.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;desc.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        require(SUCCEEDED(device->CreateDepthStencilView(textures[1].Get(),&desc,&depth_target)),"24-bit depth target");
        context->ClearDepthStencilView(depth_target.Get(),D3D11_CLEAR_DEPTH,.375F,0);
        context->OMSetRenderTargets(0,nullptr,depth_target.Get());
    }
    NgxHandle* handle{};
    require(ngx_succeeded(proc<Create>(ngx, "NVSDK_NGX_D3D11_CreateFeature")(context, 1, &parameters, &handle)), "Create DX11 game feature");
    const auto evaluate = proc<Evaluate>(ngx, "NVSDK_NGX_D3D11_EvaluateFeature");
    const auto release = proc<Release>(ngx, "NVSDK_NGX_D3D11_ReleaseFeature");
    const auto original_parameters = parameters.values;
    require(command(attachment, "1\n20\nset\nEnabled=true\nWidth=0.63\nPeripheralDlaa=false\nAutoStereoAlignment=false\nCenterMode=0\nD3D11D3D12Transport=true\nNrEnabled=false"), "Enable standalone DX11 transport");
    bool active{};
    for (unsigned i = 0; i < 100 && !active; ++i) {
        if(depth24)parameters.values=original_parameters;
        require(ngx_succeeded(evaluate(context, handle, &parameters, nullptr)), "DX11 transport evaluation");
        context->Flush();
        active = contains(snapshot(get), "\"execution_path\":\"DX12 Transport\"");
        if(!depth24 || active)require(parameters.values == original_parameters, "Transport preserves the original game parameters");
        if (!active) Sleep(25);
    }
    if (!active) puts(snapshot(get).c_str());
    require(active, "Actual private DX12 transport executes from generic DX11 host");
    if (backpressure) {
        // Hold GPU work behind a CPU-signaled fence, filling all three slots.
        // The fourth evaluation must wait for a slot, not silently omit NR.
        ComPtr<ID3D11Device5> device5;
        ComPtr<ID3D11DeviceContext4> context4;
        ComPtr<ID3D11Fence> gate11;
        ComPtr<ID3D12Fence> gate12;
        ComPtr<ID3D12Device> device12;
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        require(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5))) &&
            SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4))) &&
            SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
            SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12))),
            "Backpressure devices");
        require(SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate11))), "Backpressure fence");
        HANDLE shared{};
        require(SUCCEEDED(gate11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared)), "Share backpressure fence");
        const auto opened = device12->OpenSharedHandle(shared, IID_PPV_ARGS(&gate12));
        CloseHandle(shared);
        require(SUCCEEDED(opened), "Open backpressure fence");
        ComPtr<ID3D11Query> completed;
        const D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
        require(SUCCEEDED(device->CreateQuery(&desc, &completed)), "Backpressure completion query");
        for (unsigned order = 0; order < 2; ++order) {
            const auto cmd = std::string("1\n30\nset\nNrEnabled=true\nNrFoveated=false\nNrProcessingOrder=") + std::to_string(order);
            require(command(attachment, cmd.c_str()), "Enable backpressure NR");
            // Warm and drain all slots before deliberately holding the queue.
            for (unsigned i = 0; i < 6; ++i) {
                parameters.values = original_parameters;
                require(ngx_succeeded(evaluate(context, handle, &parameters, nullptr)), "Warm backpressure slot");
                context->End(completed.Get()); context->Flush();
                HRESULT done = S_FALSE;
                for (unsigned retry = 0; retry < 2000 && done == S_FALSE; ++retry) {
                    done = context->GetData(completed.Get(), nullptr, 0, 0);
                    if (done == S_FALSE) Sleep(1);
                }
                require(done == S_OK, "Drain backpressure warmup");
            }
            const auto before = snapshot(get);
            const auto nr_begin = before.find("\"nr_details\":");
            const auto evaluations = field(before.substr(nr_begin), "evaluations");
            require(SUCCEEDED(context4->Wait(gate11.Get(), order + 1)), "Hold GPU queue");
            // Always release the gate, including when an assertion throws.
            std::jthread release_gate([gate12, order] { Sleep(250); gate12->Signal(order + 1); });
            for (unsigned frame = 0; frame < 4; ++frame) {
                parameters.values = original_parameters;
                require(ngx_succeeded(evaluate(context, handle, &parameters, nullptr)), "Backlogged evaluation");
                const auto state = snapshot(get);
                if (!contains(state, "\"execution_path\":\"DX12 Transport\"") ||
                    !contains(state, "\"nr\":\"Active\"")) puts(state.c_str());
                require(contains(state, "\"execution_path\":\"DX12 Transport\"") &&
                    contains(state, "\"nr\":\"Active\""), "GPU backlog must not bypass transport/NR");
            }
            context->Flush();
            const auto after = snapshot(get);
            require(field(after.substr(after.find("\"nr_details\":")), "evaluations") == evaluations + 4,
                "Every backlogged frame evaluates NR");
        }
        release(handle);
        puts("PASS GPU backlog preserves transport and Before/After NR on every frame");
        return;
    }
    for (unsigned mode = 0; mode < 4; ++mode) {
        const auto order = mode % 2;
        const auto cmd = std::string("1\n21\nset\nNrEnabled=true\nNrFoveated=") + (mode >= 2 ? "true" : "false") +
            "\nNrWidth=0.5\nNrHeight=0.5\nNrProcessingOrder=" + std::to_string(order);
        require(command(attachment, cmd.c_str()), "Enable NR through private transport");
        bool nr_active{};
        for (unsigned i = 0; i < 100 && !nr_active; ++i) {
            require(ngx_succeeded(evaluate(context, handle, &parameters, nullptr)), "Transport NR evaluation");
            context->Flush();
            require(parameters.values == original_parameters, "Transport NR restores the original game parameters");
            const auto text = snapshot(get);
            nr_active = contains(text, "\"nr\":\"Active\"") &&
                contains(text, order ? "\"processing_order\":1" : "\"processing_order\":0");
            if (!nr_active) Sleep(25);
        }
        if (!nr_active) puts(snapshot(get).c_str());
        require(nr_active, "Before/After NR executes through standalone DX11 transport");
        require(contains(snapshot(get), "\"observer\":{\"ready\":true"), "Private transport installs its native queue observer for NR");
        require(field(snapshot(get), "submissions") > 0, "Observer sees actual private transport submissions");
    }
    if(depth24) {
        ComPtr<ID3D11DepthStencilView> restored;context->OMGetRenderTargets(0,nullptr,&restored);
        require(restored.Get()==depth_target.Get(),"Depth conversion restores the game's depth target");
        context->OMSetRenderTargets(0,nullptr,nullptr);release(handle);
        puts("PASS 24-bit DX11 depth transport and before/after NR, full/foveated");return;
    }
    // Drain every ring slot, then resize only NR. Unchanged SR/peripheral GPU
    // textures must survive, even though the NR feature itself gets rebuilt.
    ComPtr<ID3D11Query> completed;
    const D3D11_QUERY_DESC completion_desc{D3D11_QUERY_EVENT, 0};
    require(SUCCEEDED(device->CreateQuery(&completion_desc, &completed)), "Resize completion query");
    const auto frames = [&](bool expect_nr = true) {
        for (unsigned frame = 0; frame < 6; ++frame) {
            require(ngx_succeeded(evaluate(context, handle, &parameters, nullptr)), "Resize evaluation");
            context->End(completed.Get()); context->Flush();
            HRESULT done = S_FALSE;
            for (unsigned retry = 0; retry < 1000 && done == S_FALSE; ++retry) {
                done = context->GetData(completed.Get(), nullptr, 0, 0);
                if (done == S_FALSE) Sleep(1);
            }
            require(done == S_OK, "Resized frame completes on GPU");
            require(parameters.values == original_parameters, "Resize restores game parameters");
        }
        if (expect_nr) require(contains(snapshot(get), "\"nr\":\"Active\""), "NR active after resize");
    };
    const auto allocations = [&] {
        std::ifstream log(log_path);
        require(log.good(), "Read allocation diagnostics");
        std::array<unsigned, 3> counts{};
        for (std::string line; std::getline(log, line);) {
            if (line.find("Transport texture ") == std::string::npos || line.find(" shared via ") == std::string::npos) continue;
            ++counts[line.find("Transport texture NR ") != std::string::npos ? 2 :
                line.find("Transport texture peripheral ") != std::string::npos ? 1 : 0];
        }
        return counts;
    };
    require(command(attachment, "1\n24\nset\nEnabled=true\nPeripheralDlaa=true\nNrEnabled=true\nNrFoveated=true\nNrProcessingOrder=0\nNrWidth=0.5"), "Prepare NR resize");
    frames();
    const auto before_resize = allocations();
    require(command(attachment, "1\n25\nset\nNrWidth=0.7"), "Change only NR width");
    frames();
    const auto after_nr_resize = allocations();
    require(after_nr_resize[0] == before_resize[0] && after_nr_resize[1] == before_resize[1], "NR resize preserves SR and peripheral textures");
    require(after_nr_resize[2] > before_resize[2], "NR resize replaces NR textures");
    require(command(attachment, "1\n26\nset\nWidth=0.8"), "Change only SR width");
    frames();
    const auto after_sr_resize = allocations();
    require(after_sr_resize[0] > after_nr_resize[0], "SR resize replaces SR textures");
    require(after_sr_resize[1] == after_nr_resize[1] && after_sr_resize[2] == after_nr_resize[2], "SR resize preserves peripheral and independent NR textures");
    frames();
    require(allocations() == after_sr_resize, "Steady frames allocate no transport textures");
    require(command(attachment, "1\n27\nset\nNrEnabled=false\nPeripheralDlaa=false\nWidth=0.6\nNrWidth=0.9"), "Resize with optional groups disabled");
    frames(false);
    const auto disabled_allocations = allocations();
    require(command(attachment, "1\n28\nset\nNrEnabled=true\nPeripheralDlaa=true"), "Re-enable optional texture groups after resize");
    frames();
    const auto reenabled_allocations = allocations();
    require(reenabled_allocations[0] == disabled_allocations[0], "Re-enable preserves unchanged SR textures");
    require(reenabled_allocations[1] > disabled_allocations[1] && reenabled_allocations[2] > disabled_allocations[2], "Re-enable allocates current optional geometry");
    puts("PASS: NR/SR resize preserves unrelated textures across all ring slots");
    require(command(attachment, "1\n22\nset\nEnabled=false\nNrEnabled=true"), "Enable independent NR-only transport");
    require(ngx_succeeded(evaluate(context, handle, &parameters, nullptr)), "NR-only transport evaluation");
    context->Flush();
    require(contains(snapshot(get), "\"nr\":\"Active\""), "NR continues while foveated SR is disabled");
    require(command(attachment, "1\n23\nset\nEnabled=true\nNrEnabled=false\nWidth=0.63"), "Disable transport NR and restore shared fixture width");
    require(ngx_succeeded(release(handle)), "Release transported game feature");
    context->Flush();
    puts("PASS: DX11 private transport, full/foveated Before/After NR and NR-only with native queue observation");
}
}

int main(int argc, char** argv) {
    try {
        bool dx11{}, conflict{}, optiscaler{}, transport{}, forwarded_transport{},depth24{},backpressure{};
        unsigned openvr_version{};
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--dx11") dx11 = true;
            else if (arg == "--conflict") conflict = true;
            else if (arg == "--optiscaler") optiscaler = true;
            else if (arg == "--transport") { transport = true; dx11 = true; }
            else if (arg == "--transport-depth24") { transport = dx11 = depth24 = true; }
            else if (arg == "--transport-backpressure") { transport = dx11 = backpressure = true; }
            else if (arg == "--transport-forwarded") { transport = true; dx11 = true; forwarded_transport = true; }
            else if (arg.starts_with("--openvr-late-")) {
                openvr_version = static_cast<unsigned>(std::stoul(arg.substr(14)));
                require(openvr_version == 22 || openvr_version == 27 || openvr_version == 28 || openvr_version == 29, "Supported OpenVR fixture version");
            }
            else throw std::runtime_error("Unknown runtime host test option");
        }
        std::array<wchar_t, 32768> module_path{};
        const auto length = GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
        require(length && length < module_path.size(), "Test executable path");
        const auto bin = std::filesystem::path(module_path.data()).parent_path();
        const auto directory = bin / "runtime-host-test-data" /
            (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(directory);
        const auto directory_text = directory.wstring();
        HMODULE fake_openvr{};
        void* cached_compositor{};
        void* original_wait{};
        void (*set_openvr_initialized)(bool){};
        unsigned (*openvr_queries)(){};
        unsigned (*openvr_validity_queries)(){};
        unsigned (*openvr_init_calls)(){};
        if (openvr_version) {
            fake_openvr = LoadLibraryW((bin / "test-fixtures" / "openvr_api.dll").c_str());
            require(fake_openvr != nullptr, "Load existing native OpenVR session");
            proc<void(*)(unsigned)>(fake_openvr, "CheekyFakeOpenVR_SetVersion")(openvr_version);
            set_openvr_initialized = proc<void(*)(bool)>(fake_openvr, "CheekyFakeOpenVR_SetInitialized");
            openvr_queries = proc<unsigned(*)()>(fake_openvr, "CheekyFakeOpenVR_InterfaceQueries");
            openvr_validity_queries = proc<unsigned(*)()>(fake_openvr, "CheekyFakeOpenVR_ValidityQueries");
            openvr_init_calls = proc<unsigned(*)()>(fake_openvr, "CheekyFakeOpenVR_InitCalls");
            int error{};
            cached_compositor = proc<void*(*)(const char*, int*)>(fake_openvr, "VR_GetGenericInterface")(
                ("IVRCompositor_0" + std::to_string(openvr_version)).c_str(), &error);
            require(cached_compositor && !error, "Native host caches compositor before Cheeky loads");
            original_wait = (*static_cast<void***>(cached_compositor))[2];
            set_openvr_initialized(false);
        }
        HANDLE other_owner{};
        if (conflict) {
            other_owner = cheeky::foveated_dlss::claim_processing_owner();
            require(other_owner != nullptr, "Reserve another integration's processing ownership");
        }
        auto runtime_path = bin / "CheekyFoveatedDLSS" / "CheekyFoveatedDLSSRuntime.dll";
        HMODULE fake_ngx{};
        if (transport) {
            // Keep every fake vendor module private to this test process.
            const auto fixture = bin / "test-fixtures" / "nvngx_dlss.dll";
            const auto isolated = directory / "runtime";
            std::filesystem::create_directories(isolated);
            std::filesystem::copy_file(runtime_path, isolated / runtime_path.filename());
            runtime_path = isolated / runtime_path.filename();
            std::filesystem::copy_file(fixture, isolated / "nvngx_dlssnr.dll");
            std::filesystem::copy_file(fixture, isolated / "_nvngx.dll");
            std::filesystem::copy_file(fixture, isolated / "nvngx_dlss.dll");
            fake_ngx = LoadLibraryW((isolated / "nvngx_dlss.dll").c_str());
            require(fake_ngx && LoadLibraryW((isolated / "_nvngx.dll").c_str()), "Load fake public and private NGX runtimes");
            if (forwarded_transport) {
                // Real NVIDIA core dispatches private DX12 calls into the
                // public feature DLL, whose exports are also intercepted.
                proc<void(*)(HMODULE, bool)>(GetModuleHandleW(L"_nvngx.dll"),
                    "CheekyFakeForwardTo")(fake_ngx, false);
            }
        }
        const auto runtime = LoadLibraryW(runtime_path.c_str());
        require(runtime != nullptr, "Load resident runtime without ReShade or UEVR");
        const auto start = proc<CheekyRuntimeStartFn>(runtime, "CheekyRuntime_Start");
        const auto tick = proc<CheekyRuntimeTickFn>(runtime, "CheekyRuntime_Tick");
        const auto detach = proc<CheekyRuntimeDetachFn>(runtime, "CheekyRuntime_Detach");
        const auto command = proc<CheekyRuntimeCommandFn>(runtime, "CheekyRuntime_Command");
        const auto get = proc<CheekyRuntimeSnapshotFn>(runtime, "CheekyRuntime_Snapshot");
        const auto legacy_start = proc<CheekyUEVRStartFn>(runtime, "CheekyUEVR_Start");
        const auto publish_stereo = proc<CheekyUEVRPublishStereoFn>(runtime, "CheekyUEVR_PublishStereo");
        const auto publish_mode = proc<CheekyUEVRPublishRenderingModeFn>(runtime, "CheekyUEVR_PublishRenderingMode");
        require(!start(nullptr), "Reject null Start");
        require(!get(nullptr, 1), "Reject null snapshot storage");
        char tiny_buffer[2]{'x','x'};
        require(!get(tiny_buffer, sizeof(tiny_buffer)) && tiny_buffer[0] == 0, "Small snapshot buffer is cleared");

        std::uint64_t attachment{};
        CheekyRuntimeStart input;
        input.config_directory = directory_text.c_str(); input.attachment = &attachment;
        input.renderer = dx11 ? 0U : 1U;
        input.host = optiscaler ? CheekyRuntimeHost::optiscaler : CheekyRuntimeHost::standalone;
        auto bad = input; ++bad.abi; require(!start(&bad), "Reject unknown ABI");
        bad = input; --bad.size; require(!start(&bad), "Reject invalid Start size");
        bad = input; bad.renderer = UINT32_MAX; require(!start(&bad), "Reject unsupported renderer");
        bad = input; bad.host = static_cast<CheekyRuntimeHost>(99); require(!start(&bad), "Reject unknown host");
        if (conflict) {
            require(!start(&input) && attachment == 0, "Generic host cannot steal another integration's owner");
            const auto text = snapshot(get);
            require(contains(text, "Another Cheeky integration") && contains(text, "\"processing\":false"), "Owner conflict reported and processing disabled");
            CloseHandle(other_owner);
            puts("PASS: generic runtime ownership conflict");
            return 0;
        }
        require(start(&input) && attachment != 0, "Start before graphics discovery");
        if (openvr_version) {
            const auto queries_before = openvr_queries();
            // A nonzero generation token persists after shutdown. It must not
            // be treated as permission to fetch interfaces or initialize VR.
            for (unsigned i = 0; i < 100 && !openvr_validity_queries(); ++i) Sleep(25);
            require(openvr_validity_queries() && openvr_queries() == queries_before && openvr_init_calls() == 0,
                "Inactive OpenVR session is never probed or initialized");
            require((*static_cast<void***>(cached_compositor))[2] == original_wait, "Inactive cached compositor stays untouched");
            set_openvr_initialized(true);
            for (unsigned i = 0; i < 200 && (*static_cast<void***>(cached_compositor))[2] == original_wait; ++i) Sleep(25);
            const auto hooked_wait = (*static_cast<void***>(cached_compositor))[2];
            require(hooked_wait != original_wait, "Native cached compositor recovered without another host getter call");
            require(openvr_init_calls() == 0, "Recovery never initializes OpenVR");
            using Wait = int (*)(void*, void*, unsigned, void*, unsigned);
            require(reinterpret_cast<Wait>(hooked_wait)(cached_compositor, nullptr, 0, nullptr, 0) == 0, "Recovered compositor forwards original WaitGetPoses");
            require(contains(snapshot(get), "\"backend\":\"OpenVR\"") && field(snapshot(get), "frames") == 1,
                "Native recovered WaitGetPoses activates calibration exactly once");
            detach(attachment);
            puts("PASS: native OpenVR cached compositor recovery without initialization");
            return 0;
        }
        auto text = snapshot(get);
        require(contains(text, "\"attached\":true") && contains(text, "\"ready\":false") &&
            contains(text, "\"processing\":false"), "Pending graphics keeps processing paused");
        require(contains(text, optiscaler ? "\"host\":\"optiscaler\"" : "\"host\":\"standalone\""), "Snapshot identifies the actual host");
        require(contains(text, "\"host_supports_afw_projection\":false"), "Generic host does not claim UEVR projection data");
        require(command(attachment, "1\n1\nset\nEnabled=true\nWidth=0.63"), "Settings available before graphics");
        require(contains(snapshot(get), "\"processing\":false"), "Settings cannot bypass graphics readiness");

        ComPtr<ID3D11Device> device11;
        ComPtr<ID3D11DeviceContext> context11;
        ComPtr<ID3D12Device> device12;
        ComPtr<ID3D12CommandQueue> queue;
        if (dx11) {
            require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, &device11, nullptr, &context11)), "Create WARP D3D11 device");
        } else {
            ComPtr<IDXGIFactory4> factory;
            ComPtr<IDXGIAdapter> adapter;
            require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
                SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12))), "Create WARP D3D12 device");
            D3D12_COMMAND_QUEUE_DESC desc{};
            require(SUCCEEDED(device12->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))), "Create graphics command queue");
            tick(attachment, 1, device12.Get(), nullptr);
            require(contains(snapshot(get), "\"ready\":false"), "D3D12 remains paused without a queue");
        }
        void* device = dx11 ? static_cast<void*>(device11.Get()) : static_cast<void*>(device12.Get());
        tick(attachment, input.renderer, device, queue.Get());
        text = snapshot(get);
        require(contains(text, "\"ready\":true") && contains(text, "\"processing\":true"), "Discovered graphics enables processing");
        if (!dx11) require(contains(text, "\"observer\":{\"ready\":true"), "Native D3D12 submission observer installed");
        else {
            require(command(attachment, "1\n2\nset\nD3D11D3D12Transport=true\nNrEnabled=true"), "Generic DX11 host exposes transport and NR");
            require(contains(snapshot(get), "\"D3D11D3D12Transport\":true"), "Transport preference retained");
        }
        if (transport) verify_transport(command, get, attachment, device11.Get(), context11.Get(), fake_ngx,
            directory / (optiscaler ? "CheekyFoveatedDLSS-OptiScaler.log" : "CheekyFoveatedDLSS-Standalone.log"),depth24,backpressure);
        if(depth24 || backpressure){detach(attachment);return 0;}
        CheekyUEVRStereoProjection projection;
        require(!publish_stereo(attachment, &projection) && !publish_mode(attachment, 3), "UEVR-only publications reject generic host attachments");
        std::uint64_t duplicate = 99;
        auto second = input; second.attachment = &duplicate;
        require(!start(&second) && duplicate == 0, "Reject duplicate host attachment");
        CheekyUEVRStart legacy;
        legacy.config_directory = directory_text.c_str(); legacy.attachment = &duplicate;
        require(!legacy_start(&legacy) && duplicate == 0, "Legacy host cannot attach over generic owner");
        detach(0); detach(attachment + 100);
        require(contains(snapshot(get), "\"attached\":true"), "Stale detach cannot disable the owner");
        require(!command(attachment + 100, "1\n3\nset\nEnabled=false"), "Stale commands rejected");
        require(!command(attachment, "1\n4\nset\nWidth=0.42\nHeight=nan"), "Invalid settings transaction rejected");
        require(std::abs(field(snapshot(get), "Width") - .63) < .00001, "Invalid transaction preserves all prior settings");
        tick(attachment, input.renderer, nullptr, nullptr);
        require(contains(snapshot(get), "\"processing\":false"), "Device reset pauses processing");
        tick(attachment, input.renderer, device, queue.Get());
        require(contains(snapshot(get), "\"processing\":true"), "Device reset recovery");
        require(command(attachment, "1\n5\nreport"), "Create host-specific support report");
        std::filesystem::path report;
        for (unsigned i = 0; i < 200 && report.empty(); ++i) {
            if (std::filesystem::exists(directory / "support")) {
                for (const auto& entry : std::filesystem::directory_iterator(directory / "support"))
                    if (entry.path().extension() == ".zip") report = entry.path();
            }
            if (report.empty()) Sleep(25);
        }
        require(!report.empty(), "Support ZIP finished");
        require(report.filename().string().starts_with(optiscaler ? "Cheeky-OptiScaler-" : "Cheeky-Standalone-"), "Support filename uses host identity");
        std::ifstream zip(report, std::ios::binary);
        const std::string zip_contents((std::istreambuf_iterator<char>(zip)), std::istreambuf_iterator<char>());
        require(contains(zip_contents, optiscaler ? "CheekyFoveatedDLSS-OptiScaler.log" : "CheekyFoveatedDLSS-Standalone.log"), "Support ZIP includes this host's log");

        const auto old_attachment = attachment;
        detach(attachment);
        require(contains(snapshot(get), "\"attached\":false") && contains(snapshot(get), "\"processing\":false"), "Detach pauses resident processing");
        require(!command(old_attachment, "1\n6\nsave"), "Detached command rejected");
        auto wrong_host = input; wrong_host.host = CheekyRuntimeHost::uevr;
        require(!start(&wrong_host), "Resident host identity cannot change");
        require(start(&input) && attachment != old_attachment, "Same host can reattach with a fresh generation");
        tick(attachment, input.renderer, device, queue.Get());
        detach(old_attachment);
        tick(old_attachment, input.renderer, nullptr, nullptr);
        require(!command(old_attachment, "1\n7\nset\nEnabled=false"), "Old generation command rejected after reconnect");
        require(contains(snapshot(get), "\"attached\":true") && contains(snapshot(get), "\"processing\":true"), "Old generation cannot reset or detach new owner");
        detach(attachment);
        FreeLibrary(runtime);
        require(GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll") != nullptr, "Runtime remains resident after host unload");
        puts("PASS: generic runtime ABI, device discovery, settings, diagnostics, ownership and reconnect");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
