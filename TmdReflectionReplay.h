#pragma once
#include <array>
#include <vector>
#include <wrl/client.h>
#include <d3d11_1.h>
#include "NativeConstantSlice.h"
#include "BottleGlassDepth.h"

// Record the original reflection shader and its inputs at the native draw.
// Replaying the command list restores the immediate context's pipeline state.
class TmdReflectionReplay {
    using Draw = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
    using Execute = void (*)(ID3D11DeviceContext*,ID3D11CommandList*);
    using Buffer = Microsoft::WRL::ComPtr<ID3D11Buffer>;
    static constexpr UINT constantSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
    struct Entry {
        const void* bottle{};
        Microsoft::WRL::ComPtr<ID3D11Resource> target;
        Microsoft::WRL::ComPtr<ID3D11CommandList> commands;
        std::array<Buffer, 2 * constantSlots> copies;
        NativeConstantSlice lighting;
        bool shadowMaskBound{};
        bool visible{};
    };

    template<class T, size_t N>
    struct Bindings {
        T* values[N]{};
        ~Bindings(){ for(auto* value:values) if(value)value->Release(); }
    };

    bool RecordStage(ID3D11DeviceContext* immediate, Entry& entry,bool vertex){
        Bindings<ID3D11Buffer,constantSlots> sources;
        UINT first[constantSlots]{}, counts[constantSlots]{};
        if(immediate1){
            if(vertex) immediate1->VSGetConstantBuffers1(0,constantSlots,sources.values,first,counts);
            else immediate1->PSGetConstantBuffers1(0,constantSlots,sources.values,first,counts);
        }else{
            if(vertex) immediate->VSGetConstantBuffers(0,constantSlots,sources.values);
            else immediate->PSGetConstantBuffers(0,constantSlots,sources.values);
        }
        ID3D11Buffer* buffers[constantSlots]{};
        for(UINT slot=0;slot<constantSlots;++slot){
            auto* source=sources.values[slot];
            if(!source)continue;
            D3D11_BUFFER_DESC desc{}, copyDesc{};
            source->GetDesc(&desc);
            auto& copy=entry.copies[(vertex?0:constantSlots)+slot];
            if(copy)copy->GetDesc(&copyDesc);
            if(copyDesc.ByteWidth!=desc.ByteWidth){
                desc.Usage=D3D11_USAGE_DEFAULT;
                desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
                desc.CPUAccessFlags=desc.MiscFlags=desc.StructureByteStride=0;
                if(FAILED(device->CreateBuffer(&desc,nullptr,copy.ReleaseAndGetAddressOf())))return false;
            }
            immediate->CopyResource(copy.Get(),source);
            buffers[slot]=copy.Get();
        }
        Bindings<ID3D11ShaderResourceView,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> resources;
        Bindings<ID3D11SamplerState,D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers;
        if(vertex){
            if(deferred1)deferred1->VSSetConstantBuffers1(0,constantSlots,buffers,first,counts);
            else deferred->VSSetConstantBuffers(0,constantSlots,buffers);
            immediate->VSGetShaderResources(0,UINT(std::size(resources.values)),resources.values);
            immediate->VSGetSamplers(0,UINT(std::size(samplers.values)),samplers.values);
            deferred->VSSetShaderResources(0,UINT(std::size(resources.values)),resources.values);
            deferred->VSSetSamplers(0,UINT(std::size(samplers.values)),samplers.values);
        }else{
            entry.lighting={entry.copies[constantSlots+2],first[2],
                            immediate1?counts[2]:D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT};
            if(!sources.values[2])entry.lighting={};
            if(deferred1)deferred1->PSSetConstantBuffers1(0,constantSlots,buffers,first,counts);
            else deferred->PSSetConstantBuffers(0,constantSlots,buffers);
            immediate->PSGetShaderResources(0,UINT(std::size(resources.values)),resources.values);
            immediate->PSGetSamplers(0,UINT(std::size(samplers.values)),samplers.values);
            deferred->PSSetShaderResources(0,UINT(std::size(resources.values)),resources.values);
            deferred->PSSetSamplers(0,UINT(std::size(samplers.values)),samplers.values);
        }
        return true;
    }

public:
    explicit TmdReflectionReplay(Execute execute=ExecuteNormally):execute(execute){}

    const NativeConstantSlice* Lighting(ID3D11DeviceContext* immediate,const void* bottle,
                                        bool* shadowMaskBound=nullptr) const {
        if(context.Get()!=immediate)return nullptr;
        for(size_t i=0;i<used;++i){
            const auto& entry=entries[i];
            if(entry.commands&&entry.bottle==bottle&&entry.lighting.Contains(30)){
                if(shadowMaskBound)*shadowMaskBound=entry.shadowMaskBound;
                return &entry.lighting;
            }
        }
        return nullptr;
    }

    bool Capture(ID3D11DeviceContext* immediate,const void* bottle,ID3D11Resource* scene,
                 Draw draw,UINT count,UINT start,INT base,ID3D11ShaderResourceView* sceneShadowMask=nullptr,
                 bool visible=true,bool reverseDepth=false){
        using Microsoft::WRL::ComPtr;
        if(!scene||recording||immediate->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
        ComPtr<ID3D11BlendState> blend;
        FLOAT factors[4]{};
        UINT sampleMask{}, stencil{};
        immediate->OMGetBlendState(&blend,factors,&sampleMask);
        if(!blend)return false;
        D3D11_BLEND_DESC blendDesc{};
        blend->GetDesc(&blendDesc);
        const auto& color=blendDesc.RenderTarget[0];
        if(!color.BlendEnable||color.DestBlend!=D3D11_BLEND_ONE||color.BlendOp!=D3D11_BLEND_OP_ADD||
           (color.SrcBlend!=D3D11_BLEND_SRC_ALPHA&&color.SrcBlend!=D3D11_BLEND_ONE)||
           !(color.RenderTargetWriteMask&7))return false;
        ComPtr<ID3D11DepthStencilState> depthState;
        immediate->OMGetDepthStencilState(&depthState,&stencil);
        if(!depthState)return false;
        D3D11_DEPTH_STENCIL_DESC depthDesc{};
        depthState->GetDesc(&depthDesc);
        if(!depthDesc.DepthEnable||depthDesc.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ZERO||
           depthDesc.DepthFunc!=D3D11_COMPARISON_LESS_EQUAL||depthDesc.StencilEnable)return false;
        Bindings<ID3D11RenderTargetView,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets;
        ComPtr<ID3D11DepthStencilView> depth;
        immediate->OMGetRenderTargets(UINT(std::size(targets.values)),targets.values,&depth);
        if(!targets.values[0]||!depth)return false;
        ComPtr<ID3D11Resource> target;
        targets.values[0]->GetResource(&target);
        if(target.Get()!=scene)return false;

        ComPtr<ID3D11VertexShader> vertexShader;
        ComPtr<ID3D11PixelShader> pixelShader;
        ComPtr<ID3D11GeometryShader> geometryShader;
        ComPtr<ID3D11HullShader> hullShader;
        ComPtr<ID3D11DomainShader> domainShader;
        UINT vertexClasses=0,pixelClasses=0;
        immediate->VSGetShader(&vertexShader,nullptr,&vertexClasses);
        immediate->PSGetShader(&pixelShader,nullptr,&pixelClasses);
        immediate->GSGetShader(&geometryShader,nullptr,nullptr);
        immediate->HSGetShader(&hullShader,nullptr,nullptr);
        immediate->DSGetShader(&domainShader,nullptr,nullptr);
        if(!vertexShader||!pixelShader||geometryShader||hullShader||domainShader||vertexClasses||pixelClasses)return false;

        if(!deferred||context.Get()!=immediate){
            if(used)return false;
            immediate->GetDevice(device.ReleaseAndGetAddressOf());
            if(FAILED(device->CreateDeferredContext(0,deferred.ReleaseAndGetAddressOf())))return false;
            context=immediate;
            deferred1.Reset();immediate1.Reset();
            deferred.As(&deferred1);context.As(&immediate1);
            if(!immediate1)deferred1.Reset();
            entries.clear();
            blendSource.Reset();
            fullyLitMask.Reset();
        }
        // Skyrim's screen-space mask was resolved against the earlier opaque
        // receivers. A transparent bottle has no matching receiver there.
        // Substitute full light for that mask in the reflection replay only.
        constexpr UINT shadowSlot=14;
        ComPtr<ID3D11ShaderResourceView> boundShadow;
        if(sceneShadowMask)immediate->PSGetShaderResources(shadowSlot,1,&boundShadow);
        const bool needsUnshadowedReflection=sceneShadowMask&&boundShadow.Get()==sceneShadowMask;
        if(needsUnshadowedReflection&&!fullyLitMask){
            D3D11_TEXTURE2D_DESC maskDesc{};
            maskDesc.Width=maskDesc.Height=maskDesc.ArraySize=maskDesc.MipLevels=1;
            maskDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            maskDesc.SampleDesc.Count=1;
            maskDesc.Usage=D3D11_USAGE_IMMUTABLE;
            maskDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            const UINT white=0xffffffff;
            D3D11_SUBRESOURCE_DATA pixels{&white,sizeof(white),0};
            ComPtr<ID3D11Texture2D> mask;
            if(FAILED(device->CreateTexture2D(&maskDesc,&pixels,&mask))||
               FAILED(device->CreateShaderResourceView(mask.Get(),nullptr,&fullyLitMask)))return false;
        }
        if(used==entries.size())entries.emplace_back();
        ComPtr<ID3D11DepthStencilState> replayDepth=depthState;
        if(reverseDepth){
            if(reverseDepthSource!=depthState){
                SetNearDepthComparison(depthDesc,true);
                ComPtr<ID3D11DepthStencilState> next;
                if(FAILED(device->CreateDepthStencilState(&depthDesc,&next)))return false;
                reverseDepthSource=depthState;
                reverseReplayDepth=next;
            }
            replayDepth=reverseReplayDepth;
        }
        auto& entry=entries[used];
        if(!RecordStage(immediate,entry,true)||!RecordStage(immediate,entry,false)){
            DiscardRecording();
            return false;
        }
        if(needsUnshadowedReflection){
            auto* mask=fullyLitMask.Get();
            deferred->PSSetShaderResources(shadowSlot,1,&mask);
        }

        Bindings<ID3D11Buffer,D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertices;
        UINT strides[std::size(vertices.values)]{},offsets[std::size(vertices.values)]{};
        immediate->IAGetVertexBuffers(0,UINT(std::size(vertices.values)),vertices.values,strides,offsets);
        deferred->IASetVertexBuffers(0,UINT(std::size(vertices.values)),vertices.values,strides,offsets);
        Buffer index;
        DXGI_FORMAT format{};
        UINT offset{};
        immediate->IAGetIndexBuffer(&index,&format,&offset);
        deferred->IASetIndexBuffer(index.Get(),format,offset);
        ComPtr<ID3D11InputLayout> layout;
        immediate->IAGetInputLayout(&layout);
        deferred->IASetInputLayout(layout.Get());
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        immediate->IAGetPrimitiveTopology(&topology);
        deferred->IASetPrimitiveTopology(topology);

        ComPtr<ID3D11RasterizerState> raster;
        immediate->RSGetState(&raster);
        deferred->RSSetState(raster.Get());
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT viewportCount=UINT(std::size(viewports)),scissorCount=UINT(std::size(scissors));
        immediate->RSGetViewports(&viewportCount,viewports);
        immediate->RSGetScissorRects(&scissorCount,scissors);
        deferred->RSSetViewports(viewportCount,viewports);
        deferred->RSSetScissorRects(scissorCount,scissors);

        // Auxiliary targets keep their original engine ordering. Only replay RGB color.
        bool hasAuxiliary=false;
        for(UINT slot=1;slot<std::size(targets.values);++slot){
            const auto& targetBlend=blendDesc.RenderTarget[blendDesc.IndependentBlendEnable?slot:0];
            if(targets.values[slot]&&targetBlend.RenderTargetWriteMask)hasAuxiliary=true;
        }
        const bool keepAlpha=(color.RenderTargetWriteMask&D3D11_COLOR_WRITE_ENABLE_ALPHA)!=0;
        if(blendSource!=blend){
            auto replayDesc=blendDesc;
            replayDesc.RenderTarget[0].RenderTargetWriteMask&=7;
            auto auxiliaryDesc=blendDesc;
            if(!auxiliaryDesc.IndependentBlendEnable){
                for(UINT slot=1;slot<std::size(targets.values);++slot)auxiliaryDesc.RenderTarget[slot]=color;
                auxiliaryDesc.IndependentBlendEnable=TRUE;
            }
            auxiliaryDesc.RenderTarget[0].RenderTargetWriteMask&=D3D11_COLOR_WRITE_ENABLE_ALPHA;
            blendSource.Reset();
            if(FAILED(device->CreateBlendState(&replayDesc,replayBlend.ReleaseAndGetAddressOf()))||
               FAILED(device->CreateBlendState(&auxiliaryDesc,auxiliaryBlend.ReleaseAndGetAddressOf()))){
                DiscardRecording();
                return false;
            }
            blendSource=blend;
        }

        deferred->OMSetRenderTargets(1,targets.values,depth.Get());
        deferred->OMSetDepthStencilState(replayDepth.Get(),stencil);
        deferred->OMSetBlendState(replayBlend.Get(),factors,sampleMask);
        deferred->VSSetShader(vertexShader.Get(),nullptr,0);
        deferred->PSSetShader(pixelShader.Get(),nullptr,0);
        recording=true;
        deferred->DrawIndexed(count,start,base);
        recording=false;
        if(FAILED(deferred->FinishCommandList(FALSE,entry.commands.ReleaseAndGetAddressOf())))return false;

        entry.bottle=bottle;
        entry.target=target;
        entry.shadowMaskBound=needsUnshadowedReflection;
        entry.visible=visible;
        ++used;
        if(visible&&(hasAuxiliary||keepAlpha)){
            immediate->OMSetBlendState(auxiliaryBlend.Get(),factors,sampleMask);
            draw(immediate,count,start,base);
            immediate->OMSetBlendState(blend.Get(),factors,sampleMask);
        }
        return true;
    }

    bool DrawPending(ID3D11DeviceContext* immediate,ID3D11Resource* target,const void* bottle,bool visible=true){
        if(context.Get()!=immediate||!target)return false;
        bool drawn=false;
        for(size_t i=0;i<used;++i){
            auto& entry=entries[i];
            if(!entry.commands||entry.target.Get()!=target||(bottle&&entry.bottle!=bottle))continue;
            if(visible&&entry.visible){
                execute(immediate,entry.commands.Get());
                drawn=true;
            }
            entry.commands.Reset();
        }
        return drawn;
    }

    void Reset(){
        for(size_t i=0;i<used;++i){
            entries[i].commands.Reset();
            entries[i].target.Reset();
        }
        used=0;
    }

    bool IsRecording()const{return recording;}

private:
    static void ExecuteNormally(ID3D11DeviceContext* context,ID3D11CommandList* commands){
        context->ExecuteCommandList(commands,TRUE);
    }
    Execute execute;

    void DiscardRecording(){
        Microsoft::WRL::ComPtr<ID3D11CommandList> discarded;
        deferred->FinishCommandList(FALSE,&discarded);
    }

    std::vector<Entry> entries;
    size_t used{};
    bool recording{};
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context,deferred;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> immediate1,deferred1;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendSource,replayBlend,auxiliaryBlend;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> reverseDepthSource,reverseReplayDepth;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> fullyLitMask;
};
