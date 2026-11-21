#include "doctest.h"
#include <cstring>
#include <vector>
#include "../../GameOS/gameos/mech_motion_history.h"

namespace {
// One actor of `n` bones, root at (x,0,0); each bone's rotation row 0 encodes its index so poses are distinguishable.
std::vector<float> pose(uint32_t n, float x, float tag = 0.0f) {
    std::vector<float> b(n * 16u, 0.0f);
    for (uint32_t i = 0; i < n; ++i) { b[i * 16u] = 1.0f + i + tag; b[i * 16u + 12] = x; }
    return b;
}
struct Inst { uint32_t a = 0, base = 0, rest[14] = {}; };   // 64 bytes, baseBoneOffset at byte 4
bool same(const std::vector<float>& a, const float* b) { return std::memcmp(a.data(), b, a.size() * sizeof(float)) == 0; }
}

TEST_CASE("MechMotionHistory: first seen, pose change, unchanged, gap, teleport, bone count") {
    MechMotionHistory h;
    Inst i0; const uint32_t key = 7;
    auto f1 = pose(3, 100.0f), f2 = pose(3, 105.0f, 0.5f);
    std::vector<float> tail(3 * 16u);
    h.stage(&i0, 1, f1.data(), 3, &key, 10, tail.data());
    CHECK(same(f1, tail.data()));                       // first seen: current
    h.stage(&i0, 1, f2.data(), 3, &key, 11, tail.data());
    CHECK(same(f1, tail.data()));                       // moved 5 units: previous palette
    h.stage(&i0, 1, f2.data(), 3, &key, 12, tail.data());
    CHECK(same(f2, tail.data()));                       // pose unchanged: previous == current bit for bit
    h.stage(&i0, 1, f1.data(), 3, &key, 14, tail.data());
    CHECK(same(f1, tail.data()));                       // frame gap (N, N+2): current
    auto far = pose(3, 100.0f + 65.0f);
    h.stage(&i0, 1, far.data(), 3, &key, 15, tail.data());
    h.stage(&i0, 1, f1.data(), 3, &key, 16, tail.data());
    CHECK(same(f1, tail.data()));                       // root moved 65 units: current
    auto f4 = pose(4, 100.0f);
    std::vector<float> tail4(4 * 16u);
    h.stage(&i0, 1, f4.data(), 4, &key, 17, tail4.data());
    CHECK(same(f4, tail4.data()));                      // bone count changed: current
    h.reset();
    CHECK(h.actorCount() == 0);
}

TEST_CASE("MechMotionHistory: two actors, one known one new; null keys") {
    MechMotionHistory h;
    Inst inst[2]; inst[1].base = 2;
    const uint32_t keys[2] = {1, 2};
    auto a0 = pose(2, 10.0f), a1 = pose(2, 20.0f), b = pose(2, 500.0f);
    std::vector<float> two(4 * 16u), prev(4 * 16u), tail(4 * 16u);
    std::memcpy(prev.data(), a0.data(), 128); std::memcpy(prev.data() + 32, b.data(), 128);
    // frame 1: only actor 1 (base 0) is staged
    h.stage(inst, 1, prev.data(), 2, keys, 1, tail.data());
    // frame 2: actors 1 (moved 5) and 2 (new)
    auto cur0 = pose(2, 15.0f, 0.5f);
    std::memcpy(two.data(), cur0.data(), 128); std::memcpy(two.data() + 32, a1.data(), 128);
    h.stage(inst, 2, two.data(), 4, keys, 2, tail.data());
    CHECK(std::memcmp(tail.data(), a0.data(), 128) == 0);               // known: previous
    CHECK(std::memcmp(tail.data() + 32, a1.data(), 128) == 0);          // new: current
    h.stage(inst, 2, two.data(), 4, nullptr, 3, tail.data());
    CHECK(std::memcmp(tail.data(), two.data(), 256) == 0);              // null keys: current
}
