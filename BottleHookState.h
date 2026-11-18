#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>

inline bool IsGamePresentation(IDXGISwapChain* submitted, IDXGISwapChain* primary, UINT flags)
{
    return submitted && submitted == primary && !(flags & DXGI_PRESENT_TEST);
}

class BottleConstantBinding {
    ID3D11DeviceContext* context;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> ranged;
    Microsoft::WRL::ComPtr<ID3D11Buffer> previous;
    UINT first = 0, count = 0;
public:
    BottleConstantBinding(ID3D11DeviceContext* ctx, ID3D11DeviceContext1* known, ID3D11Buffer* bottle)
        : context(ctx), ranged(known)
    {
        if (!ranged) context->QueryInterface(IID_PPV_ARGS(&ranged));
        if (ranged) ranged->PSGetConstantBuffers1(10, 1, &previous, &first, &count);
        else context->PSGetConstantBuffers(10, 1, &previous);
        context->PSSetConstantBuffers(10, 1, &bottle);
    }

    ~BottleConstantBinding()
    {
        auto* buffer = previous.Get();
        if (ranged) ranged->PSSetConstantBuffers1(10, 1, &buffer, &first, &count);
        else context->PSSetConstantBuffers(10, 1, &buffer);
    }
};
