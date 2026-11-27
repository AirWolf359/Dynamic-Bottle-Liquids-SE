// Skyrim lighting constants; simulation uses unscaled mesh coordinates.
// User's conditional choice: interaction stays off unless cost-neutrality is
// established. Diagnostic builds can explicitly enable it for paired tests.
#if !defined(DBL_ENABLE_TUNED_INTERACTION) && !defined(DBL_DISABLE_TUNED_INTERACTION)
#define DBL_DISABLE_TUNED_INTERACTION 1
#endif
#ifdef DBL_VERTEX
#ifdef DBL_VR
// Skyrim VR instanced stereo: per-eye world and camera matrices, one instance per eye.
cbuffer NativeFog:register(b0){float4 FogParam:packoffset(c2);float4 FogNearColor:packoffset(c3);float4 FogFarColor:packoffset(c4);};
cbuffer NativeGeometry : register(b2) { row_major float3x4 WorldEyes[2] : packoffset(c0); };
cbuffer NativeFrame : register(b12) { row_major float4x4 ViewEyes[2] : packoffset(c0); row_major float4x4 ViewProjEyes[2] : packoffset(c16); };
cbuffer NativeStereo : register(b13) { float4 StereoInfo : packoffset(c0); float4 EyeClipEdge[2] : packoffset(c1); }; // y: stereo enabled, zw: eye offsets
#else
cbuffer NativeFog:register(b0){float4 FogParam:packoffset(c1);float4 FogNearColor:packoffset(c2);float4 FogFarColor:packoffset(c3);};
cbuffer NativeGeometry : register(b2) { row_major float3x4 World; };
cbuffer NativeFrame : register(b12) { row_major float3x3 ViewRotation : packoffset(c0); row_major float4x4 ViewProj : packoffset(c8); };
#endif
cbuffer NativeUV : register(b1) { float4 TexcoordOffset : packoffset(c2); };
#else
#include "RefractionCamera.hlsli"
cbuffer SceneMaterial : register(b1) { float4 EnvironmentData : packoffset(c2); };
#ifdef DBL_VR
// Skyrim VR lighting layout: point-light positions are per eye, [eye*count+i].
cbuffer SceneLighting : register(b2) {
    float3 LightDirection : packoffset(c0);
    float3 LightColor : packoffset(c13);
    float4 MaterialData : packoffset(c15);
    row_major float3x4 Ambient : packoffset(c23);
    float4 LightPositionsEyes[14] : packoffset(c27);
    float4 LightColors[7] : packoffset(c41);
    float2 LightCounts : packoffset(c48);
};
#define DBL_LIGHT_POSITION(i) LightPositionsEyes[p.eye*(uint)clamp((int)LightCounts.x,0,7)+(i)]
// The native screen shadow mask (textureFlags 16/32) isn't used in VR yet, and
// the VR layout of its channel field hasn't been mapped. Never read engine data for it.
static const float4 NativeShadowChannel=0;
#else
cbuffer SceneLighting : register(b2) {
    float3 LightDirection : packoffset(c0);
    float3 LightColor : packoffset(c1);
    float4 MaterialData : packoffset(c3);
    float4 NativeShadowChannel : packoffset(c2);
    row_major float3x4 Ambient : packoffset(c11);
    float4 LightPositions[7] : packoffset(c15);
    float4 LightColors[7] : packoffset(c22);
    float2 LightCounts : packoffset(c29);
};
#define DBL_LIGHT_POSITION(i) LightPositions[i]
#endif
TextureCube<float4> Environment : register(t4);
SamplerState EnvironmentSampler : register(s4);
Texture2D<float4> NativeLiquidGlow : register(t3);
Texture2D<float4> SurfaceBase : register(t0);
Texture2D<float4> SurfaceNormal : register(t1);
SamplerState BaseSampler : register(s0);
SamplerState NormalSampler : register(s1);
// Test53: reflection-only controls. Evolution weights are supplied per bottle.
#ifndef DBL_RIPPLE_STRENGTH
#define DBL_RIPPLE_STRENGTH 1.0
#endif
#ifndef DBL_RIPPLE_SIZE
#define DBL_RIPPLE_SIZE 72.0
#endif
Texture2D<float4> RippleSlopes : register(t13);
SamplerState RippleSampler : register(s13);
Texture2D<float> SceneDepth : register(t14);
Texture2D<float4> SceneColor : register(t15);
SamplerState SceneSampler : register(s15);
#ifdef DBL_VR
// Skyrim VR: the fitting's world matrix and light positions are per eye, as in
// NativeGeometry and SceneLighting. The single borrowed light is [eye*count].
cbuffer FittingGeometry:register(b8){row_major float3x4 DonorWorldEyes[2];};
cbuffer FittingLighting:register(b7){float4 DonorPositionEyes[14]:packoffset(c27);float4 DonorColor:packoffset(c41);float2 DonorCounts:packoffset(c48);};
#define DBL_DONOR_WORLD DonorWorldEyes[p.eye]
#define DBL_DONOR_POSITION DonorPositionEyes[p.eye*(uint)clamp((int)DonorCounts.x,0,7)]
#else
cbuffer FittingGeometry:register(b8){row_major float3x4 DonorWorld;};
cbuffer FittingLighting:register(b7){float4 DonorChannel:packoffset(c2);float4 DonorPosition:packoffset(c15);float4 DonorColor:packoffset(c22);float2 DonorCounts:packoffset(c29);};
#define DBL_DONOR_WORLD DonorWorld
#define DBL_DONOR_POSITION DonorPosition
#endif
Texture2D<float4> SunShadowMask : register(t38);
#endif
cbuffer Bottle : register(b10) {
    float4 Eye, Plane, Wave, Detail, LiquidColor, GlassColor;
    float4 Rings[16];
    float4 Bounds; // bottom, top, maximum radius, ring count
    float4 Refraction; // enabled, target width/height, maximum glass pixel offset
    float4 Viewport; // origin and extent in target pixels
    float4 BubbleInfo; // active motion bubbles, maximum 64
    float4 Bubbles[64]; // local center and radius
    float4 WetInfo;
    float4 WetFilm[64];
    float4 CavityPlanes[16]; // xy support coefficients, [0].z bound scale, [0].w count
    float4 SurfaceBubbles[64];
    float4 SurfacePop[16];
    float4 TunedModes[3];
    float4 TunedInfo;
    float4 RippleWeights; // cos/sin pairs at 3 Hz and 4.11 Hz, simulation time
    float4 EffectWeights[5];
    float4 Appearance; // shell or native front-glass opacity, liquid-only proxy, liquid alpha override, disable liquid refraction
    float4 ViscousSurface; // meniscus deltas, liquid muddiness
    float4 AuthoredSpec; // native specular RGB times strength, exponent
    float4 LiquidMaterial; // colorless glass, texture strength, designed surface height, flags: native UV=1, texture tint=2, unmirrored full tile=4, independent scroll=8, bulk-only texture pose=16
    float4 LiquidTextureRect; // original diffuse atlas region: minUV, maxUV
    float4 LiquidAnimation; // live native liquid UV offset.xy and scale.zw
    float4 LiquidEffects; // emission, extra glass refraction scale, liquid scene mip, point-light scattering reduction
    float4 LiquidIndependentAnimation; // per-geometry scroll; native UV above still restores stationary glass
    float4 LiquidTexturePlane; // smoothed bulk normal.xyz, fixed upright pivot height.w
    float4 FittingPosition; // verify fitting pose, same-bottle reflection source
    float4 FittingColor;
    float4 FittingRows[3];
    float4 EmissionColor;
    float4 TraceSettings; // sample cap, transmission mip, refraction mip, normal strength delta
    float4 VisualLight; // deltas for sunlight, point, backlight and thickness
    float4 VisualSurface; // glass highlight, liquid visibility, glass visibility, opacity override flag
    float4 RusticMapping; // original donor bottom, top, mapping enabled, reserved
};
#ifndef DBL_VERTEX
#include "CommunityLighting.hlsli"
#endif
float effectWeight(int i){return EffectWeights[i/4][i%4];}
#ifndef DBL_VERTEX
// A liquid-attached volume pattern, independent of the shell UVs. Inverse of
// the shortest rotation from bottle Z to the simulated surface normal.
float3 liquidTexturePosition(float3 q) {
    bool bulk=(((uint)round(LiquidMaterial.w)&16)!=0);
    float3 n=normalize(bulk?LiquidTexturePlane.xyz:Plane.xyz);
    float3 x,y;
    if(n.z>-.9999){float a=1/(1+n.z);x=float3(1-n.x*n.x*a,-n.x*n.y*a,-n.x);y=float3(-n.x*n.y*a,1-n.y*n.y*a,-n.y);}
    else{x=float3(-1,0,0);y=float3(0,1,0);}
    q-=bulk?float3(0,0,LiquidTexturePlane.w):n*Plane.w;
    return float3(dot(q,x),dot(q,y),dot(q,n)+LiquidMaterial.z);
}
// Stable sampling for the bulk mapping. Sidewalls use their fixed shell point;
// the top uses the flat bulk plane, never the waved ray entry or wet thickness.
float3 liquidTextureSamplePoint(float3 shell,float3 eye,float3 dir,float3 legacy){
    if(((uint)round(LiquidMaterial.w)&16)==0)return legacy;
    float3 n=normalize(LiquidTexturePlane.xyz),pivot=float3(0,0,LiquidTexturePlane.w);
    float direction=dot(dir,n);
    if(dot(shell-pivot,n)>0&&direction<-.001){
        float t=dot(pivot-eye,n)/direction;
        if(t>0)return eye+dir*t;
    }
    return shell;
}
float radius(float z);
float2 liquidChartUV(float2 chart,float4 anim,uint flags,bool rustic,float2 inset) {
    if(rustic)return frac(lerp(LiquidTextureRect.xy,LiquidTextureRect.zw,chart)*anim.zw+anim.xy);
    if((flags&1)!=0)chart=frac(chart*anim.zw+anim.xy);
    return lerp(LiquidTextureRect.xy+inset,LiquidTextureRect.zw-inset,chart);
}
float3 texturedLiquidColor(float3 q,float lod,float surfaceDepth,out float3 emissionPattern,float axialView=0) {
    bool rustic=RusticMapping.z>0;
    uint flags=(uint)round(LiquidMaterial.w);
    bool bulk=(flags&16)!=0;
    float low=rustic?RusticMapping.x:Bounds.x;
    float high=rustic?RusticMapping.y:Bounds.y;
    float span=max(.001,high-low);
    float strength=smoothstep(0,.12*span,surfaceDepth);
    if(strength<=0){
        emissionPattern=LiquidColor.rgb;
        return LiquidColor.rgb;
    }
    float3 local=liquidTexturePosition(q);
    float sectionRadius=radius(clamp(q.z,Bounds.x,Bounds.y));
    float radial=length(local.xy)/max(.001,sectionRadius);
    float planarWeight=max(smoothstep(.30,.78,axialView),1-smoothstep(.12,.55,radial));
    float4 anim=(flags&8)!=0?LiquidIndependentAnimation:LiquidAnimation;
    float2 inset=0;
    if(!rustic){
        uint w,h;SurfaceBase.GetDimensions(w,h);
        float2 size=max(float2(w,h),1);
        float2 region=LiquidTextureRect.zw-LiquidTextureRect.xy;
        lod=clamp(lod+log2(max(1,max(region.x*w,region.y*h))/512),0,
                  max(0,log2(max(1,min(region.x*w,region.y*h)/4))));
        inset=min(exp2(lod)/size,region*.25);
    }
    // Blend the sampled textures so angular wrap never interpolates across
    // atlas tiles. Skip the other projection at either end of the blend.
    float2 cylindricalUV=0,planarUV=0;
    float3 cylindrical=0,planar=0;
    if(planarWeight<1){
        float height=local.z+(bulk?LiquidTexturePlane.w:Plane.w)-LiquidMaterial.z;
        float2 chart=float2(.5+atan2(local.y,local.x)/6.2831853,1-saturate((height-low)/span));
        cylindricalUV=liquidChartUV(chart,anim,flags,rustic,inset);
        cylindrical=SurfaceBase.SampleLevel(BaseSampler,cylindricalUV,lod).rgb;
    }
    if(planarWeight>0){
        float3 center=float3(0,0,(Bounds.x+Bounds.y)*.5);
        float halfHeight=(Bounds.y-Bounds.x)*.5;
        float extent=max(.001,length(float2(Bounds.z,halfHeight)));
        float2 chart=saturate(.5+(local.xy-liquidTexturePosition(center).xy)*float2(.5,-.5)/extent);
        planarUV=liquidChartUV(chart,anim,flags,rustic,inset);
        planar=SurfaceBase.SampleLevel(BaseSampler,planarUV,lod).rgb;
    }
    float3 pattern=lerp(cylindrical,planar,planarWeight);
    const float3 luminance=float3(.2126,.7152,.0722);
    float3 hue=LiquidColor.rgb/max(max(LiquidColor.r,LiquidColor.g),max(LiquidColor.b,.0001));
    if(rustic)pattern=hue*dot(pattern,luminance);
    else if((flags&2)!=0)pattern*=LiquidColor.rgb;
    float3 tint=lerp(LiquidColor.rgb,pattern,LiquidMaterial.y*strength);
    emissionPattern=tint;
    if(EmissionColor.w>1.5){
        float3 cylinderGlow=0,planeGlow=0;
        if(planarWeight<1)cylinderGlow=NativeLiquidGlow.SampleLevel(BaseSampler,cylindricalUV,lod).rgb;
        if(planarWeight>0)planeGlow=NativeLiquidGlow.SampleLevel(BaseSampler,planarUV,lod).rgb;
        float3 glow=lerp(cylinderGlow,planeGlow,planarWeight);
        if(rustic)glow=hue*dot(glow,luminance);
        emissionPattern=lerp(LiquidColor.rgb,glow,strength);
    }
    return tint;
}
#endif
struct Vertex { float4 position : POSITION; float4 normal : NORMAL; float2 uv:TEXCOORD0; float4 tangent:TANGENT; };
#ifndef DBL_VERTEX
static float RefractionDepthSign=1;
float orderedSceneDepth(float z){return z*RefractionDepthSign;}
#endif
struct Pixel { float4 fog:COLOR1; float4 position : SV_POSITION; float3 local : TEXCOORD0; nointerpolation float3 localEye : TEXCOORD1; float3 world : TEXCOORD2; nointerpolation float3x3 basis : TEXCOORD3; float3 normal : TEXCOORD6; float2 uv:TEXCOORD7; float3 tangent:TEXCOORD8; float3 bitangent:TEXCOORD9; nointerpolation float3x3 viewBasis:TEXCOORD10; nointerpolation float4 clipOrigin:TEXCOORD13; nointerpolation float4 clipX:TEXCOORD14; nointerpolation float4 clipY:TEXCOORD15; nointerpolation float4 clipZ:TEXCOORD16;
#ifdef DBL_VR
    float eyeClip:SV_ClipDistance0; nointerpolation uint eye:TEXCOORD17;
#endif
};
// The region of the target this pixel's view occupies: the whole viewport, or in
// Skyrim VR the eye's half of the double-wide stereo target. Screen-space
// lookups clamp to it so they never sample the other eye.
#ifdef DBL_VR
#define DBL_EYE_VIEWPORT(p) float4(Viewport.x+Viewport.z*.5*(p).eye,Viewport.y,Viewport.z*.5,Viewport.w)
#else
#define DBL_EYE_VIEWPORT(p) Viewport
#endif
#ifdef DBL_VERTEX
#ifdef DBL_VR
Pixel VSMain(Vertex v,uint instance:SV_InstanceID) {
    const uint eye=StereoInfo.y!=0?(instance&1):0;
    const float3x4 World=WorldEyes[eye];const float4x4 ViewProj=ViewProjEyes[eye];const float3x3 ViewRotation=(float3x3)ViewEyes[eye];
#else
Pixel VSMain(Vertex v) {
#endif
    Pixel o; o.local=v.position.xyz; o.normal=v.normal.xyz*2-1;
    o.uv=v.uv*TexcoordOffset.zw+TexcoordOffset.xy; o.tangent=float3(v.position.w,v.normal.w*2-1,v.tangent.w*2-1);o.bitangent=v.tangent.xyz*2-1;
    o.position=mul(ViewProj,float4(mul(World,float4(v.position.xyz,1)),1));
    // Both captured Skyrim paths upload camera-relative World matrices. Invert
    // that draw's uniform-scale rotation/translation, never the player camera.
    float3x3 basis=(float3x3)World;
    o.basis=basis; o.world=mul(World,float4(v.position.xyz,1));
    o.viewBasis=mul(ViewRotation,basis);
    o.clipOrigin=mul(ViewProj,float4(World[0].w,World[1].w,World[2].w,1));
    o.clipX=mul(ViewProj,float4(World[0].x,World[1].x,World[2].x,0));
    o.clipY=mul(ViewProj,float4(World[0].y,World[1].y,World[2].y,0));
    o.clipZ=mul(ViewProj,float4(World[0].z,World[1].z,World[2].z,0));
    float3 translation=float3(World[0].w,World[1].w,World[2].w);
    o.localEye=-mul(translation,basis)/max(dot(basis[0],basis[0]),1e-8);
    float f=FogParam.y>0?min(saturate(FogParam.w),pow(saturate(length(o.world)*FogParam.y-FogParam.x),max(FogParam.z,.0001))):0;
    o.fog=float4(lerp(FogNearColor.rgb,FogFarColor.rgb,f),f);
#ifdef DBL_VR
    // Same placement as the engine's stereo shaders: the eye's clip edge keeps
    // each instance in its own half of the double-wide target.
    const float4 clip=o.position;const bool stereo=StereoInfo.y!=0;
    o.eyeClip=stereo?dot(clip,EyeClipEdge[eye]):1;
    o.position.x=(2-StereoInfo.y)*clip.x*.5+(stereo?(eye?StereoInfo.w:StereoInfo.z)*clip.w:0);
    o.eye=eye;
#endif
    return o;
}
#else
// Only called after the visible surface hit has been solved, never in tracing.
float3 rippleReflectionNormal(float3 hit,float3 normal,float coverage){
#ifdef DBL_NO_RIPPLES
    return normal;
#else
    float3 up=normalize(Plane.xyz);
    float3 tangent=normalize(cross(abs(up.z)<.95?float3(0,0,1):float3(0,1,0),up));
    float3 bitangent=cross(up,tangent);
    float2 uv=float2(dot(hit,tangent),dot(hit,bitangent))/max(DBL_RIPPLE_SIZE,.01);
    float2 dx=ddx(uv),dy=ddy(uv);
    float footprint=max(length(dx),length(dy));
    float energy=saturate((abs(Wave.x)+abs(Detail.x)+abs(Wave.w))/max(Bounds.z*.15,.001));
    float strength=DBL_RIPPLE_STRENGTH*energy*(1-smoothstep(.025,.12,footprint));
    [branch]if(coverage<=0||strength<=.00001)return normal;
    float lod=max(0,log2(max(footprint*128,1)));
    // Fixed surface coordinates: only the mixture changes, never the UVs.
    float4 first=RippleSlopes.SampleLevel(RippleSampler,uv,lod);
    float4 second=RippleSlopes.SampleLevel(RippleSampler,float2(-uv.y,uv.x)*1.73+.37,lod+.791);
    float2 a=first.rg*RippleWeights.x+first.ba*RippleWeights.y;
    float2 b=second.rg*RippleWeights.z+second.ba*RippleWeights.w;
    float2 slope=a*.65+float2(b.y,-b.x)*.35;
    return normalize(normal+strength*(tangent*slope.x+bitangent*slope.y));
#endif
}
float4 PSRippleTest(float4 position:SV_POSITION):SV_TARGET {
    float3 n=rippleReflectionNormal(float3(position.xy/512*Detail.w,0),float3(0,0,1),1);
    return float4(n*.5+.5,1);
}
float radius(float z) {
    // Fallout LiquidCore: at most four comparisons for 2..16 sorted rings.
    int last=(int)Bounds.w-1;
    if(z<Rings[0].x || z>Rings[last].x)return 0;

    int lo=1,hi=last;
    [unroll]for(int search=0;search<4;++search){
        if(lo<hi){int mid=(lo+hi)>>1;if(z>Rings[mid].x)lo=mid+1;else hi=mid;}
    }
    return lerp(Rings[lo-1].y,Rings[lo].y,saturate((z-Rings[lo-1].x)/max(Rings[lo].x-Rings[lo-1].x,.0001)));
}
float2 sectionGradient(float2 p){
    if(CavityPlanes[0].w<3)return normalize(p+float2(1e-12,0));
    float best=-1e30;float2 result=float2(1,0);
    [loop]for(int i=0;i<(int)CavityPlanes[0].w;++i){
        float d=dot(p,CavityPlanes[i].xy);
        if(d>best){best=d;result=CavityPlanes[i].xy;}
    }
    return result;
}
float crossSection(float2 p){
    if(CavityPlanes[0].w<3)return length(p);
    float support=-1e30;
    // The current polygon profile has 16 support planes. Preserve their order
    // and dot products while removing dynamic loop/index overhead.
    [branch]if(CavityPlanes[0].w==16){
        [unroll]for(int j=0;j<16;++j)support=max(support,dot(p,CavityPlanes[j].xy));
        return support;
    }
    [loop]for(int i=0;i<(int)CavityPlanes[0].w;++i)support=max(support,dot(p,CavityPlanes[i].xy));
    return support;
}
// Optical shading only. Keep crossSection(), intersections and volume geometry
// faceted, but interpolate shared vertex gradients across each polygon side.
// This is the same distinction as smooth shading on a low-poly glass shell.
float2 opticalSectionGradient(float2 p){
#ifdef DBL_FACETED_OPTICAL_NORMALS
    return sectionGradient(p);
#else
    int count=(int)CavityPlanes[0].w;
    if(count<3)return sectionGradient(p);
    float best=-1e30;int face=0;
    [loop]for(int i=0;i<count;++i){
        float d=dot(p,CavityPlanes[i].xy);
        if(d>best){best=d;face=i;}
    }
    float2 current=CavityPlanes[face].xy;
    float2 before=CavityPlanes[face==0?count-1:face-1].xy;
    float2 after=CavityPlanes[face+1==count?0:face+1].xy;
    float detA=before.x*current.y-before.y*current.x;
    float detB=current.x*after.y-current.y*after.x;
    if(abs(detA)<1e-6||abs(detB)<1e-6||best<=1e-8)return current;
    float2 a=float2(current.y-before.y,before.x-current.x)/detA;
    float2 b=float2(after.y-current.y,current.x-after.x)/detB;
    float2 edge=b-a;
    float t=saturate(dot(p/best-a,edge)/max(dot(edge,edge),1e-8));
    return lerp((before+current)*.5,(current+after)*.5,t);
#endif
}
float4 PSShapeTest(float4 position:SV_POSITION):SV_TARGET {
    float2 q=(position.xy/512-.5)*2*Bounds.z;
    return float4(crossSection(q)/(2*Bounds.z),1,0,1);
}
float fieldFromCavity(float3 p,float cavity) {
    float displacement;
    // Zero-wave liquid still needs the original meniscus, including refraction.
    // Skip only mathematically zero wave terms; keep the same tracing lattice.
    [branch]if(Wave.x*Detail.y==0 && Detail.x*Detail.z==0 && Wave.w==0){displacement=0;}
    else [branch]if(TunedInfo.x>0){
        float u=sin(dot(p,TunedModes[0].xyz)+TunedModes[0].w),v=sin(dot(p,TunedModes[1].xyz)+TunedModes[1].w);
        float a=Wave.x*Detail.y,b=Detail.x*Detail.z;
        displacement=a*u+b*v
            +Wave.w*sin(dot(p,TunedModes[2].xyz)+TunedModes[2].w);
#ifndef DBL_DISABLE_TUNED_INTERACTION
        displacement+=TunedInfo.y*.35*(abs(a)+abs(b))*u*v;
#endif
    }else{
    float u=sin(dot(p.xy,float2(.8,.6))*Wave.y+Wave.z*3);
    float v=sin(dot(p,float3(-.48,.64,.6))*Wave.y*1.73-Wave.z*5.1);
    float a=Wave.x*Detail.y, b=Detail.x*Detail.z;
    displacement=a*u+b*v+.35*(abs(a)+abs(b))*u*v;
    [branch] if(Wave.w>0)displacement+=Wave.w*sin(dot(p,float3(.36,-.48,.8))*Wave.y*2.35+Wave.z*4.2+1.1);
    }
    float wall=-cavity;
    float rimHeight=.06,rimWidth=.35;
    [branch]if(ViscousSurface.x>0 || ViscousSurface.y>0){
        float localRadius=radius(clamp(p.z,Bounds.x,Bounds.y));
        rimHeight+=min(.06*ViscousSurface.x,localRadius*.03);
        rimWidth+=min(.35*ViscousSurface.y,localRadius*.15);
    }
    rimWidth*=1+ViscousSurface.w;
    float meniscus=rimHeight*pow(saturate(1-wall/rimWidth),3);
    return dot(p,Plane.xyz)-Plane.w-displacement-meniscus;
}
float cavityField(float3 q);
float field(float3 p){return fieldFromCavity(p,cavityField(p));}
float4 PSTunedWaveTest(float4 position:SV_POSITION):SV_TARGET{
    float3 p=float3((position.xy/32-.5)*2*Bounds.z,(Bounds.x+Bounds.y)*.5);
    // Isolate the shared displacement expression from the unchanged meniscus.
    return float4(-fieldFromCavity(p,-1)+dot(p,Plane.xyz)-Plane.w,0,0,1);
}
// Fallout wall-contact history, bilinearly interpolated with angular wrap.
float wetSample(int row,int angle){int wrapped=angle&15;return WetFilm[row*4+(wrapped>>2)][wrapped&3];}
float wetCoverage(float3 p){
    if(WetInfo.x<=0 || p.z<Bounds.x || p.z>Bounds.y)return 0;
    float above=field(p);if(above<=0)return 0;
    float row=clamp((p.z-WetInfo.y)*WetInfo.z,0,15);
    float angle=(atan2(p.y,p.x)/6.283185307+1)*16;
    int r=(int)floor(row),a=(int)floor(angle);float2 f=frac(float2(angle,row));
    float wet=lerp(lerp(wetSample(r,a),wetSample(r,a+1),f.x),
        lerp(wetSample(min(r+1,15),a),wetSample(min(r+1,15),a+1),f.x),f.y);
    return saturate(wet)*smoothstep(.7,.95,crossSection(p.xy)/max(radius(p.z),.001))*smoothstep(0,.12,above);
}
float segmentWet(float a,float b) {
    if(a<=0&&b<=0)return 1;
    if(a>0&&b>0)return 0;
    float crossing=saturate(a/(a-b));
    return a<=0?crossing:1-crossing;
}
float cavityField(float3 q) {
    float z=clamp(q.z,Bounds.x,Bounds.y);
    return max(crossSection(q.xy)-radius(z),max(Bounds.x-q.z,q.z-Bounds.y));
}
// Preserve Test79's six-point stencil. Away from the wall the meniscus is
// identically zero at all six samples, so cavity tracing can be shared.
float3 surfaceShadingNormal(float3 p){
    const float e=.025;
    bool interior=false;
#ifndef DBL_REFERENCE_SURFACE_NORMAL
    [branch]if(TunedInfo.z>0)interior=cavityField(p)<-.35*(1+ViscousSurface.y)-e*TunedInfo.z-.0001;
#endif
    [branch]if(interior && Wave.x*Detail.y==0 && Detail.x*Detail.z==0 && Wave.w==0)
        return normalize(Plane.xyz);
    float3 x=float3(e,0,0),y=float3(0,e,0),z=float3(0,0,e);
    float xp=-1,xm=-1,yp=-1,ym=-1,zp=-1,zm=-1;
    [branch]if(!interior){
        xp=cavityField(p+x);xm=cavityField(p-x);yp=cavityField(p+y);
        ym=cavityField(p-y);zp=cavityField(p+z);zm=cavityField(p-z);
    }
    return normalize(float3(fieldFromCavity(p+x,xp)-fieldFromCavity(p-x,xm),
        fieldFromCavity(p+y,yp)-fieldFromCavity(p-y,ym),
        fieldFromCavity(p+z,zp)-fieldFromCavity(p-z,zm)));
}
float4 PSIntegrationTest(float4 position:SV_POSITION):SV_TARGET {
    float crossing=position.x/512;
    return float4(segmentWet(-crossing,1-crossing),0,0,1);
}
// Ported from Fallout DBL shaders/BottleLens.hlsli.
// Analytic liquid-cylinder lens. Two Snell interfaces, no marching or textures.
// Radius is the local profile radius; shoulders, caps and rectangular bottles
// remain approximations. Background distance is estimated at four radii.
bool liquidLensRay(float3 origin,float3 incident,float radius,float ior,out float3 exitPoint,out float3 outgoing) {
    exitPoint=origin;outgoing=incident;
    float a=dot(incident.xy,incident.xy);
    if(a<.04 || radius<=.001 || ior<=1.0001)return false;
    float t=-dot(origin.xy,incident.xy)/a;
    float3 closest=origin+incident*t;
    float h2=(radius*radius-dot(closest.xy,closest.xy))/a;
    if(h2<=0)return false;
    float3 entry=closest-incident*sqrt(h2);
    float3 inside=refract(incident,float3(entry.xy/radius,0),1/ior);
    float b=dot(inside.xy,inside.xy);
    if(b<1e-6)return false;
    float travel=-2*dot(entry.xy,inside.xy)/b;
    exitPoint=entry+inside*travel;
    outgoing=refract(inside,-float3(exitPoint.xy/radius,0),ior);
    if(dot(outgoing,outgoing)<.5)return false;
    return true;
}

bool liquidLensTarget(float3 origin,float3 incident,float radius,float ior,out float3 target) {
    float3 exitPoint,outgoing;
    bool valid=liquidLensRay(origin,incident,radius,ior,exitPoint,outgoing);
    target=valid?exitPoint+outgoing*(4*radius):origin;
    return valid;
}

// Reuse the profile segment along a ray and its seven-step refinements.
// The radius itself is recalculated at every point with the original formula.
// Crossing a segment forces the original binary lookup; equality selects the
// preceding segment, matching radius(). The cache is local to this ray.
float cachedRadius(float z,inout int segment){

    int last=(int)Bounds.w-1;
    if(z<Rings[0].x || z>Rings[last].x)return 0;
    [branch]if(segment<1 || z<=Rings[segment-1].x || z>Rings[segment].x){
        int lo=1,hi=last;
        [unroll]for(int search=0;search<4;++search){
            if(lo<hi){int mid=(lo+hi)>>1;if(z>Rings[mid].x)lo=mid+1;else hi=mid;}
        }
        segment=lo;
    }
    return lerp(Rings[segment-1].y,Rings[segment].y,saturate((z-Rings[segment-1].x)/max(Rings[segment].x-Rings[segment-1].x,.0001)));
}
// Reuse ring endpoints as well as their index along the colour integration ray.
// Same interpolation arithmetic and sample positions as the uncached evaluator.
float colorRadius(float z,inout int segment,inout float4 endpoints){

    int last=(int)Bounds.w-1;
    if(z<Rings[0].x || z>Rings[last].x)return 0;
    [branch]if(segment<1 || z<=endpoints.x || z>endpoints.z){
        int lo=1,hi=last;
        [unroll]for(int search=0;search<4;++search){
            if(lo<hi){int mid=(lo+hi)>>1;if(z>Rings[mid].x)lo=mid+1;else hi=mid;}
        }
        segment=lo;endpoints=float4(Rings[lo-1].xy,Rings[lo].xy);
    }
    return lerp(endpoints.y,endpoints.w,saturate((z-endpoints.x)/max(endpoints.z-endpoints.x,.0001)));
}
// Lens callers need only inside/outside, never the numeric distance.
float liquidBoundaryWithCavity(float3 q,float cavity){
    [branch] if(cavity>=0)return cavity;
    float plane=dot(q,Plane.xyz)-Plane.w;
    float bound=1.35*(abs(Wave.x*Detail.y)+abs(Detail.x*Detail.z))+abs(Wave.w)+.06*(1+ViscousSurface.x)+.001;
    [branch] if(plane < -bound)return cavity;
    [branch] if(plane > bound)return plane;
    return max(cavity,fieldFromCavity(q,cavity));
}
float liquidBoundary(float3 q){return liquidBoundaryWithCavity(q,cavityField(q));}
float cachedLiquidBoundary(float3 q,inout int segment,inout float4 endpoints){
    float cavity=max(crossSection(q.xy)-colorRadius(clamp(q.z,Bounds.x,Bounds.y),segment,endpoints),max(Bounds.x-q.z,q.z-Bounds.y));
    return liquidBoundaryWithCavity(q,cavity);
}
// Fallout's 48..96 sample screen-size budget, adapted to Skyrim's native
// camera-relative draw. A conservative projected bounding sphere handles FOV,
// object scale and resolution without a CPU readback or per-pixel derivatives.
int colorTraceSteps(Pixel p,float sphere){
#ifdef DBL_FULL_TRACE
    return 96;
#else
    if(BubbleInfo.z<=0 || DBL_EYE_VIEWPORT(p).z<=0 || DBL_EYE_VIEWPORT(p).w<=0)return 96;
    float4 center=p.clipOrigin+p.clipZ*((Bounds.x+Bounds.y)*.5);
    float wRadius=sphere*length(float3(p.clipX.w,p.clipY.w,p.clipZ.w));
    float nearW=abs(center.w)-wRadius;
    if(nearW<=.0001)return 96;
    float2 centerNdc=abs(center.xy/center.w);
    float2 extent=sphere*float2(length(float3(p.clipX.x,p.clipY.x,p.clipZ.x)),
        length(float3(p.clipX.y,p.clipY.y,p.clipZ.y)))+centerNdc*wRadius;
    float projected=max(DBL_EYE_VIEWPORT(p).z*extent.x,DBL_EYE_VIEWPORT(p).w*extent.y)/nearW;
#ifdef DBL_VR
    projected*=BubbleInfo.w>0?BubbleInfo.w:1; // VR trace budget scale
#endif
    // Same small/full thresholds as Fallout, but gradual integer steps instead
    // of 48/72/96 tier jumps. No opacity, wave amplitude or lens fade.
    if(projected<48)return (int)round(lerp(24,48,smoothstep(12,48,projected)));
    return (int)round(lerp(48,96,smoothstep(48,128,projected)));
#endif
}
void traceRange(float3 origin,float3 direction,float nearT,float farT,int steps,out int first,out int last){
    first=0;last=steps;
#ifndef DBL_FULL_TRACE
    float stepSize=(farT-nearT)/steps;
    if(stepSize<=1e-8)return;
    float radius=Bounds.z*max(1,CavityPlanes[0].z);
    float3 low=float3(-radius,-radius,Bounds.x),high=float3(radius,radius,Bounds.y);
    float enter=nearT,leave=farT;
    [unroll]for(int axis=0;axis<3;++axis){
        if(abs(direction[axis])<1e-8){if(origin[axis]<low[axis]||origin[axis]>high[axis]){last=0;return;}}
        else{float a=(low[axis]-origin[axis])/direction[axis],b=(high[axis]-origin[axis])/direction[axis];enter=max(enter,min(a,b));leave=min(leave,max(a,b));}
    }
    // The full cavity also fits inside this vertical cylinder. Tighten the
    // conservative interval, retaining the original lattice and padded ends.
    float radial=dot(direction.xy,direction.xy);
    if(radial>1e-8){
        float projected=-dot(origin.xy,direction.xy)/radial;
        float2 closest=origin.xy+direction.xy*projected;
        float marginRadius=radius+.001;
        float halfSquared=(marginRadius*marginRadius-dot(closest,closest))/radial;
        if(halfSquared<0){last=0;return;}
        float halfSpan=sqrt(halfSquared);
        enter=max(enter,projected-halfSpan);leave=min(leave,projected+halfSpan);
    }
    // Bound all THREE Skyrim waves, their coupling and the meniscus. Keep the
    // old lattice and a full padding segment for interpolated boundary coverage.
    float a=abs(Wave.x*Detail.y),b=abs(Detail.x*Detail.z);
    float height=Plane.w+1.35*(a+b)+max(Wave.w,0)+.06*(1+ViscousSurface.x)+.0001;
    float slope=dot(direction,Plane.xyz),start=dot(origin,Plane.xyz);
    if(abs(slope)>1e-8){float crossing=(height-start)/slope;if(slope>0)leave=min(leave,crossing);else enter=max(enter,crossing);}
    else if(start>height){last=0;return;}
    if(leave<enter){last=0;return;}
    first=clamp((int)floor((enter-nearT)/stepSize)-1,0,steps);
    last=clamp((int)ceil((leave-nearT)/stepSize)+1,first,steps);
#endif
}
float profileSlope(int i){
    return (Rings[i+1].y-Rings[i].y)/max(Rings[i+1].x-Rings[i].x,.0001);
}
float3 opticalRingNormal(int i){
    int last=(int)Bounds.w-1;
    float left=profileSlope(max(i-1,0));
    float right=profileSlope(min(i,last-1));
    // Average unit segment normals, as a smoothly shaded surface of revolution.
    return normalize(normalize(float3(1,0,-left))+normalize(float3(1,0,-right)));
}
float3 liquidBoundaryNormal(float3 q){
    float e=.015;
    // Keep intersection/volume geometry exact, but do not refract with the
    // piecewise-conical face normals: they split the image into height bands.
    float cavity=cavityField(q);
    if(fieldFromCavity(q,cavity)>=cavity)
        return normalize(float3(field(q+float3(e,0,0))-field(q-float3(e,0,0)),
            field(q+float3(0,e,0))-field(q-float3(0,e,0)),
            field(q+float3(0,0,e))-field(q-float3(0,0,e))));
    float wall=crossSection(q.xy)-radius(clamp(q.z,Bounds.x,Bounds.y));
    if(max(Bounds.x-q.z,q.z-Bounds.y)>wall)
        return float3(0,0,q.z<(Bounds.x+Bounds.y)*.5?-1:1);
    [loop]for(int i=1;i<(int)Bounds.w;++i){
        if(q.z>=Rings[i-1].x && q.z<=Rings[i].x){
            float t=saturate((q.z-Rings[i-1].x)/max(Rings[i].x-Rings[i-1].x,.0001));
            float3 n=normalize(lerp(opticalRingNormal(i-1),opticalRingNormal(i),t));
            return normalize(float3(opticalSectionGradient(q.xy)*n.x,n.z));
        }
    }
    return float3(0,0,q.z<Bounds.x?-1:1);
}
// Trace only the refracted interior segment. Entry comes from the existing
// volume integration; exit respects the profile AND current liquid surface.
bool traceLiquidLens(float3 entry,float3 incident,int bounceLimit,out float3 exitPoint,out float3 outgoing,out bool reflected){
    exitPoint=entry;outgoing=incident;reflected=false;
    float3 n=liquidBoundaryNormal(entry);
    if(dot(n,incident)>=0)return false;
    float3 inside=refract(incident,n,1/1.333);
    if(dot(inside,inside)<.5)return false;
    float maximum=2*length(float2(Bounds.z*max(1,CavityPlanes[0].z),(Bounds.y-Bounds.x)*.5));
    float stepSize=maximum/48;
    [loop]for(int bounce=0;bounce<=bounceLimit;++bounce){
        float lower=.005;
        int segment=0;float4 endpoints=0;
        if(cachedLiquidBoundary(entry+inside*lower,segment,endpoints)>0)return false;
        bool hit=false;
        [loop]for(int i=1;i<=48;++i){
            float upper=i*stepSize;
            if(cachedLiquidBoundary(entry+inside*upper,segment,endpoints)>=0){
                [unroll]for(int j=0;j<7;++j){float mid=(lower+upper)*.5;
                    if(cachedLiquidBoundary(entry+inside*mid,segment,endpoints)<0)lower=mid;else upper=mid;}
                exitPoint=entry+inside*((lower+upper)*.5);
                n=liquidBoundaryNormal(exitPoint);
                outgoing=refract(inside,-n,1.333);
                if(dot(outgoing,outgoing)>.5)return true;
                // Total internal reflection is a continuing optical path, not
                // permission to replace the liquid image with glass-only bend.
                reflected=true;inside=normalize(reflect(inside,n));outgoing=inside;
                entry=exitPoint;hit=true;break;
            }
            lower=upper;
        }
        if(!hit)return false;
    }
    return false;
}
bool profileLiquidLensRay(float3 entry,float3 incident,out float3 exitPoint,out float3 outgoing){
    bool reflected;return traceLiquidLens(entry,incident,0,exitPoint,outgoing,reflected);
}
float3 liquidBackgroundTarget(float3 exitPoint,float3 outgoing,float3 forward){
    // Project onto one camera-facing background plane instead of walking a
    // fixed distance on every ray (a curved target that adds a dome-like warp).
    // Depth remains estimated; this does not reconstruct off-screen scenery.
    float3 center=float3(0,0,(Bounds.x+Bounds.y)*.5);
    float distance=4*Bounds.z+dot(center-exitPoint,forward);
    return exitPoint+outgoing*(max(0,distance)/max(dot(outgoing,forward),.15));
}
float4 PSBackgroundPlaneTest(Pixel p):SV_TARGET {
    float3 center=float3(0,0,(Bounds.x+Bounds.y)*.5);
    float3 forward=float3(0,1,0);
    float depth=0;
    [unroll]for(int i=-2;i<=2;++i){
        float3 ray=normalize(float3(i*.3,1,0));
        float3 target=liquidBackgroundTarget(center,ray,forward);
        depth=max(depth,abs(dot(target-center,forward)-4*Bounds.z));
    }
    return depth<.001?float4(0,1,0,1):float4(1,0,0,1);
}
float4 PSLensBoundaryTest(Pixel p):SV_TARGET {
    float3 exitPoint,outgoing;float3 entry=float3(0,0,Plane.w);
    bool valid=profileLiquidLensRay(entry,float3(0,0,-1),exitPoint,outgoing);
    return valid&&abs(exitPoint.z-Bounds.x)<.03?float4(0,1,0,1):float4(1,0,0,1);
}
// Probe adjacent rays across submerged profile rings, away from the cap and
// free-surface boundaries. A visible refracted-direction step is a failure.
float4 PSLensContinuityTest(Pixel p):SV_TARGET {
    float jump=0;
    [loop]for(int i=1;i<(int)Bounds.w-1;++i){
        float z=Rings[i].x;
        if(z>Bounds.x+.1 && z<Plane.w-.5){
            float3 a=float3(radius(z-.03),0,z-.03);
            float3 b=float3(radius(z+.03),0,z+.03);
            float3 ra=refract(float3(-1,0,0),liquidBoundaryNormal(a),1/1.333);
            float3 rb=refract(float3(-1,0,0),liquidBoundaryNormal(b),1/1.333);
            jump=max(jump,length(ra-rb));
        }
    }
    return jump<.035?float4(0,1,0,1):float4(1,0,0,1);
}
float3 shadingNormal(Pixel p) {
    // Preserve the mesh author's smoothing across height rings and triangles.
    // Derivative normals are face normals and visibly band this low-poly shell.
    return normalize(p.normal);
}
// Probe the actual entry refraction across every angular side boundary.
float4 PSAngularContinuityTest(float4 position:SV_POSITION):SV_TARGET {
    float jump=0;int count=(int)CavityPlanes[0].w;
    float z=lerp(Bounds.x,Bounds.y,.35),r=radius(z);
    [loop]for(int i=0;i<count;++i){
        float2 a=CavityPlanes[i].xy,b=CavityPlanes[i+1==count?0:i+1].xy;
        float det=a.x*b.y-a.y*b.x;
        float2 vertex=float2(b.y-a.y,a.x-b.x)/det;
        float2 delta=float2(-vertex.y,vertex.x)*.0001;
        float2 left=vertex-delta,right=vertex+delta;
        left*=r/crossSection(left);right*=r/crossSection(right);
        float3 incident=-normalize(float3(vertex,0));
        float3 x=refract(incident,liquidBoundaryNormal(float3(left,z)),1/1.333);
        float3 y=refract(incident,liquidBoundaryNormal(float3(right,z)),1/1.333);
        jump=max(jump,length(x-y));
    }
    return float4(jump,0,0,1);
}
// Use the authored outward orientation, independent of projection handedness.
// Native Skyrim views and offline LH views have opposite screen winding.
void selectFrontShell(Pixel p){
    float3 face=cross(ddx(p.local),ddy(p.local));
    if(dot(face,p.normal)<0)face=-face;
    clip(dot(face,p.localEye-p.local));
}
float4 PSFacingTest(Pixel p):SV_TARGET {
    selectFrontShell(p);
    return dot(p.normal,normalize(p.localEye-p.local))>0?float4(1,0,0,1):float4(0,1,0,1);
}
float4 PSNormalTest(Pixel p):SV_TARGET {
#ifdef DBL_REFERENCE_NORMAL
    return float4(normalize(p.normal)*.5+.5,1);
#else
    return float4(shadingNormal(p)*.5+.5,1);
#endif
}
Texture2D<float4> DownsampledScene : register(t12);
static float UseDownsampledScene=0;
// Diagnostic builds may override this; normal rendering is the default.
#ifndef DBL_REFRACTION_DIAGNOSTIC
#define DBL_REFRACTION_DIAGNOSTIC 0
#endif
#if DBL_REFRACTION_DIAGNOSTIC
static int DiagnosticState=0;
float3 diagnosticColor(int state){
    if(state==0)return float3(.25,.25,.25); // No liquid on this ray.
    if(state==1)return float3(1,1,1);       // Lens disabled or unavailable.
    if(state==2)return float3(1,0,0);       // No valid entry crossing.
    if(state==3)return float3(1,.2,0);      // Interior trace failed.
    if(state==4)return float3(1,0,1);       // Internal reflection path failed.
    if(state==5)return float3(0,0,1);       // Projection unusable or off screen.
    if(state==6)return float3(0,1,1);       // Foreground depth rejection.
    if(state==8)return float3(1,1,0);       // Valid, but <=10% scene transmission.
    return float3(0,1,0);                  // Valid screen-space liquid lens.
}

int diagnosticFinalState(int state,float3 transmission,bool foreground){
    if(state==7||state==8){
        if(foreground)return 6;
        if(max(transmission.x,max(transmission.y,transmission.z))<=.1)return 8;
    }
    return state;
}
#endif
float3 sampleRefractionScene(float2 pixel,float2 size,float depth){
    if(UseDownsampledScene>.5){
        uint w,h,levels;DownsampledScene.GetDimensions(0,w,h,levels);
        uint mip=min((uint)max(0,round(UseDownsampledScene)-1),max(1,levels)-1);
        DownsampledScene.GetDimensions(mip,w,h,levels);
        if(w>0&&h>0){
            float2 uv=pixel/size;int2 corner=int2(floor(uv*float2(w,h)-.5));bool safe=true;
            [unroll]for(int y=0;y<2;y++)[unroll]for(int x=0;x<2;x++)
                safe=safe&&(DownsampledScene.Load(int3(clamp(corner+int2(x,y),int2(0,0),int2(w-1,h-1)),mip)).a>=orderedSceneDepth(depth)-1e-6);
            if(safe)return DownsampledScene.SampleLevel(SceneSampler,uv,mip).rgb;
        }
    }
    return SceneColor.SampleLevel(SceneSampler,pixel/size,0).rgb;
}

#if defined(DBL_FORWARD)
// Same L/T resolve as the late compositor, at the verified pre-HDR boundary.
// Replace destination colour: never add an unbent destination image.
#define OpticalOutput float4
static float2 ForwardPixel;
void rejectForwardForeground(float2 pixel,float depth){
    if(orderedSceneDepth(SceneDepth.Load(int3(int2(pixel),0)))<orderedSceneDepth(depth)-1e-6)discard;
}
float4 opticalResult(float3 light,float3 transmission,float2 target,float depth){
    float2 pixel=ForwardPixel; // supplied below through the pixel entry point
    float2 size=Refraction.yz;
    float2 shifted=clamp(target,.5,size-.5);bool valid=true;
    // Only the sampled footprint contributes color. Intermediate screen pixels
    // are not points on the refracted 3D ray (e.g. a bottle cork or strap).
    {
        int2 corner=int2(floor(shifted-.5));
        [unroll]for(int y=0;y<2;y++)[unroll]for(int x=0;x<2;x++){
            int2 tap=clamp(corner+int2(x,y),int2(0,0),int2(size)-1);
            if(orderedSceneDepth(SceneDepth.Load(int3(tap,0)))<orderedSceneDepth(depth)-1e-6)valid=false;
        }
    }
#if DBL_REFRACTION_DIAGNOSTIC
    return float4(diagnosticColor(diagnosticFinalState(DiagnosticState,transmission,!valid)),1);
#endif
    float3 scene=valid?sampleRefractionScene(shifted,size,depth):SceneColor.Load(int3(int2(pixel),0)).rgb;
    // Match the existing half-float optical targets, including quantization.
    light=f16tof32(f32tof16(light));transmission=f16tof32(f32tof16(transmission));
    return float4(light+transmission*scene,1);
}
#elif defined(DBL_OPTICAL)
struct OpticalOutput { float4 light:SV_TARGET0;float4 transmission:SV_TARGET1;float4 distortion:SV_TARGET2; };
OpticalOutput opticalResult(float3 light,float3 transmission,float2 target,float depth){
    OpticalOutput o;o.light=float4(light,1);o.transmission=float4(transmission,1);o.distortion=float4(target,depth,1+UseDownsampledScene);
#if DBL_REFRACTION_DIAGNOSTIC
    int state=diagnosticFinalState(DiagnosticState,transmission,false);
    o.light=float4(diagnosticColor(state),1);o.transmission=0;o.distortion.w=100+state;
#endif
    return o;
}
#endif
#ifdef DBL_FORWARD
OpticalOutput PSMain(Pixel p):SV_TARGET {
    ForwardPixel=p.position.xy;
#elif defined(DBL_OPTICAL)
OpticalOutput PSMain(Pixel p) {
#else
// Opaque native-glass route writes the solved liquid entry, never the dry proxy.
#ifdef DBL_NATIVE_GLASS
static float NativeLiquidDepth;
#define PSMain PSNativeLiquidColor
#endif
float4 PSMain(Pixel p) : SV_TARGET {
#endif
    RefractionDepthSign=(Refraction.x==2||Refraction.x==4)?-1:1;
    if(Refraction.x>=3)RefractionDepthSign=ProjectionDepthSign(RefractionDepthSign);
#ifdef DBL_REFERENCE_EYE
    p.localEye=Eye.xyz; // Offline camera-reference path; production uses VS-derived eye.
#endif
    selectFrontShell(p);
#ifdef DBL_FORWARD
    // Reject foreground before tracing the bottle. Refraction still tests the shifted taps.
    rejectForwardForeground(p.position.xy,p.position.z);
#endif
#ifndef DBL_DISABLE_NATIVE_LIQUID_UV
    // The cloned UV controller is evaluated by Skyrim on this visible material.
    // Undo its transform for glass detail; only the liquid sampler consumes it.
    // Patches using this mode preserve identity static UVs on the source glass.
    if((((uint)round(LiquidMaterial.w)&1)!=0))p.uv=(p.uv-LiquidAnimation.xy)/max(abs(LiquidAnimation.zw),.000001);
#endif
    float detailLOD=Eye.z==-12345?Eye.x:1;
    bool staticLOD=Eye.z==-12345&&Eye.y<=0;
    float3 dir=normalize(p.local-p.localEye);
    float3 center=float3(0,0,(Bounds.x+Bounds.y)*.5);
    float sphere=sqrt(pow(Bounds.z*max(1,CavityPlanes[0].z),2)+pow((Bounds.y-Bounds.x)*.5,2));
    float3 oc=p.localEye-center; float b=dot(oc,dir), c=dot(oc,oc)-sphere*sphere;
    float discriminant=b*b-c; float lengthWet=0; float wetEntry=1e30; float wetEntryStep=0; float entrySurfaceDepth=0; float surface=0; float3 surfaceHit=0;
    if(discriminant>0&&effectWeight(19)>0) {
        float nearT=max(0,-b-sqrt(discriminant)); float farT=-b+sqrt(discriminant);
        // Start and end the trace on the axial caps. A sidewall and the floor
        // can otherwise cross between two samples that both appear dry.
        if(abs(dir.z)>1e-8){
            float2 caps=(Bounds.xy-p.localEye.z)/dir.z;
            nearT=max(nearT,min(caps.x,caps.y));
            farT=max(nearT,min(farT,max(caps.x,caps.y)));
        }
        int steps=staticLOD?12:colorTraceSteps(p,sphere),first,last;
        int cap=TraceSettings.x>0?(int)clamp(TraceSettings.x,12,192):48;
        steps=staticLOD?6:max(1,(steps*cap+48)/96);
        float span=max(0,farT-nearT);
        float stepSize=span/steps;
        traceRange(p.localEye,dir,nearT,farT,steps,first,last);
        float3 previous=p.localEye+dir*(nearT+first*stepSize);
        int colorSegment=0;float4 colorEndpoints=0;
        float previousWall=crossSection(previous.xy)-colorRadius(clamp(previous.z,Bounds.x,Bounds.y),colorSegment,colorEndpoints);
        float previousCavity=max(previousWall,max(Bounds.x-previous.z,previous.z-Bounds.y));
        float previousSurface=staticLOD?dot(previous,Plane.xyz)-Plane.w:fieldFromCavity(previous,previousCavity);
        [loop] for(int i=first;i<last;i++) {
            float3 q=p.localEye+dir*(nearT+(i+1)*stepSize);
            float wall=crossSection(q.xy)-colorRadius(clamp(q.z,Bounds.x,Bounds.y),colorSegment,colorEndpoints);
            float cavity=max(wall,max(Bounds.x-q.z,q.z-Bounds.y));
            float f=staticLOD?dot(q,Plane.xyz)-Plane.w:fieldFromCavity(q,cavity);
            // Wall entry and free-surface exit can both occur within one step.
            // Intersect their linear intervals before computing occupied length.
            float enter=0,leave=1;
            float2 ends=float2(wall,f),starts=float2(previousWall,previousSurface);
            float2 delta=ends-starts;
            [unroll]for(int boundary=0;boundary<2;++boundary){
                if(starts[boundary]>0 && ends[boundary]>0)leave=0;
                else if(starts[boundary]>0)enter=max(enter,-starts[boundary]/delta[boundary]);
                else if(ends[boundary]>0)leave=min(leave,-starts[boundary]/delta[boundary]);
            }
            float wetPart=max(0,leave-enter);
            if(wetPart>0 && wetEntry==1e30){
                wetEntry=nearT+(i+enter)*stepSize;wetEntryStep=stepSize;
                entrySurfaceDepth=max(0,-lerp(previousSurface,f,enter));
            }
            lengthWet+=wetPart*stepSize;
            if(previousSurface>0&&f<=0) {
                float fraction=saturate(previousSurface/(previousSurface-f));
                float3 hit=lerp(previous,q,fraction);
                float coverage=saturate(-cavityField(hit)/max(stepSize,.001));
                if(surface==0&&coverage>0){surface=coverage;surfaceHit=hit;}
            }
            previous=q;previousSurface=f;previousWall=wall;
        }
    }
#ifdef DBL_SURFACE_DIAGNOSTIC
    // Actual integrated occupancy and top boundary; no light, normals or optics.
    return lengthWet>0 ? (surface>.5?float4(1,.4,.15,1):float4(.55,.02,.02,1)) : float4(0,0,0,1);
#endif
#ifdef DBL_NATIVE_GLASS
    clip(lengthWet-0.00001);
    float3 nativeEntry=p.localEye+dir*wetEntry;
    float4 nativeClip=p.clipOrigin+p.clipX*nativeEntry.x+p.clipY*nativeEntry.y+p.clipZ*nativeEntry.z;
    NativeLiquidDepth=saturate(nativeClip.z/nativeClip.w);
#endif
    float3 normal=shadingNormal(p);
    uint textureFlags=(uint)Eye.w;
    float gloss=1;float wear=0;float3 wearColor=GlassColor.rgb;
    if((textureFlags&1)&&effectWeight(12)>0&&LiquidMaterial.x<.5){
        float4 base=SurfaceBase.Sample(BaseSampler,p.uv);
        // Preserve the original authored pattern and color. Do not extract
        // high-frequency contrast or normalize the texture's color channels.
        wear=saturate(base.a)*.55*effectWeight(12);
        wearColor=max(0,base.rgb);
    }
    if((textureFlags&2)&&effectWeight(12)>0){
        float4 mapped=SurfaceNormal.Sample(NormalSampler,p.uv);
        float3 tangentNormal=normalize(mapped.xyz*2-1);
        float3 detailNormal=normalize(p.tangent*tangentNormal.x+p.bitangent*tangentNormal.y+p.normal*tangentNormal.z);
        normal=normalize(lerp(normal,detailNormal,effectWeight(12)));gloss=saturate(mapped.w);
    }
    float3 view=-dir;
    if(dot(normal,view)<0)normal=-normal;
    float fresnel=(.04+.96*pow(1-saturate(dot(normal,view)),5))*effectWeight(18);
    float liquidAlpha=1-exp(-lengthWet*LiquidColor.w*.45*effectWeight(14));
    if((Appearance.z>0 || VisualSurface.w>0) && lengthWet>0 && effectWeight(14)>0)liquidAlpha=Appearance.z;
    // A soft view-relative highlight is an appearance approximation, not sampled
    // scene lighting or environment reflection. Keep it subtle in dark scenes.
    float3 tangent=normalize(cross(abs(view.z)<.95?float3(0,0,1):float3(0,1,0),view));
    float3 light=normalize(view+tangent*.65+float3(0,0,.5));
    float highlight=pow(saturate(dot(normal,normalize(light+view))),AuthoredSpec.w)*.28*gloss;
    float glassAlpha=saturate(Appearance.x+.35*fresnel+highlight);
#ifdef DBL_SCATTERING_TEST
    return float4(liquidAlpha,0,saturate(lengthWet/max(Bounds.z*.6,.001)),1);
#endif
    float thickness=lengthWet/max(Bounds.z*2,.001)*effectWeight(16)*(1+VisualLight.w);
    // Wavelength-dependent attenuation: shallow paths stay brighter while
    // longer paths become darker and more saturated without contour thresholds.
    // Extinction still controls transmission separately. Retain a lit scattering
    // contribution in thick liquid instead of driving its local radiance to black.
    float3 emissionPattern=RusticMapping.z>0 ? LiquidColor.rgb : 1;
    float3 liquidTint=LiquidColor.rgb;
    [branch] if(LiquidMaterial.y>0&&(textureFlags&1)){
        // Derivatives execute uniformly for this draw, before the occupancy test.
        float textureLod=max(0,log2(max(length(ddx(p.local)),length(ddy(p.local)))*512/max(Bounds.z*2,.001)));
        if(lengthWet>0){
            bool bulk=((uint)round(LiquidMaterial.w)&16)!=0;
            float3 textureAxis=normalize(bulk?LiquidTexturePlane.xyz:Plane.xyz);
            float axialView=abs(dot(dir,textureAxis));
            float3 samplePoint=surface>0?surfaceHit:p.localEye+dir*(wetEntry+min(lengthWet*.15,Bounds.z*.15));
            samplePoint=liquidTextureSamplePoint(p.local,p.localEye,dir,samplePoint);
            liquidTint=texturedLiquidColor(samplePoint,textureLod,surface>0?0:entrySurfaceDepth,emissionPattern,axialView);
        }
    }
    float3 liquid=liquidTint*1.3*(.30+.70*exp(-thickness*float3(1.0,2.3,1.8)));
    // Camera-origin bubble intersections work regardless of which shell triangle
    // generated the pixel; absorption starts at the ray's liquid entry.
    // Fallout's analytic bubble rims: no ray-march sampling bands. This only
    // adds particle radiance; Test21 extinction and optical transmission stay intact.
    [loop] for(int k=0;k<min((int)BubbleInfo.x,64);++k){
        float3 offset=Bubbles[k].xyz-p.localEye;
        float along=dot(offset,dir),r=Bubbles[k].w;
        float distance2=max(0,dot(offset,offset)-along*along);
        if(along>0 && r>0 && distance2<r*r && lengthWet>0 && field(Bubbles[k].xyz)<0){
            float radial=distance2/(r*r);
            float rim=smoothstep(.1,1,radial);
            float edge=1-smoothstep(.85,1,radial);
            float visibility=exp(-(LiquidColor.w*.65+ViscousSurface.z/max(Bounds.z*.6,.001))*min(max(0,along-wetEntry),lengthWet));
            liquid+=float3(.65,.77,.8)*(.14+.9*rim)*edge*visibility;
        }
    }
    float3 surfaceNormal=normalize(Plane.xyz);
    if(surface>0&&!staticLOD&&effectWeight(8)>0){
        surfaceNormal=surfaceShadingNormal(surfaceHit);
    }
    surfaceNormal=normalize(lerp(normalize(Plane.xyz),surfaceNormal,effectWeight(8)));
    // Restrict the extra reflection to the actual liquid/glass contact region.
    // Use the existing meniscus width; filter subpixel rims without widening
    // them into a solid outline. No radiance is added without an incident light.
    float contactGap=max(0,-cavityField(surfaceHit));
    float footprint=max(fwidth(contactGap),.001);
    float rimWidth=.35*(1+ViscousSurface.y)*(1+ViscousSurface.w);
    float meniscusRim=effectWeight(10)*surface*(1-smoothstep(0,rimWidth+footprint,contactGap))*rimWidth/(rimWidth+footprint)*(1+ViscousSurface.x);
#ifdef DBL_NO_MENISCUS
    meniscusRim=0;
#endif
    float foamSignal=0;
#ifndef DBL_NO_SURFACE_FOAM
    float facing=dot(surfaceNormal,-dir);
    if(facing>.05){
        [loop]for(int i=0;i<min((int)BubbleInfo.y,64);++i){
            float r=SurfaceBubbles[i].w;
            float3 cap=SurfaceBubbles[i].xyz+normalize(Plane.xyz)*(r*.65);
            float t=dot(cap-p.localEye,surfaceNormal)/dot(dir,surfaceNormal);
            float3 q=p.localEye+dir*t;
            float disk=length(q-cap)/max(r,.001);
            if(r>.001&&t>0&&cavityField(q)<.02){
                float rim=smoothstep(.35,.75,disk)*(1-smoothstep(.82,1,disk));
                float burst=SurfacePop[i/4][i%4];
                // Tear the membrane into retreating arcs at fixed cell radius.
                float3 tangent=normalize(cross(surfaceNormal,abs(surfaceNormal.z)<.9?float3(0,0,1):float3(0,1,0)));
                float3 bitangent=cross(surfaceNormal,tangent);
                float angle=atan2(dot(q-cap,bitangent),dot(q-cap,tangent));
                float arc=.5+.5*sin(angle*3+i*2.399963);
                float membrane=burst>0?smoothstep(burst-.05,burst+.05,arc)*(1-burst):1;
                foamSignal+=rim*membrane*smoothstep(.05,.2,facing);
            }
        }
    }
#endif
    foamSignal=saturate(foamSignal);
    float3 reflectionNormal=surfaceNormal;
    if(effectWeight(7)>0)reflectionNormal=normalize(lerp(surfaceNormal,rippleReflectionNormal(surfaceHit,surfaceNormal,surface),effectWeight(7)));
    float liquidFresnel=.02+.98*pow(1-saturate(dot(reflectionNormal,view)),5);
    float3 surfaceLight=surface*(liquidTint*.16+.14*pow(saturate(dot(reflectionNormal,normalize(light+view))),48));
    surfaceLight*=effectWeight(9);
    surfaceLight+=foamSignal*float3(.28,.31,.30);
    float3 glass=GlassColor.rgb*(.3+.55*fresnel);
    float3 glassHighlight=highlight*float3(.9,.95,1)*effectWeight(11);
    float3 reflectedLight=0;
    float3 wearLighting=1;
    float wet=wetCoverage(p.local);
    if(GlassColor.w>=1&&effectWeight(13)>0) {
        // Default Skyrim lighting uses model space. Environment maps and CS
        // use world space. Evaluate each set of lights in its supplied space.
        bool useCS=csLighting();
        bool modelLighting=!useCS&&(textureFlags&8)!=0;
        float3 worldNormal=normalize(mul(p.basis,normal));
        float3 worldView=normalize(-p.world);
        float3 n=modelLighting?normal:worldNormal;
        float3 v=modelLighting?view:worldView;
        float3 lightingPosition=modelLighting?p.local:p.world;
        float3 l=LightDirection/max(length(LightDirection),.0001);
        float3 directionalColor=useCS?csDirectional(max(0,LightColor)):max(0,LightColor);
        float3 ambientNormal=max(0,mul(Ambient,float4(n,1)));
        float3 ambientVolume=max(0,float3(Ambient[0].w,Ambient[1].w,Ambient[2].w));
        float3 illumination=(useCS?csAmbient(ambientNormal):ambientNormal)+directionalColor*saturate(dot(n,l));
        float3 specular=directionalColor*pow(saturate(dot(n,normalize(l+v+1e-6))),AuthoredSpec.w)*gloss*AuthoredSpec.rgb;
        float3 wetSpecular=directionalColor*pow(saturate(dot(n,normalize(l+v+1e-6))),24);
        float3 sn=modelLighting?surfaceNormal:normalize(mul(p.basis,surfaceNormal));
        float3 worldReflectionNormal=normalize(mul(p.basis,reflectionNormal));
        float3 rn=modelLighting?reflectionNormal:worldReflectionNormal;
        float3 hitPosition=modelLighting?surfaceHit:p.world+mul(p.basis,surfaceHit-p.local);
        // Broad forward scattering when the light is behind the bottle. This is
        // scene-lit (no emissive floor) and does not change liquid coverage.
        float backlightStrength=.325*(1+VisualLight.z);
        float backlight=pow(saturate(dot(-v,l)),4);
        float3 liquidSun=directionalColor;
        [branch] if(textureFlags&16)liquidSun*=SunShadowMask.Load(int3(int2(p.position.xy),0)).r;
        float3 volumeLight=(useCS?csAmbient(ambientVolume):ambientVolume)+liquidSun*(1+VisualLight.x)*(.45+backlightStrength*backlight);
        float pointLightScattering=1-LiquidEffects.w;
        float3 ambientTop=max(0,mul(Ambient,float4(sn,1)));
        float3 topLight=(useCS?csAmbient(ambientTop):ambientTop)+liquidSun*(1+VisualLight.x)*saturate(dot(sn,l));
        float3 topSpecular=liquidSun*pow(saturate(dot(sn,normalize(l+v+1e-6))),48);
        float3 rippleSpecular=liquidSun*pow(saturate(dot(rn,normalize(l+v+1e-6))),48);
        uint2 cluster=0;if(useCS)cluster=csCluster(p.position);
        uint lightCount=useCS?min(CSStrictCount,15)+cluster.y:(uint)clamp((int)LightCounts.x,0,7);
        bool reflectionSource=FittingPosition.y>.5;
        bool fromDraw=FittingColor.w!=0 && DBL_DONOR_POSITION.w>0 &&
            (reflectionSource ? DonorCounts.x>=1 && DonorCounts.y>=1 : DonorCounts.x==1);
        if(fromDraw && FittingPosition.x>0 && !useCS && lightCount==0){
            float3 offset=float3(FittingRows[0].w,FittingRows[1].w,FittingRows[2].w);
            float3 origin=p.world+mul(p.basis,offset-p.local);
            float3x3 expected=mul(p.basis,float3x3(FittingRows[0].xyz,FittingRows[1].xyz,FittingRows[2].xyz));
            float3x3 diff=abs(expected-(float3x3)DBL_DONOR_WORLD);
            fromDraw=all(abs(origin-float3(DBL_DONOR_WORLD[0].w,DBL_DONOR_WORLD[1].w,DBL_DONOR_WORLD[2].w))<.05)
                && all(max(diff[0],max(diff[1],diff[2]))<.002);
        }
        bool addDonor=!useCS && fromDraw && (reflectionSource||lightCount==0);
        float4 donorPosition=DBL_DONOR_POSITION;
        if(addDonor){
            if(FittingColor.w>0){
                float4 pos=float4(donorPosition.xyz,1);
                donorPosition=float4(dot(FittingRows[0],pos),dot(FittingRows[1],pos),dot(FittingRows[2],pos),donorPosition.w*FittingColor.w);
                if(!modelLighting){
                    donorPosition.xyz=p.world+mul(p.basis,donorPosition.xyz-p.local);
                    donorPosition.w*=length(mul(p.basis,float3(1,0,0)));
                }
            }else if(modelLighting){
                float scale=max(length(mul(p.basis,float3(1,0,0))),.0001);
                donorPosition.xyz=p.local+mul(donorPosition.xyz-p.world,p.basis)/(scale*scale);
                donorPosition.w/=scale;
            }
            [loop] for(uint j=0;j<lightCount;j++){
                if(distance(DBL_LIGHT_POSITION(j).xyz,donorPosition.xyz)<.01 &&
                   abs(DBL_LIGHT_POSITION(j).w-donorPosition.w)<.01)addDonor=false;
            }
        }
        uint nativeCount=lightCount;
        lightCount+=addDonor?1:0;
        [loop] for(uint i=0;i<lightCount;i++) {
            CSLight csLight=(CSLight)0;
            float4 lightPosition;float3 incidentColor;
            bool fromDonor=addDonor&&i==nativeCount;
            if(useCS){if(!csLightAt(i,cluster.x,csLight))continue;lightPosition=csLight.positionWS;lightPosition.w=csLight.radius;incidentColor=csLightColor(csLight);}
            else if(fromDonor){lightPosition=donorPosition;incidentColor=max(0,DonorColor.rgb);}
            else {lightPosition=DBL_LIGHT_POSITION(i);incidentColor=max(0,LightColors[i].rgb);}
            // The screen mask has no valid receiver for the borrowed light.
            if(!useCS && !fromDonor && (textureFlags&32) && i<4 && i<LightCounts.y){
                uint channel=(uint)clamp(NativeShadowChannel[i],0,3);
                incidentColor*=SunShadowMask.Load(int3(int2(p.position.xy),0))[channel];
            }
            float3 delta=lightPosition.xyz-lightingPosition;
            float distance=length(delta); float3 direction=delta/max(distance,.0001);
            float attenuation=useCS?csAttenuation(distance,csLight):1-pow(saturate(distance/max(lightPosition.w,.0001)),2);
            float3 color=incidentColor*attenuation;
            illumination+=color*saturate(dot(n,direction));
            specular+=color*pow(saturate(dot(n,normalize(direction+v+1e-6))),AuthoredSpec.w)*gloss*AuthoredSpec.rgb;
            wetSpecular+=color*pow(saturate(dot(n,normalize(direction+v+1e-6))),24);
            volumeLight+=color*(.45+backlightStrength*pow(saturate(dot(-v,direction)),4))*pointLightScattering*(1+VisualLight.y);
            float3 topDelta=lightPosition.xyz-hitPosition;
            float topDistance=length(topDelta);float3 topDirection=topDelta/max(topDistance,.0001);
            float3 topColor=incidentColor*(useCS?csAttenuation(topDistance,csLight):(1-pow(saturate(topDistance/max(lightPosition.w,.0001)),2)));
            topLight+=topColor*saturate(dot(sn,topDirection));
            rippleSpecular+=topColor*pow(saturate(dot(rn,normalize(topDirection+v+1e-6))),48);
            topSpecular+=topColor*pow(saturate(dot(sn,normalize(topDirection+v+1e-6))),48);
        }
        volumeLight=lerp(float3(1,1,1),volumeLight,effectWeight(13));
        illumination=lerp(float3(1,1,1),illumination,effectWeight(13));
        topLight=lerp(float3(1,1,1),topLight,effectWeight(13));
        specular*=effectWeight(13)*effectWeight(11);rippleSpecular*=effectWeight(13);topSpecular*=effectWeight(13);
        liquid*=volumeLight;
        surfaceLight=surface*(liquidTint*.2*topLight+rippleSpecular*.22)*effectWeight(9);
        surfaceLight+=meniscusRim*topSpecular*.16;
        surfaceLight+=foamSignal*(float3(.28,.31,.30)*topLight+topSpecular*.15);
        glassAlpha=saturate(Appearance.x+.35*fresnel);
        glass=GlassColor.rgb*.3*illumination;
        glassHighlight=specular*.35;
        // A restrained coating tint with deliberately broad, strong wet highlights
        // its existing specular response. Keep transmission/refraction untouched.
        float3 wetTint=lerp(float3(1,1,1),saturate(LiquidColor.rgb),wet*lerp(.08,.70,saturate(WetInfo.w)));
        glass*=wetTint;glassHighlight*=wetTint;
        glass+=wetSpecular*(lerp(.9,1.8,saturate(WetInfo.w))*wet);
        wearLighting=illumination;
        if(GlassColor.w>=2) {
            float3 reflected=Environment.SampleLevel(EnvironmentSampler,reflect(-worldView,worldNormal),0).rgb;
            reflectedLight=max(0,reflected)*max(0,EnvironmentData.x*MaterialData.x)*fresnel;
            if(surface>0)surfaceLight+=surface*max(0,Environment.SampleLevel(EnvironmentSampler,reflect(-worldView,worldReflectionNormal),0).rgb)*max(0,EnvironmentData.x*MaterialData.x)*liquidFresnel*(1+.2*meniscusRim)*effectWeight(9);
        }
    }
    // Self-lit liquid is independent of scene lights and is composed behind
    // glass using the existing liquid coverage. It adds no scene light/pass.
    float3 emission=RusticMapping.z>0 ? emissionPattern :
        (EmissionColor.w>0 ? EmissionColor.rgb*emissionPattern : liquidTint);
    liquid+=emission*LiquidEffects.x;
    reflectedLight*=effectWeight(11);
    reflectedLight*=(1-wear*.65)*(1+1.5*wet);
    glass*=effectWeight(17);glassAlpha*=effectWeight(17);reflectedLight*=effectWeight(17);wear*=effectWeight(17);
    // Surface highlights are reflected radiance, independent of shell opacity.
    // Keep authored material response, wear occlusion and the glass feature gate.
    glassHighlight*=effectWeight(17)*(1-wear);
    if(Appearance.y<=.5 && LiquidEffects.x>0)
        glass+=LiquidColor.rgb*(0.20*LiquidEffects.x);
    float oldGlassAlpha=glassAlpha;
    glassAlpha=wear+oldGlassAlpha*(1-wear);
    glass=(wearColor*wearLighting*wear+glass*oldGlassAlpha*(1-wear))/max(glassAlpha,.001);
    // Opaque native vessels retain their original walls; only the interior
    // proxy contributes liquid through an opening. No proxy glass or wear.
    if(Appearance.y>.5){glassAlpha=0;glass=0;reflectedLight=0;glassHighlight=0;}
    // Front glass remains visible over the liquid instead of disappearing as
    // liquid opacity grows. Both terms use straight-alpha output blending.
    // A colored deposit on the INNER wall, behind the original glass detail.
    // Actual contact history gates it: dry glass and submerged pixels get zero.
    float filmAlpha=.65*saturate(WetInfo.w)*wet*effectWeight(17)*(Appearance.y>.5?0:1);
    float3 filmColor=max(0,LiquidColor.rgb)*wearLighting;
    liquidAlpha*=1+VisualSurface.y; surfaceLight*=1+VisualSurface.y;
    glassHighlight*=(1+VisualSurface.x); reflectedLight*=(1+VisualSurface.x);
    glassAlpha*=1+VisualSurface.z; glassHighlight*=1+VisualSurface.z; reflectedLight*=1+VisualSurface.z;
    float underAlpha=filmAlpha+liquidAlpha*(1-filmAlpha);
    float3 underColor=filmColor*filmAlpha+(liquid*liquidAlpha+surfaceLight)*(1-filmAlpha);
    float alpha=glassAlpha+underAlpha*(1-glassAlpha);
    float3 premul=glass*glassAlpha+underColor*(1-glassAlpha)+reflectedLight+glassHighlight;
    // The native body is already in the target. Reserve its front-glass share
    // in the blend and let the following native pass add its reflections.
    if(Appearance.y>.5){
        float transmission=1-Appearance.x;
        premul*=transmission;
        alpha*=transmission;
    }
    // Fog only bottle radiance and its occluded share. Captured scenery already carries fog.
    float3 fogColor=p.fog.rgb;float fogAmount=saturate(p.fog.w);
    if(csLighting()&&CSLinear){fogColor=pow(abs(fogColor),CSFogGamma);fogAmount=pow(fogAmount,CSFogAlphaGamma);}
    premul=lerp(premul,fogColor*alpha,fogAmount);
    float environmentWeight=0;float3 liquidEnvironment=0;
    UseDownsampledScene=(LiquidEffects.z>.5 && lengthWet>0 && detailLOD>0)?LiquidEffects.z:0;
#if DBL_REFRACTION_DIAGNOSTIC
    DiagnosticState=lengthWet>0?1:0;
#endif
    if(Refraction.x>0){
        float3 opticalNormal=normalize(normal+(normal-normalize(p.normal))*TraceSettings.w); float3 viewNormal=mul(p.viewBasis,opticalNormal);
        viewNormal/=max(length(viewNormal),.0001);
        float2 shift=float2(viewNormal.x,-viewNormal.y)*Refraction.w*(1+LiquidEffects.y)*(.4+.6*saturate(thickness));
        // Fallout wet-film optics: coating bend plus its screen-space thickness
        // gradient, sharing the existing scene sample and foreground checks.
        float2 filmGradient=float2(ddx(wet),ddy(wet));
        float2 filmShift=(float2(viewNormal.x,-viewNormal.y)*wet*2.4
            +clamp(filmGradient*24,-1.2,1.2)*wet)*saturate(Refraction.w/4);
        filmShift*=lerp(1,3,saturate(WetInfo.w));
        filmShift*=min(1,lerp(4,12,saturate(WetInfo.w))/max(length(filmShift),.0001));
#ifndef DBL_NO_WET_REFRACTION
        shift+=filmShift;
#endif
        shift*=effectWeight(0);
        float4 currentClip=p.clipOrigin+p.clipX*p.local.x+p.clipY*p.local.y+p.clipZ*p.local.z;
        float2 currentNdc=currentClip.xy/max(abs(currentClip.w),1e-6)*sign(currentClip.w);
        float2 ndcPerPixel=float2(ddx(currentNdc.x),ddy(currentNdc.y));
#ifndef DBL_NO_LIQUID_LENS
        [branch] if(detailLOD>0 && Appearance.w<.5){
        float3 lensTarget,exitPoint,outgoing;
        // Refine the first air-to-liquid crossing, not a far-wall cylinder entry.
        float lo=max(0,wetEntry-wetEntryStep),hi=wetEntry+wetEntryStep;
        bool entryValid=lengthWet>0 && wetEntry<1e29 &&
            liquidBoundary(p.localEye+dir*lo)>=0 && liquidBoundary(p.localEye+dir*hi)<0;
        if(entryValid){int segment=0;float4 endpoints=0;[unroll]for(int j=0;j<7;++j){float mid=(lo+hi)*.5;
            if(cachedLiquidBoundary(p.localEye+dir*mid,segment,endpoints)>0)lo=mid;else hi=mid;}}
        float3 entry=p.localEye+dir*hi;
        bool reflected=false;
        bool valid=entryValid && traceLiquidLens(entry,dir,GlassColor.w>=2?2:0,exitPoint,outgoing,reflected);
#if DBL_REFRACTION_DIAGNOSTIC
        if(lengthWet>0)DiagnosticState=!entryValid?2:!valid?(reflected?4:3):5;
#endif
        float screenWeight=0;
        if(valid){
            float3 forward=normalize(p.viewBasis[2]);
            forward*=dot(forward,dir)<0?-1:1;
            lensTarget=liquidBackgroundTarget(exitPoint,outgoing,forward);
            float4 clip=p.clipOrigin+p.clipX*lensTarget.x+p.clipY*lensTarget.y+p.clipZ*lensTarget.z;
            if(clip.w*currentClip.w>0&&abs(clip.w)>1e-6&&all(abs(ndcPerPixel)>1e-8)){
                float2 lensPixels=(clip.xy/clip.w-currentNdc)/ndcPerPixel;
                float2 projected=p.position.xy+(clip.xy/clip.w-currentNdc)/ndcPerPixel;
                float edge=min(min(projected.x-DBL_EYE_VIEWPORT(p).x,projected.y-DBL_EYE_VIEWPORT(p).y),
                    min(DBL_EYE_VIEWPORT(p).x+DBL_EYE_VIEWPORT(p).z-projected.x,DBL_EYE_VIEWPORT(p).y+DBL_EYE_VIEWPORT(p).w-projected.y));
                screenWeight=smoothstep(0,.2,dot(outgoing,forward));
                if(reflected)screenWeight*=smoothstep(0,8,edge);
#if DBL_REFRACTION_DIAGNOSTIC
                if(lengthWet>0)DiagnosticState=screenWeight>.05&&edge>=0?7:5;
#endif
                float fade=smoothstep(0,.3,lengthWet)*screenWeight;
                // A small fixed pixel cap destroys the lens's negative image
                // slope. Bound pathological projections by viewport extent,
                // leaving normal bottle-scale inversion intact.
                shift+=clamp(lensPixels,-DBL_EYE_VIEWPORT(p).zw*.5,DBL_EYE_VIEWPORT(p).zw*.5)*fade*saturate(Refraction.w/4)*detailLOD;
            }
        }
        if(reflected && GlassColor.w>=2){
            environmentWeight=(1-screenWeight)*smoothstep(0,.3,lengthWet)*saturate(Refraction.w/4);
            float3 worldRay=normalize(mul(p.basis,outgoing));
            liquidEnvironment=max(0,Environment.SampleLevel(EnvironmentSampler,worldRay,0).rgb)
                *max(0,EnvironmentData.x*MaterialData.x);
        }
        }
#endif
        environmentWeight*=detailLOD;
#ifdef DBL_NO_LIQUID_ENVIRONMENT
        environmentWeight=0;
#endif
#ifdef DBL_ENV_PATH_TEST
        return float4(environmentWeight,alpha,0,1);
#endif
        if(TraceSettings.y>0 || TraceSettings.z>0)UseDownsampledScene=dot(shift,shift)>.0001?TraceSettings.z:TraceSettings.y;
        float2 target=clamp(p.position.xy+shift,DBL_EYE_VIEWPORT(p).xy+.5,DBL_EYE_VIEWPORT(p).xy+DBL_EYE_VIEWPORT(p).zw-.5);
        #ifdef DBL_OPTICAL
        float3 tint=lerp(float3(1,1,1),float3(1,.28,.38),1-exp(-lengthWet*.10));
        // Reallocate the same attenuated image weight; do not add a second
        // unbent scene image or modify material opacity/absorption.
        float3 coatingTint=lerp(float3(1,1,1),saturate(LiquidColor.rgb),wet*.65*saturate(WetInfo.w)*effectWeight(17));
        float3 imageWeight=tint*coatingTint*(1-alpha)*effectWeight(15);
        return opticalResult(premul+imageWeight*liquidEnvironment*environmentWeight,
            imageWeight*(1-environmentWeight),target,p.position.z);
#else
        // Reject all four bilinear footprint taps if any belongs to foreground.
        int2 corner=int2(floor(target-.5));bool foreground=false;
        [unroll]for(int y=0;y<2;y++)[unroll]for(int x=0;x<2;x++){
            int2 pixel=clamp(corner+int2(x,y),int2(DBL_EYE_VIEWPORT(p).xy),int2(DBL_EYE_VIEWPORT(p).xy+DBL_EYE_VIEWPORT(p).zw)-1);
            foreground=foreground||(orderedSceneDepth(SceneDepth.Load(int3(pixel,0)))<orderedSceneDepth(p.position.z)-1e-5);
        }
        if(foreground)target=p.position.xy;
#if DBL_REFRACTION_DIAGNOSTIC
        return float4(diagnosticColor(diagnosticFinalState(DiagnosticState,(1-alpha).xxx*(1-environmentWeight),foreground)),1);
#endif
#ifdef DBL_LIVE_COMPOSITE_PROBE
        // Opaque diagnostic only: scenery visible over this region identifies
        // a later engine write, not alpha transmission in this pixel shader.
        if(lengthWet>.15)return foreground?float4(1,0,1,1):float4(0,1,1,1);
#endif
#ifdef DBL_REFRACTION_COORDINATES
        return float4(target/Refraction.yz,lengthWet>.1?1:0,1);
#endif
        float3 transmitted=lerp(sampleRefractionScene(target,Refraction.yz,p.position.z),liquidEnvironment,environmentWeight);
        transmitted*=lerp(float3(1,1,1),float3(1,.28,.38),1-exp(-lengthWet*.10));
        transmitted*=lerp(float3(1,1,1),saturate(LiquidColor.rgb),wet*.65*saturate(WetInfo.w)*effectWeight(17));
        return float4(premul+transmitted*(1-alpha),1);
#endif
    }
#ifdef DBL_OPTICAL
    return opticalResult(premul,(1-alpha).xxx*effectWeight(15)*lerp(float3(1,1,1),saturate(LiquidColor.rgb),wet*.65*saturate(WetInfo.w)*effectWeight(17)),p.position.xy,p.position.z);
#else
#if DBL_REFRACTION_DIAGNOSTIC
    return float4(diagnosticColor(DiagnosticState),1);
#endif
    return float4(premul/max(alpha,.001),alpha);
#endif
}
#ifdef DBL_NATIVE_GLASS
#undef PSMain
struct NativeLiquidOutput {float4 color:SV_TARGET;float depth:SV_Depth;};
NativeLiquidOutput PSMain(Pixel p){NativeLiquidOutput o;o.color=PSNativeLiquidColor(p);o.depth=NativeLiquidDepth;return o;}
#endif
#endif


