#include "crop_motion.hpp"
#include "runtime.hpp"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <deque>
#include <mutex>

namespace cheeky::foveated_dlss {
using Microsoft::WRL::ComPtr;
namespace {
constexpr char shader_source[] = R"(
Texture2D<float2> Source : register(t0);
RWTexture2D<float2> Destination : register(u0);
cbuffer Constants : register(b0) {
    uint2 Base; uint2 Size; float2 Offset; float2 Padding;
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= Size)) return;
    float2 mv = Source.Load(int3(Base + id.xy, 0));
    // Preserve invalid-vector sentinels. Valid vectors naturally address outside
    // the old crop at newly exposed edges, allowing DLSS to reject that history.
    Destination[id.xy] = any(abs(mv) > 1e15) ? mv : mv + Offset;
}
)";
struct Constants {
    unsigned x, y, width, height;
    float dx, dy, pad0{}, pad1{};
};
DXGI_FORMAT srv_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_UNORM:
        return format;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
bool bounds(unsigned base, unsigned extent, UINT64 size) noexcept {
    return extent != 0 && base <= size && extent <= size - base;
}
ComPtr<ID3DBlob> shader_bytecode() noexcept {
    static const ComPtr<ID3DBlob> code = [] {
        ComPtr<ID3DBlob> blob, errors;
        D3DCompile(shader_source, sizeof(shader_source) - 1, "crop_motion",
            nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, &blob, &errors);
        return blob;
    }();
    return code;
}
}

struct CropMotion11 {
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Resource> source;
    unsigned width{}, height{};
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> constants;
};
namespace {
std::mutex mutex11;
std::deque<std::shared_ptr<CropMotion11>> cache11;
}
void release_crop_motion11() noexcept {
    std::lock_guard lock(mutex11);
    cache11.clear();
}

std::shared_ptr<CropMotion11> create_crop_motion11(ID3D11DeviceContext* context,
    ID3D11Resource* source, unsigned x, unsigned y, unsigned width, unsigned height,
    CropMotionOffset offset) noexcept {
    if (!context || !source) return {};
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(source->QueryInterface(IID_PPV_ARGS(&texture)))) return {};
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    const auto format = srv_format(desc.Format);
    if (format == DXGI_FORMAT_UNKNOWN || desc.ArraySize != 1 ||
        desc.SampleDesc.Count != 1 || !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
        !bounds(x, width, desc.Width) || !bounds(y, height, desc.Height)) return {};
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    std::shared_ptr<CropMotion11> pass;
    {
        std::lock_guard lock(mutex11);
        for (auto& cached : cache11) {
            if (cached.use_count() == 1 && cached->context.Get() == context &&
                cached->source.Get() == source && cached->width == width && cached->height == height) {
                pass = cached; break;
            }
        }
    }
    const Constants data{x, y, width, height, offset.x, offset.y};
    if (!pass) {
    pass = std::make_shared<CropMotion11>();
    pass->context = context; pass->source = source;
    pass->width = width; pass->height = height;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(source, &sd, &pass->srv))) return {};
    desc = {};
    desc.Width = width; desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32G32_FLOAT;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    const auto code = shader_bytecode();
    if (!code || FAILED(device->CreateTexture2D(&desc, nullptr, &pass->output)) ||
        FAILED(device->CreateUnorderedAccessView(pass->output.Get(), nullptr, &pass->uav)) ||
        FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
            nullptr, &pass->shader))) return {};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(data); bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{&data, 0, 0};
    if (FAILED(device->CreateBuffer(&bd, &initial, &pass->constants))) return {};
    {
        std::lock_guard lock(mutex11);
        if (cache11.size() >= 16) cache11.pop_front();
        cache11.push_back(pass);
    }
    } else {
        context->UpdateSubresource(pass->constants.Get(), 0, nullptr, &data, 0, 0);
    }
    ComPtr<ID3D11ComputeShader> old_shader;
    ComPtr<ID3D11ShaderResourceView> old_srv;
    ComPtr<ID3D11UnorderedAccessView> old_uav;
    ComPtr<ID3D11Buffer> old_cb;
    std::array<ID3D11ClassInstance*, 256> classes{};
    UINT count = static_cast<UINT>(classes.size());
    context->CSGetShader(&old_shader, classes.data(), &count);
    context->CSGetShaderResources(0, 1, &old_srv);
    context->CSGetUnorderedAccessViews(0, 1, &old_uav);
    context->CSGetConstantBuffers(0, 1, &old_cb);
    ID3D11UnorderedAccessView* null_uav{};
    context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
    context->CSSetShader(pass->shader.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, pass->srv.GetAddressOf());
    context->CSSetUnorderedAccessViews(0, 1, pass->uav.GetAddressOf(), nullptr);
    context->CSSetConstantBuffers(0, 1, pass->constants.GetAddressOf());
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
    context->CSSetShaderResources(0, 1, old_srv.GetAddressOf());
    context->CSSetUnorderedAccessViews(0, 1, old_uav.GetAddressOf(), nullptr);
    context->CSSetConstantBuffers(0, 1, old_cb.GetAddressOf());
    context->CSSetShader(old_shader.Get(), classes.data(), count);
    for (UINT i = 0; i < count; ++i) if (classes[i]) classes[i]->Release();
    return pass;
}
ID3D11Resource* crop_motion_resource(const std::shared_ptr<CropMotion11>& pass) noexcept {
    return pass ? pass->output.Get() : nullptr;
}

namespace {
struct Pass12 {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> source, output;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    unsigned width{}, height{};
    std::uint64_t list_token{};
};
std::mutex mutex12;
std::deque<std::shared_ptr<Pass12>> pending12, available12;
// Private data belongs to the underlying D3D12 object. Unlike interface pointer
// equality, this also matches submissions seen through ReShade/Streamline proxies.
constexpr GUID motion_list_token_guid =
    {0x47d571bd, 0xda36, 0x4bfc, {0x9a, 0x07, 0x62, 0x85, 0x45, 0x19, 0x30, 0xe2}};
std::uint64_t next_list_token{};
std::uint64_t list_token(ID3D12CommandList* list, bool create) noexcept {
    std::uint64_t token{};
    UINT size = sizeof(token);
    if (SUCCEEDED(list->GetPrivateData(motion_list_token_guid, &size, &token)) &&
        size == sizeof(token) && token != 0) return token;
    if (!create) return 0;
    token = ++next_list_token;
    return SUCCEEDED(list->SetPrivateData(motion_list_token_guid, sizeof(token), &token)) ? token : 0;
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}
std::shared_ptr<Pass12> make_pass12(ID3D12Device* device, ID3D12Resource* source,
    unsigned width, unsigned height, DXGI_FORMAT format) noexcept {
    auto pass = std::make_shared<Pass12>();
    pass->device = device; pass->source = source;
    pass->width = width; pass->height = height;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32G32_FLOAT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = hp.VisibleNodeMask = 1;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&pass->output)))) return {};
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&pass->heap)))) return {};
    auto cpu = pass->heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = format; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(source, &sd, cpu);
    cpu.ptr += device->GetDescriptorHandleIncrementSize(hd.Type);
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = desc.Format; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(pass->output.Get(), nullptr, &ud, cpu);
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1; ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {2, ranges};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 0, sizeof(Constants) / 4};
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 2; rd.pParameters = params;
    ComPtr<ID3DBlob> root_blob, errors;
    const auto code = shader_bytecode();
    if (!code || FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1,
        &root_blob, &errors)) || FAILED(device->CreateRootSignature(0,
            root_blob->GetBufferPointer(), root_blob->GetBufferSize(), IID_PPV_ARGS(&pass->root)))) return {};
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = pass->root.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pass->pipeline)))) return {};
    return pass;
}
}
ID3D12Resource* prepare_crop_motion12(ID3D12GraphicsCommandList* list,
    ID3D12Resource* source, unsigned x, unsigned y, unsigned width, unsigned height,
    CropMotionOffset offset, D3D12_RESOURCE_STATES source_state) noexcept {
    if (!list || !source) return nullptr;
    const auto desc = source->GetDesc();
    const auto format = srv_format(desc.Format);
    if (format == DXGI_FORMAT_UNKNOWN || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 ||
        (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
        !bounds(x, width, desc.Width) || !bounds(y, height, desc.Height)) return nullptr;
    ComPtr<ID3D12Device> device;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return nullptr;
    std::lock_guard lock(mutex12);
    std::shared_ptr<Pass12> pass;
    for (auto it = available12.begin(); it != available12.end(); ++it) {
        if ((*it)->source.Get() == source && (*it)->width == width && (*it)->height == height) {
            pass = *it; available12.erase(it); break;
        }
    }
    // An unsubmitted list cannot be recycled safely; bound memory if a caller
    // abandons command lists without ever submitting them.
    if (pending12.size() >= 64) {
        static unsigned exhausted_logs{};
        if (exhausted_logs++ % 300U == 0U) {
            std::size_t unsubmitted{};
            for (const auto& pending : pending12) if (!pending->queue) ++unsubmitted;
            trace_event("D3D12 crop motion unavailable: pending=%zu unsubmitted=%zu",
                pending12.size(), unsubmitted);
        }
        return nullptr;
    }
    if (!pass) pass = make_pass12(device.Get(), source, width, height, format);
    if (!pass) return nullptr;
    pass->list = list;
    pass->list_token = list_token(list, true);
    pass->fence.Reset();
    pending12.push_back(pass);
    if (source_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        transition(list, source, source_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, pass->output.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const Constants data{x, y, width, height, offset.x, offset.y};
    list->SetDescriptorHeaps(1, pass->heap.GetAddressOf());
    list->SetComputeRootSignature(pass->root.Get());
    list->SetPipelineState(pass->pipeline.Get());
    list->SetComputeRootDescriptorTable(0, pass->heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRoot32BitConstants(1, sizeof(data) / 4, &data, 0);
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    if (source_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        transition(list, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, source_state);
    transition(list, pass->output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return pass->output.Get();
}
void crop_motion12_submitted(ID3D12CommandQueue* queue, unsigned count,
    ID3D12CommandList* const* lists) noexcept {
    if (!queue || !lists) return;
    std::lock_guard lock(mutex12);
    for (auto& pass : pending12) {
        if (pass->queue) continue;
        bool found{};
        for (unsigned i = 0; i < count; ++i) {
            if (lists[i] && (pass->list.Get() == lists[i] ||
                (pass->list_token != 0 && pass->list_token == list_token(lists[i], false)))) found = true;
        }
        if (!found) continue;
        // ReShade can notify before execution. Signal only at present.
        pass->queue = queue;
        static unsigned submission_logs{};
        if (submission_logs++ < 8U)
            trace_event("D3D12 crop motion submission matched token=%llu queue=%p",
                static_cast<unsigned long long>(pass->list_token), queue);
        pass->list.Reset();
    }
}
void collect_crop_motion12() noexcept {
    std::lock_guard lock(mutex12);
    for (auto it = pending12.begin(); it != pending12.end();) {
        auto& pass = *it;
        if (pass->queue && !pass->fence) {
            ComPtr<ID3D12Fence> fence;
            if (SUCCEEDED(pass->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
                SUCCEEDED(pass->queue->Signal(fence.Get(), 1))) pass->fence = fence;
        }
        if (!pass->fence || pass->fence->GetCompletedValue() < 1) { ++it; continue; }
        pass->list.Reset(); pass->fence.Reset(); pass->queue.Reset();
        if (available12.size() < 16) available12.push_back(pass);
        it = pending12.erase(it);
    }
}
void release_crop_motion12() noexcept {
    collect_crop_motion12();
    std::lock_guard lock(mutex12);
    available12.clear();
    // Pending work stays alive until its fence is complete.
}
}
