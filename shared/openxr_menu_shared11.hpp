#pragma once
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace cheeky::xr_menu {
// Called under the desktop renderer lock. Key 0 belongs to the producer;
// key 1 belongs to OpenXR. The consumer must release key 0 after its GPU copy.
struct SharedTexture11 {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> writer, reader;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> writer_mutex, reader_mutex;
    Microsoft::WRL::ComPtr<ID3D11Device> reader_device;
    bool pending{};

    HRESULT initialize(ID3D11Device* producer, ID3D11Device* consumer, ID3D11Texture2D* source) {
        *this = {};
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        auto hr = producer->CreateTexture2D(&desc, nullptr, &writer);
        if (FAILED(hr)) return hr;
        Microsoft::WRL::ComPtr<IDXGIResource1> resource;
        Microsoft::WRL::ComPtr<ID3D11Device1> device1;
        if (FAILED(hr = writer.As(&resource)) || FAILED(hr = consumer->QueryInterface(IID_PPV_ARGS(&device1)))) return hr;
        HANDLE handle{};
        hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
        if (FAILED(hr)) return hr;
        hr = device1->OpenSharedResource1(handle, IID_PPV_ARGS(&reader));
        CloseHandle(handle);
        if (FAILED(hr)) return hr;
        if (FAILED(hr = writer.As(&writer_mutex)) || FAILED(hr = reader.As(&reader_mutex))) return hr;
        reader_device = consumer;
        return S_OK;
    }

    HRESULT publish(ID3D11DeviceContext* context, ID3D11Texture2D* source) {
        if (!reader || pending) return S_FALSE;
        const auto hr = writer_mutex->AcquireSync(0, 0);
        if (hr == WAIT_TIMEOUT) return S_FALSE;
        if (hr != S_OK) return FAILED(hr) ? hr : E_FAIL;
        context->CopyResource(writer.Get(), source);
        const auto released = writer_mutex->ReleaseSync(1);
        context->Flush();
        if (FAILED(released)) return released;
        pending = true;
        return S_OK;
    }

    HRESULT acquire(ID3D11Texture2D** texture, IDXGIKeyedMutex** mutex) {
        if (!pending) return S_FALSE;
        const auto hr = reader_mutex->AcquireSync(1, 0);
        if (hr == WAIT_TIMEOUT) return S_FALSE;
        if (hr != S_OK) return FAILED(hr) ? hr : E_FAIL;
        pending = false;
        reader.CopyTo(texture);
        reader_mutex.CopyTo(mutex);
        return S_OK;
    }
};
}
