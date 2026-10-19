#pragma once
#include "foveation.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cheeky::foveated_dlss {
// Captured by the actual SR preparation, not by calling the gaze/crop solver
// again from FG. Evaluations are serialized on this intercepting thread.
struct FgSrRegion {
    std::uint64_t view{};
    unsigned width{}, height{};
    float center_u{.5f}, center_v{.5f};
    float region_width{}, region_height{};
    bool valid{};
};
inline thread_local FgSrRegion current_fg_sr_region{};
inline void fg_sr_begin_evaluation(std::uint64_t view) noexcept {
    current_fg_sr_region = {};
    current_fg_sr_region.view = view;
}
inline FgSrRegion fg_sr_placement(const FoveationGeometry& crop,
    unsigned output_width, unsigned output_height, unsigned origin_x, unsigned origin_y,
    std::uint64_t view) noexcept {
    if (!output_width || !output_height || !crop.output_width || !crop.output_height ||
        crop.output_base_x < origin_x || crop.output_base_y < origin_y ||
        crop.output_base_x - origin_x + crop.output_width > output_width ||
        crop.output_base_y - origin_y + crop.output_height > output_height) return {};
    const auto x = crop.output_base_x - origin_x;
    const auto y = crop.output_base_y - origin_y;
    return {view, output_width, output_height,
        (float(x) + float(crop.output_width)*.5f)/float(output_width),
        (float(y) + float(crop.output_height)*.5f)/float(output_height),
        float(crop.output_width)/float(output_width),
        float(crop.output_height)/float(output_height), true};
}
inline void fg_sr_prepared(std::uint64_t view, const FoveationGeometry& crop,
    unsigned output_width, unsigned output_height, unsigned origin_x, unsigned origin_y) noexcept {
    if (current_fg_sr_region.view != view) return;
    current_fg_sr_region=fg_sr_placement(crop,output_width,output_height,origin_x,origin_y,view);
}
inline FgSrRegion fg_sr_prepared_for(std::uint64_t view,unsigned width,unsigned height) noexcept {
    const auto s=current_fg_sr_region;
    return s.valid && s.view==view && s.width==width && s.height==height ? s : FgSrRegion{};
}
// When SR itself is bypassed (but FG remains enabled), honor the known stereo
// eye assignment rather than putting both FG rectangles at screen center.
inline float fg_fallback_center(float sr_size,float signed_sr_offset) noexcept {
    return .5f + .5f*(1.f-std::clamp(sr_size,0.f,1.f))*std::clamp(signed_sr_offset,-1.f,1.f);
}
}
