#pragma once
#include "gos_terrain_lighting.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace gos_terrain_lighting {
// Exact per-object sphere culling and MAX-blend dominance. Additive lights retain
// source order. Three retained anchors establish a guaranteed RGB lower bound;
// another mover is redundant only if its upper bound cannot exceed any channel.
inline void CullObjectPool(const GpuTerrainLight* pool, uint32_t count,
                           const float lo[3], const float hi[3], float moverScale,
                           float moverDim, bool soft, bool moverMax,
                           std::array<bool,kObjectPoolLights>& keep) {
    keep.fill(false);
    std::array<std::array<float,3>,kObjectPoolLights> upper{};
    std::array<bool,kObjectPoolLights> eligible{};
    std::array<float,3> lower{};
    std::array<uint32_t,3> anchor{kObjectPoolLights,kObjectPoolLights,kObjectPoolLights};
    for(uint32_t i=0;i<count;++i) {
        const auto& l=pool[i];if(!l.lightType)continue;
        const bool mover=l._pad1>0.5f;
        float farD=l.farDistance,nearD=l.closeDistance,oo=l.oneOverDistance;
        if(mover && moverScale<0.999f) {
            const float scale=std::max(moverScale,0.05f);
            farD*=scale;nearD*=scale;oo/=scale;
        }
        float min2=0,max2=0;
        for(unsigned a=0;a<3;++a) {
            const float d=std::max({lo[a]-l.position[a],0.0f,l.position[a]-hi[a]});
            const float f=std::max(std::abs(lo[a]-l.position[a]),std::abs(hi[a]-l.position[a]));
            min2+=d*d;max2+=f*f;
        }
        if(farD<=0 || min2>farD*farD*1.00001f)continue;
        keep[i]=true;
        // Unusual mod falloff/color records stay on the conservative full path.
        if(!moverMax || !mover || !std::isfinite(max2) || !std::isfinite(farD) ||
           nearD<0 || nearD>=farD || oo<=0 || !std::isfinite(oo) ||
           (!soft && std::abs((farD-nearD)*oo-1.0f)>0.00001f) ||
           !std::isfinite(l.intensity) || l.intensity<0 || moverDim<0)continue;
        const auto falloff=[&](float d) {
            if(d>=farD)return 0.0f;
            if(d<=nearD)return 1.0f;
            float f=(farD-d)*oo;
            if(soft) {f=std::clamp(f,0.0f,1.0f);f=f*f*(3.0f-2.0f*f);}
            return f;
        };
        // Widen distance bounds for CPU/GPU rounding; never prune a close tie.
        const float maxF=falloff(std::max(0.0f,std::sqrt(min2)*0.99999f-0.00001f));
        const float minF=falloff(std::sqrt(max2)*1.00001f+0.00001f);
        std::array<float,3> low{};
        bool valid=true;
        for(unsigned a=0;a<3;++a) {
            const float color=(l.color[a]*l.intensity)*moverDim;
            if(!std::isfinite(color) || color<0) {valid=false;break;}
            upper[i][a]=(color*maxF)*1.00001f;
            low[a]=(color*minF)*0.99999f;
        }
        if(!valid)continue;
        eligible[i]=true;
        for(unsigned a=0;a<3;++a)if(low[a]>lower[a]) {lower[a]=low[a];anchor[a]=i;}
    }
    for(uint32_t i=0;i<count;++i) {
        if(!eligible[i] || i==anchor[0] || i==anchor[1] || i==anchor[2])continue;
        if(upper[i][0]<=lower[0] && upper[i][1]<=lower[1] && upper[i][2]<=lower[2])keep[i]=false;
    }
}
} // namespace gos_terrain_lighting
