#pragma once
// Frozen from the accepted Test146 live settings; no tuner or file polling.
inline void ApplyAcceptedSettings(DistanceTuning& settings){
 settings.ranges[0]={true,192.f,256.f}; // glass_refraction
 settings.ranges[1]={true,192.f,256.f}; // liquid_refraction
 settings.ranges[2]={true,192.f,256.f}; // suspended_bubbles
 settings.ranges[3]={true,192.f,256.f}; // surface_bubbles
 settings.ranges[4]={true,192.f,256.f}; // wet_film
 settings.ranges[5]={true,512.f,768.f}; // slosh
 settings.ranges[6]={true,256.f,384.f}; // waves
 settings.ranges[7]={true,256.f,384.f}; // reflection_ripples
 settings.ranges[8]={true,0.f,384.f}; // surface_normals
 settings.ranges[9]={true,256.f,384.f}; // surface_highlights_reflections
 settings.ranges[10]={true,192.f,256.f}; // meniscus
 settings.ranges[11]={false,512.f,768.f}; // glass_highlights_reflections
 settings.ranges[12]={false,1024.f,2048.f}; // texture_detail
 settings.ranges[13]={false,1024.f,2048.f}; // scene_lighting
 settings.ranges[14]={true,1024.f,2048.f}; // liquid_absorption
 settings.ranges[15]={false,1024.f,2048.f}; // liquid_transmission
 settings.ranges[16]={true,512.f,768.f}; // liquid_thickness_shading
 settings.ranges[17]={false,1024.f,2048.f}; // base_glass_shading
 settings.ranges[18]={true,1024.f,2048.f}; // fresnel_opacity
 settings.ranges[19]={true,1024.f,2048.f}; // liquid_cavity_tracing
 settings.ranges[20]={true,512.f,768.f}; // liquid_simulation
 settings.renderer=true;
 settings.simulation=true;
 settings.liquidTrace=true;
 settings.baseGlass=true;
 settings.fresnel=true;
 settings.optical=true;
}
