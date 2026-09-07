#pragma once

#include <d3d12.h>
#include <cstdint>
#include <mutex>

namespace cheeky::foveated_dlss {
// Object private data is forwarded by D3D12 interposers, unlike COM addresses.
// Do not use a pointer value as the token: addresses can be reused after release.
inline std::uint64_t d3d12_submission_identity(ID3D12Object* object, bool create = false) noexcept {
    if (!object) return 0;
    static constexpr GUID key{0x7ae7d193, 0xac24, 0x4ef7, {0x95, 0x52, 0x4c, 0xda, 0xb8, 0xa0, 0x61, 0xf3}};
    static std::mutex mutex;
    static std::uint64_t next{};
    std::lock_guard lock(mutex);
    std::uint64_t identity{};
    UINT size = sizeof(identity);
    if (SUCCEEDED(object->GetPrivateData(key, &size, &identity)) && size == sizeof(identity) && identity)
        return identity;
    if (!create) return 0;
    identity = ++next;
    return SUCCEEDED(object->SetPrivateData(key, sizeof(identity), &identity)) ? identity : 0;
}
} // namespace cheeky::foveated_dlss
