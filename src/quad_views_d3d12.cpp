#include "quad_views_d3d12.hpp"
#include "quad_view_bridge.hpp"
#include "eye_calibration_d3d12.hpp"
#include "d3d12_native.hpp"
#include "settings.hpp"
#include "runtime.hpp"
#include <vector>
#include <wrl/client.h>
namespace cheeky::foveated_dlss {
bool quad_view_stamp12(ID3D12GraphicsCommandList* list,ID3D12Resource* output,uint64_t view,
    unsigned x,unsigned y,unsigned width,unsigned height,D3D12_RESOURCE_STATES state) noexcept {
    try {
        struct Slot {Microsoft::WRL::ComPtr<ID3D12Device> device;std::shared_ptr<Calibration12Frame> frame;};
        static std::mutex mutex;static std::vector<Slot> slots;
        const auto quad=quad_view_snapshot();const bool app=quad.active&&quad_is_app_snapshot(quad);
        if(!app||!quad.show_numbers){
            std::lock_guard lock(mutex);
            std::erase_if(slots,[](const auto& slot){return calibration12_begin(*slot.frame);});
            return app;
        }
        unsigned number{};const auto generation=stereo_view_generation(view);
        for(const auto& source:quad.sources)if(source.dlss_view==view&&source.registration_generation==generation)number=source.number;
        if(!number||!list||!output)return true;
        Microsoft::WRL::ComPtr<ID3D12Device> device;list->GetDevice(IID_PPV_ARGS(&device));if(!device)return true;
        std::lock_guard lock(mutex);std::shared_ptr<Calibration12Frame> frame;
        std::erase_if(slots,[&](const auto& slot){return !same_d3d12_device(slot.device.Get(),device.Get())&&calibration12_begin(*slot.frame);});
        for(auto& slot:slots)if(same_d3d12_device(slot.device.Get(),device.Get())&&calibration12_begin(*slot.frame)){frame=slot.frame;break;}
        if(!frame&&slots.size()<64){frame=calibration12_create(device.Get());if(frame){slots.push_back({device,frame});calibration12_begin(*frame);}}
        if(!frame||!calibration12_numbers(*frame,list,output,number,x,y,width,height,state)){
            static unsigned failures{};const auto n=++failures;
            if(n<=8||n%300==0)trace_event("Quad D3D12 number grid unavailable view=%llu number=%u format=%u; no marker work recorded",view,number,unsigned(output->GetDesc().Format));
        }
        return true;
    }catch(...){return true;}
}
}
