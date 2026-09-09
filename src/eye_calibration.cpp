#include "eye_calibration.hpp"
#include "eye_calibration_pixels.hpp"
#include "settings.hpp"
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
constexpr unsigned ring_size = 8, block = 20, inset = 12;
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
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Query> done, disjoint;
    std::array<ComPtr<ID3D11Query>, 8> timestamp;
    // Source before/after for A and B, then A/B at each submitted eye.
    std::array<Patch, 8> patches;
    std::array<View, 2> views;
    std::array<unsigned, 2> eye_submits{};
    std::array<int, 2> result{{-1, -1}};
    std::array<bool, 4> segments{};
    std::uint64_t sequence{};
    std::uint64_t epoch{}, captured_ms{};
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
    std::uint64_t measurement_start{}, last_valid_sequence{};
    std::uint64_t epoch{};
    ComPtr<ID3D11Device> device;
    std::array<ComPtr<ID3D11Texture2D>, 2> markers;
    DXGI_FORMAT marker_format{};
    EyeCalibrationStats stats;
    Average gpu, latency;
    double cpu_us{};
    std::uint64_t last_frame_ms{};
    bool unsupported_submission{};
};
State& state() {
    // Match the process-resident hook lifetime. Explicit stop releases GPU
    // objects; static destruction must not touch D3D under the loader lock.
    static auto* s = new State;
    return *s;
}
std::atomic<bool> enabled{}, pending{};
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
    if (f.queries_started) {
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
        if (!f.queries_started) {
            f.busy = false;
            if (f.sequence >= s.measurement_start)
                ++s.stats.completed;
            continue;
        }
        if (f.thread != GetCurrentThreadId())
            continue;
        const auto hr = f.context->GetData(f.done.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE)
            continue;
        if (FAILED(hr)) {
            f.busy = false;
            if (f.sequence >= s.measurement_start)
                ++s.stats.completed;
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
            float score{};
            unsigned count{};
            const unsigned candidate = i < 4 ? i / 2 : (i - 4) % 2;
            const unsigned bytes = calibration_pixel_bytes(p.format);
            for (unsigned y = p.height / 5; y < p.height - p.height / 5; ++y)
                for (unsigned x = p.width / 5; x < p.width - p.width / 5; ++x) {
                    const auto pixel = calibration_decode(static_cast<const unsigned char*>(mapped.pData) +
                                                              y * mapped.RowPitch + x * bytes,
                                                          p.format);
                    score += calibration_similarity(pixel, candidate);
                    ++count;
                }
            f.context->Unmap(p.staging.Get(), 0);
            p.score = count ? score / count : 0;
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
        bool valid = !f.invalid && f.evaluations == 2 && f.submits == 2 && f.eye_submits[0] == 1 &&
                     f.eye_submits[1] == 1 && f.result[0] == 0 && f.result[1] == 0;
        for (const auto& p : f.patches)
            valid = valid && p.used && p.ready;
        for (unsigned c = 0; c < 2; ++c) {
            valid = valid && f.patches[c * 2 + 1].score >= 0.8F &&
                    f.patches[c * 2 + 1].score - f.patches[c * 2].score >= 0.3F;
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto& p = f.patches[4 + eye * 2 + c];
                valid =
                    valid && p.reference_width == f.views[c].width && p.reference_height == f.views[c].height;
            }
        }
        const int left = calibration_classify(f.patches[4].score, f.patches[5].score);
        const int right = calibration_classify(f.patches[6].score, f.patches[7].score);
        valid = valid && left >= 0 && right >= 0 && left != right;
        if (valid) {
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
                                               &corrected)) {
                    ++s.stats.applied;
                    if (corrected)
                        ++s.stats.corrections;
                }
            }
        }
        ++s.stats.completed;
        s.latency.add(double(s.sequence - f.sequence));
        f.busy = false;
    }
    pending = std::any_of(s.ring.begin(), s.ring.end(), [](const auto& f) { return f.busy; });
}
bool copy_patch(State& s, Frame& f, unsigned index, ID3D11Texture2D* texture, unsigned x, unsigned y,
                unsigned width, unsigned height, unsigned rw = 0, unsigned rh = 0) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (!calibration_pixel_bytes(desc.Format) || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
        !width || !height || width > 256 || height > 256 || std::uint64_t(x) + width > desc.Width ||
        std::uint64_t(y) + height > desc.Height)
        return false;
    auto& p = f.patches[index];
    if (!p.staging || p.width != width || p.height != height || p.format != desc.Format) {
        p.staging.Reset();
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
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
    f.context->CopySubresourceRegion(p.staging.Get(), 0, 0, 0, 0, texture, 0, &box);
    p.used = true;
    return true;
}
} // namespace
void eye_calibration_enable(bool value) noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (enabled.exchange(value) != value) {
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
    result.openvr_active = s.last_frame_ms && GetTickCount64() - s.last_frame_ms <= 1000;
    result.unsupported_submission = s.unsupported_submission;
    result.correction_active = result.enabled && stereo_eye_assignment(result.left_view).calibrated &&
                               stereo_eye_assignment(result.right_view).calibrated;
    return result;
}
void eye_calibration_reset_stats() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    // Reset measurement, not the established mapping or its ordering guard.
    const auto left = s.stats.left_view, right = s.stats.right_view;
    s.stats = {};
    s.stats.left_view = left;
    s.stats.right_view = right;
    s.cpu_us = 0;
    s.gpu = {};
    s.latency = {};
    s.measurement_start = s.sequence + 1;
}
void eye_calibration_frame() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const bool on = enabled;
    s.last_frame_ms = GetTickCount64();
    if (!on && !pending)
        return;
    CpuScope cpu{s};
    ++s.sequence;
    if (s.current >= 0) {
        finish(s.ring[s.current]);
        s.current = -1;
    }
    poll(s);
    if (!on)
        return;
    ++s.stats.frames;
    // Rotate through all slots so the warm-up is bounded and reproducible.
    for (unsigned n = 0; n < ring_size; ++n) {
        const unsigned i = unsigned((s.sequence + n) % ring_size);
        auto& f = s.ring[i];
        if (f.busy)
            continue;
        f.sequence = s.sequence;
        f.busy = true;
        f.closed = f.invalid = f.queries_started = false;
        f.epoch = s.epoch;
        f.captured_ms = GetTickCount64();
        f.evaluations = f.submits = 0;
        f.views = {};
        f.eye_submits = {};
        f.result = {{-1, -1}};
        f.segments = {};
        for (auto& p : f.patches) {
            p.used = p.ready = false;
            p.score = 0;
        }
        s.current = int(i);
        ++s.stats.captures;
        pending = true;
        return;
    }
    ++s.stats.skipped; // No blocking or overwriting unfinished GPU readbacks.
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
                calibration_encode_marker(data.data() + i * bytes, desc.Format, c);
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
std::uint64_t eye_calibration_submit(ID3D11Texture2D* texture, unsigned eye, float u0, float v0, float u1,
                                     float v1) noexcept {
    if (!enabled || !texture || eye > 1)
        return 0;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    CpuScope cpu{s};
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
        const float nx = float(c ? ref.width - inset - block : inset) / ref.width,
                    ny = float(inset) / ref.height;
        const float ax = (u0 + nx * (u1 - u0)) * desc.Width,
                    bx = (u0 + (nx + float(block) / ref.width) * (u1 - u0)) * desc.Width;
        const float ay = (v0 + ny * (v1 - v0)) * desc.Height,
                    by = (v0 + (ny + float(block) / ref.height) * (v1 - v0)) * desc.Height;
        const unsigned x = unsigned(std::floor((std::min)(ax, bx))),
                       y = unsigned(std::floor((std::min)(ay, by)));
        const unsigned w = unsigned(std::ceil((std::max)(ax, bx))) - x,
                       h = unsigned(std::ceil((std::max)(ay, by))) - y;
        if (!copy_patch(s, f, 4 + eye * 2 + c, texture, x, y, w, h, ref.width, ref.height))
            f.invalid = true;
    }
    context->End(f.timestamp[5 + eye * 2].Get());
    f.segments[2 + eye] = true;
    return ticket_bit | (f.sequence << 1) | eye;
}
void eye_calibration_result(std::uint64_t ticket, int result) noexcept {
    if (!(ticket & ticket_bit))
        return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const auto sequence = (ticket & ~ticket_bit) >> 1;
    for (auto& f : s.ring)
        if (f.busy && f.sequence == sequence)
            f.result[ticket & 1] = result;
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
    s.last_frame_ms = 0;
    s.unsupported_submission = false;
}

const char* eye_calibration_status(const EyeCalibrationStats& stats) noexcept {
    if (!stats.enabled)
        return "Disabled";
    if (!stats.openvr_active)
        return "Waiting for OpenVR (D3D11 only)";
    if (stats.unsupported_submission)
        return "Unsupported OpenVR texture path (requires D3D11 2D)";
    if (stats.correction_active)
        return "Active";
    return "Waiting for a valid D3D11 stereo marker pair";
}

std::string eye_calibration_json() {
    const auto s = eye_calibration_stats();
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::boolalpha << std::setprecision(6)
        << "{\"backend\":\"D3D11 / OpenVR\",\"enabled\":" << s.enabled << ",\"status\":\""
        << eye_calibration_status(s) << "\",\"active\":" << s.correction_active
        << ",\"openvr_active\":" << s.openvr_active
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
        // View identities are pointers; strings preserve all bits through Lua.
        << ",\"left_view\":\"" << s.left_view << "\",\"right_view\":\"" << s.right_view << "\"}";
    return out.str();
}
} // namespace cheeky::foveated_dlss
