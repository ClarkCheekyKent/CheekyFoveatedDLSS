#include "eye_calibration.hpp"
#include "eye_calibration_pixels.hpp"
#include "eye_calibration_d3d12.hpp"
#include "settings.hpp"
#include "eye_calibration_bridge.h"
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <locale>

namespace cheeky::foveated_dlss {
namespace {
using Microsoft::WRL::ComPtr;
constexpr unsigned ring_size = 8, block = calibration_marker_size, inset = 12;
constexpr unsigned sample = calibration_sample_size;
constexpr int margin = calibration_sample_margin;
constexpr std::uint64_t ticket_bit = 1ULL << 63;
struct Patch {
    ComPtr<ID3D11Texture2D> staging;
    unsigned width{}, height{}, reference_width{}, reference_height{};
    DXGI_FORMAT format{};
    float score{};
    bool used{}, ready{};
};
struct View {
    std::uint64_t id{};
    unsigned width{}, height{};
    int assigned{-1};
    std::uint64_t generation{};
};
struct Frame {
    std::shared_ptr<Calibration12Frame> gpu12;
    ComPtr<ID3D12Device> device12;
    bool gpu12_used{}, classified{};
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Query> done, disjoint;
    std::array<ComPtr<ID3D11Query>, 8> timestamp;
    // Source before/after for A and B, then A/B at each submitted eye.
    std::array<Patch, 8> patches;
    std::array<float, 4> flipped_scores{};
    std::array<View, 2> views;
    std::array<unsigned, 2> eye_submits{};
    std::array<int, 2> result{{-1, -1}};
    std::array<unsigned, 2> physical_eyes{{0, 1}};
    std::array<bool, 4> segments{};
    std::uint64_t sequence{};
    std::uint64_t epoch{}, captured_ms{}, session_generation{};
    DWORD thread{};
    unsigned evaluations{}, submits{};
    bool busy{}, closed{}, invalid{}, queries_started{};
};
struct Average {
    std::array<double, 256> values{};
    unsigned at{}, count{};
    double sum{};
    void add(double v) {
        sum -= values[at];
        values[at] = v;
        sum += v;
        at = (at + 1) % unsigned(values.size());
        count = (std::min)(count + 1, unsigned(values.size()));
    }
    double get() const {
        return count ? sum / count : 0;
    }
};
struct State {
    std::mutex mutex;
    std::array<Frame, ring_size> ring;
    int current{-1};
    std::uint64_t sequence{};
    unsigned frames_until_capture{};
    std::uint64_t measurement_start{}, last_valid_sequence{};
    std::uint64_t epoch{};
    ComPtr<ID3D11Device> device;
    std::array<ComPtr<ID3D11Texture2D>, 2> markers;
    DXGI_FORMAT marker_format{};
    EyeCalibrationStats stats;
    Average gpu, latency;
    double cpu_us{};
    std::uint64_t last_frame_ms{}, last_openvr_ms{}, session_generation{};
    EyeCalibrationBackend backend{};
    bool unsupported_submission{};
};
State& state() {
    // Match the process-resident hook lifetime. Explicit stop releases GPU
    // objects; static destruction must not touch D3D under the loader lock.
    static auto* s = new State;
    return *s;
}
std::atomic<bool> enabled{}, pending{};
constexpr const char* rejection_names[] = {
    "capture_or_readback", "evaluation_count", "eye_submissions", "submission_result",
    "patches_incomplete", "source_marker", "dimensions", "submitted_markers"
};
void record_rejection(State& s, const Frame& f, unsigned mask) {
    ++s.stats.rejected;
    for (unsigned i = 0; i < s.stats.rejection_counts.size(); ++i)
        if (mask & (1U << i)) ++s.stats.rejection_counts[i];
    // GPU completions need not arrive in sequence order.
    if (f.sequence < s.stats.last_rejected_sequence) return;
    s.stats.last_rejected_sequence = f.sequence;
    s.stats.last_rejection_mask = mask;
    s.stats.last_evaluations = f.evaluations;
    s.stats.last_submits = f.submits;
    for (unsigned i = 0; i < f.patches.size(); ++i)
        s.stats.last_rejected_scores[i] = f.patches[i].score;
}
double now_us() {
    static const double scale = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1e6 / double(f.QuadPart);
    }();
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return double(q.QuadPart) * scale;
}
// Measures work inside the lock, including polling and warm-up allocations.
struct CpuScope {
    State& s;
    double start{now_us()};
    ~CpuScope() {
        const double us = now_us() - start;
        s.cpu_us += us;
        s.stats.max_cpu_call_us = (std::max)(s.stats.max_cpu_call_us, us);
    }
};
bool same_context(Frame& f, ID3D11DeviceContext* context) {
    return f.context.Get() == context && f.thread == GetCurrentThreadId();
}
void ensure_queries(State& s, Frame& f, ID3D11DeviceContext* context) {
    if (f.context.Get() != context) {
        f.done.Reset();
        f.disjoint.Reset();
        f.timestamp = {};
        f.patches = {};
        f.context = context;
    }
    f.thread = GetCurrentThreadId();
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    auto create = [&](ComPtr<ID3D11Query>& q, D3D11_QUERY type) {
        if (q)
            return true;
        const D3D11_QUERY_DESC desc{type, 0};
        if (FAILED(device->CreateQuery(&desc, &q)))
            return false;
        ++s.stats.allocations;
        return true;
    };
    if (!create(f.done, D3D11_QUERY_EVENT) || !create(f.disjoint, D3D11_QUERY_TIMESTAMP_DISJOINT)) {
        f.invalid = true;
        return;
    }
    for (auto& q : f.timestamp)
        if (!create(q, D3D11_QUERY_TIMESTAMP)) {
            f.invalid = true;
            return;
        }
    context->Begin(f.disjoint.Get());
    f.queries_started = true;
}
void finish(Frame& f) {
    if (!f.busy || f.closed)
        return;
    if (!f.gpu12_used && f.queries_started) {
        if (f.thread != GetCurrentThreadId())
            return;
        f.context->End(f.disjoint.Get());
        f.context->End(f.done.Get());
    }
    f.closed = true;
}
void poll(State& s) {
    for (auto& f : s.ring) {
        if (!f.busy || !f.closed)
            continue;
        bool gpu12_reusable{};
        if (f.gpu12_used) {
            const auto result = calibration12_poll(*f.gpu12);
            if (f.sequence >= s.measurement_start)
                s.stats.allocations += result.allocations;
            if (!result.ready)
                continue;
            gpu12_reusable = result.reusable;
            if (f.classified || f.sequence < s.measurement_start) {
                f.classified = true;
                if (gpu12_reusable)
                    f.busy = false;
                continue;
            }
            f.invalid = f.invalid || !result.valid;
            if (!result.valid) {
                ++s.stats.d3d12_readback_failures;
                s.stats.d3d12_last_readback_failure = result.failure.stage;
                s.stats.d3d12_readback_error = result.failure.result;
            }
            for (unsigned i = 0; i < 8; ++i) {
                f.patches[i].used = f.patches[i].ready = true;
                f.patches[i].score = result.scores[i];
            }
            std::copy_n(result.scores.begin() + 8, 4, f.flipped_scores.begin());
            if (result.timing_valid) {
                s.gpu.add(result.gpu_us);
                ++s.stats.gpu_samples;
                s.stats.max_gpu_us = (std::max)(s.stats.max_gpu_us, result.gpu_us);
            }
        } else {
            if (!f.queries_started) {
                f.busy = false;
                if (f.sequence >= s.measurement_start) {
                    ++s.stats.completed;
                    record_rejection(s, f, 1U | (f.evaluations != 2 ? 2U : 0U));
                }
                continue;
            }
            if (f.thread != GetCurrentThreadId())
                continue;
            const auto hr = f.context->GetData(f.done.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE)
                continue;
            if (FAILED(hr)) {
                f.busy = false;
                if (f.sequence >= s.measurement_start) {
                    ++s.stats.completed;
                    record_rejection(s, f, 1U);
                }
                continue;
            }
            if (f.sequence < s.measurement_start) {
                f.busy = false;
                continue;
            }
            bool waiting{};
            for (unsigned i = 0; i < f.patches.size(); ++i) {
                auto& p = f.patches[i];
                if (!p.used || p.ready)
                    continue;
                D3D11_MAPPED_SUBRESOURCE mapped{};
                const auto map_hr =
                    f.context->Map(p.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (map_hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                    waiting = true;
                    continue;
                }
                if (FAILED(map_hr)) {
                    f.invalid = true;
                    p.ready = true;
                    continue;
                }
                const unsigned candidate = i < 4 ? i / 2 : (i - 4) % 2;
                p.score = calibration_pattern_score(mapped.pData, mapped.RowPitch, p.width, p.height,
                                                    p.format, candidate, i >= 4);
                f.context->Unmap(p.staging.Get(), 0);
                p.ready = true;
            }
            if (waiting)
                continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            if (f.context->GetData(f.disjoint.Get(), &disjoint, sizeof(disjoint),
                                   D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                !disjoint.Disjoint && disjoint.Frequency) {
                double us{};
                bool valid_time =
                    std::all_of(f.segments.begin(), f.segments.end(), [](bool segment) { return segment; });
                for (unsigned i = 0; i < 4; ++i)
                    if (f.segments[i]) {
                        UINT64 first{}, last{};
                        if (f.context->GetData(f.timestamp[i * 2].Get(), &first, sizeof(first),
                                               D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                            f.context->GetData(f.timestamp[i * 2 + 1].Get(), &last, sizeof(last),
                                               D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                            last < first)
                            valid_time = false;
                        else
                            us += double(last - first) * 1e6 / double(disjoint.Frequency);
                    }
                if (valid_time) {
                    s.gpu.add(us);
                    s.stats.max_gpu_us = (std::max)(s.stats.max_gpu_us, us);
                    ++s.stats.gpu_samples;
                }
            }
        }
        unsigned rejection = f.invalid ? 1U : 0U;
        if (f.evaluations != 2) rejection |= 2U;
        if (f.submits != 2 || f.eye_submits[0] != 1 || f.eye_submits[1] != 1) rejection |= 4U;
        if (f.result[0] != 0 || f.result[1] != 0) rejection |= 8U;
        for (const auto& p : f.patches)
            if (!p.used || !p.ready) rejection |= 16U;
        for (unsigned c = 0; c < 2; ++c) {
            if (!(f.patches[c * 2 + 1].score >= 0.8F &&
                    f.patches[c * 2 + 1].score - f.patches[c * 2].score >= 0.3F)) rejection |= 32U;
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto& p = f.patches[4 + eye * 2 + c];
                if (p.reference_width != f.views[c].width || p.reference_height != f.views[c].height)
                    rejection |= 64U;
            }
        }
        const auto left_slot = f.physical_eyes[0] == 0 ? 0U : 1U;
        const auto right_slot = 1U - left_slot;
        if (f.physical_eyes[0] >= 2 || f.physical_eyes[1] >= 2 ||
                f.physical_eyes[0] == f.physical_eyes[1]) rejection |= 4U;
        int left =
            calibration_pattern_classify(f.patches[4 + left_slot * 2].score, f.patches[5 + left_slot * 2].score);
        int right =
            calibration_pattern_classify(f.patches[4 + right_slot * 2].score, f.patches[5 + right_slot * 2].score);
        const int flipped_left = f.gpu12_used ? calibration_pattern_classify(
            f.flipped_scores[left_slot * 2], f.flipped_scores[left_slot * 2 + 1]) : -1;
        const int flipped_right = f.gpu12_used ? calibration_pattern_classify(
            f.flipped_scores[right_slot * 2], f.flipped_scores[right_slot * 2 + 1]) : -1;
        const bool normal_pair = left >= 0 && right >= 0 && left != right;
        const bool flipped_pair = flipped_left >= 0 && flipped_right >= 0 && flipped_left != flipped_right;
        // A post-DLSS shader may flip the image. Accept exactly one complete
        // stereo pair; conflicting orientation evidence must never guess.
        if (normal_pair == flipped_pair) rejection |= 128U;
        if (flipped_pair) { left = flipped_left; right = flipped_right; }
        if (rejection) record_rejection(s, f, rejection);
        if (!rejection) {
            ++s.stats.valid;
            if (f.sequence > s.last_valid_sequence) {
                s.stats.left_view = f.views[left].id;
                s.stats.right_view = f.views[right].id;
                s.last_valid_sequence = f.sequence;
            }
            if (f.views[left].assigned >= 0 && f.views[right].assigned >= 0 &&
                (f.views[left].assigned != 0 || f.views[right].assigned != 1))
                ++s.stats.mismatches;
            // Readbacks may complete out of order or after a toggle. The settings
            // layer additionally verifies ordering, age and handle lifetimes.
            if (enabled && f.epoch == s.epoch) {
                bool corrected{};
                if (publish_stereo_calibration(f.views[left].id, f.views[right].id, f.views[left].generation,
                                               f.views[right].generation, f.sequence, f.captured_ms,
                                               &corrected, f.session_generation, flipped_pair)) {
                    ++s.stats.applied;
                    if (corrected)
                        ++s.stats.corrections;
                } else ++s.stats.publication_rejected;
            } else ++s.stats.publication_rejected;
        }
        ++s.stats.completed;
        s.latency.add(double(s.sequence - f.sequence));
        f.classified = true;
        f.busy = f.gpu12_used && !gpu12_reusable;
    }
    pending = std::any_of(s.ring.begin(), s.ring.end(), [](const auto& f) { return f.busy; });
}
bool copy_patch(State& s, Frame& f, unsigned index, ID3D11Texture2D* texture, unsigned x, unsigned y,
                unsigned width, unsigned height, unsigned rw = 0, unsigned rh = 0, unsigned slice = 0) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (!calibration_pixel_bytes(desc.Format) || slice >= desc.ArraySize || desc.SampleDesc.Count != 1 ||
        !width || !height || width > 256 || height > 256 || std::uint64_t(x) + width > desc.Width ||
        std::uint64_t(y) + height > desc.Height)
        return false;
    const auto subresource = D3D11CalcSubresource(0, slice, desc.MipLevels);
    auto& p = f.patches[index];
    if (!p.staging || p.width != width || p.height != height || p.format != desc.Format) {
        p.staging.Reset();
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Device> device;
        f.context->GetDevice(&device);
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &p.staging)))
            return false;
        ++s.stats.allocations;
        p.width = width;
        p.height = height;
        p.format = desc.Format;
    }
    p.reference_width = rw;
    p.reference_height = rh;
    const D3D11_BOX box{x, y, 0, x + width, y + height, 1};
    f.context->CopySubresourceRegion(p.staging.Get(), 0, 0, 0, 0, texture, subresource, &box);
    p.used = true;
    return true;
}
} // namespace
void eye_calibration_enable(bool value) noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (enabled.exchange(value) != value) {
        s.frames_until_capture = 0;
        ++s.epoch;
        clear_stereo_calibration();
    }
}
void eye_calibration_suspend() noexcept {
    enabled = false;
}
bool eye_calibration_enabled() noexcept {
    return enabled.load();
}
EyeCalibrationStats eye_calibration_stats() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto result = s.stats;
    result.enabled = enabled;
    result.in_flight =
        unsigned(std::count_if(s.ring.begin(), s.ring.end(), [](const auto& f) { return f.busy; }));
    result.cpu_us_per_frame = result.frames ? s.cpu_us / result.frames : 0;
    result.gpu_us = s.gpu.get();
    result.latency_frames = s.latency.get();
    result.backend = s.backend;
    result.runtime_active = s.last_frame_ms && GetTickCount64() - s.last_frame_ms <= 1000;
    result.openvr_active = result.runtime_active && s.backend == EyeCalibrationBackend::openvr;
    result.unsupported_submission = s.unsupported_submission;
    result.correction_active = result.enabled && stereo_eye_assignment(result.left_view).calibrated &&
                               stereo_eye_assignment(result.right_view).calibrated;
    result.vertical_flip = result.correction_active && stereo_eye_assignment(result.left_view).vertical_flip;
    return result;
}
void eye_calibration_reset_stats() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    // Reset measurement, not the established mapping or its ordering guard.
    const auto left = s.stats.left_view, right = s.stats.right_view;
    const auto graphics_api = s.stats.graphics_api;
    s.stats = {};
    s.stats.graphics_api = graphics_api;
    s.stats.left_view = left;
    s.stats.right_view = right;
    s.cpu_us = 0;
    s.gpu = {};
    s.latency = {};
    s.measurement_start = s.sequence + 1;
}
bool eye_calibration_frame(EyeCalibrationBackend backend, std::uint64_t session_generation,
                           unsigned graphics_api) noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto now = GetTickCount64();
    // OpenComposite can expose both APIs. Preserve the working OpenVR route,
    // and never mix its stamps/readbacks with an inner OpenXR frame.
    if (backend == EyeCalibrationBackend::openxr && s.last_openvr_ms && now - s.last_openvr_ms <= 1000)
        return false;
    if (backend == EyeCalibrationBackend::openvr)
        s.last_openvr_ms = now;
    if (s.backend != backend || s.session_generation != session_generation) {
        ++s.epoch;
        clear_stereo_calibration();
        s.frames_until_capture = 0;
        s.backend = backend;
        s.session_generation = session_generation;
        s.unsupported_submission = false;
    }
    const bool on = enabled;
    if (graphics_api)
        s.stats.graphics_api = graphics_api;
    s.last_frame_ms = GetTickCount64();
    if (!on && !pending)
        return false;
    CpuScope cpu{s};
    ++s.sequence;
    if (s.current >= 0) {
        finish(s.ring[s.current]);
        s.current = -1;
    }
    poll(s);
    if (!on)
        return false;
    ++s.stats.frames;
    // Drain previous readbacks on every frame, but stamp/capture only one in ten.
    if (s.frames_until_capture) {
        --s.frames_until_capture;
        return false;
    }
    s.frames_until_capture = 9;
    // Rotate through all slots so the warm-up is bounded and reproducible.
    for (unsigned n = 0; n < ring_size; ++n) {
        const unsigned i = unsigned((s.stats.captures + n) % ring_size);
        auto& f = s.ring[i];
        if (f.busy)
            continue;
        if (f.gpu12 && !calibration12_begin(*f.gpu12))
            continue;
        f.gpu12_used = f.classified = false;
        f.sequence = s.sequence;
        f.busy = true;
        f.closed = f.invalid = f.queries_started = false;
        f.epoch = s.epoch;
        f.session_generation = session_generation;
        f.captured_ms = GetTickCount64();
        f.evaluations = f.submits = 0;
        f.views = {};
        f.eye_submits = {};
        f.result = {{-1, -1}};
        f.physical_eyes = {{0, 1}};
        f.segments = {};
        for (auto& p : f.patches) {
            p.used = p.ready = false;
            p.score = 0;
        }
        s.current = int(i);
        ++s.stats.captures;
        pending = true;
        return true;
    }
    ++s.stats.skipped; // No blocking or overwriting unfinished GPU readbacks.
    return false;
}
void eye_calibration_tick() noexcept {
    if (!pending)
        return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    CpuScope cpu{s};
    poll(s);
}
void eye_calibration_stamp(ID3D11DeviceContext* context, ID3D11Resource* output, std::uint64_t view,
                           unsigned x, unsigned y, unsigned width, unsigned height) noexcept {
    if (!enabled || !context || !output)
        return;
    try {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        CpuScope cpu{s};
        if (s.current < 0)
            return;
        s.stats.graphics_api = 11;
        if (s.ring[s.current].gpu12_used) {
            s.ring[s.current].invalid = true;
            return;
        }
        auto& f = s.ring[s.current];
        const unsigned c = f.evaluations++;
        if (c >= 2) {
            f.invalid = true;
            return;
        }
        if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
            f.invalid = true;
            return;
        }
        if (!f.queries_started)
            ensure_queries(s, f, context);
        if (!f.queries_started || !same_context(f, context)) {
            f.invalid = true;
            return;
        }
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(output->QueryInterface(IID_PPV_ARGS(&texture)))) {
            f.invalid = true;
            return;
        }
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        const unsigned bytes = calibration_pixel_bytes(desc.Format);
        if (!bytes || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 || width < 2 * inset + block ||
            height < 2 * inset + block || std::uint64_t(x) + width > desc.Width ||
            std::uint64_t(y) + height > desc.Height) {
            f.invalid = true;
            return;
        }
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (s.device.Get() != device.Get() || s.marker_format != desc.Format) {
            s.markers = {};
            s.device = device;
            s.marker_format = desc.Format;
        }
        if (!s.markers[c]) {
            std::vector<unsigned char> data(block * block * bytes);
            for (unsigned i = 0; i < block * block; ++i)
                calibration_encode_pattern(data.data() + i * bytes, desc.Format, c, i % block, i / block);
            desc.Width = desc.Height = block;
            desc.MipLevels = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = desc.CPUAccessFlags = desc.MiscFlags = 0;
            const D3D11_SUBRESOURCE_DATA initial{data.data(), block * bytes, 0};
            if (FAILED(device->CreateTexture2D(&desc, &initial, &s.markers[c]))) {
                f.invalid = true;
                return;
            }
            ++s.stats.allocations;
        }
        const auto assignment = stereo_eye_assignment(view);
        f.views[c] = {view, width, height, assignment.assigned ? int(assignment.eye_index) : -1,
                      stereo_view_generation(view)};
        const unsigned px = x + (c ? width - inset - block : inset), py = y + inset;
        context->End(f.timestamp[c * 2].Get());
        if (!copy_patch(s, f, c * 2, texture.Get(), px, py, block, block))
            f.invalid = true;
        context->CopySubresourceRegion(texture.Get(), 0, px, py, 0, s.markers[c].Get(), 0, nullptr);
        if (!copy_patch(s, f, c * 2 + 1, texture.Get(), px, py, block, block))
            f.invalid = true;
        context->End(f.timestamp[c * 2 + 1].Get());
        f.segments[c] = true;
    } catch (...) {
        // Allocation failures must neither escape the NGX hook nor publish a
        // partially captured frame. The lock above has unwound before this.
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (s.current >= 0)
            s.ring[s.current].invalid = true;
    }
}
void eye_calibration_stamp12(ID3D12GraphicsCommandList* list, ID3D12Resource* output, std::uint64_t view,
                             unsigned x, unsigned y, unsigned width, unsigned height,
                             D3D12_RESOURCE_STATES output_state) noexcept {
    if (!enabled || !list || !output)
        return;
    try {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        CpuScope cpu{s};
        if (s.current < 0)
            return;
        auto& f = s.ring[s.current];
        const auto c = f.evaluations++;
        if (c >= 2 || f.queries_started || width < 2 * inset + block || height < 2 * inset + block) {
            f.invalid = true;
            return;
        }
        const auto d = output->GetDesc();
        s.stats.d3d12_source_formats[c] = unsigned(d.Format);
        if (UINT64(x) + width > d.Width || UINT64(y) + height > d.Height) {
            f.invalid = true;
            return;
        }
        ComPtr<ID3D12Device> device;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) {
            f.invalid = true;
            return;
        }
        if (!f.gpu12 || device.Get() != f.device12.Get()) {
            if (c) {
                f.invalid = true;
                return;
            }
            f.gpu12 = calibration12_create(device.Get());
            f.device12 = device;
            if (!calibration12_begin(*f.gpu12)) {
                f.invalid = true;
                return;
            }
        }
        f.gpu12_used = true;
        s.stats.graphics_api = 12;
        const auto assignment = stereo_eye_assignment(view);
        f.views[c] = {view, width, height, assignment.assigned ? int(assignment.eye_index) : -1,
                      stereo_view_generation(view)};
        const auto px = x + (c ? width - inset - block : inset), py = y + inset;
        Calibration12Failure failure;
        if (!calibration12_stamp(*f.gpu12, list, output, c, px, py, output_state, s.stats.allocations, &failure)) {
            f.invalid = true;
            ++s.stats.d3d12_stamp_failures;
            s.stats.d3d12_last_stamp_failure = failure.stage;
            s.stats.d3d12_stamp_error = failure.result;
        }
    } catch (...) {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (s.current >= 0)
            s.ring[s.current].invalid = true;
    }
}
std::uint64_t eye_calibration_submit12(ID3D12Resource* texture, ID3D12CommandQueue* queue, unsigned eye,
                                       float u0, float v0, float u1, float v1, unsigned slice,
                                       EyeCalibrationBackend backend, std::uint64_t generation) noexcept {
    if (!enabled || !texture || !queue || eye > 1)
        return 0;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    CpuScope cpu{s};
    if (s.backend != backend || s.session_generation != generation || s.current < 0)
        return 0;
    auto& f = s.ring[s.current];
    ++f.submits;
    ++f.eye_submits[eye];
    if (!f.gpu12_used || f.submits > 2 || f.eye_submits[eye] > 1) {
        f.invalid = true;
        return 0;
    }
    for (const float v : {u0, v0, u1, v1})
        if (!std::isfinite(v) || v < 0 || v > 1) {
            f.invalid = true;
            return 0;
        }
    if (u0 == u1 || v0 == v1) {
        f.invalid = true;
        return 0;
    }
    const auto d = texture->GetDesc();
    s.stats.d3d12_submitted_formats[eye] = unsigned(d.Format);
    if (d.Width > UINT32_MAX) {
        f.invalid = true;
        return 0;
    }
    std::array<D3D12_BOX, 4> boxes;
    for (unsigned index = 0; index < boxes.size(); ++index) {
        const auto c = index % 2;
        const auto& ref = f.views[c].width ? f.views[c] : f.views[0];
        if (!ref.width || !ref.height) {
            f.invalid = true;
            return 0;
        }
        const float nx = (float(c ? ref.width - inset - block : inset) + margin) / ref.width,
                    ny = (float(index < 2 ? inset : ref.height - inset - block) + margin) / ref.height;
        const float ax = (u0 + nx * (u1 - u0)) * d.Width,
                    bx = (u0 + (nx + float(sample) / ref.width) * (u1 - u0)) * d.Width;
        const float ay = (v0 + ny * (v1 - v0)) * d.Height,
                    by = (v0 + (ny + float(sample) / ref.height) * (v1 - v0)) * d.Height;
        boxes[index] = {unsigned(std::floor((std::min)(ax, bx))), unsigned(std::floor((std::min)(ay, by))), 0,
                    unsigned(std::ceil((std::max)(ax, bx))),  unsigned(std::ceil((std::max)(ay, by))),  1};
        f.patches[4 + eye * 2 + c].reference_width = ref.width;
        f.patches[4 + eye * 2 + c].reference_height = ref.height;
    }
    const auto expected_state = backend == EyeCalibrationBackend::openxr
                                    ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                    : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Calibration12Failure failure;
    if (!calibration12_capture(*f.gpu12, queue, texture, eye, slice, expected_state, boxes,
                               s.stats.allocations, &failure)) {
        f.invalid = true;
        ++s.stats.d3d12_capture_failures;
        s.stats.d3d12_last_capture_failure = failure.stage;
        s.stats.d3d12_capture_error = failure.result;
        return 0;
    }
    return ticket_bit | (f.sequence << 1) | eye;
}
std::uint64_t eye_calibration_submit(ID3D11Texture2D* texture, unsigned eye, float u0, float v0, float u1,
                                     float v1, unsigned slice, EyeCalibrationBackend backend,
                                     std::uint64_t session_generation) noexcept {
    if (!enabled || !texture || eye > 1)
        return 0;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    CpuScope cpu{s};
    if (s.backend != backend || s.session_generation != session_generation)
        return 0;
    s.unsupported_submission = false;
    if (s.current < 0)
        return 0;
    auto& f = s.ring[s.current];
    ++f.submits;
    ++f.eye_submits[eye];
    if (f.submits > 2 || f.eye_submits[eye] > 1 || !f.queries_started || f.thread != GetCurrentThreadId()) {
        f.invalid = true;
        return 0;
    }
    ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!same_context(f, context.Get())) {
        f.invalid = true;
        return 0;
    }
    for (float v : {u0, v0, u1, v1})
        if (!std::isfinite(v) || v < 0 || v > 1) {
            f.invalid = true;
            return 0;
        }
    if (u0 == u1 || v0 == v1) {
        f.invalid = true;
        return 0;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    context->End(f.timestamp[4 + eye * 2].Get());
    for (unsigned c = 0; c < 2; ++c) {
        const auto& ref = f.views[c].width ? f.views[c] : f.views[0];
        if (!ref.width || !ref.height) {
            f.invalid = true;
            continue;
        }
        const float nx = (float(c ? ref.width - inset - block : inset) + margin) / ref.width,
                    ny = (float(inset) + margin) / ref.height;
        const float ax = (u0 + nx * (u1 - u0)) * desc.Width,
                    bx = (u0 + (nx + float(sample) / ref.width) * (u1 - u0)) * desc.Width;
        const float ay = (v0 + ny * (v1 - v0)) * desc.Height,
                    by = (v0 + (ny + float(sample) / ref.height) * (v1 - v0)) * desc.Height;
        const unsigned x = unsigned(std::floor((std::min)(ax, bx))),
                       y = unsigned(std::floor((std::min)(ay, by)));
        const unsigned w = unsigned(std::ceil((std::max)(ax, bx))) - x,
                       h = unsigned(std::ceil((std::max)(ay, by))) - y;
        if (!copy_patch(s, f, 4 + eye * 2 + c, texture, x, y, w, h, ref.width, ref.height, slice))
            f.invalid = true;
    }
    context->End(f.timestamp[5 + eye * 2].Get());
    f.segments[2 + eye] = true;
    return ticket_bit | (f.sequence << 1) | eye;
}
void eye_calibration_result(std::uint64_t ticket, int result, unsigned physical_eye) noexcept {
    if (!(ticket & ticket_bit))
        return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto sequence = (ticket & ~ticket_bit) >> 1;
    for (auto& f : s.ring)
        if (f.busy && f.sequence == sequence) {
            f.result[ticket & 1] = result;
            if (physical_eye != ~0U)
                f.physical_eyes[ticket & 1] = physical_eye;
        }
}
void eye_calibration_unsupported_submit() noexcept {
    if (!enabled)
        return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.unsupported_submission = true;
    ++s.stats.unsupported_submissions;
    if (s.current >= 0)
        s.ring[s.current].invalid = true;
}
void eye_calibration_stop() noexcept {
    enabled = false;
    pending = false;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    ++s.epoch;
    clear_stereo_calibration();
    s.ring = {};
    s.markers = {};
    s.device.Reset();
    s.current = -1;
    s.last_frame_ms = s.last_openvr_ms = s.session_generation = 0;
    s.backend = EyeCalibrationBackend::none;
    s.unsupported_submission = false;
}

const char* eye_calibration_status(const EyeCalibrationStats& stats) noexcept {
    if (!stats.enabled)
        return "Disabled";
    if (!stats.runtime_active)
        return "Waiting for OpenVR or OpenXR";
    if (stats.unsupported_submission)
        return "Unsupported texture or queue path";
    if (stats.correction_active)
        return "Active";
    return "Waiting for a valid stereo marker pair";
}

const char* eye_calibration_backend_name(EyeCalibrationBackend backend) noexcept {
    switch (backend) {
    case EyeCalibrationBackend::openvr:
        return "OpenVR";
    case EyeCalibrationBackend::openxr:
        return "OpenXR";
    default:
        return "Waiting for VR";
    }
}
void eye_calibration_destroy_session(std::uint64_t generation) noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.backend != EyeCalibrationBackend::openxr || s.session_generation != generation)
        return;
    ++s.epoch;
    clear_stereo_calibration();
    s.session_generation = 0;
    s.last_frame_ms = 0;
}

std::string eye_calibration_json() {
    const auto s = eye_calibration_stats();
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::boolalpha << std::setprecision(6) << "{\"backend\":\""
        << eye_calibration_backend_name(s.backend) << "\",\"graphics_api\":" << s.graphics_api
        << ",\"enabled\":" << s.enabled << ",\"status\":\"" << eye_calibration_status(s)
        << "\",\"active\":" << s.correction_active << ",\"openvr_active\":" << s.openvr_active
        << ",\"vertical_flip\":" << s.vertical_flip
        << ",\"unsupported_submission\":" << s.unsupported_submission
        << ",\"unsupported_submissions\":" << s.unsupported_submissions << ",\"frames\":" << s.frames
        << ",\"captures\":" << s.captures << ",\"completed\":" << s.completed << ",\"valid\":" << s.valid
        << ",\"skipped\":" << s.skipped << ",\"in_flight\":" << s.in_flight
        << ",\"corrections\":" << s.corrections << ",\"applied\":" << s.applied
        << ",\"mismatches\":" << s.mismatches << ",\"allocations\":" << s.allocations
        << ",\"gpu_samples\":" << s.gpu_samples << ",\"cpu_us_per_frame\":" << s.cpu_us_per_frame
        << ",\"max_cpu_call_us\":" << s.max_cpu_call_us << ",\"gpu_us\":" << s.gpu_us
        << ",\"max_gpu_us\":" << s.max_gpu_us << ",\"latency_frames\":"
        << s.latency_frames
        << ",\"rejected\":" << s.rejected
        << ",\"publication_rejected\":" << s.publication_rejected
        << ",\"rejection_counts\":{";
    for (unsigned i = 0; i < s.rejection_counts.size(); ++i) {
        if (i) out << ',';
        out << '\"' << rejection_names[i] << "\":" << s.rejection_counts[i];
    }
    out << "},\"last_rejection\":{\"sequence\":" << s.last_rejected_sequence
        << ",\"mask\":" << s.last_rejection_mask
        << ",\"evaluations\":" << s.last_evaluations
        << ",\"submits\":" << s.last_submits << ",\"reasons\":[";
    bool separator = false;
    for (unsigned i = 0; i < s.rejection_counts.size(); ++i) {
        if (!(s.last_rejection_mask & (1U << i))) continue;
        if (separator) out << ',';
        out << '\"' << rejection_names[i] << '\"';
        separator = true;
    }
    out << "],\"scores\":[";
    for (unsigned i = 0; i < s.last_rejected_scores.size(); ++i) {
        if (i) out << ',';
        out << s.last_rejected_scores[i];
    }
    out << "]},\"d3d12\":{\"source_formats\":[" << s.d3d12_source_formats[0] << ',' << s.d3d12_source_formats[1]
        << "],\"submitted_formats\":[" << s.d3d12_submitted_formats[0] << ',' << s.d3d12_submitted_formats[1]
        << "],\"stamp_failures\":" << s.d3d12_stamp_failures
        << ",\"capture_failures\":" << s.d3d12_capture_failures
        << ",\"readback_failures\":" << s.d3d12_readback_failures
        << ",\"last_stamp_failure\":\"" << s.d3d12_last_stamp_failure
        << "\",\"stamp_hresult\":" << s.d3d12_stamp_error
        << ",\"last_capture_failure\":\"" << s.d3d12_last_capture_failure
        << "\",\"capture_hresult\":" << s.d3d12_capture_error
        << ",\"last_readback_failure\":\"" << s.d3d12_last_readback_failure
        << "\",\"readback_hresult\":" << s.d3d12_readback_error << '}';
    out << ",\"marker_mode\":\"pattern5x5\",\"pattern_min_score\":0.90,\"pattern_min_gap\":0.15";
    out
        // View identities are pointers; strings preserve all bits through Lua.
        << ",\"left_view\":\"" << s.left_view << "\",\"right_view\":\"" << s.right_view << "\"}";
    return out.str();
}
} // namespace cheeky::foveated_dlss

extern "C" __declspec(dllexport) bool __cdecl
CheekyEyeCalibration_GetBridge(CheekyEyeCalibrationBridgeV1* api) noexcept {
    using namespace cheeky::foveated_dlss;
    if (!api || api->version != 1 || api->size != sizeof(*api))
        return false;
    api->begin = [](std::uint64_t generation, std::uint32_t graphics) noexcept {
        return eye_calibration_frame(EyeCalibrationBackend::openxr, generation, graphics);
    };
    api->capture = [](std::uint64_t generation, void* texture, void* queue, std::uint32_t graphics,
                      std::uint32_t eye, std::uint32_t slice, float u0, float v0, float u1,
                      float v1) noexcept {
        if (graphics == 12)
            return eye_calibration_submit12(static_cast<ID3D12Resource*>(texture),
                                            static_cast<ID3D12CommandQueue*>(queue), eye, u0, v0, u1, v1,
                                            slice, EyeCalibrationBackend::openxr, generation);
        if (graphics != 11)
            return std::uint64_t{};
        return eye_calibration_submit(static_cast<ID3D11Texture2D*>(texture), eye, u0, v0, u1, v1, slice,
                                      EyeCalibrationBackend::openxr, generation);
    };
    api->result = eye_calibration_result;
    api->destroy = eye_calibration_destroy_session;
    return true;
}
