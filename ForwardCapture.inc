// Snapshot mutable native constants at their draw, using one GPU dispatch.
// 32 records fit in a <64KB constant buffer. Each shader binding has a
// 256-byte-aligned region, preserving D3D11.1 constant-buffer range semantics.
struct ForwardCapture {
    static constexpr UINT stride=1792,perChunk=32;
    struct Chunk {ComPtr<ID3D11Buffer> writable,constants,strictWritable,strictConstants;ComPtr<ID3D11UnorderedAccessView> views[perChunk],strictViews[perChunk];bool strictUsed=false;};
    std::vector<Chunk> chunks;ComPtr<ID3D11ComputeShader> shader,csShader;
    bool supported=false;
    void initialize(ID3D11Device* device){
        communityLighting.initialize();
        D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
        supported=SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,&options,sizeof(options)))&&options.ConstantBufferOffsetting;
        if(!supported)return;

        ComPtr<ID3DBlob> code,error;
        check(D3DReadFileToBlob((root/"NativeCapture.cso").c_str(),&code));
        check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
        code.Reset();check(D3DReadFileToBlob((root/"NativeCaptureCS.cso").c_str(),&code));
        check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&csShader));
    }
    bool capture(ID3D11DeviceContext* c,ForwardBindings& bindings,size_t index){
        if(!supported||!bindings.extended)return false;
        auto* extended=bindings.extended.Get();auto chunkIndex=index/perChunk;auto slot=index%perChunk;
        if(chunks.size()<=chunkIndex)chunks.resize(chunkIndex+1);auto& chunk=chunks[chunkIndex];
        if(!chunk.constants){
            Chunk pending;
            ComPtr<ID3D11Device> device;c->GetDevice(&device);
            D3D11_BUFFER_DESC d{};d.ByteWidth=stride*perChunk;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            check(device->CreateBuffer(&d,nullptr,&pending.constants));
            d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            check(device->CreateBuffer(&d,nullptr,&pending.writable));
            for(UINT i=0;i<perChunk;++i){D3D11_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32_TYPELESS;view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;view.Buffer.FirstElement=i*stride/4;view.Buffer.NumElements=stride/4;view.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
                check(device->CreateUnorderedAccessView(pending.writable.Get(),&view,&pending.views[i]));}
            chunk=std::move(pending);
        }
        bool csActive=bool(bindings.csStrict);
        if(slot==0)chunk.strictUsed=false;
        if(csActive&&!chunk.strictConstants){
            ComPtr<ID3D11Device> device;c->GetDevice(&device);D3D11_BUFFER_DESC d{};
            d.ByteWidth=1280*perChunk;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            check(device->CreateBuffer(&d,nullptr,&chunk.strictConstants));d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            check(device->CreateBuffer(&d,nullptr,&chunk.strictWritable));
            for(UINT i=0;i<perChunk;++i){D3D11_UNORDERED_ACCESS_VIEW_DESC v{};v.Format=DXGI_FORMAT_R32_TYPELESS;v.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;v.Buffer.FirstElement=i*320;v.Buffer.NumElements=320;v.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
                check(device->CreateUnorderedAccessView(chunk.strictWritable.Get(),&v,&chunk.strictViews[i]));}
        }
        // The engine may use compute between bottle draws. Restore every touched
        // binding, including ranges, UAV counters and shader class instances.
        ComPtr<ID3D11ComputeShader> previousShader;ID3D11ClassInstance* instances[256]{};UINT instanceCount=256;
        c->CSGetShader(&previousShader,instances,&instanceCount);
        ID3D11Buffer* previousCB[7]{};UINT first[7]{},count[7]{};UINT cbCount=7;
        extended->CSGetConstantBuffers1(0,cbCount,previousCB,first,count);
        ComPtr<ID3D11UnorderedAccessView> previousStrictUAV;if(csActive)c->CSGetUnorderedAccessViews(1,1,&previousStrictUAV);
        ComPtr<ID3D11UnorderedAccessView> previousUAV;c->CSGetUnorderedAccessViews(0,1,&previousUAV);
        ID3D11Buffer* sources[]{bindings.vsCB[0].Get(),bindings.vsCB[1].Get(),bindings.vsCB[2].Get(),bindings.psCB[0].Get(),bindings.psCB[1].Get(),bindings.csStrict.Get(),bindings.fog.Get()};
        UINT offsets[]{bindings.vsFirst[0],bindings.vsFirst[1],bindings.vsFirst[2],bindings.psFirst[0],bindings.psFirst[1],bindings.csFirst,bindings.fogFirst};
        UINT sizes[]{bindings.vsCount[0],bindings.vsCount[1],bindings.vsCount[2],bindings.psCount[0],bindings.psCount[1],bindings.csCount,bindings.fogCount};
        auto* view=chunk.views[slot].Get();c->CSSetUnorderedAccessViews(0,1,&view,nullptr);
        if(csActive){auto* v=chunk.strictViews[slot].Get();c->CSSetUnorderedAccessViews(1,1,&v,nullptr);}
        extended->CSSetConstantBuffers1(0,cbCount,sources,offsets,sizes);c->CSSetShader(csActive?csShader.Get():shader.Get(),nullptr,0);{dblui::GPUCost timing(c,dblui::GPUStage::Prepare);c->Dispatch(1,1,1);}
        view=previousUAV.Get();c->CSSetUnorderedAccessViews(0,1,&view,nullptr);
        if(csActive){auto* v=previousStrictUAV.Get();c->CSSetUnorderedAccessViews(1,1,&v,nullptr);}
        extended->CSSetConstantBuffers1(0,cbCount,previousCB,first,count);c->CSSetShader(previousShader.Get(),instances,instanceCount);
        for(auto* b:previousCB)if(b)b->Release();for(UINT i=0;i<instanceCount;++i)instances[i]->Release();
        UINT base=UINT(slot)*stride/16;
        for(int i=0;i<3;++i){bindings.vsCB[i]=chunk.constants;bindings.vsFirst[i]=base+16*i;bindings.vsCount[i]=16;}
        for(int i=0;i<2;++i){bindings.psCB[i]=chunk.constants;bindings.psFirst[i]=base+48+16*i;bindings.psCount[i]=i==0?16:32;}
        if(csActive){bindings.csStrict=chunk.strictConstants;bindings.csFirst=UINT(slot)*80;bindings.csCount=80;chunk.strictUsed=true;}
        bindings.fog=chunk.constants;bindings.fogFirst=base+96;bindings.fogCount=16;
        return true;
    }
    void finish(ID3D11DeviceContext* c,size_t records){
        dblui::GPUCost timing(c,dblui::GPUStage::Prepare,records!=0);
        for(size_t i=0;i<(records+perChunk-1)/perChunk;++i){c->CopyResource(chunks[i].constants.Get(),chunks[i].writable.Get());if(chunks[i].strictUsed)c->CopyResource(chunks[i].strictConstants.Get(),chunks[i].strictWritable.Get());}
    }
};
