#pragma once

namespace cheeky::foveated_dlss {

enum class PeripheralDlaaGenerationDecision {
    reuse,
    replace,
    reject,
};

[[nodiscard]] constexpr PeripheralDlaaGenerationDecision
peripheral_dlaa_generation_decision(
    const bool compatible,
    const bool synchronization_complete
) noexcept {
    if (compatible) return PeripheralDlaaGenerationDecision::reuse;
    return synchronization_complete
        ? PeripheralDlaaGenerationDecision::replace
        : PeripheralDlaaGenerationDecision::reject;
}

}  // namespace cheeky::foveated_dlss
