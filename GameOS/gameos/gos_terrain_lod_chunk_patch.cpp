// gos_terrain_lod_chunk: per-LOD patch shape cache: sample positions, patch key and getOrBuildPatch.
// Declared in the class headers; shared names in gos_terrain_lod_chunk_internal.h.
#include "gos_terrain_lod_chunk_internal.h"


std::map<uint32_t, PatchShape> s_patchCache;

// patchKey now also carries the 2-bit block-origin parity (X&1, Y&1) so the
// shared (qcX,qcY,lodStep) patch cache distinguishes the two checkerboard
// phasings. Gate OFF -> parity bits always 0 (only the fixed-TL-BR patch built).
static uint32_t patchKey(int qcX, int qcY, int lodStep, int parityX, int parityY)
{
    return (static_cast<uint32_t>(qcX) & 0xFF)
         | ((static_cast<uint32_t>(qcY)    & 0xFF) << 8)
         | ((static_cast<uint32_t>(lodStep) & 0xFF) << 16)
         | ((static_cast<uint32_t>((parityX & 1))) << 24)
         | ((static_cast<uint32_t>((parityY & 1))) << 25);
}

// Build sample positions along one axis, always including far edge.
static std::vector<int> makeSamplePositions(int quadCount, int lodStep)
{
    std::vector<int> pos;
    for (int p = 0; p <= quadCount; p += lodStep)
        pos.push_back(p);
    if (pos.back() != quadCount)
        pos.push_back(quadCount);
    return pos;
}

// checkerDiag: when true, emit the per-cell checkerboard diagonal keyed on
// ABSOLUTE world-tile parity (blockOriginParity + mapped sample cell), matching
// worldQuadUVMode. parityX/parityY are blockOriginX&1 / blockOriginY&1.
// Caller MUST only pass checkerDiag=true for lodStep==1 (and not the 4x
// visualDisplace path) — coarse LOD per-cell parity is ill-defined.
const PatchShape& getOrBuildPatch(int qcX, int qcY, int lodStep,
                                         bool checkerDiag, int parityX, int parityY)
{
    // parity bits only meaningful when the checkerboard is active; keep the
    // fixed-TL-BR cache entry under parity (0,0) so gate-OFF is byte-identical.
    const int keyParX = checkerDiag ? (parityX & 1) : 0;
    const int keyParY = checkerDiag ? (parityY & 1) : 0;
    uint32_t key = patchKey(qcX, qcY, lodStep, keyParX, keyParY);
    auto it = s_patchCache.find(key);
    if (it != s_patchCache.end()) return it->second;

    auto xs = makeSamplePositions(qcX, lodStep);
    auto ys = makeSamplePositions(qcY, lodStep);

    struct LocalVertex { int16_t lx, ly; };
    std::vector<LocalVertex> verts;
    verts.reserve(xs.size() * ys.size());
    for (int yy : ys)
        for (int xx : xs)
            verts.push_back({static_cast<int16_t>(xx), static_cast<int16_t>(yy)});

    // Two CCW triangles per quad cell.
    //   FIXED / BOTTOMRIGHT (TL-BR diagonal): {TL,BL,BR} + {TL,BR,TR}
    //   BOTTOMLEFT          (TR-BL diagonal): {TL,BL,TR} + {BL,BR,TR}
    // Both keep the SAME front-face (CCW) winding as the original fixed split,
    // so backface culling is unchanged. The diagonal choice mirrors
    // worldQuadUVMode(absRow,absCol): BOTTOMRIGHT when (absY&1)==(absX&1).
    // absX = parityX*blockBase + xs[i] ; we have blockOriginX&1 in parityX and
    // the sample's local cell coord xs[i] -> absolute cell parity is the XOR.
    // NOTE xs[i] is the LOD-decimated map-cell coordinate (lodStep==1 here, so
    // xs[i] is exactly the cell index), used directly — NOT the raw loop i.
    std::vector<uint16_t> indices;
    int W = static_cast<int>(xs.size());
    for (int j = 0; j < static_cast<int>(ys.size()) - 1; ++j) {
        for (int i = 0; i < static_cast<int>(xs.size()) - 1; ++i) {
            uint16_t tl = static_cast<uint16_t>((j*W+i));
            uint16_t tr = static_cast<uint16_t>((j*W+i+1));
            uint16_t bl = static_cast<uint16_t>(((j+1)*W+i));
            uint16_t br = static_cast<uint16_t>(((j+1)*W+i+1));

            bool bottomRight = true;  // fixed default == BOTTOMRIGHT (TL-BR)
            if (checkerDiag) {
                // Absolute map cell of this quad's TL corner. xs/ys are local
                // sample positions; with lodStep==1 they equal the local cell
                // index. Add block-origin parity for absolute world parity.
                const int absX = (parityX & 1) ^ (xs[i] & 1);
                const int absY = (parityY & 1) ^ (ys[j] & 1);
                // worldQuadUVMode: BOTTOMRIGHT when (tileR&1)==(tileC&1).
                bottomRight = (absY == absX);
            }

            if (bottomRight) {
                indices.insert(indices.end(), {tl, bl, br, tl, br, tr});
            } else {
                indices.insert(indices.end(), {tl, bl, tr, bl, br, tr});
            }
        }
    }

    PatchShape ps;
    ps.vertexCount = static_cast<int>(verts.size());
    ps.indexCount  = static_cast<int>(indices.size());

    ps.vbo.create();
    ps.vbo.bind();
    ps.vbo.bufferData(static_cast<intptr_t>((verts.size() * sizeof(LocalVertex))),
                      verts.data(), RenderCore::GpuBufferUsage::StaticDraw);

    ps.ibo.create();
    ps.ibo.bind();
    ps.ibo.bufferData(static_cast<intptr_t>((indices.size() * sizeof(uint16_t))),
                      indices.data(), RenderCore::GpuBufferUsage::StaticDraw);

    ps.vbo.unbind();  // GL_ARRAY_BUFFER -> 0
    ps.ibo.unbind();  // GL_ELEMENT_ARRAY_BUFFER -> 0

    // -----------------------------------------------------------------------
    // Phase 6: Build skirt geometry — four edge strips, each a quad-strip.
    // SkirtVertex: lx, ly, isSkirt (0=surface, 1=below), _pad.
    // The vertex shader reads isSkirt and applies: h -= isSkirt * u_skirtDepth.
    // Winding is CCW viewed from outside; backface culling is disabled during
    // skirt draws so winding does not need to be perfect in this first pass.
    // -----------------------------------------------------------------------
    struct SkirtVertex { int16_t lx, ly, isSkirt, _pad; };
    std::vector<SkirtVertex> skirtVerts;
    std::vector<uint16_t>    skirtIdx;

    // Each edge strip has 2 * edgeLen vertices.
    // Indices: for each quad in the strip, 2 triangles connecting top[i]/bot[i] to top[i+1]/bot[i+1].
    // Layout within strip (base offset B, 2 verts per column: top=B+2*i, bot=B+2*i+1):
    //   tri1: top[i], bot[i], bot[i+1]  → B+2*i, B+2*i+1, B+2*(i+1)+1
    //   tri2: top[i], bot[i+1], top[i+1] → B+2*i, B+2*(i+1)+1, B+2*(i+1)
    // This is CCW when the strip faces the camera from the outside (correct for all four edges
    // when backface culling is disabled, so no per-edge winding correction needed).

    auto buildEdge = [&](const std::vector<int>& uPos, int fixedCoord, bool fixedIsY)
    {
        int base = static_cast<int>(skirtVerts.size());
        int n    = static_cast<int>(uPos.size());
        for (int i = 0; i < n; ++i) {
            int16_t lx = fixedIsY ? static_cast<int16_t>(uPos[i]) : static_cast<int16_t>(fixedCoord);
            int16_t ly = fixedIsY ? static_cast<int16_t>(fixedCoord) : static_cast<int16_t>(uPos[i]);
            skirtVerts.push_back({lx, ly, 0, 0});  // surface
            skirtVerts.push_back({lx, ly, 1, 0});  // below
        }
        for (int i = 0; i < n - 1; ++i) {
            uint16_t t0 = static_cast<uint16_t>((base + 2*i));
            uint16_t b0 = static_cast<uint16_t>((base + 2*i + 1));
            uint16_t t1 = static_cast<uint16_t>((base + 2*(i+1)));
            uint16_t b1 = static_cast<uint16_t>((base + 2*(i+1) + 1));
            skirtIdx.insert(skirtIdx.end(), {t0, b0, b1, t0, b1, t1});
        }
    };

    // Phase 10.2b: record each edge's index range (build order N,S,W,E).
    ps.skirtEdgeOffset[0] = static_cast<int>(skirtIdx.size()); buildEdge(xs, static_cast<int>(ys.front()), false); ps.skirtEdgeCount[0] = static_cast<int>(skirtIdx.size()) - ps.skirtEdgeOffset[0]; // North (y=ys[0])
    ps.skirtEdgeOffset[1] = static_cast<int>(skirtIdx.size()); buildEdge(xs, static_cast<int>(ys.back()),  false); ps.skirtEdgeCount[1] = static_cast<int>(skirtIdx.size()) - ps.skirtEdgeOffset[1]; // South (y=ys.back)
    ps.skirtEdgeOffset[2] = static_cast<int>(skirtIdx.size()); buildEdge(ys, static_cast<int>(xs.front()), true ); ps.skirtEdgeCount[2] = static_cast<int>(skirtIdx.size()) - ps.skirtEdgeOffset[2]; // West  (x=xs[0])
    ps.skirtEdgeOffset[3] = static_cast<int>(skirtIdx.size()); buildEdge(ys, static_cast<int>(xs.back()),  true ); ps.skirtEdgeCount[3] = static_cast<int>(skirtIdx.size()) - ps.skirtEdgeOffset[3]; // East  (x=xs.back)

    ps.skirtVertexCount = static_cast<int>(skirtVerts.size());
    ps.skirtIndexCount  = static_cast<int>(skirtIdx.size());

    ps.skirtVbo.create();
    ps.skirtVbo.bind();
    ps.skirtVbo.bufferData(static_cast<intptr_t>((skirtVerts.size() * sizeof(SkirtVertex))),
                           skirtVerts.data(), RenderCore::GpuBufferUsage::StaticDraw);

    ps.skirtIbo.create();
    ps.skirtIbo.bind();
    ps.skirtIbo.bufferData(static_cast<intptr_t>((skirtIdx.size() * sizeof(uint16_t))),
                           skirtIdx.data(), RenderCore::GpuBufferUsage::StaticDraw);

    ps.skirtVbo.unbind();  // GL_ARRAY_BUFFER -> 0
    ps.skirtIbo.unbind();  // GL_ELEMENT_ARRAY_BUFFER -> 0

    s_patchCache[key] = std::move(ps);  // GpuBuffer members are move-only RAII
    return s_patchCache.at(key);
}
