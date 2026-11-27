// Project a bounded, light-attached transmission footprint onto opaque scene depth.
// Trace the tapered radial sections and apply Beer–Lambert attenuation.
cbuffer NativeGeometry : register(b2) { row_major float3x4 NativeWorld; };
#ifdef PROJECTION_MASK
cbuffer RelativeCasterGeometry : register(b9) { row_major float3x4 RelativeCaster; float4 ReceiverLightOffsetRadius; };
#endif
cbuffer NativeFrame : register(b12) { row_major float4x4 ViewProj : packoffset(c8); };
cbuffer BodyLights : register(b0) {
    float4 BodyChannels : packoffset(c2);
    float4 Positions[7] : packoffset(c15);
    float4 Colors[7] : packoffset(c22);
    float2 Counts : packoffset(c29);
};
cbuffer DonorLights : register(b7) {
    float4 DonorChannels : packoffset(c2);
    float4 DonorPositions[4] : packoffset(c15);
    float4 DonorColors[4] : packoffset(c22);
    float2 DonorCounts : packoffset(c29);
};
cbuffer DonorGeometry : register(b8) { row_major float3x4 DonorWorld; };
cbuffer Projection : register(b10) {
    float4 FillPlane;
    float4 Body;        // maximum outer radius XY, axial half-height, local center Z
    float4 Absorption;  // liquid RGB extinction per local unit
    float4 Controls;    // glass extinction, shadow strength, caustic strength, projection distance
    float4 Screen;      // viewport width, height, min depth, max depth
    float4 Lighting;    // body lights are local, valid receiver mask, allow unshadowed, donor lights are local
    float4 Shape;       // ring count, glass wall thickness, focus height, opaque glass
    float4 Rings[16];   // local height, cavity radius
};
Texture2D<float> ReceiverDepth : register(t0);
Texture2D<float> CurrentDepth : register(t1);
Texture2D<float4> ReceiverShadow : register(t2);

float3x4 casterWorld() {
#ifdef PROJECTION_MASK
    float3x4 world=RelativeCaster;
    world[0].w+=NativeWorld[0].w;
    world[1].w+=NativeWorld[1].w;
    world[2].w+=NativeWorld[2].w;
    return world;
#else
    return NativeWorld;
#endif
}
float3 toWorld(float3 position) { return mul(casterWorld(),float4(position,1)); }
float scaleSquared() { float3 row=casterWorld()[0].xyz; return dot(row,row); }
float3 toLocal(float3 position) {
    float3x4 world=casterWorld();
    float3 origin=float3(world[0].w,world[1].w,world[2].w);
    return mul(position-origin,(float3x3)world)/max(scaleSquared(),1e-8);
}

// The inverse is evaluated once per rectangle vertex. Reconstruct the actual
// receiving position from D3D depth in the pixel shader.
float4 cofactors(float4 a,float4 b,float4 c) {
    return float4(dot(a.yzw,cross(b.yzw,c.yzw)),-dot(a.xzw,cross(b.xzw,c.xzw)),
                  dot(a.xyw,cross(b.xyw,c.xyw)),-dot(a.xyz,cross(b.xyz,c.xyz)));
}

struct ProjectionVertex {
    float4 position : SV_Position;
    nointerpolation float4 inverse0 : TEXCOORD0;
    nointerpolation float4 inverse1 : TEXCOORD1;
    nointerpolation float4 inverse2 : TEXCOORD2;
    nointerpolation float4 inverse3 : TEXCOORD3;
    nointerpolation float4 light : TEXCOORD4;
    nointerpolation float4 colorAndChannel : TEXCOORD5;
};

void considerLight(float4 nativePosition,float3 color,int channel,bool local,
                   inout float best,inout float4 light,inout float4 colorAndChannel) {
    if(!all(isfinite(nativePosition))||!all(isfinite(color))||nativePosition.w<=0)return;
    if(channel>=0&&Lighting.y==0)channel=-1;
    if(channel<0&&Lighting.z==0)return;
    float scale=sqrt(max(scaleSquared(),1e-8));
    float3 position=local?nativePosition.xyz:toLocal(nativePosition.xyz);
    float radius=local?nativePosition.w:nativePosition.w/scale;
    float distanceFromBottle=distance(position,float3(0,0,Body.w));
    if(distanceFromBottle>=radius||distanceFromBottle<=max(Body.x,Body.y)*.6)return;
    color=max(0,color);
    float score=dot(color,float3(.2126,.7152,.0722))*(1-saturate(pow(distanceFromBottle/radius,2)));
    // For the cast-shadow trial prefer a light with a verified receiver mask.
    if(Controls.y>0&&channel>=0)score+=1e4;
    if(score<=best)return;
    best=score;
    light=float4(position,radius);
    colorAndChannel=float4(color,channel);
}

#ifdef PROJECTION_MASK
// Light Limit Fix moves shadowed points out of the vanilla per-geometry arrays.
struct ReceiverClusterLight {
    float4 colorFade, radiusData, positionWS;
    uint4 roomFlags, metadata;
};
StructuredBuffer<ReceiverClusterLight> ReceiverClusterLights : register(t35);
cbuffer ReceiverStrict : register(b3){uint4 ReceiverHeader;};
bool clusteredReceiver(inout float best,inout ProjectionVertex output){
    if(ReceiverLightOffsetRadius.w<=0 || !(ReceiverHeader.z&15))return false;
    float3 expected=ReceiverLightOffsetRadius.xyz+float3(NativeWorld[0].w,NativeWorld[1].w,NativeWorld[2].w);
    uint count,stride;ReceiverClusterLights.GetDimensions(count,stride);
    if(stride!=80)return false;
    [loop]for(uint i=0;i<count;++i){
        ReceiverClusterLight light=ReceiverClusterLights[i];
        uint flags=light.metadata.x,channel=light.metadata.y;
        // Only the screen-space mask path consumes the patched RGBA texture.
        if((flags&(2|16|512))!=2 || channel>=4 || !(ReceiverHeader.z&(1u<<channel)))continue;
        if(abs(light.radiusData.x-ReceiverLightOffsetRadius.w)>.25 || distance(light.positionWS.xyz,expected)>.5)continue;
        considerLight(float4(light.positionWS.xyz,light.radiusData.x),light.colorFade.rgb,channel,false,best,output.light,output.colorAndChannel);
        return best>0;
    }
    return false;
}
#endif
ProjectionVertex VS(uint vertex:SV_VertexID) {
    ProjectionVertex output=(ProjectionVertex)0;
    output.position=float4(-2,-2,0,1);
    float best=0;
#ifdef PROJECTION_MASK
    if(!clusteredReceiver(best,output)){
#endif
    [loop]for(int i=0;i<min((int)Counts.x,7);++i){
        int channel=i<4&&i<(int)Counts.y?(int)BodyChannels[i]:-1;
        if(channel>3)continue;
        considerLight(Positions[i],Colors[i].rgb,channel,Lighting.x>0,best,output.light,output.colorAndChannel);
    }
    [loop]for(int j=0;j<min((int)min(DonorCounts.x,DonorCounts.y),4);++j){
        int channel=(int)DonorChannels[j];
        if(channel<0||channel>3)continue;
        float4 position=DonorPositions[j];
        if(Lighting.w>0){
            position.xyz=mul(DonorWorld,float4(position.xyz,1));
            position.w*=length(DonorWorld[0].xyz);
        }
        considerLight(position,DonorColors[j].rgb,channel,false,best,output.light,output.colorAndChannel);
    }
#ifdef PROJECTION_MASK
    }
#endif
    if(best<=0)return output;

    float4 a=ViewProj[0],b=ViewProj[1],c=ViewProj[2],d=ViewProj[3];
    float4 inv0=cofactors(b,c,d);
    float det=dot(a,inv0);
    if(!isfinite(det)||abs(det)<1e-8)return output;
    float4x4 inverse=transpose(float4x4(inv0,-cofactors(a,c,d),cofactors(a,b,d),-cofactors(a,b,c)))/det;
    output.inverse0=inverse[0];output.inverse1=inverse[1];
    output.inverse2=inverse[2];output.inverse3=inverse[3];

    // Bound the caster and its short projection cone in screen space.
    float3 center=float3(0,0,Body.w);
    float radius=length(Body.xz);
    float3 lightToCenter=center-output.light.xyz;
    float distanceFromLight=length(lightToCenter);
#ifdef PROJECTION_MASK
    float reach=max(0,output.light.w-distanceFromLight)+2*radius;
#else
    float reach=Controls.w+radius;
#endif
    float farRadius=radius*(1+reach/max(distanceFromLight-radius,radius*.25));
    float3 farCenter=center+lightToCenter/distanceFromLight*reach;
    float2 lo=1,hi=-1;
    bool crossesNear=false;
    [unroll]for(int cornerId=0;cornerId<8;++cornerId){
        float3 corner=float3(cornerId&1?1:-1,cornerId&2?1:-1,cornerId&4?1:-1);
        float4 nearClip=mul(ViewProj,float4(toWorld(center+radius*corner),1));
        float4 farClip=mul(ViewProj,float4(toWorld(farCenter+farRadius*corner),1));
        crossesNear=crossesNear||min(nearClip.w,farClip.w)<=.01;
        float2 nearNdc=nearClip.xy/max(nearClip.w,.01),farNdc=farClip.xy/max(farClip.w,.01);
        lo=min(lo,min(nearNdc,farNdc));
        hi=max(hi,max(nearNdc,farNdc));
    }
    lo=crossesNear?-1:clamp(lo,-1,1);
    hi=crossesNear?1:clamp(hi,-1,1);
    output.position=float4(vertex&1?hi.x:lo.x,vertex&2?hi.y:lo.y,0,1);
    return output;
}

float intervalLength(float2 interval) { return max(0,interval.y-interval.x); }

float2 sectionInterval(float3 light,float3 dir,float2 lower,float2 upper,float2 bounds) {
    // Clip to the axial slab, then intersect the ray with its linear radius.
    float2 t=bounds;
    if(abs(dir.z)>1e-6){
        float2 caps=(float2(lower.x,upper.x)-light.z)/dir.z;
        t=float2(max(t.x,min(caps.x,caps.y)),min(t.y,max(caps.x,caps.y)));
    }else if(light.z<lower.x||light.z>=upper.x)return float2(1,0);
    if(t.y<=t.x)return float2(1,0);

    float3 start=light+dir*t.x;
    float slope=(upper.y-lower.y)/(upper.x-lower.x);
    float radius=lower.y+slope*(start.z-lower.x);
    float rate=slope*dir.z;
    float a=dot(dir.xy,dir.xy)-rate*rate;
    float b=dot(start.xy,dir.xy)-radius*rate;
    float c=dot(start.xy,start.xy)-radius*radius;
    if(abs(a)<1e-6){
        if(abs(b)<1e-6)return c<=0?t:float2(1,0);
        float crossing=t.x-c/(2*b);
        if(b>0)t.y=min(t.y,crossing);
        else t.x=max(t.x,crossing);
        return t;
    }
    float d=b*b-a*c;
    if(d<=0)return a<0?t:float2(1,0);
    float q=-b-(b>=0?1:-1)*sqrt(d);
    float2 roots=t.x+float2(q/a,c/q);
    float2 ordered=float2(min(roots.x,roots.y),max(roots.x,roots.y));
    if(a>0)return float2(max(t.x,ordered.x),min(t.y,ordered.y));
    // Only one branch intersects a slab where the radius stays positive.
    float2 nearPart=float2(t.x,min(t.y,ordered.x));
    float2 farPart=float2(max(t.x,ordered.y),t.y);
    return intervalLength(nearPart)>intervalLength(farPart)?nearPart:farPart;
}

float wetLength(float2 inside,float3 light,float3 dir) {
    float height=FillPlane.w-dot(FillPlane.xyz,light);
    float slope=dot(FillPlane.xyz,dir);
    if(abs(slope)<1e-5)return height>=0?intervalLength(inside):0;
    float intersection=height/slope;
    if(slope>0)inside.y=min(inside.y,intersection);
    else inside.x=max(inside.x,intersection);
    return intervalLength(inside);
}

// Companion tile uses stereographic coordinates on one light hemisphere.
cbuffer TileProjection : register(b11) {
    float4 TileLight, TileX, TileY, TileZ, TileRect, TileSize;
};
Texture2D<float4> OpticalTile : register(t3);
SamplerState TileFilter : register(s0);

float4 tracePaths(float3 light,float3 direction,float2 bounds,out float outerLength) {
    outerLength=0;
    float innerLength=0,liquidLength=0,lastExit=0;
    int ringCount=clamp((int)Shape.x,2,16);
    [loop]for(int i=1;i<ringCount;++i){
        float2 lower=Rings[i-1].xy,upper=Rings[i].xy;
        if(upper.x<=lower.x)continue;
        float2 outerLower=lower+float2(i==1?-Shape.y:0,Shape.y);
        float2 outerUpper=upper+float2(i==ringCount-1?Shape.y:0,Shape.y);
        float2 outer=sectionInterval(light,direction,outerLower,outerUpper,bounds);
        float span=intervalLength(outer);
        if(span<=0)continue;
        outerLength+=span;
        lastExit=max(lastExit,outer.y);
        float2 inner=sectionInterval(light,direction,lower,upper,bounds);
        innerLength+=intervalLength(inner);
        liquidLength+=wetLength(inner,light,direction);
    }
    return float4(max(0,outerLength-innerLength),liquidLength,lastExit,outerLength>0);
}
float4 TileVS(uint id:SV_VertexID):SV_Position {
    return float4(id&1?1:-1,id&2?-1:1,0,1);
}
float4 TilePS(float4 p:SV_Position):SV_Target {
    float2 q=TileRect.xy+p.xy/TileSize.xy/TileRect.zw;
    float square=dot(q,q);
    float3 dir=(2*q.x*TileX.xyz+2*q.y*TileY.xyz+(1-square)*TileZ.xyz)/(1+square);
    float3 delta=TileLight.xyz-float3(0,0,Body.w);
    float mid=-dot(delta,dir);
    float d=mid*mid+dot(Body.xz,Body.xz)-dot(delta,delta);
    if(d<=0)return 0;
    float extent=sqrt(d);
    float outerLength;
    return tracePaths(TileLight.xyz,dir,float2(max(0,mid-extent),mid+extent),outerLength);
}

Texture2D<float4> CausticTile : register(t4);
float lensFocus(float3 position,float3 light){
        float3 lensCenter=float3(0,0,Shape.z);
        float3 axis=normalize(lensCenter-light);
        float3 tangent=normalize(cross(abs(axis.z)>.95?float3(0,1,0):float3(0,0,1),axis));
        float3 bitangent=cross(axis,tangent);
        float3 offset=position-lensCenter;
        float2 footprint=float2(dot(offset,tangent),dot(offset,bitangent))/max(Body.x,.01);
        float tilt=dot(FillPlane.xyz,tangent);
        footprint.x-=.32+.18*tilt;
        float2 scaled=footprint*float2(3,1.55);
        float2 filterWidth=max(fwidth(scaled),.12);
        float2 filtered=scaled/(1+filterWidth);
        return exp(-dot(filtered,filtered))/((1+filterWidth.x)*(1+filterWidth.y));
 }
float4 CausticTilePS(float4 p:SV_Position):SV_Target {
    float2 uv=p.xy*2/TileSize.xy;
    float4 paths=OpticalTile.SampleLevel(TileFilter,uv,1);
    if(paths.w<=0)return 0;
    paths.xyz/=paths.w;
    float2 q=TileRect.xy+uv/TileRect.zw;
    float square=dot(q,q);
    float3 dir=(2*q.x*TileX.xyz+2*q.y*TileY.xyz+(1-square)*TileZ.xyz)/(1+square);
    // Focus on a plane just behind the lens. Distance falloff and receiver
    // visibility remain live so foregrounds and moving surfaces are respected.
    float3 focusPoint=TileLight.xyz+dir*(paths.z+Body.x);
    float wet=saturate(paths.y/max(.01,Body.x*.28));
    return float4(paths.w*wet*lensFocus(focusPoint,TileLight.xyz)*exp(-.62*(paths.x*Controls.x+paths.y*Absorption.rgb)),0);
}

struct ProjectionColor {
    float4 focusedLight : SV_Target0;
    float4 transmission : SV_Target1;
};

#ifndef PROJECTION_MASK
float receiverWeight(ProjectionVertex input,int2 pixel,float distanceFromLight,float projectionDistance,float cosine){
    float visibility=input.colorAndChannel.w>=0?
        ReceiverShadow.Load(int3(pixel,0))[(int)input.colorAndChannel.w]:1;
    float attenuation=1-pow(saturate(distanceFromLight/input.light.w),2);
    float intensity=saturate(dot(input.colorAndChannel.rgb,float3(.2126,.7152,.0722)));
    float fadeStart=Absorption.w>0?Controls.w-Absorption.w:Controls.w*.55;
    float fade=1-smoothstep(fadeStart,Controls.w,projectionDistance);
    return visibility*attenuation*intensity*cosine*fade;
}
#endif

#ifdef PROJECTION_MASK
float blockedLight(float4 paths,float receiverDistance){
    if(paths.w<=0||receiverDistance<=paths.z/paths.w+.1)return 0;
    if(Shape.w>0)return paths.w;
    float3 extinction=(paths.x*Controls.x+paths.y*Absorption.rgb)/paths.w;
    return paths.w*(1-dot(exp(-extinction),float3(.2126,.7152,.0722)));
}

float filteredBlockage(float3 light,float3 direction,float receiverDistance){
    float blocked=0;
    static const float2 corners[4]={float2(-1,-1),float2(1,-1),float2(-1,1),float2(1,1)};
    if(TileSize.x>0&&distance(TileLight.xyz,light)<max(.02,Body.x*.005)){
        float3 v=float3(dot(direction,TileX.xyz),dot(direction,TileY.xyz),dot(direction,TileZ.xyz));
        if(v.z<=-.99)return 0;
        float2 uv=(v.xy/(1+v.z)-TileRect.xy)*TileRect.zw;
        if(any(uv<0)||any(uv>1))return 0;
        // A two-texel guard surrounds the optical tile. Four bilinear taps
        // cover a neighborhood at the native point-light atlas density.
        float2 stepUV=1.15/TileSize.xy;
        [unroll]for(int i=0;i<4;++i)
            blocked+=blockedLight(OpticalTile.SampleLevel(TileFilter,uv+corners[i]*stepUV,0),receiverDistance);
    }else{
        // Without an atlas tile, integrate the analytic silhouette over the receiver pixel.
        float3 dx=ddx(direction)*.375,dy=ddy(direction)*.375;
        float3 origin=light-float3(0,0,Body.w);
        float bound=dot(Body.xz,Body.xz)-dot(origin,origin);
        [loop]for(int i=0;i<4;++i){
            float3 ray=normalize(direction+corners[i].x*dx+corners[i].y*dy);
            float mid=-dot(origin,ray),delta=mid*mid+bound;
            if(delta<=0)continue;
            float radius=sqrt(delta);
            float2 bounds=float2(max(0,mid-radius),mid+radius);
            if(bounds.y<=bounds.x||bounds.x>=receiverDistance)continue;
            float lengthThroughBottle;
            blocked+=blockedLight(tracePaths(light,ray,bounds,lengthThroughBottle),receiverDistance);
        }
    }
    return blocked*.25;
}
#endif

// The audit replay writes one outcome per covered pixel to a single-sample R8_UINT target.
#ifdef MASK_FRAGMENT_AUDIT
#define MASK_REJECT(reason) return reason
uint PS(ProjectionVertex input) : SV_Target {
#elif defined(PROJECTION_MASK)
#define MASK_REJECT(reason) discard
float4 PS(ProjectionVertex input) : SV_Target {
#else
ProjectionColor PS(ProjectionVertex input) {
#endif
    int2 pixel=int2(input.position.xy);
    float depth=ReceiverDepth.Load(int3(pixel,0));
    float2 ndc=input.position.xy/Screen.xy*float2(2,-2)+float2(-1,1);
    float4 clip=float4(ndc,(depth-Screen.z)/(Screen.w-Screen.z),1);
    float4 p=float4(dot(input.inverse0,clip),dot(input.inverse1,clip),dot(input.inverse2,clip),dot(input.inverse3,clip));
    float3 receiver=toLocal(p.xyz/max(p.w,1e-8));
#ifdef PROJECTION_MASK
    if(depth>=Screen.w){MASK_REJECT(1);}
#ifdef MASK_FRAGMENT_AUDIT
    if(abs(CurrentDepth.Load(int3(pixel,0))-depth)>2e-6){MASK_REJECT(2);}
#endif
    if(p.w<=0){MASK_REJECT(3);}
#else
    if(depth>=Screen.w||abs(CurrentDepth.Load(int3(pixel,0))-depth)>2e-6||p.w<=0)discard;
#endif
#ifndef PROJECTION_MASK
    float3 normal=cross(ddx(receiver),ddy(receiver));
    float normalLength=length(normal);
    if(normalLength<=1e-8||normalLength>Body.x*Body.x)discard;
    normal/=normalLength;
    float3 view=toLocal(0)-receiver;
    if(dot(normal,view)<0)normal=-normal;
#endif

    float3 light=input.light.xyz;
    float3 ray=receiver-light;
    float distanceFromLight=length(ray);
#ifdef PROJECTION_MASK
    if(distanceFromLight<=0){MASK_REJECT(4);}
    if(distanceFromLight>=input.light.w){MASK_REJECT(5);}
#else
    if(distanceFromLight<=0||distanceFromLight>=input.light.w)discard;
#endif
    float3 direction=ray/distanceFromLight;
#ifndef PROJECTION_MASK
    float cosine=saturate(dot(normal,-direction));
    if(cosine<=0)discard;
#endif

#ifdef PROJECTION_MASK
    int channel=(int)input.colorAndChannel.w;
    if(channel<0||channel>3){MASK_REJECT(6);}
    float blocked=filteredBlockage(light,direction,distanceFromLight);
    if(blocked<=0){MASK_REJECT(7);}
#ifdef MASK_FRAGMENT_AUDIT
    return 8+channel;
#else
    return 1-blocked*(float4)(int4(0,1,2,3)==channel);
#endif
#else
    float3 center=float3(0,0,Body.w);
    float3 origin=light-center;
    float b=dot(origin,direction);
    float boundSquared=dot(Body.xz,Body.xz);
    float delta=b*b+boundSquared-dot(origin,origin);
    if(delta<=0)discard;
    float reach=sqrt(delta);
    float2 bounds=float2(max(0,-b-reach),-b+reach);
    if(bounds.y<=bounds.x||bounds.x>=distanceFromLight)discard;
    if(bounds.y<distanceFromLight-Controls.w)discard;
    float2 radialStart=light.xy+direction.xy*bounds.x;
    float nearest=clamp(-dot(radialStart,direction.xy)/max(dot(direction.xy,direction.xy),1e-8),
                        0,bounds.y-bounds.x);
    float2 radialNearest=radialStart+direction.xy*nearest;
    if(dot(radialNearest,radialNearest)>Body.x*Body.x)discard;
    float4 paths;
    bool cacheHit=TileSize.x>0 && distance(TileLight.xyz,light)<max(.02,Body.x*.005);
    float2 cacheUV=0;
    float outerLength=0;
    if(cacheHit){
        float3 v=float3(dot(direction,TileX.xyz),dot(direction,TileY.xyz),dot(direction,TileZ.xyz));
        if(v.z<=-.99)discard;
        float2 uv=(v.xy/(1+v.z)-TileRect.xy)*TileRect.zw;
        cacheUV=uv;
        if(any(uv<0)||any(uv>1))discard;
        const float mip=1;
        // For an additive caustic the focus tile already includes extinction.
        // Empty focus pixels need neither the optical tile nor the live shadow mask.
        [branch]if(Controls.y==0){
            float3 focus=CausticTile.SampleLevel(TileFilter,uv,0).rgb;
            if(!any(focus>0))discard;
            float4 optical=OpticalTile.SampleLevel(TileFilter,uv,1);
            if(optical.w<=0||optical.x+optical.y<=0)discard;
            float gap=distanceFromLight-optical.z/optical.w;
            if(gap<=.1||gap>Controls.w)discard;
            float weight=receiverWeight(input,pixel,distanceFromLight,gap,cosine);
            ProjectionColor result;
            result.focusedLight=float4(focus*Controls.z*weight*min(input.colorAndChannel.rgb,2)*.42,0);
            result.transmission=1;
            return result;
        }
        paths=OpticalTile.SampleLevel(TileFilter,uv,mip);
        if(paths.w<=0)discard;
        // Distances are coverage weighted so clear border texels cannot pull
        // the exit forward and shadow geometry in front of the bottle.
        paths.xyz/=paths.w;
    }else{
        paths=tracePaths(light,direction,bounds,outerLength);
    }
    float glassLength=paths.x,liquidLength=paths.y,lastExit=paths.z;
    if(cacheHit)outerLength=glassLength+liquidLength;
    float projectionDistance=distanceFromLight-lastExit;
    if(outerLength<=0||projectionDistance<=.1)discard;
    if(projectionDistance>Controls.w)discard;

    float3 opticalDepth=glassLength*Controls.x+liquidLength*Absorption.rgb;
    float3 transmission=exp(-opticalDepth);
    float weight=receiverWeight(input,pixel,distanceFromLight,projectionDistance,cosine);
    float coverage=paths.w*smoothstep(0,1,outerLength/max(Body.x*.18,.01));

    float3 caustic=0;
    [branch]if(cacheHit){
        caustic=CausticTile.SampleLevel(TileFilter,cacheUV,0).rgb*Controls.z*weight*min(input.colorAndChannel.rgb,2)*.42;
    }else if(Controls.z>0){
        // Filtered Gaussian focus in light space, accepting a shorter optical
        // path near the edge of the lens. An empty bottle gives no wet focus.
        float focus=lensFocus(receiver,light);
        float wet=saturate(liquidLength/(max(Body.x,.01)*.28));
        caustic=Controls.z*weight*coverage*focus*wet*exp(-opticalDepth*.62)*
                min(input.colorAndChannel.rgb,2)*.42;
    }
    float3 shadow=1-Controls.y*weight*coverage*(1-transmission);
    ProjectionColor result;
    result.focusedLight=float4(caustic,0);
    result.transmission=float4(shadow,1);
    return result;
#endif
}

