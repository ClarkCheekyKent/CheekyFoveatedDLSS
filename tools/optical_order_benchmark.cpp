// Standalone real-hardware microbenchmark. No installed DLLs or settings are changed.
#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include "../openxr_layer/fg_optical.hpp"

using Microsoft::WRL::ComPtr;
using namespace cheeky::fg;
using Clock = std::chrono::steady_clock;
static void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
static void hr(HRESULT value, const char* what) {
    if (FAILED(value)) { std::ostringstream s; s << what << " HRESULT=0x" << std::hex << unsigned(value); throw std::runtime_error(s.str()); }
}
struct Command {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    void begin(ID3D12Device* device) {
        if (!allocator) {
            hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
            hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
        } else { hr(allocator->Reset(), "allocator reset"); hr(list->Reset(allocator.Get(), nullptr), "list reset"); }
    }
    void submit(ID3D12CommandQueue* queue) {
        hr(list->Close(), "close list"); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
    }
};
struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queues[7]; // coordinator; up to 2 eyes x 3 generated phases
    ComPtr<ID3D12Fence> fence, eye_done[2];
    ComPtr<ID3D12Fence> first_done[2], phase_done[6];
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> stamp_readback;
    ComPtr<ID3D12InfoQueue> info;
    Command start, first, finish;
    HANDLE event{};
    UINT64 serial{}, frequency{};
    explicit Gpu(bool debug) {
        if (debug) { ComPtr<ID3D12Debug> d; hr(D3D12GetDebugInterface(IID_PPV_ARGS(&d)), "debug layer unavailable"); d->EnableDebugLayer(); }
        ComPtr<IDXGIFactory6> factory; hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        ComPtr<IDXGIAdapter1> adapter;
        for (unsigned i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            const auto result = factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
            require(result != DXGI_ERROR_NOT_FOUND, "No NVIDIA hardware adapter");
            hr(result, "enumerate hardware adapter");
            DXGI_ADAPTER_DESC1 desc{}; hr(candidate->GetDesc1(&desc), "adapter description");
            if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { adapter = candidate; std::wcout << L"Adapter: " << desc.Description << L'\n'; break; }
        }
        hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12 hardware device");
        if (debug) device.As(&info);
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        for (auto& queue : queues) { hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "NORMAL queue"); UINT64 f{}; hr(queue->GetTimestampFrequency(&f), "timestamp frequency"); require(!frequency || frequency == f, "queue timestamp frequencies differ"); frequency = f; }
        hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        for (auto& f : eye_done) hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)), "eye fence");
        for (auto& f : first_done) hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)), "first output fence");
        for (auto& f : phase_done) hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)), "phase fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr); require(event != nullptr, "event");
        D3D12_QUERY_HEAP_DESC query{}; query.Count = 3; query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        hr(device->CreateQueryHeap(&query, IID_PPV_ARGS(&timestamps)), "timestamp queries");
        stamp_readback = buffer(D3D12_HEAP_TYPE_READBACK, 24, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    ~Gpu() { if (event) CloseHandle(event); }
    ComPtr<ID3D12Resource> buffer(D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> out; hr(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out)), "buffer"); return out;
    }
    void wait(UINT64 value) {
        if (fence->GetCompletedValue() < value) { hr(fence->SetEventOnCompletion(value, event), "completion event"); require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "GPU timeout"); }
        hr(device->GetDeviceRemovedReason(), "GPU removed");
    }
    void drain() { hr(queues[0]->Signal(fence.Get(), ++serial), "drain signal"); wait(serial); }
    ComPtr<ID3D12Resource> texture(unsigned size, const std::vector<std::uint32_t>& pixels) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = d.Height = size;
        d.DepthOrArraySize = d.MipLevels = 1; d.SampleDesc.Count = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ComPtr<ID3D12Resource> out; hr(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out)), "source texture");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 bytes{}; device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
        auto upload = buffer(D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ); void* mapped{};
        hr(upload->Map(0, nullptr, &mapped), "upload map");
        for (unsigned y = 0; y < size; ++y) memcpy(static_cast<char*>(mapped) + fp.Offset + size_t(y) * fp.Footprint.RowPitch, pixels.data() + size_t(y) * size, size_t(size) * 4);
        upload->Unmap(0, nullptr); Command c; c.begin(device.Get());
        D3D12_TEXTURE_COPY_LOCATION to{}, from{}; to.pResource = out.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = fp;
        c.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        optical::Session12::barrier(c.list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        c.submit(queues[0].Get()); drain(); return out;
    }
    std::uint64_t hash(ID3D12Resource* texture) {
        const auto d = texture->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 bytes{};
        device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
        auto readback = buffer(D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
        Command c; c.begin(device.Get()); optical::Session12::barrier(c.list.Get(), texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION to{}, from{}; from.pResource = texture; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = fp;
        c.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        optical::Session12::barrier(c.list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        c.submit(queues[0].Get()); drain(); void* mapped{}; hr(readback->Map(0, nullptr, &mapped), "hash map");
        std::uint64_t value = 14695981039346656037ull; unsigned lo = 255, hi = 0;
        for (unsigned y = 0; y < d.Height; ++y) {
            auto row = static_cast<const unsigned char*>(mapped) + fp.Offset + size_t(y) * fp.Footprint.RowPitch;
            for (size_t x = 0; x < d.Width * 4; ++x) { value = (value ^ row[x]) * 1099511628211ull; if (x % 4 != 3) { lo = std::min(lo, unsigned(row[x])); hi = std::max(hi, unsigned(row[x])); } }
        }
        readback->Unmap(0, nullptr); require(hi > lo + 16, "Output is blank/constant"); return value;
    }
    void check_debug() {
        if (!info) return;
        bool error = false;
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
            SIZE_T bytes{}; info->GetMessage(i, nullptr, &bytes); std::vector<unsigned char> data(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data()); hr(info->GetMessage(i, message, &bytes), "debug message");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { std::cerr << message->pDescription << '\n'; error = true; }
        }
        info->ClearStoredMessages(); require(!error, "D3D12 debug validation failed");
    }
};

// Benchmark-only split of Session12::prepare. The shaders, formats, resources,
// NVOF configuration and interpolation remain the production implementation.
// All schedules use these SAME methods; only ordering and queue assignment vary.
struct Session : optical::Session12 {
    UINT64 ready{};
    void prepare_luma(ID3D12Resource* old, ID3D12Resource* now, OpticalConstants c) {
        require(wait(), "Previous optical session did not retire");
        hr(allocator->Reset(), "flow allocator reset"); hr(list->Reset(allocator.Get(), nullptr), "flow list reset");
        barrier(list.Get(), old, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(list.Get(), now, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for (unsigned i = 0; i < 2; ++i) barrier(list.Get(), textures[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        bind(list.Get(), 0, old, now, nullptr, c, true, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM);
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        for (unsigned i = 0; i < 2; ++i) barrier(list.Get(), textures[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        barrier(list.Get(), old, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        barrier(list.Get(), now, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        hr(list->Close(), "flow prepare close"); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
        ready = ++serial; hr(queue->Signal(fence.Get(), ready), "luma ready");
    }
    void launch() {
        NV_OF_FENCE_POINT start{fence.Get(), ready}, done{fence.Get(), ++serial};
        NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{}; in.inputFrame = buffers[0]; in.referenceFrame = buffers[1]; in.numFencePoints = 1; in.fencePoint = &start;
        in.disableTemporalHints = NV_OF_TRUE; // Same deterministic A/B workload in every schedule.
        NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{}; out.outputBuffer = buffers[2]; out.bwdOutputBuffer = buffers[3]; out.outputCostBuffer = buffers[4]; out.bwdOutputCostBuffer = buffers[5]; out.fencePoint = &done;
        const auto status = api.nvOFExecuteD3D12(handle, &in, &out);
        if (status) { --serial; throw std::runtime_error("nvOFExecuteD3D12 status=" + std::to_string(status)); }
    }
    void dependency() { hr(queue->Wait(fence.Get(), serial), "flow GPU dependency"); }
    // Multiple phase queues share ONLY immutable SRVs. The caller transitions
    // sources/vectors once, fences readers in, then joins before restoring them.
    void render_readonly(ID3D12GraphicsCommandList* commands, ID3D12Resource* old, ID3D12Resource* now,
                         OpticalConstants c, float fraction, unsigned phase, ComPtr<ID3D12Resource>& output) {
        if (!output) require(texture(unsigned(c.sizes[2]), unsigned(c.sizes[3]), DXGI_FORMAT_R8G8B8A8_UNORM, true, output), diagnostic);
        barrier(commands, output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        c.flags[0] = fraction;
        bind(commands, 9 * (1 + phase), old, now, output.Get(), c, false, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM);
        commands->Dispatch((unsigned(c.sizes[2]) + 7) / 8, (unsigned(c.sizes[3]) + 7) / 8, 1);
        barrier(commands, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    }
};
struct Options {
    unsigned size{3000}, phases{3}, preset{1}, percent{100}, grid{4}, samples{60}, rounds{3}, warmup{8};
    unsigned peripheral_percent{50}, peripheral_preset{1}, peripheral_output{50};
    float center_width{.55f}, center_height{.45f};
    bool debug{}; std::string workloads{"Full,Center,Foveated"}, output{"samples.csv"};
    std::string modes{"serial,launch-all,prepare-all,consume-per-eye,consume-per-region,first-stereo,centers-first,eye-queues,phase-queues,two-pair-pipeline"};
};
struct Task {
    unsigned eye{}, pass{}; OpticalConstants constants;
    std::unique_ptr<Session> session;
    ComPtr<ID3D12Resource> output[3];
    Command commands[3], readable;
};
enum class Mode { serial, launch_all, prepare_all, consume_per_eye, consume_per_region, first_stereo, centers_first, eye_queues, phase_queues, two_pairs };
static const char* name(Mode m) {
    switch (m) { case Mode::serial: return "serial"; case Mode::launch_all: return "launch-all";
    case Mode::prepare_all: return "prepare-all"; case Mode::consume_per_eye: return "consume-per-eye";
    case Mode::consume_per_region: return "consume-per-region"; case Mode::eye_queues: return "eye-queues";
    case Mode::first_stereo: return "first-stereo"; case Mode::centers_first: return "centers-first";
    case Mode::phase_queues: return "phase-queues"; default: return "two-pair-pipeline"; }
}
struct Sample { double gpu_ms{}, cpu_ms{}, first_ms{}, batch_ms{}; unsigned pairs{1}; };
static Sample collect_sample(Gpu& gpu, Clock::time_point cpu_start, unsigned pairs = 1) {
    gpu.drain(); const double cpu_ms = std::chrono::duration<double, std::milli>(Clock::now() - cpu_start).count();
    void* mapped{}; hr(gpu.stamp_readback->Map(0, nullptr, &mapped), "timestamp map");
    UINT64 ticks[3]; memcpy(ticks, mapped, sizeof(ticks)); gpu.stamp_readback->Unmap(0, nullptr);
    require(ticks[2] >= ticks[1] && ticks[1] >= ticks[0], "timestamp order"); gpu.check_debug();
    const double total = double(ticks[2] - ticks[0]) * 1000. / double(gpu.frequency);
    return {total / pairs, cpu_ms / pairs, double(ticks[1] - ticks[0]) * 1000. / double(gpu.frequency), total, pairs};
}
struct Case {
    Gpu& gpu; const Options& options; Mode mode;
    std::vector<Task> tasks;
    ID3D12Resource* sources[2][2]{};
    Case(Gpu& g, const Options& o, Mode m, const std::string& workload, ComPtr<ID3D12Resource> (&input)[2][2]) : gpu(g), options(o), mode(m) {
        for (unsigned eye = 0; eye < 2; ++eye) {
            for (unsigned f = 0; f < 2; ++f) sources[eye][f] = input[eye][f].Get();
            for (unsigned pass = 0; pass < (workload == "Foveated" ? 2u : 1u); ++pass) {
                Task t; t.eye = eye; t.pass = pass;
                const bool full = workload == "Full";
                CheekyFgEye meta; meta.width = meta.height = o.size;
                meta.optical.center_percent = o.percent; meta.optical.peripheral_percent = o.peripheral_percent;
                meta.optical.center_grid = meta.optical.peripheral_grid = o.grid;
                const float center[]{(1-o.center_width)*.5f, (1-o.center_height)*.5f, o.center_width, o.center_height}, whole[]{0, 0, 1, 1};
                CompactLayout layout; layout.width = layout.height = o.size;
                const auto width = (full ? o.size : pass ? std::max(32u, (o.size * o.peripheral_output + 50) / 100) : unsigned(std::lround(o.size * o.center_width)));
                const auto height = (full ? o.size : pass ? std::max(32u, (o.size * o.peripheral_output + 50) / 100) : unsigned(std::lround(o.size * o.center_height)));
                t.constants = optical::constants(meta, full ? 1u : pass, full || pass ? whole : center, layout, layout, width, height);
                if (full) { t.constants.sizes[0] = t.constants.sizes[1] = float(std::max(64u, ((o.size * o.percent + 99) / 100 + 3) & ~3u)); }
                t.session = std::make_unique<Session>();
                auto* queue = gpu.queues[mode == Mode::eye_queues ? eye + 1 : 0].Get();
                require(t.session->initialize(gpu.device.Get(), queue, unsigned(t.constants.sizes[0]), unsigned(t.constants.sizes[1]), pass ? o.peripheral_preset : o.preset, o.grid), t.session->diagnostic);
                std::cout << "  eye=" << eye << " region=" << pass << " flow=" << t.constants.sizes[0] << 'x' << t.constants.sizes[1] << " output=" << width << 'x' << height << '\n';
                tasks.push_back(std::move(t));
            }
        }
    }
    void render(Task& t, unsigned phase) {
        auto& s = *t.session; auto& c = t.commands[phase]; s.descriptor_set = phase;
        c.begin(gpu.device.Get());
        require(s.render(c.list.Get(), sources[t.eye][0], sources[t.eye][1], nullptr, t.constants, float(phase + 1) / (options.phases + 1),
            DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, t.output[phase]), s.diagnostic);
        c.submit(s.queue.Get());
    }
    Sample run() {
        // CPU completion once per stereo interval; never queue multiple native pairs.
        const auto cpu_start = Clock::now();
        gpu.start.begin(gpu.device.Get()); gpu.start.list->EndQuery(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        gpu.start.submit(gpu.queues[0].Get()); const auto gate = ++gpu.serial;
        hr(gpu.queues[0]->Signal(gpu.fence.Get(), gate), "start gate");
        if (mode == Mode::eye_queues) for (unsigned eye = 0; eye < 2; ++eye) hr(gpu.queues[eye + 1]->Wait(gpu.fence.Get(), gate), "eye start dependency");
        std::vector<Task*> flow_order; for (auto& t : tasks) flow_order.push_back(&t);
        if (mode == Mode::centers_first) std::stable_sort(flow_order.begin(), flow_order.end(), [](const Task* a, const Task* b) { return a->pass < b->pass; });
        for (auto* t : flow_order) {
            t->session->prepare_luma(sources[t->eye][0], sources[t->eye][1], t->constants);
            if (mode != Mode::prepare_all) t->session->launch();
            if (mode == Mode::serial) t->session->dependency();
        }
        if (mode == Mode::prepare_all) for (auto& t : tasks) t.session->launch();
        const bool stereo_first = mode == Mode::first_stereo || mode == Mode::centers_first;
        if (mode != Mode::serial && mode != Mode::consume_per_eye && mode != Mode::consume_per_region && !stereo_first) for (auto& t : tasks) t.session->dependency();
        if (stereo_first) {
            for (unsigned phase = 0; phase < options.phases; ++phase) {
                for (unsigned eye = 0; eye < 2; ++eye) for (auto& t : tasks) if (t.eye == eye) {
                    if (phase == 0) t.session->dependency();
                    render(t, phase);
                }
                if (phase == 0) { gpu.first.begin(gpu.device.Get()); gpu.first.list->EndQuery(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1); gpu.first.submit(gpu.queues[0].Get()); }
            }
        } else for (unsigned eye = 0; eye < 2; ++eye) {
            if (mode == Mode::consume_per_eye) for (auto& t : tasks) if (t.eye == eye) t.session->dependency();
            if (mode == Mode::consume_per_region) {
                // One timestamp first, so a ready region does not block its eye's
                // first complete stereo timestamp behind all its later phases.
                for (auto& t : tasks) if (t.eye == eye) { t.session->dependency(); render(t, 0); }
                if (eye == 1) { gpu.first.begin(gpu.device.Get()); gpu.first.list->EndQuery(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1); gpu.first.submit(gpu.queues[0].Get()); }
                for (unsigned phase = 1; phase < options.phases; ++phase) for (auto& t : tasks) if (t.eye == eye) render(t, phase);
            } else for (unsigned phase = 0; phase < options.phases; ++phase) {
                for (auto& t : tasks) if (t.eye == eye) render(t, phase);
                if (phase == 0) {
                    if (mode == Mode::eye_queues) hr(gpu.queues[eye + 1]->Signal(gpu.first_done[eye].Get(), gate), "eye first ready");
                    else if (eye == 1) { gpu.first.begin(gpu.device.Get()); gpu.first.list->EndQuery(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1); gpu.first.submit(gpu.queues[0].Get()); }
                }
            }
            if (mode == Mode::eye_queues) {
                hr(gpu.queues[eye + 1]->Signal(gpu.eye_done[eye].Get(), gate), "eye complete");
            }
        }
        if (mode == Mode::eye_queues) {
            for (unsigned eye = 0; eye < 2; ++eye) hr(gpu.queues[0]->Wait(gpu.first_done[eye].Get(), gate), "join first eye");
            gpu.first.begin(gpu.device.Get()); gpu.first.list->EndQuery(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1); gpu.first.submit(gpu.queues[0].Get());
            for (unsigned eye = 0; eye < 2; ++eye) hr(gpu.queues[0]->Wait(gpu.eye_done[eye].Get(), gate), "join eye");
        }
        gpu.finish.begin(gpu.device.Get()); gpu.finish.list->EndQuery(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
        gpu.finish.list->ResolveQueryData(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 3, gpu.stamp_readback.Get(), 0);
        gpu.finish.submit(gpu.queues[0].Get()); return collect_sample(gpu, cpu_start);
    }
    std::vector<UINT64> hashes() { std::vector<UINT64> result; for (auto& t : tasks) for (unsigned p = 0; p < options.phases; ++p) result.push_back(gpu.hash(t.output[p].Get())); gpu.check_debug(); return result; }
};
// Schedule ALL generated timestamps independently, while OFA handles other
// eyes/regions/pairs. Two pairs use separate sessions, immutable capture slots,
// outputs, allocators and descriptors. No temporal history is shared across them.
struct ParallelCase {
    Gpu& gpu; const Options& options;
    std::unique_ptr<Case> cases[2]; unsigned pairs;
    Command source_states;
    ParallelCase(Gpu& g, const Options& o, const std::string& w, unsigned count,
                 ComPtr<ID3D12Resource> (&input)[2][2], ComPtr<ID3D12Resource> (&next)[2][2]) : gpu(g), options(o), pairs(count) {
        cases[0] = std::make_unique<Case>(g, o, Mode::launch_all, w, input);
        if (pairs == 2) cases[1] = std::make_unique<Case>(g, o, Mode::launch_all, w, next);
    }
    Sample run() {
        const auto cpu_start = Clock::now();
        gpu.start.begin(gpu.device.Get()); gpu.start.list->EndQuery(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0); gpu.start.submit(gpu.queues[0].Get());
        for (unsigned p = 0; p < pairs; ++p) for (auto& t : cases[p]->tasks) {
            t.session->prepare_luma(cases[p]->sources[t.eye][0], cases[p]->sources[t.eye][1], t.constants); t.session->launch();
        }
        source_states.begin(gpu.device.Get());
        for (unsigned p = 0; p < pairs; ++p) for (auto& eye : cases[p]->sources) for (auto* image : eye)
            Session::barrier(source_states.list.Get(), image, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        source_states.submit(gpu.queues[0].Get()); const auto gate = ++gpu.serial;
        hr(gpu.queues[0]->Signal(gpu.fence.Get(), gate), "shared SRV readiness");
        for (unsigned eye = 0; eye < 2; ++eye) for (unsigned phase = 0; phase < options.phases; ++phase)
            hr(gpu.queues[1 + eye * 3 + phase]->Wait(gpu.fence.Get(), gate), "phase source dependency");
        for (unsigned p = 0; p < pairs; ++p) {
            for (auto& t : cases[p]->tasks) {
                auto& s = *t.session; auto* control = gpu.queues[1 + t.eye * 3].Get();
                hr(control->Wait(s.fence.Get(), s.serial), "phase flow dependency");
                t.readable.begin(gpu.device.Get());
                for (unsigned i = 2; i < 6; ++i) Session::barrier(t.readable.list.Get(), s.textures[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                t.readable.submit(control); const auto ready = ++s.serial;
                hr(control->Signal(s.fence.Get(), ready), "vector SRVs ready");
                for (unsigned phase = 0; phase < options.phases; ++phase) {
                    auto* q = gpu.queues[1 + t.eye * 3 + phase].Get(); hr(q->Wait(s.fence.Get(), ready), "phase vector dependency");
                    auto& cmd = t.commands[phase]; cmd.begin(gpu.device.Get());
                    s.render_readonly(cmd.list.Get(), cases[p]->sources[t.eye][0], cases[p]->sources[t.eye][1], t.constants,
                        float(phase + 1) / (options.phases + 1), phase, t.output[phase]); cmd.submit(q);
                }
            }
            // Each phase-zero queue has now received all of that pair's regions.
            if (p == 0) for (unsigned eye = 0; eye < 2; ++eye)
                hr(gpu.queues[1 + eye * 3]->Signal(gpu.first_done[eye].Get(), gate), "first pair stereo readiness");
        }
        for (unsigned eye = 0; eye < 2; ++eye) hr(gpu.queues[0]->Wait(gpu.first_done[eye].Get(), gate), "first stereo join");
        gpu.first.begin(gpu.device.Get()); gpu.first.list->EndQuery(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1); gpu.first.submit(gpu.queues[0].Get());
        for (unsigned eye = 0; eye < 2; ++eye) for (unsigned phase = 0; phase < options.phases; ++phase) {
            const auto index = eye * 3 + phase;
            hr(gpu.queues[1 + index]->Signal(gpu.phase_done[index].Get(), gate), "phase completion");
            hr(gpu.queues[0]->Wait(gpu.phase_done[index].Get(), gate), "phase join");
        }
        gpu.finish.begin(gpu.device.Get());
        for (unsigned p = 0; p < pairs; ++p) {
            for (auto& eye : cases[p]->sources) for (auto* image : eye) Session::barrier(gpu.finish.list.Get(), image, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            for (auto& t : cases[p]->tasks) for (unsigned i = 2; i < 6; ++i) Session::barrier(gpu.finish.list.Get(), t.session->textures[i].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        gpu.finish.list->EndQuery(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
        gpu.finish.list->ResolveQueryData(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 3, gpu.stamp_readback.Get(), 0);
        gpu.finish.submit(gpu.queues[0].Get()); return collect_sample(gpu, cpu_start, pairs);
    }
    std::vector<UINT64> hashes() { auto result = cases[0]->hashes(); if (pairs == 2) { auto next = cases[1]->hashes(); result.insert(result.end(), next.begin(), next.end()); } return result; }
};
static std::uint32_t scene(int x, int y) {
    const int ix = int(std::floor(x / 8.f)), iy = int(std::floor(y / 8.f)); const float fx = x / 8.f - ix, fy = y / 8.f - iy;
    auto hash = [](int a, int b) { unsigned h = unsigned(a) * 0x9e3779b9u ^ unsigned(b) * 0x85ebca6bu; h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; return h ^ (h >> 16); };
    const unsigned h[]{hash(ix, iy), hash(ix + 1, iy), hash(ix, iy + 1), hash(ix + 1, iy + 1)};
    unsigned pixel = 0xff000000;
    for (unsigned c = 0; c < 3; ++c) { auto v = [&](unsigned n) { return float(32 + ((h[n] >> (8 * c)) & 191)); };
        pixel |= unsigned(std::lerp(std::lerp(v(0), v(1), fx), std::lerp(v(2), v(3), fx), fy)) << (8 * c); }
    return pixel;
}
int main(int argc, char** argv) try {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i]; if (k == "--debug") { o.debug = true; continue; }
        require(i + 1 < argc, "Missing argument value"); const std::string v = argv[++i];
        if (k == "--output") o.output = v; else if (k == "--workloads") o.workloads = v; else if (k == "--modes") o.modes = v;
        else if (k == "--center-width") o.center_width = std::stof(v);
        else if (k == "--center-height") o.center_height = std::stof(v);
        else { const unsigned n = unsigned(std::stoul(v));
            if (k == "--size") o.size = n; else if (k == "--phases") o.phases = n; else if (k == "--preset") o.preset = n;
            else if (k == "--percent") o.percent = n; else if (k == "--grid") o.grid = n; else if (k == "--samples") o.samples = n;
            else if (k == "--rounds") o.rounds = n; else if (k == "--warmup") o.warmup = n;
            else if (k == "--peripheral-percent") o.peripheral_percent = n;
            else if (k == "--peripheral-preset") o.peripheral_preset = n;
            else if (k == "--peripheral-output") o.peripheral_output = n;
            else throw std::runtime_error("Unknown argument: " + k);
        }
    }
    require(o.size >= 256 && o.size <= 8192 && o.phases >= 1 && o.phases <= 3 && o.preset <= 2 && o.percent >= 1 && o.percent <= 100 && (o.grid == 1 || o.grid == 2 || o.grid == 4) && o.samples && o.rounds && o.warmup, "Invalid arguments");
    require(std::isfinite(o.center_width) && std::isfinite(o.center_height) && o.center_width >= .01f && o.center_width <= 1 && o.center_height >= .01f && o.center_height <= 1 &&
        o.peripheral_percent >= 1 && o.peripheral_percent <= 100 && o.peripheral_preset <= 2 && o.peripheral_output >= 1 && o.peripheral_output <= 100, "Invalid regional settings");
    std::vector<std::string> workloads; std::stringstream parts(o.workloads); std::string workload;
    while (std::getline(parts, workload, ',')) { require(workload == "Full" || workload == "Center" || workload == "Foveated", "Invalid workload"); workloads.push_back(workload); }
    require(!workloads.empty(), "No workloads");
    std::vector<Mode> selected_modes; std::stringstream mode_parts(o.modes); std::string mode_name;
    while (std::getline(mode_parts, mode_name, ',')) {
        bool found = false;
        for (auto m : {Mode::serial, Mode::launch_all, Mode::prepare_all, Mode::consume_per_eye, Mode::consume_per_region, Mode::first_stereo, Mode::centers_first, Mode::eye_queues, Mode::phase_queues, Mode::two_pairs})
            if (mode_name == name(m)) { selected_modes.push_back(m); found = true; break; }
        require(found, "Invalid mode");
    }
    require(!selected_modes.empty(), "No modes");
    Gpu gpu(o.debug); ComPtr<ID3D12Resource> input[2][2];
    for (unsigned eye = 0; eye < 2; ++eye) for (unsigned f = 0; f < 2; ++f) {
        std::vector<std::uint32_t> pixels(size_t(o.size) * o.size);
        for (unsigned y = 0; y < o.size; ++y) for (unsigned x = 0; x < o.size; ++x) pixels[size_t(y) * o.size + x] = scene(int(x) - int(f) * 12, int(y) + int(eye) * 37);
        input[eye][f] = gpu.texture(o.size, pixels);
    }
    ComPtr<ID3D12Resource> next_input[2][2];
    if (std::find(selected_modes.begin(), selected_modes.end(), Mode::two_pairs) != selected_modes.end())
        for (unsigned eye = 0; eye < 2; ++eye) for (unsigned f = 0; f < 2; ++f) {
            std::vector<std::uint32_t> pixels(size_t(o.size) * o.size);
            for (unsigned y = 0; y < o.size; ++y) for (unsigned x = 0; x < o.size; ++x) pixels[size_t(y) * o.size + x] = scene(int(x) - int(f + 1) * 12, int(y) + int(eye) * 37);
            next_input[eye][f] = gpu.texture(o.size, pixels);
        }
    std::ofstream csv(o.output); require(bool(csv), "Cannot open results CSV"); csv << "workload,round,mode,sample,gpu_ms,cpu_ms,first_output_ms,batch_ms,pairs\n" << std::fixed << std::setprecision(6);
    std::mt19937 rng(0x434647);
    std::cout << "Two " << o.size << 'x' << o.size << " eyes, " << o.phases << " generated outputs per eye. Temporal hints OFF. Max two pairs.\n";
    for (const auto& w : workloads) {
        std::vector<UINT64> reference, next_reference;
        { std::cout << w << " establishing serial output reference\n"; Case baseline(gpu, o, Mode::serial, w, input); baseline.run(); reference = baseline.hashes(); }
        if (next_input[0][0]) { Case baseline(gpu, o, Mode::serial, w, next_input); baseline.run(); next_reference = baseline.hashes(); }
        for (unsigned round = 0; round < o.rounds; ++round) {
            auto modes = selected_modes;
            // References were established above; later rounds randomize schedule order.
            if (round) std::shuffle(modes.begin(), modes.end(), rng);
            for (const auto mode : modes) {
                std::cout << w << " round=" << round << " mode=" << name(mode) << '\n' << std::flush;
                auto measure = [&](auto& c, const auto& expected) {
                    c.run(); const auto hashes = c.hashes(); require(hashes == expected, "Output hashes differ from serial baseline; timings are invalid");
                    std::cout << "  All " << hashes.size() << " output hashes match serial baseline.\n";
                    for (unsigned i = 0; i < o.warmup; ++i) c.run();
                    double sum{}, first{};
                    for (unsigned i = 0; i < o.samples; ++i) { const auto s = c.run(); sum += s.gpu_ms; first += s.first_ms;
                        csv << w << ',' << round << ',' << name(mode) << ',' << i << ',' << s.gpu_ms << ',' << s.cpu_ms << ',' << s.first_ms << ',' << s.batch_ms << ',' << s.pairs << '\n'; }
                    csv.flush(); std::cout << "  GPU mean per-pair: " << sum / o.samples << " ms; first stereo output: " << first / o.samples << " ms\n" << std::flush;
                };
                if (mode == Mode::phase_queues || mode == Mode::two_pairs) {
                    auto expected = reference; if (mode == Mode::two_pairs) expected.insert(expected.end(), next_reference.begin(), next_reference.end());
                    ParallelCase c(gpu, o, w, mode == Mode::two_pairs ? 2u : 1u, input, next_input); measure(c, expected);
                } else { Case c(gpu, o, mode, w, input); measure(c, reference); }
            }
        }
    }
    std::cout << "PASS: real NVIDIA optical flow, all schedules bit-identical to serial; raw samples saved.\n";
    return 0;
} catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
