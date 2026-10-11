// GameAdapters/VegetationAdapter.cpp
//
// Vegetation card placement scan and per-frame flush.
// This is a GameAdapters bridge TU: it may reach both game-side headers
// (mclib/) and engine-side headers (GameOS/gameos/).

#include "VegetationAdapter.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "Gates/gate.h"

// Engine side.
#include "gos_vegetation.h"

// Game side -- terrain geometry + type queries.
#include "../mclib/terrain.h"
// Game side -- terrain type codes (TerrainType enum).
#include "../mclib/dmapdata.h"
// Game side -- MissionMap overlay / water queries.
#include "../mclib/move.h"

// Stuff::Matrix4D (entries[16], column-major) + Stuff::Vector3D.
// Full chain required (matrix.hpp alone creates circular include ordering).
#include "Stuff/Stuff.hpp"

#include "../../GameOS/gameos/diagnostic_trace.h"

#include <vector>
#include <algorithm> // std::max
#include <cstdlib>   // std::getenv, std::atoi
#include <cstdio>    // fprintf, snprintf
#include <cmath>     // sinf, fmodf, fabsf
#include "mc2fmt.h"

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Cheap, deterministic hash in [0, 1).  Inputs are world-grid coordinates
// plus a per-instance discriminator so each card within a cell differs.
[[nodiscard]] float hashf(float a, float b) noexcept
{
    return fabsf(fmodf(sinf(a * 127.1f + b * 311.7f) * 43758.5f, 1.0f));
}

// World-unit spacing between sample points.
// ~5x area density vs 32: (32/14)^2 ≈ 5.2x more grid cells per area.
constexpr float kGridStep = 14.0f;

// ---------------------------------------------------------------------------
// Placement accept/reject helpers
// ---------------------------------------------------------------------------

// Vegetation density tier from terrain material type (getTerrain codes).
// kHard   = absolute no-vegetation: water, concrete, cement, cliff, none
// kNone   = hard exclude (kept for 15% fringe logic at borders)
// kSparse = transitional surfaces: dirt, ash, mountain, slimy
// kFull   = vegetated surfaces: grass, moss, forest floor, mud, tundra
enum class VegDensity : int { kHard = -1, kNone = 0, kSparse = 1, kFull = 2 };

// Atlas frame palettes per vegetation density tier.
// vegetation_atlas_v2.png: 2 rows x 4 cols = 8 frames.
//   Row 0 (frames 0-3): grass / ground-cover variants
//   Row 1 (frames 4-7): ferns, shrubs, taller plants
// dominant = how many leading frames to pick the clump dominant from.
// NOTE: frames 4-7 (ferns/shrubs) are intentionally excluded — grass cards only.
struct FramePalette { uint8_t f[8]; int dominant; int n; };

[[nodiscard]] FramePalette framePalette(VegDensity vdc) noexcept
{
    if (vdc == VegDensity::kFull)
        return {{ 0, 1, 2, 3, 0, 1, 2, 3 }, 4, 4};  // grass only, all 4 variants
    // kSparse: short dry grass only
    return {{ 0, 1, 2, 0, 1, 2, 0, 1 }, 3, 3};
}

// Classify a terrain cell by its actual material type code from getTerrain().
// This is the authoritative classification — it uses what the engine actually
// assigned to each tile, not what color the colormap happens to render.
//
// TerrainType codes (dmapdata.h):
//   0  MC_BLUEWATER_TYPE   → kHard (water)
//   1  MC_GREEN_WATER_TYPE → kHard (water)
//   2  MC_MUD_TYPE         → kFull (moist ground, vegetation OK)
//   3  MC_MOSS_TYPE        → kFull
//   4  MC_DIRT_TYPE        → kSparse
//   5  MC_ASH_TYPE         → kSparse (barren but not paved)
//   6  MC_MOUNTAIN_TYPE    → kSparse (rocky slopes, sparse)
//   7  MC_TUNDRA_TYPE      → kFull (tundra grass)
//   8  MC_FORESTFLOOR_TYPE → kFull
//   9  MC_GRASS_TYPE       → kFull
//  10  MC_CONCRETE_TYPE    → kHard (paved surface, no vegetation)
//  11  MC_CLIFF_TYPE       → kHard (sheer rock face)
//  12  MC_SLIMY_TYPE       → kSparse (damp, sparse growth OK)
//  13  MC_CEMENT2_TYPE     → kHard
//  14  MC_CEMENT3_TYPE     → kHard
//  15  MC_CEMENT4_TYPE     → kHard
//  16  MC_CEMENT5_TYPE     → kHard
//  17  MC_CEMENT6_TYPE     → kHard
//  18  MC_CEMENT7_TYPE     → kHard
//  19  MC_CEMENT8_TYPE     → kHard
//  20  MC_NONE_TYPE        → kHard (unassigned)
[[nodiscard]] VegDensity classifyTerrainType(int terrainCode) noexcept {
    switch (terrainCode) {
        // Water — handled by elevation gate too, belt-and-suspenders.
        case MC_BLUEWATER_TYPE:
        case MC_GREEN_WATER_TYPE:
        // Paved / engineered surfaces — absolutely no vegetation.
        case MC_CONCRETE_TYPE:
        case MC_CLIFF_TYPE:
        case MC_CEMENT2_TYPE:
        case MC_CEMENT3_TYPE:
        case MC_CEMENT4_TYPE:
        case MC_CEMENT5_TYPE:
        case MC_CEMENT6_TYPE:
        case MC_CEMENT7_TYPE:
        case MC_CEMENT8_TYPE:
        case MC_NONE_TYPE:
            return VegDensity::kHard;

        // Transitional / sparse ground.
        case MC_DIRT_TYPE:
        case MC_ASH_TYPE:
        case MC_MOUNTAIN_TYPE:
        case MC_SLIMY_TYPE:
            return VegDensity::kSparse;

        // Vegetated surfaces.
        case MC_MUD_TYPE:
        case MC_MOSS_TYPE:
        case MC_TUNDRA_TYPE:
        case MC_FORESTFLOOR_TYPE:
        case MC_GRASS_TYPE:
        default:
            // Default kFull for any unknown code (future terrain types should be green).
            return VegDensity::kFull;
    }
}

// Bilinear-interpolated hash on a coarse grid → smooth spatial blobs.
// Returns [0,1].  Used as secondary variation WITHIN terrain-type zones;
// terrain type is still the primary gate.
[[nodiscard]] float macroZone(float wx, float wy, float cellSize = 512.0f) noexcept
{
    const float gx = wx / cellSize;
    const float gy = wy / cellSize;
    const int cx = static_cast<int>(gx >= 0.0f ? gx : gx - 1.0f);
    const int cy = static_cast<int>(gy >= 0.0f ? gy : gy - 1.0f);
    float fx = gx - static_cast<float>(cx);
    float fy = gy - static_cast<float>(cy);
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    const float h00 = hashf(static_cast<float>(cx    ), static_cast<float>(cy    ));
    const float h10 = hashf(static_cast<float>(cx + 1), static_cast<float>(cy    ));
    const float h01 = hashf(static_cast<float>(cx    ), static_cast<float>(cy + 1));
    const float h11 = hashf(static_cast<float>(cx + 1), static_cast<float>(cy + 1));
    return h00*(1-fx)*(1-fy) + h10*fx*(1-fy) + h01*(1-fx)*fy + h11*fx*fy;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Adapter implementation
// ---------------------------------------------------------------------------

void GameAdapters::Vegetation::init()
{
    GosVegetation::init();
}

void GameAdapters::Vegetation::shutdown()
{
    GosVegetation::shutdown();
}

void GameAdapters::Vegetation::missionUnloaded()
{
    // Clear GPU instance list.
    GosVegetation::uploadInstances(nullptr, 0);
}

void GameAdapters::Vegetation::missionLoaded(Terrain* land, MissionMap* gameMap)
{
    if (!land) return;
    if (!GosVegetation::isEnabled()) return;
    GosVegetation::init();  // no-op if already initialized

    const int density = mc2::gate::get<"MC2_VEGETATION_DENSITY">();
    const int maxInst = mc2::gate::get<"MC2_VEGETATION_MAX">();

    // Terrain map is square.  mapTopLeft3d is the top-left (max-Y, min-X)
    // corner in world space.  worldUnitsMapSide is the total span.
    // X runs from mapTopLeft3d.x rightward; Y decreases downward (row).
    const float originX = Terrain::mapTopLeft3d.x;
    const float originY = Terrain::mapTopLeft3d.y;   // max world-Y
    const float mapSpan = Terrain::worldUnitsMapSide;

    std::vector<GosVegetation::Instance> instances;
    instances.reserve(static_cast<size_t>(maxInst));

    // Classification uses getTerrain() tile type codes — no pre-built map needed.
    // This is always available as long as mapData is initialized (which it is at
    // missionLoaded time). No fallback required.

    // Placement counters — logged + emitted as VEG_HEALTH JSONL at end.
    int diagCellsTotal = 0;
    int diagOOB = 0, diagHard = 0, diagNotGreen = 0, diagOverlay = 0, diagWater = 0;
    // Per-instance rejection counters (inside the clump loop).
    int instTriedTotal = 0, instRejWater = 0, instRejHard = 0, instRejNoneFringe = 0,
        instRejOverlay = 0, instRejSlope = 0;

    // Walk the map in world-space steps of kGridStep.
    // wy decreases (southward) because MissionMap row increases as Y decreases.
    for (float wy = originY - kGridStep * 0.5f;
         wy > (originY - mapSpan + kGridStep);
         wy -= kGridStep)
    {
        if (static_cast<int>(instances.size()) >= maxInst) break;

        for (float wx = originX + kGridStep * 0.5f;
             wx < (originX + mapSpan - kGridStep);
             wx += kGridStep)
        {
            if (static_cast<int>(instances.size()) >= maxInst) break;

            // worldToTileCell: tile indices for Terrain::getTerrain (vertex grid).
            // worldToCell: absolute cell indices for MissionMap (cell grid = 3x finer).
            // Both return row/col in the same spatial convention; use the right
            // index type for each API.
            ++diagCellsTotal;

            const Stuff::Vector3D samplePos(wx, wy, 0.0f);
            int tileR = 0, tileC = 0, subR = 0, subC = 0;
            land->worldToTileCell(samplePos, tileR, tileC, subR, subC);
            const long mapSide = static_cast<long>(Terrain::realVerticesMapSide);
            if (tileR < 0 || tileC < 0 || tileR >= mapSide || tileC >= mapSide) { ++diagOOB; continue; }

            // Classify by actual terrain material type (getTerrain tile code).
            // Cement/concrete/water/cliff → kHard (rejected).
            // Dirt/ash/mountain/slimy → kSparse. Grass/moss/mud/tundra/forest → kFull.
            const int terrainCode = land->getTerrain(static_cast<long>(tileR),
                                                     static_cast<long>(tileC));
            const VegDensity vdc = classifyTerrainType(terrainCode);
            if (vdc == VegDensity::kHard) { ++diagHard; continue; }

            (void)gameMap;

            // Water rejection at grid centre — counted by diagWater.
            // Do NOT remove until placement_health event confirms:
            //   cells_total > 0 AND reject_water < cells_total AND instances > 0
            {
                const float sampleElev = land->getTerrainElevation(samplePos);
                if (sampleElev <= Terrain::waterElevation) { ++diagWater; continue; }
            }

            // macroZone [0,1] — secondary variation WITHIN terrain-type zones.
            // Large blob (512 WU) gives the overall patchy coverage.
            // Fine blob (80 WU) perturbs the threshold to roughen straight edges.
            const float zone      = macroZone(wx, wy);
            const float zoneEdge  = macroZone(wx, wy, 80.0f);  // fine-scale edge roughness
            // kFull:   base threshold 0.20, perturbed ±0.08 by fine blob → 0.12..0.28
            // kSparse: base threshold 0.10, perturbed ±0.06 by fine blob → 0.04..0.16
            const float zoneThresh = (vdc == VegDensity::kFull)
                                   ? 0.20f + (zoneEdge - 0.5f) * 0.16f
                                   : 0.10f + (zoneEdge - 0.5f) * 0.12f;
            if (zone < zoneThresh) { ++diagNotGreen; continue; }
            const float zoneDepth = (zone - zoneThresh) / (1.0f - zoneThresh);

            // Cards per clump: kFull = 6..density/2 (6..12), kSparse = 2..5.
            const int clumpMax = (vdc == VegDensity::kFull) ? density / 2 : 5;
            const int clumpMin = (vdc == VegDensity::kFull) ? 6 : 2;
            const int count = clumpMin + static_cast<int>(zoneDepth * static_cast<float>(clumpMax - clumpMin));

            const float mapHalf = mapSpan * 0.5f;

            // Jitter clump centre by half grid step — breaks visible 32 WU rows.
            const float jx = (hashf(wx * 0.017f, wy * 0.013f + 1234.0f) - 0.5f) * kGridStep;
            const float jy = (hashf(wy * 0.019f, wx * 0.023f + 5678.0f) - 0.5f) * kGridStep;
            const float clumpCx = wx + jx;
            const float clumpCy = wy + jy;

            for (int i = 0; i < count; ++i) {
                if (static_cast<int>(instances.size()) >= maxInst) break;

                const float fi = static_cast<float>(i);

                const float h1 = hashf(clumpCx + fi * 73.1f,  clumpCy + fi *  7.3f);
                const float h2 = hashf(clumpCy + fi * 83.7f,  clumpCx + fi *  3.1f);
                const float h3 = hashf(clumpCx + fi * 199.1f, clumpCy + fi * 17.3f);
                const float h4 = hashf(clumpCy + fi * 151.3f, clumpCx + fi * 11.7f);
                const float h5 = hashf(clumpCx + fi * 37.9f,  clumpCy + fi * 59.1f);
                const float h6 = hashf(clumpCy + fi * 61.3f,  clumpCx + fi * 43.1f);
                const float h7 = hashf(clumpCx + fi * 113.7f, clumpCy + fi * 89.3f);

                const float instWx = clumpCx + (h1 - 0.5f) * 20.0f;
                const float instWy = clumpCy + (h2 - 0.5f) * 20.0f;

                ++instTriedTotal;
                const Stuff::Vector3D instPos(instWx, instWy, 0.0f);
                const float elevation = land->getTerrainElevation(instPos);
                if (elevation <= Terrain::waterElevation) { ++instRejWater; continue; }

                // Per-instance terrain type classification.
                // kHard (cement/concrete/water/cliff): always reject — no fringe.
                // kNone is not returned by classifyTerrainType (kHard covers hard-excludes).
                {
                    int iInstR = 0, iInstC = 0, iInstSR = 0, iInstSC = 0;
                    land->worldToTileCell(instPos, iInstR, iInstC, iInstSR, iInstSC);
                    if (iInstR >= 0 && iInstC >= 0 &&
                        iInstR < static_cast<int>(mapSide) && iInstC < static_cast<int>(mapSide)) {
                        const int instCode = land->getTerrain(static_cast<long>(iInstR),
                                                              static_cast<long>(iInstC));
                        const VegDensity instVdc = classifyTerrainType(instCode);
                        if (instVdc == VegDensity::kHard) { ++instRejHard; continue; }
                    }
                }
                // Reject paved roads and bridges via overlay code.
                // ROUGH (detail normal) and DIRT_ROAD are allowed; paved/bridges are not.
                {
                    int iTR = 0, iTC = 0, iSR = 0, iSC = 0;
                    land->worldToTileCell(instPos, iTR, iTC, iSR, iSC);
                    if (iTR >= 0 && iTC >= 0 &&
                        iTR < static_cast<int>(mapSide) && iTC < static_cast<int>(mapSide)) {
                        Overlays ovType = INVALID_OVERLAY;
                        DWORD ovOffset = 0;
                        land->getOverlay(static_cast<long>(iTR), static_cast<long>(iTC), ovType, ovOffset);
                        if (ovType != INVALID_OVERLAY && ovType != ROUGH && ovType != DIRT_ROAD)
                            { ++instRejOverlay; continue; }
                    }
                }

                // Slope gate: getTerrainAngle() returns DEGREES.
                // Hard limit 60°: cliff faces above this never get vegetation.
                // Soft fade zone 45°..60°: reject with linearly increasing
                // probability so the edge is organic rather than a sharp line.
                // Position-based hash (deterministic, no rand()) for the fade.
                {
                    Stuff::Vector3D slopeNormal;
                    const float angle = land->getTerrainAngle(instPos, &slopeNormal);
                    constexpr float kSlopeHard  = 60.0f;   // hard reject above this
                    constexpr float kSlopeSoft  = 45.0f;   // fade begins here
                    if (angle >= kSlopeHard)
                        { ++instRejSlope; continue; }
                    if (angle > kSlopeSoft) {
                        // t in [0,1]: 0 at soft start, 1 at hard limit
                        const float t = (angle - kSlopeSoft) / (kSlopeHard - kSlopeSoft);
                        const float h8 = hashf(instWx * 97.3f, instWy * 137.9f);
                        if (h8 < t)
                            { ++instRejSlope; continue; }
                    }
                }

                GosVegetation::Instance inst;
                inst.x          = instWx - originX - mapHalf;
                inst.y          = instWy - originY + mapHalf;
                inst.z          = elevation + 0.5f;
                inst.yaw        = h3 * 6.2831f;
                inst.scale      = 1.0f + h4 * 5.0f;  // 1-6 WU range
                // Species: dominant frame for this clump (70%), fill from palette (30%).
                const FramePalette pal = framePalette(vdc);
                const float domHash = hashf(clumpCx * 0.11f, clumpCy * 0.09f + 9999.0f);
                const uint8_t domFrame = pal.f[static_cast<int>(domHash * static_cast<float>(pal.dominant)) % pal.dominant];
                const uint32_t frame = (h5 < 0.70f)
                    ? static_cast<uint32_t>(domFrame)
                    : static_cast<uint32_t>(pal.f[static_cast<int>(h5 * static_cast<float>(pal.n)) % pal.n]);
                // Card role (bits 4-5): 0=vertical 40%, 1=tilted 25%, 2=top 15%, 3=ground 20%.
                const uint32_t cardRole = (h7 < 0.40f) ? 0u : (h7 < 0.65f) ? 1u : (h7 < 0.80f) ? 2u : 3u;
                inst.atlasFrame = (cardRole << 4u) | (frame & 0x0Fu);
                inst.seed       = h6;

                instances.push_back(inst);
            }
        }
    }

    GosVegetation::uploadInstances(instances.data(),
                                   static_cast<uint32_t>(instances.size()));

    const unsigned instCount = static_cast<unsigned>(instances.size());

    // Always log: needed to diagnose zero-instance cases without re-running.
    MC2_DIAGFE(stderr,
        "[VEG v3] placement_health cells_total=%d oob=%d terrain_type_reject=%d "
        "water=%d density=%d overlay=%d instances=%u max=%d mapSpan=%.0f waterElev=%.2f\n",
        diagCellsTotal, diagOOB, diagHard,
        diagWater, diagNotGreen, diagOverlay,
        instCount, maxInst, static_cast<double>(mapSpan),
        static_cast<double>(Terrain::waterElevation));
    fflush(stderr);

    // Structured VEG_HEALTH JSONL placement event (MCP-queryable).
    {
        char dataJson[512];
        mc2::fmt::to_buf(dataJson, "{{\"event\":\"placement_health\",\"cells_total\":{:d},\"reject_oob\":{:d},\"reject_terrain_type\":{:d},\"reject_water\":{:d},\"reject_density\":{:d},\"reject_overlay\":{:d},\"instances_generated\":{:d},\"max_instances\":{:d},\"classifier\":\"terrain_type\"}}", mc2::fmt::as_i32(diagCellsTotal), mc2::fmt::as_i32(diagOOB), mc2::fmt::as_i32(diagHard), mc2::fmt::as_i32(diagWater), mc2::fmt::as_i32(diagNotGreen), mc2::fmt::as_i32(diagOverlay), mc2::fmt::as_u32(instCount), mc2::fmt::as_i32(maxInst));
        mc2_diag::writeEvent("VEGETATION", 1, 0, dataJson);
    }

    // Sample elevation range to understand water/terrain relationship.
    float elevMin = 1e30f, elevMax = -1e30f;
    {
        const float stepX = mapSpan / 40.0f;
        const float stepY = mapSpan / 40.0f;
        for (int si = 0; si < 40; ++si) {
            for (int sj = 0; sj < 40; ++sj) {
                const float sx = originX + stepX * (si + 0.5f);
                const float sy = originY - stepY * (sj + 0.5f);
                const Stuff::Vector3D sp(sx, sy, 0.0f);
                const float e = land->getTerrainElevation(sp);
                if (e < elevMin) elevMin = e;
                if (e > elevMax) elevMax = e;
            }
        }
    }

    // Also write to a file so we can read it without console access.
    if (FILE* diagF = fopen("veg_diag.txt", "w")) {
        fprintf(diagF,
            "[VEG v3] placement_health cells_total=%d oob=%d terrain_type_reject=%d "
            "water=%d density=%d overlay=%d instances=%u max=%d mapSpan=%.0f "
            "waterElev=%.2f elevRange=%.2f..%.2f\n",
            diagCellsTotal, diagOOB, diagHard,
            diagWater, diagNotGreen, diagOverlay,
            instCount, maxInst, static_cast<double>(mapSpan),
            static_cast<double>(Terrain::waterElevation),
            static_cast<double>(elevMin), static_cast<double>(elevMax));
        fprintf(diagF,
            "[VEG v3] per_inst tried=%d rej_water=%d rej_hard=%d "
            "rej_none_fringe=%d rej_overlay=%d rej_slope=%d passed=%u\n",
            instTriedTotal, instRejWater, instRejHard,
            instRejNoneFringe, instRejOverlay, instRejSlope, instCount);
        // 5x5 terrain-type classification grid (rows: N..S, cols: W..E).
        fprintf(diagF, "[VEG v3] terrain_type_class_grid 5x5 (row=N..S col=W..E): F=full S=sparse H=hard\n");
        for (int gr = 0; gr < 5; ++gr) {
            const float gy = originY - mapSpan * ((gr + 0.5f) / 5.0f);
            for (int gc = 0; gc < 5; ++gc) {
                const float gx = originX + mapSpan * ((gc + 0.5f) / 5.0f);
                const float ge = land->getTerrainElevation(Stuff::Vector3D(gx, gy, 0.0f));
                int gTR = 0, gTC = 0, gSR = 0, gSC = 0;
                land->worldToTileCell(Stuff::Vector3D(gx, gy, 0.0f), gTR, gTC, gSR, gSC);
                const long gMapSide = static_cast<long>(Terrain::realVerticesMapSide);
                VegDensity gvd = VegDensity::kHard;
                if (gTR >= 0 && gTC >= 0 && gTR < gMapSide && gTC < gMapSide) {
                    gvd = classifyTerrainType(land->getTerrain(gTR, gTC));
                }
                const char cls = (gvd == VegDensity::kFull) ? 'F'
                               : (gvd == VegDensity::kSparse) ? 'S' : 'H';
                fprintf(diagF, "  [%d,%d] class=%c elev=%.1f\n", gr, gc, cls,
                        static_cast<double>(ge));
            }
        }
        fclose(diagF);
    }
}

void GameAdapters::Vegetation::flush(const float* terrainLightDir_4f, float time,
                                     float camChunkX, float camChunkY, float camChunkZ)
{
    if (!GosVegetation::isEnabled()) return;

    // Build per-block LOD visibility from terrain chunk metadata and upload to SSBO.
    // The SSBO tells the vertex shader which blocks to cull (0) or draw at LOD1 (1) or LOD0 (2).
    const int chunkSide  = Terrain::s_terrainChunkSide;
    const int blockCount = chunkSide * chunkSide;
    int diagLod0 = 0, diagLod1 = 0, diagCulled = 0;
    if (blockCount > 0 && Terrain::s_blockMeta) {
        static thread_local std::vector<uint32_t> s_visBuffer;
        s_visBuffer.resize(static_cast<size_t>(blockCount));
        for (int i = 0; i < blockCount; ++i) {
            // Never cull to 0u — inFrustum toggles on edge blocks each frame causing
            // single-frame flashes.  GPU clips off-screen verts automatically.
            const int lod = static_cast<int>(Terrain::s_blockMeta[i].lodLevel);
            s_visBuffer[i] = (lod <= 1) ? 2u : 1u;
            if (s_visBuffer[i] == 2u) ++diagLod0;
            else if (s_visBuffer[i] == 1u) ++diagLod1;
            else ++diagCulled;
        }
        GosVegetation::setBlockVisibility(s_visBuffer.data(),
                                          static_cast<uint32_t>(blockCount));
    }

    // Emit first-frame draw_health event (MCP-queryable).
    static int s_flushCallCount = 0;
    if (++s_flushCallCount == 1 && mc2_diag::tagEnabled("VEGETATION")) {
        char dataJson[512];
        mc2::fmt::to_buf(dataJson, "{{\"event\":\"draw_health\",\"flush_called\":true,\"instances\":{:d},\"terrain_blocks\":{:d},\"lod0_blocks\":{:d},\"lod1_blocks\":{:d},\"culled_blocks\":{:d}}}", mc2::fmt::as_u32(GosVegetation::instanceCount()), mc2::fmt::as_i32(blockCount), mc2::fmt::as_i32(diagLod0), mc2::fmt::as_i32(diagLod1), mc2::fmt::as_i32(diagCulled));
        mc2_diag::writeEvent("VEGETATION", 1, 0, dataJson);
    }

    GosVegetation::flush(
        terrainLightDir_4f[0], terrainLightDir_4f[1],
        terrainLightDir_4f[2], terrainLightDir_4f[3],
        time,
        camChunkX, camChunkY, camChunkZ,
        Terrain::worldUnitsMapSide * 0.5f,
        Terrain::worldUnitsBlockSide,
        chunkSide);
}