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
unsigned int gos_GetTerrainNormalArrayTex() {
    gosRenderer* r = getGosRenderer();
    return r ? r->getTerrainNormalArrayTexEnsureBuilt() : 0u;
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
