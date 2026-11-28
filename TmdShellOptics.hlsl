Texture2D<float4> scene : register(t15);
Texture2D<float> sceneDepth : register(t14);
Texture2D<float4> levels : register(t13);
SamplerState linearClamp : register(s15);
#include "RefractionCamera.hlsli"

cbuffer Shell : register(b10) {
    float2 strength; float2 size;
    float4 viewport;
    float projectionDepth;
};
struct Pixel { float4 fog:COLOR1; float4 position : SV_POSITION; float3 local : TEXCOORD0; nointerpolation float3 localEye : TEXCOORD1; float3 world : TEXCOORD2; nointerpolation float3x3 basis : TEXCOORD3; float3 normal : TEXCOORD6; float2 uv:TEXCOORD7; float3 tangent:TEXCOORD8; float3 bitangent:TEXCOORD9; nointerpolation float3x3 viewBasis:TEXCOORD10; nointerpolation float4 clipOrigin:TEXCOORD13; nointerpolation float4 clipX:TEXCOORD14; nointerpolation float4 clipY:TEXCOORD15; nointerpolation float4 clipZ:TEXCOORD16;
#ifdef DBL_VR
    float eyeClip:SV_ClipDistance0; nointerpolation uint eye:TEXCOORD17;
#endif
};
// Skyrim VR: the eye's half of the double-wide stereo target, so the bent
// lookup never samples the other eye. The refraction camera stays off in VR.
#ifdef DBL_VR
#define DBL_EYE_VIEWPORT(p) float4(viewport.x+viewport.z*.5*(p).eye,viewport.y,viewport.z*.5,viewport.w)
#else
#define DBL_EYE_VIEWPORT(p) viewport
#endif
float4 main(Pixel p):SV_Target {
    float depthSign=size.x<0?-1:1;
    if(projectionDepth)depthSign=ProjectionDepthSign(depthSign);
    float2 extent=abs(size);
    float3 n=normalize(p.normal);
    float facing=dot(n,p.localEye-p.local)>=0?1:-1;
    float3 smoothView=normalize(mul(p.viewBasis,n))*facing;
    float2 slope=smoothView.xy*strength.x;
    const float4 area=DBL_EYE_VIEWPORT(p);
    float2 pixel=clamp(p.position.xy+4*slope*float2(1,-1),area.xy+.5,area.xy+area.zw-.5);
    int2 corner=int2(floor(pixel-.5));
    [unroll]for(int y=0;y<2;y++)[unroll]for(int x=0;x<2;x++)
        if(depthSign*sceneDepth.Load(int3(clamp(corner+int2(x,y),0,int2(extent)-1),0))<depthSign*p.position.z-.00001)return scene.Load(int3(int2(p.position.xy),0));
    if(strength.y>=1){
        uint mip=uint(strength.y)-1,w,h,count;levels.GetDimensions(mip,w,h,count);
        int2 start=int2(floor(pixel/extent*float2(w,h)-.5));bool safe=true;
        [unroll]for(int j=0;j<2;j++)[unroll]for(int i=0;i<2;i++)
            safe=safe && levels.Load(int3(clamp(start+int2(i,j),0,int2(w,h)-1),mip)).a>=depthSign*p.position.z-.00001;
        if(safe)return float4(levels.SampleLevel(linearClamp,pixel/extent,mip).rgb,scene.Load(int3(int2(p.position.xy),0)).a);
    }
    return scene.SampleLevel(linearClamp,pixel/extent,0);
}
