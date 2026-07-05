#ifndef GOS_POSTPROCESS_H
#define GOS_POSTPROCESS_H

#include "utils/gl_utils.h"

#include "../../RenderCore/RenderRegionId.h"   // RENDER-BACKEND-REGION-IFACE-1 (relative: reachable from every TU that includes this header, incl. RenderWorld)

struct glsl_program;

class gosPostProcess {
public:
    gosPostProcess();
    ~gosPostProcess();

    void init(int w, int h);
    void destroy();
    void resize(int w, int h);

    // Bind the HDR scene FBO so all rendering goes into it
    void beginScene();

    // Composite the HDR scene onto the default framebuffer via fullscreen quad
    void endScene();

    void renderSkybox(float sunDirX, float sunDirY, float sunDirZ);

    bool   isHdriReady()  const { return hdriReady_; }
    GLuint getHdriTex()   const { return hdriTex_; }   // WATER-HDRI-REFL-1

    // FRAME-GRAPH-EXECUTOR-ISLAND-1: read-only accessors used by the executor
    // wrapper (executorOwnBegin / executorOwnEnd) in gos_postprocess.cpp to
    // validate pre/postconditions without touching private members from outside
    // the class. These expose only what the executor's IslandContract checks.
    bool executorCompositeProgramValid() const;   // compositeProg_ && is_valid()
    bool executorSceneColorTexValid()    const;   // sceneColorTex_ != 0
    bool executorSceneHasTerrain()       const { return sceneHasTerrain_; }

    // FRAME-GRAPH-EXECUTOR-ISLAND-2: sub-stage accessors (EdgeFog + FogOob).
    // Each WillRun() mirrors the exact early-return gates of the matching run*().
    bool executorEdgeFogWillRun()      const;   // edgeFogEnabled_+prog_+mapHalfExtent_+terrain
    bool executorFogOobWillRun()       const;   // fogOobEnabled_+prog_+terrain
    bool executorSceneDepthTexValid()  const;   // sceneDepthTex_ != 0

    // FRAME-GRAPH-EXECUTOR-ISLAND-3: sub-stage accessors (Shoreline + CloudShadow).
    bool executorShorelineWillRun()    const;   // shorelineEnabled_+prog_+terrain
    bool executorCloudShadowWillRun()  const;   // enableCloudShadow_+prog_+terrain

    // EXECUTOR-ISLAND-SCREENSHADOW-1: sub-stage accessor for ScreenShadow.
    // WillRun() mirrors exact early-return gates of runScreenShadow() (lines 2033-2036):
    //   screenShadowEnabled_ && sceneHasTerrain_ && screenShadowProg_ valid && shadowsEnabled_
    // Unblocked by SCREENSHADOW-TEX-RESTORE-1 (a0b4189b); validate-only island.
    bool executorScreenShadowWillRun() const;   // screenShadowEnabled_+prog_+terrain+shadowsEnabled_

    // FRAMEGRAPH-APPLY-STATE-ISLAND-1: executor pre-apply for EdgeFog sub-stage.
    // Performs the 4 GL state calls that runEdgeFog() makes at entry:
    //   glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_)
    //   setSceneDrawBuffers(SingleColor, false)
    //   glViewport(0, 0, width_, height_)
    //   pipeline_binder::applyPipeline(PostProcessEdgeFog, "PostProcessEdgeFog")
    // This method touches GL; it lives in gos_postprocess.cpp (private member access).
    // Only called when executorEnabled() && executorEdgeFogWillRun() (gate: MC2_FRAMEGRAPH_EXECUTOR).
    void executorApplyEdgeFogState();
    // FRAMEGRAPH-APPLY-STATE-ISLAND-2: pre-apply declared GL state for FogOob/Shoreline/CloudShadow.
    // Mirrors executorApplyEdgeFogState(): FBO bind + setSceneDrawBuffers + glViewport + applyPipeline
    // only; no tex binds, no uniforms. Idempotent (body makes identical calls at entry).
    void executorApplyFogOobState();
    void executorApplyShorelineState();
    void executorApplyCloudShadowState();
    // APPLY-STATE-SCREENSHADOW-1: pre-apply declared GL state for ScreenShadow.
    void executorApplyScreenShadowState();

    float  getSkyYaw()    const { return skyYaw_; }    // WATER-HDRI-REFL-1: cached per-frame

    // HDRI-SKY-NUMBER-1: reload the HDRI texture to match theSkyNumber from the
    // .fit file.  Called by GameAdapters::Sky::setSkyNumber() at mission load.
    // No-op when HDRI is disabled (MC2_HDRI_SKY=0) or skyNumber==0.
    // Swaps hdriTex_ only when the resolved path differs from the current one.
    void setSkyNumber(int skyNumber);
    int  getSkyNumber() const { return skyNumber_; }  // SCENE-LIGHTING-STATE-1: last requested sky #

    // Renders the HDRI background as a fullscreen triangle.
    // Assumes scene FBO is bound. Writes only color attachment 0.
    // viewMat, projMat are column-major 4x4 floats (16 floats each).
    void renderHdriSkybox(const float* viewMat, const float* projMat);

    // HDRI-SKY frame fix (MC2_HDRI_SKY_FRAME_FIX ON path): renders the HDRI
    // background reconstructing the world ray directly from the camera's
    // WORLD-space basis (raw MC2 frame x=east, y=north, z=elevation) instead
    // of inverting view+proj. camFwd/camRight/camUp are float[3]; tHX/tHY are
    // tan(halfFOV) horizontal / vertical.
    void renderHdriSkyboxBasis(const float* camFwd, const float* camRight,
                               const float* camUp, float tHX, float tHY);

    // HDRI-SKY frame fix (MC2_HDRI_SKY_FRAME_FIX ON path, one-proven-matrix).
    // Unprojects NDC through the inverse of worldToClipGL (the EXACT matrix
    // the GPU rasterizes terrain with) to reconstruct the sky ray in the raw
    // MC2 world frame (x=east, y=north, z=elevation, Z-up). No camera-basis,
    // no FOV, no handedness guessing. invVP16 is column-major 16 floats =
    // inverse(worldToClipGL), uploaded verbatim (same convention as invProj).
    void renderHdriSkyboxInvVP(const float* invVP16);

    // Shadow mapping
    void initShadows();
    void destroyShadows();
    GLuint getSceneNormalTexture() const { return sceneNormalTex_; }
    GLuint getSceneDepthTexture() const { return sceneDepthTex_; }
    GLuint getSceneColorTexture() const { return sceneColorTex_; }
    GLuint getSceneFBO() const { return sceneFBO_; }
    // VFX-SOFT-PARTICLES-MVP-1: a copy of the resolved scene depth, taken
    // before the in-scene particle flush so particle shaders can sample it
    // WITHOUT a read-from-bound-attachment feedback loop (sceneDepthTex_ is the
    // active depth attachment during the flush). Lazily allocated on the first
    // copy (gated by MC2_VFX_SOFT_PARTICLES at the bridge) -> reads 0 and costs
    // nothing until then. DEPTH24_STENCIL8, full-res, matches sceneDepthTex_ so
    // glCopyImageSubData is a same-format copy.
    GLuint getSceneDepthCopyTexture() const { return sceneDepthCopyTex_; }
    void   copySceneDepthForParticles();
    // VFX-SCENECOLOR-GRAB-1: a feedback-safe copy of the resolved scene COLOR,
    // taken in the same frame window as the depth copy (after the opaque scene
    // color is resolved, before the in-scene VFX/transparent flush that would
    // sample it — sceneColorTex_ is the active color attachment during the
    // flush). FRAME_RESOURCE_SUBSTRATE: no consumer yet (distortion/refraction/
    // soft-color particles are future slices). Gated by MC2_VFX_SCENECOLOR_GRAB
    // at the bridge; lazily allocated on the first copy -> reads 0 and costs
    // nothing until then. RGBA16F, full-res, matches sceneColorTex_ so
    // glCopyImageSubData is a same-internalformat copy.
    GLuint getSceneColorCopyTexture() const { return sceneColorCopyTex_; }
    void   copySceneColorForVfx();
    // M1.5: readback hook for RenderWorld::lookupAtPixel.
    GLuint getSceneObjectIdTex() const { return sceneObjectIdTex_; }
    // WATER-REFLECTION-RESOURCE-1: 1/4-res reflection target (substrate only;
    // no producer renders into it until Phase C -> the texture reads black).
    GLuint getWaterReflectionTexture() const { return waterReflColorTex_; }
    GLuint getWaterReflectionFBO()     const { return waterReflFBO_; }
    int    getWaterReflectionWidth()   const { return waterReflW_; }
    int    getWaterReflectionHeight()  const { return waterReflH_; }
    // F3: explicit sentinel clear for GBuffer1 (attachment 1).
    // Sets attachment 1 to (0.5, 0.5, 1.0, 0.0) — flat-up encoded normal,
    // alpha = 0.0 (post-shadow eligible). Must be called while MRT is bound
    // (i.e., after beginScene's glDrawBuffers(2)). Defense-in-depth: every
    // visible pixel either gets overwritten by an explicit rc_gbuffer1_*
    // writer or inherits this sentinel.
    void clearGBuffer1();
    GLuint getShadowTexture() const { return shadowDepthTex_; }
    const float* getLightSpaceMatrix() const { return staticLightSpaceMatrix_; }
    GLuint getShadowFBO() const { return shadowFBO_; }
    int getShadowMapSize() const { return shadowMapSize_; }
    void beginShadowPass();
    void beginShadowPassNoClear();  // accumulate into existing shadow map
    void endShadowPass();
    bool shadowsEnabled_;

    // Shadow debug overlay
    bool showShadowDebug_;        // master toggle for debug overlay
    int shadowDebugMode_;         // 0=static, 1=dynamic
    void drawShadowDebugOverlay();

    // Shadow tuning — live-adjustable via ImGui; defaults match former hardcoded values.
    // Applied in beginShadowPass / beginShadowPassNoClear (static shadow pass only).
    float shadowBiasFactor_ = 2.0f;   // glPolygonOffset factor
    float shadowBiasUnits_  = 4.0f;   // glPolygonOffset units

    // Static world-fixed shadows: accumulate over multiple frames
    void buildStaticLightMatrix(float sunDirX, float sunDirY, float sunDirZ,
                                float mapHalfExtent);
    bool staticLightMatrixBuilt() const { return staticLightMatrixBuilt_; }
    void markStaticLightMatrixBuilt() { staticLightMatrixBuilt_ = true; }
    // VPL-#shadow C-1: per-mission re-arm so the one-shot full-map static
    // shadow rebuilds on the next mission (against fresh blocks[]) instead
    // of freezing the previous mission's shadow. Called from Terrain::destroy.
    void resetStaticLightMatrix() { staticLightMatrixBuilt_ = false; }
    void setMapHalfExtent(float extent) { mapHalfExtent_ = extent; }
    float getMapHalfExtent() const { return mapHalfExtent_; }
    void setWaterElevation(float elev) { waterElevation_ = elev; }
    // TERRAIN-SHORELINE-V3: read-only accessor so the chunk terrain driver
    // (gos_terrain_lod_chunk.cpp) can upload the SAME water elevation the
    // water fast path uses (Terrain::waterElevation, mirrored here at mission
    // load via setWaterElevation/gos_SetWaterElevation) as u_waterElevation
    // for elevation-based shoreline band placement.
    float getWaterElevation() const { return waterElevation_; }

    // Dynamic object shadows: camera-centered, re-rendered every frame
    void initDynamicShadows();
    void destroyDynamicShadows();
    // camFitCornersMC2 = 8 raw-MC2 frustum corners (clipToWorld-unprojected
    // + Stuff->MC2 swizzled by the caller). Builder clamps + fits the ortho.
    void buildDynamicLightMatrix(float sunDirX, float sunDirY, float sunDirZ,
                                 const float camFitCornersMC2[8][3],
                                 const float shadowCenterXYZ[3], bool shadowCenterValid);
    GLuint getDynamicShadowTexture() const { return dynShadowDepthTex_; }
    GLuint getDynamicShadowFBO() const { return dynShadowFBO_; }
    // When CSM is active during the caster pass, this returns the matrix for the
    // cascade currently being rendered (csmActiveCascade_), so the existing
    // batcher caster sites (which all upload getDynamicLightSpaceMatrix() as
    // "lightSpaceMatrix") render into the correct layer with NO batcher edits.
    // Outside the caster pass / when CSM is off it returns the legacy single matrix.
    const float* getDynamicLightSpaceMatrix() const {
        if (dynShadowArrayTex_ && csmActiveCascade_ >= 0 && csmActiveCascade_ < csmCount_)
            return &dynamicCascadeMatrices_[csmActiveCascade_ * 16];
        return dynamicLightSpaceMatrix_;
    }
    int getDynamicShadowMapSize() const { return dynShadowMapSize_; }

    // --- Item 1: Cascaded Shadow Maps (dynamic path) -----------------------
    // Gate (MC2_SHADOW_CSM, default ON since 2026-06-18 `8ff13a36`; "=0" opts
    // out to the legacy single-map path above, byte-identical). When ON
    // (the default), the dynamic shadow is a GL_TEXTURE_2D_ARRAY of csmCount
    // layers, each with its own light matrix.
    GLuint getDynamicShadowArrayTexture() const { return dynShadowArrayTex_; }
    int    getDynamicShadowCascadeCount() const { return csmCount_; }
    // Flat float[csmCount_*16] of per-cascade light-space matrices (col-major).
    const float* getDynamicCascadeMatrices() const { return dynamicCascadeMatrices_; }
    // Per-cascade world-texel footprint (2*cRad/mapSize), used for texel-scaled
    // depth bias in the shadow shaders. Same indexing as the cascade matrices.
    const float* getDynamicCascadeTexelWorld() const { return dynamicCascadeTexelWorld_; }
    // (farP-nearP) of the shared CSM ortho z-row; must match the C++ z-row exactly.
    float getCsmDepthSpan() const { return csmDepthSpan_; }
    // Per-cascade shadow resolution: the LAST cascade lives in a separate 2D
    // depth texture (dynamicFullMapTex_), the near cascades stay in the array.
    GLuint getDynamicFullMapTexture() const { return dynamicFullMapTex_; }
    int    getDynamicFullMapSize() const { return dynamicFullMapSize_; }
    // World-texel footprint of the last (full-map) cascade, computed with the
    // separate texture's resolution (NOT the array's). Used for the depth bias.
    float  getDynamicFullMapTexelWorld() const { return dynamicFullMapTexelWorld_; }

    // Bind the per-layer FBO + viewport + forward-Z clear for caster layer i.
    // Returns false if CSM inactive or i out of range (caller skips).
    bool beginDynamicShadowCascade(int i);
    void endDynamicShadowCascadePass();

    // Toggles and parameters
    float exposure_;

    // POST-FX-FXAA-1: FXAA post anti-aliasing. Default-OFF (byte-identical).
    // Seeded from env (MC2_FXAA / MC2_FXAA_SUBPIX / MC2_FXAA_EDGE_THRESHOLD /
    // MC2_FXAA_EDGE_THRESHOLD_MIN) at init; live-tunable via Graphics Options
    // (Ctrl+Shift+G). Defaults = the "sharper" preset.
    bool  fxaaEnabled_;
    float fxaaSubpix_;
    float fxaaEdgeThreshold_;
    float fxaaEdgeThresholdMin_;

    // POST-FX-CAS-1: AMD FidelityFX Contrast Adaptive Sharpening, folded into the
    // composite shader (same precedent as FXAA above). Applied to the final graded
    // LDR color; gate OFF -> u_casEnabled==0 -> shader skips it (byte-identical).
    // Seeded from MC2_POST_CAS / MC2_POST_CAS_SHARPNESS at init.
    bool  casEnabled_;
    float casSharpness_;  // 0..1, default 0.5

    // POST-FX-TONEMAP-V2: 0 off (default, byte-identical), 1 AgX, 2 Hable, 3 Reinhard.
    int   tonemapMode_;

    void runScreenShadow();
    bool screenShadowEnabled_;
    // APPLY-STATE-SCREENSHADOW-1: per-island executor-applied flag (see
    // cloudShadowStateAppliedByExecutor_). Set by executorApplyScreenShadowState();
    // runScreenShadow() skips its own 4 setup calls when true, then resets (one-shot).
    // Gate OFF → stays false → body sets state → byte-identical.
    bool screenShadowStateAppliedByExecutor_ = false;
    int screenShadowDebug_;  // 0=normal, 1=visualize

    // Cloud shadows — single fullscreen multiplicative pass (replaces the four
    // inline cloud blocks). Default-ON to preserve legacy look; tunable live.
    void runCloudShadow();
    glsl_program* cloudProg_;
    bool  enableCloudShadow_;   // seeded from mc2CloudShadowEnabled()
    // APPLY-STATE-REDUNDANT-BODY-REMOVE-2: set by executorApplyCloudShadowState()
    // when MC2_FRAMEGRAPH_EXECUTOR is ON; runCloudShadow() skips its own 4 setup
    // calls when true, then resets (one-shot). Gate OFF → stays false → body sets
    // state → byte-identical. Per-island flag (siblings share sceneFBO_/viewport).
    bool  cloudShadowStateAppliedByExecutor_ = false;
    float cloudStrength_;       // max darkening (factor floor = 1-strength)
    float cloudScale_;          // worldXY * scale (smaller = bigger clouds)
    float cloudScrollX_;
    float cloudScrollY_;
    float cloudThreshLo_;       // smoothstep coverage band low
    float cloudThreshHi_;       // smoothstep coverage band high
    int   cloudOctaves_;        // fbm octaves
    // Live getters/setters for ImGui.
    bool  getCloudShadowEnabled() const { return enableCloudShadow_; }
    void  setCloudShadowEnabled(bool b) { enableCloudShadow_ = b; }
    float getCloudStrength() const { return cloudStrength_; }
    void  setCloudStrength(float v) { cloudStrength_ = v; }
    float getCloudScale() const { return cloudScale_; }
    void  setCloudScale(float v) { cloudScale_ = v; }
    float getCloudScrollX() const { return cloudScrollX_; }
    void  setCloudScrollX(float v) { cloudScrollX_ = v; }
    float getCloudScrollY() const { return cloudScrollY_; }
    void  setCloudScrollY(float v) { cloudScrollY_ = v; }
    float getCloudThreshLo() const { return cloudThreshLo_; }
    void  setCloudThreshLo(float v) { cloudThreshLo_ = v; }
    float getCloudThreshHi() const { return cloudThreshHi_; }
    void  setCloudThreshHi(float v) { cloudThreshHi_ = v; }
    int   getCloudOctaves() const { return cloudOctaves_; }
    void  setCloudOctaves(int v) { cloudOctaves_ = v; }

    // Scene state — set by terrain draw, cleared each frame in beginScene()
    bool sceneHasTerrain_;
    bool prevFrameHadTerrain_;  // for clear color: blue-grey in gameplay, black in menus
    void markTerrainDrawn() { sceneHasTerrain_ = true; }

    // Shoreline
    bool shorelineEnabled_;
    // APPLY-STATE-REDUNDANT-BODY-REMOVE-2: per-island executor-applied flag (see
    // cloudShadowStateAppliedByExecutor_). Set by executorApplyShorelineState().
    bool shorelineStateAppliedByExecutor_ = false;
    void runShoreline();
    // TERRAIN-SHORELINE-MASK-1 (recon landmine #6 "double-shore"): set every
    // frame by the terrain chunk driver (gos_terrain_lod_chunk.cpp) to true
    // when a terrain-side wet/foam shoreline mask is active (gate ON + sidecar
    // loaded). runShoreline() yields when this is true so the screen-space
    // foam halo and the new terrain-side band never brighten the seam twice.
    // Default false -> runShoreline() behaves exactly as before (byte-identical).
    bool shorelineSuppressedByTerrainMask_ = false;
    void setShorelineSuppressedByTerrainMask(bool b) { shorelineSuppressedByTerrainMask_ = b; }

    // SSAO-GTAO-LITE-MVP-1 (Track V, MC2_SSAO). Half-res world-space AO.
    // Default OFF -> runSSAO() is skipped entirely (byte-identical). Resolved
    // from env at init; tunables ImGui- and profile-adjustable.
    bool  ssaoEnabled_;
    int   ssaoDebug_;       // 0 = multiplicative apply, 1 = show AO grayscale
    float aoRadius_;        // world-unit sample radius
    float aoStrength_;      // 0..2 occlusion strength
    float aoBias_;          // window-depth compare bias
    float aoPower_;         // contrast curve
    void runSSAO();

    // BT2018-BOX-DECAL-1: screen-space AABB decal volume. Reconstructs scene world
    // pos from the depth COPY (reversed-Z), box-clips, normal-rejects, and composites
    // a procedural decal into COLOR0 only. Default-OFF (MC2_BOX_DECAL). v1 = single
    // camera-anchored debug box + procedural pattern (no texture/producers yet).
    void drawBoxDecals();

    // EDGE-FOG-1: world-space map-edge fog on geometry pixels.
    // Fades terrain/props/mechs near the map boundary into the cloud color.
    // Default ON (MC2_EDGE_FOG=0 to disable).
    bool  edgeFogEnabled_  = false;
    // APPLY-STATE-REDUNDANT-BODY-REMOVE-1: set by executorApplyEdgeFogState() when
    // MC2_FRAMEGRAPH_EXECUTOR is ON and the executor pre-applies EdgeFog's 4 setup
    // GL calls (FBO/drawBuffers/viewport/applyPipeline) before runEdgeFog() runs.
    // runEdgeFog() checks this flag: if true, skips its own redundant 4 setup calls
    // (executor is sole setter), then resets to false (one-shot per frame).
    // Gate OFF (executor never called): flag stays false → body sets state as before
    // → byte-identical to pre-slice behavior.
    bool  edgeFogStateAppliedByExecutor_ = false;
    // FOG-EXPOSURE-HEADROOM-1: fog composites into sceneFBO_ BEFORE the composite
    // pass applies exposure_ + the unconditional warm grade (postprocess.frag:89,
    // 95-112). At the near-white {0.93,0.94,0.95} the graded missions (mc2_24
    // exposure 1.1, mc2_10 1.05) drove fog past 1.0 -> blown-out white cloud
    // ("fog super bright in some spots"). Drop the base so exposure*grade has
    // headroom: 0.80 * 1.1(exp) * 1.10(grade) ~= 0.97, still a bright cloud bank
    // but no clip. Env MC2_OOB_FOG_COLOR still overrides oob at runtime.
    float edgeFogColor_[3] = {0.80f, 0.81f, 0.83f};
    float edgeFogStart_    = 50.0f;    // world units inside boundary where fog begins
    float edgeFogHeight_   = 2000.0f;  // cloud bank top in world Z (MC2_EDGE_FOG_HEIGHT)
    float edgeFogMax_      = 0.92f;    // max opacity
    glsl_program* edgeFogProg_ = nullptr;
    void  runEdgeFog();

    // RENDER-BACKEND-REGION-IFACE-1 (Layer-6 ENTRY). The PostprocessFog region
    // (EdgeFog + OOB fog) made SELECTABLE GL-vs-Vulkan through IRenderBackend.
    //
    // Master gate MC2_RENDER_BACKEND_REGION_IFACE (default OFF): OFF -> the
    // ORIGINAL direct-GL fog sites run (abstraction fully skipped, byte-identical).
    // ON -> endScene() routes the fog region through runFogRegionThroughBackend(),
    // which builds the backend-neutral context, selects the backend
    // (MC2_POSTPROCESS_BACKEND=gl|vk, default gl), and calls
    // IRenderBackend::runRegion(); GL falls back on any failure.
    bool regionIfaceEnabled() const;   // MC2_RENDER_BACKEND_REGION_IFACE==1

    // 1A: the extracted GL fog region wrapper. Calls runEdgeFog() then runFogOob()
    // UNCHANGED (same two inline bodies, same order). Returns the number of GL fog
    // passes that actually executed (always 2 here; the inner run*() early-returns
    // still short-circuit per-effect but the wrapper's contract is "run both GL
    // sites"). This is what the GL backend's runRegion() invokes.
    int  runFogRegionGL();

    // 1B: the routed entry called from endScene() when the master gate is ON. Builds
    // the neutral context, picks the backend by MC2_POSTPROCESS_BACKEND, calls
    // runRegion(); on false (VK unavailable/failed) runs runFogRegionGL(). Writes
    // the region_impl / fallback_reason / equivalence counters to the health snapshot.
    void runFogRegionThroughBackend();

    // GL handle resolver for the region context. gosPostProcess owns
    // sceneColorTex_/sceneDepthTex_, so IT is the resolver: MainColor->sceneColorTex_,
    // MainDepth->sceneDepthTex_. Defined as a tiny adaptor in the .cpp; the neutral
    // context carries a pointer to it (a resolver is a SERVICE, not a handle).
    unsigned int resolveRegionResource(RenderCore::RenderResourceId id) const;

#ifdef MC2_VULKAN_ISLAND
    // VULKAN-EDGE-FOG-ISLAND-2a: the first REAL Vulkan render work in the frame
    // loop. When MC2_VULKAN_EDGE_FOG_ISLAND=1 AND lazy Vulkan init succeeds, the
    // edge-fog composite runs on a headless Vulkan device instead of the GL path
    // (see the seam in beginPostProcess()/composite). All Vulkan state is held in
    // an opaque struct so the Vulkan headers never leak into this header.
    //
    // vulkanEdgeFogIslandEnabled(): reads MC2_VULKAN_EDGE_FOG_ISLAND once, lazily
    // initializes the device on first call, and returns true only if init
    // succeeded. On failure it logs once, disables itself, and returns false so
    // the caller falls back to runEdgeFog() (the GL path). Never throws/crashes.
    // runEdgeFogVulkan(): per-frame GL->Vulkan->GL edge-fog composite.
    // destroyVulkanEdgeFogIsland(): torn down from destroy().
    bool  vulkanEdgeFogIslandEnabled();
    void  runEdgeFogVulkan();
    void  destroyVulkanEdgeFogIsland();
    struct VulkanEdgeFogIsland;   // defined in vulkan_edge_fog_island.cpp
    VulkanEdgeFogIsland* vkFogIsland_ = nullptr;

    // VULKAN-OOB-FOG-ISLAND-1: the SECOND offscreen Vulkan island (proves the
    // VULKAN-ISLAND-TEMPLATE-1 checklist repeats). When MC2_VULKAN_OOB_FOG_ISLAND=1
    // AND lazy Vulkan init succeeds, the OOB-fog composite runs on a headless Vulkan
    // device instead of the GL runFogOob() path. Opaque pointer only -- no Vulkan
    // headers leak into this header.
    // vulkanOobFogIslandEnabled(): reads MC2_VULKAN_OOB_FOG_ISLAND once, lazily
    // initializes the device on first call; false on failure -> GL fallback.
    // runFogOobVulkan(): per-frame GL->Vulkan->GL OOB-fog composite.
    // destroyVulkanOobFogIsland(): torn down from destroy().
    bool  vulkanOobFogIslandEnabled();
    void  runFogOobVulkan();
    void  destroyVulkanOobFogIsland();
    struct VulkanOobFogIsland;   // defined in vulkan_oob_fog_island.cpp
    VulkanOobFogIsland* vkOobFogIsland_ = nullptr;

    // VULKAN-POSTPROCESS-SUBGRAPH-1: the Layer-4 milestone -- fuse the EdgeFog +
    // OOB-fog islands into ONE native Vulkan subgraph. A Vulkan-owned intermediate
    // color image is copied in from GL sceneColor ONCE, blended by BOTH fog passes
    // (edge then oob) in ONE render pass / ONE subpass / TWO draws (NO CPU readback
    // between the two passes, 2 fewer copies than running both islands), then copied
    // back to GL ONCE. Opaque pointer only -- no Vulkan headers leak into this header.
    //
    // vulkanPostprocessSubgraphEnabled(): reads MC2_VULKAN_POSTPROCESS_SUBGRAPH once,
    // lazily inits the device on first call; false on failure -> GL fallback (BOTH GL
    // fog passes run). runPostprocessSubgraph(): per-frame fused GL->Vulkan->GL edge+
    // oob composite (does BOTH fog effects). destroyVulkanPostprocessSubgraph(): torn
    // down from destroy().
    bool  vulkanPostprocessSubgraphEnabled();
    // POSTPROCESS-VULKAN-BACKEND-OPT-IN: the env-gate-FREE half of
    // vulkanPostprocessSubgraphEnabled() -- lazy init + size-match + device-validity,
    // with NO MC2_VULKAN_POSTPROCESS_SUBGRAPH check. Returns true iff the subgraph is
    // ready to render THIS frame. Both the standalone gate path (after its env check)
    // and the region-iface Vulkan backend (whose backend=vk selection IS the enable
    // signal) route through this, so backend=vk runs Vulkan without the subgraph gate.
    bool  postprocessSubgraphReadyForRegion();
    void  runPostprocessSubgraph();
    void  destroyVulkanPostprocessSubgraph();

    // RENDER-BACKEND-REGION-IFACE-1 (1C): Vulkan impl of runRegion for the
    // PostprocessFog region. Returns false (fail-soft) if the subgraph is
    // unavailable/failed this frame -> caller runs the GL wrapper (FallbackGL).
    // On success runs runPostprocessSubgraph() and reports impl=VulkanSubgraph
    // with vkDraws=2. Never crashes. Defined in gos_postprocess.cpp under the
    // same #ifdef; the neutral context values already match the subgraph's
    // EXACT uploads (both pull from the same members).
    bool  runFogRegionVulkanBackend(const RenderCore::PostprocessFogRegionContext& ctx,
                                     RenderCore::RegionOutput* out);
    struct VulkanPostprocessSubgraph;   // defined in vulkan_postprocess_subgraph.cpp
    VulkanPostprocessSubgraph* vkPostprocessSubgraph_ = nullptr;
#endif

    // OOB-FOG-1: fullscreen fog over out-of-bounds far-plane pixels.
    // Default ON (MC2_OOB_FOG=0 to disable). Reads only scene depth —
    // no sceneColorTex_ feedback loop; blends SRC_ALPHA over scene color.
    bool  fogOobEnabled_ = false;
    // APPLY-STATE-REDUNDANT-BODY-REMOVE-2: per-island executor-applied flag (see
    // cloudShadowStateAppliedByExecutor_). Set by executorApplyFogOobState().
    bool  fogOobStateAppliedByExecutor_ = false;
    // FOG-EXPOSURE-HEADROOM-1: see edgeFogColor_ above. fog_oob.frag's noise
    // "sunlit tops" (col = fogColor*1.05, line 101) clip FIRST under exposure,
    // which is what read as bright *spots*. 0.80 base * 1.05(top) * 1.1(exp) *
    // 1.10(grade) ~= 1.01 -- tops stay bright, no saturated white blob.
    float oobFogColor_[3] = {0.80f, 0.81f, 0.83f}; // bright cloud bank (exposure-headroom)
    float oobFogOpacity_ = 1.0f;
    void  runFogOob();

    // FOG-HORIZON-CLAMP-1: reshape the OOB/edge-fog elevation profile so fog is
    // FULL at/below the horizon (covers OOB terrain/water sideways + down) and
    // fades to ZERO within a small elevation band just above the horizon, leaving
    // the sky above clear. Supersedes the old worldDir.z 0.22 exclusion band
    // (which let fog survive to ~12deg+ elevation, bleeding up into the sky).
    //   MC2_FOG_HORIZON_FADE_START -- elevation (deg) where the fade begins; fog
    //                                 still full at/below this.  Default 0.
    //   MC2_FOG_HORIZON_FADE_END   -- elevation (deg) where fog reaches zero.
    //                                 Default 5.  Must be > start.
    //   MC2_FOG_HORIZON_CLAMP=0    -- kill-switch: restore the previous profile
    //                                 (no elevation clamp; old worldDir.z band).
    // Shader form is cheap: we pass the SINES of the two angles (elevSin =
    // -worldDir.z for a normalized dir), so the frag does a plain smoothstep on
    // -worldDir.z with no per-fragment trig.
    bool  fogHorizonClampEnabled_ = true;    // default ON (rides existing fog gate)
    float fogHorizonFadeStartDeg_ = 0.0f;
    float fogHorizonFadeEndDeg_   = 5.0f;
    float fogHorizonFadeStartSin_ = 0.0f;    // sin(0deg)
    float fogHorizonFadeEndSin_   = 0.08716f;// sin(5deg); recomputed from env in ctor

    // HZB-DEPTH-PYRAMID-MVP-1 (TRACKRV-HZB-VISIBILITY-OPUS-1). Gated reverse-Z
    // Hi-Z depth pyramid built from sceneDepthTex_ via a custom fragment MIN
    // reduction (shaders/hzb_reduce.frag). Default-OFF (MC2_HZB_BUILD): when OFF
    // the texture/FBO are never allocated and runHzbReduce() is a no-op, so
    // output is byte-identical. Diagnostic substrate ONLY -- no consumers, no
    // culling, no draw suppression.
    bool   hzbEnabled_ = false;     // resolved once from env at init()
    bool   hzbProbeEnabled_ = false; // MC2_HZB_PROBE (requires hzbEnabled_)
    void   runHzbReduce();          // build the pyramid each frame (gated)
    void   runHzbProbe();           // HZB-OCCLUSION-PROBE-1: diagnostic only
    // Pyramid is stored as one ceil-sized R32F texture PER level (not a mip
    // chain): AMD rejects attaching mip level >0 of a mipmap-incomplete texture,
    // and the ceil ladder is deliberately mipmap-incomplete. Separate textures
    // sidestep that and remove any read/write feedback (distinct objects).
    GLuint getHzbLevelTexture(int level) const {
        return (level >= 0 && level < hzbMipCount_) ? hzbLevelTex_[level] : 0;
    }
    GLuint getHzbTexture()  const { return hzbLevelTex_[0]; } // level 0 (full-res)
    int    getHzbMipCount() const { return hzbMipCount_; }
    int    getHzbWidth()    const { return hzbW_; }
    int    getHzbHeight()   const { return hzbH_; }
    bool   isHzbEnabled()   const { return hzbEnabled_; }
    unsigned long long getHzbBuildCount() const { return hzbBuildCount_; }

    // LOWLIGHT-NIGHTVISION-MVP-1: pure-postprocess night-vision tunables.
    // Active only when the selected ViewMode is LowLight (5); the composite
    // branch is otherwise skipped (Visual byte-identical). Seeded from
    // MC2_VIEWMODE_LOWLIGHT_GAIN / _TINT at init; ImGui- and profile-adjustable.
    float lowLightGain_    = 2.5f;                 // luminance amplification
    float lowLightTint_[3] = { 0.7f, 1.0f, 0.6f }; // green-phosphor NV tint

    void setInverseViewProj(const float* m) { memcpy(inverseViewProj_, m, 16 * sizeof(float)); }
    void setViewProj(const float* m) { memcpy(viewProj_, m, 16 * sizeof(float)); }
    const float* getInverseViewProj() const { return inverseViewProj_; }
    const float* getViewProj() const { return viewProj_; }
    int getWidth()  const { return width_; }
    int getHeight() const { return height_; }

private:
    void createFBOs(int w, int h);
    void destroyFBOs();
    void createFullscreenQuad();
    void destroyFullscreenQuad();
    void createBoxDecalCube();   // BT2018-BOX-DECAL-1: unit cube [-0.5,0.5], 8 verts / 36 idx
    void destroyBoxDecalCube();

    // Scene FBO (full resolution, HDR)
    GLuint sceneFBO_;
    GLuint sceneColorTex_;
    GLuint sceneDepthTex_;
    // SKYBOX-FOG-EXCLUDE-1: stencil-only view onto sceneDepthTex_'s storage
    // (GL_DEPTH_STENCIL_TEXTURE_MODE=GL_STENCIL_INDEX baked in via glTextureView,
    // so it can be sampled in the same draw as the depth-mode sceneDepthTex_
    // binding without per-bind mode toggling). Lazily created; 0 when
    // MC2_SKYBOX_FOG_EXCLUDE=0 (never allocated -> zero cost OFF).
    GLuint sceneStencilViewTex_ = 0;
    GLuint sceneNormalTex_;
    GLuint sceneObjectIdTex_ = 0;   // M1.5 R32UI MRT attachment-2 (gated on MC2_OBJECT_ID_BUFFER)
    GLuint sceneDepthCopyTex_ = 0;  // VFX-SOFT-PARTICLES-MVP-1 lazy depth copy (DEPTH24_STENCIL8)
    GLuint sceneColorCopyTex_ = 0;  // VFX-SCENECOLOR-GRAB-1 lazy color copy (RGBA16F)

    // WATER-REFLECTION-RESOURCE-1: quarter-res reflection target (color + depth).
    // Allocated in createFBOs, freed in destroyFBOs, registered in
    // RenderResourceRegistry. No producer until Phase C (reads black).
    GLuint waterReflFBO_      = 0;
    GLuint waterReflColorTex_ = 0;
    GLuint waterReflDepthTex_ = 0;
    int    waterReflW_        = 0;
    int    waterReflH_        = 0;

    // Fullscreen quad
    GLuint quadVAO_;
    GLuint quadVBO_;

    // BT2018-BOX-DECAL-1: unit-cube geometry for the decal volume.
    GLuint        boxCubeVAO_ = 0;
    GLuint        boxCubeVBO_ = 0;
    GLuint        boxCubeIBO_ = 0;
    glsl_program* boxDecalProg_ = nullptr;
    bool          boxDecalEnabled_ = false;
    // DECAL-INTEGRATE-1: MC2_PROJECTED_DECALS (default OFF). When ON, drawBoxDecals()
    // consumes the dynamic_decal_ring live slots (one projected decal per impact) instead
    // of the v1 hardcoded screen-center test box. OFF = projection pass does not run.
    bool          projectedDecalsEnabled_ = false;

    // Composite shader
    glsl_program* compositeProg_;

    // Skybox
    glsl_program* skyboxProg_;

    // HDRI-SKY-1: background sky from a single equirect HDRI.
    // All HDRI state is null/false when MC2_HDRI_SKY=0 or load fails.
    GLuint        hdriTex_         = 0;
    glsl_program* hdriSkyboxProg_  = nullptr;
    bool          hdriEnabled_     = false;  // resolved once from env at init
    bool          hdriReady_       = false;  // true iff tex + program both valid
    GLuint        hdriDummyVao_    = 0;      // fallback when quadVAO_ unavailable
    // SKYBOX-FOG-EXCLUDE-1: stencil-only tag pass program (hdri_skybox_stencil_tag.frag).
    // Compiled lazily on first use under MC2_SKYBOX_FOG_EXCLUDE=1; null/no-op when
    // the gate is off (zero cost OFF).
    glsl_program* hdriSkyboxStencilTagProg_ = nullptr;
    // HDRI-SKY Item 2: GL-equirect azimuth (radians) of the baked sun in the
    // EXR, derived at load time by luminance-weighted centroid of the brightest
    // above-horizon texels. NaN => scan unavailable (sun-sync stays disabled).
    float         hdriBakedSunAz_  = 0.0f;
    bool          hdriBakedSunValid_ = false;
    float         skyYaw_          = 0.0f;  // WATER-HDRI-REFL-1: cached after each sky render
    // HDRI-SKY-NUMBER-1: path of the currently loaded HDRI (used by setSkyNumber
    // to detect when a swap is actually needed).
    char          hdriCurrentPath_[256] = {};
    int           skyNumber_       = -1;     // SCENE-LIGHTING-STATE-1: last requested sky #

    // Dimensions
    int width_;
    int height_;

    bool initialized_;

    // Shadow map
    GLuint shadowFBO_;
    GLuint shadowDepthTex_;
    GLuint shadowDummyColorTex_;  // AMD needs a color attachment for rasterization
    glsl_program* shadowDepthProg_;
    int shadowMapSize_;
    int savedViewport_[4];
    float staticLightSpaceMatrix_[16]; // world-fixed ortho, built once at map load
    bool staticLightMatrixBuilt_;      // true after light matrix is built (first frame)
    float mapHalfExtent_;              // half the map size in world units
    float waterElevation_ = 0.0f;     // sea-level world Z (Terrain::waterElevation)

    // Dynamic object shadow FBO (1024x1024, camera-centered, per-frame)
    GLuint dynShadowFBO_;
    GLuint dynShadowDepthTex_;
    GLuint dynShadowDummyColorTex_;
    int dynShadowMapSize_;
    float dynamicLightSpaceMatrix_[16];
    glsl_program* shadowDebugProg_;

    // --- Item 1 CSM state (only used when MC2_SHADOW_CSM gate is ON) --------
    static const int kMaxCsmCascades = 3;
    GLuint dynShadowArrayFBO_;                 // FBO whose depth attach is one array layer
    GLuint dynShadowArrayDummyColorTex_;       // 7900 XTX FBO-completeness dummy (array)
    GLuint dynShadowArrayTex_;                 // GL_TEXTURE_2D_ARRAY depth (4096^2 x N)
    int    csmCount_;                          // clamp(MC2_SHADOW_CSM_COUNT,1,3)
    float  dynamicCascadeMatrices_[kMaxCsmCascades * 16]; // per-cascade col-major
    float  dynamicCascadeTexelWorld_[kMaxCsmCascades];    // 2*cRad/mapSize per cascade, world units
    float  csmDepthSpan_;                                 // (farP-nearP) of shared ortho z-row
    int    csmActiveCascade_;                  // cascade the caster pass is drawing
    int    csmDebugLayer_;                     // which layer the debug blit shows

    // --- Per-cascade shadow resolution: separate full-map (last) cascade -----
    // The near cascades (0..N-2) live in dynShadowArrayTex_ at dynShadowMapSize_.
    // The LAST cascade (N-1, map-centered full-map catch-all) renders into this
    // separate, lower-res 2D depth texture to save VRAM while keeping the near
    // cascades razor sharp. Same sampler params as the array. When
    // MC2_SHADOW_FULLMAP_SEPARATE=0, dynamicFullMapSize_ == dynShadowMapSize_ so
    // the VRAM is equivalent to the old all-in-array layout (A/B fallback).
    GLuint dynamicFullMapTex_;                  // GL_TEXTURE_2D depth (last cascade)
    GLuint dynamicFullMapFbo_;                  // depth-only FBO for the full-map cascade
    GLuint dynamicFullMapDummyColorTex_;        // 7900 XTX FBO-completeness dummy
    int    dynamicFullMapSize_;                 // edge in texels (default 4096)
    float  dynamicFullMapTexelWorld_;           // 2*cRad/dynamicFullMapSize_ for last cascade

    // CSM caster-loop GL-state save/restore. beginDynamicShadowCascade() captures
    // the caller's full scene GL state on the FIRST cascade (csmStateSaved_ flips
    // true); endDynamicShadowCascadePass() restores it on the LAST cascade. This
    // mirrors gosRenderer::begin/endDynamicShadowPass (captureShadowGLState /
    // restoreShadowGLState in gameos_graphics.cpp) so the forward-Z 4096^2 array
    // pass does not leak FBO/viewport/depth/blend/cull into the reverse-Z scene
    // passes (props/actors/water) that draw after the cascade loop.
    bool      csmStateSaved_ = false;
    GLint     csmSavedFBO_ = 0;
    GLint     csmSavedViewport_[4] = {0,0,0,0};
    GLfloat   csmSavedClearDepth_ = 0.0f;
    GLboolean csmSavedDepthTest_ = GL_FALSE;
    GLint     csmSavedDepthFunc_ = 0;
    GLboolean csmSavedDepthMask_ = GL_TRUE;
    GLboolean csmSavedBlend_ = GL_FALSE;
    GLboolean csmSavedCull_ = GL_FALSE;

    // Post-process screen shadow
    glsl_program* screenShadowProg_;
    float inverseViewProj_[16];
    float viewProj_[16];

    // Shoreline
    glsl_program* shorelineProg_;

    // OOB fog
    glsl_program* fogOobProg_ = nullptr;

    // SSAO (half resolution)
    GLuint        ssaoFBO_      = 0;
    GLuint        ssaoColorTex_ = 0;   // GL_R16F half-res AO
    int           ssaoW_        = 0;
    int           ssaoH_        = 0;
    glsl_program* ssaoProg_      = nullptr;
    glsl_program* ssaoApplyProg_ = nullptr;

    // HZB depth pyramid (HZB-DEPTH-PYRAMID-MVP-1). Allocated lazily in
    // createFBOs only when hzbEnabled_. One ceil-sized R32F texture per level.
    static const int kHzbMaxLevels = 24;     // covers >16M px on the long axis
    GLuint        hzbLevelTex_[kHzbMaxLevels] = {0};
    GLuint        hzbFBO_        = 0;         // shared; re-attaches the dst level
    int           hzbW_          = 0;
    int           hzbH_          = 0;
    int           hzbMipCount_   = 0;
    unsigned long long hzbBuildCount_ = 0;  // frames the pyramid has been built
    glsl_program* hzbReduceProg_ = nullptr;

    // HZB-CAMERA-DISCONTINUITY-GUARD-1: previous-frame camera pose (derived in
    // the probe by unprojecting NDC near/far centers via inverseViewProj_) to
    // detect near-instant camera snaps (e.g. mc2_17 intro 180deg). Diagnostic
    // only -- flags frames as unsafe-for-cull; affects no rendering.
    bool   hzbPrevCamValid_ = false;
    float  hzbPrevCamPos_[3] = { 0.0f, 0.0f, 0.0f };
    float  hzbPrevCamFwd_[3] = { 0.0f, 0.0f, 0.0f };
    unsigned long long hzbCamDiscontinuityFrames_ = 0;
};

gosPostProcess* getGosPostProcess();

// MISSION-VISUAL-TUNING-1: free-function accessors for profile system.
float gos_GetExposure();
void  gos_SetExposure(float v);

// VIEWMODE-POSTPROCESS-PRESENTATION-1: view-mode framework accessors.
// Gate MC2_VIEWMODE_FRAMEWORK resolved once at init(). When OFF, GetSelectedViewMode
// returns 0 (Visual) regardless of what ImGui has set. Pattern mirrors gos_SetExposure.
bool gos_IsViewmodeFrameworkEnabled();
int  gos_GetSelectedViewMode();   // returns 0 when gate OFF
void gos_SetSelectedViewMode(int m);  // clamped to RenderCore::ViewMode range

// LOWLIGHT-NIGHTVISION-MVP-1 tunables (profile + ImGui). Clamped to safe ranges.
void  gos_SetLowLightGain(float v);
float gos_GetLowLightGain();
void  gos_SetLowLightTintG(float v); // green channel — the most impactful axis

// SSAO-GTAO-LITE-MVP-1 tunables (profile + ImGui).
bool  gos_IsSsaoEnabled();
void  gos_SetSsaoRadius(float v);
void  gos_SetSsaoStrength(float v);
void  gos_SetSsaoBias(float v);
float gos_GetSsaoRadius();
float gos_GetSsaoStrength();
float gos_GetSsaoBias();

// HDRI-SKY-NUMBER-1: notify the postprocessor of the mission sky number so it
// can swap to the appropriate HDRI asset (IblHdriRegistry).
// Call at mission load after the mission sky number is known.
// No-op when HDRI is disabled or skyNumber is out of range (1-21).
void gos_SetSkyNumber(int skyNumber);

#endif // GOS_POSTPROCESS_H
