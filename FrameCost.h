#pragma once
#include "PerformanceUI.h"
#include <array>
#include <map>
#include <string>
#include <cstdint>
#include <algorithm>
namespace dblui {
// Query results may arrive several Presents later. Only complete frames enter
// GPU averages; zero-work frames count, partial/invalid frames never underreport.
struct FrameCosts {
 struct Frame {std::array<unsigned,gpuStages> calls{};std::array<double,gpuStages> gpu{};unsigned pending=0;bool sealed=false,bad=false;
#ifdef DBL_FRAMEWORK_UI
 struct Bottle {std::string profile;double ms=0;unsigned draws=0;};
 std::map<uintptr_t,Bottle> bottles;
#endif
 };
#ifdef DBL_FRAMEWORK_UI
 void (*onComplete)(uint64_t,const Frame&)=nullptr;
#endif
 std::map<uint64_t,Frame> frames;
 uint64_t serial=0,cpuFrames=0,gpuFrames=0,droppedFrames=0;
 std::array<double,4> cpuCurrent{},cpuSum{},cpuPeak{};
 std::array<double,gpuStages> gpuSum{},gpuPeak{};
 std::array<uint64_t,gpuStages> gpuCalls{};
 double cpuMax=0,gpuMax=0;
 uint64_t lastComplete=0;unsigned sampleInterval=1;
 bool Selected()const{return serial%sampleInterval==0;}
 bool Reliable() const {return gpuFrames>=30 && gpuFrames*100>=95*(gpuFrames+droppedFrames) && serial<=lastComplete+sampleInterval+8;}
 void CPU(unsigned stage,double ms){cpuCurrent[stage]+=ms;}
 uint64_t Begin(){auto& f=frames[serial];++f.pending;return serial;}
 void Skip(){frames[serial].bad=true;}
 void Resolve(uint64_t id,unsigned stage,double ms,bool valid){auto it=frames.find(id);if(it==frames.end())return;auto& f=it->second;if(f.pending)--f.pending;f.bad|=!valid;if(valid){f.gpu[stage]+=ms;++f.calls[stage];}Collect();}
 void Seal(){if(Selected()||frames.contains(serial))frames[serial].sealed=true;++serial;++cpuFrames;double sum=0;for(unsigned i=0;i<4;++i){sum+=cpuCurrent[i];cpuSum[i]+=cpuCurrent[i];cpuPeak[i]=std::max(cpuPeak[i],cpuCurrent[i]);}cpuMax=std::max(cpuMax,sum);cpuCurrent={};Collect();
  while(frames.size()>128){frames.erase(frames.begin());++droppedFrames;}
 }
 void Collect(){for(auto it=frames.begin();it!=frames.end();){auto& f=it->second;if(!f.sealed||f.pending){++it;continue;}if(f.bad)++droppedFrames;else{
#ifdef DBL_FRAMEWORK_UI
 if(onComplete)onComplete(it->first,f);
#endif
 lastComplete=std::max(lastComplete,it->first+1);double sum=0;++gpuFrames;for(unsigned i=0;i<gpuStages;++i){gpuCalls[i]+=f.calls[i];sum+=f.gpu[i];gpuSum[i]+=f.gpu[i];gpuPeak[i]=std::max(gpuPeak[i],f.gpu[i]);}gpuMax=std::max(gpuMax,sum);}it=frames.erase(it);}}
};
}
