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
