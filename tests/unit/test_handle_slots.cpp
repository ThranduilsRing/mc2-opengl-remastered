// tests/unit/test_handle_slots.cpp
// P3 handle slots (RenderCore/HandleSlots.h): a retired slot is reused only after the GPU frame that may name it completed, stale handles stay refused,
// generations wrap past 0. The "GPU" is a completed-frame number advanced by hand.
#include "doctest.h"
#include "HandleSlots.h"
#include "../../RenderWorld/RenderWorld.h"

using namespace RenderCore;

TEST_CASE("handle_partition_static_boundaries") {
    using namespace RenderWorld::detail;
    const auto mech = RenderObjectHandle::make(kMechHandleBase, 7u);
    for (const uint32_t index : {65535u, 65536u, 65537u, kMechHandleBase - 1u}) {
        const auto prop = staticPropHandle(index);
        REQUIRE(prop.isValid());
        CHECK(prop.index() == index);
        CHECK(prop.index() != mech.index());
        CHECK(decodeObjectId(prop.raw()) == prop);
    }
    CHECK_FALSE(staticPropHandle(-1).isValid());
    CHECK_FALSE(staticPropRecipeIndexFits(kMechHandleBase));
    CHECK_FALSE(staticPropHandle(kMechHandleBase).isValid());
    CHECK_FALSE(staticPropHandle(0x100000u).isValid());
    // A full append range still permits a valid recycled slot.
    CHECK(staticPropRecipeIndexFits(kMechHandleBase - 1u));
}

TEST_CASE("handle_partition_mech_object_id_roundtrip") {
    using namespace RenderWorld::detail;
    CHECK(kTerrainHandleBase - kMechHandleBase >= 0x30000u);
    for (const uint32_t index : {kMechHandleBase, kTerrainHandleBase - 1u}) {
        for (const uint32_t generation : {1u, 7u, 0xFFFu}) {
            const auto mech = RenderObjectHandle::make(index, generation);
            const auto decoded = decodeObjectId(mech.raw());
            CHECK(decoded == mech);
            CHECK(decoded.index() == index);
            CHECK(decoded.generation() == generation);
            CHECK_FALSE(staticPropRecipeIndexFits(decoded.index()));
        }
    }
}

TEST_SUITE("HandleSlotTable") {

TEST_CASE("fresh slots are dense and start at generation 1") {
    HandleSlotTable t;
    t.init(8);
    const auto a = t.alloc(0), b = t.alloc(0);
    CHECK(a.ok);
    CHECK(a.slot == 0);
    CHECK(b.slot == 1);
    CHECK(a.generation == 1);
    CHECK(t.liveCount() == 2);
}

TEST_CASE("a retired slot is not reused until its frame completed") {
    HandleSlotTable t;
    t.init(8);
    const auto a = t.alloc(0);
    CHECK(t.release(a.slot, a.generation, /*retireFrame=*/10));
    const auto b = t.alloc(/*completed=*/9);                 // frame 10 has not completed: a fresh slot
    CHECK(b.slot != a.slot);
    CHECK(t.parkedCount() == 1);
    const auto c = t.alloc(/*completed=*/10);                // frame 10 completed: the parked slot comes back
    CHECK(c.slot == a.slot);
    CHECK(c.generation != a.generation);
    CHECK(t.parkedCount() == 0);
}

TEST_CASE("negative control: reusing at once (no completion gate) would hand the live index to a new object") {
    HandleSlotTable t;
    t.init(8);
    const auto a = t.alloc(0);
    t.release(a.slot, a.generation, 10);
    // The gate is the only thing separating these two outcomes: with completed < retire the slot must not return, with completed >= retire it must.
    CHECK(t.alloc(5).slot != a.slot);
    CHECK(t.alloc(10).slot == a.slot);
}

TEST_CASE("a stale handle is refused after the slot is recycled; double release changes nothing") {
    HandleSlotTable t;
    t.init(8);
    const auto a = t.alloc(0);
    REQUIRE(t.release(a.slot, a.generation, 1));
    CHECK_FALSE(t.release(a.slot, a.generation, 1));          // already retired
    CHECK_FALSE(t.isLive(a.slot, a.generation));
    const auto c = t.alloc(1);
    CHECK(c.slot == a.slot);
    CHECK(t.isLive(c.slot, c.generation));
    CHECK_FALSE(t.isLive(a.slot, a.generation));              // the old handle names the same index with an old generation
    CHECK_FALSE(t.release(a.slot, a.generation, 2));          // and cannot retire the new occupant
    CHECK(t.isLive(c.slot, c.generation));
}

TEST_CASE("generations wrap past 0") {
    HandleSlotTable t;
    t.init(1);
    HandleSlotTable::Alloc a = t.alloc(0);
    for (uint32_t i = 0; i < HandleSlotTable::kMaxGeneration + 2; ++i) {
        REQUIRE(a.ok);
        CHECK(a.generation >= 1);
        CHECK(a.generation <= HandleSlotTable::kMaxGeneration);
        REQUIRE(t.release(a.slot, a.generation, i));
        a = t.alloc(i);
    }
}

TEST_CASE("an exhausted range refuses; reset() frees everything but keeps generations moving") {
    HandleSlotTable t;
    t.init(2);
    const auto old0 = t.alloc(0);
    CHECK(t.alloc(0).ok);
    CHECK_FALSE(t.alloc(0).ok);
    t.reset();
    CHECK(t.liveCount() == 0);
    CHECK_FALSE(t.isLive(old0.slot, old0.generation));        // the old mission's handle is stale at once
    const auto a = t.alloc(0);
    CHECK(a.ok);
    CHECK(a.slot == old0.slot);
    CHECK(a.generation != old0.generation);                   // the new mission's occupant never aliases it
    CHECK(t.isLive(a.slot, a.generation));
    CHECK_FALSE(t.release(old0.slot, old0.generation, 1));    // and the old handle cannot retire it
    CHECK(t.alloc(0).ok);
    CHECK_FALSE(t.alloc(0).ok);                               // the range is still two slots
    CHECK(t.highWater() == 2);
}

TEST_CASE("spawn, destroy, reload cycle: indices stay inside the range however many objects pass through") {
    HandleSlotTable t;
    t.init(4);
    uint64_t frame = 0;
    for (int i = 0; i < 1000; ++i) {
        const auto a = t.alloc(frame > 2 ? frame - 2 : 0);    // two frames in flight
        REQUIRE(a.ok);
        CHECK(a.slot < 4);
        ++frame;
        REQUIRE(t.release(a.slot, a.generation, frame));
    }
    CHECK(t.highWater() <= 4);
}

}  // TEST_SUITE
