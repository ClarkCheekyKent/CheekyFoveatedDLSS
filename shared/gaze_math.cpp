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
