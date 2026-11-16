#include "doctest.h"
#include "../../code/tick_commit.h"
#include "../../mclib/sim_overlap.h"
#include <vector>

namespace {
std::vector<int>* commits = nullptr;
struct Object {
    bool& dead;
    ~Object() { dead=true; if (commits) commits->push_back(6); }
};
struct Effect {
    bool& killed;
    bool& dead;
    void Kill() { killed=true; }
    ~Effect() { dead=true; }
};
}
// Device-free texture seam: the engine implementation creates/collects here.
namespace mc2::textures {
void commitTickTextures() { if (commits) commits->push_back(0); }
}
TEST_CASE("tick commit keeps resources alive until ordered main-thread commit") {
    using namespace mc2::tick;
    std::vector<int> order;
    commits=&order;
    bool dead=false;
    auto* object=new Object{dead};
    mc2::overlap::runOnWorker([&] {
        Scope scope;
        setCollecting(true);
        retireObject(object);
        defer(Queue::Sound,[&] { CHECK_FALSE(dead); order.push_back(5); });
        defer(Queue::Impact,[&] { CHECK_FALSE(dead); order.push_back(4); });
        defer(Queue::Release,[&] { CHECK_FALSE(dead); order.push_back(3); });
        defer(Queue::MoverUi,[&] { CHECK_FALSE(dead); order.push_back(1); });
        defer(Queue::MoverUi,[&] { CHECK_FALSE(dead); order.push_back(2); });
    });
    mc2::overlap::waitWorker();
    CHECK_FALSE(dead);
    CHECK(order.empty());
    commitTickQueues();
    CHECK(dead);
    CHECK(order==std::vector<int>{0,1,2,3,4,5,6});
    order.clear();
    commitTickQueues();
    CHECK(order==std::vector<int>{0});
    CHECK_FALSE(collecting());
    CHECK_FALSE(defer(Queue::Sound,[]{ FAIL("frame commands must execute inline"); }));
    commits=nullptr;
}

TEST_CASE("typed effect release kills and destroys at commit or inline") {
    bool killed=false, dead=false;
    auto effect=std::make_unique<Effect>(killed,dead);
    const auto* snapshot=effect.get();
    mc2::overlap::runOnWorker([&] {
        mc2::tick::Scope scope;
        mc2::tick::setCollecting(true);
        mc2::tick::releaseEffect(effect);
    });
    mc2::overlap::waitWorker();
    CHECK_FALSE(effect); CHECK_FALSE(killed); CHECK_FALSE(dead);
    CHECK_FALSE(snapshot->killed); CHECK_FALSE(snapshot->dead);
    mc2::tick::commitTickQueues();
    CHECK(killed); CHECK(dead);
    killed=dead=false;
    effect=std::make_unique<Effect>(killed,dead);
    mc2::tick::releaseEffect(effect);
    CHECK_FALSE(effect); CHECK(killed); CHECK(dead);
}

TEST_CASE("packet retirement holds tick frees after commit and preserves release order") {
    using namespace mc2::tick;
    const auto packet = retainRenderPacket();
    bool killed = false, effectDead = false, objectDead = false;
    auto effect = std::make_unique<Effect>(killed, effectDead);
    auto* object = new Object{objectDead};
    std::vector<int> order;
    mc2::overlap::runOnWorker([&] {
        Scope scope;
        setCollecting(true);
        releaseEffect(effect);
        retireObject(object);
        defer(Queue::Sound, [&] { CHECK_FALSE(effectDead); CHECK_FALSE(objectDead); order.push_back(1); });
        deferRetirement(Queue::Release, [&] { CHECK(effectDead); CHECK_FALSE(objectDead); order.push_back(2); });
        deferRetirement(Queue::Graveyard, [&] { CHECK(objectDead); order.push_back(3); });
        defer(Queue::Release, [&] { CHECK_FALSE(effectDead); CHECK_FALSE(objectDead); order.push_back(0); });
    });
    mc2::overlap::waitWorker();
    commitTickQueues();
    CHECK_FALSE(killed); CHECK_FALSE(effectDead); CHECK_FALSE(objectDead);
    CHECK(order == std::vector<int>{0, 1});
    retireRenderPacket(packet);
    CHECK(killed); CHECK(effectDead); CHECK(objectDead);
    CHECK(order == std::vector<int>{0, 1, 2, 3});
}

TEST_CASE("packet retirement waits for older readers but not newer packets") {
    using namespace mc2::tick;
    const auto first = retainRenderPacket();
    bool firstDead = false, sharedDead = false;
    retireObject(new Object{firstDead});
    const auto second = retainRenderPacket();
    retireObject(new Object{sharedDead});
    const auto third = retainRenderPacket();
    retireRenderPacket(second);
    CHECK_FALSE(firstDead); CHECK_FALSE(sharedDead);
    retireRenderPacket(first);
    CHECK(firstDead); CHECK(sharedDead);
    // All three CPU slots can be reused after out-of-order retirement.
    const auto fourth = retainRenderPacket();
    const auto fifth = retainRenderPacket();
    bool lateDead = false;
    retireObject(new Object{lateDead});
    retireRenderPacket(fifth);
    retireRenderPacket(fourth);
    CHECK_FALSE(lateDead);
    retireRenderPacket(third);
    CHECK(lateDead);
}

TEST_CASE("packet retirement removes ready releases before reentrant destructors") {
    using namespace mc2::tick;
    const auto first = retainRenderPacket();
    bool earlyDead = false, lateDead = false;
    uint64_t next = 0;
    REQUIRE(deferRetirement(Queue::Release, [&] {
        earlyDead = true;
        next = retainRenderPacket();
        retireObject(new Object{lateDead});
    }));
    retireRenderPacket(first);
    CHECK(earlyDead); CHECK_FALSE(lateDead);
    REQUIRE(next != 0);
    retireRenderPacket(next);
    CHECK(lateDead);
}

TEST_CASE("packet retirement retains a light until its world removal and free") {
    struct WorldLights {
        bool removed = false;
        void removeWorldLight(int id, int* light) {
            CHECK(id == 9);
            CHECK(*light == 42);
            removed = true;
        }
    } world;
    auto* light = static_cast<int*>(std::malloc(sizeof(int)));
    REQUIRE(light != nullptr);
    *light = 42;
    const auto packet = mc2::tick::retainRenderPacket();
    mc2::tick::releaseLight(light, &world, 9);
    CHECK(light == nullptr);
    CHECK_FALSE(world.removed);
    mc2::tick::releaseLight(light, &world, 9);
    mc2::tick::retireRenderPacket(packet);
    CHECK(world.removed);
    light = static_cast<int*>(std::malloc(sizeof(int)));
    REQUIRE(light != nullptr);
    mc2::tick::releaseLight(light, static_cast<WorldLights*>(nullptr), 9);
    CHECK(light == nullptr);
}
