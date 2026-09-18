#include "overlay_input.hpp"
#ifdef CHEEKY_OVERLAY_TEST_DESKTOP
extern bool cheeky_overlay_test_foreground(HWND);
#endif
namespace cheeky::standalone {
namespace { constexpr wchar_t input_property[]=L"Cheeky.Standalone.Overlay.Input.1"; }
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

}
