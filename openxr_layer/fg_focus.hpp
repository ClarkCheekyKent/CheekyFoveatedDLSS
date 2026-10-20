#pragma once
#include "../shared/cheeky_fg_bridge.hpp"
#include "fg_matrices.hpp"
#include "../shared/fg_compact.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
namespace cheeky::fg {
using Microsoft::WRL::ComPtr;
struct FocusRect {unsigned x{},y{},w{},h{};float feather{};bool active{};};
struct GpuTimings;
DXGI_FORMAT depth_srv_format(DXGI_FORMAT) noexcept;
DXGI_FORMAT motion_srv_format(DXGI_FORMAT) noexcept;
struct FocusInput {
    ComPtr<ID3D12Resource> color,depth,motion,corrected_motion,recentered_color;
    unsigned output_width{},output_height{};
    bool borrowed_guides{};
};
struct FocusPipeline {
    struct FinalOutput {ID3D12Resource* center{};FocusRect region;unsigned width{},height{};bool center_direct{},linear_sdr{};unsigned shading_mode{1};};
    DXGI_FORMAT color_format{DXGI_FORMAT_R16G16B16A16_FLOAT};
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> prepare,peripheral,peripheral_coarse,peripheral_coarse2,peripheral_coarse4,peripheral_blocks,peripheral_blocks2,peripheral_blocks4,compose,pack,recenter;
    std::array<bool,2> coarse_output{};
    ComPtr<ID3D12DescriptorHeap> descriptors;
    unsigned stride{};
    FocusInput inputs[2][2]; // eye, center/full or periphery
    ComPtr<ID3D12Resource> peripheral_output[2],full[2],previous_depth[2];
    bool history[2]{};
    std::array<CompactLayout,2> color_layouts,previous_layouts;
    bool initialize(ID3D12Device*);
    bool prepare_inputs(ID3D12Device*,ID3D12GraphicsCommandList*,unsigned eye,unsigned pass,
        ID3D12Resource* color,const CheekyFgEye&,const FocusRect&,unsigned width,unsigned height,
        unsigned depth_bits=32,unsigned motion_bits=32,D3D12_RESOURCE_STATES color_state=D3D12_RESOURCE_STATE_COMMON,
        GpuTimings* timings=nullptr,float crop_motion_dx=0.f,float crop_motion_dy=0.f);
    // Re-express a midpoint-crop NGX result in the current crop's coordinates.
    // Newly uncovered pixels use the prepared current native crop.
    bool recenter_frame(ID3D12Device*,ID3D12GraphicsCommandList*,unsigned eye,
        ID3D12Resource* generated,float sample_shift_x,float sample_shift_y);
    bool generate_periphery(ID3D12Device*,ID3D12GraphicsCommandList*,unsigned eye,
        ID3D12Resource* previous,ID3D12Resource* current,const CheekyFgEye&,const Camera&,
        D3D12_RESOURCE_STATES color_state=D3D12_RESOURCE_STATE_COMMON,
        ID3D12Resource* destination=nullptr,D3D12_RESOURCE_STATES destination_state=D3D12_RESOURCE_STATE_COMMON,
        const CompactLayout* destination_layout=nullptr,const FinalOutput* final_output=nullptr);
    bool compose_frame(ID3D12Device*,ID3D12GraphicsCommandList*,unsigned eye,
        ID3D12Resource* center,ID3D12Resource* periphery,const FocusRect&,unsigned width,unsigned height,
        ID3D12Resource* destination=nullptr,D3D12_RESOURCE_STATES destination_state=D3D12_RESOURCE_STATE_COMMON);
    bool save_depth(ID3D12Device*,ID3D12GraphicsCommandList*,unsigned eye);
    bool pack_frame(ID3D12Device*,ID3D12GraphicsCommandList*,unsigned eye,ID3D12Resource* center,ID3D12Resource* context,
        ID3D12Resource* destination,D3D12_RESOURCE_STATES,const CompactLayout&,bool center_direct,bool context_direct);
    void reset() noexcept;
};
}
