#pragma once
#include "eye_calibration_pixels.hpp"
#include <span>
#include <bit>

namespace cheeky::foveated_dlss {
// A bounded hypothesis bank, not an inference from aspect ratio alone. Every
// hypothesis must still pass source proof and both physical-eye marker checks.
inline constexpr unsigned calibration_placement_count = 16;
inline constexpr unsigned calibration_patch_count = 4 + 8 * calibration_placement_count;
inline constexpr unsigned calibration_box_count = 4 * calibration_placement_count;
struct CalibrationMarkerPoint {
    unsigned x{}, y{};
    std::uint32_t code{};
    bool operator==(const CalibrationMarkerPoint&) const = default;
};
struct CalibrationPlacement {
    double x{}, y{}, width{}, height{};
    CalibrationMarkerPoint marker;
};
struct CalibrationPlacementPlan {
    std::array<CalibrationPlacement, calibration_placement_count> placements{};
    unsigned count{1};
    const CalibrationPlacement& at(unsigned i) const { return placements[i < count ? i : 0]; }
};
struct CalibrationMarkerPoints {
    std::array<CalibrationMarkerPoint, calibration_placement_count> points{};
    unsigned count{};
    std::span<const CalibrationMarkerPoint> extra() const {
        if (!count) return {};
        return {points.data() + 1, count - 1};
    }
    bool operator==(const CalibrationMarkerPoints&) const = default;
};
inline std::uint32_t calibration_location_code(CalibrationMarkerPoint p, unsigned width,
                                               unsigned candidate, std::uint32_t base) {
    if (p.x == (candidate ? width - 52 : 12) && p.y == 12) return base;
    std::uint64_t seed = (std::uint64_t(p.x) << 32) ^ p.y ^ (std::uint64_t(base) << 17) ^
        (candidate ? 0x934de173bd734ae9ULL : 0x6a09e667f3bcc909ULL);
    for (;;) {
        seed += 0x9e3779b97f4a7c15ULL;
        auto v = seed;
        v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
        v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
        const auto code = std::uint32_t((v ^ (v >> 31)) & 0x1ffffffU);
        if (std::popcount(code) >= 10 && std::popcount(code) <= 15) return code;
    }
}
inline CalibrationMarkerPoints calibration_marker_points(const CalibrationPlacementPlan& plan, unsigned x, unsigned y,
                                                          unsigned width, unsigned candidate, std::uint32_t base) {
    CalibrationMarkerPoints result;
    for (unsigned i = 0; i < plan.count; ++i) {
        auto p = plan.at(i).marker;
        p.code = calibration_location_code(p, width, candidate, base);
        p.x += x; p.y += y;
        if (std::find(result.points.begin(), result.points.begin() + result.count, p) == result.points.begin() + result.count)
            result.points[result.count++] = p;
    }
    return result;
}
inline CalibrationPlacementPlan calibration_placement_plan(unsigned width, unsigned height, unsigned candidate,
                                                           double submitted_width, double submitted_height) {
    CalibrationPlacementPlan plan;
    if (width < 64 || height < 64) return plan;
    const double w = width, h = height;
    const CalibrationPlacement full{0, 0, w, h, {candidate ? width - 52 : 12, 12}};
    plan.placements.fill(full);
    // Tiny images cannot accommodate separated top/bottom observations.
    if (width < 160 || height < 160) return plan;
    plan.count = calibration_placement_count;
    const double aspect = submitted_width > 0 && submitted_height > 0 ? submitted_width / submitted_height : w / h;
    const double cw = (std::min)(w, h * aspect), ch = (std::min)(h, w / aspect);
    const double nw = submitted_width > 0 ? (std::min)(w, submitted_width) : w;
    const double nh = submitted_height > 0 ? (std::min)(h, submitted_height) : h;
    auto add = [&](unsigned i, double rw, double rh, double ax = .5, double ay = .5) {
        const double x = (w - rw) * ax, y = (h - rh) * ay;
        if (rw < 104 || rh < 104) return;
        // A common 48px lattice per source prevents partial overlaps between
        // hypotheses. Identical positions are stamped only once. Keep the
        // marker strictly in the upper half to disambiguate vertical flips.
        const unsigned px = candidate ? width - 52 - unsigned(std::ceil((w - x - rw) / 48)) * 48
                                      : 12 + unsigned(std::ceil(x / 48)) * 48;
        const unsigned py = 12 + unsigned(std::ceil(y / 48)) * 48;
        if (px < x + 10 || px + 50 > x + rw || py < y + 10 || py + 50 > y + rh || py + 40 >= y + rh * .5) return;
        plan.placements[i] = {x, y, rw, rh, {px, py}};
    };
    add(1, cw, ch); // Aspect-preserving crop followed by any uniform resize.
    add(2, nw, nh); // Native-pixel crop.
    add(3, (std::min)(w, h), (std::min)(w, h));
    add(4, cw * .9, ch * .9); add(5, cw * .75, ch * .75); add(6, cw * .5, ch * .5);
    add(7, cw, ch, 0); add(8, cw, ch, 1); add(9, cw, ch, .5, 0); add(10, cw, ch, .5, 1);
    add(11, nw, nh, 0); add(12, nw, nh, 1); add(13, nw, nh, .5, 0); add(14, nw, nh, .5, 1);
    add(15, w * .5, h * .5);
    return plan;
}
inline unsigned calibration_patch_index(unsigned box, unsigned eye) {
    return 4 + (box / 4) * 8 + ((box % 4) / 2) * 4 + eye * 2 + box % 2;
}
// Express the source marker in the hypothesized visible source rectangle,
// then apply actual submitted UV bounds (including reversed bounds).
inline std::array<unsigned, 4> calibration_sample_rect(const CalibrationPlacement& p, bool flip,
    unsigned width, unsigned height, float u0, float v0, float u1, float v1) {
    if (p.width <= 0 || p.height <= 0) return {};
    const double nx = (p.marker.x - p.x + calibration_sample_margin) / p.width;
    const double sy = flip ? p.y + p.height - (p.marker.y - p.y) - calibration_marker_size : p.marker.y;
    const double ny = (sy - p.y + calibration_sample_margin) / p.height;
    const double ax = (u0 + nx * (u1 - u0)) * width;
    const double bx = (u0 + (nx + calibration_sample_size / p.width) * (u1 - u0)) * width;
    const double ay = (v0 + ny * (v1 - v0)) * height;
    const double by = (v0 + (ny + calibration_sample_size / p.height) * (v1 - v0)) * height;
    const double left = std::floor((std::min)(ax, bx)), top = std::floor((std::min)(ay, by));
    const double right = std::ceil((std::max)(ax, bx)), bottom = std::ceil((std::max)(ay, by));
    if (left < 0 || top < 0 || right > width || bottom > height || right <= left || bottom <= top ||
        right - left > 256 || bottom - top > 256) return {};
    return {unsigned(left), unsigned(top), unsigned(right - left), unsigned(bottom - top)};
}
} // namespace cheeky::foveated_dlss
