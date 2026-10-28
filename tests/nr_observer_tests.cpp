#include "dlss_nr_input.hpp"
#include "eye_calibration_d3d12.hpp"
#include "gaze_foveation.hpp"
#include "runtime.hpp"
#include "dlss_nr_lifetime.hpp"
#include "graphics_observer.hpp"
#include "timing_list_alias.hpp"
#include "d3d12_native.hpp"
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

// This executable links the real native observer and NR lifetime/input code.
// Unrelated gaze/timing/calibration consumers are inert; NVIDIA evaluation is
// substituted only for the copy test. Lifetime assertions use NrLifetime itself.
namespace cheeky::foveated_dlss {
void log_info(const char*) noexcept {}
void log_warning(const char*) noexcept {}
void log_error(const char*) noexcept {}
void trace_event(const char*, ...) noexcept {}
unsigned observed_copy_count{};
GazeCopyEdge observed_copy_edge{};
void record_gaze_copy(std::uint64_t, GazeCopyEdge edge) noexcept {
    ++observed_copy_count;
    observed_copy_edge = edge;
}
void submit_gaze_copies(std::uint64_t) noexcept {}
void reset_gaze_copies(std::uint64_t) noexcept {}
void forget_gaze_resource(std::uint64_t) noexcept {}
void note_d3d12_command_list_submission(ID3D12CommandQueue*, ID3D12GraphicsCommandList*) noexcept {}
void note_d3d12_command_list_reset(ID3D12GraphicsCommandList*) noexcept {}
void note_d3d12_present(ID3D12CommandQueue*) noexcept { collect_dlss_nr_input_submissions(); }
bool calibration12_internal_work() noexcept { return false; }
std::recursive_mutex& calibration12_execution_mutex() noexcept {
    static auto* mutex = new std::recursive_mutex;
    return *mutex;
}
void calibration12_submitted(ID3D12CommandQueue*, ID3D12GraphicsCommandList*) noexcept {}
void calibration12_retired(ID3D12GraphicsCommandList*) noexcept {}
bool calibration12_tagged(ID3D12GraphicsCommandList*) noexcept { return false; }
int nr_test_evaluations{};
bool nr_test_succeeds{true};
DlssNrFrame nr_test_frame{};
bool evaluate_dlss_nr(const DlssNrFrame& frame, const Settings&) noexcept {
    ++nr_test_evaluations;
    nr_test_frame = frame;
    return nr_test_succeeds && frame.color;
}
void skip_dlss_nr_history(DlssViewId) noexcept {}
void note_dlss_nr_skipped(DlssNrRoute, const Settings&, const char*) noexcept {}
}
int run_nr_lifetime_tests();
int run_d3d12_composite_tests();
namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("Observer probe fixture failed"); }
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
// Only queue creation is intercepted. Identity/private data belongs to a real
// WARP device, so this exercises the production observer cache and COM lifetime.
struct ProbeDevice {
    void** vtable;
    std::array<void*,44> methods;
    ID3D12Device* target;
    unsigned queues{}; bool fail_queue{true};
    explicit ProbeDevice(ID3D12Device* d) : vtable(methods.data()), target(d) {
        methods.fill(reinterpret_cast<void*>(&unexpected));
        methods[0]=reinterpret_cast<void*>(&query); methods[1]=reinterpret_cast<void*>(&addref); methods[2]=reinterpret_cast<void*>(&release);
        methods[3]=reinterpret_cast<void*>(&get_private); methods[5]=reinterpret_cast<void*>(&set_interface);
        methods[8]=reinterpret_cast<void*>(&queue); methods[9]=reinterpret_cast<void*>(&allocator); methods[12]=reinterpret_cast<void*>(&list);
    }
    static void STDMETHODCALLTYPE unexpected() { std::abort(); }
    static HRESULT STDMETHODCALLTYPE query(ProbeDevice* s, REFIID iid, void** out) { return s->target->QueryInterface(iid,out); }
    static ULONG STDMETHODCALLTYPE addref(ProbeDevice* s) { return s->target->AddRef(); }
    static ULONG STDMETHODCALLTYPE release(ProbeDevice* s) { return s->target->Release(); }
    static HRESULT STDMETHODCALLTYPE get_private(ProbeDevice* s, REFGUID key, UINT* size, void* data) { return s->target->GetPrivateData(key,size,data); }
    static HRESULT STDMETHODCALLTYPE set_interface(ProbeDevice* s, REFGUID key, const IUnknown* value) { return s->target->SetPrivateDataInterface(key,value); }
    static HRESULT STDMETHODCALLTYPE queue(ProbeDevice* s, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID iid, void** out) {
        ++s->queues; if (s->fail_queue) { *out=nullptr; return E_FAIL; } return s->target->CreateCommandQueue(desc,iid,out);
    }
    static HRESULT STDMETHODCALLTYPE other_queue(ProbeDevice* s, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void** out) {
        ++s->queues; *out=nullptr; return E_FAIL;
    }
    static HRESULT STDMETHODCALLTYPE allocator(ProbeDevice* s, D3D12_COMMAND_LIST_TYPE type, REFIID iid, void** out) { return s->target->CreateCommandAllocator(type,iid,out); }
    static HRESULT STDMETHODCALLTYPE list(ProbeDevice* s, UINT node, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* a, ID3D12PipelineState* p, REFIID iid, void** out) { return s->target->CreateCommandList(node,type,a,p,iid,out); }
};
struct ProbeList : TimingListAlias {
    ProbeDevice& device;
    ProbeList(ID3D12GraphicsCommandList* l, ProbeDevice& d) : TimingListAlias(l), device(d) {
        methods[7]=reinterpret_cast<void*>(&get_device);
        methods[5]=reinterpret_cast<void*>(&deny_identity);
    }
    static HRESULT STDMETHODCALLTYPE get_device(ProbeList* s, REFIID iid, void** out) {
        if (iid!=__uuidof(ID3D12Device)) return E_NOINTERFACE;
        *out=&s->device; ProbeDevice::addref(&s->device); return S_OK;
    }
    static HRESULT STDMETHODCALLTYPE deny_identity(ProbeList*, REFGUID, const IUnknown*) { return E_FAIL; }
};
// ReShade/R.E.A.L. VR and Streamline expose these interfaces to obtain the
// original COM object. The presentation queue can be wrapped while NGX uses
// native lists; installing observation on only the wrapper rejects those lists.
constexpr GUID unwrap_reshade{0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
constexpr GUID unwrap_streamline{0xadec44e2,0x61f0,0x45c3,{0xad,0x9f,0x1b,0x37,0x37,0x92,0x84,0xff}};
struct WrappedQueue {
    void** vtable;
    std::array<void*,19> methods{};
    ID3D12CommandQueue* target;
    GUID unwrap;
    WrappedQueue(ID3D12CommandQueue* q, GUID id) : vtable(methods.data()), target(q), unwrap(id) {
        methods.fill(reinterpret_cast<void*>(&ProbeDevice::unexpected));
        methods[0]=reinterpret_cast<void*>(&query); methods[1]=reinterpret_cast<void*>(&addref);
        methods[2]=reinterpret_cast<void*>(&release); methods[10]=reinterpret_cast<void*>(&execute);
    }
    static HRESULT STDMETHODCALLTYPE query(WrappedQueue* s, REFIID iid, void** out) {
        if (!out) return E_POINTER;
        *out=nullptr;
        if (iid==s->unwrap) { *out=s->target; s->target->AddRef(); return S_OK; }
        if (iid==__uuidof(IUnknown) || iid==__uuidof(ID3D12CommandQueue)) { *out=s; addref(s); return S_OK; }
        return E_NOINTERFACE;
    }
    static ULONG STDMETHODCALLTYPE addref(WrappedQueue* s) { return s->target->AddRef(); }
    static ULONG STDMETHODCALLTYPE release(WrappedQueue* s) { return s->target->Release(); }
    static void STDMETHODCALLTYPE execute(WrappedQueue* s, UINT n, ID3D12CommandList* const* lists) { s->target->ExecuteCommandLists(n,lists); }
    ID3D12CommandQueue* get() { return reinterpret_cast<ID3D12CommandQueue*>(this); }
};
struct WrappedDevice : ProbeDevice {
    GUID unwrap;
    WrappedDevice(ID3D12Device* device, GUID id) : ProbeDevice(device), unwrap(id) {
        methods[0]=reinterpret_cast<void*>(&query);
    }
    static HRESULT STDMETHODCALLTYPE query(WrappedDevice* s, REFIID iid, void** out) {
        if (!out) return E_POINTER;
        *out=nullptr;
        if (iid==s->unwrap) { *out=s->target; s->target->AddRef(); return S_OK; }
        if (iid==__uuidof(IUnknown) || iid==__uuidof(ID3D12Device)) { *out=s; addref(s); return S_OK; }
        return E_NOINTERFACE;
    }
    ID3D12Device* get() { return reinterpret_cast<ID3D12Device*>(this); }
};
struct WrappedList : TimingListAlias {
    GUID unwrap;
    WrappedList(ID3D12GraphicsCommandList* list, GUID id) : TimingListAlias(list), unwrap(id) {
        methods[0]=reinterpret_cast<void*>(&query);
        // Real tracking must resolve the native object, not rely on forwarded
        // private data or an independently hooked wrapper Reset method.
        methods[3]=methods[5]=reinterpret_cast<void*>(&unexpected);
    }
    static HRESULT STDMETHODCALLTYPE query(WrappedList* s, REFIID iid, void** out) {
        if (!out) return E_POINTER;
        *out=nullptr;
        if (iid==s->unwrap) { *out=s->target; s->target->AddRef(); return S_OK; }
        if (iid==__uuidof(IUnknown) || iid==__uuidof(ID3D12Object) || iid==__uuidof(ID3D12GraphicsCommandList)) {
            *out=s; addref(s); return S_OK;
        }
        return E_NOINTERFACE;
    }
};
// Older proxy builds forward private data but do not expose either native
// interface IID. Model the actual Hogwarts arrangement separately from the
// modern wrappers above: native presentation plus opaque NGX device/lists.
struct LegacyList : TimingListAlias {
    ProbeDevice* owner{};
    explicit LegacyList(ID3D12GraphicsCommandList* list, ProbeDevice* d) : TimingListAlias(list), owner(d) {
        references=1;
        methods[2]=reinterpret_cast<void*>(&release_owned);
        methods[7]=reinterpret_cast<void*>(&get_device);
        methods[9]=reinterpret_cast<void*>(&close);
        methods[10]=reinterpret_cast<void*>(&reset_list);
        methods[16]=reinterpret_cast<void*>(&copy_texture);
        methods[17]=reinterpret_cast<void*>(&copy_resource);
        methods[19]=reinterpret_cast<void*>(&resolve_resource);
    }
    static ULONG STDMETHODCALLTYPE release_owned(LegacyList* s) {
        const auto refs=--s->references; s->target->Release(); if(!refs) delete s; return refs;
    }
    static HRESULT STDMETHODCALLTYPE get_device(LegacyList* s, REFIID iid, void** out) {
        if(iid!=__uuidof(ID3D12Device)) return E_NOINTERFACE;
        *out=s->owner; ProbeDevice::addref(s->owner); return S_OK;
    }
    static HRESULT STDMETHODCALLTYPE close(LegacyList* s) { return s->target->Close(); }
    static HRESULT STDMETHODCALLTYPE reset_list(LegacyList* s, ID3D12CommandAllocator* a, ID3D12PipelineState* p) { return s->target->Reset(a,p); }
    static void STDMETHODCALLTYPE copy_resource(LegacyList* s, ID3D12Resource* d, ID3D12Resource* r) { s->target->CopyResource(d,r); }
    static void STDMETHODCALLTYPE copy_texture(LegacyList* s, const D3D12_TEXTURE_COPY_LOCATION* d, UINT x, UINT y, UINT z,
        const D3D12_TEXTURE_COPY_LOCATION* r, const D3D12_BOX* b) { s->target->CopyTextureRegion(d,x,y,z,r,b); }
    static void STDMETHODCALLTYPE resolve_resource(LegacyList* s, ID3D12Resource* d, UINT di, ID3D12Resource* r, UINT ri, DXGI_FORMAT f) { s->target->ResolveSubresource(d,di,r,ri,f); }
};
struct LegacyQueue : WrappedQueue {
    unsigned references{1};
    explicit LegacyQueue(ID3D12CommandQueue* queue) : WrappedQueue(queue,GUID_NULL) {
        methods[0]=reinterpret_cast<void*>(&query_owned);
        methods[2]=reinterpret_cast<void*>(&release_owned); methods[1]=reinterpret_cast<void*>(&addref_owned);
        methods[7]=reinterpret_cast<void*>(&get_device); methods[14]=reinterpret_cast<void*>(&signal);
    }
    static HRESULT STDMETHODCALLTYPE query_owned(LegacyQueue* s, REFIID id, void** out) {
        const auto hr=WrappedQueue::query(s,id,out);
        if(SUCCEEDED(hr)) ++s->references;
        return hr;
    }
    static ULONG STDMETHODCALLTYPE addref_owned(LegacyQueue* s) { ++s->references; return s->target->AddRef(); }
    static ULONG STDMETHODCALLTYPE release_owned(LegacyQueue* s) { const auto refs=--s->references; s->target->Release(); if(!refs) delete s; return refs; }
    static HRESULT STDMETHODCALLTYPE get_device(LegacyQueue* s, REFIID id, void** out) { return s->target->GetDevice(id,out); }
    static HRESULT STDMETHODCALLTYPE signal(LegacyQueue* s, ID3D12Fence* fence, UINT64 value) { return s->target->Signal(fence,value); }
};
struct LegacyDevice : ProbeDevice {
    explicit LegacyDevice(ID3D12Device* device) : ProbeDevice(device) {
        methods[8]=reinterpret_cast<void*>(&create_queue); methods[12]=reinterpret_cast<void*>(&create_list);
    }
    static HRESULT STDMETHODCALLTYPE create_queue(LegacyDevice* s, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID id, void** out) {
        ++s->queues; ID3D12CommandQueue* q{}; const auto hr=s->target->CreateCommandQueue(desc,id,reinterpret_cast<void**>(&q));
        if(SUCCEEDED(hr)) *out=(new LegacyQueue(q))->get(); return hr;
    }
    static HRESULT STDMETHODCALLTYPE create_list(LegacyDevice* s, UINT node, D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator* a, ID3D12PipelineState* p, REFIID id, void** out) {
        ID3D12GraphicsCommandList* list{}; const auto hr=s->target->CreateCommandList(node,type,a,p,id,reinterpret_cast<void**>(&list));
        if(SUCCEEDED(hr)) *out=(new LegacyList(list,s))->get(); return hr;
    }
};
// An opaque interposer owns the interpretation of its copy arguments. It may
// accept a sentinel which is not a COM resource, or rewrite caller-owned copy
// locations while forwarding. Do not submit these synthetic calls to WARP.
struct MetadataList : LegacyList {
    unsigned calls{}, kind{};
    void (*on_copy)(MetadataList*){};
    D3D12_TEXTURE_COPY_LOCATION* rewrite_location{};
    D3D12_BOX* rewrite_box{};
    void* copy_context{};
    explicit MetadataList(ID3D12GraphicsCommandList* list, ProbeDevice* device) : LegacyList(list, device) {
        methods[16] = reinterpret_cast<void*>(&copy_texture);
        methods[17] = reinterpret_cast<void*>(&copy_resource);
        methods[19] = reinterpret_cast<void*>(&resolve_resource);
    }
    static void copied(MetadataList* s) { ++s->calls; if (s->on_copy) s->on_copy(s); }
    static void STDMETHODCALLTYPE copy_resource(MetadataList* s, ID3D12Resource*, ID3D12Resource*) { s->kind = 1; copied(s); }
    static void STDMETHODCALLTYPE copy_texture(MetadataList* s, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
        const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*) { s->kind = 2; copied(s); }
    static void STDMETHODCALLTYPE resolve_resource(MetadataList* s, ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT) { s->kind = 3; copied(s); }
};
struct MetadataDevice : LegacyDevice {
    explicit MetadataDevice(ID3D12Device* d) : LegacyDevice(d) { methods[12] = reinterpret_cast<void*>(&create_list); }
    static HRESULT STDMETHODCALLTYPE create_list(MetadataDevice* s, UINT node, D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator* a, ID3D12PipelineState* p, REFIID id, void** out) {
        ID3D12GraphicsCommandList* list{};
        const auto hr = s->target->CreateCommandList(node, type, a, p, id, reinterpret_cast<void**>(&list));
        if (SUCCEEDED(hr)) *out = (new MetadataList(list, s))->get();
        return hr;
    }
};
struct MetadataResource {
    void** vtable;
    std::array<void*, 15> methods{};
    ID3D12Resource* target;
    unsigned references{}, fault_slot{UINT_MAX};
    explicit MetadataResource(ID3D12Resource* r) : vtable(methods.data()), target(r) {
        methods.fill(reinterpret_cast<void*>(&ProbeDevice::unexpected));
        methods[0] = reinterpret_cast<void*>(&query); methods[1] = reinterpret_cast<void*>(&addref);
        methods[2] = reinterpret_cast<void*>(&release); methods[3] = reinterpret_cast<void*>(&get_private);
        methods[5] = reinterpret_cast<void*>(&set_interface); methods[10] = reinterpret_cast<void*>(&get_desc);
    }
    void fault(unsigned slot) { if (fault_slot == slot) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr); }
    static HRESULT STDMETHODCALLTYPE query(MetadataResource* s, REFIID id, void** out) {
        s->fault(0); *out = nullptr;
        if (id != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = s; addref(s); return S_OK;
    }
    static ULONG STDMETHODCALLTYPE addref(MetadataResource* s) { return ++s->references; }
    static ULONG STDMETHODCALLTYPE release(MetadataResource* s) { return --s->references; }
    static HRESULT STDMETHODCALLTYPE get_private(MetadataResource* s, REFGUID key, UINT* size, void* data) {
        s->fault(3); return s->target->GetPrivateData(key, size, data);
    }
    static HRESULT STDMETHODCALLTYPE set_interface(MetadataResource* s, REFGUID key, const IUnknown* value) {
        s->fault(5); return s->target->SetPrivateDataInterface(key, value);
    }
    // COM's x64 aggregate-return ABI passes the destination after this.
    static D3D12_RESOURCE_DESC* STDMETHODCALLTYPE get_desc(MetadataResource* s, D3D12_RESOURCE_DESC* out) {
        s->fault(10); *out = s->target->GetDesc(); return out;
    }
    ID3D12Resource* get() { return reinterpret_cast<ID3D12Resource*>(this); }
};
// Catch only in the fixture so the unfixed observer reports a test failure
// instead of opening Windows Error Reporting. Production must handle this.
DWORD copy_exception(void (*call)(void*), void* context) {
    __try { call(context); return 0; }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return GetExceptionCode();
    }
}
int run_copy_metadata_tests() {
    using namespace cheeky::foveated_dlss;
    try {
        Settings settings; settings.auto_stereo_alignment = true; update_settings(settings);
        ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> warp; check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ComPtr<ID3D12Device> device; check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        MetadataDevice facade(device.Get());
        ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;
        check(MetadataDevice::create_list(&facade, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(list.GetAddressOf())));
        require(ensure_native_observer(list.Get()), "Metadata observer initialization failed");
        auto* proxy = reinterpret_cast<MetadataList*>(list.Get());
        // The noncanonical source address is the value in issue #45's dump.
        auto* invalid = reinterpret_cast<ID3D12Resource*>(0x7ffffffffffffffcULL);
        struct Call { ID3D12GraphicsCommandList* list; ID3D12Resource* resource; } call{list.Get(), invalid};
        const auto fault = copy_exception([](void* p) {
            auto& c = *static_cast<Call*>(p); c.list->CopyResource(c.resource, c.resource);
        }, &call);
        require(!fault, "CopyResource metadata inspection raised an access violation (issue #45)");
        require(proxy->calls == 1 && observed_copy_count == 0, "Unreadable metadata changed forwarding or recorded an edge");
        D3D12_TEXTURE_COPY_LOCATION bad_location{}; bad_location.pResource = invalid;
        list->CopyTextureRegion(&bad_location, 0, 0, 0, &bad_location, nullptr);
        list->ResolveSubresource(invalid, 0, invalid, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        require(proxy->calls == 3 && observed_copy_count == 0, "Invalid texture/resolve metadata changed forwarding");

        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 64; desc.Height = 32; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> source, destination;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&source)));
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&destination)));
        list->CopyResource(destination.Get(), source.Get());
        require(observed_copy_count == 1 && observed_copy_edge.source.width == 64 && observed_copy_edge.source.height == 32,
            "A skipped copy prevented subsequent valid observation");
        list->ResolveSubresource(destination.Get(), 0, source.Get(), 0, desc.Format);
        require(observed_copy_count == 2, "Valid resolve observation was lost");
        D3D12_TEXTURE_COPY_LOCATION from{}, to{}; from.pResource = source.Get(); to.pResource = destination.Get();
        D3D12_BOX box{2, 3, 0, 12, 15, 1};
        // The original sees the old count: recording must remain post-call.
        proxy->on_copy = [](MetadataList*) { require(observed_copy_count == 2, "Copy edge was published before forwarding"); };
        list->CopyTextureRegion(&to, 5, 6, 0, &from, &box);
        proxy->on_copy = nullptr;
        require(observed_copy_count == 3 && observed_copy_edge.source.x == 2 && observed_copy_edge.source.y == 3 &&
            observed_copy_edge.source.width == 10 && observed_copy_edge.source.height == 12 &&
            observed_copy_edge.destination.x == 5 && observed_copy_edge.destination.y == 6, "Texture copy bounds changed");
        proxy->rewrite_location = &from; proxy->rewrite_box = &box;
        proxy->on_copy = [](MetadataList* p) {
            p->rewrite_location->pResource = reinterpret_cast<ID3D12Resource*>(0x7ffffffffffffffcULL);
            *p->rewrite_box = {};
        };
        list->CopyTextureRegion(&to, 5, 6, 0, &from, &box);
        proxy->on_copy = nullptr;
        require(observed_copy_count == 4 && observed_copy_edge.source.width == 10 && observed_copy_edge.source.height == 12,
            "Observer reread texture locations or bounds after forwarding");
        // Reject unreadable location/box memory as well as unreadable resources.
        auto* bad = reinterpret_cast<D3D12_TEXTURE_COPY_LOCATION*>(invalid);
        list->CopyTextureRegion(&to, 0, 0, 0, bad, nullptr);
        list->CopyTextureRegion(&to, 0, 0, 0, &to, reinterpret_cast<D3D12_BOX*>(invalid));
        require(observed_copy_count == 4, "Unreadable texture locations produced an edge");
        ComPtr<ID3D12Resource> probe_resource;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&probe_resource)));
        MetadataResource metadata(probe_resource.Get());
        for (const auto slot : {10U, 0U, 3U, 5U}) {
            metadata.fault_slot = slot;
            list->CopyResource(destination.Get(), metadata.get());
            require(observed_copy_count == 4 && !metadata.references, "Failed metadata probe published an edge or leaked its COM reference");
        }
        metadata.fault_slot = UINT_MAX;
        proxy->on_copy = [](MetadataList*) { require(observed_copy_count == 4, "Resource copy was published before forwarding"); };
        list->CopyResource(destination.Get(), metadata.get());
        proxy->on_copy = nullptr;
        require(observed_copy_count == 5 && !metadata.references, "Valid proxy metadata did not recover or leaked a reference");
        proxy->copy_context = &metadata;
        proxy->on_copy = [](MetadataList* p) {
            auto& resource = *static_cast<MetadataResource*>(p->copy_context);
            require(resource.references != 0, "Metadata identity was not retained across forwarding");
            resource.fault_slot = 10;
        };
        list->CopyResource(destination.Get(), metadata.get());
        metadata.fault_slot = UINT_MAX;
        list->ResolveSubresource(destination.Get(), 0, metadata.get(), 0, desc.Format);
        proxy->on_copy = nullptr;
        require(observed_copy_count == 7 && !metadata.references, "Copy/resolve reread descriptors after forwarding or leaked a reference");
        const auto original_calls = proxy->calls;
        proxy->on_copy = [](MetadataList*) { RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr); };
        call.resource = source.Get();
        const auto original_fault = copy_exception([](void* p) {
            auto& c = *static_cast<Call*>(p); c.list->CopyResource(c.resource, c.resource);
        }, &call);
        require(original_fault == EXCEPTION_ACCESS_VIOLATION && proxy->calls == original_calls + 1 && observed_copy_count == 7,
            "Observer swallowed an original graphics fault or published failed work");
        check(list->Close());
        std::cout << "PASS optional copy metadata faults, forwarding, recovery and copy/resolve bounds\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL copy metadata: " << e.what() << '\n'; return 1; }
}
int run_legacy_tests() {
    using namespace cheeky::foveated_dlss;
    try {
        ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> warp; check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ComPtr<ID3D12Device> device; check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC desc{}; ComPtr<ID3D12CommandQueue> queue; check(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)));
        require(initialize_native_observer(device.Get(),queue.Get()),"Native presentation observer initialization failed");
        LegacyDevice legacy(device.Get());
        ComPtr<ID3D12CommandQueue> opaque_queue; check(LegacyDevice::create_queue(&legacy,&desc,__uuidof(ID3D12CommandQueue),reinterpret_cast<void**>(opaque_queue.GetAddressOf())));
        ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list; check(LegacyDevice::create_list(&legacy,0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,__uuidof(ID3D12GraphicsCommandList),reinterpret_cast<void**>(list.GetAddressOf())));
        require(ensure_dlss_nr_recording(list.Get()),"Native observer rejects opaque VR command-list family");
        require(legacy.queues==2,"Opaque observer probe did not initialize exactly once");
        NrLifetime lifetime; require(lifetime.record(list.Get()),"Opaque VR recording could not retain resources");
        require(legacy.queues==2,"Opaque observer repeats successful graphics-object creation");
        ComPtr<ID3D12Fence> block; check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&block)));
        check(queue->Wait(block.Get(),1)); check(list->Close());
        // The opaque queue receives an already-native list here, as VR runtimes
        // may do. Both hook layers run, but submission accounting must run once.
        auto* native_list=reinterpret_cast<LegacyList*>(list.Get())->target;
        ID3D12CommandList* lists[]{native_list}; const auto before=native_observer_status();
        opaque_queue->ExecuteCommandLists(1,lists); check(list->Reset(allocator.Get(),nullptr));
        const auto after=native_observer_status();
        require(after.submissions==before.submissions+1 && after.resets==before.resets+1,"Nested proxy/native hooks double-counted submission or reset");
        lifetime.collect(); const bool held=!lifetime.empty(); check(block->Signal(1));
        require(held,"Opaque VR submission released resources before GPU completion");
        for(unsigned i=0;i<200 && !lifetime.empty();++i){Sleep(5);lifetime.collect();}
        require(lifetime.empty(),"Opaque VR/native identity did not retire completed GPU work");
        check(list->Close());
        std::cout<<"PASS opaque VR/native observer families, single submission/reset, GPU lifetime\n"; return 0;
    } catch(const std::exception& e){std::cerr<<"FAIL opaque observer: "<<e.what()<<'\n';return 1;}
}
int run_wrapped_tests(bool streamline) {
    using namespace cheeky::foveated_dlss;
    try {
        ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> warp; check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ComPtr<ID3D12Device> device; check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC desc{};
        ComPtr<ID3D12CommandQueue> queue; check(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)));
        WrappedQueue wrapped(queue.Get(), streamline ? unwrap_streamline : unwrap_reshade);
        WrappedDevice wrapped_device(device.Get(),streamline ? unwrap_streamline : unwrap_reshade);
        require(initialize_native_observer(wrapped_device.get(),wrapped.get()),"Wrapped presentation observer initialization failed");
        require(wrapped_device.queues==0,"Observer probed the proxy factory instead of its native device");
        ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list; check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        require(ensure_dlss_nr_recording(list.Get()),"Wrapped presentation queue rejects native NGX recording identity");
        WrappedList inner(list.Get(),unwrap_reshade), outer(inner.get(),unwrap_streamline);
        require(ensure_dlss_nr_recording(outer.get()),"Nested wrapped NGX recording identity failed");
        NrLifetime lifetime; require(lifetime.record(outer.get()),"Wrapped NGX recording could not retain resources");
        ComPtr<ID3D12Fence> block; check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&block)));
        check(queue->Wait(block.Get(),1));
        check(list->Close()); ID3D12CommandList* lists[]{list.Get()}; wrapped.get()->ExecuteCommandLists(1,lists);
        check(list->Reset(allocator.Get(),nullptr));
        lifetime.collect(); require(!lifetime.empty(),"Wrapped submission released resources before GPU completion");
        check(block->Signal(1));
        for(unsigned i=0;i<200 && !lifetime.empty();++i){ Sleep(5); lifetime.collect(); }
        require(lifetime.empty(),"Native observer did not retire wrapped queue submission");
        require(inner.references==0 && outer.references==0,"Unwrapping leaked COM references");
        check(list->Close());
        std::cout << "PASS wrapped presentation/native NGX lifetime\n"; return 0;
    } catch(const std::exception& e){ std::cerr << "FAIL wrapped observer: " << e.what() << '\n'; return 1; }
}
// A game thread can hold the execution mutex across a forwarding wrapper's
// Execute that blocks on another thread (R.E.A.L. VR waits for its VR thread).
// That thread's own D3D12 work - an OpenXR layer's private device - reaches the
// same process-wide hooks and must not wait for the mutex. Recorded NR work
// still must.
int run_foreign_list_tests() {
    using namespace cheeky::foveated_dlss;
    try {
        ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> warp; check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ComPtr<ID3D12Device> device; check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC desc{}; ComPtr<ID3D12CommandQueue> queue; check(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)));
        require(initialize_native_observer(device.Get(),queue.Get()),"Native observer initialization failed");
        ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> foreign; check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&foreign)));
        check(foreign->Close());
        ComPtr<ID3D12CommandAllocator> nr_allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&nr_allocator)));
        ComPtr<ID3D12GraphicsCommandList> recorded; check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,nr_allocator.Get(),nullptr,IID_PPV_ARGS(&recorded)));
        require(ensure_dlss_nr_recording(recorded.Get()),"NR recording identity failed");
        check(recorded->Close());

        std::promise<void> held, release;
        auto release_signal=release.get_future().share();
        std::thread holder([&]{
            std::lock_guard lock(calibration12_execution_mutex());
            held.set_value();
            release_signal.wait();
        });
        held.get_future().wait();
        auto foreign_work=std::async(std::launch::async,[&]{
            check(allocator->Reset());
            check(foreign->Reset(allocator.Get(),nullptr)); check(foreign->Close());
            ID3D12CommandList* lists[]{foreign.Get()}; queue->ExecuteCommandLists(1,lists);
        });
        const bool foreign_done=foreign_work.wait_for(std::chrono::seconds(5))==std::future_status::ready;
        auto recorded_work=std::async(std::launch::async,[&]{ check(recorded->Reset(nr_allocator.Get(),nullptr)); });
        const bool recorded_waited=recorded_work.wait_for(std::chrono::milliseconds(200))==std::future_status::timeout;
        release.set_value(); holder.join(); foreign_work.get(); recorded_work.get();
        check(recorded->Close());
        require(foreign_done,"Untracked list Reset/Execute waited for a mutex held by another thread");
        require(recorded_waited,"NR-recorded list Reset no longer serializes with the execution mutex");
        std::cout<<"PASS untracked lists bypass the execution mutex, recorded lists keep it\n"; return 0;
    } catch(const std::exception& e){std::cerr<<"FAIL foreign list: "<<e.what()<<'\n';return 1;}
}
int run_probe_tests() {
    using namespace cheeky::foveated_dlss;
    try {
        wchar_t no_debug[2]{};
        ComPtr<ID3D12Debug> debug;
        if (!(GetEnvironmentVariableW(L"CHEEKY_NR_TEST_NO_DEBUG_LAYER",no_debug,2)==1 && no_debug[0]==L'1') &&
            SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> warp; check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ComPtr<ID3D12Device> device; check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
        ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list; check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        ProbeDevice probe(device.Get()); ProbeList alias(list.Get(),probe);
        for (unsigned i=0; i<512; ++i) require(!ensure_dlss_nr_recording(alias.get()),"Failed observer unexpectedly accepted recording");
        std::cout << "Observer failed-probe queue creations: " << probe.queues << '\n';
        require(probe.queues==1,"Repeated failed NR setup recreated a command queue on every attempt");
        Sleep(1100); // Production cooldown must retry a transient failure.
        require(!ensure_dlss_nr_recording(alias.get()) && probe.queues==2,"Observer failure never retried after cooldown");
        probe.fail_queue=false;
        Sleep(1100);
        for (unsigned i=0; i<512; ++i) require(!ensure_dlss_nr_recording(alias.get()),"Failed private-data identity unexpectedly accepted");
        require(probe.queues==3,"Successful observer probes repeated allocations when recording identity failed");
        alias.methods[5]=reinterpret_cast<void*>(&TimingListAlias::set_interface);
        require(ensure_dlss_nr_recording(alias.get()),"Observer did not recover after identity became available");
        require(probe.queues==3,"Recovered recording unnecessarily rebuilt observer graphics objects");
        probe.methods[8]=reinterpret_cast<void*>(&ProbeDevice::other_queue);
        for (unsigned i=0; i<64; ++i) require(!ensure_native_observer(alias.get()),"Observer reused success across different device factory methods");
        require(probe.queues==4,"Alternate device factory failures were not cached");
        probe.methods[8]=reinterpret_cast<void*>(&ProbeDevice::queue);
        require(ensure_native_observer(alias.get()) && probe.queues==4,"Alternate factory invalidated the working factory's cache");
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC color_desc{}; color_desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        color_desc.Width=color_desc.Height=32; color_desc.DepthOrArraySize=color_desc.MipLevels=color_desc.SampleDesc.Count=1;
        color_desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        ComPtr<ID3D12Resource> color;
        check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&color_desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&color)));
        Settings nr_settings{}; nr_settings.nr_enabled=true;
        nr_settings.nr_processing_order=NrProcessingOrder::before_upscaling;
        DlssNrFrame nr_frame{}; nr_frame.view_id=509; nr_frame.command_list=list.Get(); nr_frame.color=color.Get();
        nr_frame.color_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        nr_frame.input_width=nr_frame.input_height=nr_frame.output_width=nr_frame.output_height=32;
        const auto attempts=nr_test_evaluations;
        require(prepare_dlss_nr_input(nr_frame,nr_settings)==nullptr && nr_test_evaluations==attempts,
            "Pre-attach list without allocator ownership consumed an input slot");
        check(list->Close());
        check(list->Reset(allocator.Get(),nullptr));
        require(nr_recording_allocator_reset_target(alias.get())!=nullptr,
            "Pre-attach list did not acquire allocator ownership after observed Reset");
        require(prepare_dlss_nr_input(nr_frame,nr_settings)!=nullptr,
            "Pre-attach list did not resume NR after observed Reset bound its allocator");
        check(list->Close());
        std::cout << "PASS observer retry/cache/identity recovery\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL observer probe: " << e.what() << '\n'; return 1; }
}
int run_allocator_tests() {
    using namespace cheeky::foveated_dlss;
    try {
        ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> warp; check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ComPtr<ID3D12Device> device; check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
        ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC q{};
        check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        require(initialize_native_observer(device.Get(),queue.Get()), "Allocator observer initialization failed");
        ComPtr<ID3D12CommandAllocator> allocator, unrelated, next_allocator;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&unrelated)));
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&next_allocator)));
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=desc.Height=32; desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        ComPtr<ID3D12Resource> color;
        check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&color)));
        Settings settings{}; settings.nr_enabled=true; settings.nr_processing_order=NrProcessingOrder::before_upscaling;
        update_settings(settings);
        DlssNrFrame frame{}; frame.view_id=506; frame.color=color.Get();
        frame.color_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        frame.input_width=frame.input_height=frame.output_width=frame.output_height=32;
        std::array<ComPtr<ID3D12GraphicsCommandList>,4> lists;
        std::array<ID3D12CommandList*,4> submitted{};
        NrLifetime first;
        for (unsigned i=0;i<lists.size();++i) {
            check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&lists[i])));
            frame.command_list=lists[i].Get();
            require(prepare_dlss_nr_input(frame,settings)!=nullptr,"Could not fill four allocator-backed inputs");
            if(i==0)require(first.record(lists[i].Get()),"Allocator lifetime tracking failed");
            check(lists[i]->Close()); submitted[i]=lists[i].Get();
        }
        ComPtr<ID3D12Fence> done; check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)));
        queue->ExecuteCommandLists(static_cast<UINT>(submitted.size()),submitted.data());
        check(queue->Signal(done.Get(),1));
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        check(done->SetEventOnCompletion(1,event));
        const auto waited=WaitForSingleObject(event,10000); CloseHandle(event);
        require(waited==WAIT_OBJECT_0,"Allocator test GPU timeout");
        first.collect();
        require(!first.empty() && first.status().allocator_unknown==0 && first.status().completed_recordings==1,
            "Completed recording lost its allocator association or replay protection");
        ComPtr<ID3D12GraphicsCommandList> overflow;
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,next_allocator.Get(),nullptr,IID_PPV_ARGS(&overflow)));
        frame.command_list=overflow.Get();
        require(prepare_dlss_nr_input(frame,settings)==nullptr,"Pool did not retain four completed replayable inputs");
        nr_allocator_reset(allocator.Get(),E_FAIL); first.collect();
        require(!first.empty(),"Failed allocator Reset retired a recording");
        check(unrelated->Reset()); first.collect();
        require(!first.empty(),"Unrelated allocator Reset retired another allocator's recording");
        check(allocator->Reset()); first.collect();
        require(first.empty(),"Allocator Reset retained completed recording while list objects remained alive");
        require(prepare_dlss_nr_input(frame,settings)!=nullptr,
            "Four cached closed list objects still exhausted the input pool after allocator Reset");
        check(overflow->Close());
        // Rebinding an existing list to new allocator storage must not inherit
        // the retired epoch. No additional game list object is required.
        check(lists[0]->Reset(allocator.Get(),nullptr));
        require(first.record(lists[0].Get()),"List Reset did not bind a fresh allocator epoch");
        check(lists[0]->Close());
        nr_allocator_reset(allocator.Get(),E_FAIL); first.collect();
        require(!first.empty(),"Failed Reset discarded a fresh unsubmitted epoch");
        check(allocator->Reset()); first.collect();
        require(first.empty(),"Allocator Reset did not discard unsubmitted command storage");
        settings.nr_enabled=false; update_settings(settings);
        lists={}; overflow.Reset(); collect_dlss_nr_input_submissions();
        std::cout<<"PASS allocator Reset: four completed cached lists reclaim inputs; failed/unrelated Reset protected; fresh and discarded epochs passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL allocator retirement: "<<e.what()<<'\n';return 1; }
}

}
int main(int argc, char** argv) {
    if (argc==2 && std::strcmp(argv[1],"--copy-metadata")==0) return run_copy_metadata_tests();
    if (argc==2 && std::strcmp(argv[1],"--opaque-vr")==0) return run_legacy_tests();
    if (argc==2 && std::strcmp(argv[1],"--wrapped-reshade")==0) return run_wrapped_tests(false);
    if (argc==2 && std::strcmp(argv[1],"--wrapped-streamline")==0) return run_wrapped_tests(true);
    if (argc==2 && std::strcmp(argv[1],"--allocator")==0) return run_allocator_tests();
    if (argc==2 && std::strcmp(argv[1],"--probe")==0) return run_probe_tests();
    if (argc==2 && std::strcmp(argv[1],"--foreign-list")==0) return run_foreign_list_tests();
    cheeky::foveated_dlss::Settings settings;
    settings.enabled = false;
    settings.nr_enabled = false;
    settings.auto_stereo_alignment = false;
    cheeky::foveated_dlss::update_settings(settings);
    const auto probe = run_probe_tests();
    if (probe) return probe;
    const auto foreign = run_foreign_list_tests();
    if (foreign) return foreign;
    const auto allocator = run_allocator_tests();
    if (allocator) return allocator;
    const auto lifetime = run_nr_lifetime_tests();
    return lifetime ? lifetime : run_d3d12_composite_tests();
}
