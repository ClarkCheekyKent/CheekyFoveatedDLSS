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
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
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
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = desc.BufferCount;
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
    if (!r.input) { set_status("Unable to install the F8 window input handler"); return false; }
    set_status("F8 menu ready (SDR, scRGB and HDR10)");
    return true;
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
            set_status(menu_open ? "F8 menu opened" : "F8 menu closed");
        }
        cheeky_overlay_color_mode = shader_color_mode(color_space, r.format);
        ContextScope scope(r.context);
        if (r.attachment != runtime.attachment) { r.attachment = runtime.attachment; r.next_snapshot = 0; }
        process_overlay_input(*r.input);
        if (!r.input->open) { ImGui::GetIO().ClearInputKeys(); ImGui::GetIO().ClearInputMouse(); return; }
        release_cursor(*r.input);
        ImGui::GetIO().MouseDrawCursor = true;
        if (r.dx12) ImGui_ImplDX12_NewFrame(); else ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        set_overlay_framebuffer_scale(r.framebuffer_width, r.framebuffer_height);
        ImGui::NewFrame();
        bool open=r.input->open.load();
        draw_overlay_ui(r,runtime,r.dx12?"D3D12":"D3D11",status.load(),open);
        // Only the close button writes the atomic; concurrent F8 is preserved.
        if(!open){r.input->open=false;restore_cursor(*r.input,true);}
        ImGui::Render();
        if (r.dx12) render12(r); else render11(r);
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
