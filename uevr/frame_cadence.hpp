#pragma once
#include <cmath>

namespace cheeky::foveated_dlss {
// Host presentation intervals, independent of graphics API and headset refresh.
struct FrameCadence {
    double previous{}, sum{}, average_ms{};
    unsigned count{};
    bool enabled{}, initialized{};
    void reset() noexcept { *this = {}; }
    double sample(double seconds, bool sr_enabled) noexcept {
        const auto elapsed = seconds - previous;
        if (!initialized || enabled != sr_enabled || elapsed <= 0 || elapsed >= 1 || !std::isfinite(elapsed)) {
            reset(); previous = seconds; enabled = sr_enabled; initialized = true;
            return 0;
        }
        previous = seconds; sum += elapsed; ++count;
        if (sum >= 0.25) { average_ms = 1000 * sum / count; sum = 0; count = 0; }
        return elapsed * 1000;
    }
};
}
