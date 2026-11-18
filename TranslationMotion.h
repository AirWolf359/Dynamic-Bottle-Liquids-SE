#pragma once
#include "core/Simulation.h"
#include <cmath>
#include <algorithm>
// Translation-only input adapter. Output remains in world units/second squared.
struct TranslationMotion {
    nuka::Vec position{},observed{},velocity{},target{},filtered{};
    float elapsed=0,travelTime=0,idle=0,period=1.f/60;
    bool initialized=false;
    static constexpr float movementThreshold=.5f; // Accumulated world displacement.
    static constexpr float velocityTime=.080f,accelerationTime=.080f;
    static float Gate(float value,float low,float high){float t=std::clamp((value-low)/(high-low),0.f,1.f);return t*t*(3-2*t);}
    static bool Finite(nuka::Vec v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
    void Reset(nuka::Vec p){position=observed=p;velocity=target=filtered={};elapsed=travelTime=idle=0;period=1.f/60;initialized=Finite(p);}
    nuka::Vec Sample(nuka::Vec p,float dt,bool valid=true){
        if(!initialized||!valid||!Finite(p)||!std::isfinite(dt)||dt<=0||dt>=.1f||nuka::Length(p-observed)>=100){Reset(p);return {};}
        // Infer physics updates from distinct WORLD translations. Repeated
        // render poses are not zero-velocity physics samples. Accumulate time
        // between accepted positions, with a small window for very high FPS.
        bool changed=nuka::Length(p-observed)>movementThreshold;if(changed)observed=p;
        idle=changed?0:idle+dt;elapsed+=dt;travelTime=std::min(travelTime+dt,.1f);
        bool pending=nuka::Length(p-position)>movementThreshold;
        float stopDelay=std::max(.05f,2.5f*period);
        bool stopped=idle>=stopDelay;
        if((pending&&elapsed>=1.f/60)||(stopped&&elapsed>=period)){
            float interval=pending?travelTime:elapsed;
            auto raw=pending?(p-position)*(1/interval):nuka::Vec{};
            // At rest keep the zero-velocity tail, so a real stop still creates
            // deceleration. This is emitted only after the physics-tick grace.
            float speed=std::max(nuka::Length(raw),nuka::Length(velocity));
            auto next=velocity+(raw-velocity)*(-std::expm1(-interval/velocityTime));
            target=(next-velocity)*(1/interval);
            float magnitude=nuka::Length(target);if(magnitude>6000)target=target*(6000/magnitude);
            target=target*Gate(speed,8,24); // Gentle translation, not rotation.
            velocity=next;elapsed=0;if(pending){position=p;travelTime=0;}
            if(pending)period=std::clamp(period+(interval-period)*.2f,1.f/120,.05f);
        }
        if(!pending&&!stopped&&elapsed>std::max(.025f,1.5f*period))target={};
        filtered=filtered+(target-filtered)*(-std::expm1(-dt/accelerationTime));
        float magnitude=nuka::Length(filtered);
        // Smooth radial dead zone: no wave input below 120 units/s^2; full
        // response above 360. Direction is retained, including a real stop.
        return filtered*Gate(magnitude,120,360);
    }
};
