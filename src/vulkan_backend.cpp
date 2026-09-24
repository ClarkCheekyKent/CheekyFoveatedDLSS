#include "vulkan_backend.hpp"
#include "vulkan_gpu.hpp"
#include "vulkan_peripheral.hpp"
#include "vulkan_nr.hpp"
#include "vulkan_nr_runtime.hpp"
#include "vulkan_observer.hpp"
#include "vulkan_shaders.hpp"
#include "ngx_parameter_overlay.hpp"
#include "composite_constants.hpp"
#include "gaze_foveation.hpp"
#include "crop_motion.hpp"
#include "motion_region.hpp"
#include "runtime.hpp"
#include <array>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace cheeky::foveated_dlss {
namespace {
using FeatureKey=VulkanFeatureKey;
struct Feature : VulkanFeature {
    CropGeometry previous{};bool history{};
};
struct Slot {
    VkCommandBuffer command{};bool pending{};
    VulkanImage output,motion;
    VulkanPeripheralSlot peripheral;
    VulkanNrSlot nr;
    VulkanDispatch composite_dispatch,motion_dispatch;
    std::array<VkImageView,3> game_views{};
    std::shared_ptr<Feature> feature;
    void clear_bindings(const VulkanDeviceApi& a) {for(auto& view:game_views) {if(view)a.DestroyImageView(a.device,view,nullptr);view={};}feature.reset();peripheral.feature.reset();nr.feature.reset();}
    void destroy(const VulkanDeviceApi& a) {clear_bindings(a);output.destroy(a);motion.destroy(a);composite_dispatch.destroy(a);motion_dispatch.destroy(a);peripheral.destroy(a);nr.destroy(a);}
};
struct View {
    std::shared_ptr<VulkanDeviceApi> api;
    std::shared_ptr<Feature> feature;
    VulkanPeripheralHistory peripheral;
    VulkanNrHistory nr;
    std::deque<Slot> slots;
    unsigned diagnostic_frames{};
};
struct Pipelines {
    std::shared_ptr<VulkanDeviceApi> api;
    VulkanPeripheralPipelines peripheral;
    VulkanNrPipelines nr;
    VulkanCompute composite16,composite32,composite8,motion;
    bool ready{};
    bool initialize(const VulkanDeviceApi& a) {
        if(ready)return true;
        ready=composite16.create(a,vulkan_shaders::composite16,2,"CompositeMain") &&
            composite32.create(a,vulkan_shaders::composite32,2,"CompositeMain") &&
            composite8.create(a,vulkan_shaders::composite8,2,"CompositeMain") && motion.create(a,vulkan_shaders::motion,1,"main");
        if(!ready)destroy(a);return ready;
    }
    void destroy(const VulkanDeviceApi& a){composite16.destroy(a);composite32.destroy(a);composite8.destroy(a);motion.destroy(a);peripheral.destroy(a);nr.destroy(a);ready=false;}
    VulkanCompute* composite(VkFormat format) {
        if(format==VK_FORMAT_R16G16B16A16_SFLOAT)return &composite16;
        if(format==VK_FORMAT_R32G32B32A32_SFLOAT)return &composite32;
        if(format==VK_FORMAT_R8G8B8A8_UNORM)return &composite8;
        return nullptr;
    }
};
std::mutex mutex;
std::unordered_map<VkDevice,Pipelines> pipelines;
std::unordered_map<DlssViewId,View> views;
std::deque<View> retired;
VulkanBackendStatus stats;
VulkanNgxResource* resource(const NgxParameters* p,const char* name) {
    void* value{};if(!ngx_succeeded(p->Get(name,&value)) || !value)return nullptr;
    auto* r=static_cast<VulkanNgxResource*>(value);
    return r->type==0 && r->resource.image.image && r->resource.image.view?r:nullptr;
}
bool fits(const VulkanImageInfo& i,unsigned x,unsigned y,unsigned w,unsigned h) {
    return i.range.levelCount==1 && i.range.layerCount==1 && w && h &&
        static_cast<std::uint64_t>(x)+w<=i.width && static_cast<std::uint64_t>(y)+h<=i.height;
}
bool view_array(const VulkanDeviceApi& a,const VulkanImageInfo& image,VkImageView& view) {
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};info.image=image.image;info.format=image.format;
    info.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;info.subresourceRange=image.range;
    return a.CreateImageView(a.device,&info,nullptr,&view)==VK_SUCCESS;
}
bool completed(View&,Slot& slot) {
    // A completed submission can still be submitted again. The command buffer
    // reset/free boundary, not a fence/event alone, ends a recording's lifetime.
    return !slot.pending;
}
void collect() {
    for(auto it=retired.begin();it!=retired.end();) {
        bool busy{};for(auto& slot:it->slots)busy=!completed(*it,slot)||busy;
        if(busy){++it;continue;}for(auto& slot:it->slots)slot.destroy(*it->api);it=retired.erase(it);
    }
}
struct Recording {
    Slot& slot; const VulkanDeviceApi& api; VkCommandBuffer cmd;
    std::span<VulkanNgxResource* const> images;
    std::span<const VkImageLayout> layouts;
    bool transitioned{};
    Recording(Slot& s,const VulkanDeviceApi& a,VkCommandBuffer c,
        std::span<VulkanNgxResource* const> i,std::span<const VkImageLayout> l):slot(s),api(a),cmd(c),images(i),layouts(l) {
        slot.pending=true;slot.command=cmd;
    }
    void prepare() {
        for(unsigned i=0;i<images.size();++i)vulkan_barrier(api,cmd,images[i]->resource.image,layouts[i],VK_IMAGE_LAYOUT_GENERAL);
        transitioned=true;
    }
    ~Recording() {
        if(transitioned)for(unsigned i=0;i<images.size();++i)vulkan_barrier(api,cmd,images[i]->resource.image,
            VK_IMAGE_LAYOUT_GENERAL,layouts[i],VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
    }
};
}
VulkanBackendStatus vulkan_backend_status() noexcept {std::lock_guard lock(mutex);return stats;}

bool evaluate_vulkan_backend(VkCommandBuffer cmd,const NgxParameters* original,const DlssFrameContract& input_contract,
    const Settings& settings,const VulkanNgxCallbacks& callbacks,NgxResult& result,const NgxHandle* game_handle,
    NgxProgressCallback progress,const VulkanNgxCallbacks* nr_callbacks) noexcept {
    auto c=input_contract;
    std::lock_guard lock(mutex);
    ++stats.calls;stats.input_width=c.render_width;stats.input_height=c.render_height;stats.output_width=c.output_width;stats.output_height=c.output_height;
    const auto skip=[&](const char* reason){
        ++stats.passthrough;stats.reason=reason;
        // The next frame's vectors refer to a frame our private features did
        // not evaluate. Their histories cannot be resumed across that gap.
        const auto it=views.find(c.view_id);
        if(it!=views.end()) {
            if(it->second.feature)it->second.feature->history=false;
            it->second.nr.valid=false;it->second.peripheral.valid=false;
        }
        if(settings.enabled || settings.nr_enabled) {
            static std::uint64_t logged_skips{};const auto n=++logged_skips;
            if(n<=16 || (n&(n-1))==0)
                trace_event("Vulkan bypass view=%llu cmd=%p count=%llu active=%llu bypass=%llu recordings=%zu reason=%s",
                    static_cast<unsigned long long>(c.view_id),cmd,n,stats.active,stats.passthrough,
                    it==views.end()?0U:it->second.slots.size(),reason);
        }
        return false;
    };
    bool handled{};
    try {
        collect();
        if(!settings.enabled && !settings.nr_enabled) {
            const auto view=views.find(c.view_id);if(view!=views.end()) {
                if(view->second.feature)view->second.feature->history=false;
                view->second.nr.valid=false;view->second.peripheral.valid=false;
            }
            return skip("Foveation and NR disabled");
        }
        const auto a=vulkan_command_device(cmd);if(!a)return skip("Vulkan layer did not observe this command buffer");
        auto* color=resource(original,"Color");auto* depth=resource(original,"Depth");
        auto* motion=resource(original,"MotionVectors");auto* output=resource(original,"Output");
        if(!color || !depth || !motion || !output)return skip("Missing Vulkan DLSS image metadata");
        const std::array<VulkanNgxResource*,4> input={color,depth,motion,output};
        for(unsigned i=0;i<input.size();++i)for(unsigned j=0;j<i;++j)
            if(input[i]->resource.image.image==input[j]->resource.image.image)
                return skip("Aliased Vulkan DLSS images are unsupported");
        std::array<VkImageLayout,4> layouts{};
        for(unsigned i=0;i<input.size();++i)if(!vulkan_image_layout(cmd,input[i]->resource.image,layouts[i]))return skip("Unknown Vulkan image layout; preserving game DLSS");
        if(!fits(color->resource.image,c.color_base_x,c.color_base_y,c.render_width,c.render_height) ||
            !fits(depth->resource.image,c.depth_base_x,c.depth_base_y,c.render_width,c.render_height) ||
            !fits(output->resource.image,c.output_base_x,c.output_base_y,c.output_width,c.output_height) || !output->read_write)
            return skip("Unsupported Vulkan image subrectangle");
        auto effective=settings_for_view(settings,c.view_id);
        CropGeometry crop{};bool gaze_reset{};FoveationCenter center{};
        auto coverage=settings;coverage.enabled=true;
        if(!calculate_coordinated_crop(coverage,c.view_id,nullptr,c.render_width,c.render_height,c.output_width,c.output_height,
                c.output_base_x,c.output_base_y,crop,gaze_reset,nullptr,&center,reinterpret_cast<std::uint64_t>(output->resource.image.image)))return skip("Cannot resolve Vulkan crop");
        if(uses_coordinated_center(settings)) {const auto offsets=foveation_offsets_from_geometry(crop,c.render_width,c.render_height);effective.x_offset=offsets.x;effective.height_offset=offsets.y;apply_next_jump_preview(effective,c.view_id);}
        const auto reconstruction=supersampled_crop(crop,settings.center_supersampling);
        auto& gpu=pipelines[a->device];gpu.api=a;if(settings.enabled && !gpu.initialize(*a))return skip("Vulkan compute pipeline creation failed");
        auto* composite=gpu.composite(output->resource.image.format);if(!composite)return skip("Unsupported Vulkan output format");
        auto& view=views[c.view_id];if(view.api && view.api->device!=a->device){retired.push_back(std::move(view));view={};}view.api=a;
        Slot* slot{};for(auto& candidate:view.slots)if(completed(view,candidate)){slot=&candidate;break;}
        if(!slot) {
            // Recordings belong to worker-thread command pools, not just the
            // frames queued on the GPU. Mono sends their whole rotation to one
            // DLSS view, so an eight-slot limit periodically bypassed SR.
            // Grow on demand, retaining resources until reset/free as before.
            constexpr std::size_t max_recordings=64;
            if(view.slots.size()>=max_recordings)return skip("Vulkan recorded command resource limit");
            view.slots.emplace_back();slot=&view.slots.back();
            if(view.slots.size()==9 || view.slots.size()==17 || view.slots.size()==33)
                trace_event("Vulkan recording pool grew view=%llu recordings=%zu",static_cast<unsigned long long>(c.view_id),view.slots.size());
        }
        slot->clear_bindings(*a);
        const auto mv=resolve_motion_region(true,c.create_flags,true,motion->resource.image.width,motion->resource.image.height,c.mv_base_x,c.mv_base_y,crop,
            c.render_width,c.render_height,c.output_width,c.output_height,c.output_base_x,c.output_base_y);
        if(!mv.valid())return skip("Invalid Vulkan motion-vector crop");
        Recording recording(*slot,*a,cmd,input,layouts);
        recording.prepare();
        VulkanNgxCallbacks nr_api{};
        if(settings.nr_enabled) {
            if(nr_callbacks)nr_api=*nr_callbacks;
            else vulkan_nr_runtime(*a,nr_api,stats.nr_result);
        }else view.nr.valid=false;
        NgxParameterOverlay render_parameters(original);
        const bool before=settings.nr_processing_order==NrProcessingOrder::before_upscaling;
        if(settings.nr_enabled && before) {
            auto nr_contract=c;nr_contract.reset|=gaze_reset;
            if(vulkan_nr(*a,cmd,nr_contract,effective,&crop,&center,*color,*depth,*motion,gpu.nr,slot->nr,view.nr,nr_api,stats.nr_result))++stats.nr_active;
            if(slot->nr.produced) {
                color=&slot->nr.processed.ngx;c.color_base_x=c.color_base_y=0;
                render_parameters.Set("Color",static_cast<void*>(color));
                render_parameters.Set("DLSS.Input.Color.Subrect.Base.X",0U);render_parameters.Set("DLSS.Input.Color.Subrect.Base.Y",0U);
                original=&render_parameters;
            }
        }
        const auto center_evaluation=[&]() -> bool {
        if(!settings.enabled){if(view.feature)view.feature->history=false;view.peripheral.valid=false;return false;}
        const FeatureKey key{reconstruction.input_width,reconstruction.input_height,reconstruction.output_width,reconstruction.output_height,c.create_flags,c.perf_quality,effective.center_preset};
        const unsigned motion_width=c.motion_vectors_low_res?mv.rectangle.width:key.out_width;
        const unsigned motion_height=c.motion_vectors_low_res?mv.rectangle.height:key.out_height;
        const bool changed=!view.feature || !(view.feature->key==key);
        if(!slot->output.create(*a,key.out_width,key.out_height,output->resource.image.format) ||
            !slot->composite_dispatch.create(*a,*composite,sizeof(CompositeConstants)) ||
            !view_array(*a,color->resource.image,slot->game_views[0]) || !view_array(*a,output->resource.image,slot->game_views[1]))
            return skip("Vulkan frame resource creation failed");
        if(changed) {
            auto feature=std::make_shared<Feature>();feature->key=key;feature->release=callbacks.release;
            NgxParameterOverlay parameters(original);vulkan_feature_parameters(parameters,key);
            slot->feature=feature;
            result=vulkan_create_feature(*a,cmd,c.feature_id,parameters,*feature,callbacks);
            if(!ngx_succeeded(result) || !feature->handle){++stats.failed;stats.last_result=result;return skip("Vulkan private DLSS feature creation failed");}
            view.feature=feature;++stats.allocations;
        }
        slot->feature=view.feature;auto& history=*view.feature;
        bool reset=c.reset || gaze_reset || !history.history;
        CropMotionOffset offset{};
        if(history.history && !reset && !crop_motion_offset(history.previous,crop,c.motion_vectors_low_res,c.motion_vector_scale_x,c.motion_vector_scale_y,offset))reset=true;
        if(reset)offset={};
        // Keep the game's original view and vector representation unless crop
        // movement or output-space supersampling actually requires conversion.
        // This also preserves view swizzles and other layers' resource identity.
        const bool convert_motion=offset.x!=0.0F || offset.y!=0.0F ||
            motion_width!=mv.rectangle.width || motion_height!=mv.rectangle.height;
        if(convert_motion && (!slot->motion.create(*a,motion_width,motion_height,VK_FORMAT_R32G32_SFLOAT) ||
            !slot->motion_dispatch.create(*a,gpu.motion,32) || !view_array(*a,motion->resource.image,slot->game_views[2])))
            return skip("Vulkan motion-vector conversion creation failed");
        // Every incoming image is restored before the game resumes. Only our
        // own images remain in GENERAL between submissions.
        vulkan_prepare_image(*a,cmd,slot->output);
        if(convert_motion) {
            vulkan_prepare_image(*a,cmd,slot->motion);
            struct MotionConstants {unsigned base[2],size[2];float offset[2];unsigned source[2];};
            const MotionConstants motion_constants{{mv.rectangle.x,mv.rectangle.y},{motion_width,motion_height},{offset.x,offset.y},{mv.rectangle.width,mv.rectangle.height}};
            const VkImageView motion_inputs[]={slot->game_views[2]};
            slot->motion_dispatch.record(*a,cmd,gpu.motion,motion_inputs,slot->motion.ngx.resource.image.view,&motion_constants,sizeof(motion_constants),
                (motion_width+7)/8,(motion_height+7)/8);
            vulkan_barrier(*a,cmd,slot->motion.ngx.resource.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
        }
        NgxParameterOverlay parameters(original);vulkan_feature_parameters(parameters,key);
        parameters.Set("Output",static_cast<void*>(&slot->output.ngx));
        if(convert_motion)parameters.Set("MotionVectors",static_cast<void*>(&slot->motion.ngx));
        parameters.Set("DLSS.Input.Color.Subrect.Base.X",c.color_base_x+crop.input_base_x);parameters.Set("DLSS.Input.Color.Subrect.Base.Y",c.color_base_y+crop.input_base_y);
        parameters.Set("DLSS.Input.Depth.Subrect.Base.X",c.depth_base_x+crop.input_base_x);parameters.Set("DLSS.Input.Depth.Subrect.Base.Y",c.depth_base_y+crop.input_base_y);
        parameters.Set("DLSS.Input.MV.Subrect.Base.X",convert_motion?0U:mv.rectangle.x);
        parameters.Set("DLSS.Input.MV.Subrect.Base.Y",convert_motion?0U:mv.rectangle.y);
        parameters.Set("DLSS.Enable.Output.Subrects",0U);
        parameters.Set("DLSS.Output.Subrect.Base.X",0U);parameters.Set("DLSS.Output.Subrect.Base.Y",0U);parameters.Set("Reset",reset?1U:0U);
        const auto diagnostic_frame=view.diagnostic_frames++;
        if(diagnostic_frame<4 || (diagnostic_frame<14400 && diagnostic_frame%600==0))
            trace_event("Vulkan SR motion view=%llu frame=%u input=%ux%u output=%ux%u crop=%u,%u %ux%u flags=0x%X MV=%ux%u fmt=%u base=%u,%u scale=%.6g,%.6g jitter=%.6g,%.6g reset=%u path=%s recordings=%zu calls=%llu active=%llu bypass=%llu",
                static_cast<unsigned long long>(c.view_id),diagnostic_frame,c.render_width,c.render_height,c.output_width,c.output_height,
                crop.input_base_x,crop.input_base_y,crop.input_width,crop.input_height,c.create_flags,
                motion->resource.image.width,motion->resource.image.height,static_cast<unsigned>(motion->resource.image.format),
                mv.rectangle.x,mv.rectangle.y,c.motion_vector_scale_x,c.motion_vector_scale_y,c.jitter_x,c.jitter_y,reset,
                convert_motion?"converted":"original",view.slots.size(),stats.calls,stats.active,stats.passthrough);
        VulkanNgxScope private_call;
        result=callbacks.evaluate(cmd,history.handle,&parameters,nullptr);stats.last_result=result;
        if(ngx_succeeded(result)) {
            vulkan_barrier(*a,cmd,slot->output.ngx.resource.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
            bool peripheral_active{};
            if(settings.peripheral_dlaa_enabled) {
                NgxResult peripheral_result{};
                peripheral_active=vulkan_peripheral(*a,cmd,original,c,effective,callbacks,gpu.peripheral,slot->peripheral,
                    view.peripheral,*color,*depth,*motion,c.reset,peripheral_result);
                if(!peripheral_active)stats.peripheral_result=peripheral_result;
            }else view.peripheral.valid=false;
            const auto& base=peripheral_active?slot->peripheral.output.ngx.resource.image:color->resource.image;
            CompositeConstants constants{{c.output_width,c.output_height},{c.output_base_x,c.output_base_y},{peripheral_active?0U:c.color_base_x,peripheral_active?0U:c.color_base_y},
                {peripheral_active?base.width:c.render_width,peripheral_active?base.height:c.render_height},
                {crop.output_base_x,crop.output_base_y},{crop.output_width,crop.output_height},effective.width,effective.height,effective.x_offset,effective.height_offset,
                effective.roundness,effective.transition_width,{0,0},effective.alignment_border_enabled?1U:0U,effective.next_jump_offset_x,effective.next_jump_offset_y,
                effective.next_jump_visible?1U:0U,effective.next_jump_width,effective.next_jump_height,0,0,{}};
            const VkImageView images[]={peripheral_active?slot->peripheral.output.array_view:slot->game_views[0],slot->output.array_view};
            slot->composite_dispatch.record(*a,cmd,*composite,images,slot->game_views[1],&constants,sizeof(constants),(c.output_width+15)/16,(c.output_height+15)/16);
            history.previous=crop;history.history=true;++stats.active;if(peripheral_active)++stats.peripheral_active;stats.reason="Native Vulkan foveated DLSS active";
            note_stereo_view_geometry(c.view_id,c.render_width,c.render_height,c.output_width,c.output_height,crop);
        }else {history.history=false;++stats.failed;stats.reason="Vulkan private evaluation failed; restoring game DLSS";}
        return ngx_succeeded(result);
        };
        handled=center_evaluation();
        if(!handled && game_handle) {
            VulkanNgxScope private_call;
            // The original NGX evaluation executes once, including NR-only mode.
            result=callbacks.evaluate(cmd,game_handle,original,progress);handled=true;
            stats.reason=settings.enabled?"Game DLSS fallback":"Game DLSS with native Vulkan NR";
        }
        if(handled && ngx_succeeded(result) && settings.nr_enabled && !before) {
            vulkan_barrier(*a,cmd,output->resource.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
            auto nr_contract=input_contract;nr_contract.reset|=gaze_reset;
            if(vulkan_nr(*a,cmd,nr_contract,effective,&crop,&center,*output,*depth,*motion,gpu.nr,slot->nr,view.nr,nr_api,stats.nr_result))++stats.nr_active;
        }
        return handled;
    }catch(...){++stats.failed;stats.reason="Vulkan backend exception";return handled;}
}
void vulkan_release_view(DlssViewId id) noexcept {
    std::lock_guard lock(mutex);const auto it=views.find(id);if(it!=views.end()){retired.push_back(std::move(it->second));views.erase(it);}collect();forget_gaze_view(id);
}
void vulkan_backend_forget_command(VkCommandBuffer cmd) noexcept {
    std::lock_guard lock(mutex);
    const auto reset=[&](View& view){for(auto& slot:view.slots)if(slot.command==cmd){slot.pending=false;slot.command={};slot.clear_bindings(*view.api);}};
    for(auto& [_,view]:views)reset(view);for(auto& view:retired)reset(view);collect();
}
void vulkan_backend_release_device(VkDevice device) noexcept {
    std::lock_guard lock(mutex);
    // Device destruction is an explicit lifetime boundary; the application is
    // required to have completed its work before destroying the device.
    const auto destroy=[](View& view){for(auto& slot:view.slots)slot.destroy(*view.api);view.feature.reset();view.peripheral.feature.reset();view.nr.feature.reset();};
    for(auto it=views.begin();it!=views.end();) {if(it->second.api->device==device){destroy(it->second);it=views.erase(it);}else ++it;}
    for(auto it=retired.begin();it!=retired.end();) {if(it->api->device==device){destroy(*it);it=retired.erase(it);}else ++it;}
    auto p=pipelines.find(device);if(p!=pipelines.end()) {
        // Dispatch is retained by the observer until this callback completes.
        // Pipelines are released by the owning device in the observer callback.
        p->second.destroy(*p->second.api);pipelines.erase(p);
    }
    vulkan_nr_release_device(device);
}
}
