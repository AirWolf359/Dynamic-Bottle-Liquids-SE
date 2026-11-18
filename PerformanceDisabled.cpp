// Normal runtime: no ImGui, hotkeys, GPU queries, or profiling allocation.
#include "PerformanceUI.h"
namespace dblui {
CPUClockState cpuClock{};
FrameSummary ReadFrameSummary(){return {};}
GPUQueryStats ReadGPUQueryStats(){return {};}
BottleDetailSummary ReadBottleDetails(){return {};}
bool Enabled(){return false;}
bool Extensive(){return false;}
void AddCPU(CPUStage,double){}
void AddBottleDetail(BottleStage,double){}
void AddTier(unsigned){}
void AddScheduled(unsigned){}
void AddResting(bool,double){}
void AddSimulation(const nuka::SimulationTimings&,unsigned){}
void AddSection(unsigned,unsigned,float,float){}
int BeginGPU(ID3D11DeviceContext*,GPUStage){return -1;}
void EndGPU(ID3D11DeviceContext*,int){}
void SetMode(bool,bool){}
void Present(IDXGISwapChain*,unsigned,bool){}
}
