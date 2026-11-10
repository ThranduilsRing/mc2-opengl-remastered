// GameAdapters/VegetationAdapter.cpp
//
// Vegetation card placement scan and per-frame flush.
// This is a GameAdapters bridge TU: it may reach both game-side headers
// (mclib/) and engine-side headers (GameOS/gameos/).

#include "VegetationAdapter.h"
#include "vegetation_budget.h"
#include "vegetation_noise.h"
#include "vegetation_clumps.h"
#include "vegetation_hardsurface.h"
#include "mc2_diag_verbose.h"
#include "Gates/gate.h"

// Engine side.
#include "gos_vegetation.h"

// Game side -- terrain geometry + type queries.
#include "../mclib/terrain.h"
#include "../mclib/terrain_runtime.h"
// Game side -- terrain type codes (TerrainType enum).
#include "../mclib/dmapdata.h"
// Game side -- MissionMap overlay / water queries.
#include "../mclib/move.h"

// Stuff::Matrix4D (entries[16], column-major) + Stuff::Vector3D.
// Full chain required (matrix.hpp alone creates circular include ordering).
#include "stuff/stuff.hpp"

#include "../../GameOS/gameos/diagnostic_trace.h"

#include <vector>
#include <chrono>
#include <algorithm> // std::max
#include <cstdlib>   // std::getenv, std::atoi
#include <cstdio>    // fprintf, snprintf
#include <cmath>     // sinf, fmodf, fabsf
#include <fstream>
#include "mc2fmt.h"

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// VEG-CEMENT-1: sub-cell hard-surface coverage from the decal/overlay bake
// (GameOS/gameos/gos_terrain_indirect_mine_decal.cpp; C-linkage forwarders).
extern float gos_terrain_indirect_HardSurfaceCoverage(float wx, float wy);
extern void  gos_terrain_indirect_HardSurfaceStats(int* alpha, int* pure, int* noImage);

namespace {
using mc2::vegetation::hashf;
using mc2::vegetation::macroZone;

// World-unit spacing between sample points.
// ~5x area density vs 32: (32/14)^2 ≈ 5.2x more grid cells per area.
constexpr float kGridStep = 14.0f;

// ---------------------------------------------------------------------------
// Placement accept/reject helpers
// ---------------------------------------------------------------------------

// Vegetation density tier from terrain material type (getTerrain codes).
// kHard   = absolute no-vegetation: water, cliff, none
// kNone   = hard exclude (kept for 15% fringe logic at borders)
// kSparse = transitional surfaces: dirt, ash, mountain, slimy
// kFull   = vegetated surfaces: grass, moss, forest floor, mud, tundra
enum class VegDensity : int { kHard = -1, kNone = 0, kSparse = 1, kFull = 2 };

// Classify a terrain cell by its actual material type code from getTerrain().
// This is the authoritative classification — it uses what the engine actually
// assigned to each tile, not what color the colormap happens to render.
//
// Material groups are explicit below; numeric terrain IDs belong to dmapdata.h.
[[nodiscard]] VegDensity classifyTerrainType(int terrainCode) noexcept {
    switch (terrainCode) {
        // Water — handled by elevation gate too, belt-and-suspenders.
        case MC_BLUEWATER_TYPE:
        case MC_GREEN_WATER_TYPE:
        // Paved / engineered surfaces — absolutely no vegetation.
        case MC_CLIFF_TYPE:
        case MC_NONE_TYPE:
            return VegDensity::kHard;

        // VEG-CEMENT-1: cement is no longer a per-cell reject. Its visible edge is the
        // decal bake's alpha, so the sub-cell coverage field decides density (smooth
        // falloff + crack weeds). The cell itself is a sparse stand-in.
        case MC_CONCRETE_TYPE:
        case MC_CEMENT2_TYPE:
        case MC_CEMENT3_TYPE:
        case MC_CEMENT4_TYPE:
        case MC_CEMENT5_TYPE:
        case MC_CEMENT6_TYPE:
        case MC_CEMENT7_TYPE:
        case MC_CEMENT8_TYPE:
            return VegDensity::kSparse;

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
            return VegDensity::kFull;
        default:
            return VegDensity::kSparse; // Unknown/mod materials stay conservative.
    }
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
    const int maxInst = static_cast<int>(GosVegetation::populationCapacity(static_cast<uint32_t>(mc2::gate::get<"MC2_VEGETATION_MAX">()), Terrain::worldUnitsMapSide));
    if (maxInst == 0) return;

    // Terrain map is square.  mapTopLeft3d is the top-left (max-Y, min-X)
    // corner in world space.  worldUnitsMapSide is the total span.
    // X runs from mapTopLeft3d.x rightward; Y decreases downward (row).
    const float originX = Terrain::mapTopLeft3d.x;
    const float originY = Terrain::mapTopLeft3d.y;   // max world-Y
    const float mapSpan = Terrain::worldUnitsMapSide;
    // The clump grid keeps the 5M-era step: the raised cap is the meadow field's, not more shrubs.
    const float gridStep = mc2::vegetation::placementStep(mapSpan, kGridStep * 1.5f,
                                                        (std::max)(density,44), (std::min)(maxInst, 4750000));

    std::vector<GosVegetation::Instance> instances;
    instances.reserve(static_cast<size_t>(maxInst));

    // Classification uses getTerrain() tile type codes — no pre-built map needed.
    // This is always available as long as mapData is initialized (which it is at
    // missionLoaded time). No fallback required.

    // Placement counters — logged + emitted as VEG_HEALTH JSONL at end.
    int diagCellsTotal = 0;
    int diagOOB = 0, diagHard = 0, diagNotGreen = 0, diagOverlay = 0, diagWater = 0;
    int diagWeeds = 0, diagCoverKept = 0;
    const auto hsT0 = std::chrono::steady_clock::now();
    int hsAlpha = 0, hsPure = 0, hsNoImg = 0;
    gos_terrain_indirect_HardSurfaceStats(&hsAlpha, &hsPure, &hsNoImg);   // builds the field once
    const double hsBuildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hsT0).count();
    // Per-instance rejection counters (inside the clump loop).
    int instTriedTotal = 0, instRejWater = 0, instRejHard = 0, instRejNoneFringe = 0,
        instRejOverlay = 0, instRejSlope = 0;

    using namespace mc2::vegetation;
    const uint32_t seed = mapSeed(Terrain::terrainName ? std::string_view(Terrain::terrainName) : std::string_view());
    std::array<ClumpRecipe,4> recipes{{{9,0.30f,0,12},{8,0.25f,-0.15f,8},
                                      {7,0.10f,-0.3f,8},{11,0.4f,0.2f,14}}};
    constexpr std::array<const char*,4> paths{
        "data/vegetation/schemas/grasslands.json", "data/vegetation/schemas/snow.json",
        "data/vegetation/schemas/desert.json", "data/vegetation/schemas/swamp.json"};
    std::optional<FieldRecipe> fieldRecipe;
    int woodyPlaced = 0, woodyGated = 0, clumpsGated = 0, woodyThinned = 0;   // shrub/sapling counts for the owner sheet
    for (size_t i = 0; i < paths.size(); ++i) {
        std::ifstream input(paths[i]);
        if (input) {
            const auto doc = nlohmann::json::parse(input,nullptr,false);
            if (const auto recipe = readClumpRecipe(doc)) recipes[i] = *recipe;
            if (i == 0) fieldRecipe = readFieldRecipe(doc);   // only the temperate biome carpets its ground
        }
    }
    const int side = static_cast<int>(mapSpan/gridStep);
    const long mapSide = static_cast<long>(Terrain::realVerticesMapSide);
    for (int row = 1; row < side-1; ++row) {
        for (int col = 1; col < side-1; ++col) {
            ++diagCellsTotal;
            const auto centre = clumpCandidate(col,row,seed);
            const float wx = originX+centre.x*gridStep;
            const float wy = originY-centre.y*gridStep;
            const bool isCentre = clumpCentreAccepted(col,row,seed);
            // Sparse grass singles occupy gaps between layered clumps.
            const bool single = !isCentre || clumpRoll(col,row,seed,110) < 0.12f;
            if (single && clumpRoll(col,row,seed,111) > 0.10f) { ++diagNotGreen; continue; }
            const glm::vec3 pos(wx,wy,0);
            int tr=0,tc=0,sr=0,sc=0;
            land->worldToTileCell(pos,tr,tc,sr,sc);
            if (tr<0 || tc<0 || tr>=mapSide || tc>=mapSide) { ++diagOOB; continue; }
            const int code = land->getTerrain(tr,tc);
            const auto tier = classifyTerrainType(code);
            if (tier == VegDensity::kHard) { ++diagHard; continue; }
            Biome biome = Biome::Temperate;
            if (code == MC_TUNDRA_TYPE) biome = Biome::Snow;
            else if (code == MC_DIRT_TYPE || code == MC_ASH_TYPE || code == MC_MOUNTAIN_TYPE) biome = Biome::Desert;
            else if (code == MC_MUD_TYPE || code == MC_MOSS_TYPE || code == MC_SLIMY_TYPE) biome = Biome::Swamp;
            // A low-frequency field is a moisture proxy, not a simulation input.
            const float moisture = macroZone(wx+static_cast<float>(seed&4095u),wy,512);
            if (clumpRoll(col,row,seed,112) > (tier == VegDensity::kFull ? 0.9f : 0.4f)*(0.5f+moisture))
                { ++diagNotGreen; continue; }
            glm::vec3 normal;
            const auto clump = makeClump(col,row,seed,recipes[static_cast<size_t>(biome)],
                                          moisture,land->getTerrainAngle(pos,&normal),single);
            // Where the meadow field carpets the ground, shrubs and saplings gather under
            // and around the stock trees (owner 2026-11-10); open meadow keeps a rare one.
            // Every biome but snow: mc2_01's stock tile codes put most clumps in other biomes.
            int forestCells = 0;
            if (gameMap) {
                int cr=0,cc=0;
                land->worldToCell(pos,cr,cc);
                for (int dr=-3; dr<=3; ++dr) for (int dc=-3; dc<=3; ++dc)
                    if (gameMap->inBounds(cr+dr,cc+dc) && gameMap->cellAt(cr+dr,cc+dc).getForest()) ++forestCells;
            }
            if (fieldRecipe && biome != Biome::Snow && !single && gameMap) {
                if (clumpRoll(col,row,seed,113) > understoryChance(forestCells)) {
                    for (int i=0; i<clump.count; ++i) woodyGated += clump.members[static_cast<size_t>(i)].woody ? 1 : 0;
                    ++clumpsGated; continue;
                }
            }
            for (int i=0; i<clump.count; ++i) {
                if (static_cast<int>(instances.size()) >= maxInst) break;
                const auto& member = clump.members[static_cast<size_t>(i)];
                const float instWx = wx+member.x, instWy = wy+member.y;
                const glm::vec3 instPos(instWx,instWy,0);
                ++instTriedTotal;
                int ir=0,ic=0,isr=0,isc=0;
                land->worldToTileCell(instPos,ir,ic,isr,isc);
                if (ir<0 || ic<0 || ir>=mapSide || ic>=mapSide) { ++instRejHard; continue; }
                if (classifyTerrainType(land->getTerrain(ir,ic)) == VegDensity::kHard)
                    { ++instRejHard; continue; }
                const float elevation = TerrainRuntime::sampleVisualHeight(instPos);   // visual surface (nifty), not the sim grid
                bool occupied = false;
                if (gameMap) {
                    int cr=0,cc=0;
                    land->worldToCell(instPos,cr,cc);
                    // Read-only static masks: never alter movement, LOS or RNG.
                    occupied = !gameMap->inBounds(cr,cc) ||
                        clumpStaticOccupied(gameMap->cellAt(cr,cc), fieldRecipe.has_value() && biome != Biome::Snow);
                }
                const float coverage = gos_terrain_indirect_HardSurfaceCoverage(instWx,instWy);
                const float slope = land->getTerrainAngle(instPos,&normal);
                if (!clumpSurfaceAllowed(true,elevation<=Terrain::waterElevation,occupied,coverage,slope,member.woody)) {
                    if (elevation<=Terrain::waterElevation) ++instRejWater;
                    else if (occupied || coverage>=0.15f) ++instRejOverlay;
                    else ++instRejSlope;
                    continue;
                }
                const auto salt = static_cast<uint32_t>(150+i*3);
                // Clustered shrub thinning (owner: ~10x fewer); grass members are untouched.
                if (member.woody && biome != Biome::Snow) {
                    const float cluster = macroZone(instWx+371.0f+static_cast<float>(seed&511u), instWy-529.0f, 220.0f);
                    if (clumpRoll(col,row,seed,salt+1000) >= shrubKeepChance(forestCells,cluster,slope)) { ++woodyThinned; continue; }
                }
                GosVegetation::Instance inst;
                inst.x = instWx-originX-mapSpan*0.5f;
                inst.y = instWy-originY+mapSpan*0.5f;
                inst.z = elevation+0.5f;
                inst.yaw = clumpRoll(col,row,seed,salt)*6.2831853f;
                inst.scale = member.scale;
                unsigned frame = member.frame;
                // Keep two silhouettes per role; snow favours the conifer.
                if (frame>=6 && biome==Biome::Snow && clumpRoll(col,row,seed,salt+1)<0.8f) frame=7;
                const unsigned role = member.woody ? 0u : (clumpRoll(col,row,seed,salt+1)<0.25f ? 1u : 0u);
                inst.atlasFrame = frame | (role<<4u) | (biome==Biome::Snow ? 128u : 0u);
                inst.seed = clumpRoll(col,row,seed,salt+2);
                instances.push_back(inst);
                woodyPlaced += member.woody ? 1 : 0;
            }
        }
    }

    // Meadow field: a stratified carpet of blades over open temperate ground,
    // after the clumps so its budget is exactly the capacity they left.
    int fieldBlades = 0, fieldSide = 0, fieldGaps = 0, fieldFlowers = 0;
    const auto fieldT0 = std::chrono::steady_clock::now();
    if (fieldRecipe) {
        constexpr float kBeachMarginWU = 8.0f;   // keep the shoreline sand bare
        std::vector<std::pair<int,int>> fieldCells;
        for (int row = 1; row < side-1; ++row) {
            for (int col = 1; col < side-1; ++col) {
                const glm::vec3 pos(originX+(static_cast<float>(col)+0.5f)*gridStep,
                                    originY-(static_cast<float>(row)+0.5f)*gridStep, 0);
                int tr=0,tc=0,sr=0,sc=0;
                land->worldToTileCell(pos,tr,tc,sr,sc);
                if (tr<0 || tc<0 || tr>=mapSide || tc>=mapSide) continue;
                const int code = land->getTerrain(tr,tc);
                // Stock tile codes do not separate meadow from sand or rock (mc2_01 reads
                // "sparse" everywhere); the fragment stage drops blades whose root colour
                // is not vegetation. Snow keeps its clumps only.
                if (classifyTerrainType(code) == VegDensity::kHard || code == MC_TUNDRA_TYPE) continue;
                if (TerrainRuntime::sampleVisualHeight(pos) <= Terrain::waterElevation+kBeachMarginWU) continue;
                fieldCells.emplace_back(col,row);
            }
        }
        const long budget = static_cast<long>(maxInst) - static_cast<long>(instances.size());
        fieldSide = fieldBladeSide(gridStep, fieldRecipe->spacing, budget, static_cast<long>(fieldCells.size()));
        for (const auto& [col,row] : fieldCells) {
            if (fieldSide == 0) break;
            glm::vec3 normal;
            const glm::vec3 centre(originX+(static_cast<float>(col)+0.5f)*gridStep,
                                   originY-(static_cast<float>(row)+0.5f)*gridStep, 0);
            const float slope = land->getTerrainAngle(centre,&normal);
            for (int j = 0; j < fieldSide*fieldSide; ++j) {
                const auto blade = fieldBlade(col,row,seed,j,fieldSide,gridStep,*fieldRecipe);
                const float bx = originX+static_cast<float>(col)*gridStep+blade.x;
                const float by = originY-static_cast<float>(row)*gridStep+blade.y;
                const glm::vec3 bpos(bx,by,0);
                // Owner ref 3 (2026-11-10, gameplay camera) over ref 2: a fine, even, continuous
                // sward. The 14 WU tuft field keeps only small soft dirt gaps (close-up flavour)
                // and a gentle height swell instead of tall tufts.
                const float tuft = macroZone(bx+static_cast<float>(seed&1023u), by, 14.0f);
                const float tuftHeight = fieldTuftHeight(tuft);
                if (tuftHeight <= 0.0f) { ++fieldGaps; continue; }
                const bool flower = macroZone(bx+911.0f, by-377.0f+static_cast<float>(seed&2047u), 45.0f) > 0.74f &&
                                    blade.seed < 0.4f;
                int ir=0,ic=0,isr=0,isc=0;
                land->worldToTileCell(bpos,ir,ic,isr,isc);
                if (ir<0 || ic<0 || ir>=mapSide || ic>=mapSide ||
                    classifyTerrainType(land->getTerrain(ir,ic)) == VegDensity::kHard) continue;
                const float elevation = TerrainRuntime::sampleVisualHeight(bpos);
                bool occupied = false;
                if (gameMap) {
                    int cr=0,cc=0;
                    land->worldToCell(bpos,cr,cc);
                    occupied = !gameMap->inBounds(cr,cc) || clumpStaticOccupied(gameMap->cellAt(cr,cc), true);
                }
                const float coverage = gos_terrain_indirect_HardSurfaceCoverage(bx,by);
                if (!clumpSurfaceAllowed(true,elevation<=Terrain::waterElevation+kBeachMarginWU,
                                         occupied,coverage,slope,false)) continue;
                GosVegetation::Instance inst;
                inst.x = bx-originX-mapSpan*0.5f;
                inst.y = by-originY+mapSpan*0.5f;
                inst.z = elevation+0.25f;
                inst.yaw = blade.yaw;
                inst.scale = blade.scale * (flower ? 0.55f : tuftHeight);   // flowers sit on short stalks
                // role 0: the shader bends field blades downwind; bit 2 = white wildflower heads.
                inst.atlasFrame = blade.frame | (flower ? kFieldFlowerBit : 0u);
                fieldFlowers += flower ? 1 : 0;
                inst.seed = blade.seed;
                instances.push_back(inst);
                ++fieldBlades;
            }
        }
    }
    const double fieldMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fieldT0).count();
    MC2_DIAGFE(stderr,
        "[VEG v3] field blades=%d side=%d gaps=%d flowers=%d ms=%.1f shrubs_placed=%d shrubs_moved_off_meadow=%d clumps_gated=%d shrubs_thinned=%d\n",
        fieldBlades, fieldSide, fieldGaps, fieldFlowers, fieldMs, woodyPlaced, woodyGated, clumpsGated, woodyThinned);

    GosVegetation::uploadInstances(instances.data(),
                                   static_cast<uint32_t>(instances.size()));

    const unsigned instCount = static_cast<unsigned>(instances.size());

    // Always log: needed to diagnose zero-instance cases without re-running.
    MC2_DIAGFE(stderr,
        "[VEG v3] placement_health cells_total=%d oob=%d terrain_type_reject=%d "
        "water=%d density=%d overlay=%d instances=%u max=%d mapSpan=%.0f waterElev=%.2f "
        "hs_reject=%d hs_weeds=%d hs_cover_kept=%d hs_tiles_alpha=%d hs_tiles_pure=%d hs_tiles_noimg=%d hs_build_ms=%.2f\n",
        diagCellsTotal, diagOOB, diagHard,
        diagWater, diagNotGreen, diagOverlay,
        instCount, maxInst, static_cast<double>(mapSpan),
        static_cast<double>(Terrain::waterElevation),
        instRejOverlay, diagWeeds, diagCoverKept, hsAlpha, hsPure, hsNoImg, hsBuildMs);
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
                const glm::vec3 sp(sx, sy, 0.0f);
                const float e = land->getTerrainElevation(sp);
                elevMin = std::min(elevMin, e);
                elevMax = std::max(elevMax, e);
            }
        }
    }

    // Also write to a file so we can read it without console access.
    if (FILE* diagF = fopen("veg_diag.txt", "w")) {
        mc2::fmt::print(diagF, "[VEG v3] placement_health cells_total={:d} oob={:d} terrain_type_reject={:d} water={:d} density={:d} overlay={:d} instances={:d} max={:d} mapSpan={:.0f} waterElev={:.2f} elevRange={:.2f}..{:.2f}\n", mc2::fmt::as_i32(diagCellsTotal), mc2::fmt::as_i32(diagOOB), mc2::fmt::as_i32(diagHard), mc2::fmt::as_i32(diagWater), mc2::fmt::as_i32(diagNotGreen), mc2::fmt::as_i32(diagOverlay), mc2::fmt::as_u32(instCount), mc2::fmt::as_i32(maxInst), static_cast<double>(mapSpan), static_cast<double>(Terrain::waterElevation), static_cast<double>(elevMin), static_cast<double>(elevMax));
        mc2::fmt::print(diagF, "[VEG v3] per_inst tried={:d} rej_water={:d} rej_hard={:d} rej_none_fringe={:d} rej_overlay={:d} rej_slope={:d} passed={:d} hs_weeds={:d} hs_cover_kept={:d} hs_tiles_alpha={:d} hs_tiles_pure={:d} hs_tiles_noimg={:d} hs_build_ms={:.2f}\n", mc2::fmt::as_i32(instTriedTotal), mc2::fmt::as_i32(instRejWater), mc2::fmt::as_i32(instRejHard), mc2::fmt::as_i32(instRejNoneFringe), mc2::fmt::as_i32(instRejOverlay), mc2::fmt::as_i32(instRejSlope), mc2::fmt::as_u32(instCount), mc2::fmt::as_i32(diagWeeds), mc2::fmt::as_i32(diagCoverKept), mc2::fmt::as_i32(hsAlpha), mc2::fmt::as_i32(hsPure), mc2::fmt::as_i32(hsNoImg), hsBuildMs);
        // 5x5 terrain-type classification grid (rows: N..S, cols: W..E).
        mc2::fmt::print(diagF, "[VEG v3] terrain_type_class_grid 5x5 (row=N..S col=W..E): F=full S=sparse H=hard\n");
        for (int gr = 0; gr < 5; ++gr) {
            const float gy = originY - mapSpan * ((gr + 0.5f) / 5.0f);
            for (int gc = 0; gc < 5; ++gc) {
                const float gx = originX + mapSpan * ((gc + 0.5f) / 5.0f);
                const float ge = land->getTerrainElevation(glm::vec3(gx, gy, 0.0f));
                int gTR = 0, gTC = 0, gSR = 0, gSC = 0;
                land->worldToTileCell(glm::vec3(gx, gy, 0.0f), gTR, gTC, gSR, gSC);
                const long gMapSide = static_cast<long>(Terrain::realVerticesMapSide);
                VegDensity gvd = VegDensity::kHard;
                if (gTR >= 0 && gTC >= 0 && gTR < gMapSide && gTC < gMapSide) {
                    gvd = classifyTerrainType(land->getTerrain(gTR, gTC));
                }
                const char cls = (gvd == VegDensity::kFull) ? 'F'
                               : (gvd == VegDensity::kSparse) ? 'S' : 'H';
                mc2::fmt::print(diagF, "  [{:d},{:d}] class={:c} elev={:.1f}\n", mc2::fmt::as_i32(gr), mc2::fmt::as_i32(gc), mc2::fmt::as_char(cls), static_cast<double>(ge));
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
