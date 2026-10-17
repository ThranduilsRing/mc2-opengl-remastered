// tests/unit/test_frame_ring.cpp
// The offset arithmetic of the Vulkan frame ring (vk_frame_ring.h): two slot regions, linear bump, alignment, slot restart per frame, growth sizing.
#include "doctest.h"
#include "vk_frame_ring.h"

using namespace mc2vk;

TEST_SUITE("FrameRingCore") {

TEST_CASE("allocations bump and align inside the frame slot") {
    FrameRingCore r;
    r.setCapacity(1024);
    r.beginFrame(1);                                     // slot 1: the region starts at 1024
    uint64_t a = 0, b = 0, c = 0;
    CHECK(r.alloc(10, 16, &a));
    CHECK(a == 1024);
    CHECK(r.alloc(100, 256, &b));
    CHECK(b == 1024 + 256);                              // aligned up from 10
    CHECK(r.alloc(1, 1, &c));
    CHECK(c == 1024 + 256 + 100);
    CHECK(r.used() == 256 + 100 + 1);
    CHECK(r.peak() == 256 + 100 + 1);
    r.beginFrame(2);                                     // a later, smaller frame does not lower the peak
    uint64_t d = 0;
    CHECK(r.alloc(8, 1, &d));
    CHECK(r.peak() == 256 + 100 + 1);
}

TEST_CASE("the two frames in flight never share bytes") {
    FrameRingCore r;
    r.setCapacity(512);
    uint64_t f2 = 0, f3 = 0, f4 = 0;
    r.beginFrame(2);
    CHECK(r.alloc(512, 1, &f2));                         // slot 0 full
    CHECK(f2 == 0);
    r.beginFrame(3);
    CHECK(r.alloc(512, 1, &f3));                         // slot 1
    CHECK(f3 == 512);
    r.beginFrame(4);                                     // slot 0 again: restarts at its base
    CHECK(r.alloc(8, 1, &f4));
    CHECK(f4 == 0);
}

TEST_CASE("a request that does not fit is refused and leaves the head alone") {
    FrameRingCore r;
    r.setCapacity(100);
    r.beginFrame(0);
    uint64_t o = 0;
    CHECK(r.alloc(60, 1, &o));
    CHECK_FALSE(r.alloc(41, 1, &o));
    CHECK(r.used() == 60);
    CHECK(r.alloc(40, 1, &o));                           // exactly the rest
    CHECK_FALSE(r.alloc(1, 1, &o));
}

TEST_CASE("alignment padding counts against the slot") {
    FrameRingCore r;
    r.setCapacity(300);
    r.beginFrame(0);
    uint64_t o = 0;
    CHECK(r.alloc(1, 1, &o));
    CHECK_FALSE(r.alloc(100, 256, &o));                  // aligned start 256 + 100 > 300
}

TEST_CASE("grown capacity doubles and always fits the request twice over") {
    FrameRingCore r;
    r.setCapacity(1000);
    CHECK(r.grownCapacity(10) == 2000);
    CHECK(r.grownCapacity(5000) >= 10000);
    r.setCapacity(0);
    CHECK(r.grownCapacity(3) >= 6);
    r.setCapacity(2000);                                 // a new buffer starts both slots empty
    r.beginFrame(1);
    uint64_t o = 0;
    CHECK(r.alloc(2000, 1, &o));
    CHECK(o == 2000);
}

}  // TEST_SUITE
