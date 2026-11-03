#pragma once
#include "../third_party/openxr/include/openxr/openxr.h"
#include <cstring>

namespace cheeky::openxr {
// Borrowed handles are diagnostic values only. Reasons must have static lifetime:
// the runtime keeps the reason pointer for the overlay and support snapshot.
struct SubmissionDiagnostic {
    const char* reason{};
    int layer{-1}, eye{-1};
    XrStructureType structure_type{XR_TYPE_UNKNOWN};
    XrSpace space{};
    XrSwapchain swapchain{};
    unsigned view_count{};

    bool same_rejection(const SubmissionDiagnostic& other) const noexcept {
        const bool same_reason=reason&&other.reason?std::strcmp(reason,other.reason)==0:reason==other.reason;
        // Handles may change every frame; do not turn that into repeated logs.
        return same_reason&&layer==other.layer&&eye==other.eye&&structure_type==other.structure_type;
    }
};
}
