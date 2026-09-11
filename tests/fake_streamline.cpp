#include "streamline_abi.hpp"
#include "ngx_abi.hpp"
#include <atomic>
#include <cstring>
#include <map>
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
    struct View {
        void* color{}; unsigned reset{}; bool has_reset{};
        std::map<unsigned, SlResource> resources;
        std::map<unsigned, SlExtent> extents;
    };
    std::map<unsigned, View> views;
    bool fail_color_tag{};
    unsigned submit_tags(const void* viewport, const void* tags, unsigned count) {
        auto& view = views[static_cast<const SlViewportHandle*>(viewport)->value];
        const auto* values = static_cast<const SlResourceTag*>(tags);
        for (unsigned i = 0; i < count; ++i) {
            if (values[i].type == 3U) view.color = values[i].resource->native;
            view.resources[values[i].type] = *values[i].resource;
            view.extents[values[i].type] = values[i].extent;
        }
        // Deliberately consume the replacement before rejecting it, exercising
        // restoration of all tags after partial submission failure.
        return fail_color_tag && count == 1U && values[0].type == 3U ? 0x18U : 0U;
    }
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
EXPORT void CheekyFakeFailColorTag(bool fail) { fail_color_tag = fail; }
EXPORT bool CheekyFakeTagsMatch(unsigned viewport, const SlResourceTag* tags, unsigned count) {
    const auto& view = views[viewport];
    for (unsigned i = 0; i < count; ++i) {
        const auto resource = view.resources.find(tags[i].type);
        const auto extent = view.extents.find(tags[i].type);
        if (resource == view.resources.end() || extent == view.extents.end() ||
            resource->second.native != tags[i].resource->native ||
            resource->second.state != tags[i].resource->state ||
            std::memcmp(&extent->second, &tags[i].extent, sizeof(SlExtent)) != 0) return false;
    }
    return true;
}
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
EXPORT unsigned slEvaluateFeature(unsigned feature,const void*,const void* const* inputs,unsigned count,void* command) {
    ++evaluate_calls;
    if(feature!=0U || (!cached_evaluate && !cached_evaluate11)) return 0x18U;
    ID3D12Resource* original_color{};
    unsigned original_reset{};
    const View* view = count && inputs && inputs[0]
        ? &views[static_cast<const SlViewportHandle*>(inputs[0])->value] : nullptr;
    const bool tagged = !cached_evaluate11 && view && view->color;
    if (tagged) {
        parameters->Get("Color", &original_color);
        parameters->Get("Reset", &original_reset);
        parameters->Set("Color", static_cast<ID3D12Resource*>(view->color));
        if (view->has_reset) parameters->Set("Reset", view->reset);
    }
    const auto result=cached_evaluate11
        ? cached_evaluate11(static_cast<ID3D11DeviceContext*>(command),game_handle,parameters,nullptr)
        : cached_evaluate(static_cast<ID3D12GraphicsCommandList*>(command),game_handle,parameters,nullptr);
    if (tagged) {
        parameters->Set("Color", original_color);
        parameters->Set("Reset", original_reset);
    }
    return ngx_succeeded(result) ? 0U : 0x18U;
}
EXPORT unsigned slSetTag(const void* viewport,const void* tags,unsigned count,void*) {
    ++evaluate_calls;
    return submit_tags(viewport,tags,count);
}
EXPORT unsigned slSetTagForFrame(const void* frame,const void* viewport,const void* tags,unsigned count,void* command) {
    (void)frame;
    (void)command;
    ++evaluate_calls;
    return submit_tags(viewport,tags,count);
}
EXPORT unsigned slSetConstants(const void* constants,const void*,const void* viewport) {
    ++evaluate_calls;
    auto& view = views[static_cast<const SlViewportHandle*>(viewport)->value];
    view.reset = static_cast<unsigned>(static_cast<const SlConstants*>(constants)->reset);
    view.has_reset = true;
    return 0U;
}
