#include "afw_depth_coverage.hpp"
#include "afw_compatibility.hpp"
#include "dlss_nr_lifetime.hpp"
#include "eye_calibration_d3d12.hpp"
#include <Windows.h>
#include <wrl/client.h>
#include <mutex>
#include <cstring>
#include <vector>

namespace cheeky::foveated_dlss {
namespace {
using Microsoft::WRL::ComPtr;
struct Readback {
    NrLifetime lifetime;
    ComPtr<ID3D12Resource> depth, buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    AfwMatrix matrix{}, projection_only{};
    std::uint64_t captured{}, generation{}, bytes{};
    unsigned eye{};
    DXGI_FORMAT format{};
    bool pending{}, submitted{};
};
struct Sample { float margin{}; std::uint64_t captured{}, generation{}; bool valid{}; AfwDepthMarginPolicy policy{}; };
struct State {
    std::mutex mutex;
    std::array<Readback, 4> slots;
    std::array<Sample, 2> samples{};
    std::array<std::uint64_t, 2> next_capture{};
    std::uint64_t captures{}, completed{}, skipped{};
    unsigned format{}, initial_state{}, skip_reason{};
};
State& state() { static auto* value = new State; return *value; } // Recording references can outlive adapter unload.
constexpr std::uint64_t freshness_ms = 500;

void collect(State& s) {
    const auto now = GetTickCount64();
    const auto projection = afw_stereo_projection();
    for (auto& slot : s.slots) {
        if (!slot.pending) continue;
        slot.submitted |= slot.lifetime.has_submissions();
        slot.lifetime.collect();
        // Do not read or reuse a buffer until Reset/destruction and completion
        // of every queue execution. An unsubmitted Reset never produces data.
        if (!slot.lifetime.empty()) continue;
        slot.pending = false;
        if (slot.submitted && slot.generation == projection.generation && projection.valid &&
                now >= slot.captured && now - slot.captured <= freshness_ms) {
            auto& sample = s.samples[slot.eye];
            if (slot.captured >= sample.captured) {
                const auto old_policy = sample.valid && sample.generation == slot.generation ? sample.policy : AfwDepthMarginPolicy{};
                sample = {0.F, slot.captured, slot.generation, false};
                sample.policy = old_policy;
                ComPtr<ID3D12Device> device;
                const bool device_ok = SUCCEEDED(slot.buffer->GetDevice(IID_PPV_ARGS(&device))) && SUCCEEDED(device->GetDeviceRemovedReason());
                const D3D12_RANGE range{0, static_cast<SIZE_T>(slot.bytes)};
                void* mapped{};
                if (device_ok && SUCCEEDED(slot.buffer->Map(0, &range, &mapped))) {
                    const auto* data = static_cast<const unsigned char*>(mapped);
                    const auto w = slot.footprint.Footprint.Width, h = slot.footprint.Footprint.Height;
                    const auto step_x = (std::max)(1U, (w + 63U) / 64U), step_y = (std::max)(1U, (h + 63U) / 64U);
                    unsigned valid{}; bool malformed{};
                    float displacement{};
                    for (unsigned y = 0; y < h; y += step_y) for (unsigned x = 0; x < w; x += step_x) {
                        const float depth = afw_decode_depth(data + slot.footprint.Offset + static_cast<std::size_t>(y) * slot.footprint.Footprint.RowPitch +
                            x * afw_depth_sample_bytes(slot.format), slot.format);
                        if (!std::isfinite(depth) || depth < 0 || depth > 1) { malformed = true; continue; }
                        const float u = (x + .5F) / w, v = (y + .5F) / h;
                        float dx{}, dy{}, bx{}, by{};
                        if (afw_project_depth(slot.matrix, u, v, depth, dx, dy) &&
                                afw_project_depth(slot.projection_only, u, v, depth, bx, by, false)) {
                            // Stereo projection-center/FOV differences already
                            // belong to the fixed coverage envelope. Add only
                            // geometric displacement beyond that optical map.
                            displacement = (std::max)({displacement, std::abs(dx - bx), std::abs(dy - by)});
                            ++valid;
                        }
                    }
                    const D3D12_RANGE unwritten{0, 0}; slot.buffer->Unmap(0, &unwritten);
                    sample.valid = valid != 0 && !malformed;
                    // One sample-cell guard accompanies the estimated motion;
                    // user padding remains an additional independent minimum.
                    sample.margin = sample.policy.update((std::min)(1.F, displacement + (std::max)(static_cast<float>(step_x) / w, static_cast<float>(step_y) / h)), now);
                    ++s.completed;
                }
            }
        }
        slot.depth.Reset();
    }
}
void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, 0, from, to}; list->ResourceBarrier(1, &b); // Depth plane only; leave stencil untouched.
}
}
void poll_afw_depth_coverage() noexcept {
    try {
        std::lock_guard execution_lock(calibration12_execution_mutex());
        auto& s = state(); std::lock_guard lock(s.mutex); collect(s);
    } catch (...) {}
}
AfwDepthCoverageStatus afw_depth_coverage_status(unsigned eye) noexcept {
    auto& s = state(); std::lock_guard lock(s.mutex);
    const auto now = GetTickCount64(); const auto projection = afw_stereo_projection();
    AfwDepthCoverageStatus result;
    result.captures = s.captures; result.completed = s.completed; result.skipped = s.skipped;
    result.format = s.format; result.initial_state = s.initial_state; result.skip_reason = s.skip_reason;
    for (const auto& slot : s.slots) result.pending += slot.pending;
    for (unsigned i = 0; i < 2; ++i) {
        if (eye < 2 && i != eye) continue;
        const auto& sample = s.samples[i];
        if (!sample.valid || !projection.valid || sample.generation != projection.generation ||
                now < sample.captured || now - sample.captured > freshness_ms) continue;
        if (!result.valid || sample.margin > result.margin) {
            result.valid = true; result.margin = sample.margin; result.age_ms = now - sample.captured; result.source_eye = i;
        }
    }
    return result;
}
void capture_afw_depth_coverage(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, D3D12_RESOURCE_STATES initial,
    const AfwMatrix& matrix, unsigned eye, const AfwMatrix& projection_only) noexcept {
    if (!list || !depth || eye > 1) return;
    try {
        std::lock_guard execution_lock(calibration12_execution_mutex());
        auto& s = state(); std::lock_guard lock(s.mutex); collect(s);
        const auto now = GetTickCount64();
        if (now < s.next_capture[eye]) return;
        s.next_capture[eye] = now + 100; // At most ten copies/second/eye; no render-thread waits.
        const auto skip = [&](unsigned reason) { ++s.skipped; s.skip_reason = reason; };
        for (float value : matrix) if (!std::isfinite(value)) { skip(1); return; }
        for (float value : projection_only) if (!std::isfinite(value)) { skip(1); return; }
        const auto projection = afw_stereo_projection();
        const auto desc = depth->GetDesc();
        s.format = desc.Format; s.initial_state = initial;
        if (!projection.valid) { skip(2); return; }
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels != 1 ||
                desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1) { skip(3); return; }
        if (!afw_depth_sample_bytes(desc.Format)) { skip(4); return; }
        if (!(initial & D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) ||
                (initial & ~(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_DEPTH_READ))) { skip(5); return; }
        auto slot = std::find_if(s.slots.begin(), s.slots.end(), [](const auto& r) { return !r.pending; });
        if (slot == s.slots.end()) { skip(6); return; }
        ComPtr<ID3D12Device> device;
        if (FAILED(depth->GetDevice(IID_PPV_ARGS(&device)))) { skip(7); return; }
        UINT64 bytes{}; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        if (!bytes || bytes > 64ULL * 1024 * 1024 || footprint.Footprint.RowPitch < desc.Width * afw_depth_sample_bytes(desc.Format)) { skip(8); return; }
        ComPtr<ID3D12Device> previous_device;
        if (slot->buffer) slot->buffer->GetDevice(IID_PPV_ARGS(&previous_device));
        if (!slot->buffer || slot->bytes != bytes || previous_device.Get() != device.Get()) {
            slot->buffer.Reset();
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = bytes; buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot->buffer)))) { skip(9); return; }
        }
        if (!slot->lifetime.record(list)) { skip(10); return; }
        slot->depth = depth; slot->matrix = matrix; slot->eye = eye; slot->captured = now; slot->generation = projection.generation;
        slot->projection_only = projection_only;
        slot->footprint = footprint; slot->bytes = bytes; slot->pending = true; slot->submitted = false;
        slot->format = desc.Format; s.skip_reason = 0;
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = depth; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = slot->buffer.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint;
        barrier(list, depth, initial, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        barrier(list, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, initial);
        ++s.captures;
    } catch (...) {}
}
}
