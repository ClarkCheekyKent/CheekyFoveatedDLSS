#pragma once
#include "ngx_abi.hpp"
#include <array>

namespace cheeky::foveated_dlss {
struct NgxOutputExtent {
    unsigned width{}, height{};
};

// Optimal-settings queries reuse Width/OutWidth for display/input sizes.
// Evaluation instead uses the feature's created output extent and the current
// render subrect. Keep that contract consistent across preparation, diagnostics
// and marker stamping, then restore the application's parameter bag exactly.
class NgxEvaluationExtentScope {
    NgxParameters* parameters_{};
    static constexpr std::array<const char*, 4> names_{"Width", "Height", "OutWidth", "OutHeight"};
    std::array<unsigned, 4> saved_{};
    std::array<bool, 4> changed_{};
public:
    NgxEvaluationExtentScope(const NgxParameters* parameters, NgxOutputExtent output) noexcept
        : parameters_(const_cast<NgxParameters*>(parameters)) {
        if (!parameters_) return;
        const unsigned render_width = get_ui(parameters, "DLSS.Render.Subrect.Dimensions.Width");
        const unsigned render_height = get_ui(parameters, "DLSS.Render.Subrect.Dimensions.Height");
        const bool render_valid = render_width && render_height;
        const bool output_valid = output.width && output.height;
        const std::array<unsigned, 4> desired{
            render_valid ? render_width : 0U, render_valid ? render_height : 0U,
            output_valid ? output.width : 0U, output_valid ? output.height : 0U};
        for (unsigned i = 0; i < names_.size(); ++i) {
            // Do not invent absent keys: NGX provides no single-key erase.
            if (!desired[i] || !ngx_succeeded(parameters->Get(names_[i], &saved_[i]))) continue;
            if (desired[i] != saved_[i]) {
                changed_[i] = true;
                parameters_->Set(names_[i], desired[i]);
            }
        }
    }
    ~NgxEvaluationExtentScope() {
        for (unsigned i = 0; i < names_.size(); ++i)
            if (changed_[i]) parameters_->Set(names_[i], saved_[i]);
    }
    NgxEvaluationExtentScope(const NgxEvaluationExtentScope&) = delete;
    NgxEvaluationExtentScope& operator=(const NgxEvaluationExtentScope&) = delete;
};
} // namespace cheeky::foveated_dlss
