#include "libovr_gaze.hpp"
#include "libovr_abi.hpp"
#include "libovr_gaze_math.hpp"
#include "pvr_abi.hpp"
#include "eye_calibration.hpp"
#include "eye_calibration_bridge.h"
#include "../openxr_layer/eye_calibration.hpp"
#include "libovr_calibration.hpp"
#include "openvr_vtable_hook.hpp"
#include "gaze_math.hpp"
#include "runtime.hpp"
#include "settings.hpp"
#include <Windows.h>
#include <Psapi.h>
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

namespace cheeky::foveated_dlss {
namespace {
using Microsoft::WRL::ComPtr;
namespace ovr = libovr;

// Distinct from OpenXR layer (from 1) and OpenVR (from 2^63) generations.
constexpr std::uint64_t first_generation = 0x4000000000000001ULL;
constexpr ULONGLONG submission_retention_ms = 500;
// A tracker whose sample time stops advancing is stale, whatever its clock.
constexpr ULONGLONG pvr_stall_ms = 200;

// Installation state, protected by hook_mutex.
std::mutex hook_mutex;
std::atomic<bool> stopping{}, armed{};
ULONGLONG next_scan{};
bool arm_failed{};
char module_name[64]{};
std::vector<void*> inline_hooks;
std::vector<HMODULE> retained_modules;

ovr::InitializeFn original_initialize{};
ovr::ShutdownFn original_shutdown{};
ovr::CreateFn original_create{};
ovr::DestroyFn original_destroy{};
ovr::DestroySwapChainFn original_destroy_chain{};
ovr::CommitFn original_commit{};
ovr::EndFrameFn original_end_frame{}, original_submit_frame{}, original_submit_frame2{};
ovr::GetCurrentIndexFn get_current_index{};
ovr::GetBufferDXFn get_buffer_dx{};
// Requested client minor version from ovr_Initialize; 0 when it was missed.
std::atomic<std::uint32_t> requested_minor{};
thread_local unsigned frame_depth{}, commit_depth{};

// Pimax PVR client. Its interface table is patched per slot, never its code.
struct PvrClient {
    HMODULE module{};
    std::uint32_t minor{};
    std::vector<OpenVRVtableHook> slots;
    std::array<void*, pvr::slot_count> original_slots{};
    bool attach_failed{}, implementations_observed{};
} pvr_client;  // hook_mutex
std::atomic<bool> pvr_ready{};
pvr::GetTimeSecondsFn pvr_time{};
pvr::GetEyeTrackingInfoFn pvr_eye_tracking{};
pvr::FrameFn pvr_end_frame{}, pvr_submit_frame{};
pvr::GetTrackingStateFn pvr_tracking_state{};
pvr::DestroyHmdFn pvr_destroy_hmd{};
// The game's own PVR session, as used by the LibOVR compatibility runtime.
// Sampling holds this lock shared; destroyHmd waits for it exclusively.
SRWLOCK hmd_lock = SRWLOCK_INIT;
std::atomic<pvr::HmdHandle> frame_hmd{}, tracking_hmd{};
// First observation that found the session: 0 none, 1 interface table, 2 implementation.
std::atomic<unsigned> capture_route{};
// LibOVR frames published while PVR was attached but no session was seen.
std::atomic<std::uint32_t> frames_without_session{};
constexpr std::uint32_t implementation_fallback_frames = 90;

struct PvrSample {
    bool attached{}, session{}, valid{}, supported{};
    float ray[3]{0, 0, -1};
    pvr::Vector2f tan{};
    double time{};
    ULONGLONG age_ms{};
    std::uint64_t valid_samples{};
};
std::mutex sample_mutex;
struct SampleCache {
    PvrSample sample;
    pvr::HmdHandle hmd{};
    std::uint64_t qpc{};
    double last_time{};
    ULONGLONG last_change_ms{};
    bool ever_valid{};
} sample_cache;  // sample_mutex
PvrSample sample_pvr() noexcept;
void forget_hmd(pvr::HmdHandle hmd) noexcept;

// Session state, protected by state_mutex. Never held while calling LibOVR,
// PVR or eye calibration; calibration_mutex is never taken while holding it.
struct KnownChain {
    ovr::SwapChain chain{};
    int committed{-1};
};
struct Submitted {
    ComPtr<IUnknown> identity;
    ovr::SwapChain chain{};
    CheekyGazeViewV1 view{};
    ULONGLONG observed{};
};
using Retired = std::array<std::vector<Submitted>, 2>;
std::mutex state_mutex;
ovr::Session session{};
std::uint64_t generation{}, next_generation{first_generation}, frame{}, swapchain_generation{first_generation};
std::vector<KnownChain> chains;
LibOVRLayout layout{LibOVRLayout::unknown};
std::array<ovr::FovPort, 2> fovs{};
bool fovs_valid{}, submitted_rotation{};
float eye_to_head[2][3][4]{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}, {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
CheekyGazeSnapshotV1 snapshot{};
std::array<std::vector<Submitted>, 2> submitted;
bool simulate{};
std::uint32_t pattern{};
ULONGLONG simulation_start{};
std::uint64_t frames_observed{}, projection_frames{};
unsigned projection_layers{}, submission_api{};

std::mutex calibration_mutex;
openxr_calibration::Frame calibration;  // calibration_mutex
bool calibration_deferred{};             // calibration_mutex
std::atomic<std::uint64_t> calibration_deferrals{};

std::uint64_t qpc() noexcept { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return static_cast<std::uint64_t>(value.QuadPart); }
std::uint64_t qpc_frequency() noexcept { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return static_cast<std::uint64_t>(value.QuadPart); }

const CheekyEyeCalibrationBridgeV1* calibration_bridge() noexcept {
    static const CheekyEyeCalibrationBridgeV1 api = [] {
        CheekyEyeCalibrationBridgeV1 value;
        value.begin = [](std::uint64_t gen, std::uint32_t graphics) noexcept {
            return eye_calibration_frame(EyeCalibrationBackend::libovr, gen, graphics);
        };
        value.capture = [](std::uint64_t gen, void* texture, void*, std::uint32_t graphics, std::uint32_t eye,
                           std::uint32_t slice, float u0, float v0, float u1, float v1) noexcept {
            if (graphics != 11) return std::uint64_t{};
            return eye_calibration_submit(static_cast<ID3D11Texture2D*>(texture), eye, u0, v0, u1, v1, slice,
                                          EyeCalibrationBackend::libovr, gen);
        };
        value.result = eye_calibration_result;
        value.destroy = eye_calibration_destroy_session;
        return value;
    }();
    return &api;
}

void destroy_calibration(std::uint64_t ended) noexcept {
    if (!ended) return;
    std::lock_guard lock(calibration_mutex);
    if (calibration.generation == ended) {
        calibration.destroy(calibration_bridge());
        calibration_deferred = false;
    }
    else eye_calibration_destroy_session(ended);
}

// Caller holds state_mutex. Returns the ended generation, if any.
std::uint64_t clear_session_locked(Retired& retired) noexcept {
    const auto ended = generation;
    session = nullptr;
    generation = 0;
    chains.clear();
    fovs_valid = submitted_rotation = false;
    libovr_eye_rotations({}, eye_to_head);
    snapshot = {};
    for (unsigned eye = 0; eye < 2; ++eye) retired[eye].swap(submitted[eye]);
    ++swapchain_generation;
    simulation_start = 0;
    return ended;
}

// Caller holds state_mutex. A new ovrSession starts a new generation.
std::uint64_t ensure_session_locked(ovr::Session value, Retired& retired, bool force = false) noexcept {
    if (!value || (value == session && !force)) return 0;
    const auto ended = clear_session_locked(retired);
    session = value;
    generation = next_generation++;
    trace_event("LibOVR session observed session=%p generation=0x%llx", value,
        static_cast<unsigned long long>(generation));
    return ended;
}

void end_session(ovr::Session value, bool all) noexcept {
    std::uint64_t ended{};
    Retired retired;
    {
        std::lock_guard lock(state_mutex);
        if (!all && (!value || value != session)) return;
        ended = clear_session_locked(retired);
    }
    destroy_calibration(ended);
}

enum class BufferOwnership : unsigned { unknown, owned, borrowed };
std::atomic<BufferOwnership> ownership11{}, ownership12{};

// ovr_GetTextureSwapChainBufferDX is documented as QueryInterface-like, but a
// compatibility runtime may return a borrowed pointer. Measure once rather
// than risk releasing a reference we do not own; a leak is the safe failure.
template <class T>
ComPtr<T> swapchain_buffer(ovr::Session value, ovr::SwapChain chain, int index,
                           std::atomic<BufferOwnership>& ownership) noexcept {
    void* raw{};
    if (!get_buffer_dx || index < 0 || !ovr::succeeded(get_buffer_dx(value, chain, index, __uuidof(T), &raw)) || !raw)
        return {};
    auto* buffer = static_cast<T*>(raw);
    ComPtr<T> result(buffer);
    auto state = ownership.load();
    if (state == BufferOwnership::unknown) {
        const ULONG before = buffer->AddRef();
        buffer->Release();
        void* again{};
        const bool repeated = ovr::succeeded(get_buffer_dx(value, chain, index, __uuidof(T), &again)) && again == raw;
        const ULONG after = buffer->AddRef();
        buffer->Release();
        if (repeated && after == before + 1) state = BufferOwnership::owned;
        else if (repeated && after == before) state = BufferOwnership::borrowed;
        if (state == BufferOwnership::owned) buffer->Release();
        // An inconclusive probe treats only this call as borrowed and retries.
        if (state != BufferOwnership::unknown) {
            ownership.store(state);
            trace_event("LibOVR swap-chain buffers are %s references", state == BufferOwnership::owned ? "owned" : "borrowed");
        }
    }
    if (state == BufferOwnership::owned) buffer->Release();
    return result;
}

struct EyeImage {
    ComPtr<IUnknown> identity;
    unsigned width{}, height{}, api{};
};

EyeImage resolve_image(ovr::Session value, ovr::SwapChain chain, int index) noexcept {
    EyeImage image;
    if (auto texture = swapchain_buffer<ID3D11Texture2D>(value, chain, index, ownership11)) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.ArraySize == 1 && desc.SampleDesc.Count == 1) {
            texture.As(&image.identity);
            image.width = desc.Width; image.height = desc.Height; image.api = 11;
        }
        return image;
    }
    if (auto resource = swapchain_buffer<ID3D12Resource>(value, chain, index, ownership12)) {
        const auto desc = resource->GetDesc();
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 1 &&
            desc.SampleDesc.Count == 1 && desc.Width <= UINT32_MAX) {
            resource.As(&image.identity);
            image.width = static_cast<unsigned>(desc.Width); image.height = desc.Height; image.api = 12;
        }
    }
    return image;
}

// Reading a layer list with the wrong header layout may run past a legacy
// client's structure. Parsing copies only; faults reject the frame.
bool parse_guarded(const void* const* layers, unsigned count, LibOVRLayout candidate_layout,
                   const ovr::SwapChain* known, std::size_t known_count, LibOVRProjection& output) noexcept {
    __try {
        output = parse_libovr_projection(layers, count, candidate_layout, [known, known_count](ovr::SwapChain chain) {
            for (std::size_t i = 0; i < known_count; ++i)
                if (known[i] == chain) return true;
            return false;
        });
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        return false;
    }
}

LibOVRProjection select_projection(ovr::Session value, const void* const* layers, unsigned count) noexcept {
    std::array<ovr::SwapChain, 16> known{};
    std::size_t known_count{};
    auto decided = LibOVRLayout::unknown;
    {
        std::lock_guard lock(state_mutex);
        if (value != session) return {};
        for (const auto& chain : chains)
            if (chain.committed >= 0 && known_count < known.size()) known[known_count++] = chain.chain;
        decided = layout;
    }
    if (const auto minor = requested_minor.load(); minor != 0)
        decided = minor >= ovr::reserved_header_minor_version ? LibOVRLayout::current : LibOVRLayout::legacy;
    // Try the expected layout first. If the client version was set before
    // Cheeky attached, only the matching layout places observed swap chains
    // in ColorTexture, so trying the other one is safe.
    const auto first = decided == LibOVRLayout::legacy ? LibOVRLayout::legacy : LibOVRLayout::current;
    LibOVRProjection result;
    for (const auto candidate : {first, first == LibOVRLayout::current ? LibOVRLayout::legacy : LibOVRLayout::current}) {
        if (parse_guarded(layers, count, candidate, known.data(), known_count, result) && result.valid) {
            decided = candidate;
            break;
        }
    }
    if (result.valid) {
        std::lock_guard lock(state_mutex);
        if (layout != decided) {
            layout = decided;
            trace_event("LibOVR layer header layout=%s requested_minor=%u",
                decided == LibOVRLayout::current ? "current" : "legacy", requested_minor.load());
        }
    }
    return result;
}

void publish_frame(ovr::Session value, const LibOVRProjection& projection, ovr::Result result) {
    std::array<int, 2> indices{{-1, -1}};
    {
        std::lock_guard lock(state_mutex);
        if (value != session) return;
        ++frames_observed;
        if (projection.valid)
            for (unsigned eye = 0; eye < 2; ++eye)
                for (const auto& chain : chains)
                    if (chain.chain == projection.chains[eye]) indices[eye] = chain.committed;
    }
    std::array<EyeImage, 2> images;
    if (projection.valid) {
        images[0] = resolve_image(value, projection.chains[0], indices[0]);
        images[1] = projection.chains[1] == projection.chains[0] && indices[1] == indices[0]
            ? images[0] : resolve_image(value, projection.chains[1], indices[1]);
    }
    const bool focused = result == ovr::success;
    Retired retired;
    std::uint64_t current_generation{};
    unsigned api{};
    {
        std::lock_guard lock(state_mutex);
        if (value != session) return;
        current_generation = generation;
        const auto now = GetTickCount64();
        snapshot = {};
        snapshot.abi_version = CHEEKY_GAZE_ABI_VERSION;
        snapshot.structure_size = sizeof(snapshot);
        snapshot.view_count = 2;
        snapshot.sequence = ++frame;
        // Opaque frame stamp for shared mapping/freshness policy.
        snapshot.predicted_display_time = static_cast<std::int64_t>(frame);
        snapshot.sample_time = snapshot.predicted_display_time;
        snapshot.publication_qpc = qpc();
        snapshot.session_generation = generation;
        snapshot.status_flags = CHEEKY_GAZE_STATUS_LAYER_ACTIVE | CHEEKY_GAZE_STATUS_LIBOVR;
        if (focused) snapshot.status_flags |= CHEEKY_GAZE_STATUS_SESSION_FOCUSED;
        sprintf_s(snapshot.runtime_name, "LibOVR (%s)%s", module_name,
            pvr_ready.load() ? " + Pimax PVR eye tracking" : "");
        fovs_valid = projection.valid;
        if (projection.valid) {
            fovs = projection.fovs;
            submitted_rotation = libovr_eye_rotations(projection.poses, eye_to_head);
            projection_layers = projection.candidates;
            ++projection_frames;
        }
        bool mapped = projection.valid;
        for (unsigned eye = 0; eye < 2; ++eye) {
            auto& history = submitted[eye];
            const auto& image = images[eye];
            const auto& rect = projection.viewports[eye];
            if (projection.valid && image.identity && std::uint64_t(rect.x) + rect.width <= image.width &&
                std::uint64_t(rect.y) + rect.height <= image.height) {
                api = image.api;
                Submitted entry;
                entry.identity = image.identity;
                entry.chain = projection.chains[eye];
                entry.observed = now;
                auto& view = entry.view;
                view.structure_size = sizeof(view);
                view.view_index = eye;
                view.resource_identity = reinterpret_cast<std::uint64_t>(image.identity.Get());
                view.swapchain_identity = reinterpret_cast<std::uint64_t>(projection.chains[eye]);
                view.image_rect_x = rect.x; view.image_rect_y = rect.y;
                view.image_rect_width = static_cast<std::uint32_t>(rect.width);
                view.image_rect_height = static_cast<std::uint32_t>(rect.height);
                view.flags = CHEEKY_GAZE_VIEW_RESOURCE_VALID;
                if (!history.empty()) {
                    const auto& previous = history.back().view;
                    if (previous.image_rect_x != view.image_rect_x || previous.image_rect_y != view.image_rect_y ||
                        previous.image_rect_width != view.image_rect_width ||
                        previous.image_rect_height != view.image_rect_height) {
                        for (auto& old : history) retired[eye].push_back(std::move(old));
                        history.clear();
                        ++swapchain_generation;
                    }
                }
                if (history.empty())
                    trace_event("LibOVR submit eye=%u resource=0x%llx rect=%d,%d %ux%u api=%u", eye,
                        static_cast<unsigned long long>(view.resource_identity), view.image_rect_x, view.image_rect_y,
                        view.image_rect_width, view.image_rect_height, image.api);
                std::erase_if(history, [&](const Submitted& old) {
                    return old.view.resource_identity == view.resource_identity;
                });
                if (history.size() >= 4) { retired[eye].push_back(std::move(history.front())); history.erase(history.begin()); }
                history.push_back(std::move(entry));
            }
            for (auto& old : history)
                if (now - old.observed > submission_retention_ms) retired[eye].push_back(std::move(old));
            std::erase_if(history, [](const Submitted& old) { return !old.identity; });
            auto& view = snapshot.views[eye];
            if (!history.empty()) view = history.back().view;
            else mapped = false;
            view.structure_size = sizeof(view);
            view.view_index = eye;
            if (fovs_valid) {
                view.fov_left = std::atan(-fovs[eye].LeftTan); view.fov_right = std::atan(fovs[eye].RightTan);
                view.fov_up = std::atan(fovs[eye].UpTan); view.fov_down = std::atan(-fovs[eye].DownTan);
                view.flags |= CHEEKY_GAZE_VIEW_FOV_VALID | CHEEKY_GAZE_VIEW_SUBMITTED_PROJECTION;
            }
        }
        snapshot.swapchain_generation = swapchain_generation;
        if (mapped) snapshot.status_flags |= CHEEKY_GAZE_STATUS_MAPPING_READY;
        if (api) submission_api = api;
    }
    // Keep tracker diagnostics current in fixed placement too. DLSS reads
    // reuse this query when it is under a millisecond old.
    if (const auto sample = sample_pvr(); sample.attached && !sample.session) ++frames_without_session;
    // Commit is LibOVR's release: captures happen there with the previous
    // frame's rectangles; this frame confirms which committed image was used.
    std::lock_guard lock(calibration_mutex);
    std::array<openxr_calibration::Region, 2> regions{};
    std::array<std::uint32_t, 2> released{};
    const bool usable = ovr::succeeded(result) && projection.valid && api == 11 && indices[0] >= 0 && indices[1] >= 0;
    if (usable) {
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto& rect = projection.viewports[eye];
            regions[eye] = {reinterpret_cast<std::uint64_t>(projection.chains[eye]), 0, rect.x, rect.y, rect.width, rect.height};
            released[eye] = static_cast<std::uint32_t>(indices[eye]);
        }
    }
    if (libovr_await_second_eye(calibration, usable, calibration_deferred)) {
        calibration_deferred = true;
        if (calibration_deferrals++ == 0) trace_event("LibOVR calibration pairs alternating eye commits across frames");
        return;
    }
    calibration_deferred = false;
    calibration.end(calibration_bridge(), regions, released, usable);
    // Arm the next interval only when this frame showed a capturable D3D11
    // projection; otherwise sources would be stamped with nothing to read back.
    if (api == 12) {
        // Never open an interval for an unsupported path or invalidate
        // another runtime's calibration while reporting it.
        eye_calibration_unsupported_submit(EyeCalibrationBackend::libovr, current_generation);
    }
    else if (api == 11 && projection.valid) calibration.begin(calibration_bridge(), current_generation, 11);
}

void capture_before_commit(ovr::Session value, ovr::SwapChain chain, int index) noexcept {
    std::lock_guard lock(calibration_mutex);
    if (!calibration.active || calibration.graphics_api != 11 || index < 0) return;
    bool needed{};
    for (const auto& region : calibration.history) needed |= region.swapchain == reinterpret_cast<std::uint64_t>(chain);
    if (!needed) return;
    const auto texture = swapchain_buffer<ID3D11Texture2D>(value, chain, index, ownership11);
    if (!texture) return;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    calibration.before_release(calibration_bridge(), reinterpret_cast<std::uint64_t>(chain),
        static_cast<std::uint32_t>(index), texture.Get(), nullptr, desc.Width, desc.Height);
}

ovr::Result initialize_hook(const ovr::InitParams* params) {
    requested_minor.store(params && (params->Flags & ovr::init_request_version) ? params->RequestedMinorVersion : UINT32_MAX);
    return original_initialize(params);
}
void shutdown_hook() {
    end_session(nullptr, true);
    forget_hmd(nullptr);
    original_shutdown();
}
ovr::Result create_hook(ovr::Session* output, void* luid) {
    const auto result = original_create(output, luid);
    if (!stopping.load() && ovr::succeeded(result) && output && *output) {
        std::uint64_t ended{};
        Retired retired;
        {
            std::lock_guard lock(state_mutex);
            ended = ensure_session_locked(*output, retired, true);
        }
        destroy_calibration(ended);
    }
    return result;
}
void destroy_hook(ovr::Session value) {
    end_session(value, false);
    // The runtime ends this session's PVR session inside ovr_Destroy, even if
    // its destroyHmd call bypasses both observation routes.
    forget_hmd(nullptr);
    original_destroy(value);
}
void destroy_chain_hook(ovr::Session value, ovr::SwapChain chain) {
    if (!stopping.load()) {
        {
            std::lock_guard lock(calibration_mutex);
            for (const auto& region : calibration.history)
                if (region.swapchain == reinterpret_cast<std::uint64_t>(chain)) {
                    calibration.destroy(calibration_bridge());
                    calibration_deferred = false;
                    break;
                }
        }
        Retired retired;
        std::lock_guard lock(state_mutex);
        std::erase_if(chains, [&](const KnownChain& known) { return known.chain == chain; });
        for (unsigned eye = 0; eye < 2; ++eye) {
            for (auto& entry : submitted[eye])
                if (entry.chain == chain) retired[eye].push_back(std::move(entry));
            std::erase_if(submitted[eye], [](const Submitted& entry) { return !entry.identity; });
        }
        ++swapchain_generation;
    }
    original_destroy_chain(value, chain);
}
ovr::Result commit_hook(ovr::Session value, ovr::SwapChain chain) {
    if (commit_depth++ || stopping.load() || !value || !chain) {
        const auto result = original_commit(value, chain);
        --commit_depth;
        return result;
    }
    int index{-1};
    if (!get_current_index || !ovr::succeeded(get_current_index(value, chain, &index)) || index < 0 || index > 255)
        index = -1;
    std::uint64_t ended{};
    {
        Retired retired;
        std::lock_guard lock(state_mutex);
        ended = ensure_session_locked(value, retired);
    }
    destroy_calibration(ended);
    // Read before forwarding: the runtime owns the committed image afterwards.
    capture_before_commit(value, chain, index);
    const auto result = original_commit(value, chain);
    try {
        std::lock_guard lock(state_mutex);
        if (value == session) {
            auto it = std::find_if(chains.begin(), chains.end(), [&](const KnownChain& known) { return known.chain == chain; });
            if (it == chains.end() && chains.size() < 16) it = chains.insert(chains.end(), KnownChain{chain});
            if (it != chains.end()) it->committed = ovr::succeeded(result) ? index : -1;
        }
    } catch (...) {}
    {
        std::lock_guard lock(calibration_mutex);
        calibration.after_release(reinterpret_cast<std::uint64_t>(chain), static_cast<std::uint32_t>(index),
            ovr::succeeded(result) && index >= 0);
    }
    --commit_depth;
    return result;
}
template <ovr::EndFrameFn* Original>
ovr::Result frame_hook(ovr::Session value, long long index, const void* scale, const void* const* layers, unsigned count) {
    // A runtime may implement one submission export with another.
    if (frame_depth++ || stopping.load()) {
        const auto result = (*Original)(value, index, scale, layers, count);
        --frame_depth;
        return result;
    }
    const auto projection = select_projection(value, layers, count);
    const auto result = (*Original)(value, index, scale, layers, count);
    try {
        publish_frame(value, projection, result);
    } catch (...) {
        log_warning("LibOVR frame observation failed; foveation keeps its fixed fallback");
    }
    eye_calibration_tick();
    --frame_depth;
    return result;
}

void note_hmd(std::atomic<pvr::HmdHandle>& slot, pvr::HmdHandle hmd, unsigned route) noexcept {
    if (!hmd || slot.load(std::memory_order_relaxed) == hmd) return;
    slot.store(hmd);
    unsigned none{};
    capture_route.compare_exchange_strong(none, route);
    trace_event("Pimax PVR game session found hmd=%p route=%s", hmd, route == 1 ? "table" : "implementation");
}
// Null forgets every session: the LibOVR session that owned it has ended.
void forget_hmd(pvr::HmdHandle hmd) noexcept {
    AcquireSRWLockExclusive(&hmd_lock);
    for (auto* slot : {&frame_hmd, &tracking_hmd}) {
        pvr::HmdHandle expected = hmd;
        if (!hmd) slot->store(nullptr);
        else slot->compare_exchange_strong(expected, nullptr);
    }
    ReleaseSRWLockExclusive(&hmd_lock);
}
// Interface-table detours (route 1).
pvr::Result pvr_end_frame_hook(pvr::HmdHandle hmd, long long index, const void* const* layers, unsigned count) {
    note_hmd(frame_hmd, hmd, 1);
    return pvr_end_frame(hmd, index, layers, count);
}
pvr::Result pvr_submit_frame_hook(pvr::HmdHandle hmd, long long index, const void* const* layers, unsigned count) {
    note_hmd(frame_hmd, hmd, 1);
    return pvr_submit_frame(hmd, index, layers, count);
}
pvr::Result pvr_tracking_state_hook(pvr::HmdHandle hmd, double time, void* state) {
    note_hmd(tracking_hmd, hmd, 1);
    return pvr_tracking_state(hmd, time, state);
}
void pvr_destroy_hmd_hook(pvr::HmdHandle hmd) {
    forget_hmd(hmd);
    pvr_destroy_hmd(hmd);
}
// Implementation detours (route 2), for a runtime that copied the table.
pvr::FrameFn inline_end_frame{}, inline_submit_frame{};
pvr::GetTrackingStateFn inline_tracking_state{};
pvr::DestroyHmdFn inline_destroy_hmd{};
pvr::Result pvr_inline_end_frame_hook(pvr::HmdHandle hmd, long long index, const void* const* layers, unsigned count) {
    note_hmd(frame_hmd, hmd, 2);
    return inline_end_frame(hmd, index, layers, count);
}
pvr::Result pvr_inline_submit_frame_hook(pvr::HmdHandle hmd, long long index, const void* const* layers, unsigned count) {
    note_hmd(frame_hmd, hmd, 2);
    return inline_submit_frame(hmd, index, layers, count);
}
pvr::Result pvr_inline_tracking_state_hook(pvr::HmdHandle hmd, double time, void* state) {
    note_hmd(tracking_hmd, hmd, 2);
    return inline_tracking_state(hmd, time, state);
}
void pvr_inline_destroy_hmd_hook(pvr::HmdHandle hmd) {
    forget_hmd(hmd);
    inline_destroy_hmd(hmd);
}

PvrSample sample_pvr() noexcept {
    std::lock_guard lock(sample_mutex);
    auto& cache = sample_cache;
    PvrSample result;
    if (!pvr_ready.load()) return result;
    result.attached = true;
    const auto now_qpc = qpc();
    AcquireSRWLockShared(&hmd_lock);
    auto hmd = frame_hmd.load();
    if (!hmd) hmd = tracking_hmd.load();
    if (!hmd) {
        ReleaseSRWLockShared(&hmd_lock);
        cache = {};
        return result;
    }
    if (hmd != cache.hmd) {
        cache = {};
        cache.hmd = hmd;
        cache.sample = result;
    } else if (cache.qpc && now_qpc - cache.qpc < qpc_frequency() / 1000) {
        // Several DLSS evaluations in one frame share one tracker query.
        ReleaseSRWLockShared(&hmd_lock);
        return cache.sample;
    }
    pvr::EyeTrackingInfo info{};
    const bool queried = pvr_eye_tracking(hmd, pvr_time(), &info) == pvr::success;
    ReleaseSRWLockShared(&hmd_lock);
    auto& sample = cache.sample;
    const auto now = GetTickCount64();
    sample.session = true;
    sample.valid = queried && pvr_combined_gaze_ray(info, sample.ray);
    if (sample.valid && info.TimeInSeconds != cache.last_time) {
        cache.last_time = info.TimeInSeconds;
        cache.last_change_ms = now;
    }
    sample.age_ms = cache.last_change_ms ? now - cache.last_change_ms : 0;
    if (sample.valid && sample.age_ms > pvr_stall_ms) sample.valid = false;
    if (sample.valid) {
        ++sample.valid_samples;
        cache.ever_valid = true;
        sample.tan = {(info.GazeTan[0].x + info.GazeTan[1].x) * .5F, (info.GazeTan[0].y + info.GazeTan[1].y) * .5F};
        sample.time = info.TimeInSeconds;
    }
    sample.supported = cache.ever_valid;
    cache.qpc = now_qpc;
    return sample;
}

HMODULE find_runtime_module() noexcept {
    std::array<HMODULE, 2048> modules{};
    DWORD bytes{};
    if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), sizeof(modules), &bytes)) return nullptr;
    const auto count = (std::min)(modules.size(), static_cast<std::size_t>(bytes / sizeof(HMODULE)));
    // Most games never load LibOVR. With HookRescanOnModuleChange, skip the
    // export probes until the module list differs from the last one searched.
    std::uint64_t signature = 14695981039346656037ULL ^ count;
    for (std::size_t i = 0; i < count; ++i)
        signature = (signature ^ reinterpret_cast<std::uintptr_t>(modules[i])) * 1099511628211ULL;
    static std::uint64_t searched_signature{};
    if (configured_settings().hook_rescan_on_module_change && signature == searched_signature) return nullptr;
    for (std::size_t i = 0; i < count; ++i) {
        const auto module = modules[i];
        if (GetProcAddress(module, "ovr_CommitTextureSwapChain") && GetProcAddress(module, "ovr_GetTextureSwapChainBufferDX") &&
            GetProcAddress(module, "ovr_GetTextureSwapChainCurrentIndex") &&
            (GetProcAddress(module, "ovr_EndFrame") || GetProcAddress(module, "ovr_SubmitFrame2") ||
             GetProcAddress(module, "ovr_SubmitFrame")))
            return module;
    }
    searched_signature = signature;
    return nullptr;
}

bool install(void* target, void* detour, void** original) noexcept {
    if (!target) return false;
    if (std::find(inline_hooks.begin(), inline_hooks.end(), target) != inline_hooks.end()) return true;
    if (MH_CreateHook(target, detour, original) != MH_OK) return false;
    if (MH_EnableHook(target) != MH_OK) { MH_RemoveHook(target); return false; }
    inline_hooks.push_back(target);
    return true;
}

void arm_runtime(HMODULE module) {
    auto proc = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(module, name)); };
    HMODULE retained{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(proc("ovr_CommitTextureSwapChain")), &retained))
        return;
    // Keep hooked code mapped even if the game's loader unloads the runtime.
    retained_modules.push_back(retained);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* base = wcsrchr(path, L'\\');
    WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : path, -1, module_name, sizeof(module_name) - 1, nullptr, nullptr);
    get_current_index = reinterpret_cast<ovr::GetCurrentIndexFn>(proc("ovr_GetTextureSwapChainCurrentIndex"));
    get_buffer_dx = reinterpret_cast<ovr::GetBufferDXFn>(proc("ovr_GetTextureSwapChainBufferDX"));
    const bool commit = install(proc("ovr_CommitTextureSwapChain"), reinterpret_cast<void*>(&commit_hook),
        reinterpret_cast<void**>(&original_commit));
    bool frames{};
    frames |= install(proc("ovr_EndFrame"), reinterpret_cast<void*>(&frame_hook<&original_end_frame>),
        reinterpret_cast<void**>(&original_end_frame));
    frames |= install(proc("ovr_SubmitFrame2"), reinterpret_cast<void*>(&frame_hook<&original_submit_frame2>),
        reinterpret_cast<void**>(&original_submit_frame2));
    frames |= install(proc("ovr_SubmitFrame"), reinterpret_cast<void*>(&frame_hook<&original_submit_frame>),
        reinterpret_cast<void**>(&original_submit_frame));
    if (!commit || !frames) {
        for (auto* target : inline_hooks) { MH_DisableHook(target); MH_RemoveHook(target); }
        inline_hooks.clear();
        arm_failed = true;
        log_warning("LibOVR runtime found, but frame submission hooks could not be installed");
        return;
    }
    install(proc("ovr_Initialize"), reinterpret_cast<void*>(&initialize_hook), reinterpret_cast<void**>(&original_initialize));
    install(proc("ovr_Shutdown"), reinterpret_cast<void*>(&shutdown_hook), reinterpret_cast<void**>(&original_shutdown));
    install(proc("ovr_Create"), reinterpret_cast<void*>(&create_hook), reinterpret_cast<void**>(&original_create));
    install(proc("ovr_Destroy"), reinterpret_cast<void*>(&destroy_hook), reinterpret_cast<void**>(&original_destroy));
    install(proc("ovr_DestroyTextureSwapChain"), reinterpret_cast<void*>(&destroy_chain_hook),
        reinterpret_cast<void**>(&original_destroy_chain));
    armed.store(true);
    char message[160]{};
    sprintf_s(message, "LibOVR runtime observed (%s); frame submission hooks armed", module_name);
    log_info(message);
}

void attach_pvr() {
    auto& client = pvr_client;
    HMODULE module{};
    // Use only a client the game's runtime already loaded; never start PVR.
    if (!GetModuleHandleExW(0, L"LibPVRClient64.dll", &module)) return;
    const auto fail = [&](const char* reason) {
        FreeLibrary(module);
        client.attach_failed = true;
        char message[192]{};
        sprintf_s(message, "Pimax PVR eye tracking unavailable: %s", reason);
        log_warning(message);
    };
    const auto get = reinterpret_cast<pvr::GetInterfaceFn>(GetProcAddress(module, "getPvrInterface"));
    if (!get) return fail("getPvrInterface export missing");
    pvr::Interface* table{};
    for (auto minor = pvr::newest_minor_version; minor >= pvr::oldest_minor_version && !table; --minor)
        if ((table = get(pvr::major_version, minor))) client.minor = minor;
    if (!table) return fail("no supported interface version");
    const auto slot = [](const pvr::Interface* value, pvr::Slot index) { return value->slots[index]; };
    for (const auto index : {pvr::slot_destroy_hmd, pvr::slot_get_time_seconds, pvr::slot_get_tracking_state,
             pvr::slot_end_frame, pvr::slot_submit_frame, pvr::slot_get_eye_tracking_info})
        if (!slot(table, index)) return fail("incomplete interface");
    pvr_time = reinterpret_cast<pvr::GetTimeSecondsFn>(slot(table, pvr::slot_get_time_seconds));
    pvr_eye_tracking = reinterpret_cast<pvr::GetEyeTrackingInfoFn>(slot(table, pvr::slot_get_eye_tracking_info));
    pvr_end_frame = reinterpret_cast<pvr::FrameFn>(slot(table, pvr::slot_end_frame));
    pvr_submit_frame = reinterpret_cast<pvr::FrameFn>(slot(table, pvr::slot_submit_frame));
    pvr_tracking_state = reinterpret_cast<pvr::GetTrackingStateFn>(slot(table, pvr::slot_get_tracking_state));
    pvr_destroy_hmd = reinterpret_cast<pvr::DestroyHmdFn>(slot(table, pvr::slot_destroy_hmd));
    // The game's runtime may have requested another version. Patch each
    // table that forwards to the same implementations; skip any other.
    std::vector<pvr::Interface*> tables{table};
    for (auto minor = pvr::oldest_minor_version; minor < 64; ++minor) {
        auto* candidate = get(pvr::major_version, minor);
        if (!candidate || std::find(tables.begin(), tables.end(), candidate) != tables.end()) continue;
        bool same = true;
        for (const auto index : {pvr::slot_destroy_hmd, pvr::slot_get_tracking_state, pvr::slot_end_frame, pvr::slot_submit_frame})
            same = same && slot(candidate, index) == slot(table, index);
        if (same) tables.push_back(candidate);
        else trace_event("PVR interface 1.%u uses different implementations; not observed", minor);
    }
    if (get(pvr::major_version, client.minor) != table)
        trace_event("PVR interface tables are not stable; eye-tracking session capture may fail");
    std::copy(std::begin(table->slots), std::end(table->slots), client.original_slots.begin());
    const std::array<std::pair<pvr::Slot, void*>, 4> patches{{
        {pvr::slot_end_frame, reinterpret_cast<void*>(&pvr_end_frame_hook)},
        {pvr::slot_submit_frame, reinterpret_cast<void*>(&pvr_submit_frame_hook)},
        {pvr::slot_get_tracking_state, reinterpret_cast<void*>(&pvr_tracking_state_hook)},
        {pvr::slot_destroy_hmd, reinterpret_cast<void*>(&pvr_destroy_hmd_hook)},
    }};
    for (auto* value : tables) {
        for (const auto& [index, detour] : patches) {
            OpenVRVtableHook hook;
            void* original{};
            if (hook.install(&value->slots[index], detour, &original)) client.slots.push_back(hook);
        }
    }
    client.module = module;
    pvr_ready.store(true);
    char message[160]{};
    sprintf_s(message, "Pimax PVR client observed (interface 1.%u, %zu table(s)); waiting for the game's session",
        client.minor, tables.size());
    log_info(message);
}

// The runtime saw frames but never called through a patched table slot, so it
// holds its own copy. Detour the implementations instead, but only those whose
// address no other interface entry shares: a shared thunk has other ABIs.
void observe_pvr_implementations() {
    auto& client = pvr_client;
    client.implementations_observed = true;
    const auto unique = [&](pvr::Slot index) {
        const auto* target = client.original_slots[index];
        return target && std::count(client.original_slots.begin(), client.original_slots.end(), target) == 1;
    };
    struct Target { pvr::Slot slot; void* detour; void** original; };
    const Target targets[]{
        {pvr::slot_end_frame, reinterpret_cast<void*>(&pvr_inline_end_frame_hook), reinterpret_cast<void**>(&inline_end_frame)},
        {pvr::slot_submit_frame, reinterpret_cast<void*>(&pvr_inline_submit_frame_hook), reinterpret_cast<void**>(&inline_submit_frame)},
        {pvr::slot_get_tracking_state, reinterpret_cast<void*>(&pvr_inline_tracking_state_hook),
            reinterpret_cast<void**>(&inline_tracking_state)},
        {pvr::slot_destroy_hmd, reinterpret_cast<void*>(&pvr_inline_destroy_hmd_hook), reinterpret_cast<void**>(&inline_destroy_hmd)},
    };
    unsigned installed{};
    for (const auto& target : targets)
        if (unique(target.slot) && install(client.original_slots[target.slot], target.detour, target.original)) ++installed;
    char message[160]{};
    sprintf_s(message, "Pimax PVR interface table not used by the runtime; observing %u of 4 implementations", installed);
    log_info(message);
}
}  // namespace

void poll_libovr_hooks() noexcept {
    if (stopping.load()) return;
    std::lock_guard lock(hook_mutex);
    if (stopping.load()) return;
    try {
        if (!armed.load() && !arm_failed) {
            const auto now = GetTickCount64();
            if (now < next_scan) return;
            next_scan = now + 1000;
            if (const auto module = find_runtime_module()) arm_runtime(module);
        }
        // PVR is observed only for a LibOVR game on the Pimax runtime.
        if (armed.load() && !pvr_ready.load() && !pvr_client.attach_failed) attach_pvr();
        if (pvr_ready.load() && !pvr_client.implementations_observed && !capture_route.load() &&
            frames_without_session.load() >= implementation_fallback_frames)
            observe_pvr_implementations();
    } catch (...) {
        log_warning("LibOVR observation setup failed; waiting for the next scan");
    }
}

void stop_libovr_hooks() noexcept {
    stopping.store(true);
    {
        std::lock_guard lock(hook_mutex);
        for (auto it = pvr_client.slots.rbegin(); it != pvr_client.slots.rend(); ++it) it->restore();
        for (auto* target : inline_hooks) MH_DisableHook(target);
    }
    end_session(nullptr, true);
    // Trampolines are removed by the shared MinHook owner immediately after this.
    // Retain target modules through that teardown; process exit releases them.
}

bool read_libovr_gaze(const Settings& settings, IUnknown* resource, CheekyGazeSnapshotV1& output,
                      std::uint64_t native_identity) noexcept {
    if (!armed.load()) return false;
    std::array<LibOVRRawProjection, 2> raw{};
    float rotations[2][3][4]{};
    bool projection{}, simulated{};
    std::uint32_t simulation_pattern{};
    ULONGLONG simulation_origin{};
    ComPtr<IUnknown> identity;
    if (resource) resource->QueryInterface(IID_PPV_ARGS(&identity));
    const auto id = native_identity ? native_identity : reinterpret_cast<std::uint64_t>(identity.Get());
    {
        std::lock_guard lock(state_mutex);
        const bool next_simulate = settings.center_mode == FoveationCenterMode::simulated_gaze;
        if (next_simulate != simulate || pattern != settings.simulation_pattern) simulation_start = 0;
        simulate = next_simulate;
        pattern = settings.simulation_pattern;
        if (snapshot.abi_version != CHEEKY_GAZE_ABI_VERSION) return false;
        output = snapshot;
        if (snapshot.swapchain_generation != swapchain_generation)
            output.status_flags &= ~CHEEKY_GAZE_STATUS_MAPPING_READY;
        const auto now_qpc = qpc();
        if (now_qpc < output.publication_qpc ||
            static_cast<double>(now_qpc - output.publication_qpc) / static_cast<double>(qpc_frequency()) > 0.050)
            output.status_flags &= ~CHEEKY_GAZE_STATUS_SESSION_FOCUSED;
        projection = fovs_valid;
        for (unsigned eye = 0; eye < 2; ++eye) raw[eye] = libovr_raw_projection(fovs[eye]);
        std::memcpy(rotations, eye_to_head, sizeof(rotations));
        const auto now = GetTickCount64();
        // Select an observed member of the rotating swap chain for exact matching.
        for (unsigned eye = 0; eye < 2; ++eye) {
            for (const auto& entry : submitted[eye]) {
                if (!id || entry.view.resource_identity != id || now - entry.observed > submission_retention_ms) continue;
                auto& view = output.views[eye];
                view.resource_identity = entry.view.resource_identity;
                view.swapchain_identity = entry.view.swapchain_identity;
                view.image_rect_x = entry.view.image_rect_x; view.image_rect_y = entry.view.image_rect_y;
                view.image_rect_width = entry.view.image_rect_width; view.image_rect_height = entry.view.image_rect_height;
                view.flags |= CHEEKY_GAZE_VIEW_RESOURCE_VALID;
            }
        }
        simulated = simulate;
        simulation_pattern = pattern;
        if (simulate && !simulation_start) simulation_start = now;
        simulation_origin = simulation_start;
    }
    const auto sample = sample_pvr();
    float ray[3]{0, 0, -1}, next_ray[3]{};
    bool valid{}, next_valid{};
    if (simulated) {
        output.status_flags |= CHEEKY_GAZE_STATUS_SIMULATED | CHEEKY_GAZE_STATUS_SYSTEM_SUPPORTED;
        const double elapsed = static_cast<double>(GetTickCount64() - simulation_origin) / 1000.0;
        const auto direction = [](const gaze_math::Pose& pose, float* result) {
            const auto& q = pose.orientation;
            result[0] = -2 * (q.x * q.z + q.w * q.y);
            result[1] = -2 * (q.y * q.z - q.w * q.x);
            result[2] = -(1 - 2 * (q.x * q.x + q.y * q.y));
        };
        direction(gaze_math::simulated_gaze_pose({}, elapsed, simulation_pattern), ray);
        next_valid = simulation_pattern == 2 || simulation_pattern == 3;
        if (next_valid)
            direction(gaze_math::simulated_gaze_pose({}, gaze_math::next_simulated_jump_time(elapsed, simulation_pattern),
                simulation_pattern), next_ray);
        valid = gaze_math::simulated_gaze_valid(elapsed, simulation_pattern);
    } else {
        if (sample.supported) output.status_flags |= CHEEKY_GAZE_STATUS_SYSTEM_SUPPORTED;
        valid = sample.valid;
        std::copy(std::begin(sample.ray), std::end(sample.ray), ray);
    }
    valid = valid && projection;
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto& view = output.views[eye];
        const auto& p = raw[eye];
        const float forward[3]{0, 0, -1};
        if (projection && openvr_project_direction(rotations[eye], p.left, p.right, p.top, p.bottom, forward,
                view.forward_u, view.forward_v))
            view.flags |= CHEEKY_GAZE_VIEW_FORWARD_VALID;
        valid = valid && openvr_project_direction(rotations[eye], p.left, p.right, p.top, p.bottom, ray,
            view.center_u, view.center_v);
        if (valid && next_valid && openvr_project_direction(rotations[eye], p.left, p.right, p.top, p.bottom,
                next_ray, view.next_jump_u, view.next_jump_v))
            view.flags |= CHEEKY_GAZE_VIEW_NEXT_JUMP_VALID;
    }
    if (valid && (output.status_flags & CHEEKY_GAZE_STATUS_SESSION_FOCUSED)) {
        output.status_flags |= CHEEKY_GAZE_STATUS_GAZE_VALID | CHEEKY_GAZE_STATUS_ACTION_ACTIVE;
        for (auto& view : output.views) view.flags |= CHEEKY_GAZE_VIEW_ORIENTATION_VALID;
    }
    return true;
}

std::string libovr_gaze_json() {
    // Report the render path's latest tracker query; snapshots never call PVR.
    PvrSample sample;
    if (std::unique_lock lock(sample_mutex, std::try_to_lock); lock.owns_lock()) sample = sample_cache.sample;
    sample.attached = pvr_ready.load();
    sample.session = sample.attached && (frame_hmd.load() || tracking_hmd.load());
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::boolalpha;
    const auto minor = requested_minor.load();
    {
        std::lock_guard lock(state_mutex);
        out << "{\"hooked\":" << armed.load() << ",\"module\":\"" << module_name << "\",\"requested_minor\":";
        if (minor == 0) out << "null";
        else if (minor == UINT32_MAX) out << "\"current\"";
        else out << minor;
        out << ",\"layout\":\"" << (layout == LibOVRLayout::current ? "current" : layout == LibOVRLayout::legacy ? "legacy" : "unknown")
            << "\",\"session\":" << (session != nullptr) << ",\"frames\":" << frames_observed
            << ",\"projection_frames\":" << projection_frames << ",\"projection_layers\":" << projection_layers
            << ",\"submission_api\":" << submission_api << ",\"submitted_eye_rotation\":" << submitted_rotation;
    }
    out << ",\"calibration_pair_deferrals\":" << calibration_deferrals.load();
    out << ",\"pvr\":{\"client\":" << sample.attached << ",\"interface_minor\":" << (sample.attached ? pvr_client.minor : 0)
        << ",\"session_captured\":" << sample.session << ",\"session_route\":\""
        << (capture_route.load() == 1 ? "table" : capture_route.load() == 2 ? "implementation" : "none")
        << "\",\"gaze_valid\":" << sample.valid << ",\"supported\":" << sample.supported
        << ",\"valid_samples\":" << sample.valid_samples << ",\"sample_age_ms\":" << sample.age_ms
        << ",\"gaze_tan\":[" << sample.tan.x << ',' << sample.tan.y << "]}}";
    return out.str();
}
}  // namespace cheeky::foveated_dlss
