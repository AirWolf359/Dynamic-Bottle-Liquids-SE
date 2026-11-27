#pragma once
#include <vector>

// Two-sided, depth-writing glass must not blend rear triangles over opaque
// liquid in mesh index order. Keep the native shaders/materials and split only
// the rasterization order: rear glass, liquid, front glass.
struct NativeGlassOrder {
    struct States {
        ComPtr<ID3D11RasterizerState> original, rear, front;
    };
    ComPtr<ID3D11RasterizerState> original, front;
    ID3D11DeviceContext* context=nullptr;
    bool active=false;

    template<class Draw>
    bool Begin(ID3D11DeviceContext* ctx,Draw draw,UINT count,UINT start,INT base,bool drawRear=true){
        ComPtr<ID3D11RasterizerState> state;ctx->RSGetState(&state);
        if(!state)return false; // D3D default already culls rear faces.
        D3D11_RASTERIZER_DESC desc{};state->GetDesc(&desc);
        if(desc.CullMode!=D3D11_CULL_NONE)return false;
        // A small retained-state cache avoids creating raster states per bottle.
        static std::vector<States> cache;
        States* found=nullptr;
        for(auto& entry:cache)if(entry.original.Get()==state.Get()){found=&entry;break;}
        if(!found){
            States entry;entry.original=state;ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
            desc.CullMode=D3D11_CULL_FRONT;
            if(FAILED(device->CreateRasterizerState(&desc,&entry.rear)))return false;
            desc.CullMode=D3D11_CULL_BACK;
            if(FAILED(device->CreateRasterizerState(&desc,&entry.front)))return false;
            if(cache.size()>=16)cache.clear();
            cache.push_back(entry);found=&cache.back();
        }
        original=state;front=found->front;context=ctx;
        if(drawRear){
            ctx->RSSetState(found->rear.Get());
            draw(ctx,count,start,base);
            ctx->RSSetState(original.Get());
        }
        active=true;return true;
    }
    template<class Draw>
    void Finish(Draw draw,UINT count,UINT start,INT base){
        context->RSSetState(front.Get());draw(context,count,start,base);context->RSSetState(original.Get());active=false;
    }
};
