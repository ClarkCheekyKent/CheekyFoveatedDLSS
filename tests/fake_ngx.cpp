#include "ngx_abi.hpp"
#include "mock_ngx_parameters.hpp"
#include <atomic>
using namespace cheeky::foveated_dlss;
namespace {
std::atomic<unsigned> creates{}, evaluates{}, releases{};
using Observe = void(*)(const NgxParameters*);
Observe observe{};
Observe observe_created{};
struct Feature { unsigned id; MockNgxParameters created; };
bool fail_evaluation{};
bool copy_nr_color{};
unsigned fail_next{};
NgxResult evaluate(const NgxHandle* handle, const NgxParameters* params) {
    ++evaluates;
    if (observe_created) observe_created(&reinterpret_cast<const Feature*>(handle)->created);
    if (observe) observe(params);
    if (fail_next) { --fail_next; return 0xBAD00007U; }
    return fail_evaluation ? 0xBAD00007U : 1U;
}
}
#define EXPORT extern "C" __declspec(dllexport) __declspec(noinline)
EXPORT unsigned CheekyFakeCreates() { return creates.load(); }
EXPORT unsigned CheekyFakeEvaluates() { return evaluates.load(); }
EXPORT unsigned CheekyFakeReleases() { return releases.load(); }
EXPORT void CheekyFakeObserve(Observe callback) { observe = callback; }
EXPORT void CheekyFakeObserveCreated(Observe callback) { observe_created = callback; }
EXPORT void CheekyFakeFailEvaluations(bool fail) { fail_evaluation = fail; }
EXPORT void CheekyFakeCopyNrColor(bool enabled) { copy_nr_color = enabled; }
EXPORT void CheekyFakeFailNextEvaluations(unsigned count) { fail_next = count; }
// The hook harness loads a second copy as its optional feature-18 runtime.
EXPORT NgxResult NVSDK_NGX_D3D12_Init_Ext(unsigned long long, const wchar_t*, ID3D12Device*, unsigned, const NgxParameters*) {
    wchar_t path[MAX_PATH]{};
    return GetModuleFileNameW(nullptr, path, MAX_PATH) ? 1U : 0xBAD00007U;
}
EXPORT NgxResult NVSDK_NGX_D3D12_AllocateParameters(NgxParameters** out) { *out = new MockNgxParameters; return 1U; }
EXPORT NgxResult NVSDK_NGX_D3D12_DestroyParameters(NgxParameters* params) { delete static_cast<MockNgxParameters*>(params); return 1U; }
EXPORT NgxResult NVSDK_NGX_D3D11_Init(unsigned long long, const wchar_t*, ID3D11Device*, const void*, unsigned) { return 1U; }
EXPORT NgxResult NVSDK_NGX_D3D12_Init(unsigned long long, const wchar_t*, ID3D12Device*, const void*, unsigned) { return 1U; }
EXPORT NgxResult NVSDK_NGX_D3D11_CreateFeature(ID3D11DeviceContext*, unsigned, NgxParameters*, NgxHandle** out) {
    *out=reinterpret_cast<NgxHandle*>(new Feature{++creates, {}}); return 1U;
}
EXPORT NgxResult NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList*, unsigned, NgxParameters* params, NgxHandle** out) {
    *out=reinterpret_cast<NgxHandle*>(new Feature{++creates, *static_cast<MockNgxParameters*>(params)}); return 1U;
}
EXPORT NgxResult NVSDK_NGX_D3D11_EvaluateFeature(ID3D11DeviceContext*, const NgxHandle*, const NgxParameters*, NgxProgressCallback) {
    ++evaluates; return 1U;
}
EXPORT NgxResult NVSDK_NGX_D3D11_EvaluateFeature_C(ID3D11DeviceContext*, const NgxHandle*, const NgxParameters*, NgxProgressCallbackC) {
    ++evaluates; return 1U;
}
EXPORT NgxResult NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList* list, const NgxHandle* handle, const NgxParameters* params, NgxProgressCallback) {
    const auto result = evaluate(handle, params);
    if (ngx_succeeded(result) && copy_nr_color) {
        ID3D12Resource* color{}; ID3D12Resource* output{};
        params->Get("DLSSNR.Color", &color); params->Get("DLSSNR.Output", &output);
        if (color && output) {
            D3D12_RESOURCE_BARRIER barriers[2]{};
            barriers[0].Type = barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[0].Transition = {color, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE};
            barriers[1].Transition = {output, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST};
            list->ResourceBarrier(2, barriers); list->CopyResource(output, color);
            for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(2, barriers);
        }
    }
    return result;
}
EXPORT NgxResult NVSDK_NGX_D3D12_EvaluateFeature_C(ID3D12GraphicsCommandList*, const NgxHandle* handle, const NgxParameters* params, NgxProgressCallbackC) {
    return evaluate(handle, params);
}
EXPORT NgxResult NVSDK_NGX_D3D11_ReleaseFeature(NgxHandle* handle) { ++releases; delete reinterpret_cast<Feature*>(handle); return 1U; }
EXPORT NgxResult NVSDK_NGX_D3D12_ReleaseFeature(NgxHandle* handle) { ++releases; delete reinterpret_cast<Feature*>(handle); return 1U; }
