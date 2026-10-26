// LOD-chunk terrain: shared height/type/cement inputs, Init and Destroy and the
// shared sampler/uniform plumbing. Draw, upload, VK feed, patch cache, material albedo and the tuning caches live in
// gos_terrain_lod_chunk_{draw,upload,feed,patch,matalbedo,gates}.cpp; what they share is gos_terrain_lod_chunk_internal.h.

#include "gos_terrain_lod_chunk_internal.h"
#include "mc2fmt.h"
#include "terrain_field_store.h"

#include <memory>

namespace {

std::unique_ptr<mc2terrain::TerrainFieldStore> s_fieldStore;
}

mc2terrain::TerrainFieldStore* gos_TerrainFieldStore() { return s_fieldStore.get(); }

int    s_visualSide       = 0; // V = (mapSide-1)*4+1, fine grid side (0 = bake not loaded)

int    s_visualMipFloats  = 0;

int s_visualDampSide = 0;                 // == mapSide when loaded
std::vector<float> s_visualDampStatic;    // load-time (buildings)
std::vector<float> s_visualDampCombined;  // static + mover stamps
bool s_visualDampHadMovers = false;       // skip redundant re-uploads
int    s_mapSide    = 0;   // mapSide stored at last UploadHeightFull
float  s_halfMap    = 0.0f;// (mapSide * 128.0 * 0.5)

float  s_overlayBounds[4]  = {0.0f, 0.0f, 0.0f, 0.0f};  // topLeftX,topLeftY(=maxY),sizeX,sizeY

// TERRAIN-OVERLAY-V2-DECAL-SUPPRESS-1: read-only accessors (see header). Let the
// indirect decal bake key off the SAME resolved sidecar-loaded state the frag
// samples (not a re-read of the env), so suppression matches what actually draws.
bool         gos_TerrainLodChunk_OverlaySidecarLoaded() { return s_fieldStore && !s_fieldStore->overlay().texels.empty(); }
const float* gos_TerrainLodChunk_OverlayBounds()        { return s_overlayBounds; }

float  s_shorelineBounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // topLeftX,topLeftY(=maxY),sizeX,sizeY

// WATER-SHORE-MASK-1: water-shader accessors (see header).
const float* gos_TerrainLodChunk_ShorelineMaskBounds()  { return s_shorelineBounds; }

// WATER-SHORE-EDGE-1: hi-res R32F visual-height texture for the water FS
// per-fragment shore crossing. Same single-owner pattern as the mask above.
float  s_waterShoreHeightBounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};

const float* gos_TerrainLodChunk_WaterShoreHeightBounds() { return s_waterShoreHeightBounds; }

const float* mc2_chunkDetailAntiTileYZW()
{
    static float s_yzw[3] = { 0.0f, 0.0f, 0.0f };
    static bool s_init = false;
    if (!s_init) {
        s_init = true;
        if (mc2::gate::get<"MC2_TERRAIN_DETAIL_ANTITILE">()) {
            s_yzw[0] = mc2::gate::get<"MC2_TERRAIN_DETAIL_LOD1_STRENGTH">();    // .y LOD1 fade (row default 0.4)
            s_yzw[1] = 1.0f;                                                    // .z enable
            s_yzw[2] = mc2::gate::get<"MC2_TERRAIN_DETAIL_MACRO_STRENGTH">();   // .w macro (chunk: off, row default 0)
        }
    }
    return s_yzw;
}

// ---------------------------------------------------------------------------
// Init / Destroy — called from gosRenderer::init / gosRenderer::destroy.
// ---------------------------------------------------------------------------

void gos_TerrainLodChunk_Init()
{
    s_fieldStore = std::make_unique<mc2terrain::TerrainFieldStore>();
}

void gos_TerrainLodChunk_Destroy()
{
    s_fieldStore.reset();
    s_mapSide = s_visualSide = s_visualMipFloats = s_visualDampSide = 0;
    s_halfMap = 0.0f;
    s_visualDampStatic.clear(); s_visualDampCombined.clear();
    s_visualDampHadMovers = false;
    std::fill_n(s_overlayBounds, 4, 0.0f);
    std::fill_n(s_shorelineBounds, 4, 0.0f);
    std::fill_n(s_waterShoreHeightBounds, 4, 0.0f);
}
