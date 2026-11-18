#pragma once
#include <array>
#include <algorithm>
struct DistanceTuning {
 static constexpr std::array<const char*,21> names={"glass_refraction","liquid_refraction","suspended_bubbles","surface_bubbles","wet_film","slosh","waves","reflection_ripples","surface_normals","surface_highlights_reflections","meniscus","glass_highlights_reflections","texture_detail","scene_lighting","liquid_absorption","liquid_transmission","liquid_thickness_shading","base_glass_shading","fresnel_opacity","liquid_cavity_tracing","liquid_simulation"};
 bool renderer=true,simulation=true,liquidTrace=true,baseGlass=true,fresnel=true,optical=true;
 struct Range {bool enabled=false;float start=192,end=256;};
 std::array<Range,21> ranges{};
 DistanceTuning(){for(int i=0;i<7;++i)ranges[i]={true,i==5||i==6?1024.f:192.f,i==5||i==6?1280.f:256.f};}
 // Outer glass does not require a moving liquid simulation or dense trace.
 static float LiquidActivity(const std::array<float,20>& weights){float result=0;for(int k=1;k<7;++k)result=std::max(result,weights[k]);return result;}
 float Weight(int i,float distance)const{auto r=ranges[i];if(!r.enabled)return 1;float t=std::clamp((distance-r.start)/(r.end-r.start),0.f,1.f);return 1-t*t*(3-2*t);}
};
