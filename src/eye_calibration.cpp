#include "eye_calibration.hpp"
#include "eye_calibration_pixels.hpp"
#include "eye_calibration_d3d12.hpp"
#include "d3d12_native.hpp"
#include "settings.hpp"
#include "runtime.hpp"
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
    unsigned marker_failure{};
    bool shared_source_assumed{};
    double marker_error_x{}, marker_error_y{};
    bool motion_unreliable{};
    CalibrationImageRequestPtr support;
    std::array<SupportReadback11, 4> support11;
    bool wide_search{};
    EyeCalibrationMethod method{EyeCalibrationMethod::standard};
    std::array<std::array<double, 7>, 2> submitted_signature{};
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
    std::array<CalibrationTrackingInputPtr, calibration_patch_count> tracking_inputs{};
    std::shared_ptr<CalibrationVerificationJob> verification;
    bool verification_staged{};
    std::array<std::array<double, 2>, 2> submitted_sizes{};
    std::array<std::array<bool, calibration_placement_count>, 2> usable_placements{};
    unsigned placement_count{1};
    std::array<Submitted11, 2> submitted11;
    std::array<View, 2> views;
    std::array<std::uint32_t, 2> codes{};
    bool pipelined{}, collect_source_pair{}, protected_context{};
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
struct TrackingHint {
    CalibrationTrackingPatch reference;
    CalibrationPlacement observed;
    std::uint64_t epoch{}, sequence{}, captured_ms{}, view{}, generation{};
};
struct State {
    std::mutex mutex;
    std::array<PlacementLock, 2> placements;
    std::array<TrackingHint,calibration_patch_count> tracking_hints;
    std::array<std::array<double, 2>, 2> submitted_sizes{};
    std::uint64_t placement_epoch{}, placement_sequence{}, placement_locks{}, placement_losses{};
    bool search_needed{};
    EyeCalibrationPolicy policy;
    std::uint64_t learned_this_launch_signature{};
    unsigned learned_this_launch_method{};
    bool published{};
    // Metadata remains observable without stamping markers or copying pixels.
    std::array<std::array<double, 7>, 2> observed_submissions{};
    unsigned search_candidate{}, tracking_misses{};
    std::uint64_t wide_searches{}, last_wide_search_ms{};
    std::uint64_t last_verified_ms{}, motion_inconclusive{}, geometry_rejections{};
    std::array<CalibrationSearchResult, 2> last_search_results;
    std::array<CalibrationSearchPtr, 2> last_search_diagnostics;
    std::uint64_t next_diagnostic_ms{}, slow_calls{}, sampled_black_images{};
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
struct CalibrationMotion {
    std::mutex mutex;
    std::uint64_t session{}, space{}, observed_ms{}, unstable_until{};
    std::int64_t time{};
    std::array<float, 4> orientation{};
    bool valid{};
    std::uint64_t observations{};
    double degrees_per_second{}, peak_degrees_per_second{};
    std::uint64_t fast_samples{};
};
CalibrationMotion motion;
bool motion_unreliable(std::uint64_t session) {
    if (!session) return false; // OpenVR keeps the ordinary verification policy.
    std::lock_guard lock(motion.mutex);
    const auto now = GetTickCount64();
    return motion.session == session && now >= motion.observed_ms && now - motion.observed_ms < 250 &&
        (!motion.valid || now < motion.unstable_until);
}
void request_full_calibration(State& s, const char* reason) {
    if (!s.search_needed) {
        if (s.placements[0].locked || s.placements[1].locked) ++s.stats.recalibration_requests;
        s.stats.full_calibration_reason=reason;
    }
    s.search_needed=true;
    s.policy.recover(GetTickCount64());
    s.published=false;
}
void placement_epoch(State& s) {
    if (s.placement_epoch == s.epoch) return;
    s.placement_epoch = s.epoch;
    request_full_calibration(s,"session_or_calibration_reset");
    s.placements = {}; s.tracking_hints={}; s.submitted_sizes = {}; s.placement_sequence = 0;
    s.search_needed = true; s.last_wide_search_ms = 0; s.last_verified_ms = 0;
    s.search_candidate = s.tracking_misses = 0;
    s.observed_submissions = {};
    s.policy.begin(eye_calibration_selected_method(), GetTickCount64());
}
bool retaining_calibration(const State& s) {
    // Publication already passed identity, geometry, age and generation checks.
    // Change-only mode must not keep verifying a usable mapping while waiting
    // for consecutive marker hits: misses otherwise restart full acquisition.
    return !eye_calibration_continuous_validation() && s.published && !s.search_needed;
}
void restart_calibration(State& s, const char* reason) {
    request_full_calibration(s, reason);
    ++s.epoch;
    s.frames_until_capture = 0;
    clear_stereo_calibration();
    placement_epoch(s);
    if (std::string_view(reason) == "source_or_submitted_geometry_changed") s.policy.recover(GetTickCount64());
}
void observe_source(State& s, std::uint64_t view, unsigned width, unsigned height) {
    if (!retaining_calibration(s)) return;
    const auto generation = stereo_view_generation(view);
    for (const auto& p : s.placements)
        if (p.locked && p.view.id == view && p.view.generation == generation &&
            p.view.width == width && p.view.height == height) return;
    restart_calibration(s, "source_or_submitted_geometry_changed");
}
void observe_submission(State& s, unsigned eye, unsigned width, unsigned height,
                        float u0, float v0, float u1, float v1, unsigned slice) {
    for (const float v : {u0, v0, u1, v1})
        if (!std::isfinite(v) || v < 0 || v > 1) return;
    if (!width || !height || u0 == u1 || v0 == v1) return;
    placement_epoch(s);
    const std::array<double, 7> geometry{double(width), double(height), u0, v0, u1, v1, double(slice)};
    if (retaining_calibration(s) && s.observed_submissions[eye][0] &&
        s.observed_submissions[eye] != geometry)
        restart_calibration(s, "source_or_submitted_geometry_changed");
    s.observed_submissions[eye] = geometry;
}
CalibrationPlacementPlan source_placement(State& s, unsigned c, std::uint64_t view, unsigned width, unsigned height) {
    placement_epoch(s);
    auto& lock = s.placements[c];
    if (!s.search_needed && lock.locked && lock.view.id == view && lock.view.generation == stereo_view_generation(view) &&
        lock.view.width == width && lock.view.height == height && lock.submitted_sizes == s.submitted_sizes) {
        CalibrationPlacementPlan plan; plan.placements[0] = lock.placement;
        plan.per_eye = lock.per_eye; plan.eye_placements = lock.eye_placements; return plan;
    }
    if (lock.locked) { ++s.placement_losses; request_full_calibration(s,"source_or_submitted_geometry_changed"); invalidate_stereo_crop(); }
    lock.locked = false;
    if (s.policy.active == EyeCalibrationMethod::full) return calibration_grid_plan(width, height);
    CalibrationPlacementPlan plan;
    plan.placements[0] = {0, 0, double(width), double(height), {c ? width - 52 : 12, 12}};
    // Use the same geometry validation as locked crops; no full-image readbacks.
    plan.per_eye = true;
    plan.eye_placements.fill(plan.placements[0]);
    return plan;
}
void submitted_geometry(State& s, Frame& f, unsigned eye, unsigned width, unsigned height,
                        float u0, float v0, float u1, float v1, unsigned slice) {
    placement_epoch(s);
    f.submitted_sizes[eye] = {double(width) * std::abs(double(u1) - u0), double(height) * std::abs(double(v1) - v0)};
    f.motion_unreliable |= motion_unreliable(f.session_generation);
    f.submitted_signature[eye] = {double(width), double(height), u0, v0, u1, v1, double(slice)};
    if (f.epoch == s.epoch) s.submitted_sizes[eye] = f.submitted_sizes[eye];
    f.placement_count = (std::max)(f.placement_plans[0].count, f.placement_plans[1].count);
}
std::array<unsigned, 4> submitted_rect(State& s, Frame& f, unsigned eye, unsigned index, unsigned width, unsigned height,
                                     float u0, float v0, float u1, float v1) {
    const unsigned c = index % 2, h = index / 4;
    const auto source = f.views[c].width ? c : (f.views[0].width ? 0U : 1U);
    // Missing source in a single-source pipeline still probes the absent code,
    // using the same geometry but its own left/right marker position.
    auto plan = f.placement_plans[source];
    if (source != c) {
        for (unsigned i = 0; i < plan.count; ++i)
            plan.placements[i].marker.x = f.views[source].width - block - plan.placements[i].marker.x;
        for (auto& p : plan.eye_placements) p.marker.x = f.views[source].width - block - p.marker.x;
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
        if (tracking.enabled) {
            const auto& hint=s.tracking_hints[calibration_patch_index(index,eye)];
            const auto& old=hint.reference;
            const auto& a=old.placement; const auto& b=tracking.placement;
            // Hints guide recognition only; the original crop still validates gaze geometry.
            if (hint.sequence && hint.epoch==f.epoch && hint.view==f.views[c].id &&
                hint.generation==f.views[c].generation && GetTickCount64()-hint.captured_ms<1000 &&
                old.rect==tracking.rect && old.flip==tracking.flip && old.reverse_x==tracking.reverse_x &&
                old.reverse_y==tracking.reverse_y && old.eye_width==tracking.eye_width && old.eye_height==tracking.eye_height &&
                a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height &&
                a.marker.x==b.marker.x && a.marker.y==b.marker.y) {
                tracking.hint_valid=true; tracking.hint=hint.observed;
            }
            rect=tracking.rect;
        }
    }
    if (!rect[2] || !rect[3]) { f.usable_placements[eye][h] = false; rect = {0, 0, 1, 1}; }
    return rect;
}
std::uint64_t calibration_signature(const State& s, const Frame& f) {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto add = [&](std::uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
    static const auto executable = [] {
        std::array<wchar_t, 32768> path{};
        const auto n = GetModuleFileNameW(nullptr, path.data(), unsigned(path.size()));
        std::uint64_t h = 14695981039346656037ULL;
        for (unsigned i=0; i<n; ++i) h = (h ^ std::uint64_t(path[i])) * 1099511628211ULL;
        return h;
    }();
    add(executable); add(1); // Signature schema version.
    add(unsigned(s.backend)); add(s.stats.source_graphics_api); add(s.stats.submission_graphics_api);
    for (const auto& v : f.views) { add(v.width); add(v.height); }
    for (const auto& eye : f.submitted_signature) for (double value : eye) add(std::bit_cast<std::uint64_t>(value));
    return hash;
}
CalibrationSearchPtr prepare_search(Frame& f, unsigned eye) noexcept try {
    if (!f.wide_search) return {};
    auto request = std::make_shared<CalibrationSearch>();
    request->require_grid = true;
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
    request->capture_started_ms = calibration_clock_ms();
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
    request->copy_issued_ms = GetTickCount64();
    request->texture_identity = reinterpret_cast<std::uintptr_t>(texture);
    context->CopySubresourceRegion(capture.staging.Get(), 0, 0, 0, 0, texture, subresource, nullptr);
    request->copy_recorded_ms=calibration_clock_ms();
    request->setup_ms=request->copy_recorded_ms-request->capture_started_ms;
} catch (...) { if (f.search[eye]) f.search[eye]->ready = true; }
bool poll_search11(Frame& f, unsigned eye, ID3D11DeviceContext* context) {
    auto& capture = f.search11[eye];
    if (!capture.staging) return true;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto map_start=calibration_clock_ms();
    const auto hr = context->Map(capture.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    f.search[eye]->map_cpu_ms+=calibration_clock_ms()-map_start;
    ++f.search[eye]->map_polls; f.search[eye]->map_result = hr;
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
    f.search[eye]->readback_ready_ms = GetTickCount64();
    if (SUCCEEDED(hr)) f.search[eye]->readback_wall_ms=calibration_clock_ms()-f.search[eye]->copy_recorded_ms;
    CalibrationSearchImage image;
    if (SUCCEEDED(hr)) {
        const auto& info = f.search_info[eye];
        try { image = calibration_search_image(mapped.pData, mapped.RowPitch, info.width, info.height, info.format, info.bounds, true); }
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
constexpr const char* marker_failure_names[]{"none","no_consistent_eye_pair","ambiguous_eye_identity",
    "wide_search_no_consistent_pair","tracking_result_missing","marker_shift_over_32px"};
void record_rejection(State& s, const Frame& f, unsigned mask) {
    ++s.stats.rejected;
    if (f.marker_failure && f.marker_failure<=5) ++s.stats.marker_failure_counts[f.marker_failure-1];
    for (unsigned i = 0; i < s.stats.rejection_counts.size(); ++i)
        if (mask & (1U << i)) ++s.stats.rejection_counts[i];
    // GPU completions need not arrive in sequence order.
    if (f.sequence < s.stats.last_rejected_sequence) return;
    s.stats.last_rejected_sequence = f.sequence;
    s.stats.last_rejection_mask = mask;
    s.stats.last_evaluations = f.evaluations;
    s.stats.last_submits = f.submits;
    s.stats.last_marker_failure=marker_failure_names[f.marker_failure];
    s.stats.last_source_mask=(f.views[0].id ? 1U : 0U) | (f.views[1].id ? 2U : 0U);
    s.stats.last_shared_source_assumed=f.shared_source_assumed;
    s.stats.last_motion_unreliable=f.motion_unreliable;
    s.stats.last_marker_error_x=f.marker_error_x; s.stats.last_marker_error_y=f.marker_error_y;
    for (unsigned i = 0; i < s.stats.last_rejected_scores.size(); ++i) {
        s.stats.last_rejected_scores[i] = f.patches[i].score;
        const auto& d=f.tracked[i].match_diagnostics;
        s.stats.last_best_scores[i]=d.best_score; s.stats.last_best_contrasts[i]=d.best_score_contrast;
        s.stats.last_max_contrasts[i]=d.max_contrast; s.stats.last_best_bits[i]=d.best_coarse_bits;
        s.stats.last_score_bits[i]=d.best_score_bits; s.stats.last_search_positions[i]=d.positions;
        s.stats.last_score_probes[i]=d.scored_candidates; s.stats.last_low_contrast_positions[i]=d.low_contrast_positions;
    }
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
        if (us > 20000) ++s.slow_calls;
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
                f.tracking_inputs[patch_index]=calibration_tracking_copy(mapped.pData,mapped.RowPitch,p.format,f.tracking[patch_index]);
                if (!f.tracking_inputs[patch_index]) f.invalid=true;
            } else p.score = calibration_pattern_score(mapped.pData, mapped.RowPitch, p.width, p.height,
                p.format, c, true, f.patch_codes[calibration_patch_index(index, eye)],
                f.patch_mirrors[calibration_patch_index(index, eye)]);
            capture.context->Unmap(p.staging.Get(), 0);
            p.ready = true;
        }
        capture.ready = !waiting;
    }
}
void log_calibration_capture(State& s, const Frame& f, unsigned rejection) {
    if (GetTickCount64() >= s.next_diagnostic_ms) {
            s.next_diagnostic_ms = GetTickCount64()+5000;
            trace_event("CALIB summary seq=%llu session=%llu source_api=%u submit_api=%u evals=%u submits=%u reject=0x%X captured_age_ms=%llu completed=%llu accepted=%llu black_sample_images=%llu slow_calls_over20ms=%llu max_call_ms=%.2f",
                f.sequence, f.session_generation, s.stats.source_graphics_api, s.stats.submission_graphics_api,
                f.evaluations, f.submits, rejection, GetTickCount64()-f.captured_ms, s.stats.completed+1,
                s.stats.applied, s.sampled_black_images, s.slow_calls, s.stats.max_cpu_call_us/1000);
            trace_event("CALIB verification background=1 completed_patch_calls=%llu worker_us_per_frame=%.2f last_sample_ms=%.2f peak_sample_ms=%.2f exact=%llu nearby=%llu broad=%llu",
                s.stats.verification_patch_calls,s.stats.frames ? s.stats.verification_cpu_ms*1000/s.stats.frames : 0,
                s.stats.verification_last_ms,s.stats.verification_peak_ms,s.stats.verification_paths[0],s.stats.verification_paths[1],s.stats.verification_paths[2]);
            trace_event("CALIB acquisition_counts attempts=%llu successes=%llu recalibration_requests=%llu failure_streak=%u/%u last_reason=%s",
                s.wide_searches,s.stats.full_calibration_successes,s.stats.recalibration_requests,
                s.tracking_misses,eye_calibration_failure_limit,s.stats.full_calibration_reason);
            if (s.stats.last_rejected_sequence) {
                const auto& d=s.stats;
                trace_event("CALIB last_failure seq=%llu mask=0x%X detail=%s evals=%u source_mask=0x%X shared_assumed=%u motion_unreliable=%u error_xy=%.2f,%.2f scores=%.3f,%.3f,%.3f,%.3f;%.3f,%.3f,%.3f,%.3f;%.3f,%.3f,%.3f,%.3f",
                    d.last_rejected_sequence,d.last_rejection_mask,d.last_marker_failure,d.last_evaluations,d.last_source_mask,
                    unsigned(d.last_shared_source_assumed),unsigned(d.last_motion_unreliable),d.last_marker_error_x,d.last_marker_error_y,
                    d.last_rejected_scores[0],d.last_rejected_scores[1],d.last_rejected_scores[2],d.last_rejected_scores[3],
                    d.last_rejected_scores[4],d.last_rejected_scores[5],d.last_rejected_scores[6],d.last_rejected_scores[7],
                    d.last_rejected_scores[8],d.last_rejected_scores[9],d.last_rejected_scores[10],d.last_rejected_scores[11]);
                for(unsigned i=4;i<12;++i) if(d.last_search_positions[i])
                    trace_event("CALIB last_failure_patch seq=%llu patch=%u accepted_score=%.3f best_sampled_score=%.3f bits_at_best=%u/25 contrast_at_best=%.4f best_coarse_bits=%u/25 max_contrast=%.4f positions=%u scored=%u low_contrast=%u",
                        d.last_rejected_sequence,i,d.last_rejected_scores[i],d.last_best_scores[i],d.last_score_bits[i],
                        d.last_best_contrasts[i],d.last_best_bits[i],d.last_max_contrasts[i],d.last_search_positions[i],
                        d.last_score_probes[i],d.last_low_contrast_positions[i]);
            }
            if (s.stats.capture_timing_samples) {
                const auto& p=s.stats;
                trace_event("CALIB acquisition_peak seq=%llu eye=%u size=%ux%u total_ms=%.2f setup_ms=%.2f readback_wall_ms=%.2f map_cpu_ms=%.2f raw_copy_ms=%.2f other_ms=%.2f map_polls=%u",
                    p.peak_capture_sequence,p.peak_capture_eye,p.peak_capture_width,p.peak_capture_height,
                    p.max_capture_ms,p.peak_capture_setup_ms,p.peak_capture_wait_ms,p.peak_capture_map_ms,
                    p.peak_capture_copy_ms,p.peak_capture_other_ms,p.peak_capture_map_polls);
            }
            for(unsigned eye=0;eye<2;++eye) if(const auto& q=f.search[eye]; q && q->ready.load(std::memory_order_acquire)) {
                trace_event("CALIB search seq=%llu eye=%u texture=%p size=%ux%u map_hr=0x%08X map_polls=%u copy_to_map_ms=%llu capture_total_ms=%.2f setup_ms=%.2f readback_wall_ms=%.2f map_cpu_ms=%.2f raw_copy_ms=%.2f worker_ms=%.2f locator_initial=%u locator_last=%u passes=%u budget_exhausted=%u samples=%u black=%u luma_min=%.4f max=%.4f mean=%.4f matched=%u points=%u",
                    f.sequence,eye,reinterpret_cast<void*>(q->texture_identity),q->image_width,q->image_height,
                    unsigned(q->map_result),q->map_polls,
                    q->copy_issued_ms && q->readback_ready_ms >= q->copy_issued_ms ? q->readback_ready_ms-q->copy_issued_ms : 0,
                    q->capture_total_ms,q->setup_ms,q->readback_wall_ms,q->map_cpu_ms,q->conversion_ms,q->elapsed_ms,q->initial_locator_factor,q->last_locator_factor,q->locator_passes,unsigned(q->search_budget_exhausted),q->sample_count,q->sampled_black,q->sampled_min,q->sampled_max,q->sampled_mean,
                    unsigned(q->result.valid),q->result.support_points);
            }
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
        // A staged job owns only CPU bytes; GPU retirement was already confirmed.
        // Discard results across reset/toggle/session changes before classification.
        if (f.verification_staged && (f.sequence<s.measurement_start || f.epoch!=s.epoch || !enabled)) {
            if (f.verification) f.verification->canceled=true;
            f.classified=true; f.busy=false;
            continue;
        }
        // A mixed frame owns two independent GPU timelines. Do not classify or
        // recycle either half until submission-thread DX11 queries also finish.
        if (std::any_of(f.submitted11.begin(), f.submitted11.end(),
            [](const auto& capture) { return capture.active && !capture.ready; })) continue;
        if (f.search[0] && f.search[1] && f.search[0]->started && f.search[1]->started &&
            (!f.search[0]->ready.load(std::memory_order_acquire) || !f.search[1]->ready.load(std::memory_order_acquire))) continue;
        bool gpu12_reusable{};
        const unsigned source_mask = (f.views[0].id ? 1U : 0U) | (f.views[1].id ? 2U : 0U);
        const bool mono = f.pipelined && f.evaluations == 1 && (source_mask == 1 || source_mask == 2);
        f.shared_source_assumed=mono;
        if (!f.verification_staged) {
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
                    f.tracking_inputs[i]=result.tracking_inputs[i];
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
                        log_calibration_capture(s, f, 1U | (f.evaluations != 2 ? 2U : 0U));
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
                        log_calibration_capture(s, f, 1U);
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
                        f.tracking_inputs[i]=calibration_tracking_copy(mapped.pData,mapped.RowPitch,p.format,f.tracking[i]);
                        if (!f.tracking_inputs[i]) f.invalid=true;
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
            f.verification_staged=true;
            if (std::any_of(f.tracking_inputs.begin(),f.tracking_inputs.end(),[](const auto& input){return bool(input);})) {
                try {
                    f.verification=std::make_shared<CalibrationVerificationJob>();
                    f.verification->inputs=std::move(f.tracking_inputs);
                } catch (...) { f.invalid=true; }
            }
        } else gpu12_reusable=f.gpu12_used; // GPU readbacks were already completed before staging.
        if (f.verification) {
            if (f.epoch!=s.epoch || !enabled) f.verification->canceled=true;
            if (!calibration_verification_start(f.verification) || !f.verification->ready.load(std::memory_order_acquire)) continue;
            for (unsigned i=0;i<f.tracked.size();++i) if (f.verification->inputs[i]) {
                f.tracked[i]=f.verification->results[i];
                f.patches[i].score=f.tracked[i].valid ? f.tracked[i].score : 0;
            }
        }
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
        const bool grid = ((source_mask & 1U) && !f.placement_plans[0].per_eye) ||
            ((source_mask & 2U) && !f.placement_plans[1].per_eye);
        if (grid) { selected = -1; ambiguous = false; }
        bool acquired{};
        if (f.wide_search) for (unsigned eye = 0; eye < 2; ++eye) {
            s.last_search_results[eye] = f.search[eye] ? f.search[eye]->result : CalibrationSearchResult{};
            const auto& search = f.search[eye];
            s.last_search_diagnostics[eye] = search;
            s.stats.capture_total_ms[eye]=search ? search->capture_total_ms : -1;
            s.stats.capture_setup_ms[eye]=search ? search->setup_ms : -1;
            s.stats.capture_wait_ms[eye]=search ? search->readback_wall_ms : -1;
            s.stats.capture_map_ms[eye]=search ? search->map_cpu_ms : -1;
            s.stats.capture_copy_ms[eye]=search && search->capture_total_ms>=0 ? search->conversion_ms : -1;
            if (search && search->capture_total_ms>=0) {
                ++s.stats.capture_timing_samples;
                if (search->capture_total_ms>s.stats.max_capture_ms) {
                    s.stats.max_capture_ms=search->capture_total_ms;
                    s.stats.peak_capture_setup_ms=search->setup_ms;
                    s.stats.peak_capture_wait_ms=search->readback_wall_ms;
                    s.stats.peak_capture_map_ms=search->map_cpu_ms;
                    s.stats.peak_capture_copy_ms=search->conversion_ms;
                    // Map time overlaps readback elapsed and must not be added twice.
                    s.stats.peak_capture_other_ms=(std::max)(0.,search->capture_total_ms-search->setup_ms-search->readback_wall_ms-search->conversion_ms);
                    s.stats.peak_capture_sequence=f.sequence;
                    s.stats.peak_capture_eye=eye;
                    s.stats.peak_capture_width=search->image_width;
                    s.stats.peak_capture_height=search->image_height;
                    s.stats.peak_capture_map_polls=search->map_polls;
                }
            }
            if (search && search->sample_count && search->sampled_black == search->sample_count) ++s.sampled_black_images;
            s.stats.search_ms[eye] = search && search->timed ? search->elapsed_ms : -1.;
            if (search && search->timed) {
                ++s.stats.search_timing_samples;
                s.stats.max_search_ms = (std::max)(s.stats.max_search_ms, search->elapsed_ms);
            }
        }
        if (selected < 0 && !ambiguous && f.search[0] && f.search[1]) {
            const auto& a = f.search[left_slot]->result;
            const auto& b = f.search[right_slot]->result;
            if (a.valid && b.valid && !a.ambiguous && !b.ambiguous && a.flipped == b.flipped &&
                (mono ? a.candidate == b.candidate && (source_mask & (1U << a.candidate)) : a.candidate != b.candidate)) {
                left = int(a.candidate); right = int(b.candidate); flipped_pair = a.flipped;
                acquired = true;
            }
        }
        // A grid cannot publish a crop from a single coincidental local match.
        if (grid && !acquired) selected = -1;
        if ((selected < 0 && !acquired) || ambiguous) {
            rejection |= 128U;
            f.marker_failure=ambiguous ? 2 : f.wide_search ? 3 : 1;
        }
        bool geometry_changed{};
        if (!rejection && !acquired) {
            for (unsigned eye = 0; eye < 2; ++eye) {
                const unsigned slot = eye ? right_slot : left_slot;
                const unsigned c = unsigned(eye ? right : left);
                const unsigned h = unsigned(eye ? selected_right : selected);
                const unsigned patch = 4 + h * 8 + unsigned(flipped_pair) * 4 + slot * 2 + c;
                const auto& actual = f.tracked[patch];
                const auto& expected = f.placement_plans[c].for_eye(slot, h);
                // A raw template score alone supplies identity, not geometry.
                if (!actual.valid) { rejection |= 128U; f.marker_failure=4; continue; }
                const auto& p = actual.placement;
                // Compare the observed marker endpoints in submitted pixels.
                // Extrapolating a 40px marker's fitted scale to the far crop edge
                // amplifies one pixel of recognition noise into ~40px of error.
                const auto endpoint_error = [](double marker, double origin, double extent,
                    double expected_origin, double expected_extent, double pixels) {
                    return std::abs((marker-origin)/extent -
                        (marker-expected_origin)/expected_extent) * pixels;
                };
                double dx{}, dy{};
                if (p.width <= 0 || p.height <= 0) { rejection |= 128U; f.marker_failure=4; continue; }
                for (const double offset : {0., 40.}) {
                    dx = (std::max)(dx, endpoint_error(expected.marker.x+offset, p.x, p.width,
                        expected.x, expected.width, f.submitted_sizes[slot][0]));
                    dy = (std::max)(dy, endpoint_error(expected.marker.y+offset, p.y, p.height,
                        expected.y, expected.height, f.submitted_sizes[slot][1]));
                }
                f.marker_error_x=(std::max)(f.marker_error_x,dx); f.marker_error_y=(std::max)(f.marker_error_y,dy);
                geometry_changed |= dx > 32 || dy > 32;
            }
            if (geometry_changed) { rejection |= 128U; f.marker_failure=5; }
        }
        std::array<StereoSourceCrop, 2> crops{};
        if (!rejection) for (unsigned eye = 0; eye < 2; ++eye) {
            const unsigned slot = eye ? right_slot : left_slot;
            const unsigned c = unsigned(eye ? right : left);
            const auto& p = acquired ? f.search[slot]->result.placement :
                f.placement_plans[c].for_eye(slot, unsigned(eye ? selected_right : selected));
            const auto& view = f.views[c];
            crops[eye] = {float(p.x/view.width), float(p.y/view.height), float(p.width/view.width),
                float(p.height/view.height), view.width, view.height, true};
        }
        // A failed wide search only advances acquisition; it never authenticates
        // pixels. Its CPU work can exceed the publication/tracking age limit.
        // Discarding that failure would retry the same clipped corner forever.
        const bool failed_wide_search = f.wide_search && rejection == 128U;
        const auto signature = calibration_signature(s, f);
        if (enabled && f.epoch == s.epoch && !s.policy.signature_checked && (rejection & ~128U) == 0) {
            s.policy.signature_checked = true;
            s.policy.started_ms = s.policy.stage_ms = GetTickCount64();
            const auto settings = configured_settings();
            const auto learned = settings.eye_calibration_learned_method;
            if (s.policy.configured == EyeCalibrationMethod::automatic &&
                signature == settings.eye_calibration_learned_signature && learned >= 1 && learned <= 3 &&
                (learned != 2 || settings.eye_calibration_learned_sessions >= 2)) {
                s.policy.select(static_cast<EyeCalibrationMethod>(learned), GetTickCount64());
                s.frames_until_capture = 0;
            }
        }
        // Do not let an old epoch or out-of-order completion undo a newer placement.
        if (enabled && !retaining_calibration(s) && f.epoch == s.epoch && f.method == s.policy.active && f.sequence > s.placement_sequence &&
            (failed_wide_search || GetTickCount64() - f.captured_ms < (f.wide_search ? 10000U : 1000U))) {
            placement_epoch(s);
            s.placement_sequence = f.sequence;
            if (!rejection) {
                if (f.wide_search && acquired) ++s.stats.full_calibration_successes;
                if (!acquired) for (unsigned physical=0;physical<2;++physical) {
                    const auto slot=physical ? right_slot : left_slot;
                    const auto c=unsigned(physical ? right : left);
                    const auto h=unsigned(physical ? selected_right : selected);
                    const auto index=4+h*8+unsigned(flipped_pair)*4+slot*2+c;
                    const auto& result=f.tracked[index];
                    auto& hint=s.tracking_hints[index];
                    if (result.valid && f.sequence>hint.sequence)
                        hint={f.tracking[index],result.placement,f.epoch,f.sequence,f.captured_ms,f.views[c].id,f.views[c].generation};
                }
                s.tracking_misses = s.search_candidate = 0;
                for (unsigned c = 0; c < 2; ++c) {
                    if (!(source_mask & (1U << c))) continue;
                    if (!s.placements[c].locked) ++s.placement_locks;
                    auto& learned = s.placements[c];
                    if (acquired) {
                        const unsigned first_eye = f.search[0]->result.candidate == c ? 0U : 1U;
                        const auto corner = [&](unsigned eye) {
                            return calibration_padded_corner(f.search[eye]->result.placement,
                                f.views[c].width, f.views[c].height, c, f.submitted_sizes[eye][0], f.submitted_sizes[eye][1]);
                        };
                        learned = {f.views[c], f.submitted_sizes, corner(first_eye), true, true, {}};
                        for (unsigned eye = 0; eye < 2; ++eye)
                            learned.eye_placements[eye] = f.search[eye]->result.candidate == c ?
                                corner(eye) : learned.placement;
                    } else {
                        const auto& plan = f.placement_plans[c];
                        const unsigned eye = int(c) == left ? left_slot : right_slot;
                        const auto chosen_for = [&](unsigned physical_eye, unsigned hypothesis) {
                            // Verification checks a cached crop; do not let single-marker
                            // scale noise or head motion gradually move the gaze mapping.
                            return plan.for_eye(physical_eye, hypothesis);
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
                s.policy.confirmations = 0;
                const bool locked = s.placements[0].locked || s.placements[1].locked;
                const bool inconclusive = f.motion_unreliable && !ambiguous && locked;
                if (inconclusive) { ++s.motion_inconclusive; s.tracking_misses = 0; }
                if (geometry_changed || ambiguous) {
                    ++s.geometry_rejections;
                    if (ambiguous) invalidate_stereo_crop();
                }
                // Motion-sensitive misses and shifts do not refresh crop validity.
                // After settling, require twenty bad samples before reacquiring.
                // Ambiguous identity still invalidates immediately.
                if (!locked && !f.wide_search) {
                    if (geometry_changed && !f.motion_unreliable && s.policy.configured == EyeCalibrationMethod::automatic) {
                        s.policy.recover(GetTickCount64()); s.frames_until_capture = 0;
                    } else if (s.policy.failed(GetTickCount64(), f.motion_unreliable)) s.frames_until_capture = 0;
                } else if (!inconclusive && (!locked || ambiguous || ++s.tracking_misses >= eye_calibration_failure_limit)) {
                    request_full_calibration(s, ambiguous ? "ambiguous_identity" : locked ? "verification_failure_limit" : "no_locked_crop");
                    invalidate_stereo_crop();
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
            if (enabled && !retaining_calibration(s) && f.epoch == s.epoch && f.method == s.policy.active) {
                bool corrected{};
                if (publish_stereo_calibration(f.views[left].id, f.views[right].id, f.views[left].generation,
                                               f.views[right].generation, f.sequence, f.captured_ms,
                                               &corrected, f.session_generation, flipped_pair, mono, &crops)) {
                    s.last_verified_ms = f.captured_ms;
                    s.published = true;
                    s.policy.failures = 0;
                    const unsigned learning_confirmations = eye_calibration_continuous_validation() ? 4U : 1U;
                    if (++s.policy.confirmations == learning_confirmations && s.policy.configured == EyeCalibrationMethod::automatic) {
                        const auto settings = configured_settings();
                        const auto method = unsigned(s.policy.active);
                        const bool same = settings.eye_calibration_learned_signature == signature &&
                            settings.eye_calibration_learned_method == method;
                        const bool already = s.learned_this_launch_signature == signature && s.learned_this_launch_method == method;
                        const auto sessions = same ? settings.eye_calibration_learned_sessions + (already ? 0U : 1U) : 1U;
                        set_eye_calibration_learning(method, signature, sessions);
                        s.learned_this_launch_signature = signature; s.learned_this_launch_method = method;
                    }
                    ++s.stats.applied;
                    if (corrected)
                        ++s.stats.corrections;
                } else ++s.stats.publication_rejected;
            } else ++s.stats.publication_rejected;
        }
        double verification_ms{};
        for (const auto& tracked : f.tracked) if (tracked.tracking_cpu_ms>0) {
            verification_ms+=tracked.tracking_cpu_ms;
            ++s.stats.verification_patch_calls;
            if (tracked.tracking_path>=1 && tracked.tracking_path<=3) ++s.stats.verification_paths[tracked.tracking_path-1];
        }
        s.stats.verification_cpu_ms+=verification_ms;
        s.stats.verification_last_ms=verification_ms;
        s.stats.verification_peak_ms=(std::max)(s.stats.verification_peak_ms,verification_ms);
        log_calibration_capture(s, f, rejection);
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
    std::vector<unsigned char> data(72 * 72 * calibration_placement_count * bytes);
    for (unsigned n = 0; n < points.count; ++n)
        for (unsigned i = 0; i < 72 * 72; ++i)
            if (points.points[n].locator || (i % 72 < 40 && i / 72 < 40)) calibration_encode_locator(data.data() + (n * 72 * 72 + i) * bytes, format, candidate,
                                       i % 72, i / 72, points.points[n].code, points.points[n].locator);
    if (!s.markers[candidate]) {
        const D3D11_TEXTURE2D_DESC desc{72, 72 * calibration_placement_count, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA initial{data.data(), 72 * bytes, 0};
        if (FAILED(device->CreateTexture2D(&desc, &initial, &s.markers[candidate]))) return false;
        ++s.stats.allocations;
    } else context->UpdateSubresource(s.markers[candidate].Get(), 0, nullptr, data.data(), 72 * bytes, 0);
    s.marker_layouts[candidate] = points;
    return true;
}
void stamp_points11(State& s, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                    unsigned c, const CalibrationMarkerPoints& points) {
    for (unsigned i = 0; i < points.count; ++i) {
        const unsigned size = points.points[i].locator ? 72 : 40;
        const unsigned pad = points.points[i].locator ? 16 : 0;
        const D3D11_BOX box{0, i * 72, 0, size, i * 72 + size, 1};
        context->CopySubresourceRegion(texture, 0, points.points[i].x-pad, points.points[i].y-pad,
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
    // Delayed submissions may use any intervening render. Keep the acquisition
    // grid present until a crop is locked, then retain its corner markers.
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
            Calibration12StampMode::marker_only, points.extra(), points.points[0].locator)) {
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
void eye_calibration_recalibrate() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (enabled) restart_calibration(s, "manual_recalibration");
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
    result.full_calibration_attempts=s.wide_searches;
    result.verification_failure_streak=s.tracking_misses;
    result.active_method = s.policy.active;
    result.acquisition_failure_streak = s.policy.failures;
    result.acquisition_confirmations = s.policy.confirmations;
    result.cpu_us_per_frame = result.frames ? s.cpu_us / result.frames : 0;
    result.verification_cpu_us_per_frame=result.frames ? result.verification_cpu_ms*1000/result.frames : 0;
    result.gpu_us = s.gpu.get();
    result.latency_frames = s.latency.get();
    result.backend = s.backend;
    result.runtime_active = s.last_frame_ms && GetTickCount64() - s.last_frame_ms <= 1000;
    result.openvr_active = result.runtime_active && s.backend == EyeCalibrationBackend::openvr;
    result.unsupported_submission = s.unsupported_submission;
    result.correction_active = result.enabled && stereo_eye_assignment(result.left_view).calibrated &&
                               stereo_eye_assignment(result.right_view).calibrated;
    result.vertical_flip = result.correction_active && stereo_eye_assignment(result.left_view).vertical_flip;
    const auto assignment = stereo_eye_assignment(result.left_view);
    result.crop_mapping_active = result.correction_active && assignment.source_crops[0].valid &&
        assignment.source_crops[1].valid;
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
    if (s.policy.configured != eye_calibration_selected_method())
        restart_calibration(s, "method_changed");
    const bool capture_due = s.frames_until_capture == 0;
    if (s.frames_until_capture) --s.frames_until_capture;
    if (s.current >= 0) {
        auto& f = s.ring[s.current];
        // AER can submit the previous eye pair between the two DLSS renders.
        // Keep sources across intervals, but never combine submission pairs.
        if (on && (f.pipelined || f.collect_source_pair) && f.epoch == s.epoch && !f.invalid && !f.submits &&
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
    if (retaining_calibration(s)) {
        for (const auto& p : s.placements) {
            if (p.locked && stereo_view_generation(p.view.id) != p.view.generation) {
                restart_calibration(s, "source_or_submitted_geometry_changed");
                break;
            }
        }
        if (retaining_calibration(s)) return false;
    }
    if (eye_calibration_continuous_validation() && s.last_verified_ms && now - s.last_verified_ms > 2500) {
        // Stop using stale gaze geometry, but retain corner probes. Movement
        // alone must not flash the grid; settled verification decides reacquisition.
        invalidate_stereo_crop();
        s.published = false;
    }
    // A single acquisition pair owns the wide readbacks and CPU workers.
    // Keep only one acquisition in flight; never queue more full-image searches.
    if (std::any_of(s.ring.begin(), s.ring.end(), [](const auto& frame) {
        return frame.busy && frame.wide_search;
    })) return false;
    // Drain every frame. Sample one in ten except during timing recovery. DX11 OpenXR
    // submissions use stable corner stamps on intervening renders, including DX12 sources.
    if (!capture_due) return false;
    if (s.search_needed && s.policy.active == EyeCalibrationMethod::full && s.last_wide_search_ms && now - s.last_wide_search_ms < 200) return false;
    s.frames_until_capture = s.search_needed && s.policy.active == EyeCalibrationMethod::timing ? 0 : 9;
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
        // Native DX12 submissions can also reuse the previous eye under AFW.
        // Keep both source proofs, without selecting the mixed DX12/DX11 readback path.
        f.collect_source_pair = graphics_api == 12;
        f.protected_context = false;
        f.codes = (f.pipelined || f.collect_source_pair) ? capture_codes(s.epoch) :
            std::array<std::uint32_t, 2>{};
        f.sequence = s.sequence;
        f.support = claim_calibration_images(f.sequence, session_generation, f.codes);
        f.support11 = {};
        f.search = {}; f.search11 = {};
        f.method = s.policy.active;
        f.submitted_signature = {};
        f.wide_search = s.search_needed && s.policy.active == EyeCalibrationMethod::full && (!s.last_wide_search_ms || now - s.last_wide_search_ms >= 200);
        if (f.wide_search) { ++s.wide_searches; s.last_wide_search_ms = now; }
        f.busy = true;
        f.closed = f.close_requested = f.invalid = f.queries_started = false;
        f.epoch = s.epoch;
        f.session_generation = session_generation;
        f.captured_ms = GetTickCount64();
        f.motion_unreliable = motion_unreliable(session_generation);
        f.evaluations = f.submits = 0;
        f.marker_failure=0; f.shared_source_assumed=false; f.marker_error_x=f.marker_error_y=0;
        f.views = {};
        f.placement_plans = {}; f.submitted_sizes = {}; f.placement_count = 1;
        f.tracking = {}; f.tracked = {}; f.tracking_inputs={}; f.verification.reset(); f.verification_staged=false;
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
        observe_source(s, view, width, height);
        if (retaining_calibration(s)) return;
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
        observe_source(s, view, width, height);
        if (retaining_calibration(s)) return;
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
        if (f.collect_source_pair) {
            if (f.submits || f.invalid || f.epoch != s.epoch) return;
            for (unsigned i = 0; i < 2; ++i) {
                if (f.views[i].id == view) { c = i; repeated = true; break; }
            }
        }
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
            f.support, image, points.points[0].code, repeated ? Calibration12StampMode::refresh : Calibration12StampMode::source_proof, points.extra(), points.points[0].locator)) {
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
    if (s.backend != backend || s.session_generation != generation)
        return 0;
    const auto observed_desc = texture->GetDesc();
    if (observed_desc.Width <= UINT32_MAX)
        observe_submission(s, eye, unsigned(observed_desc.Width), observed_desc.Height, u0, v0, u1, v1, slice);
    if (s.current < 0 || s.ring[s.current].epoch != s.epoch || retaining_calibration(s)) return 0;
    auto& f = s.ring[s.current];
    s.stats.submission_graphics_api = 12;
    if (f.collect_source_pair && f.evaluations < 2) {
        ++s.waiting_for_sources;
        return 0;
    }
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
    submitted_geometry(s, f, eye, unsigned(d.Width), d.Height, u0, v0, u1, v1, slice);
    std::array<D3D12_BOX, calibration_box_count> boxes;
    std::array<std::uint32_t, calibration_box_count> codes;
    std::array<unsigned, calibration_box_count> mirrors;
    std::array<CalibrationTrackingPatch, calibration_box_count> tracking;
    const unsigned box_count = f.placement_count * 4;
    for (unsigned index = 0; index < box_count; ++index) {
        const auto c = index % 2;
        const auto& ref = f.views[c].width ? f.views[c] : f.views[f.views[0].width ? 0 : 1];
        if (!ref.width || !ref.height) { f.invalid = true; return 0; }
        const auto r = submitted_rect(s, f, eye, index, unsigned(d.Width), d.Height, u0, v0, u1, v1);
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
        if (s.backend != backend || s.session_generation != session_generation)
            return 0;
        if (s.current < 0) {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            observe_submission(s, eye, desc.Width, desc.Height, u0, v0, u1, v1, slice);
            return 0;
        }
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
    observe_submission(s, eye, desc.Width, desc.Height, u0, v0, u1, v1, slice);
    if (f.epoch != s.epoch || retaining_calibration(s)) return 0;
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
    submitted_geometry(s, f, eye, desc.Width, desc.Height, u0, v0, u1, v1, slice);
    image.sample_count = f.placement_count * 4;
    for (unsigned index = 0; index < image.sample_count; ++index) {
        const unsigned c = index % 2;
        const auto& ref = f.views[c].width ? f.views[c] : f.views[f.views[0].width ? 0 : 1];
        if (!ref.width || !ref.height) {
            f.invalid = true;
            continue;
        }
        const auto r = submitted_rect(s, f, eye, index, desc.Width, desc.Height, u0, v0, u1, v1);
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
        return !stats.crop_mapping_active ? "Eye identified; acquiring crop" :
            stats.left_view == stats.right_view ? "Active (shared mono source)" : "Active";
    return stats.active_method == EyeCalibrationMethod::full ? "Acquiring crop and eye mapping" : "Acquiring corner eye mapping";
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
        << ",\"continuous_validation\":" << eye_calibration_continuous_validation()
        << ",\"active_method\":\"" << eye_calibration_method_name(s.active_method) << "\""
        << ",\"acquisition_failure_streak\":" << s.acquisition_failure_streak
        << ",\"acquisition_confirmations\":" << s.acquisition_confirmations
        << ",\"vertical_flip\":" << s.vertical_flip << ",\"crop_mapping_active\":" << s.crop_mapping_active
        << ",\"unsupported_submission\":" << s.unsupported_submission
        << ",\"unsupported_submissions\":" << s.unsupported_submissions << ",\"frames\":" << s.frames
        << ",\"captures\":" << s.captures << ",\"completed\":" << s.completed << ",\"valid\":" << s.valid
        << ",\"skipped\":" << s.skipped << ",\"in_flight\":" << s.in_flight
        << ",\"corrections\":" << s.corrections << ",\"applied\":" << s.applied
        << ",\"mismatches\":" << s.mismatches << ",\"allocations\":" << s.allocations
        << ",\"search_left_ms\":" << s.search_ms[0] << ",\"search_right_ms\":" << s.search_ms[1]
        << ",\"full_calibration_attempts\":" << s.full_calibration_attempts
        << ",\"full_calibration_successes\":" << s.full_calibration_successes
        << ",\"recalibration_requests\":" << s.recalibration_requests
        << ",\"verification_failure_streak\":" << s.verification_failure_streak
        << ",\"full_calibration_reason\":\"" << s.full_calibration_reason << '"'
        << ",\"verification_background\":true"
        << ",\"verification_exact\":" << s.verification_paths[0]
        << ",\"verification_nearby\":" << s.verification_paths[1]
        << ",\"verification_broad\":" << s.verification_paths[2]
        << ",\"verification_cpu_us_per_frame\":" << s.verification_cpu_us_per_frame
        << ",\"verification_last_ms\":" << s.verification_last_ms
        << ",\"verification_peak_ms\":" << s.verification_peak_ms
        << ",\"verification_patch_calls\":" << s.verification_patch_calls
        << ",\"capture_total_left_ms\":" << s.capture_total_ms[0] << ",\"capture_total_right_ms\":" << s.capture_total_ms[1]
        << ",\"capture_setup_left_ms\":" << s.capture_setup_ms[0] << ",\"capture_setup_right_ms\":" << s.capture_setup_ms[1]
        << ",\"capture_wait_left_ms\":" << s.capture_wait_ms[0] << ",\"capture_wait_right_ms\":" << s.capture_wait_ms[1]
        << ",\"capture_map_left_ms\":" << s.capture_map_ms[0] << ",\"capture_map_right_ms\":" << s.capture_map_ms[1]
        << ",\"capture_copy_left_ms\":" << s.capture_copy_ms[0] << ",\"capture_copy_right_ms\":" << s.capture_copy_ms[1]
        << ",\"peak_capture_setup_ms\":" << s.peak_capture_setup_ms
        << ",\"peak_capture_wait_ms\":" << s.peak_capture_wait_ms
        << ",\"peak_capture_map_ms\":" << s.peak_capture_map_ms
        << ",\"peak_capture_copy_ms\":" << s.peak_capture_copy_ms
        << ",\"peak_capture_other_ms\":" << s.peak_capture_other_ms
        << ",\"peak_capture_sequence\":" << s.peak_capture_sequence
        << ",\"peak_capture_eye\":" << s.peak_capture_eye
        << ",\"peak_capture_width\":" << s.peak_capture_width
        << ",\"peak_capture_height\":" << s.peak_capture_height
        << ",\"peak_capture_map_polls\":" << s.peak_capture_map_polls
        << ",\"capture_timing_samples\":" << s.capture_timing_samples << ",\"max_capture_ms\":" << s.max_capture_ms
        << ",\"max_search_ms\":" << s.max_search_ms << ",\"search_timing_samples\":" << s.search_timing_samples
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
    out << "},\"marker_failure_counts\":{";
    for(unsigned i=0;i<s.marker_failure_counts.size();++i) {
        if(i) out << ',';
        out << '"' << marker_failure_names[i+1] << "\":" << s.marker_failure_counts[i];
    }
    out << "},\"last_rejection\":{\"sequence\":" << s.last_rejected_sequence
        << ",\"mask\":" << s.last_rejection_mask
        << ",\"marker_failure_detail\":\"" << s.last_marker_failure << '"'
        << ",\"source_mask\":" << s.last_source_mask
        << ",\"shared_source_assumed\":" << s.last_shared_source_assumed
        << ",\"motion_unreliable\":" << s.last_motion_unreliable
        << ",\"marker_error_x_px\":" << s.last_marker_error_x << ",\"marker_error_y_px\":" << s.last_marker_error_y
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
    out << "],\"patch_search_diagnostics\":[";
    for(unsigned i=0;i<s.last_best_scores.size();++i) {
        if(i) out << ',';
        out << "{\"patch\":" << i << ",\"best_sampled_score\":" << s.last_best_scores[i]
            << ",\"bits_at_best_score\":" << s.last_score_bits[i] << ",\"contrast_at_best_score\":" << s.last_best_contrasts[i]
            << ",\"best_coarse_bits\":" << s.last_best_bits[i] << ",\"max_contrast\":" << s.last_max_contrasts[i]
            << ",\"positions\":" << s.last_search_positions[i] << ",\"scored_candidates\":" << s.last_score_probes[i]
            << ",\"low_contrast_positions\":" << s.last_low_contrast_positions[i] << '}';
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
        {
            std::lock_guard motion_lock(motion.mutex);
            const auto now = GetTickCount64();
            out << ",\"calibration_motion\":{\"observations\":" << motion.observations
                << ",\"session\":" << motion.session
                << ",\"age_ms\":" << (motion.observations ? now-motion.observed_ms : 0)
                << ",\"fresh\":" << (motion.observations && now-motion.observed_ms < 250)
                << ",\"orientation_valid\":" << motion.valid
                << ",\"degrees_per_second\":" << motion.degrees_per_second
                << ",\"peak_degrees_per_second\":" << motion.peak_degrees_per_second
                << ",\"fast_samples\":" << motion.fast_samples
                << ",\"settling\":" << (now < motion.unstable_until)
                << ",\"settle_ms\":350}";
        }
        out << ",\"capture_diagnostics\":{\"slow_calls_over_20ms\":" << live.slow_calls
            << ",\"sampled_black_images\":" << live.sampled_black_images << ",\"last_searches\":[";
        for(unsigned eye=0;eye<2;++eye) {
            if(eye) out << ',';
            const auto& q=live.last_search_diagnostics[eye];
            if(!q) { out << "null"; continue; }
            out << "{\"width\":" << q->image_width << ",\"height\":" << q->image_height
                << ",\"texture\":\"" << q->texture_identity << "\",\"map_hresult\":" << q->map_result
                << ",\"map_polls\":" << q->map_polls
                << ",\"copy_to_map_ms\":" << (q->copy_issued_ms && q->readback_ready_ms>=q->copy_issued_ms ? q->readback_ready_ms-q->copy_issued_ms : 0)
                << ",\"raw_copy_ms\":" << q->conversion_ms << ",\"worker_ms\":" << q->elapsed_ms
                << ",\"locator_initial_factor\":" << q->initial_locator_factor << ",\"locator_last_factor\":" << q->last_locator_factor
                << ",\"locator_passes\":" << q->locator_passes << ",\"budget_exhausted\":" << q->search_budget_exhausted
                << ",\"samples\":" << q->sample_count << ",\"black_samples\":" << q->sampled_black
                << ",\"luma_min\":" << q->sampled_min << ",\"luma_max\":" << q->sampled_max
                << ",\"luma_mean\":" << q->sampled_mean << '}';
        }
        out << "]}";
        out << ",\"placement_search\":{\"hypotheses\":" << calibration_placement_count
            << ",\"locks\":" << live.placement_locks << ",\"losses\":" << live.placement_losses
            << ",\"search_candidate\":" << live.search_candidate
            << ",\"tracking_misses\":" << live.tracking_misses
            << ",\"tracking_mode\":\"cached_crop_with_verified_position_hint\",\"hint_max_age_ms\":1000,\"nearby_search_radii_px\":[32,64],\"local_search_coarse_to_fine\":true,\"local_search_refine_peaks\":8,\"tracking_padding_px\":64,\"tracking_max_patch_px\":256"
            << ",\"acquisition_marker_size_px\":72,\"locator_target_long_side\":800,\"locator_min_factor\":4,\"locator_max_factor\":32,\"locator_fallback_downsample\":2,\"wide_search_budget_ms\":100"
            << ",\"verification_inset_px\":20,\"verification_tolerance_px\":32,\"verification_failure_limit\":" << eye_calibration_failure_limit
            << ",\"max_acquisition_markers_per_source\":16,\"grid_min_points\":3"
            << ",\"stamping\":\"bordered_grid_then_small_corner\""
            << ",\"motion_inconclusive\":" << live.motion_inconclusive
            << ",\"geometry_rejections\":" << live.geometry_rejections
            << ",\"crop_max_age_ms\":" << (eye_calibration_continuous_validation() ? 2500 : 0)
            << ",\"motion_threshold_degrees_per_second\":90"
            << ",\"verification_age_ms\":" << (live.last_verified_ms ? GetTickCount64()-live.last_verified_ms : 0)
            << ",\"wide_searches\":" << live.wide_searches
            << ",\"wide_search_pending\":" << std::any_of(live.ring.begin(), live.ring.end(), [](const auto& f) { return f.busy && f.wide_search; })
            << ",\"last_wide_results\":[";
        for (unsigned eye = 0; eye < 2; ++eye) {
            if (eye) out << ',';
            const auto& r = live.last_search_results[eye];
            out << "{\"valid\":" << r.valid << ",\"ambiguous\":" << r.ambiguous << ",\"candidate\":" << r.candidate
                << ",\"flipped\":" << r.flipped << ",\"score\":" << r.score << ",\"support_points\":" << r.support_points
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

extern "C" __declspec(dllexport) void __cdecl
CheekyEyeCalibration_ObservePoseV1(std::uint64_t session, std::uint64_t space,
    std::int64_t time, const float* xyzw, bool valid) noexcept {
    using namespace cheeky::foveated_dlss;
    std::lock_guard lock(motion.mutex);
    const auto now = GetTickCount64();
    ++motion.observations;
    double norm{};
    if (xyzw) for (unsigned i = 0; i < 4; ++i) norm += double(xyzw[i])*xyzw[i];
    valid = valid && xyzw && std::isfinite(norm) && norm > .5 && norm < 1.5 && time > 0;
    if (motion.session != session || motion.space != space) {
        motion.valid = false; motion.unstable_until = 0; motion.time = 0;
    }
    std::array<float, 4> q{};
    if (valid) for (unsigned i = 0; i < 4; ++i) q[i] = float(xyzw[i]/std::sqrt(norm));
    if (valid && motion.valid && time > motion.time) {
        const double dt = double(time-motion.time)*1e-9;
        if (dt >= .001 && dt <= .25) {
            double dot{};
            for (unsigned i = 0; i < 4; ++i) dot += double(q[i])*motion.orientation[i];
            const double speed = 2*std::acos(std::clamp(std::abs(dot), 0., 1.))/dt;
            motion.degrees_per_second = speed * 57.2957795131;
            motion.peak_degrees_per_second = (std::max)(motion.peak_degrees_per_second, motion.degrees_per_second);
            if (speed > 1.57079632679) { ++motion.fast_samples; motion.unstable_until = now + 350; } // 90 degrees/s.
        }
    }
    // Repeated LocateViews calls for the same time must not advance the baseline.
    if (!valid || time != motion.time) { motion.orientation = q; motion.time = time; }
    motion.session = session; motion.space = space; motion.valid = valid; motion.observed_ms = now;
}
