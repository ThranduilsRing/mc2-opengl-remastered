#ifndef GOS_POSTPROCESS_H
#define GOS_POSTPROCESS_H

#include <cstdint>
#include <memory>
#include <cstring>

struct HdriCpuImage;   // gos_hdri.h
#include "../../RenderCore/RenderResourceRegistry.h"   // RenderCore::RenderResourceId (relative: reachable from every TU that includes this header, incl. RenderWorld)

struct Mc2SharedSceneDepth;
struct Mc2SharedSceneColor;

struct Mc2ColorPropFeed;

class gosPostProcess {
public:
    gosPostProcess();
    ~gosPostProcess();

    void init(int w, int h);
    void destroy();
    void resize(int w, int h);

    // Begin CPU scene bookkeeping; native owners clear their attachments.
    void beginScene();

    // Composite the HDR scene into the Vulkan present image
    void endScene();

    bool   isHdriReady()  const { return hdriReady_; }
    uint64_t getHdriGeneration() const { return hdriCpu_ ? hdriCpuGeneration_ : 0; }
    const struct HdriCpuImage* getHdriCpu() const { return hdriCpu_.get(); }

    // FRAME-GRAPH-EXECUTOR-ISLAND-1: read-only accessors used by the executor
    // wrapper (executorOwnBegin / executorOwnEnd) in gos_postprocess.cpp to
    // validate pre/postconditions without touching private members from outside
    // the class. These expose only what the executor's IslandContract checks.
    bool executorSceneColorReady()    const;   // actual native color image readiness
    bool executorSceneHasTerrain()       const { return sceneHasTerrain_; }

    float  getSkyYaw()    const { return skyYaw_; }    // WATER-HDRI-REFL-1: cached per-frame

    // HDRI-SKY-NUMBER-1: reload the HDRI texture to match theSkyNumber from the
    // .fit file.  Called by GameAdapters::Sky::setSkyNumber() at mission load.
    // No-op when HDRI is disabled (MC2_HDRI_SKY=0) or skyNumber==0.
    // Replaces the retained source only when the resolved path differs from the current one.
    void setSkyNumber(int skyNumber);
    int  getSkyNumber() const { return skyNumber_; }  // SCENE-LIGHTING-STATE-1: last requested sky #

    // Player post-FX options (options.cfg PostCAS / PostCASSharpness / TonemapMode); see gos_SetPostFxOptions().
    void setPostFxOptions(bool casEnabled, float casSharpness, int tonemapMode) {
        casEnabled_ = casEnabled; casSharpness_ = casSharpness; tonemapMode_ = tonemapMode;
    }

    // Renders the HDRI background as a fullscreen triangle.
    // The Vulkan terrain pass consumes the queued sky after drawing terrain.
    // viewMat, projMat are column-major 4x4 floats (16 floats each).
    void renderHdriSkybox(const float* viewMat, const float* projMat);

    // Shadow mapping
    void initShadows();
    void destroyShadows();
    bool sceneColorReady() const;
    bool sceneNormalReady() const;
    bool sceneObjectIdReady() const;
    const float* getLightSpaceMatrix() const { return staticLightSpaceMatrix_; }
    int getShadowMapSize() const { return shadowMapSize_; }
    bool shadowsEnabled_;

    // Shadow debug overlay: the Vulkan CSM near-cascade view (the GL shadow maps are gone).
    bool showShadowDebug_;        // master toggle for debug overlay
    void drawShadowDebugOverlay();

    // Shadow tuning — live-adjustable via ImGui; defaults match former hardcoded values.
    // Applied in beginShadowPass / beginShadowPassNoClear (static shadow pass only).
    float shadowBiasFactor_ = 2.0f;   // glPolygonOffset factor
    float shadowBiasUnits_  = 4.0f;   // glPolygonOffset units

    // Static world-fixed shadows: accumulate over multiple frames
    void buildStaticLightMatrix(float sunDirX, float sunDirY, float sunDirZ,
                                float mapHalfExtent);
    bool staticLightMatrixBuilt() const { return staticLightMatrixBuilt_; }
    // VK-SHADOW-COPY-ONCE-1: bump a generation counter each time the static
    // shadow map is (re)built (mission load / Terrain::destroy re-arm). The VK
    // interop copy path uses this to copy-once per generation instead of every
    // frame -- the static map only changes on this event, never per-frame.
    void markStaticLightMatrixBuilt() { staticLightMatrixBuilt_ = true; ++staticShadowGeneration_; }
    uint64_t staticShadowGeneration() const { return staticShadowGeneration_; }
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
    // The dynamic shadow state exists (the sizes and light matrices; the cascades when ShadowCSM is on). The maps themselves are Vulkan images owned by the shadow island.
    bool dynShadowBuilt() const { return dynShadowBuilt_; }
    bool csmBuilt() const { return csmBuilt_; }
    // The single (legacy) dynamic light-space matrix; the cascade matrices are getDynamicCascadeMatrices().
    const float* getDynamicLightSpaceMatrix() const { return dynamicLightSpaceMatrix_; }
    int getDynamicShadowMapSize() const { return dynShadowMapSize_; }
    // VK-P8a SHADOW-INTEROP: the dynamic prop caster draw enables polygon offset via
    // glPolygonOffset(shadowBiasFactor_, shadowBiasUnits_) (gos_static_prop_batcher.cpp
    // drawDynamicPropShadows). The VK shadow island must reproduce this as VK depthBias
    // EXACTLY (the parity STOP-watch), so it needs these values.
    float getShadowBiasFactor() const { return shadowBiasFactor_; }
    float getShadowBiasUnits() const { return shadowBiasUnits_; }

    // --- Item 1: Cascaded Shadow Maps (dynamic path) -----------------------
    // Option ShadowCSM (options.cfg, default on; off selects the legacy
    // single-map path above, byte-identical). When on
    // (the default), the dynamic shadow is an array of csmCount layers, each
    // with its own light matrix.
    int    getDynamicShadowCascadeCount() const { return csmCount_; }
    // Flat float[csmCount_*16] of per-cascade light-space matrices (col-major).
    const float* getDynamicCascadeMatrices() const { return dynamicCascadeMatrices_; }
    // Per-cascade world-texel footprint (2*cRad/mapSize), used for texel-scaled
    // depth bias in the shadow shaders. Same indexing as the cascade matrices.
    const float* getDynamicCascadeTexelWorld() const { return dynamicCascadeTexelWorld_; }
    // (farP-nearP) of the shared CSM ortho z-row; must match the C++ z-row exactly.
    float getCsmDepthSpan() const { return csmDepthSpan_; }
    // Per-cascade shadow resolution: the LAST cascade lives in a separate 2D
    // depth image, the near cascades stay in the array.
    int    getDynamicFullMapSize() const { return dynamicFullMapSize_; }
    // World-texel footprint of the last (full-map) cascade, computed with the
    // separate texture's resolution (NOT the array's). Used for the depth bias.
    float  getDynamicFullMapTexelWorld() const { return dynamicFullMapTexelWorld_; }

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
    // LDR color; off -> u_casEnabled==0 -> shader skips it.
    // Set from options.cfg (PostCAS / PostCASSharpness) through gos_SetPostFxOptions().
    bool  casEnabled_;
    float casSharpness_;  // 0..1, default 0.5

    // POST-FX-TONEMAP-V2: 0 off (default), 1 AgX, 2 Hable, 3 Reinhard; options.cfg TonemapMode.
    int   tonemapMode_;

    bool screenShadowEnabled_;
    int screenShadowDebug_;  // 0=normal, 1=visualize

    // Cloud shadows — single fullscreen multiplicative pass (replaces the four
    // inline cloud blocks). Default-ON to preserve legacy look; tunable live.
    bool  enableCloudShadow_;   // seeded from mc2CloudShadowEnabled()
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
    // TERRAIN-SHORELINE-MASK-1 (recon landmine #6 "double-shore"): set every
    // frame by the terrain chunk driver (gos_terrain_lod_chunk.cpp) to true
    // when a terrain-side wet/foam shoreline mask is active (gate ON + sidecar
    // loaded). The native shoreline pass yields when this is true so the screen-space
    // foam halo and the new terrain-side band never brighten the seam twice.
    bool shorelineSuppressedByTerrainMask_ = false;
    void setShorelineSuppressedByTerrainMask(bool b) { shorelineSuppressedByTerrainMask_ = b; }

    // EDGE-FOG-1: world-space map-edge fog on geometry pixels.
    // Fades terrain/props/mechs near the map boundary into the cloud color.
    // Default ON (MC2_EDGE_FOG=0 to disable).
    bool  edgeFogEnabled_  = false;
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

#ifdef MC2_VULKAN_ISLAND
    // Native scene image owners, rebuilt together on resize. Their registry
    // entries are the only images the islands and explicit readbacks consume.
    Mc2SharedSceneDepth* sharedSceneDepth_ = nullptr;
    Mc2SharedSceneColor* sharedSceneColor_ = nullptr;
    Mc2SharedSceneColor* sharedSceneNormal_ = nullptr;
    Mc2SharedSceneColor* sharedSceneObjectId_ = nullptr;

    // VK-POST-FOG-NATIVE-1 (rung 8.1, .claude/VK-RUNG8-LADDER-1.md): the fog pair
    // (edge + oob) rendered NATIVELY on the shared engine VulkanContext, sampling
    // the SHARED scene depth and blending into the SHARED scene color0 IN PLACE —
    // zero copies, zero CPU readbacks, no private device (supersedes the subgraph
    // when its gate is on; highest precedence at the fog site). Gate
    // umbrella-armed (mc2vkgate::umbrellaOn()). runVkPostFogNative()
    // returns true only when BOTH fogs rendered via VK (caller skips BOTH GL sites);
    // any not-ready condition returns false -> unchanged subgraph/island/GL chain.
    // destroyVkPostFogNative(): torn down from destroyFBOs() (resize; the shared
    // GBuffer it borrows is rebuilt) and destroy(). Defined in
    // vulkan_post_fog_native.cpp (same #ifdef MC2_VULKAN_ISLAND discipline).
    bool  runVkPostFogNative();
    void  destroyVkPostFogNative();
    struct VkPostFogNative;             // defined in vulkan_post_fog_native.cpp
    // Owner of the state: releases through destroy_state (idles the GPU first), defined next to the state.
    struct VkPostFogNativeDeleter { void operator()(VkPostFogNative*) const; };
    std::unique_ptr<VkPostFogNative, VkPostFogNativeDeleter> vkPostFogNative_;

    // VK-POST-SCREENFX-NATIVE-1 (rung 8.3, .claude/VK-RUNG8-LADDER-1.md):
    // cloud-shadow + shoreline-foam rendered NATIVELY on the shared engine
    // VulkanContext (one render pass / one submit, adjacent GL passes), sampling
    // the SHARED scene depth (cloud) + normal (shoreline) and MULTIPLYING into
    // the SHARED scene color0 IN PLACE -- zero copies, zero CPU readbacks, no
    // private device. Screen-shadow is NOT ported (its CSM cascade array has no
    // zero-copy VK producer until ladder rung 2 -- see the TU header). Gate
    // MC2_VK_POST_SCREENFX (a kill switch, on under the VK umbrella; =0 turns the VK passes off) + per-pass bisect
    // sub-gates MC2_VK_POST_SCREENFX_CLOUD / _SHORELINE (explicit 0 forces that
    // pass to GL). runVkPostScreenFxNative() returns a bitmask (bit0=cloud,
    // bit1=shoreline rendered via VK) -- the caller skips EXACTLY those GL pass
    // bodies; 0 -> the unchanged GL chain runs (fail-soft).
    // destroyVkPostScreenFxNative(): torn down from destroyFBOs() (resize; the
    // shared GBuffer it borrows is rebuilt) and destroy(). Defined in
    // vulkan_post_screenfx_native.cpp (same #ifdef MC2_VULKAN_ISLAND discipline).
    bool      vkPostScreenFxNativeEnabled();
    bool      runVkPostScreenShadowStaticNative();
    unsigned  runVkPostScreenFxNative();
    void      destroyVkPostScreenFxNative();
    struct VkPostScreenFx;              // defined in vulkan_post_screenfx_native.cpp
    // Owner of the state: releases through destroy_state (idles the GPU first), defined next to the state.
    struct VkPostScreenFxDeleter { void operator()(VkPostScreenFx*) const; };
    std::unique_ptr<VkPostScreenFx, VkPostScreenFxDeleter> vkPostScreenFx_;

    bool  runVkPostHzbNative(const Mc2ColorPropFeed& feed, uint32_t generation, void** maskBuffer);
    void  destroyVkPostHzbNative();
    struct VkPostHzbNative;             // defined in vulkan_post_hzb_native.cpp
    // Owner of the state: releases through destroy_state (idles the GPU first), defined next to the state.
    struct VkPostHzbNativeDeleter { void operator()(VkPostHzbNative*) const; };
    std::unique_ptr<VkPostHzbNative, VkPostHzbNativeDeleter> vkPostHzbNative_;


#endif

    // OOB-FOG-1: fullscreen fog over out-of-bounds far-plane pixels.
    // Default ON (MC2_OOB_FOG=0 to disable). Reads only scene depth —
    // no sceneColorTex_ feedback loop; blends SRC_ALPHA over scene color.
    bool  fogOobEnabled_ = false;
    // FOG-EXPOSURE-HEADROOM-1: see edgeFogColor_ above. fog_oob.frag's noise
    // "sunlit tops" (col = fogColor*1.05, line 101) clip FIRST under exposure,
    // which is what read as bright *spots*. 0.80 base * 1.05(top) * 1.1(exp) *
    // 1.10(grade) ~= 1.01 -- tops stay bright, no saturated white blob.
    float oobFogColor_[3] = {0.80f, 0.81f, 0.83f}; // bright cloud bank (exposure-headroom)
    float oobFogOpacity_ = 1.0f;

    // FOG-HORIZON-CLAMP-1: reshape the OOB/edge-fog elevation profile so fog is
    // FULL at/below the horizon (covers OOB terrain/water sideways + down) and
    // fades to ZERO within a small elevation band just above the horizon, leaving
    // the sky above clear. Supersedes the old worldDir.z 0.22 exclusion band
    // (which let fog survive to ~12deg+ elevation, bleeding up into the sky).
    //   fogHorizonFadeStartDeg_ -- elevation (deg) where the fade begins; fog
    //                              still full at/below this (0).
    //   fogHorizonFadeEndDeg_   -- elevation (deg) where fog reaches zero (5).
    // Shader form is cheap: we pass the SINES of the two angles (elevSin =
    // -worldDir.z for a normalized dir), so the frag does a plain smoothstep on
    // -worldDir.z with no per-fragment trig.
    bool  fogHorizonClampEnabled_ = true;    // always on (the uniform the fog shaders read)
    float fogHorizonFadeStartDeg_ = 0.0f;
    float fogHorizonFadeEndDeg_   = 5.0f;
    float fogHorizonFadeStartSin_ = 0.0f;    // sin(0deg)
    float fogHorizonFadeEndSin_   = 0.08716f;// sin(5deg); recomputed from env in ctor

    // Vulkan render-only culling; MC2_HZB_BUILD=0 keeps every color instance.
    bool   hzbEnabled_ = true;
    int    getHzbMipCount() const { return hzbMipCount_; }
    int    getHzbWidth()    const { return hzbW_; }
    int    getHzbHeight()   const { return hzbH_; }
    bool   isHzbEnabled()   const { return hzbEnabled_; }
    unsigned long long getHzbBuildCount() const { return hzbBuildCount_; }

    // LOWLIGHT-NIGHTVISION-MVP-1: pure-postprocess night-vision tunables.
    // Active only when the selected ViewMode is LowLight (5); the composite
    // branch is otherwise skipped (Visual byte-identical). ImGui- and
    // profile-adjustable.
    float lowLightGain_    = 2.5f;                 // luminance amplification
    float lowLightTint_[3] = { 0.7f, 1.0f, 0.6f }; // green-phosphor NV tint

    void setInverseViewProj(const float* m) { memcpy(inverseViewProj_, m, 16 * sizeof(float)); }
    void setViewProj(const float* m) { memcpy(viewProj_, m, 16 * sizeof(float)); }
    const float* getInverseViewProj() const { return inverseViewProj_; }
    const float* getViewProj() const { return viewProj_; }
    int getWidth()  const { return width_; }
    int getHeight() const { return height_; }

private:
    void createSceneTargets(int w, int h);
    void destroySceneTargets();

    // HDRI-SKY-1: background sky from a single equirect HDRI.
    // All HDRI state is null/false when MC2_HDRI_SKY=0 or load fails.
    // The retained HDRI source bytes the Vulkan sky and water islands upload; kept only while the Vulkan umbrella is armed.
    std::shared_ptr<struct HdriCpuImage> hdriCpu_;
    uint64_t      hdriCpuGeneration_ = 0;
    void          loadHdri(const char* path, float* outSunAz, bool* outSunValid);   // sets hdriCpu_ and its generation
    bool          hdriEnabled_     = false;  // resolved once from env at init
    bool          hdriReady_       = false;  // HDRI source is available to Vulkan
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

    // Shadow map (size only: the maps are Vulkan images)
    int shadowMapSize_;
    int savedViewport_[4];
    float staticLightSpaceMatrix_[16]; // world-fixed ortho, built once at map load
    bool staticLightMatrixBuilt_;      // true after light matrix is built (first frame)
    uint64_t staticShadowGeneration_ = 0; // VK-SHADOW-COPY-ONCE-1: bumped each (re)build
    float mapHalfExtent_;              // half the map size in world units
    float waterElevation_ = 0.0f;     // sea-level world Z (Terrain::waterElevation)

    // Dynamic object shadows (camera-centered, per-frame): size and matrices only
    int dynShadowMapSize_;
    bool dynShadowBuilt_ = false;
    bool csmBuilt_ = false;
    float dynamicLightSpaceMatrix_[16];

    // --- Item 1 CSM state (only used when the ShadowCSM option is on) --------
    static const int kMaxCsmCascades = 3;
    int    csmCount_;                          // clamp(MC2_SHADOW_CSM_COUNT,1,3)
    float  dynamicCascadeMatrices_[kMaxCsmCascades * 16]; // per-cascade col-major
    float  dynamicCascadeTexelWorld_[kMaxCsmCascades];    // 2*cRad/mapSize per cascade, world units
    float  csmDepthSpan_;                                 // (farP-nearP) of shared ortho z-row
    int    csmDebugLayer_;                     // which layer the debug blit shows

    // --- Per-cascade shadow resolution: separate full-map (last) cascade -----
    // The near cascades (0..N-2) are array layers at dynShadowMapSize_. The LAST
    // cascade (N-1, map-centered full-map catch-all) is a separate, lower-res 2D
    // depth image to save VRAM while keeping the near cascades sharp. When
    // MC2_SHADOW_FULLMAP_SEPARATE=0, dynamicFullMapSize_ == dynShadowMapSize_.
    int    dynamicFullMapSize_;                 // edge in texels (default 4096)
    float  dynamicFullMapTexelWorld_;           // 2*cRad/dynamicFullMapSize_ for last cascade

    float inverseViewProj_[16];
    float viewProj_[16];

    int           hzbW_          = 0;
    int           hzbH_          = 0;
    int           hzbMipCount_   = 0;
    unsigned long long hzbBuildCount_ = 0;  // frames the pyramid has been built
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


// HDRI-SKY-NUMBER-1: notify the postprocessor of the mission sky number so it
// can swap to the appropriate HDRI asset (IblHdriRegistry).
// Call at mission load after the mission sky number is known.
// No-op when HDRI is disabled or skyNumber is out of range (1-21).
void gos_SetSkyNumber(int skyNumber);

// Player post-FX options from options.cfg (CPrefs::applyPrefs): CAS on/off, CAS sharpness 0..1, tonemap mode
// 0 off / 1 AgX / 2 Hable / 3 Reinhard. Safe before the post-processor exists: the values are kept and read at its init.
void gos_SetPostFxOptions(int casEnabled, float casSharpness, int tonemapMode);

#endif // GOS_POSTPROCESS_H
