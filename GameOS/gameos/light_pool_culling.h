#pragma once
#include "gos_terrain_lighting.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <bit>
#include <map>
#include <vector>
#include <limits>
#include <span>

namespace gos_terrain_lighting {
// Exact per-object sphere culling and MAX-blend dominance. Additive lights retain
// source order. Three retained anchors establish a guaranteed RGB lower bound;
// another mover is redundant only if its upper bound cannot exceed any channel.
inline void CullObjectPool(std::span<const GpuTerrainLight> pool,
                           const float lo[3], const float hi[3], float moverScale,
                           float moverDim, bool soft, bool moverMax,
                           std::array<bool,kObjectPoolLights>& keep) {
    const auto count=pool.size();
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
// Identical-profile MAX lights can be dominated for an entire XY cell without
// approximating terrain depth. Unequal light heights use a conservative distance
// penalty over the candidate sphere. Only MAX mode without contact rings is valid.
class GridMoverDominance {
public:
    GridMoverDominance(std::span<const GpuTerrainLight> pool,bool soft,float moverScale=1.0f):groups_(pool.size(),-1) {
        std::map<std::array<uint32_t,9>,int> profiles;
        for(size_t i=0;i<pool.size();++i) {
            const auto& l=pool[i];
            if(!l.lightType || !(l._pad1>0.5f) || l.closeDistance<0 || l.farDistance<=l.closeDistance ||
               !(l.oneOverDistance>0) || !std::isfinite(l.position[2]) ||
               (!soft && (l.farDistance-l.closeDistance)*l.oneOverDistance>1.0f))continue;
            const float values[7]={l.closeDistance,l.farDistance,l.oneOverDistance,
                                   l.color[0],l.color[1],l.color[2],l.intensity};
            std::array<uint32_t,9> key{};bool valid=true;
            for(unsigned a=0;a<7;++a) {
                if(!std::isfinite(values[a]) || (a>=3 && values[a]<0)) {valid=false;break;}
                key[a]=std::bit_cast<uint32_t>(values[a]);
            }
            if(!valid)continue;
            key[7]=l.lightType;
            // Object consumers retain only the first 256 source lights. Never
            // remove one of those in favor of an anchor outside their cap.
            key[8]=i<kObjectPoolLights?0u:1u;
            auto [it,added]=profiles.try_emplace(key,static_cast<int>(profiles.size()));
            const auto& [profile,group]=*it;groups_[i]=group;
            if(added)far_.push_back(l.farDistance*(moverScale<0.999f?std::max(moverScale,0.05f):1.0f));
        }
        best_.resize(profiles.size());bestZ_.resize(profiles.size());seen_.resize(profiles.size(),0);
    }
    void appendCell(std::span<const GpuTerrainLight> pool,std::span<const uint32_t> indices,
                    float x0,float y0,float x1,float y1,std::vector<uint32_t>& out) {
        ++stamp_;
        const float margin=0.01f+std::max({std::abs(x0),std::abs(y0),std::abs(x1),std::abs(y1)})*0.000002f;
        x0-=margin;y0-=margin;x1+=margin;y1+=margin;
        for(const uint32_t i:indices) {
            const int g=groups_[i];if(g<0)continue;
            if(seen_[g]!=stamp_) {seen_[g]=stamp_;best_[g]=std::numeric_limits<float>::infinity();bestZ_[g]=0;}
            const float dx=std::max(std::abs(x0-pool[i].position[0]),std::abs(x1-pool[i].position[0]));
            const float dy=std::max(std::abs(y0-pool[i].position[1]),std::abs(y1-pool[i].position[1]));
            const float d2=dx*dx+dy*dy;
            if(d2<best_[g]) {best_[g]=d2;bestZ_[g]=pool[i].position[2];}
        }
        for(const uint32_t i:indices) {
            const int g=groups_[i];
            if(g>=0) {
                const float dx=std::max({x0-pool[i].position[0],0.0f,pool[i].position[0]-x1});
                const float dy=std::max({y0-pool[i].position[1],0.0f,pool[i].position[1]-y1});
                // Wherever this candidate contributes, |receiverZ-lightZ|<far.
                // The anchor's extra squared Z distance is at most 2*|dz|*far+dz^2.
                const float dz=std::abs(pool[i].position[2]-bestZ_[g]);
                const float bound=best_[g]+2.0f*dz*far_[g]+dz*dz;
                if(dx*dx+dy*dy>bound*1.00001f+0.0001f)continue;
            }
            out.push_back(i);
        }
    }
private:
    std::vector<int> groups_;
    std::vector<float> best_,bestZ_,far_;
    std::vector<uint32_t> seen_;
    uint32_t stamp_=0;
};
} // namespace gos_terrain_lighting
