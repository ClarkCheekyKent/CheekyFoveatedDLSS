#include "overlay.hpp"
#include "settings_io.hpp"
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
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace cheeky::standalone;
using namespace cheeky::foveated_dlss;
// GPU test runners can use a non-interactive window station with no global
// foreground HWND. Production retains GetForegroundWindow; only this binary
// supplies an explicit foreground state for reproducible input tests.
HWND test_foreground{};
bool cheeky_overlay_test_foreground(HWND window) { return window && window == test_foreground; }
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void check(HRESULT value, const char* message) { require(SUCCEEDED(value), message); }
unsigned snapshots{}, game_keys{}, game_button_down{}, game_button_up{};
Settings settings;
bool snapshot(char* output, std::uint32_t capacity) {
    ++snapshots;
    const auto text = "{\"message\":\"Overlay regression fixture\",\"settings\":" + settings_json(settings) + "}";
    return strcpy_s(output, capacity, text.c_str()) == 0;
}
bool command(std::uint64_t attachment, const char*) { return attachment == 1; }
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
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

int main(int argc, char** argv) {
    try {
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
        if (!dx11) {
            overlay_present(gpu.swapchain.Get(), nullptr, runtime);
            require(std::string(overlay_status()).find("waiting") != std::string::npos, "Unknown queue was not refused");
            require(gpu.pixels() == baseline, "Unknown-queue path modified the swap chain");
        }
        overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime);
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
        toggle(window.value, true); gpu.clear(); overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); gpu.idle();
        require(gpu.pixels() != baseline, "F8 autorepeat incorrectly closed the overlay");
        toggle(window.value); gpu.clear(); overlay_present(gpu.swapchain.Get(), gpu.queue.Get(), runtime); gpu.idle();
        require(gpu.pixels() == baseline, "F8 did not close the overlay");
        if (test_clip) { RECT restored{}; require(GetClipCursor(&restored) && EqualRect(&actual_clip, &restored), "Closing overlay did not restore cursor confinement"); }
        SendMessageW(window.value, WM_KEYDOWN, 'W', 1); SendMessageW(window.value, WM_KEYUP, 'W', LPARAM{1} << 31);
        require(game_keys == 2, "Closed overlay did not pass keyboard input through");
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
