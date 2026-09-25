#include "overlay_input.hpp"
#include <imgui.h>
#include <MinHook.h>
#include <filesystem>
extern IMGUI_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
#ifdef CHEEKY_OVERLAY_TEST_DESKTOP
extern bool cheeky_overlay_test_foreground(HWND);
extern bool cheeky_overlay_test_key_down(int);
extern bool cheeky_overlay_test_mouse_down(unsigned);
#endif
namespace cheeky::standalone {
namespace {
constexpr wchar_t input_property[]=L"Cheeky.Standalone.Overlay.Input.1";
std::atomic<InputState*> capture_owner{};
using AsyncKeyState = SHORT (WINAPI*)(int);
AsyncKeyState original_async_key_state = GetAsyncKeyState;
constexpr int mouse_keys[]{VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
bool valid_menu_key(unsigned key) {
    return key >= VK_BACK && key <= 0xfe && key != VK_ESCAPE &&
        key != VK_SHIFT && key != VK_CONTROL && key != VK_MENU &&
        key != VK_LSHIFT && key != VK_RSHIFT &&
        key != VK_LCONTROL && key != VK_RCONTROL &&
        key != VK_LMENU && key != VK_RMENU &&
        key != VK_LWIN && key != VK_RWIN;
}
std::wstring menu_config_path() {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&attach_input), &module)) return {};
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(module, path, 32768);
    if (!length || length >= 32768) return {};
    return (std::filesystem::path(path).parent_path() / L"CheekyOverlay.ini").wstring();
}
SHORT physical_key_state(int key) {
#ifdef CHEEKY_OVERLAY_TEST_DESKTOP
    for (unsigned button = 0; button < 5; ++button)
        if (key == mouse_keys[button]) return cheeky_overlay_test_mouse_down(button) ? SHORT(0x8000) : 0;
#endif
    return original_async_key_state(key);
}
SHORT WINAPI captured_async_key_state(int key) {
    const auto result = physical_key_state(key);
    for (const auto mouse_key : mouse_keys) if (key == mouse_key) {
        const auto input = capture_owner.load();
        if (input && input->enabled && input->open && foreground(input->window)) return 0;
        break;
    }
    return result;
}
void install_mouse_capture() {
    static std::once_flag once;
    std::call_once(once, [] {
        const auto initialized = MH_Initialize();
        if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return;
        const auto target = reinterpret_cast<void*>(GetAsyncKeyState);
        if (MH_CreateHook(target, reinterpret_cast<void*>(captured_async_key_state),
                reinterpret_cast<void**>(&original_async_key_state)) != MH_OK ||
            MH_EnableHook(target) != MH_OK)
            OutputDebugStringW(L"Cheeky: mouse polling capture could not be installed.\n");
    });
}
UINT toggle_message() {
    static const UINT value = RegisterWindowMessageW(L"Cheeky.Standalone.Overlay.Toggle.1");
    return value;
}
}
bool foreground(HWND window) {
#ifdef CHEEKY_OVERLAY_TEST_DESKTOP
    return cheeky_overlay_test_foreground(window);
#else
    const auto active = GetForegroundWindow();
    return window && (active == window || GetAncestor(window, GA_ROOT) == active);
#endif
}

void release_cursor(InputState& input) {
    capture_owner = &input;
    std::lock_guard lock(input.cursor_mutex);
    if (!input.cursor_released) input.cursor_released = GetClipCursor(&input.previous_clip) != FALSE;
    ClipCursor(nullptr);
    CURSORINFO cursor{sizeof(cursor)};
    if (GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING))
        PostMessageW(input.window, WM_SETCURSOR, reinterpret_cast<WPARAM>(input.window), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

void restore_cursor(InputState& input, bool restore) {
    auto* expected = &input;
    capture_owner.compare_exchange_strong(expected, nullptr);
    std::lock_guard lock(input.cursor_mutex);
    // Never confine another application/the desktop after our window loses
    // focus. The game's own activation handler can establish its next clip.
    if (input.cursor_released && restore && foreground(input.window)) ClipCursor(&input.previous_clip);
    input.cursor_released = false;
    if (restore && foreground(input.window))
        PostMessageW(input.window, WM_SETCURSOR, reinterpret_cast<WPARAM>(input.window), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

bool input_message(UINT message) {
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
        (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
        message == WM_INPUT || message == WM_SETCURSOR;
}

namespace {
bool pointer_message(UINT message) {
    return message == WM_POINTERDOWN || message == WM_POINTERUP ||
        message == WM_POINTERUPDATE || message == WM_POINTERWHEEL ||
        message == WM_POINTERHWHEEL || message == WM_POINTERCAPTURECHANGED;
}
void queue_mouse_button(InputState& input, unsigned button, bool down, LPARAM position) {
    constexpr UINT presses[]{WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN, WM_XBUTTONDOWN, WM_XBUTTONDOWN};
    constexpr UINT releases[]{WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP, WM_XBUTTONUP};
    if (input.messages.size() < 2048)
        input.messages.push_back({down ? presses[button] : releases[button],
            button < 3 ? 0 : MAKEWPARAM(0, button == 3 ? XBUTTON1 : XBUTTON2), position});
}
void queue_pointer(InputState& input, UINT message, WPARAM wparam, LPARAM lparam) {
    // Pointer-capable games need not generate legacy WM_*BUTTON messages.
    // Translate on the window thread, before consuming the original event.
    POINT position{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    ScreenToClient(input.window, &position);
    const auto client_position = MAKELPARAM(position.x, position.y);
    std::lock_guard lock(input.mutex);
    if (message == WM_POINTERWHEEL || message == WM_POINTERHWHEEL) {
        if (input.messages.size() < 2048)
            input.messages.push_back({static_cast<UINT>(message == WM_POINTERWHEEL ? WM_MOUSEWHEEL : WM_MOUSEHWHEEL), wparam, lparam});
        return;
    }
    if (message != WM_POINTERCAPTURECHANGED && !IS_POINTER_PRIMARY_WPARAM(wparam)) return;
    if (input.messages.size() < 2048 && message != WM_POINTERCAPTURECHANGED)
        input.messages.push_back({WM_MOUSEMOVE, 0, client_position});
    const unsigned buttons = message == WM_POINTERCAPTURECHANGED || IS_POINTER_CANCELED_WPARAM(wparam)
        ? 0U : (HIWORD(wparam) >> 4U) & 0x1FU;
    for (unsigned button = 0; button < 5; ++button)
        if ((buttons ^ input.pointer_buttons) & (1U << button))
            queue_mouse_button(input, button, (buttons & (1U << button)) != 0, client_position);
    input.pointer_buttons = buttons;
}
void queue_raw_mouse(InputState& input, HRAWINPUT handle) {
    RAWINPUT raw{};
    UINT size = sizeof(raw);
    if (GetRawInputData(handle, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == UINT(-1) ||
        raw.header.dwType != RIM_TYPEMOUSE) return;
    std::lock_guard lock(input.mutex);
    const auto flags = raw.data.mouse.usButtonFlags;
    for (unsigned button = 0; button < 5; ++button) {
        if (flags & (RI_MOUSE_LEFT_BUTTON_DOWN << (2U * button))) queue_mouse_button(input, button, true, 0);
        if (flags & (RI_MOUSE_LEFT_BUTTON_UP << (2U * button))) queue_mouse_button(input, button, false, 0);
    }
    if ((flags & (RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL)) && input.messages.size() < 2048)
        input.messages.push_back({static_cast<UINT>(flags & RI_MOUSE_WHEEL ? WM_MOUSEWHEEL : WM_MOUSEHWHEEL),
            MAKEWPARAM(0, raw.data.mouse.usButtonData), 0});
}
void toggle_on_window_thread(InputState* input, HWND window) {
    const bool opening = !input->open.load();
    input->open = opening;
    input->pointer_buttons = 0;
    if (opening) {
        capture_owner = input;
        SetCursor(nullptr);
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
}

void process_overlay_input(InputState& input) {
    std::vector<InputMessage> messages;
    { std::lock_guard lock(input.mutex); messages.swap(input.messages); }
    unsigned message_buttons{};
    for (const auto& message : messages) {
        switch (message.message) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: message_buttons |= 1U; break;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: message_buttons |= 2U; break;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: message_buttons |= 4U; break;
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
            message_buttons |= HIWORD(message.wparam) == XBUTTON1 ? 8U : 16U; break;
        }
        ImGui_ImplWin32_WndProcHandler(input.window, message.message, message.wparam, message.lparam);
    }
    if (!input.enabled || !input.open || !foreground(input.window)) return;
    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    for (unsigned button = 0; button < 5; ++button) {
        // Preserve message ordering (including quick down/up pairs). Only fill
        // gaps from physical state, bypassing our game's polling suppression.
        if (message_buttons & (1U << button)) continue;
        const auto key = mouse_keys[swapped && button < 2 ? 1 - button : button];
        ImGui::GetIO().AddMouseButtonEvent(button, (physical_key_state(key) & 0x8000) != 0);
    }
}

void set_overlay_framebuffer_scale(unsigned width, unsigned height) {
    auto& io = ImGui::GetIO();
    // Input and layout stay in client coordinates; the renderer scales them
    // into the actual backbuffer. VR mods can resize it independently of HWND.
    io.DisplayFramebufferScale = ImVec2(io.DisplaySize.x > 0 ? width / io.DisplaySize.x : 1.0F,
        io.DisplaySize.y > 0 ? height / io.DisplaySize.y : 1.0F);
}

void poll_overlay_hotkey(InputState& input) {
    if (input.rebinding) return;
    const auto key = input.menu_key.load();
#ifdef CHEEKY_OVERLAY_TEST_DESKTOP
    const bool down = cheeky_overlay_test_key_down(key);
#else
    const bool down = (physical_key_state(key) & 0x8000) != 0;
#endif
    const bool was_down = input.menu_key_down.exchange(down);
    if (down && !was_down && input.enabled && foreground(input.window)) {
        // Preserve window-thread ownership of input release and cursor handling.
        // Both polling and normal key messages claim the same press edge.
        const auto message = toggle_message();
        if (!message || !PostMessageW(input.window, message, 0, 0)) input.menu_key_down = false;
    }
}

void begin_menu_key_rebind(InputState& input) {
    input.rebound_key = 0;
    input.rebinding = true;
}

unsigned consume_menu_key_rebind(InputState& input) { return input.rebound_key.exchange(0); }

bool save_menu_key(InputState& input, unsigned key) {
    if (!valid_menu_key(key) || input.config_path.empty()) return false;
    const auto value = std::to_wstring(key);
    if (!WritePrivateProfileStringW(L"Overlay", L"MenuKey", value.c_str(), input.config_path.c_str())) return false;
    // A held binding must finish its current press before it can toggle.
    input.menu_key_down = (physical_key_state(key) & 0x8000) != 0;
    input.menu_key = key;
    return true;
}

LRESULT CALLBACK overlay_wndproc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* input = static_cast<InputState*>(GetPropW(window, input_property));
    if (!input || !input->previous) return DefWindowProcW(window, message, wparam, lparam);
    const bool focused = foreground(window);
    if (const auto toggle = toggle_message(); toggle && message == toggle) {
        if (input->enabled && focused) toggle_on_window_thread(input, window);
        return 0;
    }
    if (message == WM_KILLFOCUS || (message == WM_ACTIVATEAPP && !wparam)) {
        input->open = false; input->menu_key_down = false; input->rebinding = false;
        input->pointer_buttons = 0; restore_cursor(*input, false);
    }
    const bool key_message = message == WM_KEYDOWN || message == WM_KEYUP ||
        message == WM_SYSKEYDOWN || message == WM_SYSKEYUP;
    const bool key_down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    if (input->enabled && focused && input->rebinding && key_message) {
        if (key_down && !(lparam & (LPARAM{1} << 30))) {
            const auto key = static_cast<unsigned>(wparam);
            if (key == VK_ESCAPE || valid_menu_key(key)) {
                input->menu_key_down = true;
                input->rebound_key = key;
                input->rebinding = false;
            }
        }
        return 0;
    }
    if (input->enabled && focused && wparam == input->menu_key.load() &&
        (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)) {
        if (key_down && !(lparam & (LPARAM{1} << 30))) {
            if (!input->menu_key_down.exchange(true)) toggle_on_window_thread(input, window);
        }
        if (message == WM_KEYUP || message == WM_SYSKEYUP) input->menu_key_down = false;
        return 0;
    }
    const bool capture = input->enabled && input->open && focused;
    if (capture && pointer_message(message)) {
        queue_pointer(*input, message, wparam, lparam);
        return 0; // Do not let the game act on the menu's pointer click.
    }
    if (capture && message == WM_INPUT) {
        queue_raw_mouse(*input, reinterpret_cast<HRAWINPUT>(lparam));
        return DefWindowProcW(window, message, wparam, lparam); // Required raw-input cleanup.
    }
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
        if (message == WM_SETCURSOR) { SetCursor(nullptr); return TRUE; }
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
    install_mouse_capture();
    if (auto* existing = static_cast<InputState*>(GetPropW(window, input_property))) {
        existing->enabled = true;
        return existing;
    }
    // The state remains resident even if another overlay chains our subclass.
    // Releasing it would leave that overlay with a dangling WndProc reference.
    auto* input = new InputState;
    input->window = window;
    input->config_path = menu_config_path();
    if (!input->config_path.empty()) {
        const auto saved = GetPrivateProfileIntW(L"Overlay", L"MenuKey", VK_F8, input->config_path.c_str());
        if (valid_menu_key(saved)) input->menu_key = saved;
    }
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

}
