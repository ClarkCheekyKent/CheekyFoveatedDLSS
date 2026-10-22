// Read-only probe through the game's loader and the installed API-layer chain.
// The executable is named FlightSimulator2024.exe only to exercise the narrowly
// scoped MSFS version bridge. It does not create a graphics session or render.
#include <Windows.h>
#include "../shared/openxr_startup_log.hpp"
#include <openxr/openxr.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

template<class T> T load(PFN_xrGetInstanceProcAddr gipa,XrInstance instance,const char* name) {
    PFN_xrVoidFunction fn{};const auto result=gipa(instance,name,&fn);
    if(XR_FAILED(result)||!fn)throw std::runtime_error(name);return reinterpret_cast<T>(fn);
}
int wmain(int argc,wchar_t** argv) {
    if(argc<2){std::cerr<<"Usage: FlightSimulator2024.exe <absolute openxr_loader.dll path> [--extensions-only]\n";return 2;}
    std::cout.setf(std::ios::unitbuf);
    std::cout<<"PROBE_PID="<<GetCurrentProcessId()<<'\n';
    std::wcout<<L"LAYER_LOG_PATH="<<cheeky::openxr_startup_log_path().wstring()<<L'\n';
    const auto module=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!module){std::cerr<<"Loader load failed: "<<GetLastError()<<'\n';return 1;}
    try {
        const auto gipa=reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(module,"xrGetInstanceProcAddr"));
        if(!gipa)throw std::runtime_error("xrGetInstanceProcAddr export");
        const auto enumerate=load<PFN_xrEnumerateInstanceExtensionProperties>(gipa,XR_NULL_HANDLE,"xrEnumerateInstanceExtensionProperties");
        bool global_quad{};
        for(const char* layer:{static_cast<const char*>(nullptr),"XR_APILAYER_CHEEKY_foveated_dlss"}){
            uint32_t count{};auto result=enumerate(layer,0,&count,nullptr);
            std::cout<<"extensions layer="<<(layer?layer:"GLOBAL")<<" capacity=0 result="<<result<<" count="<<count<<'\n';
            if(XR_FAILED(result))continue;
            std::vector<XrExtensionProperties> values(count,{XR_TYPE_EXTENSION_PROPERTIES});
            result=enumerate(layer,count,&count,values.data());
            std::cout<<"extensions layer="<<(layer?layer:"GLOBAL")<<" capacity="<<values.size()<<" result="<<result<<" count="<<count<<'\n';
            if(XR_FAILED(result))continue;
            for(const auto& value:values){
                if(!std::strcmp(value.extensionName,XR_VARJO_QUAD_VIEWS_EXTENSION_NAME)&&!layer)global_quad=true;
                std::cout<<"  extension="<<value.extensionName<<" revision="<<value.extensionVersion<<'\n';
            }
        }
        std::cout<<"GLOBAL_QUAD_EXTENSION_VISIBLE="<<global_quad<<'\n';
        if(argc>2&&!std::wcscmp(argv[2],L"--extensions-only"))return global_quad?0:1;
        const auto create=load<PFN_xrCreateInstance>(gipa,XR_NULL_HANDLE,"xrCreateInstance");
        const char* extensions[]{"XR_KHR_D3D12_enable"};
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};strcpy_s(info.applicationInfo.applicationName,"Cheeky capability probe");
        info.applicationInfo.apiVersion=XR_MAKE_VERSION(1,1,43);info.enabledExtensionCount=1;info.enabledExtensionNames=extensions;
        XrInstance instance{};auto result=create(&info,&instance);
        std::cout<<"create app_api=1.1.43 result="<<result<<'\n';if(XR_FAILED(result))return 1;
        const auto destroy=load<PFN_xrDestroyInstance>(gipa,instance,"xrDestroyInstance");
        struct Cleanup {XrInstance instance;PFN_xrDestroyInstance destroy;~Cleanup(){destroy(instance);}} cleanup{instance,destroy};
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        result=load<PFN_xrGetInstanceProperties>(gipa,instance,"xrGetInstanceProperties")(instance,&properties);
        std::cout<<"runtime result="<<result<<" name="<<properties.runtimeName<<'\n';
        XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};system_info.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;XrSystemId system{};
        result=load<PFN_xrGetSystem>(gipa,instance,"xrGetSystem")(instance,&system_info,&system);
        std::cout<<"get_system result="<<result<<'\n';if(XR_FAILED(result))return 3;
        const auto enumerate_configs=load<PFN_xrEnumerateViewConfigurations>(gipa,instance,"xrEnumerateViewConfigurations");
        uint32_t count{};result=enumerate_configs(instance,system,0,&count,nullptr);
        std::cout<<"configurations capacity=0 result="<<result<<" count="<<count<<'\n';if(XR_FAILED(result))return 1;
        std::vector<XrViewConfigurationType> configs(count);result=enumerate_configs(instance,system,count,&count,configs.data());
        std::cout<<"configurations capacity="<<configs.size()<<" result="<<result<<" count="<<count<<'\n';if(XR_FAILED(result))return 1;
        const auto enumerate_views=load<PFN_xrEnumerateViewConfigurationViews>(gipa,instance,"xrEnumerateViewConfigurationViews");
        const auto get_properties=load<PFN_xrGetViewConfigurationProperties>(gipa,instance,"xrGetViewConfigurationProperties");
        for(size_t index=0;index<configs.size();++index){const auto type=configs[index];
            std::cout<<"  preference="<<index<<" configuration="<<type<<'\n';
            XrViewConfigurationProperties p{XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};result=get_properties(instance,system,type,&p);
            std::cout<<"  properties configuration="<<type<<" result="<<result<<" fov_mutable="<<p.fovMutable<<'\n';
            uint32_t views_count{};result=enumerate_views(instance,system,type,0,&views_count,nullptr);
            std::cout<<"  views configuration="<<type<<" capacity=0 result="<<result<<" count="<<views_count<<'\n';
            if(XR_FAILED(result))continue;
            std::vector<XrViewConfigurationView> views(views_count,{XR_TYPE_VIEW_CONFIGURATION_VIEW});
            result=enumerate_views(instance,system,type,views_count,&views_count,views.data());
            std::cout<<"  views configuration="<<type<<" capacity="<<views.size()<<" result="<<result<<" count="<<views_count<<'\n';
            if(XR_SUCCEEDED(result))for(size_t i=0;i<views.size();++i)std::cout<<"    view="<<i<<" recommended="<<views[i].recommendedImageRectWidth<<'x'<<views[i].recommendedImageRectHeight<<'\n';
        }
        std::cout<<"QUAD_CONFIGURATION_VISIBLE="<<(std::find(configs.begin(),configs.end(),XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO)!=configs.end())<<'\n';
        return global_quad?0:1;
    } catch(const std::exception& error){std::cerr<<"Probe failed: "<<error.what()<<'\n';return 1;}
}
