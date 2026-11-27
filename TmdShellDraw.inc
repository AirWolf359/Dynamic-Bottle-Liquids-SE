thread_local float shellCoverage=-1;
#ifdef DBL_SHELL_OPTICS
// The held Jessica wine body's authored 128px cubemap produced the changing
// cavity highlight in the fireplace captures. Keep its directional reflection
// using the 2x2 and 1x1 face levels for this single native draw.
struct HeldWineReflectionFilter {
    ID3D11DeviceContext* context;
    ComPtr<ID3D11SamplerState> original;
    bool bound=false;
    void bind(){
        ComPtr<ID3D11ShaderResourceView> view;
        context->PSGetShaderResources(4,1,&view);
        if(!view)return;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};view->GetDesc(&v);
        if(v.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURECUBE||v.Format!=DXGI_FORMAT_BC7_UNORM||
           v.TextureCube.MostDetailedMip!=0||v.TextureCube.MipLevels<8)return;
        ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;if(FAILED(resource.As(&texture)))return;
        D3D11_TEXTURE2D_DESC t{};texture->GetDesc(&t);
        if(t.Width!=128||t.Height!=128||t.MipLevels!=8)return;
        context->PSGetSamplers(4,1,&original);
        if(!original)return;
        static ComPtr<ID3D11SamplerState> source,filtered;
        if(source!=original){
            D3D11_SAMPLER_DESC desc{};original->GetDesc(&desc);
            if(desc.MinLOD>=6||desc.MaxLOD<6)return;
            desc.MinLOD=6;
            ComPtr<ID3D11Device> device;context->GetDevice(&device);
            ComPtr<ID3D11SamplerState> next;
            if(FAILED(device->CreateSamplerState(&desc,&next)))return;
            source=original;filtered=next;
        }
        auto* sampler=filtered.Get();context->PSSetSamplers(4,1,&sampler);bound=true;
    }
    ~HeldWineReflectionFilter(){if(bound){auto* sampler=original.Get();context->PSSetSamplers(4,1,&sampler);}}
};
#endif
bool DrawTmdShell(ID3D11DeviceContext* ctx,UINT count,UINT start,INT base,
                  bool authoredBody=false,float authoredOpacity=1.f,float bend=0,int mip=0){
    const float coverage=authoredBody?authoredOpacity:shellCoverage;
    if(coverage<0)return false;
    ComPtr<ID3D11BlendState> original;FLOAT oldFactors[4];UINT mask;
    ctx->OMGetBlendState(&original,oldFactors,&mask);
    D3D11_BLEND_DESC desc{};
    if(original)original->GetDesc(&desc);
    else {
        auto& r=desc.RenderTarget[0];r.SrcBlend=r.SrcBlendAlpha=D3D11_BLEND_ONE;
        r.DestBlend=r.DestBlendAlpha=D3D11_BLEND_ZERO;r.BlendOp=r.BlendOpAlpha=D3D11_BLEND_OP_ADD;r.RenderTargetWriteMask=15;
    }
    auto& rt=desc.RenderTarget[0];
    const bool additiveShell=rt.SrcBlend==D3D11_BLEND_SRC_ALPHA&&rt.DestBlend==D3D11_BLEND_ONE;
    if(authoredBody&&additiveShell)return false;
    if(rt.BlendOp!=D3D11_BLEND_OP_ADD||(!additiveShell&&rt.DestBlend!=D3D11_BLEND_INV_SRC_ALPHA&&!(rt.SrcBlend==D3D11_BLEND_ONE&&rt.DestBlend==D3D11_BLEND_ZERO))||
       (rt.RenderTargetWriteMask&7)!=7)return false;
    ComPtr<ID3D11RenderTargetView> output;ctx->OMGetRenderTargets(1,&output,nullptr);
    if(!output)return false;
    D3D11_RENDER_TARGET_VIEW_DESC target{};output->GetDesc(&target);
    if(target.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D)return false;
    if(!rt.BlendEnable&&target.Format!=DXGI_FORMAT_R11G11B10_FLOAT&&target.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT&&
       target.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&target.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)return false;
    struct Entry {ComPtr<ID3D11BlendState> from,to;};
    static std::vector<Entry> cache;
    auto i=std::find_if(cache.begin(),cache.end(),[&](auto& e){return e.from.Get()==original.Get();});
    if(i==cache.end()){
        auto nativeTarget=rt;Entry e;e.from=original;rt.BlendEnable=TRUE;rt.SrcBlend=D3D11_BLEND_BLEND_FACTOR;rt.DestBlend=D3D11_BLEND_INV_BLEND_FACTOR;
        if(!desc.IndependentBlendEnable){
            for(int n=1;n<8;++n)desc.RenderTarget[n]=nativeTarget;
            desc.IndependentBlendEnable=TRUE;
        }
        ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
        if(FAILED(device->CreateBlendState(&desc,&e.to)))return false;
        if(cache.size()>=32)cache.clear();cache.push_back(std::move(e));i=std::prev(cache.end());
    }
#ifdef DBL_SHELL_OPTICS
    if(coverage<1)DrawShellTransmission(ctx,count,start,base,authoredBody?bend:shellBend,authoredBody?mip:shellMip);
#endif
    const FLOAT factors[4]={coverage,coverage,coverage,coverage};
    ComPtr<ID3D11DepthStencilState> oldDepth;UINT oldStencil=0;
    if(!authoredBody&&coverage>=1){
        ctx->OMGetDepthStencilState(&oldDepth,&oldStencil);
        D3D11_DEPTH_STENCIL_DESC d{};
        if(oldDepth)oldDepth->GetDesc(&d);
        if(oldDepth&&d.DepthEnable&&d.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ALL){
            static ComPtr<ID3D11DepthStencilState> source,writing;
            if(source.Get()!=oldDepth.Get()){
                ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
                writing.Reset();if(SUCCEEDED(dev->CreateDepthStencilState(&d,&writing)))source=oldDepth;else source.Reset();
            }
            if(writing)ctx->OMSetDepthStencilState(writing.Get(),oldStencil);
        }
    }
    ctx->OMSetBlendState(i->to.Get(),factors,mask);
#ifdef DBL_SHELL_OPTICS
    {
        BottleGlassDepthRequest depthRequest(ctx);
        if(IsNativeGlassColorPass(ctx)&&UsesBottledWorldGlassDepth(ctx))depthRequest.normalize();
        HeldWineReflectionFilter reflection{ctx};
        if(authoredBody&&active&&active->geometry->name=="DBL_SSE_TMDWinery_winebottle01jessica"&&GrabbedBottle(active->geometry))
            reflection.bind();
        oldDraw(ctx,count,start,base);
    }
#else
    oldDraw(ctx,count,start,base);
#endif
    ctx->OMSetBlendState(original.Get(),oldFactors,mask);
    if(!authoredBody&&coverage>=1)ctx->OMSetDepthStencilState(oldDepth.Get(),oldStencil);
#ifdef DBL_SHELL_OPTICS
    if(!authoredBody&&coverage>0&&coverage<1)
        DrawTmdShellCoverage(ctx,shellOpticsBuffer.Get(),shellCoveragePS.Get(),coverage,oldDraw,count,start,base);
#endif
    return true;
}
