#pragma once
#include <cstdint>
#include <array>
namespace cheeky::standalone {
struct MenuPointer { float x{}, y{}; bool down{}, active{}; };
// Coalesce motion, never a normal press/release pair. The XR and desktop
// render loops have independent cadence (especially with frame generation).
struct MenuPointerQueue {
    std::array<MenuPointer, 64> events{};
    unsigned count{};
    void push(MenuPointer event) noexcept {
        if (count && events[count-1].down == event.down && events[count-1].active == event.active) {
            events[count-1] = event; return;
        }
        // Bounded fail-safe: overflow cancels the drag rather than sticking it.
        if (count == events.size()) { count = 1; events[0] = {}; return; }
        events[count++] = event;
    }
    MenuPointer pop() noexcept {
        const auto event = events[0];
        for (unsigned i = 1; i < count; ++i) events[i-1] = events[i];
        --count;
        return event;
    }
};
constexpr bool controller_pointer_owns(bool hit, bool dragging, bool clicked,
    std::uint64_t now, std::uint64_t desktop_activity, bool desktop_held = false) noexcept {
    return hit && (dragging || (!desktop_held && (clicked || !desktop_activity ||
        (now >= desktop_activity && now - desktop_activity > 1500))));
}
// Latch for the renderer lifetime. A stalled XR frame must not start a second
// compositor consuming the same menu texture. Module presence alone is not
// proof of an active XR application (native OpenVR hosts can load the layer).
constexpr bool openxr_menu_owns(bool owned, bool layer_loaded,
    std::uint64_t now, std::uint64_t last_request) noexcept {
    return owned || (layer_loaded && last_request && now >= last_request && now - last_request <= 3000);
}
}
