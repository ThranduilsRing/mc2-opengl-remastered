#include "doctest.h"
#include "gameos/shadow_mover_fit.h"
#include "gameos/shadow_mover_feed.h"
#include "gameos/vulkan_shadow_island_live_algo.h"
#include "gameos/local_light_shadow.h"
#include <memory>
#include <vector>

using namespace mc2shadow;

TEST_CASE("R12 instance ordering matches fresh identity sort") {
    InstanceOrder scratch;
    struct Instance { uint32_t typeID; uint32_t payload; };
    auto compare = [&](const std::vector<uint32_t>& types) {
        std::vector<Instance> input(types.size());
        std::vector<uint32_t> old(types.size());
        for (uint32_t i = 0; i < types.size(); ++i) { input[i] = {types[i], i}; old[i] = i; }
        std::sort(old.begin(), old.end(), [&](uint32_t a, uint32_t b) { return input[a].typeID < input[b].typeID; });
        CHECK(scratch.update(std::span<const Instance>(input)) == old);
        const auto* storage = scratch.update(std::span<const Instance>(input)).data();
        std::reverse(input.begin(), input.end());
        for (uint32_t i = 0; i < input.size(); ++i) input[i].typeID = types[i];
        CHECK(scratch.update(std::span<const Instance>(input)) == old);
        CHECK(scratch.update(std::span<const Instance>(input)).data() == storage);
    };
    compare({}); compare({7}); compare(std::vector<uint32_t>(64, 3));
    compare({4,1,4,2,1,4}); compare({4,1,4,2,1,4});
    compare({1,4,2,4,4,1}); compare({1,4,2,5,4,1});
    std::vector<uint32_t> growth(1025);
    for (uint32_t i = 0; i < growth.size(); ++i) growth[i] = (i * 17) % 13;
    compare(growth); compare({}); compare(growth);
    scratch.clear(); compare(growth);
}

TEST_CASE("mover feed: current-frame poses never replay history or duplicate owners") {
    MoverPropFeed feed;
    int first=0,second=0;
    GpuStaticPropInstance a{},b{}; a.typeID=1; b.typeID=2;
    feed.record(&first,std::span(&a,1),10);
    feed.record(&first,std::span(&b,1),10);
    feed.record(&second,std::span(&b,1),10);
    std::vector<GpuStaticPropInstance> leaves;
    feed.append(leaves,11); CHECK(leaves.empty());
    feed.append(leaves,10); REQUIRE(leaves.size()==2);
    CHECK(leaves[0].typeID==1); CHECK(leaves[1].typeID==2);
    feed.record(&second,std::span(&b,1),11);
    CHECK_FALSE(feed.contains(&first,11));
    leaves.clear(); feed.append(leaves,11); REQUIRE(leaves.size()==1);
    feed.pause(); feed.record(&first,std::span(&a,1),11); feed.resume();
    CHECK_FALSE(feed.contains(&first,11));
}

TEST_CASE("mover fit: an offscreen caster is admitted when its swept shadow meets the view") {
    constexpr float camera[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const Vec3 sun = normalized({1,0,1});
    CHECK(sweptShadowInView({{2,0,1},0.05f}, sun, 0, camera));
    CHECK_FALSE(sweptShadowInView({{4,0,1},0.05f}, sun, 0, camera));
    CHECK(sweptShadowInView({{0,0,2},0.05f}, {0,0,1}, 0, camera));
    CHECK_FALSE(sweptShadowInView({{0,3,0.5f},0.05f}, sun, 0, camera));
    CHECK(sweptShadowInView({{1.02f,0,0.5f},0.05f}, {0,0,1}, 0, camera));
}

TEST_CASE("mover fit: the window contains caster spheres and their ground reach") {
    const std::array spheres{MoverSphere{{-100,30,80},12}, MoverSphere{{180,-40,160},24}};
    const LightBasis basis = lightBasis(clampSunAltitude({1,1,0},15));
    const Window window = fitMoverWindow(spheres, basis, -20, 4096, {{0,0,80},300});
    float matrix[16];
    windowMatrix(basis, window, matrix);
    for (const auto& s : spheres) {
        for (Vec3 center : {s.center, shadowEnd(s, basis.toward, -20)}) {
            for (Vec3 axis : {basis.right, basis.up, basis.toward}) {
                for (float sign : {-1.0f,1.0f}) {
                    const Vec3 q = applyMatrix(matrix, {center.x+sign*s.radius*axis.x,
                        center.y+sign*s.radius*axis.y, center.z+sign*s.radius*axis.z});
                    CHECK(q.x >= -1.0001f); CHECK(q.x <= 1.0001f);
                    CHECK(q.y >= -1.0001f); CHECK(q.y <= 1.0001f);
                    CHECK(q.z >= -0.0001f); CHECK(q.z <= 1.0001f);
                }
            }
        }
    }
    CHECK((window.lx0 + window.lx1) / (2*window.texelWorld) == doctest::Approx(std::round((window.lx0+window.lx1)/(2*window.texelWorld))).epsilon(0.001));
}

TEST_CASE("mover fit: yaw visibility cannot change projection extent or texel density") {
    const auto basis = lightBasis(normalized({1,1,1}));
    const MoverSphere region{{35,-20,0},200};
    const std::array spheres{MoverSphere{{0,0,80},110}, MoverSphere{{180,-40,160},153},
                            MoverSphere{{-220,130,30},40}};
    const auto reference = fitMoverWindow(spheres,basis,-20,4096,region);
    for (size_t yaw = 0; yaw < 4; ++yaw) {
        const auto visible = std::span(spheres).subspan(yaw % 3, 1 + (yaw == 0));
        const auto window = fitMoverWindow(visible,basis,-20,4096,region);
        CHECK(window.texelWorld == reference.texelWorld);
        CHECK(window.lx0 == reference.lx0); CHECK(window.lx1 == reference.lx1);
        CHECK(window.ly0 == reference.ly0); CHECK(window.ly1 == reference.ly1);
    }
    CHECK(reference.texelWorld <= 0.125f); // retain close-up crispness
    CHECK(fitMoverWindow(spheres,basis,-20,4096,{{35,-20,0},400}).texelWorld > reference.texelWorld);
}

TEST_CASE("mover coverage: packed intervals include receivers behind every caster") {
    const std::array spheres{MoverSphere{{-100,30,80},12}, MoverSphere{{180,-40,160},24}};
    const LightBasis basis = lightBasis(clampSunAltitude({1,1,0},15));
    const Window window = fitMoverWindow(spheres, basis, -20, 4096, {{0,0,80},300});
    MoverShadowFrame frame{};
    windowMatrix(basis, window, frame.matrix);
    buildMoverCoverage(frame, spheres, basis, window);
    unsigned empty = 0;
    for (uint32_t word : frame.coverage) for (int i = 0; i < 4; ++i) if (((word >> (i*8)) & 255u) == 255u) ++empty;
    CHECK(empty > 0);
    for (const auto& s : spheres) {
        const Vec3 receiver = shadowEnd(s,basis.toward,-20);
        const Vec3 pc = applyMatrix(frame.matrix,receiver);
        const int n = kMoverCoverageSide;
        const int x = (std::clamp)(int((pc.x*0.5f+0.5f)*n),0,n-1);
        const int y = (std::clamp)(int((pc.y*0.5f+0.5f)*n),0,n-1);
        const unsigned cell = static_cast<unsigned>(y*n+x);
        const uint32_t lo = (frame.coverage[cell/4] >> ((cell&3u)*8u)) & 255u;
        CHECK(lo != 255u);
        CHECK(float(lo) <= pc.z*255);
    }
    CHECK(sizeof(frame) <= 16384); // Vulkan's minimum maxUniformBufferRange
}

TEST_CASE("mover fit: continuous zoom keeps world texel phase across yaw and pan") {
    const auto basis=lightBasis(normalized({1,1,1}));
    const std::array bounds{MoverSphere{{-5000,-5000,0},200},MoverSphere{{5000,5000,800},200}};
    const Vec3 point=toLight(basis,{123,456,0});
    float band=0, previous=0;
    for (int altitude=300;altitude<=3000;altitude+=10) {
        const float a=static_cast<float>(altitude);
        const float rows[16]{1/a,0,0,0, 0,1/a,0,0, 0,0,0,0.5f, 0,0,0,1};
        auto region=moverGroundFootprint(rows,0,5000);
        CHECK(region.radius == doctest::Approx(std::sqrt(2.0f)*a));
        const float required=region.radius;
        region.radius=moverRadiusBand(required,band);
        const auto reference=fitMoverWindow(bounds,basis,0,4096,region);
        CHECK(band==required);
        if (previous) CHECK(band / previous < 1.04f);
        previous=band;
        // A 90-degree yaw and pan use the same world lattice within this band.
        const float yawRows[16]{0,1/a,0,0.01f, -1/a,0,0,0.02f, 0,0,0,0.5f, 0,0,0,1};
        auto rotated=moverGroundFootprint(yawRows,0,5000,{0.02f*a,-0.01f*a,0});
        rotated.radius=moverRadiusBand(rotated.radius,band);
        const auto window=fitMoverWindow(bounds,basis,0,4096,rotated);
        REQUIRE(window.texelWorld==doctest::Approx(reference.texelWorld).epsilon(0.00001));
        for (int axis=0;axis<2;++axis) {
            const float p=axis?point.y:point.x;
            const float u=(p-(axis?reference.ly0:reference.lx0))/reference.texelWorld;
            const float v=(p-(axis?window.ly0:window.lx0))/window.texelWorld;
            CHECK(std::abs((u-v)-std::round(u-v))<0.002f);
        }
        CHECK(window.dMin==reference.dMin); CHECK(window.dMax==reference.dMax);
    }
    for (int radius=3000;radius>=300;radius-=10) {
        moverRadiusBand(static_cast<float>(radius),band);
        CHECK(band==static_cast<float>(radius));
    }
}

TEST_CASE("mover fit: whole map coordinates retain local caster depth precision") {
    const auto basis=lightBasis(normalized({1,1,1}));
    for (float offset : {0.0f, 16384.0f, -16384.0f}) {
        const std::array casters{MoverSphere{{offset,offset,80},24}};
        const auto window=fitMoverWindow(casters,basis,0,4096,{{offset,offset,0},400});
        CHECK(window.dMax-window.dMin < 256.0f);
        float matrix[16]; windowMatrix(basis,window,matrix);
        const auto top=applyMatrix(matrix,casters[0].center);
        const auto ground=applyMatrix(matrix,shadowEnd(casters[0],basis.toward,0));
        CHECK(top.z > 0.0f); CHECK(top.z < 1.0f);
        CHECK(ground.z > 0.0f); CHECK(ground.z < 1.0f);
    }
    float band=0;
    for (float boundary : {512.0f,1024.0f,2048.0f,4096.0f,8192.0f,16384.0f}) {
        const float before=moverRadiusBand(boundary-0.01f,band);
        CHECK(moverRadiusBand(boundary+0.01f,band)/before < 1.001f);
    }
}

TEST_CASE("mover fit: map clamps a ground footprint crossing the horizon") {
    const float rows[16]{0.01f,0,0,0, 0,0,0,0.5f, 0,0,0,0.5f, 0,0.01f,0,1};
    const auto region=moverGroundFootprint(rows,0,1000);
    CHECK(std::isfinite(region.radius));
    CHECK(region.center.y==0);
    CHECK(region.radius>500);
    CHECK(region.radius<=1500);
}

TEST_CASE("selectTiles: five percent camera move retains membership until challenged") {
    struct Light { float position[3]; uint32_t lightType; float _pad1; float farDistance; };
    std::array<Light,9> lights{};
    for (auto& tile : mc2localshadow::g_tiles) tile={};
    for (int i=0;i<9;++i) lights[i]={{i==8?-102.0f:10.0f+static_cast<float>(i)*12.0f,0,0},0x80000001u,0,512};
    mc2localshadow::selectTiles(std::span<Light>(lights),0,0,true);
    std::array<uint32_t,9> membership{};
    for (int i=0;i<9;++i) membership[i]=lights[i].lightType&0xf0u;
    mc2localshadow::selectTiles(std::span<Light>(lights),-5,0,true);
    for (int i=0;i<9;++i) CHECK((lights[i].lightType&0xf0u)==membership[i]);
    // A substantial challenger still replaces the farthest incumbent.
    for (auto& light : lights) light.lightType&=~0xf0u;
    mc2localshadow::selectTiles(std::span<Light>(lights),-40,0,true);
    CHECK((lights[8].lightType&0xf0u)!=0);
    mc2localshadow::selectTiles(std::span<Light>(lights),0,0,false);
}

TEST_CASE("mover coverage: the blocker-search reach grows a caster's footprint past its sphere") {
    const MoverSphere s{{-100,30,80},12};
    const LightBasis basis = lightBasis(clampSunAltitude({1,1,0},15));
    const Vec3 q = toLight(basis, s.center);
    const int n = kMoverCoverageSide;
    // 120 cells of 1 light-space unit. Each window puts one sphere edge 0.001 inside one side of a cell boundary, so the point 0.01 beyond that edge
    // lies in the NEXT cell and only the reach margin can cover it (a point in the same cell as the edge would pass without any reach).
    const std::array spheres{s};
    auto coverageIn = [&](float lx0) {
        Window w{};
        w.texelWorld = 0.01f; w.dMin = 0; w.dMax = 400;
        w.lx0 = lx0;         w.lx1 = w.lx0 + float(n);
        w.ly0 = q.y - 60.0f; w.ly1 = w.ly0 + float(n);
        auto frame = std::make_shared<MoverShadowFrame>();
        buildMoverCoverage(*frame, spheres, basis, w);
        return [frame, w, n](float lx, float ly) {
            const int x = int(std::floor((lx - w.lx0) * float(n) / (w.lx1 - w.lx0)));
            const int y = int(std::floor((ly - w.ly0) * float(n) / (w.ly1 - w.ly0)));
            const unsigned cell = static_cast<unsigned>(y * n + x);
            return (frame->coverage[cell / 4] >> ((cell & 3u) * 8u)) & 255u;
        };
    };
    const auto plusX = coverageIn(q.x + s.radius - (60.0f - 0.001f));   // +x edge 0.001 below the edge of cell 60
    CHECK(plusX(q.x, q.y) != 255u);                                      // under the caster
    CHECK(plusX(q.x + s.radius + 0.01f, q.y) != 255u);                   // just off its edge: only the reach margin covers it
    CHECK(plusX(q.x + s.radius + 8.0f, q.y) == 255u);                    // far beyond the reach: empty
    const auto minusX = coverageIn(q.x - s.radius - 30.001f);            // -x edge 0.001 above the edge of cell 30
    CHECK(minusX(q.x - s.radius - 0.01f, q.y) != 255u);
    CHECK(minusX(q.x - s.radius - 8.0f, q.y) == 255u);
}

TEST_CASE("mover fit: a hundred spread movers use one count independent window") {
    std::vector<MoverSphere> spheres;
    for (int y=0;y<10;++y) for (int x=0;x<10;++x)
        spheres.push_back({{float(x*600-2700),float(y*600-2700),24},12});
    const auto basis=lightBasis(normalized({1,1,1}));
    const Window window=fitMoverWindow(spheres,basis,0,4096,{{0,0,0},6000});
    CHECK(window.texels==4096);
    CHECK(window.texelWorld<3.0f);
    MoverShadowFrame frame{};
    buildMoverCoverage(frame,spheres,basis,window);
    CHECK(frame.coverage.size()==3600);
}

TEST_CASE("mover fit: a caster reaches a mover only when its swept shadow passes within their combined radii") {
    const Vec3 sun = normalized({1, 0, 1});   // shadow runs toward -x on the ground (z = 0)
    const MoverSphere prop{{10, 0, 4}, 1.0f};
    const Vec3 end = shadowEnd(prop, sun, 0);
    CHECK(end.x < prop.center.x);
    CHECK(propShadowReachesMover(prop, {{prop.center.x, 0, 4}, 0.5f}, sun, 0));                           // mover at the caster
    CHECK(propShadowReachesMover(prop, {{(prop.center.x + end.x) * 0.5f, 0, 2}, 0.5f}, sun, 0));          // inside the shadow
    CHECK_FALSE(propShadowReachesMover(prop, {{end.x - 20.0f, 0, 0}, 1.0f}, sun, 0));                     // past the shadow tip
    CHECK_FALSE(propShadowReachesMover(prop, {{10, 30, 0}, 1.0f}, sun, 0));                               // off to the side
    CHECK_FALSE(propShadowReachesMover(prop, {{20, 0, 4}, 1.0f}, sun, 0));                                // sun side of the caster
}

TEST_CASE("mech actor table: masks follow the refreshed epoch, never the cleared one") {
    struct Run { uint32_t instanceBase, instanceCount; };
    MechActorTable t; t.begin(7);
    t.actors = {{{{0, 0, 0}, 1.0f}, false}, {{{0, 0, 0}, 1.0f}, true}, {{{500, 0, 0}, 1.0f}, false}};   // off-view, in-view, off-view far
    t.instActor = {0, 1, 0, 1, -1, 2};
    const std::vector<Run> runs{{0, 1}, {1, 1}, {2, 2}, {4, 1}, {5, 1}, {9, 1}};
    const float pos[3] = {0, 0, 0};
    std::vector<uint8_t> light, view;
    CHECK(t.lightMask(7, runs, pos, 10.0f, light));   // refresh, then light, then view (production order)
    CHECK(t.viewMask(7, runs, view));
    CHECK(view == std::vector<uint8_t>{0, 1, 1, 1, 0, 1});    // off-view 0, in-view 1, shared run kept, unmapped kept, run past the map kept
    CHECK(light == std::vector<uint8_t>{1, 1, 1, 1, 0, 1});   // far actor outside the light reach
    CHECK(t.actors.size() == 3);                              // the sun pass reads the same table (prepare no longer clears it)
    constexpr float camera[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};   // sphere outside the frustum, swept shadow inside: inView true, run kept
    t.actors[0] = {{{2, 0, 1}, 0.05f}, sweptShadowInView({{2, 0, 1}, 0.05f}, normalized({1, 0, 1}), 0, camera)};
    CHECK(t.viewMask(7, runs, view)); CHECK(view[0] == 1);
    CHECK_FALSE(t.viewMask(8, runs, view));
    CHECK(view == std::vector<uint8_t>(runs.size(), 1));   // another epoch: all ones
}
