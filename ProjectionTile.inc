// A resting light can reuse its native shadow map across frames. Its last verified viewport is valid until the next Utility draw or scene reset. A tile stores glass and wet path lengths, exit
// distance and coverage. Mip 1 is generated only when caustics request it.
struct TileView { V4 light,x,y,z,rect,size; };
struct Tile {
    const RE::BSGeometry* owner{};
    TileView view{};
    Constants profile{};
    OpticalBuffers::Target target,focus;
    V4 focusExtinction{};
    uint64_t used{};
    bool halfReady{};
};
std::array<Tile,maxPerFrame> tiles{};
ComPtr<ID3D11VertexShader> tileVS;
ComPtr<ID3D11PixelShader> tilePS,focusPS;
ComPtr<ID3D11SamplerState> tileSampler;
ComPtr<ID3D11Buffer> tileBuffer;
struct AtlasSize { const RE::BSGeometry* owner{}; const RE::BSLight* light{}; UINT w{},h{}; uint64_t seen{}; };
std::array<AtlasSize,maxPerFrame> atlasSizes{};
uint64_t tileBuilds{},tileHits{};

void InitTiles(ID3D11Device* dev,const std::filesystem::path& dir){
    ComPtr<ID3DBlob> code;
    if(FAILED(D3DReadFileToBlob((dir/"BottleTileVS.cso").c_str(),&code))||
       FAILED(dev->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&tileVS)))return;
    if(FAILED(D3DReadFileToBlob((dir/"BottleTilePS.cso").c_str(),code.ReleaseAndGetAddressOf()))||
       FAILED(dev->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&tilePS)))return;
    if(FAILED(D3DReadFileToBlob((dir/"BottleCausticTilePS.cso").c_str(),code.ReleaseAndGetAddressOf()))||
       FAILED(dev->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&focusPS)))return;
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(TileView);cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(dev->CreateBuffer(&cb,nullptr,&tileBuffer)))return;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD=1;
    dev->CreateSamplerState(&sd,&tileSampler);
}
void ObserveAtlas(ID3D11DeviceContext* ctx,const RE::BSGeometry* owner,const RE::BSLight* point,uint64_t frame){
    auto found=std::find_if(atlasSizes.begin(),atlasSizes.end(),[&](const auto& a){return a.owner==owner;});
    if(found==atlasSizes.end())found=std::min_element(atlasSizes.begin(),atlasSizes.end(),
        [](const auto& a,const auto& b){return a.seen<b.seen;});
    ComPtr<ID3D11DepthStencilView> ds;
    D3D11_VIEWPORT vp{};
    if(dbl::GetNativePointShadowAtlas(ctx,ds,vp))*found={owner,point,UINT(vp.Width),UINT(vp.Height),frame};
}
Tile* SelectTile(ID3D11DeviceContext* ctx,const RE::BSGeometry* owner,uint64_t frame,bool half){
    if(!tileSampler||values.shape.w>0||!HasReceiverLight(frame))return nullptr;
    const auto atlas=std::find_if(atlasSizes.begin(),atlasSizes.end(),[&](const auto& a){
        return a.owner==owner&&(!a.light||a.light==receiverLight);
    });
    if(atlas==atlasSizes.end())return nullptr;
    const auto& pose=owner->world;
    if(!(pose.scale>0))return nullptr;
    const auto offset=(receiverOrigin-pose.translate)*(1.f/pose.scale);
    const auto& m=pose.rotate.entry;
    TileView v{};
    v.light={m[0][0]*offset.x+m[1][0]*offset.y+m[2][0]*offset.z,
             m[0][1]*offset.x+m[1][1]*offset.y+m[2][1]*offset.z,
             m[0][2]*offset.x+m[1][2]*offset.y+m[2][2]*offset.z,0};
    const RE::NiPoint3 delta{-v.light.x,-v.light.y,values.body.w-v.light.z};
    const float d=delta.Length(),radius=std::hypot(values.body.x,values.body.z);
    if(!(d>radius*1.1f))return nullptr;
    const auto worldDir=pose.rotate*(delta*(1.f/d));
    const float sign=worldDir.x>=0?1.f:-1.f;
    v.x={m[2][0],m[2][1],m[2][2],0};v.y={m[1][0],m[1][1],m[1][2],0};
    v.z={sign*m[0][0],sign*m[0][1],sign*m[0][2],0};
    // The cone enclosing a sphere projects to a circle in a stereographic chart.
    const float sine=radius/d,denom=std::abs(worldDir.x)+std::sqrt(1-sine*sine);
    const float extent=sine/denom;
    const float stepX=2.f/atlas->w,stepY=2.f/atlas->h;
    const float x0=std::floor((worldDir.z/denom-extent)/stepX)-2;
    const float y0=std::floor((worldDir.y/denom-extent)/stepY)-2;
    const auto width=UINT(std::ceil(extent/stepX)+3)*2,height=UINT(std::ceil(extent/stepY)+3)*2;
    if(width>1024||height>1024)return nullptr;
    v.rect={x0*stepX,y0*stepY,1/(width*stepX),1/(height*stepY)};
    v.size={float(width),float(height),0,0};
    auto slot=std::find_if(tiles.begin(),tiles.end(),[&](const Tile& t){return t.owner==owner;});
    if(slot==tiles.end())slot=std::min_element(tiles.begin(),tiles.end(),[](const Tile& a,const Tile& b){return a.used<b.used;});
    auto& t=*slot;
    auto profile=values;
    profile.controls=profile.screen=profile.lighting=profile.absorption={};
    // Store path lengths, so changes in tint or extinction only affect the receiver.
    // Sub-texel ripples in a resting simulation do not warrant a tile rebuild.
    const float axialStep=std::max(.0001f,radius*.0005f);
    profile.plane.w=std::round(profile.plane.w/axialStep)*axialStep;
    for(float* n:{&profile.plane.x,&profile.plane.y,&profile.plane.z})*n=std::round(*n*2048.f)/2048.f;
    const bool valid=t.owner==owner&&t.target.srv&&memcmp(&v,&t.view,sizeof(v))==0&&
                     memcmp(&profile,&t.profile,sizeof(profile))==0;
    if(!valid){
        if(!t.target.srv||t.view.size.x!=v.size.x||t.view.size.y!=v.size.y){
            ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width=width;desc.Height=height;desc.ArraySize=desc.SampleDesc.Count=1;desc.MipLevels=2;
            desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;
            desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            OpticalBuffers::Target target;
            if(FAILED(dev->CreateTexture2D(&desc,nullptr,&target.texture))||
               FAILED(dev->CreateRenderTargetView(target.texture.Get(),nullptr,&target.rt))||
               FAILED(dev->CreateShaderResourceView(target.texture.Get(),nullptr,&target.srv)))return nullptr;
            t.target=std::move(target);t.focus={};
        }
        dblui::GPUCost timing(ctx,half?dblui::GPUStage::CausticTile:dblui::GPUStage::Prepare);
        auto* rt=t.target.rt.Get();ctx->OMSetRenderTargets(1,&rt,nullptr);
        ctx->OMSetBlendState(nullptr,nullptr,~0u);
        D3D11_VIEWPORT vp{0,0,v.size.x,v.size.y,0,1};ctx->RSSetViewports(1,&vp);
        ctx->VSSetShader(tileVS.Get(),nullptr,0);ctx->PSSetShader(tilePS.Get(),nullptr,0);
        ctx->UpdateSubresource(tileBuffer.Get(),0,nullptr,&v,0,0);
        ctx->UpdateSubresource(constants.Get(),0,nullptr,&profile,0,0);
        ctx->Draw(4,0);
        ctx->OMSetRenderTargets(0,nullptr,nullptr);
        dblui::RecordTile(half,dblui::TileEvent::OpticalBuild);
        t.view=v;t.profile=profile;t.owner=owner;t.halfReady=false;++tileBuilds;
    }else ++tileHits;
    const V4 extinction{values.absorption.x,values.absorption.y,values.absorption.z,values.controls.x};
    const bool buildFocus=half&&(!t.halfReady||memcmp(&extinction,&t.focusExtinction,sizeof(extinction)));
    if(buildFocus){
        if(!t.focus.srv){
            ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);
            D3D11_TEXTURE2D_DESC desc{};t.target.texture->GetDesc(&desc);
            desc.Width/=2;desc.Height/=2;desc.MipLevels=1;desc.MiscFlags=0;
            OpticalBuffers::Target focus;
            if(FAILED(dev->CreateTexture2D(&desc,nullptr,&focus.texture))||
               FAILED(dev->CreateRenderTargetView(focus.texture.Get(),nullptr,&focus.rt))||
               FAILED(dev->CreateShaderResourceView(focus.texture.Get(),nullptr,&focus.srv)))return nullptr;
            t.focus=std::move(focus);
        }
        dblui::GPUCost timing(ctx,dblui::GPUStage::CausticTile);
        if(!t.halfReady)ctx->GenerateMips(t.target.srv.Get());
        auto* rt=t.focus.rt.Get();ctx->OMSetRenderTargets(1,&rt,nullptr);
        ctx->OMSetBlendState(nullptr,nullptr,~0u);
        D3D11_VIEWPORT vp{0,0,v.size.x*.5f,v.size.y*.5f,0,1};ctx->RSSetViewports(1,&vp);
        ctx->VSSetShader(tileVS.Get(),nullptr,0);ctx->PSSetShader(focusPS.Get(),nullptr,0);
        ctx->UpdateSubresource(tileBuffer.Get(),0,nullptr,&v,0,0);
        ctx->UpdateSubresource(constants.Get(),0,nullptr,&values,0,0);
        auto* srv=t.target.srv.Get();ctx->PSSetShaderResources(3,1,&srv);
        auto* sampler=tileSampler.Get();ctx->PSSetSamplers(0,1,&sampler);
        ctx->Draw(4,0);
        ctx->OMSetRenderTargets(0,nullptr,nullptr);
        t.halfReady=true;t.focusExtinction=extinction;
        dblui::RecordTile(true,dblui::TileEvent::FocusBuild);
    }
    if(valid&&!buildFocus)dblui::RecordTile(half,dblui::TileEvent::Hit);
    t.used=frame;
    return &t;
}

