#pragma once
// Reproduce confidence-dependent camera ghosts with actual shader pixels.
// Bidirectional is a control: shared pose/vector faults are distinct from the
// confidence shader's crossfade with an unwarped current endpoint.
inline void fg_confidence_motion_validation(){
    using namespace cheeky::fg;using namespace fg_parity;
    using shader11_fixture::device;using shader11_fixture::context;
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"Confidence WARP device");
    D3D12 gpu;FocusPipeline pipeline;pipeline.color_format=DXGI_FORMAT_R8G8B8A8_UNORM;
    check(pipeline.initialize(gpu.device.Get()),"Confidence D3D12 pipeline");
    std::array<std::array<Frame,2>,2> translated;
    for(unsigned eye=0;eye<2;++eye)for(unsigned f=0;f<2;++f){auto& frame=translated[eye][f];
        const int previous=eye?32:16,current=eye?16:32,start=f?current:previous;
        for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){const auto i=y*size+x;
            frame.color[i]=rgba(x>=unsigned(start)&&x<unsigned(start+8)?255:0,0,0);
            // Eye 0 has 50% depth confidence; eye 1 rejects previous depth.
            frame.depth[i]=f?.5f:(eye?.53f:.51f);
            if(f)frame.motion[i*2]=float(previous-current);
        }
    }
    const unsigned methods[]{cheeky_fg_dlss_vectors_bidirectional,cheeky_fg_dlss_vectors_confidence,cheeky_fg_dlss_vectors_fast};
    for(const auto method:methods){
        const auto native=generate11(translated,method,cheeky_fg_previous,false);
        for(unsigned eye=0;eye<2;++eye){const auto transported=generate12(gpu,pipeline,eye,translated[eye],method,cheeky_fg_previous,false);
            for(unsigned y=8;y<size-8;++y)for(unsigned x=0;x<size;++x){const auto i=y*size+x;
                const unsigned expected=x>=24&&x<32?255:0;
                for(const auto* image:{&native[eye],&transported}){
                    const unsigned red=(*image)[i]&255;
                    check(std::abs(int(red)-int(expected))<=1,"Partial/rejected depth must keep a single warped image, without an endpoint ghost");
                }
            }
        }
    }
    auto prior=shader11_fixture::eye_views(XrSwapchain(1),XrSwapchain(2)),later=prior;
    constexpr float yaw=.15f,pitch=-.07f;
    const float cy=std::cos(yaw*.5f),sy=std::sin(yaw*.5f),cp=std::cos(pitch*.5f),sp=std::sin(pitch*.5f);
    for(auto& view:later)view.pose.orientation={cy*sp,sy*cp,-sy*sp,cy*cp};
    auto transform=[](const Matrix& matrix,const std::array<float,4>& p){
        std::array<float,4> out{};
        for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)out[r]+=matrix.m[r*4+c]*p[c];return out;
    };
    auto render=[&](const XrCompositionLayerProjectionView& view,const XrCompositionLayerProjectionView* previous,float depth_bias){
        Frame frame;const auto world=world_from_view(view.pose);Matrix inverse_world;
        check(inverse(world,inverse_world),"Confidence camera inverse");
        const auto projection_view=multiply(projection(view.fov),inverse_world);
        Matrix previous_projection_view=projection_view;
        if(previous){Matrix previous_view;check(inverse(world_from_view(previous->pose),previous_view),"Confidence previous camera inverse");
            previous_projection_view=multiply(projection(previous->fov),previous_view);}
        for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){const float u=(x+.5f)/size,v=(y+.5f)/size;
            const auto ray=transform(world,{std::lerp(std::tan(view.fov.angleLeft),std::tan(view.fov.angleRight),u),
                std::lerp(std::tan(view.fov.angleUp),std::tan(view.fov.angleDown),v),-1,0});
            const float distance=-2/ray[2];const std::array<float,4> point{distance*ray[0],distance*ray[1],-2,1};
            const auto i=y*size+x;
            frame.color[i]=rgba(unsigned(127+100*std::sin(point[0]*7)),unsigned(127+100*std::sin(point[1]*5)),unsigned(127+100*std::sin((point[0]+point[1])*6)));
            const auto clip=transform(projection_view,point);frame.depth[i]=clip[2]/clip[3]+depth_bias;
            if(previous){const auto p=transform(previous_projection_view,point);
                frame.motion[i*2]=(p[0]/p[3]*.5f+.5f-u)*size;
                frame.motion[i*2+1]=(.5f-p[1]/p[3]*.5f-v)*size;}
        }
        return frame;
    };
    std::array<std::array<Frame,2>,2> rotated;
    const auto mid=BufferedPresenter::midpoint(prior,later,.5f);
    std::array<Frame,2> expected;
    for(unsigned eye=0;eye<2;++eye){
        rotated[eye][0]=render(prior[eye],nullptr,eye?.03f:.01f);
        rotated[eye][1]=render(later[eye],&prior[eye],0);expected[eye]=render(mid[eye],nullptr,0);
    }
    for(const auto method:methods){
        const auto native=generate11(rotated,method,cheeky_fg_previous,false,.5f,&prior,&later);
        for(unsigned eye=0;eye<2;++eye){const auto cam=camera(later[eye],&prior[eye]);
            const auto transported=generate12(gpu,pipeline,eye,rotated[eye],method,cheeky_fg_previous,false,.5f,&cam);
            for(const auto* image:{&native[eye],&transported}){double error{},repeat{};unsigned count{};
                for(unsigned y=12;y<size-12;++y)for(unsigned x=12;x<size-12;++x){const auto i=y*size+x;
                    for(unsigned c=0;c<3;++c){
                        const int target=int((expected[eye].color[i]>>(c*8))&255);
                        error+=std::abs(int(((*image)[i]>>(c*8))&255)-target);
                        repeat+=std::abs(int((rotated[eye][1].color[i]>>(c*8))&255)-target);++count;
                    }
                }
                error/=count;repeat/=count;
                std::cout<<"Head-motion method="<<method<<" eye="<<eye<<" MAE="<<error<<" current_endpoint="<<repeat<<'\n';
                check(error<6&&error<repeat*.65,"Head rotation with uncertain depth must follow the intermediate view, not the current endpoint");
            }
        }
    }
    device.Reset();context.Reset();
    std::cout<<"PASS confidence motion: partial/zero depth confidence, no doubled edge, yaw+pitch, regular/fast/bidirectional, D3D11/D3D12\n";
}

// Exercise the production worker, including the crop-history gate that a
// direct shader fixture bypasses. Current and previous colors differ so a
// false history rejection cannot pass by showing only a sharp current frame.
inline void fg_gaze_motion_validation(){
    using namespace cheeky::fg;using namespace fg_parity;
    D3D12 gpu;
    std::vector<std::uint32_t> a(size*size),b(size*size),jump(size*size);
    std::vector<float> old_depth(size*size),next_depth(size*size),jump_depth(size*size);
    std::vector<float> still(size*size*2),moving(size*size*2),jump_motion(size*size*2);
    for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){const auto i=y*size+x;
        a[i]=rgba(x>=16&&x<24?255:0,0,0);b[i]=rgba(0,0,x>=32&&x<40?255:0);
        jump[i]=rgba(0,0,x>=48&&x<56?255:0);
        old_depth[i]=.15f+.005f*x+.003f*y;
        next_depth[i]=.15f+.005f*(int(x)-16)+.003f*y;
        jump_depth[i]=.15f+.005f*(int(x)-8)+.003f*y;
        moving[i*2]=-16;jump_motion[i*2]=-8;
    }
    auto old_color=gpu.texture(DXGI_FORMAT_R8G8B8A8_UNORM,a.data(),4),now_color=gpu.texture(DXGI_FORMAT_R8G8B8A8_UNORM,b.data(),4),jump_color=gpu.texture(DXGI_FORMAT_R8G8B8A8_UNORM,jump.data(),4);
    auto z0=gpu.texture(DXGI_FORMAT_R32_FLOAT,old_depth.data(),4),z1=gpu.texture(DXGI_FORMAT_R32_FLOAT,next_depth.data(),4),z2=gpu.texture(DXGI_FORMAT_R32_FLOAT,jump_depth.data(),4);
    auto m0=gpu.texture(DXGI_FORMAT_R32G32_FLOAT,still.data(),8),m1=gpu.texture(DXGI_FORMAT_R32G32_FLOAT,moving.data(),8),m2=gpu.texture(DXGI_FORMAT_R32G32_FLOAT,jump_motion.data(),8);gpu.execute();
    auto views=shader11_fixture::eye_views(XrSwapchain(1),XrSwapchain(2));std::array<Camera,2> cameras;
    for(unsigned eye=0;eye<2;++eye)cameras[eye]=camera(views[eye],&views[eye]);
    for(unsigned method:{cheeky_fg_dlss_vectors_bidirectional,cheeky_fg_dlss_vectors_confidence,cheeky_fg_dlss_vectors_fast})for(unsigned scenario=0;scenario<3;++scenario){
        Worker worker;check(worker.initialize(gpu.device.Get(),gpu.queue.Get(),size,size,DXGI_FORMAT_R8G8B8A8_UNORM,false),worker.diagnostic);
        CheekyFgPair guides;guides.producer_queue=gpu.queue.Get();
        for(unsigned eye=0;eye<2;++eye){auto& g=guides.eyes[eye];g.width=g.height=g.depth_width=g.depth_height=g.motion_width=g.motion_height=size;
            g.center_method=method;g.peripheral_method=cheeky_fg_previous;g.focus_width=g.focus_height=.5f;
            const unsigned x=scenario?0:eye?4:8,y=eye?20:16;
            g.focus_x=(float(x)-16)/size;g.focus_y=(float(y)-16)/size;
            g.resolution_percent=g.peripheral_percent=100;g.depth=z0.Get();g.motion=m0.Get();g.reset=1;g.configuration=1;
        }
        std::array<ID3D12Resource*,2> before{old_color.Get(),old_color.Get()},warm=before,out;
        check(worker.generate(guides,warm,before,cameras,out),worker.diagnostic);
        const auto saved0=worker.focus.previous_center_depth_rect[0],saved1=worker.focus.previous_center_depth_rect[1];
        for(unsigned eye=0;eye<2;++eye){auto& g=guides.eyes[eye];
            const unsigned x=scenario?32:eye?16:20,y=eye?12:8;
            g.focus_x=(float(x)-16)/size;g.focus_y=(float(y)-16)/size;
            g.depth=scenario?z2.Get():z1.Get();g.motion=scenario?m2.Get():m1.Get();g.reset=0;
            if(scenario==2)worker.focus.center_history[eye]=false;
        }
        std::array<ID3D12Resource*,2> current{scenario?jump_color.Get():now_color.Get(),scenario?jump_color.Get():now_color.Get()};
        for(unsigned phase=1;phase<=2;++phase){const float t=float(phase)/3;
            check(worker.generate(guides,current,before,cameras,out,nullptr,nullptr,nullptr,nullptr,2,phase),worker.diagnostic);
            for(unsigned eye=0;eye<2;++eye){const auto pixels=gpu.read(out[eye]);
                const unsigned x=scenario?unsigned(std::round(48-8*(1-t)+3)):unsigned(std::round(16+16*t+3));
                const auto pixel=pixels[24*size+x];const int red=int(pixel&255),blue=int((pixel>>16)&255);
                const int expected_red=scenario?0:int(std::round(255*(1-t))),expected_blue=scenario?255:int(std::round(255*t));
                if(std::abs(red-expected_red)>2||std::abs(blue-expected_blue)>2)
                    std::cout<<"Gaze method="<<method<<" scenario="<<scenario<<" phase="<<phase<<" eye="<<eye<<" red/blue="<<red<<'/'<<blue<<" expected="<<expected_red<<'/'<<expected_blue<<'\n';
                check(std::abs(red-expected_red)<=2&&std::abs(blue-expected_blue)<=2,
                    "Moving gaze must preserve correctly mapped history; uncovered or unavailable history must use phase-warped current color");
            }
            if(phase==1)check(worker.focus.previous_center_depth_rect[0].x==saved0.x&&worker.focus.previous_center_depth_rect[1].y==saved1.y,
                "Every generated phase keeps the previous native depth crop immutable");
        }
    }
    std::cout<<"PASS moving-gaze worker: bidirectional/regular/fast, both eyes/axes, old crop depth mapping, uncovered and missing history, both 3x phases\n";
}
