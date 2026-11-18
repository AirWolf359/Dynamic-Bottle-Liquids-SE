#include "PerformanceUI.h"
#include "FrameCost.h"
#include <windows.h>
#include <wrl/client.h>
#ifndef DBL_FRAMEWORK_UI
#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#endif
#include <array>
#include <filesystem>
#include <format>
#include <vector>
#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>
namespace dblui {
CPUClockState cpuClock;
using Microsoft::WRL::ComPtr;
namespace {
bool full=false,compact=false,failed=false;
#ifndef DBL_FRAMEWORK_UI
ImGuiContext* ui=nullptr;HWND window=nullptr;
ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;

#endif
// The swap-chain/overlay context may be an ENB wrapper. Timing belongs to
// the context that actually submits DBL draws, including query ownership.
ComPtr<ID3D11DeviceContext> queryContext;
bool queryInitializationFailed=false;
struct Slot{std::string label;uintptr_t instance=0;ComPtr<ID3D11Query> start,end;bool pending=false;unsigned epoch=0;uint64_t frame=0;GPUStage stage=GPUStage::Draw;};
std::array<Slot,4096> slots;
struct QueryFrame{ComPtr<ID3D11Query> disjoint;std::vector<int> tokens;bool pending=false,closed=false;};
std::array<QueryFrame,32> queryFrames;
int activeQueryFrame=-1;
unsigned nextSlot=0;
void CloseQueryFrame(){if(activeQueryFrame>=0){auto& f=queryFrames[activeQueryFrame];queryContext->End(f.disjoint.Get());f.closed=true;activeQueryFrame=-1;nextSlot=0;}}
FrameCosts costs;
GPUQueryStats queryStats;
BottleDetailSummary bottleDetails;
unsigned epoch=0,skipped=0,invalid=0,gpuCount=0,updates=0,bubbles=0;
double gpuSum=0,gpuMax=0; nuka::SimulationTimings sums{},peaks{};
unsigned sectionHits=0,sectionBuilds=0;float sectionArea=0,sectionDepth=0;
#ifndef DBL_FRAMEWORK_UI
std::array<bool,4> keys{};
auto last=std::chrono::steady_clock::now();
#endif
#ifdef DBL_FRAMEWORK_UI
void ResetAttribution();
#endif
void Reset(){cpuClock.ticks[0]=cpuClock.ticks[1]=cpuClock.ticks[2]=cpuClock.ticks[3]=0;++cpuClock.epoch;cpuClock.enabled=!failed&&(full||compact);CloseQueryFrame();++epoch;costs={};costs.sampleInterval=16;
#ifdef DBL_FRAMEWORK_UI
ResetAttribution();
#endif
queryStats={};bottleDetails={};sums={};peaks={};gpuSum=gpuMax=0;gpuCount=updates=bubbles=skipped=invalid=0;sectionHits=sectionBuilds=0;}
#ifndef DBL_FRAMEWORK_UI
bool Edge(int key,int index,bool focus){bool down=focus&&(GetAsyncKeyState(key)&0x8000)&&(GetAsyncKeyState(VK_CONTROL)&0x8000)&&!(GetAsyncKeyState(VK_MENU)&0x8000)&&!(GetAsyncKeyState(VK_SHIFT)&0x8000);bool hit=down&&!keys[index];keys[index]=down;return hit;}
bool Initialize(IDXGISwapChain* swap){
 DXGI_SWAP_CHAIN_DESC desc{};if(FAILED(swap->GetDesc(&desc))||FAILED(swap->GetDevice(IID_PPV_ARGS(&device))))return false;
 window=desc.OutputWindow;device->GetImmediateContext(&context);
 ui=ImGui::CreateContext();ImGui::SetCurrentContext(ui);auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
 io.ConfigFlags|=ImGuiConfigFlags_NoMouse|ImGuiConfigFlags_NoMouseCursorChange;
 ImGui::StyleColorsDark();
 if(!ImGui_ImplWin32_Init(window)||!ImGui_ImplDX11_Init(device.Get(),context.Get()))return false;
 return true;
}
#endif
bool InitializeQueries(ID3D11DeviceContext* ctx){
 if(queryInitializationFailed||!ctx||ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
 ComPtr<ID3D11Device> owner;ctx->GetDevice(&owner);if(!owner)return false;
 auto create=[&](D3D11_QUERY kind,ComPtr<ID3D11Query>& query){D3D11_QUERY_DESC desc{kind,0};return SUCCEEDED(owner->CreateQuery(&desc,&query));};
 bool ok=true;
 for(auto& f:queryFrames){ok=create(D3D11_QUERY_TIMESTAMP_DISJOINT,f.disjoint)&&ok;f.tokens.reserve(256);}

 if(!ok){queryInitializationFailed=true;spdlog::error("[DBL-GPU-context] Query creation failed on draw device; GPU timing unavailable");return false;}
 queryContext=ctx;
 spdlog::info("[DBL-GPU-context] Timing queries use the bottle draw context");
 return true;
}
void Poll(){for(auto& f:queryFrames){if(!f.pending||!f.closed)continue;
 D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};auto hr=queryContext->GetData(f.disjoint.Get(),&d,sizeof(d),D3D11_ASYNC_GETDATA_DONOTFLUSH);if(hr==S_FALSE)continue;
 bool outstanding=false;
 for(auto& token:f.tokens){if(token<0)continue;auto& s=slots[token];UINT64 a=0,b=0;
  auto ha=FAILED(hr)?hr:queryContext->GetData(s.start.Get(),&a,sizeof(a),D3D11_ASYNC_GETDATA_DONOTFLUSH);
  auto hb=FAILED(hr)?hr:queryContext->GetData(s.end.Get(),&b,sizeof(b),D3D11_ASYNC_GETDATA_DONOTFLUSH);
  if(ha==S_FALSE||hb==S_FALSE){outstanding=true;continue;}
  s.pending=false;token=-1;if(s.epoch!=epoch)continue;
  bool valid=SUCCEEDED(hr)&&SUCCEEDED(ha)&&SUCCEEDED(hb)&&!d.Disjoint&&d.Frequency&&b>=a;
  if(!valid){++invalid;costs.Resolve(s.frame,unsigned(s.stage),0,false);continue;}
  double ms=1000.*double(b-a)/d.Frequency;
#ifdef DBL_FRAMEWORK_UI
  if(full&&s.instance&&costs.frames.contains(s.frame)){
   auto& bottle=costs.frames.at(s.frame).bottles[s.instance];
   bottle.profile=s.label;bottle.ms+=ms;++bottle.draws;
  }
#endif
  costs.Resolve(s.frame,unsigned(s.stage),ms,true);
  if(full&&s.stage==GPUStage::Draw){gpuSum+=ms;gpuMax=std::max(gpuMax,ms);++gpuCount;}
 }
 if(!outstanding){f.pending=f.closed=false;f.tokens.clear();}
}}

}
GPUQueryStats ReadGPUQueryStats(){return queryStats;}
FrameSummary ReadFrameSummary(){double cpu=0,gpu=0;for(double x:costs.cpuSum)cpu+=x;for(double x:costs.gpuSum)gpu+=x;return {costs.cpuFrames,costs.gpuFrames,costs.droppedFrames,costs.cpuFrames?cpu/costs.cpuFrames:0,costs.gpuFrames?gpu/costs.gpuFrames:0};}
BottleDetailSummary ReadBottleDetails(){return bottleDetails;}
void AddBottleDetail(BottleStage stage,double ms){if(Extensive())bottleDetails.ms[unsigned(stage)]+=ms;}
void AddScheduled(unsigned tier){if(Extensive()&&tier<3)++bottleDetails.scheduled[tier];}
void AddTier(unsigned tier){if(Extensive()&&tier<5)++bottleDetails.tiers[tier];}
void AddResting(bool hit,double ms){if(!Extensive())return;if(hit)++bottleDetails.restHits;else ++bottleDetails.restBuilds;bottleDetails.restMs+=ms;}
void AddCPU(CPUStage stage,double ms){if(Enabled())costs.CPU(unsigned(stage),ms);}
bool Enabled(){return !failed&&(full||compact);}
bool Extensive(){return !failed&&full;}
void AddSection(unsigned hits,unsigned rebuilds,float area,float depth){if(!Extensive())return;sectionHits+=hits;sectionBuilds+=rebuilds;sectionArea=area;sectionDepth=depth;}
void SetMode(bool detailed,bool brief){full=detailed;compact=!detailed&&brief;Reset();spdlog::info("[DBL-diagnostics] mode={}",full?"extensive":compact?"simple":"off");}
void AddSimulation(const nuka::SimulationTimings& values,unsigned count){if(!Extensive())return;++updates;bubbles=count;for(unsigned i=0;i<7;i++){sums[i]+=values[i];peaks[i]=std::max(peaks[i],values[i]);}}
int BeginGPU(ID3D11DeviceContext* ctx,GPUStage stage){
 if(!Enabled()||!costs.Selected())return -1;
#ifndef DBL_FRAMEWORK_UI
 if(!ui){costs.Skip();return -1;}
#endif
 if((!queryContext&&!InitializeQueries(ctx))||ctx!=queryContext.Get()){++skipped;costs.Skip();return -1;}
 if(activeQueryFrame<0){
  for(unsigned i=0;i<queryFrames.size();++i){auto& f=queryFrames[i];if(!f.pending&&f.disjoint){f.pending=true;f.closed=false;f.tokens.clear();activeQueryFrame=int(i);ctx->Begin(f.disjoint.Get());++queryStats.clockScopes;break;}}
  if(activeQueryFrame<0){++skipped;costs.Skip();return -1;}
 }
 for(unsigned attempt=0;attempt<slots.size();++attempt){unsigned i=nextSlot;nextSlot=(nextSlot+1)%slots.size();auto& s=slots[i];if(!s.pending){
  if(!s.start||!s.end){
   ComPtr<ID3D11Device> owner;ctx->GetDevice(&owner);
   D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP,0};
   if(!owner||FAILED(owner->CreateQuery(&desc,s.start.ReleaseAndGetAddressOf()))||
      FAILED(owner->CreateQuery(&desc,s.end.ReleaseAndGetAddressOf()))){++skipped;costs.Skip();return -1;}
  }
  s.instance=0;s.pending=true;s.epoch=epoch;s.stage=stage;s.frame=costs.Begin();queryFrames[activeQueryFrame].tokens.push_back(int(i));ctx->End(s.start.Get());++queryStats.timestampWrites;return int(i);
 }}++skipped;costs.Skip();return -1;
}
void EndGPU(ID3D11DeviceContext*,int token){if(token>=0){queryContext->End(slots[token].end.Get());++queryStats.timestampWrites;}}
#ifdef DBL_FRAMEWORK_UI
#include "FrameworkPerformance.inc"
#else
void Present(IDXGISwapChain* swap,unsigned instances,bool allowUnfocused){
 CloseQueryFrame();
 if(failed)return;
 DXGI_SWAP_CHAIN_DESC sd{};if(FAILED(swap->GetDesc(&sd)))return;
 bool focused=GetForegroundWindow()==sd.OutputWindow;
 if(Edge(VK_F7,0,focused)){full=!full;if(full)compact=false;Reset();}
 if(Edge(VK_F8,1,focused)){compact=!compact;if(compact)full=false;Reset();}
 if(Edge(VK_F9,2,focused))Reset();
 if(focused&&(GetAsyncKeyState(VK_ESCAPE)&0x8000)){full=false;compact=false;}
 cpuClock.enabled=Enabled();
 if(!ui&&!Enabled())return;
 auto* previous=ImGui::GetCurrentContext();
 if(!ui){if(!Initialize(swap)){failed=true;cpuClock.enabled=false;spdlog::error("DBL performance UI initialization failed; bottle rendering continues");ImGui::SetCurrentContext(previous);return;}}
 ImGui::SetCurrentContext(ui);Poll();if(Enabled()){
  static const double tickMilliseconds=[](){LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);return 1000./double(frequency.QuadPart);}();
  for(unsigned i=0;i<4;++i){costs.CPU(i,double(cpuClock.ticks[i])*tickMilliseconds);cpuClock.ticks[i]=0;}
  costs.Seal();
 }
 if(!Enabled()||(!focused&&!allowUnfocused)||IsIconic(window)){ImGui::SetCurrentContext(previous);return;}
 // Never retain the swap-chain buffer across frames/resizes.
 ComPtr<ID3D11Texture2D> back;ComPtr<ID3D11RenderTargetView> target;
 if(FAILED(swap->GetBuffer(0,IID_PPV_ARGS(&back)))||FAILED(device->CreateRenderTargetView(back.Get(),nullptr,&target))){ImGui::SetCurrentContext(previous);return;}
 ImGui_ImplDX11_NewFrame();ImGui_ImplWin32_NewFrame();
 float dpi=ImGui_ImplWin32_GetDpiScaleForHwnd(window);ImGui::GetIO().FontGlobalScale=dpi;
 ImGui::NewFrame();ImGui::SetNextWindowPos(ImVec2(16*dpi,16*dpi),ImGuiCond_Always);
 ImGui::SetNextWindowBgAlpha(.90f);ImGui::SetNextWindowSizeConstraints(ImVec2(0,0),ImVec2(std::max(100.f,ImGui::GetIO().DisplaySize.x-32*dpi),std::max(100.f,ImGui::GetIO().DisplaySize.y-32*dpi)));
 auto flags=ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoFocusOnAppearing;
 ImGui::Begin(full?"DBL - Performance diagnostics":"DBL - Performance",nullptr,flags);
 double sum=0;for(auto v:sums)sum+=v;
 double cpuTotal=0,gpuTotal=0;for(double v:costs.cpuSum)cpuTotal+=v;for(double v:costs.gpuSum)gpuTotal+=v;
 ImGui::Text("CPU mod: %.3f ms/frame avg | %.3f peak",costs.cpuFrames?cpuTotal/costs.cpuFrames:0,costs.cpuMax);
 if(costs.Reliable())ImGui::Text("GPU elapsed: %.3f ms/sampled frame | %.3f sampled peak",gpuTotal/costs.gpuFrames,costs.gpuMax);else ImGui::TextUnformatted("GPU elapsed: insufficient samples (warming up, dropped or stale)");
 if(full){
 ImGui::Text("Tracked bottles: %u | Simulation updates: %u",instances,updates);
 ImGui::Text("CPU simulation: %.3f ms/update avg",updates?sum/updates:0);
 if(gpuCount&&costs.Reliable())ImGui::Text("GPU draw interval: %.3f ms/draw avg | %.3f peak",gpuSum/gpuCount,gpuMax);else ImGui::TextUnformatted("GPU draw interval: waiting for samples");
 const char* cpuNames[]={"Hook matching","Bottle CPU (includes simulation)","Late composition CPU","Housekeeping"};
 const char* gpuNames[]={"Preparation/copies","All bottle passes","Late composition/copies"};
 for(unsigned i=0;i<4;++i)ImGui::Text("CPU %s: %.3f ms/frame",cpuNames[i],costs.cpuFrames?costs.cpuSum[i]/costs.cpuFrames:0);
 if(costs.Reliable())for(unsigned i=0;i<3;++i)ImGui::Text("GPU %s: %.3f ms/frame",gpuNames[i],costs.gpuFrames?costs.gpuSum[i]/costs.gpuFrames:0);
 ImGui::Text("Complete sampled GPU frames: %llu | dropped: %llu",(unsigned long long)costs.gpuFrames,(unsigned long long)costs.droppedFrames);
 const char* detailNames[]={"State queries","Pose/constants","Optical setup","Buffer upload","Draw/restore"};
 for(unsigned i=0;i<5;++i)ImGui::Text("Bottle CPU %s: %.3f ms/frame",detailNames[i],costs.cpuFrames?bottleDetails.ms[i]/costs.cpuFrames:0);
 ImGui::Text("Rest pose: %llu hits / %llu solves | %.3f ms/frame (inside pose CPU)",bottleDetails.restHits,bottleDetails.restBuilds,costs.cpuFrames?bottleDetails.restMs/costs.cpuFrames:0);
 ImGui::Text("Bottle frames: 60Hz %llu / 30Hz %llu / 15Hz %llu",bottleDetails.tiers[0],bottleDetails.tiers[1],bottleDetails.tiers[2]);
 ImGui::Text("Bottle frames: frozen %llu / inactive %llu",bottleDetails.tiers[3],bottleDetails.tiers[4]);
 ImGui::Text("Timed updates: 60Hz %llu / 30Hz %llu / 15Hz %llu",bottleDetails.scheduled[0],bottleDetails.scheduled[1],bottleDetails.scheduled[2]);
 ImGui::Separator();for(unsigned i=0;i<7;i++)ImGui::Text("%s: %.3f avg / %.3f peak ms",nuka::SimulationStageNames[i],updates?sums[i]/updates:0,peaks[i]);
 ImGui::Separator();ImGui::Text("GPU samples: %u | skipped: %u | invalid: %u",gpuCount,skipped,invalid);
 ImGui::Text("Last updated bottle: %u rendered bubbles",bubbles);
 ImGui::TextUnformatted("Ctrl+F7 extensive | Ctrl+F8 simple | Ctrl+F9 reset | Esc hide");
 ImGui::TextUnformatted("Read-only panel; gameplay input stays active.");
 ImGui::TextUnformatted("GPU elapsed intervals include possible waits; not isolated execution cost.");
 ImGui::TextUnformatted("CPU/GPU overlap: do not add. Excludes monitor UI and startup.");
 ImGui::Text("Cavity cache: %u hits / %u rebuilds",sectionHits,sectionBuilds);
 ImGui::Text("Last surface: area %.2f | depth %.2f (mesh units)",sectionArea,sectionDepth);
 ImGui::TextUnformatted("GPU: 1/16 frames, including zero-bottle frames; never multiplied by 16.");}
 if(compact)ImGui::TextUnformatted("Ctrl+F7 extensive | Ctrl+F8 toggle | Esc hide\nCPU/GPU overlap; GPU intervals may include waits.");
 ImGui::End();ImGui::Render();
 ID3D11RenderTargetView* old[8]{};ComPtr<ID3D11DepthStencilView> depth;context->OMGetRenderTargets(8,old,&depth);
 auto* rtv=target.Get();context->OMSetRenderTargets(1,&rtv,nullptr);ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());context->OMSetRenderTargets(8,old,depth.Get());for(auto* p:old)if(p)p->Release();
 ImGui::SetCurrentContext(previous);
 auto now=std::chrono::steady_clock::now();if(now-last>std::chrono::seconds(5)){last=now;if(compact){spdlog::info("[DBL-total-simple] CPU ms/frame={} GPU elapsed ms/sampled_frame={} frames={} gpu_frames={} valid={} dropped={}",costs.cpuFrames?cpuTotal/costs.cpuFrames:0,costs.Reliable()?gpuTotal/costs.gpuFrames:-1,costs.cpuFrames,costs.gpuFrames,costs.Reliable(),costs.droppedFrames);return;}spdlog::info("[DBL-sampling] status={} capacity=4096 interval=16 shared_disjoint=true CPU_all_frames=true GPU_sampled_frames=true GPU_metric=elapsed_query_interval_including_possible_waits GPU_total_minus_one_means_unavailable",costs.Reliable()?"valid":"insufficient");spdlog::info("[DBL-total] CPU ms/frame={} peak={} frames={} GPU ms/frame={} peak={} complete_frames={} dropped_frames={} CPU hooks/bottles/compose/housekeeping={}/{}/{}/{} GPU prepare/draw/compose={}/{}/{}",costs.cpuFrames?cpuTotal/costs.cpuFrames:0,costs.cpuMax,costs.cpuFrames,costs.Reliable()?gpuTotal/costs.gpuFrames:-1,costs.gpuMax,costs.gpuFrames,costs.droppedFrames,costs.cpuFrames?costs.cpuSum[0]/costs.cpuFrames:0,costs.cpuFrames?costs.cpuSum[1]/costs.cpuFrames:0,costs.cpuFrames?costs.cpuSum[2]/costs.cpuFrames:0,costs.cpuFrames?costs.cpuSum[3]/costs.cpuFrames:0,costs.gpuFrames?costs.gpuSum[0]/costs.gpuFrames:0,costs.gpuFrames?costs.gpuSum[1]/costs.gpuFrames:0,costs.gpuFrames?costs.gpuSum[2]/costs.gpuFrames:0);double divisor=double(std::max<uint64_t>(costs.cpuFrames,1));double accounted=0;for(double ms:bottleDetails.ms)accounted+=ms;
 spdlog::info("[DBL-bottle-other] ms/frame={} (early rejection and timing overhead)",std::max(0.0,costs.cpuSum[1]-accounted)/divisor);
 spdlog::info("[DBL-bottle-cpu] ms/frame state={} pose_constants={} optical_setup={} upload={} draw_restore={} motion_sim={} rest_cache={} rest_hits={} rest_solves={} (motion_sim/rest_cache are included in pose_constants)",bottleDetails.ms[0]/divisor,bottleDetails.ms[1]/divisor,bottleDetails.ms[2]/divisor,bottleDetails.ms[3]/divisor,bottleDetails.ms[4]/divisor,sum/divisor,bottleDetails.restMs/divisor,bottleDetails.restHits,bottleDetails.restBuilds);
 spdlog::info("[DBL-scheduled] nonzero_dt_updates 60Hz={} 30Hz={} 15Hz={}",bottleDetails.scheduled[0],bottleDetails.scheduled[1],bottleDetails.scheduled[2]);
 double updateDivisor=std::max(1u,updates);
 spdlog::info("[DBL-sim-stages] ms/update motion_section={} volume={} wet_film={} shaking={} surface_foam={} carbonation={} packing={} section_hits={} section_rebuilds={}",sums[0]/updateDivisor,sums[1]/updateDivisor,sums[2]/updateDivisor,sums[3]/updateDivisor,sums[4]/updateDivisor,sums[5]/updateDivisor,sums[6]/updateDivisor,sectionHits,sectionBuilds);
 spdlog::info("[DBL-tiers] bottle_frames 60Hz={} 30Hz={} 15Hz={} frozen={} inactive={} (unique drawn instance per frame; cumulative since reset)",bottleDetails.tiers[0],bottleDetails.tiers[1],bottleDetails.tiers[2],bottleDetails.tiers[3],bottleDetails.tiers[4]);
 spdlog::info("[DBL-perf] CPU simulation ms/update={} updates={} GPU bottle ms/draw={} samples={} tracked={} skipped={} invalid={}",updates?sum/updates:0,updates,gpuCount?gpuSum/gpuCount:0,gpuCount,instances,skipped,invalid);}
}
#endif
}
