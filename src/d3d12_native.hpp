#pragma once
#include <utility>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <mutex>
#include <new>

namespace cheeky::foveated_dlss {
// Public proxy-unwrapping interfaces for proxies which implement them. Older
// R.E.A.L. VR wrappers need separate observer hooks instead. QueryInterface
// owns a reference on every returned object.
// https://github.com/crosire/reshade/blob/main/source/com_utils.hpp
// https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md
inline constexpr GUID d3d12_unwrap_reshade{0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
inline constexpr GUID d3d12_unwrap_streamline{0xadec44e2,0x61f0,0x45c3,{0xad,0x9f,0x1b,0x37,0x37,0x92,0x84,0xff}};

template<class T>
Microsoft::WRL::ComPtr<T> native_d3d12_interface(T* object) noexcept {
    Microsoft::WRL::ComPtr<T> current = object;
    std::array<T*, 8> seen{};
    for (unsigned depth = 0; current && depth < seen.size(); ++depth) {
        seen[depth] = current.Get();
        Microsoft::WRL::ComPtr<T> next;
        for (const auto& iid : {d3d12_unwrap_reshade, d3d12_unwrap_streamline}) {
            Microsoft::WRL::ComPtr<IUnknown> unwrapped;
            if (SUCCEEDED(current->QueryInterface(iid, reinterpret_cast<void**>(unwrapped.GetAddressOf()))) &&
                unwrapped && SUCCEEDED(unwrapped.As(&next)) && next.Get() != current.Get()) break;
            next.Reset();
        }
        if (!next) return current; // Native object or an unsupported proxy.
        for (unsigned i = 0; i <= depth; ++i)
            if (seen[i] == next.Get()) return {}; // Malformed cyclic proxy chain.
        current = std::move(next);
    }
    return {}; // Do not trust a chain that exceeds the bounded traversal.
}
// GetDevice can return a facade while a resource returns the underlying device.
// Resolve the documented unwrap interfaces, then COM's canonical IUnknown.
// Never infer device equality from adapter LUIDs (separate devices may share one).
inline Microsoft::WRL::ComPtr<ID3D12Device> canonical_d3d12_device(ID3D12Device* device) noexcept {
    auto native = native_d3d12_interface(device);
    Microsoft::WRL::ComPtr<IUnknown> identity;
    Microsoft::WRL::ComPtr<ID3D12Device> result;
    if (!native || FAILED(native.As(&identity)) || FAILED(identity.As(&result))) return {};
    return result;
}
namespace d3d12_device_detail {
inline constexpr GUID identity_key{0x61a85e71, 0x7fbb, 0x4599, {0x9d, 0x4b, 0xab, 0x71, 0x98, 0x6e, 0x36, 0x42}};
class Identity final : public IUnknown {
    std::atomic<ULONG> references{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --references; if (!left) delete this; return left;
    }
};
inline Microsoft::WRL::ComPtr<IUnknown> identity(ID3D12Device* device) noexcept {
    Microsoft::WRL::ComPtr<IUnknown> token;
    UINT bytes = sizeof(IUnknown*);
    if (SUCCEEDED(device->GetPrivateData(identity_key, &bytes, token.GetAddressOf())) && token) return token;
    // A device can outlive a host adapter; its Release callback must stay valid.
    HMODULE resident{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&identity), &resident)) return {};
    token.Attach(new (std::nothrow) Identity);
    if (!token || FAILED(device->SetPrivateDataInterface(identity_key, token.Get()))) return {};
    return token;
}
}
inline bool same_d3d12_device(ID3D12Device* a, ID3D12Device* b) noexcept {
    const auto first = canonical_d3d12_device(a), second = canonical_d3d12_device(b);
    Microsoft::WRL::ComPtr<IUnknown> first_identity, second_identity;
    if (!first || !second || FAILED(first.As(&first_identity)) || FAILED(second.As(&second_identity))) return false;
    if (first_identity.Get() == second_identity.Get()) return true;
    // Older opaque proxies have their own IUnknown and no unwrap IID, but
    // forward device-private data. A shared, device-owned COM token proves
    // that relationship without guessing, retaining devices, or pointer casts.
    // Distinct devices receive distinct live tokens even on the same adapter.
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    const auto first_token = d3d12_device_detail::identity(first.Get());
    const auto second_token = d3d12_device_detail::identity(second.Get());
    return first_token && second_token && first_token.Get() == second_token.Get();
}
}
