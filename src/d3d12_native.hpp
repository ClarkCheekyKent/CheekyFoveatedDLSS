#pragma once
#include <utility>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>

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
}
