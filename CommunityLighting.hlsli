// Community Shaders 1.9.1 ABI, verified against the installed shaders and capture.
// Only lighting is adapted; bottle material, coverage and optics remain DBL's.
struct CSLight {
    float3 color; float fade;
    float radius, invRadius, fadeZone, sizeBias;
    float4 positionWS; uint4 roomFlags;
    uint flags, shadowIndex, pad0, pad1;
};
cbuffer CSStrict : register(b3) {
    uint CSStrictCount; int CSRoom; uint CSShadowMask; uint CSPad;
    CSLight CSStrictLights[15];
};
cbuffer CSShared : register(b5) {
    float4 CSCamera : packoffset(c33);
    float4 CSBufferDim : packoffset(c34);
    uint CSInterior : packoffset(c35.w);
};
cbuffer CSFeatures : register(b6) {
    uint4 CSClusterSize : packoffset(c10);
    uint CSLinear : packoffset(c45.x);
    uint CSDirLinear : packoffset(c45.y);
    float CSDirMult : packoffset(c45.z);
    float CSLightGamma : packoffset(c45.w);
    float CSFogGamma : packoffset(c47.x);
    float CSFogAlphaGamma : packoffset(c47.y);
    float CSAmbientGamma : packoffset(c46.w);
    float CSDirectionalMult : packoffset(c49.x);
    float CSPointMult : packoffset(c49.y);
    float CSAmbientMult : packoffset(c49.z);
};
StructuredBuffer<CSLight> CSLights : register(t35);
StructuredBuffer<uint> CSList : register(t36);
StructuredBuffer<uint4> CSGrid : register(t37);
bool csLighting(){return ((uint)Eye.w&4)!=0;}
float3 csAmbient(float3 v){return CSLinear?pow(abs(v),CSAmbientGamma)*CSAmbientMult:v;}
float3 csDirectional(float3 v){
    float m=CSLinear&&!CSDirLinear&&!CSInterior?CSDirMult:1;
    return CSLinear&&!CSDirLinear?pow(abs(v/max(m,1e-5)),CSLightGamma)*(3.14159265*CSDirectionalMult*m):v;
}
uint2 csCluster(float4 pixel){
    uint count,stride;CSGrid.GetDimensions(count,stride);
    float z=max(CSCamera.w/max(CSCamera.x-pixel.z*CSCamera.z,1e-6),CSCamera.y);
    float2 uv=(pixel.xy-Viewport.xy)/max(Viewport.zw,1);
    uint3 cell=uint3(uv*CSClusterSize.xy,log(z/max(CSCamera.y,1e-6))*CSClusterSize.z/log(max(CSCamera.x/CSCamera.y,1.0001)));
    if(any(cell>=CSClusterSize.xyz))return 0;
    uint index=cell.x+CSClusterSize.x*(cell.y+CSClusterSize.y*cell.z);
    if(index>=count)return 0;
    uint2 result=CSGrid[index].xy;
    uint listSize;CSList.GetDimensions(listSize,stride);
    if(result.x>=listSize)return 0;
    result.y=min(min(result.y,256),listSize-result.x);return result;
}
bool csLightAt(uint index,uint offset,out CSLight light){
    light=(CSLight)0;
    uint strictCount=min(CSStrictCount,15);
    if(index<strictCount){light=CSStrictLights[index];return (light.flags&512)==0;}
    uint count,stride;CSLights.GetDimensions(count,stride);
    uint id=CSList[offset+index-strictCount];if(id>=count)return false;
    light=CSLights[id];
    if(light.flags&512)return false;
    if(light.flags&2)return light.shadowIndex<32&&(CSShadowMask&(1u<<light.shadowIndex))!=0;
    if((light.flags&1)&&CSRoom>=0){
        if(CSRoom>=128)return false;
        return (light.roomFlags[CSRoom/32]&(1u<<(CSRoom%32)))!=0;
    }
    return true;
}
float csAttenuation(float distance,CSLight light){
    if(light.flags&512)return 0;
    if(light.flags&1024){
        float t=saturate((light.radius-distance)*light.fadeZone);
        return (3920/max(distance*distance+light.sizeBias,1e-6))*t*t*(3-2*t);
    }
    float t=saturate(distance*light.invRadius);return 1-t*t;
}
float3 csLightColor(CSLight light){
    return CSLinear&&!(light.flags&2048)?pow(abs(light.color),CSLightGamma)*(3.14159265*CSPointMult)*light.fade:light.color*light.fade;
}
