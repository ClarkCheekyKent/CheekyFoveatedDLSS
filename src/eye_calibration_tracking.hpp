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
// Correlate every pixel of an unflipped fit with its rendered 5x5 code, at the
// best sub-cell offset. Unlike the 4-sample bit decode, this exposes cell edges
// misaligned by a wrong scale; the search's coarse translation is forgiven.
inline float calibration_dense_fit(const CalibrationSearchImage& image, const CalibrationSearchResult& fit) {
    const auto& p = fit.placement;
    const double cw = image.width * 8. / p.width, ch = image.height * 8. / p.height;
    const double fx = (p.marker.x - p.x) * cw / 8, fy = (p.marker.y - p.y) * ch / 8;
    const int rx = int(cw * .5), ry = int(ch * .5);
    float best{};
    for (int oy = -ry; oy <= ry; ++oy) for (int ox = -rx; ox <= rx; ++ox) {
        const double x0 = fx + ox, y0 = fy + oy;
        double sum{}, square{}, dot{}, sign_sum{}, count{};
        for (int y = (std::max)(0, int(std::ceil(y0))); y < (std::min)(int(image.height), int(y0 + 5 * ch)); ++y)
            for (int x = (std::max)(0, int(std::ceil(x0))); x < (std::min)(int(image.width), int(x0 + 5 * cw)); ++x) {
                const unsigned cx = (std::min)(4U, unsigned((x + .5 - x0) / cw)), cy = (std::min)(4U, unsigned((y + .5 - y0) / ch));
                const double v = image.sample(unsigned(x), unsigned(y));
                const double sign = calibration_pattern_bit(fit.candidate, cx, cy, p.marker.code) ? 1. : -1.;
                sum += v; square += v * v; dot += v * sign; sign_sum += sign; ++count;
            }
        // Clipped boxes compare fewer pixels; require most of the marker.
        if (count < 20 * cw * ch) continue;
        const double variance = square - sum * sum / count, signs = count - sign_sum * sign_sum / count;
        if (variance > 1e-9 && signs > 0) best = (std::max)(best, float((dot - sum * sign_sum / count) / std::sqrt(variance * signs)));
    }
    return best;
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
    // A corner code can also decode at the compressed full-canvas scale.
    // Test native pixels explicitly rather than trusting that coarse fit.
    if (!patch.flip && patch.eye_width <= patch.placement.width && patch.eye_height <= patch.placement.height &&
        (patch.placement.width / patch.eye_width > 1.1 || patch.placement.height / patch.eye_height > 1.1)) {
        auto fixed_options = options;
        fixed_options.fixed_geometry = true;
        fixed_options.translation_radius = 32;
        // Bit decoding tolerates about +-7% scale error and brightness gradients
        // (vignette, fog, grading) can tip its score toward a wrong scale, so
        // choose between native, hypothesized and tracked fits by how well every
        // pixel matches the rendered code. A free-scale fit (path 3) means the
        // hypothesized scale already failed; it is only kept as a last resort.
        CalibrationSearchResult best;
        float best_fit = -1;
        const auto consider = [&](const CalibrationSearchResult& candidate) {
            if (!candidate.valid || candidate.ambiguous || candidate.score < calibration_pattern_min_score) return;
            const float fit = calibration_dense_fit(image, candidate);
            if (fit > best_fit) { best = candidate; best_fit = fit; }
        };
        if (path != 3) consider(result);
        fixed_options.expected_cw = fixed_options.expected_ch = 8;
        consider(calibration_search(image, targets, nullptr, fixed_options));
        // A hint may predict native pixels; still test the hypothesized scale.
        if (patch.hint_valid) {
            fixed_options.expected_cw = patch.cell_x;
            fixed_options.expected_ch = patch.cell_y;
            consider(calibration_search(image, targets, nullptr, fixed_options));
        }
        if (best_fit >= 0) { result = best; path = 2; }
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
