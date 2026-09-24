#include "vulkan_calibration.hpp"
#include "eye_calibration.hpp"
#include "eye_calibration_pixels.hpp"
#include <d3d11.h>
#include <wrl/client.h>
#include "vulkan_api.hpp"
#include "vulkan_gpu.hpp"
#include "vulkan_observer.hpp"
#include "vulkan_backend.hpp"
#include "vulkan_nr_runtime.hpp"
#include <filesystem>
#include "ngx_frame_contract.hpp"
#include "mock_ngx_parameters.hpp"
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <cmath>
#include <array>

using namespace cheeky::foveated_dlss;
namespace cheeky::foveated_dlss {
HMODULE find_loaded_ngx_core_runtime() noexcept {return GetModuleHandleW(L"_nvngx.dll");}
}
extern "C" PFN_vkVoidFunction VKAPI_CALL CheekyVkGetDeviceProcAddr(VkDevice,const char*);
namespace {
void require(bool ok,const char* message){if(!ok){std::fprintf(stderr,"Vulkan check failed: %s\n",message);throw std::runtime_error(message);}}
VulkanDeviceApi api;
unsigned creations{},releases{},evaluations{};
struct FakeFeature {unsigned type{},quality{};};
unsigned nr_evaluations{},peripheral_evaluations{};
VulkanNgxResource* original_motion{};
VkBuffer motion_readback{};
VkDeviceSize motion_readback_offset{};
bool inspect_motion{},expect_converted_motion{};
int expected_history_reset{-1};
template<class T> T load(const char* name){return reinterpret_cast<T>(api.gdpa(api.device,name));}
NgxResult create(VkCommandBuffer,unsigned feature,NgxParameters* p,NgxHandle** out) {
    require((feature==1 || feature==18) && get_ui(p,"Width") && get_ui(p,"OutWidth"),"private create contract");
    ++creations;*out=reinterpret_cast<NgxHandle*>(new FakeFeature{feature,get_ui(p,"PerfQualityValue")});return 1;
}
NgxResult release(NgxHandle* handle){delete reinterpret_cast<FakeFeature*>(handle);++releases;return 1;}
NgxResult evaluate(VkCommandBuffer cmd,const NgxHandle* handle,const NgxParameters* p,NgxProgressCallback) {
    const auto feature=*reinterpret_cast<const FakeFeature*>(handle);
    void* value{};require(ngx_succeeded(p->Get(feature.type==18?"DLSSNR.Output":"Output",&value)),"private output binding");
    const auto& image=static_cast<VulkanNgxResource*>(value)->resource.image;
    require(image.width==get_ui(p,"OutWidth") && image.height==get_ui(p,"OutHeight"),"private output extent");
    const bool nr=feature.type==18,peripheral=feature.quality==5;
    if(expected_history_reset>=0 && !nr && !peripheral)
        require(get_ui(p,"Reset")==static_cast<unsigned>(expected_history_reset),"private history must reset after a bypass and stay warm on consecutive frames");
    if(inspect_motion && !nr && !peripheral) {
        void* mv{};require(ngx_succeeded(p->Get("MotionVectors",&mv)),"motion binding");
        require((mv!=original_motion)==expect_converted_motion,"fixed crop must preserve game motion resource; moving crop must convert");
        const auto x=get_ui(p,"DLSS.Input.MV.Subrect.Base.X"),y=get_ui(p,"DLSS.Input.MV.Subrect.Base.Y");
        require(x==(expect_converted_motion?0U:get_ui(p,"DLSS.Input.Color.Subrect.Base.X")) &&
            y==(expect_converted_motion?0U:get_ui(p,"DLSS.Input.Depth.Subrect.Base.Y")),"motion crop coordinates");
        const auto& source=static_cast<VulkanNgxResource*>(mv)->resource.image;
        vulkan_barrier(api,cmd,source,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};copy.bufferOffset=motion_readback_offset;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.imageOffset={static_cast<int>(x),static_cast<int>(y),0};copy.imageExtent={1,1,1};
        load<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(cmd,source.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,motion_readback,1,&copy);
        vulkan_barrier(api,cmd,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    }
    const VkClearColorValue color{{nr||peripheral?1.0F:0, nr?0.0F:1,0,1}};
    load<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(cmd,image.image,VK_IMAGE_LAYOUT_GENERAL,&color,1,&image.range);
    if(nr)++nr_evaluations;else {++evaluations;if(peripheral)++peripheral_evaluations;}return 1;
}
void calibration_test(VkCommandBuffer cmd,VkQueue queue,PFN_vkGetDeviceProcAddr observed,
    VkFormat format,bool mono,bool cropped) {
    using Microsoft::WRL::ComPtr;
    constexpr unsigned size=512;
    const unsigned submitted_size=cropped?384:size,offset=cropped?64:0;
    const auto saved=configured_settings();
    auto settings=saved;settings.eye_calibration_method=cropped?EyeCalibrationMethod::full:EyeCalibrationMethod::standard;
    settings.eye_calibration_continuous=!cropped;update_settings(settings);
    set_eye_calibration_learning(0,0,0);
    eye_calibration_stop();eye_calibration_reset_stats();eye_calibration_enable(true);
    register_stereo_view(8901);if (!mono) register_stereo_view(8902);
    ComPtr<ID3D11Device> device11;ComPtr<ID3D11DeviceContext> context11;
    require(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
        &device11,nullptr,&context11)),"calibration submission device");
    std::array<ComPtr<ID3D11Texture2D>,2> submitted;
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=submitted_size;td.MipLevels=td.ArraySize=1;
    td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;
    for (auto& texture:submitted) require(SUCCEEDED(device11->CreateTexture2D(&td,nullptr,&texture)),"submission texture");
    VulkanImage source;require(source.create(api,size,size,format),"calibration source");
    const auto dxformat=format==VK_FORMAT_R16G16B16A16_SFLOAT?DXGI_FORMAT_R16G16B16A16_FLOAT:
        format==VK_FORMAT_R8G8B8A8_UNORM?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R32G32B32A32_FLOAT;
    const unsigned bytes=calibration_pixel_bytes(dxformat);
    VulkanBuffer readback;require(readback.create(api,size*size*bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT),"calibration transfer");
    MockNgxParameters parameters;parameters.Set("Output",static_cast<void*>(&source.ngx));
    DlssFrameContract contract{};contract.output_width=contract.output_height=size;
    std::vector<CalibrationPixel> pixels(submitted_size*submitted_size);
    bool blank=true;
    const auto render=[&](unsigned candidate,bool execute=true) {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        require(reinterpret_cast<PFN_vkBeginCommandBuffer>(observed(api.device,"vkBeginCommandBuffer"))(cmd,&begin)==VK_SUCCESS,"begin calibration");
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.image=source.ngx.resource.image.image;barrier.subresourceRange=source.ngx.resource.image.range;
        barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        barrier.dstAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        reinterpret_cast<PFN_vkCmdPipelineBarrier>(observed(api.device,"vkCmdPipelineBarrier"))(cmd,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        const VkClearColorValue gray{{.2F,.2F,.2F,1}};
        load<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(cmd,barrier.image,VK_IMAGE_LAYOUT_GENERAL,&gray,1,&barrier.subresourceRange);
        contract.view_id=8901+candidate;
        vulkan_calibration_stamp(cmd,&parameters,contract);
        vulkan_barrier(api,cmd,source.ngx.resource.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={size,size,1};
        load<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(cmd,barrier.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,1,&copy);
        require(load<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(cmd)==VK_SUCCESS,"end calibration");
        if (execute) {
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;
            require(reinterpret_cast<PFN_vkQueueSubmit>(observed(api.device,"vkQueueSubmit"))(queue,1,&submit,VK_NULL_HANDLE)==VK_SUCCESS,"submit calibration");
            require(load<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue)==VK_SUCCESS,"calibration transfer completion");
        }
        require(reinterpret_cast<PFN_vkResetCommandBuffer>(observed(api.device,"vkResetCommandBuffer"))(cmd,0)==VK_SUCCESS,"retire calibration recording");
        if (!execute) return;
        const auto* data=static_cast<const unsigned char*>(readback.mapped);
        for (unsigned y=0;y<submitted_size;++y) for (unsigned x=0;x<submitted_size;++x)
            pixels[y*submitted_size+x]=blank?CalibrationPixel{.2F,.2F,.2F,1}:
                calibration_decode(data+((y+offset)*size+x+offset)*bytes,dxformat);
        context11->UpdateSubresource(submitted[mono?0:1-candidate].Get(),0,nullptr,pixels.data(),submitted_size*16,0);
        if (mono) context11->UpdateSubresource(submitted[1].Get(),0,nullptr,pixels.data(),submitted_size*16,0);
    };
    const auto tick=[&] {
        eye_calibration_frame(EyeCalibrationBackend::openxr,8900,11);
        render(0);if (!mono) render(1);
        for (unsigned eye=0;eye<2;++eye) eye_calibration_result(eye_calibration_submit(submitted[eye].Get(),eye,0,0,1,1,0,
            EyeCalibrationBackend::openxr,8900),0,eye);
        context11->Flush();eye_calibration_tick();Sleep(5);
    };
    for (unsigned n=0;n<12;++n) tick();
    require(!eye_calibration_stats().valid,"absent submitted markers must never map Vulkan eyes");
    blank=false;eye_calibration_recalibrate();
    for (unsigned n=0;n<180 && !eye_calibration_stats().valid;++n) tick();
    auto stats=eye_calibration_stats();
    if (!stats.valid) std::puts(eye_calibration_json().c_str());
    require(stats.valid && stats.source_graphics_api==13 && stats.submission_graphics_api==11,"Vulkan source must join D3D11 submissions");
    require(stats.left_view==(mono?8901:8902) && stats.right_view==8901,"physical eye mapping from Vulkan marker pixels");
    if (cropped) {
        require(stats.crop_mapping_active,"Vulkan locator grid must recover cropped submissions");
        const auto captures=eye_calibration_stats().captures;
        for (unsigned n=0;n<12;++n) tick();
        require(eye_calibration_stats().captures==captures,"retained Vulkan calibration stops capture");
        require(std::abs(pixels[20*submitted_size+20].r-.2F)<.01F,"retained source must remain unstamped");
    }
    // A recording which was reset without submission must not provide source proof.
    eye_calibration_recalibrate();
    eye_calibration_frame(EyeCalibrationBackend::openxr,8900,11);render(0,false);if (!mono) render(1,false);
    for (unsigned n=0;n<5;++n) eye_calibration_frame(EyeCalibrationBackend::openxr,8900,11);
    require(!stereo_eye_assignment(8901).calibrated,"unsubmitted recording cannot establish calibration");
    eye_calibration_stop();vulkan_calibration_forget_command(cmd);
    source.destroy(api);readback.destroy(api);
    unregister_stereo_view(8901);if (!mono) unregister_stereo_view(8902);update_settings(saved);
    std::printf("PASS Vulkan calibration format=%u mono=%u crop=%u: GPU source proof, missing markers, eye mapping, retirement\n",format,mono,cropped);
}

}
int run_vulkan_tests(bool real, bool integration) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const unsigned input_size=real?512U:64U,output_size=real?512U:128U;
    try {
        std::puts(real?"Starting real NVIDIA Vulkan model test":"Starting native Vulkan pixel tests");
        wchar_t bootstrap_path[32768]{};
        if(GetEnvironmentVariableW(L"CHEEKY_VULKAN_TEST_BOOTSTRAP",bootstrap_path,32768))
            require(LoadLibraryW(bootstrap_path)!=nullptr,"load drop-in bootstrap");
        HMODULE core{};std::filesystem::path model_directory;
        if(real) {
            wchar_t path[32768]{};
            require(GetEnvironmentVariableW(L"CHEEKY_VULKAN_TEST_CORE",path,32768)>0,"Set CHEEKY_VULKAN_TEST_CORE to NVIDIA's core DLL");
            core=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);require(core!=nullptr,"load core");
            require(GetEnvironmentVariableW(L"CHEEKY_VULKAN_TEST_SR",path,32768)>0,"Set CHEEKY_VULKAN_TEST_SR to NVIDIA's SR DLL");
            model_directory=std::filesystem::path(path).parent_path();
        }
        auto loader=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);require(loader!=nullptr,"Vulkan loader unavailable");
        auto gi=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));
        auto create_instance=reinterpret_cast<PFN_vkCreateInstance>(gi(VK_NULL_HANDLE,"vkCreateInstance"));
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="Cheeky Vulkan tests";app.apiVersion=VK_API_VERSION_1_1;
        VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ii.pApplicationInfo=&app;
        std::vector<std::string> nr_instance_extensions;
        std::vector<const char*> instance_extensions;
        if(real) {
            vulkan_nr_extensions(nr_instance_extensions);
            for(const auto& name:nr_instance_extensions){instance_extensions.push_back(name.c_str());std::printf("NR instance extension %s\n",name.c_str());}
            ii.enabledExtensionCount=static_cast<unsigned>(instance_extensions.size());ii.ppEnabledExtensionNames=instance_extensions.data();
        }
        VkInstance instance{};require(create_instance(&ii,nullptr,&instance)==VK_SUCCESS,"vkCreateInstance");
        auto enumerate=reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gi(instance,"vkEnumeratePhysicalDevices"));
        unsigned count{};require(enumerate(instance,&count,nullptr)==VK_SUCCESS && count,"No Vulkan GPU");
        std::vector<VkPhysicalDevice> physicals(count);enumerate(instance,&count,physicals.data());auto physical=physicals[0];
        auto families=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gi(instance,"vkGetPhysicalDeviceQueueFamilyProperties"));
        families(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> props(count);families(physical,&count,props.data());
        unsigned family{};while(family<count && !(props[family].queueFlags&VK_QUEUE_COMPUTE_BIT))++family;require(family<count,"No compute queue");
        float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
        std::vector<std::string> nr_device_extensions;
        std::vector<const char*> device_extensions;
        if(real) {
            vulkan_nr_extensions(nr_device_extensions,instance,physical);
            for(const auto& name:nr_device_extensions){device_extensions.push_back(name.c_str());std::printf("NR device extension %s\n",name.c_str());}
            di.enabledExtensionCount=static_cast<unsigned>(device_extensions.size());di.ppEnabledExtensionNames=device_extensions.data();
        }
        VkDevice device{};require(reinterpret_cast<PFN_vkCreateDevice>(gi(instance,"vkCreateDevice"))(physical,&di,nullptr,&device)==VK_SUCCESS,"vkCreateDevice");
        auto gd=reinterpret_cast<PFN_vkGetDeviceProcAddr>(gi(instance,"vkGetDeviceProcAddr"));
        require(api.initialize(instance,physical,device,gi,gd) && (integration || vulkan_observe_device(instance,physical,device,gi,gd)),"Vulkan dispatch initialization");
        const auto observed=integration?gd:CheekyVkGetDeviceProcAddr;
        VkQueue queue{};reinterpret_cast<PFN_vkGetDeviceQueue>(observed(device,"vkGetDeviceQueue"))(device,family,0,&queue);
        VkCommandPool pool{};VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.queueFamilyIndex=family;pci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        require(load<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device,&pci,nullptr,&pool)==VK_SUCCESS,"command pool");
        VkCommandBuffer cmd{};VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;
        require(reinterpret_cast<PFN_vkAllocateCommandBuffers>(observed(device,"vkAllocateCommandBuffers"))(device,&cai,&cmd)==VK_SUCCESS,"command buffer");
        VulkanNgxCallbacks real_sr{},real_nr{};
        if(real) {
            using Init=NgxResult(*)(unsigned long long,const wchar_t*,VkInstance,VkPhysicalDevice,VkDevice,PFN_vkGetInstanceProcAddr,PFN_vkGetDeviceProcAddr,unsigned,const NgxFeatureCommonInfo*);
            auto init=reinterpret_cast<Init>(GetProcAddress(core,"NVSDK_NGX_VULKAN_Init_Ext2"));require(init!=nullptr,"core Vulkan init export");
            const wchar_t* dirs[]={model_directory.c_str()};NgxFeatureCommonInfo common;common.path_list={dirs,1};
            const auto initialized=init(0x0876232cULL,L".",instance,physical,device,gi,gd,0x15,&common);
            std::printf("Real SR init 0x%08X\n",initialized);require(ngx_succeeded(initialized),"real SR initialization");
            real_sr.create=reinterpret_cast<VulkanNgxCreate>(GetProcAddress(core,"NVSDK_NGX_VULKAN_CreateFeature"));
            real_sr.evaluate=reinterpret_cast<VulkanNgxEvaluate>(GetProcAddress(core,"NVSDK_NGX_VULKAN_EvaluateFeature"));
            real_sr.release=reinterpret_cast<VulkanNgxRelease>(GetProcAddress(core,"NVSDK_NGX_VULKAN_ReleaseFeature"));
            require(real_sr.create && real_sr.evaluate && real_sr.release,"core Vulkan callbacks");
            NgxResult result{};const bool ready=integration || vulkan_nr_runtime(api,real_nr,result);
            std::printf("Real NR init 0x%08X ready=%u\n",result,ready);require(ready,"real NR initialization");
        }
        VulkanImage color,depth,motion,output;
        require(color.create(api,input_size,input_size,VK_FORMAT_R32G32B32A32_SFLOAT) && depth.create(api,input_size,input_size,VK_FORMAT_R32_SFLOAT) &&
            motion.create(api,input_size,input_size,VK_FORMAT_R32G32_SFLOAT) && output.create(api,output_size,output_size,VK_FORMAT_R32G32B32A32_SFLOAT),"test images");
        VkBuffer readback{};VkDeviceMemory memory{};
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=output_size*output_size*16+16;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        require(api.CreateBuffer(device,&bi,nullptr,&readback)==VK_SUCCESS,"readback buffer");
        VkMemoryRequirements mr{};api.GetBufferMemoryRequirements(device,readback,&mr);
        unsigned type{};const auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        while(type<api.memory.memoryTypeCount && (!(mr.memoryTypeBits&(1U<<type)) || (api.memory.memoryTypes[type].propertyFlags&flags)!=flags))++type;
        require(type<api.memory.memoryTypeCount,"readback memory type");
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=mr.size;ma.memoryTypeIndex=type;
        require(api.AllocateMemory(device,&ma,nullptr,&memory)==VK_SUCCESS && api.BindBufferMemory(device,readback,memory,0)==VK_SUCCESS,"readback memory");
        original_motion=&motion.ngx;motion_readback=readback;motion_readback_offset=output_size*output_size*16;
        MockNgxParameters p;p.Set("Width",input_size);p.Set("Height",input_size);p.Set("OutWidth",output_size);p.Set("OutHeight",output_size);
        p.Set("DLSS.Feature.Create.Flags",real?66U:2U);p.Set("PerfQualityValue",real?5U:1U);p.Set("MV.Scale.X",1.0F);p.Set("MV.Scale.Y",1.0F);
        p.Set("Color",static_cast<void*>(&color.ngx));p.Set("Depth",static_cast<void*>(&depth.ngx));p.Set("MotionVectors",static_cast<void*>(&motion.ngx));p.Set("Output",static_cast<void*>(&output.ngx));
        p.Set("Jitter.Offset.X",0.0F);p.Set("Jitter.Offset.Y",0.0F);p.Set("Pre.Exposure",1.0F);p.Set("Exposure.Scale",1.0F);
        const auto original=p.values;
        Settings settings;settings.auto_stereo_alignment=false;settings.peripheral_dlaa_enabled=false;settings.width=.5F;settings.height=.5F;
        settings.x_offset=settings.height_offset=0;settings.transition_width=0;settings.alignment_border_enabled=true;
        const VulkanNgxCallbacks callbacks=real?real_sr:VulkanNgxCallbacks{create,evaluate,release};
        const VulkanNgxCallbacks nr_callbacks=real?real_nr:callbacks;
        FakeFeature game_feature{1,1};
        NgxHandle* integrated_feature{};
        using HostCommand=bool(*)(const char*);
        using Snapshot=bool(*)(char*,unsigned);
        HMODULE host=GetModuleHandleW(L"CheekyFoveatedDLSSHost.dll");
        HostCommand command{};Snapshot snapshot{};
        if(integration) {
            require(host!=nullptr,"Vulkan bootstrap did not load the host");
            command=reinterpret_cast<HostCommand>(GetProcAddress(host,"CheekyHost_Command"));
            snapshot=reinterpret_cast<Snapshot>(GetProcAddress(host,"CheekyHost_Snapshot"));
            require(command && snapshot,"host test exports");
            require(command("1\n1\nset\nEnabled=true\nNrEnabled=false\nPeripheralDlaa=false\nAutoStereoAlignment=false\nCenterMode=0\nWidth=0.5\nHeight=0.5\nAlignmentBorder=true\n"),"fixed SR settings");
        }
        for(unsigned frame=0;frame<(integration?6U:9U);++frame) {
            std::printf("Vulkan frame %u recording\n",frame);
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            require(reinterpret_cast<PFN_vkBeginCommandBuffer>(observed(device,"vkBeginCommandBuffer"))(cmd,&begin)==VK_SUCCESS,"begin command");
            auto barrier=reinterpret_cast<PFN_vkCmdPipelineBarrier>(observed(device,"vkCmdPipelineBarrier"));
            for(auto* image:{&color,&depth,&motion,&output}) {
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image->ngx.resource.image.image;b.subresourceRange=image->ngx.resource.image.range;
                b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
                b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
                barrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
                const VkClearColorValue fill{{!real && image==&motion?4.0F:0.0F,!real && image==&motion?-2.0F:0.0F,image==&color?1.0F:0.0F,1}};
                load<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(cmd,b.image,VK_IMAGE_LAYOUT_GENERAL,&fill,1,&b.subresourceRange);
            }
            DlssFrameContract contract{};require(read_ngx_frame_contract(&p,123,1,contract),"frame decode");
            if(frame==2)settings.width=.75F;
            if(!real)settings.height_offset=frame==3?.25F:0.0F;
            inspect_motion=!real && frame<8;expect_converted_motion=frame==3 || frame==4;
            if(frame==4){settings.peripheral_dlaa_enabled=true;settings.alignment_border_enabled=false;}
            if(frame==6){settings.peripheral_dlaa_enabled=false;settings.nr_enabled=true;settings.nr_width=settings.nr_height=.5F;settings.nr_transition_width=0;}
            if(frame==7)settings.nr_processing_order=NrProcessingOrder::before_upscaling;
            if(frame==8){settings.enabled=real;settings.nr_processing_order=NrProcessingOrder::after_upscaling;}
            NgxResult result{};
            if(integration) {
                if(!integrated_feature)require(ngx_succeeded(real_sr.create(cmd,1,&p,&integrated_feature)),"game Vulkan feature creation");
                if(frame==2)require(command("1\n2\nset\nWidth=0.75\n"),"resize fixed SR");
                result=real_sr.evaluate(cmd,integrated_feature,&p,nullptr);
                require(ngx_succeeded(result),"intercepted game evaluation");
                char status[32768]{};require(snapshot(status,sizeof(status)),"runtime snapshot");
                auto vk=std::string_view(status).substr(std::string_view(status).find("\"vulkan\""));
                const auto expected="\"active\":"+std::to_string(frame+1)+",";
                if(vk.find(expected)==vk.npos)std::puts(status);
                require(vk.find(expected)!=vk.npos,"native Vulkan SR hook did not process the frame");
            }else require(evaluate_vulkan_backend(cmd,&p,contract,settings,callbacks,result,real?nullptr:reinterpret_cast<NgxHandle*>(&game_feature),nullptr,&nr_callbacks),vulkan_backend_status().reason);
            require(p.values==original,"game parameter map changed");
            vulkan_barrier(api,cmd,output.ngx.resource.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={output_size,output_size,1};
            load<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(cmd,output.ngx.resource.image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
            require(load<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(cmd)==VK_SUCCESS,"end command");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;
            require(reinterpret_cast<PFN_vkQueueSubmit>(observed(device,"vkQueueSubmit"))(queue,1,&submit,VK_NULL_HANDLE)==VK_SUCCESS,"submit");
            require(load<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue)==VK_SUCCESS,"wait for test readback");
            float* pixels{};require(api.MapMemory(device,memory,0,bi.size,0,reinterpret_cast<void**>(&pixels))==VK_SUCCESS,"map readback");
            if(real) {
                unsigned finite{};double energy{};
                for(unsigned i=0;i<output_size*output_size*4;++i){finite+=std::isfinite(pixels[i]);energy+=std::abs(pixels[i]);}
                std::printf("Real frame %u result=0x%08X NR=0x%08X energy=%.3f active=%llu NRactive=%llu\n",frame,result,
                    vulkan_backend_status().nr_result,energy,vulkan_backend_status().active,vulkan_backend_status().nr_active);
                require(finite==output_size*output_size*4 && energy>output_size*output_size*.1,"real model pixels invalid");
                if(frame>=6)require(vulkan_backend_status().nr_active==frame-5,"real NR evaluation failed");
            }else {
            if(inspect_motion) {
                const auto* mv=pixels+output_size*output_size*4;
                require(std::abs(mv[0]-4.0F)<.001F && std::abs(mv[1]-(frame==3?2.0F:frame==4?-6.0F:-2.0F))<.001F,
                    "motion values changed for fixed crop or wrong moving-crop correction");
            }
            const auto center=(64*128+64)*4;
            if(frame==6 || frame==8)require(pixels[center]>.99F && pixels[center+1]<.01F,"post-SR NR output missing");
            else require(std::abs(pixels[center+1]-1)<.01F && pixels[center]<.01F,"center is not private DLSS output");
            if(frame==4 || frame==5)require(pixels[0]>.99F && pixels[1]>.99F,"peripheral DLAA output missing");
            else if(frame==8)require(pixels[1]>.99F && pixels[0]<.01F,"NR-only path lost original game DLSS output");
            else require(std::abs(pixels[2]-1)<.01F && pixels[0]<.01F,"periphery is not source image");
            const unsigned border_x=frame<2?32:16;
            if(frame<4)require(pixels[(64*128+border_x)*4]>.99F,"alignment border missing");
            }
            api.UnmapMemory(device,memory);
        }
        if(!real)require(creations==6 && evaluations==11 && nr_evaluations==3 && peripheral_evaluations==2,"feature cache or pass count mismatch");
        if(!real) {
            // Worker threads can retain more command recordings than there are
            // queued frames. A single mono view must handle the whole rotation.
            std::array<VkCommandBuffer,16> rotation{};cai.commandBufferCount=static_cast<unsigned>(rotation.size());
            require(reinterpret_cast<PFN_vkAllocateCommandBuffers>(observed(device,"vkAllocateCommandBuffers"))(device,&cai,rotation.data())==VK_SUCCESS,"rotation command buffers");
            Settings fixed;fixed.auto_stereo_alignment=false;fixed.width=fixed.height=.5F;fixed.x_offset=fixed.height_offset=0;
            fixed.peripheral_dlaa_enabled=false;fixed.nr_enabled=false;fixed.transition_width=0;fixed.alignment_border_enabled=true;
            inspect_motion=false;
            for(unsigned frame=0;frame<rotation.size()*2;++frame) {
                const auto recording=rotation[frame%rotation.size()];
                VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                require(reinterpret_cast<PFN_vkBeginCommandBuffer>(observed(device,"vkBeginCommandBuffer"))(recording,&begin)==VK_SUCCESS,"begin rotation command");
                vulkan_barrier(api,recording,output.ngx.resource.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
                DlssFrameContract contract{};require(read_ngx_frame_contract(&p,456,1,contract),"rotation frame contract");NgxResult result{};
                if(frame==16) {
                    auto missing=contract;missing.mv_base_x=UINT32_MAX;
                    require(!evaluate_vulkan_backend(recording,&p,missing,fixed,callbacks,result),"invalid motion region must bypass");
                }
                expected_history_reset=frame==0 || frame==16?1:0;
                const bool handled=evaluate_vulkan_backend(recording,&p,contract,fixed,callbacks,result);
                if(!handled)std::printf("Rotation frame %u: %s\n",frame,vulkan_backend_status().reason);
                require(handled,"mono command rotation bypassed Cheeky");
                vulkan_barrier(api,recording,output.ngx.resource.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={output_size,output_size,1};
                load<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(recording,output.ngx.resource.image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
                require(load<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(recording)==VK_SUCCESS,"end rotation command");
                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&recording;
                require(reinterpret_cast<PFN_vkQueueSubmit>(observed(device,"vkQueueSubmit"))(queue,1,&submit,VK_NULL_HANDLE)==VK_SUCCESS,"rotation submit");
                require(load<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue)==VK_SUCCESS,"rotation completion");
                float* pixels{};require(api.MapMemory(device,memory,0,bi.size,0,reinterpret_cast<void**>(&pixels))==VK_SUCCESS,"rotation readback");
                require(pixels[(64*128+32)*4]>.99F,"border disappeared during command-buffer rotation");api.UnmapMemory(device,memory);
            }
            expected_history_reset=-1;
            vulkan_release_view(456);
            reinterpret_cast<PFN_vkFreeCommandBuffers>(observed(device,"vkFreeCommandBuffers"))(device,pool,static_cast<unsigned>(rotation.size()),rotation.data());
            std::puts("PASS mono view: border present across 32 frames using 16 command buffers; history reset after bypass");
        }
        if (!real) {
            calibration_test(cmd,queue,observed,VK_FORMAT_R32G32B32A32_SFLOAT,false,false);
            calibration_test(cmd,queue,observed,VK_FORMAT_R16G16B16A16_SFLOAT,true,false);
            calibration_test(cmd,queue,observed,VK_FORMAT_R8G8B8A8_UNORM,false,true);
        }
        std::puts("Test releasing game feature");
        if(integrated_feature)real_sr.release(integrated_feature);
        if(!integration){vulkan_release_view(123);vulkan_forget_command(cmd);vulkan_backend_release_device(device);}
        if(!real)require(releases==creations,"private feature lifetime leak");
        std::puts("Test destroying images");
        for(auto* image:{&color,&depth,&motion,&output})image->destroy(api);
        api.DestroyBuffer(device,readback,nullptr);api.FreeMemory(device,memory,nullptr);
        std::puts("Test destroying command pool");
        load<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device,pool,nullptr);
        if(real) {
            // The driver-core export has a result out-parameter; it is not the
            // one-argument public SDK entry point with the same export name.
            using Shutdown=void(*)(VkDevice,NgxResult*);
            auto shutdown=reinterpret_cast<Shutdown>(GetProcAddress(core,"NVSDK_NGX_VULKAN_Shutdown1"));
            std::puts("Test shutting down NGX core");
            NgxResult shutdown_result{};
            if(shutdown){shutdown(device,&shutdown_result);require(ngx_succeeded(shutdown_result),"game NGX shutdown");}
            std::puts("Test core shutdown returned");
        }
        reinterpret_cast<PFN_vkDestroyDevice>(observed(device,"vkDestroyDevice"))(device,nullptr);
        reinterpret_cast<PFN_vkDestroyInstance>(gi(instance,"vkDestroyInstance"))(instance,nullptr);
        std::puts(integration?"PASS Vulkan layer/bootstrap, native SR interception, real inference, resize, immutable game parameters and teardown":
            real?"PASS real Vulkan SR, peripheral DLAA, pre/post NR, parameter preservation and resize":
            "PASS native Vulkan SR, peripheral DLAA, pre/post NR, NR-only, parameter preservation, resize and feature lifetime");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL Vulkan: %s\n",error.what());return 1;}
}
