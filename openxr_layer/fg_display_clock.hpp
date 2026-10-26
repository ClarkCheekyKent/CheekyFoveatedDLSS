#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cheeky::fg {
// A target, not a throughput estimate. Runtime frame periods can grow when
// frames miss deadlines. Such growth must never lower the game's target.
struct StableDisplayClock {
    unsigned override_hz{};
    double runtime_hz{};
    std::int64_t fastest_period_ns{};

    static bool valid_hz(double hz) noexcept {return std::isfinite(hz)&&hz>=30.&&hz<=360.;}
    void set_override(unsigned hz) noexcept {override_hz=valid_hz(hz)?hz:0u;}
    void observe_refresh(double hz) noexcept {if(valid_hz(hz))runtime_hz=hz;}
    void observe_period(std::int64_t ns) noexcept {
        // Fallback for runtimes without a refresh-rate query. Learn faster
        // reports during startup/recovery; retain that target through stalls.
        // A real downward refresh change needs an explicit override or query.
        if(ns<1000000000/360||ns>1000000000)return;
        if(!fastest_period_ns||ns<fastest_period_ns)fastest_period_ns=ns;
    }
    std::int64_t display_ns() const noexcept {
        const double hz=override_hz?double(override_hz):runtime_hz;
        return hz>0?std::int64_t(std::llround(1e9/hz)):fastest_period_ns;
    }
    double display_ms() const noexcept {return double(display_ns())/1e6;}
    std::int64_t native_ns(unsigned multiplier) const noexcept {return display_ns()*std::clamp(multiplier,1u,4u);}
    const char* source() const noexcept {return override_hz?"override":runtime_hz>0?"refresh query":"fastest runtime period";}
};
}
