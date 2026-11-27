// The verified fitting shadow pass reads positions only. Both the fitting and
// glass share the same transform, so the original mesh buffers can be rebound.
struct NativeOpaqueShadow {
    static bool VerifiedFittingBody(const RE::BSGeometry& body){
        return body.name=="DBL_SSE_TMDWinery_winebottle01jessica"||body.name=="DBL_SSE_XMead_Ashfire";
    }

    struct Stats { size_t fits{},atlas{},draws{},confirmed{}; };

    Stats GetStats(uint64_t frame) const { return drawFrame==frame?stats:Stats{}; }

    bool Covers(const RE::BSGeometry* body,const RE::BSLight* light,uint64_t frame) const {
        if(drawFrame!=frame)return false;
        return std::any_of(covered.begin(),covered.begin()+stats.confirmed,[&](const auto& entry){
            return entry.body==body&&(!light||!entry.light||entry.light==light);
        });
    }

    void Draw(ID3D11DeviceContext* ctx,const RE::BSGeometry& body,const RE::BSGeometry& fitting,
              const RE::BSLight* light,UINT fittingIndices,uint64_t frame){
        if(!body.parent||body.parent!=fitting.parent||fitting.GetType()!=RE::BSGeometry::Type::kTriShape||
           body.GetType()!=RE::BSGeometry::Type::kTriShape)return;
        const bool verified=VerifiedFittingBody(body);
        const bool wine=body.name=="DBL_SSE_TMDWinery_winebottle01jessica";
        if(verified){
            if(wine?(fitting.name!="Cork"||fittingIndices!=432):
                    (fitting.name!="MeadSolid:3"||fittingIndices!=324))return;
        }else if(fitting.GetGeometryRuntimeData().properties[0]||fitting.GetGeometryRuntimeData().skinInstance||
                 body.GetGeometryRuntimeData().skinInstance||
                 fittingIndices!=3u*static_cast<const RE::BSTriShape&>(fitting).GetTrishapeRuntimeData().triangleCount)return;
        if(ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
        BeginFrame(ctx,frame);
        ++stats.fits;

        ComPtr<ID3D11DepthStencilView> depth;
        D3D11_VIEWPORT viewport{};
        if(!dbl::GetNativePointShadowAtlas(ctx,depth,viewport))return;
        ++stats.atlas;
        if(!SamePose(fitting.world,body.world,1e-5f,1e-4f))return;

        D3D11_PRIMITIVE_TOPOLOGY topology{};
        ctx->IAGetPrimitiveTopology(&topology);
        if(topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)return;
        const auto* fittingMesh=fitting.GetGeometryRuntimeData().rendererData;
        const auto* bodyMesh=body.GetGeometryRuntimeData().rendererData;
        if(!fittingMesh||!bodyMesh||!bodyMesh->vertexBuffer||!bodyMesh->indexBuffer||
           body.GetType()!=RE::BSGeometry::Type::kTriShape)return;
        const UINT count=3u*static_cast<const RE::BSTriShape&>(body).GetTrishapeRuntimeData().triangleCount;
        auto vertexDesc=bodyMesh->vertexDesc;
        const UINT stride=vertexDesc.GetSize();
        auto fittingDesc=fittingMesh->vertexDesc;
        if(verified){
            if(count!=(wine?9384u:1338u)||stride!=(wine?32u:28u))return;
        }else if(!count||!stride||memcmp(&vertexDesc,&fittingDesc,sizeof(vertexDesc))!=0)return;
        ComPtr<ID3D11Buffer> oldVertices,oldIndices;
        UINT oldStride=0,oldVertexOffset=0,oldIndexOffset=0;
        DXGI_FORMAT indexFormat{};
        ctx->IAGetVertexBuffers(0,1,&oldVertices,&oldStride,&oldVertexOffset);
        ctx->IAGetIndexBuffer(&oldIndices,&indexFormat,&oldIndexOffset);
        if(oldStride!=(verified?28u:fittingDesc.GetSize())||oldVertexOffset||oldIndexOffset||indexFormat!=DXGI_FORMAT_R16_UINT||
           oldVertices.Get()!=reinterpret_cast<ID3D11Buffer*>(fittingMesh->vertexBuffer)||
           oldIndices.Get()!=reinterpret_cast<ID3D11Buffer*>(fittingMesh->indexBuffer))return;

        const SampleKey key{&body,light,depth.Get(),viewport.TopLeftX,viewport.TopLeftY,body.world};
        const Probe* latest=nullptr;
        Probe* reusable=nullptr;
        for(auto& probe:probes){
            if(probe.frame==frame&&Matches(probe.key,key))return;
            if(probe.pending)continue;
            if(probe.frame!=noFrame&&frame-probe.frame<=confirmationFrames&&Matches(probe.key,key)&&
               (!latest||probe.frame>latest->frame))latest=&probe;
            if(probe.frame!=frame&&(!reusable||probe.frame==noFrame||
               (reusable->frame!=noFrame&&probe.frame<reusable->frame)))reusable=&probe;
        }
        if(!reusable)return;
        if(!reusable->query){
            ComPtr<ID3D11Device> device;
            ctx->GetDevice(&device);
            const D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION,0};
            if(FAILED(device->CreateQuery(&desc,&reusable->query)))return;
        }
        const bool confirmed=latest&&latest->samples>0;
        reusable->key=key;
        reusable->frame=frame;
        reusable->pending=true;

        const UINT offset=0;
        auto* vertices=reinterpret_cast<ID3D11Buffer*>(bodyMesh->vertexBuffer);
        ctx->IASetVertexBuffers(0,1,&vertices,&stride,&offset);
        ctx->IASetIndexBuffer(reinterpret_cast<ID3D11Buffer*>(bodyMesh->indexBuffer),DXGI_FORMAT_R16_UINT,0);
        ComPtr<ID3DUserDefinedAnnotation> annotation;
        ctx->QueryInterface(IID_PPV_ARGS(&annotation));
        const bool marked=annotation&&annotation->GetStatus();
        if(marked)annotation->BeginEvent(L"DBL native opaque bottle caster");
        {
            dblui::GPUCost timing(ctx,dblui::GPUStage::Compose);
            ctx->Begin(reusable->query.Get());
            oldDraw(ctx,count,0,0);
            ctx->End(reusable->query.Get());
        }
        if(marked)annotation->EndEvent();
        vertices=oldVertices.Get();
        ctx->IASetVertexBuffers(0,1,&vertices,&oldStride,&oldVertexOffset);
        ctx->IASetIndexBuffer(oldIndices.Get(),indexFormat,oldIndexOffset);
        ++stats.draws;
        if(confirmed&&!Covers(&body,light,frame)&&stats.confirmed<covered.size())
            covered[stats.confirmed++]={&body,light};
    }

private:
    static constexpr uint64_t noFrame=~0ull;
    static constexpr uint64_t confirmationFrames=6;
    struct Coverage {
        const RE::BSGeometry* body{};
        const RE::BSLight* light{};
    };
    struct SampleKey {
        const RE::BSGeometry* body{};
        const RE::BSLight* light{};
        const ID3D11DepthStencilView* view{};
        float x{},y{};
        RE::NiTransform pose;
    };
    struct Probe {
        ComPtr<ID3D11Query> query;
        SampleKey key;
        uint64_t frame=noFrame;
        UINT64 samples{};
        bool pending=false;
    };

    static bool SamePose(const RE::NiTransform& a,const RE::NiTransform& b,
                         float positionSquaredTolerance,float rotationTolerance){
        if(!((a.translate-b.translate).SqrLength()<=positionSquaredTolerance&&
             std::abs(a.scale-b.scale)<=1e-4f))return false;
        for(size_t row=0;row<3;++row)
            for(size_t column=0;column<3;++column)
                if(!(std::abs(a.rotate.entry[row][column]-b.rotate.entry[row][column])<=rotationTolerance))return false;
        return true;
    }

    static bool Matches(const SampleKey& previous,const SampleKey& current){
        return previous.body==current.body&&previous.light==current.light&&previous.view==current.view&&
               previous.x==current.x&&previous.y==current.y&&SamePose(previous.pose,current.pose,.01f,1e-3f);
    }

    void BeginFrame(ID3D11DeviceContext* ctx,uint64_t frame){
        if(drawFrame==frame)return;
        drawFrame=frame;
        stats={};
        // Keep the preview until a recent draw in this view and pose covered
        // depth samples. Never wait for a shadow result on the render thread.
        for(auto& probe:probes){
            if(!probe.pending)continue;
            const auto result=ctx->GetData(probe.query.Get(),&probe.samples,sizeof(probe.samples),
                                           D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(result==S_OK)probe.pending=false;
            else if(FAILED(result)){probe.pending=false;probe.frame=noFrame;}
        }
    }

    uint64_t drawFrame=noFrame;
    Stats stats;
    std::array<Coverage,16> covered;
    std::array<Probe,64> probes;
} nativeOpaqueShadow;
