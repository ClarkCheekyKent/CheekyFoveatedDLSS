#pragma once

namespace cheeky::gaze_math {

struct Vector3 {
    float x{};
    float y{};
    float z{};
};

struct Quaternion {
    float x{};
    float y{};
    float z{};
    float w{1.0F};
};

struct Pose {
    Quaternion orientation{};
    Vector3 position{};
};

struct Fov {
    float angle_left{};
    float angle_right{};
    float angle_up{};
    float angle_down{};
};

// Head-relative synthetic gaze, repeating every eight seconds.
[[nodiscard]] Pose simulated_gaze_pose(const Pose& head, double seconds) noexcept;

[[nodiscard]] bool project_gaze_to_view(
    const Pose& gaze_pose,
    const Pose& view_pose,
    const Fov& fov,
    float& center_u,
    float& center_v
) noexcept;

}  // namespace cheeky::gaze_math

