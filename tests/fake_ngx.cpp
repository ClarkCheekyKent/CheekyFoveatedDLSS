#include "ngx_abi.hpp"
#include "mock_ngx_parameters.hpp"
#include "../shared/cheeky_gaze_abi.h"
#include <atomic>
using namespace cheeky::foveated_dlss;
namespace {
std::atomic<unsigned> creates{}, evaluates{}, releases{};
void* last_warp_parameters{};
unsigned warp_calls{};
CheekyGazeSnapshotV1 fake_gaze{};
using ObserveHandle = void(*)(const NgxHandle*, const NgxParameters*);
ObserveHandle observe_handle{};
using Observe = void(*)(const NgxParameters*);
Observe observe{};
Observe observe_created{};
struct Feature { unsigned id; MockNgxParameters created; NgxHandle* lower{}; };
using Create12 = NgxResult(*)(ID3D12GraphicsCommandList*, unsigned, NgxParameters*, NgxHandle**);
using Evaluate12 = NgxResult(*)(ID3D12GraphicsCommandList*, const NgxHandle*, const NgxParameters*, NgxProgressCallback);
using Evaluate12C = NgxResult(*)(ID3D12GraphicsCommandList*, const NgxHandle*, const NgxParameters*, NgxProgressCallbackC);
using Release12 = NgxResult(*)(NgxHandle*);
Create12 forward_create{};
Evaluate12 forward_evaluate{};
Evaluate12C forward_evaluate_c{};
Release12 forward_release{};
using Create11 = NgxResult(*)(ID3D11DeviceContext*, unsigned, NgxParameters*, NgxHandle**);
using Evaluate11 = NgxResult(*)(ID3D11DeviceContext*, const NgxHandle*, const NgxParameters*, NgxProgressCallback);
using Evaluate11C = NgxResult(*)(ID3D11DeviceContext*, const NgxHandle*, const NgxParameters*, NgxProgressCallbackC);
Create11 forward_create11{};
Evaluate11 forward_evaluate11{};
Evaluate11C forward_evaluate11_c{};
Release12 forward_release11{};
bool fail_evaluation{};
bool copy_nr_color{};
bool require_feature_path{};
bool fail_initialization{};
std::atomic<unsigned> initializations{};
unsigned fail_next{};
NgxResult evaluate(const NgxHandle* handle, const NgxParameters* params) {
    ++evaluates;
    if (observe_created) observe_created(&reinterpret_cast<const Feature*>(handle)->created);
    if (observe) observe(params);
    if (observe_handle) observe_handle(handle, params);
    if (fail_next) { --fail_next; return 0xBAD00007U; }
    return fail_evaluation ? 0xBAD00007U : 1U;
}
}
#define EXPORT extern "C" __declspec(dllexport) __declspec(noinline)
EXPORT unsigned CheekyFakeCreates() { return creates.load(); }
EXPORT unsigned NVSDK_NGX_GetSnippetVersion() { return 0x01360900U; }
EXPORT unsigned CheekyFakeEvaluates() { return evaluates.load(); }
EXPORT unsigned CheekyFakeReleases() { return releases.load(); }
EXPORT void CheekyFakeObserve(Observe callback) { observe = callback; }
EXPORT void CheekyFakeObserveHandle(ObserveHandle callback) { observe_handle = callback; }
EXPORT void CheekyFakeGazeSnapshot(const CheekyGazeSnapshotV1* value) { fake_gaze = *value; }
EXPORT unsigned __cdecl CheekyOpenXR_GetGazeSnapshot(unsigned abi, void* output, unsigned size) {
    if (abi != CHEEKY_GAZE_ABI_VERSION || size < sizeof(fake_gaze) || !output) return 0;
    memcpy(output, &fake_gaze, sizeof(fake_gaze)); return 1;
}
EXPORT void __cdecl CheekyOpenXR_SetSimulatedGaze(unsigned) {}
EXPORT void __cdecl CheekyOpenXR_SetSimulationPattern(unsigned) {}
EXPORT void CheekyFakeObserveCreated(Observe callback) { observe_created = callback; }
EXPORT void CheekyFakeFailEvaluations(bool fail) { fail_evaluation = fail; }
EXPORT void CheekyFakeCopyNrColor(bool enabled) { copy_nr_color = enabled; }
EXPORT void CheekyFakeRequireFeaturePath(bool enabled) { require_feature_path = enabled; }
EXPORT void CheekyFakeFailInitialization(bool fail) { fail_initialization = fail; }
EXPORT unsigned CheekyFakeInitializations() { return initializations.load(); }
EXPORT void CheekyFakeFailNextEvaluations(unsigned count) { fail_next = count; }
// A separately loaded copy acts as the core runtime and deliberately wraps the
// lower handle. Cache these addresses before Cheeky installs its real detours.
EXPORT void CheekyFakeForwardTo(HMODULE lower, bool use_c) {
    forward_create = reinterpret_cast<Create12>(GetProcAddress(lower, "NVSDK_NGX_D3D12_CreateFeature"));
    forward_release = reinterpret_cast<Release12>(GetProcAddress(lower, "NVSDK_NGX_D3D12_ReleaseFeature"));
    forward_evaluate = use_c ? nullptr : reinterpret_cast<Evaluate12>(GetProcAddress(lower, "NVSDK_NGX_D3D12_EvaluateFeature"));
    forward_evaluate_c = use_c ? reinterpret_cast<Evaluate12C>(GetProcAddress(lower, "NVSDK_NGX_D3D12_EvaluateFeature_C")) : nullptr;
}
EXPORT void CheekyFakeForwardDX11To(HMODULE lower) {
    forward_create11 = reinterpret_cast<Create11>(GetProcAddress(lower, "NVSDK_NGX_D3D11_CreateFeature"));
    forward_evaluate11 = reinterpret_cast<Evaluate11>(GetProcAddress(lower, "NVSDK_NGX_D3D11_EvaluateFeature"));
    forward_evaluate11_c = reinterpret_cast<Evaluate11C>(GetProcAddress(lower, "NVSDK_NGX_D3D11_EvaluateFeature_C"));
    forward_release11 = reinterpret_cast<Release12>(GetProcAddress(lower, "NVSDK_NGX_D3D11_ReleaseFeature"));
}
// Only the test fixture exports these stubs. No real AFW binary is executed.
EXPORT void InitDevice() {}
EXPORT void InitFrameWarp() {}
EXPORT void __stdcall EvaluateFrameWarp(void* parameters) { last_warp_parameters = parameters; ++warp_calls; }
EXPORT void* CheekyFakeLastWarpParameters() { return last_warp_parameters; }
EXPORT unsigned CheekyFakeWarpCalls() { return warp_calls; }
// The hook harness loads a second copy as its optional feature-18 runtime.
EXPORT NgxResult NVSDK_NGX_D3D12_Init_Ext(unsigned long long, const wchar_t*, ID3D12Device*, unsigned, const void* common) {
    ++initializations;
    if (fail_initialization) return 0xBAD00002U;
    if (require_feature_path) {
        const auto* info = static_cast<const NgxFeatureCommonInfo*>(common);
        bool found{};
        if (info && info->path_list.paths) {
            for (unsigned i = 0; i < info->path_list.count; ++i) {
                const std::wstring file = std::wstring(info->path_list.paths[i]) + L"\\nvngx_dlss.dll";
                found = found || GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
            }
        }
        if (!found) return 0xBAD0000BU;
    }
    wchar_t path[MAX_PATH]{};
    return GetModuleFileNameW(nullptr, path, MAX_PATH) ? 1U : 0xBAD00007U;
}
EXPORT NgxResult NVSDK_NGX_D3D12_AllocateParameters(NgxParameters** out) { *out = new MockNgxParameters; return 1U; }
EXPORT NgxResult NVSDK_NGX_D3D12_DestroyParameters(NgxParameters* params) { delete static_cast<MockNgxParameters*>(params); return 1U; }
EXPORT NgxResult NVSDK_NGX_D3D12_Shutdown1(ID3D12Device*) { return 1U; }
EXPORT NgxResult NVSDK_NGX_D3D11_Init(unsigned long long, const wchar_t*, ID3D11Device*, const void*, unsigned) { return 1U; }
EXPORT NgxResult NVSDK_NGX_D3D12_Init(unsigned long long, const wchar_t*, ID3D12Device*, const void*, unsigned) { return 1U; }
EXPORT NgxResult NVSDK_NGX_D3D11_CreateFeature(ID3D11DeviceContext* context, unsigned feature, NgxParameters* params, NgxHandle** out) {
    NgxHandle* lower{};
    if (forward_create11) {
        const auto result = forward_create11(context, feature, params, &lower);
        if (!ngx_succeeded(result)) return result;
    }
    *out=reinterpret_cast<NgxHandle*>(new Feature{++creates, {}, lower}); return 1U;
}
EXPORT NgxResult NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList* list, unsigned feature, NgxParameters* params, NgxHandle** out) {
    NgxHandle* lower{};
    if (forward_create) {
        const auto result = forward_create(list, feature, params, &lower);
        if (!ngx_succeeded(result)) return result;
    }
    *out=reinterpret_cast<NgxHandle*>(new Feature{++creates, *static_cast<MockNgxParameters*>(params), lower}); return 1U;
}
EXPORT NgxResult NVSDK_NGX_D3D11_EvaluateFeature(ID3D11DeviceContext* context, const NgxHandle* handle, const NgxParameters* params, NgxProgressCallback callback) {
    ++evaluates;
    return forward_evaluate11 ? forward_evaluate11(context, reinterpret_cast<const Feature*>(handle)->lower, params, callback) : 1U;
}
EXPORT NgxResult NVSDK_NGX_D3D11_EvaluateFeature_C(ID3D11DeviceContext* context, const NgxHandle* handle, const NgxParameters* params, NgxProgressCallbackC callback) {
    ++evaluates;
    return forward_evaluate11_c ? forward_evaluate11_c(context, reinterpret_cast<const Feature*>(handle)->lower, params, callback) : 1U;
}
EXPORT NgxResult NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList* list, const NgxHandle* handle, const NgxParameters* params, NgxProgressCallback callback) {
    const auto result = evaluate(handle, params);
    if (ngx_succeeded(result) && forward_evaluate)
        return forward_evaluate(list, reinterpret_cast<const Feature*>(handle)->lower, params, callback);
    if (ngx_succeeded(result) && forward_evaluate_c)
        return forward_evaluate_c(list, reinterpret_cast<const Feature*>(handle)->lower, params, nullptr);
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
EXPORT NgxResult NVSDK_NGX_D3D12_EvaluateFeature_C(ID3D12GraphicsCommandList* list, const NgxHandle* handle, const NgxParameters* params, NgxProgressCallbackC callback) {
    const auto result = evaluate(handle, params);
    if (ngx_succeeded(result) && forward_evaluate)
        return forward_evaluate(list, reinterpret_cast<const Feature*>(handle)->lower, params, nullptr);
    if (ngx_succeeded(result) && forward_evaluate_c)
        return forward_evaluate_c(list, reinterpret_cast<const Feature*>(handle)->lower, params, callback);
    return result;
}
EXPORT NgxResult NVSDK_NGX_D3D11_ReleaseFeature(NgxHandle* handle) {
    auto* feature = reinterpret_cast<Feature*>(handle);
    if (forward_release11 && feature->lower) forward_release11(feature->lower);
    ++releases; delete feature; return 1U;
}
EXPORT NgxResult NVSDK_NGX_D3D12_ReleaseFeature(NgxHandle* handle) {
    auto* feature = reinterpret_cast<Feature*>(handle);
    if (forward_release && feature->lower) forward_release(feature->lower);
    ++releases; delete feature; return 1U;
}
