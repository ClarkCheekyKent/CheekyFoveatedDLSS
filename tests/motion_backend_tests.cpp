#include "backend.hpp"
#include "gaze_foveation.hpp"
#include "openvr_gaze.hpp"
#include "peripheral_dlaa.hpp"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <map>
#include <string>
#include <iostream>
#include <cstdlib>
using namespace cheeky::foveated_dlss;
using Microsoft::WRL::ComPtr;
namespace cheeky::foveated_dlss {
void trace_event(const char*, ...) noexcept {}
void release_dlss_nr_view(DlssViewId) noexcept {}
void release_dlss_nr_resources() noexcept {}
// Motion backend tests run without a live VR runtime.
bool read_openvr_gaze(const Settings&, IUnknown*, CheekyGazeSnapshotV1&) noexcept { return false; }
}
void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
struct Parameters : NgxParameters {
    std::map<std::string, unsigned> ints;
    std::map<std::string, ID3D12Resource*> resources;
    void Set(const char*, unsigned long long) override {}
    void Set(const char*, float) override {}
    void Set(const char*, double) override {}
    void Set(const char* n, unsigned v) override { ints[n] = v; }
    void Set(const char* n, int v) override { ints[n] = static_cast<unsigned>(v); }
    void Set(const char*, ID3D11Resource*) override {}
    void Set(const char* n, ID3D12Resource* v) override { resources[n] = v; }
    void Set(const char*, void*) override {}
    NgxResult Get(const char*, unsigned long long*) const override { return 0xBAD00005U; }
    NgxResult Get(const char*, float*) const override { return 0xBAD00005U; }
    NgxResult Get(const char*, double*) const override { return 0xBAD00005U; }
    NgxResult Get(const char* n, unsigned* v) const override {
        auto it = ints.find(n); if (it == ints.end()) return 0xBAD00005U; *v = it->second; return 1;
    }
    NgxResult Get(const char* n, int* v) const override {
        unsigned value{}; auto r = Get(n, &value); *v = static_cast<int>(value); return r;
    }
    NgxResult Get(const char*, ID3D11Resource**) const override { return 0xBAD00005U; }
    NgxResult Get(const char* n, ID3D12Resource** v) const override {
        auto it = resources.find(n); if (it == resources.end()) return 0xBAD00005U; *v = it->second; return 1;
    }
    NgxResult Get(const char*, void**) const override { return 0xBAD00005U; }
    void Reset() override {}
};
unsigned creates{}, evaluations{}, releases{}, seen_flags{}, seen_reset{};
bool fail_evaluation{};
NgxResult create(ID3D12GraphicsCommandList*, unsigned, NgxParameters* p, NgxHandle** h) {
    ++creates; seen_flags = get_ngx_integer_bits(p, "DLSS.Feature.Create.Flags");
    *h = reinterpret_cast<NgxHandle*>(std::uintptr_t{creates}); return 1;
}
NgxResult evaluate(ID3D12GraphicsCommandList*, const NgxHandle*, const NgxParameters* p, NgxProgressCallback) {
    ++evaluations; seen_reset = get_ui(p, "Reset"); return fail_evaluation ? 0xBAD00005U : 1U;
}
NgxResult release(NgxHandle*) { ++releases; return 1; }
int main() {
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> adapter; ComPtr<ID3D12Device> device;
    check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
    check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "WARP adapter");
    check(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "WARP device");
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
    check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))), "allocator");
    check(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))), "list");
    const auto texture = [&](unsigned w, unsigned h) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w; desc.Height = h; desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r;
        check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&r))), "texture");
        return r;
    };
    auto color = texture(4936,1189), depth = texture(4936,1189), mv = texture(4936,1189), output = texture(7404,1784);
    Parameters p;
    p.resources = {{"Color",color.Get()},{"Depth",depth.Get()},{"MotionVectors",mv.Get()},{"Output",output.Get()}};
    p.ints = {{"Width",2468},{"Height",1189},{"OutWidth",3702},{"OutHeight",1784},
        {"DLSS.Feature.Create.Flags",2},{"Reset",0}};
    Settings settings{}; settings.enabled = true;
    CropGeometry crop{100,304,1800,486,150,456,2700,729};
    DlssFrameContract contract{}; contract.view_id = 1; contract.feature_id = 1;
    contract.create_flags = 2; contract.motion_vectors_low_res = true;
    contract.preserve_history_on_crop_move = true;
    contract.motion_vector_scale_x = contract.motion_vector_scale_y = 1;
    D3D12BackendCallbacks callbacks{create,evaluate,release};
    const auto run = [&]() {
        ScopedCoordinatedCrop override{1,crop,false};
        auto* prepared = prepare_d3d12(list.Get(),&p,1,settings);
        if (!prepared) return false;
        check(d3d12_evaluation_low_res_motion(prepared) == contract.motion_vectors_low_res, "center disagrees with declaration");
        D3D12DlssInputs inputs{};
        inputs.color=color.Get(); inputs.depth=depth.Get(); inputs.motion_vectors=mv.Get();
        inputs.output=d3d12_private_output(prepared);
        inputs.mv_base_x=get_ui(&p,"DLSS.Input.MV.Subrect.Base.X"); inputs.mv_base_y=get_ui(&p,"DLSS.Input.MV.Subrect.Base.Y");
        auto result=evaluate_d3d12_backend(list.Get(),contract,inputs,&p,crop,callbacks);
        // Exercise failure cleanup without recording a compositor dispatch. NVIDIA
        // callbacks are fakes; no output pixels were produced by this test.
        finish_d3d12(list.Get(),&p,prepared,0xBAD00005U);
        check(p.resources.at("Output")==output.Get() && get_ui(&p,"Width")==2468 &&
            get_ui(&p,"DLSS.Input.MV.Subrect.Base.Y")==0 && get_ui(&p,"Reset")==0 &&
            get_ui(&p,"DLSS.Render.Subrect.Dimensions.Width")==0, "original parameter restoration");
        return ngx_succeeded(result);
    };
    check(run() && creates==1 && seen_flags==2 && seen_reset==1, "first private evaluation");
    crop.input_base_y=312; crop.output_base_y=468;
    check(run() && creates==1 && seen_reset==0, "fixed crop movement recreated/reset feature");
    for (unsigned y : {304U,312U,304U,312U}) {
        crop.input_base_y=y; crop.output_base_y=y*3/2;
        check(run() && creates==1 && seen_flags==2 && seen_reset==0, "repeated crop movement changed feature/space");
    }
    const auto before = evaluations;
    p.ints.erase("DLSS.Feature.Create.Flags");
    check(!run() && evaluations==before && p.resources.at("Output")==output.Get(), "missing declaration touched custom evaluation/output");
    p.Set("DLSS.Feature.Create.Flags",2U);
    check(run() && creates==1 && seen_reset==1, "skipped history did not reset on resume");
    p.Set("DLSS.Feature.Create.Flags",0U); contract.create_flags=0; contract.motion_vectors_low_res=false;
    check(!run() && evaluations==before+1, "invalid high mapping was reinterpreted");
    crop={0,0,100,100,0,0,100,100};
    check(run() && creates==2 && seen_flags==0, "explicit high/equal dimensions or legitimate flag change");
    p.Set("DLSS.Feature.Create.Flags",2U); contract.create_flags=2; contract.motion_vectors_low_res=true;
    check(run() && creates==3 && seen_flags==2, "flag-only key change ignored");
    p.Set("DLSS.Feature.Create.Flags",0U); contract.create_flags=0; contract.motion_vectors_low_res=false;
    check(run() && creates==4 && seen_flags==0, "high flag-only key change ignored");
    contract.perf_quality=3;
    check(run() && creates==5, "quality key change ignored");
    p.Set("DLSS.Hint.Render.Preset.DLAA",7U);
    check(run() && creates==6, "preset key change ignored");
    crop.input_width=104;
    check(run() && creates==7, "dimension key change ignored");
    fail_evaluation=true;
    check(!run(), "failed private evaluation");
    fail_evaluation=false;
    check(run() && seen_reset==1, "failed history did not reset");
    // Full peripheral region is invalid even though a center region fits.
    PeripheralDlaaRequest request{}; request.command_list=list.Get(); request.view_id=1;
    request.color=color.Get(); request.depth=depth.Get(); request.motion_vectors=mv.Get(); request.output_template=output.Get();
    request.render_width=2468; request.render_height=1189; request.source_output_width=3702; request.source_output_height=1784;
    request.parameters=&p; request.create_flags=0; request.motion_vectors_output_space=true;
    PeripheralDlaaResources peripheral{};
    check(!prepare_peripheral_dlaa_resources(request,peripheral) && peripheral.output==nullptr, "invalid peripheral pass not disabled");
    // Peripheral working dimensions are equal; even after resampling, its
    // private feature must retain an explicit high-resolution declaration.
    request.render_width=128; request.render_height=128;
    request.source_output_width=256; request.source_output_height=256;
    request.scale=1.0F; request.callbacks=callbacks;
    NgxResult peripheral_result{};
    const auto before_peripheral=creates;
    check(evaluate_peripheral_dlaa_ngx(request,peripheral,peripheral_result) &&
        creates==before_peripheral+1 && seen_flags==0, "peripheral rewrote declared high flag");
    restore_peripheral_dlaa_output(list.Get(),peripheral);
    const auto before_missing=evaluations;
    p.ints.erase("DLSS.Feature.Create.Flags");
    check(!evaluate_peripheral_dlaa_ngx(request,peripheral,peripheral_result) &&
        evaluations==before_missing, "missing peripheral declaration evaluated");
    p.Set("DLSS.Feature.Create.Flags",0U);
    check(evaluate_peripheral_dlaa_ngx(request,peripheral,peripheral_result) &&
        creates==before_peripheral+1 && seen_reset==1, "peripheral skipped history not reset/reused");
    restore_peripheral_dlaa_output(list.Get(),peripheral);
    p.resources["MotionVectors"]=nullptr;
    const auto before_missing_mv=evaluations;
    check(!run() && evaluations==before_missing_mv, "missing center resource evaluated");
    p.resources["MotionVectors"]=mv.Get();
    release_d3d12_view(peripheral_dlaa_view_id(1));
    release_d3d12_view(1);
    check(releases==creates, "private feature lifecycle leak");
    check(SUCCEEDED(list->Close()), "command list close");
    std::cout << "Motion backend regressions passed\n";
}
