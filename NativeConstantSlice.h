#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>

struct NativeConstantSlice {
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    UINT first{}, count{D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT};

    bool Contains(UINT elements) const {
        if(!buffer||count<elements)return false;
        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        return first<=desc.ByteWidth/16&&elements<=desc.ByteWidth/16-first;
    }

    void Read(ID3D11DeviceContext* context,ID3D11DeviceContext1* ranged,bool vertex,UINT slot){
        first=0;
        count=D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
        auto** target=buffer.ReleaseAndGetAddressOf();
        if(ranged){
            if(vertex)ranged->VSGetConstantBuffers1(slot,1,target,&first,&count);
            else ranged->PSGetConstantBuffers1(slot,1,target,&first,&count);
        }else{
            if(vertex)context->VSGetConstantBuffers(slot,1,target);
            else context->PSGetConstantBuffers(slot,1,target);
        }
    }

    void Bind(ID3D11DeviceContext* context,ID3D11DeviceContext1* ranged,bool vertex,UINT slot) const {
        auto* value=buffer.Get();
        if(ranged){
            if(vertex)ranged->VSSetConstantBuffers1(slot,1,&value,&first,&count);
            else ranged->PSSetConstantBuffers1(slot,1,&value,&first,&count);
        }else{
            if(vertex)context->VSSetConstantBuffers(slot,1,&value);
            else context->PSSetConstantBuffers(slot,1,&value);
        }
    }

    bool Copy(ID3D11DeviceContext* context,ID3D11Device* device,const NativeConstantSlice& source){
        if(!source.buffer)return false;
        D3D11_BUFFER_DESC desc{}, saved{};
        source.buffer->GetDesc(&desc);
        if(buffer)buffer->GetDesc(&saved);
        if(saved.ByteWidth!=desc.ByteWidth){
            desc.Usage=D3D11_USAGE_DEFAULT;
            desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            desc.CPUAccessFlags=desc.MiscFlags=desc.StructureByteStride=0;
            if(FAILED(device->CreateBuffer(&desc,nullptr,buffer.ReleaseAndGetAddressOf())))return false;
        }
        context->CopyResource(buffer.Get(),source.buffer.Get());
        first=source.first;
        count=source.count;
        return true;
    }
};
