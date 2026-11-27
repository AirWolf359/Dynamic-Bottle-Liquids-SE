#include <SKSE/SKSE.h>
#include <RE/Skyrim.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <intrin.h>
#include <spdlog/sinks/basic_file_sink.h>
#include "Simulation.h"
#include "BubbleMotion.h"
#include "TranslationMotion.h"
#include "SimulationSchedule.h"
#include "LiquidPose.h"
#include "BubbleBudgetRuntime.h"
#include "PerformanceUI.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <optional>
#include <span>
#include "DistanceTuning.h"
#include "FillHeightTuning.h"
#include "ProfileLookup.h"
#include "LayeredProfiles.h"
#include "TmdReflectionReplay.h"
#include "NativePointShadowAtlas.h"
#include "TmdShellCoverage.h"
#include "ProjectionLightChecks.h"
#include <chrono>
#ifdef DBL_DEVELOPMENT
#include <atomic>
#endif
#include <unordered_map>
#include <unordered_set>
#include <cctype>

namespace {
using Microsoft::WRL::ComPtr;
#include "LiquidTextureAnimation.h"
#include "LiquidTextureSlosh.h"
using nuka::Vec; using nuka::V4;
const std::filesystem::path root=L"Data/SKSE/Plugins/SkyrimBottlePrototype";
#include "VisualValues.h"
struct Profile { VisualValues visual; float designedFill=0;double designedHeight=0; nuka::Settings settings; V4 color; UINT indices; nuka::Constants distant{};
    float glassOpacity=.40f,nativeFrontGlassOpacity=0,liquidAlphaOverride=0,liquidPointLightScattering=1,liquidInteriorPointLightScattering=1;
    bool shieldWetParticles=false,liquidOnly=false,nativeGlassPrototype=false,nativeLiquidDepth=false;
    bool LiquidOpaque() const {return liquidAlphaOverride==1.f;}
    bool DirectNativeLiquid() const {return nativeGlassPrototype&&LiquidOpaque();}
    bool authoredGlass=false;
    bool nativeLiquidGlow=false; bool colorlessGlass=false; int refractionMip=0; float liquidTexture=0; bool liquidNativeUV=false,liquidTextureTint=false,liquidTextureUnmirrored=false; V4 emissionColor{}; float liquidEmission=0,glassRefractionScale=1; bool independentLiquidAnimation=false,bulkLiquidTexture=false; std::array<double,2> liquidScrollRate{}; V4 liquidTextureRect{0,0,1,1}; V4 rusticMapping{}; };
std::unordered_map<std::string,Profile,ProfileNameHash,std::equal_to<>> profiles;
struct Instance { LiquidTextureSlosh textureSlosh; LiquidTextureAnimation liquidScroll; RE::NiPointer<RE::BSGeometry> owner; nuka::Motion motion; BubbleMotion emission; TranslationMotion translation; Vec position{}; double time{}; uint64_t frame=~0ull,nativeGlassFrame=~0ull; nuka::Constants constants; bool distant=false; SimulationSchedule schedule; nuka::Constants previous{}; Vec observedUp{},observedRight{}; float previousClock=0;
    nuka::BubbleBudgetRequest bubbleRequest;
    nuka::BubbleLimits bubbleQuota{0,0,0};uint64_t bubbleSeen=~0ull;
    uint64_t projectionSeen=~0ull;float projectionDistance{};
    const RE::BSLight* projectionLight{};uint64_t projectionLightFrame=~0ull;
    Instance(){motion.bubbleLimits={0,0,0};}
    const Profile* restingProfile=nullptr;Vec restingUp{};V4 restingPlane{};
    float restingFill=-1;unsigned restingBuilds=0;
    LiquidPose lastPose;bool hasPose=false,suspended=false;uint64_t metricsFrame=~0ull;
};
// Shared seam for the fading and fully distant liquid pose.
V4 RestingPlane(Instance& instance,const Profile& p,Vec up){
    bool measured=dblui::Extensive(),rebuilt=false;auto begin=measured?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
    // Caller supplies the same normalized local gravity used by live simulation.
    // A fill edit invalidates instances; the shared profile geometry remains fixed.
    // Cache local gravity, not camera position or the last animated wave crest.
    if(instance.restingProfile!=&p||instance.restingFill!=p.settings.fill||
        up.x!=instance.restingUp.x||up.y!=instance.restingUp.y||up.z!=instance.restingUp.z){
        nuka::Motion settled{};settled.lastUp=up;
        instance.restingPlane=nuka::Simulate(p.settings,settled,{},up,0,false).plane;
        instance.restingProfile=&p;instance.restingFill=p.settings.fill;instance.restingUp=up;
        ++instance.restingBuilds;rebuilt=true;
    }
    if(measured)dblui::AddResting(!rebuilt,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
    return instance.restingPlane;
}
std::unordered_map<RE::BSGeometry*,Instance> instances;
// Previous-frame visibility gives all bottles a fair request before allocation,
// independent of draw order. New bottles start at zero until the next frame.
void AllocateBubbleQuotas(uint64_t currentFrame){
    static std::vector<nuka::BubbleBudgetRequest> requests;requests.clear();requests.reserve(instances.size());
    for(auto& [geometry,i]:instances){
        i.bubbleQuota={0,0,0};
        if(currentFrame>0&&i.bubbleSeen==currentFrame-1){auto r=i.bubbleRequest;r.destination=&i.bubbleQuota;requests.push_back(r);}
    }
    nuka::AllocateSceneBubbles(requests);
    for(auto& [geometry,i]:instances){
        nuka::ApplyBubbleLimits(i.motion,i.bubbleQuota);
        LimitCachedBubbles(i.constants,i.bubbleQuota);
        LimitCachedBubbles(i.previous,i.bubbleQuota);
    }
}
DistanceTuning distanceTuning;
uint64_t frame=0;
#ifdef DBL_SKYRIM_VR
constexpr bool vrStereo=true;  // Skyrim VR instanced stereo build
#else
constexpr bool vrStereo=false;
#endif
// VR trace budget: scales each bottle's projected size before the 24-96 step
// choice. 0 keeps the fixed 96 steps of the direct path (1.0.0.0 behaviour).
// In-game A/B (VR timing build, 22-28 bottles): x1.00 saved about 10% of DBL's
// draw time with no visible change; lower scales gave no further measurable gain.
float vrTraceBudget=1.f;
thread_local RE::BSRenderPass* active=nullptr;
thread_local Profile* profile=nullptr;
using GeometryFn=void(*)(RE::BSShader*,RE::BSRenderPass*,uint32_t);
GeometryFn oldSetup{},oldRestore{};
GeometryFn oldUtilitySetup{},oldUtilityRestore{};
thread_local const RE::BSRenderPass* utilityPass=nullptr;
using DrawFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT);
struct IndexedDrawHook {
    void* target{};
    DrawFn original{};
};
std::array<IndexedDrawHook,4> indexedDrawHooks;
thread_local DrawFn indexedDrawOriginal=nullptr;
void STDMETHODCALLTYPE oldDraw(ID3D11DeviceContext*,UINT,UINT,INT);
void STDMETHODCALLTYPE PassThroughDraw(ID3D11DeviceContext*,UINT,UINT,INT);
TmdReflectionReplay tmdReflections([](ID3D11DeviceContext* ctx,ID3D11CommandList* commands){
    dblui::GPUCost timing(ctx,dblui::GPUStage::TmdReflectionReplay);
    ctx->ExecuteCommandList(commands,TRUE);
});
thread_local RE::BSGeometry* tmdReflectionBody=nullptr;
thread_local const RE::BSLight* tmdReflectionPoint=nullptr;
using PresentFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
PresentFn oldPresent{};
ComPtr<ID3D11VertexShader> vs;
ComPtr<ID3D11PixelShader> ps,nativeDepthPS,shellOpticsPS,shellCoveragePS;
ComPtr<ID3D11Buffer> shellOpticsBuffer;
thread_local float shellBend=0; thread_local int shellMip=0;

ComPtr<ID3D11InputLayout> layout;
ComPtr<ID3D11Buffer> buffer;
ComPtr<ID3D11BlendState> blend;
ComPtr<ID3D11DepthStencilState> depth,liquidDepthWrite;

ComPtr<ID3D11RasterizerState> raster;
struct alignas(16) GPU { V4 eye,plane,wave,detail,color,glass; V4 rings[16]; V4 bounds,refraction,viewport; V4 bubbleInfo,bubbles[64]; V4 wetInfo,wetFilm[64]; V4 cavityPlanes[16]; V4 surfaceBubbles[64]; V4 surfacePop[16]; V4 tunedModes[3],tunedInfo; V4 rippleWeights{1,0,.4535961214f,.8912073601f}; V4 effectWeights[5]{{1,1,1,1},{1,1,1,1},{1,1,1,1},{1,1,1,1},{1,1,1,1}}; V4 appearance{.40f,0,0,0}; V4 viscousSurface{}; V4 authoredSpec{1,1,1,80}; V4 liquidMaterial{}; V4 liquidTextureRect{0,0,1,1}; V4 liquidAnimation{0,0,1,1}; V4 liquidEffects{}; V4 liquidIndependentAnimation{0,0,1,1}; V4 liquidTexturePlane{0,0,1,0}; V4 fittingPosition{},fittingColor{},fittingRows[3]{},emissionColor{},traceSettings{},visualLight{},visualSurface{},rusticMapping{}; };
static_assert(sizeof(GPU)==4480);
void rippleGPU(GPU& g,double seconds,float viscosity=0){
    constexpr double tau=6.283185307179586;
    const auto response=nuka::Viscosity(viscosity);
    // Fine reflection texture has its own clock and slopes. Scale both here;
    // broad-wave energy alone saturates and leaves thick liquids looking noisy.
    double phase=seconds*3*tau*response.flow;
    g.rippleWeights={float(std::cos(std::remainder(phase,tau))),float(std::sin(std::remainder(phase,tau))),float(std::cos(std::remainder(phase*1.37+1.1,tau))),float(std::sin(std::remainder(phase*1.37+1.1,tau)))};
    g.rippleWeights.x*=response.ripple;g.rippleWeights.y*=response.ripple;
    g.rippleWeights.z*=response.ripple;g.rippleWeights.w*=response.ripple;
}
void tuningGPU(GPU& g,const nuka::Constants& c){std::copy_n(c.tunedModes,3,g.tunedModes);g.tunedInfo=c.tunedInfo;}
void surfaceGPU(GPU& gpu,const nuka::Constants& c){
    gpu.bubbleInfo.y=float(std::min(c.surfaceCount,64));std::copy_n(c.surfacePop,16,gpu.surfacePop);
    std::copy_n(c.bubbles+c.shakingCount,int(gpu.bubbleInfo.y),gpu.surfaceBubbles);
}
uint64_t bubbleBudgetFrame=~0ull;
RE::NiPointer<RE::NiAVObject> grabbedBottleRoot;
float bubblePixelScale=1024;
void BeginBubbleFrame(ID3D11DeviceContext* ctx){
    if(bubbleBudgetFrame==frame)return;bubbleBudgetFrame=frame;AllocateBubbleQuotas(frame);
    grabbedBottleRoot.reset();if(auto* player=RE::PlayerCharacter::GetSingleton())if(auto grabbed=player->GetGrabbedRef())grabbedBottleRoot.reset(grabbed->Get3D());
    UINT count=1;D3D11_VIEWPORT viewport{};ctx->RSGetViewports(&count,&viewport);
    float fov=75;if(auto* camera=RE::PlayerCamera::GetSingleton())if(float worldFOV=camera->GetRuntimeData().worldFOV;std::isfinite(worldFOV)&&worldFOV>1&&worldFOV<179)fov=worldFOV;
    // VR draws both eyes side by side in one viewport; each eye gets half the width.
    bubblePixelScale=count==1?viewport.Width*(vrStereo?.5f:1.f)/(2*std::tan(fov*.00872664626f)):1024;
    if(!dblui::Extensive())return;
    static double nextReport=0;auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if(seconds>=nextReport){nextReport=seconds+5;unsigned reserved=0,live=0,packed=0;
        for(auto& [g,i]:instances){reserved+=i.bubbleQuota.Total();live+=unsigned(i.motion.shaking.size()+i.motion.surfaceBubbles.size()+i.motion.bubbles.size());packed+=unsigned(i.constants.bubbleCount);}
        SKSE::log::info("[DBL-bubble-budget] maximum=1280 reserved={} retained={} cached={} tracked={}",reserved,live,packed,instances.size());
    }
}
bool InBottleTree(const RE::NiAVObject* object,const RE::NiAVObject* root){
    if(!root)return false;
    for(unsigned depth=0;object&&depth<128;object=object->parent,++depth)if(object==root)return true;
    return false;
}
bool GrabbedBottle(const RE::NiAVObject* object){return InBottleTree(object,grabbedBottleRoot.get());}

void check(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error(std::format("D3D HRESULT {:08X}",uint32_t(hr))); }
#include "RippleTexture.h"
RippleTexture rippleTexture;
#include "RefractionCamera.inc"
#include "SharedBlur.inc"
#include "Refraction.inc"
#include "OpticalBuffers.inc"
#include "BottleProjection.inc"
#include "NativeOpaqueShadow.inc"
double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
Vec localDirection(const RE::NiTransform& t,Vec p) {
    auto& r=t.rotate.entry;
    return {r[0][0]*p.x+r[1][0]*p.y+r[2][0]*p.z,r[0][1]*p.x+r[1][1]*p.y+r[2][1]*p.z,r[0][2]*p.x+r[1][2]*p.y+r[2][2]*p.z};
}

#include "AcceptedSettings.h"
#include "LateOptics.inc"
bool MeadSurfaceProfile(std::string_view name){
    if(name=="DBL_SSE_nf_1e15df143487ebfe_0_v100_vis2"||
       name=="DBL_SSE_nf_451de57115aaac8d_0_vis2")return true;
    if(name.starts_with("DBL_SSE_XMead_"))
        return name!="DBL_SSE_XMead_Ale"&&name!="DBL_SSE_XMead_Argonian";
    if(name.starts_with("DBL_SSE_MakeWineExtra_"))return name.find("Mead")!=name.npos;
    return (name.starts_with("DBL_SSE_MedievalSpirits_")||name.starts_with("DBL_SSE_VergisOpenBar_"))&&
        (name.ends_with("mead01")||name.ends_with("meadblackbriar")||
         name.ends_with("meadhonningbrew01")||name.ends_with("blackbriarprivatereserve"));
}
void loadProfiles(const std::filesystem::path& path=root/"profiles.json",bool previewTuning=true) {
    const auto documents=dblconfig::ReadProfiles(path);const auto& j=documents.values;
    decltype(profiles) loaded;
    for(auto& item:j) {
        const auto name=item.at("name").get<std::string>();
        try{
        Profile p{};p.refractionMip=std::clamp(item.value("refraction_mip",item.value("downsample_refraction",false)?2:0),0,4); auto& s=p.settings;s.previewTuning=previewTuning;
        s.base.viscousSurface.w=name.starts_with("DBL_SSE_Skyking_")?.5f:0.f;
        s.viscosity=item.value("viscosity",0.f);
        if(!std::isfinite(s.viscosity)||s.viscosity<0||s.viscosity>100)throw std::runtime_error("Invalid viscosity");
        s.fill=item.at("fill"); p.designedFill=s.fill; s.frequency=1.6f; s.damping=.24f; s.gain=1.2f; s.maxTilt=.4f;
        s.waveAmplitude=.28f; s.waveFrequency=.65f; s.settle=.7f; s.shakingBubbles=1; s.base.wetTuning.w=1.2f;
        s.foamStrength=1;s.intensity=1;s.surfaceLifetimeJitter=1.f;
        s.surfaceLifetime=MeadSurfaceProfile(name)?7.f:2.f;
        const auto& rings=item.at("rings");
        if(!rings.is_array()||rings.size()<2||rings.size()>16)throw std::runtime_error("Invalid rings");
        s.base.ringCount=int(rings.size());
        for(int i=0;i<s.base.ringCount;i++){
            const auto& ring=rings.at(i);
            s.base.rings[i]={ring.at(0),ring.at(1),0,0};
        }
        // Binary profile lookup requires strictly ascending finite ring heights.
        for(int i=0;i<s.base.ringCount;++i)if(!std::isfinite(s.base.rings[i].x)||!std::isfinite(s.base.rings[i].y)||s.base.rings[i].y<0||(i&&s.base.rings[i].x<=s.base.rings[i-1].x))
            throw std::runtime_error("Invalid cavity profile: rings must be finite, nonnegative and strictly height-sorted");
        // Fallout uses ring zero z as the cavity radius for bubble size limits.
        for(int i=0;i<s.base.ringCount;++i)s.base.rings[0].z=std::max(s.base.rings[0].z,s.base.rings[i].y);
        if(item.contains("cavity_shape")){
            const auto& shape=item.at("cavity_shape");const auto& planes=shape.at("planes");
            if(!planes.is_array()||planes.size()<3||planes.size()>16)throw std::runtime_error("Invalid cavity polygon");
            for(size_t i=0;i<planes.size();++i){
                const auto& plane=planes.at(i);
                s.base.cavityPlanes[i]={plane.at(0),plane.at(1),0,0};
            }
            s.base.cavityPlanes[0].z=shape.at("radius_scale");s.base.cavityPlanes[0].w=float(planes.size());
        }
        // Roughly one primary crest/trough across the body, with proportional height.
        s.waveAmplitude=.09f*s.base.rings[0].z;s.surfaceImpactBoost=.6f;s.mediumWaveAmplitude=.055f*s.base.rings[0].z;
        s.waveFrequency=2.6f/std::max(s.base.rings[0].z,.1f);
        nuka::MakeSamples(s);
        if(previewTuning){nuka::BuildCavityEdges(s);s.maxTilt=.5f;}
        // First-pass size response: linear dimension scales with volume^(1/3),
        // and the time scale with its square root. This is tuning, not a mass solver.
        double volume=0;for(const auto& sample:s.samples)volume+=sample.w;
        float responseScale=std::clamp(float(std::pow(std::max(volume*s.fill,1.0)/200.0,1.0/6.0)),.8f,1.6f);
        s.frequency=1.25f/responseScale;s.damping=.11f;
        s.broadWaveDecay=2.0f*responseScale;s.waveTimeScale=.85f/responseScale;
        const auto& color=item.at("color");
        p.color={color.at(0),color.at(1),color.at(2),item.value("extinction",.32f)}; p.indices=item.at("indices");
        p.glassOpacity=item.value("glass_opacity",.40f);p.liquidOnly=item.value("liquid_only",false);
        p.nativeGlassPrototype=item.value("native_glass_prototype",false);
        p.nativeLiquidDepth=item.value("native_liquid_depth",false);
        p.authoredGlass=item.value("authored_glass",false);
        if(p.authoredGlass&&!p.nativeGlassPrototype)throw std::runtime_error("Authored glass requires native glass");
        p.shieldWetParticles=p.nativeGlassPrototype&&!p.authoredGlass&&name.starts_with("DBL_SSE_TMDWinery_");
        if(p.nativeGlassPrototype&&!p.liquidOnly)throw std::runtime_error("Native glass requires liquid-only rendering");
        auto liquidOpacity=item.value("liquid_opacity",0.f);
        if(!std::isfinite(liquidOpacity)||liquidOpacity<0||liquidOpacity>1)throw std::runtime_error("Invalid liquid opacity");
        p.colorlessGlass=item.value("colorless_glass",false);
        p.liquidTexture=item.value("liquid_texture_strength",0.f);
        p.nativeLiquidGlow=item.value("native_liquid_glow",false);
        p.liquidNativeUV=item.value("liquid_texture_native_uv",false);
        p.bulkLiquidTexture=item.value("liquid_texture_bulk_slosh",false);
        if(item.contains("liquid_texture_scroll_rate")){
            const auto& rate=item.at("liquid_texture_scroll_rate");
            if(!rate.is_array()||rate.size()!=2)throw std::runtime_error("Liquid scroll rate requires two numbers");
            p.liquidScrollRate={rate[0].get<double>(),rate[1].get<double>()};
            if(!p.liquidNativeUV||!std::isfinite(p.liquidScrollRate[0])||!std::isfinite(p.liquidScrollRate[1]))throw std::runtime_error("Independent liquid scroll requires native UV and finite rates");
            p.independentLiquidAnimation=true;
        }
        p.liquidTextureTint=item.value("liquid_texture_tint",false);
        p.liquidTextureUnmirrored=item.value("liquid_texture_unmirrored",false);
        if(item.contains("liquid_emission_color")){
            auto rgb=item.at("liquid_emission_color").get<std::array<float,3>>();
            for(float v:rgb)if(!std::isfinite(v)||v<0)throw std::runtime_error("Invalid liquid emission color");
            p.emissionColor={rgb[0],rgb[1],rgb[2],1};
        }
        p.liquidEmission=item.value("liquid_emission",0.f);
        p.liquidPointLightScattering=item.value("liquid_point_light_scattering",1.f);
        p.liquidInteriorPointLightScattering=item.value("liquid_interior_point_light_scattering",p.liquidPointLightScattering);
        p.glassRefractionScale=p.shieldWetParticles?0.f:item.value("glass_refraction_scale",1.f);
        if(!std::isfinite(p.glassRefractionScale)||p.glassRefractionScale<0||p.glassRefractionScale>8)throw std::runtime_error("Glass refraction scale must be in [0,8]");
        if(!std::isfinite(p.liquidEmission)||p.liquidEmission<0||p.liquidEmission>4)throw std::runtime_error("Liquid emission must be in [0,4]");
        if(!std::isfinite(p.liquidPointLightScattering)||p.liquidPointLightScattering<0||p.liquidPointLightScattering>1)throw std::runtime_error("Liquid point-light scattering must be in [0,1]");
        if(!std::isfinite(p.liquidInteriorPointLightScattering)||p.liquidInteriorPointLightScattering<0||p.liquidInteriorPointLightScattering>1)throw std::runtime_error("Interior liquid point-light scattering must be in [0,1]");
        if(p.liquidNativeUV&&p.liquidTexture<=0)throw std::runtime_error("Native liquid animation requires textured liquid");
        if(!std::isfinite(p.liquidTexture)||p.liquidTexture<0||p.liquidTexture>1||(p.liquidTexture>0&&!p.colorlessGlass))throw std::runtime_error("Liquid texture requires colorless glass and strength in [0,1]");
        if(item.contains("liquid_texture_rect")){
            const auto& r=item.at("liquid_texture_rect");if(!r.is_array()||r.size()!=4)throw std::runtime_error("Liquid texture rectangle requires four values");
            p.liquidTextureRect={r[0],r[1],r[2],r[3]};const auto& t=p.liquidTextureRect;
            if(!std::isfinite(t.x)||!std::isfinite(t.y)||!std::isfinite(t.z)||!std::isfinite(t.w)||t.x<0||t.y<0||t.z>1||t.w>1||t.z<=t.x||t.w<=t.y)throw std::runtime_error("Invalid liquid texture rectangle");
        }
        if(p.nativeLiquidGlow&&name.starts_with("DBL_SSE_Rustic_")&&item.contains("rustic_liquid_uv_bounds")){
            const auto uv=item.at("rustic_liquid_uv_bounds").get<std::array<float,6>>();
            if(!std::all_of(uv.begin(),uv.end(),[](float n){return std::isfinite(n);})||
               uv[2]<=uv[0]||uv[3]<=uv[1]||uv[5]<=uv[4])throw std::runtime_error("Invalid RUSTIC liquid UV bounds");
            p.liquidTextureRect={uv[0],uv[1],uv[2],uv[3]};
            p.rusticMapping={uv[4],uv[5],1,0};
        }
        p.nativeFrontGlassOpacity=item.value("native_front_glass_opacity",0.f);
        if(!std::isfinite(p.nativeFrontGlassOpacity)||p.nativeFrontGlassOpacity<0||p.nativeFrontGlassOpacity>1||
           (p.nativeFrontGlassOpacity>0&&!p.nativeGlassPrototype))
            throw std::runtime_error("Native front-glass opacity requires native glass and a value in [0,1]");
        // These native families author fixed liquid coverage. Other translucent
        // profiles retain the existing thickness-based absorption curve.
        const bool fixedNativeCoverage=p.nativeGlassPrototype&&
            (p.authoredGlass||p.shieldWetParticles||name.starts_with("DBL_SSE_HPP_"));
        if(liquidOpacity==1.f||fixedNativeCoverage)
            p.liquidAlphaOverride=liquidOpacity;
        s.shakingBubbles=item.value("agitation",1.f);s.foamStrength=s.shakingBubbles;
        if(item.contains("carbonation")){
            float rate=item.at("carbonation").get<float>();
            if(!std::isfinite(rate)||rate<0||rate>8)throw std::runtime_error("Invalid carbonation rate");
            s.baseline=rate;s.movement=rate*.5f;
            s.radiusMin=s.radiusMax=nuka::CarbonationRadius(s.viscosity);s.rise=2.f;
        }
        if(!std::isfinite(p.color.w)||p.color.w<0||!std::isfinite(p.glassOpacity)||p.glassOpacity<0||p.glassOpacity>1||
           !std::isfinite(s.shakingBubbles)||s.shakingBubbles<0||s.shakingBubbles>1||!std::isfinite(s.fill)||s.fill<=0||s.fill>1)
            throw std::runtime_error("Invalid bottle appearance");
        p.designedHeight=FillHeightTuning::Height(s.base,p.designedFill);
        loaded.emplace(name,std::move(p));
        }catch(const std::exception& e){throw std::runtime_error(documents.source.at(name).string()+" ["+name+"]: "+e.what());}
    }
    if(previewTuning){auto wine=loaded.find("DBL_SSE_wine");if(wine==loaded.end())throw std::runtime_error("Alto Wine reference profile missing");
        auto reference=nuka::MeasureSurface(wine->second.settings,{0,0,1},.72f);
        for(auto& [name,p]:loaded)p.settings.referenceSurface=reference;
    }
    for(auto& [name,p]:loaded){nuka::Motion m{};m.lastUp={0,0,1};p.distant=nuka::Simulate(p.settings,m,{}, {0,0,1},0,false);}
    profiles.swap(loaded);
    SKSE::log::info("[DBL-config] {} profiles from {} files; {} named overrides; startup-only load",profiles.size(),documents.files.size(),documents.overrides);
    for(const auto& file:documents.files)SKSE::log::info("[DBL-config] loaded {}",file.string());
}
#ifndef DBL_FIXED_SETTINGS
void ApplyFillHeightOffset(float points){
    for(auto& [name,p]:profiles){
        p.settings.fill=FillHeightTuning::Adjust(p.settings.base,p.designedFill,p.designedHeight,points);
        nuka::Motion settled{};settled.lastUp={0,0,1};
        p.distant=nuka::Simulate(p.settings,settled,{}, {0,0,1},0,false);
    }
    // A deliberate fill edit starts fresh poses, including frozen/distant ones.
    // Shared cavity/meniscus geometry caches in the profiles remain intact.
    instances.clear();
}
#endif
#include "VisualTuning.inc"
void initialize(ID3D11Device* device,const std::filesystem::path& shaderPath=root/"Bottle.hlsl") {
    rippleTexture.Create(device);
    ComPtr<ID3DBlob> v,p,e;
    auto compile=[&](const char* entry,const char* target,ComPtr<ID3DBlob>& code) {
        auto file=shaderPath.parent_path()/(std::string(target[0]=='v'?"BottleVertex":"BottleImmediate")+(vrStereo?"VR.cso":".cso"));
        if(FAILED(D3DReadFileToBlob(file.c_str(),&code)))throw std::runtime_error("Missing precompiled bottle shader: "+file.string());
    };
    compile("VSMain","vs_5_0",v); compile("PSMain","ps_5_0",p);
    check(device->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs));
    check(device->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps));
    ComPtr<ID3DBlob> nativeCode;check(D3DReadFileToBlob((shaderPath.parent_path()/(vrStereo?"BottleNativeLiquidVR.cso":"BottleNativeLiquid.cso")).c_str(),&nativeCode));
    check(device->CreatePixelShader(nativeCode->GetBufferPointer(),nativeCode->GetBufferSize(),nullptr,&nativeDepthPS));
    D3D11_INPUT_ELEMENT_DESC input[]={
        {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,20,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R16G16_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TANGENT",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,24,D3D11_INPUT_PER_VERTEX_DATA,0}};
    check(device->CreateInputLayout(input,4,v->GetBufferPointer(),v->GetBufferSize(),&layout));
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth=sizeof(GPU); bd.Usage=D3D11_USAGE_DYNAMIC; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    check(device->CreateBuffer(&bd,nullptr,&buffer));
    D3D11_BLEND_DESC b{}; auto& rt=b.RenderTarget[0]; rt.BlendEnable=true; rt.SrcBlend=D3D11_BLEND_SRC_ALPHA; rt.DestBlend=D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp=D3D11_BLEND_OP_ADD; rt.SrcBlendAlpha=D3D11_BLEND_ONE; rt.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha=D3D11_BLEND_OP_ADD; rt.RenderTargetWriteMask=15;
    // Only color is ours; preserve native motion vectors and normals.
    b.IndependentBlendEnable=true;
    for(int i=1;i<8;i++){b.RenderTarget[i]=rt;b.RenderTarget[i].BlendEnable=false;b.RenderTarget[i].RenderTargetWriteMask=0;}
    check(device->CreateBlendState(&b,&blend));
    D3D11_DEPTH_STENCIL_DESC d{}; d.DepthEnable=true; d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO; d.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
    check(device->CreateDepthStencilState(&d,&depth));
    d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;check(device->CreateDepthStencilState(&d,&liquidDepthWrite));
    D3D11_RASTERIZER_DESC r{}; r.FillMode=D3D11_FILL_SOLID; r.CullMode=D3D11_CULL_NONE; r.DepthClipEnable=true;
    check(device->CreateRasterizerState(&r,&raster));
    ComPtr<ID3DBlob> shellCode;
    check(D3DReadFileToBlob((shaderPath.parent_path()/"TmdShellOptics.cso").c_str(),&shellCode));
    check(device->CreatePixelShader(shellCode->GetBufferPointer(),shellCode->GetBufferSize(),nullptr,&shellOpticsPS));
    shellCode.Reset();
    check(D3DReadFileToBlob((shaderPath.parent_path()/"TmdShellCoverage.cso").c_str(),&shellCode));
    check(device->CreatePixelShader(shellCode->GetBufferPointer(),shellCode->GetBufferSize(),nullptr,&shellCoveragePS));
    try{
        bottleProjection.Initialize(device,shaderPath.parent_path());
    }catch(const std::exception& e){
        SKSE::log::warn("[DBL-bottle-projection] Preview unavailable: {}",e.what());
    }
    D3D11_BUFFER_DESC shellDesc{};shellDesc.ByteWidth=sizeof(TmdShellConstants);shellDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    check(device->CreateBuffer(&shellDesc,nullptr,&shellOpticsBuffer));
    opticalBuffers.initialize(device,shaderPath);
}
bool IsMainSceneTarget(ID3D11RenderTargetView* view){
    auto* renderer=RE::BSGraphics::Renderer::GetRendererData();
    if(!renderer||!view)return false;
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    return resource&&resource.Get()==renderer->renderTargets[RE::RENDER_TARGET::kMAIN].texture;
}
bool IsHdrBottleWorldLayout(ID3D11RenderTargetView* const (&targets)[8]){
    if(!targets[0]||!targets[1]||!targets[2]||
       std::any_of(targets+3,targets+8,[](auto* target){return target!=nullptr;}))return false;
    const auto* renderer=RE::BSGraphics::Renderer::GetRendererData();
    if(!renderer)return false;
    constexpr RE::RENDER_TARGET slots[]={RE::RENDER_TARGET::kMAIN,RE::RENDER_TARGET::kTEMPORAL_AA_MASK,
        RE::RENDER_TARGET::kMAIN_ONLY_ALPHA};
    constexpr DXGI_FORMAT formats[]={DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R8G8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT};
    for(int i=0;i<3;++i){
        D3D11_RENDER_TARGET_VIEW_DESC desc{};
        targets[i]->GetDesc(&desc);
        if(desc.Format!=formats[i]||desc.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D||desc.Texture2D.MipSlice)return false;
        ComPtr<ID3D11Resource> resource;
        targets[i]->GetResource(&resource);
        if(resource.Get()!=renderer->renderTargets[slots[i]].texture)return false;
    }
    return true;
}
#include "BottleGlassDepth.inc"
void DrawNativeBottleGlass(ID3D11DeviceContext* ctx,UINT count,UINT start,INT base){
    BottleGlassDepthRequest depthRequest(ctx);
    if(profile&&profile->nativeGlassPrototype&&count==profile->indices&&
       IsNativeGlassColorPass(ctx)&&UsesBottledWorldGlassDepth(ctx))
        depthRequest.normalize();
    oldDraw(ctx,count,start,base);
}
bool supportedTargets(ID3D11DeviceContext* ctx,D3D11_RENDER_TARGET_VIEW_DESC* primary=nullptr,
                      ID3D11RenderTargetView** primaryView=nullptr,bool retainPrimaryForDeferred=false,
                      bool* hdrWorldLayout=nullptr) {
    ID3D11RenderTargetView* targets[8]{}; ctx->OMGetRenderTargets(8,targets,nullptr);
    if(primary&&targets[0])targets[0]->GetDesc(primary);
    bool single=targets[0]!=nullptr; for(int i=1;i<8;i++)if(targets[i])single=false;
    bool world=targets[0]&&targets[1]&&targets[2];
    const DXGI_FORMAT expected[]={DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_R8G8_UNORM,DXGI_FORMAT_R11G11B10_FLOAT};
    for(int i=0;i<8;i++){
        if(i>=3){if(targets[i])world=false;}
        else if(targets[i]){D3D11_RENDER_TARGET_VIEW_DESC d{};targets[i]->GetDesc(&d);const bool mainHdr=i==0&&d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT&&IsMainSceneTarget(targets[0]);if((d.Format!=expected[i]&&!mainHdr)||d.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D)world=false;}
    }
    const bool hdrWorld=!single&&!world&&hdrWorldLayout&&IsHdrBottleWorldLayout(targets);
    if(hdrWorldLayout)*hdrWorldLayout=hdrWorld;
    if(primaryView&&(single||world||retainPrimaryForDeferred||hdrWorld)){*primaryView=targets[0];targets[0]=nullptr;}
    for(auto* t:targets)if(t)t->Release();
    return single||world;
}
#ifdef DBL_DEVELOPMENT
bool medievalTrace=false;
bool jessicaTrace=false;
void TraceMedieval(const RE::BSGeometry* geometry,std::string_view stage,ID3D11DeviceContext* ctx=nullptr,
                   UINT count=0,UINT stride=0,bool geometryCB=false,bool cameraCB=false){
    if(!medievalTrace||!geometry)return;
    const std::string_view name=geometry->name.c_str();
    if(!name.starts_with("DBL_SSE_MedievalSpirits_"))return;
    static thread_local std::unordered_set<std::string> seen;
    if(!seen.emplace(std::format("{}/{}",name,stage)).second)return;
    if(!ctx){
        SKSE::log::info("[DBL-medieval-trace] shape={} stage={}",name,stage);
        return;
    }
    ComPtr<ID3D11RenderTargetView> color;
    ComPtr<ID3D11DepthStencilView> depthView;
    ctx->OMGetRenderTargets(1,&color,&depthView);
    D3D11_RENDER_TARGET_VIEW_DESC colorDesc{};
    D3D11_DEPTH_STENCIL_VIEW_DESC depthDesc{};
    if(color)color->GetDesc(&colorDesc);
    if(depthView)depthView->GetDesc(&depthDesc);
    SKSE::log::info("[DBL-medieval-trace] shape={} stage={} indices={} stride={} geometry_cb={} camera_cb={} main={} color_format={} depth_format={}",
        name,stage,count,stride,geometryCB,cameraCB,IsMainSceneTarget(color.Get()),unsigned(colorDesc.Format),unsigned(depthDesc.Format));
}
void TraceJessica(const RE::BSGeometry* geometry,std::string_view stage,ID3D11DeviceContext* ctx=nullptr,
                  UINT count=0,UINT stride=0){
    if(!jessicaTrace||!geometry||geometry->name!="DBL_SSE_TMDWinery_winebottle01jessica")return;
    const bool held=GrabbedBottle(geometry);
    if(!ctx){
        static thread_local std::unordered_set<std::string_view> seen[2];
        if(seen[held].emplace(stage).second)
            SKSE::log::info("[DBL-jessica] frame={} stage={} held={}",frame,stage,held);
        return;
    }
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DepthStencilView> depthView;
    ComPtr<ID3D11BlendState> blendState;
    ctx->OMGetRenderTargets(1,&target,&depthView);
    ctx->OMGetBlendState(&blendState,nullptr,nullptr);
    D3D11_RENDER_TARGET_VIEW_DESC targetDesc{};
    D3D11_DEPTH_STENCIL_VIEW_DESC depthDesc{};
    D3D11_BLEND_DESC blendDesc{};
    if(target)target->GetDesc(&targetDesc);
    if(depthView)depthView->GetDesc(&depthDesc);
    if(blendState)blendState->GetDesc(&blendDesc);
    const auto& color=blendDesc.RenderTarget[0];
    const auto state=std::format("held={} main={} color={} depth={} blend={} src={} dst={} write={} indices={} stride={}",
        held,IsMainSceneTarget(target.Get()),unsigned(targetDesc.Format),unsigned(depthDesc.Format),
        bool(color.BlendEnable),unsigned(color.SrcBlend),unsigned(color.DestBlend),
        unsigned(color.RenderTargetWriteMask),count,stride);
    static thread_local std::unordered_set<std::string> seen;
    if(seen.emplace(std::format("{}/{}",stage,state)).second)
        SKSE::log::info("[DBL-jessica] frame={} stage={} {}",frame,stage,state);
}
#endif
GPU BuildLiquidGPU(Instance& inst,const Profile* profile,Vec bottleUp,bool outsideSimulation,bool resuming,float detailWeight,float effectWeight,const std::array<float,20>& effectWeights){
    GPU gpu{}; const auto& c=inst.constants;LiquidPose pose;pose.Capture(c,inst.motion.time);
    float renderClock=inst.motion.time;
    if(!inst.distant&&!outsideSimulation&&!resuming&&inst.schedule.interval>0){
        float a=inst.schedule.Alpha();auto blend=[&](V4 x,V4 y){return V4{std::lerp(x.x,y.x,a),std::lerp(x.y,y.y,a),std::lerp(x.z,y.z,a),std::lerp(x.w,y.w,a)};};
        pose.plane=blend(inst.previous.plane,pose.plane);pose.wave=blend(inst.previous.wave,pose.wave);pose.detail=blend(inst.previous.waveDetail,pose.detail);
        for(int k=0;k<3;++k){auto target=pose.modes[k];pose.modes[k]=blend(inst.previous.tunedModes[k],target);pose.modes[k].w=SimulationSchedule::Phase(inst.previous.tunedModes[k].w,target.w,a);}
        renderClock=std::lerp(inst.previousClock,inst.motion.time,a);
    }
    if(outsideSimulation||resuming){pose=inst.lastPose;renderClock=pose.clock;}
    else if(!inst.distant){pose.clock=renderClock;inst.lastPose=pose;inst.hasPose=true;}
    if(effectWeight<=1){
        const auto resting=effectWeights[5]<1?RestingPlane(inst,*profile,bottleUp):pose.plane;
        const float weight=1-effectWeights[5];
        pose.plane={std::lerp(pose.plane.x,resting.x,weight),std::lerp(pose.plane.y,resting.y,weight),
                    std::lerp(pose.plane.z,resting.z,weight),std::lerp(pose.plane.w,resting.w,weight)};
        pose.wave.x*=effectWeights[6];pose.detail.x*=effectWeights[6];
    }
    pose.wave.w*=effectWeights[6];
    for(int k=0;k<20;++k)reinterpret_cast<float*>(gpu.effectWeights)[k]=effectWeights[k];
    gpu.eye.x=detailWeight;gpu.eye.y=effectWeight;gpu.eye.z=-12345;
    gpu.effectWeights[4].y=distanceTuning.baseGlass?effectWeights[17]:0.f;gpu.effectWeights[4].z=distanceTuning.fresnel?effectWeights[18]:0.f;gpu.effectWeights[4].w=distanceTuning.liquidTrace?effectWeights[19]:0.f;
    std::copy_n(pose.modes,3,gpu.tunedModes);gpu.tunedInfo=c.tunedInfo;gpu.tunedInfo.w=0;rippleGPU(gpu,renderClock,profile->settings.viscosity);
    gpu.viscousSurface=c.viscousSurface;gpu.viscousSurface.z=0;gpu.wetInfo=c.wetInfo;std::copy_n(c.wetFilm,64,gpu.wetFilm);
    // Reserved wet-film channel: water=0, 30x viscosity and above=1.
    gpu.wetInfo.w=std::clamp(std::log2(std::max(profile->settings.viscosity,1.f))/std::log2(30.f),0.f,1.f);
    gpu.bubbleInfo.y=float(std::min({c.surfaceCount,int(inst.bubbleQuota.foam),64}));
    std::copy_n(c.surfacePop,16,gpu.surfacePop);
    std::copy_n(c.bubbles+c.shakingCount,int(gpu.bubbleInfo.y),gpu.surfaceBubbles);
    gpu.bubbleInfo.x=float(std::min({c.shakingCount,int(inst.bubbleQuota.shaking),64}));
    std::copy_n(c.bubbles,int(gpu.bubbleInfo.x),gpu.bubbles);
    int carbonation=std::min({c.carbonationCount,int(inst.bubbleQuota.carbonation),64-int(gpu.bubbleInfo.x)});
    std::copy_n(c.bubbles+c.shakingCount+c.surfaceCount,carbonation,gpu.bubbles+int(gpu.bubbleInfo.x));
    gpu.bubbleInfo.x+=float(carbonation);
    gpu.plane=pose.plane;gpu.wave=pose.wave;gpu.detail=pose.detail;gpu.color=profile->color;gpu.glass={.35f,.48f,.38f,0};
    gpu.liquidMaterial={profile->colorlessGlass?1.f:0.f,profile->liquidTexture,float(profile->designedHeight),float((profile->liquidNativeUV?1:0)|(profile->liquidTextureTint?2:0)|(profile->liquidTextureUnmirrored?4:0)|(profile->independentLiquidAnimation?8:0)|(profile->bulkLiquidTexture?16:0))};
    gpu.traceSettings.x=float(dblui::TraceSampleCap());
    gpu.emissionColor=profile->emissionColor;
    gpu.liquidEffects={profile->liquidEmission,profile->glassRefractionScale-1,float(profile->refractionMip),1-profile->liquidPointLightScattering};
    if(profile->refractionMip>0&&!profile->DirectNativeLiquid())downsampleRequestedFrame=frame;
    gpu.liquidTextureRect=profile->liquidTextureRect;
    gpu.rusticMapping=profile->rusticMapping;
    if(profile->colorlessGlass)gpu.glass={.40f,.40f,.40f,0};
    const float glassOpacity=profile->liquidOnly?profile->nativeFrontGlassOpacity:profile->glassOpacity;
    gpu.appearance={profile->shieldWetParticles?0.f:glassOpacity,profile->liquidOnly?1.f:0.f,profile->liquidAlphaOverride,profile->LiquidOpaque()?1.f:0.f};
    const auto& v=profile->visual;
    gpu.visualLight=v.light;gpu.visualSurface=v.surface;
    if(v.samples)gpu.traceSettings.x=float(v.samples);
    gpu.traceSettings.y=float(v.straight>=0?v.straight:profile->refractionMip);
    gpu.traceSettings.z=float(v.bent?v.bent:(profile->refractionMip?profile->refractionMip+1:0));
    gpu.traceSettings.w=0;
    if((v.straight>0||v.bent)&&!profile->DirectNativeLiquid())downsampleRequestedFrame=frame;
    gpu.viscousSurface.w=(1+gpu.viscousSurface.w)*v.meniscus-1;
    gpu.effectWeights[1].x*=v.wet;gpu.effectWeights[1].w*=v.ripple;
    std::copy_n(c.cavityPlanes,16,gpu.cavityPlanes);std::copy_n(c.rings,16,gpu.rings); float radius=0; for(int i=0;i<c.ringCount;i++)radius=std::max(radius,c.rings[i].y);
    gpu.bounds={c.rings[0].x,c.rings[c.ringCount-1].x,radius,float(c.ringCount)};
    return gpu;
}
bool UseNativeEnvironment(RE::BSShaderMaterial::Feature feature,bool cubeBound) {
    // Unused D3D slots retain earlier draws' resources. A bound cube alone
    // does not mean the current material authored environment reflections.
    using F=RE::BSShaderMaterial::Feature;
    return cubeBound&&(feature==F::kEnvironmentMap||feature==F::kMultilayerParallax||feature==F::kEye);
}
#include "BottleHookState.h"
#include "NativeGlassBinding.h"
#include "NativeGlassOrder.h"
#include "LiquidDrawDistance.h"
struct FittingSample {
    ComPtr<ID3D11Buffer> lighting;
    UINT first{},count{};
    ComPtr<ID3D11Buffer> pose;UINT poseFirst{},poseCount{};
    RE::NiTransform transform;
    bool local{};
    uint64_t seen=~0ull;
};
// Constant registers a borrowed fitting draw must provide. SE's lighting layout
// ends at c29 with one world matrix; Skyrim VR's ends at c48, with per-eye light
// positions and one world matrix per eye.
constexpr UINT fittingLightingRegisters=vrStereo?49:30;
constexpr UINT fittingPoseRegisters=vrStereo?6:3;
// Skyrim VR: log once why a fitting's lighting wasn't borrowed, so the VR path
// can be checked in game.
void ReportFittingCapture(const char* reason){
    if constexpr(vrStereo){
        static std::unordered_set<std::string_view> reported;
        if(reported.emplace(reason).second)SKSE::log::info("[DBL-VR-fitting] Not borrowed: {}",reason);
    }
}
thread_local std::unordered_map<const RE::BSGeometry*,FittingSample> fittingSamples;
thread_local RE::BSGeometry* fittingBottle=nullptr;
thread_local RE::BSGeometry* fittingGeometry=nullptr;
// Discover the fitting from the confirmed liquid owner. The opaque batch can
// submit its draw without leaving that owner in the shader setup hook.
struct ElSopaFitting {
    RE::NiPointer<RE::BSGeometry> bottle,geometry;
    UINT indices{};
    uint64_t visible{};
};
thread_local std::unordered_multimap<ID3D11Buffer*,ElSopaFitting> elSopaFittings;
thread_local std::unordered_set<UINT> fittingIndexCounts;
thread_local const RE::BSGeometry* setupGeometry=nullptr;
thread_local const RE::BSRenderPass* projectionReceiverPass=nullptr;
void RegisterElSopaFitting(RE::BSGeometry* bottle){
    const std::string_view tag=bottle->name.c_str();
    if(!bottle->parent||(!tag.starts_with("DBL_SSE_ElSopa_")&&!tag.starts_with("DBL_SSE_Mega_ElSopa")))return;
    for(const auto& child:bottle->parent->GetChildren()){
        auto* tri=child?child->AsTriShape():nullptr;
        if(!tri)continue;
        const std::string_view name=tri->name.c_str();
        if(name!="organic"&&name!="Organic")continue;
        auto* mesh=tri->GetGeometryRuntimeData().rendererData;
        if(!mesh||!mesh->indexBuffer||!mesh->vertexBuffer)continue;
        auto* key=reinterpret_cast<ID3D11Buffer*>(mesh->indexBuffer);
        auto [begin,end]=elSopaFittings.equal_range(key);
        for(auto it=begin;it!=end;++it)if(it->second.bottle.get()==bottle&&it->second.geometry.get()==tri){it->second.visible=frame;return;}
        const UINT n=3u*tri->GetTrishapeRuntimeData().triangleCount;
        elSopaFittings.emplace(key,ElSopaFitting{RE::NiPointer<RE::BSGeometry>(bottle),RE::NiPointer<RE::BSGeometry>(tri),n,frame});
        fittingIndexCounts.insert(n);
        return;
    }
}
void ObserveBottleFitting(RE::BSRenderPass* pass){
    auto* g=pass->geometry;
    if(!g->parent)return;
    const char* raw=g->name.c_str();
    if(*raw!='C'&&*raw!='P'&&*raw!='o'&&*raw!='O')return;
    const std::string_view name(raw);
    if(name!="Cork"&&name!="organic"&&name!="Organic"&&!(name.starts_with("Potion ")&&name.ends_with(" Cork")))return;
    for(const auto& child:g->parent->GetChildren()){
        auto* bottle=child?child->AsGeometry():nullptr;
        if(!bottle)continue;
        const std::string_view tag=bottle->name.c_str();
        if(!tag.starts_with("DBL_SSE_Skyking_")&&!tag.starts_with("DBL_SSE_ElSopa_")&&!tag.starts_with("DBL_SSE_Mega_ElSopa"))continue;
        const auto* target=FindBottleProfile(profiles,bottle->name.c_str());
        if(target&&target->nativeGlassPrototype){fittingBottle=bottle;fittingGeometry=g;return;}
    }
}
ComPtr<ID3D11ShaderResourceView> NativeSunShadow(ID3D11DeviceContext*,bool);
void CaptureFitting(ID3D11DeviceContext* ctx,const RE::BSGeometry* bottle,RE::BSGeometry* geometry,bool verifyPose){
    auto& sample=fittingSamples[bottle];
    if(sample.seen==frame&&(!verifyPose||sample.pose))return;
    if(verifyPose&&!NativeSunShadow(ctx,false)){ReportFittingCapture("no screen shadow mask on the fitting's draw");return;}
    auto* renderer=RE::BSGraphics::Renderer::GetRendererData();
    ComPtr<ID3D11ShaderResourceView> shadow;
    ctx->PSGetShaderResources(14,1,&shadow);
    if(!renderer||!shadow||shadow.Get()!=reinterpret_cast<ID3D11ShaderResourceView*>(renderer->renderTargets[RE::RENDER_TARGET::kSHADOW_MASK].SRV)){
        ReportFittingCapture("the fitting's draw has no shadow mask at t14");return;
    }
    auto* prop=netimmerse_cast<RE::BSLightingShaderProperty*>(geometry->properties[1].get());
    if(!prop||!prop->material)return;
    ComPtr<ID3D11DeviceContext1> cx;ctx->QueryInterface(IID_PPV_ARGS(&cx));
    ComPtr<ID3D11Buffer> src;
    UINT first=0,count=0;
    if(cx)cx->PSGetConstantBuffers1(2,1,&src,&first,&count);
    else ctx->PSGetConstantBuffers(2,1,&src);
    if(!src){ReportFittingCapture("no lighting constants");return;}
    D3D11_BUFFER_DESC desc{};src->GetDesc(&desc);
    constexpr UINT lightingBytes=16*fittingLightingRegisters;
    if(desc.ByteWidth<lightingBytes||first>(desc.ByteWidth-lightingBytes)/16||(cx&&count<fittingLightingRegisters)){
        ReportFittingCapture("lighting constants smaller than expected");return;
    }
    D3D11_BUFFER_DESC saved{};
    if(sample.lighting)sample.lighting->GetDesc(&saved);
    if(saved.ByteWidth!=desc.ByteWidth){
        ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);
        saved={};saved.ByteWidth=desc.ByteWidth;saved.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(dev->CreateBuffer(&saved,nullptr,sample.lighting.ReleaseAndGetAddressOf())))return;
    }
    ctx->CopyResource(sample.lighting.Get(),src.Get());
    if(verifyPose){
        ComPtr<ID3D11Buffer> pose;
        if(cx)cx->VSGetConstantBuffers1(2,1,&pose,&sample.poseFirst,&sample.poseCount);
        else ctx->VSGetConstantBuffers(2,1,&pose);
        if(!pose){ReportFittingCapture("no geometry constants");return;}
        D3D11_BUFFER_DESC srcDesc{},dstDesc{};pose->GetDesc(&srcDesc);
        constexpr UINT poseBytes=16*fittingPoseRegisters;
        if(srcDesc.ByteWidth<poseBytes||sample.poseFirst>(srcDesc.ByteWidth-poseBytes)/16||(cx&&sample.poseCount<fittingPoseRegisters)){
            ReportFittingCapture("geometry constants smaller than expected");return;
        }
        if(sample.pose)sample.pose->GetDesc(&dstDesc);
        if(dstDesc.ByteWidth!=srcDesc.ByteWidth){
            ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);
            dstDesc={};dstDesc.ByteWidth=srcDesc.ByteWidth;dstDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            if(FAILED(dev->CreateBuffer(&dstDesc,nullptr,sample.pose.ReleaseAndGetAddressOf())))return;
        }
        ctx->CopyResource(sample.pose.Get(),pose.Get());
    }else sample.pose.Reset();
    sample.first=first;sample.count=count;sample.transform=geometry->world;
    sample.local=prop->material->GetFeature()==RE::BSShaderMaterial::Feature::kDefault;
    sample.seen=frame;
}
bool CaptureSubmittedFitting(ID3D11DeviceContext* ctx,UINT indices){
    if(!fittingIndexCounts.contains(indices))return false;
    ComPtr<ID3D11Buffer> index,vertex;DXGI_FORMAT format;UINT offset,stride;
    ctx->IAGetIndexBuffer(&index,&format,&offset);
    auto [begin,end]=elSopaFittings.equal_range(index.Get());
    if(begin==end)return false;
    ctx->IAGetVertexBuffers(0,1,&vertex,&stride,&offset);
    ElSopaFitting* match=nullptr;bool ambiguous=false;
    for(auto it=begin;it!=end;++it){
        auto& reg=it->second;
        if(frame-reg.visible>1||indices!=reg.indices)continue;
        auto* mesh=reg.geometry->GetGeometryRuntimeData().rendererData;
        if(!mesh||reinterpret_cast<ID3D11Buffer*>(mesh->vertexBuffer)!=vertex.Get())continue;
        if(setupGeometry==reg.geometry.get()){match=&reg;ambiguous=false;break;}
        if(match)ambiguous=true;
        match=&reg;
    }
    if(!match||ambiguous)return false;
    CaptureFitting(ctx,match->bottle.get(),match->geometry.get(),true);
    return true;
}
RE::BSLightingShaderProperty* NativeLiquidProperty(RE::BSGeometry* glass){
    if(!glass->parent)return {};
    for(const auto& child:glass->parent->GetChildren()){
        if(!child)continue;
        const std::string_view name=child->name.c_str();
        if(name!="PotionLiquidAnimated"&&name!="DBL_NativeLiquidSource")continue;
        auto* geometry=child->AsGeometry();if(!geometry)continue;
        auto* property=netimmerse_cast<RE::BSLightingShaderProperty*>(geometry->GetGeometryRuntimeData().properties[1].get());
        if(!property||!property->material)continue;
        return property;
    }
    return nullptr;
}
ComPtr<ID3D11ShaderResourceView> NativeTextureView(RE::NiSourceTexture* texture){
    if(!texture||!texture->rendererTexture)return {};
    auto* data=reinterpret_cast<RE::NiTexture::RendererData*>(texture->rendererTexture);
    return ComPtr<ID3D11ShaderResourceView>(reinterpret_cast<ID3D11ShaderResourceView*>(data->resourceView));
}
ComPtr<ID3D11ShaderResourceView> NativeSunShadow(ID3D11DeviceContext* ctx,bool fittingMask=false){
    auto* renderer=RE::BSGraphics::Renderer::GetRendererData();
    if(!renderer)return {};
    const auto& mask=renderer->renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];
    const auto& main=renderer->renderTargets[RE::RENDER_TARGET::kMAIN];
    if(!mask.texture||!mask.SRV||!main.texture)return {};

    // The native lighting draw uses t14 for its screen-space mask. Verify
    // its identity before borrowing it so other passes and wrappers can fall back.
    ComPtr<ID3D11ShaderResourceView> bound;
    ctx->PSGetShaderResources(14,1,&bound);
    if(fittingMask)bound=reinterpret_cast<ID3D11ShaderResourceView*>(mask.SRV);
    else if(bound.Get()!=reinterpret_cast<ID3D11ShaderResourceView*>(mask.SRV))return {};
    ComPtr<ID3D11RenderTargetView> output;
    ctx->OMGetRenderTargets(1,&output,nullptr);
    if(!output)return {};
    ComPtr<ID3D11Resource> outputResource;
    output->GetResource(&outputResource);
    if(outputResource.Get()!=reinterpret_cast<ID3D11Texture2D*>(main.texture))return {};

    D3D11_TEXTURE2D_DESC shadowDesc{},mainDesc{};
    reinterpret_cast<ID3D11Texture2D*>(mask.texture)->GetDesc(&shadowDesc);
    reinterpret_cast<ID3D11Texture2D*>(main.texture)->GetDesc(&mainDesc);
    if(shadowDesc.Width!=mainDesc.Width||shadowDesc.Height!=mainDesc.Height||shadowDesc.SampleDesc.Count!=1)return {};
    return bound;
}

struct NearbyProjection {
    RE::NiPointer<RE::BSGeometry> body;
    float distance{};
};
std::array<NearbyProjection,BottleProjection::maxPerFrame> nearbyProjections;

void SelectNearbyProjections(){
    nearbyProjections={};

    size_t size=0;
    for(const auto& [body,instance]:instances){
        if(instance.projectionSeen!=frame)continue;
        const NearbyProjection entry{instance.owner,instance.projectionDistance};
        auto slot=std::lower_bound(nearbyProjections.begin(),nearbyProjections.begin()+size,entry,
            [](const NearbyProjection& a,const NearbyProjection& b){
                if(a.distance!=b.distance)return a.distance<b.distance;
                return std::less<const RE::BSGeometry*>{}(a.body.get(),b.body.get());
            });
        if(slot==nearbyProjections.end())continue;
        size=std::min(size+1,nearbyProjections.size());
        auto end=nearbyProjections.begin()+size;
        std::move_backward(slot,end-1,end);
        *slot=entry;
    }
}

bool ProjectionTargets(const RE::BSGeometry* body){
    return std::any_of(nearbyProjections.begin(),nearbyProjections.end(),
        [body](const NearbyProjection& entry){return entry.body.get()==body;});
}

bool UsesNativeOpaqueShadow(const RE::BSGeometry* body){
    if(!body)return false;
    const auto* ownerProfile=FindBottleProfile(profiles,body->name.c_str());
    if(selectedVisual&&body==selectedGeometry.get())ownerProfile=&*selectedVisual;
    return ownerProfile&&(!ownerProfile->liquidOnly||ownerProfile->authoredGlass||NativeOpaqueShadow::VerifiedFittingBody(*body))&&
        ownerProfile->visual.surface.z>-1.f&&
        ownerProfile->glassOpacity>=1.f;
}

const RE::BSLight* SingleProjectionPointLight(const RE::BSRenderPass& pass);

#include "ProjectionLightChecks.inc"

void DrawNativeOpaqueShadow(ID3D11DeviceContext* ctx,UINT count,UINT start,INT base){
    if(!utilityPass||!distanceTuning.renderer||!nearbyProjections.front().body)return;
    const auto& fitting=*utilityPass->geometry;
    if(!fitting.parent)return;
    for(const auto& candidate:nearbyProjections){
        const auto* body=candidate.body.get();
        if(!body||body->parent!=fitting.parent)continue;
        const auto* light=SingleProjectionPointLight(*utilityPass);
        bottleProjection.ObserveAtlas(ctx,body,light,frame);
        if(!start&&!base&&body!=&fitting&&UsesNativeOpaqueShadow(body))
            nativeOpaqueShadow.Draw(ctx,*body,fitting,light,count,frame);
    }
}

const RE::BSLight* SingleProjectionPointLight(const RE::BSRenderPass& pass){
    if(!pass.sceneLights)return nullptr;
    const RE::BSLight* point=nullptr;
    for(unsigned i=0;i<pass.numLights;++i){
        const auto* light=pass.sceneLights[i];
        if(!light||!light->pointLight||!light->light)continue;
        if(point)return nullptr;
        point=light;
    }
    return point;
}

const RE::BSLight* ProjectionReceiverLight(const RE::BSRenderPass& pass){
    // Binding an existing mask only requires membership in this receiver's light list.
    // Bootstrap it on the same receivers when the fireplace is already identified.
    const auto onPass=[&](const RE::BSLight* light){
        return light&&std::find(pass.sceneLights,pass.sceneLights+pass.numLights,light)!=pass.sceneLights+pass.numLights;
    };
    if(const auto* recent=bottleProjection.RecentReceiverLight(frame);onPass(recent)&&recent->pointLight&&recent->light)return recent;
    if(bottleProjection.HasReceiverLight(frame))return nullptr;
    const RE::BSLight* casterLight=nullptr;
    for(const auto* light:bottleProjection.ReceiverCasterLights(frame)){
        if(!onPass(light)||!light->pointLight||!light->light)continue;
        if(casterLight)return nullptr;
        casterLight=light;
    }
    if(casterLight)return casterLight;
    auto* prop=netimmerse_cast<RE::BSLightingShaderProperty*>(pass.geometry->properties[1].get());
    if(!prop||!prop->material||prop->material->GetFeature()!=RE::BSShaderMaterial::Feature::kDefault)return nullptr;
    // A single native point light gives a verified position, range and mask channel.
    const auto* point=SingleProjectionPointLight(pass);
    if(!point)return nullptr;
    return point;
}

struct ProjectionReceiverCandidates {
    RE::NiPoint3 origin{};
    float radiusSquared{};
    std::array<const BottleProjection::Caster*,BottleProjection::maxPerFrame> casters{};
    size_t count{};
};

ProjectionReceiverCandidates FindProjectionReceiverCandidates(const RE::BSLight& point){
    ProjectionReceiverCandidates result;
    const float radius=point.light->GetLightRuntimeData().radius.x;
    if(!(radius>0))return result;
    result.origin=point.light->world.translate;
    result.radiusSquared=radius*radius;
    for(const auto& caster:bottleProjection.Casters(frame)){
        if((caster.light&&caster.light!=&point)||!caster.body||!ProjectionTargets(caster.body))continue;
        if((result.origin-caster.body->world.translate).SqrLength()<result.radiusSquared)
            result.casters[result.count++]=&caster;
    }
    return result;
}

void PrepareProjectionReceiver(ID3D11DeviceContext* ctx,const RE::BSRenderPass& pass,
                               ID3D11ShaderResourceView* mask,const RE::BSLight* point,
                               const ProjectionReceiverCandidates& candidates){
    std::array<const BottleProjection::Caster*,BottleProjection::maxPerFrame> eligible{};
    size_t count=0;
    bool needsTileLight=false;
    for(size_t i=0;i<candidates.count;++i){
        const auto* caster=candidates.casters[i];
        if(caster->light==point)needsTileLight=true;
        if(!UsesNativeOpaqueShadow(caster->body)||!nativeOpaqueShadow.Covers(caster->body,point,frame))
            eligible[count++]=caster;
    }
    if(!count&&!needsTileLight)return;
    bottleProjection.CaptureReceiverLight(ctx,frame,point,candidates.origin,candidates.radiusSquared,pass.geometry->world.translate);
    auto end=std::remove_if(eligible.begin(),eligible.begin()+count,
        [&](const auto* caster){return !bottleProjection.ReceiverReaches(caster->body,frame);});
    bottleProjection.BuildReceiverMask(ctx,frame,mask,{eligible.data(),size_t(end-eligible.begin())});
}

struct BottleReceiverMaskBinding {
    ID3D11DeviceContext* context;
    ComPtr<ID3D11ShaderResourceView> original;
    BottleReceiverMaskBinding(ID3D11DeviceContext* ctx,const RE::BSRenderPass* pass):context(ctx){
        if(!pass||!pass->sceneLights||!pass->numLights)return;
        try{
            const auto validateMask=[&](){
#ifdef DBL_FRAMEWORK_UI
                dblui::ReceiverTimer timing(dblui::ReceiverStage::MaskValidation);
#endif
                return NativeSunShadow(ctx,false);
            };
            const auto affectsDraw=[&](){
#ifdef DBL_FRAMEWORK_UI
                dblui::ReceiverTimer timing(dblui::ReceiverStage::LightMatching);
#endif
                const auto* light=bottleProjection.ReceiverLight(frame);
                return light&&std::find(pass->sceneLights,pass->sceneLights+pass->numLights,light)!=
                    pass->sceneLights+pass->numLights;
            };
            const RE::BSLight* point=nullptr;
            std::optional<ProjectionReceiverCandidates> candidates;
            auto* modified=bottleProjection.ModifiedMask(frame);
            if(modified){
                if(!affectsDraw())return;
            }else{
#ifdef DBL_FRAMEWORK_UI
                dblui::ReceiverTimer timing(dblui::ReceiverStage::LightMatching);
#endif
                point=ProjectionReceiverLight(*pass);
                if(!point)return;
                candidates=FindProjectionReceiverCandidates(*point);
                if(!candidates->count)return;
            }
            auto mask=validateMask();
            if(!mask)return;
            ComPtr<ID3D11BlendState> blend;
            ctx->OMGetBlendState(&blend,nullptr,nullptr);
            if(blend){
                D3D11_BLEND_DESC desc{};
                blend->GetDesc(&desc);
                if(desc.RenderTarget[0].BlendEnable)return;
            }
            if(point){
                PrepareProjectionReceiver(ctx,*pass,mask.Get(),point,*candidates);
                modified=bottleProjection.ModifiedMask(frame);
                if(!modified||!affectsDraw())return;
            }
            original=std::move(mask);
            context->PSSetShaderResources(14,1,&modified);
        }catch(const std::exception& e){
            static bool reported=false;
            if(!reported){
                reported=true;
                SKSE::log::warn("[DBL-bottle-projection] Native mask fallback: {}",e.what());
            }
        }
    }
    ~BottleReceiverMaskBinding(){
        if(original){
            auto* view=original.Get();
            context->PSSetShaderResources(14,1,&view);
        }
    }
};

void PrepareBottleProjection(ID3D11DeviceContext* ctx,const RE::BSGeometry* g,const Profile& profile,
                             const GPU& gpu,const NativeConstantSlice* reflection,bool reflectionShadowed,
                             const RE::BSLight* point,SceneSnapshot* freshCapture){
    if(!ProjectionTargets(g)||gpu.bounds.w<2)return;
    // Use the full cavity profile for the shadow, including the shoulder and
    // neck. Its wide section only determines where to focus the caustic.
    const float radius=gpu.bounds.z;
    if(radius<=0)return;
    float low=gpu.bounds.y,high=gpu.bounds.x;
    for(int i=0;i<int(gpu.bounds.w);++i){
        if(gpu.rings[i].y<radius*.55f)continue;
        low=std::min(low,gpu.rings[i].x);
        high=std::max(high,gpu.rings[i].x);
    }
    const float outerRadius=radius/.95f;
    const float wall=outerRadius-radius;
    const float center=(gpu.bounds.x+gpu.bounds.y)*.5f;
    const float halfHeight=(gpu.bounds.y-gpu.bounds.x)*.5f+wall;
    const float glassOpacity=profile.visual.surface.z<=-1.f?0.f:std::clamp(profile.glassOpacity,0.f,1.f);
    const float glassExtinction=-std::log(std::max(1-glassOpacity,.01f))/(.1f*outerRadius);
    const bool liquidVisible=profile.visual.surface.y>-1.f&&profile.settings.fill>0;
    const float liquidOpacity=gpu.appearance.z;
    const float extinction=liquidVisible?(liquidOpacity>0||profile.visual.surface.w>.5f?
        -std::log(std::max(1-liquidOpacity,.005f))/(2*radius):gpu.color.w*.45f):0.f;
    const float peak=std::max({gpu.color.x,gpu.color.y,gpu.color.z,.01f});
    auto spectralExtinction=[&](float channel){return extinction*(1+.75f*(1-std::clamp(channel/peak,0.f,1.f)));};
    BottleProjection::Constants data{
        gpu.plane,{outerRadius,outerRadius,halfHeight,center},
        {spectralExtinction(gpu.color.x),spectralExtinction(gpu.color.y),spectralExtinction(gpu.color.z),0},
        {glassExtinction,0.f,liquidVisible&&glassOpacity<1.f?.9f:0.f,8*outerRadius},
        {},{(unsigned(gpu.eye.w)&8u)?1.f:0.f,0,0,0},
        {gpu.bounds.w,wall,(low+high)*.5f,glassOpacity>=1.f?1.f:0.f},{}
    };
    std::copy_n(gpu.rings,16,data.rings);
    // Projection rays are in mesh space. Convert both world-unit limits.
    const float scale=std::abs(g->world.scale);
    if(scale>0.f){
        data.controls.w=std::min(data.controls.w,32.f/scale);
        data.absorption.w=std::min(data.controls.w,8.f/scale);
    }else data.controls.z=0;
    if(extinction<=0&&glassOpacity<=0&&data.controls.z<=0)return;
    bottleProjection.RememberCaster(g,frame,data,point);
#ifdef DBL_DEVELOPMENT
    TraceMedieval(g,"caster-registered");
    TraceJessica(g,"caster-registered");
#endif
    static bool loggedCaster=false;
    if(!loggedCaster){
        loggedCaster=true;
        SKSE::log::info("[DBL-shadow-caster] registered {} on main scene",g->name.c_str());
    }
    if(data.controls.z<=0)return;
    const bool useReceiver=!reflection&&bottleProjection.ReceiverReaches(g,frame)&&
        (!point||point==bottleProjection.ReceiverLight(frame));
    const bool mayHaveLight=CausticMayHaveLight(active,WorldProjectionLights(),g->world,data.body,reflectionShadowed||useReceiver);
    try{
        const auto* renderer=mayHaveLight?RE::BSGraphics::Renderer::GetRendererData():nullptr;
        const bool directLightMask=mayHaveLight&&(reflectionShadowed||bool(NativeSunShadow(ctx)));
        // Nearby targets are fixed for the frame.
        const bool multiple=bool(nearbyProjections[1].body);
        bottleProjection.Prepare(ctx,renderer,g,frame,data,reflection,directLightMask,useReceiver,multiple,freshCapture,mayHaveLight);
    }catch(const std::exception& e){
        static bool reported=false;
        if(!reported){
            reported=true;
            SKSE::log::warn("[DBL-bottle-projection] Projection skipped: {}",e.what());
        }
    }
}
// Skyrim VR draws bottles with DrawIndexedInstanced, one instance per eye. The
// renderer submits through oldDraw, so during an instanced bottle draw every
// native submission, including DBL's own liquid draw, keeps the same instancing.
using DrawInstancedFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,INT,UINT);
struct InstancedCall{DrawInstancedFn original;UINT instances,startInstance;};
thread_local const InstancedCall* instancedCall=nullptr;
void DrawBottleInstanced(ID3D11DeviceContext* ctx,UINT count,UINT instances,UINT start,INT base,UINT startInstance,DrawInstancedFn original);
bool render(ID3D11DeviceContext* ctx,UINT count,UINT start,INT base,NativeGlassOrder& glassOrder) {
    if(!distanceTuning.renderer||!active||!profile)return false;
    auto* g=active->geometry;
    if(!vs||count!=profile->indices){
#ifdef DBL_DEVELOPMENT
        TraceMedieval(g,!vs?"missing-shader":"index-count-rejected",ctx,count);
        TraceJessica(g,!vs?"missing-shader":"index-count-rejected",ctx,count);
#endif
        return false;
    }
    dblui::BottleTimer detailTimer;
    ForwardBindings native;bool captured=distanceTuning.optical&&forwardDraws.enabled&&forwardDraws.optimized;
    ComPtr<ID3D11Buffer> vb,geometry,perFrame; UINT stride{},offset{};
    if(captured){native.read(ctx,forwardDraws.cachedContext.Get()==ctx?forwardDraws.extended.Get():nullptr,true);stride=native.stride;geometry=native.vsCB[1];perFrame=native.vsCB[2];}
    else {ctx->IAGetVertexBuffers(0,1,&vb,&stride,&offset);
        ctx->VSGetConstantBuffers(2,1,&geometry);ctx->VSGetConstantBuffers(12,1,&perFrame);}
    if((stride!=32&&!(profile->nativeGlassPrototype&&stride==28))||!geometry||!perFrame){
#ifdef DBL_DEVELOPMENT
        TraceMedieval(g,"vertex-input-rejected",ctx,count,stride,bool(geometry),bool(perFrame));
        TraceJessica(g,"vertex-input-rejected",ctx,count,stride);
#endif
        return false;
    }
    D3D11_RENDER_TARGET_VIEW_DESC lodDesc{};ComPtr<ID3D11RenderTargetView> primaryView;
    bool immediateTargets=true,reverseWorld=false;
    if(profile->nativeGlassPrototype){
        if(!NativeGlassBinding::Eligible(ctx,lodDesc,profile->authoredGlass,primaryView.GetAddressOf())){
#ifdef DBL_DEVELOPMENT
            TraceJessica(g,"native-glass-rejected",ctx,count,stride);
#endif
            return false;
        }
#ifdef DBL_DEVELOPMENT
        TraceJessica(g,"native-glass-admitted",ctx,count,stride);
#endif
    }else{
        const bool medieval=std::string_view(g->name.c_str()).starts_with("DBL_SSE_MedievalSpirits_");
        bool hdrWorldLayout=false;
        immediateTargets=supportedTargets(ctx,&lodDesc,primaryView.GetAddressOf(),medieval,&hdrWorldLayout);
        // Replay into MAIN only. Keep the TAA mask and alpha target outside DBL's draw.
        reverseWorld=(medieval||hdrWorldLayout)&&primaryView&&UsesBottledWorldGlassDepth(ctx,!immediateTargets);
    }
    if(!immediateTargets&&!reverseWorld){
#ifdef DBL_DEVELOPMENT
        TraceMedieval(g,"color-targets-rejected",ctx,count,stride,true,true);
#endif
        return false;
    }
    const auto& t=g->world; if(t.scale<=.0001f)return false;
    
    BeginBubbleFrame(ctx); SelectHeld(g); if(selectedVisual && g==selectedGeometry.get())profile=&*selectedVisual;
    if(profile->shieldWetParticles&&profile->visual.nativeAlpha>=1.f&&profile->visual.surface.z==0){
        return true;
    }
    auto& inst=instances[g]; inst.owner.reset(g);
    if(profile->nativeGlassPrototype&&inst.nativeGlassFrame==frame)return false;
    auto* liquidProperty=profile->nativeGlassPrototype?NativeLiquidProperty(g):nullptr;
    auto* liquidMaterial=liquidProperty?static_cast<RE::BSLightingShaderMaterialBase*>(liquidProperty->material):nullptr;
    ComPtr<ID3D11ShaderResourceView> nativeLiquid,nativeGlow;
    // Authored bottle bodies can anchor plain-colored liquid without a donor mesh.
    if(profile->nativeGlassPrototype&&(!profile->authoredGlass||profile->liquidTexture>0)){
        if(liquidMaterial)nativeLiquid=NativeTextureView(liquidMaterial->diffuseTexture.get());
        if(!nativeLiquid)return false;
    }
    double time=now(); Vec pos{t.translate.x,t.translate.y,t.translate.z};
    bool worldDistance=false;
    float detailWeight=1,effectWeight=1,lodDistance=0;std::array<float,20> effectWeights;effectWeights.fill(1);
    const bool mainScene=IsMainSceneTarget(primaryView.Get());
    if(mainScene){
        auto* camera=RE::PlayerCamera::GetSingleton();auto* ui=RE::UI::GetSingleton();
        if(camera&&camera->cameraRoot&&!(ui&&ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME))){
            worldDistance=true;
            auto eye=camera->cameraRoot->world.translate;
            float distance=nuka::Length(pos-Vec{eye.x,eye.y,eye.z});lodDistance=distance;
            auto fade=[](float x,float a,float b){float t=std::clamp((x-a)/(b-a),0.f,1.f);return 1-t*t*(3-2*t);};
            for(int k=0;k<20;++k)effectWeights[k]=distanceTuning.Weight(k,distance);
            detailWeight=effectWeights[1];effectWeight=DistanceTuning::LiquidActivity(effectWeights);
        }
    }
    detailTimer.Switch(dblui::BottleStage::Pose);
    bool preview=!worldDistance;
    float budgetRadius=std::max(g->worldBound.radius,profile->settings.base.rings[0].z*t.scale);
    float pixels=preview||lodDistance<=budgetRadius?1024:2*budgetRadius/std::max(lodDistance-budgetRadius,.001f)*bubblePixelScale;
    const auto& settings=profile->settings;
    bool bubbleActive=distanceTuning.simulation&&distanceTuning.liquidTrace&&settings.fill>0&&settings.fill<1;
    inst.bubbleRequest={reinterpret_cast<std::uintptr_t>(g),pixels,lodDistance,preview||GrabbedBottle(g),
        {bubbleActive&&effectWeights[2]>0&&settings.shakingBubbles>0?(settings.baseline>0?32u:64u):0u,
         bubbleActive&&effectWeights[3]>0&&settings.foamStrength>0&&settings.intensity>0?64u:0u,
         bubbleActive&&effectWeights[2]>0&&settings.baseline>0?32u:0u},{0,0,0}};
    inst.bubbleSeen=frame;
    Vec bottleUp=nuka::Normalize(localDirection(t,{0,0,1}));
    float simulationWeight=distanceTuning.Weight(20,lodDistance);
    effectWeights[5]*=simulationWeight;effectWeights[6]*=simulationWeight;effectWeight*=simulationWeight;
    if(!distanceTuning.simulation)effectWeight=0;
    if(dblui::Extensive()&&inst.metricsFrame!=frame){inst.metricsFrame=frame;dblui::AddTier(!distanceTuning.simulation||effectWeight==0?4:lodDistance>512?3:lodDistance<=192?0:lodDistance<=384?1:2);}
    bool outsideSimulation=distanceTuning.simulation&&lodDistance>512;
    bool resuming=inst.suspended&&!outsideSimulation;
    if(outsideSimulation){
        if(!inst.hasPose){inst.constants=profile->distant;inst.constants.plane=RestingPlane(inst,*profile,bottleUp);inst.lastPose.Capture(inst.constants,inst.motion.time);inst.hasPose=true;}
        inst.suspended=true;inst.schedule={};inst.time=time;inst.position=pos;inst.frame=frame;
    }
    else if(effectWeight==0){inst.suspended=false;inst.hasPose=false;inst.constants=profile->distant;inst.constants.plane=RestingPlane(inst,*profile,bottleUp);inst.time=time;inst.position=pos;inst.frame=frame;inst.distant=true;inst.schedule={};}
    else if(inst.frame!=frame) {
        if(resuming){
            // No catch-up for time spent outside the simulation range. Re-enter
            // from the displayed pose; new motion interpolates from this state.
            float clock;inst.lastPose.Apply(inst.constants,clock);inst.previous=inst.constants;inst.previousClock=clock;
            inst.suspended=false;inst.distant=false;inst.time=time;inst.schedule={};inst.schedule.interval=SimulationSchedule::Interval(lodDistance);
            inst.translation={};inst.emission={};inst.motion.lastUp=bottleUp;
        }
        if(inst.distant){inst.motion={};inst.translation={};inst.emission={};inst.time=0;inst.distant=false;}

        float dt=inst.time>0?float(time-inst.time):0; Vec acceleration{};
        Vec up=bottleUp;
        if(!(dt>0&&dt<.1f&&nuka::Length(pos-inst.position)<100)){dt=0;inst.motion.lastUp=up;}
        if(auto* ui=RE::UI::GetSingleton();ui&&ui->GameIsPaused())dt=0;
        acceleration=localDirection(t,inst.translation.Sample(pos,dt,dt>0));
        // Reuse Fallout's qualified rotation/vertical-stroke detector. Discontinuous
        // updates and paused inventory views reset its history without emitting.
        Vec right=nuka::Normalize(localDirection(t,{1,0,0}));
        inst.emission.Sample(inst.motion,right,up,
            pos,{0,0,1},1/t.scale,dt,false,dt>0,g);
        bool moved=nuka::Length(pos-inst.position)>TranslationMotion::movementThreshold||nuka::Length(up-inst.observedUp)>.0001f||nuka::Length(right-inst.observedRight)>.0001f;
        inst.observedUp=up;inst.observedRight=right;
        bool tick=!resuming&&inst.schedule.Tick(dt,lodDistance,moved,dt==0);
        nuka::ApplyBubbleLimits(inst.motion,inst.bubbleQuota);
        if(tick){
        inst.motion.restWorldThreshold=.1f;inst.motion.worldScale=t.scale;
        float simulationDt=inst.schedule.Consume();
        if(dblui::Extensive()&&simulationDt>0)dblui::AddScheduled(lodDistance<=192?0:lodDistance<=384?1:2);
        inst.previous=inst.constants;inst.previousClock=inst.motion.time;
        nuka::SimulationTimings timings{};
        auto cacheHits=inst.motion.sectionHits,cacheBuilds=inst.motion.sectionBuilds;
        int steps=simulationDt<=dt?1:std::max(1,int(std::ceil(simulationDt/.05f)));
        for(int step=0;step<steps;++step){nuka::SimulationTimings part{};
            inst.constants=nuka::Simulate(profile->settings,inst.motion,acceleration,up,simulationDt/steps,true,false,1.f,dblui::Extensive()?&part:nullptr);
            if(dblui::Extensive())for(unsigned k=0;k<7;++k)timings[k]+=part[k];
        }
        if(inst.schedule.interval==0||dt==0){inst.previous=inst.constants;inst.previousClock=inst.motion.time;}
        dblui::AddSection(inst.motion.sectionHits-cacheHits,inst.motion.sectionBuilds-cacheBuilds,inst.motion.section.area,inst.motion.section.depth);
        dblui::AddSimulation(timings,inst.constants.bubbleCount);
        } // simulation tick; input sampling above remains per frame
        inst.position=pos;inst.time=time;inst.frame=frame;
    }
    // The vertex shader derives the eye from the current draw's native matrix.
    // Keep this reserved vector zero for the unchanged GPU buffer layout.
    GPU gpu=BuildLiquidGPU(inst,profile,bottleUp,outsideSimulation,resuming,detailWeight,effectWeight,effectWeights);
    if(profile->bulkLiquidTexture){
        auto* ui=RE::UI::GetSingleton();
        auto n=inst.textureSlosh.Sample(frame,time,ui&&ui->GameIsPaused(),{gpu.plane.x,gpu.plane.y,gpu.plane.z});
        const auto& shape=profile->settings.base;
        float pivot=float(shape.rings[0].x+profile->designedHeight*(shape.rings[shape.ringCount-1].x-shape.rings[0].x));
        gpu.liquidTexturePlane={n[0],n[1],n[2],pivot};
    }
    detailTimer.Switch(dblui::BottleStage::State);
    using MaterialFeature=RE::BSShaderMaterial::Feature;
    auto materialFeature=MaterialFeature::kNone;
    if(auto* prop=netimmerse_cast<RE::BSLightingShaderProperty*>(g->GetGeometryRuntimeData().properties[1].get());prop&&prop->material){
        auto* mat=static_cast<RE::BSLightingShaderMaterialBase*>(prop->material);
        // The draw's default lighting technique supplies model-space constants.
        // A native liquid texture donor does not determine the active technique.
        if(mat->GetFeature()==MaterialFeature::kDefault)gpu.eye.w+=8.f;
        if(liquidMaterial)mat=liquidMaterial;
        if(profile->nativeLiquidGlow && liquidMaterial && liquidProperty->emissiveColor){
            const auto& color=*liquidProperty->emissiveColor;
            gpu.emissionColor={color.red,color.green,color.blue,1};
            gpu.liquidEffects.x=std::max(0.f,liquidProperty->emissiveMult);
            if(mat->GetFeature()==MaterialFeature::kGlowMap){
                auto* glow=static_cast<RE::BSLightingShaderMaterialGlowmap*>(mat);
                nativeGlow=NativeTextureView(glow->glowTexture.get());
                if(nativeGlow)gpu.emissionColor.w=2;
            }
        }
        materialFeature=mat->GetFeature();
        if(profile->liquidNativeUV)gpu.liquidAnimation={mat->texCoordOffset[0].x,mat->texCoordOffset[0].y,mat->texCoordScale[0].x,mat->texCoordScale[0].y};
        gpu.authoredSpec={mat->specularColor.red*mat->specularColorScale,mat->specularColor.green*mat->specularColorScale,mat->specularColor.blue*mat->specularColorScale,std::clamp(mat->specularPower,1.f,1024.f)};
    }
    if(profile->independentLiquidAnimation){
        auto* ui=RE::UI::GetSingleton();
        auto offset=inst.liquidScroll.Sample(frame,time,ui&&ui->GameIsPaused(),reinterpret_cast<std::uintptr_t>(g),profile->liquidScrollRate);
        gpu.liquidIndependentAnimation=gpu.liquidAnimation;
        gpu.liquidIndependentAnimation.x=offset[0];gpu.liquidIndependentAnimation.y=offset[1];
    }
    for(UINT slot=0;slot<2;slot++){
        ComPtr<ID3D11ShaderResourceView> texture;ComPtr<ID3D11SamplerState> textureSampler;
        if(captured){texture=native.textures[slot];textureSampler=native.samplers[slot];}
        else {ctx->PSGetShaderResources(slot,1,&texture);ctx->PSGetSamplers(slot,1,&textureSampler);}
        if(texture&&textureSampler){D3D11_SHADER_RESOURCE_VIEW_DESC d{};texture->GetDesc(&d);
            if(d.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D)gpu.eye.w+=float(1u<<slot);
        }
    }
    // Reuse the current native draw's pixel constants/resources without copying
    // or rebinding them. Missing inputs retain the approved Test5 appearance.
    ComPtr<ID3D11Buffer> lighting,material;
    if(captured){lighting=native.psCB[1];material=native.psCB[0];}
    else {ctx->PSGetConstantBuffers(2,1,&lighting);ctx->PSGetConstantBuffers(1,1,&material);}
    if(lighting){D3D11_BUFFER_DESC d{};lighting->GetDesc(&d);if(d.ByteWidth>=30*16)gpu.glass.w=1;}
    ComPtr<ID3D11ShaderResourceView> environment;ComPtr<ID3D11SamplerState> sampler;
    if(captured){environment=native.textures[2];sampler=native.samplers[2];}
    else {ctx->PSGetShaderResources(4,1,&environment);ctx->PSGetSamplers(4,1,&sampler);}
    if(gpu.glass.w&&environment&&sampler&&material){
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};environment->GetDesc(&d);
        D3D11_BUFFER_DESC m{};material->GetDesc(&m);
        if(UseNativeEnvironment(materialFeature,d.ViewDimension==D3D11_SRV_DIMENSION_TEXTURECUBE)&&m.ByteWidth>=48)gpu.glass.w=2;
    }
    auto& sceneSnapshot=profile->nativeGlassPrototype?nativeGlassSnapshot: ::sceneSnapshot;
    const auto capturesBefore=sceneSnapshot.captures;
    RefractionCamera refractionCamera;
    bool lateDraw=false;
    // The immediate shader cannot take the unfamiliar auxiliary layout.
    // While HDR is unavailable, use the same isolated MAIN forward replay
    // on this draw. Other bottles keep their existing fallback.
    const bool replayNow=!immediateTargets&&reverseWorld&&opticalBuffers.boundaryMissing;
    detailTimer.Switch(dblui::BottleStage::Optical);
    { // GPU commands are timed at their submission sites, not around CPU setup.
    try{lateDraw=!profile->nativeGlassPrototype&&distanceTuning.optical&&opticalBuffers.prepare(ctx,mainScene?primaryView.Get():nullptr,frame,gpu.refraction,gpu.viewport,reverseWorld,replayNow);}catch(const std::exception& e){opticalBuffers.failed=true;SKSE::log::error("Optical producer unavailable; immediate fallback: {}",e.what());}
    if(!immediateTargets&&!lateDraw){
#ifdef DBL_DEVELOPMENT
        TraceMedieval(g,"deferred-targets-rejected",ctx,count,stride,true,true);
#endif
        return false;
    }
#ifdef DBL_DEVELOPMENT
    TraceMedieval(g,replayNow?"auxiliary-targets-immediate-forward":lateDraw?(immediateTargets?"deferred-admitted":"auxiliary-targets-deferred"):
        mainScene?"main-immediate":"offscreen-immediate",ctx,count,stride,true,true);
#endif
    gpu.bubbleInfo.z=lateDraw?1.f:0.f;
    if constexpr(vrStereo)if(!lateDraw&&vrTraceBudget>0){
        // VR draws every bottle directly. Give it the screen-size trace budget
        // that late draws use; the shader scales the projected size by .w.
        UINT viewports=1;D3D11_VIEWPORT vp{};ctx->RSGetViewports(&viewports,&vp);
        if(viewports==1&&vp.Width>0&&vp.Height>0){gpu.bubbleInfo.z=1;gpu.bubbleInfo.w=vrTraceBudget;gpu.viewport={vp.TopLeftX,vp.TopLeftY,vp.Width,vp.Height};}
    }
    // VR draws world bottles directly; give them the same distance weights as
    // SE's late-drawn world bottles instead of full detail at every distance.
    ApplyLiquidDrawDistance(gpu,effectWeights,lateDraw,(profile->nativeGlassPrototype||vrStereo)&&worldDistance);
    if(!distanceTuning.liquidTrace)gpu.bubbleInfo.x=gpu.bubbleInfo.y=0;
    if(lateDraw)sceneSnapshot.status=forwardDraws.enabled?"pre-HDR forward resolve; shared final scene snapshot":"separate optical buffers";
    else if(profile->DirectNativeLiquid()){
        // Draw liquid on the native glass boundary, immediately before glass.
        // Empty pixels alpha-blend away. No captured glass/background is
        // sampled, so the native shell cannot be refracted or fogged twice.
        gpu.refraction={};
    }else try{
        if constexpr(vrStereo){
            // VR: one shared snapshot per frame. A per-bottle copy of the double-wide
            // stereo target is too costly; the shader clamps lookups to each eye's half.
            // The refraction camera reads one view's constants, so VR doesn't use it.
            sceneSnapshot.captureShared(ctx,frame,gpu.refraction,gpu.viewport);
        }else
        if(profile->nativeGlassPrototype){
            if(profile->nativeGlassPrototype&&worldDistance)refractionCamera.read(ctx);
            const bool refreshHeld=g->name=="DBL_SSE_TMDWinery_winebottle01jessica"||
                (mainScene&&g->name=="DBL_SSE_XMead_Ashfire");
            const bool refresh=refreshHeld&&GrabbedBottle(g);
            const auto* camera=refractionCamera.camera.buffer?&refractionCamera:nullptr;
            sceneSnapshot.captureShared(ctx,frame,gpu.refraction,gpu.viewport,refresh,camera);
#ifdef DBL_DEVELOPMENT
            TraceJessica(g,refresh?(gpu.refraction.x?"held-refraction-refreshed":"held-refraction-unavailable"):
                (gpu.refraction.x?"refraction-captured":"refraction-unavailable"));
#endif
        }else if(reverseWorld){
            refractionCamera.read(ctx);
            const auto* camera=refractionCamera.camera.buffer?&refractionCamera:nullptr;
            sceneSnapshot.capture(ctx,gpu.refraction,gpu.viewport,false,false,camera,true);
        }else sceneSnapshot.capture(ctx,gpu.refraction,gpu.viewport);
    }catch(const std::exception& e){gpu.refraction={};sceneSnapshot.status=e.what();}
    }
    if(profile->nativeGlassPrototype&&captured&&native.csStrict&&communityLighting.liveBindingsReady(ctx))gpu.eye.w=float(unsigned(gpu.eye.w)|4u);
    const FittingSample* fitting=nullptr;
    bool reflectionShadowed=false;
    // VR: the screen shadow mask, TMD's reflection lighting and bottle projection
    // assume one view, so they stay off in the stereo build.
    const NativeConstantSlice* reflectionLighting=!vrStereo&&profile->authoredGlass?
        tmdReflections.Lighting(ctx,g,&reflectionShadowed):nullptr;
    const NativeConstantSlice* borrowedLight=nullptr;
    if(worldDistance&&effectWeights[13]>0){
        auto* player=RE::PlayerCharacter::GetSingleton();
        auto* cell=player?player->GetParentCell():nullptr;
        if(cell&&cell->IsInteriorCell()){
            gpu.liquidEffects.w=1-profile->liquidInteriorPointLightScattering;
            if(profile->nativeGlassPrototype&&!(unsigned(gpu.eye.w)&4u)){
                RegisterElSopaFitting(g);
                const auto it=fittingSamples.find(g);
                if(it!=fittingSamples.end()&&it->second.seen==frame){
                    fitting=&it->second;
                    const auto relative=t.Invert()*fitting->transform;
                    for(int i=0;i<3;++i){
                        const auto& row=relative.rotate.entry[i];
                        gpu.fittingRows[i]={row[0]*relative.scale,row[1]*relative.scale,row[2]*relative.scale,relative.translate[i]};
                    }
                    gpu.fittingPosition.x=fitting->pose?1.f:0.f;
                    if(fitting->pose&&dblui::Extensive()){
                        static std::unordered_set<std::string> reported;
                        if(reported.emplace(g->name.c_str()).second)
                            SKSE::log::info("[DBL-fitting] Bound same-frame fitting lighting and pose for {}",g->name.c_str());
                    }
                    gpu.fittingColor.w=fitting->local?relative.scale:-1.f;
                    if constexpr(vrStereo){
                        static std::unordered_set<std::string> reportedVR;
                        if(reportedVR.emplace(g->name.c_str()).second)
                            SKSE::log::info("[DBL-VR-fitting] Borrowed fitting lighting for {} (pose {}, {} space)",g->name.c_str(),fitting->pose?"verified":"not verified",fitting->local?"model":"world");
                    }
                }
            }
            if(reflectionLighting&&!(unsigned(gpu.eye.w)&4u)){
                borrowedLight=reflectionLighting;
                // TMD's environment-map reflection uses camera-relative world
                // positions. The existing donor path converts to body-local if needed.
                gpu.fittingPosition.y=1;
                gpu.fittingColor.w=-1;
            }
            if(!vrStereo&&!profile->authoredGlass&&(fitting||std::string_view(g->name.c_str()).starts_with("DBL_SSE_TMDWinery_"))){
                native.sunShadow=NativeSunShadow(ctx,fitting!=nullptr);
                if(native.sunShadow)gpu.eye.w=float(unsigned(gpu.eye.w)|32u);
            }
        }else if(cell&&!vrStereo){
            native.sunShadow=NativeSunShadow(ctx);
            if(native.sunShadow)gpu.eye.w=float(unsigned(gpu.eye.w)|16u);
        }
    }
    if(!vrStereo&&worldDistance&&gpu.bounds.w>=2&&gpu.bounds.z>0){
        inst.projectionSeen=frame;
        inst.projectionDistance=lodDistance;
        const auto* point=inst.projectionLightFrame==frame?inst.projectionLight:nullptr;
        PrepareBottleProjection(ctx,g,*profile,gpu,reflectionLighting,reflectionShadowed,point,
            gpu.refraction.x&&sceneSnapshot.captures!=capturesBefore?&sceneSnapshot:nullptr);
    }
    detailTimer.Switch(dblui::BottleStage::Upload);
    if(lateDraw&&forwardDraws.enabled&&forwardDraws.optimized){
        // No temporary graphics-state substitution and no extra copy of our own
        // constant buffer. Capture native bindings before touching the pipeline.
        forwardDraws.capture(ctx,count,start,base,&gpu,captured?&native:nullptr,g->name.c_str(),reinterpret_cast<uintptr_t>(g));++opticalBuffers.draws;
        if(replayNow){
            try{opticalBuffers.replayPreparedForward();}
            catch(...){opticalBuffers.reset();throw;}
            return true;
        }
        if(reverseWorld){
            static bool reported=false;
            if(!reported){
                reported=true;
                SKSE::log::info("[DBL-world] Reverse-Z forward draw queued for {}",g->name.c_str());
            }
        }
        return true;
    }
    D3D11_MAPPED_SUBRESOURCE map{}; if(FAILED(ctx->Map(buffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map)))return false;
    memcpy(map.pData,&gpu,sizeof(gpu));ctx->Unmap(buffer.Get(),0);
    detailTimer.Switch(dblui::BottleStage::State);
    ComPtr<ID3D11VertexShader> ov;ComPtr<ID3D11PixelShader> op;ComPtr<ID3D11InputLayout> ol;
    ComPtr<ID3D11BlendState> obl;ComPtr<ID3D11DepthStencilState> od;ComPtr<ID3D11RasterizerState> ors;
    FLOAT factors[4];UINT mask,stencil;
    ctx->VSGetShader(&ov,nullptr,nullptr);ctx->PSGetShader(&op,nullptr,nullptr);ctx->IAGetInputLayout(&ol);
    ctx->OMGetBlendState(&obl,factors,&mask);ctx->OMGetDepthStencilState(&od,&stencil);ctx->RSGetState(&ors);
    // Split direct liquid draws in two-sided native glass.
    // Submit the rear shell with its untouched engine bindings before liquid.
    // TMD's translucent shell leaves depth open for particles drawn after wine.
    // Keep normal alpha blending, but occlude those draws at the solved wet entry.
    const bool shieldWetParticles=profile->shieldWetParticles && profile->liquidAlphaOverride>0 && profile->visual.surface.y>-1;
    const bool writeLiquidDepth=effectWeights[19]>0 &&
        (shieldWetParticles || (profile->DirectNativeLiquid() && profile->nativeLiquidDepth));
    if(profile->authoredGlass)
        glassOrder.Begin(ctx,oldDraw,count,start,base,false);
    else if(profile->nativeGlassPrototype&&profile->liquidAlphaOverride>0&&(!writeLiquidDepth||shieldWetParticles)&&effectWeights[19]>0)
        glassOrder.Begin(ctx,DrawNativeBottleGlass,count,start,base);
    ComPtr<ID3D11ShaderResourceView> oldScene,oldSceneDepth,oldBlur;ComPtr<ID3D11SamplerState> oldSceneSampler;
    if(gpu.refraction.x&&!lateDraw){ctx->PSGetShaderResources(12,1,&oldBlur);ctx->PSGetShaderResources(14,1,&oldSceneDepth);ctx->PSGetShaderResources(15,1,&oldScene);ctx->PSGetSamplers(15,1,&oldSceneSampler);}
    if(gpu.refraction.x&&!lateDraw){auto* blur=sceneSnapshot.blur.view();ctx->PSSetShaderResources(12,1,&blur);auto* d=sceneSnapshot.depthView.Get();auto* s=sceneSnapshot.colorView.Get();auto* sampler=sceneSnapshot.sampler.Get();ctx->PSSetShaderResources(14,1,&d);ctx->PSSetShaderResources(15,1,&s);ctx->PSSetSamplers(15,1,&sampler);}
    detailTimer.Switch(dblui::BottleStage::Submit);
    ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(writeLiquidDepth?nativeDepthPS.Get():ps.Get(),nullptr,0);ctx->IASetInputLayout(layout.Get());
    BottleConstantBinding bottleConstants(ctx,native.extended.Get(),buffer.Get());
    RefractionCameraBinding cameraBinding(ctx,gpu.refraction.x>=3?&refractionCamera:nullptr,false);
    ctx->OMSetBlendState(blend.Get(),nullptr,~0u);
    ctx->OMSetDepthStencilState(writeLiquidDepth?liquidDepthWrite.Get():depth.Get(),0);ctx->RSSetState(raster.Get());
    RippleTexture::Binding rippleBinding(ctx,rippleTexture);
    NativeGlassBinding nativeGlassBinding(ctx);
    if(profile->nativeGlassPrototype)nativeGlassBinding.Bind(nativeLiquid.Get(),nativeGlow.Get());
    struct RestoreSunShadow {
        ID3D11DeviceContext* context;
        ComPtr<ID3D11ShaderResourceView> previous;
        bool bound;
        RestoreSunShadow(ID3D11DeviceContext* ctx,ID3D11ShaderResourceView* mask):context(ctx),bound(mask!=nullptr){
            if(bound){
                context->PSGetShaderResources(sunShadowSlot,1,&previous);
                context->PSSetShaderResources(sunShadowSlot,1,&mask);
            }
        }
        ~RestoreSunShadow(){
            if(bound){auto* view=previous.Get();context->PSSetShaderResources(sunShadowSlot,1,&view);}
        }
    } restoreSunShadow(ctx,native.sunShadow.Get());
    NativeConstantSlice oldFitting;
    ComPtr<ID3D11Buffer> oldPose;UINT oldPoseFirst=0,oldPoseCount=0;
    ComPtr<ID3D11DeviceContext1> fittingContext;
    if(borrowedLight||fitting){
        ctx->QueryInterface(IID_PPV_ARGS(&fittingContext));
        oldFitting.Read(ctx,fittingContext.Get(),false,7);
        if(borrowedLight)borrowedLight->Bind(ctx,fittingContext.Get(),false,7);
        else NativeConstantSlice{fitting->lighting,fitting->first,fitting->count}.Bind(ctx,fittingContext.Get(),false,7);
        if(!borrowedLight&&fitting->pose){
            auto* pose=fitting->pose.Get();
            if(fittingContext){
                fittingContext->PSGetConstantBuffers1(8,1,&oldPose,&oldPoseFirst,&oldPoseCount);
                fittingContext->PSSetConstantBuffers1(8,1,&pose,&fitting->poseFirst,&fitting->poseCount);
            }else{ctx->PSGetConstantBuffers(8,1,&oldPose);ctx->PSSetConstantBuffers(8,1,&pose);}
        }
    }
    if(lateDraw)opticalBuffers.produce(ctx,oldDraw,count,start,base,nullptr,g->name.c_str(),reinterpret_cast<uintptr_t>(g));
    else {dblui::GPUCost timing(ctx,dblui::GPUStage::Draw);dblui::LabelDraw(timing.token,g->name.c_str(),reinterpret_cast<uintptr_t>(g));oldDraw(ctx,count,start,base);}
#ifdef DBL_DEVELOPMENT
    TraceJessica(g,"liquid-submitted",ctx,count,stride);
#endif
    if(borrowedLight||fitting){
        if(!borrowedLight&&fitting->pose){
            auto* pose=oldPose.Get();
            if(fittingContext)fittingContext->PSSetConstantBuffers1(8,1,&pose,&oldPoseFirst,&oldPoseCount);
            else ctx->PSSetConstantBuffers(8,1,&pose);
        }
        oldFitting.Bind(ctx,fittingContext.Get(),false,7);
    }
    if(profile->nativeGlassPrototype)inst.nativeGlassFrame=frame;
    if(gpu.refraction.x&&!lateDraw){auto* blur=oldBlur.Get();ctx->PSSetShaderResources(12,1,&blur);auto* d=oldSceneDepth.Get();auto* s=oldScene.Get();auto* sampler=oldSceneSampler.Get();ctx->PSSetShaderResources(14,1,&d);ctx->PSSetShaderResources(15,1,&s);ctx->PSSetSamplers(15,1,&sampler);}
    ctx->VSSetShader(ov.Get(),nullptr,0);ctx->PSSetShader(op.Get(),nullptr,0);ctx->IASetInputLayout(ol.Get());
    ctx->OMSetBlendState(obl.Get(),factors,mask);ctx->OMSetDepthStencilState(od.Get(),stencil);ctx->RSSetState(ors.Get());
    return true;
}
#include "VRDiagnostics.inc"
__declspec(noinline) bool TryRenderBottle(ID3D11DeviceContext* ctx,UINT count,UINT start,INT base,NativeGlassOrder& glassOrder){
    try{return render(ctx,count,start,base,glassOrder);}catch(const std::exception& e){
        SKSE::log::error("Bottle render skipped: {}",e.what());
        return false;
    }
}
#include "TmdShellOptics.inc"
#define DBL_SHELL_OPTICS
#include "TmdShellDraw.inc"
#undef DBL_SHELL_OPTICS
void DrawAuthoredBottleGlass(ID3D11DeviceContext* ctx,UINT count,UINT start,INT base){
    const auto& visual=profile->visual;
    if(visual.surface.z<=-1.f)return;
    float weight=1;
    if(auto* camera=RE::PlayerCamera::GetSingleton();camera&&camera->cameraRoot){
        auto delta=camera->cameraRoot->world.translate-active->geometry->world.translate;
        weight=distanceTuning.Weight(0,delta.Length());
    }
    if(profile->nativeLiquidGlow&&visual.nativeAlpha<0){
        // RUSTIC glass uses the source shader's per-pixel alpha until the user overrides opacity.
        DrawShellTransmission(ctx,count,start,base,weight*visual.shellRefraction,
                              weight>0?std::max(0,visual.straight):0);
        DrawNativeBottleGlass(ctx,count,start,base);
        return;
    }
    if(!DrawTmdShell(ctx,count,start,base,true,profile->glassOpacity,
                     weight*visual.shellRefraction,weight>0?std::max(0,visual.straight):0)){
#ifdef DBL_DEVELOPMENT
        TraceJessica(active->geometry,"native-shell-fallback",ctx,count);
#endif
        DrawNativeBottleGlass(ctx,count,start,base);
        return;
    }
#ifdef DBL_DEVELOPMENT
    TraceJessica(active->geometry,"authored-shell-submitted",ctx,count);
#endif
}
bool QueueTmdReflection(ID3D11DeviceContext* ctx,UINT count,UINT start,INT base){
    auto* body=tmdReflectionBody;
    if(!body||!distanceTuning.renderer)return false;
    auto* bodyProfile=FindBottleProfile(profiles,body->name.c_str());
    if(selectedVisual&&body==selectedGeometry.get())bodyProfile=&*selectedVisual;
    if(!bodyProfile||!bodyProfile->authoredGlass)return false;
    const bool visible=bodyProfile->visual.surface.z>-1.f;
    auto instance=instances.find(body);
    if(instance!=instances.end()&&instance->second.nativeGlassFrame==frame)return !visible;
    auto* renderer=RE::BSGraphics::Renderer::GetRendererData();
    if(!renderer)return false;
    auto* target=renderer->renderTargets[RE::RENDER_TARGET::kMAIN].texture;
    auto* shadowMask=renderer->renderTargets[RE::RENDER_TARGET::kSHADOW_MASK].SRV;
    const bool reverseDepth=IsNativeGlassColorPass(ctx)&&UsesBottledWorldGlassDepth(ctx);
    BottleGlassDepthRequest depthRequest(ctx);
    if(reverseDepth)depthRequest.normalize();
    const bool captured=tmdReflections.Capture(ctx,body,target,oldDraw,count,start,base,
                                  reinterpret_cast<ID3D11ShaderResourceView*>(shadowMask),visible,reverseDepth);
#ifdef DBL_DEVELOPMENT
    TraceJessica(body,captured?"reflection-queued":"reflection-pass-through",ctx,count);
#endif
    if(captured&&tmdReflectionPoint){
        auto& bottle=instances[body];
        bottle.projectionLight=tmdReflectionPoint;
        bottle.projectionLightFrame=frame;
    }
    return captured||!visible;
}
// The bottle path shared by SE's DrawIndexed hook and Skyrim VR's instanced
// stereo draw (DrawBottleInstanced).
__forceinline void DrawBottle(ID3D11DeviceContext* ctx,UINT count,UINT start,INT base){
    bool handled=false;NativeGlassOrder glassOrder;
    {
        // Keep every rejection timed, but avoid entering the large renderer
        // stack frame for unrelated engine draws. Native draw time stays out.
        dblui::CPUCost cpuCost(dblui::CPUStage::Bottles);
        if(distanceTuning.renderer){
            const bool registered=!elSopaFittings.empty()&&CaptureSubmittedFitting(ctx,count);
            if(!registered&&fittingBottle)CaptureFitting(ctx,fittingBottle,fittingGeometry,false);
        }
        if(distanceTuning.renderer&&active&&profile){
            handled=TryRenderBottle(ctx,count,start,base,glassOrder);
        }
    }
    if constexpr(vrStereo){
        // TMD's authored shell and the shell refraction pass use one-view
        // screen-space shaders, so VR draws those bottles' native glass instead.
        if(glassOrder.active)glassOrder.Finish(DrawNativeBottleGlass,count,start,base);
        else if(!handled||(profile&&(profile->nativeGlassPrototype||profile->authoredGlass)))DrawNativeBottleGlass(ctx,count,start,base);
        return;
    }
    if(handled&&profile&&profile->authoredGlass){
        if(glassOrder.active)glassOrder.Finish(DrawAuthoredBottleGlass,count,start,base);
        else DrawAuthoredBottleGlass(ctx,count,start,base);
        ComPtr<ID3D11RenderTargetView> view;
        ctx->OMGetRenderTargets(1,&view,nullptr);
        ComPtr<ID3D11Resource> target;
        if(view)view->GetResource(&target);
        tmdReflections.DrawPending(ctx,target.Get(),active->geometry,profile->visual.surface.z>-1.f);
        return;
    }
    if(handled&&profile&&profile->nativeGlassPrototype&&!profile->shieldWetParticles&&
       (profile->visual.shellRefraction>0||profile->visual.straight>0)){
        float weight=1;
        if(auto* camera=RE::PlayerCamera::GetSingleton();camera&&camera->cameraRoot){
            auto delta=camera->cameraRoot->world.translate-active->geometry->world.translate;
            weight=distanceTuning.Weight(0,delta.Length());
        }
        if(glassOrder.active)ctx->RSSetState(glassOrder.front.Get());
        DrawShellTransmission(ctx,count,start,base,weight*profile->visual.shellRefraction,weight>0?profile->visual.straight:0);
    }
    if(glassOrder.active)glassOrder.Finish(DrawNativeBottleGlass,count,start,base);
    else if(!handled||(profile&&profile->nativeGlassPrototype)){
        // Fitting capture verifies the engine's original mask. Substitute the
        // projected mask only when the native receiver is about to draw.
        BottleReceiverMaskBinding receiverMask(ctx,!active&&!tmdReflectionBody?projectionReceiverPass:nullptr);
        DrawNativeBottleGlass(ctx,count,start,base);
    }
}
__declspec(noinline) void STDMETHODCALLTYPE draw(ID3D11DeviceContext* ctx,UINT count,UINT start,INT base) {
    if(vrDiagnostics.enabled){
        // Skyrim VR: bottles arrive through DrawBottleInstanced; plain draws pass through.
        vrDiagnostics.Observe(ctx,VRDiagnostics::kDrawIndexed,count,1,_ReturnAddress());
        PassThroughDraw(ctx,count,start,base);return;
    }
    if(tmdReflections.IsRecording()){
        oldDraw(ctx,count,start,base);
        return;
    }
    DrawNativeOpaqueShadow(ctx,count,start,base);
    if(tmdReflectionBody){
        try{
            if(QueueTmdReflection(ctx,count,start,base))return;
        }catch(const std::exception& e){
            static bool reported=false;
            if(!reported){
                reported=true;
                SKSE::log::warn("[DBL-TMD-reflection] Native draw fallback: {}",e.what());
            }
        }
    }
    if(shellCoverage>=0 && DrawTmdShell(ctx,count,start,base))return;
    DrawBottle(ctx,count,start,base);
}
void DrawBottleInstanced(ID3D11DeviceContext* ctx,UINT count,UINT instances,UINT start,INT base,UINT startInstance,DrawInstancedFn original){
    const InstancedCall* previous=instancedCall;
    const InstancedCall call{original,instances,startInstance};instancedCall=&call;
    DrawBottle(ctx,count,start,base);
    instancedCall=previous;
}
void* IndexedDrawEntry(ID3D11DeviceContext* context){
    return (*reinterpret_cast<void***>(context))[12];
}

// An entry added at runtime may forward to an entry we hooked at startup.
// Only the outer hook runs DBL. Both entries keep their own original call.
struct IndexedDrawScope {
    DrawFn previous=indexedDrawOriginal;
    explicit IndexedDrawScope(DrawFn original){indexedDrawOriginal=original;}
    ~IndexedDrawScope(){indexedDrawOriginal=previous;}
};

template<size_t Index>
void STDMETHODCALLTYPE DispatchIndexedDraw(ID3D11DeviceContext* context,UINT count,UINT first,INT base){
    IndexedDrawScope scope(indexedDrawHooks[Index].original);
    if(scope.previous){
        indexedDrawOriginal(context,count,first,base);
        return;
    }
    draw(context,count,first,base);
}

void STDMETHODCALLTYPE oldDraw(ID3D11DeviceContext* context,UINT count,UINT first,INT base){
    if constexpr(vrStereo)if(instancedCall){
        instancedCall->original(context,count,instancedCall->instances,first,base,instancedCall->startInstance);
        return;
    }
    PassThroughDraw(context,count,first,base);
}
// The engine's own draw, without VR instancing: upstream's oldDraw.
void STDMETHODCALLTYPE PassThroughDraw(ID3D11DeviceContext* context,UINT count,UINT first,INT base){
    if(indexedDrawOriginal){
        indexedDrawOriginal(context,count,first,base);
        return;
    }
    // HDR replay runs outside the indexed draw hook.
    IndexedDrawScope scope(indexedDrawHooks[0].original);
    context->DrawIndexed(count,first,base);
}

MH_STATUS HookIndexedDraw(void* target){
    constexpr DrawFn dispatchers[]={DispatchIndexedDraw<0>,DispatchIndexedDraw<1>,
        DispatchIndexedDraw<2>,DispatchIndexedDraw<3>};
    static_assert(std::size(dispatchers)==std::tuple_size_v<decltype(indexedDrawHooks)>);
    if(!target)return MH_ERROR_NOT_EXECUTABLE;
    for(size_t i=0;i<indexedDrawHooks.size();++i){
        auto& hook=indexedDrawHooks[i];
        if(hook.target==target)return MH_OK;
        if(hook.target)continue;
        if(auto status=MH_CreateHook(target,reinterpret_cast<void*>(dispatchers[i]),
                reinterpret_cast<void**>(&hook.original));status!=MH_OK)return status;
        if(auto status=MH_EnableHook(target);status!=MH_OK){
            MH_RemoveHook(target);
            hook={};
            return status;
        }
        hook.target=target;
        return MH_OK;
    }
    return MH_ERROR_MEMORY_ALLOC;
}

void RefreshIndexedDraw(ID3D11DeviceContext* context){
    static void* observed=indexedDrawHooks[0].target;
    if(!context)return;
    auto* entry=IndexedDrawEntry(context);
    if(entry==observed)return;
    observed=entry;
    auto status=HookIndexedDraw(entry);
    if(status!=MH_OK)SKSE::log::warn("[DBL-draw-entry] DrawIndexed hook recovery failed: {}",MH_StatusToString(status));
}

#ifdef DBL_DEVELOPMENT
#include "DevelopmentCamera.inc"
#endif
HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* s,UINT interval,UINT flags) {
    auto* renderer=RE::BSGraphics::Renderer::GetRendererData();
    const bool game=renderer&&IsGamePresentation(s,reinterpret_cast<IDXGISwapChain*>(renderer->renderWindows[0].swapChain),flags);
    if(vrDiagnostics.enabled)vrDiagnostics.Present(game);
    if(!game)return oldPresent(s,interval,flags);
#ifdef DBL_DEVELOPMENT
    ScheduleDevelopmentCameraGuard(frame);
#endif
    if(frame==1)SKSE::log::info("Present hook active");
    {dblui::CPUCost housekeeping(dblui::CPUStage::Housekeeping);
    if(opticalBuffers.finishFrame(frame)){
        SKSE::log::warn("[DBL-HDR-fallback] No matching HDR pass; using visible immediate rendering until a verified MAIN HDR boundary returns. Crowded-scene performance may be reduced. attempts={} declared_MAIN={} bound_mismatch={}",opticalBuffers.hdrAttempts,opticalBuffers.hdrVerified,opticalBuffers.hdrBoundMismatch);
    }
    if((frame+1)%120==0){std::erase_if(elSopaFittings,[&](const auto& p){return frame-p.second.visible>120;});std::erase_if(fittingSamples,[&](const auto& p){return p.second.seen==~0ull||frame-p.second.seen>120;});double t=now();std::erase_if(instances,[&](auto& v){return t-v.second.time>3;});}
    }
    const auto native=nativeOpaqueShadow.GetStats(frame);
    auto* savedActive=active;active=nullptr;
    try{dblui::Present(s,unsigned(instances.size()));}catch(const std::exception& e){SKSE::log::error("Performance UI skipped: {}",e.what());}
    active=savedActive;
    static std::string_view lastStatus;
    const auto status=native.confirmed?std::string_view("Native opaque shadow confirmed"):bottleProjection.Status(frame);
    if(status!=lastStatus){
        lastStatus=status;
        SKSE::log::info("[DBL-bottle-projection] mode=automatic native_confirmed={} mask_draws={} caustic_draws={} status='{}'",
                        native.confirmed,bottleProjection.ShadowCount(frame),bottleProjection.CausticCount(frame),status);
    }
    sceneSnapshot.depthLoaned=nativeGlassSnapshot.depthLoaned=false;
    SelectNearbyProjections();
    tmdReflections.Reset();
    RefreshIndexedDraw(reinterpret_cast<ID3D11DeviceContext*>(renderer->context));
    ++frame;
    return oldPresent(s,interval,flags);
}
void setup(RE::BSShader* shader,RE::BSRenderPass* pass,uint32_t flags) {
    projectionReceiverPass=nullptr;
    tmdReflectionPoint=nullptr;
    shellCoverage=-1;setupGeometry=nullptr;fittingBottle=nullptr;fittingGeometry=nullptr;active=nullptr;profile=nullptr;tmdReflectionBody=nullptr;
    // Only touch the tagged native glass or the TMD shell paired with its tagged sibling.
    RE::BSLightingShaderMaterialBase* tunedMaterial=nullptr;
    float savedAlpha=0,savedPower=0,savedSpec=0;
    if(pass&&pass->geometry&&(selectedVisual||!visualOriginal.empty()||pass->geometry->name=="Bottle"||pass->geometry->name=="Bottle01")){
        auto* g=pass->geometry;auto* owner=g;
        auto* tuned=FindBottleProfile(profiles,g->name.c_str());
        if(!tuned&&g->parent&&(g->name=="Bottle"||g->name=="Bottle01")){
            for(const auto& child:g->parent->GetChildren()){
                auto* sibling=child?child->AsGeometry():nullptr;if(!sibling)continue;
                auto* candidate=FindBottleProfile(profiles,sibling->name.c_str());
                if(candidate&&candidate->shieldWetParticles){tuned=candidate;owner=sibling;break;}
            }
        }
        if(selectedVisual&&owner==selectedGeometry.get())tuned=&*selectedVisual;
        if(tuned&&tuned->shieldWetParticles&&owner!=g){
            auto* prop=netimmerse_cast<RE::BSLightingShaderProperty*>(g->properties[1].get());
            if(prop&&prop->material){
                tunedMaterial=static_cast<RE::BSLightingShaderMaterialBase*>(prop->material);auto& mat=*tunedMaterial;
                savedAlpha=mat.materialAlpha;savedPower=mat.refractionPower;savedSpec=mat.specularColorScale;
                const auto& v=tuned->visual;
                if(tuned->shieldWetParticles&&owner!=g){
                    shellCoverage=(v.nativeAlpha>=0?v.nativeAlpha:.85f)*(1+v.surface.z);
                    shellBend=v.shellRefraction;shellMip=std::max(0,v.straight);
                    if(auto* camera=RE::PlayerCamera::GetSingleton();camera&&camera->cameraRoot){
                        auto delta=camera->cameraRoot->world.translate-g->world.translate;
                        auto weight=distanceTuning.Weight(0,delta.Length());shellBend*=weight;if(weight<=0)shellMip=0;
                    }
                }
                mat.materialAlpha=tuned->shieldWetParticles?1.f:(v.nativeAlpha>=0?v.nativeAlpha:savedAlpha)*(1+v.surface.z);

                if(v.highlight>=0)mat.specularColorScale*=v.highlight;
            }
        }
    }
    oldSetup(shader,pass,flags);
    if(tunedMaterial){tunedMaterial->materialAlpha=savedAlpha;tunedMaterial->refractionPower=savedPower;tunedMaterial->specularColorScale=savedSpec;}
    dblui::CPUCost hooks(dblui::CPUStage::Hooks);
    if(!pass||!pass->geometry)return;
    setupGeometry=pass->geometry;
    if(!bottleProjection.Casters(frame).empty()&&pass->numLights&&pass->sceneLights&&
       !SkipProjectionForNoShadow(*pass,WorldProjectionLights()))projectionReceiverPass=pass;
    if(pass->geometry->name=="Reflection"&&pass->geometry->parent){
        for(const auto& child:pass->geometry->parent->GetChildren()){
            auto* sibling=child?child->AsGeometry():nullptr;
            if(!sibling)continue;
            auto* candidate=FindBottleProfile(profiles,sibling->name.c_str());
            if(candidate&&candidate->authoredGlass){
                tmdReflectionBody=sibling;
                tmdReflectionPoint=SingleProjectionPointLight(*pass);
                break;
            }
        }
    }
    profile=FindBottleProfile(profiles,pass->geometry->name.c_str());
#ifdef DBL_DEVELOPMENT
    TraceMedieval(pass->geometry,profile?"lighting-setup":"profile-missing");
    TraceJessica(pass->geometry,profile?"lighting-setup":"profile-missing");
#endif
    if(selectedVisual && pass->geometry==selectedGeometry.get())profile=&*selectedVisual;
    if(profile){active=pass;if(vrDiagnostics.enabled)vrDiagnostics.Setup(pass);}
    else ObserveBottleFitting(pass);


}
void restore(RE::BSShader* shader,RE::BSRenderPass* pass,uint32_t flags) {
    if(vrDiagnostics.enabled&&active)vrDiagnostics.Restore(active);
    projectionReceiverPass=nullptr;
    tmdReflectionPoint=nullptr;
    shellCoverage=-1;setupGeometry=nullptr;fittingBottle=nullptr;fittingGeometry=nullptr;active=nullptr;profile=nullptr;tmdReflectionBody=nullptr;oldRestore(shader,pass,flags);
}
void setupUtility(RE::BSShader* shader,RE::BSRenderPass* pass,uint32_t flags){
    utilityPass=pass&&pass->geometry?pass:nullptr;
#ifdef DBL_DEVELOPMENT
    if(utilityPass)TraceMedieval(utilityPass->geometry,"utility-setup");
#endif
    oldUtilitySetup(shader,pass,flags);
}
void restoreUtility(RE::BSShader* shader,RE::BSRenderPass* pass,uint32_t flags){
    utilityPass=nullptr;
    oldUtilityRestore(shader,pass,flags);
}
void install() {
    auto* data=RE::BSGraphics::Renderer::GetRendererData();
    if(!data||!data->context||!data->forwarder)throw std::runtime_error("Renderer unavailable");
    initialize(reinterpret_cast<ID3D11Device*>(data->forwarder));
    forwardDraws.enabled=true;
    auto status=MH_Initialize();if(status!=MH_OK&&status!=MH_ERROR_ALREADY_INITIALIZED)throw std::runtime_error("MinHook initialization failed");
    auto** swap=*reinterpret_cast<void***>(data->renderWindows[0].swapChain);
    void* drawTarget=IndexedDrawEntry(reinterpret_cast<ID3D11DeviceContext*>(data->context));
    void* presentTarget=swap[8];
    SKSE::log::info("[DBL-receiver] receiverSearch=on");
#ifdef DBL_DEVELOPMENT
    bottledGlassDepthTrial=std::filesystem::exists(root/"BottledGlassDepth.enable");
    if(bottledGlassDepthTrial)SKSE::log::info("[DBL-glass-depth] Explicit MAIN depth override enabled");
    medievalTrace=std::filesystem::exists(root/"MedievalTrace.enable");
    if(medievalTrace)SKSE::log::info("[DBL-medieval-trace] enabled");
    jessicaTrace=std::filesystem::exists(root/"JessicaTrace.enable");
    if(jessicaTrace)SKSE::log::info("[DBL-jessica] enabled");
#endif
    if(MH_CreateHook(presentTarget,reinterpret_cast<void*>(&present),reinterpret_cast<void**>(&oldPresent))!=MH_OK)
        throw std::runtime_error("Present hook failed");
    auto drawStatus=HookIndexedDraw(drawTarget);
    auto presentStatus=drawStatus==MH_OK?MH_EnableHook(presentTarget):MH_UNKNOWN;
    SKSE::log::info("Hook activation: DrawIndexed={} Present={}",MH_StatusToString(drawStatus),MH_StatusToString(presentStatus));
    if(drawStatus!=MH_OK||presentStatus!=MH_OK){
        if(drawStatus==MH_OK)MH_RemoveHook(drawTarget);
        MH_RemoveHook(presentTarget);
        throw std::runtime_error("Hook activation failed; see individual statuses above");
    }
    if constexpr(vrStereo){
        // Immediate path only: the late optical composition assumes one view and
        // is not ported to stereo yet.
        forwardDraws.enabled=false;distanceTuning.optical=false;
        SKSE::log::info("[DBL-VR] Skyrim VR stereo rendering: immediate path with shared-snapshot refraction; late composition off");
        vrDiagnostics.Install(reinterpret_cast<ID3D11Device*>(data->forwarder),*reinterpret_cast<void***>(data->context));
        vrDiagnostics.enabled=true;
    }
    REL::Relocation<uintptr_t> vt{RE::VTABLE_BSLightingShader[0]};
    oldSetup=reinterpret_cast<GeometryFn>(vt.write_vfunc(6,&setup));oldRestore=reinterpret_cast<GeometryFn>(vt.write_vfunc(7,&restore));
    REL::Relocation<uintptr_t> utility{RE::VTABLE_BSUtilityShader[0]};
    oldUtilitySetup=reinterpret_cast<GeometryFn>(utility.write_vfunc(6,&setupUtility));
    oldUtilityRestore=reinterpret_cast<GeometryFn>(utility.write_vfunc(7,&restoreUtility));
    installHDRProbe();
    SKSE::log::info("Bottle hooks ready. {} loaded shape/appearance profiles.",profiles.size());
}

void ValidateBottleCoverage(){
    auto path=root/"assignments.json";
    if(!std::filesystem::exists(path))throw std::runtime_error("Missing assignments.json");
    std::ifstream input(path);nlohmann::json assignments;input>>assignments;
    if(assignments.size()!=303)throw std::runtime_error("Expected 303 base-game bottle assignments");
    auto* data=RE::TESDataHandler::GetSingleton();if(!data)throw std::runtime_error("Game data unavailable");
    std::vector<std::pair<RE::AlchemyItem*,std::string>> pending;
    std::unordered_set<RE::AlchemyItem*> seen;
    auto normalize=[](std::string s){for(auto& c:s){if(c=='/')c='\\';else c=char(std::tolower(static_cast<unsigned char>(c)));}return s;};
    unsigned missing=0,conflicts=0;std::string conflictNames;
    for(const auto& row:assignments){
        for(const auto& name:row.at("profiles"))if(!profiles.contains(name.get<std::string>()))throw std::runtime_error("Assignment references missing profile");
        std::string model=row.at("model");
        if(model!=row.at("original_model").get<std::string>())throw std::runtime_error("Base assignments must preserve vanilla model paths");
        if(!std::filesystem::exists(std::filesystem::path("Data/meshes")/model))throw std::runtime_error("Missing patched model: "+model);
        auto* item=data->LookupForm<RE::AlchemyItem>(row.at("form").get<uint32_t>(),row.at("plugin").get<std::string>());
        if(!item){++missing;SKSE::log::warn("[DBL-assignment] missing {}",row.at("editor_id").get<std::string>());continue;}
        if(!seen.insert(item).second)throw std::runtime_error("Duplicate base bottle assignment");
        const char* current=item->GetModel();
        if(normalize(current?current:"")!=normalize(row.at("original_model").get<std::string>())){
            // Expected with bottle replacers; reported once below.
            ++conflicts;conflictNames+=(conflictNames.empty()?"":", ")+row.at("editor_id").get<std::string>();continue;
        }
        pending.emplace_back(item,std::move(model));
    }
    if(conflicts)SKSE::log::info("[DBL-assignment] preserving {} non-vanilla models (replaced by another mod): {}",conflicts,conflictNames);
    // Validate coverage without redirecting vanilla model paths.
    SKSE::log::info("[DBL-coverage] validated={} missing={} model_conflicts={} expected=303; White Phial series excluded",pending.size(),missing,conflicts);
}
}
#ifdef DBL_SKYRIM_VR
// VR fork: upstream release (1.1.0) plus the VR build number.
constexpr REL::Version releaseVersion{1,1,0,1};
#else
constexpr REL::Version releaseVersion{1,1,0,0};
#endif
#ifdef DBL_SKYRIM_VR
// SKSE VR calls SKSEPlugin_Query and reports 1.4.15.1, so match the runtime family.
constexpr bool SkyrimVR(REL::Version runtime){return REL::Module::RuntimeFor(runtime)==REL::Module::Runtime::VR;}
extern "C" __declspec(dllexport) bool SKSEPlugin_Query(const SKSE::QueryInterface* skse, SKSE::PluginInfo* info) {
    info->infoVersion=SKSE::PluginInfo::kVersion;
    info->name="Dynamic Bottle Liquids SE";
    info->version=releaseVersion.pack();
    return !skse->IsEditor() && SkyrimVR(skse->RuntimeVersion());
}
#else
#if defined(DBL_STEAM_1597) || defined(DBL_STEAM_UNIFIED)
constexpr REL::Version steam1597{1,5,97,0};
extern "C" __declspec(dllexport) bool SKSEPlugin_Query(const SKSE::QueryInterface* skse, SKSE::PluginInfo* info) {
    info->infoVersion=SKSE::PluginInfo::kVersion;
    info->name="Dynamic Bottle Liquids SE";
    info->version=releaseVersion.pack();
    return !skse->IsEditor() && skse->RuntimeVersion()==steam1597;
}
#endif
#ifndef DBL_STEAM_1597
constexpr REL::Version steam1170{1,6,1170,0};
#ifdef DBL_STEAM_17104
constexpr REL::Version steam17104{1,7,104,0};
#endif
extern "C" __declspec(dllexport) constinit SKSE::PluginVersionData SKSEPlugin_Version=[] {
    SKSE::PluginVersionData v;v.PluginVersion(releaseVersion);v.PluginName("Dynamic Bottle Liquids SE");v.AuthorName("DynamicBottleLiquids contributors");
    v.UsesAddressLibrary();v.UsesStructsPost629();
#ifdef DBL_STEAM_UNIFIED
    v.UsesAddressLibraryV5();v.CompatibleVersions({steam1597,steam1170,steam17104});
#elif defined(DBL_STEAM_17104)
    v.UsesAddressLibraryV5();v.CompatibleVersions({steam1170,steam17104});
#else
    v.CompatibleVersions({steam1170});
#endif
    return v;
}();
#endif
#endif
extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* skse) {
    const auto runtime=skse->RuntimeVersion();
#if defined(DBL_SKYRIM_VR)
    if(!SkyrimVR(runtime))return false;
#elif defined(DBL_STEAM_UNIFIED)
    if(runtime!=steam1597&&runtime!=steam1170&&runtime!=steam17104)return false;
#elif defined(DBL_STEAM_1597)
    if(runtime!=steam1597)return false;
#elif defined(DBL_STEAM_17104)
    if(runtime!=steam1170&&runtime!=steam17104)return false;
#else
    if(runtime!=steam1170)return false;
#endif
    SKSE::Init(skse);
    auto path=SKSE::log::log_directory();if(!path)return false;
    auto sink=std::make_shared<spdlog::sinks::basic_file_sink_mt>((*path/"Dynamic Bottle Liquids SE.log").string(),true);
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("Dynamic Bottle Liquids SE",sink));spdlog::flush_on(spdlog::level::info);
#ifdef DBL_SKYRIM_VR
    spdlog::info("Dynamic Bottle Liquids SE VR {} | Skyrim {}",releaseVersion.string("."),runtime.string());
#else
    spdlog::info("Dynamic Bottle Liquids SE {}.{}.{} | Skyrim {}",
        releaseVersion.major(),releaseVersion.minor(),releaseVersion.patch(),runtime.string());
#endif
    #ifdef DBL_FRAMEWORK_UI
    // Collection starts only after the framework draws a DBL view.
    #elif defined(DBL_DIAGNOSTIC_DEFAULT_EXTENSIVE)
    dblui::SetMode(true,false);
#else
    dblui::SetMode(false,true);
#endif
#ifdef DBL_VR_TIMING
    dblui::EnableLogTiming(10);
#endif
    try{ApplyAcceptedSettings(distanceTuning);sharedBlurEnabled=true;loadProfiles();LoadSavedVisual();}catch(const std::exception& e){SKSE::log::error("Profile load failed: {}",e.what());return false;}
    if(!SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* m){
#ifdef DBL_DEVELOPMENT
        if(m->type==SKSE::MessagingInterface::kDataLoaded){
            auto* setting=RE::GetINISetting("bDisableAutoVanityMode:Camera");
            if(setting&&setting->GetType()==RE::Setting::Type::kBool){
                setting->data.b=true;
                SKSE::log::info("[DBL-development-camera] Automatic idle camera setting enabled");
            }else SKSE::log::warn("[DBL-development] Automatic idle camera setting unavailable");
            auto* delay=RE::GetINISetting("fAutoVanityModeDelay:Camera");
            if(delay&&delay->GetType()==RE::Setting::Type::kFloat)delay->data.f=24*60*60.f;
        }
#endif
        #ifdef DBL_FRAMEWORK_UI
        if(m->type==SKSE::MessagingInterface::kDataLoaded){dblui::RegisterFramework();RegisterVisualMenu();}
#endif
        if(m->type==SKSE::MessagingInterface::kDataLoaded){try{install();ValidateBottleCoverage();}catch(const std::exception& e){SKSE::log::error("Bottle initialization failed: {}",e.what());}}
        if(m->type==SKSE::MessagingInterface::kPreLoadGame||m->type==SKSE::MessagingInterface::kNewGame){
            selectedGeometry.reset();selectedVisual.reset();selectedName.clear();instances.clear();
            nearbyProjections={};bottleProjection.Reset();
            projectionLightFrame={};
            bubbleBudgetFrame=~0ull;grabbedBottleRoot.reset();active=nullptr;profile=nullptr;opticalBuffers.reset();
        }
    })){
        SKSE::log::error("Could not register the SKSE message listener");
        return false;
    }
    return true;
}
