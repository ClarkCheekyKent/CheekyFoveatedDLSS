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
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
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
// A LibOVR game on Pimax: the runtime (fixture) forwards sessions and frames
// through an already loaded PVR client (fixture). Cheeky must observe both
// without initializing either, and read gaze from the game's own PVR session.
// Modes: 1 current SDK, 2 missed ovr_Initialize with pre-1.25 layers, 3 runtime
// returning borrowed swap-chain image references, 4 runtime using its own copy
// of the PVR interface table, 5 missed ovr_Initialize with 1.25+ layers.
void verify_libovr(unsigned mode, HMODULE libovr, HMODULE pvr, CheekyRuntimeSnapshotFn get,
    CheekyRuntimeCommandFn command, std::uint64_t attachment) {
    const bool initialized = mode != 2 && mode != 5;
    const auto state = [&] {
        const auto text = snapshot(get);
        const auto at = text.find("\"libovr\":");
        require(at != std::string::npos, "LibOVR adapter diagnostics in snapshot");
        return text.substr(at);
    };
    for (unsigned i = 0; i < 200 && !contains(state(), "\"client\":true"); ++i) Sleep(25);
    require(contains(state(), "\"hooked\":true") && contains(state(), "\"client\":true"),
        "Loaded LibOVR runtime and PVR client observed by their exports");
    require(contains(state(), "\"interface_minor\":32"), "Newest verified PVR interface selected");
    ComPtr<ID3D11Device> device;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, nullptr)), "Create WARP D3D11 device");
    using Initialize = int (*)(const void*);
    using Create = int (*)(void**, void*);
    using Destroy = void (*)(void*);
    using CreateChain = int (*)(void*, IUnknown*, const void*, void**);
    using DestroyChain = void (*)(void*, void*);
    using Commit = int (*)(void*, void*);
    using EndFrame = int (*)(void*, long long, const void*, const void* const*, unsigned);
    using References = unsigned long (*)(void*, int);
    if (mode == 3) proc<void(*)(bool)>(libovr, "CheekyFakeLibOVR_SetBorrowedBuffers")(true);
    if (initialized) {
        const std::uint32_t params[8]{4U /* ovrInit_RequestVersion */, 43U};
        require(proc<Initialize>(libovr, "ovr_Initialize")(params) == 0, "Initialize LibOVR");
    }
    void* session{};
    require(proc<Create>(libovr, "ovr_Create")(&session, nullptr) == 0 && session, "Create LibOVR session");
    const unsigned size[2]{512, 512};
    std::array<void*, 2> chains{};
    for (auto& chain : chains)
        require(proc<CreateChain>(libovr, "ovr_CreateTextureSwapChainDX")(session, device.Get(), size, &chain) == 0,
            "Create LibOVR D3D11 swap chain");
    // ovrLayerEyeFov: ColorTexture, Viewport, Fov after a 136-byte header
    // (SDK 1.25+) or an 8-byte header (older clients).
    alignas(8) std::array<unsigned char, 320> layer{};
    const std::size_t header = mode == 2 ? 8 : 136;
    const std::int32_t type = 1;
    std::memcpy(layer.data(), &type, sizeof(type));
    std::memcpy(layer.data() + header, chains.data(), sizeof(void*) * 2);
    const std::int32_t viewports[8]{0, 0, 512, 512, 0, 0, 512, 512};
    std::memcpy(layer.data() + header + 16, viewports, sizeof(viewports));
    const float fovs[8]{1, 1, 1, 1, 1, 1, 1, 1};
    std::memcpy(layer.data() + header + 48, fovs, sizeof(fovs));
    // RenderPose[2]: orientation xyzw, position xyz (identity rotations).
    const float poses[14]{0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
    std::memcpy(layer.data() + header + 80, poses, sizeof(poses));
    const void* layers[]{layer.data()};
    const auto commit = proc<Commit>(libovr, "ovr_CommitTextureSwapChain");
    const auto end_frame = proc<EndFrame>(libovr, mode == 1 ? "ovr_EndFrame" : "ovr_SubmitFrame2");
    const auto frames = [&](unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            for (auto* chain : chains) require(commit(session, chain) == 0, "Commit LibOVR swap chain");
            require(end_frame(session, i, nullptr, layers, 1) == 0, "Submit LibOVR frame");
            Sleep(2);
        }
    };
    frames(60);
    if (mode == 4) {
        // Table patches are never called: after ~1 s of frames, the adapter
        // observes the implementations instead.
        require(contains(state(), "\"session_captured\":false"), "Copied table hides the session from slot patches");
        frames(40);
        for (unsigned i = 0; i < 100 && !contains(state(), "\"session_route\":\"implementation\""); ++i) frames(5);
    }
    auto text = state();
    require(contains(text, mode == 4 ? "\"session_route\":\"implementation\"" : "\"session_route\":\"table\""),
        "Game PVR session found through the expected route");
    require(contains(text, "\"session\":true") && contains(text, "\"projection_layers\":1") &&
        contains(text, "\"submission_api\":11") && field(text, "projection_frames") >= 50,
        "Stereo projection and D3D11 swap-chain images observed");
    require(contains(text, mode == 2 ? "\"layout\":\"legacy\"" : "\"layout\":\"current\""),
        "Layer header layout follows the client's SDK version");
    require(contains(text, initialized ? "\"requested_minor\":43" : "\"requested_minor\":null"),
        "Requested LibOVR minor version recorded only when observed");
    require(proc<bool(*)()>(pvr, "CheekyFakePVR_EndFrameObserved")() &&
        proc<unsigned(*)()>(pvr, "CheekyFakePVR_EndFrames")() == proc<unsigned(*)()>(libovr, "CheekyFakeLibOVR_Frames")(),
        "Patched PVR frame slot forwards every call");
    require(contains(text, "\"submitted_eye_rotation\":true"), "Eye rotations come from submitted render poses");
    require(contains(text, "\"session_captured\":true") &&
        contains(text, "\"gaze_valid\":true") && contains(text, "\"gaze_tan\":[0.2,-0.1]") &&
        proc<unsigned(*)()>(pvr, "CheekyFakePVR_EyeQueries")() > 0,
        "Gaze read from the game's own PVR session");
    const auto full = snapshot(get);
    const auto calibration = full.substr(full.find("\"eye_calibration\":"));
    require(contains(calibration, "\"backend\":\"LibOVR\"") && field(calibration, "frames") >= 50,
        "LibOVR frames drive eye calibration");
    proc<void(*)(float, float, bool)>(pvr, "CheekyFakePVR_SetGaze")(0, 0, false);
    frames(5);
    require(contains(state(), "\"gaze_valid\":false"), "PVR samples without a timestamp are invalid");
    proc<void(*)(float, float, bool)>(pvr, "CheekyFakePVR_SetGaze")(0.2F, -0.1F, true);
    const auto set_clock = proc<void(*)(unsigned)>(pvr, "CheekyFakePVR_SetClockMode");
    set_clock(2); frames(5);
    require(contains(state(), "\"gaze_valid\":false"), "Non-finite PVR timestamps must not supply gaze");
    set_clock(0); frames(5);
    require(contains(state(), "\"gaze_valid\":true"), "Valid PVR timestamps recover gaze");
    set_clock(1); Sleep(220); frames(5);
    require(contains(state(), "\"gaze_valid\":false"), "Frozen PVR timestamps expire");
    set_clock(0); frames(5);
    require(contains(state(), "\"gaze_valid\":true"), "Advancing PVR timestamps recover after a stall");
    if (mode == 1) {
        using namespace cheeky::foveated_dlss;
        const auto ngx = GetModuleHandleW(L"nvngx_dlss.dll");
        using CreateFeature = NgxResult(*)(ID3D11DeviceContext*, unsigned, NgxParameters*, NgxHandle**);
        using Evaluate = NgxResult(*)(ID3D11DeviceContext*, const NgxHandle*, const NgxParameters*, NgxProgressCallback);
        const auto create_feature = proc<CreateFeature>(ngx, "NVSDK_NGX_D3D11_CreateFeature");
        const auto evaluate = proc<Evaluate>(ngx, "NVSDK_NGX_D3D11_EvaluateFeature");
        const auto get_index = proc<int(*)(void*, void*, int*)>(libovr, "ovr_GetTextureSwapChainCurrentIndex");
        const auto get_buffer = proc<int(*)(void*, void*, int, IID, void**)>(libovr, "ovr_GetTextureSwapChainBufferDX");
        ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context);
        MockNgxParameters parameters;
        parameters.Set("Width", 512U); parameters.Set("Height", 512U);
        parameters.Set("OutWidth", 512U); parameters.Set("OutHeight", 512U);
        parameters.Set("DLSS.Feature.Create.Flags", 2U); parameters.Set("PerfQualityValue", 2U);
        ComPtr<ID3D11Texture2D> source;
        const D3D11_TEXTURE2D_DESC desc{512,512,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,0,0,0};
        require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &source)), "AER source image");
        for (const auto* name : {"Color", "Depth", "MotionVectors"})
            parameters.Set(name, static_cast<ID3D11Resource*>(source.Get()));
        std::array<NgxHandle*,2> handles{};
        for (auto& handle : handles)
            require(ngx_succeeded(create_feature(context.Get(), 1, &parameters, &handle)), "Create AER eye feature through hooks");
        require(command(attachment, "1\n100\nset\nEnabled=false\nNrEnabled=false\nPeripheralDlaa=false\nEyeCalibrationMethod=3\nEyeCalibrationContinuous=false"), "Configure AER full search");
        const std::vector<unsigned> background(512 * 512, 0xff404040);
        unsigned frame_number{};
        const auto render_commit = [&](unsigned eye, bool fail = false, bool commit_now = true) {
            int index{};
            require(get_index(session, chains[eye], &index) == 0, "Get rotating AER image index");
            ComPtr<ID3D11Texture2D> image;
            require(get_buffer(session, chains[eye], index, __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(image.GetAddressOf())) == 0, "Get AER image");
            context->UpdateSubresource(image.Get(), 0, nullptr, background.data(), 512 * 4, 0);
            parameters.Set("Output", static_cast<ID3D11Resource*>(image.Get()));
            require(ngx_succeeded(evaluate(context.Get(), handles[eye], &parameters, nullptr)), "Stamp AER eye through NGX hooks");
            if (!commit_now) return;
            if (fail) proc<void(*)()>(libovr, "CheekyFakeLibOVR_FailNextCommit")();
            require((commit(session, chains[eye]) == 0) != fail, "Forward commit success or failure");
        };
        const auto submit = [&](bool fail = false) {
            if (fail) proc<void(*)()>(libovr, "CheekyFakeLibOVR_FailNextFrame")();
            require((end_frame(session, frame_number++, nullptr, layers, 1) == 0) != fail, "Forward frame success or failure");
            context->Flush(); Sleep(3);
        };
        const auto mapped = [&] { return contains(snapshot(get), "\"crop_mapping_active\":true"); };
        const auto stereo_frame = [&] {
            // Ordinary stereo renders both source eyes before releasing images.
            render_commit(0, false, false); render_commit(1, false, false);
            for (auto* chain : chains) require(commit(session, chain) == 0, "Commit complete stereo pair");
            submit();
        };
        const auto acquire = [&](bool alternating) {
            const auto deadline = GetTickCount64() + 10000;
            do {
                if (alternating) { render_commit(frame_number & 1U); submit(); }
                else stereo_frame();
            } while (!mapped() && GetTickCount64() < deadline);
            if (!mapped()) puts(snapshot(get).c_str());
            require(mapped(), "Actual LibOVR hooks acquire calibration with rotating images");
        };
        acquire(true);
        require(field(state(), "calibration_pair_deferrals") > 0, "Actual AER path pairs commits");
        const auto recalibrate = [&] {
            require(command(attachment, "1\n101\ncalibration_recalibrate"), "Recalibrate after AER transition");
        };
        recalibrate();
        acquire(false);
        const auto paired = field(state(), "calibration_pair_deferrals");
        for (unsigned i = 0; i < 8; ++i) stereo_frame();
        require(field(state(), "calibration_pair_deferrals") == paired, "Ordinary stereo never defers a complete pair");
        recalibrate();
        // Force an open pair, then lose the second commit and frame.
        auto before = field(state(), "calibration_pair_deferrals");
        for (unsigned i = 0; i < 300 && field(state(), "calibration_pair_deferrals") == before; ++i) {
            render_commit(frame_number & 1U); submit();
        }
        require(field(state(), "calibration_pair_deferrals") > before, "Open pair before failed commit");
        render_commit(frame_number & 1U, true); submit(true);
        require(!mapped(), "Failed submission cannot publish an incomplete pair");
        acquire(true);
        recalibrate();
        before = field(state(), "calibration_pair_deferrals");
        for (unsigned i = 0; i < 300 && field(state(), "calibration_pair_deferrals") == before; ++i) {
            render_commit(frame_number & 1U); submit();
        }
        require(field(state(), "calibration_pair_deferrals") > before, "Open pair before swap-chain recreation");
        parameters.values.erase("Output");
        for (auto& chain : chains) {
            proc<DestroyChain>(libovr, "ovr_DestroyTextureSwapChain")(session, chain);
            require(proc<CreateChain>(libovr, "ovr_CreateTextureSwapChainDX")(session, device.Get(), size, &chain) == 0,
                "Recreate AER swap chain while a pair is open");
        }
        std::memcpy(layer.data() + header, chains.data(), sizeof(void*) * 2);
        acquire(true);
        parameters.values.erase("Output");
        for (auto* handle : handles)
            require(ngx_succeeded(proc<NgxResult(*)(NgxHandle*)>(ngx, "NVSDK_NGX_D3D11_ReleaseFeature")(handle)), "Release AER feature");
        puts("PASS: LibOVR hook AER pairing, rotating images, mode switches, failures and swap-chain recreation");
    }
    const auto references = proc<References>(libovr, "CheekyFakeLibOVR_References");
    for (auto* chain : chains)
        for (int index = 0; index < 3; ++index)
            require(references(chain, index) <= 4, "Swap-chain image references stay bounded");
    proc<Destroy>(libovr, "ovr_Destroy")(session);
    for (auto* chain : chains)
        for (int index = 0; index < 3; ++index)
            require(references(chain, index) == 1, "Ending the session returns every swap-chain image reference");
    text = state();
    require(contains(text, "\"session\":false") && contains(text, "\"session_captured\":false") &&
        proc<unsigned(*)()>(pvr, "CheekyFakePVR_Destroyed")() == 1, "Session end forgets the game's PVR session");
    for (auto* chain : chains) proc<DestroyChain>(libovr, "ovr_DestroyTextureSwapChain")(session, chain);
    proc<void(*)()>(libovr, "ovr_Shutdown")();
}

void verify_transport(CheekyRuntimeCommandFn command, CheekyRuntimeSnapshotFn get,
    std::uint64_t attachment, ID3D11Device* device, ID3D11DeviceContext* context, HMODULE ngx,
    const std::filesystem::path& log_path,bool depth24=false,bool backpressure=false,bool init_failure=false, const std::filesystem::path& game_feature_directory={}, bool release_drain=false) {
    using namespace cheeky::foveated_dlss;
    using Init = NgxResult (*)(unsigned long long, const wchar_t*, ID3D11Device*, const void*, unsigned);
    using Create = NgxResult (*)(ID3D11DeviceContext*, unsigned, NgxParameters*, NgxHandle**);
    using Evaluate = NgxResult (*)(ID3D11DeviceContext*, const NgxHandle*, const NgxParameters*, NgxProgressCallback);
    using Release = NgxResult (*)(NgxHandle*);
    for (unsigned i = 0; i < 200 && !contains(snapshot(get), "\"direct_detour\":true"); ++i) Sleep(25);
    require(contains(snapshot(get), "\"direct_detour\":true"), "NGX detours installed for transport fixture");
    // With only the cached snippet loaded, supply the game's explicit search
    // path: this fixture's game DLL lives outside the test EXE directory.
    const auto feature_directory = game_feature_directory.wstring();
    const wchar_t* feature_path = feature_directory.c_str();
    NgxFeatureCommonInfo feature_info{};
    feature_info.path_list.paths = &feature_path;
    feature_info.path_list.count = 1;
    require(ngx_succeeded(proc<Init>(ngx, "NVSDK_NGX_D3D11_Init")(42, L".", device,
        feature_directory.empty() ? nullptr : &feature_info, 1)), "Record DX11 initialization for private transport");
    // Standalone/ASI runtime DLLs are nested away from the game's DLSS DLL.
    // The core must receive an explicit feature path when Init was recovered
    // or the game's Init supplied no FeatureCommonInfo.
    const auto core_runtime = GetModuleHandleW(L"_nvngx.dll");
    proc<void(*)(bool)>(core_runtime, "CheekyFakeRequireFeaturePath")(true);
    // Real SR rejects direct Init_Ext callers outside core NGX. The synthetic
    // core does not forward Init, so this rejects the incorrect direct route.
    proc<void(*)(bool)>(ngx, "CheekyFakeFailInitialization")(true);
    if (init_failure) proc<void(*)(bool)>(core_runtime, "CheekyFakeFailInitialization")(true);
    // A game/VR core hook may reject private-device evaluations. Transport
    // must use the snippet lifecycle even when all core callbacks exist.
    proc<void(*)(bool)>(core_runtime, "CheekyFakeFailEvaluations")(true);
    MockNgxParameters parameters;
    parameters.Set("Width", 128U); parameters.Set("Height", 128U);
    parameters.Set("OutWidth", 256U); parameters.Set("OutHeight", 256U);
    parameters.Set("DLSS.Feature.Create.Flags", 2U); parameters.Set("PerfQualityValue", 2U);
    parameters.Set("MV.Scale.X", 1.F); parameters.Set("MV.Scale.Y", 1.F);
    if (init_failure) {
        // Native DX11 fallback restores absent optional fields as zero. Seed
        // their defaults so exact map equality also checks their restoration.
        for (const char* key : {"DLSS.Render.Subrect.Dimensions.Width", "DLSS.Render.Subrect.Dimensions.Height",
                "DLSS.Input.Color.Subrect.Base.X", "DLSS.Input.Color.Subrect.Base.Y",
                "DLSS.Input.Depth.Subrect.Base.X", "DLSS.Input.Depth.Subrect.Base.Y",
                "DLSS.Input.MV.Subrect.Base.X", "DLSS.Input.MV.Subrect.Base.Y",
                "DLSS.Output.Subrect.Base.X", "DLSS.Output.Subrect.Base.Y"}) parameters.Set(key, 0U);
        parameters.Set("DLSS.Enable.Output.Subrects", 0);
        parameters.Set("Reset", 0);
    }
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
    if (init_failure) {
        const auto attempts = proc<unsigned(*)()>(core_runtime, "CheekyFakeInitializations");
        require(ngx_succeeded(evaluate(context, handle, &parameters, nullptr)), "Failed initialization falls back to DX11");
        require(parameters.values == original_parameters, "Initial fallback preserves game parameters");
        require(attempts() == 1, "Exercise a real private initialization failure");
        const auto began = GetTickCount64();
        for (unsigned i = 0; i < 20; ++i) {
            require(ngx_succeeded(evaluate(context, handle, &parameters, nullptr)), "Native frames continue during retry cooldown");
            require(parameters.values == original_parameters, "Failed initialization preserves game parameters");
        }
        require(GetTickCount64() - began < 5000 && attempts() == 1,
            "Failed initialization is not repeated on each frame");
        proc<void(*)(bool)>(core_runtime, "CheekyFakeFailInitialization")(false);
        Sleep(5100);
    }
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
    const auto verify_extent = [&] {
        ComPtr<ID3D11UnorderedAccessView> view;
        require(SUCCEEDED(device->CreateUnorderedAccessView(textures[3].Get(), nullptr, &view)), "Extent sentinel view");
        const float sentinel[]{.25F,.25F,.25F,.25F};
        context->ClearUnorderedAccessViewFloat(view.Get(), sentinel);
        parameters.Set("OutWidth", 96U); parameters.Set("OutHeight", 96U);
        require(ngx_succeeded(evaluate(context, handle, &parameters, nullptr)), "Transport with reused query extent");
        require(contains(snapshot(get), "\"execution_path\":\"DX12 Transport\""), "Extent regression actually uses transport");
        require(get_ui(&parameters, "OutWidth") == 96 && get_ui(&parameters, "OutHeight") == 96,
            "Transport restores reused query dimensions");
        D3D11_TEXTURE2D_DESC desc{}; textures[3]->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "Transport extent staging");
        context->CopyResource(staging.Get(), textures[3].Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Read transport extent output");
        unsigned stale{};
        for (unsigned y = 0; y < desc.Height; ++y) for (unsigned x = 0; x < desc.Width; ++x) {
            const auto* rgba = reinterpret_cast<const unsigned short*>(static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch + x * 8);
            stale += rgba[0] == 0x3400 && rgba[1] == 0x3400 && rgba[2] == 0x3400 && rgba[3] == 0x3400;
        }
        context->Unmap(staging.Get(), 0);
        parameters.values = original_parameters;
        require(stale == 0, "Transport composite covers created output after reused query");
    };
    verify_extent();
    require(proc<unsigned(*)()>(core_runtime, "CheekyFakeInitializations")() == (init_failure ? 2U : 1U) &&
        proc<unsigned(*)()>(ngx, "CheekyFakeInitializations")() == 0,
        "Transport initializes through core and recovers after a deferred retry");
    require(proc<unsigned(*)()>(core_runtime, "CheekyFakeCreates")() == 0 &&
        proc<unsigned(*)()>(core_runtime, "CheekyFakeEvaluates")() == 0,
        "Private transport SR must bypass the game's core feature hooks");
    if (backpressure || release_drain) {
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
            if (release_drain) {
                require(SUCCEEDED(context4->Wait(gate11.Get(), order + 1)), "Hold work during feature release");
                // The gate is always released, even if evaluation/assertions fail.
                // Start the delay at ReleaseFeature so a slow evaluation cannot
                // consume the two-second regression window.
                std::atomic<bool> releasing{};
                std::jthread release_gate([gate12, order, &releasing](std::stop_token stop) {
                    while (!releasing.load() && !stop.stop_requested()) Sleep(1);
                    if (!stop.stop_requested()) Sleep(2300);
                    gate12->Signal(order + 1);
                });
                parameters.values = original_parameters;
                require(ngx_succeeded(evaluate(context, handle, &parameters, nullptr)), "Queue work before feature release");
                // Do not Flush here: release must flush DX11's buffered signals.
                const auto start = GetTickCount64();
                releasing = true;
                const auto result = release(handle);
                const auto elapsed = GetTickCount64() - start;
                const bool gate_completed = gate12->GetCompletedValue() >= order + 1;
                release_gate.join();
                require(ngx_succeeded(result), "Release transported feature after draining");
                require(gate_completed && elapsed >= 2000,
                    "Release must retain GPU resources beyond the old two-second timeout");
                if (order == 0) {
                    parameters.values = original_parameters;
                    require(ngx_succeeded(proc<Create>(ngx, "NVSDK_NGX_D3D11_CreateFeature")(
                        context, 1, &parameters, &handle)), "Recreate feature after draining Before NR");
                }
                continue;
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
        if (release_drain) {
            puts("PASS feature release drains Before/After NR beyond two seconds with buffered DX11 signals");
            return;
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
    // BG3 also crashes with NR disabled: the first private peripheral SR
    // evaluation enters the game's core DX12 hook. Exercise both SR features
    // while the fake core evaluator remains deliberately unusable.
    require(command(attachment, "1\n29\nset\nEnabled=true\nPeripheralDlaa=true\nNrEnabled=false"),
        "Enable peripheral DLAA without NR");
    const auto sr_evaluations = proc<unsigned(*)()>(ngx, "CheekyFakeEvaluates")();
    frames(false);
    verify_extent();
    require(contains(snapshot(get), "\"execution_path\":\"DX12 Transport\"") &&
        proc<unsigned(*)()>(ngx, "CheekyFakeEvaluates")() >= sr_evaluations + 12,
        "Center SR and peripheral DLAA both evaluate through the snippet without NR");
    require(proc<unsigned(*)()>(core_runtime, "CheekyFakeCreates")() == 0 &&
        proc<unsigned(*)()>(core_runtime, "CheekyFakeEvaluates")() == 0,
        "Peripheral DLAA must bypass the game's core feature hooks");
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
        bool ota_transport{}, ota_only{}, calibration_disabled{};
        bool uevr{}, dx11{}, conflict{}, optiscaler{}, transport{}, forwarded_transport{},depth24{},backpressure{},init_failure{},release_drain{};
        unsigned openvr_version{}, libovr_mode{};
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--dx11") dx11 = true;
            else if (arg == "--calibration-disabled") calibration_disabled = true;
            else if (arg == "--libovr") libovr_mode = 1;
            else if (arg == "--libovr-legacy") libovr_mode = 2;
            else if (arg == "--libovr-borrowed") libovr_mode = 3;
            else if (arg == "--libovr-copied-table") libovr_mode = 4;
            else if (arg == "--libovr-late") libovr_mode = 5;
            else if (arg == "--conflict") conflict = true;
            else if (arg == "--uevr") uevr = true;
            else if (arg == "--optiscaler") optiscaler = true;
            else if (arg == "--transport-ota-only") { ota_only = ota_transport = transport = dx11 = true; }
            else if (arg == "--transport-ota") { ota_transport = transport = dx11 = true; }
            else if (arg == "--transport") { transport = true; dx11 = true; }
            else if (arg == "--transport-init-failure") { transport = dx11 = init_failure = true; }
            else if (arg == "--transport-depth24") { transport = dx11 = depth24 = true; }
            else if (arg == "--transport-release-drain") { transport = dx11 = release_drain = true; }
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
        HMODULE fake_pvr{}, fake_libovr{};
        if (libovr_mode) {
            // The game's runtime loaded both before Cheeky; Cheeky loads neither.
            fake_pvr = LoadLibraryW((bin / "test-fixtures" / "LibPVRClient64.dll").c_str());
            fake_libovr = LoadLibraryW((bin / "test-fixtures" / "LibOVRRT64_1.dll").c_str());
            require(fake_pvr && fake_libovr, "Load LibOVR runtime and PVR client fixtures");
            if (libovr_mode == 4) proc<void(*)()>(fake_libovr, "CheekyFakeLibOVR_CopyPvrTable")();
        }
        HANDLE other_owner{};
        if (conflict) {
            other_owner = cheeky::foveated_dlss::claim_processing_owner();
            require(other_owner != nullptr, "Reserve another integration's processing ownership");
        }
        auto runtime_path = bin / "CheekyFoveatedDLSS" / "CheekyFoveatedDLSSRuntime.dll";
        HMODULE fake_ngx{};
        if (libovr_mode == 1) {
            fake_ngx = LoadLibraryW((bin / "test-fixtures/nvngx_dlss.dll").c_str());
            require(fake_ngx != nullptr, "Load NGX fixture for actual LibOVR calibration hooks");
        }
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
            auto sr_path = isolated / "nvngx_dlss.dll";
            if (ota_only) {
                sr_path = directory / "NVIDIA/NGX/models/dlss/versions/20318464/files/160_E658700.bin";
                std::filesystem::create_directories(sr_path.parent_path());
                std::filesystem::copy_file(fixture, sr_path);
            }
            fake_ngx = LoadLibraryW(sr_path.c_str());
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
        input.host = uevr ? CheekyRuntimeHost::uevr : optiscaler ? CheekyRuntimeHost::optiscaler : CheekyRuntimeHost::standalone;
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
        if (uevr) {
            std::ofstream saved(directory / "CheekyFoveatedDLSS.ini");
            saved << "[CheekyFoveatedDLSS]\nSchemaVersion=1\nD3D11D3D12Transport=true\n";
        }
        if (calibration_disabled) {
            std::ofstream saved(directory / "CheekyRuntime.ini");
            saved << "[Calibration]\nEnabled=0\n";
        }
        require(start(&input) && attachment != 0, "Start before graphics discovery");
        {
            const auto full = snapshot(get);
            const auto pos = full.find("\"eye_calibration\":");
            require(pos != full.npos, "Calibration snapshot exists");
            const auto calibration = full.substr(pos);
            require(contains(calibration, calibration_disabled ? "\"enabled\":false" : "\"enabled\":true"),
                "Calibration startup preference honored");
        }
        if (uevr) require(contains(snapshot(get), "\"D3D11D3D12Transport\":true"), "UEVR loads saved transport preference");
        if (libovr_mode) {
            verify_libovr(libovr_mode, fake_libovr, fake_pvr, get, command, attachment);
            detach(attachment);
            puts(libovr_mode == 1 ? "PASS: LibOVR frames and Pimax PVR gaze observed without OpenXR" :
                libovr_mode == 2 ? "PASS: LibOVR legacy layer header detected after a missed ovr_Initialize" :
                libovr_mode == 3 ? "PASS: LibOVR borrowed swap-chain references are never released" :
                libovr_mode == 4 ? "PASS: PVR session found through implementations when the table is copied" :
                "PASS: LibOVR current layer header detected after a missed ovr_Initialize");
            return 0;
        }
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
        require(contains(text, uevr ? "\"host\":\"uevr\"" : optiscaler ? "\"host\":\"optiscaler\"" : "\"host\":\"standalone\""), "Snapshot identifies the actual host");
        require(contains(text, uevr ? "\"host_supports_afw_projection\":true" : "\"host_supports_afw_projection\":false"), "Projection capability matches host");
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
        if (ota_transport && !ota_only) {
            // The named DLL is loaded and selected first, but override frames
            // arrive through a subsequently loaded cached snippet.
            proc<void(*)(bool)>(fake_ngx, "CheekyFakeFailEvaluations")(true);
            const auto ota_path = directory / "NVIDIA/NGX/models/dlss/versions/20318464/files/160_E658700.bin";
            std::filesystem::create_directories(ota_path.parent_path());
            std::filesystem::copy_file(bin / "test-fixtures/nvngx_dlss.dll", ota_path);
            fake_ngx = LoadLibraryW(ota_path.c_str()); require(fake_ngx != nullptr, "Load late transport OTA runtime");
            bool hooked{};
            for (unsigned i = 0; i < 400 && !hooked; ++i) {
                std::ifstream log(directory / "CheekyFoveatedDLSS-Standalone.log");
                const std::string value((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
                const auto found = value.find("DX11 OTA runtime discovered");
                hooked = found != value.npos && value.find("Direct detour installed export=NVSDK_NGX_D3D11_ReleaseFeature", found) != value.npos;
                if (!hooked) Sleep(25);
            }
            require(hooked, "OTA hooks ready before transport evaluation");
            require(contains(snapshot(get), "Cached runtime loaded; use not observed"), "Loading alone does not claim override active");
        }
        if (transport) verify_transport(command, get, attachment, device11.Get(), context11.Get(), fake_ngx,
            directory / (uevr ? "CheekyFoveatedDLSS-UEVR.log" : optiscaler ? "CheekyFoveatedDLSS-OptiScaler.log" : "CheekyFoveatedDLSS-Standalone.log"),depth24,backpressure,init_failure, ota_only ? directory / "runtime" : std::filesystem::path{}, release_drain);
        if (ota_transport) {
            require(contains(snapshot(get), "Active (NVIDIA cached runtime)"), "Override status follows actual OTA evaluations");
            detach(attachment);
            puts("PASS: late NVIDIA OTA transport selects matching DX12 callbacks and executes SR/NR");
            return 0;
        }
        if (uevr) {
            require(transport, "UEVR runtime fixture exercises transport");
            tick(attachment, input.renderer, nullptr, nullptr);
            tick(attachment, input.renderer, device, nullptr);
            require(contains(snapshot(get), "\"D3D11D3D12Transport\":true"), "UEVR reset preserves transport");
            detach(attachment);
            require(start(&input), "Reconnect UEVR host");
            tick(attachment, input.renderer, device, nullptr);
            require(contains(snapshot(get), "\"D3D11D3D12Transport\":true"), "UEVR reconnect preserves transport");
            detach(attachment);
            puts("PASS: UEVR DX11 transport and NR, device reset and reconnect");
            return 0;
        }
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
        require(contains(zip_contents, "stereo-capture.json") && contains(zip_contents, "\"stereo_capture\":"),
            "Support ZIP must include capture diagnostics even when VR is unavailable");
        require(contains(zip_contents, optiscaler ? "CheekyFoveatedDLSS-OptiScaler.log" : "CheekyFoveatedDLSS-Standalone.log"), "Support ZIP includes this host's log");

        require(command(attachment, "1\n6\nset\nCenterMode=3"), "Shared-gaze menu command accepted");
        require(field(snapshot(get), "CenterMode") == 3, "Runtime preserves shared-gaze enum instead of clamping it");
        require(!command(attachment, "1\n7\nset\nCenterMode=4"), "Unknown center mode rejected");
        require(command(attachment, "1\n8\nsave"), "Save shared-gaze settings");
        require(contains(snapshot(get), "\"shared_gaze\":") && contains(snapshot(get), "\"shared_projection\":"),
            "Support diagnostics expose shared mode separately from eye mapping");
        const auto old_attachment = attachment;
        detach(attachment);
        require(contains(snapshot(get), "\"attached\":false") && contains(snapshot(get), "\"processing\":false"), "Detach pauses resident processing");
        require(!command(old_attachment, "1\n6\nsave"), "Detached command rejected");
        auto wrong_host = input; wrong_host.host = CheekyRuntimeHost::uevr;
        require(!start(&wrong_host), "Resident host identity cannot change");
        require(start(&input) && attachment != old_attachment, "Same host can reattach with a fresh generation");
        require(field(snapshot(get), "CenterMode") == 3, "Shared gaze survives settings save and reconnect");
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
