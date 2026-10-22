#pragma once
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <openxr/openxr_platform.h>
#include <mutex>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace cheeky::fg {
// The application and OpenXR keep their D3D11 device. Only private processing
// uses D3D12. Never enqueue a D3D12 wait on the application's D3D11 context:
// SR's private D3D12 queue may still need later D3D11 signals from that context.
struct D3D11Bridge {
    using MicrosoftDevice = Microsoft::WRL::ComPtr<ID3D11Device5>;
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Image { Ptr<ID3D11Texture2D> original, shared11; Ptr<ID3D12Resource> shared12; std::uint64_t last_read{}; };
    MicrosoftDevice device11;
    Ptr<ID3D11DeviceContext4> context;
    Ptr<ID3D11Multithread> protection;
    Ptr<ID3D11DeviceContext> copy_commands;
    bool native_shared_textures{};
    struct ContextGuard {
        ID3D11Multithread* protection;
        explicit ContextGuard(ID3D11Multithread* value):protection(value){protection->Enter();}
        ~ContextGuard(){protection->Leave();}
    };
    Ptr<ID3D12Device> device12;
    Ptr<ID3D12CommandQueue> queue;
    Ptr<ID3D12Fence> fence12;
    Ptr<ID3D11Fence> fence11;
    Ptr<ID3D12Fence> completion;
    std::recursive_mutex mutex;
    std::recursive_mutex* dispatch_mutex{}; // Layer lifetime; shared with game NGX evaluation.
    std::unique_lock<std::recursive_mutex> lock_dispatch() {
        return dispatch_mutex?std::unique_lock<std::recursive_mutex>(*dispatch_mutex):std::unique_lock<std::recursive_mutex>{};
    }
    template<class Submit> auto runtime_submit(Submit&& submit) {
        // The runtime may use helper threads. Do not hold the immediate-
        // context lock across that external call; serialize against NGX and
        // our bridge, while individual D3D11 calls retain API protection.
        auto dispatch=lock_dispatch();return submit();
    }
    std::uint64_t serial{};
    std::uint64_t completion_serial{};
    std::unordered_map<XrSwapchain, std::unordered_map<ID3D11Texture2D*, Image>> sources;
    std::unordered_map<XrSwapchain, std::vector<Image>> outputs;
    bool initialize(ID3D11Device* device,bool native_shared=false) {
        if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&device11)))) return false;
        Ptr<ID3D11DeviceContext> immediate; device->GetImmediateContext(&immediate);
        if (FAILED(immediate.As(&context))) return false;
        if (FAILED(immediate.As(&protection))) return false;
        protection->SetMultithreadProtected(TRUE);
        native_shared_textures=native_shared;
        if(native_shared_textures&&FAILED(device->CreateDeferredContext(0,&copy_commands)))return false;
        Ptr<IDXGIDevice> dxgi; Ptr<IDXGIAdapter> adapter;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
            FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12)))) return false;
        D3D12_COMMAND_QUEUE_DESC desc{};
        if (FAILED(device12->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))) ||
            FAILED(device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12))) ||
            FAILED(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completion)))) return false;
        HANDLE handle{};
        auto hr = device12->CreateSharedHandle(fence12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
        if (SUCCEEDED(hr)) hr = device11->OpenSharedFence(handle, IID_PPV_ARGS(&fence11));
        if (handle) CloseHandle(handle);
        return SUCCEEDED(hr);
    }
    bool image(ID3D11Texture2D* original, Image& result) {
        if (!original) return false;
        D3D11_TEXTURE2D_DESC source{}; original->GetDesc(&source);
        if (source.SampleDesc.Count != 1 || source.MipLevels != 1 || !source.ArraySize) return false;
        result.original = original;
        // Quad presentation crosses the game's D3D11 context in both directions.
        // Keep allocation and command recording native to D3D11, opening only
        // the private texture alias on D3D12. Do not use the D3D12 allocation
        // fallback on this path after the driver command-dispatch crash.
        if(native_shared_textures)return image11(source,result);
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = source.Width; desc.Height = source.Height;
        desc.DepthOrArraySize = static_cast<UINT16>(source.ArraySize); desc.MipLevels = 1;
        desc.Format = source.Format; desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        auto hr = device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result.shared12));
        HANDLE handle{};
        if (SUCCEEDED(hr)) hr = device12->CreateSharedHandle(result.shared12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
        if (SUCCEEDED(hr)) hr = device11->OpenSharedResource1(handle, IID_PPV_ARGS(&result.shared11));
        if (handle) CloseHandle(handle);
        if (SUCCEEDED(hr)) return true;
        result.shared12.Reset(); result.shared11.Reset(); handle = nullptr;
        return image11(source,result);
    }
    bool image11(D3D11_TEXTURE2D_DESC source,Image& result) {
        source.Usage = D3D11_USAGE_DEFAULT; source.CPUAccessFlags = 0;
        source.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        source.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        auto hr = device11->CreateTexture2D(&source, nullptr, &result.shared11);
        HANDLE handle{};
        Ptr<IDXGIResource1> shared;
        if (SUCCEEDED(hr)) hr = result.shared11.As(&shared);
        if (SUCCEEDED(hr)) hr = shared->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
        if (SUCCEEDED(hr)) hr = device12->OpenSharedHandle(handle, IID_PPV_ARGS(&result.shared12));
        if (handle) CloseHandle(handle);
        return SUCCEEDED(hr);
    }
    bool from11() {
        ContextGuard guard(protection.Get());
        const auto value = serial + 1;
        if (FAILED(context->Signal(fence11.Get(), value))) return false;
        serial = value; context->Flush();
        return SUCCEEDED(queue->Wait(fence12.Get(), value));
    }
    bool copy_from11(ID3D11Resource* destination,ID3D11Resource* source) {
        Ptr<ID3D11CommandList> recorded;
        if(copy_commands){
            copy_commands->CopyResource(destination,source);
            if(FAILED(copy_commands->FinishCommandList(FALSE,&recorded)))return false;
        }
        // The copy, signal and flush form one immediate-context transaction.
        // Other DCS/layer threads must not interleave commands into it.
        ContextGuard guard(protection.Get());
        if(recorded)context->ExecuteCommandList(recorded.Get(),TRUE);
        else context->CopyResource(destination,source);
        return from11();
    }
    bool mark_complete(std::uint64_t& value) {
        value=completion_serial+1;
        if(FAILED(queue->Signal(completion.Get(),value)))return false;
        completion_serial=value;return true;
    }
    bool wait_complete(std::uint64_t value) {
        if(!value||completion->GetCompletedValue()>=value)return true;
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return false;
        const bool good=SUCCEEDED(completion->SetEventOnCompletion(value,event))&&
            WaitForSingleObject(event,5000)==WAIT_OBJECT_0;
        CloseHandle(event);return good&&SUCCEEDED(device12->GetDeviceRemovedReason());
    }
    template<class Capture> bool capture(XrSwapchain chain, ID3D11Texture2D* original, Capture&& consume) {
        auto dispatch=lock_dispatch();
        std::lock_guard lock(mutex);
        auto& images = sources[chain];
        auto it = images.find(original);
        if (it == images.end()) {
            Image shared; if (!image(original, shared)) return false;
            it = images.emplace(original, std::move(shared)).first;
        }
        // Reuse only after the private copy has read this shared source. A busy
        // slot skips FG capture, leaving native rendering and its context free.
        if(it->second.last_read&&completion->GetCompletedValue()<it->second.last_read)return false;
        if (!copy_from11(it->second.shared11.Get(),original)) return false;
        const bool good = consume(it->second.shared12.Get());
        // Even a rejected capture may have enqueued reads. Protect reuse.
        const bool ordered = mark_complete(it->second.last_read);
        return good && ordered;
    }
    bool enumerate(PFN_xrEnumerateSwapchainImages fn, XrSwapchain chain,
        std::vector<XrSwapchainImageD3D12KHR>& aliases) {
        std::lock_guard lock(mutex);
        unsigned count{};
        if (XR_FAILED(fn(chain, 0, &count, nullptr)) || !count) return false;
        std::vector<XrSwapchainImageD3D11KHR> native(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (XR_FAILED(fn(chain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(native.data())))) return false;
        std::vector<Image> shared(count);
        for (unsigned i = 0; i < count; ++i) if (!image(native[i].texture, shared[i])) return false;
        aliases.resize(count);
        for (unsigned i = 0; i < count; ++i) {
            aliases[i] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR}; aliases[i].texture = shared[i].shared12.Get();
        }
        outputs.insert_or_assign(chain, std::move(shared));
        return true;
    }
    bool upload(XrSwapchain chain, unsigned index) {
        Image shared;std::uint64_t ready{};
        {
            std::lock_guard lock(mutex);
            const auto it = outputs.find(chain);
            if (it == outputs.end() || index >= it->second.size()) return false;
            shared=it->second[index];
            if(!mark_complete(ready))return false;
        }
        // This runs on the presenter thread, outside the D3D11 context lock.
        // The game can submit the SR signals needed by this private GPU work.
        // Do not hold the bridge while waiting: the next game's guide atlas
        // and color captures use it too. The local COM references pin this eye.
        if(!wait_complete(ready))return false;
        auto dispatch=lock_dispatch();
        std::lock_guard lock(mutex);
        // Also protect the shared image from the next private D3D12 upload.
        return copy_from11(shared.original.Get(),shared.shared11.Get());
    }
    void drain() noexcept {
        if (!completion || !completion_serial) return;
        context->Flush();
        std::uint64_t retired{};if(!mark_complete(retired))return;
        while (completion->GetCompletedValue() < retired && SUCCEEDED(device12->GetDeviceRemovedReason())) Sleep(1);
    }
    void forget(XrSwapchain chain) {
        std::lock_guard lock(mutex); drain(); outputs.erase(chain); sources.erase(chain);
    }
    ~D3D11Bridge() { drain(); }
};
}
