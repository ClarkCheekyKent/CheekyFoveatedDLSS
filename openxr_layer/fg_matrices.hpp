#pragma once
#include <openxr/openxr.h>
#include <algorithm>
#include <cmath>
#include <cstring>
namespace cheeky::fg {
struct Matrix {float m[16]{};};
inline Matrix identity() noexcept {Matrix x{};for(unsigned i=0;i<4;++i)x.m[i*4+i]=1;return x;}
inline Matrix multiply(Matrix a,Matrix b) noexcept {
    Matrix c{};for(unsigned r=0;r<4;++r)for(unsigned col=0;col<4;++col)
        for(unsigned k=0;k<4;++k)c.m[r*4+col]+=a.m[r*4+k]*b.m[k*4+col];return c;
}
inline bool inverse(Matrix a,Matrix& out) noexcept {
    out=identity();
    for(unsigned i=0;i<4;++i){unsigned pivot=i;
        for(unsigned r=i+1;r<4;++r)if(std::abs(a.m[r*4+i])>std::abs(a.m[pivot*4+i]))pivot=r;
        if(std::abs(a.m[pivot*4+i])<1e-7f)return false;
        for(unsigned c=0;c<4;++c){std::swap(a.m[pivot*4+c],a.m[i*4+c]);std::swap(out.m[pivot*4+c],out.m[i*4+c]);}
        float q=a.m[i*4+i];for(unsigned c=0;c<4;++c){a.m[i*4+c]/=q;out.m[i*4+c]/=q;}
        for(unsigned r=0;r<4;++r)if(r!=i){q=a.m[r*4+i];for(unsigned c=0;c<4;++c){a.m[r*4+c]-=q*a.m[i*4+c];out.m[r*4+c]-=q*out.m[i*4+c];}}
    }return true;
}
inline Matrix projection(XrFovf f,float z_near=0.05f,float z_far=10000.0f,bool inverted=false) noexcept {
    float l=std::tan(f.angleLeft),r=std::tan(f.angleRight),u=std::tan(f.angleUp),d=std::tan(f.angleDown);
    Matrix p{};p.m[0]=2/(r-l);p.m[2]=(r+l)/(r-l);
    p.m[5]=2/(u-d);p.m[6]=(u+d)/(u-d);
    p.m[10]=z_far/(z_near-z_far);p.m[11]=z_far*z_near/(z_near-z_far);
    if(inverted){p.m[10]=z_near/(z_far-z_near);p.m[11]=z_far*z_near/(z_far-z_near);}
    p.m[14]=-1;return p;
}
inline Matrix world_from_view(XrPosef pose) noexcept {
    float x=pose.orientation.x,y=pose.orientation.y,z=pose.orientation.z,w=pose.orientation.w;
    float d=x*x+y*y+z*z+w*w;if(d<1e-7f)return identity();
    float s=2/d;Matrix m=identity();
    m.m[0]=1-s*(y*y+z*z);m.m[1]=s*(x*y-z*w);m.m[2]=s*(x*z+y*w);
    m.m[4]=s*(x*y+z*w);m.m[5]=1-s*(x*x+z*z);m.m[6]=s*(y*z-x*w);
    m.m[8]=s*(x*z-y*w);m.m[9]=s*(y*z+x*w);m.m[10]=1-s*(x*x+y*y);
    m.m[3]=pose.position.x;m.m[7]=pose.position.y;m.m[11]=pose.position.z;return m;
}
struct Camera {
    Matrix view_to_clip{},clip_to_view{},clip_to_prev{},prev_to_clip{};
    float pos[3]{},right[3]{},up[3]{},forward[3]{};
    float fov{},aspect{};
    bool valid{};
};
inline Camera camera(const XrCompositionLayerProjectionView& current,
                     const XrCompositionLayerProjectionView* previous,bool inverted=false) noexcept {
    Camera out{};auto p=projection(current.fov,.05f,10000.f,inverted),cam=world_from_view(current.pose);
    Matrix view{},prev_clip{};if(!inverse(cam,view)||!inverse(p,out.clip_to_view))return out;
    out.view_to_clip=p;
    if(previous){auto previous_cam=world_from_view(previous->pose),previous_projection=projection(previous->fov,.05f,10000.f,inverted);
        Matrix previous_view{};if(!inverse(previous_cam,previous_view))return out;
        out.clip_to_prev=multiply(multiply(multiply(previous_projection,previous_view),cam),out.clip_to_view);
    }else out.clip_to_prev=identity();
    if(!inverse(out.clip_to_prev,out.prev_to_clip))return out;
    out.pos[0]=cam.m[3];out.pos[1]=cam.m[7];out.pos[2]=cam.m[11];
    for(unsigned i=0;i<3;++i){out.right[i]=cam.m[i*4];out.up[i]=cam.m[i*4+1];out.forward[i]=-cam.m[i*4+2];}
    const auto& f=current.fov;out.fov=f.angleUp-f.angleDown;
    out.aspect=(std::tan(f.angleRight)-std::tan(f.angleLeft))/(std::tan(f.angleUp)-std::tan(f.angleDown));
    out.valid=std::isfinite(out.aspect)&&out.aspect>0&&out.fov>0;return out;
}
}
