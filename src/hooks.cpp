#include "backend.hpp"
#include "d3d11_d3d12_transport.hpp"
#include "d3d11_peripheral_dlaa.hpp"
#include "d3d12_ngx_dispatch.hpp"
#include "diagnostics.hpp"
#include "gaze_foveation.hpp"
#include "ngx_abi.hpp"
#include "peripheral_dlaa.hpp"
#include "runtime.hpp"
#include "settings.hpp"

#include <Windows.h>
#include <Psapi.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <iterator>
#include <mutex>

namespace cheeky::foveated_dlss {

extern "C" {

void register_d3d11_game_feature(
    const NgxHandle*,
    std::uint32_t,
    NgxResult (*)(ID3D11DeviceContext*, std::uint32_t, NgxParameters*, NgxHandle**),
    NgxResult (*)(NgxHandle*)
) noexcept;

void unregister_d3d11_game_feature(const NgxHandle*) noexcept;

D3D11Evaluation* prepare_d3d11_private(
    ID3D11DeviceContext*,
    const NgxHandle*,
    const NgxParameters*,
    const Settings&
) noexcept;

const NgxHandle* d3d11_private_handle(const D3D11Evaluation*) noexcept;
bool is_d3d11_private_handle(const NgxHandle*) noexcept;

}  // extern "C"

namespace {

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);

using InitD3D11Fn = NgxResult (*)(
    unsigned long long,
    const wchar_t*,
    ID3D11Device*,
    const void*,
    std::uint32_t
);

using CreateD3D11Fn = NgxResult (*)(
    ID3D11DeviceContext*,
    std::uint32_t,
    NgxParameters*,
    NgxHandle**
);
using EvaluateD3D11Fn = NgxResult (*)(
    ID3D11DeviceContext*,
    const NgxHandle*,
    const NgxParameters*,
    NgxProgressCallback
);
using EvaluateD3D11CFn = NgxResult (*)(
    ID3D11DeviceContext*,
    const NgxHandle*,
    const NgxParameters*,
    NgxProgressCallbackC
);
using ReleaseD3D11Fn = NgxResult (*)(NgxHandle*);

using CreateD3D12Fn = NgxResult (*)(
    ID3D12GraphicsCommandList*,
    std::uint32_t,
    NgxParameters*,
    NgxHandle**
);
using EvaluateD3D12Fn = D3D12NgxEvaluateFn;
using EvaluateD3D12CFn = NgxResult (*)(
    ID3D12GraphicsCommandList*,
    const NgxHandle*,
    const NgxParameters*,
    NgxProgressCallbackC
);
using ReleaseD3D12Fn = NgxResult (*)(NgxHandle*);

struct SlStructType {
    std::uint32_t data1{};
    std::uint16_t data2{};
    std::uint16_t data3{};
    std::uint8_t data4[8]{};
};

struct SlBaseStructure {
    SlBaseStructure* next{};
    SlStructType struct_type{};
    std::size_t struct_version{};
};

struct SlExtent {
    std::uint32_t top{};
    std::uint32_t left{};
    std::uint32_t width{};
    std::uint32_t height{};
};

enum class SlResourceType : char {
    texture_2d = 0,
};

struct SlResource : SlBaseStructure {
    SlResourceType type{SlResourceType::texture_2d};
    void* native{};
    void* memory{};
    void* view{};
    std::uint32_t state{0xFFFFFFFFU};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t native_format{};
    std::uint32_t mip_levels{};
    std::uint32_t array_layers{};
    std::uint64_t gpu_virtual_address{};
    std::uint32_t flags{};
    std::uint32_t usage{};
    std::uint32_t reserved{};
};

struct SlResourceTag : SlBaseStructure {
    SlResource* resource{};
    std::uint32_t type{};
    std::uint32_t lifecycle{};
    SlExtent extent{};
};

struct SlFloat2 { float x{}; float y{}; };
struct SlFloat3 { float x{}; float y{}; float z{}; };
struct SlFloat4x4 { float values[16]{}; };

struct SlConstants : SlBaseStructure {
    SlFloat4x4 camera_view_to_clip{};
    SlFloat4x4 clip_to_camera_view{};
    SlFloat4x4 clip_to_lens_clip{};
    SlFloat4x4 clip_to_prev_clip{};
    SlFloat4x4 prev_clip_to_clip{};
    SlFloat2 jitter_offset{};
    SlFloat2 motion_vector_scale{};
    SlFloat2 camera_pinhole_offset{};
    SlFloat3 camera_position{};
    SlFloat3 camera_up{};
    SlFloat3 camera_right{};
    SlFloat3 camera_forward{};
    float camera_near{};
    float camera_far{};
    float camera_fov{};
    float camera_aspect_ratio{};
    float motion_vectors_invalid_value{};
    char depth_inverted{};
    char camera_motion_included{};
    char motion_vectors_3d{};
    char reset{};
    char orthographic_projection{};
    char motion_vectors_dilated{};
    char motion_vectors_jittered{};
    float minimum_relative_linear_depth_object_separation{};
};

struct SlDlssOptions : SlBaseStructure {
    std::uint32_t mode{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    float sharpness{};
    float pre_exposure{};
    float exposure_scale{};
    char color_buffers_hdr{};
    char indicator_invert_axis_x{};
    char indicator_invert_axis_y{};
    std::uint32_t dlaa_preset{};
    std::uint32_t quality_preset{};
    std::uint32_t balanced_preset{};
    std::uint32_t performance_preset{};
    std::uint32_t ultra_performance_preset{};
    std::uint32_t ultra_quality_preset{};
    char use_auto_exposure{};
    char alpha_upscaling_enabled{};
};

struct SlViewportHandle : SlBaseStructure {
    std::uint32_t value{0xFFFFFFFFU};
};

using SlEvaluateFeatureFn = std::uint32_t (*)(
    std::uint32_t, const void*, const void* const*, std::uint32_t, void*
);
using SlSetTagFn = std::uint32_t (*)(
    const void*, const void*, std::uint32_t, void*
);
using SlSetTagForFrameFn = std::uint32_t (*)(
    const void*, const void*, const void*, std::uint32_t, void*
);
using SlSetConstantsFn = std::uint32_t (*)(
    const void*, const void*, const void*
);
using SlGetFeatureFunctionFn = std::uint32_t (*)(
    std::uint32_t, const char*, void**
);
using SlDlssSetOptionsFn = std::uint32_t (*)(
    const void*, const SlDlssOptions*
);

constexpr std::uint32_t sl_tag_depth = 0U;
constexpr std::uint32_t sl_tag_motion_vectors = 1U;
constexpr std::uint32_t sl_tag_scaling_input = 3U;
constexpr std::uint32_t sl_tag_scaling_output = 4U;
constexpr std::size_t sl_tag_capacity = 73U;
constexpr std::uint32_t sl_dlss_mode_dlaa = 6U;
constexpr std::uint32_t peripheral_streamline_view_mask = 0x40000000U;

std::atomic<GetProcAddressFn> real_get_proc_address{};
std::atomic<InitD3D11Fn> real_init_d3d11{};
std::atomic<InitD3D11Fn> real_core_init_d3d11{};
std::atomic<NgxD3D12InitFn> real_init_d3d12{};
std::atomic<NgxD3D12InitFn> real_core_init_d3d12{};
std::atomic<NgxD3D12Shutdown1Fn> real_shutdown_d3d12_1{};
std::atomic<NgxD3D12Shutdown1Fn> real_core_shutdown_d3d12_1{};
std::atomic<CreateD3D11Fn> real_create_d3d11{};
std::atomic<CreateD3D11Fn> real_core_create_d3d11{};
std::atomic<EvaluateD3D11Fn> real_evaluate_d3d11{};
std::atomic<EvaluateD3D11CFn> real_evaluate_d3d11_c{};
std::atomic<ReleaseD3D11Fn> real_release_d3d11{};
std::atomic<ReleaseD3D11Fn> real_core_release_d3d11{};
std::atomic<CreateD3D12Fn> real_create_d3d12{};
std::atomic<CreateD3D12Fn> real_core_create_d3d12{};
std::atomic<EvaluateD3D12Fn> real_evaluate_d3d12{};
std::atomic<EvaluateD3D12Fn> real_core_evaluate_d3d12{};
std::atomic<EvaluateD3D12CFn> real_evaluate_d3d12_c{};
std::atomic<ReleaseD3D12Fn> real_release_d3d12{};
std::atomic<ReleaseD3D12Fn> real_core_release_d3d12{};
std::atomic<SlEvaluateFeatureFn> real_sl_evaluate_feature{};
std::atomic<SlSetTagFn> real_sl_set_tag{};
std::atomic<SlSetTagForFrameFn> real_sl_set_tag_for_frame{};
std::atomic<SlSetConstantsFn> real_sl_set_constants{};
std::atomic<SlGetFeatureFunctionFn> real_sl_get_feature_function{};
std::atomic<SlDlssSetOptionsFn> real_sl_dlss_set_options{};

struct D3D12GameView {
    const NgxHandle* handle{};
    std::uint32_t feature{1U};
};

std::mutex d3d12_game_views_mutex;
std::deque<D3D12GameView> d3d12_game_views;

struct CachedSlTag {
    bool present{};
    SlResource resource{};
    SlResourceTag tag{};
};

SRWLOCK streamline_lock = SRWLOCK_INIT;
std::array<CachedSlTag, sl_tag_capacity> cached_sl_tags{};
SlViewportHandle cached_sl_viewport{};
bool has_cached_sl_viewport{};
bool cached_sl_frame_tagging{};
SlConstants cached_sl_constants{};
bool has_cached_sl_constants{};
SlDlssOptions cached_sl_options{};
SlViewportHandle cached_sl_options_viewport{};
bool has_cached_sl_options{};
std::atomic<std::uint32_t> applied_sl_output_width{};
std::atomic<std::uint32_t> applied_sl_output_height{};
std::atomic<bool> streamline_foveation_active{};
std::atomic<std::uint32_t> captured_d3d12_create_flags{};
std::atomic<bool> captured_d3d12_create_flags_valid{};
thread_local bool inside_streamline_evaluation{};

struct StreamlineEvaluationScope {
    bool previous{};
    StreamlineEvaluationScope() noexcept
        : previous(inside_streamline_evaluation) {
        inside_streamline_evaluation = true;
    }
    ~StreamlineEvaluationScope() {
        inside_streamline_evaluation = previous;
    }
};

struct InlineHook {
    std::byte* target{};
    std::array<std::byte, 14U> saved{};
    std::array<std::byte, 14U> patch{};
    bool active{};
};

CRITICAL_SECTION streamline_hook_lock{};
CRITICAL_SECTION streamline_evaluation_lock{};
std::atomic<bool> streamline_hook_lock_ready{};
std::atomic<bool> streamline_inline_mode{};
std::atomic<bool> streamline_inline_install_failed{};
InlineHook inline_sl_evaluate{};
InlineHook inline_sl_set_tag{};
InlineHook inline_sl_set_tag_for_frame{};
InlineHook inline_sl_set_constants{};
InlineHook inline_sl_get_feature_function{};

std::atomic<std::uint64_t> hook_debug_sequence{};

std::atomic<PVOID> hook_debug_veh{};
wchar_t hook_debug_crash_log_path[MAX_PATH]{};

struct HookDebugUnicodeString {
    USHORT length{};
    USHORT maximum_length{};
    PWSTR buffer{};
};

struct HookDebugLdrDllNotificationEntry {
    ULONG flags{};
    const HookDebugUnicodeString* full_dll_name{};
    const HookDebugUnicodeString* base_dll_name{};
    PVOID dll_base{};
    ULONG size_of_image{};
};

union HookDebugLdrDllNotificationData {
    HookDebugLdrDllNotificationEntry loaded;
    HookDebugLdrDllNotificationEntry unloaded;
};

using HookDebugDllNotificationCallback = VOID (NTAPI*)(
    ULONG,
    const HookDebugLdrDllNotificationData*,
    PVOID
);
using HookDebugLdrRegisterDllNotificationFn = LONG (NTAPI*)(
    ULONG,
    HookDebugDllNotificationCallback,
    PVOID,
    PVOID*
);
using HookDebugLdrUnregisterDllNotificationFn = LONG (NTAPI*)(PVOID);

struct HookDebugLoaderEvent {
    std::atomic<bool> ready{};
    std::uint64_t sequence{};
    ULONG reason{};
    PVOID dll_base{};
    ULONG size_of_image{};
    wchar_t base_name[160]{};
};

constexpr std::size_t hook_debug_loader_event_capacity = 128U;
std::array<HookDebugLoaderEvent, hook_debug_loader_event_capacity>
    hook_debug_loader_events{};
std::atomic<std::uint64_t> hook_debug_loader_event_sequence{};
std::atomic<PVOID> hook_debug_loader_cookie{};

void initialize_hook_debug_crash_log_path() noexcept {
    hook_debug_crash_log_path[0] = L'\0';
    const auto length = GetTempPathW(
        static_cast<DWORD>(std::size(hook_debug_crash_log_path)),
        hook_debug_crash_log_path
    );
    constexpr wchar_t filename[] = L"CheekyFoveatedDLSS_crash.log";
    if (length == 0U || length >= std::size(hook_debug_crash_log_path)) {
        std::memcpy(
            hook_debug_crash_log_path,
            filename,
            sizeof(filename)
        );
        return;
    }
    const auto used = static_cast<std::size_t>(length);
    constexpr auto filename_chars = std::size(filename);
    if (used + filename_chars <= std::size(hook_debug_crash_log_path)) {
        std::memcpy(
            hook_debug_crash_log_path + used,
            filename,
            sizeof(filename)
        );
    }
}

void hook_debug_emergency_logf(const char* const format, ...) noexcept {
    char buffer[1024]{};
    va_list arguments;
    va_start(arguments, format);
    const auto count = std::vsnprintf(
        buffer,
        sizeof(buffer) - 3U,
        format,
        arguments
    );
    va_end(arguments);
    std::size_t length{};
    if (count < 0) {
        length = std::strlen(buffer);
    } else {
        length = (std::min)(
            static_cast<std::size_t>(count),
            sizeof(buffer) - 3U
        );
    }
    if (length == 0U) return;
    if (buffer[length - 1U] != '\n') buffer[length++] = '\r', buffer[length++] = '\n';
    buffer[length] = '\0';
    OutputDebugStringA(buffer);
    if (hook_debug_crash_log_path[0] == L'\0') return;
    const auto file = CreateFileW(
        hook_debug_crash_log_path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written{};
    static_cast<void>(WriteFile(
        file,
        buffer,
        static_cast<DWORD>(length),
        &written,
        nullptr
    ));
    CloseHandle(file);
}

[[nodiscard]] bool hook_debug_interesting_exception(
    const DWORD code
) noexcept {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_STACK_OVERFLOW:
            return true;
        default:
            return false;
    }
}

LONG CALLBACK hook_debug_exception_handler(
    EXCEPTION_POINTERS* const pointers
) noexcept {
    if (pointers == nullptr || pointers->ExceptionRecord == nullptr ||
        pointers->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto* const record = pointers->ExceptionRecord;
    if (!hook_debug_interesting_exception(record->ExceptionCode)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto* const context = pointers->ContextRecord;
    void* instruction{};
    std::uintptr_t stack_pointer{};
    std::uintptr_t frame_pointer{};
#if defined(_M_X64) || defined(__x86_64__)
    instruction = reinterpret_cast<void*>(context->Rip);
    stack_pointer = static_cast<std::uintptr_t>(context->Rsp);
    frame_pointer = static_cast<std::uintptr_t>(context->Rbp);
#elif defined(_M_IX86) || defined(__i386__)
    instruction = reinterpret_cast<void*>(context->Eip);
    stack_pointer = static_cast<std::uintptr_t>(context->Esp);
    frame_pointer = static_cast<std::uintptr_t>(context->Ebp);
#endif

    MEMORY_BASIC_INFORMATION memory{};
    const auto queried = instruction != nullptr
        ? VirtualQuery(instruction, &memory, sizeof(memory))
        : 0U;
    hook_debug_emergency_logf(
        "HOOKDBG EXCEPTION first_chance code=0x%08lX flags=0x%08lX tid=%lu address=%p rip=%p rsp=%p rbp=%p allocBase=%p regionBase=%p protect=0x%08lX state=0x%08lX",
        static_cast<unsigned long>(record->ExceptionCode),
        static_cast<unsigned long>(record->ExceptionFlags),
        static_cast<unsigned long>(GetCurrentThreadId()),
        record->ExceptionAddress,
        instruction,
        reinterpret_cast<void*>(stack_pointer),
        reinterpret_cast<void*>(frame_pointer),
        queried != 0U ? memory.AllocationBase : nullptr,
        queried != 0U ? memory.BaseAddress : nullptr,
        queried != 0U ? static_cast<unsigned long>(memory.Protect) : 0UL,
        queried != 0U ? static_cast<unsigned long>(memory.State) : 0UL
    );
    if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        record->NumberParameters >= 2U) {
        hook_debug_emergency_logf(
            "HOOKDBG EXCEPTION memory operation=%llu target=%p extra=0x%llX",
            static_cast<unsigned long long>(record->ExceptionInformation[0]),
            reinterpret_cast<void*>(record->ExceptionInformation[1]),
            record->NumberParameters >= 3U
                ? static_cast<unsigned long long>(record->ExceptionInformation[2])
                : 0ULL
        );
    }
    for (const auto& event : hook_debug_loader_events) {
        if (!event.ready.load(std::memory_order_acquire)) continue;
        hook_debug_emergency_logf(
            "HOOKDBG EXCEPTION pending-loader-event seq=%llu action=%s name=%ls base=%p size=%lu",
            static_cast<unsigned long long>(event.sequence),
            event.reason == 1U ? "LOAD" : "UNLOAD",
            event.base_name[0] != L'\0' ? event.base_name : L"<unknown>",
            event.dll_base,
            static_cast<unsigned long>(event.size_of_image)
        );
    }

    if (record->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
        void* frames[16]{};
        const auto count = CaptureStackBackTrace(
            0U,
            static_cast<DWORD>(std::size(frames)),
            frames,
            nullptr
        );
        for (USHORT index{}; index < count; ++index) {
            hook_debug_emergency_logf(
                "HOOKDBG EXCEPTION handler-stack[%u]=%p",
                static_cast<unsigned int>(index),
                frames[index]
            );
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

VOID NTAPI hook_debug_loader_notification(
    const ULONG reason,
    const HookDebugLdrDllNotificationData* const data,
    PVOID
) noexcept {
    if (data == nullptr || (reason != 1U && reason != 2U)) return;
    const auto sequence = hook_debug_loader_event_sequence.fetch_add(
        1U,
        std::memory_order_relaxed
    );
    auto& event = hook_debug_loader_events[
        sequence % hook_debug_loader_events.size()
    ];
    event.ready.store(false, std::memory_order_release);
    const auto& entry = reason == 1U ? data->loaded : data->unloaded;
    event.sequence = sequence;
    event.reason = reason;
    event.dll_base = entry.dll_base;
    event.size_of_image = entry.size_of_image;
    event.base_name[0] = L'\0';
    if (entry.base_dll_name != nullptr &&
        entry.base_dll_name->buffer != nullptr) {
        const auto characters = (std::min)(
            static_cast<std::size_t>(entry.base_dll_name->length / sizeof(wchar_t)),
            std::size(event.base_name) - 1U
        );
        std::memcpy(
            event.base_name,
            entry.base_dll_name->buffer,
            characters * sizeof(wchar_t)
        );
        event.base_name[characters] = L'\0';
    }
    event.ready.store(true, std::memory_order_release);
}

void drain_hook_debug_loader_events() noexcept {
    for (auto& event : hook_debug_loader_events) {
        if (!event.ready.exchange(false, std::memory_order_acq_rel)) continue;
        trace_event(
            "HOOKDBG DLL %s seq=%llu name=%ls base=%p size=%lu",
            event.reason == 1U ? "LOAD" : "UNLOAD",
            static_cast<unsigned long long>(event.sequence),
            event.base_name[0] != L'\0' ? event.base_name : L"<unknown>",
            event.dll_base,
            static_cast<unsigned long>(event.size_of_image)
        );
    }
}

void install_hook_debug_diagnostics() noexcept {
    initialize_hook_debug_crash_log_path();
    const auto veh = AddVectoredExceptionHandler(
        1UL,
        &hook_debug_exception_handler
    );
    hook_debug_veh.store(veh, std::memory_order_release);
    trace_event(
        "HOOKDBG crash diagnostics VEH=%p emergency_log=%ls",
        veh,
        hook_debug_crash_log_path[0] != L'\0'
            ? hook_debug_crash_log_path
            : L"<disabled>"
    );

    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto register_notification = ntdll != nullptr
        ? reinterpret_cast<HookDebugLdrRegisterDllNotificationFn>(
            GetProcAddress(ntdll, "LdrRegisterDllNotification")
        )
        : nullptr;
    if (register_notification == nullptr) {
        trace_event("HOOKDBG DLL notification registration unavailable");
        return;
    }
    PVOID cookie{};
    const auto status = register_notification(
        0U,
        &hook_debug_loader_notification,
        nullptr,
        &cookie
    );
    if (status >= 0) {
        hook_debug_loader_cookie.store(cookie, std::memory_order_release);
    }
    trace_event(
        "HOOKDBG DLL notification register status=0x%08lX cookie=%p",
        static_cast<unsigned long>(status),
        cookie
    );
}

void uninstall_hook_debug_diagnostics() noexcept {
    const auto cookie = hook_debug_loader_cookie.exchange(
        nullptr,
        std::memory_order_acq_rel
    );
    if (cookie != nullptr) {
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto unregister_notification = ntdll != nullptr
            ? reinterpret_cast<HookDebugLdrUnregisterDllNotificationFn>(
                GetProcAddress(ntdll, "LdrUnregisterDllNotification")
            )
            : nullptr;
        if (unregister_notification != nullptr) {
            const auto status = unregister_notification(cookie);
            trace_event(
                "HOOKDBG DLL notification unregister status=0x%08lX cookie=%p",
                static_cast<unsigned long>(status),
                cookie
            );
        }
    }
    drain_hook_debug_loader_events();
    const auto veh = hook_debug_veh.exchange(nullptr, std::memory_order_acq_rel);
    if (veh != nullptr) {
        static_cast<void>(RemoveVectoredExceptionHandler(veh));
        trace_event("HOOKDBG crash diagnostics VEH removed=%p", veh);
    }
}

void trace_pointer_context(const char* const label, const void* const pointer) noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    wchar_t module_path[MAX_PATH]{};
    HMODULE module{};
    const auto query = pointer != nullptr
        ? VirtualQuery(pointer, &memory, sizeof(memory))
        : 0U;
    if (pointer != nullptr) {
        static_cast<void>(GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(pointer),
            &module
        ));
        if (module != nullptr) {
            static_cast<void>(GetModuleFileNameW(
                module, module_path, static_cast<DWORD>(MAX_PATH)
            ));
        }
    }
    trace_event(
        "HOOKDBG ptr label=%s ptr=%p module=%p path=%ls vq=%zu base=%p size=%zu state=0x%08X protect=0x%08X type=0x%08X",
        label != nullptr ? label : "?",
        pointer,
        module,
        module_path[0] != L'\0' ? module_path : L"<unknown>",
        static_cast<std::size_t>(query),
        query != 0U ? memory.BaseAddress : nullptr,
        query != 0U ? static_cast<std::size_t>(memory.RegionSize) : 0U,
        query != 0U ? memory.State : 0U,
        query != 0U ? memory.Protect : 0U,
        query != 0U ? memory.Type : 0U
    );
    const auto pointer_address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto region_base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const auto region_end = region_base + static_cast<std::uintptr_t>(memory.RegionSize);
    const bool readable_bytes = query != 0U && memory.State == MEM_COMMIT &&
        pointer != nullptr && (memory.Protect & PAGE_GUARD) == 0U &&
        (memory.Protect & PAGE_NOACCESS) == 0U && pointer_address <= region_end &&
        region_end - pointer_address >= 16U;
    if (readable_bytes) {
        const auto* const bytes = static_cast<const unsigned char*>(pointer);
        trace_event(
            "HOOKDBG bytes label=%s ptr=%p %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            label != nullptr ? label : "?", pointer,
            bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
            bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]
        );
    }
}

[[nodiscard]] bool write_inline_code(
    void* const destination,
    const void* const source,
    const std::size_t size
) noexcept {
    const auto seq = hook_debug_sequence.fetch_add(1U, std::memory_order_relaxed);
    trace_event(
        "HOOKDBG write[%llu] begin dst=%p src=%p size=%zu tid=%lu",
        static_cast<unsigned long long>(seq), destination, source, size,
        static_cast<unsigned long>(GetCurrentThreadId())
    );
    DWORD old_protection{};
    if (!VirtualProtect(
            destination,
            size,
            PAGE_EXECUTE_READWRITE,
            &old_protection
        )) {
        trace_event(
            "HOOKDBG write[%llu] VirtualProtect RWX FAILED err=%lu",
            static_cast<unsigned long long>(seq),
            static_cast<unsigned long>(GetLastError())
        );
        return false;
    }
    trace_event(
        "HOOKDBG write[%llu] RWX ok oldProtect=0x%08X memcpy begin",
        static_cast<unsigned long long>(seq), old_protection
    );
    std::memcpy(destination, source, size);
    trace_event(
        "HOOKDBG write[%llu] memcpy end restore-protect begin",
        static_cast<unsigned long long>(seq)
    );
    DWORD ignored{};
    const auto restored = VirtualProtect(
        destination,
        size,
        old_protection,
        &ignored
    );
    trace_event(
        "HOOKDBG write[%llu] restore-protect=%s err=%lu flush begin",
        static_cast<unsigned long long>(seq), restored ? "yes" : "NO",
        restored ? 0UL : static_cast<unsigned long>(GetLastError())
    );
    const auto flushed = FlushInstructionCache(
        GetCurrentProcess(),
        destination,
        size
    );
    trace_event(
        "HOOKDBG write[%llu] end flush=%s err=%lu",
        static_cast<unsigned long long>(seq), flushed ? "yes" : "NO",
        flushed ? 0UL : static_cast<unsigned long>(GetLastError())
    );
    return true;
}

[[nodiscard]] bool install_inline_hook(
    InlineHook& hook,
    void* const target,
    void* const detour
) noexcept {
    trace_event("HOOKDBG install-inline begin hook=%p target=%p detour=%p active=%s tid=%lu", &hook, target, detour, hook.active ? "yes" : "no", static_cast<unsigned long>(GetCurrentThreadId()));
    if (target == nullptr || detour == nullptr || hook.active) {
        trace_event("HOOKDBG install-inline rejected hook=%p target=%p detour=%p active=%s", &hook, target, detour, hook.active ? "yes" : "no");
        return false;
    }
    trace_pointer_context("inline-target-before", target);
    hook.target = static_cast<std::byte*>(target);
    std::memcpy(hook.saved.data(), hook.target, hook.saved.size());
    hook.patch[0U] = std::byte{0xFF};
    hook.patch[1U] = std::byte{0x25};
    hook.patch[2U] = std::byte{};
    hook.patch[3U] = std::byte{};
    hook.patch[4U] = std::byte{};
    hook.patch[5U] = std::byte{};
    std::memcpy(
        hook.patch.data() + 6U,
        &detour,
        sizeof(detour)
    );
    // Publish a callable hook state before the entry point becomes reachable.
    // A thread entering immediately after the write can then forward safely.
    hook.active = true;
    if (!write_inline_code(
            hook.target,
            hook.patch.data(),
            hook.patch.size()
        )) {
        trace_event("HOOKDBG install-inline WRITE FAILED hook=%p target=%p", &hook, hook.target);
        hook.active = false;
        hook.target = nullptr;
        return false;
    }
    trace_pointer_context("inline-target-after", hook.target);
    trace_event("HOOKDBG install-inline success hook=%p target=%p detour=%p", &hook, hook.target, detour);
    return true;
}

void remove_inline_hook(InlineHook& hook) noexcept {
    if (hook.active && hook.target != nullptr) {
        static_cast<void>(write_inline_code(
            hook.target,
            hook.saved.data(),
            hook.saved.size()
        ));
    }
}

void restore_inline_hook(InlineHook& hook) noexcept {
    if (hook.active && hook.target != nullptr) {
        static_cast<void>(write_inline_code(
            hook.target,
            hook.patch.data(),
            hook.patch.size()
        ));
    }
}

template <typename Function, typename... Arguments>
auto forward_inline(
    const char* const name,
    InlineHook& hook,
    Arguments... arguments
) noexcept -> decltype(reinterpret_cast<Function>(hook.target)(arguments...)) {
    const auto seq = hook_debug_sequence.fetch_add(1U, std::memory_order_relaxed);
    trace_event(
        "HOOKDBG forward[%llu] %s ENTER hook=%p target=%p active=%s tid=%lu",
        static_cast<unsigned long long>(seq), name, &hook, hook.target,
        hook.active ? "yes" : "no", static_cast<unsigned long>(GetCurrentThreadId())
    );
    trace_event("HOOKDBG forward[%llu] %s wait-lock", static_cast<unsigned long long>(seq), name);
    EnterCriticalSection(&streamline_hook_lock);
    trace_event("HOOKDBG forward[%llu] %s got-lock remove begin", static_cast<unsigned long long>(seq), name);
    remove_inline_hook(hook);
    trace_event("HOOKDBG forward[%llu] %s remove end ORIGINAL CALL BEGIN target=%p", static_cast<unsigned long long>(seq), name, hook.target);
    const auto result = reinterpret_cast<Function>(hook.target)(arguments...);
    trace_event("HOOKDBG forward[%llu] %s ORIGINAL CALL END result=0x%08X restore begin", static_cast<unsigned long long>(seq), name, static_cast<unsigned int>(result));
    restore_inline_hook(hook);
    trace_event("HOOKDBG forward[%llu] %s restore end unlock", static_cast<unsigned long long>(seq), name);
    LeaveCriticalSection(&streamline_hook_lock);
    trace_event("HOOKDBG forward[%llu] %s EXIT", static_cast<unsigned long long>(seq), name);
    return result;
}

std::uint32_t forward_sl_evaluate_feature(
    const std::uint32_t feature,
    const void* const frame,
    const void* const* const inputs,
    const std::uint32_t input_count,
    void* const command_buffer
) {
    return forward_inline<SlEvaluateFeatureFn>(
        "slEvaluateFeature",
        inline_sl_evaluate,
        feature,
        frame,
        inputs,
        input_count,
        command_buffer
    );
}

std::uint32_t forward_sl_set_tag(
    const void* const viewport,
    const void* const tags,
    const std::uint32_t count,
    void* const command_buffer
) {
    return forward_inline<SlSetTagFn>(
        "slSetTag",
        inline_sl_set_tag,
        viewport,
        tags,
        count,
        command_buffer
    );
}

std::uint32_t forward_sl_set_tag_for_frame(
    const void* const frame,
    const void* const viewport,
    const void* const tags,
    const std::uint32_t count,
    void* const command_buffer
) {
    return forward_inline<SlSetTagForFrameFn>(
        "slSetTagForFrame",
        inline_sl_set_tag_for_frame,
        frame,
        viewport,
        tags,
        count,
        command_buffer
    );
}

std::uint32_t forward_sl_set_constants(
    const void* const values,
    const void* const frame,
    const void* const viewport
) {
    return forward_inline<SlSetConstantsFn>(
        "slSetConstants",
        inline_sl_set_constants,
        values,
        frame,
        viewport
    );
}

std::uint32_t forward_sl_get_feature_function(
    const std::uint32_t feature,
    const char* const name,
    void** const function
) {
    return forward_inline<SlGetFeatureFunctionFn>(
        "slGetFeatureFunction",
        inline_sl_get_feature_function,
        feature,
        name,
        function
    );
}

struct PatchedSlot {
    void** slot{};
    void* original{};
};

constexpr std::size_t maximum_patched_slots = 8192U;
std::array<PatchedSlot, maximum_patched_slots> patched_slots{};
std::size_t patched_slot_count{};
SRWLOCK patch_lock = SRWLOCK_INIT;
std::atomic<HANDLE> stop_event{};
std::atomic<HANDLE> worker_thread{};
std::atomic<bool> started{};
std::atomic<bool> minhook_initialized{};
std::atomic<bool> early_loader_interception{};
constexpr std::size_t maximum_direct_hooks = 32U;
std::array<void*, maximum_direct_hooks> direct_hook_targets{};
std::size_t direct_hook_count{};
SRWLOCK direct_hook_lock = SRWLOCK_INIT;

// Late NGX runtimes can be transient during game startup. Do not patch a
// newly observed runtime from the worker until the same HMODULE survives three
// consecutive scans. Runtimes present during initial add-on startup are
// admitted immediately.
struct RuntimeStability {
    HMODULE module{};
    std::uint32_t consecutive_scans{};
    bool admitted{};
};

constexpr std::uint32_t runtime_stability_required_scans = 3U;
RuntimeStability public_runtime_stability{};
RuntimeStability core_runtime_stability{};
SRWLOCK runtime_stability_lock = SRWLOCK_INIT;

constexpr std::size_t d3d11_dlss_timing_slot_count = 4U;

enum class D3D11DlssTimingKind {
    foveated,
    native,
};

struct D3D11DlssTimingSlot {
    ID3D11Query* disjoint{};
    ID3D11Query* begin{};
    ID3D11Query* end{};
    D3D11DlssTimingKind kind{D3D11DlssTimingKind::native};
    bool pending{};
    bool recording{};
};

struct D3D11DlssTimer {
    ID3D11DeviceContext* context{};
    std::array<D3D11DlssTimingSlot, d3d11_dlss_timing_slot_count> slots{};
    std::size_t next_slot{};
};

std::mutex d3d11_dlss_timing_mutex;
std::deque<D3D11DlssTimer> d3d11_dlss_timers;

void release_d3d11_dlss_timing_slot(
    D3D11DlssTimingSlot& slot
) noexcept {
    if (slot.end != nullptr) slot.end->Release();
    if (slot.begin != nullptr) slot.begin->Release();
    if (slot.disjoint != nullptr) slot.disjoint->Release();
    slot = {};
}

void release_d3d11_dlss_timers() noexcept {
    std::lock_guard lock(d3d11_dlss_timing_mutex);
    for (auto& timer : d3d11_dlss_timers) {
        for (auto& slot : timer.slots) {
            release_d3d11_dlss_timing_slot(slot);
        }
        if (timer.context != nullptr) timer.context->Release();
    }
    d3d11_dlss_timers.clear();
}

void resolve_d3d11_dlss_timing(
    ID3D11DeviceContext* const context,
    D3D11DlssTimingSlot& slot
) noexcept {
    if (!slot.pending) return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    std::uint64_t begin{};
    std::uint64_t end{};
    const auto disjoint_result = context->GetData(
        slot.disjoint, &disjoint, sizeof(disjoint),
        D3D11_ASYNC_GETDATA_DONOTFLUSH
    );
    const auto begin_result = context->GetData(
        slot.begin, &begin, sizeof(begin),
        D3D11_ASYNC_GETDATA_DONOTFLUSH
    );
    const auto end_result = context->GetData(
        slot.end, &end, sizeof(end),
        D3D11_ASYNC_GETDATA_DONOTFLUSH
    );
    if (disjoint_result == S_FALSE || begin_result == S_FALSE ||
        end_result == S_FALSE) {
        return;
    }
    slot.pending = false;
    if (FAILED(disjoint_result) || FAILED(begin_result) || FAILED(end_result) ||
        disjoint.Disjoint || disjoint.Frequency == 0U || end < begin) {
        return;
    }
    const auto milliseconds = static_cast<float>(
        static_cast<double>(end - begin) * 1000.0 /
        static_cast<double>(disjoint.Frequency)
    );
    if (slot.kind == D3D11DlssTimingKind::foveated) {
        diagnostic_note_foveated_dlss_gpu_time(milliseconds);
    } else {
        diagnostic_note_native_dlss_gpu_time(milliseconds);
    }
}

[[nodiscard]] D3D11DlssTimer* find_or_create_d3d11_dlss_timer(
    ID3D11DeviceContext* const context
) noexcept {
    for (auto& timer : d3d11_dlss_timers) {
        if (timer.context == context) return &timer;
    }
    ID3D11Device* device{};
    context->GetDevice(&device);
    if (device == nullptr) return nullptr;
    D3D11DlssTimer created{};
    created.context = context;
    created.context->AddRef();
    const D3D11_QUERY_DESC disjoint_desc{
        D3D11_QUERY_TIMESTAMP_DISJOINT, 0U
    };
    const D3D11_QUERY_DESC timestamp_desc{D3D11_QUERY_TIMESTAMP, 0U};
    HRESULT result = S_OK;
    for (auto& slot : created.slots) {
        result = device->CreateQuery(&disjoint_desc, &slot.disjoint);
        if (SUCCEEDED(result)) {
            result = device->CreateQuery(&timestamp_desc, &slot.begin);
        }
        if (SUCCEEDED(result)) {
            result = device->CreateQuery(&timestamp_desc, &slot.end);
        }
        if (FAILED(result)) break;
    }
    device->Release();
    if (FAILED(result)) {
        for (auto& slot : created.slots) {
            release_d3d11_dlss_timing_slot(slot);
        }
        created.context->Release();
        return nullptr;
    }
    d3d11_dlss_timers.push_back(created);
    return &d3d11_dlss_timers.back();
}

struct D3D11DlssTimingScope {
    ID3D11DeviceContext* context{};
    D3D11DlssTimingSlot* slot{};
    D3D11DlssTimingKind kind{D3D11DlssTimingKind::native};

    D3D11DlssTimingScope(
        ID3D11DeviceContext* const in_context,
        const D3D11DlssTimingKind in_kind
    ) noexcept : context(in_context), kind(in_kind) {
        if (context == nullptr ||
            context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
            return;
        }
        const auto metric = kind == D3D11DlssTimingKind::foveated
            ? DiagnosticGpuTiming::foveated_dlss
            : DiagnosticGpuTiming::native_dlss;
        if (!diagnostic_should_sample_gpu_time(metric)) return;
        std::lock_guard lock(d3d11_dlss_timing_mutex);
        auto* const timer = find_or_create_d3d11_dlss_timer(context);
        if (timer == nullptr) return;
        for (std::size_t offset{}; offset < timer->slots.size(); ++offset) {
            const auto index = (timer->next_slot + offset) % timer->slots.size();
            auto& candidate = timer->slots[index];
            resolve_d3d11_dlss_timing(context, candidate);
            if (candidate.pending || candidate.recording) continue;
            timer->next_slot = (index + 1U) % timer->slots.size();
            candidate.recording = true;
            candidate.kind = kind;
            context->Begin(candidate.disjoint);
            context->End(candidate.begin);
            slot = &candidate;
            break;
        }
    }

    void finish() noexcept {
        if (context == nullptr || slot == nullptr) return;
        std::lock_guard lock(d3d11_dlss_timing_mutex);
        context->End(slot->end);
        context->End(slot->disjoint);
        slot->recording = false;
        slot->pending = true;
        slot = nullptr;
    }

    ~D3D11DlssTimingScope() { finish(); }
};

constexpr std::size_t d3d12_nr_timing_slot_count = 6U;

enum class D3D12TimingKind : std::uint32_t {
    full_nr,
    foveated_nr,
    peripheral_dlaa,
    foveated_dlss,
    native_dlss,
};

struct D3D12NrTimingSlot {
    ID3D12Fence* fence{};
    ID3D12GraphicsCommandList* command_list{};
    ID3D12CommandQueue* queue{};
    std::uint64_t next_fence_value{};
    std::uint64_t fence_value{};
    std::uint64_t timestamp_frequency{};
    D3D12TimingKind kind{D3D12TimingKind::full_nr};
    bool publish{};
    bool pending{};
    bool recording{};
};

struct D3D12NrTimer {
    ID3D12Device* device{};
    ID3D12QueryHeap* query_heap{};
    ID3D12Resource* readback{};
    std::array<D3D12NrTimingSlot, d3d12_nr_timing_slot_count> slots{};
    std::size_t next_slot{};
};

std::mutex d3d12_nr_timing_mutex;
std::deque<D3D12NrTimer> d3d12_nr_timers;

void release_d3d12_nr_timing_slot(D3D12NrTimingSlot& slot) noexcept {
    if (slot.queue != nullptr) slot.queue->Release();
    if (slot.command_list != nullptr) slot.command_list->Release();
    if (slot.fence != nullptr) slot.fence->Release();
    slot = {};
}

void release_d3d12_nr_timers() noexcept {
    std::lock_guard lock(d3d12_nr_timing_mutex);
    for (auto& timer : d3d12_nr_timers) {
        for (auto& slot : timer.slots) release_d3d12_nr_timing_slot(slot);
        if (timer.readback != nullptr) timer.readback->Release();
        if (timer.query_heap != nullptr) timer.query_heap->Release();
        if (timer.device != nullptr) timer.device->Release();
    }
    d3d12_nr_timers.clear();
}

void resolve_d3d12_nr_timing(
    D3D12NrTimer& timer,
    D3D12NrTimingSlot& slot,
    const std::size_t slot_index
) noexcept {
    if (!slot.pending || slot.fence_value == 0U ||
        slot.fence == nullptr ||
        slot.fence->GetCompletedValue() < slot.fence_value) {
        return;
    }
    const auto byte_offset = sizeof(std::uint64_t) * 2U * slot_index;
    const D3D12_RANGE read_range{
        byte_offset,
        byte_offset + sizeof(std::uint64_t) * 2U
    };
    void* mapped{};
    if (SUCCEEDED(timer.readback->Map(0U, &read_range, &mapped)) &&
        mapped != nullptr) {
        const auto* const timestamps = reinterpret_cast<const std::uint64_t*>(
            static_cast<const std::byte*>(mapped) + byte_offset
        );
        const auto begin = timestamps[0U];
        const auto end = timestamps[1U];
        const D3D12_RANGE written_range{0U, 0U};
        timer.readback->Unmap(0U, &written_range);
        if (slot.publish && slot.timestamp_frequency != 0U && end >= begin) {
            const auto milliseconds = static_cast<float>(
                static_cast<double>(end - begin) * 1000.0 /
                static_cast<double>(slot.timestamp_frequency)
            );
            if (slot.kind == D3D12TimingKind::peripheral_dlaa) {
                diagnostic_note_peripheral_dlaa_gpu_time(
                    DiagnosticApi::d3d12, milliseconds
                );
            } else if (slot.kind == D3D12TimingKind::foveated_dlss ||
                       slot.kind == D3D12TimingKind::native_dlss) {
                diagnostic_note_d3d12_dlss_gpu_time(
                    milliseconds, slot.kind == D3D12TimingKind::foveated_dlss
                );
            } else {
                diagnostic_note_dlss_nr_gpu_time(
                    DiagnosticApi::d3d12,
                    milliseconds,
                    slot.kind == D3D12TimingKind::foveated_nr
                );
            }
        }
    }
    slot.fence_value = 0U;
    slot.timestamp_frequency = 0U;
    slot.publish = false;
    slot.pending = false;
}

[[nodiscard]] D3D12NrTimer* find_or_create_d3d12_nr_timer(
    ID3D12GraphicsCommandList* const command_list
) noexcept {
    ID3D12Device* device{};
    if (command_list == nullptr ||
        FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) ||
        device == nullptr) {
        return nullptr;
    }
    for (auto& timer : d3d12_nr_timers) {
        if (timer.device == device) {
            device->Release();
            return &timer;
        }
    }

    D3D12NrTimer created{};
    created.device = device;
    D3D12_QUERY_HEAP_DESC query_desc{};
    query_desc.Count = static_cast<UINT>(d3d12_nr_timing_slot_count * 2U);
    query_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    auto result = device->CreateQueryHeap(
        &query_desc, IID_PPV_ARGS(&created.query_heap)
    );

    D3D12_HEAP_PROPERTIES heap_properties{};
    heap_properties.Type = D3D12_HEAP_TYPE_READBACK;
    heap_properties.CreationNodeMask = 1U;
    heap_properties.VisibleNodeMask = 1U;
    D3D12_RESOURCE_DESC resource_desc{};
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Width = sizeof(std::uint64_t) *
        d3d12_nr_timing_slot_count * 2U;
    resource_desc.Height = 1U;
    resource_desc.DepthOrArraySize = 1U;
    resource_desc.MipLevels = 1U;
    resource_desc.SampleDesc.Count = 1U;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (SUCCEEDED(result)) {
        result = device->CreateCommittedResource(
            &heap_properties,
            D3D12_HEAP_FLAG_NONE,
            &resource_desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&created.readback)
        );
    }
    if (SUCCEEDED(result)) {
        for (auto& slot : created.slots) {
            result = device->CreateFence(
                0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence)
            );
            if (FAILED(result)) break;
        }
    }
    if (FAILED(result)) {
        for (auto& slot : created.slots) release_d3d12_nr_timing_slot(slot);
        if (created.readback != nullptr) created.readback->Release();
        if (created.query_heap != nullptr) created.query_heap->Release();
        created.device->Release();
        return nullptr;
    }
    d3d12_nr_timers.push_back(created);
    return &d3d12_nr_timers.back();
}

struct D3D12NrTimingScope {
    ID3D12GraphicsCommandList* command_list{};
    D3D12NrTimer* timer{};
    D3D12NrTimingSlot* slot{};
    std::size_t slot_index{};
    bool foveated{};

    D3D12NrTimingScope(
        ID3D12GraphicsCommandList* const in_command_list,
        const bool in_foveated
    ) noexcept : command_list(in_command_list), foveated(in_foveated) {
        const auto metric = foveated
            ? DiagnosticGpuTiming::d3d12_foveated_dlss_nr
            : DiagnosticGpuTiming::d3d12_full_dlss_nr;
        if (command_list == nullptr ||
            !diagnostic_should_sample_gpu_time(metric)) {
            return;
        }
        std::lock_guard lock(d3d12_nr_timing_mutex);
        timer = find_or_create_d3d12_nr_timer(command_list);
        if (timer == nullptr) return;
        for (std::size_t index{}; index < timer->slots.size(); ++index) {
            resolve_d3d12_nr_timing(*timer, timer->slots[index], index);
        }
        for (std::size_t offset{}; offset < timer->slots.size(); ++offset) {
            const auto index = (timer->next_slot + offset) % timer->slots.size();
            auto& candidate = timer->slots[index];
            if (candidate.pending || candidate.recording) continue;
            timer->next_slot = (index + 1U) % timer->slots.size();
            candidate.recording = true;
            candidate.kind = foveated
                ? D3D12TimingKind::foveated_nr
                : D3D12TimingKind::full_nr;
            slot = &candidate;
            slot_index = index;
            command_list->EndQuery(
                timer->query_heap,
                D3D12_QUERY_TYPE_TIMESTAMP,
                static_cast<UINT>(index * 2U)
            );
            break;
        }
    }

    void finish(const bool succeeded) noexcept {
        if (command_list == nullptr || timer == nullptr || slot == nullptr) return;
        std::lock_guard lock(d3d12_nr_timing_mutex);
        command_list->EndQuery(
            timer->query_heap,
            D3D12_QUERY_TYPE_TIMESTAMP,
            static_cast<UINT>(slot_index * 2U + 1U)
        );
        command_list->ResolveQueryData(
            timer->query_heap,
            D3D12_QUERY_TYPE_TIMESTAMP,
            static_cast<UINT>(slot_index * 2U),
            2U,
            timer->readback,
            sizeof(std::uint64_t) * 2U * slot_index
        );
        command_list->AddRef();
        slot->command_list = command_list;
        slot->publish = succeeded;
        slot->recording = false;
        slot->pending = true;
        slot = nullptr;
    }

    ~D3D12NrTimingScope() { finish(false); }
};

struct D3D12PeripheralTimingScope {
    ID3D12GraphicsCommandList* command_list{};
    D3D12NrTimer* timer{};
    D3D12NrTimingSlot* slot{};
    std::size_t slot_index{};
    D3D12BackendTiming backend_timing{};
    bool manual_begin{};

    explicit D3D12PeripheralTimingScope(
        ID3D12GraphicsCommandList* const in_command_list,
        const D3D12TimingKind kind = D3D12TimingKind::peripheral_dlaa
    ) noexcept : command_list(in_command_list) {
        if (command_list == nullptr ||
            !diagnostic_should_sample_gpu_time(
                kind == D3D12TimingKind::foveated_dlss
                    ? DiagnosticGpuTiming::d3d12_foveated_dlss
                    : kind == D3D12TimingKind::native_dlss
                        ? DiagnosticGpuTiming::d3d12_native_dlss
                        : DiagnosticGpuTiming::d3d12_peripheral_dlaa
            )) {
            return;
        }
        std::lock_guard lock(d3d12_nr_timing_mutex);
        timer = find_or_create_d3d12_nr_timer(command_list);
        if (timer == nullptr) return;
        for (std::size_t index{}; index < timer->slots.size(); ++index) {
            resolve_d3d12_nr_timing(*timer, timer->slots[index], index);
        }
        for (std::size_t offset{}; offset < timer->slots.size(); ++offset) {
            const auto index = (timer->next_slot + offset) % timer->slots.size();
            auto& candidate = timer->slots[index];
            if (candidate.pending || candidate.recording) continue;
            timer->next_slot = (index + 1U) % timer->slots.size();
            candidate.recording = true;
            candidate.kind = kind;
            slot = &candidate;
            slot_index = index;
            backend_timing.query_heap = timer->query_heap;
            backend_timing.begin_query_index =
                static_cast<std::uint32_t>(index * 2U);
            backend_timing.end_query_index =
                static_cast<std::uint32_t>(index * 2U + 1U);
            backend_timing.write_begin_timestamp = true;
            break;
        }
    }

    [[nodiscard]] D3D12BackendTiming* backend() noexcept {
        return slot == nullptr ? nullptr : &backend_timing;
    }

    void begin() noexcept {
        if (command_list == nullptr || timer == nullptr || slot == nullptr ||
            manual_begin || backend_timing.sr_timestamp_written) {
            return;
        }
        command_list->EndQuery(
            timer->query_heap,
            D3D12_QUERY_TYPE_TIMESTAMP,
            backend_timing.begin_query_index
        );
        manual_begin = true;
    }

    void finish(const bool succeeded) noexcept {
        if (command_list == nullptr || timer == nullptr || slot == nullptr) {
            return;
        }
        std::lock_guard lock(d3d12_nr_timing_mutex);
        const bool backend_recorded = backend_timing.sr_timestamp_written;
        if (manual_begin && !backend_recorded) {
            command_list->EndQuery(
                timer->query_heap,
                D3D12_QUERY_TYPE_TIMESTAMP,
                backend_timing.end_query_index
            );
        }
        if (!manual_begin && !backend_recorded) {
            slot->recording = false;
            slot = nullptr;
            return;
        }
        command_list->ResolveQueryData(
            timer->query_heap,
            D3D12_QUERY_TYPE_TIMESTAMP,
            backend_timing.begin_query_index,
            2U,
            timer->readback,
            sizeof(std::uint64_t) * 2U * slot_index
        );
        command_list->AddRef();
        slot->command_list = command_list;
        slot->publish = succeeded;
        slot->recording = false;
        slot->pending = true;
        slot = nullptr;
    }

    ~D3D12PeripheralTimingScope() { finish(false); }
};

void note_d3d12_command_list_submission_impl(
    ID3D12CommandQueue* const queue,
    ID3D12GraphicsCommandList* const command_list
) noexcept {
    if (queue == nullptr || command_list == nullptr) return;
    note_peripheral_dlaa_submission(queue, command_list);
    std::uint64_t frequency{};
    if (FAILED(queue->GetTimestampFrequency(&frequency)) || frequency == 0U) return;
    std::lock_guard lock(d3d12_nr_timing_mutex);
    for (auto& timer : d3d12_nr_timers) {
        for (auto& slot : timer.slots) {
            if (!slot.pending || slot.command_list != command_list ||
                slot.queue != nullptr) {
                continue;
            }
            queue->AddRef();
            slot.queue = queue;
            slot.timestamp_frequency = frequency;
            slot.command_list->Release();
            slot.command_list = nullptr;
        }
    }
}

void note_d3d12_present_impl(
    ID3D12CommandQueue* const present_queue
) noexcept {
    collect_peripheral_dlaa_resources();
    std::uint64_t present_frequency{};
    if (present_queue != nullptr) {
        static_cast<void>(present_queue->GetTimestampFrequency(&present_frequency));
    }
    std::lock_guard lock(d3d12_nr_timing_mutex);
    for (auto& timer : d3d12_nr_timers) {
        for (std::size_t index{}; index < timer.slots.size(); ++index) {
            resolve_d3d12_nr_timing(timer, timer.slots[index], index);
        }
        for (auto& slot : timer.slots) {
            if (!slot.pending || slot.fence_value != 0U) {
                continue;
            }
            ID3D12CommandQueue* signal_queue = slot.queue;
            if (signal_queue == nullptr && present_queue != nullptr &&
                present_frequency != 0U) {
                signal_queue = present_queue;
                slot.timestamp_frequency = present_frequency;
                if (slot.command_list != nullptr) {
                    slot.command_list->Release();
                    slot.command_list = nullptr;
                }
            }
            if (signal_queue == nullptr) continue;
            const auto value = ++slot.next_fence_value;
            if (SUCCEEDED(signal_queue->Signal(slot.fence, value))) {
                slot.fence_value = value;
                if (slot.queue != nullptr) {
                    slot.queue->Release();
                    slot.queue = nullptr;
                }
            }
        }
    }
}

[[nodiscard]] bool streamline_loaded() noexcept {
    static std::atomic<bool> detected{};
    if (detected.load(std::memory_order_acquire)) {
        return true;
    }
    constexpr const wchar_t* modules[] = {
        L"sl.interposer.dll",
        L"sl.common.dll",
        L"sl.dlss.dll",
        L"sl.dlss_g.dll",
        L"sl.dlss_g_v2.dll",
    };
    for (const auto* const module : modules) {
        if (GetModuleHandleW(module) != nullptr) {
            detected.store(true, std::memory_order_release);
            diagnostic_note_streamline_detected();
            trace_event("Streamline detected module=%ls", module);
            return true;
        }
    }
    return false;
}

void cache_streamline_tags(
    const void* const viewport,
    const void* const tags,
    const std::uint32_t count,
    const bool frame_tagging
) noexcept {
    if (viewport == nullptr || tags == nullptr) return;
    const auto* const typed = static_cast<const SlResourceTag*>(tags);
    AcquireSRWLockExclusive(&streamline_lock);
    cached_sl_viewport = *static_cast<const SlViewportHandle*>(viewport);
    cached_sl_viewport.next = nullptr;
    has_cached_sl_viewport = true;
    cached_sl_frame_tagging = frame_tagging;
    for (std::uint32_t index{}; index < count; ++index) {
        const auto& source = typed[index];
        if (source.type >= cached_sl_tags.size() || source.resource == nullptr) {
            continue;
        }
        auto& destination = cached_sl_tags[source.type];
        destination.present = true;
        destination.resource = *source.resource;
        destination.resource.next = nullptr;
        destination.tag = source;
        destination.tag.next = nullptr;
        destination.tag.resource = &destination.resource;
    }
    ReleaseSRWLockExclusive(&streamline_lock);
}

[[nodiscard]] std::uint32_t resource_width(
    const SlResourceTag& tag
) noexcept {
    if (tag.extent.width != 0U) return tag.extent.width;
    if (tag.resource == nullptr || tag.resource->native == nullptr) return 0U;
    if (tag.resource->width != 0U) return tag.resource->width;
    return static_cast<std::uint32_t>(
        static_cast<ID3D12Resource*>(tag.resource->native)->GetDesc().Width
    );
}

[[nodiscard]] std::uint32_t resource_height(
    const SlResourceTag& tag
) noexcept {
    if (tag.extent.height != 0U) return tag.extent.height;
    if (tag.resource == nullptr || tag.resource->native == nullptr) return 0U;
    if (tag.resource->height != 0U) return tag.resource->height;
    return static_cast<ID3D12Resource*>(
        tag.resource->native
    )->GetDesc().Height;
}

[[nodiscard]] std::uint32_t native_resource_width(
    const SlResourceTag& tag
) noexcept {
    if (tag.resource == nullptr || tag.resource->native == nullptr) return 0U;
    const auto desc = static_cast<ID3D12Resource*>(tag.resource->native)->GetDesc();
    return static_cast<std::uint32_t>(desc.Width);
}

[[nodiscard]] std::uint32_t native_resource_height(
    const SlResourceTag& tag
) noexcept {
    if (tag.resource == nullptr || tag.resource->native == nullptr) return 0U;
    return static_cast<std::uint32_t>(
        static_cast<ID3D12Resource*>(tag.resource->native)->GetDesc().Height
    );
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

[[nodiscard]] bool apply_streamline_options(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t preset
) noexcept {
    const auto original = real_sl_dlss_set_options.load(
        std::memory_order_acquire
    );
    if (original == nullptr || width == 0U || height == 0U) {
        static std::atomic<bool> logged_invalid{};
        if (!logged_invalid.exchange(true, std::memory_order_relaxed)) {
            trace_event(
                "SL options apply unavailable original=%p requested=%ux%u",
                reinterpret_cast<void*>(original), width, height
            );
        }
        return false;
    }

    SlDlssOptions options{};
    SlViewportHandle viewport{};
    AcquireSRWLockShared(&streamline_lock);
    const bool available = has_cached_sl_options;
    if (available) {
        options = cached_sl_options;
        viewport = cached_sl_options_viewport;
    }
    ReleaseSRWLockShared(&streamline_lock);
    if (!available) {
        static std::atomic<bool> logged_missing{};
        if (!logged_missing.exchange(true, std::memory_order_relaxed)) {
            trace_event(
                "SL options cache MISSING: slDLSSSetOptions wrapper has not observed the game's options yet; requested=%ux%u target=%p",
                width, height, reinterpret_cast<void*>(original)
            );
        }
        return false;
    }

    options.next = nullptr;
    viewport.next = nullptr;
    const auto original_width = options.output_width;
    const auto original_height = options.output_height;
    options.output_width = width;
    options.output_height = height;
    if (preset != 0U) {
        options.dlaa_preset = preset;
        options.quality_preset = preset;
        options.balanced_preset = preset;
        options.performance_preset = preset;
        options.ultra_performance_preset = preset;
        options.ultra_quality_preset = preset;
    }
    const auto result = original(&viewport, &options);

    static std::atomic<bool> apply_log_initialized{};
    static std::atomic<std::uint32_t> logged_apply_mode{0xFFFFFFFFU};
    static std::atomic<std::uint32_t> logged_apply_original_width{};
    static std::atomic<std::uint32_t> logged_apply_original_height{};
    static std::atomic<std::uint32_t> logged_apply_width{};
    static std::atomic<std::uint32_t> logged_apply_height{};
    static std::atomic<std::uint32_t> logged_apply_preset{0xFFFFFFFFU};
    const auto previous_apply_preset = logged_apply_preset.exchange(preset);
    const bool first_apply_state =
        !apply_log_initialized.exchange(true, std::memory_order_relaxed);
    const auto previous_apply_mode = logged_apply_mode.exchange(
        options.mode,
        std::memory_order_relaxed
    );
    const auto previous_apply_original_width = logged_apply_original_width.exchange(
        original_width,
        std::memory_order_relaxed
    );
    const auto previous_apply_original_height = logged_apply_original_height.exchange(
        original_height,
        std::memory_order_relaxed
    );
    const auto previous_apply_width = logged_apply_width.exchange(
        width,
        std::memory_order_relaxed
    );
    const auto previous_apply_height = logged_apply_height.exchange(
        height,
        std::memory_order_relaxed
    );
    if (result != 0U || first_apply_state || previous_apply_preset != preset ||
        previous_apply_mode != options.mode ||
        previous_apply_original_width != original_width ||
        previous_apply_original_height != original_height ||
        previous_apply_width != width ||
        previous_apply_height != height) {
        trace_event(
            "SL options apply viewport=%u mode=%u original=%ux%u requested=%ux%u preset=%u result=0x%08X",
            viewport.value,
            options.mode,
            original_width,
            original_height,
            width,
            height,
            preset,
            result
        );
    }
    if (result == 0U) {
        applied_sl_output_width.store(width, std::memory_order_release);
        applied_sl_output_height.store(height, std::memory_order_release);
    }
    return result == 0U;
}

void restore_streamline_options() noexcept {
    if (applied_sl_output_width.load(std::memory_order_acquire) == 0U) return;
    const auto original = real_sl_dlss_set_options.load(
        std::memory_order_acquire
    );
    SlDlssOptions options{};
    SlViewportHandle viewport{};
    AcquireSRWLockShared(&streamline_lock);
    const bool available = has_cached_sl_options;
    if (available) {
        options = cached_sl_options;
        viewport = cached_sl_options_viewport;
    }
    ReleaseSRWLockShared(&streamline_lock);
    if (original != nullptr && available) {
        options.next = nullptr;
        viewport.next = nullptr;
        const auto preset = current_settings().center_preset;
        if (preset != 0U) {
            options.dlaa_preset = preset;
            options.quality_preset = preset;
            options.balanced_preset = preset;
            options.performance_preset = preset;
            options.ultra_performance_preset = preset;
            options.ultra_quality_preset = preset;
        }
        static_cast<void>(original(&viewport, &options));
    }
    applied_sl_output_width.store(0U, std::memory_order_release);
    applied_sl_output_height.store(0U, std::memory_order_release);
}

struct StreamlineEvaluation {
    D3D12Evaluation* backend{};
    std::array<SlResource, 4U> resources{};
    std::array<SlResourceTag, 4U> tags{};
    SlViewportHandle viewport{};
    SlConstants original_constants{};
    SlConstants nr_constants{};
    Settings settings{};
    PeripheralDlaaResources peripheral{};
    bool constants_overridden{};
    bool motion_vectors_output_space{};
    bool has_nr_constants{};
    bool peripheral_ready{};
    bool frame_tagging{};
};

// Export availability does not indicate the tagging mode enabled by the game.
[[nodiscard]] std::uint32_t submit_streamline_tags(
    const bool frame_tagging,
    const void* const frame,
    const SlViewportHandle& viewport,
    const SlResourceTag* const tags,
    const std::uint32_t count,
    ID3D12GraphicsCommandList* const command_list
) noexcept {
    if (frame_tagging) {
        const auto submit = real_sl_set_tag_for_frame.load(std::memory_order_acquire);
        return submit != nullptr && frame != nullptr
            ? submit(frame, &viewport, tags, count, command_list) : 0x18U;
    }
    const auto submit = real_sl_set_tag.load(std::memory_order_acquire);
    return submit != nullptr
        ? submit(&viewport, tags, count, command_list) : 0x18U;
}

[[nodiscard]] bool evaluate_streamline_peripheral_dlaa(
    ID3D12GraphicsCommandList* const command_list,
    const void* const frame,
    StreamlineEvaluation& evaluation,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const bool verbose,
    const std::uint64_t sequence
) noexcept {
    if (!evaluation.settings.peripheral_dlaa_enabled ||
        evaluation.backend == nullptr || command_list == nullptr ||
        frame == nullptr || render_width == 0U || render_height == 0U) {
        return false;
    }

    const auto evaluate = real_sl_evaluate_feature.load(
        std::memory_order_acquire
    );
    const auto set_options = real_sl_dlss_set_options.load(
        std::memory_order_acquire
    );
    const auto set_for_frame = real_sl_set_tag_for_frame.load(
        std::memory_order_acquire
    );
    const auto set_tag = real_sl_set_tag.load(std::memory_order_acquire);
    const auto set_constants = real_sl_set_constants.load(
        std::memory_order_acquire
    );
    if (evaluate == nullptr || set_options == nullptr ||
        (set_for_frame == nullptr && set_tag == nullptr) ||
        set_constants == nullptr) {
        return false;
    }

    SlDlssOptions options{};
    SlConstants constants{};
    bool have_options{};
    bool have_constants{};
    AcquireSRWLockShared(&streamline_lock);
    if (has_cached_sl_options) {
        options = cached_sl_options;
        have_options = true;
    }
    if (has_cached_sl_constants) {
        constants = cached_sl_constants;
        have_constants = true;
    }
    ReleaseSRWLockShared(&streamline_lock);
    if (!have_options || !have_constants) return false;

    const auto& color_tag = evaluation.tags[0U];
    const auto& depth_tag = evaluation.tags[1U];
    const auto& motion_tag = evaluation.tags[2U];
    const auto& output_tag = evaluation.tags[3U];

    PeripheralDlaaRequest request{};
    request.view_id = static_cast<DlssViewId>(evaluation.viewport.value) + 1U;
    request.command_list = command_list;
    request.color = static_cast<ID3D12Resource*>(color_tag.resource->native);
    request.depth = static_cast<ID3D12Resource*>(depth_tag.resource->native);
    request.motion_vectors =
        static_cast<ID3D12Resource*>(motion_tag.resource->native);
    request.output_template =
        static_cast<ID3D12Resource*>(output_tag.resource->native);
    request.render_width = render_width;
    request.render_height = render_height;
    request.source_output_width = output_width;
    request.source_output_height = output_height;
    request.scale = evaluation.settings.peripheral_dlaa_scale;
    request.preset = evaluation.settings.peripheral_dlaa_preset;
    request.color_base_x = color_tag.extent.left;
    request.color_base_y = color_tag.extent.top;
    request.depth_base_x = depth_tag.extent.left;
    request.depth_base_y = depth_tag.extent.top;
    request.mv_base_x = motion_tag.extent.left;
    request.mv_base_y = motion_tag.extent.top;
    request.motion_vectors_output_space =
        evaluation.motion_vectors_output_space;
    if (color_tag.resource->state != 0xFFFFFFFFU) {
        request.color_state = static_cast<D3D12_RESOURCE_STATES>(
            color_tag.resource->state
        );
    }
    if (depth_tag.resource->state != 0xFFFFFFFFU) {
        request.depth_state = static_cast<D3D12_RESOURCE_STATES>(
            depth_tag.resource->state
        );
    }
    if (motion_tag.resource->state != 0xFFFFFFFFU) {
        request.motion_state = static_cast<D3D12_RESOURCE_STATES>(
            motion_tag.resource->state
        );
    }

    if (!prepare_peripheral_dlaa_resources(request, evaluation.peripheral)) {
        return false;
    }
    const auto working_width = evaluation.peripheral.working_width;
    const auto working_height = evaluation.peripheral.working_height;

    const auto cleanup = [&]() noexcept {
        finish_peripheral_dlaa_motion_read(
            command_list,
            evaluation.peripheral
        );
        evaluation.peripheral = {};
    };

    auto peripheral_viewport = evaluation.viewport;
    peripheral_viewport.next = nullptr;
    peripheral_viewport.value ^= peripheral_streamline_view_mask;

    options.next = nullptr;
    options.mode = sl_dlss_mode_dlaa;
    options.output_width = working_width;
    options.output_height = working_height;
    options.dlaa_preset = evaluation.settings.peripheral_dlaa_preset;
    if (set_options(&peripheral_viewport, &options) != 0U) {
        cleanup();
        return false;
    }

    auto resources = evaluation.resources;
    auto tags = evaluation.tags;
    for (std::size_t index{}; index < tags.size(); ++index) {
        resources[index].next = nullptr;
        tags[index].next = nullptr;
        tags[index].resource = &resources[index];
    }

    if (evaluation.peripheral.downsampled_color) {
        resources[0U].native = evaluation.peripheral.color;
        resources[0U].memory = nullptr;
        resources[0U].view = nullptr;
        resources[0U].state =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        resources[0U].width = working_width;
        resources[0U].height = working_height;
        tags[0U].resource = &resources[0U];
        tags[0U].extent = {0U, 0U, working_width, working_height};
    }
    if (evaluation.peripheral.downsampled_depth) {
        resources[1U].native = evaluation.peripheral.depth;
        resources[1U].memory = nullptr;
        resources[1U].view = nullptr;
        resources[1U].state =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        resources[1U].width = working_width;
        resources[1U].height = working_height;
        tags[1U].resource = &resources[1U];
        tags[1U].extent = {0U, 0U, working_width, working_height};
    }
    if (evaluation.peripheral.converted_motion) {
        resources[2U].native = evaluation.peripheral.motion_vectors;
        resources[2U].memory = nullptr;
        resources[2U].view = nullptr;
        resources[2U].state =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        resources[2U].width = working_width;
        resources[2U].height = working_height;
        tags[2U].resource = &resources[2U];
        tags[2U].extent = {0U, 0U, working_width, working_height};
    }

    resources[3U].native = evaluation.peripheral.output;
    resources[3U].memory = nullptr;
    resources[3U].view = nullptr;
    resources[3U].state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    resources[3U].width = working_width;
    resources[3U].height = working_height;
    tags[3U].resource = &resources[3U];
    tags[3U].extent = {0U, 0U, working_width, working_height};

    const auto tag_result = submit_streamline_tags(
        evaluation.frame_tagging, frame, peripheral_viewport, tags.data(),
        static_cast<std::uint32_t>(tags.size()), command_list
    );
    if (tag_result != 0U) {
        if (verbose) trace_event("SL eval=%llu peripheral DLAA tag submission failed result=0x%08X", static_cast<unsigned long long>(sequence), tag_result);
        cleanup();
        return false;
    }

    // Streamline's mvecScale normalizes vector values. Point-downsampling the
    // MV texture does not change those values, so keep the game's scale here.
    // Jitter, however, is in pixel space, so scale it to the peripheral grid.
    constants.next = nullptr;
    constants.jitter_offset.x *= static_cast<float>(working_width) /
        static_cast<float>(render_width);
    constants.jitter_offset.y *= static_cast<float>(working_height) /
        static_cast<float>(render_height);
    if (set_constants(&constants, frame, &peripheral_viewport) != 0U) {
        cleanup();
        return false;
    }

    const void* peripheral_inputs[]{&peripheral_viewport};
    std::uint32_t peripheral_result{};
    D3D12PeripheralTimingScope peripheral_timing{command_list};
    peripheral_timing.begin();
    {
        StreamlineEvaluationScope scope;
        peripheral_result = evaluate(
            0U,
            frame,
            peripheral_inputs,
            1U,
            command_list
        );
    }
    peripheral_timing.finish(peripheral_result == 0U);

    finish_peripheral_dlaa_motion_read(
        command_list,
        evaluation.peripheral
    );
    if (peripheral_result != 0U) {
        evaluation.peripheral = {};
        if (verbose) {
            trace_event(
                "SL eval=%llu peripheral DLAA failed result=0x%08X",
                static_cast<unsigned long long>(sequence),
                peripheral_result
            );
        }
        return false;
    }

    finish_peripheral_dlaa_write(command_list, evaluation.peripheral);
    if (!d3d12_set_composite_base(
            evaluation.backend,
            evaluation.peripheral.output,
            0U,
            0U
        )) {
        restore_peripheral_dlaa_output(
            command_list,
            evaluation.peripheral
        );
        evaluation.peripheral = {};
        return false;
    }

    evaluation.peripheral_ready = true;
    if (verbose) {
        trace_event(
            "SL eval=%llu peripheral DLAA ready size=%ux%u scale=%.2f convertedMV=%s",
            static_cast<unsigned long long>(sequence),
            working_width,
            working_height,
            evaluation.settings.peripheral_dlaa_scale,
            evaluation.peripheral.converted_motion ? "yes" : "no"
        );
    }
    return true;
}

[[nodiscard]] bool prepare_streamline_evaluation(
    ID3D12GraphicsCommandList* const command_list,
    const void* const frame,
    StreamlineEvaluation& evaluation,
    const bool verbose,
    const std::uint64_t sequence
) noexcept {
    constexpr std::array<std::uint32_t, 4U> required{
        sl_tag_scaling_input,
        sl_tag_depth,
        sl_tag_motion_vectors,
        sl_tag_scaling_output,
    };
    bool cached{};
    AcquireSRWLockShared(&streamline_lock);
    if (has_cached_sl_viewport) {
        cached = true;
        evaluation.viewport = cached_sl_viewport;
        evaluation.frame_tagging = cached_sl_frame_tagging;
        for (std::size_t index{}; index < required.size(); ++index) {
            const auto& source = cached_sl_tags[required[index]];
            if (!source.present || source.resource.native == nullptr) {
                cached = false;
                break;
            }
            evaluation.resources[index] = source.resource;
            evaluation.tags[index] = source.tag;
            evaluation.resources[index].next = nullptr;
            evaluation.tags[index].next = nullptr;
            evaluation.tags[index].resource = &evaluation.resources[index];
        }
    }
    ReleaseSRWLockShared(&streamline_lock);
    if (verbose) {
        trace_event(
            "SL eval=%llu prepare cache=%s command_list=%p frame=%p",
            static_cast<unsigned long long>(sequence),
            cached ? "ready" : "missing",
            command_list,
            frame
        );
    }
    if (!cached || command_list == nullptr) {
        diagnostic_note_state(
            DiagnosticApi::d3d12,
            DiagnosticState::missing_resources
        );
        return false;
    }

    auto& color_tag = evaluation.tags[0U];
    auto& output_tag = evaluation.tags[3U];
    const auto render_width = resource_width(color_tag);
    const auto render_height = resource_height(color_tag);
    const auto output_width = resource_width(output_tag);
    const auto output_height = resource_height(output_tag);
    if (verbose) {
        trace_event(
            "SL eval=%llu resources color=%p output=%p input=%ux%u@%u,%u output=%ux%u@%u,%u",
            static_cast<unsigned long long>(sequence),
            color_tag.resource->native,
            output_tag.resource->native,
            render_width,
            render_height,
            color_tag.extent.left,
            color_tag.extent.top,
            output_width,
            output_height,
            output_tag.extent.left,
            output_tag.extent.top
        );
    }
    diagnostic_note_evaluate(
        DiagnosticApi::d3d12,
        render_width,
        render_height,
        output_width,
        output_height
    );

    const auto streamline_view_id = static_cast<DlssViewId>(
        evaluation.viewport.value
    ) + 1U;
    register_stereo_view(streamline_view_id);
    const auto streamline_settings = settings_for_view(
        current_settings(),
        streamline_view_id
    );
    evaluation.settings = streamline_settings;
    evaluation.backend = prepare_d3d12_streamline(
        command_list,
        static_cast<ID3D12Resource*>(color_tag.resource->native),
        static_cast<ID3D12Resource*>(output_tag.resource->native),
        render_width,
        render_height,
        output_width,
        output_height,
        color_tag.extent.left,
        color_tag.extent.top,
        output_tag.extent.left,
        output_tag.extent.top,
        streamline_view_id,
        current_settings(),
        verbose,
        sequence
    );
    if (evaluation.backend == nullptr) {
        if (verbose) trace_event("SL eval=%llu backend prepare rejected", static_cast<unsigned long long>(sequence));
        return false;
    }

    const auto crop = d3d12_evaluation_crop(evaluation.backend);
    note_stereo_view_geometry(
        streamline_view_id,
        render_width,
        render_height,
        output_width,
        output_height,
        crop
    );
    if (verbose) {
        trace_event(
            "SL eval=%llu backend ready scratch=%p crop input=%ux%u@%u,%u output=%ux%u@%u,%u",
            static_cast<unsigned long long>(sequence),
            d3d12_private_output(evaluation.backend),
            crop.input_width,
            crop.input_height,
            crop.input_base_x,
            crop.input_base_y,
            crop.output_width,
            crop.output_height,
            crop.output_base_x,
            crop.output_base_y
        );
    }
    // Match the Hogwarts/native NGX fix: infer MV coordinate space from the
    // actual motion-vector texture dimensions rather than assuming low-res MVs.
    // The tag extent is still the region we crop *within*; native dimensions are
    // used only to decide whether the MV field lives in input or output space.
    const auto color_native_width = native_resource_width(evaluation.tags[0U]);
    const auto color_native_height = native_resource_height(evaluation.tags[0U]);
    const auto mv_native_width = native_resource_width(evaluation.tags[2U]);
    const auto mv_native_height = native_resource_height(evaluation.tags[2U]);
    const auto output_native_width = native_resource_width(evaluation.tags[3U]);
    const auto output_native_height = native_resource_height(evaluation.tags[3U]);

    const auto mv_to_input = dimension_distance(
        mv_native_width, mv_native_height,
        color_native_width != 0U ? color_native_width : render_width,
        color_native_height != 0U ? color_native_height : render_height
    );
    const auto mv_to_output = dimension_distance(
        mv_native_width, mv_native_height,
        output_native_width != 0U ? output_native_width : output_width,
        output_native_height != 0U ? output_native_height : output_height
    );
    evaluation.motion_vectors_output_space =
        mv_native_width != 0U && mv_native_height != 0U && mv_to_output < mv_to_input;

    diagnostic_note_motion_vectors(
        DiagnosticApi::d3d12,
        mv_native_width,
        mv_native_height,
        mv_native_width == 0U || mv_native_height == 0U
            ? MotionVectorSpace::unknown
            : evaluation.motion_vectors_output_space
                ? MotionVectorSpace::output
                : MotionVectorSpace::input
    );

    static_cast<void>(evaluate_streamline_peripheral_dlaa(
        command_list,
        frame,
        evaluation,
        render_width,
        render_height,
        output_width,
        output_height,
        verbose,
        sequence
    ));

    // Streamline shares NGX preset parameters across viewports. The peripheral
    // options must not be the last options submitted before the center call.
    if (!apply_streamline_options(
            crop.output_width, crop.output_height,
            streamline_settings.center_preset
        )) {
        if (verbose) trace_event("SL eval=%llu cropped options failed", static_cast<unsigned long long>(sequence));
        finish_d3d12_streamline(command_list, evaluation.backend, false);
        evaluation.backend = nullptr;
        diagnostic_note_state(DiagnosticApi::d3d12, DiagnosticState::prepare_rejected);
        return false;
    }
    if (verbose) trace_event("SL eval=%llu cropped options applied", static_cast<unsigned long long>(sequence));

    if (verbose) {
        const auto& mv_tag = evaluation.tags[2U];
        trace_event(
            "SL MV space=%s native=%ux%u tagExtent=%ux%u@%u,%u colorNative=%ux%u outputNative=%ux%u distanceIn=%llu distanceOut=%llu",
            evaluation.motion_vectors_output_space ? "output" : "input",
            mv_native_width,
            mv_native_height,
            mv_tag.extent.width,
            mv_tag.extent.height,
            mv_tag.extent.left,
            mv_tag.extent.top,
            color_native_width,
            color_native_height,
            output_native_width,
            output_native_height,
            static_cast<unsigned long long>(mv_to_input),
            static_cast<unsigned long long>(mv_to_output)
        );
    }

    for (std::size_t index{}; index < 3U; ++index) {
        auto& tag = evaluation.tags[index];
        const auto width = resource_width(tag);
        const auto height = resource_height(tag);
        if (width == 0U || height == 0U) {
            finish_d3d12_streamline(command_list, evaluation.backend, false);
            evaluation.backend = nullptr;
            return false;
        }

        const bool output_space = index == 2U &&
            evaluation.motion_vectors_output_space;
        const auto reference_width = output_space ? output_width : render_width;
        const auto reference_height = output_space ? output_height : render_height;
        const auto crop_base_x = output_space ? crop.output_base_x : crop.input_base_x;
        const auto crop_base_y = output_space ? crop.output_base_y : crop.input_base_y;
        const auto crop_width = output_space ? crop.output_width : crop.input_width;
        const auto crop_height = output_space ? crop.output_height : crop.input_height;
        if (reference_width == 0U || reference_height == 0U) {
            finish_d3d12_streamline(command_list, evaluation.backend, false);
            evaluation.backend = nullptr;
            return false;
        }

        const auto crop_end_x = crop_base_x + crop_width;
        const auto crop_end_y = crop_base_y + crop_height;
        const auto left = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(crop_base_x) * width / reference_width
        );
        const auto top = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(crop_base_y) * height / reference_height
        );
        const auto right = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(crop_end_x) * width / reference_width
        );
        const auto bottom = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(crop_end_y) * height / reference_height
        );
        tag.extent.left += left;
        tag.extent.top += top;
        tag.extent.width = right - left;
        tag.extent.height = bottom - top;

        if (verbose && index == 2U) {
            trace_event(
                "SL MV crop space=%s ref=%ux%u crop=%ux%u@%u,%u -> tagged=%ux%u@%u,%u",
                output_space ? "output" : "input",
                reference_width,
                reference_height,
                crop_width,
                crop_height,
                crop_base_x,
                crop_base_y,
                tag.extent.width,
                tag.extent.height,
                tag.extent.left,
                tag.extent.top
            );
        }
    }

    auto& output_resource = evaluation.resources[3U];
    output_resource.native = d3d12_private_output(evaluation.backend);
    output_resource.memory = nullptr;
    output_resource.view = nullptr;
    output_resource.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    output_resource.width = output_width;
    output_resource.height = output_height;
    output_tag.resource = &output_resource;
    output_tag.extent = {0U, 0U, crop.output_width, crop.output_height};
    if (verbose) trace_event("SL eval=%llu cropped tags prepared", static_cast<unsigned long long>(sequence));

    const auto tag_result = submit_streamline_tags(
        evaluation.frame_tagging, frame, evaluation.viewport, evaluation.tags.data(),
        static_cast<std::uint32_t>(evaluation.tags.size()), command_list
    );
    if (tag_result != 0U) {
        trace_event(
            "SL eval=%llu cropped tag submission failed result=0x%08X",
            static_cast<unsigned long long>(sequence),
            tag_result
        );
        finish_d3d12_streamline(command_list, evaluation.backend, false);
        evaluation.backend = nullptr;
        diagnostic_note_state(
            DiagnosticApi::d3d12,
            DiagnosticState::prepare_rejected
        );
        return false;
    }
    if (verbose) trace_event("SL eval=%llu cropped tags submitted", static_cast<unsigned long long>(sequence));

    const auto set_constants = real_sl_set_constants.load(
        std::memory_order_acquire
    );
    SlConstants constants{};
    bool has_constants{};
    AcquireSRWLockShared(&streamline_lock);
    if (has_cached_sl_constants) {
        constants = cached_sl_constants;
        has_constants = true;
    }
    ReleaseSRWLockShared(&streamline_lock);
    if (set_constants != nullptr && frame != nullptr && has_constants) {
        auto cropped = constants;
        cropped.next = nullptr;
        if (d3d12_evaluation_gaze_reset(evaluation.backend)) {
            cropped.reset = 1;
        }
        const auto mv_reference_width = evaluation.motion_vectors_output_space
            ? output_width : render_width;
        const auto mv_reference_height = evaluation.motion_vectors_output_space
            ? output_height : render_height;
        const auto mv_crop_width = evaluation.motion_vectors_output_space
            ? crop.output_width : crop.input_width;
        const auto mv_crop_height = evaluation.motion_vectors_output_space
            ? crop.output_height : crop.input_height;
        cropped.motion_vector_scale.x *=
            static_cast<float>(mv_reference_width) / mv_crop_width;
        cropped.motion_vector_scale.y *=
            static_cast<float>(mv_reference_height) / mv_crop_height;
        evaluation.nr_constants = cropped;
        evaluation.has_nr_constants = true;
        if (verbose) {
            trace_event(
                "SL MV constants space=%s scale original=%.6f,%.6f cropped=%.6f,%.6f",
                evaluation.motion_vectors_output_space ? "output" : "input",
                constants.motion_vector_scale.x,
                constants.motion_vector_scale.y,
                cropped.motion_vector_scale.x,
                cropped.motion_vector_scale.y
            );
        }
        if (set_constants(&cropped, frame, &evaluation.viewport) == 0U) {
            evaluation.original_constants = constants;
            evaluation.constants_overridden = true;
            if (verbose) trace_event("SL eval=%llu motion constants applied", static_cast<unsigned long long>(sequence));
        }
    } else if (has_constants) {
        evaluation.nr_constants = constants;
        evaluation.has_nr_constants = true;
    }
    streamline_foveation_active.store(true, std::memory_order_release);
    if (verbose) trace_event("SL eval=%llu prepare complete", static_cast<unsigned long long>(sequence));
    return true;
}

[[nodiscard]] bool prepare_streamline_nr_passthrough(
    StreamlineEvaluation& evaluation
) noexcept {
    constexpr std::array<std::uint32_t, 4U> required{
        sl_tag_scaling_input,
        sl_tag_depth,
        sl_tag_motion_vectors,
        sl_tag_scaling_output,
    };
    AcquireSRWLockShared(&streamline_lock);
    bool available = has_cached_sl_viewport && has_cached_sl_constants;
    if (available) {
        evaluation.viewport = cached_sl_viewport;
        evaluation.nr_constants = cached_sl_constants;
        evaluation.has_nr_constants = true;
        for (std::size_t index{}; index < required.size(); ++index) {
            const auto& source = cached_sl_tags[required[index]];
            if (!source.present || source.resource.native == nullptr) {
                available = false;
                break;
            }
            evaluation.resources[index] = source.resource;
            evaluation.tags[index] = source.tag;
            evaluation.resources[index].next = nullptr;
            evaluation.tags[index].next = nullptr;
            evaluation.tags[index].resource = &evaluation.resources[index];
        }
    }
    ReleaseSRWLockShared(&streamline_lock);
    if (!available) return false;
    const auto view_id = static_cast<DlssViewId>(evaluation.viewport.value) + 1U;
    register_stereo_view(view_id);
    evaluation.settings = settings_for_view(current_settings(), view_id);
    return true;
}

void evaluate_streamline_nr(
    ID3D12GraphicsCommandList* const command_list,
    const StreamlineEvaluation& evaluation,
    const std::uint32_t result
) noexcept {
    if (result != 0U || !evaluation.settings.nr_enabled ||
        command_list == nullptr || !evaluation.has_nr_constants) return;
    if (!captured_d3d12_create_flags_valid.load(std::memory_order_acquire)) {
        return;
    }
    const auto& color = evaluation.tags[0U];
    const auto& depth = evaluation.tags[1U];
    const auto& motion = evaluation.tags[2U];
    const auto& output = evaluation.tags[3U];
    if (color.resource == nullptr || depth.resource == nullptr ||
        motion.resource == nullptr || output.resource == nullptr ||
        color.resource->native == nullptr || depth.resource->native == nullptr ||
        motion.resource->native == nullptr || output.resource->native == nullptr ||
        output.resource->state == 0xFFFFFFFFU) return;
    const auto input_width = resource_width(color);
    const auto input_height = resource_height(color);
    const auto output_width = resource_width(output);
    const auto output_height = resource_height(output);
    const auto depth_width = resource_width(depth);
    const auto depth_height = resource_height(depth);
    const auto motion_width = resource_width(motion);
    const auto motion_height = resource_height(motion);
    const auto view_id = static_cast<DlssViewId>(evaluation.viewport.value) + 1U;
    const DlssNrFrame frame{
        view_id,
        DlssNrRoute::streamline,
        command_list,
        static_cast<ID3D12Resource*>(output.resource->native),
        static_cast<D3D12_RESOURCE_STATES>(output.resource->state),
        static_cast<ID3D12Resource*>(depth.resource->native),
        static_cast<ID3D12Resource*>(motion.resource->native),
        input_width,
        input_height,
        output_width,
        output_height,
        depth.extent.left,
        depth.extent.top,
        depth_width,
        depth_height,
        motion.extent.left,
        motion.extent.top,
        motion_width,
        motion_height,
        evaluation.nr_constants.motion_vector_scale.x,
        evaluation.nr_constants.motion_vector_scale.y,
        evaluation.nr_constants.depth_inverted != 0,
        evaluation.nr_constants.reset != 0,
        captured_d3d12_create_flags.load(std::memory_order_acquire),
        output.extent.left,
        output.extent.top,
        false,
    };
    D3D12NrTimingScope timing{command_list, evaluation.settings.nr_foveated};
    const bool evaluated = evaluate_dlss_nr(frame, evaluation.settings);
    timing.finish(evaluated);
}

std::uint32_t hook_sl_dlss_set_options(
    const void* const viewport,
    const SlDlssOptions* const options
) {
    const auto original = real_sl_dlss_set_options.load(
        std::memory_order_acquire
    );
    if (original == nullptr) return 0x18U;
    if (viewport == nullptr || options == nullptr) {
        return original(viewport, options);
    }
    AcquireSRWLockExclusive(&streamline_lock);
    cached_sl_options = *options;
    cached_sl_options.next = nullptr;
    cached_sl_options_viewport = *static_cast<const SlViewportHandle*>(viewport);
    cached_sl_options_viewport.next = nullptr;
    has_cached_sl_options = true;
    ReleaseSRWLockExclusive(&streamline_lock);

    static std::atomic<bool> logged_state_initialized{};
    static std::atomic<std::uint32_t> logged_state_mode{0xFFFFFFFFU};
    static std::atomic<std::uint32_t> logged_state_width{};
    static std::atomic<std::uint32_t> logged_state_height{};
    const bool first_state =
        !logged_state_initialized.exchange(true, std::memory_order_relaxed);
    const auto previous_mode = logged_state_mode.exchange(
        options->mode,
        std::memory_order_relaxed
    );
    const auto previous_width = logged_state_width.exchange(
        options->output_width,
        std::memory_order_relaxed
    );
    const auto previous_height = logged_state_height.exchange(
        options->output_height,
        std::memory_order_relaxed
    );
    if (first_state || previous_mode != options->mode ||
        previous_width != options->output_width ||
        previous_height != options->output_height) {
        trace_event(
            "slDLSSSetOptions state viewport=%p mode=%u output=%ux%u",
            viewport,
            options->mode,
            options->output_width,
            options->output_height
        );
    }

    auto forwarded = *options;
    const auto width = applied_sl_output_width.load(std::memory_order_acquire);
    const auto height = applied_sl_output_height.load(std::memory_order_acquire);
    if (current_settings().enabled && width != 0U && height != 0U) {
        forwarded.next = nullptr;
        forwarded.output_width = width;
        forwarded.output_height = height;
        return original(viewport, &forwarded);
    }
    return original(viewport, options);
}

std::uint32_t hook_sl_set_tag(
    const void* const viewport,
    const void* const tags,
    const std::uint32_t count,
    void* const command_buffer
) {
    static std::atomic<std::uint32_t> tag_logs{};
    const auto log_index = tag_logs.fetch_add(1U, std::memory_order_relaxed);
    if (log_index < 8U) {
        trace_event(
            "slSetTag enter viewport=%p tags=%p count=%u command=%p",
            viewport,
            tags,
            count,
            command_buffer
        );
    }
    cache_streamline_tags(viewport, tags, count, false);
    if (log_index < 8U) trace_event("slSetTag cache complete index=%u", log_index);
    const auto original = real_sl_set_tag.load(std::memory_order_acquire);
    const auto result = original == nullptr
        ? 0x18U
        : original(viewport, tags, count, command_buffer);
    if (log_index < 4U) {
        trace_event("slSetTag forwarded index=%u result=0x%08X", log_index, result);
    }
    return result;
}

std::uint32_t hook_sl_set_tag_for_frame(
    const void* const frame,
    const void* const viewport,
    const void* const tags,
    const std::uint32_t count,
    void* const command_buffer
) {
    static std::atomic<std::uint32_t> frame_tag_logs{};
    const auto log_index = frame_tag_logs.fetch_add(1U, std::memory_order_relaxed);
    if (log_index < 8U) {
        trace_event(
            "slSetTagForFrame enter frame=%p viewport=%p tags=%p count=%u command=%p",
            frame,
            viewport,
            tags,
            count,
            command_buffer
        );
    }
    cache_streamline_tags(viewport, tags, count, true);
    if (log_index < 8U) trace_event("slSetTagForFrame cache complete index=%u", log_index);
    const auto original = real_sl_set_tag_for_frame.load(
        std::memory_order_acquire
    );
    const auto result = original == nullptr
        ? 0x18U
        : original(frame, viewport, tags, count, command_buffer);
    if (log_index < 4U) {
        trace_event("slSetTagForFrame forwarded index=%u result=0x%08X", log_index, result);
    }
    return result;
}

std::uint32_t hook_sl_set_constants(
    const void* const values,
    const void* const frame,
    const void* const viewport
) {
    static std::atomic<std::uint32_t> constant_entry_logs{};
    if (constant_entry_logs.fetch_add(1U, std::memory_order_relaxed) < 8U) {
        trace_event(
            "slSetConstants enter values=%p frame=%p viewport=%p",
            values,
            frame,
            viewport
        );
    }
    if (values != nullptr) {
        AcquireSRWLockExclusive(&streamline_lock);
        cached_sl_constants = *static_cast<const SlConstants*>(values);
        cached_sl_constants.next = nullptr;
        has_cached_sl_constants = true;
        ReleaseSRWLockExclusive(&streamline_lock);
        static std::atomic<std::uint32_t> constant_logs{};
        if (constant_logs.fetch_add(1U, std::memory_order_relaxed) < 4U) {
            trace_event("slSetConstants captured values=%p frame=%p viewport=%p", values, frame, viewport);
        }
    }
    const auto original = real_sl_set_constants.load(
        std::memory_order_acquire
    );
    const auto result = original == nullptr ? 0x18U : original(values, frame, viewport);
    const auto n = constant_entry_logs.load(std::memory_order_relaxed);
    if (n <= 4U) {
        trace_event("slSetConstants forwarded n=%u result=0x%08X", n, result);
    }
    return result;
}

std::uint32_t hook_sl_get_feature_function(
    const std::uint32_t feature,
    const char* const name,
    void** const function
) {
    const auto original = real_sl_get_feature_function.load(
        std::memory_order_acquire
    );
    static std::atomic<std::uint32_t> get_feature_calls{};
    const auto call = get_feature_calls.fetch_add(1U, std::memory_order_relaxed);
    trace_event("HOOKDBG slGetFeatureFunction[%u] ENTER feature=%u name=%s function_slot=%p original=%p", call, feature, name != nullptr ? name : "<null>", function, reinterpret_cast<void*>(original));
    if (original == nullptr) {
        trace_event("HOOKDBG slGetFeatureFunction[%u] original=NULL", call);
        return 0x18U;
    }
    trace_event("HOOKDBG slGetFeatureFunction[%u] ORIGINAL CALL BEGIN", call);
    const auto result = original(feature, name, function);
    trace_event("HOOKDBG slGetFeatureFunction[%u] ORIGINAL CALL END result=0x%08X returned=%p", call, result, function != nullptr ? *function : nullptr);
    if (function != nullptr && *function != nullptr) trace_pointer_context("slGetFeatureFunction-return", *function);
    if (result == 0U && feature == 0U && name != nullptr &&
        function != nullptr && *function != nullptr &&
        std::strcmp(name, "slDLSSSetOptions") == 0) {
        const auto target = reinterpret_cast<SlDlssSetOptionsFn>(*function);
        if (target != &hook_sl_dlss_set_options) {
            real_sl_dlss_set_options.store(target, std::memory_order_release);
            *function = reinterpret_cast<void*>(&hook_sl_dlss_set_options);
        }
        trace_event(
            "slGetFeatureFunction captured slDLSSSetOptions target=%p returned_wrapper=%p",
            reinterpret_cast<void*>(target),
            reinterpret_cast<void*>(&hook_sl_dlss_set_options)
        );
    }
    trace_event("HOOKDBG slGetFeatureFunction[%u] EXIT result=0x%08X returned=%p", call, result, function != nullptr ? *function : nullptr);
    return result;
}

std::uint32_t hook_sl_evaluate_feature(
    const std::uint32_t feature,
    const void* const frame,
    const void* const* const inputs,
    const std::uint32_t input_count,
    void* const command_buffer
) {
    const auto original = real_sl_evaluate_feature.load(
        std::memory_order_acquire
    );
    static std::atomic<std::uint32_t> eval_entries{};
    const auto eval_entry = eval_entries.fetch_add(1U, std::memory_order_relaxed);
    if (eval_entry < 8U) {
        trace_event(
            "slEvaluateFeature[%u] feature=%u frame=%p count=%u command=%p original=%p",
            eval_entry, feature, frame, input_count, command_buffer,
            reinterpret_cast<void*>(original)
        );
    }
    if (original == nullptr) return 0x18U;
    if (feature != 0U) {
        StreamlineEvaluationScope scope;
        const auto passthrough = original(feature, frame, inputs, input_count, command_buffer);
        if (eval_entry < 4U) {
            trace_event("slEvaluateFeature non-DLSS feature=%u result=0x%08X", feature, passthrough);
        }
        return passthrough;
    }
    EnterCriticalSection(&streamline_evaluation_lock);
    static std::atomic<std::uint64_t> evaluation_sequence{};
    const auto sequence = evaluation_sequence.fetch_add(
        1U,
        std::memory_order_relaxed
    );
    const bool verbose = sequence < 16U;
    if (verbose) {
        trace_event(
            "SL eval=%llu enter frame=%p inputs=%p count=%u command=%p enabled=%s",
            static_cast<unsigned long long>(sequence),
            frame,
            inputs,
            input_count,
            command_buffer,
            current_settings().enabled ? "yes" : "no"
        );
    }
    const auto live_settings = current_settings();
    if (!live_settings.enabled) {
        restore_streamline_options();
        streamline_foveation_active.store(false, std::memory_order_release);
        diagnostic_note_state(DiagnosticApi::d3d12, DiagnosticState::disabled);
        StreamlineEvaluation nr_evaluation{};
        if (live_settings.nr_enabled) {
            static_cast<void>(prepare_streamline_nr_passthrough(nr_evaluation));
        }
        StreamlineEvaluationScope scope;
        D3D12PeripheralTimingScope sr_timing{
            static_cast<ID3D12GraphicsCommandList*>(command_buffer),
            D3D12TimingKind::native_dlss
        };
        sr_timing.begin();
        const auto result = original(
            feature,
            frame,
            inputs,
            input_count,
            command_buffer
        );
        sr_timing.finish(result == 0U);
        evaluate_streamline_nr(
            static_cast<ID3D12GraphicsCommandList*>(command_buffer),
            nr_evaluation,
            result
        );
        LeaveCriticalSection(&streamline_evaluation_lock);
        return result;
    }

    StreamlineEvaluation evaluation{};
    auto* const command_list = static_cast<ID3D12GraphicsCommandList*>(
        command_buffer
    );
    const bool prepared = prepare_streamline_evaluation(
        command_list,
        frame,
        evaluation,
        verbose,
        sequence
    );
    if (!prepared) {
        // Preparation may have changed output dimensions before a later failure.
        // Restore them before forwarding the game's uncropped evaluation.
        restore_streamline_options();
        streamline_foveation_active.store(false, std::memory_order_release);
    }
    D3D12PeripheralTimingScope sr_timing{
        command_list, prepared ? D3D12TimingKind::foveated_dlss
                               : D3D12TimingKind::native_dlss
    };
    sr_timing.begin();
    if (verbose) trace_event("SL eval=%llu original begin foveated=%s", static_cast<unsigned long long>(sequence), evaluation.backend != nullptr ? "yes" : "no");
    std::uint32_t result{};
    {
        StreamlineEvaluationScope scope;
        result = original(
            feature,
            frame,
            inputs,
            input_count,
            command_buffer
        );
    }
    sr_timing.finish(result == 0U);
    if (verbose) trace_event("SL eval=%llu original end result=0x%08X", static_cast<unsigned long long>(sequence), result);
    if (evaluation.constants_overridden) {
        const auto set_constants = real_sl_set_constants.load(
            std::memory_order_acquire
        );
        if (set_constants != nullptr) {
            evaluation.original_constants.next = nullptr;
            static_cast<void>(set_constants(
                &evaluation.original_constants,
                frame,
                &evaluation.viewport
            ));
            if (verbose) trace_event("SL eval=%llu constants restored", static_cast<unsigned long long>(sequence));
        }
    }
    const bool foveated = evaluation.backend != nullptr;
    if (verbose) trace_event("SL eval=%llu composite begin", static_cast<unsigned long long>(sequence));
    finish_d3d12_streamline(
        command_list,
        evaluation.backend,
        result == 0U
    );
    if (evaluation.peripheral_ready) {
        restore_peripheral_dlaa_output(
            command_list,
            evaluation.peripheral
        );
    }
    if (result == 0U && live_settings.nr_enabled) {
        StreamlineEvaluation nr_evaluation{};
        if (prepare_streamline_nr_passthrough(nr_evaluation)) {
            evaluate_streamline_nr(command_list, nr_evaluation, result);
        }
    }
    if (verbose) trace_event("SL eval=%llu composite end", static_cast<unsigned long long>(sequence));
    diagnostic_note_result(DiagnosticApi::d3d12, result);
    if (foveated && result == 0U) {
        diagnostic_note_state(DiagnosticApi::d3d12, DiagnosticState::active);
    } else if (foveated) {
        diagnostic_note_state(
            DiagnosticApi::d3d12,
            DiagnosticState::ngx_evaluation_failed
        );
    }
    LeaveCriticalSection(&streamline_evaluation_lock);
    return result;
}

void note_evaluation_begin(
    const DiagnosticApi api,
    const NgxParameters* const parameters
) noexcept {
    auto input_width = get_ui(
        parameters,
        "DLSS.Render.Subrect.Dimensions.Width"
    );
    auto input_height = get_ui(
        parameters,
        "DLSS.Render.Subrect.Dimensions.Height"
    );
    if (input_width == 0U) input_width = get_ui(parameters, "Width");
    if (input_height == 0U) input_height = get_ui(parameters, "Height");
    diagnostic_note_evaluate(
        api,
        input_width,
        input_height,
        get_ui(parameters, "OutWidth"),
        get_ui(parameters, "OutHeight")
    );
}

[[nodiscard]] bool is_dlss_feature(const std::uint32_t feature) noexcept {
    return feature == 1U || feature == 13U;
}

void remember_d3d12_game_view(
    const NgxHandle* const handle,
    const std::uint32_t feature
) {
    if (handle == nullptr || !is_dlss_feature(feature)) return;
    register_stereo_view(static_cast<DlssViewId>(
        reinterpret_cast<std::uintptr_t>(handle)
    ));
    std::lock_guard lock(d3d12_game_views_mutex);
    for (auto& view : d3d12_game_views) {
        if (view.handle == handle) {
            view.feature = feature;
            return;
        }
    }
    d3d12_game_views.push_back({handle, feature});
}

[[nodiscard]] std::uint32_t d3d12_game_feature(
    const NgxHandle* const handle
) noexcept {
    std::lock_guard lock(d3d12_game_views_mutex);
    for (const auto& view : d3d12_game_views) {
        if (view.handle == handle) return view.feature;
    }
    return 1U;
}

[[nodiscard]] bool has_d3d12_game_view(
    const NgxHandle* const handle
) noexcept {
    if (handle == nullptr) return false;
    std::lock_guard lock(d3d12_game_views_mutex);
    for (const auto& view : d3d12_game_views) {
        if (view.handle == handle) return true;
    }
    return false;
}

void forget_d3d12_game_view(const NgxHandle* const handle) noexcept {
    if (handle == nullptr) return;
    const auto view_id = static_cast<DlssViewId>(
        reinterpret_cast<std::uintptr_t>(handle)
    );
    {
        std::lock_guard lock(d3d12_game_views_mutex);
        for (auto iterator = d3d12_game_views.begin();
             iterator != d3d12_game_views.end(); ++iterator) {
            if (iterator->handle != handle) continue;
            d3d12_game_views.erase(iterator);
            break;
        }
    }
    release_peripheral_dlaa_view(view_id);
    release_d3d12_view(view_id);
    unregister_stereo_view(view_id);
    forget_gaze_view(view_id);
}

void enable_output_subrects(
    const std::uint32_t feature,
    NgxParameters* const parameters
) noexcept {
    if (is_dlss_feature(feature) && parameters != nullptr) {
        parameters->Set("DLSS.Enable.Output.Subrects", 1);
    }
}

NgxResult hook_init_d3d11(
    const unsigned long long application_id,
    const wchar_t* const application_data_path,
    ID3D11Device* const device,
    const void* const feature_common_info,
    const std::uint32_t sdk_version
) {
    const auto original = real_init_d3d11.load(std::memory_order_acquire);
    if (original == nullptr) return 0xBAD00007U;
    const auto result = original(
        application_id, application_data_path, device,
        feature_common_info, sdk_version
    );
    if (ngx_succeeded(result)) {
        remember_d3d11_ngx_init(
            application_id, application_data_path,
            feature_common_info, sdk_version
        );
        trace_event(
            "Captured public D3D11 NGX init app=%llu sdk=%u",
            application_id,
            sdk_version
        );
    }
    return result;
}

NgxResult hook_core_init_d3d11(
    const unsigned long long application_id,
    const wchar_t* const application_data_path,
    ID3D11Device* const device,
    const void* const feature_common_info,
    const std::uint32_t sdk_version
) {
    const auto original = real_core_init_d3d11.load(std::memory_order_acquire);
    if (original == nullptr) return 0xBAD00007U;
    const auto result = original(
        application_id, application_data_path, device,
        feature_common_info, sdk_version
    );
    if (ngx_succeeded(result)) {
        remember_d3d11_ngx_init(
            application_id, application_data_path,
            feature_common_info, sdk_version
        );
        trace_event(
            "Captured core D3D11 NGX init app=%llu sdk=%u",
            application_id,
            sdk_version
        );
    }
    return result;
}

NgxResult hook_init_d3d12(
    const unsigned long long application_id,
    const wchar_t* const application_data_path,
    ID3D12Device* const device,
    const void* const feature_common_info,
    const std::uint32_t sdk_version
) {
    const auto original = real_init_d3d12.load(std::memory_order_acquire);
    return original == nullptr ? 0xBAD00007U : original(
        application_id, application_data_path, device,
        feature_common_info, sdk_version
    );
}

NgxResult hook_core_init_d3d12(
    const unsigned long long application_id,
    const wchar_t* const application_data_path,
    ID3D12Device* const device,
    const void* const feature_common_info,
    const std::uint32_t sdk_version
) {
    const auto original = real_core_init_d3d12.load(std::memory_order_acquire);
    return original == nullptr ? 0xBAD00007U : original(
        application_id, application_data_path, device,
        feature_common_info, sdk_version
    );
}

NgxResult hook_shutdown_d3d12_1(ID3D12Device* const device) {
    const auto original = real_shutdown_d3d12_1.load(std::memory_order_acquire);
    return original == nullptr ? 0xBAD00007U : original(device);
}

NgxResult hook_core_shutdown_d3d12_1(ID3D12Device* const device) {
    const auto original = real_core_shutdown_d3d12_1.load(std::memory_order_acquire);
    return original == nullptr ? 0xBAD00007U : original(device);
}

[[nodiscard]] D3D11TransportNgx current_transport_ngx() noexcept {
    D3D11TransportNgx ngx{};
    ngx.runtime_module = GetModuleHandleW(L"_nvngx.dll");
    if (ngx.runtime_module != nullptr) {
        ngx.init_ext = reinterpret_cast<NgxD3D12InitExtFn>(GetProcAddress(
            ngx.runtime_module, "NVSDK_NGX_D3D12_Init_Ext"
        ));
        ngx.allocate_parameters =
            reinterpret_cast<NgxD3D12AllocateParametersFn>(GetProcAddress(
                ngx.runtime_module, "NVSDK_NGX_D3D12_AllocateParameters"
            ));
        ngx.backend.create_feature = real_core_create_d3d12.load(
            std::memory_order_acquire
        );
        ngx.backend.evaluate_feature = real_core_evaluate_d3d12.load(
            std::memory_order_acquire
        );
        ngx.backend.release_feature = real_core_release_d3d12.load(
            std::memory_order_acquire
        );
        ngx.shutdown = real_core_shutdown_d3d12_1.load(
            std::memory_order_acquire
        );
    }
    const auto public_runtime = GetModuleHandleW(L"nvngx_dlss.dll");
    if (public_runtime != nullptr) {
        ngx.get_application_id = reinterpret_cast<NgxGetApplicationIdFn>(
            GetProcAddress(public_runtime, "NVSDK_NGX_GetApplicationId")
        );
        ngx.get_api_version = reinterpret_cast<NgxGetApiVersionFn>(
            GetProcAddress(public_runtime, "NVSDK_NGX_GetAPIVersion")
        );
    }
    return ngx;
}

constexpr std::array<const char*, 6U> dlss_preset_parameter_names{
    "DLSS.Hint.Render.Preset.DLAA",
    "DLSS.Hint.Render.Preset.Quality",
    "DLSS.Hint.Render.Preset.Balanced",
    "DLSS.Hint.Render.Preset.Performance",
    "DLSS.Hint.Render.Preset.UltraPerformance",
    "DLSS.Hint.Render.Preset.UltraQuality",
};

class NgxPresetOverrideScope {
public:
    NgxPresetOverrideScope(
        const NgxParameters* const parameters,
        const std::uint32_t preset
    ) noexcept {
        if (parameters == nullptr || preset == 0U) return;
        parameters_ = const_cast<NgxParameters*>(parameters);
        for (std::size_t index{}; index < saved_.size(); ++index) {
            int signed_value{};
            if (ngx_succeeded(parameters_->Get(
                    dlss_preset_parameter_names[index],
                    &signed_value
                ))) {
                saved_[index] = static_cast<std::uint32_t>(signed_value);
            } else {
                saved_[index] = get_ui(
                    parameters_,
                    dlss_preset_parameter_names[index]
                );
            }
            parameters_->Set(
                dlss_preset_parameter_names[index],
                preset
            );
        }
        active_ = true;
    }

    void restore() noexcept {
        if (!active_ || parameters_ == nullptr) return;
        for (std::size_t index{}; index < saved_.size(); ++index) {
            parameters_->Set(
                dlss_preset_parameter_names[index],
                saved_[index]
            );
        }
        active_ = false;
    }

    ~NgxPresetOverrideScope() { restore(); }

private:
    NgxParameters* parameters_{};
    std::array<std::uint32_t, 6U> saved_{};
    bool active_{};
};

void prepare_d3d11_direct_peripheral(
    ID3D11DeviceContext* const context,
    const NgxHandle* const handle,
    const NgxParameters* const parameters,
    const Settings& settings,
    const D3D11PeripheralEvaluateFeatureFn evaluate_feature,
    D3D11PeripheralDlaaResult& result
) noexcept {
    if (!settings.enabled || !settings.peripheral_dlaa_enabled) {
        release_d3d11_peripheral_dlaa_view(handle);
        return;
    }
    auto create_feature = real_create_d3d11.load(std::memory_order_acquire);
    if (create_feature == nullptr) {
        create_feature = real_core_create_d3d11.load(std::memory_order_acquire);
    }
    auto release_feature = real_release_d3d11.load(std::memory_order_acquire);
    if (release_feature == nullptr) {
        release_feature = real_core_release_d3d11.load(
            std::memory_order_acquire
        );
    }
    if (create_feature == nullptr || evaluate_feature == nullptr ||
        release_feature == nullptr) {
        return;
    }
    static_cast<void>(evaluate_d3d11_peripheral_dlaa(
        context, handle, parameters, settings,
        create_feature, evaluate_feature, release_feature, result
    ));
}

NgxResult hook_create_d3d11(
    ID3D11DeviceContext* const context,
    const std::uint32_t feature,
    NgxParameters* const parameters,
    NgxHandle** const handle
) {
    const auto original = real_create_d3d11.load(std::memory_order_acquire);
    if (original == nullptr) return 0xBAD00007U;
    diagnostic_note_create(DiagnosticApi::d3d11);

    // Do not force output subrects on the game's feature. The foveated DX11
    // path uses a separate private feature whose creation-time output size is
    // the crop size, so the game feature can keep its original contract.
    const auto result = original(context, feature, parameters, handle);
    if (ngx_succeeded(result) && handle != nullptr && *handle != nullptr &&
        feature == 1U) {
        register_d3d11_game_feature(
            *handle,
            feature,
            original,
            real_release_d3d11.load(std::memory_order_acquire)
        );
        register_stereo_view(static_cast<DlssViewId>(
            reinterpret_cast<std::uintptr_t>(*handle)
        ));
    }
    return result;
}

NgxResult hook_core_create_d3d11(
    ID3D11DeviceContext* const context,
    const std::uint32_t feature,
    NgxParameters* const parameters,
    NgxHandle** const handle
) {
    const auto original = real_core_create_d3d11.load(std::memory_order_acquire);
    if (original == nullptr) return 0xBAD00007U;
    diagnostic_note_create(DiagnosticApi::d3d11);

    const auto result = original(context, feature, parameters, handle);
    if (ngx_succeeded(result) && handle != nullptr && *handle != nullptr &&
        feature == 1U) {
        register_d3d11_game_feature(
            *handle,
            feature,
            original,
            real_core_release_d3d11.load(std::memory_order_acquire)
        );
    }
    return result;
}

NgxResult hook_evaluate_d3d11(
    ID3D11DeviceContext* const context,
    const NgxHandle* const handle,
    const NgxParameters* const parameters,
    const NgxProgressCallback callback
) {
    const auto original = real_evaluate_d3d11.load(std::memory_order_acquire);
    if (original == nullptr) return 0xBAD00007U;
    note_evaluation_begin(DiagnosticApi::d3d11, parameters);
    if (!is_d3d11_private_handle(handle)) {
        register_stereo_view(static_cast<DlssViewId>(
            reinterpret_cast<std::uintptr_t>(handle)
        ));
    }
    const auto settings = current_settings();

    if (!settings.enabled &&
        !(settings.nr_enabled && settings.d3d11_use_d3d12_transport)) {
        diagnostic_note_state(DiagnosticApi::d3d11, DiagnosticState::disabled);
        diagnostic_note_d3d11_execution_path(
            D3D11ExecutionPath::disabled_passthrough
        );
        diagnostic_note_d3d11_transport_status(
            D3D11TransportStatus::not_attempted
        );
        D3D11DlssTimingScope timing{
            context, D3D11DlssTimingKind::native
        };
        const auto result = original(context, handle, parameters, callback);
        diagnostic_note_result(DiagnosticApi::d3d11, result);
        return result;
    }

    NgxResult transport_result{};
    if (!settings.d3d11_use_d3d12_transport) {
        diagnostic_note_d3d11_transport_status(
            D3D11TransportStatus::not_attempted
        );
    }
    if (settings.d3d11_use_d3d12_transport) {
        NgxPresetOverrideScope center_preset{
            parameters, settings.center_preset
        };
        if (evaluate_d3d11_via_d3d12(
                context, handle, parameters, settings,
                current_transport_ngx(), transport_result)) {
            release_d3d11_peripheral_dlaa_view(handle);
            diagnostic_note_d3d11_execution_path(
                D3D11ExecutionPath::dx12_transport
            );
            diagnostic_note_state(DiagnosticApi::d3d11, DiagnosticState::active);
            diagnostic_note_result(DiagnosticApi::d3d11, transport_result);
            return transport_result;
        }
    }

    D3D11PeripheralDlaaResult peripheral{};
    prepare_d3d11_direct_peripheral(
        context,
        handle,
        parameters,
        settings,
        original,
        peripheral
    );
    NgxPresetOverrideScope center_preset{
        parameters, settings.center_preset
    };
    auto* const evaluation = prepare_d3d11_private(
        context,
        handle,
        parameters,
        settings
    );
    if (evaluation != nullptr && peripheral.output_srv != nullptr) {
        d3d11_set_composite_base(
            evaluation,
            peripheral.output_srv,
            peripheral.working_width,
            peripheral.working_height
        );
    }
    release_d3d11_peripheral_dlaa_result(peripheral);
    const auto* const private_handle = d3d11_private_handle(evaluation);
    if (evaluation == nullptr || private_handle == nullptr) {
        center_preset.restore();
        diagnostic_note_state(
            DiagnosticApi::d3d11,
            DiagnosticState::prepare_rejected
        );
        diagnostic_note_d3d11_execution_path(
            D3D11ExecutionPath::game_fallback
        );
        D3D11DlssTimingScope timing{
            context, D3D11DlssTimingKind::native
        };
        const auto result = original(context, handle, parameters, callback);
        diagnostic_note_result(DiagnosticApi::d3d11, result);
        return result;
    }

    NgxResult result{};
    {
        D3D11DlssTimingScope timing{
            context, D3D11DlssTimingKind::foveated
        };
        result = original(context, private_handle, parameters, callback);
    }
    finish_d3d11(context, parameters, evaluation, result);
    center_preset.restore();

    // If the private feature ever rejects a frame, its parameter block has now
    // been restored by finish_d3d11. Fall back to the game's original feature
    // so a bad mod frame does not blank the game's output.
    if (!ngx_succeeded(result)) {
        diagnostic_note_state(
            DiagnosticApi::d3d11,
            DiagnosticState::ngx_evaluation_failed
        );
        diagnostic_note_d3d11_execution_path(
            D3D11ExecutionPath::game_fallback
        );
        D3D11DlssTimingScope timing{
            context, D3D11DlssTimingKind::native
        };
        result = original(context, handle, parameters, callback);
    } else {
        diagnostic_note_d3d11_execution_path(
            D3D11ExecutionPath::dx11_direct
        );
        diagnostic_note_state(DiagnosticApi::d3d11, DiagnosticState::active);
    }

    diagnostic_note_result(DiagnosticApi::d3d11, result);
    return result;
}

NgxResult hook_evaluate_d3d11_c(
    ID3D11DeviceContext* const context,
    const NgxHandle* const handle,
    const NgxParameters* const parameters,
    const NgxProgressCallbackC callback
) {
    const auto original = real_evaluate_d3d11_c.load(std::memory_order_acquire);
    if (original == nullptr) return 0xBAD00007U;
    note_evaluation_begin(DiagnosticApi::d3d11, parameters);
    if (!is_d3d11_private_handle(handle)) {
        register_stereo_view(static_cast<DlssViewId>(
            reinterpret_cast<std::uintptr_t>(handle)
        ));
    }
    const auto settings = current_settings();

    if (!settings.enabled &&
        !(settings.nr_enabled && settings.d3d11_use_d3d12_transport)) {
        diagnostic_note_state(DiagnosticApi::d3d11, DiagnosticState::disabled);
        diagnostic_note_d3d11_execution_path(
            D3D11ExecutionPath::disabled_passthrough
        );
        diagnostic_note_d3d11_transport_status(
            D3D11TransportStatus::not_attempted
        );
        D3D11DlssTimingScope timing{
            context, D3D11DlssTimingKind::native
        };
        const auto result = original(context, handle, parameters, callback);
        diagnostic_note_result(DiagnosticApi::d3d11, result);
        return result;
    }

    NgxResult transport_result{};
    if (!settings.d3d11_use_d3d12_transport) {
        diagnostic_note_d3d11_transport_status(
            D3D11TransportStatus::not_attempted
        );
    }
    if (settings.d3d11_use_d3d12_transport) {
        NgxPresetOverrideScope center_preset{
            parameters, settings.center_preset
        };
        if (evaluate_d3d11_via_d3d12(
                context, handle, parameters, settings,
                current_transport_ngx(), transport_result)) {
            release_d3d11_peripheral_dlaa_view(handle);
            diagnostic_note_d3d11_execution_path(
                D3D11ExecutionPath::dx12_transport
            );
            diagnostic_note_state(DiagnosticApi::d3d11, DiagnosticState::active);
            diagnostic_note_result(DiagnosticApi::d3d11, transport_result);
            return transport_result;
        }
    }

    D3D11PeripheralDlaaResult peripheral{};
    prepare_d3d11_direct_peripheral(
        context,
        handle,
        parameters,
        settings,
        real_evaluate_d3d11.load(std::memory_order_acquire),
        peripheral
    );
    NgxPresetOverrideScope center_preset{
        parameters, settings.center_preset
    };
    auto* const evaluation = prepare_d3d11_private(
        context,
        handle,
        parameters,
        settings
    );
    if (evaluation != nullptr && peripheral.output_srv != nullptr) {
        d3d11_set_composite_base(
            evaluation,
            peripheral.output_srv,
            peripheral.working_width,
            peripheral.working_height
        );
    }
    release_d3d11_peripheral_dlaa_result(peripheral);
    const auto* const private_handle = d3d11_private_handle(evaluation);
    if (evaluation == nullptr || private_handle == nullptr) {
        center_preset.restore();
        diagnostic_note_state(
            DiagnosticApi::d3d11,
            DiagnosticState::prepare_rejected
        );
        diagnostic_note_d3d11_execution_path(
            D3D11ExecutionPath::game_fallback
        );
        D3D11DlssTimingScope timing{
            context, D3D11DlssTimingKind::native
        };
        const auto result = original(context, handle, parameters, callback);
        diagnostic_note_result(DiagnosticApi::d3d11, result);
        return result;
    }

    NgxResult result{};
    {
        D3D11DlssTimingScope timing{
            context, D3D11DlssTimingKind::foveated
        };
        result = original(context, private_handle, parameters, callback);
    }
    finish_d3d11(context, parameters, evaluation, result);
    center_preset.restore();
    if (!ngx_succeeded(result)) {
        diagnostic_note_state(
            DiagnosticApi::d3d11,
            DiagnosticState::ngx_evaluation_failed
        );
        diagnostic_note_d3d11_execution_path(
            D3D11ExecutionPath::game_fallback
        );
        D3D11DlssTimingScope timing{
            context, D3D11DlssTimingKind::native
        };
        result = original(context, handle, parameters, callback);
    } else {
        diagnostic_note_d3d11_execution_path(
            D3D11ExecutionPath::dx11_direct
        );
        diagnostic_note_state(DiagnosticApi::d3d11, DiagnosticState::active);
    }

    diagnostic_note_result(DiagnosticApi::d3d11, result);
    return result;
}

NgxResult hook_release_d3d11(NgxHandle* const handle) {
    const auto original = real_release_d3d11.load(std::memory_order_acquire);
    release_d3d11_transport_view(handle);
    release_d3d11_peripheral_dlaa_view(handle);
    unregister_d3d11_game_feature(handle);
    const auto view_id = static_cast<DlssViewId>(
        reinterpret_cast<std::uintptr_t>(handle)
    );
    unregister_stereo_view(view_id);
    forget_gaze_view(view_id);
    return original == nullptr ? 0xBAD00007U : original(handle);
}

NgxResult hook_core_release_d3d11(NgxHandle* const handle) {
    const auto original = real_core_release_d3d11.load(std::memory_order_acquire);
    release_d3d11_transport_view(handle);
    release_d3d11_peripheral_dlaa_view(handle);
    unregister_d3d11_game_feature(handle);
    const auto view_id = static_cast<DlssViewId>(
        reinterpret_cast<std::uintptr_t>(handle)
    );
    unregister_stereo_view(view_id);
    forget_gaze_view(view_id);
    return original == nullptr ? 0xBAD00007U : original(handle);
}

NgxResult hook_create_d3d12(
    ID3D12GraphicsCommandList* const command_list,
    const std::uint32_t feature,
    NgxParameters* const parameters,
    NgxHandle** const handle
) {
    const auto original = real_create_d3d12.load(std::memory_order_acquire);
    if (original == nullptr) return 0xBAD00007U;
    D3D12NgxInterceptionScope scope;
    if (!scope.outermost()) {
        return original(command_list, feature, parameters, handle);
    }
    diagnostic_note_create(DiagnosticApi::d3d12);
    if (is_dlss_feature(feature) && parameters != nullptr) {
        captured_d3d12_create_flags.store(
            get_ngx_integer_bits(parameters, "DLSS.Feature.Create.Flags"),
            std::memory_order_release
        );
        captured_d3d12_create_flags_valid.store(true, std::memory_order_release);
    }
    const auto result = original(command_list, feature, parameters, handle);
    if (ngx_succeeded(result) && handle != nullptr) {
        remember_d3d12_game_view(*handle, feature);
    }
    return result;
}

NgxResult hook_core_create_d3d12(
    ID3D12GraphicsCommandList* const command_list,
    const std::uint32_t feature,
    NgxParameters* const parameters,
    NgxHandle** const handle
) {
    const auto original = real_core_create_d3d12.load(std::memory_order_acquire);
    if (original == nullptr) return 0xBAD00007U;
    D3D12NgxInterceptionScope scope;
    if (!scope.outermost()) {
        return original(command_list, feature, parameters, handle);
    }
    diagnostic_note_create(DiagnosticApi::d3d12);
    if (is_dlss_feature(feature) && parameters != nullptr) {
        captured_d3d12_create_flags.store(
            get_ngx_integer_bits(parameters, "DLSS.Feature.Create.Flags"),
            std::memory_order_release
        );
        captured_d3d12_create_flags_valid.store(true, std::memory_order_release);
    }
    const auto result = original(command_list, feature, parameters, handle);
    if (ngx_succeeded(result) && handle != nullptr) {
        remember_d3d12_game_view(*handle, feature);
    }
    return result;
}

[[nodiscard]] ID3D12Resource* get_d3d12_parameter_resource(
    const NgxParameters* const parameters,
    const char* const name
) noexcept {
    ID3D12Resource* resource{};
    return parameters != nullptr && ngx_succeeded(parameters->Get(name, &resource))
        ? resource
        : nullptr;
}

[[nodiscard]] float get_d3d12_parameter_float(
    const NgxParameters* const parameters,
    const char* const name,
    const float fallback
) noexcept {
    float value{};
    return parameters != nullptr && ngx_succeeded(parameters->Get(name, &value))
        ? value
        : fallback;
}

void evaluate_nr_after_native_d3d12(
    ID3D12GraphicsCommandList* const command_list,
    const NgxHandle* const handle,
    const NgxParameters* const parameters,
    const Settings& settings,
    const NgxResult result
) noexcept {
    if (!settings.nr_enabled || !ngx_succeeded(result) ||
        command_list == nullptr || handle == nullptr || parameters == nullptr ||
        !is_dlss_feature(d3d12_game_feature(handle))) return;
    auto input_width = get_ui(parameters, "DLSS.Render.Subrect.Dimensions.Width");
    auto input_height = get_ui(parameters, "DLSS.Render.Subrect.Dimensions.Height");
    if (input_width == 0U) input_width = get_ui(parameters, "Width");
    if (input_height == 0U) input_height = get_ui(parameters, "Height");
    const auto output_width = get_ui(parameters, "OutWidth");
    const auto output_height = get_ui(parameters, "OutHeight");
    const auto flags = get_ngx_integer_bits(
        parameters, "DLSS.Feature.Create.Flags"
    );
    const bool low_resolution_motion = (flags & (1U << 1U)) != 0U;
    const auto view_id = static_cast<DlssViewId>(
        reinterpret_cast<std::uintptr_t>(handle)
    );
    const DlssNrFrame frame{
        view_id,
        DlssNrRoute::d3d12_native,
        command_list,
        get_d3d12_parameter_resource(parameters, "Output"),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        get_d3d12_parameter_resource(parameters, "Depth"),
        get_d3d12_parameter_resource(parameters, "MotionVectors"),
        input_width,
        input_height,
        output_width,
        output_height,
        get_ui(parameters, "DLSS.Input.Depth.Subrect.Base.X"),
        get_ui(parameters, "DLSS.Input.Depth.Subrect.Base.Y"),
        input_width,
        input_height,
        get_ui(parameters, "DLSS.Input.MV.Subrect.Base.X"),
        get_ui(parameters, "DLSS.Input.MV.Subrect.Base.Y"),
        low_resolution_motion ? input_width : output_width,
        low_resolution_motion ? input_height : output_height,
        get_d3d12_parameter_float(parameters, "MV.Scale.X", 1.0F),
        get_d3d12_parameter_float(parameters, "MV.Scale.Y", 1.0F),
        (flags & (1U << 3U)) != 0U,
        get_ui(parameters, "Reset") != 0U,
        flags,
        get_ui(parameters, "DLSS.Output.Subrect.Base.X"),
        get_ui(parameters, "DLSS.Output.Subrect.Base.Y"),
        false,
    };
    const auto view_settings = settings_for_view(settings, view_id);
    D3D12NrTimingScope timing{command_list, view_settings.nr_foveated};
    const bool evaluated = evaluate_dlss_nr(
        frame,
        view_settings
    );
    timing.finish(evaluated);
}

[[nodiscard]] bool evaluate_native_d3d12_canonical(
    ID3D12GraphicsCommandList* const command_list,
    const NgxHandle* const handle,
    const NgxParameters* const parameters,
    const Settings& settings,
    const D3D12BackendCallbacks& callbacks,
    NgxResult& result,
    bool& private_attempted
) noexcept {
    private_attempted = false;
    if (!settings.enabled || command_list == nullptr || handle == nullptr ||
        parameters == nullptr || callbacks.create_feature == nullptr ||
        callbacks.evaluate_feature == nullptr ||
        callbacks.release_feature == nullptr) return false;

    DlssFrameContract contract{};
    contract.view_id = static_cast<DlssViewId>(
        reinterpret_cast<std::uintptr_t>(handle)
    );
    contract.feature_id = d3d12_game_feature(handle);
    contract.render_width = get_ui(parameters, "Width");
    contract.render_height = get_ui(parameters, "Height");
    const auto subrect_width = get_ui(
        parameters, "DLSS.Render.Subrect.Dimensions.Width"
    );
    const auto subrect_height = get_ui(
        parameters, "DLSS.Render.Subrect.Dimensions.Height"
    );
    if (subrect_width != 0U) contract.render_width = subrect_width;
    if (subrect_height != 0U) contract.render_height = subrect_height;
    contract.output_width = get_ui(parameters, "OutWidth");
    contract.output_height = get_ui(parameters, "OutHeight");
    contract.color_base_x = get_ui(parameters, "DLSS.Input.Color.Subrect.Base.X");
    contract.color_base_y = get_ui(parameters, "DLSS.Input.Color.Subrect.Base.Y");
    contract.depth_base_x = get_ui(parameters, "DLSS.Input.Depth.Subrect.Base.X");
    contract.depth_base_y = get_ui(parameters, "DLSS.Input.Depth.Subrect.Base.Y");
    contract.mv_base_x = get_ui(parameters, "DLSS.Input.MV.Subrect.Base.X");
    contract.mv_base_y = get_ui(parameters, "DLSS.Input.MV.Subrect.Base.Y");
    contract.output_base_x = get_ui(parameters, "DLSS.Output.Subrect.Base.X");
    contract.output_base_y = get_ui(parameters, "DLSS.Output.Subrect.Base.Y");
    contract.create_flags = get_ngx_integer_bits(
        parameters, "DLSS.Feature.Create.Flags"
    );
    contract.motion_vectors_low_res =
        (contract.create_flags & (1U << 1U)) != 0U;
    contract.depth_inverted = (contract.create_flags & (1U << 3U)) != 0U;
    contract.reset = get_ui(parameters, "Reset") != 0U;
    contract.perf_quality = get_ngx_integer_bits(
        parameters, "PerfQualityValue"
    );
    contract.motion_vector_scale_x = get_d3d12_parameter_float(
        parameters, "MV.Scale.X", 1.0F
    );
    contract.motion_vector_scale_y = get_d3d12_parameter_float(
        parameters, "MV.Scale.Y", 1.0F
    );

    const auto effective_settings = settings_for_view(settings, contract.view_id);

    auto* const full_color = get_d3d12_parameter_resource(parameters, "Color");
    auto* const full_depth = get_d3d12_parameter_resource(parameters, "Depth");
    auto* const full_motion =
        get_d3d12_parameter_resource(parameters, "MotionVectors");
    auto* const full_output = get_d3d12_parameter_resource(parameters, "Output");
    const auto full_color_x = contract.color_base_x;
    const auto full_color_y = contract.color_base_y;
    const auto full_depth_x = contract.depth_base_x;
    const auto full_depth_y = contract.depth_base_y;
    const auto full_mv_x = contract.mv_base_x;
    const auto full_mv_y = contract.mv_base_y;

    bool motion_vectors_output_space = !contract.motion_vectors_low_res;
    if (full_motion != nullptr) {
        const auto motion_description = full_motion->GetDesc();
        const auto mv_width =
            static_cast<std::uint32_t>(motion_description.Width);
        const auto mv_height = motion_description.Height;
        const auto distance_input = dimension_distance(
            mv_width,
            mv_height,
            contract.render_width,
            contract.render_height
        );
        const auto distance_output = dimension_distance(
            mv_width,
            mv_height,
            contract.output_width,
            contract.output_height
        );
        if (distance_input != distance_output) {
            motion_vectors_output_space = distance_output < distance_input;
        }
        diagnostic_note_motion_vectors(
            DiagnosticApi::d3d12,
            mv_width,
            mv_height,
            motion_vectors_output_space
                ? MotionVectorSpace::output
                : MotionVectorSpace::input
        );
    }

    PeripheralDlaaResources peripheral{};
    bool peripheral_ready{};
    if (effective_settings.peripheral_dlaa_enabled &&
        contract.feature_id == 1U &&
        full_color != nullptr && full_depth != nullptr &&
        full_motion != nullptr && full_output != nullptr) {
        PeripheralDlaaRequest peripheral_request{};
        peripheral_request.view_id = contract.view_id;
        peripheral_request.command_list = command_list;
        peripheral_request.color = full_color;
        peripheral_request.depth = full_depth;
        peripheral_request.motion_vectors = full_motion;
        peripheral_request.output_template = full_output;
        peripheral_request.render_width = contract.render_width;
        peripheral_request.render_height = contract.render_height;
        peripheral_request.source_output_width = contract.output_width;
        peripheral_request.source_output_height = contract.output_height;
        peripheral_request.scale = effective_settings.peripheral_dlaa_scale;
        peripheral_request.preset = effective_settings.peripheral_dlaa_preset;
        peripheral_request.color_base_x = full_color_x;
        peripheral_request.color_base_y = full_color_y;
        peripheral_request.depth_base_x = full_depth_x;
        peripheral_request.depth_base_y = full_depth_y;
        peripheral_request.mv_base_x = full_mv_x;
        peripheral_request.mv_base_y = full_mv_y;
        peripheral_request.motion_vectors_output_space =
            motion_vectors_output_space;
        peripheral_request.motion_vector_scale_x =
            contract.motion_vector_scale_x;
        peripheral_request.motion_vector_scale_y =
            contract.motion_vector_scale_y;
        peripheral_request.depth_inverted = contract.depth_inverted;
        peripheral_request.reset = contract.reset;
        peripheral_request.create_flags = contract.create_flags;
        peripheral_request.parameters =
            const_cast<NgxParameters*>(parameters);
        peripheral_request.callbacks = callbacks;
        D3D12PeripheralTimingScope peripheral_timing{command_list};
        NgxResult peripheral_result{};
        peripheral_ready = evaluate_peripheral_dlaa_ngx(
            peripheral_request,
            peripheral,
            peripheral_result,
            peripheral_timing.backend()
        );
        peripheral_timing.finish(peripheral_ready);
    }

    NgxPresetOverrideScope center_preset{
        parameters, effective_settings.center_preset
    };
    auto* const evaluation = prepare_d3d12(
        command_list,
        parameters,
        contract.view_id,
        settings
    );
    if (evaluation == nullptr) {
        if (peripheral_ready) {
            restore_peripheral_dlaa_output(command_list, peripheral);
        }
        return false;
    }
    contract.reset = contract.reset || d3d12_evaluation_gaze_reset(evaluation);
    contract.preserve_history_on_crop_move =
        settings.center_mode != FoveationCenterMode::fixed;
    private_attempted = true;

    if (peripheral_ready && !d3d12_set_composite_base(
            evaluation,
            peripheral.output,
            0U,
            0U
        )) {
        restore_peripheral_dlaa_output(command_list, peripheral);
        peripheral_ready = false;
    }

    D3D12DlssInputs inputs{};
    inputs.color = get_d3d12_parameter_resource(parameters, "Color");
    inputs.depth = get_d3d12_parameter_resource(parameters, "Depth");
    inputs.motion_vectors = get_d3d12_parameter_resource(parameters, "MotionVectors");
    inputs.exposure = get_d3d12_parameter_resource(parameters, "ExposureTexture");
    inputs.output = d3d12_private_output(evaluation);
    inputs.color_base_x = get_ui(parameters, "DLSS.Input.Color.Subrect.Base.X");
    inputs.color_base_y = get_ui(parameters, "DLSS.Input.Color.Subrect.Base.Y");
    inputs.depth_base_x = get_ui(parameters, "DLSS.Input.Depth.Subrect.Base.X");
    inputs.depth_base_y = get_ui(parameters, "DLSS.Input.Depth.Subrect.Base.Y");
    inputs.mv_base_x = get_ui(parameters, "DLSS.Input.MV.Subrect.Base.X");
    inputs.mv_base_y = get_ui(parameters, "DLSS.Input.MV.Subrect.Base.Y");

    const auto crop = d3d12_evaluation_crop(evaluation);
    note_stereo_view_geometry(
        contract.view_id,
        contract.render_width,
        contract.render_height,
        contract.output_width,
        contract.output_height,
        crop
    );
    D3D12PeripheralTimingScope sr_timing{
        command_list, D3D12TimingKind::foveated_dlss
    };
    result = evaluate_d3d12_backend(
        command_list, contract, inputs,
        const_cast<NgxParameters*>(parameters),
        crop, callbacks, sr_timing.backend()
    );
    sr_timing.finish(ngx_succeeded(result));
    diagnostic_note_private_result(DiagnosticApi::d3d12, result);
    finish_d3d12(command_list, parameters, evaluation, result);
    if (peripheral_ready) {
        restore_peripheral_dlaa_output(command_list, peripheral);
    }
    if (!ngx_succeeded(result)) return false;
    evaluate_nr_after_native_d3d12(
        command_list, handle, parameters, effective_settings, result
    );
    diagnostic_note_activation(DiagnosticApi::d3d12, crop);
    return true;
}

[[nodiscard]] D3D12BackendCallbacks d3d12_backend_callbacks(
    const D3D12NgxRoute route
) noexcept {
    if (route == D3D12NgxRoute::core_runtime) {
        return {
            real_core_create_d3d12.load(std::memory_order_acquire),
            real_core_evaluate_d3d12.load(std::memory_order_acquire),
            real_core_release_d3d12.load(std::memory_order_acquire),
        };
    }
    return {
        real_create_d3d12.load(std::memory_order_acquire),
        real_evaluate_d3d12.load(std::memory_order_acquire),
        real_release_d3d12.load(std::memory_order_acquire),
    };
}

[[nodiscard]] bool recognizable_d3d12_dlss_evaluation(
    const D3D12NgxEvaluationCall& call
) noexcept {
    if (call.handle == nullptr || call.parameters == nullptr) return false;
    const auto input_width = get_ui(call.parameters, "Width") != 0U
        ? get_ui(call.parameters, "Width")
        : get_ui(call.parameters, "DLSS.Render.Subrect.Dimensions.Width");
    const auto input_height = get_ui(call.parameters, "Height") != 0U
        ? get_ui(call.parameters, "Height")
        : get_ui(call.parameters, "DLSS.Render.Subrect.Dimensions.Height");
    return input_width != 0U && input_height != 0U &&
        get_ui(call.parameters, "OutWidth") != 0U &&
        get_ui(call.parameters, "OutHeight") != 0U &&
        get_d3d12_parameter_resource(call.parameters, "Color") != nullptr &&
        get_d3d12_parameter_resource(call.parameters, "Depth") != nullptr &&
        get_d3d12_parameter_resource(
            call.parameters, "MotionVectors"
        ) != nullptr &&
        get_d3d12_parameter_resource(call.parameters, "Output") != nullptr;
}

NgxResult process_d3d12_evaluation(
    const D3D12NgxEvaluationCall& call,
    const D3D12NgxEvaluateFn original,
    void*
) {
    if (call.route == D3D12NgxRoute::core_runtime &&
        !has_d3d12_game_view(call.handle)) {
        if (!recognizable_d3d12_dlss_evaluation(call)) {
            return original(
                call.command_list,
                call.handle,
                call.parameters,
                call.callback
            );
        }
        // Core runtimes may initialize and create the game feature before the
        // delayed direct hooks are admitted. Adopt that feature on its first
        // recognizable evaluation so stereo and per-view cleanup still work.
        remember_d3d12_game_view(call.handle, 1U);
    }
    note_evaluation_begin(DiagnosticApi::d3d12, call.parameters);
    diagnostic_note_d3d12_ngx_route(call.route);
    static std::atomic<std::uint32_t> route_logs{};
    const auto route_log = route_logs.fetch_add(1U, std::memory_order_relaxed);
    if (route_log < 8U) {
        trace_event(
            "D3D12 NGX evaluation route=%s handle=%p command=%p",
            d3d12_ngx_route_name(call.route),
            call.handle,
            call.command_list
        );
    }
    if (inside_streamline_evaluation) {
        diagnostic_note_state(
            DiagnosticApi::d3d12,
            DiagnosticState::streamline_direct_path_suppressed
        );
        const auto result = original(
            call.command_list,
            call.handle,
            call.parameters,
            call.callback
        );
        diagnostic_note_result(DiagnosticApi::d3d12, result);
        return result;
    }
    const auto settings = current_settings();
    NgxResult result{};
    bool private_attempted{};
    const auto callbacks = d3d12_backend_callbacks(call.route);
    if (evaluate_native_d3d12_canonical(
            call.command_list,
            call.handle,
            call.parameters,
            settings,
            callbacks,
            result,
            private_attempted)) {
        diagnostic_note_state(DiagnosticApi::d3d12, DiagnosticState::active);
        diagnostic_note_result(DiagnosticApi::d3d12, result);
        return result;
    }
    diagnostic_note_state(
        DiagnosticApi::d3d12,
        !settings.enabled ? DiagnosticState::disabled
        : private_attempted ? DiagnosticState::ngx_evaluation_failed
                            : DiagnosticState::prepare_rejected
    );
    D3D12PeripheralTimingScope sr_timing{
        call.command_list, D3D12TimingKind::native_dlss
    };
    sr_timing.begin();
    result = original(
        call.command_list,
        call.handle,
        call.parameters,
        call.callback
    );
    sr_timing.finish(ngx_succeeded(result));
    evaluate_nr_after_native_d3d12(
        call.command_list, call.handle, call.parameters, settings, result
    );
    diagnostic_note_result(DiagnosticApi::d3d12, result);
    if (!ngx_succeeded(result)) {
        diagnostic_note_state(
            DiagnosticApi::d3d12,
            DiagnosticState::ngx_evaluation_failed
        );
    }
    return result;
}

NgxResult hook_evaluate_d3d12(
    ID3D12GraphicsCommandList* const command_list,
    const NgxHandle* const handle,
    const NgxParameters* const parameters,
    const NgxProgressCallback callback
) {
    const auto original = real_evaluate_d3d12.load(std::memory_order_acquire);
    return dispatch_d3d12_ngx_evaluation(
        {
            D3D12NgxRoute::public_runtime,
            command_list,
            handle,
            parameters,
            callback,
        },
        original,
        &process_d3d12_evaluation
    );
}

NgxResult hook_core_evaluate_d3d12(
    ID3D12GraphicsCommandList* const command_list,
    const NgxHandle* const handle,
    const NgxParameters* const parameters,
    const NgxProgressCallback callback
) {
    const auto original = real_core_evaluate_d3d12.load(
        std::memory_order_acquire
    );
    return dispatch_d3d12_ngx_evaluation(
        {
            D3D12NgxRoute::core_runtime,
            command_list,
            handle,
            parameters,
            callback,
        },
        original,
        &process_d3d12_evaluation
    );
}

NgxResult hook_evaluate_d3d12_c(
    ID3D12GraphicsCommandList* const command_list,
    const NgxHandle* const handle,
    const NgxParameters* const parameters,
    const NgxProgressCallbackC callback
) {
    const auto original = real_evaluate_d3d12_c.load(std::memory_order_acquire);
    if (original == nullptr) return 0xBAD00007U;
    D3D12NgxInterceptionScope scope;
    if (!scope.outermost()) {
        return original(command_list, handle, parameters, callback);
    }
    note_evaluation_begin(DiagnosticApi::d3d12, parameters);
    diagnostic_note_d3d12_ngx_route(D3D12NgxRoute::public_runtime);
    if (inside_streamline_evaluation) {
        diagnostic_note_state(
            DiagnosticApi::d3d12,
            DiagnosticState::streamline_direct_path_suppressed
        );
        const auto result = original(
            command_list,
            handle,
            parameters,
            callback
        );
        diagnostic_note_result(DiagnosticApi::d3d12, result);
        return result;
    }
    const auto settings = current_settings();
    NgxResult result{};
    bool private_attempted{};
    const auto callbacks = d3d12_backend_callbacks(
        D3D12NgxRoute::public_runtime
    );
    if (evaluate_native_d3d12_canonical(
            command_list, handle, parameters, settings,
            callbacks, result, private_attempted)) {
        diagnostic_note_state(DiagnosticApi::d3d12, DiagnosticState::active);
        diagnostic_note_result(DiagnosticApi::d3d12, result);
        return result;
    }
    diagnostic_note_state(
        DiagnosticApi::d3d12,
        !settings.enabled ? DiagnosticState::disabled
        : private_attempted ? DiagnosticState::ngx_evaluation_failed
                            : DiagnosticState::prepare_rejected
    );
    D3D12PeripheralTimingScope sr_timing{
        command_list, D3D12TimingKind::native_dlss
    };
    sr_timing.begin();
    result = original(command_list, handle, parameters, callback);
    sr_timing.finish(ngx_succeeded(result));
    evaluate_nr_after_native_d3d12(
        command_list, handle, parameters, settings, result
    );
    diagnostic_note_result(DiagnosticApi::d3d12, result);
    if (!ngx_succeeded(result)) {
        diagnostic_note_state(
            DiagnosticApi::d3d12,
            DiagnosticState::ngx_evaluation_failed
        );
    }
    return result;
}

NgxResult hook_release_d3d12(NgxHandle* const handle) {
    const auto original = real_release_d3d12.load(std::memory_order_acquire);
    if (original == nullptr) return 0xBAD00007U;
    D3D12NgxInterceptionScope scope;
    if (!scope.outermost()) return original(handle);
    if (has_d3d12_game_view(handle)) forget_d3d12_game_view(handle);
    return original(handle);
}

NgxResult hook_core_release_d3d12(NgxHandle* const handle) {
    const auto original = real_core_release_d3d12.load(
        std::memory_order_acquire
    );
    if (original == nullptr) return 0xBAD00007U;
    D3D12NgxInterceptionScope scope;
    if (!scope.outermost()) return original(handle);
    if (has_d3d12_game_view(handle)) forget_d3d12_game_view(handle);
    return original(handle);
}

template <typename Function>
[[nodiscard]] bool install_streamline_minhook(
    const char* const name,
    void* const target,
    void* const detour,
    std::atomic<Function>& storage
) noexcept {
    if (target == nullptr || detour == nullptr) return false;
    void* trampoline{};
    const auto create_result = MH_CreateHook(target, detour, &trampoline);
    if (create_result != MH_OK) {
        trace_event(
            "Streamline MinHook create failed name=%s target=%p result=%d",
            name, target, static_cast<int>(create_result)
        );
        return false;
    }
    // Publish the trampoline before enabling the detour. The detour is not
    // reachable until MH_EnableHook succeeds, so this avoids a tiny window where
    // a newly enabled hook could observe a null original. If enable fails, clear
    // storage before removing the hook/trampoline.
    storage.store(reinterpret_cast<Function>(trampoline), std::memory_order_release);
    const auto enable_result = MH_EnableHook(target);
    if (enable_result != MH_OK && enable_result != MH_ERROR_ENABLED) {
        storage.store(nullptr, std::memory_order_release);
        const auto remove_result = MH_RemoveHook(target);
        trace_event(
            "Streamline MinHook enable failed name=%s target=%p result=%d remove=%d",
            name, target, static_cast<int>(enable_result),
            static_cast<int>(remove_result)
        );
        return false;
    }
    trace_event(
        "Streamline MinHook installed name=%s target=%p trampoline=%p detour=%p",
        name, target, trampoline, detour
    );
    return true;
}

[[nodiscard]] bool install_streamline_inline_hooks() noexcept {
    if (streamline_inline_mode.load(std::memory_order_acquire)) return true;
    if (streamline_inline_install_failed.load(std::memory_order_acquire)) {
        return false;
    }

    const auto module = GetModuleHandleW(L"sl.interposer.dll");
    if (module == nullptr) return false;
    const auto resolve = [module](const char* const name) noexcept {
        return reinterpret_cast<void*>(GetProcAddress(module, name));
    };

    auto* const evaluate_target = resolve("slEvaluateFeature");
    auto* const set_tag_target = resolve("slSetTag");
    auto* const set_constants_target = resolve("slSetConstants");
    auto* const get_feature_target = resolve("slGetFeatureFunction");
    auto* const set_for_frame_target = resolve("slSetTagForFrame");
    trace_event(
        "SL persistent-hook resolve module=%p evaluate=%p setTag=%p setTagForFrame=%p setConstants=%p getFeature=%p",
        module, evaluate_target, set_tag_target, set_for_frame_target,
        set_constants_target, get_feature_target
    );
    if (evaluate_target == nullptr || set_tag_target == nullptr ||
        set_constants_target == nullptr || get_feature_target == nullptr) {
        return false;
    }

    if (!streamline_hook_lock_ready.exchange(true, std::memory_order_acq_rel)) {
        InitializeCriticalSection(&streamline_hook_lock);
        InitializeCriticalSection(&streamline_evaluation_lock);
    }

    const bool evaluate = install_streamline_minhook(
        "slEvaluateFeature", evaluate_target,
        reinterpret_cast<void*>(&hook_sl_evaluate_feature),
        real_sl_evaluate_feature
    );
    const bool set_tag = install_streamline_minhook(
        "slSetTag", set_tag_target,
        reinterpret_cast<void*>(&hook_sl_set_tag),
        real_sl_set_tag
    );
    const bool set_tag_for_frame = set_for_frame_target != nullptr &&
        install_streamline_minhook(
            "slSetTagForFrame", set_for_frame_target,
            reinterpret_cast<void*>(&hook_sl_set_tag_for_frame),
            real_sl_set_tag_for_frame
        );
    const bool set_constants = install_streamline_minhook(
        "slSetConstants", set_constants_target,
        reinterpret_cast<void*>(&hook_sl_set_constants),
        real_sl_set_constants
    );
    const bool get_feature = install_streamline_minhook(
        "slGetFeatureFunction", get_feature_target,
        reinterpret_cast<void*>(&hook_sl_get_feature_function),
        real_sl_get_feature_function
    );

    const bool essential = evaluate && set_tag && set_constants && get_feature;
    if (!essential) {
        streamline_inline_install_failed.store(true, std::memory_order_release);
    }
    trace_event(
        "Streamline persistent hooks evaluate=%s setTag=%s setTagForFrame=%s setConstants=%s getFeatureFunction=%s",
        evaluate ? "yes" : "no",
        set_tag ? "yes" : "no",
        set_tag_for_frame ? "yes" : "no",
        set_constants ? "yes" : "no",
        get_feature ? "yes" : "no"
    );
    streamline_inline_mode.store(essential, std::memory_order_release);
    diagnostic_note_streamline_detected();
    if (essential) diagnostic_note_direct_detour(DiagnosticApi::d3d12);
    return essential;
}

void uninstall_streamline_inline_hooks() noexcept {
    const auto module = GetModuleHandleW(L"sl.interposer.dll");
    if (module != nullptr) {
        constexpr const char* names[] = {
            "slGetFeatureFunction",
            "slSetConstants",
            "slSetTagForFrame",
            "slSetTag",
            "slEvaluateFeature",
        };
        for (const auto* const name : names) {
            auto* const target = reinterpret_cast<void*>(GetProcAddress(module, name));
            if (target == nullptr) continue;
            static_cast<void>(MH_DisableHook(target));
            static_cast<void>(MH_RemoveHook(target));
        }
    }
    if (streamline_hook_lock_ready.load(std::memory_order_acquire)) {
        DeleteCriticalSection(&streamline_hook_lock);
        DeleteCriticalSection(&streamline_evaluation_lock);
    }
    streamline_hook_lock_ready.store(false, std::memory_order_release);
    streamline_inline_mode.store(false, std::memory_order_release);
    streamline_inline_install_failed.store(false, std::memory_order_release);
    real_sl_evaluate_feature.store(nullptr, std::memory_order_release);
    real_sl_set_tag.store(nullptr, std::memory_order_release);
    real_sl_set_tag_for_frame.store(nullptr, std::memory_order_release);
    real_sl_set_constants.store(nullptr, std::memory_order_release);
    real_sl_get_feature_function.store(nullptr, std::memory_order_release);
    real_sl_dlss_set_options.store(nullptr, std::memory_order_release);
}

[[nodiscard]] bool runtime_ready_for_direct_hooks(
    const HMODULE module,
    RuntimeStability& stability,
    const wchar_t* const label,
    const bool require_stability
) noexcept {
    AcquireSRWLockExclusive(&runtime_stability_lock);

    if (module == nullptr) {
        if (stability.module != nullptr && !stability.admitted) {
            trace_event(
                "NGX runtime vanished before stabilization runtime=%ls module=%p scans=%u",
                label,
                stability.module,
                stability.consecutive_scans
            );
        }
        stability = {};
        ReleaseSRWLockExclusive(&runtime_stability_lock);
        return false;
    }

    if (!require_stability) {
        stability.module = module;
        stability.consecutive_scans = runtime_stability_required_scans;
        stability.admitted = true;
        ReleaseSRWLockExclusive(&runtime_stability_lock);
        trace_event(
            "NGX runtime admitted immediately runtime=%ls module=%p phase=startup",
            label,
            module
        );
        return true;
    }

    if (stability.module != module) {
        if (stability.module != nullptr) {
            trace_event(
                "NGX runtime instance changed runtime=%ls old=%p new=%p old_scans=%u old_admitted=%s",
                label,
                stability.module,
                module,
                stability.consecutive_scans,
                stability.admitted ? "yes" : "no"
            );
        }
        stability.module = module;
        stability.consecutive_scans = 1U;
        stability.admitted = false;
        ReleaseSRWLockExclusive(&runtime_stability_lock);
        trace_event(
            "NGX runtime first sighting runtime=%ls module=%p scans=1/%u; deferring direct hooks",
            label,
            module,
            runtime_stability_required_scans
        );
        return false;
    }

    if (stability.admitted) {
        ReleaseSRWLockExclusive(&runtime_stability_lock);
        return true;
    }

    if (stability.consecutive_scans < runtime_stability_required_scans) {
        ++stability.consecutive_scans;
    }
    const auto scans = stability.consecutive_scans;
    if (scans >= runtime_stability_required_scans) {
        stability.admitted = true;
        ReleaseSRWLockExclusive(&runtime_stability_lock);
        trace_event(
            "NGX runtime stabilized runtime=%ls module=%p scans=%u/%u; direct hooks allowed",
            label,
            module,
            scans,
            runtime_stability_required_scans
        );
        return true;
    }

    ReleaseSRWLockExclusive(&runtime_stability_lock);
    return false;
}

const char* hook_debug_minhook_status_name(const MH_STATUS status) noexcept {
    switch (static_cast<int>(status)) {
        case 0: return "MH_OK";
        case 1: return "MH_ERROR_ALREADY_INITIALIZED";
        case 2: return "MH_ERROR_NOT_INITIALIZED";
        case 3: return "MH_ERROR_ALREADY_CREATED";
        case 4: return "MH_ERROR_NOT_CREATED";
        case 5: return "MH_ERROR_ENABLED";
        case 6: return "MH_ERROR_DISABLED";
        case 7: return "MH_ERROR_NOT_EXECUTABLE";
        case 8: return "MH_ERROR_UNSUPPORTED_FUNCTION";
        case 9: return "MH_ERROR_MEMORY_ALLOC";
        case 10: return "MH_ERROR_MEMORY_PROTECT";
        case 11: return "MH_ERROR_MODULE_NOT_FOUND";
        case 12: return "MH_ERROR_FUNCTION_NOT_FOUND";
        default: return "MH_UNKNOWN";
    }
}

template <typename T>
[[nodiscard]] bool install_direct_hook(
    const HMODULE module,
    const char* const export_name,
    void* const detour,
    std::atomic<T>& original_storage,
    const DiagnosticApi api
) noexcept {
    if (module == nullptr || export_name == nullptr || detour == nullptr) {
        return false;
    }
    const auto get_proc = real_get_proc_address.load(std::memory_order_acquire);
    if (get_proc == nullptr) return false;
    void* const target = reinterpret_cast<void*>(get_proc(module, export_name));
    if (target == nullptr || target == detour) return false;

    AcquireSRWLockExclusive(&direct_hook_lock);
    for (std::size_t index{}; index < direct_hook_count; ++index) {
        if (direct_hook_targets[index] == target) {
            ReleaseSRWLockExclusive(&direct_hook_lock);
            return false;
        }
    }
    wchar_t module_path[MAX_PATH]{};
    static_cast<void>(GetModuleFileNameW(
        module, module_path, static_cast<DWORD>(std::size(module_path))
    ));
    trace_event(
        "HOOKDBG MinHook attempt export=%s module=%p path=%ls target=%p detour=%p",
        export_name, module,
        module_path[0] != L'\0' ? module_path : L"<unknown>",
        target, detour
    );
    if (direct_hook_count >= direct_hook_targets.size()) {
        ReleaseSRWLockExclusive(&direct_hook_lock);
        return false;
    }

    void* trampoline{};
    const auto create_result = MH_CreateHook(target, detour, &trampoline);
    trace_event(
        "HOOKDBG MinHook create export=%s target=%p result=%d(%s) trampoline=%p",
        export_name, target, static_cast<int>(create_result),
        hook_debug_minhook_status_name(create_result), trampoline
    );
    if (create_result != MH_OK) {
        trace_event(
            "Direct detour create failed export=%s module=%p target=%p result=%d",
            export_name,
            module,
            target,
            static_cast<int>(create_result)
        );
        ReleaseSRWLockExclusive(&direct_hook_lock);
        return false;
    }

    // Publish the trampoline before enabling the detour. The hook becomes
    // reachable on another thread as soon as MH_EnableHook succeeds.
    const auto previous_original = original_storage.exchange(
        reinterpret_cast<T>(trampoline),
        std::memory_order_acq_rel
    );
    const auto enable_result = MH_EnableHook(target);
    trace_event(
        "HOOKDBG MinHook enable export=%s target=%p result=%d(%s)",
        export_name, target, static_cast<int>(enable_result),
        hook_debug_minhook_status_name(enable_result)
    );
    if (enable_result != MH_OK && enable_result != MH_ERROR_ENABLED) {
        original_storage.store(previous_original, std::memory_order_release);
        const auto remove_result = MH_RemoveHook(target);
        trace_event(
            "Direct detour enable failed export=%s module=%p target=%p result=%d(%s) remove=%d(%s) trampoline=%p NOT_PUBLISHED",
            export_name,
            module,
            target,
            static_cast<int>(enable_result),
            hook_debug_minhook_status_name(enable_result),
            static_cast<int>(remove_result),
            hook_debug_minhook_status_name(remove_result),
            trampoline
        );
        ReleaseSRWLockExclusive(&direct_hook_lock);
        return false;
    }

    direct_hook_targets[direct_hook_count++] = target;
    ReleaseSRWLockExclusive(&direct_hook_lock);
    diagnostic_note_direct_detour(api);
    trace_event("Direct detour installed export=%s target=%p detour=%p", export_name, target, detour);
    return true;
}

[[nodiscard]] bool install_direct_export_hooks(
    const bool require_runtime_stability = false
) noexcept {
    if (!minhook_initialized.load(std::memory_order_acquire)) return false;
    bool installed{};

    const auto observed_public_runtime = GetModuleHandleW(L"nvngx_dlss.dll");
    const auto public_runtime = runtime_ready_for_direct_hooks(
        observed_public_runtime,
        public_runtime_stability,
        L"nvngx_dlss.dll",
        require_runtime_stability
    ) ? observed_public_runtime : nullptr;

    if (public_runtime != nullptr) {
        diagnostic_note_runtime_loaded(DiagnosticApi::d3d11);
        diagnostic_note_runtime_loaded(DiagnosticApi::d3d12);
    }
    {
        installed |= install_direct_hook(
            public_runtime,
            "NVSDK_NGX_D3D11_Init",
            reinterpret_cast<void*>(&hook_init_d3d11),
            real_init_d3d11,
            DiagnosticApi::d3d11
        );
        installed |= install_direct_hook(
            public_runtime,
            "NVSDK_NGX_D3D12_Init",
            reinterpret_cast<void*>(&hook_init_d3d12),
            real_init_d3d12,
            DiagnosticApi::d3d12
        );
        installed |= install_direct_hook(
            public_runtime,
            "NVSDK_NGX_D3D12_Shutdown1",
            reinterpret_cast<void*>(&hook_shutdown_d3d12_1),
            real_shutdown_d3d12_1,
            DiagnosticApi::d3d12
        );
        installed |= install_direct_hook(
            public_runtime,
            "NVSDK_NGX_D3D11_CreateFeature",
            reinterpret_cast<void*>(&hook_create_d3d11),
            real_create_d3d11,
            DiagnosticApi::d3d11
        );
        installed |= install_direct_hook(
            public_runtime,
            "NVSDK_NGX_D3D11_EvaluateFeature",
            reinterpret_cast<void*>(&hook_evaluate_d3d11),
            real_evaluate_d3d11,
            DiagnosticApi::d3d11
        );
        installed |= install_direct_hook(
            public_runtime,
            "NVSDK_NGX_D3D11_EvaluateFeature_C",
            reinterpret_cast<void*>(&hook_evaluate_d3d11_c),
            real_evaluate_d3d11_c,
            DiagnosticApi::d3d11
        );
        installed |= install_direct_hook(
            public_runtime,
            "NVSDK_NGX_D3D11_ReleaseFeature",
            reinterpret_cast<void*>(&hook_release_d3d11),
            real_release_d3d11,
            DiagnosticApi::d3d11
        );
        installed |= install_direct_hook(
            public_runtime,
            "NVSDK_NGX_D3D12_CreateFeature",
            reinterpret_cast<void*>(&hook_create_d3d12),
            real_create_d3d12,
            DiagnosticApi::d3d12
        );
        installed |= install_direct_hook(
            public_runtime,
            "NVSDK_NGX_D3D12_EvaluateFeature",
            reinterpret_cast<void*>(&hook_evaluate_d3d12),
            real_evaluate_d3d12,
            DiagnosticApi::d3d12
        );
        installed |= install_direct_hook(
            public_runtime,
            "NVSDK_NGX_D3D12_EvaluateFeature_C",
            reinterpret_cast<void*>(&hook_evaluate_d3d12_c),
            real_evaluate_d3d12_c,
            DiagnosticApi::d3d12
        );
        installed |= install_direct_hook(
            public_runtime,
            "NVSDK_NGX_D3D12_ReleaseFeature",
            reinterpret_cast<void*>(&hook_release_d3d12),
            real_release_d3d12,
            DiagnosticApi::d3d12
        );
    }

    const auto observed_core_runtime = GetModuleHandleW(L"_nvngx.dll");
    const auto core_runtime = runtime_ready_for_direct_hooks(
        observed_core_runtime,
        core_runtime_stability,
        L"_nvngx.dll",
        require_runtime_stability
    ) ? observed_core_runtime : nullptr;
    if (core_runtime != nullptr) {
        diagnostic_note_runtime_loaded(DiagnosticApi::d3d11);
        diagnostic_note_runtime_loaded(DiagnosticApi::d3d12);
    }
    {
        installed |= install_direct_hook(
            core_runtime,
            "NVSDK_NGX_D3D11_Init",
            reinterpret_cast<void*>(&hook_core_init_d3d11),
            real_core_init_d3d11,
            DiagnosticApi::d3d11
        );
        installed |= install_direct_hook(
            core_runtime,
            "NVSDK_NGX_D3D12_Init",
            reinterpret_cast<void*>(&hook_core_init_d3d12),
            real_core_init_d3d12,
            DiagnosticApi::d3d12
        );
        installed |= install_direct_hook(
            core_runtime,
            "NVSDK_NGX_D3D12_Shutdown1",
            reinterpret_cast<void*>(&hook_core_shutdown_d3d12_1),
            real_core_shutdown_d3d12_1,
            DiagnosticApi::d3d12
        );
        installed |= install_direct_hook(
            core_runtime,
            "NVSDK_NGX_D3D11_CreateFeature",
            reinterpret_cast<void*>(&hook_core_create_d3d11),
            real_core_create_d3d11,
            DiagnosticApi::d3d11
        );
        installed |= install_direct_hook(
            core_runtime,
            "NVSDK_NGX_D3D11_ReleaseFeature",
            reinterpret_cast<void*>(&hook_core_release_d3d11),
            real_core_release_d3d11,
            DiagnosticApi::d3d11
        );
        installed |= install_direct_hook(
            core_runtime,
            "NVSDK_NGX_D3D12_CreateFeature",
            reinterpret_cast<void*>(&hook_core_create_d3d12),
            real_core_create_d3d12,
            DiagnosticApi::d3d12
        );
        installed |= install_direct_hook(
            core_runtime,
            "NVSDK_NGX_D3D12_EvaluateFeature",
            reinterpret_cast<void*>(&hook_core_evaluate_d3d12),
            real_core_evaluate_d3d12,
            DiagnosticApi::d3d12
        );
        installed |= install_direct_hook(
            core_runtime,
            "NVSDK_NGX_D3D12_ReleaseFeature",
            reinterpret_cast<void*>(&hook_core_release_d3d12),
            real_core_release_d3d12,
            DiagnosticApi::d3d12
        );
    }
    return installed;
}

void shutdown_direct_export_hooks() noexcept {
    if (!minhook_initialized.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    static_cast<void>(MH_DisableHook(MH_ALL_HOOKS));
    static_cast<void>(MH_Uninitialize());
    AcquireSRWLockExclusive(&direct_hook_lock);
    direct_hook_count = 0U;
    direct_hook_targets.fill(nullptr);
    ReleaseSRWLockExclusive(&direct_hook_lock);

    AcquireSRWLockExclusive(&runtime_stability_lock);
    public_runtime_stability = {};
    core_runtime_stability = {};
    ReleaseSRWLockExclusive(&runtime_stability_lock);
}

template <typename T>
void remember_original(
    std::atomic<T>& storage,
    const FARPROC original,
    const FARPROC replacement
) noexcept {
    if (original == nullptr || original == replacement) return;
    T expected{};
    storage.compare_exchange_strong(
        expected,
        reinterpret_cast<T>(original),
        std::memory_order_acq_rel
    );
}

[[nodiscard]] bool module_name(
    const HMODULE module,
    wchar_t* const output,
    const std::size_t capacity
) noexcept {
    if (module == nullptr || output == nullptr || capacity == 0U) return false;
    const auto length = GetModuleFileNameW(
        module,
        output,
        static_cast<DWORD>(capacity)
    );
    if (length == 0U || length >= capacity) return false;
    wchar_t* name = output;
    for (DWORD index{}; index < length; ++index) {
        if (output[index] == L'\\' || output[index] == L'/') {
            name = output + index + 1U;
        }
    }
    if (name != output) {
        std::memmove(output, name, (std::wcslen(name) + 1U) * sizeof(wchar_t));
    }
    return true;
}

[[nodiscard]] FARPROC replacement_for_name(
    const wchar_t* const target_name,
    const char* const function_name,
    const FARPROC original
) noexcept {
    if (target_name == nullptr || function_name == nullptr || original == nullptr) {
        return original;
    }
    const bool streamline = _wcsicmp(target_name, L"sl.interposer.dll") == 0;
    if (streamline) {
        diagnostic_note_streamline_detected();
#define CHEEKY_REPLACE_SL(export_name, storage, hook) \
        if (std::strcmp(function_name, export_name) == 0) { \
            remember_original( \
                storage, \
                original, \
                reinterpret_cast<FARPROC>(&hook) \
            ); \
            diagnostic_note_hook(DiagnosticApi::d3d12); \
            return reinterpret_cast<FARPROC>(&hook); \
        }
        CHEEKY_REPLACE_SL(
            "slEvaluateFeature",
            real_sl_evaluate_feature,
            hook_sl_evaluate_feature
        )
        CHEEKY_REPLACE_SL("slSetTag", real_sl_set_tag, hook_sl_set_tag)
        CHEEKY_REPLACE_SL(
            "slSetTagForFrame",
            real_sl_set_tag_for_frame,
            hook_sl_set_tag_for_frame
        )
        CHEEKY_REPLACE_SL(
            "slSetConstants",
            real_sl_set_constants,
            hook_sl_set_constants
        )
        CHEEKY_REPLACE_SL(
            "slGetFeatureFunction",
            real_sl_get_feature_function,
            hook_sl_get_feature_function
        )
#undef CHEEKY_REPLACE_SL
        return original;
    }

    const bool public_runtime = _wcsicmp(target_name, L"nvngx_dlss.dll") == 0;
    const bool core_runtime = _wcsicmp(target_name, L"_nvngx.dll") == 0;
    if (!public_runtime && !core_runtime) return original;

#define CHEEKY_REPLACE(export_name, storage, hook, api) \
    if (std::strcmp(function_name, export_name) == 0) { \
        remember_original(storage, original, reinterpret_cast<FARPROC>(&hook)); \
        diagnostic_note_hook(api); \
        return reinterpret_cast<FARPROC>(&hook); \
    }

    if (public_runtime) {
        CHEEKY_REPLACE("NVSDK_NGX_D3D11_Init", real_init_d3d11, hook_init_d3d11, DiagnosticApi::d3d11)
        CHEEKY_REPLACE("NVSDK_NGX_D3D12_Init", real_init_d3d12, hook_init_d3d12, DiagnosticApi::d3d12)
        CHEEKY_REPLACE("NVSDK_NGX_D3D12_Shutdown1", real_shutdown_d3d12_1, hook_shutdown_d3d12_1, DiagnosticApi::d3d12)
        CHEEKY_REPLACE("NVSDK_NGX_D3D11_CreateFeature", real_create_d3d11, hook_create_d3d11, DiagnosticApi::d3d11)
        CHEEKY_REPLACE("NVSDK_NGX_D3D11_EvaluateFeature", real_evaluate_d3d11, hook_evaluate_d3d11, DiagnosticApi::d3d11)
        CHEEKY_REPLACE("NVSDK_NGX_D3D11_EvaluateFeature_C", real_evaluate_d3d11_c, hook_evaluate_d3d11_c, DiagnosticApi::d3d11)
        CHEEKY_REPLACE("NVSDK_NGX_D3D11_ReleaseFeature", real_release_d3d11, hook_release_d3d11, DiagnosticApi::d3d11)
        CHEEKY_REPLACE("NVSDK_NGX_D3D12_CreateFeature", real_create_d3d12, hook_create_d3d12, DiagnosticApi::d3d12)
        CHEEKY_REPLACE("NVSDK_NGX_D3D12_EvaluateFeature", real_evaluate_d3d12, hook_evaluate_d3d12, DiagnosticApi::d3d12)
        CHEEKY_REPLACE("NVSDK_NGX_D3D12_EvaluateFeature_C", real_evaluate_d3d12_c, hook_evaluate_d3d12_c, DiagnosticApi::d3d12)
        CHEEKY_REPLACE("NVSDK_NGX_D3D12_ReleaseFeature", real_release_d3d12, hook_release_d3d12, DiagnosticApi::d3d12)
    } else {
        CHEEKY_REPLACE("NVSDK_NGX_D3D11_Init", real_core_init_d3d11, hook_core_init_d3d11, DiagnosticApi::d3d11)
        CHEEKY_REPLACE("NVSDK_NGX_D3D12_Init", real_core_init_d3d12, hook_core_init_d3d12, DiagnosticApi::d3d12)
        CHEEKY_REPLACE("NVSDK_NGX_D3D12_Shutdown1", real_core_shutdown_d3d12_1, hook_core_shutdown_d3d12_1, DiagnosticApi::d3d12)
        CHEEKY_REPLACE("NVSDK_NGX_D3D11_CreateFeature", real_core_create_d3d11, hook_core_create_d3d11, DiagnosticApi::d3d11)
        CHEEKY_REPLACE("NVSDK_NGX_D3D11_ReleaseFeature", real_core_release_d3d11, hook_core_release_d3d11, DiagnosticApi::d3d11)
        CHEEKY_REPLACE("NVSDK_NGX_D3D12_CreateFeature", real_core_create_d3d12, hook_core_create_d3d12, DiagnosticApi::d3d12)
        CHEEKY_REPLACE("NVSDK_NGX_D3D12_EvaluateFeature", real_core_evaluate_d3d12, hook_core_evaluate_d3d12, DiagnosticApi::d3d12)
        CHEEKY_REPLACE("NVSDK_NGX_D3D12_ReleaseFeature", real_core_release_d3d12, hook_core_release_d3d12, DiagnosticApi::d3d12)
    }
#undef CHEEKY_REPLACE
    return original;
}

[[nodiscard]] FARPROC replacement_for_module(
    const HMODULE target,
    const char* const function_name,
    const FARPROC original
) noexcept {
    std::array<wchar_t, MAX_PATH> name{};
    return module_name(target, name.data(), name.size())
        ? replacement_for_name(name.data(), function_name, original)
        : original;
}

FARPROC WINAPI hook_get_proc_address(
    const HMODULE target,
    const LPCSTR name
) {
    static std::atomic<std::uint32_t> gpa_calls{};
    const auto call = gpa_calls.fetch_add(1U, std::memory_order_relaxed);
    const auto ordinal = reinterpret_cast<std::uintptr_t>(name) <= 0xFFFFU;
    if (call < 128U) {
        trace_event("HOOKDBG GetProcAddress[%u] ENTER module=%p name=%s ordinal=%s tid=%lu", call, target, ordinal ? "<ordinal>" : (name != nullptr ? name : "<null>"), ordinal ? "yes" : "no", static_cast<unsigned long>(GetCurrentThreadId()));
    }
    const auto original = real_get_proc_address.load(std::memory_order_acquire);
    if (original == nullptr) {
        if (call < 128U) trace_event("HOOKDBG GetProcAddress[%u] original=NULL", call);
        return nullptr;
    }
    const auto resolved = original(target, name);
    if (ordinal) {
        if (call < 128U) trace_event("HOOKDBG GetProcAddress[%u] EXIT ordinal resolved=%p", call, resolved);
        return resolved;
    }
    const auto replacement = replacement_for_module(target, name, resolved);
    if (call < 128U || replacement != resolved) {
        trace_event("HOOKDBG GetProcAddress[%u] EXIT name=%s resolved=%p returned=%p replaced=%s", call, name != nullptr ? name : "<null>", resolved, replacement, replacement != resolved ? "YES" : "no");
    }
    return replacement;
}

[[nodiscard]] bool patch_slot(void** const slot, void* const replacement) noexcept {
    if (slot == nullptr || replacement == nullptr || *slot == replacement) {
        return false;
    }
    AcquireSRWLockExclusive(&patch_lock);
    for (std::size_t index{}; index < patched_slot_count; ++index) {
        if (patched_slots[index].slot == slot) {
            ReleaseSRWLockExclusive(&patch_lock);
            return false;
        }
    }
    if (patched_slot_count >= patched_slots.size()) {
        ReleaseSRWLockExclusive(&patch_lock);
        return false;
    }
    DWORD old_protection{};
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old_protection)) {
        ReleaseSRWLockExclusive(&patch_lock);
        return false;
    }
    patched_slots[patched_slot_count++] = {slot, *slot};
    InterlockedExchangePointer(
        reinterpret_cast<PVOID volatile*>(slot),
        replacement
    );
    DWORD ignored{};
    VirtualProtect(slot, sizeof(*slot), old_protection, &ignored);
    ReleaseSRWLockExclusive(&patch_lock);
    return true;
}

[[nodiscard]] bool is_get_proc_candidate(const HMODULE module) noexcept {
    if (module == GetModuleHandleW(nullptr)) return true;
    std::array<wchar_t, MAX_PATH> name{};
    if (!module_name(module, name.data(), name.size())) return false;
    return _wcsnicmp(name.data(), L"sl.", 3U) == 0 ||
        _wcsnicmp(name.data(), L"sl_", 3U) == 0;
}

[[nodiscard]] bool is_interception_candidate(const HMODULE module) noexcept {
    if (module == GetModuleHandleW(nullptr)) return true;
    std::array<wchar_t, MAX_PATH> name{};
    if (!module_name(module, name.data(), name.size())) return false;
    return _wcsicmp(name.data(), L"sl.interposer.dll") == 0 ||
        _wcsicmp(name.data(), L"sl.common.dll") == 0 ||
        _wcsicmp(name.data(), L"sl.dlss.dll") == 0 ||
        _wcsicmp(name.data(), L"sl.dlss_d.dll") == 0 ||
        _wcsicmp(name.data(), L"sl.dlss_nr.dll") == 0;
}

[[nodiscard]] HMODULE imported_module_handle(const char* const name) noexcept {
    if (name == nullptr) return nullptr;
    std::array<wchar_t, MAX_PATH> wide{};
    const auto length = MultiByteToWideChar(
        CP_ACP,
        0,
        name,
        -1,
        wide.data(),
        static_cast<int>(wide.size())
    );
    return length <= 0 ? nullptr : GetModuleHandleW(wide.data());
}

[[nodiscard]] bool patch_module_imports(const HMODULE module) noexcept {
    if (module == nullptr) return false;
    std::array<wchar_t, MAX_PATH> owner_name{};
    static_cast<void>(module_name(
        module,
        owner_name.data(),
        owner_name.size()
    ));
    auto* const image = reinterpret_cast<std::byte*>(module);
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const auto* const headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        image + dos->e_lfanew
    );
    if (headers->Signature != IMAGE_NT_SIGNATURE ||
        headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return false;
    }
    const auto& imports = headers->OptionalHeader.DataDirectory[
        IMAGE_DIRECTORY_ENTRY_IMPORT
    ];
    if (imports.VirtualAddress == 0U || imports.Size == 0U) return false;

    // Do not patch the host executable's GetProcAddress import. Routing the
    // process-wide resolver through an add-on wrapper is unnecessarily invasive
    // and can perturb startup even when every lookup is passed through unchanged.
    // Runtime polling plus direct NGX and Streamline interception cover discovery.
    const bool patch_get_proc = false;
    bool patched{};
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        image + imports.VirtualAddress
    );
    for (; descriptor->Name != 0U; ++descriptor) {
        if (descriptor->OriginalFirstThunk == 0U ||
            descriptor->FirstThunk == 0U) continue;
        const auto* const imported_name = reinterpret_cast<const char*>(
            image + descriptor->Name
        );
        std::array<wchar_t, MAX_PATH> target_name{};
        MultiByteToWideChar(
            CP_ACP,
            0,
            imported_name,
            -1,
            target_name.data(),
            static_cast<int>(target_name.size())
        );
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            image + descriptor->OriginalFirstThunk
        );
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            image + descriptor->FirstThunk
        );
        const auto target_module = imported_module_handle(imported_name);
        for (; names->u1.AddressOfData != 0U; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto* const import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                image + names->u1.AddressOfData
            );
            const auto* const function_name = reinterpret_cast<const char*>(
                import->Name
            );
            auto* const slot = reinterpret_cast<void**>(&slots->u1.Function);
            if (patch_get_proc && _stricmp(function_name, "GetProcAddress") == 0) {
                const auto current = reinterpret_cast<GetProcAddressFn>(*slot);
                if (current != &hook_get_proc_address) {
                    GetProcAddressFn expected{};
                    real_get_proc_address.compare_exchange_strong(
                        expected,
                        current,
                        std::memory_order_acq_rel
                    );
                    const auto changed = patch_slot(
                        slot,
                        reinterpret_cast<void*>(&hook_get_proc_address)
                    );
                    patched |= changed;
                    if (changed) {
                        trace_event(
                            "IAT patched owner=%ls import=%s!GetProcAddress slot=%p old=%p new=%p",
                            owner_name.data(),
                            imported_name,
                            slot,
                            reinterpret_cast<void*>(current),
                            reinterpret_cast<void*>(&hook_get_proc_address)
                        );
                    }
                }
                continue;
            }
            const auto replacement = target_module != nullptr
                ? replacement_for_module(
                    target_module,
                    function_name,
                    reinterpret_cast<FARPROC>(*slot)
                )
                : replacement_for_name(
                    target_name.data(),
                    function_name,
                    reinterpret_cast<FARPROC>(*slot)
                );
            if (replacement != reinterpret_cast<FARPROC>(*slot)) {
                const auto previous = reinterpret_cast<void*>(*slot);
                const auto changed = patch_slot(
                    slot,
                    reinterpret_cast<void*>(replacement)
                );
                patched |= changed;
                if (changed) {
                    trace_event(
                        "IAT patched owner=%ls import=%s!%s slot=%p old=%p new=%p",
                        owner_name.data(),
                        imported_name,
                        function_name,
                        slot,
                        previous,
                        reinterpret_cast<void*>(replacement)
                    );
                }
            }
        }
    }
    return patched;
}

[[nodiscard]] bool patch_loaded_modules() noexcept {
    std::array<HMODULE, 1024> modules{};
    DWORD required{};
    if (!K32EnumProcessModules(
            GetCurrentProcess(),
            modules.data(),
            static_cast<DWORD>(sizeof(modules)),
            &required
        )) return false;
    const auto count = (std::min)(
        modules.size(),
        static_cast<std::size_t>(required / sizeof(HMODULE))
    );
    bool patched{};
    for (std::size_t index{}; index < count; ++index) {
        const bool candidate = streamline_loaded()
            ? modules[index] == GetModuleHandleW(nullptr)
            : is_interception_candidate(modules[index]);
        if (candidate) {
            patched |= patch_module_imports(modules[index]);
        }
    }
    return patched;
}

void restore_patched_slots() noexcept;

DWORD WINAPI interception_worker(void*) noexcept {
    const auto event = stop_event.load(std::memory_order_acquire);
    bool announced{};
    bool streamline_announced{};
    std::uint32_t worker_tick{};
    while (event != nullptr &&
           WaitForSingleObject(event, 250U) == WAIT_TIMEOUT) {
        drain_hook_debug_loader_events();
        if (worker_tick < 20U) trace_event("HOOKDBG worker tick=%u begin tid=%lu", worker_tick, static_cast<unsigned long>(GetCurrentThreadId()));
        if (streamline_loaded()) {
            if (!streamline_inline_mode.load(std::memory_order_acquire) &&
                !streamline_inline_install_failed.load(
                    std::memory_order_acquire
                )) {
                // Streamline commonly loads after ReShade add-ons. Wait until
                // every mandatory export is present, then install exactly once.
                if (install_streamline_inline_hooks()) {
                    if (!streamline_announced) {
                        streamline_announced = true;
                        log_info("Late-loaded Streamline interception armed.");
                    }
                }
            }
        }

        if (worker_tick < 20U) trace_event("HOOKDBG worker tick=%u patch scan begin", worker_tick);
        const bool patched = patch_loaded_modules();
        if (worker_tick < 20U) trace_event("HOOKDBG worker tick=%u patch scan end patched=%s direct scan begin", worker_tick, patched ? "yes" : "no");
        const bool detoured = install_direct_export_hooks(true);
        if (worker_tick < 20U) trace_event("HOOKDBG worker tick=%u direct scan end detoured=%s", worker_tick, detoured ? "yes" : "no");
        if ((patched || detoured) && !announced) {
            announced = true;
            log_info("NGX D3D11/D3D12 interception armed.");
        }
        if (worker_tick < 20U) trace_event("HOOKDBG worker tick=%u end", worker_tick);
        ++worker_tick;
    }
    return 0U;
}

void restore_patched_slots() noexcept {
    AcquireSRWLockExclusive(&patch_lock);
    for (std::size_t index = patched_slot_count; index > 0U; --index) {
        const auto& patch = patched_slots[index - 1U];
        MEMORY_BASIC_INFORMATION memory{};
        if (patch.slot == nullptr ||
            VirtualQuery(patch.slot, &memory, sizeof(memory)) == 0U ||
            memory.State != MEM_COMMIT) continue;
        DWORD old_protection{};
        if (!VirtualProtect(
                patch.slot,
                sizeof(*patch.slot),
                PAGE_READWRITE,
                &old_protection
            )) continue;
        InterlockedExchangePointer(
            reinterpret_cast<PVOID volatile*>(patch.slot),
            patch.original
        );
        DWORD ignored{};
        VirtualProtect(
            patch.slot,
            sizeof(*patch.slot),
            old_protection,
            &ignored
        );
    }
    patched_slot_count = 0U;
    patched_slots.fill({});
    ReleaseSRWLockExclusive(&patch_lock);
}

}  // namespace

void note_d3d12_command_list_submission(
    ID3D12CommandQueue* const queue,
    ID3D12GraphicsCommandList* const command_list
) noexcept {
    note_d3d12_command_list_submission_impl(queue, command_list);
}

void note_d3d12_present(ID3D12CommandQueue* const queue) noexcept {
    note_d3d12_present_impl(queue);
}

bool install_early_loader_interception() noexcept {
    real_get_proc_address.store(&GetProcAddress, std::memory_order_release);
    if (early_loader_interception.load(std::memory_order_acquire)) {
        return true;
    }
    const auto patched = patch_module_imports(GetModuleHandleW(nullptr));
    early_loader_interception.store(patched, std::memory_order_release);
    return patched;
}

void uninstall_early_loader_interception() noexcept {
    restore_patched_slots();
    early_loader_interception.store(false, std::memory_order_release);
}

bool start_interception() noexcept {
    if (started.exchange(true, std::memory_order_acq_rel)) return true;
    real_get_proc_address.store(&GetProcAddress, std::memory_order_release);
    trace_event("Interception startup begin");
    install_hook_debug_diagnostics();
    trace_event(
        "Early executable interception active=%s",
        early_loader_interception.load(std::memory_order_acquire)
            ? "yes"
            : "no"
    );
    const auto minhook_result = MH_Initialize();
    trace_event(
        "MinHook initialize result=%d",
        static_cast<int>(minhook_result)
    );
    minhook_initialized.store(
        minhook_result == MH_OK ||
            minhook_result == MH_ERROR_ALREADY_INITIALIZED,
        std::memory_order_release
    );

    if (streamline_loaded()) {
        trace_event(
            "Arming Streamline persistent hooks alongside direct NGX hooks"
        );
        if (!install_streamline_inline_hooks()) {
            trace_event(
                streamline_inline_install_failed.load(
                    std::memory_order_acquire
                )
                    ? "Streamline inline hook installation failed; no runtime retry"
                    : "Streamline exports incomplete; worker will retry installation"
            );
        }
    }
    trace_event(
        "HOOKDBG GetProcAddress IAT interception disabled; Streamline interception remains enabled"
    );
    const auto patched = install_early_loader_interception();
    trace_event(
        "Initial executable IAT patch complete patched=%s getProcAddress=disabled",
        patched ? "yes" : "no"
    );
    const auto detoured = install_direct_export_hooks();
    trace_event("Initial direct hook scan complete installed=%s", detoured ? "yes" : "no");
    const auto event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        shutdown_direct_export_hooks();
        restore_patched_slots();
        started.store(false, std::memory_order_release);
        uninstall_hook_debug_diagnostics();
        return false;
    }
    stop_event.store(event, std::memory_order_release);
    const auto thread = CreateThread(
        nullptr,
        0U,
        &interception_worker,
        nullptr,
        0U,
        nullptr
    );
    if (thread == nullptr) {
        stop_event.store(nullptr, std::memory_order_release);
        CloseHandle(event);
        shutdown_direct_export_hooks();
        restore_patched_slots();
        started.store(false, std::memory_order_release);
        uninstall_hook_debug_diagnostics();
        return false;
    }
    worker_thread.store(thread, std::memory_order_release);
    return true;
}

void stop_interception() noexcept {
    if (!started.exchange(false, std::memory_order_acq_rel)) return;
    const auto event = stop_event.exchange(nullptr, std::memory_order_acq_rel);
    const auto thread = worker_thread.exchange(nullptr, std::memory_order_acq_rel);
    if (event != nullptr) SetEvent(event);
    if (thread != nullptr) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
    if (event != nullptr) CloseHandle(event);
    restore_streamline_options();
    if (streamline_hook_lock_ready.load(std::memory_order_acquire)) {
        uninstall_streamline_inline_hooks();
    }
    shutdown_direct_export_hooks();
    release_d3d11_dlss_timers();
    release_d3d11_peripheral_dlaa_resources();
    release_d3d12_nr_timers();
    uninstall_early_loader_interception();
    uninstall_hook_debug_diagnostics();
}

}  // namespace cheeky::foveated_dlss
