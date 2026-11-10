#include "doctest.h"
#include "terrain_dirty_queue.h"

TEST_CASE("terrain_dirty_queue/static scene drains to zero; one mark = one update")
{
    mc2::TerrainDirtyQueue queue;
    queue.reset(10);
    queue.markBlock(1, 9); // spawn
    CHECK(queue.drain().size() == 9);
    CHECK(queue.drain().empty());
    queue.mark(4);
    queue.mark(4); // damage and power in the same presentation
    auto work = queue.drain();
    REQUIRE(work.size() == 1);
    CHECK(work.front() == 4);
    CHECK(queue.drain().empty());
    queue.mark(4); // continuing animation/falling schedules the next presentation
    CHECK(queue.drain() == work);
    CHECK(queue.drain().empty());
}

TEST_CASE("terrain_dirty_queue/height epoch marks every object of the block")
{
    mc2::TerrainDirtyQueue queue;
    queue.reset(12);
    queue.mark(8);
    queue.markBlock(3, 4);
    queue.markBlock(3, 4);
    const std::vector<long> expected{3, 4, 5, 6, 8};
    CHECK(queue.drain() == expected);
    CHECK(queue.drain().empty());
    queue.mark(-1);
    queue.mark(12);
    CHECK(queue.drain().empty());
    queue.mark(4);
    queue.reset(3); // mission teardown/reload must discard stale IDs
    CHECK(queue.drain().empty());
    queue.markAll(); // power generator destruction invalidates linked blocks
    CHECK(queue.drain() == std::vector<long>{1, 2});
    CHECK(queue.drain().empty());
}

TEST_CASE("terrain_dirty_queue/camera move skips settled residents")
{
    mc2::TerrainDirtyQueue queue;
    queue.reset(8);
    std::vector<long> bound;
    auto bind = [&bound](long handle) { bound.push_back(handle); };
    queue.markBlockNonResident(1, 7, bind); // first camera frame: nothing resident yet
    CHECK(queue.drain().size() == 7);
    for (long h = 1; h < 8; ++h) if (h != 5) queue.setResident(h); // 5 stays camera-dependent
    bound.clear();
    queue.markBlockNonResident(1, 7, bind); // camera pan
    CHECK(bound == std::vector<long>{5});
    CHECK(queue.drain() == std::vector<long>{5});
    queue.markBlockNonResident(1, 7, bind);
    queue.markBlockNonResident(1, 7, bind); // same frame twice: one visit
    CHECK(queue.drain() == std::vector<long>{5});
}

TEST_CASE("terrain_dirty_queue/a mark drops residency for that handle only")
{
    mc2::TerrainDirtyQueue queue;
    queue.reset(6);
    for (long h = 1; h < 6; ++h) queue.setResident(h);
    queue.mark(3); // damage/destroy/selection
    CHECK_FALSE(queue.isResident(3));
    CHECK(queue.isResident(2));
    queue.setResident(3); // ignored while 3 is still queued
    CHECK_FALSE(queue.isResident(3));
    CHECK(queue.drain() == std::vector<long>{3});
    auto noBind = [](long) {};
    queue.markBlockNonResident(1, 5, noBind);
    CHECK(queue.drain() == std::vector<long>{3}); // revisited until it settles again
    queue.setResident(3);
    queue.markBlockNonResident(1, 5, noBind);
    CHECK(queue.drain().empty());
}

TEST_CASE("terrain_dirty_queue/height epoch and night revisit residents")
{
    mc2::TerrainDirtyQueue queue;
    queue.reset(6);
    for (long h = 1; h < 6; ++h) queue.setResident(h);
    queue.markBlock(1, 5); // height epoch / night change
    CHECK(queue.drain() == std::vector<long>{1, 2, 3, 4, 5});
    for (long h = 1; h < 6; ++h) CHECK_FALSE(queue.isResident(h));
    for (long h = 1; h < 6; ++h) queue.setResident(h);
    queue.markAll();
    CHECK(queue.drain().size() == 5);
    CHECK_FALSE(queue.isResident(1));
    queue.setResident(2);
    queue.reset(6); // mission reload forgets residency
    CHECK_FALSE(queue.isResident(2));
}

TEST_CASE("terrain_dirty_queue/night drift does not invalidate residents")
{
    CHECK(mc2::terrainNightFullInvalidate(-1.0f, 0.0f, false, false));      // first frame
    CHECK_FALSE(mc2::terrainNightFullInvalidate(0.31f, 0.32f, false, false)); // dusk drift
    CHECK_FALSE(mc2::terrainNightFullInvalidate(0.0f, 0.0f, false, false));   // full day
    CHECK(mc2::terrainNightFullInvalidate(0.0f, 0.01f, false, false));        // lamps may spawn
    CHECK(mc2::terrainNightFullInvalidate(0.4f, 0.0f, false, false));         // lamps may drop
    CHECK(mc2::terrainNightFullInvalidate(0.5f, 0.5f, false, true));          // isNight flip
}

#include "static_resident_set.h"

// static-world-s2: the residency state machine. A resident's object is skipped by the render extract only while
// the queue says render-resident and the set still holds it.
TEST_CASE("static_resident/mark, epoch, light generation, destroy and cull each end residency")
{
    mc2::TerrainDirtyQueue queue;
    mc2::StaticResidentSet set;
    queue.reset(8);
    set.reset(8);
    auto settle = [&](long h, int32_t recipe) {   // presentation settles, then the render visit settles
        queue.setResident(h);
        if (queue.setRenderResident(h)) set.add(h, recipe);
    };
    std::vector<int32_t> cleared;
    bool exists[8] = {true, true, true, true, true, true, true, true};
    bool culled[8] = {};
    auto run = [&](uint64_t key) {
        set.validate(key,
            [&](long h, int32_t) { return exists[h] && !culled[h] && queue.isRenderResidentQuiescent(h); },
            [&](long, int32_t r) { cleared.push_back(r); });
    };

    settle(1, 11); settle(2, 12); settle(3, 13); settle(4, 14); settle(5, 15);
    CHECK(set.size() == 5);
    run(7);   // first pass fixes the epoch key (nothing was recorded before: all dropped)
    CHECK(set.size() == 0);
    cleared.clear();
    settle(1, 11); settle(2, 12); settle(3, 13); settle(4, 14); settle(5, 15);
    run(7);
    CHECK(set.size() == 5);
    CHECK(cleared.empty());

    queue.mark(1);                       // damage / selection / status
    exists[2] = false;                   // destroy
    culled[3] = true;                    // cull-state change
    run(7);
    CHECK(set.size() == 2);
    CHECK(!set.contains(1)); CHECK(!set.contains(2)); CHECK(!set.contains(3));
    CHECK(set.recipe(4) == 14);

    queue.drain();                       // object 1 revisited; it may settle again only after its presentation visit
    CHECK(!queue.setRenderResident(1));  // presentation has not re-settled it yet
    queue.setResident(1);
    CHECK(queue.setRenderResident(1));
    set.add(1, 11);
    CHECK(set.size() == 3);

    run(8);                              // height epoch / light generation change drops every resident
    CHECK(set.size() == 0);
    CHECK(cleared.size() >= 5);

    // a mark during the same frame as the render visit wins
    queue.setResident(6);
    queue.mark(6);
    CHECK(!queue.setRenderResident(6));
    // markAll (power change) clears render residency too
    queue.drain();
    queue.setResident(7);
    CHECK(queue.setRenderResident(7));
    queue.markAll();
    CHECK(!queue.isRenderResidentQuiescent(7));
}

#include "terrain_walk_skip.h"

// perf-terrain-walk: the render walk drops only views that would draw nothing this frame. The decision is taken per
// frame from that frame's presentation state, so an object entering the view is drawn the same frame (no vanishing
// props) and a falling tree off view still captures its moving shadow.
TEST_CASE("terrain_walk_skip/only off-view trees and terrain objects with nothing to draw leave the walk")
{
    // A settled tree outside the view: render() is a no-op, the walk skips it.
    CHECK(mc2::treeRenderIsNoOp(false, false));
    CHECK(mc2::terrainWalkSkips(TREE, false, false, mc2::treeRenderIsNoOp(false, false)));
    // The camera turns: the presentation pass sets inView, the same frame's walk draws it.
    CHECK(!mc2::treeRenderIsNoOp(true, false));
    CHECK(!mc2::terrainWalkSkips(TREE, false, false, mc2::treeRenderIsNoOp(true, false)));
    // A tree that starts to fall off view keeps its shadow capture visit.
    CHECK(!mc2::treeRenderIsNoOp(false, true));
    CHECK(!mc2::terrainWalkSkips(TREE, false, false, mc2::treeRenderIsNoOp(false, true)));
    // Bars (selected) or a dust effect are drawn by renderView itself: always visited.
    CHECK(!mc2::terrainWalkSkips(TREE, true, false, true));
    CHECK(!mc2::terrainWalkSkips(TERRAINOBJECT, false, true, true));
    CHECK(mc2::terrainWalkSkips(TERRAINOBJECT, false, false, true));
    // An appearance that does not claim a no-op render (the default, e.g. buildings) is always visited.
    CHECK(!mc2::terrainWalkSkips(TERRAINOBJECT, false, false, false));
    // Other classes have their own renderView: never skipped here.
    CHECK(!mc2::terrainWalkSkips(BUILDING, false, false, true));
    CHECK(!mc2::terrainWalkSkips(TURRET, false, false, true));
    CHECK(!mc2::terrainWalkSkips(GATE, false, false, true));
    CHECK(!mc2::terrainWalkSkips(BRIDGE, false, false, true));
}

TEST_CASE("terrain_dirty_queue/a quiescent read during a simulated sim phase is caught, counted, and still correct")
{
    mc2::TerrainDirtyQueue queue;
    queue.reset(4);
    queue.setResident(2);
    REQUIRE(queue.setRenderResident(2));

    queue.beginQuiescentRead(false);                 // sim joined: lock-free read, no violation
    CHECK(queue.isRenderResidentQuiescent(2));
    CHECK(queue.quiescentViolations() == 0);

    queue.beginQuiescentRead(true);                  // sim job in flight at extract entry
    CHECK(queue.quiescentViolations() == 1);
    CHECK(queue.isRenderResidentQuiescent(2));       // falls back to the locked read: same answer
    queue.mark(2);
    CHECK_FALSE(queue.isRenderResidentQuiescent(2));
    queue.beginQuiescentRead(true);
    CHECK(queue.quiescentViolations() == 2);

    queue.beginQuiescentRead(false);                 // next clean extract returns to the lock-free path
    queue.drain();
    queue.setResident(2);
    queue.setRenderResident(2);
    CHECK(queue.isRenderResidentQuiescent(2));
    CHECK(queue.quiescentViolations() == 2);
}
