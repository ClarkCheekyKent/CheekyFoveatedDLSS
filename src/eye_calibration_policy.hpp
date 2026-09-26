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
// Counts completed acquisition failures, never pending GPU work. Incomplete
// submitted pairs may advance Auto, but never authorize a mapping.
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
        // Losing a mapping is not evidence that corner stamps cannot work.
        // Give the new scene a fresh discovery budget, just like a manual reset.
        begin(configured, now);
        signature_checked = true;
    }
    bool incomplete(std::uint64_t now, unsigned rejection, bool inconclusive) {
        // The capture has retired: these are missing/failed eye submissions or
        // missing submitted patches, possibly with dimension/marker failures.
        // Invalid GPU readback, missing source evaluations, and failed source
        // stamp proof remain excluded from this acquisition evidence.
        constexpr unsigned missing_submission = 4U | 8U | 16U;
        constexpr unsigned allowed = missing_submission | 64U | 128U;
        if (!(rejection & missing_submission) || (rejection & ~allowed)) return false;
        return failed(now, inconclusive);
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
