#pragma once
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include "runtime.hpp"

namespace cheeky::foveated_dlss {
// Bytecode is device-independent. Cache successful compiles only; descriptor
// and texture turnover must never compile NR's unchanged shaders again.
inline HRESULT compile_nr_shader(const char* source, size_t length, const char* name,
    const D3D_SHADER_MACRO* defines, ID3DInclude* include, const char* entry, const char* target,
    UINT flags, UINT flags2, ID3DBlob** code, ID3DBlob** errors) {
    if (defines || include) return D3DCompile(source, length, name, defines, include, entry, target, flags, flags2, code, errors);
    using Key = std::tuple<std::string, std::string, std::string, UINT, UINT>;
    static std::mutex mutex;
    static std::map<Key, Microsoft::WRL::ComPtr<ID3DBlob>> cache;
    std::lock_guard lock(mutex);
    Key key{std::string(source, length), entry, target, flags, flags2};
    if (auto it = cache.find(key); it != cache.end()) {
        if (errors) *errors = nullptr;
        return it->second.CopyTo(code);
    }
    const auto result = D3DCompile(source, length, name, nullptr, nullptr, entry, target, flags, flags2, code, errors);
    if (FAILED(result)) {
        static unsigned failures{}; // Protected by the cache mutex.
        const auto count=++failures;
        if(count<=4 || (count & (count-1))==0) {
            auto* blob=errors ? *errors : nullptr;
            trace_event("SHADER_COMPILE NR name=%s entry=%s target=%s hr=0x%08X failures=%u error=%.*s",
                name ? name : "",entry,target,unsigned(result),count,
                blob ? static_cast<int>((std::min)(blob->GetBufferSize(),SIZE_T{2048})) : 0,
                blob ? static_cast<const char*>(blob->GetBufferPointer()) : "");
        }
    }
    if (SUCCEEDED(result)) cache.emplace(std::move(key), *code);
    return result;
}
}
