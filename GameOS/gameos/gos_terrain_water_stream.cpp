// GameOS/gameos/gos_terrain_water_stream.cpp
//
// Native water input source: mission-static recipes plus per-frame scalars.
// The Vulkan producer culls the recipe population and emits thin records.
// No CPU candidate walk or GL upload remains.
//
// Why the indirection: `Terrain::quadList` is rebuilt every frame as a
// camera-relative window (mapdata.cpp:1072 makeLists), so a "static recipe
// indexed by quadList slot" is invalid by frame 2. The MAP coordinates
// (mapX, mapY) of each vertex are stable; the recipe is keyed by the
// top-left vertex's `vertexNum = mapY*W + mapX` (set at mapdata.cpp:1104).
//
// Spec: docs/superpowers/specs/2026-04-29-renderwater-fastpath-design.md.

#include "gos_terrain_water_stream.h"
#include "../../mclib/terrain_material.h"  // single-sourced terrain type -> material
extern int gos_WaterShoreMode();
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1

#include "gos_profiler.h"
#include "gpu_driven_common.h"
#include "gos_static_prop_killswitch.h"  // gos_GetTerrainMVPMat4()
#include "gos_terrain_indirect.h"        // IsFrameSolidArmed()
#include <cassert>

// WATER-BIAS-LINEARIZE-1 (gameos.hpp): FAST-water bias mode (0 = proportional
// default, 1 = MC2_WATER_BIAS_LEGACY). The pzOk cull gate must match the VS.
extern int mc2WaterBiasLegacy();

#include <vector>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>

#include "../../mclib/terrain.h"
#include "../../mclib/quad.h"
#include "../../mclib/vertex.h"
#include "../../mclib/mapdata.h"

#include "Gates/gate.h"
#include "mc2fmt.h"

// [WATER_DEPTHPROBE v1] cross-TU Probe-8 fingerprint accessors (terrain-solid
// side). Paired per-frame against the water-upload MVP fingerprint to
// discriminate a RUNTIME temporal MVP divergence (fps differ under camera
// motion) from a static depth-derivation/fudge mismatch (fps stay equal).
extern "C" uint32_t gos_terrain_indirect_getDispatchMvpFp();
extern "C" uint64_t gos_terrain_indirect_getDispatchMvpFrameIdx();
// Water-consistency fix (2026-05-17): full MVP terrain-solid baked its Fix-B
// clipPos with this frame. Used only when IsFrameSolidArmed() (then it is
// this-frame-fresh) so the drawn water matches the drawn terrain exactly.
extern "C" const float* gos_terrain_indirect_getDispatchMvp16();

namespace WaterStream {

// SHORELINE-STAIRCASE-1: MC2_WATER_SHORE_FEATHER floor on effective alphaDepth
// (world units). Widens the water FS shore smoothstep band + the beach-ring
// shoreExt geometry in lockstep so the tile-quantized water-edge staircase
// dissolves into a soft, waterline-hugging alpha gradient. Read once; default
// unset/<=0 -> returns 0 -> EffAlphaDepth is the identity -> byte-identical.
float ShoreFeatherFloorWu() {
    static const float s_floor = mc2::gate::get<"MC2_WATER_SHORE_FEATHER">();
    return s_floor;
}

float EffAlphaDepth(float baseAlphaDepth) {
    const float floorWu = ShoreFeatherFloorWu();
    return floorWu > 0.0f ? fmaxf(baseAlphaDepth, floorWu) : baseAlphaDepth;
}

float EffShoreExt(float baseAlphaDepth) {
    const float effAD = EffAlphaDepth(baseAlphaDepth);
    const float base  = (effAD * 0.5f > 0.0f) ? effAD * 0.5f : 15.0f;
    float ext = base;
    // MC2_WATER_SHORE_MODE=3 (GL prototype): widen admission so geometry exists
    // beyond the true sidecar contour for the per-pixel discard to carve.
    if (gos_WaterShoreMode() == 3)
        ext += 60.0f;
    return ext;
}

namespace {

std::vector<WaterRecipe> g_recipes;
// vertexNum (= mapY*realVerticesMapSide + mapX of top-left corner) → recipeIdx
std::unordered_map<uint32_t, uint32_t> g_vertexNumToRecipe;
bool g_ready = false;

bool s_debugEnabledKnown = false;
bool s_debugEnabled = false;
bool DebugOn() {
    if (!s_debugEnabledKnown) {
        s_debugEnabled = mc2::gate::get<"MC2_WATER_STREAM_DEBUG">();
        s_debugEnabledKnown = true;
    }
    return s_debugEnabled;
}

// STEP 3.4P: native Vulkan cull inputs (see VkNativeInputs).
VkNativeInputs g_vkNative{};
bool           g_vkNativeValid = false;

// Single-sourced in mclib/terrain_material.h (was a local copy missing type 20).
inline uint8_t terrainTypeToMaterialLocal(uint32_t terrainType) {
    return mc2terrain::terrainTypeToMaterial(terrainType);
}

} // namespace

void Reset() {
    g_recipes.clear();
    g_recipes.shrink_to_fit();
    g_vertexNumToRecipe.clear();
    g_ready = false;
}

// Default-on native full-recipe GPU cull. Disabling it produces no water;
// the retired CPU candidate path is not a fallback.
bool IsFullRecipeAuthoritative() {
    static bool s_known = false, s_val = false;
    if (!s_known) {
        // DEFAULT-ON since 2026-06-03 (1A byte-identical parity across tier1 +
        // user pan-across-water-edge + visual soak): only literal "0" opts out
        // (revert escape hatch); any other value INCLUDING UNSET opts in.
        s_val = mc2::gate::get<"MC2_WATER_GPU_FULL_RECIPE_AUTHORITATIVE">();
        s_known = true;
    }
    return s_val;
}

void Build() {
    g_recipes.clear();
    g_vertexNumToRecipe.clear();
    g_ready = false;

    // Source of truth: MapData::blocks (PostcompVertex array, mission-static,
    // dimensioned realVerticesMapSide × realVerticesMapSide). Iterate the
    // FULL MAP and build one WaterRecipe per water-bearing quad. World
    // coordinates derived from map indices via mapdata.cpp:1123-1124.
    if (!Terrain::mapData) {
        if (DebugOn()) {
            mc2::fmt::print_err("[WATER_STREAM v1] event=build_skipped reason=no_mapdata\n");
            fflush(stderr);
        }
        return;
    }

    const PostcompVertexPtr blocks = Terrain::mapData->getBlocks();
    if (!blocks) {
        if (DebugOn()) {
            mc2::fmt::print_err("[WATER_STREAM v1] event=build_skipped reason=no_blocks\n");
            fflush(stderr);
        }
        return;
    }

    const long mapSide  = Terrain::realVerticesMapSide;
    const long halfSide = Terrain::halfVerticesMapSide;
    const float wupv    = Terrain::worldUnitsPerVertex;
    const bool has_terrainTextures2 = (Terrain::terrainTextures2 != nullptr);

    auto blockAt = [blocks, mapSide](long mx, long my) -> const PostcompVertex& {
        return blocks[mx + my * mapSide];
    };
    auto worldX = [halfSide, wupv](long mx) -> float {
        return float(mx - halfSide) * wupv;
    };
    auto worldY = [halfSide, wupv](long my) -> float {
        return float(halfSide - my) * wupv;
    };

    g_recipes.reserve(static_cast<size_t>((mapSide * mapSide / 8)));  // rough upper bound
    g_vertexNumToRecipe.reserve(static_cast<size_t>((mapSide * mapSide / 8)));

    long candidates = 0;

    // Quads span (mx, my)..(mx+1, my+1) so the outer loop runs to mapSide-1.
    // The corner ordering matches mapdata.cpp:1175-ish setup of TerrainQuad:
    //   v0 = (mx,   my)     top-left
    //   v1 = (mx+1, my)     top-right
    //   v2 = (mx+1, my+1)   bottom-right
    //   v3 = (mx,   my+1)   bottom-left
    for (long my = 0; my < mapSide - 1; ++my) {
        for (long mx = 0; mx < mapSide - 1; ++mx) {
            ++candidates;
            const PostcompVertex& p0 = blockAt(mx,     my);
            const PostcompVertex& p1 = blockAt(mx + 1, my);
            const PostcompVertex& p2 = blockAt(mx + 1, my + 1);
            const PostcompVertex& p3 = blockAt(mx,     my + 1);

            // Include EVERY map quad in the recipe (no water-bit filter).
            //
            // Why: legacy `setupTextures` sets `waterHandle != 0xffffffff` for
            // any quad where the WATER PLANE PROJECTS INTO THE FRUSTUM (per
            // the `clipped1||clipped2` gate at quad.cpp:963). That's a
            // dynamic, camera-driven criterion — independent of whether the
            // quad has any per-vertex `water & 1` bit set. On flat-but-large
            // missions like mc2_03 / mc2_10 there exist pure-land quads above
            // the water plane where the water plane still projects on-screen
            // and legacy emits semi-transparent (alphaEdge) water vertices.
            //
            // The Stage 2 build used `water & 1` as a recipe-inclusion gate,
            // which fired `lookup_miss` parity mismatches on those missions
            // because `UploadAndBindThinRecords` would silently skip quads
            // with no recipe (mc2_03: 7,131 misses/run; mc2_10: 12,888).
            //
            // Including every map quad lifts recipe SSBO size ~10× (mc2_01
            // 8K → ~80K records, 0.5 → ~5 MB) but keeps the per-frame thin
            // record count unchanged — pz-gate culls non-visible quads at
            // draw time exactly as before. Memory growth is bounded by
            // mapSide² ; well within budget.
            (void)p0; (void)p1; (void)p2; (void)p3;

            const float v0x = worldX(mx);
            const float v0y = worldY(my);
            const float v1x = worldX(mx + 1);
            const float v1y = worldY(my);
            const float v2x = worldX(mx + 1);
            const float v2y = worldY(my + 1);
            const float v3x = worldX(mx);
            const float v3y = worldY(my + 1);

            // The runtime quad uvMode depends on the (mx, my) parity per
            // mapdata.cpp:115 — `((tileR & 1) == (tileC & 1)) ? BOTTOMRIGHT : BOTTOMLEFT`.
            // tileR = my, tileC = mx for our convention.
            const bool isBottomLeft = ((my & 1L) != (mx & 1L));

            WaterRecipe r{};
            r.v0x = v0x; r.v0y = v0y;
            r.v1x = v1x; r.v1y = v1y;
            r.v2x = v2x; r.v2y = v2y;
            r.v3x = v3x; r.v3y = v3y;
            r.v0e = p0.elevation;
            r.v1e = p1.elevation;
            r.v2e = p2.elevation;
            r.v3e = p3.elevation;
            r.quadIdx = static_cast<uint32_t>((mx + my * mapSide));  // top-left vertexNum

            uint32_t flags = 0;
            if (isBottomLeft)         flags |= kFlagBitUvModeBottomLeft;
            if (has_terrainTextures2) flags |= kFlagBitHasDetail;
            r.flags = flags;

            const uint32_t t0 = static_cast<uint32_t>(static_cast<uint8_t>(p0.terrainType));
            const uint32_t t1 = static_cast<uint32_t>(static_cast<uint8_t>(p1.terrainType));
            const uint32_t t2 = static_cast<uint32_t>(static_cast<uint8_t>(p2.terrainType));
            const uint32_t t3 = static_cast<uint32_t>(static_cast<uint8_t>(p3.terrainType));
            r.terrainTypes = t0 | (t1 << 8) | (t2 << 16) | (t3 << 24);

            const uint32_t w0 = static_cast<uint32_t>((p0.water & 0xFFu));
            const uint32_t w1 = static_cast<uint32_t>((p1.water & 0xFFu));
            const uint32_t w2 = static_cast<uint32_t>((p2.water & 0xFFu));
            const uint32_t w3 = static_cast<uint32_t>((p3.water & 0xFFu));
            r.waterBits = w0 | (w1 << 8) | (w2 << 16) | (w3 << 24);

            const uint32_t recipeIdx = static_cast<uint32_t>(g_recipes.size());
            g_recipes.push_back(r);
            g_vertexNumToRecipe.emplace(static_cast<uint32_t>((mx + my * mapSide)), recipeIdx);
        }
    }

    g_ready = true;

    // [WATER_MAT v1] positive-marker probe (env MC2_WATER_MATERIAL_PROBE; SEPARATE
    // from the retained [WATER_DEPTHPROBE v2] MVP instrument - do not share its env).
    // Recomputes the VS thickness formula CPU-side over the populated recipes so a
    // smoke can assert the elevation path is live (max > 0), not a flat unbound read.
    {
        static const bool s_waterMatProbe = mc2::gate::get<"MC2_WATER_MATERIAL_PROBE">();
        if (s_waterMatProbe && !g_recipes.empty()) {
            float tmin = 1e30f, tmax = -1e30f;
            for (const WaterRecipe& r : g_recipes) {
                float floorMin = r.v0e;
                floorMin = (r.v1e < floorMin) ? r.v1e : floorMin;
                floorMin = (r.v2e < floorMin) ? r.v2e : floorMin;
                floorMin = (r.v3e < floorMin) ? r.v3e : floorMin;
                float thick = static_cast<float>(Terrain::waterElevation) - floorMin;
                if (thick < 0.0f) thick = 0.0f;
                if (thick < tmin) tmin = thick;
                if (thick > tmax) tmax = thick;
            }
            mc2::fmt::print_err("[WATER_MAT v1] event=summary recipes={:d} thickness_min={:.3f} thickness_max={:.3f} waterElevation={:.3f}\n", mc2::fmt::as_size(g_recipes.size()), static_cast<double>(tmin), static_cast<double>(tmax), static_cast<double>(Terrain::waterElevation));
            fflush(stderr);
        }
    }

    if (DebugOn()) {
        mc2::fmt::print_err("[WATER_STREAM v1] event=build_done recipes={:d} candidates_walked={:d} waterElevation={:.3f} has_terrainTextures2={:d} mapSide={:d} halfSide={:d} wupv={:.3f}\n", mc2::fmt::as_size(g_recipes.size()), mc2::fmt::as_i32(candidates), static_cast<double>(Terrain::waterElevation), mc2::fmt::as_i32(has_terrainTextures2 ? 1 : 0), mc2::fmt::as_i32(mapSide), mc2::fmt::as_i32(halfSide), static_cast<double>(wupv));
        // Dump several recipes spread across the map to spot uniformity bugs.
        const size_t n = g_recipes.size();
        const size_t dumps[] = { 0, n/8, n/4, n/2, n*3/4, n-1 };
        for (size_t di = 0; di < sizeof(dumps)/sizeof(dumps[0]); ++di) {
            if (dumps[di] >= n) continue;
            const WaterRecipe& r = g_recipes[dumps[di]];
            const bool uniform_elev = (r.v0e == r.v1e && r.v1e == r.v2e && r.v2e == r.v3e);
            mc2::fmt::print_err("[WATER_STREAM v1] event=recipe[{:d}] elev=({:.1f},{:.1f},{:.1f},{:.1f}) {} v0=({:.0f},{:.0f}) v3=({:.0f},{:.0f}) quadIdx={:d}\n", mc2::fmt::as_size(dumps[di]), static_cast<double>(r.v0e), static_cast<double>(r.v1e), static_cast<double>(r.v2e), static_cast<double>(r.v3e), mc2::fmt::safe(uniform_elev ? "[UNIFORM]" : "[VARIED]"), static_cast<double>(r.v0x), static_cast<double>(r.v0y), static_cast<double>(r.v3x), static_cast<double>(r.v3y), mc2::fmt::as_u32(r.quadIdx));
        }
        fflush(stderr);
    }
}

const WaterRecipe* GetRecipes() {
    return g_recipes.empty() ? nullptr : g_recipes.data();
}

uint32_t GetRecipeCount() {
    return static_cast<uint32_t>(g_recipes.size());
}

bool IsReady() {
    return g_ready;
}

const WaterRecipe* RecipeForVertexNum(int32_t vn) {
    if (vn < 0) return nullptr;
    auto it = g_vertexNumToRecipe.find(static_cast<uint32_t>(vn));
    if (it == g_vertexNumToRecipe.end()) return nullptr;
    assert(it->second < static_cast<uint32_t>(g_recipes.size())); // invariant: Build populates both atomically
    return &g_recipes[it->second];
}

bool NativeInputSourceReady() {
#ifdef MC2_VULKAN_ISLAND
    return gpu_driven::IsWaterEnabled() && g_ready && !g_recipes.empty() && IsFullRecipeAuthoritative();
#else
    return false;
#endif
}

std::optional<NativeCapacity> GetNativeCapacity() {
    if (!NativeInputSourceReady()) return std::nullopt;
    const uint32_t side = static_cast<uint32_t>(Terrain::realVerticesMapSide);
    const float threshold = Terrain::waterElevation + EffShoreExt(MapData::alphaDepth) + 64.0f;
    uint32_t n = 0;
    for (const auto& r : g_recipes)
        if ((r.waterBits & 0x01010101u) != 0u || r.v0e < threshold || r.v1e < threshold || r.v2e < threshold || r.v3e < threshold) ++n;
    return NativeCapacity{side * side, n + n / 8u + 4096u};
}

// STEP 3.4P: the native Vulkan cull/pack produces the thin records on the GPU (the GL compute dispatch, the CPU thin pack, the GL thin ring,
// the GL indirect draw and the GL readback feed were deleted in s62). This captures, once per frame at the water pass, the scalars the native
// producers read. False = no water this frame (MC2_GPU_DRIVEN_WATER=0 or MC2_WATER_GPU_FULL_RECIPE_AUTHORITATIVE=0 kill it, no recipes, no MVP).
bool CaptureVkNativeInputs(float frameCos) {
    g_vkNativeValid = false;
#ifdef MC2_VULKAN_ISLAND
    if (!NativeInputSourceReady()) return false;
    const float* nmvp = gos_terrain_indirect::IsFrameSolidArmed() ? gos_terrain_indirect_getDispatchMvp16() : gos_GetTerrainMVPMat4();
    if (!nmvp) nmvp = gos_GetTerrainMVPMat4();
    if (!nmvp) return false;
    std::memcpy(g_vkNative.mvp, nmvp, sizeof(g_vkNative.mvp));
    g_vkNative.frameCos = frameCos;
    g_vkNative.waterElevation = Terrain::waterElevation;
    g_vkNative.shoreExt = EffShoreExt(MapData::alphaDepth);
    g_vkNative.mapSide = static_cast<uint32_t>(Terrain::realVerticesMapSide);
    g_vkNative.biasLegacy = mc2WaterBiasLegacy();
    g_vkNative.recipeCount = static_cast<uint32_t>(g_recipes.size());
    g_vkNativeValid = true;
    return true;
#else
    (void)frameCos;
    return false;
#endif
}

bool GetVkNativeInputs(VkNativeInputs& out) {
    if (!g_vkNativeValid) return false;
    out = g_vkNative;
    return true;
}

} // namespace WaterStream
