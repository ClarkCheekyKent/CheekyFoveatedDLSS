#pragma once
#include "dlss_nr.hpp"
#include <array>

namespace cheeky::foveated_dlss {
// Owns only the substitution, not the game parameter object or GPU resources.
class NgxNrInputSubstitution {
    NgxParameters* parameters_{};
    ID3D12Resource* color_{};
    std::uint32_t reset_{};
    bool color_changed_{}, reset_changed_{};
public:
    NgxNrInputSubstitution(const NgxParameters* parameters, ID3D12Resource* original,
        ID3D12Resource* processed, bool reset) noexcept
        : parameters_(const_cast<NgxParameters*>(parameters)), color_(original) {
        if (!parameters_) return;
        color_changed_ = processed != nullptr;
        reset_changed_ = reset && ngx_succeeded(parameters_->Get("Reset", &reset_));
        if (color_changed_) parameters_->Set("Color", processed);
        if (reset_changed_) parameters_->Set("Reset", 1U);
    }
    ~NgxNrInputSubstitution() {
        if (!parameters_) return;
        if (color_changed_) parameters_->Set("Color", color_);
        if (reset_changed_) parameters_->Set("Reset", reset_);
    }
    NgxNrInputSubstitution(const NgxNrInputSubstitution&) = delete;
    NgxNrInputSubstitution& operator=(const NgxNrInputSubstitution&) = delete;
};

// Keeps original tags alive and restores them even when submission rejects the
// replacement after partially consuming it. Does not submit host constants.
template<class Resource, class Tag>
class NrTagSubstitution {
public:
    using Submit = std::uint32_t (*)(void*, const Tag*, std::uint32_t);
private:
    std::array<Resource, 4U> resources_;
    std::array<Tag, 4U> tags_;
    Resource processed_resource_{};
    Tag processed_tag_{};
    Submit submit_{};
    void* context_{};
    bool attempted_{};
public:
    NrTagSubstitution(const std::array<Resource, 4U>& resources,
        const std::array<Tag, 4U>& tags, Submit submit, void* context)
        : resources_(resources), tags_(tags), submit_(submit), context_(context) {
        for (std::size_t i = 0; i < tags_.size(); ++i) tags_[i].resource = &resources_[i];
    }
    bool apply(ID3D12Resource* processed) {
        if (!processed || !submit_) return false;
        processed_resource_ = resources_[0];
        processed_tag_ = tags_[0];
        processed_resource_.native = processed;
        if constexpr (requires { processed_resource_.view; }) processed_resource_.view = nullptr;
        if constexpr (requires { processed_resource_.memory; }) processed_resource_.memory = nullptr;
        if constexpr (requires { processed_resource_.mip_levels; }) processed_resource_.mip_levels = 1U;
        processed_tag_.resource = &processed_resource_;
        attempted_ = true;
        if (submit_(context_, &processed_tag_, 1U) == 0U) return true;
        restore();
        return false;
    }
    void restore() {
        if (!attempted_) return;
        static_cast<void>(submit_(context_, tags_.data(), static_cast<std::uint32_t>(tags_.size())));
        attempted_ = false;
    }
    ~NrTagSubstitution() { restore(); }
    NrTagSubstitution(const NrTagSubstitution&) = delete;
    NrTagSubstitution& operator=(const NrTagSubstitution&) = delete;
};

// Returns a private color in the same coordinate space/state as the source.
// The source is never written. A null result means SR must use original color.
[[nodiscard]] ID3D12Resource* prepare_dlss_nr_input(
    DlssNrFrame frame, const Settings& settings) noexcept;
void collect_dlss_nr_input_submissions() noexcept;
void release_dlss_nr_inputs(DlssViewId view_id = 0U) noexcept;
// Per-view transitions invalidate both SR histories, including fallback->NR.
[[nodiscard]] bool dlss_nr_input_history_compatible(DlssViewId view_id,
    NrProcessingOrder order, bool processed, std::uint32_t width, std::uint32_t height) noexcept;
[[nodiscard]] bool dlss_nr_input_history_reset(DlssViewId view_id,
    NrProcessingOrder order, bool processed, std::uint32_t width,
    std::uint32_t height) noexcept;
} // namespace cheeky::foveated_dlss
