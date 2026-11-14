// GameOS/gameos/gos_terrain_water_stream.h
//
// Water-stream parallel to gos_terrain_patch_stream — but for the renderWater
// architectural slice (CPU→GPU offload), where the population is the small
// subset of terrain quads with at least one vertex below `Terrain::waterElevation`
// (the alpha-band trigger). Mission-static; no live-game write paths exist
// (verified 2026-04-30 — see specs/2026-04-29-renderwater-fastpath-design.md
// "Verified mutation invariant").
//
// Stage 1 (this header): CPU-side recipe + load-time population. No GL bindings,
// no shader, no draw. Pure data structure that future stages upload to a GPU SSBO.
//
// Schema is intentionally NOT shared with TerrainQuadRecord — water and solid
// terrain populations are disjoint (water quads frequently have terrainHandle==0;
// terrain quads frequently have no vertices below waterElevation), and the
// shaders read different fields. Co-indexing forces padding-pad in both schemas.

#pragma once
#include <optional>

#include <cstdint>
#include <cstddef>


namespace WaterStream {

// One record per water-bearing quad in a mission. Built once on the first
// Terrain::update after mapData->makeLists populates quadList; persists for
// mission lifetime.
//
// std430-aligned. 64 bytes/record. mc2_01 measured at 7,995 records = 512 KB.
//
// Schema completeness audit (Stage 1 refinement, 2026-04-30): every per-vertex
// field that drawWater() reads from `vertices[i]` or `vertices[i]->pVertex`
// AND is mission-stable lives in this record. Per-frame fields (lightRGB,
// fogRGB, projected wx/wy/wz/ww) live in the per-frame thin SSBO (Stage 2).
//
//   drawWater() reads:                        | source                           | in recipe?
//   ------------------------------------------|----------------------------------|-----------
//   vertices[i]->vx, vy                        | mission-stable (PostcompVertex)  | YES (v*x/v*y)
//   vertices[i]->pVertex->elevation            | mission-stable (PostcompVertex)  | YES (v*e)
//   vertices[i]->pVertex->terrainType          | mission-stable (PostcompVertex)  | YES (terrainTypes)
//   vertices[i]->pVertex->water (bits 0/6/7)   | mission-stable (calcWater)       | YES (waterBits)
//   vertices[i]->pVertex->selected             | runtime debug, ignored           | NO
//   vertices[i]->wx, wy, wz, ww                | per-frame projected              | shader recomputes
//   vertices[i]->lightRGB, fogRGB              | per-frame                        | per-frame SSBO (Stage 2)
//   q.uvMode                                   | mission-stable                   | YES (flags bit 0)
//   q.waterHandle, waterDetailHandle           | per-frame texture-slot animation | global uniform (Stage 2)
struct alignas(16) WaterRecipe {
    // Per-vertex raw world XY (matches vertices[i]->vx, ->vy). 32 B.
    float v0x, v0y;
    float v1x, v1y;
    float v2x, v2y;
    float v3x, v3y;
    // Per-vertex elevation (used for alpha-band classification in VS). 16 B.
    float v0e, v1e, v2e, v3e;
    // Quad index inside Terrain::quadList (debug + parity correlation). 4 B.
    uint32_t quadIdx;
    // bit 0: uvMode (0 = BOTTOMRIGHT, 1 = BOTTOMLEFT)
    // bit 1: hasDetail (terrainTextures2 has a valid water-detail texture pool)
    uint32_t flags;
    // Per-vertex terrainType (uchar each, packed v0..v3 into low to high byte).
    // Fed through terrainTypeToMaterial() in the VS to populate fogRGB low byte
    // — preserves legacy `(frgb & 0xFFFFFF00) | terrainTypeToMaterial(...)` arithmetic.
    uint32_t terrainTypes;
    // Per-vertex water bits (uchar each, packed v0..v3). bit 0 set → underwater
    // (this quad's classification reason); bits 6/7 modulate the per-vertex wave
    // displacement applied in setupTextures' water-projection block at
    // quad.cpp:689-700: when bit 7 set → -frameCosAlpha; bit 6 set → +frameCosAlpha;
    // neither → 0. The fast-path VS reproduces that displacement to keep wave-bob.
    uint32_t waterBits;
};
static_assert(sizeof(WaterRecipe) == 64,
              "WaterRecipe must be 64 bytes for std430 alignment");

constexpr uint32_t kFlagBitUvModeBottomLeft = 0x1u;
constexpr uint32_t kFlagBitHasDetail        = 0x2u;

// WATER-GPU-FULL-RECIPE-CULL-1B: true when authoritative full-recipe GPU cull is
// enabled (MC2_WATER_GPU_FULL_RECIPE_AUTHORITATIVE, default-ON). The native
// GPU culls the whole world-indexed recipe set
// directly into the draw-feeding thin buffer.
bool IsFullRecipeAuthoritative();

// Build the recipe array from the live Terrain::quadList. Walks all
// numberQuads entries; flags water-bearing quads (≥1 vertex below
// waterElevation). Idempotent — calling twice rebuilds.
//
// MC2_WATER_STREAM_DEBUG=1 prints `[WATER_STREAM v1] event=build_done
// recipes=N candidates_walked=M waterElevation=F has_terrainTextures2=B`
// at completion.
void Build();

// Reset to empty state (mission unload).
void Reset();

// Read-only access to the recipe array. Pointer is stable across the
// mission lifetime (vector capacity is fixed after Build).
const WaterRecipe* GetRecipes();
uint32_t GetRecipeCount();

// True after Build has succeeded; false after Reset or before first Build.
bool IsReady();

// Returns the WaterRecipe for a quad whose corner-0 vertexNum is vn.
// Returns nullptr if vn has no water recipe (pure-land quad or vn < 0).
// Only valid after IsReady() returns true.
const WaterRecipe* RecipeForVertexNum(int32_t vn);

// SHORELINE-STAIRCASE-1 (MC2_WATER_SHORE_FEATHER): the water-edge silhouette
// snaps to the terrain tile grid because the shore alpha ramp (shoreBlend =
// max(alphaDepth,1) in the water FS) is only ~alphaDepth world-units wide —
// a 1-2 px transition at top-down altitudes that reads as a hard tile step.
// This gate sets a FLOOR (world units) on the effective alphaDepth used by
// BOTH the FS shore smoothstep (fed via the water bridge, no gameos_graphics
// edit) AND the beach-ring shoreExt geometry (which is exactly alphaDepth*0.5,
// the band's land-side extent — the two stay matched). A wider band dissolves
// the staircase into a soft gradient that HUGS the rendered waterline by
// construction (the FS band is driven by per-fragment WaterThickness =
// waterElevation - interpolated terrain z), so it auto-widens horizontally on
// flat shores (worst staircase) and stays tight on cliffs. Read once, cached.
//   ShoreFeatherFloorWu() : the gate value in wu (0 if unset/<=0 -> disabled).
//   EffAlphaDepth(base)   : max(base, floor) when the gate is on, else base
//                           (default OFF -> returns base -> byte-identical).
float ShoreFeatherFloorWu();
float EffAlphaDepth(float baseAlphaDepth);
float NavigableShallowDepth();

// Shared tile-admission extent for water recipes and native culling.
float EffShoreExt(float baseAlphaDepth);

// STEP 3.4P: the per-frame scalars the cull dispatch uses, captured at the same point so the native Vulkan cull/pack (vk_native_producers) reads
// identical values. Valid for the frame only when the full-recipe authoritative path ran its guards and the native producer is active.
struct VkNativeInputs {
    float    mvp[16];
    float    frameCos;
    float    waterElevation;
    float    shoreExt;
    uint32_t mapSide;
    int32_t  biasLegacy;
    uint32_t recipeCount;
};
bool GetVkNativeInputs(VkNativeInputs& out);
// Current-camera, conservative water visibility. Static chunk bounds only;
// no GPU readback, previous-frame visibility, or reflection history.
bool NativeWaterInFrustum(const VkNativeInputs& frame, bool shoreFieldActive, float* ndcBounds = nullptr);
// Load-time: the field-coverage tables NativeWaterInFrustum builds on its first call (about 100 ms on a shore-field map), built behind the loading
// screen instead. Same inputs as the frame path, so the first frame finds them current. No-op when the shore field is not usable.
void PrewarmShoreFieldCoverage();

// Readiness of the native input source, usable before the water pass captures
// its per-frame MVP/scalars. Does not read a previous frame's captured snapshot.
bool NativeInputSourceReady();
// Mission-static allocation bounds, available before the first lighting pack.
struct NativeCapacity { uint32_t vertices, eligibleBound; };
std::optional<NativeCapacity> GetNativeCapacity();

// Captures the per-frame native-producer scalars at the water pass (the GL compute dispatch, the CPU thin pack, the GL draw and the readback
// feed are gone). False = no water this frame.
bool CaptureVkNativeInputs(float frameCos);

} // namespace WaterStream
