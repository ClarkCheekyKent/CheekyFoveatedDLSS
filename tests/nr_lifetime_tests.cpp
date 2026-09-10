#include "dlss_nr_lifetime.hpp"
#include "timing_list_alias.hpp"
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
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
HRESULT STDMETHODCALLTYPE reject_interface(TimingListAlias*, REFGUID, const IUnknown*) { return E_NOINTERFACE; }
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
    void execute(ID3D12CommandQueue* target = nullptr) {
        if (!target) target = queue.Get();
        ID3D12CommandList* lists[]{list.Get()};
        target->ExecuteCommandLists(1, lists);
#ifndef CHEEKY_NR_NATIVE_OBSERVER
        nr_recording_submitted(target, list.Get());
#endif
    }
    void reset() {
        // The list may Reset while GPU work runs, using a fresh allocator.
        ComPtr<ID3D12CommandAllocator> next;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&next)));
        check(list->Reset(next.Get(), nullptr));
#ifndef CHEEKY_NR_NATIVE_OBSERVER
        nr_recording_reset(list.Get(), S_OK);
#endif
        allocator = next;
        check(list->Close());
    }
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
            gpu.wait();
            for (auto& view : views) {
                view.collect();
                require(view.size() == 1, "Completion retired a replayable recording");
                require(view.fences_created() == cycle + 1 && view.fences_released() == cycle + 1,
                    "Active recording did not collect completed fence points");
            }
            gpu.reset();
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
        nr_recording_submitted(gpu.queue.Get(), gpu.list.Get(), fail_signal);
        retired.collect(fail_signal);
        require(!retired.empty(), "Failed signaling released retired work");
        retired.collect();
        require(!retired.empty(), "Blocked GPU work released");
        check(gpu.gate->Signal(1));
        gpu.wait();
        gpu.reset();
        retired.collect();
        require(retired.empty() && retired.fences_released() == 1, "Retired view did not drain");
        NrLifetime failed;
        require(failed.record(gpu.list.Get()), "Failed signal recording failed");
        gpu.execute(); gpu.wait();
        nr_recording_submitted(gpu.queue.Get(), gpu.list.Get(), fail_signal);
        gpu.reset(); failed.collect(fail_signal);
        require(!failed.empty(), "Retirement and an older completion bypassed a failed signal");
        failed.collect(); gpu.wait(); failed.collect();
        require(failed.empty(), "Retired failed signal did not recover after successful retry");
        // Replay after completion, including a second independently blocked
        // queue. The first queue completing a newer signal cannot free it.
        NrLifetime replay;
        TimingListAlias alias(gpu.list.Get());
        require(replay.record(alias.get()) && replay.record(gpu.list.Get()) && replay.size() == 1,
            "Wrapper/native aliases did not share a recording");
        require(alias.references == 0, "Recording retained the command-list wrapper");
        gpu.execute(); gpu.wait(); replay.collect();
        require(!replay.empty(), "First execution made replay resources reusable");
        ComPtr<ID3D12CommandQueue> second;
        D3D12_COMMAND_QUEUE_DESC desc{};
        check(gpu.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&second)));
        check(second->Wait(gpu.gate.Get(), 2));
        gpu.execute(second.Get());
        gpu.execute(); gpu.wait();
        replay.collect();
        require(replay.fences_created() - replay.fences_released() == 1,
            "Independent queue completion was lost");
        nr_recording_reset(alias.get(), E_FAIL);
        replay.collect();
        require(!replay.empty(), "Failed Reset retired the recording");
        auto pending_allocator = gpu.allocator;
        gpu.reset(); replay.collect();
        require(!replay.empty(), "Reset released work on the blocked replay queue");
        check(gpu.gate->Signal(2));
        check(second->Signal(gpu.done.Get(), ++gpu.value));
        check(gpu.done->SetEventOnCompletion(gpu.value, gpu.event));
        require(WaitForSingleObject(gpu.event, 10000) == WAIT_OBJECT_0, "Replay queue timed out");
        replay.collect();
        require(replay.empty() && replay.fences_created() == replay.fences_released(),
            "Retired multi-queue recording did not drain");
        NrLifetime discarded;
        require(discarded.record(gpu.list.Get()), "Discard recording failed");
        nr_recording_reset(gpu.list.Get(), E_FAIL);
        discarded.collect();
        require(!discarded.empty(), "Failed Reset discarded unsubmitted work");
        gpu.reset(); discarded.collect();
        require(discarded.empty() && discarded.fences_created() == 0,
            "Successful Reset failed to discard unsubmitted work");
        NrLifetime destroyed;
        require(destroyed.record(gpu.list.Get()), "Destruction recording failed");
        check(gpu.queue->Wait(gpu.gate.Get(), 3));
        gpu.execute();
        // No list reference in NR: destruction retires its generation even
        // while the queue still owns pending execution requirements.
        gpu.list.Reset(); destroyed.collect();
        require(!destroyed.empty(), "Destruction released pending GPU work");
        check(gpu.gate->Signal(3)); gpu.wait(); destroyed.collect();
        require(destroyed.empty(), "Destruction notification failed to retire NR work");
        ComPtr<ID3D12GraphicsCommandList> unused;
        check(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(),
            nullptr, IID_PPV_ARGS(&unused)));
        TimingListAlias unsupported(unused.Get());
        unsupported.methods[5] = reinterpret_cast<void*>(&reject_interface);
        require(!destroyed.record(unsupported.get()), "Unsupported recording identity was accepted");
        require(destroyed.record(unused.Get()), "Unsubmitted destruction recording failed");
        check(unused->Close()); unused.Reset(); destroyed.collect();
        require(destroyed.empty(), "Destruction retained discarded resources");
        gpu.validate();
        std::cout << "NR production lifetime: 1000 two-view cycles, replay, multi-queue, Reset, aliases, destruction, failed signaling passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "NR lifetime: " << error.what() << '\n';
        return 1;
    }
}
