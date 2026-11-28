
// Replace only the transmitted image. The native material draws its lit color
// over this image using the menu coverage immediately afterwards.
bool DrawShellTransmission(ID3D11DeviceContext* ctx,UINT count,UINT start,INT base,float bend,int mip){
    if(bend<=0&&mip<=0)return false;
    static SceneSnapshot snapshot;
    V4 info{},area{};
    ComPtr<ID3D11RenderTargetView> target;
    ctx->OMGetRenderTargets(1,&target,nullptr);
    RefractionCamera refractionCamera;
    // The refraction camera reads one view's constants, so VR doesn't use it.
    if(!vrStereo&&IsMainSceneTarget(target.Get()))refractionCamera.read(ctx);
    const auto* camera=refractionCamera.camera.buffer?&refractionCamera:nullptr;
    try{if(!snapshot.capture(ctx,info,area,false,true,camera)){
        if constexpr(vrStereo)ReportShellRefraction(std::format("Not applied: {}",snapshot.status));
        return false;
    }}
    catch(const std::exception& e){static bool logged=false;if(!logged){SKSE::log::warn("Shell refraction unavailable: {}",e.what());logged=true;}return false;}
    if(mip>0){
        snapshot.blur.invalidate();
        try{snapshot.blur.buildUnsafe(ctx,snapshot.colorView.Get(),snapshot.sampledDepth(),snapshot.color.Get(),snapshot.width,snapshot.height,
                                      camera,UINT(mip));}
        catch(const std::exception&){mip=0;}
    }
    const TmdShellConstants values{{bend,float(mip),snapshot.blur.reverse?-info.y:info.y,info.z},
                                   {area.x,area.y,area.z,area.w},camera?1.f:0.f};
    ctx->UpdateSubresource(shellOpticsBuffer.Get(),0,nullptr,&values,0,0);
    ComPtr<ID3D11VertexShader> oldVS;ComPtr<ID3D11PixelShader> oldPS;
    ComPtr<ID3D11InputLayout> oldLayout;ComPtr<ID3D11SamplerState> oldSampler;
    ComPtr<ID3D11DepthStencilState> oldDepth;UINT stencil;
    ComPtr<ID3D11BlendState> oldBlend;FLOAT factors[4];UINT mask;
    ID3D11ShaderResourceView* resources[3]{};
    ctx->VSGetShader(&oldVS,nullptr,nullptr);ctx->PSGetShader(&oldPS,nullptr,nullptr);
    ctx->IAGetInputLayout(&oldLayout);ctx->PSGetSamplers(15,1,&oldSampler);
    ctx->OMGetDepthStencilState(&oldDepth,&stencil);ctx->OMGetBlendState(&oldBlend,factors,&mask);
    ctx->PSGetShaderResources(13,3,resources);
    BottleConstantBinding constants(ctx,nullptr,shellOpticsBuffer.Get());
    RefractionCameraBinding cameraBinding(ctx,camera,false);
    ID3D11RenderTargetView* targets[8]{};ComPtr<ID3D11DepthStencilView> z;
    ctx->OMGetRenderTargets(8,targets,&z);ctx->OMSetRenderTargets(1,targets,z.Get());
    ID3D11ShaderResourceView* inputs[]={mip?snapshot.blur.chainView.Get():nullptr,snapshot.sampledDepth(),snapshot.colorView.Get()};
    auto* sampler=snapshot.sampler.Get();ctx->PSSetShaderResources(13,3,inputs);ctx->PSSetSamplers(15,1,&sampler);
    ctx->IASetInputLayout(layout.Get());ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(shellOpticsPS.Get(),nullptr,0);
    ctx->OMSetDepthStencilState(depth.Get(),0);ctx->OMSetBlendState(nullptr,nullptr,mask);
    {
        dblui::GPUCost timing(ctx,dblui::GPUStage::ShellTransmission);
        oldDraw(ctx,count,start,base);
    }
    ctx->PSSetShaderResources(13,3,resources);for(auto* resource:resources)if(resource)resource->Release();
    sampler=oldSampler.Get();ctx->PSSetSamplers(15,1,&sampler);
    ctx->VSSetShader(oldVS.Get(),nullptr,0);ctx->PSSetShader(oldPS.Get(),nullptr,0);ctx->IASetInputLayout(oldLayout.Get());
    ctx->OMSetDepthStencilState(oldDepth.Get(),stencil);ctx->OMSetBlendState(oldBlend.Get(),factors,mask);
    ctx->OMSetRenderTargets(8,targets,z.Get());for(auto* target:targets)if(target)target->Release();
    return true;
}
