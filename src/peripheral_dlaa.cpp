#include "peripheral_dlaa.hpp"

#include "runtime.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>

namespace cheeky::foveated_dlss {
namespace {

constexpr std::uint32_t dlss_feature_flag_mv_low_res = 1U << 1U;
constexpr std::uint32_t perf_quality_dlaa = 5U;
constexpr DlssViewId peripheral_view_mask = 0x8000000000000000ULL;

constexpr char motion_convert_shader_source[] = R"(
Texture2D<float2> SourceMotion : register(t0);
RWTexture2D<float2> PackedMotion : register(u0);
cbuffer Constants : register(b0) {
    uint2 SourceBase;
    uint2 SourceSize;
    uint2 DestSize;
};
[numthreads(16, 16, 1)]
void Main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= DestSize)) return;
    const uint2 numerator = (2U * id.xy + 1U) * SourceSize;
    const uint2 denominator = 2U * DestSize;
    const uint2 local = min(numerator / denominator, SourceSize - 1U);
    PackedMotion[id.xy] = SourceMotion.Load(int3(SourceBase + local, 0));
}
)";

constexpr char color_downsample_shader_source[] = R"(
Texture2D<float4> SourceColor : register(t0);
RWTexture2D<float4> PackedColor : register(u0);
cbuffer Constants : register(b0) {
    uint2 SourceBase;
    uint2 SourceSize;
    uint2 DestSize;
};
float4 LoadClamped(int2 local) {
    const int2 maximum = int2(SourceSize) - 1;
    return SourceColor.Load(int3(int2(SourceBase) + clamp(local, int2(0, 0), maximum), 0));
}
[numthreads(16, 16, 1)]
void Main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= DestSize)) return;
    const float2 source =
        (float2(id.xy) + 0.5) * float2(SourceSize) / float2(DestSize) - 0.5;
    const int2 base = int2(floor(source));
    const float2 fraction = source - floor(source);
    const float4 p00 = LoadClamped(base);
    const float4 p10 = LoadClamped(base + int2(1, 0));
    const float4 p01 = LoadClamped(base + int2(0, 1));
    const float4 p11 = LoadClamped(base + int2(1, 1));
    PackedColor[id.xy] = lerp(
        lerp(p00, p10, fraction.x),
        lerp(p01, p11, fraction.x),
        fraction.y
    );
}
)";

constexpr char depth_downsample_shader_source[] = R"(
Texture2D<float> SourceDepth : register(t0);
RWTexture2D<float> PackedDepth : register(u0);
cbuffer Constants : register(b0) {
    uint2 SourceBase;
    uint2 SourceSize;
    uint2 DestSize;
};
[numthreads(16, 16, 1)]
void Main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= DestSize)) return;
    const uint2 numerator = (2U * id.xy + 1U) * SourceSize;
    const uint2 denominator = 2U * DestSize;
    const uint2 local = min(numerator / denominator, SourceSize - 1U);
    PackedDepth[id.xy] = SourceDepth.Load(int3(SourceBase + local, 0));
}
)";

template <typename T>
void release(T*& object) noexcept {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

void transition(
    ID3D12GraphicsCommandList* const command_list,
    ID3D12Resource* const resource,
    const D3D12_RESOURCE_STATES before,
    const D3D12_RESOURCE_STATES after
) noexcept {
    if (command_list == nullptr || resource == nullptr || before == after) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    command_list->ResourceBarrier(1U, &barrier);
}

void uav_barrier(
    ID3D12GraphicsCommandList* const command_list,
    ID3D12Resource* const resource
) noexcept {
    if (command_list == nullptr || resource == nullptr) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    command_list->ResourceBarrier(1U, &barrier);
}

constexpr std::uint32_t converter_descriptor_set_count = 256U;

struct PeripheralViewState {
    DlssViewId view_id{};
    ID3D12Device* device{};
    ID3D12Resource* output{};
    ID3D12Resource* downsampled_color{};
    ID3D12Resource* downsampled_depth{};
    ID3D12Resource* converted_motion{};
    ID3D12DescriptorHeap* converter_descriptors{};
    ID3D12RootSignature* converter_root{};
    ID3D12PipelineState* converter_pipeline{};
    ID3D12PipelineState* color_pipeline{};
    ID3D12PipelineState* depth_pipeline{};
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    std::uint32_t working_width{};
    std::uint32_t working_height{};
    DXGI_FORMAT color_format{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT depth_format{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT output_format{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT motion_format{DXGI_FORMAT_UNKNOWN};
    std::uint32_t descriptor_size{};
    std::uint32_t next_descriptor_set{};
};

std::mutex state_mutex;
std::deque<PeripheralViewState> states;

void release_state(PeripheralViewState& state) noexcept {
    release(state.depth_pipeline);
    release(state.color_pipeline);
    release(state.converter_pipeline);
    release(state.converter_root);
    release(state.converter_descriptors);
    release(state.converted_motion);
    release(state.downsampled_depth);
    release(state.downsampled_color);
    release(state.output);
    release(state.device);
    state = {};
}

[[nodiscard]] bool create_texture(
    ID3D12Device* const device,
    const D3D12_RESOURCE_DESC& source_desc,
    const std::uint32_t width,
    const std::uint32_t height,
    const D3D12_RESOURCE_STATES initial_state,
    ID3D12Resource** const resource,
    const DXGI_FORMAT format_override = DXGI_FORMAT_UNKNOWN
) noexcept {
    if (device == nullptr || resource == nullptr || width == 0U || height == 0U) {
        return false;
    }
    auto desc = source_desc;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    if (format_override != DXGI_FORMAT_UNKNOWN) desc.Format = format_override;
    desc.DepthOrArraySize = 1U;
    desc.MipLevels = 1U;
    desc.SampleDesc.Count = 1U;
    desc.SampleDesc.Quality = 0U;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1U;
    heap.VisibleNodeMask = 1U;
    return SUCCEEDED(device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        initial_state,
        nullptr,
        IID_PPV_ARGS(resource)
    ));
}

[[nodiscard]] bool create_converter(PeripheralViewState& state) noexcept {
    release(state.depth_pipeline);
    release(state.color_pipeline);
    release(state.converter_pipeline);
    release(state.converter_root);
    release(state.converter_descriptors);
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 2U * converter_descriptor_set_count;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(state.device->CreateDescriptorHeap(
            &heap_desc,
            IID_PPV_ARGS(&state.converter_descriptors)
        ))) {
        return false;
    }
    state.descriptor_size = state.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
    );

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1U;
    ranges[0].BaseShaderRegister = 0U;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1U;
    ranges[1].BaseShaderRegister = 0U;

    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[2].Constants.ShaderRegister = 0U;
    parameters[2].Constants.Num32BitValues = 6U;

    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 3U;
    root_desc.pParameters = parameters;

    ID3DBlob* serialized{};
    ID3DBlob* errors{};
    auto result = D3D12SerializeRootSignature(
        &root_desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &errors
    );
    release(errors);
    if (FAILED(result) || serialized == nullptr) {
        release(serialized);
        return false;
    }
    result = state.device->CreateRootSignature(
        0U,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&state.converter_root)
    );
    release(serialized);
    if (FAILED(result)) return false;

    const auto compile_pipeline = [&](
        const char* const source,
        const std::size_t source_size,
        const char* const label,
        ID3D12PipelineState** const pipeline
    ) noexcept {
        ID3DBlob* bytecode{};
        ID3DBlob* compile_errors{};
        auto compile_result = D3DCompile(
            source,
            source_size,
            label,
            nullptr,
            nullptr,
            "Main",
            "cs_5_1",
            D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0U,
            &bytecode,
            &compile_errors
        );
        release(compile_errors);
        if (FAILED(compile_result) || bytecode == nullptr) {
            release(bytecode);
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
        pipeline_desc.pRootSignature = state.converter_root;
        pipeline_desc.CS.pShaderBytecode = bytecode->GetBufferPointer();
        pipeline_desc.CS.BytecodeLength = bytecode->GetBufferSize();
        compile_result = state.device->CreateComputePipelineState(
            &pipeline_desc,
            IID_PPV_ARGS(pipeline)
        );
        release(bytecode);
        return SUCCEEDED(compile_result);
    };

    return compile_pipeline(
            motion_convert_shader_source,
            sizeof(motion_convert_shader_source) - 1U,
            "Cheeky peripheral MV point conversion",
            &state.converter_pipeline
        ) &&
        compile_pipeline(
            color_downsample_shader_source,
            sizeof(color_downsample_shader_source) - 1U,
            "Cheeky peripheral color bilinear downsample",
            &state.color_pipeline
        ) &&
        compile_pipeline(
            depth_downsample_shader_source,
            sizeof(depth_downsample_shader_source) - 1U,
            "Cheeky peripheral depth point downsample",
            &state.depth_pipeline
        );
}

[[nodiscard]] DXGI_FORMAT typed_resource_format(
    const DXGI_FORMAT format
) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    default:
        return format;
    }
}

[[nodiscard]] DXGI_FORMAT depth_srv_format(
    const DXGI_FORMAT format
) noexcept {
    switch (format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

[[nodiscard]] PeripheralViewState* find_or_create_state(
    const PeripheralDlaaRequest& request
) noexcept {
    ID3D12Device* device{};
    if (request.command_list == nullptr ||
        FAILED(request.command_list->GetDevice(IID_PPV_ARGS(&device))) ||
        device == nullptr) {
        return nullptr;
    }

    const auto color_desc = request.color != nullptr
        ? request.color->GetDesc()
        : D3D12_RESOURCE_DESC{};
    const auto depth_desc = request.depth != nullptr
        ? request.depth->GetDesc()
        : D3D12_RESOURCE_DESC{};
    const auto output_desc = request.output_template != nullptr
        ? request.output_template->GetDesc()
        : D3D12_RESOURCE_DESC{};
    const auto motion_desc = request.motion_vectors != nullptr
        ? request.motion_vectors->GetDesc()
        : D3D12_RESOURCE_DESC{};
    const auto dimensions = peripheral_dlaa_dimensions(
        request.render_width,
        request.render_height,
        request.scale
    );
    const auto color_format = typed_resource_format(color_desc.Format);
    const auto depth_format = depth_srv_format(depth_desc.Format);
    const auto motion_format = typed_resource_format(motion_desc.Format);

    std::lock_guard lock(state_mutex);
    for (auto& state : states) {
        if (state.view_id != request.view_id) continue;
        const bool compatible = state.device == device &&
            state.render_width == request.render_width &&
            state.render_height == request.render_height &&
            state.working_width == dimensions.width &&
            state.working_height == dimensions.height &&
            state.color_format == color_format &&
            state.depth_format == depth_format &&
            state.output_format == output_desc.Format &&
            state.motion_format == motion_format;
        if (compatible) {
            device->Release();
            return &state;
        }
        release_state(state);
        state.view_id = request.view_id;
        state.device = device;
        state.render_width = request.render_width;
        state.render_height = request.render_height;
        state.working_width = dimensions.width;
        state.working_height = dimensions.height;
        state.color_format = color_format;
        state.depth_format = depth_format;
        state.output_format = output_desc.Format;
        state.motion_format = motion_format;
        return &state;
    }

    states.push_back({});
    auto& state = states.back();
    state.view_id = request.view_id;
    state.device = device;
    state.render_width = request.render_width;
    state.render_height = request.render_height;
    state.working_width = dimensions.width;
    state.working_height = dimensions.height;
    state.color_format = color_format;
    state.depth_format = depth_format;
    state.output_format = output_desc.Format;
    state.motion_format = motion_format;
    return &state;
}

[[nodiscard]] bool ensure_output(
    PeripheralViewState& state,
    const PeripheralDlaaRequest& request
) noexcept {
    if (request.output_override != nullptr) {
        const auto desc = request.output_override->GetDesc();
        return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            desc.Width == state.working_width &&
            desc.Height == state.working_height;
    }
    if (state.output != nullptr) return true;
    if (request.output_template == nullptr) return false;
    const auto desc = request.output_template->GetDesc();
    if (!create_texture(
            state.device,
            desc,
            state.working_width,
            state.working_height,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            &state.output,
            state.output_format
        )) {
        return false;
    }
    state.output->SetName(L"Cheeky Peripheral DLAA output");
    return true;
}

[[nodiscard]] bool ensure_resampler(PeripheralViewState& state) noexcept {
    return (state.converter_pipeline != nullptr &&
            state.color_pipeline != nullptr &&
            state.depth_pipeline != nullptr) ||
        create_converter(state);
}

[[nodiscard]] bool ensure_downsampled_color(
    PeripheralViewState& state,
    const PeripheralDlaaRequest& request
) noexcept {
    if (state.downsampled_color == nullptr) {
        const auto desc = request.color->GetDesc();
        if (state.color_format == DXGI_FORMAT_UNKNOWN ||
            !create_texture(
                state.device,
                desc,
                state.working_width,
                state.working_height,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                &state.downsampled_color,
                state.color_format
            )) {
            return false;
        }
        state.downsampled_color->SetName(
            L"Cheeky Peripheral downsampled color"
        );
    }
    return ensure_resampler(state);
}

[[nodiscard]] bool ensure_downsampled_depth(
    PeripheralViewState& state,
    const PeripheralDlaaRequest& request
) noexcept {
    if (state.downsampled_depth == nullptr) {
        const auto desc = request.depth->GetDesc();
        if (state.depth_format == DXGI_FORMAT_UNKNOWN ||
            !create_texture(
                state.device,
                desc,
                state.working_width,
                state.working_height,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                &state.downsampled_depth,
                DXGI_FORMAT_R32_FLOAT
            )) {
            return false;
        }
        state.downsampled_depth->SetName(
            L"Cheeky Peripheral downsampled depth"
        );
    }
    return ensure_resampler(state);
}

[[nodiscard]] bool ensure_converted_motion(
    PeripheralViewState& state,
    const PeripheralDlaaRequest& request
) noexcept {
    if (state.converted_motion == nullptr) {
        const auto desc = request.motion_vectors->GetDesc();
        if (state.motion_format == DXGI_FORMAT_UNKNOWN ||
            !create_texture(
                state.device,
                desc,
                state.working_width,
                state.working_height,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                &state.converted_motion,
                state.motion_format
            )) {
            return false;
        }
        state.converted_motion->SetName(
            L"Cheeky Peripheral working-resolution motion vectors"
        );
    }
    return ensure_resampler(state);
}

[[nodiscard]] bool dispatch_resample(
    PeripheralViewState& state,
    const PeripheralDlaaRequest& request,
    ID3D12Resource* const source,
    const D3D12_RESOURCE_STATES source_state,
    const DXGI_FORMAT source_format,
    ID3D12Resource* const destination,
    const DXGI_FORMAT destination_format,
    ID3D12PipelineState* const pipeline,
    const std::uint32_t source_base_x,
    const std::uint32_t source_base_y,
    const std::uint32_t source_width,
    const std::uint32_t source_height
) noexcept {
    if (source == nullptr || destination == nullptr || pipeline == nullptr ||
        source_format == DXGI_FORMAT_UNKNOWN ||
        destination_format == DXGI_FORMAT_UNKNOWN || source_width == 0U ||
        source_height == 0U || state.working_width == 0U ||
        state.working_height == 0U) {
        return false;
    }

    const auto descriptor_set =
        state.next_descriptor_set++ % converter_descriptor_set_count;
    const auto descriptor_offset =
        static_cast<std::uint64_t>(descriptor_set) * 2U *
        state.descriptor_size;
    auto cpu = state.converter_descriptors->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += descriptor_offset;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = source_format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1U;
    state.device->CreateShaderResourceView(source, &srv, cpu);
    cpu.ptr += state.descriptor_size;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = destination_format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    state.device->CreateUnorderedAccessView(destination, nullptr, &uav, cpu);

    if (source_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        transition(
            request.command_list,
            source,
            source_state,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
        );
    }

    ID3D12DescriptorHeap* heaps[]{state.converter_descriptors};
    request.command_list->SetDescriptorHeaps(1U, heaps);
    request.command_list->SetComputeRootSignature(state.converter_root);
    request.command_list->SetPipelineState(pipeline);
    auto gpu = state.converter_descriptors->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += descriptor_offset;
    request.command_list->SetComputeRootDescriptorTable(0U, gpu);
    gpu.ptr += state.descriptor_size;
    request.command_list->SetComputeRootDescriptorTable(1U, gpu);
    const std::uint32_t constants[6]{
        source_base_x,
        source_base_y,
        source_width,
        source_height,
        state.working_width,
        state.working_height,
    };
    request.command_list->SetComputeRoot32BitConstants(
        2U,
        6U,
        constants,
        0U
    );
    request.command_list->Dispatch(
        (state.working_width + 15U) / 16U,
        (state.working_height + 15U) / 16U,
        1U
    );
    uav_barrier(request.command_list, destination);
    transition(
        request.command_list,
        destination,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    );

    if (source_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        transition(
            request.command_list,
            source,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            source_state
        );
    }
    return true;
}

[[nodiscard]] bool convert_color(
    PeripheralViewState& state,
    const PeripheralDlaaRequest& request
) noexcept {
    if (!ensure_downsampled_color(state, request)) return false;
    return dispatch_resample(
        state,
        request,
        request.color,
        request.color_state,
        state.color_format,
        state.downsampled_color,
        state.color_format,
        state.color_pipeline,
        request.color_base_x,
        request.color_base_y,
        request.render_width,
        request.render_height
    );
}

[[nodiscard]] bool convert_depth(
    PeripheralViewState& state,
    const PeripheralDlaaRequest& request
) noexcept {
    if (!ensure_downsampled_depth(state, request)) return false;
    return dispatch_resample(
        state,
        request,
        request.depth,
        request.depth_state,
        state.depth_format,
        state.downsampled_depth,
        DXGI_FORMAT_R32_FLOAT,
        state.depth_pipeline,
        request.depth_base_x,
        request.depth_base_y,
        request.render_width,
        request.render_height
    );
}

[[nodiscard]] bool convert_motion(
    PeripheralViewState& state,
    const PeripheralDlaaRequest& request
) noexcept {
    if (!ensure_converted_motion(state, request)) return false;
    const auto source_width = request.motion_vectors_output_space
        ? request.source_output_width
        : request.render_width;
    const auto source_height = request.motion_vectors_output_space
        ? request.source_output_height
        : request.render_height;
    return dispatch_resample(
        state,
        request,
        request.motion_vectors,
        request.motion_state,
        state.motion_format,
        state.converted_motion,
        state.motion_format,
        state.converter_pipeline,
        request.mv_base_x,
        request.mv_base_y,
        source_width,
        source_height
    );
}

[[nodiscard]] float read_float(
    const NgxParameters* const parameters,
    const char* const name,
    const float fallback
) noexcept {
    float value{};
    return parameters != nullptr && ngx_succeeded(parameters->Get(name, &value))
        ? value
        : fallback;
}

}  // namespace

PeripheralDlaaDimensions peripheral_dlaa_dimensions(
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const float scale
) noexcept {
    if (render_width == 0U || render_height == 0U) return {};
    const auto clamped = std::clamp(scale, 0.20F, 1.0F);
    const auto scale_dimension = [clamped](const std::uint32_t value) noexcept {
        const auto scaled = static_cast<std::uint32_t>(
            static_cast<float>(value) * clamped + 0.5F
        );
        return (std::min)(value, (std::max)(32U, scaled));
    };
    return {scale_dimension(render_width), scale_dimension(render_height)};
}

DlssViewId peripheral_dlaa_view_id(const DlssViewId view_id) noexcept {
    return view_id ^ peripheral_view_mask;
}

bool prepare_peripheral_dlaa_resources(
    const PeripheralDlaaRequest& request,
    PeripheralDlaaResources& resources
) noexcept {
    resources = {};
    if (request.command_list == nullptr || request.color == nullptr ||
        request.depth == nullptr || request.motion_vectors == nullptr ||
        request.output_template == nullptr || request.render_width == 0U ||
        request.render_height == 0U) {
        return false;
    }
    auto* const state = find_or_create_state(request);
    if (state == nullptr || !ensure_output(*state, request)) return false;

    resources.working_width = state->working_width;
    resources.working_height = state->working_height;
    resources.output = request.output_override != nullptr
        ? request.output_override
        : state->output;
    resources.output_restore_state = request.output_override != nullptr
        ? request.output_state
        : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (resources.output == nullptr) return false;

    if (resources.output_restore_state !=
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        transition(
            request.command_list,
            resources.output,
            resources.output_restore_state,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS
        );
    }

    resources.color = request.color;
    resources.depth = request.depth;
    resources.motion_vectors = request.motion_vectors;
    resources.color_base_x = request.color_base_x;
    resources.color_base_y = request.color_base_y;
    resources.depth_base_x = request.depth_base_x;
    resources.depth_base_y = request.depth_base_y;
    resources.mv_base_x = request.mv_base_x;
    resources.mv_base_y = request.mv_base_y;

    const auto fail = [&]() noexcept {
        finish_peripheral_dlaa_motion_read(request.command_list, resources);
        if (resources.output_restore_state !=
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            transition(
                request.command_list,
                resources.output,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                resources.output_restore_state
            );
        }
        resources = {};
        return false;
    };

    const bool scaled = state->working_width != request.render_width ||
        state->working_height != request.render_height;
    if (scaled) {
        if (!convert_color(*state, request)) return fail();
        resources.color = state->downsampled_color;
        resources.color_base_x = 0U;
        resources.color_base_y = 0U;
        resources.downsampled_color = true;

        if (!convert_depth(*state, request)) return fail();
        resources.depth = state->downsampled_depth;
        resources.depth_base_x = 0U;
        resources.depth_base_y = 0U;
        resources.downsampled_depth = true;

        if (!convert_motion(*state, request)) return fail();
        resources.motion_vectors = state->converted_motion;
        resources.mv_base_x = 0U;
        resources.mv_base_y = 0U;
        resources.converted_motion = true;
    } else if (request.motion_vectors_output_space) {
        if (!convert_motion(*state, request)) return fail();
        resources.motion_vectors = state->converted_motion;
        resources.mv_base_x = 0U;
        resources.mv_base_y = 0U;
        resources.converted_motion = true;
    }
    return true;
}

bool evaluate_peripheral_dlaa_ngx(
    const PeripheralDlaaRequest& request,
    PeripheralDlaaResources& resources,
    NgxResult& result,
    D3D12BackendTiming* const timing
) noexcept {
    result = 0xBAD00005U;
    if (request.parameters == nullptr ||
        request.callbacks.create_feature == nullptr ||
        request.callbacks.evaluate_feature == nullptr ||
        request.callbacks.release_feature == nullptr ||
        !prepare_peripheral_dlaa_resources(request, resources)) {
        return false;
    }

    DlssFrameContract contract{};
    contract.view_id = peripheral_dlaa_view_id(request.view_id);
    contract.feature_id = 1U;
    contract.render_width = resources.working_width;
    contract.render_height = resources.working_height;
    contract.output_width = resources.working_width;
    contract.output_height = resources.working_height;
    contract.color_base_x = resources.color_base_x;
    contract.color_base_y = resources.color_base_y;
    contract.depth_base_x = resources.depth_base_x;
    contract.depth_base_y = resources.depth_base_y;
    contract.mv_base_x = resources.mv_base_x;
    contract.mv_base_y = resources.mv_base_y;
    contract.motion_vectors_low_res = true;
    contract.depth_inverted = request.depth_inverted;
    contract.reset = request.reset;
    contract.create_flags = request.create_flags | dlss_feature_flag_mv_low_res;
    contract.perf_quality = perf_quality_dlaa;

    D3D12DlssInputs inputs{};
    inputs.color = resources.color;
    inputs.depth = resources.depth;
    inputs.motion_vectors = resources.motion_vectors;
    inputs.output = resources.output;
    inputs.color_base_x = resources.color_base_x;
    inputs.color_base_y = resources.color_base_y;
    inputs.depth_base_x = resources.depth_base_x;
    inputs.depth_base_y = resources.depth_base_y;
    inputs.mv_base_x = resources.mv_base_x;
    inputs.mv_base_y = resources.mv_base_y;

    const CropGeometry full{
        0U,
        0U,
        resources.working_width,
        resources.working_height,
        0U,
        0U,
        resources.working_width,
        resources.working_height,
    };

    const auto saved_quality = get_ngx_integer_bits(
        request.parameters,
        "PerfQualityValue"
    );
    const auto saved_preset = get_ngx_integer_bits(
        request.parameters,
        "DLSS.Hint.Render.Preset.DLAA"
    );
    const auto saved_flags = get_ngx_integer_bits(
        request.parameters,
        "DLSS.Feature.Create.Flags"
    );
    const auto saved_mv_scale_x = read_float(
        request.parameters,
        "MV.Scale.X",
        request.motion_vector_scale_x
    );
    const auto saved_mv_scale_y = read_float(
        request.parameters,
        "MV.Scale.Y",
        request.motion_vector_scale_y
    );
    const auto saved_jitter_x = read_float(
        request.parameters,
        "Jitter.Offset.X",
        0.0F
    );
    const auto saved_jitter_y = read_float(
        request.parameters,
        "Jitter.Offset.Y",
        0.0F
    );

    auto mv_scale_x = request.motion_vector_scale_x;
    auto mv_scale_y = request.motion_vector_scale_y;
    if (resources.converted_motion) {
        const auto source_mv_width = request.motion_vectors_output_space
            ? request.source_output_width
            : request.render_width;
        const auto source_mv_height = request.motion_vectors_output_space
            ? request.source_output_height
            : request.render_height;
        if (source_mv_width != 0U && source_mv_height != 0U) {
            mv_scale_x *= static_cast<float>(resources.working_width) /
                static_cast<float>(source_mv_width);
            mv_scale_y *= static_cast<float>(resources.working_height) /
                static_cast<float>(source_mv_height);
        }
    }
    contract.motion_vector_scale_x = mv_scale_x;
    contract.motion_vector_scale_y = mv_scale_y;

    const auto jitter_scale_x = request.render_width != 0U
        ? static_cast<float>(resources.working_width) /
            static_cast<float>(request.render_width)
        : 1.0F;
    const auto jitter_scale_y = request.render_height != 0U
        ? static_cast<float>(resources.working_height) /
            static_cast<float>(request.render_height)
        : 1.0F;

    request.parameters->Set("PerfQualityValue", perf_quality_dlaa);
    request.parameters->Set("DLSS.Hint.Render.Preset.DLAA", request.preset);
    request.parameters->Set(
        "DLSS.Feature.Create.Flags",
        contract.create_flags
    );
    request.parameters->Set("MV.Scale.X", mv_scale_x);
    request.parameters->Set("MV.Scale.Y", mv_scale_y);
    request.parameters->Set("Jitter.Offset.X", saved_jitter_x * jitter_scale_x);
    request.parameters->Set("Jitter.Offset.Y", saved_jitter_y * jitter_scale_y);

    result = evaluate_d3d12_backend(
        request.command_list,
        contract,
        inputs,
        request.parameters,
        full,
        request.callbacks,
        timing
    );

    request.parameters->Set("PerfQualityValue", saved_quality);
    request.parameters->Set("DLSS.Hint.Render.Preset.DLAA", saved_preset);
    request.parameters->Set("DLSS.Feature.Create.Flags", saved_flags);
    request.parameters->Set("MV.Scale.X", saved_mv_scale_x);
    request.parameters->Set("MV.Scale.Y", saved_mv_scale_y);
    request.parameters->Set("Jitter.Offset.X", saved_jitter_x);
    request.parameters->Set("Jitter.Offset.Y", saved_jitter_y);

    if (!ngx_succeeded(result)) {
        finish_peripheral_dlaa_motion_read(request.command_list, resources);
        if (resources.output_restore_state !=
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            transition(
                request.command_list,
                resources.output,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                resources.output_restore_state
            );
        }
        resources = {};
        return false;
    }

    finish_peripheral_dlaa_motion_read(request.command_list, resources);
    finish_peripheral_dlaa_write(request.command_list, resources);
    return true;
}

void finish_peripheral_dlaa_motion_read(
    ID3D12GraphicsCommandList* const command_list,
    const PeripheralDlaaResources& resources
) noexcept {
    if (command_list == nullptr) return;
    if (resources.downsampled_color && resources.color != nullptr) {
        transition(
            command_list,
            resources.color,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS
        );
    }
    if (resources.downsampled_depth && resources.depth != nullptr) {
        transition(
            command_list,
            resources.depth,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS
        );
    }
    if (resources.converted_motion && resources.motion_vectors != nullptr) {
        transition(
            command_list,
            resources.motion_vectors,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS
        );
    }
}

void finish_peripheral_dlaa_write(
    ID3D12GraphicsCommandList* const command_list,
    const PeripheralDlaaResources& resources
) noexcept {
    if (command_list == nullptr || resources.output == nullptr) return;
    uav_barrier(command_list, resources.output);
    transition(
        command_list,
        resources.output,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    );
}

void restore_peripheral_dlaa_output(
    ID3D12GraphicsCommandList* const command_list,
    const PeripheralDlaaResources& resources
) noexcept {
    if (command_list == nullptr || resources.output == nullptr) return;
    transition(
        command_list,
        resources.output,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        resources.output_restore_state
    );
}

void release_peripheral_dlaa_view(const DlssViewId view_id) noexcept {
    {
        std::lock_guard lock(state_mutex);
        for (auto iterator = states.begin(); iterator != states.end(); ++iterator) {
            if (iterator->view_id != view_id) continue;
            release_state(*iterator);
            states.erase(iterator);
            break;
        }
    }
    release_d3d12_view(peripheral_dlaa_view_id(view_id));
}

void release_peripheral_dlaa_resources() noexcept {
    std::deque<DlssViewId> view_ids;
    {
        std::lock_guard lock(state_mutex);
        for (auto& state : states) {
            view_ids.push_back(state.view_id);
            release_state(state);
        }
        states.clear();
    }
    for (const auto view_id : view_ids) {
        release_d3d12_view(peripheral_dlaa_view_id(view_id));
    }
}

}  // namespace cheeky::foveated_dlss
