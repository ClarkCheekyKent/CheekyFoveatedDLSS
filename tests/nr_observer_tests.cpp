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
#include <iostream>
#include <stdexcept>

// This executable links the real native observer and NR lifetime/input code.
// Unrelated gaze/timing/calibration consumers are inert; NVIDIA evaluation is
// substituted only for the copy test. Lifetime assertions use NrLifetime itself.
namespace cheeky::foveated_dlss {
void log_info(const char*) noexcept {}
void log_warning(const char*) noexcept {}
void log_error(const char*) noexcept {}
void trace_event(const char*, ...) noexcept {}
void record_gaze_copy(std::uint64_t, GazeCopyEdge) noexcept {}
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
int nr_test_evaluations{};
bool nr_test_succeeds{true};
DlssNrFrame nr_test_frame{};
bool evaluate_dlss_nr(const DlssNrFrame& frame, const Settings&) noexcept {
    ++nr_test_evaluations;
    nr_test_frame = frame;
    return nr_test_succeeds && frame.color;
}
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
        check(list->Close());
        std::cout << "PASS observer retry/cache/identity recovery\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL observer probe: " << e.what() << '\n'; return 1; }
}
}
int main(int argc, char** argv) {
    if (argc==2 && std::strcmp(argv[1],"--opaque-vr")==0) return run_legacy_tests();
    if (argc==2 && std::strcmp(argv[1],"--wrapped-reshade")==0) return run_wrapped_tests(false);
    if (argc==2 && std::strcmp(argv[1],"--wrapped-streamline")==0) return run_wrapped_tests(true);
    if (argc==2 && std::strcmp(argv[1],"--probe")==0) return run_probe_tests();
    cheeky::foveated_dlss::Settings settings;
    settings.enabled = false;
    settings.nr_enabled = false;
    settings.auto_stereo_alignment = false;
    cheeky::foveated_dlss::update_settings(settings);
    const auto probe = run_probe_tests();
    if (probe) return probe;
    const auto lifetime = run_nr_lifetime_tests();
    return lifetime ? lifetime : run_d3d12_composite_tests();
}
