#pragma once

#include "backend.hpp"

namespace cheeky::foveated_dlss {

using NgxD3D12InitFn = NgxResult (*)(
    unsigned long long,
    const wchar_t*,
    ID3D12Device*,
    const void*,
    std::uint32_t
);
using NgxD3D12InitExtFn = NgxResult (*)(
    unsigned long long,
    const wchar_t*,
    ID3D12Device*,
    int,
    const void*
);
using NgxD3D12AllocateParametersFn = NgxResult (*)(NgxParameters**);
using NgxD3D12Shutdown1Fn = NgxResult (*)(ID3D12Device*);
using NgxGetApplicationIdFn = unsigned long long (*)();
using NgxGetApiVersionFn = std::uint32_t (*)();

struct D3D11TransportNgx {
    HMODULE runtime_module{};
    NgxD3D12InitExtFn init_ext{};
    NgxD3D12AllocateParametersFn allocate_parameters{};
    NgxD3D12Shutdown1Fn shutdown{};
    NgxGetApplicationIdFn get_application_id{};
    NgxGetApiVersionFn get_api_version{};
    D3D12BackendCallbacks backend{};
};

void remember_d3d11_ngx_init(
    unsigned long long application_id,
    const wchar_t* application_data_path,
    const void* feature_common_info,
    std::uint32_t sdk_version
) noexcept;

// Returns true only when the D3D12 transport owned the frame. A false return
// leaves the game parameter block untouched so the caller can use native DX11.
[[nodiscard]] bool evaluate_d3d11_via_d3d12(
    ID3D11DeviceContext* context,
    const NgxHandle* game_handle,
    const NgxParameters* parameters,
    const Settings& settings,
    const D3D11TransportNgx& ngx,
    NgxResult& result
) noexcept;

void release_d3d11_transport_view(const NgxHandle* game_handle) noexcept;
void release_d3d11_d3d12_transport() noexcept;

}  // namespace cheeky::foveated_dlss
