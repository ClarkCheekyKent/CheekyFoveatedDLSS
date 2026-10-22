#pragma once
namespace {
std::vector<XrVersion> quad_version_attempts;
bool quad_version_runtime_native{},quad_version_promoted{};
XrResult quad_version_first_result=XR_ERROR_API_VERSION_UNSUPPORTED;
XrResult quad_version_fallback_result=XR_SUCCESS;
XrResult XRAPI_CALL quad_version_extensions(const char*,uint32_t capacity,uint32_t* count,XrExtensionProperties* output) {
    *count=quad_version_promoted?2:0;if(!capacity)return XR_SUCCESS;if(capacity<*count)return XR_ERROR_SIZE_INSUFFICIENT;
    if(quad_version_promoted){strcpy_s(output[0].extensionName,XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);output[0].extensionVersion=1;
        strcpy_s(output[1].extensionName,XR_EXT_PALM_POSE_EXTENSION_NAME);output[1].extensionVersion=1;}
    return XR_SUCCESS;
}
XrResult XRAPI_CALL quad_version_gipa(XrInstance,const char* name,PFN_xrVoidFunction* output) {
    *output=nullptr;
    if(!std::strcmp(name,"xrEnumerateInstanceExtensionProperties"))*output=reinterpret_cast<PFN_xrVoidFunction>(quad_version_extensions);
    if(!std::strcmp(name,"xrEnumerateViewConfigurations"))*output=reinterpret_cast<PFN_xrVoidFunction>(quad_test_configurations);
    return *output?XR_SUCCESS:XR_ERROR_FUNCTION_UNSUPPORTED;
}
XrResult XRAPI_CALL quad_version_create(const XrInstanceCreateInfo* info,const XrApiLayerCreateInfo*,XrInstance* output) {
    quad_version_attempts.push_back(info->applicationInfo.apiVersion);
    bool graphics{},floor{},palm{};
    for(uint32_t i=0;i<info->enabledExtensionCount;++i){const auto* name=info->enabledExtensionNames[i];
        graphics|=!std::strcmp(name,XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
        floor|=!std::strcmp(name,XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);palm|=!std::strcmp(name,XR_EXT_PALM_POSE_EXTENSION_NAME);
        check(std::strcmp(name,XR_VARJO_QUAD_VIEWS_EXTENSION_NAME)!=0,"Quad provider extension leaked to runtime");}
    check(graphics,"Version bridge lost application graphics extension");
    check(!std::strcmp(info->applicationInfo.applicationName,"MSFS version fixture"),"Version bridge changed application identity");
    if(XR_VERSION_MINOR(info->applicationInfo.apiVersion)>0&&!quad_version_runtime_native)return quad_version_first_result;
    if(quad_version_attempts.size()>1&&quad_version_promoted)check(floor&&palm,"Promoted feature extensions missing on fallback");
    if(XR_FAILED(quad_version_fallback_result))return quad_version_fallback_result;
    *output=h<XrInstance>(888);return XR_SUCCESS;
}
unsigned quad_locate_calls{};XrSpace quad_locate_fail{};
XrResult XRAPI_CALL quad_version_locate(XrSpace space,XrSpace base,XrTime time,XrSpaceLocation* location) {
    ++quad_locate_calls;check(base==h<XrSpace>(300)&&time==789,"Batch locate changed time/base space");
    if(space==quad_locate_fail)return XR_ERROR_HANDLE_INVALID;
    location->locationFlags=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    location->pose.orientation.w=1;location->pose.position.x=space==h<XrSpace>(10)?1.f:2.f;
    if(location->next){auto* velocity=static_cast<XrSpaceVelocity*>(location->next);velocity->velocityFlags=XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
        velocity->linearVelocity={3,4,5};velocity->angularVelocity={6,7,8};}
    return XR_SUCCESS;
}
XrResult XRAPI_CALL quad_version_native_locate(XrSession,const XrSpacesLocateInfo*,XrSpaceLocations*) {return XR_SESSION_LOSS_PENDING;}
void quad_version_validation() {
    const bool host=msfs_quad_version_bridge_host();
    XrApiLayerNextInfo next{};next.nextGetInstanceProcAddr=quad_version_gipa;next.nextCreateApiLayerInstance=quad_version_create;
    XrApiLayerCreateInfo layer{};layer.nextInfo=&next;
    const char* names[]{XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};strcpy_s(info.applicationInfo.applicationName,"MSFS version fixture");
    info.applicationInfo.apiVersion=XR_MAKE_VERSION(1,1,43);info.enabledExtensionCount=1;info.enabledExtensionNames=names;
    const auto run=[&]{instances.clear();sessions.clear();quad_version_attempts.clear();XrInstance instance{};
        const auto original_names=info.enabledExtensionNames;const auto original_count=info.enabledExtensionCount;
        const auto result=cheeky_xrCreateApiLayerInstance(&info,&layer,&instance);
        check(info.applicationInfo.apiVersion==XR_MAKE_VERSION(1,1,43)||XR_VERSION_MINOR(info.applicationInfo.apiVersion)!=1,"Version bridge modified caller version");
        check(info.enabledExtensionCount==original_count&&info.enabledExtensionNames==original_names,"Version bridge modified caller extensions");return result;};
    quad_version_runtime_native=true;
    check(run()==XR_SUCCESS&&quad_version_attempts.size()==1&&!instances.at(h<XrInstance>(888)).msfs_version_bridge,"Native 1.1 startup should not downgrade");
    quad_version_runtime_native=false;quad_version_promoted=true;
    check(run()==(host?XR_SUCCESS:XR_ERROR_API_VERSION_UNSUPPORTED),"MSFS version fallback result");
    check(quad_version_attempts.size()==(host?2:1),"Version fallback must be bounded and scoped to MSFS");
    if(host){
        check(quad_version_attempts[0]==XR_MAKE_VERSION(1,1,43)&&quad_version_attempts[1]==XR_MAKE_VERSION(1,0,43),"Version fallback versions");
        const auto& state=instances.at(h<XrInstance>(888));check(state.quad_capture_enabled&&state.msfs_version_bridge,"Version fallback disabled quad provider");
        uint32_t count{};std::array<XrViewConfigurationType,2> types{};
        check(cheeky_xrEnumerateViewConfigurations(h<XrInstance>(888),1,2,&count,types.data())==XR_SUCCESS&&count==2&&
            types[0]==cheeky::quad_capture::configuration&&types[1]==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            "Bridged MSFS must prefer quad views and retain stereo fallback");
    }
    quad_version_first_result=XR_ERROR_RUNTIME_FAILURE;
    check(run()==XR_ERROR_RUNTIME_FAILURE&&quad_version_attempts.size()==1,"Non-version error must not trigger downgrade");
    quad_version_first_result=XR_ERROR_API_VERSION_UNSUPPORTED;quad_version_fallback_result=XR_ERROR_API_VERSION_UNSUPPORTED;
    check(run()==XR_ERROR_API_VERSION_UNSUPPORTED&&quad_version_attempts.size()==(host?2:1),"Failed fallback must not loop");
    quad_version_fallback_result=XR_ERROR_EXTENSION_NOT_PRESENT;
    check(run()==(host?XR_ERROR_EXTENSION_NOT_PRESENT:XR_ERROR_API_VERSION_UNSUPPORTED)&&instances.empty(),"Failed bridge returned a false instance success");
    quad_version_fallback_result=XR_SUCCESS;quad_version_promoted=false;
    info.applicationInfo.apiVersion=XR_MAKE_VERSION(1,2,43);
    check(run()==XR_ERROR_API_VERSION_UNSUPPORTED&&quad_version_attempts.size()==1,"Future API version silently downgraded");
    info.applicationInfo.apiVersion=XR_MAKE_VERSION(1,0,43);
    check(run()==XR_SUCCESS&&quad_version_attempts.size()==1&&!instances.at(h<XrInstance>(888)).quad_capture_enabled,"Existing 1.0 startup changed");
    const char* explicit_quad[]{XR_KHR_D3D12_ENABLE_EXTENSION_NAME,XR_VARJO_QUAD_VIEWS_EXTENSION_NAME};
    info.enabledExtensionNames=explicit_quad;info.enabledExtensionCount=2;
    check(run()==XR_SUCCESS&&instances.at(h<XrInstance>(888)).quad_capture_enabled,"1.0 explicit quad request disabled provider");
    const bool prefer=host||dcs_quad_capture_host();
    check(instances.at(h<XrInstance>(888)).quad_capture_prefer_quad==prefer,"Quad preference must be scoped to MSFS/DCS executables");
    uint32_t offered{};std::array<XrViewConfigurationType,2> configurations{};
    check(cheeky_xrEnumerateViewConfigurations(h<XrInstance>(888),1,2,&offered,configurations.data())==XR_SUCCESS&&offered==2&&
        configurations[prefer?0:1]==cheeky::quad_capture::configuration,"Explicit vendor quad negotiation order incorrect");
    // Core 1.1 batch locations are supplied through the 1.0 single-space API.
    reset();instances.at(instance).msfs_version_bridge=true;instances.at(instance).dispatch.locate_space=quad_version_locate;
    SessionState session;session.session=h<XrSession>(2);session.instance=instance;sessions.emplace(session.session,std::move(session));
    const XrSpace spaces[]{h<XrSpace>(10),h<XrSpace>(11)};
    XrSpacesLocateInfo locate{XR_TYPE_SPACES_LOCATE_INFO};locate.baseSpace=h<XrSpace>(300);locate.time=789;locate.spaceCount=2;locate.spaces=spaces;
    std::array<XrSpaceLocationData,2> positions{};std::array<XrSpaceVelocityData,2> speeds{};
    XrSpaceVelocities velocities{XR_TYPE_SPACE_VELOCITIES};velocities.velocityCount=2;velocities.velocities=speeds.data();
    XrSpaceLocations output{XR_TYPE_SPACE_LOCATIONS};output.locationCount=2;output.locations=positions.data();output.next=&velocities;
    quad_locate_calls=0;quad_locate_fail=XR_NULL_HANDLE;
    check(cheeky_xrLocateSpaces(h<XrSession>(2),&locate,&output)==XR_SUCCESS&&quad_locate_calls==2,"Batch locate fallback");
    check(positions[0].pose.position.x==1&&positions[1].pose.position.x==2&&speeds[1].linearVelocity.z==5&&
        speeds[0].velocityFlags==XR_SPACE_VELOCITY_LINEAR_VALID_BIT,"Batch locate poses/velocities lost");
    positions[0].pose.position.x=99;quad_locate_fail=spaces[1];
    check(cheeky_xrLocateSpaces(h<XrSession>(2),&locate,&output)==XR_ERROR_HANDLE_INVALID&&positions[0].pose.position.x==99,"Failed batch locate partially overwrote output");
    output.locationCount=1;
    check(cheeky_xrLocateSpaces(h<XrSession>(2),&locate,&output)==XR_ERROR_VALIDATION_FAILURE,"Batch locate size mismatch");
    output.locationCount=2;instances.at(instance).msfs_version_bridge=false;
    check(cheeky_xrLocateSpaces(h<XrSession>(2),&locate,&output)==XR_ERROR_FUNCTION_UNSUPPORTED,"Unbridged runtime got synthetic core function");
    instances.at(instance).dispatch.locate_spaces=quad_version_native_locate;
    check(cheeky_xrLocateSpaces(h<XrSession>(2),&locate,&output)==XR_SESSION_LOSS_PENDING,"Native batch result changed");
    instances.clear();sessions.clear();quad_locate_fail=XR_NULL_HANDLE;
    std::cout<<"PASS MSFS 1.1 version bridge host="<<host<<", native/legacy/future versions, failure propagation, quad enumeration and core batch locations\n";
}
}
