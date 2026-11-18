// Versioned adapter for the CS 1.9.1 Light Limit Fix ABI. No CPU readbacks.
struct CSFrameLighting {
    bool supported=false,ready=false,attempted=false;
    ComPtr<ID3D11Buffer> constants[2],resources[3];
    ComPtr<ID3D11ShaderResourceView> views[3];
    void initialize(){
        auto module=GetModuleHandleW(L"CommunityShaders.dll");if(!module)return;
        wchar_t path[MAX_PATH]{};if(!GetModuleFileNameW(module,path,MAX_PATH))return;
        DWORD ignored{};auto size=GetFileVersionInfoSizeW(path,&ignored);if(!size)return;
        std::vector<unsigned char> bytes(size);if(!GetFileVersionInfoW(path,0,size,bytes.data()))return;
        VS_FIXEDFILEINFO* info{};UINT length{};
        if(VerQueryValueW(bytes.data(),L"\\",reinterpret_cast<void**>(&info),&length)&&length>=sizeof(*info))
            supported=info->dwFileVersionMS==MAKELONG(9,1)&&info->dwFileVersionLS==MAKELONG(0,1);
        SKSE::log::info("[DBL-CS-lighting] CS 1.9.1 adapter {}",supported?"enabled":"not enabled: unsupported version");
    }
    static bool bufferSize(ID3D11Buffer* b,UINT minimum){D3D11_BUFFER_DESC d{};if(!b)return false;b->GetDesc(&d);return d.ByteWidth>=minimum;}
    void clear(){ready=attempted=false;}
    static bool readBindings(ID3D11DeviceContext* c,ComPtr<ID3D11Buffer> (&srcCB)[2],ComPtr<ID3D11Buffer> (&srcBuffers)[3],D3D11_SHADER_RESOURCE_VIEW_DESC (&viewDesc)[3]){
        ComPtr<ID3D11ShaderResourceView> srcViews[3];
        c->PSGetConstantBuffers(5,1,&srcCB[0]);c->PSGetConstantBuffers(6,1,&srcCB[1]);
        if(!bufferSize(srcCB[0].Get(),672)||!bufferSize(srcCB[1].Get(),1296))return false;
        const UINT strides[]{80,4,16};
        for(int i=0;i<3;++i){
            c->PSGetShaderResources(35+i,1,&srcViews[i]);if(!srcViews[i])return false;
            srcViews[i]->GetDesc(&viewDesc[i]);if(viewDesc[i].ViewDimension!=D3D11_SRV_DIMENSION_BUFFER)return false;
            ComPtr<ID3D11Resource> resource;srcViews[i]->GetResource(&resource);if(FAILED(resource.As(&srcBuffers[i])))return false;
            D3D11_BUFFER_DESC d{};srcBuffers[i]->GetDesc(&d);if(d.StructureByteStride!=strides[i]||!(d.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_STRUCTURED))return false;
        }
        return true;
    }
    static bool liveBindingsReady(ID3D11DeviceContext* c){
        ComPtr<ID3D11Buffer> cb[2],buffers[3];
        D3D11_SHADER_RESOURCE_VIEW_DESC views[3]{};
        return readBindings(c,cb,buffers,views);
    }
    bool capture(ID3D11DeviceContext* c){
        if(attempted)return ready;attempted=true;if(!supported)return false;
        ComPtr<ID3D11Device> device;c->GetDevice(&device);
        ComPtr<ID3D11Buffer> srcCB[2];ComPtr<ID3D11Buffer> srcBuffers[3];
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc[3]{};
        if(!readBindings(c,srcCB,srcBuffers,viewDesc))return false;
        auto copy=[&](ID3D11Buffer* source,ComPtr<ID3D11Buffer>& target,UINT bind,bool structured){
            D3D11_BUFFER_DESC d{},old{};source->GetDesc(&d);if(target)target->GetDesc(&old);
            if(!target||old.ByteWidth!=d.ByteWidth||old.StructureByteStride!=d.StructureByteStride){
                target.Reset();d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=0;d.BindFlags=bind;
                d.MiscFlags=structured?D3D11_RESOURCE_MISC_BUFFER_STRUCTURED:0;if(!structured)d.StructureByteStride=0;
                check(device->CreateBuffer(&d,nullptr,&target));
            }
            {dblui::GPUCost timing(c,dblui::GPUStage::Prepare);c->CopyResource(target.Get(),source);}
        };
        for(int i=0;i<2;++i)copy(srcCB[i].Get(),constants[i],D3D11_BIND_CONSTANT_BUFFER,false);
        for(int i=0;i<3;++i){auto* old=resources[i].Get();copy(srcBuffers[i].Get(),resources[i],D3D11_BIND_SHADER_RESOURCE,true);
            if(!views[i]||old!=resources[i].Get()){views[i].Reset();check(device->CreateShaderResourceView(resources[i].Get(),&viewDesc[i],&views[i]));}}
        ready=true;return true;
    }
    void bind(ID3D11DeviceContext* c){
        ID3D11Buffer* cb[]{constants[0].Get(),constants[1].Get()};c->PSSetConstantBuffers(5,2,cb);
        ID3D11ShaderResourceView* v[]{views[0].Get(),views[1].Get(),views[2].Get()};c->PSSetShaderResources(35,3,v);
    }
} communityLighting;

struct CSLightingRestore {
    ID3D11DeviceContext* c;ComPtr<ID3D11DeviceContext1> extended;
    ComPtr<ID3D11Buffer> cb[3];ComPtr<ID3D11ShaderResourceView> views[3];UINT first[3]{},count[3]{};bool active;
    CSLightingRestore(ID3D11DeviceContext* context,bool enabled):c(context),active(enabled){
        if(!active)return;c->QueryInterface(IID_PPV_ARGS(&extended));
        for(UINT i=0;i<3;++i){UINT slot=i?i+4:3;if(extended)extended->PSGetConstantBuffers1(slot,1,&cb[i],first+i,count+i);else c->PSGetConstantBuffers(slot,1,&cb[i]);c->PSGetShaderResources(35+i,1,&views[i]);}
    }
    ~CSLightingRestore(){if(!active)return;
        for(UINT i=0;i<3;++i){UINT slot=i?i+4:3;auto* b=cb[i].Get();if(extended)extended->PSSetConstantBuffers1(slot,1,&b,first+i,count+i);else c->PSSetConstantBuffers(slot,1,&b);auto* v=views[i].Get();c->PSSetShaderResources(35+i,1,&v);}
    }
};
