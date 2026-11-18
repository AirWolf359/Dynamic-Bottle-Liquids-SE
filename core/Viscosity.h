#pragma once
#include <algorithm>
#include <cmath>
namespace nuka {
// Art-direction response for the three surface modes, derived from viscosity.
// Fine/medium strength and decay are additional to the shared response below.
struct ViscousWaveResponse {
 float broadSpeed=1,mediumSpeed=1,fineSpeed=1,mediumStrength=1,fineStrength=1,mediumDecay=1,fineDecay=1;
};
inline ViscousWaveResponse ViscousWaves(float value){
 if(value<=1)return {};
 const float points[]{1,2,5,30,100};
 const ViscousWaveResponse values[]={{},{.95f,.90f,.80f,.90f,.80f,.90f,.80f},
 {.80f,.65f,.45f,.65f,.40f,.75f,.50f},{.45f,.25f,.10f,.35f,.10f,.50f,.25f},
 {.25f,.12f,.04f,.15f,.025f,.30f,.12f}};
 int i=1;while(i<4&&value>points[i])++i;
 float t=std::clamp(std::log(value/points[i-1])/std::log(points[i]/points[i-1]),0.f,1.f);
 auto a=values[i-1],b=values[i];
 return {std::lerp(a.broadSpeed,b.broadSpeed,t),std::lerp(a.mediumSpeed,b.mediumSpeed,t),std::lerp(a.fineSpeed,b.fineSpeed,t),
 std::lerp(a.mediumStrength,b.mediumStrength,t),std::lerp(a.fineStrength,b.fineStrength,t),
 std::lerp(a.mediumDecay,b.mediumDecay,t),std::lerp(a.fineDecay,b.fineDecay,t)};
}
struct ViscosityResponse {float damping=1,level=1,ripple=1,flow=1,rise=1,film=1,height=1,width=1,rimTime=1,excitation=1;};
inline ViscosityResponse Viscosity(float value){
 if(value<=1)return {};
 const float points[]{1,3,10,30,100};
 const ViscosityResponse values[]={{},{1.4f,1.2f,.75f,.8f,.5f,1.5f,1.15f,1.1f,1.3f,.9f},
 {2.2f,1.8f,.45f,.5f,.2f,2.5f,1.4f,1.25f,2,.7f},{3.5f,3,.25f,.3f,.08f,4,1.8f,1.5f,3.5f,.45f},
 {5,5,.12f,.15f,.03f,7,2,1.7f,6,.25f}};
 int i=1;while(i<4&&value>points[i])++i;
 float t=std::clamp(std::log(value/points[i-1])/std::log(points[i]/points[i-1]),0.f,1.f);
 auto a=values[i-1],b=values[i];
 return {std::lerp(a.damping,b.damping,t),std::lerp(a.level,b.level,t),std::lerp(a.ripple,b.ripple,t),std::lerp(a.flow,b.flow,t),std::lerp(a.rise,b.rise,t),std::lerp(a.film,b.film,t),std::lerp(a.height,b.height,t),std::lerp(a.width,b.width,t),std::lerp(a.rimTime,b.rimTime,t),std::lerp(a.excitation,b.excitation,t)};
}
inline constexpr float CarbonationBaseRadius=.10f;
inline float CarbonationRadius(float viscosity){
 // The fourth root offsets only part of the rise penalty from viscosity
 // after the simulation squares the relative bubble radius.
 return CarbonationBaseRadius*std::min(1.5f,std::pow(Viscosity(viscosity).rise,-.25f));
}
}
