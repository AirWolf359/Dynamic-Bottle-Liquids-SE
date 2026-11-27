// Engine state changed by the producer/compositor is restored on every exit.
struct OpticalState {
    ID3D11DeviceContext* c;
    ID3D11RenderTargetView* rt[8]{};ComPtr<ID3D11DepthStencilView> ds;
    ID3D11ShaderResourceView* resources[16]{};ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;ComPtr<ID3D11GeometryShader> geometry;
    ComPtr<ID3D11InputLayout> input;D3D11_PRIMITIVE_TOPOLOGY topology;
    ComPtr<ID3D11BlendState> blendState;ComPtr<ID3D11DepthStencilState> depthState;ComPtr<ID3D11RasterizerState> rasterState;
    FLOAT factors[4]{};UINT mask{},stencil{},viewportCount=16;D3D11_VIEWPORT viewports[16]{};
    explicit OpticalState(ID3D11DeviceContext* ctx):c(ctx){
        c->OMGetRenderTargets(8,rt,&ds);c->PSGetShaderResources(0,16,resources);c->PSGetSamplers(0,1,&sampler);
        c->VSGetShader(&vertex,nullptr,nullptr);c->PSGetShader(&pixel,nullptr,nullptr);c->GSGetShader(&geometry,nullptr,nullptr);
        c->IAGetInputLayout(&input);c->IAGetPrimitiveTopology(&topology);c->RSGetViewports(&viewportCount,viewports);
        c->OMGetBlendState(&blendState,factors,&mask);c->OMGetDepthStencilState(&depthState,&stencil);c->RSGetState(&rasterState);
    }
    ~OpticalState(){
        ID3D11ShaderResourceView* none[16]{};c->PSSetShaderResources(0,16,none);c->OMSetRenderTargets(8,rt,ds.Get());
        c->PSSetShaderResources(0,16,resources);auto* s=sampler.Get();c->PSSetSamplers(0,1,&s);
        c->VSSetShader(vertex.Get(),nullptr,0);c->PSSetShader(pixel.Get(),nullptr,0);c->GSSetShader(geometry.Get(),nullptr,0);
        c->IASetInputLayout(input.Get());c->IASetPrimitiveTopology(topology);c->RSSetViewports(viewportCount,viewports);
        c->OMSetBlendState(blendState.Get(),factors,mask);c->OMSetDepthStencilState(depthState.Get(),stencil);c->RSSetState(rasterState.Get());
        for(auto* r:rt)if(r)r->Release();for(auto* r:resources)if(r)r->Release();
    }
};
// Producer changes only OM targets/blend/depth and PS. Preserve all PS SRVs
// because rebinding targets can implicitly unbind overlapping input resources.
struct OpticalProducerState {
    ID3D11DeviceContext* c;
    ID3D11RenderTargetView* rt[8]{};ComPtr<ID3D11DepthStencilView> ds;
    ID3D11ShaderResourceView* resources[16]{};
    ComPtr<ID3D11PixelShader> pixel;ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;FLOAT factors[4]{};UINT mask{},stencil{};
    explicit OpticalProducerState(ID3D11DeviceContext* ctx):c(ctx){
        c->OMGetRenderTargets(8,rt,&ds);c->PSGetShaderResources(0,16,resources);
        c->PSGetShader(&pixel,nullptr,nullptr);c->OMGetBlendState(&blend,factors,&mask);
        c->OMGetDepthStencilState(&depth,&stencil);
    }
    ~OpticalProducerState(){
        c->OMSetRenderTargets(8,rt,ds.Get());c->PSSetShaderResources(0,16,resources);
        c->PSSetShader(pixel.Get(),nullptr,0);c->OMSetBlendState(blend.Get(),factors,mask);
        c->OMSetDepthStencilState(depth.Get(),stencil);
        for(auto* r:rt)if(r)r->Release();for(auto* r:resources)if(r)r->Release();
    }
};
#include "ForwardDraw.inc"
struct OpticalBuffers {
    struct Target {ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11RenderTargetView> rt;ComPtr<ID3D11ShaderResourceView> srv;};
    Target light,transmission,distortion,output;SharedBlur blur;
    ComPtr<ID3D11Texture2D> privateDepth,sourceColor,sourceDepth;
    ComPtr<ID3D11DepthStencilView> nearestDS;
    ComPtr<ID3D11ShaderResourceView> background,finalDepth;
    ComPtr<ID3D11PixelShader> producer,composePS;ComPtr<ID3D11VertexShader> composeVS;
    ComPtr<ID3D11BlendState> opaque;ComPtr<ID3D11DepthStencilState> nearest,reverseNearest,noDepth;
    ComPtr<ID3D11RasterizerState> screenRaster;ComPtr<ID3D11SamplerState> linear;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11RenderTargetView> preparedRT;ComPtr<ID3D11DepthStencilView> preparedDS;
    UINT width{},height{};DXGI_FORMAT format{};uint64_t prepared=~0ull,composed=~0ull;unsigned draws{};bool failed=false,boundaryMissing=false;
    bool preparedReverse=false;
    D3D11_VIEWPORT producerViewport{};
    unsigned hdrAttempts{},hdrVerified{},hdrBoundMismatch{};
    void initialize(ID3D11Device* device,const std::filesystem::path& shaderPath){
        forwardDraws.packing.initialize(device);
        auto compile=[&](const std::filesystem::path& path,const char* entry,const char* target,const D3D_SHADER_MACRO* macros){
            const char* name=macros?(macros[1].Name?"BottleForward.cso":"BottleOptical.cso"):(target[0]=='v'?"ComposeVertex.cso":"ComposePixel.cso");
            ComPtr<ID3DBlob> code;auto file=path.parent_path()/name;
            if(FAILED(D3DReadFileToBlob(file.c_str(),&code)))throw std::runtime_error("Missing precompiled optical shader: "+file.string());return code;
        };
        D3D_SHADER_MACRO macros[]={{"DBL_OPTICAL","1"},{nullptr,nullptr}};
        auto code=compile(shaderPath,"PSMain","ps_5_0",macros);check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&producer));
        D3D_SHADER_MACRO forwardMacros[]={{"DBL_OPTICAL","1"},{"DBL_FORWARD","1"},{nullptr,nullptr}};
        code=compile(shaderPath,"PSMain","ps_5_0",forwardMacros);check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&forwardDraws.shader));
        auto path=shaderPath.parent_path()/"OpticalCompose.hlsl";
        code=compile(path,"VS","vs_5_0",nullptr);check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&composeVS));
        code=compile(path,"PS","ps_5_0",nullptr);check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&composePS));
        D3D11_BLEND_DESC bd{};for(auto& t:bd.RenderTarget)t.RenderTargetWriteMask=15;check(device->CreateBlendState(&bd,&opaque));
        D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=true;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;check(device->CreateDepthStencilState(&dd,&nearest));
        dd.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;check(device->CreateDepthStencilState(&dd,&reverseNearest));
        dd.DepthEnable=false;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;check(device->CreateDepthStencilState(&dd,&noDepth));
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=true;check(device->CreateRasterizerState(&rd,&screenRaster));
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;check(device->CreateSamplerState(&sd,&linear));
    }
    bool prepare(ID3D11DeviceContext* ctx,ID3D11RenderTargetView* verifiedMain,uint64_t currentFrame,V4& info,V4& viewport,
                 bool reverseTarget=false,bool replayNow=false){
        // The caller verifies MAIN by resource identity. Bindings still have to
        // match that verified view when this draw enters the queue.
        if(!verifiedMain||failed||boundaryMissing!=replayNow||!producer||composed==currentFrame||
           ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
        if(replayNow&&(!reverseTarget||!forwardDraws.optimized))return false;
        if((reverseTarget&&!forwardDraws.enabled)||(prepared==currentFrame&&preparedReverse!=reverseTarget))return false;
        ComPtr<ID3D11RenderTargetView> rt;ComPtr<ID3D11DepthStencilView> ds;
        ctx->OMGetRenderTargets(1,&rt,&ds);if(rt.Get()!=verifiedMain||!ds)return false;
        // Views are immutable. Retain references so pointer identity cannot be reused.
        // Still validate context, frame, target identity and current viewport.
        if(prepared==currentFrame&&context.Get()==ctx&&preparedRT==rt&&preparedDS==ds){
            UINT count=1;D3D11_VIEWPORT vp{};ctx->RSGetViewports(&count,&vp);
            if(count!=1||vp.TopLeftX!=0||vp.TopLeftY!=0||vp.Width!=width||vp.Height!=height||vp.MinDepth<0||vp.MaxDepth>1||vp.MaxDepth<=vp.MinDepth)return false;
            info={reverseTarget?2.f:1.f,float(width),float(height),4};viewport={0,0,float(width),float(height)};return true;
        }
        D3D11_RENDER_TARGET_VIEW_DESC rv{};rt->GetDesc(&rv);
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};ds->GetDesc(&dv);
        const bool hdrColor=rv.Format==DXGI_FORMAT_R11G11B10_FLOAT||
            (reverseTarget&&rv.Format==DXGI_FORMAT_R16G16B16A16_FLOAT);
        if(!hdrColor||rv.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D||rv.Texture2D.MipSlice||dv.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D||dv.Texture2D.MipSlice)return false;
        DXGI_FORMAT depthFormat=DXGI_FORMAT_UNKNOWN;
        if(reverseTarget){
            if(dv.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT)depthFormat=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        }else{
            if(dv.Format==DXGI_FORMAT_D24_UNORM_S8_UINT)depthFormat=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            if(dv.Format==DXGI_FORMAT_D32_FLOAT)depthFormat=DXGI_FORMAT_R32_FLOAT;
        }
        if(depthFormat==DXGI_FORMAT_UNKNOWN)return false;
        ComPtr<ID3D11Resource> cr,dr;rt->GetResource(&cr);ds->GetResource(&dr);
        ComPtr<ID3D11Texture2D> ct,dt;if(FAILED(cr.As(&ct))||FAILED(dr.As(&dt)))return false;
        D3D11_TEXTURE2D_DESC cd{},dd{};ct->GetDesc(&cd);dt->GetDesc(&dd);
        if(cd.SampleDesc.Count!=1||dd.SampleDesc.Count!=1||cd.ArraySize!=1||dd.ArraySize!=1||cd.MipLevels!=1||dd.MipLevels!=1||cd.Width!=dd.Width||cd.Height!=dd.Height)return false;
        UINT count=1;D3D11_VIEWPORT vp{};ctx->RSGetViewports(&count,&vp);
        if(count!=1||vp.TopLeftX!=0||vp.TopLeftY!=0||vp.Width!=cd.Width||vp.Height!=cd.Height||vp.MinDepth<0||vp.MaxDepth>1||vp.MaxDepth<=vp.MinDepth)return false;
        if(prepared==currentFrame&&(sourceColor!=ct||sourceDepth!=dt))return false;
        ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
        if(!forwardDraws.enabled&&(sourceColor!=ct||!background)){background.Reset();check(device->CreateShaderResourceView(ct.Get(),nullptr,&background));}
        if(!forwardDraws.enabled&&(sourceDepth!=dt||!finalDepth)){finalDepth.Reset();D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=depthFormat;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;check(device->CreateShaderResourceView(dt.Get(),&sd,&finalDepth));}
        if(width!=cd.Width||height!=cd.Height||format!=cd.Format||!nearestDS||(!forwardDraws.enabled&&(!output.rt||!light.rt||!transmission.rt||!distortion.rt))){
            width=cd.Width;height=cd.Height;format=cd.Format;
            auto make=[&](Target& t,DXGI_FORMAT f){t={};D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=1;d.Format=f;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;check(device->CreateTexture2D(&d,nullptr,&t.texture));check(device->CreateRenderTargetView(t.texture.Get(),nullptr,&t.rt));check(device->CreateShaderResourceView(t.texture.Get(),nullptr,&t.srv));};
            if(!forwardDraws.enabled){make(light,DXGI_FORMAT_R16G16B16A16_FLOAT);make(transmission,DXGI_FORMAT_R16G16B16A16_FLOAT);make(distortion,DXGI_FORMAT_R32G32B32A32_FLOAT);make(output,format);}
            else {light={};transmission={};distortion={};output={};background.Reset();finalDepth.Reset();}
            privateDepth.Reset();nearestDS.Reset();D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_D32_FLOAT;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_DEPTH_STENCIL;check(device->CreateTexture2D(&d,nullptr,&privateDepth));check(device->CreateDepthStencilView(privateDepth.Get(),nullptr,&nearestDS));
        }
        sourceColor=ct;sourceDepth=dt;context=ctx;preparedRT=rt;preparedDS=ds;
        if(prepared!=currentFrame){{dblui::GPUCost timing(ctx,dblui::GPUStage::Prepare);float zero[4]{};if(!forwardDraws.enabled){ctx->ClearRenderTargetView(light.rt.Get(),zero);ctx->ClearRenderTargetView(transmission.rt.Get(),zero);ctx->ClearRenderTargetView(distortion.rt.Get(),zero);}ctx->ClearDepthStencilView(nearestDS.Get(),D3D11_CLEAR_DEPTH,reverseTarget?0.f:1.f,0);}prepared=currentFrame;preparedReverse=reverseTarget;hdrAttempts=hdrVerified=hdrBoundMismatch=0;draws=0;forwardDraws.clear();producerViewport=vp;}
        info={reverseTarget?2.f:1.f,float(width),float(height),4};viewport={0,0,float(width),float(height)};return true;
    }
    void produce(ID3D11DeviceContext* ctx,DrawFn draw,UINT count,UINT start,INT base,const GPU* bottleData=nullptr,const char* name=nullptr,uintptr_t instance=0){
        if(forwardDraws.enabled){forwardDraws.capture(ctx,count,start,base,bottleData,nullptr,name,instance);++draws;return;}
        OpticalProducerState saved(ctx);
        ID3D11RenderTargetView* targets[]={light.rt.Get(),transmission.rt.Get(),distortion.rt.Get()};
        ctx->OMSetRenderTargets(3,targets,nearestDS.Get());ctx->PSSetShader(producer.Get(),nullptr,0);
        ctx->OMSetBlendState(opaque.Get(),nullptr,~0u);ctx->OMSetDepthStencilState(nearest.Get(),0);
        {dblui::GPUCost timing(ctx,dblui::GPUStage::Draw);dblui::LabelDraw(timing.token,name,instance);draw(ctx,count,start,base);}++draws;
    }
    void replayPreparedForward(){
        // Immediate replays can see different MAIN pixels in the same frame.
        if(boundaryMissing)forwardDraws.snapshot.blur.invalidate();
        auto* depthState=preparedReverse?reverseNearest.Get():nearest.Get();
        forwardDraws.draw(context.Get(),oldDraw,preparedRT.Get(),preparedDS.Get(),nearestDS.Get(),opaque.Get(),
                         depthState,producerViewport,preparedReverse);
        forwardDraws.clear();
        draws=0;
    }
    bool compose(uint64_t currentFrame,ID3D11Resource* verifiedHDRScene=nullptr){
        if(prepared!=currentFrame||!draws||composed==currentFrame||!context)return false;
        auto* ctx=context.Get();ComPtr<ID3D11RenderTargetView> bound;ctx->OMGetRenderTargets(1,&bound,nullptr);
        ComPtr<ID3D11Resource> resource;if(bound)bound->GetResource(&resource);
        ++hdrAttempts;
        if(verifiedHDRScene)++hdrVerified;
        if(resource.Get()!=sourceColor.Get())++hdrBoundMismatch;
        // HDR's declared scene is authoritative even when OM still contains a
        // previous pass's output. Replay explicitly targets preparedRT and restores
        // OM state. An explicitly different scene must never authorize replay.
        if(verifiedHDRScene ? verifiedHDRScene!=sourceColor.Get() : resource.Get()!=sourceColor.Get())return false;
        if(forwardDraws.enabled){
            replayPreparedForward();
            composed=currentFrame;return true;
        }
        OpticalState saved(ctx);blur.build(ctx,background.Get(),finalDepth.Get(),sourceColor.Get(),width,height);ID3D11ShaderResourceView* none[16]{};ctx->PSSetShaderResources(0,16,none);
        auto* target=output.rt.Get();ctx->OMSetRenderTargets(1,&target,nullptr);
        ctx->VSSetShader(composeVS.Get(),nullptr,0);ctx->PSSetShader(composePS.Get(),nullptr,0);ctx->GSSetShader(nullptr,nullptr,0);
        ctx->IASetInputLayout(nullptr);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->OMSetBlendState(opaque.Get(),nullptr,~0u);ctx->OMSetDepthStencilState(noDepth.Get(),0);ctx->RSSetState(screenRaster.Get());
        D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};ctx->RSSetViewports(1,&vp);
        ID3D11ShaderResourceView* resources[]={background.Get(),light.srv.Get(),transmission.srv.Get(),distortion.srv.Get(),finalDepth.Get()};ctx->PSSetShaderResources(0,5,resources);auto* blurred=blur.view();ctx->PSSetShaderResources(12,1,&blurred);
        auto* sampler=linear.Get();ctx->PSSetSamplers(0,1,&sampler);{dblui::GPUCost timing(ctx,dblui::GPUStage::Compose);ctx->Draw(3,0);}
        ctx->PSSetShaderResources(0,16,none);ctx->OMSetRenderTargets(0,nullptr,nullptr);
        {dblui::GPUCost timing(ctx,dblui::GPUStage::Compose);ctx->CopyResource(sourceColor.Get(),output.texture.Get());}composed=currentFrame;return true;
    }
    bool finishFrame(uint64_t currentFrame){
        if(prepared!=currentFrame||!draws||composed==currentFrame)return false;
        // No matching boundary means queued draws were never displayed. Do not
        // keep accepting and discarding every subsequent frame (Test96).
        // Use immediate rendering until a verified MAIN HDR boundary returns.
        boundaryMissing=true;reset();return true;
    }
    bool observeHDRBoundary(uint64_t currentFrame,bool verifiedMain){
        if(failed||!boundaryMissing||!verifiedMain)return false;
        boundaryMissing=false;composed=currentFrame;return true;
    }
    void reset(){prepared=composed=~0ull;draws=0;forwardDraws.clear();}
} opticalBuffers;
