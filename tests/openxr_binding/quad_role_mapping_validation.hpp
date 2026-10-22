#pragma once
namespace {
void quad_role_mapping_validation() {
    using Microsoft::WRL::ComPtr;using cheeky::quad_capture::RoleMapping11;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
    auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context);
    if(hr==DXGI_ERROR_SDK_COMPONENT_MISSING)hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context);
    check(SUCCEEDED(hr),"Quad mapping WARP device");
    ComPtr<ID3D11InfoQueue> errors;device.As(&errors);if(errors)errors->ClearStoredMessages();
    RoleMapping11 mapper(device.Get());
    cheeky::quad_capture::Compositor11 convert;convert.device=device;convert.create=quad11_create;convert.destroy=quad11_destroy;
    convert.enumerate_images=quad11_images;convert.acquire=quad11_acquire;convert.wait=quad11_wait;convert.release=quad11_release;
    check(convert.initialize(),"Quad mapping conversion renderer");
    for(auto& eye:convert.eyes){eye.width=eye.height=256;eye.format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;}
    std::array<ComPtr<ID3D11Texture2D>,4> sources,submitted;
    std::array<ComPtr<ID3D11RenderTargetView>,4> source_targets;
    std::array<RoleMapping11::Input,4> targets;
    std::array<CheekyQuadSourceD3D11V1,4> metadata;
    for(unsigned i=0;i<4;++i){D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=256;desc.MipLevels=desc.ArraySize=1;
        desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&sources[i]))&&SUCCEEDED(device->CreateRenderTargetView(sources[i].Get(),nullptr,&source_targets[i])),"Quad mapping source");
        desc.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;
        check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&submitted[i])),"Quad mapping submitted image");
        targets[i]={submitted[i].Get(),0,0,0,256,256,uint64_t(1000+i)};
        metadata[i]={};metadata[i].version=CHEEKY_QUAD_VIEWS_VERSION;metadata[i].size=sizeof(metadata[i]);
        metadata[i].dlss_view=300+i;metadata[i].registration_generation=40+i;
        metadata[i].context=context.Get();metadata[i].texture=sources[i].Get();metadata[i].width=metadata[i].height=256;
    }
    constexpr unsigned evaluation_order[]{2,0,3,1};
    constexpr unsigned submitted_order[]{3,0,1,2};
    const float gray[]{.2f,.2f,.2f,1};
    const auto evaluate=[&]{for(auto i:evaluation_order){context->ClearRenderTargetView(source_targets[i].Get(),gray);mapper.observe(metadata[i]);}};
    const auto postprocess=[&](bool duplicate=false){const XrFovf fov{-.8f,.8f,.8f,-.8f};
        for(unsigned i=0;i<4;++i){const auto source=submitted_order[duplicate&&i==1?0:i];
            cheeky::quad_capture::Input input;input.texture11=sources[source].Get();input.format=DXGI_FORMAT_R16G16B16A16_FLOAT;input.image.imageRect.extent={256,256};
            check(convert.record(i%2,input,input,fov,fov,submitted[i].Get()),"Quad mapping postprocessing pass");}
        ComPtr<ID3D11CommandList> commands;check(SUCCEEDED(convert.deferred->FinishCommandList(FALSE,&commands)),"Quad mapping finish conversion");
        context->ExecuteCommandList(commands.Get(),TRUE);context->Flush();};
    for(unsigned frame=1;frame<80&&mapper.snapshot().status!=2;++frame){evaluate();postprocess();
        const auto token=mapper.capture(targets,frame);mapper.complete(token,true);Sleep(1);}
    auto result=mapper.snapshot();
    if(result.status!=2){std::cerr<<"Quad proof status="<<result.status<<" captures="<<result.captures<<" rejected="<<result.rejected<<" confirmations="<<result.confirmations<<" revision="<<result.revision<<'\n';
        for(const auto& source:result.sources)if(source.dlss_view)std::cerr<<"source "<<source.number<<" id="<<source.dlss_view<<" generation="<<source.registration_generation<<" size="<<source.width<<'x'<<source.height<<'\n';}
    check(result.status==2&&result.confirmations>=2,"Four-view pixel proof never acquired");
    for(unsigned i=0;i<4;++i)check(result.views[i].verified&&result.views[i].dlss_view==metadata[submitted_order[i]].dlss_view&&
        result.views[i].registration_generation==metadata[submitted_order[i]].registration_generation,"Quad roles inferred from evaluation order instead of submitted pixels");
    const auto capture_count=result.captures;evaluate();postprocess();
    check(mapper.capture(targets,100)==0&&mapper.snapshot().captures==capture_count,"Verified quad mapping kept capturing/stamping");
    const auto read=[&](ID3D11Texture2D* texture){D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);desc.BindFlags=desc.MiscFlags=0;
        desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;
        check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging)),"Quad number readback");context->CopyResource(staging.Get(),texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"Quad number map");
        std::vector<unsigned char> data(mapped.RowPitch*256);std::memcpy(data.data(),mapped.pData,data.size());context->Unmap(staging.Get(),0);
        return std::pair{std::move(data),mapped.RowPitch};};
    auto clean=read(sources[0].Get());
    check(std::abs(cheeky::foveated_dlss::calibration_decode(clean.first.data()+12*clean.second+12*8,DXGI_FORMAT_R16G16B16A16_FLOAT).r-.2f)<.01f,
        "Automatic source markers did not stop after verification");
    mapper.configure(false,true);evaluate();result=mapper.snapshot();
    check(!result.manual_mode&&result.show_numbers&&result.status==2,"Showing numbers changed automatic assignment mode");
    auto numbered=read(sources[0].Get());unsigned left{},right{},min_y=256,max_y{};
    for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x){const auto p=cheeky::foveated_dlss::calibration_decode(numbered.first.data()+y*numbered.second+x*8,DXGI_FORMAT_R16G16B16A16_FLOAT);
        if(p.r>.9f&&(x<64||x>=192)){left+=x<64;right+=x>=192;min_y=(std::min)(min_y,y);max_y=(std::max)(max_y,y);}}
    check(left>100&&right>100&&max_y-min_y>128,"Repeated peripheral numbers do not cover the area outside the center inset");
    mapper.configure(true,false);result=mapper.snapshot();
    check(result.manual_mode&&!result.show_numbers&&result.status==1,"Manual mode enabled labels or reused automatic proof");
    const auto source_for=[&](uint64_t id){for(const auto& source:mapper.snapshot().sources)if(source.dlss_view==id)return source;return CheekyQuadSourceV1{};};
    for(unsigned i=0;i<4;++i){const auto source=source_for(metadata[i].dlss_view);
        check(mapper.assign(source.number,i+1,source.dlss_view,source.registration_generation),"Manual quad assignment rejected");}
    result=mapper.snapshot();check(result.status==2&&result.manual_mode,"Manual quad mapping incomplete");
    for(unsigned i=0;i<4;++i)check(result.views[i].dlss_view==metadata[i].dlss_view&&!result.views[i].verified,"Manual assignments were overridden or called pixel-verified");
    const auto first=source_for(metadata[0].dlss_view),second=source_for(metadata[1].dlss_view);
    check(!mapper.assign(second.number,1,second.dlss_view,second.registration_generation),"Duplicate manual role accepted");
    check(!mapper.assign(first.number,0,first.dlss_view,first.registration_generation+1),"Stale dropdown generation accepted");
    ++metadata[0].registration_generation;mapper.observe(metadata[0]);result=mapper.snapshot();
    check(result.status==1&&std::all_of(std::begin(result.sources),std::end(result.sources),[](const auto& source){return source.manual_role==0;}),"Recreated handles inherited manual roles");
    mapper.configure(false,false);mapper.reset();
    for(unsigned frame=200;frame<215;++frame){evaluate();postprocess(true);const auto token=mapper.capture(targets,frame);mapper.complete(token,true);Sleep(1);}
    evaluate();check(mapper.snapshot().status!=2&&mapper.snapshot().rejected>0,"Ambiguous/duplicate submitted source became a four-view mapping");
    context->ClearState();context->Flush();
    if(errors)for(UINT64 i=0;i<errors->GetNumStoredMessages();++i){SIZE_T size{};errors->GetMessage(i,nullptr,&size);std::vector<char> storage(size);
        auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());errors->GetMessage(i,message,&size);
        if(message->Severity<=D3D11_MESSAGE_SEVERITY_ERROR){std::cerr<<message->pDescription<<'\n';check(false,"Quad role mapping D3D11 debug error");}}
    std::cout<<"PASS quad role pixel proof through FP16->SRGB postprocessing, shuffled handles, manual bijection, independent repeated numbers, generation invalidation and ambiguous rejection (WARP)\n";
}
}
