// The descriptor-write cache key: a stamp skips a write only while its destination already holds exactly that key in
// the current descriptor epoch; any key word, an epoch bump or an invalidate forces the write, and the revision a
// consumer keys on moves whenever the destination is (re)written.
#include "doctest.h"

#include "vk_desc_write_cache.h"

using mc2vk::DescKey;
using mc2vk::DescWriteStamp;

TEST_CASE("desc write stamp: first write, then skip while the key holds") {
    DescWriteStamp s;
    const DescKey k{1, 2, 3, 4};
    CHECK(s.needsWrite(k, 7));
    const uint64_t rev = s.rev;
    CHECK_FALSE(s.needsWrite(k, 7));
    CHECK_FALSE(s.needsWrite(k, 7));
    CHECK(s.rev == rev);
}

TEST_CASE("desc write stamp: any key word change rewrites (dummy vs live mode is a key word)") {
    for (int w = 0; w < 4; ++w) {
        DescWriteStamp s;
        DescKey k{10, 20, 30, 0};
        CHECK(s.needsWrite(k, 1));
        const uint64_t rev = s.rev;
        k[w] += 1;
        CHECK(s.needsWrite(k, 1));
        CHECK(s.rev != rev);
        CHECK_FALSE(s.needsWrite(k, 1));
    }
}

TEST_CASE("desc write stamp: an epoch bump rewrites the same key (recycled handle value)") {
    DescWriteStamp s;
    const DescKey k{0xABCD, 0, 0, 0};
    CHECK(s.needsWrite(k));
    CHECK_FALSE(s.needsWrite(k));
    mc2vk::bumpDescEpoch();
    CHECK(s.needsWrite(k));
    CHECK_FALSE(s.needsWrite(k));
}

TEST_CASE("desc write stamp: invalidate forces the next write and moves the revision") {
    DescWriteStamp s;
    const DescKey k{5, 6, 7, 8};
    CHECK(s.needsWrite(k, 3));
    const uint64_t rev = s.rev;
    s.invalidate();
    CHECK(s.rev != rev);
    CHECK(s.needsWrite(k, 3));
    CHECK_FALSE(s.needsWrite(k, 3));
}

TEST_CASE("desc write stamp: stamps are per destination") {
    DescWriteStamp a, b;
    const DescKey k{1, 1, 1, 1};
    CHECK(a.needsWrite(k, 2));
    CHECK(b.needsWrite(k, 2));   // the other set has never been written
    CHECK(a.rev != b.rev);
}

TEST_CASE("desc key word: pointers and integers") {
    int x = 0;
    CHECK(mc2vk::descKeyWord(&x) == static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&x)));
    CHECK(mc2vk::descKeyWord(uint64_t{42}) == 42u);
    CHECK(mc2vk::descKeyWord(static_cast<int*>(nullptr)) == 0u);
}

TEST_CASE("desc write counts accumulate issued and skipped") {
    auto& c = mc2vk::descWriteCounts();
    const uint64_t i0 = c.issued.load(), s0 = c.skipped.load();
    mc2vk::noteDescWrites(3, 6);
    mc2vk::noteDescWrites(0, 2);
    CHECK(c.issued.load() - i0 == 3);
    CHECK(c.skipped.load() - s0 == 8);
}
