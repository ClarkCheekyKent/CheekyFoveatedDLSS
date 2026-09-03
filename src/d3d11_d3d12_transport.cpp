#include "d3d11_d3d12_transport.hpp"

#include "diagnostics.hpp"
#include "runtime.hpp"

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace cheeky::foveated_dlss {
namespace {

constexpr std::uint32_t transport_slot_count = 3U;
constexpr std::uint32_t dlss_feature_flag_mv_low_res = 1U << 1U;
constexpr std::uint32_t dlss_feature_flag_depth_inverted = 1U << 3U;
std::atomic<std::uint64_t> shared_texture_failure_sequence{};
std::atomic<std::uint64_t> ngx_callback_failure_sequence{};

[[nodiscard]] bool should_trace_shared_texture_failure() noexcept {
    const auto sequence = shared_texture_failure_sequence.fetch_add(
        1U, std::memory_order_relaxed
    );
    return sequence < 16U || sequence % 300U == 0U;
}

template <typename T>
void release(T*& value) noexcept {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
}

[[nodiscard]] ID3D11Resource* get_resource(
    const NgxParameters* const parameters,
    const char* const name
) noexcept {
    ID3D11Resource* resource{};
    return parameters != nullptr && ngx_succeeded(parameters->Get(name, &resource))
        ? resource
        : nullptr;
}

[[nodiscard]] float get_float(
    const NgxParameters* const parameters,
    const char* const name,
    const float fallback = 0.0F
) noexcept {
    float value{};
    return parameters != nullptr && ngx_succeeded(parameters->Get(name, &value))
        ? value
        : fallback;
}

[[nodiscard]] int get_int(
    const NgxParameters* const parameters,
    const char* const name
) noexcept {
    int value{};
    return parameters != nullptr && ngx_succeeded(parameters->Get(name, &value))
        ? value
        : 0;
}

[[nodiscard]] std::uint32_t get_integer_bits(
    const NgxParameters* const parameters,
    const char* const name
) noexcept {
    int value{};
    if (parameters != nullptr && ngx_succeeded(parameters->Get(name, &value))) {
        return static_cast<std::uint32_t>(value);
    }
    return get_ui(parameters, name);
}

[[nodiscard]] bool in_bounds(
    const std::uint32_t base,
    const std::uint32_t size,
    const std::uint32_t capacity
) noexcept {
    return base <= capacity && size <= capacity - base;
}

struct ScaledRange {
    std::uint32_t base{};
    std::uint32_t extent{};
};

[[nodiscard]] ScaledRange scale_range(
    const std::uint32_t base,
    const std::uint32_t extent,
    const std::uint32_t source_extent,
    const std::uint32_t output_extent
) noexcept {
    const auto scaled_base = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(base) * source_extent / output_extent
    );
    const auto scaled_end = static_cast<std::uint32_t>((std::min)(
        static_cast<std::uint64_t>(source_extent),
        (static_cast<std::uint64_t>(base + extent) * source_extent +
            output_extent - 1U) / output_extent
    ));
    return {scaled_base, (std::max)(1U, scaled_end - scaled_base)};
}

struct InitContract {
    unsigned long long application_id{};
    std::wstring application_data_path;
    const void* feature_common_info{};
    std::uint32_t sdk_version{};
    bool valid{};
};

std::mutex transport_mutex;
InitContract init_contract;

struct SharedTexture {
    ID3D12Resource* resource12{};
    ID3D11Texture2D* texture11{};
};

struct TransportSlot {
    SharedTexture color;
    SharedTexture depth;
    SharedTexture motion_vectors;
    SharedTexture output;
    SharedTexture nr_color;
    SharedTexture nr_depth;
    SharedTexture nr_motion_vectors;
    ID3D12CommandAllocator* allocator{};
    ID3D12CommandAllocator* nr_allocator{};
    ID3D11Query* timing_disjoint{};
    ID3D11Query* timing_begin{};
    ID3D11Query* timing_end{};
    bool timing_pending{};
    ID3D12QueryHeap* dlss_timing_heap{};
    ID3D12Resource* dlss_timing_readback{};
    bool dlss_timing_pending{};
    std::uint32_t dlss_timing_query_count{};
    bool dlss_nr_timing_foveated{};
    std::uint64_t done_value{};
    std::uint32_t input_width{};
    std::uint32_t input_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t nr_input_width{};
    std::uint32_t nr_input_height{};
    std::uint32_t nr_output_width{};
    std::uint32_t nr_output_height{};
    std::uint32_t nr_motion_width{};
    std::uint32_t nr_motion_height{};
    DXGI_FORMAT color_format{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT motion_format{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT output_format{DXGI_FORMAT_UNKNOWN};
};

struct TransportView {
    DlssViewId view_id{};
    std::array<TransportSlot, transport_slot_count> slots{};
    std::uint32_t next_slot{};
};

struct TransportDevice {
    ID3D11Device* device11{};
    ID3D11Device1* device11_1{};
    ID3D11Device5* device11_5{};
    ID3D12Device* device12{};
    ID3D12CommandQueue* queue12{};
    ID3D12GraphicsCommandList* command_list12{};
    ID3D11Fence* fence11{};
    ID3D12Fence* fence12{};
    ID3D11ComputeShader* depth_shader{};
    ID3D11Buffer* depth_constants{};
    NgxParameters* ngx_parameters{};
    std::deque<TransportView> views;
    std::uint64_t next_fence_value{1U};
    std::uint64_t timestamp_frequency{};
    NgxD3D12Shutdown1Fn shutdown{};
    bool ngx_initialized{};
    bool format_support_logged{};
};

std::deque<TransportDevice> transport_devices;

constexpr char depth_shader_source[] = R"(
Texture2D<float> SourceDepth : register(t0);
RWTexture2D<float> PackedDepth : register(u0);
cbuffer Constants : register(b0) { uint2 SourceBase; uint2 PackedSize; };
[numthreads(16, 16, 1)]
void Main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= PackedSize)) return;
    PackedDepth[id.xy] = SourceDepth.Load(int3(SourceBase + id.xy, 0));
}
)";

void release_shared_texture(SharedTexture& texture) noexcept {
    release(texture.texture11);
    release(texture.resource12);
}

void release_slot(TransportSlot& slot) noexcept {
    release(slot.dlss_timing_readback);
    release(slot.dlss_timing_heap);
    release(slot.timing_end);
    release(slot.timing_begin);
    release(slot.timing_disjoint);
    release_shared_texture(slot.output);
    release_shared_texture(slot.motion_vectors);
    release_shared_texture(slot.depth);
    release_shared_texture(slot.color);
    release_shared_texture(slot.nr_motion_vectors);
    release_shared_texture(slot.nr_depth);
    release_shared_texture(slot.nr_color);
    release(slot.nr_allocator);
    release(slot.allocator);
    slot = {};
}

void release_device(TransportDevice& device) noexcept {
    for (auto& view : device.views) {
        release_d3d12_view(view.view_id);
    }
    if (device.ngx_parameters != nullptr) {
        device.ngx_parameters->Set(
            "Color", static_cast<ID3D12Resource*>(nullptr)
        );
        device.ngx_parameters->Set(
            "Depth", static_cast<ID3D12Resource*>(nullptr)
        );
        device.ngx_parameters->Set(
            "MotionVectors", static_cast<ID3D12Resource*>(nullptr)
        );
        device.ngx_parameters->Set(
            "ExposureTexture", static_cast<ID3D12Resource*>(nullptr)
        );
        device.ngx_parameters->Set(
            "Output", static_cast<ID3D12Resource*>(nullptr)
        );
    }
    for (auto& view : device.views) {
        for (auto& slot : view.slots) release_slot(slot);
    }
    device.views.clear();
    if (device.ngx_initialized && device.shutdown != nullptr &&
        device.device12 != nullptr) {
        static_cast<void>(device.shutdown(device.device12));
    }
    release(device.depth_constants);
    release(device.depth_shader);
    release(device.fence12);
    release(device.fence11);
    release(device.command_list12);
    release(device.queue12);
    release(device.device12);
    release(device.device11_5);
    release(device.device11_1);
    release(device.device11);
    device.ngx_parameters = nullptr;
}

[[nodiscard]] int capture_transport_exception(
    const EXCEPTION_POINTERS* const exception,
    DWORD& exception_code
) noexcept {
    exception_code = exception != nullptr &&
        exception->ExceptionRecord != nullptr
        ? exception->ExceptionRecord->ExceptionCode
        : EXCEPTION_NONCONTINUABLE_EXCEPTION;
    return EXCEPTION_EXECUTE_HANDLER;
}

[[nodiscard]] NgxResult call_init_ext_guarded(
    const NgxD3D12InitExtFn init,
    const unsigned long long application_id,
    const wchar_t* const application_data_path,
    ID3D12Device* const device,
    const int sdk_version,
    const void* const feature_common_info,
    DWORD& exception_code
) noexcept {
    exception_code = 0U;
    __try {
        return init(
            application_id,
            application_data_path,
            device,
            sdk_version,
            feature_common_info
        );
    } __except (capture_transport_exception(
            GetExceptionInformation(), exception_code)) {
        return 0xBAD0FFFFU;
    }
}

[[nodiscard]] NgxResult allocate_parameters_guarded(
    const NgxD3D12AllocateParametersFn allocate,
    NgxParameters** const parameters,
    DWORD& exception_code
) noexcept {
    exception_code = 0U;
    __try {
        return allocate(parameters);
    } __except (capture_transport_exception(
            GetExceptionInformation(), exception_code)) {
        return 0xBAD0FFFFU;
    }
}

[[nodiscard]] bool create_shared_texture(
    TransportDevice& device,
    const char* const label,
    const std::uint32_t width,
    const std::uint32_t height,
    const DXGI_FORMAT format,
    const D3D12_RESOURCE_FLAGS flags,
    SharedTexture& texture
) noexcept {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1U;
    heap.VisibleNodeMask = 1U;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1U;
    desc.MipLevels = 1U;
    desc.Format = format;
    desc.SampleDesc.Count = 1U;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    auto result = device.device12->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_SHARED, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&texture.resource12)
    );
    HANDLE handle{};
    if (SUCCEEDED(result)) {
        result = device.device12->CreateSharedHandle(
            texture.resource12, nullptr, GENERIC_ALL, nullptr, &handle
        );
    }
    if (SUCCEEDED(result)) {
        result = device.device11_1->OpenSharedResource1(
            handle, IID_PPV_ARGS(&texture.texture11)
        );
    }
    if (handle != nullptr) CloseHandle(handle);
    if (SUCCEEDED(result)) {
        trace_event(
            "Transport texture %s shared via D3D12->D3D11 "
            "size=%ux%u format=%u flags12=0x%X",
            label, width, height, static_cast<unsigned int>(format),
            static_cast<unsigned int>(desc.Flags)
        );
        return true;
    }

    if (should_trace_shared_texture_failure()) trace_event(
        "Transport texture %s D3D12->D3D11 failed hr=0x%08X; "
        "trying D3D11->D3D12",
        label, static_cast<unsigned int>(result)
    );
    release_shared_texture(texture);

    D3D11_TEXTURE2D_DESC desc11{};
    desc11.Width = width;
    desc11.Height = height;
    desc11.MipLevels = 1U;
    desc11.ArraySize = 1U;
    desc11.Format = format;
    desc11.SampleDesc.Count = 1U;
    desc11.Usage = D3D11_USAGE_DEFAULT;
    desc11.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if ((flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0U) {
        desc11.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    }
    desc11.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
        D3D11_RESOURCE_MISC_SHARED;
    result = device.device11->CreateTexture2D(
        &desc11, nullptr, &texture.texture11
    );
    IDXGIResource1* shared_resource{};
    if (SUCCEEDED(result)) {
        result = texture.texture11->QueryInterface(
            IID_PPV_ARGS(&shared_resource)
        );
    }
    handle = nullptr;
    if (SUCCEEDED(result)) {
        result = shared_resource->CreateSharedHandle(
            nullptr,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            nullptr,
            &handle
        );
    }
    release(shared_resource);
    if (SUCCEEDED(result)) {
        result = device.device12->OpenSharedHandle(
            handle, IID_PPV_ARGS(&texture.resource12)
        );
    }
    if (handle != nullptr) CloseHandle(handle);
    if (FAILED(result)) {
        if (should_trace_shared_texture_failure()) trace_event(
            "Transport texture %s D3D11->D3D12 failed "
            "hr=0x%08X size=%ux%u format=%u bind11=0x%X misc11=0x%X",
            label, static_cast<unsigned int>(result), width, height,
            static_cast<unsigned int>(format),
            static_cast<unsigned int>(desc11.BindFlags),
            static_cast<unsigned int>(desc11.MiscFlags)
        );
        release_shared_texture(texture);
        return false;
    }
    trace_event(
        "Transport texture %s shared via D3D11->D3D12 "
        "size=%ux%u format=%u bind11=0x%X misc11=0x%X",
        label, width, height, static_cast<unsigned int>(format),
        static_cast<unsigned int>(desc11.BindFlags),
        static_cast<unsigned int>(desc11.MiscFlags)
    );
    return true;
}

[[nodiscard]] bool create_depth_converter(TransportDevice& device) noexcept {
    ID3DBlob* bytecode{};
    ID3DBlob* errors{};
    const auto result = D3DCompile(
        depth_shader_source, sizeof(depth_shader_source) - 1U,
        "Cheeky crop depth transport", nullptr, nullptr, "Main", "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0U, &bytecode, &errors
    );
    release(errors);
    if (FAILED(result)) {
        release(bytecode);
        return false;
    }
    const auto shader_result = device.device11->CreateComputeShader(
        bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr,
        &device.depth_shader
    );
    release(bytecode);
    if (FAILED(shader_result)) return false;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 16U;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device.device11->CreateBuffer(
        &desc, nullptr, &device.depth_constants
    ));
}

void trace_format_support(
    ID3D11Device* const device,
    const char* const label,
    const DXGI_FORMAT format
) noexcept {
    UINT support{};
    const auto result = device->CheckFormatSupport(format, &support);
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support2{format, 0U};
    const auto result2 = device->CheckFeatureSupport(
        D3D11_FEATURE_FORMAT_SUPPORT2,
        &support2,
        sizeof(support2)
    );
    trace_event(
        "D3D11 transport format %s=%u support hr=0x%08X raw=0x%08X "
        "support2Hr=0x%08X raw2=0x%08X texture2d=%u srv=%u rtv=%u "
        "uavLoad=%u uavStore=%u",
        label,
        static_cast<unsigned int>(format),
        static_cast<unsigned int>(result),
        support,
        static_cast<unsigned int>(result2),
        static_cast<unsigned int>(support2.OutFormatSupport2),
        (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0U,
        (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) != 0U,
        (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) != 0U,
        (support2.OutFormatSupport2 &
            D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0U,
        (support2.OutFormatSupport2 &
            D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0U
    );
}

[[nodiscard]] bool create_transport_device(
    ID3D11Device* const device11,
    const D3D11TransportNgx& ngx,
    TransportDevice& device
) noexcept {
    if (!init_contract.valid || ngx.runtime_module == nullptr ||
        ngx.init_ext == nullptr || ngx.allocate_parameters == nullptr ||
        ngx.shutdown == nullptr ||
        device11 == nullptr) {
        return false;
    }
    std::array<wchar_t, MAX_PATH> runtime_path{};
    static_cast<void>(GetModuleFileNameW(
        ngx.runtime_module,
        runtime_path.data(),
        static_cast<DWORD>(runtime_path.size())
    ));
    trace_event(
        "Private D3D12 NGX runtime=%ls initExt=%p allocate=%p "
        "create=%p evaluate=%p release=%p shutdown=%p",
        runtime_path.data(),
        reinterpret_cast<void*>(ngx.init_ext),
        reinterpret_cast<void*>(ngx.allocate_parameters),
        reinterpret_cast<void*>(ngx.backend.create_feature),
        reinterpret_cast<void*>(ngx.backend.evaluate_feature),
        reinterpret_cast<void*>(ngx.backend.release_feature),
        reinterpret_cast<void*>(ngx.shutdown)
    );
    device.device11 = device11;
    device.device11->AddRef();
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    const auto options_result = device11->CheckFeatureSupport(
        D3D11_FEATURE_D3D11_OPTIONS,
        &options,
        sizeof(options)
    );
    trace_event(
        "D3D11 transport sharing capabilities hr=0x%08X "
        "ExtendedResourceSharing=%u",
        static_cast<unsigned int>(options_result),
        SUCCEEDED(options_result) ? options.ExtendedResourceSharing : 0U
    );
    if (FAILED(device11->QueryInterface(IID_PPV_ARGS(&device.device11_1))) ||
        FAILED(device11->QueryInterface(IID_PPV_ARGS(&device.device11_5)))) {
        release_device(device);
        return false;
    }

    IDXGIDevice* dxgi_device{};
    IDXGIAdapter* adapter{};
    if (FAILED(device11->QueryInterface(IID_PPV_ARGS(&dxgi_device))) ||
        FAILED(dxgi_device->GetAdapter(&adapter)) ||
        FAILED(D3D12CreateDevice(
            adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device.device12)))) {
        release(adapter);
        release(dxgi_device);
        release_device(device);
        return false;
    }
    release(adapter);
    release(dxgi_device);

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device.device12->CreateCommandQueue(
            &queue_desc, IID_PPV_ARGS(&device.queue12)))) {
        release_device(device);
        return false;
    }
    if (FAILED(device.queue12->GetTimestampFrequency(
            &device.timestamp_frequency))) {
        device.timestamp_frequency = 0U;
        trace_event("D3D12 transport timestamp frequency unavailable");
    }
    ID3D12CommandAllocator* initial_allocator{};
    if (FAILED(device.device12->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&initial_allocator))) ||
        FAILED(device.device12->CreateCommandList(
            0U, D3D12_COMMAND_LIST_TYPE_DIRECT, initial_allocator, nullptr,
            IID_PPV_ARGS(&device.command_list12)))) {
        release(initial_allocator);
        release_device(device);
        return false;
    }
    static_cast<void>(device.command_list12->Close());
    release(initial_allocator);

    if (FAILED(device.device11_5->CreateFence(
            0U, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&device.fence11)))) {
        release_device(device);
        return false;
    }
    HANDLE fence_handle{};
    if (FAILED(device.fence11->CreateSharedHandle(
            nullptr, GENERIC_ALL, nullptr, &fence_handle)) ||
        FAILED(device.device12->OpenSharedHandle(
            fence_handle, IID_PPV_ARGS(&device.fence12)))) {
        if (fence_handle != nullptr) CloseHandle(fence_handle);
        release_device(device);
        return false;
    }
    CloseHandle(fence_handle);

    if (!create_depth_converter(device)) {
        release_device(device);
        return false;
    }

    DWORD init_exception{};
    const auto init_result = call_init_ext_guarded(
        ngx.init_ext,
        init_contract.application_id,
        init_contract.application_data_path.c_str(),
        device.device12,
        static_cast<int>(init_contract.sdk_version),
        init_contract.feature_common_info,
        init_exception
    );
    trace_event(
        "Private D3D12 NGX Init_Ext app=%llu sdk=%u result=0x%08X "
        "exception=0x%08X",
        init_contract.application_id,
        init_contract.sdk_version,
        static_cast<unsigned int>(init_result),
        static_cast<unsigned int>(init_exception)
    );
    if (init_exception != 0U || !ngx_succeeded(init_result)) {
        release_device(device);
        return false;
    }
    device.ngx_initialized = true;
    device.shutdown = ngx.shutdown;
    DWORD allocate_exception{};
    const auto allocate_result = allocate_parameters_guarded(
        ngx.allocate_parameters,
        &device.ngx_parameters,
        allocate_exception
    );
    trace_event(
        "Private D3D12 NGX AllocateParameters result=0x%08X "
        "exception=0x%08X parameters=%p",
        static_cast<unsigned int>(allocate_result),
        static_cast<unsigned int>(allocate_exception),
        device.ngx_parameters
    );
    if (allocate_exception != 0U || !ngx_succeeded(allocate_result) ||
        device.ngx_parameters == nullptr) {
        release_device(device);
        return false;
    }
    return true;
}

[[nodiscard]] TransportDevice* find_or_create_device(
    ID3D11Device* const device11,
    const D3D11TransportNgx& ngx
) noexcept {
    for (auto& device : transport_devices) {
        if (device.device11 == device11) return &device;
    }
    TransportDevice created{};
    if (!create_transport_device(device11, ngx, created)) return nullptr;
    transport_devices.push_back(created);
    return &transport_devices.back();
}

[[nodiscard]] TransportView* find_or_create_view(
    TransportDevice& device,
    const DlssViewId view_id
) {
    for (auto& view : device.views) {
        if (view.view_id == view_id) return &view;
    }
    device.views.push_back(TransportView{});
    device.views.back().view_id = view_id;
    return &device.views.back();
}

[[nodiscard]] bool slot_matches(
    const TransportSlot& slot,
    const CropGeometry& crop,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const std::uint32_t motion_width,
    const std::uint32_t motion_height,
    const bool nr_enabled,
    const DXGI_FORMAT color_format,
    const DXGI_FORMAT motion_format,
    const DXGI_FORMAT output_format
) noexcept {
    return slot.color.resource12 != nullptr &&
        slot.input_width == crop.input_width &&
        slot.input_height == crop.input_height &&
        slot.output_width == crop.output_width &&
        slot.output_height == crop.output_height &&
        slot.color_format == color_format &&
        slot.motion_format == motion_format &&
        slot.output_format == output_format &&
        (!nr_enabled || (
            slot.nr_color.resource12 != nullptr &&
            slot.nr_input_width == render_width &&
            slot.nr_input_height == render_height &&
            slot.nr_output_width == output_width &&
            slot.nr_output_height == output_height &&
            slot.nr_motion_width == motion_width &&
            slot.nr_motion_height == motion_height
        ));
}

[[nodiscard]] bool initialize_slot(
    TransportDevice& device,
    TransportSlot& slot,
    const CropGeometry& crop,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const std::uint32_t motion_width,
    const std::uint32_t motion_height,
    const bool nr_enabled,
    const DXGI_FORMAT color_format,
    const DXGI_FORMAT motion_format,
    const DXGI_FORMAT output_format
) noexcept {
    release_slot(slot);
    if (!device.format_support_logged) {
        trace_format_support(device.device11, "color", color_format);
        trace_format_support(device.device11, "depth", DXGI_FORMAT_R32_FLOAT);
        trace_format_support(
            device.device11, "motion vectors", motion_format
        );
        trace_format_support(device.device11, "output", output_format);
        device.format_support_logged = true;
    }
    D3D11_QUERY_DESC disjoint_desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0U};
    D3D11_QUERY_DESC timestamp_desc{D3D11_QUERY_TIMESTAMP, 0U};
    const auto allocator_result = device.device12->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)
    );
    const auto nr_allocator_result = FAILED(allocator_result) || !nr_enabled
        ? allocator_result
        : device.device12->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&slot.nr_allocator)
        );
    const auto disjoint_result = FAILED(nr_allocator_result)
        ? nr_allocator_result
        : device.device11->CreateQuery(&disjoint_desc, &slot.timing_disjoint);
    const auto begin_result = FAILED(disjoint_result)
        ? disjoint_result
        : device.device11->CreateQuery(&timestamp_desc, &slot.timing_begin);
    const auto end_result = FAILED(begin_result)
        ? begin_result
        : device.device11->CreateQuery(&timestamp_desc, &slot.timing_end);
    if (FAILED(end_result)) {
        trace_event(
            "Transport command allocator/query creation failed hr=0x%08X",
            static_cast<unsigned int>(end_result)
        );
        release_slot(slot);
        return false;
    }
    D3D12_QUERY_HEAP_DESC query_desc{};
    query_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query_desc.Count = 4U;
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    readback_heap.CreationNodeMask = 1U;
    readback_heap.VisibleNodeMask = 1U;
    D3D12_RESOURCE_DESC readback_desc{};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Width = sizeof(std::uint64_t) * 4U;
    readback_desc.Height = 1U;
    readback_desc.DepthOrArraySize = 1U;
    readback_desc.MipLevels = 1U;
    readback_desc.SampleDesc.Count = 1U;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto query_result = device.device12->CreateQueryHeap(
        &query_desc, IID_PPV_ARGS(&slot.dlss_timing_heap)
    );
    const auto readback_result = FAILED(query_result)
        ? query_result
        : device.device12->CreateCommittedResource(
            &readback_heap,
            D3D12_HEAP_FLAG_NONE,
            &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&slot.dlss_timing_readback)
        );
    if (FAILED(readback_result)) {
        release(slot.dlss_timing_readback);
        release(slot.dlss_timing_heap);
        trace_event(
            "D3D12 DLSS timing resources unavailable hr=0x%08X",
            static_cast<unsigned int>(readback_result)
        );
    }
    if (!create_shared_texture(device, "color",
            crop.input_width, crop.input_height, color_format,
            D3D12_RESOURCE_FLAG_NONE, slot.color) ||
        !create_shared_texture(device, "depth",
            crop.input_width, crop.input_height, DXGI_FORMAT_R32_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, slot.depth) ||
        !create_shared_texture(device, "motion vectors",
            crop.output_width, crop.output_height, motion_format,
            D3D12_RESOURCE_FLAG_NONE, slot.motion_vectors) ||
        !create_shared_texture(device, "output",
            crop.output_width, crop.output_height, output_format,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, slot.output) ||
        (nr_enabled && (
            !create_shared_texture(device, "NR composited color",
                output_width, output_height, output_format,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, slot.nr_color) ||
            !create_shared_texture(device, "NR depth",
                render_width, render_height, DXGI_FORMAT_R32_FLOAT,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, slot.nr_depth) ||
            !create_shared_texture(device, "NR motion vectors",
                motion_width, motion_height, motion_format,
                D3D12_RESOURCE_FLAG_NONE, slot.nr_motion_vectors)
        ))) {
        release_slot(slot);
        return false;
    }
    slot.input_width = crop.input_width;
    slot.input_height = crop.input_height;
    slot.output_width = crop.output_width;
    slot.output_height = crop.output_height;
    slot.nr_input_width = render_width;
    slot.nr_input_height = render_height;
    slot.nr_output_width = output_width;
    slot.nr_output_height = output_height;
    slot.nr_motion_width = motion_width;
    slot.nr_motion_height = motion_height;
    slot.color_format = color_format;
    slot.motion_format = motion_format;
    slot.output_format = output_format;
    return true;
}

[[nodiscard]] DXGI_FORMAT depth_srv_format(const DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

[[nodiscard]] bool convert_depth_crop(
    TransportDevice& device,
    ID3D11DeviceContext* const context,
    ID3D11Resource* const depth,
    const DXGI_FORMAT source_format,
    const std::uint32_t source_x,
    const std::uint32_t source_y,
    const std::uint32_t width,
    const std::uint32_t height,
    SharedTexture& destination
) noexcept {
    const auto srv_format = depth_srv_format(source_format);
    if (srv_format == DXGI_FORMAT_UNKNOWN) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = srv_format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1U;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.Format = DXGI_FORMAT_R32_FLOAT;
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    ID3D11ShaderResourceView* srv{};
    ID3D11UnorderedAccessView* uav{};
    if (FAILED(device.device11->CreateShaderResourceView(depth, &srv_desc, &srv)) ||
        FAILED(device.device11->CreateUnorderedAccessView(
            destination.texture11, &uav_desc, &uav))) {
        release(uav);
        release(srv);
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(device.depth_constants, 0U,
            D3D11_MAP_WRITE_DISCARD, 0U, &mapped))) {
        release(uav);
        release(srv);
        return false;
    }
    const std::uint32_t constants[4]{
        source_x, source_y, width, height
    };
    std::memcpy(mapped.pData, constants, sizeof(constants));
    context->Unmap(device.depth_constants, 0U);

    ID3D11ComputeShader* old_shader{};
    ID3D11ShaderResourceView* old_srv{};
    ID3D11UnorderedAccessView* old_uav{};
    ID3D11Buffer* old_buffer{};
    context->CSGetShader(&old_shader, nullptr, nullptr);
    context->CSGetShaderResources(0U, 1U, &old_srv);
    context->CSGetUnorderedAccessViews(0U, 1U, &old_uav);
    context->CSGetConstantBuffers(0U, 1U, &old_buffer);
    context->CSSetShader(device.depth_shader, nullptr, 0U);
    context->CSSetShaderResources(0U, 1U, &srv);
    context->CSSetUnorderedAccessViews(0U, 1U, &uav, nullptr);
    context->CSSetConstantBuffers(0U, 1U, &device.depth_constants);
    context->Dispatch(
        (width + 15U) / 16U,
        (height + 15U) / 16U,
        1U
    );
    ID3D11ShaderResourceView* null_srv{};
    ID3D11UnorderedAccessView* null_uav{};
    ID3D11Buffer* null_buffer{};
    context->CSSetShaderResources(0U, 1U, &null_srv);
    context->CSSetUnorderedAccessViews(0U, 1U, &null_uav, nullptr);
    context->CSSetConstantBuffers(0U, 1U, &null_buffer);
    context->CSSetShader(old_shader, nullptr, 0U);
    context->CSSetShaderResources(0U, 1U, &old_srv);
    const UINT keep_counter = static_cast<UINT>(-1);
    context->CSSetUnorderedAccessViews(0U, 1U, &old_uav, &keep_counter);
    context->CSSetConstantBuffers(0U, 1U, &old_buffer);
    release(old_buffer);
    release(old_uav);
    release(old_srv);
    release(old_shader);
    release(uav);
    release(srv);
    return true;
}

void transition(
    ID3D12GraphicsCommandList* const list,
    ID3D12Resource* const resource,
    const D3D12_RESOURCE_STATES before,
    const D3D12_RESOURCE_STATES after
) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1U, &barrier);
}

void resolve_timing(
    ID3D11DeviceContext* const context,
    TransportSlot& slot
) noexcept {
    if (!slot.timing_pending) return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    std::uint64_t begin{};
    std::uint64_t end{};
    if (context->GetData(slot.timing_disjoint, &disjoint, sizeof(disjoint), 0U) != S_OK ||
        context->GetData(slot.timing_begin, &begin, sizeof(begin), 0U) != S_OK ||
        context->GetData(slot.timing_end, &end, sizeof(end), 0U) != S_OK) {
        return;
    }
    slot.timing_pending = false;
    if (!disjoint.Disjoint && disjoint.Frequency != 0U && end >= begin) {
        const auto milliseconds = static_cast<float>(
            static_cast<double>(end - begin) * 1000.0 /
            static_cast<double>(disjoint.Frequency)
        );
        diagnostic_note_transport_gpu_time(milliseconds);
    }
}

void resolve_dlss_timing(
    const TransportDevice& device,
    TransportSlot& slot
) noexcept {
    if (!slot.dlss_timing_pending ||
        slot.dlss_timing_readback == nullptr ||
        device.timestamp_frequency == 0U) {
        return;
    }
    const auto query_count = (std::max)(slot.dlss_timing_query_count, 2U);
    const D3D12_RANGE read_range{
        0U, sizeof(std::uint64_t) * query_count
    };
    void* mapped{};
    if (FAILED(slot.dlss_timing_readback->Map(0U, &read_range, &mapped)) ||
        mapped == nullptr) {
        return;
    }
    const auto* const timestamps = static_cast<const std::uint64_t*>(mapped);
    const auto begin = timestamps[0U];
    const auto sr_end = timestamps[1U];
    const auto nr_begin = query_count >= 4U ? timestamps[2U] : 0U;
    const auto nr_end = query_count >= 4U ? timestamps[3U] : 0U;
    const bool nr_foveated = slot.dlss_nr_timing_foveated;
    const D3D12_RANGE written_range{0U, 0U};
    slot.dlss_timing_readback->Unmap(0U, &written_range);
    slot.dlss_timing_pending = false;
    slot.dlss_timing_query_count = 0U;
    if (sr_end >= begin) {
        const auto milliseconds = static_cast<float>(
            static_cast<double>(sr_end - begin) * 1000.0 /
            static_cast<double>(device.timestamp_frequency)
        );
        diagnostic_note_foveated_dlss_gpu_time(milliseconds);
    }
    if (query_count >= 4U && nr_end >= nr_begin) {
        const auto milliseconds = static_cast<float>(
            static_cast<double>(nr_end - nr_begin) * 1000.0 /
            static_cast<double>(device.timestamp_frequency)
        );
        diagnostic_note_dlss_nr_gpu_time(
            DiagnosticApi::d3d11, milliseconds, nr_foveated
        );
    }
}

struct TimingScope {
    ID3D11DeviceContext* context{};
    TransportSlot* slot{};
    bool active{};

    TimingScope(ID3D11DeviceContext* const in_context, TransportSlot& in_slot)
        : context(in_context), slot(&in_slot),
          active(!in_slot.timing_pending && diagnostic_should_sample_gpu_time(
              DiagnosticGpuTiming::transport_total
          )) {
        if (!active) return;
        context->Begin(slot->timing_disjoint);
        context->End(slot->timing_begin);
    }

    void finish() noexcept {
        if (!active) return;
        context->End(slot->timing_end);
        context->End(slot->timing_disjoint);
        slot->timing_pending = true;
        active = false;
    }

    ~TimingScope() { finish(); }
};

[[nodiscard]] bool reject_transport(
    const D3D11TransportStatus status
) noexcept {
    diagnostic_note_d3d11_transport_status(status);
    return false;
}

[[nodiscard]] bool recover_init_contract(
    const D3D11TransportNgx& ngx
) noexcept {
    if (init_contract.valid) return true;
    if (ngx.get_application_id == nullptr || ngx.get_api_version == nullptr) {
        return false;
    }

    const auto application_id = ngx.get_application_id();
    const auto sdk_version = ngx.get_api_version();
    if (application_id == 0U || sdk_version == 0U) return false;

    std::array<wchar_t, MAX_PATH> executable_path{};
    const auto length = GetModuleFileNameW(
        nullptr,
        executable_path.data(),
        static_cast<DWORD>(executable_path.size())
    );
    if (length == 0U || length >= executable_path.size()) return false;
    std::wstring executable_directory(executable_path.data(), length);
    const auto separator = executable_directory.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return false;

    init_contract.application_id = application_id;
    init_contract.application_data_path.assign(
        executable_directory.data(), separator
    );
    init_contract.feature_common_info = nullptr;
    init_contract.sdk_version = sdk_version;
    init_contract.valid = true;
    trace_event(
        "Recovered NGX init contract app=%llu sdk=%u path=%ls",
        application_id,
        sdk_version,
        init_contract.application_data_path.c_str()
    );
    return true;
}

void mirror_transport_parameters(
    NgxParameters* const destination,
    const NgxParameters* const source,
    const DlssFrameContract& contract
) noexcept {
    destination->Set("DLSS.Feature.Create.Flags", contract.create_flags);
    destination->Set("PerfQualityValue", contract.perf_quality);
    destination->Set("Jitter.Offset.X", contract.jitter_x);
    destination->Set("Jitter.Offset.Y", contract.jitter_y);
    destination->Set("MV.Scale.X", contract.motion_vector_scale_x);
    destination->Set("MV.Scale.Y", contract.motion_vector_scale_y);
    destination->Set("Pre.Exposure", contract.pre_exposure);
    destination->Set("Exposure.Scale", contract.exposure_scale);
    destination->Set("Sharpness", get_float(source, "Sharpness"));
    destination->Set("CreationNodeMask", 1U);
    destination->Set("VisibilityNodeMask", 1U);
    destination->Set("RTXValue", 0);
    destination->Set(
        "ExposureTexture", static_cast<ID3D12Resource*>(nullptr)
    );

    constexpr std::array<const char*, 6U> preset_names{
        "DLSS.Hint.Render.Preset.DLAA",
        "DLSS.Hint.Render.Preset.Quality",
        "DLSS.Hint.Render.Preset.Balanced",
        "DLSS.Hint.Render.Preset.Performance",
        "DLSS.Hint.Render.Preset.UltraPerformance",
        "DLSS.Hint.Render.Preset.UltraQuality",
    };
    for (const auto* const name : preset_names) {
        int signed_value{};
        if (source != nullptr &&
            ngx_succeeded(source->Get(name, &signed_value))) {
            destination->Set(name, signed_value);
            continue;
        }
        unsigned int unsigned_value{};
        if (source != nullptr &&
            ngx_succeeded(source->Get(name, &unsigned_value))) {
            destination->Set(name, unsigned_value);
        }
    }
}

}  // namespace

void remember_d3d11_ngx_init(
    const unsigned long long application_id,
    const wchar_t* const application_data_path,
    const void* const feature_common_info,
    const std::uint32_t sdk_version
) noexcept {
    std::lock_guard lock(transport_mutex);
    init_contract.application_id = application_id;
    init_contract.application_data_path = application_data_path == nullptr
        ? std::wstring{}
        : std::wstring{application_data_path};
    init_contract.feature_common_info = feature_common_info;
    init_contract.sdk_version = sdk_version;
    init_contract.valid = true;
}

bool evaluate_d3d11_via_d3d12(
    ID3D11DeviceContext* const context,
    const NgxHandle* const game_handle,
    const NgxParameters* const parameters,
    const Settings& settings,
    const D3D11TransportNgx& ngx,
    NgxResult& result
) noexcept {
    result = 0xBAD00005U;
    if ((!settings.enabled && !settings.nr_enabled) || context == nullptr ||
        game_handle == nullptr ||
        parameters == nullptr) {
        return reject_transport(D3D11TransportStatus::unsupported_context);
    }
    const auto view_id = static_cast<DlssViewId>(
        reinterpret_cast<std::uintptr_t>(game_handle)
    );
    auto transport_settings = settings;
    if (!transport_settings.enabled && transport_settings.nr_enabled) {
        // NR-only mode still needs the transport to run the game's SR feature
        // first. Use a full-frame SR crop, then apply the independent NR crop.
        transport_settings.enabled = true;
        transport_settings.width = 1.0F;
        transport_settings.height = 1.0F;
        transport_settings.x_offset = 0.0F;
        transport_settings.height_offset = 0.0F;
        transport_settings.roundness = 0.0F;
        transport_settings.transition_width = 0.0F;
        transport_settings.alignment_border_enabled = false;
    }
    const auto effective_settings = settings_for_view(
        transport_settings,
        view_id
    );
    const auto nr_settings = settings_for_view(settings, view_id);
    if (ngx.runtime_module == nullptr || ngx.init_ext == nullptr ||
        ngx.allocate_parameters == nullptr ||
        ngx.shutdown == nullptr ||
        ngx.backend.create_feature == nullptr ||
        ngx.backend.evaluate_feature == nullptr ||
        ngx.backend.release_feature == nullptr) {
        const auto sequence = ngx_callback_failure_sequence.fetch_add(
            1U, std::memory_order_relaxed
        );
        if (sequence < 8U || sequence % 300U == 0U) {
            trace_event(
                "Private D3D12 NGX callbacks missing seq=%llu module=%p "
                "initExt=%p allocate=%p create=%p evaluate=%p release=%p "
                "shutdown=%p",
                static_cast<unsigned long long>(sequence),
                ngx.runtime_module,
                reinterpret_cast<void*>(ngx.init_ext),
                reinterpret_cast<void*>(ngx.allocate_parameters),
                reinterpret_cast<void*>(ngx.backend.create_feature),
                reinterpret_cast<void*>(ngx.backend.evaluate_feature),
                reinterpret_cast<void*>(ngx.backend.release_feature),
                reinterpret_cast<void*>(ngx.shutdown)
            );
        }
        return reject_transport(D3D11TransportStatus::missing_callbacks);
    }

    auto* const color = get_resource(parameters, "Color");
    auto* const depth = get_resource(parameters, "Depth");
    auto* const motion = get_resource(parameters, "MotionVectors");
    auto* const output = get_resource(parameters, "Output");
    if (color == nullptr || depth == nullptr || motion == nullptr ||
        output == nullptr) {
        return reject_transport(D3D11TransportStatus::missing_resources);
    }

    ID3D11Texture2D* color_texture{};
    ID3D11Texture2D* depth_texture{};
    ID3D11Texture2D* motion_texture{};
    ID3D11Texture2D* output_texture{};
    if (FAILED(color->QueryInterface(IID_PPV_ARGS(&color_texture))) ||
        FAILED(depth->QueryInterface(IID_PPV_ARGS(&depth_texture))) ||
        FAILED(motion->QueryInterface(IID_PPV_ARGS(&motion_texture))) ||
        FAILED(output->QueryInterface(IID_PPV_ARGS(&output_texture)))) {
        release(output_texture); release(motion_texture);
        release(depth_texture); release(color_texture);
        return reject_transport(D3D11TransportStatus::unsupported_resources);
    }
    D3D11_TEXTURE2D_DESC color_desc{}, depth_desc{}, motion_desc{}, output_desc{};
    color_texture->GetDesc(&color_desc);
    depth_texture->GetDesc(&depth_desc);
    motion_texture->GetDesc(&motion_desc);
    output_texture->GetDesc(&output_desc);
    release(output_texture); release(motion_texture);
    release(depth_texture); release(color_texture);

    const auto width = get_ui(parameters, "Width");
    const auto height = get_ui(parameters, "Height");
    const auto out_width = get_ui(parameters, "OutWidth");
    const auto out_height = get_ui(parameters, "OutHeight");
    auto render_width = get_ui(parameters, "DLSS.Render.Subrect.Dimensions.Width");
    auto render_height = get_ui(parameters, "DLSS.Render.Subrect.Dimensions.Height");
    if (render_width == 0U) render_width = width;
    if (render_height == 0U) render_height = height;
    const auto color_x = get_ui(parameters, "DLSS.Input.Color.Subrect.Base.X");
    const auto color_y = get_ui(parameters, "DLSS.Input.Color.Subrect.Base.Y");
    const auto depth_x = get_ui(parameters, "DLSS.Input.Depth.Subrect.Base.X");
    const auto depth_y = get_ui(parameters, "DLSS.Input.Depth.Subrect.Base.Y");
    const auto mv_x = get_ui(parameters, "DLSS.Input.MV.Subrect.Base.X");
    const auto mv_y = get_ui(parameters, "DLSS.Input.MV.Subrect.Base.Y");
    const auto output_x = get_ui(parameters, "DLSS.Output.Subrect.Base.X");
    const auto output_y = get_ui(parameters, "DLSS.Output.Subrect.Base.Y");
    CropGeometry crop{};
    if (render_width == 0U || render_height == 0U || out_width == 0U ||
        out_height == 0U || color_desc.SampleDesc.Count != 1U ||
        depth_desc.SampleDesc.Count != 1U || motion_desc.SampleDesc.Count != 1U ||
        output_desc.SampleDesc.Count != 1U ||
        !in_bounds(color_x, render_width, color_desc.Width) ||
        !in_bounds(color_y, render_height, color_desc.Height) ||
        !in_bounds(depth_x, render_width, depth_desc.Width) ||
        !in_bounds(depth_y, render_height, depth_desc.Height) ||
        !calculate_crop(effective_settings, render_width, render_height,
            out_width, out_height, output_x, output_y, crop) ||
        crop.output_width < 32U || crop.output_height < 32U) {
        const bool unsupported_samples = color_desc.SampleDesc.Count != 1U ||
            depth_desc.SampleDesc.Count != 1U ||
            motion_desc.SampleDesc.Count != 1U ||
            output_desc.SampleDesc.Count != 1U;
        return reject_transport(
            unsupported_samples
                ? D3D11TransportStatus::unsupported_resources
                : D3D11TransportStatus::invalid_dimensions
        );
    }

    const auto create_flags = get_integer_bits(parameters, "DLSS.Feature.Create.Flags");
    const bool mv_low_res = (create_flags & dlss_feature_flag_mv_low_res) != 0U;
    const auto mv_crop_x = mv_low_res
        ? crop.input_base_x
        : crop.output_base_x - output_x;
    const auto mv_crop_y = mv_low_res
        ? crop.input_base_y
        : crop.output_base_y - output_y;
    const auto mv_width = mv_low_res ? crop.input_width : crop.output_width;
    const auto mv_height = mv_low_res ? crop.input_height : crop.output_height;
    const auto nr_motion_width = mv_low_res ? render_width : out_width;
    const auto nr_motion_height = mv_low_res ? render_height : out_height;
    DlssNrGeometry nr_geometry{};
    if (settings.nr_enabled && !calculate_dlss_nr_geometry(
            nr_settings,
            out_width,
            out_height,
            nr_geometry
        )) {
        return reject_transport(D3D11TransportStatus::invalid_dimensions);
    }
    const auto nr_depth_x = settings.nr_enabled
        ? scale_range(nr_geometry.base_x, nr_geometry.width, render_width, out_width)
        : ScaledRange{};
    const auto nr_depth_y = settings.nr_enabled
        ? scale_range(nr_geometry.base_y, nr_geometry.height, render_height, out_height)
        : ScaledRange{};
    const auto nr_mv_region_x = settings.nr_enabled
        ? scale_range(
            nr_geometry.base_x,
            nr_geometry.width,
            nr_motion_width,
            out_width
        )
        : ScaledRange{};
    const auto nr_mv_region_y = settings.nr_enabled
        ? scale_range(
            nr_geometry.base_y,
            nr_geometry.height,
            nr_motion_height,
            out_height
        )
        : ScaledRange{};
    if (!in_bounds(mv_x + mv_crop_x, mv_width, motion_desc.Width) ||
        !in_bounds(mv_y + mv_crop_y, mv_height, motion_desc.Height) ||
        (settings.nr_enabled && (
            !in_bounds(
                mv_x + nr_mv_region_x.base,
                nr_mv_region_x.extent,
                motion_desc.Width
            ) ||
            !in_bounds(
                mv_y + nr_mv_region_y.base,
                nr_mv_region_y.extent,
                motion_desc.Height
            ) ||
            !in_bounds(
                output_x + nr_geometry.base_x,
                nr_geometry.width,
                output_desc.Width
            ) ||
            !in_bounds(
                output_y + nr_geometry.base_y,
                nr_geometry.height,
                output_desc.Height
            )
        ))) {
        return reject_transport(
            D3D11TransportStatus::unsupported_motion_vectors
        );
    }
    note_stereo_view_geometry(
        view_id,
        render_width,
        render_height,
        out_width,
        out_height,
        crop
    );

    DlssFrameContract contract{};
    contract.view_id = view_id;
    contract.render_width = render_width;
    contract.render_height = render_height;
    contract.output_width = out_width;
    contract.output_height = out_height;
    contract.color_base_x = color_x;
    contract.color_base_y = color_y;
    contract.depth_base_x = depth_x;
    contract.depth_base_y = depth_y;
    contract.mv_base_x = mv_x;
    contract.mv_base_y = mv_y;
    contract.output_base_x = output_x;
    contract.output_base_y = output_y;
    contract.motion_vectors_low_res = mv_low_res;
    contract.depth_inverted =
        (create_flags & dlss_feature_flag_depth_inverted) != 0U;
    contract.reset = get_int(parameters, "Reset") != 0;
    contract.create_flags = create_flags;
    contract.perf_quality = get_ui(parameters, "PerfQualityValue");
    contract.jitter_x = get_float(parameters, "Jitter.Offset.X");
    contract.jitter_y = get_float(parameters, "Jitter.Offset.Y");
    contract.motion_vector_scale_x = get_float(parameters, "MV.Scale.X", 1.0F);
    contract.motion_vector_scale_y = get_float(parameters, "MV.Scale.Y", 1.0F);
    contract.pre_exposure = get_float(parameters, "Pre.Exposure", 1.0F);
    contract.exposure_scale = get_float(parameters, "Exposure.Scale", 1.0F);

    std::lock_guard lock(transport_mutex);
    if (!recover_init_contract(ngx)) {
        return reject_transport(
            D3D11TransportStatus::missing_initialization_data
        );
    }
    ID3D11Device* device11{};
    context->GetDevice(&device11);
    auto* const device = find_or_create_device(device11, ngx);
    release(device11);
    if (device == nullptr) {
        return reject_transport(
            D3D11TransportStatus::device_initialization_failed
        );
    }
    ID3D11DeviceContext4* context4{};
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context4)))) {
        return reject_transport(D3D11TransportStatus::unsupported_context);
    }

    auto* const view = find_or_create_view(*device, contract.view_id);
    auto& slot = view->slots[view->next_slot++ % transport_slot_count];
    if (slot.done_value != 0U &&
        device->fence12->GetCompletedValue() < slot.done_value) {
        release(context4);
        return reject_transport(D3D11TransportStatus::transport_slot_busy);
    }
    resolve_timing(context, slot);
    resolve_dlss_timing(*device, slot);
    if (!slot_matches(
            slot, crop, nr_depth_x.extent, nr_depth_y.extent,
            nr_geometry.width, nr_geometry.height,
            nr_mv_region_x.extent, nr_mv_region_y.extent, settings.nr_enabled,
            color_desc.Format, motion_desc.Format, output_desc.Format
        ) && !initialize_slot(
            *device, slot, crop, nr_depth_x.extent, nr_depth_y.extent,
            nr_geometry.width, nr_geometry.height,
            nr_mv_region_x.extent, nr_mv_region_y.extent,
            settings.nr_enabled, color_desc.Format,
            motion_desc.Format, output_desc.Format
        )) {
        release(context4);
        return reject_transport(
            D3D11TransportStatus::resource_initialization_failed
        );
    }
    TimingScope timing{context, slot};

    D3D11_BOX color_box{
        color_x + crop.input_base_x, color_y + crop.input_base_y, 0U,
        color_x + crop.input_base_x + crop.input_width,
        color_y + crop.input_base_y + crop.input_height, 1U
    };
    context->CopySubresourceRegion(slot.color.texture11, 0U, 0U, 0U, 0U,
        color, 0U, &color_box);
    if (!convert_depth_crop(*device, context, depth, depth_desc.Format,
            depth_x + crop.input_base_x, depth_y + crop.input_base_y,
            crop.input_width, crop.input_height, slot.depth)) {
        release(context4);
        return reject_transport(D3D11TransportStatus::depth_conversion_failed);
    }
    D3D11_BOX mv_box{
        mv_x + mv_crop_x, mv_y + mv_crop_y, 0U,
        mv_x + mv_crop_x + mv_width, mv_y + mv_crop_y + mv_height, 1U
    };
    context->CopySubresourceRegion(slot.motion_vectors.texture11, 0U,
        0U, 0U, 0U, motion, 0U, &mv_box);
    if (settings.nr_enabled) {
        if (!convert_depth_crop(
                *device, context, depth, depth_desc.Format,
                depth_x + nr_depth_x.base,
                depth_y + nr_depth_y.base,
                nr_depth_x.extent,
                nr_depth_y.extent,
                slot.nr_depth
            )) {
            release(context4);
            return reject_transport(
                D3D11TransportStatus::depth_conversion_failed
            );
        }
        D3D11_BOX nr_mv_box{
            mv_x + nr_mv_region_x.base,
            mv_y + nr_mv_region_y.base,
            0U,
            mv_x + nr_mv_region_x.base + nr_mv_region_x.extent,
            mv_y + nr_mv_region_y.base + nr_mv_region_y.extent,
            1U
        };
        context->CopySubresourceRegion(
            slot.nr_motion_vectors.texture11, 0U,
            0U, 0U, 0U, motion, 0U, &nr_mv_box
        );
    }

    const auto ready_value = device->next_fence_value++;
    if (FAILED(context4->Signal(device->fence11, ready_value)) ||
        FAILED(device->queue12->Wait(device->fence12, ready_value)) ||
        FAILED(slot.allocator->Reset()) ||
        FAILED(device->command_list12->Reset(slot.allocator, nullptr))) {
        release(context4);
        return reject_transport(D3D11TransportStatus::synchronization_failed);
    }

    transition(device->command_list12, slot.color.resource12,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(device->command_list12, slot.depth.resource12,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(device->command_list12, slot.motion_vectors.resource12,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(device->command_list12, slot.output.resource12,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    D3D12DlssInputs inputs{};
    inputs.color = slot.color.resource12;
    inputs.depth = slot.depth.resource12;
    inputs.motion_vectors = slot.motion_vectors.resource12;
    inputs.output = slot.output.resource12;
    mirror_transport_parameters(device->ngx_parameters, parameters, contract);
    const bool measure_dlss = slot.dlss_timing_heap != nullptr &&
        slot.dlss_timing_readback != nullptr &&
        device->timestamp_frequency != 0U &&
        diagnostic_should_sample_gpu_time(
            DiagnosticGpuTiming::foveated_dlss
        );
    if (measure_dlss) {
        device->command_list12->EndQuery(
            slot.dlss_timing_heap,
            D3D12_QUERY_TYPE_TIMESTAMP,
            0U
        );
    }
    D3D12BackendTiming backend_timing{};
    backend_timing.query_heap = measure_dlss ? slot.dlss_timing_heap : nullptr;
    result = evaluate_d3d12_backend(
        device->command_list12, contract, inputs,
        device->ngx_parameters, crop, ngx.backend,
        &backend_timing
    );
    if (measure_dlss) {
        if (!backend_timing.sr_timestamp_written) {
            device->command_list12->EndQuery(
                slot.dlss_timing_heap,
                D3D12_QUERY_TYPE_TIMESTAMP,
                1U
            );
        }
        device->command_list12->ResolveQueryData(
            slot.dlss_timing_heap,
            D3D12_QUERY_TYPE_TIMESTAMP,
            0U,
            2U,
            slot.dlss_timing_readback,
            0U
        );
        slot.dlss_timing_query_count = 2U;
    }

    transition(device->command_list12, slot.output.resource12,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    transition(device->command_list12, slot.motion_vectors.resource12,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    transition(device->command_list12, slot.depth.resource12,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    transition(device->command_list12, slot.color.resource12,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (FAILED(device->command_list12->Close())) {
        release(context4);
        return reject_transport(D3D11TransportStatus::synchronization_failed);
    }
    ID3D12CommandList* lists[]{device->command_list12};
    device->queue12->ExecuteCommandLists(1U, lists);
    slot.done_value = device->next_fence_value++;
    if (FAILED(device->queue12->Signal(device->fence12, slot.done_value)) ||
        FAILED(context4->Wait(device->fence11, slot.done_value))) {
        release(context4);
        return reject_transport(D3D11TransportStatus::synchronization_failed);
    }
    if (!ngx_succeeded(result)) {
        slot.dlss_timing_pending = measure_dlss;
        release(context4);
        return reject_transport(D3D11TransportStatus::ngx_evaluation_failed);
    }
    if (!composite_d3d11_crop(context, color, output, slot.output.texture11,
            contract, crop, effective_settings)) {
        slot.dlss_timing_pending = measure_dlss;
        release(context4);
        return reject_transport(D3D11TransportStatus::compositing_failed);
    }

    bool nr_succeeded{};
    if (settings.nr_enabled) {
        D3D11_BOX output_box{
            output_x + nr_geometry.base_x,
            output_y + nr_geometry.base_y,
            0U,
            output_x + nr_geometry.base_x + nr_geometry.width,
            output_y + nr_geometry.base_y + nr_geometry.height,
            1U
        };
        context->CopySubresourceRegion(
            slot.nr_color.texture11, 0U, 0U, 0U, 0U,
            output, 0U, &output_box
        );

        const auto nr_ready_value = device->next_fence_value++;
        const bool nr_ready =
            SUCCEEDED(context4->Signal(device->fence11, nr_ready_value)) &&
            SUCCEEDED(device->queue12->Wait(device->fence12, nr_ready_value)) &&
            SUCCEEDED(slot.nr_allocator->Reset()) &&
            SUCCEEDED(device->command_list12->Reset(
                slot.nr_allocator, nullptr
            ));
        if (nr_ready) {
            transition(
                device->command_list12, slot.nr_color.resource12,
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS
            );
            transition(
                device->command_list12, slot.nr_depth.resource12,
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            );
            transition(
                device->command_list12, slot.nr_motion_vectors.resource12,
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            );
            if (measure_dlss) {
                device->command_list12->EndQuery(
                    slot.dlss_timing_heap,
                    D3D12_QUERY_TYPE_TIMESTAMP,
                    2U
                );
            }
            const DlssNrFrame nr_frame{
                contract.view_id,
                DlssNrRoute::d3d11_transport,
                device->command_list12,
                slot.nr_color.resource12,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                slot.nr_depth.resource12,
                slot.nr_motion_vectors.resource12,
                nr_depth_x.extent,
                nr_depth_y.extent,
                out_width,
                out_height,
                0U,
                0U,
                nr_depth_x.extent,
                nr_depth_y.extent,
                0U,
                0U,
                nr_mv_region_x.extent,
                nr_mv_region_y.extent,
                contract.motion_vector_scale_x,
                contract.motion_vector_scale_y,
                contract.depth_inverted,
                contract.reset,
                contract.create_flags,
                0U,
                0U,
                true,
            };
            nr_succeeded = evaluate_dlss_nr(nr_frame, nr_settings);
            if (measure_dlss && nr_succeeded) {
                device->command_list12->EndQuery(
                    slot.dlss_timing_heap,
                    D3D12_QUERY_TYPE_TIMESTAMP,
                    3U
                );
                device->command_list12->ResolveQueryData(
                    slot.dlss_timing_heap,
                    D3D12_QUERY_TYPE_TIMESTAMP,
                    2U,
                    2U,
                    slot.dlss_timing_readback,
                    sizeof(std::uint64_t) * 2U
                );
                slot.dlss_timing_query_count = 4U;
                slot.dlss_nr_timing_foveated = nr_settings.nr_foveated;
            }
            transition(
                device->command_list12, slot.nr_motion_vectors.resource12,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COMMON
            );
            transition(
                device->command_list12, slot.nr_depth.resource12,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COMMON
            );
            transition(
                device->command_list12, slot.nr_color.resource12,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON
            );
            if (SUCCEEDED(device->command_list12->Close())) {
                device->queue12->ExecuteCommandLists(1U, lists);
                const auto nr_done_value = device->next_fence_value++;
                if (SUCCEEDED(device->queue12->Signal(
                        device->fence12, nr_done_value
                    ))) {
                    slot.done_value = nr_done_value;
                    if (SUCCEEDED(context4->Wait(
                            device->fence11, nr_done_value
                        )) && nr_succeeded) {
                        context->CopySubresourceRegion(
                            output,
                            0U,
                            output_x + nr_geometry.base_x,
                            output_y + nr_geometry.base_y,
                            0U,
                            slot.nr_color.texture11, 0U, nullptr
                        );
                    } else {
                        nr_succeeded = false;
                    }
                } else {
                    nr_succeeded = false;
                    slot.dlss_timing_query_count = 2U;
                }
            } else {
                nr_succeeded = false;
                slot.dlss_timing_query_count = 2U;
            }
        }
    }
    slot.dlss_timing_pending = measure_dlss;
    release(context4);
    timing.finish();
    diagnostic_note_d3d11_transport_status(D3D11TransportStatus::active);
    diagnostic_note_activation(DiagnosticApi::d3d11, crop);
    return true;
}

void release_d3d11_transport_view(const NgxHandle* const game_handle) noexcept {
    if (game_handle == nullptr) return;
    const auto view_id = static_cast<DlssViewId>(
        reinterpret_cast<std::uintptr_t>(game_handle)
    );
    std::lock_guard lock(transport_mutex);
    for (auto& device : transport_devices) {
        for (auto iterator = device.views.begin();
             iterator != device.views.end(); ++iterator) {
            if (iterator->view_id != view_id) continue;
            release_d3d12_view(view_id);
            for (auto& slot : iterator->slots) release_slot(slot);
            device.views.erase(iterator);
            break;
        }
    }
}

void release_d3d11_d3d12_transport() noexcept {
    std::lock_guard lock(transport_mutex);
    for (auto& device : transport_devices) release_device(device);
    transport_devices.clear();
    init_contract = {};
}

}  // namespace cheeky::foveated_dlss
