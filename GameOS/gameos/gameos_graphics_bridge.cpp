// gameos_graphics: terrain program-id accessors and the terrain surface, indirect, mask, mine and decal draw bridges.
// Declared in the class headers; shared names in gameos_graphics_material_internal.h.
#include "gameos_graphics_material_internal.h"
#include "mdi_submit.h"                            // MDI-SUBMISSION-SCAFFOLD-1: this piece issues indirect draws
#include "mc2fmt.h"


// TERRAIN-SPINE-0: free-function program-id accessors for the read-only
// Terrain Pass inspector. Defined here (after the gosRenderer class body)
// so other TUs (gameosmain.cpp) can reference them without seeing the full
// class. Each returns the raw GL program object id, or 0 if not linked.
uint32_t gos_getTerrainSurfaceProgramId() {
    gosRenderer* r = getGosRenderer();
    if (!r) return 0u;
    glsl_program* p = r->getTerrainSurfaceProgram();
    return (p && p->shp_) ? (uint32_t)p->shp_ : 0u;
}
uint32_t gos_getThinTerrainProgramId() {
    gosRenderer* r = getGosRenderer();
    if (!r) return 0u;
    glsl_program* p = r->getThinTerrainProgram();
    return (p && p->shp_) ? (uint32_t)p->shp_ : 0u;
}
uint32_t gos_getWaterFastProgramId() {
    gosRenderer* r = getGosRenderer();
    if (!r) return 0u;
    glsl_program* p = r->getWaterFastProgram();
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
uint32_t gos_getStaticPropShadowProgramId() {
    gosRenderer* r = getGosRenderer();
    if (!r) return 0u;
    glsl_program* p = r->getShadowStaticPropProg();
    return (p && p->shp_) ? (uint32_t)p->shp_ : 0u;
}
// SHADOW-SPINE-0: terrain shadow program is owned by the shadow_terrain
// gosRenderMaterial (not a direct glsl_program* slot). Look up via the
// material's shader. Returns 0 when not yet loaded.
uint32_t gos_getTerrainShadowProgramId() {
    gosRenderer* r = getGosRenderer();
    if (!r) return 0u;
    gosRenderMaterial* m = r->getShadowTerrainMaterial();
    if (!m) return 0u;
    glsl_program* p = m->getShader();
    return (p && p->shp_) ? (uint32_t)p->shp_ : 0u;
}
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
unsigned int gos_GetTerrainNormalArrayTex() {
    gosRenderer* r = getGosRenderer();
    return r ? r->getTerrainNormalArrayTexEnsureBuilt() : 0u;
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


static const bool s_patchStreamDirectBind = mc2::gate::get<"MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND">();

// GL sampler object used in the direct-bind fast path to enforce correct
// filter/wrap state on unit 0 without per-bucket glTexParameteri calls.
// Sampler objects override per-texture-object sampler state while bound.
// Terrain colormaps have no mipmaps → GL_LINEAR + GL_CLAMP_TO_EDGE.
static GLuint s_terrainBucketSampler = 0;

// gos_terrain_bridge_beginBucketLoop and gos_terrain_bridge_drawSingleBucket deleted by
// TERRAIN-BRIDGE-BODY-DELETE-1: called only from TerrainPatchStream::flush() (retired
// in PATCHSTREAM-THIN-RETIRE-1, 026e7276).


unsigned int gos_terrain_bridge_getThinShaderProgram() {
    if (!g_gos_renderer) return 0u;
    glsl_program* p = g_gos_renderer->getThinTerrainProgram();
    return (p && p->shp_) ? (unsigned int)p->shp_ : 0u;
}
// gos_terrain_indirect_getDispatchMvp16 and gos_GetTerrainMVPMat4 are declared
// TU-wide near the top of this file (after g_terrainMaterialProfile) so that
// renderWaterFastPath (which precedes this block) can see them.

//
// Plan : docs/superpowers/plans/.../terrain-continuous-surface-producer-plan.md
//        PR-2 (Wave 1, ADDITIVE / DEFAULT-OFF / DELETES NOTHING). Behind the
//        MC2_TERRAIN_SURFACE path-select kill-switch (gos_terrain_surface::
//        IsEnabled()); a no-op when OFF (behaviour-neutral). When ON it draws
//        the surface ON TOP of the still-running legacy/indirect path purely
//        for visual validation of the V-ssbo VS + Fork D reverse-Z bias --
//        PR-2 lands NO deletion and NO legacy kill site (that is PR-4).
//
// Fork V = V-ssbo (RULED): the surface vertex SSBO (binding 11) + the baked
// mission-static index SSBO (binding 12) are uploaded once per generation
// epoch (gos_terrain_surface::GetGenerationEpoch()); the VS pulls
// surfaceVerts[ surfaceIndices[gl_VertexID] ]. NO IBO / VAO element-array
// state (memory/element_array_buffer_is_vao_state_new_draw_paths_own_their_vao.md).
//
// Screen-agnostic (design Convergence C-1): the draw does NOT test
// IsFrameSolidArmed() -- surface EXISTENCE is decoupled from arming. PR-2
// draws at a single (fixed) LOD with draw-all visibility on BOTH armed and
// unarmed frames; the per-frame band/visibility refinement is PR-3 scope.
//
// State save/restore + AMD VAO-0 / attr-0 mitigations mirror
// gos_terrain_bridge_drawIndirect exactly (the proven pattern in this file).
//
void gos_terrain_surface_bridge_draw()
{
    if (!gos_terrain_surface::IsEnabled())   return;   // kill-switch OFF: no-op
    if (!gos_terrain_surface::IsGenerated()) return;   // nothing generated yet
    if (!g_gos_renderer)                     return;

    glsl_program* p = g_gos_renderer->getTerrainSurfaceProgram();
    if (!p || !p->shp_) return;   // compile failed (logged once at init)
    const GLuint prog = p->shp_;

    const uint32_t indexCount = gos_terrain_surface::GetIndexCount();
    const uint32_t vertCount  = gos_terrain_surface::GetVertexCount();
    if (indexCount == 0u || vertCount == 0u) return;

    ZoneScopedN("Terrain::SurfaceValidationDraw");
    TracyGpuZone("Terrain::SurfaceValidationDraw");

    // ---- Lazy GPU upload, epoch-tracked (re-upload on regeneration) --------
    // s_surfaceVB/IB: mission-static vertex + finest index (PR-1/PR-2).
    // s_surfaceTB: mission-static per-tile material table (META-FIX). All
    // mission-static; the topology/VB never changes -- the SSE/meshlet
    // drop-in invariant (design 3.4). Band-LOD per-frame buffers deleted in
    // the 2026-05-19 STOP+HARVEST (uniform finest, no per-frame compute).
    static GLuint   s_surfaceVB     = 0;
    static GLuint   s_surfaceIB     = 0;
    static GLuint   s_surfaceTB     = 0;   // META-FIX: per-tile material SSBO
    static uint32_t s_uploadedEpoch = 0;
    const uint32_t  epoch           = gos_terrain_surface::GetGenerationEpoch();
    // surface bridge behind IsEnabled() kill-switch; s_surfaceVB/IB/TB
    // SSBO-bound at slots 20/21/22 for the meshlet-validation draw only.
    // TIER2-EXCLUDED: substrate-gated
    if (s_surfaceVB == 0) glGenBuffers(1, &s_surfaceVB);
    // TIER2-EXCLUDED: substrate-gated
    if (s_surfaceIB == 0) glGenBuffers(1, &s_surfaceIB);
    // TIER2-EXCLUDED: substrate-gated
    if (s_surfaceTB == 0) glGenBuffers(1, &s_surfaceTB);
    if (epoch != s_uploadedEpoch) {
        const void* vd = gos_terrain_surface::GetVertexData();
        const void* id = gos_terrain_surface::GetIndexData();
        const void* td = gos_terrain_surface::GetTileData();
        if (!vd || !id || !td) return;
        const uint32_t tileCount = gos_terrain_surface::GetTileCount();
        // 32 B / TerrainSurfaceVertex (std430 vec4[2]); 4 B / uint32 index;
        // 32 B / TerrainSurfaceTile (std430 uvec4 wp + vec4 uvExt).
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_surfaceVB);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)vertCount * 32, vd, GL_STATIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_surfaceIB);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)indexCount * 4, id, GL_STATIC_DRAW);
        // META-FIX: mission-static per-tile material table. Row-major-keyed
        // (mx + my*cells), uploaded byte-identical to the dense recipe; the
        // surface VS recovers the row-major key from the cell's top-left
        // vertexNum so it is independent of the ADJUST-1 block-clustered
        // index emission order.
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_surfaceTB);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)tileCount * 32, td, GL_STATIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        s_uploadedEpoch = epoch;
        TS_TRACE("event=ssbo_upload epoch=%u verts=%u indices=%u vb=%u ib=%u "
                 "tb=%u",
                 epoch, vertCount, indexCount,
                 (unsigned)s_surfaceVB, (unsigned)s_surfaceIB,
                 (unsigned)s_surfaceTB);
    }

    GLint     savedProgram   = 0; glGetIntegerv(GL_CURRENT_PROGRAM,    &savedProgram);
    GLboolean savedBlend     = glIsEnabled(GL_BLEND);
    GLint     savedSrcRGB    = 0; glGetIntegerv(GL_BLEND_SRC_RGB,      &savedSrcRGB);
    GLint     savedDstRGB    = 0; glGetIntegerv(GL_BLEND_DST_RGB,      &savedDstRGB);
    GLint     savedDepthMask = 0; glGetIntegerv(GL_DEPTH_WRITEMASK,    &savedDepthMask);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint     savedDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC,         &savedDepthFunc);
    GLint     savedVAO       = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&savedVAO);
    GLboolean savedColorMask[4]; glGetBooleanv(GL_COLOR_WRITEMASK,     savedColorMask);
    GLuint    savedSampler   = 0;
    { GLint q = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &q); savedSampler = (GLuint)q; }

    // ---- VAO rebind (AMD VAO-0 trap) + attr-0 (AMD attribute-0 trap) ------
    extern void gos_RendererRebindVAO();
    gos_RendererRebindVAO();
    glEnableVertexAttribArray(0);

    // ---- Program + uniforms (reuse the thin binder: it sets mvp /
    //      camera / shadows / tex1 / matNormal* / atlas
    //      uniforms for the override program). terrainMVP IS declared by the
    //      surface VS (it projects world->clip itself, unlike the thin VS),
    //      so the binder's terrainMVP upload is consumed here. ----
    g_gos_renderer->terrainBindThinUniformsForPatchStream(p);

    // ---- Depth + color state for opaque terrain (inherit scene reverse-Z;
    //      do NOT set glClearDepth; design Section 4.4). GL_GEQUAL is the
    //      scene-global reverse-Z compare. ----
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);

    static GLuint s_surfaceSampler = 0;
    if (s_surfaceSampler == 0) {
        // TEX-CLASS: per-pass-rebind -- surface sampler object (CLAMP+LINEAR)
        glGenSamplers(1, &s_surfaceSampler);
        glSamplerParameteri(s_surfaceSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_surfaceSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_surfaceSampler, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_surfaceSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s_surfaceSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glBindSampler(0, s_surfaceSampler);

    // ---- Bind merged colormap atlas at unit 0; per-fragment WorldPos->tile
    //      material model (design Section 2 NC1: useAtlasColormap=1, the frag
    //      reconstructs atlas-absolute UV from WorldPos). useCementAtlas=0 so
    //      the frag never dereferences the binding-2 thin SSBO (PR-2 does not
    //      bind one; the cement-atlas path is unchanged legacy scope). ----
    extern GLuint gos_terrain_indirect_getAtlasGLTex();
    extern float  gos_terrain_indirect_getNumTexturesAcross();
    extern float  gos_terrain_indirect_getAtlasMapTopLeftX();
    extern float  gos_terrain_indirect_getAtlasMapTopLeftY();
    extern float  gos_terrain_indirect_getAtlasOneOverWorldUnits();

    GLint savedTex0Binding = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex0Binding);
    glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getAtlasGLTex());
    {
        const GLint locNTA  = glGetUniformLocation(prog, "atlasNumTexturesAcross");
        const GLint locMTX  = glGetUniformLocation(prog, "atlasMapTopLeftX");
        const GLint locMTY  = glGetUniformLocation(prog, "atlasMapTopLeftY");
        const GLint locOOWS = glGetUniformLocation(prog, "atlasOneOverWorldUnits");
        const GLint locUAC  = glGetUniformLocation(prog, "useAtlasColormap");
        const GLint locUCA  = glGetUniformLocation(prog, "useCementAtlas");
        if (locNTA  >= 0) glUniform1f(locNTA,  gos_terrain_indirect_getNumTexturesAcross());
        if (locMTX  >= 0) glUniform1f(locMTX,  gos_terrain_indirect_getAtlasMapTopLeftX());
        if (locMTY  >= 0) glUniform1f(locMTY,  gos_terrain_indirect_getAtlasMapTopLeftY());
        if (locOOWS >= 0) glUniform1f(locOOWS, gos_terrain_indirect_getAtlasOneOverWorldUnits());
        if (locUAC  >= 0) glUniform1i(locUAC,  1);
        if (locUCA  >= 0) glUniform1i(locUCA,  0);
    }

    // ---- Bind the mission-static vertex SSBO at binding 20 (the VS always
    //      pulls surfaceVerts[surfaceIndices[gl_VertexID]] from here). High
    //      bindings (20-24) chosen clear of the live thin path (1=recipe,
    //      2=thin) and compute-cull (11/12) so this additive PR-3 draw never
    //      disturbs the armed indirect path.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 20, s_surfaceVB);

    // ---- META-FIX 2026-05-19: bind the mission-static per-tile material SSBO
    //      at binding 22 so the surface VS can forward the REAL Texcoord /
    //      TerrainType varyings (replacing the PR-2 placeholders that broke
    //      the detail / cement frag layer close-up). The VS recovers the
    //      row-major tile key from the cell's top-left vertexNum via
    //      u_mapSide, so the lookup is correct under the ADJUST-1
    //      block-clustered index emission order.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 22, s_surfaceTB);
    {
        const GLint locMS = glGetUniformLocation(prog, "u_mapSide");
        if (locMS >= 0)
            glUniform1i(locMS, (GLint)gos_terrain_surface::GetMapSide());
    }

    // ── Screen-agnostic surface draw (band-LOD deferred, user-ruled
    //    2026-05-19 STOP+HARVEST per the 2026-05-19 architecture
    //    re-evaluation): draw the PR-1 mission-static FINEST index buffer
    //    directly every frame. Uniform finest => zero band-boundary artifact
    //    class, tessLevel>=1, draw-all (over-draws, never distance-culls --
    //    correctness-safe re the long-sightline invariant). The discrete
    //    band-LOD subsystem is DELETED, not parked; future LOD is a
    //    re-chartered NS2 slice. Surface EXISTS every frame regardless of
    //    arming; no C-1 *repoint* here (legacy + Fix-B retained).
    const bool armed = gos_terrain_indirect::IsFrameSolidArmed();

    // Unconditional PR-1 mission-static FINEST index-buffer draw
    // (binding 21 = s_surfaceIB). Band-LOD deleted (not parked) per the
    // STOP+HARVEST; uniform finest => zero band-boundary artifact class,
    // tessLevel>=1, draw-all. Surface EXISTS every frame regardless of
    // arming; arming no longer selects a path here.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 21, s_surfaceIB);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)indexCount);
    // first_draw lifecycle print -- once per process (coarse, never per-quad).
    static bool s_firstSurfaceDrawPrinted = false;
    if (!s_firstSurfaceDrawPrinted) {
        s_firstSurfaceDrawPrinted = true;
        TS_TRACE("event=first_surface_draw verts=%u indices=%u "
                 "armed=%d screen_agnostic=1 fork_v=ssbo "
                 "lod=uniform_finest fork_d=clip_pre_divide",
                 vertCount, indexCount, (int)armed);
    }

    // ---- Reset shared-frag flags so legacy/indirect path doesn't inherit --
    {
        const GLint locUAC = glGetUniformLocation(prog, "useAtlasColormap");
        if (locUAC >= 0) glUniform1i(locUAC, 0);
    }

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 20, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 21, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 22, 0);  // META-FIX tile SSBO
    glBindSampler(0, savedSampler);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex0Binding);
    glColorMask(savedColorMask[0], savedColorMask[1],
                savedColorMask[2], savedColorMask[3]);
    glDepthMask((GLboolean)savedDepthMask);
    glDepthFunc((GLenum)savedDepthFunc);
    if (!savedDepthTest) glDisable(GL_DEPTH_TEST);
    else                 glEnable(GL_DEPTH_TEST);
    if (savedBlend) glEnable(GL_BLEND);
    else            glDisable(GL_BLEND);
    glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
    glBindVertexArray((GLuint)savedVAO);
    glUseProgram((GLuint)savedProgram);

    if (g_gos_renderer) g_gos_renderer->invalidateRenderStateCache();
}

//
// Called by gos_terrain_indirect::DrawIndirect() after preflight arming.
// Mirrors gos_terrain_bridge_renderWaterFast structure but for opaque terrain:
//  - Depth writes ON (terrain is the primary depth source)
//  - CLAMP_TO_EDGE / LINEAR sampler (atlas-tiled, matches M2 path)
//  - ColorMask save/restore (M5: shadow pass leaves it FALSE)
//  - AMD attr-0: glEnableVertexAttribArray(0) after VAO rebind
//  - glMultiDrawArraysIndirect — NO EBO (thin VS reads SSBO via gl_VertexID)
//
// State save/restore covers: Program, Blend enable, BlendSrcDstRGB, DepthMask,
// DepthTest, DepthFunc, VAO, ColorMask, Sampler[0].
// Returns false only if the thin program is not available.
// ──────────────────────────────────────────────────────────────────────────
bool gos_terrain_bridge_drawIndirect(int cmdCount, unsigned int recipeSSBO,
                                     unsigned int thinRecordSSBO,
                                     unsigned int indirectCmdBuffer)
{
    ZoneScopedN("Terrain::IndirectDraw");
    TracyGpuZone("Terrain::IndirectDraw");
    if (!g_gos_renderer) return false;
    // Honour the ImGui "Terrain Draw" toggle on the GPU-driven path too.
    if (!g_gos_renderer->getTerrainDrawEnabled()) return false;
    glsl_program* p = g_gos_renderer->getThinTerrainProgram();
    if (!p || !p->shp_) return false;
    if (thinRecordSSBO == 0 || indirectCmdBuffer == 0) return false;

    const GLuint prog = p->shp_;

    GLint       savedProgram   = 0; glGetIntegerv(GL_CURRENT_PROGRAM,   &savedProgram);
    GLboolean   savedBlend     = glIsEnabled(GL_BLEND);
    GLint       savedSrcRGB    = 0; glGetIntegerv(GL_BLEND_SRC_RGB,     &savedSrcRGB);
    GLint       savedDstRGB    = 0; glGetIntegerv(GL_BLEND_DST_RGB,     &savedDstRGB);
    GLint       savedDepthMask = 0; glGetIntegerv(GL_DEPTH_WRITEMASK,   &savedDepthMask);
    GLboolean   savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint       savedDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC,        &savedDepthFunc);
    GLint       savedVAO       = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVAO);
    GLboolean   savedColorMask[4];  glGetBooleanv(GL_COLOR_WRITEMASK,   savedColorMask);
    GLuint      savedSampler   = 0;
    { GLint q = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &q); savedSampler = (GLuint)q; }

    // ---- VAO rebind (AMD VAO-0 trap) + attr-0 (AMD attribute-0 trap) -------
    extern void gos_RendererRebindVAO();
    gos_RendererRebindVAO();
    // AMD silently drops draws when attribute 0 is disabled and the VS has no
    // layout(location=0) input. The thin VS reads from SSBOs only and has no
    // vertex attribute declarations (docs/amd-driver-rules.md:5). Enable a
    // dummy attr-0 array on the rebound VAO as the runtime mitigation.
    glEnableVertexAttribArray(0);

    // ---- Program + uniforms ------------------------------------------------
    // terrainBindThinUniformsForPatchStream sets the program AND all uniforms
    // (projection chain, camera, shadow maps, PBR params, tex1 sampler).
    // It also calls glUseProgram, so we can safely use the uniform locations
    // it caches.  This mirrors how the M2 thin path sets up uniforms before
    // issueDraws, without duplicating the uniform logic.
    // ssboRecordBase is set to 0 — the indirect thin-record SSBO is indexed
    // globally (slot 2); recordIdx in the VS = 0 + vid/6.
    const int ssboRecordBaseLoc =
        g_gos_renderer->terrainBindThinUniformsForPatchStream();
    if (ssboRecordBaseLoc >= 0)
        glUniform1i(ssboRecordBaseLoc, 0);

    // Fix A (2026-05-14): override the just-uploaded terrainMVP with the
    // per-ring-slot snapshot captured at compute-dispatch time.  Compute
    // wrote pzOk gates into the thin records using that MVP; the VS must
    // project them with the same MVP or fast-rotation frames produce the
    // giant grey-banded terrain triangle.  See
    // docs/superpowers/progress/2026-05-14-raster-triangle-handoff.md.
    extern const float* gos_terrain_indirect_getRingSlotMvp();
    if (const float* slotMvp = gos_terrain_indirect_getRingSlotMvp()) {
        g_gos_renderer->terrainOverrideThinMVP(slotMvp);
    }

    // ---- Depth + color state for opaque terrain ----------------------------
    // TERRAIN-SOLID-APPLYPIPELINE-ROUTING-1: drive depth/blend/cull from the
    // TerrainSolid pipeline row instead of hand-setting them. Byte-identical:
    // depthTest ON, GEQUAL reverse-Z, depthWrite ON, blend OFF (Opaque), and
    // cull None / frontFace Ccw — the latter two are the EMPIRICALLY PROBED ambient
    // (TERRAIN-CULL-STATE-PROBE-1), so naming them is a no-op, not a change.
    // glProgramName=0: the program is already bound above by
    // terrainBindThinUniformsForPatchStream.
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::TerrainSolid), "TerrainSolid");
    // M5 (LOAD-BEARING — DO NOT REMOVE): undo any prior shadow-pass
    // glColorMask(FALSE,...) from gos_postprocess.cpp:1134/1156. applyPipeline does
    // NOT own colorMask state, so this repair MUST stay explicit here — without it
    // the indirect path draws nothing on the frame following a shadow pass. It is
    // NOT redundant until the pipeline system owns color-mask state.
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    render_contract::assertPassContract(render_contract::PassIdentity::TerrainBase,
                                        "gos_TerrainLodChunk_SubmitDrawCommands");

    static GLuint s_indirectTerrainSampler = 0;
    if (s_indirectTerrainSampler == 0) {
        // TEX-CLASS: per-pass-rebind -- indirect-terrain sampler object
        glGenSamplers(1, &s_indirectTerrainSampler);
        glSamplerParameteri(s_indirectTerrainSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_indirectTerrainSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_indirectTerrainSampler, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_indirectTerrainSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s_indirectTerrainSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glBindSampler(0, s_indirectTerrainSampler);

    // ---- Bind merged colormap atlas at unit 0 --------------------------------
    // The legacy per-bucket path binds each tile's gosHandle before glDrawArrays.
    // The indirect path issues one glMultiDrawArraysIndirect for ALL quads, so
    // per-bucket binding is impossible.  Instead we bind the full merged colormap
    // atlas (cpuColorMap uploaded once by BuildColormapAtlas at mission load) and
    // let the thin VS compute atlas-absolute UV from world position.
    // State save: capture current GL_TEXTURE_BINDING_2D on unit 0 so the next
    // renderer doesn't see the atlas leak.
    extern GLuint gos_terrain_indirect_getAtlasGLTex();
    extern float  gos_terrain_indirect_getNumTexturesAcross();
    extern float  gos_terrain_indirect_getAtlasMapTopLeftX();
    extern float  gos_terrain_indirect_getAtlasMapTopLeftY();
    extern float  gos_terrain_indirect_getAtlasOneOverWorldUnits();
    extern GLuint gos_terrain_indirect_getCementAtlasGLTex();
    extern int    gos_terrain_indirect_getCementAtlasGridSide();
    extern bool   gos_terrain_indirect_isCementAtlasReady();
    extern float  gos_terrain_indirect_getWorldUnitsPerVertex();
    extern GLuint gos_terrain_indirect_getTransitionMaskArrayGL();
    extern bool   gos_terrain_indirect_isTransitionMaskReady();

    GLint savedTex0Binding = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex0Binding);
    glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getAtlasGLTex());

    // Upload atlas UV decomposition uniforms (set AFTER glUseProgram via
    // terrainBindThinUniformsForPatchStream above; prog is already in scope).
    {
        const GLint locNTA  = glGetUniformLocation(prog, "atlasNumTexturesAcross");
        const GLint locMTX  = glGetUniformLocation(prog, "atlasMapTopLeftX");
        const GLint locMTY  = glGetUniformLocation(prog, "atlasMapTopLeftY");
        const GLint locOOWS = glGetUniformLocation(prog, "atlasOneOverWorldUnits");
        const GLint locUAC  = glGetUniformLocation(prog, "useAtlasColormap");
        if (locNTA  >= 0) glUniform1f(locNTA,  gos_terrain_indirect_getNumTexturesAcross());
        if (locMTX  >= 0) glUniform1f(locMTX,  gos_terrain_indirect_getAtlasMapTopLeftX());
        if (locMTY  >= 0) glUniform1f(locMTY,  gos_terrain_indirect_getAtlasMapTopLeftY());
        if (locOOWS >= 0) glUniform1f(locOOWS, gos_terrain_indirect_getAtlasOneOverWorldUnits());
        // useAtlasColormap=1: frag samples tex1 from AtlasUV (atlas-absolute) instead
        // of Texcoord (per-tile). Texcoord remains per-tile so detail/POM/anti-tile
        // math stays unchanged. Legacy paths leave this at 0 (default) and continue
        // to sample tex1 with per-tile Texcoord against per-tile bound textures.
        if (locUAC  >= 0) glUniform1i(locUAC,  1);
    }

    // ---- Bind cement catalog atlas at unit 3 (Stage 4 / PR2) ---------------
    // Sampler-object override safety: clear unit 3's sampler with
    // glBindSampler(3, 0) so the texture-object wrap (GL_REPEAT) is the binding
    // (M1/B3 of plan v2.1).
    GLint  savedTex3Binding = 0;
    GLuint savedTex3Sampler = 0;
    const bool cementAtlasReady = gos_terrain_indirect_isCementAtlasReady();
    if (cementAtlasReady) {
        glActiveTexture(GL_TEXTURE3);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex3Binding);
        { GLint q = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 3, &q); savedTex3Sampler = (GLuint)q; }
        glBindSampler(3, 0);
        glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getCementAtlasGLTex());
        glActiveTexture(GL_TEXTURE0);
    }

    {
        const GLint locTex3   = glGetUniformLocation(prog, "tex3");
        const GLint locUCA    = glGetUniformLocation(prog, "useCementAtlas");
        const GLint locGSide  = glGetUniformLocation(prog, "atlasCementGridSide");
        const GLint locWUPT   = glGetUniformLocation(prog, "atlasCementWorldUnitsPerTile");

        // Lifecycle warning (subagent M2-v2): if any uniform location is missing
        // when the atlas is ready, the frag failed to compile with the new
        // uniforms — print once per process so the failure is visible in the log.
        static bool s_warnedCementUniforms = false;
        if (cementAtlasReady && !s_warnedCementUniforms &&
            (locTex3 < 0 || locUCA < 0 || locGSide < 0 || locWUPT < 0)) {
            MC2_DIAGF("[TERRAIN_INDIRECT v1] event=cement_uniform_missing "
                   "tex3=%d useCementAtlas=%d gridSide=%d wupt=%d\n",
                   locTex3, locUCA, locGSide, locWUPT);
            fflush(stdout);
            s_warnedCementUniforms = true;
        }

        if (locTex3  >= 0) glUniform1i(locTex3, 3);
        if (cementAtlasReady) {
            if (locUCA   >= 0) glUniform1i(locUCA,   1);
            if (locGSide >= 0) glUniform1i(locGSide, gos_terrain_indirect_getCementAtlasGridSide());
            if (locWUPT  >= 0) glUniform1f(locWUPT,  gos_terrain_indirect_getWorldUnitsPerVertex());
        } else {
            if (locUCA   >= 0) glUniform1i(locUCA,   0);
        }
    }

    GLint  savedTex4Binding2D     = 0;
    GLuint savedTex4Binding2DArr  = 0;
    const bool transitionMaskReady = gos_terrain_indirect_isTransitionMaskReady();
    if (transitionMaskReady) {
        glActiveTexture(GL_TEXTURE4);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex4Binding2D);
        { GLint q = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &q); savedTex4Binding2DArr = (GLuint)q; }
        glBindTexture(GL_TEXTURE_2D_ARRAY, gos_terrain_indirect_getTransitionMaskArrayGL());
        glActiveTexture(GL_TEXTURE0);
    }
    {
        const GLint locTMArr = glGetUniformLocation(prog, "u_transitionMaskArray");
        const GLint locUseTM = glGetUniformLocation(prog, "u_useTransitionMask");
        if (locTMArr >= 0) glUniform1i(locTMArr, 4);
        if (locUseTM >= 0) glUniform1i(locUseTM, transitionMaskReady ? 1 : 0);
    }

    // ---- SSBO bindings + indirect buffer -----------------------------------
    // Slot 1 = recipe (static, mission-stable)
    // Slot 2 = thin record (per-frame ring) — will be re-bound via range below
    if (recipeSSBO != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, (GLuint)recipeSSBO);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, (GLuint)indirectCmdBuffer);

    // Bind the thin-record slice at the correct ring offset.
    // The ring slot was advanced by PackThinRecordsForFrame; we don't
    // re-derive the slot offset here — instead we bind the whole buffer
    // at slot 2 and configure ssboRecordBase = 0 with base-instance 0.
    // The thin VS reads thinRecs[recordIdx] where recordIdx = ssboRecordBase + vid/6.
    // Because we uploaded at offset ringSlot*kThinRecordBytes, we need to bind
    // the sub-range at that offset so index 0 in the shader == our first record.
    // Re-bind as a range binding to point ssboRecordBase=0 to the current slot.
    {
        // Ring slot offset in bytes.  Each slot holds kMaxRecs records of
        // sizeof(TerrainQuadThinRecord) each — post Fix B (2026-05-14) the
        // record size is 96 B (was 32 B), so this offset would silently
        // diverge from gos_terrain_indirect.cpp's kThinRecordBytes if the
        // multiplier were left hardcoded.  Source the size from the struct.
        extern int gos_terrain_indirect_getRingSlot();
        const int    slot       = gos_terrain_indirect_getRingSlot();
        const size_t kRecordSz  = sizeof(TerrainQuadThinRecord);
        const size_t kMaxRecs   = 65536u;
        const GLintptr offset   = (GLintptr)(slot * kMaxRecs * kRecordSz);
        const GLsizeiptr sz     = (GLsizeiptr)(kMaxRecs * kRecordSz);
        gpuBindSsboRange(2, (GLuint)thinRecordSSBO, (long long)offset, (long long)sz,
                         "gameos.thinRecord");
    }

    // ── Probe 8: compare draw-time MVP fingerprint vs compute-time fingerprint.
    // Both compute and the thin VS use gos_GetTerrainMVPMat4().  If the matrix
    // has been mutated between those two read points (in the same frame), the
    // compute's pzOk gate decisions disagree with the VS's projection — quads
    // that compute marked as visible may project to wild screen positions.
    // Hypothesis after probes 1-7a all silent + bug persists: MVP delta is the
    // last data-source candidate we haven't tested.
    // VPL retirement deferred #4 (2026-05-16): inert post-Fix-B (no
    // load-bearing consumer); gated behind MC2_RING_TRACE like the rest of
    // the Step 9 demotion so the per-frame FNV + glGetBufferSubData on the
    // indirect buffer cost nothing in the default config.
    { static const bool s_ringMvpProbe = mc2::gate::get<"MC2_RING_TRACE">(); if (s_ringMvpProbe) {
        const float* drawMvp = gos_GetTerrainMVPMat4();
        if (drawMvp) {
            uint32_t drawFp = 2166136261u;
            for (int k = 0; k < 12; ++k) {
                uint32_t bits = 0;
                memcpy(&bits, &drawMvp[k], sizeof(bits));
                drawFp ^= bits; drawFp *= 16777619u;
            }
            const uint32_t dispatchFp     = gos_terrain_indirect_getDispatchMvpFp();
            const uint64_t dispatchFrame  = gos_terrain_indirect_getDispatchMvpFrameIdx();
            static uint64_t s_bridgeFrame = 0;
            ++s_bridgeFrame;
            if (drawFp != dispatchFp) {
                static FILE* s_probeSink2 = []{ FILE* f = fopen("ring_trace.log", "a"); return f; }();
                static uint32_t s_mvpMismatchCount = 0;
                ++s_mvpMismatchCount;
                if (s_mvpMismatchCount == 1 || s_mvpMismatchCount % 100 == 0) {
                    // Probe 8b: read back compute-time matrix bytes for byte-level verification.
                    float dispatchFloats[4] = { 0, 0, 0, 0 };
                    gos_terrain_indirect_getDispatchMvpFloats4(dispatchFloats);
                    mc2::fmt::print_err("[RING_MVP_DELTA v1] bridge_frame={:d} dispatch_frame={:d} dispatch_fp=0x{:08x} draw_fp=0x{:08x} count={:d} disp_mvp[0..3]=[{:.6f},{:.6f},{:.6f},{:.6f}] draw_mvp[0..3]=[{:.6f},{:.6f},{:.6f},{:.6f}]\n", mc2::fmt::as_u64((unsigned long long)s_bridgeFrame), mc2::fmt::as_u64((unsigned long long)dispatchFrame), mc2::fmt::as_u32(dispatchFp), mc2::fmt::as_u32(drawFp), mc2::fmt::as_u32(s_mvpMismatchCount), dispatchFloats[0], dispatchFloats[1], dispatchFloats[2], dispatchFloats[3], drawMvp[0], drawMvp[1], drawMvp[2], drawMvp[3]);
                    fflush(stderr);
                    if (s_probeSink2) {
                        mc2::fmt::print(s_probeSink2, "[RING_MVP_DELTA v1] bridge_frame={:d} dispatch_frame={:d} dispatch_fp=0x{:08x} draw_fp=0x{:08x} count={:d} disp_mvp[0..3]=[{:.6f},{:.6f},{:.6f},{:.6f}] draw_mvp[0..3]=[{:.6f},{:.6f},{:.6f},{:.6f}]\n", mc2::fmt::as_u64((unsigned long long)s_bridgeFrame), mc2::fmt::as_u64((unsigned long long)dispatchFrame), mc2::fmt::as_u32(dispatchFp), mc2::fmt::as_u32(drawFp), mc2::fmt::as_u32(s_mvpMismatchCount), dispatchFloats[0], dispatchFloats[1], dispatchFloats[2], dispatchFloats[3], drawMvp[0], drawMvp[1], drawMvp[2], drawMvp[3]);
                        fflush(s_probeSink2);
                    }
                }
            }
        }

        // Also dump cmd block (terrain-indirect-expert's probe 8 secondary):
        // check cmd.first and cmd.baseInstance fields are zero.  cmd-patch
        // shader sets them to 0 explicitly; non-zero indicates a race or wrong
        // upload.
        uint32_t cmdBlock[4] = {0,0,0,0};
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, (GLuint)indirectCmdBuffer);
        glGetBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, sizeof(cmdBlock), cmdBlock);
        if (cmdBlock[2] != 0u || cmdBlock[3] != 0u) {
            static FILE* s_probeSink3 = []{ FILE* f = fopen("ring_trace.log", "a"); return f; }();
            static uint32_t s_cmdFieldsBad = 0;
            ++s_cmdFieldsBad;
            if (s_cmdFieldsBad == 1 || s_cmdFieldsBad % 50 == 0) {
                mc2::fmt::print_err("[RING_CMDFIELDS v1] count={:d} inst={:d} first={:d} base={:d} bad_count={:d}\n", mc2::fmt::as_u32(cmdBlock[0]), mc2::fmt::as_u32(cmdBlock[1]), mc2::fmt::as_u32(cmdBlock[2]), mc2::fmt::as_u32(cmdBlock[3]), mc2::fmt::as_u32(s_cmdFieldsBad));
                fflush(stderr);
                if (s_probeSink3) {
                    mc2::fmt::print(s_probeSink3, "[RING_CMDFIELDS v1] count={:d} inst={:d} first={:d} base={:d} bad_count={:d}\n", mc2::fmt::as_u32(cmdBlock[0]), mc2::fmt::as_u32(cmdBlock[1]), mc2::fmt::as_u32(cmdBlock[2]), mc2::fmt::as_u32(cmdBlock[3]), mc2::fmt::as_u32(s_cmdFieldsBad));
                    fflush(s_probeSink3);
                }
            }
        }
    } }

    const bool indirectWireframe = g_gos_renderer->getTerrainWireframe();
    if (indirectWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, (GLsizei)cmdCount, 0);
    mdi_submit::trace("TerrainSolid", "MultiDrawArraysIndirect", (int)cmdCount,
                      "GPU", "g_indirectCmdBuffer", false);
    if (indirectWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // Reset useAtlasColormap so M2 fast path (shares this program) doesn't
    // inherit the atlas-mode flag and sample from AtlasUV against a per-tile
    // bound texture.
    {
        const GLint locUAC = glGetUniformLocation(prog, "useAtlasColormap");
        if (locUAC >= 0) glUniform1i(locUAC, 0);
    }
    // Reset useCementAtlas so the M2 fast path doesn't inherit the cement flag.
    {
        const GLint locUCA  = glGetUniformLocation(prog, "useCementAtlas");
        const GLint locUseTM = glGetUniformLocation(prog, "u_useTransitionMask");
        if (locUCA  >= 0) glUniform1i(locUCA,  0);
        if (locUseTM >= 0) glUniform1i(locUseTM, 0);
    }
    if (cementAtlasReady) {
        glActiveTexture(GL_TEXTURE3);
        glBindSampler(3, savedTex3Sampler);
        glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex3Binding);
        glActiveTexture(GL_TEXTURE0);
    }
    if (transitionMaskReady) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D_ARRAY, savedTex4Binding2DArr);
        glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex4Binding2D);
        glActiveTexture(GL_TEXTURE0);
    }

    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, 0);
    glBindSampler(0, savedSampler);
    // Restore atlas texture bind so next renderer doesn't see the atlas.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex0Binding);
    glColorMask(savedColorMask[0], savedColorMask[1],
                savedColorMask[2], savedColorMask[3]);
    glDepthMask((GLboolean)savedDepthMask);
    glDepthFunc((GLenum)savedDepthFunc);
    if (!savedDepthTest) glDisable(GL_DEPTH_TEST);
    else                 glEnable(GL_DEPTH_TEST);
    if (savedBlend) glEnable(GL_BLEND);
    else            glDisable(GL_BLEND);
    glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
    glBindVertexArray((GLuint)savedVAO);
    glUseProgram((GLuint)savedProgram);

    // RENDER_STATES v1: indirect terrain bound textures on units 0 and 3 directly;
    // the applyRenderStates cache is now stale. Force a full re-apply on next call.
    if (g_gos_renderer) g_gos_renderer->invalidateRenderStateCache();
    return true;
}

// ──────────────────────────────────────────────────────────────────────────
// VK-OPT-4: class-D orphan-then-upload for the terrain-mask indirect command
// buffers (s_indirectCmdBuf / s_waterIndirectCmdBuf). These are per-frame
// CPU-written DrawArraysIndirect command buffers that were previously
// glBufferSubData'd WITHOUT orphaning/fencing, so the GL driver implicit-sync
// stalls (waits for the prior frame's glDrawArraysIndirect to finish reading
// the buffer before overwriting). Orphaning renames the storage so the driver
// hands back fresh memory instead of stalling — same bytes, byte-identical
// output, only the driver serialization is removed. Mirrors the reusable
// tubeOrphanUpload template from gos_particle_bridge.cpp (VK-OPT-1).
//
// The orphan reuses the buffer's fixed capacity (a single 16-byte command),
// so it is a cheap rename, not a reallocation. On Vulkan this class of
// implicit-sync buffer does not exist at all — this is also VK-migration prep.
//
// Optional, zero-cost-when-off instrumentation behind MC2_TERRAIN_MASK_UPLOAD_STATS
// makes the orphaning measurable / regression-guardable (parallels
// MC2_VFX_TUBE_UPLOAD_STATS / MC2_LIGHT_UPLOAD_STATS).
namespace {
struct MaskUploadStats {
    unsigned long long uploadBytes = 0;  // command bytes pushed this frame
    unsigned long long orphans     = 0;  // orphan-then-upload calls this frame
};
MaskUploadStats s_maskUploadStats;

bool maskUploadStatsEnabled() {
    static int cached = -1;
    if (cached < 0) {
        cached = mc2::gate::get<"MC2_TERRAIN_MASK_UPLOAD_STATS">();
    }
    return cached != 0;
}

// Orphan-then-upload the indirect command: rename storage (glBufferData NULL at
// the buffer's existing size) before the glBufferSubData so the driver does not
// implicit-sync-stall on the previous frame's indirect read. Buffer must be
// bound to `target` by the caller (it already is at the draw site).
void maskOrphanUploadCmd(GLenum target, GLsizeiptr bytes, const void* data) {
    glBufferData(target, bytes, nullptr, GL_DYNAMIC_DRAW);  // orphan/rename
    glBufferSubData(target, 0, bytes, data);
    if (maskUploadStatsEnabled()) {
        s_maskUploadStats.uploadBytes += (unsigned long long)bytes;
        ++s_maskUploadStats.orphans;
        mc2::fmt::print_err("[TERRAIN_MASK_UPLOAD_STATS] orphan+upload bytes={:d} orphans={:d} totalBytes={:d}\n", mc2::fmt::as_i64((long long)bytes), mc2::fmt::as_u64(s_maskUploadStats.orphans), mc2::fmt::as_u64(s_maskUploadStats.uploadBytes));
        std::fflush(stderr);
    }
}
}  // namespace

//
// Called by gos_terrain_mask_dispatch::DrawMaskSolid() from
// Render.TerrainMask.Solid. Reads the per-frame SOLID mask SSBO at binding 17,
// the dense recipe SSBO at binding 19, and the per-vertex lighting SSBO at
// binding 2. Issues ONE glDrawArraysIndirect covering all quadCount*6 vertices;
// the VS emits degenerate triangles for quads outside the mask.
// State save/restore mirrors gos_terrain_bridge_drawIndirect.
// ──────────────────────────────────────────────────────────────────────────
bool gos_terrain_bridge_drawMaskSolid(uint32_t solidMaskSSBO,
                                      uint32_t recipeSSBO,
                                      uint32_t lightingSSBO,
                                      int      quadCount,
                                      int      mapSide)
{
    ZoneScopedN("Terrain::MaskSolidDraw");
    if (!g_gos_renderer) return false;
    glsl_program* p = g_gos_renderer->getMaskSolidProgram();
    if (!p || !p->shp_) return false;
    if (solidMaskSSBO == 0 || recipeSSBO == 0 || quadCount <= 0 || mapSide <= 0) return false;

    const GLuint prog = p->shp_;

    GLint     savedProgram   = 0; glGetIntegerv(GL_CURRENT_PROGRAM,     &savedProgram);
    GLboolean savedBlend     = glIsEnabled(GL_BLEND);
    GLint     savedSrcRGB    = 0; glGetIntegerv(GL_BLEND_SRC_RGB,       &savedSrcRGB);
    GLint     savedDstRGB    = 0; glGetIntegerv(GL_BLEND_DST_RGB,       &savedDstRGB);
    GLint     savedDepthMask = 0; glGetIntegerv(GL_DEPTH_WRITEMASK,     &savedDepthMask);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint     savedDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC,          &savedDepthFunc);
    GLint     savedVAO       = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&savedVAO);
    GLboolean savedColorMask[4]; glGetBooleanv(GL_COLOR_WRITEMASK, savedColorMask);
    GLuint    savedSampler   = 0;
    { GLint q = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &q); savedSampler = (GLuint)q; }

    // ---- AMD VAO-0 + attr-0 traps ------------------------------------------
    extern void gos_RendererRebindVAO();
    gos_RendererRebindVAO();
    glEnableVertexAttribArray(0);

    // ---- Program + uniforms ------------------------------------------------
    // Pass overrideProg so terrainBindThinUniformsForPatchStream binds + sets
    // uniforms directly on mask_solid_prog_. All uniform names match the thin
    // shader (cache is keyed by program, so it re-fetches locations on switch).
    g_gos_renderer->terrainBindThinUniformsForPatchStream(p);

    {
        const GLint locMS = glGetUniformLocation(prog, "mapSide");
        if (locMS >= 0) glUniform1i(locMS, mapSide);
    }

    extern GLuint gos_terrain_indirect_getAtlasGLTex();
    extern float  gos_terrain_indirect_getNumTexturesAcross();
    extern float  gos_terrain_indirect_getAtlasMapTopLeftX();
    extern float  gos_terrain_indirect_getAtlasMapTopLeftY();
    extern float  gos_terrain_indirect_getAtlasOneOverWorldUnits();
    extern GLuint gos_terrain_indirect_getCementAtlasGLTex();
    extern int    gos_terrain_indirect_getCementAtlasGridSide();
    extern bool   gos_terrain_indirect_isCementAtlasReady();
    extern float  gos_terrain_indirect_getWorldUnitsPerVertex();
    extern GLuint gos_terrain_indirect_getTransitionMaskArrayGL();
    extern bool   gos_terrain_indirect_isTransitionMaskReady();

    // ---- Depth + color state -----------------------------------------------
    // Stage 1b dual-run soak: suppress all framebuffer writes so the mask draw
    // validates the pipeline (mask culling, shader compile/run, GL errors) without
    // z-fighting the concurrent PR1 indirect draw. Stage 1d flips this when PR1
    // is retired and the mask draw becomes the sole SOLID path.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);   // reverse-Z (U2): was GL_LEQUAL (mask soak 1b)
    glDepthMask(GL_FALSE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDisable(GL_BLEND);

    static GLuint s_maskSolidSampler = 0;
    if (s_maskSolidSampler == 0) {
        // TEX-CLASS: per-pass-rebind -- mask-solid sampler object
        glGenSamplers(1, &s_maskSolidSampler);
        glSamplerParameteri(s_maskSolidSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_maskSolidSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_maskSolidSampler, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_maskSolidSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s_maskSolidSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glBindSampler(0, s_maskSolidSampler);

    GLint savedTex0Binding = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex0Binding);
    glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getAtlasGLTex());
    {
        const GLint locNTA  = glGetUniformLocation(prog, "atlasNumTexturesAcross");
        const GLint locMTX  = glGetUniformLocation(prog, "atlasMapTopLeftX");
        const GLint locMTY  = glGetUniformLocation(prog, "atlasMapTopLeftY");
        const GLint locOOWS = glGetUniformLocation(prog, "atlasOneOverWorldUnits");
        const GLint locUAC  = glGetUniformLocation(prog, "useAtlasColormap");
        if (locNTA  >= 0) glUniform1f(locNTA,  gos_terrain_indirect_getNumTexturesAcross());
        if (locMTX  >= 0) glUniform1f(locMTX,  gos_terrain_indirect_getAtlasMapTopLeftX());
        if (locMTY  >= 0) glUniform1f(locMTY,  gos_terrain_indirect_getAtlasMapTopLeftY());
        if (locOOWS >= 0) glUniform1f(locOOWS, gos_terrain_indirect_getAtlasOneOverWorldUnits());
        if (locUAC  >= 0) glUniform1i(locUAC,  1);
    }

    GLint  savedTex3Binding = 0;
    GLuint savedTex3Sampler = 0;
    const bool cementReady = gos_terrain_indirect_isCementAtlasReady();
    if (cementReady) {
        glActiveTexture(GL_TEXTURE3);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex3Binding);
        { GLint q = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 3, &q); savedTex3Sampler = (GLuint)q; }
        glBindSampler(3, 0);
        glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getCementAtlasGLTex());
        glActiveTexture(GL_TEXTURE0);
    }
    {
        const GLint locTex3  = glGetUniformLocation(prog, "tex3");
        const GLint locUCA   = glGetUniformLocation(prog, "useCementAtlas");
        const GLint locGSide = glGetUniformLocation(prog, "atlasCementGridSide");
        const GLint locWUPT  = glGetUniformLocation(prog, "atlasCementWorldUnitsPerTile");
        if (locTex3  >= 0) glUniform1i(locTex3, 3);
        if (cementReady) {
            if (locUCA   >= 0) glUniform1i(locUCA,   1);
            if (locGSide >= 0) glUniform1i(locGSide, gos_terrain_indirect_getCementAtlasGridSide());
            if (locWUPT  >= 0) glUniform1f(locWUPT,  gos_terrain_indirect_getWorldUnitsPerVertex());
        } else {
            if (locUCA   >= 0) glUniform1i(locUCA,   0);
        }
    }

    GLint  savedTex4Binding2D_b    = 0;
    GLuint savedTex4Binding2DArr_b = 0;
    const bool transitionMaskReady2 = gos_terrain_indirect_isTransitionMaskReady();
    if (transitionMaskReady2) {
        glActiveTexture(GL_TEXTURE4);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex4Binding2D_b);
        { GLint q = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &q); savedTex4Binding2DArr_b = (GLuint)q; }
        glBindTexture(GL_TEXTURE_2D_ARRAY, gos_terrain_indirect_getTransitionMaskArrayGL());
        glActiveTexture(GL_TEXTURE0);
    }
    {
        const GLint locTMArr = glGetUniformLocation(prog, "u_transitionMaskArray");
        const GLint locUseTM = glGetUniformLocation(prog, "u_useTransitionMask");
        if (locTMArr >= 0) glUniform1i(locTMArr, 4);
        if (locUseTM >= 0) glUniform1i(locUseTM, transitionMaskReady2 ? 1 : 0);
    }

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 17, (GLuint)solidMaskSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 19, (GLuint)recipeSSBO);
    if (lightingSSBO != 0)
        // VK-H2-BINDING-SPLIT-1: LightingBuf moved slot 2 -> 4 (lockstep with
        // gos_terrain_mask_solid.vert). Slot 2 kept clear for ThinRecordBufFrag.
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, (GLuint)lightingSSBO);

    struct DrawArraysIndirectCommand {
        uint32_t count;
        uint32_t instanceCount;
        uint32_t first;
        uint32_t baseInstance;
    };
    static GLuint s_indirectCmdBuf = 0;
    if (s_indirectCmdBuf == 0) {
        glGenBuffers(1, &s_indirectCmdBuf);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, s_indirectCmdBuf);
        glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(DrawArraysIndirectCommand),
                     nullptr, GL_DYNAMIC_DRAW);
    }
    DrawArraysIndirectCommand cmd = { uint32_t(quadCount * 6), 1u, 0u, 0u };
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, s_indirectCmdBuf);
    // VK-OPT-4 (class-D fix): orphan-then-upload so the driver renames storage
    // instead of implicit-sync-stalling on the previous frame's indirect read.
    maskOrphanUploadCmd(GL_DRAW_INDIRECT_BUFFER, sizeof(cmd), &cmd);
    glDrawArraysIndirect(GL_TRIANGLES, nullptr);
    mdi_submit::trace("TerrainMaskSolid", "DrawArraysIndirect", 1,
                      "CPU", "s_indirectCmdBuf", false);

    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 17, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 19, 0);
    if (lightingSSBO != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, 0);  // VK-H2-BINDING-SPLIT-1: LightingBuf slot 2 -> 4

    // Reset useAtlasColormap on this program so re-entry doesn't inherit atlas mode.
    {
        const GLint locUAC  = glGetUniformLocation(prog, "useAtlasColormap");
        const GLint locUCA  = glGetUniformLocation(prog, "useCementAtlas");
        const GLint locUseTM = glGetUniformLocation(prog, "u_useTransitionMask");
        if (locUAC  >= 0) glUniform1i(locUAC,  0);
        if (locUCA  >= 0) glUniform1i(locUCA,  0);
        if (locUseTM >= 0) glUniform1i(locUseTM, 0);
    }

    if (cementReady) {
        glActiveTexture(GL_TEXTURE3);
        glBindSampler(3, savedTex3Sampler);
        glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex3Binding);
        glActiveTexture(GL_TEXTURE0);
    }
    if (transitionMaskReady2) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D_ARRAY, savedTex4Binding2DArr_b);
        glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex4Binding2D_b);
        glActiveTexture(GL_TEXTURE0);
    }
    glBindSampler(0, savedSampler);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex0Binding);
    glColorMask(savedColorMask[0], savedColorMask[1], savedColorMask[2], savedColorMask[3]);
    glDepthMask((GLboolean)savedDepthMask);
    glDepthFunc((GLenum)savedDepthFunc);
    if (savedDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (savedBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
    glBindVertexArray((GLuint)savedVAO);
    glUseProgram((GLuint)savedProgram);

    // Mark render-state cache stale since we bound textures + program directly.
    if (g_gos_renderer) g_gos_renderer->invalidateRenderStateCache();
    return true;
}

//
// Called by gos_terrain_mask_dispatch::DrawMaskWater() from
// Render.TerrainMask.Water. Reads the per-frame water mask SSBO at binding 18,
// the WaterRecipe SSBO at binding 5, and the per-vertex lighting SSBO at
// binding 2. Issues ONE glDrawArraysIndirect covering all recipeCount*6 vertices;
// the VS emits degenerate triangles for quads outside the mask.
// State save/restore mirrors gos_terrain_bridge_drawMaskSolid.
// ──────────────────────────────────────────────────────────────────────────
bool gos_terrain_bridge_drawMaskWater(uint32_t waterMaskSSBO,
                                      uint32_t recipeSSBO,
                                      uint32_t lightingSSBO,
                                      int      recipeCount,
                                      float    waterElevation,
                                      float    frameCos)
{
    ZoneScopedN("Terrain::MaskWaterDraw");
    if (!g_gos_renderer) return false;
    glsl_program* p = g_gos_renderer->getMaskWaterProgram();
    if (!p || !p->shp_) return false;
    if (waterMaskSSBO == 0 || recipeSSBO == 0 || recipeCount <= 0) return false;

    const GLuint prog = p->shp_;

    GLint     savedProgram   = 0; glGetIntegerv(GL_CURRENT_PROGRAM,      &savedProgram);
    GLboolean savedBlend     = glIsEnabled(GL_BLEND);
    GLint     savedSrcRGB    = 0; glGetIntegerv(GL_BLEND_SRC_RGB,        &savedSrcRGB);
    GLint     savedDstRGB    = 0; glGetIntegerv(GL_BLEND_DST_RGB,        &savedDstRGB);
    GLint     savedDepthMask = 0; glGetIntegerv(GL_DEPTH_WRITEMASK,      &savedDepthMask);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint     savedDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC,           &savedDepthFunc);
    GLint     savedVAO       = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVAO);
    GLboolean savedColorMask[4]; glGetBooleanv(GL_COLOR_WRITEMASK, savedColorMask);
    GLuint    savedSampler   = 0;
    { GLint q = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &q); savedSampler = (GLuint)q; }

    // ---- AMD VAO-0 + attr-0 traps ------------------------------------------
    extern void gos_RendererRebindVAO();
    gos_RendererRebindVAO();
    glEnableVertexAttribArray(0);

    // ---- Program + uniforms ------------------------------------------------
    // terrainBindThinUniformsForPatchStream sets terrainMVP, mvp,
    // and other terrain-shared uniforms (same uniform names as thin/mask-solid).
    g_gos_renderer->terrainBindThinUniformsForPatchStream(p);

    // waterElevation and frameCos are not set by terrainBindThinUniformsForPatchStream.
    {
        const GLint locWE = glGetUniformLocation(prog, "waterElevation");
        const GLint locFC = glGetUniformLocation(prog, "frameCos");
        if (locWE >= 0) glUniform1f(locWE, waterElevation);
        if (locFC >= 0) glUniform1f(locFC, frameCos);
    }

    // ---- Depth + color state -----------------------------------------------
    // Stage 1c dual-run soak: suppress all framebuffer writes so the mask draw
    // validates the pipeline without z-fighting the concurrent water fast path.
    // Water is translucent so depth writes are off anyway; keep depth test for
    // correct mask geometry.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);   // reverse-Z (U2): was GL_LEQUAL (mask soak 1c)
    glDepthMask(GL_FALSE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    // Blend on (water is semi-transparent); blend func matches water fast path.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    static GLuint s_maskWaterSampler = 0;
    if (s_maskWaterSampler == 0) {
        // TEX-CLASS: per-pass-rebind -- mask-water sampler object
        glGenSamplers(1, &s_maskWaterSampler);
        glSamplerParameteri(s_maskWaterSampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glSamplerParameteri(s_maskWaterSampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glSamplerParameteri(s_maskWaterSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s_maskWaterSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glBindSampler(0, s_maskWaterSampler);

    // ---- Bind a valid texture at unit 0 (terrain atlas) to prevent undefined sampling.
    // glColorMask(GL_FALSE) suppresses output, but we still bind something safe.
    extern GLuint gos_terrain_indirect_getAtlasGLTex();
    GLint savedTex0Binding = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex0Binding);
    glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getAtlasGLTex());

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 18, (GLuint)waterMaskSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,  5, (GLuint)recipeSSBO);
    if (lightingSSBO != 0)
        // VK-H2-BINDING-SPLIT-1: LightingBuf moved slot 2 -> 4 (lockstep with
        // gos_terrain_mask_water.vert). Slot 2 kept clear for ThinRecordBufFrag.
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, (GLuint)lightingSSBO);

    struct DrawArraysIndirectCommand {
        uint32_t count;
        uint32_t instanceCount;
        uint32_t first;
        uint32_t baseInstance;
    };
    static GLuint s_waterIndirectCmdBuf = 0;
    if (s_waterIndirectCmdBuf == 0) {
        glGenBuffers(1, &s_waterIndirectCmdBuf);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, s_waterIndirectCmdBuf);
        glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(DrawArraysIndirectCommand),
                     nullptr, GL_DYNAMIC_DRAW);
    }
    DrawArraysIndirectCommand cmd = { uint32_t(recipeCount * 6), 1u, 0u, 0u };
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, s_waterIndirectCmdBuf);
    // VK-OPT-4 (class-D fix): orphan-then-upload so the driver renames storage
    // instead of implicit-sync-stalling on the previous frame's indirect read.
    maskOrphanUploadCmd(GL_DRAW_INDIRECT_BUFFER, sizeof(cmd), &cmd);
    glDrawArraysIndirect(GL_TRIANGLES, nullptr);
    mdi_submit::trace("TerrainMaskWater", "DrawArraysIndirect", 1,
                      "CPU", "s_waterIndirectCmdBuf", false);

    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 18, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,  5, 0);
    if (lightingSSBO != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, 0);  // VK-H2-BINDING-SPLIT-1: LightingBuf slot 2 -> 4

    glBindSampler(0, savedSampler);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex0Binding);
    glColorMask(savedColorMask[0], savedColorMask[1], savedColorMask[2], savedColorMask[3]);
    glDepthMask((GLboolean)savedDepthMask);
    glDepthFunc((GLenum)savedDepthFunc);
    if (savedDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (savedBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
    glBindVertexArray((GLuint)savedVAO);
    glUseProgram((GLuint)savedProgram);

    // Mark render-state cache stale since we bound textures + program directly.
    if (g_gos_renderer) g_gos_renderer->invalidateRenderStateCache();
    return true;
}

//
// Issues ONE glDrawArrays(GL_TRIANGLES) against the mission-static MineStaticVBO,
// with the 2-layer mine sprite array bound at unit 5. State save/restore mirrors
// gos_terrain_bridge_drawIndirect (Program, Blend, Depth, ColorMask, VAO,
// sampler unit 5, texture binding on unit 5, vertex attrib state). Per-frame
// CPU work is dominated by this state churn — the actual draw is one dispatch
// over a few hundred verts max (mines are sparse: 2-3 missions x small
// overlay each per user direction 2026-05-08).
// ──────────────────────────────────────────────────────────────────────────
bool gos_terrain_bridge_drawMineStatic(int          vertCount,
                                       unsigned int vboGL,
                                       unsigned int textureArrayGL)
{
    ZoneScopedN("Terrain::MineStaticDraw");
    if (!g_gos_renderer) return false;
    if (vertCount <= 0 || vboGL == 0 || textureArrayGL == 0) return false;
    glsl_program* p = g_gos_renderer->getMineStaticProgram();
    if (!p || !p->shp_) return false;

    const GLuint prog = p->shp_;

    GLint     savedProgram   = 0; glGetIntegerv(GL_CURRENT_PROGRAM,         &savedProgram);
    GLboolean savedBlend     = glIsEnabled(GL_BLEND);
    GLint     savedSrcRGB    = 0; glGetIntegerv(GL_BLEND_SRC_RGB,           &savedSrcRGB);
    GLint     savedDstRGB    = 0; glGetIntegerv(GL_BLEND_DST_RGB,           &savedDstRGB);
    GLint     savedDepthMask = 0; glGetIntegerv(GL_DEPTH_WRITEMASK,         &savedDepthMask);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint     savedDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC,              &savedDepthFunc);
    GLint     savedVAO       = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING,    &savedVAO);
    GLint     savedActiveTex = GL_TEXTURE0; glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTex);
    GLboolean savedColorMask[4]; glGetBooleanv(GL_COLOR_WRITEMASK,           savedColorMask);
    GLint     savedArrayBuf  = 0; glGetIntegerv(GL_ARRAY_BUFFER_BINDING,    &savedArrayBuf);
    GLint     savedTex5Bind  = 0;
    GLuint    savedTex5Sampler = 0;
    glActiveTexture(GL_TEXTURE5);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &savedTex5Bind);
    { GLint q = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 5, &q); savedTex5Sampler = (GLuint)q; }

    // ---- VAO rebind (AMD VAO-0 trap) ---------------------------------------
    extern void gos_RendererRebindVAO();
    gos_RendererRebindVAO();

    glUseProgram(prog);

    // terrainMVP (mirrors PR1 thin-VS upload — GL_FALSE + row-major per
    // terrain_mvp_gl_false.md).
    {
        const GLint loc = glGetUniformLocation(prog, "u_worldToClipGL");
        if (loc >= 0)
            glUniformMatrix4fv(loc, 1, GL_FALSE,
                               (const float*)&g_gos_renderer->getTerrainMVP());
    }

    {
        const GLint loc = glGetUniformLocation(prog, "mineSpriteArray");
        if (loc >= 0) glUniform1i(loc, 5);
    }

    // ---- Vertex attrib setup (GL_ARRAY_BUFFER + 3 attrs at locations 0/1/2)
    // Vertex format must match MineVert in gos_terrain_indirect.cpp:
    //   offset  0: vec3 pos    (location 0)
    //   offset 12: vec2 uv     (location 1)
    //   offset 20: uint layer  (location 2)
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)vboGL);
    constexpr GLsizei kStride = 24;  // sizeof(MineVert)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, kStride, (const void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, kStride, (const void*)12);
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT,    kStride, (const void*)20);

    glActiveTexture(GL_TEXTURE5);
    glBindSampler(5, 0);  // use the texture's own params (NEAREST/CLAMP_TO_EDGE)
    glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)textureArrayGL);
    glActiveTexture(GL_TEXTURE0);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);   // reverse-Z (U2): was GL_LEQUAL (scene terrain)
    glDepthMask(GL_TRUE);
    // M5: undo any prior shadow-pass glColorMask(FALSE,...) leakage.
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    // Mine sprite uses alpha-test discard (in FS); no blend needed.
    glDisable(GL_BLEND);

    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)vertCount);

    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(1);
    // Leave attr-0 enabled — AMD attribute-0 trap (other paths assume it on).

    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)savedTex5Bind);
    glBindSampler(5, savedTex5Sampler);
    glActiveTexture((GLenum)savedActiveTex);

    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)savedArrayBuf);
    glColorMask(savedColorMask[0], savedColorMask[1], savedColorMask[2], savedColorMask[3]);
    glDepthFunc((GLenum)savedDepthFunc);
    glDepthMask((GLboolean)savedDepthMask);
    if (savedDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (savedBlend)     glEnable(GL_BLEND);      else glDisable(GL_BLEND);
    glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
    glBindVertexArray((GLuint)savedVAO);
    glUseProgram((GLuint)savedProgram);

    // RENDER_STATES v1: invalidate cache since we touched units + state.
    if (g_gos_renderer) g_gos_renderer->invalidateRenderStateCache();
    return true;
}

// Slice A — cement-overlay static-bake draw bridge. Thin forwarder to the
// gosRenderer member (which owns the private overlay program/locs/texture
// list), mirroring how gos_DrawTerrainOverlays forwards to drawTerrainOverlays
// and how gos_terrain_bridge_drawMineStatic gates on g_gos_renderer.
bool gos_terrain_bridge_drawDecalStatic(unsigned int               vboGL,
                                        const GosDecalStaticDraw*  draws,
                                        int                        drawCount)
{
    ZoneScopedN("Terrain::DecalStaticDraw");
    if (!g_gos_renderer) return false;
    return g_gos_renderer->drawDecalStaticBatch(vboGL, draws, drawCount);
}
