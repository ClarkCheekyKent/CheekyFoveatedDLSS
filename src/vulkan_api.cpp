#include "vulkan_api.hpp"
namespace cheeky::foveated_dlss {
bool VulkanDeviceApi::initialize(VkInstance i, VkPhysicalDevice p, VkDevice d,
    PFN_vkGetInstanceProcAddr gi, PFN_vkGetDeviceProcAddr gd) noexcept {
    instance=i; physical=p; device=d; gipa=gi; gdpa=gd;
    if (!i || !p || !d || !gi || !gd) return false;
#define CHEEKY_VK_LOAD(name) name=reinterpret_cast<PFN_vk##name>(gd(d,"vk" #name)); if (!name) return false;
    CHEEKY_VK_DEVICE_FUNCTIONS(CHEEKY_VK_LOAD)
#undef CHEEKY_VK_LOAD
    const auto get_memory=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gi(i,"vkGetPhysicalDeviceMemoryProperties"));
    if (!get_memory) return false;
    get_memory(p,&memory);
    return true;
}
}
