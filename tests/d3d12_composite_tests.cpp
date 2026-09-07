#include "d3d12_composite_shader.hpp"
#include "d3d12_output_contract.hpp"

#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using namespace cheeky::foveated_dlss;

void check(HRESULT hr) {
    if (FAILED(hr)) {
        std::cerr << "D3D12 test HRESULT: 0x" << std::hex << hr << std::dec << '\n';
        throw std::runtime_error("D3D12 call failed");
    }
}
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

ComPtr<ID3D12Resource> buffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&resource)));
    return resource;
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}

struct Texture {
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12Resource> upload;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    UINT64 bytes{};
};

Texture texture(ID3D12Device* device, ID3D12GraphicsCommandList* list,
    UINT width, UINT height, UINT16 slices, UINT16 mips, UINT32 value, DXGI_FORMAT format) {
    Texture t;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = slices;
    desc.MipLevels = mips;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t.resource)));
    const UINT count = static_cast<UINT>(slices) * mips;
    t.footprints.resize(count);
    device->GetCopyableFootprints(&desc, 0, count, 0, t.footprints.data(), nullptr, nullptr, &t.bytes);
    t.upload = buffer(device, t.bytes, D3D12_HEAP_TYPE_UPLOAD);
    unsigned char* mapped{};
    check(t.upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    for (UINT i = 0; i < count; ++i) {
        const auto& f = t.footprints[i];
        for (UINT y = 0; y < f.Footprint.Height; ++y) {
            auto* row = reinterpret_cast<UINT32*>(mapped + f.Offset + y * f.Footprint.RowPitch);
            for (UINT x = 0; x < f.Footprint.Width; ++x) row[x] = value;
        }
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = t.upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = f;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = t.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = i;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    t.upload->Unmap(0, nullptr);
    return t;
}

void run_case(ID3D12Device* device, UINT16 slices, UINT16 mips,
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
    ComPtr<ID3D12InfoQueue> messages;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&messages)))) {
        messages->ClearStoredMessages();
    }
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC q{};
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    const bool hdr = format == DXGI_FORMAT_R11G11B10_FLOAT;
    const UINT32 blue = hdr ? (0x1e0U << 22) : 0xffff0000U;
    const UINT32 green = hdr ? (0x3c0U << 11) : 0xff00ff00U;
    const UINT32 sentinel = hdr ? 0x3c0U : 0xff332211U;
    auto color = texture(device, list.Get(), 32, 24, slices, mips, blue, format);
    auto dlss = texture(device, list.Get(), 8, 8, 1, 1, green, format);
    auto output = texture(device, list.Get(), 32, 24, slices, mips, sentinel, format);
    transition(list.Get(), color.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list.Get(), dlss.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list.Get(), output.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 3;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    const auto increment = device->GetDescriptorHandleIncrementSize(hd.Type);
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    auto srv = d3d12_composite_srv(format);
    device->CreateShaderResourceView(color.resource.Get(), &srv, cpu);
    cpu.ptr += increment;
    device->CreateShaderResourceView(dlss.resource.Get(), &srv, cpu);
    cpu.ptr += increment;
    auto uav = d3d12_composite_uav(format);
    device->CreateUnorderedAccessView(output.resource.Get(), nullptr, &uav, cpu);

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[3]{};
    for (UINT i = 0; i < 2; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable = {1, &ranges[i]};
    }
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants = {0, 0, 24};
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 3;
    rd.pParameters = params;
    ComPtr<ID3DBlob> serialized, errors, shader;
    check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)));
    check(D3DCompile(composite_shader_source, sizeof(composite_shader_source) - 1, nullptr,
        nullptr, nullptr, "CompositeMain", "cs_5_1", 0, 0, &shader, &errors));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.Get();
    pd.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipeline)));
    list->SetPipelineState(pipeline.Get());
    list->SetComputeRootSignature(root.Get());
    ID3D12DescriptorHeap* heaps[]{heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
    list->SetComputeRootDescriptorTable(0, gpu);
    gpu.ptr += 2ULL * increment;
    list->SetComputeRootDescriptorTable(1, gpu);
    std::array<UINT32, 24> constants{24, 20, 4, 2, 0, 0, 32, 24, 12, 8, 8, 8};
    const float one = 1.0F;
    std::memcpy(&constants[12], &one, sizeof(one));
    std::memcpy(&constants[13], &one, sizeof(one));
    list->SetComputeRoot32BitConstants(2, 24, constants.data(), 0);
    list->Dispatch(2, 2, 1);
    transition(list.Get(), output.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto readback = buffer(device, output.bytes, D3D12_HEAP_TYPE_READBACK);
    for (UINT i = 0; i < output.footprints.size(); ++i) {
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = output.resource.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = i;
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = output.footprints[i];
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    check(list->Close());
    ID3D12CommandList* lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    check(queue->Signal(fence.Get(), 1));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "CreateEvent failed");
    const auto hr = fence->SetEventOnCompletion(1, event);
    const auto waited = SUCCEEDED(hr) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
    CloseHandle(event);
    require(waited == WAIT_OBJECT_0, "GPU test timed out");
    unsigned char* mapped{};
    check(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    bool correct = true;
    for (UINT i = 0; i < output.footprints.size(); ++i) {
        const auto& f = output.footprints[i];
        for (UINT y = 0; y < f.Footprint.Height; ++y) {
            const auto* row = reinterpret_cast<const UINT32*>(mapped + f.Offset + y * f.Footprint.RowPitch);
            for (UINT x = 0; x < f.Footprint.Width; ++x) {
                UINT32 expected = sentinel;
                if (i == 0 && x >= 4 && x < 28 && y >= 2 && y < 22) {
                    expected = x >= 12 && x < 20 && y >= 8 && y < 16 ? green : blue;
                }
                correct &= row[x] == expected;
            }
        }
    }
    readback->Unmap(0, nullptr);
    require(correct, "Composite pixels incorrect or another slice/mip was modified");
    if (messages) {
        for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
            SIZE_T size{};
            check(messages->GetMessage(i, nullptr, &size));
            std::vector<unsigned char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            check(messages->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::cerr << message->pDescription << '\n';
                throw std::runtime_error("D3D12 debug layer reported an error");
            }
        }
    }
    std::cout << "Composite pixels verified: slices=" << slices << " mips=" << mips
        << " format=" << format << '\n';
}
} // namespace

int run_d3d12_composite_tests() {
    try {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            std::cout << "D3D12 debug layer enabled\n";
        } else {
            std::cout << "D3D12 debug layer unavailable; verifying readback pixels\n";
        }
        ComPtr<IDXGIFactory4> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> warp;
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ComPtr<ID3D12Device> device;
        check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        run_case(device.Get(), 1, 1);
        run_case(device.Get(), 2, 5);
        run_case(device.Get(), 4, 5);
        run_case(device.Get(), 2, 5, DXGI_FORMAT_R11G11B10_FLOAT);
        run_case(device.Get(), 4, 5, DXGI_FORMAT_R11G11B10_FLOAT);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
