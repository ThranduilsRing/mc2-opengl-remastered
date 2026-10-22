#include "doctest.h"
#include "../../mclib/scoped_collision_shape.h"
#include <array>

namespace {
struct CollisionFixture {
    int vertices;
    int* cleaned;
    int* destroyed;
    void ClearAnimation() { ++*cleaned; }
    ~CollisionFixture() { ++*destroyed; }
};
}

TEST_CASE("render-only queries keep stock cell budget and restore rendering") {
    int cleaned = 0, destroyed = 0;
    CollisionFixture render{23490, &cleaned, &destroyed};
    auto* instance = &render;
    long lod = 2;
    bool pbr = true;
    std::array<short, 5000> cells{};
    {
        mc2::ScopedCollisionShape collision(instance, lod, pbr,
            new CollisionFixture{98, &cleaned, &destroyed});
        REQUIRE(collision.active());
        REQUIRE(instance->vertices * 2 < static_cast<int>(cells.size()));
        for (int i = 0; i < instance->vertices * 2; ++i)
            cells[i] = 1;
        CHECK(lod == 0);
        CHECK_FALSE(pbr);
    }
    CHECK(instance == &render);
    CHECK(instance->vertices == 23490);
    CHECK(lod == 2);
    CHECK(pbr);
    CHECK(cleaned == 1);
    CHECK(destroyed == 1);
}

TEST_CASE("unoverridden terrain queries retain their instance") {
    int cleaned = 0, destroyed = 0;
    CollisionFixture stock{98, &cleaned, &destroyed};
    auto* instance = &stock;
    long lod = 1;
    bool pbr = false;
    {
        mc2::ScopedCollisionShape collision(instance, lod, pbr,
            static_cast<CollisionFixture*>(nullptr));
        CHECK_FALSE(collision.active());
        CHECK(instance == &stock);
        CHECK(lod == 1);
    }
    CHECK(cleaned == 0);
    CHECK(destroyed == 0);
}
