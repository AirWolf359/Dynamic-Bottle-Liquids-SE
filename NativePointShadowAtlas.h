#pragma once

#include <d3d11.h>
#include <wrl/client.h>

namespace dbl {
// Utility point shadows use one writable D16 array slice, split into two
// full-width viewports. Use the bound view so renderer wrappers can own it.
inline bool GetNativePointShadowAtlas(ID3D11DeviceContext* context,
                                     Microsoft::WRL::ComPtr<ID3D11DepthStencilView>& depth,
                                     D3D11_VIEWPORT& viewport){
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> color;
    context->OMGetRenderTargets(1,&color,&depth);
    if(color||!depth)return false;

    D3D11_DEPTH_STENCIL_VIEW_DESC view{};
    depth->GetDesc(&view);
    if(view.Format!=DXGI_FORMAT_D16_UNORM||view.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2DARRAY||
       (view.Flags&D3D11_DSV_READ_ONLY_DEPTH)||view.Texture2DArray.ArraySize!=1||
       view.Texture2DArray.MipSlice!=0)return false;

    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    depth->GetResource(&resource);
    if(!resource||FAILED(resource.As(&texture)))return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    constexpr UINT bind=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    if(desc.Format!=DXGI_FORMAT_R16_TYPELESS||desc.ArraySize<2||(desc.BindFlags&bind)!=bind)return false;

    UINT count=1;
    context->RSGetViewports(&count,&viewport);
    const float halfHeight=float(desc.Height)*.5f;
    if(count!=1||viewport.TopLeftX!=0||viewport.Width!=float(desc.Width)||viewport.Height!=halfHeight||
       (viewport.TopLeftY!=0&&viewport.TopLeftY!=halfHeight))return false;

    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> state;
    context->OMGetDepthStencilState(&state,nullptr);
    if(!state)return true; // D3D11 defaults enable depth testing and writing.
    D3D11_DEPTH_STENCIL_DESC stencil{};
    state->GetDesc(&stencil);
    return stencil.DepthEnable&&stencil.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL;
}
}
