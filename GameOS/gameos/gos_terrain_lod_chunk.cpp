// LOD-chunk terrain: shared height/type/cement inputs, Init and Destroy and the
// shared sampler/uniform plumbing. Draw, upload, VK feed, patch cache, material albedo and the tuning caches live in
// gos_terrain_lod_chunk_{draw,upload,feed,patch,matalbedo,gates}.cpp; what they share is gos_terrain_lod_chunk_internal.h.


#include "gos_terrain_lod_chunk_internal.h"
#include "mc2fmt.h"
#include "terrain_field_store.h"

#include <memory>

namespace {
// Lives from Init to Destroy, like the SSBOs it feeds.
std::unique_ptr<mc2terrain::TerrainFieldStore> s_fieldStore;
}

mc2terrain::TerrainFieldStore* gos_TerrainFieldStore() { return s_fieldStore.get(); }



RenderCore::GpuBuffer s_heightSsbo{  // GL handle; name() 0 = not yet allocated
    RenderCore::RenderResourceId::TerrainHeightSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainHeightSsbo",
    RenderCore::GpuBufferKind::Storage};
// TERRAIN-VISUAL-HEIGHT-SSBO-OWNER-1 (now RAII, see block above): the 4x visual
// heightfield SSBO (binding 26). Lifetime Mission: uploaded per mission load via
// glBufferData. CREATE is gated upstream (mclib/terrain.cpp: MC2_TERRAIN_VISUAL_HEIGHT
// / _DISPLACE + a visual_height_4x.r32 bake) — when the gate is off / no bake ships,
// the upload entry is never called, name() stays 0, and nothing registers
// (passthrough byte-identical). No stock bake ships → create path is offline-only.
RenderCore::GpuBuffer s_visualHeightSsbo{ // TERRAIN-VISUAL-HEIGHT-SAMPLE-1: 4x visual heightfield (binding 26)
    RenderCore::RenderResourceId::TerrainVisualHeightSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainVisualHeightSsbo",
    RenderCore::GpuBufferKind::Storage};
int    s_visualSide       = 0; // V = (mapSide-1)*4+1, fine grid side (0 = bake not loaded)
// TERRAIN-LOD-GEOMORPH-1: float count of the max-mip levels APPENDED to the
// binding-26 SSBO after the V*V fine bake (5 levels x mapSide^2, strides
// 2/4/5/10/20). 0 = no mips shipped -> u_geomorphMips uploads 0 -> the vert's
// coarse-band branch behaves exactly as S2 (legacy layout untouched).
int    s_visualMipFloats  = 0;
// TERRAIN-REAUTH-UNPIN-1 Half B: coarse object-proximity displacement damp
// (binding 27). Static half = building footprints from the bake sidecar
// (visual_damp.r32), uploaded once per mission load; dynamic half = per-frame
// mover stamps min-combined on the CPU and re-uploaded (side^2 floats — tiny).
// CREATE gated upstream like the visual-height SSBO (displace gate + sidecar);
// glName 0 => never bound, u_visualDampOn stays 0 (passthrough).
RenderCore::GpuBuffer s_visualDampSsbo{
    RenderCore::RenderResourceId::TerrainVisualDampSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainVisualDampSsbo",
    RenderCore::GpuBufferKind::Storage};
int s_visualDampSide = 0;                 // == mapSide when loaded
std::vector<float> s_visualDampStatic;    // load-time (buildings)
std::vector<float> s_visualDampCombined;  // static + mover stamps
bool s_visualDampHadMovers = false;       // skip redundant re-uploads
// TERRAIN-LODCHUNK-SSBO-OWNER-1 (now RAII, see block above): the LOD-chunk
// type/cement SSBOs. Lifetime Mission: (re-)uploaded per mission load via
// glBufferData.
RenderCore::GpuBuffer s_typeSsbo{   // Step 5b: per-vertex terrainType SSBO (binding 24)
    RenderCore::RenderResourceId::TerrainTypeSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainTypeSsbo",
    RenderCore::GpuBufferKind::Storage};
RenderCore::GpuBuffer s_cementSsbo{ // Step 5c: per-vertex cement word SSBO (binding 25)
    RenderCore::RenderResourceId::TerrainCementSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainCementSsbo",
    RenderCore::GpuBufferKind::Storage};
int    s_mapSide    = 0;   // mapSide stored at last UploadHeightFull
float  s_halfMap    = 0.0f;// (mapSide * 128.0 * 0.5)

// TERRAIN-CONTROLMAP-SAMPLE-1: authored RGBA override control-map texture
// (unit 12). Plain GLuint (not GpuBufferOwner) — mirrors gos_terrain_height_tex.cpp's
// self-contained texture handling for a single-owner render-only raster. glName
// 0 = not loaded (gate off or no sidecar) -> u_useControlMap uploads 0 at draw.
GLuint s_controlMapTex  = 0;
int    s_controlMapSide = 0;

// TERRAIN-OVERLAY-V2-PARITY-1: authored cement/pad/runway overlay sidecar
// texture (unit TERRAIN_OVERLAY_SIDECAR_TEXUNIT). Plain GLuint, same
// self-contained single-owner pattern as s_controlMapTex. glName 0 = not
// loaded (gate off or no sidecar) -> u_useOverlaySidecar uploads 0 at draw.
GLuint s_overlaySidecarTex = 0;
float  s_overlayBounds[4]  = {0.0f, 0.0f, 0.0f, 0.0f};  // topLeftX,topLeftY(=maxY),sizeX,sizeY

// TERRAIN-OVERLAY-V2-DECAL-SUPPRESS-1: read-only accessors (see header). Let the
// indirect decal bake key off the SAME resolved sidecar-loaded state the frag
// samples (not a re-read of the env), so suppression matches what actually draws.
bool         gos_TerrainLodChunk_OverlaySidecarLoaded() { return s_overlaySidecarTex != 0; }
const float* gos_TerrainLodChunk_OverlayBounds()        { return s_overlayBounds; }

// TERRAIN-SHORELINE-V3: authored land-side wet/foam shoreline mask sidecar
// texture (unit TERRAIN_SHORELINE_TEXUNIT). Plain GLuint, same self-contained
// single-owner pattern as s_overlaySidecarTex. glName 0 = not loaded (no
// sidecar found) -> u_hasShorelineMask uploads 0 at draw -> the elevation
// band still renders (mask is now an optional modulator, not a requirement);
// u_useShorelineMask (the band's own on/off) is driven directly by the
// MC2_TERRAIN_SHORELINE gate, independent of this texture's state.
GLuint s_shorelineMaskTex   = 0;
float  s_shorelineBounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // topLeftX,topLeftY(=maxY),sizeX,sizeY

// WATER-SHORE-MASK-1: water-shader accessors (see header).
unsigned int gos_TerrainLodChunk_ShorelineMaskTex()     { return s_shorelineMaskTex; }
const float* gos_TerrainLodChunk_ShorelineMaskBounds()  { return s_shorelineBounds; }

// WATER-SHORE-EDGE-1: hi-res R32F visual-height texture for the water FS
// per-fragment shore crossing. Same single-owner pattern as the mask above.
GLuint s_waterShoreHeightTex       = 0;
float  s_waterShoreHeightBounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};

unsigned int gos_TerrainLodChunk_WaterShoreHeightTex()    { return s_waterShoreHeightTex; }
// VK-PARITY-WATER-SHORE-1: bumped by every shore-data (mask / height) upload so
// the VK water island can re-import its copies of these GL textures when a new
// mission replaces the contents behind the SAME GL texture ids. GL never reads it.
unsigned s_waterShoreDataGen = 0;
unsigned int gos_TerrainLodChunk_WaterShoreDataGeneration() { return s_waterShoreDataGen; }
const float* gos_TerrainLodChunk_WaterShoreHeightBounds() { return s_waterShoreHeightBounds; }

// ---------------------------------------------------------------------------
// Shader program + uniform locations (Phase 4).
// ---------------------------------------------------------------------------

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
// VAO for patch draws — reused every frame, attributes re-pointed per batch.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Init / Destroy — called from gosRenderer::init / gosRenderer::destroy.
// ---------------------------------------------------------------------------

void gos_TerrainLodChunk_Init()
{
    if (s_heightSsbo.created())
        return; // idempotent

    if (!s_heightSsbo.create())
    {
        mc2::fmt::print_err("[TerrainLodChunk] glGenBuffers failed for height SSBO\n");
        fflush(stderr);
        return;
    }
    s_typeSsbo.create();   // Step 5b: terrainType SSBO (concrete)
    s_cementSsbo.create(); // Step 5c: cement word SSBO
    s_fieldStore = std::make_unique<mc2terrain::TerrainFieldStore>();

}

void gos_TerrainLodChunk_Destroy()
{
    s_fieldStore.reset();

    // TERRAIN-CONTROLMAP-SAMPLE-1: free the authored control-map texture (if loaded).
    if (s_controlMapTex != 0)
    {
        glDeleteTextures(1, &s_controlMapTex);
        s_controlMapTex  = 0;
        s_controlMapSide = 0;
    }

    // TERRAIN-MATERIAL-TEXTURES-1: free the per-layer albedo array (if loaded).
    // Reset the load-tried latch so a renderer re-create reloads it.
    if (s_matAlbedoArrayTex != 0)
    {
        glDeleteTextures(1, &s_matAlbedoArrayTex);
        s_matAlbedoArrayTex = 0;
    }
    s_matAlbedoLoadTried = false;

    // TERRAIN-SHORELINE-MASK-1: free the authored shoreline mask texture (if loaded).
    if (s_shorelineMaskTex != 0)
    {
        glDeleteTextures(1, &s_shorelineMaskTex);
        s_shorelineMaskTex = 0;
    }

    // GPU-BUFFER-TIER0-LODCHUNK-RAII-1: GpuBuffer::destroy() = glDeleteBuffers
    // + registry-invalidate (the hand-copied "mark slot unavailable" blocks
    // moved into the class). Safe no-ops when never created.
    if (s_visualHeightSsbo.created())
        s_visualMipFloats = 0;   // TERRAIN-LOD-GEOMORPH-1
    s_visualHeightSsbo.destroy();
    // TERRAIN-REAUTH-UNPIN-1 Half B: free the damp SSBO + CPU copies on teardown.
    s_visualDampSsbo.destroy();
    s_visualDampSide = 0;
    s_visualDampStatic.clear();
    s_visualDampCombined.clear();
    s_visualDampHadMovers = false;
    if (s_heightSsbo.created())
    {
        s_mapSide    = 0;
        s_halfMap    = 0.0f;
    }
    s_heightSsbo.destroy();
    s_typeSsbo.destroy();
    s_cementSsbo.destroy();
}
