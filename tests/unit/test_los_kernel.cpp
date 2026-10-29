// T5: the pure LOS kernel on a synthetic heightfield + cell grid, and the worker matrix (1, 2, 4 workers bit-identical to serial).
// Kernel-vs-legacy bit identity is proven in-engine over tier1 queries (proofs/arch-t5), not here.
#include "doctest.h"
#include "los_kernel.h"
#include "job_system.h"
#include <algorithm>
#include <array>
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
        const long vs = static_cast<long>(sizeof(Vtx) / sizeof(float));
        snap.vertices = mc2::los::Snapshot::VertexGrid(&verts[0].elevation,
            std::layout_stride::mapping<std::dextents<long, 2>>(std::dextents<long, 2>(side, side),
                std::array<long, 2>{ side * vs, vs }));
        snap.verticesMapSide = side;
        snap.worldUnitsPerVertex = 128.0f;
        snap.oneOverWorldUnitsPerVertex = 1.0f / 128.0f;
        snap.worldUnitsMapSide = 128.0f * static_cast<float>(side);
        snap.mapTopLeft3d = glm::vec3(-3840.0f, 3840.0f, 0.0f);
        snap.topLeftVertexX = 0.0f;
        snap.topLeftVertexY = 0.0f;
        snap.cells = mc2::los::Snapshot::CellGrid(cells.data(), cw, cw);
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

// Verbatim structure of the legacy MapData::terrainElevation triangle selection (mclib/mapdata.cpp), kept here as the
// bit reference for the table-driven kernel version.
float legacyElevation(const World& w, const glm::vec3& position)
{
    const mc2::los::Snapshot& s = w.snap;
    const long side = s.vertices.extent(0);
    auto elevAt = [&](long x, long y) { return w.verts[static_cast<size_t>(x + y * side)].elevation; };
    glm::vec3 triVert[3] = {};
    glm::vec3 triPos, triangleVector[2], perpendicularVec;
    float result = 0.0f;
    float metersCheck = (s.worldUnitsMapSide / 2.0f);
    if (!((position.x > -metersCheck) && (position.x < metersCheck) && (position.y > -metersCheck) && (position.y < metersCheck)))
        return 0.0f;
    glm::vec2 upperLeft;
    upperLeft.x = floor(position.x * s.oneOverWorldUnitsPerVertex);
    upperLeft.x *= s.worldUnitsPerVertex;
    upperLeft.y = floor(position.y * s.oneOverWorldUnitsPerVertex);
    if (float(position.y * s.oneOverWorldUnitsPerVertex) != static_cast<float>(upperLeft.y))
        upperLeft.y += 1.0;
    upperLeft.y *= s.worldUnitsPerVertex;
    glm::vec2 ul2;
    ul2.x = upperLeft.x * float(s.oneOverWorldUnitsPerVertex);
    ul2.y = upperLeft.y * float(s.oneOverWorldUnitsPerVertex);
    long mx = static_cast<long>(floor(ul2.x)), my = static_cast<long>(floor(ul2.y));
    long vms = s.verticesMapSide;
    mx += (vms >> 1);
    my = (vms >> 1) - my;
    if ((mx < 0) || (mx >= (vms - 1)) || (my < 0) || (my >= (vms - 1)))
        return 0.0f;
    float z1 = elevAt(mx, my), z2 = elevAt(mx + 1, my), z3 = elevAt(mx + 1, my + 1), z4 = elevAt(mx, my + 1);
    triPos.x = s.worldUnitsPerVertex * floor(ul2.x);
    triPos.y = s.worldUnitsPerVertex * floor(ul2.y);
    triPos.z = z1;
    bool tlx = (long(s.topLeftVertexX) & 1), tly = (long(s.topLeftVertexY) & 1);
    long x = mx - s.topLeftVertexX, y = my - s.topLeftVertexY;
    bool yby2 = (y & 1) ^ (tly), xby2 = (x & 1) ^ (tlx);
    long uvMode = 0;  // 1 = bottom right, 2 = bottom left
    if (yby2) uvMode = xby2 ? 1 : 2; else uvMode = xby2 ? 2 : 1;
    float deltaX = 0.0f, deltaY = 0.0f;
    const float W = s.worldUnitsPerVertex;
    if (uvMode == 1)
    {
        deltaX = fabs(position.x - upperLeft.x);
        deltaY = fabs(upperLeft.y - position.y);
        if (deltaX > deltaY)
        {
            triVert[0] = triPos;
            triVert[1].x = triVert[0].x + W; triVert[1].y = triVert[0].y; triVert[1].z = z2;
            triVert[2].x = triVert[1].x; triVert[2].y = triVert[0].y - W; triVert[2].z = z3;
            triangleVector[0] = triVert[1] - triVert[0];
            triangleVector[1] = triVert[2] - triVert[0];
        }
        else
        {
            triVert[0] = triPos;
            triVert[1].x = triVert[0].x + W; triVert[1].y = triVert[0].y - W; triVert[1].z = z3;
            triVert[2].x = triVert[0].x; triVert[2].y = triVert[1].y; triVert[2].z = z4;
            triangleVector[0] = triVert[2] - triVert[0];
            triangleVector[1] = triVert[1] - triVert[0];
        }
    }
    else
    {
        deltaX = fabs((upperLeft.x + W) - position.x);
        deltaY = fabs(upperLeft.y - position.y);
        if (deltaX > deltaY)
        {
            triVert[1] = triPos;
            triVert[0].x = triPos.x + W; triVert[0].y = triPos.y; triVert[0].z = z2;
            triVert[2].x = triVert[1].x; triVert[2].y = triVert[1].y - W; triVert[2].z = z4;
            triangleVector[0] = triVert[1] - triVert[0];
            triangleVector[1] = triVert[2] - triVert[0];
        }
        else
        {
            triVert[0].x = triPos.x + W; triVert[0].y = triPos.y; triVert[0].z = z2;
            triVert[1].x = triPos.x; triVert[1].y = triVert[0].y - W; triVert[1].z = z4;
            triVert[2].x = triVert[0].x; triVert[2].y = triVert[1].y; triVert[2].z = z3;
            triangleVector[0] = triVert[2] - triVert[0];
            triangleVector[1] = triVert[1] - triVert[0];
        }
        deltaX = -deltaX;
    }
    triangleVector[0] = glm::normalize(triangleVector[0]);
    triangleVector[1] = glm::normalize(triangleVector[1]);
    perpendicularVec = glm::cross(triangleVector[0], triangleVector[1]);
    if (perpendicularVec.z != 0L)
    {
        if (perpendicularVec.z < 0L) perpendicularVec = -perpendicularVec;
        float px = perpendicularVec.x / perpendicularVec.z;
        float py = perpendicularVec.y / perpendicularVec.z;
        result = (deltaX * px);
        result += ((-deltaY) * py);
        result = -result;
        result += triVert[0].z;
    }
    return result;
}
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
        for (size_t count : {size_t(0), size_t(1), size_t(3), size_t(4), size_t(63), size_t(64), size_t(65), size_t(128), size_t(n)}) {
            std::fill(got.begin(), got.end(), uint8_t(2));
            mc2::los::batch(w.snap, std::span(q).first(count), std::span(got).first(count));
            CHECK(std::memcmp(serial.data(), got.data(), count) == 0);
            if (count < got.size()) CHECK(got[count] == 2);
        }
        mc2::jobs::stopPool();
    }
    // Pool off: batch runs inline and still matches.
    std::fill(got.begin(), got.end(), uint8_t(2));
    mc2::los::batch(w.snap, q, got);
    CHECK(std::memcmp(serial.data(), got.data(), static_cast<size_t>(n)) == 0);
}

TEST_CASE("los_kernel_elevation_matches_legacy_structure") {
    for (float tl : { 0.0f, 1.0f, 2.0f, 3.0f }) {
        World w;
        w.snap.topLeftVertexX = tl;
        w.snap.topLeftVertexY = 3.0f - tl;
        Rng r{777u};
        int bad = 0;
        for (int i = 0; i < 100000; ++i) {
            glm::vec3 p(-3900.0f + 7800.0f * r.unit(), -3900.0f + 7800.0f * r.unit(), 0.0f);
            const float a = mc2::los::terrainElevation(w.snap, p), b = legacyElevation(w, p);
            if (std::memcmp(&a, &b, sizeof(float)) != 0) ++bad;
        }
        CHECK(bad == 0);
    }
}
