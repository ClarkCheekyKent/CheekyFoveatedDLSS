#include "d3d11_peripheral_dlaa.hpp"

#include "diagnostics.hpp"
#include "peripheral_dlaa.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>

namespace cheeky::foveated_dlss {
namespace {

constexpr std::uint32_t dlss_feature_flag_mv_low_res = 1U << 1U;
constexpr std::uint32_t perf_quality_dlaa = 5U;
constexpr UINT preparation_constant_buffer_size = 32U;
constexpr std::size_t timing_slot_count = 4U;

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
    return SourceColor.Load(
        int3(int2(SourceBase) + clamp(local, int2(0, 0), maximum), 0)
    );
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

constexpr char motion_downsample_shader_source[] = R"(
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

template <typename T>
void release(T*& object) noexcept {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

[[nodiscard]] ID3D11Resource* read_resource(
    const NgxParameters* const parameters,
    const char* const name
) noexcept {
    ID3D11Resource* resource{};
    return parameters != nullptr &&
        ngx_succeeded(parameters->Get(name, &resource))
        ? resource
        : nullptr;
}

[[nodiscard]] std::uint32_t read_ui(
    const NgxParameters* const parameters,
    const char* const name
) noexcept {
    return get_ui(parameters, name);
}

[[nodiscard]] int read_i(
    const NgxParameters* const parameters,
    const char* const name
) noexcept {
    int value{};
    return parameters != nullptr &&
        ngx_succeeded(parameters->Get(name, &value))
        ? value
        : 0;
}

[[nodiscard]] std::uint32_t read_integer_bits(
    const NgxParameters* const parameters,
    const char* const name
) noexcept {
    int signed_value{};
    if (parameters != nullptr &&
        ngx_succeeded(parameters->Get(name, &signed_value))) {
        return static_cast<std::uint32_t>(signed_value);
    }
    return read_ui(parameters, name);
}

[[nodiscard]] float read_float(
    const NgxParameters* const parameters,
    const char* const name,
    const float fallback
) noexcept {
    float value{};
    return parameters != nullptr &&
        ngx_succeeded(parameters->Get(name, &value))
        ? value
        : fallback;
}

[[nodiscard]] DXGI_FORMAT typed_resource_format(
    const DXGI_FORMAT format
) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
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

[[nodiscard]] std::uint64_t dimension_distance(
    const std::uint32_t width_a,
    const std::uint32_t height_a,
    const std::uint32_t width_b,
    const std::uint32_t height_b
) noexcept {
    const auto dx = width_a > width_b ? width_a - width_b : width_b - width_a;
    const auto dy = height_a > height_b ? height_a - height_b : height_b - height_a;
    return static_cast<std::uint64_t>(dx) + static_cast<std::uint64_t>(dy);
}

struct TimingSlot {
    ID3D11Query* disjoint{};
    ID3D11Query* total_begin{};
    ID3D11Query* k_begin{};
    ID3D11Query* k_end{};
    ID3D11Query* total_end{};
    bool pending{};
    bool recording{};
};

constexpr std::size_t source_view_cache_size = 4U;

struct CachedSourceView {
    ID3D11Resource* resource{};
    ID3D11ShaderResourceView* view{};
    std::uint64_t last_used{};
};

struct SourceViewCache {
    std::array<CachedSourceView, source_view_cache_size> entries{};
    std::uint64_t sequence{};
};

struct ViewState {
    const NgxHandle* game_handle{};
    ID3D11DeviceContext* context{};
    D3D11PeripheralCreateFeatureFn create_feature{};
    D3D11PeripheralEvaluateFeatureFn evaluate_feature{};
    D3D11PeripheralReleaseFeatureFn release_feature{};
    NgxHandle* private_handle{};

    ID3D11Texture2D* color{};
    ID3D11Texture2D* depth{};
    ID3D11Texture2D* motion{};
    ID3D11Texture2D* output{};
    ID3D11UnorderedAccessView* color_uav{};
    ID3D11UnorderedAccessView* depth_uav{};
    ID3D11UnorderedAccessView* motion_uav{};
    ID3D11ShaderResourceView* output_srv{};

    SourceViewCache color_source_views{};
    SourceViewCache depth_source_views{};
    SourceViewCache motion_source_views{};

    ID3D11ComputeShader* color_shader{};
    ID3D11ComputeShader* depth_shader{};
    ID3D11ComputeShader* motion_shader{};
    ID3D11Buffer* constants{};

    std::array<TimingSlot, timing_slot_count> timing{};
    std::size_t next_timing_slot{};

    std::uint32_t render_width{};
    std::uint32_t render_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t motion_source_width{};
    std::uint32_t motion_source_height{};
    std::uint32_t working_width{};
    std::uint32_t working_height{};
    std::uint32_t peripheral_preset{5U};
    std::uint32_t create_flags{};
    DXGI_FORMAT color_format{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT depth_format{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT motion_format{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT output_format{DXGI_FORMAT_UNKNOWN};
    bool motion_vectors_low_res{};
    bool force_reset{};
};

std::recursive_mutex state_mutex;
std::deque<ViewState> states;

std::atomic<std::uint32_t> preparation_gpu_ms_bits{};
std::atomic<std::uint32_t> total_gpu_ms_bits{};
std::mutex split_timing_mutex;
std::int64_t split_timing_window_start_ns{};
double split_preparation_sum_ms{};
double split_total_sum_ms{};
std::uint32_t split_timing_sample_count{};

[[nodiscard]] std::int64_t timing_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

void store_float_bits(
    std::atomic<std::uint32_t>& destination,
    const float value
) noexcept {
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    destination.store(bits, std::memory_order_release);
}

[[nodiscard]] float load_float_bits(
    const std::atomic<std::uint32_t>& source
) noexcept {
    const auto bits = source.load(std::memory_order_acquire);
    float value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void note_split_timing(
    const float preparation_ms,
    const float total_ms
) noexcept {
    if (!(preparation_ms >= 0.0F) || !(total_ms > 0.0F)) return;
    const auto now = timing_now_ns();
    std::lock_guard lock(split_timing_mutex);
    if (split_timing_window_start_ns == 0) {
        split_timing_window_start_ns = now;
    }
    split_preparation_sum_ms += preparation_ms;
    split_total_sum_ms += total_ms;
    ++split_timing_sample_count;

    constexpr auto publish_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::milliseconds(250)
        ).count();
    if (now - split_timing_window_start_ns < publish_ns) return;

    const auto count = static_cast<double>(split_timing_sample_count);
    store_float_bits(
        preparation_gpu_ms_bits,
        static_cast<float>(split_preparation_sum_ms / count)
    );
    store_float_bits(
        total_gpu_ms_bits,
        static_cast<float>(split_total_sum_ms / count)
    );
    split_timing_window_start_ns = now;
    split_preparation_sum_ms = 0.0;
    split_total_sum_ms = 0.0;
    split_timing_sample_count = 0U;
}

void release_timing_slot(TimingSlot& slot) noexcept {
    release(slot.total_end);
    release(slot.k_end);
    release(slot.k_begin);
    release(slot.total_begin);
    release(slot.disjoint);
    slot = {};
}

void release_source_view_cache(SourceViewCache& cache) noexcept {
    for (auto& entry : cache.entries) {
        release(entry.view);
        entry = {};
    }
    cache.sequence = 0U;
}

void release_state(ViewState& state) noexcept {
    if (state.private_handle != nullptr && state.release_feature != nullptr) {
        static_cast<void>(state.release_feature(state.private_handle));
        state.private_handle = nullptr;
    }
    for (auto& slot : state.timing) release_timing_slot(slot);
    release(state.constants);
    release(state.motion_shader);
    release(state.depth_shader);
    release(state.color_shader);
    release_source_view_cache(state.motion_source_views);
    release_source_view_cache(state.depth_source_views);
    release_source_view_cache(state.color_source_views);
    release(state.output_srv);
    release(state.motion_uav);
    release(state.depth_uav);
    release(state.color_uav);
    release(state.output);
    release(state.motion);
    release(state.depth);
    release(state.color);
    release(state.context);
    state = {};
}

[[nodiscard]] bool compile_shader(
    ID3D11Device* const device,
    const char* const source,
    const std::size_t source_size,
    const char* const label,
    ID3D11ComputeShader** const shader
) noexcept {
    if (device == nullptr || source == nullptr || shader == nullptr) return false;
    ID3DBlob* bytecode{};
    ID3DBlob* errors{};
    auto result = D3DCompile(
        source,
        source_size,
        label,
        nullptr,
        nullptr,
        "Main",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0U,
        &bytecode,
        &errors
    );
    release(errors);
    if (FAILED(result) || bytecode == nullptr) {
        release(bytecode);
        return false;
    }
    result = device->CreateComputeShader(
        bytecode->GetBufferPointer(),
        bytecode->GetBufferSize(),
        nullptr,
        shader
    );
    release(bytecode);
    return SUCCEEDED(result);
}

[[nodiscard]] bool create_working_texture(
    ID3D11Device* const device,
    const std::uint32_t width,
    const std::uint32_t height,
    const DXGI_FORMAT format,
    ID3D11Texture2D** const texture
) noexcept {
    if (device == nullptr || texture == nullptr || width == 0U || height == 0U ||
        format == DXGI_FORMAT_UNKNOWN) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1U;
    desc.ArraySize = 1U;
    desc.Format = format;
    desc.SampleDesc.Count = 1U;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
        D3D11_BIND_UNORDERED_ACCESS;
    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, texture));
}

[[nodiscard]] bool initialize_timing(
    ID3D11Device* const device,
    ViewState& state
) noexcept {
    if (device == nullptr) return false;
    const D3D11_QUERY_DESC disjoint_desc{
        D3D11_QUERY_TIMESTAMP_DISJOINT, 0U
    };
    const D3D11_QUERY_DESC timestamp_desc{D3D11_QUERY_TIMESTAMP, 0U};
    for (auto& slot : state.timing) {
        if (FAILED(device->CreateQuery(&disjoint_desc, &slot.disjoint)) ||
            FAILED(device->CreateQuery(&timestamp_desc, &slot.total_begin)) ||
            FAILED(device->CreateQuery(&timestamp_desc, &slot.k_begin)) ||
            FAILED(device->CreateQuery(&timestamp_desc, &slot.k_end)) ||
            FAILED(device->CreateQuery(&timestamp_desc, &slot.total_end))) {
            for (auto& cleanup : state.timing) release_timing_slot(cleanup);
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool initialize_state(
    ViewState& state,
    ID3D11DeviceContext* const context,
    const NgxHandle* const game_handle,
    D3D11PeripheralCreateFeatureFn const create_feature,
    D3D11PeripheralEvaluateFeatureFn const evaluate_feature,
    D3D11PeripheralReleaseFeatureFn const release_feature,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const std::uint32_t motion_source_width,
    const std::uint32_t motion_source_height,
    const std::uint32_t working_width,
    const std::uint32_t working_height,
    const std::uint32_t peripheral_preset,
    const std::uint32_t create_flags,
    const bool motion_vectors_low_res,
    const DXGI_FORMAT color_format,
    const DXGI_FORMAT depth_format,
    const DXGI_FORMAT motion_format,
    const DXGI_FORMAT output_format
) noexcept {
    ID3D11Device* device{};
    context->GetDevice(&device);
    if (device == nullptr) return false;

    ViewState created{};
    created.game_handle = game_handle;
    created.context = context;
    created.context->AddRef();
    created.create_feature = create_feature;
    created.evaluate_feature = evaluate_feature;
    created.release_feature = release_feature;
    created.render_width = render_width;
    created.render_height = render_height;
    created.output_width = output_width;
    created.output_height = output_height;
    created.motion_source_width = motion_source_width;
    created.motion_source_height = motion_source_height;
    created.working_width = working_width;
    created.working_height = working_height;
    created.peripheral_preset = peripheral_preset;
    created.create_flags = create_flags;
    created.motion_vectors_low_res = motion_vectors_low_res;
    created.color_format = color_format;
    created.depth_format = depth_format;
    created.motion_format = motion_format;
    created.output_format = output_format;

    const bool resources_ready =
        create_working_texture(
            device, working_width, working_height,
            color_format, &created.color
        ) &&
        create_working_texture(
            device, working_width, working_height,
            DXGI_FORMAT_R32_FLOAT, &created.depth
        ) &&
        create_working_texture(
            device, working_width, working_height,
            motion_format, &created.motion
        ) &&
        create_working_texture(
            device, working_width, working_height,
            output_format, &created.output
        );

    bool shaders_ready = resources_ready &&
        compile_shader(
            device,
            color_downsample_shader_source,
            sizeof(color_downsample_shader_source) - 1U,
            "Cheeky peripheral DX11 color",
            &created.color_shader
        ) &&
        compile_shader(
            device,
            depth_downsample_shader_source,
            sizeof(depth_downsample_shader_source) - 1U,
            "Cheeky peripheral DX11 depth",
            &created.depth_shader
        ) &&
        compile_shader(
            device,
            motion_downsample_shader_source,
            sizeof(motion_downsample_shader_source) - 1U,
            "Cheeky peripheral DX11 motion",
            &created.motion_shader
        );

    if (shaders_ready) {
        D3D11_BUFFER_DESC buffer_desc{};
        buffer_desc.ByteWidth = preparation_constant_buffer_size;
        buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
        buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        shaders_ready = SUCCEEDED(device->CreateBuffer(
            &buffer_desc, nullptr, &created.constants
        ));
    }

    if (shaders_ready) {
        shaders_ready =
            SUCCEEDED(device->CreateUnorderedAccessView(
                created.color, nullptr, &created.color_uav
            )) &&
            SUCCEEDED(device->CreateUnorderedAccessView(
                created.depth, nullptr, &created.depth_uav
            )) &&
            SUCCEEDED(device->CreateUnorderedAccessView(
                created.motion, nullptr, &created.motion_uav
            )) &&
            SUCCEEDED(device->CreateShaderResourceView(
                created.output, nullptr, &created.output_srv
            ));
    }

    // Timing is diagnostic-only. Failure to create queries must not disable DLAA.
    if (shaders_ready) static_cast<void>(initialize_timing(device, created));
    device->Release();

    if (!shaders_ready) {
        release_state(created);
        return false;
    }

    release_state(state);
    state = created;
    return true;
}

[[nodiscard]] bool state_matches(
    const ViewState& state,
    ID3D11DeviceContext* const context,
    const NgxHandle* const game_handle,
    D3D11PeripheralCreateFeatureFn const create_feature,
    D3D11PeripheralEvaluateFeatureFn const evaluate_feature,
    D3D11PeripheralReleaseFeatureFn const release_feature,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const std::uint32_t motion_source_width,
    const std::uint32_t motion_source_height,
    const std::uint32_t working_width,
    const std::uint32_t working_height,
    const std::uint32_t peripheral_preset,
    const std::uint32_t create_flags,
    const bool motion_vectors_low_res,
    const DXGI_FORMAT color_format,
    const DXGI_FORMAT depth_format,
    const DXGI_FORMAT motion_format,
    const DXGI_FORMAT output_format
) noexcept {
    return state.game_handle == game_handle &&
        state.context == context &&
        state.create_feature == create_feature &&
        state.evaluate_feature == evaluate_feature &&
        state.release_feature == release_feature &&
        state.render_width == render_width &&
        state.render_height == render_height &&
        state.output_width == output_width &&
        state.output_height == output_height &&
        state.motion_source_width == motion_source_width &&
        state.motion_source_height == motion_source_height &&
        state.working_width == working_width &&
        state.working_height == working_height &&
        state.peripheral_preset == peripheral_preset &&
        state.create_flags == create_flags &&
        state.motion_vectors_low_res == motion_vectors_low_res &&
        state.color_format == color_format &&
        state.depth_format == depth_format &&
        state.motion_format == motion_format &&
        state.output_format == output_format;
}

[[nodiscard]] ViewState* find_or_create_state(
    ID3D11DeviceContext* const context,
    const NgxHandle* const game_handle,
    D3D11PeripheralCreateFeatureFn const create_feature,
    D3D11PeripheralEvaluateFeatureFn const evaluate_feature,
    D3D11PeripheralReleaseFeatureFn const release_feature,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const std::uint32_t motion_source_width,
    const std::uint32_t motion_source_height,
    const std::uint32_t working_width,
    const std::uint32_t working_height,
    const std::uint32_t peripheral_preset,
    const std::uint32_t create_flags,
    const bool motion_vectors_low_res,
    const DXGI_FORMAT color_format,
    const DXGI_FORMAT depth_format,
    const DXGI_FORMAT motion_format,
    const DXGI_FORMAT output_format
) noexcept {
    for (auto& state : states) {
        if (state.game_handle != game_handle) continue;
        if (state_matches(
                state, context, game_handle,
                create_feature, evaluate_feature, release_feature,
                render_width, render_height,
                output_width, output_height,
                motion_source_width, motion_source_height,
                working_width, working_height,
                peripheral_preset,
                create_flags, motion_vectors_low_res,
                color_format, depth_format, motion_format, output_format
            )) {
            return &state;
        }
        if (!initialize_state(
                state, context, game_handle,
                create_feature, evaluate_feature, release_feature,
                render_width, render_height,
                output_width, output_height,
                motion_source_width, motion_source_height,
                working_width, working_height,
                peripheral_preset,
                create_flags, motion_vectors_low_res,
                color_format, depth_format, motion_format, output_format
            )) {
            return nullptr;
        }
        return &state;
    }

    states.push_back({});
    auto& state = states.back();
    if (!initialize_state(
            state, context, game_handle,
            create_feature, evaluate_feature, release_feature,
            render_width, render_height,
            output_width, output_height,
            motion_source_width, motion_source_height,
            working_width, working_height,
            peripheral_preset,
            create_flags, motion_vectors_low_res,
            color_format, depth_format, motion_format, output_format
        )) {
        states.pop_back();
        return nullptr;
    }
    return &state;
}

struct ComputeState {
    ID3D11ComputeShader* shader{};
    std::array<ID3D11ClassInstance*, 256U> classes{};
    UINT class_count{static_cast<UINT>(classes.size())};
    ID3D11ShaderResourceView* srv{};
    ID3D11UnorderedAccessView* uav{};
    ID3D11Buffer* constant_buffer{};
};

void capture_compute_state(
    ID3D11DeviceContext* const context,
    ComputeState& state
) noexcept {
    context->CSGetShader(
        &state.shader,
        state.classes.data(),
        &state.class_count
    );
    context->CSGetShaderResources(0U, 1U, &state.srv);
    context->CSGetUnorderedAccessViews(0U, 1U, &state.uav);
    context->CSGetConstantBuffers(0U, 1U, &state.constant_buffer);
}

void restore_compute_state(
    ID3D11DeviceContext* const context,
    ComputeState& state
) noexcept {
    ID3D11ShaderResourceView* null_srv{};
    ID3D11UnorderedAccessView* null_uav{};
    ID3D11Buffer* null_buffer{};
    context->CSSetShaderResources(0U, 1U, &null_srv);
    context->CSSetUnorderedAccessViews(0U, 1U, &null_uav, nullptr);
    context->CSSetConstantBuffers(0U, 1U, &null_buffer);

    context->CSSetShader(
        state.shader,
        state.classes.data(),
        state.class_count
    );
    context->CSSetShaderResources(0U, 1U, &state.srv);
    const UINT keep_counter = static_cast<UINT>(-1);
    context->CSSetUnorderedAccessViews(0U, 1U, &state.uav, &keep_counter);
    context->CSSetConstantBuffers(0U, 1U, &state.constant_buffer);

    release(state.constant_buffer);
    release(state.uav);
    release(state.srv);
    for (UINT index{}; index < state.class_count; ++index) {
        release(state.classes[index]);
    }
    release(state.shader);
    state = {};
}

[[nodiscard]] ID3D11ShaderResourceView* get_cached_source_view(
    ID3D11DeviceContext* const context,
    SourceViewCache& cache,
    ID3D11Resource* const resource,
    const DXGI_FORMAT format
) noexcept {
    if (context == nullptr || resource == nullptr ||
        format == DXGI_FORMAT_UNKNOWN) {
        return nullptr;
    }

    const auto use = ++cache.sequence;
    for (auto& entry : cache.entries) {
        if (entry.resource != resource || entry.view == nullptr) continue;
        entry.last_used = use;
        return entry.view;
    }

    auto* target = &cache.entries.front();
    for (auto& entry : cache.entries) {
        if (entry.view == nullptr) {
            target = &entry;
            break;
        }
        if (entry.last_used < target->last_used) target = &entry;
    }

    ID3D11Device* device{};
    ID3D11ShaderResourceView* view{};
    context->GetDevice(&device);
    if (device == nullptr) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC description{};
    description.Format = format;
    description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    description.Texture2D.MipLevels = 1U;
    const auto result = device->CreateShaderResourceView(
        resource,
        &description,
        &view
    );
    device->Release();
    if (FAILED(result) || view == nullptr) {
        release(view);
        return nullptr;
    }

    release(target->view);
    target->resource = resource;
    target->view = view;
    target->last_used = use;
    return view;
}

[[nodiscard]] bool dispatch_downsample(
    ID3D11DeviceContext* const context,
    ID3D11ComputeShader* const shader,
    ID3D11Buffer* const constants,
    ID3D11ShaderResourceView* const source,
    ID3D11UnorderedAccessView* const destination,
    const std::uint32_t source_base_x,
    const std::uint32_t source_base_y,
    const std::uint32_t source_width,
    const std::uint32_t source_height,
    const std::uint32_t destination_width,
    const std::uint32_t destination_height
) noexcept {
    if (context == nullptr || shader == nullptr || constants == nullptr ||
        source == nullptr || destination == nullptr ||
        source_width == 0U || source_height == 0U ||
        destination_width == 0U || destination_height == 0U) {
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(
            constants, 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped
        ))) {
        return false;
    }
    const std::uint32_t values[8]{
        source_base_x,
        source_base_y,
        source_width,
        source_height,
        destination_width,
        destination_height,
        0U,
        0U,
    };
    std::memcpy(mapped.pData, values, sizeof(values));
    context->Unmap(constants, 0U);

    context->CSSetShader(shader, nullptr, 0U);
    context->CSSetShaderResources(0U, 1U, &source);
    context->CSSetConstantBuffers(0U, 1U, &constants);
    context->CSSetUnorderedAccessViews(0U, 1U, &destination, nullptr);
    context->Dispatch(
        (destination_width + 15U) / 16U,
        (destination_height + 15U) / 16U,
        1U
    );
    return true;
}

[[nodiscard]] bool prepare_working_inputs(
    ViewState& state,
    ID3D11Resource* const color,
    ID3D11Resource* const depth,
    ID3D11Resource* const motion,
    const std::uint32_t color_base_x,
    const std::uint32_t color_base_y,
    const std::uint32_t depth_base_x,
    const std::uint32_t depth_base_y,
    const std::uint32_t mv_base_x,
    const std::uint32_t mv_base_y,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t motion_width,
    const std::uint32_t motion_height,
    const bool reduce_color,
    const bool reduce_motion
) noexcept {
    if (!reduce_color && !reduce_motion) return true;

    ID3D11ShaderResourceView* color_srv{};
    ID3D11ShaderResourceView* depth_srv{};
    ID3D11ShaderResourceView* motion_srv{};
    if (reduce_color) {
        color_srv = get_cached_source_view(
            state.context,
            state.color_source_views,
            color,
            state.color_format
        );
        depth_srv = get_cached_source_view(
            state.context,
            state.depth_source_views,
            depth,
            state.depth_format
        );
        if (color_srv == nullptr || depth_srv == nullptr) return false;
    }
    if (reduce_motion) {
        motion_srv = get_cached_source_view(
            state.context,
            state.motion_source_views,
            motion,
            state.motion_format
        );
        if (motion_srv == nullptr) return false;
    }

    ComputeState previous{};
    capture_compute_state(state.context, previous);

    bool succeeded = true;
    if (reduce_color) {
        succeeded = dispatch_downsample(
            state.context,
            state.color_shader,
            state.constants,
            color_srv,
            state.color_uav,
            color_base_x,
            color_base_y,
            render_width,
            render_height,
            state.working_width,
            state.working_height
        );
        if (succeeded) {
            succeeded = dispatch_downsample(
                state.context,
                state.depth_shader,
                state.constants,
                depth_srv,
                state.depth_uav,
                depth_base_x,
                depth_base_y,
                render_width,
                render_height,
                state.working_width,
                state.working_height
            );
        }
    }
    if (succeeded && reduce_motion) {
        succeeded = dispatch_downsample(
            state.context,
            state.motion_shader,
            state.constants,
            motion_srv,
            state.motion_uav,
            mv_base_x,
            mv_base_y,
            motion_width,
            motion_height,
            state.working_width,
            state.working_height
        );
    }

    restore_compute_state(state.context, previous);
    return succeeded;
}

struct ParameterState {
    ID3D11Resource* color{};
    ID3D11Resource* depth{};
    ID3D11Resource* motion{};
    ID3D11Resource* output{};
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
    std::uint32_t perf_quality{};
    std::uint32_t dlaa_preset{};
    std::uint32_t create_flags{};
    int output_subrects{};
    int reset{};
    float mv_scale_x{1.0F};
    float mv_scale_y{1.0F};
    float jitter_x{};
    float jitter_y{};
};

[[nodiscard]] ParameterState capture_parameters(
    const NgxParameters* const parameters
) noexcept {
    return {
        read_resource(parameters, "Color"),
        read_resource(parameters, "Depth"),
        read_resource(parameters, "MotionVectors"),
        read_resource(parameters, "Output"),
        read_ui(parameters, "Width"),
        read_ui(parameters, "Height"),
        read_ui(parameters, "OutWidth"),
        read_ui(parameters, "OutHeight"),
        read_ui(parameters, "DLSS.Render.Subrect.Dimensions.Width"),
        read_ui(parameters, "DLSS.Render.Subrect.Dimensions.Height"),
        read_ui(parameters, "DLSS.Input.Color.Subrect.Base.X"),
        read_ui(parameters, "DLSS.Input.Color.Subrect.Base.Y"),
        read_ui(parameters, "DLSS.Input.Depth.Subrect.Base.X"),
        read_ui(parameters, "DLSS.Input.Depth.Subrect.Base.Y"),
        read_ui(parameters, "DLSS.Input.MV.Subrect.Base.X"),
        read_ui(parameters, "DLSS.Input.MV.Subrect.Base.Y"),
        read_ui(parameters, "DLSS.Output.Subrect.Base.X"),
        read_ui(parameters, "DLSS.Output.Subrect.Base.Y"),
        read_integer_bits(parameters, "PerfQualityValue"),
        read_integer_bits(parameters, "DLSS.Hint.Render.Preset.DLAA"),
        read_integer_bits(parameters, "DLSS.Feature.Create.Flags"),
        read_i(parameters, "DLSS.Enable.Output.Subrects"),
        read_i(parameters, "Reset"),
        read_float(parameters, "MV.Scale.X", 1.0F),
        read_float(parameters, "MV.Scale.Y", 1.0F),
        read_float(parameters, "Jitter.Offset.X", 0.0F),
        read_float(parameters, "Jitter.Offset.Y", 0.0F),
    };
}

void restore_parameters(
    NgxParameters* const parameters,
    const ParameterState& state
) noexcept {
    parameters->Set("Color", state.color);
    parameters->Set("Depth", state.depth);
    parameters->Set("MotionVectors", state.motion);
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
    parameters->Set("PerfQualityValue", state.perf_quality);
    parameters->Set("DLSS.Hint.Render.Preset.DLAA", state.dlaa_preset);
    parameters->Set("DLSS.Feature.Create.Flags", state.create_flags);
    parameters->Set("DLSS.Enable.Output.Subrects", state.output_subrects);
    parameters->Set("Reset", state.reset);
    parameters->Set("MV.Scale.X", state.mv_scale_x);
    parameters->Set("MV.Scale.Y", state.mv_scale_y);
    parameters->Set("Jitter.Offset.X", state.jitter_x);
    parameters->Set("Jitter.Offset.Y", state.jitter_y);
}

void configure_dlaa_parameters(
    NgxParameters* const parameters,
    const ViewState& state,
    ID3D11Resource* const color,
    ID3D11Resource* const depth,
    ID3D11Resource* const motion,
    const std::uint32_t color_base_x,
    const std::uint32_t color_base_y,
    const std::uint32_t depth_base_x,
    const std::uint32_t depth_base_y,
    const std::uint32_t mv_base_x,
    const std::uint32_t mv_base_y,
    const ParameterState& original
) noexcept {
    parameters->Set("Color", color);
    parameters->Set("Depth", depth);
    parameters->Set("MotionVectors", motion);
    parameters->Set("Output", static_cast<ID3D11Resource*>(state.output));
    parameters->Set("Width", state.working_width);
    parameters->Set("Height", state.working_height);
    parameters->Set("OutWidth", state.working_width);
    parameters->Set("OutHeight", state.working_height);
    parameters->Set(
        "DLSS.Render.Subrect.Dimensions.Width", state.working_width
    );
    parameters->Set(
        "DLSS.Render.Subrect.Dimensions.Height", state.working_height
    );
    parameters->Set("DLSS.Input.Color.Subrect.Base.X", color_base_x);
    parameters->Set("DLSS.Input.Color.Subrect.Base.Y", color_base_y);
    parameters->Set("DLSS.Input.Depth.Subrect.Base.X", depth_base_x);
    parameters->Set("DLSS.Input.Depth.Subrect.Base.Y", depth_base_y);
    parameters->Set("DLSS.Input.MV.Subrect.Base.X", mv_base_x);
    parameters->Set("DLSS.Input.MV.Subrect.Base.Y", mv_base_y);
    parameters->Set("DLSS.Output.Subrect.Base.X", 0U);
    parameters->Set("DLSS.Output.Subrect.Base.Y", 0U);
    parameters->Set("DLSS.Enable.Output.Subrects", 0);
    parameters->Set("PerfQualityValue", perf_quality_dlaa);
    parameters->Set("DLSS.Hint.Render.Preset.DLAA", state.peripheral_preset);
    parameters->Set(
        "DLSS.Feature.Create.Flags",
        original.create_flags | dlss_feature_flag_mv_low_res
    );

    const auto mv_scale_x = original.mv_scale_x *
        static_cast<float>(state.working_width) /
        static_cast<float>(state.motion_source_width);
    const auto mv_scale_y = original.mv_scale_y *
        static_cast<float>(state.working_height) /
        static_cast<float>(state.motion_source_height);
    parameters->Set("MV.Scale.X", mv_scale_x);
    parameters->Set("MV.Scale.Y", mv_scale_y);

    // NGX jitter is in render-pixel space. Keep the same sub-pixel phase on
    // the reduced peripheral working grid.
    parameters->Set(
        "Jitter.Offset.X",
        original.jitter_x * static_cast<float>(state.working_width) /
            static_cast<float>(state.render_width)
    );
    parameters->Set(
        "Jitter.Offset.Y",
        original.jitter_y * static_cast<float>(state.working_height) /
            static_cast<float>(state.render_height)
    );
}

[[nodiscard]] bool ensure_feature(
    ViewState& state,
    NgxParameters* const parameters,
    const ParameterState& original
) noexcept {
    if (state.private_handle != nullptr) return true;
    configure_dlaa_parameters(
        parameters,
        state,
        original.color,
        original.depth,
        original.motion,
        original.color_x,
        original.color_y,
        original.depth_x,
        original.depth_y,
        original.mv_x,
        original.mv_y,
        original
    );
    NgxHandle* created{};
    const auto result = state.create_feature(
        state.context,
        1U,
        parameters,
        &created
    );
    restore_parameters(parameters, original);
    if (!ngx_succeeded(result) || created == nullptr) return false;
    state.private_handle = created;
    state.force_reset = true;
    return true;
}

void resolve_timing(ViewState& state) noexcept {
    if (state.context == nullptr) return;
    for (auto& slot : state.timing) {
        if (!slot.pending || slot.disjoint == nullptr ||
            slot.total_begin == nullptr || slot.k_begin == nullptr ||
            slot.k_end == nullptr || slot.total_end == nullptr) {
            continue;
        }

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        std::uint64_t total_begin{};
        std::uint64_t k_begin{};
        std::uint64_t k_end{};
        std::uint64_t total_end{};

        const auto disjoint_result = state.context->GetData(
            slot.disjoint, &disjoint, sizeof(disjoint),
            D3D11_ASYNC_GETDATA_DONOTFLUSH
        );
        const auto total_begin_result = state.context->GetData(
            slot.total_begin, &total_begin, sizeof(total_begin),
            D3D11_ASYNC_GETDATA_DONOTFLUSH
        );
        const auto k_begin_result = state.context->GetData(
            slot.k_begin, &k_begin, sizeof(k_begin),
            D3D11_ASYNC_GETDATA_DONOTFLUSH
        );
        const auto k_end_result = state.context->GetData(
            slot.k_end, &k_end, sizeof(k_end),
            D3D11_ASYNC_GETDATA_DONOTFLUSH
        );
        const auto total_end_result = state.context->GetData(
            slot.total_end, &total_end, sizeof(total_end),
            D3D11_ASYNC_GETDATA_DONOTFLUSH
        );

        if (disjoint_result == S_FALSE ||
            total_begin_result == S_FALSE ||
            k_begin_result == S_FALSE ||
            k_end_result == S_FALSE ||
            total_end_result == S_FALSE) {
            continue;
        }

        slot.pending = false;
        if (FAILED(disjoint_result) ||
            FAILED(total_begin_result) ||
            FAILED(k_begin_result) ||
            FAILED(k_end_result) ||
            FAILED(total_end_result) ||
            disjoint.Disjoint ||
            disjoint.Frequency == 0U ||
            k_begin < total_begin ||
            k_end < k_begin ||
            total_end < k_end) {
            continue;
        }

        const auto to_ms = [&disjoint](
            const std::uint64_t begin,
            const std::uint64_t end
        ) noexcept {
            return static_cast<float>(
                static_cast<double>(end - begin) * 1000.0 /
                static_cast<double>(disjoint.Frequency)
            );
        };

        const auto preparation_ms = to_ms(total_begin, k_begin);
        const auto k_ms = to_ms(k_begin, k_end);
        const auto total_ms = to_ms(total_begin, total_end);

        diagnostic_note_peripheral_dlaa_gpu_time(
            DiagnosticApi::d3d11,
            k_ms
        );
        note_split_timing(preparation_ms, total_ms);
    }
}

[[nodiscard]] TimingSlot* begin_timing(ViewState& state) noexcept {
    resolve_timing(state);
    if (state.context == nullptr ||
        state.context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        !diagnostic_should_sample_gpu_time(
            DiagnosticGpuTiming::peripheral_dlaa
        )) {
        return nullptr;
    }
    for (std::size_t offset{}; offset < state.timing.size(); ++offset) {
        const auto index =
            (state.next_timing_slot + offset) % state.timing.size();
        auto& slot = state.timing[index];
        if (slot.disjoint == nullptr || slot.pending || slot.recording) continue;
        state.next_timing_slot = (index + 1U) % state.timing.size();
        slot.recording = true;
        state.context->Begin(slot.disjoint);
        state.context->End(slot.total_begin);
        return &slot;
    }
    return nullptr;
}

void mark_k_begin(
    ViewState& state,
    TimingSlot* const slot
) noexcept {
    if (slot == nullptr || state.context == nullptr) return;
    state.context->End(slot->k_begin);
}

void mark_k_end(
    ViewState& state,
    TimingSlot* const slot
) noexcept {
    if (slot == nullptr || state.context == nullptr) return;
    state.context->End(slot->k_end);
}

void finish_timing(
    ViewState& state,
    TimingSlot* const slot
) noexcept {
    if (slot == nullptr || state.context == nullptr) return;
    state.context->End(slot->total_end);
    state.context->End(slot->disjoint);
    slot->recording = false;
    slot->pending = true;
}

struct PeripheralTimingScope {
    ViewState* state{};
    TimingSlot* slot{};
    bool k_started{};
    bool k_finished{};

    explicit PeripheralTimingScope(ViewState& in_state) noexcept
        : state(&in_state), slot(begin_timing(in_state)) {}

    void k_begin() noexcept {
        if (state == nullptr || slot == nullptr || k_started) return;
        mark_k_begin(*state, slot);
        k_started = true;
    }

    void k_end() noexcept {
        if (state == nullptr || slot == nullptr || k_finished) return;
        if (!k_started) k_begin();
        mark_k_end(*state, slot);
        k_finished = true;
    }

    void finish() noexcept {
        if (state == nullptr) return;
        // Keep every timestamp slot complete even if preparation fails before
        // NGX evaluation, otherwise an unreadable query can permanently occupy
        // one of the small timing-ring slots.
        if (slot != nullptr && !k_finished) k_end();
        finish_timing(*state, slot);
        state = nullptr;
        slot = nullptr;
    }

    ~PeripheralTimingScope() { finish(); }
};

}  // namespace

bool evaluate_d3d11_peripheral_dlaa(
    ID3D11DeviceContext* const context,
    const NgxHandle* const game_handle,
    const NgxParameters* const parameters,
    const Settings& settings,
    D3D11PeripheralCreateFeatureFn const create_feature,
    D3D11PeripheralEvaluateFeatureFn const evaluate_feature,
    D3D11PeripheralReleaseFeatureFn const release_feature,
    D3D11PeripheralDlaaResult& output
) noexcept {
    release_d3d11_peripheral_dlaa_result(output);
    if (!settings.enabled || !settings.peripheral_dlaa_enabled ||
        context == nullptr || game_handle == nullptr || parameters == nullptr ||
        create_feature == nullptr || evaluate_feature == nullptr ||
        release_feature == nullptr) {
        return false;
    }

    const auto original = capture_parameters(parameters);
    if (original.color == nullptr || original.depth == nullptr ||
        original.motion == nullptr || original.output == nullptr) {
        return false;
    }

    auto render_width = original.render_width;
    auto render_height = original.render_height;
    if (render_width == 0U) render_width = original.width;
    if (render_height == 0U) render_height = original.height;
    if (render_width == 0U || render_height == 0U ||
        original.out_width == 0U || original.out_height == 0U) {
        return false;
    }

    ID3D11Texture2D* color_texture{};
    ID3D11Texture2D* depth_texture{};
    ID3D11Texture2D* motion_texture{};
    ID3D11Texture2D* output_texture{};
    const bool textures_ready =
        SUCCEEDED(original.color->QueryInterface(
            IID_PPV_ARGS(&color_texture)
        )) &&
        SUCCEEDED(original.depth->QueryInterface(
            IID_PPV_ARGS(&depth_texture)
        )) &&
        SUCCEEDED(original.motion->QueryInterface(
            IID_PPV_ARGS(&motion_texture)
        )) &&
        SUCCEEDED(original.output->QueryInterface(
            IID_PPV_ARGS(&output_texture)
        ));
    if (!textures_ready) {
        release(output_texture);
        release(motion_texture);
        release(depth_texture);
        release(color_texture);
        return false;
    }

    D3D11_TEXTURE2D_DESC color_desc{};
    D3D11_TEXTURE2D_DESC depth_desc{};
    D3D11_TEXTURE2D_DESC motion_desc{};
    D3D11_TEXTURE2D_DESC output_desc{};
    color_texture->GetDesc(&color_desc);
    depth_texture->GetDesc(&depth_desc);
    motion_texture->GetDesc(&motion_desc);
    output_texture->GetDesc(&output_desc);
    release(output_texture);
    release(motion_texture);
    release(depth_texture);
    release(color_texture);

    if (color_desc.SampleDesc.Count != 1U ||
        depth_desc.SampleDesc.Count != 1U ||
        motion_desc.SampleDesc.Count != 1U ||
        output_desc.SampleDesc.Count != 1U) {
        return false;
    }

    const bool flag_says_low_res =
        (original.create_flags & dlss_feature_flag_mv_low_res) != 0U;
    const auto mv_to_input = dimension_distance(
        motion_desc.Width, motion_desc.Height, render_width, render_height
    );
    const auto mv_to_output = dimension_distance(
        motion_desc.Width, motion_desc.Height,
        original.out_width, original.out_height
    );
    const bool mv_low_res = mv_to_input == mv_to_output
        ? flag_says_low_res
        : mv_to_input < mv_to_output;
    const auto mv_source_width = mv_low_res
        ? render_width
        : original.out_width;
    const auto mv_source_height = mv_low_res
        ? render_height
        : original.out_height;

    diagnostic_note_motion_vectors(
        DiagnosticApi::d3d11,
        motion_desc.Width,
        motion_desc.Height,
        mv_low_res ? MotionVectorSpace::input : MotionVectorSpace::output
    );

    const auto dimensions = peripheral_dlaa_dimensions(
        render_width,
        render_height,
        settings.peripheral_dlaa_scale
    );
    if (dimensions.width < 32U || dimensions.height < 32U) return false;

    const auto color_format = typed_resource_format(color_desc.Format);
    const auto depth_format = depth_srv_format(depth_desc.Format);
    const auto motion_format = typed_resource_format(motion_desc.Format);
    const auto output_format = typed_resource_format(output_desc.Format);
    if (color_format == DXGI_FORMAT_UNKNOWN ||
        depth_format == DXGI_FORMAT_UNKNOWN ||
        motion_format == DXGI_FORMAT_UNKNOWN ||
        output_format == DXGI_FORMAT_UNKNOWN) {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    auto* const state = find_or_create_state(
        context,
        game_handle,
        create_feature,
        evaluate_feature,
        release_feature,
        render_width,
        render_height,
        original.out_width,
        original.out_height,
        mv_source_width,
        mv_source_height,
        dimensions.width,
        dimensions.height,
        settings.peripheral_dlaa_preset,
        original.create_flags,
        mv_low_res,
        color_format,
        depth_format,
        motion_format,
        output_format
    );
    if (state == nullptr) return false;

    auto* mutable_parameters = const_cast<NgxParameters*>(parameters);
    if (!ensure_feature(*state, mutable_parameters, original)) return false;

    PeripheralTimingScope timing{*state};

    const bool reduce_color =
        dimensions.width != render_width ||
        dimensions.height != render_height;
    const bool reduce_motion =
        dimensions.width != mv_source_width ||
        dimensions.height != mv_source_height;

    ID3D11Resource* dlaa_color = original.color;
    ID3D11Resource* dlaa_depth = original.depth;
    ID3D11Resource* dlaa_motion = original.motion;
    auto color_base_x = original.color_x;
    auto color_base_y = original.color_y;
    auto depth_base_x = original.depth_x;
    auto depth_base_y = original.depth_y;
    auto mv_base_x = original.mv_x;
    auto mv_base_y = original.mv_y;

    if (!prepare_working_inputs(
            *state,
            original.color,
            original.depth,
            original.motion,
            original.color_x,
            original.color_y,
            original.depth_x,
            original.depth_y,
            original.mv_x,
            original.mv_y,
            render_width,
            render_height,
            mv_source_width,
            mv_source_height,
            reduce_color,
            reduce_motion
        )) {
        return false;
    }

    if (reduce_color) {
        dlaa_color = state->color;
        dlaa_depth = state->depth;
        color_base_x = 0U;
        color_base_y = 0U;
        depth_base_x = 0U;
        depth_base_y = 0U;
    }

    if (reduce_motion) {
        dlaa_motion = state->motion;
        mv_base_x = 0U;
        mv_base_y = 0U;
    }

    configure_dlaa_parameters(
        mutable_parameters,
        *state,
        dlaa_color,
        dlaa_depth,
        dlaa_motion,
        color_base_x,
        color_base_y,
        depth_base_x,
        depth_base_y,
        mv_base_x,
        mv_base_y,
        original
    );
    if (state->force_reset || original.reset != 0) {
        mutable_parameters->Set("Reset", 1);
    }

    timing.k_begin();
    const auto result = state->evaluate_feature(
        context,
        state->private_handle,
        mutable_parameters,
        nullptr
    );
    timing.k_end();
    restore_parameters(mutable_parameters, original);
    timing.finish();

    if (!ngx_succeeded(result)) {
        state->force_reset = true;
        return false;
    }

    state->force_reset = false;
    output.output_srv = state->output_srv;
    output.output_srv->AddRef();
    output.working_width = dimensions.width;
    output.working_height = dimensions.height;
    return true;
}

void release_d3d11_peripheral_dlaa_result(
    D3D11PeripheralDlaaResult& result
) noexcept {
    release(result.output_srv);
    result = {};
}

void release_d3d11_peripheral_dlaa_view(
    const NgxHandle* const game_handle
) noexcept {
    if (game_handle == nullptr) return;
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    for (auto iterator = states.begin(); iterator != states.end(); ++iterator) {
        if (iterator->game_handle != game_handle) continue;
        release_state(*iterator);
        states.erase(iterator);
        break;
    }
}

float d3d11_peripheral_dlaa_preparation_gpu_ms() noexcept {
    return load_float_bits(preparation_gpu_ms_bits);
}

float d3d11_peripheral_dlaa_total_gpu_ms() noexcept {
    return load_float_bits(total_gpu_ms_bits);
}

void release_d3d11_peripheral_dlaa_resources() noexcept {
    std::lock_guard<std::recursive_mutex> lock(state_mutex);
    for (auto& state : states) release_state(state);
    states.clear();
}

}  // namespace cheeky::foveated_dlss
