#include "nr_runtime_module.hpp"
#include "runtime_search.hpp"
#include "runtime.hpp"
#include <cstddef>
#include <cstring>
#include <mutex>
namespace cheeky::foveated_dlss {
namespace {
using GetModuleFileNameWFn=DWORD(WINAPI*)(HMODULE,LPWSTR,DWORD);
HMODULE caller_module{};
GetModuleFileNameWFn original_module_name{};
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
    if (module == caller_module && output != nullptr && capacity != 0U) {
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
    return original_module_name == nullptr
        ? 0U
        : original_module_name(module, output, capacity);
}

[[nodiscard]] bool addon_directory(
    std::array<wchar_t, 32768U>& directory
) noexcept {
    HMODULE addon{};
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&load_nr_runtime_module),
            &addon
        )) return false;
    caller_module = addon;
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

}
NrRuntimeModule load_nr_runtime_module() noexcept {
    static std::mutex mutex;
    static NrRuntimeModule cached;
    std::lock_guard lock(mutex);
    if(cached.module)return cached;
    if(!addon_directory(cached.directory)){cached.error=ERROR_PATH_NOT_FOUND;return cached;}
    std::array<wchar_t, 32768U> executable{};
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    auto* slash = length && length < executable.size() ? std::wcsrchr(executable.data(), L'\\') : nullptr;
    if (slash) *slash = L'\0'; else executable[0] = L'\0';
    const auto loaded = load_runtime_library(L"nvngx_dlssnr.dll", cached.directory.data(), executable.data(),
        [](const wchar_t* path, DWORD error) {
            trace_event("DLSS-NR runtime search path=%ls error=%lu", path, static_cast<unsigned long>(error));
        });
    cached.module = loaded.module;
    if (cached.module == nullptr) {
        cached.error=loaded.error; return cached;
    }

    void* original_get_module_file_name{};
    if (!patch_named_import(
            cached.module,
            "GetModuleFileNameW",
            reinterpret_cast<void*>(&hook_nr_get_module_file_name),
            &original_get_module_file_name
        )) {
        cached.module=nullptr;cached.error=ERROR_PROC_NOT_FOUND;
        trace_event("DLSS-NR runtime identity import patch failed");
        return cached;
    }
    original_module_name = reinterpret_cast<GetModuleFileNameWFn>(
        original_get_module_file_name
    );

    // The identity hook and all feature callbacks are process-resident.
    HMODULE pinned{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(cached.module),&pinned);
    cached.error=0;
    return cached;
}
}
