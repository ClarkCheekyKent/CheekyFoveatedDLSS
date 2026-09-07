#pragma once

#include <array>
#include <cstdint>
#include <cstring>

namespace cheeky::foveated_dlss {

// Keep the original frame/viewport constants immutable. The private SR viewport
// is distinct from both the host and the peripheral DLAA namespace (0x40000000).
template <typename Viewport, std::size_t Capacity>
[[nodiscard]] bool prepare_streamline_sr_inputs(
    const void* const* inputs, std::uint32_t count, const Viewport& original,
    Viewport& cropped, std::array<const void*, Capacity>& redirected
) noexcept {
    if (inputs == nullptr || count == 0U || count > Capacity ||
        (original.value & 0x60000000U) != 0U) return false;
    cropped = original;
    cropped.next = nullptr;
    cropped.value |= 0x20000000U;
    bool found{};
    for (std::uint32_t i{}; i < count; ++i) {
        redirected[i] = inputs[i];
        // All Streamline input structures share the BaseStructure prefix.
        const auto* candidate = static_cast<const Viewport*>(inputs[i]);
        if (candidate == nullptr || std::memcmp(&candidate->struct_type,
                &original.struct_type, sizeof(original.struct_type)) != 0) continue;
        if (found || candidate->value != original.value || candidate->next != nullptr) return false;
        redirected[i] = &cropped;
        found = true;
    }
    return found;
}
} // namespace cheeky::foveated_dlss
