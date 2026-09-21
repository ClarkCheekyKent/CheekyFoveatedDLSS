#include "backend.hpp"
#include "dlss_nr_lifetime.hpp"
#include "gaze_foveation.hpp"
#include "timing_list_alias.hpp"
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
using namespace cheeky::foveated_dlss;
using Microsoft::WRL::ComPtr;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void check(HRESULT hr) { require(SUCCEEDED(hr), "D3D12 safety fixture failed"); }
constexpr GUID lifetime_key{0xa1a10442, 0x24cb, 0x421a, {0x99,0x65,0x1f,0x70,0x3c,0x95,0x92,0x84}};
struct Sentinel final : IUnknown {
    std::atomic<ULONG> refs{1}; bool& destroyed;
    explicit Sentinel(bool& value) : destroyed(value) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto n = --refs;
        if (!n) { destroyed = true; delete this; }
        return n;
    }
};
void watch(ID3D12Resource* resource, bool& destroyed) {
    auto* sentinel = new Sentinel(destroyed);
    check(resource->SetPrivateDataInterface(lifetime_key, sentinel)); sentinel->Release();
}

// Forward real compositor commands and inspect their ordering. Failed baseline
// recordings are discarded rather than submitting a known invalid GPU stream.
struct List : TimingListAlias {
    ID3D12Resource *color{}, *output{};
    D3D12_RESOURCE_STATES color_state{}, output_state{};
    bool valid_dispatch{true}; unsigned dispatches{};
    bool valid_before_states{true}, only_first_subresource{true};
    std::set<UINT64> tables;
    bool unique_tables{true};
    explicit List(ID3D12GraphicsCommandList* list) : TimingListAlias(list) {
        methods[14] = reinterpret_cast<void*>(&dispatch);
        methods[25] = reinterpret_cast<void*>(&pipeline);
        methods[26] = reinterpret_cast<void*>(&barriers);
        methods[28] = reinterpret_cast<void*>(&heaps);
        methods[29] = reinterpret_cast<void*>(&signature);
        methods[31] = reinterpret_cast<void*>(&table);
        methods[35] = reinterpret_cast<void*>(&constants);
    }
    static void STDMETHODCALLTYPE dispatch(List* self, UINT x, UINT y, UINT z) {
        ++self->dispatches;
        if (self->color) self->valid_dispatch &=
            (self->color_state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0 &&
            self->output_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        self->target->Dispatch(x,y,z);
    }
    static void STDMETHODCALLTYPE barriers(List* self, UINT count, const D3D12_RESOURCE_BARRIER* values) {
        for (UINT i=0; i<count; ++i) if (values[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            const auto& t = values[i].Transition;
            if (t.pResource == self->color) { self->valid_before_states &= self->color_state == t.StateBefore; self->color_state = t.StateAfter; }
            if (t.pResource == self->output) { self->valid_before_states &= self->output_state == t.StateBefore; self->output_state = t.StateAfter; }
            if (t.pResource == self->color || t.pResource == self->output) self->only_first_subresource &= t.Subresource == 0;
        }
        self->target->ResourceBarrier(count, values);
    }
    static void STDMETHODCALLTYPE pipeline(List* s, ID3D12PipelineState* p) { s->target->SetPipelineState(p); }
    static void STDMETHODCALLTYPE heaps(List* s, UINT n, ID3D12DescriptorHeap* const* h) { s->target->SetDescriptorHeaps(n,h); }
    static void STDMETHODCALLTYPE signature(List* s, ID3D12RootSignature* r) { s->target->SetComputeRootSignature(r); }
    static void STDMETHODCALLTYPE table(List* s, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE h) {
        if (index == 0) s->unique_tables &= s->tables.insert(h.ptr).second;
        s->target->SetComputeRootDescriptorTable(index,h);
    }
    static void STDMETHODCALLTYPE constants(List* s, UINT i, UINT n, const void* data, UINT offset) { s->target->SetComputeRoot32BitConstants(i,n,data,offset); }
};
struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> done;
    ComPtr<ID3D12InfoQueue> info;
    UINT64 sequence{};
    Gpu() {
        reset_gaze_foveation();
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        std::cout << "SR safety debug validation: " << (debug ? "enabled" : "unavailable") << '\n';
        ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> warp; check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        device.As(&info);
        D3D12_COMMAND_QUEUE_DESC q{}; check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)));
    }
    ~Gpu() { list.Reset(); release_d3d12_resources(); }
    ComPtr<ID3D12Resource> texture(UINT size, D3D12_RESOURCE_STATES state, UINT16 slices=1, UINT16 mips=1) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=d.Height=size; d.DepthOrArraySize=slices; d.MipLevels=mips; d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> result;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&result)));
        return result;
    }
    void execute() {
        check(list->Close()); ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1,lists); nr_recording_submitted(queue.Get(),list.Get());
    }
    void wait() {
        check(queue->Signal(done.Get(),++sequence));
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); require(event != nullptr,"GPU event failed");
        check(done->SetEventOnCompletion(sequence,event)); const auto result=WaitForSingleObject(event,10000); CloseHandle(event);
        require(result==WAIT_OBJECT_0,"GPU safety test timed out");
        require(device->GetDeviceRemovedReason()==S_OK,"Safety test removed device");
    }
    void clean_debug() {
        if (!info) return;
        for (UINT64 i=0; i<info->GetNumStoredMessages(); ++i) {
            SIZE_T bytes{}; check(info->GetMessage(i,nullptr,&bytes));
            std::vector<unsigned char> storage(bytes); auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            check(info->GetMessage(i,m,&bytes));
            if (m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(m->pDescription);
        }
    }
};
D3D12Evaluation* prepare(ID3D12GraphicsCommandList* list, ID3D12Resource* color, ID3D12Resource* output,
    UINT size=128, D3D12_RESOURCE_STATES cs=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
    D3D12_RESOURCE_STATES os=D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    Settings settings; settings.enabled=true; settings.center_mode=FoveationCenterMode::fixed;
    settings.auto_stereo_alignment=false; settings.width=settings.height=.5F;
    return prepare_d3d12_streamline(list,color,output,size,size,size,size,0,0,0,0,7001,settings,false,0,cs,os);
}
void states() {
    Gpu gpu;
    auto color=gpu.texture(128,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,2,2);
    auto output=gpu.texture(128,D3D12_RESOURCE_STATE_COPY_SOURCE,2,2);
    List list(gpu.list.Get()); list.color=color.Get(); list.output=output.Get();
    list.color_state=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; list.output_state=D3D12_RESOURCE_STATE_COPY_SOURCE;
    auto* e=prepare(list.get(),color.Get(),output.Get(),128,list.color_state,list.output_state);
    require(e!=nullptr,"State fixture preparation failed"); finish_d3d12_streamline(list.get(),e,true);
    require(list.dispatches==1 && list.valid_dispatch,"SR dispatch used color/output in incompatible states");
    require(list.color_state==D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE && list.output_state==D3D12_RESOURCE_STATE_COPY_SOURCE,
        "SR did not restore the tagged resource states");
    require(list.valid_before_states && list.only_first_subresource,"SR modified untagged slices/mips or used incorrect before-states");
    require(prepare(list.get(),color.Get(),output.Get(),128,static_cast<D3D12_RESOURCE_STATES>(0xFFFFFFFFU))==nullptr,
        "SR accepted unknown input state");
    require(prepare(list.get(),color.Get(),output.Get(),128,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,static_cast<D3D12_RESOURCE_STATES>(0xFFFFFFFFU))==nullptr,
        "SR accepted unknown output state");
    // A peripheral base replaces the tagged input and has its own known state.
    auto base=gpu.texture(128,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    e=prepare(list.get(),color.Get(),output.Get(),128,list.color_state,list.output_state);
    require(e && d3d12_set_composite_base(e,base.Get()),"Peripheral base setup failed");
    list.color=base.Get(); list.color_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    finish_d3d12_streamline(list.get(),e,true);
    require(list.valid_dispatch && list.valid_before_states,"SR used original tag state for the peripheral base");
    gpu.execute(); gpu.wait(); gpu.clean_debug();
}
void descriptors() {
    Gpu gpu; auto color=gpu.texture(128,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto output=gpu.texture(128,D3D12_RESOURCE_STATE_UNORDERED_ACCESS); List list(gpu.list.Get());
    for (unsigned i=0; i<257; ++i) {
        auto* e=prepare(list.get(),color.Get(),output.Get()); require(e!=nullptr,"SR descriptor reservation failed");
        finish_d3d12_streamline(list.get(),e,true);
    }
    require(list.unique_tables,"SR overwrote descriptors belonging to an unsubmitted recording (slot 257)");
    gpu.execute(); gpu.wait(); gpu.clean_debug();
}
void eviction() {
    // Sentinel outlives Gpu cleanup, including the intentionally failing baseline.
    bool destroyed=false;
    Gpu gpu; auto color=gpu.texture(256,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto output=gpu.texture(256,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto* first=prepare(gpu.list.Get(),color.Get(),output.Get(),64); require(first!=nullptr,"Initial cache allocation failed");
    watch(d3d12_private_output(first),destroyed); finish_d3d12_streamline(gpu.list.Get(),first,true);
    unsigned accepted=1;
    for (unsigned size=80; size<=192; size+=16) {
        auto* e=prepare(gpu.list.Get(),color.Get(),output.Get(),size);
        if (e) { ++accepted; finish_d3d12_streamline(gpu.list.Get(),e,true); }
    }
    require(!destroyed,"SR evicted a resource referenced by an unsubmitted recording");
    require(accepted<=8,"SR resource pressure did not respect the bounded pool");
    release_d3d12_resources(); require(!destroyed,"SR teardown freed an executable recording's resources");
    gpu.execute(); gpu.wait();
    release_d3d12_resources(); require(!destroyed,"SR freed resources while the completed list could still replay");
    ID3D12CommandList* replay[]{gpu.list.Get()}; gpu.queue->ExecuteCommandLists(1,replay);
    nr_recording_submitted(gpu.queue.Get(),gpu.list.Get()); gpu.wait();
    check(gpu.allocator->Reset()); check(gpu.list->Reset(gpu.allocator.Get(),nullptr)); nr_recording_reset(gpu.list.Get(),S_OK);
    release_d3d12_resources(); require(destroyed,"Retired and completed SR resources did not drain"); gpu.clean_debug();
}
void pending_reset() {
    bool destroyed=false, color_destroyed=false, output_destroyed=false;
    Gpu gpu;
    // Always unblock the queue on failure so assertions cannot hang teardown.
    struct Gate {
        ComPtr<ID3D12Fence> fence;
        ~Gate() { if (fence) fence->Signal(1); }
    } gate;
    check(gpu.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate.fence)));
    auto color=gpu.texture(128,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto output=gpu.texture(128,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto* e=prepare(gpu.list.Get(),color.Get(),output.Get()); require(e!=nullptr,"Pending fixture preparation failed");
    auto* scratch=d3d12_private_output(e);
    watch(scratch,destroyed); watch(color.Get(),color_destroyed); watch(output.Get(),output_destroyed);
    finish_d3d12_streamline(gpu.list.Get(),e,true);
    check(gpu.queue->Wait(gate.fence.Get(),1)); gpu.execute();
    // Reset is legal before completion with a fresh allocator. Keep the old
    // allocator alive until its blocked execution finishes.
    ComPtr<ID3D12CommandAllocator> next;
    check(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&next)));
    check(gpu.list->Reset(next.Get(),nullptr)); nr_recording_reset(gpu.list.Get(),S_OK);
    e=prepare(gpu.list.Get(),color.Get(),output.Get()); require(e!=nullptr,"Second recording preparation failed");
    require(d3d12_private_output(e)!=scratch,"SR reused scratch while a retired recording was still executing");
    finish_d3d12_streamline(gpu.list.Get(),e,true);
    color.Reset(); output.Reset(); release_d3d12_resources();
    require(!destroyed && !color_destroyed && !output_destroyed,"SR freed resources/descriptors behind a blocked GPU queue");
    check(gate.fence->Signal(1)); gpu.wait(); collect_d3d12_resources();
    require(destroyed,"Completed, retired scratch did not drain during normal collection");
    require(!color_destroyed && !output_destroyed,"Second executable recording lost its descriptor resources");
    gpu.execute(); gpu.wait(); gpu.clean_debug();
    gpu.list.Reset(); collect_d3d12_resources();
    require(color_destroyed && output_destroyed,"Descriptor resource references leaked after list destruction");
}
void active_evaluation() {
    bool destroyed=false;
    Gpu gpu; auto color=gpu.texture(128,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto output=gpu.texture(128,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto* e=prepare(gpu.list.Get(),color.Get(),output.Get()); require(e!=nullptr,"Active evaluation fixture failed");
    watch(d3d12_private_output(e),destroyed);
    release_d3d12_resources(); require(!destroyed,"Teardown freed an active SR evaluation");
    finish_d3d12_streamline(gpu.list.Get(),e,false);
    check(gpu.list->Close()); gpu.list.Reset(); collect_d3d12_resources();
    require(destroyed,"Abandoned SR evaluation did not release after recording destruction");
}
void recycling() {
    bool destroyed=false;
    Gpu gpu; auto color=gpu.texture(128,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto output=gpu.texture(128,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* first{};
    for (unsigned i=0; i<24; ++i) {
        auto* e=prepare(gpu.list.Get(),color.Get(),output.Get()); require(e!=nullptr,"Completed recordings exhausted the SR pool");
        if (!i) { first=d3d12_private_output(e); watch(first,destroyed); }
        require(d3d12_private_output(e)==first && !destroyed,"SR failed to reuse a safely retired allocation");
        finish_d3d12_streamline(gpu.list.Get(),e,true); gpu.execute(); gpu.wait();
        check(gpu.allocator->Reset()); check(gpu.list->Reset(gpu.allocator.Get(),nullptr)); nr_recording_reset(gpu.list.Get(),S_OK);
    }
    release_d3d12_resources(); require(destroyed,"Idle SR cache did not release at teardown"); gpu.clean_debug();
}
}
int run_d3d12_safety_tests() {
    unsigned failures{};
    const std::pair<const char*, void(*)()> tests[]{{"SR tagged states",states}, {"SR descriptor exhaustion",descriptors},
        {"SR cache eviction/replay",eviction}, {"SR pending GPU/reset",pending_reset},
        {"SR active evaluation teardown",active_evaluation}, {"SR safe recycling",recycling}};
    for (const auto& test : tests) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << test.first << ": " << e.what() << '\n'; }
    }
    return failures ? 1 : 0;
}
