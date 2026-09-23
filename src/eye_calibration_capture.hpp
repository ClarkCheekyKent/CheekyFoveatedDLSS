#pragma once
#include "eye_calibration_placement.hpp"
#include "support_zip.hpp"
#include "exposure_capture.hpp"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <sstream>
#include <locale>

namespace cheeky::foveated_dlss {
// Support images are requested once, never continuously. Source slots are
// candidates, not trusted eyes: assignment is precisely what may be broken.
struct CalibrationImageInfo {
    unsigned width{}, height{}, slice{}, graphics_api{};
    DXGI_FORMAT format{};
    std::uint64_t view{};
    int prior_eye{-1}, submitted_eye{-1};
    std::array<unsigned, 4> view_rect{}, marker_rect{};
    std::array<float, 4> bounds{0, 0, 1, 1};
    std::array<std::array<unsigned, 4>, calibration_box_count> sample_rects{};
    CalibrationMarkerPoints markers;
    unsigned sample_count{};
};
struct CalibrationImage {
    CalibrationImageInfo info;
    const char* status{"not_observed"};
    unsigned preview_width{}, preview_height{};
    std::string bitmap;
};
struct CalibrationImageRequest {
    std::mutex mutex;
    std::condition_variable changed;
    std::array<CalibrationImage, 4> images;
    std::uint64_t sequence{}, session_generation{};
    std::uint64_t exposure_generation{};
    std::array<std::uint32_t, 2> marker_codes{};
    bool claimed{}, closed{};
    bool shared_source{};
    const char* status{"waiting_for_calibration_frame"};
    std::chrono::steady_clock::time_point deadline;
};
using CalibrationImageRequestPtr = std::shared_ptr<CalibrationImageRequest>;
inline std::atomic<CalibrationImageRequestPtr>& calibration_image_request_slot() {
    static auto* slot = new std::atomic<CalibrationImageRequestPtr>;
    return *slot;
}
inline CalibrationImageRequestPtr request_calibration_images(bool enabled,
    std::chrono::milliseconds timeout = std::chrono::seconds(5), const char* unavailable = "calibration_disabled") {
    auto request = std::make_shared<CalibrationImageRequest>();
    request->deadline = std::chrono::steady_clock::now() + timeout;
    request->exposure_generation = begin_exposure_capture(unsigned(std::clamp<long long>(timeout.count(), 0, 5000)));
    if (!enabled) { request->closed = true; request->status = unavailable; }
    calibration_image_request_slot().store(request);
    return request;
}
inline CalibrationImageRequestPtr claim_calibration_images(std::uint64_t sequence,
    std::uint64_t session, std::array<std::uint32_t, 2> codes) {
    auto request = calibration_image_request_slot().load();
    if (!request) return {};
    std::lock_guard lock(request->mutex);
    if (request->closed || request->claimed || std::chrono::steady_clock::now() >= request->deadline) return {};
    request->claimed = true; request->sequence = sequence;
    request->session_generation = session; request->marker_codes = codes;
    request->status = "capturing";
    return request;
}
inline bool begin_calibration_image(const CalibrationImageRequestPtr& request, unsigned index,
    const CalibrationImageInfo& info) noexcept {
    if (!request || index >= 4) return false;
    std::lock_guard lock(request->mutex);
    auto& image = request->images[index];
    if (request->closed || std::chrono::steady_clock::now() >= request->deadline ||
        std::strcmp(image.status, "not_observed")) return false;
    image.info = info; image.status = "waiting_for_gpu";
    return true;
}
inline void fail_calibration_image(const CalibrationImageRequestPtr& request, unsigned index,
    const char* reason) noexcept {
    if (!request || index >= 4) return;
    std::lock_guard lock(request->mutex);
    if (!request->closed && request->images[index].bitmap.empty()) request->images[index].status = reason;
    request->changed.notify_all();
}
// Readback allocations are bounded across requests, including timed-out GPU
// recordings which must remain alive until reset and fence completion.
inline std::atomic<std::uint64_t> calibration_image_bytes{};
struct CalibrationImageMemory {
    std::uint64_t bytes{};
    explicit CalibrationImageMemory(std::uint64_t size) : bytes(size) {}
    ~CalibrationImageMemory() { calibration_image_bytes.fetch_sub(bytes); }
};
inline std::shared_ptr<CalibrationImageMemory> reserve_calibration_image_memory(std::uint64_t bytes) {
    constexpr std::uint64_t per_image = 128ULL * 1024 * 1024, total = 512ULL * 1024 * 1024;
    if (!bytes || bytes > per_image) return {};
    auto used = calibration_image_bytes.load();
    do { if (used > total - bytes) return {}; }
    while (!calibration_image_bytes.compare_exchange_weak(used, used + bytes));
    try { return std::make_shared<CalibrationImageMemory>(bytes); }
    catch (...) { calibration_image_bytes.fetch_sub(bytes); throw; }
}
inline void complete_calibration_image(const CalibrationImageRequestPtr& request, unsigned index,
    const void* data, unsigned pitch) noexcept {
    if (!request || index >= 4 || !data) return;
    try {
        CalibrationImageInfo info;
        { std::lock_guard lock(request->mutex);
          if (request->closed || !request->images[index].bitmap.empty()) return;
          info = request->images[index].info; }
        const auto bytes = calibration_pixel_bytes(info.format);
        if (!bytes || !info.width || !info.height || std::uint64_t(info.width) * bytes > pitch) {
            fail_calibration_image(request, index, "unsupported_pixel_layout"); return;
        }
        // Lossless 24-bit BMP needs no codec on the render thread. The preview
        // is nearest-neighbor and preserves the entire texture/aspect ratio.
        // At most 300k pixels, plus row padding/header: strictly < 1,000,000 B.
        const double scale = (std::min)({1.0, 1024.0 / (std::max)(info.width, info.height),
            std::sqrt(300000.0 / (double(info.width) * info.height))});
        const auto width = (std::max)(1U, unsigned(info.width * scale));
        const auto height = (std::max)(1U, unsigned(info.height * scale));
        const auto stride = (width * 3 + 3) & ~3U;
        std::string bitmap(54 + std::size_t(stride) * height, '\0');
        const auto u16 = [&](unsigned at, unsigned value) { bitmap[at] = char(value); bitmap[at + 1] = char(value >> 8); };
        const auto u32 = [&](unsigned at, std::uint32_t value) { u16(at, value); u16(at + 2, value >> 16); };
        bitmap[0] = 'B'; bitmap[1] = 'M'; u32(2, unsigned(bitmap.size())); u32(10, 54);
        u32(14, 40); u32(18, width); u32(22, height); u16(26, 1); u16(28, 24);
        u32(34, stride * height);
        const auto channel = [](float value) {
            if (!std::isfinite(value)) value = 0;
            return char(unsigned(std::clamp(value, 0.0F, 1.0F) * 255.0F + .5F));
        };
        for (unsigned y = 0; y < height; ++y) {
            const auto sy = unsigned(std::uint64_t(y) * info.height / height);
            auto* dst = bitmap.data() + 54 + std::size_t(height - 1 - y) * stride;
            const auto* src = static_cast<const unsigned char*>(data) + std::size_t(sy) * pitch;
            for (unsigned x = 0; x < width; ++x) {
                const auto sx = unsigned(std::uint64_t(x) * info.width / width);
                const auto p = calibration_decode(src + std::size_t(sx) * bytes, info.format);
                dst[x * 3] = channel(p.b); dst[x * 3 + 1] = channel(p.g); dst[x * 3 + 2] = channel(p.r);
            }
        }
        if (bitmap.size() >= 1000000) { fail_calibration_image(request, index, "preview_size_limit"); return; }
        std::lock_guard lock(request->mutex);
        if (request->closed) return;
        auto& image = request->images[index];
        image.bitmap = std::move(bitmap); image.preview_width = width; image.preview_height = height;
        image.status = "captured";
        request->changed.notify_all();
    } catch (...) { fail_calibration_image(request, index, "preview_conversion_failed"); }
}
inline void calibration_image_physical_eye(const CalibrationImageRequestPtr& request,
    unsigned index, unsigned eye) noexcept {
    if (!request || index >= 4 || eye >= 2) return;
    std::lock_guard lock(request->mutex);
    if (!request->closed) request->images[index].info.submitted_eye = int(eye);
}
struct CalibrationImageReport { std::string diagnostics; std::vector<SupportFile> files; };
inline void calibration_image_shared_source(const CalibrationImageRequestPtr& request, unsigned candidate) noexcept {
    if (!request || candidate >= 2) return;
    std::lock_guard lock(request->mutex);
    if (request->closed) return;
    request->shared_source = true;
    request->images[1 - candidate].status = "not_applicable_single_source";
    request->changed.notify_all();
}
inline CalibrationImageReport collect_calibration_images(const CalibrationImageRequestPtr& request) {
    CalibrationImageReport result;
    std::unique_lock lock(request->mutex);
    const auto done = [&] {
        return request->closed || std::all_of(request->images.begin(), request->images.end(), [](const auto& image) {
            return std::strcmp(image.status, "not_observed") && std::strcmp(image.status, "waiting_for_gpu");
        });
    };
    request->changed.wait_until(lock, request->deadline, done); // Support worker only; never wait on rendering.
    end_exposure_capture(request->exposure_generation);
    if (!request->closed) {
        const auto captured = std::count_if(request->images.begin(), request->images.end(),
            [](const auto& image) { return !image.bitmap.empty(); });
        request->status = captured == (request->shared_source ? 3 : 4) ? "complete" : captured ? "partial" : "unavailable";
        request->closed = true;
    }
    for (auto& image : request->images) {
        if (!std::strcmp(image.status, "not_observed"))
            image.status = request->claimed ? "not_observed_in_sample" : request->status;
        else if (!std::strcmp(image.status, "waiting_for_gpu"))
            image.status = "gpu_readback_timeout";
    }
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::boolalpha;
    out << "{\"status\":\"" << request->status << "\",\"shared_source\":" << request->shared_source
        << ",\"sequence\":" << request->sequence
        << ",\"session_generation\":" << request->session_generation
        << ",\"marker_codes\":[" << request->marker_codes[0] << ',' << request->marker_codes[1]
        << "],\"preview\":\"full texture; nearest-neighbor; RGB clamped to [0,1]; not a color-accurate HDR screenshot\","
        << "\"source_labels\":\"candidate A/B are not confirmed left/right\",\"images\":[";
    const char* names[]{"stereo-source-A.bmp", "stereo-source-B.bmp", "stereo-submitted-0.bmp", "stereo-submitted-1.bmp"};
    for (unsigned i = 0; i < 4; ++i) {
        const auto& image = request->images[i]; const auto& info = image.info;
        if (i) out << ',';
        out << "{\"file\":\"" << (image.bitmap.empty() ? "" : names[i]) << "\",\"slot\":" << i
            << ",\"stage\":\"" << (i < 2 ? "after_marker_stamp" : "at_marker_read")
            << "\",\"status\":\"" << image.status << "\",\"original_width\":" << info.width
            << ",\"original_height\":" << info.height << ",\"preview_width\":" << image.preview_width
            << ",\"preview_height\":" << image.preview_height << ",\"bytes\":" << image.bitmap.size()
            << ",\"dxgi_format\":" << unsigned(info.format) << ",\"graphics_api\":" << info.graphics_api
            << ",\"array_slice\":" << info.slice << ",\"dlss_view\":\"" << info.view
            << "\",\"prior_eye_assignment\":" << info.prior_eye << ",\"submitted_eye\":" << info.submitted_eye;
        auto array = [&](const char* key, const auto& values) {
            out << ",\"" << key << "\":[";
            for (unsigned n = 0; n < values.size(); ++n) { if (n) out << ','; out << values[n]; } out << ']';
        };
        array("view_rect_xywh", info.view_rect); array("marker_rect_xywh", info.marker_rect);
        out << ",\"marker_positions_xy\":[";
        for (unsigned n = 0; n < info.markers.count; ++n) {
            if (n) out << ',';
            out << '[' << info.markers.points[n].x << ',' << info.markers.points[n].y << ']';
        }
        out << ']';
        out << ",\"marker_location_codes\":[";
        for (unsigned n = 0; n < info.markers.count; ++n) {
            if (n) out << ',';
            out << info.markers.points[n].code;
        }
        out << ']';
        array("submitted_uv_bounds", info.bounds);
        out << ",\"sample_rects_xywh\":[";
        for (unsigned n = 0; n < info.sample_count; ++n) {
            if (n) out << ','; out << '[';
            for (unsigned j = 0; j < 4; ++j) { if (j) out << ','; out << info.sample_rects[n][j]; } out << ']';
        }
        out << "]}";
        if (!image.bitmap.empty()) result.files.push_back({names[i], image.bitmap});
    }
    out << "]}"; result.diagnostics = out.str();
    result.files.push_back({"stereo-capture.json", result.diagnostics});
    auto expected = request; calibration_image_request_slot().compare_exchange_strong(expected, {});
    return result;
}
}
