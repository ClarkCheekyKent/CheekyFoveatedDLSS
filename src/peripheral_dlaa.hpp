#pragma once

#include "backend.hpp"

namespace cheeky::foveated_dlss {

struct PeripheralDlaaRequest {
    DlssViewId view_id{};
    ID3D12GraphicsCommandList* command_list{};
    ID3D12Resource* color{};
    ID3D12Resource* depth{};
    ID3D12Resource* motion_vectors{};
    ID3D12Resource* output_template{};
    ID3D12Resource* output_override{};
    D3D12_RESOURCE_STATES color_state{
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    };
    D3D12_RESOURCE_STATES depth_state{
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    };
    D3D12_RESOURCE_STATES motion_state{
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    };
    D3D12_RESOURCE_STATES output_state{
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    };
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    std::uint32_t source_output_width{};
    std::uint32_t source_output_height{};
    float scale{1.0F};
    std::uint32_t preset{5U};
    std::uint32_t color_base_x{};
    std::uint32_t color_base_y{};
    std::uint32_t depth_base_x{};
    std::uint32_t depth_base_y{};
    std::uint32_t mv_base_x{};
    std::uint32_t mv_base_y{};
    bool motion_vectors_output_space{};
    float motion_vector_scale_x{1.0F};
    float motion_vector_scale_y{1.0F};
    bool depth_inverted{};
    bool reset{};
    std::uint32_t create_flags{};
    NgxParameters* parameters{};
    D3D12BackendCallbacks callbacks{};
};

struct PeripheralDlaaResources {
    ID3D12Resource* color{};
    ID3D12Resource* depth{};
    ID3D12Resource* output{};
    ID3D12Resource* motion_vectors{};
    std::uint32_t color_base_x{};
    std::uint32_t color_base_y{};
    std::uint32_t depth_base_x{};
    std::uint32_t depth_base_y{};
    std::uint32_t mv_base_x{};
    std::uint32_t mv_base_y{};
    std::uint32_t working_width{};
    std::uint32_t working_height{};
    bool downsampled_color{};
    bool downsampled_depth{};
    bool converted_motion{};
    D3D12_RESOURCE_STATES output_restore_state{
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    };
};

struct PeripheralDlaaDimensions {
    std::uint32_t width{};
    std::uint32_t height{};
};

[[nodiscard]] PeripheralDlaaDimensions peripheral_dlaa_dimensions(
    std::uint32_t render_width,
    std::uint32_t render_height,
    float scale
) noexcept;

[[nodiscard]] DlssViewId peripheral_dlaa_view_id(DlssViewId view_id) noexcept;

[[nodiscard]] bool prepare_peripheral_dlaa_resources(
    const PeripheralDlaaRequest& request,
    PeripheralDlaaResources& resources
) noexcept;

[[nodiscard]] bool evaluate_peripheral_dlaa_ngx(
    const PeripheralDlaaRequest& request,
    PeripheralDlaaResources& resources,
    NgxResult& result,
    D3D12BackendTiming* timing = nullptr
) noexcept;

void finish_peripheral_dlaa_motion_read(
    ID3D12GraphicsCommandList* command_list,
    const PeripheralDlaaResources& resources
) noexcept;

void finish_peripheral_dlaa_write(
    ID3D12GraphicsCommandList* command_list,
    const PeripheralDlaaResources& resources
) noexcept;

void restore_peripheral_dlaa_output(
    ID3D12GraphicsCommandList* command_list,
    const PeripheralDlaaResources& resources
) noexcept;

void release_peripheral_dlaa_view(DlssViewId view_id) noexcept;
void release_peripheral_dlaa_resources() noexcept;
void note_peripheral_dlaa_submission(
    ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* command_list
) noexcept;
void collect_peripheral_dlaa_resources() noexcept;

}  // namespace cheeky::foveated_dlss
