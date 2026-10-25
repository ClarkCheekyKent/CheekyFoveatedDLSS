#pragma once
#include <atomic>

namespace cheeky::foveated_dlss::reflex_override {
inline std::atomic<bool> requested{}, running{}, installed{}, processing{true};
inline bool active() noexcept {
    return installed.load(std::memory_order_acquire) && requested.load(std::memory_order_acquire) && running.load(std::memory_order_acquire) &&
        processing.load(std::memory_order_acquire);
}
}
