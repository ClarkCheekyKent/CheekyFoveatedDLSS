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
// Pattern IDs: 0 figure eight, 1 slow sweep, 2/3 jumps (2/8 s),
// 4 tracking loss, 5 stationary center.
[[nodiscard]] Pose simulated_gaze_pose(const Pose& head, double seconds, unsigned pattern = 0U) noexcept;
[[nodiscard]] double next_simulated_jump_time(double seconds, unsigned pattern) noexcept;
[[nodiscard]] bool simulated_gaze_valid(double seconds, unsigned pattern) noexcept;

[[nodiscard]] bool project_gaze_to_view(
    const Pose& gaze_pose,
    const Pose& view_pose,
    const Fov& fov,
    float& center_u,
    float& center_v
) noexcept;

}  // namespace cheeky::gaze_math

