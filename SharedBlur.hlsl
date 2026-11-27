Texture2D<float4> Input : register(t0);
Texture2D<float> Depth : register(t1);
RWTexture2D<float4> Output : register(u0);
#ifdef DBL_CAMERA_DEPTH
#include "RefractionCamera.hlsli"
#endif
#ifdef DBL_REDUCE
void accumulateMip(uint2 p,inout float3 sum,inout float nearest){
 float4 sample=Input.Load(int3(p,0));
 sum+=sample.rgb;
 nearest=min(nearest,sample.a);
}
#endif
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){
 uint w,h;Output.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;
 uint iw,ih;Input.GetDimensions(iw,ih);
 float3 sum=0;float nearest=1;float weight=0;
#ifndef DBL_REDUCE
 float depthSign=1;
#ifdef DBL_REVERSE_DEPTH
 depthSign=-1;
#endif
#ifdef DBL_CAMERA_DEPTH
 depthSign=ProjectionDepthSign(depthSign);
#endif
#endif
 [unroll]for(int y=0;y<2;y++)[unroll]for(int x=0;x<2;x++){
  int2 p=min(int2(id.xy)*2+int2(x,y),int2(iw-1,ih-1));
  float4 sample=Input.Load(int3(p,0));sum+=sample.rgb;
#ifdef DBL_REDUCE
  nearest=min(nearest,sample.a);
#else
  float sceneDepth=Depth.Load(int3(p,0))*depthSign;
  nearest=min(nearest,sceneDepth);
#endif
  weight++;
 }
#ifdef DBL_REDUCE
 // Native mip sizes round down. Include the unmatched source texels in the
 // last destination texel without shifting the rest of the filter.
 uint2 start=id.xy*2;
 bool edgeX=iw==2*w+1 && id.x+1==w;
 bool edgeY=ih==2*h+1 && id.y+1==h;
 [branch] if(edgeX){
  accumulateMip(start+uint2(2,0),sum,nearest);
  accumulateMip(start+uint2(2,1),sum,nearest);
  weight+=2;
 }
 [branch] if(edgeY){
  accumulateMip(start+uint2(0,2),sum,nearest);
  accumulateMip(start+uint2(1,2),sum,nearest);
  weight+=2;
  if(edgeX){
   accumulateMip(start+uint2(2,2),sum,nearest);
   weight++;
  }
 }
#endif
 Output[id.xy]=float4(sum/weight,nearest);
}
