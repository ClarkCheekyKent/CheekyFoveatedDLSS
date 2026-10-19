#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace cheeky::fg {
// Signed time: +ms means OpenXR end-frame entry was later than native color
// GPU copy completion; -ms means OpenXR submission began before GPU completion.
// No assumption that the GPU and CPU timeline are already synchronized: caller
// must supply calibrated QPC ticks (both native eyes, and CPU submission).
inline double native_submission_delta_ms(std::uint64_t submit_qpc,
                                         std::uint64_t left_ready_qpc,
                                         std::uint64_t right_ready_qpc,
                                         double qpc_frequency) noexcept {
    if(!submit_qpc||!left_ready_qpc||!right_ready_qpc||!(qpc_frequency>0))return NAN;
    return (double(submit_qpc)-double(std::max(left_ready_qpc,right_ready_qpc))) * 1000. / qpc_frequency;
}
struct SignedTimingAverage {
    std::array<double,60> values{};
    unsigned count{},next{};
    double sum{},last{};
    void add(double ms) noexcept {
        if(!std::isfinite(ms))return;
        if(count==values.size())sum-=values[next];else ++count;
        values[next]=last=ms;sum+=ms;next=(next+1)%values.size();
    }
    double mean() const noexcept {return count?sum/count:0.;}
};
}
