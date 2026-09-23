#pragma once
#include "ngx_abi.hpp"
#include "runtime.hpp"
#include <d3d11.h>
#include <d3d12.h>
#include <array>
#include <atomic>
#include <cstdint>
#include "exposure_capture.hpp"

namespace cheeky::foveated_dlss {
// Collect only during a support capture. Reset quotas for every request.
inline bool exposure_log_due(unsigned route, std::uintptr_t view) noexcept {
    const auto deadline = exposure_capture_deadline.load(std::memory_order_relaxed);
    if (!deadline) return false;
    const auto now = GetTickCount64();
    if (now >= deadline) return false;
    struct Entry { unsigned route{}; std::uintptr_t view{}; ULONGLONG next{}; };
    static thread_local std::array<Entry, 32> entries{};
    static thread_local std::uint64_t generation{};
    const auto current = exposure_capture_generation.load();
    if (generation != current) { entries = {}; generation = current; }
    for (auto& e : entries) {
        if (e.route && (e.route != route || e.view != view)) continue;
        if (e.route && now < e.next) return false;
        e = {route, view, now + 5000};
        return true;
    }
    return false;
}
struct ExposureResourceInfo { unsigned format{}; std::uint64_t width{}; unsigned height{}; };
inline ExposureResourceInfo exposure_resource_info(ID3D12Resource* r) noexcept {
    if (!r) return {};
    const auto d = r->GetDesc();
    return {unsigned(d.Format), d.Width, d.Height};
}
inline ExposureResourceInfo exposure_resource_info(ID3D11Resource* r) noexcept {
    ID3D11Texture2D* t{};
    if (!r || FAILED(r->QueryInterface(IID_PPV_ARGS(&t)))) return {};
    D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d); t->Release();
    return {unsigned(d.Format), d.Width, d.Height};
}
template<class Resource>
inline void log_ngx_exposure(unsigned route, const void* view, const NgxParameters* p) noexcept {
    if (!p || !exposure_log_due(route, reinterpret_cast<std::uintptr_t>(view))) return;
    float pre{}, scale{}, legacy_pre{}, legacy_scale{};
    const auto pr = p->Get("DLSS.Pre.Exposure", &pre);
    const auto sr = p->Get("DLSS.Exposure.Scale", &scale);
    const auto lpr = p->Get("Pre.Exposure", &legacy_pre);
    const auto lsr = p->Get("Exposure.Scale", &legacy_scale);
    std::uint32_t flags{};
    const bool flags_present = try_get_ngx_integer_bits(p, "DLSS.Feature.Create.Flags", flags);
    Resource *exposure{}, *color{}, *output{};
    const auto er = p->Get("ExposureTexture", &exposure);
    if (!ngx_succeeded(er)) exposure = nullptr;
    if (!ngx_succeeded(p->Get("Color", &color))) color = nullptr;
    if (!ngx_succeeded(p->Get("Output", &output))) output = nullptr;
    const auto ed = exposure_resource_info(exposure);
    const auto cd = exposure_resource_info(color);
    const auto od = exposure_resource_info(output);
    trace_event("EXPOSURE v1 tick=%llu api=DX%u view=%p pre=%.9g pre_result=0x%08X scale=%.9g scale_result=0x%08X legacy_pre=%.9g legacy_pre_result=0x%08X legacy_scale=%.9g legacy_scale_result=0x%08X flags_present=%u flags=0x%08X hdr=%u auto_exposure=%u exposure=%p exposure_result=0x%08X exposure_format=%u exposure_size=%llux%u color_format=%u output_format=%u (metadata_only; missing values are not defaults; support_capture; 5s/view; max32 views/thread)",
        GetTickCount64(), route, view, pre, unsigned(pr), scale, unsigned(sr),
        legacy_pre, unsigned(lpr), legacy_scale, unsigned(lsr), unsigned(flags_present), flags,
        (flags & 1U) != 0, (flags & 64U) != 0, exposure, unsigned(er), ed.format,
        static_cast<unsigned long long>(ed.width), ed.height, cd.format, od.format);
}
} // namespace cheeky::foveated_dlss

