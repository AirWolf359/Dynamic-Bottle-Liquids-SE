#pragma once
#include <array>
#include <cmath>
#include <cstdint>

// One clock per rendered geometry. Neither grabbing/slosh nor shared native
// material controller writes are inputs. Immutable authored rates remain shared.
struct LiquidTextureAnimation {
    bool initialized=false;
    std::uint64_t lastFrame=~0ull;
    double lastTime=0,elapsed=0,phase=0;
    std::array<float,2> Sample(std::uint64_t frame,double time,bool paused,
                              std::uintptr_t identity,const std::array<double,2>& rate) {
        if(!initialized){
            std::uint64_t seed=std::uint64_t(identity)+0x9e3779b97f4a7c15ull;
            seed=(seed^(seed>>30))*0xbf58476d1ce4e5b9ull;
            seed=(seed^(seed>>27))*0x94d049bb133111ebull;seed^=seed>>31;
            phase=double(seed>>11)*(1.0/9007199254740992.0);
            lastTime=time;initialized=true;
        }
        if(frame!=lastFrame){
            double dt=time-lastTime;
            if(!paused&&std::isfinite(dt)&&dt>0&&dt<1)elapsed+=dt;
            lastTime=time;lastFrame=frame;
        }
        std::array<float,2> result{};
        for(unsigned axis=0;axis<2;++axis)if(rate[axis]!=0){
            double value=phase+elapsed*rate[axis];result[axis]=float(value-std::floor(value));
        }
        return result;
    }
};
