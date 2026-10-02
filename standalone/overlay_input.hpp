#pragma once
#include <Windows.h>
#include <atomic>
#include <array>
#include <mutex>
#include <string>
#include <vector>
namespace cheeky::standalone {
struct InputMessage { UINT message; WPARAM wparam; LPARAM lparam; };
struct InputState {
    HWND window{};
    WNDPROC previous{};
    std::mutex mutex;
    std::vector<InputMessage> messages;
    std::atomic<bool> open{}, enabled{};
    std::atomic<unsigned> menu_key{VK_F8};
    std::atomic<bool> menu_key_down{}, rebinding{};
    std::atomic<unsigned> rebound_key{};
    std::wstring config_path;
    std::atomic<ULONGLONG> desktop_pointer_tick{};
    std::atomic<bool> desktop_button_down{};
    std::atomic<bool> raw_pointer_mode{};
    LONG raw_dx{}, raw_dy{}; // Protected by mutex; consumed by render thread.
    float raw_x{}, raw_y{}; // Render thread only.
    std::array<bool, 256> passed_keys{};
    std::array<bool, 5> passed_buttons{};
    unsigned pointer_buttons{}; // Window-thread state for WM_POINTER translation.
    LPARAM last_mouse_position{};
    std::mutex cursor_mutex;
    RECT previous_clip{};
    bool cursor_released{};
};

bool foreground(HWND);
void release_cursor(InputState&);
void restore_cursor(InputState&,bool);
InputState* attach_input(HWND);
void poll_overlay_hotkey(InputState&);
void begin_menu_key_rebind(InputState&);
unsigned consume_menu_key_rebind(InputState&);
bool save_menu_key(InputState&, unsigned);
void process_overlay_input(InputState&, unsigned controller_buttons = 0);
void queue_raw_motion(InputState&, LONG dx, LONG dy);
void apply_desktop_pointer(InputState&, int first_event);
void discard_desktop_pointer_events(int first_event);
void set_overlay_framebuffer_scale(unsigned width, unsigned height);
}
