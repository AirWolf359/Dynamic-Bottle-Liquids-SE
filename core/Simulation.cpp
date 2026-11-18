#include "Simulation.h"
#include "BubbleFlow.h"
#include "BubbleTrigger.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
namespace nuka {
float Dot(Vec a,Vec b){return a.x*b.x+a.y*b.y+a.z*b.z;}
float Length(Vec a){return std::sqrt(Dot(a,a));}
Vec Normalize(Vec a){return a*(1/std::max(Length(a),0.00001f));}
static float Radius(const Constants& c,float z){
    for(int i=1;i<c.ringCount;i++)if(z>=c.rings[i-1].x&&z<=c.rings[i].x){auto a=c.rings[i-1],b=c.rings[i];return std::lerp(a.y,b.y,(z-a.x)/(b.x-a.x));}return 0;
}
// A hint only avoids searching an already known segment. At a shared endpoint
// use the original first matching segment and the original lerp arithmetic.
static float RadiusHinted(const Constants& c,float z,int& hint){
    if(hint>0&&hint<c.ringCount){auto a=c.rings[hint-1],b=c.rings[hint];
        if((z>a.x||(hint==1&&z==a.x))&&z<=b.x)return std::lerp(a.y,b.y,(z-a.x)/(b.x-a.x));}
    for(int i=1;i<c.ringCount;++i)if(z>=c.rings[i-1].x&&z<=c.rings[i].x){hint=i;auto a=c.rings[i-1],b=c.rings[i];return std::lerp(a.y,b.y,(z-a.x)/(b.x-a.x));}
    return 0;
}
static bool SurfacePairSeparated(float squaredDistance,float target){
    // Conservative squared comparison: values near contact still use the exact
    // original square root. The margin covers rounding of target*target.
    return squaredDistance>target*target*(1.f+4*std::numeric_limits<float>::epsilon());
}
#ifdef DBL_SURFACE_REFERENCE
bool ValidateSurfaceShortcuts(const Constants& c){
    std::vector<float> zs;
    for(int i=0;i<c.ringCount;++i){float z=c.rings[i].x;zs.push_back(z);zs.push_back(std::nextafter(z,-std::numeric_limits<float>::infinity()));zs.push_back(std::nextafter(z,std::numeric_limits<float>::infinity()));}
    zs.push_back(-std::numeric_limits<float>::infinity());zs.push_back(std::numeric_limits<float>::infinity());zs.push_back(std::numeric_limits<float>::quiet_NaN());
    for(int initial=1;initial<c.ringCount;++initial)for(float z:zs){int hint=initial;float a=Radius(c,z),b=RadiusHinted(c,z,hint);if(std::memcmp(&a,&b,sizeof(float)))return false;}
    for(int i=1;i<10000;++i){float target=i*.00037f;float d=target*target;
        for(int j=0;j<12;++j)d=std::nextafter(d,0.f);
        for(int j=0;j<24;++j){if(SurfacePairSeparated(d,target)&&!(std::sqrt(d)>=target))return false;d=std::nextafter(d,std::numeric_limits<float>::infinity());}
    }
    return true;
}
#endif
static float CrossSection(const Constants& c,float x,float y){
    if(c.cavityPlanes[0].w<3)return std::hypot(x,y);
    float support=0;for(int i=0;i<int(c.cavityPlanes[0].w);++i)
        support=std::max(support,x*c.cavityPlanes[i].x+y*c.cavityPlanes[i].y);
    return support;
}
static Vec SectionNormal(const Constants& c,float x,float y){
    if(c.cavityPlanes[0].w<3)return Normalize({x,y,0});
    float best=-1e30f;Vec normal{1,0,0};
    for(int i=0;i<int(c.cavityPlanes[0].w);++i){auto plane=c.cavityPlanes[i];float d=x*plane.x+y*plane.y;if(d>best){best=d;normal={plane.x,plane.y,0};}}
    return Normalize(normal);
}
static float AngularScale(const Constants& c,float a){return 1/std::max(CrossSection(c,std::cos(a),std::sin(a)),.001f);}
static V4 MeniscusGeometry(const Constants& c,Vec p){
    float bottom=c.rings[0].x,top=c.rings[c.ringCount-1].x;
    float r=Radius(c,std::clamp(p.z,bottom,top));
    float wall=std::min(r-CrossSection(c,p.x,p.y),std::min(p.z-bottom,top-p.z));
    return {wall,r,0,0};
}
static float MeniscusFromGeometry(const Constants& c,V4 g){
    float width=.35f+std::min(.35f*c.viscousSurface.y,g.y*.15f),height=.06f+std::min(.06f*c.viscousSurface.x,g.y*.03f);
    float t=std::clamp(1.f-g.x/width,0.f,1.f);
    return height*t*t*t;
}
#ifdef DBL_MENISCUS_REFERENCE
bool UseReferenceMeniscus=false;
#endif
void MakeSamples(Settings& s){
    ++s.sampleRevision;s.tunedSampleCache.clear();
    float slope=0,radial=1;
    for(int i=1;i<s.base.ringCount;++i)slope=std::max(slope,std::abs(s.base.rings[i].y-s.base.rings[i-1].y)/std::max(s.base.rings[i].x-s.base.rings[i-1].x,.0001f));
    for(int i=0;i<int(s.base.cavityPlanes[0].w);++i)radial=std::max(radial,std::hypot(s.base.cavityPlanes[i].x,s.base.cavityPlanes[i].y));
    s.base.tunedInfo.z=std::max(1.f,std::hypot(radial,slope))*1.0001f;

    s.wetSamples.clear();s.wetPhases.clear();s.wetRingCount=s.base.ringCount;s.wetFrequency=s.waveFrequency;
    std::copy_n(s.base.rings,16,s.wetRings.begin());
    float bottom=s.base.rings[0].x,span=std::max(s.base.rings[s.base.ringCount-1].x-bottom,.001f);
    for(int row=0;row<16;row++){
        float z=bottom+span*row/15,rad=Radius(s.base,z)*.98f;
        for(int k=0;k<16;k++){
            float a=k*6.283185307f/16;float shaped=rad*AngularScale(s.base,a);Vec q{std::cos(a)*shaped,std::sin(a)*shaped,z};
            s.wetSamples.push_back({q.x,q.y,q.z,MeniscusHeight(s.base,q)});
            s.wetPhases.push_back({(.8f*q.x+.6f*q.y)*s.waveFrequency,(-.48f*q.x+.64f*q.y+.6f*q.z)*s.waveFrequency*1.73f,(.36f*q.x-.48f*q.y+.8f*q.z)*s.waveFrequency*2.35f,0});
        }
    }
    s.samples.clear();s.sampleWaveBasis.clear();s.sampleMediumBasis.clear();s.sampleWaveFrequency=s.waveFrequency;const auto& c=s.base;float dz=(c.rings[c.ringCount-1].x-c.rings[0].x)/64;
    for(int k=0;k<64;k++)for(int j=0;j<5;j++)for(int i=0;i<16;i++){
        // Stratify heights within each slice so an upright plane does not jump by an entire slice's volume.
        float z=c.rings[0].x+(k+(((j*16+i)*37%80)+.5f)/80)*dz;
        float R=Radius(c,z),w=3.14159265f*R*R*dz/80;
        float r=R*std::sqrt((j+.5f)/5),a=6.2831853f*(i+.5f+(k%2)*.25f)/16;float shape=AngularScale(c,a);s.samples.push_back({r*shape*std::cos(a),r*shape*std::sin(a),z,w*shape*shape});}
    s.sampleWeightTotal=0;s.sampleWeightCount=s.samples.size();for(auto p:s.samples)s.sampleWeightTotal+=p.w;
    s.sampleWaveBasis.reserve(s.samples.size());s.sampleMediumBasis.reserve(s.samples.size());
    s.sampleMeniscus.clear();s.sampleMeniscus.reserve(s.samples.size());
    s.sampleMeniscusGeometry.clear();s.sampleMeniscusGeometry.reserve(s.samples.size());
    s.wetMeniscusGeometry.clear();s.wetMeniscusGeometry.reserve(s.wetSamples.size());
    s.meniscusRingCount=c.ringCount;
    std::copy_n(c.rings,16,s.meniscusRings.begin());std::copy_n(c.cavityPlanes,16,s.meniscusPlanes.begin());
    for(auto p:s.wetSamples)s.wetMeniscusGeometry.push_back(MeniscusGeometry(c,{p.x,p.y,p.z}));
    for(auto p:s.samples){float x=(.8f*p.x+.6f*p.y)*s.waveFrequency,y=(-.48f*p.x+.64f*p.y+.6f*p.z)*s.waveFrequency*1.73f;
        s.sampleMeniscus.push_back(MeniscusHeight(c,{p.x,p.y,p.z}));
        s.sampleMeniscusGeometry.push_back(MeniscusGeometry(c,{p.x,p.y,p.z}));
        s.sampleWaveBasis.push_back({std::sin(x),std::cos(x),std::sin(y),std::cos(y)});
        float z=(.36f*p.x-.48f*p.y+.8f*p.z)*s.waveFrequency*2.35f;
        s.sampleMediumBasis.push_back({std::sin(z),std::cos(z),0,0});
    }
}
float MeniscusHeight(const Constants& c,Vec p){
    float bottom=c.rings[0].x,top=c.rings[c.ringCount-1].x;
    float wall=std::min(Radius(c,std::clamp(p.z,bottom,top))-CrossSection(c,p.x,p.y),std::min(p.z-bottom,top-p.z));
    // Match LiquidCore.inc; cached samples retain volume conservation.
    float r=Radius(c,std::clamp(p.z,bottom,top));
    float width=.35f+std::min(.35f*c.viscousSurface.y,r*.15f),height=.06f+std::min(.06f*c.viscousSurface.x,r*.03f);
    float t=std::clamp(1.f-wall/width,0.f,1.f);
    return height*t*t*t;
}
static float MeniscusHeightHinted(const Constants& c,Vec p,int& hint){
    float bottom=c.rings[0].x,top=c.rings[c.ringCount-1].x;
    float wall=std::min(RadiusHinted(c,std::clamp(p.z,bottom,top),hint)-CrossSection(c,p.x,p.y),std::min(p.z-bottom,top-p.z));
    // Match LiquidCore.inc; cached samples retain volume conservation.
    float r=RadiusHinted(c,std::clamp(p.z,bottom,top),hint);
    float width=.35f+std::min(.35f*c.viscousSurface.y,r*.15f),height=.06f+std::min(.06f*c.viscousSurface.x,r*.03f);
    float t=std::clamp(1.f-wall/width,0.f,1.f);
    return height*t*t*t;
}
// Coupling two cached sine modes adds sum/difference crests without more trig.
static float WaveSum(float u,float v,float a,float b){return a*u+b*v+.35f*(std::abs(a)+std::abs(b))*u*v;}
static float Wave(Vec p,float time,float amplitude,float secondary,float frequency,float medium,const Constants& c){if(c.tunedInfo.x>0)return TunedWave(c,p,amplitude,secondary,medium);if(amplitude==0&&secondary==0&&medium==0)return 0;return WaveSum(std::sin((.8f*p.x+.6f*p.y)*frequency+time*3),std::sin((-.48f*p.x+.64f*p.y+.6f*p.z)*frequency*1.73f-time*5.1f),amplitude,secondary)+medium*std::sin((.36f*p.x-.48f*p.y+.8f*p.z)*frequency*2.35f+time*4.2f+1.1f);}
#include "SurfaceTuning.inc"
#ifdef DBL_SURFACE_REFERENCE
#include "../tests/ReferenceSurface.inc"
#include "../tests/ReferenceSurface77.inc"
bool UseReferenceFoam=false;
bool UseReferenceGeometry=false;
bool UseReferenceVolume=false;
#endif
static float WaveSlope(const Constants& c,float a,float b,float medium,float frequency){
    if(c.tunedInfo.x>0){float result=0,amplitudes[]={a,b,medium};for(int i=0;i<3;++i){auto k=c.tunedModes[i];float bound=std::abs(amplitudes[i]);if(i<2)bound+=c.tunedInfo.y*.35f*(std::abs(a)+std::abs(b));result+=bound*Length({k.x,k.y,k.z});}return result;}
    return (a+b*1.73f+.35f*(a+b)*2.73f+medium*2.35f)*frequency;
}
// Retain existing particles; no new shader resources or unbounded emission.
#ifdef DBL_SURFACE_REFERENCE
float FoamAttachmentError(const Constants& c,const SurfaceBubble& p){
    Vec n{c.plane.x,c.plane.y,c.plane.z};
    return std::abs(c.plane.w-Dot(p.position,n)+Wave(p.position,c.wave.z,c.wave.x*c.waveDetail.y,c.waveDetail.x*c.waveDetail.z,c.wave.y,c.wave.w,c)+MeniscusHeight(c,p.position)-p.radius*.65f);
}
float FoamWallClearance(const Constants& c,const SurfaceBubble& p){
    float e=p.radius*1.3f;auto q=p.position;
    return std::min({Radius(c,q.z-e),Radius(c,q.z),Radius(c,q.z+e)})-CrossSection(c,q.x,q.y)-e-.002f;
}
#endif
static float SurfaceLifetime(const Settings& s,const SurfaceBubble& p){return p.lifetime>0?p.lifetime:s.surfaceLifetime;}
static bool RetainSurfaceBubble(const Settings& s,Motion& m,Vec position,float radius){
    if(s.foamStrength<=0||s.intensity<=0||s.fill<=0||s.fill>=1||radius<=.001f||m.surfaceBubbles.size()>=m.bubbleLimits.foam)return false;
    auto& random=m.foamRandom;random^=random<<13;random^=random>>17;random^=random<<5;
    float variation=(float(random>>8)/16777215.f*2-1)*s.surfaceLifetimeJitter;
    float lifetime=std::max(.13f,s.surfaceLifetime+variation);
    m.surfaceBubbles.push_back({position,radius,0,radius,lifetime});++m.foamRetained;return true;
}
static void ShakingBubbles(const Settings& s,Motion& m,Constants& c,Vec acceleration,Vec up,float dt,bool detail){
    m.shakingBirths=0;
    if(!detail||s.shakingBubbles<=0||s.fill<=0||s.fill>=1||s.samples.empty()){
        m.shaking.clear();m.shakeActivity=m.shakeCredit=m.previousShakeDrive=m.shakeBurstCooldown=0;
        ClearBubbleTrigger(m);m.flowEnergy=0;return;
    }
    if(dt<=0)return;
    const float span=c.rings[c.ringCount-1].x-c.rings[0].x;
    auto trigger=UpdateBubbleTrigger(m,span,dt);float drive=trigger.drive;
    m.shakeActivity+=(drive-m.shakeActivity)*(1-std::exp(-dt/(drive>m.shakeActivity?.015f:.09f)));
    m.flowEnergy=std::max(m.shakeActivity*Viscosity(s.viscosity).excitation,m.flowEnergy*std::exp(-dt/(.45f*Viscosity(s.viscosity).flow)));
    m.shakeBurstCooldown=std::max(0.f,m.shakeBurstCooldown-dt);
    bool burst=drive>.55f&&m.shakeBurstCooldown==0&&(m.previousShakeDrive<.3f||trigger.reversal);
    // A gentle qualified vertical stroke may last only a few updates. Give its
    // onset one tiny-particle credit rather than requiring a post-motion tail
    // to accumulate enough credit. Strong strokes keep the existing burst.
    bool verticalOnset=drive>0&&m.previousShakeDrive==0&&(m.triggerSources&2);
    m.previousShakeDrive=drive;m.previousShakeOmega=m.emissionOmega;
    if(m.shaking.empty()&&m.shakeActivity<.001f){m.shakeActivity=m.shakeCredit=0;return;}
    const Vec normal{c.plane.x,c.plane.y,c.plane.z};
    const float primary=c.wave.x*c.waveDetail.y,secondary=c.waveDetail.x*c.waveDetail.z;
    const BubbleFlow flow(c,m,up,acceleration);
    auto clearance=[&](Vec q){return c.plane.w-Dot(q,normal)+Wave(q,m.time*s.waveTimeScale,primary,secondary,s.waveFrequency,c.wave.w,c);};
    auto walls=[&](Vec q,float radius){
        if(q.z-radius<c.rings[0].x||q.z+radius>c.rings[c.ringCount-1].x)return false;
        float inner=std::min({Radius(c,q.z-radius),Radius(c,q.z),Radius(c,q.z+radius)});
        return CrossSection(c,q.x,q.y)+radius<inner;
    };
    auto contain=[&](ShakingParticle& p){
        float r=p.baseRadius*BubbleStretchLimit(p.baseRadius);
        float bottom=c.rings[0].x+r+.001f,top=c.rings[c.ringCount-1].x-r-.001f;
        if(bottom>=top)return false;
        if(p.position.z<bottom){p.position.z=bottom;p.velocity.z=std::max(0.f,p.velocity.z);}
        if(p.position.z>top){p.position.z=top;p.velocity.z=std::min(0.f,p.velocity.z);}
        float inner=std::min({Radius(c,p.position.z-r),Radius(c,p.position.z),Radius(c,p.position.z+r)})-r-.001f;
        if(inner<=.001f)return false;
        float radial=CrossSection(c,p.position.x,p.position.y);
        if(radial>inner){
            Vec n=SectionNormal(c,p.position.x,p.position.y);
            p.position.x*=inner/radial;p.position.y*=inner/radial;
            p.velocity=p.velocity-n*std::max(0.f,Dot(p.velocity,n));
        }
        return true;
    };
    int steps=std::max(1,int(std::ceil(dt*120)));float h=dt/steps;
    for(auto& p:m.shaking){
        const float lifetime=ShakingLifetime(p.phase)/Viscosity(s.viscosity).rise;
        float size=std::clamp(p.baseRadius/.3f,0.f,1.f);
        float response=1-std::exp(-h/(.012f+.035f*size));
        for(int step=0;step<steps;step++){
        p.age+=h;
        p.velocity=p.velocity+(flow.Target(p.position,span*(.30f+.70f*size)*Viscosity(s.viscosity).rise)-p.velocity)*response;
        const Vec previousPosition=p.position;
        p.position=p.position+p.velocity*h;
        if(!contain(p)){
            // Previous valid location is a bounded, cheap collision fallback.
            p.position=previousPosition;p.velocity={};++m.shakeWallBlocked;
        }
        float gap=clearance(p.position);
            if(gap<=.65f*p.radius && RetainSurfaceBubble(s,m,p.position,p.radius)){
                ++m.shakeTransferred;p.radius=0;break;
            }
        if(gap<=.002f){++m.shakeSurfacePopped;p.radius=0;break;}
        float birth=std::clamp(p.age/.05f,0.f,1.f);birth=birth*birth*(3-2*birth);
        // Keep full size until contact when foam can accept the bubble.
        const bool foamAvailable=s.foamStrength>0&&s.intensity>0&&m.surfaceBubbles.size()<m.bubbleLimits.foam;
        float pop=foamAvailable?1.f:std::clamp(gap/std::max(p.baseRadius,.001f),0.f,1.f);
        p.radius=p.baseRadius*(.25f+.75f*birth)*pop*std::clamp((lifetime-p.age)/1.f,0.f,1.f);
        }
        if(p.age<lifetime&&p.radius>.001f)DeformBubble(p,flow,p.phase,m.time,dt);
    }
    std::erase_if(m.shaking,[&](const auto& p){if(p.age>=ShakingLifetime(p.phase)/Viscosity(s.viscosity).rise){++m.shakeTimedOut;return true;}return p.radius<.001f;});
    const float excitation=std::clamp(std::min(m.shakeActivity,drive),0.f,1.f);
    const int cap=std::min(int(m.bubbleLimits.shaking),8+int(std::ceil(56*excitation)));
    if(excitation>.025f&&m.emissionValid){
        if(burst){m.shakeCredit+=s.shakingBubbles*(3+9*excitation);m.shakeBurstCooldown=.06f;}
        else if(verticalOnset)m.shakeCredit+=s.shakingBubbles;
        m.shakeCredit=std::min(m.shakeCredit+dt*s.shakingBubbles*(12+200*excitation),24.f);
        int births=std::max(0,std::min(cap-int(m.shaking.size()),int(m.shakeCredit)));
        m.shakeCredit-=std::floor(m.shakeCredit);
        std::uniform_real_distribution<float> random(0,1);
        float maximum=std::min(.3f,c.rings[0].z*.12f);
        for(int i=0;i<births;i++){
            float category=random(m.shakeRandom),variation=random(m.shakeRandom);
            float radius=maximum*ShakingRadiusFraction(trigger.sizeSpeed,category,variation);
            float bound=radius*BubbleStretchLimit(radius);
            for(int attempt=0;attempt<24;attempt++){
            auto sample=s.samples[std::min(s.samples.size()-1,size_t(random(m.shakeRandom)*s.samples.size()))];
            Vec q{sample.x,sample.y,sample.z};
            float margin=bound*(1+WaveSlope(c,c.wave.x,c.waveDetail.x,c.wave.w,s.waveFrequency));
            float depth=clearance(q);
            if(!walls(q,bound)||depth<margin||depth>span*.28f+bound)continue;
            bool overlaps=false;for(const auto& p:m.shaking)if(Length(q-p.position)<(radius+p.baseRadius)*.75f){overlaps=true;break;}
            if(overlaps)continue;
            Vec velocity=flow.Target(q,span*.5f)-normal*(span*.8f);
            m.shaking.push_back({q,ShakeLimit(velocity,span*3),radius*.25f,radius,0,random(m.shakeRandom)*6.2831853f});++m.shakingBirths;break;
            }
        }
    }else m.shakeCredit=0;
}
Constants Simulate(const Settings& s,Motion& m,Vec acceleration,Vec up,float dt,bool detail,bool held,float motionWeight,SimulationTimings* timings){
    SimulationStageTimer timer(timings);
    ApplyBubbleLimits(m,m.bubbleLimits);
    Constants c=s.base;up=Normalize(up);dt=std::clamp(dt,0.f,.05f);
    auto visc=Viscosity(s.viscosity);auto waveVisc=ViscousWaves(s.viscosity);Vec surfaceUp=up;
    if(s.viscosity>0){
        if(!m.liquidUpValid){m.liquidUp=up;m.liquidUpValid=true;m.rimHeight=visc.height-1;}
        if(dt>0){
            float cosine=std::clamp(Dot(m.liquidUp,up),-1.f,1.f),angle=std::acos(cosine);
            Vec tangent=up-m.liquidUp*cosine;
            if(Length(tangent)<.00001f)tangent=std::abs(m.liquidUp.x)<.8f?Vec{1,0,0}-m.liquidUp*m.liquidUp.x:Vec{0,1,0}-m.liquidUp*m.liquidUp.y;
            float turn=angle*(-std::expm1(-dt/(.10f*visc.level)));
            m.liquidUp=angle<.00001f?up:Normalize(m.liquidUp*std::cos(turn)+Normalize(tangent)*std::sin(turn));
            float wanted=(visc.height-1)*(1-.25f*m.activity);
            m.rimHeight+=(wanted-m.rimHeight)*(-std::expm1(-dt/(.08f*visc.rimTime)));
        }
        surfaceUp=m.liquidUp;
        c.viscousSurface={m.rimHeight,visc.width-1,0,0};
        // Thick deposits drain more slowly than the old thin wet sheen.
        float coating=std::clamp(std::log2(std::max(s.viscosity,1.f))/std::log2(30.f),0.f,1.f);
        c.wetTuning.w*=visc.film*(1+2*coating);
    }
    float shakeDrive=0,sloshFrequency=6.2831853f*s.frequency;
    if(s.previewTuning)UpdateTunedSection(s,m,Normalize(surfaceUp+m.slosh),dt);
    if(dt>0){
        // Reject extreme single-update impulses, then integrate in seconds.
        float accelerationLength=Length(acceleration);
        if(accelerationLength>6000)acceleration=acceleration*(6000/accelerationLength);
        // Acceleration is transformed from world space by the adapter, never a rotated world position.
        float angular=Length(up-m.lastUp)/dt;
        float target=std::clamp(angular/2+Length(acceleration)/1500,0.f,1.f);
        shakeDrive=target;target*=visc.excitation;
        m.activity=target+(m.activity-target)*std::exp(-dt/(target>m.activity?.08f:s.settle));
        // Two cached directional modes: steer their signed weights, never rebuild
        // sample trigonometry or add another shader wave.
        Vec lateral=acceleration-up*Dot(acceleration,up);
        Vec drive=lateral*(-1.f/1500)+(m.lastUp-up)*10;
        if(Length(drive)>.01f){
            drive=Normalize(drive);Vec wanted{Dot(drive,{.8f,.6f,0}),Dot(drive,{-.48f,.64f,.6f}),0};
            float peak=std::max(std::abs(wanted.x),std::abs(wanted.y));
            if(peak>.01f){wanted=wanted*(1/peak);float blend=m.waveEnergy==0?1.f:1-std::exp(-dt/.12f);m.waveMix=m.waveMix+(wanted-m.waveMix)*blend;}
        }
        // Fast attack; fine ripples lose energy sooner than the broad wave.
        m.waveEnergy=std::max(target,m.waveEnergy*std::exp(-dt/((s.previewTuning?10.f/2.302585093f*m.modeDecay[0]:s.broadWaveDecay)*visc.ripple)));
        m.rippleEnergy=std::max(target,m.rippleEnergy*std::exp(-dt/(.35f*(s.previewTuning?m.modeDecay[1]:1)*visc.ripple*waveVisc.fineDecay)));
        if(s.previewTuning)m.secondaryEnergy=std::max(target,m.secondaryEnergy*std::exp(-dt/(10.f/2.302585093f*m.modeDecay[1]*visc.ripple*waveVisc.fineDecay)));
        // Independent intermediate-scale response: motion-driven, 1.3s release.
        float mediumDrive=std::clamp(Length(acceleration)/1200+angular/3,0.f,1.f);
        m.mediumEnergy=std::max(s.previewTuning?target:mediumDrive*visc.excitation,m.mediumEnergy*std::exp(-dt/((s.previewTuning?15.f/2.302585093f*m.modeDecay[2]:1.3f)*visc.ripple*waveVisc.mediumDecay)));
        if(m.mediumEnergy<.0001f)m.mediumEnergy=0;
        if(m.waveEnergy<.0001f)m.waveEnergy=0;
        if(m.rippleEnergy<.0001f)m.rippleEnergy=0;
        if(m.secondaryEnergy<.0001f)m.secondaryEnergy=0;
        float impulse=(s.previewTuning?3.f:1.f)*visc.excitation;
        m.sloshVelocity=m.sloshVelocity+(m.lastUp-up)*(s.gain*4*impulse)+lateral*(-s.gain*.006f*dt*impulse);
        int steps=std::max(1,int(std::ceil(dt*240)));float step=dt/steps,w=6.2831853f*s.frequency,damping=s.damping;
        if(s.previewTuning){float decay=2.302585093f/(10*m.sloshDecay),wd=6.2831853f*.894051f*std::sqrt(1-.11f*.11f)*3*m.sloshRate;w=std::sqrt(wd*wd+decay*decay);damping=decay/w;}
        // Preview's legacy damping ratio is very small. Reach critical damping
        // for sap instead of merely multiplying that tiny ratio indefinitely.
        damping=std::max(damping*visc.damping,(visc.damping-1)*.4f);sloshFrequency=w;
        for(int i=0;i<steps;i++){m.sloshVelocity=m.sloshVelocity+(m.slosh*(-w*w)+m.sloshVelocity*(-2*damping*w))*step;m.slosh=m.slosh+m.sloshVelocity*step;
            float length=Length(m.slosh);if(length>s.maxTilt){m.slosh=m.slosh*(s.maxTilt/length);m.sloshVelocity=m.sloshVelocity*.3f;}}
        // Fully settled instances can reuse their cached volume solution.
        if(Length(m.slosh)<.00001f&&Length(m.sloshVelocity)<.00001f){m.slosh={};m.sloshVelocity={};}
        m.time+=dt;
    }
    m.lastUp=up;
    if(s.previewTuning){
        const float speeds[]={3*.607955f*5,-5.1f*.607955f*10,4.2f*.607955f*10};
        c.tunedInfo.x=1;c.tunedInfo.y=s.tunedInteraction?1.f:0.f;
        const float phaseRates[]{waveVisc.broadSpeed,waveVisc.fineSpeed,waveVisc.mediumSpeed};
        for(int i=0;i<3;++i){m.modePhase[i]=std::remainder(m.modePhase[i]+dt*speeds[i]*m.modeRate[i]*phaseRates[i],6.283185307f);auto k=m.modeVectors[i];c.tunedModes[i]={k.x,k.y,k.z,m.modePhase[i]-Dot({k.x,k.y,k.z},m.section.center)};}
        m.waveMix={1,1,0};
    }
    motionWeight=std::clamp(motionWeight,0.f,1.f);
    Vec normal=Normalize(surfaceUp+m.slosh*motionWeight);float amplitude=s.waveAmplitude*(m.waveEnergy+Length(m.slosh))*motionWeight;
    float secondary=.55f*s.waveAmplitude*(.65f*m.waveEnergy+.35f*std::min(m.rippleEnergy,m.waveEnergy))*motionWeight*waveVisc.fineStrength;
    // Preserve gentle motion and the quiet tail; amplify only energetic waves.
    float impact=std::clamp((m.waveEnergy-.25f)/.75f,0.f,1.f);
    float impactScale=1+std::clamp(s.surfaceImpactBoost,0.f,.6f)*impact*impact*(3-2*impact);
    amplitude*=impactScale;secondary*=impactScale;
    float primaryWave=amplitude*m.waveMix.x,secondaryWave=secondary*m.waveMix.y;
    float mediumWave=s.mediumWaveAmplitude*m.mediumEnergy*motionWeight*waveVisc.mediumStrength;
    if(s.previewTuning){
        float r=std::sqrt(m.section.area/3.14159265f);
        auto boost=[](float e){float x=std::clamp((e-.25f)/.75f,0.f,1.f);return 1+.6f*x*x*(3-2*x);};
        amplitude=.09f*r*1.1f*m.waveEnergy*boost(m.waveEnergy)*motionWeight;
        secondary=.55f*.09f*r*.8f*(.65f*m.secondaryEnergy+.35f*std::min(m.rippleEnergy,m.secondaryEnergy))*boost(m.secondaryEnergy)*motionWeight*waveVisc.fineStrength;
        mediumWave=.055f*r*1.1f*m.mediumEnergy*motionWeight*waveVisc.mediumStrength;
        float gain=m.heightGain,peak=(amplitude+secondary+mediumWave+(s.tunedInteraction?.35f*(amplitude+secondary):0))*gain;
        if(gain<1){float knee=.15f*m.section.depth;if(peak>knee)gain*=(knee+knee*(1-std::exp(-(peak-knee)/knee)))/peak;}
        amplitude*=gain;secondary*=gain;mediumWave*=gain;primaryWave=amplitude;secondaryWave=secondary;
    }
    // Test79: world-space, phase-independent displacement envelope. Include
    // stored slosh velocity so a zero crossing cannot be mistaken for rest.
    if(m.restWorldThreshold>0&&dt>0&&s.previewTuning){
        float radius=std::hypot(m.section.rx,m.section.ry);
        float sloshEnvelope=std::sqrt(Dot(m.slosh,m.slosh)+Dot(m.sloshVelocity,m.sloshVelocity)/std::max(sloshFrequency*sloshFrequency,.0001f));
        float waveEnvelope=std::abs(primaryWave)+std::abs(secondaryWave)+std::abs(mediumWave);
        if(s.tunedInteraction)waveEnvelope+=.35f*(std::abs(primaryWave)+std::abs(secondaryWave));
        // Twice the excursion conservatively allows the volume-preserving
        // plane shift. The section span scales slosh to this actual cavity.
        m.surfaceDisplacement=2*(waveEnvelope+radius*sloshEnvelope)*std::abs(m.worldScale);
        if(shakeDrive>=.01f||m.surfaceDisplacement>=m.restWorldThreshold)m.quietSurfaceAge=0;
        else {
            float before=m.quietSurfaceAge;m.quietSurfaceAge=std::min(before+dt,.6f);
            auto weight=[](float age){float t=std::clamp((age-.2f)/.4f,0.f,1.f);return 1-t*t*(3-2*t);};
            float oldWeight=weight(before),newWeight=weight(m.quietSurfaceAge);
            float factor=oldWeight>0?newWeight/oldWeight:0;
            m.waveEnergy*=factor;m.secondaryEnergy*=factor;m.mediumEnergy*=factor;m.rippleEnergy*=factor;
            m.slosh=m.slosh*factor;m.sloshVelocity=m.sloshVelocity*factor;
            amplitude*=factor;secondary*=factor;primaryWave*=factor;secondaryWave*=factor;mediumWave*=factor;
            normal=Normalize(surfaceUp+m.slosh*motionWeight);
        }
    }
    // Surface detail grows smoothly when a wave is excited. Its release follows
    // the existing fast fine-ripple decay instead of the longer broad slosh.
    float noiseTarget=!s.previewTuning&&detail&&s.fill>0&&s.fill<1?std::min(m.rippleEnergy,m.waveEnergy):0.f;
    if(dt>0){
        float response=noiseTarget>m.surfaceNoise?.12f:.06f;
        m.surfaceNoise+=(noiseTarget-m.surfaceNoise)*(1-std::exp(-dt/response));
        if(noiseTarget==0&&m.surfaceNoise<.0001f)m.surfaceNoise=0;
    }
    timer.End(0);
    bool cachedMeniscusGeometry=s.meniscusRingCount==c.ringCount&&s.sampleMeniscusGeometry.size()==s.samples.size()&&
        std::memcmp(s.meniscusRings.data(),c.rings,sizeof(c.rings))==0&&std::memcmp(s.meniscusPlanes.data(),c.cavityPlanes,sizeof(c.cavityPlanes))==0;
#ifdef DBL_MENISCUS_REFERENCE
    if(UseReferenceMeniscus)cachedMeniscusGeometry=false;
#endif
    float height=m.solvedHeight;
    bool reuse=m.solvedRim==c.viscousSurface.x&&amplitude==0&&m.solvedAmplitude==0&&secondary==0&&m.solvedSecondary==0&&mediumWave==0&&m.solvedMedium==0&&s.fill==m.solvedFill&&
        normal.x==m.solvedNormal.x&&normal.y==m.solvedNormal.y&&normal.z==m.solvedNormal.z;
    if(!reuse)m.wetGeometryValid=false;
    m.solvedRim=c.viscousSurface.x;
    if(!reuse){
        auto& values=m.volumeScratch;values.clear();values.reserve(s.samples.size());
        bool cachedTotal=s.sampleRevision>0&&s.sampleWeightCount==s.samples.size();
        float low=1e30f,high=-1e30f;double total=cachedTotal?s.sampleWeightTotal:0;
        const auto* basis=s.previewTuning?m.tunedSamples.get():nullptr;
        bool cachedProjection=basis&&std::memcmp(&basis->projectionNormal,&normal,sizeof(normal))==0;
#ifdef DBL_SURFACE_REFERENCE
        if(UseReferenceGeometry){cachedTotal=false;total=0;cachedProjection=false;}
#endif
        const auto* primaryBasis=basis?basis->primarySecondary.data():nullptr;
        const auto* mediumBasis=basis?basis->medium.data():nullptr;
        bool cachedWave=!s.previewTuning&&s.sampleWaveBasis.size()==s.samples.size()&&s.sampleMediumBasis.size()==s.samples.size()&&s.sampleWaveFrequency==s.waveFrequency;
        float sx=0,cx=0,sy=0,cy=0,sz=0,cz=0;
        if((amplitude!=0||secondary!=0||mediumWave!=0)&&cachedWave){sx=std::sin(m.time*s.waveTimeScale*3);cx=std::cos(m.time*s.waveTimeScale*3);sy=std::sin(-m.time*s.waveTimeScale*5.1f);cy=std::cos(-m.time*s.waveTimeScale*5.1f);sz=std::sin(m.time*s.waveTimeScale*4.2f+1.1f);cz=std::cos(m.time*s.waveTimeScale*4.2f+1.1f);}
        if(s.previewTuning){sx=std::sin(c.tunedModes[0].w);cx=std::cos(c.tunedModes[0].w);sy=std::sin(c.tunedModes[1].w);cy=std::cos(c.tunedModes[1].w);sz=std::sin(c.tunedModes[2].w);cz=std::cos(c.tunedModes[2].w);}
        for(size_t index=0;index<s.samples.size();index++){
            auto p=s.samples[index];float wave=0;
            if(amplitude!=0||secondary!=0||mediumWave!=0){
                if(cachedWave){auto b=s.sampleWaveBasis[index];auto d=s.sampleMediumBasis[index];wave=WaveSum(b.x*cx+b.y*sx,b.z*cy+b.w*sy,primaryWave,secondaryWave)+mediumWave*(d.x*cz+d.y*sz);}
                else if(s.previewTuning){auto b=primaryBasis[index],d=mediumBasis[index];float u=b.x*cx+b.y*sx,v=b.z*cy+b.w*sy;wave=primaryWave*u+secondaryWave*v+mediumWave*(d.x*cz+d.y*sz);if(s.tunedInteraction)wave+=.35f*(std::abs(primaryWave)+std::abs(secondaryWave))*u*v;}
                else wave=Wave({p.x,p.y,p.z},m.time*s.waveTimeScale,primaryWave,secondaryWave,s.waveFrequency,mediumWave,c);
            }
            float meniscus=s.viscosity<=0&&s.sampleMeniscus.size()==s.samples.size()?s.sampleMeniscus[index]:
                cachedMeniscusGeometry?MeniscusFromGeometry(c,s.sampleMeniscusGeometry[index]):MeniscusHeight(c,{p.x,p.y,p.z});
            float projection=cachedProjection?basis->volumeDots[index]:Dot({p.x,p.y,p.z},normal);
            float v=projection-wave-meniscus;values.emplace_back(v,p.w);low=std::min(low,v);high=std::max(high,v);if(!cachedTotal)total+=p.w;
        }
        if(s.fill<=0)height=low-100;else if(s.fill>=1)height=high+100;else {
            // Bin once to narrow the height range; retain 18-bit bisection precision.
            auto selectedEnd=values.end();double fixedVolume=0;
            std::array<double,256> weights{};float width=(high-low)/256;
            if(width>0){
                for(auto [v,w]:values){int bin=std::clamp(int((v-low)/width),0,255);weights[bin]+=w;}
                double before=0,target=total*s.fill;int bin=0;
                for(;bin<255&&before+weights[bin]<target;bin++)before+=weights[bin];
                // Refinement only needs the selected histogram bin. Lower bins
                // contribute a fixed volume; upper bins cannot enter the result.
                float origin=low;fixedVolume=before;
                // Cheap conservative rejection before the original bin division.
                // Keep partition ordering and all bisection arithmetic unchanged.
                float binLow=origin+bin*width,binHigh=binLow+width;
                float margin=16*std::numeric_limits<float>::epsilon()*std::max({std::abs(origin),std::abs(high),std::abs(width),1.f});
                selectedEnd=std::partition(values.begin(),values.end(),[&](const auto& p){
#ifdef DBL_SURFACE_REFERENCE
                    if(!UseReferenceVolume)
#endif
                    if(p.first<binLow-margin||p.first>binHigh+margin)return false;
                    return std::clamp(int((p.first-origin)/width),0,255)==bin;
                });
                float start=low+bin*width;high=start+width;low=start;
            }
            for(int i=0;i<10;i++){float mid=(low+high)*.5f;double volume=fixedVolume;for(auto it=values.begin();it!=selectedEnd;++it){auto [v,w]=*it;volume+=(v<=mid?w:0.f);}if(volume/total<s.fill)low=mid;else high=mid;}
            height=(low+high)*.5f;
        }
        m.solvedNormal=normal;m.solvedHeight=height;m.solvedFill=s.fill;m.solvedAmplitude=amplitude;m.solvedMedium=mediumWave;m.solvedSecondary=secondary;
    }
    c.plane={normal.x,normal.y,normal.z,height};c.wave={amplitude,s.waveFrequency,m.time*s.waveTimeScale,mediumWave};c.waveDetail={secondary,m.waveMix.x,m.waveMix.y,m.surfaceNoise};c.foam=0;
    timer.End(1);
    // A small per-instance wall history records contact, not merely agitation.
    if(!detail||s.fill<=0||s.fill>=1){m.wetFilm.fill(0);c.wetInfo={};}
    else{
        static const auto directions=[](){std::array<Vec,16> result{};for(int k=0;k<16;k++){float a=k*6.283185307f/16;result[k]={std::cos(a),std::sin(a),0};}return result;}();
        float bottom=c.rings[0].x,span=std::max(c.rings[c.ringCount-1].x-bottom,.001f),exposed=0,decay=std::exp(-dt/std::max(c.wetTuning.w,.1f));
        // Brief contact builds continuously; a zero-time solve cannot wet new glass.
        const float contactBlend=-std::expm1(-dt/.06f);
        bool cachedWall=s.wetSamples.size()==256 && s.wetRingCount==c.ringCount && std::memcmp(s.wetRings.data(),c.rings,sizeof(c.rings))==0;
        const auto* wallBasis=s.previewTuning&&cachedWall?m.tunedSamples.get():nullptr;
        bool cachedWallProjection=wallBasis&&wallBasis->wallDots.size()==256&&std::memcmp(&wallBasis->projectionNormal,&normal,sizeof(normal))==0;
        bool cachedTunedPhase=wallBasis&&wallBasis->wallPhases.size()==256;
#ifdef DBL_SURFACE_REFERENCE
        if(UseReferenceGeometry){cachedWallProjection=false;cachedTunedPhase=false;}
#endif
        bool cachedPhase=!s.previewTuning&&cachedWall && s.wetPhases.size()==256 && s.wetFrequency==s.waveFrequency;
        bool reuseWall=reuse&&m.wetGeometryValid&&m.wetGeometryRingCount==c.ringCount&&
            std::memcmp(m.wetGeometryRings.data(),c.rings,sizeof(c.rings))==0&&
            std::memcmp(m.wetGeometryPlanes.data(),c.cavityPlanes,sizeof(c.cavityPlanes))==0;
        if(reuseWall)++m.wetGeometryHits;else ++m.wetGeometryBuilds;
        for(int row=0;row<16;row++){
            float z=bottom+span*row/15,rad=cachedWall?0:Radius(c,z)*.98f;
            for(int k=0;k<16;k++){
                float d;
                if(reuseWall)d=m.wetDistances[row*16+k];
                else {
                auto wall=cachedWall?s.wetSamples[row*16+k]:V4{};
                float factor=cachedWall?1:1/std::max(CrossSection(c,directions[k].x,directions[k].y),.001f);
                Vec q=cachedWall?Vec{wall.x,wall.y,wall.z}:Vec{directions[k].x*rad*factor,directions[k].y*rad*factor,z};
                float wave=0;
                if(primaryWave!=0||secondaryWave!=0||mediumWave!=0){
                    if(cachedPhase){auto phase=s.wetPhases[row*16+k];wave=WaveSum(std::sin(phase.x+m.time*s.waveTimeScale*3),std::sin(phase.y-m.time*s.waveTimeScale*5.1f),primaryWave,secondaryWave)+mediumWave*std::sin(phase.z+m.time*s.waveTimeScale*4.2f+1.1f);}
                    else if(cachedTunedPhase){
                        auto phase=m.tunedSamples->wallPhases[row*16+k];
                        float u=std::sin(phase.x+c.tunedModes[0].w),v=std::sin(phase.y+c.tunedModes[1].w),w=std::sin(phase.z+c.tunedModes[2].w);
                        wave=primaryWave*u+secondaryWave*v+mediumWave*w;
                        if(c.tunedInfo.y>0)wave+=.35f*(std::abs(primaryWave)+std::abs(secondaryWave))*u*v;
                    }
                    else wave=Wave(q,m.time*s.waveTimeScale,primaryWave,secondaryWave,s.waveFrequency,mediumWave,c);
                }
                float projection=cachedWallProjection?wallBasis->wallDots[row*16+k]:Dot(q,normal);
                d=projection-height-wave-(cachedWall&&s.viscosity<=0?wall.w:
                    cachedMeniscusGeometry&&cachedWall&&s.wetMeniscusGeometry.size()==256?MeniscusFromGeometry(c,s.wetMeniscusGeometry[row*16+k]):MeniscusHeight(c,q));
                m.wetDistances[row*16+k]=d;
                }
                auto& wet=m.wetFilm[row*16+k];wet=d<=0?wet+(1.f-wet)*contactBlend:wet*decay;if(wet<.001f)wet=0;
                if(d>0)exposed=std::max(exposed,wet);
                auto& packed=c.wetFilm[row*4+k/4];switch(k%4){case 0:packed.x=wet;break;case 1:packed.y=wet;break;case 2:packed.z=wet;break;default:packed.w=wet;break;}
            }
        }
        m.wetGeometryValid=true;m.wetGeometryRingCount=c.ringCount;
        std::copy_n(c.rings,16,m.wetGeometryRings.begin());
        std::copy_n(c.cavityPlanes,16,m.wetGeometryPlanes.begin());
        c.wetInfo={exposed,bottom,15/span,0};
    }
    auto valid=[&](const Particle& p){float r=p.radius;auto q=p.position;if(s.fill<=0||q.z-r<c.rings[0].x||q.z+r>c.rings[c.ringCount-1].x)return false;
        float inner=std::min({Radius(c,q.z-r),Radius(c,q.z),Radius(c,q.z+r)});
        return CrossSection(c,q.x,q.y)+r<inner&&(s.fill>=1||Dot(q,normal)-Wave(q,m.time*s.waveTimeScale,primaryWave,secondaryWave,s.waveFrequency,mediumWave,c)+r*(1+WaveSlope(c,amplitude,secondary,mediumWave,s.waveFrequency))<height);};
    timer.End(2);
    ShakingBubbles(s,m,c,acceleration,up,dt,detail);
    timer.End(3);
    const BubbleFlow bubbleFlow(c,m,up,acceleration);
#ifdef DBL_SURFACE_REFERENCE
    if(UseReferenceFoam){
#include "../tests/ReferenceFoam76.inc"
    }else
#endif
    {
    if(!detail||s.foamStrength<=0||s.intensity<=0||s.fill<=0||s.fill>=1)m.surfaceBubbles.clear();
    // Pop expired cells before neighbour separation. Survivors use only the
    // original surface flow, rim migration and contact relaxation below.
    if(dt>0)std::erase_if(m.surfaceBubbles,[&](const auto& p){return p.age+dt>=SurfaceLifetime(s,p);});
    // Bounded tangential relaxation distributes foam without vertical stacking.
    // Four small iterations, at most 64*63/2 pairs each; no per-pixel neighbours.
    if(dt>0)for(int iteration=0;iteration<4;iteration++){
        for(size_t a=0;a<m.surfaceBubbles.size();a++)for(size_t b=a+1;b<m.surfaceBubbles.size();b++){
            auto& x=m.surfaceBubbles[a];auto& y=m.surfaceBubbles[b];
            Vec d=y.position-x.position;d=d-normal*Dot(d,normal);float squaredDistance=Dot(d,d);
            float target=(x.radius+y.radius)*1.02f;
            if(SurfacePairSeparated(squaredDistance,target))continue;
            float length=std::sqrt(squaredDistance);
            if(length>=target)continue;
            if(length<.00001f){
                float angle=float(a*37+b*17)*2.399963f;Vec seed{std::cos(angle),std::sin(angle),.37f};
                d=Normalize(seed-normal*Dot(seed,normal));
                if(Length(d)<.1f)d=Normalize(Vec{normal.z,0,-normal.x});
            }else d=d*(1/length);
            Vec correction=d*((target-length)*.5f*std::min(dt*30.f,1.f));
            x.position=x.position-correction;y.position=y.position+correction;
        }
    }
    int surfaceRingHint=1,clearanceRingHint=1;
    auto attachToSurface=[&](Vec q,float radius){
        for(int i=0;i<12;i++){
            float gap=height-Dot(q,normal)+Wave(q,m.time*s.waveTimeScale,primaryWave,secondaryWave,s.waveFrequency,mediumWave,c)+MeniscusHeightHinted(c,q,surfaceRingHint);
            Vec next=q+normal*(gap-radius*.65f);
            // An exact fixed point cannot change in the remaining iterations.
            // Final attachment is refined independently of the flat rim search.
            if(std::memcmp(&next,&q,sizeof(Vec))==0)return next;
            q=next;
        }
        return q;
    };
    // Lateral motion and rim searches use the mean plane. Evaluate the waves
    // only for final attachment and for exceptional wall-contact corrections.
    bool useWaveBoundary=false;
    auto surfacePoint=[&](Vec q,float radius){
        if(useWaveBoundary)return attachToSurface(q,radius);
        return q+normal*(height-Dot(q,normal)-radius*.65f);
    };
    auto clearance=[&](Vec q,float extent){
        return std::min({RadiusHinted(c,q.z-extent,clearanceRingHint),RadiusHinted(c,q.z,clearanceRingHint),RadiusHinted(c,q.z+extent,clearanceRingHint)})-CrossSection(c,q.x,q.y)-extent-.002f;
    };
    // Find an interior surface anchor from the authored cavity, including horizontal
    // bottles where intersecting the bottle axis with the plane is ill-conditioned.
    Vec anchor{};float best=-1e30f;
    if(!m.surfaceBubbles.empty())for(int i=1;i<c.ringCount;i++){
        Vec q=surfacePoint({0,0,(c.rings[i-1].x+c.rings[i].x)*.5f},0);
        float room=clearance(q,.01f);
        if(room>best){best=room;anchor=q;}
    }
    float lastCenterRadius=-1;Vec lastCenter{};
    bool waveAnchorReady=false;Vec waveAnchor{};
    for(size_t index=0;index<m.surfaceBubbles.size();index++){
        useWaveBoundary=false;
        auto& p=m.surfaceBubbles[index];p.age+=dt;
        const float extent=p.radius*1.3f;
        // Identical consecutive radii have an identical anchor projection.
        Vec center;
        if(p.radius==lastCenterRadius)center=lastCenter;
        else {center=surfacePoint(anchor,p.radius);lastCenter=center;lastCenterRadius=p.radius;}
        if(clearance(center,extent)<0){
            // A nearly full, tilted bottle may have no usable mean-plane anchor.
            // Fall back to the wave-aware boundary only for those cells.
            useWaveBoundary=true;
            if(!waveAnchorReady){
                float bestRoom=-1e30f;
                for(int ring=1;ring<c.ringCount;++ring){
                    Vec q=attachToSurface({0,0,(c.rings[ring-1].x+c.rings[ring].x)*.5f},0);
                    float room=clearance(q,.01f);if(room>bestRoom){waveAnchor=q;bestRoom=room;}
                }
                waveAnchorReady=true;
            }
            center=attachToSurface(waveAnchor,p.radius);
            if(clearance(center,extent)<0){++m.foamFitRejected;p.age=SurfaceLifetime(s,p);continue;}
        }
        Vec outward=p.position-center;outward=outward-normal*Dot(outward,normal);
        if(Length(outward)<.001f){
            float angle=float(index)*2.399963f;Vec seed{std::cos(angle),std::sin(angle),.37f};
            outward=seed-normal*Dot(seed,normal);
            if(Length(outward)<.1f)outward={normal.z,0,-normal.x};
        }
        outward=Normalize(outward);
        // The target is just inside the actual tilted liquid/cavity intersection.
        float range=2*(c.rings[c.ringCount-1].x-c.rings[0].x+c.rings[0].z);
        float lo=0,hi=range;Vec edge{};bool foundEdge=false;
        for(int step=0;step<12;step++){
            float mid=(lo+hi)*.5f;
            Vec point=surfacePoint(center+outward*mid,p.radius);
            if(clearance(point,extent)>=0){lo=mid;edge=point;foundEdge=true;}else hi=mid;
        }
        // The final lower-bound point was already evaluated by the search.
        if(!foundEdge)edge=surfacePoint(center+outward*lo,p.radius);
        Vec drift=bubbleFlow.Current(p.position);drift=drift-normal*Dot(drift,normal);
        // Keep only along-rim current near the wall; no independent perpetual spin.
        float nearEdge=std::clamp(Length(p.position-center)/std::max(lo,.001f),0.f,1.f);
        drift=drift-outward*(Dot(drift,outward)*nearEdge);
        Vec candidate=surfacePoint(p.position+(edge-p.position)*(1-std::exp(-dt*1.8f))+drift*(dt*.25f),p.radius);
        if(clearance(candidate,extent)<0){
            // Constrain contact rather than killing foam at the wall.
            float a=0,b=1;Vec fitted{};bool found=false;
            for(int step=0;step<10;step++){
                float t=(a+b)*.5f;Vec q=surfacePoint(center+(candidate-center)*t,p.radius);
                if(clearance(q,extent)>=0){a=t;fitted=q;found=true;}else b=t;
            }
            candidate=found?fitted:surfacePoint(center+(candidate-center)*a,p.radius);
        }
        Vec attached=attachToSurface(candidate,p.radius);
        if(clearance(attached,extent)<0){
            // A crest can move the final point into a narrowing wall. Find an
            // interior attached point, then constrain along the surface itself.
            Vec safe=attachToSurface(center,p.radius);
            if(clearance(safe,extent)<0){
                float bestRoom=clearance(safe,extent);
                for(int ring=1;ring<c.ringCount;++ring){
                    Vec q=attachToSurface({0,0,(c.rings[ring-1].x+c.rings[ring].x)*.5f},p.radius);
                    float room=clearance(q,extent);if(room>bestRoom){safe=q;bestRoom=room;}
                }
            }
            if(clearance(safe,extent)<0){++m.foamFitRejected;p.age=SurfaceLifetime(s,p);continue;}
            float lo=0,hi=1;Vec fitted=safe;
            for(int step=0;step<10;++step){
                float t=(lo+hi)*.5f;Vec q=attachToSurface(safe+(candidate-safe)*t,p.radius);
                if(clearance(q,extent)>=0){lo=t;fitted=q;}else hi=t;
            }
            attached=fitted;
        }
        p.position=attached;
        p.visibleRadius=p.radius; // Keep the cell full-sized until its membrane bursts.
    }
    std::erase_if(m.surfaceBubbles,[&](const auto& p){return p.age>=SurfaceLifetime(s,p);});
    }
    timer.End(4);
    if(!detail||(!s.shotBubbles&&s.intensity<=0)||s.fill<=0){m.carbCleared+=static_cast<unsigned>(m.bubbles.size());m.bubbles.clear();m.credit=0;m.bubbleSourceValid.fill(false);}else {
        for(auto& p:m.bubbles){
            const Vec previousPosition=p.position;
            // One size/viscosity response, not a separate slow-bubble setting.
            const float referenceRadius=s.baseline>0&&!s.shotBubbles?CarbonationBaseRadius:.05f;
            const float relativeRadius=p.radius/referenceRadius;
            const float baseRise=s.rise>0?s.rise:1.f;
            float rise=s.viscosity>0?baseRise*std::clamp(relativeRadius*relativeRadius,.1f,4.f)*visc.rise:s.rise*p.speed;
            if(dt>0){if(s.shakingBubbles>0&&m.flowEnergy>.001f){
                int steps=std::max(1,int(std::ceil(dt*120)));float h=dt/steps;
                for(int step=0;step<steps;step++){
                    Vec current=bubbleFlow.Current(p.position);
                    p.velocity=p.velocity+(current-p.velocity)*(1-std::exp(-h/.012f));
                    p.position=p.position+(up*rise+p.velocity)*h;
                    float maximumRadius=s.shotBubbles?std::min(s.radiusMax,p.baseRadius*1.15f):std::max(p.baseRadius,s.radiusMax);
                    float extent=maximumRadius*BubbleStretchLimit(maximumRadius);
                    float low=c.rings[0].x+extent+.001f,high=c.rings[c.ringCount-1].x-extent-.001f;
                    if(low>=high){++m.carbContain;p.position=previousPosition;p.velocity={};break;}
                    p.position.z=std::clamp(p.position.z,low,high);
                    float inner=std::min({Radius(c,p.position.z-extent),Radius(c,p.position.z),Radius(c,p.position.z+extent)})-extent-.001f;
                    float radial=CrossSection(c,p.position.x,p.position.y);
                    if(inner<=0){++m.carbContain;p.position=previousPosition;p.velocity={};break;}
                    if(radial>inner){Vec n=SectionNormal(c,p.position.x,p.position.y);p.position.x*=inner/radial;p.position.y*=inner/radial;p.velocity=p.velocity-n*std::max(0.f,Dot(p.velocity,n));}
                }
                if(p.age<=20)DeformBubble(p,bubbleFlow,p.speed*13,m.time,dt);
            }else {p.position=p.position+up*(rise*dt);p.velocity={};p.stretch=1;p.stretchVelocity=0;}}
            // Constrain the cavity in both quiet and agitated motion.
            const float ownRadius=std::min(s.radiusMax,p.baseRadius*1.15f);
            const float extent=s.shotBubbles?ownRadius*(s.shakingBubbles>0&&m.flowEnergy>.001f?BubbleStretchLimit(ownRadius):1.f):s.radiusMax*BubbleStretchLimit(s.radiusMax);
            float lo=c.rings[0].x+extent+.001f,hi=c.rings[c.ringCount-1].x-extent-.001f;
            if(lo<hi){
                p.position.z=std::clamp(p.position.z,lo,hi);
                float inner=std::min({Radius(c,p.position.z-extent),Radius(c,p.position.z),Radius(c,p.position.z+extent)})-extent-.001f;
                float radial=CrossSection(c,p.position.x,p.position.y);
                if(inner<=0){p.position=previousPosition;p.velocity={};++m.carbContain;}
                else if(radial>inner){p.position.x*=inner/radial;p.position.y*=inner/radial;p.velocity={};}
            }else {p.position=previousPosition;p.velocity={};++m.carbContain;}
            p.age+=dt;
            float grown=std::min(s.radiusMax,p.baseRadius*(1+.15f*std::min(p.age/3,1.f)));
            float birth=std::clamp(p.age/.2f,0.f,1.f);
            float surface=height-Dot(p.position,normal)+Wave(p.position,m.time*s.waveTimeScale,primaryWave,secondaryWave,s.waveFrequency,mediumWave,c);
            if(surface<=.65f*p.radius && RetainSurfaceBubble(s,m,p.position,p.radius)){++m.carbTransfer;p.age=21;p.radius=0;continue;}
            // Shrink near the free surface instead of abruptly removing a full ring.
            float shapeBound=s.shakingBubbles>0&&m.flowEnergy>.001f?BubbleStretchLimit(grown):1.f;
            float pop=s.fill>=1?1.f:std::clamp(surface/std::max(grown*shapeBound,.001f),0.f,1.f);
            if(s.foamStrength>0 && m.surfaceBubbles.size()<m.bubbleLimits.foam && surface>0)pop=1.f;
            // Rate-limit shrinking when a wave crosses a whole group.
            const float target=grown*(.4f+.6f*birth)*pop;
            if(target<p.radius){p.radius=std::max(target,p.radius-dt*grown/.15f);continue;}
            p.radius=grown*(.4f+.6f*birth)*pop;
        }
        std::erase_if(m.bubbles,[&](const auto& p){if(p.age>20/visc.rise){if(p.age<21/visc.rise)++m.carbExpiry;return true;}
            if(p.radius<.001f){++m.carbTiny;return true;}
            return false;});
        // Independent fixed slots: births never evict another category.
        const int cap=int(m.bubbleLimits.carbonation);
        if(!s.shotBubbles)m.credit+=dt*s.intensity*(s.baseline+s.movement*m.activity*(held?2.f:1.f));int spawn=std::min(cap-int(m.bubbles.size()),int(m.credit));m.credit-=std::floor(m.credit);
        std::uniform_real_distribution<float> random(0,1);
        auto sample=[&](){auto q=s.samples[std::min(s.samples.size()-1,size_t(random(m.random)*s.samples.size()))];return Vec{q.x,q.y,q.z};};
        if(s.shotBubbles){
            // Canonical tank axis, just above the bottom feed opening. Reject
            // uncovered sources rather than making bubbles appear in the air.
            const auto& cfg=s.shotConfig;
            const unsigned perShot=cfg.bigCount+cfg.smallCount;
            const unsigned births=std::min(24u,m.pendingShots*perShot);
            for(unsigned i=0;i<births&&m.bubbles.size()<m.bubbleLimits.carbonation;++i){
                const unsigned slot=i%perShot;const bool big=slot<cfg.bigCount;
                const unsigned groupIndex=big?slot:slot-cfg.bigCount;
                float angle=6.2831853f*groupIndex/std::max(1u,big?cfg.bigCount:cfg.smallCount);
                float lo=big?cfg.bigRadiusMin:cfg.smallRadiusMin,hi=big?cfg.bigRadiusMax:cfg.smallRadiusMax;
                float radius=lo+(hi-lo)*random(m.random);
                // Separate the larger spheres around the inlet plume so they do
                // not visually merge into one bubble. Small bubbles start below.
                float spread=big?.36f:.14f;if(!big)angle+=1.0471976f;
                Vec source{spread*std::cos(angle),spread*std::sin(angle),c.rings[0].x+(big?.85f:.45f)+.05f*(i%3)};
                float slow=big?cfg.bigSpeedMin:cfg.smallSpeedMin,fast=big?cfg.bigSpeedMax:cfg.smallSpeedMax;
                Particle particle{source,radius,slow+(fast-slow)*random(m.random),0,radius};
                if(valid(particle))m.bubbles.push_back(particle);
            }
            spawn=0;m.credit=0;
        }
        for(int i=0;i<spawn;i++){
            unsigned site=std::min(3u,unsigned(random(m.random)*4));
            // Recurring sites make trails. Re-select when tilting uncovers a site;
            // choose lower valid samples without a per-frame scan of the cavity.
            Particle source{m.bubbleSources[site],s.radiusMax,1,0};
            if(!m.bubbleSourceValid[site]||!valid(source)){
                m.bubbleSourceValid[site]=false;float lowest=1e30f;
                for(int candidate=0;candidate<12;candidate++){
                    Vec q=sample();Particle test{q,s.radiusMax,1,0};float level=Dot(q,up);
                    if(valid(test)&&level<lowest){lowest=level;m.bubbleSources[site]=q;m.bubbleSourceValid[site]=true;}
                }
            }
            for(int attempt=0;attempt<12;attempt++){
                bool trail=m.bubbleSourceValid[site]&&random(m.random)>(.15f+.7f*m.activity);
                Vec q=trail?m.bubbleSources[site]:sample();
                float size=.65f+.35f*random(m.random);
                float radius=s.radiusMax;
                Particle particle{q,radius*.4f,.7f+.6f*size,0,radius};
                if(valid(particle)){m.bubbles.push_back(particle);break;}
            }
        }
    }
    m.pendingShots=0; // Consume once, including dry/culled/zero-quota draws.
    timer.End(5);
    // Independent caps: 64 agitation + 64 foam + 128 carbonation = 256
    // Screen/distance submission budgets remain independently applied per category.
    c.shakingCount=int(m.shaking.size());c.surfaceCount=int(m.surfaceBubbles.size());c.carbonationCount=int(m.bubbles.size());
    c.bubbleCount=0;
    for(const auto& p:m.shaking){int k=c.bubbleCount++;c.bubbles[k]={p.position.x,p.position.y,p.position.z,p.radius};c.bubbleShape[k]={p.shapeAxis.x,p.shapeAxis.y,p.shapeAxis.z,p.stretch};}
    int surfaceIndex=0;
    for(const auto& p:m.surfaceBubbles){if(surfaceIndex<64){auto& packed=c.surfacePop[surfaceIndex/4];float phase=std::clamp((p.age-(SurfaceLifetime(s,p)-.12f))/.12f,0.f,1.f);switch(surfaceIndex%4){case 0:packed.x=phase;break;case 1:packed.y=phase;break;case 2:packed.z=phase;break;case 3:packed.w=phase;break;}}++surfaceIndex;int k=c.bubbleCount++;c.bubbles[k]={p.position.x,p.position.y,p.position.z,p.visibleRadius};c.bubbleShape[k]={normal.x,normal.y,normal.z,-.65f};}
    for(const auto& p:m.bubbles){int k=c.bubbleCount++;c.bubbles[k]={p.position.x,p.position.y,p.position.z,p.radius};if(s.shakingBubbles>0&&m.flowEnergy>.001f)c.bubbleShape[k]={p.shapeAxis.x,p.shapeAxis.y,p.shapeAxis.z,p.stretch};}
    if(c.gradientInfo.x>=2){
        float bottom=1e30f,radial=std::hypot(c.plane.x,c.plane.y);
        for(int i=0;i<c.ringCount;i++)bottom=std::min(bottom,c.plane.z*c.rings[i].x-radial*c.rings[i].y);
        c.gradientInfo.y=bottom;c.gradientInfo.z=1/std::max(c.plane.w-bottom,.001f);
    }
    ApplyDetailDistance(c,detail);
    timer.End(6);
    return c;
}
}

