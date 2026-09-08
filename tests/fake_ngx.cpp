#include "ngx_abi.hpp"
#include <atomic>
using namespace cheeky::foveated_dlss;
namespace { std::atomic<unsigned> creates{}, evaluates{}, releases{}; }
#define EXPORT extern "C" __declspec(dllexport) __declspec(noinline)
EXPORT unsigned CheekyFakeCreates() { return creates.load(); }
EXPORT unsigned CheekyFakeEvaluates() { return evaluates.load(); }
EXPORT unsigned CheekyFakeReleases() { return releases.load(); }
EXPORT NgxResult NVSDK_NGX_D3D11_Init(unsigned long long, const wchar_t*, ID3D11Device*, const void*, unsigned) { return 1U; }
EXPORT NgxResult NVSDK_NGX_D3D12_Init(unsigned long long, const wchar_t*, ID3D12Device*, const void*, unsigned) { return 1U; }
EXPORT NgxResult NVSDK_NGX_D3D11_CreateFeature(ID3D11DeviceContext*, unsigned, NgxParameters*, NgxHandle** out) {
    *out=reinterpret_cast<NgxHandle*>(new unsigned(++creates)); return 1U;
}
EXPORT NgxResult NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList*, unsigned, NgxParameters*, NgxHandle** out) {
    *out=reinterpret_cast<NgxHandle*>(new unsigned(++creates)); return 1U;
}
EXPORT NgxResult NVSDK_NGX_D3D11_EvaluateFeature(ID3D11DeviceContext*, const NgxHandle*, const NgxParameters*, NgxProgressCallback) {
    ++evaluates; return 1U;
}
EXPORT NgxResult NVSDK_NGX_D3D11_EvaluateFeature_C(ID3D11DeviceContext*, const NgxHandle*, const NgxParameters*, NgxProgressCallbackC) {
    ++evaluates; return 1U;
}
EXPORT NgxResult NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList*, const NgxHandle*, const NgxParameters*, NgxProgressCallback) {
    ++evaluates; return 1U;
}
EXPORT NgxResult NVSDK_NGX_D3D12_EvaluateFeature_C(ID3D12GraphicsCommandList*, const NgxHandle*, const NgxParameters*, NgxProgressCallbackC) {
    ++evaluates; return 1U;
}
EXPORT NgxResult NVSDK_NGX_D3D11_ReleaseFeature(NgxHandle* handle) { ++releases; delete reinterpret_cast<unsigned*>(handle); return 1U; }
EXPORT NgxResult NVSDK_NGX_D3D12_ReleaseFeature(NgxHandle* handle) { ++releases; delete reinterpret_cast<unsigned*>(handle); return 1U; }
