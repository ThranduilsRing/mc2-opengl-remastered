// gos_terrain_lod_chunk: gos_TerrainLodChunk_SubmitDrawCommands, the uniform mirroring it uses and the eager draw tuning caches.
// Declared in the class headers; shared names in gos_terrain_lod_chunk_internal.h.
#include "gos_terrain_lod_chunk_internal.h"
#include "mc2fmt.h"

// TERRAIN-VISUAL-HEIGHT-S2-ALLLOD: per-LOD-band displaced-chunk counters, reset
// and logged once per submit alongside [TerrainLOD v1] so acceptance has hard
// per-band numbers. Index = LOD level (0..5), same mapping as [TerrainLOD v1].
static int s_visualDisplacedCounts[6] = {0, 0, 0, 0, 0, 0};
static int      s_submitZeroStreak  = 0;  // Phase 7.5: consecutive frames with count==0
// Mirror gameos_graphics.cpp's file-static terrain shadow texture units (9/10).
static constexpr GLint kChunkTexUnitStaticShadow  = 9;
static constexpr GLint kChunkTexUnitDynamicShadow = 10;
static constexpr GLint kChunkTexUnitDynFullMap    = 13;  // free unit (chunk uses up to 11)
static constexpr GLint kChunkTexUnitMatNormalArray = 5;
static constexpr GLint kChunkTexUnitMatAlbedoArray = 4;
static constexpr GLint kChunkTexUnitCement = 3;  // matches legacy tex3
static constexpr GLint kChunkTexUnitTransitionMask = 11;

// MC2_TERRAIN_LOD_CHECKER_DIAG (default ON, read once, static-cached). When ON,
// the per-quad triangle diagonal honors the same per-cell checkerboard used by
// grounding (worldQuadUVMode, mapdata.cpp:116), the road/cement overlay bake
// (gos_terrain_indirect.cpp:4404) and water/shadow/GPU-compute terrain, instead
// of the fixed TL-BR diagonal. The chunk render was the sole TL-BR outlier, which
// made roads/cement vanish on slopes (user-confirmed fixed). Set =0 to revert to
// the prior fixed TL-BR behaviour (killswitch).
bool terrainLodCheckerDiagEnabled()
{
    static const bool s_on = mc2::gate::get<"MC2_TERRAIN_LOD_CHECKER_DIAG">();
    return s_on;
}

// Locals the terrain LOD-chunk submit phases share: the GL state saved for the restore, the RAII state guards
// (destroyed after the restore, in reverse declaration order, as they were as locals) and the feature flags.
struct TerrainLodSubmitFrame {
    GLint prevProg;
    GLint prevVAO;
    bool useGuards;
    std::optional<mc2gl::GlScopedCapability> gDepthTest;
    std::optional<mc2gl::GlScopedCapability> gBlend;
    std::optional<mc2gl::GlScopedCapability> gCull;
    std::optional<mc2gl::GlScopedDepthState> gDepthState;
    GLboolean prevCullFace;
    GLboolean prevDepthTest;
    GLboolean prevDepthMask;
    GLboolean prevBlend;
    GLint prevDepthFunc;
    bool visualDisplaceActive;
    bool visualDampActive;
    bool geomorphActive;
#ifdef MC2_VULKAN_ISLAND
    bool s_vkTerrainEnabled;
#endif
#ifdef MC2_VULKAN_ISLAND
    bool s_vkTerrainActive;
#endif

    void saveGlStateAndGuards();
    void bindProgramAndVisualHeight(const float* mvp);
    void countSubmitAndPrepVk(int count, const float* mvp, int& s_submitCount);
    void drawChunks(int count, const TerrainDrawCommand* cmds, const float* skirtDepths, const unsigned char* skirtEdgeMasks, const unsigned int* edgeStitch, const int* shadowTiers, const float* morphFactors);
    void markDrawn();
    void restoreGlState();
    void submitVkIsland(int& s_submitCount);
};


// Save the GL state the draw changes and apply the terrain depth/blend/cull state.
void TerrainLodSubmitFrame::saveGlStateAndGuards() {
    prevProg = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    prevVAO = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);

    // Phase 10.3: chunk terrain is OPAQUE and must EXPLICITLY own depth/blend/
    // cull state. The driver previously inherited GL_DEPTH_TEST / glDepthMask /
    // GL_BLEND / glDepthFunc / GL_CULL_FACE from whatever pass ran before. If a
    // prior transparent/overlay pass left depth WRITES off (glDepthMask FALSE)
    // or blend on, the terrain top renders color but writes NO depth -> never
    // occludes -> "transparent, see-through to the skirts / lower terrain",
    // flipping with draw order and mech-selection (which changes the prior
    // pass). Confirmed independent of frag output (diag7). The opaque reverse-Z
    // state is set explicitly here and restored at the end.
    //
    // GlStateGuard slice 2: when MC2_GLSTATEGUARD_TERRAIN is on (default), RAII
    // guards (mc2gl::GlScoped*) own that save/set/restore — the function-scope
    // optionals capture prev in ctor and restore in dtor at the closing brace
    // (no gos_InvalidateRenderStateCache() here, so function scope is correct).
    // =0 reverts to the legacy hand-rolled path below, byte-for-byte. cull is
    // disabled for the WHOLE draw (terrainMVP bakes the GL-NDC X-flip -> winding
    // is inverted -> the terrain TOP would be culled as a backface; terrain is
    // an opaque heightfield so double-sided is free).
    static const bool s_depthAlways = mc2::gate::get<"MC2_TERRAIN_LOD_DEPTH_ALWAYS">();
    const GLenum s_wantDepthFunc = s_depthAlways ? GL_ALWAYS : GL_GEQUAL;
    useGuards = glStateGuardTerrainEnabled();

    
    prevCullFace = glIsEnabled(GL_CULL_FACE);
    prevDepthTest = GL_FALSE;
    prevDepthMask = GL_TRUE;
    prevBlend = GL_FALSE;
    prevDepthFunc = GL_GEQUAL;

    if (useGuards) {
        gDepthTest.emplace(GL_DEPTH_TEST, /*enable=*/true);
        gDepthState.emplace(GL_TRUE, s_wantDepthFunc);
        gBlend.emplace(GL_BLEND, /*enable=*/false);
        gCull.emplace(GL_CULL_FACE, /*enable=*/false);
    } else {
        prevDepthTest = glIsEnabled(GL_DEPTH_TEST);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
        prevBlend     = glIsEnabled(GL_BLEND);
        glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDepthFunc(s_wantDepthFunc);   // reverse-Z opaque terrain
        glDisable(GL_CULL_FACE);        // double-sided (see comment above)
    }
}

// Apply the terrain pipeline, bind the program, VAO and height SSBOs and set the displacement uniforms.
void TerrainLodSubmitFrame::bindProgramAndVisualHeight(const float* mvp) {
    // TERRAIN-LODCHUNK-APPLYPIPELINE-ROUTING-1: make the TerrainSolid pipeline row the
    // AUTHORITATIVE fixed-function state for the live terrain-solid path (this LOD-chunk
    // draw is the default; the bridge is suppressed by mc2TerrainLodChunkEnabled). Both
    // branches above already SET + save/restore the identical state (RAII guards when
    // MC2_GLSTATEGUARD_TERRAIN on, manual otherwise); this re-asserts it from the row so
    // the state is row-owned and [PIPELINE_BIND] TerrainSolid fires (the proof hook). It
    // is byte-identical: depth ON / GEQUAL / depthWrite ON / blend Opaque / cull None, and
    // the row's frontFace=Ccw + polygonOffset=off are the ambient defaults (no-ops).
    // colorMask is owned by the COLORMASK-ROLLOUT-1 beginScene keystone. glProgramName=0
    // -> glUseProgram(s_terrainProgram) below still binds the program. (The default-OFF
    // s_depthAlways debug override is intentionally not modeled — the row forces GEQUAL.)
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::TerrainSolid), "TerrainSolid");

    glUseProgram(s_terrainProgram);
    glBindVertexArray(s_patchVao);

    // (Depth/blend/cull state is owned above — RAII guards when
    // MC2_GLSTATEGUARD_TERRAIN is on, else the legacy explicit calls. Cull is
    // disabled for the whole draw: terrainMVP bakes the GL-NDC X-flip so winding
    // is inverted and the opaque heightfield is rendered double-sided.)

    s_heightSsbo.bindBase(TERRAIN_HEIGHT_SSBO_BINDING);
    // Step 5b: terrainType SSBO (concrete). 0 if never uploaded -> vert reads 0.
    if (s_typeSsbo.created())
        s_typeSsbo.bindBase(TERRAIN_TYPE_SSBO_BINDING);
    if (s_cementSsbo.created())
        s_cementSsbo.bindBase(TERRAIN_CEMENT_SSBO_BINDING);

    // TERRAIN-VISUAL-HEIGHT-SAMPLE-1: resolve geometry-displacement activation once
    // per frame. Active = MC2_TERRAIN_VISUAL_DISPLACE on AND the 4x bake loaded.
    // Bind binding 26 for the whole draw; the per-chunk u_visualDisplace gates the
    // near (LOD0) band only. Default OFF -> u_visualDisplace stays 0 -> byte-identical.
#ifdef MC2_VULKAN_ISLAND
    const bool s_visualDisplaceGate = terrainVisualDisplaceGateOn();  // rung 5: shared helper (same env read)
#else
    static const bool s_visualDisplaceGate = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE">();
#endif
    visualDisplaceActive = s_visualDisplaceGate && s_visualHeightSsbo.created() && s_visualSide > 0;
    if (visualDisplaceActive)
        s_visualHeightSsbo.bindBase(TERRAIN_VISUAL_HEIGHT_SSBO_BINDING);
    if (s_locVisualSide >= 0)
        glUniform1i(s_locVisualSide, s_visualSide);
    // TERRAIN-REAUTH-UNPIN-1 Half B: near-object displacement fade. Active only
    // when displacing AND the objfade gate is on (default ON when displacing —
    // it is the safety) AND a damp map for THIS map size was uploaded. Binds
    // binding 27 for the whole draw; u_visualDampOn=0 => shader never reads it.
    static const bool s_visualDampGate = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE">();
    visualDampActive = visualDisplaceActive && s_visualDampGate
                                  && s_visualDampSsbo.created()
                                  && s_visualDampSide == s_mapSide;
    if (visualDampActive)
        s_visualDampSsbo.bindBase(TERRAIN_VISUAL_DAMP_SSBO_BINDING);
    if (s_locVisualDampOn >= 0)
        glUniform1i(s_locVisualDampOn, visualDampActive ? 1 : 0);
    // TERRAIN-VISUAL-HEIGHT-S2-ALLLOD: far-band displacement fade, 0..1, default 1
    // (full displacement). Only scales the coarser-band (u_visualDisplace==2) verts;
    // LOD0 (mode 1) is untouched by this knob.
#ifdef MC2_VULKAN_ISLAND
    const float s_visualDisplaceFar = terrainVisualDisplaceFarParam();  // rung 5: shared helper (same env read)
#else
    static const float s_visualDisplaceFar = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE_FAR">();
#endif
    if (s_locVisualDisplaceFar >= 0)
        glUniform1f(s_locVisualDisplaceFar, s_visualDisplaceFar);

    // TERRAIN-LOD-GEOMORPH-1: coarse-band vertices read their OWN max-mip level
    // (silhouette keeps peaks) when the bake shipped mips. Rides the same
    // MC2_TERRAIN_VISUAL_DISPLACE gate (visualDisplaceActive); mips absent ->
    // uploads 0 -> vert branch skipped (S2 behavior verbatim).
    // MC2_TERRAIN_LOD_GEOMORPH=0 is the killswitch when mips ARE present.
    static const bool s_geomorphKill = !mc2::gate::get<"MC2_TERRAIN_LOD_GEOMORPH">();
    geomorphActive =
        visualDisplaceActive && s_visualMipFloats > 0 && !s_geomorphKill;
    if (s_locGeomorphMips >= 0)
        glUniform1i(s_locGeomorphMips, geomorphActive ? 1 : 0);
    // One-shot draw-time truth line so a dead geomorph is diagnosable from the
    // console instead of a pixel A/B (mirrors [VISUAL_HEIGHT v1] cadence).
    {
        static bool s_geoLogged = false;
        if (!s_geoLogged && visualDisplaceActive) {
            s_geoLogged = true;
            mc2::fmt::print_out("[GEOMORPH v1] active={:d} mipFloats={:d} kill={:d} locMips={:d} locMorph={:d} locLodStep={:d}\n", mc2::fmt::as_i32(geomorphActive ? 1 : 0), mc2::fmt::as_i32(s_visualMipFloats), mc2::fmt::as_i32(s_geomorphKill ? 1 : 0), mc2::fmt::as_i32((int)s_locGeomorphMips), mc2::fmt::as_i32((int)s_locMorphFactor), mc2::fmt::as_i32((int)s_locLodStep));
            fflush(stdout);
        }
    }

    if (s_locMapSide >= 0)
        glUniform1i(s_locMapSide, s_mapSide);
    if (s_locHalfMap >= 0)
        glUniform1f(s_locHalfMap, s_halfMap);
    if (s_locMvp >= 0)
        glUniformMatrix4fv(s_locMvp, 1, GL_FALSE, mvp);

    // Phase 7.5: neon force-color mode — set once per frame before the patch loop.
    // REDUNDANT-PASS-HUNT-1: env resolved once (kTglcForceColor), not per frame.
    if (s_locForceColor >= 0)
        glUniform1i(s_locForceColor, kTglcForceColor ? 1 : 0);

    // Bisection bitmask uniform (MC2_TERRAIN_LOD_CHUNK_DIAG): 1=no GBuffer1,
    // 2=no depth fudge, 4=no lighting. Shader-side A/B without rebuilding.
    // REDUNDANT-PASS-HUNT-1: env resolved once (kTglcDiag), not per frame.
    if (s_locDiag >= 0)
        glUniform1i(s_locDiag, kTglcDiag);

    // LIGHTING-DEBUG-VIEWS-1A-CHUNK: unified lighting debug channel (40-series),
    // the SAME enum as static_prop / gos_terrain.frag. Separate uniform from
    // u_diag (which is a bitmask, so reusing it would mis-trigger bits). Resolver
    // returns -1 when MC2_LIGHTING_DEBUG_VIEW is unset/unknown -> upload 0 ->
    // shader skips all channels (pixel-invariant default).
    if (s_locLightDebugView >= 0) {
        extern int mc2LightingDebugMode();
        int lvm = mc2LightingDebugMode();
        glUniform1i(s_locLightDebugView, lvm < 0 ? 0 : lvm);
    }

    // MC2_SHADER_PATH_TINT: solid GREEN for the chunk terrain path (default 0 = OFF).
    if (s_locPathTint >= 0)
        glUniform1i(s_locPathTint, mc2ShaderPathTint());
}

// Bind the colormap atlas (with its UV params), the shadow maps and the light matrices.
static void bindAtlasAndShadowMaps() {
    // Phase 10 (Step 1a): bind the merged colormap atlas (tex1) on unit 0 and
    // feed the atlas-UV reconstruction params (same source as the legacy
    // gos_terrain.frag useAtlasColormap path). When the atlas is not yet built
    // (g_atlasGLTex==0) the sampler reads the default texture -> dark; the
    // colormap pipeline normally has it ready by first in-mission frame.
    {
        const GLuint atlasTex = gos_terrain_indirect_getAtlasGLTex();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, atlasTex);
        if (s_locColormap >= 0) glUniform1i(s_locColormap, 0);
        if (s_locAtlasTLX >= 0) glUniform1f(s_locAtlasTLX, gos_terrain_indirect_getAtlasMapTopLeftX());
        if (s_locAtlasTLY >= 0) glUniform1f(s_locAtlasTLY, gos_terrain_indirect_getAtlasMapTopLeftY());
        if (s_locAtlasOOW >= 0) glUniform1f(s_locAtlasOOW, gos_terrain_indirect_getAtlasOneOverWorldUnits());
        if (s_locLightDir >= 0) {
            float lx = 0.f, ly = 0.f, lz = 1.f;
            gos_GetTerrainLightDir(&lx, &ly, &lz);
            glUniform4f(s_locLightDir, lx, ly, lz, 0.0f);
        }
        // CEMENT-NIGHT-LIGHT-1: mission flat-pad light for the RAW cement-atlas
        // texels (night darkness is burned into the colormap CPU-side; the
        // cement atlas is built from unburned tile art). MUST be uploaded by
        // every binder of terrain_lod_chunk.frag or draws read 0 -> black pads.
        // (The tess variant program auto-mirrors this via mirrorTerrainUniforms.)
        if (s_locMissionFlatLight >= 0) {
            float fr = 1.f, fg = 1.f, fb = 1.f;
            gos_GetTerrainMissionFlatLight(&fr, &fg, &fb);
            glUniform3f(s_locMissionFlatLight, fr, fg, fb);
        }
        // PAD-AREA-GLOW-1: premultiplied night pad-area lift. nightFactor is
        // derived from the SAME mission flat light CEMENT-NIGHT-LIGHT-1 uses
        // (1 - brightest channel): day missions upload ~(1,1,1) -> factor 0 ->
        // u_padGlow = exact (0,0,0) -> byte-identical. Warm color matches the
        // building SpotLight_ register tint direction (0xffe8c870-ish).
        if (s_locPadGlow >= 0) {
            // VK-PARITY-TERRAIN-2: premultiply moved verbatim into the shared
            // producer gos_TerrainPadGlowRGB() (VK terrain island reads it too).
            float pg[3];
            gos_TerrainPadGlowRGB(pg);
            glUniform3f(s_locPadGlow, pg[0], pg[1], pg[2]);
            if (s_locPadGlowSpill >= 0)
                glUniform1f(s_locPadGlowSpill, gos_TerrainPadGlowSpillWu());
        }
        // NIGHT-LIGHT-POOLS-1: bind
        // the per-frame terrain light pool SSBO (packed+uploaded every frame by
        // gos_terrain_lighting::PackAndDispatch — the SAME buffer the legacy
        // vertex-lighting compute reads) at binding 28 and tell the frag how
        // many lights it holds. Gate=0, producer disabled
        // (MC2_TERRAIN_LIGHTING_GPU=0), or zero registered lights this frame
        // -> u_numPoolLights force-uploads 0 -> the frag loop is never entered
        // -> byte-identical output (the killswitch contract).
        {
            // VK-PARITY-TERRAIN-2: gate + 256 cap now resolved by the shared
            // producer gos_TerrainLightPoolCount() (VK terrain island reads it too).
            const int poolCount = gos_TerrainLightPoolCount();
            if (poolCount > 0)
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER,
                                 TERRAIN_LIGHT_POOL_SSBO_BINDING,
                                 gos_terrain_lighting::GetLightPoolSsbo());
            if (s_locNumPoolLights >= 0)
                glUniform1i(s_locNumPoolLights, poolCount);
            // LIGHT-CLUSTER-PERF-1: bind this frame's world-space XY light grid so
            // the frag loop visits only the lights whose ground circle covers the
            // fragment, instead of the whole (now 256-slot) pool. dim=0 uploads
            // when the grid is inactive (MC2_LIGHT_GRID=0 killswitch, small pool,
            // or no pool this frame) -> frag takes the exact linear loop.
            {
                gos_terrain_lighting::LightGridParams lg{};
                bool haveGrid = (poolCount > 0) &&
                                gos_terrain_lighting::GetLightGridParams(lg);
                if (haveGrid) {
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,
                                     TERRAIN_LIGHT_GRID_HEADER_SSBO_BINDING, lg.headerSsbo);
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,
                                     TERRAIN_LIGHT_GRID_INDEX_SSBO_BINDING, lg.indexSsbo);
                }
                if (s_locLightGridDim >= 0)
                    glUniform1i(s_locLightGridDim, haveGrid ? static_cast<GLint>(lg.dim) : 0);
                if (haveGrid) {
                    if (s_locLightGridMin >= 0)
                        glUniform2f(s_locLightGridMin, lg.minXY[0], lg.minXY[1]);
                    if (s_locLightGridInvCell >= 0)
                        glUniform2f(s_locLightGridInvCell, lg.invCell[0], lg.invCell[1]);
                }
            }
            // NIGHT-LIGHT-POOLS-2 tuning (user feedback 2026-07-21): soften the
            // stamped-disc falloff edge (smoothstep tail; MC2_LIGHT_POOL_SOFT=0
            // -> exact legacy TG_Light::GetFalloff linear ramp) and dim
            // mover-attached lights (MC2_LIGHT_POOL_MOVER_DIM, default 0.5;
            // producer flags mover lights in the SSBO pad).
            if (s_locLightPoolSoft >= 0)
                glUniform1i(s_locLightPoolSoft, kTglcLightPoolSoft ? 1 : 0);
            if (s_locLightPoolMoverDim >= 0)
                glUniform1f(s_locLightPoolMoverDim, kTglcLightPoolMoverDim);
            // NIGHT-LIGHT-POOLS-3: summed-pool tone compression toggle.
            if (s_locLightPoolCompress >= 0)
                glUniform1i(s_locLightPoolCompress, kTglcLightPoolCompress ? 1 : 0);
            // NIGHT-LIGHT-POOLS-4: mover radius scale + max-blend.
            if (s_locLightPoolMoverScale >= 0)
                glUniform1f(s_locLightPoolMoverScale, kTglcLightPoolMoverRadius);
            if (s_locLightPoolMoverMax >= 0)
                glUniform1i(s_locLightPoolMoverMax, kTglcLightPoolMoverMax ? 1 : 0);
            if (s_locMoverContactShadow >= 0)
                glUniform1f(s_locMoverContactShadow, kTglcMoverContactShadow);
        }
    }

    // Phase 10 Step 1c: bind the shadow maps + light matrices that
    // include/shadow.hglsl reads. The chunk draw is a bolt-on — like the GL depth
    // state, it MUST set these or enableShadows defaults to 0 and calcShadow
    // returns 1.0 (no shadows). Mirrors gosRenderer::terrainBindShadowUniforms.
    {
        gosPostProcess* pp = getGosPostProcess();
        if (pp && pp->shadowsEnabled_) {
            if (s_locLightSpaceMatrix >= 0)
                glUniformMatrix4fv(s_locLightSpaceMatrix, 1, GL_FALSE, pp->getLightSpaceMatrix());
            if (s_locEnableShadows >= 0)  glUniform1i(s_locEnableShadows, 1);
            if (s_locShadowSoftness >= 0) glUniform1f(s_locShadowSoftness, 2.5f);  // shadow.hglsl default
            if (s_locShadowMap >= 0) {
                glUniform1i(s_locShadowMap, kChunkTexUnitStaticShadow);
                glActiveTexture(GL_TEXTURE0 + kChunkTexUnitStaticShadow);
                glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture());
                glActiveTexture(GL_TEXTURE0);
            }
            if (pp->getDynamicShadowFBO()) {
                if (s_locEnableDynShadows >= 0) glUniform1i(s_locEnableDynShadows, 1);
                if (mc2ShadowCsmEnabled() && pp->getDynamicShadowArrayTexture()) {
                    if (s_locDynCascadeMats >= 0)
                        glUniformMatrix4fv(s_locDynCascadeMats, pp->getDynamicShadowCascadeCount(),
                                           GL_FALSE, pp->getDynamicCascadeMatrices());
                    if (s_locDynCsmCount >= 0) glUniform1i(s_locDynCsmCount, pp->getDynamicShadowCascadeCount());
                    if (s_locDynCascadeTexel >= 0)
                        glUniform1fv(s_locDynCascadeTexel, pp->getDynamicShadowCascadeCount(),
                                     pp->getDynamicCascadeTexelWorld());
                    if (s_locCsmDepthSpan >= 0)
                        glUniform1f(s_locCsmDepthSpan, pp->getCsmDepthSpan());
                    if (s_locDynShadowArray >= 0) {
                        glUniform1i(s_locDynShadowArray, kChunkTexUnitDynamicShadow);
                        glActiveTexture(GL_TEXTURE0 + kChunkTexUnitDynamicShadow);
                        glBindTexture(GL_TEXTURE_2D_ARRAY, pp->getDynamicShadowArrayTexture());
                        glActiveTexture(GL_TEXTURE0);
                    }
                    if (s_locDynFullMapTexel >= 0)
                        glUniform1f(s_locDynFullMapTexel, pp->getDynamicFullMapTexelWorld());
                    if (s_locDynFullMapShadow >= 0) {
                        glUniform1i(s_locDynFullMapShadow, kChunkTexUnitDynFullMap);
                        glActiveTexture(GL_TEXTURE0 + kChunkTexUnitDynFullMap);
                        glBindTexture(GL_TEXTURE_2D, pp->getDynamicFullMapTexture());
                        glActiveTexture(GL_TEXTURE0);
                    }
                } else {
                    if (s_locDynLightSpaceMat >= 0)
                        glUniformMatrix4fv(s_locDynLightSpaceMat, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix());
                    if (s_locDynShadowMap >= 0) {
                        glUniform1i(s_locDynShadowMap, kChunkTexUnitDynamicShadow);
                        glActiveTexture(GL_TEXTURE0 + kChunkTexUnitDynamicShadow);
                        glBindTexture(GL_TEXTURE_2D, pp->getDynamicShadowTexture());
                        glActiveTexture(GL_TEXTURE0);
                    }
                }
            } else if (s_locEnableDynShadows >= 0) {
                glUniform1i(s_locEnableDynShadows, 0);
            }
        } else {
            if (s_locEnableShadows >= 0)    glUniform1i(s_locEnableShadows, 0);
            if (s_locEnableDynShadows >= 0) glUniform1i(s_locEnableDynShadows, 0);
        }
    }
}

// Bind the material normal array, the cement atlas, the transition mask and the control map.
static void bindAuxTextures() {
    // Phase 10 Step 5a: bind the merged material normal sampler2DArray (same
    // texture the legacy terrain uses) on its own unit. 0 until all 5 material
    // slots are populated -> the frag samples the default texture (flat-ish
    // normal -> falls back to the smooth base normal, no crash).
    if (s_locMatNormalArray >= 0) {
        GLuint matArrTex = (GLuint)gos_GetTerrainNormalArrayTex();
        glUniform1i(s_locMatNormalArray, kChunkTexUnitMatNormalArray);
        glActiveTexture(GL_TEXTURE0 + kChunkTexUnitMatNormalArray);
        glBindTexture(GL_TEXTURE_2D_ARRAY, matArrTex);
        glActiveTexture(GL_TEXTURE0);
    }

    // Step 5c: cement catalog atlas (tex3 / unit 3) + params — concrete tiles
    // sample this instead of the colormap (legacy gos_terrain.frag:414-426).
    {
        bool cementReady = gos_terrain_indirect_isCementAtlasReady();
        if (s_locUseCement >= 0)
            glUniform1i(s_locUseCement, (cementReady && s_cementSsbo.created()) ? 1 : 0);
        // CEMENT-DIAG-CONNECT-1: env gate MC2_TERRAIN_CEMENT_DIAG_CONNECT, default OFF
        // -> uploads 0 -> frag diagonal-fill block skipped (byte-identical).
        // REDUNDANT-PASS-HUNT-1: env resolved once (kTglcCementDiagConnect).
        if (s_locCementDiagConnect >= 0)
            glUniform1i(s_locCementDiagConnect, kTglcCementDiagConnect ? 1 : 0);
        // CEMENT-ATLAS-SEAM-1: half-texel sampling inset for the gutter-less
        // cement atlas (faint tile-grid seam lines across continuous pads).
        // Default ON; killswitch MC2_CEMENT_ATLAS_SEAM_FIX=0 reverts.
        if (s_locCementAtlasSeamFix >= 0)
            glUniform1i(s_locCementAtlasSeamFix, kTglcCementAtlasSeamFix ? 1 : 0);
        if (cementReady) {
            if (s_locCementGridSide >= 0)
                glUniform1i(s_locCementGridSide, gos_terrain_indirect_getCementAtlasGridSide());
            if (s_locCementWUPT >= 0)
                glUniform1f(s_locCementWUPT, 128.0f);  // Terrain::worldUnitsPerVertex
            if (s_locCementAtlas >= 0) {
                glUniform1i(s_locCementAtlas, kChunkTexUnitCement);
                glActiveTexture(GL_TEXTURE0 + kChunkTexUnitCement);
                glBindTexture(GL_TEXTURE_2D, (GLuint)gos_terrain_indirect_getCementAtlasGLTex());
                glActiveTexture(GL_TEXTURE0);
            }
        }
    }

    // Stage B: transition mask array at unit 11.
    {
        const bool tmReady = gos_terrain_indirect_isTransitionMaskReady();
        if (s_locUseTransitionMask >= 0)
            glUniform1i(s_locUseTransitionMask, tmReady ? 1 : 0);
        if (tmReady && s_locTransitionMaskArray >= 0) {
            glUniform1i(s_locTransitionMaskArray, kChunkTexUnitTransitionMask);
            glActiveTexture(GL_TEXTURE0 + kChunkTexUnitTransitionMask);
            glBindTexture(GL_TEXTURE_2D_ARRAY, gos_terrain_indirect_getTransitionMaskArrayGL());
            glActiveTexture(GL_TEXTURE0);
        }
    }

    // TERRAIN-CONTROLMAP-SAMPLE-1: authored control map at unit 12. Only bound
    // when a sidecar was actually loaded (s_controlMapTex != 0); u_useControlMap
    // uploads 0 otherwise (gate off / no sidecar) -> frag takes the verbatim
    // chunkColorWeights() else-branch -> byte-identical.
    {
        const bool controlMapReady = (s_controlMapTex != 0);
        if (s_locUseControlMap >= 0)
            glUniform1i(s_locUseControlMap, controlMapReady ? 1 : 0);
        if (controlMapReady && s_locControlMap >= 0) {
            glUniform1i(s_locControlMap, TERRAIN_CONTROLMAP_TEXUNIT);
            glActiveTexture(GL_TEXTURE0 + TERRAIN_CONTROLMAP_TEXUNIT);
            glBindTexture(GL_TEXTURE_2D, s_controlMapTex);
            glActiveTexture(GL_TEXTURE0);
        }
    }
}

// Bind the per-layer material albedo array and the overlay-v2 sidecar.
static void bindMaterialAndOverlay() {
    // TERRAIN-MATERIAL-TEXTURES-1: per-layer PBR albedo array at unit 4. Lazy
    // load at first gate-ON bind; u_useMatAlbedo uploads 0 when the gate is
    // OFF or the load failed -> frag takes the verbatim colormap-tint path
    // (byte-identical). Strength precedence: env > JSON (matAlbedoStrength,
    // via gos_SetTerrainMatAlbedoStrength) > 0.7 default.
    {
        GLuint albTex = 0;
        if (kTglcMatAlbedo) {
            tglc_EnsureMatAlbedoArrayLoaded();
            albTex = s_matAlbedoArrayTex;
        }
        const bool matAlbedoReady = (albTex != 0);
        if (s_locUseMatAlbedo >= 0)
            glUniform1i(s_locUseMatAlbedo, matAlbedoReady ? 1 : 0);
        if (matAlbedoReady) {
            if (s_locMatAlbedoArray >= 0) {
                glUniform1i(s_locMatAlbedoArray, kChunkTexUnitMatAlbedoArray);
                glActiveTexture(GL_TEXTURE0 + kChunkTexUnitMatAlbedoArray);
                glBindTexture(GL_TEXTURE_2D_ARRAY, albTex);
                glActiveTexture(GL_TEXTURE0);
            }
            if (s_locMatAlbedoStrength >= 0) {
                // env resolved once (mission-constant, REDUNDANT-PASS-HUNT-1
                // discipline); -1 sentinel = env absent -> JSON -> 0.7.
                static const float s_envMatAlbedoStrength = mc2::gate::get<"MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH">();
                const float strength =
                      (s_envMatAlbedoStrength >= 0.0f)  ? s_envMatAlbedoStrength
                    : (s_matAlbedoStrengthJson >= 0.0f) ? s_matAlbedoStrengthJson
                    : 0.7f;
                glUniform1f(s_locMatAlbedoStrength, strength);
            }
        }
    }

    // TERRAIN-OVERLAY-V2-PARITY-1: authored cement/pad/runway overlay sidecar
    // at unit TERRAIN_OVERLAY_SIDECAR_TEXUNIT. Only bound when a sidecar was
    // actually loaded (s_overlaySidecarTex != 0); u_useOverlaySidecar uploads
    // 0 otherwise (gate off / no sidecar) -> frag takes the verbatim legacy
    // cement-word composite else-branch -> byte-identical.
    {
        const bool overlaySidecarReady = (s_overlaySidecarTex != 0);
        if (s_locUseOverlaySidecar >= 0)
            glUniform1i(s_locUseOverlaySidecar, overlaySidecarReady ? 1 : 0);
        if (overlaySidecarReady) {
            if (s_locOverlaySidecar >= 0) {
                glUniform1i(s_locOverlaySidecar, TERRAIN_OVERLAY_SIDECAR_TEXUNIT);
                glActiveTexture(GL_TEXTURE0 + TERRAIN_OVERLAY_SIDECAR_TEXUNIT);
                glBindTexture(GL_TEXTURE_2D, s_overlaySidecarTex);
                glActiveTexture(GL_TEXTURE0);
            }
            if (s_locOverlayBounds >= 0)
                glUniform4f(s_locOverlayBounds, s_overlayBounds[0], s_overlayBounds[1],
                            s_overlayBounds[2], s_overlayBounds[3]);
        }
    }
}

// Bind the shoreline mask and set the wet/foam band uniforms.
TerrainShorelineKnobs gos_TerrainShorelineKnobs() {
    TerrainShorelineKnobs k;
    k.on = gos_TerrainShorelineGateOn();
    // The water plane the bands are placed against: the same source as the water fast path (Terrain::waterElevation,
    // mirrored into gosPostProcess at mission load).
    if (gosPostProcess* pp = getGosPostProcess()) k.waterElevation = pp->getWaterElevation();
    k.timeSeconds  = gos_GetShaderClockSeconds();
    k.strength     = mc2::gate::get<"MC2_TERRAIN_SHORELINE_STRENGTH">();
    k.foamStrength = mc2::gate::get<"MC2_TERRAIN_SHORELINE_FOAM">();
    // Band widths are HORIZONTAL runs from the waterline (wet 16 wu, foam 5 wu by default); the legacy _HEIGHT names
    // are aliases read only while the _RUN name is unset.
    k.wetRun  = mc2::gate::isSet<"MC2_TERRAIN_SHORELINE_WET_RUN">()  ? mc2::gate::get<"MC2_TERRAIN_SHORELINE_WET_RUN">()
                                                                     : mc2::gate::get<"MC2_TERRAIN_SHORELINE_WET_HEIGHT">();
    k.foamRun = mc2::gate::isSet<"MC2_TERRAIN_SHORELINE_FOAM_RUN">() ? mc2::gate::get<"MC2_TERRAIN_SHORELINE_FOAM_RUN">()
                                                                     : mc2::gate::get<"MC2_TERRAIN_SHORELINE_FOAM_HEIGHT">();
    // Static world-XY jitter on the run so the lobes stop tracing the mesh waterline's straight segments; 0 = exact contour.
    k.edgeJitter = mc2::gate::get<"MC2_TERRAIN_SHORELINE_EDGE_JITTER">();
    return k;
}

static void bindShorelineMask() {
    // TERRAIN-SHORELINE-V3: elevation-placed wet/foam band. u_useShorelineMask
    // now gates the BAND ITSELF (MC2_TERRAIN_SHORELINE on), independent of
    // whether a mask sidecar was found -- v1/v2 required a loaded mask before
    // any band showed; V3's placement comes from v_worldPos.z vs
    // u_waterElevation, so the band works unconditionally once the gate is
    // on. The mask, when present (s_shorelineMaskTex != 0), is uploaded too
    // and applied as an OPTIONAL modulator (u_hasShorelineMask) -- it no
    // longer decides whether the band exists, only how it's shaped. Gate OFF
    // -> u_useShorelineMask uploads 0 -> frag skips the whole block -> byte-
    // identical (no band; legacy screen runShoreline() stays active per its
    // own gate).
    {
        // SHORELINE-SMOOTHING-REVISIT-1: default flipped ON. The elevation-
        // placed V3 band (fbm edge jitter, optional mask modulator) is what
        // draws the shoreline now. SHORELINE-GLOW-1: resolved through the one
        // shared TU accessor, so this read and the suppression accessor below
        // can no longer drift apart.
        const bool s_shorelineGateOn = gos_TerrainShorelineGateOn();
        const bool shorelineMaskReady = (s_shorelineMaskTex != 0);
        if (s_locUseShorelineMask >= 0)
            glUniform1i(s_locUseShorelineMask, s_shorelineGateOn ? 1 : 0);
        if (s_shorelineGateOn) {
            if (s_locHasShorelineMask >= 0)
                glUniform1i(s_locHasShorelineMask, shorelineMaskReady ? 1 : 0);
            if (shorelineMaskReady) {
                if (s_locShorelineMask >= 0) {
                    glUniform1i(s_locShorelineMask, TERRAIN_SHORELINE_TEXUNIT);
                    glActiveTexture(GL_TEXTURE0 + TERRAIN_SHORELINE_TEXUNIT);
                    glBindTexture(GL_TEXTURE_2D, s_shorelineMaskTex);
                    glActiveTexture(GL_TEXTURE0);
                }
                if (s_locShorelineBounds >= 0)
                    glUniform4f(s_locShorelineBounds, s_shorelineBounds[0], s_shorelineBounds[1],
                                s_shorelineBounds[2], s_shorelineBounds[3]);
            }
            const TerrainShorelineKnobs k = gos_TerrainShorelineKnobs();
            if (s_locWaterElevation >= 0)        glUniform1f(s_locWaterElevation, k.waterElevation);
            if (s_locShaderTime >= 0)            glUniform1f(s_locShaderTime, k.timeSeconds);
            if (s_locShorelineStrength >= 0)     glUniform1f(s_locShorelineStrength, k.strength);
            if (s_locShorelineFoamStrength >= 0) glUniform1f(s_locShorelineFoamStrength, k.foamStrength);
            if (s_locShorelineWetHeight >= 0)    glUniform1f(s_locShorelineWetHeight, k.wetRun);
            if (s_locShorelineFoamHeight >= 0)   glUniform1f(s_locShorelineFoamHeight, k.foamRun);
            if (s_locShorelineEdgeJitter >= 0)   glUniform1f(s_locShorelineEdgeJitter, k.edgeJitter);
        }
    }
}

// Upload the material tiling, tint, lighting and shadow tuning uniforms.
static void uploadTerrainTuning() {
    {
        float mt[5] = {3,2,1,6,1};  gos_GetTerrainMatTiling(&mt[0], &mt[1], &mt[2], &mt[3], &mt[4]);
        float nb[4] = {0.9f,1.1f,1.1f,2.5f}; gos_GetTerrainMatNormalBoost(&nb[0], &nb[1], &nb[2], &nb[3]);
        // The concrete detail normal is permanently off (user ruling 2026-07-26): nb[3]
        // is matNormalBoost.w, the only scale on the concrete branch in
        // terrain_lod_chunk.frag, so zeroing it makes that branch contribute exactly
        // vec3(0) without touching the other materials or the cement albedo path.
        nb[3] = 0.0f;
        float cg[4] = {-0.02f,0.06f,0.22f,0.40f}; gos_GetTerrainClassGrass(&cg[0], &cg[1], &cg[2], &cg[3]);
        float cd[4] = {-0.02f,0.06f,0.22f,0.45f}; gos_GetTerrainClassDirt(&cd[0], &cd[1], &cd[2], &cd[3]);
        float dt = gos_GetTerrainDetailTiling();
        float ds = gos_GetTerrainDetailStrength();
        if (s_locMatTiling      >= 0) glUniform4f(s_locMatTiling,      mt[0], mt[1], mt[2], mt[3]);
        if (s_locMatTilingSnow  >= 0) glUniform1f(s_locMatTilingSnow,  mt[4]);
        if (s_locMatNormalBoost >= 0) glUniform4f(s_locMatNormalBoost, nb[0], nb[1], nb[2], nb[3]);
        if (s_locClassGrass     >= 0) glUniform4f(s_locClassGrass,     cg[0], cg[1], cg[2], cg[3]);
        if (s_locClassDirt      >= 0) glUniform4f(s_locClassDirt,      cd[0], cd[1], cd[2], cd[3]);
        if (s_locDetailTiling   >= 0) glUniform4f(s_locDetailTiling,   dt, 0.0f, 0.0f, 0.0f);
        const float* _datYZW = mc2_chunkDetailAntiTileYZW();  // TERRAIN-DETAIL-ANTI-TILE-1 (yzw=0 when gate OFF)
        if (s_locDetailStrength >= 0) glUniform4f(s_locDetailStrength, ds, _datYZW[0], _datYZW[1], _datYZW[2]);

        float tr[3]={0.36f,0.37f,0.40f}; gos_GetTerrainTintRock(&tr[0],&tr[1],&tr[2]);
        float tg[3]={0.35f,0.42f,0.25f}; gos_GetTerrainTintGrass(&tg[0],&tg[1],&tg[2]);
        float td[3]={0.48f,0.42f,0.33f}; gos_GetTerrainTintDirt(&td[0],&td[1],&td[2]);
        float tss = gos_GetTerrainTintStrengthScale();
        if (s_locTintRock          >= 0) glUniform3f(s_locTintRock,  tr[0], tr[1], tr[2]);
        if (s_locTintGrass         >= 0) glUniform3f(s_locTintGrass, tg[0], tg[1], tg[2]);
        if (s_locTintDirt          >= 0) glUniform3f(s_locTintDirt,  td[0], td[1], td[2]);
        if (s_locTintStrengthScale >= 0) glUniform1f(s_locTintStrengthScale, tss);
        // Snow brightness dampen: <1 darkens detected snow. Default 0.78 (visibly
        // turned down); MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN overrides.
        if (s_locSnowBrightnessDampen >= 0) glUniform1f(s_locSnowBrightnessDampen, gos_TerrainSnowBrightnessDampen());
        // TERRAIN-CONTROLMAP-ALBEDO-1: default member 0.0f -> uploaded verbatim
        // -> frag's mix(x,1.0,0.0)==x (byte-identical) when gate is OFF.
        if (s_locControlAlbedoStrength >= 0)
            glUniform1f(s_locControlAlbedoStrength, gos_GetTerrainControlAlbedoStrength());

        // TERRAIN-MATERIAL-LIB-1: promoted tints always upload (no gate -- they
        // replace former frag literals; default member values are the exact
        // former literals, so this is byte-identical when no JSON was loaded).
        // Roughness/AO + the branch flag ARE gated: u_useMaterialLib defaults to
        // 0 so the frag's roughness/AO branch is never taken unless the env gate
        // is on, matching the legacy patch-stream path's byte-identity contract.
        {
            float tc[3]={0.55f,0.53f,0.50f}; gos_GetTerrainTintConcrete(&tc[0],&tc[1],&tc[2]);
            float tsn[3]={0.75f,0.78f,0.84f}; gos_GetTerrainTintSnow(&tsn[0],&tsn[1],&tsn[2]);
            float mr[4]={1,1,1,1}; gos_GetTerrainMatRoughness(&mr[0],&mr[1],&mr[2],&mr[3]);
            float ma[4]={1,1,1,1}; gos_GetTerrainMatAO(&ma[0],&ma[1],&ma[2],&ma[3]);
            float mh[4]={1,1,1,1}; gos_GetTerrainMatHeight(&mh[0],&mh[1],&mh[2],&mh[3]);  // POM-PERLAYER-HEIGHT-1
            if (s_locTintConcrete >= 0) glUniform3f(s_locTintConcrete, tc[0], tc[1], tc[2]);
            if (s_locTintSnow     >= 0) glUniform3f(s_locTintSnow,     tsn[0], tsn[1], tsn[2]);
            if (s_locMatRoughness >= 0) glUniform4f(s_locMatRoughness, mr[0], mr[1], mr[2], mr[3]);
            if (s_locMatAO        >= 0) glUniform4f(s_locMatAO,        ma[0], ma[1], ma[2], ma[3]);
            if (s_locMatHeight    >= 0) glUniform4f(s_locMatHeight,    mh[0], mh[1], mh[2], mh[3]);  // POM-PERLAYER-HEIGHT-1
            const bool matLibOn = gos_TerrainMaterialLibEnabled();
            if (s_locUseMaterialLib >= 0) glUniform1i(s_locUseMaterialLib, matLibOn ? 1 : 0);
        }

        // Remaining tunables. NFH strength scales the chunk's always-on smooth normal
        // (default 1.0 = no change). POM = legacy scale (default 0.02). Material
        // profile = global int (0=legacy).
        // CLIFF SHADOW FLOOR: env-gated. Unset/=0 uploads 0.0 (shader no-op,
        // byte-identical). Non-zero uploads the member value (ImGui-tunable), 0..1.
        {
            static const bool s_cliffOn = mc2::gate::get<"MC2_TERRAIN_CLIFF_SHADOW_FLOOR">();
            if (s_locCliffShadowFloor >= 0) {
                float cf = s_cliffOn ? gos_GetTerrainCliffShadowFloor() : 0.0f;
                if (cf < 0.0f) cf = 0.0f;
                if (cf > 1.0f) cf = 1.0f;
                glUniform1f(s_locCliffShadowFloor, cf);
            }
        }
        // ── TERRAIN-AMBIENT-FLOOR-2 (MC2_TERRAIN_AMBIENT_FLOOR) — DEFAULT-ON at 0.35 ──
        //
        // Terrain receives no other ambient term, so it is
        // exactly `baseColor * normalLight * shadow`, while static props standing
        // on it get ambient through SH-IBL. That is what bottoms out daylight
        // ground on mc2_03 beside the (correctly, user-ratified) flat-lit pads.
        // The frag adds `baseColor * ambCol * floor * ...`, albedo-scaled so dark
        // materials stay proportionally dark, masked by the flat-lit share so
        // concrete AND paved-overlay pavement are bit-identical.
        //
        // FULL REVERT IS THIS ONE GATE: MC2_TERRAIN_AMBIENT_FLOOR=0 uploads 0.0,
        // the frag block is skipped, output is byte-identical. There is no second
        // gate to remember -- the predecessor slice shipped two (a diffuse wrap
        // as well) and documented a one-gate revert that left half the change
        // running. Bounded 0..2; junk/NaN clamps to 0. Resolved once at start.
        {
            // ONE getenv. This used to carry a second, bare
            // `static const char* s_ambEnv = getenv("MC2_TERRAIN_AMBIENT_FLOOR");`
            // purely so the trace could echo the raw string, and that bare
            // capture -- a getenv with no idiom attached -- was the sole reason
            // check-gate-default-agreement.py reported this DEFAULT-ON gate as
            // UNCLASSIFIABLE (also called out in scripts/check-delivery.py's
            // notes as "ships default 0.35 and classified UNKNOWN"). The raw
            // string is now reported as a set/unset flag recorded by the
            // resolving lambda, so the single remaining reader is the ordinary
            // numeric-tunable form and the checker stops flagging it.
            // Removed 2026-07-30 (LIGHTING-INCAPABLE-BATCH-3). Behaviour is
            // unchanged: s_ambEnv was never read for anything but the trace line.
            // VK-PARITY: the env read + clamp now lives in gos_TerrainAmbientFloor()
            // (below, same file) so the VK terrain island shares ONE producer.
            const float s_amb = gos_TerrainAmbientFloor();
            const bool  s_ambEnvSet = gos_TerrainAmbientFloorEnvSet();
            if (s_locAmbientFloor >= 0) glUniform1f(s_locAmbientFloor, s_amb);
            // ── TERRAIN-AMBIENT-FLOOR-NIGHT-1 (MC2_TERRAIN_AMBIENT_FLOOR_NIGHT,
            //    DEFAULT 0.0 = OFF = byte-identical) ────────────────────────────
            //
            // WHY THIS EXISTS. TERRAIN-AMBIENT-FLOOR-2 shipped DEFAULT-ON to cure
            // "terrain is too dark", and its daylight gate is EXACTLY 0.0000 on
            // mc2_05, mc2_10, mc2_19, mc2_21 and mc2_23 -- which are precisely
            // the five lowest-multiplier (0.3670-0.4814), highest-floor-pin
            // (14.3-100% of fragments at diffuse == 0.02) maps in the game. The
            // shipped cure for dark terrain is switched off on exactly the five
            // maps where terrain is darkest, and those frames are byte-identical
            // to before it landed. Verified here at source, not inherited:
            // terrain_daylight.h uses kNightFullDeg = 10, and the authored
            // LightDirPitch values in mc2-srcdata/missions are 270 / 180 / 0 /
            // 270 / 9, folding to altitudes -1 / 0 / 0 / -1 / 9, all <= 10.
            //
            // WHAT THIS IS. A FLOOR under the daylight gate, not a second gate
            // and not a night-only branch: the uploaded value becomes
            // max(dayGate, floor). Consequences that made this the chosen shape:
            //   * default 0.0 -> max(dayGate, 0) == dayGate -> byte-identical on
            //     every mission, which is the whole revert;
            //   * monotone, so there is no discontinuity at the dusk band edge
            //     (a mission at gate 0.2 with floor 0.3 lands on 0.3, not on a
            //     step from 0.001 to 0.3);
            //   * it composes with MC2_TERRAIN_AMBIENT_FLOOR rather than
            //     duplicating it -- the strength stays one number, this is the
            //     fraction of it night maps are allowed.
            //
            // WHY DEFAULT OFF, AND IT MUST STAY OFF UNTIL THE USER GRADES IT.
            // The predecessor slice (TERRAIN-AMBIENT-FLOOR-1) was rejected at
            // merge review for lifting mc2_10's ground +59.1% through a fade that
            // could not fade out, and the user has explicitly PRAISED how mc2_05
            // currently looks. A night lift on these five maps is a taste call
            // this lane is not entitled to make, and it is not a regression fix.
            //
            // WHERE AN A/B ACTUALLY SHOWS SOMETHING (saturation checked first, so
            // this is not a recipe pointing at a mission where nothing can move):
            // the ambient term is ADDITIVE, so the normalLight floor pin does not
            // block it, and terrain's measured headroom to clip on these maps is
            // 9.07x (mc2_10) to 20.83x (mc2_21) -- nothing saturates. Grade
            // mc2_05 and mc2_10 from a ground-level pose that has both bare
            // terrain and a concrete pad in frame, 0 vs 0.30. EXPECT: bare
            // terrain up, pads 0.00% (flatLitShare exempts them), i.e. the
            // pad:ground ratio closes from the terrain side only.
            // ONE getenv, deliberately. The sibling gate above uses a second,
            // bare `static const char* = getenv(...)` purely so its trace can
            // echo the raw string, and that bare capture is exactly why
            // check-gate-default-agreement.py reports MC2_TERRAIN_AMBIENT_FLOOR
            // as UNCLASSIFIABLE: a getenv with no idiom on it is an unreadable
            // reader. Here the raw-string need is met by a set/unset flag the
            // resolving lambda records, so this gate presents a single reader
            // whose unset branch (`if (!e || !e[0]) return 0.0f;`) is the
            // canonical explicit DEFAULT-OFF form.
            const float s_nightGateFloor = gos_TerrainAmbientNightFloor();   // VK-PARITY shared producer
            const bool  s_nightEnvSet    = gos_TerrainAmbientNightFloorEnvSet();
            // Daylight level, computed by terrain_daylight.h from the AUTHORED sun
            // pitch min the engine night state and pushed per frame by gamecam.cpp.
            // 0 on every night mission in the shipping data -> the block is skipped
            // there -> night is byte-identical (unless the floor above arms it).
            const float rawDayGate = gos_GetTerrainDaylightLevel();
            const float dayGate    = (rawDayGate > s_nightGateFloor) ? rawDayGate
                                                                    : s_nightGateFloor;
            if (s_locAmbientDayGate >= 0) glUniform1f(s_locAmbientDayGate, dayGate);
            // MC2_TERRAIN_AMBIENT_FLOOR_TRACE: one line, first upload. Prints the
            // resolved floor, the day gate actually uploaded, and both raw inputs,
            // so "did the gate engage on this mission and why" is answerable from
            // a log instead of from a capture diff. raw_dayGate is printed
            // alongside the uploaded gate so the night floor's effect is visible
            // rather than inferred.
            static bool s_traced = false;
            if (!s_traced && mc2::gate::get<"MC2_TERRAIN_AMBIENT_FLOOR_TRACE">()) {
                s_traced = true;
                bool  isNight = false; float nf = 0.0f;
                gos_GetNightState(&isNight, &nf);
                float lx = 0.0f, ly = 0.0f, lz = 0.0f;
                gos_GetTerrainLightDir(&lx, &ly, &lz);
                mc2::fmt::print_err("[TERRAIN_AMBIENT_FLOOR v2] floor={:.3f} dayGate={:.4f} raw_dayGate={:.4f} night_floor={:.4f} night_env_set={:d} floor_env_set={:d} runtime_isNight={:d} runtime_nightFactor={:.3f} runtime_lightDirZ={:.4f}\n", s_amb, dayGate, rawDayGate, s_nightGateFloor, mc2::fmt::as_i32(s_nightEnvSet ? 1 : 0), mc2::fmt::as_i32(s_ambEnvSet ? 1 : 0), mc2::fmt::as_i32(isNight ? 1 : 0), nf, lz);
                fflush(stderr);
            }
        }
        if (s_locNfhStrength >= 0) glUniform1f(s_locNfhStrength, gos_GetTerrainNormalsFromHeightStrength());
        // TERRAIN-SLOPE-BIAS-VISUAL-1 (B4a): env gate MC2_TERRAIN_SLOPE_BIAS, default
        // OFF. When unset/0 the gate uploads 0 and the frag block is a no-op
        // (byte-identical). Strength via MC2_TERRAIN_SLOPE_BIAS_STRENGTH (default 1.0).
        // REDUNDANT-PASS-HUNT-1: env resolved once (kTglcSlopeBias/Str), not per frame.
        {
            if (s_locUseRockSlopeBias >= 0) glUniform1i(s_locUseRockSlopeBias, kTglcSlopeBias ? 1 : 0);
            if (s_locRockSlopeBiasStr >= 0) glUniform1f(s_locRockSlopeBiasStr, kTglcSlopeBiasStr);
        }
        // TERRAIN-MACRO-VARIATION-1: env gate MC2_TERRAIN_MACRO_VARIATION, default
        // OFF -> uploads 0 -> frag block skipped (byte-identical). Strength via
        // MC2_TERRAIN_MACRO_VARIATION_STRENGTH (default 1.0).
        // REDUNDANT-PASS-HUNT-1: env resolved once (kTglcMacroVariation).
        if (s_locMacroVariation >= 0)
            glUniform1f(s_locMacroVariation, kTglcMacroVariation);
        // TERRAIN-EDGE-FEATHER-1: env gate MC2_TERRAIN_EDGE_FEATHER, default ON
        // (kill-switch; reader gos_terrain_lod_chunk.cpp:158 is `!(e && e[0]=='0')`.
        // Said "default OFF" until ENV-DROP-CLOSE-1, 2026-07-30).
        // =0 uploads 0 -> frag block skipped (byte-identical). Fades the terrain
        // colormap to sky/haze over the last ~tile band so the hard straight
        // map-perimeter line dissolves into the fog. Strength via
        // MC2_TERRAIN_EDGE_FEATHER_STRENGTH (default 1.0).
        // REDUNDANT-PASS-HUNT-1: env resolved once (kTglcEdgeFeather/Str).
        {
            if (s_locEdgeFeather >= 0) glUniform1i(s_locEdgeFeather, kTglcEdgeFeather ? 1 : 0);
            if (s_locEdgeFeatherStr >= 0) glUniform1f(s_locEdgeFeatherStr, kTglcEdgeFeatherStr);
        }
        // Chunk POM: the march uses the real per-fragment view vector; its layer counts and
        // world-distance fade band are constants in terrain_lod_chunk.frag.
        {
            if (s_locPomScale >= 0)
                glUniform1f(s_locPomScale, gos_GetTerrainPOMScale());
            // cameraPos is read by the POM view vector and the 8192 oracle viz.
            if (s_locCameraPos >= 0) {
                float cx = 0.0f, cy = 0.0f, cz = 0.0f;
                gos_GetTerrainCameraPos(&cx, &cy, &cz);
                glUniform4f(s_locCameraPos, cx, cy, cz, 1.0f);
            }
        }
        if (s_locMatProfile  >= 0) glUniform1i(s_locMatProfile,  g_terrainMaterialProfile);
    }
}

// Log the first non-empty submit and reset the zero-submit streak.
static void noteFirstSubmit(int count) {
    {
        static bool s_firstSubmit = true;
        s_submitZeroStreak = 0;  // reset on any non-zero submit
        if (s_firstSubmit && count > 0) {
            mc2::fmt::print_out("[TerrainLOD v1] FIRST SUBMIT: {:d} draw commands queued\n", mc2::fmt::as_i32(count));
            fflush(stdout);
            s_firstSubmit = false;
        }
    }
}

// Count the submit, reset the displaced-chunk counters and prepare the VK terrain island.
void TerrainLodSubmitFrame::countSubmitAndPrepVk(int count, const float* mvp, int& s_submitCount) {
    ++s_submitCount;
    if (s_submitCount % 600 == 0) {
        mc2::fmt::print_out("[TerrainLOD submit] count={:d} cmds={:d}\n", mc2::fmt::as_i32(s_submitCount), mc2::fmt::as_i32(count));
        fflush(stdout);
    }

    for (int lvl = 0; lvl < 6; ++lvl) s_visualDisplacedCounts[lvl] = 0;

#ifdef MC2_VULKAN_ISLAND
    // VK-P10 TERRAIN TIER-1: when armed, the VK terrain island co-writes the LOD0
    // (lodStep==1) MAIN patches into the shared GBuffer. Capture them here and SKIP
    // their GL main+skirt draws (so the A/B reference and the VK path both exclude
    // LOD0 skirts — the probe compares LOD0 main-triangle pixels like-for-like).
    // Default-OFF -> s_vkTerrainEnabled=false -> the loop below is byte-identical.
    //
    // FAIL-SOFT (DEFAULT-ON safety): the SKIP is gated on island-READY, not merely
    // enabled. If the island can't co-write this frame (SPV/pipeline/init/device
    // failure, or a latched transient — mc2_vk_terrain_island_ready()==0), GL keeps
    // drawing LOD0 so there is NO terrain hole. run() is still called below whenever
    // enabled (even when we did NOT skip) so the island can BIRTH/heal — the first
    // successful run flips ready for the next frame (one-frame GL-draw then VK takeover,
    // never a hole).
    s_vkTerrainEnabled = (mc2_vk_terrain_island_enabled() != 0);
    // PREPARE the island NOW (GL context current) so the skip decision below can gate on
    // real readiness — this births the island independent of the feed, breaking the
    // skip<->ready<->feed deadlock. On any failure prepare()==0 -> GL draws LOD0.
    bool s_vkTerrainReady = false;
    if (s_vkTerrainEnabled) {
        if (gosPostProcess* ppPrep = getGosPostProcess()) {
            s_vkTerrainReady =
                (mc2_vk_terrain_island_prepare(ppPrep->getWidth(), ppPrep->getHeight()) != 0);
        }
    }
    s_vkTerrainActive = s_vkTerrainEnabled && s_vkTerrainReady;
    static bool s_vkTerrainFailSoftLogged = false;
    if (s_vkTerrainEnabled && !s_vkTerrainActive && !s_vkTerrainFailSoftLogged) {
        mc2::fmt::print_err("[VK_TERRAIN_ISLAND] FAIL-SOFT: gate ON but island not ready this frame -> GL draws LOD0 (no hole). Will retry birth/heal via run().\n");
        fflush(stderr);
        s_vkTerrainFailSoftLogged = true;
    }
    s_vkTerrainPatches.clear();
    s_vkTerrainFeedValid = false;
    if (s_vkTerrainActive) {
        std::memcpy(s_vkTerrainMvp, mvp, sizeof(s_vkTerrainMvp)); // same MVP the GL draw uses
    }
#endif
}

// Issue the draw for every queued command, then report the per-band displaced-chunk counts.
void TerrainLodSubmitFrame::drawChunks(int count, const TerrainDrawCommand* cmds, const float* skirtDepths, const unsigned char* skirtEdgeMasks, const unsigned int* edgeStitch, const int* shadowTiers, const float* morphFactors) {
    for (int i = 0; i < count; ++i)
    {
        const TerrainDrawCommand& cmd = cmds[i];
        int qcX = cmd.quadCountsPacked & 0xFF;
        int qcY = (cmd.quadCountsPacked >> 8) & 0xFF;

        if (qcX <= 0 || qcY <= 0) continue;

#ifdef MC2_VULKAN_ISLAND
        // VK-P10 TERRAIN TIER-1 (+ rung-3 VK-TERRAIN-ALL-LODS): hand main patches to
        // the VK island and skip their GL main+skirt draws. LOD0 (lodStep==1) is the
        // original probe scope; MC2_VK_TERRAIN_ALL_LODS extends capture to coarse bands
        // (lodStep>1) too -- BANDS ONLY, still excluding mode-1 visual-displace (LOD0
        // 4x-finer topology, a separate L slice). Edge-stitch is NO LONGER excluded
        // (VK-TERRAIN-EDGE-STITCH-1 / commit 0aae0337 removed the edgeStitch[i]==0
        // predicate term; stitched patches are VK-captured, corner-pin math ported into
        // the tier1 verts). Default-OFF (MC2_VK_TERRAIN_ALL_LODS unset)
        // -> capture predicate is IDENTICAL to before (lodStep==1 only).
        const bool s_vkAllLodsActive = s_vkTerrainActive &&
            (mc2_vk_terrain_all_lods_enabled() != 0);
        const bool s_vkCaptureThisBand = (cmd.lodStep == 1) || s_vkAllLodsActive;
        // VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): relax the visualDisplaceActive
        // exclusion for mode-2 patches (lodStep>1, pure Z-swap, same vertex density)
        // when the sub-gate is armed. Mode-1 patches (lodStep==1, 4x-finer topology +
        // corner-pinned edges) stay GL-owned (separate L slice later). Stitched patches
        // are VK-captured (edge-stitch exclusion removed, see above). Gate OFF ->
        // predicate IDENTICAL to rung 4.
        const bool s_vkVisDispFarActive = s_vkAllLodsActive &&
            (mc2_vk_terrain_visualdisplace_far_enabled() != 0);
        const bool s_vkOwnsDisplace = !visualDisplaceActive ||
            (s_vkVisDispFarActive && cmd.lodStep != 1);
        const bool s_vkNoEdgeStitch = (edgeStitch == nullptr || edgeStitch[i] == 0);
        if (s_vkTerrainActive && s_vkCaptureThisBand &&
            s_vkOwnsDisplace) {
            const int maxOffX = (cmd.lodStep > 0) ? (qcX / cmd.lodStep) * cmd.lodStep : qcX;
            const int maxOffY = (cmd.lodStep > 0) ? (qcY / cmd.lodStep) * cmd.lodStep : qcY;
            Mc2TerrainTier1Patch p;
            p.blockOriginX = cmd.blockOriginX;
            p.blockOriginY = cmd.blockOriginY;
            p.quadCountX   = maxOffX;
            p.quadCountY   = maxOffY;
            p.lodStep      = cmd.lodStep;
            // VK-TERRAIN-SKIRT (rung 4): snapshot this block's skirt inputs into the
            // SAME record so the VK island can draw the skirt in the same co-write pass
            // as the main patch, with no extra GL state read on the VK side. Only
            // populated when the skirt sub-gate is armed; else 0/0 (island draws no
            // skirt for this patch, matching today's GL-owns-all-skirts fallback --
            // the GL skip below is ALSO gated on the same predicate so GL keeps drawing
            // the skirt whenever the VK island isn't going to).
            const bool s_vkSkirtsActive = mc2_vk_terrain_skirts_enabled() != 0;
            p.skirtDepth    = (s_vkSkirtsActive && skirtDepths)    ? skirtDepths[i]    : 0.0f;
            p.skirtEdgeMask = (s_vkSkirtsActive && skirtEdgeMasks) ? skirtEdgeMasks[i] : 0u;
            // VK-TERRAIN-VISUALDISPLACE-FAR-1: mirror the GL displaceFar decision below
            // (visualDisplaceActive && lodStep != 1 -> mode 2). Mode-1 patches never get
            // here (s_vkOwnsDisplace excluded them), so this is only ever 0 or 2.
            p.visualMode    = (visualDisplaceActive && cmd.lodStep != 1) ? 2 : 0;
            p.edgeStitch    = edgeStitch ? edgeStitch[i] : 0u;
            s_vkTerrainPatches.push_back(p);
            s_vkTerrainFeedValid = true;
            continue;   // GL skips this patch's main + skirt draws unconditionally (this
                        // was already true pre-rung-4: VK-owned patches never got a GL
                        // skirt draw, since `continue` fires before the skirt-draw code
                        // below runs). Pre-rung-4 that meant VK-owned patches had NO
                        // skirt at all -- a latent crack risk masked only by rung-3's
                        // narrow scope (bands still matched GL neighbor geometry closely
                        // enough per the ladder doc). This rung closes that gap: when the
                        // skirts sub-gate is armed, skirtDepth/skirtEdgeMask above are
                        // populated and the VK island draws the skirt itself; when the
                        // sub-gate is off, behavior is UNCHANGED from rung-3 (0/0 ->
                        // island draws no skirt for this patch).
        }
#endif

        // TERRAIN-VISUAL-HEIGHT-SAMPLE-1 (LOD0) + TERRAIN-VISUAL-HEIGHT-S2-ALLLOD
        // (LOD1+): displace every band. LOD0 uses the 4x-finer builder (mode 1,
        // unchanged from S1: fine grid + skirts, corner-pinned edges). Coarser
        // bands (lodStep>1) reuse the EXISTING coarse-density patch unmodified
        // (mode 2: no new vertices — the vert shader Z-swaps each existing sample
        // point to its coincident bake index, sec vert comment for stitch proof).
        const bool displaceLod0  = visualDisplaceActive && cmd.lodStep == 1;
        const bool displaceFar   = visualDisplaceActive && cmd.lodStep != 1;
        const int  visualMode    = displaceLod0 ? 1 : (displaceFar ? 2 : 0);
        // Checkerboard diagonal applies ONLY to lodStep==1 fine cells on the
        // normal (non-4x-displace) path — coarse LOD (lodStep>1) per-cell parity
        // is ill-defined and the 4x heightsFine path is a separate follow-up;
        // both keep the existing fixed TL-BR diagonal. Mode 2 does NOT change the
        // patch builder at all (same qcX/qcY/lodStep/checkerDiag as the
        // non-displaced path), so this is unaffected by S2.
        const bool checkerDiag = terrainLodCheckerDiagEnabled()
                                 && !displaceLod0 && cmd.lodStep == 1;
        const PatchShape& patch = displaceLod0
            ? getOrBuildPatch(qcX * 4, qcY * 4, 1, /*checkerDiag*/false, 0, 0)
            : getOrBuildPatch(qcX, qcY, cmd.lodStep, checkerDiag,
                              cmd.blockOriginX & 1, cmd.blockOriginY & 1);
        if (s_locVisualDisplace >= 0)
            glUniform1i(s_locVisualDisplace, visualMode);

        // TERRAIN-VISUAL-HEIGHT-S2-ALLLOD: per-LOD-band displaced-chunk counters,
        // logged alongside the existing [TerrainLOD v1] telemetry so the crack /
        // silhouette acceptance pass has hard numbers per band.
        if (visualDisplaceActive) {
            int lvl = (cmd.lodStep == 1) ? 0 : (cmd.lodStep == 2) ? 1
                    : (cmd.lodStep == 4) ? 2 : (cmd.lodStep == 5) ? 3
                    : (cmd.lodStep == 10) ? 4 : 5;
            ++s_visualDisplacedCounts[lvl];
        }

        if (s_locBlockOriginX >= 0)
            glUniform1i(s_locBlockOriginX, cmd.blockOriginX);
        if (s_locBlockOriginY >= 0)
            glUniform1i(s_locBlockOriginY, cmd.blockOriginY);
        if (s_locLodStep >= 0)
            glUniform1i(s_locLodStep, cmd.lodStep);  // Phase 5: LOD band for debug vis

        // Phase 10.4: edge stitching. Block quad extent (for edge detection) +
        // packed coarser-neighbour stride per edge. Skirt verts (isSkirtFlag!=0)
        // skip the snap in the vert, so this is safe to set once per block.
        // u_quadCount* must be the MAX localOffset the patch actually emits, which
        // makeSamplePositions caps at the last multiple of lodStep <= quad count.
        // (For partial map-edge blocks qcX may not be a multiple of lodStep.)
        const int maxOffX = (cmd.lodStep > 0) ? (qcX / cmd.lodStep) * cmd.lodStep : qcX;
        const int maxOffY = (cmd.lodStep > 0) ? (qcY / cmd.lodStep) * cmd.lodStep : qcY;
        if (s_locQuadCountX >= 0) glUniform1i(s_locQuadCountX, maxOffX);
        if (s_locQuadCountY >= 0) glUniform1i(s_locQuadCountY, maxOffY);
        if (s_locEdgeStitch >= 0)
            glUniform1i(s_locEdgeStitch, edgeStitch ? (GLint)edgeStitch[i] : 0);
        if (s_locShadowTier >= 0)  // Slice B: per-chunk shadow tier (DIAG=40 tint only)
            glUniform1i(s_locShadowTier, shadowTiers ? (GLint)shadowTiers[i] : 0);
        // TERRAIN-LOD-GEOMORPH-1: per-block geomorph factor. Uploaded 0 whenever
        // the geomorph is inactive so a stale value can never leak into a draw.
        if (s_locMorphFactor >= 0)
            glUniform1f(s_locMorphFactor,
                        (geomorphActive && morphFactors) ? morphFactors[i] : 0.0f);

        // --- Draw main patch (skirtDepth=0 so isSkirtFlag pulls height by 0) ---
        if (s_locSkirtDepth >= 0)
            glUniform1f(s_locSkirtDepth, 0.0f);

        // Attrib 0: ivec2 localOffset. Attrib 1 (isSkirt) left disabled -> reads as 0.
        glDisableVertexAttribArray(1);
        patch.vbo.bind();
        glEnableVertexAttribArray(0);
        glVertexAttribIPointer(0, 2, GL_SHORT, (GLsizei)(2 * sizeof(int16_t)), (const void*)0);

        patch.ibo.bind();

        // SLICE 3a: near-field (LOD0 == lodStep 1) chunks draw as GL_PATCHES
        // through the tess variant program when the gate is ON and the program
        // built. Pass-through TES => visually identical to the base draw. All
        // other bands, and the skirts below, stay GL_TRIANGLES on the base
        // program. Default OFF => useTess is always false => byte-identical.
        const bool useTess = s_cliffTessGate && s_terrainTessProgram
                             && s_terrainTessProgram->is_valid()
                             && (cmd.lodStep == 1 /* near-field proxy */);
        if (useTess) {
            // CLIFF-TESS-PERF: hoisted mirror. The expensive full enumerate-and-
            // copy (all active uniforms) runs ONCE PER FRAME on the first near-
            // field tess patch; every subsequent patch mirrors only the ~10
            // per-patch uniforms via the cached-location targeted fast path.
            const GLuint tessProg = s_terrainTessProgram->shp_;
            glUseProgram(tessProg);
            resolveTessPatchLocs(tessProg);  // one-time lazy location cache
            if (!s_tessMirroredThisFrame) {
                // Full mirror: copies frame-constant uniforms (mvp, mapSide,
                // material/POM/shadow/light state, samplers, u_cliffTessLevel).
                // Per-patch uniforms are (re)set by the targeted mirror below.
                mirrorTerrainUniforms(s_terrainProgram, tessProg);
                s_tessMirroredThisFrame = true;
            }
            mirrorTerrainPatchUniforms(s_terrainProgram, tessProg);
            ZoneScopedN("Terrain.CliffTess");
            // NOTE: global GL state, left set intentionally. Safe today because
            // this is the only GL_PATCHES draw in the frame; a future second
            // tessellated path with a different patch size MUST re-set this.
            glPatchParameteri(GL_PATCH_VERTICES, 3);
            glDrawElements(GL_PATCHES, patch.indexCount, GL_UNSIGNED_SHORT, 0);
            glUseProgram(s_terrainProgram);
        } else {
            glDrawElements(GL_TRIANGLES, patch.indexCount, GL_UNSIGNED_SHORT, 0);
        }

        if (patch.skirtIndexCount > 0 && skirtDepths != nullptr)
        {
            float skirtDepth = skirtDepths[i];
            if (skirtDepth > 0.0f)
            {
                if (s_locSkirtDepth >= 0)
                    glUniform1f(s_locSkirtDepth, skirtDepth);

                // (GL_CULL_FACE already disabled for the whole draw — see top.)
                // Attrib 0: lx, ly (first 2 int16_t of SkirtVertex, stride=8).
                // Attrib 1: isSkirt (third int16_t of SkirtVertex, offset=4).
                patch.skirtVbo.bind();
                glEnableVertexAttribArray(0);
                glVertexAttribIPointer(0, 2, GL_SHORT, (GLsizei)(4 * sizeof(int16_t)), (const void*)0);
                glEnableVertexAttribArray(1);
                glVertexAttribIPointer(1, 1, GL_SHORT, (GLsizei)(4 * sizeof(int16_t)), (const void*)(2 * sizeof(int16_t)));

                patch.skirtIbo.bind();
                // Phase 10.2b: draw ONLY the edges flagged in the per-block mask
                // (bit 0=N,1=S,2=W,3=E). No mask array -> all four (back-compat).
                const unsigned int mask = skirtEdgeMasks ? skirtEdgeMasks[i] : 0xFu;
                for (int e = 0; e < 4; ++e) {
                    if (((mask >> e) & 1u) == 0u) continue;
                    if (patch.skirtEdgeCount[e] <= 0) continue;
                    glDrawElements(GL_TRIANGLES, patch.skirtEdgeCount[e], GL_UNSIGNED_SHORT,
                                   (const void*)(size_t)(patch.skirtEdgeOffset[e] * sizeof(uint16_t)));
                }

                glDisableVertexAttribArray(1);
            }
        }
    }

    // TERRAIN-VISUAL-HEIGHT-S2-ALLLOD: per-band displaced-chunk-count telemetry.
    // Same cadence as [TerrainLOD v1] (every frame for the first 60, then every
    // 180th submit) so the acceptance pass has hard numbers per LOD band without
    // spamming the log. Only fires when the displace gate is actually active.
    if (visualDisplaceActive) {
        static unsigned long s_visFrame = 0;
        ++s_visFrame;
        if (s_visFrame <= 60 || (s_visFrame % 180) == 0) {
            mc2::fmt::print_out("[TerrainLOD] visualDisplace band-counts LOD0={:d} LOD1={:d} LOD2={:d} LOD3={:d} LOD4={:d} LOD5={:d}\n", mc2::fmt::as_i32(s_visualDisplacedCounts[0]), mc2::fmt::as_i32(s_visualDisplacedCounts[1]), mc2::fmt::as_i32(s_visualDisplacedCounts[2]), mc2::fmt::as_i32(s_visualDisplacedCounts[3]), mc2::fmt::as_i32(s_visualDisplacedCounts[4]), mc2::fmt::as_i32(s_visualDisplacedCounts[5]));
            fflush(stdout);
        }
    }
}

// Mark terrain drawn for the post-process passes.
void TerrainLodSubmitFrame::markDrawn() {
    // Terrain pixels are now on screen. Mark terrain drawn so the terrain-gated
    // post-process passes (runScreenShadow / runCloudShadow / runShoreline /
    // runGodRays) actually run. The chunk path is the default-on production
    // terrain draw (8z cutover); the legacy markTerrainDrawn() sites
    // (gameos_graphics.cpp tess draw + gos_terrain_patch_stream.cpp) do NOT
    // fire under this path, so sceneHasTerrain_ would otherwise stay false and
    // all four passes silently skip (root cause of the dead cloud-shadow pass).
    if (gosPostProcess* pp = getGosPostProcess()) {
        pp->markTerrainDrawn();
        // TERRAIN-SHORELINE-MASK-1 (recon landmine #6): when the terrain-side
        // wet/foam band is active, suppress the legacy screen-space
        // runShoreline() pass so the seam isn't brightened twice.
        //
        // LEGACY-SHORELINE-MISAIM-1: the AND with IsShorelineMaskActive() (i.e.
        // with MC2_TERRAIN_SHORELINE) is REMOVED. It encoded "don't brighten the
        // seam twice", but the legacy pass provably cannot reach a water pixel
        // on this path (see gos_TerrainShorelineLegacySuppressOn's block), so
        // there is no second brightening of the seam to avoid -- there is an
        // animated foam rim on prop silhouettes, and turning the V3 band off
        // must not bring it back. Suppression is now solely its own gate,
        // default ON.
        //
        // SHORELINE-DISPOSITION-1 (2026-07-31) CORRECTION: this comment used to
        // end "This call site stays inside the LOD-chunk draw ON PURPOSE: the
        // legacy terrain path (MC2_TERRAIN_LOD_CHUNK=0) never reaches it, so
        // that path keeps the pass, which is the one configuration where the
        // 0.15-0.35 classifier actually matches water." That is FALSE -- see the
        // CORRECTION block on gos_TerrainShorelineLegacySuppressOn() below.
        // gos_terrain.frag's materialAlpha write is behind #ifdef MRT_ENABLED,
        // which reaches exactly one program, and that program is never bound.
        // The classifier matches water in NO configuration.
        //
        // The call site staying here is therefore an ACCIDENT OF HISTORY, not a
        // design: under MC2_TERRAIN_LOD_CHUNK=0 the setter simply never runs and
        // the mis-aimed pass runs unsuppressed. That is a latent wart, not a
        // feature. It is left in place only because the correct fix is deleting
        // the pass outright rather than relocating a setter for a dead effect.
        pp->setShorelineSuppressedByTerrainMask(
            gos_TerrainShorelineLegacySuppressOn());
    }
}

// Unbind the terrain buffers and restore the saved GL state.
void TerrainLodSubmitFrame::restoreGlState() {
    RenderCore::framegraph::noteTerrainPath(RenderCore::framegraph::TerrainPath::LODChunk);  // TERRAIN-PATH-TELEMETRY-1

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_HEIGHT_SSBO_BINDING, 0);
    if (visualDisplaceActive)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_VISUAL_HEIGHT_SSBO_BINDING, 0);
    if (visualDampActive)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_VISUAL_DAMP_SSBO_BINDING, 0);
    if (!useGuards) {
        if (prevCullFace)   glEnable(GL_CULL_FACE);
        glDepthMask(prevDepthMask);
        if (!prevDepthTest) glDisable(GL_DEPTH_TEST);
        if (prevBlend)      glEnable(GL_BLEND);
        glDepthFunc((GLenum)prevDepthFunc);
    }
    // useGuards path: gCull/gBlend/gDepthState/gDepthTest restore depth/blend/
    // cull/mask/func when their optionals destruct at the closing brace below.
    glBindVertexArray((GLuint)prevVAO);
    glUseProgram((GLuint)prevProg);

}

// Hand the captured LOD0 patches to the VK island once GL state is restored.
void TerrainLodSubmitFrame::submitVkIsland(int& s_submitCount) {
#ifdef MC2_VULKAN_ISLAND
    // VK-P10 TERRAIN TIER-1: after the GL terrain bands drew (and GL state is fully
    // restored), the VK island co-writes the captured LOD0 patches into the SAME
    // shared scene GBuffer (color MRT0 + normal MRT1 + shared depth), reverse-Z
    // depth-tested against what GL just authored. Default-OFF -> feed empty -> no-op.
    // markTerrainDrawn already fired above (GL owns it); the island also latches it
    // for robustness if a frame is all-LOD0.
    //
    // FAIL-SOFT: run() is called whenever the gate is ENABLED (not only when we skipped)
    // so the island can BIRTH/heal even on a frame where we did NOT skip (ready==0 this
    // frame -> GL drew LOD0 -> feed empty -> run births the island + returns 0 harmlessly;
    // next frame ready==1 -> skip + VK draws). On a frame where we DID skip (ready was 1),
    // a mid-frame run()==0 (a fresh transient) latches s->disabled inside the island, so
    // ready flips 0 next frame (GL resumes LOD0) — at worst a single self-healing frame,
    // and the fail-soft log above fires. No persistent hole.
    // VK-TERRAIN-ALL-LODS diagnostic (rung 3 oracle evidence): per-600-submit
    // lodStep breakdown of what the VK island actually captured this submit,
    // so a smoke log proves coarse bands (lodStep>1) are reaching the island
    // (not just LOD0). Cheap (once per 600 submits), no gate needed.
    if (s_submitCount % 600 == 0) {
        int cnt1 = 0, cntCoarse = 0;
        for (const auto& p : s_vkTerrainPatches) {
            if (p.lodStep == 1) ++cnt1; else ++cntCoarse;
        }
        mc2::fmt::print_out("[VK_TERRAIN_ALL_LODS] submit={:d} feedValid={:d} active={:d} allLods={:d} vkPatches={:d} lodStep1={:d} coarse(lodStep>1)={:d}\n", mc2::fmt::as_i32(s_submitCount), mc2::fmt::as_i32(s_vkTerrainFeedValid ? 1 : 0), mc2::fmt::as_i32(s_vkTerrainActive ? 1 : 0), mc2::fmt::as_i32(mc2_vk_terrain_all_lods_enabled()), mc2::fmt::as_size(s_vkTerrainPatches.size()), mc2::fmt::as_i32(cnt1), mc2::fmt::as_i32(cntCoarse));
        fflush(stdout);
    }

    if (s_vkTerrainEnabled) {
        if (gosPostProcess* ppVk = getGosPostProcess()) {
            const int ran = mc2_vk_terrain_island_run(ppVk->getWidth(), ppVk->getHeight());
            // VK-PARITY-F3: the terrain pass just cleared color0 to the flat sky colour;
            // fill the untouched (depth==far) pixels with the HDRI sky GL would have drawn.
            if (ran) (void)mc2_vk_sky_island_run(ppVk->getWidth(), ppVk->getHeight());
            if (s_vkTerrainActive && s_vkTerrainFeedValid && ran == 0) {
                // We SKIPPED LOD0 this frame but VK failed to co-write -> this frame has a
                // 1-frame LOD0 gap. Loud one-shot; the island latched disabled so ready
                // will be 0 next frame and GL resumes drawing LOD0 (self-heal).
                static bool s_gapLogged = false;
                if (!s_gapLogged) {
                    mc2::fmt::print_err("[VK_TERRAIN_ISLAND] FAIL-SOFT: run()==0 AFTER LOD0-skip -> 1-frame LOD0 gap this frame; island latched off, GL resumes LOD0 next frame (self-heal).\n");
                    fflush(stderr);
                    s_gapLogged = true;
                }
            }
        }
    }
#endif
}

void gos_TerrainLodChunk_EnsureSceneClear() {
#ifdef MC2_VULKAN_ISLAND
    if (gosPostProcess* pp = getGosPostProcess())
        (void)mc2_vk_scene_clear(pp->getWidth(), pp->getHeight());
#endif
}

// Submits the LOD chunk draw commands. The early bail-outs, the top-level pass guard and the MVP fetch stay here;
// each phase below is a verbatim helper, run in the original order.
void gos_TerrainLodChunk_SubmitDrawCommands(
    const TerrainDrawCommand* cmds,
    const float*              skirtDepths,
    const unsigned char*      skirtEdgeMasks,
    const unsigned int*       edgeStitch,
    const int*                shadowTiers,
    const float*              morphFactors,
    int                       count)
{
    if (count == 0) return;
    if (s_terrainProgram == 0 || !s_heightSsbo.created()) return;
    if (s_patchVao == 0) return;

    // CLIFF-TESS-PERF: reset the once/frame full-mirror guard. This entrypoint is
    // called once per terrain draw (once/frame), so clearing it here means the
    // first near-field tess patch does the full enumerate-and-copy mirror and
    // every subsequent patch this frame takes the cheap targeted per-patch path.
    // Inside the gate-ON path's effect only (the flag is unread when the gate is
    // off), so gate-OFF remains byte-identical.
    s_tessMirroredThisFrame = false;

    // SAME-ORDER-EXECUTOR-VALIDATE-1: top-level validate-only wrapper (gate MC2_FRAMEGRAPH_EXECUTOR).
    // No-op when gate unset (byte-identical). PIN INVARIANT: additive only — no GL state change,
    // no reorder. markTerrainDrawn/g_dispatchMvp16/knownEarly undisturbed.
    render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::TerrainBase,
                                              "gos_TerrainLodChunk_SubmitDrawCommands");
    struct TopLevelGuard_ {
        ~TopLevelGuard_() {
            render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::TerrainBase,
                                                    "gos_TerrainLodChunk_SubmitDrawCommands");
        }
    } _tlGuard;

    // [RENDER_PASS v1] advisory telemetry (env-gated, rate-limited).
    // Chunk path is the default-on production terrain draw (8z cutover).
    render_contract::noteRenderPass(render_contract::PassIdentity::TerrainBase,
                                    "gos_TerrainLodChunk_SubmitDrawCommands");

    // Match the water-cull / decal frame-of-reference: use the baked dispatch MVP
    // when the solid pass is armed (== what the legacy terrain draw used), else
    // the live MVP. Eliminates the 1-frame offset that caused shore-water dropout
    // + decal tearing under camera motion (greybeard META-FIX).
    const float* mvp = gos_terrain_indirect::IsFrameSolidArmed()
                       ? gos_terrain_indirect_getDispatchMvp16()
                       : gos_GetTerrainMVPMat4();
    if (!mvp) mvp = gos_GetTerrainMVPMat4();
    {
        // Diag 7.5: log every time mvp is null (causes silent bail-out).
        static int s_mvpNullCount = 0;
        if (!mvp) {
            ++s_mvpNullCount;
            if (s_mvpNullCount <= 5 || s_mvpNullCount % 300 == 0)
                mc2::fmt::print_out("[TerrainLOD submit] mvp=NULL bail count={:d} (count={:d})\n", mc2::fmt::as_i32(s_mvpNullCount), mc2::fmt::as_i32(count));
            return;
        }
    }

    TerrainLodSubmitFrame f;
    f.saveGlStateAndGuards();
    f.bindProgramAndVisualHeight(mvp);
    bindAtlasAndShadowMaps();
    bindAuxTextures();
    bindMaterialAndOverlay();
    bindShorelineMask();
    uploadTerrainTuning();
    noteFirstSubmit(count);

    static int s_submitCount = 0;
    f.countSubmitAndPrepVk(count, mvp, s_submitCount);
    f.drawChunks(count, cmds, skirtDepths, skirtEdgeMasks, edgeStitch, shadowTiers, morphFactors);
    f.markDrawn();
    f.restoreGlState();
    f.submitVkIsland(s_submitCount);
}
