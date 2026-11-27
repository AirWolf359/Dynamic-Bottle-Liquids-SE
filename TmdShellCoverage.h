#pragma once
#include "BottleHookState.h"

struct alignas(16) TmdShellConstants {
    float optical[4]{};
    float viewport[4]{};
    float projectionDepth{};
    float padding[3]{};
};
static_assert(sizeof(TmdShellConstants)==48);

template<class Draw>
void DrawTmdShellCoverage(ID3D11DeviceContext* ctx, ID3D11Buffer* buffer,
                          ID3D11PixelShader* shader, float opacity,
                          Draw draw, UINT count, UINT start, INT base){
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11DepthStencilState> original;
    UINT stencil=0;
    ctx->OMGetDepthStencilState(&original,&stencil);
    if(!original)return;
    D3D11_DEPTH_STENCIL_DESC desc{};
    original->GetDesc(&desc);
    if(!desc.DepthEnable||desc.StencilEnable)return;

    static ComPtr<ID3D11DepthStencilState> source,writing;
    if(source!=original){
        ComPtr<ID3D11Device> device;
        ctx->GetDevice(&device);
        desc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
        ComPtr<ID3D11DepthStencilState> next;
        if(FAILED(device->CreateDepthStencilState(&desc,&next)))return;
        source=original;
        writing=next;
    }
    ID3D11RenderTargetView* targets[8]{};
    ComPtr<ID3D11DepthStencilView> depth;
    ctx->OMGetRenderTargets(8,targets,&depth);
    if(depth){
        const TmdShellConstants values{{opacity,0,0,0}};
        ctx->UpdateSubresource(buffer,0,nullptr,&values,0,0);
        BottleConstantBinding constants(ctx,nullptr,buffer);
        ComPtr<ID3D11PixelShader> previous;
        ctx->PSGetShader(&previous,nullptr,nullptr);
        ctx->OMSetRenderTargets(0,nullptr,depth.Get());
        ctx->OMSetDepthStencilState(writing.Get(),stencil);
        ctx->PSSetShader(shader,nullptr,0);
        draw(ctx,count,start,base);
        ctx->PSSetShader(previous.Get(),nullptr,0);
        ctx->OMSetDepthStencilState(original.Get(),stencil);
        ctx->OMSetRenderTargets(8,targets,depth.Get());
    }
    for(auto* target:targets)if(target)target->Release();
}
