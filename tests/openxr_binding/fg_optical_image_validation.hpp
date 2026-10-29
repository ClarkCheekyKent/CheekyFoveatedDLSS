#pragma once
// Deterministic shader checks do not depend on what vectors a driver estimates.
// A mutually consistent but incorrect field over a stationary image must lose
// to image evidence; a correct translated field must survive that same check.
inline void fg_optical_image_validation(){
    using namespace cheeky::fg;
    constexpr unsigned edge=64,grid=4,cells=edge/grid;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"Image validation WARP device");
    auto texture=[&](DXGI_FORMAT format,unsigned size,const void* pixels,unsigned stride,bool writable=false){
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=size;d.ArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=format;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|(writable?D3D11_BIND_UNORDERED_ACCESS:0);
        D3D11_SUBRESOURCE_DATA data{pixels,size*stride};ComPtr<ID3D11Texture2D> out;
        check(SUCCEEDED(device->CreateTexture2D(&d,pixels?&data:nullptr,&out)),"Image validation texture");return out;
    };
    auto color=[](float x,float y){return std::array<float,4>{
        .5f+.4f*std::sin(x*.41f+y*.13f),.5f+.4f*std::cos(x*.23f-y*.31f),.5f+.4f*std::sin(x*.17f+y*.37f),1};};
    std::vector<std::array<float,4>> old(edge*edge),now(edge*edge);
    for(unsigned y=0;y<edge;++y)for(unsigned x=0;x<edge;++x)old[y*edge+x]=color(float(x),float(y));
    auto a=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,edge,old.data(),16),b=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,edge,old.data(),16);
    std::vector<std::array<short,2>> forward(cells*cells,{12*32,0}),backward(cells*cells,{-12*32,0});
    std::vector<unsigned char> cost(cells*cells,0);std::vector<float> depth(edge*edge,.5f);
    std::vector<std::array<float,4>> cacheForward(cells*cells,{12*32,0,0,0}),cacheBackward(cells*cells,{-12*32,0,0,0});
    auto f=texture(DXGI_FORMAT_R16G16_SINT,cells,forward.data(),4),r=texture(DXGI_FORMAT_R16G16_SINT,cells,backward.data(),4),
        fc=texture(DXGI_FORMAT_R8_UINT,cells,cost.data(),1),rc=texture(DXGI_FORMAT_R8_UINT,cells,cost.data(),1),
        z=texture(DXGI_FORMAT_R32_FLOAT,edge,depth.data(),4),cf=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,cells,cacheForward.data(),16),
        cr=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,cells,cacheBackward.data(),16),out=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,edge,nullptr,16,true);
    ID3D11Texture2D* resources[]{a.Get(),b.Get(),f.Get(),r.Get(),fc.Get(),rc.Get(),z.Get(),cf.Get(),cr.Get()};
    ComPtr<ID3D11ShaderResourceView> views[9];ID3D11ShaderResourceView* srvs[9]{};
    for(unsigned i=0;i<9;++i){check(SUCCEEDED(device->CreateShaderResourceView(resources[i],nullptr,&views[i])),"Image validation SRV");srvs[i]=views[i].Get();}
    ComPtr<ID3D11UnorderedAccessView> target;check(SUCCEEDED(device->CreateUnorderedAccessView(out.Get(),nullptr,&target)),"Image validation UAV");
    ComPtr<ID3DBlob> code;ComPtr<ID3D11ComputeShader> interpolate,prepare;
    check(optical::compile("Interpolate","cs_5_0",code)&&SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&interpolate)),"Image validation shader");
    code.Reset();check(optical::compile("Prepare","cs_5_0",code)&&SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&prepare)),"RGB preparation shader");
    code.Reset();check(optical::compile("Interpolate","cs_5_1",code),"D3D12 optical shader compiles");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;check(SUCCEEDED(device->CreateSamplerState(&sd,&sampler)),"Image validation sampler");
    D3D11_TEXTURE2D_DESC stagingDesc{};out->GetDesc(&stagingDesc);stagingDesc.Usage=D3D11_USAGE_STAGING;stagingDesc.BindFlags=0;stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;check(SUCCEEDED(device->CreateTexture2D(&stagingDesc,nullptr,&staging)),"Image validation staging");
    OpticalConstants c;c.sizes[0]=c.sizes[1]=c.sizes[2]=c.sizes[3]=float(edge);
    auto dispatch=[&](ID3D11ComputeShader* shader){
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(c);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;D3D11_SUBRESOURCE_DATA data{&c};
        ComPtr<ID3D11Buffer> constants;check(SUCCEEDED(device->CreateBuffer(&bd,&data,&constants)),"Image validation constants");
        auto* cb=constants.Get();auto* s=sampler.Get();auto* uav=target.Get();
        context->CSSetShader(shader,nullptr,0);context->CSSetConstantBuffers(0,1,&cb);context->CSSetSamplers(0,1,&s);
        context->CSSetShaderResources(0,9,srvs);context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);context->Dispatch(edge/8,edge/8,1);optical::Session11::unbind(context.Get());
        context->CopyResource(staging.Get(),out.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"Image validation readback");
        std::vector<std::array<float,4>> pixels(edge*edge);
        for(unsigned y=0;y<edge;++y)memcpy(pixels.data()+y*edge,static_cast<const char*>(mapped.pData)+y*mapped.RowPitch,edge*16);
        context->Unmap(staging.Get(),0);return pixels;
    };
    unsigned cases{};
    for(bool moving:{false,true})for(bool hdr:{false,true}){
        const float exposure=hdr?12.f:1.f;
        for(unsigned y=0;y<edge;++y)for(unsigned x=0;x<edge;++x){old[y*edge+x]=color(float(x),float(y));now[y*edge+x]=color(float(x)-(moving?12:0),float(y));
            for(unsigned channel=0;channel<3;++channel){old[y*edge+x][channel]*=exposure;now[y*edge+x][channel]*=exposure;}}
        context->UpdateSubresource(a.Get(),0,nullptr,old.data(),edge*16,0);context->UpdateSubresource(b.Get(),0,nullptr,now.data(),edge*16,0);
        for(unsigned mode=0;mode<4;++mode)for(bool guided:{false,true})for(bool cached:{false,true})for(float t:{0.f,.25f,.5f,.75f,1.f}){
            if(cached&&mode<2)continue;
            c.texel[3]=float(mode)+(cached?4.f:0.f);c.flags[0]=t;c.flags[2]=guided?1.f:0.f;
            double errors[3]{};
            for(unsigned validation=0;validation<3;++validation){c.texel[1]=float(validation*4);const auto pixels=dispatch(interpolate.Get());
                for(unsigned y=16;y<edge-16;++y)for(unsigned x=16;x<edge-16;++x){const auto expected=color(float(x)-(moving?12*t:0),float(y));
                    for(unsigned channel=0;channel<3;++channel){check(std::isfinite(pixels[y*edge+x][channel]),"Validation output must remain finite");
                        errors[validation]+=std::abs(pixels[y*edge+x][channel]/exposure-expected[channel]);}}
                errors[validation]/=(edge-32)*(edge-32)*3;
            }
            if(moving||t==0||t==1)check(errors[1]<.002&&errors[2]<.002,"Image validation must retain correct motion and exact endpoints");
            else check(errors[1]<errors[0]*.25&&errors[2]<errors[0]*.25,"Image evidence must reject mutually consistent but incorrect motion");
            ++cases;
        }
    }
    // A good trajectory with an expensive driver match distinguishes image-only
    // validation from the combined option, which deliberately retains that cost.
    std::fill(cost.begin(),cost.end(),255);context->UpdateSubresource(fc.Get(),0,nullptr,cost.data(),cells,0);
    context->UpdateSubresource(rc.Get(),0,nullptr,cost.data(),cells,0);
    c.texel[3]=2;c.flags[0]=.5f;c.flags[2]=0;double costErrors[3]{};
    for(unsigned validation=0;validation<3;++validation){c.texel[1]=float(validation*4);const auto pixels=dispatch(interpolate.Get());
        for(unsigned y=16;y<edge-16;++y)for(unsigned x=16;x<edge-16;++x){const auto expected=color(float(x)-6,float(y));
            for(unsigned channel=0;channel<3;++channel)costErrors[validation]+=std::abs(pixels[y*edge+x][channel]/12-expected[channel]);}
    }
    for(auto& error:costErrors)error/=(edge-32)*(edge-32)*3;
    std::cout<<"High-cost motion MAE: default="<<costErrors[0]<<" image="<<costErrors[1]<<" combined="<<costErrors[2]<<"\n";
    check(costErrors[1]<.002&&costErrors[0]>.02&&std::abs(costErrors[0]-costErrors[2])<.001,
        "Image-only validation replaces matching cost; combined retains the method's confidence");
    // Preparation uses the same hue-preserving HDR mapping for both endpoints.
    for(auto& p:old)p={8,2,.5f,1};context->UpdateSubresource(a.Get(),0,nullptr,old.data(),edge*16,0);
    c.texel[1]=0;const auto rgb=dispatch(prepare.Get())[edge*32+32];
    check(std::abs(rgb[0]-8.f/9)<.0001&&std::abs(rgb[1]-2.f/9)<.0001&&std::abs(rgb[2]-.5f/9)<.0001,"RGB input bounds highlights while preserving channel ratios");
    std::cout<<"PASS optical image validation: "<<cases<<" SDR/HDR, interpolation, depth, cache and phase cases; RGB preprocessing\n";
}
