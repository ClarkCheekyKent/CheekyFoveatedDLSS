#pragma once

#include "frame_contract.hpp"
#include "ngx_abi.hpp"
#include "settings.hpp"

#include <cstdint>

namespace cheeky::foveated_dlss {

enum class DlssNrRoute : std::uint32_t {
    none,
    d3d12_native,
    d3d11_transport,
    streamline,
};

enum class DlssNrState : std::uint32_t {
    waiting,
    disabled,
    runtime_missing,
    runtime_failed,
    unsupported_resources,
    feature_failed,
    evaluation_failed,
    active,
};

struct DlssNrFrame {
    DlssViewId view_id{};
    DlssNrRoute route{DlssNrRoute::none};
    ID3D12GraphicsCommandList* command_list{};
    ID3D12Resource* color{};
    D3D12_RESOURCE_STATES color_state{D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    ID3D12Resource* depth{};
    ID3D12Resource* motion_vectors{};
    std::uint32_t input_width{};
    std::uint32_t input_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t depth_base_x{};
    std::uint32_t depth_base_y{};
    std::uint32_t depth_width{};
    std::uint32_t depth_height{};
    std::uint32_t motion_base_x{};
    std::uint32_t motion_base_y{};
    std::uint32_t motion_width{};
    std::uint32_t motion_height{};
    float motion_scale_x{1.0F};
    float motion_scale_y{1.0F};
    bool depth_inverted{};
    bool reset{};
    std::uint32_t create_flags{};
    std::uint32_t color_base_x{};
    std::uint32_t color_base_y{};
    bool color_is_region{};
};

struct DlssNrGeometry {
    std::uint32_t base_x{};
    std::uint32_t base_y{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t working_width{};
    std::uint32_t working_height{};
};

[[nodiscard]] bool calculate_dlss_nr_geometry(
    const Settings& settings,
    std::uint32_t output_width,
    std::uint32_t output_height,
    DlssNrGeometry& geometry
) noexcept;

struct DlssNrSnapshot {
    DlssNrState state{DlssNrState::waiting};
    DlssNrRoute route{DlssNrRoute::none};
    std::uint64_t candidate_calls{};
    std::uint64_t evaluation_calls{};
    std::uint64_t failed_calls{};
    NgxResult last_result{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t region_base_x{};
    std::uint32_t region_base_y{};
    std::uint32_t region_width{};
    std::uint32_t region_height{};
    std::uint32_t working_width{};
    std::uint32_t working_height{};
    std::uint64_t intermediate_vram_bytes{};
};

[[nodiscard]] bool evaluate_dlss_nr(
    const DlssNrFrame& frame,
    const Settings& settings
) noexcept;

void release_dlss_nr_view(DlssViewId view_id) noexcept;
void release_dlss_nr_resources() noexcept;
void reset_dlss_nr() noexcept;

[[nodiscard]] DlssNrSnapshot dlss_nr_snapshot() noexcept;
[[nodiscard]] const char* dlss_nr_state_name(DlssNrState state) noexcept;
[[nodiscard]] const char* dlss_nr_route_name(DlssNrRoute route) noexcept;

}  // namespace cheeky::foveated_dlss
