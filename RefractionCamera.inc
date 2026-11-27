// Bind the same ranged camera constants the bottle vertex shader uses.
struct RefractionCamera {
    ComPtr<ID3D11DeviceContext1> extended;
    NativeConstantSlice camera;

    void read(ID3D11DeviceContext* context){
        context->QueryInterface(IID_PPV_ARGS(&extended));
        camera.Read(context,extended.Get(),true,12);
        if(!camera.Contains(12))camera={};
    }
};

struct RefractionCameraBinding {
    ID3D11DeviceContext* context;
    const RefractionCamera* camera;
    NativeConstantSlice previous;
    bool compute;

    RefractionCameraBinding(ID3D11DeviceContext* context,const RefractionCamera* camera,bool compute)
        :context(context),camera(camera&&camera->camera.buffer?camera:nullptr),compute(compute){
        if(!this->camera)return;
        auto* ranged=camera->extended.Get();
        if(compute){
            if(ranged)ranged->CSGetConstantBuffers1(12,1,&previous.buffer,&previous.first,&previous.count);
            else context->CSGetConstantBuffers(12,1,&previous.buffer);
        }else previous.Read(context,ranged,false,12);
        bind(camera->camera);
    }
    ~RefractionCameraBinding(){
        if(camera)bind(previous);
    }
private:
    void bind(const NativeConstantSlice& slice){
        auto* ranged=camera->extended.Get();
        if(compute){
            auto* buffer=slice.buffer.Get();
            if(ranged)ranged->CSSetConstantBuffers1(12,1,&buffer,&slice.first,&slice.count);
            else context->CSSetConstantBuffers(12,1,&buffer);
        }else slice.Bind(context,ranged,false,12);
    }
};
