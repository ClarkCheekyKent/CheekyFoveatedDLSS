#include "late_attach_tests.hpp"
#include "mock_ngx_parameters.hpp"
#include "streamline_abi.hpp"
#include "timing_list_alias.hpp"
#include <wrl/client.h>
#include <array>
#include <vector>
#include <stdexcept>
#include <cstdio>
#include <cmath>
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
float nr_motion_x{};
unsigned nr_motion_width{};
ComPtr<ID3D12Resource> nr_motion_resource;
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
    NgxResult evaluate() {
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
    // Exercise the production feature cache through the fake NVIDIA runtime:
    // warm two toggle states, then churn dimensions without growing live features.
    const auto nr_creates = proc<Counter>(nr, "CheekyFakeCreates");
    const auto nr_releases = proc<Counter>(nr, "CheekyFakeReleases");
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
    command("1\n97\nset\nEnabled=true\nNrEnabled=false\nPeripheralDlaa=false\nAutoStereoAlignment=false");
    nr_motion_resource.Reset();
    puts("NR reset isolation: After jump, evaluation order, per-view transitions, fallback and restoration passed");
}
}
void prepare_late_attach_test(const std::filesystem::path& bin,ID3D11Device* dx11,ID3D12Device* dx12,ID3D12CommandQueue* queue,bool use_c,bool use_sl) {
    auto& f=fixture(); f.use_c=use_c; f.use_sl=use_sl; f.device=dx12; f.queue=queue;
    f.ngx=LoadLibraryW((bin/L"test-fixtures"/L"nvngx_dlss.dll").c_str()); require(f.ngx!=nullptr,"Load fake NGX before plugin");
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
void verify_late_attach_test(CheekyUEVRSnapshotFn get, void (*command)(const char*)) {
    auto& f=fixture();
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
    require(ngx_succeeded(f.release(f.handle)),"Release recreated feature");
    puts("Late attachment: cached exports, pre-existing feature, missing metadata, private reuse, release/recreation passed");
}
