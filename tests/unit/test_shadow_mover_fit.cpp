#include "doctest.h"
#include "gameos/shadow_mover_fit.h"
#include "gameos/shadow_mover_feed.h"
#include <vector>

using namespace mc2shadow;

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
    float extent = 0;
    const Window window = fitMoverWindow(spheres, basis, -20, 4096, extent);
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

TEST_CASE("mover fit: sqrt two extents retain a ten percent shrink hysteresis") {
    const float extent = stabilizedMoverExtent(100,0);
    CHECK(extent >= 100);
    CHECK(extent < 100*std::sqrt(2.0f));
    CHECK(stabilizedMoverExtent(extent*0.7f, extent) == extent);
    CHECK(stabilizedMoverExtent(extent*0.6f, extent) < extent);
    CHECK(stabilizedMoverExtent(extent+0.1f, extent) > extent);
}

TEST_CASE("mover coverage: packed intervals include receivers behind every caster") {
    const std::array spheres{MoverSphere{{-100,30,80},12}, MoverSphere{{180,-40,160},24}};
    const LightBasis basis = lightBasis(clampSunAltitude({1,1,0},15));
    float extent = 0;
    const Window window = fitMoverWindow(spheres, basis, -20, 4096, extent);
    MoverShadowFrame frame{};
    windowMatrix(basis, window, frame.matrix);
    buildMoverCoverage(frame, spheres, basis, window);
    unsigned empty = 0;
    for (uint32_t word : frame.coverage) {
        if ((word & 255u) > ((word >> 8) & 255u)) ++empty;
        if (((word >> 16) & 255u) > (word >> 24)) ++empty;
    }
    CHECK(empty > 0);
    for (const auto& s : spheres) {
        const Vec3 receiver = shadowEnd(s,basis.toward,-20);
        const Vec3 pc = applyMatrix(frame.matrix,receiver);
        const int x = (std::clamp)(int((pc.x*0.5f+0.5f)*64),0,63);
        const int y = (std::clamp)(int((pc.y*0.5f+0.5f)*64),0,63);
        const unsigned cell = static_cast<unsigned>(y*64+x);
        const uint32_t interval = (frame.coverage[cell/2] >> ((cell&1u)*16u)) & 65535u;
        CHECK(float(interval&255u) <= pc.z*255);
        CHECK(float(interval>>8u) >= pc.z*255);
    }
    CHECK(sizeof(frame) <= 16384); // Vulkan's minimum maxUniformBufferRange
}

TEST_CASE("mover fit: a hundred spread movers use one count independent window") {
    std::vector<MoverSphere> spheres;
    for (int y=0;y<10;++y) for (int x=0;x<10;++x)
        spheres.push_back({{float(x*600-2700),float(y*600-2700),24},12});
    float extent=0;
    const auto basis=lightBasis(normalized({1,1,1}));
    const Window window=fitMoverWindow(spheres,basis,0,4096,extent);
    CHECK(window.texels==4096);
    CHECK(window.texelWorld<3.0f);
    MoverShadowFrame frame{};
    buildMoverCoverage(frame,spheres,basis,window);
    CHECK(frame.coverage.size()==2048);
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
