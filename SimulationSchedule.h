#pragma once
#include <algorithm>
#include <cmath>
struct SimulationSchedule {
 static float Phase(float a,float b,float t){return a+std::remainder(b-a,6.283185307f)*t;}
 static float Interval(float distance){return distance>512?0.f:(distance<=192?1.f/60:distance<=384?1.f/30:1.f/15);}
 float elapsed=0,interval=0,phase=0;
 bool zeroTimeCached=false;
 bool Tick(float dt,float distance,bool moved,bool reset){
  interval=Interval(distance);
  if(distance>512){elapsed=phase=0;return false;}
  if(reset){elapsed=phase=0;bool rebuild=!zeroTimeCached||moved;zeroTimeCached=true;return rebuild;}
  zeroTimeCached=false;
  elapsed+=dt;phase+=dt;
  if(phase+1e-6f<interval)return false;
  // Retain fractional scheduling time: 144 FPS must not turn 60 Hz into 48 Hz.
  phase=std::fmod(std::max(phase,interval),interval);return true;
 }
 float Consume(){float dt=elapsed;elapsed=0;return dt;}
 float Alpha()const{return interval>0?std::clamp(phase/interval,0.f,1.f):1.f;}
};
