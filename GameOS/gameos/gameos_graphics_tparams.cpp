// gameos_graphics: terrain uniform binding for the patch stream, the lighting debug view and the terrain, lighting, night and material setters and getters.
// Declared in the class headers; shared names in gameos_graphics_material_internal.h.
#include "gameos_graphics_material_internal.h"


// TERRAIN-MATERIAL-LIB-1: default OFF. Drives u_useMaterialLib (chunk-path
// roughness/AO branch). Evaluated once (static) -- matches terrainNormalArrayEnabled().
static bool terrainMaterialLibEnabled() {
    static const bool enabled = mc2::gate::get<"MC2_TERRAIN_MATERIAL_LIB">();
    return enabled;
}

// Item 1 CSM: upload the dynamic-shadow uniforms for a terrain-style loc set.
// When CSM is OFF this is byte-identical to the legacy inline block (sets
// dynamicLightSpaceMatrix + dynamicShadowMap + binds the 2D texture). When ON
// it instead uploads dynamicCascadeMatrices[N] + dynamicCsmCount and binds the
// GL_TEXTURE_2D_ARRAY to the same texunit. Template over the loc struct because
// TerrainUniformLocs and ThinTerrainUniformLocs share field names.
template <typename Locs>
static void uploadDynamicShadowUniforms(const Locs& tl, gosPostProcess* pp, GLint texUnit)
{
    if (mc2ShadowCsmEnabled() && pp->getDynamicShadowArrayTexture()) {
        if (tl.dynamicCascadeMatrices >= 0)
            glUniformMatrix4fv(tl.dynamicCascadeMatrices,
                               pp->getDynamicShadowCascadeCount(), GL_FALSE,
                               pp->getDynamicCascadeMatrices());
        if (tl.dynamicCsmCount >= 0)
            glUniform1i(tl.dynamicCsmCount, pp->getDynamicShadowCascadeCount());
        if (tl.dynamicCascadeTexelWorld >= 0)
            glUniform1fv(tl.dynamicCascadeTexelWorld,
                         pp->getDynamicShadowCascadeCount(),
                         pp->getDynamicCascadeTexelWorld());
        if (tl.csmDepthSpan >= 0)
            glUniform1f(tl.csmDepthSpan, pp->getCsmDepthSpan());
        if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 1);
        if (tl.dynamicShadowArray >= 0) {
            glUniform1i(tl.dynamicShadowArray, texUnit);
            glActiveTexture(GL_TEXTURE0 + texUnit);
            glBindTexture(GL_TEXTURE_2D_ARRAY, pp->getDynamicShadowArrayTexture());
            glActiveTexture(GL_TEXTURE0);
        }
        // Per-cascade shadow resolution: the LAST cascade samples this separate
        // lower-res 2D depth texture on its own dedicated unit.
        if (tl.dynamicFullMapTexelWorld >= 0)
            glUniform1f(tl.dynamicFullMapTexelWorld, pp->getDynamicFullMapTexelWorld());
        if (tl.dynamicFullMapShadow >= 0) {
            glUniform1i(tl.dynamicFullMapShadow, kTerrainTexUnitDynFullMap);
            glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitDynFullMap);
            glBindTexture(GL_TEXTURE_2D, pp->getDynamicFullMapTexture());
            glActiveTexture(GL_TEXTURE0);
        }
        return;
    }
    // Legacy single-map path (byte-identical to the prior inline block).
    if (tl.dynamicLightSpaceMatrix >= 0)
        glUniformMatrix4fv(tl.dynamicLightSpaceMatrix, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix());
    if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 1);
    if (tl.dynamicShadowMap >= 0) {
        glUniform1i(tl.dynamicShadowMap, texUnit);
        glActiveTexture(GL_TEXTURE0 + texUnit);
        glBindTexture(GL_TEXTURE_2D, pp->getDynamicShadowTexture());
        glActiveTexture(GL_TEXTURE0);
    }
}

// LIGHTING-DEBUG-VIEWS: named, render-family-shared lighting debug enum. Maps
// MC2_LIGHTING_DEBUG_VIEW=<name> onto a shared surfaceDebugMode integer space.
// The 40-series is reserved for the unified lighting channels so the SAME name
// means the SAME channel across terrain (1A), static_prop (1B) and mech (1C).
// Returns -1 when the env var is unset OR the name is unknown, so callers keep
// their existing debug mode (default 0 = off = byte-identical legacy render).
// Takes precedence over MC2_TERRAIN_DEBUG_MODE when both are set.
// NON-static: also consumed by gos_static_prop_batcher.cpp (extern decl there).
// Note: not every channel applies to every family — a family ignores ids it
// does not implement (falls through to its normal render), which is safe.
//
// DEBUG-VIEW-SWEEP-1: runtime override slot. The env resolve stays once-only,
// but gos_SetGlobalDebugView() can now retarget the shared channel at runtime
// (command-channel `debugview N`). -1 = no override (env value, default off).
static int g_mc2LightingDebugOverride = -1;
int mc2LightingDebugMode()
{
    // REDUNDANT-PASS-HUNT-1 (re-compute class): called per frame from 5 hot draw
    // sites (terrain chunk, static-prop batcher x2, this TU x2), each hit doing a
    // getenv() + up to 11 strcmp on a value that cannot change mid-process (no
    // setenv/putenv/ImGui setter exists for MC2_LIGHTING_DEBUG_VIEW). Resolve once.
    static const int s_mode = []() -> int {
        using mc2::gate::is;
        // Enum row: unset, empty and an unknown name all read the (empty) default token -> -1.
        if (is<"MC2_LIGHTING_DEBUG_VIEW", "off">() || is<"MC2_LIGHTING_DEBUG_VIEW", "0">()) return 0;   // explicit no-op render
        if (is<"MC2_LIGHTING_DEBUG_VIEW", "albedo">())     return 40;
        if (is<"MC2_LIGHTING_DEBUG_VIEW", "normal">())     return 41;   // final per-fragment N as RGB
        if (is<"MC2_LIGHTING_DEBUG_VIEW", "sun">())        return 42;   // sun N·L diffuse term
        if (is<"MC2_LIGHTING_DEBUG_VIEW", "ambient">())    return 43;   // hemisphere/ambient/SH fill only
        if (is<"MC2_LIGHTING_DEBUG_VIEW", "shadow">())     return 44;   // shadow factor (terrain) / no-shadow marker (props)
        if (is<"MC2_LIGHTING_DEBUG_VIEW", "final">())      return 45;   // == default lit render (falls through)
        if (is<"MC2_LIGHTING_DEBUG_VIEW", "overbright">()) return 46;   // over/under-bright heatmap
        if (is<"MC2_LIGHTING_DEBUG_VIEW", "lightcount">()) return 47;   // dynamic light-count heatmap (props/mech)
        if (is<"MC2_LIGHTING_DEBUG_VIEW", "lightindex">()) return 48;   // baked-static-light-index palette (props/mech)
        return -1;                                                      // unset / unknown -> safe fallback (keep existing)
    }();
    // DEBUG-VIEW-SWEEP-1: runtime override wins over the env-resolved value.
    return (g_mc2LightingDebugOverride >= 0) ? g_mc2LightingDebugOverride : s_mode;
}
static int g_mc2GlobalDebugView = 0;
extern "C" void gos_SetGlobalDebugView(int view)
{
    if (view < 0) view = 0;
    if (view > 6) view = 6;
    g_mc2GlobalDebugView = view;
    static const struct { int lighting; int mech; int vfx; int propMat; } kMap[7] = {
        { -1,  0, 0, 0 },   // 0 off
        { 44,  3, 0, 0 },   // 1 shadow factor
        { 50, 50, 0, 0 },   // 2 normalized depth
        { 40,  2, 1, 0 },   // 3 unlit albedo
        { 51, 51, 4, 0 },   // 4 overdraw proxy
        { 41,  4, 0, 0 },   // 5 world normals
        { 52, 52, 3, 2 },   // 6 chunk/LOD tint
    };
    g_mc2LightingDebugOverride = kMap[view].lighting;
    batcher_setMechDebugMode(kMap[view].mech);
    gos_vfx_setDebugMode(kMap[view].vfx);
    batcher_setDebugMaterialMode(kMap[view].propMat);
    fprintf(stderr, "[DEBUG_VIEW v1] view=%d lighting=%d mech=%d vfx=%d propMat=%d\n",
            view, kMap[view].lighting, kMap[view].mech, kMap[view].vfx, kMap[view].propMat);
}
extern "C" int gos_GetGlobalDebugView() { return g_mc2GlobalDebugView; }

// TERRAIN-DETAIL-ANTI-TILE-1: pack distance-fade + macro-noise knobs into the
// unused .yzw of the detailNormalStrength uniform (the shader reads only .x today).
// Gate MC2_TERRAIN_DETAIL_ANTITILE; default OFF => all three return 0 =>
// detailNormalStrength stays {strength,0,0,0} => byte-identical render.
//   .y = fadeStart (world units)   .z = fadeEnd (world units)   .w = macroStrength
// Read once. Returns pointer to a static float[3].
static const float* mc2_detailAntiTileYZW()
{
    static float s_yzw[3] = { 0.0f, 0.0f, 0.0f };
    static bool s_init = false;
    if (!s_init) {
        s_init = true;
        const bool antiTile = mc2::gate::get<"MC2_TERRAIN_DETAIL_ANTITILE">();   // row: Lead0, same rule as the chunk reader
        if (antiTile) {
            // The rows' defaults are the legacy ones for the fade band (1000 / 3000). MACRO_STRENGTH is the
            // ledgered divergent gate: this legacy path defaults to 0.35, the chunk path to 0.
            float start = mc2::gate::get<"MC2_TERRAIN_DETAIL_FADE_START">();
            float end   = mc2::gate::get<"MC2_TERRAIN_DETAIL_FADE_END">();
            if (end < start + 1.0f) end = start + 1.0f;   // keep fade band positive
            s_yzw[0] = start;
            s_yzw[1] = end;
            s_yzw[2] = mc2::gate::isNonEmpty<"MC2_TERRAIN_DETAIL_MACRO_STRENGTH">()
                           ? mc2::gate::get<"MC2_TERRAIN_DETAIL_MACRO_STRENGTH">() : 0.35f;
        }
    }
    return s_yzw;
}

void gosRenderer::terrainBindUniformsForPatchStream(gosRenderMaterial* material)
{
    if (!material) return;
    material->setTransform(projection_);
    material->setFogColor(fog_color_);
    material->apply();
    material->setSamplerUnit(gosMesh::s_tex1, 0);

    GLuint shp = material->getShader()->shp_;
    cacheTerrainUniformLocations(shp);
    const auto& tl = terrainLocs_;

    float tessParams[4] = { terrain_tess_level_, terrain_tess_level_, 0.0f, 0.0f };
    float tessDist[4]   = { terrain_tess_dist_near_, terrain_tess_dist_far_, 0.0f, 0.0f };
    float tessDisp[4]   = { terrain_phong_alpha_, terrain_displace_scale_, 0.0f, 0.0f };
    if (tl.tessLevel >= 0)         glUniform4fv(tl.tessLevel, 1, tessParams);
    if (tl.tessDistanceRange >= 0) glUniform4fv(tl.tessDistanceRange, 1, tessDist);
    if (tl.tessDisplace >= 0)      glUniform4fv(tl.tessDisplace, 1, tessDisp);
    if (tl.cameraPos >= 0)         glUniform4fv(tl.cameraPos, 1, (const float*)&terrain_camera_pos_);
    float debugMode = terrain_debug_mode_;
    if (mc2::gate::isSet<"MC2_TERRAIN_DEBUG_MODE">())
        debugMode = mc2::gate::get<"MC2_TERRAIN_DEBUG_MODE">();
    { int lvm = mc2LightingDebugMode(); if (lvm >= 0) debugMode = (float)lvm; }  // LIGHTING-DEBUG-VIEWS-1A
    float tessDebugVec[4] = { debugMode, 0.0f, 0.0f, 0.0f };
    if (tl.tessDebug >= 0)         glUniform4fv(tl.tessDebug, 1, tessDebugVec);
    if (tl.pathTint >= 0)          glUniform1i(tl.pathTint, mc2ShaderPathTint());  // MC2_SHADER_PATH_TINT

    if (tl.mapHalfExtent >= 0) {
        gosPostProcess* pp = getGosPostProcess();
        float halfExt = pp ? pp->getMapHalfExtent() : 0.0f;
        glUniform1f(tl.mapHalfExtent, halfExt);

        // OVERLAY-MAGENTA-TEXTURE-RECON-1 (Source B): the colormap "no-data" magenta
        // texels are hidden by the off-map edge-haze ONLY when mapHalfExtent > 0. If
        // this is <= 0 for a map, the haze guard is skipped and magenta renders raw.
        // Emit the live value (throttled to value changes) so we can tell Source-B
        // guard-fail from a Source-A missing-texture. Gated MC2_OVERLAY_MAGENTA_TRACE.
        {
            static const bool s_magentaTrace = mc2::gate::get<"MC2_OVERLAY_MAGENTA_TRACE">();
            static float s_lastHalfExt = -99999.0f;
            if (s_magentaTrace && halfExt != s_lastHalfExt && mc2_diag::tagEnabled("OVERLAY_MAGENTA")) {
                s_lastHalfExt = halfExt;
                char _mg_buf[160];
                snprintf(_mg_buf, sizeof(_mg_buf),
                         "{\"site\":\"map_half_extent\",\"value\":%.1f,\"guard_fail\":%d}",
                         halfExt, (halfExt <= 0.0f) ? 1 : 0);
                mc2_diag::writeEvent("OVERLAY_MAGENTA", 1, 0, _mg_buf);
            }
        }
    }
    if (terrain_mvp_valid_ && tl.terrainMVP >= 0)
        glUniformMatrix4fv(tl.terrainMVP, 1, GL_FALSE, (const float*)&terrain_mvp_);
    // F1 Task 7b: probe-only worldToClipGL flat-uniform fallback path.
    // Kept for non-AMD or future driver fixes; SSBO path (binding 23) is
    // the reliable transport on AMD TES stages (proven: loc=-1 from driver
    // optimizer removing the unused flat uniform once SSBO became primary).
    // When the shader declares u_worldToClipGL as a flat uniform, loc >= 0
    // and this path fires. Currently loc=-1 (shader uses SSBO only).
    if (probeWorldToClipGLValid_ && tl.worldToClipGL >= 0)
        glUniformMatrix4fv(tl.worldToClipGL, 1, GL_FALSE, probeWorldToClipGL_);

    if (tl.terrainLightDir >= 0)        glUniform4fv(tl.terrainLightDir, 1, (const float*)&terrain_light_dir_);
    float tiling[4]      = { terrain_detail_tiling_, 0.0f, 0.0f, 0.0f };
    const float* _datYZW = mc2_detailAntiTileYZW();  // TERRAIN-DETAIL-ANTI-TILE-1 (yzw=0 when gate OFF)
    float strength[4]    = { terrain_detail_strength_, _datYZW[0], _datYZW[1], _datYZW[2] };
    float pomP[4]        = { terrain_pom_scale_, 8.0f, 32.0f, 0.0f };
    float worldScaleV[4] = { terrain_world_scale_, 0.0f, 0.0f, 0.0f };
    float cellP[4]       = { terrain_cell_scale_, terrain_cell_jitter_, terrain_cell_rotation_, 0.0f };
    if (tl.detailNormalTiling >= 0)   glUniform4fv(tl.detailNormalTiling, 1, tiling);
    if (tl.detailNormalStrength >= 0) glUniform4fv(tl.detailNormalStrength, 1, strength);
    // C1 tactical: push mission-gated material profile to terrain classifier.
    // Default 0 = LEGACY = exact pre-C1 byte-for-byte rendering.
    if (tl.terrainMaterialProfile >= 0) glUniform1i(tl.terrainMaterialProfile, g_terrainMaterialProfile);
    if (tl.pomParams >= 0)            glUniform4fv(tl.pomParams, 1, pomP);
    if (tl.terrainWorldScale >= 0)    glUniform4fv(tl.terrainWorldScale, 1, worldScaleV);
    if (tl.cellBombParams >= 0)       glUniform4fv(tl.cellBombParams, 1, cellP);
    if (tl.matNormalBoost >= 0)       glUniform4fv(tl.matNormalBoost, 1, terrain_mat_normal_boost_);
    if (tl.matTiling >= 0)            glUniform4fv(tl.matTiling, 1, terrain_mat_tiling_);
    if (tl.matTilingSnow >= 0)        glUniform1f(tl.matTilingSnow, terrain_mat_tiling_snow_);
    if (tl.tintStrengthScale >= 0)    glUniform1f(tl.tintStrengthScale, terrain_tint_strength_scale_);
    if (tl.snowBrightnessDampen >= 0) glUniform1f(tl.snowBrightnessDampen, terrain_snow_brightness_dampen_);
    if (tl.tintRock  >= 0)            glUniform3fv(tl.tintRock,  1, terrain_tint_rock_);
    if (tl.tintGrass >= 0)            glUniform3fv(tl.tintGrass, 1, terrain_tint_grass_);
    if (tl.tintDirt  >= 0)            glUniform3fv(tl.tintDirt,  1, terrain_tint_dirt_);
    if (tl.terrainClassGrass >= 0)    glUniform4fv(tl.terrainClassGrass, 1, terrain_class_grass_);
    if (tl.terrainClassDirt  >= 0)    glUniform4fv(tl.terrainClassDirt,  1, terrain_class_dirt_);
    // TERRAIN-MATERIAL-LIB-1: promoted tints always upload (they replace what
    // were frag literals, so there is no gate-OFF skip for these -- the shader
    // now always reads a uniform, and the default member value is the exact
    // former literal). Roughness/AO + the branch flag ARE gated (byte-identity
    // requires the u_useMaterialLib==0 branch to be taken by default).
    if (tl.tintConcrete >= 0)         glUniform3fv(tl.tintConcrete, 1, terrain_tint_concrete_);
    if (tl.tintSnow     >= 0)         glUniform3fv(tl.tintSnow,     1, terrain_tint_snow_);
    if (tl.matRoughness  >= 0)        glUniform4fv(tl.matRoughness, 1, terrain_mat_roughness_);
    if (tl.matAO         >= 0)        glUniform4fv(tl.matAO,        1, terrain_mat_ao_);
    if (tl.useMaterialLib >= 0)       glUniform1i(tl.useMaterialLib, terrainMaterialLibEnabled() ? 1 : 0);
    // TERRAIN-CONTROLMAP-ALBEDO-1: gate default OFF -> member stays 0.0f ->
    // uploaded verbatim -> frag's mix(x,1.0,0.0)==x (byte-identical).
    if (tl.controlAlbedoStrength >= 0) glUniform1f(tl.controlAlbedoStrength, terrain_control_albedo_strength_);
    if (tl.time >= 0) {
        float elapsed = SmokeMode::fixedTimestepEnabled()
                        ? (float)SmokeMode::fixedClockSeconds()
                        : (float)(timing::get_wall_time_ms() - timeStart_) / 1000.0f;
        glUniform1f(tl.time, elapsed);
    }

    if (terrainNormalArrayEnabled()) {
        if (terrain_normal_array_dirty_) buildTerrainNormalArray();
        if (terrain_normal_array_tex_ != 0 && tl.matNormalArray >= 0) {
            glUniform1i(tl.matNormalArray, kTerrainTexUnitNormalArray);
            glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitNormalArray);
            glBindTexture(GL_TEXTURE_2D_ARRAY, terrain_normal_array_tex_);
        }
    } else {
        for (int i = 0; i < 5; i++) {
            if (terrain_mat_normal_[i] != 0 && tl.matNormal[i] >= 0) {
                glUniform1i(tl.matNormal[i], kTerrainMatNormalUnits[i]);
                glActiveTexture(GL_TEXTURE0 + kTerrainMatNormalUnits[i]);
                glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[i]);
            }
        }
    }
    bindTerrainHeightTexUniforms(tl.terrainHeightTex, tl.terrainHeightParams,
                                 tl.terrainCliffShadowFloor,
                                 terrain_cliff_shadow_floor_);
    glActiveTexture(GL_TEXTURE0);

    gosPostProcess* pp = getGosPostProcess();
    if (pp && pp->shadowsEnabled_) {
        if (tl.lightSpaceMatrix >= 0) glUniformMatrix4fv(tl.lightSpaceMatrix, 1, GL_FALSE, pp->getLightSpaceMatrix());
        if (tl.enableShadows >= 0)    glUniform1i(tl.enableShadows, 1);
        if (tl.shadowSoftness >= 0)   glUniform1f(tl.shadowSoftness, terrain_shadow_softness_);
        if (tl.shadowMap >= 0) {
            glUniform1i(tl.shadowMap, kTerrainTexUnitStaticShadow);
            glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitStaticShadow);
            glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture());
            glActiveTexture(GL_TEXTURE0);
        }
        if (pp->getDynamicShadowFBO()) {
            uploadDynamicShadowUniforms(tl, pp, kTerrainTexUnitDynamicShadow);
        } else {
            if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 0);
        }
    } else {
        if (tl.enableShadows >= 0)        glUniform1i(tl.enableShadows, 0);
        if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 0);
    }
}

int gosRenderer::terrainBindThinUniformsForPatchStream(glsl_program* overrideProg)
{
    glsl_program* target = overrideProg ? overrideProg : thin_terrain_prog_;
    if (!target || !target->shp_) return -1;
    GLuint shp = target->shp_;
    glUseProgram(shp);
    cacheThinTerrainUniformLocations(shp);
    const auto& tl = thinTerrainLocs_;

    if (terrain_mvp_valid_ && tl.terrainMVP >= 0)
        glUniformMatrix4fv(tl.terrainMVP, 1, GL_FALSE, (const float*)&terrain_mvp_);
    // projection_: row-major Stuff matrix — upload GL_TRUE (column-major interpretation).
    // All other projection_ upload sites (shadow line ~2737, material setTransform) use GL_TRUE.
    // terrainMVP stays GL_FALSE — its D3D chain math cancels the implicit transpose.
    if (tl.mvp >= 0)
        glUniformMatrix4fv(tl.mvp, 1, GL_TRUE, (const float*)&projection_);

    static const bool s_thinDebug   = mc2::gate::get<"MC2_THIN_DEBUG">();
    static bool       s_thinDbgDone = false;
    if (s_thinDebug && !s_thinDbgDone && terrain_mvp_valid_) {
        s_thinDbgDone = true;
        const float* p = (const float*)&projection_;
        const float* m = (const float*)&terrain_mvp_;
        fprintf(stderr,
            "[THIN_DEBUG v1] event=thin_uniforms_bound "
            "proj_row0=[%.5f,%.5f,%.5f,%.5f] proj_row1=[%.5f,%.5f,%.5f,%.5f] "
            "tmvp_diag=[%.5f,%.5f,%.5f,%.5f]\n",
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
            m[0], m[5], m[10], m[15]);
        fflush(stderr);
    }

    if (tl.cameraPos >= 0)        glUniform4fv(tl.cameraPos, 1, (const float*)&terrain_camera_pos_);
    if (tl.terrainLightDir >= 0)  glUniform4fv(tl.terrainLightDir, 1, (const float*)&terrain_light_dir_);
    // tessDebug: matches non-thin path (line ~4080). The thin path historically
    // omitted this — debug-viz modes 1..8 in gos_terrain.frag therefore could
    // not fire on the indirect/substrate draw, which silently broke the entire
    // shader-debug channel for the path that needs it most.  Env override
    // mirrors the tessellated path so MC2_TERRAIN_DEBUG_MODE works uniformly.
    {
        float debugMode = terrain_debug_mode_;
        if (mc2::gate::isSet<"MC2_TERRAIN_DEBUG_MODE">())
            debugMode = mc2::gate::get<"MC2_TERRAIN_DEBUG_MODE">();
        { int lvm = mc2LightingDebugMode(); if (lvm >= 0) debugMode = (float)lvm; }  // LIGHTING-DEBUG-VIEWS-1A
        float tessDebugVec[4] = { debugMode, 0.0f, 0.0f, 0.0f };
        if (tl.tessDebug >= 0) glUniform4fv(tl.tessDebug, 1, tessDebugVec);
        if (tl.pathTint >= 0)  glUniform1i(tl.pathTint, mc2ShaderPathTint());  // MC2_SHADER_PATH_TINT
    }
    if (tl.mapHalfExtent >= 0) {
        gosPostProcess* pp = getGosPostProcess();
        float halfExt = pp ? pp->getMapHalfExtent() : 0.0f;
        glUniform1f(tl.mapHalfExtent, halfExt);
    }
    float tiling[4]      = { terrain_detail_tiling_, 0.0f, 0.0f, 0.0f };
    const float* _datYZW = mc2_detailAntiTileYZW();  // TERRAIN-DETAIL-ANTI-TILE-1 (yzw=0 when gate OFF)
    float strength[4]    = { terrain_detail_strength_, _datYZW[0], _datYZW[1], _datYZW[2] };
    float pomP[4]        = { terrain_pom_scale_, 8.0f, 32.0f, 0.0f };
    float worldScaleV[4] = { terrain_world_scale_, 0.0f, 0.0f, 0.0f };
    float cellP[4]       = { terrain_cell_scale_, terrain_cell_jitter_, terrain_cell_rotation_, 0.0f };
    if (tl.detailNormalTiling >= 0)   glUniform4fv(tl.detailNormalTiling, 1, tiling);
    if (tl.detailNormalStrength >= 0) glUniform4fv(tl.detailNormalStrength, 1, strength);
    // C1 tactical: push mission-gated material profile to terrain classifier.
    // Default 0 = LEGACY = exact pre-C1 byte-for-byte rendering.
    if (tl.terrainMaterialProfile >= 0) glUniform1i(tl.terrainMaterialProfile, g_terrainMaterialProfile);
    if (tl.pomParams >= 0)            glUniform4fv(tl.pomParams, 1, pomP);
    if (tl.terrainWorldScale >= 0)    glUniform4fv(tl.terrainWorldScale, 1, worldScaleV);
    if (tl.cellBombParams >= 0)       glUniform4fv(tl.cellBombParams, 1, cellP);
    if (tl.matNormalBoost >= 0)       glUniform4fv(tl.matNormalBoost, 1, terrain_mat_normal_boost_);
    if (tl.matTiling >= 0)            glUniform4fv(tl.matTiling, 1, terrain_mat_tiling_);
    if (tl.matTilingSnow >= 0)        glUniform1f(tl.matTilingSnow, terrain_mat_tiling_snow_);
    if (tl.tintStrengthScale >= 0)    glUniform1f(tl.tintStrengthScale, terrain_tint_strength_scale_);
    if (tl.snowBrightnessDampen >= 0) glUniform1f(tl.snowBrightnessDampen, terrain_snow_brightness_dampen_);
    if (tl.tintRock  >= 0)            glUniform3fv(tl.tintRock,  1, terrain_tint_rock_);
    if (tl.tintGrass >= 0)            glUniform3fv(tl.tintGrass, 1, terrain_tint_grass_);
    if (tl.tintDirt  >= 0)            glUniform3fv(tl.tintDirt,  1, terrain_tint_dirt_);
    if (tl.terrainClassGrass >= 0)    glUniform4fv(tl.terrainClassGrass, 1, terrain_class_grass_);
    if (tl.terrainClassDirt  >= 0)    glUniform4fv(tl.terrainClassDirt,  1, terrain_class_dirt_);
    // TERRAIN-CONTROLMAP-ALBEDO-1: loc stays -1 on this (dead thin/legacy)
    // path today; guarded upload is a no-op until gos_terrain.frag declares
    // the uniform too.
    if (tl.controlAlbedoStrength >= 0) glUniform1f(tl.controlAlbedoStrength, terrain_control_albedo_strength_);
    if (tl.time >= 0) {
        float elapsed = SmokeMode::fixedTimestepEnabled()
                        ? (float)SmokeMode::fixedClockSeconds()
                        : (float)(timing::get_wall_time_ms() - timeStart_) / 1000.0f;
        glUniform1f(tl.time, elapsed);
    }
    if (tl.tex1 >= 0) glUniform1i(tl.tex1, 0);
    if (terrainNormalArrayEnabled()) {
        if (terrain_normal_array_dirty_) buildTerrainNormalArray();
        if (terrain_normal_array_tex_ != 0 && tl.matNormalArray >= 0) {
            glUniform1i(tl.matNormalArray, kTerrainTexUnitNormalArray);
            glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitNormalArray);
            glBindTexture(GL_TEXTURE_2D_ARRAY, terrain_normal_array_tex_);
        }
    } else {
        for (int i = 0; i < 5; i++) {
            if (terrain_mat_normal_[i] != 0 && tl.matNormal[i] >= 0) {
                glUniform1i(tl.matNormal[i], kTerrainMatNormalUnits[i]);
                glActiveTexture(GL_TEXTURE0 + kTerrainMatNormalUnits[i]);
                glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[i]);
            }
        }
    }
    bindTerrainHeightTexUniforms(tl.terrainHeightTex, tl.terrainHeightParams,
                                 tl.terrainCliffShadowFloor,
                                 terrain_cliff_shadow_floor_);
    glActiveTexture(GL_TEXTURE0);

    gosPostProcess* pp = getGosPostProcess();
    if (pp && pp->shadowsEnabled_) {
        if (tl.lightSpaceMatrix >= 0)
            glUniformMatrix4fv(tl.lightSpaceMatrix, 1, GL_FALSE, pp->getLightSpaceMatrix());
        if (tl.enableShadows >= 0)  glUniform1i(tl.enableShadows, 1);
        if (tl.shadowSoftness >= 0) glUniform1f(tl.shadowSoftness, terrain_shadow_softness_);
        if (tl.shadowMap >= 0) {
            glUniform1i(tl.shadowMap, kTerrainTexUnitStaticShadow);
            glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitStaticShadow);
            glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture());
            glActiveTexture(GL_TEXTURE0);
        }
        if (pp->getDynamicShadowFBO()) {
            uploadDynamicShadowUniforms(tl, pp, kTerrainTexUnitDynamicShadow);
        } else {
            if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 0);
        }
    } else {
        if (tl.enableShadows >= 0)        glUniform1i(tl.enableShadows, 0);
        if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 0);
    }

    return tl.ssboRecordBase;
}

void __stdcall gos_SetTerrainTessParams(float level, float near_dist, float far_dist) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTessParams(level, near_dist, far_dist);
}
void __stdcall gos_SetTerrainPhongAlpha(float a) {
    if (g_gos_renderer) g_gos_renderer->setTerrainPhongAlpha(a);
}
void __stdcall gos_SetTerrainDisplaceScale(float s) {
    if (g_gos_renderer) g_gos_renderer->setTerrainDisplaceScale(s);
}
float gos_GetTerrainPhongAlpha() {
    return g_gos_renderer ? g_gos_renderer->getTerrainPhongAlpha() : 0.0f;
}
float gos_GetTerrainDisplaceScale() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDisplaceScale() : 0.0f;
}
float gos_GetTerrainDetailTiling() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDetailTiling() : 1.0f;
}
float gos_GetTerrainDetailStrength() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDetailStrength() : 4.0f;
}
float gos_GetTerrainPOMScale() {
    return g_gos_renderer ? g_gos_renderer->getTerrainPOMScale() : 0.0f;
}
void __stdcall gos_SetTerrainWireframe(bool w) {
    if (g_gos_renderer) g_gos_renderer->setTerrainWireframe(w);
}
void __stdcall gos_SetTerrainDebugMode(float mode) {
    if (g_gos_renderer) g_gos_renderer->setTerrainDebugMode(mode);
}
float __stdcall gos_GetTerrainDebugMode() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDebugMode() : 0.0f;
}
void __stdcall gos_TerrainExtraReset() {
    if (g_gos_renderer) g_gos_renderer->terrainExtraReset();
}
void __stdcall gos_TerrainExtraAdd(const gos_TERRAIN_EXTRA* data, int count) {
    if (g_gos_renderer) g_gos_renderer->terrainExtraAdd(data, count);
}
void __stdcall gos_SetTerrainBatchExtras(const gos_TERRAIN_EXTRA* extras, int count) {
    if (g_gos_renderer) g_gos_renderer->setTerrainBatchExtras(extras, count);
}
bool __stdcall gos_IsTerrainTessellationActive() {
    return g_gos_renderer && g_gos_renderer->getTerrainMaterial() != nullptr;
}
// ISSUE-34: "modern shadow/render path active" — true when the shadow_terrain
// material loaded. Shadow-pass builds (txmmgr.cpp) and the blob-shadow
// suppression / GPU transform skips (mech3d.cpp, gvactor.cpp) key on THIS,
// not on the tessellation material: when tess is off (TCS/TES compile failure
// or MC2_TERRAIN_TESS=0) CSM/static shadows stay on and the legacy pz-gated
// CPU blob shadows stay off, keeping near-plane clipping behavior uniform.
bool __stdcall gos_IsModernShadowPathActive() {
    return g_gos_renderer && g_gos_renderer->getShadowTerrainMaterial() != nullptr;
}

void gos_SetShadowMode(bool enable) {
    if (g_gos_renderer) g_gos_renderer->setShadowMode(enable);
}

void gos_GetTerrainCameraPos(float* x, float* y, float* z) {
    if (g_gos_renderer) {
        const vec4& cp = g_gos_renderer->getTerrainCameraPos();
        if (x) *x = cp.x;
        if (y) *y = cp.y;
        if (z) *z = cp.z;
    }
}
// F1 Stage A unified-projection production setter.
// Repackages column-major Stuff::Matrix4D -> row-major M (matching
// the legacy upload convention so all consumers see the same
// shader-visible matrix orientation). Writes the terrain_mvp_ cache
// for 15+ existing gos_GetTerrainMVPMat4() callers (CullUBO at
// gpu_cull_compute.cpp:831, mech-batcher, static-prop-batcher,
// etc.) to inherit transparently. The 10 CPU bind sites (Task 15)
// each upload the cached matrix to their respective programs via
// glUniformMatrix4fv(loc, 1, GL_FALSE, terrain_mvp_).
//
// Signature is parameter-light — no explicit program handle —
// because per-program upload happens at the 10 bind sites via the
// cache, not here. R-clipw polarity is baked into the matrix
// itself via kAxisSwapMC2toGL (Task 7g); shader-visible clip.w > 0
// for in-front MC2 verts.
// [MVP_DIAG v1] S2.7 — shared frame counter incremented at the
// canonical world-to-clip set site. Read by gos_GetTerrainMVPMat4
// and the static-prop coalesce uploader (extern in
// gos_static_prop_batcher.cpp). Kept past S2.7 because every probe
// is throttled (frames {1,5,30,120}) — silent at steady state.
long g_mvpDiagFrame = 0;

// RENDER-VIEW-CURRENCY-1 (M2): object/mech stale-snapshot-read telemetry. Bumped
// by gos_GetObjectDrawMVP() (gos_object_draw_mvp.h) whenever an armed object draw
// requested the dispatch snapshot but its view epoch did not match the current
// view (it fell back to the live MVP instead). Should be 0 in steady state; a
// rising count localizes a future re-introduction of the BUG1/3/5 staleness. The
// counter is read by the debug-state dump. MC2_OBJ_MVP_STALE_FATAL=1 aborts so CI
// catches it instead of a player on NVIDIA three days later.
static unsigned long g_objMvpStaleCount = 0;
unsigned long gos_object_mvp_stale_count() { return g_objMvpStaleCount; }
// VIEW-EPOCH-DEDUPE-1: counts the snapshot-USED path (object draw projected through
// the depth-matched dispatch snapshot = the FixB z-fight fix active). Paired with
// the stale counter it proves the fix is live, not merely "fewer fallbacks".
static unsigned long g_objMvpUsedCount = 0;
unsigned long gos_object_mvp_used_count() { return g_objMvpUsedCount; }
void gos_object_mvp_note_used() { ++g_objMvpUsedCount; }

// RENDER-FRAME-CONTEXT-1 (M-guard): counts divergences between the read-only
// gos_FrameCtx() mirror and the authoritative globals. No-op by construction in
// the additive slice (the mirror reads those globals); becomes load-bearing when
// the authority-inversion slice makes the globals shims. MC2_FRAMECTX_MISMATCH_FATAL=1
// aborts so CI catches a desync instead of it going silent.
static unsigned long g_framectxMismatchCount = 0;
unsigned long gos_framectx_mismatch_count() { return g_framectxMismatchCount; }
void gos_framectx_note_mismatch(const char* what) {
    ++g_framectxMismatchCount;
    static const bool s_fatal = mc2::gate::get<"MC2_FRAMECTX_MISMATCH_FATAL">();
    if (s_fatal) {
        fprintf(stderr, "[FRAMECTX] FATAL: frame-context mirror diverged from "
                        "authoritative global (%s); count=%lu\n",
                        what ? what : "?", g_framectxMismatchCount);
        fflush(stderr);
        abort();
    }
}
void gos_object_mvp_note_stale() {
    ++g_objMvpStaleCount;
    static const bool s_fatal = mc2::gate::get<"MC2_OBJ_MVP_STALE_FATAL">();
    if (s_fatal) {
        fprintf(stderr, "[OBJ_MVP] FATAL: object draw read a stale-view dispatch "
                        "MVP snapshot (view-epoch mismatch); count=%lu\n",
                        g_objMvpStaleCount);
        fflush(stderr);
        abort();
    }
}

// VIEW-EPOCH-DEDUPE-1: semantic VIEW-CONTENT epoch. g_mvpDiagFrame is a raw publish
// counter that bumps on EVERY gos_SetWorldToClipGL (~2x/frame: the early-publish at
// mission.cpp + the gamecam publish, usually the SAME camera). The object/mech MVP
// currency check must NOT treat a redundant same-camera republish as a view change,
// or it false-stales the dispatch snapshot every frame and silently disables the
// FixB z-fight fix (telemetry: stale_mvp_reads ~= frame count). This epoch bumps
// ONLY when the published world-to-clip CONTENT actually changes; real camera motion
// still advances it (so vanish/flicker stays fixed). Consumed via gos_object_draw_mvp.h.
long g_viewContentEpoch = 0;
static float g_prevPublishedWtc[16] = { 0 };
static bool  g_havePrevPublishedWtc = false;

void __stdcall gos_SetWorldToClipGL(const Stuff::Matrix4D& mat)
{
    if (!g_gos_renderer) return;
    const float* col = (const float*)&mat;
    #define WTC(r,c) col[(c)*4 + (r)]
    // Repackage column-major Stuff -> row-major M (same convention as
    // the legacy terrain upload; consumers see GL_FALSE-interpretation
    // transpose, R-clipw polarity baked into kAxisSwapMC2toGL).
    float M[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            M[i*4 + j] = WTC(i, j);
    #undef WTC
    g_gos_renderer->setTerrainMVP(M);

    // [MVP_DIAG v1] S2.7 — log set_mvp entry. Throttled to frames
    // {1,5,30,120}. row0 is M[0..3] AFTER the column->row repack.
    ++g_mvpDiagFrame;

    // VIEW-EPOCH-DEDUPE-1: advance the semantic view-content epoch only on a real
    // matrix change. Identical same-camera republishes (early-publish then gamecam)
    // leave it unchanged, so the dispatch snapshot is not false-staled and object draw
    // keeps the depth-matched snapshot. Logic lives in the pure kernel so the offline
    // unit harness (tests/unit/test_view_currency.cpp) verifies the SAME code.
    {
        static const float kViewEpsilon = 1e-6f;  // ignore redundant republish, not real motion
        if (mc2::view_currency::viewContentChanged(g_prevPublishedWtc, M,
                                                   g_havePrevPublishedWtc, kViewEpsilon))
            ++g_viewContentEpoch;
        for (int i = 0; i < 16; ++i) g_prevPublishedWtc[i] = M[i];
        g_havePrevPublishedWtc = true;
    }
    if (g_mvpDiagFrame == 1 || g_mvpDiagFrame == 5 ||
        g_mvpDiagFrame == 30 || g_mvpDiagFrame == 120) {
        MC2_DIAGFE(stderr,
                "[MVP_DIAG v1] event=set_mvp frame=%ld renderer=%p row0=[%g %g %g %g]\n",
                g_mvpDiagFrame, (void*)g_gos_renderer,
                M[0], M[1], M[2], M[3]);
        fflush(stderr);
    }
}
// gos_SetTerrainMVP / gos_SetTerrainViewport: raw-float counterparts to
// gos_SetWorldToClipGL. Used by EditorCamera.h which builds the matrix
// via axis-swap math rather than going through Stuff::Matrix4D. Both APIs
// write the same terrain_mvp_ cache in the renderer.
void __stdcall gos_SetTerrainMVP(const float* matrix16) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMVP(matrix16);
}
void __stdcall gos_SetTerrainViewport(float vmx, float vmy, float vax, float vay) {
    if (g_gos_renderer) g_gos_renderer->setTerrainViewport(vmx, vmy, vax, vay);
}
void __stdcall gos_SetTerrainCameraPos(float x, float y, float z) {
    if (g_gos_renderer) g_gos_renderer->setTerrainCameraPos(x, y, z);
}
// Static shadow API — world-fixed shadow map rendered once at map load
void gos_SetMapHalfExtent(float halfExtent) {
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->setMapHalfExtent(halfExtent);
}
void gos_SetWaterElevation(float elevation) {
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->setWaterElevation(elevation);
}
bool gos_StaticLightMatrixBuilt() {
    gosPostProcess* pp = getGosPostProcess();
    return pp && pp->staticLightMatrixBuilt();
}
void gos_BuildStaticLightMatrix() {
    gosPostProcess* pp = getGosPostProcess();
    if (!pp || !pp->shadowsEnabled_) return;
    float lx = 0, ly = 0, lz = 0;
    gos_GetTerrainLightDir(&lx, &ly, &lz);
    // Negate: lightDir points scene→sun, but matrix needs light→scene
    pp->buildStaticLightMatrix(-lx, -ly, -lz, pp->getMapHalfExtent());
}
void gos_MarkStaticLightMatrixBuilt() {
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->markStaticLightMatrixBuilt();
}

void gos_GetTerrainLightDir(float* x, float* y, float* z) {
    if (g_gos_renderer) {
        const vec4& ld = g_gos_renderer->getTerrainLightDir();
        if (x) *x = ld.x;
        if (y) *y = ld.y;
        if (z) *z = ld.z;
    }
}
void gos_SetTerrainLightDir(float x, float y, float z) {
    if (g_gos_renderer) g_gos_renderer->setTerrainLightDir(x, y, z);
}

// EDGE-LIGHT-EMISSION-1: night-state bridge for the GPU static-prop path.
//
// THE BUG THIS EXISTS TO FIX. Stock MC2 lights runway/taxiway edge lights and
// beacons through the authored "hot colour magic" vertex tags: every one of
// data/tgl/{runway,red,green,blue}beaconlight.ase paints its post base pure
// green (0xff00ff00 = "Hot Green -- Building Base Lights -- ONLY at NIGHT"),
// and each fixture's .ini carries a `//Night Lights` block giving the colour
// to emit (runwaylight/redbeaconlight 0x00ff0000 red, greenbeaconlight
// 0x0000ff00, bluebeaconlight 0x000000ff). The legacy CPU path
// (mclib/tgl.cpp:1901) reads eye->getIsNight()/getNightFactor() and honours
// those branches. The GPU static-prop batcher path -- which is the path these
// props ACTUALLY take at runtime -- passed `false, 0.0` hardcoded into
// get_base_light() (static_prop.vert, "stubbed for now"), so every night-only
// branch evaluated its DAYTIME arm and the emission was simply never applied.
// The lights were switched off. This restores the dropped stock state; it is
// not a new visual treatment.
//
// Push model mirrors gos_SetTerrainLightDir / gos_SetTerrainMissionFlatLight:
// gamecam.cpp feeds it once per frame, GameOS reads it at uniform-upload time.
static bool  s_gosIsNight     = false;
static float s_gosNightFactor = 0.0f;

void gos_SetNightState(bool isNight, float nightFactor) {
    s_gosIsNight     = isNight;
    if (nightFactor < 0.0f) nightFactor = 0.0f;
    if (nightFactor > 1.0f) nightFactor = 1.0f;
    s_gosNightFactor = nightFactor;
}
void gos_GetNightState(bool* isNight, float* nightFactor) {
    if (isNight)     *isNight     = s_gosIsNight;
    if (nightFactor) *nightFactor = s_gosNightFactor;
}

// TERRAIN-AMBIENT-FLOOR-2: daylight level in [0,1] for the terrain ambient
// floor, pushed from gamecam.cpp once per frame in the same place the night
// state is pushed. The DECISION lives in GameOS/gameos/utils/terrain_daylight.h
// (pure, harness-executed); this is only the transport.
//
// Deliberately NOT derived here from s_gosIsNight/s_gosNightFactor alone.
//
// CORRECTED 2026-07-30 (DAYLIGHT-GATE-RECHECK-1). This comment used to justify
// that by claiming Camera::update() re-derives the flags "AFTER camera.cpp:2033
// has folded a below-horizon authored pitch into a moon pitch -- so on mc2_05
// (authored 270) they report full day with the light overhead." That is FALSE.
// The fold is at camera.cpp:2079-2080 inside updateDaylight(), which never runs
// in a mission (GameCamera::activate overrides the virtual base without chaining
// -- code/gamecam.cpp:1024, its own call commented at :1060; per-frame site
// commented at camera.cpp:2238). On mc2_05 lightPitch stays 270, so
// Camera::update() (camera.cpp:2254-2281) gets altitude -90 and correctly
// reports isNight = true, nightFactor = 1.
//
// The decision to key on the AUTHORED pitch still stands, for a sounder reason:
// dayLightPitch is written once at load and is never folded or animated, so it
// stays correct even if the fold is ever revived, whereas the runtime flags are
// one commented-out line away from being wrong. The gate takes the MIN of both,
// which fails dark on disagreement. See the header for the measured evidence.
//
// Default 0.0 = fully night = the floor is OFF until something feeds it. A
// missing feed can only make this change a no-op, never a bright regression.
static float s_gosTerrainDaylight = 0.0f;

void gos_SetTerrainDaylightLevel(float level) {
    if (!(level >= 0.0f)) level = 0.0f;   // also catches NaN
    if (level > 1.0f)     level = 1.0f;
    s_gosTerrainDaylight = level;
}
float gos_GetTerrainDaylightLevel() {
    return s_gosTerrainDaylight;
}

// CEMENT-NIGHT-LIGHT-1: mission flat-pad light colour feed (gamecam.cpp, per
// frame). Killswitch MC2_CEMENT_MISSION_LIGHT=0 -> force (1,1,1) (repro of the
// old bright-pads-at-night behaviour); default ON.
void gos_SetTerrainMissionFlatLight(float r, float g, float b) {
    static const bool s_enabled = mc2::gate::get<"MC2_CEMENT_MISSION_LIGHT">();
    if (!s_enabled) { r = 1.0f; g = 1.0f; b = 1.0f; }
    if (g_gos_renderer) g_gos_renderer->setTerrainMissionFlatLight(r, g, b);
}
void gos_GetTerrainMissionFlatLight(float* r, float* g, float* b) {
    float rr = 1.0f, gg = 1.0f, bb = 1.0f;
    if (g_gos_renderer) {
        const vec4& fl = g_gos_renderer->getTerrainMissionFlatLight();
        rr = fl.x; gg = fl.y; bb = fl.z;
    }
    if (r) *r = rr;
    if (g) *g = gg;
    if (b) *b = bb;
}
void gos_SetTerrainDetailParams(float tiling, float strength) {
    if (g_gos_renderer) g_gos_renderer->setTerrainDetailParams(tiling, strength);
}
void gos_SetTerrainMaterialNormal(int index, unsigned int glTexId) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMaterialNormal(index, glTexId);
}
void gos_SetTerrainWorldScale(float scale) {
    if (g_gos_renderer) g_gos_renderer->setTerrainWorldScale(scale);
}
void gos_SetTerrainCellBombParams(float scale, float jitter, float rotation) {
    if (g_gos_renderer) g_gos_renderer->setTerrainCellBombParams(scale, jitter, rotation);
}
void gos_SetTerrainPOMParams(float scale, float minLayers, float maxLayers) {
    if (g_gos_renderer) g_gos_renderer->setTerrainPOMParams(scale, minLayers, maxLayers);
}
void gos_SetTerrainDetailNormalTexture(unsigned int glTexId) {
    // Legacy single-texture path — not used with per-material splatting, but terrtxm2 still calls it
}
void gos_SetTerrainDisplacementTexture(unsigned int glTexId) {
    // Legacy single displacement — not used with per-material splatting
}
void gos_SetTerrainViewDir(float x, float y, float z) {
    // View direction for POM — stored but not critical for tessellation path
}
unsigned int gos_CreateTerrainNormalTexture(const unsigned char* rgbaData, int width) {
    GLuint texId = 0;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, width, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbaData);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    printf("[TESS] Created terrain normal texture: id=%u size=%d\n", texId, width);
    return texId;
}

void gos_SetTerrainShadowSoftness(float s) {
    if (g_gos_renderer) g_gos_renderer->setTerrainShadowSoftness(s);
}
float gos_GetTerrainShadowSoftness() {
    return g_gos_renderer ? g_gos_renderer->getTerrainShadowSoftness() : 2.5f;
}

void gos_SetTerrainMatNormalBoost(float rock, float grass, float dirt, float concrete) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMatNormalBoost(rock, grass, dirt, concrete);
}
void gos_GetTerrainMatNormalBoost(float* rock, float* grass, float* dirt, float* concrete) {
    if (g_gos_renderer) g_gos_renderer->getTerrainMatNormalBoost(rock, grass, dirt, concrete);
    else { *rock = 0.9f; *grass = 1.1f; *dirt = 1.1f; *concrete = 2.5f; }
}
void gos_SetTerrainMatTiling(float rock, float grass, float dirt, float concrete, float snow) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMatTiling(rock, grass, dirt, concrete, snow);
}
void gos_GetTerrainMatTiling(float* rock, float* grass, float* dirt, float* concrete, float* snow) {
    if (g_gos_renderer) g_gos_renderer->getTerrainMatTiling(rock, grass, dirt, concrete, snow);
    else { *rock = 3.0f; *grass = 2.0f; *dirt = 1.0f; *concrete = 6.0f; *snow = 1.0f; }
}
void gos_SetTerrainClassGrass(float gMinusRLo, float gMinusRHi, float gBrightLo, float gBrightHi) {
    if (!g_gos_renderer) return;
    const float v[4] = { gMinusRLo, gMinusRHi, gBrightLo, gBrightHi };
    g_gos_renderer->setTerrainClassGrass(v);
}
void gos_GetTerrainClassGrass(float* gMinusRLo, float* gMinusRHi, float* gBrightLo, float* gBrightHi) {
    if (g_gos_renderer) {
        float v[4]; g_gos_renderer->getTerrainClassGrass(v);
        *gMinusRLo = v[0]; *gMinusRHi = v[1]; *gBrightLo = v[2]; *gBrightHi = v[3];
    } else { *gMinusRLo = -0.02f; *gMinusRHi = 0.06f; *gBrightLo = 0.22f; *gBrightHi = 0.40f; }
}
void gos_SetTerrainClassDirt(float rMinusGLo, float rMinusGHi, float rBrightLo, float rBrightHi) {
    if (!g_gos_renderer) return;
    const float v[4] = { rMinusGLo, rMinusGHi, rBrightLo, rBrightHi };
    g_gos_renderer->setTerrainClassDirt(v);
}
void gos_GetTerrainClassDirt(float* rMinusGLo, float* rMinusGHi, float* rBrightLo, float* rBrightHi) {
    if (g_gos_renderer) {
        float v[4]; g_gos_renderer->getTerrainClassDirt(v);
        *rMinusGLo = v[0]; *rMinusGHi = v[1]; *rBrightLo = v[2]; *rBrightHi = v[3];
    } else { *rMinusGLo = -0.02f; *rMinusGHi = 0.06f; *rBrightLo = 0.22f; *rBrightHi = 0.45f; }
}
void gos_SetTerrainTintStrengthScale(float s) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTintStrengthScale(s);
}
float gos_GetTerrainTintStrengthScale() {
    return g_gos_renderer ? g_gos_renderer->getTerrainTintStrengthScale() : 1.0f;
}
void gos_SetTerrainControlAlbedoStrength(float s) {
    if (g_gos_renderer) g_gos_renderer->setTerrainControlAlbedoStrength(s);
}
float gos_GetTerrainControlAlbedoStrength() {
    return g_gos_renderer ? g_gos_renderer->getTerrainControlAlbedoStrength() : 0.0f;
}
void gos_SetTerrainSnowBrightnessDampen(float v) {
    if (g_gos_renderer) g_gos_renderer->setTerrainSnowBrightnessDampen(v);
}
float gos_GetTerrainSnowBrightnessDampen() {
    return g_gos_renderer ? g_gos_renderer->getTerrainSnowBrightnessDampen() : 0.78f;
}
void gos_SetTerrainTintRock(float r, float g, float b) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTintRock(r, g, b);
}
void gos_GetTerrainTintRock(float* r, float* g, float* b) {
    if (g_gos_renderer) g_gos_renderer->getTerrainTintRock(r, g, b);
    else { *r = 0.36f; *g = 0.37f; *b = 0.40f; }
}
void gos_SetTerrainTintGrass(float r, float g, float b) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTintGrass(r, g, b);
}
void gos_GetTerrainTintGrass(float* r, float* g, float* b) {
    if (g_gos_renderer) g_gos_renderer->getTerrainTintGrass(r, g, b);
    else { *r = 0.35f; *g = 0.42f; *b = 0.25f; }
}
void gos_SetTerrainTintDirt(float r, float g, float b) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTintDirt(r, g, b);
}
void gos_GetTerrainTintDirt(float* r, float* g, float* b) {
    if (g_gos_renderer) g_gos_renderer->getTerrainTintDirt(r, g, b);
    else { *r = 0.48f; *g = 0.42f; *b = 0.33f; }
}
void gos_SetTerrainTintConcrete(float r, float g, float b) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTintConcrete(r, g, b);
}
void gos_GetTerrainTintConcrete(float* r, float* g, float* b) {
    if (g_gos_renderer) g_gos_renderer->getTerrainTintConcrete(r, g, b);
    else { *r = 0.55f; *g = 0.53f; *b = 0.50f; }
}
void gos_SetTerrainTintSnow(float r, float g, float b) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTintSnow(r, g, b);
}
void gos_GetTerrainTintSnow(float* r, float* g, float* b) {
    if (g_gos_renderer) g_gos_renderer->getTerrainTintSnow(r, g, b);
    else { *r = 0.75f; *g = 0.78f; *b = 0.84f; }
}
void gos_SetTerrainMatRoughness(float rock, float grass, float dirt, float concrete) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMatRoughness(rock, grass, dirt, concrete);
}
void gos_GetTerrainMatRoughness(float* rock, float* grass, float* dirt, float* concrete) {
    if (g_gos_renderer) g_gos_renderer->getTerrainMatRoughness(rock, grass, dirt, concrete);
    else { *rock = 1.0f; *grass = 1.0f; *dirt = 1.0f; *concrete = 1.0f; }
}
void gos_SetTerrainMatAO(float rock, float grass, float dirt, float concrete) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMatAO(rock, grass, dirt, concrete);
}
void gos_GetTerrainMatAO(float* rock, float* grass, float* dirt, float* concrete) {
    if (g_gos_renderer) g_gos_renderer->getTerrainMatAO(rock, grass, dirt, concrete);
    else { *rock = 1.0f; *grass = 1.0f; *dirt = 1.0f; *concrete = 1.0f; }
}
// POM-PERLAYER-HEIGHT-1: per-layer POM displacement-height multipliers.
void gos_SetTerrainMatHeight(float rock, float grass, float dirt, float concrete) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMatHeight(rock, grass, dirt, concrete);
}
void gos_GetTerrainMatHeight(float* rock, float* grass, float* dirt, float* concrete) {
    if (g_gos_renderer) g_gos_renderer->getTerrainMatHeight(rock, grass, dirt, concrete);
    else { *rock = 1.0f; *grass = 1.0f; *dirt = 1.0f; *concrete = 1.0f; }
}
// TERRAIN-MATERIAL-LIB-1: public wrapper -- terrainMaterialLibEnabled() above is
// file-static (TU-local); the live chunk binder (gos_terrain_lod_chunk.cpp) is a
// separate TU and needs the same MC2_TERRAIN_MATERIAL_LIB gate value to drive its
// own u_useMaterialLib upload. Same cached-static evaluation as the bridge path.
bool gos_TerrainMaterialLibEnabled() {
    return terrainMaterialLibEnabled();
}
void gos_SetTerrainNormalsFromHeightStrength(float s) {
    if (g_gos_renderer) g_gos_renderer->setTerrainNormalsFromHeightStrength(s);
}
float gos_GetTerrainNormalsFromHeightStrength() {
    return g_gos_renderer ? g_gos_renderer->getTerrainNormalsFromHeightStrength() : 1.0f;
}
void gos_SetTerrainCliffShadowFloor(float f) {
    if (g_gos_renderer) g_gos_renderer->setTerrainCliffShadowFloor(f);
}
float gos_GetTerrainCliffShadowFloor() {
    return g_gos_renderer ? g_gos_renderer->getTerrainCliffShadowFloor() : 0.0f;
}

void gos_SetTerrainDrawEnabled(bool e) {
    if (g_gos_renderer) g_gos_renderer->setTerrainDrawEnabled(e);
}
bool gos_GetTerrainDrawEnabled() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDrawEnabled() : true;
}
