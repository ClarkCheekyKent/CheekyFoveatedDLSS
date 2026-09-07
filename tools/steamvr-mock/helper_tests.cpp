#include <Windows.h>
#include "EyeTrackingOutput.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace vr;
void DriverLog(const char*, ...) {}
struct Input : IVRDriverInput {
    VREyeTrackingData_t sample{};
    int updates{};
    EVRInputError CreateBooleanComponent(PropertyContainerHandle_t,const char*,VRInputComponentHandle_t*) override { return VRInputError_None; }
    EVRInputError UpdateBooleanComponent(VRInputComponentHandle_t,bool,double) override { return VRInputError_None; }
    EVRInputError CreateScalarComponent(PropertyContainerHandle_t,const char*,VRInputComponentHandle_t*,EVRScalarType,EVRScalarUnits) override { return VRInputError_None; }
    EVRInputError UpdateScalarComponent(VRInputComponentHandle_t,float,double) override { return VRInputError_None; }
    EVRInputError CreateHapticComponent(PropertyContainerHandle_t,const char*,VRInputComponentHandle_t*) override { return VRInputError_None; }
    EVRInputError CreateSkeletonComponent(PropertyContainerHandle_t,const char*,const char*,const char*,EVRSkeletalTrackingLevel,const VRBoneTransform_t*,uint32_t,VRInputComponentHandle_t*) override { return VRInputError_None; }
    EVRInputError UpdateSkeletonComponent(VRInputComponentHandle_t,EVRSkeletalMotionRange,const VRBoneTransform_t*,uint32_t) override { return VRInputError_None; }
    EVRInputError CreatePoseComponent(PropertyContainerHandle_t,const char*,VRInputComponentHandle_t*) override { return VRInputError_None; }
    EVRInputError UpdatePoseComponent(VRInputComponentHandle_t,const HmdMatrix34_t*,double) override { return VRInputError_None; }
    EVRInputError CreateEyeTrackingComponent(PropertyContainerHandle_t,const char*,VRInputComponentHandle_t*) override { return VRInputError_None; }
    EVRInputError UpdateEyeTrackingComponent(VRInputComponentHandle_t,const VREyeTrackingData_t* data,double) override { sample=*data; ++updates; return VRInputError_None; }
};
struct Context : IVRDriverContext {
    Input input;
    void* GetGenericInterface(const char* version,EVRInitError* error) override {
        if(error) *error=VRInitError_None;
        return std::strcmp(version,IVRDriverInput_Version)==0 ? &input : nullptr;
    }
    DriverHandle_t GetDriverHandle() override { return 1; }
};
int main(int argc,char** argv) {
    Context context;
    VRDriverContext()=&context;
    OpenVRInternal_ModuleServerDriverContext().Clear();
    auto& helper=eyeTrackingOutput;
    helper.initialized=true;
    helper.eyeTrackingComponentHandle=1;
    EyeTrackingOutput::EyeTrackingData data;
    data.focalPointX=0.4f; data.focalPointY=0.2f; data.focalPointZ=2;
    helper.SetEyeTrackingData(data);
    if(context.input.updates!=1 || !context.input.sample.bValid || context.input.sample.vGazeTarget.v[2]!=-2 || context.input.sample.vGazeTarget.v[0]!=0.4f) return 1;
    data.valid=false;
    helper.SetEyeTrackingData(data);
    if(context.input.sample.bValid || context.input.sample.bActive || context.input.sample.bTracked) return 2;
    data.valid=true;
    helper.SetEyeTrackingData(data);
    if(!context.input.sample.bValid) return 3;
    data.timestamp=std::chrono::duration<double>(std::chrono::high_resolution_clock::now().time_since_epoch()).count()-4;
    helper.SetEyeTrackingData(data);
    if(context.input.sample.bValid) return 4;
    CleanupDriverContext();
    if(argc!=2) return 5;
    auto module=LoadLibraryA(argv[1]);
    if(!module) return 6;
    using Factory=void* (*)(const char*,int*);
    auto factory=reinterpret_cast<Factory>(GetProcAddress(module,"HmdDriverFactory"));
    if(!factory) return 7;
    int error{};
    if(factory("unsupported",&error) || error!=VRInitError_Init_InterfaceNotFound) return 8;
    auto* provider=static_cast<IServerTrackedDeviceProvider*>(factory(IServerTrackedDeviceProvider_Version,&error));
    if(!provider || provider->ShouldBlockStandbyMode() || !provider->GetInterfaceVersions()) return 9;
    FreeLibrary(module);
    std::puts("PASS: helper coordinate conversion, immediate invalidation/recovery, stale input, and built DLL interface loading.");
}
