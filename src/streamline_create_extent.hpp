#pragma once

#include "ngx_abi.hpp"

namespace cheeky::foveated_dlss {

// Streamline derives creation input dimensions from its quality mode and output
// size. A supersampled center keeps its actual input crop fixed instead.
template <typename Parameters>
class StreamlineCreateExtentScope {
public:
    StreamlineCreateExtentScope(Parameters* parameters, unsigned width, unsigned height) noexcept {
        if (!parameters || !width || !height ||
            !ngx_succeeded(parameters->Get("Width", &saved_width_)) ||
            !ngx_succeeded(parameters->Get("Height", &saved_height_))) return;
        parameters_ = parameters;
        parameters_->Set("Width", width);
        parameters_->Set("Height", height);
    }
    ~StreamlineCreateExtentScope() {
        if (!parameters_) return;
        parameters_->Set("Width", saved_width_);
        parameters_->Set("Height", saved_height_);
    }
    StreamlineCreateExtentScope(const StreamlineCreateExtentScope&) = delete;
    StreamlineCreateExtentScope& operator=(const StreamlineCreateExtentScope&) = delete;

private:
    Parameters* parameters_{};
    unsigned saved_width_{}, saved_height_{};
};

} // namespace cheeky::foveated_dlss
