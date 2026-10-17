#pragma once
#include <cstdint>

namespace cheeky::foveated_dlss {
// Entering a UEVR command announces upcoming render work. A final color copy
// may still precede that work. Keep the prior completed stereo interval until
// the new interval starts evaluating; partial/extra intervals have no color ID.
// This is local to the executing RHI thread, never a global latest-guide lookup.
struct FgFramePhase {
    std::uint64_t current{},previous{};
    unsigned evaluations{};
    void begin(std::uint64_t ticket) {
        previous=evaluations==2?current:0;
        current=ticket;evaluations=0;
    }
    void evaluated(){if(evaluations<3)++evaluations;}
    std::uint64_t color() const {
        return evaluations==0?previous:evaluations==2?current:0;
    }
};
}
