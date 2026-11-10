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
