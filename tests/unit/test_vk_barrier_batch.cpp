#include "doctest.h"
#include "../../RenderCore/vk_barrier_batch.h"
using namespace mc2vk::sync;
namespace {
const State colorWrite{7, 0x400, 0x100, true};       // COLOR_ATTACHMENT_OPTIMAL-like
const State sampled{5, 0x80, 0x20, false};           // SHADER_READ_ONLY-like
const State sampledOther{5, 0x800, 0x40, false};
}
TEST_CASE("BarrierBatch/source state comes from the previous use") {
    BarrierBatch b;
    ImageTracker t(1, 1);
    CHECK(t.use(b, colorWrite) == 1);                       // UNDEFINED -> write
    CHECK(b.images().back().before.layout == 0);
    CHECK(t.use(b, sampled) == 1);
    CHECK(b.images().back().before.stages == colorWrite.stages);
    CHECK(b.images().back().after.layout == sampled.layout);
    CHECK(t.use(b, sampled) == 0);                          // read after the same read: nothing to do
    CHECK(t.use(b, sampledOther) == 1);                     // a reader in a new stage needs its own visibility
    CHECK(t.use(b, colorWrite) == 1);                       // the writer waits for ALL readers
    CHECK(b.images().back().before.stages == (sampled.stages | sampledOther.stages));
    CHECK(b.size() == 4);
}
TEST_CASE("BarrierBatch/per mip runs, assume, invalidate and memory merge") {
    BarrierBatch b;
    ImageTracker t(2, 1, 4);
    CHECK(t.use(b, colorWrite) == 1);
    CHECK(b.images().back().mipCount == 4);
    CHECK(t.use(b, sampled, 1, 2) == 1);                    // mips 1-2 only
    CHECK(b.images().back().baseMip == 1);
    CHECK(b.images().back().mipCount == 2);
    t.assume(sampled);                                      // an outside producer left every mip sampled
    b.clear();
    CHECK(t.use(b, sampled) == 0);
    t.invalidate();
    CHECK(t.use(b, sampled) == 1);
    CHECK(b.images().back().before.layout == 0);
    b.clear();
    b.memory(1, 2, 4, 8);
    b.memory(16, 32, 64, 128);
    REQUIRE(b.memoryBarrier());
    CHECK(b.memoryBarrier()->srcStages == 17);
    CHECK(b.memoryBarrier()->dstAccess == 136);
    CHECK(b.size() == 1);
    b.clear();
    CHECK(b.empty());
}
