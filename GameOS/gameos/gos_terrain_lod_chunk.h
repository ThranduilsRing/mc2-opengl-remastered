#pragma once
#include <cstdint>

// Terrain LOD chunk render system.
// mclib/ calls these functions. GameOS/gameos/ implements them.
// mclib/ must NEVER include GL headers or call gl*() directly.

struct TerrainDrawCommand {
    int blockOriginX;      // vertex-grid X origin = blockX * 20
    int blockOriginY;      // vertex-grid Y origin = blockY * 20
    int lodStep;           // one of {1, 2, 4, 5, 10, 20}
    int quadCountsPacked;  // (quadCountX & 0xFF) | ((quadCountY & 0xFF) << 8)
};
static_assert(sizeof(TerrainDrawCommand) == 16, "TerrainDrawCommand must be 16 bytes");

constexpr uint32_t TERRAIN_HEIGHT_SSBO_BINDING = 23u;
constexpr uint32_t TERRAIN_TYPE_SSBO_BINDING   = 24u;  // Step 5b: per-vertex terrainType (concrete)
constexpr uint32_t TERRAIN_CEMENT_SSBO_BINDING = 25u;  // Step 5c: per-vertex cement word (valid|layerIdx)
constexpr uint32_t TERRAIN_VISUAL_HEIGHT_SSBO_BINDING = 26u; // TERRAIN-VISUAL-HEIGHT-SAMPLE-1: 4x VISUAL heightfield (render-only)
constexpr uint32_t TERRAIN_VISUAL_DAMP_SSBO_BINDING   = 27u; // TERRAIN-REAUTH-UNPIN-1 Half B: coarse object-proximity displacement damp

// TERRAIN-CONTROLMAP-SAMPLE-1: authored RGBA control-map texture unit (not an
// SSBO — sampled with GL_LINEAR like the colormap). Gate MC2_TERRAIN_CONTROLMAP,
// default OFF; upload is only called when a sidecar was found at mission load.
constexpr int TERRAIN_CONTROLMAP_TEXUNIT = 12;

// TERRAIN-OVERLAY-V2-PARITY-1: authored cement/pad/runway overlay sidecar
// texture unit (free per recon's list: 1,2,4,6,7,8). Bounds-aware RGBA8,
// sampled by WORLD XY (not the 128wu cement tile grid) -- see
// gos_TerrainLodChunk_UploadOverlaySidecar. Gate MC2_TERRAIN_OVERLAY_V2,
// default OFF; upload only called when a sidecar was found at mission load.
constexpr int TERRAIN_OVERLAY_SIDECAR_TEXUNIT = 1;

// TERRAIN-SHORELINE-V3: authored land-side wet/foam shoreline mask sidecar
// texture unit (free per recon's list: 2,4,6,7,8 after overlay-V2 took 1).
// Bounds-aware RGBA8 (R=signed dist, G=wet, B=foam, A=valid), sampled by WORLD
// XY (same u_*Bounds pattern as control-map/overlay-V2) -- see
// gos_TerrainLodChunk_UploadShorelineMask. Gate MC2_TERRAIN_SHORELINE, default
// OFF. As of V3 the band's PLACEMENT comes from elevation
// (v_worldPos.z vs u_waterElevation), not this mask -- the mask is now an
// OPTIONAL modulator (wide-beach falloff / basin exclusion) applied on top;
// upload only called when a mask sidecar was found at mission load, but the
// band shows with or without one once the gate is on.
constexpr int TERRAIN_SHORELINE_TEXUNIT = 2;

// Submit block draw commands for the current frame.
// count==0 is a strict no-op. mclib calls this via Terrain::flushDrawCommands() only.
// skirtDepths: parallel float array [count], one depth value per command.
// skirtEdgeMasks: parallel uint8 array [count] (Phase 10.2b), bit 0=N,1=S,2=W,3=E
//   — draw a skirt only on edges whose neighbour LOD differs. nullptr -> all edges.
// edgeStitch: parallel uint32 array [count] (Phase 10.4). Packs the COARSER
//   neighbour's vertex stride per edge (N=bits0-7, S=8-15, W=16-23, E=24-31; 0 =
//   no stitch). Passed to the vert as u_edgeStitch; the fine edge's intermediate
//   verts snap onto the coarse edge line -> crack-free LOD seam. nullptr -> off.
// shadowTiers: parallel int array [count] (Slice B). Per-chunk shadow tier
//   (0=high-res dynamic near, 1=low-res dynamic mid, 2=static-only far, 3=none).
//   Set as u_shadowTier; used ONLY by the MC2_TERRAIN_LOD_CHUNK_DIAG=40 tier-tint
//   debug view. Does NOT change shadow sampling (Slice C). nullptr -> 0.
// morphFactors: parallel float array [count] (TERRAIN-LOD-GEOMORPH-1). Per-block
//   geomorph factor m in [0,1]: 0 = pure own-band surface, 1 = parent-band
//   surface (block interior slides onto the next-coarser band before the LOD
//   switch, killing the one-frame silhouette snap). Set as u_morphFactor; only
//   consumed by the vert when the bake shipped max mips (u_geomorphMips).
//   nullptr -> 0 (no morph).
void gos_TerrainLodChunk_SubmitDrawCommands(
    const TerrainDrawCommand* cmds,
    const float*              skirtDepths,
    const unsigned char*      skirtEdgeMasks,
    const unsigned int*       edgeStitch,
    const int*                shadowTiers,
    const float*              morphFactors,
    int                       count);

// Upload full heightfield to GPU SSBO at map load.
// elevations: float[mapSide*mapSide] row-major.
void gos_TerrainLodChunk_UploadHeightFull(const float* elevations, int mapSide);

// TERRAIN-VISUAL-HEIGHT-SAMPLE-1 Stage 1: upload the 4x VISUAL heightfield bake to
// a dedicated SSBO (binding 26). visualHeights: float[V*V] row-major, V=(mapSide-1)*4+1.
// Stage 1 is load+log only — NO geometry samples binding 26 yet (Stage 2 displaces).
// TERRAIN-LOD-GEOMORPH-1: optional max-preserving mip levels (visual_height_mips.r32)
// are APPENDED to the same binding-26 SSBO: mipMaxes = 5 levels (strides 2,4,5,10,20),
// each mapSide*mapSide row-major floats (mipFloats = 5*mapSide*mapSide total), holding
// the MAX of the fine bake over the +/- stride/2 coarse-cell footprint at every coarse
// vertex. nullptr/0 = no mips (legacy layout, geomorph inactive).
void gos_TerrainLodChunk_UploadVisualHeightFull(const float* visualHeights, int V,
                                                const float* mipMaxes = nullptr,
                                                int mipFloats = 0);

// TERRAIN-REAUTH-UNPIN-1 Half B: near-object displacement fade (the objfade
// safety). Static damp map (float[side*side] row-major, 0 = displacement OFF on
// object footprints .. 1 = full displacement) uploaded once per mission load to
// SSBO binding 27; the bake tool emits it as <mission>.beauty/visual_damp.r32.
// Rides MC2_TERRAIN_VISUAL_DISPLACE; MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE=0
// disables (default ON when displacing — it is the safety).
void gos_TerrainLodChunk_UploadVisualDampStatic(const float* damp01, int side);

// Per-frame MOVER stamps: combined = min(static damp, per-mover smoothstep
// falloff). cellXY = float pairs in COARSE CELL space (x = (worldX+halfMap)/128,
// y = (halfMap-worldY)/128), count = number of movers. radiusCells / innerCells
// are the fade radius / inner full-damp radius in cell units. No-op unless the
// damp SSBO is live. Cheap: N^2 copy + count small stamps + one BufferSubData.
void gos_TerrainLodChunk_UpdateVisualDampMovers(const float* cellXY, int count,
                                                float radiusCells, float innerCells);

// True when the mover-damp update is worth computing this frame (displace gate
// on + bake loaded + objfade gate on + static damp uploaded). Lets the game
// layer (code/mission.cpp) skip mover gathering entirely when inactive.
bool gos_TerrainLodChunk_VisualDampWanted();

// Step 5b: upload per-vertex terrainType (0..N; cement/concrete ~3) to its SSBO.
// types: float[mapSide*mapSide] row-major (parallel to the heightfield).
void gos_TerrainLodChunk_UploadTerrainTypeFull(const float* types, int mapSide);

// Step 5c: upload per-vertex CEMENT WORD (bit31 = valid, bits15:0 = cement-atlas
// layer index; 0 = not cement). words: uint32[count] indexed by vn = mx + my*mapSide
// (matches the heightfield grid). Called after the cement catalog atlas is built.
void gos_TerrainLodChunk_UploadCementWordsFull(const unsigned int* words, int count, int mapSide);

// TERRAIN-CONTROLMAP-SAMPLE-1: upload the authored RGBA control map (vertex
// resolution, side*side, row-major, 4 bytes/texel) as a GL_RGBA8 2D texture
// bound at TERRAIN_CONTROLMAP_TEXUNIT. rgba may be null / side<=0 to mean
// "no sidecar" — the caller (mclib/terrain.cpp) only calls this when a sidecar
// was actually loaded; the driver uploads u_useControlMap=1 only when the
// texture handle is valid. Passthrough (gate off or no sidecar) never calls
// this and u_useControlMap uploads 0 (byte-identical legacy classifier path).
void gos_TerrainLodChunk_UploadControlMap(const unsigned char* rgba, int side);

// TERRAIN-OVERLAY-V2-PARITY-1: upload the authored cement/pad/runway overlay
// sidecar as a GL_RGBA8 2D texture (arbitrary WxH, NOT tied to vertex grid)
// bound at TERRAIN_OVERLAY_SIDECAR_TEXUNIT. RGB = pre-tinted cement/overlay
// diffuse, A = coverage/edge alpha (0 = no overlay, >=0.5 legacy-parity hit).
// worldBounds = {topLeftX, topLeftY, sizeX, sizeY} in world units: topLeftX =
// MIN world X (west edge), topLeftY = MAX world Y (north/top edge) -- SAME
// convention as the colormap atlas uniforms (u_atlasTopLeftX/Y), so PNG row 0
// (top) == north edge, no vertical flip. The frag maps v_worldPos.xy -> UV via
// uv.x=(worldX-topLeftX)/sizeX, uv.y=(topLeftY-worldY)/sizeY. rgba may be
// null / w<=0 / h<=0 to mean "no sidecar" -- the caller (mclib/terrain.cpp)
// only calls this when a sidecar was actually loaded; the driver uploads
// u_useOverlaySidecar=1 only when the texture handle is valid. Passthrough
// (gate off or no sidecar) never calls this and u_useOverlaySidecar uploads 0
// (byte-identical legacy cement-word + overlay-pass path).
void gos_TerrainLodChunk_UploadOverlaySidecar(const unsigned char* rgba, int w, int h,
                                               float boundsTopLeftX, float boundsTopLeftY,
                                               float boundsSizeX, float boundsSizeY);

// TERRAIN-OVERLAY-V2-DECAL-SUPPRESS-1: read-only accessors so the indirect
// decal bake (gos_terrain_indirect.cpp BuildDecalStaticVBO) can skip legacy
// runway/road/cement decals for tiles the authored OVERLAY_V2 sidecar already
// covers -- otherwise BOTH draw and the runway visibly double-draws. Returns
// the loaded state and the {topLeftX, topLeftY(=maxY), sizeX, sizeY} world
// bounds (same convention as the upload). bounds valid only when loaded==true.
bool         gos_TerrainLodChunk_OverlaySidecarLoaded();
const float* gos_TerrainLodChunk_OverlayBounds();

// TERRAIN-SHORELINE-V3: upload the authored land-side wet/foam shoreline mask
// as a GL_RGBA8 2D texture (arbitrary WxH, NOT tied to the vertex grid) bound
// at TERRAIN_SHORELINE_TEXUNIT. R=signed dist (0.5=waterline), G=wet weight,
// B=foam weight, A=valid/coverage. worldBounds = {topLeftX, topLeftY, sizeX,
// sizeY} in world units, SAME convention as gos_TerrainLodChunk_UploadOverlaySidecar
// (topLeftX=MIN world X, topLeftY=MAX world Y, PNG row 0 = north edge, no
// vertical flip). rgba may be null / w<=0 / h<=0 to mean "no sidecar" -- the
// caller (mclib/terrain.cpp) only calls this when a mask was actually loaded;
// the driver uploads u_hasShorelineMask=1 only when the texture handle is
// valid. As of V3 this mask is an OPTIONAL MODULATOR on top of the
// elevation-placed band (u_useShorelineMask gates the band itself, driven by
// MC2_TERRAIN_SHORELINE directly, independent of this upload) -- absence of a
// mask still produces full elevation bands. Passthrough (gate off) never
// reaches the shoreline block at all (byte-identical -- no shoreline band,
// legacy screen runShoreline() stays active).
void gos_TerrainLodChunk_UploadShorelineMask(const unsigned char* rgba, int w, int h,
                                              float boundsTopLeftX, float boundsTopLeftY,
                                              float boundsSizeX, float boundsSizeY);

// TERRAIN-SHORELINE-V3: true whenever the elevation-based shoreline band is
// active (MC2_TERRAIN_SHORELINE gate ON) -- a mask sidecar is NO LONGER
// required (V3 places the band by v_worldPos.z vs u_waterElevation; a loaded
// mask is only an optional modulator). Consumed by gos_postprocess.cpp to
// suppress the legacy screen-space runShoreline() pass while the terrain-side
// band is active (recon landmine #6: "double-shore" -- both must not
// brighten the seam at once). false (default, gate off) -> runShoreline()
// behaves exactly as before (byte-identical). Function name kept for
// caller-site stability even though the semantics widened past "mask
// uploaded" to "gate on".
bool gos_TerrainLodChunk_IsShorelineMaskActive();

// Patch a dirty block's heightfield rows after terrain edit.
// rowData: float[(quadCountY+1)*(quadCountX+1)] row-major.
void gos_TerrainLodChunk_UploadHeightPatch(
    const float* rowData,
    int originX, int originY,
    int quadCountX, int quadCountY,
    int mapSide);

// Allocate the height SSBO. Call once after GL context is live (from gosRenderer::init).
void gos_TerrainLodChunk_Init();

// Free the height SSBO. Call at shutdown (from gosRenderer::destroy).
void gos_TerrainLodChunk_Destroy();
