Texture2D<float4> Input : register(t0);
Texture2D<float> Depth : register(t1);
RWTexture2D<float4> Output : register(u0);
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){
 uint w,h;Output.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;
 uint iw,ih;Input.GetDimensions(iw,ih);
 float3 sum=0;float nearest=1;float weight=0;
 [unroll]for(int y=0;y<2;y++)[unroll]for(int x=0;x<2;x++){
  int2 p=min(int2(id.xy)*2+int2(x,y),int2(iw-1,ih-1));
  float4 sample=Input.Load(int3(p,0));sum+=sample.rgb;
#ifdef DBL_REDUCE
  nearest=min(nearest,sample.a);
#else
  nearest=min(nearest,Depth.Load(int3(p,0)));
#endif
  weight++;
 }
 Output[id.xy]=float4(sum/weight,nearest);
}
