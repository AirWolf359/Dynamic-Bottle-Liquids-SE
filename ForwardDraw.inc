#include "CommunityLighting.inc"
// Fallout's shared-scene forward resolve, deferred to Skyrim's verified HDR
// boundary. Native constant buffers are mutable: retain GPU copies, not pointers.
struct ForwardBindings {
    static constexpr UINT vsSlots[3]{1,2,12},psSlots[3]{1,2,10},textureSlots[4]{0,1,4,13},samplerSlots[5]{0,1,4,13,15};
    ComPtr<ID3D11Buffer> vertex,index,vsCB[3],psCB[3],csStrict,fog;UINT fogFirst{},fogCount{};UINT csFirst{},csCount{};
    ComPtr<ID3D11ShaderResourceView> textures[4];ComPtr<ID3D11SamplerState> samplers[5];
    UINT stride{},offset{},indexOffset{};DXGI_FORMAT indexFormat{};
    ComPtr<ID3D11DeviceContext1> extended;UINT vsFirst[3]{},vsCount[3]{},psFirst[3]{},psCount[3]{};
    void read(ID3D11DeviceContext* c,ID3D11DeviceContext1* known=nullptr,bool ownBottle=false){
        extended=known;if(!extended)c->QueryInterface(IID_PPV_ARGS(&extended));
        c->IAGetVertexBuffers(0,1,vertex.ReleaseAndGetAddressOf(),&stride,&offset);
        c->IAGetIndexBuffer(index.ReleaseAndGetAddressOf(),&indexFormat,&indexOffset);
        ID3D11Buffer* v[3]{},*p[3]{};
        if(extended){
            extended->VSGetConstantBuffers1(1,2,v,vsFirst,vsCount);extended->VSGetConstantBuffers1(12,1,v+2,vsFirst+2,vsCount+2);
            extended->PSGetConstantBuffers1(1,2,p,psFirst,psCount);if(!ownBottle)extended->PSGetConstantBuffers1(10,1,p+2,psFirst+2,psCount+2);
        }else{
            c->VSGetConstantBuffers(1,2,v);c->VSGetConstantBuffers(12,1,v+2);c->PSGetConstantBuffers(1,2,p);if(!ownBottle)c->PSGetConstantBuffers(10,1,p+2);
        }
        if(extended)extended->VSGetConstantBuffers1(0,1,&fog,&fogFirst,&fogCount);else c->VSGetConstantBuffers(0,1,&fog);
        for(int i=0;i<3;++i){vsCB[i].Attach(v[i]);psCB[i].Attach(p[i]);}
        if(ownBottle&&communityLighting.supported){
            if(extended)extended->PSGetConstantBuffers1(3,1,&csStrict,&csFirst,&csCount);else c->PSGetConstantBuffers(3,1,&csStrict);
            if(!CSFrameLighting::bufferSize(csStrict.Get(),1216))csStrict.Reset();
        }
        for(int i=0;i<(ownBottle?3:4);++i)c->PSGetShaderResources(textureSlots[i],1,textures[i].ReleaseAndGetAddressOf());
        for(int i=0;i<(ownBottle?3:5);++i)c->PSGetSamplers(samplerSlots[i],1,samplers[i].ReleaseAndGetAddressOf());
    }
    void bind(ID3D11DeviceContext* c,const ForwardBindings* previous=nullptr,bool replay=false)const{
        if(!previous||vertex!=previous->vertex||stride!=previous->stride||offset!=previous->offset){auto* v=vertex.Get();c->IASetVertexBuffers(0,1,&v,&stride,&offset);}
        if(!previous||index!=previous->index||indexFormat!=previous->indexFormat||indexOffset!=previous->indexOffset)c->IASetIndexBuffer(index.Get(),indexFormat,indexOffset);
        ID3D11Buffer* a[]{vsCB[0].Get(),vsCB[1].Get(),vsCB[2].Get()},*b[]{psCB[0].Get(),psCB[1].Get(),psCB[2].Get()};
        if(extended){
            extended->VSSetConstantBuffers1(1,2,a,vsFirst,vsCount);extended->VSSetConstantBuffers1(12,1,a+2,vsFirst+2,vsCount+2);
            extended->PSSetConstantBuffers1(1,2,b,psFirst,psCount);extended->PSSetConstantBuffers1(10,1,b+2,psFirst+2,psCount+2);
        }else{
            c->VSSetConstantBuffers(1,2,a);c->VSSetConstantBuffers(12,1,a+2);c->PSSetConstantBuffers(1,2,b);c->PSSetConstantBuffers(10,1,b+2);
        }
        auto* fogBuffer=fog.Get();if(extended)extended->VSSetConstantBuffers1(0,1,&fogBuffer,&fogFirst,&fogCount);else c->VSSetConstantBuffers(0,1,&fogBuffer);
        if(csStrict){auto* b=csStrict.Get();if(extended)extended->PSSetConstantBuffers1(3,1,&b,&csFirst,&csCount);else c->PSSetConstantBuffers(3,1,&b);}
        for(int i=0;i<4;++i)if(!previous||textures[i]!=previous->textures[i]){auto* t=textures[i].Get();c->PSSetShaderResources(textureSlots[i],1,&t);}
        for(int i=0;i<(replay?4:5);++i)if(!previous||samplers[i]!=previous->samplers[i]){auto* s=samplers[i].Get();c->PSSetSamplers(samplerSlots[i],1,&s);}
    }
};
#include "ForwardCapture.inc"
struct ForwardDraws {
    static constexpr UINT bottleStride=((sizeof(GPU)+255)/256)*256,bottlesPerUpload=65536/bottleStride;
    struct Record {ForwardBindings state;ComPtr<ID3D11Buffer> copies[7],singleUpload,strictCopy;GPU bottleData{};bool upload=false;UINT count{},start{};INT base{};};
    std::vector<Record> records;size_t used=0;
    std::vector<ComPtr<ID3D11Buffer>> uploads;
    ForwardCapture packing;bool optimized=true,packed=false;
    bool lastPacked=false;size_t lastTransfers=0;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> cachedContext;ComPtr<ID3D11DeviceContext1> extended;
    ComPtr<ID3D11PixelShader> shader;
    SceneSnapshot snapshot;
    bool enabled=false,borrowSceneDepth=true;
    void capture(ID3D11DeviceContext* c,UINT count,UINT start,INT base,const GPU* bottleData=nullptr,ForwardBindings* captured=nullptr){
        if(used==records.size())records.emplace_back();auto& r=records[used];
        if(cachedContext.Get()!=c){cachedContext=c;device.Reset();extended.Reset();c->GetDevice(&device);c->QueryInterface(IID_PPV_ARGS(&extended));}
        if(captured)r.state=std::move(*captured);else r.state.read(c,extended.Get(),optimized&&bottleData);
        bool csActive=r.state.csStrict&&communityLighting.capture(c);
        if(communityLighting.supported){static unsigned reports=0;if(reports<3){++reports;SKSE::log::info("[DBL-CS-lighting] draw lighting={} strict_buffer={} frame_snapshot={}",csActive?"CS":"vanilla fallback",bool(r.state.csStrict),communityLighting.ready);}}
        GPU adapted{};if(bottleData&&csActive){adapted=*bottleData;adapted.eye.w=float(unsigned(adapted.eye.w)|4u);bottleData=&adapted;}
        if(!csActive)r.state.csStrict.Reset();
        bool nativePacked=optimized&&packing.capture(c,r.state,used);packed|=nativePacked;
        r.upload=optimized&&bottleData&&packing.supported&&extended;
        if(optimized&&bottleData){
            if(!r.upload){
                if(!r.singleUpload){D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(GPU);d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;d.Usage=D3D11_USAGE_DYNAMIC;d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;check(device->CreateBuffer(&d,nullptr,&r.singleUpload));}
                D3D11_MAPPED_SUBRESOURCE m{};check(c->Map(r.singleUpload.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&m));memcpy(m.pData,bottleData,sizeof(GPU));c->Unmap(r.singleUpload.Get(),0);
                r.state.psCB[2]=r.singleUpload;r.state.psFirst[2]=0;r.state.psCount[2]=4096;
            }else{
            auto chunk=used/bottlesPerUpload;if(uploads.size()<=chunk)uploads.resize(chunk+1);
            if(!uploads[chunk]){D3D11_BUFFER_DESC d{};d.ByteWidth=bottleStride*bottlesPerUpload;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;d.Usage=D3D11_USAGE_DYNAMIC;d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;check(device->CreateBuffer(&d,nullptr,&uploads[chunk]));}
            r.bottleData=*bottleData;r.state.psCB[2]=uploads[chunk];r.state.psFirst[2]=UINT(used%bottlesPerUpload)*bottleStride/16;r.state.psCount[2]=bottleStride/16;
            }
            r.state.textures[3]=rippleTexture.view;r.state.samplers[3]=rippleTexture.sampler;
        }
        for(int i=0;i<7;++i){
            if(((i<5||i==6)&&nativePacked)||(i==5&&optimized&&bottleData))continue;
            auto& source=i==6?r.state.fog:i<3?r.state.vsCB[i]:r.state.psCB[i-3];
            if(!source)continue;
            D3D11_BUFFER_DESC desc{},existing{};source->GetDesc(&desc);if(r.copies[i])r.copies[i]->GetDesc(&existing);
            if(!r.copies[i]||existing.ByteWidth!=desc.ByteWidth){
                desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=desc.MiscFlags=0;desc.StructureByteStride=0;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
                r.copies[i].Reset();check(device->CreateBuffer(&desc,nullptr,&r.copies[i]));
            }
            {dblui::GPUCost timing(c,dblui::GPUStage::Prepare);c->CopyResource(r.copies[i].Get(),source.Get());}source=r.copies[i];
        }
        if(csActive&&!nativePacked){
            D3D11_BUFFER_DESC d{},old{};r.state.csStrict->GetDesc(&d);if(r.strictCopy)r.strictCopy->GetDesc(&old);
            if(!r.strictCopy||old.ByteWidth!=d.ByteWidth){r.strictCopy.Reset();d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=d.MiscFlags=d.StructureByteStride=0;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;check(device->CreateBuffer(&d,nullptr,&r.strictCopy));}
            {dblui::GPUCost timing(c,dblui::GPUStage::Prepare);c->CopyResource(r.strictCopy.Get(),r.state.csStrict.Get());}r.state.csStrict=r.strictCopy;
        }
        r.count=count;r.start=start;r.base=base;++used;
    }
    void clear(){for(size_t i=0;i<used;++i)records[i].state={};used=0;packed=false;communityLighting.clear();}
    void finishCapture(ID3D11DeviceContext* c){
        if(packed)packing.finish(c,used);
        for(size_t begin=0;begin<used;begin+=bottlesPerUpload){
            size_t end=std::min(used,begin+bottlesPerUpload);bool needed=false;for(size_t i=begin;i<end;++i)needed|=records[i].upload;if(!needed)continue;
            auto* buffer=uploads[begin/bottlesPerUpload].Get();D3D11_MAPPED_SUBRESOURCE mapped{};check(c->Map(buffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped));
            for(size_t i=begin;i<end;++i)if(records[i].upload)memcpy(static_cast<uint8_t*>(mapped.pData)+(i-begin)*bottleStride,&records[i].bottleData,sizeof(GPU));
            c->Unmap(buffer,0);
        }
    }
    void draw(ID3D11DeviceContext* c,DrawFn original,ID3D11RenderTargetView* target,
        ID3D11DepthStencilView* sceneDepth,ID3D11DepthStencilView* nearestDepth,ID3D11BlendState* opaque,ID3D11DepthStencilState* nearest,
        const D3D11_VIEWPORT& viewport){
        CSLightingRestore csRestore(c,communityLighting.ready);
        OpticalState pipeline(c);ForwardBindings saved;saved.read(c);
        struct Restore {ID3D11DeviceContext* c;ForwardBindings& b;~Restore(){b.bind(c);}} restore{c,saved};
        V4 info{},view{};
        c->OMSetRenderTargets(1,&target,sceneDepth);c->RSSetViewports(1,&viewport);
        {
            lastPacked=packed;lastTransfers=packed?(used+ForwardCapture::perChunk-1)/ForwardCapture::perChunk:used*5;
            finishCapture(c);
            if(!snapshot.capture(c,info,view,borrowSceneDepth))throw std::runtime_error("Forward final scene snapshot unavailable: "+snapshot.status);
        }
        c->OMSetRenderTargets(1,&target,nearestDepth);c->OMSetBlendState(opaque,nullptr,~0u);c->OMSetDepthStencilState(nearest,0);
        c->RSSetViewports(1,&viewport);c->RSSetState(raster.Get());c->VSSetShader(vs.Get(),nullptr,0);c->PSSetShader(shader.Get(),nullptr,0);c->GSSetShader(nullptr,nullptr,0);
        c->IASetInputLayout(layout.Get());c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        auto* scene=snapshot.colorView.Get();auto* depth=snapshot.sampledDepth();auto* sampler=snapshot.sampler.Get();
        auto* blurred=snapshot.blur.view();c->PSSetShaderResources(12,1,&blurred);c->PSSetShaderResources(14,1,&depth);c->PSSetShaderResources(15,1,&scene);
        c->PSSetSamplers(15,1,&sampler);
        if(communityLighting.ready)communityLighting.bind(c);
        const bool detailed=dblui::Extensive();
        dblui::GPUCost passTiming(c,dblui::GPUStage::Draw,!detailed&&used!=0);
        for(size_t i=0;i<used;++i){
            const auto& r=records[i];r.state.bind(c,optimized&&i?&records[i-1].state:nullptr,true);
            dblui::GPUCost timing(c,dblui::GPUStage::Draw,detailed);original(c,r.count,r.start,r.base);
        }
    }
} forwardDraws;
