#pragma once
#include "vulkan_api.hpp"
#include "ngx_frame_contract.hpp"

namespace cheeky::foveated_dlss {
void vulkan_calibration_stamp(VkCommandBuffer, const NgxParameters*, const DlssFrameContract&) noexcept;
void vulkan_calibration_forget_command(VkCommandBuffer) noexcept;
void vulkan_calibration_release_device(VkDevice) noexcept;
}
