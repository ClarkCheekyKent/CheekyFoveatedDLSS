#include "gaze_math.hpp"

#include <algorithm>
#include <cmath>

namespace cheeky::gaze_math {
namespace {

[[nodiscard]] Quaternion conjugate(const Quaternion& q) noexcept {
    return {-q.x, -q.y, -q.z, q.w};
}

[[nodiscard]] Vector3 rotate(
    const Quaternion& q,
    const Vector3& v
) noexcept {
    const Vector3 u{q.x, q.y, q.z};
    const auto dot_uv = u.x * v.x + u.y * v.y + u.z * v.z;
    const auto dot_uu = u.x * u.x + u.y * u.y + u.z * u.z;
    const Vector3 cross{
        u.y * v.z - u.z * v.y,
        u.z * v.x - u.x * v.z,
        u.x * v.y - u.y * v.x,
    };
    return {
        2.0F * dot_uv * u.x + (q.w * q.w - dot_uu) * v.x +
            2.0F * q.w * cross.x,
        2.0F * dot_uv * u.y + (q.w * q.w - dot_uu) * v.y +
            2.0F * q.w * cross.y,
        2.0F * dot_uv * u.z + (q.w * q.w - dot_uu) * v.z +
            2.0F * q.w * cross.z,
    };
}

}  // namespace

double next_simulated_jump_time(const double seconds, const unsigned pattern) noexcept {
    const double interval = pattern == 3U ? 8.0 : 2.0;
    return (std::floor(std::max(seconds, 0.0) / interval) + 1.0) * interval;
}

bool simulated_gaze_valid(const double seconds, const unsigned pattern) noexcept {
    return pattern != 4U || std::fmod(std::max(seconds, 0.0), 5.0) < 4.0;
}

Pose simulated_gaze_pose(const Pose& head, const double seconds, const unsigned pattern) noexcept {
    const double phase = std::fmod(seconds, 8.0) * 0.7853981633974483;
    float yaw = 0.30F * static_cast<float>(std::sin(phase));
    float pitch = 0.22F * static_cast<float>(std::sin(2.0 * phase));
    if (pattern == 1U) {
        yaw = 0.35F * static_cast<float>(std::sin(seconds * 0.3141592653589793));
        pitch = 0.0F;
    } else if (pattern == 2U || pattern == 3U) {
        const double interval = pattern == 2U ? 2.0 : 8.0;
        const unsigned target = static_cast<unsigned>(std::fmod(std::floor(std::max(seconds, 0.0) / interval), 5.0));
        constexpr float yaw_targets[]{0.0F, -0.35F, 0.35F, -0.35F, 0.35F};
        constexpr float pitch_targets[]{0.0F, 0.25F, -0.25F, -0.25F, 0.25F};
        yaw = yaw_targets[target]; pitch = pitch_targets[target];
    } else if (pattern == 5U) {
        yaw = 0.0F; pitch = 0.0F;
    }
    const float sy = std::sin(yaw * 0.5F), cy = std::cos(yaw * 0.5F);
    const float sx = std::sin(pitch * 0.5F), cx = std::cos(pitch * 0.5F);
    const Quaternion b{cy * sx, sy * cx, -sy * sx, cy * cx};
    const auto& a = head.orientation;
    return {{
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    }, head.position};
}

bool project_gaze_to_view(
    const Pose& gaze_pose,
    const Pose& view_pose,
    const Fov& fov,
    float& center_u,
    float& center_v
) noexcept {
    const auto world_direction = rotate(
        gaze_pose.orientation,
        Vector3{0.0F, 0.0F, -1.0F}
    );
    const auto view_direction = rotate(
        conjugate(view_pose.orientation),
        world_direction
    );
    if (!std::isfinite(view_direction.x) ||
        !std::isfinite(view_direction.y) ||
        !std::isfinite(view_direction.z) || view_direction.z >= -0.0001F) {
        return false;
    }

    const auto left = std::tan(fov.angle_left);
    const auto right = std::tan(fov.angle_right);
    const auto down = std::tan(fov.angle_down);
    const auto up = std::tan(fov.angle_up);
    const auto width = right - left;
    const auto height = up - down;
    if (!std::isfinite(width) || !std::isfinite(height) ||
        width <= 0.0001F || height <= 0.0001F) {
        return false;
    }

    const auto tangent_x = view_direction.x / -view_direction.z;
    const auto tangent_y = view_direction.y / -view_direction.z;
    center_u = (tangent_x - left) / width;
    center_v = (up - tangent_y) / height;
    return std::isfinite(center_u) && std::isfinite(center_v);
}

}  // namespace cheeky::gaze_math
