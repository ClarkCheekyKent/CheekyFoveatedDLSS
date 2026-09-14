#pragma once
#include <cstdint>

namespace cheeky::foveated_dlss {
// Bind only resources explicitly submitted by AFW with an eye index. The
// current core call must copy its own original depth into one of these exact
// resources before its lower DLSS evaluation. No frame/handle parity guesses.
class AfwDepthEyes {
    struct Binding { std::uint64_t resource{}, time{}, generation{}; } eyes_[2]{};
    std::uint64_t ambiguous_resource_{}, ambiguous_generation_{};
public:
    void record(unsigned eye, std::uint64_t resource, std::uint64_t now, std::uint64_t generation) noexcept {
        if (eye > 1 || !resource) return;
        if (ambiguous_resource_ == resource && ambiguous_generation_ == generation) return;
        if (eyes_[eye ^ 1].resource == resource && eyes_[eye ^ 1].generation == generation) {
            ambiguous_resource_ = resource; ambiguous_generation_ = generation;
            eyes_[0] = {}; eyes_[1] = {}; return;
        }
        eyes_[eye] = {resource, now, generation};
    }
    unsigned lookup(std::uint64_t resource, std::uint64_t now, std::uint64_t generation) const noexcept {
        for (unsigned i = 0; i < 2; ++i) {
            const auto& b = eyes_[i];
            if (resource && b.resource == resource && b.generation == generation && now >= b.time && now - b.time <= 250)
                return i;
        }
        return UINT32_MAX;
    }
    void forget(std::uint64_t resource) noexcept {
        for (auto& b : eyes_) if (b.resource == resource) b = {};
        if (ambiguous_resource_ == resource) ambiguous_resource_ = 0;
    }
};
}
