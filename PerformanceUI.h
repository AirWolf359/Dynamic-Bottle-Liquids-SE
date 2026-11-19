#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include "core/SimulationTimings.h"
#include <chrono>
namespace dblui {
enum class GPUStage {Prepare,Draw,Compose};
enum class CPUStage {Hooks,Bottles,Compose,Housekeeping};
struct FrameSummary {unsigned long long cpuFrames,gpuFrames,droppedFrames;double cpuAverage,gpuAverage;};
FrameSummary ReadFrameSummary();
struct GPUQueryStats {unsigned long long clockScopes{},timestampWrites{};};
GPUQueryStats ReadGPUQueryStats();
void AddCPU(CPUStage,double);
// Render-thread clock accumulation: every scope is measured, conversion occurs
// once per frame. The monitor already owns all counters on this same thread.
struct CPUClockState {bool enabled=false;unsigned epoch=0;long long ticks[4]{};};
extern CPUClockState cpuClock;
struct CPUCost {
 CPUStage stage;bool enabled;unsigned epoch;LARGE_INTEGER start{};
 explicit CPUCost(CPUStage s):stage(s),enabled(cpuClock.enabled),epoch(cpuClock.epoch){if(enabled)QueryPerformanceCounter(&start);}
 ~CPUCost(){if(enabled){LARGE_INTEGER end;QueryPerformanceCounter(&end);if(cpuClock.enabled&&epoch==cpuClock.epoch)cpuClock.ticks[unsigned(stage)]+=end.QuadPart-start.QuadPart;}}
};
bool Enabled();
bool Extensive();
enum class BottleStage {State,Pose,Optical,Upload,Submit};
struct BottleDetailSummary {double ms[5]{};unsigned long long tiers[5]{},scheduled[3]{},restHits=0,restBuilds=0;double restMs=0;};
BottleDetailSummary ReadBottleDetails();
void AddBottleDetail(BottleStage,double);
void AddTier(unsigned);
void AddScheduled(unsigned);
void AddResting(bool hit,double ms);
struct BottleTimer {
 bool enabled;BottleStage stage=BottleStage::State;std::chrono::steady_clock::time_point start;
 BottleTimer():enabled(Extensive()){if(enabled)start=std::chrono::steady_clock::now();}
 void Switch(BottleStage next){if(enabled){auto end=std::chrono::steady_clock::now();AddBottleDetail(stage,std::chrono::duration<double,std::milli>(end-start).count());start=end;}stage=next;}
 ~BottleTimer(){Switch(stage);}
};
void AddSimulation(const nuka::SimulationTimings&,unsigned bubbles);
void AddSection(unsigned hits,unsigned rebuilds,float area,float depth);
int BeginGPU(ID3D11DeviceContext*,GPUStage stage=GPUStage::Draw);
void EndGPU(ID3D11DeviceContext*,int);
struct GPUCost {ID3D11DeviceContext* context;int token;GPUCost(ID3D11DeviceContext* c,GPUStage s,bool measure=true):context(c),token(measure?BeginGPU(c,s):-1){}~GPUCost(){EndGPU(context,token);}};
#ifdef DBL_VR_TIMING
// Test builds: run the detailed timers without the overlay. Present logs the
// summary and resets every window.
void EnableLogTiming(double windowSeconds);
#endif
#ifdef DBL_FRAMEWORK_UI
void RegisterFramework();
void LabelDraw(int token,const char* name,uintptr_t instance);
#else
inline void LabelDraw(int,const char*,uintptr_t){}
#endif
void SetMode(bool full,bool compact);
void Present(IDXGISwapChain*,unsigned instances,bool allowUnfocused=false);
}
