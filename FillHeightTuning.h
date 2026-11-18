#pragma once
#include "Simulation.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
struct FillHeightTuning {
 bool enabled=false;float offset=0;double next=0;
 std::filesystem::file_time_type stamp{};
 float Effective()const{return enabled?offset:0;}
 static double Volume(const nuka::Constants& c,double height){
  double sum=0;
  for(int i=1;i<c.ringCount;++i){auto a=c.rings[i-1],b=c.rings[i];double end=std::min(height,double(b.x));if(end<=a.x)break;
   double r=a.y+(b.y-a.y)*(end-a.x)/(b.x-a.x);sum+=(end-a.x)*(a.y*a.y+a.y*r+r*r)/3;
  }return sum;
 }
 static double Height(const nuka::Constants& c,float fill){
  double low=c.rings[0].x,high=c.rings[c.ringCount-1].x,target=Volume(c,high)*fill;
  for(int i=0;i<48;++i){double mid=(low+high)*.5;if(Volume(c,mid)<target)low=mid;else high=mid;}
  return ((low+high)*.5-c.rings[0].x)/(c.rings[c.ringCount-1].x-c.rings[0].x);
 }
 static float Adjust(const nuka::Constants& c,float original,double designedHeight,float points){
  if(points==0)return original; // Exact restoration, never accumulate rounding/drift.
  double fraction=std::clamp(designedHeight+points/100.0,.01,.99);
  double h=c.rings[0].x+fraction*(c.rings[c.ringCount-1].x-c.rings[0].x);
  return float(std::clamp(Volume(c,h)/Volume(c,c.rings[c.ringCount-1].x),.000001,.999999));
 }
 bool Reload(const std::filesystem::path& path,double time){
  if(time<next)return false;next=time+1;
  std::error_code ec;auto t=std::filesystem::last_write_time(path,ec);if(ec||t==stamp)return false;stamp=t;
  try{std::ifstream f(path);auto j=nlohmann::json::parse(f);
   bool on=j.at("enabled").get<bool>();float value=j.at("height_offset_points").get<float>();
   if(!std::isfinite(value)||value < -100||value>100)throw std::runtime_error("Height offset must be between -100 and +100 percentage points");
   // Test116 folds the accepted Test115 -22-point setting into profile fills.
   // Older tuner files describe offsets from Test114; translate once on read.
   if(on&&j.value("baseline_version",115)<116)value=std::clamp(value+22.f,-100.f,100.f);
   float previous=Effective();enabled=on;offset=value;
   spdlog::info("[DBL-fill-height] enabled={} offset_points={} effective_points={}",enabled,offset,Effective());
   try{std::ofstream ack(path.wstring()+L".applied.json");ack<<nlohmann::json{{"revision",j.value("_revision",std::string{})},{"effective_points",Effective()},{"process",GetCurrentProcessId()}}.dump();}catch(...){}
   return previous!=Effective();
  }catch(const std::exception& e){spdlog::warn("[DBL-fill-height] retained previous setting: {}",e.what());return false;}
 }
};
