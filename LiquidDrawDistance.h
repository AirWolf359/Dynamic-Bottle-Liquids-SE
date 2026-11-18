#pragma once
// Keep ordinary fallback/preview behavior; native world draws use the same
// configured weights and particle reduction as the deferred world renderer.
template<class GPUData,class Weights>
void ApplyLiquidDrawDistance(GPUData& gpu,const Weights& weights,bool lateDraw,bool nativeWorld){
    if(!lateDraw&&!nativeWorld){
        gpu.eye.x=gpu.eye.y=1;
        for(int k=0;k<20;++k)reinterpret_cast<float*>(gpu.effectWeights)[k]=1;
        return;
    }
    gpu.wetInfo.x*=weights[4];
    gpu.bubbleInfo.x=std::ceil(gpu.bubbleInfo.x*weights[2]);
    gpu.bubbleInfo.y=std::ceil(gpu.bubbleInfo.y*weights[3]);
    for(int k=0;k<int(gpu.bubbleInfo.x);++k)gpu.bubbles[k].w*=std::sqrt(weights[2]);
    for(int k=0;k<int(gpu.bubbleInfo.y);++k)gpu.surfaceBubbles[k].w*=std::sqrt(weights[3]);
}
