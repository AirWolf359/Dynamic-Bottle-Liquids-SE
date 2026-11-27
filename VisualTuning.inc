using VisualJson=nlohmann::json;
const std::filesystem::path& VisualDirectory(){
    static const auto path=[]{
        wchar_t base[32768]{};
        auto count=GetEnvironmentVariableW(L"LOCALAPPDATA",base,32768);
        if(!count||count>=32768)throw std::runtime_error("LOCALAPPDATA unavailable");
        return std::filesystem::path(base)/"SkyrimDBL"/"VisualSettings";
    }();
    return path;
}
std::unordered_map<std::string,Profile> visualOriginal;
std::optional<Profile> selectedVisual;
RE::NiPointer<RE::BSGeometry> selectedGeometry;
void ApplyVisual(Profile& p,const VisualJson& j){
    auto number=[&](const char* key,float fallback,float lo,float hi){
        if(!j.contains(key))return fallback;
        float n=j.at(key).get<float>();
        if(!std::isfinite(n)||n<lo||n>hi)throw std::runtime_error(key);
        return n;
    };
    auto& v=p.visual;
    v.nativeAlpha=number("Glass opacity",v.nativeAlpha,0,1);
    if(v.nativeAlpha>=0)p.glassOpacity=v.nativeAlpha;
    if(p.nativeGlassPrototype)v.shellRefraction=number("Glass refraction strength",v.shellRefraction,0,8);
    else p.glassRefractionScale=number("Glass refraction strength",p.glassRefractionScale,0,8);

    v.highlight=number("Glass highlights",-1,0,4);
    v.surface.x=v.highlight>=0?v.highlight-1:0;
    v.surface.y=number("Show liquid",1,0,1)-1;
    v.surface.z=number("Show glass",1,0,1)-1;
    if(j.contains("Liquid opacity")){
        v.surface.w=1;p.liquidAlphaOverride=number("Liquid opacity",1,0,1);
    }
    v.light={number("Sun scattering",1,0,4)-1,number("Point scattering",1,0,4)-1,number("Backlighting",1,0,4)-1,number("Thickness shading",1,0,4)-1};
    v.samples=int(number("Sample cap",0,12,192));
    v.straight=int(number("Transmission mip",v.straight,0,4));v.bent=int(number("Refraction mip",v.bent,2,5));
    v.meniscus=number("Meniscus width",1,.1f,4);v.wet=number("Wet film",1,0,4);v.ripple=number("Ripples",1,0,4);
    p.color.x=number("Liquid red",p.color.x,0,4);p.color.y=number("Liquid green",p.color.y,0,4);p.color.z=number("Liquid blue",p.color.z,0,4);
    p.color.w=number("Absorption",p.color.w,0,8);p.liquidEmission=number("Liquid emission",p.liquidEmission,0,64);
    auto& s=p.settings;
    s.fill=FillHeightTuning::Adjust(s.base,p.designedFill,p.designedHeight,number("Fill offset points",0,-99,99));
    s.viscosity=number("Viscosity",s.viscosity,0,100);s.gain*=number("Slosh",1,0,4);
    s.shakingBubbles*=number("Bubbles",1,0,1);s.foamStrength*=number("Foam",1,0,1);
    nuka::Motion m{};m.lastUp={0,0,1};p.distant=nuka::Simulate(s,m,{}, {0,0,1},0,false);
}
VisualJson savedVisual=VisualJson::object(),draftVisual=VisualJson::object();
std::string selectedName,visualStatus;
bool compareOriginal=false;

const Profile& AuthoredVisual(const std::string& name){
    auto i=visualOriginal.find(name);
    return i==visualOriginal.end()?profiles.at(name):i->second;
}
void ApplySaved(const std::string& name,const VisualJson& values){
    auto& target=profiles.at(name);
    auto result=AuthoredVisual(name);
    if(!values.empty())ApplyVisual(result,values);
    visualOriginal.try_emplace(name,target);
    target=std::move(result);
}
void ReadVisualDistances(const VisualJson& values,DistanceTuning& settings){
    ApplyAcceptedSettings(settings);
    for(size_t i=0;i<settings.names.size();++i){
        auto key=settings.names[i];if(!values.contains(key))continue;
        auto value=values.at(key).get<float>();
        if(!std::isfinite(value)||value<0||value>16384)throw std::runtime_error(key);
        settings.ranges[i]={value>0,.75f*value,value};
    }
}
void LoadSavedVisual(){
    for(auto& [name,p]:profiles){
        if(name.starts_with("DBL_SSE_TMDWinery_")){
            p.visual.nativeAlpha=p.glassOpacity=.9f;
            p.visual.shellRefraction=3;
            p.visual.straight=2;
        }else if(name.starts_with("DBL_SSE_XMead_")){
            p.visual.bent=3;
            p.visual.light.x=0;
        }
    }
    try{
        auto path=VisualDirectory()/"profiles.json";
        if(!std::filesystem::exists(path))return;
        auto values=VisualJson::parse(std::ifstream(path));
        if(!values.is_object())throw std::runtime_error("expected profile list");
        if(values.contains("$distances"))ReadVisualDistances(values.at("$distances"),distanceTuning);
        for(const auto& [name,entry]:values.items())if(profiles.contains(name))ApplySaved(name,entry);
        savedVisual=std::move(values);
    }catch(const std::exception& e){SKSE::log::warn("Visual settings load: {}",e.what());}
}
void PreviewVisual(){
    auto p=AuthoredVisual(selectedName);
    if(!compareOriginal&&!draftVisual.empty())ApplyVisual(p,draftVisual);
    selectedVisual=std::move(p);
}
void SelectHeld(RE::BSGeometry* g){
    if(g==selectedGeometry.get()||!GrabbedBottle(g))return;
    selectedName=g->name.c_str();
    selectedGeometry.reset(g);
    draftVisual=savedVisual.value(selectedName,VisualJson::object());
    compareOriginal=false;visualStatus.clear();
    PreviewVisual();
}
VisualJson previewDistances=VisualJson::object();
void SaveVisual(){
    auto values=savedVisual;
    if(draftVisual.empty())values.erase(selectedName);else values[selectedName]=draftVisual;
    auto distances=values.value("$distances",VisualJson::object());
    distances.update(previewDistances);if(!distances.empty())values["$distances"]=distances;
    auto dir=VisualDirectory();std::filesystem::create_directories(dir);
    auto path=dir/"profiles.json";auto tmp=dir/"profiles.tmp";
    {std::ofstream out;out.exceptions(std::ios::failbit|std::ios::badbit);out.open(tmp);out<<values.dump(2);out.close();}
    if(!MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Could not replace saved settings");
    ApplySaved(selectedName,draftVisual);
    savedVisual=std::move(values);visualStatus="Saved for all bottles using this profile.";
}
struct VisualMenuAPI {
    struct Size {float x,y;};
    void (*text)(const char*,const char*){};
    bool (*slide)(const char*,float*,float,float,const char*,int){};
    bool (*button)(const char*,Size){};
    bool (*header)(const char*,int){};
    bool (*checkbox)(const char*,bool*){};
} menu;
void VisualPage(){
    try{
        if(menu.header("Bottle shadows and caustics",0)){
            const auto text=std::format("Shadow mask draws: {}   Native shadows: {}   Caustic draws: {}",
                bottleProjection.ShadowCount(frame-1),nativeOpaqueShadow.GetStats(frame-1).confirmed,
                bottleProjection.CausticCount(frame-1));
            menu.text(text.c_str(),nullptr);
        }
        if(!selectedGeometry||!selectedVisual){menu.text("Grab a patched bottle in the world to start tuning.",nullptr);return;}
        menu.text(selectedName.c_str(),nullptr);
        if(menu.button(compareOriginal?"Resume preview":"Compare authored",{0,0})){compareOriginal=!compareOriginal;PreviewVisual();}
        if(menu.button("Save for this bottle profile",{0,0}))SaveVisual();
        if(menu.button("Reload saved values",{0,0})){draftVisual=savedVisual.value(selectedName,VisualJson::object());compareOriginal=false;PreviewVisual();visualStatus="Loaded.";}
        if(menu.button("Reset preview to authored values",{0,0})){draftVisual=VisualJson::object();compareOriginal=false;PreviewVisual();visualStatus="Preview reset. Save to remove the profile override.";}
        menu.text(visualStatus.c_str(),nullptr);
        const auto& p=AuthoredVisual(selectedName);
        auto slide=[&](const char* key,float initial,float lo,float hi,bool integral=false){
            float v=draftVisual.value(key,initial);
            const char* label=std::string_view(key)=="Transmission mip"?"Glass transmission mip":std::string_view(key)=="Refraction mip"?"Liquid refraction mip":key;
            if(menu.slide(label,&v,lo,hi,integral?"%.0f":"%.2f",0)){
                draftVisual[key]=integral?std::round(std::clamp(v,lo,hi)):std::clamp(v,lo,hi);
                compareOriginal=false;PreviewVisual();visualStatus="Live preview";
            }
        };
        if(menu.header("Glass and liquid",32)){
            if(!p.nativeGlassPrototype||p.shieldWetParticles||p.authoredGlass)slide("Glass opacity",p.visual.nativeAlpha>=0?p.visual.nativeAlpha:(p.shieldWetParticles?.85f:p.glassOpacity),0,1);
            slide("Liquid opacity",p.liquidAlphaOverride,0,1);
            if(!p.liquidAlphaOverride&&!draftVisual.contains("Liquid opacity"))menu.text("Authored liquid opacity varies with depth.",nullptr);
            slide("Fill offset points",0,-99,99);slide("Viscosity",p.settings.viscosity,0,100);
            slide("Liquid red",p.color.x,0,4);slide("Liquid green",p.color.y,0,4);slide("Liquid blue",p.color.z,0,4);
            slide("Liquid emission",p.liquidEmission,0,64);
        }
        if(menu.header("Optics and lighting",0)){
            slide("Glass refraction strength",p.nativeGlassPrototype?p.visual.shellRefraction:p.glassRefractionScale,0,8);

            if(!p.nativeGlassPrototype||p.shieldWetParticles)slide("Glass highlights",1,0,4);
            slide("Transmission mip",p.visual.straight>=0?p.visual.straight:(p.nativeGlassPrototype?0:p.refractionMip),0,4,true);
            if(!p.LiquidOpaque())slide("Refraction mip",p.visual.bent>0?p.visual.bent:std::max(2,p.refractionMip+1),2,5,true);
            else menu.text("Liquid refraction mip: inactive at 100% opacity.",nullptr);
            slide("Sun scattering",1,0,4);slide("Point scattering",1,0,4);slide("Backlighting",1,0,4);slide("Thickness shading",1,0,4);slide("Absorption",p.color.w,0,8);
        }
        if(menu.header("Surface and motion",0)){
            slide("Meniscus width",1,.1f,4);slide("Slosh",1,0,4);slide("Ripples",1,0,4);
            slide("Bubbles",1,0,1);slide("Foam",1,0,1);slide("Wet film",1,0,4);
        }
        if(menu.header("Rendering tests",0)){
            slide("Sample cap",float(dblui::TraceSampleCap()),12,192,true);
            slide("Show glass",1,0,1,true);slide("Show liquid",1,0,1,true);
        }
        if(menu.header("Scene-wide effect distances",0)){
            menu.text("End distance in game units. Zero keeps the effect active at any distance.",nullptr);
            for(size_t i=0;i<distanceTuning.names.size();++i){
                auto& range=distanceTuning.ranges[i];float end=range.enabled?range.end:0;
                auto key=distanceTuning.names[i];
                if(menu.slide(key,&end,0,16384,"%.0f",0)){end=std::round(std::clamp(end,0.f,16384.f));range={end>0,.75f*end,end};previewDistances[key]=end;visualStatus="Global distance preview. Save to keep it.";}
            }
            if(menu.button("Reload saved distances",{0,0})){previewDistances=VisualJson::object();ReadVisualDistances(savedVisual.value("$distances",VisualJson::object()),distanceTuning);}
        }
    }catch(const std::exception& e){visualStatus=e.what();}
}
void RegisterVisualMenu(){
    auto dll=GetModuleHandleW(L"SKSEMenuFramework.dll");if(!dll)return;
    auto bind=[&](auto& fn,const char* name){fn=reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(dll,name));return fn!=nullptr;};
    bind(menu.checkbox,"igCheckbox");
    void (*section)(const char*,void(*)()){};
    if(bind(section,"AddSectionItem")&&bind(menu.text,"igTextUnformatted")&&bind(menu.slide,"igSliderFloat")&&bind(menu.button,"igButton")&&bind(menu.header,"igCollapsingHeader_TreeNodeFlags"))
        section("Dynamic Bottle Liquids SE/Visual tuning",VisualPage);
}
