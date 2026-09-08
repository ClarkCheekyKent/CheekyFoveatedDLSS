#include "streamline_abi.hpp"
#include "ngx_abi.hpp"
#include <atomic>
#include <cstring>
using namespace cheeky::foveated_dlss;
namespace {
    std::atomic<unsigned> option_calls{}, get_calls{}, evaluate_calls{};
    SlDlssOptions latest{};
    using Evaluate=NgxResult(*)(ID3D12GraphicsCommandList*,const NgxHandle*,const NgxParameters*,NgxProgressCallback);
    Evaluate cached_evaluate{};
    using Evaluate11=NgxResult(*)(ID3D11DeviceContext*,const NgxHandle*,const NgxParameters*,NgxProgressCallback);
    Evaluate11 cached_evaluate11{};
    const NgxHandle* game_handle{};
    NgxParameters* parameters{};
}
#define EXPORT extern "C" __declspec(dllexport) __declspec(noinline)
EXPORT void CheekyFakeConfigure(Evaluate evaluate,const NgxHandle* handle,NgxParameters* params) {
    cached_evaluate=evaluate; game_handle=handle; parameters=params;
}
EXPORT void CheekyFakeConfigure11(Evaluate11 evaluate,const NgxHandle* handle,NgxParameters* params) {
    cached_evaluate11=evaluate; game_handle=handle; parameters=params;
}
EXPORT unsigned CheekyFakeOptionCalls() { return option_calls.load(); }
EXPORT unsigned CheekyFakeGetCalls() { return get_calls.load(); }
EXPORT unsigned CheekyFakeOptionWidth() { return latest.output_width; }
EXPORT unsigned CheekyFakeOptionHeight() { return latest.output_height; }
EXPORT unsigned slDLSSSetOptions(const void*, const SlDlssOptions* options) {
    ++option_calls;
    if (!options) return 0x18U;
    // Test-only fixture receives complete storage even in its version tests.
    latest=*options; return 0U;
}
EXPORT unsigned slGetFeatureFunction(unsigned feature,const char* name,void** out) {
    ++get_calls;
    if(feature!=0U || !name || !out || std::strcmp(name,"slDLSSSetOptions")!=0) return 0x18U;
    *out=reinterpret_cast<void*>(&slDLSSSetOptions); return 0U;
}
EXPORT unsigned slEvaluateFeature(unsigned feature,const void*,const void* const*,unsigned,void* command) {
    ++evaluate_calls;
    if(feature!=0U || (!cached_evaluate && !cached_evaluate11)) return 0x18U;
    const auto result=cached_evaluate11
        ? cached_evaluate11(static_cast<ID3D11DeviceContext*>(command),game_handle,parameters,nullptr)
        : cached_evaluate(static_cast<ID3D12GraphicsCommandList*>(command),game_handle,parameters,nullptr);
    return ngx_succeeded(result) ? 0U : 0x18U;
}
EXPORT unsigned slSetTag(const void*,const void*,unsigned,void*) { return ++evaluate_calls,0U; }
EXPORT unsigned slSetTagForFrame(const void*,const void*,const void*,unsigned,void*) { return ++evaluate_calls,0U; }
EXPORT unsigned slSetConstants(const void*,const void*,const void*) { return ++evaluate_calls,0U; }
