#pragma once
// Pair the liquid draw with the original transparent glass draw. Never write
// auxiliary ENB/CS targets. Depth is read-only by default; the opt-in opaque
// liquid-entry shader writes physical liquid depth to occlude inner glass.
struct NativeGlassBinding {
    ID3D11DeviceContext* context;
    ID3D11RenderTargetView* targets[8]{};
    ID3D11DepthStencilView* depth=nullptr;
    ComPtr<ID3D11ShaderResourceView> diffuse;
    bool bound=false;
    explicit NativeGlassBinding(ID3D11DeviceContext* c):context(c){}
    static bool Eligible(ID3D11DeviceContext* c,D3D11_RENDER_TARGET_VIEW_DESC& desc){
        ComPtr<ID3D11BlendState> blend;FLOAT factor[4];UINT mask;
        c->OMGetBlendState(&blend,factor,&mask);if(!blend)return false;
        D3D11_BLEND_DESC b{};blend->GetDesc(&b);if(!b.RenderTarget[0].BlendEnable)return false;
        ComPtr<ID3D11RenderTargetView> target;c->OMGetRenderTargets(1,&target,nullptr);if(!target)return false;
        target->GetDesc(&desc);
        return desc.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D;
    }
    void Bind(ID3D11ShaderResourceView* liquid){
        context->OMGetRenderTargets(8,targets,&depth);
        context->PSGetShaderResources(0,1,&diffuse);
        context->OMSetRenderTargets(1,targets,depth);
        context->PSSetShaderResources(0,1,&liquid);bound=true;
    }
    ~NativeGlassBinding(){if(!bound)return;
        auto* original=diffuse.Get();context->PSSetShaderResources(0,1,&original);
        context->OMSetRenderTargets(8,targets,depth);
        for(auto* t:targets)if(t)t->Release();if(depth)depth->Release();
    }
};
