#include "overlay.hpp"
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
constexpr wchar_t input_property[] = L"Cheeky.Standalone.Overlay.Input.1";
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

struct InputMessage { UINT message; WPARAM wparam; LPARAM lparam; };
struct InputState {
    HWND window{};
    WNDPROC previous{};
    std::mutex mutex;
    std::vector<InputMessage> messages;
    std::atomic<bool> open{}, enabled{};
    std::array<bool, 256> passed_keys{};
    std::array<bool, 5> passed_buttons{};
    LPARAM last_mouse_position{};
    std::mutex cursor_mutex;
    RECT previous_clip{};
    bool cursor_released{};
};

bool foreground(HWND window) {
#ifdef CHEEKY_OVERLAY_TEST_DESKTOP
    return cheeky_overlay_test_foreground(window);
#else
    const auto active = GetForegroundWindow();
    return window && (active == window || GetAncestor(window, GA_ROOT) == active);
#endif
}

void release_cursor(InputState& input) {
    std::lock_guard lock(input.cursor_mutex);
    if (!input.cursor_released) input.cursor_released = GetClipCursor(&input.previous_clip) != FALSE;
    ClipCursor(nullptr);
}

void restore_cursor(InputState& input, bool restore) {
    std::lock_guard lock(input.cursor_mutex);
    // Never confine another application/the desktop after our window loses
    // focus. The game's own activation handler can establish its next clip.
    if (input.cursor_released && restore && foreground(input.window)) ClipCursor(&input.previous_clip);
    input.cursor_released = false;
}

bool input_message(UINT message) {
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
        (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
        message == WM_INPUT || message == WM_SETCURSOR;
}

LRESULT CALLBACK overlay_wndproc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* input = static_cast<InputState*>(GetPropW(window, input_property));
    if (!input || !input->previous) return DefWindowProcW(window, message, wparam, lparam);
    const bool focused = foreground(window);
    if (message == WM_KILLFOCUS || (message == WM_ACTIVATEAPP && !wparam)) {
        input->open = false; restore_cursor(*input, false);
    }
    if (input->enabled && focused && wparam == VK_F8 &&
        (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)) {
        if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && !(lparam & (LPARAM{1} << 30))) {
            const bool opening = !input->open.load();
            input->open = opening;
            if (opening) {
                // Do not leave movement keys held in the game when the menu
                // takes over their eventual key-up messages.
                for (UINT key = 0; key < input->passed_keys.size(); ++key) {
                    if (!input->passed_keys[key]) continue;
                    input->passed_keys[key] = false;
                    CallWindowProcW(input->previous, window, WM_KEYUP, key,
                        (LPARAM{1} << 31) | (LPARAM{1} << 30) | (MapVirtualKeyW(key, MAPVK_VK_TO_VSC) << 16) | 1);
                }
                constexpr UINT releases[]{WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP, WM_XBUTTONUP};
                for (UINT button = 0; button < input->passed_buttons.size(); ++button) if (input->passed_buttons[button]) {
                    input->passed_buttons[button] = false;
                    CallWindowProcW(input->previous, window, releases[button],
                        button < 3 ? 0 : MAKEWPARAM(0, button == 3 ? XBUTTON1 : XBUTTON2), input->last_mouse_position);
                }
            } else restore_cursor(*input, true);
        }
        return 0;
    }
    const bool capture = input->enabled && input->open && focused;
    if (capture || message == WM_KILLFOCUS || message == WM_SETFOCUS) {
        // Win32 and Present may execute on different threads. No ImGui calls
        // occur in the subclass; only the rendering thread touches its context.
        if (input_message(message) || message == WM_KILLFOCUS || message == WM_SETFOCUS) {
            std::lock_guard lock(input->mutex);
            if (input->messages.size() < 2048 && message != WM_INPUT && message != WM_SETCURSOR)
                input->messages.push_back({message, wparam, lparam});
        }
    }
    if (capture && input_message(message)) {
        if (message == WM_INPUT) return DefWindowProcW(window, message, wparam, lparam);
        if (message == WM_SETCURSOR) { SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE; }
        return 0;
    }
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_KEYUP || message == WM_SYSKEYUP) && wparam < 256)
        input->passed_keys[wparam] = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) input->last_mouse_position = lparam;
    switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: input->passed_buttons[0] = true; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: input->passed_buttons[1] = true; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: input->passed_buttons[2] = true; break;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: input->passed_buttons[HIWORD(wparam) == XBUTTON1 ? 3 : 4] = true; break;
    case WM_LBUTTONUP: input->passed_buttons[0] = false; break;
    case WM_RBUTTONUP: input->passed_buttons[1] = false; break;
    case WM_MBUTTONUP: input->passed_buttons[2] = false; break;
    case WM_XBUTTONUP: input->passed_buttons[HIWORD(wparam) == XBUTTON1 ? 3 : 4] = false; break;
    }
    if (message == WM_NCDESTROY) {
        input->enabled = false;
        input->open = false;
        restore_cursor(*input, false);
        RemovePropW(window, input_property);
    }
    return CallWindowProcW(input->previous, window, message, wparam, lparam);
}

InputState* attach_input(HWND window) {
    if (auto* existing = static_cast<InputState*>(GetPropW(window, input_property))) {
        existing->enabled = true;
        return existing;
    }
    // The state remains resident even if another overlay chains our subclass.
    // Releasing it would leave that overlay with a dangling WndProc reference.
    auto* input = new InputState;
    input->window = window;
    input->previous = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
    if (!input->previous || !SetPropW(window, input_property, input)) { delete input; return nullptr; }
    SetLastError(ERROR_SUCCESS);
    const auto previous = SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(overlay_wndproc));
    if (!previous && GetLastError() != ERROR_SUCCESS) {
        RemovePropW(window, input_property); delete input; return nullptr;
    }
    input->previous = reinterpret_cast<WNDPROC>(previous);
    input->enabled = true;
    return input;
}

struct Frame {
    ComPtr<ID3D12CommandAllocator> allocator;
    std::uint64_t fence_value{};
};
struct Renderer {
    IDXGISwapChain* swapchain{}; // Identity only; never prevents game destruction.
    HWND window{};
    InputState* input{};
    ImGuiContext* context{};
    bool win32_ready{}, gpu_ready{}, dx12{}, poisoned{};
    DXGI_FORMAT format{};
    DXGI_COLOR_SPACE_TYPE color_space{};
    std::uint64_t chain_identity{};
    UINT width{}, height{};
    ULONGLONG last_present{}, next_snapshot{};
    Settings draft{};
    std::string snapshot, message;
    std::uint64_t attachment{}, request{};
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
        if (FAILED(r.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r.frames.front().allocator.Get(), nullptr, IID_PPV_ARGS(&r.command_list))) ||
            FAILED(r.command_list->Close()) || FAILED(r.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&r.fence)))) return false;
        r.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!r.fence_event) return false;
    } else {
        r.device11->GetImmediateContext(&r.context11);
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

// Snapshot is emitted by our runtime. This reader only extracts a direct
// member, respecting nesting and escaped strings so similarly named diagnostic
// values cannot accidentally become settings.
std::string_view member(std::string_view object, std::string_view name) {
    auto whitespace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t i = object.find('{');
    if (i == object.npos) return {};
    ++i;
    while (i < object.size()) {
        while (i < object.size() && (whitespace(object[i]) || object[i] == ',')) ++i;
        if (i == object.size() || object[i] != '"') return {};
        const auto key_start = ++i;
        while (i < object.size() && object[i] != '"') { if (object[i] == '\\') ++i; ++i; }
        if (i == object.size()) return {};
        const auto key = object.substr(key_start, i++ - key_start);
        while (i < object.size() && whitespace(object[i])) ++i;
        if (i == object.size() || object[i++] != ':') return {};
        while (i < object.size() && whitespace(object[i])) ++i;
        const auto start = i;
        int depth{}; bool quoted{}, escaped{};
        for (; i < object.size(); ++i) {
            const auto c = object[i];
            if (quoted) { if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == '"') quoted = false; continue; }
            if (c == '"') { quoted = true; continue; }
            if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') { if (!depth) break; --depth; }
            else if (c == ',' && !depth) break;
        }
        auto value = object.substr(start, i - start);
        while (!value.empty() && whitespace(value.back())) value.remove_suffix(1);
        if (key == name) return value;
        if (i == object.size() || object[i] == '}') return {};
    }
    return {};
}

std::string plain(std::string_view value) {
    if (value.size() < 2 || value.front() != '"') return std::string(value);
    std::string out;
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        if (value[i] == '\\' && i + 2 < value.size()) {
            const char c = value[++i];
            if (c == 'n') out += '\n'; else if (c == 't') out += '\t';
            else if (c == 'r') out += '\r'; else out += c;
        } else out += value[i];
    }
    return out;
}

void refresh(Renderer& r, const OverlayRuntime& runtime, bool force = false) {
    const auto now = GetTickCount64();
    if (!runtime.snapshot || (!force && (now < r.next_snapshot || ImGui::IsAnyItemActive()))) return;
    std::array<char, 32768> buffer{};
    if (!runtime.snapshot(buffer.data(), static_cast<std::uint32_t>(buffer.size()))) {
        r.message = "Runtime snapshot unavailable"; return;
    }
    r.snapshot = buffer.data(); r.next_snapshot = now + 250;
    const auto settings = member(r.snapshot, "settings");
    if (!settings.empty()) {
        auto draft = r.draft;
        bool valid = true;
#define CHEEKY_SETTING(name, field) { const auto value = member(settings, name); if (!value.empty()) valid &= set_named_setting(draft, name, value); }
#include "settings_fields.inc"
#undef CHEEKY_SETTING
        if (valid) r.draft = draft;
    }
    r.message = plain(member(r.snapshot, "message"));
}

bool command(Renderer& r, const OverlayRuntime& runtime, std::string_view action, std::string_view payload = {}) {
    if (!runtime.command || !runtime.attachment) return false;
    const auto text = "1\n" + std::to_string(++r.request) + "\n" + std::string(action) + "\n" + std::string(payload);
    const bool result = runtime.command(runtime.attachment, text.c_str());
    refresh(r, runtime, true);
    if (!result && r.message.empty()) r.message = "Runtime rejected the command";
    return result;
}

template<class T> auto scalar(T value) {
    if constexpr (std::is_enum_v<T>) return static_cast<std::uint32_t>(value);
    else return value;
}

void commit(Renderer& r, const OverlayRuntime& runtime, const Settings& previous) {
    std::ostringstream out; out.imbue(std::locale::classic()); out.precision(9);
#define CHEEKY_SETTING(name, field) if (previous.field != r.draft.field) out << name << '=' << scalar(r.draft.field) << '\n';
#include "settings_fields.inc"
#undef CHEEKY_SETTING
    const auto payload = out.str();
    if (!payload.empty()) command(r, runtime, "set", payload);
}

void slider(const char* label, float& value, float low, float high, const char* format = "%.2f") {
    ImGui::SliderFloat(label, &value, low, high, format, ImGuiSliderFlags_AlwaysClamp);
}

template<class T> void combo(const char* label, T& value, const char* names) {
    int index = static_cast<int>(value);
    if (ImGui::Combo(label, &index, names)) value = static_cast<T>(index);
}

void preset(const char* label, std::uint32_t& value, bool game_default) {
    constexpr std::uint32_t values[]{0,5,11,12,13};
    constexpr const char* names[]{"Game default", "E (fastest)", "K", "L", "M"};
    const int first = game_default ? 0 : 1;
    int selected{};
    for (int i = first; i < static_cast<int>(std::size(values)); ++i) if (values[i] == value) selected = i - first;
    if (ImGui::Combo(label, &selected, names + first, static_cast<int>(std::size(values)) - first)) value = values[first + selected];
}

void draw_sr(Settings& s) {
    ImGui::Checkbox("Enable foveated DLSS-SR", &s.enabled);
    ImGui::BeginDisabled(!s.enabled);
    preset("Center preset", s.center_preset, true);
    slider("Center supersampling", s.center_supersampling, 1.0F, 2.0F, "%.2fx");
    ImGui::Checkbox("Peripheral DLAA", &s.peripheral_dlaa_enabled);
    ImGui::BeginDisabled(!s.peripheral_dlaa_enabled);
    preset("Peripheral preset", s.peripheral_dlaa_preset, false);
    slider("Periphery scale", s.peripheral_dlaa_scale, .2F, 1.0F);
    ImGui::EndDisabled();
    slider("Fovea width", s.width, .2F, 1.0F);
    slider("Fovea height", s.height, .2F, 1.0F);
    slider("Roundness", s.roundness, 0.0F, 1.0F);
    slider("Transition width", s.transition_width, 0.0F, .3F, "%.3f");
    ImGui::Checkbox("Show red alignment border", &s.alignment_border_enabled);
    ImGui::EndDisabled();
}

void draw_gaze(Settings& s) {
    combo("Foveation center", s.center_mode, "Fixed\0Runtime gaze (OpenXR / OpenVR)\0Simulated gaze\0");
    ImGui::Checkbox("Automatic stereo alignment", &s.auto_stereo_alignment);
    ImGui::TextWrapped("Runtime gaze needs the Cheeky OpenXR layer or a supported OpenVR runtime. Fixed placement is used when tracking is unavailable.");
    slider("Stereo X offset", s.x_offset, -1.0F, 1.0F);
    slider("Height offset", s.auto_stereo_alignment ? s.aligned_height_offset : s.height_offset, -1.0F, 1.0F);
    ImGui::Checkbox("Invert stereo eye order", &s.invert_stereo_x_offset);
    if (s.center_mode == FoveationCenterMode::simulated_gaze) {
        combo("Simulation pattern", s.simulation_pattern, "Figure eight (8 s)\0Slow sweep (20 s)\0Jump every 2 s\0Jump every 8 s\0Tracking loss\0Hold center\0");
        ImGui::Checkbox("Show next jump target", &s.show_next_jump_target);
    }
    slider("Gaze smoothing", s.gaze_smoothing_ms, 0.0F, 100.0F, "%.0f ms");
    int pixels = static_cast<int>(s.gaze_quantization_pixels);
    if (ImGui::SliderInt("Crop origin quantization", &pixels, 1, 64, "%d px", ImGuiSliderFlags_AlwaysClamp)) s.gaze_quantization_pixels = static_cast<std::uint32_t>(pixels);
    slider("Jump reset threshold", s.gaze_jump_reset_ratio, .01F, 1.0F, "%.3f crop");
    if (ImGui::TreeNode("AFW coverage")) {
        ImGui::Checkbox("Automatic AFW coverage", &s.afw_automatic_coverage);
        ImGui::Checkbox("Manual AFW coverage", &s.afw_manual_coverage);
        slider("AFW warp margin", s.afw_warp_margin, 0.0F, .3F, "%.3f");
        ImGui::TreePop();
    }
}

void draw_nr(Settings& s) {
    ImGui::Checkbox("Enable DLSS-NR", &s.nr_enabled);
    ImGui::BeginDisabled(!s.nr_enabled);
    ImGui::Checkbox("Foveated DLSS-NR", &s.nr_foveated);
    ImGui::Checkbox("Use DLSS-SR size and shape", &s.nr_use_sr_foveation);
    ImGui::BeginDisabled(!s.nr_foveated || s.nr_use_sr_foveation);
    slider("NR fovea width", s.nr_width, .2F, 1.0F);
    slider("NR fovea height", s.nr_height, .2F, 1.0F);
    slider("NR roundness", s.nr_roundness, 0.0F, 1.0F);
    slider("NR transition width", s.nr_transition_width, 0.0F, .3F, "%.3f");
    ImGui::EndDisabled();
    ImGui::Checkbox("Show green alignment border", &s.nr_alignment_border_enabled);
    combo("Rendering order", s.nr_processing_order, "After upscaling\0Before upscaling (experimental)\0");
    slider("Working scale", s.nr_working_scale, .1F, 1.0F);
    combo("DLSS-NR style", s.nr_style, "Standard\0Natural\0Cinematic\0");
    slider("Intensity", s.nr_intensity, 0.0F, 1.0F);
    if (ImGui::TreeNode("Advanced neural rendering")) {
        slider("Local tone strength", s.nr_local_tone_strength, 0.0F, 2.0F);
        slider("Local structure strength", s.nr_local_structure_strength, 0.0F, 2.0F);
        ImGui::Checkbox("Automatic mask", &s.nr_automatic_mask);
        slider("Skin structure strength", s.nr_skin_structure_strength, 0.0F, 2.0F);
        ImGui::Checkbox("UI correction", &s.nr_ui_correction);
        slider("Paper white scale", s.nr_paper_white_scale, .01F, 8.0F);
        slider("HDR transfer strength", s.nr_hdr_transfer_strength, 0.0F, 2.0F);
        slider("Color strength", s.nr_color_strength, 0.0F, 2.0F);
        combo("Depth convention", s.nr_depth_convention, "Game NGX flags\0Normal depth\0Reversed depth\0");
        slider("Motion scale X multiplier", s.nr_motion_scale_x_multiplier, -4.0F, 4.0F);
        slider("Motion scale Y multiplier", s.nr_motion_scale_y_multiplier, -4.0F, 4.0F);
        ImGui::TreePop();
    }
    ImGui::EndDisabled();
}

void diagnostic_line(std::string_view object, const char* label, const char* key) {
    const auto value = plain(member(object, key));
    ImGui::TextWrapped("%s: %s", label, value.empty() ? "unavailable" : value.c_str());
}

template<class T> void raw_setting(const char* name, T& value) {
    if constexpr (std::is_same_v<T, bool>) ImGui::Checkbox(name, &value);
    else if constexpr (std::is_floating_point_v<T>) ImGui::InputFloat(name, &value, 0.0F, 0.0F, "%.6g");
    else {
        std::uint32_t number = static_cast<std::uint32_t>(value);
        if (ImGui::InputScalar(name, ImGuiDataType_U32, &number)) value = static_cast<T>(number);
    }
}

void draw_ui(Renderer& r, const OverlayRuntime& runtime) {
    refresh(r, runtime);
    bool open = r.input->open.load();
    ImGui::SetNextWindowSize(ImVec2(620, 650), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(420, 300), ImVec2(FLT_MAX, FLT_MAX));
    if (ImGui::Begin("Cheeky Foveated DLSS###CheekyStandalone", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextDisabled("v" CHEEKY_VERSION " | %s | %s | F8 to close", runtime.host_name ? runtime.host_name : "Standalone", r.dx12 ? "D3D12" : "D3D11");
        ImGui::TextWrapped("Changes apply when you release a control and are saved automatically.");
        ImGui::TextWrapped("%s", r.message.c_str());
        ImGui::Separator();
        const auto previous = r.draft;
        if (ImGui::BeginTabBar("controls")) {
            if (ImGui::BeginTabItem("DLSS-SR")) {
                draw_sr(r.draft);
                if (ImGui::Button("Reset SR defaults")) command(r, runtime, "defaults_sr");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Gaze / stereo")) {
                draw_gaze(r.draft);
                if (ImGui::Button("Reset gaze defaults")) command(r, runtime, "defaults_gaze");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("DLSS-NR")) {
                draw_nr(r.draft);
                if (ImGui::Button("Reset NR history / retry")) command(r, runtime, "reset_nr");
                ImGui::SameLine();
                if (ImGui::Button("Reset NR defaults")) command(r, runtime, "defaults_nr");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Diagnostics")) {
                diagnostic_line(r.snapshot, "Runtime ready", "ready");
                diagnostic_line(r.snapshot, "NR state", "nr");
                const auto gaze = member(r.snapshot, "gaze");
                diagnostic_line(gaze, "Gaze source", "runtime");
                diagnostic_line(gaze, "Gaze driving foveation", "using_gaze");
                diagnostic_line(gaze, "Active stereo views", "views");
                diagnostic_line(member(r.snapshot, "frame"), "Frame time (ms)", "present_ms");
                diagnostic_line(member(r.snapshot, "observer"), "Native observer ready", "ready");
                diagnostic_line(member(r.snapshot, "nr_details"), "NR skip reason", "skip_reason");
                ImGui::TextWrapped("Overlay: %s", status.load());
                if (ImGui::Button("Report an issue...")) command(r, runtime, "report_issue");
                ImGui::SameLine();
                if (ImGui::Button("Create support ZIP")) command(r, runtime, "report");
                ImGui::SameLine();
                if (ImGui::Button("Show support ZIP")) command(r, runtime, "show_report");
                if (ImGui::Button("Copy diagnostic snapshot")) ImGui::SetClipboardText(r.snapshot.c_str());
                if (ImGui::Button("Enable eye calibration")) command(r, runtime, "calibration_enable");
                ImGui::SameLine();
                if (ImGui::Button("Disable eye calibration")) command(r, runtime, "calibration_disable");
                if (ImGui::Button("Reset calibration counters")) command(r, runtime, "calibration_reset");
                if (ImGui::TreeNode("Full diagnostic snapshot")) {
                    ImGui::TextWrapped("%s", r.snapshot.c_str());
                    ImGui::TreePop();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("All settings")) {
                ImGui::TextWrapped("Advanced values use the same keys as the configuration file. The runtime validates and clamps each transaction.");
                ImGui::BeginChild("fields", ImVec2(0, 0), ImGuiChildFlags_None);
#define CHEEKY_SETTING(name, field) raw_setting(name, r.draft.field);
#include "settings_fields.inc"
#undef CHEEKY_SETTING
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        // Geometry/model controls can allocate expensive GPU resources. Keep a
        // local draft during dragging; commit only after the active edit ends.
        if (!ImGui::IsAnyItemActive()) {
            Settings configured = previous;
            const auto current = member(r.snapshot, "settings");
#define CHEEKY_SETTING(name, field) { const auto value = member(current, name); if (!value.empty()) set_named_setting(configured, name, value); }
#include "settings_fields.inc"
#undef CHEEKY_SETTING
            commit(r, runtime, configured);
        }
    }
    ImGui::End();
    // A concurrent F8 press may have changed the atomic while Present was
    // building the UI. Only the close button should write it from this thread.
    if (!open) { r.input->open = false; restore_cursor(*r.input, true); }
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
        cheeky_overlay_color_mode = shader_color_mode(color_space, r.format);
        ContextScope scope(r.context);
        if (r.attachment != runtime.attachment) { r.attachment = runtime.attachment; r.next_snapshot = 0; }
        std::vector<InputMessage> messages;
        { std::lock_guard input_lock(r.input->mutex); messages.swap(r.input->messages); }
        for (const auto& message : messages) ImGui_ImplWin32_WndProcHandler(r.window, message.message, message.wparam, message.lparam);
        if (!r.input->open) { ImGui::GetIO().ClearInputKeys(); ImGui::GetIO().ClearInputMouse(); return; }
        release_cursor(*r.input);
        ImGui::GetIO().MouseDrawCursor = true;
        if (r.dx12) ImGui_ImplDX12_NewFrame(); else ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw_ui(r, runtime);
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
