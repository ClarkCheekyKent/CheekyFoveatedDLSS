#include "eye_calibration_d3d12.hpp"
#include "eye_calibration_pixels.hpp"
#include "graphics_observer.hpp"
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <vector>

namespace cheeky::foveated_dlss {
using Microsoft::WRL::ComPtr;
namespace {
constexpr unsigned marker_size = 20;
constexpr GUID list_key{0x4f637a10, 0x6e74, 0x4a72, {0x81, 0x9a, 0x39, 0xd5, 0x21, 0x36, 0x8b, 0xf1}};
std::atomic<std::uint64_t> next_list_id{1};
thread_local bool internal_work{};
struct Registry {
    std::mutex mutex;
    // Keep recordings alive even when the host stops or changes devices.
    std::vector<std::shared_ptr<Calibration12Frame>> frames;
};
Registry& registry() {
    static auto* r = new Registry;
    return *r;
}
void retire(std::uint64_t) noexcept;

class ListLifetime final : public IUnknown {
    std::atomic<ULONG> refs{1};

  public:
    const std::uint64_t id{next_list_id++};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown))
            return E_NOINTERFACE;
        *out = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto n = --refs;
        if (!n) {
            retire(id);
            delete this;
        }
        return n;
    }
};
std::uint64_t list_id(ID3D12GraphicsCommandList* list, bool create) {
    IUnknown* value{};
    UINT bytes = sizeof(value);
    if (SUCCEEDED(list->GetPrivateData(list_key, &bytes, &value)) && value) {
        const auto id = static_cast<ListLifetime*>(value)->id;
        value->Release();
        return id;
    }
    if (!create)
        return 0;
    auto* tag = new ListLifetime;
    const auto id = tag->id;
    const auto hr = list->SetPrivateDataInterface(list_key, tag);
    tag->Release();
    return SUCCEEDED(hr) ? id : 0;
}
struct Patch {
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes{};
    bool used{};
};
struct FencePoint {
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    UINT64 signal{};
    bool active{}, failed{};
};
struct Segment {
    std::vector<FencePoint> points;
    UINT64 frequency{};
    std::uint64_t recording{};
    bool used{}, submitted{}, retired{};
    bool untracked{};
};
bool buffer(ID3D12Device* device, D3D12_HEAP_TYPE heap_type, UINT64 size, ComPtr<ID3D12Resource>& out) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heap_type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     heap_type == D3D12_HEAP_TYPE_READBACK
                                                         ? D3D12_RESOURCE_STATE_COPY_DEST
                                                         : D3D12_RESOURCE_STATE_GENERIC_READ,
                                                     nullptr, IID_PPV_ARGS(&out)));
}
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, UINT subresource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, subresource, before, after};
    list->ResourceBarrier(1, &barrier);
}
bool texture_supported(const D3D12_RESOURCE_DESC& d, unsigned slice) {
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width <= UINT32_MAX && d.MipLevels &&
           d.DepthOrArraySize > slice && d.SampleDesc.Count == 1 && calibration_pixel_bytes(d.Format);
}
} // namespace

struct Calibration12Frame {
    std::mutex mutex;
    ComPtr<ID3D12Device> device;
    std::array<Patch, 8> patches;
    std::array<Segment, 4> segments;
    std::array<ComPtr<ID3D12Resource>, 2> markers;
    std::array<DXGI_FORMAT, 2> marker_formats{};
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> marker_footprints{};
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> timestamps;
    std::array<ComPtr<ID3D12CommandAllocator>, 2> allocators;
    std::array<ComPtr<ID3D12GraphicsCommandList>, 2> lists;
    bool invalid{};
    Calibration12Failure failure{};
    std::uint64_t asynchronous_allocations{};
};
namespace {
bool completed(const Segment& s) {
    if (s.untracked)
        return false;
    if (!s.used)
        return true;
    if (!s.submitted)
        return s.retired;
    for (const auto& p : s.points)
        if (p.active && (p.failed || p.fence->GetCompletedValue() < p.signal))
            return false;
    return true;
}
bool reusable(const Calibration12Frame& f) {
    if (FAILED(f.device->GetDeviceRemovedReason()))
        return true;
    for (const auto& s : f.segments)
        if (!completed(s) || (s.used && !s.retired))
            return false;
    return true;
}
void retire(std::uint64_t id) noexcept {
    if (!id || internal_work)
        return;
    std::lock_guard execution_lock(calibration12_execution_mutex());
    auto& r = registry();
    std::lock_guard registry_lock(r.mutex);
    for (const auto& f : r.frames) {
        std::lock_guard lock(f->mutex);
        for (auto& s : f->segments)
            if (s.used && s.recording == id) {
                s.retired = true;
                if (!s.submitted) {
                    f->invalid = true;
                    f->failure = {"recording_retired_without_observed_submit"};
                }
            }
    }
}
bool initialize(Calibration12Frame& f, std::uint64_t& allocations) {
    if (!f.queries) {
        D3D12_QUERY_HEAP_DESC desc{};
        desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        desc.Count = 8;
        if (FAILED(f.device->CreateQueryHeap(&desc, IID_PPV_ARGS(&f.queries))))
            return false;
        ++allocations;
    }
    if (!f.timestamps) {
        if (!buffer(f.device.Get(), D3D12_HEAP_TYPE_READBACK, sizeof(UINT64) * 8, f.timestamps))
            return false;
        ++allocations;
    }
    return true;
}
bool signal(Calibration12Frame& f, Segment& s, ID3D12CommandQueue* queue) noexcept {
    // A list may be resubmitted on another queue. Each queue needs its own
    // fence timeline; a higher signal from one queue cannot retire another.
    const bool previously_untracked = s.untracked;
    s.untracked = true;
    try {
        FencePoint* point{};
        for (auto& p : s.points)
            if (p.queue.Get() == queue) {
                point = &p;
                break;
            }
        if (!point) {
            FencePoint p;
            p.queue = queue;
            if (FAILED(f.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p.fence))))
                return false;
            s.points.push_back(std::move(p));
            point = &s.points.back();
            ++f.asynchronous_allocations;
        }
        point->active = true;
        ++point->signal;
        point->failed = FAILED(queue->Signal(point->fence.Get(), point->signal));
        s.submitted = true;
        queue->GetTimestampFrequency(&s.frequency);
        s.untracked = previously_untracked || point->failed;
        return !point->failed;
    } catch (...) {
        return false;
    }
}
bool prepare_patch(Calibration12Frame& f, unsigned index, DXGI_FORMAT format, const D3D12_BOX& box,
                   std::uint64_t& allocations) {
    if (box.left >= box.right || box.top >= box.bottom || box.front || box.back != 1 ||
        box.right - box.left > 256 || box.bottom - box.top > 256)
        return false;
    auto& p = f.patches[index];
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = box.right - box.left;
    d.Height = box.bottom - box.top;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes{};
    f.device->GetCopyableFootprints(&d, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    if (!p.buffer || p.bytes < bytes) {
        p.buffer.Reset();
        if (!buffer(f.device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, p.buffer))
            return false;
        ++allocations;
        p.bytes = bytes;
    }
    p.footprint = footprint;
    return true;
}
void copy_patch(Calibration12Frame& f, ID3D12GraphicsCommandList* list, unsigned index,
                ID3D12Resource* texture, UINT subresource, const D3D12_BOX& box) {
    auto& p = f.patches[index];
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = p.buffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = p.footprint;
    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = subresource;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    p.used = true;
}
void begin_segment(Calibration12Frame& f, ID3D12GraphicsCommandList* list, unsigned segment) {
    list->EndQuery(f.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, segment * 2);
    f.segments[segment].used = true;
}
void end_segment(Calibration12Frame& f, ID3D12GraphicsCommandList* list, unsigned segment) {
    list->EndQuery(f.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, segment * 2 + 1);
    list->ResolveQueryData(f.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, segment * 2, 2, f.timestamps.Get(),
                           sizeof(UINT64) * segment * 2);
}
} // namespace

std::shared_ptr<Calibration12Frame> calibration12_create(ID3D12Device* device) {
    auto f = std::make_shared<Calibration12Frame>();
    f->device = device;
    std::lock_guard execution_lock(calibration12_execution_mutex());
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    std::erase_if(r.frames, [](const auto& item) {
        if (item.use_count() != 1)
            return false;
        std::lock_guard frame_lock(item->mutex);
        return reusable(*item);
    });
    r.frames.push_back(f);
    return f;
}
bool calibration12_begin(Calibration12Frame& f) noexcept {
    std::lock_guard execution_lock(calibration12_execution_mutex());
    std::lock_guard lock(f.mutex);
    if (!reusable(f))
        return false;
    f.invalid = false;
    f.failure = {};
    for (auto& p : f.patches)
        p.used = false;
    for (auto& s : f.segments) {
        s.used = s.submitted = s.retired = s.untracked = false;
        s.recording = 0;
        s.frequency = 0;
        for (auto& p : s.points)
            p.active = p.failed = false;
    }
    return true;
}
bool calibration12_stamp(Calibration12Frame& f, ID3D12GraphicsCommandList* list, ID3D12Resource* texture,
                         unsigned candidate, unsigned x, unsigned y, D3D12_RESOURCE_STATES state,
                         std::uint64_t& allocations, Calibration12Failure* failure) noexcept {
    if (failure) *failure = {};
    const auto reject = [&](const char* stage, HRESULT hr = S_OK) {
        if (failure) *failure = {stage, hr};
        return false;
    };
    try {
        if (!list || !texture || candidate > 1 || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            return reject("stamp_arguments");
        ComPtr<ID3D12Device> list_device, texture_device;
        auto hr = list->GetDevice(IID_PPV_ARGS(&list_device));
        if (FAILED(hr)) return reject("stamp_list_device", hr);
        hr = texture->GetDevice(IID_PPV_ARGS(&texture_device));
        if (FAILED(hr)) return reject("stamp_texture_device", hr);
        if (list_device.Get() != f.device.Get() || texture_device.Get() != f.device.Get())
            return reject("stamp_device_mismatch");
        // Install post-Execute observation before recording any marker commands.
        if (!native_observer_status().ready) {
            D3D12_COMMAND_QUEUE_DESC q{};
            q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ComPtr<ID3D12CommandQueue> queue;
            hr = f.device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
            if (FAILED(hr)) return reject("observer_queue", hr);
            if (!initialize_native_observer(f.device.Get(), queue.Get()))
                return reject("observer_install");
        }
        const auto recording = list_id(list, true);
        if (!recording)
            return reject("recording_identity");
        std::lock_guard lock(f.mutex);
        const auto d = texture->GetDesc();
        if (!texture_supported(d, 0)) return reject("stamp_texture_format_or_layout");
        if (UINT64(x) + marker_size > d.Width || UINT64(y) + marker_size > d.Height)
            return reject("stamp_bounds");
        if (!initialize(f, allocations)) return reject("stamp_query_buffers");
        const D3D12_BOX box{x, y, 0, x + marker_size, y + marker_size, 1};
        if (!prepare_patch(f, candidate * 2, d.Format, box, allocations) ||
            !prepare_patch(f, candidate * 2 + 1, d.Format, box, allocations))
            return reject("stamp_readback_buffers");
        if (f.marker_formats[candidate] != d.Format) {
            f.markers[candidate].Reset();
            f.marker_formats[candidate] = d.Format;
        }
        auto& footprint = f.marker_footprints[candidate];
        if (!f.markers[candidate]) {
            auto marker_desc = d;
            marker_desc.Width = marker_desc.Height = marker_size;
            marker_desc.DepthOrArraySize = marker_desc.MipLevels = 1;
            UINT64 bytes{};
            f.device->GetCopyableFootprints(&marker_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
            if (!buffer(f.device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, f.markers[candidate]))
                return reject("marker_upload_buffer");
            ++allocations;
            void* data{};
            const D3D12_RANGE empty{0, 0};
            hr = f.markers[candidate]->Map(0, &empty, &data);
            if (FAILED(hr)) {
                f.markers[candidate].Reset();
                return reject("marker_upload_map", hr);
            }
            const auto bpp = calibration_pixel_bytes(d.Format);
            for (unsigned yy = 0; yy < marker_size; ++yy)
                for (unsigned xx = 0; xx < marker_size; ++xx)
                    calibration_encode_marker(static_cast<unsigned char*>(data) +
                                                  yy * footprint.Footprint.RowPitch + xx * bpp,
                                              d.Format, candidate);
            f.markers[candidate]->Unmap(0, nullptr);
        }
        begin_segment(f, list, candidate);
        f.segments[candidate].recording = recording;
        transition(list, texture, 0, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        copy_patch(f, list, candidate * 2, texture, 0, box);
        transition(list, texture, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = f.markers[candidate].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint;
        list->CopyTextureRegion(&dst, x, y, 0, &src, nullptr);
        transition(list, texture, 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        copy_patch(f, list, candidate * 2 + 1, texture, 0, box);
        transition(list, texture, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        end_segment(f, list, candidate);
        return true;
    } catch (...) {
        return reject("stamp_exception", E_FAIL);
    }
}
bool calibration12_capture(Calibration12Frame& f, ID3D12CommandQueue* queue, ID3D12Resource* texture,
                           unsigned eye, unsigned slice, D3D12_RESOURCE_STATES state,
                           const std::array<D3D12_BOX, 2>& boxes, std::uint64_t& allocations,
                           Calibration12Failure* failure) noexcept {
    if (failure) *failure = {};
    const auto reject = [&](const char* stage, HRESULT hr = S_OK) {
        if (failure) *failure = {stage, hr};
        return false;
    };
    if (!queue || !texture || eye > 1 || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return reject("capture_arguments");
    std::lock_guard lock(f.mutex);
    const auto d = texture->GetDesc();
    if (!texture_supported(d, slice)) return reject("capture_texture_format_or_layout");
    if (!initialize(f, allocations)) return reject("capture_query_buffers");
    ComPtr<ID3D12Device> device;
    auto hr = queue->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(hr)) return reject("capture_queue_device", hr);
    if (device.Get() != f.device.Get()) return reject("capture_queue_device_mismatch");
    device.Reset();
    hr = texture->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(hr)) return reject("capture_texture_device", hr);
    if (device.Get() != f.device.Get()) return reject("capture_texture_device_mismatch");
    for (unsigned c = 0; c < 2; ++c) {
        if (boxes[c].right > d.Width || boxes[c].bottom > d.Height)
            return reject("capture_bounds");
        if (!prepare_patch(f, 4 + eye * 2 + c, d.Format, boxes[c], allocations))
            return reject("capture_readback_buffers");
    }
    auto& allocator = f.allocators[eye];
    auto& list = f.lists[eye];
    if (!allocator) {
        hr = f.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (FAILED(hr)) return reject("capture_allocator_create", hr);
        ++allocations;
    }
    hr = allocator->Reset();
    if (FAILED(hr)) return reject("capture_allocator_reset", hr);
    // Own lists are not game recordings; suppress observer callbacks while
    // submitting them under this frame lock.
    struct Scope {
        Scope() {
            internal_work = true;
        }
        ~Scope() {
            internal_work = false;
        }
    } scope;
    if (!list) {
        hr = f.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list));
        if (FAILED(hr)) return reject("capture_list_create", hr);
        ++allocations;
    } else {
        hr = list->Reset(allocator.Get(), nullptr);
        if (FAILED(hr)) return reject("capture_list_reset", hr);
    }
    const auto subresource = slice * d.MipLevels;
    begin_segment(f, list.Get(), 2 + eye);
    transition(list.Get(), texture, subresource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (unsigned c = 0; c < 2; ++c)
        copy_patch(f, list.Get(), 4 + eye * 2 + c, texture, subresource, boxes[c]);
    transition(list.Get(), texture, subresource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    end_segment(f, list.Get(), 2 + eye);
    hr = list->Close();
    if (FAILED(hr)) {
        f.invalid = true;
        f.segments[2 + eye].retired = true;
        return reject("capture_list_close", hr);
    }
    auto& segment = f.segments[2 + eye];
    segment.retired = true;
    ID3D12CommandList* lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (!signal(f, segment, queue)) {
        f.invalid = true;
        return reject("capture_fence_signal");
    }
    return true;
}
void calibration12_submitted(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list) noexcept {
    if (internal_work || !queue || !list)
        return;
    std::lock_guard execution_lock(calibration12_execution_mutex());
    const auto id = list_id(list, false);
    if (!id)
        return;
    auto& r = registry();
    std::lock_guard registry_lock(r.mutex);
    for (const auto& f : r.frames) {
        std::lock_guard lock(f->mutex);
        for (auto& s : f->segments)
            if (s.used && !s.retired && s.recording == id) {
                if (!signal(*f, s, queue)) {
                    f->invalid = true;
                    f->failure = {"source_fence_signal"};
                }
            }
    }
}
void calibration12_retired(ID3D12GraphicsCommandList* list) noexcept {
    if (list && !internal_work)
        retire(list_id(list, false));
}
bool calibration12_internal_work() noexcept {
    return internal_work;
}
std::recursive_mutex& calibration12_execution_mutex() noexcept {
    static auto* mutex = new std::recursive_mutex;
    return *mutex;
}
Calibration12Readback calibration12_poll(Calibration12Frame& f) noexcept {
    std::lock_guard execution_lock(calibration12_execution_mutex());
    std::lock_guard lock(f.mutex);
    Calibration12Readback out;
    out.allocations = f.asynchronous_allocations;
    f.asynchronous_allocations = 0;
    const auto device_status = f.device->GetDeviceRemovedReason();
    if (FAILED(device_status)) {
        out.ready = out.reusable = true;
        out.failure = {"device_removed", device_status};
        return out;
    }
    out.reusable = reusable(f);
    // Until Reset/destruction, the game may resubmit this recording and write
    // these buffers again. Do not map them merely because its first use ended.
    if (!out.reusable)
        return out;
    for (const auto& s : f.segments)
        if (!completed(s))
            return out;
    out.ready = true;
    out.valid = !f.invalid && SUCCEEDED(f.device->GetDeviceRemovedReason());
    out.failure = f.failure;
    for (unsigned i = 0; i < f.patches.size(); ++i) {
        auto& p = f.patches[i];
        if (!p.used) {
            out.valid = false;
            if (!strcmp(out.failure.stage, "none")) out.failure = {"readback_patch_not_recorded"};
            continue;
        }
        const auto& d = p.footprint.Footprint;
        void* data{};
        const D3D12_RANGE range{0, static_cast<SIZE_T>(p.bytes)};
        const auto hr = p.buffer->Map(0, &range, &data);
        if (FAILED(hr)) {
            out.valid = false;
            out.failure = {"readback_map", hr};
            continue;
        }
        unsigned count{};
        float score{};
        const auto bpp = calibration_pixel_bytes(d.Format);
        const auto candidate = i < 4 ? i / 2 : (i - 4) % 2;
        for (unsigned y = d.Height / 5; y < d.Height - d.Height / 5; ++y)
            for (unsigned x = d.Width / 5; x < d.Width - d.Width / 5; ++x) {
                score += calibration_similarity(
                    calibration_decode(static_cast<const unsigned char*>(data) + y * d.RowPitch + x * bpp,
                                       d.Format),
                    candidate);
                ++count;
            }
        const D3D12_RANGE empty{0, 0};
        p.buffer->Unmap(0, &empty);
        out.scores[i] = count ? score / count : 0;
    }
    out.timing_valid = true;
    for (const auto& s : f.segments)
        if (!s.used || !s.submitted || !s.frequency)
            out.timing_valid = false;
    for (const auto& s : f.segments)
        if (std::count_if(s.points.begin(), s.points.end(), [](const auto& p) { return p.active; }) > 1)
            out.timing_valid = false;
    if (out.timing_valid) {
        void* data{};
        const D3D12_RANGE range{0, sizeof(UINT64) * 8};
        if (FAILED(f.timestamps->Map(0, &range, &data)))
            out.timing_valid = false;
        else {
            const auto* times = static_cast<const UINT64*>(data);
            for (unsigned i = 0; i < 4; ++i) {
                if (times[i * 2 + 1] < times[i * 2])
                    out.timing_valid = false;
                else
                    out.gpu_us +=
                        double(times[i * 2 + 1] - times[i * 2]) * 1e6 / double(f.segments[i].frequency);
            }
            const D3D12_RANGE empty{0, 0};
            f.timestamps->Unmap(0, &empty);
        }
    }
    return out;
}
} // namespace cheeky::foveated_dlss
