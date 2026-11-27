// Run on the game thread through the SKSE task queue.
void GuardDevelopmentCamera(){
    auto* camera=RE::PlayerCamera::GetSingleton();
    if(!camera)return;
    const auto state=camera->GetCurrentState();
    static std::optional<RE::CameraState> lastPlayerView;
    static bool reportedTimer=false;
    static bool reportedOrbit=false;
    if(state==RE::CameraState::kAutoVanity){
        if(!reportedOrbit)SKSE::log::warn("[DBL-development-camera] Entered automatic idle orbit");
        reportedOrbit=true;
        if(lastPlayerView==RE::CameraState::kFirstPerson)camera->ForceFirstPerson();
        else if(lastPlayerView==RE::CameraState::kThirdPerson)camera->ForceThirdPerson();
        return;
    }
    reportedOrbit=false;
    if(state!=RE::CameraState::kFirstPerson&&state!=RE::CameraState::kThirdPerson){
        lastPlayerView.reset();
        return;
    }
    lastPlayerView=state;
    camera->GetRuntimeData().idleTimer=0.f;
    if(!reportedTimer){
        SKSE::log::info("[DBL-development-camera] Game-thread idle timer guard active");
        reportedTimer=true;
    }
}

void ScheduleDevelopmentCameraGuard(uint64_t frame){
    if(frame%120!=0)return;
    static std::atomic_bool pending=false;
    auto* tasks=SKSE::GetTaskInterface();
    if(!tasks||pending.exchange(true))return;
    tasks->AddTask([]{
        GuardDevelopmentCamera();
        pending.store(false);
    });
}
