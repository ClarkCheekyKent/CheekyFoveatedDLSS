#pragma once

#include <d3d11.h>
#include <d3d12.h>

#include <cstdint>

namespace cheeky::foveated_dlss {

using NgxResult = std::uint32_t;

struct NgxHandle;

// ABI-compatible public subset of NVSDK_NGX_Parameter. This keeps the add-on
// independent from a particular NGX SDK release; the game owns every object.
struct NgxParameters {
    virtual void Set(const char*, unsigned long long) = 0;
    virtual void Set(const char*, float) = 0;
    virtual void Set(const char*, double) = 0;
    virtual void Set(const char*, unsigned int) = 0;
    virtual void Set(const char*, int) = 0;
    virtual void Set(const char*, ID3D11Resource*) = 0;
    virtual void Set(const char*, ID3D12Resource*) = 0;
    virtual void Set(const char*, void*) = 0;
    virtual NgxResult Get(const char*, unsigned long long*) const = 0;
    virtual NgxResult Get(const char*, float*) const = 0;
    virtual NgxResult Get(const char*, double*) const = 0;
    virtual NgxResult Get(const char*, unsigned int*) const = 0;
    virtual NgxResult Get(const char*, int*) const = 0;
    virtual NgxResult Get(const char*, ID3D11Resource**) const = 0;
    virtual NgxResult Get(const char*, ID3D12Resource**) const = 0;
    virtual NgxResult Get(const char*, void**) const = 0;
    virtual void Reset() = 0;
};

using NgxProgressCallback = void (*)(float, bool&);
using NgxProgressCallbackC = void (*)(float, bool*);

[[nodiscard]] inline bool ngx_succeeded(const NgxResult result) noexcept {
    return (result & 0xFFF00000U) != 0xBAD00000U;
}

[[nodiscard]] inline std::uint32_t get_ui(
    const NgxParameters* const parameters,
    const char* const name
) noexcept {
    unsigned int value{};
    return parameters != nullptr && ngx_succeeded(parameters->Get(name, &value))
        ? value
        : 0U;
}

// A successful zero is an explicit declaration, not an unavailable parameter.
[[nodiscard]] inline bool try_get_ngx_integer_bits(
    const NgxParameters* parameters, const char* name, std::uint32_t& value
) noexcept {
    value = 0U;
    if (!parameters) return false;
    int signed_value{};
    if (ngx_succeeded(parameters->Get(name, &signed_value))) {
        value = static_cast<std::uint32_t>(signed_value);
        return true;
    }
    unsigned int unsigned_value{};
    if (!ngx_succeeded(parameters->Get(name, &unsigned_value))) return false;
    value = unsigned_value;
    return true;
}

// NGX integer parameters are not consistently exposed through the same signed
// overload by every integration. Preserve their bits regardless of which
// integer overload the title used when setting the value.
[[nodiscard]] inline std::uint32_t get_ngx_integer_bits(
    const NgxParameters* const parameters,
    const char* const name
) noexcept {
    std::uint32_t value{};
    static_cast<void>(try_get_ngx_integer_bits(parameters, name, value));
    return value;
}

}  // namespace cheeky::foveated_dlss
