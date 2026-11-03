#pragma once
#include <Windows.h>
#include <atomic>
#include "../shared/startup_diagnostics.hpp"

namespace cheeky::standalone {
inline void log_overlay_diagnostic(const char* message) noexcept {
    cheeky::startup::host_log(message);
}
inline void log_first_menu_hotkey() noexcept {
    static std::atomic_flag reported{};
    if (!reported.test_and_set(std::memory_order_relaxed))
        log_overlay_diagnostic("STARTUP milestone=first_menu_hotkey_received");
}
} // namespace cheeky::standalone
