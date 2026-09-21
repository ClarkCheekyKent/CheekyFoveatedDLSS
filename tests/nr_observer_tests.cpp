#include "dlss_nr_input.hpp"
#include "eye_calibration_d3d12.hpp"
#include "gaze_foveation.hpp"
#include "runtime.hpp"
#include "dlss_nr_lifetime.hpp"
#include "graphics_observer.hpp"
#include "timing_list_alias.hpp"
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
