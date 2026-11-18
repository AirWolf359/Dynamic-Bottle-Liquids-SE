Texture2D<float4> Background:register(t0);
Texture2D<float4> Light:register(t1);
Texture2D<float4> Transmission:register(t2);
Texture2D<float4> Distortion:register(t3);
Texture2D<float> SceneDepth:register(t4);
SamplerState LinearClamp:register(s0);
struct Vertex {float4 position:SV_POSITION;};
Vertex VS(uint id:SV_VertexID){Vertex v;v.position=float4(id==2?3:-1,id==1?3:-1,0,1);return v;}
Texture2D<float4> DownsampledScene : register(t12);
static float UseDownsampledScene=0;
float3 sampleRefractionScene(float2 pixel,float2 size,float depth){
    if(UseDownsampledScene>.5){
        uint w,h,levels;DownsampledScene.GetDimensions(0,w,h,levels);
        uint mip=min((uint)max(0,round(UseDownsampledScene)-1),max(1,levels)-1);
        DownsampledScene.GetDimensions(mip,w,h,levels);
        if(w>0&&h>0){
            float2 uv=pixel/size;int2 corner=int2(floor(uv*float2(w,h)-.5));bool safe=true;
            [unroll]for(int y=0;y<2;y++)[unroll]for(int x=0;x<2;x++)
                safe=safe&&(DownsampledScene.Load(int3(clamp(corner+int2(x,y),int2(0,0),int2(w-1,h-1)),mip)).a>=depth-1e-6);
            if(safe)return DownsampledScene.SampleLevel(LinearClamp,uv,mip).rgb;
        }
    }
    return Background.SampleLevel(LinearClamp,pixel/size,0).rgb;
}

float4 PS(Vertex v):SV_TARGET {
    uint width,height;Background.GetDimensions(width,height);
    int2 pixel=int2(v.position.xy);float2 size=float2(width,height);
    float4 original=Background.Load(int3(pixel,0));
    float4 d=Distortion.Load(int3(pixel,0));
    if(d.w==0||SceneDepth.Load(int3(pixel,0))<d.z-1e-6)return original;
    float2 shifted=clamp(d.xy,.5,size-.5);bool valid=true;
    // Reject foreground in the sampled footprint, not unrelated pixels
    // between the original and refracted screen positions.
    {
        int2 corner=int2(floor(shifted-.5));
        [unroll]for(int y=0;y<2;y++)[unroll]for(int x=0;x<2;x++){
            int2 tap=clamp(corner+int2(x,y),int2(0,0),int2(width-1,height-1));
            if(SceneDepth.Load(int3(tap,0))<d.z-1e-6)valid=false;
        }
    }
    // Test110: diagnostic states travel in the existing distortion attachment.
    if(d.w>=100){
        int state=(int)round(d.w-100);
        if(!valid&&(state==7||state==8))return float4(0,1,1,original.a);
        return float4(Light.Load(int3(pixel,0)).rgb,original.a);
    }
    UseDownsampledScene=max(0,d.w-1);
    float3 scene=valid?sampleRefractionScene(shifted,size,d.z):original.rgb;
    return float4(Light.Load(int3(pixel,0)).rgb+Transmission.Load(int3(pixel,0)).rgb*scene,original.a);
}
