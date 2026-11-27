struct BottleProjection {
    static constexpr size_t maxPerFrame=6;
    struct Constants {
        V4 plane,body,absorption,controls,screen,lighting,shape;
        V4 rings[16];
    };
    static_assert(sizeof(Constants)==368);

#include "ProjectionTile.inc"

    bool HasReceiverLight(uint64_t frame) const { return receiverFrame==frame; }
    const RE::BSLight* ReceiverLight(uint64_t frame) const { return HasReceiverLight(frame)?receiverLight:nullptr; }
    const RE::BSLight* RecentReceiverLight(uint64_t frame) const {
        return (receiverFrame==frame||(frame>0&&receiverFrame==frame-1))?receiverLight:nullptr;
    }
    bool ReceiverReaches(const RE::BSGeometry* body,uint64_t frame) const {
        return HasReceiverLight(frame)&&(receiverOrigin-body->world.translate).SqrLength()<receiverRadiusSquared;
    }
    void CaptureReceiverLight(ID3D11DeviceContext* ctx,uint64_t frame,const RE::BSLight* light,
                              RE::NiPoint3 origin,float radiusSquared,RE::NiPoint3 anchor){
        if(HasReceiverLight(frame))return;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext1> ranged;
        ctx->GetDevice(&device);
        ctx->QueryInterface(IID_PPV_ARGS(&ranged));
        NativeConstantSlice geometry,lighting;
        geometry.Read(ctx,ranged.Get(),true,2);
        lighting.Read(ctx,ranged.Get(),false,2);
        if(geometry.Contains(3)&&lighting.Contains(30)&&
           receiverWorld.Copy(ctx,device.Get(),geometry)&&receiverLights.Copy(ctx,device.Get(),lighting)){
            receiverFrame=frame;
            receiverOrigin=origin;
            receiverRadiusSquared=radiusSquared;
            receiverAnchor=anchor;
            receiverLight=light;
        }
    }

    size_t Count(uint64_t frame) const {
        return std::max(CausticCount(frame),ShadowCount(frame));
    }
    size_t ShadowCount(uint64_t frame) const { return maskFrame==frame?maskCount:0; }
    size_t CausticCount(uint64_t frame) const { return submittedFrame==frame?submittedCount:0; }
    std::string_view Status(uint64_t frame) const {
        if(Count(frame))return "Projection drawn on the world scene";
        if(attemptedFrame==frame)return waiting;
        return "Waiting for a supported bottle's world draw";
    }

    void Reset(){
        for(auto& caster:casters)caster.size=0;
        receiverCasterLightFrame=~0ull;
        tiles={};atlasSizes={};tileBuilds=tileHits=0;
        submittedFrame=snapshotFrame=receiverFrame=maskFrame=~0ull;
    }

    void Initialize(ID3D11Device* device,const std::filesystem::path& directory){
        InitTiles(device,directory);
        ComPtr<ID3DBlob> code;
        check(D3DReadFileToBlob((directory/"BottleProjectionVS.cso").c_str(),&code));
        check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vertex));
        check(D3DReadFileToBlob((directory/"BottleProjectionPS.cso").c_str(),code.ReleaseAndGetAddressOf()));
        check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&pixel));
        check(D3DReadFileToBlob((directory/"BottleShadowMaskVS.cso").c_str(),code.ReleaseAndGetAddressOf()));
        check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&maskVertex));
        check(D3DReadFileToBlob((directory/"BottleShadowMaskPS.cso").c_str(),code.ReleaseAndGetAddressOf()));
        check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&maskPixel));

        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.ByteWidth=sizeof(Constants);
        bufferDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&bufferDesc,nullptr,&constants));
        bufferDesc.ByteWidth=sizeof(V4)*4;
        check(device->CreateBuffer(&bufferDesc,nullptr,&relativeCaster));
        D3D11_BLEND_DESC blendDesc{};
        auto& color=blendDesc.RenderTarget[0];
        color.BlendEnable=TRUE;
        color.SrcBlend=D3D11_BLEND_ONE;
        color.DestBlend=D3D11_BLEND_SRC1_COLOR;
        color.BlendOp=D3D11_BLEND_OP_ADD;
        color.SrcBlendAlpha=D3D11_BLEND_ZERO;
        color.DestBlendAlpha=D3D11_BLEND_ONE;
        color.BlendOpAlpha=D3D11_BLEND_OP_ADD;
        color.RenderTargetWriteMask=7;
        check(device->CreateBlendState(&blendDesc,&modulate));
        color.SrcBlend=D3D11_BLEND_ZERO;
        color.DestBlend=D3D11_BLEND_SRC_COLOR;
        color.DestBlendAlpha=D3D11_BLEND_SRC_ALPHA;
        color.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        check(device->CreateBlendState(&blendDesc,&maskMultiply));

        D3D11_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode=D3D11_FILL_SOLID;
        rasterDesc.CullMode=D3D11_CULL_NONE;
        rasterDesc.DepthClipEnable=TRUE;
        check(device->CreateRasterizerState(&rasterDesc,&raster));
    }

    struct Caster {
        const RE::BSGeometry* body{};
        Constants data{};
        const RE::BSLight* light{};
    };

    void RememberCaster(const RE::BSGeometry* body,uint64_t frame,const Constants& data,const RE::BSLight* light){
        if(attemptedFrame!=frame){
            attemptedFrame=frame;
            waiting="Waiting for a matching native fireplace light";
        }
        auto& batch=casters[frame%casters.size()];
        if(batch.frame!=frame){batch.frame=frame;batch.size=0;}
        const auto end=batch.entries.begin()+batch.size;
        const auto found=std::find_if(batch.entries.begin(),end,[&](const auto& e){return e.body==body;});
        if(found!=end)return;
        if(batch.size<maxPerFrame)batch.entries[batch.size++]={body,data,light};
    }

    std::span<const Caster> Casters(uint64_t frame) const {
        if(frame==0)return {};
        const auto& batch=casters[(frame-1)%casters.size()];
        return batch.frame==frame-1?std::span<const Caster>(batch.entries.data(),batch.size):std::span<const Caster>{};
    }

    std::span<const RE::BSLight* const> ReceiverCasterLights(uint64_t frame){
        // Rendered casters belong to the previous frame and stay fixed during
        // this frame. The caller still checks live light flags and membership.
        if(receiverCasterLightFrame!=frame){
            receiverCasterLightFrame=frame;
            receiverCasterLightCount=0;
            for(const auto& caster:Casters(frame)){
                auto end=receiverCasterLights.begin()+receiverCasterLightCount;
                if(caster.light&&std::find(receiverCasterLights.begin(),end,caster.light)==end)
                    receiverCasterLights[receiverCasterLightCount++]=caster.light;
            }
        }
        return {receiverCasterLights.data(),receiverCasterLightCount};
    }

    ID3D11ShaderResourceView* ModifiedMask(uint64_t frame) const {
        return maskFrame==frame&&maskCount?privateMask.srv.Get():nullptr;
    }

    void BuildReceiverMask(ID3D11DeviceContext* ctx,uint64_t frame,ID3D11ShaderResourceView* original,
                           std::span<const Caster* const> batch){
        if(maskFrame==frame||!maskMultiply||batch.empty()||!HasReceiverLight(frame))return;
        attemptedFrame=frame;
        waiting="Waiting for the shadow receiver depth";
        ComPtr<ID3D11DepthStencilView> ds;
        ctx->OMGetRenderTargets(0,nullptr,&ds);
        if(!ds)return;
        ComPtr<ID3D11Resource> depthResource,maskResource;
        ds->GetResource(&depthResource);
        original->GetResource(&maskResource);
        ComPtr<ID3D11Texture2D> depth,source;
        if(FAILED(depthResource.As(&depth))||FAILED(maskResource.As(&source)))return;
        D3D11_TEXTURE2D_DESC depthDesc{},maskDesc{};
        depth->GetDesc(&depthDesc);
        source->GetDesc(&maskDesc);
        D3D11_SHADER_RESOURCE_VIEW_DESC maskView{};
        original->GetDesc(&maskView);
        if(!SingleTexture(depthDesc)||!SingleTexture(maskDesc)||maskDesc.Width!=depthDesc.Width||
           maskDesc.Height!=depthDesc.Height||maskView.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||
           maskView.Format!=DXGI_FORMAT_R8G8B8A8_UNORM)return;
        const auto viewport=WorldViewport(ctx,depthDesc);
        if(!viewport)return;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext1> ranged;
        ctx->GetDevice(&device);
        ctx->QueryInterface(IID_PPV_ARGS(&ranged));
        NativeConstantSlice frameConstants;
        frameConstants.Read(ctx,ranged.Get(),true,12);
        if(!frameConstants.Contains(12)||!SelectDepthView(device.Get(),depth.Get(),depthDesc))return;
        if(maskSource!=source){
            maskDesc.Format=maskView.Format;
            maskDesc.Usage=D3D11_USAGE_DEFAULT;
            maskDesc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            maskDesc.CPUAccessFlags=maskDesc.MiscFlags=0;
            OpticalBuffers::Target replacement;
            check(device->CreateTexture2D(&maskDesc,nullptr,&replacement.texture));
            check(device->CreateRenderTargetView(replacement.texture.Get(),nullptr,&replacement.rt));
            check(device->CreateShaderResourceView(replacement.texture.Get(),nullptr,&replacement.srv));
            privateMask=std::move(replacement);
            maskSource=source;
        }
        {
            dblui::GPUCost timing(ctx,dblui::GPUStage::MaskTextureCopy);
            ctx->CopyResource(privateMask.texture.Get(),source.Get());
        }
        camera=std::move(frameConstants);
        world=receiverWorld;
        donorLights=receiverLights;
        localDonor=receiverWorld;
        lights={};
        maskFrame=frame;
        maskCount=0;
        for(const auto* caster:batch){
            values=caster->data;
            values.screen=*viewport;
            values.lighting={0,1,0,1};
            const auto& transform=caster->body->world;
            const auto offset=transform.translate-receiverAnchor;
            const float position[]{offset.x,offset.y,offset.z};
            V4 relative[4]{};
            const auto lightOffset=receiverOrigin-receiverAnchor;
            relative[3]={lightOffset.x,lightOffset.y,lightOffset.z,std::sqrt(receiverRadiusSquared)};
            for(size_t i=0;i<3;++i){
                const auto& row=transform.rotate.entry[i];
                relative[i]={row[0]*transform.scale,row[1]*transform.scale,row[2]*transform.scale,position[i]};
            }
            ctx->UpdateSubresource(relativeCaster.Get(),0,nullptr,relative,0,0);
            Submit(ctx,privateMask.rt.Get(),true,caster->body,frame);
            ++maskCount;
        }
    }

    // A bottle can project once per frame and only into the verified MAIN view.
    void Prepare(ID3D11DeviceContext* ctx,const RE::BSGraphics::RendererData* renderer,
                 const RE::BSGeometry* body,uint64_t frame,Constants data,const NativeConstantSlice* reflection,
                 bool nativeShadowed,bool useReceiver,bool multiple,SceneSnapshot* freshCapture,bool mayHaveLight){
        attemptedFrame=frame;
        if(!mayHaveLight){
            waiting="No eligible point light for caustics";
            return;
        }
        waiting="Projection shaders unavailable";
        if(submittedFrame!=frame){
            submittedFrame=frame;
            submittedCount=0;
        }
        const auto end=submitted.begin()+submittedCount;
        if(!modulate||submittedCount==maxPerFrame||std::find(submitted.begin(),end,body)!=end)return;
        waiting="Waiting for native scene depth";
        if(!renderer)return;
        const auto& scene=renderer->renderTargets[RE::RENDER_TARGET::kMAIN];
        const auto& mask=renderer->renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];

        ComPtr<ID3D11RenderTargetView> target;
        ComPtr<ID3D11DepthStencilView> depthView;
        ctx->OMGetRenderTargets(1,&target,&depthView);
        if(!target||!depthView)return;
        ComPtr<ID3D11Resource> colorResource,depthResource;
        target->GetResource(&colorResource);
        depthView->GetResource(&depthResource);
        if(colorResource.Get()!=scene.texture)return;
        ComPtr<ID3D11Texture2D> depth;
        if(FAILED(depthResource.As(&depth)))return;

        D3D11_TEXTURE2D_DESC depthDesc{};
        depth->GetDesc(&depthDesc);
        const auto viewDesc=DepthViewDescription(depthDesc);
        if(!viewDesc)return;
        waiting="Waiting for a matching world viewport";
        const auto viewport=WorldViewport(ctx,depthDesc);
        if(!viewport)return;
        data.screen=*viewport;

        auto* shadowSRV=reinterpret_cast<ID3D11ShaderResourceView*>(mask.SRV);
        ComPtr<ID3D11ShaderResourceView> receiverShadow;
        if(shadowSRV&&mask.texture){
            D3D11_TEXTURE2D_DESC shadowDesc{};
            D3D11_SHADER_RESOURCE_VIEW_DESC shadowView{};
            mask.texture->GetDesc(&shadowDesc);
            shadowSRV->GetDesc(&shadowView);
            if(shadowDesc.Width==depthDesc.Width&&shadowDesc.Height==depthDesc.Height&&
               shadowView.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D&&shadowView.Texture2D.MostDetailedMip==0)
                receiverShadow=shadowSRV;
        }
        waiting="Waiting for the fireplace light and its receiver mask";
        if(useReceiver&&(!ReceiverReaches(body,frame)||!receiverShadow))return;
        data.lighting.y=receiverShadow?1.f:0.f;
        data.lighting.z=(nativeShadowed||useReceiver)&&receiverShadow?0.f:1.f;
        data.lighting.w=useReceiver?1.f:0.f;

        ComPtr<ID3D11Device> device;
        ctx->GetDevice(&device);
        ComPtr<ID3D11DeviceContext1> ranged;
        ctx->QueryInterface(IID_PPV_ARGS(&ranged));
        NativeConstantSlice nativeWorld,nativeFrame,nativeLights;
        nativeWorld.Read(ctx,ranged.Get(),true,2);
        nativeFrame.Read(ctx,ranged.Get(),true,12);
        nativeLights.Read(ctx,ranged.Get(),false,2);
        waiting="Waiting for native camera and light constants";
        if(!nativeWorld.Contains(3)||!nativeFrame.Contains(12)||
           (!nativeLights.Contains(30)&&!reflection&&!useReceiver))return;
        waiting="Unable to copy the scene inputs";
        if(!SelectDepthView(device.Get(),depth.Get(),depthDesc))return;
        // Submit unbinds the world DSV before sampling. For a single receiver,
        // Test241's first depth copy and live view therefore contain identical pixels.
        if(!multiple){
            snapshotFrame=~0ull;
            receiverDepth=currentDepth;
        }else if(snapshotFrame!=frame||memcmp(&snapshotViewport,&data.screen,sizeof(V4))!=0){
            // freshCapture is supplied only for a successful copy on this exact
            // bottle draw. Older same-frame optical captures are ineligible.
            receiverDepth=freshCapture?freshCapture->LendDepth():nullptr;
            if(!receiverDepth){
                EnsureDepthSnapshot(device.Get(),depthDesc,*viewDesc);
                dblui::GPUCost timing(ctx,dblui::GPUStage::CausticDepthCopy);
                ctx->CopyResource(snapshot.Get(),depth.Get());
                receiverDepth=snapshotView;
            }
            snapshotFrame=frame;
            snapshotViewport=data.screen;
        }

        world=std::move(nativeWorld);
        camera=std::move(nativeFrame);
        lights=nativeLights.Contains(30)?std::move(nativeLights):NativeConstantSlice{};
        shadow=std::move(receiverShadow);
        donorLights=useReceiver?receiverLights:(reflection?*reflection:NativeConstantSlice{});
        localDonor=useReceiver?receiverWorld:NativeConstantSlice{};
        values=data;
        Submit(ctx,target.Get(),false,body,frame);
        submitted[submittedCount++]=body;
    }

private:
    void EnsureDepthSnapshot(ID3D11Device* device,D3D11_TEXTURE2D_DESC depthDesc,
                             const D3D11_SHADER_RESOURCE_VIEW_DESC& viewDesc){
        auto oldFormat=DXGI_FORMAT_UNKNOWN;
        if(snapshot){
            D3D11_TEXTURE2D_DESC old{};
            snapshot->GetDesc(&old);
            if(old.Width==depthDesc.Width&&old.Height==depthDesc.Height&&old.Format==depthDesc.Format)
                oldFormat=viewFormat;
        }
        if(oldFormat!=viewDesc.Format){
            depthDesc.Usage=D3D11_USAGE_DEFAULT;
            depthDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            depthDesc.CPUAccessFlags=depthDesc.MiscFlags=0;
            ComPtr<ID3D11Texture2D> replacement;
            ComPtr<ID3D11ShaderResourceView> replacementView;
            check(device->CreateTexture2D(&depthDesc,nullptr,&replacement));
            check(device->CreateShaderResourceView(replacement.Get(),&viewDesc,&replacementView));
            snapshot=std::move(replacement);
            snapshotView=std::move(replacementView);
            viewFormat=viewDesc.Format;
        }
    }

    static bool SingleTexture(const D3D11_TEXTURE2D_DESC& desc){
        return desc.SampleDesc.Count==1&&desc.ArraySize==1&&desc.MipLevels==1;
    }

    static std::optional<D3D11_SHADER_RESOURCE_VIEW_DESC> DepthViewDescription(const D3D11_TEXTURE2D_DESC& desc){
        if(!SingleTexture(desc)||!(desc.BindFlags&D3D11_BIND_SHADER_RESOURCE))return std::nullopt;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        switch(desc.Format){
            case DXGI_FORMAT_R24G8_TYPELESS: view.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;break;
            case DXGI_FORMAT_R32_TYPELESS: view.Format=DXGI_FORMAT_R32_FLOAT;break;
            case DXGI_FORMAT_R32G8X24_TYPELESS: view.Format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;break;
            default:return std::nullopt;
        }
        view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels=1;
        return view;
    }

    static std::optional<V4> WorldViewport(ID3D11DeviceContext* ctx,const D3D11_TEXTURE2D_DESC& depth){
        D3D11_VIEWPORT v{};
        UINT count=1;
        ctx->RSGetViewports(&count,&v);
        if(count!=1||v.TopLeftX!=0||v.TopLeftY!=0||v.Width<=0||v.Height<=0||v.Width>depth.Width||
           v.Height>depth.Height||v.MinDepth<0||v.MaxDepth>1||v.MaxDepth<=v.MinDepth)return std::nullopt;
        return V4{v.Width,v.Height,v.MinDepth,v.MaxDepth};
    }

    bool SelectDepthView(ID3D11Device* device,ID3D11Texture2D* depth,const D3D11_TEXTURE2D_DESC& desc){
        if(depth==currentDepthTexture.Get())return true;
        const auto descView=DepthViewDescription(desc);
        if(!descView)return false;
        ComPtr<ID3D11ShaderResourceView> view;
        check(device->CreateShaderResourceView(depth,&*descView,&view));
        currentDepthTexture=depth;
        currentDepth=std::move(view);
        snapshotFrame=~0ull;
        return true;
    }

    // Shade the verified MAIN target before this bottle draws its liquid and
    // glass. The world has depth and lighting already, and later draws cover
    // the projection in their original order even with a different HDR path.
    void Submit(ID3D11DeviceContext* ctx,ID3D11RenderTargetView* target,bool writingMask,const RE::BSGeometry* owner,uint64_t frame){
        OpticalState saved(ctx);
        ComPtr<ID3D11DeviceContext1> ranged;
        ctx->QueryInterface(IID_PPV_ARGS(&ranged));
        const UINT vertexSlots[]={0,2,7,8,9,10,12}, pixelSlots[]={2,9,10,11};
        NativeConstantSlice oldVS[std::size(vertexSlots)],oldPS[std::size(pixelSlots)];
        for(size_t i=0;i<std::size(vertexSlots);++i)oldVS[i].Read(ctx,ranged.Get(),true,vertexSlots[i]);
        for(size_t i=0;i<std::size(pixelSlots);++i)oldPS[i].Read(ctx,ranged.Get(),false,pixelSlots[i]);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->GSSetShader(nullptr,nullptr,0);
        ctx->RSSetState(raster.Get());
        auto* cb=constants.Get();ctx->PSSetConstantBuffers(10,1,&cb);
        cb=tileBuffer.Get();ctx->PSSetConstantBuffers(11,1,&cb);
        auto* tile=SelectTile(ctx,owner,frame,!writingMask);
        if(!tile)dblui::RecordTile(!writingMask,dblui::TileEvent::Fallback);
        if(tileBuffer){
            const TileView disabled{};
            ctx->UpdateSubresource(tileBuffer.Get(),0,nullptr,tile?&tile->view:&disabled,0,0);
        }
        auto* tileInput=tile?tile->target.srv.Get():nullptr;
        ctx->PSSetShaderResources(3,1,&tileInput);
        auto* focusInput=tile&&!writingMask?tile->focus.srv.Get():nullptr;
        ctx->PSSetShaderResources(4,1,&focusInput);
        if(tile){auto* filter=tileSampler.Get();ctx->PSSetSamplers(0,1,&filter);}
        ctx->OMSetRenderTargets(1,&target,nullptr);
        ctx->OMSetBlendState(writingMask?maskMultiply.Get():modulate.Get(),nullptr,~0u);
        const D3D11_VIEWPORT viewport{0,0,values.screen.x,values.screen.y,values.screen.z,values.screen.w};
        ctx->RSSetViewports(1,&viewport);
        ctx->VSSetShader(writingMask?maskVertex.Get():vertex.Get(),nullptr,0);
        ctx->PSSetShader(writingMask?maskPixel.Get():pixel.Get(),nullptr,0);
        auto* block=constants.Get();
        ctx->UpdateSubresource(block,0,nullptr,&values,0,0);
        ctx->VSSetConstantBuffers(10,1,&block);
        ctx->PSSetConstantBuffers(10,1,&block);
        lights.Bind(ctx,ranged.Get(),true,0);
        world.Bind(ctx,ranged.Get(),true,2);
        world.Bind(ctx,ranged.Get(),false,2);
        donorLights.Bind(ctx,ranged.Get(),true,7);
        localDonor.Bind(ctx,ranged.Get(),true,8);
        camera.Bind(ctx,ranged.Get(),true,12);

        ComPtr<ID3D11ShaderResourceView> previousCluster;
        NativeConstantSlice previousStrict;
        if(writingMask){
            ComPtr<ID3D11ShaderResourceView> cluster;
            ctx->PSGetShaderResources(35,1,&cluster);
            ctx->VSGetShaderResources(35,1,&previousCluster);
            auto* clusterView=cluster.Get();ctx->VSSetShaderResources(35,1,&clusterView);
            previousStrict.Read(ctx,ranged.Get(),true,3);
            NativeConstantSlice strict;strict.Read(ctx,ranged.Get(),false,3);
            strict.Bind(ctx,ranged.Get(),true,3);
            auto* relative=relativeCaster.Get();
            ctx->VSSetConstantBuffers(9,1,&relative);
            ctx->PSSetConstantBuffers(9,1,&relative);
        }
        ID3D11ShaderResourceView* inputs[]={writingMask?currentDepth.Get():receiverDepth.Get(),
                                          writingMask?nullptr:currentDepth.Get(),writingMask?nullptr:shadow.Get()};
        ctx->PSSetShaderResources(0,3,inputs);
        ComPtr<ID3DUserDefinedAnnotation> annotation;
        ctx->QueryInterface(IID_PPV_ARGS(&annotation));
        const bool marked=annotation&&annotation->GetStatus();
        if(marked)annotation->BeginEvent(writingMask?L"DBL fireplace shadow mask":L"DBL bottle caustic");
        {dblui::GPUCost timing(ctx,writingMask?dblui::GPUStage::Compose:dblui::GPUStage::CausticReceiver);ctx->Draw(4,0);}
        if(marked)annotation->EndEvent();
        if(writingMask){
            auto* old=previousCluster.Get();ctx->VSSetShaderResources(35,1,&old);
            previousStrict.Bind(ctx,ranged.Get(),true,3);
        }
        for(size_t i=0;i<std::size(vertexSlots);++i)oldVS[i].Bind(ctx,ranged.Get(),true,vertexSlots[i]);
        for(size_t i=0;i<std::size(pixelSlots);++i)oldPS[i].Bind(ctx,ranged.Get(),false,pixelSlots[i]);
    }

    NativeConstantSlice world,camera,lights,donorLights,localDonor,receiverLights,receiverWorld;
    ComPtr<ID3D11Buffer> constants,relativeCaster;
    ComPtr<ID3D11VertexShader> vertex,maskVertex;
    ComPtr<ID3D11PixelShader> pixel,maskPixel;
    ComPtr<ID3D11BlendState> modulate,maskMultiply;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11Texture2D> snapshot,currentDepthTexture,maskSource;
    ComPtr<ID3D11ShaderResourceView> snapshotView,currentDepth,shadow,receiverDepth;
    OpticalBuffers::Target privateMask;
    struct CasterBatch {
        std::array<Caster,maxPerFrame> entries;
        uint64_t frame=~0ull;
        size_t size=0;
    };
    std::array<CasterBatch,2> casters;
    std::array<const RE::BSLight*,maxPerFrame> receiverCasterLights{};
    size_t receiverCasterLightCount{};
    uint64_t receiverCasterLightFrame=~0ull;
    std::array<const RE::BSGeometry*,maxPerFrame> submitted{};
    size_t submittedCount=0,maskCount=0;
    RE::NiPoint3 receiverOrigin{},receiverAnchor{};
    const RE::BSLight* receiverLight{};
    float receiverRadiusSquared=0;
    Constants values{};
    V4 snapshotViewport{};
    DXGI_FORMAT viewFormat{};
    uint64_t submittedFrame=~0ull,attemptedFrame=~0ull,receiverFrame=~0ull,snapshotFrame=~0ull,maskFrame=~0ull;
    std::string_view waiting;
} bottleProjection;

