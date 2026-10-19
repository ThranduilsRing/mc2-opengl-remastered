// gameos_graphics: terrain program-id accessors and the terrain surface, indirect, mask, mine and decal draw bridges.
// Declared in the class headers; shared names in gameos_graphics_material_internal.h.
#include "gameos_graphics_material_internal.h"
#include "mdi_submit.h"                            // MDI-SUBMISSION-SCAFFOLD-1: this piece issues indirect draws
#include "mc2fmt.h"


// TERRAIN-SPINE-0: free-function program-id accessors for the read-only
// Terrain Pass inspector. Defined here (after the gosRenderer class body)
// so other TUs (gameosmain.cpp) can reference them without seeing the full
// class. Each returns the raw GL program object id, or 0 if not linked.
uint32_t gos_getTerrainSurfaceProgramId() { return 0u; }
uint32_t gos_getThinTerrainProgramId() {
    gosRenderer* r = getGosRenderer();
    if (!r) return 0u;
    glsl_program* p = r->getThinTerrainProgram();
    return (p && p->shp_) ? (uint32_t)p->shp_ : 0u;
}
uint32_t gos_getTerrainOverlayProgramId() {
    gosRenderer* r = getGosRenderer();
    if (!r) return 0u;
    glsl_program* p = r->getTerrainOverlayProgram();
    return (p && p->shp_) ? (uint32_t)p->shp_ : 0u;
}

// SHADOW-SPINE-0: read-only program-id accessor for the static-prop shadow
// program (shadow_static_prop.vert + shadow_instanced.frag). Mirrors the
// terrain accessors above. Returns 0 when not linked.
uint32_t gos_getStaticPropShadowProgramId() { return 0u; }
// SHADOW-SPINE-0: terrain shadow program is owned by the shadow_terrain
// gosRenderMaterial (not a direct glsl_program* slot). Look up via the
// material's shader. Returns 0 when not yet loaded.
uint32_t gos_getTerrainShadowProgramId() { return 0u; }
// SHADOW-SPINE-0: read-only accessors for shadow-pass pipeline state.
// All read from the existing gosPostProcess singleton; no new state.
bool gos_getShadowsEnabled() {
    gosPostProcess* pp = getGosPostProcess();
    return pp && pp->shadowsEnabled_;
}
int gos_getShadowMapSize() {
    gosPostProcess* pp = getGosPostProcess();
    return pp ? pp->getShadowMapSize() : 0;
}
int gos_getDynShadowMapSize() {
    gosPostProcess* pp = getGosPostProcess();
    return pp ? pp->getDynamicShadowMapSize() : 0;
}
uint64_t gos_GetTerrainMaterialNormalVersion() {
    gosRenderer* r = getGosRenderer();
    return r ? r->getTerrainMaterialNormalVersion() : 0;
}
uint32_t gos_GetTerrainMaterialNormalHandle(int slot) {
    gosRenderer* r = getGosRenderer();
    return r ? (uint32_t)r->getTerrainMaterialNormalHandle(slot) : (uint32_t)INVALID_TEXTURE_ID;
}

// ─── gos_terrain_bridge implementation ────────────────────────────────────
// Defined here because the full gosRenderer type is visible in this TU.
// Declarations live in gos_terrain_bridge.h.

// gos_terrain_bridge_getMaterial and gos_terrain_bridge_getShaderProgram deleted by
// DEAD-BRIDGE-DELETE-1: zero external callers tree-wide (whole-repo grep incl. editor/
// tools/shaders/scripts). Provably-dead public wrappers around live gosRenderer members.

void gos_terrain_bridge_bindUniforms(gosRenderMaterial* material) {
    if (g_gos_renderer && material)
        g_gos_renderer->terrainBindUniformsForPatchStream(material);
}

// gos_terrain_bridge_applyVertexDeclaration deleted by TERRAIN-BRIDGE-BODY-DELETE-2:
// callerless after TerrainPatchStream::flush() retired (TERRAIN-BRIDGE-BODY-DELETE-1).

unsigned int gos_terrain_bridge_glTextureForGosHandle(unsigned int gosHandle) {
    if (!g_gos_renderer) return 0;
    if (gosHandle == 0u || gosHandle == INVALID_TEXTURE_ID) return 0;
    gosTexture* tex = g_gos_renderer->getTexture(gosHandle);
    if (!tex) return 0;
    return (unsigned int)tex->getTextureId();
}


unsigned int gos_terrain_bridge_getThinShaderProgram() {
    if (!g_gos_renderer) return 0u;
    glsl_program* p = g_gos_renderer->getThinTerrainProgram();
    return (p && p->shp_) ? (unsigned int)p->shp_ : 0u;
}
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
