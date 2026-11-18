#pragma once
#include "Simulation.h"
// Only interpolated liquid state is cached; textures and camera state stay live.
struct LiquidPose {
 nuka::V4 plane{},wave{},detail{},modes[3]{};float clock=0;
 void Capture(const nuka::Constants& c,float time){plane=c.plane;wave=c.wave;detail=c.waveDetail;for(int i=0;i<3;++i)modes[i]=c.tunedModes[i];clock=time;}
 void Apply(nuka::Constants& c,float& time)const{c.plane=plane;c.wave=wave;c.waveDetail=detail;for(int i=0;i<3;++i)c.tunedModes[i]=modes[i];time=clock;}
};
