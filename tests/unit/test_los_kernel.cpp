// T5: the pure LOS kernel on a synthetic heightfield + cell grid, and the worker matrix (1, 2, 4 workers bit-identical to serial).
// Kernel-vs-legacy bit identity is proven in-engine over tier1 queries (proofs/arch-t5), not here.
#include "doctest.h"
#include "los_kernel.h"
#include "job_system.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
struct Rng {
    uint32_t s;
    uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; }
    float unit() { return static_cast<float>(next() & 0xFFFF) / 65536.0f; }
};

struct World {
    struct Vtx { float pad[3]; float elevation; uint32_t rest[4]; };  // strided like PostcompVertex
    std::vector<Vtx> verts;
    std::vector<uint32_t> cells;
    mc2::los::Snapshot snap;

    World() {
        const long side = 60;
        Rng r{12345u};
        verts.resize(static_cast<size_t>(side * side));
        for (long y = 0; y < side; ++y)
            for (long x = 0; x < side; ++x)
                verts[static_cast<size_t>(x + y * side)].elevation =
                    300.0f * static_cast<float>((x * 7 + y * 3) % 11) + 900.0f * r.unit();
        const int cw = static_cast<int>(side * 3);
        cells.resize(static_cast<size_t>(cw * cw));
        for (auto& c : cells) {
            uint32_t v = 0;
            if ((r.next() & 7u) == 0) v |= mc2::los::kCellForestMask;
            if ((r.next() & 15u) == 0) v |= (r.next() & 15u) << mc2::los::kCellHeightShift;
            c = v;
        }
        snap.valid = true;
        snap.vertexBase = reinterpret_cast<const unsigned char*>(&verts[0].elevation);
        snap.vertexStride = sizeof(Vtx);
        snap.realVerticesMapSide = side;
        snap.verticesMapSide = side;
        snap.worldUnitsPerVertex = 128.0f;
        snap.oneOverWorldUnitsPerVertex = 1.0f / 128.0f;
        snap.worldUnitsMapSide = 128.0f * static_cast<float>(side);
        snap.mapTopLeft3d = glm::vec3(-3840.0f, 3840.0f, 0.0f);
        snap.topLeftVertexX = 0.0f;
        snap.topLeftVertexY = 0.0f;
        snap.cells = cells.data();
        snap.cellWidth = cw;
        snap.cellHeight = cw;
        snap.useRealLOS = true;
        snap.maxTeams = 8;
        snap.worldUnitsPerMeter = 5.01f;
        snap.smallEps = 1e-4f;
        snap.maxTreeLOSCellBlock = 5;
    }

    std::vector<mc2::los::Query> queries(int n, uint32_t seed) const {
        Rng r{seed};
        std::vector<mc2::los::Query> q(static_cast<size_t>(n));
        for (auto& e : q) {
            auto pt = [&](glm::vec3& p) {
                p.x = -3500.0f + 7000.0f * r.unit();
                p.y = -3500.0f + 7000.0f * r.unit();
                p.z = mc2::los::terrainElevation(snap, p) + 20.0f + 400.0f * r.unit();
            };
            pt(e.position);
            pt(e.target);
            e.teamId = static_cast<long>(r.next() % 9) - 1;  // includes -1 and 8 (no team: always visible)
            e.extRad = (r.next() & 1u) ? 30.0f * r.unit() : 0.0f;
            e.startExtRad = (r.next() & 1u) ? 30.0f * r.unit() : 0.0f;
        }
        return q;
    }
};
}  // namespace

TEST_CASE("los_kernel_worker_matrix") {
    World w;
    const int n = 6000;
    const std::vector<mc2::los::Query> q = w.queries(n, 99u);

    mc2::jobs::stopPool();
    std::vector<uint8_t> serial(static_cast<size_t>(n)), got(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        serial[static_cast<size_t>(i)] = mc2::los::query(w.snap, q[static_cast<size_t>(i)]) ? 1 : 0;

    int visible = 0;
    for (uint8_t v : serial) visible += v;
    CHECK(visible > n / 10);       // the corpus exercises both outcomes
    CHECK(visible < n - n / 10);

    for (int workers : {1, 2, 4}) {
        mc2::jobs::startPool(workers);
        std::fill(got.begin(), got.end(), uint8_t(2));
        mc2::los::batch(w.snap, q.data(), got.data(), n);
        CHECK(std::memcmp(serial.data(), got.data(), static_cast<size_t>(n)) == 0);
        mc2::jobs::stopPool();
    }
    // Pool off: batch runs inline and still matches.
    std::fill(got.begin(), got.end(), uint8_t(2));
    mc2::los::batch(w.snap, q.data(), got.data(), n);
    CHECK(std::memcmp(serial.data(), got.data(), static_cast<size_t>(n)) == 0);
}
