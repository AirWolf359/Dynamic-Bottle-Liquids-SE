// Replay bottle rendering at the verified pre-HDR boundary.
using HDRRenderFn=void(*)(RE::ImageSpaceEffect*,RE::BSTriShape*,RE::ImageSpaceEffectParam*);
HDRRenderFn originalHDRRender{};
// HDR entry may have no OM output yet. Resolve the declared MAIN scene rather
// than treating an unbound output as a different render target.
ID3D11Resource* verifiedHDRMain(RE::ImageSpaceEffect* effect){
    if(!effect)return nullptr;
    auto* data=RE::BSGraphics::Renderer::GetRendererData();
    if(!data)return nullptr;
    auto* main=data->renderTargets[RE::RENDER_TARGET::kMAIN].texture;
    if(!main)return nullptr;
    for(auto* texture:effect->effectTextures)
        if(texture&&texture->renderTarget==RE::RENDER_TARGET::kMAIN)return main;
    return nullptr;
}
void hdrRender(RE::ImageSpaceEffect* effect,RE::BSTriShape* shape,RE::ImageSpaceEffectParam* params){
    {dblui::CPUCost compositionCPU(dblui::CPUStage::Compose);
    try{
        auto* mainScene=verifiedHDRMain(effect);
        if(opticalBuffers.observeHDRBoundary(frame,mainScene!=nullptr))SKSE::log::info("[DBL-HDR-recovered] Verified MAIN boundary returned; shared rendering resumes next frame");
        if(opticalBuffers.prepared==frame&&opticalBuffers.draws&&opticalBuffers.composed!=frame){
            // Replay reports copies/draws itself; do not count them twice.
            if(opticalBuffers.compose(frame,mainScene)){
            static unsigned reported=0;static double nextReport=0;double time=now();
            if(reported++<3||(dblui::Enabled()&&time>=nextReport)){
                nextReport=time+5;
                SKSE::log::info("[DBL-optical-path] mode={} frame={} bottle_draws={} shared_scene_snapshots={} before_HDR=true capture={} native_buffer_transfers={} borrowed_depth={}",
                    forwardDraws.enabled?"forward":"optical-buffers",frame,opticalBuffers.draws,forwardDraws.enabled?1:0,
                    forwardDraws.lastPacked?"packed":"legacy",forwardDraws.lastTransfers,forwardDraws.snapshot.usingNativeDepth);
            }
            }
        }
    }catch(const std::exception& e){
        if(forwardDraws.enabled){forwardDraws.enabled=false;opticalBuffers.reset();SKSE::log::error("Forward resolve failed; restoring Test68 optical buffers next frame: {}",e.what());}
        else {opticalBuffers.failed=true;SKSE::log::error("Late optical composition failed; immediate fallback from next frame: {}",e.what());}
    }
    }
    originalHDRRender(effect,shape,params);
}
void installHDRProbe(){
    REL::Relocation<std::uintptr_t> table{RE::VTABLE_ImageSpaceEffectHDR[0]};
    originalHDRRender=reinterpret_cast<HDRRenderFn>(table.write_vfunc(1,&hdrRender));
    SKSE::log::info("ImageSpaceEffectHDR::Render boundary hook installed");
}

