// Four shared scene levels: half, quarter, eighth and sixteenth resolution.
// Legacy SharedBlur names retained only to avoid changing resource bindings.
// Alpha carries conservative nearest depth through the complete filter footprint.
uint64_t downsampleRequestedFrame=~0ull;
bool sharedBlurEnabled=true; // Startup-only A/B switch; no per-frame file access.
struct SharedBlur {
 struct Target {ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;};
 Target targets[4];ComPtr<ID3D11ShaderResourceView> chainView;ComPtr<ID3D11ComputeShader> shaders[2];UINT width{},height{};
 bool valid=false,failed=false;
 uint64_t builtFrame=~0ull,builds=0;ComPtr<ID3D11Resource> source;
 void buildUnsafe(ID3D11DeviceContext* c,ID3D11ShaderResourceView* color,ID3D11ShaderResourceView* depth,ID3D11Resource* identity,UINT w,UINT h){
  if(builtFrame==frame&&source.Get()==identity&&width==std::max(8u,(w+1)/2)&&height==std::max(8u,(h+1)/2))return;
  ComPtr<ID3D11Device> dev;c->GetDevice(&dev);
  if(!shaders[0]){const wchar_t* names[]={L"SharedBlurDown.cso",L"SharedBlurReduce.cso"};
   for(int i=0;i<2;i++){ComPtr<ID3DBlob> b;check(D3DReadFileToBlob((root/names[i]).c_str(),&b));check(dev->CreateComputeShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&shaders[i]));}}
  UINT qw=std::max(8u,(w+1)/2),qh=std::max(8u,(h+1)/2);
  if(width!=qw||height!=qh||!targets[0].srv){
   for(auto& t:targets)t={};chainView.Reset();
   D3D11_TEXTURE2D_DESC d{};d.Width=qw;d.Height=qh;d.MipLevels=4;d.ArraySize=1;d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
   check(dev->CreateTexture2D(&d,nullptr,&targets[0].texture));check(dev->CreateShaderResourceView(targets[0].texture.Get(),nullptr,&chainView));
   for(UINT i=0;i<4;i++){
    targets[i].texture=targets[0].texture;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=d.Format;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MostDetailedMip=i;sd.Texture2D.MipLevels=1;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=d.Format;ud.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;ud.Texture2D.MipSlice=i;
    check(dev->CreateShaderResourceView(targets[0].texture.Get(),&sd,&targets[i].srv));check(dev->CreateUnorderedAccessView(targets[0].texture.Get(),&ud,&targets[i].uav));
   }width=qw;height=qh;
  }
  ComPtr<ID3D11ComputeShader> shader;ID3D11ClassInstance* classes[256]{};UINT count=256;c->CSGetShader(&shader,classes,&count);
  ID3D11ShaderResourceView* oldSRV[2]{};c->CSGetShaderResources(0,2,oldSRV);ComPtr<ID3D11UnorderedAccessView> oldUAV;c->CSGetUnorderedAccessViews(0,1,&oldUAV);
  ID3D11RenderTargetView* rt[8]{};ComPtr<ID3D11DepthStencilView> ds;c->OMGetRenderTargets(8,rt,&ds);c->OMSetRenderTargets(0,nullptr,nullptr);
  // A previous PS read must not overlap the output being regenerated.
  ComPtr<ID3D11ShaderResourceView> oldBlur;c->PSGetShaderResources(12,1,&oldBlur);ID3D11ShaderResourceView* nil=nullptr;c->PSSetShaderResources(12,1,&nil);
  for(int i=0;i<4;i++){
   ID3D11ShaderResourceView* inputs[]={i?targets[i-1].srv.Get():color,i?nullptr:depth};auto* output=targets[i].uav.Get();
   c->CSSetShaderResources(0,2,inputs);c->CSSetUnorderedAccessViews(0,1,&output,nullptr);c->CSSetShader(shaders[i?1:0].Get(),nullptr,0);{dblui::GPUCost timing(c,dblui::GPUStage::Prepare);c->Dispatch(((width>>i)+7)/8,((height>>i)+7)/8,1);}
   ID3D11UnorderedAccessView* noUAV=nullptr;ID3D11ShaderResourceView* none[2]{};c->CSSetUnorderedAccessViews(0,1,&noUAV,nullptr);c->CSSetShaderResources(0,2,none);
  }
  c->OMSetRenderTargets(8,rt,ds.Get());auto* previous=oldBlur.Get();c->PSSetShaderResources(12,1,&previous);
  auto* u=oldUAV.Get();UINT keep=~0u;c->CSSetUnorderedAccessViews(0,1,&u,&keep);c->CSSetShaderResources(0,2,oldSRV);c->CSSetShader(shader.Get(),classes,count);
  for(auto* p:oldSRV)if(p)p->Release();for(auto* p:rt)if(p)p->Release();for(UINT i=0;i<count;i++)classes[i]->Release();
  source=identity;builtFrame=frame;++builds;
 }
 void build(ID3D11DeviceContext* c,ID3D11ShaderResourceView* color,ID3D11ShaderResourceView* depth,ID3D11Resource* identity,UINT w,UINT h){
  if(!sharedBlurEnabled||downsampleRequestedFrame!=frame){valid=false;return;}
  if(failed)return;
  try{buildUnsafe(c,color,depth,identity,w,h);valid=true;}
  catch(const std::exception& e){failed=true;valid=false;SKSE::log::warn("[DBL-downsample-fallback] Scene downsample disabled; sharp rendering retained: {}",e.what());}
 }
 ID3D11ShaderResourceView* view()const{return valid?chainView.Get():nullptr;}
};
