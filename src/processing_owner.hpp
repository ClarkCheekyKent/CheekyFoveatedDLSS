#pragma once
#include <Windows.h>
#include <cstdio>

namespace cheeky::foveated_dlss {
// The existence of this process-specific kernel object is the ownership claim.
// Do not take thread-affine mutex ownership: initialization and teardown may
// occur on different host threads.
inline HANDLE claim_processing_owner() noexcept {
    wchar_t name[96]{};
    swprintf_s(name, L"Local\\CheekyFoveatedDLSS.ProcessingOwner.%lu", GetCurrentProcessId());
    const auto handle = CreateMutexW(nullptr, FALSE, name);
    const auto error = GetLastError();
    if (handle && error == ERROR_ALREADY_EXISTS) { CloseHandle(handle); return nullptr; }
    return handle;
}
}
