#include "late_attach_tests.hpp"
#include "mock_ngx_parameters.hpp"
#include "streamline_abi.hpp"
#include <wrl/client.h>
#include <vector>
#include <stdexcept>
#include <cstdio>
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
    NgxHandle* handle{}; bool use_c{},use_sl{};
    NgxResult evaluate() {
        if(use_sl) { const void* inputs[]{&viewport}; return sl_evaluate(0,nullptr,inputs,1,context ? static_cast<void*>(context.Get()) : static_cast<void*>(list.Get())); }
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
            d.DepthOrArraySize=d.MipLevels=1; d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; d.SampleDesc.Count=1; d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
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
void verify_late_attach_test(CheekyUEVRSnapshotFn get) {
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
    require(ngx_succeeded(f.release(f.handle)),"Release recreated feature");
    puts("Late attachment: cached exports, pre-existing feature, missing metadata, private reuse, release/recreation passed");
}
