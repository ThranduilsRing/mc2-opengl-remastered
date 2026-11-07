#include "doctest.h"
#include "../../code/world/actor_pool.h"

namespace {
struct Actor {
    static inline int live = 0;
    bool* dead = nullptr;
    Actor() { ++live; }
    ~Actor() { --live; if (dead) *dead = true; }
};
}

TEST_CASE("ActorPool/spawn fills slot order, retire frees, reuse bumps generation") {
    mc2::world::ActorPool<Actor> pool;
    Actor** view = pool.reset(3);
    const auto a = pool.spawn(), b = pool.spawn(), c = pool.spawn();
    CHECK(a.index == 0); CHECK(b.index == 1); CHECK(c.index == 2);
    CHECK(view[1] == pool.resolve(b));
    CHECK_FALSE(pool.spawn()); // full
    CHECK(Actor::live == 3);

    bool dead = false;
    pool.resolve(b)->dead = &dead;
    CHECK(pool.retire(b));
    CHECK(dead); // no render packet in flight: freed at once
    CHECK(view[1] == nullptr);
    CHECK_FALSE(pool.retire(b)); // double retire rejected

    const auto reused = pool.spawn();
    CHECK(reused.index == 1);
    CHECK(reused.generation == b.generation + 1);
    CHECK(pool.resolve(reused) == view[1]);
    CHECK(pool.resolve(b) == nullptr);
    pool.clear();
    CHECK(Actor::live == 0);
}

TEST_CASE("ActorPool/stale IDs are rejected across retire, reset and forgery") {
    mc2::world::ActorPool<Actor> pool;
    pool.reset(2);
    const auto a = pool.spawn();
    REQUIRE(pool.resolve(a));
    CHECK(pool.id(a.index) == a);
    CHECK_FALSE(pool.resolve({}));
    CHECK_FALSE(pool.resolve({a.session, a.index, a.generation + 1}));
    CHECK_FALSE(pool.resolve({a.session, 7, a.generation}));
    CHECK_FALSE(pool.resolve({a.session + 1000, a.index, a.generation}));
    CHECK_FALSE(pool.id(1)); // empty index has no ID

    pool.reset(2); // new session: same index and generation is still stale
    const auto again = pool.spawn();
    CHECK(again.index == a.index);
    CHECK(again.generation == a.generation);
    CHECK(pool.resolve(a) == nullptr);
    CHECK(pool.resolve(again) != nullptr);
    pool.clear();
    CHECK(pool.resolve(again) == nullptr);
    CHECK(Actor::live == 0);
}

TEST_CASE("ActorPool/retired actors outlive the render packets admitted before them") {
    using namespace mc2::tick;
    mc2::world::ActorPool<Actor> pool;
    pool.reset(2);
    bool firstDead = false, secondDead = false, clearedDead = false;
    const auto first = pool.spawn();
    pool.resolve(first)->dead = &firstDead;

    const auto packetA = retainRenderPacket();
    CHECK(pool.retire(first));
    CHECK_FALSE(firstDead); // packet A may still draw it
    CHECK(pool.resolve(first) == nullptr); // but the ID is already stale
    const auto reused = pool.spawn(); // and the index is reusable at once
    CHECK(reused.index == first.index);
    pool.resolve(reused)->dead = &secondDead;

    const auto packetB = retainRenderPacket();
    const auto other = pool.spawn();
    pool.resolve(other)->dead = &clearedDead;
    CHECK(pool.retire(reused));
    pool.clear(); // session teardown defers the same way

    retireRenderPacket(packetB);
    CHECK_FALSE(firstDead); CHECK_FALSE(secondDead); CHECK_FALSE(clearedDead); // A is older and still live
    retireRenderPacket(packetA);
    CHECK(firstDead); CHECK(secondDead); CHECK(clearedDead);
    CHECK(Actor::live == 0);
}
