#include "crop_motion.hpp"
#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using namespace cheeky::foveated_dlss;
struct Vector { float x, y; };
constexpr unsigned width = 12, height = 9, base_x = 2, base_y = 1, crop_width = 8, crop_height = 6;
void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("motion resampling GPU call failed"); }
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::array<Vector, width * height> source_data() {
    std::array<Vector, width * height> data{};
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x)
        data[y * width + x] = {x < 6 ? -2.0F : 7.0F, float(y) - 4.0F};
    data[2 * width + 3] = {1e20F, -1e20F};
    data[4 * width + 5] = {std::numeric_limits<float>::quiet_NaN(), 3.0F};
    data[5 * width + 8] = {4.0F, std::numeric_limits<float>::infinity()};
    return data;
}
void verify(const void* mapped, unsigned pitch, unsigned ow, unsigned oh, CropMotionOffset offset) {
    const auto input = source_data();
    for (unsigned y = 0; y < oh; ++y) {
        const auto* row = reinterpret_cast<const Vector*>(static_cast<const unsigned char*>(mapped) + y * pitch);
        for (unsigned x = 0; x < ow; ++x) {
            const auto sx = base_x + unsigned((2ULL * x + 1) * crop_width / (2ULL * ow));
            const auto sy = base_y + unsigned((2ULL * y + 1) * crop_height / (2ULL * oh));
            auto expected = input[sy * width + sx];
            const bool invalid = !std::isfinite(expected.x) || !std::isfinite(expected.y) ||
                std::abs(expected.x) > 1e15F || std::abs(expected.y) > 1e15F;
            if (!invalid) {
                expected.x = (expected.x + offset.x) * ow / crop_width;
                expected.y = (expected.y + offset.y) * oh / crop_height;
            }
            const auto equal = [](float a, float b) {
                return (std::isnan(a) && std::isnan(b)) || a == b ||
                    (std::isfinite(a) && std::isfinite(b) && std::abs(a - b) < 0.0001F);
            };
            require(equal(row[x].x, expected.x) && equal(row[x].y, expected.y),
                "motion vectors, crop correction, boundary sampling or invalid markers differ");
        }
    }
}
void run11() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.ArraySize = 2; desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32G32_FLOAT; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const auto data = source_data();
    std::array<D3D11_SUBRESOURCE_DATA, 2> initial{};
    for (auto& sub : initial) { sub.pSysMem = data.data(); sub.SysMemPitch = width * sizeof(Vector); }
    ComPtr<ID3D11Texture2D> source;
    check(device->CreateTexture2D(&desc, initial.data(), &source));
    const unsigned sizes[][2]{{4,3},{6,5},{8,6},{10,9},{12,7},{16,12}};
    for (auto& size : sizes) for (const CropMotionOffset offset : {CropMotionOffset{}, CropMotionOffset{-4,-8}}) {
        auto pass = create_crop_motion11(context.Get(), source.Get(), base_x, base_y,
            crop_width, crop_height, offset, size[0], size[1]);
        require(bool(pass), "DX11 vector pass creation failed");
        D3D11_TEXTURE2D_DESC staging_desc = desc;
        staging_desc.Width = size[0]; staging_desc.Height = size[1]; staging_desc.ArraySize = 1;
        staging_desc.BindFlags = 0; staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&staging_desc, nullptr, &staging));
        context->CopyResource(staging.Get(), crop_motion_resource(pass));
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        verify(mapped.pData, mapped.RowPitch, size[0], size[1], offset);
        context->Unmap(staging.Get(), 0);
    }
    require(!create_crop_motion11(context.Get(), source.Get(), width - 1, 0,
        crop_width, crop_height, {}, 16, 12), "DX11 out-of-bounds source was accepted");
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> copy;
    check(device->CreateTexture2D(&desc, nullptr, &copy));
    context->CopyResource(copy.Get(), source.Get());
    for (unsigned slice = 0; slice < 2; ++slice) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(copy.Get(), slice, D3D11_MAP_READ, 0, &mapped));
        for (unsigned y = 0; y < height; ++y)
            require(std::memcmp(static_cast<unsigned char*>(mapped.pData) + y * mapped.RowPitch,
                data.data() + y * width, width * sizeof(Vector)) == 0, "DX11 source vectors changed");
        context->Unmap(copy.Get(), slice);
    }
    release_crop_motion11();
}
ComPtr<ID3D12Resource> buffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes; desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&result)));
    return result;
}
void run12() {
    ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter; check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device; check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC q{}; check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
    const unsigned sizes[][2]{{4,3},{6,5},{8,6},{10,9},{12,7},{16,12}};
    for (auto& size : sizes) for (const CropMotionOffset offset : {CropMotionOffset{}, CropMotionOffset{-4,-8}}) {
        ComPtr<ID3D12CommandAllocator> allocator;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 2; desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_R32G32_FLOAT;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> source;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&source)));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 bytes{};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
        auto upload = buffer(device.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD);
        unsigned char* mapped{}; check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
        const auto data = source_data();
        for (unsigned y = 0; y < height; ++y)
            std::memcpy(mapped + fp.Offset + y * fp.Footprint.RowPitch, data.data() + y * width, width * sizeof(Vector));
        upload->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = fp;
        to.pResource = source.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        // Exercises source-state restoration as well as an array slice-zero view.
        auto* result = prepare_crop_motion12(list.Get(), source.Get(), base_x, base_y,
            crop_width, crop_height, offset, D3D12_RESOURCE_STATE_COPY_DEST, size[0], size[1]);
        require(result != nullptr, "DX12 vector pass creation failed");
        const auto output_desc = result->GetDesc();
        device->GetCopyableFootprints(&output_desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
        auto readback = buffer(device.Get(), bytes, D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {result, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &barrier);
        from = {}; from.pResource = result; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to = {}; to.pResource = readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = fp;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        check(list->Close());
        ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
        crop_motion12_submitted(queue.Get(), 1, lists); collect_crop_motion12();
        ComPtr<ID3D12Fence> fence; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        check(queue->Signal(fence.Get(), 1));
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(event != nullptr, "motion test event creation failed");
        check(fence->SetEventOnCompletion(1, event));
        const auto waited = WaitForSingleObject(event, 10000); CloseHandle(event);
        require(waited == WAIT_OBJECT_0, "motion GPU test timed out");
        check(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
        verify(mapped + fp.Offset, fp.Footprint.RowPitch, size[0], size[1], offset);
        readback->Unmap(0, nullptr);
        collect_crop_motion12();
    }
    release_crop_motion12();
}
} // namespace
int run_motion_resample_tests() {
    try {
        // Non-unit/signed MV.Scale must yield the same physical crop shift.
        cheeky::foveated_dlss::FoveationGeometry previous{}, current{};
        previous.input_base_x = 50; previous.input_base_y = 25;
        current.input_base_x = 54; current.input_base_y = 23;
        previous.output_base_x = 100; previous.output_base_y = 50;
        current.output_base_x = 108; current.output_base_y = 46;
        cheeky::foveated_dlss::CropMotionOffset offset{};
        require(cheeky::foveated_dlss::crop_motion_offset(previous, current, false, -2, 0.5F, offset) &&
            offset.x == -4 && offset.y == -8, "output-grid crop correction must use output-pixel units");
        run11(); run12();
        std::cout << "DX11/DX12 motion resampling passed: scales, boundaries, gaze offsets, invalid vectors\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
