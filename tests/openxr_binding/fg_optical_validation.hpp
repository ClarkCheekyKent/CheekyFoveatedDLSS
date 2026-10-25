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
// The presenter's in-between orientation for generated frames.
inline XrQuaternionf midpoint_pose(const XrCompositionLayerProjectionView& a,const XrCompositionLayerProjectionView& b,float t){
    auto x=a.pose.orientation,y=b.pose.orientation;if(x.x*y.x+x.y*y.y+x.z*y.z+x.w*y.w<0)y={-y.x,-y.y,-y.z,-y.w};
    XrQuaternionf q{x.x*(1-t)+y.x*t,x.y*(1-t)+y.y*t,x.z*(1-t)+y.z*t,x.w*(1-t)+y.w*t};
    const float n=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);return {q.x/n,q.y/n,q.z/n,q.w/n};
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
    // A depth ramp exercises depth arbitration. Both trajectories of one
    // surface meet at the same current-frame depth, so it must not reject them.
    std::vector<float> depth(edge*edge);for(unsigned y=0;y<edge;++y)for(unsigned x=0;x<edge;++x)depth[y*edge+x]=.3f+.4f*x/edge;
    const std::vector<float> motion(edge*edge*2);
    ComPtr<ID3D11Texture2D> depth11;ComPtr<ID3D11ShaderResourceView> depth_view;
    {D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=edge;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
     D3D11_SUBRESOURCE_DATA data{depth.data(),edge*4,0};check(SUCCEEDED(device11->CreateTexture2D(&d,&data,&depth11))&&SUCCEEDED(device11->CreateShaderResourceView(depth11.Get(),nullptr,&depth_view)),"Optical depth texture");}
    auto views=shader11_fixture::eye_views(XrSwapchain(1),XrSwapchain(2));for(auto& v:views)v.subImage.imageRect.extent={edge,edge};
    for(unsigned method:{cheeky_fg_optical_flow,cheeky_fg_optical_depth})for(unsigned quality=0;quality<3;++quality){
        Backend11 backend;check(backend.initialize(device11.Get(),7,nullptr),backend.diagnostic);
        std::array<Image,2> previous,current;
        std::array<std::shared_ptr<Backend11::Guides>,2> guides;
        for(unsigned eye=0;eye<2;++eye){previous[eye]=backend.copy(a.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);current[eye]=backend.copy(b.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);
            guides[eye]=std::make_shared<Backend11::Guides>();auto& g=guides[eye]->source.meta;g.width=g.height=edge;g.center_method=g.peripheral_method=method;
            if(method==cheeky_fg_optical_depth)guides[eye]->depth_srv=depth_view;
            g.focus_width=.55f;g.focus_height=.45f;g.resolution_percent=100;g.peripheral_percent=75;
            g.optical.center_preset=quality;g.optical.peripheral_preset=2-quality;g.optical.center_percent=100;g.optical.peripheral_percent=50;g.optical.center_grid=1u<<quality;g.optical.peripheral_grid=4u>>quality;}
        for(unsigned multiplier=2;multiplier<=4;++multiplier)for(unsigned phase=1;phase<multiplier;++phase){
            std::array<Image,2> output;const float t=float(phase)/multiplier;
            check(backend.generate(previous,current,guides,guides,views,views,output,false,t),backend.diagnostic);
            validate(read11(device11.Get(),context.Get(),output[0]->texture11.Get()),t,"D3D11");
        }
    }
    {
        // Occlusion: a near textured square moves 12 px right over a static far
        // background. Report each variant's error around the square's edges.
        auto inside=[](float x,float y,float shift){return x>=96+shift&&x<160+shift&&y>=96&&y<160;};
        auto pixel=[&](float x,float y,float shift){return inside(x,y,shift)?scene(x-shift+1000,y):scene(x,y);};
        std::vector<std::uint32_t> before(edge*edge),after(edge*edge);std::vector<float> square(edge*edge);
        for(unsigned y=0;y<edge;++y)for(unsigned x=0;x<edge;++x){before[y*edge+x]=pixel(float(x),float(y),0);after[y*edge+x]=pixel(float(x),float(y),12);
            square[y*edge+x]=inside(float(x),float(y),12)?.2f:.9f;}
        auto c0=texture11(before),c1=texture11(after);
        ComPtr<ID3D11Texture2D> square11;ComPtr<ID3D11ShaderResourceView> square_view;
        {D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=edge;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
         D3D11_SUBRESOURCE_DATA data{square.data(),edge*4,0};check(SUCCEEDED(device11->CreateTexture2D(&d,&data,&square11))&&SUCCEEDED(device11->CreateShaderResourceView(square11.Get(),nullptr,&square_view)),"Optical occlusion depth");}
        double errors[2]{};
        for(unsigned variant=0;variant<2;++variant){
            Backend11 backend;check(backend.initialize(device11.Get(),7,nullptr),backend.diagnostic);
            std::array<Image,2> previous,current,output;std::array<std::shared_ptr<Backend11::Guides>,2> guides;
            for(unsigned eye=0;eye<2;++eye){previous[eye]=backend.copy(c0.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);current[eye]=backend.copy(c1.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);
                guides[eye]=std::make_shared<Backend11::Guides>();auto& g=guides[eye]->source.meta;g.width=g.height=edge;
                g.center_method=g.peripheral_method=variant?cheeky_fg_optical_depth:cheeky_fg_optical_flow;
                if(variant)guides[eye]->depth_srv=square_view;
                g.focus_width=g.focus_height=1;g.resolution_percent=100;g.optical.center_preset=0;g.optical.center_percent=100;}
            check(backend.generate(previous,current,guides,guides,views,views,output,false,.5f),backend.diagnostic);
            const auto image=read11(device11.Get(),context.Get(),output[0]->texture11.Get());unsigned count{};
            for(unsigned y=88;y<168;++y)for(unsigned x=84;x<180;++x){
                if(inside(float(x),float(y),6)==inside(float(x)+8,float(y),6)&&inside(float(x),float(y),6)==inside(float(x)-8,float(y),6))continue;
                const auto expected=pixel(float(x),float(y),6),actual=image[y*edge+x];
                for(unsigned c=0;c<3;++c){errors[variant]+=std::abs(int((actual>>(c*8))&255)-int((expected>>(c*8))&255));++count;}
            }
            errors[variant]/=count;
        }
        std::cout<<"Occlusion edge MAE: color-only="<<errors[0]<<" DLSS depth="<<errors[1]<<"\n";
        check(errors[1]<errors[0]*.97,"DLSS depth must reduce optical flow error at occlusion edges");
    }
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;check(SUCCEEDED(device11.As(&dxgi))&&SUCCEEDED(dxgi->GetAdapter(&adapter)),"Optical adapter");
    fg_parity::D3D12 gpu(adapter.Get());
    auto old12=gpu.texture(DXGI_FORMAT_R8G8B8A8_UNORM,old.data(),4,edge),now12=gpu.texture(DXGI_FORMAT_R8G8B8A8_UNORM,now.data(),4,edge);
    auto depth12=gpu.texture(DXGI_FORMAT_R32_FLOAT,depth.data(),4,edge),motion12=gpu.texture(DXGI_FORMAT_R32G32_FLOAT,motion.data(),8,edge);gpu.execute();
    for(unsigned method:{cheeky_fg_optical_flow,cheeky_fg_optical_depth})for(unsigned quality=0;quality<3;++quality){
        Worker worker;check(worker.initialize(gpu.device.Get(),gpu.queue.Get(),edge,edge,DXGI_FORMAT_R8G8B8A8_UNORM,false),worker.diagnostic);
        CheekyFgPair pair;pair.producer_queue=gpu.queue.Get();
        for(auto& g:pair.eyes){g.width=g.height=edge;g.center_method=g.peripheral_method=method;g.focus_width=.55f;g.focus_height=.45f;
            if(method==cheeky_fg_optical_depth){g.depth=depth12.Get();g.motion=motion12.Get();g.depth_width=g.depth_height=g.motion_width=g.motion_height=edge;g.motion_scale_x=g.motion_scale_y=1;}
            g.optical.center_preset=quality;g.optical.peripheral_preset=2-quality;g.optical.center_percent=100;g.optical.peripheral_percent=50;g.optical.center_grid=1u<<quality;g.optical.peripheral_grid=4u>>quality;g.peripheral_percent=75;g.configuration=method*4+quality+1;}
        std::array<Camera,2> cameras;for(unsigned eye=0;eye<2;++eye)cameras[eye]=camera(views[eye],&views[eye],false);
        std::array<ID3D12Resource*,2> previous{old12.Get(),old12.Get()},current{now12.Get(),now12.Get()},output;
        for(unsigned multiplier=2;multiplier<=4;++multiplier)for(unsigned phase=1;phase<multiplier;++phase){
            check(worker.generate(pair,current,previous,cameras,output,nullptr,nullptr,nullptr,nullptr,multiplier-1,phase),worker.diagnostic);
            validate(gpu.read(output[0]),float(phase)/multiplier,"D3D12");
            if(phase==multiplier-1){const auto batch=worker.batch_timings.snapshot();
                check(batch.valid&&batch.generated_count==multiplier-1&&batch.optical_regions==4,
                    "Optical batch timings retain all generated phases and four deferred regional dependencies");}
        }
    }
    {
        // Head rotation: a distant environment seen by a camera that yaws and
        // pitches between frames. The generated image must match the view
        // from the submitted in-between pose.
        constexpr float pi=3.14159265f;
        auto pose=[](float yaw,float pitch){const float cy=std::cos(yaw*.5f),sy=std::sin(yaw*.5f),cp=std::cos(pitch*.5f),sp=std::sin(pitch*.5f);
            return XrQuaternionf{cy*sp,sy*cp,-sy*sp,cy*cp};}; // yaw about +Y, then pitch about +X.
        auto render=[&](XrQuaternionf q){std::vector<std::uint32_t> out(edge*edge);
            for(unsigned y=0;y<edge;++y)for(unsigned x=0;x<edge;++x){
                float v[3]{std::lerp(-1.f,1.f,(x+.5f)/edge),std::lerp(1.f,-1.f,(y+.5f)/edge),-1.f};
                const float c[3]{q.y*v[2]-q.z*v[1],q.z*v[0]-q.x*v[2],q.x*v[1]-q.y*v[0]};
                const float u[3]{c[0]+q.w*v[0],c[1]+q.w*v[1],c[2]+q.w*v[2]};
                const float d[3]{v[0]+2*(q.y*u[2]-q.z*u[1]),v[1]+2*(q.z*u[0]-q.x*u[2]),v[2]+2*(q.x*u[1]-q.y*u[0])};
                const float lon=std::atan2(d[0],-d[2]),lat=std::atan2(d[1],std::sqrt(d[0]*d[0]+d[2]*d[2]));
                out[y*edge+x]=scene(lon*edge*2/pi+500,500-lat*edge*2/pi);}
            return out;};
        const auto q0=pose(0,0),q1=pose(4*pi/180,-2*pi/180);
        auto prior=views,later=views;
        for(unsigned eye=0;eye<2;++eye){for(auto* v:{&prior[eye],&later[eye]})v->fov={-pi/4,pi/4,pi/4,-pi/4};prior[eye].pose.orientation=q0;later[eye].pose.orientation=q1;}
        auto c0=texture11(render(q0)),c1=texture11(render(q1));
        Backend11 backend;check(backend.initialize(device11.Get(),7,nullptr),backend.diagnostic);
        std::array<Image,2> previous,current;std::array<std::shared_ptr<Backend11::Guides>,2> guides;
        for(unsigned eye=0;eye<2;++eye){previous[eye]=backend.copy(c0.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);current[eye]=backend.copy(c1.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);
            guides[eye]=std::make_shared<Backend11::Guides>();auto& g=guides[eye]->source.meta;g.width=g.height=edge;
            g.center_method=g.peripheral_method=cheeky_fg_optical_flow;g.focus_width=g.focus_height=1;g.resolution_percent=100;g.optical.center_percent=100;}
        for(const float t:{1.f/3,.5f,2.f/3}){
            std::array<Image,2> output;check(backend.generate(previous,current,guides,guides,prior,later,output,false,t),backend.diagnostic);
            const auto mid=midpoint_pose(prior[0],later[0],t);
            const auto expected=render(mid),actual=read11(device11.Get(),context.Get(),output[0]->texture11.Get());
            double error{},repeat{};unsigned count{};const auto& nearest=t<.5f?render(q0):render(q1);
            for(unsigned y=40;y<edge-40;++y)for(unsigned x=40;x<edge-40;++x)for(unsigned c=0;c<3;++c){
                error+=std::abs(int((actual[y*edge+x]>>(c*8))&255)-int((expected[y*edge+x]>>(c*8))&255));
                repeat+=std::abs(int((nearest[y*edge+x]>>(c*8))&255)-int((expected[y*edge+x]>>(c*8))&255));++count;}
            error/=count;repeat/=count;std::cout<<"Head rotation t="<<t<<" MAE="<<error<<" repeat="<<repeat<<"\n";
            check(error<6&&error<repeat*.3,"Optical flow must follow head rotation into the submitted in-between pose");
        }
    }
    std::cout<<"PASS NVIDIA Optical Flow: D3D11/D3D12, color-only and DLSS-depth variants, all presets and vector grids, independent regional scales, head rotation, all 2x/3x/4x phases\n";
}
// CPU cost of the D3D12 optical flow worker at a VR eye size, per phase.
inline void fg_optical_profile(){
    using namespace cheeky::fg;using namespace optical_fixture;
    ComPtr<ID3D11Device> device11;ComPtr<ID3D11DeviceContext> context;
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device11,nullptr,&context)),"Optical hardware device");
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;check(SUCCEEDED(device11.As(&dxgi))&&SUCCEEDED(dxgi->GetAdapter(&adapter)),"Optical adapter");
    constexpr unsigned size=2816;fg_parity::D3D12 gpu(adapter.Get());
    std::vector<std::uint32_t> a(size*size),b(size*size);
    for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){a[y*size+x]=scene(float(x),float(y));b[y*size+x]=scene(float(x)-20,float(y));}
    auto old12=gpu.texture(DXGI_FORMAT_R8G8B8A8_UNORM,a.data(),4,size),now12=gpu.texture(DXGI_FORMAT_R8G8B8A8_UNORM,b.data(),4,size);gpu.execute();
    const unsigned settings[][2]{{2,50},{1,50},{0,50},{2,100},{1,100}};
    for(const auto& setting:settings){
    Worker worker;check(worker.initialize(gpu.device.Get(),gpu.queue.Get(),size,size,DXGI_FORMAT_R8G8B8A8_UNORM,false),worker.diagnostic);
    auto views=shader11_fixture::eye_views(XrSwapchain(1),XrSwapchain(2));for(auto& v:views)v.subImage.imageRect.extent={int(size),int(size)};
    CheekyFgPair pair;pair.producer_queue=gpu.queue.Get();
    for(auto& g:pair.eyes){g.width=g.height=size;g.center_method=g.peripheral_method=4;g.focus_width=.55f;g.focus_height=.45f;
        g.optical.center_preset=setting[0];g.optical.peripheral_preset=2;g.optical.center_percent=setting[1];g.optical.peripheral_percent=25;g.peripheral_percent=25;g.configuration=1;}
    std::array<Camera,2> cameras;for(unsigned eye=0;eye<2;++eye)cameras[eye]=camera(views[eye],&views[eye],false);
    std::array<ID3D12Resource*,2> previous{old12.Get(),old12.Get()},current{now12.Get(),now12.Get()},output;
    double phase_ms[3]{};constexpr unsigned frames=60;
    for(unsigned frame=0;frame<frames+5;++frame)for(unsigned phase=1;phase<=2;++phase){
        const auto start=std::chrono::steady_clock::now();
        check(worker.generate(pair,current,previous,cameras,output,nullptr,nullptr,nullptr,nullptr,2,phase),worker.diagnostic);
        if(frame>=5)phase_ms[phase]+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if(phase==2)std::swap(previous,current);
    }
    std::cout<<"D3D12 optical worker CPU ms per generate (3x, 2816 eye, periphery 25% fast) center preset="<<setting[0]<<" "<<setting[1]<<"%: phase1="<<phase_ms[1]/frames<<" phase2="<<phase_ms[2]/frames<<"\n";
    }
}

// A fast head turn moves the foveated region to an eye edge or far from the
// previous one. The flow window must keep its size (its size is the driver
// session's size), and a one-frame full-eye request must not rebuild the
// session: each rebuild stalled generation for about two seconds.
inline void fg_optical_size_stability(){
    using namespace cheeky::fg;
    CheekyFgEye g;g.width=2000;g.height=1800;g.optical.center_percent=75;
    const CompactLayout none{};
    const float centered[]{.3f,.3f,.4f,.4f},edge_left[]{0,.6f,.4f,.4f},edge_right[]{.6f,0,.4f,.4f};
    const auto a=optical::constants(g,0,centered,none,none,800,720),b=optical::constants(g,0,edge_left,none,none,800,720),
        c=optical::constants(g,0,edge_right,none,none,800,720);
    check(a.sizes[0]==b.sizes[0]&&a.sizes[1]==b.sizes[1]&&a.sizes[0]==c.sizes[0]&&a.sizes[1]==c.sizes[1],"Optical flow window must keep its size at eye edges and across far jumps");
    for(const auto* k:{&a,&b,&c})check(k->flow_rect[0]>=0&&k->flow_rect[1]>=0&&k->flow_rect[0]+k->flow_rect[2]<=1.0001f&&k->flow_rect[1]+k->flow_rect[3]<=1.0001f,"Optical flow window must stay inside the eye");
    check(b.flow_rect[0]<=edge_left[0]&&b.flow_rect[0]+b.flow_rect[2]>=edge_left[0]+edge_left[2],"Optical flow window must still cover the region at an edge");
    optical::Session12 session;session.width=unsigned(a.sizes[0]);session.height=unsigned(a.sizes[1]);
    check(session.keep_size(unsigned(a.sizes[0]),unsigned(a.sizes[1]))&&session.keep_size(2000,1800)&&session.keep_size(unsigned(a.sizes[0]),unsigned(a.sizes[1])),
        "A transient full-eye flow request must keep the optical flow session");
    bool kept=true;for(unsigned i=0;i<100;++i)kept=session.keep_size(2000,1800);
    check(!kept,"A lasting flow size change must rebuild the optical flow session");
    std::cout<<"PASS optical flow session size stability (edge, far jump, transient full eye)\n";
}
