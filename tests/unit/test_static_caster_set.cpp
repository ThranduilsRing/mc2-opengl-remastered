// Caster lifecycle against an owned CPU recipe feed; no renderer or graphics context.
#include "doctest.h"
#include "gameos/static_caster_set.h"
#include "gameos/shadow_mover_feed.h"

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

TEST_CASE("static caster set: turrets never enter the bake and live poses replace their registry fallback") {
    RecipeFeed feed;
    feed.add(1,10); feed.add(2,20); feed.add(3,30);
    int turret=0,building=0;
    mc2shadow::StaticCasterSet set;
    set.beginRebuild();
    set.add({&turret,2,1,mc2shadow::CasterKind::Turret});
    set.add({&building,1,2,mc2shadow::CasterKind::Building});
    set.endRebuild();
    REQUIRE(set.entries().size()==1);
    CHECK(set.moverOwnerCount()==1);
    CHECK(set.isMoverOwner(&turret));
    CHECK_FALSE(set.isMoverOwner(&building));
    std::vector<GpuStaticPropInstance> leaves;
    const uint64_t hash=set.loadSnapshot(leaves);
    REQUIRE(leaves.size()==1); CHECK(leaves[0].typeID==20);
    CHECK(set.recipeMask()[1]==0);
    leaves.clear(); set.appendMoverLeaves(leaves);
    REQUIRE(leaves.size()==1); CHECK(leaves[0].typeID==10);
    set.ownerInvalidated(&turret,1); set.ownerRegistered(&turret,3);
    CHECK_FALSE(set.hasEvents());
    leaves.clear(); set.appendMoverLeaves(leaves);
    REQUIRE(leaves.size()==1); CHECK(leaves[0].typeID==30);
    CHECK(set.loadSnapshot(leaves)==hash);
    GpuStaticPropInstance live{}; live.typeID=40;
    mc2shadow::moverPropFeed().record(&turret,std::span(&live,1),100);
    leaves.clear(); set.appendMoverLeaves(leaves,100);
    CHECK(leaves.empty());
    mc2shadow::moverPropFeed().append(leaves,100);
    REQUIRE(leaves.size()==1); CHECK(leaves[0].typeID==40);
    set.ownerInvalidated(&turret,3);
    CHECK_FALSE(set.hasEvents());
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

TEST_CASE("static caster set: explicit placements admit late recipes and exclude previews") {
    RecipeFeed feed;
    int placed = 0, preview = 0;
    mc2shadow::StaticCasterSet set;
    set.beginRebuild();
    set.endRebuild();
    set.admitOwner(&placed);
    REQUIRE(set.entries().size() == 1);
    CHECK(set.entries()[0].recipeIndex == -1);
    feed.add(1, 20);
    set.ownerRegistered(&preview, 1);
    CHECK_FALSE(set.hasEvents());
    set.ownerRegistered(&placed, 1);
    auto events = set.takeEvents();
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].after.size() == 1);
    CHECK(events[0].after[0].typeID == 20);
    set.ownerInvalidated(&placed, 1);
    events = set.takeEvents();
    REQUIRE(events.size() == 1);
    CHECK(events[0].after.empty());
    REQUIRE(events[0].before.size() == 1);
    CHECK(events[0].before[0].typeID == 20);
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
    const uint64_t delta=set.deltaVersion();
    set.ownerInvalidated(&owner, 1);
    set.ownerRegistered(&owner, 2);
    CHECK_FALSE(set.hasEvents());
    CHECK(set.deltaVersion()==delta); // cancelled registration must not rebuild the object map
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

TEST_CASE("static caster set: a second full bake uses live casters after events were drained") {
    RecipeFeed feed;
    feed.add(1, 10); feed.add(2, 20);
    int destroyed = 0, placed = 0;
    mc2shadow::StaticCasterSet set;
    set.beginRebuild(); set.add({&destroyed, 1, 1}); set.endRebuild();
    std::vector<GpuStaticPropInstance> leaves;
    const auto first = set.resolveForBake(0, leaves);
    REQUIRE(leaves.size() == 1); CHECK(leaves[0].typeID == 10);
    const auto version = set.version();
    set.ownerInvalidated(&destroyed, 1);
    set.setAdmitNewOwners(true); set.ownerRegistered(&placed, 2);
    set.takeEvents();
    const auto second = set.resolveForBake(version, leaves);
    REQUIRE(leaves.size() == 1); CHECK(leaves[0].typeID == 20);
    CHECK(second != first);
    CHECK(second == set.resolve(leaves));
}

TEST_CASE("static caster set: load-time damage applied before the first mask settles into the snapshot with zero drift") {
    RecipeFeed feed;
    feed.add(1, 10); feed.add(2, 20); feed.add(3, 30);
    int tree = 0, bldg = 0;
    mc2shadow::StaticCasterSet set;
    set.beginRebuild();
    set.add({&tree, 1, 1});
    set.add({&bldg, 2, 2, mc2shadow::CasterKind::Building});
    set.endRebuild();
    // the presentation pass applies the destroyed status after the build: the tree swaps shape, the building loses its shadow
    set.ownerInvalidated(&tree, 1); recipes[3][0].typeID = 31; set.ownerRegistered(&tree, 3);
    set.ownerInvalidated(&bldg, 2);
    CHECK(set.hasEvents());
    CHECK(set.logDriftSinceLoad(0) == 2);
    set.settleLoadSnapshot();
    CHECK(set.logDriftSinceLoad(0) == 0);
    CHECK_FALSE(set.hasEvents());
    std::vector<GpuStaticPropInstance> leaves;
    const uint64_t hash = set.resolveForBake(0, leaves);   // the first mask keys on the settled set
    CHECK(hash == set.resolve(leaves));
    REQUIRE(leaves.size() == 1); CHECK(leaves[0].typeID == 31);
    // a real destruction after the settle is still an event
    set.ownerInvalidated(&tree, 3);
    CHECK(set.takeEvents().size() == 1);
}

TEST_CASE("static caster set: an object destroyed between a bake attempt and its retry is replayed, not dropped") {
    RecipeFeed feed;
    feed.add(1, 10); feed.add(2, 20);
    int tree = 0, bldg = 0;
    mc2shadow::StaticCasterSet set;
    set.beginRebuild();
    set.add({&tree, 1, 1});
    set.add({&bldg, 2, 2, mc2shadow::CasterKind::Building});
    set.endRebuild();
    set.settleLoadSnapshot();   // first bake attempt (the cooked-mask read is pending, so it returns false)
    std::vector<GpuStaticPropInstance> leaves;
    const uint64_t key = set.resolveForBake(0, leaves);
    CHECK(leaves.size() == 2);
    set.ownerInvalidated(&bldg, 2);   // destroyed before the first mask completes
    set.settleLoadSnapshot();         // the retry settles again: must not forget the destruction
    CHECK(set.resolveForBake(0, leaves) == key);   // the mask keys and renders the snapshot the cache lookup used
    CHECK(leaves.size() == 2);
    CHECK(set.hasEvents());           // and the destruction reaches the dirty path once the mask lands
    CHECK(set.takeEvents().size() == 1);
}

TEST_CASE("static caster set: an owner that keeps moving becomes a mover and emits no more static events") {
    RecipeFeed feed;
    for (int r = 1; r <= 6; ++r) { feed.add(r, 10); recipes[r][0].modelMatrix[3] = 43.0f * static_cast<float>(r); }
    int walker = 0;
    mc2shadow::StaticCasterSet set;
    set.beginRebuild();
    set.add({&walker, 1, 1});
    set.endRebuild();
    int events = 0;
    for (int r = 2; r <= 6; ++r) {
        set.ownerInvalidated(&walker, r - 1);
        set.ownerRegistered(&walker, r);
        events += static_cast<int>(set.takeEvents().size());
    }
    CHECK(set.isMoverOwner(&walker));
    CHECK(events == mc2shadow::StaticCasterSet::kDemoteAfterChanges);   // the last one removes its static leaves; nothing after it is an event
    CHECK_FALSE(set.hasEvents());
    std::vector<GpuStaticPropInstance> leaves;
    set.appendMoverLeaves(leaves);
    REQUIRE(leaves.size() == 1);
}

// Investigation (w138e-review-p2 item 4): a building walking intact -> damaged -> destroyed -> rubble makes three real (non-cancelling) shape changes,
// which is exactly kDemoteAfterChanges. This pins what happens today: the rubble registration is the third change, so the building is demoted to a
// mover, its rubble leaves are dropped from the static bake (ev.after cleared) and it never casts a static rubble shadow. A tree that fell and landed
// is the same shape. Proposed fix (not applied here): count only changes that happen while the leaves keep MOVING (same typeID, matrix differs), or
// reset churn_ on a typeID change; shape swaps of a stationary owner are not the every-frame emitter the demotion exists to stop.
TEST_CASE("static caster set: a building that goes intact, damaged, destroyed, rubble is demoted on the third shape change (known issue)") {
    RecipeFeed feed;
    feed.add(1, 10); feed.add(2, 20); feed.add(3, 30); feed.add(4, 40);
    int building = 0;
    mc2shadow::StaticCasterSet set;
    set.beginRebuild();
    set.add({&building, 1, 1, mc2shadow::CasterKind::Building});
    set.endRebuild();
    for (int r = 2; r <= 3; ++r) {
        set.ownerInvalidated(&building, r - 1);
        set.ownerRegistered(&building, r);
        REQUIRE(set.takeEvents().size() == 1);
        CHECK_FALSE(set.isMoverOwner(&building));   // damaged, destroyed: still static
    }
    set.ownerInvalidated(&building, 3);
    set.ownerRegistered(&building, 4);   // rubble
    const auto events = set.takeEvents();
    REQUIRE(events.size() == 1);
    CHECK(set.isMoverOwner(&building));      // the permanent demotion
    CHECK(events[0].after.empty());          // the rubble shadow is not baked as static
    CHECK(set.recipeMask()[4] == 0);
}
