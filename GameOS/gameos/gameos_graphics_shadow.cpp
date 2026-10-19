// gameos_graphics: the terrain shadow uniform uploads, gos_SetupObjectShadows and the shadow wrappers (the GL shadow passes are gone).
// Declared in the class headers; shared names in gameos_graphics_material_internal.h.
#include "gameos_graphics_material_internal.h"
#include "mc2fmt.h"
#include "static_shadow_spec.h"

#include <cstring>

// Shared helper called from each of the three terrain uniform-upload sites below.
// Binds the per-mission R32F height texture to unit 11, pushes the worldspace
// mapping params and the cliff shadow floor. Caller is responsible for restoring
// the active texture unit afterwards (existing sites already do glActiveTexture
// (GL_TEXTURE0) immediately after this helper).
void bindTerrainHeightTexUniforms(GLint heightTexLoc, GLint paramsLoc,
                                         GLint cliffShadowFloorLoc,
                                         float cliffShadowFloor)
{
    const GLuint htex = (GLuint)gos_terrainHeightTexHandle();
    if (htex != 0 && heightTexLoc >= 0) {
        glUniform1i(heightTexLoc, kTerrainTexUnitHeight);
        glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitHeight);
        glBindTexture(GL_TEXTURE_2D, htex);
    }
    if (htex != 0 && paramsLoc >= 0) {
        const float wuPerVert = gos_terrainHeightWorldUnitsPerVertex();
        const float invWu = (wuPerVert > 0.0f) ? (1.0f / wuPerVert) : 0.0f;
        const float p[4] = {
            static_cast<float>(gos_terrainHeightTexSide()),
            invWu,
            gos_terrainHeightMapTopLeftX(),
            gos_terrainHeightMapTopLeftY(),
        };
        glUniform4fv(paramsLoc, 1, p);
    }
    // CLIFF SHADOW FLOOR: lifts shadow-side steep terrain faces off near-black.
    // Env gate MC2_TERRAIN_CLIFF_SHADOW_FLOOR is authoritative — unset/=0 forces
    // 0.0 (shader max(shadow, 0*blend) => no-op, byte-identical). When set to a
    // non-zero float the member value (ImGui-tunable) is uploaded, bounded 0..1.
    float effectiveCliffFloor = 0.0f;
    if (mc2::gate::get<"MC2_TERRAIN_CLIFF_SHADOW_FLOOR">()) {
        effectiveCliffFloor = cliffShadowFloor;
        if (effectiveCliffFloor < 0.0f) effectiveCliffFloor = 0.0f;
        if (effectiveCliffFloor > 1.0f) effectiveCliffFloor = 1.0f;
    }
    if (cliffShadowFloorLoc >= 0) {
        glUniform1f(cliffShadowFloorLoc, effectiveCliffFloor);
    }
}

void __stdcall gos_SetRenderMaterialUniformBlockBindingPoint(HGOSRENDERMATERIAL material, const char* name, uint32_t slot)
{
	gosASSERT(material && name);
	material->setUniformBlock(name, slot);
}

void __stdcall gos_SetupObjectShadows(HGOSRENDERMATERIAL material)
{
	ZoneScopedN("SetupObjectShadows");
	gosASSERT(material);
	gosASSERT(g_gos_renderer);

	// MERGE-CONFLICT-UI-PHASE1: theirs (ui-phase1 fork) wanted to move this
	// `gosPostProcess* pp = getGosPostProcess(); if (!pp) return;` guard to
	// AFTER the terrainMVP upload block below, so that 3D camera views shown
	// during the logistics/menu phase (encyclopedia mech viewer, options
	// gameplay preview — added by the ui-phase1 GuiRuntime work) still get a
	// valid MVP uniform even when gosPostProcess hasn't been created yet
	// (pp is only created once a mission/scene is loaded). Ours has since
	// substantially rewritten the terrainMVP upload block itself
	// (OBJECT-DECAL-MATRIX-SHARE-1 / OBJECT-SHADOW-MVP-CURRENCY-1: routes
	// object MVP through gos_GetObjectDrawMVP with a view-epoch check +
	// MC2_PROP_FIXB_MVP killswitch, rather than always using
	// getTerrainMVP() directly) — the exact lines theirs' hunk reorders.
	// Reordering the pp-null-check without re-verifying that rewritten
	// block's assumptions (e.g. whether gos_GetObjectDrawMVP or
	// g_gos_renderer state depends on pp already existing) is exactly the
	// kind of renderer-internals change this merge must not blind-apply.
	// Preserving ours' early-return-before-MVP-upload ordering as-is;
	// theirs' pp-guard-after-MVP reordering was NOT applied. If the
	// encyclopedia/options 3D preview needs a valid MVP without
	// gosPostProcess, this is the seam to revisit — verify the rewritten
	// MVP block tolerates pp==nullptr before moving the guard.
	gosPostProcess* pp = getGosPostProcess();
	if (!pp) return;

	GLuint shp = material->getShader()->shp_;

	// Upload terrainMVP for GPU projection (MC2 world → clip space).
	// OBJECT-DECAL-MATRIX-SHARE-1: on solid-armed frames terrain + the cement/road
	// decals (drawDecalStaticBatch) project through the BAKED dispatch matrix
	// (getDispatchMvp16). Objects (buildings/turrets/generators + legacy shapes via
	// this path) used the LIVE camera matrix, which lags the baked snapshot as the
	// camera zooms — so a turret base and the coplanar cement decal drifted apart
	// and the turret sank at some zooms. Project objects through the SAME baked
	// matrix when armed so the coplanar ordering is fixed (OBJECT_DEPTH_BIAS then
	// cleanly decides). Fall back to live pre-arm / first frame.
	if (g_gos_renderer->isTerrainMVPValid()) {
		GLint mvpLoc = glGetUniformLocation(shp, "u_worldToClipGL");
		if (mvpLoc >= 0) {
			// OBJECT-SHADOW-MVP-CURRENCY-1: this projects OBJECT geometry
			// (buildings/turrets/generators + legacy shapes), so a stale-view
			// dispatch snapshot drifts/sinks them on zoom/rotate — the same class
			// as RENDER-VIEW-CURRENCY-1, previously hidden in this terrain-phase TU.
			// Route through the view-epoch-checked accessor: snapshot only when its
			// epoch is current, else live MVP. Killswitch shares the object family's
			// MC2_PROP_FIXB_MVP gate.
			static const bool s_objShadowFixB = mc2::gate::get<"MC2_PROP_FIXB_MVP">();
			const float* mvp = gos_GetObjectDrawMVP(s_objShadowFixB);
			if (!mvp) mvp = reinterpret_cast<const float*>(&g_gos_renderer->getTerrainMVP());
			glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, mvp);
		}
	}

	// Terrain light direction (needed for shadow bias calculations)
	GLint loc = glGetUniformLocation(shp, "terrainLightDir");
	if (loc >= 0)
		glUniform4fv(loc, 1, reinterpret_cast<const float*>(&g_gos_renderer->getTerrainLightDir()));

	// The shadow maps are Vulkan images the GL shaders cannot sample: the GL object material runs shadow-free.
	loc = glGetUniformLocation(shp, "enableShadows");
	if (loc >= 0) glUniform1i(loc, 0);
	loc = glGetUniformLocation(shp, "enableDynamicShadows");
	if (loc >= 0) glUniform1i(loc, 0);
}

void __stdcall gos_SetCommonMaterialParameters(HGOSRENDERMATERIAL material)
{
	gosASSERT(material);
	gosASSERT(g_gos_renderer);

	const mat4& projection = getGosRenderer()->getProj2Screen();
	const vec4& vp = getGosRenderer()->getRenderViewport();

	// TODO: make typed parameters !!!!!!!!!!!!!!! not just float* pointers, helps track errors
	gos_SetRenderMaterialParameterMat4(material, "projection_", projection);
	gos_SetRenderMaterialParameterFloat4(material, "vp", vp);
}
// VK-SHADOW-COPY-ONCE-1: generation counter bumped each time the static
// shadow map is (re)built (mission load / re-arm). 0 while never built.
uint64_t gos_GetStaticShadowGeneration() {
    gosPostProcess* pp = getGosPostProcess();
    return pp ? pp->staticShadowGeneration() : 0;
}
bool gos_GetStaticShadowSpec(StaticShadowSpec* out) {
    gosPostProcess* pp = getGosPostProcess();
    if (!out || !pp || !pp->shadowsEnabled_ || !pp->staticLightMatrixBuilt() || pp->staticShadowGeneration() == 0) return false;
    std::memcpy(out->lightSpace, pp->getLightSpaceMatrix(), sizeof(out->lightSpace));
    out->size = pp->getShadowMapSize();
    out->biasFactor = pp->getShadowBiasFactor();
    out->biasUnits = pp->getShadowBiasUnits();
    out->generation = pp->staticShadowGeneration();
    return out->size > 0;
}
// VPL-#shadow C-1: per-mission re-arm of the one-shot full-map static
// shadow build (called from Terrain::destroy so mission 2+ rebuilds
// against fresh blocks[] instead of freezing mission 1's shadow).
void gos_ResetStaticLightMatrix() {
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->resetStaticLightMatrix();
}
// VPL-#shadow Phase 1: the gos_*ShadowRebuild* one-shot-flag API
// (s_shadowRebuildPending + Request/Pending/Clear) is RETIRED. Its only
// caller was the txmmgr camera-windowed-accumulate prime block, deleted
// with the move to the build-once full-map static shadow. Do not
// reintroduce a camera-motion shadow trigger.
void gos_BuildDynamicLightMatrix(float sx, float sy, float sz,
                                  const float camFitCornersMC2[8][3],
                                  const float shadowCenterXYZ[3], bool shadowCenterValid) {
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->buildDynamicLightMatrix(sx, sy, sz, camFitCornersMC2,
                                        shadowCenterXYZ, shadowCenterValid);
}
