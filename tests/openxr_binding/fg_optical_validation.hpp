#pragma once
namespace optical_fixture {
using Microsoft::WRL::ComPtr;
constexpr unsigned edge=256;
inline std::uint32_t scene(float x,float y){
    const int ix=int(std::floor(x/8)),iy=int(std::floor(y/8));const float fx=x/8-ix,fy=y/8-iy;
    auto hash=[](int px,int py){unsigned h=unsigned(px)*0x9e3779b9u^unsigned(py)*0x85ebca6bu;h^=h>>16;h*=0x7feb352du;h^=h>>15;h*=0x846ca68bu;return h^(h>>16);};
    unsigned result=0xff000000;
    for(unsigned c=0;c<3;++c){auto value=[&](int px,int py){return float(32+((hash(px,py)>>(c*8))&191));};
        const float top=std::lerp(value(ix,iy),value(ix+1,iy),fx),bottom=std::lerp(value(ix,iy+1),value(ix+1,iy+1),fx);
        result|=unsigned(std::lerp(top,bottom,fy))<<(c*8);}
    return result;
}
inline std::vector<std::uint32_t> frame(float shift){
    std::vector<std::uint32_t> out(edge*edge);for(unsigned y=0;y<edge;++y)for(unsigned x=0;x<edge;++x)out[y*edge+x]=scene(float(x)-shift,float(y));return out;
}
inline void validate(const std::vector<std::uint32_t>& output,float fraction,const char* api){
    double error{},repeat{};unsigned count{};
    for(unsigned y=32;y<edge-32;y+=2)for(unsigned x=32;x<edge-32;x+=2){
        auto expected=scene(float(x)-12*fraction,float(y));const auto fallback=scene(float(x)-(fraction<.5f?0:12),float(y));
        const auto actual=output[y*edge+x];
        for(unsigned c=0;c<3;++c){error+=std::abs(int((actual>>(c*8))&255)-int((expected>>(c*8))&255));
            repeat+=std::abs(int((fallback>>(c*8))&255)-int((expected>>(c*8))&255));++count;}
    }
    error/=count;repeat/=count;std::cout<<api<<" t="<<fraction<<" MAE="<<error<<" repeat="<<repeat<<" actual="<<std::hex<<output[64*edge+64]<<" expected="<<scene(64-12*fraction,64)<<std::dec<<"\n";
    check(error<12&&error<repeat*.7,"Optical flow must move image content to the requested phase, outperforming repeated endpoints");
}
inline std::vector<std::uint32_t> read11(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* image){
    D3D11_TEXTURE2D_DESC d{};image->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"Optical staging");context->CopyResource(staging.Get(),image);
    D3D11_MAPPED_SUBRESOURCE data{};check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&data)),"Optical readback");
    std::vector<std::uint32_t> out(edge*edge);for(unsigned y=0;y<edge;++y)memcpy(out.data()+y*edge,static_cast<const char*>(data.pData)+y*data.RowPitch,edge*4);context->Unmap(staging.Get(),0);return out;
}
}
inline void fg_optical_validation(){
    using namespace cheeky::fg;using namespace optical_fixture;
    // Real OFA execution is required. This entry point deliberately fails on
    // unsupported hardware; the ordinary WARP suites keep testing fallback.
    ComPtr<ID3D11Device> device11;ComPtr<ID3D11DeviceContext> context;
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device11,nullptr,&context)),"Optical hardware device");
    const auto old=frame(0),now=frame(12);
    auto texture11=[&](const auto& pixels){D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=edge;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels.data(),edge*4,0};ComPtr<ID3D11Texture2D> t;check(SUCCEEDED(device11->CreateTexture2D(&d,&data,&t)),"Optical source texture");return t;};
    auto a=texture11(old),b=texture11(now);
    auto views=shader11_fixture::eye_views(XrSwapchain(1),XrSwapchain(2));for(auto& v:views)v.subImage.imageRect.extent={edge,edge};
    for(unsigned quality=0;quality<3;++quality){
        Backend11 backend;check(backend.initialize(device11.Get(),7,nullptr),backend.diagnostic);
        std::array<Image,2> previous,current;
        std::array<std::shared_ptr<Backend11::Guides>,2> guides;
        for(unsigned eye=0;eye<2;++eye){previous[eye]=backend.copy(a.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);current[eye]=backend.copy(b.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);
            guides[eye]=std::make_shared<Backend11::Guides>();auto& g=guides[eye]->source.meta;g.width=g.height=edge;g.center_method=g.peripheral_method=4;
            g.focus_width=.55f;g.focus_height=.45f;g.resolution_percent=100;g.peripheral_percent=75;
            g.optical.center_preset=quality;g.optical.peripheral_preset=2-quality;g.optical.center_percent=100;g.optical.peripheral_percent=50;}
        for(unsigned multiplier=2;multiplier<=4;++multiplier)for(unsigned phase=1;phase<multiplier;++phase){
            std::array<Image,2> output;const float t=float(phase)/multiplier;
            check(backend.generate(previous,current,guides,guides,views,views,output,false,t),backend.diagnostic);
            validate(read11(device11.Get(),context.Get(),output[0]->texture11.Get()),t,"D3D11");
        }
    }
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;check(SUCCEEDED(device11.As(&dxgi))&&SUCCEEDED(dxgi->GetAdapter(&adapter)),"Optical adapter");
    fg_parity::D3D12 gpu(adapter.Get());
    auto old12=gpu.texture(DXGI_FORMAT_R8G8B8A8_UNORM,old.data(),4,edge),now12=gpu.texture(DXGI_FORMAT_R8G8B8A8_UNORM,now.data(),4,edge);gpu.execute();
    for(unsigned quality=0;quality<3;++quality){
        Worker worker;check(worker.initialize(gpu.device.Get(),gpu.queue.Get(),edge,edge,DXGI_FORMAT_R8G8B8A8_UNORM,false),worker.diagnostic);
        CheekyFgPair pair;pair.producer_queue=gpu.queue.Get();
        for(auto& g:pair.eyes){g.width=g.height=edge;g.center_method=g.peripheral_method=4;g.focus_width=.55f;g.focus_height=.45f;
            g.optical.center_preset=quality;g.optical.peripheral_preset=2-quality;g.optical.center_percent=100;g.optical.peripheral_percent=50;g.peripheral_percent=75;g.configuration=quality+1;}
        std::array<Camera,2> cameras;for(unsigned eye=0;eye<2;++eye)cameras[eye]=camera(views[eye],&views[eye],false);
        std::array<ID3D12Resource*,2> previous{old12.Get(),old12.Get()},current{now12.Get(),now12.Get()},output;
        for(unsigned multiplier=2;multiplier<=4;++multiplier)for(unsigned phase=1;phase<multiplier;++phase){
            check(worker.generate(pair,current,previous,cameras,output,nullptr,nullptr,nullptr,nullptr,multiplier-1,phase),worker.diagnostic);
            validate(gpu.read(output[0]),float(phase)/multiplier,"D3D12");
        }
    }
    std::cout<<"PASS NVIDIA Optical Flow: D3D11/D3D12, all presets, independent regional scales, no depth/motion guides, all 2x/3x/4x phases\n";
}
