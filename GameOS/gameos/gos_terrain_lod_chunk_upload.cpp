// gos_terrain_lod_chunk: heightfield, visual-damp, control-map, overlay/shoreline sidecar and cement-word uploads, and the terrain light/ambient accessors.
// Declared in the class headers; shared names in gos_terrain_lod_chunk_internal.h.
#include <algorithm>
#include "gos_terrain_lod_chunk_internal.h"
#include "mc2fmt.h"
#include "Gates/render_options.h"
#include "terrain_field_store.h"

// ---------------------------------------------------------------------------
// Full heightfield upload — called once at map load.
// elevations: float[mapSide*mapSide] row-major.
// ---------------------------------------------------------------------------

void gos_TerrainLodChunk_UploadHeightFull(const float* elevations, int mapSide)
{
    auto* store = gos_TerrainFieldStore();
    if (!store || !store->setHeight(elevations, mapSide)) return;
    s_mapSide = mapSide;
    s_halfMap = static_cast<float>(mapSide) * 128.0f * 0.5f;
}

void gos_TerrainLodChunk_UploadVisualHeightFull(const float* visualHeights, int V,
                                                const float* mipMaxes, int mipFloats)
{
    if (!mipMaxes) mipFloats = 0;
    auto* store = gos_TerrainFieldStore();
    if (!store || !store->setVisualHeight(visualHeights, V, mipMaxes, mipFloats)) return;
    s_visualSide = V; s_visualMipFloats = mipFloats;
}

void gos_TerrainLodChunk_ClearVisualHeight()
{
    if (auto* store = gos_TerrainFieldStore()) store->clearVisualHeight();
    s_visualSide = s_visualMipFloats = s_visualDampSide = 0;
    s_visualDampStatic.clear(); s_visualDampCombined.clear();
    s_visualDampHadMovers = false;
}

// TERRAIN-REAUTH-UNPIN-1 Half B: static (buildings) object-proximity damp map.
// Reached only when the upstream gates passed (mclib/terrain.cpp). Keeps a CPU
// copy so per-frame mover stamps can min-combine without re-reading the GPU.
void gos_TerrainLodChunk_UploadVisualDampStatic(const float* damp01, int side)
{
    if (!damp01 || side <= 0) return;
    const size_t count = static_cast<size_t>(side) * static_cast<size_t>(side);
    s_visualDampStatic.assign(damp01, damp01 + count);
    s_visualDampCombined = s_visualDampStatic;
    s_visualDampSide = side;
    s_visualDampHadMovers = false;
}

bool gos_TerrainLodChunk_VisualDampWanted()
{
    static const bool s_displaceGate = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE">();
    static const bool s_dampGate = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE">();
    return s_displaceGate && s_dampGate
           && s_visualDampSide > 0 && !s_visualDampStatic.empty();
}

// Per-frame mover stamps. combined = min(static, smoothstep(inner, inner+radius,
// dist)) per mover; one full-buffer BufferSubData (side^2 floats, ~57KB@120 —
// negligible). Zero-mover frames upload only on the first one after movers
// disappear (restores the pure-static map, then no-ops).
void gos_TerrainLodChunk_UpdateVisualDampMovers(const float* cellXY, int count,
                                                float radiusCells, float innerCells)
{
    if (s_visualDampStatic.empty() || s_visualDampSide <= 0)
        return;
    if (count <= 0 && !s_visualDampHadMovers)
        return;   // steady state: the static map is already current
    const int N = s_visualDampSide;
    s_visualDampCombined = s_visualDampStatic;
    if (radiusCells < 0.25f) radiusCells = 0.25f;
    if (innerCells < 0.0f) innerCells = 0.0f;
    const float reach = innerCells + radiusCells;
    for (int m = 0; m < count; ++m)
    {
        const float cx = cellXY[m * 2 + 0];
        const float cy = cellXY[m * 2 + 1];
        int x0 = static_cast<int>(floorf(cx - reach)), x1 = static_cast<int>(ceilf(cx + reach));
        int y0 = static_cast<int>(floorf(cy - reach)), y1 = static_cast<int>(ceilf(cy + reach));
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
        if (x1 > N - 1) x1 = N - 1; if (y1 > N - 1) y1 = N - 1;
        for (int iy = y0; iy <= y1; ++iy)
            for (int ix = x0; ix <= x1; ++ix)
            {
                const float dx = static_cast<float>(ix) - cx;
                const float dy = static_cast<float>(iy) - cy;
                float t = (sqrtf(dx * dx + dy * dy) - innerCells) / radiusCells;
                if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
                const float s = t * t * (3.0f - 2.0f * t);
                float& ref = s_visualDampCombined[static_cast<size_t>(ix) + static_cast<size_t>(iy) * N];
                if (s < ref) ref = s;
            }
    }
    s_visualDampHadMovers = (count > 0);

}

// Step 5b: per-vertex terrainType upload (parallel to the heightfield). Used by
// the chunk frag's concrete material/colour selection (pureConcrete).
void gos_TerrainLodChunk_UploadTerrainTypeFull(const float* types, int mapSide)
{
    if (auto* store = gos_TerrainFieldStore()) (void)store->setTerrainTypes(types, mapSide);
}

void gos_TerrainLodChunk_UploadControlMap(const unsigned char* rgba, int side)
{
    if (auto* store = gos_TerrainFieldStore()) (void)store->setControlMap(rgba, side);
}

void gos_TerrainLodChunk_UploadOverlaySidecar(const unsigned char* rgba, int w, int h,
                                               float boundsTopLeftX, float boundsTopLeftY,
                                               float boundsSizeX, float boundsSizeY)
{
    auto* store = gos_TerrainFieldStore();
    if (!store || !store->setOverlay(rgba, w, h)) return;
    s_overlayBounds[0]=boundsTopLeftX; s_overlayBounds[1]=boundsTopLeftY;
    s_overlayBounds[2]=boundsSizeX; s_overlayBounds[3]=boundsSizeY;
}

void gos_TerrainLodChunk_UploadShorelineMask(const unsigned char* rgba, int w, int h,
                                              float boundsTopLeftX, float boundsTopLeftY,
                                              float boundsSizeX, float boundsSizeY)
{
    auto* store = gos_TerrainFieldStore();
    if (!store || !store->setShorelineMask(rgba, w, h)) return;
    s_shorelineBounds[0]=boundsTopLeftX; s_shorelineBounds[1]=boundsTopLeftY;
    s_shorelineBounds[2]=boundsSizeX; s_shorelineBounds[3]=boundsSizeY;
}

void gos_TerrainLodChunk_UploadWaterShoreHeight(const float* heights, int w, int h,
                                                 float boundsTopLeftX, float boundsTopLeftY,
                                                 float boundsSizeX, float boundsSizeY)
{
    auto* store = gos_TerrainFieldStore();
    if (!store || !store->setWaterShoreHeight(heights, w, h)) return;
    s_waterShoreHeightBounds[0]=boundsTopLeftX; s_waterShoreHeightBounds[1]=boundsTopLeftY;
    s_waterShoreHeightBounds[2]=boundsSizeX; s_waterShoreHeightBounds[3]=boundsSizeY;
}

void gos_TerrainLodChunk_UploadWaterShoreSdf(const float* heights, int w, int h,
                                              float boundsTopLeftX, float boundsTopLeftY,
                                              float boundsSizeX, float boundsSizeY)
{
    // Upload the caller's texels (never the store's own buffer: setWaterShoreHeight may
    // reallocate it), then re-tag the stored copy as a real shore SDF.
    gos_TerrainLodChunk_UploadWaterShoreHeight(heights, w, h, boundsTopLeftX, boundsTopLeftY,
                                               boundsSizeX, boundsSizeY);
    if (mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore())
        store->setWaterShoreHeight(heights, w, h, mc2terrain::WaterShoreHeightKind::ShoreSdf);
}

// WATER-SHORE-EDGE-1: single engine-wide resolution of the water shore-edge
// mode (see header). Same lazy-once idiom as gos_TerrainShorelineGateOn so the
// loader (mclib/terrain.cpp) and the draw path (gameos_graphics.cpp) can never
// disagree about what mode a stock launch runs.
int gos_WaterShoreMode()
{
    static const int s_mode = []() {
        // Enum row 0|1|2|3: anything else (unset, empty, unparseable) falls through to default resolution
        if (mc2::gate::is<"MC2_WATER_SHORE_MODE", "0">()) return 0;
        if (mc2::gate::is<"MC2_WATER_SHORE_MODE", "1">()) return 1;
        if (mc2::gate::is<"MC2_WATER_SHORE_MODE", "2">()) return 2;
        if (mc2::gate::is<"MC2_WATER_SHORE_MODE", "3">()) return 3;
        // Back-compat killswitch: MC2_WATER_SHORE_MASK=0 predates the mode
        // gate and meant "full legacy ramp"; keep that meaning.
        const bool shoreMaskOn = mc2::gate::get<"MC2_WATER_SHORE_MASK">();  // default ON
        // SHORE-FIELD-1: default is mode 3 -- edge AND depth tint from the smooth field
        // (cooked shore_field.r16f, else visual_height_4x; no sidecar -> the FS falls back to 2/1/0).
        return shoreMaskOn ? 3 : 0;
    }();
    return s_mode;
}

// TERRAIN-SHORELINE-V3: true whenever the elevation-based shoreline band is
// active (MC2_TERRAIN_SHORELINE gate on) -- NOT dependent on a mask sidecar
// being loaded, since V3 places the band by elevation unconditionally (the
// mask, if present, is only an optional modulator). Consumed by
// gos_postprocess.cpp to suppress the legacy screen runShoreline() pass
// (recon landmine #6 -- avoid double-brightening the seam). Name kept for
// caller-site stability; semantics widened from "mask uploaded" to "gate on".
bool gos_TerrainLodChunk_IsShorelineMaskActive()
{

    return gos_TerrainShorelineGateOn();
}

bool gos_TerrainShorelineGateOn()
{
    static const bool s_on = mc2::gate::get<"MC2_TERRAIN_SHORELINE">();
    return s_on;
}

bool gos_TerrainShorelineLegacySuppressOn()
{
    static const bool s_on = mc2::options::current().shorelineLegacySuppress;
    return s_on;
}

// Step 5c: per-vertex cement word upload (valid bit | atlas layer index). Built by
// gos_terrain_indirect after the cement catalog atlas is ready (PopulateRecipeCementWords).
void gos_TerrainLodChunk_UploadCementWordsFull(const unsigned int* words, int count, int mapSide)
{
    if (mapSide <= 0) return;
    if (auto* store = gos_TerrainFieldStore()) (void)store->setCementWords(words, count);
}

void gos_TerrainLodChunk_UploadHeightPatch(
    const float* rowData,
    int originX, int originY,
    int quadCountX, int quadCountY,
    int mapSide)
{
    if (auto* store = gos_TerrainFieldStore())
        (void)store->patchHeight(rowData, originX, originY, quadCountX, quadCountY, mapSide);
}

void gos_GetTerrainChunkGridInfo(int* mapSide, float* halfMap)
{
    if (mapSide) *mapSide = s_mapSide;
    if (halfMap) *halfMap = s_halfMap;
}

float gos_TerrainSnowBrightnessDampen()
{
    static const float s_v = mc2::gate::get<"MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN">();
    return s_v;
}
int gos_TerrainLightPoolCount()
{
    uint32_t count=0;
    const auto* pool = gos_terrain_lighting::GetLightPoolCpu(&count);
    return pool ? static_cast<int>((std::min)(count, 256u)) : 0;
}

void gos_TerrainPadGlowRGB(float out[3])
{
    // Compatibility UBO slots remain neutral; occupied buildings own spill now.
    out[0] = out[1] = out[2] = 0.0f;
}

// VK-PARITY-TERRAIN-2: the NIGHT-LIGHT-POOLS-2..4 / MOVER-CONTACT-SHADOW-1 knob
// constants (anonymous-namespace kTglc*) for the VK island.
void gos_TerrainLightPoolKnobs(int* soft, float* moverDim, float* moverScale,
                               int* moverMax, int* compress, float* contactShadow)
{
    if (soft)          *soft          = kTglcLightPoolSoft ? 1 : 0;
    if (moverDim)      *moverDim      = kTglcLightPoolMoverDim;
    if (moverScale)    *moverScale    = kTglcLightPoolMoverRadius;
    if (moverMax)      *moverMax      = kTglcLightPoolMoverMax ? 1 : 0;
    if (compress)      *compress      = kTglcLightPoolCompress ? 1 : 0;
    if (contactShadow) *contactShadow = kTglcMoverContactShadow;
}

float gos_TerrainPadGlowSpillWu()
{
    static const float s_v = 48.0f;
    return s_v;
}
