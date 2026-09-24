#pragma once
#include "eye_calibration_placement.hpp"
#include <memory>

namespace cheeky::foveated_dlss {
// Source proof from a foreign graphics API. poll never waits for the GPU and
// must not call back into calibration. Recordings retain their own resources
// until command reset/free, independently of the calibration frame lifetime.
struct CalibrationSourceResult {
    bool ready{}, valid{};
    std::array<float, 2> scores{};
};
struct CalibrationSourceProof {
    virtual ~CalibrationSourceProof() = default;
    virtual CalibrationSourceResult poll() noexcept = 0;
};
using CalibrationSourcePtr = std::shared_ptr<CalibrationSourceProof>;
using CalibrationSourceRecord = CalibrationSourcePtr (*)(void*, const CalibrationMarkerPoints&, unsigned);
void eye_calibration_external_source(std::uint64_t view, unsigned x, unsigned y, unsigned width,
    unsigned height, unsigned graphics_api, void* context, CalibrationSourceRecord record) noexcept;
}
