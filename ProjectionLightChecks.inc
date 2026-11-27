// Absence is sampled from the native world scene once per game frame. Shader
// extensions can supply lights outside these arrays, so they keep the full path.
struct ProjectionShadowLight {
    const RE::BSLight* source{};
    ProjectionLightSphere influence;
};

struct ProjectionLightFrame {
    const RE::ShadowSceneNode* scene{};
    uint64_t observed=~0ull;
    ProjectionLights lights=ProjectionLights::Unknown;
    std::array<ProjectionShadowLight,32> shadowLights{};
    size_t shadowLightCount{};
    bool shadowRangesComplete{};
};
thread_local ProjectionLightFrame projectionLightFrame;

bool CanCheckNativeProjectionLights(){
    static const bool available=[](){
        for(const auto* module:{L"CommunityShaders.dll",L"BottledShader.dll",L"BottledShaders.dll",
                               L"OpenShaders.dll",L"ShadowLimitFix.dll",L"LightLimitFix.dll"}){
            if(GetModuleHandleW(module))return false;
        }
        return true;
    }();
    return available;
}

template<class Lights>
bool CollectProjectionShadowRanges(ProjectionLightFrame& sample,const Lights& lights){
    for(const auto& light:lights){
        if(!light||!light->pointLight)continue;
        const auto* source=&*light;
        const auto end=sample.shadowLights.begin()+sample.shadowLightCount;
        if(std::find_if(sample.shadowLights.begin(),end,[&](const auto& entry){return entry.source==source;})!=end)continue;
        const auto* node=source->light.get();
        if(!node||sample.shadowLightCount==sample.shadowLights.size())return false;
        const auto radius=node->GetLightRuntimeData().radius;
        const auto& world=node->world;
        const float scale=std::abs(world.scale);
        if(!std::isfinite(scale)||scale<=0.f||!std::isfinite(radius.x)||radius.x<=0.f||
           !std::isfinite(radius.y)||!std::isfinite(radius.z))return false;
        // The receiver uses radius.x. Include the largest axis and a possible
        // scale increase when proving that a bottle is outside the light.
        const auto& p=world.translate;
        const ProjectionLightSphere influence{p.x,p.y,p.z,std::max({radius.x,radius.y,radius.z})*std::max(1.f,scale)};
        if(!ValidProjectionLightSphere(influence))return false;
        sample.shadowLights[sample.shadowLightCount++]={source,influence};
    }
    return true;
}

ProjectionLights WorldProjectionLights(){
    if(!CanCheckNativeProjectionLights())return ProjectionLights::Unknown;
    static const auto& state=RE::BSShaderManager::State::GetSingleton();
    const auto* scene=state.shadowSceneNode[0];
    if(!scene||state.sceneGraph!=0)return ProjectionLights::Unknown;
    auto& sample=projectionLightFrame;
    if(sample.observed==frame&&sample.scene==scene)return sample.lights;

    const auto& runtime=scene->GetRuntimeData();
    // The pinned CommonLib calls activeShadowLights and lightQueueAdd by
    // their offsets (148 and 160). Check queued additions conservatively.
    static_assert(offsetof(RE::ShadowSceneNode::RUNTIME_DATA,unk148)==0x20);
    static_assert(offsetof(RE::ShadowSceneNode::RUNTIME_DATA,unk160)==0x38);
    RE::BSSpinLockGuard lock(runtime.unk1A8);
    const auto hasPoint=[](const auto& lights){
        return !lights.empty()&&std::any_of(lights.begin(),lights.end(),[](const auto& light){return light&&light->pointLight;});
    };
    sample.scene=scene;
    sample.observed=frame;
    sample.shadowLightCount=0;
    sample.shadowRangesComplete=false;
    if(hasPoint(runtime.unk160))sample.lights=ProjectionLights::Unknown;
    else if(hasPoint(runtime.unk148)||hasPoint(runtime.shadowCasterLights)){
        sample.lights=ProjectionLights::Shadow;
        sample.shadowRangesComplete=CollectProjectionShadowRanges(sample,runtime.unk148)&&
            CollectProjectionShadowRanges(sample,runtime.shadowCasterLights)&&sample.shadowLightCount>0;
    }
    else sample.lights=ProjectionLights::NoShadow;
    return sample.lights;
}

bool PassDeclaresProjectionShadow(const RE::BSRenderPass& pass){
    static_assert(offsetof(RE::BSRenderPass,unk20)==0x20);
    return static_cast<uint8_t>(pass.unk20)!=0;
}

bool SkipProjectionForNoShadow(const RE::BSRenderPass& pass,ProjectionLights lights,bool shadowDonor=false){
    if(lights!=ProjectionLights::NoShadow)return false;
    // The first byte is numShadowLights. A live pass takes precedence over
    // a scene snapshot, including if a light appeared during this frame.
    if(shadowDonor||PassDeclaresProjectionShadow(pass)||bottleProjection.HasReceiverLight(frame)){
        projectionLightFrame.lights=ProjectionLights::Unknown;
        return false;
    }
    return true;
}

bool CausticMayHaveLight(const RE::BSRenderPass* pass,ProjectionLights lights,const RE::NiTransform& pose,
                        const V4& body,bool shadowDonor){
    if(!pass||(pass->numLights&&!pass->sceneLights))return true;
    if(SkipProjectionForNoShadow(*pass,lights,shadowDonor))return false;
    // Live shadow inputs and incomplete scene lists take precedence.
    const auto& sample=projectionLightFrame;
    if(lights!=ProjectionLights::Shadow||PassDeclaresProjectionShadow(*pass)||shadowDonor||
       !sample.shadowRangesComplete||sample.observed!=frame||!sample.shadowLightCount||!(body.z>0.f))return true;
    // Enclose the shader's cavity and the receiver check's caster origin.
    const auto& p=pose.translate;
    const ProjectionLightSphere bottle{p.x,p.y,p.z,(std::abs(body.w)+body.x+body.z)*std::abs(pose.scale)};
    const auto end=sample.shadowLights.begin()+sample.shadowLightCount;
    return std::any_of(sample.shadowLights.begin(),end,
        [&](const auto& light){return ShadowLightMayReach(light.influence,bottle);});
}
