#include "dlss_nr_input.hpp"
#include "dlss_nr_lifetime.hpp"
#include "eye_calibration_d3d12.hpp"
#include "dlss_nr.hpp"

#include "d3d12_output_contract.hpp"
#include "dlss_nr_contract.hpp"
#include "crop_motion.hpp"
#include "runtime.hpp"
#include "runtime_search.hpp"

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
    HMODULE addon{};
    ID3D12Device* device{};
    NgxAllocateParametersFn allocate_parameters{};
    NgxDestroyParametersFn destroy_parameters{};
    NgxCreateFeatureFn create_feature{};
    NgxEvaluateFeatureFn evaluate_feature{};
    NgxReleaseFeatureFn release_feature{};
    GetModuleFileNameWFn get_module_file_name{};
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
};

[[nodiscard]] bool operator==(
    const FeatureKey& left,
    const FeatureKey& right
) noexcept {
    return std::memcmp(&left, &right, sizeof(left)) == 0;
}

struct GpuResources {
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
std::atomic<std::uint64_t> requested_reset_generation{};
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

[[nodiscard]] bool patch_slot(void** const slot, void* const replacement) noexcept {
    if (slot == nullptr || replacement == nullptr) return false;
    DWORD previous{};
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previous)) {
        return false;
    }
    InterlockedExchangePointer(
        reinterpret_cast<PVOID volatile*>(slot),
        replacement
    );
    DWORD ignored{};
    static_cast<void>(VirtualProtect(
        slot,
        sizeof(*slot),
        previous,
        &ignored
    ));
    return true;
}

[[nodiscard]] bool patch_named_import(
    const HMODULE module,
    const char* const requested_name,
    void* const replacement,
    void** const original_output
) noexcept {
    if (module == nullptr || requested_name == nullptr || replacement == nullptr) {
        return false;
    }
    auto* const image = reinterpret_cast<std::byte*>(module);
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const auto* const headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        image + dos->e_lfanew
    );
    if (headers->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto& imports = headers->OptionalHeader.DataDirectory[
        IMAGE_DIRECTORY_ENTRY_IMPORT
    ];
    if (imports.VirtualAddress == 0U) return false;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        image + imports.VirtualAddress
    );
    for (; descriptor->Name != 0U; ++descriptor) {
        if (descriptor->OriginalFirstThunk == 0U ||
            descriptor->FirstThunk == 0U) continue;
        auto* original = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            image + descriptor->OriginalFirstThunk
        );
        auto* resolved = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            image + descriptor->FirstThunk
        );
        for (; original->u1.AddressOfData != 0U; ++original, ++resolved) {
            if (IMAGE_SNAP_BY_ORDINAL64(original->u1.Ordinal)) continue;
            const auto* const import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                image + original->u1.AddressOfData
            );
            if (std::strcmp(
                    reinterpret_cast<const char*>(import->Name),
                    requested_name
                ) != 0) continue;
            auto* const slot = reinterpret_cast<void**>(&resolved->u1.Function);
            if (original_output != nullptr) *original_output = *slot;
            return patch_slot(slot, replacement);
        }
    }
    return false;
}

DWORD WINAPI hook_nr_get_module_file_name(
    const HMODULE module,
    LPWSTR const output,
    const DWORD capacity
) noexcept {
    // DLSS-NR 310.8 checks the identity of its external caller. Match the
    // working bridge implementation and present this add-on as nvngx.dll.
    if (module == runtime.addon && output != nullptr && capacity != 0U) {
        constexpr wchar_t identity[] = L"nvngx.dll";
        constexpr DWORD length = static_cast<DWORD>(std::size(identity) - 1U);
        if (capacity <= length) {
            output[0] = L'\0';
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return capacity;
        }
        std::memcpy(output, identity, sizeof(identity));
        return length;
    }
    return runtime.get_module_file_name == nullptr
        ? 0U
        : runtime.get_module_file_name(module, output, capacity);
}

[[nodiscard]] bool addon_directory(
    std::array<wchar_t, 32768U>& directory
) noexcept {
    HMODULE addon{};
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&evaluate_dlss_nr),
            &addon
        )) return false;
    runtime.addon = addon;
    const auto length = GetModuleFileNameW(
        addon,
        directory.data(),
        static_cast<DWORD>(directory.size())
    );
    if (length == 0U || length >= directory.size()) return false;
    for (DWORD index = length; index > 0U; --index) {
        if (directory[index - 1U] == L'\\' || directory[index - 1U] == L'/') {
            directory[index - 1U] = L'\0';
            return true;
        }
    }
    return false;
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

    std::array<wchar_t, 32768U> directory{};
    if (!addon_directory(directory)) {
        runtime.state = 3U;
        diagnostics.state = DlssNrState::runtime_failed;
        trace_event("DLSS-NR runtime initialization failed: add-on directory unavailable");
        return false;
    }
    std::array<wchar_t, 32768U> executable{};
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    auto* slash = length && length < executable.size() ? std::wcsrchr(executable.data(), L'\\') : nullptr;
    if (slash) *slash = L'\0'; else executable[0] = L'\0';
    const auto loaded = load_runtime_library(L"nvngx_dlssnr.dll", directory.data(), executable.data(),
        [](const wchar_t* path, DWORD error) {
            trace_event("DLSS-NR runtime search path=%ls error=%lu", path, static_cast<unsigned long>(error));
        });
    runtime.module = loaded.module;
    if (runtime.module == nullptr) {
        runtime.state = 2U;
        diagnostics.state = DlssNrState::runtime_missing;
        diagnostics.last_result = loaded.error;
        return false;
    }

    void* original_get_module_file_name{};
    if (!patch_named_import(
            runtime.module,
            "GetModuleFileNameW",
            reinterpret_cast<void*>(&hook_nr_get_module_file_name),
            &original_get_module_file_name
        )) {
        runtime.state = 3U;
        diagnostics.state = DlssNrState::runtime_failed;
        trace_event("DLSS-NR runtime identity import patch failed");
        return false;
    }
    runtime.get_module_file_name = reinterpret_cast<GetModuleFileNameWFn>(
        original_get_module_file_name
    );

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
        const auto core = GetModuleHandleW(L"_nvngx.dll");
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

NgxResult neural_scaling_ratio_callback(NgxParameters* const parameters) noexcept {
    if (parameters == nullptr) return 0xBAD00005U;
    float scale{1.0F};
    if (!ngx_succeeded(parameters->Get("DLSSNR.Scale", &scale))) scale = 1.0F;
    parameters->Set("DLSSNR.ScalingRatio", std::clamp(scale, 0.1F, 1.0F));
    return 1U;
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
    const auto preset = settings.nr_preset == 0U ? 1U : settings.nr_preset;
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
    parameters->Set("DLSSNR.Hint.Render.Preset", preset);
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
    GpuResources& gpu
) noexcept {
    ID3D12Device* device{};
    ID3DBlob* encoded{};
    ID3DBlob* decoded{};
    ID3DBlob* shader_errors{};
    ID3DBlob* serialized{};
    ID3DBlob* signature_errors{};
    const auto cleanup = [&]() noexcept {
        release(signature_errors);
        release(serialized);
        release(shader_errors);
        release(decoded);
        release(encoded);
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
    root_parameters[2].Constants.Num32BitValues = 21U;
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

    constexpr char shader[] = R"(
Texture2D<float4> Source0 : register(t0);
Texture2D<float4> Source1 : register(t1);
Texture2D<float4> Source2 : register(t2);
RWTexture2D<float4> Output0 : register(u0);
RWTexture2D<float4> Output1 : register(u1);

cbuffer CodecConstants : register(b0) {
    uint2 Size;
    uint2 SourceSize;
    uint2 SourceBase;
    uint2 ProxySize;
    float PaperWhiteScale;
    float TransferStrength;
    float ColorStrength;
    uint HdrMode;
    uint2 RegionBase;
    uint2 RegionSize;
    float FoveationWidth;
    float FoveationHeight;
    float FoveationRoundness;
    float FoveationFeather;
    uint ShowAlignmentBorder;
};

float FoveationShapeDistance(float2 pixel) {
    const float2 centered =
        (pixel - float2(RegionBase) + 0.5) /
        (0.5 * max(float2(RegionSize), 1.0)) - 1.0;
    const float2 scaled = abs(centered);
    return lerp(
        max(scaled.x, scaled.y),
        length(scaled),
        saturate(FoveationRoundness)
    );
}

float Luminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float SrgbEncodeChannel(float value) {
    value = saturate(value);
    return value <= 0.0031308
        ? 12.92 * value
        : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}

float3 SrgbEncode(float3 color) {
    return float3(
        SrgbEncodeChannel(color.r),
        SrgbEncodeChannel(color.g),
        SrgbEncodeChannel(color.b)
    );
}

float SrgbDecodeChannel(float value) {
    value = saturate(value);
    return value <= 0.04045
        ? value / 12.92
        : pow((value + 0.055) / 1.055, 2.4);
}

float3 SrgbDecode(float3 color) {
    return float3(
        SrgbDecodeChannel(color.r),
        SrgbDecodeChannel(color.g),
        SrgbDecodeChannel(color.b)
    );
}

float4 LoadSource0Bilinear(float2 position, uint2 origin, uint2 dimensions) {
    const float2 base = floor(position);
    const float2 fraction = position - base;
    const int2 minimum = int2(origin);
    const int2 maximum = minimum + int2(dimensions) - 1;
    const int2 p00 = clamp(int2(base), minimum, maximum);
    const int2 p10 = clamp(p00 + int2(1, 0), minimum, maximum);
    const int2 p01 = clamp(p00 + int2(0, 1), minimum, maximum);
    const int2 p11 = clamp(p00 + int2(1, 1), minimum, maximum);
    return lerp(
        lerp(Source0.Load(int3(p00, 0)), Source0.Load(int3(p10, 0)), fraction.x),
        lerp(Source0.Load(int3(p01, 0)), Source0.Load(int3(p11, 0)), fraction.x),
        fraction.y
    );
}

float4 LoadSource1Bilinear(float2 position, uint2 dimensions) {
    const float2 base = floor(position);
    const float2 fraction = position - base;
    const int2 maximum = int2(dimensions) - 1;
    const int2 p00 = clamp(int2(base), int2(0, 0), maximum);
    const int2 p10 = clamp(p00 + int2(1, 0), int2(0, 0), maximum);
    const int2 p01 = clamp(p00 + int2(0, 1), int2(0, 0), maximum);
    const int2 p11 = clamp(p00 + int2(1, 1), int2(0, 0), maximum);
    return lerp(
        lerp(Source1.Load(int3(p00, 0)), Source1.Load(int3(p10, 0)), fraction.x),
        lerp(Source1.Load(int3(p01, 0)), Source1.Load(int3(p11, 0)), fraction.x),
        fraction.y
    );
}

float4 LoadSource2Bilinear(float2 position, uint2 dimensions) {
    const float2 base = floor(position);
    const float2 fraction = position - base;
    const int2 maximum = int2(dimensions) - 1;
    const int2 p00 = clamp(int2(base), int2(0, 0), maximum);
    const int2 p10 = clamp(p00 + int2(1, 0), int2(0, 0), maximum);
    const int2 p01 = clamp(p00 + int2(0, 1), int2(0, 0), maximum);
    const int2 p11 = clamp(p00 + int2(1, 1), int2(0, 0), maximum);
    return lerp(
        lerp(Source2.Load(int3(p00, 0)), Source2.Load(int3(p10, 0)), fraction.x),
        lerp(Source2.Load(int3(p01, 0)), Source2.Load(int3(p11, 0)), fraction.x),
        fraction.y
    );
}

float3 UpgradeToneMap(float3 original, float3 proxy, float3 neural) {
    float original_y = Luminance(original);
    float proxy_y = Luminance(proxy);
    float neural_y = Luminance(neural);
    float ratio;
    if (original_y < proxy_y) {
        ratio = proxy_y > 0.0 ? original_y / proxy_y : 0.0;
    } else {
        float new_y = neural_y + max(0.0, original_y - proxy_y);
        ratio = neural_y > 0.0 ? new_y / neural_y : 0.0;
    }
    return lerp(original, max(neural * ratio, 0.0), TransferStrength);
}

[numthreads(16, 16, 1)]
void EncodeMain(uint3 dispatch_id : SV_DispatchThreadID) {
    if (all(dispatch_id.xy < Size)) {
        Output0[dispatch_id.xy] = Source0.Load(int3(SourceBase + dispatch_id.xy, 0));
    }
    if (all(dispatch_id.xy < ProxySize)) {
        const float2 source_position =
            float2(SourceBase) +
            (float2(dispatch_id.xy) + 0.5) * float2(SourceSize) /
            float2(ProxySize) - 0.5;
        const float4 proxy_source = LoadSource0Bilinear(
            source_position,
            SourceBase,
            SourceSize
        );
        const float3 linear_color = max(
            proxy_source.rgb / max(PaperWhiteScale, 0.0001),
            0.0
        );
        const float3 encoded = HdrMode != 0 ? SrgbEncode(linear_color) : linear_color;
        Output1[dispatch_id.xy] = float4(encoded, proxy_source.a);
    }
}

[numthreads(16, 16, 1)]
void BorderMain(uint3 dispatch_id : SV_DispatchThreadID) {
    if (any(dispatch_id.xy >= Size)) return;
    const float distance = FoveationShapeDistance(dispatch_id.xy);
    const float step = max(
        abs(FoveationShapeDistance(float2(dispatch_id.xy) + float2(1, 0)) - distance),
        abs(FoveationShapeDistance(float2(dispatch_id.xy) + float2(0, 1)) - distance));
    if (distance <= 1.0 && distance >= 1.0 - 5.0 * step)
        Output0[SourceBase + dispatch_id.xy] = float4(0, 1, 0, 1);
}

[numthreads(16, 16, 1)]
void DecodeMain(uint3 dispatch_id : SV_DispatchThreadID) {
    if (any(dispatch_id.xy >= Size)) return;
    const float4 original_sample = Source0.Load(int3(dispatch_id.xy, 0));
    const bool inside_region = all(dispatch_id.xy >= RegionBase) &&
        all(dispatch_id.xy < RegionBase + RegionSize);
    const float distance_from_center = FoveationShapeDistance(dispatch_id.xy);
    const float distance_per_pixel = max(
        abs(FoveationShapeDistance(float2(dispatch_id.xy) + float2(1.0, 0.0)) -
            distance_from_center),
        abs(FoveationShapeDistance(float2(dispatch_id.xy) + float2(0.0, 1.0)) -
            distance_from_center)
    );
    const bool alignment_border = ShowAlignmentBorder != 0U &&
        inside_region && distance_from_center <= 1.0 &&
        distance_from_center >= 1.0 - 5.0 * distance_per_pixel;
    if (alignment_border) {
        Output0[SourceBase + dispatch_id.xy] = float4(0.0, 1.0, 0.0, 1.0);
        return;
    }
    const float normalized_feather = FoveationFeather /
        max(0.0001, min(FoveationWidth, FoveationHeight));
    const float foveation_weight = FoveationFeather <= 0.0
        ? (distance_from_center <= 1.0 ? 1.0 : 0.0)
        : 1.0 - smoothstep(
            max(0.0, 1.0 - normalized_feather),
            1.0,
            distance_from_center
        );
    if (!inside_region || foveation_weight <= 0.0) {
        Output0[SourceBase + dispatch_id.xy] = original_sample;
        return;
    }
    const float2 proxy_position =
        (float2(dispatch_id.xy - RegionBase) + 0.5) * float2(ProxySize) /
        float2(RegionSize) - 0.5;
    const float4 proxy_sample = LoadSource1Bilinear(proxy_position, ProxySize);
    const float4 neural_sample = LoadSource2Bilinear(proxy_position, ProxySize);
    if (HdrMode == 0) {
        const float4 processed = lerp(original_sample, neural_sample, ColorStrength);
        Output0[SourceBase + dispatch_id.xy] = lerp(original_sample, processed, foveation_weight);
        return;
    }
    const float3 original = max(
        original_sample.rgb / max(PaperWhiteScale, 0.0001),
        0.0
    );
    const float3 proxy = SrgbDecode(proxy_sample.rgb);
    const float3 neural = SrgbDecode(neural_sample.rgb);
    const float3 upgraded = UpgradeToneMap(original, proxy, neural);
    const float3 decoded = lerp(original, upgraded, ColorStrength) * PaperWhiteScale;
    const float4 processed = float4(max(decoded, 0.0), original_sample.a);
    Output0[SourceBase + dispatch_id.xy] = lerp(original_sample, processed, foveation_weight);
}
)";

    result = D3DCompile(
        shader,
        sizeof(shader) - 1U,
        "Cheeky DLSS-NR codec",
        nullptr,
        nullptr,
        "EncodeMain",
        "cs_5_1",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0U,
        &encoded,
        &shader_errors
    );
    if (FAILED(result)) return fail("D3DCompile(encode)", result);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = gpu.root_signature;
    pipeline.CS = {encoded->GetBufferPointer(), encoded->GetBufferSize()};
    result = device->CreateComputePipelineState(
        &pipeline,
        IID_PPV_ARGS(&gpu.encode_pipeline)
    );
    if (FAILED(result)) return fail("CreateComputePipelineState(encode)", result);
    release(shader_errors);
    result = D3DCompile(
        shader,
        sizeof(shader) - 1U,
        "Cheeky DLSS-NR codec",
        nullptr,
        nullptr,
        "DecodeMain",
        "cs_5_1",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0U,
        &decoded,
        &shader_errors
    );
    if (FAILED(result)) return fail("D3DCompile(decode)", result);
    pipeline.CS = {decoded->GetBufferPointer(), decoded->GetBufferSize()};
    result = device->CreateComputePipelineState(
        &pipeline,
        IID_PPV_ARGS(&gpu.decode_pipeline)
    );
    if (FAILED(result)) return fail("CreateComputePipelineState(decode)", result);
    release(decoded);
    release(shader_errors);
    result = D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr,
        "BorderMain", "cs_5_0", 0U, 0U, &decoded, &shader_errors);
    if (FAILED(result)) return fail("D3DCompile(border)", result);
    pipeline.CS = {decoded->GetBufferPointer(), decoded->GetBufferSize()};
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
    const std::uint32_t working_height
) noexcept {
    for (auto& gpu : view.gpu_resources) {
        if (gpu.game_output == frame.color && gpu.width == region.width &&
            gpu.height == region.height && gpu.working_width == working_width &&
            gpu.working_height == working_height) {
            if (!gpu.uses.record(frame.command_list)) return nullptr;
            gpu.last_use = ++view.gpu_use_sequence;
            return &gpu;
        }
    }
    if (view.gpu_resources.size() >= gpu_resource_cache_capacity) {
        auto oldest = view.gpu_resources.end();
        for (auto it = view.gpu_resources.begin(); it != view.gpu_resources.end(); ++it) {
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
    view.gpu_resources.push_back(GpuResources{});
    auto& gpu = view.gpu_resources.back();
    if (!gpu.uses.record(frame.command_list) || !initialize_gpu_resources(
            frame.command_list,
            frame.color,
            region,
            working_width,
            working_height,
            gpu
        )) {
        view.gpu_resources.pop_back();
        return nullptr;
    }
    gpu.last_use = ++view.gpu_use_sequence;
    return &gpu;
}

[[nodiscard]] std::uint64_t settings_signature(
    const Settings& settings,
    const NrRegion& region
) noexcept {
    std::uint64_t signature = 1469598103934665603ULL;
    const auto append = [&signature](const std::uint32_t value) noexcept {
        signature ^= value;
        signature *= 1099511628211ULL;
    };
    append(static_cast<std::uint32_t>(settings.nr_processing_order));
    for (const auto value : {
            settings.nr_working_scale,
            settings.nr_intensity,
            settings.nr_local_tone_strength,
            settings.nr_local_structure_strength,
            settings.nr_skin_structure_strength,
            settings.nr_paper_white_scale,
            settings.nr_hdr_transfer_strength,
            settings.nr_color_strength,
            settings.nr_motion_scale_x_multiplier,
            settings.nr_motion_scale_y_multiplier,
            region.shape_width,
            region.shape_height,
            region.roundness,
            region.transition,
        }) {
        std::uint32_t bits{};
        std::memcpy(&bits, &value, sizeof(bits));
        append(bits);
    }
    append(settings.nr_foveated ? 1U : 0U);
    append(settings.nr_automatic_mask ? 1U : 0U);
    append(settings.nr_ui_correction ? 1U : 0U);
    append(settings.nr_depth_convention);
    append(region.width);
    append(region.height);
    return signature;
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
    struct CodecConstants {
        std::uint32_t size[2];
        std::uint32_t source_size[2];
        std::uint32_t source_base[2];
        std::uint32_t proxy_size[2];
        float paper_white_scale;
        float transfer_strength;
        float color_strength;
        std::uint32_t hdr_mode;
        std::uint32_t region_base[2];
        std::uint32_t region_size[2];
        float foveation_width;
        float foveation_height;
        float foveation_roundness;
        float foveation_feather;
        std::uint32_t show_alignment_border;
    };
    static_assert(sizeof(CodecConstants) == 21U * sizeof(std::uint32_t));
    const CodecConstants constants{
        {gpu.width, gpu.height},
        {gpu.width, gpu.height},
        {region.base_x, region.base_y},
        {gpu.working_width, gpu.working_height},
        settings.nr_paper_white_scale,
        settings.nr_hdr_transfer_strength,
        settings.nr_color_strength,
        1U,
        {0U, 0U},
        {gpu.width, gpu.height},
        region.shape_width,
        region.shape_height,
        region.roundness,
        region.transition,
        settings.nr_alignment_border_enabled ? 1U : 0U,
    };
    frame.command_list->SetComputeRoot32BitConstants(2U, 21U, &constants, 0U);
    const auto dispatch_width = (std::max)(gpu.width, gpu.working_width);
    const auto dispatch_height = (std::max)(gpu.height, gpu.working_height);
    frame.command_list->Dispatch(
        (dispatch_width + 15U) / 16U,
        (dispatch_height + 15U) / 16U,
        1U
    );
}

}  // namespace

bool evaluate_dlss_nr(
    const DlssNrFrame& frame,
    const Settings& settings
) noexcept {
    std::lock_guard execution_lock(calibration12_execution_mutex());
    std::lock_guard lock(nr_mutex);
    collect_retired_views();
    diagnostics.skip_reason = nullptr;
    diagnostics.route = frame.route;
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
        ~HistoryGuard() { if (!succeeded) view.was_enabled = false; }
    } history_guard{view};
    if (frame.command_list == nullptr || frame.view_id == 0U ||
        frame.color == nullptr || frame.depth == nullptr ||
        frame.motion_vectors == nullptr || frame.input_width == 0U ||
        frame.input_height == 0U || frame.output_width == 0U ||
        frame.output_height == 0U || frame.depth_width == 0U ||
        frame.depth_height == 0U || frame.motion_width == 0U ||
        frame.motion_height == 0U) {
        diagnostics.state = DlssNrState::unsupported_resources;
        ++diagnostics.failed_calls;
        return false;
    }
    const auto guide_valid = [](ID3D12Resource* resource, std::uint32_t x, std::uint32_t y,
        std::uint32_t width, std::uint32_t height) noexcept {
        const auto desc = resource->GetDesc();
        return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 1U &&
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
    auto signature = settings_signature(settings, region);
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
    const auto reset_generation = requested_reset_generation.load(
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
    auto* motion_vectors = frame.motion_vectors;
    auto motion_base_x = motion_x.base;
    auto motion_base_y = motion_y.base;
    CropMotionOffset offset{};
    if (!reset) {
        reset = !dlss_nr_motion_offset(view.history, history, offset.x, offset.y);
        if (reset) reset_reason = "incompatible-geometry-or-scale";
        if (!reset && (offset.x != 0.0F || offset.y != 0.0F)) {
            // Crop origins and history scales use processing pixels. The
            // correction remains in stored-vector units at every working size.
            auto* corrected = !frame.motion_vectors_3d &&
                frame.motion_state != static_cast<D3D12_RESOURCE_STATES>(0xFFFFFFFFU)
                ? prepare_crop_motion12(frame.command_list, frame.motion_vectors,
                    motion_x.base, motion_y.base, motion_x.extent, motion_y.extent,
                    offset, frame.motion_state) : nullptr;
            if (corrected) {
                motion_vectors = corrected;
                motion_base_x = motion_base_y = 0U;
            } else {
                reset = true;
                reset_reason = "compensation-unavailable";
            }
        }
    }
    const bool moved = view.was_enabled &&
        (view.history.x != history.x || view.history.y != history.y);
    ++view.history_calls;
    if (moved) ++view.history_moves;
    if (reset) ++view.history_resets;
    if (motion_vectors != frame.motion_vectors) ++view.history_corrections;
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
            motion_vectors != frame.motion_vectors ? "yes" : "no", reset ? "yes" : "no",
            reset_reason != nullptr ? reset_reason : "none",
            static_cast<unsigned long long>(view.history_calls),
            static_cast<unsigned long long>(view.history_moves),
            static_cast<unsigned long long>(view.history_resets),
            static_cast<unsigned long long>(view.history_corrections),
            static_cast<unsigned long long>(view.history_compensation_failures));
        trace_event("DLSS-NR inputs view=%llu flags=0x%X output=%ux%u@%u,%u "
            "mvTexture=%llux%u format=%u state=0x%X mvRect=%ux%u@%u,%u "
            "mvUvScale=%.9f,%.9f nrScale=%.6f,%.6f depthRect=%ux%u@%u,%u "
            "resolvedCenter=%s center=%.6f,%.6f",
            static_cast<unsigned long long>(frame.view_id), frame.create_flags,
            frame.output_width, frame.output_height, frame.color_base_x, frame.color_base_y,
            static_cast<unsigned long long>(motion_desc.Width), motion_desc.Height,
            static_cast<unsigned>(motion_desc.Format), static_cast<unsigned>(frame.motion_state),
            motion_x.extent, motion_y.extent, motion_x.base, motion_y.base,
            frame.motion_uv_scale_x, frame.motion_uv_scale_y,
            motion_scale_x.runtime_scale,
            motion_scale_y.runtime_scale,
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
    parameters->Set("DLSSNR.Depth", frame.depth);
    parameters->Set("DLSSNR.ColorSubrectBaseX", 0U);
    parameters->Set("DLSSNR.ColorSubrectBaseY", 0U);
    parameters->Set("DLSSNR.ColorSubrectWidth", working_width);
    parameters->Set("DLSSNR.ColorSubrectHeight", working_height);
    parameters->Set("DLSSNR.OutputSubrectBaseX", 0U);
    parameters->Set("DLSSNR.OutputSubrectBaseY", 0U);
    parameters->Set("DLSSNR.OutputSubrectWidth", working_width);
    parameters->Set("DLSSNR.OutputSubrectHeight", working_height);
    parameters->Set("DLSSNR.DepthSubrectBaseX", depth_x.base);
    parameters->Set("DLSSNR.DepthSubrectBaseY", depth_y.base);
    parameters->Set("DLSSNR.DepthSubrectWidth", depth_x.extent);
    parameters->Set("DLSSNR.DepthSubrectHeight", depth_y.extent);
    parameters->Set("DLSSNR.MVecSubrectBaseX", motion_base_x);
    parameters->Set("DLSSNR.MVecSubrectBaseY", motion_base_y);
    parameters->Set("DLSSNR.MVecSubrectWidth", motion_x.extent);
    parameters->Set("DLSSNR.MVecSubrectHeight", motion_y.extent);
    parameters->Set(
        "DLSSNR.MVecScaleX",
        motion_scale_x.runtime_scale
    );
    parameters->Set(
        "DLSSNR.MVecScaleY",
        motion_scale_y.runtime_scale
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
    parameters->Set("DLSSNR.Intensity", settings.nr_intensity);
    parameters->Set("DLSSNR.LocalToneStrength", settings.nr_local_tone_strength);
    parameters->Set(
        "DLSSNR.LocalStructureStrength",
        settings.nr_local_structure_strength
    );
    parameters->Set(
        "DLSSNR.SkinStructureStrength",
        settings.nr_skin_structure_strength
    );
    parameters->Set("DLSSNR.UseAutoMask", settings.nr_automatic_mask ? 1U : 0U);
    parameters->Set("DLSSNR.Style", 0U);
    parameters->Set("DLSSNR.UICorrection", settings.nr_ui_correction ? 1U : 0U);

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
        static_cast<std::uint64_t>(working_width) * working_height * 16U;
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

void draw_dlss_nr_border(const DlssNrFrame& frame, const Settings& settings) noexcept {
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
    const auto x = scale_subrect(region.base_x, region.width, frame.color_base_x,
        frame.output_width, frame.input_width);
    const auto y = scale_subrect(region.base_y, region.height, frame.color_base_y,
        frame.output_height, frame.input_height);
    region.base_x = x.base; region.base_y = y.base;
    region.width = x.extent; region.height = y.extent;
    auto& view = find_or_create_view(frame.view_id);
    auto* gpu = find_or_create_gpu(view, frame, region, 8U, 8U);
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
    requested_reset_generation.fetch_add(1U, std::memory_order_acq_rel);
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
