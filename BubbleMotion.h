#pragma once
#include "core/ShakeMotion.h"

// Physics transforms may repeat across several render frames. Measure emission
// between distinct poses, keeping the velocity briefly between physics ticks.
// Slosh integration still runs at its original render cadence.
struct BubbleMotion {
    nuka::EmissionTracker tracker;
    float elapsed=0;
    static constexpr float horizontalTurnThreshold=45.f*.0174532925199433f;
    void Sample(nuka::Motion& m,nuka::Vec x,nuka::Vec up,nuka::Vec position,nuka::Vec vertical,
                float scale,float dt,bool held,bool valid,const void* identity=nullptr,const void* origin=nullptr){
        if(!valid||dt<=0||dt>.1f){
            tracker.Sample(m,x,up,position,vertical,scale,0,held,false,identity,origin);
            elapsed=0;return;
        }
        elapsed+=dt;
        bool changed=!tracker.valid||tracker.identity!=identity||tracker.originIdentity!=origin||
            tracker.held!=held||std::abs(tracker.scale-scale)>.00001f||
            nuka::Length(x-tracker.x)>.00001f||nuka::Length(up-tracker.up)>.00001f||
            nuka::Length(position-tracker.position)>.0001f;
        if(changed){
            // A stopped/culled object must establish fresh history on resuming.
            if(elapsed>.05f)tracker.valid=false;
            tracker.Sample(m,x,up,position,vertical,scale,elapsed,held,true,identity,origin);
            // Emission-only yaw gate: left/right turning about world vertical
            // needs 45 deg/s. Preserve tilt and full strength above the gate.
            auto axis=nuka::Normalize(up);
            float yaw=nuka::Dot(m.emissionOmega,axis);
            if(std::abs(yaw)<horizontalTurnThreshold)
                m.emissionOmega=m.emissionOmega-axis*yaw;
            elapsed=0;
        }else if(elapsed>.05f){
            m.emissionOmega={};m.emissionVerticalSpeed=m.emissionVerticalAcceleration=0;
            m.emissionTranslationValid=false;
        }
    }
};
