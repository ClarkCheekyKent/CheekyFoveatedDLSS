#pragma once
#include <cstdint>

namespace cheeky::foveated_dlss {
enum class EyeCalibrationMethod : unsigned { automatic, standard, timing, full };
inline const char* eye_calibration_method_name(EyeCalibrationMethod method) {
    switch (method) {
    case EyeCalibrationMethod::standard: return "Standard corners";
    case EyeCalibrationMethod::timing: return "Timing tolerant corners";
    case EyeCalibrationMethod::full: return "Full crop search";
    default: return "Auto";
    }
}
// Counts completed usable observations, never pending GPU work or missing eyes.
struct EyeCalibrationPolicy {
    EyeCalibrationMethod configured{}, active{EyeCalibrationMethod::standard};
    unsigned failures{}, confirmations{};
    std::uint64_t started_ms{}, stage_ms{};
    bool signature_checked{};
    void begin(EyeCalibrationMethod method, std::uint64_t now) {
        *this = {};
        configured = method;
        active = method == EyeCalibrationMethod::automatic ? EyeCalibrationMethod::standard : method;
        started_ms = stage_ms = now;
    }
    void select(EyeCalibrationMethod method, std::uint64_t now) {
        active = method; failures = confirmations = 0; stage_ms = now;
    }
    void recover(std::uint64_t now) {
        select(configured == EyeCalibrationMethod::automatic ? EyeCalibrationMethod::full : configured, now);
        signature_checked = true;
    }
    bool failed(std::uint64_t now, bool inconclusive) {
        confirmations = 0;
        if (inconclusive) { failures = 0; return false; }
        ++failures;
        if (configured != EyeCalibrationMethod::automatic || active == EyeCalibrationMethod::full) return false;
        // A five-second cheap-discovery budget, but never escalate on one miss.
        if (failures >= 2 && now - started_ms >= 5000) {
            select(EyeCalibrationMethod::full, now); return true;
        }
        if (failures < 20) return false;
        if (active == EyeCalibrationMethod::standard) {
            select(EyeCalibrationMethod::timing, now); return true;
        }
        if (now - stage_ms < 1000) return false;
        select(EyeCalibrationMethod::full, now); return true;
    }
};
}
