// tests/unit/test_frame_arena.cpp
// GL-free unit tests for RenderCore::FrameArena.
//
// FrameArena is the typed L2 (frame-lifetime) linear allocator already in
// production behind render_snapshot.cpp's ping-pong buffers. These tests pin
// its contract independently of the renderer: no GL context, no game startup,
// no asset I/O. The backing buffer is a plain stack/global byte array owned by
// the test -- mirroring how render_snapshot.cpp owns s_arenaBuffers and hands
// non-owning pointers to FrameArena::init().
#include "doctest.h"
#include "FrameArena.h"

#include <cstdint>
#include <cstddef>

using namespace RenderCore;

namespace {

// Distinct alignment requirements so we can prove the aligner does real work.
struct Rec4  { uint32_t a; };                 // alignof 4
struct Rec8  { uint64_t a; };                 // alignof 8
struct alignas(64) RecOver { uint32_t a; };   // over-aligned (cache line)

static_assert(alignof(RecOver) == 64, "RecOver must be over-aligned for the test");

// Helper: a fixed backing buffer that is itself over-aligned, so the arena's
// own alignment math -- not the buffer's accidental address -- is under test.
template <size_t N>
struct Backing {
    alignas(64) uint8_t bytes[N];
};

bool isAligned(const void* p, size_t a) {
    return (reinterpret_cast<uintptr_t>(p) & (a - 1)) == 0;
}

} // namespace

TEST_SUITE("RenderCore") {

TEST_CASE("FrameArena raw address alignment and huge requests") {
    alignas(64) uint8_t backing[129];
    FrameArena arena; arena.init(backing + 1, 128);
    CHECK(arena.allocBytes(1, 64, "unaligned base") == backing + 64);
    const auto used = arena.bytesUsed();
    CHECK(arena.allocBytes(SIZE_MAX, 1, "overflow") == nullptr);
    CHECK(arena.bytesUsed() == used);
    CHECK(arena.allocBytes(65, 1, "too large") == nullptr);
    CHECK(arena.allocBytes(64, 1, "exact fit") == backing + 65);
    CHECK(arena.remaining() == 0);
    arena.init(backing + 1, 1);
    CHECK(arena.allocBytes(1, 64, "padding exceeds capacity") == nullptr);
}

TEST_CASE("FrameArena initial capacity/used/remaining") {
    Backing<1024> buf;
    FrameArena arena;
    arena.init(buf.bytes, sizeof(buf.bytes));

    CHECK(arena.capacity()  == 1024);
    CHECK(arena.bytesUsed() == 0);
    CHECK(arena.remaining() == 1024);
    CHECK(arena.overflowed() == false);

    const FrameArenaStats& s = arena.stats();
    CHECK(s.capacityBytes  == 1024);
    CHECK(s.usedBytes      == 0);
    CHECK(s.highWaterBytes == 0);
    CHECK(s.allocCount     == 0);
    CHECK(s.overflowCount  == 0);
}

TEST_CASE("FrameArena typed allocation returns correct count/span/pointer") {
    Backing<1024> buf;
    FrameArena arena;
    arena.init(buf.bytes, sizeof(buf.bytes));

    Span<Rec8> span = arena.allocArray<Rec8>(10, "Rec8");
    REQUIRE(span.data != nullptr);
    CHECK(span.count == 10);
    CHECK(span.size() == 10);
    CHECK(span.empty() == false);

    // Pointer lies inside the backing buffer and is correctly aligned.
    CHECK(reinterpret_cast<uintptr_t>(span.data) >= reinterpret_cast<uintptr_t>(buf.bytes));
    CHECK(isAligned(span.data, alignof(Rec8)));

    // Usage accounting: 10 * 8 = 80 bytes consumed.
    CHECK(arena.bytesUsed() == 80);
    CHECK(arena.remaining() == 1024 - 80);
    CHECK(arena.stats().allocCount == 1);

    // Span is writable and round-trips through begin()/end().
    for (uint32_t i = 0; i < span.count; ++i) span[i].a = i + 1;
    uint64_t sum = 0;
    for (const Rec8& r : span) sum += r.a;
    CHECK(sum == 55);  // 1..10
}

TEST_CASE("FrameArena reset returns used bytes to zero, preserves high-water") {
    Backing<1024> buf;
    FrameArena arena;
    arena.init(buf.bytes, sizeof(buf.bytes));

    arena.allocArray<Rec8>(16, "Rec8");      // 128 bytes
    CHECK(arena.bytesUsed() == 128);
    CHECK(arena.stats().highWaterBytes == 128);

    arena.reset(1);
    CHECK(arena.bytesUsed() == 0);
    CHECK(arena.remaining() == 1024);
    CHECK(arena.stats().frameIndex    == 1);
    CHECK(arena.stats().allocCount    == 0);
    CHECK(arena.stats().overflowCount == 0);
    // High-water persists across reset (lifetime peak).
    CHECK(arena.stats().highWaterBytes == 128);

    // A smaller subsequent frame does not lower the high-water mark.
    arena.allocArray<Rec8>(4, "Rec8");       // 32 bytes
    CHECK(arena.bytesUsed() == 32);
    CHECK(arena.stats().highWaterBytes == 128);
}

TEST_CASE("FrameArena alignment for common and over-aligned types") {
    Backing<4096> buf;
    FrameArena arena;
    arena.init(buf.bytes, sizeof(buf.bytes));

    // Force a 1-byte cursor offset by allocating a single odd-sized blob, then
    // prove each typed alloc still lands on its required alignment boundary.
    void* odd = arena.allocBytes(1, 1, "odd");
    REQUIRE(odd != nullptr);

    Span<Rec4> s4 = arena.allocArray<Rec4>(1, "Rec4");
    REQUIRE(s4.data != nullptr);
    CHECK(isAligned(s4.data, alignof(Rec4)));

    Span<Rec8> s8 = arena.allocArray<Rec8>(1, "Rec8");
    REQUIRE(s8.data != nullptr);
    CHECK(isAligned(s8.data, alignof(Rec8)));

    Span<RecOver> so = arena.allocArray<RecOver>(2, "RecOver");
    REQUIRE(so.data != nullptr);
    CHECK(isAligned(so.data, 64));
}

TEST_CASE("FrameArena sequential allocations preserve alignment and do not overlap") {
    Backing<4096> buf;
    FrameArena arena;
    arena.init(buf.bytes, sizeof(buf.bytes));

    Span<Rec8>    a = arena.allocArray<Rec8>(3, "a");   // 24 bytes @ align 8
    Span<RecOver> b = arena.allocArray<RecOver>(2, "b"); // 128 bytes @ align 64
    Span<Rec4>    c = arena.allocArray<Rec4>(5, "c");   // 20 bytes @ align 4
    REQUIRE(a.data != nullptr);
    REQUIRE(b.data != nullptr);
    REQUIRE(c.data != nullptr);

    CHECK(isAligned(a.data, alignof(Rec8)));
    CHECK(isAligned(b.data, 64));
    CHECK(isAligned(c.data, alignof(Rec4)));

    auto aBeg = reinterpret_cast<uintptr_t>(a.begin());
    auto aEnd = reinterpret_cast<uintptr_t>(a.end());
    auto bBeg = reinterpret_cast<uintptr_t>(b.begin());
    auto bEnd = reinterpret_cast<uintptr_t>(b.end());
    auto cBeg = reinterpret_cast<uintptr_t>(c.begin());
    auto cEnd = reinterpret_cast<uintptr_t>(c.end());

    // Strictly increasing, non-overlapping regions (alignment padding may gap).
    CHECK(aEnd <= bBeg);
    CHECK(bEnd <= cBeg);
    CHECK(aBeg < aEnd);
    CHECK(bBeg < bEnd);
    CHECK(cBeg < cEnd);

    // Fill each region with a distinct pattern; verify no cross-contamination.
    for (uint32_t i = 0; i < a.count; ++i) a[i].a = 0xAAAAAAAAu;
    for (uint32_t i = 0; i < b.count; ++i) b[i].a = 0xBBBBBBBBu;
    for (uint32_t i = 0; i < c.count; ++i) c[i].a = 0xCCCCCCCCu;
    for (uint32_t i = 0; i < a.count; ++i) CHECK(a[i].a == 0xAAAAAAAAu);
    for (uint32_t i = 0; i < b.count; ++i) CHECK(b[i].a == 0xBBBBBBBBu);
    for (uint32_t i = 0; i < c.count; ++i) CHECK(c[i].a == 0xCCCCCCCCu);
}

TEST_CASE("FrameArena overflow fails safely without corrupting arena state") {
    Backing<128> buf;
    FrameArena arena;
    arena.init(buf.bytes, sizeof(buf.bytes));

    // Consume most of the buffer.
    Span<Rec8> ok = arena.allocArray<Rec8>(12, "ok");  // 96 bytes
    REQUIRE(ok.data != nullptr);
    size_t usedBefore = arena.bytesUsed();
    CHECK(usedBefore == 96);

    // Request more than remains (12 * 8 = 96 > 32 left) -> must fail closed.
    Span<Rec8> bad = arena.allocArray<Rec8>(12, "bad");
    CHECK(bad.data == nullptr);
    CHECK(bad.count == 0);
    CHECK(bad.empty());
    CHECK(arena.overflowed());
    CHECK(arena.stats().overflowCount == 1);

    // Failed alloc must NOT advance the cursor or corrupt accounting.
    CHECK(arena.bytesUsed() == usedBefore);

    // Arena is still usable: a fitting allocation after overflow succeeds and
    // does not alias the earlier live region.
    Span<Rec8> tail = arena.allocArray<Rec8>(4, "tail");  // 32 bytes -> exactly fills
    REQUIRE(tail.data != nullptr);
    CHECK(reinterpret_cast<uintptr_t>(tail.begin()) >= reinterpret_cast<uintptr_t>(ok.end()));
    CHECK(arena.bytesUsed() == 128);
    CHECK(arena.remaining() == 0);
}

TEST_CASE("FrameArena zero-count and zero-byte allocations are inert") {
    Backing<256> buf;
    FrameArena arena;
    arena.init(buf.bytes, sizeof(buf.bytes));

    Span<Rec8> zero = arena.allocArray<Rec8>(0, "zero");
    CHECK(zero.data == nullptr);
    CHECK(zero.empty());
    CHECK(arena.bytesUsed() == 0);
    CHECK(arena.stats().allocCount == 0);   // zero-count does not count as an alloc
    CHECK(arena.overflowed() == false);     // ...nor as an overflow

    CHECK(arena.allocBytes(0, 8, "zb") == nullptr);
    CHECK(arena.bytesUsed() == 0);
}

TEST_CASE("FrameArena frame-flip accounting across repeated reset cycles") {
    // Models the per-frame lifecycle that render_snapshot.cpp drives on each of
    // its two ping-pong arenas: reset(frameIndex) then a wave of allocArray.
    Backing<512> buf;
    FrameArena arena;
    arena.init(buf.bytes, sizeof(buf.bytes));

    size_t peak = 0;
    for (uint32_t frame = 0; frame < 5; ++frame) {
        arena.reset(frame);
        CHECK(arena.bytesUsed() == 0);
        CHECK(arena.stats().frameIndex == frame);

        uint32_t n = (frame % 3) + 1;             // varying load per frame
        Span<Rec8> s = arena.allocArray<Rec8>(n, "frame");
        REQUIRE(s.data != nullptr);
        size_t used = static_cast<size_t>(n) * sizeof(Rec8);
        CHECK(arena.bytesUsed() == used);
        peak = used > peak ? used : peak;
        CHECK(arena.stats().highWaterBytes == peak);
    }
}

} // TEST_SUITE("RenderCore")
