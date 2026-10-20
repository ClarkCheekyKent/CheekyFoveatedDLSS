#pragma once
#include <cstdint>

namespace cheeky::fg {
// The current unconsumed NGX stereo interval may contain one submitted eye
// while OpenXR is releasing the first image. With one eye, reserve the next
// evaluation as the expected second-eye token. xrEndFrame must verify it.
inline std::uint64_t pending_stereo_crop_token(std::uint64_t ordinal,
                                               std::uint64_t consumed,
                                               bool hookless) noexcept {
    if(ordinal<=consumed||ordinal-consumed>2)return 0;
    const auto count=ordinal-consumed;
    if(count==1&&!hookless)return 0;
    return ordinal+(count==1?1u:0u);
}
}
