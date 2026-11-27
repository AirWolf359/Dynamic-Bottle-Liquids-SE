#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <string_view>
#pragma comment(lib,"dxguid.lib")

enum class BottledDepthSource { None, NamedResource, NamedView, ExplicitOverride };

inline BottledDepthSource IdentifyBottledDepth(ID3D11DepthStencilView& view,bool explicitOverride){
    D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
    view.GetDesc(&desc);
    if(desc.Format!=DXGI_FORMAT_D32_FLOAT_S8X24_UINT||desc.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D||
       desc.Texture2D.MipSlice!=0)return BottledDepthSource::None;

    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    view.GetResource(&resource);
    auto namedDepth=[](ID3D11DeviceChild* object,bool viewName){
        char label[64]{};
        UINT size=sizeof(label);
        if(!object||FAILED(object->GetPrivateData(WKPDID_D3DDebugObjectName,&size,label)))return false;
        std::string_view name(label,size);
        if(!name.empty()&&name.back()=='\0')name.remove_suffix(1);
        constexpr std::string_view prefix="ReverseZ::Depth";
        if(name.compare(0,prefix.size(),prefix)!=0)return false;
        const auto tail=name.substr(prefix.size());
        const auto split=tail.find(' ');
        const auto index=tail.substr(0,split);
        if(index.empty()||index.find_first_not_of("0123456789")!=std::string_view::npos)return false;
        if(!viewName)return split==std::string_view::npos;
        if(split==std::string_view::npos)return false;
        auto suffix=tail.substr(split+1);
        if(suffix.compare(0,3,"DSV")==0)suffix.remove_prefix(3);
        else if(suffix.compare(0,5,"RODSV")==0)suffix.remove_prefix(5);
        else return false;
        return !suffix.empty()&&suffix.find_first_not_of("0123456789")==std::string_view::npos;
    };
    if(namedDepth(resource.Get(),false))return BottledDepthSource::NamedResource;
    // The engine can rename the texture to kMAIN while Bottled retains its DSV name.
    if(namedDepth(&view,true))return BottledDepthSource::NamedView;
    return explicitOverride?BottledDepthSource::ExplicitOverride:BottledDepthSource::None;
}

inline bool IsNearDepthComparison(D3D11_COMPARISON_FUNC comparison)
{
    return comparison==D3D11_COMPARISON_LESS||comparison==D3D11_COMPARISON_LESS_EQUAL||
        comparison==D3D11_COMPARISON_GREATER||comparison==D3D11_COMPARISON_GREATER_EQUAL;
}

// Preserve stencil, write masks and the distinction between strict and inclusive tests.
inline void SetNearDepthComparison(D3D11_DEPTH_STENCIL_DESC& desc,bool reverse)
{
    const bool inclusive=desc.DepthFunc==D3D11_COMPARISON_LESS_EQUAL||
        desc.DepthFunc==D3D11_COMPARISON_GREATER_EQUAL;
    desc.DepthFunc=reverse?
        (inclusive?D3D11_COMPARISON_GREATER_EQUAL:D3D11_COMPARISON_GREATER):
        (inclusive?D3D11_COMPARISON_LESS_EQUAL:D3D11_COMPARISON_LESS);
}

struct BottleGlassDepthRequest {
    ID3D11DeviceContext* context;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> original;
    UINT stencil{};
    bool changed{};

    explicit BottleGlassDepthRequest(ID3D11DeviceContext* ctx):context(ctx){}
    void normalize(){
        using Microsoft::WRL::ComPtr;
        context->OMGetDepthStencilState(&original,&stencil);
        if(!original)return;
        D3D11_DEPTH_STENCIL_DESC desc{};
        original->GetDesc(&desc);
        if(!desc.DepthEnable||(desc.DepthFunc!=D3D11_COMPARISON_GREATER&&
           desc.DepthFunc!=D3D11_COMPARISON_GREATER_EQUAL))return;
        static thread_local ComPtr<ID3D11DepthStencilState> source,replacement;
        if(source!=original){
            SetNearDepthComparison(desc,false);
            ComPtr<ID3D11Device> device;
            context->GetDevice(&device);
            ComPtr<ID3D11DepthStencilState> next;
            if(FAILED(device->CreateDepthStencilState(&desc,&next)))return;
            source=original;
            replacement=next;
        }
        context->OMSetDepthStencilState(replacement.Get(),stencil);
        changed=true;
    }
    ~BottleGlassDepthRequest(){
        if(changed)context->OMSetDepthStencilState(original.Get(),stencil);
    }
};
