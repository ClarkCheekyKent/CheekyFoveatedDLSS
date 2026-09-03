#include "backend.hpp"
#include "diagnostics.hpp"
#include "runtime.hpp"

#include <d3dcompiler.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace cheeky::foveated_dlss {
namespace {

struct CanonicalFeatureKey {
    std::uint32_t feature{};
    std::uint32_t input_width{};
    std::uint32_t input_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t perf_quality{};
    std::uint32_t create_flags{};
    std::array<std::uint32_t, 6U> presets{};
};

struct CanonicalViewState {
    DlssViewId view_id{};
    CanonicalFeatureKey key{};
    NgxHandle* private_handle{};
    D3D12ReleaseFeatureFn release_feature{};
    CropGeometry last_crop{};
    bool has_key{};
    bool has_crop{};
};

std::mutex canonical_views_mutex;
std::deque<CanonicalViewState> canonical_views;
std::atomic<std::uint64_t> direct_preparation_sequence{};
std::atomic<std::uint64_t> canonical_rejection_sequence{};
std::atomic<std::uint64_t> canonical_failure_sequence{};

constexpr std::uint32_t dlss_feature_flag_mv_low_res = 1U << 1U;

[[nodiscard]] bool same_key(
    const CanonicalFeatureKey& left,
    const CanonicalFeatureKey& right
) noexcept {
    return left.feature == right.feature &&
        left.input_width == right.input_width &&
        left.input_height == right.input_height &&
        left.output_width == right.output_width &&
        left.output_height == right.output_height &&
        left.perf_quality == right.perf_quality &&
        left.create_flags == right.create_flags &&
        left.presets == right.presets;
}

[[nodiscard]] CanonicalViewState* find_view(const DlssViewId view_id) noexcept {
    for (auto& view : canonical_views) {
        if (view.view_id == view_id) return &view;
    }
    return nullptr;
}

[[nodiscard]] CanonicalFeatureKey make_key(
    const DlssFrameContract& contract,
    const CropGeometry& crop,
    const NgxParameters* const parameters
) noexcept {
    CanonicalFeatureKey key{};
    key.feature = contract.feature_id;
    key.input_width = crop.input_width;
    key.input_height = crop.input_height;
    key.output_width = crop.output_width;
    key.output_height = crop.output_height;
    key.perf_quality = contract.perf_quality;
    key.create_flags = contract.create_flags;
    constexpr std::array<const char*, 6U> names{
        "DLSS.Hint.Render.Preset.DLAA",
        "DLSS.Hint.Render.Preset.Quality",
        "DLSS.Hint.Render.Preset.Balanced",
        "DLSS.Hint.Render.Preset.Performance",
        "DLSS.Hint.Render.Preset.UltraPerformance",
        "DLSS.Hint.Render.Preset.UltraQuality",
    };
    for (std::size_t index{}; index < names.size(); ++index) {
        key.presets[index] = get_ngx_integer_bits(parameters, names[index]);
    }
    return key;
}

struct CanonicalParameterState {
    ID3D12Resource* color{};
    ID3D12Resource* depth{};
    ID3D12Resource* motion_vectors{};
    ID3D12Resource* exposure{};
    ID3D12Resource* output{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t out_width{};
    std::uint32_t out_height{};
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    std::uint32_t color_x{};
    std::uint32_t color_y{};
    std::uint32_t depth_x{};
    std::uint32_t depth_y{};
    std::uint32_t mv_x{};
    std::uint32_t mv_y{};
    std::uint32_t output_x{};
    std::uint32_t output_y{};
    int output_subrects{};
    int reset{};
};

[[nodiscard]] ID3D12Resource* read_d3d12_resource(
    const NgxParameters* const parameters,
    const char* const name
) noexcept {
    ID3D12Resource* value{};
    return parameters != nullptr && ngx_succeeded(parameters->Get(name, &value))
        ? value
        : nullptr;
}

[[nodiscard]] int read_int(
    const NgxParameters* const parameters,
    const char* const name
) noexcept {
    int value{};
    return parameters != nullptr && ngx_succeeded(parameters->Get(name, &value))
        ? value
        : 0;
}

[[nodiscard]] CanonicalParameterState capture_parameters(
    const NgxParameters* const parameters
) noexcept {
    return {
        read_d3d12_resource(parameters, "Color"),
        read_d3d12_resource(parameters, "Depth"),
        read_d3d12_resource(parameters, "MotionVectors"),
        read_d3d12_resource(parameters, "ExposureTexture"),
        read_d3d12_resource(parameters, "Output"),
        get_ui(parameters, "Width"), get_ui(parameters, "Height"),
        get_ui(parameters, "OutWidth"), get_ui(parameters, "OutHeight"),
        get_ui(parameters, "DLSS.Render.Subrect.Dimensions.Width"),
        get_ui(parameters, "DLSS.Render.Subrect.Dimensions.Height"),
        get_ui(parameters, "DLSS.Input.Color.Subrect.Base.X"),
        get_ui(parameters, "DLSS.Input.Color.Subrect.Base.Y"),
        get_ui(parameters, "DLSS.Input.Depth.Subrect.Base.X"),
        get_ui(parameters, "DLSS.Input.Depth.Subrect.Base.Y"),
        get_ui(parameters, "DLSS.Input.MV.Subrect.Base.X"),
        get_ui(parameters, "DLSS.Input.MV.Subrect.Base.Y"),
        get_ui(parameters, "DLSS.Output.Subrect.Base.X"),
        get_ui(parameters, "DLSS.Output.Subrect.Base.Y"),
        read_int(parameters, "DLSS.Enable.Output.Subrects"),
        read_int(parameters, "Reset"),
    };
}

void restore_parameters(
    NgxParameters* const parameters,
    const CanonicalParameterState& state
) noexcept {
    parameters->Set("Color", state.color);
    parameters->Set("Depth", state.depth);
    parameters->Set("MotionVectors", state.motion_vectors);
    parameters->Set("ExposureTexture", state.exposure);
    parameters->Set("Output", state.output);
    parameters->Set("Width", state.width);
    parameters->Set("Height", state.height);
    parameters->Set("OutWidth", state.out_width);
    parameters->Set("OutHeight", state.out_height);
    parameters->Set("DLSS.Render.Subrect.Dimensions.Width", state.render_width);
    parameters->Set("DLSS.Render.Subrect.Dimensions.Height", state.render_height);
    parameters->Set("DLSS.Input.Color.Subrect.Base.X", state.color_x);
    parameters->Set("DLSS.Input.Color.Subrect.Base.Y", state.color_y);
    parameters->Set("DLSS.Input.Depth.Subrect.Base.X", state.depth_x);
    parameters->Set("DLSS.Input.Depth.Subrect.Base.Y", state.depth_y);
    parameters->Set("DLSS.Input.MV.Subrect.Base.X", state.mv_x);
    parameters->Set("DLSS.Input.MV.Subrect.Base.Y", state.mv_y);
    parameters->Set("DLSS.Output.Subrect.Base.X", state.output_x);
    parameters->Set("DLSS.Output.Subrect.Base.Y", state.output_y);
    parameters->Set("DLSS.Enable.Output.Subrects", state.output_subrects);
    parameters->Set("Reset", state.reset);
}

constexpr std::uint32_t descriptors_per_set = 3U;
constexpr std::uint32_t descriptor_set_count = 256U;

struct D3D12Resources {
    ID3D12Device* device{};
    ID3D12Resource* dlss_output{};
    ID3D12DescriptorHeap* descriptors{};
    ID3D12RootSignature* root_signature{};
    ID3D12PipelineState* composite_pipeline{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    DXGI_FORMAT output_format{DXGI_FORMAT_UNKNOWN};
    std::uint32_t descriptor_size{};
    std::atomic<std::uint32_t> next_descriptor_set{};
    std::uint64_t last_used{};
    D3D12Resources* next{};
};

struct CompositeConstants {
    std::uint32_t output_size[2];
    std::uint32_t output_origin[2];
    std::uint32_t input_base[2];
    std::uint32_t input_size[2];
    std::uint32_t rect_base[2];
    std::uint32_t rect_size[2];
    float shape_width;
    float shape_height;
    float shape_offset_x;
    float shape_offset_y;
    float shape_roundness;
    float feather;
    std::uint32_t dlss_origin[2];
    std::uint32_t show_alignment_border;
};

static_assert(sizeof(CompositeConstants) == 21U * sizeof(std::uint32_t));

SRWLOCK resources_lock = SRWLOCK_INIT;
D3D12Resources* resource_list{};
std::uint64_t resource_use_sequence{};
constexpr std::size_t resource_cache_capacity = 8U;

SRWLOCK settings_lock = SRWLOCK_INIT;
bool last_enabled{};
std::uint32_t last_width_bits{};
std::uint32_t last_height_bits{};
std::uint32_t last_height_offset_bits{};
std::uint32_t last_roundness_bits{};

constexpr char composite_shader_source[] = R"(
Texture2D<float4> LowResolutionColor : register(t0);
Texture2D<float4> DlssColor : register(t1);
RWTexture2D<float4> GameOutput : register(u0);

cbuffer Constants : register(b0) {
    uint2 OutputSize;
    uint2 OutputOrigin;
    uint2 InputBase;
    uint2 InputSize;
    uint2 RectBase;
    uint2 RectSize;
    float ShapeWidth;
    float ShapeHeight;
    float ShapeOffsetX;
    float ShapeOffsetY;
    float ShapeRoundness;
    float Feather;
    uint2 DlssOrigin;
    uint ShowAlignmentBorder;
};

float ShapeDistance(float2 centered) {
    const float2 shape_size = max(
        float2(ShapeWidth, ShapeHeight),
        float2(0.0001, 0.0001)
    );
    const float2 scaled = abs(centered) / shape_size;
    return lerp(
        max(scaled.x, scaled.y),
        length(scaled),
        saturate(ShapeRoundness)
    );
}

float4 LoadInputBilinear(float2 position) {
    const float2 base = floor(position);
    const float2 fraction = position - base;
    const int2 minimum = int2(InputBase);
    const int2 maximum = minimum + int2(InputSize) - 1;
    const int2 p00 = clamp(int2(base), minimum, maximum);
    const int2 p10 = clamp(p00 + int2(1, 0), minimum, maximum);
    const int2 p01 = clamp(p00 + int2(0, 1), minimum, maximum);
    const int2 p11 = clamp(p00 + int2(1, 1), minimum, maximum);
    return lerp(
        lerp(LowResolutionColor.Load(int3(p00, 0)),
             LowResolutionColor.Load(int3(p10, 0)), fraction.x),
        lerp(LowResolutionColor.Load(int3(p01, 0)),
             LowResolutionColor.Load(int3(p11, 0)), fraction.x),
        fraction.y
    );
}

float2 InputPosition(uint2 local_pixel) {
    return float2(InputBase) +
        (float2(local_pixel) + 0.5) * float2(InputSize) /
        float2(OutputSize) - 0.5;
}

[numthreads(16, 16, 1)]
void CompositeMain(uint3 dispatch_id : SV_DispatchThreadID) {
    if (any(dispatch_id.xy >= OutputSize)) return;
    const uint2 local_pixel = dispatch_id.xy;
    const uint2 output_pixel = OutputOrigin + local_pixel;
    const float4 bilinear = LoadInputBilinear(InputPosition(local_pixel));
    float2 centered =
        (float2(local_pixel) + 0.5) / (0.5 * float2(OutputSize)) - 1.0;
    centered.x -= ShapeOffsetX * (1.0 - ShapeWidth);
    centered.y -= ShapeOffsetY * (1.0 - ShapeHeight);
    const float distance_from_center = ShapeDistance(centered);
    const float2 pixel_size = 2.0 / float2(OutputSize);
    const float distance_per_pixel = max(
        abs(ShapeDistance(centered + float2(pixel_size.x, 0.0)) -
            distance_from_center),
        abs(ShapeDistance(centered + float2(0.0, pixel_size.y)) -
            distance_from_center)
    );
    const bool alignment_border = ShowAlignmentBorder != 0U &&
        distance_from_center <= 1.0 &&
        distance_from_center >= 1.0 - 5.0 * distance_per_pixel;
    if (alignment_border) {
        GameOutput[output_pixel] = float4(1.0, 0.0, 0.0, 1.0);
        return;
    }
    const float normalized_feather = Feather /
        max(0.0001, min(ShapeWidth, ShapeHeight));
    const float weight = Feather <= 0.0
        ? (distance_from_center <= 1.0 ? 1.0 : 0.0)
        : 1.0 - smoothstep(
            max(0.0, 1.0 - normalized_feather),
            1.0,
            distance_from_center
        );
    const bool inside_rect = all(output_pixel >= RectBase) &&
        all(output_pixel < RectBase + RectSize);
    if (!inside_rect || weight <= 0.0) {
        GameOutput[output_pixel] = bilinear;
        return;
    }
    const uint2 dlss_pixel = DlssOrigin + (output_pixel - RectBase);
    const float4 dlss = DlssColor.Load(int3(dlss_pixel, 0));
    GameOutput[output_pixel] = lerp(bilinear, dlss, weight);
}
)";

void release_resources(D3D12Resources* const resources) noexcept {
    if (resources == nullptr) {
        return;
    }
    if (resources->composite_pipeline != nullptr) {
        resources->composite_pipeline->Release();
    }
    if (resources->root_signature != nullptr) {
        resources->root_signature->Release();
    }
    if (resources->descriptors != nullptr) {
        resources->descriptors->Release();
    }
    if (resources->dlss_output != nullptr) {
        resources->dlss_output->Release();
    }
    if (resources->device != nullptr) {
        resources->device->Release();
    }
    delete resources;
}

[[nodiscard]] D3D12Resources* create_resources(
    ID3D12Device* const device,
    ID3D12Resource* const game_output,
    const std::uint32_t output_width,
    const std::uint32_t output_height
) noexcept {
    if (device == nullptr || game_output == nullptr || output_width == 0U ||
        output_height == 0U) {
        return nullptr;
    }

    const auto output_description = game_output->GetDesc();
    if (output_description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        output_description.MipLevels != 1U ||
        output_description.DepthOrArraySize != 1U ||
        output_description.SampleDesc.Count != 1U ||
        output_description.Format == DXGI_FORMAT_UNKNOWN ||
        (output_description.Flags &
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0U) {
        return nullptr;
    }

    auto* const resources = new (std::nothrow) D3D12Resources{};
    if (resources == nullptr) {
        return nullptr;
    }
    resources->device = device;
    resources->device->AddRef();
    resources->output_width = output_width;
    resources->output_height = output_height;
    resources->output_format = output_description.Format;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1U;
    heap.VisibleNodeMask = 1U;
    auto private_description = output_description;
    private_description.Width = output_width;
    private_description.Height = output_height;
    private_description.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto result = device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &private_description,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&resources->dlss_output)
    );
    if (FAILED(result)) {
        release_resources(resources);
        return nullptr;
    }
    resources->dlss_output->SetName(L"Cheeky Foveated DLSS-SR output");

    D3D12_DESCRIPTOR_HEAP_DESC descriptor_description{};
    descriptor_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptor_description.NumDescriptors =
        descriptors_per_set * descriptor_set_count;
    descriptor_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    result = device->CreateDescriptorHeap(
        &descriptor_description,
        IID_PPV_ARGS(&resources->descriptors)
    );
    if (FAILED(result)) {
        release_resources(resources);
        return nullptr;
    }
    resources->descriptor_size = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
    );

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2U;
    ranges[0].BaseShaderRegister = 0U;
    ranges[0].OffsetInDescriptorsFromTableStart = 0U;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1U;
    ranges[1].BaseShaderRegister = 0U;
    ranges[1].OffsetInDescriptorsFromTableStart = 0U;

    D3D12_ROOT_PARAMETER root_parameters[3]{};
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1U;
    root_parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[1].DescriptorTable.NumDescriptorRanges = 1U;
    root_parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[2].Constants.ShaderRegister = 0U;
    root_parameters[2].Constants.Num32BitValues = 21U;

    D3D12_ROOT_SIGNATURE_DESC root_description{};
    root_description.NumParameters = 3U;
    root_description.pParameters = root_parameters;

    ID3DBlob* serialized_signature{};
    ID3DBlob* signature_errors{};
    result = D3D12SerializeRootSignature(
        &root_description,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized_signature,
        &signature_errors
    );
    if (signature_errors != nullptr) {
        signature_errors->Release();
    }
    if (FAILED(result) || serialized_signature == nullptr) {
        if (serialized_signature != nullptr) {
            serialized_signature->Release();
        }
        release_resources(resources);
        return nullptr;
    }
    result = device->CreateRootSignature(
        0U,
        serialized_signature->GetBufferPointer(),
        serialized_signature->GetBufferSize(),
        IID_PPV_ARGS(&resources->root_signature)
    );
    serialized_signature->Release();
    if (FAILED(result)) {
        release_resources(resources);
        return nullptr;
    }

    ID3DBlob* shader{};
    ID3DBlob* shader_errors{};
    result = D3DCompile(
        composite_shader_source,
        sizeof(composite_shader_source) - 1U,
        "Cheeky Foveated DLSS-SR",
        nullptr,
        nullptr,
        "CompositeMain",
        "cs_5_1",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0U,
        &shader,
        &shader_errors
    );
    if (shader_errors != nullptr) {
        shader_errors->Release();
    }
    if (FAILED(result) || shader == nullptr) {
        if (shader != nullptr) {
            shader->Release();
        }
        release_resources(resources);
        return nullptr;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description{};
    pipeline_description.pRootSignature = resources->root_signature;
    pipeline_description.CS.pShaderBytecode = shader->GetBufferPointer();
    pipeline_description.CS.BytecodeLength = shader->GetBufferSize();
    result = device->CreateComputePipelineState(
        &pipeline_description,
        IID_PPV_ARGS(&resources->composite_pipeline)
    );
    shader->Release();
    if (FAILED(result)) {
        release_resources(resources);
        return nullptr;
    }
    return resources;
}

[[nodiscard]] D3D12Resources* find_or_create_resources(
    ID3D12Device* const device,
    ID3D12Resource* const game_output,
    const std::uint32_t output_width,
    const std::uint32_t output_height
) noexcept {
    const auto format = game_output->GetDesc().Format;
    AcquireSRWLockExclusive(&resources_lock);
    for (auto* current = resource_list; current != nullptr;
         current = current->next) {
        if (current->device == device &&
            current->output_width == output_width &&
            current->output_height == output_height &&
            current->output_format == format) {
            current->last_used = ++resource_use_sequence;
            ReleaseSRWLockExclusive(&resources_lock);
            return current;
        }
    }
    auto* const created = create_resources(
        device,
        game_output,
        output_width,
        output_height
    );
    if (created != nullptr) {
        created->last_used = ++resource_use_sequence;
        created->next = resource_list;
        resource_list = created;

        std::size_t count{};
        D3D12Resources* oldest{};
        D3D12Resources* oldest_previous{};
        D3D12Resources* previous{};
        for (auto* current = resource_list; current != nullptr;
             current = current->next) {
            ++count;
            if (current != created &&
                (oldest == nullptr || current->last_used < oldest->last_used)) {
                oldest = current;
                oldest_previous = previous;
            }
            previous = current;
        }
        if (count > resource_cache_capacity && oldest != nullptr) {
            if (oldest_previous == nullptr) {
                resource_list = oldest->next;
            } else {
                oldest_previous->next = oldest->next;
            }
            release_resources(oldest);
        }
    }
    ReleaseSRWLockExclusive(&resources_lock);
    return created;
}

[[nodiscard]] ID3D12Resource* get_resource(
    const NgxParameters* const parameters,
    const char* const name
) noexcept {
    ID3D12Resource* resource{};
    return parameters != nullptr &&
            ngx_succeeded(parameters->Get(name, &resource))
        ? resource
        : nullptr;
}

[[nodiscard]] bool texture_region_fits(
    ID3D12Resource* const resource,
    const std::uint32_t x,
    const std::uint32_t y,
    const std::uint32_t width,
    const std::uint32_t height
) noexcept {
    if (resource == nullptr || width == 0U || height == 0U) return false;
    const auto description = resource->GetDesc();
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        return false;
    }
    return static_cast<std::uint64_t>(x) + width <= description.Width &&
        static_cast<std::uint64_t>(y) + height <= description.Height;
}

void transition_resource(
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

void insert_uav_barrier(
    ID3D12GraphicsCommandList* const command_list,
    ID3D12Resource* const resource
) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    command_list->ResourceBarrier(1U, &barrier);
}

[[nodiscard]] bool consume_reset(const Settings& settings) noexcept {
    std::uint32_t width_bits{};
    std::uint32_t height_bits{};
    std::uint32_t height_offset_bits{};
    std::uint32_t roundness_bits{};
    std::memcpy(&width_bits, &settings.width, sizeof(width_bits));
    std::memcpy(&height_bits, &settings.height, sizeof(height_bits));
    std::memcpy(
        &height_offset_bits,
        &settings.height_offset,
        sizeof(height_offset_bits)
    );
    std::memcpy(&roundness_bits, &settings.roundness, sizeof(roundness_bits));

    AcquireSRWLockExclusive(&settings_lock);
    const bool reset = !last_enabled || width_bits != last_width_bits ||
        height_bits != last_height_bits ||
        height_offset_bits != last_height_offset_bits ||
        roundness_bits != last_roundness_bits;
    last_enabled = true;
    last_width_bits = width_bits;
    last_height_bits = height_bits;
    last_height_offset_bits = height_offset_bits;
    last_roundness_bits = roundness_bits;
    ReleaseSRWLockExclusive(&settings_lock);
    return reset;
}

}  // namespace

struct D3D12Evaluation {
    D3D12Resources* resources{};
    ID3D12Resource* original_output{};
    std::uint32_t original_width{};
    std::uint32_t original_height{};
    std::uint32_t original_out_width{};
    std::uint32_t original_out_height{};
    std::uint32_t original_output_subrects{};
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t color_x{};
    std::uint32_t color_y{};
    std::uint32_t depth_x{};
    std::uint32_t depth_y{};
    std::uint32_t motion_x{};
    std::uint32_t motion_y{};
    std::uint32_t output_x{};
    std::uint32_t output_y{};
    std::uint32_t composite_x{};
    std::uint32_t composite_y{};
    std::uint32_t dlss_source_x{};
    std::uint32_t dlss_source_y{};
    std::uint32_t reset{};
    CropGeometry crop{};
    float shape_width{};
    float shape_height{};
    float shape_offset_x{};
    float shape_offset_y{};
    float roundness{};
    float feather{};
    bool alignment_border{};
    std::uint64_t descriptor_offset{};
    bool diagnostic_trace{};
    std::uint64_t diagnostic_sequence{};
};

D3D12Evaluation* prepare_d3d12(
    ID3D12GraphicsCommandList* const command_list,
    const NgxParameters* const parameters,
    const Settings& settings
) noexcept {
    if (!settings.enabled) {
        AcquireSRWLockExclusive(&settings_lock);
        last_enabled = false;
        ReleaseSRWLockExclusive(&settings_lock);
        diagnostic_note_state(
            DiagnosticApi::d3d12,
            DiagnosticState::disabled
        );
        return nullptr;
    }
    if (command_list == nullptr || parameters == nullptr) {
        diagnostic_note_state(
            DiagnosticApi::d3d12,
            DiagnosticState::invalid_arguments
        );
        return nullptr;
    }

    auto* const color = get_resource(parameters, "Color");
    auto* const motion_vectors = get_resource(parameters, "MotionVectors");
    auto* const output = get_resource(parameters, "Output");
    const auto render_width = get_ui(parameters, "Width");
    const auto render_height = get_ui(parameters, "Height");
    const auto output_width = get_ui(parameters, "OutWidth");
    const auto output_height = get_ui(parameters, "OutHeight");
    if (color == nullptr || output == nullptr || color == output ||
        render_width == 0U || render_height == 0U || output_width == 0U ||
        output_height == 0U) {
        diagnostic_note_state(
            DiagnosticApi::d3d12,
            color == nullptr || output == nullptr || color == output
                ? DiagnosticState::missing_resources
                : DiagnosticState::incompatible_contract
        );
        return nullptr;
    }

    const auto color_x = get_ui(parameters, "DLSS.Input.Color.Subrect.Base.X");
    const auto color_y = get_ui(parameters, "DLSS.Input.Color.Subrect.Base.Y");
    const auto output_x = get_ui(parameters, "DLSS.Output.Subrect.Base.X");
    const auto output_y = get_ui(parameters, "DLSS.Output.Subrect.Base.Y");
    CropGeometry crop{};
    if (!calculate_crop(
            settings,
            render_width,
            render_height,
            output_width,
            output_height,
            output_x,
            output_y,
            crop
        )) {
        diagnostic_note_state(
            DiagnosticApi::d3d12,
            DiagnosticState::incompatible_contract
        );
        return nullptr;
    }

    ID3D12Device* device{};
    const auto device_result = command_list->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(device_result) || device == nullptr) {
        diagnostic_note_state(
            DiagnosticApi::d3d12,
            DiagnosticState::invalid_arguments
        );
        return nullptr;
    }
    auto* const resources = find_or_create_resources(
        device,
        output,
        crop.output_width,
        crop.output_height
    );
    if (resources == nullptr) {
        device->Release();
        diagnostic_note_state(
            DiagnosticApi::d3d12,
            DiagnosticState::resource_initialization_failed
        );
        return nullptr;
    }

    auto* const evaluation = new (std::nothrow) D3D12Evaluation{};
    if (evaluation == nullptr) {
        device->Release();
        diagnostic_note_state(
            DiagnosticApi::d3d12,
            DiagnosticState::allocation_failed
        );
        return nullptr;
    }
    evaluation->resources = resources;
    evaluation->original_output = output;
    evaluation->original_width = render_width;
    evaluation->original_height = render_height;
    evaluation->original_out_width = output_width;
    evaluation->original_out_height = output_height;
    evaluation->original_output_subrects = get_ui(
        parameters, "DLSS.Enable.Output.Subrects"
    );
    evaluation->render_width = get_ui(
        parameters,
        "DLSS.Render.Subrect.Dimensions.Width"
    );
    evaluation->render_height = get_ui(
        parameters,
        "DLSS.Render.Subrect.Dimensions.Height"
    );
    if (evaluation->render_width == 0U) {
        evaluation->render_width = render_width;
    }
    if (evaluation->render_height == 0U) {
        evaluation->render_height = render_height;
    }
    evaluation->output_width = output_width;
    evaluation->output_height = output_height;
    evaluation->color_x = color_x;
    evaluation->color_y = color_y;
    evaluation->depth_x = get_ui(parameters, "DLSS.Input.Depth.Subrect.Base.X");
    evaluation->depth_y = get_ui(parameters, "DLSS.Input.Depth.Subrect.Base.Y");
    evaluation->motion_x = get_ui(parameters, "DLSS.Input.MV.Subrect.Base.X");
    evaluation->motion_y = get_ui(parameters, "DLSS.Input.MV.Subrect.Base.Y");
    evaluation->output_x = output_x;
    evaluation->output_y = output_y;
    evaluation->composite_x = crop.output_base_x;
    evaluation->composite_y = crop.output_base_y;
    evaluation->dlss_source_x = 0U;
    evaluation->dlss_source_y = 0U;
    evaluation->reset = get_ui(parameters, "Reset");
    evaluation->crop = crop;
    evaluation->shape_width = settings.width;
    evaluation->shape_height = settings.height;
    evaluation->shape_offset_x = settings.x_offset;
    evaluation->shape_offset_y = settings.height_offset;
    evaluation->roundness = settings.roundness;
    evaluation->feather = settings.transition_width;
    evaluation->alignment_border = settings.alignment_border_enabled;

    const auto descriptor_set = resources->next_descriptor_set.fetch_add(
        1U,
        std::memory_order_relaxed
    ) % descriptor_set_count;
    evaluation->descriptor_offset =
        static_cast<std::uint64_t>(descriptor_set) * descriptors_per_set *
        resources->descriptor_size;

    auto cpu = resources->descriptors->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += evaluation->descriptor_offset;
    const auto color_description = color->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC color_srv{};
    color_srv.Format = color_description.Format;
    color_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    color_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    color_srv.Texture2D.MipLevels = 1U;
    device->CreateShaderResourceView(color, &color_srv, cpu);
    cpu.ptr += resources->descriptor_size;

    D3D12_SHADER_RESOURCE_VIEW_DESC dlss_srv{};
    dlss_srv.Format = resources->output_format;
    dlss_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    dlss_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    dlss_srv.Texture2D.MipLevels = 1U;
    device->CreateShaderResourceView(resources->dlss_output, &dlss_srv, cpu);
    cpu.ptr += resources->descriptor_size;

    D3D12_UNORDERED_ACCESS_VIEW_DESC output_uav{};
    output_uav.Format = resources->output_format;
    output_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(output, nullptr, &output_uav, cpu);
    device->Release();

    auto* const mutable_parameters = const_cast<NgxParameters*>(parameters);
    mutable_parameters->Set("Output", resources->dlss_output);
    mutable_parameters->Set("Width", crop.input_width);
    mutable_parameters->Set("Height", crop.input_height);
    mutable_parameters->Set("OutWidth", crop.output_width);
    mutable_parameters->Set("OutHeight", crop.output_height);
    mutable_parameters->Set(
        "DLSS.Render.Subrect.Dimensions.Width",
        crop.input_width
    );
    mutable_parameters->Set(
        "DLSS.Render.Subrect.Dimensions.Height",
        crop.input_height
    );
    mutable_parameters->Set(
        "DLSS.Input.Color.Subrect.Base.X",
        color_x + crop.input_base_x
    );
    mutable_parameters->Set(
        "DLSS.Input.Color.Subrect.Base.Y",
        color_y + crop.input_base_y
    );
    mutable_parameters->Set(
        "DLSS.Input.Depth.Subrect.Base.X",
        evaluation->depth_x + crop.input_base_x
    );
    mutable_parameters->Set(
        "DLSS.Input.Depth.Subrect.Base.Y",
        evaluation->depth_y + crop.input_base_y
    );
    const auto create_flags = get_ngx_integer_bits(
        parameters, "DLSS.Feature.Create.Flags"
    );
    const bool flag_says_low_res =
        (create_flags & dlss_feature_flag_mv_low_res) != 0U;
    const auto low_res_motion_x =
        evaluation->motion_x + crop.input_base_x;
    const auto low_res_motion_y =
        evaluation->motion_y + crop.input_base_y;
    const auto high_res_motion_x =
        evaluation->motion_x + crop.output_base_x - output_x;
    const auto high_res_motion_y =
        evaluation->motion_y + crop.output_base_y - output_y;
    const bool low_res_fits = texture_region_fits(
        motion_vectors,
        low_res_motion_x,
        low_res_motion_y,
        crop.input_width,
        crop.input_height
    );
    const bool high_res_fits = texture_region_fits(
        motion_vectors,
        high_res_motion_x,
        high_res_motion_y,
        crop.output_width,
        crop.output_height
    );
    bool motion_vectors_low_res = flag_says_low_res;
    if (low_res_fits != high_res_fits) {
        motion_vectors_low_res = low_res_fits;
    }
    const auto motion_crop_x = motion_vectors_low_res
        ? crop.input_base_x
        : crop.output_base_x - output_x;
    const auto motion_crop_y = motion_vectors_low_res
        ? crop.input_base_y
        : crop.output_base_y - output_y;
    mutable_parameters->Set(
        "DLSS.Input.MV.Subrect.Base.X",
        evaluation->motion_x + motion_crop_x
    );
    mutable_parameters->Set(
        "DLSS.Input.MV.Subrect.Base.Y",
        evaluation->motion_y + motion_crop_y
    );
    mutable_parameters->Set("DLSS.Output.Subrect.Base.X", 0U);
    mutable_parameters->Set("DLSS.Output.Subrect.Base.Y", 0U);
    mutable_parameters->Set("DLSS.Enable.Output.Subrects", 0);
    if (consume_reset(settings)) {
        mutable_parameters->Set("Reset", 1U);
    }
    const auto preparation_sequence = direct_preparation_sequence.fetch_add(
        1U, std::memory_order_relaxed
    );
    if (preparation_sequence < 8U || preparation_sequence % 600U == 0U) {
        const auto motion_description = motion_vectors != nullptr
            ? motion_vectors->GetDesc()
            : D3D12_RESOURCE_DESC{};
        trace_event(
            "D3D12 crop prepare seq=%llu flags=0x%08X flagMV=%s "
            "mv=%llux%u low=(%u,%u %ux%u fit=%s) "
            "high=(%u,%u %ux%u fit=%s) selected=%s",
            static_cast<unsigned long long>(preparation_sequence),
            create_flags,
            flag_says_low_res ? "low" : "high",
            static_cast<unsigned long long>(motion_description.Width),
            motion_description.Height,
            low_res_motion_x,
            low_res_motion_y,
            crop.input_width,
            crop.input_height,
            low_res_fits ? "yes" : "no",
            high_res_motion_x,
            high_res_motion_y,
            crop.output_width,
            crop.output_height,
            high_res_fits ? "yes" : "no",
            motion_vectors_low_res ? "low" : "high"
        );
    }
    diagnostic_note_crop(DiagnosticApi::d3d12, crop);
    return evaluation;
}

D3D12Evaluation* prepare_d3d12_streamline(
    ID3D12GraphicsCommandList* const command_list,
    ID3D12Resource* const color,
    ID3D12Resource* const output,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const std::uint32_t color_x,
    const std::uint32_t color_y,
    const std::uint32_t output_x,
    const std::uint32_t output_y,
    const Settings& settings,
    const bool diagnostic_trace,
    const std::uint64_t diagnostic_sequence
) noexcept {
    if (!settings.enabled || command_list == nullptr || color == nullptr ||
        output == nullptr || color == output || render_width == 0U ||
        render_height == 0U || output_width == 0U || output_height == 0U) {
        return nullptr;
    }

    CropGeometry crop{};
    if (!calculate_crop(
            settings,
            render_width,
            render_height,
            output_width,
            output_height,
            output_x,
            output_y,
            crop
        )) {
        return nullptr;
    }

    ID3D12Device* device{};
    if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) ||
        device == nullptr) {
        return nullptr;
    }
    auto* const resources = find_or_create_resources(
        device,
        output,
        output_width,
        output_height
    );
    if (resources == nullptr) {
        device->Release();
        diagnostic_note_state(
            DiagnosticApi::d3d12,
            DiagnosticState::resource_initialization_failed
        );
        return nullptr;
    }

    auto* const evaluation = new (std::nothrow) D3D12Evaluation{};
    if (evaluation == nullptr) {
        device->Release();
        diagnostic_note_state(
            DiagnosticApi::d3d12,
            DiagnosticState::allocation_failed
        );
        return nullptr;
    }
    evaluation->resources = resources;
    evaluation->original_output = output;
    evaluation->render_width = render_width;
    evaluation->render_height = render_height;
    evaluation->output_width = output_width;
    evaluation->output_height = output_height;
    evaluation->color_x = color_x;
    evaluation->color_y = color_y;
    evaluation->output_x = output_x;
    evaluation->output_y = output_y;
    evaluation->composite_x = crop.output_base_x;
    evaluation->composite_y = crop.output_base_y;
    evaluation->dlss_source_x = 0U;
    evaluation->dlss_source_y = 0U;
    evaluation->crop = crop;
    evaluation->shape_width = settings.width;
    evaluation->shape_height = settings.height;
    evaluation->shape_offset_x = settings.x_offset;
    evaluation->shape_offset_y = settings.height_offset;
    evaluation->roundness = settings.roundness;
    evaluation->feather = settings.transition_width;
    evaluation->alignment_border = settings.alignment_border_enabled;
    evaluation->diagnostic_trace = diagnostic_trace;
    evaluation->diagnostic_sequence = diagnostic_sequence;

    const auto descriptor_set = resources->next_descriptor_set.fetch_add(
        1U,
        std::memory_order_relaxed
    ) % descriptor_set_count;
    evaluation->descriptor_offset =
        static_cast<std::uint64_t>(descriptor_set) * descriptors_per_set *
        resources->descriptor_size;

    auto cpu = resources->descriptors->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += evaluation->descriptor_offset;
    const auto color_description = color->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC color_srv{};
    color_srv.Format = color_description.Format;
    color_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    color_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    color_srv.Texture2D.MipLevels = 1U;
    device->CreateShaderResourceView(color, &color_srv, cpu);
    cpu.ptr += resources->descriptor_size;

    D3D12_SHADER_RESOURCE_VIEW_DESC dlss_srv{};
    dlss_srv.Format = resources->output_format;
    dlss_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    dlss_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    dlss_srv.Texture2D.MipLevels = 1U;
    device->CreateShaderResourceView(resources->dlss_output, &dlss_srv, cpu);
    cpu.ptr += resources->descriptor_size;

    D3D12_UNORDERED_ACCESS_VIEW_DESC output_uav{};
    output_uav.Format = resources->output_format;
    output_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(output, nullptr, &output_uav, cpu);
    device->Release();

    diagnostic_note_activation(DiagnosticApi::d3d12, crop);
    return evaluation;
}

ID3D12Resource* d3d12_private_output(
    const D3D12Evaluation* const evaluation
) noexcept {
    return evaluation == nullptr || evaluation->resources == nullptr
        ? nullptr
        : evaluation->resources->dlss_output;
}

CropGeometry d3d12_evaluation_crop(
    const D3D12Evaluation* const evaluation
) noexcept {
    return evaluation == nullptr ? CropGeometry{} : evaluation->crop;
}

void finish_d3d12(
    ID3D12GraphicsCommandList* const command_list,
    const NgxParameters* const parameters,
    D3D12Evaluation* const evaluation,
    const NgxResult result
) noexcept {
    if (evaluation == nullptr) {
        return;
    }

    if (parameters != nullptr) {
        auto* const mutable_parameters = const_cast<NgxParameters*>(parameters);
        mutable_parameters->Set("Output", evaluation->original_output);
        mutable_parameters->Set("Width", evaluation->original_width);
        mutable_parameters->Set("Height", evaluation->original_height);
        mutable_parameters->Set("OutWidth", evaluation->original_out_width);
        mutable_parameters->Set("OutHeight", evaluation->original_out_height);
        mutable_parameters->Set(
            "DLSS.Render.Subrect.Dimensions.Width",
            evaluation->render_width
        );
        mutable_parameters->Set(
            "DLSS.Render.Subrect.Dimensions.Height",
            evaluation->render_height
        );
        mutable_parameters->Set(
            "DLSS.Input.Color.Subrect.Base.X",
            evaluation->color_x
        );
        mutable_parameters->Set(
            "DLSS.Input.Color.Subrect.Base.Y",
            evaluation->color_y
        );
        mutable_parameters->Set(
            "DLSS.Input.Depth.Subrect.Base.X",
            evaluation->depth_x
        );
        mutable_parameters->Set(
            "DLSS.Input.Depth.Subrect.Base.Y",
            evaluation->depth_y
        );
        mutable_parameters->Set(
            "DLSS.Input.MV.Subrect.Base.X",
            evaluation->motion_x
        );
        mutable_parameters->Set(
            "DLSS.Input.MV.Subrect.Base.Y",
            evaluation->motion_y
        );
        mutable_parameters->Set(
            "DLSS.Output.Subrect.Base.X",
            evaluation->output_x
        );
        mutable_parameters->Set(
            "DLSS.Output.Subrect.Base.Y",
            evaluation->output_y
        );
        mutable_parameters->Set(
            "DLSS.Enable.Output.Subrects",
            evaluation->original_output_subrects
        );
        mutable_parameters->Set("Reset", evaluation->reset);
    }

    if (command_list != nullptr && ngx_succeeded(result)) {
        auto* const resources = evaluation->resources;
        if (evaluation->diagnostic_trace) {
            trace_event(
                "SL eval=%llu composite UAV barrier begin",
                static_cast<unsigned long long>(
                    evaluation->diagnostic_sequence
                )
            );
        }
        insert_uav_barrier(command_list, resources->dlss_output);
        if (evaluation->diagnostic_trace) {
            trace_event(
                "SL eval=%llu composite scratch transition begin",
                static_cast<unsigned long long>(
                    evaluation->diagnostic_sequence
                )
            );
        }
        transition_resource(
            command_list,
            resources->dlss_output,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
        );
        if (evaluation->diagnostic_trace) {
            trace_event(
                "SL eval=%llu composite descriptor/root binding begin",
                static_cast<unsigned long long>(
                    evaluation->diagnostic_sequence
                )
            );
        }

        const CompositeConstants constants{
            {evaluation->output_width, evaluation->output_height},
            {evaluation->output_x, evaluation->output_y},
            {evaluation->color_x, evaluation->color_y},
            {evaluation->render_width, evaluation->render_height},
            {
                evaluation->composite_x,
                evaluation->composite_y,
            },
            {evaluation->crop.output_width, evaluation->crop.output_height},
            evaluation->shape_width,
            evaluation->shape_height,
            evaluation->shape_offset_x,
            evaluation->shape_offset_y,
            evaluation->roundness,
            evaluation->feather,
            {
                evaluation->dlss_source_x,
                evaluation->dlss_source_y,
            },
            evaluation->alignment_border ? 1U : 0U,
        };

        ID3D12DescriptorHeap* heaps[] = {resources->descriptors};
        command_list->SetDescriptorHeaps(1U, heaps);
        command_list->SetComputeRootSignature(resources->root_signature);
        command_list->SetPipelineState(resources->composite_pipeline);
        auto gpu = resources->descriptors->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += evaluation->descriptor_offset;
        command_list->SetComputeRootDescriptorTable(0U, gpu);
        gpu.ptr += static_cast<std::uint64_t>(2U) * resources->descriptor_size;
        command_list->SetComputeRootDescriptorTable(1U, gpu);
        command_list->SetComputeRoot32BitConstants(
            2U,
            21U,
            &constants,
            0U
        );
        if (evaluation->diagnostic_trace) {
            trace_event(
                "SL eval=%llu composite dispatch begin groups=%ux%u",
                static_cast<unsigned long long>(
                    evaluation->diagnostic_sequence
                ),
                (evaluation->output_width + 15U) / 16U,
                (evaluation->output_height + 15U) / 16U
            );
        }
        command_list->Dispatch(
            (evaluation->output_width + 15U) / 16U,
            (evaluation->output_height + 15U) / 16U,
            1U
        );
        if (evaluation->diagnostic_trace) {
            trace_event(
                "SL eval=%llu composite dispatch recorded",
                static_cast<unsigned long long>(
                    evaluation->diagnostic_sequence
                )
            );
        }
        insert_uav_barrier(command_list, evaluation->original_output);
        transition_resource(
            command_list,
            resources->dlss_output,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS
        );
        if (evaluation->diagnostic_trace) {
            trace_event(
                "SL eval=%llu composite barriers complete",
                static_cast<unsigned long long>(
                    evaluation->diagnostic_sequence
                )
            );
        }
    }
    delete evaluation;
}


void finish_d3d12_streamline(
    ID3D12GraphicsCommandList* const command_list,
    D3D12Evaluation* const evaluation,
    const bool succeeded
) noexcept {
    finish_d3d12(
        command_list,
        nullptr,
        evaluation,
        succeeded ? 0U : 0xBAD00000U
    );
}

NgxResult evaluate_d3d12_backend(
    ID3D12GraphicsCommandList* const command_list,
    const DlssFrameContract& contract,
    const D3D12DlssInputs& inputs,
    NgxParameters* const parameters,
    const CropGeometry& crop,
    const D3D12BackendCallbacks& callbacks,
    D3D12BackendTiming* const timing
) noexcept {
    if (command_list == nullptr || parameters == nullptr ||
        inputs.color == nullptr || inputs.depth == nullptr ||
        inputs.motion_vectors == nullptr || inputs.output == nullptr ||
        callbacks.create_feature == nullptr ||
        callbacks.evaluate_feature == nullptr ||
        callbacks.release_feature == nullptr || contract.view_id == 0U ||
        crop.input_width == 0U || crop.input_height == 0U ||
        crop.output_width < 32U || crop.output_height < 32U) {
        const auto rejection_sequence = canonical_rejection_sequence.fetch_add(
            1U, std::memory_order_relaxed
        );
        if (rejection_sequence < 16U || rejection_sequence % 300U == 0U) {
            trace_event(
                "D3D12 canonical rejected seq=%llu cmd=%p params=%p "
                "color=%p depth=%p mv=%p output=%p create=%p eval=%p "
                "release=%p view=%llu crop=%ux%u->%ux%u",
                static_cast<unsigned long long>(rejection_sequence),
                command_list,
                parameters,
                inputs.color,
                inputs.depth,
                inputs.motion_vectors,
                inputs.output,
                reinterpret_cast<void*>(callbacks.create_feature),
                reinterpret_cast<void*>(callbacks.evaluate_feature),
                reinterpret_cast<void*>(callbacks.release_feature),
                static_cast<unsigned long long>(contract.view_id),
                crop.input_width,
                crop.input_height,
                crop.output_width,
                crop.output_height
            );
        }
        return 0xBAD00005U;
    }

    const auto saved = capture_parameters(parameters);
    parameters->Set("Color", inputs.color);
    parameters->Set("Depth", inputs.depth);
    parameters->Set("MotionVectors", inputs.motion_vectors);
    if (inputs.exposure != nullptr) parameters->Set("ExposureTexture", inputs.exposure);
    parameters->Set("Output", inputs.output);
    parameters->Set("Width", crop.input_width);
    parameters->Set("Height", crop.input_height);
    parameters->Set("OutWidth", crop.output_width);
    parameters->Set("OutHeight", crop.output_height);
    parameters->Set("DLSS.Render.Subrect.Dimensions.Width", crop.input_width);
    parameters->Set("DLSS.Render.Subrect.Dimensions.Height", crop.input_height);
    parameters->Set("DLSS.Input.Color.Subrect.Base.X", inputs.color_base_x);
    parameters->Set("DLSS.Input.Color.Subrect.Base.Y", inputs.color_base_y);
    parameters->Set("DLSS.Input.Depth.Subrect.Base.X", inputs.depth_base_x);
    parameters->Set("DLSS.Input.Depth.Subrect.Base.Y", inputs.depth_base_y);
    parameters->Set("DLSS.Input.MV.Subrect.Base.X", inputs.mv_base_x);
    parameters->Set("DLSS.Input.MV.Subrect.Base.Y", inputs.mv_base_y);
    parameters->Set("DLSS.Output.Subrect.Base.X", inputs.output_base_x);
    parameters->Set("DLSS.Output.Subrect.Base.Y", inputs.output_base_y);
    parameters->Set("DLSS.Enable.Output.Subrects", 0);

    NgxResult result = 0xBAD00005U;
    {
        std::lock_guard lock(canonical_views_mutex);
        auto* view = find_view(contract.view_id);
        if (view == nullptr) {
            canonical_views.push_back(CanonicalViewState{});
            view = &canonical_views.back();
            view->view_id = contract.view_id;
        }

        const auto key = make_key(contract, crop, parameters);
        const bool key_changed = !view->has_key || !same_key(view->key, key);
        const bool crop_changed = !view->has_crop ||
            std::memcmp(&view->last_crop, &crop, sizeof(crop)) != 0;
        if (key_changed && view->private_handle != nullptr &&
            view->release_feature != nullptr) {
            static_cast<void>(view->release_feature(view->private_handle));
            view->private_handle = nullptr;
        }

        if (view->private_handle == nullptr) {
            NgxHandle* created{};
            result = callbacks.create_feature(
                command_list,
                contract.feature_id,
                parameters,
                &created
            );
            trace_event(
                "D3D12 canonical create view=%llu feature=%u "
                "input=%ux%u output=%ux%u flags=0x%08X quality=%u "
                "result=0x%08X handle=%p",
                static_cast<unsigned long long>(contract.view_id),
                contract.feature_id,
                crop.input_width,
                crop.input_height,
                crop.output_width,
                crop.output_height,
                contract.create_flags,
                contract.perf_quality,
                result,
                created
            );
            if (ngx_succeeded(result) && created != nullptr) {
                view->private_handle = created;
                view->key = key;
                view->has_key = true;
                view->release_feature = callbacks.release_feature;
            } else if (ngx_succeeded(result)) {
                result = 0xBAD00005U;
            }
        }

        if (view->private_handle != nullptr) {
            if (contract.reset || key_changed || crop_changed) {
                parameters->Set("Reset", 1);
            }
            result = callbacks.evaluate_feature(
                command_list,
                view->private_handle,
                parameters,
                nullptr
            );
            if (timing != nullptr && timing->query_heap != nullptr) {
                command_list->EndQuery(
                    timing->query_heap,
                    D3D12_QUERY_TYPE_TIMESTAMP,
                    1U
                );
                timing->sr_timestamp_written = true;
            }
            if (!ngx_succeeded(result)) {
                const auto failure_sequence =
                    canonical_failure_sequence.fetch_add(
                        1U, std::memory_order_relaxed
                    );
                if (failure_sequence < 16U || failure_sequence % 300U == 0U) {
                    const auto mv_description = inputs.motion_vectors->GetDesc();
                    trace_event(
                        "D3D12 canonical evaluate FAILED seq=%llu "
                        "view=%llu result=0x%08X flags=0x%08X "
                        "crop=%ux%u->%ux%u mv=%llux%u mvBase=%u,%u "
                        "colorBase=%u,%u depthBase=%u,%u",
                        static_cast<unsigned long long>(failure_sequence),
                        static_cast<unsigned long long>(contract.view_id),
                        result,
                        contract.create_flags,
                        crop.input_width,
                        crop.input_height,
                        crop.output_width,
                        crop.output_height,
                        static_cast<unsigned long long>(mv_description.Width),
                        mv_description.Height,
                        inputs.mv_base_x,
                        inputs.mv_base_y,
                        inputs.color_base_x,
                        inputs.color_base_y,
                        inputs.depth_base_x,
                        inputs.depth_base_y
                    );
                }
            }
            view->last_crop = crop;
            view->has_crop = true;
        }
    }

    restore_parameters(parameters, saved);
    return result;
}

void release_d3d12_view(const DlssViewId view_id) noexcept {
    NgxHandle* handle{};
    D3D12ReleaseFeatureFn release{};
    {
        std::lock_guard lock(canonical_views_mutex);
        for (auto iterator = canonical_views.begin();
             iterator != canonical_views.end(); ++iterator) {
            if (iterator->view_id != view_id) continue;
            handle = iterator->private_handle;
            release = iterator->release_feature;
            canonical_views.erase(iterator);
            break;
        }
    }
    if (handle != nullptr && release != nullptr) {
        static_cast<void>(release(handle));
    }
    release_dlss_nr_view(view_id);
}

void release_d3d12_resources() noexcept {
    for (;;) {
        DlssViewId view_id{};
        {
            std::lock_guard lock(canonical_views_mutex);
            if (canonical_views.empty()) break;
            view_id = canonical_views.front().view_id;
        }
        release_d3d12_view(view_id);
    }

    AcquireSRWLockExclusive(&resources_lock);
    auto* current = resource_list;
    resource_list = nullptr;
    while (current != nullptr) {
        auto* const next = current->next;
        release_resources(current);
        current = next;
    }
    resource_use_sequence = 0U;
    ReleaseSRWLockExclusive(&resources_lock);

    AcquireSRWLockExclusive(&settings_lock);
    last_enabled = false;
    last_width_bits = 0U;
    last_height_bits = 0U;
    last_height_offset_bits = 0U;
    last_roundness_bits = 0U;
    ReleaseSRWLockExclusive(&settings_lock);
}

}  // namespace cheeky::foveated_dlss
