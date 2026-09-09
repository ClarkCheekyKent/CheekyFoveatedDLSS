#pragma once
#include <d3d12.h>
#include <array>
#include <cstdlib>

// Narrow COM-ABI forwarding facade for native-passthrough timing tests. Its
// interface address differs from the list actually submitted to the queue.
// Indices are from ID3D12GraphicsCommandListVtbl in the Windows SDK; unneeded
// rendering methods abort so accidental use outside this fixture is visible.
struct TimingListAlias {
    void** vtable;
    ID3D12GraphicsCommandList* target;
    std::array<void*, 60> methods;
    unsigned references{};
    static void STDMETHODCALLTYPE unexpected(TimingListAlias*) { std::abort(); }
    static ULONG STDMETHODCALLTYPE addref(TimingListAlias* self) { ++self->references; return self->target->AddRef(); }
    static ULONG STDMETHODCALLTYPE release(TimingListAlias* self) { --self->references; return self->target->Release(); }
    static HRESULT STDMETHODCALLTYPE query(TimingListAlias* self, REFIID iid, void** out) {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D12GraphicsCommandList)) return E_NOINTERFACE;
        *out = self; addref(self); return S_OK;
    }
    static HRESULT STDMETHODCALLTYPE get_private(TimingListAlias* self, REFGUID key, UINT* size, void* data) { return self->target->GetPrivateData(key, size, data); }
    static HRESULT STDMETHODCALLTYPE set(TimingListAlias* self, REFGUID key, UINT size, const void* data) { return self->target->SetPrivateData(key, size, data); }
    static HRESULT STDMETHODCALLTYPE device(TimingListAlias* self, REFIID iid, void** out) { return self->target->GetDevice(iid, out); }
    static void STDMETHODCALLTYPE end(TimingListAlias* self, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) { self->target->EndQuery(heap, type, index); }
    static void STDMETHODCALLTYPE resolve(TimingListAlias* self, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT first, UINT count, ID3D12Resource* out, UINT64 offset) {
        self->target->ResolveQueryData(heap, type, first, count, out, offset);
    }
    explicit TimingListAlias(ID3D12GraphicsCommandList* list) : vtable(methods.data()), target(list) {
        methods.fill(reinterpret_cast<void*>(&unexpected));
        methods[0] = reinterpret_cast<void*>(&query); methods[1] = reinterpret_cast<void*>(&addref); methods[2] = reinterpret_cast<void*>(&release);
        methods[3] = reinterpret_cast<void*>(&get_private); methods[4] = reinterpret_cast<void*>(&set); methods[7] = reinterpret_cast<void*>(&device);
        methods[53] = reinterpret_cast<void*>(&end); methods[54] = reinterpret_cast<void*>(&resolve);
    }
    ID3D12GraphicsCommandList* get() { return reinterpret_cast<ID3D12GraphicsCommandList*>(this); }
};
