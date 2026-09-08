#pragma once
#include "ngx_abi.hpp"
#include <string>
#include <unordered_map>
#include <variant>

// Local test double; no NVIDIA code or runtime is used by these fixtures.
struct MockNgxParameters : cheeky::foveated_dlss::NgxParameters {
    using Value = std::variant<unsigned long long, float, double, unsigned int, int,
        ID3D11Resource*, ID3D12Resource*, void*>;
    std::unordered_map<std::string, Value> values;
    template<class T> void put(const char* key, T v) { values.insert_or_assign(key, v); }
    template<class T> std::uint32_t read(const char* key, T* out) const {
        const auto it=values.find(key); if(it==values.end() || !out) return 0xBAD00007U;
        bool found=false;
        std::visit([&](const auto& v) {
            using V=std::decay_t<decltype(v)>;
            if constexpr(std::is_same_v<V,T>) { *out=v; found=true; }
            else if constexpr(std::is_arithmetic_v<V> && std::is_arithmetic_v<T>) { *out=static_cast<T>(v); found=true; }
        },it->second);
        return found ? 1U : 0xBAD00007U;
    }
#define MOCK_NGX_TYPE(T) \
    void Set(const char* key,T v) override { put(key,v); } \
    std::uint32_t Get(const char* key,T* out) const override { return read(key,out); }
    MOCK_NGX_TYPE(unsigned long long)
    MOCK_NGX_TYPE(float)
    MOCK_NGX_TYPE(double)
    MOCK_NGX_TYPE(unsigned int)
    MOCK_NGX_TYPE(int)
    MOCK_NGX_TYPE(ID3D11Resource*)
    MOCK_NGX_TYPE(ID3D12Resource*)
    MOCK_NGX_TYPE(void*)
#undef MOCK_NGX_TYPE
    void Reset() override { values.clear(); }
};
