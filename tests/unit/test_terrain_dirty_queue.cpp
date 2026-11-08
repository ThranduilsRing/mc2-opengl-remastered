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
}
