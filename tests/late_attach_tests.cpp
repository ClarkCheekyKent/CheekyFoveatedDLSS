#include "late_attach_tests.hpp"
#include "mock_ngx_parameters.hpp"
#include "streamline_abi.hpp"
#include "timing_list_alias.hpp"
#include "../shared/cheeky_gaze_abi.h"
#include <filesystem>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <stdexcept>
#include <cstdio>
#include <cmath>
#include <DirectXPackedVector.h>
using namespace cheeky::foveated_dlss;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
void check(HRESULT hr,const char* why) { require(SUCCEEDED(hr),why); }
template<class T> T proc(HMODULE module,const char* name) {
    auto result=reinterpret_cast<T>(GetProcAddress(module,name)); require(result!=nullptr,name); return result;
}
using Create11=NgxResult(*)(ID3D11DeviceContext*,unsigned,NgxParameters*,NgxHandle**);
using Create12=NgxResult(*)(ID3D12GraphicsCommandList*,unsigned,NgxParameters*,NgxHandle**);
using Evaluate11=NgxResult(*)(ID3D11DeviceContext*,const NgxHandle*,const NgxParameters*,NgxProgressCallback);
using Evaluate11C=NgxResult(*)(ID3D11DeviceContext*,const NgxHandle*,const NgxParameters*,NgxProgressCallbackC);
using Evaluate12=NgxResult(*)(ID3D12GraphicsCommandList*,const NgxHandle*,const NgxParameters*,NgxProgressCallback);
using Evaluate12C=NgxResult(*)(ID3D12GraphicsCommandList*,const NgxHandle*,const NgxParameters*,NgxProgressCallbackC);
using Release=NgxResult(*)(NgxHandle*);
using Counter=unsigned(*)();
using SlEvaluate=unsigned(*)(unsigned,const void*,const void* const*,unsigned,void*);
using SlOptions=unsigned(*)(const void*,const SlDlssOptions*);
struct SrInput { ID3D12Resource* color{}; unsigned reset{}; };
std::vector<SrInput> sr_inputs;
std::string evaluation_order;
unsigned nr_reset{};
const NgxHandle* nr_last_handle{};
void observe_nr_handle(const NgxHandle* handle, const NgxParameters*) { nr_last_handle = handle; }
float nr_motion_x{};
unsigned nr_motion_width{};
ComPtr<ID3D12Resource> nr_motion_resource;
MockNgxParameters nr_created;
void observe_nr_created(const NgxParameters* params) {
    nr_created.values = static_cast<const MockNgxParameters*>(params)->values;
}
void observe_sr(const NgxParameters* params) {
    evaluation_order += 'S';
    SrInput input;
    params->Get("Color", &input.color);
    params->Get("Reset", &input.reset);
    sr_inputs.push_back(input);
}
void observe_nr(const NgxParameters* params) {
    evaluation_order += 'N';
    params->Get("DLSSNR.Reset", &nr_reset);
    params->Get("DLSSNR.MVecScaleX", &nr_motion_x);
    params->Get("DLSSNR.MVecSubrectWidth", &nr_motion_width);
    ID3D12Resource* mv{}; params->Get("DLSSNR.MVec", &mv); nr_motion_resource=mv;
}
struct FrameToken : SlFrameToken {
    unsigned index{};
    operator std::uint32_t() const override { return index; }
};
struct Fixture {
    HMODULE ngx{},sl{}; MockNgxParameters params;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Device> device;
    std::vector<ComPtr<ID3D11Texture2D>> textures11;
    std::vector<ComPtr<ID3D12Resource>> textures12;
    Create11 create11{}; Create12 create12{};
    Evaluate11 evaluate11{}; Evaluate11C evaluate11c{};
    Evaluate12 evaluate12{}; Evaluate12C evaluate12c{};
    Release release{}; Counter creates{},evaluates{},releases{};
    SlEvaluate sl_evaluate{}; SlOptions sl_options{};
    SlViewportHandle viewport{}; SlDlssOptions options{};
    FrameToken frame;
    bool complete_sl_metadata{};
    bool missing_sl_constants{}, ambiguous_sl_inputs{}, cache_other_view{};
    std::array<SlResource, 4> resources{};
    std::array<SlResourceTag, 4> tags{};
    void submit_metadata() {
        if (!complete_sl_metadata) return;
        const unsigned types[]{3U, 0U, 1U, 4U};
        for (unsigned i = 0; i < 4; ++i) {
            resources[i].native = textures12[i].Get();
            resources[i].state = i == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            tags[i].resource = &resources[i]; tags[i].type = types[i];
            tags[i].extent.width = tags[i].extent.height = i == 3 ? 256U : 128U;
        }
        SlConstants constants{};
        constants.struct_version = 1;
        constants.motion_vector_scale = {1.F, 1.F};
        params.Get("Jitter.Offset.X", &constants.jitter_offset.x);
        params.Get("Jitter.Offset.Y", &constants.jitter_offset.y);
        constants.motion_vectors_jittered = (get_ui(&params,"DLSS.Feature.Create.Flags") & 4U) != 0;
        constants.reset = static_cast<char>(get_ui(&params, "Reset"));
        require(proc<unsigned(*)(const void*,const void*,const void*,unsigned,void*)>(sl,"slSetTagForFrame")(
            &frame,&viewport,tags.data(),4,list.Get()) == 0, "Submit complete viewport tags");
        if (!missing_sl_constants)
            require(proc<unsigned(*)(const void*,const void*,const void*)>(sl,"slSetConstants")(
                &constants,&frame,&viewport) == 0, "Submit current viewport constants");
        if (cache_other_view) {
            auto other = viewport; ++other.value;
            require(proc<unsigned(*)(const void*,const void*,const void*,unsigned,void*)>(sl,"slSetTagForFrame")(
                &frame,&other,tags.data(),4,list.Get()) == 0, "Cache another viewport last");
            require(proc<unsigned(*)(const void*,const void*,const void*)>(sl,"slSetConstants")(
                &constants,&frame,&other) == 0, "Cache another viewport's constants last");
        }
    }
    NgxHandle* handle{}; bool use_c{},use_sl{};
    void (*before_frame)(){};
    NgxResult evaluate() {
        if (before_frame) before_frame();
        if(use_sl) { ++frame.index; submit_metadata(); const void* inputs[]{&viewport, &viewport}; return sl_evaluate(0,complete_sl_metadata ? &frame : nullptr,inputs,ambiguous_sl_inputs ? 2U : 1U,context ? static_cast<void*>(context.Get()) : static_cast<void*>(list.Get())); }
        if(context) return use_c ? evaluate11c(context.Get(),handle,&params,nullptr) : evaluate11(context.Get(),handle,&params,nullptr);
        return use_c ? evaluate12c(list.Get(),handle,&params,nullptr) : evaluate12(list.Get(),handle,&params,nullptr);
    }
    void finish_gpu() {
        if(context) { context->Flush(); return; }
        check(list->Close(),"Close late-attach list"); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1,lists);
        ComPtr<ID3D12Fence> fence; check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Late fence");
        check(queue->Signal(fence.Get(),1),"Late signal");
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); require(event!=nullptr,"Late event");
        check(fence->SetEventOnCompletion(1,event),"Late fence event");
        auto result=WaitForSingleObject(event,10000); CloseHandle(event); require(result==WAIT_OBJECT_0,"Late GPU timeout");
        check(allocator->Reset(),"Late allocator reset"); check(list->Reset(allocator.Get(),nullptr),"Late list reset");
    }
};
Fixture& fixture() { static auto* f=new Fixture; return *f; }
HMODULE afw_core{};
HMODULE afw_warp_module{};
void (__stdcall* afw_cached_warp)(void*){};
unsigned afw_largest_output_width{};
bool afw_missing_lower{};
bool afw_public_first{};
bool afw_ota{}, afw_ambiguous{};
unsigned afw_full_calls{}, afw_lower_calls{}, afw_reduced_depth_calls{}, afw_full_resets{};
unsigned afw_expected_reset{};
struct AfwHistorySample { const NgxHandle* handle{}; unsigned reset{}; };
std::vector<AfwHistorySample> afw_history;
void observe_afw_history(const NgxHandle* handle, const NgxParameters* params) {
    afw_history.push_back({handle, get_ui(params, "Reset")});
}
bool afw_contract_ok{true}, afw_order_ok{true};
void observe_afw_core(const NgxParameters* params) {
    evaluation_order += 'A';
    ++afw_full_calls;
    auto& f = fixture();
    const char* names[]{"Color", "Depth", "MotionVectors", "Output"};
    for (unsigned i = 0; i < 4; ++i) {
        ID3D12Resource* resource{}; params->Get(names[i], &resource);
        afw_contract_ok &= resource == f.textures12[i].Get();
        if (resource) afw_contract_ok &= resource->GetDesc().Width == (i == 3 ? 256U : 128U) && resource->GetDesc().Height == (i == 3 ? 256U : 128U);
    }
    afw_contract_ok &= get_ui(params, "Width") == 128 && get_ui(params, "OutWidth") == 256 && get_ui(params, "Reset") == afw_expected_reset;
    // Model AFW's pre-DLSS work. The lower observer must run after this update.
    const_cast<NgxParameters*>(params)->Set("CheekyFake.AfwCorrected", afw_full_calls);
}
void observe_afw_lower(const NgxParameters* params) {
    observe_sr(params);
    ++afw_lower_calls;
    afw_order_ok &= get_ui(params, "CheekyFake.AfwCorrected") == afw_full_calls;
    ID3D12Resource* depth{}; params->Get("Depth", &depth);
    if (depth && depth->GetDesc().Width < 128) ++afw_reduced_depth_calls;
    ID3D12Resource* output{}; params->Get("Output", &output);
    if (output) afw_largest_output_width = (std::max)(afw_largest_output_width, static_cast<unsigned>(output->GetDesc().Width));
    if (output == fixture().textures12[3].Get()) afw_full_resets += get_ui(params, "Reset") != 0;
}
std::string snapshot(CheekyUEVRSnapshotFn get) { std::vector<char> text(32768); require(get(text.data(),32768),"Late snapshot"); return text.data(); }
void verify_nr_reset_isolation(void (*command)(const char*)) {
    auto& f = fixture();
    if (f.context) return;
    f.params.Set("Jitter.Offset.X", 0.F); f.params.Set("Jitter.Offset.Y", 0.F);
    f.params.Set("DLSS.Hint.Render.Preset.DLAA", 0U);
    proc<void(*)(void(*)(const NgxParameters*))>(f.ngx,"CheekyFakeObserve")(&observe_sr);
    if (f.use_sl) {
        f.options.struct_version = 3;
        require(f.sl_options(&f.viewport,&f.options) == 0, "Restore supported viewport options");
        f.complete_sl_metadata = true;
    }
    command("1\n90\nset\nEnabled=false\nNrEnabled=true\nNrProcessingOrder=0\nNrFoveated=true\nAutoStereoAlignment=true\nCenterMode=0\nWidth=0.5\nHeight=0.5\nXOffset=0\nHeightOffset=-1\nAlignmentBorder=false\nNrAlignmentBorder=false");
    HMODULE nr{};
    const auto evaluate = [&](unsigned host_reset) {
        f.params.Set("Reset", host_reset);
        const auto original = f.params.values;
        sr_inputs.clear();
        evaluation_order.clear();
        require(ngx_succeeded(f.evaluate()), "NR isolation SR evaluation");
        f.finish_gpu();
        if (f.params.values != original) {
            for (const auto& [key, value] : f.params.values) {
                const auto found = original.find(key);
                if (found == original.end() || found->second != value)
                    printf("NR parameter changed: %s\n", key.c_str());
            }
        }
        require(f.params.values == original, "NR scope did not restore original parameters");
        if (f.use_sl)
            require(proc<bool(*)(unsigned,const SlResourceTag*,unsigned)>(f.sl,"CheekyFakeTagsMatch")(
                f.viewport.value,f.tags.data(),4), "NR scope did not restore original viewport tags");
    };
    evaluate(0); // Establish the coordinated region without relying on timing.
    nr = GetModuleHandleW(L"nvngx_dlssnr.dll");
    require(nr != nullptr, "Hook fixture loaded its local fake NR runtime");
    proc<void(*)(void(*)(const NgxParameters*))>(nr,"CheekyFakeObserve")(&observe_nr);
    proc<void(*)(void(*)(const NgxParameters*))>(nr,"CheekyFakeObserveCreated")(&observe_nr_created);
    command("1\n91\nset\nHeightOffset=1");
    evaluate(7);
    require(sr_inputs.size() == 1 && sr_inputs[0].color == f.textures12[0].Get(),
        "After NR replaced original SR color");
    require(sr_inputs[0].reset == 7, "After NR region jump changed the host SR reset");
    require(evaluation_order == "SN" && nr_reset == 1, "After NR lost post-SR placement or history reset");
    require(std::abs(nr_motion_x - static_cast<float>(nr_motion_width)) < 0.00001F,
        "After foveated NR used incorrect guide-pixel motion units");
    for (unsigned i = 0; i < 3; ++i) {
        evaluate(0);
        require(sr_inputs.size() == 1 && sr_inputs[0].reset == 0, "Repeated After frame reset SR");
    }
    command("1\n92\nset\nEnabled=true\nPeripheralDlaa=true\nHeightOffset=-1");
    evaluate(0); evaluate(0); // Warm ordinary center/peripheral SR histories.
    require(sr_inputs.size() == 2 && sr_inputs[0].reset == 0 && sr_inputs[1].reset == 0,
        "Stable SR histories did not settle");
    command("1\n93\nset\nHeightOffset=1");
    evaluate(0);
    require(sr_inputs.size() == 2, "Expected peripheral then center SR evaluation");
    require(sr_inputs[0].reset == 0, "After input scope added a peripheral SR reset");
    require(sr_inputs[1].reset == 1, "After input scope consumed the center SR gaze reset");
    require(evaluation_order == "SSN" && nr_reset == 1, "After NR did not follow peripheral and center SR");
    command("1\n94\nset\nEnabled=false\nNrEnabled=false\nNrProcessingOrder=1\nHeightOffset=-1");
    for (unsigned i = 0; i < 3; ++i) {
        evaluate(0);
        require(sr_inputs.size() == 1 && sr_inputs[0].color == f.textures12[0].Get() && sr_inputs[0].reset == 0,
            "Disabled Before NR changed raw SR input/reset");
    }
    command("1\n95\nset\nNrEnabled=true\nNrFoveated=false");
    const auto fail_nr = proc<void(*)(bool)>(nr,"CheekyFakeFailEvaluations");
    fail_nr(true);
    evaluate(0);
    require(sr_inputs.size() == 1 && sr_inputs[0].color == f.textures12[0].Get() && sr_inputs[0].reset == 0,
        "Failed Before preparation changed original SR input/reset");
    command("1\n96\nset\nNrProcessingOrder=0");
    evaluate(0);
    require(sr_inputs.size() == 1 && sr_inputs[0].reset == 0, "Raw fallback to After unnecessarily reset SR");
    fail_nr(false);
    command("1\n96\nset\nNrProcessingOrder=1");
    const auto expect_input = [&](bool processed, unsigned reset, const char* why) {
        require(sr_inputs.size() == 1 &&
            (sr_inputs[0].color != f.textures12[0].Get()) == processed && sr_inputs[0].reset == reset, why);
    };
    evaluate(0);
    expect_input(true, 1, "First successful Before substitution did not reset SR");
    require(evaluation_order == "NS", "Before NR did not run before SR exactly once");
    require(std::abs(nr_motion_x / nr_motion_width - 1.F) < 0.000001F,
        "NR hook did not convert original motion units to the runtime convention");
    evaluate(0);
    expect_input(true, 0, "Stable successful Before substitution reset SR");
    if (!f.use_sl) {
        command("1\n96\nset\nEnabled=true\nPeripheralDlaa=false");
        proc<void(*)(unsigned)>(f.ngx,"CheekyFakeFailNextEvaluations")(1);
        evaluate(0);
        require(evaluation_order == "NSS" && sr_inputs.size() == 2 &&
            sr_inputs[0].color != f.textures12[0].Get() && sr_inputs[1].color == sr_inputs[0].color,
            "Private SR failure did not preserve prepared Before input through native fallback");
        command("1\n96\nset\nEnabled=false");
    }
    // Keep another eye's processed history while this eye returns to raw input.
    auto* first_handle = f.handle;
    const auto first_viewport = f.viewport.value;
    if (f.use_sl) {
        ++f.viewport.value;
        require(f.sl_options(&f.viewport,&f.options) == 0, "Second viewport options");
    } else {
        require(ngx_succeeded(f.create12(f.list.Get(),1,&f.params,&f.handle)), "Second native view");
    }
    evaluate(0);
    expect_input(true, 1, "Second eye inherited first eye's processed history");
    command("1\n96\nset\nNrProcessingOrder=0");
    evaluate(0);
    expect_input(false, 1, "Processed Before to After did not reset this eye");
    evaluate(0);
    expect_input(false, 0, "Processed Before to After reset this eye twice");
    if (!f.use_sl) require(ngx_succeeded(f.release(f.handle)), "Release second native view");
    f.handle = first_handle; f.viewport.value = first_viewport;
    if (f.use_sl) {
        require(f.sl_options(&f.viewport,&f.options) == 0, "Restore first viewport options");
        f.ambiguous_sl_inputs = true;
        evaluate(0);
        expect_input(false, 0, "Ambiguous viewport consumed another view's transition");
        f.ambiguous_sl_inputs = false;
        f.cache_other_view = true;
    }
    evaluate(0);
    expect_input(false, 1, "First eye lost its pending processed-to-After transition");
    evaluate(0);
    expect_input(false, 0, "First eye repeated its transition reset");
    f.cache_other_view = false;
    command("1\n96\nset\nNrProcessingOrder=1");
    evaluate(0);
    expect_input(true, 1, "Returning to successful Before did not reset");
    fail_nr(true);
    evaluate(0);
    expect_input(false, 1, "Failed preparation did not reset processed history");
    evaluate(0);
    expect_input(false, 0, "Repeated failed preparation reset raw history");
    fail_nr(false);
    evaluate(0);
    expect_input(true, 1, "Recovered preparation did not reset raw history");
    if (f.use_sl) {
        f.missing_sl_constants = true;
        evaluate(0);
        expect_input(false, 1, "Missing current metadata discarded a processed transition");
        evaluate(0);
        expect_input(false, 0, "Repeated missing metadata reset original input");
        f.missing_sl_constants = false;
        evaluate(0);
        expect_input(true, 1, "Metadata recovery did not restore Before substitution");
        const auto fail_tag = proc<void(*)(bool)>(f.sl,"CheekyFakeFailColorTag");
        fail_tag(true);
        evaluate(0);
        expect_input(false, 1, "Rejected tag substitution was recorded as processed");
        evaluate(0);
        expect_input(false, 0, "Repeated rejected tag reset raw history");
        fail_tag(false);
        evaluate(0);
        expect_input(true, 1, "Successful tag substitution did not reset raw history");
    }
    // Observe actual GPU-written jitter compensation through both host adapters.
    const auto make_buffer = [&](UINT64 bytes, D3D12_HEAP_TYPE type) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=type;
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; d.Width=bytes;
        d.Height=d.DepthOrArraySize=d.MipLevels=1; d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        check(f.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,
            type==D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(&r)), "NR jitter buffer"); return r;
    };
    const auto transition_resource = [&](ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b}; f.list->ResourceBarrier(1,&v);
    };
    const auto motion_desc=f.textures12[2]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};
    f.device->GetCopyableFootprints(&motion_desc,0,1,0,&fp,nullptr,nullptr,&bytes);
    auto upload=make_buffer(bytes,D3D12_HEAP_TYPE_UPLOAD);
    void* mapped{};check(upload->Map(0,nullptr,&mapped),"Map zero motion");std::memset(mapped,0,static_cast<size_t>(bytes));upload->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION source{},destination{};
    source.pResource=upload.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=fp;
    destination.pResource=f.textures12[2].Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    transition_resource(destination.pResource,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    f.list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
    transition_resource(destination.pResource,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    f.finish_gpu();
    const auto read_motion = [&]() {
        auto readback=make_buffer(256,D3D12_HEAP_TYPE_READBACK);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};
        src.pResource=nr_motion_resource.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32_FLOAT,1,1,1,256};
        const D3D12_BOX box{0,0,0,1,1,1};
        transition_resource(src.pResource,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        f.list->CopyTextureRegion(&dst,0,0,0,&src,&box);
        transition_resource(src.pResource,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        f.finish_gpu();float* data{};check(readback->Map(0,nullptr,reinterpret_cast<void**>(&data)),"NR jitter readback");
        const std::array<float,2> value{data[0],data[1]};readback->Unmap(0,nullptr);return value;
    };
    command("1\n96\nset\nEnabled=false\nNrEnabled=true\nNrFoveated=false\nNrWorkingScale=1\nNrProcessingOrder=1\nNrMotionScaleXMultiplier=-2");
    f.params.Set("Jitter.Offset.X",0.F);f.params.Set("Jitter.Offset.Y",0.F);evaluate(1);
    f.params.Set("Jitter.Offset.X",0.25F);f.params.Set("Jitter.Offset.Y",-0.375F);evaluate(0);
    auto jitter_motion=read_motion();
    require(std::abs(jitter_motion[0]+0.25F/128.F)<1e-7F && std::abs(jitter_motion[1]-0.375F/128.F)<1e-7F,
        "Before NR lost per-view jitter or applied the user multiplier to jitter");
    evaluate(1);jitter_motion=read_motion();
    require(jitter_motion[0]==0.F && jitter_motion[1]==0.F,"Reset retained previous jitter");
    command("1\n96\nset\nNrProcessingOrder=0");evaluate(0);
    f.params.Set("Jitter.Offset.X",-0.25F);evaluate(0);jitter_motion=read_motion();
    require(jitter_motion[0]==0.F && jitter_motion[1]==0.F,"After NR added render jitter to stabilized color");
    f.params.Set("Jitter.Offset.X",0.F);f.params.Set("Jitter.Offset.Y",0.F);
    command("1\n96\nset\nNrProcessingOrder=1\nNrMotionScaleXMultiplier=1");evaluate(1);
    // Jittered source vectors contain both scene motion and raster jitter.
    // Scale only scene motion, then restore the selected color domain's jitter.
    for (bool before : {true, false}) for (float multiplier : {-2.F, 0.F, 0.5F}) {
        command((std::string("1\n96\nset\nNrProcessingOrder=") + (before ? "1" : "0") +
            "\nNrMotionScaleXMultiplier=" + std::to_string(multiplier)).c_str());
        f.params.Set("DLSS.Feature.Create.Flags", 6U);
        f.params.Set("Jitter.Offset.X",0.F); f.params.Set("Jitter.Offset.Y",0.F); evaluate(1);
        check(upload->Map(0,nullptr,&mapped), "Map jittered motion");
        for (unsigned y = 0; y < motion_desc.Height; ++y) {
            auto* row = reinterpret_cast<DirectX::PackedVector::HALF*>(static_cast<std::byte*>(mapped) + fp.Offset + y * fp.Footprint.RowPitch);
            for (unsigned x = 0; x < motion_desc.Width; ++x) {
                row[2*x] = DirectX::PackedVector::XMConvertFloatToHalf(1.75F / (f.use_sl ? 128.F : 1.F));
                row[2*x+1] = DirectX::PackedVector::XMConvertFloatToHalf(0.375F / (f.use_sl ? 128.F : 1.F));
            }
        }
        upload->Unmap(0,nullptr);
        transition_resource(destination.pResource,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        f.list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
        transition_resource(destination.pResource,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        f.params.Set("Jitter.Offset.X",0.25F); f.params.Set("Jitter.Offset.Y",-0.375F); evaluate(0);
        const auto actual = read_motion();
        require(nr_reset == 0 && std::abs(actual[0] - (2.F*multiplier - (before ? 0.25F : 0.F))/128.F) < 1e-7F &&
            std::abs(actual[1] - (before ? 0.375F/128.F : 0.F)) < 1e-7F,
            "NR multiplier scaled embedded jitter instead of only scene motion");
    }
    f.params.Set("DLSS.Feature.Create.Flags", 2U);
    f.params.Set("Jitter.Offset.X",0.F); f.params.Set("Jitter.Offset.Y",0.F);
    command("1\n96\nset\nNrProcessingOrder=1\nNrMotionScaleXMultiplier=1"); evaluate(1);

    // Identity-model round trip: SDR is already encoded. Paper white must not
    // dim it, and alpha belongs to the original color, not the model.
    const auto copy_nr_color = proc<void(*)(bool)>(nr, "CheekyFakeCopyNrColor");
    copy_nr_color(true);
    const auto color_desc = f.textures12[0]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT color_fp{}; UINT64 color_bytes{};
    f.device->GetCopyableFootprints(&color_desc,0,1,0,&color_fp,nullptr,nullptr,&color_bytes);
    auto color_upload = make_buffer(color_bytes,D3D12_HEAP_TYPE_UPLOAD);
    check(color_upload->Map(0,nullptr,&mapped), "Map NR color");
    for (unsigned y = 0; y < color_desc.Height; ++y) {
        auto* row = reinterpret_cast<DirectX::PackedVector::HALF*>(static_cast<std::byte*>(mapped) + color_fp.Offset + y*color_fp.Footprint.RowPitch);
        for (unsigned x = 0; x < color_desc.Width; ++x) {
            row[4*x] = DirectX::PackedVector::XMConvertFloatToHalf(static_cast<float>(x)/128.F);
            row[4*x+1] = DirectX::PackedVector::XMConvertFloatToHalf(static_cast<float>(y)/128.F);
            row[4*x+2] = DirectX::PackedVector::XMConvertFloatToHalf(0.75F);
            row[4*x+3] = DirectX::PackedVector::XMConvertFloatToHalf(0.25F);
        }
    }
    color_upload->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION color_src{}, color_dst{};
    color_src.pResource=color_upload.Get(); color_src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; color_src.PlacedFootprint=color_fp;
    color_dst.pResource=f.textures12[0].Get(); color_dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    transition_resource(color_dst.pResource,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    f.list->CopyTextureRegion(&color_dst,0,0,0,&color_src,nullptr);
    transition_resource(color_dst.pResource,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (float white : {0.25F, 1.F, 4.F}) for (float scale : {1.F, 0.5F})
    for (float transfer : {0.F, 0.5F, 1.F, 2.F}) {
        command((std::string("1\n96\nset\nNrPaperWhiteScale=") + std::to_string(white) +
            "\nNrWorkingScale=" + std::to_string(scale) +
            "\nNrHdrTransferStrength=" + std::to_string(transfer)).c_str()); evaluate(0);
        require(sr_inputs.size() == 1 && sr_inputs[0].color != f.textures12[0].Get(), "Color round trip did not execute NR");
        auto readback=make_buffer(256,D3D12_HEAP_TYPE_READBACK);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource=sr_inputs[0].color; from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=readback.Get(); to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,1,1,1,256};
        // The first reconstructed pixel clamps to the first proxy pixel. Clamp
        // every bilinear tap independently, not relative to an already-clamped tap.
        const D3D12_BOX pixel{0,0,0,1,1,1};
        transition_resource(from.pResource,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        f.list->CopyTextureRegion(&to,0,0,0,&from,&pixel);
        transition_resource(from.pResource,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        f.finish_gpu(); check(readback->Map(0,nullptr,&mapped), "Map NR color result");
        const auto* result = static_cast<const DirectX::PackedVector::HALF*>(mapped);
        const float expected_edge = scale == 1.F ? 0.F : transfer*0.5F/128.F;
        const bool unchanged = DirectX::PackedVector::XMConvertHalfToFloat(result[0]) == expected_edge &&
            DirectX::PackedVector::XMConvertHalfToFloat(result[1]) == expected_edge &&
            DirectX::PackedVector::XMConvertHalfToFloat(result[2]) == 0.75F &&
            DirectX::PackedVector::XMConvertHalfToFloat(result[3]) == 0.25F;
        readback->Unmap(0,nullptr);
        require(unchanged, "SDR NR round trip changed paper white, alpha, or working-scale edge sampling");
    }
    command("1\n96\nset\nNrPaperWhiteScale=1\nNrWorkingScale=1\nNrHdrTransferStrength=1"); evaluate(0);
    copy_nr_color(false);
    // Exercise the production feature cache through the fake NVIDIA runtime:
    // warm two toggle states, then churn dimensions without growing live features.
    const auto nr_creates = proc<Counter>(nr, "CheekyFakeCreates");
    const auto nr_releases = proc<Counter>(nr, "CheekyFakeReleases");
    // The fixture observes the immutable creation snapshot of the actual handle
    // being evaluated, including cached handles. Evaluate-only writes cannot pass.
    const auto check_model = [&]() {
        unsigned preset{}, mask{}, ui{};
        void* control_mask = reinterpret_cast<void*>(123);
        require(ngx_succeeded(nr_created.Get("DLSSNR.Hint.Render.Preset", &preset)) && preset == 0,
            "Default NR preset must be passed as zero");
        require(ngx_succeeded(nr_created.Get("DLSSNR.ControlMask", &control_mask)) && control_mask == nullptr,
            "Explicit control mask overrides automatic NR mask");
        require(ngx_succeeded(nr_created.Get("DLSSNR.UseAutoMask", &mask)) && mask == 0 &&
            ngx_succeeded(nr_created.Get("DLSSNR.UICorrection", &ui)) && ui == 0,
            "Default NR mask/UI tuning missing at creation");
    };
    evaluate(0); check_model();
    const auto model_live_limit = nr_creates() - nr_releases() + 1;
    struct ModelSetting { const char* setting; const char* parameter; float original; float changed; };
    for (const auto& item : {
        ModelSetting{"NrIntensity", "DLSSNR.Intensity", 1.F, 0.25F},
        ModelSetting{"NrLocalToneStrength", "DLSSNR.LocalToneStrength", 1.F, 0.5F},
        ModelSetting{"NrLocalStructureStrength", "DLSSNR.LocalStructureStrength", 1.F, 1.5F},
        ModelSetting{"NrSkinStructureStrength", "DLSSNR.SkinStructureStrength", 1.F, 0.75F},
        ModelSetting{"NrAutomaticMask", "DLSSNR.UseAutoMask", 0.F, 1.F},
        ModelSetting{"NrUiCorrection", "DLSSNR.UICorrection", 0.F, 1.F},
        ModelSetting{"NrPreset", "DLSSNR.Hint.Render.Preset", 0.F, 3.F},
        ModelSetting{"NrStyle", "DLSSNR.Style", 0.F, 2.F}}) {
        const auto set_value = [&](float value) {
            const bool boolean = std::string(item.setting) == "NrAutomaticMask" || std::string(item.setting) == "NrUiCorrection";
            const auto text = std::string("1\n96\nset\n") + item.setting + "=" +
                (boolean ? (value != 0 ? "true" : "false") :
                    (std::string(item.setting) == "NrPreset" || std::string(item.setting) == "NrStyle") ? std::to_string(static_cast<unsigned>(value)) : std::to_string(value));
            command(text.c_str()); evaluate(0);
            float actual = -100.F;
            require(evaluation_order == "NS" && ngx_succeeded(nr_created.Get(item.parameter, &actual)) && actual == value,
                "NR control did not reach the feature's immutable creation parameters");
        };
        const auto previous_creates = nr_creates();
        set_value(item.changed);
        require(nr_creates() == previous_creates + 1 && nr_reset == 1,
            "NR model tuning did not recreate and reset the feature");
        evaluate(0);
        require(nr_creates() == previous_creates + 1 && nr_reset == 0,
            "Unchanged NR tuning recreated the feature or reset history");
        set_value(item.original);
        require(nr_creates() == previous_creates + 1 && nr_reset == 1,
            "Returning to cached NR tuning used the wrong feature or history");
        require(nr_creates() - nr_releases() <= model_live_limit, "NR tuning grew the feature cache");
    }
    check_model();
    const auto set_foveated = [&](bool enabled) {
        command(enabled
            ? "1\n96\nset\nNrFoveated=true\nNrWidth=0.5\nNrHeight=0.5\nNrWorkingScale=1"
            : "1\n96\nset\nNrFoveated=false\nNrWorkingScale=1");
        evaluate(0);
        require(evaluation_order == "NS", "Cache toggle failed to run Before NR");
    };
    set_foveated(false); set_foveated(true);
    const auto warm_creates = nr_creates();
    const auto warm_live = nr_creates() - nr_releases();
    for (unsigned i = 0; i < 12; ++i) set_foveated(i % 2 != 0);
    require(nr_creates() == warm_creates, "Two-state NR toggle recreated warmed features");
    for (unsigned i = 0; i < 24; ++i) {
        const auto setting = std::string("1\n96\nset\nNrFoveated=false\nNrWorkingScale=") +
            std::to_string(0.5 + 0.01 * i);
        command(setting.c_str());
        evaluate(0);
        require(evaluation_order == "NS", "Settings churn failed to run Before NR");
        require(std::abs(nr_motion_x - static_cast<float>(nr_motion_width)) < 0.00001F,
            "Changing NR working resolution changed motion displacement");
        require(nr_creates() - nr_releases() <= warm_live,
            "Settings churn accumulated retired NVIDIA features");
    }
    command("1\n96\nset\nNrEnabled=false");
    evaluate(0);
    expect_input(false, 1, "Disabling Before NR did not reset processed history");
    evaluate(0);
    expect_input(false, 0, "Repeated disabled frame reset raw history");
    require(evaluation_order == "S", "Disabled NR still evaluated");
    proc<void(*)(void(*)(const NgxParameters*))>(f.ngx,"CheekyFakeObserve")(nullptr);
    proc<void(*)(void(*)(const NgxParameters*))>(nr,"CheekyFakeObserve")(nullptr);
    proc<void(*)(void(*)(const NgxParameters*))>(nr,"CheekyFakeObserveCreated")(nullptr);
    command("1\n97\nset\nEnabled=true\nNrEnabled=false\nPeripheralDlaa=false\nAutoStereoAlignment=false");
    nr_motion_resource.Reset();
    puts("NR controls, cached creation tuning, jittered vectors, SDR codec/edge sampling and reset isolation passed");
}
}
void prepare_late_attach_test(const std::filesystem::path& bin,ID3D11Device* dx11,ID3D12Device* dx12,ID3D12CommandQueue* queue,bool use_c,bool use_sl,const std::filesystem::path& ngx_path) {
    auto& f=fixture(); f.use_c=use_c; f.use_sl=use_sl; f.device=dx12; f.queue=queue;
    f.ngx=LoadLibraryW((ngx_path.empty() ? bin/L"test-fixtures"/L"nvngx_dlss.dll" : ngx_path).c_str()); require(f.ngx!=nullptr,"Load fake NGX before plugin");
    f.creates=proc<Counter>(f.ngx,"CheekyFakeCreates"); f.evaluates=proc<Counter>(f.ngx,"CheekyFakeEvaluates"); f.releases=proc<Counter>(f.ngx,"CheekyFakeReleases");
    f.params.Set("Width",128U); f.params.Set("Height",128U); f.params.Set("OutWidth",256U); f.params.Set("OutHeight",256U);
    f.params.Set("DLSS.Feature.Create.Flags",2U); f.params.Set("PerfQualityValue",2U);
    f.params.Set("MV.Scale.X",1.0f); f.params.Set("MV.Scale.Y",1.0f);
    const char* names[]{"Color","Depth","MotionVectors","Output"};
    if(dx11) {
        dx11->GetImmediateContext(&f.context);
        for(unsigned i=0;i<4;++i) {
            D3D11_TEXTURE2D_DESC d{}; d.Width=d.Height=i==3?256:128; d.MipLevels=d.ArraySize=1;
            d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; d.SampleDesc.Count=1; d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
            ComPtr<ID3D11Texture2D> texture; check(dx11->CreateTexture2D(&d,nullptr,&texture),"Late DX11 texture");
            f.params.Set(names[i],static_cast<ID3D11Resource*>(texture.Get())); f.textures11.push_back(texture);
        }
        auto init=proc<NgxResult(*)(unsigned long long,const wchar_t*,ID3D11Device*,const void*,unsigned)>(f.ngx,"NVSDK_NGX_D3D11_Init");
        require(ngx_succeeded(init(42,L".",dx11,nullptr,1)),"Fake DX11 init");
        f.create11=proc<Create11>(f.ngx,"NVSDK_NGX_D3D11_CreateFeature"); f.evaluate11=proc<Evaluate11>(f.ngx,"NVSDK_NGX_D3D11_EvaluateFeature");
        f.evaluate11c=proc<Evaluate11C>(f.ngx,"NVSDK_NGX_D3D11_EvaluateFeature_C"); f.release=proc<Release>(f.ngx,"NVSDK_NGX_D3D11_ReleaseFeature");
        require(ngx_succeeded(f.create11(f.context.Get(),1,&f.params,&f.handle)),"Create game feature before injection");
    } else {
        check(dx12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f.allocator)),"Late allocator");
        check(dx12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,f.allocator.Get(),nullptr,IID_PPV_ARGS(&f.list)),"Late list");
        for(unsigned i=0;i<4;++i) {
            D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=d.Height=i==3?256:128;
            d.DepthOrArraySize=d.MipLevels=1;
            d.Format = i == 1 ? DXGI_FORMAT_R32_FLOAT : i == 2 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.SampleDesc.Count=1; d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ComPtr<ID3D12Resource> texture; check(dx12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,i==3?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&texture)),"Late DX12 texture");
            f.params.Set(names[i],texture.Get()); f.textures12.push_back(texture);
        }
        auto init=proc<NgxResult(*)(unsigned long long,const wchar_t*,ID3D12Device*,const void*,unsigned)>(f.ngx,"NVSDK_NGX_D3D12_Init");
        require(ngx_succeeded(init(42,L".",dx12,nullptr,1)),"Fake DX12 init");
        f.create12=proc<Create12>(f.ngx,"NVSDK_NGX_D3D12_CreateFeature"); f.evaluate12=proc<Evaluate12>(f.ngx,"NVSDK_NGX_D3D12_EvaluateFeature");
        f.evaluate12c=proc<Evaluate12C>(f.ngx,"NVSDK_NGX_D3D12_EvaluateFeature_C"); f.release=proc<Release>(f.ngx,"NVSDK_NGX_D3D12_ReleaseFeature");
        require(ngx_succeeded(f.create12(f.list.Get(),1,&f.params,&f.handle)),"Create DX12 game feature before injection");
    }
    if(use_sl) {
        f.sl=LoadLibraryW((bin/L"test-fixtures"/L"sl.interposer.dll").c_str()); require(f.sl!=nullptr,"Load fake Streamline");
        if(dx11) proc<void(*)(Evaluate11,const NgxHandle*,NgxParameters*)>(f.sl,"CheekyFakeConfigure11")(f.evaluate11,f.handle,&f.params);
        else proc<void(*)(Evaluate12,const NgxHandle*,NgxParameters*)>(f.sl,"CheekyFakeConfigure")(f.evaluate12,f.handle,&f.params);
        f.sl_evaluate=proc<SlEvaluate>(f.sl,"slEvaluateFeature");
        void* setter{}; require(proc<unsigned(*)(unsigned,const char*,void**)>(f.sl,"slGetFeatureFunction")(0,"slDLSSSetOptions",&setter)==0,"Cache SL setter before injection");
        f.sl_options=reinterpret_cast<SlOptions>(setter);
        f.viewport.struct_version=1; f.viewport.struct_type.data1=0x12345678; f.viewport.value=7;
        f.options.struct_version=3; f.options.mode=3; f.options.output_width=f.options.output_height=256;
        f.options.pre_exposure=f.options.exposure_scale=1.0f;
        require(f.sl_options(&f.viewport,&f.options)==0,"Set SL options before injection");
    }
    require(ngx_succeeded(f.evaluate()),"Evaluate before injection");
    require(f.creates()==1 && f.evaluates()==1,"Fixture initialized before hook installation");
}
void prepare_afw_test(const std::filesystem::path& bin, const std::filesystem::path& root, ID3D12Device* device,
    ID3D12CommandQueue* queue, std::string_view mode) {
    const bool use_c = mode.ends_with("-c"), use_sl = mode.find("streamline") != mode.npos;
    const bool missing_lower = mode == "--afw-missing-lower", public_first = mode.starts_with("--afw-public-first");
    afw_ota = mode.starts_with("--afw-ota"); afw_ambiguous = mode == "--afw-ota-ambiguous";
    std::filesystem::path ngx_path;
    if (afw_ota) {
        ngx_path = root / "NVIDIA/NGX/models/dlss/versions/20318464/files/160_E658700.bin";
        std::filesystem::create_directories(ngx_path.parent_path());
        std::filesystem::copy_file(bin / "test-fixtures/nvngx_dlss.dll", ngx_path);
        // Identical export names are insufficient: an RR or NR snippet must
        // never be selected as SR, even with the same generated basename.
        for (const auto* model : {"dlssd", "dlssnr", "sl_dlss_0"}) {
            const auto decoy = root / "NVIDIA/NGX/models" / model / "versions/20318464/files/160_E658700.bin";
            std::filesystem::create_directories(decoy.parent_path());
            std::filesystem::copy_file(bin / "test-fixtures/nvngx_dlss.dll", decoy);
            require(LoadLibraryW(decoy.c_str()) != nullptr, "Load non-SR OTA decoy");
        }
        if (afw_ambiguous) require(LoadLibraryW((bin / "test-fixtures/nvngx_dlss.dll").c_str()) != nullptr, "Load a second SR runtime");
    }
    prepare_late_attach_test(bin, nullptr, device, queue, false, use_sl, ngx_path);
    auto& f = fixture();
    require(ngx_succeeded(f.release(f.handle)), "Release initial public fixture handle");
    const auto dir = root / "afw-fixtures";
    std::filesystem::create_directories(dir);
    for (const auto* name : {L"_nvngx.dll", L"PDAFWPlugin.dll"})
        std::filesystem::copy_file(bin / "test-fixtures" / "nvngx_dlss.dll", dir / name);
    afw_core = LoadLibraryW((dir / "_nvngx.dll").c_str());
    afw_warp_module = LoadLibraryW((dir / "PDAFWPlugin.dll").c_str());
    require(afw_core && afw_warp_module, "Load simulated AFW core before Cheeky");
    afw_cached_warp = proc<void(__stdcall*)(void*)>(afw_warp_module, "EvaluateFrameWarp");
    afw_missing_lower = missing_lower || public_first || afw_ambiguous;
    afw_public_first = public_first;
    if (public_first) {
        proc<void(*)(HMODULE, bool)>(f.ngx, "CheekyFakeForwardTo")(afw_core, false);
        f.use_c = use_c;
    } else {
        if (!missing_lower) proc<void(*)(HMODULE, bool)>(afw_core, "CheekyFakeForwardTo")(f.ngx, use_c);
        f.create12 = proc<Create12>(afw_core, "NVSDK_NGX_D3D12_CreateFeature");
        f.evaluate12 = proc<Evaluate12>(afw_core, "NVSDK_NGX_D3D12_EvaluateFeature");
        f.release = proc<Release>(afw_core, "NVSDK_NGX_D3D12_ReleaseFeature");
    }
    f.params.Set("Reset", 0U);
    require(ngx_succeeded(f.create12(f.list.Get(), 1U, &f.params, &f.handle)), "Create wrapped game feature before injection");
    if (use_sl) proc<void(*)(Evaluate12,const NgxHandle*,NgxParameters*)>(f.sl, "CheekyFakeConfigure")(f.evaluate12, f.handle, &f.params);
    proc<void(*)(void(*)(const NgxParameters*))>(afw_core, "CheekyFakeObserve")(&observe_afw_core);
    if (!public_first) proc<void(*)(void(*)(const NgxParameters*))>(f.ngx, "CheekyFakeObserve")(&observe_afw_lower);
}

void verify_afw_gaze_history(CheekyUEVRSnapshotFn get, void (*command)(const char*)) {
    auto& f = fixture();
    wchar_t filename[32768]{};
    require(GetModuleFileNameW(afw_warp_module, filename, 32768) != 0, "Find fake gaze runtime source");
    const auto gaze_path = std::filesystem::path(filename).parent_path() / L"CheekyOpenXRLayer.dll";
    std::filesystem::copy_file(filename, gaze_path, std::filesystem::copy_options::overwrite_existing);
    const auto layer = LoadLibraryW(gaze_path.c_str()); require(layer != nullptr, "Load fake gaze publication runtime");
    const auto publish = proc<void(*)(const CheekyGazeSnapshotV1*)>(layer, "CheekyFakeGazeSnapshot");
    proc<void(*)(void(*)(const NgxHandle*, const NgxParameters*))>(f.ngx, "CheekyFakeObserveHandle")(observe_afw_history);
    CheekyGazeSnapshotV1 gaze{};
    gaze.structure_size = sizeof(gaze); gaze.abi_version = CHEEKY_GAZE_ABI_VERSION;
    gaze.view_count = 2; gaze.session_generation = 1; gaze.swapchain_generation = 1;
    gaze.status_flags = CHEEKY_GAZE_STATUS_LAYER_ACTIVE | CHEEKY_GAZE_STATUS_SESSION_FOCUSED | CHEEKY_GAZE_STATUS_GAZE_VALID;
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto& v = gaze.views[eye]; v.structure_size = sizeof(v); v.view_index = eye;
        v.flags = CHEEKY_GAZE_VIEW_FOV_VALID | CHEEKY_GAZE_VIEW_ORIENTATION_VALID;
        v.fov_left = v.fov_down = -std::atan(1.F); v.fov_right = v.fov_up = std::atan(1.F);
        v.center_u = v.center_v = .5F;
    }
    const auto evaluate = [&] {
        command("1\n230\nget"); // Publish the current UEVR projection pair.
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now); gaze.publication_qpc = now.QuadPart;
        ++gaze.predicted_display_time; publish(&gaze); afw_history.clear();
        require(ngx_succeeded(f.evaluate()), "AFW gaze evaluation succeeds"); f.finish_gpu();
        require(afw_contract_ok && afw_order_ok, "Gaze preserves full AFW inputs and corrected motion order");
    };
    command("1\n231\nset\nEnabled=true\nCenterMode=1\nAfwAutomaticCoverage=true\nWidth=0.25\nHeight=0.3\nAfwWarpMargin=0.03\nGazeSmoothingMs=0\nCenterSupersampling=1\nPeripheralDlaa=true");
    evaluate(); // Changing source rejects the publication preceding that change.
    evaluate();
    if (snapshot(get).find("\"afw_fresh_sample\":true") == std::string::npos || snapshot(get).find("\"coverage_mode\":3") == std::string::npos)
        puts(snapshot(get).c_str());
    require(snapshot(get).find("\"afw_fresh_sample\":true") != std::string::npos &&
        snapshot(get).find("\"coverage_mode\":3") != std::string::npos, "Real hook consumes bilateral gaze without eye mapping");
    require(afw_history.size() == 2 && afw_history.back().reset, "Gaze acquisition resets center history");
    const auto first = afw_history;
    const auto creates = f.creates();
    evaluate();
    require(afw_history.size() == 2 && !afw_history[0].reset && !afw_history[1].reset && f.creates() == creates,
        "Stable shared game handle preserves both private histories");
    afw_expected_reset = 1; f.params.Set("Reset", 1U); evaluate();
    require(afw_history.size() == 2 && afw_history[0].reset && afw_history[1].reset && get_ui(&f.params, "Reset") == 1,
        "Game reset reaches both private histories and original Reset is restored");
    afw_expected_reset = 0; f.params.Set("Reset", 0U); evaluate();
    require(!afw_history[0].reset && !afw_history[1].reset, "Game reset is not repeated");
    command("1\n232\nset\nPeripheralDlaa=false"); evaluate();
    require(afw_history.size() == 1 && afw_history[0].handle == first[1].handle, "Center keeps its history while periphery is off");
    command("1\n233\nset\nPeripheralDlaa=true"); evaluate();
    require(afw_history.size() == 2 && afw_history[0].reset && !afw_history[1].reset,
        "Resuming starved periphery resets only its history");
    evaluate(); require(!afw_history[0].reset && !afw_history[1].reset, "Resumed private histories stabilize");
    const auto first_game = f.handle;
    NgxHandle* second_game{};
    require(ngx_succeeded(f.create12(f.list.Get(), 1, &f.params, &second_game)), "Create second game history");
    const auto select = [&](NgxHandle* handle) {
        f.handle = handle;
        if (f.use_sl) proc<void(*)(Evaluate12,const NgxHandle*,NgxParameters*)>(f.sl, "CheekyFakeConfigure")(f.evaluate12, handle, &f.params);
    };
    select(second_game); evaluate();
    require(afw_history.size() == 2 && afw_history[0].handle != first[0].handle && afw_history[1].handle != first[1].handle &&
        afw_history[0].reset && afw_history[1].reset, "Distinct game handles have distinct center and peripheral histories");
    select(first_game); evaluate();
    require(afw_history.size() == 2 && afw_history[0].handle == first[0].handle && afw_history[1].handle == first[1].handle &&
        !afw_history[0].reset && !afw_history[1].reset, "Returning to first game handle preserves its own histories");
    require(ngx_succeeded(f.release(second_game)), "Release second history independently");
    command("1\n234\nset\nEnabled=false"); evaluate();
    command("1\n235\nset\nEnabled=true"); evaluate();
    require(afw_history.size() == 2 && afw_history[0].reset && afw_history[1].reset, "Native fallback invalidates both skipped private histories");
    evaluate(); require(!afw_history[0].reset && !afw_history[1].reset, "Native-to-private transition resets only once");
    command("1\n237\nset\nNrEnabled=true\nNrProcessingOrder=0\nNrFoveated=true\nNrUseSrFoveation=false\nNrWidth=0.2\nNrHeight=0.3\nNrWorkingScale=0.5");
    evaluate(); evaluate();
    require(snapshot(get).find("\"afw_fresh_sample\":true") != std::string::npos &&
        snapshot(get).find("\"nr\":\"Active\"") != std::string::npos, "Independent foveated NR consumes bilateral gaze through the real hook");
    const auto gaze_sr_creates = f.creates();
    command("1\n238\nset\nNrWidth=0.45"); evaluate();
    require(f.creates() == gaze_sr_creates, "NR gaze size changes do not recreate SR histories");
    command("1\n239\nset\nNrUseSrFoveation=true\nNrProcessingOrder=1"); evaluate(); evaluate();
    require(snapshot(get).find("\"afw_fresh_sample\":true") != std::string::npos &&
        snapshot(get).find("\"processing_width\":128") != std::string::npos, "Before NR supports gaze and linked SR coverage");
    // Exercise the production NR allocator and border path through the actual
    // nested AFW hook. NVIDIA inference alone is replaced by the fixture.
    command("1\n251\nset\nCenterMode=2\nGazeSmoothingMs=100\nNrAlignmentBorder=true\nAlignmentBorder=true");
    gaze.status_flags |= CHEEKY_GAZE_STATUS_SIMULATED;
    for (unsigned i = 0; i < 8; ++i) evaluate();
    const auto counter = [&](const char* key) {
        const auto text = snapshot(get); const auto token = std::string("\"") + key + "\":";
        const auto start = text.find(token); require(start != std::string::npos, "NR allocation diagnostic is present");
        return std::stoull(text.substr(start + token.size()));
    };
    const auto codecs = counter("codec_creations"), borders = counter("border_creations");
    const auto sr_allocations = f.creates();
    const auto nr_module = GetModuleHandleW(L"nvngx_dlssnr.dll");
    require(nr_module != nullptr, "NR fixture is resident");
    const auto nr_creates = proc<Counter>(nr_module, "CheekyFakeCreates");
    const auto nr_allocations = nr_creates();
    for (unsigned i = 0; i < 90; ++i) {
        for (unsigned eye = 0; eye < 2; ++eye) {
            gaze.views[eye].center_u = .01F + .98F * ((i * 7 + eye) % 89) / 88.F;
            gaze.views[eye].center_v = i % 2 ? .98F : .02F;
        }
        evaluate();
        require(counter("codec_creations") == codecs && counter("border_creations") == borders,
            "Moving simulated gaze with Before NR and border must not rebuild codecs or border resources");
        require(f.creates() == sr_allocations && nr_creates() == nr_allocations,
            "Moving simulated gaze must not recreate SR or NR features");
    }
    command("1\n252\nset\nNrProcessingOrder=0\nNrUseSrFoveation=false");
    for (unsigned i = 0; i < 8; ++i) evaluate();
    const auto original_textures = f.textures12;
    std::vector<ComPtr<ID3D12Resource>> alternate_textures;
    for (unsigned i = 0; i < 4; ++i) {
        auto desc = original_textures[i]->GetDesc();
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> texture;
        check(f.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            i == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            nullptr, IID_PPV_ARGS(&texture)), "Create rotating AFW frame texture");
        alternate_textures.push_back(texture);
    }
    const auto rebinds = counter("resource_rebinds");
    const auto after_codecs = counter("codec_creations");
    const auto after_sr = f.creates(), after_nr = nr_creates();
    for (unsigned i = 0; i < 30; ++i) {
        f.textures12 = i % 2 ? original_textures : alternate_textures;
        const char* names[]{"Color", "Depth", "MotionVectors", "Output"};
        for (unsigned index = 0; index < 4; ++index) f.params.Set(names[index], f.textures12[index].Get());
        for (auto& view : gaze.views) { view.center_u = i / 29.F; view.center_v = 1.F - view.center_u; }
        evaluate();
        require(counter("codec_creations") == after_codecs && f.creates() == after_sr && nr_creates() == after_nr,
            "Independent After NR gaze must retain codec and feature allocations");
    }
    f.textures12 = original_textures;
    const char* names[]{"Color", "Depth", "MotionVectors", "Output"};
    for (unsigned index = 0; index < 4; ++index) f.params.Set(names[index], f.textures12[index].Get());
    require(counter("resource_rebinds") > rebinds, "Rotating frame textures reuse completed NR descriptor allocations");
    puts("PASS: AFW simulated gaze: zero SR/NR feature and codec/border allocations after warm-up");
    command("1\n250\nset\nNrEnabled=false"); evaluate();
    command("1\n236\nset\nCenterMode=0");
    proc<void(*)(void(*)(const NgxHandle*, const NgxParameters*))>(f.ngx, "CheekyFakeObserveHandle")(nullptr);
    FreeLibrary(layer);
    puts("PASS: AFW bilateral gaze, shared/separate game histories, reset propagation and skipped periphery");
}

void verify_afw_nr(CheekyUEVRSnapshotFn get, void (*command)(const char*)) {
    auto& f = fixture();
    wchar_t runtime_path[32768]{};
    require(GetModuleFileNameW(GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll"), runtime_path, 32768), "Locate isolated AFW NR fixture");
    const auto nr = LoadLibraryW((std::filesystem::path(runtime_path).parent_path() / "nvngx_dlssnr.dll").c_str());
    require(nr != nullptr, "Load isolated NR snippet");
    proc<void(*)(void(*)(const NgxParameters*))>(nr, "CheekyFakeObserve")(observe_nr);
    proc<void(*)(void(*)(const NgxParameters*))>(nr, "CheekyFakeObserveCreated")(observe_nr_created);
    proc<void(*)(void(*)(const NgxHandle*, const NgxParameters*))>(nr, "CheekyFakeObserveHandle")(observe_nr_handle);
    proc<void(*)(bool)>(nr, "CheekyFakeCopyNrColor")(true);
    const auto fail = proc<void(*)(bool)>(nr, "CheekyFakeFailEvaluations");
    const auto creates = proc<Counter>(nr, "CheekyFakeCreates");
    const auto evaluate = [&] {
        command("1\n240\nget");
        evaluation_order.clear(); sr_inputs.clear(); nr_reset = ~0U;
        auto original = f.params.values;
        require(ngx_succeeded(f.evaluate()), "AFW NR frame succeeds"); f.finish_gpu();
        original["CheekyFake.AfwCorrected"] = f.params.values.at("CheekyFake.AfwCorrected");
        require(f.params.values == original, "NR restores all game parameters after AFW processing");
        require(afw_contract_ok && afw_order_ok, "NR never changes the contract visible to the AFW core");
    };
    command("1\n241\nset\nEnabled=true\nPeripheralDlaa=true\nNrEnabled=true\nNrProcessingOrder=0\nNrFoveated=true\nNrUseSrFoveation=false\nNrWidth=0.3\nNrHeight=0.4\nNrWorkingScale=1\nAfwAutomaticCoverage=true\nCenterMode=0");
    evaluate();
    require(evaluation_order == "ASSN" && nr_reset == 1, "After NR follows AFW preparation, periphery and center exactly once");
    const auto first_nr_creates = creates();
    evaluate();
    require(evaluation_order == "ASSN" && nr_reset == 0 && creates() == first_nr_creates,
        "Stable AFW NR retains its private feature and temporal history");
    require(snapshot(get).find("\"processing_width\":256") != std::string::npos, "After NR processes output-space pixels");
    proc<void(*)(unsigned)>(f.ngx, "CheekyFakeFailNextEvaluations")(2); evaluate();
    require(evaluation_order == "ASSSN", "After NR runs once after private SR failure and successful native fallback");
    evaluate(); evaluate();
    const auto first_nr_handle = nr_last_handle;
    const auto first_game_handle = f.handle;
    NgxHandle* second_game_handle{};
    require(ngx_succeeded(f.create12(f.list.Get(), 1, &f.params, &second_game_handle)), "Create second AFW NR history");
    const auto select = [&](NgxHandle* handle) {
        f.handle = handle;
        if (f.use_sl) proc<void(*)(Evaluate12,const NgxHandle*,NgxParameters*)>(f.sl, "CheekyFakeConfigure")(f.evaluate12, handle, &f.params);
    };
    select(second_game_handle); evaluate();
    require(nr_last_handle != first_nr_handle && nr_reset == 1, "Separate AFW game handles own separate NR features");
    select(first_game_handle); evaluate();
    require(nr_last_handle == first_nr_handle && nr_reset == 0, "Returning to a game handle preserves its NR history");
    require(ngx_succeeded(f.release(second_game_handle)), "Release second AFW NR history");
    const auto sr_creates = f.creates();
    command("1\n242\nset\nNrWidth=0.6\nNrHeight=0.5"); evaluate();
    require(nr_reset == 1 && f.creates() == sr_creates, "Independent NR coverage change leaves SR feature allocations intact");
    command("1\n243\nset\nNrProcessingOrder=1\nNrAlignmentBorder=true"); evaluate();
    require(evaluation_order == "ANSS" && nr_reset == 1 && sr_inputs.size() == 2 &&
        sr_inputs.back().color != f.textures12[0].Get(), "Before NR substitutes a private full-size color only below AFW");
    require(snapshot(get).find("\"processing_width\":128") != std::string::npos, "Before NR processes input-space pixels");
    evaluate(); require(nr_reset == 0 && !sr_inputs[0].reset && !sr_inputs[1].reset, "Stable Before NR preserves all three histories");
    proc<void(*)(unsigned)>(f.ngx, "CheekyFakeFailNextEvaluations")(2); evaluate();
    require(evaluation_order == "ANSSS" && sr_inputs.back().color != f.textures12[0].Get(),
        "Before NR is evaluated once and its processed input survives native SR fallback");
    evaluate(); evaluate();
    fail(true); evaluate();
    require(evaluation_order == "ANSS" && sr_inputs.back().color == f.textures12[0].Get() &&
        sr_inputs[0].reset && sr_inputs[1].reset, "NR failure restores raw SR input and resets both processed-input histories");
    fail(false); evaluate();
    require(nr_reset == 1 && sr_inputs[0].reset && sr_inputs[1].reset, "NR recovery resets raw-to-processed histories once");
    command("1\n244\nset\nNrProcessingOrder=0\nNrAlignmentBorder=false\nEnabled=false"); evaluate();
    if (evaluation_order != "ASN") { printf("Unexpected NR order: %s\n", evaluation_order.c_str()); puts(snapshot(get).c_str()); }
    require(evaluation_order == "ASN", "After NR also works with ordinary SR beneath AFW");
    command("1\n245\nset\nNrEnabled=false"); evaluate();
    require(evaluation_order == "AS", "NR disabled performs no private neural work");
    command("1\n246\nset\nNrEnabled=true"); evaluate();
    require(evaluation_order == "ASN" && nr_reset == 1, "Re-enabled After NR resets skipped temporal history");
    evaluate(); require(nr_reset == 0, "Re-enabled NR reset is not repeated");
    command("1\n247\nset\nNrFoveated=false\nNrWorkingScale=0.5"); evaluate();
    require(snapshot(get).find("\"region_width\":256") != std::string::npos && snapshot(get).find("\"working_width\":128") != std::string::npos,
        "Full-frame AFW NR supports its working scale without resizing game output");
    afw_expected_reset = 1; f.params.Set("Reset", 1U); evaluate();
    require(nr_reset == 1 && get_ui(&f.params, "Reset") == 1, "AFW game reset propagates to NR without changing original parameters");
    afw_expected_reset = 0; f.params.Set("Reset", 0U);
    command("1\n248\nset\nNrEnabled=false\nEnabled=true\nNrFoveated=true");
    evaluate();
    FreeLibrary(nr);
    puts("PASS: AFW NR Before/After order, independent coverage, full-frame scaling, failure recovery and original contract");
}

void verify_afw_test(CheekyUEVRSnapshotFn get, void (*command)(const char*)) {
    auto& f = fixture();
    command("1\n200\nset\nEnabled=true\nPeripheralDlaa=true\nPeripheralDlaaScale=0.5\nWidth=0.35\nHeight=0.4\nCenterSupersampling=2\nAutoStereoAlignment=true\nCenterMode=2\nNrEnabled=false\nAlignmentBorder=false");
    if (afw_missing_lower) command("1\n249\nset\nNrEnabled=true");
    require(snapshot(get).find("\"afw_experiment\":{\"enabled\":true") != std::string::npos, "AFW experiment visible in exported status");
    if (afw_ota) {
        const auto discovery = snapshot(get);
        require(discovery.find(afw_ambiguous ? "\"runtime_candidates\":2,\"runtime_selected\":false" : "\"runtime_candidates\":1,\"runtime_selected\":true") != std::string::npos,
            "OTA SR discovery excludes decoys and rejects ambiguous runtimes");
        if (!afw_ambiguous) require(GetModuleHandleW(L"nvngx_dlss.dll") == nullptr, "OTA test has no conventionally named SR module");
    }
    const auto creates_before = f.creates();
    // Exceed AFW's 90-frame suspension window. It must see one stable, full-size
    // input per game evaluation, while the private periphery really is smaller.
    for (unsigned i = 0; i < 100; ++i) {
        require(ngx_succeeded(f.evaluate()), "AFW game evaluation succeeds");
        f.finish_gpu();
        require(get_ui(&f.params, "Width") == 128 && get_ui(&f.params, "OutWidth") == 256 && get_ui(&f.params, "Reset") == 0,
            "AFW game dimensions and reset restored");
    }
    require(afw_full_calls == 100 && proc<Counter>(afw_core, "CheekyFakeCreates")() == 1,
        "AFW core receives game work only, no private creates/evaluations");
    require(afw_contract_ok && afw_order_ok, "AFW sees full resources and precedes private DLSS work");
    const auto status = snapshot(get);
    if (afw_missing_lower) {
        const auto nr_begin = status.find("\"nr_details\":{");
        require(nr_begin != std::string::npos && status.substr(nr_begin, status.find('}', nr_begin) - nr_begin).find("\"evaluations\":0") != std::string::npos,
            "Unavailable or ambiguous nested route does not run enabled NR outside AFW");
    }
    require(status.find("\"rejected_core_reentry\":0") != std::string::npos, "Supported route never reenters core");
    require(status.find("\"warp_observer_ready\":true") != std::string::npos &&
        status.find("\"warp_calls\":0") != std::string::npos && status.find("\"last_warp_age_ms\":-1") != std::string::npos,
        "Module detection and SR evaluations do not claim warp activity");
    std::array<unsigned char, 256> opaque_warp_parameters{};
    for (unsigned i = 0; i < opaque_warp_parameters.size(); ++i) opaque_warp_parameters[i] = static_cast<unsigned char>(i);
    const auto unchanged_warp_parameters = opaque_warp_parameters;
    for (unsigned i = 0; i < 3; ++i) afw_cached_warp(opaque_warp_parameters.data());
    require(opaque_warp_parameters == unchanged_warp_parameters &&
        proc<void*(*)()>(afw_warp_module, "CheekyFakeLastWarpParameters")() == opaque_warp_parameters.data() &&
        proc<Counter>(afw_warp_module, "CheekyFakeWarpCalls")() == 3 &&
        snapshot(get).find("\"warp_calls\":3") != std::string::npos,
        "Warp observer forwards cached calls and opaque parameters exactly once without mutation");
    if (afw_missing_lower) {
        require(f.creates() == creates_before && afw_lower_calls == (afw_ambiguous ? 100U : 0U), "Absent lower route stays ordinary DLSS");
        require(status.find("\"missing_lower_calls\":100") != std::string::npos, "Absent route explicitly reported");
        if (afw_public_first) require(status.find("\"standalone_lower_calls\":100") != std::string::npos,
            "Reversed hook topology remains full-frame public-to-core passthrough");
    } else {
        if (f.creates() != creates_before + 2 || afw_lower_calls != 200) puts(status.c_str());
        require(f.creates() == creates_before + 2 && afw_lower_calls == 200 && afw_reduced_depth_calls == 100,
            "Center and reduced periphery run below AFW with stable private handles");
        require(afw_largest_output_width > 256, "AFW center supersampling enlarges only the private output");
        require(status.find("\"missing_lower_calls\":0") != std::string::npos && status.find("\"lower_calls\":100") != std::string::npos,
            "One nested game DLSS route per full-frame core call");
        const auto nr_begin = status.find("\"nr_details\":{");
        require(nr_begin != std::string::npos && status.substr(nr_begin, status.find('}', nr_begin) - nr_begin).find("\"evaluations\":0") != std::string::npos,
            "Disabled NR does not evaluate");
        // Change coverage and quality while AFW continues to see the same full
        // game textures. Stable manual placement must retain private handles.
        command("1\n210\nset\nAfwManualCoverage=true\nAfwWarpMargin=0.05\nXOffset=0.5\nHeightOffset=-0.25\nCenterSupersampling=1.25");
        require(ngx_succeeded(f.evaluate()), "Manual AFW coverage evaluates"); f.finish_gpu();
        const auto manual_creates = f.creates();
        for (unsigned i = 0; i < 4; ++i) {
            require(ngx_succeeded(f.evaluate()), "Manual AFW coverage stays active"); f.finish_gpu();
        }
        require(f.creates() == manual_creates && afw_contract_ok && afw_order_ok,
            "Manual envelope retains private history and isolates resolution changes from AFW");
        require(snapshot(get).find("\"manual_coverage\":true") != std::string::npos,
            "Report includes effective AFW coverage mode");
        command("1\n212\nset\nAfwAutomaticCoverage=true\nAlignedHeightOffset=0.1");
        require(ngx_succeeded(f.evaluate()), "Automatic stereo envelope evaluates below AFW"); f.finish_gpu();
        require(snapshot(get).find("\"coverage_mode\":2") != std::string::npos && afw_contract_ok,
            "Automatic placement uses matching public host projections without resizing AFW inputs");
        const auto automatic_creates = f.creates();
        for (unsigned i = 0; i < 3; ++i) { require(ngx_succeeded(f.evaluate()), "Stable automatic evaluation"); f.finish_gpu(); }
        require(f.creates() == automatic_creates, "Stable automatic geometry retains private handles");
        Sleep(300);
        require(ngx_succeeded(f.evaluate()), "Stale host projection falls back safely"); f.finish_gpu();
        require(snapshot(get).find("\"coverage_mode\":0") != std::string::npos,
            "Stale public projections select the centered fallback in the real hook");
        CheekyUEVRStereoProjection mismatched;
        mismatched.active = 1; mismatched.output_width = 512; mismatched.output_height = 256;
        for (auto& m : mismatched.matrices) { m[0] = m[5] = m[11] = 1.F; m[14] = 10.F; }
        const auto publish = proc<CheekyUEVRPublishStereoFn>(GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll"), "CheekyUEVR_PublishStereo");
        require(publish(1, &mismatched), "Publish a valid projection for a different output size");
        require(ngx_succeeded(f.evaluate()), "Mismatched projection cannot displace current DLSS view"); f.finish_gpu();
        require(snapshot(get).find("\"coverage_mode\":0") != std::string::npos, "Extra output view retains centered coverage");
        command("1\n214\nget");
        require(ngx_succeeded(f.evaluate()), "Fresh matching projection resumes automatic coverage"); f.finish_gpu();
        require(snapshot(get).find("\"coverage_mode\":2") != std::string::npos && afw_contract_ok,
            "Projection reacquisition preserves AFW's full-frame contract");
        command("1\n213\nset\nAfwAutomaticCoverage=false");
        command("1\n211\nset\nAfwManualCoverage=false\nCenterSupersampling=2");
        require(ngx_succeeded(f.evaluate()), "Tested centered AFW mode can be restored"); f.finish_gpu();
        // A private failure must restore the original contract and reset the
        // game feature's starved history exactly on the fallback transition.
        proc<void(*)(unsigned)>(f.ngx, "CheekyFakeFailNextEvaluations")(2);
        require(ngx_succeeded(f.evaluate()), "Private failure falls back to game DLSS"); f.finish_gpu();
        require(afw_full_resets == 1 && afw_contract_ok && get_ui(&f.params, "Reset") == 0,
            "Fallback resets native history below AFW and restores game Reset");
        command("1\n201\nset\nEnabled=false");
        require(ngx_succeeded(f.evaluate()), "Disabled SR uses ordinary nested DLSS"); f.finish_gpu();
        require(afw_full_resets == 1, "Consecutive native frame does not repeat reset");
        command("1\n202\nset\nEnabled=true");
        require(ngx_succeeded(f.evaluate()), "SR resumes after native fallback"); f.finish_gpu();
        command("1\n203\nset\nEnabled=false");
        require(ngx_succeeded(f.evaluate()), "Toggle off resumes native history"); f.finish_gpu();
        require(afw_full_resets == 2, "Toggle transition resets native history once");
        verify_afw_gaze_history(get, command);
        verify_afw_nr(get, command);
    }
    require(ngx_succeeded(f.release(f.handle)), "Wrapped core release cleans lower game and private handles");
    require(f.creates() == f.releases(), "No leaked lower private features after core release");
    // A post-injection create must also reach the lower lifecycle hook with its
    // own handle; the core and public wrappers deliberately have different IDs.
    require(ngx_succeeded(f.create12(f.list.Get(), 1U, &f.params, &f.handle)), "Create wrapped feature after injection");
    if (f.use_sl) proc<void(*)(Evaluate12,const NgxHandle*,NgxParameters*)>(f.sl, "CheekyFakeConfigure")(f.evaluate12, f.handle, &f.params);
    command("1\n204\nset\nEnabled=true");
    require(ngx_succeeded(f.evaluate()), "Evaluate post-injection wrapped feature"); f.finish_gpu();
    require(ngx_succeeded(f.release(f.handle)) && f.creates() == f.releases(), "Post-injection lower lifecycle cleans all features");
    require(afw_contract_ok && afw_order_ok, "Full-frame contract survives lifecycle and fallback transitions");
    puts("PASS: AFW full-frame routing, private resolution isolation, fallback, and lifecycle");
}

void verify_late_attach_test(CheekyUEVRSnapshotFn get, void (*command)(const char*), void (*before_frame)(), void (*set_mode)(unsigned)) {
    auto& f=fixture();
    f.before_frame = before_frame;
    // Missing metadata must forward unchanged and must not create a feature.
    const auto complete=f.params.values;
    for(const auto* key : {"DLSS.Feature.Create.Flags","PerfQualityValue","OutWidth","Depth","MotionVectors","Output"}) {
        f.params.values=complete; f.params.values.erase(key);
        require(ngx_succeeded(f.evaluate()),"Incomplete late evaluation forwards");
        if(f.creates()!=1 || f.params.values!=[&] { auto expected=complete; expected.erase(key); return expected; }()) {
            printf("Incomplete metadata failure: %s creates=%u\n",key,f.creates());
            for(const auto& [name,value] : f.params.values) {
                auto found=complete.find(name);
                if(found==complete.end() || found->second!=value) printf("Parameter changed: %s\n",name.c_str());
            }
        }
        require(f.creates()==1 && f.params.values==[&] { auto expected=complete; expected.erase(key); return expected; }(),"Incomplete metadata does not mutate parameters or create features");
    }
    f.params.values=complete;
    require(ngx_succeeded(f.evaluate()),"Complete late evaluation"); f.finish_gpu();
    if(f.creates()<2) { puts(snapshot(get).c_str()); throw std::runtime_error("Late evaluation did not create private feature"); }
    const auto first_private=f.creates();
    require(get_ui(&f.params,"Width")==128 && get_ui(&f.params,"OutWidth")==256,"Evaluation restores dimensions");
    require(ngx_succeeded(f.evaluate()),"Repeated late evaluation"); f.finish_gpu();
    require(f.creates()==first_private,"Adopted feature reuses its private state");
    f.params.values.erase("DLSS.Feature.Create.Flags");
    require(ngx_succeeded(f.evaluate()),"Incomplete frame after adoption forwards");
    require(f.creates()==first_private,"Do not recreate with guessed flags after adoption");
    f.params.Set("DLSS.Feature.Create.Flags",2U);
    if(f.use_sl) {
        const auto status=snapshot(get);
        require(status.find("\"options_hooked\":true")!=status.npos && status.find("\"options_seen\":false")!=status.npos,"Proactive SL setter hook without invented options");
        require(status.find("\"native_fallback\":true")!=status.npos,"Missing SL options permit native NGX processing");
        const auto before=proc<Counter>(f.sl,"CheekyFakeGetCalls")();
        require(f.sl_options(&f.viewport,&f.options)==0,"Previously cached setter remains callable");
        require(snapshot(get).find("\"options_seen\":true")!=std::string::npos,"Previously cached setter is intercepted");
        require(proc<Counter>(f.sl,"CheekyFakeGetCalls")()==before,"Game did not request setter again");
        require(proc<Counter>(f.sl,"CheekyFakeOptionWidth")()==256 && proc<Counter>(f.sl,"CheekyFakeOptionHeight")()==256,"Game options forwarded unmodified");
        if(f.context) {
            require(ngx_succeeded(f.evaluate()),"DX11 Streamline with known options still uses native path");
            require(snapshot(get).find("\"native_fallback\":true")!=std::string::npos,"DX11 must not enter DX12 SL compositor");
        }
        ++f.viewport.value;
        require(ngx_succeeded(f.evaluate()),"Another viewport uses native fallback"); f.finish_gpu();
        require(snapshot(get).find("\"native_fallback\":true")!=std::string::npos,"Do not reuse another viewport's SL options");
        --f.viewport.value;
        f.options.struct_version=99;
        require(f.sl_options(&f.viewport,&f.options)==0,"Unknown options version forwarded");
        require(snapshot(get).find("\"options_seen\":false")!=std::string::npos,"Unknown options version not reconstructed");
    }
    const auto released=f.releases(); require(ngx_succeeded(f.release(f.handle)),"Release adopted game feature");
    require(f.releases()>=released+2,"Release cleans up both game and private features");
    // Recreate through cached CreateFeature after injection, then release again.
    if(f.context) require(ngx_succeeded(f.create11(f.context.Get(),1,&f.params,&f.handle)),"Recreate DX11 game feature");
    else require(ngx_succeeded(f.create12(f.list.Get(),1,&f.params,&f.handle)),"Recreate DX12 game feature");
    if(f.use_sl) {
        if(f.context) proc<void(*)(Evaluate11,const NgxHandle*,NgxParameters*)>(f.sl,"CheekyFakeConfigure11")(f.evaluate11,f.handle,&f.params);
        else proc<void(*)(Evaluate12,const NgxHandle*,NgxParameters*)>(f.sl,"CheekyFakeConfigure")(f.evaluate12,f.handle,&f.params);
    }
    require(ngx_succeeded(f.evaluate()),"Evaluate recreated game feature"); f.finish_gpu();
    verify_nr_reset_isolation(command);
    if (!f.context && !f.use_sl) {
        command("1\n70\nset\nEnabled=false");
        // UE can discard a recording after evaluation. Exhaust more than the
        // entire timestamp pool without submitting those command lists.
        for (unsigned i = 0; i < 9; ++i) {
            Sleep(130);
            require(ngx_succeeded(f.evaluate()), "Evaluate discarded timing recording");
            check(f.list->Close(), "Close discarded timing recording");
            check(f.allocator->Reset(), "Reset discarded allocator");
            check(f.list->Reset(f.allocator.Get(), nullptr), "Discard timing recording");
            check(f.list->Close(), "Close empty discarded list");
            f.list.Reset(); f.allocator.Reset();
            check(f.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)), "Replacement allocator");
            check(f.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, f.allocator.Get(), nullptr, IID_PPV_ARGS(&f.list)), "Replacement list");
        }
        for (unsigned i = 0; i < 8; ++i) {
            Sleep(130);
            require(ngx_succeeded(f.evaluate()), "Evaluate native timing frame");
            f.finish_gpu(); command("1\n71\nget");
        }
        const auto data = snapshot(get);
        const auto offset = data.rfind("\"native_ms\":");
        require(offset != data.npos && std::stod(data.substr(offset + 12)) > 0,
            "Native GPU timings recover after discarded command lists");
        TimingListAlias alias(f.list.Get());
        for (unsigned i = 0; i < 8; ++i) {
            Sleep(130);
            const auto result = f.use_c ? f.evaluate12c(alias.get(), f.handle, &f.params, nullptr)
                : f.evaluate12(alias.get(), f.handle, &f.params, nullptr);
            require(ngx_succeeded(result), "Evaluate through forwarding command-list alias");
            f.finish_gpu(); command("1\n72\nget");
            require(alias.references == 0, "Submitted native list matches timing recorded through wrapper");
        }
        require(snapshot(get).find("\"waiting_submission\":0") != std::string::npos, "No stranded wrapper timestamp slots");
        command("1\n73\nset\nEnabled=true\nPeripheralDlaa=true");
        for (unsigned i = 0; i < 8; ++i) {
            Sleep(130); require(ngx_succeeded(f.evaluate()), "Foveated and peripheral timing frame");
            f.finish_gpu(); command("1\n74\nget");
        }
        const auto enabled_data = snapshot(get);
        for (const auto* key : {"\"foveated_ms\":", "\"peripheral_ms\":"}) {
            const auto position = enabled_data.rfind(key);
            if (position == enabled_data.npos || std::stod(enabled_data.substr(position + std::strlen(key))) <= 0) {
                printf("Missing timing: %s\n%s\n", key, enabled_data.c_str());
            }
            require(position != enabled_data.npos && std::stod(enabled_data.substr(position + std::strlen(key))) > 0,
                "Foveated center and peripheral GPU timings reach snapshot");
        }
        puts("GPU timestamps: discarded recordings, forwarding wrapper, native/center/peripheral readback passed");
    }
    if (set_mode && !f.context) {
        command("1\n120\nset\nEnabled=true\nPeripheralDlaa=true\nNrEnabled=true\nNrProcessingOrder=0\nNrFoveated=true\nAutoStereoAlignment=false\nCenterMode=0\nWidth=0.5\nHeight=0.5\nXOffset=0\nHeightOffset=0");
        f.params.Set("Reset", 0U);
        if (f.use_sl) {
            f.options.struct_version = 3;
            require(f.sl_options(&f.viewport, &f.options) == 0, "Mode transition viewport options");
            f.complete_sl_metadata = true;
        }
        proc<void(*)(void(*)(const NgxParameters*))>(f.ngx, "CheekyFakeObserve")(&observe_sr);
        proc<void(*)(void(*)(const NgxParameters*))>(GetModuleHandleW(L"nvngx_dlssnr.dll"), "CheekyFakeObserve")(&observe_nr);
        const auto evaluate = [&] {
            sr_inputs.clear(); evaluation_order.clear();
            const auto original = f.params.values;
            require(ngx_succeeded(f.evaluate()), "Mode transition evaluation"); f.finish_gpu();
            require(f.params.values == original, "Mode transition restores game parameters");
        };
        evaluate(); evaluate();
        require(evaluation_order == "SSN" && sr_inputs[0].reset == 0 && sr_inputs[1].reset == 0 && nr_reset == 0,
            "Non-AFW SR/NR histories settle before switching modes");
        for (unsigned mode : {3U, UINT32_MAX, 0U}) {
            set_mode(mode); before_frame();
            if (mode == 0U) { f.before_frame = nullptr; Sleep(270); }
            evaluate();
            require(evaluation_order == "S" && sr_inputs[0].color == f.textures12[0].Get(),
                "AFW or unknown/stale mode forwards the full-frame call without private SR/NR");
            set_mode(0); f.before_frame = before_frame;
            evaluate();
            require(evaluation_order == "SSN" && sr_inputs[0].reset == 1 && sr_inputs[1].reset == 1 && nr_reset == 1,
                "Returning from skipped AFW frames resets private center/peripheral SR and NR histories");
            evaluate();
            require(evaluation_order == "SSN" && sr_inputs[0].reset == 0 && sr_inputs[1].reset == 0 && nr_reset == 0,
                "Recovered non-AFW histories reset only once");
        }
        puts("Inactive AFW: mode switches, stale-mode fallback and SR/NR history recovery passed");
    }
    require(ngx_succeeded(f.release(f.handle)),"Release recreated feature");
    puts("Late attachment: cached exports, pre-existing feature, missing metadata, private reuse, release/recreation passed");
}
