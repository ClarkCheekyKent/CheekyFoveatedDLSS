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

using namespace cheeky::foveated_dlss;
namespace cheeky::foveated_dlss {
HMODULE find_loaded_ngx_core_runtime() noexcept {return GetModuleHandleW(L"_nvngx.dll");}
}
extern "C" PFN_vkVoidFunction VKAPI_CALL CheekyVkGetDeviceProcAddr(VkDevice,const char*);
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
VulkanDeviceApi api;
unsigned creations{},releases{},evaluations{};
struct FakeFeature {unsigned type{},quality{};};
unsigned nr_evaluations{},peripheral_evaluations{};
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
    const VkClearColorValue color{{nr||peripheral?1.0F:0, nr?0.0F:1,0,1}};
    load<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(cmd,image.image,VK_IMAGE_LAYOUT_GENERAL,&color,1,&image.range);
    if(nr)++nr_evaluations;else {++evaluations;if(peripheral)++peripheral_evaluations;}return 1;
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
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=output_size*output_size*16;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        require(api.CreateBuffer(device,&bi,nullptr,&readback)==VK_SUCCESS,"readback buffer");
        VkMemoryRequirements mr{};api.GetBufferMemoryRequirements(device,readback,&mr);
        unsigned type{};const auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        while(type<api.memory.memoryTypeCount && (!(mr.memoryTypeBits&(1U<<type)) || (api.memory.memoryTypes[type].propertyFlags&flags)!=flags))++type;
        require(type<api.memory.memoryTypeCount,"readback memory type");
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=mr.size;ma.memoryTypeIndex=type;
        require(api.AllocateMemory(device,&ma,nullptr,&memory)==VK_SUCCESS && api.BindBufferMemory(device,readback,memory,0)==VK_SUCCESS,"readback memory");
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
                const VkClearColorValue fill{{0,0,image==&color?1.0F:0.0F,1}};
                load<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(cmd,b.image,VK_IMAGE_LAYOUT_GENERAL,&fill,1,&b.subresourceRange);
            }
            DlssFrameContract contract{};require(read_ngx_frame_contract(&p,123,1,contract),"frame decode");
            if(frame==2)settings.width=.75F;
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
