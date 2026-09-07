#include "dlss_nr_input.hpp"
#include "d3d12_submission_identity.hpp"
#include "runtime.hpp"
#include <atomic>
#include <deque>
#include <mutex>

namespace cheeky::foveated_dlss {
namespace {
struct Input {
    DlssViewId view{};
    ID3D12Resource* color{};
    ID3D12Device* device{}; // Kept alive by color.
    ID3D12Fence* fence{};
    ID3D12GraphicsCommandList* recording{};
    std::uint64_t recording_identity{};
    ID3D12CommandQueue* queue{};
    std::uint64_t value{};
    bool retired{};
    D3D12_RESOURCE_STATES state{};
};
struct History {
    DlssViewId view{};
    NrProcessingOrder order{};
    bool processed{};
    std::uint32_t width{}, height{};
};
std::mutex mutex;
std::deque<Input> inputs;
std::deque<History> histories;
bool complete(const Input& input) noexcept {
    return input.recording == nullptr && input.queue == nullptr && input.fence->GetCompletedValue() >= input.value;
}
void collect() noexcept {
    for (auto it = inputs.begin(); it != inputs.end();) {
        if (it->retired && complete(*it)) {
            it->color->Release();
            it->fence->Release();
            it = inputs.erase(it);
        } else ++it;
    }
}
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, 0U, before, after};
    list->ResourceBarrier(1U, &barrier);
}
}
ID3D12Resource* prepare_dlss_nr_input(DlssNrFrame frame, const Settings& settings) noexcept {
    if (!settings.nr_enabled || settings.nr_processing_order != NrProcessingOrder::before_upscaling)
        return nullptr;
    const auto resolution = dlss_nr_processing_resolution(settings.nr_processing_order,
        frame.input_width, frame.input_height, frame.output_width, frame.output_height);
    frame.processing_width = resolution.width;
    frame.processing_height = resolution.height;
    frame.reset = frame.reset || !dlss_nr_input_history_compatible(frame.view_id, settings.nr_processing_order,
        true, frame.input_width, frame.input_height);
    if (!frame.color || !frame.command_list) {
        static_cast<void>(evaluate_dlss_nr(frame, settings));
        return nullptr;
    }
    auto desc = frame.color->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1U ||
        desc.SampleDesc.Count != 1U || frame.color_base_x > desc.Width ||
        frame.input_width > desc.Width - frame.color_base_x || frame.color_base_y > desc.Height ||
        frame.input_height > desc.Height - frame.color_base_y || !frame.input_width || !frame.input_height) {
        frame.color = nullptr;
        static_cast<void>(evaluate_dlss_nr(frame, settings));
        return nullptr;
    }
    ID3D12Device* frame_device{};
    if (FAILED(frame.command_list->GetDevice(IID_PPV_ARGS(&frame_device)))) {
        frame.color = nullptr;
        static_cast<void>(evaluate_dlss_nr(frame, settings));
        return nullptr;
    }
    ID3D12Resource* private_color{};
    {
        std::lock_guard lock(mutex);
        collect();
        Input* selected{};
        std::size_t count{};
        for (auto& input : inputs) {
            if (input.view != frame.view_id || input.retired) continue;
            ++count;
            const auto existing = input.color->GetDesc();
            if (input.device != frame_device || existing.Width != desc.Width || existing.Height != desc.Height || existing.Format != desc.Format || input.state != frame.color_state) {
                input.retired = true;
                continue;
            }
            if (complete(input)) { selected = &input; break; }
        }
        if (!selected && count < 8U) {
            ID3D12Device* device{};
            if (SUCCEEDED(frame.command_list->GetDevice(IID_PPV_ARGS(&device)))) {
                Input created{};
                created.view = frame.view_id;
                created.device = device;
                created.state = frame.color_state;
                desc.MipLevels = 1U;
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                        frame.color_state, nullptr, IID_PPV_ARGS(&created.color))) &&
                    SUCCEEDED(device->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&created.fence)))) {
                    inputs.push_back(created);
                    selected = &inputs.back();
                } else {
                    if (created.color) created.color->Release();
                    if (created.fence) created.fence->Release();
                }
                device->Release();
            }
        }
        if (selected) {
            if (selected->value == 1 || (selected->value && selected->value % 300 == 0))
                trace_event("DLSS-NR Before input recycled view=%llu completed=%llu", frame.view_id, selected->value);
            selected->recording = frame.command_list;
            selected->recording_identity = d3d12_submission_identity(frame.command_list, true);
            frame.command_list->AddRef();
            private_color = selected->color;
        }
    }
    frame_device->Release();
    if (!private_color) {
        static std::atomic<std::uint64_t> skips{};
        const auto skipped = ++skips;
        if (skipped == 1 || skipped % 300 == 0)
            trace_event("DLSS-NR Before input unavailable view=%llu skips=%llu", frame.view_id, skipped);
        frame.color = nullptr;
        static_cast<void>(evaluate_dlss_nr(frame, settings));
        return nullptr;
    }
    transition(frame.command_list, frame.color, frame.color_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(frame.command_list, private_color, frame.color_state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
    source.pResource = frame.color;
    destination.pResource = private_color;
    source.Type = destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{frame.color_base_x, frame.color_base_y, 0U,
        frame.color_base_x + frame.input_width, frame.color_base_y + frame.input_height, 1U};
    frame.command_list->CopyTextureRegion(&destination, frame.color_base_x, frame.color_base_y,
        0U, &source, &box);
    transition(frame.command_list, frame.color, D3D12_RESOURCE_STATE_COPY_SOURCE, frame.color_state);
    transition(frame.command_list, private_color, D3D12_RESOURCE_STATE_COPY_DEST, frame.color_state);
    frame.color = private_color;
    auto processing_settings = settings;
    processing_settings.nr_alignment_border_enabled = false;
    return evaluate_dlss_nr(frame, processing_settings) ? private_color : nullptr;
}
void note_dlss_nr_input_submission(ID3D12CommandQueue* queue,
    ID3D12Object* command_list) noexcept {
    if (!queue || !command_list) return;
    const auto identity = d3d12_submission_identity(command_list);
    std::lock_guard lock(mutex);
    for (auto& input : inputs) {
        if (!input.recording || (input.recording != command_list &&
            (!identity || input.recording_identity != identity))) continue;
        if (input.value == 0)
            trace_event("DLSS-NR Before submission matched view=%llu aliased=%s", input.view,
                input.recording != command_list ? "yes" : "no");
        queue->AddRef();
        input.queue = queue;
        input.recording->Release();
        input.recording = nullptr;
    }
}
void collect_dlss_nr_input_submissions() noexcept {
    std::lock_guard lock(mutex);
    for (auto& input : inputs) {
        if (input.queue && SUCCEEDED(input.queue->Signal(input.fence, input.value + 1U))) {
            ++input.value;
            input.queue->Release();
            input.queue = nullptr;
        }
    }
    collect();
}
void release_dlss_nr_inputs(DlssViewId view_id) noexcept {
    std::lock_guard lock(mutex);
    for (auto& input : inputs)
        if (!view_id || input.view == view_id) input.retired = true;
    for (auto it = histories.begin(); it != histories.end();)
        if (!view_id || it->view == view_id) it = histories.erase(it); else ++it;
    collect();
}
bool dlss_nr_input_history_compatible(DlssViewId view, NrProcessingOrder order,
    bool processed, std::uint32_t width, std::uint32_t height) noexcept {
    std::lock_guard lock(mutex);
    for (const auto& history : histories)
        if (history.view == view) return history.order == order && history.processed == processed &&
            history.width == width && history.height == height;
    return false;
}
bool dlss_nr_input_history_reset(DlssViewId view, NrProcessingOrder order,
    bool processed, std::uint32_t width, std::uint32_t height) noexcept {
    std::lock_guard lock(mutex);
    const History next{view, order, processed, width, height};
    for (auto& history : histories) {
        if (history.view != view) continue;
        const bool reset = history.order != order || history.processed != processed ||
            history.width != width || history.height != height;
        history = next;
        return reset;
    }
    histories.push_back(next);
    return true;
}
} // namespace cheeky::foveated_dlss
