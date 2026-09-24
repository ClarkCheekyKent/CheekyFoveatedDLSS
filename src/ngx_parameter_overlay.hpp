#pragma once
#include "ngx_abi.hpp"
#include <array>
#include <cstring>
#include <type_traits>

namespace cheeky::foveated_dlss {
// A bounded, allocation-free parameter overlay. The game's map is read-only;
// private evaluations cannot leave cropped extents or replacement images in it.
class NgxParameterOverlay final : public NgxParameters {
    enum class Kind { ui, integer, ull, real, wide_real, pointer, d11, d12 };
    struct Value { const char* name{}; Kind kind{}; union { unsigned ui; int integer; unsigned long long ull; float real; double wide_real; void* pointer; } data{}; };
    std::array<Value,128> values_{};
    unsigned count_{};
    const NgxParameters* fallback_{};
    Value* find(const char* name) noexcept { for(unsigned i=0;i<count_;++i) if(std::strcmp(name,values_[i].name)==0) return &values_[i];return nullptr; }
    const Value* find(const char* name) const noexcept { return const_cast<NgxParameterOverlay*>(this)->find(name); }
    Value* put(const char* name,Kind kind) noexcept {auto* v=find(name);if(!v && count_<values_.size()) v=&values_[count_++];if(v){v->name=name;v->kind=kind;}return v;}
    template<class T> NgxResult numeric(const char* name,T* out) const {
        const auto* v=find(name);if(!v) return fallback_?fallback_->Get(name,out):0xBAD00005U;
        switch(v->kind) {
        case Kind::ui:*out=static_cast<T>(v->data.ui);break; case Kind::integer:*out=static_cast<T>(v->data.integer);break;
        case Kind::ull:*out=static_cast<T>(v->data.ull);break;case Kind::real:*out=static_cast<T>(v->data.real);break;
        case Kind::wide_real:*out=static_cast<T>(v->data.wide_real);break;default:return 0xBAD00005U;
        }return 1U;
    }
public:
    explicit NgxParameterOverlay(const NgxParameters* fallback=nullptr) noexcept:fallback_(fallback){}
    void Set(const char* n,unsigned v) override {if(auto* p=put(n,Kind::ui))p->data.ui=v;}
    void Set(const char* n,int v) override {if(auto* p=put(n,Kind::integer))p->data.integer=v;}
    void Set(const char* n,unsigned long long v) override {if(auto* p=put(n,Kind::ull))p->data.ull=v;}
    void Set(const char* n,float v) override {if(auto* p=put(n,Kind::real))p->data.real=v;}
    void Set(const char* n,double v) override {if(auto* p=put(n,Kind::wide_real))p->data.wide_real=v;}
    void Set(const char* n,void* v) override {if(auto* p=put(n,Kind::pointer))p->data.pointer=v;}
    void Set(const char* n,ID3D11Resource* v) override {if(auto* p=put(n,Kind::d11))p->data.pointer=v;}
    void Set(const char* n,ID3D12Resource* v) override {if(auto* p=put(n,Kind::d12))p->data.pointer=v;}
    NgxResult Get(const char* n,unsigned* v) const override {return numeric(n,v);}
    NgxResult Get(const char* n,int* v) const override {return numeric(n,v);}
    NgxResult Get(const char* n,unsigned long long* v) const override {return numeric(n,v);}
    NgxResult Get(const char* n,float* v) const override {return numeric(n,v);}
    NgxResult Get(const char* n,double* v) const override {return numeric(n,v);}
    NgxResult Get(const char* n,void** v) const override {const auto* p=find(n);if(!p)return fallback_?fallback_->Get(n,v):0xBAD00005U;if(p->kind!=Kind::pointer)return 0xBAD00005U;*v=p->data.pointer;return 1U;}
    NgxResult Get(const char* n,ID3D11Resource** v) const override {const auto* p=find(n);if(!p)return fallback_?fallback_->Get(n,v):0xBAD00005U;if(p->kind!=Kind::d11)return 0xBAD00005U;*v=static_cast<ID3D11Resource*>(p->data.pointer);return 1U;}
    NgxResult Get(const char* n,ID3D12Resource** v) const override {const auto* p=find(n);if(!p)return fallback_?fallback_->Get(n,v):0xBAD00005U;if(p->kind!=Kind::d12)return 0xBAD00005U;*v=static_cast<ID3D12Resource*>(p->data.pointer);return 1U;}
    void Reset() override {count_=0;}
};
}
