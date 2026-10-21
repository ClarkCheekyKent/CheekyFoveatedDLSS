#pragma once
#include <variant>

// Invalid inputs must be rejected before they can advance FG's camera history.
void fg_camera_validation() {
    using namespace cheeky::fg;
    const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
    Matrix inverse_result;
    for(unsigned i=0;i<16;++i)for(float bad:{nan,inf,-inf}){
        auto m=identity();m.m[i]=bad;
        check(!inverse(m,inverse_result),"Matrix inversion rejects every non-finite input element");
    }
    check(!inverse(Matrix{},inverse_result),"Singular matrix is rejected");
    auto scaled=identity();scaled.m[0]=.25f;scaled.m[5]=4.f;
    check(inverse(scaled,inverse_result)&&inverse_result.m[0]==4.f&&inverse_result.m[5]==.25f,"Finite invertible matrices remain usable");
    XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    view.pose.orientation.w=1;view.fov={-.7f,.6f,.65f,-.55f};
    for(bool inverted:{false,true}){
        check(camera(view,nullptr,inverted).valid&&camera(view,&view,inverted).valid,"Valid asymmetric camera remains usable at either depth convention");
        auto rotation=view;rotation.pose.orientation.w=2;
        check(camera(rotation,&view,inverted).valid,"Finite non-unit quaternions remain normalized by camera math");
        for(bool previous:{false,true}){
            auto reject=[&](const XrCompositionLayerProjectionView& bad){
                check(!(previous?camera(view,&bad,inverted):camera(bad,&view,inverted)).valid,"Invalid current or previous camera is rejected");
            };
            for(float bad:{nan,inf,-inf}){
                for(unsigned axis=0;axis<3;++axis){auto broken=view;
                    if(axis==0)broken.pose.position.x=bad;else if(axis==1)broken.pose.position.y=bad;else broken.pose.position.z=bad;reject(broken);}
                for(unsigned axis=0;axis<4;++axis){auto broken=view;
                    if(axis==0)broken.pose.orientation.x=bad;else if(axis==1)broken.pose.orientation.y=bad;
                    else if(axis==2)broken.pose.orientation.z=bad;else broken.pose.orientation.w=bad;reject(broken);}
                for(unsigned edge=0;edge<4;++edge){auto broken=view;
                    if(edge==0)broken.fov.angleLeft=bad;else if(edge==1)broken.fov.angleRight=bad;
                    else if(edge==2)broken.fov.angleUp=bad;else broken.fov.angleDown=bad;reject(broken);}
            }
            auto broken=view;broken.pose.orientation={0,0,0,0};reject(broken);
            broken=view;broken.fov.angleRight=broken.fov.angleLeft;reject(broken);
            broken=view;broken.fov.angleUp=broken.fov.angleDown;reject(broken);
            broken=view;std::swap(broken.fov.angleRight,broken.fov.angleLeft);reject(broken);
            broken=view;broken.pose.orientation.x=std::numeric_limits<float>::max();reject(broken);
        }
    }
    std::cout<<"PASS FG camera validation: NaN/infinity, singular matrices, current/previous poses, rotations, FOV and both depth conventions\n";
}

namespace fg_completion_fixture {
struct Parameters final : NVSDK_NGX_Parameter {
    using Value=std::variant<unsigned long long,float,double,unsigned,int,ID3D11Resource*,ID3D12Resource*,void*>;
    std::unordered_map<std::string,Value> values;
#define FG_COMPLETION_PARAMETER(T) \
    void Set(const char* key,T value) override {values.insert_or_assign(key,value);} \
    NVSDK_NGX_Result Get(const char* key,T* out) const override { \
        const auto it=values.find(key);if(it==values.end()||!out)return NVSDK_NGX_Result_FAIL_InvalidParameter; \
        const auto* value=std::get_if<T>(&it->second);if(!value)return NVSDK_NGX_Result_FAIL_InvalidParameter; \
        *out=*value;return NVSDK_NGX_Result_Success;}
    FG_COMPLETION_PARAMETER(unsigned long long)
    FG_COMPLETION_PARAMETER(float)
    FG_COMPLETION_PARAMETER(double)
    FG_COMPLETION_PARAMETER(unsigned)
    FG_COMPLETION_PARAMETER(int)
    FG_COMPLETION_PARAMETER(ID3D11Resource*)
    FG_COMPLETION_PARAMETER(ID3D12Resource*)
    FG_COMPLETION_PARAMETER(void*)
#undef FG_COMPLETION_PARAMETER
    void Reset() override {values.clear();}
};
inline Microsoft::WRL::ComPtr<ID3D12Resource> flag_upload;
inline unsigned calls{},fail_at{},created{};
inline std::vector<unsigned> reset_flags;
NVSDK_NGX_Result NVSDK_CONV allocate(NVSDK_NGX_Parameter** out){*out=new Parameters;return NVSDK_NGX_Result_Success;}
NVSDK_NGX_Result NVSDK_CONV destroy(NVSDK_NGX_Parameter* p){delete static_cast<Parameters*>(p);return NVSDK_NGX_Result_Success;}
NVSDK_NGX_Result NVSDK_CONV create(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle** out){
    *out=reinterpret_cast<NVSDK_NGX_Handle*>(new unsigned(++created));return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV release(NVSDK_NGX_Handle* h){delete reinterpret_cast<unsigned*>(h);return NVSDK_NGX_Result_Success;}
NVSDK_NGX_Result NVSDK_CONV evaluate(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter* p,PFN_NVSDK_NGX_ProgressCallback){
    unsigned reset{};check(NVSDK_NGX_SUCCEED(p->Get("DLSSG.Reset",&reset)),"Fixture receives actual NGX reset policy");reset_flags.push_back(reset);
    if(++calls==fail_at)return NVSDK_NGX_Result_FAIL_InvalidParameter;
    ID3D12Resource* disable{};check(NVSDK_NGX_SUCCEED(p->Get("DLSSG.OutputDisableInterpolation",&disable))&&disable,"Fixture receives disable-output buffer");
    cheeky::fg::color_transition(list,disable,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(disable,0,flag_upload.Get(),0,4);
    cheeky::fg::color_transition(list,disable,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return NVSDK_NGX_Result_Success;
}
}

// Real worker, command recording, queue submission and readback on WARP. Only
// the NGX backend is replaced, so failed and suppressed evaluations are deterministic.
void fg_completion_validation(ID3D12Device* device,ID3D12CommandQueue* queue) {
    using namespace cheeky::fg;using namespace fg_completion_fixture;using Microsoft::WRL::ComPtr;
    calls=fail_at=created=0;reset_flags.clear();
    Worker worker;worker.probe_attempted=true;worker.capability_ready=true;worker.probed_device=device;
    check(worker.initialize(device,queue,64,64,DXGI_FORMAT_R8G8B8A8_UNORM),"Initialize completion fixture without NVIDIA inference");
    worker.allocate=allocate;worker.destroy=destroy;worker.create=create;worker.release=release;worker.evaluate=evaluate;
    auto buffer=[&](D3D12_HEAP_TYPE heap_type,UINT64 bytes){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=heap_type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width=bytes;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> result;check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&result))),"Completion upload buffer");return result;
    };
    flag_upload=buffer(D3D12_HEAP_TYPE_UPLOAD,8);
    auto flag=[&](unsigned value){void* data{};check(SUCCEEDED(flag_upload->Map(0,nullptr,&data)),"Map completion flag");
        std::memcpy(data,&value,4);flag_upload->Unmap(0,nullptr);};flag(1);
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))&&
        SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))),"Completion fixture initialization commands");
    ComPtr<ID3D12DescriptorHeap> rtvs;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=3;
    check(SUCCEEDED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs))),"Completion RTV heap");
    const auto stride=device->GetDescriptorHandleIncrementSize(hd.Type);
    auto texture=[&](DXGI_FORMAT format,unsigned index,const std::array<float,4>& value,unsigned size=64){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
        d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=size;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;
        d.Format=format;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;ComPtr<ID3D12Resource> result;
        check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&result))),"Completion fixture texture");
        auto slot=rtvs->GetCPUDescriptorHandleForHeapStart();slot.ptr+=index*stride;device->CreateRenderTargetView(result.Get(),nullptr,slot);
        list->ClearRenderTargetView(slot,value.data(),0,nullptr);color_transition(list.Get(),result.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);return result;
    };
    auto color=texture(DXGI_FORMAT_R8G8B8A8_UNORM,0,{1,0,0,1}),depth=texture(DXGI_FORMAT_R32_FLOAT,1,{.5f,0,0,0}),motion=texture(DXGI_FORMAT_R32G32_FLOAT,2,{0,0,0,0});
    check(SUCCEEDED(list->Close()),"Close completion initialization");ID3D12CommandList* initial[]{list.Get()};queue->ExecuteCommandLists(1,initial);
    ComPtr<ID3D12Fence> initialized;check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&initialized)))&&SUCCEEDED(queue->Signal(initialized.Get(),1)),"Completion initialization fence");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    check(event&&SUCCEEDED(initialized->SetEventOnCompletion(1,event))&&WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"Wait completion initialization");CloseHandle(event);
    CheekyFgPair guides;guides.producer_queue=queue;
    for(auto& g:guides.eyes){g.depth=depth.Get();g.motion=motion.Get();g.width=g.height=g.depth_width=g.depth_height=g.motion_width=g.motion_height=64;g.configuration=1;}
    XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};view.pose.orientation.w=1;view.fov={-.7f,.7f,.7f,-.7f};
    const std::array<Camera,2> cameras{camera(view,&view),camera(view,&view)};
    const std::array<ID3D12Resource*,2> colors{color.Get(),color.Get()};std::array<ID3D12Resource*,2> output{};unsigned submissions{};
    auto generate=[&](const std::array<Camera,2>& input,std::array<Worker::OutputCopy,2>* copies=nullptr){
        ++guides.render_frame;
        return worker.generate(guides,colors,colors,input,output,copies,[](void* p) noexcept {++*static_cast<unsigned*>(p);},&submissions);
    };
    check(!generate(cameras)&&worker.temporal_evaluation_completed&&!worker.failed&&submissions==1,"Suppressed NGX output is a completed submitted evaluation");
    check(!generate(cameras)&&worker.temporal_evaluation_completed&&!worker.failed&&created==2&&submissions==2,"Repeated warm-up preserves feature history");
    check(reset_flags.size()==4&&reset_flags[2]==0&&reset_flags[3]==0,"Warm-up is not trapped in repeated history resets");
    flag(0);check(generate(cameras)&&worker.temporal_evaluation_completed&&!worker.failed,"NGX resumes interpolation after warm-up");
    const auto serial=worker.serial,accepted_frame=worker.features[0].evaluated_frame;const auto submitted=submissions;
    auto broken=cameras;broken[0].valid=false;const auto calls_before=calls;
    check(!generate(broken)&&!worker.temporal_evaluation_completed&&!worker.failed&&calls==calls_before&&worker.serial==serial,"Input rejection clears stale completion without evaluating or submitting");
    broken=cameras;broken[unsigned(worker.gpu_timings.samples&1)^1u].view_to_clip.m[0]=std::numeric_limits<float>::quiet_NaN();
    check(!generate(broken)&&!worker.temporal_evaluation_completed&&!worker.failed&&worker.serial==serial&&submissions==submitted&&calls==calls_before+1,
        "Invalid second-eye projection discards the first eye's recorded NGX commands");
    check(worker.features[0].evaluated_frame==accepted_frame,"Rejected camera pair cannot advance submitted crop history");
    fail_at=calls+2;
    check(!generate(cameras)&&!worker.temporal_evaluation_completed&&worker.failed&&worker.serial==serial&&submissions==submitted,"Second-eye NGX failure discards the entire recorded batch");
    fail_at=0;for(auto& g:guides.eyes)++g.configuration;
    check(generate(cameras)&&worker.temporal_evaluation_completed&&!worker.failed,"Validated source change recovers after evaluation failure");
    const auto before_copy=worker.serial;const auto before_copy_calls=calls;
    // An incompatible-format publication target fails after both NGX calls,
    // before ExecuteCommandLists. No recorded evaluation can advance history.
    std::array<Worker::OutputCopy,2> copies{{{depth.Get()},{motion.Get()}}};
    check(!generate(cameras,&copies)&&!worker.temporal_evaluation_completed&&worker.failed&&worker.serial==before_copy&&calls==before_copy_calls+2,
        "Failed final publication discards recorded evaluations and clears completion");
    for(auto& g:guides.eyes)++g.configuration;
    check(generate(cameras)&&worker.temporal_evaluation_completed&&!worker.failed,"Recover after discarded publication");
    worker.reset_features();check(!worker.temporal_evaluation_completed,"Feature reset invalidates completion state");
    check(generate(cameras)&&worker.temporal_evaluation_completed,"Generation resumes after resetting features");
    worker.reset();check(!worker.temporal_evaluation_completed,"Worker reset clears completion state");flag_upload.Reset();
    std::cout<<"PASS FG evaluation completion: warm-up/resume, early rejection, partial recording, NGX failure, failed publication, recovery and reset\n";
}

void fg_hardening_validation() {
    fg_camera_validation();
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;Microsoft::WRL::ComPtr<IDXGIAdapter> warp;
    check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))&&SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))),"Hardening WARP adapter");
    Microsoft::WRL::ComPtr<ID3D12Device> device;Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};
    check(SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))&&SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"Hardening WARP device/queue");
    fg_completion_validation(device.Get(),queue.Get());
}
