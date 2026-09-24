#include "dlss_nr_input.hpp"
#include "dlss_nr_lifetime.hpp"
#include "eye_calibration_d3d12.hpp"
#include "dlss_nr.hpp"
#include "nr_codec_shader.hpp"
#include "d3d_shaders.hpp"
#include "nr_parameters.hpp"
#include "nr_runtime_module.hpp"

#include "d3d12_output_contract.hpp"
#include "dlss_nr_contract.hpp"
#include "crop_motion.hpp"
#include "nr_guides.hpp"
#include "runtime.hpp"
#include "runtime_search.hpp"
#include "afw_compatibility.hpp"
#include "gaze_foveation.hpp"
#include "d3d12_ngx_dispatch.hpp"
#include "ngx_runtime_discovery.hpp"

#include <Windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <deque>
#include <mutex>
#include <utility>

namespace cheeky::foveated_dlss {
namespace {

using NgxInitExtFn = NgxResult (*)(
    unsigned long long,
    const wchar_t*,
    ID3D12Device*,
    std::uint32_t,
    const NgxParameters*
);
using NgxAllocateParametersFn = NgxResult (*)(NgxParameters**);
using NgxDestroyParametersFn = NgxResult (*)(NgxParameters*);
using NgxCreateFeatureFn = NgxResult (*)(
    ID3D12GraphicsCommandList*,
    std::uint32_t,
    NgxParameters*,
    NgxHandle**
);
using NgxEvaluateFeatureFn = NgxResult (*)(
    ID3D12GraphicsCommandList*,
    const NgxHandle*,
    const NgxParameters*,
    NgxProgressCallback
);
using NgxReleaseFeatureFn = NgxResult (*)(NgxHandle*);
using GetModuleFileNameWFn = DWORD (WINAPI*)(HMODULE, LPWSTR, DWORD);

template <typename T>
void release(T*& object) noexcept {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

struct RuntimeState {
    HMODULE module{};
    ID3D12Device* device{};
    NgxAllocateParametersFn allocate_parameters{};
    NgxDestroyParametersFn destroy_parameters{};
    NgxCreateFeatureFn create_feature{};
    NgxEvaluateFeatureFn evaluate_feature{};
    NgxReleaseFeatureFn release_feature{};
    // 0 not attempted, 1 ready, 2 missing DLL, 3 failed.
    std::uint32_t state{};
};

struct FeatureKey {
    std::uint32_t input_width{};
    std::uint32_t input_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t create_flags{};
    NrProcessingOrder order{};
    std::uint32_t processing_width{}, processing_height{};
    std::uint32_t preset{};
    std::uint32_t style{};
    float intensity{}, local_tone{}, local_structure{}, skin_structure{};
    bool automatic_mask{}, ui_correction{};

    bool operator==(const FeatureKey&) const = default;
};

bool verify_model_tuning(NgxParameters* parameters, const Settings& settings,
    DlssViewId view_id) noexcept {
    const char* names[]{"DLSSNR.Intensity", "DLSSNR.LocalToneStrength",
        "DLSSNR.LocalStructureStrength", "DLSSNR.SkinStructureStrength"};
    const float expected[]{settings.nr_intensity, settings.nr_local_tone_strength,
        settings.nr_local_structure_strength, settings.nr_skin_structure_strength};
    float readback[4]{};
    bool valid = true;
    for (unsigned i = 0; i < 4; ++i) {
        const auto result = parameters->Get(names[i], &readback[i]);
        if (!ngx_succeeded(result) || readback[i] != expected[i]) {
            valid = false;
            trace_event("DLSS-NR parameter mismatch view=%llu key=%s requested=%.6f read=%.6f result=0x%08X",
                view_id, names[i], expected[i], readback[i], result);
        }
    }
    void* mask{};
    valid = ngx_succeeded(parameters->Get("DLSSNR.ControlMask", &mask)) && !mask && valid;
    unsigned style{}, auto_mask{};
    valid = ngx_succeeded(parameters->Get("DLSSNR.Style", &style)) && style == settings.nr_style && valid;
    valid = ngx_succeeded(parameters->Get("DLSSNR.UseAutoMask", &auto_mask)) && auto_mask == (settings.nr_automatic_mask ? 1U : 0U) && valid;
    trace_event("DLSS-NR parameter readback view=%llu valid=%s style=%u intensity=%.6f tone=%.6f structure=%.6f skin=%.6f autoMask=%u skinEnabled=%s",
        view_id, valid ? "yes" : "no", style, readback[0], readback[1], readback[2], readback[3],
        auto_mask, auto_mask ? "yes" : "no");
    return valid;
}

struct GpuResources {
    bool border_only{};
    NrGuidePass guides;
    NrLifetime uses;
    std::uint64_t last_use{};
    ID3D12Resource* game_output{};
    ID3D12Resource* original_output{};
    ID3D12Resource* color_proxy{};
    ID3D12Resource* neural_output{};
    ID3D12DescriptorHeap* descriptors{};
    ID3D12RootSignature* root_signature{};
    ID3D12PipelineState* encode_pipeline{};
    ID3D12PipelineState* decode_pipeline{};
    ID3D12PipelineState* border_pipeline{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t working_width{};
    std::uint32_t working_height{};
    std::uint32_t descriptor_size{};
};

struct CachedFeature {
    FeatureKey key{};
    NgxParameters* parameters{};
    NgxHandle* handle{};
    bool reusable{true};
    NrLifetime uses;
};

struct ViewState {
    NrLifetime uses;
    bool retired{};
    DlssViewId view_id{};
    NgxParameters* parameters{};
    NgxHandle* handle{};
    FeatureKey key{};
    bool has_key{};
    bool feature_failed{};
    bool was_enabled{};
    std::uint64_t settings_signature{};
    std::uint64_t reset_generation{};
    DlssNrHistory history{};
    NrRegion last_region{};
    NrProcessingOrder last_order{};
    float jitter_uv_x{}, jitter_uv_y{};
    DlssNrRoute history_route{};
    std::uint64_t history_calls{}, history_moves{}, history_resets{};
    std::uint64_t history_corrections{}, history_compensation_failures{};
    std::deque<CachedFeature> retired_features;
    std::deque<GpuResources> gpu_resources;
    std::uint64_t gpu_use_sequence{};
};

std::mutex nr_mutex;
RuntimeState runtime;
std::deque<ViewState> views;

DlssNrSnapshot diagnostics;
// One active feature plus one alternate for full/foveated toggling. Pending
// recordings count against this budget too; exhausted caches fall back to SR.
constexpr std::size_t retired_feature_capacity = 1U;
// Two configurations across three rotating private input textures per eye.
constexpr std::size_t gpu_resource_cache_capacity = 6U;

void release_gpu(GpuResources& gpu) noexcept {
    release(gpu.border_pipeline);
    release(gpu.decode_pipeline);
    release(gpu.encode_pipeline);
    release(gpu.root_signature);
    release(gpu.descriptors);
    release(gpu.neural_output);
    release(gpu.color_proxy);
    release(gpu.original_output);
    release(gpu.game_output);
    gpu = {};
}

void release_feature(ViewState& view) noexcept {
    if (view.handle != nullptr && runtime.release_feature != nullptr) {
        static_cast<void>(runtime.release_feature(view.handle));
    }
    if (view.parameters != nullptr && runtime.destroy_parameters != nullptr) {
        static_cast<void>(runtime.destroy_parameters(view.parameters));
    }
    view.handle = nullptr;
    view.parameters = nullptr;
    view.has_key = false;
    view.feature_failed = false;
    for (const auto& retired : view.retired_features) {
        if (retired.handle != nullptr && runtime.release_feature != nullptr) {
            static_cast<void>(runtime.release_feature(retired.handle));
        }
        if (retired.parameters != nullptr && runtime.destroy_parameters != nullptr) {
            static_cast<void>(runtime.destroy_parameters(retired.parameters));
        }
    }
    view.retired_features.clear();
}

bool view_complete(ViewState& view) noexcept {
    view.uses.collect();
    return view.uses.empty();
}
bool record_use(ViewState& view, ID3D12GraphicsCommandList* list) noexcept {
    return view.uses.record(list);
}
void evict_retired_features(ViewState& view) noexcept;
void collect_retired_views() noexcept {
    for (auto it = views.begin(); it != views.end();) {
        const bool complete = view_complete(*it);
        if (it->retired && complete) {
            release_feature(*it);
            for (auto& gpu : it->gpu_resources) release_gpu(gpu);
            it = views.erase(it);
        } else {
            evict_retired_features(*it);
            for (auto& gpu : it->gpu_resources) gpu.uses.collect();
            ++it;
        }
    }
}

void evict_retired_features(ViewState& view) noexcept {
    for (auto it = view.retired_features.begin(); it != view.retired_features.end();) {
        it->uses.collect();
        if (!it->uses.empty() || (it->reusable &&
                view.retired_features.size() <= retired_feature_capacity)) {
            ++it;
            continue;
        }
        const auto retired = std::move(*it);
        it = view.retired_features.erase(it);
        if (retired.handle != nullptr && runtime.release_feature != nullptr) {
            static_cast<void>(runtime.release_feature(retired.handle));
        }
        if (retired.parameters != nullptr && runtime.destroy_parameters != nullptr) {
            static_cast<void>(runtime.destroy_parameters(retired.parameters));
        }
    }
}

[[nodiscard]] bool initialize_runtime(ID3D12Device* const device) noexcept {
    if (runtime.state == 1U) {
        if (runtime.device == device) return true;
        diagnostics.state = DlssNrState::runtime_failed;
        diagnostics.skip_reason = "NR runtime belongs to a different device";
        ++diagnostics.failed_calls;
        return false;
    }
    if (runtime.state == 2U || runtime.state == 3U || device == nullptr) {
        return false;
    }

    const auto module=load_nr_runtime_module();
    runtime.module=module.module;
    const auto& directory=module.directory;
    if(!runtime.module) {
        runtime.state=module.error==ERROR_MOD_NOT_FOUND?2U:3U;
        diagnostics.state=runtime.state==2U?DlssNrState::runtime_missing:DlssNrState::runtime_failed;
        diagnostics.last_result=module.error;
        return false;
    }

    const auto initialize = reinterpret_cast<NgxInitExtFn>(GetProcAddress(
        runtime.module,
        "NVSDK_NGX_D3D12_Init_Ext"
    ));
    auto allocate = reinterpret_cast<NgxAllocateParametersFn>(GetProcAddress(
        runtime.module,
        "NVSDK_NGX_D3D12_AllocateParameters"
    ));
    auto destroy = reinterpret_cast<NgxDestroyParametersFn>(GetProcAddress(
        runtime.module,
        "NVSDK_NGX_D3D12_DestroyParameters"
    ));
    runtime.create_feature = reinterpret_cast<NgxCreateFeatureFn>(GetProcAddress(
        runtime.module,
        "NVSDK_NGX_D3D12_CreateFeature"
    ));
    runtime.evaluate_feature = reinterpret_cast<NgxEvaluateFeatureFn>(
        GetProcAddress(runtime.module, "NVSDK_NGX_D3D12_EvaluateFeature")
    );
    runtime.release_feature = reinterpret_cast<NgxReleaseFeatureFn>(GetProcAddress(
        runtime.module,
        "NVSDK_NGX_D3D12_ReleaseFeature"
    ));
    if (allocate == nullptr || destroy == nullptr) {
        const auto core = find_loaded_ngx_core_runtime();
        if (core != nullptr) {
            allocate = reinterpret_cast<NgxAllocateParametersFn>(GetProcAddress(
                core,
                "NVSDK_NGX_D3D12_AllocateParameters"
            ));
            destroy = reinterpret_cast<NgxDestroyParametersFn>(GetProcAddress(
                core,
                "NVSDK_NGX_D3D12_DestroyParameters"
            ));
        }
    }
    if (initialize == nullptr || allocate == nullptr || destroy == nullptr ||
        runtime.create_feature == nullptr || runtime.evaluate_feature == nullptr ||
        runtime.release_feature == nullptr) {
        runtime.state = 3U;
        diagnostics.state = DlssNrState::runtime_failed;
        trace_event("DLSS-NR runtime has an incomplete NGX export set");
        return false;
    }

    constexpr unsigned long long application_id = 0x0876232cULL;
    constexpr std::uint32_t ngx_sdk_version = 0x15U;
    const auto result = initialize(
        application_id,
        directory.data(),
        device,
        ngx_sdk_version,
        nullptr
    );
    diagnostics.last_result = result;
    if (!ngx_succeeded(result)) {
        runtime.state = 3U;
        diagnostics.state = DlssNrState::runtime_failed;
        trace_event("DLSS-NR Init_Ext failed result=0x%08X", result);
        return false;
    }
    runtime.allocate_parameters = allocate;
    runtime.destroy_parameters = destroy;
    runtime.device = device;
    runtime.device->AddRef();
    runtime.state = 1U;
    trace_event("DLSS-NR 310.8 feature-18 runtime initialized");
    return true;
}

[[nodiscard]] ViewState& find_or_create_view(const DlssViewId view_id) {
    for (auto& view : views) {
        if (view.view_id == view_id && !view.retired) return view;
    }
    views.push_back(ViewState{});
    views.back().view_id = view_id;
    return views.back();
}

[[nodiscard]] bool create_feature(
    ViewState& view,
    const DlssNrFrame& frame,
    const Settings& settings,
    const std::uint32_t working_width,
    const std::uint32_t working_height
) noexcept {
    const FeatureKey key{
        working_width,
        working_height,
        working_width,
        working_height,
        frame.create_flags,
        settings.nr_processing_order,
        frame.processing_width ? frame.processing_width : frame.output_width,
        frame.processing_height ? frame.processing_height : frame.output_height,
        settings.nr_preset,
        settings.nr_style,
        settings.nr_intensity, settings.nr_local_tone_strength,
        settings.nr_local_structure_strength, settings.nr_skin_structure_strength,
        settings.nr_automatic_mask, settings.nr_ui_correction,
    };
    if (view.handle != nullptr && view.has_key && view.key == key) return true;
    if (view.feature_failed && view.has_key && view.key == key) return false;
    for (auto iterator = view.retired_features.begin();
         iterator != view.retired_features.end(); ++iterator) {
        if (!iterator->reusable || !(iterator->key == key)) continue;
        const CachedFeature current{view.key, view.parameters, view.handle, true, view.uses};
        view.parameters = iterator->parameters;
        view.handle = iterator->handle;
        view.retired_features.erase(iterator);
        if (current.parameters != nullptr || current.handle != nullptr) {
            view.retired_features.push_back(current);
        }
        evict_retired_features(view);
        view.key = key;
        view.has_key = true;
        view.feature_failed = false;
        view.was_enabled = false;
        view.settings_signature = 0U;
        return true;
    }
    if (view.handle != nullptr || view.parameters != nullptr) {
        // Retain a bounded set of replaced features. This both avoids releasing
        // work still in flight and makes common on/off foveation sizes reusable.
        view.retired_features.push_back({view.key, view.parameters, view.handle, true, view.uses});
        view.parameters = nullptr;
        view.handle = nullptr;
        evict_retired_features(view);
    }
    // Reserve the second slot for the new active feature. Never accumulate
    // additional feature instances while the old recordings are still live.
    evict_retired_features(view);
    if (view.retired_features.size() > retired_feature_capacity) {
        diagnostics.state = DlssNrState::input_preparation_failed;
        diagnostics.skip_reason = "NR feature cache busy; using original color";
        ++diagnostics.failed_calls;
        return false;
    }
    view.feature_failed = false;
    view.key = key;
    view.has_key = true;

    NgxParameters* parameters{};
    auto result = runtime.allocate_parameters(&parameters);
    if (!ngx_succeeded(result) || parameters == nullptr) {
        diagnostics.state = DlssNrState::feature_failed;
        diagnostics.last_result = result;
        ++diagnostics.failed_calls;
        view.feature_failed = true;
        return false;
    }
    const auto preset = settings.nr_preset;
    parameters->Set("Width", working_width);
    parameters->Set("Height", working_height);
    parameters->Set("OutWidth", working_width);
    parameters->Set("OutHeight", working_height);
    parameters->Set("DLSSNR.Width", working_width);
    parameters->Set("DLSSNR.Height", working_height);
    parameters->Set("DLSSNR.InputWidth", working_width);
    parameters->Set("DLSSNR.InputHeight", working_height);
    parameters->Set("DLSSNR.OutputWidth", working_width);
    parameters->Set("DLSSNR.OutputHeight", working_height);
    parameters->Set("DLSSNR.Output.Width", working_width);
    parameters->Set("DLSSNR.Output.Height", working_height);
    parameters->Set("DLSSNR.ScalingRatio", 1.0F);
    parameters->Set("DLSSNR.Scale", 1.0F);
    parameters->Set("DLSSNR.Upscaling", 0U);
    parameters->Set("DLSSNR.Enabled", 1U);
    set_model_tuning(parameters, settings);
    parameters->Set(
        "DLSSNRComputeScalingRatioCallback",
        reinterpret_cast<void*>(
            reinterpret_cast<std::uintptr_t>(&neural_scaling_ratio_callback)
        )
    );
    parameters->Set("PerfQualityValue", 0U);
    parameters->Set("DLSS.Feature.Create.Flags", frame.create_flags);
    parameters->Set("CreationNodeMask", 1U);
    parameters->Set("VisibilityNodeMask", 1U);

    if (!verify_model_tuning(parameters, settings, frame.view_id)) {
        static_cast<void>(runtime.destroy_parameters(parameters));
        view.feature_failed = true;
        diagnostics.state = DlssNrState::feature_failed;
        diagnostics.skip_reason = "NR parameter readback failed; model tuning was not applied";
        ++diagnostics.failed_calls;
        return false;
    }

    NgxHandle* handle{};
    constexpr std::uint32_t neural_feature_id = 18U;
    result = runtime.create_feature(
        frame.command_list,
        neural_feature_id,
        parameters,
        &handle
    );
    diagnostics.last_result = result;
    if (!ngx_succeeded(result) || handle == nullptr) {
        // Creation may have recorded GPU work even on failure. Retain these
        // objects with the recording, but never reuse a failed feature.
        view.retired_features.push_back({key, parameters, handle, false, view.uses});
        diagnostics.state = DlssNrState::feature_failed;
        ++diagnostics.failed_calls;
        view.feature_failed = true;
        trace_event(
            "DLSS-NR feature 18 creation failed view=%llu input=%ux%u "
            "output=%ux%u flags=0x%X result=0x%08X",
            static_cast<unsigned long long>(frame.view_id),
            working_width,
            working_height,
            working_width,
            working_height,
            frame.create_flags,
            result
        );
        return false;
    }
    view.parameters = parameters;
    view.handle = handle;
    view.key = key;
    view.has_key = true;
    view.feature_failed = false;
    view.was_enabled = false;
    view.settings_signature = 0U;
    trace_event(
        "DLSS-NR feature 18 created view=%llu input=%ux%u output=%ux%u "
        "scale=%.2f flags=0x%X preset=%u",
        static_cast<unsigned long long>(frame.view_id),
        working_width,
        working_height,
        working_width,
        working_height,
        1.0,
        frame.create_flags,
        preset
    );
    return true;
}

[[nodiscard]] bool initialize_gpu_resources(
    ID3D12GraphicsCommandList* const command_list,
    ID3D12Resource* const game_output,
    const NrRegion& region,
    const std::uint32_t working_width,
    const std::uint32_t working_height,
    GpuResources& gpu,
    const GpuResources* shared = nullptr
) noexcept {
    ID3D12Device* device{};
    ID3DBlob* serialized{};
    ID3DBlob* signature_errors{};
    const auto cleanup = [&]() noexcept {
        release(signature_errors);
        release(serialized);
        release(device);
    };
    auto fail = [&](const char* const stage, const HRESULT result) noexcept {
        cleanup();
        release_gpu(gpu);
        diagnostics.state = DlssNrState::unsupported_resources;
        diagnostics.last_result = static_cast<NgxResult>(result);
        ++diagnostics.failed_calls;
        trace_event("DLSS-NR GPU setup failed stage=%s hr=0x%08X", stage, result);
        return false;
    };

    auto result = command_list->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(result) || device == nullptr) return fail("GetDevice", result);
    const auto game_desc = game_output->GetDesc();
    if (!is_dlss_nr_output_compatible(game_desc)) {
        return fail("unsupported output", E_INVALIDARG);
    }

    gpu.game_output = game_output;
    gpu.game_output->AddRef();
    gpu.width = region.width;
    gpu.height = region.height;
    gpu.working_width = working_width;
    gpu.working_height = working_height;
    if (region.base_x + region.width > game_desc.Width ||
        region.base_y + region.height > game_desc.Height) {
        return fail("output region out of bounds", E_INVALIDARG);
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1U;
    heap.VisibleNodeMask = 1U;
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = gpu.width;
    texture.Height = gpu.height;
    texture.DepthOrArraySize = 1U;
    texture.MipLevels = 1U;
    texture.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texture.SampleDesc.Count = 1U;
    texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!gpu.border_only) {
        result = device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &texture,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&gpu.original_output)
        );
        if (FAILED(result)) return fail("CreateCommittedResource(original)", result);
        texture.Width = working_width;
        texture.Height = working_height;
        for (auto** destination : {&gpu.color_proxy, &gpu.neural_output}) {
            result = device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &texture,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr,
                IID_PPV_ARGS(destination)
            );
            if (FAILED(result)) return fail("CreateCommittedResource", result);
        }
        gpu.original_output->SetName(L"Cheeky DLSS-NR original HDR output");
        gpu.color_proxy->SetName(L"Cheeky DLSS-NR color proxy");
        gpu.neural_output->SetName(L"Cheeky DLSS-NR neural output");
    }

    D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap{};
    descriptor_heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptor_heap.NumDescriptors = 8U;
    descriptor_heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    result = device->CreateDescriptorHeap(
        &descriptor_heap,
        IID_PPV_ARGS(&gpu.descriptors)
    );
    if (FAILED(result)) return fail("CreateDescriptorHeap", result);
    gpu.descriptor_size = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
    );
    auto cpu = gpu.descriptors->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC game_srv{};
    game_srv.Format = game_desc.Format;
    game_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    game_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    game_srv.Texture2D.MipLevels = 1U;
    device->CreateShaderResourceView(game_output, &game_srv, cpu);
    cpu.ptr += gpu.descriptor_size;
    D3D12_SHADER_RESOURCE_VIEW_DESC fp16_srv{};
    fp16_srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    fp16_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    fp16_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    fp16_srv.Texture2D.MipLevels = 1U;
    for (auto* resource : {
            gpu.original_output,
            gpu.color_proxy,
            gpu.neural_output,
        }) {
        device->CreateShaderResourceView(resource, &fp16_srv, cpu);
        cpu.ptr += gpu.descriptor_size;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC fp16_uav{};
    fp16_uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    fp16_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(
        gpu.original_output,
        nullptr,
        &fp16_uav,
        cpu
    );
    cpu.ptr += gpu.descriptor_size;
    device->CreateUnorderedAccessView(gpu.color_proxy, nullptr, &fp16_uav, cpu);
    cpu.ptr += gpu.descriptor_size;
    D3D12_UNORDERED_ACCESS_VIEW_DESC game_uav{};
    game_uav.Format = game_desc.Format;
    game_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(game_output, nullptr, &game_uav, cpu);
    cpu.ptr += gpu.descriptor_size;
    device->CreateUnorderedAccessView(game_output, nullptr, &game_uav, cpu);

    if (shared && shared->root_signature && shared->border_pipeline &&
        (gpu.border_only || (shared->encode_pipeline && shared->decode_pipeline))) {
        gpu.root_signature = shared->root_signature; gpu.root_signature->AddRef();
        gpu.border_pipeline = shared->border_pipeline; gpu.border_pipeline->AddRef();
        if (!gpu.border_only) {
            gpu.encode_pipeline = shared->encode_pipeline; gpu.encode_pipeline->AddRef();
            gpu.decode_pipeline = shared->decode_pipeline; gpu.decode_pipeline->AddRef();
        }
        cleanup(); return true;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3U;
    ranges[0].BaseShaderRegister = 0U;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2U;
    ranges[1].BaseShaderRegister = 0U;
    D3D12_ROOT_PARAMETER root_parameters[3]{};
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1U;
    root_parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[1].DescriptorTable.NumDescriptorRanges = 1U;
    root_parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[2].Constants.ShaderRegister = 0U;
    root_parameters[2].Constants.Num32BitValues = 40U;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 3U;
    root_desc.pParameters = root_parameters;
    result = D3D12SerializeRootSignature(
        &root_desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &signature_errors
    );
    if (FAILED(result)) return fail("D3D12SerializeRootSignature", result);
    result = device->CreateRootSignature(
        0U,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&gpu.root_signature)
    );
    if (FAILED(result)) return fail("CreateRootSignature", result);



    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = gpu.root_signature;
    if (!gpu.border_only) {
        pipeline.CS = {d3d_shaders::nr_encode.data, d3d_shaders::nr_encode.size};
        result = device->CreateComputePipelineState(
            &pipeline,
            IID_PPV_ARGS(&gpu.encode_pipeline)
        );
        if (FAILED(result)) return fail("CreateComputePipelineState(encode)", result);
        pipeline.CS = {d3d_shaders::nr_decode.data, d3d_shaders::nr_decode.size};
        result = device->CreateComputePipelineState(
            &pipeline,
            IID_PPV_ARGS(&gpu.decode_pipeline)
        );
        if (FAILED(result)) return fail("CreateComputePipelineState(decode)", result);
    }
    pipeline.CS = {d3d_shaders::nr_border.data, d3d_shaders::nr_border.size};
    result = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&gpu.border_pipeline));
    if (FAILED(result)) return fail("CreateComputePipelineState(border)", result);
    cleanup();
    trace_event(
        "DLSS-NR region codec created region=%ux%u working=%ux%u source=%u,%u",
        gpu.width,
        gpu.height,
        gpu.working_width,
        gpu.working_height,
        region.base_x,
        region.base_y
    );
    return true;
}

[[nodiscard]] GpuResources* find_or_create_gpu(
    ViewState& view,
    const DlssNrFrame& frame,
    const NrRegion& region,
    const std::uint32_t working_width,
    const std::uint32_t working_height,
    const bool border_only = false
) noexcept {
    for (auto& gpu : view.gpu_resources) {
        if (gpu.border_only == border_only && gpu.game_output == frame.color &&
            (border_only || (gpu.width == region.width && gpu.height == region.height &&
            gpu.working_width == working_width && gpu.working_height == working_height &&
            gpu.guides.source_motion.Get() == frame.motion_vectors &&
            gpu.guides.source_depth.Get() == frame.depth))) {
            if (!gpu.uses.record(frame.command_list)) return nullptr;
            gpu.last_use = ++view.gpu_use_sequence;
            return &gpu;
        }
    }
    // Rebind rotating inputs only after every recording referencing the old
    // descriptors is complete. Keep the intermediate textures and pipelines.
    for (auto& gpu : view.gpu_resources) {
        if (gpu.border_only != border_only || (!border_only &&
            (gpu.width != region.width || gpu.height != region.height ||
             gpu.working_width != working_width || gpu.working_height != working_height))) continue;
        gpu.uses.collect();
        if (!gpu.uses.empty()) continue;
        Microsoft::WRL::ComPtr<ID3D12Device> device, incoming;
        if (FAILED(gpu.game_output->GetDevice(IID_PPV_ARGS(&device))) ||
            FAILED(frame.color->GetDevice(IID_PPV_ARGS(&incoming))) || device != incoming) continue;
        const auto desc = frame.color->GetDesc();
        if (!is_dlss_nr_output_compatible(desc)) continue;
        auto cpu = gpu.descriptors->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = desc.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(frame.color, &srv, cpu);
        cpu.ptr += 6U * gpu.descriptor_size;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = desc.Format; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(frame.color, nullptr, &uav, cpu);
        cpu.ptr += gpu.descriptor_size;
        device->CreateUnorderedAccessView(frame.color, nullptr, &uav, cpu);
        frame.color->AddRef(); release(gpu.game_output); gpu.game_output = frame.color;
        if (!border_only) gpu.guides.rebind(device.Get(), frame.motion_vectors, frame.depth);
        ++diagnostics.resource_rebinds;
        if (!gpu.uses.record(frame.command_list)) return nullptr;
        gpu.last_use = ++view.gpu_use_sequence;
        return &gpu;
    }
    if (std::count_if(view.gpu_resources.begin(), view.gpu_resources.end(),
            [&](const auto& gpu) { return gpu.border_only == border_only; }) >= gpu_resource_cache_capacity) {
        auto oldest = view.gpu_resources.end();
        for (auto it = view.gpu_resources.begin(); it != view.gpu_resources.end(); ++it) {
            if (it->border_only != border_only) continue;
            it->uses.collect();
            if (it->uses.empty() && (oldest == view.gpu_resources.end() ||
                    it->last_use < oldest->last_use)) oldest = it;
        }
        if (oldest == view.gpu_resources.end()) {
            diagnostics.state = DlssNrState::input_preparation_failed;
            diagnostics.skip_reason = "NR texture cache busy; using original color";
            ++diagnostics.failed_calls;
            return nullptr;
        }
        release_gpu(*oldest);
        view.gpu_resources.erase(oldest);
    }
    const GpuResources* shared{};
    Microsoft::WRL::ComPtr<ID3D12Device> incoming;
    if (FAILED(frame.color->GetDevice(IID_PPV_ARGS(&incoming)))) return nullptr;
    for (const auto& candidate : view.gpu_resources) {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if ((!border_only && candidate.border_only) ||
            FAILED(candidate.game_output->GetDevice(IID_PPV_ARGS(&device))) || device != incoming) continue;
        shared = &candidate; break;
    }
    view.gpu_resources.push_back(GpuResources{});
    auto& gpu = view.gpu_resources.back();
    gpu.border_only = border_only;
    if (!gpu.uses.record(frame.command_list) || !initialize_gpu_resources(
            frame.command_list,
            frame.color,
            region,
            working_width,
            working_height,
            gpu, shared
        ) || (!border_only && !gpu.guides.initialize(frame.motion_vectors, frame.depth, working_width, working_height, shared ? &shared->guides : nullptr))) {
        release_gpu(gpu);
        view.gpu_resources.pop_back();
        return nullptr;
    }
    gpu.last_use = ++view.gpu_use_sequence;
    if (border_only) ++diagnostics.border_creations; else ++diagnostics.codec_creations;
    return &gpu;
}


void transition(
    ID3D12GraphicsCommandList* const command_list,
    ID3D12Resource* const resource,
    const D3D12_RESOURCE_STATES before,
    const D3D12_RESOURCE_STATES after
) noexcept {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1U, &barrier);
}

void uav_barrier(
    ID3D12GraphicsCommandList* const command_list,
    ID3D12Resource* const resource
) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    command_list->ResourceBarrier(1U, &barrier);
}

void dispatch_codec(
    const DlssNrFrame& frame,
    GpuResources& gpu,
    ID3D12PipelineState* const pipeline,
    const std::uint32_t source_descriptor,
    const std::uint32_t destination_descriptor,
    const Settings& settings,
    const NrRegion& region
) noexcept {
    ID3D12DescriptorHeap* heaps[]{gpu.descriptors};
    frame.command_list->SetDescriptorHeaps(1U, heaps);
    frame.command_list->SetComputeRootSignature(gpu.root_signature);
    frame.command_list->SetPipelineState(pipeline);
    auto handle = gpu.descriptors->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<std::uint64_t>(source_descriptor) *
        gpu.descriptor_size;
    frame.command_list->SetComputeRootDescriptorTable(0U, handle);
    handle = gpu.descriptors->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<std::uint64_t>(destination_descriptor) *
        gpu.descriptor_size;
    frame.command_list->SetComputeRootDescriptorTable(1U, handle);
    const auto width = gpu.border_only ? region.width : gpu.width;
    const auto height = gpu.border_only ? region.height : gpu.height;
    CodecConstants constants{
        {width, height},
        {width, height},
        {region.base_x, region.base_y},
        {gpu.working_width, gpu.working_height},
        settings.nr_paper_white_scale,
        settings.nr_hdr_transfer_strength,
        settings.nr_color_strength,
        (frame.create_flags & 1U) != 0U ? 1U : 0U,
        {0U, 0U},
        {width, height},
        region.shape_width,
        region.shape_height,
        region.roundness,
        region.transition,
        settings.nr_alignment_border_enabled ? 1U : 0U,
        region.mask.count, {}, {},
    };
    std::memcpy(constants.mask_bounds, region.mask.bounds, sizeof(constants.mask_bounds));
    frame.command_list->SetComputeRoot32BitConstants(2U, 40U, &constants, 0U);
    const auto dispatch_width = (std::max)(width, gpu.working_width);
    const auto dispatch_height = (std::max)(height, gpu.working_height);
    frame.command_list->Dispatch(
        (dispatch_width + 15U) / 16U,
        (dispatch_height + 15U) / 16U,
        1U
    );
}

// Resolve NR independently of SR: changing NR dimensions must not resize the
// SR history, and NR must keep tracking when foveated SR is disabled.
bool resolve_afw_nr_coverage(DlssNrFrame& frame, Settings& settings) noexcept {
    if (!settings.eye_independent_coverage || !settings.nr_enabled || !settings.nr_foveated) return true;
    auto coverage = settings;
    coverage.enabled = true;
    coverage.width = settings.afw_nr.width; coverage.height = settings.afw_nr.height;
    coverage.x_offset = settings.afw_nr.x; coverage.height_offset = settings.afw_nr.y;
    coverage.afw_gaze_width = settings.afw_nr.gaze_width; coverage.afw_gaze_height = settings.afw_nr.gaze_height;
    coverage.afw_nr_coverage = true;
    coverage.afw_mask = settings.afw_nr_mask;
    CropGeometry crop{}; bool reset{};
    if (settings.nr_use_sr_foveation && frame.has_shared_sr_crop) {
        crop = frame.shared_sr_crop;
        if (!frame.input_width || !frame.input_height) return false;
        frame.center = foveation_center_from_geometry(crop, frame.input_width, frame.input_height);
        coverage.afw_mask = settings.afw_mask;
        apply_next_jump_preview(coverage, frame.view_id);
    } else if (!calculate_coordinated_crop(coverage, afw_nr_gaze_view(frame.view_id), frame.color,
            frame.input_width, frame.input_height, frame.output_width, frame.output_height,
            frame.view_output_base_x, frame.view_output_base_y, crop, reset, nullptr, &frame.center)) return false;
    else apply_next_jump_preview(coverage, afw_nr_gaze_view(frame.view_id));
    frame.has_center = true; frame.reset |= reset;
    settings.nr_width = static_cast<float>(crop.input_width) / frame.input_width;
    settings.nr_height = static_cast<float>(crop.input_height) / frame.input_height;
    settings.nr_roundness = settings.nr_use_sr_foveation ? settings.roundness : settings.nr_roundness;
    settings.afw_nr_mask = coverage.afw_mask;
    if (settings.nr_use_sr_foveation) settings.nr_transition_width = settings.transition_width;
    settings.nr_use_sr_foveation = false;
    return true;
}
}  // namespace

bool evaluate_dlss_nr(
    const DlssNrFrame& input_frame,
    const Settings& input_settings
) noexcept {
    auto frame = input_frame;
    auto settings = input_settings;
    if (!resolve_afw_nr_coverage(frame, settings)) { skip_dlss_nr_history(frame.view_id); return false; }
    AfwPrivateWorkScope private_work;
    std::lock_guard execution_lock(calibration12_execution_mutex());
    std::lock_guard lock(nr_mutex);
    collect_retired_views();
    diagnostics.skip_reason = nullptr;
    diagnostics.route = frame.route;
    diagnostics.hdr_input = (frame.create_flags & 1U) != 0U;
    diagnostics.processing_order = settings.nr_processing_order;
    diagnostics.processing_width = frame.processing_width ? frame.processing_width : frame.output_width;
    diagnostics.processing_height = frame.processing_height ? frame.processing_height : frame.output_height;
    if (!settings.nr_enabled) {
        diagnostics.state = DlssNrState::disabled;
        for (auto& view : views) view.was_enabled = false;
        return false;
    }
    diagnostics.output_width = frame.output_width;
    diagnostics.output_height = frame.output_height;
    diagnostics.region_base_x = diagnostics.region_base_y = 0U;
    diagnostics.region_width = diagnostics.region_height = 0U;
    diagnostics.working_width = diagnostics.working_height = 0U;
    diagnostics.last_result = 0U;
    ++diagnostics.candidate_calls;
    auto& view = find_or_create_view(frame.view_id);
    // Any rejected/failed NR evaluation breaks consecutive temporal history.
    struct HistoryGuard {
        ViewState& view;
        bool succeeded{};
        ~HistoryGuard() {
            if (succeeded) return;
            view.was_enabled = false;
            const auto count = diagnostics.failed_calls;
            if (count <= 8U || (count & (count - 1U)) == 0U)
                trace_event("DLSS-NR rejected view=%llu state=%s reason=%s result=0x%X failures=%llu",
                    view.view_id, dlss_nr_state_name(diagnostics.state),
                    diagnostics.skip_reason ? diagnostics.skip_reason : "none", diagnostics.last_result, count);
        }
    } history_guard{view};
    if (frame.command_list == nullptr || frame.view_id == 0U ||
        frame.color == nullptr || frame.depth == nullptr ||
        frame.motion_vectors == nullptr || frame.input_width == 0U ||
        frame.input_height == 0U || frame.output_width == 0U ||
        frame.output_height == 0U || frame.depth_width == 0U ||
        frame.depth_height == 0U || frame.motion_width == 0U ||
        frame.motion_height == 0U || !std::isfinite(frame.jitter_uv_x) ||
        !std::isfinite(frame.jitter_uv_y) || !std::isfinite(frame.motion_uv_scale_x) ||
        !std::isfinite(frame.motion_uv_scale_y) || frame.motion_vectors_3d ||
        frame.motion_state == static_cast<D3D12_RESOURCE_STATES>(0xFFFFFFFFU) ||
        frame.depth_state == static_cast<D3D12_RESOURCE_STATES>(0xFFFFFFFFU)) {
        diagnostics.state = DlssNrState::unsupported_resources;
        ++diagnostics.failed_calls;
        return false;
    }
    const auto guide_valid = [](ID3D12Resource* resource, std::uint32_t x, std::uint32_t y,
        std::uint32_t width, std::uint32_t height) noexcept {
        const auto desc = resource->GetDesc();
        return !(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
            desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 1U &&
            desc.SampleDesc.Count == 1U && x <= desc.Width && width <= desc.Width - x &&
            y <= desc.Height && height <= desc.Height - y;
    };
    if (!guide_valid(frame.depth, frame.depth_base_x, frame.depth_base_y, frame.depth_width, frame.depth_height) ||
        !guide_valid(frame.motion_vectors, frame.motion_base_x, frame.motion_base_y, frame.motion_width, frame.motion_height)) {
        diagnostics.state = DlssNrState::unsupported_resources;
        ++diagnostics.failed_calls;
        return false;
    }
    ID3D12Device* device{};
    const auto device_result = frame.command_list->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(device_result) || device == nullptr) {
        diagnostics.state = DlssNrState::unsupported_resources;
        diagnostics.last_result = static_cast<NgxResult>(device_result);
        ++diagnostics.failed_calls;
        return false;
    }
    if (!ensure_dlss_nr_recording(frame.command_list)) {
        device->Release();
        diagnostics.state = DlssNrState::input_preparation_failed;
        diagnostics.skip_reason = "NR requires compatible Execute/Reset observation and recording identity";
        ++diagnostics.failed_calls;
        return false;
    }
    const bool runtime_ready = initialize_runtime(device);
    device->Release();
    if (!runtime_ready) return false;

    const auto processing_width = frame.processing_width != 0U ? frame.processing_width : frame.output_width;
    const auto processing_height = frame.processing_height != 0U ? frame.processing_height : frame.output_height;
    const auto region = calculate_region(
        settings,
        processing_width,
        processing_height,
        frame.has_shared_sr_crop ? &frame.shared_sr_crop : nullptr,
        frame.input_width,
        frame.input_height,
        frame.has_center ? &frame.center : nullptr
    );
    const auto working_width = scaled_extent(region.width, settings.nr_working_scale);
    const auto working_height = scaled_extent(region.height, settings.nr_working_scale);
    NrRegion codec_region = region;
    const auto resource_base = dlss_nr_resource_base(
        region.base_x,
        region.base_y,
        frame.color_base_x,
        frame.color_base_y,
        frame.color_is_region
    );
    codec_region.base_x = resource_base.x;
    codec_region.base_y = resource_base.y;
    const auto color_desc = frame.color->GetDesc();
    if (codec_region.base_x + codec_region.width > color_desc.Width ||
        codec_region.base_y + codec_region.height > color_desc.Height) {
        diagnostics.state = DlssNrState::unsupported_resources;
        ++diagnostics.failed_calls;
        return false;
    }
    if (!record_use(view, frame.command_list)) return false;
    if (!create_feature(view, frame, settings, working_width, working_height)) {
        return false;
    }
    auto* const gpu = find_or_create_gpu(
        view,
        frame,
        codec_region,
        working_width,
        working_height
    );
    if (gpu == nullptr) return false;
    const auto depth_x = frame.color_is_region
        ? ScaledSubrect{frame.depth_base_x, frame.depth_width}
        : scale_subrect(
            region.base_x,
            region.width,
            frame.depth_base_x,
            frame.depth_width,
            processing_width
        );
    const auto depth_y = frame.color_is_region
        ? ScaledSubrect{frame.depth_base_y, frame.depth_height}
        : scale_subrect(
            region.base_y,
            region.height,
            frame.depth_base_y,
            frame.depth_height,
            processing_height
        );
    const auto motion_x = frame.color_is_region
        ? ScaledSubrect{frame.motion_base_x, frame.motion_width}
        : scale_subrect(
            region.base_x,
            region.width,
            frame.motion_base_x,
            frame.motion_width,
            processing_width
        );
    const auto motion_y = frame.color_is_region
        ? ScaledSubrect{frame.motion_base_y, frame.motion_height}
        : scale_subrect(
            region.base_y,
            region.height,
            frame.motion_base_y,
            frame.motion_height,
            processing_height
        );

    auto* const parameters = view.parameters;
    auto signature = nr_settings_signature(settings, region);
    // Guides can change resolution/origin while the working texture stays the
    // same size (e.g. output-resolution motion with dynamic display sizing).
    for (const auto dimension : {frame.input_width, frame.input_height,
            frame.output_width, frame.output_height, processing_width, processing_height,
            frame.depth_width, frame.depth_height, frame.motion_width, frame.motion_height,
            frame.color_base_x, frame.color_base_y, frame.depth_base_x, frame.depth_base_y,
            frame.motion_base_x, frame.motion_base_y}) {
        signature ^= dimension;
        signature *= 1099511628211ULL;
    }
    signature ^= frame.motion_vectors_jittered ? 1U : 0U;
    for (const auto dimension : {frame.motion_full_width, frame.motion_full_height,
            frame.depth_full_width, frame.depth_full_height, frame.motion_copy_x,
            frame.motion_copy_y, frame.depth_copy_x, frame.depth_copy_y}) {
        signature *= 1099511628211ULL;
        signature ^= dimension;
    }
    const auto reset_generation = requested_nr_reset_generation.load(
        std::memory_order_acquire
    );
    const char* reset_reason = frame.reset ? "explicit" :
        !view.was_enabled ? "no-history" :
        view.settings_signature != signature ? "settings" :
        view.reset_generation != reset_generation ? "requested" :
        view.history_route != frame.route ? "route" : nullptr;
    bool reset = reset_reason != nullptr;
    const auto motion_scale_x = dlss_nr_motion_axis(
        frame.motion_uv_scale_x * settings.nr_motion_scale_x_multiplier,
        processing_width, region.width, motion_x.extent);
    const auto motion_scale_y = dlss_nr_motion_axis(
        frame.motion_uv_scale_y * settings.nr_motion_scale_y_multiplier,
        processing_height, region.height, motion_y.extent);
    const DlssNrHistory history{
        region.base_x, region.base_y, region.width, region.height,
        frame.output_width, frame.output_height, working_width, working_height,
        motion_scale_x.processing_pixel_scale,
        motion_scale_y.processing_pixel_scale,
    };
    CropMotionOffset offset{};
    if (!reset) {
        // Validate overlap and geometry before using any previous-frame data.
        reset = !dlss_nr_motion_offset(view.history, history, offset.x, offset.y);
        if (reset) reset_reason = "incompatible-geometry-or-scale";
    }
    const bool before = settings.nr_processing_order == NrProcessingOrder::before_upscaling;
    const float jitter_x = dlss_nr_jitter_delta(view.jitter_uv_x, frame.jitter_uv_x,
        before, frame.motion_vectors_jittered, reset, settings.nr_motion_scale_x_multiplier);
    const float jitter_y = dlss_nr_jitter_delta(view.jitter_uv_y, frame.jitter_uv_y,
        before, frame.motion_vectors_jittered, reset, settings.nr_motion_scale_y_multiplier);
    offset.x = reset ? 0.0F : offset.x + jitter_x * processing_width / region.width;
    offset.y = reset ? 0.0F : offset.y + jitter_y * processing_height / region.height;
    const NrGuideConstants guide_constants{
        {working_width, working_height}, {region.base_x, region.base_y},
        {region.width, region.height}, {processing_width, processing_height},
        {frame.motion_full_width ? frame.motion_full_width : frame.motion_width,
         frame.motion_full_height ? frame.motion_full_height : frame.motion_height},
        {frame.depth_full_width ? frame.depth_full_width : frame.depth_width,
         frame.depth_full_height ? frame.depth_full_height : frame.depth_height},
        {static_cast<float>(frame.motion_base_x) - frame.motion_copy_x,
         static_cast<float>(frame.motion_base_y) - frame.motion_copy_y},
        {static_cast<float>(frame.depth_base_x) - frame.depth_copy_x,
         static_cast<float>(frame.depth_base_y) - frame.depth_copy_y},
        {motion_scale_x.processing_pixel_scale / region.width,
         motion_scale_y.processing_pixel_scale / region.height},
        {offset.x, offset.y}
    };
    gpu->guides.dispatch(frame.command_list, guide_constants, frame.motion_state, frame.depth_state);
    auto* motion_vectors = gpu->guides.motion.Get();
    const bool moved = view.was_enabled &&
        (view.history.x != history.x || view.history.y != history.y);
    ++view.history_calls;
    if (moved) ++view.history_moves;
    if (reset) ++view.history_resets;
    const bool corrected = offset.x != 0.0F || offset.y != 0.0F;
    if (corrected) ++view.history_corrections;
    const bool compensation_failed = reset_reason != nullptr &&
        std::strcmp(reset_reason, "compensation-unavailable") == 0;
    if (compensation_failed) ++view.history_compensation_failures;
    // Budget startup and moving-frame samples separately for each eye. A
    // desktop/menu view must not consume all diagnostics before VR starts.
    if (view.history_calls <= 8U || (moved && view.history_moves <= 16U) ||
        (compensation_failed && view.history_compensation_failures <= 8U) ||
        view.history_calls % 300U == 0U) {
        const auto motion_desc = frame.motion_vectors->GetDesc();
        trace_event("DLSS-NR history view=%llu region=%ux%u@%u,%u "
            "previous=%u,%u working=%ux%u offset=%.6f,%.6f corrected=%s reset=%s reason=%s "
            "calls=%llu moves=%llu resets=%llu corrections=%llu compensationFailures=%llu",
            static_cast<unsigned long long>(frame.view_id), region.width, region.height,
            region.base_x, region.base_y, view.history.x, view.history.y,
            working_width, working_height, offset.x, offset.y,
            corrected ? "yes" : "no", reset ? "yes" : "no",
            reset_reason != nullptr ? reset_reason : "none",
            static_cast<unsigned long long>(view.history_calls),
            static_cast<unsigned long long>(view.history_moves),
            static_cast<unsigned long long>(view.history_resets),
            static_cast<unsigned long long>(view.history_corrections),
            static_cast<unsigned long long>(view.history_compensation_failures));
        trace_event("DLSS-NR guides view=%llu size=%ux%u before=%s mvJittered=%s jitterUV=%.9f,%.9f jitterDeltaUV=%.9f,%.9f",
            static_cast<unsigned long long>(frame.view_id), working_width, working_height,
            before ? "yes" : "no", frame.motion_vectors_jittered ? "yes" : "no",
            frame.jitter_uv_x, frame.jitter_uv_y, jitter_x, jitter_y);
        trace_event("DLSS-NR inputs view=%llu flags=0x%X output=%ux%u@%u,%u "
            "mvTexture=%llux%u format=%u state=0x%X sourceMvRect=%ux%u@%u,%u "
            "mvUvScale=%.9f,%.9f nrScale=%.6f,%.6f depthRect=%ux%u@%u,%u "
            "resolvedCenter=%s center=%.6f,%.6f",
            static_cast<unsigned long long>(frame.view_id), frame.create_flags,
            frame.output_width, frame.output_height, frame.color_base_x, frame.color_base_y,
            static_cast<unsigned long long>(motion_desc.Width), motion_desc.Height,
            static_cast<unsigned>(motion_desc.Format), static_cast<unsigned>(frame.motion_state),
            motion_x.extent, motion_y.extent, motion_x.base, motion_y.base,
            frame.motion_uv_scale_x, frame.motion_uv_scale_y,
            static_cast<float>(working_width),
            static_cast<float>(working_height),
            depth_x.extent, depth_y.extent, depth_x.base, depth_y.base,
            frame.has_center ? "yes" : "no", frame.center.u, frame.center.v);
    }

    uav_barrier(frame.command_list, frame.color);
    transition(
        frame.command_list,
        frame.color,
        frame.color_state,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    );
    dispatch_codec(
        frame,
        *gpu,
        gpu->encode_pipeline,
        0U,
        4U,
        settings,
        codec_region
    );
    uav_barrier(frame.command_list, gpu->original_output);
    uav_barrier(frame.command_list, gpu->color_proxy);
    transition(
        frame.command_list,
        gpu->original_output,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    );
    transition(
        frame.command_list,
        gpu->color_proxy,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    );

    parameters->Set("DLSSNR.Color", gpu->color_proxy);
    parameters->Set("DLSSNR.Output", gpu->neural_output);
    parameters->Set("DLSSNR.MVec", motion_vectors);
    parameters->Set("DLSSNR.Depth", gpu->guides.depth.Get());
    parameters->Set("DLSSNR.ColorSubrectBaseX", 0U);
    parameters->Set("DLSSNR.ColorSubrectBaseY", 0U);
    parameters->Set("DLSSNR.ColorSubrectWidth", working_width);
    parameters->Set("DLSSNR.ColorSubrectHeight", working_height);
    parameters->Set("DLSSNR.OutputSubrectBaseX", 0U);
    parameters->Set("DLSSNR.OutputSubrectBaseY", 0U);
    parameters->Set("DLSSNR.OutputSubrectWidth", working_width);
    parameters->Set("DLSSNR.OutputSubrectHeight", working_height);
    parameters->Set("DLSSNR.DepthSubrectBaseX", 0U);
    parameters->Set("DLSSNR.DepthSubrectBaseY", 0U);
    parameters->Set("DLSSNR.DepthSubrectWidth", working_width);
    parameters->Set("DLSSNR.DepthSubrectHeight", working_height);
    parameters->Set("DLSSNR.MVecSubrectBaseX", 0U);
    parameters->Set("DLSSNR.MVecSubrectBaseY", 0U);
    parameters->Set("DLSSNR.MVecSubrectWidth", working_width);
    parameters->Set("DLSSNR.MVecSubrectHeight", working_height);
    parameters->Set(
        "DLSSNR.MVecScaleX",
        static_cast<float>(working_width)
    );
    parameters->Set(
        "DLSSNR.MVecScaleY",
        static_cast<float>(working_height)
    );
    const bool depth_inverted = settings.nr_depth_convention == 1U
        ? false
        : settings.nr_depth_convention == 2U ? true : frame.depth_inverted;
    parameters->Set("DLSSNR.DepthInverted", depth_inverted ? 1U : 0U);
    parameters->Set("DLSSNR.Enabled", 1U);
    parameters->Set("DLSSNR.Reset", reset ? 1U : 0U);
    parameters->Set("DLSSNR.ScalingRatio", 1.0F);
    parameters->Set("DLSSNR.Scale", 1.0F);
    parameters->Set("DLSSNR.Upscaling", 0U);
    set_model_tuning(parameters, settings);

    const auto result = runtime.evaluate_feature(
        frame.command_list,
        view.handle,
        parameters,
        nullptr
    );
    diagnostics.last_result = result;
    diagnostics.output_width = frame.output_width;
    diagnostics.output_height = frame.output_height;
    diagnostics.region_base_x = region.base_x;
    diagnostics.region_base_y = region.base_y;
    diagnostics.region_width = region.width;
    diagnostics.region_height = region.height;
    diagnostics.working_width = working_width;
    diagnostics.working_height = working_height;
    diagnostics.intermediate_vram_bytes =
        static_cast<std::uint64_t>(region.width) * region.height * 8U +
        static_cast<std::uint64_t>(working_width) * working_height * 28U;
    if (!ngx_succeeded(result)) {
        transition(
            frame.command_list,
            gpu->original_output,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS
        );
        transition(
            frame.command_list,
            gpu->color_proxy,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS
        );
        transition(
            frame.command_list,
            frame.color,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            frame.color_state
        );
        view.was_enabled = false;
        diagnostics.state = DlssNrState::evaluation_failed;
        ++diagnostics.failed_calls;
        trace_event(
            "DLSS-NR feature 18 evaluation failed view=%llu result=0x%08X",
            static_cast<unsigned long long>(frame.view_id),
            result
        );
        return false;
    }

    transition(
        frame.command_list,
        gpu->neural_output,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    );
    transition(
        frame.command_list,
        frame.color,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    );
    dispatch_codec(
        frame,
        *gpu,
        gpu->decode_pipeline,
        1U,
        6U,
        settings,
        codec_region
    );
    uav_barrier(frame.command_list, frame.color);
    transition(
        frame.command_list,
        frame.color,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        frame.color_state
    );
    transition(
        frame.command_list,
        gpu->original_output,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    );
    transition(
        frame.command_list,
        gpu->color_proxy,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    );
    transition(
        frame.command_list,
        gpu->neural_output,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    );
    ++diagnostics.evaluation_calls;
    view.history = history;
    view.last_region = region; view.last_order = settings.nr_processing_order;
    view.jitter_uv_x = frame.jitter_uv_x;
    view.jitter_uv_y = frame.jitter_uv_y;
    view.history_route = frame.route;
    view.settings_signature = signature;
    view.reset_generation = reset_generation;
    view.was_enabled = true;
    history_guard.succeeded = true;
    diagnostics.state = DlssNrState::active;
    if (diagnostics.evaluation_calls == 1U ||
        diagnostics.evaluation_calls % 300U == 0U) {
        trace_event(
            "DLSS-NR active route=%s view=%llu region=%ux%u@%u,%u "
            "working=%ux%u evaluations=%llu",
            dlss_nr_route_name(frame.route),
            static_cast<unsigned long long>(frame.view_id),
            region.width,
            region.height,
            region.base_x,
            region.base_y,
            working_width,
            working_height,
            static_cast<unsigned long long>(diagnostics.evaluation_calls)
        );
    }
    return true;
}

void draw_dlss_nr_border(const DlssNrFrame& input_frame, const Settings& input_settings) noexcept {
    auto frame = input_frame;
    auto settings = input_settings;
    if (!settings.nr_enabled || !settings.nr_alignment_border_enabled ||
        settings.nr_processing_order != NrProcessingOrder::before_upscaling ||
        !frame.color || !frame.command_list || !frame.output_width || !frame.output_height) return;
    std::lock_guard execution_lock(calibration12_execution_mutex());
    std::lock_guard lock(nr_mutex);
    collect_retired_views();
    if (!ensure_dlss_nr_recording(frame.command_list)) return;
    // Compute the rounded processing region first, then map it to display pixels.
    auto region = calculate_region(settings, frame.input_width, frame.input_height,
        frame.has_shared_sr_crop ? &frame.shared_sr_crop : nullptr, frame.input_width, frame.input_height,
        frame.has_center ? &frame.center : nullptr);
    if (settings.eye_independent_coverage) {
        // Before NR already sampled gaze. Draw the region actually processed,
        // even if inference took long enough for the gaze publication to age.
        const auto found = std::find_if(views.begin(), views.end(), [&](const auto& v) { return v.view_id == frame.view_id; });
        if (found == views.end() || !found->was_enabled || found->last_order != NrProcessingOrder::before_upscaling) return;
        region = found->last_region;
    }
    const auto x = scale_subrect(region.base_x, region.width, frame.color_base_x,
        frame.output_width, frame.input_width);
    const auto y = scale_subrect(region.base_y, region.height, frame.color_base_y,
        frame.output_height, frame.input_height);
    region.base_x = x.base; region.base_y = y.base;
    region.width = x.extent; region.height = y.extent;
    auto& view = find_or_create_view(frame.view_id);
    auto* gpu = find_or_create_gpu(view, frame, region, 8U, 8U, true);
    if (!gpu || !record_use(view, frame.command_list)) return;
    transition(frame.command_list, frame.color, frame.color_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch_codec(frame, *gpu, gpu->border_pipeline, 1U, 6U, settings, region);
    uav_barrier(frame.command_list, frame.color);
    transition(frame.command_list, frame.color, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, frame.color_state);
}

void note_dlss_nr_skipped(DlssNrRoute route, const Settings& settings, const char* reason) noexcept {
    std::lock_guard execution_lock(calibration12_execution_mutex());
    std::lock_guard lock(nr_mutex);
    diagnostics.route = route;
    diagnostics.hdr_input = false;
    diagnostics.processing_order = settings.nr_processing_order;
    diagnostics.skip_reason = reason;
    diagnostics.state = DlssNrState::input_preparation_failed;
    diagnostics.processing_width = diagnostics.processing_height = 0U;
    diagnostics.region_width = diagnostics.region_height = 0U;
    diagnostics.working_width = diagnostics.working_height = 0U;
    ++diagnostics.candidate_calls;
    ++diagnostics.failed_calls;
}
void collect_dlss_nr_submissions() noexcept {
    std::lock_guard execution_lock(calibration12_execution_mutex());
    std::lock_guard lock(nr_mutex);
    collect_retired_views();
}
void skip_dlss_nr_history(const DlssViewId view_id) noexcept {
    std::lock_guard lock(nr_mutex);
    for (auto& view : views) if (view.view_id == view_id) view.was_enabled = false;
}

void release_dlss_nr_view(const DlssViewId view_id) noexcept {
    std::lock_guard execution_lock(calibration12_execution_mutex());
    release_dlss_nr_inputs(view_id);
    std::lock_guard lock(nr_mutex);
    for (auto& view : views) if (view.view_id == view_id) view.retired = true;
    collect_retired_views();
}
void release_dlss_nr_resources() noexcept {
    std::lock_guard execution_lock(calibration12_execution_mutex());
    release_dlss_nr_inputs();
    std::lock_guard lock(nr_mutex);
    for (auto& view : views) view.retired = true;
    collect_retired_views();
    // Keep runtime callbacks alive while submitted or recorded work owns features.
    if (views.empty()) { release(runtime.device); runtime = {}; }
    diagnostics = {};
}

void reset_dlss_nr() noexcept {
    requested_nr_reset_generation.fetch_add(1U, std::memory_order_acq_rel);
    std::lock_guard execution_lock(calibration12_execution_mutex());
    std::lock_guard lock(nr_mutex);
    if (runtime.state == 2U) runtime.state = 0U;
    for (auto& view : views) {
        view.was_enabled = false;
        view.feature_failed = false;
    }
    diagnostics.state = DlssNrState::waiting;
    diagnostics.last_result = 0U;
}

DlssNrSnapshot dlss_nr_snapshot() noexcept {
    std::lock_guard execution_lock(calibration12_execution_mutex());
    std::lock_guard lock(nr_mutex);
    return diagnostics;
}

const char* dlss_nr_state_name(const DlssNrState state) noexcept {
    switch (state) {
    case DlssNrState::waiting: return "Waiting for a compatible DLSS frame";
    case DlssNrState::disabled: return "Disabled";
    case DlssNrState::runtime_missing: return "nvngx_dlssnr.dll could not load beside the processing DLL or game executable; see log";
    case DlssNrState::runtime_failed: return "DLSS-NR runtime initialization failed";
    case DlssNrState::unsupported_resources: return "Unsupported or missing depth, motion, or output resource";
    case DlssNrState::feature_failed: return "DLSS-NR feature 18 creation failed";
    case DlssNrState::evaluation_failed: return "DLSS-NR feature 18 evaluation failed";
    case DlssNrState::active: return "Active";
    case DlssNrState::input_preparation_failed: return "NR input preparation failed; using original color";
    }
    return "Unknown";
}

const char* dlss_nr_route_name(const DlssNrRoute route) noexcept {
    switch (route) {
    case DlssNrRoute::none: return "Waiting";
    case DlssNrRoute::d3d12_native: return "Direct3D 12";
    case DlssNrRoute::d3d11_transport: return "DX11 -> DX12 Transport";
    case DlssNrRoute::streamline: return "Streamline / Direct3D 12";
    }
    return "Unknown";
}

}  // namespace cheeky::foveated_dlss
