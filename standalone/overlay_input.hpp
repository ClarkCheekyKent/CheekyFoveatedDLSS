#pragma once
#include <Windows.h>
#include <atomic>
#include <array>
#include <mutex>
#include <vector>
namespace cheeky::standalone {
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

bool foreground(HWND);
void release_cursor(InputState&);
void restore_cursor(InputState&,bool);
InputState* attach_input(HWND);
}
