#pragma once
#include "eye_calibration_placement.hpp"
#include "eye_calibration_capture.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
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
    unsigned support_points{};
    double tracking_cpu_ms{};
    unsigned tracking_path{};
};
inline double calibration_clock_ms() {
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct CalibrationSearch {
    std::vector<CalibrationSearchTarget> targets;
    std::atomic<bool> started{}, ready{}, canceled{};
    CalibrationSearchResult result;
    bool require_grid{};
    bool timed{};
    double capture_started_ms{}, copy_recorded_ms{}, setup_ms{-1}, map_cpu_ms{}, readback_wall_ms{-1}, capture_total_ms{-1};
    double elapsed_ms{}, conversion_ms{}, sampled_min{}, sampled_max{}, sampled_mean{};
    unsigned image_width{}, image_height{}, sample_count{}, sampled_black{};
    unsigned initial_locator_factor{}, last_locator_factor{}, locator_passes{};
    bool search_budget_exhausted{};
    std::uint64_t copy_issued_ms{}, readback_ready_ms{};
    unsigned map_polls{};
    long map_result{};
    std::uintptr_t texture_identity{};
};
using CalibrationSearchPtr = std::shared_ptr<CalibrationSearch>;
struct CalibrationSearchImage {
    unsigned width{}, height{};
    double original_width{}, original_height{};
    std::vector<float> pixels;
    double conversion_ms{};
    std::shared_ptr<unsigned char[]> raw;
    std::shared_ptr<CalibrationImageMemory> raw_memory;
    unsigned raw_width{}, raw_height{}, raw_pitch{}, raw_bytes{};
    DXGI_FORMAT raw_format{};
    std::array<float,4> raw_bounds{};
    float sample(unsigned x, unsigned y) const {
        if (!raw) return pixels[std::size_t(y)*width+x];
        const auto sx = unsigned(std::clamp((raw_bounds[0]+(x+.5)/width*(raw_bounds[2]-raw_bounds[0]))*raw_width,0.,double(raw_width-1)));
        const auto sy = unsigned(std::clamp((raw_bounds[1]+(y+.5)/height*(raw_bounds[3]-raw_bounds[1]))*raw_height,0.,double(raw_height-1)));
        const auto p=calibration_decode(raw.get()+std::size_t(sy)*raw_pitch+sx*raw_bytes,raw_format);
        return std::isfinite(p.r+p.g+p.b) ? (p.r+p.g+p.b)/3 : 0;
    }
};
inline CalibrationSearchImage calibration_search_image(const void* data, unsigned pitch,
    unsigned width, unsigned height, DXGI_FORMAT format, std::array<float, 4> bounds, bool sparse = false) {
    const auto conversion_start = std::chrono::steady_clock::now();
    CalibrationSearchImage image;
    const unsigned bytes = calibration_pixel_bytes(format);
    if (!data || !bytes || !width || !height || std::uint64_t(width) * bytes > pitch) return image;
    image.original_width = width * std::abs(double(bounds[2]) - bounds[0]);
    image.original_height = height * std::abs(double(bounds[3]) - bounds[1]);
    // Preserve original pixels for local decoding; only the locator pass downsamples.
    const double scale = 1.;
    image.width = unsigned(image.original_width * scale);
    image.height = unsigned(image.original_height * scale);
    if (sparse) {
        // Own packed source bytes before Unmap. Decode only requested samples on the worker.
        // This retains the full readback but avoids full-image float allocation/conversion.
        image.raw_width=width; image.raw_height=height; image.raw_pitch=width*bytes;
        image.raw_bytes=bytes; image.raw_format=format; image.raw_bounds=bounds;
        image.raw_memory=reserve_calibration_image_memory(std::uint64_t(image.raw_pitch)*height);
        if (!image.raw_memory) return {};
        image.raw.reset(new unsigned char[std::size_t(image.raw_pitch)*height]);
        for(unsigned y=0;y<height;++y)
            std::memcpy(image.raw.get()+std::size_t(y)*image.raw_pitch,
                static_cast<const unsigned char*>(data)+std::size_t(y)*pitch,image.raw_pitch);
        image.conversion_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-conversion_start).count();
        return image;
    }
    image.pixels.resize(std::size_t(image.width) * image.height);
    for (unsigned y = 0; y < image.height; ++y) for (unsigned x = 0; x < image.width; ++x) {
        const auto sx = (std::min)(width - 1, unsigned((bounds[0] + (x + .5) / image.width * (bounds[2] - bounds[0])) * width));
        const auto sy = (std::min)(height - 1, unsigned((bounds[1] + (y + .5) / image.height * (bounds[3] - bounds[1])) * height));
        const auto p = calibration_decode(static_cast<const unsigned char*>(data) + std::size_t(sy) * pitch + sx * bytes, format);
        image.pixels[std::size_t(y) * image.width + x] = std::isfinite(p.r + p.g + p.b) ? (p.r + p.g + p.b) / 3 : 0;
    }
    image.conversion_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-conversion_start).count();
    return image;
}
struct CalibrationLocatorBox { double x, y, width, height; };
// A dark connected ring enclosed by a light ring supplies an approximate box.
// No code templates or scale sweep are evaluated during this coarse pass.
inline std::vector<CalibrationLocatorBox> calibration_locators(const CalibrationSearchImage& image,
    unsigned factor, const std::atomic<bool>* canceled,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) {
    const auto stop = [&] { return (canceled && canceled->load(std::memory_order_relaxed)) || std::chrono::steady_clock::now() >= deadline; };
    const unsigned w = image.width/factor, h = image.height/factor;
    if (!w || !h) return {};
    std::vector<float> gray(std::size_t(w)*h);
    for (unsigned y=0; y<h; ++y) {
        if (stop()) return {};
        for (unsigned x=0; x<w; ++x) {
            double sum{};
            // Four stratified samples, independent of reduction factor.
            for (double j : {.25,.75}) for (double i : {.25,.75})
                sum += image.sample(x*factor+unsigned(i*factor),y*factor+unsigned(j*factor));
            gray[std::size_t(y)*w+x] = float(sum*.25);
        }
    }
    std::vector<CalibrationLocatorBox> boxes;
    std::vector<unsigned char> seen(gray.size());
    std::vector<unsigned> queue; queue.reserve(gray.size());
    for (float threshold : {.25F, .5F, .75F}) {
        std::fill(seen.begin(), seen.end(), static_cast<unsigned char>(0));
        for (unsigned seed=0; seed<gray.size(); ++seed) {
            if (seed%4096==0 && stop()) return {};
            if (seen[seed] || gray[seed] >= threshold) continue;
            queue.clear(); queue.push_back(seed); seen[seed]=1;
            unsigned x0=seed%w, x1=x0, y0=seed/w, y1=y0;
            for (std::size_t q=0; q<queue.size(); ++q) {
                if (q%4096==0 && stop()) return {};
                const unsigned pos=queue[q], x=pos%w, y=pos/w;
                x0=(std::min)(x0,x); x1=(std::max)(x1,x);
                y0=(std::min)(y0,y); y1=(std::max)(y1,y);
                const auto visit=[&](unsigned n) { if (!seen[n] && gray[n]<threshold) { seen[n]=1; queue.push_back(n); } };
                if(x) visit(pos-1); if(x+1<w) visit(pos+1);
                if(y) visit(pos-w); if(y+1<h) visit(pos+w);
            }
            const double bw=x1-x0+1, bh=y1-y0+1;
            if(bw<7 || bh<7 || bw>140 || bh>140 || bw/bh<.5 || bw/bh>2) continue;
            // Sample the middle of the black ring and the surrounding white ring.
            double dark{}, light{}; bool inside=true;
            for (double t : {.25,.5,.75}) for(unsigned side=0;side<4;++side) {
                const auto at=[&](double edge) {
                    double x=x0+t*bw, y=y0+t*bh;
                    if(side==0) x=x0+edge*bw; if(side==1) x=x0+(1-edge)*bw;
                    if(side==2) y=y0+edge*bh; if(side==3) y=y0+(1-edge)*bh;
                    if(x<0 || y<0 || x>=w || y>=h) { inside=false; return 0.F; }
                    return gray[std::size_t(unsigned(y))*w+unsigned(x)];
                };
                dark+=at(4./56); light+=at(-4./56);
            }
            if(!inside || (light-dark)/12 < .15) continue;
            CalibrationLocatorBox box{double(x0*factor),double(y0*factor),bw*factor,bh*factor};
            bool duplicate=false;
            for(const auto& b:boxes) if(std::abs(b.x-box.x)<factor*2 && std::abs(b.y-box.y)<factor*2) { duplicate=true; break; }
            if(!duplicate) { if(boxes.size()>=256) return {}; boxes.push_back(box); }
        }
    }
    return boxes;
}
struct CalibrationSearchOptions {
    double min_cell{1.5}, max_cell{20.}, aspect{1.};
    unsigned flip_mask{3};
    bool tracking{};
    bool fixed_geometry{};
    double expected_x{}, expected_y{}, expected_cw{}, expected_ch{}, translation_radius{};
    bool require_grid{};
    unsigned locator_factor{4};
    std::chrono::steady_clock::time_point deadline{std::chrono::steady_clock::time_point::max()};
};
inline CalibrationSearchResult calibration_search(const CalibrationSearchImage& image,
    const std::vector<CalibrationSearchTarget>& targets, const std::atomic<bool>* canceled = nullptr,
    CalibrationSearchOptions options = {}) {
    CalibrationSearchResult result;
    if (image.width < 10 || image.height < 10 || targets.empty()) return result;
    struct Template { unsigned target{}, flip{}; std::uint32_t code{}; };
    std::vector<Template> templates;
    // Hash complete 25-bit words (and single-bit errors), rather than running
    // every template at every pixel. Colliding words remain explicitly ambiguous.
    std::unordered_map<std::uint32_t, std::vector<unsigned>> words;
    for (unsigned t = 0; t < targets.size(); ++t) for (unsigned flip = 0; flip < 2; ++flip) {
        if (!(options.flip_mask & (1U << flip))) continue;
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
        return image.sample(unsigned(std::clamp(int(x),0,int(image.width)-1)),
            unsigned(std::clamp(int(y),0,int(image.height)-1)));
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
    struct Window { double x0,y0,x1,y1,cell,cy; };
    std::vector<Window> windows;
    if (options.fixed_geometry) {
        const auto r=options.translation_radius;
        windows.push_back({(std::max)(0.,options.expected_x-r),(std::max)(0.,options.expected_y-r),
            options.expected_x+r,options.expected_y+r,options.expected_cw,options.expected_ch});
    } else if (options.require_grid) {
        for (const auto& b : calibration_locators(image, options.locator_factor, canceled, options.deadline)) {
            const double margin=2.*options.locator_factor;
            for(double scale : {.88,1.,1.12}) {
                const double cell=b.width/7*scale, cy=b.height/7*scale;
                const double x=b.x+b.width/7, y=b.y+b.height/7;
                windows.push_back({(std::max)(0.,x-margin),(std::max)(0.,y-margin),
                    x+margin,y+margin,cell,cy});
            }
        }
    } else {
        for(double cell=options.min_cell;cell<=options.max_cell;cell*=1.12)
            for(double aspect : {.8,1.,1.25}) {
                const double cy=cell*aspect*options.aspect;
                windows.push_back({0,0,image.width-5*cell,image.height-5*cy,cell,cy});
            }
    }
    for (const auto& window : windows) {
        if ((canceled && canceled->load(std::memory_order_relaxed)) || std::chrono::steady_clock::now() >= options.deadline) return {};
        const double cell=window.cell, cy=window.cy;
        const double step=(std::max)(1.,cell*.5);
        for(double y=window.y0;y<=window.y1 && y+5*cy<=image.height;y+=step)
            for(double x=window.x0;x<=window.x1 && x+5*cell<=image.width;x+=step) {
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
    double best_edge = 1e30;
    struct Observation { unsigned target; double u, v, x, y; };
    std::vector<Observation> observations;
    for (auto hit : hits) {
        if ((canceled && canceled->load(std::memory_order_relaxed)) || std::chrono::steady_clock::now() >= options.deadline) return {};
        const auto& pattern = templates[hit.index];
        const auto& target = targets[pattern.target];
        // Refine independent X/Y scale and translation using the cell contrast.
        // Average the plateau of equally good fits to avoid biasing to the first
        // coarse position/scale that happens to put all samples inside cells.
        if (!options.fixed_geometry) for (double radius : {.18, .06}) {
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
        if (!options.tracking && (hit.x < 1.25 * hit.cw || hit.y < 1.25 * hit.ch ||
            hit.x + 6.25 * hit.cw > image.width || hit.y + 6.25 * hit.ch > image.height)) continue;
        if (result.valid && (result.candidate != target.candidate || result.flipped != bool(pattern.flip))) {
            result.valid = false; result.ambiguous = true; return result;
        }
        if (options.require_grid) {
            const double u = (hit.x + 2.5 * hit.cw) / image.width;
            const double image_v = (hit.y + 2.5 * hit.ch) / image.height;
            const double v = pattern.flip ? 1 - image_v : image_v;
            // Multiple spatial matches for the same code cannot establish identity.
            const auto previous = std::find_if(observations.begin(), observations.end(),
                [&](const auto& o) { return o.target == pattern.target; });
            if (previous != observations.end()) {
                if (std::abs(previous->u - u) > .015 || std::abs(previous->v - v) > .015) {
                    result.valid = false; result.ambiguous = true; return result;
                }
            } else observations.push_back({pattern.target, u, v,
                double(target.marker.x + 20), double(target.marker.y + 20)});
        }
        const double width = image.width * 8. / hit.cw, height = image.height * 8. / hit.ch;
        const double px = hit.x * 8. / hit.cw, py = hit.y * 8. / hit.ch;
        CalibrationPlacement placement{target.marker.x - px,
            pattern.flip ? target.marker.y + 40 - height + py : target.marker.y - py,
            width, height, target.marker};
        // Prefer a complete, decodable marker nearest a corner of the submitted
        // view. Actual headset hidden-area visibility is not available here.
        const double edge = calibration_corner_distance(hit.x, hit.y, image.width, image.height,
            hit.cw * 5, hit.ch * 5);
        if (!result.valid || edge < best_edge) {
            best_edge = edge;
            result = {true, bool(pattern.flip), false, target.candidate, placement, hit.score};
        }
    }
    if (options.require_grid && result.valid) {
        // Fit source_x = x0 + u*width and source_y = y0 + v*height from
        // separated points. Refuse skew/warp instead of publishing a false crop.
        if (observations.size() < 3) return {};
        double u{}, v{}, x{}, y{};
        for (const auto& o : observations) { u += o.u; v += o.v; x += o.x; y += o.y; }
        const double n = double(observations.size()); u /= n; v /= n; x /= n; y /= n;
        double uu{}, vv{}, ux{}, vy{}, uv{};
        for (const auto& o : observations) {
            uu += (o.u-u)*(o.u-u); vv += (o.v-v)*(o.v-v); uv += (o.u-u)*(o.v-v);
            ux += (o.u-u)*(o.x-x); vy += (o.v-v)*(o.y-y);
        }
        if (uu < .01 || vv < .01 || uu*vv-uv*uv < .0001) return {};
        auto& p = result.placement;
        p.width = ux/uu; p.height = vy/vv; p.x = x-u*p.width; p.y = y-v*p.height;
        if (!std::isfinite(p.width+p.height+p.x+p.y) || p.width <= 0 || p.height <= 0) return {};
        for (const auto& o : observations) {
            const double dx = (o.x-p.x-o.u*p.width)/p.width*image.original_width;
            const double dy = (o.y-p.y-o.v*p.height)/p.height*image.original_height;
            if (std::abs(dx) > 6 || std::abs(dy) > 6) return {};
        }
        result.support_points = unsigned(observations.size());
    }
    return result;
}
inline void calibration_search_start(const CalibrationSearchPtr& request, CalibrationSearchImage image) noexcept {
    if (!request || request->started.exchange(true)) return;
    if (request->capture_started_ms && image.width && image.height)
        request->capture_total_ms=calibration_clock_ms()-request->capture_started_ms;
    request->conversion_ms = image.conversion_ms;
    request->image_width = image.width; request->image_height = image.height;
    // Sparse content probe, at most 256 values from pixels already converted.
    // A black sample grid is evidence, not proof the entire texture is black.
    if (image.width && image.height && (image.raw || !image.pixels.empty())) {
        request->sampled_min = 1e30; request->sampled_max = -1e30;
        double sum{};
        for(unsigned y=0;y<16;++y) for(unsigned x=0;x<16;++x) {
            const unsigned sx=(std::min)(image.width-1, unsigned((x+.5)*image.width/16));
            const unsigned sy=(std::min)(image.height-1, unsigned((y+.5)*image.height/16));
            const float v=image.sample(sx,sy);
            request->sampled_min=(std::min)(request->sampled_min,double(v));
            request->sampled_max=(std::max)(request->sampled_max,double(v));
            sum+=v; ++request->sample_count; request->sampled_black += std::abs(v)<.001F;
        }
        request->sampled_mean=sum/request->sample_count;
    }
    static std::atomic<unsigned> workers{};
    if (workers.fetch_add(1) >= 2) {
        --workers; request->ready.store(true, std::memory_order_release); return;
    }
    try {
        std::thread([request, image = std::move(image)] {
            const auto start = std::chrono::steady_clock::now();
            try {
                CalibrationSearchOptions options; options.require_grid = request->require_grid;
                if (options.require_grid) {
                    // Start near 800 pixels on the longest side, then halve the factor.
                    // A soft wall-time budget also covers flood fill on blank captures.
                    options.locator_factor=4;
                    while (options.locator_factor<32 && (std::max)(image.width,image.height)/options.locator_factor>800)
                        options.locator_factor*=2;
                    options.deadline=start+std::chrono::milliseconds(100);
                    request->initial_locator_factor=options.locator_factor;
                    for (;;) {
                        request->last_locator_factor=options.locator_factor; ++request->locator_passes;
                        request->result=calibration_search(image,request->targets,&request->canceled,options);
                        request->search_budget_exhausted=std::chrono::steady_clock::now()>=options.deadline;
                        if(request->result.valid || request->result.ambiguous || request->canceled.load() ||
                            request->search_budget_exhausted || options.locator_factor==2) break;
                        options.locator_factor/=2;
                    }
                } else request->result=calibration_search(image,request->targets,&request->canceled,options);
            } catch (...) { request->result = {}; }
            request->elapsed_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            request->timed = true;
            --workers;
            request->ready.store(true, std::memory_order_release);
        }).detach();
    } catch (...) { --workers; request->ready.store(true, std::memory_order_release); }
}
} // namespace cheeky::foveated_dlss
