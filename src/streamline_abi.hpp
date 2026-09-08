#pragma once
#include <cstdint>
#include <cstddef>

namespace cheeky::foveated_dlss {
struct SlStructType {
    std::uint32_t data1{};
    std::uint16_t data2{};
    std::uint16_t data3{};
    std::uint8_t data4[8]{};
};

struct SlBaseStructure {
    SlBaseStructure* next{};
    SlStructType struct_type{};
    std::size_t struct_version{};
};

// Streamline FrameToken's public ABI exposes the frame index through this
// virtual conversion (include/sl_core_types.h). Token objects are reused.
struct SlFrameToken : SlBaseStructure {
    virtual operator std::uint32_t() const = 0;
};
inline std::uintptr_t gaze_frame_key(const void* frame) {
    return frame ? static_cast<std::uintptr_t>(
        static_cast<std::uint32_t>(*static_cast<const SlFrameToken*>(frame))) + 1U : 0U;
}

struct SlExtent {
    std::uint32_t top{};
    std::uint32_t left{};
    std::uint32_t width{};
    std::uint32_t height{};
};

enum class SlResourceType : char {
    texture_2d = 0,
};

struct SlResource : SlBaseStructure {
    SlResourceType type{SlResourceType::texture_2d};
    void* native{};
    void* memory{};
    void* view{};
    std::uint32_t state{0xFFFFFFFFU};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t native_format{};
    std::uint32_t mip_levels{};
    std::uint32_t array_layers{};
    std::uint64_t gpu_virtual_address{};
    std::uint32_t flags{};
    std::uint32_t usage{};
    std::uint32_t reserved{};
};

struct SlResourceTag : SlBaseStructure {
    SlResource* resource{};
    std::uint32_t type{};
    std::uint32_t lifecycle{};
    SlExtent extent{};
};

struct SlFloat2 { float x{}; float y{}; };
struct SlFloat3 { float x{}; float y{}; float z{}; };
struct SlFloat4x4 { float values[16]{}; };

struct SlConstants : SlBaseStructure {
    SlFloat4x4 camera_view_to_clip{};
    SlFloat4x4 clip_to_camera_view{};
    SlFloat4x4 clip_to_lens_clip{};
    SlFloat4x4 clip_to_prev_clip{};
    SlFloat4x4 prev_clip_to_clip{};
    SlFloat2 jitter_offset{};
    SlFloat2 motion_vector_scale{};
    SlFloat2 camera_pinhole_offset{};
    SlFloat3 camera_position{};
    SlFloat3 camera_up{};
    SlFloat3 camera_right{};
    SlFloat3 camera_forward{};
    float camera_near{};
    float camera_far{};
    float camera_fov{};
    float camera_aspect_ratio{};
    float motion_vectors_invalid_value{};
    char depth_inverted{};
    char camera_motion_included{};
    char motion_vectors_3d{};
    char reset{};
    char orthographic_projection{};
    char motion_vectors_dilated{};
    char motion_vectors_jittered{};
    float minimum_relative_linear_depth_object_separation{};
};

struct SlDlssOptions : SlBaseStructure {
    std::uint32_t mode{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    float sharpness{};
    float pre_exposure{};
    float exposure_scale{};
    char color_buffers_hdr{};
    char indicator_invert_axis_x{};
    char indicator_invert_axis_y{};
    std::uint32_t dlaa_preset{};
    std::uint32_t quality_preset{};
    std::uint32_t balanced_preset{};
    std::uint32_t performance_preset{};
    std::uint32_t ultra_performance_preset{};
    std::uint32_t ultra_quality_preset{};
    char use_auto_exposure{};
    char alpha_upscaling_enabled{};
};

struct SlViewportHandle : SlBaseStructure {
    std::uint32_t value{0xFFFFFFFFU};
};

} // namespace cheeky::foveated_dlss
