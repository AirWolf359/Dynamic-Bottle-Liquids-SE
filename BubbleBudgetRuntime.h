#pragma once
#include "Simulation.h"
// Clamp cached submission too: a slow/frozen simulation may not repack this frame.
// Keep surface cells in their own category and preserve their burst phases.
inline void LimitCachedBubbles(nuka::Constants& c,nuka::BubbleLimits limit){
    int oldShake=c.shakingCount,oldFoam=c.surfaceCount;
    int shake=std::min(oldShake,int(limit.shaking)),foam=std::min(oldFoam,int(limit.foam));
    int carb=std::min(c.carbonationCount,int(limit.carbonation));
    for(int k=0;k<foam;++k){c.bubbles[shake+k]=c.bubbles[oldShake+k];c.bubbleShape[shake+k]=c.bubbleShape[oldShake+k];}
    for(int k=0;k<carb;++k){c.bubbles[shake+foam+k]=c.bubbles[oldShake+oldFoam+k];c.bubbleShape[shake+foam+k]=c.bubbleShape[oldShake+oldFoam+k];}
    c.shakingCount=shake;c.surfaceCount=foam;c.carbonationCount=carb;c.bubbleCount=shake+foam+carb;
}
