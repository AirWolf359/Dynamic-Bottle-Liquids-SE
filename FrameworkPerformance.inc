/* SKSE Menu Framework 3 optional C exports. All callbacks run on its render
   thread. Only register after verifying the complete interface. */
void LabelDraw(int token,const char* name,uintptr_t instance){
    if(token<0||!full||!name)return;
    slots[token].label=name;slots[token].instance=instance;
}
namespace {
uint64_t measuredFrames=0;
std::map<std::string,double> profileTotals;
std::map<uintptr_t,FrameCosts::Frame::Bottle> instanceTotals;
void CompleteFrame(uint64_t,const FrameCosts::Frame& frame){
    if(!full)return;
    ++measuredFrames;
    for(const auto& [id,bottle]:frame.bottles){
        profileTotals[bottle.profile]+=bottle.ms;
        auto& total=instanceTotals[id];total.profile=bottle.profile;total.ms+=bottle.ms;total.draws+=bottle.draws;
    }
}
void ResetAttribution(){
    measuredFrames=0;profileTotals.clear();instanceTotals.clear();costs.onComplete=CompleteFrame;
}
void ReportAttribution(){
    if(!full||!measuredFrames)return;
    const double frames=double(measuredFrames);
    std::vector<std::pair<double,std::string>> profiles;
    for(const auto& [name,ms]:profileTotals)profiles.emplace_back(ms/frames,name);
    std::sort(profiles.rbegin(),profiles.rend());
    for(size_t i=0;i<std::min<size_t>(10,profiles.size());++i)
        spdlog::info("[DBL-profile-cost] rank={} profile={} draw_ms_per_frame={:.4f} baseline_frames={}",i+1,profiles[i].second,profiles[i].first,measuredFrames);
    std::vector<std::pair<double,uintptr_t>> instances;
    for(const auto& [id,total]:instanceTotals)instances.emplace_back(total.ms/frames,id);
    std::sort(instances.rbegin(),instances.rend());
    for(size_t i=0;i<std::min<size_t>(10,instances.size());++i){
        const auto& [ms,id]=instances[i];const auto& bottle=instanceTotals.at(id);
        spdlog::info("[DBL-instance-cost] rank={} shape={:x} profile={} draw_ms_per_frame={:.4f} draws={}",i+1,id,bottle.profile,ms,bottle.draws);
    }

}
struct HudPoint { float x,y; };
struct Framework {
    void (*section)(const char*,void (*)()){};
    int64_t (*hud)(void (*)()){};
    int64_t (*events)(void (*)(int),float){};
    void (*text)(const char*,const char*){};
    bool (*checkbox)(const char*,bool*){};
    bool (*button)(const char*,HudPoint){};
    void* (*foreground)(){};
    void (*addText)(void*,HudPoint,unsigned,const char*,const char*){};
} framework;

bool registered=false, compactRequested=false;
bool pageDrawn=false,hudDrawn=false,frameworkFrame=false,keyDown=false;
unsigned trackedBottles=0;
struct Shortcut { int key=VK_F8; bool ctrl=true,shift=false,alt=false; } shortcut;
const auto settingsPath=std::filesystem::absolute("Data/SKSE/Plugins/DBLPerformance.ini");

bool listening=false;
std::array<bool,256> heldKeys{};
bool Pressed(int key){return (GetAsyncKeyState(key)&0x8000)!=0;}
bool BindableKey(int key){
    return key>=VK_BACK && key<255 && key!=VK_ESCAPE && key!=VK_SHIFT &&
        key!=VK_CONTROL && key!=VK_MENU && key!=VK_LWIN && key!=VK_RWIN &&
        !(key>=VK_LSHIFT && key<=VK_RMENU) && MapVirtualKeyW(key,MAPVK_VK_TO_VSC_EX)!=0;
}
std::string ShortcutLabel(){
    UINT scan=MapVirtualKeyW(shortcut.key,MAPVK_VK_TO_VSC_EX);
    LONG flags=((scan&0xff)<<16) | ((scan&0xff00)?1<<24:0);
    wchar_t name[64]{};
    char utf8[256]{};
    if(GetKeyNameTextW(flags,name,64))WideCharToMultiByte(CP_UTF8,0,name,-1,utf8,sizeof(utf8),nullptr,nullptr);
    return std::format("Compact overlay shortcut: {}{}{}{}",shortcut.ctrl?"Ctrl+":"",
        shortcut.shift?"Shift+":"",shortcut.alt?"Alt+":"",*utf8?std::string(utf8):std::format("Key {}",shortcut.key));
}
void LoadShortcut(){
    auto read=[](const wchar_t* key,int fallback){
        return int(GetPrivateProfileIntW(L"CompactOverlay",key,fallback,settingsPath.c_str()));
    };
    int legacy=VK_F1+std::clamp(read(L"FunctionKey",8),1,12)-1;
    int key=read(L"VirtualKey",legacy);
    shortcut.key=BindableKey(key)?key:legacy;
    shortcut.ctrl=read(L"Ctrl",1)!=0;
    shortcut.shift=read(L"Shift",0)!=0;
    shortcut.alt=read(L"Alt",0)!=0;
}
bool SaveShortcut(){
    auto write=[](const wchar_t* key,int value){
        return WritePrivateProfileStringW(L"CompactOverlay",key,std::to_wstring(value).c_str(),settingsPath.c_str())!=0;
    };
    return write(L"VirtualKey",shortcut.key)&&write(L"Ctrl",shortcut.ctrl)&&
        write(L"Shift",shortcut.shift)&&write(L"Alt",shortcut.alt);
}
bool Focused(){
    DWORD process=0;
    HWND window=GetForegroundWindow();
    GetWindowThreadProcessId(window,&process);
    return process==GetCurrentProcessId()&&!IsIconic(window);
}
void RenderHud(){
    bool down=Pressed(shortcut.key);
    bool focused=Focused();
    if(focused&&!listening&&down&&!keyDown&&Pressed(VK_CONTROL)==shortcut.ctrl&&
       Pressed(VK_SHIFT)==shortcut.shift&&Pressed(VK_MENU)==shortcut.alt)
        compactRequested=!compactRequested;
    keyDown=down;
    if(!compactRequested||!focused)return;
    auto draw=framework.foreground();
    if(!draw)return;
    hudDrawn=true;
    auto sample=ReadFrameSummary();
    auto gpu=costs.Reliable()?std::format("{:.3f} ms",sample.gpuAverage):std::string("sampling...");
    auto label=std::format("DBL   CPU {:.3f} ms   GPU {}   Bottles {}",sample.cpuAverage,gpu,trackedBottles);
    framework.addText(draw,{17,17},0xff000000,label.c_str(),nullptr);
    framework.addText(draw,{16,16},0xffffffff,label.c_str(),nullptr);
}
void RenderPage(){
    if(!Focused()){listening=false;return;}
    pageDrawn=true;
    framework.checkbox("Show compact overlay",&compactRequested);
    static bool saveFailed=false;
    if(!listening){
        if(framework.button(ShortcutLabel().c_str(),{0,0})){
            for(int key=VK_BACK;key<255;++key)heldKeys[key]=Pressed(key);
            listening=true;
        }
    }else{
        framework.text("Press a shortcut. Esc cancels.",nullptr);
        for(int key=VK_BACK;key<255;++key){
            bool down=Pressed(key);
            bool fresh=down&&!heldKeys[key];
            heldKeys[key]=down;
            if(!fresh)continue;
            if(key==VK_ESCAPE){listening=false;break;}
            if(!BindableKey(key))continue;
            shortcut={key,Pressed(VK_CONTROL),Pressed(VK_SHIFT),Pressed(VK_MENU)};
            keyDown=true;
            listening=false;
            saveFailed=!SaveShortcut();
            break;
        }
    }
    if(saveFailed)framework.text("Could not save the shortcut. It applies for this session.",nullptr);
    if(framework.button("Reset measurements",{0,0}))Reset();
    auto line=[](const std::string& s){framework.text(s.c_str(),nullptr);};
    auto frame=ReadFrameSummary();
    line(std::format("DBL CPU {:.3f} ms/frame average, {:.3f} ms peak",frame.cpuAverage,costs.cpuMax));
    line(costs.Reliable()?std::format("GPU {:.3f} ms/sampled frame, {:.3f} ms peak",frame.gpuAverage,costs.gpuMax):
                          std::string("GPU timing is warming up or has insufficient valid samples."));
    line(std::format("{} tracked bottles, {} simulation updates, {} bubbles on last update",trackedBottles,updates,bubbles));
    const char* cpuNames[]{"Hook matching","Bottle CPU","Late composition","Housekeeping"};
    const char* gpuNames[]{"Preparation and copies","Bottle draws","Late composition and copies"};
    for(unsigned i=0;i<4;++i)line(std::format("{}: {:.3f} ms/frame",cpuNames[i],costs.cpuFrames?costs.cpuSum[i]/costs.cpuFrames:0));
    if(costs.Reliable())for(unsigned i=0;i<3;++i)line(std::format("GPU {}: {:.3f} ms/frame",gpuNames[i],costs.gpuSum[i]/costs.gpuFrames));
    for(unsigned i=0;i<7;++i)line(std::format("{}: {:.3f} avg / {:.3f} peak ms/update",nuka::SimulationStageNames[i],updates?sums[i]/updates:0,peaks[i]));
    line(std::format("Rest pose cache: {} hits, {} solves",bottleDetails.restHits,bottleDetails.restBuilds));
    line(std::format("GPU sample frames: {} complete, {} dropped",frame.gpuFrames,frame.droppedFrames));
}
void LogTimingSummary(){
    if(!costs.cpuFrames)return;
    ReportAttribution();
    auto avg=ReadFrameSummary();
    const double divisor=double(std::max<uint64_t>(1,costs.gpuFrames));
    spdlog::info("[DBL-frame-cost] mode={} cpu_ms={:.3f} gpu_ms={:.3f} reliable={} prepare_ms={:.3f} draw_ms={:.3f} compose_ms={:.3f} sampled_frames={} dropped={} tracked={}",
        full?"detailed":"compact",avg.cpuAverage,avg.gpuAverage,costs.Reliable(),
        costs.gpuSum[0]/divisor,costs.gpuSum[1]/divisor,costs.gpuSum[2]/divisor,
        avg.gpuFrames,avg.droppedFrames,trackedBottles);
}
void Visibility(bool detailed,bool brief){
    if(full==detailed&&compact==brief)return;
    if(Enabled())LogTimingSummary();
    if(!detailed)listening=false;
    SetMode(detailed,brief);
}
void FrameworkEvent(int event){
    if(event==3){pageDrawn=hudDrawn=false;} // BeforeRender
    if(event==4){ // AfterRender: also fires after changing pages.
        frameworkFrame=true;
        Visibility(pageDrawn,hudDrawn&&!pageDrawn);
    }
    if(event==2&&full)Visibility(false,false); // CloseMenu
}
template<class T> bool Bind(HMODULE module,T& target,const char* name){
    target=reinterpret_cast<T>(GetProcAddress(module,name));
    return target!=nullptr;
}
}

void RegisterFramework(){
    if(registered)return;
    auto module=GetModuleHandleW(L"SKSEMenuFramework.dll");
    if(!module)return;
    float (*version)(){};
    if(!Bind(module,version,"GetMenuFrameworkVersion")||version()<3||
       !Bind(module,framework.section,"AddSectionItem")||!Bind(module,framework.hud,"RegisterHudElement")||
       !Bind(module,framework.events,"RegisterEventPriority")||!Bind(module,framework.text,"igTextUnformatted")||
       !Bind(module,framework.checkbox,"igCheckbox")||
       !Bind(module,framework.button,"igButton")||!Bind(module,framework.foreground,"igGetForegroundDrawList_Nil")||
       !Bind(module,framework.addText,"ImDrawList_AddText_Vec2")){
        spdlog::info("DBL performance overlay requires SKSE Menu Framework 3 HUD and page APIs");
        return;
    }
    LoadShortcut();
    framework.events(FrameworkEvent,0.f);
    framework.hud(RenderHud);
    framework.section("Dynamic Bottle Liquids SE/Performance",RenderPage);
    registered=true;
}
void Present(IDXGISwapChain*,unsigned instances,bool){
    if(!registered)return;
    if(!frameworkFrame)Visibility(false,false);
    frameworkFrame=false;
    if(!Enabled())return;
    trackedBottles=instances;
    CloseQueryFrame();
    Poll();
    static const double milliseconds=[](){LARGE_INTEGER f;QueryPerformanceFrequency(&f);return 1000./f.QuadPart;}();
    for(unsigned i=0;i<4;++i){costs.CPU(i,cpuClock.ticks[i]*milliseconds);cpuClock.ticks[i]=0;}
    costs.Seal();
    if(costs.cpuFrames%300==0)LogTimingSummary();
}
