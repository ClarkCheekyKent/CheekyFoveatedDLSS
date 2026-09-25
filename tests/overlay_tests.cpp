#include "overlay.hpp"
#include "overlay_ui.hpp"
#include "settings_io.hpp"
#include "../shared/openxr_menu_shared11.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace cheeky::standalone;
using namespace cheeky::foveated_dlss;
// GPU test runners can use a non-interactive window station with no global
// foreground HWND. Production retains GetForegroundWindow; only this binary
// supplies an explicit foreground state for reproducible input tests.
HWND test_foreground{};
bool test_f8_down{};
int test_extra_key{};
bool test_extra_down{};
bool test_mouse_down[5]{};
bool cheeky_overlay_test_mouse_down(unsigned button) { return test_mouse_down[button]; }
bool cheeky_overlay_test_key_down(int key) { return key == VK_F8 ? test_f8_down : key == test_extra_key && test_extra_down; }
bool cheeky_overlay_test_foreground(HWND window) { return window && window == test_foreground; }
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void check(HRESULT value, const char* message) { require(SUCCEEDED(value), message); }

void test_shared_menu11() {
    ComPtr<ID3D11Device> producer, consumer;
    ComPtr<ID3D11DeviceContext> write, read;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &producer, nullptr, &write), "Producer device");
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &consumer, nullptr, &read), "Consumer device");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 16;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> source, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    check(producer->CreateTexture2D(&desc, nullptr, &source), "Source texture");
    check(producer->CreateRenderTargetView(source.Get(), nullptr, &rtv), "Source RTV");
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(consumer->CreateTexture2D(&desc, nullptr, &staging), "Consumer readback");
    cheeky::xr_menu::SharedTexture11 shared;
    check(shared.initialize(producer.Get(), consumer.Get(), source.Get()), "Cross-device sharing setup");
    for (unsigned pass = 0; pass < 2; ++pass) {
        const float color[]{float(pass), float(1 - pass), 0, 1};
        write->ClearRenderTargetView(rtv.Get(), color);
        HRESULT hr = S_FALSE;
        for (unsigned retry = 0; retry < 100 && hr == S_FALSE; ++retry) {
            hr = shared.publish(write.Get(), source.Get());
            if (hr == S_FALSE) Sleep(1);
        }
        require(hr == S_OK, "Shared frame publication");
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<IDXGIKeyedMutex> mutex;
        hr = S_FALSE;
        for (unsigned retry = 0; retry < 100 && hr == S_FALSE; ++retry) {
            hr = shared.acquire(&texture, &mutex);
            if (hr == S_FALSE) Sleep(1);
        }
        require(hr == S_OK, "Shared frame acquisition");
        require(shared.publish(write.Get(), source.Get()) == S_FALSE, "Producer must wait for consumer ownership");
        read->CopyResource(staging.Get(), texture.Get());
        check(mutex->ReleaseSync(0), "Return shared frame ownership");
        read->Flush();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(read->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read shared pixels");
        const auto pixel = *static_cast<const unsigned*>(mapped.pData);
        read->Unmap(staging.Get(), 0);
        require(pixel == (pass ? 0xFF0000FFU : 0xFF00FF00U), "Cross-device menu pixel mismatch");
    }
    std::puts("PASS: D3D11 menu sharing between two devices, pixels and ownership");
}

// Exercise the real tab renderer with snapshots, including nested API/eye
// objects. This catches incorrect JSON-member selection and misleading status
// warnings without depending on a headset or a particular GPU timing sample.
void test_ui_diagnostics() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    auto* context = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 900);
    unsigned char* pixels{}; int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    OverlayUiState state;
    InputState input;
    state.draft.center_mode = FoveationCenterMode::openxr_gaze;
    const OverlayRuntime runtime{};
    auto render = [&](const char* tab, bool expand_details = true) {
        if (context->TabBars.GetAliveCount()) {
            auto* bar = context->TabBars.GetByIndex(0);
            for (auto& item : bar->Tabs)
                if (std::strcmp(ImGui::TabBarGetTabName(bar, &item), tab) == 0) ImGui::TabBarQueueFocus(bar, &item);
        }
        bool open = true;
        ImGui::NewFrame();
        ImGui::Begin("Diagnostic capture");
        ImGui::LogToBuffer(expand_details ? 8 : 0);
        draw_overlay_ui(state, runtime, input, "D3D12", "Ready", open);
        const std::string text = context->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return text;
    };
    state.snapshot = R"({"gaze":{"layer":false,"views":2,"alignment":0}})";
    render("Stereo / Gaze"); render("Stereo / Gaze");
    auto text = render("Stereo / Gaze");
    auto* bar = context->TabBars.GetByIndex(0);
    require(std::strcmp(ImGui::TabBarGetTabName(bar, &bar->Tabs[0]), "General") == 0, "General should be the first tab");
    require(bar->Tabs.Size == 6, "General should join the existing five tabs");
    require(text.find("no active OpenXR layer") != text.npos, "Missing gaze adapter warning");
    require(text.find("manual fallback placement") != text.npos, "Missing stereo fallback warning");
    state.draft.center_mode = FoveationCenterMode::fixed;
    state.snapshot = R"({"gaze":{"layer":false,"views":1,"alignment":0}})";
    require(render("Stereo / Gaze").find("manual fallback placement") == std::string::npos, "Flat games do not require stereo alignment");
    state.draft.center_mode = FoveationCenterMode::openxr_gaze;
    state.snapshot = R"({"gaze":{"layer":false,"views":2,"alignment":1}})";
    text = render("Stereo / Gaze");
    require(text.find("manual fallback placement") == text.npos, "Streamline alignment does not need eye tracking");
    state.snapshot = R"({"gaze":{"layer":true,"abi":false,"views":2,"alignment":0}})";
    require(render("Stereo / Gaze").find("update the OpenXR layer") != std::string::npos, "ABI mismatch warning");
    state.snapshot = R"({"gaze":{"layer":true,"abi":true,"views":2,"alignment":2,"status_flags":12}})";
    require(render("Stereo / Gaze").find("No valid eye-tracking signal") != std::string::npos, "Invalid gaze signal warning");
    state.snapshot = R"({"gaze":{"layer":true,"abi":true,"views":2,"alignment":2,"status_flags":108,"using_gaze":false,"ambiguous":true}})";
    require(render("Stereo / Gaze").find("Eye mapping is ambiguous") != std::string::npos, "Ambiguous eye mapping warning");
    state.snapshot = R"({"ready":true,"d3d12_lower_hook_active":true,"d3d12_hook_restart_required":true,
        "gaze":{"layer":true,"abi":true,"views":2,"alignment":3,"status_flags":108,"using_gaze":true},
        "frame":{"present_ms":10,"sr_enabled_ms":10,"sr_disabled_ms":12},
        "apis":[{"evaluations":0},{"evaluations":10,"state":"Active","native_ms":2,"foveated_ms":1.25,"nr_full_ms":3,
            "motion_width":2000,"motion_height":1600,"motion_space":"Output-resolution",
            "crop":{"input_width":500,"input_height":400},"input_width":1000,"input_height":800}],
        "nr_details":{"result":1,"output_width":2000,"output_height":1600,"processing_width":2000,"processing_height":1600,
            "region_width":1000,"region_height":800,"region_x":100,"region_y":200},
        "eye_calibration":{"enabled":true,"backend":"OpenVR","graphics_api":12,"status":"Ready, {mapped}","active_method":"Full crop search",
            "corrections":7,"applied":9,"gpu_samples":2,"gpu_us":12.5,"left_view":"18446744073709551614","right_view":"42"}})";
    text = render("Stereo / Gaze");
    require(text.find("fixed fallback") == text.npos && text.find("manual fallback placement") == text.npos, "Healthy tracking reported as fallback");
    require(text.find("Eye Tracking Ready: Yes") != text.npos, "Visible eye tracking readiness missing");
    require(text.find("Automatic eye calibration (this session)") != text.npos &&
        text.find("Recalibrate now") != text.npos && text.find("Recalibration") != text.npos,
        "Calibration controls belong in Stereo / Gaze");
    require(text.find("Eye tracking details") == text.npos && text.find("Corrections applied") == text.npos, "Tracking details belong in Diagnostics");
    render("General"); text = render("General");
    require(text.find("Change menu key") != text.npos && text.find("DX11 -> DX12 transport") != text.npos &&
        text.find("Use lower DLSS hook (DX12)") != text.npos && text.find("Runtime ready") != text.npos &&
        text.find("DLSS hook change saved") != text.npos, "General controls and statuses missing");
    render("DLSS-SR"); text = render("DLSS-SR");
    require(text.find("Use lower DLSS hook (DX12)") == text.npos, "Hook selector should live in General");
    require(text.find("Cosmetic only; no performance impact.") != text.npos, "SR roundness note missing");
    require(text.find("Full DLSS call: 2.000 ms") != text.npos, "GPU timing from the second API object");
    require(text.find("Foveated FPS gain: +16.7 FPS (+20.0%)") != text.npos, "FPS gain must use the non-foveated FPS baseline");
    require(text.find("Frame-time change: -2.00 ms (-16.7%)") != text.npos, "Frame-time change must use the non-foveated frame time baseline");
    require(text.find("Foveated savings: 0.750 ms (37.5%)") != text.npos, "GPU savings must remain separate from FPS gain");
    require(text.find("DLSS input: 1000 x 800") != text.npos, "Nested crop dimensions leaked into input dimensions");
    require(text.find("Center input: 500 x 400 (25.0% of original) at 0,0") != text.npos, "Crop pixel percentage and origin missing");
    require(text.find("Peripheral DLAA: Enabled (auto MV conversion)") != text.npos, "Motion-vector compatibility status missing");
    require(text.find("Full DLSS-NR call") == text.npos, "NR timings should not appear in SR");
    render("DLSS-NR"); text = render("DLSS-NR");
    require(text.find("Cosmetic only; no performance impact.") != text.npos, "NR roundness note missing");
    require(text.find("Full DLSS-NR call: 3.000 ms") != text.npos, "NR timings belong in NR");
    require(text.find("DLSS-NR region: 1000 x 800 (25.0% of original) at 100,200") != text.npos, "NR region comparison missing");
    require(text.find("Last NGX result: 0x00000001") != text.npos, "NR result missing");
    render("Diagnostics", false); text = render("Diagnostics", false);
    require(text.find("Eye tracking details") != text.npos && text.find("Eye calibration diagnostics") != text.npos, "Diagnostic groups missing");
    require(text.find("Corrections applied") == text.npos && text.find("Sample age") == text.npos, "Detailed diagnostics should start collapsed");
    render("Diagnostics"); text = render("Diagnostics");
    require(text.find("Corrections applied: 7") != text.npos, "Calibration counters missing");
    require(text.find("Automatic eye calibration (this session)") == text.npos &&
        text.find("Recalibrate now") == text.npos, "Diagnostics must not contain calibration controls");
    require(text.find("Ready, {mapped}") != text.npos, "Quoted diagnostic braces parsed as structure");
    require(text.find("18446744073709551614") != text.npos, "Calibration view identity lost integer precision");
    // Cover slower/equal modes and incomplete baselines without inventing a
    // percentage or confusing percentage FPS gain with frame-time savings.
    for (const auto& sample : {std::pair{
            R"({"frame":{"sr_disabled_ms":10,"sr_enabled_ms":12}})",
            "Foveated FPS gain: -16.7 FPS (-16.7%)"},
            {R"({"frame":{"sr_disabled_ms":10,"sr_enabled_ms":10}})", "Foveated FPS gain: +0.0 FPS (+0.0%)"},
            {R"({"frame":{"sr_disabled_ms":0,"sr_enabled_ms":10}})", "Foveated FPS gain: Not sampled yet"},
            {R"({"frame":{"sr_disabled_ms":10,"sr_enabled_ms":0}})", "Foveated FPS gain: Not sampled yet"},
            {R"({"frame":{}})", "Foveated FPS gain: Not sampled yet"}}) {
        state.snapshot = sample.first;
        render("DLSS-SR"); text = render("DLSS-SR");
        require(text.find(sample.second) != text.npos, "Incorrect SR on/off comparison for slower, equal or unsampled frames");
    }
    state.snapshot = R"({"apis":[{}, {"reconstruction_feature":13}]})";
    state.draft.rr_center_preset = 6; state.draft.rr_peripheral_preset = 4;
    state.draft.peripheral_dlaa_enabled = true;
    render("DLSS-SR"); text = render("DLSS-SR");
    require(text.find("Center RR preset") != text.npos && text.find("Peripheral RR preset") != text.npos,
        "Standalone preset menus did not switch to RR");
    state.snapshot = R"({"apis":[{}, {"reconstruction_feature":1}]})";
    render("DLSS-SR"); text = render("DLSS-SR");
    require(text.find("Center RR preset") == text.npos && text.find("Center preset") != text.npos,
        "Standalone preset menus did not return to SR");
    state.draft.enabled = false;
    state.draft.nr_enabled = false;
    state.snapshot = R"({"gaze":{"layer":false,"views":2,"alignment":0}})";
    require(render("Stereo / Gaze").find("no active OpenXR layer") == std::string::npos, "Inactive features should not report tracking failure");
    state.draft.nr_enabled = true;
    require(render("Stereo / Gaze").find("no active OpenXR layer") != std::string::npos, "NR-only foveation still requires tracking warnings");
    // Physical polling must not release a VR trigger between controller events.
    input.window = reinterpret_cast<HWND>(1);
    input.enabled = true; input.open = true;
    test_foreground = input.window;
    io.AddMouseButtonEvent(0, true);
    ImGui::NewFrame(); ImGui::EndFrame();
    require(io.MouseDown[0], "Controller press was not applied");
    input.messages.push_back({WM_LBUTTONUP, 0, 0});
    process_overlay_input(input, 1U);
    ImGui::NewFrame(); ImGui::EndFrame();
    require(io.MouseDown[0], "Desktop input interrupted the controller drag");
    // A pending VR motion must survive, while a new Win32 fallback cursor and
    // mouse-up must not move/release the drag. Keyboard input still gets through.
    io.AddMousePosEvent(120, 100);
    const int desktop_begin = context->InputEventsQueue.Size;
    io.AddMousePosEvent(700, 700);
    io.AddMouseButtonEvent(0, false);
    io.AddKeyEvent(ImGuiKey_F2, true);
    discard_desktop_pointer_events(desktop_begin);
    ImGui::NewFrame(); ImGui::EndFrame();
    require(io.MouseDown[0] && io.MousePos.x == 120 && io.MousePos.y == 100,
        "Desktop fallback cursor displaced the controller drag");
    require(ImGui::IsKeyDown(ImGuiKey_F2), "Controller pointer arbitration swallowed keyboard input");
    io.AddMouseButtonEvent(0, false);
    ImGui::NewFrame(); ImGui::EndFrame();
    require(!io.MouseDown[0], "Controller release was not applied");
    test_foreground = nullptr;
    ImGui::DestroyContext(context);
    std::puts("PASS: overlay snapshot parsing, GPU diagnostics, calibration and gaze/alignment warnings");
}
unsigned snapshots{}, game_keys{}, game_button_down{}, game_button_up{};
unsigned game_pointer_messages{};
bool pointer_checkbox{};
ImVec2 pointer_checkbox_position{};
void pointer_checkbox_hook(ImGuiContext*, ImGuiContextHook*) {
    ImGui::SetNextWindowPos(ImVec2(650, 40));
    ImGui::SetNextWindowSize(ImVec2(140, 90));
    ImGui::Begin("Pointer fixture", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    ImGui::Checkbox("Toggle", &pointer_checkbox);
    const auto p = ImGui::GetItemRectMin();
    pointer_checkbox_position = ImVec2(p.x + 6, p.y + 6);
    ImGui::End();
}
Settings settings;
bool snapshot(char* output, std::uint32_t capacity) {
    ++snapshots;
    const auto text = "{\"message\":\"Overlay regression fixture\",\"settings\":" + settings_json(settings) + "}";
    return strcpy_s(output, capacity, text.c_str()) == 0;
}
bool command(std::uint64_t attachment, const char*) { return attachment == 1; }
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_POINTERDOWN || message == WM_POINTERUP || message == WM_POINTERUPDATE) ++game_pointer_messages;
    if (message == WM_KEYDOWN || message == WM_KEYUP) ++game_keys;
    if (message == WM_LBUTTONDOWN) ++game_button_down;
    if (message == WM_LBUTTONUP) ++game_button_up;
    return DefWindowProcW(window, message, wparam, lparam);
}
struct Window {
    HWND value{};
    RECT original_clip{};
    bool have_clip{};
    Window() {
        WNDCLASSW type{}; type.lpfnWndProc = window_proc; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"CheekyOverlayTest";
        RegisterClassW(&type);
        value = CreateWindowExW(0, type.lpszClassName, L"Cheeky overlay GPU regression", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 820, 760, nullptr, nullptr, type.hInstance, nullptr);
        require(value != nullptr, "CreateWindow failed");
        ShowWindow(value, SW_SHOW);
        // The runner may supply STARTF_USESHOWWINDOW/SW_HIDE; the first
        // ShowWindow follows that startup request. The second honors ours.
        ShowWindow(value, SW_SHOWNORMAL);
        SetForegroundWindow(value); SetFocus(value);
        MSG message; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        test_foreground = value;
        have_clip = GetClipCursor(&original_clip) != FALSE;
    }
    ~Window() { overlay_shutdown(); if (have_clip) ClipCursor(&original_clip); if (value) DestroyWindow(value); }
};
struct Gpu {
    bool dx11{};
    DXGI_FORMAT format{};
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> errors;
    std::uint64_t sequence{};
    Gpu(HWND window, bool use11, DXGI_FORMAT color) : dx11(use11), format(color) {
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory failed");
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter unavailable");
        if (dx11) check(D3D11CreateDevice(warp.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device11, nullptr, &context11), "D3D11 WARP device failed");
        else {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
            check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12)), "D3D12 WARP device failed");
            device12.As(&errors);
            D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            check(device12->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue failed");
            check(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator failed");
            check(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "CreateCommandList failed");
            check(list->Close(), "Close failed");
            check(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence failed");
        }
        create_chain(window);
    }
    void create_chain(HWND window) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 800; desc.Height = 720; desc.Format = format;
        desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
        desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> chain;
        check(factory->CreateSwapChainForHwnd(dx11 ? static_cast<IUnknown*>(device11.Get()) : static_cast<IUnknown*>(queue.Get()), window, &desc, nullptr, nullptr, &chain), "CreateSwapChain failed");
        check(chain.As(&swapchain), "SwapChain3 unavailable");
    }
    void begin() {
        check(allocator->Reset(), "Allocator reset failed");
        check(list->Reset(allocator.Get(), nullptr), "Command list reset failed");
    }
    void end() {
        check(list->Close(), "Command list close failed");
        ID3D12CommandList* commands = list.Get(); queue->ExecuteCommandLists(1, &commands);
        idle();
    }
    void idle() {
        if (dx11) { context11->Flush(); return; }
        check(queue->Signal(fence.Get(), ++sequence), "Fence signal failed");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(event != nullptr, "Fence event failed");
        check(fence->SetEventOnCompletion(sequence, event), "SetEventOnCompletion failed");
        const auto result = WaitForSingleObject(event, 10000); CloseHandle(event);
        require(result == WAIT_OBJECT_0, "GPU fence timed out");
    }
    static D3D12_RESOURCE_BARRIER barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER value{}; value.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        value.Transition.pResource = resource; value.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        value.Transition.StateBefore = before; value.Transition.StateAfter = after; return value;
    }
    void clear() {
        const float black[]{0, 0, 0, 1};
        if (dx11) {
            ComPtr<ID3D11Texture2D> buffer; ComPtr<ID3D11RenderTargetView> target;
            check(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "DX11 buffer failed");
            check(device11->CreateRenderTargetView(buffer.Get(), nullptr, &target), "DX11 RTV failed");
            context11->ClearRenderTargetView(target.Get(), black);
        } else {
            ComPtr<ID3D12Resource> buffer; ComPtr<ID3D12DescriptorHeap> heap;
            check(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)), "DX12 buffer failed");
            D3D12_DESCRIPTOR_HEAP_DESC desc{}; desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; desc.NumDescriptors = 1;
            check(device12->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)), "DX12 RTV heap failed");
            const auto target = heap->GetCPUDescriptorHandleForHeapStart(); device12->CreateRenderTargetView(buffer.Get(), nullptr, target);
            begin(); auto transition = barrier(buffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &transition); list->ClearRenderTargetView(target, black, 0, nullptr);
            std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter); list->ResourceBarrier(1, &transition); end();
        }
    }
    std::vector<unsigned char> pixels() {
        const auto stride = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8U : 4U;
        std::vector<unsigned char> result(800U * 720U * stride);
        if (dx11) {
            ComPtr<ID3D11Texture2D> buffer, staging;
            check(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "Readback buffer failed");
            D3D11_TEXTURE2D_DESC desc{}; buffer->GetDesc(&desc); desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
            check(device11->CreateTexture2D(&desc, nullptr, &staging), "Readback texture failed");
            context11->CopyResource(staging.Get(), buffer.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{}; check(context11->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Readback map failed");
            for (UINT y = 0; y < 720; ++y) memcpy(result.data() + SIZE_T(y) * 800 * stride, static_cast<unsigned char*>(mapped.pData) + SIZE_T(y) * mapped.RowPitch, 800 * stride);
            context11->Unmap(staging.Get(), 0);
        } else {
            ComPtr<ID3D12Resource> buffer, staging;
            check(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)), "Readback buffer failed");
            const auto texture_desc = buffer->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 total{};
            device12->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
            D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = total; desc.Height = 1;
            desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
            check(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&staging)), "Readback resource failed");
            begin(); auto transition = barrier(buffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE); list->ResourceBarrier(1, &transition);
            D3D12_TEXTURE_COPY_LOCATION source{}, target{}; source.pResource = buffer.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            target.pResource = staging.Get(); target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; target.PlacedFootprint = footprint;
            list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
            std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter); list->ResourceBarrier(1, &transition); end();
            unsigned char* mapped{}; check(staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Readback map failed");
            for (UINT y = 0; y < 720; ++y) memcpy(result.data() + SIZE_T(y) * 800 * stride, mapped + footprint.Offset + SIZE_T(y) * footprint.Footprint.RowPitch, 800 * stride);
            staging->Unmap(0, nullptr);
        }
        return result;
    }
    void verify_errors() {
        if (!errors) return;
        for (UINT64 i = 0; i < errors->GetNumStoredMessages(); ++i) {
            SIZE_T size{}; errors->GetMessage(i, nullptr, &size); std::vector<unsigned char> bytes(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); errors->GetMessage(i, message, &size);
            if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
                std::fprintf(stderr, "%s\n", message->pDescription); require(false, "D3D12 debug layer reported an error");
            }
        }
    }
};
struct PipelineProbe11 {
    ComPtr<ID3D11ShaderResourceView> backbuffer_srv;
    ComPtr<ID3D11RenderTargetView> other_target;
    ComPtr<ID3D11UnorderedAccessView> other_uav;
    ComPtr<ID3D11ComputeShader> compute;
    void bind(Gpu& gpu) {
        if (!gpu.dx11) return;
        ComPtr<ID3D11Texture2D> buffer, target, unordered;
        check(gpu.swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "Pipeline probe backbuffer unavailable");
        check(gpu.device11->CreateShaderResourceView(buffer.Get(), nullptr, &backbuffer_srv), "Pipeline probe backbuffer SRV unavailable");
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = desc.Height = 16; desc.MipLevels = desc.ArraySize = 1;
        desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        check(gpu.device11->CreateTexture2D(&desc, nullptr, &target), "Pipeline probe target failed");
        check(gpu.device11->CreateRenderTargetView(target.Get(), nullptr, &other_target), "Pipeline probe RTV failed");
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        check(gpu.device11->CreateTexture2D(&desc, nullptr, &unordered), "Pipeline probe unordered resource failed");
        check(gpu.device11->CreateUnorderedAccessView(unordered.Get(), nullptr, &other_uav), "Pipeline probe UAV failed");
        constexpr char shader[] = "[numthreads(1,1,1)] void main() {}";
        ComPtr<ID3DBlob> code;
        check(D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, nullptr), "Pipeline probe shader compilation failed");
        check(gpu.device11->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &compute), "Pipeline probe shader creation failed");
        auto* rtv = other_target.Get(); auto* uav = other_uav.Get(); const UINT keep_count = UINT(-1);
        gpu.context11->OMSetRenderTargetsAndUnorderedAccessViews(1, &rtv, nullptr, 1, 1, &uav, &keep_count);
        auto* srv = backbuffer_srv.Get(); gpu.context11->PSSetShaderResources(7, 1, &srv);
        gpu.context11->CSSetShader(compute.Get(), nullptr, 0);
    }
    void verify_and_clear(Gpu& gpu) {
        if (!gpu.dx11) return;
        ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11UnorderedAccessView> uav; ComPtr<ID3D11ComputeShader> shader;
        gpu.context11->PSGetShaderResources(7, 1, &srv);
        gpu.context11->OMGetRenderTargets(1, &rtv, nullptr);
        gpu.context11->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, 1, &uav);
        gpu.context11->CSGetShader(&shader, nullptr, nullptr);
        require(srv.Get() == backbuffer_srv.Get(), "Overlay lost a conflicting game SRV outside slot zero");
        require(rtv.Get() == other_target.Get() && uav.Get() == other_uav.Get(), "Overlay lost game output-merger state");
        require(shader.Get() == compute.Get(), "Overlay did not restore the game compute shader");
        gpu.context11->ClearState();
        backbuffer_srv.Reset(); // A held view would correctly prevent resize.
    }
};
float red(const unsigned char* pixel, DXGI_FORMAT format) {
    if (format == DXGI_FORMAT_R10G10B10A2_UNORM) { std::uint32_t packed; memcpy(&packed, pixel, 4); return float(packed & 1023U) / 1023.0F; }
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        std::uint16_t half; memcpy(&half, pixel, 2); const auto exponent = (half >> 10) & 31; const auto mantissa = half & 1023;
        return exponent ? std::ldexp(1.0F + float(mantissa) / 1024.0F, int(exponent) - 15) : std::ldexp(float(mantissa), -24);
    }
    return float(pixel[0]) / 255.0F;
}
void toggle(HWND window, bool repeat = false) {
    SendMessageW(window, WM_KEYDOWN, VK_F8, repeat ? (LPARAM{1} << 30) | 1 : 1);
    if (!repeat) SendMessageW(window, WM_KEYUP, VK_F8, (LPARAM{1} << 31) | (LPARAM{1} << 30) | 1);
}
void capture(const std::string& path, const std::vector<unsigned char>& image) {
    if (path.empty()) return;
    BITMAPFILEHEADER file{}; file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER); file.bfSize = file.bfOffBits + static_cast<DWORD>(image.size());
    BITMAPINFOHEADER info{}; info.biSize = sizeof(info); info.biWidth = 800; info.biHeight = -720;
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    auto bgra = image; for (std::size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i+2]);
    std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(&file), sizeof(file)); out.write(reinterpret_cast<const char*>(&info), sizeof(info)); out.write(reinterpret_cast<const char*>(bgra.data()), bgra.size());
    require(bool(out), "Screenshot write failed");
}
}

int run_vulkan_overlay_tests(bool layer=false);
int main(int argc, char** argv) {
    if(argc==2 && std::strcmp(argv[1],"--vulkan-layer")==0)return run_vulkan_overlay_tests(true);
    if(argc==2 && std::strcmp(argv[1],"--vulkan")==0)return run_vulkan_overlay_tests();
    try {
        if (argc == 2 && std::strcmp(argv[1], "--ui") == 0) { test_ui_diagnostics(); return 0; }
        if (argc == 2 && std::strcmp(argv[1], "--shared11") == 0) { test_shared_menu11(); return 0; }
        bool dx11{}, hdr10{}, scrgb{}; std::string capture_path;
        for (int i = 1; i < argc; ++i) {
            const std::string arg(argv[i]); dx11 |= arg == "--dx11"; hdr10 |= arg == "--hdr10"; scrgb |= arg == "--scrgb";
            if (arg.starts_with("--capture=")) capture_path = arg.substr(10);
        }
        const auto format = hdr10 ? DXGI_FORMAT_R10G10B10A2_UNORM : scrgb ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
        Window window; Gpu gpu(window.value, dx11, format);
        const OverlayRuntime runtime{1, command, snapshot, "Test"};
        const auto space = hdr10 ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : scrgb ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        overlay_set_color_space(gpu.swapchain.Get(), space);
        gpu.clear(); const auto baseline = gpu.pixels();
        test_foreground = nullptr;
        overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime);
        require(std::string(overlay_status()).find("focus the game's") != std::string::npos,
            "Unfocused startup did not explain why the menu is unavailable");
        require(gpu.pixels() == baseline, "Unfocused startup modified the swap chain");
        test_foreground = window.value;
        ShowWindow(window.value, SW_HIDE);
        overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime);
        require(std::string(overlay_status()).find("window is hidden") != std::string::npos,
            "Hidden startup did not explain why the menu is unavailable");
        ShowWindow(window.value, SW_SHOW);
        if (!dx11) {
            overlay_present(gpu.swapchain.Get(), nullptr, runtime);
            require(std::string(overlay_status()).find("waiting") != std::string::npos, "Unknown queue was not refused");
            require(gpu.pixels() == baseline, "Unknown-queue path modified the swap chain");
        }
        overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime);
        require(std::string(overlay_status()).find("Menu ready") != std::string::npos,
            "Menu did not recover after restoring window visibility and focus");
        require(snapshots == 0, "Closed overlay should not poll diagnostic snapshots");
        require(gpu.pixels() == baseline, "Closed overlay modified the swap chain");
        RECT requested_clip{100, 100, 600, 500}, actual_clip{};
        const bool test_clip = ClipCursor(&requested_clip) && GetClipCursor(&actual_clip);
        SendMessageW(window.value, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(1, 1));
        toggle(window.value);
        require(game_button_down == 1 && game_button_up == 1, "Opening overlay left a held mouse button active in the game");
        SendMessageW(window.value, WM_LBUTTONUP, 0, MAKELPARAM(1, 1));
        require(game_button_up == 1, "Open overlay did not capture mouse button input");
        SendMessageW(window.value, WM_KEYDOWN, 'W', 1); SendMessageW(window.value, WM_KEYUP, 'W', LPARAM{1} << 31);
        require(game_keys == 0, "Open overlay did not suppress keyboard input");
        for (int i = 0; i < 4; ++i) {
            gpu.clear();
            PipelineProbe11 pipeline; if (i == 3) pipeline.bind(gpu);
            overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); gpu.idle();
            if (i == 3) pipeline.verify_and_clear(gpu);
        }
        require(snapshots > 0, "F8 did not open the overlay");
        const auto image = gpu.pixels(); require(image != baseline, "Overlay produced no pixels");
        if (!hdr10 && !scrgb) capture(capture_path, image);
        const auto stride = scrgb ? 8U : 4U; float peak{}; unsigned visible{};
        for (std::size_t offset = 0; offset < image.size(); offset += stride) { const float value = red(image.data() + offset, format); peak = (std::max)(peak, value); visible += value > .01F; }
        require(visible > 10000, "Overlay draw coverage is unexpectedly small");
        require(red(image.data() + image.size() - stride, format) == 0.0F, "Overlay changed pixels outside its window");
        require(hdr10 ? peak > .5F && peak < .65F : scrgb ? peak > 2.0F && peak < 3.0F : peak > .9F, "Overlay white does not match its target transfer function");
        // Pointer-capable games can deliver no legacy mouse-button messages.
        // Exercise an actual checkbox, and verify that the game sees no click.
        auto* imgui = ImGui::GetCurrentContext();
        require(imgui != nullptr, "Overlay ImGui context");
        ImGuiContextHook hook{};
        hook.Type = ImGuiContextHookType_EndFramePre;
        hook.Callback = pointer_checkbox_hook;
        const auto hook_id = ImGui::AddContextHook(imgui, &hook);
        const auto draw = [&] { gpu.clear(); overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); gpu.idle(); };
        draw(); draw();
        POINT pointer{static_cast<LONG>(pointer_checkbox_position.x), static_cast<LONG>(pointer_checkbox_position.y)};
        ClientToScreen(window.value, &pointer);
        const auto position = MAKELPARAM(pointer.x, pointer.y);
        const auto game_pointer_before = game_pointer_messages;
        SendMessageW(window.value, WM_POINTERUPDATE, MAKEWPARAM(1, POINTER_MESSAGE_FLAG_PRIMARY), position);
        draw();
        SendMessageW(window.value, WM_POINTERDOWN, MAKEWPARAM(1, POINTER_MESSAGE_FLAG_PRIMARY | POINTER_MESSAGE_FLAG_FIRSTBUTTON | POINTER_MESSAGE_FLAG_INCONTACT), position);
        draw();
        SendMessageW(window.value, WM_POINTERUP, MAKEWPARAM(1, POINTER_MESSAGE_FLAG_PRIMARY), position);
        draw();
        require(pointer_checkbox, "Pointer click must toggle the overlay checkbox");
        require(game_pointer_messages == game_pointer_before, "Menu pointer click leaked to game");
        // FH6 polls mouse buttons directly instead of delivering click messages.
        test_mouse_down[0] = true;
        require((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0, "Open menu leaked a polled mouse press to the game");
        draw(); draw();
        require(pointer_checkbox, "Checkbox changed before physical button release");
        test_mouse_down[0] = false;
        draw(); draw();
        require(!pointer_checkbox, "Physical mouse click without messages must toggle the menu checkbox once");
        test_foreground = nullptr; test_mouse_down[0] = true;
        require((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, "Unfocused menu suppressed the game's mouse polling");
        test_foreground = window.value; test_mouse_down[0] = false;
        const auto* draw_data = ImGui::GetDrawData();
        require(std::abs(draw_data->DisplaySize.x * draw_data->FramebufferScale.x - 800.0F) < .1F &&
            std::abs(draw_data->DisplaySize.y * draw_data->FramebufferScale.y - 720.0F) < .1F,
            "Menu viewport must match actual backbuffer, not window client size");
        require(ImGui::GetIO().MouseDrawCursor, "Menu cursor must remain visible at the hit-test position");
        ImGui::RemoveContextHook(imgui, hook_id);
        auto* focus_input = attach_input(window.value);
        test_foreground = nullptr;
        SendMessageW(window.value, WM_KILLFOCUS, 0, 0);
        SendMessageW(window.value, WM_ACTIVATEAPP, FALSE, 0);
        require(focus_input->open && !focus_input->cursor_released,
            "Focus loss must keep the menu open and release cursor ownership");
        toggle(window.value);
        const auto keys_before_blur = game_keys;
        SendMessageW(window.value, WM_KEYDOWN, 'W', 1);
        SendMessageW(window.value, WM_KEYUP, 'W', LPARAM{1} << 31);
        require(game_keys == keys_before_blur + 2, "Unfocused menu captured desktop keyboard input");
        gpu.clear(); overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); gpu.idle();
        require(gpu.pixels() != baseline && focus_input->open,
            "Unfocused menu must keep rendering and ignore background F8");
        require(!focus_input->cursor_released, "Background rendering reclaimed the desktop cursor");
        test_foreground = window.value;
        SendMessageW(window.value, WM_SETFOCUS, 0, 0);
        SendMessageW(window.value, WM_ACTIVATEAPP, TRUE, 0);
        // Simulate the game restoring its own cursor confinement on activation.
        if (test_clip) ClipCursor(&actual_clip);
        toggle(window.value, true); gpu.clear(); overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); gpu.idle();
        require(gpu.pixels() != baseline, "F8 autorepeat incorrectly closed the overlay");
        toggle(window.value); gpu.clear(); overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); gpu.idle();
        require(gpu.pixels() == baseline, "F8 did not close the overlay");
        test_mouse_down[0] = true;
        require((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, "Closed menu suppressed the game's mouse polling");
        test_mouse_down[0] = false;
        SendMessageW(window.value, WM_POINTERDOWN, MAKEWPARAM(1, POINTER_MESSAGE_FLAG_PRIMARY | POINTER_MESSAGE_FLAG_FIRSTBUTTON), position);
        SendMessageW(window.value, WM_POINTERUP, MAKEWPARAM(1, POINTER_MESSAGE_FLAG_PRIMARY), position);
        require(game_pointer_messages == game_pointer_before + 2, "Closed overlay must pass pointer clicks to game");
        const auto pump = [] {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
        };
        // No keyboard window message: simulate a game consuming F8 upstream.
        test_f8_down = true;
        overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime);
        pump();
        for (unsigned frame = 0; frame < 3; ++frame) {
            gpu.clear(); overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); gpu.idle(); pump();
        }
        require(gpu.pixels() != baseline, "Polled F8 must open the overlay without a key message and stay open while held");
        // A normal key message arriving after polling must not toggle again.
        SendMessageW(window.value, WM_KEYDOWN, VK_F8, 1);
        gpu.clear(); overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); gpu.idle();
        require(gpu.pixels() != baseline, "Polled and window F8 toggled the same press twice");
        test_f8_down = false;
        overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime);
        // Also check the opposite order: message first, polling second.
        test_f8_down = true;
        SendMessageW(window.value, WM_KEYDOWN, VK_F8, 1);
        gpu.clear(); overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); gpu.idle(); pump();
        gpu.clear(); overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); gpu.idle();
        require(gpu.pixels() == baseline, "Window and polled F8 reopened the menu on one press");
        test_f8_down = false;
        overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime);
        if (test_clip) { RECT restored{}; require(GetClipCursor(&restored) && EqualRect(&actual_clip, &restored), "Closing overlay did not restore cursor confinement"); }
        SendMessageW(window.value, WM_KEYDOWN, 'W', 1); SendMessageW(window.value, WM_KEYUP, 'W', LPARAM{1} << 31);
        require(game_keys == keys_before_blur + 4, "Closed overlay did not pass keyboard input through");
        // Rebinding must consume the captured press, persist the new key, and
        // leave the former key unable to toggle the menu.
        auto* input = attach_input(window.value);
        require(input != nullptr, "Menu input state unavailable for rebind test");
        const auto test_ini = std::filesystem::temp_directory_path() /
            (std::wstring(L"CheekyOverlayTests-") + std::to_wstring(GetCurrentProcessId()) + L".ini");
        input->config_path = test_ini.wstring();
        toggle(window.value);
        begin_menu_key_rebind(*input);
        SendMessageW(window.value, WM_KEYDOWN, VK_F9, 1);
        require(consume_menu_key_rebind(*input) == VK_F9, "Rebind did not capture F9");
        require(save_menu_key(*input, VK_F9), "Rebind did not save F9");
        SendMessageW(window.value, WM_KEYUP, VK_F9, LPARAM{1} << 31);
        require(GetPrivateProfileIntW(L"Overlay", L"MenuKey", 0, test_ini.c_str()) == VK_F9,
            "Rebind did not persist F9");
        toggle(window.value);
        gpu.clear(); overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); gpu.idle();
        require(gpu.pixels() != baseline, "Old F8 key toggled the rebound menu");
        test_extra_key = VK_F9; test_extra_down = true;
        overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); pump();
        gpu.clear(); overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); gpu.idle();
        require(gpu.pixels() == baseline, "Polled F9 did not close the rebound menu");
        test_extra_down = false;
        overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime);
        SendMessageW(window.value, WM_KEYDOWN, VK_F9, 1);
        SendMessageW(window.value, WM_KEYUP, VK_F9, LPARAM{1} << 31);
        gpu.clear(); overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); gpu.idle();
        require(gpu.pixels() != baseline, "F9 did not reopen the rebound menu");
        begin_menu_key_rebind(*input);
        SendMessageW(window.value, WM_KEYDOWN, VK_ESCAPE, 1);
        require(consume_menu_key_rebind(*input) == VK_ESCAPE && input->menu_key == VK_F9,
            "Escape did not cancel the key change");
        SendMessageW(window.value, WM_KEYUP, VK_ESCAPE, LPARAM{1} << 31);
        require(save_menu_key(*input, VK_F8), "Could not restore F8 after rebind test");
        require(GetPrivateProfileIntW(L"Overlay", L"MenuKey", 0, test_ini.c_str()) == VK_F8,
            "F8 reset did not persist");
        DeleteFileW(test_ini.c_str());
        toggle(window.value);
        gpu.clear(); overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); gpu.idle();
        require(gpu.pixels() == baseline, "Reset F8 did not close the menu");
        test_foreground = nullptr; toggle(window.value);
        gpu.clear(); overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime);
        require(gpu.pixels() == baseline, "Unfocused F8 opened the overlay");
        test_foreground = window.value;
        overlay_before_resize(gpu.swapchain.Get());
        check(gpu.swapchain->ResizeBuffers(2, 800, 720, format, 0), "Resize retained overlay backbuffer references");
        overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); toggle(window.value);
        for (int i = 0; i < 3; ++i) { gpu.clear(); overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); gpu.idle(); }
        require(gpu.pixels() != baseline, "Overlay failed to recreate after resize");
        // A game can release its old chain and create another on the same
        // HWND without ResizeBuffers. Host factory hooks must drop our refs.
        gpu.swapchain.Reset(); overlay_before_create(window.value);
        gpu.create_chain(window.value);
        overlay_set_color_space(gpu.swapchain.Get(), space);
        overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); toggle(window.value);
        for (int i = 0; i < 3; ++i) { gpu.clear(); overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); gpu.idle(); }
        require(gpu.pixels() != baseline, "Overlay failed after same-HWND swap-chain replacement");
        overlay_shutdown(); gpu.verify_errors();
        std::printf("Overlay %s %s passed: F8, input capture, actual pixels, white %.3f, resize, GPU validation\n", dx11 ? "DX11" : "DX12", hdr10 ? "HDR10" : scrgb ? "scRGB" : "SDR", peak);
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "Overlay test failed: %s (%s)\n", error.what(), overlay_status()); return 1; }
}
