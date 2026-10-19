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
