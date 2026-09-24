#pragma once
#include <d3d11_1.h>
#include <array>

namespace cheeky::foveated_dlss {
// D3D11 silently replaces an SRV with NULL if its resource is still bound for
// writing. Detach incoming compute/graphics outputs before binding our inputs,
// then restore them after the caller has unbound its private pass resources.
class D3D11WriteBindingsScope {
    ID3D11DeviceContext* context_;
    unsigned slots_{D3D11_PS_CS_UAV_REGISTER_COUNT},rt_count_{};
    std::array<ID3D11UnorderedAccessView*,D3D11_1_UAV_SLOT_COUNT> compute_{},graphics_{};
    std::array<ID3D11RenderTargetView*,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets_{};
    ID3D11DepthStencilView* depth_{};
public:
    explicit D3D11WriteBindingsScope(ID3D11DeviceContext* context) noexcept:context_(context) {
        ID3D11Device* device{};context_->GetDevice(&device);
        if(device){if(device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1)slots_=D3D11_1_UAV_SLOT_COUNT;device->Release();}
        context_->CSGetUnorderedAccessViews(0,slots_,compute_.data());
        context_->OMGetRenderTargets(static_cast<UINT>(targets_.size()),targets_.data(),&depth_);
        context_->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,slots_,graphics_.data());
        for(unsigned i=0;i<targets_.size();++i)if(targets_[i])rt_count_=i+1;
        std::array<ID3D11UnorderedAccessView*,D3D11_1_UAV_SLOT_COUNT> empty{};
        context_->CSSetUnorderedAccessViews(0,slots_,empty.data(),nullptr);
        context_->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,slots_,empty.data(),nullptr);
    }
    ~D3D11WriteBindingsScope() {
        std::array<UINT,D3D11_1_UAV_SLOT_COUNT> counters;counters.fill(UINT(-1));
        context_->CSSetUnorderedAccessViews(0,slots_,compute_.data(),counters.data());
        context_->OMSetRenderTargetsAndUnorderedAccessViews(rt_count_,targets_.data(),depth_,
            rt_count_,slots_-rt_count_,graphics_.data()+rt_count_,counters.data()+rt_count_);
        for(auto* view:compute_)if(view)view->Release();
        for(auto* view:graphics_)if(view)view->Release();
        for(auto* view:targets_)if(view)view->Release();
        if(depth_)depth_->Release();
    }
    D3D11WriteBindingsScope(const D3D11WriteBindingsScope&)=delete;
    D3D11WriteBindingsScope& operator=(const D3D11WriteBindingsScope&)=delete;
};
}
