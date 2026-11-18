#pragma once
#include "ShotBubbleConfig.h"
#include "Viscosity.h"
#include "SceneBubbleBudget.h"
#include "SimulationTimings.h"
#include <array>
#include <vector>
#include <random>
#include <memory>
#include <cstddef>
#include <cstdint>

namespace nuka {
struct Vec { float x{},y{},z{}; Vec operator+(Vec b)const{return{x+b.x,y+b.y,z+b.z};} Vec operator-(Vec b)const{return{x-b.x,y-b.y,z-b.z};} Vec operator*(float a)const{return{x*a,y*a,z*a};} };
float Dot(Vec a,Vec b);
float Length(Vec a);
Vec Normalize(Vec a);
struct CavitySection {size_t begin{},end{};Vec low{},high{};};
struct alignas(16) V4 { float x{},y{},z{},w{}; };
// Shared prefix with Bottle.hlsl; FO4Bottle.hlsl additionally consumes meshToWorld.
struct alignas(16) Constants {
    float matrix[16]{};
    V4 eye{},plane{},rings[16]{};
    V4 wave{},optics{},trace{},absorption{},scatter{},emission{},glassAbsorption{},foamColor{};
    V4 bubbles[256]{};
    int bubbleCount{}; float padding[3]{};
    int ringCount{};float capZ{},foam{};int debug{};
    // Native reflection basis, independent of the canonical liquid coordinate system.
    V4 meshToWorld[3]{{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    V4 surfaceDirection[4]{},surfaceLight[4]{};
    float sunMatrices[4][16]{};
    V4 sunBounds[4]{},sunInfo{},sunSlices{},sunVolume{},worldOrigin{},materialFlags{};
    V4 bubbleGroups[32]{}; // conservative bounds for eight nearby spheres
    V4 wetFilm[64]{}; // 16 height rows x 16 angular samples, packed four per vector
    V4 wetInfo{}; // exposed strength, bottom, height-to-row scale, reserved
    V4 wetTuning{0,.15f,.8f,3.5f};
    V4 waveDetail{0,1,1,0}; // secondary amplitude, signed gains, surface-noise envelope
    // Authored Effect cubemap strength, lighting influence, LIGHTING variant, appearance reflection multiplier.
    // Independent of the liquid/detail distance weights.
    V4 surfaceEnvironment{1,1,0,0};
    V4 simulationToMesh[3]{{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    // Original BGSM response for converted glass. Effect materials stay live.
    V4 surfaceSpecular{1,1,1,1}; // colour, multiplier
    V4 surfaceResponse{1,5,0,1}; // smoothness, Fresnel power, BGSM family, original env scale
    V4 surfaceAlpha{1,0,0,0}; // live base alpha, FALLOFF, reserved, reserved
    V4 surfaceFalloff{0,1,1,1}; // live native falloff constants (already in cosine space)
    // Appended only: all native surface/transform constant offsets stay unchanged.
    V4 bubbleShape[256]{}; // local unit axis + volume-preserving axial stretch; zero = sphere
    V4 liquidGradient[16]{}; // linear RGB, bottom-to-surface; optional material rule
    V4 gradientInfo{}; // count, projected cavity bottom, inverse liquid depth, reserved
    V4 surfacePop[16]{}; // packed burst phase for up to 64 visible surface bubbles
    V4 cavityPlanes[16]{}; // xy support coefficients, [0].z bound scale, [0].w count
    int shakingCount{},surfaceCount{},carbonationCount{},countPadding{}; // CPU submission metadata
    V4 tunedModes[3]{}; // xyz spatial wave vector; w accumulated phase
    V4 tunedInfo{};
    V4 viscousSurface{}; // meniscus height/width deltas, reserved // enabled; reserved (interaction/noise disabled)
};
static_assert(sizeof(Constants)==11872);
static_assert(offsetof(Constants,bubbleShape)==6896);
struct SurfaceSection { Vec center{},u{1,0,0},v{0,1,0},normal{0,0,1};float rx=1,ry=1,area=3.14159f,depth=1; };
// Immutable while shared by motions. Phases depend on cavity samples and wave
// vectors, not wave amplitudes, elapsed phase, bubbles or wet-film history.
struct TunedSampleBasis {
    std::array<V4,3> modes{};
    const V4* sampleData=nullptr;const V4* wallData=nullptr;uint64_t revision=0;
    std::vector<V4> primarySecondary,medium,wallPhases;
    Vec projectionNormal{};std::vector<float> volumeDots,wallDots;
};
struct Settings {
    float viscosity=0; // zero preserves legacy behavior; 1..100 relative to water
    Constants base{};
    float fill{},frequency{},damping{},gain{},maxTilt{},waveAmplitude{},waveFrequency{},foamStrength{};
    ShotBubbleConfig shotConfig;
    bool shotBubbles=false; // Isolated Acid Soaker emission; no resting carbonation.
    float intensity{},baseline{},movement{},radiusMin{},radiusMax{},rise{},settle{};
    float broadWaveDecay=1.2f, waveTimeScale=1.f;
    float mediumWaveAmplitude=0;
    float surfaceImpactBoost=0; // Extra height only for energetic broad waves.
    float surfaceLifetime=2;
    float surfaceLifetimeJitter=0;
    bool previewTuning=false;
    bool tunedInteraction=false;
    SurfaceSection referenceSurface{};
    std::vector<Vec> cavityEdges; // Shared immutable profile geometry, paired endpoints.
    std::vector<CavitySection> cavitySections; // Conservative bounds, rebuilt with edges.
    std::vector<V4> samples;
    uint64_t sampleRevision=0;double sampleWeightTotal=0;size_t sampleWeightCount=0;
    mutable std::vector<std::weak_ptr<TunedSampleBasis>> tunedSampleCache;
    std::vector<float> sampleMeniscus;
    // Shape-only meniscus geometry: wall distance and local radius. The current
    // viscosity still supplies height/width each update, with identical math.
    std::vector<V4> sampleMeniscusGeometry,wetMeniscusGeometry;
    std::array<V4,16> meniscusRings{},meniscusPlanes{};
    int meniscusRingCount=0;
    // sin/cos of each sample's fixed diagonal wave phase; built once per profile.
    std::vector<V4> sampleWaveBasis, sampleMediumBasis;
    float sampleWaveFrequency{};
    std::vector<V4> wetSamples,wetPhases;
    std::array<V4,16> wetRings{};int wetRingCount=0;float wetFrequency=0;
    float shakingBubbles{}; // 0..1, independent of resting carbonation
};
struct Particle {Vec position;float radius,speed,age;float baseRadius{};Vec velocity{},shapeAxis{0,0,1};float stretch=1,stretchVelocity{};};
inline float ShakingLifetime(float phase){return 8.f+4.f*phase/6.2831853f;}
struct ShakingParticle {Vec position,velocity;float radius{},baseRadius{},age{},phase{};Vec shapeAxis{0,0,1};float stretch=1,stretchVelocity{};};
struct SurfaceBubble {Vec position;float radius{},age{},visibleRadius{},lifetime{};};
struct Motion {
    Vec liquidUp{0,0,1};bool liquidUpValid=false;float rimHeight=0,solvedRim=-1;
    BubbleLimits bubbleLimits;
    unsigned pendingShots=0;
    double last{};
    float time{},activity{},credit{},foamActivity{},waveEnergy{},rippleEnergy{},surfaceNoise{},mediumEnergy{};
    float secondaryEnergy{},surfaceAge=1,sectionFill=-1;
    SurfaceSection section{};Vec sectionNormal{};
    float modePhase[3]{0,0,1.1f},modeRate[3]{1,1,1},modeDecay[3]{1,1,1};
    float sloshRate=1,sloshDecay=1,heightGain=1;
    V4 modeVectors[3]{};
    std::shared_ptr<TunedSampleBasis> tunedSamples;
    unsigned sectionBuilds{},sectionHits{};
    Vec waveMix{1,1,0};
    float restWorldThreshold=0,worldScale=1,quietSurfaceAge=0,surfaceDisplacement=0;
    Vec lastPosition{},lastUp{0,0,1},velocity{},slosh{},sloshVelocity{};
    std::array<float,256> wetFilm{};
    // Cache wall contact geometry at rest; wetness itself still evolves.
    std::array<float,256> wetDistances{};
    std::array<V4,16> wetGeometryRings{},wetGeometryPlanes{};
    int wetGeometryRingCount=0;
    bool wetGeometryValid=false;
    unsigned wetGeometryBuilds=0,wetGeometryHits=0;
    std::vector<Particle> bubbles;
    std::vector<SurfaceBubble> surfaceBubbles;
    uint32_t foamRandom=0x6d2b79f5u; // Independent of motion and carbonation RNGs.
    std::array<Vec,4> bubbleSources{};
    std::array<bool,4> bubbleSourceValid{};
    std::vector<std::pair<float,float>> volumeScratch;
    Vec solvedNormal{};
    float solvedHeight{},solvedFill{},solvedAmplitude=-1,solvedMedium=-1,solvedSecondary=-1;
    std::mt19937 random{12345};
    float shakeActivity{},shakeCredit{},shakeAngularSpeed{};
    std::vector<ShakingParticle> shaking;
    std::mt19937 shakeRandom{54321}; // Never advances the carbonation RNG.
    Vec shakeOmega{},previousShakeAcceleration{},previousShakeOmega{};
    float previousShakeDrive{},shakeBurstCooldown{};
    Vec emissionOmega{},previousEmissionOmega{};
    bool emissionValid=true;
    float rotationMemory{},filteredAngularAcceleration{},flowEnergy{};
    // Emission-only measurements. Physical slosh and render transforms retain
    // the original world acceleration/basis. Translation is carry-relative.
    float emissionVerticalSpeed{},emissionVerticalAcceleration{};
    bool emissionTranslationValid=false;
    float rotationStroke{},verticalStroke{},verticalStrokeSpeed{},previousVerticalSpeed{};
    float bubbleSizeSpeed{},emissionSpeedHold{},bubbleSizeHold{};
    float verticalPeakAge{},triggerSizeSpeed{};
    unsigned triggerSources{},shakingBirths{};
    unsigned carbTransfer{},carbExpiry{},carbTiny{},carbInvalid{},carbBudget{},carbCleared{},carbContain{};
    unsigned foamRetained{},foamFitRejected{},shakeTimedOut{},shakeWallBlocked{},shakeTransferred{},shakeSurfacePopped{};
};
inline void ApplyBubbleLimits(Motion& m,BubbleLimits limits){
    m.bubbleLimits=limits;
    if(m.shaking.size()>limits.shaking)m.shaking.resize(limits.shaking);
    if(m.surfaceBubbles.size()>limits.foam)m.surfaceBubbles.resize(limits.foam);
    if(m.bubbles.size()>limits.carbonation)m.bubbles.resize(limits.carbonation);
}
inline bool WithinEffectDistance(Vec bottle,Vec camera){auto d=bottle-camera;return Dot(d,d)<=768.f*768.f;}
inline bool WithinDetailDistance(Vec bottle,Vec camera){auto d=bottle-camera;return Dot(d,d)<=256.f*256.f;}
inline void ApplyDetailDistance(Constants& c,bool detail){c.optics.w=detail?0.f:1.f;if(!detail){c.trace.z=0;c.bubbleCount=0;c.scatter.w=0;c.wetInfo={};}}
void MakeSamples(Settings& s);
void BuildCavityEdges(Settings& s);
SurfaceSection MeasureSurface(const Settings& s,Vec normal,float fill,Vec previousU={1,0,0});
float TunedWave(const Constants& c,Vec p,float primary,float secondary,float medium);
float MeniscusHeight(const Constants& c,Vec p);
Constants Simulate(const Settings& s,Motion& m,Vec localAcceleration,Vec up,float dt,bool detail=true,bool held=false,float motionWeight=1.f,SimulationTimings* timings=nullptr);
}
