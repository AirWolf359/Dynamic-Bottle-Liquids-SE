// Snapshot the current color/depth before each bottle; never sample a bound output.
struct SceneSnapshot {
    uint64_t sharedFrame=~0ull;
    uint64_t captures=0;
    ComPtr<ID3D11RenderTargetView> sharedTarget;
    ComPtr<ID3D11DepthStencilView> sharedDepth;
    D3D11_VIEWPORT sharedViewport{};
    V4 sharedInfo{},sharedArea{};
    bool captureShared(ID3D11DeviceContext* ctx,uint64_t frame,V4& info,V4& area){
        ComPtr<ID3D11RenderTargetView> rt;ComPtr<ID3D11DepthStencilView> ds;
        ctx->OMGetRenderTargets(1,&rt,&ds);
        UINT count=1;D3D11_VIEWPORT vp{};ctx->RSGetViewports(&count,&vp);
        if(count==1&&sharedFrame==frame&&rt==sharedTarget&&ds==sharedDepth&&memcmp(&vp,&sharedViewport,sizeof(vp))==0){
            // A later bottle may be the first one requesting downsampled color.
            // SharedBlur independently gates actual mip work to once per source/frame.
            ComPtr<ID3D11Resource> resource;rt->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;
            if(SUCCEEDED(resource.As(&texture)))blur.build(ctx,colorView.Get(),sampledDepth(),texture.Get(),width,height);
            info=sharedInfo;area=sharedArea;return true;
        }
        sharedFrame=~0ull;
        if(!capture(ctx,info,area))return false;
        sharedFrame=frame;sharedTarget=rt;sharedDepth=ds;sharedViewport=vp;sharedInfo=info;sharedArea=area;return true;
    }
    SharedBlur blur;
    ComPtr<ID3D11Texture2D> color,depth;
    ComPtr<ID3D11ShaderResourceView> colorView,depthView;
    ComPtr<ID3D11SamplerState> sampler;
    std::string status;
    ComPtr<ID3D11Texture2D> nativeDepth;ComPtr<ID3D11ShaderResourceView> nativeDepthView;
    bool usingNativeDepth=false;
    UINT width{},height{};DXGI_FORMAT colorFormat{},depthFormat{};
    bool capture(ID3D11DeviceContext* ctx,V4& info,V4& viewport,bool immutableDepth=false) {
        info={};ComPtr<ID3D11RenderTargetView> rt;ComPtr<ID3D11DepthStencilView> ds;
        ctx->OMGetRenderTargets(1,&rt,&ds);if(!rt||!ds){status="missing color or depth target";return false;}
        D3D11_RENDER_TARGET_VIEW_DESC rv{};rt->GetDesc(&rv);
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};ds->GetDesc(&dv);
        if(rv.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D||rv.Texture2D.MipSlice||dv.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D||dv.Texture2D.MipSlice){status=std::format("view color format={} dimension={} mip={}; depth format={} dimension={} mip={}",int(rv.Format),int(rv.ViewDimension),rv.Texture2D.MipSlice,int(dv.Format),int(dv.ViewDimension),dv.Texture2D.MipSlice);return false;}
        DXGI_FORMAT df=DXGI_FORMAT_UNKNOWN,sf=DXGI_FORMAT_UNKNOWN;
        if(dv.Format==DXGI_FORMAT_D24_UNORM_S8_UINT){df=DXGI_FORMAT_R24G8_TYPELESS;sf=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;}
        if(dv.Format==DXGI_FORMAT_D32_FLOAT){df=DXGI_FORMAT_R32_TYPELESS;sf=DXGI_FORMAT_R32_FLOAT;}
        if(dv.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT){df=DXGI_FORMAT_R32G8X24_TYPELESS;sf=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;}
        if(df==DXGI_FORMAT_UNKNOWN){status=std::format("Unsupported depth format {}",int(dv.Format));return false;}
        ComPtr<ID3D11Resource> cr,dr;rt->GetResource(&cr);ds->GetResource(&dr);
        ComPtr<ID3D11Texture2D> ct,dt;if(FAILED(cr.As(&ct))||FAILED(dr.As(&dt)))return false;
        D3D11_TEXTURE2D_DESC cd{},dd{};ct->GetDesc(&cd);dt->GetDesc(&dd);
        const bool depthSampleable=(dd.BindFlags&D3D11_BIND_SHADER_RESOURCE)!=0;
        if(cd.SampleDesc.Count!=1||dd.SampleDesc.Count!=1||cd.ArraySize!=1||dd.ArraySize!=1||cd.MipLevels!=1||dd.MipLevels!=1||cd.Width!=dd.Width||cd.Height!=dd.Height){status=std::format("textures color={}x{} format={} samples={} array={} mips={}; depth={}x{} format={} samples={} array={} mips={}",cd.Width,cd.Height,int(cd.Format),cd.SampleDesc.Count,cd.ArraySize,cd.MipLevels,dd.Width,dd.Height,int(dd.Format),dd.SampleDesc.Count,dd.ArraySize,dd.MipLevels);return false;}
        UINT count=1;D3D11_VIEWPORT vp{};ctx->RSGetViewports(&count,&vp);
        // Skyrim uses MaxDepth=0.999998. SV_Position.z and sampled scene depth
        // already share the viewport mapping; only reject invalid/reversed ranges.
        if(count!=1||!std::isfinite(vp.MinDepth)||!std::isfinite(vp.MaxDepth)||vp.MinDepth<0||vp.MaxDepth>1||vp.MaxDepth<=vp.MinDepth){status=std::format("viewport count={} x={} y={} width={} height={} depth=[{},{}]",count,vp.TopLeftX,vp.TopLeftY,vp.Width,vp.Height,vp.MinDepth,vp.MaxDepth);return false;}
        ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
        if(!color||!depth||!colorView||!depthView||width!=cd.Width||height!=cd.Height||colorFormat!=rv.Format||depthFormat!=df){
            color.Reset();depth.Reset();colorView.Reset();depthView.Reset();
            width=cd.Width;height=cd.Height;colorFormat=rv.Format;depthFormat=df;
            cd.Format=rv.Format;cd.Usage=D3D11_USAGE_DEFAULT;cd.BindFlags=D3D11_BIND_SHADER_RESOURCE;cd.CPUAccessFlags=cd.MiscFlags=0;
            check(device->CreateTexture2D(&cd,nullptr,&color));check(device->CreateShaderResourceView(color.Get(),nullptr,&colorView));
            dd.Format=df;dd.Usage=D3D11_USAGE_DEFAULT;dd.BindFlags=D3D11_BIND_SHADER_RESOURCE;dd.CPUAccessFlags=dd.MiscFlags=0;
            check(device->CreateTexture2D(&dd,nullptr,&depth));
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=sf;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
            check(device->CreateShaderResourceView(depth.Get(),&sd,&depthView));
        }
        if(!sampler){D3D11_SAMPLER_DESC d{};d.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;d.AddressU=d.AddressV=d.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;d.MaxLOD=D3D11_FLOAT32_MAX;check(device->CreateSamplerState(&d,&sampler));}
        // Only the forward HDR resolve may borrow completed scene depth: its draws
        // write a separate nearest-depth target, never this resource.
        usingNativeDepth=false;
        if(immutableDepth&&depthSampleable){
            if(nativeDepth!=dt){nativeDepth=dt;nativeDepthView.Reset();
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=sf;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
                device->CreateShaderResourceView(dt.Get(),&sd,&nativeDepthView);
            }
            usingNativeDepth=bool(nativeDepthView);
        }
        {dblui::GPUCost timing(ctx,dblui::GPUStage::Prepare);
        ctx->CopyResource(color.Get(),ct.Get());
        ++captures;
        if(!usingNativeDepth)ctx->CopyResource(depth.Get(),dt.Get());}
        blur.build(ctx,colorView.Get(),sampledDepth(),ct.Get(),width,height);
        status="active";info={1,float(width),float(height),4};viewport={vp.TopLeftX,vp.TopLeftY,vp.Width,vp.Height};return true;
    }
    ID3D11ShaderResourceView* sampledDepth()const{return usingNativeDepth?nativeDepthView.Get():depthView.Get();}
} sceneSnapshot;
SceneSnapshot nativeGlassSnapshot;
