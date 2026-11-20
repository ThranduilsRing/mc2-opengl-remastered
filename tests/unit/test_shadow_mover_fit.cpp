#include "doctest.h"
#include "gameos/shadow_mover_fit.h"
#include "gameos/shadow_mover_feed.h"
#include "gameos/vulkan_shadow_island_live_algo.h"
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
    float extent=0;
    const auto basis=lightBasis(normalized({1,1,1}));
    const Window window=fitMoverWindow(spheres,basis,0,4096,extent);
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
