#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdint>
namespace cheeky::foveated_dlss {
inline std::atomic<ULONGLONG> exposure_capture_deadline{};
inline std::atomic<std::uint64_t> exposure_capture_generation{};
inline std::uint64_t begin_exposure_capture(unsigned milliseconds) noexcept {
    const auto generation = ++exposure_capture_generation;
    exposure_capture_deadline.store(GetTickCount64() + milliseconds);
    return generation;
}
inline void end_exposure_capture(std::uint64_t generation) noexcept {
    if (exposure_capture_generation.load() == generation) exposure_capture_deadline.store(0);
}
}
