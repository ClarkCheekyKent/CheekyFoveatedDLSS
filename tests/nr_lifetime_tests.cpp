#include "dlss_nr_lifetime.hpp"
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace cheeky::foveated_dlss;
using Microsoft::WRL::ComPtr;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void check(HRESULT result) { require(SUCCEEDED(result), "D3D12 lifetime fixture failed"); }
HRESULT fail_signal(ID3D12CommandQueue*, ID3D12Fence*, std::uint64_t) { return E_FAIL; }
struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> done, gate;
    ComPtr<ID3D12InfoQueue> messages;
    std::uint64_t value{};
    HANDLE event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    Gpu() {
        ComPtr<ID3D12Debug> debug;
        check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> warp;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        check(device.As(&messages));
        D3D12_COMMAND_QUEUE_DESC desc{};
        check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        check(list->Close());
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)));
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    }
    ~Gpu() { gate->Signal(UINT64_MAX); CloseHandle(event); }
    void execute() { ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists); }
    void wait() {
        check(queue->Signal(done.Get(), ++value));
        check(done->SetEventOnCompletion(value, event));
        require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "GPU completion timed out");
    }
    void validate() {
        for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
            SIZE_T size{};
            check(messages->GetMessage(i, nullptr, &size));
            std::vector<char> data(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
            check(messages->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::cerr << message->pDescription << '\n';
                throw std::runtime_error("D3D12 debug-layer error");
            }
        }
    }
};
}
int run_nr_lifetime_tests() {
    try {
        Gpu gpu;
        // Two stable view owners, with no cache eviction or retirement required
        // to reclaim completed fences. This is the production bookkeeping,
        // independent of the NVIDIA evaluator stub used by input-copy tests.
        std::array<NrLifetime, 2> views;
        for (unsigned cycle = 0; cycle < 1000; ++cycle) {
            for (auto& view : views) {
                require(view.record(gpu.list.Get()), "Could not record NR use");
                require(view.record(gpu.list.Get()) && view.size() == 1, "Duplicate recording retained a fence");
            }
            gpu.execute();
            for (auto& view : views) view.submitted(gpu.queue.Get(), gpu.list.Get(), true);
            gpu.wait();
            for (auto& view : views) {
                view.collect();
                require(view.empty(), "Stable active view leaked completed use");
                require(view.fences_created() == cycle + 1 && view.fences_released() == cycle + 1,
                    "Fence ownership did not drain exactly once");
                view.collect();
                require(view.fences_released() == cycle + 1, "Completed fence released twice");
            }
        }
        NrLifetime retired;
        require(retired.record(gpu.list.Get()), "Could not record retiring view");
        retired.collect();
        require(!retired.empty(), "Unsubmitted retired work released");
        check(gpu.queue->Wait(gpu.gate.Get(), 1));
        gpu.execute();
        retired.submitted(gpu.queue.Get(), gpu.list.Get(), false);
        retired.signal_pending(fail_signal);
        retired.collect();
        require(!retired.empty(), "Failed signaling released retired work");
        retired.signal_pending();
        retired.collect();
        require(!retired.empty(), "Blocked GPU work released");
        check(gpu.gate->Signal(1));
        gpu.wait();
        retired.collect();
        require(retired.empty() && retired.fences_released() == 1, "Retired view did not drain");
        gpu.validate();
        std::cout << "NR production lifetime: 1000 two-view cycles, pending, failed signaling, retirement passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "NR lifetime: " << error.what() << '\n';
        return 1;
    }
}
