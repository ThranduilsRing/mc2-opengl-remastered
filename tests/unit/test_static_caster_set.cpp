// Caster lifecycle against an owned CPU recipe feed; no renderer or graphics context.
#include "doctest.h"
#include "gameos/static_caster_set.h"

#include <map>

namespace {
std::map<int32_t, std::vector<GpuStaticPropInstance>> recipes;

struct RecipeFeed {
    RecipeFeed() { recipes.clear(); }
    ~RecipeFeed() { recipes.clear(); }
    void add(int32_t recipe, uint32_t type) {
        GpuStaticPropInstance leaf{};
        leaf.typeID = type;
        leaf.modelMatrix[0] = leaf.modelMatrix[5] = leaf.modelMatrix[10] = leaf.modelMatrix[15] = 1.0f;
        recipes[recipe] = {leaf};
    }
};
}

namespace GpuStaticPropRegistry {
size_t getRecipeShadowLeaves(int32_t recipe, std::vector<GpuStaticPropInstance>& out) {
    const auto it = recipes.find(recipe);
    if (it == recipes.end()) return 0;
    const auto& [key, leaves] = *it;
    out.insert(out.end(), leaves.begin(), leaves.end());
    return leaves.size();
}
}

TEST_CASE("static caster set: editor admission preserves the load snapshot and assigns unique handles") {
    RecipeFeed feed;
    feed.add(1, 10);
    feed.add(2, 20);
    feed.add(3, 30);
    int loaded = 0, placed = 0, second = 0;
    mc2shadow::StaticCasterSet set;
    set.beginRebuild();
    set.add({&loaded, 41, 1});
    set.endRebuild();
    std::vector<GpuStaticPropInstance> original;
    const uint64_t key = set.loadSnapshot(original);
    set.ownerRegistered(&placed, 2);
    CHECK(set.entries().size() == 1); // runtime unknown owners stay outside the static set
    set.setAdmitNewOwners(true);
    set.ownerRegistered(&placed, 2);
    set.ownerRegistered(&second, 3);
    REQUIRE(set.entries().size() == 3);
    CHECK(set.entries()[1].gameHandle == 42);
    CHECK(set.entries()[2].gameHandle == 43);
    REQUIRE(set.hasEvents());
    const auto events = set.takeEvents();
    REQUIRE(events.size() == 2);
    CHECK(events[0].before.empty());
    REQUIRE(events[0].after.size() == 1);
    CHECK(events[0].after[0].typeID == 20);
    std::vector<GpuStaticPropInstance> snapshot;
    CHECK(set.loadSnapshot(snapshot) == key);
    REQUIRE(snapshot.size() == original.size());
    CHECK(snapshot[0].typeID == 10);
    set.logDriftSinceLoad(0); // admitted entries have no load range
}

TEST_CASE("static caster set: a removal without replacement queues its old leaves") {
    RecipeFeed feed;
    feed.add(1, 10);
    int owner = 0;
    mc2shadow::StaticCasterSet set;
    set.beginRebuild();
    set.add({&owner, 1, 1});
    set.endRebuild();
    set.ownerInvalidated(&owner, 1);
    REQUIRE(set.hasEvents());
    const auto events = set.takeEvents();
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].before.size() == 1);
    CHECK(events[0].before[0].typeID == 10);
    CHECK(events[0].after.empty());
    CHECK_FALSE(set.hasEvents());
}

TEST_CASE("static caster set: identical registration cancels a change and map reset disables admission") {
    RecipeFeed feed;
    feed.add(1, 10);
    feed.add(2, 10);
    int owner = 0, unknown = 0;
    mc2shadow::StaticCasterSet set;
    set.beginRebuild();
    set.add({&owner, 1, 1});
    set.endRebuild();
    set.setAdmitNewOwners(true);
    set.ownerInvalidated(&owner, 1);
    set.ownerRegistered(&owner, 2);
    CHECK_FALSE(set.hasEvents());
    CHECK(set.recipeMask()[1] == 0);
    CHECK(set.recipeMask()[2] == 1);
    set.beginRebuild();
    set.endRebuild();
    set.ownerRegistered(&unknown, 1);
    CHECK(set.entries().empty());
    CHECK_FALSE(set.hasEvents());
}

TEST_CASE("static caster set: pending removals follow gameplay handles") {
    RecipeFeed feed;
    feed.add(1, 30);
    feed.add(2, 10);
    int first = 0, second = 0;
    mc2shadow::StaticCasterSet set;
    set.beginRebuild();
    set.add({&first, 30, 1});
    set.add({&second, 10, 2});
    set.endRebuild();
    set.ownerInvalidated(&first, 1);
    set.ownerInvalidated(&second, 2);
    const auto events = set.takeEvents();
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].before.size() == 1);
    REQUIRE(events[1].before.size() == 1);
    CHECK(events[0].before[0].typeID == 10);
    CHECK(events[1].before[0].typeID == 30);
}
