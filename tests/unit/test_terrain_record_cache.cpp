// Terrain record cache (GameOS/gameos/terrain_record_cache.h): a still camera rebuilds 0 patches, one dirty patch rebuilds exactly one,
// and the cached result equals a full rebuild.
#include "doctest.h"
#include "terrain_record_cache.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace {
struct P { int32_t x, y; float depth; };
struct D { int32_t x, y; int mesh; bool operator==(const D& o) const { return x == o.x && y == o.y && mesh == o.mesh; } };
using Cache = mc2::terrain::RecordCache<P, D>;

struct Harness {
    uint32_t resolves = 0;
    uint32_t run(Cache& c, const std::vector<P>& ps, uint64_t key, float cx, float cy) {
        return c.update(std::span<const P>(ps), key, cx, cy,
            [&](const P& p, D& d) { ++resolves; if (p.depth < 0.0f) return false; d = {p.x, p.y, p.x % 3}; return true; },
            [&](const P& p) { const float dx = p.x - cx, dy = p.y - cy; return dx * dx + dy * dy; });
    }
};
std::vector<P> grid() {
    std::vector<P> v;
    for (int y = 0; y < 6; ++y) for (int x = 0; x < 6; ++x) v.push_back({x * 4, y * 4, 1.0f});
    return v;
}
}  // namespace

TEST_CASE("terrain record cache: static camera rebuilds 0 patches and keeps the generation") {
    Cache c; Harness h; auto ps = grid();
    CHECK(h.run(c, ps, 7, 10.0f, 10.0f) == ps.size());
    const uint64_t gen = c.generation();
    for (int i = 0; i < 5; ++i) CHECK(h.run(c, ps, 7, 10.0f, 10.0f) == 0u);
    CHECK(c.generation() == gen);
    CHECK(h.resolves == ps.size());
}

TEST_CASE("terrain record cache: one dirty patch rebuilds exactly one and matches a full rebuild") {
    Cache c; Harness h; auto ps = grid();
    h.run(c, ps, 7, 10.0f, 10.0f);
    const uint64_t gen = c.generation();
    ps[13].x += 1;                                   // LOD / block change for one patch
    CHECK(h.run(c, ps, 7, 10.0f, 10.0f) == 1u);
    CHECK(c.generation() == gen + 1);
    Cache full; Harness h2;
    h2.run(full, ps, 7, 10.0f, 10.0f);
    CHECK(c.draws() == full.draws());
}

TEST_CASE("terrain record cache: camera move reorders without re-resolving, equals a full rebuild, keeps front-to-back") {
    Cache c; Harness h; auto ps = grid();
    h.run(c, ps, 7, 0.0f, 0.0f);
    CHECK(h.run(c, ps, 7, 20.0f, 20.0f) == 0u);
    Cache full; Harness h2;
    h2.run(full, ps, 7, 20.0f, 20.0f);
    CHECK(c.draws() == full.draws());
    float prev = -1.0f;
    for (const D& d : c.draws()) {
        const float dx = d.x - 20.0f, dy = d.y - 20.0f, dist = dx * dx + dy * dy;
        CHECK(dist >= prev); prev = dist;
    }
}

TEST_CASE("terrain record cache: a sub-order camera nudge keeps the generation; an external key change rebuilds all") {
    Cache c; Harness h; auto ps = grid();
    h.run(c, ps, 7, 10.1f, 10.1f);
    const uint64_t gen = c.generation();
    CHECK(h.run(c, ps, 7, 10.1001f, 10.1f) == 0u);
    CHECK(c.generation() == gen);
    CHECK(h.run(c, ps, 8, 10.1f, 10.1f) == ps.size());
    CHECK(c.generation() == gen + 1);
}

TEST_CASE("terrain record cache: a patch without a mesh is dropped from the draws and stays dropped") {
    Cache c; Harness h; auto ps = grid();
    ps[5].depth = -1.0f;
    h.run(c, ps, 7, 10.0f, 10.0f);
    CHECK(c.draws().size() == ps.size() - 1);
    CHECK(h.run(c, ps, 7, 10.0f, 10.0f) == 0u);
    CHECK(c.draws().size() == ps.size() - 1);
}
