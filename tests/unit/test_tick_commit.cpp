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
