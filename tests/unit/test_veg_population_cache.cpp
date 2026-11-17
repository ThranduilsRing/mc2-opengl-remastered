#include "doctest.h"
#include "GameAdapters/vegetation_population_cache.h"
using namespace mc2::vegetation;
TEST_CASE("veg population identity refactor") {
    const std::vector<GosVegetation::Instance> input{
        {256,0,3,0.2f,5,1,0.3f}, {1,1,7,0.7f,6,2,0.4f},
        {2,2,-1,0.9f,8,3,0.5f}, {-1,1,2,1.2f,9,4,0.6f}};
    const auto p = buildPopulation(input, 1);
    REQUIRE(p.instances.size() == 4);
    REQUIRE(p.regions.size() == 3);
    CHECK(p.instances[1].x == 1);
    CHECK(p.instances[2].x == 2);
    CHECK(p.regions[0].first == 0);
    CHECK(p.regions[0].last == 1);
    CHECK(p.regions[1].first == 1);
    CHECK(p.regions[1].last == 3);
    CHECK(p.regions[2].last == 4);
    auto expected = packInstance(1,1,7,0.7f,6,2,0.4f);
    CHECK(std::memcmp(&p.instances[1], &expected, sizeof(expected)) == 0);
    const auto q = buildPopulation(input, 2);
    CHECK(q.instances.size() == 2);
    CHECK(q.instances[0].x == -1);
    CHECK(q.instances[1].x == 2);
}

