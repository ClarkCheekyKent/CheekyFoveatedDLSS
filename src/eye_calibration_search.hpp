#pragma once
#include "eye_calibration_placement.hpp"
#include <atomic>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

namespace cheeky::foveated_dlss {
// Acquisition works on owned CPU pixels only. No D3D objects or calibration
// state are accessed by the worker. At most one stereo acquisition is in flight.
struct CalibrationSearchTarget {
    CalibrationMarkerPoint marker;
    unsigned candidate{}, width{}, height{};
};
struct CalibrationSearchResult {
    bool valid{}, flipped{}, ambiguous{};
    unsigned candidate{};
    CalibrationPlacement placement;
    float score{};
};
struct CalibrationSearch {
    std::vector<CalibrationSearchTarget> targets;
    std::atomic<bool> started{}, ready{}, canceled{};
    CalibrationSearchResult result;
};
using CalibrationSearchPtr = std::shared_ptr<CalibrationSearch>;
struct CalibrationSearchImage {
    unsigned width{}, height{};
    double original_width{}, original_height{};
    std::vector<float> pixels;
};
inline CalibrationSearchImage calibration_search_image(const void* data, unsigned pitch,
    unsigned width, unsigned height, DXGI_FORMAT format, std::array<float, 4> bounds) {
    CalibrationSearchImage image;
    const unsigned bytes = calibration_pixel_bytes(format);
    if (!data || !bytes || !width || !height || std::uint64_t(width) * bytes > pitch) return image;
    image.original_width = width * std::abs(double(bounds[2]) - bounds[0]);
    image.original_height = height * std::abs(double(bounds[3]) - bounds[1]);
    const double scale = (std::min)(1., 2048. / (std::max)(image.original_width, image.original_height));
    image.width = unsigned(image.original_width * scale);
    image.height = unsigned(image.original_height * scale);
    image.pixels.resize(std::size_t(image.width) * image.height);
    for (unsigned y = 0; y < image.height; ++y) for (unsigned x = 0; x < image.width; ++x) {
        const auto sx = (std::min)(width - 1, unsigned((bounds[0] + (x + .5) / image.width * (bounds[2] - bounds[0])) * width));
        const auto sy = (std::min)(height - 1, unsigned((bounds[1] + (y + .5) / image.height * (bounds[3] - bounds[1])) * height));
        const auto p = calibration_decode(static_cast<const unsigned char*>(data) + std::size_t(sy) * pitch + sx * bytes, format);
        image.pixels[std::size_t(y) * image.width + x] = std::isfinite(p.r + p.g + p.b) ? (p.r + p.g + p.b) / 3 : 0;
    }
    return image;
}
inline CalibrationSearchResult calibration_search(const CalibrationSearchImage& image,
    const std::vector<CalibrationSearchTarget>& targets, const std::atomic<bool>* canceled = nullptr) {
    CalibrationSearchResult result;
    if (image.width < 10 || image.height < 10 || targets.empty()) return result;
    struct Template { unsigned target{}, flip{}; std::uint32_t code{}; };
    std::vector<Template> templates;
    // Hash complete 25-bit words (and single-bit errors), rather than running
    // every template at every pixel. Colliding words remain explicitly ambiguous.
    std::unordered_map<std::uint32_t, std::vector<unsigned>> words;
    for (unsigned t = 0; t < targets.size(); ++t) for (unsigned flip = 0; flip < 2; ++flip) {
        std::uint32_t code{};
        for (unsigned y = 0; y < 5; ++y) for (unsigned x = 0; x < 5; ++x)
            code |= unsigned(calibration_pattern_bit(targets[t].candidate, x, flip ? 4 - y : y,
                targets[t].marker.code)) << (y * 5 + x);
        const unsigned index = unsigned(templates.size());
        templates.push_back({t, flip, code});
        words[code].push_back(index);
        for (unsigned bit = 0; bit < 25; ++bit) words[code ^ (1U << bit)].push_back(index);
    }
    auto sample = [&](double x, double y) {
        return image.pixels[std::size_t(std::clamp(int(y), 0, int(image.height) - 1)) * image.width +
            std::clamp(int(x), 0, int(image.width) - 1)];
    };
    auto score = [&](double x, double y, double cw, double ch, std::uint32_t code) {
        if (x < 0 || y < 0 || x + 5 * cw > image.width || y + 5 * ch > image.height) return 0.F;
        float values[25], high{}, low{}, sum{}, square{}, dot{}, sign_sum{};
        unsigned highs{};
        for (unsigned yy = 0; yy < 5; ++yy) for (unsigned xx = 0; xx < 5; ++xx) {
            const unsigned i = yy * 5 + xx;
            const float v = (sample(x + (xx + .35) * cw, y + (yy + .35) * ch) +
                sample(x + (xx + .65) * cw, y + (yy + .35) * ch) +
                sample(x + (xx + .35) * cw, y + (yy + .65) * ch) +
                sample(x + (xx + .65) * cw, y + (yy + .65) * ch)) * .25F;
            const bool bit = (code & (1U << i)) != 0;
            const float sign = bit ? 1.F : -1.F;
            values[i] = v; sum += v; square += v * v; dot += v * sign; sign_sum += sign;
            if (bit) { high += v; ++highs; } else low += v;
        }
        if (!highs || highs == 25) return 0.F;
        high /= highs; low /= 25 - highs;
        if (high - low < .04F) return 0.F;
        unsigned correct{};
        for (unsigned i = 0; i < 25; ++i) correct += (values[i] > (high + low) * .5F) == ((code & (1U << i)) != 0);
        const float variance = square - sum * sum / 25.F;
        if (correct < 24 || variance <= 1e-6F) return 0.F;
        return std::clamp((dot - sum * sign_sum / 25.F) / std::sqrt(variance * (25 - sign_sum * sign_sum / 25.F)), 0.F, 1.F);
    };
    struct Hit { unsigned index; double x, y, cw, ch; float score; };
    std::vector<Hit> hits;
    // Covers 7.5--100 pixel patterns in the acquisition image. Both axes are
    // refined independently below; do not infer a crop from aspect ratio alone.
    for (double cell = 1.5; cell <= 20.; cell *= 1.12) {
        if (canceled && canceled->load(std::memory_order_relaxed)) return {};
        const double step = (std::max)(1., cell * .5);
        for (double aspect : {.8, 1., 1.25}) {
            const double cy = cell * aspect;
            for (double y = 0; y + 5 * cy <= image.height; y += step)
                for (double x = 0; x + 5 * cell <= image.width; x += step) {
                    float v[25], low = 1e30F, high = -1e30F;
                    for (unsigned yy = 0; yy < 5; ++yy) for (unsigned xx = 0; xx < 5; ++xx) {
                        auto& a = v[yy * 5 + xx]; a = sample(x + (xx + .5) * cell, y + (yy + .5) * cy);
                        low = (std::min)(low, a); high = (std::max)(high, a);
                    }
                    if (high - low < .04F) continue;
                    std::uint32_t code{};
                    for (unsigned i = 0; i < 25; ++i) code |= unsigned(v[i] > (high + low) * .5F) << i;
                    const auto found = words.find(code);
                    if (found == words.end()) continue;
                    for (const unsigned index : found->second) {
                        float quality = score(x, y, cell, cy, templates[index].code);
                        if (quality < calibration_pattern_min_score) continue;
                        bool duplicate{};
                        for (auto& hit : hits) if (hit.index == index && std::abs(hit.x - x) < cell * 2 && std::abs(hit.y - y) < cy * 2) {
                            if (quality > hit.score) hit = {index, x, y, cell, cy, quality};
                            duplicate = true; break;
                        }
                        if (!duplicate) {
                            if (hits.size() >= 128) { result.ambiguous = true; return result; }
                            hits.push_back({index, x, y, cell, cy, quality});
                        }
                    }
                }
        }
    }
    double best_edge = 1e30;
    for (auto hit : hits) {
        const auto& pattern = templates[hit.index];
        const auto& target = targets[pattern.target];
        // Refine independent X/Y scale and translation using the cell contrast.
        // Average the plateau of equally good fits to avoid biasing to the first
        // coarse position/scale that happens to put all samples inside cells.
        for (double radius : {.18, .06}) {
            double sx{}, sy{}, sw{}, sh{}; unsigned count{};
            float best = hit.score;
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
                for (int dh = -1; dh <= 1; ++dh) for (int dw = -1; dw <= 1; ++dw) {
                    const double cw = hit.cw * (1 + dw * radius), ch = hit.ch * (1 + dh * radius);
                    const double x = hit.x + dx * hit.cw * radius - 2.5 * (cw - hit.cw);
                    const double y = hit.y + dy * hit.ch * radius - 2.5 * (ch - hit.ch);
                    const float q = score(x, y, cw, ch, pattern.code);
                    if (q + .0001F < best) continue;
                    if (q > best + .0001F) { best = q; sx = sy = sw = sh = 0; count = 0; }
                    sx += x; sy += y; sw += cw; sh += ch; ++count;
                }
            if (count) hit = {hit.index, sx / count, sy / count, sw / count, sh / count, best};
        }
        float other{};
        hit.score = score(hit.x, hit.y, hit.cw, hit.ch, pattern.code);
        if (hit.score < calibration_pattern_min_score) continue;
        for (unsigned i = 0; i < templates.size(); ++i) if (i != hit.index)
            other = (std::max)(other, score(hit.x, hit.y, hit.cw, hit.ch, templates[i].code));
        if (hit.score - other < calibration_pattern_min_gap) continue;
        // Keep enough margin for the 60-source-pixel tracking patch. A marker
        // clipped by the submission boundary cannot provide stable tracking.
        if (hit.x < 1.25 * hit.cw || hit.y < 1.25 * hit.ch ||
            hit.x + 6.25 * hit.cw > image.width || hit.y + 6.25 * hit.ch > image.height) continue;
        if (result.valid && (result.candidate != target.candidate || result.flipped != bool(pattern.flip))) {
            result.valid = false; result.ambiguous = true; return result;
        }
        const double width = image.width * 8. / hit.cw, height = image.height * 8. / hit.ch;
        const double px = hit.x * 8. / hit.cw, py = hit.y * 8. / hit.ch;
        CalibrationPlacement placement{target.marker.x - px,
            pattern.flip ? target.marker.y + 40 - height + py : target.marker.y - py,
            width, height, target.marker};
        // Prefer a complete, decodable marker nearest an edge of the submitted
        // view. Actual headset hidden-area visibility is not available here.
        const double edge = (std::min)({hit.x / image.width, hit.y / image.height,
            (image.width - hit.x - hit.cw * 5) / image.width,
            (image.height - hit.y - hit.ch * 5) / image.height});
        if (!result.valid || edge < best_edge) {
            best_edge = edge;
            result = {true, bool(pattern.flip), false, target.candidate, placement, hit.score};
        }
    }
    return result;
}
inline void calibration_search_start(const CalibrationSearchPtr& request, CalibrationSearchImage image) noexcept {
    if (!request || request->started.exchange(true)) return;
    static std::atomic<unsigned> workers{};
    if (workers.fetch_add(1) >= 2) {
        --workers; request->ready.store(true, std::memory_order_release); return;
    }
    try {
        std::thread([request, image = std::move(image)] {
            try { request->result = calibration_search(image, request->targets, &request->canceled); } catch (...) { request->result = {}; }
            --workers;
            request->ready.store(true, std::memory_order_release);
        }).detach();
    } catch (...) { --workers; request->ready.store(true, std::memory_order_release); }
}
} // namespace cheeky::foveated_dlss
