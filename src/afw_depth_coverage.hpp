#pragma once
#include <d3d12.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cheeky::foveated_dlss {
using AfwMatrix = std::array<float, 16>;
inline constexpr AfwMatrix afw_identity_matrix{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
inline AfwMatrix afw_multiply(const AfwMatrix& a, const AfwMatrix& b) noexcept {
    AfwMatrix result{};
    for (unsigned c = 0; c < 4; ++c) for (unsigned r = 0; r < 4; ++r)
        for (unsigned k = 0; k < 4; ++k) result[r + 4 * c] += a[r + 4 * k] * b[k + 4 * c];
    return result;
}
// AFW supplies column-major source/destination cameras already using the
// depth convention of its texture. Preserve their homogeneous transform.
inline bool afw_project_depth(const AfwMatrix& matrix, float u, float v, float depth, float& x, float& y, bool clip_to_image = true) noexcept {
    if (!std::isfinite(depth) || depth < 0 || depth > 1) return false;
    const double clip[]{2. * u - 1., 1. - 2. * v, depth, 1.};
    double target[4]{};
    for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c) target[r] += matrix[r + 4 * c] * clip[c];
    if (!std::isfinite(target[3]) || target[3] <= 1e-8) return false;
    x = static_cast<float>((target[0] / target[3] + 1.) * .5);
    y = static_cast<float>((1. - target[1] / target[3]) * .5);
    return std::isfinite(x) && std::isfinite(y) && (!clip_to_image || (x >= 0 && x <= 1 && y >= 0 && y <= 1));
}
struct AfwDepthCoverageStatus {
    bool valid{};
    float margin{};
    std::uint64_t captures{}, completed{}, skipped{}, age_ms{UINT64_MAX};
    unsigned pending{}, source_eye{UINT32_MAX};
};
struct AfwDepthMarginPolicy {
    float value{}, candidate{};
    std::uint64_t since{};
    bool reducing{};
    float update(float needed, std::uint64_t now) noexcept {
        needed = std::clamp(std::ceil(needed * 32.F) / 32.F, 0.F, 1.F);
        if (needed >= value) { value = needed; reducing = false; }
        else {
            if (!reducing || now < since) { reducing = true; since = now; candidate = needed; }
            candidate = (std::max)(candidate, needed);
            if (now - since >= 1000) { value = candidate; reducing = false; }
        }
        return value;
    }
};
void capture_afw_depth_coverage(ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES,
    const AfwMatrix& source_clip_to_destination_clip, unsigned eye, const AfwMatrix& projection_only = afw_identity_matrix) noexcept;
void poll_afw_depth_coverage() noexcept;
[[nodiscard]] AfwDepthCoverageStatus afw_depth_coverage_status(unsigned eye = UINT32_MAX) noexcept;
}
