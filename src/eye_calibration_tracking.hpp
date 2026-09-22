#pragma once
#include "eye_calibration_search.hpp"

namespace cheeky::foveated_dlss {
// A bounded readback around the last observation, clipped to this eye's viewport.
// Pixel coordinates are retained so recognition can update the full-eye transform.
struct CalibrationTrackingPatch {
    bool enabled{}, flip{}, reverse_x{}, reverse_y{};
    unsigned candidate{};
    CalibrationPlacement placement;
    bool hint_valid{};
    CalibrationPlacement hint;
    std::array<unsigned, 4> rect{};
    double eye_width{}, eye_height{}, offset_x{}, offset_y{}, cell_x{}, cell_y{};
};
inline CalibrationTrackingPatch calibration_tracking_patch(CalibrationPlacement placement,
    unsigned candidate, bool flip, unsigned width, unsigned height,
    float u0, float v0, float u1, float v1) {
    CalibrationTrackingPatch p;
    if (placement.width <= 0 || placement.height <= 0) return p;
    p.placement = placement; p.candidate = candidate; p.flip = flip;
    p.reverse_x = u1 < u0; p.reverse_y = v1 < v0;
    p.eye_width = width * std::abs(double(u1) - u0);
    p.eye_height = height * std::abs(double(v1) - v0);
    p.cell_x = p.eye_width * 8 / placement.width;
    p.cell_y = p.eye_height * 8 / placement.height;
    if (p.cell_x < 1.5 || p.cell_y < 1.5 || p.cell_x > 24 || p.cell_y > 24) return p;
    const double sx = (placement.marker.x - placement.x + 20) / placement.width;
    const double sy = (placement.marker.y - placement.y + 20) / placement.height;
    const double cx = width * (u0 + sx * (u1 - u0));
    const double cy = height * (v0 + (flip ? 1 - sy : sy) * (v1 - v0));
    const double x0 = width * (std::min)(u0, u1), x1 = width * (std::max)(u0, u1);
    const double y0 = height * (std::min)(v0, v1), y1 = height * (std::max)(v0, v1);
    const double left = (std::max)(std::ceil(x0), std::floor(cx - p.cell_x * 2.5 - 64));
    const double top = (std::max)(std::ceil(y0), std::floor(cy - p.cell_y * 2.5 - 64));
    const double right = (std::min)(std::floor(x1), std::ceil(cx + p.cell_x * 2.5 + 64));
    const double bottom = (std::min)(std::floor(y1), std::ceil(cy + p.cell_y * 2.5 + 64));
    if (left < 0 || top < 0 || right > width || bottom > height || right <= left || bottom <= top ||
        right - left > 256 || bottom - top > 256) return p;
    p.rect = {unsigned(left), unsigned(top), unsigned(right - left), unsigned(bottom - top)};
    p.offset_x = p.reverse_x ? x1 - right : left - x0;
    p.offset_y = p.reverse_y ? y1 - bottom : top - y0;
    p.enabled = true;
    return p;
}
inline CalibrationSearchResult calibration_track_impl(const void* data, unsigned pitch, DXGI_FORMAT format,
    const CalibrationTrackingPatch& patch, CalibrationMatchDiagnostics& diagnostics) {
    if (!patch.enabled) return {};
    const auto image = calibration_search_image(data, pitch, patch.rect[2], patch.rect[3], format,
        {patch.reverse_x ? 1.F : 0.F, patch.reverse_y ? 1.F : 0.F,
         patch.reverse_x ? 0.F : 1.F, patch.reverse_y ? 0.F : 1.F});
    CalibrationSearchOptions options;
    options.diagnostics=&diagnostics;
    options.min_cell = (std::max)(1.5, patch.cell_x * .72);
    options.max_cell = patch.cell_x * 1.4;
    options.aspect = patch.cell_y / patch.cell_x;
    options.flip_mask = 1U << unsigned(patch.flip);
    options.tracking = true;
    const std::vector<CalibrationSearchTarget> targets{{patch.placement.marker,patch.candidate,0,0}};
    options.fixed_geometry=true;
    const auto& predicted=patch.hint_valid ? patch.hint : patch.placement;
    options.expected_cw=patch.eye_width*8/predicted.width;
    options.expected_ch=patch.eye_height*8/predicted.height;
    options.expected_x=(patch.placement.marker.x-predicted.x)*patch.eye_width/predicted.width-patch.offset_x;
    options.expected_y=patch.flip ?
        (1-(patch.placement.marker.y+40-predicted.y)/predicted.height)*patch.eye_height-patch.offset_y :
        (patch.placement.marker.y-predicted.y)*patch.eye_height/predicted.height-patch.offset_y;
    unsigned path=1;
    auto result=calibration_search(image,targets,nullptr,options);
    if (!result.valid && !result.ambiguous) {
        path=2;
        options.translation_radius=32;
        result=calibration_search(image,targets,nullptr,options);
        if (!result.valid && !result.ambiguous) {
            options.translation_radius=64;
            result=calibration_search(image,targets,nullptr,options);
        }
    }
    if (!result.valid && !result.ambiguous) {
        path=3;
        options.fixed_geometry=false;
        result=calibration_search(image,targets,nullptr,options);
    }
    result.tracking_path=path;
    if (!result.valid || result.ambiguous) { result.valid=false; return result; }
    auto& p = result.placement;
    const double sx = p.width / image.width, sy = p.height / image.height;
    p.x -= patch.offset_x * sx;
    p.y -= (patch.flip ? patch.eye_height - patch.offset_y - image.height : patch.offset_y) * sy;
    p.width = patch.eye_width * sx; p.height = patch.eye_height * sy;
    return result;
}
// Time every attempted patch, including failed recognition, without per-call logging.
inline CalibrationSearchResult calibration_track(const void* data, unsigned pitch, DXGI_FORMAT format,
    const CalibrationTrackingPatch& patch) {
    const auto start=calibration_clock_ms();
    CalibrationMatchDiagnostics diagnostics;
    auto result=calibration_track_impl(data,pitch,format,patch,diagnostics);
    result.match_diagnostics=diagnostics;
    result.tracking_cpu_ms=calibration_clock_ms()-start;
    return result;
}
// Capture only owned bytes on the graphics thread; no mapped memory or D3D
// objects are retained by workers. The frame keeps the job alive until publication.
struct CalibrationTrackingInput {
    std::vector<unsigned char> bytes;
    unsigned pitch{};
    DXGI_FORMAT format{};
    CalibrationTrackingPatch patch;
};
using CalibrationTrackingInputPtr=std::shared_ptr<CalibrationTrackingInput>;
inline CalibrationTrackingInputPtr calibration_tracking_copy(const void* data,unsigned pitch,DXGI_FORMAT format,
    const CalibrationTrackingPatch& patch) noexcept {
    try {
        const auto bytes=calibration_pixel_bytes(format);
        if (!data || !patch.enabled || !bytes || !patch.rect[2] || !patch.rect[3] ||
            patch.rect[2]>256 || patch.rect[3]>256 || pitch<patch.rect[2]*bytes) return {};
        auto input=std::make_shared<CalibrationTrackingInput>();
        input->pitch=patch.rect[2]*bytes; input->format=format; input->patch=patch;
        input->bytes.resize(std::size_t(input->pitch)*patch.rect[3]);
        for(unsigned y=0;y<patch.rect[3];++y)
            std::memcpy(input->bytes.data()+std::size_t(y)*input->pitch,
                static_cast<const unsigned char*>(data)+std::size_t(y)*pitch,input->pitch);
        return input;
    } catch (...) { return {}; }
}
struct CalibrationVerificationJob {
    std::array<CalibrationTrackingInputPtr,calibration_patch_count> inputs;
    std::array<CalibrationSearchResult,calibration_patch_count> results;
    std::atomic<bool> ready{}, canceled{};
    bool started{}; // Accessed only under the calibration state mutex.
};
inline bool calibration_verification_start(const std::shared_ptr<CalibrationVerificationJob>& job) noexcept {
    if (job->started) return true;
    static std::atomic<unsigned> workers{};
    if (workers.fetch_add(1)>=2) { --workers; return false; }
    job->started=true;
    try {
        std::thread([job] {
            try {
                for(unsigned i=0;i<job->inputs.size() && !job->canceled.load();++i)
                    if (const auto& input=job->inputs[i])
                        job->results[i]=calibration_track(input->bytes.data(),input->pitch,input->format,input->patch);
            } catch (...) { job->results={}; }
            --workers;
            job->ready.store(true,std::memory_order_release);
        }).detach();
    } catch (...) { --workers; job->ready.store(true,std::memory_order_release); }
    return true;
}
} // namespace cheeky::foveated_dlss
