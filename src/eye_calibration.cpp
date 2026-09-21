#include "eye_calibration.hpp"
#include "eye_calibration_pixels.hpp"
#include "eye_calibration_d3d12.hpp"
#include "d3d12_native.hpp"
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
#include <bit>
#include <memory>
#include <d3d11_4.h>

namespace cheeky::foveated_dlss {
namespace {
using Microsoft::WRL::ComPtr;
constexpr unsigned ring_size = 8, block = calibration_marker_size, inset = 12;
constexpr std::uint64_t ticket_bit = 1ULL << 63;
struct Patch {
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Device> device;
    unsigned width{}, height{}, reference_width{}, reference_height{};
    unsigned capacity_width{}, capacity_height{};
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
struct Submitted11 {
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Query> done;
    DWORD thread{};
    bool active{}, ready{};
};
struct SupportReadback11 {
    std::shared_ptr<CalibrationImageMemory> memory;
    ComPtr<ID3D11Texture2D> staging;
    bool published{};
};
struct Frame {
    CalibrationImageRequestPtr support;
    std::array<SupportReadback11, 4> support11;
    bool wide_search{};
    std::array<CalibrationSearchPtr, 2> search;
    std::array<SupportReadback11, 2> search11;
    std::array<CalibrationImageInfo, 2> search_info;
    std::shared_ptr<Calibration12Frame> gpu12;
    ComPtr<ID3D12Device> device12;
    bool gpu12_used{}, classified{};
    ComPtr<ID3D11DeviceContext> context;
    std::uintptr_t device_identity{}, context_identity{};
    unsigned device_flags{};
    ComPtr<ID3D11Query> done, disjoint;
    std::array<ComPtr<ID3D11Query>, 8> timestamp;
    // Source before/after for A and B, then A/B at each submitted eye.
    std::array<Patch, calibration_patch_count> patches;
    std::array<CalibrationPlacementPlan, 2> placement_plans;
    std::array<std::uint32_t, calibration_patch_count> patch_codes{};
    std::array<unsigned, calibration_patch_count> patch_mirrors{};
    std::array<CalibrationTrackingPatch, calibration_patch_count> tracking{};
    std::array<CalibrationSearchResult, calibration_patch_count> tracked{};
    std::array<std::array<double, 2>, 2> submitted_sizes{};
    std::array<std::array<bool, calibration_placement_count>, 2> usable_placements{};
    unsigned placement_count{1};
    std::array<Submitted11, 2> submitted11;
    std::array<View, 2> views;
    std::array<std::uint32_t, 2> codes{};
    bool pipelined{}, protected_context{};
    std::array<unsigned, 2> eye_submits{};
    std::array<int, 2> result{{-1, -1}};
    std::array<unsigned, 2> physical_eyes{{0, 1}};
    std::array<bool, 4> segments{};
    std::uint64_t sequence{};
    std::uint64_t epoch{}, captured_ms{}, session_generation{};
    DWORD thread{};
    unsigned evaluations{}, submits{};
    bool busy{}, closed{}, close_requested{}, invalid{}, queries_started{};
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
struct SubmissionContext {
    std::uint64_t sequence{};
    std::uintptr_t source_device{}, source_context{}, submitted_device{}, submitted_context{};
    unsigned source_flags{}, submitted_flags{};
    bool source_protected{}, submitted_protected{};
    HRESULT protection_query{E_PENDING};
    const char* rejection{"none"};
};
struct PlacementLock {
    View view;
    std::array<std::array<double, 2>, 2> submitted_sizes{};
    CalibrationPlacement placement;
    bool locked{};
    bool per_eye{};
    std::array<CalibrationPlacement, 2> eye_placements{};
};
struct State {
    std::mutex mutex;
    std::array<PlacementLock, 2> placements;
    std::array<std::array<double, 2>, 2> submitted_sizes{};
    std::uint64_t placement_epoch{}, placement_sequence{}, placement_locks{}, placement_losses{};
    bool search_needed{};
    unsigned search_candidate{}, tracking_misses{};
    std::uint64_t wide_searches{}, last_wide_search_ms{};
    std::array<CalibrationSearchResult, 2> last_search_results;
    std::array<Frame, ring_size> ring;
    int current{-1};
    std::uint64_t sequence{};
    unsigned frames_until_capture{};
    std::uint64_t measurement_start{}, last_valid_sequence{};
    std::uint64_t epoch{};
    ComPtr<ID3D11Device> device;
    std::array<ComPtr<ID3D11Texture2D>, 2> markers;
    DXGI_FORMAT marker_format{};
    std::array<CalibrationMarkerPoints, 2> marker_layouts;
    EyeCalibrationStats stats;
    Average gpu, latency;
    double cpu_us{};
    std::uint64_t last_frame_ms{}, last_openvr_ms{}, session_generation{};
    EyeCalibrationBackend backend{};
    bool unsupported_submission{};
    std::uint64_t finish_wrong_thread{}, submit_wrong_thread{}, poll_wrong_thread{};
    std::uint64_t protected_submits{}, carried_frames{}, waiting_for_sources{};
    std::uint64_t cross_device_submits{};
    SubmissionContext submission_context;
    DWORD frame_thread{}, stamp_thread{}, submit_thread{}, tick_thread{};
    std::uint64_t continuous_epoch{};
    std::array<View, 2> continuous_views;
    // Separate from sampled proof: never extend a capture's lifetime just to
    // mark an intervening render. Full pools skip without waiting on the GPU.
    std::array<std::shared_ptr<Calibration12Frame>, ring_size * 2> continuous12;
    ComPtr<ID3D12Device> continuous12_device;
    unsigned continuous12_next{};
};
State& state() {
    // Match the process-resident hook lifetime. Explicit stop releases GPU
    // objects; static destruction must not touch D3D under the loader lock.
    static auto* s = new State;
    return *s;
}
std::atomic<bool> enabled{}, pending{};
void placement_epoch(State& s) {
    if (s.placement_epoch == s.epoch) return;
    s.placement_epoch = s.epoch;
    s.placements = {}; s.submitted_sizes = {}; s.placement_sequence = 0;
    s.search_needed = false; s.last_wide_search_ms = 0;
    s.search_candidate = s.tracking_misses = 0;
}
CalibrationPlacementPlan source_placement(State& s, unsigned c, std::uint64_t view, unsigned width, unsigned height) {
    placement_epoch(s);
    auto& lock = s.placements[c];
    if (lock.locked && lock.view.id == view && lock.view.generation == stereo_view_generation(view) &&
        lock.view.width == width && lock.view.height == height && lock.submitted_sizes == s.submitted_sizes) {
        CalibrationPlacementPlan plan; plan.placements[0] = lock.placement;
        plan.per_eye = lock.per_eye; plan.eye_placements = lock.eye_placements; return plan;
    }
    if (lock.locked) { ++s.placement_losses; s.search_needed = true; }
    lock.locked = false;
    // Probe only one corner and its nearby clipping fallback at a time.
    // A recent lost lock gets first chance at its known location before cycling
    // the bounded hypotheses. Never put the full hypothesis bank on screen.
    const bool known = lock.view.id == view && lock.view.generation == stereo_view_generation(view) &&
        lock.view.width == width && lock.view.height == height && lock.placement.width > 0;
    if (known && !s.search_candidate)
        return calibration_corner_pair(calibration_padded_corner(lock.placement, width, height, c,
            s.submitted_sizes[c][0], s.submitted_sizes[c][1]), width, height, c);
    const auto bank = calibration_placement_plan(width, height, c, s.submitted_sizes[0][0], s.submitted_sizes[0][1]);
    const unsigned index = (s.search_candidate - unsigned(known)) % bank.count;
    return calibration_corner_pair(bank.at(index), width, height, c);
}
void submitted_geometry(State& s, Frame& f, unsigned eye, unsigned width, unsigned height,
                        float u0, float v0, float u1, float v1) {
    placement_epoch(s);
    f.submitted_sizes[eye] = {double(width) * std::abs(double(u1) - u0), double(height) * std::abs(double(v1) - v0)};
    if (f.epoch == s.epoch) s.submitted_sizes[eye] = f.submitted_sizes[eye];
    f.placement_count = (std::max)(f.placement_plans[0].count, f.placement_plans[1].count);
}
std::array<unsigned, 4> submitted_rect(Frame& f, unsigned eye, unsigned index, unsigned width, unsigned height,
                                     float u0, float v0, float u1, float v1) {
    const unsigned c = index % 2, h = index / 4;
    const auto source = f.views[c].width ? c : (f.views[0].width ? 0U : 1U);
    // Missing source in a single-source pipeline still probes the absent code,
    // using the same geometry but its own left/right marker position.
    auto plan = f.placement_plans[source];
    if (source != c) {
        for (unsigned i = 0; i < plan.count; ++i)
            plan.placements[i].marker.x = f.views[source].width - block - plan.placements[i].marker.x;
    }
    const auto& placement = plan.for_eye(eye, h);
    f.patch_codes[calibration_patch_index(index, eye)] = calibration_location_code(placement.marker,
        f.views[source].width, c, f.codes[c]);
    const unsigned mirror = (u1 < u0 ? 1U : 0U) | ((v1 < v0) != (index % 4 >= 2) ? 2U : 0U);
    f.patch_mirrors[calibration_patch_index(index, eye)] = 1U << mirror;
    auto rect = calibration_sample_rect(placement, index % 4 >= 2, width, height, u0, v0, u1, v1);
    auto& tracking = f.tracking[calibration_patch_index(index, eye)];
    tracking = {};
    if (plan.per_eye) {
        auto coded = placement;
        coded.marker.code = f.patch_codes[calibration_patch_index(index, eye)];
        tracking = calibration_tracking_patch(coded, c, index % 4 >= 2, width, height, u0, v0, u1, v1);
        if (tracking.enabled) rect = tracking.rect;
    }
    if (!rect[2] || !rect[3]) { f.usable_placements[eye][h] = false; rect = {0, 0, 1, 1}; }
    return rect;
}
CalibrationSearchPtr prepare_search(Frame& f, unsigned eye) noexcept try {
    if (!f.wide_search) return {};
    auto request = std::make_shared<CalibrationSearch>();
    for (unsigned c = 0; c < 2; ++c) {
        if (!f.views[c].id) continue;
        const auto points = calibration_marker_points(f.placement_plans[c], 0, 0, f.views[c].width, c, f.codes[c]);
        for (unsigned i = 0; i < points.count; ++i)
            request->targets.push_back({points.points[i], c, f.views[c].width, f.views[c].height});
    }
    return f.search[eye] = request;
} catch (...) { return {}; }
void capture_search11(Frame& f, unsigned eye, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
    const CalibrationImageInfo& info) noexcept try {
    auto request = prepare_search(f, eye);
    if (!request) return;
    f.search_info[eye] = info;
    auto& capture = f.search11[eye];
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    const auto bytes = calibration_pixel_bytes(desc.Format);
    if (!bytes || desc.SampleDesc.Count != 1 || info.slice >= desc.ArraySize) { request->ready = true; return; }
    capture.memory = reserve_calibration_image_memory(std::uint64_t((desc.Width * bytes + 255) & ~255U) * desc.Height);
    if (!capture.memory) { request->ready = true; return; }
    const auto subresource = info.slice * desc.MipLevels;
    desc.MipLevels = desc.ArraySize = 1; desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Device> device; texture->GetDevice(&device);
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &capture.staging))) {
        capture.memory.reset(); request->ready = true; return;
    }
    context->CopySubresourceRegion(capture.staging.Get(), 0, 0, 0, 0, texture, subresource, nullptr);
} catch (...) { if (f.search[eye]) f.search[eye]->ready = true; }
bool poll_search11(Frame& f, unsigned eye, ID3D11DeviceContext* context) {
    auto& capture = f.search11[eye];
    if (!capture.staging) return true;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto hr = context->Map(capture.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
    CalibrationSearchImage image;
    if (SUCCEEDED(hr)) {
        const auto& info = f.search_info[eye];
        try { image = calibration_search_image(mapped.pData, mapped.RowPitch, info.width, info.height, info.format, info.bounds); }
        catch (...) {}
        context->Unmap(capture.staging.Get(), 0);
    }
    calibration_search_start(f.search[eye], std::move(image));
    capture.staging.Reset(); capture.memory.reset();
    return true;
}

// Codes remain stable for a calibration epoch so delayed submissions can
// match. Candidate slots remain bound to view identities throughout that epoch.
std::array<std::uint32_t, 2> capture_codes(std::uint64_t sequence) {
    std::array<std::uint32_t, 2> codes{};
    auto random = [&] {
        sequence += 0x9e3779b97f4a7c15ULL;
        auto v = sequence;
        v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
        v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
        return std::uint32_t((v ^ (v >> 31)) & 0x1ffffffU);
    };
    for (unsigned c = 0; c < 2; ++c) {
        for (;;) {
            const auto code = random();
            if (std::popcount(code) < 10 || std::popcount(code) > 15) continue;
            bool distinct = true;
            for (unsigned mirror = 0; c && mirror < 4; ++mirror) {
                std::uint32_t reflected{};
                for (unsigned y = 0; y < 5; ++y) for (unsigned x = 0; x < 5; ++x)
                    reflected |= unsigned(calibration_pattern_bit(0, mirror & 1 ? 4 - x : x,
                        mirror & 2 ? 4 - y : y, codes[0])) << (y * 5 + x);
                if (std::popcount(code ^ reflected) < 8) distinct = false;
            }
            if (distinct) { codes[c] = code; break; }
        }
    }
    return codes;
}
struct ContextLock {
    ComPtr<ID3D11Multithread> protection;
    HRESULT query_result{E_POINTER};
    explicit ContextLock(ID3D11DeviceContext* context) {
        if (context && SUCCEEDED(query_result = context->QueryInterface(IID_PPV_ARGS(&protection)))) {
            if (protection->GetMultithreadProtected()) protection->Enter();
            else protection.Reset();
        }
    }
    ~ContextLock() { if (protection) protection->Leave(); }
};
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
    for (unsigned i = 0; i < s.stats.last_rejected_scores.size(); ++i)
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
    ComPtr<IUnknown> device_identity, context_identity;
    device.As(&device_identity);
    context->QueryInterface(IID_PPV_ARGS(&context_identity));
    f.device_identity = reinterpret_cast<std::uintptr_t>(device_identity.Get());
    f.context_identity = reinterpret_cast<std::uintptr_t>(context_identity.Get());
    f.device_flags = device->GetCreationFlags();
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
void finish(State& s, Frame& f) {
    if (!f.busy || f.closed)
        return;
    f.close_requested = true;
    if (!f.gpu12_used && f.queries_started) {
        if (f.thread != GetCurrentThreadId()) {
            ++s.finish_wrong_thread;
            return;
        }
        f.context->End(f.disjoint.Get());
        f.context->End(f.done.Get());
    }
    f.closed = true;
}
void capture_support11(Frame& f, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
    unsigned index, CalibrationImageInfo info) noexcept {
    if (!begin_calibration_image(f.support, index, info)) return;
    try {
        auto& capture = f.support11[index];
        D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
        const auto bytes = calibration_pixel_bytes(desc.Format);
        if (!bytes || desc.SampleDesc.Count != 1 || info.slice >= desc.ArraySize) {
            fail_calibration_image(f.support, index, "unsupported_texture_layout"); return;
        }
        const auto source_subresource = info.slice * desc.MipLevels;
        capture.memory = reserve_calibration_image_memory(std::uint64_t((desc.Width * bytes + 255) & ~255U) * desc.Height);
        if (!capture.memory) { fail_calibration_image(f.support, index, "readback_memory_limit"); return; }
        desc.MipLevels = desc.ArraySize = 1; desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Device> device; texture->GetDevice(&device);
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &capture.staging))) {
            capture.memory.reset(); fail_calibration_image(f.support, index, "readback_allocation_failed"); return;
        }
        context->CopySubresourceRegion(capture.staging.Get(), 0, 0, 0, 0, texture, source_subresource, nullptr);
    } catch (...) { fail_calibration_image(f.support, index, "readback_allocation_failed"); }
}
bool poll_support11(Frame& f, ID3D11DeviceContext* context, unsigned index) noexcept {
    auto& capture = f.support11[index];
    if (!capture.staging || capture.published) return true;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto hr = context->Map(capture.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
    if (SUCCEEDED(hr)) {
        complete_calibration_image(f.support, index, mapped.pData, mapped.RowPitch);
        context->Unmap(capture.staging.Get(), 0);
    } else fail_calibration_image(f.support, index, "readback_map_failed");
    capture.published = true;
    capture.staging.Reset(); capture.memory.reset();
    return true;
}
void poll_submitted(Frame& f) {
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto& capture = f.submitted11[eye];
        if (!capture.active || capture.ready || capture.thread != GetCurrentThreadId()) continue;
        const auto hr = capture.context->GetData(capture.done.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE) continue;
        if (FAILED(hr)) { f.invalid = true; capture.ready = true; continue; }
        bool waiting = !poll_support11(f, capture.context.Get(), 2 + eye);
        waiting |= !poll_search11(f, eye, capture.context.Get());
        for (unsigned index = 0; index < f.placement_count * 4; ++index) {
            const unsigned c = index % 2;
            auto& p = f.patches[calibration_patch_index(index, eye)];
            if (!p.used || p.ready) continue;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const auto result = capture.context->Map(p.staging.Get(), 0, D3D11_MAP_READ,
                D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (result == DXGI_ERROR_WAS_STILL_DRAWING) { waiting = true; continue; }
            if (FAILED(result)) { f.invalid = true; p.ready = true; continue; }
            const auto patch_index = calibration_patch_index(index, eye);
            if (f.tracking[patch_index].enabled) {
                f.tracked[patch_index] = calibration_track(mapped.pData, mapped.RowPitch, p.format, f.tracking[patch_index]);
                p.score = f.tracked[patch_index].valid ? f.tracked[patch_index].score : 0;
            } else p.score = calibration_pattern_score(mapped.pData, mapped.RowPitch, p.width, p.height,
                p.format, c, true, f.patch_codes[calibration_patch_index(index, eye)],
                f.patch_mirrors[calibration_patch_index(index, eye)]);
            capture.context->Unmap(p.staging.Get(), 0);
            p.ready = true;
        }
        capture.ready = !waiting;
    }
}
void poll(State& s) {
    for (auto& f : s.ring) {
        if (f.epoch != s.epoch || !enabled) for (auto& request : f.search)
            if (request) request->canceled = true;
        // Submission textures can belong to a second D3D11 device. Its tiny
        // copies and completion query must be read on that context's thread.
        if (f.busy) poll_submitted(f);
        // XR may end the interval on its submission thread. Keep the slot
        // alive and close its D3D11 queries when the render thread returns;
        // the calibration mutex alone does not make the context thread-safe.
        if (f.busy && !f.closed && f.close_requested && !f.gpu12_used &&
            f.thread == GetCurrentThreadId()) finish(s, f);
        if (!f.busy || !f.closed)
            continue;
        // A mixed frame owns two independent GPU timelines. Do not classify or
        // recycle either half until submission-thread DX11 queries also finish.
        if (std::any_of(f.submitted11.begin(), f.submitted11.end(),
            [](const auto& capture) { return capture.active && !capture.ready; })) continue;
        if (f.search[0] && f.search[1] && f.search[0]->started && f.search[1]->started &&
            (!f.search[0]->ready.load(std::memory_order_acquire) || !f.search[1]->ready.load(std::memory_order_acquire))) continue;
        bool gpu12_reusable{};
        const unsigned source_mask = (f.views[0].id ? 1U : 0U) | (f.views[1].id ? 2U : 0U);
        const bool mono = f.pipelined && f.evaluations == 1 && (source_mask == 1 || source_mask == 2);
        if (f.gpu12_used) {
            const bool mixed = f.pipelined;
            const auto result = calibration12_poll(*f.gpu12, mixed, mixed ? source_mask : 3U);
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
            for (unsigned i = 0; i < (mixed ? 4U : 4U + f.placement_count * 8); ++i) {
                if (mixed && !(source_mask & (1U << (i / 2)))) continue;
                f.patches[i].used = f.patches[i].ready = true;
                f.patches[i].score = result.scores[i];
                f.tracked[i] = result.tracked[i];
            }
            if (result.timing_valid) {
                s.stats.gpu_timing_status = "Available";
                s.gpu.add(result.gpu_us);
                ++s.stats.gpu_samples;
                s.stats.max_gpu_us = (std::max)(s.stats.max_gpu_us, result.gpu_us);
            } else s.stats.gpu_timing_status = mixed ? "Unavailable across D3D12 source and D3D11 submission" :
                "D3D12 marker/copy timestamps unavailable for this capture";
        } else {
            if (!f.queries_started) {
                f.busy = false;
                if (f.sequence >= s.measurement_start) {
                    ++s.stats.completed;
                    record_rejection(s, f, 1U | (f.evaluations != 2 ? 2U : 0U));
                }
                continue;
            }
            if (f.thread != GetCurrentThreadId()) {
                ++s.poll_wrong_thread;
                continue;
            }
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
            for (unsigned i = 0; i < 4; ++i) {
                if (i >= 2 && f.submitted11[i - 2].active) continue;
                if (!poll_support11(f, f.context.Get(), i)) waiting = true;
            }
            for (unsigned i = 0; i < 4 + f.placement_count * 8; ++i) {
                if (i >= 4 && f.submitted11[((i - 4) % 4) / 2].active) continue;
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
                if (i >= 4 && f.tracking[i].enabled) {
                    f.tracked[i] = calibration_track(mapped.pData, mapped.RowPitch, p.format, f.tracking[i]);
                    p.score = f.tracked[i].valid ? f.tracked[i].score : 0;
                } else p.score = calibration_pattern_score(mapped.pData, mapped.RowPitch, p.width, p.height,
                                                    p.format, candidate, i >= 4, f.patch_codes[i], i < 4 ? 1U : f.patch_mirrors[i]);
                f.context->Unmap(p.staging.Get(), 0);
                p.ready = true;
            }
            if (waiting)
                continue;
            for (unsigned eye = 0; eye < 2; ++eye)
                waiting |= !poll_search11(f, eye, f.context.Get());
            if (waiting) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            const auto timing_result = f.context->GetData(f.disjoint.Get(), &disjoint, sizeof(disjoint),
                                   D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (std::any_of(f.submitted11.begin(), f.submitted11.end(), [](const auto& capture) { return capture.active; }))
                s.stats.gpu_timing_status = "Unavailable across separate D3D11 devices (submission copies have no timestamps)";
            else if (!std::all_of(f.segments.begin(), f.segments.end(), [](bool segment) { return segment; }))
                s.stats.gpu_timing_status = "Incomplete marker/copy timestamp coverage";
            else if (timing_result == S_FALSE)
                s.stats.gpu_timing_status = "D3D11 timestamp results were not ready when the capture completed";
            else if (FAILED(timing_result))
                s.stats.gpu_timing_status = "D3D11 timestamp query failed";
            else if (disjoint.Disjoint || !disjoint.Frequency)
                s.stats.gpu_timing_status = "D3D11 timestamps invalid (GPU clock disjoint or frequency unavailable)";
            else s.stats.gpu_timing_status = "D3D11 marker/copy timestamps incomplete or not ready";
            if (timing_result == S_OK &&
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
                    s.stats.gpu_timing_status = "Available";
                    s.gpu.add(us);
                    s.stats.max_gpu_us = (std::max)(s.stats.max_gpu_us, us);
                    ++s.stats.gpu_samples;
                }
            }
        }
        if (std::any_of(f.search.begin(), f.search.end(), [](const auto& search) {
            return search && !search->ready.load(std::memory_order_acquire);
        })) continue;
        unsigned rejection = f.invalid ? 1U : 0U;
        if (f.evaluations != 2 && !mono) rejection |= 2U;
        if (f.submits != 2 || f.eye_submits[0] != 1 || f.eye_submits[1] != 1) rejection |= 4U;
        if (f.result[0] != 0 || f.result[1] != 0) rejection |= 8U;
        for (unsigned i = 0; i < 4 + f.placement_count * 8; ++i) {
            if (mono && i < 4 && !(source_mask & (1U << (i / 2)))) continue;
            if (!f.patches[i].used || !f.patches[i].ready) rejection |= 16U;
        }
        for (unsigned c = 0; c < 2; ++c) {
            if (mono && !(source_mask & (1U << c))) continue;
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
        int left = -1, right = -1, selected = -1, selected_right = -1;
        bool flipped_pair{}, ambiguous{};
        float selected_score{};
        double selected_edge = 1e30;
        for (unsigned h = 0; h < f.placement_count; ++h) for (unsigned rh = 0; rh < f.placement_count; ++rh) {
            if (!f.usable_placements[left_slot][h] || !f.usable_placements[right_slot][rh]) continue;
            for (unsigned flip = 0; flip < 2; ++flip) {
                const auto base = 4 + h * 8 + flip * 4;
                const auto right_base = 4 + rh * 8 + flip * 4;
                const int a = calibration_pattern_classify(f.patches[base + left_slot * 2].score,
                                                          f.patches[base + left_slot * 2 + 1].score);
                const int b = calibration_pattern_classify(f.patches[right_base + right_slot * 2].score,
                                                          f.patches[right_base + right_slot * 2 + 1].score);
                if (a < 0 || b < 0 || (mono ? a != b || !(source_mask & (1U << a)) : a == b)) continue;
                if (selected >= 0 && (left != a || right != b || flipped_pair != bool(flip))) ambiguous = true;
                const auto score = (std::min)(f.patches[base + left_slot * 2 + unsigned(a)].score,
                                              f.patches[right_base + right_slot * 2 + unsigned(b)].score);
                const auto edge_distance = [](const CalibrationPlacement& p) {
                    return calibration_corner_distance(p.marker.x - p.x, p.marker.y - p.y, p.width, p.height);
                };
                const double edge = edge_distance(f.placement_plans[a].for_eye(left_slot, h)) +
                    edge_distance(f.placement_plans[b].for_eye(right_slot, rh));
                if (selected < 0 || edge < selected_edge || (edge == selected_edge && score > selected_score)) {
                    selected = int(h); selected_right = int(rh); left = a; right = b; flipped_pair = flip != 0; selected_score = score;
                    selected_edge = edge;
                }
            }
        }
        bool acquired{};
        if (f.wide_search) for (unsigned eye = 0; eye < 2; ++eye)
            s.last_search_results[eye] = f.search[eye] ? f.search[eye]->result : CalibrationSearchResult{};
        if (selected < 0 && !ambiguous && f.search[0] && f.search[1]) {
            const auto& a = f.search[left_slot]->result;
            const auto& b = f.search[right_slot]->result;
            if (a.valid && b.valid && !a.ambiguous && !b.ambiguous && a.flipped == b.flipped &&
                (mono ? a.candidate == b.candidate && (source_mask & (1U << a.candidate)) : a.candidate != b.candidate)) {
                left = int(a.candidate); right = int(b.candidate); flipped_pair = a.flipped;
                acquired = true;
            }
        }
        if ((selected < 0 && !acquired) || ambiguous) rejection |= 128U;
        // A failed wide search only advances acquisition; it never authenticates
        // pixels. Its CPU work can exceed the publication/tracking age limit.
        // Discarding that failure would retry the same clipped corner forever.
        const bool failed_wide_search = f.wide_search && rejection == 128U;
        // Do not let an old epoch or out-of-order completion undo a newer placement.
        if (enabled && f.epoch == s.epoch && f.sequence > s.placement_sequence &&
            (failed_wide_search || GetTickCount64() - f.captured_ms < (f.wide_search ? 10000U : 1000U))) {
            placement_epoch(s);
            s.placement_sequence = f.sequence;
            if (!rejection) {
                s.tracking_misses = s.search_candidate = 0;
                for (unsigned c = 0; c < 2; ++c) {
                    if (!(source_mask & (1U << c))) continue;
                    if (!s.placements[c].locked) ++s.placement_locks;
                    auto& learned = s.placements[c];
                    if (acquired) {
                        const unsigned first_eye = f.search[0]->result.candidate == c ? 0U : 1U;
                        learned = {f.views[c], f.submitted_sizes, f.search[first_eye]->result.placement, true, true, {}};
                        for (unsigned eye = 0; eye < 2; ++eye)
                            learned.eye_placements[eye] = f.search[eye]->result.candidate == c ?
                                f.search[eye]->result.placement : learned.placement;
                    } else {
                        const auto& plan = f.placement_plans[c];
                        const unsigned eye = int(c) == left ? left_slot : right_slot;
                        const auto chosen_for = [&](unsigned physical_eye, unsigned hypothesis) {
                            const unsigned patch = 4 + hypothesis * 8 + unsigned(flipped_pair) * 4 + physical_eye * 2 + c;
                            return f.tracked[patch].valid ? f.tracked[patch].placement : plan.for_eye(physical_eye, hypothesis);
                        };
                        const auto chosen = chosen_for(eye, unsigned(int(c) == left ? selected : selected_right));
                        learned = {f.views[c], f.submitted_sizes, chosen, true, true, plan.eye_placements};
                        if (learned.per_eye) {
                            learned.eye_placements.fill(chosen);
                            if (int(c) == left) learned.eye_placements[left_slot] = chosen_for(left_slot, unsigned(selected));
                            if (int(c) == right) learned.eye_placements[right_slot] = chosen_for(right_slot, unsigned(selected_right));
                        }
                    }
                }
                s.search_needed = false;
                // A slow acquisition may only seed tracking. Publication still
                // uses the normal age check; verify immediately on fresh pixels.
                if (f.wide_search) s.frames_until_capture = 0;
            } else if (rejection == 128U) {
                const bool locked = s.placements[0].locked || s.placements[1].locked;
                // A missed readback is not proof that the placement was lost.
                // Keep the one-marker layout for three independent bad samples;
                // failed samples still cannot publish/refresh eye identity.
                if (!locked || ++s.tracking_misses >= 3) {
                    s.search_needed = true;
                    if (locked) {
                        for (auto& lock : s.placements) {
                            if (lock.locked) ++s.placement_losses;
                            lock.locked = false;
                        }
                        s.search_candidate = 0;
                        s.frames_until_capture = 0;
                    } else ++s.search_candidate;
                    s.tracking_misses = 0;
                }
            }
        }
        if (rejection) record_rejection(s, f, rejection);
        if (!rejection) {
            if (mono) calibration_image_shared_source(f.support, unsigned(left));
            ++s.stats.valid;
            if (f.sequence > s.last_valid_sequence) {
                s.stats.left_view = f.views[left].id;
                s.stats.right_view = f.views[right].id;
                s.last_valid_sequence = f.sequence;
            }
            if (!mono && f.views[left].assigned >= 0 && f.views[right].assigned >= 0 &&
                (f.views[left].assigned != 0 || f.views[right].assigned != 1))
                ++s.stats.mismatches;
            // Readbacks may complete out of order or after a toggle. The settings
            // layer additionally verifies ordering, age and handle lifetimes.
            if (enabled && f.epoch == s.epoch) {
                bool corrected{};
                if (publish_stereo_calibration(f.views[left].id, f.views[right].id, f.views[left].generation,
                                               f.views[right].generation, f.sequence, f.captured_ms,
                                               &corrected, f.session_generation, flipped_pair, mono)) {
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
                unsigned width, unsigned height, unsigned rw = 0, unsigned rh = 0, unsigned slice = 0,
                ID3D11DeviceContext* capture_context = nullptr) {
    auto* context = capture_context ? capture_context : f.context.Get();
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (!calibration_pixel_bytes(desc.Format) || slice >= desc.ArraySize || desc.SampleDesc.Count != 1 ||
        !width || !height || width > 256 || height > 256 || std::uint64_t(x) + width > desc.Width ||
        std::uint64_t(y) + height > desc.Height)
        return false;
    const auto subresource = D3D11CalcSubresource(0, slice, desc.MipLevels);
    auto& p = f.patches[index];
    // Recentered edge patches change shape by a few pixels. Reserve their
    // bounded maximum once instead of reallocating GPU resources on each fit.
    const unsigned capacity_width = f.tracking[index].enabled ? 256U : width;
    const unsigned capacity_height = f.tracking[index].enabled ? 256U : height;
    if (!p.staging || p.device != device || p.capacity_width != capacity_width ||
        p.capacity_height != capacity_height || p.format != desc.Format) {
        p.staging.Reset();
        desc.Width = capacity_width;
        desc.Height = capacity_height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &p.staging)))
            return false;
        p.device = device;
        ++s.stats.allocations;
        p.capacity_width = capacity_width;
        p.capacity_height = capacity_height;
        p.format = desc.Format;
    }
    p.width = width;
    p.height = height;
    p.reference_width = rw;
    p.reference_height = rh;
    const D3D11_BOX box{x, y, 0, x + width, y + height, 1};
    context->CopySubresourceRegion(p.staging.Get(), 0, 0, 0, 0, texture, subresource, &box);
    p.used = true;
    return true;
}
bool prepare_marker(State& s, ID3D11DeviceContext* context, DXGI_FORMAT format,
                    unsigned candidate, const CalibrationMarkerPoints& points) {
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (s.device.Get() != device.Get() || s.marker_format != format) {
        s.markers = {};
        s.device = device;
        s.marker_format = format;
    }
    if (s.markers[candidate] && s.marker_layouts[candidate] == points) return true;
    const auto bytes = calibration_pixel_bytes(format);
    std::vector<unsigned char> data(block * block * calibration_placement_count * bytes);
    for (unsigned n = 0; n < points.count; ++n)
        for (unsigned i = 0; i < block * block; ++i)
            calibration_encode_pattern(data.data() + (n * block * block + i) * bytes, format, candidate,
                                       i % block, i / block, points.points[n].code);
    if (!s.markers[candidate]) {
        const D3D11_TEXTURE2D_DESC desc{block, block * calibration_placement_count, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA initial{data.data(), block * bytes, 0};
        if (FAILED(device->CreateTexture2D(&desc, &initial, &s.markers[candidate]))) return false;
        ++s.stats.allocations;
    } else context->UpdateSubresource(s.markers[candidate].Get(), 0, nullptr, data.data(), block * bytes, 0);
    s.marker_layouts[candidate] = points;
    return true;
}
void stamp_points11(State& s, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                    unsigned c, const CalibrationMarkerPoints& points) {
    for (unsigned i = 0; i < points.count; ++i) {
        const D3D11_BOX box{0, i * block, 0, block, (i + 1) * block, 1};
        context->CopySubresourceRegion(texture, 0, points.points[i].x, points.points[i].y,
            0, s.markers[c].Get(), 0, &box);
    }
}
unsigned continuous_candidate(State& s, std::uint64_t view) {
    if (s.continuous_epoch != s.epoch) {
        s.continuous_views = {};
        s.continuous_epoch = s.epoch;
    }
    const auto generation = stereo_view_generation(view);
    // Replacing a source invalidates delayed pixels from its predecessor.
    for (const auto& v : s.continuous_views) {
        if (v.id && stereo_view_generation(v.id) != v.generation) {
            ++s.epoch;
            s.frames_until_capture = 0;
            clear_stereo_calibration();
            s.continuous_views = {};
            s.continuous_epoch = s.epoch;
            break;
        }
    }
    for (unsigned i = 0; i < 2; ++i)
        if (s.continuous_views[i].id == view && s.continuous_views[i].generation == generation) return i;
    for (unsigned i = 0; i < 2; ++i) {
        auto& v = s.continuous_views[i];
        if (!v.id || stereo_view_generation(v.id) != v.generation) {
            v.id = view; v.generation = generation;
            return i;
        }
    }
    return 2;
}
void continuous_stamp_only(State& s, ID3D11DeviceContext* context, ID3D11Resource* output,
                           unsigned c, std::uint64_t view, unsigned x, unsigned y, unsigned width, unsigned height) {
    if (c >= 2 || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(&texture)))) return;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (!calibration_pixel_bytes(desc.Format) || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
        width < 2 * inset + block || height < 2 * inset + block ||
        std::uint64_t(x) + width > desc.Width || std::uint64_t(y) + height > desc.Height) return;
    const auto points = calibration_marker_points(source_placement(s, c, view, width, height), x, y, width, c, capture_codes(s.epoch)[c]);
    if (!prepare_marker(s, context, desc.Format, c, points)) return;
    stamp_points11(s, context, texture.Get(), c, points);
}
void continuous_stamp_only12(State& s, ID3D12GraphicsCommandList* list, ID3D12Resource* output,
    unsigned c, std::uint64_t view, unsigned x, unsigned y, unsigned width, unsigned height, D3D12_RESOURCE_STATES output_state) {
    if (c >= 2 || width < 2 * inset + block || height < 2 * inset + block) return;
    const auto desc = output->GetDesc();
    if (std::uint64_t(x) + width > desc.Width || std::uint64_t(y) + height > desc.Height) return;
    ComPtr<ID3D12Device> device;
    if (FAILED(output->GetDevice(IID_PPV_ARGS(&device)))) return;
    device = canonical_d3d12_device(device.Get());
    if (!device) return;
    if (!same_d3d12_device(device.Get(), s.continuous12_device.Get())) {
        // The backend registry retains any recordings still in flight.
        s.continuous12 = {};
        s.continuous12_device = device;
        s.continuous12_next = 0;
    }
    for (unsigned n = 0; n < s.continuous12.size(); ++n) {
        const unsigned slot = (s.continuous12_next + n) % unsigned(s.continuous12.size());
        auto& frame = s.continuous12[slot];
        if (!frame) frame = calibration12_create(device.Get());
        if (!frame || !calibration12_begin(*frame, &s.stats.allocations)) continue;
        s.continuous12_next = (slot + 1) % unsigned(s.continuous12.size());
        Calibration12Failure failure;
        const auto points = calibration_marker_points(source_placement(s, c, view, width, height), x, y, width, c, capture_codes(s.epoch)[c]);
        if (calibration12_stamp(*frame, list, output, c, points.points[0].x,
            points.points[0].y, output_state, s.stats.allocations, &failure, {}, {}, points.points[0].code,
            Calibration12StampMode::marker_only, points.extra())) {
            ++s.stats.d3d12_continuous_stamps;
        } else {
            ++s.stats.d3d12_stamp_failures;
            s.stats.d3d12_last_stamp_failure = failure.stage;
            s.stats.d3d12_stamp_error = failure.result;
        }
        return;
    }
    ++s.stats.d3d12_continuous_skipped;
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
    const auto source_api = s.stats.source_graphics_api, submission_api = s.stats.submission_graphics_api;
    s.stats = {};
    s.stats.graphics_api = graphics_api;
    s.stats.source_graphics_api = source_api; s.stats.submission_graphics_api = submission_api;
    s.stats.left_view = left;
    s.stats.right_view = right;
    s.cpu_us = 0;
    s.gpu = {};
    s.latency = {};
    s.placement_locks = s.placement_losses = s.wide_searches = 0;
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
    if (graphics_api) {
        s.stats.graphics_api = graphics_api;
        s.stats.submission_graphics_api = graphics_api;
    }
    s.last_frame_ms = GetTickCount64();
    if (!on && !pending)
        return false;
    CpuScope cpu{s};
    ++s.sequence;
    s.frame_thread = GetCurrentThreadId();
    if (on) ++s.stats.frames;
    const bool capture_due = s.frames_until_capture == 0;
    if (s.frames_until_capture) --s.frames_until_capture;
    if (s.current >= 0) {
        auto& f = s.ring[s.current];
        // AER can submit the previous eye pair between the two DLSS renders.
        // Keep sources across intervals, but never combine submission pairs.
        if (on && f.pipelined && f.epoch == s.epoch && !f.invalid && !f.submits &&
            s.sequence - f.sequence < 4 && now - f.captured_ms < 250) {
            ++s.carried_frames;
            poll(s);
            return true;
        }
        finish(s, f);
        s.current = -1;
    }
    poll(s);
    if (!on)
        return false;
    placement_epoch(s);
    // A single acquisition pair owns the wide readbacks and CPU workers.
    // Keep only the current corner pair/selected marker; never queue more full-image searches.
    if (std::any_of(s.ring.begin(), s.ring.end(), [](const auto& frame) {
        return frame.busy && frame.wide_search;
    })) return false;
    // Drain readbacks every frame and sample one in ten. DX11 OpenXR
    // submissions use stable corner stamps on intervening renders, including DX12 sources.
    if (!capture_due) return false;
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
        f.pipelined = backend == EyeCalibrationBackend::openxr && graphics_api == 11;
        f.protected_context = false;
        f.codes = f.pipelined ? capture_codes(s.epoch) :
            std::array<std::uint32_t, 2>{};
        f.sequence = s.sequence;
        f.support = claim_calibration_images(f.sequence, session_generation, f.codes);
        f.support11 = {};
        f.search = {}; f.search11 = {};
        f.wide_search = s.search_needed && (!s.last_wide_search_ms || now - s.last_wide_search_ms >= 1000);
        if (f.wide_search) { ++s.wide_searches; s.last_wide_search_ms = now; }
        f.busy = true;
        f.closed = f.close_requested = f.invalid = f.queries_started = false;
        f.epoch = s.epoch;
        f.session_generation = session_generation;
        f.captured_ms = GetTickCount64();
        f.evaluations = f.submits = 0;
        f.views = {};
        f.placement_plans = {}; f.submitted_sizes = {}; f.placement_count = 1;
        f.tracking = {}; f.tracked = {};
        for (auto& usable : f.usable_placements) usable.fill(true);
        for (auto& capture : f.submitted11) capture.active = capture.ready = false;
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
    s.tick_thread = GetCurrentThreadId();
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
        s.stamp_thread = GetCurrentThreadId();
        // Drain deferred closes even when this interval was not sampled or
        // all ring slots were occupied at the last XR frame boundary.
        poll(s);
        const bool continuous = s.backend == EyeCalibrationBackend::openxr;
        const unsigned continuous_c = continuous ? continuous_candidate(s, view) : 2;
        if (continuous && (s.current < 0 || s.ring[s.current].epoch != s.epoch ||
                s.ring[s.current].submits || s.ring[s.current].invalid)) {
            continuous_stamp_only(s, context, output, continuous_c, view, x, y, width, height);
            return;
        }
        if (s.current < 0)
            return;
        s.stats.graphics_api = 11;
        s.stats.source_graphics_api = 11;
        if (s.ring[s.current].gpu12_used) {
            s.ring[s.current].invalid = true;
            return;
        }
        auto& f = s.ring[s.current];
        if (f.pipelined && (f.submits || f.invalid)) return;
        unsigned c = f.evaluations;
        bool repeated{};
        if (f.pipelined) {
            c = continuous_c;
            repeated = c < 2 && f.views[c].id == view;
        }
        if (!repeated) ++f.evaluations;
        if (c >= 2) {
            f.invalid = true;
            return;
        }
        if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
            f.invalid = true;
            return;
        }
        if (f.pipelined && (s.frame_thread != GetCurrentThreadId() ||
            (s.submit_thread && s.submit_thread != GetCurrentThreadId()))) {
            // Only D3D's serialization also covers the game's context calls.
            // Enable on the render thread, never on SINGLETHREADED devices.
            ComPtr<ID3D11Device> device;
            context->GetDevice(&device);
            ComPtr<ID3D11Multithread> protection;
            if (!(device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) &&
                SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&protection)))) {
                protection->SetMultithreadProtected(TRUE);
                f.protected_context = protection->GetMultithreadProtected() != FALSE;
            }
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
        if (repeated && (f.views[c].generation != stereo_view_generation(view) ||
            f.views[c].width != width || f.views[c].height != height)) {
            f.invalid = true;
            return;
        }
        const auto assignment = stereo_eye_assignment(view);
        f.views[c] = {view, width, height, assignment.assigned ? int(assignment.eye_index) : -1,
                      stereo_view_generation(view)};
        if (!repeated) f.placement_plans[c] = source_placement(s, c, view, width, height);
        const auto points = calibration_marker_points(f.placement_plans[c], x, y, width, c, f.codes[c]);
        f.patch_codes[c * 2] = f.patch_codes[c * 2 + 1] = points.points[0].code;
        const auto px = points.points[0].x, py = points.points[0].y;
        if (!prepare_marker(s, context, desc.Format, c, points)) { f.invalid = true; return; }
        if (repeated) {
            // Refresh new renders; retain the first before/after proof.
            stamp_points11(s, context, texture.Get(), c, points);
            return;
        }
        context->End(f.timestamp[c * 2].Get());
        if (!copy_patch(s, f, c * 2, texture.Get(), px, py, block, block))
            f.invalid = true;
        stamp_points11(s, context, texture.Get(), c, points);
        if (!copy_patch(s, f, c * 2 + 1, texture.Get(), px, py, block, block))
            f.invalid = true;
        CalibrationImageInfo image;
        image.width = desc.Width; image.height = desc.Height; image.format = desc.Format; image.graphics_api = 11;
        image.view = view; image.prior_eye = f.views[c].assigned;
        image.view_rect = {x, y, width, height}; image.marker_rect = {px, py, block, block}; image.markers = points;
        capture_support11(f, context, texture.Get(), c, image);
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
        const bool continuous = s.backend == EyeCalibrationBackend::openxr && s.stats.submission_graphics_api == 11;
        const unsigned continuous_c = continuous ? continuous_candidate(s, view) : 2;
        if (continuous && (s.current < 0 || s.ring[s.current].epoch != s.epoch ||
                s.ring[s.current].submits || s.ring[s.current].invalid)) {
            continuous_stamp_only12(s, list, output, continuous_c, view, x, y, width, height, output_state);
            return;
        }
        if (s.current < 0)
            return;
        auto& f = s.ring[s.current];
        unsigned c = f.evaluations;
        bool repeated{};
        if (f.pipelined) {
            if (f.submits || f.invalid || f.epoch != s.epoch) return;
            c = continuous_c;
            if (f.epoch != s.epoch) { f.invalid = true; return; }
            repeated = c < 2 && f.views[c].id == view;
        }
        if (!repeated) ++f.evaluations;
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
        // Own allocations through the texture's device. The backend separately
        // verifies that the command list belongs to the same underlying device.
        if (FAILED(output->GetDevice(IID_PPV_ARGS(&device)))) {
            f.invalid = true;
            return;
        }
        device = canonical_d3d12_device(device.Get());
        if (!device) { f.invalid = true; return; }
        if (!f.gpu12 || !same_d3d12_device(device.Get(), f.device12.Get())) {
            if (f.gpu12_used) {
                f.invalid = true;
                return;
            }
            f.gpu12 = calibration12_create(device.Get());
            f.device12 = device;
            if (!f.gpu12 || !calibration12_begin(*f.gpu12)) {
                f.invalid = true;
                return;
            }
        }
        f.gpu12_used = true;
        f.thread = GetCurrentThreadId();
        s.stamp_thread = f.thread;
        ComPtr<IUnknown> identity; device.As(&identity);
        f.device_identity = reinterpret_cast<std::uintptr_t>(identity.Get());
        f.context_identity = reinterpret_cast<std::uintptr_t>(list);
        f.device_flags = 0;
        s.stats.graphics_api = 12;
        s.stats.source_graphics_api = 12;
        if (repeated && (f.views[c].generation != stereo_view_generation(view) ||
            f.views[c].width != width || f.views[c].height != height)) { f.invalid = true; return; }
        const auto assignment = stereo_eye_assignment(view);
        f.views[c] = {view, width, height, assignment.assigned ? int(assignment.eye_index) : -1,
                      stereo_view_generation(view)};
        if (!repeated) f.placement_plans[c] = source_placement(s, c, view, width, height);
        const auto points = calibration_marker_points(f.placement_plans[c], x, y, width, c, f.codes[c]);
        f.patch_codes[c * 2] = f.patch_codes[c * 2 + 1] = points.points[0].code;
        const auto px = points.points[0].x, py = points.points[0].y;
        Calibration12Failure failure;
        CalibrationImageInfo image;
        image.width = unsigned(d.Width); image.height = d.Height; image.format = d.Format; image.graphics_api = 12;
        image.view = view; image.prior_eye = f.views[c].assigned;
        image.view_rect = {x, y, width, height}; image.marker_rect = {px, py, block, block}; image.markers = points;
        if (!calibration12_stamp(*f.gpu12, list, output, c, px, py, output_state, s.stats.allocations, &failure,
            f.support, image, points.points[0].code, repeated ? Calibration12StampMode::refresh : Calibration12StampMode::source_proof, points.extra())) {
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
    s.stats.submission_graphics_api = 12;
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
    submitted_geometry(s, f, eye, unsigned(d.Width), d.Height, u0, v0, u1, v1);
    std::array<D3D12_BOX, calibration_box_count> boxes;
    std::array<std::uint32_t, calibration_box_count> codes;
    std::array<unsigned, calibration_box_count> mirrors;
    std::array<CalibrationTrackingPatch, calibration_box_count> tracking;
    const unsigned box_count = f.placement_count * 4;
    for (unsigned index = 0; index < box_count; ++index) {
        const auto c = index % 2;
        const auto& ref = f.views[c].width ? f.views[c] : f.views[f.views[0].width ? 0 : 1];
        if (!ref.width || !ref.height) { f.invalid = true; return 0; }
        const auto r = submitted_rect(f, eye, index, unsigned(d.Width), d.Height, u0, v0, u1, v1);
        codes[index] = f.patch_codes[calibration_patch_index(index, eye)];
        mirrors[index] = f.patch_mirrors[calibration_patch_index(index, eye)];
        tracking[index] = f.tracking[calibration_patch_index(index, eye)];
        boxes[index] = {r[0], r[1], 0, r[0] + r[2], r[1] + r[3], 1};
        f.patches[calibration_patch_index(index, eye)].reference_width = ref.width;
        f.patches[calibration_patch_index(index, eye)].reference_height = ref.height;
    }
    const auto expected_state = backend == EyeCalibrationBackend::openxr
                                    ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                    : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Calibration12Failure failure;
    CalibrationImageInfo image;
    image.width = unsigned(d.Width); image.height = d.Height; image.format = d.Format; image.graphics_api = 12;
    image.slice = slice; image.submitted_eye = int(eye); image.bounds = {u0, v0, u1, v1};
    image.sample_count = box_count;
    for (unsigned i = 0; i < box_count; ++i) image.sample_rects[i] =
        {boxes[i].left, boxes[i].top, boxes[i].right - boxes[i].left, boxes[i].bottom - boxes[i].top};
    if (!calibration12_capture(*f.gpu12, queue, texture, eye, slice, expected_state, {boxes.data(), box_count},
                               s.stats.allocations, &failure, f.support, image, {codes.data(), box_count}, {mirrors.data(), box_count},
                               prepare_search(f, eye), {tracking.data(), box_count})) {
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
    std::uint64_t capture_sequence{};
    std::uintptr_t source_device_identity{};
    bool pipelined{}, mixed{};
    {
        std::unique_lock lock(s.mutex, std::try_to_lock);
        if (!lock.owns_lock()) return 0;
        if (s.backend != backend || s.session_generation != session_generation || s.current < 0)
            return 0;
        auto& f = s.ring[s.current];
        if (f.invalid || f.epoch != s.epoch) return 0;
        capture_sequence = f.sequence;
        source_device_identity = f.device_identity;
        pipelined = f.pipelined;
        mixed = f.gpu12_used && f.pipelined;
        // Reject unprotected foreign-thread access before any D3D calls,
        // including GetDevice/QueryInterface on a SINGLETHREADED device.
        if (f.queries_started && f.thread != GetCurrentThreadId() && !f.protected_context) {
            s.submit_thread = GetCurrentThreadId();
            ++f.submits; ++f.eye_submits[eye];
            ++s.submit_wrong_thread;
            s.submission_context = {f.sequence, f.device_identity, f.context_identity, 0, 0,
                f.device_flags, 0, f.protected_context, false, E_PENDING, "source_context_unprotected"};
            f.invalid = true;
            return 0;
        }
        if (!f.queries_started && !mixed) {
            s.submit_thread = GetCurrentThreadId();
            if (f.pipelined) {
                ++s.waiting_for_sources;
            }
            return 0;
        }
    }
    ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    ComPtr<IUnknown> device_identity, context_identity;
    device.As(&device_identity);
    context.As(&context_identity);
    const bool separate_device = source_device_identity != reinterpret_cast<std::uintptr_t>(device_identity.Get());
    if (pipelined && separate_device && !(device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)) {
        // This is the submission device, used here before XR releases its
        // texture. It needs its own serialization and readback resources.
        ComPtr<ID3D11Multithread> protection;
        if (SUCCEEDED(context.As(&protection))) protection->SetMultithreadProtected(TRUE);
    }
    ContextLock context_lock(context.Get());
    // Host code may already hold the D3D lock when it calls the render hook.
    // Never wait for our mutex while holding that lock: a contended capture
    // is skipped, avoiding an inversion with render-thread polling/stamping.
    std::unique_lock lock(s.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return 0;
    CpuScope cpu{s};
    if (s.backend != backend || s.session_generation != session_generation)
        return 0;
    s.submit_thread = GetCurrentThreadId();
    s.stats.submission_graphics_api = 11;
    s.unsupported_submission = false;
    if (s.current < 0)
        return 0;
    auto& f = s.ring[s.current];
    if (f.sequence != capture_sequence) return 0;
    if (f.epoch != s.epoch || f.invalid) return 0;
    // Give alternating-eye rendering time to produce its second source.
    // After two intervals, one source may be captured, but publication still
    // requires its marker in BOTH successfully submitted physical eyes.
    if (f.pipelined && f.evaluations < 2 && !(f.evaluations == 1 && s.sequence - f.sequence >= 2)) {
        ++s.waiting_for_sources;
        return 0;
    }
    ++f.submits;
    ++f.eye_submits[eye];
    const bool other_thread = f.thread != GetCurrentThreadId();
    s.submission_context = {f.sequence, f.device_identity, f.context_identity,
        reinterpret_cast<std::uintptr_t>(device_identity.Get()),
        reinterpret_cast<std::uintptr_t>(context_identity.Get()), f.device_flags, device->GetCreationFlags(),
        f.protected_context, context_lock.protection != nullptr, context_lock.query_result};
    auto& observed = s.submission_context;
    const bool independent_capture = f.pipelined && separate_device && context_lock.protection &&
        !(observed.submitted_flags & D3D11_CREATE_DEVICE_SINGLETHREADED);
    if (f.context.Get() != context.Get() && !independent_capture) observed.rejection = "different_submission_context";
    else if (other_thread && (observed.submitted_flags & D3D11_CREATE_DEVICE_SINGLETHREADED))
        observed.rejection = "singlethreaded_submission_device";
    else if (other_thread && !context_lock.protection) observed.rejection = "submission_context_unprotected";
    const bool protected_copy = f.pipelined && context_lock.protection &&
        !(device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED);
    if (f.submits > 2 || f.eye_submits[eye] > 1 || (!f.queries_started && !mixed) || (other_thread && !protected_copy)) {
        if (f.queries_started && other_thread && !protected_copy) {
            ++s.submit_wrong_thread;
        }
        f.invalid = true;
        return 0;
    }
    if (f.context.Get() != context.Get() && !independent_capture) {
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
    auto& submitted = f.submitted11[eye];
    if (independent_capture) {
        if (submitted.context.Get() != context.Get()) {
            submitted.done.Reset();
            submitted.context = context;
        }
        if (!submitted.done) {
            const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
            if (FAILED(device->CreateQuery(&query, &submitted.done))) {
                observed.rejection = "submission_query_creation_failed";
                f.invalid = true;
                return 0;
            }
            ++s.stats.allocations;
        }
        submitted.thread = GetCurrentThreadId();
    } else context->End(f.timestamp[4 + eye * 2].Get());
    CalibrationImageInfo image;
    image.width = desc.Width; image.height = desc.Height; image.format = desc.Format; image.graphics_api = 11;
    image.slice = slice; image.submitted_eye = int(eye); image.bounds = {u0, v0, u1, v1};
    submitted_geometry(s, f, eye, desc.Width, desc.Height, u0, v0, u1, v1);
    image.sample_count = f.placement_count * 4;
    for (unsigned index = 0; index < image.sample_count; ++index) {
        const unsigned c = index % 2;
        const auto& ref = f.views[c].width ? f.views[c] : f.views[f.views[0].width ? 0 : 1];
        if (!ref.width || !ref.height) {
            f.invalid = true;
            continue;
        }
        const auto r = submitted_rect(f, eye, index, desc.Width, desc.Height, u0, v0, u1, v1);
        image.sample_rects[index] = r;
        if (!copy_patch(s, f, calibration_patch_index(index, eye), texture, r[0], r[1], r[2], r[3],
            ref.width, ref.height, slice, context.Get()))
            f.invalid = true;
    }
    capture_support11(f, context.Get(), texture, 2 + eye, image);
    capture_search11(f, eye, context.Get(), texture, image);
    if (independent_capture) {
        context->End(submitted.done.Get());
        submitted.active = true;
        ++s.cross_device_submits;
    } else {
        context->End(f.timestamp[5 + eye * 2].Get());
        f.segments[2 + eye] = true;
    }
    if (other_thread) {
        ++s.protected_submits;
    }
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
            calibration_image_physical_eye(f.support, 2 + unsigned(ticket & 1), f.physical_eyes[ticket & 1]);
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
    for (auto& f : s.ring) for (auto& search : f.search) if (search) search->canceled = true;
    s.ring = {};
    s.markers = {};
    s.device.Reset();
    s.current = -1;
    s.last_frame_ms = s.last_openvr_ms = s.session_generation = 0;
    s.backend = EyeCalibrationBackend::none;
    s.unsupported_submission = false;
    s.finish_wrong_thread = s.submit_wrong_thread = s.poll_wrong_thread = 0;
    s.protected_submits = s.carried_frames = s.waiting_for_sources = 0;
    s.cross_device_submits = 0;
    s.submission_context = {};
    s.frame_thread = s.stamp_thread = s.submit_thread = s.tick_thread = 0;
    s.continuous_epoch = 0;
    s.continuous_views = {};
    s.continuous12 = {};
    s.continuous12_device.Reset();
    s.continuous12_next = 0;
    placement_epoch(s);
}

const char* eye_calibration_status(const EyeCalibrationStats& stats) noexcept {
    if (!stats.enabled)
        return "Disabled";
    if (!stats.runtime_active)
        return "Waiting for OpenVR or OpenXR";
    if (stats.unsupported_submission)
        return "Unsupported texture or queue path";
    if (stats.correction_active)
        return stats.left_view == stats.right_view ? "Active (shared mono source)" : "Active";
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
        << ",\"source_graphics_api\":" << s.source_graphics_api << ",\"submission_graphics_api\":" << s.submission_graphics_api
        << ",\"enabled\":" << s.enabled << ",\"status\":\"" << eye_calibration_status(s)
        << "\",\"active\":" << s.correction_active << ",\"openvr_active\":" << s.openvr_active
        << ",\"shared_source\":" << (s.correction_active && s.left_view == s.right_view)
        << ",\"vertical_flip\":" << s.vertical_flip
        << ",\"unsupported_submission\":" << s.unsupported_submission
        << ",\"unsupported_submissions\":" << s.unsupported_submissions << ",\"frames\":" << s.frames
        << ",\"captures\":" << s.captures << ",\"completed\":" << s.completed << ",\"valid\":" << s.valid
        << ",\"skipped\":" << s.skipped << ",\"in_flight\":" << s.in_flight
        << ",\"corrections\":" << s.corrections << ",\"applied\":" << s.applied
        << ",\"mismatches\":" << s.mismatches << ",\"allocations\":" << s.allocations
        << ",\"gpu_samples\":" << s.gpu_samples << ",\"cpu_us_per_frame\":" << s.cpu_us_per_frame
        << ",\"gpu_timing_status\":\"" << s.gpu_timing_status << '"'
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
        << ",\"continuous_stamps\":" << s.d3d12_continuous_stamps
        << ",\"continuous_skipped\":" << s.d3d12_continuous_skipped
        << ",\"capture_failures\":" << s.d3d12_capture_failures
        << ",\"readback_failures\":" << s.d3d12_readback_failures
        << ",\"last_stamp_failure\":\"" << s.d3d12_last_stamp_failure
        << "\",\"stamp_hresult\":" << s.d3d12_stamp_error
        << ",\"last_capture_failure\":\"" << s.d3d12_last_capture_failure
        << "\",\"capture_hresult\":" << s.d3d12_capture_error
        << ",\"last_readback_failure\":\"" << s.d3d12_last_readback_failure
        << "\",\"readback_hresult\":" << s.d3d12_readback_error << '}';
    {
        auto& live = state();
        std::lock_guard lock(live.mutex);
        out << ",\"placement_search\":{\"hypotheses\":" << calibration_placement_count
            << ",\"locks\":" << live.placement_locks << ",\"losses\":" << live.placement_losses
            << ",\"search_candidate\":" << live.search_candidate
            << ",\"tracking_misses\":" << live.tracking_misses
            << ",\"tracking_mode\":\"local_recenter\",\"tracking_padding_px\":64,\"tracking_max_patch_px\":256"
            << ",\"max_acquisition_markers_per_source\":2"
            << ",\"stamping\":\"corner_pair_then_selected\""
            << ",\"wide_searches\":" << live.wide_searches
            << ",\"wide_search_pending\":" << std::any_of(live.ring.begin(), live.ring.end(), [](const auto& f) { return f.busy && f.wide_search; })
            << ",\"last_wide_results\":[";
        for (unsigned eye = 0; eye < 2; ++eye) {
            if (eye) out << ',';
            const auto& r = live.last_search_results[eye];
            out << "{\"valid\":" << r.valid << ",\"ambiguous\":" << r.ambiguous << ",\"candidate\":" << r.candidate
                << ",\"flipped\":" << r.flipped << ",\"score\":" << r.score
                << ",\"marker_xy\":[" << r.placement.marker.x << ',' << r.placement.marker.y << "]}";
        }
        out << "],\"sources\":[";
        for (unsigned c = 0; c < 2; ++c) {
            if (c) out << ',';
            const auto& p = live.placements[c];
            out << "{\"locked\":" << (p.locked && live.placement_epoch == live.epoch)
                << ",\"visible_source_xywh\":[" << p.placement.x << ',' << p.placement.y << ','
                << p.placement.width << ',' << p.placement.height << "],\"marker_xy\":["
                << p.placement.marker.x << ',' << p.placement.marker.y << "]"
                << ",\"per_eye\":" << p.per_eye << ",\"eye_placements\":[";
            for (unsigned eye = 0; eye < 2; ++eye) {
                if (eye) out << ',';
                const auto& e = p.per_eye ? p.eye_placements[eye] : p.placement;
                out << "{\"visible_source_xywh\":[" << e.x << ',' << e.y << ',' << e.width << ',' << e.height
                    << "],\"marker_xy\":[" << e.marker.x << ',' << e.marker.y << "]}";
            }
            out << "]}";
        }
        out << "]}";
        out << ",\"d3d11_lifecycle\":{\"frame_thread\":" << live.frame_thread
            << ",\"stamp_thread\":" << live.stamp_thread << ",\"submit_thread\":" << live.submit_thread
            << ",\"tick_thread\":" << live.tick_thread << ",\"finish_wrong_thread\":" << live.finish_wrong_thread
            << ",\"submit_wrong_thread\":" << live.submit_wrong_thread << ",\"poll_wrong_thread\":" << live.poll_wrong_thread
            << ",\"protected_submits\":" << live.protected_submits << ",\"carried_frames\":" << live.carried_frames
            << ",\"waiting_for_sources\":" << live.waiting_for_sources
            << ",\"cross_device_submits\":" << live.cross_device_submits
            << ",\"last_submission_context\":{\"sequence\":" << live.submission_context.sequence
            << ",\"source_device\":\"" << live.submission_context.source_device
            << "\",\"source_context\":\"" << live.submission_context.source_context
            << "\",\"submitted_device\":\"" << live.submission_context.submitted_device
            << "\",\"submitted_context\":\"" << live.submission_context.submitted_context
            << "\",\"source_device_flags\":" << live.submission_context.source_flags
            << ",\"submitted_device_flags\":" << live.submission_context.submitted_flags
            << ",\"source_protected_when_stamped\":" << live.submission_context.source_protected
            << ",\"submitted_protected\":" << live.submission_context.submitted_protected
            << ",\"protection_query_hresult\":" << live.submission_context.protection_query
            << ",\"rejection\":\"" << live.submission_context.rejection << "\"}"
            << ",\"slots\":[";
        bool slot_separator = false;
        for (const auto& f : live.ring) {
            if (!f.busy) continue;
            if (slot_separator) out << ',';
            slot_separator = true;
            out << "{\"sequence\":" << f.sequence << ",\"age_ms\":" << GetTickCount64() - f.captured_ms
                << ",\"thread\":" << f.thread << ",\"context\":\"" << reinterpret_cast<std::uintptr_t>(f.context.Get())
                << "\",\"closed\":" << f.closed << ",\"queries_started\":" << f.queries_started
                << ",\"close_requested\":" << f.close_requested
                << ",\"pipelined\":" << f.pipelined
                << ",\"protected_context\":" << f.protected_context
                << ",\"invalid\":" << f.invalid << ",\"gpu12\":" << f.gpu12_used
                << ",\"evaluations\":" << f.evaluations << ",\"submits\":" << f.submits << '}';
        }
        out << "]}";
    }
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
