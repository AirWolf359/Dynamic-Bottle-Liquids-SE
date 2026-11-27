#ifdef DBL_DEVELOPMENT
bool bottledGlassDepthTrial=false;
#else
constexpr bool bottledGlassDepthTrial=false;
#endif

// Scope glass compatibility to the actual world color and floating-point
// depth/stencil views. The engine's main-depth slot may name a different view.
bool UsesBottledWorldGlassDepth(ID3D11DeviceContext* ctx,bool requireNamedResource=false){
    if(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    ComPtr<ID3D11RenderTargetView> color;
    ComPtr<ID3D11DepthStencilView> depthView;
    ctx->OMGetRenderTargets(1,&color,&depthView);
    if(!depthView||!IsMainSceneTarget(color.Get()))return false;

    static ComPtr<ID3D11DepthStencilView> previous;
    static BottledDepthSource converted=BottledDepthSource::None;
    if(previous!=depthView){
        previous=depthView;
        converted=IdentifyBottledDepth(*depthView.Get(),bottledGlassDepthTrial);
        if(converted!=BottledDepthSource::None){
            static bool reportedResource=false,reportedView=false,reportedOverride=false;
            const bool resource=converted==BottledDepthSource::NamedResource;
            const bool view=converted==BottledDepthSource::NamedView;
            bool& reported=resource?reportedResource:view?reportedView:reportedOverride;
            if(!reported){
                reported=true;
                SKSE::log::info("[DBL-glass-depth] MAIN reverse-Z detection={}",resource?"named-resource":view?"named-view":"explicit-override");
            }
        }
    }
    return converted==BottledDepthSource::NamedResource||converted==BottledDepthSource::NamedView||
        (!requireNamedResource&&converted==BottledDepthSource::ExplicitOverride);
}

bool IsNativeGlassColorPass(ID3D11DeviceContext* ctx){
    ComPtr<ID3D11BlendState> blendState;
    ctx->OMGetBlendState(&blendState,nullptr,nullptr);
    if(!blendState)return false;
    D3D11_BLEND_DESC desc{};
    blendState->GetDesc(&desc);
    const auto& color=desc.RenderTarget[0];
    return color.BlendEnable&&(color.RenderTargetWriteMask&7)&&
        (color.DestBlend==D3D11_BLEND_INV_SRC_ALPHA||color.DestBlend==D3D11_BLEND_ONE||
         color.DestBlend==D3D11_BLEND_INV_BLEND_FACTOR);
}
