#pragma once
#include <array>
#include <cmath>
#include <cstdint>

// Per-geometry visual orientation only. Never feeds back into fluid simulation.
struct LiquidTextureSlosh {
    bool initialized=false;std::uint64_t lastFrame=~0ull;double lastTime=0;
    std::array<float,3> normal{0,0,1};
    std::array<float,3> Sample(std::uint64_t frame,double time,bool paused,std::array<float,3> target){
        auto normalize=[](std::array<float,3> a){double length=std::sqrt(double(a[0])*a[0]+double(a[1])*a[1]+double(a[2])*a[2]);if(!(length>1e-8))return std::array<float,3>{0,0,1};for(auto& x:a)x=float(x/length);return a;};
        target=normalize(target);
        if(!initialized){normal=target;lastTime=time;initialized=true;}
        if(frame!=lastFrame){
            double dt=time-lastTime;
            if(!paused&&dt>0&&dt<.5){
                float amount=float(1-std::exp(-dt/.15));
                for(unsigned i=0;i<3;i++)normal[i]+=amount*(target[i]-normal[i]);
                normal=normalize(normal);
            }else if(!paused&&dt>=.5)normal=target; // Re-entry, not a long catch-up sweep.
            lastTime=time;lastFrame=frame;
        }
        return normal;
    }
};
