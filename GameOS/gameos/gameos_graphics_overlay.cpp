// gameos_graphics: overlay texture trace, overlay and decal batches, uniform upload and the executor apply-state hooks.
// Declared in the class headers; shared names in gameos_graphics_material_internal.h.
#include "gameos_graphics_material_internal.h"
#include "mc2fmt.h"


// OVERLAY-BIAS-LINEARIZE-1: bias-mode selector for the overlay/decal VS
// (terrain_overlay.vert, shared by TerrainOverlayBatch + DecalBatch +
// decal-static). Default 0 = distance-proportional bias (world error ~
// linear -> no far-zoom overlay float). MC2_OVERLAY_BIAS_LEGACY=1 -> legacy
// constant NDC offset. Single consumer is the VS (no compute admission gate
// mirrors the overlay bias). See mclib/terrain_depth_bias.h.
static int mc2OverlayBiasLegacy() {
    static const int mode = mc2::gate::get<"MC2_OVERLAY_BIAS_LEGACY">();
    return mode;
}

static gosTexture* lookupBatchTextureOrWarn(const std::vector<gosTexture*>& textureList,
                                            DWORD textureId,
                                            const char* batchName) {
    if (textureId == INVALID_TEXTURE_ID)
        return nullptr;

    if (textureId >= textureList.size() || textureList[textureId] == nullptr) {
        static unsigned int warnCount = 0;
        if (warnCount < 16) {
            mc2::fmt::print_out("{}: dropping invalid texture handle {:d} (texture count={:d})\n", mc2::fmt::safe(batchName), mc2::fmt::as_u32(textureId), mc2::fmt::as_size(textureList.size()));
            ++warnCount;
        }
        return nullptr;
    }

    return textureList[textureId];
}

// OMT-1-OVERLAY-MISSING-TEXTURE-GUARD ─────────────────────────────────────────
// The overlay/decal batch draws previously did `glBindTexture(GL_TEXTURE_2D,
// t ? id : 0)` — binding GL name 0 on a resolve-fail makes the sampler read an
// INCOMPLETE texture, whose result is driver-defined (black on some GPUs,
// garbage/magenta on others = a vendor-dependent mystery; cf. OMT recon). Route
// resolve-fails to an explicit 1x1 magenta fallback + a deterministic
// once-per-handle log + counters, so a missing overlay texture is visible AND
// explained instead of undefined. NO bake/composite/cement/shader change — this
// only hardens the per-draw texture BIND. Verbose summary: MC2_OVERLAY_TEXTURE_TRACE=1.
namespace {
struct OverlayTexStats {
    uint64_t pushes = 0, resolved = 0, resolve_failed = 0,
             bound_zero_prevented = 0, fallback_bound = 0;
};
OverlayTexStats g_overlayTexStats;
GLuint g_overlayFallbackTex = 0;

GLuint getOverlayFallbackTexture() {
    if (g_overlayFallbackTex == 0) {
        const unsigned char magenta[4] = { 255, 0, 255, 255 };  // 1x1 RGBA debug
        // TEX-CLASS: asset-pool -- overlay magenta/fallback content texture
        glGenTextures(1, &g_overlayFallbackTex);
        glBindTexture(GL_TEXTURE_2D, g_overlayFallbackTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, magenta);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return g_overlayFallbackTex;
}

bool overlayTexTraceOn() {
    static int c = -1;
    if (c < 0) c = (mc2::gate::get<"MC2_OVERLAY_TEXTURE_TRACE">()) ? 1 : 0;
    return c != 0;
}

// Once-per-handle dedup without pulling in <set> (handles are few + bounded to 64).
bool overlayHandleAlreadyLogged(DWORD h) {
    static DWORD seen[64];
    static int   seenCount = 0;
    for (int i = 0; i < seenCount; ++i) if (seen[i] == h) return true;
    if (seenCount < 64) { seen[seenCount++] = h; return false; }  // new → log + record
    return true;  // table full → suppress (bounds log volume)
}

// Bind the resolved batch texture, or an explicit 1x1 magenta fallback on
// resolve-fail. NEVER binds GL name 0 on the resolve-fail path. Logs once per
// failing handle (deterministic, with reason) and counts every category.
void bindBatchTextureOrFallback(gosTexture* t, DWORD texHandle, const char* batchName) {
    ++g_overlayTexStats.pushes;
    const GLuint id = t ? (GLuint)t->getTextureId() : 0u;
    if (t && id != 0u) {
        ++g_overlayTexStats.resolved;
        glBindTexture(GL_TEXTURE_2D, id);
        return;
    }
    ++g_overlayTexStats.resolve_failed;
    ++g_overlayTexStats.bound_zero_prevented;   // the bug this guard prevents
    ++g_overlayTexStats.fallback_bound;
    if (!overlayHandleAlreadyLogged(texHandle)) {
        mc2::fmt::print_err("[OVERLAY_TEXTURE v1] event=resolve_fail batch={} texHandle={:d} fallback_bound=1 texture=<1x1 magenta debug> reason={}\n", mc2::fmt::safe(batchName), mc2::fmt::as_u32((unsigned)texHandle), mc2::fmt::safe((t && id == 0u) ? "resolved_but_gl_id_0" : "null_or_invalid_handle"));
        fflush(stderr);
    }
    glBindTexture(GL_TEXTURE_2D, getOverlayFallbackTexture());
}

// Cumulative counter summary; rate-limited, gated by MC2_OVERLAY_TEXTURE_TRACE.
// Uses an internal tick (this file has no g_mc2FrameCounter in scope); the tick
// advances once per drawTerrainOverlays call ≈ once per overlay-active frame.
void overlayTexTraceSummary() {
    if (!overlayTexTraceOn()) return;
    static uint64_t s_tick = 0;
    ++s_tick;
    if ((s_tick % 300u) != 1u) return;
    mc2::fmt::print_err("[OVERLAY_TEXTURE v1] event=summary tick={:d} pushes={:d} resolved={:d} resolve_failed={:d} bound_zero_prevented={:d} fallback_bound={:d}\n", mc2::fmt::as_u64((unsigned long long)s_tick), mc2::fmt::as_u64((unsigned long long)g_overlayTexStats.pushes), mc2::fmt::as_u64((unsigned long long)g_overlayTexStats.resolved), mc2::fmt::as_u64((unsigned long long)g_overlayTexStats.resolve_failed), mc2::fmt::as_u64((unsigned long long)g_overlayTexStats.bound_zero_prevented), mc2::fmt::as_u64((unsigned long long)g_overlayTexStats.fallback_bound));
    fflush(stderr);
}
} // anonymous namespace (OMT-1)



// Private helper: push one triangle into a batch, grouping consecutive same-texture
// calls into a single draw entry.  Batches are cleared at the END of the draw
// functions (called exactly once per frame from renderLists), not on push.
void gosRenderer::pushToOverlayBatch_(OverlayBatch_& b,
                                      const WorldOverlayVert* v3,
                                      unsigned int texHandle)
{
    if (!b.draws.empty() && b.draws.back().texHandle == texHandle) {
        b.draws.back().vertCount += 3;
    } else {
        OverlayBatchEntry_ entry;
        entry.texHandle = texHandle;
        entry.firstVert = (unsigned int)b.verts.size();
        entry.vertCount = 3;
        b.draws.push_back(entry);
    }
    b.verts.push_back(v3[0]);
    b.verts.push_back(v3[1]);
    b.verts.push_back(v3[2]);
}

void gosRenderer::pushTerrainOverlayTri(const WorldOverlayVert* verts3, unsigned int texHandle)
{
    pushToOverlayBatch_(terrainOverlayBatch_, verts3, texHandle);
}

void gosRenderer::pushDecalTri(const WorldOverlayVert* verts3, unsigned int texHandle)
{
    pushToOverlayBatch_(decalBatch_, verts3, texHandle);
}

// Upload shadow uniforms for a standalone shader program.
// Kept as a static helper because it only touches public getters — no private types.
static void setupOverlayShadowsForShp(GLuint shp)
{
    gosPostProcess* pp = getGosPostProcess();
    if (!pp) return;

    GLint loc;
    loc = glGetUniformLocation(shp, "terrainLightDir");
    if (loc >= 0) glUniform4fv(loc, 1, (const float*)&g_gos_renderer->getTerrainLightDir());

    // The shadow maps are Vulkan images the GL shaders cannot sample: the GL overlay runs shadow-free.
    loc = glGetUniformLocation(shp, "enableShadows");
    if (loc >= 0) glUniform1i(loc, 0);
    loc = glGetUniformLocation(shp, "enableDynamicShadows");
    if (loc >= 0) glUniform1i(loc, 0);
}

void gosRenderer::uploadOverlayUniforms_(GLuint shp, const OverlayUniformLocs_& L, float elapsed, const float* terrainMvpOverride)
{
    if (L.terrainMVP >= 0) {
        const float* tmvp = terrainMvpOverride
                                ? terrainMvpOverride
                                : (const float*)&getTerrainMVP();
        glUniformMatrix4fv(L.terrainMVP, 1, GL_FALSE, tmvp);
    }
    // projection_: row-major Stuff matrix — upload GL_TRUE (column-major interpretation)
    if (L.mvp >= 0)
        glUniformMatrix4fv(L.mvp, 1, GL_TRUE, (const float*)&getProj2Screen());
    if (L.fog_color >= 0)
        glUniform4fv(L.fog_color, 1, (const float*)&getFogColor());
    if (L.time >= 0)
        glUniform1f(L.time, elapsed);
    if (L.cameraPos >= 0)
        glUniform4fv(L.cameraPos, 1, (const float*)&getTerrainCameraPos());
    if (L.surfaceDebugMode >= 0)
        glUniform1i(L.surfaceDebugMode, (GLint)terrain_debug_mode_);
    if (L.pathTint >= 0)
        glUniform1i(L.pathTint, mc2ShaderPathTint());  // MC2_SHADER_PATH_TINT
    if (L.mapHalfExtent >= 0) {
        gosPostProcess* pp = getGosPostProcess();
        float halfExt = pp ? pp->getMapHalfExtent() : 0.0f;
        glUniform1f(L.mapHalfExtent, halfExt);
    }
    // CEMENT-NIGHT-LIGHT-1: mission flat-pad light. CRITICAL: every binder of
    // terrain_overlay.frag MUST upload this (an unset uniform reads 0 -> black
    // cement). All three overlay/decal bind sites route through this helper.
    if (L.missionFlatLight >= 0) {
        const vec4& fl = terrain_mission_flat_light_;
        glUniform3f(L.missionFlatLight, fl.x, fl.y, fl.z);
    }
    // OVERLAY-BIAS-LINEARIZE-1: bias-mode selector (MC2_OVERLAY_BIAS_LEGACY).
    if (L.overlayBiasLegacy >= 0)
        glUniform1i(L.overlayBiasLegacy, mc2OverlayBiasLegacy());

    // PAD-AREA-GLOW-1: pad-area night lift, lockstep with the chunk terrain
    // (same strength/spill env values, same nightFactor derived from the
    // mission flat light). mapSide force-uploads 0 when the chunk cement grid
    // is not resident so the frag never reads the binding-25 SSBO.
    if (L.padGlow >= 0) {
        int   pgSide = 0;
        float pgHalf = 0.0f;
        gos_GetTerrainChunkGridInfo(&pgSide, &pgHalf);
        const vec4& fl = terrain_mission_flat_light_;
        float flMax = fl.x;                   // (std::max unusable: windows.h max macro)
        if (fl.y > flMax) flMax = fl.y;
        if (fl.z > flMax) flMax = fl.z;
        float nf = 1.0f - flMax;
        if (nf < 0.0f) nf = 0.0f;
        if (nf > 1.0f) nf = 1.0f;
        const float s = gos_TerrainPadGlowStrength() * nf;
        glUniform3f(L.padGlow, s * 1.00f, s * 0.88f, s * 0.70f);
        if (L.padGlowSpill >= 0)
            glUniform1f(L.padGlowSpill, gos_TerrainPadGlowSpillWu());
        if (L.padGlowMapSide >= 0)
            glUniform1i(L.padGlowMapSide, pgSide);
        if (L.padGlowHalfMap >= 0)
            glUniform1f(L.padGlowHalfMap, pgHalf);
    }

    // OVERLAY-CEMENT-ADOPT-1 (CONCRETE-DELIVER-1). This helper is the SINGLE
    // chokepoint every binder of terrain_overlay.frag routes through, so
    // uploading here covers BOTH overlay bind sites (drawTerrainOverlays and
    // drawDecalStaticBatch) — MC2_TERRAIN_INDIRECT_OVERLAY=0 flips which one is
    // live and cannot silently lose the feature.
    //
    // Bind the SAME cement atlas the chunk pass samples, at the SAME unit 3
    // (kChunkTexUnitCement), so the adopted albedo is identical to the terrain's
    // by construction. Unit 3 is free on the overlay path (it guards only unit 0
    // for tex1) and is not one of the shadow/height/normal units (5, 9-11, 13).
    if (L.overlayCementAdopt >= 0) {
        extern GLuint gos_terrain_indirect_getCementAtlasGLTex();
        extern int    gos_terrain_indirect_getCementAtlasGridSide();
        extern bool   gos_terrain_indirect_isCementAtlasReady();
        extern float  gos_terrain_indirect_getWorldUnitsPerVertex();

        static const int s_overlayCementAdopt = mc2::gate::get<"MC2_OVERLAY_CEMENT_ADOPT">();
        glUniform1i(L.overlayCementAdopt, s_overlayCementAdopt);

        // gridSide force-uploads 0 unless the gate is on AND the atlas is
        // resident, which disarms the ALBEDO adoption in the frag independently
        // of the lighting reconciliation. Never sample an unbound sampler.
        const bool cementReady =
            (s_overlayCementAdopt != 0) && gos_terrain_indirect_isCementAtlasReady();
        // OVERLAY-CEMENT-REVERT-PURITY-1 (CONCRETE-DELIVER-2): only touch GL
        // texture unit 3 when the gate is actually ON. As landed, the killswitch
        // path still ran glActiveTexture(3) + glBindTexture(2D, 0) every overlay
        // draw, i.e. it UNBOUND unit 3 on a code path that is supposed to be a
        // no-op. Harmless in practice (the chunk terrain rebinds unit 3 per draw,
        // and the two indirect sites save/restore it), but it made the documented
        // "MC2_OVERLAY_CEMENT_ADOPT=0 => byte-identical stock" revert untrue at
        // the GL-state level. With the gate off we now leave unit 3 exactly as we
        // found it; u_cementAtlasOv keeps its default 0 and is never sampled,
        // because ovAdoptAlbedo is false whenever u_overlayCementAdopt is 0.
        if (s_overlayCementAdopt != 0 && L.cementAtlasOv >= 0) {
            glUniform1i(L.cementAtlasOv, 3);
            glActiveTexture(GL_TEXTURE0 + 3);
            glBindTexture(GL_TEXTURE_2D,
                          cementReady ? gos_terrain_indirect_getCementAtlasGLTex() : 0);
            glActiveTexture(GL_TEXTURE0);
        }
        if (L.cementGridSideOv >= 0)
            glUniform1i(L.cementGridSideOv,
                        cementReady ? gos_terrain_indirect_getCementAtlasGridSide() : 0);
        if (L.cementWUPTOv >= 0)
            glUniform1f(L.cementWUPTOv, gos_terrain_indirect_getWorldUnitsPerVertex());
        // OVERLAY-CEMENT-SEAM-PARITY-1 (CONCRETE-DELIVER-2): the chunk frag
        // applies a half-texel inset to its cement-atlas sample
        // (CEMENT-ATLAS-SEAM-1, kTglcCementAtlasSeamFix in
        // gos_terrain_lod_chunk.cpp:73, DEFAULT ON). The adopt path claimed
        // identical UV math but omitted it, which would leave a half-texel
        // neighbour-cell bleed along every 128wu tile border INSIDE the region
        // the adoption exists to make seamless. Read the SAME env var with the
        // SAME default so the two shaders cannot drift apart.
        if (L.cementSeamFixOv >= 0) {
            static const int s_cementSeamFixOv = mc2::gate::get<"MC2_CEMENT_ATLAS_SEAM_FIX">();
            glUniform1i(L.cementSeamFixOv, s_cementSeamFixOv);
        }
    }

    // NIGHT-LIGHT-POOLS-2b: decals take the same additive light-pool term as
    // the chunk terrain. Bind the pool
    // SSBO ourselves — the chunk terrain draw also binds slot 28 but may be
    // disabled (legacy terrain path), and context binding state is not
    // guaranteed here. poolCount 0 -> frag loop skipped -> byte-identical.
    if (L.numPoolLights >= 0) {
        static const bool s_poolSoft = mc2::gate::get<"MC2_LIGHT_POOL_SOFT">();
        // NIGHT-LIGHT-POOLS-4: default 0.5 -> 0.25 (lockstep with the chunk
        // terrain consts in gos_terrain_lod_chunk.cpp).
        static const float s_poolMoverDim = mc2::gate::get<"MC2_LIGHT_POOL_MOVER_DIM">();
        static const float s_poolMoverRadius = mc2::gate::get<"MC2_LIGHT_POOL_MOVER_RADIUS">();
        static const bool s_poolMoverMax = mc2::gate::get<"MC2_LIGHT_POOL_MOVER_MAXBLEND">();
        int poolCount = 0;
        {
            const GLuint   poolSsbo = gos_terrain_lighting::GetLightPoolSsbo();
            const uint32_t n        = gos_terrain_lighting::GetLightPoolLightCount();
            if (poolSsbo != 0 && n > 0) {
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER,
                                 TERRAIN_LIGHT_POOL_SSBO_BINDING, poolSsbo);
                poolCount = static_cast<int>(n > 256u ? 256u : n);
            }
        }
        glUniform1i(L.numPoolLights, poolCount);
        if (L.lightPoolSoft >= 0)     glUniform1i(L.lightPoolSoft, s_poolSoft ? 1 : 0);
        if (L.lightPoolMoverDim >= 0) glUniform1f(L.lightPoolMoverDim, s_poolMoverDim);
        static const bool s_poolCompress = mc2::gate::get<"MC2_LIGHT_POOL_COMPRESS">();
        if (L.lightPoolCompress >= 0)  glUniform1i(L.lightPoolCompress, s_poolCompress ? 1 : 0);
        if (L.lightPoolMoverScale >= 0) glUniform1f(L.lightPoolMoverScale, s_poolMoverRadius);
        if (L.lightPoolMoverMax >= 0)   glUniform1i(L.lightPoolMoverMax, s_poolMoverMax ? 1 : 0);
    }

    setupOverlayShadowsForShp(shp);

    // Same height-texture/cliff-floor stack for the cement overlay program; locs that
    // are -1 (decal program, no uniforms declared) get skipped. After binding we
    // restore active texture unit 0 because the helper leaves unit 11 bound —
    // overlay per-draw glBindTexture targets unit 0.
    bindTerrainHeightTexUniforms(L.terrainHeightTex,
                                 L.terrainHeightParams,
                                 L.terrainCliffShadowFloor,
                                 terrain_cliff_shadow_floor_);

    glActiveTexture(GL_TEXTURE0);
}

#ifdef MC2_VULKAN_ISLAND
// VK-PARITY-OVERLAY-1: GL-side per-frame inputs of the overlay/decal shaders that the VK
// owner (vulkan_terrain_island.cpp) cannot read itself. Same values uploadOverlayUniforms_ gives GL.
extern "C" void mc2_vk_overlay_fill_frame(Mc2VkOverlayFrame* out)
{
    if (!out) return;
    *out = Mc2VkOverlayFrame{};
    if (g_gos_renderer) {
        const vec4& fc = g_gos_renderer->getFogColor();
        out->fogColor[0] = fc.x; out->fogColor[1] = fc.y; out->fogColor[2] = fc.z; out->fogColor[3] = fc.w;
    }
    out->artFlatExempt = 0.0f;   // stock flat-pad multiply (the RUNWAY-CLOSE-1 exemption arm is gone; the push-constant slot stays)
    gosPostProcess* pp = getGosPostProcess();
    out->mapHalfExtent = pp ? pp->getMapHalfExtent() : 0.0f;
    out->biasLegacy = mc2OverlayBiasLegacy();
    static const int s_cementAdopt = mc2::gate::get<"MC2_OVERLAY_CEMENT_ADOPT">();
    static const int s_seamFix = mc2::gate::get<"MC2_CEMENT_ATLAS_SEAM_FIX">();
    out->cementAdopt = s_cementAdopt;
    out->seamFix = s_seamFix;
}
#endif

// Draw the terrain overlay batch (alpha cement perimeter tiles).
// Render state: opaque, depth-write ON, depth-test LEQUAL (same as solid terrain).
// MRT: terrain_overlay.frag writes GBuffer1.alpha=1 → shadow_screen skips these pixels.
// Batch cleared after draw.
// [RENDER_CONTRACT:Pass=TerrainOverlay id=gosRenderer_drawTerrainOverlays]
void gosRenderer::drawTerrainOverlays()
{
    if (terrainOverlayBatch_.draws.empty() || !overlayProg_) {
        terrainOverlayBatch_.verts.clear();
        terrainOverlayBatch_.draws.clear();
        return;
    }

#ifdef MC2_VULKAN_ISLAND
    // VK-PARITY-OVERLAY-1: under the VK umbrella the GL scene FBO is discarded; hand the batch
    // to the VK overlay owner (drawn right after Render.Decals) instead. Gate off => unchanged.
    if (mc2_vk_overlay_enabled()) {
        static_assert(sizeof(OverlayBatchEntry_) == sizeof(Mc2VkOverlayDraw),
                      "OverlayBatchEntry_ must layout-match Mc2VkOverlayDraw");
        mc2_vk_overlay_submit(0u, terrainOverlayBatch_.verts.data(), (uint32_t)terrainOverlayBatch_.verts.size(),
                              reinterpret_cast<const Mc2VkOverlayDraw*>(terrainOverlayBatch_.draws.data()),
                              (uint32_t)terrainOverlayBatch_.draws.size(), 0);
        terrainOverlayBatch_.verts.clear();
        terrainOverlayBatch_.draws.clear();
        return;
    }
#endif

    // SAME-ORDER-EXECUTOR-VALIDATE-1: top-level validate-only wrapper (gate MC2_FRAMEGRAPH_EXECUTOR).
    // No-op when gate unset (byte-identical). PIN INVARIANT: additive only — no GL state change,
    // no reorder. Body sets its own state UNCHANGED between begin and end.
    render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::TerrainOverlay,
                                              "gosRenderer_drawTerrainOverlays");
    struct TopLevelGuard_ {
        ~TopLevelGuard_() {
            render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::TerrainOverlay,
                                                    "gosRenderer_drawTerrainOverlays");
        }
    } _tlGuard;

    // APPLY-STATE-TERRAINOVERLAY-1: second top-level scene-pass apply-state. Gated by the
    // SAME predicate executorOwnBeginTopLevel uses internally (MC2_FRAMEGRAPH_EXECUTOR).
    // When ON and the executor applies state for this pass, executorApplyTerrainOverlayState()
    // pre-applies the TerrainOverlay pipeline and sets overlayStateAppliedByExecutor_; the body
    // applyPipeline below is then skipped (one-shot). The VBO upload that follows touches no FF
    // pipeline state, so this apply is order-equivalent to the body's. Idempotent ON.
    if (render_contract::isTopLevelExecutorEnabled() &&
        RenderCore::framegraph::findTopLevelStateDesc(RenderCore::RenderPassId::TerrainOverlay) != nullptr) {
        executorApplyTerrainOverlayState();
    }

    render_contract::noteRenderPass(render_contract::PassIdentity::TerrainOverlay,
                                    "gosRenderer_drawTerrainOverlays");
    render_contract::beginPassScope(render_contract::PassIdentity::TerrainOverlay,
                                    "gosRenderer_drawTerrainOverlays");

    glBindBuffer(GL_ARRAY_BUFFER, terrainOverlayBatch_.vbo);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)(terrainOverlayBatch_.verts.size() * sizeof(WorldOverlayVert)),
        terrainOverlayBatch_.verts.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // TERRAIN-OVERLAY-DECAL-APPLYPIPELINE-ROUTING-1: TerrainOverlay row (opaque,
    // depth test+write, GEQUAL, cull none) — byte-identical to the old hand-set
    // state. program(0)=skip keeps the manual overlay program bind below.
    // APPLY-STATE-TERRAINOVERLAY-1: skip when the executor already applied the pipeline
    // (MC2_FRAMEGRAPH_EXECUTOR ON). Reset one-shot before draw. Mirrors decal.
    // Gate OFF -> flag false -> body applies -> byte-identical.
    if (!overlayStateAppliedByExecutor_) {
        pipeline_binder::applyPipeline(
            RenderCore::getPipelineDesc(RenderCore::PipelineId::TerrainOverlay), "TerrainOverlay");
    }
    overlayStateAppliedByExecutor_ = false;

    glUseProgram(overlayProg_->shp_);
    float elapsed = SmokeMode::fixedTimestepEnabled()
                        ? (float)SmokeMode::fixedClockSeconds()
                        : (float)(timing::get_wall_time_ms() - timeStart_) / 1000.0f;
    const float* fixBMvpOverlay =
        gos_terrain_indirect::IsFrameSolidArmed()
            ? gos_terrain_indirect_getDispatchMvp16()  // TERRAIN-PHASE-RAW-MVP-OK
            : gos_GetTerrainMVPMat4();
    if (!fixBMvpOverlay) fixBMvpOverlay = gos_GetTerrainMVPMat4();
    uploadOverlayUniforms_(overlayProg_->shp_, overlayLocs_, elapsed, fixBMvpOverlay);

    {
        mc2gl::GlScopedTextureUnit texGuard(0);  // GLSTATE-TEXTURE-UNIT0-RESTORE-1
        GLint prevVao = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glBindVertexArray(terrainOverlayBatch_.vao);
        for (const auto& entry : terrainOverlayBatch_.draws) {
            if (overlayLocs_.tex1 >= 0)
                glUniform1i(overlayLocs_.tex1, 0);
            glActiveTexture(GL_TEXTURE0);
            gosTexture* t = lookupBatchTextureOrWarn(textureList_, entry.texHandle, "terrainOverlayBatch");
            bindBatchTextureOrFallback(t, entry.texHandle, "terrainOverlayBatch");  // OMT-1: no bind-0 on resolve-fail
            glDrawArrays(GL_TRIANGLES, (GLint)entry.firstVert, (GLsizei)entry.vertCount);
        }
        glBindVertexArray((GLuint)prevVao);
    }  // texGuard: restores unit-0 binding + active texture unit before invalidate

    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glUseProgram(0);

    terrainOverlayBatch_.verts.clear();
    terrainOverlayBatch_.draws.clear();

    gos_InvalidateRenderStateCache();

    overlayTexTraceSummary();  // OMT-1 counter summary (rate-limited, gated)

    render_contract::endPassScope(render_contract::PassIdentity::TerrainOverlay,
                                  "gosRenderer_drawTerrainOverlays");
}

//
// Byte-for-byte mirror of gosRenderer::drawTerrainOverlays()'s render-state
// block + program + uniform upload + per-draw texture bind + glDrawArrays,
// EXCEPT: (1) it draws the caller-owned static VBO via a dedicated persistent
// VAO (the per-frame terrainOverlayBatch_.vao captured the per-frame VBO at
// makeOverlayVAO time, so it cannot be reused for a different buffer);
// (2) draw ranges are supplied by the caller (gos_terrain_indirect's static
// bake) rather than read from terrainOverlayBatch_.draws; (3) it does NOT
// clear anything (mirrors DrawMineStatic — the static buffer persists across
// frames). The vertex layout (WorldOverlayVert, 28-byte stride, attribs
// 0..3) is identical to makeOverlayVAO so the same overlay shader binds.
bool gosRenderer::drawDecalStaticBatch(unsigned int vboGL,
                                       const struct GosDecalStaticDraw* draws,
                                       int drawCount)
{
    if (!overlayProg_ || vboGL == 0 || !draws || drawCount <= 0)
        return false;

    // APPLY-STATE-TERRAINOVERLAY-SITE-FIX-1: TerrainOverlay executor validate+apply
    // hooks live HERE (the LIVE overlay draw on cement/road maps), not in the dormant
    // drawTerrainOverlays(). When MC2_TERRAIN_INDIRECT_OVERLAY is armed (default-ON since
    // 2026-05-17) the per-quad producer is skipped, so drawTerrainOverlays() flushes an
    // empty batch and early-returns BEFORE its own validate wrapper — only THIS function
    // actually draws overlay content. The two sites are mutually exclusive per frame (the
    // indirect-overlay gate makes exactly one draw), so both can own PassIdentity::
    // TerrainOverlay without ever double-owning in a single frame. Placed AFTER the
    // early-returns above so we only own/apply on a real draw. PIN: additive only — no GL
    // state change, no reorder; the body sets its own state UNCHANGED between begin and end.
    render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::TerrainOverlay,
                                              "gosRenderer_drawDecalStaticBatch");
    struct TopLevelGuard_ {
        ~TopLevelGuard_() {
            render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::TerrainOverlay,
                                                    "gosRenderer_drawDecalStaticBatch");
        }
    } _tlGuard;

    // [TEMP DECAL_GLPROBE] eager-drain probe — env MC2_DECAL_GLPROBE=1.
    // Removed once root cause is pinned. Attributes GL_INVALID_OPERATION to
    // the exact call instead of the deferred CHECK_GL_ERROR drain site.
    static const bool s_decalGlProbe = mc2::gate::get<"MC2_DECAL_GLPROBE">();
    #define DECAL_GLPROBE(tag) do { if (s_decalGlProbe) { \
        GLenum e; while ((e = glGetError()) != GL_NO_ERROR) \
            printf("[DECAL_GLPROBE] at=%s err=0x%x\n", tag, (unsigned)e); \
        fflush(stdout); } } while(0)
    DECAL_GLPROBE("entry");

    // Capture the caller's VAO binding ONCE, before any VAO mutation. The
    // lazy-init block below clobbers the binding; if prevVao were captured
    // after it (as drawTerrainOverlays captures it — that path has no lazy
    // init), the first armed draw would record VAO 0 and this function would
    // return with VAO 0 bound, breaking the next generic-mesh draw on AMD
    // (trap #4, gpu_direct_renderer_bringup_checklist.md — VAO 0 left bound
    // raises GL_INVALID_OPERATION at the next applyVertexDeclaration/draw).
    GLint prevVao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);

    // Persistent VAO bound to whatever static VBO name the bake hands us.
    // Created once; the bake keeps a single GL_STATIC_DRAW buffer for the
    // process lifetime (mirror MineStaticVBO), so the name is stable.
    static GLuint s_decalStaticVAO = 0;
    static unsigned int s_decalStaticVAOBoundVBO = 0;
    if (s_decalStaticVAO == 0 || s_decalStaticVAOBoundVBO != vboGL) {
        if (s_decalStaticVAO == 0)
            glGenVertexArrays(1, &s_decalStaticVAO);
        constexpr int kStride = 28;  // sizeof(WorldOverlayVert) — mirror makeOverlayVAO
        glBindVertexArray(s_decalStaticVAO);
        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)vboGL);
        glVertexAttribPointer(0, 3, GL_FLOAT,         GL_FALSE, kStride, (void*)0);
        glVertexAttribPointer(1, 2, GL_FLOAT,         GL_FALSE, kStride, (void*)12);
        glVertexAttribPointer(2, 1, GL_FLOAT,         GL_FALSE, kStride, (void*)20);
        glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE,  kStride, (void*)24);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glEnableVertexAttribArray(3);
        // Restore the caller's VAO (NOT 0) so a draw issued between this
        // init and the bind below still sees a valid VAO on AMD.
        glBindVertexArray((GLuint)prevVao);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        s_decalStaticVAOBoundVBO = vboGL;
        DECAL_GLPROBE("after_vao_init");
    }
    DECAL_GLPROBE("after_vao_block");

    // ---- State block: identical to drawTerrainOverlays() -------------------
    // TERRAIN-OVERLAY-DECAL-APPLYPIPELINE-ROUTING-1: drive FF state from the
    // TerrainOverlay row (opaque, depth test+write, reverse-Z GEQUAL, cull none).
    // Byte-identical to the old hand-set state (+ no-op glFrontFace(CCW)/blendFunc/
    // offset-disable). glProgramName=0 -> applyPipeline SKIPs program; the manual
    // glUseProgram(overlayProg_) below stays.
    // APPLY-STATE-TERRAINOVERLAY-SITE-FIX-1: relocated apply-state dispatch. When
    // MC2_FRAMEGRAPH_EXECUTOR is ON and the executor applies state for this pass,
    // executorApplyTerrainOverlayState() pre-applies the TerrainOverlay pipeline and sets
    // overlayStateAppliedByExecutor_; the body applyPipeline below is then skipped (one-shot).
    // Gate OFF -> flag false -> body applies -> byte-identical. Mirrors drawDecals().
    if (render_contract::isTopLevelExecutorEnabled() &&
        RenderCore::framegraph::findTopLevelStateDesc(RenderCore::RenderPassId::TerrainOverlay) != nullptr) {
        executorApplyTerrainOverlayState();
    }
    if (!overlayStateAppliedByExecutor_) {
        pipeline_binder::applyPipeline(
            RenderCore::getPipelineDesc(RenderCore::PipelineId::TerrainOverlay), "TerrainOverlay");
    }
    overlayStateAppliedByExecutor_ = false;

    DECAL_GLPROBE("after_state_block");
    glUseProgram(overlayProg_->shp_);
    float elapsed = SmokeMode::fixedTimestepEnabled()
                        ? (float)SmokeMode::fixedClockSeconds()
                        : (float)(timing::get_wall_time_ms() - timeStart_) / 1000.0f;
    const float* fixBMvpDecalStatic =
        gos_terrain_indirect::IsFrameSolidArmed()
            ? gos_terrain_indirect_getDispatchMvp16()  // TERRAIN-PHASE-RAW-MVP-OK
            : gos_GetTerrainMVPMat4();
    if (!fixBMvpDecalStatic) fixBMvpDecalStatic = gos_GetTerrainMVPMat4();
    uploadOverlayUniforms_(overlayProg_->shp_, overlayLocs_, elapsed, fixBMvpDecalStatic);
    DECAL_GLPROBE("after_uniform_upload");

    {
        mc2gl::GlScopedTextureUnit texGuard(0);  // GLSTATE-TEXTURE-UNIT0-RESTORE-1
        glBindVertexArray(s_decalStaticVAO);
        for (int i = 0; i < drawCount; ++i) {
            const struct GosDecalStaticDraw& entry = draws[i];
            if (overlayLocs_.tex1 >= 0)
                glUniform1i(overlayLocs_.tex1, 0);
            glActiveTexture(GL_TEXTURE0);
            gosTexture* t = lookupBatchTextureOrWarn(textureList_, entry.texHandle, "decalStaticBatch");
            bindBatchTextureOrFallback(t, entry.texHandle, "decalStaticBatch");  // OMT-1: no bind-0 on resolve-fail
            glDrawArrays(GL_TRIANGLES, (GLint)entry.firstVert, (GLsizei)entry.vertCount);
            if (i == 0) DECAL_GLPROBE("after_first_drawarrays");
        }
        DECAL_GLPROBE("after_all_drawarrays");
        glBindVertexArray((GLuint)prevVao);
        DECAL_GLPROBE("after_vao_restore");
    }  // texGuard: restores unit-0 binding + active texture unit before invalidate

    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glUseProgram(0);

    // NO batch clear — the static bake persists across frames (mirror
    // DrawMineStatic; the per-frame drawTerrainOverlays clears here).

    gos_InvalidateRenderStateCache();
    DECAL_GLPROBE("exit");
    #undef DECAL_GLPROBE
    return true;
}

// Draw the decal batch (bomb craters + mech footprints).
// Render state: alpha blend, depth-write OFF, depth-test LEQUAL.
// Batch cleared after draw.
// [RENDER_CONTRACT:Pass=TerrainDecal id=gosRenderer_drawDecals]
void gosRenderer::drawDecals()
{
    if (decalBatch_.draws.empty() || !decalProg_) {
        decalBatch_.verts.clear();
        decalBatch_.draws.clear();
        return;
    }

#ifdef MC2_VULKAN_ISLAND
    // VK-PARITY-OVERLAY-1: see drawTerrainOverlays. Decal pass -> VK kind 1.
    if (mc2_vk_overlay_enabled()) {
        mc2_vk_overlay_submit(1u, decalBatch_.verts.data(), (uint32_t)decalBatch_.verts.size(),
                              reinterpret_cast<const Mc2VkOverlayDraw*>(decalBatch_.draws.data()),
                              (uint32_t)decalBatch_.draws.size(), 0);
        decalBatch_.verts.clear();
        decalBatch_.draws.clear();
        return;
    }
#endif

    // SAME-ORDER-EXECUTOR-VALIDATE-1: top-level validate-only wrapper (gate MC2_FRAMEGRAPH_EXECUTOR).
    // No-op when gate unset (byte-identical). PIN INVARIANT: additive only — no GL state change,
    // no reorder. Body sets its own state UNCHANGED between begin and end.
    render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::TerrainDecal,
                                              "gosRenderer_drawDecals");
    struct TopLevelGuard_ {
        ~TopLevelGuard_() {
            render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::TerrainDecal,
                                                    "gosRenderer_drawDecals");
        }
    } _tlGuard;

    // APPLY-STATE-TERRAINDECAL-1: first top-level scene-pass apply-state. Gated by the
    // SAME predicate executorOwnBeginTopLevel uses internally (MC2_FRAMEGRAPH_EXECUTOR).
    // When ON and the executor applies state for this pass, executorApplyTerrainDecalState()
    // pre-applies the TerrainDecal pipeline and sets decalStateAppliedByExecutor_; the body
    // applyPipeline below is then skipped (one-shot). The VBO upload that follows touches no
    // FF pipeline state, so this apply is order-equivalent to the body's. Idempotent ON.
    if (render_contract::isTopLevelExecutorEnabled() &&
        RenderCore::framegraph::findTopLevelStateDesc(RenderCore::RenderPassId::TerrainDecal) != nullptr) {
        executorApplyTerrainDecalState();
    }

    render_contract::noteRenderPass(render_contract::PassIdentity::TerrainDecal,
                                    "gosRenderer_drawDecals");
    render_contract::beginPassScope(render_contract::PassIdentity::TerrainDecal,
                                    "gosRenderer_drawDecals");

    glBindBuffer(GL_ARRAY_BUFFER, decalBatch_.vbo);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)(decalBatch_.verts.size() * sizeof(WorldOverlayVert)),
        decalBatch_.verts.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // TERRAIN-OVERLAY-DECAL-APPLYPIPELINE-ROUTING-1: TerrainDecal row (AlphaBlend
    // SRC_ALPHA/ONE_MINUS_SRC_ALPHA, depth-test but depth-WRITE OFF, GEQUAL, cull
    // none) — byte-identical to the old hand-set state. program(0)=skip keeps the
    // manual decalProg_ bind below.
    // APPLY-STATE-TERRAINDECAL-1: skip when the executor already applied the pipeline
    // (MC2_FRAMEGRAPH_EXECUTOR ON). Reset one-shot before draw. Mirrors EdgeFog
    // REMOVE-1 / SCREENSHADOW-1. Gate OFF -> flag false -> body applies -> byte-identical.
    if (!decalStateAppliedByExecutor_) {
        pipeline_binder::applyPipeline(
            RenderCore::getPipelineDesc(RenderCore::PipelineId::TerrainDecal), "TerrainDecal");
    }
    decalStateAppliedByExecutor_ = false;

    glUseProgram(decalProg_->shp_);
    float elapsed = SmokeMode::fixedTimestepEnabled()
                        ? (float)SmokeMode::fixedClockSeconds()
                        : (float)(timing::get_wall_time_ms() - timeStart_) / 1000.0f;
    const float* fixBMvpDecals =
        gos_terrain_indirect::IsFrameSolidArmed()
            ? gos_terrain_indirect_getDispatchMvp16()  // TERRAIN-PHASE-RAW-MVP-OK
            : gos_GetTerrainMVPMat4();
    if (!fixBMvpDecals) fixBMvpDecals = gos_GetTerrainMVPMat4();
    uploadOverlayUniforms_(decalProg_->shp_, decalLocs_, elapsed, fixBMvpDecals);

    {
        mc2gl::GlScopedTextureUnit texGuard(0);  // GLSTATE-TEXTURE-UNIT0-RESTORE-1
        GLint prevVao = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glBindVertexArray(decalBatch_.vao);
        for (const auto& entry : decalBatch_.draws) {
            if (decalLocs_.tex1 >= 0)
                glUniform1i(decalLocs_.tex1, 0);
            glActiveTexture(GL_TEXTURE0);
            gosTexture* t = lookupBatchTextureOrWarn(textureList_, entry.texHandle, "decalBatch");
            bindBatchTextureOrFallback(t, entry.texHandle, "decalBatch");  // OMT-1: no bind-0 on resolve-fail
            glDrawArrays(GL_TRIANGLES, (GLint)entry.firstVert, (GLsizei)entry.vertCount);
        }
        glBindVertexArray((GLuint)prevVao);
    }  // texGuard: restores unit-0 binding + active texture unit before invalidate

    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glUseProgram(0);

    decalBatch_.verts.clear();
    decalBatch_.draws.clear();

    gos_InvalidateRenderStateCache();

    render_contract::endPassScope(render_contract::PassIdentity::TerrainDecal,
                                  "gosRenderer_drawDecals");
}

// APPLY-STATE-TERRAINDECAL-1: pre-apply the declared TerrainDecal pipeline. This is
// the top-level analog of gosPostProcess::executorApply<X>State() — it performs the
// SOLE entry render-state of drawDecals (applyPipeline(TerrainDecal)), bumps the
// apply-state counter, and flags the body to skip its own applyPipeline (one-shot).
// Only the pipeline is lifted this slice; FBO/drawBuffers/viewport are inherited from
// the preceding Terrain pass and are NOT touched here (kTopLevelStateDesc records them
// as Unknown/Inherit). Idempotent: the body makes the identical call when the flag is
// false. Only reached when MC2_FRAMEGRAPH_EXECUTOR is ON and the pass runs.
void gosRenderer::executorApplyTerrainDecalState()
{
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::TerrainDecal), "TerrainDecal");
    mc2_framegraph_executor_bump_apply_state((unsigned)RenderCore::framegraph::ApplyPassId::TerrainDecal);
    decalStateAppliedByExecutor_ = true;
}

// APPLY-STATE-TERRAINOVERLAY-1: pre-apply the declared TerrainOverlay pipeline. Second
// consumer of the top-level apply-state machinery, mirroring executorApplyTerrainDecalState().
// Performs the SOLE entry render-state of drawTerrainOverlays (applyPipeline(TerrainOverlay)
// — opaque, depth-write ON), bumps the apply-state counter, and flags the body to skip its
// own applyPipeline (one-shot). Only the pipeline is lifted; FBO/drawBuffers/viewport are
// inherited from the preceding Terrain pass and NOT touched here (kTopLevelStateDesc records
// them Unknown/Inherit). Idempotent: the body makes the identical call when the flag is false.
// Only reached when MC2_FRAMEGRAPH_EXECUTOR is ON and the pass runs.
void gosRenderer::executorApplyTerrainOverlayState()
{
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::TerrainOverlay), "TerrainOverlay");
    mc2_framegraph_executor_bump_apply_state((unsigned)RenderCore::framegraph::ApplyPassId::TerrainOverlay);
    overlayStateAppliedByExecutor_ = true;
}

// FRAMEGRAPH-APPLY-STATE-EXTEND-1: shared GENERIC-AXES apply helper (declared in
// RenderCore/top_level_apply_axes.h). Applies the OPTIONAL non-pipeline axes a
// TopLevelStateDesc declares, each SKIPPED on its sentinel so the 4 unchanged consumers
// (fboTarget=Unknown / viewport=Inherit / clear=None) are byte-identical no-ops. The
// pipeline is applied by the per-pass fn AFTER this returns. Cross-TU shared so the
// upcoming Shadow slice reuses it (FBO + ShadowMap viewport + DepthForwardZ clear).
namespace RenderCore { namespace framegraph {
void applyTopLevelGenericAxes(const TopLevelStateDesc& d, unsigned fbo, int vpW, int vpH) {
    // 1. FBO bind — skip on Unknown (body inherits the bound FBO).
    if (d.fboTarget != RenderResourceId::Unknown) {
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
    }
    // 2. Viewport — skip on Inherit (body inherits the active viewport).
    if (d.viewport != ViewportKind::Inherit) {
        glViewport(0, 0, vpW, vpH);
    }
    // 3. Depth clear — skip on None. DepthForwardZ = forward-Z clear; the scene runs reverse-Z
    //    (clear-to-0) by default, so we clear to 1 then RESTORE clear-depth to 0 to leave global
    //    GL clear-depth state unchanged. NOT used this slice (StaticProp.clear == None); present
    //    for the Shadow slice (forward-Z shadow map).
    if (d.clear == ClearSpec::DepthForwardZ) {
        glClearDepth(1.0);
        glClear(GL_DEPTH_BUFFER_BIT);
        glClearDepth(0.0);
    }
}
}} // namespace RenderCore::framegraph


void __stdcall gos_PushTerrainOverlay(const WorldOverlayVert* verts3, unsigned int texHandle) {
    // PR2b Stage 0b probe — counts every entry into the producer regardless
    // of which call site invoked. Diagnoses whether the overlay-via-quad-walk
    // pipeline is live anywhere in tier1.
    gos_terrain_indirect::Counters_AddGosPushOverlayCall();
    if (g_gos_renderer && verts3) g_gos_renderer->pushTerrainOverlayTri(verts3, texHandle);
}

void __stdcall gos_PushDecal(const WorldOverlayVert* verts3, unsigned int texHandle) {
    if (g_gos_renderer && verts3) g_gos_renderer->pushDecalTri(verts3, texHandle);
}

void __stdcall gos_DrawTerrainOverlays() {
    if (g_gos_renderer) g_gos_renderer->drawTerrainOverlays();
}

void __stdcall gos_DrawDecals() {
    if (g_gos_renderer) g_gos_renderer->drawDecals();
}
