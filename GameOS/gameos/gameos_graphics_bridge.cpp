#include "shadow_quality.h"
// gameos_graphics: terrain program-id accessors and the terrain surface, indirect, mask, mine and decal draw bridges.
// Declared in the class headers; shared names in gameos_graphics_material_internal.h.
#include "gameos_graphics_material_internal.h"
#include "mdi_submit.h"                            // MDI-SUBMISSION-SCAFFOLD-1: this piece issues indirect draws
#include "mc2fmt.h"


// SHADOW-SPINE-0: read-only accessors for shadow-pass pipeline state.
// All read from the existing gosPostProcess singleton; no new state.
bool gos_getShadowsEnabled() {
    gosPostProcess* pp = getGosPostProcess();
    return pp && pp->shadowsEnabled_;
}
int gos_getShadowMapSize() {
    gosPostProcess* pp = getGosPostProcess();
    return mc2shadow::shadowQuality().mapSide;
}
int gos_getDynShadowMapSize() {
    gosPostProcess* pp = getGosPostProcess();
    return mc2shadow::shadowQuality().mapSide;
}
uint64_t gos_GetTerrainMaterialNormalVersion() {
    gosRenderer* r = getGosRenderer();
    return r ? r->getTerrainMaterialNormalVersion() : 0;
}
uint32_t gos_GetTerrainMaterialNormalHandle(int slot) {
    gosRenderer* r = getGosRenderer();
    return r ? static_cast<uint32_t>(r->getTerrainMaterialNormalHandle(slot)) : static_cast<uint32_t>(INVALID_TEXTURE_ID);
}

// ─── gos_terrain_bridge implementation ────────────────────────────────────
// Defined here because the full gosRenderer type is visible in this TU.
// Declarations live in gos_terrain_bridge.h.

// gos_terrain_bridge_getMaterial and gos_terrain_bridge_getShaderProgram deleted by
// DEAD-BRIDGE-DELETE-1: zero external callers tree-wide (whole-repo grep incl. editor/
// tools/shaders/scripts). Provably-dead public wrappers around live gosRenderer members.



// gos_terrain_bridge_applyVertexDeclaration deleted by TERRAIN-BRIDGE-BODY-DELETE-2:
// callerless after TerrainPatchStream::flush() retired (TERRAIN-BRIDGE-BODY-DELETE-1).


// gos_terrain_indirect_getDispatchMvp16 and gos_GetTerrainMVPMat4 are declared
// TU-wide near the top of this file (after g_terrainMaterialProfile) so that
// renderWaterFastPath (which precedes this block) can see them.

// Slice A — cement-overlay static-bake draw bridge. Thin forwarder to the
// gosRenderer member (which owns the private overlay program/locs/texture
// list), mirroring how gos_DrawTerrainOverlays forwards to drawTerrainOverlays
// Native mines submit through the overlay owner directly.
bool gos_terrain_bridge_drawDecalStatic(unsigned int               vboGL,
                                        const GosDecalStaticDraw*  draws,
                                        int                        drawCount)
{
    ZoneScopedN("Terrain::DecalStaticDraw");
    if (!g_gos_renderer) return false;
    return g_gos_renderer->drawDecalStaticBatch(vboGL, draws, drawCount);
}
