#include "doctest.h"
#include "../../GameOS/gameos/light_pool_culling.h"
#include <vector>
using namespace gos_terrain_lighting;
namespace {
GpuTerrainLight light(float x,float y,float r,float g,float b,bool mover=true) {
    return {{x,y,0},1,{r,g,b},2,50,1.0f/48,mover?1.0f:0.0f,1};
}
std::array<float,3> evaluate(const std::vector<GpuTerrainLight>& lights,const float* p,
                           const std::array<bool,kObjectPoolLights>* keep,bool soft,bool maxBlend) {
    std::array<float,3> sum{},maximum{};
    for(size_t i=0;i<lights.size();++i) {
        if(keep && !(*keep)[i])continue;
        const auto& l=lights[i];float d2=0;
        for(unsigned a=0;a<3;++a) {const float d=p[a]-l.position[a];d2+=d*d;}
        const float d=std::sqrt(d2);if(d>=l.farDistance)continue;
        float f=d<=l.closeDistance?1:(l.farDistance-d)*l.oneOverDistance;
        if(soft) {f=std::clamp(f,0.0f,1.0f);f=f*f*(3-2*f);}
        for(unsigned a=0;a<3;++a) {
            const float v=l.color[a]*l.intensity*f;
            if(l._pad1>0.5f && maxBlend)maximum[a]=std::max(maximum[a],v);
            else sum[a]+=v;
        }
    }
    for(unsigned a=0;a<3;++a)sum[a]+=maximum[a];return sum;
}
}
TEST_CASE("object light bounds prune only dominated MAX movers") {
    std::vector<GpuTerrainLight> lights{light(0,0,1,0,0),light(20,0,1,0,0),
        light(20,0,0,1,0),light(30,0,1,1,1,false),light(500,500,1,1,1)};
    const float lo[3]={-1,-1,-1},hi[3]={1,1,1};
    for(bool soft:{false,true})for(bool maxBlend:{false,true}) {
        std::array<bool,kObjectPoolLights> keep;
        CullObjectPool(lights.data(),uint32_t(lights.size()),lo,hi,1,1,soft,maxBlend,keep);
        CHECK(keep[0]);CHECK(keep[1]==!maxBlend);CHECK(keep[2]);CHECK(keep[3]);CHECK_FALSE(keep[4]);
        for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z) {
            const float p[3]={float(x),float(y),float(z)};
            const auto full=evaluate(lights,p,nullptr,soft,maxBlend),culled=evaluate(lights,p,&keep,soft,maxBlend);
            for(unsigned a=0;a<3;++a)CHECK(culled[a]==doctest::Approx(full[a]).epsilon(1e-6));
        }
    }
}
TEST_CASE("object light bounds retain RGB anchors and mod falloff") {
    std::vector<GpuTerrainLight> lights{light(0,0,1,0,0),light(0,0,0,1,0),light(0,0,0,0,1),
        light(20,0,1,1,1),light(25,0,1,1,1)};
    lights.back().oneOverDistance=3; // nonstandard, potentially discontinuous ramp
    const float lo[3]={0,0,0},hi[3]={0,0,0};std::array<bool,kObjectPoolLights> keep;
    CullObjectPool(lights.data(),uint32_t(lights.size()),lo,hi,0.6f,0.25f,false,true,keep);
    CHECK(keep[0]);CHECK(keep[1]);CHECK(keep[2]);CHECK_FALSE(keep[3]);CHECK(keep[4]);
}
