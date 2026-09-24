#include "backend.hpp"
#include "afw_compatibility.hpp"
#include "gaze_foveation.hpp"
#include "crop_motion.hpp"
#include "mock_ngx_parameters.hpp"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace cheeky::foveated_dlss {
// This fixture links the production SR backend. Independent NR/periphery
// inference is covered by the existing runtime fixtures.
void release_dlss_nr_view(DlssViewId) noexcept {}
void release_peripheral_dlaa_resources() noexcept {}
}

namespace {
using namespace cheeky::foveated_dlss;
using Microsoft::WRL::ComPtr;
void check(HRESULT result) { if (FAILED(result)) throw std::runtime_error("D3D12 history fixture failed"); }
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
unsigned creates{}, resets{};
int observed_reset{};
bool evaluate_succeeds{true};
ID3D12Resource* observed_motion{};
NgxResult create(ID3D12GraphicsCommandList*, unsigned, NgxParameters*, NgxHandle** handle) {
    *handle = reinterpret_cast<NgxHandle*>(static_cast<uintptr_t>(++creates));
    return 1;
}
NgxResult evaluate(ID3D12GraphicsCommandList*, const NgxHandle*, const NgxParameters* parameters, NgxProgressCallback) {
    observed_reset = 0;
    parameters->Get("Reset", &observed_reset);
    parameters->Get("MotionVectors", &observed_motion);
    resets += observed_reset != 0;
    return evaluate_succeeds ? 1U : 0xBAD00005U;
}
NgxResult release(NgxHandle*) { return 1; }

ComPtr<ID3D12Resource> buffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes; desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
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
void read_pixel(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
    ID3D12Resource* readback, UINT64 offset) {
    transition(list, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = source; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = readback; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = {offset, {DXGI_FORMAT_R32G32_FLOAT, 1, 1, 1, 256}};
    const D3D12_BOX box{0, 0, 0, 1, 1, 1};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void run_alternating(bool low_res_motion, float margin, unsigned feature = 1U, float mv_scale = -128.F) {
    reset_gaze_foveation(); allow_afw_stereo_projection(true);
    creates = resets = 0;
    ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter; check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device; check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{}; check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    ComPtr<ID3D12Resource> resources[4];
    const char* names[]{"Color", "Depth", "MotionVectors", "Output"};
    MockNgxParameters parameters;
    for (unsigned i = 0; i < 4; ++i) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = desc.Height = i < 2 || (i == 2 && low_res_motion) ? 128 : 256;
        desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
        desc.Format = i == 1 ? DXGI_FORMAT_R32_FLOAT : i == 2 ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            i == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            nullptr, IID_PPV_ARGS(&resources[i])));
        parameters.Set(names[i], resources[i].Get());
    }
    // Initialize a known vector field and inspect the actual resource passed
    // to inference after alternating-eye crop correction executes on the GPU.
    const auto motion_desc = resources[2]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes{};
    device->GetCopyableFootprints(&motion_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto upload = buffer(device.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD);
    unsigned char* data{}; check(upload->Map(0, nullptr, reinterpret_cast<void**>(&data)));
    for (unsigned y = 0; y < motion_desc.Height; ++y) {
        auto* row = reinterpret_cast<float*>(data + footprint.Offset + y * footprint.Footprint.RowPitch);
        for (unsigned x = 0; x < motion_desc.Width; ++x) { row[x * 2] = .125F; row[x * 2 + 1] = -.25F; }
    }
    // A unique pixel verifies that the exact crop copy honors its source origin.
    auto* sentinel = reinterpret_cast<float*>(data + footprint.Offset + 9 * footprint.Footprint.RowPitch);
    sentinel[7 * 2] = 1.25F; sentinel[7 * 2 + 1] = -2.5F;
    upload->Unmap(0, nullptr);
    transition(list.Get(), resources[2].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = footprint;
    to.pResource = resources[2].Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(list.Get(), resources[2].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto readback = buffer(device.Get(), 1536, D3D12_HEAP_TYPE_READBACK);
    CropGeometry previous{}; CropMotionOffset expected_offset{};
    parameters.Set("Width", 128U); parameters.Set("Height", 128U);
    parameters.Set("OutWidth", 256U); parameters.Set("OutHeight", 256U);
    const unsigned flags = low_res_motion ? 0x4B : 0x49;
    parameters.Set("DLSS.Feature.Create.Flags", flags);
    parameters.Set("MV.Scale.X", mv_scale); parameters.Set("MV.Scale.Y", mv_scale);
    parameters.Set("Reset", 0);
    Settings requested;
    requested.afw_automatic_coverage = true;
    requested.width = requested.height = .2F;
    requested.afw_warp_margin = margin;
    requested.aligned_height_offset = -.044F;
    float matrices[2][16]{};
    for (auto& m : matrices) { m[0] = m[5] = m[11] = 1.F; m[14] = .1F; }
    // Tangent FOV bounds from the RC2 support report's OpenXR log.
    for (unsigned eye = 0; eye < 2; ++eye) {
        const float left = eye == 0 ? -1.1756597F : -.7167311F;
        const float right = eye == 0 ? .7167311F : 1.1756597F;
        matrices[eye][0] = 2.F / (right - left);
        matrices[eye][8] = -(right + left) / (right - left);
        matrices[eye][5] = 2.F / (.7282905F + 1.0770619F);
        matrices[eye][9] = -(.7282905F - 1.0770619F) / (.7282905F + 1.0770619F);
    }
    bool stable = true, all_resets_correct = true, corrected = true;
    for (unsigned frame = 0; frame < 24; ++frame) {
        const DlssViewId view = frame == 21 ? 992 : 991;
        const bool game_reset = frame == 10;
        if (frame == 12) skip_d3d12_history(view); // Real bypass/discontinuity.
        if (frame == 14) requested.width = requested.height = .4F;
        if (frame == 20) requested.roundness = .5F; // Composite-only edit.
        evaluate_succeeds = frame != 17;
        parameters.Set("Reset", static_cast<int>(game_reset));
        publish_afw_stereo_projection(matrices, 256, 256, true);
        const auto projection = afw_stereo_projection();
        requested.afw_source_eye = frame % 2;
        const auto settings = afw_experiment_settings(requested, &projection);
        if (frame == 23) {
            auto disabled = settings; disabled.enabled = false;
            require(prepare_d3d12(list.Get(), &parameters, view, disabled) == nullptr, "Disabled SR prepared a crop");
        }
        auto* prepared = prepare_d3d12(list.Get(), &parameters, view, settings);
        require(prepared != nullptr, "Alternating-eye preparation failed");
        const auto crop = d3d12_evaluation_crop(prepared);
        DlssFrameContract contract{};
        contract.feature_id = feature;
        contract.view_id = view; contract.create_flags = flags;
        contract.motion_vectors_low_res = low_res_motion;
        contract.preserve_history_on_crop_move = true;
        contract.motion_vector_scale_x = contract.motion_vector_scale_y = mv_scale;
        contract.reset = game_reset || d3d12_evaluation_gaze_reset(prepared);
        D3D12DlssInputs inputs{};
        inputs.color = resources[0].Get(); inputs.depth = resources[1].Get();
        inputs.motion_vectors = resources[2].Get(); inputs.output = d3d12_private_output(prepared);
        inputs.color_base_x = inputs.depth_base_x = crop.input_base_x;
        inputs.color_base_y = inputs.depth_base_y = crop.input_base_y;
        inputs.mv_base_x = low_res_motion ? crop.input_base_x : crop.output_base_x;
        inputs.mv_base_y = low_res_motion ? crop.input_base_y : crop.output_base_y;
        const auto result = evaluate_d3d12_backend(list.Get(), contract, inputs, &parameters,
            d3d12_reconstruction_crop(prepared), {create, evaluate, release}, nullptr, &crop);
        require(ngx_succeeded(result) == evaluate_succeeds, "Private SR result was not preserved");
        if (frame >= 2 && frame < 10 && observed_reset) stable = false;
        const bool expected_reset = frame == 0 || frame == 10 || frame == 12 || frame == 14 || frame == 15 ||
            frame == 18 || frame == 21 || frame == 23;
        if (static_cast<bool>(observed_reset) != expected_reset) {
            all_resets_correct = false;
            std::cerr << "Incorrect SR reset on frame " << frame << ": " << observed_reset << '\n';
        }
        if (frame >= 2 && frame < 10) corrected &= observed_motion != resources[2].Get();
        if (frame == 9) {
            auto* copied=prepare_crop_texture12(list.Get(),resources[2].Get(),7,9,16,12);
            require(copied && copied->GetDesc().Width==16 && copied->GetDesc().Height==12 &&
                copied->GetDesc().Format==motion_desc.Format,"Exact crop copy changed extent or format");
            read_pixel(list.Get(),copied,readback.Get(),1024);
            require(crop_motion_offset(previous, crop, low_res_motion, mv_scale, mv_scale, expected_offset), "Invalid expected crop correction");
            read_pixel(list.Get(), observed_motion, readback.Get(), 0);
            read_pixel(list.Get(), resources[2].Get(), readback.Get(), 512);
        }
        previous = crop;
        if (frame < 4) std::cout << "AFW history frame=" << frame << " width_bits=" << std::bit_cast<unsigned>(settings.width)
            << " crop=" << crop.input_width << "x" << crop.input_height << " gaze_reset=" << contract.reset
            << " actual_reset=" << observed_reset << '\n';
        // Restore the game bag without compositing an unwritten private color
        // texture. Only our GPU vector passes execute; NVIDIA is mocked.
        finish_d3d12(list.Get(), &parameters, prepared, 0xBAD00000U);
        int restored = -1; parameters.Get("Reset", &restored);
        require(restored == static_cast<int>(game_reset), "Private reset leaked into the game parameter bag");
    }
    check(list->Close());
    ID3D12CommandList* submitted[]{list.Get()};
    queue->ExecuteCommandLists(1, submitted);
    crop_motion12_submitted(queue.Get(), 1, submitted); collect_crop_motion12();
    ComPtr<ID3D12Fence> done; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)));
    check(queue->Signal(done.Get(), 1));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "Could not create GPU completion event");
    check(done->SetEventOnCompletion(1, event));
    const auto waited = WaitForSingleObject(event, 10000); CloseHandle(event);
    require(waited == WAIT_OBJECT_0, "History fixture GPU submission timed out");
    check(readback->Map(0, nullptr, reinterpret_cast<void**>(&data)));
    const auto* actual = reinterpret_cast<const float*>(data);
    const auto* original = reinterpret_cast<const float*>(data + 512);
    const auto* copied = reinterpret_cast<const float*>(data + 1024);
    const bool copy_correct = copied[0]==1.25F && copied[1]==-2.5F;
    const bool vectors_correct = std::abs(actual[0] - (.125F + expected_offset.x)) < 1e-6F &&
        std::abs(actual[1] - (-.25F + expected_offset.y)) < 1e-6F && original[0] == .125F && original[1] == -.25F;
    readback->Unmap(0, nullptr);
    list.Reset(); allocator.Reset();
    release_d3d12_resources(); reset_gaze_foveation(); allow_afw_stereo_projection(false);
    std::cout << "AFW backend history: feature=" << feature << " mv_scale=" << mv_scale << " low_res_mv=" << low_res_motion << " margin=" << margin
        << " creates=" << creates << " resets=" << resets << '\n';
    require(creates == 3, "SR should create only for first use, pixel resize and a new view");
    require(stable, "Source-eye alternation reset private SR despite stable pixel dimensions");
    require(all_resets_correct, "SR failed to retain or invalidate history at the correct boundary");
    require(corrected, "Alternating source eyes did not receive privately corrected motion vectors");
    require(vectors_correct, "Inference received incorrect crop-relative vectors or game vectors were modified");
    require(copy_correct, "Exact crop copy did not preserve source pixel values");
}
}

int run_d3d12_history_tests() {
    try {
        for (bool low_res_motion : {false, true})
            for (float margin : {0.F, 1.F / 32, 2.F / 32}) run_alternating(low_res_motion, margin);
        // RR's supported low-resolution vectors include pixel-space vectors
        // (the Hogwarts capture uses scale 1) and normalized signed vectors.
        for (float scale : {1.F, -128.F}) run_alternating(true, 0.F, 13U, scale);
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
