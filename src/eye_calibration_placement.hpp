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
    bool locator{};
    bool operator==(const CalibrationMarkerPoint&) const = default;
};
struct CalibrationPlacement {
    double x{}, y{}, width{}, height{};
    CalibrationMarkerPoint marker;
};
struct CalibrationPlacementPlan {
    std::array<CalibrationPlacement, calibration_placement_count> placements{};
    unsigned count{1};
    bool per_eye{};
    std::array<CalibrationPlacement, 2> eye_placements{};
    const CalibrationPlacement& at(unsigned i) const { return placements[i < count ? i : 0]; }
    const CalibrationPlacement& for_eye(unsigned eye, unsigned i) const { return per_eye ? eye_placements[eye] : at(i); }
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
// Spatially unique 5x5 codes distributed over the source. Acquisition requires
// several decoded locations; a repeating checkerboard cannot identify a crop.
inline CalibrationPlacementPlan calibration_grid_plan(unsigned width, unsigned height) {
    CalibrationPlacementPlan plan;
    plan.count = 0;
    if (width < 328 || height < 328) {
        plan.count = 1; plan.placements[0] = {0, 0, double(width), double(height), {12, 12}};
        return plan; // Too small for a spatially verified crop; acquisition stays unknown.
    }
    for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x) {
        const unsigned px = 36 + unsigned((width - 112.) * x / 3.);
        const unsigned py = 36 + unsigned((height - 112.) * y / 3.);
        plan.placements[plan.count++] = {0, 0, double(width), double(height), {px, py, 0, true}};
    }
    return plan;
}
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
    if (plan.per_eye) for (const auto& placement : plan.eye_placements) {
        auto p = placement.marker;
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
        plan.placements[i] = {x, y, rw, rh, {px, py, 0, true}};
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
// Rank distance to a corner, not merely distance to any one edge. A marker
// halfway across the top edge must not beat a slightly inset corner marker.
inline double calibration_corner_distance(double x, double y, double w, double h,
                                           double marker_w = 40, double marker_h = 40) {
    if (w <= 0 || h <= 0) return 1e30;
    const double dx = (std::min)(x, w - x - marker_w) / w;
    const double dy = (std::min)(y, h - y - marker_h) / h;
    return dx * dx + dy * dy;
}
inline CalibrationPlacement calibration_padded_corner(CalibrationPlacement p, unsigned width,
    unsigned height, unsigned candidate, double submitted_width, double submitted_height) {
    p.marker.locator = false;
    // Padding is measured in the submitted eye, then mapped into source pixels.
    // Rebuild from the learned crop boundary, not from the last inset marker:
    // repeated reacquisition must not walk the marker farther into the view.
    const double px = (std::max)(12., submitted_width > 0 ? 20. * p.width / submitted_width : 20.);
    const double py = (std::max)(12., submitted_height > 0 ? 20. * p.height / submitted_height : 20.);
    p.marker.x = unsigned(std::clamp(candidate ? std::floor(p.x + p.width - 40 - px) :
        std::ceil(p.x + px), 0., double(width - 40)));
    p.marker.y = unsigned(std::clamp(std::ceil(p.y + py), 0., double(height - 40)));
    return p;
}
inline CalibrationPlacementPlan calibration_corner_pair(CalibrationPlacement outer,
                                                        unsigned width, unsigned height, unsigned candidate) {
    CalibrationPlacementPlan plan;
    plan.placements[0] = outer;
    // 40px code + 8px gap: close enough to remain near the same corner without
    // overwriting either code. The inset protects against clipping/reprojection.
    auto inner = outer;
    const int x = int(outer.marker.x) + (candidate ? -48 : 48);
    const unsigned y = outer.marker.y + 48;
    if (x >= 0 && unsigned(x) + 40 <= width && y + 40 <= height &&
        y + 40 < height / 2) {
        inner.marker.x = unsigned(x); inner.marker.y = y;
        plan.placements[1] = inner;
        plan.count = 2;
    }
    return plan;
}
// Compare observed marker endpoints with a hypothesis in submitted pixels.
// Extrapolating a 40px marker's fitted scale to the far crop edge would
// amplify one pixel of recognition noise into ~40px of error.
struct CalibrationMarkerFitError { double x{}, y{}; };
inline CalibrationMarkerFitError calibration_marker_fit_error(const CalibrationPlacement& expected,
    const CalibrationPlacement& observed, double submitted_width, double submitted_height) {
    const auto error = [](double marker, double origin, double extent,
        double expected_origin, double expected_extent, double pixels) {
        return std::abs((marker - origin) / extent - (marker - expected_origin) / expected_extent) * pixels;
    };
    CalibrationMarkerFitError result;
    for (const double offset : {0., 40.}) {
        result.x = (std::max)(result.x, error(expected.marker.x + offset, observed.x, observed.width,
            expected.x, expected.width, submitted_width));
        result.y = (std::max)(result.y, error(expected.marker.y + offset, observed.y, observed.height,
            expected.y, expected.height, submitted_height));
    }
    return result;
}
// Corner probes hypothesize the full canvas, which shares its corner marker
// with any native-pixel crop anchored at that edge (for example UEVR
// projection cropping). When the tracked marker scale decisively contradicts
// the hypothesis and instead proves 1:1 pixels, return that native crop. Its
// size is the exact submitted size; its origin comes from the observed marker
// position, not the coarser fitted scale, and snaps to the source edge.
inline bool calibration_native_crop(const CalibrationPlacement& expected, const CalibrationPlacement& observed,
    unsigned width, unsigned height, double submitted_width, double submitted_height, CalibrationPlacement& out) {
    constexpr double scale_tolerance = .05, edge_snap = 3;
    const double w = width, h = height;
    const double sw = std::round(submitted_width), sh = std::round(submitted_height);
    if (!(expected.width > 0 && expected.height > 0 && observed.width > 0 && observed.height > 0) ||
        !(sw >= 1 && sh >= 1 && sw <= w && sh <= h) || (sw == w && sh == h)) return false;
    const double mx = observed.width / sw, my = observed.height / sh;
    const double native_x = std::abs(mx - 1), native_y = std::abs(my - 1);
    if (native_x > scale_tolerance || native_y > scale_tolerance) return false;
    const double gap = (std::max)(std::abs(mx - expected.width / sw) - native_x,
                                  std::abs(my - expected.height / sh) - native_y);
    if (gap < 2 * scale_tolerance) return false;
    const double marker_x = expected.marker.x, marker_y = expected.marker.y;
    const auto snap = [&](double v, double limit) {
        if (std::abs(v) <= edge_snap) return 0.;
        if (std::abs(v - limit) <= edge_snap) return limit;
        return std::round(v);
    };
    const double x = snap(marker_x - (marker_x - observed.x) * sw / observed.width, w - sw);
    const double y = snap(marker_y - (marker_y - observed.y) * sh / observed.height, h - sh);
    if (x < 0 || y < 0 || x > w - sw || y > h - sh ||
        marker_x < x || marker_x + 40 > x + sw || marker_y < y || marker_y + 40 > y + sh) return false;
    out = expected;
    out.x = x; out.y = y; out.width = sw; out.height = sh;
    return true;
}
// A grid/full-search fit measures a native-pixel crop with sub-pixel error
// (for example x=-0.3 or width=2041.2 for a 2040px eye). FG requires an exact
// 1:1 crop inside the source, so near-native fits become the submitted size
// at an integer origin. Scaled fits are left untouched.
inline bool calibration_snap_native_extent(CalibrationPlacement& p, unsigned width, unsigned height,
    double submitted_width, double submitted_height) {
    constexpr double size_tolerance = 3, edge_snap = 3;
    const double w = width, h = height;
    const double sw = std::round(submitted_width), sh = std::round(submitted_height);
    if (!(sw >= 1 && sh >= 1 && sw <= w && sh <= h) ||
        std::abs(p.width - sw) > size_tolerance || std::abs(p.height - sh) > size_tolerance) return false;
    // Snap to a source edge the fit touches; otherwise keep the fitted center.
    const auto snap = [&](double origin, double extent, double size, double canvas) {
        if (std::abs(origin) <= edge_snap) return 0.;
        if (std::abs(origin + extent - canvas) <= edge_snap) return canvas - size;
        return std::clamp(std::round(origin + (extent - size) * .5), 0., canvas - size);
    };
    p.x = snap(p.x, p.width, sw, w);
    p.y = snap(p.y, p.height, sh, h);
    p.width = sw; p.height = sh;
    return true;
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
