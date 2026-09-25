#include "overlay.hpp"
#include "overlay_ui.hpp"
#include "overlay_input.hpp"
#include "settings_io.hpp"
#include "version.h"
#include <Windows.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>
#include "../third_party/openvr/include/openvr.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
thread_local int cheeky_overlay_color_mode{};
#ifdef CHEEKY_OVERLAY_TEST_DESKTOP
extern bool cheeky_overlay_test_foreground(HWND);
#endif

namespace cheeky::standalone {
namespace {
using Microsoft::WRL::ComPtr;
using namespace cheeky::foveated_dlss;
constexpr UINT descriptor_count = 64;
constexpr float headset_distance = 1.5f;
constexpr float headset_circumference = 6.28318530718f * 3.0f; // Gentle 3 m curve radius.
constexpr float headset_max_curvature = 1.0f / 3.0f; // At most 120 degrees of curvature.
constexpr GUID overlay_metadata_guid{0x9dbb7074, 0x42a8, 0x4bca, {0xb4, 0x4e, 0x3e, 0xc2, 0x55, 0xd5, 0x7a, 0x18}};
struct ChainMetadata { std::uint64_t identity{}; DXGI_COLOR_SPACE_TYPE color_space{}; };
std::atomic<std::uint64_t> chain_sequence{};
std::atomic<const char*> status{"Waiting for a supported foreground swap chain"};

ChainMetadata chain_metadata(IDXGISwapChain* swapchain, DXGI_FORMAT format) {
    ChainMetadata result; UINT size = sizeof(result);
    if (SUCCEEDED(swapchain->GetPrivateData(overlay_metadata_guid, &size, &result)) && size == sizeof(result) && result.identity) return result;
    result.identity = ++chain_sequence;
    result.color_space = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    swapchain->SetPrivateData(overlay_metadata_guid, sizeof(result), &result);
    return result;
}

int shader_color_mode(DXGI_COLOR_SPACE_TYPE space, DXGI_FORMAT format) {
    return space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 2 :
        space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ? 1 :
        format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? 3 : 0;
}

void set_status(const char* text) {
    if (status.exchange(text) != text) {
        OutputDebugStringA("[Cheeky overlay] ");
        OutputDebugStringA(text);
        OutputDebugStringA("\n");
    }
}

struct Frame {
    ComPtr<ID3D12CommandAllocator> allocator;
    std::uint64_t fence_value{};
};
struct Renderer : OverlayUiState {
    IDXGISwapChain* swapchain{}; // Identity only; never prevents game destruction.
    HWND window{};
    InputState* input{};
    ImGuiContext* context{};
    bool win32_ready{}, gpu_ready{}, dx12{}, poisoned{};
    bool menu_was_open{};
    DXGI_FORMAT format{};
    DXGI_COLOR_SPACE_TYPE color_space{};
    std::uint64_t chain_identity{};
    UINT width{}, height{};
    UINT framebuffer_width{}, framebuffer_height{};
    ULONGLONG last_present{};
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    ComPtr<ID3D11DeviceContext1> context11_state;
    ComPtr<ID3DDeviceContextState> isolated11;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12DescriptorHeap> rtv_heap, srv_heap;
    ComPtr<ID3D12GraphicsCommandList> command_list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D11Texture2D> headset_texture11;
    ComPtr<ID3D11RenderTargetView> headset_rtv11;
    ComPtr<ID3D12Resource> headset_texture12;
    vr::IVROverlay* vr_overlay{};
    vr::IVRSystem* vr_system{};
    vr::VROverlayHandle_t vr_handle{vr::k_ulOverlayHandleInvalid};
    std::uint32_t vr_token{};
    ULONGLONG next_vr_probe{};
    unsigned vr_buttons{};
    ImVec2 vr_pointer{};
    bool vr_pointer_valid{};
    float vr_mouse_height{};
    vr::HmdMatrix34_t vr_canvas_origin{};
    float vr_meters_per_pixel{};
    bool vr_canvas_anchored{};
    bool vr_geometry_configured{};
    ImVec2 vr_geometry_display{}, vr_geometry_scale{};
    std::vector<ComPtr<ID3D12Resource>> buffers;
    std::vector<Frame> frames;
    std::array<bool, descriptor_count> descriptors{};
    UINT srv_stride{}, rtv_stride{};
    std::uint64_t serial{}, fence_value{};
    HANDLE fence_event{};
};
struct State {
    std::mutex mutex;
    std::unique_ptr<Renderer> renderer;
};
State& state() { static auto* value = new State; return *value; }

struct ContextScope {
    ImGuiContext* previous{ImGui::GetCurrentContext()};
    explicit ContextScope(ImGuiContext* context) { ImGui::SetCurrentContext(context); }
    ~ContextScope() { ImGui::SetCurrentContext(previous); }
};

bool drain(Renderer& r) {
    if (!r.queue || !r.fence) return true;
    if (FAILED(r.device12->GetDeviceRemovedReason())) return true;
    // Signaling now also fences any texture upload performed by the backend.
    const auto value = ++r.fence_value;
    if (FAILED(r.queue->Signal(r.fence.Get(), value))) return false;
    if (r.fence->GetCompletedValue() >= value) return true;
    if (!r.fence_event || FAILED(r.fence->SetEventOnCompletion(value, r.fence_event))) return false;
    if (WaitForSingleObject(r.fence_event, 2000) != WAIT_OBJECT_0) {
        set_status("GPU timeout: overlay resources retained until a safe retry");
        return false;
    }
    return true;
}

bool destroy_renderer(std::unique_ptr<Renderer>& renderer) {
    if (!renderer) return true;
    auto& r = *renderer;
    if (r.input) { r.input->enabled = false; r.input->open = false; restore_cursor(*r.input, true); }
    if (!drain(r)) return false;
    if (r.vr_overlay && r.vr_handle != vr::k_ulOverlayHandleInvalid &&
        r.vr_token && GetModuleHandleW(L"openvr_api.dll")) {
        auto token = reinterpret_cast<std::uint32_t (*)()>(GetProcAddress(GetModuleHandleW(L"openvr_api.dll"), "VR_GetInitToken"));
        if (token && token() == r.vr_token) r.vr_overlay->DestroyOverlay(r.vr_handle);
    }
    const auto old = ImGui::GetCurrentContext();
    if (r.context) {
        ImGui::SetCurrentContext(r.context);
        if (r.gpu_ready) {
            if (r.dx12) ImGui_ImplDX12_Shutdown(); else ImGui_ImplDX11_Shutdown();
        }
        if (r.win32_ready) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(r.context);
        ImGui::SetCurrentContext(old == r.context ? nullptr : old);
    }
    if (r.fence_event) CloseHandle(r.fence_event);
    renderer.reset();
    return true;
}

bool supported_surface(DXGI_COLOR_SPACE_TYPE space, DXGI_FORMAT format) {
    if (space != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 && space != DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 &&
        space != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
        set_status("Overlay unavailable for this presentation color space");
        return false;
    }
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R16G16B16A16_FLOAT: return true;
    default: set_status("Overlay unavailable for this swap-chain format"); return false;
    }
}

bool initialize(Renderer& r, IDXGISwapChain* swapchain, ID3D12CommandQueue* queue, const DXGI_SWAP_CHAIN_DESC& desc, ChainMetadata metadata) {
    r.swapchain = swapchain; r.window = desc.OutputWindow;
    r.color_space = metadata.color_space; r.chain_identity = metadata.identity;
    cheeky_overlay_color_mode = shader_color_mode(r.color_space, desc.BufferDesc.Format);
    r.width = desc.BufferDesc.Width; r.height = desc.BufferDesc.Height; r.format = desc.BufferDesc.Format;
    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&r.device11)))) {
        r.dx12 = true;
        if (!queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            FAILED(swapchain->GetDevice(IID_PPV_ARGS(&r.device12)))) {
            set_status("D3D12 overlay waiting for the swap chain's direct command queue"); return false;
        }
        ComPtr<ID3D12Device> queue_device;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&queue_device))) || queue_device.Get() != r.device12.Get()) {
            set_status("D3D12 overlay rejected a queue from another device"); return false;
        }
        ComPtr<IDXGISwapChain3> swapchain3;
        if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) || !desc.BufferCount || desc.BufferCount > 16) return false;
        r.queue = queue;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = desc.BufferCount + 1;
        if (FAILED(r.device12->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&r.rtv_heap)))) return false;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = descriptor_count; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(r.device12->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&r.srv_heap)))) return false;
        r.rtv_stride = r.device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        r.srv_stride = r.device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        r.frames.resize(desc.BufferCount); r.buffers.resize(desc.BufferCount);
        auto handle = r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT index = 0; index < desc.BufferCount; ++index) {
            if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&r.buffers[index]))) ||
                FAILED(r.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&r.frames[index].allocator)))) return false;
            r.device12->CreateRenderTargetView(r.buffers[index].Get(), nullptr, handle);
            handle.ptr += r.rtv_stride;
        }
        const auto backbuffer = r.buffers.front()->GetDesc();
        r.framebuffer_width = static_cast<UINT>(backbuffer.Width); r.framebuffer_height = backbuffer.Height;
        if (FAILED(r.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r.frames.front().allocator.Get(), nullptr, IID_PPV_ARGS(&r.command_list))) ||
            FAILED(r.command_list->Close()) || FAILED(r.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&r.fence)))) return false;
        r.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!r.fence_event) return false;
    } else {
        r.device11->GetImmediateContext(&r.context11);
        ComPtr<ID3D11Texture2D> backbuffer;
        if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) return false;
        D3D11_TEXTURE2D_DESC backbuffer_desc{}; backbuffer->GetDesc(&backbuffer_desc);
        r.framebuffer_width = backbuffer_desc.Width; r.framebuffer_height = backbuffer_desc.Height;
        ComPtr<ID3D11Device1> device1;
        if (FAILED(r.device11.As(&device1)) || FAILED(r.context11.As(&r.context11_state))) {
            set_status("Overlay needs D3D11.1 context-state isolation"); return false;
        }
        const auto feature = (std::min)(r.device11->GetFeatureLevel(), D3D_FEATURE_LEVEL_11_1);
        const UINT flags = r.device11->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
        if (FAILED(device1->CreateDeviceContextState(flags, &feature, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &r.isolated11))) {
            set_status("Unable to isolate the overlay from the D3D11 game pipeline"); return false;
        }
    }
    r.context = ImGui::CreateContext();
    ContextScope scope(r.context);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    r.win32_ready = ImGui_ImplWin32_Init(r.window);
    if (!r.win32_ready) return false;
    if (r.dx12) {
        ImGui_ImplDX12_InitInfo info;
        info.Device = r.device12.Get(); info.CommandQueue = r.queue.Get();
        info.NumFramesInFlight = static_cast<int>(r.frames.size()); info.RTVFormat = r.format;
        info.SrvDescriptorHeap = r.srv_heap.Get(); info.UserData = &r;
        info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
            auto& renderer = *static_cast<Renderer*>(info->UserData);
            for (UINT index = 0; index < descriptor_count; ++index) if (!renderer.descriptors[index]) {
                renderer.descriptors[index] = true;
                *cpu = renderer.srv_heap->GetCPUDescriptorHandleForHeapStart(); cpu->ptr += SIZE_T(index) * renderer.srv_stride;
                *gpu = renderer.srv_heap->GetGPUDescriptorHandleForHeapStart(); gpu->ptr += UINT64(index) * renderer.srv_stride;
                return;
            }
            // Only the built-in font is used; a fixed 64-descriptor pool leaves
            // ample room for atlas growth without sharing the game's heap.
            *cpu = {}; *gpu = {}; renderer.poisoned = true;
        };
        info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
            auto& renderer = *static_cast<Renderer*>(info->UserData);
            const auto base = renderer.srv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
            if (cpu.ptr >= base && renderer.srv_stride) {
                const auto index = (cpu.ptr - base) / renderer.srv_stride;
                if (index < descriptor_count) renderer.descriptors[index] = false;
            }
        };
        r.gpu_ready = ImGui_ImplDX12_Init(&info);
    } else r.gpu_ready = ImGui_ImplDX11_Init(r.device11.Get(), r.context11.Get());
    if (!r.gpu_ready) return false;
    // Backend NewFrame asserts if first-time device-object creation fails.
    // Create explicitly here so allocation/compiler failures remain contained.
    if (!(r.dx12 ? ImGui_ImplDX12_CreateDeviceObjects() : ImGui_ImplDX11_CreateDeviceObjects())) {
        set_status("Unable to create overlay GPU shaders/resources"); return false;
    }
    r.input = attach_input(r.window);
    if (!r.input) { set_status("Unable to install the menu input handler"); return false; }
    set_status("Menu ready (SDR, scRGB and HDR10)");
    return true;
}

// Join the game's existing OpenVR session. Never initialize a second session:
// native games and bridge mods own the lifetime of openvr_api.dll.
bool prepare_headset(Renderer& r) {
    const auto now = GetTickCount64();
    const auto module = GetModuleHandleW(L"openvr_api.dll");
    if (!module) return false;
    const auto token_fn = reinterpret_cast<std::uint32_t (*)()>(GetProcAddress(module, "VR_GetInitToken"));
    if (!token_fn || !token_fn()) return false;
    if (r.vr_overlay && r.vr_system && r.vr_token == token_fn() && r.vr_handle != vr::k_ulOverlayHandleInvalid &&
        (r.dx12 ? bool(r.headset_texture12) : bool(r.headset_texture11))) return true;
    if (now < r.next_vr_probe) return false;
    r.next_vr_probe = now + 1000;
    const auto valid_fn = reinterpret_cast<bool (*)(const char*)>(GetProcAddress(module, "VR_IsInterfaceVersionValid"));
    const auto get_fn = reinterpret_cast<void* (*)(const char*, vr::EVRInitError*)>(GetProcAddress(module, "VR_GetGenericInterface"));
    if (!valid_fn || !get_fn) return false;
    const auto token = token_fn();
    if (r.vr_token != token) {
        r.vr_overlay = nullptr;
        r.vr_system = nullptr;
        r.vr_canvas_anchored = false;
        r.vr_geometry_configured = false;
        r.vr_handle = vr::k_ulOverlayHandleInvalid;
        r.vr_token = token;
    }
    if (!r.vr_overlay) {
        if (!valid_fn(vr::IVROverlay_Version)) return false;
        vr::EVRInitError error{};
        r.vr_overlay = static_cast<vr::IVROverlay*>(get_fn(vr::IVROverlay_Version, &error));
        if (!r.vr_overlay || error != vr::VRInitError_None) { r.vr_overlay = nullptr; return false; }
    }
    if (!r.vr_system) {
        if (!valid_fn(vr::IVRSystem_Version)) return false;
        vr::EVRInitError error{};
        r.vr_system = static_cast<vr::IVRSystem*>(get_fn(vr::IVRSystem_Version, &error));
        if (!r.vr_system || error != vr::VRInitError_None) { r.vr_system = nullptr; return false; }
    }
    if (r.vr_handle == vr::k_ulOverlayHandleInvalid) {
        const auto key = "cheeky.foveated_dlss.menu." + std::to_string(GetCurrentProcessId());
        if (r.vr_overlay->CreateOverlay(key.c_str(), "Cheeky Foveated DLSS", &r.vr_handle) != vr::VROverlayError_None) {
            r.vr_handle = vr::k_ulOverlayHandleInvalid;
            return false;
        }
        const vr::VRTextureBounds_t bounds{0, 0, 1, 1};
        r.vr_overlay->SetOverlayTextureBounds(r.vr_handle, &bounds);
        r.vr_overlay->SetOverlayInputMethod(r.vr_handle, vr::VROverlayInputMethod_Mouse);
        r.vr_overlay->SetOverlayFlag(r.vr_handle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
        r.vr_overlay->SetOverlayFlag(r.vr_handle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
        r.vr_overlay->SetOverlayFlag(r.vr_handle, vr::VROverlayFlags_EnableClickStabilization, true);
        r.vr_overlay->SetOverlayFlag(r.vr_handle, vr::VROverlayFlags_IsPremultiplied, true);
    }
    if (r.dx12 && !r.headset_texture12) {
        D3D12_RESOURCE_DESC texture{};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = r.framebuffer_width;
        texture.Height = r.framebuffer_height;
        texture.DepthOrArraySize = 1;
        texture.MipLevels = 1;
        texture.Format = r.format;
        texture.SampleDesc.Count = 1;
        texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(r.device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r.headset_texture12)))) return false;
        auto target = r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
        target.ptr += SIZE_T(r.buffers.size()) * r.rtv_stride;
        r.device12->CreateRenderTargetView(r.headset_texture12.Get(), nullptr, target);
    } else if (!r.dx12 && !r.headset_texture11) {
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = r.framebuffer_width;
        texture.Height = r.framebuffer_height;
        texture.MipLevels = texture.ArraySize = 1;
        texture.Format = r.format;
        texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_DEFAULT;
        texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(r.device11->CreateTexture2D(&texture, nullptr, &r.headset_texture11)) ||
            FAILED(r.device11->CreateRenderTargetView(r.headset_texture11.Get(), nullptr, &r.headset_rtv11))) return false;
    }
    return true;
}

void release_headset_input(Renderer& r) {
    for (unsigned button = 0; button < 3; ++button)
        if (r.vr_buttons & (1U << button)) ImGui::GetIO().AddMouseButtonEvent(button, false);
    r.vr_buttons = 0;
    r.vr_pointer_valid = false;
}

void poll_headset_input(Renderer& r, bool ready, int desktop_events_begin) {
    if (!ready) { release_headset_input(r); return; }
    auto& io = ImGui::GetIO();
    // A click can already be queued even if SteamVR's current hover query no
    // longer targets us. Decide pointer ownership from the whole event batch.
    std::array<vr::VREvent_t, 128> batch{};
    unsigned event_count{};
    bool controller_input = r.vr_buttons || r.vr_overlay->IsHoverTargetOverlay(r.vr_handle);
    while (event_count < batch.size() && r.vr_overlay->PollNextOverlayEvent(r.vr_handle, &batch[event_count], sizeof(vr::VREvent_t))) {
        const auto type = batch[event_count++].eventType;
        controller_input |= type == vr::VREvent_MouseMove || type == vr::VREvent_MouseButtonDown ||
            type == vr::VREvent_MouseButtonUp || type == vr::VREvent_ScrollDiscrete;
    }
    if (controller_input) {
        // Win32's queued/fallback cursor and the laser must not both move the
        // same drag. Keep earlier pending VR events (ImGui may trickle them),
        // and discard only desktop pointer events queued during this frame.
        discard_desktop_pointer_events(desktop_events_begin);
    }
    // Poll only this overlay's queue, leaving the game's event queue untouched.
    for (unsigned count = 0; count < event_count; ++count) {
        const auto& event = batch[count];
        switch (event.eventType) {
        case vr::VREvent_MouseMove:
            if (std::isfinite(event.data.mouse.x) && std::isfinite(event.data.mouse.y) && r.vr_mouse_height > 0) {
                // The fixed canvas maps directly to desktop client coordinates.
                // OpenVR's texture origin is bottom-left; ImGui's is top-left.
                r.vr_pointer = ImVec2(event.data.mouse.x, r.vr_mouse_height - event.data.mouse.y);
                r.vr_pointer_valid = true;
                io.AddMousePosEvent(r.vr_pointer.x, r.vr_pointer.y);
            }
            break;
        case vr::VREvent_MouseButtonDown:
        case vr::VREvent_MouseButtonUp: {
            const bool down = event.eventType == vr::VREvent_MouseButtonDown;
            const unsigned masks[]{vr::VRMouseButton_Left, vr::VRMouseButton_Right, vr::VRMouseButton_Middle};
            for (unsigned button = 0; button < 3; ++button) if (event.data.mouse.button & masks[button]) {
                if (down) r.vr_buttons |= 1U << button;
                else r.vr_buttons &= ~(1U << button);
                if (r.vr_pointer_valid) io.AddMousePosEvent(r.vr_pointer.x, r.vr_pointer.y);
                io.AddMouseButtonEvent(button, down);
            }
            break;
        }
        case vr::VREvent_ScrollDiscrete:
            if (std::isfinite(event.data.scroll.xdelta) && std::isfinite(event.data.scroll.ydelta))
                io.AddMouseWheelEvent(event.data.scroll.xdelta, event.data.scroll.ydelta);
            break;
        case vr::VREvent_FocusLeave:
        case vr::VREvent_OverlayHidden:
            release_headset_input(r);
            break;
        default: break;
        }
    }
    // Win32 can enqueue the stationary desktop cursor each frame. Keep the
    // VR cursor steady while pointing at the panel; pointing away restores mouse.
    if (r.vr_pointer_valid && (r.vr_buttons || r.vr_overlay->IsHoverTargetOverlay(r.vr_handle)))
        io.AddMousePosEvent(r.vr_pointer.x, r.vr_pointer.y);
}

void publish_headset(Renderer& r) {
    if (!r.vr_overlay || !r.vr_system || r.vr_handle == vr::k_ulOverlayHandleInvalid) return;
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0 || r.menu_width <= 0 || r.menu_height <= 0) return;
    if (!r.vr_canvas_anchored) {
        vr::TrackedDevicePose_t head{};
        r.vr_system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &head, 1);
        if (!head.bPoseIsValid || !head.bDeviceIsConnected) return;
        // Place the desktop canvas once, independently of the menu position or
        // size. Its center is 1.5 m ahead, with a gentle curve toward the sides.
        // Pixel coordinates still match the desktop canvas.
        // Hiding/reopening the menu preserves this room-fixed placement.
        r.vr_canvas_origin = head.mDeviceToAbsoluteTracking;
        // Preserve the usual menu size until an ultrawide canvas would exceed
        // the wrap limit, then scale the canvas down enough to avoid overlap.
        r.vr_meters_per_pixel = (std::min)(1.2f / 620.0f,
            headset_circumference * headset_max_curvature / display.x);
        const float x = -display.x * 0.5f * r.vr_meters_per_pixel;
        const float y = display.y * 0.5f * r.vr_meters_per_pixel;
        for (unsigned row = 0; row < 3; ++row)
            r.vr_canvas_origin.m[row][3] += head.mDeviceToAbsoluteTracking.m[row][0] * x +
                head.mDeviceToAbsoluteTracking.m[row][1] * y - head.mDeviceToAbsoluteTracking.m[row][2] * headset_distance;
        r.vr_canvas_anchored = true;
        r.vr_geometry_configured = false;
    }
    const auto scale = ImGui::GetIO().DisplayFramebufferScale;
    if (!r.vr_geometry_configured || display.x != r.vr_geometry_display.x || display.y != r.vr_geometry_display.y ||
        scale.x != r.vr_geometry_scale.x || scale.y != r.vr_geometry_scale.y) {
        auto position = r.vr_canvas_origin;
        for (unsigned row = 0; row < 3; ++row)
            position.m[row][3] += position.m[row][0] * display.x * 0.5f * r.vr_meters_per_pixel -
                position.m[row][1] * display.y * 0.5f * r.vr_meters_per_pixel;
        r.vr_overlay->SetOverlayTransformAbsolute(r.vr_handle, vr::TrackingUniverseStanding, &position);
        const float canvas_width = display.x * r.vr_meters_per_pixel;
        r.vr_overlay->SetOverlayWidthInMeters(r.vr_handle, canvas_width);
        // OpenVR curvature is the fraction of a complete cylinder. The runtime
        // handles laser intersections with the curved surface in the same UI pixels.
        const float curvature = (std::min)(canvas_width / headset_circumference, headset_max_curvature);
        if (r.vr_overlay->SetOverlayCurvature(r.vr_handle, curvature) != vr::VROverlayError_None) {
            r.vr_overlay->SetOverlayCurvature(r.vr_handle, 0);
            set_status("SteamVR curvature unavailable; using the flat menu surface");
        }
        // VR mirror textures can differ in aspect from the desktop client area.
        if (scale.x > 0 && scale.y > 0) r.vr_overlay->SetOverlayTexelAspect(r.vr_handle, scale.y / scale.x);
        const vr::HmdVector2_t mouse_scale{{display.x, display.y}};
        r.vr_overlay->SetOverlayMouseScale(r.vr_handle, &mouse_scale);
        r.vr_mouse_height = display.y;
        // Geometry remains unchanged during normal frames/clicks/drags. Only the
        // texture needs publishing every frame; repeated curvature setters are unnecessary.
        r.vr_geometry_display = display;
        r.vr_geometry_scale = scale;
        r.vr_geometry_configured = true;
    }
    vr::D3D12TextureData_t texture12{r.headset_texture12.Get(), r.queue.Get(), 0};
    vr::Texture_t texture{r.dx12 ? static_cast<void*>(&texture12) : static_cast<void*>(r.headset_texture11.Get()),
        r.dx12 ? vr::TextureType_DirectX12 : vr::TextureType_DirectX, vr::ColorSpace_Auto};
    if (r.vr_overlay->SetOverlayTexture(r.vr_handle, &texture) == vr::VROverlayError_None)
        r.vr_overlay->ShowOverlay(r.vr_handle);
}

void hide_headset(Renderer& r) {
    const auto module = GetModuleHandleW(L"openvr_api.dll");
    const auto token = module ? reinterpret_cast<std::uint32_t (*)()>(GetProcAddress(module, "VR_GetInitToken")) : nullptr;
    if (token && token() == r.vr_token && r.vr_overlay && r.vr_handle != vr::k_ulOverlayHandleInvalid)
        r.vr_overlay->HideOverlay(r.vr_handle);
}

void render11(Renderer& r) {
    ComPtr<ID3D11Texture2D> buffer;
    ComPtr<ID3D11RenderTargetView> target;
    if (FAILED(r.swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer))) ||
        FAILED(r.device11->CreateRenderTargetView(buffer.Get(), nullptr, &target))) return;
    // The stock backend clears HS/DS/CS without restoring them, and binding
    // our RTV can unbind a game's overlapping SRVs before its backup begins.
    // A separate context state preserves the complete game pipeline, including
    // predication, stream output, UAVs, and all shader stages/resource slots.
    struct RestoreContext {
        ID3D11DeviceContext1* context;
        ComPtr<ID3DDeviceContextState> previous;
        ~RestoreContext() {
            context->ClearState(); // Do not retain the backbuffer across resize.
            context->SwapDeviceContextState(previous.Get(), nullptr);
        }
    } restore{r.context11_state.Get(), {}};
    r.context11_state->SwapDeviceContextState(r.isolated11.Get(), &restore.previous);
    auto* view = target.Get();
    r.context11->OMSetRenderTargets(1, &view, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (r.headset_rtv11) {
        const float clear[]{0, 0, 0, 0};
        r.context11->ClearRenderTargetView(r.headset_rtv11.Get(), clear);
        view = r.headset_rtv11.Get();
        r.context11->OMSetRenderTargets(1, &view, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}

void render12(Renderer& r) {
    auto& frame = r.frames[r.serial % r.frames.size()];
    if (r.fence->GetCompletedValue() < frame.fence_value) return;
    ComPtr<IDXGISwapChain3> swapchain3;
    if (FAILED(r.swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3)))) return;
    const auto index = swapchain3->GetCurrentBackBufferIndex();
    if (index >= r.buffers.size()) return;
    if (FAILED(frame.allocator->Reset()) || FAILED(r.command_list->Reset(frame.allocator.Get(), nullptr))) { r.poisoned = true; return; }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = r.buffers[index].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    r.command_list->ResourceBarrier(1, &barrier);
    auto target = r.rtv_heap->GetCPUDescriptorHandleForHeapStart(); target.ptr += SIZE_T(index) * r.rtv_stride;
    r.command_list->OMSetRenderTargets(1, &target, FALSE, nullptr);
    auto* heap = r.srv_heap.Get(); r.command_list->SetDescriptorHeaps(1, &heap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), r.command_list.Get());
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    r.command_list->ResourceBarrier(1, &barrier);
    if (r.headset_texture12) {
        D3D12_RESOURCE_BARRIER headset{};
        headset.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        headset.Transition.pResource = r.headset_texture12.Get();
        headset.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        headset.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        headset.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        r.command_list->ResourceBarrier(1, &headset);
        auto headset_target = r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
        headset_target.ptr += SIZE_T(r.buffers.size()) * r.rtv_stride;
        const float clear[]{0, 0, 0, 0};
        r.command_list->ClearRenderTargetView(headset_target, clear, 0, nullptr);
        r.command_list->OMSetRenderTargets(1, &headset_target, FALSE, nullptr);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), r.command_list.Get());
        std::swap(headset.Transition.StateBefore, headset.Transition.StateAfter);
        r.command_list->ResourceBarrier(1, &headset);
    }
    if (FAILED(r.command_list->Close())) { r.poisoned = true; return; }
    ID3D12CommandList* list = r.command_list.Get();
    r.queue->ExecuteCommandLists(1, &list);
    frame.fence_value = ++r.fence_value;
    if (FAILED(r.queue->Signal(r.fence.Get(), frame.fence_value))) r.poisoned = true;
    ++r.serial;
}
}

void overlay_present(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue, const OverlayRuntime& runtime) noexcept {
    try {
        if (!swapchain || !runtime.attachment || !runtime.snapshot || !runtime.command) return;
        auto& global = state();
        std::unique_lock lock(global.mutex, std::try_to_lock);
        if (!lock.owns_lock()) return;
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(swapchain->GetDesc(&desc)) || !IsWindow(desc.OutputWindow) ||
            !IsWindowVisible(desc.OutputWindow) || !foreground(desc.OutputWindow) ||
            desc.BufferDesc.Width < 160 || desc.BufferDesc.Height < 100) return;
        const auto metadata = chain_metadata(swapchain, desc.BufferDesc.Format);
        const auto color_space = metadata.color_space;
        if (!supported_surface(color_space, desc.BufferDesc.Format)) {
            if (global.renderer && global.renderer->input) {
                global.renderer->input->enabled = false;
                restore_cursor(*global.renderer->input, true);
            }
            return;
        }
        const auto now = GetTickCount64();
        if (global.renderer && global.renderer->swapchain != swapchain) {
            // Keep one menu on the main presentation surface. Secondary/movie
            // swap chains do not steal input while the selected one is active.
            if (now - global.renderer->last_present < 1000) return;
            if (!destroy_renderer(global.renderer)) return;
        }
        if (global.renderer && (global.renderer->window != desc.OutputWindow || global.renderer->format != desc.BufferDesc.Format ||
            global.renderer->width != desc.BufferDesc.Width || global.renderer->height != desc.BufferDesc.Height || global.renderer->color_space != color_space || global.renderer->chain_identity != metadata.identity ||
            (global.renderer->dx12 && global.renderer->queue.Get() != queue))) {
            if (!destroy_renderer(global.renderer)) return;
        }
        if (!global.renderer) {
            global.renderer = std::make_unique<Renderer>();
            if (!initialize(*global.renderer, swapchain, queue, desc, metadata)) { destroy_renderer(global.renderer); return; }
        }
        auto& r = *global.renderer;
        r.last_present = now;
        if (r.poisoned) { set_status("Overlay GPU submission failed; waiting for swap-chain reset"); return; }
        // ImGui's texture retirement uses NewFrame counts. Do not age its
        // textures during skipped GPU submissions or its backend ring can
        // retire an atlas still referenced by a queued frame.
        if (r.dx12 && r.fence->GetCompletedValue() < r.frames[r.serial % r.frames.size()].fence_value) return;
        r.input->enabled = true;
        poll_overlay_hotkey(*r.input);
        const bool menu_open = r.input->open.load();
        if (menu_open != r.menu_was_open) {
            r.menu_was_open = menu_open;
            set_status(menu_open ? "Menu opened" : "Menu closed");
        }
        cheeky_overlay_color_mode = shader_color_mode(color_space, r.format);
        ContextScope scope(r.context);
        if (r.attachment != runtime.attachment) { r.attachment = runtime.attachment; r.next_snapshot = 0; }
        const int desktop_events_begin = ImGui::GetCurrentContext()->InputEventsQueue.Size;
        process_overlay_input(*r.input, r.vr_buttons);
        if (!r.input->open) { release_headset_input(r); hide_headset(r); ImGui::GetIO().ClearInputKeys(); ImGui::GetIO().ClearInputMouse(); return; }
        release_cursor(*r.input);
        ImGui::GetIO().MouseDrawCursor = true;
        if (r.dx12) ImGui_ImplDX12_NewFrame(); else ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        set_overlay_framebuffer_scale(r.framebuffer_width, r.framebuffer_height);
        const bool headset_ready = prepare_headset(r);
        poll_headset_input(r, headset_ready, desktop_events_begin);
        ImGui::NewFrame();
        bool open=r.input->open.load();
        draw_overlay_ui(r,runtime,*r.input,r.dx12?"D3D12":"D3D11",status.load(),open);
        // Only the close button writes the atomic; concurrent menu-key input is preserved.
        if(!open){r.input->open=false;restore_cursor(*r.input,true);}
        ImGui::Render();
        if (r.dx12) render12(r); else render11(r);
        if (headset_ready && !r.poisoned && open) publish_headset(r);
        if (!open) { release_headset_input(r); hide_headset(r); }
    } catch (...) { set_status("Overlay exception contained; rendering skipped"); }
}

void overlay_before_resize(IDXGISwapChain* swapchain) noexcept {
    try {
        auto& global = state(); std::lock_guard lock(global.mutex);
        if (global.renderer && global.renderer->swapchain == swapchain) destroy_renderer(global.renderer);
    } catch (...) { set_status("Unable to release overlay resources before resize"); }
}

void overlay_shutdown() noexcept {
    try { auto& global = state(); std::lock_guard lock(global.mutex); destroy_renderer(global.renderer); }
    catch (...) { set_status("Overlay shutdown deferred"); }
}

void overlay_before_create(HWND window) noexcept {
    try {
        auto& global = state(); std::lock_guard lock(global.mutex);
        if (global.renderer && global.renderer->window == window) destroy_renderer(global.renderer);
    } catch (...) { set_status("Unable to release previous overlay before swap-chain recreation"); }
}

void overlay_set_color_space(IDXGISwapChain* swapchain, std::uint32_t color_space) noexcept {
    try {
        if (!swapchain) return;
        auto& global = state(); std::lock_guard lock(global.mutex);
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(swapchain->GetDesc(&desc))) return;
        auto metadata = chain_metadata(swapchain, desc.BufferDesc.Format);
        metadata.color_space = static_cast<DXGI_COLOR_SPACE_TYPE>(color_space);
        swapchain->SetPrivateData(overlay_metadata_guid, sizeof(metadata), &metadata);
    } catch (...) { set_status("Unable to record presentation color space"); }
}

const char* overlay_status() noexcept { return status.load(); }
}
