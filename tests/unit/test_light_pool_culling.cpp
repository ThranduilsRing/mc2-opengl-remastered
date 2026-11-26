#include "doctest.h"
#include "../../GameOS/gameos/light_pool_culling.h"
#include "../../mclib/spotlight_diag.h"
#include <vector>
using namespace gos_terrain_lighting;

TEST_CASE("Registered mover lights equal the world slot scan across lifecycle changes") {
    namespace tags = mc2_spotlight_diag;
    int camera = 0, preview = 0;
    int lights[5]{};
    std::array<int*, 6> slots{&lights[0], &lights[1], nullptr, &lights[2], &lights[1], &lights[3]};
    tags::reset(&camera);
    tags::reset(&preview);
    RegisteredMoverLights cached;
    size_t scans = 0;
    auto moverSlot = [&](size_t slot) {
        ++scans;
        const auto src = tags::is_e_slot(&camera, static_cast<long>(slot));
        return src == tags::Mech || src == tags::Gv;
    };
    auto check = [&] {
        cached.refresh(&camera, tags::registration_revision(), std::span<int*>(slots), moverSlot);
        for (const int& light : lights) {
            bool reference = false;
            for (size_t i = 0; i < slots.size(); ++i) {
                const auto src = tags::is_e_slot(&camera, static_cast<long>(i));
                reference |= slots[i] == &light && (src == tags::Mech || src == tags::Gv);
            }
            CHECK(cached.contains(&light) == reference);
        }
    };
    tags::tag_slot(&camera, 1, tags::Mech);
    tags::tag_slot(&camera, 3, tags::Gv);
    tags::tag_slot(&camera, 0, tags::Bldg);
    tags::tag_slot(&preview, 0, tags::Mech);
    check();
    const auto once = scans;
    check();
    CHECK(scans == once); // stable frames must not rescan slots
    tags::untag_slot(&camera, 1);
    slots[1] = &lights[4];
    check();
    tags::tag_slot(&camera, 4, tags::Gv); // duplicate pointer still classified
    check();
    tags::tag_slot(&camera, 3, tags::Bldg); // retag in place
    check();
    tags::untag_slot(&camera, 5); // untagged slot reuse must also invalidate
    slots[5] = &lights[2];
    check();
    tags::reset(&camera);
    check();
    tags::reset(&preview);
}
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
        CullObjectPool(lights,lo,hi,1,1,soft,maxBlend,keep);
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
    CullObjectPool(lights,lo,hi,0.6f,0.25f,false,true,keep);
    CHECK(keep[0]);CHECK(keep[1]);CHECK(keep[2]);CHECK_FALSE(keep[3]);CHECK(keep[4]);
}

TEST_CASE("XY grid dominance is exact at arbitrary receiver heights") {
    std::vector<GpuTerrainLight> lights{light(0,0,1,1,1),light(20,0,1,1,1),
        light(20,0,1,1,1),light(20,0,1,0,0),light(20,0,1,1,1,false),light(30,0,1,1,1)};
    lights[2].position[2]=10; // potentially wins at high receiver elevations
    lights[5].position[2]=0.001f; // small height variation still admits a conservative proof
    const uint32_t indices[]={0,1,2,3,4,5};
    for(bool soft:{false,true}) {
        GridMoverDominance dominance(lights,soft);
        std::vector<uint32_t> selected;dominance.appendCell(lights,indices,-1,-1,1,1,selected);
        REQUIRE((selected==std::vector<uint32_t>{0,2,3,4}));
        std::array<bool,kObjectPoolLights> keep{};for(auto i:selected)keep[i]=true;
        for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(float z:{-100.0f,-5.0f,0.0f,5.0f,40.0f,100.0f}) {
            const float p[3]={float(x),float(y),z};
            const auto full=evaluate(lights,p,nullptr,soft,true),culled=evaluate(lights,p,&keep,soft,true);
            for(unsigned a=0;a<3;++a)CHECK(culled[a]==doctest::Approx(full[a]).epsilon(1e-6));
        }
    }
}
TEST_CASE("XY grid dominance preserves the object consumer cap") {
    std::vector<GpuTerrainLight> lights(kObjectPoolLights+1);
    lights[0]=light(20,0,1,1,1);lights[kObjectPoolLights]=light(0,0,1,1,1);
    const uint32_t indices[]={0,kObjectPoolLights};
    GridMoverDominance dominance(lights,true);
    std::vector<uint32_t> selected;dominance.appendCell(lights,indices,-1,-1,1,1,selected);
    CHECK((selected==std::vector<uint32_t>{0,kObjectPoolLights}));
}

TEST_CASE("Thousand local lights retain contribution across small camera and receiver moves") {
    REQUIRE(kObjectPoolLights >= 1000);
    std::vector<GpuTerrainLight> lights;
    for (unsigned i=0; i<1000; ++i)
        lights.push_back(light(float(i%20)-10, float(i/20)-25, 0.001f, 0.002f, 0.003f, false));
    LightGridInputs grid;
    CHECK(grid.update(lights));
    std::array<float,3> previous{};
    for (int step=0; step<3; ++step) {
        // A camera pan changes no world lights and must not rebuild or select a subset.
        CHECK_FALSE(grid.update(lights));
        const float receiver[3]={0.01f*step, 0, 0};
        std::array<bool,kObjectPoolLights> keep;
        CullObjectPool(lights,receiver,receiver,1,1,true,true,keep);
        CHECK(std::count(keep.begin(),keep.end(),true) == 1000);
        const auto reference=evaluate(lights,receiver,nullptr,true,true);
        const auto selected=evaluate(lights,receiver,&keep,true,true);
        for (unsigned c=0;c<3;++c) {
            CHECK(selected[c] == doctest::Approx(reference[c]));
            if (step) CHECK(std::abs(selected[c]-previous[c]) < 0.001f);
        }
        previous=selected;
    }
}
