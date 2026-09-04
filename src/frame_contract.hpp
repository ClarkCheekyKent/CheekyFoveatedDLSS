#pragma once

#include <d3d12.h>

#include <cstdint>

namespace cheeky::foveated_dlss {

using DlssViewId = std::uint64_t;

struct DlssFrameContract {
    DlssViewId view_id{};
    std::uint32_t feature_id{1U};

    std::uint32_t render_width{};
    std::uint32_t render_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};

    std::uint32_t color_base_x{};
    std::uint32_t color_base_y{};
    std::uint32_t depth_base_x{};
    std::uint32_t depth_base_y{};
    std::uint32_t mv_base_x{};
    std::uint32_t mv_base_y{};
    std::uint32_t output_base_x{};
    std::uint32_t output_base_y{};

    bool motion_vectors_low_res{};
    bool depth_inverted{};
    bool reset{};
    bool preserve_history_on_crop_move{};

    std::uint32_t create_flags{};
    std::uint32_t perf_quality{};

    float jitter_x{};
    float jitter_y{};
    float motion_vector_scale_x{};
    float motion_vector_scale_y{};
    float pre_exposure{};
    float exposure_scale{};
};

struct D3D12DlssInputs {
    ID3D12Resource* color{};
    ID3D12Resource* depth{};
    ID3D12Resource* motion_vectors{};
    ID3D12Resource* exposure{};
    ID3D12Resource* output{};

    std::uint32_t color_base_x{};
    std::uint32_t color_base_y{};
    std::uint32_t depth_base_x{};
    std::uint32_t depth_base_y{};
    std::uint32_t mv_base_x{};
    std::uint32_t mv_base_y{};
    std::uint32_t output_base_x{};
    std::uint32_t output_base_y{};
};

}  // namespace cheeky::foveated_dlss
