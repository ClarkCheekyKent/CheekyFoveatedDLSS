#pragma once
#include <cstdint>

namespace cheeky::fg {
// Source/capture layout changes require a fresh OpenXR presenter, rather than
// an in-place history flush: setup() owns the queues, fences and swapchain state.
// The configuration hash excludes live pacing-only settings (release mode).
constexpr bool replace_presenter_for_configuration(bool enabled, std::uint64_t active,
                                                    std::uint64_t requested) noexcept {
    return enabled && active != 0 && requested != 0 && active != requested;
}
}
