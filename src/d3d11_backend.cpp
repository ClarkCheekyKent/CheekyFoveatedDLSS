#include "backend.hpp"
#include "diagnostics.hpp"

#include <d3dcompiler.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>

namespace cheeky::foveated_dlss {

using D3D11CreateFeatureFn = NgxResult (*)(
    ID3D11DeviceContext*,
    std::uint32_t,
    NgxParameters*,
    NgxHandle**
);
using D3D11ReleaseFeatureFn = NgxResult (*)(NgxHandle*);

namespace {

constexpr UINT constant_buffer_size = 96U;
constexpr std::uint32_t dlss_feature_flag_mv_low_res = 1U << 1U;

struct FoveatedConstants {
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

static_assert(sizeof(FoveatedConstants) == 21U * sizeof(std::uint32_t));

struct ResourceSet {
    ID3D11DeviceContext* context{};
    ID3D11Texture2D* dlss_output{};
    ID3D11ShaderResourceView* dlss_srv{};
    ID3D11ComputeShader* composite_shader{};
    ID3D11Buffer* constant_buffer{};
    D3D11_TEXTURE2D_DESC game_output_desc{};
    std::uint32_t private_width{};
    std::uint32_t private_height{};
    std::uint64_t last_used{};
};

struct PrivateFeatureKey {
    std::uint32_t feature{};
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t perf_quality{};
    std::uint32_t create_flags{};
    std::array<std::uint32_t, 6U> presets{};
};

struct FeatureState {
    const NgxHandle* game_handle{};
    std::uint32_t feature{};
    D3D11CreateFeatureFn create_feature{};
    D3D11ReleaseFeatureFn release_feature{};
    NgxHandle* private_handle{};
    PrivateFeatureKey private_key{};
    CropGeometry last_crop{};
    bool has_private_key{};
    bool has_last_crop{};
};

std::mutex resources_mutex;
std::deque<ResourceSet> resource_sets;
std::uint64_t resource_use_sequence{};
constexpr std::size_t resource_cache_capacity = 8U;

std::mutex features_mutex;
std::deque<FeatureState> feature_states;

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

    // The private DX11 DLSS feature writes a packed crop at scratch (0, 0).
    // RectBase is where that crop belongs in the game's full-resolution output.
    const uint2 dlss_pixel = DlssOrigin + (output_pixel - RectBase);
    const float4 dlss = DlssColor.Load(int3(dlss_pixel, 0));
    GameOutput[output_pixel] = lerp(bilinear, dlss, weight);
}
)";

template <typename T>
void release(T*& object) noexcept {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
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

void release_resource_set(ResourceSet& resources) noexcept {
    release(resources.constant_buffer);
    release(resources.composite_shader);
    release(resources.dlss_srv);
    release(resources.dlss_output);
    release(resources.context);
}

[[nodiscard]] bool same_texture_desc(
    const D3D11_TEXTURE2D_DESC& left,
    const D3D11_TEXTURE2D_DESC& right
) noexcept {
    return left.Width == right.Width &&
        left.Height == right.Height &&
        left.MipLevels == right.MipLevels &&
        left.ArraySize == right.ArraySize &&
        left.Format == right.Format &&
        left.SampleDesc.Count == right.SampleDesc.Count &&
        left.SampleDesc.Quality == right.SampleDesc.Quality;
}

[[nodiscard]] bool initialize_resource_set(
    ID3D11DeviceContext* const context,
    const D3D11_TEXTURE2D_DESC& game_output_desc,
    const std::uint32_t private_width,
    const std::uint32_t private_height,
    ResourceSet& resources
) noexcept {
    if (context == nullptr || private_width == 0U || private_height == 0U) {
        return false;
    }

    ID3D11Device* device{};
    ID3DBlob* shader_blob{};
    ID3DBlob* errors{};
    ID3D11Texture2D* dlss_output{};
    ID3D11ShaderResourceView* dlss_srv{};
    ID3D11ComputeShader* composite_shader{};
    ID3D11Buffer* constant_buffer{};

    const auto clean_up = [&]() noexcept {
        release(constant_buffer);
        release(composite_shader);
        release(dlss_srv);
        release(dlss_output);
        release(errors);
        release(shader_blob);
        release(device);
    };

    context->GetDevice(&device);
    if (device == nullptr) {
        return false;
    }

    // IMPORTANT: unlike the old implementation, the private texture is the
    // size of the foveated output itself. DLSS writes the crop packed at (0, 0).
    D3D11_TEXTURE2D_DESC private_desc = game_output_desc;
    private_desc.Width = private_width;
    private_desc.Height = private_height;
    private_desc.Usage = D3D11_USAGE_DEFAULT;
    private_desc.BindFlags =
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    private_desc.CPUAccessFlags = 0U;
    private_desc.MiscFlags = 0U;

    auto result = device->CreateTexture2D(
        &private_desc,
        nullptr,
        &dlss_output
    );
    if (FAILED(result)) {
        clean_up();
        return false;
    }

    result = device->CreateShaderResourceView(
        dlss_output,
        nullptr,
        &dlss_srv
    );
    if (FAILED(result)) {
        clean_up();
        return false;
    }

    result = D3DCompile(
        composite_shader_source,
        sizeof(composite_shader_source) - 1U,
        "Cheeky Foveated DLSS-SR",
        nullptr,
        nullptr,
        "CompositeMain",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0U,
        &shader_blob,
        &errors
    );
    if (FAILED(result)) {
        clean_up();
        return false;
    }

    result = device->CreateComputeShader(
        shader_blob->GetBufferPointer(),
        shader_blob->GetBufferSize(),
        nullptr,
        &composite_shader
    );
    if (FAILED(result)) {
        clean_up();
        return false;
    }

    D3D11_BUFFER_DESC buffer_desc{};
    buffer_desc.ByteWidth = constant_buffer_size;
    buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    result = device->CreateBuffer(&buffer_desc, nullptr, &constant_buffer);
    if (FAILED(result)) {
        clean_up();
        return false;
    }

    resources.context = context;
    resources.context->AddRef();
    resources.dlss_output = dlss_output;
    resources.dlss_srv = dlss_srv;
    resources.composite_shader = composite_shader;
    resources.constant_buffer = constant_buffer;
    resources.game_output_desc = game_output_desc;
    resources.private_width = private_width;
    resources.private_height = private_height;

    dlss_output = nullptr;
    dlss_srv = nullptr;
    composite_shader = nullptr;
    constant_buffer = nullptr;
    clean_up();
    return true;
}

[[nodiscard]] ResourceSet* find_or_create_resources(
    ID3D11DeviceContext* const context,
    const D3D11_TEXTURE2D_DESC& game_output_desc,
    const std::uint32_t private_width,
    const std::uint32_t private_height
) noexcept {
    std::lock_guard lock(resources_mutex);
    for (auto& resources : resource_sets) {
        if (resources.context == context &&
            resources.private_width == private_width &&
            resources.private_height == private_height &&
            same_texture_desc(resources.game_output_desc, game_output_desc)) {
            resources.last_used = ++resource_use_sequence;
            return &resources;
        }
    }

    ResourceSet resources{};
    if (!initialize_resource_set(
            context,
            game_output_desc,
            private_width,
            private_height,
            resources
        )) {
        return nullptr;
    }

    resources.last_used = ++resource_use_sequence;
    resource_sets.push_back(resources);
    if (resource_sets.size() > resource_cache_capacity) {
        const auto oldest = std::min_element(
            resource_sets.begin(),
            resource_sets.end(),
            [](const ResourceSet& left, const ResourceSet& right) noexcept {
                return left.last_used < right.last_used;
            }
        );
        if (oldest != resource_sets.end() && &*oldest != &resource_sets.back()) {
            release_resource_set(*oldest);
            resource_sets.erase(oldest);
        }
    }
    return &resource_sets.back();
}

[[nodiscard]] ID3D11Resource* get_resource(
    const NgxParameters* const parameters,
    const char* const name
) noexcept {
    ID3D11Resource* resource{};
    return parameters != nullptr &&
        ngx_succeeded(parameters->Get(name, &resource))
        ? resource
        : nullptr;
}

[[nodiscard]] bool in_bounds(
    const std::uint32_t base,
    const std::uint32_t size,
    const std::uint32_t capacity
) noexcept {
    return base <= capacity && size <= capacity - base;
}

[[nodiscard]] bool same_crop(
    const CropGeometry& left,
    const CropGeometry& right
) noexcept {
    return std::memcmp(&left, &right, sizeof(CropGeometry)) == 0;
}

[[nodiscard]] bool same_private_key(
    const PrivateFeatureKey& left,
    const PrivateFeatureKey& right
) noexcept {
    return left.feature == right.feature &&
        left.render_width == right.render_width &&
        left.render_height == right.render_height &&
        left.output_width == right.output_width &&
        left.output_height == right.output_height &&
        left.perf_quality == right.perf_quality &&
        left.create_flags == right.create_flags &&
        left.presets == right.presets;
}

[[nodiscard]] PrivateFeatureKey make_private_key(
    const std::uint32_t feature,
    const NgxParameters* const parameters,
    const CropGeometry& crop
) noexcept {
    PrivateFeatureKey key{};
    key.feature = feature;
    key.render_width = crop.input_width;
    key.render_height = crop.input_height;
    key.output_width = crop.output_width;
    key.output_height = crop.output_height;
    key.perf_quality = read_integer_bits(parameters, "PerfQualityValue");
    key.create_flags = read_integer_bits(parameters, "DLSS.Feature.Create.Flags");

    constexpr std::array<const char*, 6U> preset_names{
        "DLSS.Hint.Render.Preset.DLAA",
        "DLSS.Hint.Render.Preset.Quality",
        "DLSS.Hint.Render.Preset.Balanced",
        "DLSS.Hint.Render.Preset.Performance",
        "DLSS.Hint.Render.Preset.UltraPerformance",
        "DLSS.Hint.Render.Preset.UltraQuality",
    };
    for (std::size_t index{}; index < preset_names.size(); ++index) {
        key.presets[index] = read_integer_bits(parameters, preset_names[index]);
    }
    return key;
}

struct CreateParameterState {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t out_width{};
    std::uint32_t out_height{};
    std::uint32_t render_subrect_width{};
    std::uint32_t render_subrect_height{};
    std::uint32_t output_base_x{};
    std::uint32_t output_base_y{};
    int output_subrects{};
};

[[nodiscard]] CreateParameterState capture_create_parameters(
    const NgxParameters* const parameters
) noexcept {
    return {
        read_ui(parameters, "Width"),
        read_ui(parameters, "Height"),
        read_ui(parameters, "OutWidth"),
        read_ui(parameters, "OutHeight"),
        read_ui(parameters, "DLSS.Render.Subrect.Dimensions.Width"),
        read_ui(parameters, "DLSS.Render.Subrect.Dimensions.Height"),
        read_ui(parameters, "DLSS.Output.Subrect.Base.X"),
        read_ui(parameters, "DLSS.Output.Subrect.Base.Y"),
        read_i(parameters, "DLSS.Enable.Output.Subrects"),
    };
}

void restore_create_parameters(
    NgxParameters* const parameters,
    const CreateParameterState& state
) noexcept {
    parameters->Set("Width", state.width);
    parameters->Set("Height", state.height);
    parameters->Set("OutWidth", state.out_width);
    parameters->Set("OutHeight", state.out_height);
    parameters->Set(
        "DLSS.Render.Subrect.Dimensions.Width",
        state.render_subrect_width
    );
    parameters->Set(
        "DLSS.Render.Subrect.Dimensions.Height",
        state.render_subrect_height
    );
    parameters->Set("DLSS.Output.Subrect.Base.X", state.output_base_x);
    parameters->Set("DLSS.Output.Subrect.Base.Y", state.output_base_y);
    parameters->Set("DLSS.Enable.Output.Subrects", state.output_subrects);
}

[[nodiscard]] FeatureState* find_feature_state_locked(
    const NgxHandle* const game_handle
) noexcept {
    for (auto& state : feature_states) {
        if (state.game_handle == game_handle) {
            return &state;
        }
    }
    return nullptr;
}

[[nodiscard]] bool ensure_private_feature(
    ID3D11DeviceContext* const context,
    const NgxHandle* const game_handle,
    const NgxParameters* const parameters,
    const CropGeometry& crop,
    NgxHandle*& private_handle,
    bool& force_reset
) noexcept {
    private_handle = nullptr;
    force_reset = false;
    if (context == nullptr || game_handle == nullptr || parameters == nullptr) {
        return false;
    }

    const PrivateFeatureKey key = [&]() noexcept {
        std::lock_guard lock(features_mutex);
        const auto* const state = find_feature_state_locked(game_handle);
        return state == nullptr
            ? PrivateFeatureKey{}
            : make_private_key(state->feature, parameters, crop);
    }();

    D3D11CreateFeatureFn create_feature{};
    D3D11ReleaseFeatureFn release_feature{};
    NgxHandle* old_private_handle{};
    bool key_changed{};
    bool crop_changed{};

    {
        std::lock_guard lock(features_mutex);
        auto* const state = find_feature_state_locked(game_handle);
        if (state == nullptr || state->create_feature == nullptr ||
            state->release_feature == nullptr) {
            return false;
        }

        create_feature = state->create_feature;
        release_feature = state->release_feature;
        key_changed = !state->has_private_key ||
            !same_private_key(state->private_key, key);
        crop_changed = !state->has_last_crop ||
            !same_crop(state->last_crop, crop);

        if (!key_changed) {
            state->last_crop = crop;
            state->has_last_crop = true;
            private_handle = state->private_handle;
            force_reset = crop_changed;
            return private_handle != nullptr;
        }

        // Detach the old private handle while holding the state lock, but never
        // call NGX while holding it. Public NGX can enter the hooked core NGX
        // exports internally; holding features_mutex across that call would
        // deadlock when the nested hook tries to register/unregister a handle.
        old_private_handle = state->private_handle;
        state->private_handle = nullptr;
        state->has_private_key = false;
        state->last_crop = crop;
        state->has_last_crop = true;
    }

    if (old_private_handle != nullptr) {
        static_cast<void>(release_feature(old_private_handle));
    }

    auto* const mutable_parameters = const_cast<NgxParameters*>(parameters);
    const auto saved = capture_create_parameters(parameters);

    // Create a *separate* DLSS instance whose creation-time target exactly
    // matches the foveated crop. Output subrects do not resize an existing
    // feature: their output dimensions come from feature creation.
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
    mutable_parameters->Set("DLSS.Output.Subrect.Base.X", 0U);
    mutable_parameters->Set("DLSS.Output.Subrect.Base.Y", 0U);
    mutable_parameters->Set("DLSS.Enable.Output.Subrects", 0);

    NgxHandle* created{};
    const auto result = create_feature(
        context,
        key.feature,
        mutable_parameters,
        &created
    );

    restore_create_parameters(mutable_parameters, saved);

    if (!ngx_succeeded(result) || created == nullptr) {
        return false;
    }

    bool keep_created{};
    {
        std::lock_guard lock(features_mutex);
        auto* const state = find_feature_state_locked(game_handle);
        if (state != nullptr) {
            state->private_handle = created;
            state->private_key = key;
            state->has_private_key = true;
            private_handle = created;
            keep_created = true;
        }
    }

    if (!keep_created) {
        static_cast<void>(release_feature(created));
        return false;
    }

    force_reset = true;
    return true;
}

}  // namespace

struct D3D11Evaluation {
    const NgxHandle* private_handle{};
    ID3D11Resource* original_output{};
    ID3D11Texture2D* private_output{};
    ID3D11ShaderResourceView* color_srv{};
    ID3D11ShaderResourceView* dlss_srv{};
    ID3D11UnorderedAccessView* output_uav{};
    ID3D11ComputeShader* composite_shader{};
    ID3D11Buffer* constant_buffer{};

    std::uint32_t original_width{};
    std::uint32_t original_height{};
    std::uint32_t original_out_width{};
    std::uint32_t original_out_height{};
    std::uint32_t original_render_width{};
    std::uint32_t original_render_height{};
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
    int output_subrects{};
    int reset{};
    CropGeometry crop{};
    Settings settings{};
};

namespace {

void destroy_evaluation(D3D11Evaluation* const evaluation) noexcept {
    if (evaluation == nullptr) {
        return;
    }
    release(evaluation->constant_buffer);
    release(evaluation->composite_shader);
    release(evaluation->output_uav);
    release(evaluation->dlss_srv);
    release(evaluation->color_srv);
    release(evaluation->private_output);
    release(evaluation->original_output);
    delete evaluation;
}

}  // namespace

extern "C" void register_d3d11_game_feature(
    const NgxHandle* const game_handle,
    const std::uint32_t feature,
    D3D11CreateFeatureFn const create_feature,
    D3D11ReleaseFeatureFn const release_feature
) noexcept {
    if (game_handle == nullptr || create_feature == nullptr) {
        return;
    }

    std::lock_guard lock(features_mutex);
    if (auto* const existing = find_feature_state_locked(game_handle);
        existing != nullptr) {
        existing->feature = feature;
        existing->create_feature = create_feature;
        if (release_feature != nullptr) {
            existing->release_feature = release_feature;
        }
        return;
    }

    FeatureState state{};
    state.game_handle = game_handle;
    state.feature = feature;
    state.create_feature = create_feature;
    state.release_feature = release_feature;
    feature_states.push_back(state);
}

extern "C" void unregister_d3d11_game_feature(
    const NgxHandle* const game_handle
) noexcept {
    if (game_handle == nullptr) {
        return;
    }

    NgxHandle* private_handle{};
    D3D11ReleaseFeatureFn release_feature{};
    {
        std::lock_guard lock(features_mutex);
        for (auto iterator = feature_states.begin();
             iterator != feature_states.end(); ++iterator) {
            if (iterator->game_handle != game_handle) {
                continue;
            }

            private_handle = iterator->private_handle;
            release_feature = iterator->release_feature;
            feature_states.erase(iterator);
            break;
        }
    }

    // Same rule as private creation: do not call NGX while holding the state
    // mutex because a public release can enter our core release hook.
    if (private_handle != nullptr && release_feature != nullptr) {
        static_cast<void>(release_feature(private_handle));
    }
}

extern "C" D3D11Evaluation* prepare_d3d11_private(
    ID3D11DeviceContext* const context,
    const NgxHandle* const game_handle,
    const NgxParameters* const parameters,
    const Settings& settings
) noexcept {
    if (!settings.enabled) {
        diagnostic_note_state(
            DiagnosticApi::d3d11,
            DiagnosticState::disabled
        );
        return nullptr;
    }
    if (context == nullptr || game_handle == nullptr || parameters == nullptr) {
        diagnostic_note_state(
            DiagnosticApi::d3d11,
            DiagnosticState::invalid_arguments
        );
        return nullptr;
    }
    const auto effective_settings = settings_for_view(
        settings,
        static_cast<DlssViewId>(reinterpret_cast<std::uintptr_t>(game_handle))
    );

    auto* const color_resource = get_resource(parameters, "Color");
    auto* const output_resource = get_resource(parameters, "Output");
    if (color_resource == nullptr || output_resource == nullptr ||
        color_resource == output_resource) {
        diagnostic_note_state(
            DiagnosticApi::d3d11,
            DiagnosticState::missing_resources
        );
        return nullptr;
    }

    ID3D11Texture2D* color{};
    ID3D11Texture2D* output{};
    if (FAILED(color_resource->QueryInterface(IID_PPV_ARGS(&color))) ||
        FAILED(output_resource->QueryInterface(IID_PPV_ARGS(&output)))) {
        release(color);
        release(output);
        diagnostic_note_state(
            DiagnosticApi::d3d11,
            DiagnosticState::unsupported_resources
        );
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC color_desc{};
    D3D11_TEXTURE2D_DESC output_desc{};
    color->GetDesc(&color_desc);
    output->GetDesc(&output_desc);
    release(color);
    release(output);

    const auto width = read_ui(parameters, "Width");
    const auto height = read_ui(parameters, "Height");
    const auto out_width = read_ui(parameters, "OutWidth");
    const auto out_height = read_ui(parameters, "OutHeight");
    const auto original_render_width = read_ui(
        parameters,
        "DLSS.Render.Subrect.Dimensions.Width"
    );
    const auto original_render_height = read_ui(
        parameters,
        "DLSS.Render.Subrect.Dimensions.Height"
    );
    const auto active_render_width = original_render_width == 0U
        ? width
        : original_render_width;
    const auto active_render_height = original_render_height == 0U
        ? height
        : original_render_height;

    const auto color_x = read_ui(
        parameters,
        "DLSS.Input.Color.Subrect.Base.X"
    );
    const auto color_y = read_ui(
        parameters,
        "DLSS.Input.Color.Subrect.Base.Y"
    );
    const auto output_x = read_ui(parameters, "DLSS.Output.Subrect.Base.X");
    const auto output_y = read_ui(parameters, "DLSS.Output.Subrect.Base.Y");

    CropGeometry crop{};
    if (width == 0U || height == 0U || out_width == 0U || out_height == 0U ||
        active_render_width == 0U || active_render_height == 0U ||
        output_desc.MipLevels != 1U || output_desc.ArraySize != 1U ||
        output_desc.SampleDesc.Count != 1U ||
        (output_desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) == 0U ||
        (color_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0U ||
        !in_bounds(color_x, active_render_width, color_desc.Width) ||
        !in_bounds(color_y, active_render_height, color_desc.Height) ||
        !in_bounds(output_x, out_width, output_desc.Width) ||
        !in_bounds(output_y, out_height, output_desc.Height) ||
        !calculate_crop(
            effective_settings,
            active_render_width,
            active_render_height,
            out_width,
            out_height,
            output_x,
            output_y,
            crop
        )) {
        diagnostic_note_state(
            DiagnosticApi::d3d11,
            DiagnosticState::incompatible_contract
        );
        return nullptr;
    }

    // DLSS supports output sizes down to 32x32. Do not attempt to create a
    // private feature below that contract; fall back to the game's own DLSS.
    if (crop.output_width < 32U || crop.output_height < 32U) {
        diagnostic_note_state(
            DiagnosticApi::d3d11,
            DiagnosticState::incompatible_contract
        );
        return nullptr;
    }
    note_stereo_view_geometry(
        static_cast<DlssViewId>(reinterpret_cast<std::uintptr_t>(game_handle)),
        active_render_width,
        active_render_height,
        out_width,
        out_height,
        crop
    );

    auto* const resources = find_or_create_resources(
        context,
        output_desc,
        crop.output_width,
        crop.output_height
    );
    if (resources == nullptr) {
        diagnostic_note_state(
            DiagnosticApi::d3d11,
            DiagnosticState::resource_initialization_failed
        );
        return nullptr;
    }

    NgxHandle* private_handle{};
    bool force_reset{};
    if (!ensure_private_feature(
            context,
            game_handle,
            parameters,
            crop,
            private_handle,
            force_reset
        )) {
        diagnostic_note_state(
            DiagnosticApi::d3d11,
            DiagnosticState::prepare_rejected
        );
        return nullptr;
    }

    auto* const evaluation = new (std::nothrow) D3D11Evaluation{};
    if (evaluation == nullptr) {
        diagnostic_note_state(
            DiagnosticApi::d3d11,
            DiagnosticState::allocation_failed
        );
        return nullptr;
    }

    ID3D11Device* device{};
    context->GetDevice(&device);
    if (device == nullptr || FAILED(device->CreateShaderResourceView(
            color_resource,
            nullptr,
            &evaluation->color_srv
        )) || FAILED(device->CreateUnorderedAccessView(
            output_resource,
            nullptr,
            &evaluation->output_uav
        ))) {
        release(device);
        destroy_evaluation(evaluation);
        diagnostic_note_state(
            DiagnosticApi::d3d11,
            DiagnosticState::resource_initialization_failed
        );
        return nullptr;
    }
    release(device);

    evaluation->private_handle = private_handle;
    evaluation->original_output = output_resource;
    evaluation->original_output->AddRef();
    evaluation->private_output = resources->dlss_output;
    evaluation->private_output->AddRef();
    evaluation->dlss_srv = resources->dlss_srv;
    evaluation->dlss_srv->AddRef();
    evaluation->composite_shader = resources->composite_shader;
    evaluation->composite_shader->AddRef();
    evaluation->constant_buffer = resources->constant_buffer;
    evaluation->constant_buffer->AddRef();

    evaluation->original_width = width;
    evaluation->original_height = height;
    evaluation->original_out_width = out_width;
    evaluation->original_out_height = out_height;
    evaluation->original_render_width = original_render_width;
    evaluation->original_render_height = original_render_height;
    evaluation->render_width = active_render_width;
    evaluation->render_height = active_render_height;
    evaluation->output_width = out_width;
    evaluation->output_height = out_height;
    evaluation->color_x = color_x;
    evaluation->color_y = color_y;
    evaluation->depth_x = read_ui(
        parameters,
        "DLSS.Input.Depth.Subrect.Base.X"
    );
    evaluation->depth_y = read_ui(
        parameters,
        "DLSS.Input.Depth.Subrect.Base.Y"
    );
    evaluation->motion_x = read_ui(
        parameters,
        "DLSS.Input.MV.Subrect.Base.X"
    );
    evaluation->motion_y = read_ui(
        parameters,
        "DLSS.Input.MV.Subrect.Base.Y"
    );
    evaluation->output_x = output_x;
    evaluation->output_y = output_y;
    evaluation->output_subrects = read_i(
        parameters,
        "DLSS.Enable.Output.Subrects"
    );
    evaluation->reset = read_i(parameters, "Reset");
    evaluation->crop = crop;
    evaluation->settings = effective_settings;

    auto* const mutable_parameters = const_cast<NgxParameters*>(parameters);

    // Evaluate the private feature with the exact contract it was created for.
    // The input resources remain the game's full-sized resources, but the input
    // bases select only the centered crop. The output is a packed private texture
    // and therefore always starts at (0, 0).
    mutable_parameters->Set(
        "Output",
        static_cast<ID3D11Resource*>(evaluation->private_output)
    );
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
        evaluation->color_x + crop.input_base_x
    );
    mutable_parameters->Set(
        "DLSS.Input.Color.Subrect.Base.Y",
        evaluation->color_y + crop.input_base_y
    );
    mutable_parameters->Set(
        "DLSS.Input.Depth.Subrect.Base.X",
        evaluation->depth_x + crop.input_base_x
    );
    mutable_parameters->Set(
        "DLSS.Input.Depth.Subrect.Base.Y",
        evaluation->depth_y + crop.input_base_y
    );

    // Motion vectors can be either render-resolution or output-resolution.
    // Preserve the coordinate space selected when the game created DLSS.
    const auto create_flags = read_integer_bits(
        parameters,
        "DLSS.Feature.Create.Flags"
    );
    const bool motion_vectors_low_res =
        (create_flags & dlss_feature_flag_mv_low_res) != 0U;
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
    if (force_reset || evaluation->reset != 0) {
        mutable_parameters->Set("Reset", 1);
    }

    diagnostic_note_activation(DiagnosticApi::d3d11, crop);
    return evaluation;
}

extern "C" const NgxHandle* d3d11_private_handle(
    const D3D11Evaluation* const evaluation
) noexcept {
    return evaluation == nullptr ? nullptr : evaluation->private_handle;
}

extern "C" bool is_d3d11_private_handle(
    const NgxHandle* const handle
) noexcept {
    if (handle == nullptr) return false;
    std::lock_guard lock(features_mutex);
    for (const auto& state : feature_states) {
        if (state.private_handle == handle) return true;
    }
    return false;
}

void finish_d3d11(
    ID3D11DeviceContext* const context,
    const NgxParameters* const parameters,
    D3D11Evaluation* const evaluation,
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
            evaluation->original_render_width
        );
        mutable_parameters->Set(
            "DLSS.Render.Subrect.Dimensions.Height",
            evaluation->original_render_height
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
            evaluation->output_subrects
        );
        mutable_parameters->Set("Reset", evaluation->reset);
    }

    if (context == nullptr || !ngx_succeeded(result)) {
        destroy_evaluation(evaluation);
        return;
    }

    // Destination is the centered location in the game output. Source is the
    // packed private-DLSS texture starting at (0, 0).
    FoveatedConstants constants{
        {evaluation->output_width, evaluation->output_height},
        {evaluation->output_x, evaluation->output_y},
        {evaluation->color_x, evaluation->color_y},
        {evaluation->render_width, evaluation->render_height},
        {evaluation->crop.output_base_x, evaluation->crop.output_base_y},
        {evaluation->crop.output_width, evaluation->crop.output_height},
        evaluation->settings.width,
        evaluation->settings.height,
        evaluation->settings.x_offset,
        evaluation->settings.height_offset,
        evaluation->settings.roundness,
        evaluation->settings.transition_width,
        {0U, 0U},
        evaluation->settings.alignment_border_enabled ? 1U : 0U,
    };

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(
            evaluation->constant_buffer,
            0U,
            D3D11_MAP_WRITE_DISCARD,
            0U,
            &mapped
        ))) {
        destroy_evaluation(evaluation);
        return;
    }
    std::memset(mapped.pData, 0, constant_buffer_size);
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(evaluation->constant_buffer, 0U);

    ID3D11ComputeShader* previous_shader{};
    std::array<ID3D11ClassInstance*, 256U> previous_classes{};
    UINT previous_class_count = static_cast<UINT>(previous_classes.size());
    context->CSGetShader(
        &previous_shader,
        previous_classes.data(),
        &previous_class_count
    );

    ID3D11ShaderResourceView* previous_srvs[2]{};
    ID3D11UnorderedAccessView* previous_uav{};
    ID3D11Buffer* previous_constant_buffer{};
    context->CSGetShaderResources(0U, 2U, previous_srvs);
    context->CSGetUnorderedAccessViews(0U, 1U, &previous_uav);
    context->CSGetConstantBuffers(0U, 1U, &previous_constant_buffer);

    ID3D11ShaderResourceView* srvs[] = {
        evaluation->color_srv,
        evaluation->dlss_srv,
    };
    context->CSSetShader(evaluation->composite_shader, nullptr, 0U);
    context->CSSetShaderResources(0U, 2U, srvs);
    context->CSSetUnorderedAccessViews(
        0U,
        1U,
        &evaluation->output_uav,
        nullptr
    );
    context->CSSetConstantBuffers(0U, 1U, &evaluation->constant_buffer);
    context->Dispatch(
        (evaluation->output_width + 15U) / 16U,
        (evaluation->output_height + 15U) / 16U,
        1U
    );

    ID3D11ShaderResourceView* null_srvs[2]{};
    ID3D11UnorderedAccessView* null_uav{};
    ID3D11Buffer* null_buffer{};
    context->CSSetShaderResources(0U, 2U, null_srvs);
    context->CSSetUnorderedAccessViews(0U, 1U, &null_uav, nullptr);
    context->CSSetConstantBuffers(0U, 1U, &null_buffer);
    context->CSSetShader(
        previous_shader,
        previous_classes.data(),
        previous_class_count
    );
    context->CSSetShaderResources(0U, 2U, previous_srvs);
    const UINT keep_uav_counter = static_cast<UINT>(-1);
    context->CSSetUnorderedAccessViews(
        0U,
        1U,
        &previous_uav,
        &keep_uav_counter
    );
    context->CSSetConstantBuffers(0U, 1U, &previous_constant_buffer);

    release(previous_constant_buffer);
    release(previous_uav);
    release(previous_srvs[1]);
    release(previous_srvs[0]);
    for (UINT index{}; index < previous_class_count; ++index) {
        release(previous_classes[index]);
    }
    release(previous_shader);
    destroy_evaluation(evaluation);
}

bool composite_d3d11_crop(
    ID3D11DeviceContext* const context,
    ID3D11Resource* const game_color,
    ID3D11Resource* const game_output,
    ID3D11Resource* const packed_dlss_output,
    const DlssFrameContract& contract,
    const CropGeometry& crop,
    const Settings& settings
) noexcept {
    if (context == nullptr || game_color == nullptr || game_output == nullptr ||
        packed_dlss_output == nullptr) return false;

    ID3D11Texture2D* output_texture{};
    if (FAILED(game_output->QueryInterface(IID_PPV_ARGS(&output_texture)))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC output_desc{};
    output_texture->GetDesc(&output_desc);
    release(output_texture);

    auto* const resources = find_or_create_resources(
        context, output_desc, crop.output_width, crop.output_height
    );
    if (resources == nullptr) return false;

    ID3D11Device* device{};
    ID3D11ShaderResourceView* color_srv{};
    ID3D11ShaderResourceView* dlss_srv{};
    ID3D11UnorderedAccessView* output_uav{};
    context->GetDevice(&device);
    const bool views_ready = device != nullptr &&
        SUCCEEDED(device->CreateShaderResourceView(game_color, nullptr, &color_srv)) &&
        SUCCEEDED(device->CreateShaderResourceView(packed_dlss_output, nullptr, &dlss_srv)) &&
        SUCCEEDED(device->CreateUnorderedAccessView(game_output, nullptr, &output_uav));
    release(device);
    if (!views_ready) {
        release(output_uav);
        release(dlss_srv);
        release(color_srv);
        return false;
    }

    const FoveatedConstants constants{
        {contract.output_width, contract.output_height},
        {contract.output_base_x, contract.output_base_y},
        {contract.color_base_x, contract.color_base_y},
        {contract.render_width, contract.render_height},
        {crop.output_base_x, crop.output_base_y},
        {crop.output_width, crop.output_height},
        settings.width, settings.height, settings.x_offset,
        settings.height_offset,
        settings.roundness, settings.transition_width,
        {0U, 0U},
        settings.alignment_border_enabled ? 1U : 0U,
    };
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(resources->constant_buffer, 0U,
            D3D11_MAP_WRITE_DISCARD, 0U, &mapped))) {
        release(output_uav);
        release(dlss_srv);
        release(color_srv);
        return false;
    }
    std::memset(mapped.pData, 0, constant_buffer_size);
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(resources->constant_buffer, 0U);

    ID3D11ComputeShader* previous_shader{};
    std::array<ID3D11ClassInstance*, 256U> previous_classes{};
    UINT previous_class_count = static_cast<UINT>(previous_classes.size());
    ID3D11ShaderResourceView* previous_srvs[2]{};
    ID3D11UnorderedAccessView* previous_uav{};
    ID3D11Buffer* previous_buffer{};
    context->CSGetShader(&previous_shader, previous_classes.data(), &previous_class_count);
    context->CSGetShaderResources(0U, 2U, previous_srvs);
    context->CSGetUnorderedAccessViews(0U, 1U, &previous_uav);
    context->CSGetConstantBuffers(0U, 1U, &previous_buffer);

    ID3D11ShaderResourceView* srvs[]{color_srv, dlss_srv};
    context->CSSetShader(resources->composite_shader, nullptr, 0U);
    context->CSSetShaderResources(0U, 2U, srvs);
    context->CSSetUnorderedAccessViews(0U, 1U, &output_uav, nullptr);
    context->CSSetConstantBuffers(0U, 1U, &resources->constant_buffer);
    context->Dispatch(
        (contract.output_width + 15U) / 16U,
        (contract.output_height + 15U) / 16U,
        1U
    );

    ID3D11ShaderResourceView* null_srvs[2]{};
    ID3D11UnorderedAccessView* null_uav{};
    ID3D11Buffer* null_buffer{};
    context->CSSetShaderResources(0U, 2U, null_srvs);
    context->CSSetUnorderedAccessViews(0U, 1U, &null_uav, nullptr);
    context->CSSetConstantBuffers(0U, 1U, &null_buffer);
    context->CSSetShader(previous_shader, previous_classes.data(), previous_class_count);
    context->CSSetShaderResources(0U, 2U, previous_srvs);
    const UINT keep_counter = static_cast<UINT>(-1);
    context->CSSetUnorderedAccessViews(0U, 1U, &previous_uav, &keep_counter);
    context->CSSetConstantBuffers(0U, 1U, &previous_buffer);

    release(previous_buffer);
    release(previous_uav);
    release(previous_srvs[1]);
    release(previous_srvs[0]);
    for (UINT index{}; index < previous_class_count; ++index) {
        release(previous_classes[index]);
    }
    release(previous_shader);
    release(output_uav);
    release(dlss_srv);
    release(color_srv);
    return true;
}

// Keep the old entry point as a safe fallback for any call site that has not
// been moved to the private-feature path yet. It deliberately does not attempt
// the invalid "smaller output via Output.Subrect.Base" contract.
D3D11Evaluation* prepare_d3d11(
    ID3D11DeviceContext* const,
    const NgxParameters* const,
    const Settings&
) noexcept {
    return nullptr;
}

void release_d3d11_resources() noexcept {
    std::deque<FeatureState> states_to_release;
    {
        std::lock_guard lock(features_mutex);
        states_to_release.swap(feature_states);
    }

    for (auto& state : states_to_release) {
        if (state.private_handle != nullptr && state.release_feature != nullptr) {
            static_cast<void>(state.release_feature(state.private_handle));
            state.private_handle = nullptr;
        }
    }

    std::lock_guard lock(resources_mutex);
    for (auto& resources : resource_sets) {
        release_resource_set(resources);
    }
    resource_sets.clear();
    resource_use_sequence = 0U;
}


}  // namespace cheeky::foveated_dlss
