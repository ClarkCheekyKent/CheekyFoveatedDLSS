#pragma once
#include <Windows.h>

namespace cheeky::foveated_dlss {
// Patch an interface slot, never its implementation: SteamVR shares forwarding
// thunks between unrelated interfaces, including methods with different ABIs.
struct OpenVRVtableHook {
    void** slot{};
    void* original{};
    void* replacement{};

    bool install(void** entry, void* detour, void** trampoline) noexcept {
        DWORD protection{};
        if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &protection)) return false;
        original = *entry;
        replacement = detour;
        // Publish the original before making the detour callable.
        *trampoline = original;
        slot = entry;
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(entry), detour);
        DWORD unused{};
        VirtualProtect(entry, sizeof(void*), protection, &unused);
        return true;
    }
    void restore() noexcept {
        DWORD protection{};
        if (!slot || !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) return;
        // Do not overwrite a hook installed by somebody else after ours.
        InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot), original, replacement);
        DWORD unused{};
        VirtualProtect(slot, sizeof(void*), protection, &unused);
    }
};
}
