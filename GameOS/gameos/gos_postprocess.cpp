#include "gos_postprocess.h"
#include "utils/shader_builder.h"
#include "utils/gl_utils.h"
#include "utils/vec.h"
#include "gos_hdri.h"
#include "../../RenderCore/IblHdriRegistry.h"  // HDRI-SKY-NUMBER-1
#include "gos_profiler.h"
#include "gos_validate.h"  // drainGLErrors (Tier-1 instr §4)
#include "gos_smoke.h"     // S9E: SmokeMode fixed deterministic render-shader clock
#include "gameos.hpp"      // gos_InvalidateRenderStateCache (RENDER_STATES v1)
#include "../../RenderWorld/RenderWorld.h"  // M1.5: IsObjectIdBufferEnabled
#include "../../RenderCore/RenderResourceRegistry.h"
#include "../../RenderCore/fbo_ledger.h"   // FRAME-GRAPH-FBO-LEDGER-1: GLuint->logical target
#include "../../RenderCore/EngineView.h"
#include "view_uniforms_gl.h"
#include "gos_static_prop_registry.h"   // HZB-STATICPROP-CULL-RECON-1: real bounds for the probe
#include "gl_state_guard.h"  // GLSTATE-GUARD-ADOPTION-1: GlScopedTextureUnit (composite tex-unit leak)
#include "../../RenderCore/PipelineRegistry.h"  // POSTPROCESS-COMPOSITE-REGISTRATION-1
#include "pipeline_binder.h"                     // applyPipeline — composite FF state
#include "GLBackend.h"                           // RENDER-BACKEND-IFACE-POSTPROCESS-1: getGLBackend()
#include "render_frame_plan.h"                   // RENDER-FRAME-PLAN-SCAFFOLD-1
#include "gos_cluster_depth_pyramid.h"  // CLUSTER-DEPTH-PYRAMID-NATIVE-1 (gated substrate)
#include "gos_lightgrid_build.h"         // MC2-LIGHTGRID-BUILD-NATIVE-1 (gated, inert)
#include "gos_postprocess_blur.h"        // POSTPROCESS-COMPUTE-BLUR-1 (gated substrate)
#include "../../mclib/dynamic_decal_ring.h"  // DECAL-INTEGRATE-1: ring-fed projected decals

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <cstdlib>
#include <SDL2/SDL.h>
#include "../../RenderCore/frame_executor.h"  // FRAME-GRAPH-EXECUTOR-ISLAND-1 IslandContract

// === Item 1: Cascaded Shadow Maps gate (dynamic shadow path) ==============
// MC2_SHADOW_CSM  : master gate, DEFAULT ON (reworked 2026-06-18, `8ff13a36`;
//                   see docs/tier1_env_vars.md "Dynamic CSM"). Set "=0" to opt
//                   out to the legacy single dynamic map path.
// MC2_SHADOW_CSM_COUNT  : cascade count, default 3, clamp [1,3].
// MC2_SHADOW_CSM_LAMBDA : log/uniform split blend, default 0.5, clamp [0,1].
bool mc2ShadowCsmEnabled()
{
    static const bool s_on = []() {
        const char* v = getenv("MC2_SHADOW_CSM");
        return !(v && v[0] == '0' && v[1] == '\0');   // DEFAULT ON; only "0" disables
    }();
    return s_on;
}

// MC2_SHADER_PATH_TINT: debug-only solid-color-per-shader-path tint. DEFAULT OFF.
// 1 = each candidate ground shader returns its signature solid color early.
int mc2ShaderPathTint()
{
    static const int s_v = []() {
        const char* v = getenv("MC2_SHADER_PATH_TINT");
        return (v && v[0]) ? atoi(v) : 0;   // DEFAULT 0 (off, byte-identical)
    }();
    return s_v;
}

int mc2ShadowCsmCount()
{
    static const int s_n = []() {
        const char* v = getenv("MC2_SHADOW_CSM_COUNT");
        int n = (v && v[0]) ? atoi(v) : 3;
        if (n < 1) n = 1;
        if (n > 3) n = 3;
        return n;
    }();
    return s_n;
}

float mc2ShadowCsmLambda()
{
    static const float s_l = []() {
        const char* v = getenv("MC2_SHADOW_CSM_LAMBDA");
        float l = (v && v[0]) ? (float)atof(v) : 0.5f;
        if (l < 0.0f) l = 0.0f;
        if (l > 1.0f) l = 1.0f;
        return l;
    }();
    return s_l;
}

// MC2_SHADOW_MAP_SIZE : dynamic shadow map edge in texels. Default 8192.
// Clamp {2048,4096,8192}. VRAM (CSM 3 layers depth24): 4096=~201MB, 8192=~805MB.
int mc2ShadowMapSize()
{
    static const int s_sz = []() {
        const char* v = getenv("MC2_SHADOW_MAP_SIZE");
        int s = (v && v[0]) ? atoi(v) : 8192;
        if (s <= 2048) return 2048;
        if (s <= 4096) return 4096;
        return 8192;
    }();
    return s_sz;
}

// Per-cascade shadow resolution: separate full-map (last) cascade.
// MC2_SHADOW_FULLMAP_SEPARATE (default ON): render the last (map-centered
// full-map catch-all) cascade into a separate lower-res 2D depth texture so the
// high-res near-cascade array can be (csmCount-1) layers instead of csmCount.
// When OFF, dynamicFullMapSize_ is forced == dynShadowMapSize_ (equivalent VRAM
// to the old all-in-array layout; the texture still exists, just full-res).
bool mc2ShadowFullMapSeparate()
{
    static const bool s_on = []() {
        const char* v = getenv("MC2_SHADOW_FULLMAP_SEPARATE");
        if (!v || !v[0]) return true;          // DEFAULT ON
        return !(v[0] == '0' && v[1] == '\0'); // only "0" disables
    }();
    return s_on;
}

// MC2_SHADOW_FULLMAP_SIZE : edge of the separate full-map cascade depth texture.
// Default 4096. Clamp {2048,4096,8192}. VRAM (depth24, 2D): 4096=~67MB, 8192=~268MB.
int mc2ShadowFullMapSize()
{
    static const int s_sz = []() {
        const char* v = getenv("MC2_SHADOW_FULLMAP_SIZE");
        int s = (v && v[0]) ? atoi(v) : 4096;
        if (s <= 2048) return 2048;
        if (s <= 4096) return 4096;
        return 8192;
    }();
    return s_sz;
}

// CSM-REDESIGN: fixed near/mid cascade radii (WU), texel-snapped, frame-stable
// (kills frustum-refit popping). Last cascade is full-map (map-centered).
float mc2ShadowCsmR0()
{
    static const float s = []() {
        const char* v = getenv("MC2_SHADOW_CSM_R0");
        float r = (v && v[0]) ? (float)atof(v) : 512.0f;
        return (r < 64.0f) ? 64.0f : r;
    }();
    return s;
}
float mc2ShadowCsmR1()
{
    static const float s = []() {
        const char* v = getenv("MC2_SHADOW_CSM_R1");
        float r = (v && v[0]) ? (float)atof(v) : 4096.0f;
        return (r < 128.0f) ? 128.0f : r;
    }();
    return s;
}

// MC2_SHADOW_CSM_SOFTNESS : PCF penumbra radius in texels (pre-adaptiveScale).
// Default 0.9. Lower = sharper mech/terrain shadows; higher = softer. Clamp [0.1, 6.0].
float mc2ShadowCsmSoftness()
{
    static const float s_s = []() {
        const char* v = getenv("MC2_SHADOW_CSM_SOFTNESS");
        float s = (v && v[0]) ? (float)atof(v) : 0.9f;
        if (s < 0.1f) s = 0.1f;
        if (s > 6.0f) s = 6.0f;
        return s;
    }();
    return s_s;
}

// Object shadow-receive normal-offset bias (in cascade texels). Default 2.0,
// clamped [0,16]. 0 disables the normal-offset (A/B). Slope-scale term in the
// shader is always-on (tiny); this only gates the world-space normal-offset.
float mc2ShadowObjNormalBias()
{
    static const float s_b = []() {
        const char* v = getenv("MC2_SHADOW_OBJ_NORMAL_BIAS");
        float b = (v && v[0]) ? (float)atof(v) : 2.0f;
        if (b < 0.0f) b = 0.0f;
        if (b > 16.0f) b = 16.0f;
        return b;
    }();
    return s_b;
}

// Mech self-shadow softening factor. MC2_SHADOW_MECH_SOFT, default 1.0,
// clamped [0,4]. Scales the extra PCF penumbra radius + terminator smoothstep
// width applied to MECH pixels only (a<0.1). 0 = no extra softening (legacy
// hard mech self-shadow); >1 = blurrier/softer terminator.
float mc2ShadowMechSoft()
{
    static const float s_s = []() {
        const char* v = getenv("MC2_SHADOW_MECH_SOFT");
        float s = (v && v[0]) ? (float)atof(v) : 1.0f;
        if (s < 0.0f) s = 0.0f;
        if (s > 4.0f) s = 4.0f;
        return s;
    }();
    return s_s;
}

// PROP-SHADOW-RECEIVE-1: static props RECEIVE the dynamic CSM cascade shadow
// (building self-shadow + prop-on-prop) in shadow_screen.frag instead of the
// static map only. MC2_PROP_SHADOW_RECEIVE, DEFAULT OFF; =1 enables. OFF =
// legacy: the GBuffer1.a=0.25 prop class skips sampleDynamicShadow entirely
// (uniform uploads 0 -> shader branch unreachable -> pixel-identical).
int mc2PropShadowReceive()
{
    static const int s_v = []() {
        const char* v = getenv("MC2_PROP_SHADOW_RECEIVE");
        return (v && v[0]) ? atoi(v) : 0;   // DEFAULT 0 (off)
    }();
    return s_v;
}

// Cloud-shadow master gate. MC2_CLOUD_SHADOW, DEFAULT ON (preserves the legacy
// inline-cloud behavior that this fullscreen pass replaces). =0 disables the
// whole pass (and the C++ early-return skips it entirely).
bool mc2CloudShadowEnabled()
{
    static const bool s_on = []() {
        const char* v = getenv("MC2_CLOUD_SHADOW");
        if (!v || !v[0]) return false;           // default OFF
        return !(v[0] == '0' && v[1] == '\0');
    }();
    return s_on;
}

namespace {

// M1.5 C1 fix + M3 plan-review fix: centralized scene-FBO draw-buffer
// policy. Every site that calls glDrawBuffers against sceneFBO_ routes
// through this helper. The caller passes objectIdAttachmentReady so the
// helper does not have to guess whether sceneObjectIdTex_ has been
// allocated yet (avoids GL_INVALID_VALUE when env-ON but FBO setup
// hasn't run). Callers pass `sceneObjectIdTex_ != 0` for MRT sites;
// SingleColor sites pass false.
//
// glClearBufferuiv(GL_COLOR, 2, ...) at frame-entry is ONLY safe
// after setSceneDrawBuffers(MainSceneMRT, true) has bound the 3-entry list.
//
// Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md sec 3.
enum class SceneDrawBufferMode { MainSceneMRT, SingleColor };

static void setSceneDrawBuffers(SceneDrawBufferMode mode,
                                bool objectIdAttachmentReady) {
    const bool oid =
        RenderWorld::IsObjectIdBufferEnabled() && objectIdAttachmentReady;

    if (mode == SceneDrawBufferMode::SingleColor) {
        GLenum bufs[1] = { GL_COLOR_ATTACHMENT0 };
        glDrawBuffers(1, bufs);
        // DRAWBUFFER-OWNERSHIP-1: self-report draw-set transitions at the single
        // chokepoint so the frame trace shows which set is live per pass (catches a
        // pass running under the wrong inherited set). Gated by the frame-plan gate.
        if (render_frame_plan::traceEnabled()) {
            std::fprintf(stderr, "[DRAWBUF] set=SingleColor0 attachments={0}\n");
            std::fflush(stderr);
        }
        return;
    }

    if (oid) {
        GLenum bufs[3] = {
            GL_COLOR_ATTACHMENT0,
            GL_COLOR_ATTACHMENT1,
            GL_COLOR_ATTACHMENT2
        };
        glDrawBuffers(3, bufs);
    } else {
        GLenum bufs[2] = {
            GL_COLOR_ATTACHMENT0,
            GL_COLOR_ATTACHMENT1
        };
        glDrawBuffers(2, bufs);
    }
    if (render_frame_plan::traceEnabled()) {
        std::fprintf(stderr, "[DRAWBUF] set=MainSceneMRT attachments={0,1%s}\n",
                     oid ? ",2" : "");
        std::fflush(stderr);
    }
}

} // namespace

// SKYBOX-FOG-EXCLUDE-1: gate helper (read once).
// SKYBOX-FOG-EXCLUDE-DEFAULT-ON-1: flipped default OFF->ON. The gate is inert
// on non-HDRI missions -- renderHdriSkybox() early-returns before the stencil-
// tag pass ("black sky baseline"), so no stencil is ever tagged, the fog frags'
// stencil branch is a no-op, and fog_oob.frag's worldDir.z<-0.22 band fallback
// is retained (byte-identical to legacy there). On HDRI missions it excludes
// fog from true-sky pixels so the skybox is visible instead of being painted
// over by the OOB cloud bank (the user-reported "can't see the skybox").
// Kill-switch preserved: MC2_SKYBOX_FOG_EXCLUDE=0 restores the legacy OFF path.
static bool skyboxFogExcludeEnabled()
{
    static const bool s_on = []() {
        const char* v = ::getenv("MC2_SKYBOX_FOG_EXCLUDE");
        return !(v && v[0] == '0');   // default ON; explicit "0" disables.
    }();
    return s_on;
}

static gosPostProcess* s_postProcess = nullptr;

gosPostProcess* getGosPostProcess()
{
    return s_postProcess;
}

// VIEWMODE-POSTPROCESS-PRESENTATION-1: module-level selected view mode.
// 0 = Visual (default, byte-identical), 1 = ObjectIdDebug.
// Written by ImGui combo (gated MC2_VIEWMODE_FRAMEWORK); read in endScene().
// Pattern mirrors gos_SetExposure/gos_GetExposure.
static bool s_viewmodeFrameworkEnabled = false;
static int  s_selectedViewMode         = 0;  // ViewMode::Visual

bool  gos_IsViewmodeFrameworkEnabled() { return s_viewmodeFrameworkEnabled; }
int   gos_GetSelectedViewMode()        { return s_viewmodeFrameworkEnabled ? s_selectedViewMode : 0; }
void  gos_SetSelectedViewMode(int m)   { s_selectedViewMode = (m < 0 ? 0 : (m > 5 ? 5 : m)); } // clamp to RenderCore::ViewMode range

float gos_GetExposure() { return s_postProcess ? s_postProcess->exposure_ : 1.0f; }
void  gos_SetExposure(float v) { if (s_postProcess) s_postProcess->exposure_ = (v < 0.0f ? 0.0f : v); }

// LOWLIGHT-NIGHTVISION-MVP-1 tunables (clamped to conservative ranges).
void  gos_SetLowLightGain(float v)   { if (s_postProcess) s_postProcess->lowLightGain_ = (v < 0.1f ? 0.1f : (v > 16.0f ? 16.0f : v)); }
float gos_GetLowLightGain()          { return s_postProcess ? s_postProcess->lowLightGain_ : 2.5f; }
void  gos_SetLowLightTintG(float v)  { if (s_postProcess) s_postProcess->lowLightTint_[1] = (v < 0.0f ? 0.0f : (v > 2.0f ? 2.0f : v)); }

// SSAO-GTAO-LITE-MVP-1 tunables (clamped to conservative ranges).
bool  gos_IsSsaoEnabled() { return s_postProcess && s_postProcess->ssaoEnabled_; }
void  gos_SetSsaoRadius(float v)   { if (s_postProcess) s_postProcess->aoRadius_   = (v < 0.1f ? 0.1f : (v > 64.0f ? 64.0f : v)); }
void  gos_SetSsaoStrength(float v) { if (s_postProcess) s_postProcess->aoStrength_ = (v < 0.0f ? 0.0f : (v > 2.0f  ? 2.0f  : v)); }
void  gos_SetSsaoBias(float v)     { if (s_postProcess) s_postProcess->aoBias_     = (v < 0.0f ? 0.0f : (v > 0.1f  ? 0.1f  : v)); }
float gos_GetSsaoRadius()   { return s_postProcess ? s_postProcess->aoRadius_   : 3.0f; }
float gos_GetSsaoStrength() { return s_postProcess ? s_postProcess->aoStrength_ : 0.7f; }
float gos_GetSsaoBias()     { return s_postProcess ? s_postProcess->aoBias_     : 0.0025f; }

// HDRI-SKY-NUMBER-1: swap the loaded HDRI texture to match theSkyNumber.
// Called at mission load (via GameAdapters::Sky::setSkyNumber -> gos_SetSkyNumber).
// Pattern: load is deferred to first mission load so the default init texture
// (DaySkyHDRI063B_4K) is overridden before the first frame draws the sky.
// GL context is valid at mission load time (game is running).
void gosPostProcess::setSkyNumber(int skyNumber)
{
    skyNumber_ = skyNumber;   // SCENE-LIGHTING-STATE-1: record requested sky # even if HDRI disabled
    if (!hdriEnabled_) return;
    if (skyNumber < 1 || skyNumber > 21) return;

    const RenderCore::IblHdriSet& hdriSet =
        RenderCore::lookupHdriForSkyNumber(skyNumber);
    const char* newPath = hdriSet.exrPath;

    // Skip reload if the path hasn't changed (same mission reloaded, etc.).
    if (std::strncmp(hdriCurrentPath_, newPath, sizeof(hdriCurrentPath_)) == 0)
        return;

    std::fprintf(stderr,
        "[HDRI_SKY v1] setSkyNumber sky=%d mood=%s path=%s\n",
        skyNumber, hdriSet.name, newPath);

    // Delete the old texture before loading the new one.
    if (hdriTex_) {
        glDeleteTextures(1, &hdriTex_);
        hdriTex_ = 0;
    }

    float newSunAz = 0.0f;
    bool  newSunValid = false;
    GLuint newTex = loadHdriTexture(newPath, &newSunAz, &newSunValid);

    if (newTex) {
        hdriTex_          = newTex;
        hdriBakedSunAz_   = newSunAz;
        hdriBakedSunValid_ = newSunValid;
        std::strncpy(hdriCurrentPath_, newPath, sizeof(hdriCurrentPath_) - 1);
        hdriCurrentPath_[sizeof(hdriCurrentPath_) - 1] = '\0';
        hdriReady_ = (hdriSkyboxProg_ != nullptr) && hdriSkyboxProg_->is_valid();
    } else {
        // Load failed — keep hdriReady_=false so sky renders black rather than crash.
        hdriReady_ = false;
        std::fprintf(stderr,
            "[HDRI_SKY v1] setSkyNumber sky=%d load_failed path=%s\n",
            skyNumber, newPath);
    }
}

// Free-function bridge: GameAdapters or profile code calls this without
// including the full gosPostProcess class.
void gos_SetSkyNumber(int skyNumber)
{
    if (s_postProcess) s_postProcess->setSkyNumber(skyNumber);
}

// SCENE-LIGHTING-STATE-1: read-only accessor for the current sky number.
int gos_GetSkyNumber()
{
    return s_postProcess ? s_postProcess->getSkyNumber() : -1;
}

// Fullscreen quad vertices: 2 triangles covering NDC [-1,1]
// Each vertex: pos.x, pos.y, uv.x, uv.y
static const float kQuadVerts[] = {
    // Triangle 1
    -1.0f, -1.0f,  0.0f, 0.0f,
    1.0f, -1.0f,  1.0f, 0.0f,
    1.0f,  1.0f,  1.0f, 1.0f,
    // Triangle 2
    -1.0f, -1.0f,  0.0f, 0.0f,
    1.0f,  1.0f,  1.0f, 1.0f,
    -1.0f,  1.0f,  0.0f, 1.0f,
};

gosPostProcess::gosPostProcess()
    : exposure_(1.0f)
    , sceneFBO_(0)
    , sceneColorTex_(0)
    , sceneDepthTex_(0)
    , sceneNormalTex_(0)
    , quadVAO_(0)
    , quadVBO_(0)
    , compositeProg_(nullptr)
    , skyboxProg_(nullptr)
    , width_(0)
    , height_(0)
    , initialized_(false)
    , shadowFBO_(0)
    , shadowDepthTex_(0)
    , shadowDummyColorTex_(0)
    , shadowDepthProg_(nullptr)
    , shadowMapSize_(4096)
    , shadowsEnabled_(true)
    , staticLightMatrixBuilt_(false)
    , mapHalfExtent_(0.0f)
    , dynShadowFBO_(0)
    , dynShadowDepthTex_(0)
    , dynShadowDummyColorTex_(0)
    , dynShadowMapSize_(2048)
    , shadowDebugProg_(nullptr)
    , dynShadowArrayFBO_(0)
    , dynShadowArrayDummyColorTex_(0)
    , dynShadowArrayTex_(0)
    , csmCount_(0)
    , csmActiveCascade_(-1)
    , csmDebugLayer_(0)
    , screenShadowProg_(nullptr)
    , screenShadowEnabled_(true)
    , screenShadowDebug_(0)
    , cloudProg_(nullptr)
    , enableCloudShadow_(mc2CloudShadowEnabled())
    , cloudStrength_(0.15f)
    , cloudScale_(0.0006f)
    , cloudScrollX_(0.012f)
    , cloudScrollY_(0.005f)
    , cloudThreshLo_(0.3f)
    , cloudThreshHi_(0.7f)
    , cloudOctaves_(4)
    , sceneHasTerrain_(false)
    , prevFrameHadTerrain_(false)
    , shorelineEnabled_(true)
    , shorelineProg_(nullptr)
    , ssaoEnabled_(false)
    , ssaoDebug_(0)
    , aoRadius_(3.0f)
    , aoStrength_(0.7f)
    , aoBias_(0.0025f)
    , aoPower_(1.5f)
{
    memset(staticLightSpaceMatrix_, 0, sizeof(staticLightSpaceMatrix_));
    memset(dynamicLightSpaceMatrix_, 0, sizeof(dynamicLightSpaceMatrix_));
    dynamicFullMapTex_ = 0;
    dynamicFullMapFbo_ = 0;
    dynamicFullMapDummyColorTex_ = 0;
    dynamicFullMapSize_ = 4096;
    dynamicFullMapTexelWorld_ = 0.0f;
    memset(dynamicCascadeMatrices_, 0, sizeof(dynamicCascadeMatrices_));
    memset(dynamicCascadeTexelWorld_, 0, sizeof(dynamicCascadeTexelWorld_));
    csmDepthSpan_ = 1.0f;
    memset(savedViewport_, 0, sizeof(savedViewport_));
    memset(inverseViewProj_, 0, sizeof(inverseViewProj_));
    memset(viewProj_, 0, sizeof(viewProj_));
    showShadowDebug_ = false;
    shadowDebugMode_ = 0;
}

gosPostProcess::~gosPostProcess()
{
    if (initialized_)
        destroy();
}

void gosPostProcess::init(int w, int h)
{
    ZoneScopedN("gosPostProcess::init");
    assert(!initialized_);

    width_ = w;
    height_ = h;

    // HZB-DEPTH-PYRAMID-MVP-1: resolve the build gate BEFORE createFBOs so the
    // pyramid texture is allocated in the same pass as the other scene targets.
    {
        const char* hzbEnv = getenv("MC2_HZB_BUILD");
        hzbEnabled_ = (hzbEnv && hzbEnv[0] && hzbEnv[0] != '0');
        std::fprintf(stderr, "[HZB_BUILD v1] enabled=%d (MC2_HZB_BUILD=%s)\n",
                     hzbEnabled_ ? 1 : 0, hzbEnv ? hzbEnv : "(unset)");
    }

    createFBOs(w, h);
    createFullscreenQuad();
    createBoxDecalCube();   // BT2018-BOX-DECAL-1

    // Load shaders — version provided via prefix (shader files must NOT have #version).
    // 4.3 matches the GL context requirement (SSBO + std430 used by the static-prop
    // renderer); using a lower version here worked on AMD but broke on NVIDIA which
    // defaults to GLSL 1.10 when the context/shader versions disagree at the boundary.
    static const char* kShaderPrefix = "#version 430\n";

    compositeProg_ = glsl_program::makeProgram(
        "postprocess",
        "shaders/postprocess.vert",
        "shaders/postprocess.frag",
        kShaderPrefix
    );

    if (!compositeProg_ || !compositeProg_->is_valid()) {
        fprintf(stderr, "gosPostProcess: failed to compile postprocess shader\n");
    }

    skyboxProg_ = glsl_program::makeProgram("skybox",
        "shaders/skybox.vert", "shaders/skybox.frag", kShaderPrefix);
    if (!skyboxProg_ || !skyboxProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile skybox shader\n");

    // HDRI-SKY-1 init. Gate read once; default enabled unless env var == "0".
    {
        const char* gateEnv = getenv("MC2_HDRI_SKY");
        hdriEnabled_ = !(gateEnv && gateEnv[0] == '0' && gateEnv[1] == '\0');

        if (hdriEnabled_) {
            // HDRI-SKY-NUMBER-1: default path (overridden at mission load via setSkyNumber).
            const char* hdrPath = "data/hdr/DaySkyHDRI063B_4K.exr";
            // Item 2: also scan for the baked sun azimuth (read-only; used only
            // when sun-sync is enabled at draw time).
            hdriTex_ = loadHdriTexture(hdrPath, &hdriBakedSunAz_, &hdriBakedSunValid_);  // logs failures internally
            if (hdriTex_) {
                std::strncpy(hdriCurrentPath_, hdrPath, sizeof(hdriCurrentPath_) - 1);
                hdriCurrentPath_[sizeof(hdriCurrentPath_) - 1] = '\0';
            }

            hdriSkyboxProg_ = glsl_program::makeProgram(
                "hdri_skybox",
                "shaders/hdri_skybox.vert",
                "shaders/hdri_skybox.frag",
                kShaderPrefix
            );

            hdriReady_ = (hdriTex_ != 0)
                      && (hdriSkyboxProg_ != nullptr)
                      && hdriSkyboxProg_->is_valid();

            if (!hdriReady_) {
                std::fprintf(stderr,
                    "[HDRI_SKY v1] enabled=0 reason=init_failed "
                    "tex=%u prog=%p valid=%d\n",
                    hdriTex_, (void*)hdriSkyboxProg_,
                    hdriSkyboxProg_ ? (int)hdriSkyboxProg_->is_valid() : 0);
            }
        } else {
            std::fprintf(stderr,
                "[HDRI_SKY v1] enabled=0 reason=env_gate MC2_HDRI_SKY=0\n");
        }
    }

    // Item 1 P5: inject MC2_SHADOW_CSM so the debug blit can sample an array layer.
    std::string shadowDebugPrefix = "#version 430\n";
    if (mc2ShadowCsmEnabled())
        shadowDebugPrefix += "#define MC2_SHADOW_CSM 1\n";
    shadowDebugProg_ = glsl_program::makeProgram("shadow_debug",
        "shaders/postprocess.vert", "shaders/shadow_debug.frag", shadowDebugPrefix.c_str());
    if (!shadowDebugProg_ || !shadowDebugProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile shadow_debug shader\n");

    // Item 1: shadow_screen.frag #includes shadow.hglsl -> inject MC2_SHADOW_CSM
    // when ON so it compiles the array-sampler dynamic-shadow variant.
    std::string screenShadowPrefix = "#version 430\n";
    if (mc2ShadowCsmEnabled()) {
        char csmDef[64];
        snprintf(csmDef, sizeof(csmDef),
                 "#define MC2_SHADOW_CSM 1\n#define MC2_SHADOW_CSM_MAX %d\n",
                 mc2ShadowCsmCount());
        screenShadowPrefix += csmDef;
    }
    screenShadowProg_ = glsl_program::makeProgram("shadow_screen",
        "shaders/postprocess.vert", "shaders/shadow_screen.frag", screenShadowPrefix.c_str());
    if (!screenShadowProg_ || !screenShadowProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile shadow_screen shader\n");

    cloudProg_ = glsl_program::makeProgram("cloud",
        "shaders/postprocess.vert", "shaders/cloud.frag", kShaderPrefix);
    if (!cloudProg_ || !cloudProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile cloud shader\n");

    shorelineProg_ = glsl_program::makeProgram("shoreline",
        "shaders/postprocess.vert", "shaders/shoreline.frag", kShaderPrefix);
    if (!shorelineProg_ || !shorelineProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile shoreline shader\n");

    // SSAO-GTAO-LITE-MVP-1 (Track V) shaders.
    ssaoProg_ = glsl_program::makeProgram("ssao",
        "shaders/postprocess.vert", "shaders/ssao.frag", kShaderPrefix);
    if (!ssaoProg_ || !ssaoProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile ssao shader\n");
    ssaoApplyProg_ = glsl_program::makeProgram("ssao_apply",
        "shaders/postprocess.vert", "shaders/ssao_apply.frag", kShaderPrefix);
    if (!ssaoApplyProg_ || !ssaoApplyProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile ssao_apply shader\n");

    // BT2018-BOX-DECAL-1: screen-space box-decal program. Its OWN vert (cube volume),
    // not postprocess.vert. Default-OFF gate resolved once from env (SSAO style).
    boxDecalProg_ = glsl_program::makeProgram("box_decal",
        "shaders/box_decal.vert", "shaders/box_decal.frag", kShaderPrefix);
    if (!boxDecalProg_ || !boxDecalProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile box_decal shader\n");
    {
        const char* e = getenv("MC2_BOX_DECAL");
        boxDecalEnabled_ = (e && e[0] && e[0] != '0');
        std::fprintf(stderr, "[BOX_DECAL v1] enabled=%d (MC2_BOX_DECAL=%s)\n",
                     boxDecalEnabled_ ? 1 : 0, e ? e : "(unset)");
        // DECAL-INTEGRATE-1: MC2_PROJECTED_DECALS (default OFF). When ON, the box-decal
        // pass is fed by the dynamic_decal_ring impact slots instead of the v1 test box.
        const char* pd = getenv("MC2_PROJECTED_DECALS");
        projectedDecalsEnabled_ = (pd && pd[0] && pd[0] != '0');
        std::fprintf(stderr, "[PROJECTED_DECALS] enabled=%d (MC2_PROJECTED_DECALS=%s)\n",
                     projectedDecalsEnabled_ ? 1 : 0, pd ? pd : "(unset)");
    }

    // SSAO gate + debug, resolved once from env. Default OFF -> runSSAO()
    // skipped entirely (byte-identical). aoRadius/strength/bias keep their
    // member defaults (ImGui + per-mission profile adjustable).
    {
        const char* ssaoEnv = getenv("MC2_SSAO");
        ssaoEnabled_ = (ssaoEnv && ssaoEnv[0] && ssaoEnv[0] != '0');
        const char* dbgEnv = getenv("MC2_SSAO_DEBUG");
        ssaoDebug_ = (dbgEnv && dbgEnv[0] && dbgEnv[0] != '0') ? 1 : 0;
        std::fprintf(stderr, "[SSAO v1] enabled=%d debug=%d (MC2_SSAO=%s) radius=%.2f strength=%.2f bias=%.4f\n",
                     ssaoEnabled_ ? 1 : 0, ssaoDebug_,
                     ssaoEnv ? ssaoEnv : "(unset)", aoRadius_, aoStrength_, aoBias_);
    }

    // POST-FX-FXAA-1: FXAA gate + tunables, resolved once from env. Default OFF
    // -> composite uploads u_fxaaEnabled=0 -> single tap (byte-identical).
    // Defaults = "sharper" preset (subpix 0.25 / edge 0.166 / edgeMin 0.0833).
    {
        const char* fxEnv = getenv("MC2_FXAA");
        fxaaEnabled_         = (fxEnv && fxEnv[0] && fxEnv[0] != '0');
        fxaaSubpix_          = 0.25f;
        fxaaEdgeThreshold_   = 0.166f;
        fxaaEdgeThresholdMin_= 0.0833f;
        if (const char* s = getenv("MC2_FXAA_SUBPIX"))            { float v = (float)atof(s); if (v >= 0.0f) fxaaSubpix_ = v; }
        if (const char* s = getenv("MC2_FXAA_EDGE_THRESHOLD"))    { float v = (float)atof(s); if (v >  0.0f) fxaaEdgeThreshold_ = v; }
        if (const char* s = getenv("MC2_FXAA_EDGE_THRESHOLD_MIN")){ float v = (float)atof(s); if (v >  0.0f) fxaaEdgeThresholdMin_ = v; }
        std::fprintf(stderr, "[FXAA v1] enabled=%d subpix=%.3f edge=%.3f edgeMin=%.4f (MC2_FXAA=%s)\n",
                     fxaaEnabled_ ? 1 : 0, fxaaSubpix_, fxaaEdgeThreshold_, fxaaEdgeThresholdMin_,
                     fxEnv ? fxEnv : "(unset)");
    }

    // POST-FX-CAS-1: AMD FidelityFX Contrast Adaptive Sharpening gate + sharpness,
    // resolved once from env. Default OFF -> composite uploads u_casEnabled=0 ->
    // CAS skipped in the shader (byte-identical). Sharpness clamped 0..1, default
    // 0.5. Applied to the final graded LDR color, after FXAA/grade, before UI.
    {
        const char* casEnv = getenv("MC2_POST_CAS");
        casEnabled_   = (casEnv && casEnv[0] && casEnv[0] != '0');
        casSharpness_ = 0.5f;
        if (const char* s = getenv("MC2_POST_CAS_SHARPNESS")) {
            float v = (float)atof(s);
            casSharpness_ = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
        }
        std::fprintf(stderr, "[CAS v1] enabled=%d sharpness=%.3f (MC2_POST_CAS=%s)\n",
                     casEnabled_ ? 1 : 0, casSharpness_,
                     casEnv ? casEnv : "(unset)");
    }

    // POST-FX-TONEMAP-V2 (SCOUR-1 #3): optional filmic tonemap curve on the final
    // composited colour. Resolved once from env. Default OFF (mode 0) -> composite
    // uploads u_tonemapMode=0 -> shader no-op branch -> byte-identical.
    // MC2_TONEMAP_V2 = 0/unset off, 1=AgX, 2=Hable, 3=Reinhard (also accepts the
    // names agx/hable/reinhard). Visual-only; no geometry/depth/object-id effect.
    {
        tonemapMode_ = 0;
        if (const char* v = getenv("MC2_TONEMAP_V2")) {
            if      (v[0]=='1' || !strcmp(v,"agx"))      tonemapMode_ = 1;
            else if (v[0]=='2' || !strcmp(v,"hable"))    tonemapMode_ = 2;
            else if (v[0]=='3' || !strcmp(v,"reinhard")) tonemapMode_ = 3;
        }
        std::fprintf(stderr, "[TONEMAP v2] mode=%d (MC2_TONEMAP_V2=%s)\n",
                     tonemapMode_, getenv("MC2_TONEMAP_V2") ? getenv("MC2_TONEMAP_V2") : "(unset)");
    }

    // OOB-FOG-1: far-plane fog over the out-of-bounds region. Default ON.
    {
        fogOobProg_ = glsl_program::makeProgram("fog_oob",
            "shaders/postprocess.vert", "shaders/fog_oob.frag", kShaderPrefix);
        if (!fogOobProg_ || !fogOobProg_->is_valid())
            std::fprintf(stderr, "gosPostProcess: failed to compile fog_oob shader\n");
        const char* oobEnv = getenv("MC2_OOB_FOG");
        fogOobEnabled_ = !(oobEnv && oobEnv[0] == '0' && oobEnv[1] == '\0')
                      && fogOobProg_ && fogOobProg_->is_valid();
        if (const char* colorEnv = getenv("MC2_OOB_FOG_COLOR")) {
            float r, g, b;
            if (std::sscanf(colorEnv, "%f,%f,%f", &r, &g, &b) == 3) {
                oobFogColor_[0] = r; oobFogColor_[1] = g; oobFogColor_[2] = b;
            }
        }
        std::fprintf(stderr, "[OOB_FOG v1] enabled=%d color=(%.2f,%.2f,%.2f)\n",
            fogOobEnabled_ ? 1 : 0, oobFogColor_[0], oobFogColor_[1], oobFogColor_[2]);
    }

    // EDGE-FOG-1: world-space map-edge fog on geometry pixels. Default ON.
    {
        edgeFogProg_ = glsl_program::makeProgram("edge_fog",
            "shaders/postprocess.vert", "shaders/edge_fog.frag", kShaderPrefix);
        if (!edgeFogProg_ || !edgeFogProg_->is_valid())
            std::fprintf(stderr, "gosPostProcess: failed to compile edge_fog shader\n");
        const char* efEnv = getenv("MC2_EDGE_FOG");
        edgeFogEnabled_ = !(efEnv && efEnv[0] == '0' && efEnv[1] == '\0')
                       && edgeFogProg_ && edgeFogProg_->is_valid();
        if (const char* v = getenv("MC2_EDGE_FOG_START"))  edgeFogStart_  = std::atof(v);
        if (const char* v = getenv("MC2_EDGE_FOG_HEIGHT")) edgeFogHeight_ = std::atof(v);
        if (const char* v = getenv("MC2_EDGE_FOG_MAX"))    edgeFogMax_    = std::atof(v);
        std::fprintf(stderr, "[EDGE_FOG v3] enabled=%d start=%.0f height=%.0f max=%.2f\n",
            edgeFogEnabled_ ? 1 : 0, edgeFogStart_, edgeFogHeight_, edgeFogMax_);
    }

    // FOG-HORIZON-CLAMP-1: elevation profile shared by OOB + edge fog. Rides the
    // existing (default-ON) fog gates -- this is a tuning of the fog's SHAPE, not
    // a new feature. Kill-switch MC2_FOG_HORIZON_CLAMP=0 restores the previous
    // worldDir.z 0.22 exclusion band. Knobs are in DEGREES of elevation above the
    // horizon; we precompute their sines here so the frag stays trig-free
    // (elevSin = -worldDir.z for a normalized view ray).
    {
        if (const char* v = getenv("MC2_FOG_HORIZON_CLAMP"))
            fogHorizonClampEnabled_ = !(v[0] == '0' && v[1] == '\0');
        if (const char* v = getenv("MC2_FOG_HORIZON_FADE_START"))
            fogHorizonFadeStartDeg_ = (float)std::atof(v);
        if (const char* v = getenv("MC2_FOG_HORIZON_FADE_END"))
            fogHorizonFadeEndDeg_   = (float)std::atof(v);
        // Guard the ordering so smoothstep(edge0<edge1) always holds; a degenerate
        // or inverted band would make the fade a hard step. Clamp to [-90,90].
        auto clampDeg = [](float d) { return d < -90.0f ? -90.0f : (d > 90.0f ? 90.0f : d); };
        fogHorizonFadeStartDeg_ = clampDeg(fogHorizonFadeStartDeg_);
        fogHorizonFadeEndDeg_   = clampDeg(fogHorizonFadeEndDeg_);
        if (fogHorizonFadeEndDeg_ <= fogHorizonFadeStartDeg_)
            fogHorizonFadeEndDeg_ = fogHorizonFadeStartDeg_ + 0.5f; // minimum band
        const float kDeg2Rad = 3.14159265358979323846f / 180.0f;
        fogHorizonFadeStartSin_ = std::sin(fogHorizonFadeStartDeg_ * kDeg2Rad);
        fogHorizonFadeEndSin_   = std::sin(fogHorizonFadeEndDeg_   * kDeg2Rad);
        std::fprintf(stderr,
            "[FOG_HORIZON v1] clamp=%d fadeStart=%.2fdeg(sin=%.4f) fadeEnd=%.2fdeg(sin=%.4f)\n",
            fogHorizonClampEnabled_ ? 1 : 0,
            fogHorizonFadeStartDeg_, fogHorizonFadeStartSin_,
            fogHorizonFadeEndDeg_,   fogHorizonFadeEndSin_);
    }

    // HZB-DEPTH-PYRAMID-MVP-1: reduction shader. Gate (hzbEnabled_) is resolved
    // earlier (before createFBOs); default OFF -> no allocation, no-op build.
    hzbReduceProg_ = glsl_program::makeProgram("hzb_reduce",
        "shaders/postprocess.vert", "shaders/hzb_reduce.frag", kShaderPrefix);
    if (!hzbReduceProg_ || !hzbReduceProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile hzb_reduce shader\n");
    {
        // HZB-OCCLUSION-PROBE-1 gate (diagnostic only; requires the build gate).
        const char* probeEnv = getenv("MC2_HZB_PROBE");
        hzbProbeEnabled_ = hzbEnabled_ &&
                           (probeEnv && probeEnv[0] && probeEnv[0] != '0');
        std::fprintf(stderr, "[HZB_PROBE v1] enabled=%d (MC2_HZB_PROBE=%s, requires MC2_HZB_BUILD)\n",
                     hzbProbeEnabled_ ? 1 : 0, probeEnv ? probeEnv : "(unset)");
    }

    // SHADOW-ENV-DEBUG-MODE-1: select shadow debug overlay from env var so
    // automated capture can request it without ImGui interaction.
    // Default unset/0/off -> showShadowDebug_ stays false (byte-identical).
    // RAlt+F2 hotkey and ImGui checkbox still override at runtime.
    {
        const char* sdEnv = getenv("MC2_SHADOW_DEBUG_MODE");
        if (sdEnv && sdEnv[0]) {
            if (sdEnv[0] == '1' || (sdEnv[0] == 's' && sdEnv[1] == 't')) {
                showShadowDebug_ = true;
                shadowDebugMode_ = 0;
                std::fprintf(stderr, "[SHADOW_DEBUG] MC2_SHADOW_DEBUG_MODE=static (mode 0)\n");
            } else if (sdEnv[0] == '2' || sdEnv[0] == 'd') {
                showShadowDebug_ = true;
                shadowDebugMode_ = 1;
                std::fprintf(stderr, "[SHADOW_DEBUG] MC2_SHADOW_DEBUG_MODE=dynamic (mode 1)\n");
            } else {
                std::fprintf(stderr, "[SHADOW_DEBUG] MC2_SHADOW_DEBUG_MODE=%s (unrecognized, using OFF)\n", sdEnv);
            }
        }
    }

    // VIEWMODE-POSTPROCESS-PRESENTATION-1: resolve MC2_VIEWMODE_FRAMEWORK once.
    // Default OFF -> u_viewMode forced 0, ImGui combo not rendered,
    // endScene() output byte-identical to baseline.
    {
        const char* vmEnv = getenv("MC2_VIEWMODE_FRAMEWORK");
        s_viewmodeFrameworkEnabled = (vmEnv && vmEnv[0] && vmEnv[0] != '0');

        // MC2_VIEW_MODE seeds the startup mode (numeric 0..5 matching
        // RenderCore::ViewMode, or a name) so headless capture can request a
        // mode without ImGui. Only meaningful when the framework gate is ON;
        // the ImGui combo overrides it live.
        const char* modeEnv = getenv("MC2_VIEW_MODE");
        int initialMode = 0;
        if (modeEnv && modeEnv[0]) {
            if (modeEnv[0] >= '0' && modeEnv[0] <= '9') initialMode = atoi(modeEnv);
            else if (!strcmp(modeEnv, "visual"))   initialMode = 0;
            else if (!strcmp(modeEnv, "objectid")) initialMode = 1;
            else if (!strcmp(modeEnv, "tactical")) initialMode = 2;
            else if (!strcmp(modeEnv, "thermal"))  initialMode = 3;
            else if (!strcmp(modeEnv, "infrared")) initialMode = 4;
            else if (!strcmp(modeEnv, "lowlight")) initialMode = 5;
        }
        gos_SetSelectedViewMode(initialMode);
        std::fprintf(stderr, "[VIEWMODE v1] framework=%d initialMode=%d (MC2_VIEWMODE_FRAMEWORK=%s MC2_VIEW_MODE=%s)\n",
                     s_viewmodeFrameworkEnabled ? 1 : 0, s_selectedViewMode,
                     vmEnv ? vmEnv : "(unset)", modeEnv ? modeEnv : "(unset)");

        // LOWLIGHT-NIGHTVISION-MVP-1: seed night-vision tunables from env.
        // Inert unless the selected ViewMode is LowLight (5). Direct member
        // writes — s_postProcess is not yet assigned during init(), so the
        // gos_SetLowLight* setters would no-op here.
        const char* gainEnv = getenv("MC2_VIEWMODE_LOWLIGHT_GAIN");
        if (gainEnv && gainEnv[0]) {
            float g = (float)atof(gainEnv);
            lowLightGain_ = (g < 0.1f ? 0.1f : (g > 16.0f ? 16.0f : g));
        }
        const char* tintEnv = getenv("MC2_VIEWMODE_LOWLIGHT_TINT");
        if (tintEnv && tintEnv[0])
            sscanf(tintEnv, "%f,%f,%f", &lowLightTint_[0], &lowLightTint_[1], &lowLightTint_[2]);
        std::fprintf(stderr, "[VIEWMODE_LOWLIGHT v1] gain=%.2f tint=(%.2f,%.2f,%.2f)\n",
                     lowLightGain_, lowLightTint_[0], lowLightTint_[1], lowLightTint_[2]);
    }

    initShadows();
    initDynamicShadows();

    s_postProcess = this;
    initialized_ = true;
}

void gosPostProcess::destroy()
{
    if (!initialized_)
        return;

    destroyFBOs();
    destroyFullscreenQuad();
    destroyBoxDecalCube();   // BT2018-BOX-DECAL-1

#ifdef MC2_VULKAN_ISLAND
    // VULKAN-EDGE-FOG-ISLAND-2a: tear down the headless Vulkan device + all
    // island resources (no-op if the island was never lazily initialized).
    destroyVulkanEdgeFogIsland();
    // VULKAN-OOB-FOG-ISLAND-1: same teardown for the OOB-fog island.
    destroyVulkanOobFogIsland();
    // VULKAN-POSTPROCESS-SUBGRAPH-1: same teardown for the fused subgraph.
    destroyVulkanPostprocessSubgraph();
#endif

    // CLUSTER-DEPTH-PYRAMID-NATIVE-1: release the gated pass's GL resources.
    // No-op when the gate was never enabled (nothing was allocated).
    cluster_depth_pyramid::Shutdown();

    // MC2-LIGHTGRID-BUILD-NATIVE-1: release the gated light-grid builder's GL
    // resources. No-op when the gate was never enabled.
    lightgrid_build::Shutdown();

    // POSTPROCESS-COMPUTE-BLUR-1: release the gated blur substrate's GL
    // resources. No-op when the gate was never enabled.
    postprocess_blur::Shutdown();

    if (compositeProg_) {
        glsl_program::deleteProgram("postprocess");
        compositeProg_ = nullptr;
    }

    if (skyboxProg_) {
        glsl_program::deleteProgram("skybox");
        skyboxProg_ = nullptr;
    }

    if (hdriSkyboxProg_) {
        glsl_program::deleteProgram("hdri_skybox");
        hdriSkyboxProg_ = nullptr;
    }
    if (hdriSkyboxStencilTagProg_) {
        glsl_program::deleteProgram("hdri_skybox_stencil_tag");
        hdriSkyboxStencilTagProg_ = nullptr;
    }
    if (hdriTex_) {
        glDeleteTextures(1, &hdriTex_);
        hdriTex_ = 0;
    }
    if (hdriDummyVao_) {
        glDeleteVertexArrays(1, &hdriDummyVao_);
        hdriDummyVao_ = 0;
    }
    hdriReady_ = false;
    hdriEnabled_ = false;

    if (shadowDebugProg_) {
        glsl_program::deleteProgram("shadow_debug");
        shadowDebugProg_ = nullptr;
    }

    if (screenShadowProg_) {
        glsl_program::deleteProgram("shadow_screen");
        screenShadowProg_ = nullptr;
    }

    if (cloudProg_) {
        glsl_program::deleteProgram("cloud");
        cloudProg_ = nullptr;
    }

    if (shorelineProg_) {
        glsl_program::deleteProgram("shoreline");
        shorelineProg_ = nullptr;
    }

    if (ssaoProg_) {
        glsl_program::deleteProgram("ssao");
        ssaoProg_ = nullptr;
    }
    if (ssaoApplyProg_) {
        glsl_program::deleteProgram("ssao_apply");
        ssaoApplyProg_ = nullptr;
    }
    if (fogOobProg_) {
        glsl_program::deleteProgram("fog_oob");
        fogOobProg_ = nullptr;
    }
    if (edgeFogProg_) {
        glsl_program::deleteProgram("edge_fog");
        edgeFogProg_ = nullptr;
    }

    destroyShadows();
    destroyDynamicShadows();

    s_postProcess = nullptr;
    initialized_ = false;
}


void gosPostProcess::resize(int w, int h)
{
    if (w == width_ && h == height_)
        return;

    width_ = w;
    height_ = h;

    destroyFBOs();
    createFBOs(w, h);
}

void gosPostProcess::createFBOs(int w, int h)
{
    // --- Scene FBO (full resolution, HDR) ---
    glGenFramebuffers(1, &sceneFBO_);
    RenderCore::framegraph::fboLedger().registerFbo(sceneFBO_, RenderCore::RenderResourceId::MainColor);  // FBO-LEDGER-1
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);

    // Color attachment: RGBA16F
    // TEX-CLASS: render-target -- scene HDR color (MRT0, FBO attach)
    glGenTextures(1, &sceneColorTex_);
    glBindTexture(GL_TEXTURE_2D, sceneColorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sceneColorTex_, 0);

    // LINEAR-COLOR-AUDIT-1: one-shot runtime color-pipeline probe. Logging only,
    // no GL state change. Gate MC2_LIGHTING_LINEAR_AUDIT=1. Confirms at RUNTIME
    // (not just from source) the facts linear-correctness/PBR work depends on:
    // whether GL_FRAMEBUFFER_SRGB auto-encode is enabled, the scene render-target
    // internal format, and whether the HDR/tonemap post path is engaged.
    if (getenv("MC2_LIGHTING_LINEAR_AUDIT")) {
        static bool s_linearAudited = false;
        if (!s_linearAudited) {
            s_linearAudited = true;
            GLint sceneFmt = 0;  // sceneColorTex_ is still bound to GL_TEXTURE_2D here
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &sceneFmt);
            GLboolean fbSrgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
            const char* hdr   = getenv("MC2_HDR_POST");
            const char* tm    = getenv("MC2_TONEMAP_ACES");
            const char* bloom = getenv("MC2_BLOOM");
            fprintf(stderr,
                "[LINEAR_AUDIT] event=color_pipeline_probe framebuffer_srgb=%d "
                "scene_color_internalformat=0x%04X (0x881A=GL_RGBA16F) "
                "hdr_post=%s tonemap_aces=%s bloom=%s verdict=%s\n",
                (int)fbSrgb, (unsigned)sceneFmt,
                hdr ? hdr : "(unset/off)", tm ? tm : "(unset/off)", bloom ? bloom : "(unset/off)",
                (fbSrgb == GL_FALSE)
                  ? "GAMMA_SPACE_MATH:no_FRAMEBUFFER_SRGB->shader_lighting_in_sRGB_space->linear_correctness_required_before_PBR"
                  : "FRAMEBUFFER_SRGB_ON:GL_auto_encodes->verify_shader_inputs_decoded_to_linear");
            fflush(stderr);
        }
    }

    // Depth/stencil texture (sampleable for post-process depth reconstruction)
    // TEX-CLASS: render-target -- scene depth (sampleable, FBO attach)
    glGenTextures(1, &sceneDepthTex_);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, sceneDepthTex_, 0);

    // SKYBOX-FOG-EXCLUDE-1: stencil-only view onto sceneDepthTex_'s storage.
    // glTextureView requires immutable storage (glTexStorage2D above), which
    // sceneDepthTex_ already uses. GL_DEPTH_STENCIL_TEXTURE_MODE=GL_STENCIL_INDEX
    // is baked into the view at creation, so runEdgeFog/runFogOob can sample
    // depth (via sceneDepthTex_) and stencil (via this view) in the SAME draw
    // without per-bind mode toggling on one shared object. Gated on
    // MC2_SKYBOX_FOG_EXCLUDE=1 -- skipped entirely when OFF (zero cost OFF).
    if (skyboxFogExcludeEnabled()) {
        glGenTextures(1, &sceneStencilViewTex_);
        glTextureView(sceneStencilViewTex_, GL_TEXTURE_2D, sceneDepthTex_,
                      GL_DEPTH24_STENCIL8, 0, 1, 0, 1);
        glBindTexture(GL_TEXTURE_2D, sceneStencilViewTex_);
        glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // Normal buffer: MRT attachment 1 (rgb=world normal encoded, a=shadow skip flag)
    // TEX-CLASS: render-target -- GBuffer normal (MRT1, FBO attach)
    glGenTextures(1, &sceneNormalTex_);
    glBindTexture(GL_TEXTURE_2D, sceneNormalTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, sceneNormalTex_, 0);

    // M1.5: object-ID attachment-2 (GL_R32UI). Gated on
    // MC2_OBJECT_ID_BUFFER; when env-OFF we skip the texture
    // creation entirely so env-OFF runtime cost is exactly zero on
    // the FBO side. glTexImage2D matches the sceneNormalTex_ pattern
    // above (decision m4); glTexStorage2D migration deferred.
    if (RenderWorld::IsObjectIdBufferEnabled()) {
        // TEX-CLASS: render-target -- objectID GBuffer (MRT2, FBO attach)
        glGenTextures(1, &sceneObjectIdTex_);
        glBindTexture(GL_TEXTURE_2D, sceneObjectIdTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, w, h, 0,
                     GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2,
                               GL_TEXTURE_2D, sceneObjectIdTex_, 0);
    }

    // MRT: draw to color attachments via centralized policy. Helper
    // adds GL_COLOR_ATTACHMENT2 when env-ON AND sceneObjectIdTex_
    // exists (M1.5 C1 + M3 fix).
    setSceneDrawBuffers(SceneDrawBufferMode::MainSceneMRT,
                        sceneObjectIdTex_ != 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gosPostProcess: scene FBO incomplete (0x%x)\n", status);
    }

    // REGISTER-MAIN-DEPTH-RESOURCE: publish the full-res scene depth as the
    // MainDepth slot (descriptive only; this owner keeps the GL lifetime).
    // sceneDepthTex_ is a sampleable GL_TEXTURE_2D (GL_DEPTH24_STENCIL8); the
    // Depth24 enum is the closest descriptive label (no combined D/S enum).
    // A future HZB build pass reads desc->glName to source the pyramid.
    {
        RenderCore::RenderResourceDesc dd;
        dd.id        = RenderCore::RenderResourceId::MainDepth;
        dd.kind      = RenderCore::RenderResourceKind::Texture2D;
        dd.lifetime  = RenderCore::RenderResourceLifetime::Persistent;  // screen-sized, recreated only on resize
        dd.format    = RenderCore::RenderResourceFormat::Depth24;
        dd.debugName = "MainDepth";
        dd.width     = (uint32_t)w;
        dd.height    = (uint32_t)h;
        dd.glName    = sceneDepthTex_;
        dd.valid     = (status == GL_FRAMEBUFFER_COMPLETE);
        RenderCore::registerOrUpdateRenderResource(dd);
    }

    // REGISTRY-POSTPROCESS-FBO-1: publish the scene FBO color targets (observe-only
    // metadata; this owner keeps the GL lifetimes — registry is never read by the
    // draw path). MainColor closes the asymmetry where the FBO-LEDGER tracked
    // sceneFBO_ but the resource registry had no glName for sceneColorTex_.
    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::MainColor;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.lifetime  = RenderCore::RenderResourceLifetime::Persistent;  // screen-sized, recreated only on resize
        d.format    = RenderCore::RenderResourceFormat::RGBA16F;
        d.debugName = "MainColor";
        d.width     = (uint32_t)w;
        d.height    = (uint32_t)h;
        d.glName    = sceneColorTex_;
        d.valid     = (status == GL_FRAMEBUFFER_COMPLETE);
        RenderCore::registerOrUpdateRenderResource(d);
    }
    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::MainNormal;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.lifetime  = RenderCore::RenderResourceLifetime::Persistent;  // screen-sized GBuffer1, recreated only on resize
        d.format    = RenderCore::RenderResourceFormat::RGBA16F;
        d.debugName = "MainNormal";
        d.width     = (uint32_t)w;
        d.height    = (uint32_t)h;
        d.glName    = sceneNormalTex_;
        d.valid     = (status == GL_FRAMEBUFFER_COMPLETE);
        RenderCore::registerOrUpdateRenderResource(d);
    }
    // SceneObjectId is gated on RenderWorld::IsObjectIdBufferEnabled(); the GBuffer2
    // texture only exists when the gate is live, so register valid ONLY then. When
    // the gate is OFF the slot stays at its default (unregistered / invalid).
    if (sceneObjectIdTex_ != 0) {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::SceneObjectId;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.lifetime  = RenderCore::RenderResourceLifetime::Persistent;  // screen-sized GBuffer2, recreated only on resize
        d.format    = RenderCore::RenderResourceFormat::Unknown;  // GL_R32UI; no enum slot
        d.debugName = "SceneObjectId";
        d.width     = (uint32_t)w;
        d.height    = (uint32_t)h;
        d.glName    = sceneObjectIdTex_;
        d.valid     = (status == GL_FRAMEBUFFER_COMPLETE);
        RenderCore::registerOrUpdateRenderResource(d);
    }

    // --- SSAO FBO (half resolution, single-channel R16F) ---
    {
        ssaoW_ = w / 2; if (ssaoW_ < 1) ssaoW_ = 1;
        ssaoH_ = h / 2; if (ssaoH_ < 1) ssaoH_ = 1;

        // TEX-CLASS: render-target -- SSAO occlusion (FBO attach)
        glGenTextures(1, &ssaoColorTex_);
        glBindTexture(GL_TEXTURE_2D, ssaoColorTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, ssaoW_, ssaoH_, 0, GL_RED, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &ssaoFBO_);
        RenderCore::framegraph::fboLedger().registerFbo(ssaoFBO_, RenderCore::RenderResourceId::SsaoOcclusion);  // FBO-LEDGER-EXTEND-1
        glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ssaoColorTex_, 0);

        GLenum aoStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (aoStatus != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "gosPostProcess: SSAO FBO incomplete (0x%x)\n", aoStatus);

        // REGISTRY-POSTPROCESS-FBO-1: publish the half-res AO target (observe-only).
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::SsaoOcclusion;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.lifetime  = RenderCore::RenderResourceLifetime::FrameLocal;  // transient AO, produced+consumed within endScene
        d.format    = RenderCore::RenderResourceFormat::R32F;  // GL_R16F storage; closest enum
        d.debugName = "SsaoOcclusion";
        d.width     = (uint32_t)ssaoW_;
        d.height    = (uint32_t)ssaoH_;
        d.glName    = ssaoColorTex_;
        d.valid     = (aoStatus == GL_FRAMEBUFFER_COMPLETE);
        RenderCore::registerOrUpdateRenderResource(d);
    }

    // --- WATER-REFLECTION-RESOURCE-1: quarter-res reflection target ---
    // Substrate only: allocated + registered, but NO producer renders into it
    // until Phase C, so the texture reads black. Color (RGBA16F) + depth (D24).
    {
        waterReflW_ = w / 4; if (waterReflW_ < 1) waterReflW_ = 1;
        waterReflH_ = h / 4; if (waterReflH_ < 1) waterReflH_ = 1;

        // TEX-CLASS: render-target -- water reflection color (FBO attach)
        glGenTextures(1, &waterReflColorTex_);
        glBindTexture(GL_TEXTURE_2D, waterReflColorTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, waterReflW_, waterReflH_, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // TEX-CLASS: render-target -- water reflection depth (FBO attach)
        glGenTextures(1, &waterReflDepthTex_);
        glBindTexture(GL_TEXTURE_2D, waterReflDepthTex_);
        // GL_FLOAT type + CLAMP_TO_EDGE wrap: match the project's other depth
        // textures (shadow maps) to avoid AMD quirks + OOB-sample wrapping traps.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, waterReflW_, waterReflH_, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &waterReflFBO_);
        RenderCore::framegraph::fboLedger().registerFbo(waterReflFBO_, RenderCore::RenderResourceId::WaterReflectionColor);  // FBO-LEDGER-1
        glBindFramebuffer(GL_FRAMEBUFFER, waterReflFBO_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, waterReflColorTex_, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,  GL_TEXTURE_2D, waterReflDepthTex_, 0);

        GLenum wrStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (wrStatus != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "gosPostProcess: water reflection FBO incomplete (0x%x)\n", wrStatus);

        // Register descriptors (descriptive; this owner keeps the GL lifetime).
        RenderCore::RenderResourceDesc cdesc;
        cdesc.id        = RenderCore::RenderResourceId::WaterReflectionColor;
        cdesc.kind      = RenderCore::RenderResourceKind::Texture2D;
        cdesc.lifetime  = RenderCore::RenderResourceLifetime::External;  // temporal N-1 reflection target
        cdesc.format    = RenderCore::RenderResourceFormat::RGBA16F;
        cdesc.debugName = "WaterReflectionColor";
        cdesc.width     = (uint32_t)waterReflW_;
        cdesc.height    = (uint32_t)waterReflH_;
        cdesc.glName    = waterReflColorTex_;
        cdesc.valid     = (wrStatus == GL_FRAMEBUFFER_COMPLETE);
        RenderCore::registerOrUpdateRenderResource(cdesc);

        RenderCore::RenderResourceDesc ddesc;
        ddesc.id        = RenderCore::RenderResourceId::WaterReflectionDepth;
        ddesc.kind      = RenderCore::RenderResourceKind::Texture2D;
        ddesc.lifetime  = RenderCore::RenderResourceLifetime::External;  // temporal N-1 reflection target
        ddesc.format    = RenderCore::RenderResourceFormat::Depth24;
        ddesc.debugName = "WaterReflectionDepth";
        ddesc.width     = (uint32_t)waterReflW_;
        ddesc.height    = (uint32_t)waterReflH_;
        ddesc.glName    = waterReflDepthTex_;
        ddesc.valid     = (wrStatus == GL_FRAMEBUFFER_COMPLETE);
        RenderCore::registerOrUpdateRenderResource(ddesc);
    }

    // --- HZB-DEPTH-PYRAMID-MVP-1: full-res reverse-Z Hi-Z pyramid (R32F) ---
    // Allocated ONLY when MC2_HZB_BUILD is on -> zero cost / byte-identical when
    // off. Immutable mip chain (glTexStorage2D) sized by the ceil ladder down to
    // 1x1; NEAREST + CLAMP_TO_EDGE (explicit-LOD sampling, clamped 2x2 taps).
    if (hzbEnabled_) {
        hzbW_ = w;
        hzbH_ = h;
        // ceil mip ladder: each level halves+rounds-up each axis to 1.
        int maxDim = (w > h) ? w : h;
        int mips = 1;
        while (maxDim > 1) { maxDim = (maxDim + 1) / 2; ++mips; }
        if (mips > kHzbMaxLevels) mips = kHzbMaxLevels;
        hzbMipCount_ = mips;

        // One ceil-sized R32F texture PER level (NOT a mip chain). A single
        // mipped texture cannot be used here: AMD rejects attaching mip level >0
        // unless the texture is mipmap-complete, but the ceil ladder is
        // deliberately mipmap-incomplete (preserves odd-extent texels per
        // docs/hzb-depth-convention.md). Separate textures also remove any
        // read/write feedback (source and dest are distinct objects).
        int lw = w, lh = h;
        for (int level = 0; level < hzbMipCount_; ++level) {
            // TEX-CLASS: render-target -- Hi-Z pyramid per-level (FBO attach)
            glGenTextures(1, &hzbLevelTex_[level]);
            glBindTexture(GL_TEXTURE_2D, hzbLevelTex_[level]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, lw, lh, 0,
                         GL_RED, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            lw = (lw + 1) / 2; if (lw < 1) lw = 1;
            lh = (lh + 1) / 2; if (lh < 1) lh = 1;
        }

        glGenFramebuffers(1, &hzbFBO_);
        RenderCore::framegraph::fboLedger().registerFbo(hzbFBO_, RenderCore::RenderResourceId::HzbPyramid);  // FBO-LEDGER-EXTEND-1
        // The destination level texture is bound to COLOR_ATTACHMENT0 per pass
        // in runHzbReduce(). No dedicated registry slot yet -- surfaced via the
        // getHzb* accessors.

        std::fprintf(stderr, "[HZB_BUILD v1] allocated %dx%d mips=%d (R32F, per-level textures)\n",
                     hzbW_, hzbH_, hzbMipCount_);

        // REGISTRY-POSTPROCESS-FBO-1: publish the Hi-Z pyramid base level (observe-only).
        // Gated: only allocated/registered when hzbEnabled_; the per-level chain is
        // surfaced via getHzb* accessors, the registry tracks the level-0 handle.
        if (hzbLevelTex_[0]) {
            RenderCore::RenderResourceDesc d;
            d.id        = RenderCore::RenderResourceId::HzbPyramid;
            d.kind      = RenderCore::RenderResourceKind::Texture2D;
            d.lifetime  = RenderCore::RenderResourceLifetime::Persistent;  // frame-persistent, survives mission reload; recreated on resize
            d.format    = RenderCore::RenderResourceFormat::R32F;
            d.debugName = "HzbPyramid";
            d.width     = (uint32_t)hzbW_;
            d.height    = (uint32_t)hzbH_;
            d.layers    = (uint32_t)hzbMipCount_;
            d.glName    = hzbLevelTex_[0];
            d.valid     = true;
            RenderCore::registerOrUpdateRenderResource(d);
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gosPostProcess::copySceneDepthForParticles()
{
    // VFX-SOFT-PARTICLES-MVP-1: snapshot the current scene depth into a
    // dedicated texture so the in-scene particle flush can sample it without a
    // GL feedback loop (sceneDepthTex_ is the bound FBO's depth attachment).
    // Same internal format as sceneDepthTex_ (DEPTH24_STENCIL8) -> straight
    // glCopyImageSubData (no blit/format-match constraints). Lazily allocated;
    // full-res; freed + re-created on resize (destroyFBOs zeroes it).
    if (width_ <= 0 || height_ <= 0 || sceneDepthTex_ == 0) return;

    if (sceneDepthCopyTex_ == 0) {
        // TEX-CLASS: render-target -- depth snapshot for soft particles (copy dst)
        glGenTextures(1, &sceneDepthCopyTex_);
        glBindTexture(GL_TEXTURE_2D, sceneDepthCopyTex_);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, width_, height_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        // REGISTRY-POSTPROCESS-FBO-1: publish the lazily-allocated depth-copy RT
        // (observe-only). Registered at the lazy-alloc site so glName is the live
        // handle; invalidated in destroyFBOs (resize re-allocates lazily).
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::SceneDepthCopy;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.lifetime  = RenderCore::RenderResourceLifetime::FrameLocal;  // per-frame depth snapshot for soft-depth sampling
        d.format    = RenderCore::RenderResourceFormat::Depth24;  // DEPTH24_STENCIL8; closest enum
        d.debugName = "SceneDepthCopy";
        d.width     = (uint32_t)width_;
        d.height    = (uint32_t)height_;
        d.glName    = sceneDepthCopyTex_;
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    // Texture-to-texture copy of the whole depth image. Ordered after the
    // opaque scene's depth writes; particles never write depth so the source
    // is stable at flush time.
    glCopyImageSubData(sceneDepthTex_,     GL_TEXTURE_2D, 0, 0, 0, 0,
                       sceneDepthCopyTex_, GL_TEXTURE_2D, 0, 0, 0, 0,
                       width_, height_, 1);
}

void gosPostProcess::copySceneColorForVfx()
{
    // VFX-SCENECOLOR-GRAB-1 (FRAME_RESOURCE_SUBSTRATE): snapshot the resolved
    // scene COLOR into a dedicated texture so a future in-scene VFX/transparent
    // pass can sample it WITHOUT a GL feedback loop (sceneColorTex_ is the bound
    // FBO's COLOR_ATTACHMENT0 during the flush). Mirrors copySceneDepthForParticles
    // exactly: same internal format as sceneColorTex_ (RGBA16F) -> a straight
    // glCopyImageSubData (no blit/format-match constraint). Lazily allocated;
    // full-res; freed + re-created on resize (destroyFBOs zeroes it). There is
    // NO consumer yet — this slice only produces the resource (gate default OFF).
    if (width_ <= 0 || height_ <= 0 || sceneColorTex_ == 0) return;

    if (sceneColorCopyTex_ == 0) {
        // TEX-CLASS: render-target -- scene-color snapshot for VFX feedback (copy dst)
        glGenTextures(1, &sceneColorCopyTex_);
        glBindTexture(GL_TEXTURE_2D, sceneColorCopyTex_);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, width_, height_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        // REGISTRY-SCENECOLORCOPY-PRODUCER-1: publish the lazily-allocated scene
        // color-copy RT (observe-only). Registered at the lazy-alloc site so glName
        // is the live handle; invalidated in destroyFBOs (resize re-allocates
        // lazily). Mirrors SceneDepthCopy (REGISTRY-POSTPROCESS-FBO-1). Closes the
        // "id-without-producer" gap for SceneColorCopy (enum id 20).
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::SceneColorCopy;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.lifetime  = RenderCore::RenderResourceLifetime::FrameLocal;  // per-frame scene-color snapshot for VFX feedback safety
        d.format    = RenderCore::RenderResourceFormat::RGBA16F;
        d.debugName = "SceneColorCopy";
        d.width     = (uint32_t)width_;
        d.height    = (uint32_t)height_;
        d.glName    = sceneColorCopyTex_;
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    // Texture-to-texture copy of the whole color image. Ordered after the
    // opaque scene color is resolved but before the VFX/transparent flush that
    // would sample it, so the snapshot is the scene as it stands pre-VFX.
    // RGBA16F -> RGBA16F: same internalformat, GL-clean under glCopyImageSubData.
    glCopyImageSubData(sceneColorTex_,     GL_TEXTURE_2D, 0, 0, 0, 0,
                       sceneColorCopyTex_, GL_TEXTURE_2D, 0, 0, 0, 0,
                       width_, height_, 1);
}

void gosPostProcess::destroyFBOs()
{
    if (sceneFBO_) {
        glDeleteFramebuffers(1, &sceneFBO_);
        sceneFBO_ = 0;
    }
    if (sceneColorTex_) {
        glDeleteTextures(1, &sceneColorTex_);
        sceneColorTex_ = 0;
        // REGISTRY-POSTPROCESS-FBO-1: mark MainColor unavailable on teardown
        // (resize destroys+recreates; createFBOs re-validates).
        RenderCore::RenderResourceDesc inv;
        inv.id = RenderCore::RenderResourceId::MainColor; inv.valid = false;
        RenderCore::registerOrUpdateRenderResource(inv);
    }
    if (sceneDepthTex_) {
        glDeleteTextures(1, &sceneDepthTex_);
        sceneDepthTex_ = 0;
        // REGISTER-MAIN-DEPTH-RESOURCE: mark the slot unavailable on teardown
        // (resize destroys+recreates, so this re-validates in createFBOs).
        RenderCore::RenderResourceDesc inv;
        inv.id = RenderCore::RenderResourceId::MainDepth; inv.valid = false;
        RenderCore::registerOrUpdateRenderResource(inv);
    }
    // SKYBOX-FOG-EXCLUDE-1: the stencil view is a texture-view onto
    // sceneDepthTex_'s storage, not a separate allocation -- it must be
    // deleted before/independently of the parent (view deletion never frees
    // the parent's storage either way, but drop our handle so resize()
    // re-creates a fresh view against the new sceneDepthTex_).
    if (sceneStencilViewTex_) {
        glDeleteTextures(1, &sceneStencilViewTex_);
        sceneStencilViewTex_ = 0;
    }
    if (sceneNormalTex_) {
        glDeleteTextures(1, &sceneNormalTex_);
        sceneNormalTex_ = 0;
        // REGISTRY-POSTPROCESS-FBO-1: mark MainNormal unavailable on teardown.
        RenderCore::RenderResourceDesc inv;
        inv.id = RenderCore::RenderResourceId::MainNormal; inv.valid = false;
        RenderCore::registerOrUpdateRenderResource(inv);
    }
    if (sceneObjectIdTex_) {
        glDeleteTextures(1, &sceneObjectIdTex_);
        sceneObjectIdTex_ = 0;
        // REGISTRY-POSTPROCESS-FBO-1: mark SceneObjectId unavailable on teardown.
        RenderCore::RenderResourceDesc inv;
        inv.id = RenderCore::RenderResourceId::SceneObjectId; inv.valid = false;
        RenderCore::registerOrUpdateRenderResource(inv);
    }
    if (sceneDepthCopyTex_) {  // VFX-SOFT-PARTICLES-MVP-1
        glDeleteTextures(1, &sceneDepthCopyTex_);
        sceneDepthCopyTex_ = 0;
        // REGISTRY-POSTPROCESS-FBO-1: mark SceneDepthCopy unavailable on teardown
        // (lazily re-allocated by copySceneDepthForParticles after resize).
        RenderCore::RenderResourceDesc inv;
        inv.id = RenderCore::RenderResourceId::SceneDepthCopy; inv.valid = false;
        RenderCore::registerOrUpdateRenderResource(inv);
    }
    if (sceneColorCopyTex_) {  // VFX-SCENECOLOR-GRAB-1
        glDeleteTextures(1, &sceneColorCopyTex_);
        sceneColorCopyTex_ = 0;
        // REGISTRY-SCENECOLORCOPY-PRODUCER-1: mark SceneColorCopy unavailable on
        // teardown (lazily re-allocated by copySceneColorForVfx after resize).
        RenderCore::RenderResourceDesc inv;
        inv.id = RenderCore::RenderResourceId::SceneColorCopy; inv.valid = false;
        RenderCore::registerOrUpdateRenderResource(inv);
    }
    // SSAO-GTAO-LITE-MVP-1: free half-res AO target.
    if (ssaoColorTex_) {
        glDeleteTextures(1, &ssaoColorTex_); ssaoColorTex_ = 0;
        // REGISTRY-POSTPROCESS-FBO-1: mark SsaoOcclusion unavailable on teardown.
        RenderCore::RenderResourceDesc inv;
        inv.id = RenderCore::RenderResourceId::SsaoOcclusion; inv.valid = false;
        RenderCore::registerOrUpdateRenderResource(inv);
    }
    if (ssaoFBO_)      { glDeleteFramebuffers(1, &ssaoFBO_);   ssaoFBO_ = 0; }
    ssaoW_ = ssaoH_ = 0;

    // HZB-DEPTH-PYRAMID-MVP-1: free the per-level pyramid textures + FBO
    // (resize re-allocates if the gate is on).
    bool hzbHadBase = (hzbLevelTex_[0] != 0);
    for (int i = 0; i < kHzbMaxLevels; ++i) {
        if (hzbLevelTex_[i]) { glDeleteTextures(1, &hzbLevelTex_[i]); hzbLevelTex_[i] = 0; }
    }
    if (hzbHadBase) {
        // REGISTRY-POSTPROCESS-FBO-1: mark HzbPyramid unavailable on teardown.
        RenderCore::RenderResourceDesc inv;
        inv.id = RenderCore::RenderResourceId::HzbPyramid; inv.valid = false;
        RenderCore::registerOrUpdateRenderResource(inv);
    }
    if (hzbFBO_) { glDeleteFramebuffers(1, &hzbFBO_); hzbFBO_ = 0; }
    hzbW_ = hzbH_ = hzbMipCount_ = 0;

    // WATER-REFLECTION-RESOURCE-1: free reflection target + mark slots invalid.
    if (waterReflColorTex_) { glDeleteTextures(1, &waterReflColorTex_); waterReflColorTex_ = 0; }
    if (waterReflDepthTex_) { glDeleteTextures(1, &waterReflDepthTex_); waterReflDepthTex_ = 0; }
    if (waterReflFBO_)      { glDeleteFramebuffers(1, &waterReflFBO_);   waterReflFBO_ = 0; }
    waterReflW_ = waterReflH_ = 0;
    {
        RenderCore::RenderResourceDesc inv;
        inv.id = RenderCore::RenderResourceId::WaterReflectionColor; inv.valid = false;
        RenderCore::registerOrUpdateRenderResource(inv);
        inv.id = RenderCore::RenderResourceId::WaterReflectionDepth;
        RenderCore::registerOrUpdateRenderResource(inv);
    }
}

void gosPostProcess::createFullscreenQuad()
{
    glGenVertexArrays(1, &quadVAO_);
    glBindVertexArray(quadVAO_);

    glGenBuffers(1, &quadVBO_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVerts), kQuadVerts, GL_STATIC_DRAW);

    // layout(location = 0) in vec2 pos
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    // layout(location = 1) in vec2 uv
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void gosPostProcess::destroyFullscreenQuad()
{
    if (quadVBO_) {
        glDeleteBuffers(1, &quadVBO_);
        quadVBO_ = 0;
    }
    if (quadVAO_) {
        glDeleteVertexArrays(1, &quadVAO_);
        quadVAO_ = 0;
    }
}

// BT2018-BOX-DECAL-1: unit cube [-0.5,0.5]^3, 8 corners / 36 indices (12 tris).
void gosPostProcess::createBoxDecalCube()
{
    static const float kCubeVerts[24] = {
        -0.5f,-0.5f,-0.5f,  0.5f,-0.5f,-0.5f,  0.5f, 0.5f,-0.5f,  -0.5f, 0.5f,-0.5f,
        -0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f,  -0.5f, 0.5f, 0.5f
    };
    // Outward-CCW winding (so Cull Front draws the BACK faces -> coverage persists
    // even when the camera is inside the tall box).
    static const unsigned int kCubeIdx[36] = {
        0,1,2, 0,2,3,   // -Z
        4,6,5, 4,7,6,   // +Z
        0,4,5, 0,5,1,   // -Y
        3,2,6, 3,6,7,   // +Y
        0,3,7, 0,7,4,   // -X
        1,5,6, 1,6,2    // +X
    };
    glGenVertexArrays(1, &boxCubeVAO_);
    glBindVertexArray(boxCubeVAO_);
    glGenBuffers(1, &boxCubeVBO_);
    glBindBuffer(GL_ARRAY_BUFFER, boxCubeVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kCubeVerts), kCubeVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glGenBuffers(1, &boxCubeIBO_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, boxCubeIBO_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(kCubeIdx), kCubeIdx, GL_STATIC_DRAW);
    glBindVertexArray(0);   // unbind VAO first (captures the IBO binding)
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void gosPostProcess::destroyBoxDecalCube()
{
    if (boxCubeIBO_) { glDeleteBuffers(1, &boxCubeIBO_);      boxCubeIBO_ = 0; }
    if (boxCubeVBO_) { glDeleteBuffers(1, &boxCubeVBO_);      boxCubeVBO_ = 0; }
    if (boxCubeVAO_) { glDeleteVertexArrays(1, &boxCubeVAO_); boxCubeVAO_ = 0; }
}

// Column-major mat4 * vec4 (matches GLSL `M * v` on a GL_FALSE-uploaded array — the
// convention inverseViewProj_/viewProj_ use). out[i] = sum_c m[c*4 + i] * v[c].
static void boxDecalMulColMajor(const float* m, const float* v, float* out)
{
    for (int i = 0; i < 4; ++i)
        out[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i] * v[3];
}

// BT2018-BOX-DECAL-1: screen-space AABB decal volume. Composites into COLOR0 only.
void gosPostProcess::drawBoxDecals()
{
    ZoneScopedN("Render.BoxDecals");
    TracyGpuZone("Render.BoxDecals");

    // DECAL-INTEGRATE-1: the projection pass is now a SECOND, opt-in consumer of the
    // dynamic_decal_ring impact slots. Gate MC2_PROJECTED_DECALS (default OFF). When OFF,
    // this pass does not run at all -> byte-identical, no GL state touched. The baked
    // gos_PushDecal / drawDecals crater path is UNTOUCHED and runs regardless (parallel).
    if (!projectedDecalsEnabled_) return;
    if (!sceneHasTerrain_) return;                 // suppress in menus
    if (!boxDecalProg_ || !boxDecalProg_->is_valid()) return;

    // Snapshot live ring impact sites (pure read; does NOT advance/expire ring slots —
    // that stays gatherToDecalBatch()'s job under MC2_DYNAMIC_DECALS). One projected
    // decal per live impact. No live impacts -> nothing to draw, return early.
    DynDecal::Slot slots[DynDecal::kCapacity];
    float          slotAlpha[DynDecal::kCapacity];
    int nSlots = DynDecal::snapshotLiveSlots(slots, slotAlpha, DynDecal::kCapacity);
    if (nSlots <= 0) return;

    // Feedback-safe depth: refresh the COPY and sample IT — never the live bound
    // depth attachment (sceneDepthTex_). Idempotent / lazy-alloc.
    copySceneDepthForParticles();
    if (sceneDepthCopyTex_ == 0) return;

    float strength = 0.85f;
    if (const char* s = getenv("MC2_BOX_DECAL_STRENGTH")) { float v = (float)atof(s); if (v >= 0.0f) strength = v; }

    // Vertical span of the AABB on the reconstruct-frame up axis (+Y). The box is made
    // tall so it drapes across terrain elevation under the impact; the FS world-AABB clip
    // does the real coverage. Overridable for tuning.
    float ySpan = 4000.0f;
    if (const char* s = getenv("MC2_BOX_DECAL_YSPAN")) { float v = (float)atof(s); if (v > 1.0f) ySpan = v; }

    // Normal-reject: SHIPPED DISABLED by default. The terrain GBuffer1 normal is a
    // constant flat-up sentinel in a Z-up frame while the box up-axis here is Y-up, so a
    // live dot()-reject would discard ALL terrain pixels (see DECAL-INTEGRATE-1 doc, the
    // up-axis TODO). A threshold <= -1.5 makes the reject a no-op (never discards). Do
    // NOT raise this without first reconciling u_decalUpAxis against the reconstruct frame.
    float normalRejectCos = -1.5f;
    if (const char* s = getenv("MC2_BOX_DECAL_NORMAL_REJECT")) { float v = (float)atof(s); normalRejectCos = v; }

    float upAxis[3] = { 0.0f, 1.0f, 0.0f };   // reconstruct-frame up (Y-up). TODO: reconcile.

    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);   // COLOR0 only -> normal/objectId untouched
    glViewport(0, 0, width_, height_);

    // Draw the box BACK faces (Cull Front) with depth-test OFF: the FS box-clip +
    // depth reconstruct decide coverage, which sidesteps the reversed-Z ZTest-direction
    // trap and any feedback with the live depth attachment. Depth-write OFF.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Bind samplers + textures once; per-slot we only re-push the box volume uniforms.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthCopyTex_);   // the COPY, not the live attachment
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sceneNormalTex_);

    float screenSz[2] = { (float)width_, (float)height_ };

    glBindVertexArray(boxCubeVAO_);
    for (int i = 0; i < nSlots; ++i) {
        const DynDecal::Slot& sl = slots[i];

        // World centre from the ring (MC2 world frame x=east, y=north, z=elev). The FS
        // world-AABB clip is frame-consistent; a tall +Y box drapes over the terrain.
        float center[3] = { sl.wx, sl.wy, sl.wz };
        float half[3]   = { sl.radius, ySpan, sl.radius };
        float strengthSlot = strength * slotAlpha[i];   // lifetime fade reuse

        boxDecalProg_->setInt("u_sceneDepthTex", 0);
        boxDecalProg_->setInt("u_sceneNormalTex", 1);
        boxDecalProg_->setFloat3("u_boxCenter", center);
        boxDecalProg_->setFloat3("u_boxHalf", half);
        boxDecalProg_->setFloat3("u_decalUpAxis", upAxis);
        boxDecalProg_->setFloat("u_normalRejectCos", normalRejectCos);
        boxDecalProg_->setFloat("u_decalStrength", strengthSlot);
        boxDecalProg_->setFloat2("u_screenSize", screenSz);
        boxDecalProg_->apply();

        GLint loc;
        loc = glGetUniformLocation(boxDecalProg_->shp_, "u_viewProj");
        if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, viewProj_);
        loc = glGetUniformLocation(boxDecalProg_->shp_, "u_inverseViewProj");
        if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, inverseViewProj_);

        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0);
    }
    glBindVertexArray(0);

    // Restore the depth/cull/blend baseline the following passes + composite expect.
    glDisable(GL_BLEND);
    glCullFace(GL_BACK);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
}

void gosPostProcess::beginScene()
{
    if (!initialized_)
        return;

    prevFrameHadTerrain_ = sceneHasTerrain_;  // save for clear color decision
    sceneHasTerrain_ = false;  // reset each frame; set by markTerrainDrawn()

    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);

    // COLORMASK-ROLLOUT-1 (keystone): assert the steady-state color mask (all
    // attachments writable) at frame begin, before the clear + the first MRT scene
    // draw. In normal frames this is a no-op (the mask is already all-TRUE), so it is
    // byte-identical — but it HEALS any prior-frame set-only colorMask leak (e.g. a
    // future applyPipeline opt-in that left attachment 1/2 masked off), guaranteeing
    // terrain's GBuffer1 + objectId writes are never silently suppressed. Legacy
    // bridges keep their own save/restore; this only sets a known-good baseline so
    // incremental PipelineDesc colorMask opt-in is safe. (Resets the indexed masks
    // too, so a prior glColorMaski leak on attachment 1/2 cannot persist.)
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // Bind both draw buffers so the upcoming glClear in gameosmain.cpp clears
    // both COLOR0 and COLOR1 (the GBuffer1 normal/post-shadow-mask attachment).
    // After the clear, gameosmain.cpp calls pp->clearGBuffer1() to overwrite
    // attachment 1 with the post-shadow-eligible sentinel (0.5, 0.5, 1.0, 0.0)
    // before the scene renders. MRT remains bound for the entire scene draw;
    // every Group I/II shader either writes GBuffer1 explicitly via the
    // render-contract registry helpers or relies on the pre-cleared sentinel.
    // See docs/superpowers/specs/render-contract-f3-report.md for the F3
    // coherence guarantee. AMD location=1 corruption claim refuted 2026-04-27;
    // see docs/amd-driver-rules.md "Tested-and-refuted claims".
    if (sceneNormalTex_) {
        // M1.5 C1 + M3 fix: helper takes readiness flag explicitly.
        setSceneDrawBuffers(SceneDrawBufferMode::MainSceneMRT,
                            sceneObjectIdTex_ != 0);
    }
    // M1.5 m1 clear-order rule + M3 plan-review fix: glClearBufferuiv
    // at INDEX 2 only safe AFTER the env-ON 3-entry list is bound.
    // Guarded by the same readiness predicate that selects the 3-entry
    // list above; env-OFF byte-identical.
    if (RenderWorld::IsObjectIdBufferEnabled() && sceneObjectIdTex_) {
        static const GLuint kClearZero[4] = { 0u, 0u, 0u, 0u };
        setSceneDrawBuffers(SceneDrawBufferMode::MainSceneMRT, true);
        glClearBufferuiv(GL_COLOR, 2, kClearZero);
    }
    glViewport(0, 0, width_, height_);

    // [RES_DIAG v1] One-shot dump of the resolution split: scene FBO size
    // (this object's width_/height_) vs the HUD canvas (Environment.screenWidth)
    // vs the native drawable (Environment.drawableWidth). Env-gated so it is
    // byte-identical when MC2_RES_DIAG is unset. Confirms whether the scene
    // FBO tracks options-res (screenWidth) or the native desktop (drawable).
    static const bool s_resDiag = (getenv("MC2_RES_DIAG") != nullptr);
    static bool s_resDiagDone = false;
    if (s_resDiag && !s_resDiagDone) {
        s_resDiagDone = true;
        fprintf(stderr,
            "[RES_DIAG v1] sceneFBO=%dx%d  screenWidth=%dx%d (HUD canvas)  "
            "drawable=%dx%d (native)\n",
            width_, height_,
            Environment.screenWidth, Environment.screenHeight,
            Environment.drawableWidth, Environment.drawableHeight);
        fflush(stderr);
    }
}

void gosPostProcess::runHzbReduce()
{
    ZoneScopedN("Render.HZB");
    TracyGpuZone("Render.HZB");

    // HZB-DEPTH-PYRAMID-MVP-1: gated reverse-Z Hi-Z pyramid build. Diagnostic
    // substrate ONLY -- builds the pyramid, has no consumers, suppresses no
    // draws. Runs whenever the gate is on and the pyramid is allocated; on
    // depth-cleared frames (menus) the pyramid simply fills with the far value
    // (0.0) -- harmless, and the gate is default-OFF anyway.
    if (!hzbEnabled_) return;
    if (!hzbReduceProg_ || !hzbReduceProg_->is_valid()) return;
    if (!hzbLevelTex_[0] || !hzbFBO_ || hzbMipCount_ < 1) return;
    if (sceneDepthTex_ == 0) return;

    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);

    glBindFramebuffer(GL_FRAMEBUFFER, hzbFBO_);
    glBindVertexArray(quadVAO_);
    glActiveTexture(GL_TEXTURE0);

    bool ok = true;
    int dstW = hzbW_, dstH = hzbH_;     // tracks the current destination size
    for (int level = 0; level < hzbMipCount_; ++level) {
        // Render into this level's dedicated texture (always level 0 of it).
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, hzbLevelTex_[level], 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            if (hzbBuildCount_ == 0)
                fprintf(stderr, "[HZB_BUILD v1] FBO incomplete at level %d (0x%x)\n", level, st);
            ok = false;
            break;
        }
        glViewport(0, 0, dstW, dstH);

        if (level == 0) {
            // Seed level 0 with the raw reverse-Z scene depth (1:1 pass-through).
            glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
            float texel[2] = { 1.0f / (float)hzbW_, 1.0f / (float)hzbH_ };
            hzbReduceProg_->setInt("uReduce", 0);
            hzbReduceProg_->setInt("uSrc", 0);
            hzbReduceProg_->setFloat2("uSrcTexel", texel);
            hzbReduceProg_->apply();
        } else {
            // 2x2 MIN reduction from the previous level's texture. Source size =
            // the previous (larger) level; uSrcTexel drives the clamped 2x2 tap.
            int pW = hzbW_, pH = hzbH_;
            for (int k = 0; k < level - 1; ++k) { pW = (pW + 1) / 2; pH = (pH + 1) / 2; }
            if (pW < 1) pW = 1;
            if (pH < 1) pH = 1;

            glBindTexture(GL_TEXTURE_2D, hzbLevelTex_[level - 1]);
            float texel[2] = { 1.0f / (float)pW, 1.0f / (float)pH };
            hzbReduceProg_->setInt("uReduce", 1);
            hzbReduceProg_->setInt("uSrc", 0);
            hzbReduceProg_->setFloat2("uSrcTexel", texel);
            hzbReduceProg_->apply();
        }
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Advance destination size to the next (smaller) ceil level.
        dstW = (dstW + 1) / 2; if (dstW < 1) dstW = 1;
        dstH = (dstH + 1) / 2; if (dstH < 1) dstH = 1;
    }

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);

    if (ok) {
        ++hzbBuildCount_;
        if (hzbBuildCount_ <= 3 || (hzbBuildCount_ % 600) == 0)
            fprintf(stderr, "[HZB_BUILD v1] built pyramid %dx%d mips=%d (count=%llu)\n",
                    hzbW_, hzbH_, hzbMipCount_, (unsigned long long)hzbBuildCount_);
    }
}

void gosPostProcess::runHzbProbe()
{
    ZoneScopedN("Render.HZBProbe");

    // HZB-OCCLUSION-PROBE-1: DIAGNOSTIC ONLY. Reads back a parent HZB level and
    // its child level and (a) checks parent == MIN(children) -- the reverse-Z
    // reduction invariant -- and (b) runs the real conservative cull comparison
    // childDepth < parentDepth, which must be wouldKeep for every self-point
    // (a texel inside a tile can never be culled by that tile's min). This
    // exercises the exact cull math + the pyramid before real object bounds are
    // wired (next slice), with ZERO effect on rendering: read-only, no draw
    // suppression. neverAppliedToDraws is always true.
    if (!hzbProbeEnabled_) return;
    if (hzbMipCount_ < 2) return;
    if (hzbBuildCount_ == 0) return;        // need a built pyramid

    // Choose a small parent level (dims <= 64) so the readback is cheap.
    int pLevel = 1;
    {
        int dw = hzbW_, dh = hzbH_, lvl = 0;
        while (lvl < hzbMipCount_ - 1 && (dw > 64 || dh > 64)) {
            dw = (dw + 1) / 2; dh = (dh + 1) / 2; ++lvl;
        }
        pLevel = (lvl < 1) ? 1 : lvl;        // parent must have a child (>=1)
    }
    const int cLevel = pLevel - 1;

    auto dimsAt = [&](int level, int& w, int& h) {
        w = hzbW_; h = hzbH_;
        for (int k = 0; k < level; ++k) { w = (w + 1) / 2; h = (h + 1) / 2; }
        if (w < 1) w = 1; if (h < 1) h = 1;
    };
    int pw, ph, cw, ch;
    dimsAt(pLevel, pw, ph);
    dimsAt(cLevel, cw, ch);

    // Cost-bound readback: only levels at/under 256 px on the long axis (Lmin).
    // Both self-test levels (cLevel/pLevel) are coarser than Lmin, and object
    // LOD selection is CLAMPED to >= Lmin (clamping coarser only ever makes the
    // test MORE conservative -- a bigger tile MIN is smaller, so it keeps more).
    int Lmin = 0;
    {
        int dw = hzbW_, dh = hzbH_, lvl = 0;
        while (lvl < hzbMipCount_ - 1 && (dw > 256 || dh > 256)) {
            dw = (dw + 1) / 2; dh = (dh + 1) / 2; ++lvl;
        }
        Lmin = lvl;
    }
    if (Lmin > cLevel) Lmin = cLevel;   // guarantee the self-test levels are resident

    std::vector<std::vector<float>> hzbCpu(hzbMipCount_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    for (int L = Lmin; L < hzbMipCount_; ++L) {
        int lw, lh; dimsAt(L, lw, lh);
        hzbCpu[L].resize((size_t)lw * lh);
        glBindTexture(GL_TEXTURE_2D, hzbLevelTex_[L]);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, hzbCpu[L].data());
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    const std::vector<float>& parent = hzbCpu[pLevel];
    const std::vector<float>& child  = hzbCpu[cLevel];

    const float eps = 1e-5f;
    unsigned tested = 0, wouldKeep = 0, wouldCull = 0;
    unsigned integrityMismatch = 0, invalidDepth = 0;

    // Parent-centric, using the EXACT footprint hzb_reduce.frag samples: for
    // parent texel (px,py) the shader reads child texels at
    //   floor( ((px+0.5)/pw)*cw +/- 0.5 ),  floor( ((py+0.5)/ph)*ch +/- 0.5 )
    // (NEAREST + clamp). On odd child extents these 2x2 windows overlap at the
    // boundary -- which is why a naive px*2 / cx/2 inverse mapping produces
    // false mismatches. Replicating the sample positions exactly makes the
    // integrity + cull self-tests agree with the GPU when the pyramid is sound.
    auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    for (int py = 0; py < ph; ++py) {
        const float sv = ((py + 0.5f) / (float)ph) * (float)ch;
        const int cy0 = clampi((int)std::floor(sv - 0.5f), 0, ch - 1);
        const int cy1 = clampi((int)std::floor(sv + 0.5f), 0, ch - 1);
        for (int px = 0; px < pw; ++px) {
            const float su = ((px + 0.5f) / (float)pw) * (float)cw;
            const int cx0 = clampi((int)std::floor(su - 0.5f), 0, cw - 1);
            const int cx1 = clampi((int)std::floor(su + 0.5f), 0, cw - 1);

            const float ca = child[(size_t)cy0 * cw + cx0];
            const float cb = child[(size_t)cy0 * cw + cx1];
            const float cc = child[(size_t)cy1 * cw + cx0];
            const float cd = child[(size_t)cy1 * cw + cx1];
            float m = ca;
            if (cb < m) m = cb;
            if (cc < m) m = cc;
            if (cd < m) m = cd;

            const float pv = parent[(size_t)py * pw + px];
            if (!std::isfinite(pv) || !std::isfinite(m)) { ++invalidDepth; continue; }

            // (a) reduction integrity: GPU parent must equal the CPU min.
            if (std::fabs(pv - m) > eps) ++integrityMismatch;

            // (b) conservative cull self-test: each child the parent covers is a
            // self-point inside the tile, so its depth must be >= parent's MIN
            // (reverse-Z) -> never culled. wouldCull MUST stay 0.
            const float kids[4] = { ca, cb, cc, cd };
            for (float d : kids) {
                ++tested;
                if (d < pv - eps) ++wouldCull; else ++wouldKeep;
            }
        }
    }

    // ---- Real static-prop occlusion probe (HZB-STATICPROP-CULL-RECON-1) ----
    // For each active static prop: build an AABB from its world center +/- the
    // extent radius, project the 8 corners through viewProj_ (the GL-NDC
    // reverse-Z world->clip transform that produced the scene depth, fed by
    // Camera::worldToClipGL), take the screen rect + the object's CLOSEST
    // reverse-Z depth (max over corners), pick the HZB LOD matching the rect
    // size, sample that level's covered texels (MIN = farthest occluder), and
    // run the conservative cull comparison objClosest < hzbMin. DIAGNOSTIC ONLY
    // -- counts would-cull/would-keep; NEVER suppresses a draw. Real props can be
    // genuinely occluded, so objWouldCull > 0 is expected; the safety invariant
    // is only that nothing acts on it. objOnScreen ~ 0 would mean the projection
    // convention is wrong (axis-swap) -- a loud red flag, not a silent failure.
    // ---- HZB-CAMERA-DISCONTINUITY-GUARD-1 ----------------------------------
    // Derive the camera pose this frame by unprojecting the NDC near/far centers
    // through inverseViewProj_ (set every frame alongside viewProj_, BEFORE the
    // scene depth render -> same-frame coherent with the HZB-source depth). A
    // near-instant pose change (e.g. mc2_17's intro 180deg snaps) makes a single
    // frame's screen-space occlusion test unreliable; we FLAG such frames as
    // unsafe-for-cull and split the raw vs guarded would-cull counts. This is
    // DIAGNOSTIC: it changes no rendering and suppresses no draw.
    auto unproject = [&](float nx, float ny, float nz, float out[3]) {
        const float v[4] = { nx, ny, nz, 1.0f };
        float w[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                w[r] += inverseViewProj_[c * 4 + r] * v[c];
        const float iw = (std::fabs(w[3]) > 1e-12f) ? 1.0f / w[3] : 0.0f;
        out[0] = w[0] * iw; out[1] = w[1] * iw; out[2] = w[2] * iw;
    };
    float camPos[3], camFar[3], camFwd[3] = { 0.0f, 0.0f, 0.0f };
    unproject(0.0f, 0.0f, 1.0f, camPos);   // reverse-Z near center (z=1)
    unproject(0.0f, 0.0f, 0.0f, camFar);   // far center (z=0)
    {
        float dx = camFar[0] - camPos[0], dy = camFar[1] - camPos[1], dz = camFar[2] - camPos[2];
        float fl = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (fl > 1e-6f) { camFwd[0] = dx / fl; camFwd[1] = dy / fl; camFwd[2] = dz / fl; }
    }
    bool  unsafeForCull = false;
    float camFwdAngleDeg = 0.0f, camPosDelta = 0.0f;
    if (hzbPrevCamValid_) {
        float px = camPos[0] - hzbPrevCamPos_[0], py = camPos[1] - hzbPrevCamPos_[1], pz = camPos[2] - hzbPrevCamPos_[2];
        camPosDelta = std::sqrt(px * px + py * py + pz * pz);
        float dot = camFwd[0] * hzbPrevCamFwd_[0] + camFwd[1] * hzbPrevCamFwd_[1] + camFwd[2] * hzbPrevCamFwd_[2];
        if (dot > 1.0f) dot = 1.0f; if (dot < -1.0f) dot = -1.0f;
        camFwdAngleDeg = std::acos(dot) * (180.0f / 3.14159265f);
        // Thresholds: a smooth pan is a few deg/frame; a 180deg snap is unmistakable.
        // Position guard only when the map extent is known (one-frame teleport).
        const float kAngleThreshDeg = 30.0f;
        const float kPosThresh = (mapHalfExtent_ > 0.0f) ? (0.25f * mapHalfExtent_) : 1e30f;
        unsafeForCull = (camFwdAngleDeg > kAngleThreshDeg) || (camPosDelta > kPosThresh);
    }
    if (unsafeForCull) ++hzbCamDiscontinuityFrames_;
    hzbPrevCamPos_[0] = camPos[0]; hzbPrevCamPos_[1] = camPos[1]; hzbPrevCamPos_[2] = camPos[2];
    hzbPrevCamFwd_[0] = camFwd[0]; hzbPrevCamFwd_[1] = camFwd[1]; hzbPrevCamFwd_[2] = camFwd[2];
    hzbPrevCamValid_ = true;

    // ---- Real static-prop occlusion probe (HZB-STATICPROP-CULL-RECON-1 +
    //      HZB-CULL-READINESS-COUNTERS-1) -- DIAGNOSTIC ONLY, neverAppliedToDraws.
    // objWouldCullRaw            = raw conservative cull decisions (always counted)
    // objWouldCullGuarded        = raw culls on SAFE (non-discontinuous) frames
    // objSkippedCameraDiscont    = raw culls suppressed because the frame is unsafe
    //   (objWouldCullRaw == objWouldCullGuarded + objSkippedCameraDiscont)
    unsigned objActive = 0, objScanned = 0, objTested = 0;
    unsigned objWouldKeep = 0, objWouldCullRaw = 0, objWouldCullGuarded = 0;
    unsigned objSkippedCameraDiscont = 0, objOffscreen = 0, objNearClippedKeep = 0;
    unsigned objInvalidRect = 0;
    unsigned lodHist[kHzbMaxLevels] = { 0 };

    // HZB-CULL-MARGIN-SWEEP-1: how many GUARDED would-cull candidates survive
    // increasingly conservative depth-gap margins. gap = objClosest - hzbMin
    // (reverse-Z: gap < 0 == behind the occluder). A real cull path should
    // require gap < -margin, not just gap < 0; this quantifies the marginal
    // (near-zero) candidates so we can pick that margin. Accumulated over SAFE
    // (non-discontinuous) frames only, matching the guarded semantics.
    static const float kMargins[] = { 0.00000f, 0.00005f, 0.00010f, 0.00025f, 0.00050f, 0.00100f };
    const int kNumMargins = (int)(sizeof(kMargins) / sizeof(kMargins[0]));
    unsigned objWouldCullAtMargin[6] = { 0 };   // size matches kMargins
    float minGap = 1e9f, maxGap = -1e9f, closestToZeroNegGap = -1e9f;
    unsigned numMarginalCandidates = 0;          // -0.00010 < gap < 0

    // Bounded sample of the GUARDED would-cull candidates CLOSEST to zero (the
    // most marginal / highest false-positive risk), so a human can verify they
    // are truly hidden rather than grazing false positives.
    struct CullCand { int idx; float r0, r1, r2, r3, closest, hmin, gap; int lod; };
    const int kCandMax = 8;
    CullCand cand[kCandMax];
    int candCount = 0;
    {
        auto projectClip = [&](float wx, float wy, float wz, float out[4]) {
            const float w4[4] = { wx, wy, wz, 1.0f };
            float clip[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    clip[r] += viewProj_[c * 4 + r] * w4[c];
            out[0] = clip[0]; out[1] = clip[1]; out[2] = clip[2]; out[3] = clip[3];
        };

        objActive = GpuStaticPropRegistry::getActiveCount();
        const int      kScanCap = 1 << 16;   // backstop vs sparse indices
        const unsigned kObjCap  = 4096;       // bound per-frame cost
        for (int idx = 0; idx < kScanCap && objScanned < objActive && objScanned < kObjCap; ++idx) {
            float mtx[16];
            if (!GpuStaticPropRegistry::staticPropGetModelMatrix(idx, mtx)) continue; // tombstone/gap
            ++objScanned;

            float radius = 0.0f;
            GpuStaticPropRegistry::staticPropGetExtentRadius(idx, &radius);
            if (!(radius > 0.0f)) radius = 1.0f;
            const float cx = -mtx[3], cy = mtx[11], cz = mtx[7];  // MC2 east/north/elev

            float uvMinX = 1e9f, uvMaxX = -1e9f, uvMinY = 1e9f, uvMaxY = -1e9f;
            float objClosest = -1e9f;  // reverse-Z: closest = MAX depth
            bool crossesNear = false;
            for (int ci = 0; ci < 8; ++ci) {
                const float wx = cx + ((ci & 1) ? radius : -radius);
                const float wy = cy + ((ci & 2) ? radius : -radius);
                const float wz = cz + ((ci & 4) ? radius : -radius);
                float clip[4];
                projectClip(wx, wy, wz, clip);
                if (clip[3] <= 1e-6f) { crossesNear = true; break; }
                const float inv = 1.0f / clip[3];
                const float ndcx = clip[0] * inv, ndcy = clip[1] * inv;
                const float depth = clip[2] * inv;            // reverse-Z [0,1]
                const float ux = ndcx * 0.5f + 0.5f, uy = ndcy * 0.5f + 0.5f;
                if (ux < uvMinX) uvMinX = ux; if (ux > uvMaxX) uvMaxX = ux;
                if (uy < uvMinY) uvMinY = uy; if (uy > uvMaxY) uvMaxY = uy;
                if (depth > objClosest) objClosest = depth;
            }
            if (crossesNear) { ++objNearClippedKeep; continue; }   // conservative: keep
            if (uvMaxX < 0.0f || uvMinX > 1.0f || uvMaxY < 0.0f || uvMinY > 1.0f) {
                ++objOffscreen; continue;
            }

            // Clamp rect to screen, pick LOD from its pixel size.
            const float cMinX = uvMinX < 0.0f ? 0.0f : uvMinX;
            const float cMaxX = uvMaxX > 1.0f ? 1.0f : uvMaxX;
            const float cMinY = uvMinY < 0.0f ? 0.0f : uvMinY;
            const float cMaxY = uvMaxY > 1.0f ? 1.0f : uvMaxY;
            float rpw = (cMaxX - cMinX) * (float)hzbW_;
            float rph = (cMaxY - cMinY) * (float)hzbH_;
            if (rpw < 1.0f) rpw = 1.0f; if (rph < 1.0f) rph = 1.0f;
            int L = (int)std::ceil(std::log2((rpw > rph ? rpw : rph)));
            if (L < Lmin) L = Lmin;
            if (L > hzbMipCount_ - 1) L = hzbMipCount_ - 1;

            int lw, lh; dimsAt(L, lw, lh);
            const std::vector<float>& lvl = hzbCpu[L];
            int tx0 = clampi((int)std::floor(cMinX * lw), 0, lw - 1);
            int tx1 = clampi((int)std::floor(cMaxX * lw), 0, lw - 1);
            int ty0 = clampi((int)std::floor(cMinY * lh), 0, lh - 1);
            int ty1 = clampi((int)std::floor(cMaxY * lh), 0, lh - 1);
            if (tx1 - tx0 > 3) tx1 = tx0 + 3;                  // bound the inner loop
            if (ty1 - ty0 > 3) ty1 = ty0 + 3;
            float hzbMin = 1e9f;
            for (int ty = ty0; ty <= ty1; ++ty)
                for (int tx = tx0; tx <= tx1; ++tx) {
                    const float d = lvl[(size_t)ty * lw + tx];
                    if (d < hzbMin) hzbMin = d;
                }
            if (!std::isfinite(hzbMin) || !std::isfinite(objClosest)) { ++objInvalidRect; continue; }

            ++objTested;
            if (L >= 0 && L < kHzbMaxLevels) ++lodHist[L];
            const float gap = objClosest - hzbMin;   // reverse-Z: <0 == behind

            // Margin sweep + gap stats over SAFE frames only (guarded semantics).
            if (!unsafeForCull) {
                if (gap < minGap) minGap = gap;
                if (gap > maxGap) maxGap = gap;
                if (gap < 0.0f && gap > closestToZeroNegGap) closestToZeroNegGap = gap;
                if (gap > -0.00010f && gap < 0.0f) ++numMarginalCandidates;
                for (int mi = 0; mi < kNumMargins; ++mi)
                    if (gap < -kMargins[mi]) ++objWouldCullAtMargin[mi];
            }

            // Conservative reverse-Z cull: object's nearest point is behind the
            // farthest occluder in its footprint -> fully occluded.
            if (objClosest < hzbMin - eps) {
                ++objWouldCullRaw;
                if (unsafeForCull) {
                    ++objSkippedCameraDiscont;       // guard suppresses this frame
                } else {
                    ++objWouldCullGuarded;
                    // Keep the kCandMax candidates with gap CLOSEST to zero (most
                    // marginal). If full, replace the least-marginal (smallest gap).
                    if (candCount < kCandMax) {
                        cand[candCount] = { idx, cMinX, cMinY, cMaxX, cMaxY,
                                            objClosest, hzbMin, gap, L };
                        ++candCount;
                    } else {
                        int worst = 0;
                        for (int k = 1; k < kCandMax; ++k)
                            if (cand[k].gap < cand[worst].gap) worst = k;  // most negative
                        if (gap > cand[worst].gap)
                            cand[worst] = { idx, cMinX, cMinY, cMaxX, cMaxY,
                                            objClosest, hzbMin, gap, L };
                    }
                }
            } else {
                ++objWouldKeep;
            }
        }
    }

    static unsigned long long s_probeFrame = 0;
    ++s_probeFrame;
    const bool logTick = (s_probeFrame <= 5 || (s_probeFrame % 600) == 0);

    if (s_probeFrame <= 3 || (s_probeFrame % 600) == 0 || wouldCull || integrityMismatch) {
        fprintf(stderr,
            "[HZB_PROBE v1] parentL=%d(%dx%d) childL=%d(%dx%d) tested=%u "
            "wouldKeep=%u wouldCull=%u integrityMismatch=%u invalidDepth=%u "
            "neverAppliedToDraws=1\n",
            pLevel, pw, ph, cLevel, cw, ch, tested,
            wouldKeep, wouldCull, integrityMismatch, invalidDepth);
    }
    // Log object readiness counters on the tick OR on any unsafe (discontinuity)
    // frame, so camera snaps are always visible in the trace.
    if (logTick || unsafeForCull) {
        fprintf(stderr,
            "[HZB_PROBE_OBJ v2] staticProps active=%u scanned=%u tested=%u "
            "wouldKeep=%u wouldCullRaw=%u wouldCullGuarded=%u "
            "skippedCameraDiscont=%u nearClippedKeep=%u offscreen=%u "
            "invalidRect=%u Lmin=%d cameraDiscontinuity=%d fwdAngleDeg=%.1f "
            "posDelta=%.1f discontFramesCumulative=%llu unsafeForCull=%d "
            "neverAppliedToDraws=1\n",
            objActive, objScanned, objTested, objWouldKeep, objWouldCullRaw,
            objWouldCullGuarded, objSkippedCameraDiscont, objNearClippedKeep,
            objOffscreen, objInvalidRect, Lmin, unsafeForCull ? 1 : 0,
            camFwdAngleDeg, camPosDelta,
            (unsigned long long)hzbCamDiscontinuityFrames_, unsafeForCull ? 1 : 0);
    }
    if (logTick && objTested > 0) {
        char hist[256]; int n = 0;
        n += snprintf(hist + n, sizeof(hist) - n, "[HZB_PROBE_LOD v1] selectedLod");
        for (int L = 0; L < hzbMipCount_ && L < kHzbMaxLevels && n < (int)sizeof(hist) - 16; ++L)
            if (lodHist[L]) n += snprintf(hist + n, sizeof(hist) - n, " L%d=%u", L, lodHist[L]);
        fprintf(stderr, "%s\n", hist);
    }
    // Depth-gap margin sweep (safe-frame guarded candidates). guard@0.00000
    // equals wouldCullGuarded by gap<0; higher margins show how many survive a
    // more conservative gap<-margin requirement.
    if (logTick && objTested > 0) {
        fprintf(stderr,
            "[HZB_PROBE_MARGIN v1] guardCull@{0=%u,5e-5=%u,1e-4=%u,2.5e-4=%u,"
            "5e-4=%u,1e-3=%u} minGap=%.6f maxGap=%.6f closestToZeroNegGap=%.6f "
            "numMarginal(-1e-4<gap<0)=%u neverAppliedToDraws=1\n",
            objWouldCullAtMargin[0], objWouldCullAtMargin[1], objWouldCullAtMargin[2],
            objWouldCullAtMargin[3], objWouldCullAtMargin[4], objWouldCullAtMargin[5],
            (minGap <= 1e8f ? minGap : 0.0f),
            (maxGap >= -1e8f ? maxGap : 0.0f),
            (closestToZeroNegGap >= -1e8f ? closestToZeroNegGap : 0.0f),
            numMarginalCandidates);
    }
    // Bounded sample of guarded would-cull candidates CLOSEST to zero (safe
    // frames only) -- the highest false-positive risk to eyeball.
    if (logTick && candCount > 0) {
        for (int i = 0; i < candCount; ++i) {
            const CullCand& c = cand[i];
            fprintf(stderr,
                "[HZB_PROBE_CULLCAND v1] idx=%d rectUV=(%.3f,%.3f,%.3f,%.3f) "
                "objClosest=%.5f hzbMin=%.5f gap=%.6f L=%d neverAppliedToDraws=1\n",
                c.idx, c.r0, c.r1, c.r2, c.r3, c.closest, c.hmin, c.gap, c.lod);
        }
    }
}

void gosPostProcess::runSSAO()
{
    ZoneScopedN("Render.SSAO");
    TracyGpuZone("Render.SSAO");

    // SSAO-GTAO-LITE-MVP-1: master gate + in-mission guard (matches screen
    // shadow: only run when terrain was drawn, i.e. not in menus).
    if (!ssaoEnabled_) return;
    if (!sceneHasTerrain_) return;
    if (!ssaoProg_ || !ssaoProg_->is_valid()) return;
    if (!ssaoApplyProg_ || !ssaoApplyProg_->is_valid()) return;
    if (!ssaoFBO_ || !ssaoColorTex_) return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    // --- Pass 1: compute AO at half resolution ---
    glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO_);
    glViewport(0, 0, ssaoW_, ssaoH_);
    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT);

    ssaoProg_->setInt("sceneDepthTex", 0);
    ssaoProg_->setInt("sceneNormalTex", 1);
    float ss[2] = { (float)width_, (float)height_ };
    ssaoProg_->setFloat2("screenSize", ss);
    ssaoProg_->setFloat("aoRadius", aoRadius_);
    ssaoProg_->setFloat("aoBias", aoBias_);
    ssaoProg_->setFloat("aoStrength", aoStrength_);
    ssaoProg_->setFloat("aoPower", aoPower_);
    ssaoProg_->apply();
    // Matrices via raw upload while bound (GL_FALSE matches the proven
    // inverseViewProj path in runScreenShadow; viewProj is its forward mate).
    GLint locInv = glGetUniformLocation(ssaoProg_->shp_, "inverseViewProj");
    if (locInv >= 0) glUniformMatrix4fv(locInv, 1, GL_FALSE, inverseViewProj_);
    GLint locVP = glGetUniformLocation(ssaoProg_->shp_, "viewProj");
    if (locVP >= 0) glUniformMatrix4fv(locVP, 1, GL_FALSE, viewProj_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sceneNormalTex_);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // --- Pass 2: apply AO multiplicatively into the scene color ---
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);

    // BLENDMODE-MULTIPLY-1: PostProcessSsaoApply row (Multiply = DST_COLOR/ZERO,
    // scene *= ao). depth test+write OFF / cull None re-asserted (already set by SSAO
    // pass 1; no-op). Byte-identical in the default path; the debug override
    // (ssaoDebug_!=0 overwrites scene with AO grayscale) is preserved explicitly.
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::PostProcessSsaoApply),
        "PostProcessSsaoApply");
    if (ssaoDebug_ != 0) glDisable(GL_BLEND);
    render_frame_plan::trace(render_frame_plan::Phase::PostProcess, "SsaoApply",
        render_frame_plan::PathKind::ApplyPipeline, 1, "PostProcessSsaoApply");

    ssaoApplyProg_->setInt("ssaoTex", 0);
    float texel[2] = { 1.0f / (float)ssaoW_, 1.0f / (float)ssaoH_ };
    ssaoApplyProg_->setFloat2("ssaoTexel", texel);
    ssaoApplyProg_->setInt("debugMode", ssaoDebug_);
    ssaoApplyProg_->apply();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ssaoColorTex_);

    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
}

void gosPostProcess::clearGBuffer1()
{
    if (!sceneNormalTex_) return;
    // Sentinel: flat-up encoded normal (0.5, 0.5, 1.0), alpha=0.0
    // (post-shadow eligible). Matches rc_gbuffer1_screenShadowEligible(vec3(0,0,1))
    // in shaders/include/render_contract.hglsl.
    static const GLfloat sentinel[4] = { 0.5f, 0.5f, 1.0f, 0.0f };
    // glClearBufferfv with buffer=GL_COLOR, drawbuffer=1 clears the
    // SECOND draw buffer of the currently bound FBO. Because beginScene()
    // calls glDrawBuffers(2, {COLOR0, COLOR1}), drawbuffer index 1 maps
    // to GL_COLOR_ATTACHMENT1 here. Caller must ensure MRT is bound.
    glClearBufferfv(GL_COLOR, 1, sentinel);
}


void gosPostProcess::runScreenShadow()
{
    ZoneScopedN("Render.ScreenShadow");
    TracyGpuZone("Render.ScreenShadow");

    if (!screenShadowEnabled_) return;
    if (!sceneHasTerrain_) return;
    if (!screenShadowProg_ || !screenShadowProg_->is_valid()) return;
    if (!shadowsEnabled_) return;

    // APPLY-STATE-SCREENSHADOW-1: skip the 4 setup calls when the executor already
    // applied them (MC2_FRAMEGRAPH_EXECUTOR ON). Reset one-shot before draw.
    if (!screenShadowStateAppliedByExecutor_) {
        // Render to sceneFBO_ color-only (no normal write) with multiplicative blending
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
        // M1.5: single-color composite. Helper preserves env-OFF/ON parity
        // (the postprocess composite never writes attachment-2 regardless).
        setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
        glViewport(0, 0, width_, height_);

        // BLENDMODE-MULTIPLY-1: drive FF state from the PostProcessScreenShadow row
        // (Multiply = DST_COLOR/ZERO scene-darkening, depth test+write OFF, cull None).
        // Byte-identical in the default path. The debug override (screenShadowDebug_!=0
        // overwrites scene color) is preserved explicitly — applyPipeline sets Multiply,
        // then debug disables blend.
        pipeline_binder::applyPipeline(
            RenderCore::getPipelineDesc(RenderCore::PipelineId::PostProcessScreenShadow),
            "PostProcessScreenShadow");
    }
    screenShadowStateAppliedByExecutor_ = false;
    // Debug override stays body-owned (depends on applyPipeline having set Multiply,
    // which the executor also did). Runs regardless of who applied the pipeline.
    if (screenShadowDebug_ != 0) glDisable(GL_BLEND);
    render_frame_plan::trace(render_frame_plan::Phase::PostProcess, "ScreenShadow",
        render_frame_plan::PathKind::ApplyPipeline, 1, "PostProcessScreenShadow");

    // Set uniforms BEFORE apply()
    const bool csmActive = (mc2ShadowCsmEnabled() && dynShadowArrayTex_ != 0);
    screenShadowProg_->setInt("sceneDepthTex", 0);
    screenShadowProg_->setInt("sceneNormalTex", 1);
    screenShadowProg_->setInt("shadowMap", 2);
    if (csmActive) {
        screenShadowProg_->setInt("dynamicShadowArray", 3);
        // Per-cascade shadow resolution: separate full-map (last) cascade on unit 4.
        screenShadowProg_->setInt("dynamicFullMapShadow", 4);
    } else {
        screenShadowProg_->setInt("dynamicShadowMap", 3);
    }
    screenShadowProg_->setInt("overlayPass", 0);
    screenShadowProg_->setInt("enableShadows", shadowsEnabled_ ? 1 : 0);
    screenShadowProg_->setInt("enableDynamicShadows",
                              (csmActive || dynShadowDepthTex_ != 0) ? 1 : 0);
    if (csmActive)
        screenShadowProg_->setInt("dynamicCsmCount", csmCount_);
    screenShadowProg_->setFloat("shadowSoftness", mc2ShadowCsmSoftness());  // match terrain default
    screenShadowProg_->setFloat("objNormalBiasScale", mc2ShadowObjNormalBias());
    screenShadowProg_->setFloat("mechSoft", mc2ShadowMechSoft());
    // PROP-SHADOW-RECEIVE-1: default 0 (gate OFF) -> shader prop branch inert.
    screenShadowProg_->setInt("propShadowReceive", mc2PropShadowReceive());
    {
        // SCREEN-SHADOW-LIGHTDIR-FRAME-FIX: the back-face guard + normal-offset
        // dot objN against lightDir. objN in shadow_screen.frag is the SAME
        // (-x,z,y) swap of the Stuff GBuffer normal that static_prop's diffuse
        // uses as worldNormalMC2 — and the diffuse dots that against the RAW
        // gos_GetTerrainLightDir (toward-sun) and is correct. So lightDir here
        // must be the RAW terrain sun too (no negate, no extra swap), or the
        // guard lands the self-shadow on the wrong (90deg-off) faces.
        float lx = 0.0f, ly = 0.0f, lz = 1.0f;
        gos_GetTerrainLightDir(&lx, &ly, &lz);
        float len = sqrtf(lx*lx + ly*ly + lz*lz);
        if (len > 1e-6f) { lx /= len; ly /= len; lz /= len; }
        // negate: was 180deg off with the raw vector -> flip sign (frame already correct).
        float lightDirVec[3] = { -lx, -ly, -lz };    // matches objN frame, correct sign
        screenShadowProg_->setFloat3("lightDir", lightDirVec);
    }
    screenShadowProg_->setInt("debugMode", screenShadowDebug_);
    float screenSz[2] = { (float)width_, (float)height_ };
    screenShadowProg_->setFloat2("screenSize", screenSz);
    screenShadowProg_->setFloat("time", SmokeMode::fixedTimestepEnabled()
                                            ? (float)SmokeMode::fixedClockSeconds()
                                            : (float)SDL_GetTicks() * 0.001f);
    screenShadowProg_->apply();

    // Upload matrices via direct GL (after apply binds the program)
    GLint loc;
    loc = glGetUniformLocation(screenShadowProg_->shp_, "inverseViewProj");
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, inverseViewProj_);
    loc = glGetUniformLocation(screenShadowProg_->shp_, "lightSpaceMatrix");
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, staticLightSpaceMatrix_);
    if (csmActive) {
        loc = glGetUniformLocation(screenShadowProg_->shp_, "dynamicCascadeMatrices");
        if (loc >= 0) glUniformMatrix4fv(loc, csmCount_, GL_FALSE, dynamicCascadeMatrices_);
        // Stage 3: per-cascade texel-scaled depth bias inputs.
        loc = glGetUniformLocation(screenShadowProg_->shp_, "dynamicCascadeTexelWorld");
        if (loc >= 0) glUniform1fv(loc, csmCount_, dynamicCascadeTexelWorld_);
        loc = glGetUniformLocation(screenShadowProg_->shp_, "csmDepthSpan");
        if (loc >= 0) glUniform1f(loc, csmDepthSpan_);
        // Per-cascade shadow resolution: separate full-map (last) cascade texel.
        loc = glGetUniformLocation(screenShadowProg_->shp_, "dynamicFullMapTexelWorld");
        if (loc >= 0) glUniform1f(loc, dynamicFullMapTexelWorld_);
    } else {
        loc = glGetUniformLocation(screenShadowProg_->shp_, "dynamicLightSpaceMatrix");
        if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, dynamicLightSpaceMatrix_);
    }

    // Bind textures
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sceneNormalTex_);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, shadowDepthTex_);
    glActiveTexture(GL_TEXTURE3);
    if (csmActive)
        glBindTexture(GL_TEXTURE_2D_ARRAY, dynShadowArrayTex_);
    else
        glBindTexture(GL_TEXTURE_2D, dynShadowDepthTex_);
    if (csmActive) {
        // Per-cascade shadow resolution: separate full-map (last) cascade, unit 4.
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, dynamicFullMapTex_);
    }

    // Draw fullscreen quad — pass 1: normal (skip terrain)
    // Draw fullscreen quad - single pass for terrain, objects, and overlays.
    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindVertexArray(0);

    // Restore state
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    // SCREENSHADOW-TEX-RESTORE-1: unbind the CSM cascade 2D_ARRAY left on unit 3
    // (csmActive path binds GL_TEXTURE_2D_ARRAY on unit 3; nothing unbinds it at exit,
    // blocking executor ownership of this pass). Mirror GLSTATE-SHADOWDEBUG-2DARRAY-1.
    if (csmActive) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    }
    glActiveTexture(GL_TEXTURE0);
}

// Fullscreen procedural cloud-shadow pass. Replaces the four inline cloud
// blocks (gos_terrain / terrain_overlay / decal / shadow_screen). Runs AFTER
// runScreenShadow; multiplicative (GL_DST_COLOR, GL_ZERO) onto the scene color.
// Screen-space UV (TexCoord) — no world-position reconstruction or inverseViewProj needed.
void gosPostProcess::runCloudShadow()
{
    ZoneScopedN("Render.CloudShadow");
    TracyGpuZone("Render.CloudShadow");

    static const bool s_cloudDiag = (getenv("MC2_CLOUD_DIAG") != nullptr);
    static int s_cloudDiagFrame = 0;
    const bool cloudDiagNow = s_cloudDiag && ((s_cloudDiagFrame++ % 120) == 0);
    if (cloudDiagNow)
        fprintf(stderr, "[CLOUD_DIAG] runCloudShadow called enable=%d hasTerrain=%d progValid=%d\n",
                (int)enableCloudShadow_, (int)sceneHasTerrain_,
                (int)(cloudProg_ && cloudProg_->is_valid()));

    if (!enableCloudShadow_) {                     // env/ImGui gate -> skip entirely
        if (cloudDiagNow) fprintf(stderr, "[CLOUD_DIAG] bail: !enableCloudShadow_\n");
        return;
    }
    if (!sceneHasTerrain_) {
        if (cloudDiagNow) fprintf(stderr, "[CLOUD_DIAG] bail: !sceneHasTerrain_\n");
        return;
    }
    if (!cloudProg_ || !cloudProg_->is_valid()) {
        if (cloudDiagNow) fprintf(stderr, "[CLOUD_DIAG] bail: prog invalid\n");
        return;
    }

    // APPLY-STATE-REDUNDANT-BODY-REMOVE-2: skip the 4 setup calls when the executor
    // already applied them (MC2_FRAMEGRAPH_EXECUTOR ON). Reset one-shot before draw.
    if (!cloudShadowStateAppliedByExecutor_) {
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
        setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
        glViewport(0, 0, width_, height_);

        // BLENDMODE-MULTIPLY-1: PostProcessCloudShadow row (Multiply = DST_COLOR/ZERO
        // cloud darkening, depth test+write OFF, cull None). Byte-identical to hand-set.
        pipeline_binder::applyPipeline(
            RenderCore::getPipelineDesc(RenderCore::PipelineId::PostProcessCloudShadow),
            "PostProcessCloudShadow");
    }
    cloudShadowStateAppliedByExecutor_ = false;
    render_frame_plan::trace(render_frame_plan::Phase::PostProcess, "CloudShadow",
        render_frame_plan::PathKind::ApplyPipeline, 1, "PostProcessCloudShadow");

    cloudProg_->setInt("sceneDepthTex", 0);
    cloudProg_->setInt("u_cloudEnable", 1);   // gated in C++ above; 1 inside the pass
    cloudProg_->setFloat("u_time", SmokeMode::fixedTimestepEnabled()
                                       ? (float)SmokeMode::fixedClockSeconds()
                                       : (float)SDL_GetTicks() * 0.001f);
    cloudProg_->setFloat("u_cloudScale", cloudScale_);
    float scroll[2] = { cloudScrollX_, cloudScrollY_ };
    cloudProg_->setFloat2("u_cloudScroll", scroll);
    cloudProg_->setFloat("u_cloudStrength", cloudStrength_);
    float thresh[2] = { cloudThreshLo_, cloudThreshHi_ };
    cloudProg_->setFloat2("u_cloudThreshold", thresh);
    cloudProg_->setInt("u_cloudOctaves", cloudOctaves_);
    cloudProg_->apply();

    GLint loc = glGetUniformLocation(cloudProg_->shp_, "inverseViewProj");
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, inverseViewProj_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Restore state.
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);

    if (cloudDiagNow)
        fprintf(stderr, "[CLOUD_DIAG] cloud pass DREW (fbo=%u blend on)\n",
                (unsigned)sceneFBO_);
}

void gosPostProcess::runShoreline()
{
    ZoneScopedN("Render.Shoreline");
    TracyGpuZone("Render.Shoreline");

    // TERRAIN-SHORELINE-MASK-1: yield to the terrain-side wet/foam band when
    // active (recon landmine #6 "double-shore" -- avoid brightening the seam
    // twice). Default false -> byte-identical to the pre-slice gate.
    if (!shorelineEnabled_ || !sceneHasTerrain_ || !shorelineProg_ || !shorelineProg_->is_valid()
        || shorelineSuppressedByTerrainMask_) return;

    // APPLY-STATE-REDUNDANT-BODY-REMOVE-2: skip the 4 setup calls when the executor
    // already applied them (MC2_FRAMEGRAPH_EXECUTOR ON). Reset one-shot before draw.
    if (!shorelineStateAppliedByExecutor_) {
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
        // M1.5: single-color multiplicative composite; helper preserves env shape.
        setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
        glViewport(0, 0, width_, height_);

        // BLENDMODE-MULTIPLY-1: PostProcessShoreline row (Multiply = DST_COLOR/ZERO;
        // mask values >1 brighten water at the shoreline). depth test+write OFF, cull
        // None. Byte-identical to the hand-set state.
        pipeline_binder::applyPipeline(
            RenderCore::getPipelineDesc(RenderCore::PipelineId::PostProcessShoreline),
            "PostProcessShoreline");
    }
    shorelineStateAppliedByExecutor_ = false;
    render_frame_plan::trace(render_frame_plan::Phase::PostProcess, "Shoreline",
        render_frame_plan::PathKind::ApplyPipeline, 1, "PostProcessShoreline");

    shorelineProg_->setInt("sceneDepthTex", 0);
    shorelineProg_->setInt("sceneNormalTex", 1);
    float screenSz[2] = { (float)width_, (float)height_ };
    shorelineProg_->setFloat2("screenSize", screenSz);
    float elapsed = SmokeMode::fixedTimestepEnabled()
                        ? (float)SmokeMode::fixedClockSeconds()
                        : (float)SDL_GetTicks() / 1000.0f;
    shorelineProg_->setFloat("time", elapsed);
    shorelineProg_->apply();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sceneNormalTex_);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
}

void gosPostProcess::runEdgeFog()
{
    ZoneScopedN("Render.EdgeFog");
    TracyGpuZone("Render.EdgeFog");

    if (!edgeFogEnabled_ || !edgeFogProg_ || !edgeFogProg_->is_valid()) return;
    if (mapHalfExtent_ <= 0.0f) return;
    // No world fog on the front-end / logistics menus: same in-mission gate the
    // sibling passes (cloud-shadow, godrays, shoreline, OOB fog) use.
    if (!sceneHasTerrain_) return;

    // APPLY-STATE-REDUNDANT-BODY-REMOVE-1: when MC2_FRAMEGRAPH_EXECUTOR is ON,
    // executorApplyEdgeFogState() already ran these 4 setup calls before us
    // (executorOwnBeginSub → executorApplyEdgeFogState → runEdgeFog).
    // Skip the body's copies so the executor is the SOLE setter (no double-set).
    // Gate OFF: flag stays false → body still sets state → byte-identical to pre-slice.
    if (!edgeFogStateAppliedByExecutor_) {
        // Bind scene FBO — writes to colour attachment 0 only.
        // Reads sceneDepthTex_ (separate attachment — no read/write conflict).
        // sceneColorTex_ is NEVER sampled here; blend equation does the composite.
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
        setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
        glViewport(0, 0, width_, height_);

        // POSTPROCESS-FOG-REGISTRATION-1: drive FF state from the PostProcessEdgeFog
        // row instead of hand-setting it. Byte-identical: depth test+write OFF, cull
        // None, AlphaBlend (SRC_ALPHA/ONE_MINUS_SRC_ALPHA). glProgramName=0 -> the
        // edgeFogProg_->apply() below still binds the program. Teardown (glDisable
        // BLEND / glDepthMask TRUE / glEnable DEPTH_TEST) stays owned by this site.
        pipeline_binder::applyPipeline(
            RenderCore::getPipelineDesc(RenderCore::PipelineId::PostProcessEdgeFog),
            "PostProcessEdgeFog");
    }
    // Reset flag: one-shot per executor-apply; must clear before draw so flag never
    // leaks to a subsequent frame or a second call in the same composite pass.
    edgeFogStateAppliedByExecutor_ = false;
    render_frame_plan::trace(render_frame_plan::Phase::PostProcess, "EdgeFog",
        render_frame_plan::PathKind::ApplyPipeline, 1, "PostProcessEdgeFog");

    // invViewProj: same GL_FALSE / row-major convention as SSAO and other passes.
    edgeFogProg_->apply();
    edgeFogProg_->setInt("depthTex", 0);
    glUniformMatrix4fv(
        glGetUniformLocation(edgeFogProg_->shp_, "invViewProj"),
        1, GL_FALSE, inverseViewProj_);
    edgeFogProg_->setFloat3("u_fogColor",        edgeFogColor_);
    edgeFogProg_->setFloat("u_halfExtent",       mapHalfExtent_);
    edgeFogProg_->setFloat("u_fogStart",         edgeFogStart_);
    edgeFogProg_->setFloat("u_fogHeight",        edgeFogHeight_);
    edgeFogProg_->setFloat("u_fogMax",           edgeFogMax_);
    edgeFogProg_->setFloat("u_waterElevation",   waterElevation_);

    // FOG-HORIZON-CLAMP-1: same elevation profile as the OOB pass. The edge-fog
    // frag already refuses fog for rays looking up/horizontal (dz >= -0.001), so
    // this narrows the surviving small-downward-elevation band toward the horizon
    // exactly like fog_oob, keeping upward bleed out of the sky.
    edgeFogProg_->setInt("u_horizonClampEnabled", fogHorizonClampEnabled_ ? 1 : 0);
    edgeFogProg_->setFloat("u_horizonFadeStartSin", fogHorizonFadeStartSin_);
    edgeFogProg_->setFloat("u_horizonFadeEndSin",   fogHorizonFadeEndSin_);

    // SKYBOX-FOG-EXCLUDE-1: gate-ON only; default OFF -> u_skyExcludeEnabled=0,
    // stencilTex left unbound (unit 1 stays whatever it was -- shader never
    // reads it when the uniform is 0, so this is byte-identical OFF).
    const bool skyExclude = skyboxFogExcludeEnabled() && sceneStencilViewTex_ != 0;
    edgeFogProg_->setInt("u_skyExcludeEnabled", skyExclude ? 1 : 0);
    if (skyExclude) {
        edgeFogProg_->setInt("stencilTex", 1);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, sceneStencilViewTex_);

        // SKYBOX-FOG-EXCLUDE-1 review fix: this pass samples the stencil
        // view of the shared depth-stencil attachment while GL_STENCIL_TEST
        // may still be enabled (left ON by the skybox stencil-tag pass, and
        // PipelineRegistry's PostProcessEdgeFog desc has no stencil fields --
        // stencil state is never part of the state-guard idiom here, only
        // inherited). Sampling a texture that aliases the currently-bound
        // stencil attachment while stencil writes are possible is exactly
        // the feedback case GL disallows; force test off + write mask 0 so
        // this pass never writes stencil regardless of inherited state.
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0x00);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    if (skyExclude) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
}

void gosPostProcess::runFogOob()
{
    ZoneScopedN("Render.FogOob");
    TracyGpuZone("Render.FogOob");

    if (!fogOobEnabled_ || !fogOobProg_ || !fogOobProg_->is_valid()) return;

    // Skip on menus / logistics (mech bay, purchase screen): no terrain drawn this
    // frame means we are not in an active mission. Mirrors the sceneHasTerrain_
    // gate used by cloud-shadow / godrays / shoreline. Without this the OOB fog
    // leaks onto the front-end backdrop. markTerrainDrawn() sets this in-mission.
    if (!sceneHasTerrain_) return;

    // APPLY-STATE-REDUNDANT-BODY-REMOVE-2: skip the 4 setup calls when the executor
    // already applied them (MC2_FRAMEGRAPH_EXECUTOR ON). Reset one-shot before draw.
    if (!fogOobStateAppliedByExecutor_) {
        // Bind scene FBO — writes to color attachment 0 only.
        // Reads sceneDepthTex_ (separate attachment — no read/write conflict).
        // RENDER-BACKEND-IFACE-FBO-1: route this FBO-bind through IRenderBackend
        // under MC2_RENDER_BACKEND_IFACE (default-OFF). OFF = direct GL, byte-
        // identical. ON = the SAME glBindFramebuffer(sceneFBO_) via GLBackend —
        // both GL, output identical; the gate just proves the seam routes.
        static const bool s_backendIface = []() {
            const char* v = getenv("MC2_RENDER_BACKEND_IFACE");
            return v && v[0] == '1';
        }();
        if (s_backendIface) {
            RenderCore::getGLBackend().bindFramebuffer(sceneFBO_);
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
        }
        setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
        glViewport(0, 0, width_, height_);

        // POSTPROCESS-FOG-REGISTRATION-1: drive FF state from the PostProcessFogOob row.
        // Byte-identical: depth test+write OFF, cull None, AlphaBlend
        // (SRC_ALPHA/ONE_MINUS_SRC_ALPHA). fogOobProg_->apply() below binds the program.
        // OOB pixels emit (fogColor, opacity), sky pixels (0,0); sceneColorTex_ never
        // sampled (no read/write feedback). Teardown stays owned by this site.
        pipeline_binder::applyPipeline(
            RenderCore::getPipelineDesc(RenderCore::PipelineId::PostProcessFogOob),
            "PostProcessFogOob");
    }
    fogOobStateAppliedByExecutor_ = false;
    render_frame_plan::trace(render_frame_plan::Phase::PostProcess, "FogOob",
        render_frame_plan::PathKind::ApplyPipeline, 1, "PostProcessFogOob");

    // Other post-process passes upload inverseViewProj_ with GL_FALSE (row-major
    // data as-is), which in GLSL gives (W^{-1})^T * v — the correct clip->world
    // unprojection when the terrain MVP was uploaded via GL_FALSE.
    // setMat4 uses GL_TRUE internally; transposing here gives the same result.
    float invT[16];
    {
        const float* s = inverseViewProj_;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                invT[i*4+j] = s[j*4+i];
    }
    float fogTime = SmokeMode::fixedTimestepEnabled()
                        ? (float)SmokeMode::fixedClockSeconds()
                        : (float)SDL_GetTicks() / 1000.0f;
    fogOobProg_->apply();
    fogOobProg_->setInt("depthTex", 0);
    fogOobProg_->setMat4("invViewProj", invT);
    fogOobProg_->setFloat3("u_fogColor",  oobFogColor_);
    fogOobProg_->setFloat("u_fogOpacity", oobFogOpacity_);
    fogOobProg_->setFloat("u_time", fogTime);

    // FOG-HORIZON-CLAMP-1: elevation profile. clamp=1 (default) selects the new
    // shape (full at/below horizon, fade to zero in [startSin,endSin]); clamp=0
    // restores the legacy worldDir.z 0.22 band in-shader.
    fogOobProg_->setInt("u_horizonClampEnabled", fogHorizonClampEnabled_ ? 1 : 0);
    fogOobProg_->setFloat("u_horizonFadeStartSin", fogHorizonFadeStartSin_);
    fogOobProg_->setFloat("u_horizonFadeEndSin",   fogHorizonFadeEndSin_);

    // SKYBOX-FOG-EXCLUDE-1: gate-ON only; default OFF -> u_skyExcludeEnabled=0,
    // stencilTex left unbound (shader never reads it when the uniform is 0,
    // so this is byte-identical OFF).
    const bool skyExclude = skyboxFogExcludeEnabled() && sceneStencilViewTex_ != 0;
    fogOobProg_->setInt("u_skyExcludeEnabled", skyExclude ? 1 : 0);
    if (skyExclude) {
        fogOobProg_->setInt("stencilTex", 1);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, sceneStencilViewTex_);

        // SKYBOX-FOG-EXCLUDE-1 review fix: same reasoning as runEdgeFog() --
        // this pass samples the stencil view of the shared depth-stencil
        // attachment; force stencil test off + write mask 0 so the GL
        // feedback-exception rule (no writes to a resource being sampled)
        // can never be violated regardless of inherited stencil state.
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0x00);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    if (skyExclude) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
}

// ===========================================================================
// RENDER-BACKEND-REGION-IFACE-1 (Layer-6 ENTRY): PostprocessFog region.
// ===========================================================================
//
// 1A: the extracted GL fog region wrapper. Calls the UNCHANGED runEdgeFog() +
// runFogOob() inline bodies, in the same order the endScene() sites call them.
// No interface, no Vulkan: this is a pure code-move. Under the master gate OFF
// this wrapper is never entered (endScene() runs the original direct sites);
// under gate ON, the GL backend calls this so its output == the old inline path.
int gosPostProcess::runFogRegionGL()
{
    runEdgeFog();
    runFogOob();
    // Both GL fog sites ran (each may per-effect early-return internally, exactly
    // as the original inline sites did — that behavior is unchanged). The region
    // contract is "the two GL fog passes", so the equivalence count is 2.
    return 2;
}

// Master-gate read (once). MC2_RENDER_BACKEND_REGION_IFACE==1 -> route the fog
// region through IRenderBackend; else the original direct-GL sites run.
bool gosPostProcess::regionIfaceEnabled() const
{
    static const bool s_on = []() {
        const char* v = getenv("MC2_RENDER_BACKEND_REGION_IFACE");
        return v && v[0] == '1';
    }();
    return s_on;
}

// Resolver: gosPostProcess owns the scene textures, so it maps the neutral ids
// to the concrete GL handles. Keeps handles OUT of the region context.
unsigned int gosPostProcess::resolveRegionResource(RenderCore::RenderResourceId id) const
{
    switch (id) {
        case RenderCore::RenderResourceId::MainColor: return (unsigned int)sceneColorTex_;
        case RenderCore::RenderResourceId::MainDepth: return (unsigned int)sceneDepthTex_;
        default: return 0u;
    }
}

#ifdef MC2_VULKAN_ISLAND
// 1C: Vulkan impl of the PostprocessFog region. Reuses the shipped, parity-proven
// subgraph. POSTPROCESS-VULKAN-BACKEND-OPT-IN: the region-iface routing backend=vk
// IS the enable signal for Vulkan -- so this DECOUPLES from the standalone subgraph
// gate. It invokes postprocessSubgraphReadyForRegion() (lazy init + fail-soft, NO
// MC2_VULKAN_POSTPROCESS_SUBGRAPH check), NOT vulkanPostprocessSubgraphEnabled().
// Fail-soft: readiness returns false ONLY on a genuine device/init failure (and
// self-disables), so we return false -> caller runs GL (FallbackGL = real failure,
// not "gate off"). The standalone MC2_VULKAN_POSTPROCESS_SUBGRAPH path stays
// independently runnable (that gate still routes through the same readiness half).
// The neutral ctx values already equal the subgraph's EXACT member-sourced uploads,
// so no data is threaded through here — the ctx is carried for contract symmetry
// and resolver exercise.
bool gosPostProcess::runFogRegionVulkanBackend(
    const RenderCore::PostprocessFogRegionContext& ctx,
    RenderCore::RegionOutput* out)
{
    if (ctx.resolver) {
        (void)ctx.resolver->resolve(ctx.color);
        (void)ctx.resolver->resolve(ctx.depth);
    }
    if (!postprocessSubgraphReadyForRegion()) {
        if (out) { out->impl = RenderCore::RenderRegionImpl::None;
                   out->fallbackReason = "vk_device_init_failed";
                   out->glPassesRun = 0; out->vkDraws = 0; }
        return false;
    }
    runPostprocessSubgraph();   // does BOTH fog effects in ONE Vulkan render pass
    if (out) { out->impl = RenderCore::RenderRegionImpl::VulkanSubgraph;
               out->fallbackReason = ""; out->glPassesRun = 0; out->vkDraws = 2; }
    return true;
}
#endif

// ---------------------------------------------------------------------------
// 1B: region routing — selector + backend impls + health snapshot.
// ---------------------------------------------------------------------------
namespace {

// Process-lifetime region-selection health, surfaced in the debug-state dump so
// backend_region_selected = gl|vk|fallback_gl is catchable. Distinct from the
// subgraph's own g_sub health (that one is the Vulkan device/perf report).
struct RegionSelectHealth {
    int         ifaceGateOn      = 0;    // MC2_RENDER_BACKEND_REGION_IFACE==1
    char        requestedBackend[8] = "gl";  // MC2_POSTPROCESS_BACKEND (gl|vk)
    int         lastImpl         = 0;    // RenderRegionImpl (None/GLInline/VulkanSubgraph/FallbackGL)
    char        lastFallbackReason[64] = {0};
    int         lastGlPassesRun  = 0;    // GL fog passes actually run (0 or 2)
    int         lastVkDraws      = 0;    // Vulkan fog draws (0 or 2)
    unsigned long routedFrames   = 0;    // frames routed through runRegion
};
RegionSelectHealth g_region;

void region_set_fallback_reason(const char* r) {
    std::strncpy(g_region.lastFallbackReason, r ? r : "",
                 sizeof(g_region.lastFallbackReason) - 1);
    g_region.lastFallbackReason[sizeof(g_region.lastFallbackReason) - 1] = '\0';
}

// Neutral-context resolver adaptor wrapping gosPostProcess::resolveRegionResource.
struct GosResourceResolver final : RenderCore::IResourceResolver {
    const gosPostProcess* pp = nullptr;
    unsigned int resolve(RenderCore::RenderResourceId id) const override {
        return pp ? pp->resolveRegionResource(id) : 0u;
    }
};

// GL impl of runRegion for the PostprocessFog region: resolves color/depth from
// the neutral context (proving the id->handle path is exercised even though the
// GL fog bodies read the members directly today) and runs the extracted GL
// wrapper. Always succeeds -> out->impl = GLInline. Delegates to the owner.
struct GlFogRegionBackend final : RenderCore::IRenderBackend {
    gosPostProcess* pp = nullptr;
    void bindBackbuffer() override {}
    void bindFramebuffer(unsigned int) override {}
    void setViewport(int, int, int, int) override {}
    void clear(unsigned int, float, float, float, float) override {}
    bool runRegion(RenderCore::RenderRegionId id,
                   const RenderCore::PostprocessFogRegionContext& ctx,
                   RenderCore::RegionOutput* out) override {
        if (id != RenderCore::RenderRegionId::PostprocessFog || !pp) return false;
        // Exercise the id->handle resolve so the neutral-context contract is real
        // (a bad resolver would surface as 0 handles here, not a silent divergence).
        if (ctx.resolver) {
            (void)ctx.resolver->resolve(ctx.color);
            (void)ctx.resolver->resolve(ctx.depth);
        }
        int gl = pp->runFogRegionGL();
        if (out) { out->impl = RenderCore::RenderRegionImpl::GLInline;
                   out->glPassesRun = gl; out->vkDraws = 0; out->fallbackReason = ""; }
        return true;
    }
};

} // namespace

// C-linkage accessor for the debug-state dump (region-selection health). Mirrors
// the mc2_vulkan_postprocess_subgraph_health pattern; always compiled (no #ifdef)
// because the region iface exists on every build.
extern "C" void mc2_render_backend_region_health(
    int* ifaceGateOn, const char** requestedBackend, int* lastImpl,
    const char** lastFallbackReason, int* lastGlPassesRun, int* lastVkDraws,
    unsigned long* routedFrames) {
    if (ifaceGateOn)         *ifaceGateOn         = g_region.ifaceGateOn;
    if (requestedBackend)    *requestedBackend    = g_region.requestedBackend;
    if (lastImpl)            *lastImpl            = g_region.lastImpl;
    if (lastFallbackReason)  *lastFallbackReason  = g_region.lastFallbackReason;
    if (lastGlPassesRun)     *lastGlPassesRun     = g_region.lastGlPassesRun;
    if (lastVkDraws)         *lastVkDraws         = g_region.lastVkDraws;
    if (routedFrames)        *routedFrames        = g_region.routedFrames;
}

// The routed entry (master gate ON). Builds the neutral context, selects the
// backend, calls runRegion; falls back to GL on false. Never double-applies:
// exactly ONE of {GL wrapper, Vulkan subgraph} runs, and the GL fog sites in
// endScene() are skipped whenever this routed path is taken.
void gosPostProcess::runFogRegionThroughBackend()
{
    ++g_region.routedFrames;
    g_region.ifaceGateOn = 1;

    // Sub-selector: MC2_POSTPROCESS_BACKEND = gl | vk (default gl).
    static const bool s_wantVk = []() {
        const char* v = getenv("MC2_POSTPROCESS_BACKEND");
        return v && (v[0] == 'v' || v[0] == 'V');   // "vk"
    }();
    std::strncpy(g_region.requestedBackend, s_wantVk ? "vk" : "gl",
                 sizeof(g_region.requestedBackend) - 1);

    // Build the BACKEND-NEUTRAL context (RenderResourceId + params + dims; NO
    // raw handles). Values pulled EXACTLY from the GL runEdgeFog()/runFogOob().
    GosResourceResolver resolver;
    resolver.pp = this;
    RenderCore::PostprocessFogRegionContext ctx;
    ctx.color  = RenderCore::RenderResourceId::MainColor;
    ctx.depth  = RenderCore::RenderResourceId::MainDepth;
    ctx.width  = width_;
    ctx.height = height_;
    ctx.resolver = &resolver;
    // EdgeFog params (row-major invViewProj as the GL path uploads with GL_FALSE).
    std::memcpy(ctx.edgeFog.invViewProj, inverseViewProj_, 16 * sizeof(float));
    ctx.edgeFog.color[0] = edgeFogColor_[0];
    ctx.edgeFog.color[1] = edgeFogColor_[1];
    ctx.edgeFog.color[2] = edgeFogColor_[2];
    ctx.edgeFog.halfExtent     = mapHalfExtent_;
    ctx.edgeFog.fogStart       = edgeFogStart_;
    ctx.edgeFog.fogHeight      = edgeFogHeight_;
    ctx.edgeFog.fogMax         = edgeFogMax_;
    ctx.edgeFog.waterElevation = waterElevation_;
    // OOB params.
    std::memcpy(ctx.oobFog.invViewProj, inverseViewProj_, 16 * sizeof(float));
    ctx.oobFog.color[0] = oobFogColor_[0];
    ctx.oobFog.color[1] = oobFogColor_[1];
    ctx.oobFog.color[2] = oobFogColor_[2];
    ctx.oobFog.opacity  = oobFogOpacity_;
    ctx.oobFog.time     = SmokeMode::fixedTimestepEnabled()
                              ? (float)SmokeMode::fixedClockSeconds()
                              : (float)SDL_GetTicks() / 1000.0f;

    RenderCore::RegionOutput out;
    bool ran = false;

#ifdef MC2_VULKAN_ISLAND
    if (s_wantVk) {
        // Route through the Vulkan subgraph backend (1C). On false the caller
        // (this function) runs GL below with impl=FallbackGL.
        ran = runFogRegionVulkanBackend(ctx, &out);
        if (!ran) {
            region_set_fallback_reason(out.fallbackReason && out.fallbackReason[0]
                                           ? out.fallbackReason : "vk_declined");
        }
    }
#endif

    if (!ran) {
        GlFogRegionBackend gl;
        gl.pp = this;
        bool okGl = gl.runRegion(RenderCore::RenderRegionId::PostprocessFog, ctx, &out);
        if (s_wantVk) {
            // requested vk but GL actually ran -> FallbackGL (not plain GLInline).
            out.impl = RenderCore::RenderRegionImpl::FallbackGL;
        }
        (void)okGl;
    }

    g_region.lastImpl        = (int)out.impl;
    g_region.lastGlPassesRun = out.glPassesRun;
    g_region.lastVkDraws     = out.vkDraws;
    if (out.impl != RenderCore::RenderRegionImpl::FallbackGL)
        region_set_fallback_reason("");

    render_frame_plan::trace(render_frame_plan::Phase::PostProcess, "PostprocessFogRegion",
        render_frame_plan::PathKind::ApplyPipeline, 1,
        RenderCore::region_impl_to_string(out.impl));
}

// Forward declarations for ISLAND-2 sub-stage wrappers (defined below the executor section).
static void executorOwnBeginSub(gosPostProcess*, RenderCore::framegraph::ExecutorIslandId);
static void executorOwnEndSub(gosPostProcess*, RenderCore::framegraph::ExecutorIslandId);

void gosPostProcess::endScene()
{
    ZoneScopedN("Render.PostProcess");
    TracyGpuZone("Render.PostProcess");

    if (!initialized_)
        return;

    // GLSTATE-VAO-RESTORE-1: save the caller's VAO so endScene() is self-contained.
    // NVIDIA Core profile rejects any draw with VAO=0 (GL_INVALID_OPERATION). endScene
    // uses its own quadVAO_ internally and leaves VAO=0 on exit; callers that draw
    // afterward must not rely on the side effect. Restoring here eliminates that
    // contract leak and makes future draw sites safe without a gos_RendererRebindVAO() call.
    GLint prevVAO = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);

    // HZB-DEPTH-PYRAMID-MVP-1: build the reverse-Z Hi-Z pyramid from the
    // resolved scene depth before any post pass. Gated (MC2_HZB_BUILD), no
    // consumers, no draw suppression -> no-op + byte-identical when OFF.
    runHzbReduce();
    runHzbProbe();   // diagnostic-only; reads the pyramid, suppresses no draws

    // CLUSTER-DEPTH-PYRAMID-NATIVE-1: build a per-tile (min,max) depth image
    // from the resolved scene depth, here where depth is fully populated and
    // before any post pass. Gated (MC2_CLUSTER_DEPTH_PYRAMID, default OFF) ->
    // no-op + byte-identical when OFF. Substrate only: no consumer of the
    // output exists yet (no lighting / decal / material reader).
    cluster_depth_pyramid::Run(sceneDepthTex_, width_, height_);

    // MC2-LIGHTGRID-BUILD-NATIVE-1: build a per-tile light-bin grid from a native
    // sphere cull-geometry buffer (derived from ObjectLights @ binding 20) and the
    // depth pyramid's per-tile (min,max) image. Runs AFTER the depth pyramid (it
    // samples that tile texture) and AFTER the light SSBO upload. Gated
    // (MC2_LIGHTGRID_BUILD, default OFF) -> no-op + byte-identical when OFF.
    // INERT: no shading consumer reads the grid; zero visual change.
    lightgrid_build::Run(inverseViewProj_, width_, height_);

    // POSTPROCESS-COMPUTE-BLUR-1: greenfield GPU compute downsample + separable
    // Gaussian blur SUBSTRATE (Vulkan-prep: typed-sync + ping-pong pattern).
    // Runs after the other compute passes. Gated (MC2_POSTPROCESS_COMPUTE_BLUR,
    // default OFF) -> no-op + byte-identical when OFF. INERT: NO bloom/glow/DOF
    // consumer reads the blurred output; zero visual change. When ON, the live
    // path blurs the feedback-safe scene-color copy if present (substrate only);
    // the VERIFY sub-gate always proves CPU-vs-GPU parity on a controlled test
    // pattern, independent of any consumer.
    postprocess_blur::Run(getSceneColorCopyTexture(), width_, height_);

    // Post-process shadow pass: covers terrain, objects, and overlays in one
    // pass, with reduced terrain darkening to avoid obvious double-shadowing.
    // EXECUTOR-ISLAND-SCREENSHADOW-1: validate->call-unchanged->validate (default-OFF).
    executorOwnBeginSub(this, RenderCore::framegraph::ExecutorIslandId::ScreenShadow);
    runScreenShadow();
    executorOwnEndSub(this, RenderCore::framegraph::ExecutorIslandId::ScreenShadow);

    // Procedural cloud shadows: single fullscreen multiplicative pass over all
    // non-sky pixels (replaces the four inline cloud blocks). After the screen
    // shadow so cloud darkens the already-shadowed scene color.
    // FRAME-GRAPH-EXECUTOR-ISLAND-3: validate->call-unchanged->validate (default-OFF).
    executorOwnBeginSub(this, RenderCore::framegraph::ExecutorIslandId::CloudShadow);
    runCloudShadow();
    executorOwnEndSub(this, RenderCore::framegraph::ExecutorIslandId::CloudShadow);

    // Shoreline foam pass (brightens water pixels adjacent to terrain)
    // FRAME-GRAPH-EXECUTOR-ISLAND-3: validate->call-unchanged->validate (default-OFF).
    executorOwnBeginSub(this, RenderCore::framegraph::ExecutorIslandId::Shoreline);
    runShoreline();
    executorOwnEndSub(this, RenderCore::framegraph::ExecutorIslandId::Shoreline);

    // SSAO grounding pass (multiplicative darkening into scene color). Default-OFF (gated).
    runSSAO();

    // BT2018-BOX-DECAL-1: screen-space box decal composited into lit+shadowed+AO'd
    // scene color, before fog/tonemap. Default-OFF (MC2_BOX_DECAL); no-op when gate off.
    drawBoxDecals();

    // Edge fog: fades geometry near the map boundary into the cloud color.
    // After SSAO so AO-darkening is preserved under the fog.
    // FRAME-GRAPH-EXECUTOR-ISLAND-2: validate->call-unchanged->validate (default-OFF).
    executorOwnBeginSub(this, RenderCore::framegraph::ExecutorIslandId::EdgeFog);
    // RENDER-BACKEND-REGION-IFACE-1 (Layer-6 ENTRY): when the master gate is ON,
    // route the WHOLE fog region (edge + oob) through IRenderBackend::runRegion
    // ONCE here, and skip BOTH original fog sites (guard below). Selector
    // MC2_POSTPROCESS_BACKEND=gl|vk picks the impl; VK falls back to the GL
    // wrapper on failure. Gate OFF -> the original direct sites run verbatim
    // (byte-identical). Precedence when ON: region-iface > (subgraph > island > GL
    // inside the GL/VK impls). The region-iface path is mutually exclusive with
    // the legacy in-site subgraph/island paths (which stay reachable gate-OFF).
    const bool regionRouted = regionIfaceEnabled();
    bool subgraphRanThisFrame = false;   // scope spans to the FogOob site below
    (void)subgraphRanThisFrame;          // read only under MC2_VULKAN_ISLAND
    if (regionRouted) {
        runFogRegionThroughBackend();   // does BOTH edge + oob (GL wrapper or VK subgraph)
    } else {
#ifdef MC2_VULKAN_ISLAND
    // VULKAN-POSTPROCESS-SUBGRAPH-1: gate precedence (correctness-critical). When
    // MC2_VULKAN_POSTPROCESS_SUBGRAPH=1 and lazy init succeeded, run the FUSED
    // subgraph ONCE here (it does BOTH edge fog AND oob fog in one Vulkan render
    // pass), and set subgraphRanThisFrame so the OOB-fog site below SKIPS entirely
    // (no GL, no island) -- otherwise oob would double-apply. The subgraph, the
    // EdgeFog island, and the OOB island must NEVER all fire; precedence is:
    //   subgraph  >  per-pass island  >  GL.
    // VULKAN-EDGE-FOG-ISLAND-2a: else route the edge-fog composite through the
    // per-pass Vulkan island when MC2_VULKAN_EDGE_FOG_ISLAND=1. Otherwise GL.
    if (vulkanPostprocessSubgraphEnabled()) {
        runPostprocessSubgraph();       // does BOTH fog effects
        subgraphRanThisFrame = true;    // -> OOB site is skipped below (no double-apply)
    } else if (vulkanEdgeFogIslandEnabled()) { runEdgeFogVulkan(); } else
#endif
    { runEdgeFog(); }
    }
    executorOwnEndSub(this, RenderCore::framegraph::ExecutorIslandId::EdgeFog);

    // OOB fog: applies fog color to far-plane pixels pointing toward ground.
    // After edge fog so the two cloud colors match seamlessly.
    // FRAME-GRAPH-EXECUTOR-ISLAND-2: validate->call-unchanged->validate (default-OFF).
    executorOwnBeginSub(this, RenderCore::framegraph::ExecutorIslandId::FogOob);
    // RENDER-BACKEND-REGION-IFACE-1: when the fog region was routed through the
    // backend above, it ALREADY did the oob fog -> skip this site ENTIRELY (no GL,
    // no island). Explicit no-double-apply guard, symmetric with subgraphRanThisFrame.
    if (regionRouted) {
        // region iface did edge+oob; nothing to do here.
    } else {
#ifdef MC2_VULKAN_ISLAND
    // VULKAN-POSTPROCESS-SUBGRAPH-1: if the fused subgraph already ran this frame it
    // ALSO did the oob fog, so this site is skipped ENTIRELY (no GL, no island) --
    // the explicit, unmissable no-double-apply guard.
    if (subgraphRanThisFrame) {
        // subgraph did edge+oob; nothing to do here.
    } else
    // VULKAN-OOB-FOG-ISLAND-1: route the OOB-fog composite through the Vulkan
    // island when MC2_VULKAN_OOB_FOG_ISLAND=1 and lazy init succeeded. Otherwise
    // (gate off, or init fail-soft) fall through to the unchanged GL path.
    if (vulkanOobFogIslandEnabled()) { runFogOobVulkan(); } else
#endif
    { runFogOob(); }
    }
    executorOwnEndSub(this, RenderCore::framegraph::ExecutorIslandId::FogOob);

    // Bind default framebuffer (the backbuffer) for the composite output edge.
    // RENDER-BACKEND-IFACE-POSTPROCESS-1: first render-backend boundary route.
    // Gate MC2_RENDER_BACKEND_IFACE (default-OFF). OFF = direct GL, byte-
    // identical. ON = the SAME glBindFramebuffer(0) via GLBackend — both GL, so
    // output is identical; the gate just proves the seam routes. The interface
    // grows one edge at a time; only this bind is routed in this slice.
    static const bool s_backendIface = []() {
        const char* v = getenv("MC2_RENDER_BACKEND_IFACE");
        return v && v[0] == '1';
    }();
    if (s_backendIface) {
        RenderCore::getGLBackend().bindBackbuffer();
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    // [FORCE-43 v1] Pillarbox the final scene composite into a centered 4:3
    // rect with black bars. The scene FBO content (4:3 world stretched into the
    // 16:9 target) un-stretches when drawn into a 4:3 rect. Mouse is remapped
    // box-relative in gameos_input so pick stays aligned with the visible box.
    {
        // [OOB-LETTERBOX v1] (gate MC2_OOB_LETTERBOX, default ON; =0 reverts).
        // When the backbuffer is larger than the scene FBO — e.g. a smoke run
        // that STARTS minimized (FBO sized to the minimized drawable, backbuffer
        // larger), or a menu where the window exceeds the render res — the
        // composite quad below only covers glViewport(0,0,width_,height_) (the
        // bottom-left); the rest of the backbuffer is never written and shows
        // stale/alternating swapchain content -> a blue/black ~2Hz strobe in the
        // out-of-bounds border (user-reported headache; also seen in menus).
        // Clear the WHOLE backbuffer to black first so the OOB stays a stable
        // black letterbox. glClear ignores glViewport (only scissor bounds it),
        // so disable scissor for the clear. No-op in normal play: the composite
        // quad overdraws the full backbuffer. FORCE-43 already cleared in its own
        // branch; this generalizes it to ALL composite paths.
        static const bool s_oobLetterbox = []() {
            const char* v = getenv("MC2_OOB_LETTERBOX");
            return !(v && v[0] == '0');
        }();
        if (s_oobLetterbox) {
            const GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
            if (prevScissor) glDisable(GL_SCISSOR_TEST);
            // RENDER-BACKEND-IFACE-CLEAR-1: route the OOB letterbox clear.
            if (s_backendIface) {
                RenderCore::getGLBackend().clear(GL_COLOR_BUFFER_BIT, 0.0f, 0.0f, 0.0f, 1.0f);
            } else {
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            if (prevScissor) glEnable(GL_SCISSOR_TEST);
        }
        int bx, by, bw, bh;
        if (gos_Compute43Box(width_, height_, &bx, &by, &bw, &bh)) {
            // RENDER-BACKEND-IFACE-VIEWPORT-1 / -CLEAR-1: route FORCE-43 pillarbox.
            if (s_backendIface) {
                RenderCore::getGLBackend().setViewport(0, 0, width_, height_);
                RenderCore::getGLBackend().clear(GL_COLOR_BUFFER_BIT, 0.0f, 0.0f, 0.0f, 1.0f);
                RenderCore::getGLBackend().setViewport(bx, by, bw, bh);
            } else {
                glViewport(0, 0, width_, height_);
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                glViewport(bx, by, bw, bh);
            }
        } else {
            // RENDER-BACKEND-IFACE-VIEWPORT-1: route the non-pillarbox composite viewport.
            if (s_backendIface) {
                RenderCore::getGLBackend().setViewport(0, 0, width_, height_);
            } else {
                glViewport(0, 0, width_, height_);
            }
        }
    }

    // POSTPROCESS-COMPOSITE-REGISTRATION-1: drive the fullscreen-quad fixed-function
    // state from the PostProcessComposite pipeline row instead of hand-setting it.
    // Byte-identical to the prior hand-set: depth test OFF, depth-write OFF (mask
    // FALSE), cull None, and Opaque = GL_BLEND OFF. The "force opaque" matters
    // because gosFX/MLR additive draws (gos_Alpha_OneOne) leak GL_BLEND+ONE/ONE into
    // the composite; with an RGBA8 backbuffer that clamps at 1.0 the additive
    // accumulation saturates to white over ~1s (pylon power-generator canary on
    // mc2_05/mc2_24). Composite must fully overwrite the backbuffer. glProgramName=0
    // in the row, so compositeProg_->apply() below still binds the program;
    // applyPipeline only sets the fixed-function state. (The binder also sets a
    // neutral glBlendFunc(ONE,ZERO) + frontFace Ccw + depthFunc Always — all inert
    // for this blend-off, cull-off, depth-off draw.)
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::PostProcessComposite),
        "PostProcessComposite");
    render_frame_plan::trace(render_frame_plan::Phase::PostProcess, "Composite",
        render_frame_plan::PathKind::ApplyPipeline, 1, "PostProcessComposite");

    // Draw fullscreen quad with composite shader
    if (compositeProg_ && compositeProg_->is_valid()) {
        // Set uniforms BEFORE apply() — apply() binds program + flushes dirty uniforms
        compositeProg_->setInt("sceneTex", 0);
        // The unconditional sunset grade + exposure in postprocess.frag are the
        // pre-existing default look and are untouched.
        compositeProg_->setFloat("exposure", exposure_);

        float invSize[2] = { 1.0f / (float)width_, 1.0f / (float)height_ };
        compositeProg_->setFloat2("inverseScreenSize", invSize);

        // POST-FX-FXAA-1: gate + tunables. enabled=0 -> shader single-taps the
        // scene (byte-identical). Set before apply() like the others.
        compositeProg_->setInt  ("u_fxaaEnabled",        fxaaEnabled_ ? 1 : 0);
        compositeProg_->setFloat("u_fxaaSubpix",         fxaaSubpix_);
        compositeProg_->setFloat("u_fxaaEdgeThreshold",  fxaaEdgeThreshold_);
        compositeProg_->setFloat("u_fxaaEdgeThresholdMin", fxaaEdgeThresholdMin_);

        // POST-FX-CAS-1: gate + sharpness. enabled=0 -> shader skips CAS entirely
        // (byte-identical). Set before apply() like the others.
        compositeProg_->setInt  ("u_casEnabled",   casEnabled_ ? 1 : 0);
        compositeProg_->setFloat("u_casSharpness", casSharpness_);
        // POST-FX-TONEMAP-V2: 0 = no-op branch in the shader (byte-identical).
        compositeProg_->setInt  ("u_tonemapMode",  tonemapMode_);

        // VIEWMODE-POSTPROCESS-PRESENTATION-1: resolve effective view mode.
        // Gate OFF -> forced 0 (Visual). ObjectIdDebug requires sceneObjectIdTex_;
        // if it is 0 (MC2_OBJECT_ID_BUFFER not set) we fall back to Visual and
        // warn once so the caller knows why the debug view is blank.
        int effectiveMode = gos_GetSelectedViewMode();
        if (effectiveMode == 1 && sceneObjectIdTex_ == 0) {
            static bool s_warnedOidMissing = false;
            if (!s_warnedOidMissing) {
                std::fprintf(stderr,
                    "[VIEWMODE v1] ObjectIdDebug requested but sceneObjectIdTex_=0 "
                    "(MC2_OBJECT_ID_BUFFER not set); falling back to Visual\n");
                s_warnedOidMissing = true;
            }
            effectiveMode = 0;
        }
        compositeProg_->setInt("u_viewMode", effectiveMode);

        // u_objectIdTex is always declared in the shader at unit 2.
        // The sampler is only read when effectiveMode==1 (and sceneObjectIdTex_ != 0).
        // In the Visual path the sampler goes unread, so the unit-2 binding
        // does not matter — but we still set the uniform to keep drivers happy.
        compositeProg_->setInt("u_objectIdTex", 2);

        // GAMEADAPTERS-VISUAL-STATE-BRIDGE: Thermal (mode 3) reads hot for
        // engine-bearing units. Mech handles occupy index >= kMechHandleBase;
        // static props/terrain are below it (RenderWorld invariant). Pass the
        // base so the shader classifies object-ID pixels. 0 = OID buffer
        // unavailable -> Thermal falls back to luminance-only (placeholder).
        // Vehicles render via the static-prop batcher and are NOT yet
        // distinguishable; vehicles-hot is a documented follow-up.
        //
        // FIREWALL NOTE (manual review — GameOS/ is OUTSIDE the
        // check-include-firewall.sh SCOPE_DIRS, so the CI script does NOT police
        // this call): RenderWorld::MechHandleIndexBase() is a deliberate,
        // RenderWorld-owned classification API (RenderWorld owns the handle
        // partition; see RenderWorld.h). RenderWorld.h was already included here
        // (M1.5 IsObjectIdBufferEnabled) — no new engine header crosses a seam.
        // The numeric threshold must reach the GPU as a uniform; there is no
        // per-pixel C++ classifier path. Reviewed clean (render-spine advisor M2).
        compositeProg_->setInt("u_engineIdxBase",
            sceneObjectIdTex_ != 0 ? (int)RenderWorld::MechHandleIndexBase() : 0);

        // LOWLIGHT-NIGHTVISION-MVP-1: night-vision tunables (read only by the
        // shader when effectiveMode == 5; harmless no-op uniforms otherwise).
        compositeProg_->setFloat("u_lowLightGain", lowLightGain_);
        compositeProg_->setFloat3("u_lowLightTint", lowLightTint_);

        compositeProg_->apply();

        // GLSTATE-GUARD-ADOPTION-1: the composite is the LAST tex-binding draw
        // before gos_InvalidateRenderStateCache() (line ~2281). That invalidate
        // does NOT track texture-unit state (see gl_state_guard.h L160-177), so
        // the unit-0/unit-2 bindings set below otherwise leak past endScene into
        // the next renderer (HUD/menu) — the reported "post-fx textures bleed
        // into menus" symptom. These guards snapshot the prior unit-0/unit-2
        // GL_TEXTURE_2D bindings + active unit on construction and restore them
        // when this block exits at the closing brace below — which is BEFORE the
        // invalidate call, satisfying the header's block-scope requirement.
        // Restore-previous is behavior-neutral for the composite's own draw.
        mc2gl::GlScopedTextureUnit unit0Guard(0);
        mc2gl::GlScopedTextureUnit unit2Guard(2);

        // Bind scene color texture to unit 0
        // FEEDBACK-SAFE: composite resolve — the draw target is the backbuffer (FBO 0,
        // bound earlier in endScene), NOT sceneFBO_, so sampling sceneColorTex_ here
        // reads the resolved scene into a different target. Not a feedback loop.
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sceneColorTex_);

        // Bind object-ID texture to unit 2 (GL_R32UI; read only in ObjectIdDebug mode).
        // Only bind when the texture exists — if sceneObjectIdTex_==0 effectiveMode
        // was already forced back to 0 above so the usampler2D goes unread.
        // Do NOT bind a mismatched type (float tex to usampler2D) as that causes GL errors.
        if (sceneObjectIdTex_ != 0) {
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, sceneObjectIdTex_);
        }

        // Draw the fullscreen quad
        glBindVertexArray(quadVAO_);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        glActiveTexture(GL_TEXTURE0);
    }

    // Shadow debug overlay (draws on top of composite)
    drawShadowDebugOverlay();

    // Re-enable depth test
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);

    // RENDER_STATES v1: post-process disturbed program, depth, blend, sampler,
    // and unit-0/1 texture bindings outside applyRenderStates. Invalidate the
    // applyRenderStates cache so the next renderer (e.g. HUD/debug overlay)
    // gets a full state re-apply, not a stale-cache short-circuit.
    gos_InvalidateRenderStateCache();

    drainGLErrors("post_process");

    // GLSTATE-VAO-RESTORE-1: restore the caller's VAO (see capture above).
    glBindVertexArray((GLuint)prevVAO);
}

void gosPostProcess::drawShadowDebugOverlay()
{
    if (!showShadowDebug_ || !shadowDebugProg_ || !shadowDebugProg_->is_valid())
        return;
    if (!initialized_)
        return;

    // Item 1 P5: when CSM is active AND we're inspecting the dynamic map
    // (mode 1), the dynamic shadow is a GL_TEXTURE_2D_ARRAY -> sample a layer
    // (csmDebugLayer_) via the array sampler. Static map (mode 0) stays 2D.
    const bool csmDynamic = (mc2ShadowCsmEnabled() && dynShadowArrayTex_ != 0 && shadowDebugMode_ != 0);
    const GLenum texTarget = csmDynamic ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    GLuint tex = csmDynamic ? dynShadowArrayTex_
                            : ((shadowDebugMode_ == 0) ? shadowDepthTex_ : dynShadowDepthTex_);
    if (!tex)
        return;

    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    int quadSize = 256;
    int margin = 16;
    glViewport(margin, height_ - quadSize - margin, quadSize, quadSize);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    // Temporarily switch shadow texture from comparison mode to raw depth read
    glBindTexture(texTarget, tex);
    glTexParameteri(texTarget, GL_TEXTURE_COMPARE_MODE, GL_NONE);

    shadowDebugProg_->setInt("shadowDebugMap", 0);
    if (mc2ShadowCsmEnabled()) {
        shadowDebugProg_->setInt("shadowDebugUseArray", csmDynamic ? 1 : 0);
        if (csmDynamic) {
            shadowDebugProg_->setInt("shadowDebugArray", 0);
            int layer = csmDebugLayer_;
            if (layer < 0) layer = 0;
            if (layer >= csmCount_) layer = csmCount_ - 1;
            shadowDebugProg_->setInt("shadowDebugLayer", layer);
        }
    }
    shadowDebugProg_->apply();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(texTarget, tex);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // CRITICAL: restore comparison mode so PCF sampling works next frame
    glBindTexture(texTarget, tex);
    glTexParameteri(texTarget, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);

    // GLSTATE-SHADOWDEBUG-2DARRAY-1: unbind unit-0's 2D_ARRAY so it doesn't
    // leak into the HUD/UI pass (or escape executorOwnEnd under broader ownership).
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(texTarget, 0);

    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

void gosPostProcess::renderSkybox(float sunDirX, float sunDirY, float sunDirZ)
{
    if (!skyboxProg_ || !skyboxProg_->is_valid()) return;

    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    // Set uniforms BEFORE apply() — apply() flushes dirty uniforms to GPU
    float sunDirVec[3] = { sunDirX, sunDirY, sunDirZ };
    float zenith[3] = { 0.18f, 0.35f, 0.72f };    // deeper blue overhead
    float horizon[3] = { 0.55f, 0.62f, 0.72f };   // desaturated blue-grey haze
    float sun[3] = { 0.9f, 0.8f, 0.6f };           // warm but subtle
    skyboxProg_->setFloat3("sunDir", sunDirVec);
    skyboxProg_->setFloat3("zenithColor", zenith);
    skyboxProg_->setFloat3("horizonColor", horizon);
    skyboxProg_->setFloat3("sunColor", sun);
    skyboxProg_->apply();

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glUseProgram(0);
}

void gosPostProcess::renderHdriSkybox(const float* viewMat, const float* projMat)
{
    if (!hdriReady_ || !hdriSkyboxProg_ || !hdriSkyboxProg_->is_valid()
        || !hdriTex_ || !viewMat || !projMat) {
        return;  // no-op: black sky baseline
    }

    // Compute the inverse projection. Convert float arrays to mat4 structs.
    // mat4 constructor takes column-major order; projMat is already column-major.
    mat4 projMat4;
    memcpy(&projMat4.elem[0][0], projMat, 16 * sizeof(float));
    mat4 invProj = inverseMat4(projMat4);
    float invProjArray[16];
    memcpy(invProjArray, &invProj.elem[0][0], 16 * sizeof(float));

    // Extract upper 3x3 of column-major viewMat and transpose
    // (transpose-of-rotation = inverse-of-rotation). Translation is
    // intentionally excluded so the sky does not parallax with the camera.
    float invViewRot[9] = {
        viewMat[0], viewMat[4], viewMat[8],
        viewMat[1], viewMat[5], viewMat[9],
        viewMat[2], viewMat[6], viewMat[10]
    };

    // Query runtime cap on color attachments so save/mask/restore
    // adapts to whichever buffers are bound (ObjectID attachment 2
    // only exists when MC2_OBJECT_ID_BUFFER is set).
    GLint maxDrawBuffers = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
    const int nAtt = (maxDrawBuffers < 3) ? maxDrawBuffers : 3;

    // --- Save state ---
    GLboolean prevDepthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    GLboolean prevDepthTest  = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevBlend      = glIsEnabled(GL_BLEND);
    GLboolean prevCull       = glIsEnabled(GL_CULL_FACE);
    GLint     prevActiveTex  = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
    glActiveTexture(GL_TEXTURE0);
    GLint     prevTex2DBind  = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2DBind);
    GLint     prevProgram    = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    GLint     prevVAO        = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);

    // SKYBOX-FOG-EXCLUDE-1: gate ON only. Save stencil state so the write
    // below cannot leak into any pass downstream of the sky draw. sceneFBO_'s
    // depth attachment is GL_DEPTH24_STENCIL8 (real stencil bits available --
    // see createFBOs() sceneDepthTex_ alloc), so no fallback design is needed.
    const bool skyExclude = skyboxFogExcludeEnabled();
    GLboolean prevStencilTest = GL_FALSE;
    GLint     prevStencilFunc = GL_ALWAYS, prevStencilRef = 0, prevStencilValueMask = 0xFF;
    GLint     prevStencilFail = GL_KEEP, prevStencilZFail = GL_KEEP, prevStencilZPass = GL_KEEP;
    GLint     prevStencilWriteMask = 0xFF;
    if (skyExclude) {
        prevStencilTest = glIsEnabled(GL_STENCIL_TEST);
        glGetIntegerv(GL_STENCIL_FUNC,       &prevStencilFunc);
        glGetIntegerv(GL_STENCIL_REF,        &prevStencilRef);
        glGetIntegerv(GL_STENCIL_VALUE_MASK, &prevStencilValueMask);
        glGetIntegerv(GL_STENCIL_FAIL,       &prevStencilFail);
        glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &prevStencilZFail);
        glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &prevStencilZPass);
        glGetIntegerv(GL_STENCIL_WRITEMASK,  &prevStencilWriteMask);
    }

    // Save per-attachment color masks for whichever attachments exist.
    GLboolean prevMask[3][4] = {
        { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE },
        { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE },
        { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE },
    };
    for (int i = 0; i < nAtt; ++i) {
        glGetBooleani_v(GL_COLOR_WRITEMASK, (GLuint)i, prevMask[i]);
    }

    // Mask writes to attachments 1..2 (preserve normals + ObjectID).
    if (nAtt > 0) glColorMaski(0, GL_TRUE,  GL_TRUE,  GL_TRUE,  GL_TRUE);
    if (nAtt > 1) glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    if (nAtt > 2) glColorMaski(2, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    // --- Item 2 sub-flag resolution (env read once; all default to the
    //     byte-identical baseline). ---
    // MC2_HDRI_SKY_UV_DEBUG : worldDir-gradient diagnostic instead of sample.
    // MC2_HDRI_SKY_AZ_OFFSET: degrees trim added to the sky yaw.
    // MC2_HDRI_SKY_STATE_PROBE: GL-state save/restore self-check.
    // The sun-sync rotation is only applied when the AZ offset is set OR the
    // baked-sun scan succeeded AND a non-zero offset/sync makes skyYaw != 0;
    // when skyYaw resolves to exactly 0.0 the frag path is byte-identical.
    static const int   s_uvDebug = []() {
        const char* e = getenv("MC2_HDRI_SKY_UV_DEBUG");
        return (e && e[0] != '0') ? 1 : 0;
    }();
    // MC2_HDRI_SKY_FRAME_FIX: when set (any non-"0" value), applies the
    // kAxisSwapMC2toGL conversion to worldDir in the frag shader before equirect
    // lookup.  Default OFF => frameFix=0 => byte-identical baseline.
    static const int   s_frameFix = []() {
        const char* e = getenv("MC2_HDRI_SKY_FRAME_FIX");
        return (e && e[0] != '0') ? 1 : 0;
    }();
    static const bool  s_azOffsetSet = (getenv("MC2_HDRI_SKY_AZ_OFFSET") != nullptr);
    static const float s_azOffsetRad = []() {
        const char* e = getenv("MC2_HDRI_SKY_AZ_OFFSET");
        return e ? (float)(atof(e) * 3.14159265358979323846 / 180.0) : 0.0f;
    }();
    static const bool  s_stateProbe = (getenv("MC2_HDRI_SKY_STATE_PROBE") != nullptr);

    // Compute skyYaw. Auto-sync is always-on when hdriBakedSunValid_:
    // aligns the HDRI baked-sun azimuth to the mission terrain sun.
    // MC2_HDRI_SKY_AZ_OFFSET adds a manual trim on top (degrees).
    float skyYaw = s_azOffsetRad;   // manual trim always applied
    if (hdriBakedSunValid_) {
        // Mission sun in RAW MC2 (x=east, y=north, z=elevation, Z-up).
        float lx = 0.0f, ly = 0.0f, lz = 0.0f;
        gos_GetTerrainLightDir(&lx, &ly, &lz);
        // Convert to the SAME GL-equirect frame the baked-sun scan used.
        // kAxisSwapMC2toGL: GL.x = -MC2.x, GL.y = MC2.z(elev), GL.z = MC2.y(north).
        // Equirect azimuth = atan(worldDir.z, worldDir.x) = atan(GL.z, GL.x)
        //                  = atan2(MC2.north, -MC2.east) = atan2(ly, -lx).
        const float sunAzGL = atan2f(ly, -lx);
        // frag rotates worldDir by +skyYaw about +Y before atan(z,x), which
        // SUBTRACTS skyYaw from the recovered azimuth. To move the baked sun
        // (hdriBakedSunAz_) onto the mission sun (sunAzGL) we need
        //   bakedAz - skyYaw == sunAzGL  ->  skyYaw = bakedAz - sunAzGL.
        skyYaw += hdriBakedSunAz_ - sunAzGL;
    }

    // --- Item 2 / Phase 1: one-shot screen-center worldDir log. ---
    // Reconstruct the center-pixel direction CPU-side from the same matrices the
    // frag uses (vNdc = (0,0) at screen center), so the log is independent of
    // whether the gradient draw is visible.
    if (s_uvDebug) {
        static bool s_loggedCenter = false;
        if (!s_loggedCenter) {
            s_loggedCenter = true;
            // viewDir = invProj * (0,0,1,1); worldDir = invViewRot * viewDir.
            float vd[3] = {
                invProjArray[8] + invProjArray[12],   // col2.x + col3.x
                invProjArray[9] + invProjArray[13],   // col2.y + col3.y
                invProjArray[10] + invProjArray[14],  // col2.z + col3.z
            };
            // invViewRot is column-major 3x3 (cols are the rows of viewMat 3x3).
            float wd[3] = {
                invViewRot[0]*vd[0] + invViewRot[3]*vd[1] + invViewRot[6]*vd[2],
                invViewRot[1]*vd[0] + invViewRot[4]*vd[1] + invViewRot[7]*vd[2],
                invViewRot[2]*vd[0] + invViewRot[5]*vd[1] + invViewRot[8]*vd[2],
            };
            float len = std::sqrt(wd[0]*wd[0] + wd[1]*wd[1] + wd[2]*wd[2]);
            if (len > 0.0f) { wd[0]/=len; wd[1]/=len; wd[2]/=len; }
            std::fprintf(stderr,
                "[HDRI_SKY v1] uv_debug center_worldDir=(%.3f,%.3f,%.3f) "
                "bakedSunValid=%d bakedSunAz=%.4f skyYaw=%.4f\n",
                wd[0], wd[1], wd[2], hdriBakedSunValid_ ? 1 : 0,
                hdriBakedSunAz_, skyYaw);
            std::fflush(stderr);
        }
    }

    // --- Phase 3: snapshot GL state pre-draw for the probe. ---
    GLint probeFbo = 0, probeDrawBuf0 = 0;
    GLboolean probeMask0[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    if (s_stateProbe) {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &probeFbo);
        glGetIntegerv(GL_DRAW_BUFFER0, &probeDrawBuf0);
        glGetBooleani_v(GL_COLOR_WRITEMASK, 0, probeMask0);
    }

    // Bind shader + uniforms + texture.
    skyYaw_ = skyYaw;  // WATER-HDRI-REFL-1: cache for water shader uniform
    hdriSkyboxProg_->apply();
    hdriSkyboxProg_->setMat4("invProj", invProjArray);
    hdriSkyboxProg_->setMat3("invViewRot", invViewRot);
    hdriSkyboxProg_->setInt("uvDebug", s_uvDebug);
    hdriSkyboxProg_->setFloat("skyYaw", skyYaw);
    hdriSkyboxProg_->setInt("frameFix", s_frameFix);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hdriTex_);
    hdriSkyboxProg_->setInt("u_hdri", 0);

    // Fullscreen triangle. Core profile requires a non-zero VAO bound.
    if (quadVAO_ != 0) {
        glBindVertexArray(quadVAO_);
    } else {
        if (hdriDummyVao_ == 0) glGenVertexArrays(1, &hdriDummyVao_);
        glBindVertexArray(hdriDummyVao_);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // SKYBOX-FOG-EXCLUDE-1: second, stencil-only pass. Tags stencil=1 for
    // pixels the fog passes must exclude (deep sky -- SKYBOX-FOG-EXCLUDE-2:
    // elevation test worldDir.y > 0.22 in the skybox frame; equivalent to
    // fog_oob's fog-frame worldDir.z < -0.22 band top, see the frag shader's
    // fix note). Pixels at/below that
    // band (horizon + OOB void) are `discard`ed by the frag shader, leaving
    // their stencil at the frame-clear value (0) so fog behavior there is
    // unchanged. Color writes are masked off; only the stencil buffer is
    // touched. Ordering note: terrain/props may later redraw over the same
    // screen pixels -- that is harmless, because the fog passes only ever
    // consult stencil for pixels where rawDepth is still the clear value
    // (nothing drew there); terrain pixels have rawDepth != 0 and never
    // reach the stencil check regardless of what stencil holds underneath.
    if (skyExclude) {
        if (!hdriSkyboxStencilTagProg_) {
            static const char* kStencilTagPrefix = "#version 430\n";
            hdriSkyboxStencilTagProg_ = glsl_program::makeProgram(
                "hdri_skybox_stencil_tag",
                "shaders/hdri_skybox.vert",
                "shaders/hdri_skybox_stencil_tag.frag",
                kStencilTagPrefix);
            if (!hdriSkyboxStencilTagProg_ || !hdriSkyboxStencilTagProg_->is_valid()) {
                std::fprintf(stderr,
                    "[SKYBOX_FOG_EXCLUDE v1] stencil-tag shader failed to compile; "
                    "gate stays ON but exclusion pass is skipped this/every frame\n");
            }
        }
        if (hdriSkyboxStencilTagProg_ && hdriSkyboxStencilTagProg_->is_valid()) {
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_ALWAYS, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            glStencilMask(0xFF);
            // Color writes off for every attachment -- this pass only tags stencil.
            if (nAtt > 0) glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            if (nAtt > 1) glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            if (nAtt > 2) glColorMaski(2, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

            // SKYBOX-FOG-EXCLUDE-1 review fix: clear stencil once per frame
            // before tagging. Without this, stencil=1 from a prior frame's
            // deep-sky pixels persists on pixels the camera has since panned
            // away from sky (nothing else in the renderer clears stencil),
            // so fog would incorrectly stay excluded there. Cheap because
            // it's a single clear of the currently-bound sceneFBO_'s stencil
            // plane, gated behind skyExclude so gate-OFF has zero cost.
            {
                const GLint zero = 0;
                glClearBufferiv(GL_STENCIL, 0, &zero);
            }

            hdriSkyboxStencilTagProg_->apply();
            hdriSkyboxStencilTagProg_->setMat4("invProj", invProjArray);
            hdriSkyboxStencilTagProg_->setMat3("invViewRot", invViewRot);
            // SKYBOX-FOG-EXCLUDE-2: skyYaw upload removed. The tag test is
            // now elevation-only (worldDir.y > 0.22), which the sun-sync yaw
            // (an azimuth rotation) cannot affect. v1 uploaded it and
            // thresholded worldDir.z -- a HORIZONTAL axis in this frame --
            // tagging an azimuth wedge of sky and hard-excluding the fog
            // inside it (the vertical-edged dark-sky artifact).

            glBindVertexArray(quadVAO_ != 0 ? quadVAO_ : hdriDummyVao_);
            glDrawArrays(GL_TRIANGLES, 0, 3);

            static const bool s_tagLogOnce = []() {
                std::fprintf(stderr,
                    "[SKYBOX_FOG_EXCLUDE v2] enabled=1 stencil_tag_pass=active "
                    "threshold=worldDir.y>0.22 (elevation; v1 wedge bug fixed)\n");
                std::fflush(stderr);
                return true;
            }();
            (void)s_tagLogOnce;
        }
    }

    // --- Restore state (exact) ---
    glBindVertexArray(prevVAO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex2DBind);
    glActiveTexture(prevActiveTex);
    glUseProgram(prevProgram);

    for (int i = 0; i < nAtt; ++i) {
        glColorMaski((GLuint)i,
            prevMask[i][0], prevMask[i][1], prevMask[i][2], prevMask[i][3]);
    }

    glDepthMask(prevDepthMask);
    if (prevDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prevBlend)     glEnable(GL_BLEND);      else glDisable(GL_BLEND);
    if (prevCull)      glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);

    // SKYBOX-FOG-EXCLUDE-1: restore stencil state exactly (mirrors the
    // depth/blend/cull restore pattern above).
    if (skyExclude) {
        if (prevStencilTest) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
        glStencilFunc((GLenum)prevStencilFunc, prevStencilRef, (GLuint)prevStencilValueMask);
        glStencilOp((GLenum)prevStencilFail, (GLenum)prevStencilZFail, (GLenum)prevStencilZPass);
        glStencilMask((GLuint)prevStencilWriteMask);
    }

    // --- Phase 3: GL-state probe (read-only; never calls glDrawBuffers). ---
    // After the restore block above, every piece of state we touched must match
    // what we saved. Logs any mismatch; does not mutate state.
    if (s_stateProbe) {
        GLboolean curDepthMask = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &curDepthMask);
        const GLboolean curDepthTest = glIsEnabled(GL_DEPTH_TEST);
        const GLboolean curBlend     = glIsEnabled(GL_BLEND);
        const GLboolean curCull      = glIsEnabled(GL_CULL_FACE);
        GLint curFbo = 0, curDrawBuf0 = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &curFbo);
        glGetIntegerv(GL_DRAW_BUFFER0, &curDrawBuf0);
        GLboolean curMask0[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        glGetBooleani_v(GL_COLOR_WRITEMASK, 0, curMask0);

        int mismatch = 0;
        if (curDepthMask != prevDepthMask) mismatch |= 1;
        if (curDepthTest != prevDepthTest) mismatch |= 2;
        if (curBlend     != prevBlend)     mismatch |= 4;
        if (curCull      != prevCull)      mismatch |= 8;
        if (curFbo       != probeFbo)      mismatch |= 16;
        if (curDrawBuf0  != probeDrawBuf0) mismatch |= 32;
        for (int k = 0; k < 4; ++k)
            if (curMask0[k] != probeMask0[k]) mismatch |= 64;

        if (mismatch != 0) {
            std::fprintf(stderr,
                "[HDRI_SKY v1] state_probe MISMATCH bits=0x%02x "
                "depthMask(%d->%d) depthTest(%d->%d) blend(%d->%d) cull(%d->%d) "
                "fbo(%d->%d) drawBuf0(0x%x->0x%x) "
                "mask0(%d%d%d%d->%d%d%d%d)\n",
                mismatch,
                prevDepthMask, curDepthMask, prevDepthTest, curDepthTest,
                prevBlend, curBlend, prevCull, curCull,
                probeFbo, curFbo, probeDrawBuf0, curDrawBuf0,
                probeMask0[0], probeMask0[1], probeMask0[2], probeMask0[3],
                curMask0[0], curMask0[1], curMask0[2], curMask0[3]);
        } else {
            std::fprintf(stderr, "[HDRI_SKY v1] state_probe ok\n");
        }
        std::fflush(stderr);
    }

    // Note: do NOT call glDrawBuffers anywhere in this function.
    // setSceneDrawBuffers owns the FBO draw-buffer array.
}

// HDRI-SKY frame fix (MC2_HDRI_SKY_FRAME_FIX ON path). Reconstructs the world
// ray directly from the camera WORLD-space basis (raw MC2 frame x=east,
// y=north, z=elevation) — no matrix inversion, so the kPixelHomogToGLNDC
// X/Y-flip never enters the ray. GL state save/mask/restore mirrors
// renderHdriSkybox() exactly; only the direction-feeding uniforms and the
// equirect frame differ (frameFix=1; sun azimuth = atan2(north,east)).
void gosPostProcess::renderHdriSkyboxBasis(const float* camFwd,
                                           const float* camRight,
                                           const float* camUp,
                                           float tHX, float tHY)
{
    if (!hdriReady_ || !hdriSkyboxProg_ || !hdriSkyboxProg_->is_valid()
        || !hdriTex_ || !camFwd || !camRight || !camUp) {
        return;  // no-op: black sky baseline
    }

    GLint maxDrawBuffers = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
    const int nAtt = (maxDrawBuffers < 3) ? maxDrawBuffers : 3;

    // --- Save state ---
    GLboolean prevDepthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    GLboolean prevDepthTest  = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevBlend      = glIsEnabled(GL_BLEND);
    GLboolean prevCull       = glIsEnabled(GL_CULL_FACE);
    GLint     prevActiveTex  = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
    glActiveTexture(GL_TEXTURE0);
    GLint     prevTex2DBind  = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2DBind);
    GLint     prevProgram    = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    GLint     prevVAO        = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);

    GLboolean prevMask[3][4] = {
        { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE },
        { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE },
        { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE },
    };
    for (int i = 0; i < nAtt; ++i) {
        glGetBooleani_v(GL_COLOR_WRITEMASK, (GLuint)i, prevMask[i]);
    }

    if (nAtt > 0) glColorMaski(0, GL_TRUE,  GL_TRUE,  GL_TRUE,  GL_TRUE);
    if (nAtt > 1) glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    if (nAtt > 2) glColorMaski(2, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    // --- Sub-flag resolution (env read once). ---
    static const int   s_uvDebug = []() {
        const char* e = getenv("MC2_HDRI_SKY_UV_DEBUG");
        return (e && e[0] != '0') ? 1 : 0;
    }();
    static const bool  s_azOffsetSet = (getenv("MC2_HDRI_SKY_AZ_OFFSET") != nullptr);
    static const float s_azOffsetRad = []() {
        const char* e = getenv("MC2_HDRI_SKY_AZ_OFFSET");
        return e ? (float)(atof(e) * 3.14159265358979323846 / 180.0) : 0.0f;
    }();
    static const bool  s_stateProbe = (getenv("MC2_HDRI_SKY_STATE_PROBE") != nullptr);

    // skyYaw. Default (no offset env) => 0.0 => no rotation. Frame-consistent
    // with the MC2-frame equirect: azimuth = atan2(north, east) = atan2(ly, lx)
    // (+lx, NOT -lx — the matrix path's -lx was for the GL Y-up equirect).
    float skyYaw = 0.0f;
    if (s_azOffsetSet) {
        float sunSyncYaw = 0.0f;
        if (hdriBakedSunValid_) {
            float lx = 0.0f, ly = 0.0f, lz = 0.0f;
            gos_GetTerrainLightDir(&lx, &ly, &lz);
            // MC2-frame equirect azimuth of the mission sun.
            const float sunAzGL = atan2f(ly, lx);
            sunSyncYaw = hdriBakedSunAz_ - sunAzGL;
        }
        skyYaw = sunSyncYaw + s_azOffsetRad;
    }

    if (s_uvDebug) {
        static bool s_loggedCenterBasis = false;
        if (!s_loggedCenterBasis) {
            s_loggedCenterBasis = true;
            // Center pixel: vNdc=(0,0) -> worldDir = normalize(camFwd).
            float wd[3] = { camFwd[0], camFwd[1], camFwd[2] };
            float len = std::sqrt(wd[0]*wd[0] + wd[1]*wd[1] + wd[2]*wd[2]);
            if (len > 0.0f) { wd[0]/=len; wd[1]/=len; wd[2]/=len; }
            std::fprintf(stderr,
                "[HDRI_SKY framefix] uv_debug center_worldDir=(%.3f,%.3f,%.3f) "
                "tHX=%.4f tHY=%.4f bakedSunValid=%d bakedSunAz=%.4f skyYaw=%.4f\n",
                wd[0], wd[1], wd[2], tHX, tHY, hdriBakedSunValid_ ? 1 : 0,
                hdriBakedSunAz_, skyYaw);
            std::fflush(stderr);
        }
    }

    GLint probeFbo = 0, probeDrawBuf0 = 0;
    GLboolean probeMask0[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    if (s_stateProbe) {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &probeFbo);
        glGetIntegerv(GL_DRAW_BUFFER0, &probeDrawBuf0);
        glGetBooleani_v(GL_COLOR_WRITEMASK, 0, probeMask0);
    }

    // Bind shader + uniforms + texture (direct-basis path).
    skyYaw_ = skyYaw;  // WATER-HDRI-REFL-1: cache for water shader uniform
    hdriSkyboxProg_->apply();
    hdriSkyboxProg_->setFloat3("camFwd",   camFwd);
    hdriSkyboxProg_->setFloat3("camRight", camRight);
    hdriSkyboxProg_->setFloat3("camUp",    camUp);
    hdriSkyboxProg_->setFloat("tHX", tHX);
    hdriSkyboxProg_->setFloat("tHY", tHY);
    hdriSkyboxProg_->setInt("uvDebug", s_uvDebug);
    hdriSkyboxProg_->setFloat("skyYaw", skyYaw);
    hdriSkyboxProg_->setInt("frameFix", 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hdriTex_);
    hdriSkyboxProg_->setInt("u_hdri", 0);

    if (quadVAO_ != 0) {
        glBindVertexArray(quadVAO_);
    } else {
        if (hdriDummyVao_ == 0) glGenVertexArrays(1, &hdriDummyVao_);
        glBindVertexArray(hdriDummyVao_);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // --- Restore state (exact) ---
    glBindVertexArray(prevVAO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex2DBind);
    glActiveTexture(prevActiveTex);
    glUseProgram(prevProgram);

    for (int i = 0; i < nAtt; ++i) {
        glColorMaski((GLuint)i,
            prevMask[i][0], prevMask[i][1], prevMask[i][2], prevMask[i][3]);
    }

    glDepthMask(prevDepthMask);
    if (prevDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prevBlend)     glEnable(GL_BLEND);      else glDisable(GL_BLEND);
    if (prevCull)      glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);

    if (s_stateProbe) {
        GLboolean curDepthMask = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &curDepthMask);
        const GLboolean curDepthTest = glIsEnabled(GL_DEPTH_TEST);
        const GLboolean curBlend     = glIsEnabled(GL_BLEND);
        const GLboolean curCull      = glIsEnabled(GL_CULL_FACE);
        GLint curFbo = 0, curDrawBuf0 = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &curFbo);
        glGetIntegerv(GL_DRAW_BUFFER0, &curDrawBuf0);
        GLboolean curMask0[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        glGetBooleani_v(GL_COLOR_WRITEMASK, 0, curMask0);

        int mismatch = 0;
        if (curDepthMask != prevDepthMask) mismatch |= 1;
        if (curDepthTest != prevDepthTest) mismatch |= 2;
        if (curBlend     != prevBlend)     mismatch |= 4;
        if (curCull      != prevCull)      mismatch |= 8;
        if (curFbo       != probeFbo)      mismatch |= 16;
        if (curDrawBuf0  != probeDrawBuf0) mismatch |= 32;
        for (int k = 0; k < 4; ++k)
            if (curMask0[k] != probeMask0[k]) mismatch |= 64;

        if (mismatch != 0) {
            std::fprintf(stderr,
                "[HDRI_SKY framefix] state_probe MISMATCH bits=0x%02x\n",
                mismatch);
        } else {
            std::fprintf(stderr, "[HDRI_SKY framefix] state_probe ok\n");
        }
        std::fflush(stderr);
    }
}

// HDRI-SKY frame fix (MC2_HDRI_SKY_FRAME_FIX ON path, one-proven-matrix).
// Reconstructs the sky ray by UNPROJECTING NDC through the inverse of
// worldToClipGL — the EXACT matrix the GPU rasterizes terrain with
// (= kAxisSwapMC2toGL * worldToCameraMatrix * cameraToClipGL, see
// mclib/camera.cpp:2839). Because that matrix's leftmost factor is the MC2->GL
// axis swap, its INVERSE maps clip-space straight back to the raw MC2 world
// frame (x=east, y=north, z=elevation, Z-up). No camera basis, no FOV, no
// handedness toggle — the proven engine matrix carries the whole frame.
// invVP16 is uploaded verbatim (same convention as renderHdriSkybox's invProj),
// so the shader does invWorldToClipGL * vec4(ndc, depth, 1.0). frameFix=2
// selects the inverse-VP path in the frag shader (1 = legacy camera-basis,
// 0 = byte-identical legacy matrix path). GL state save/mask/restore mirrors
// renderHdriSkybox() exactly.
void gosPostProcess::renderHdriSkyboxInvVP(const float* invVP16)
{
    if (!hdriReady_ || !hdriSkyboxProg_ || !hdriSkyboxProg_->is_valid()
        || !hdriTex_ || !invVP16) {
        return;  // no-op: black sky baseline
    }

    GLint maxDrawBuffers = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
    const int nAtt = (maxDrawBuffers < 3) ? maxDrawBuffers : 3;

    // --- Save state ---
    GLboolean prevDepthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    GLboolean prevDepthTest  = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevBlend      = glIsEnabled(GL_BLEND);
    GLboolean prevCull       = glIsEnabled(GL_CULL_FACE);
    GLint     prevActiveTex  = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
    glActiveTexture(GL_TEXTURE0);
    GLint     prevTex2DBind  = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2DBind);
    GLint     prevProgram    = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    GLint     prevVAO        = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);

    GLboolean prevMask[3][4] = {
        { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE },
        { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE },
        { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE },
    };
    for (int i = 0; i < nAtt; ++i) {
        glGetBooleani_v(GL_COLOR_WRITEMASK, (GLuint)i, prevMask[i]);
    }

    if (nAtt > 0) glColorMaski(0, GL_TRUE,  GL_TRUE,  GL_TRUE,  GL_TRUE);
    if (nAtt > 1) glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    if (nAtt > 2) glColorMaski(2, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    // --- Sub-flag resolution (env read once). ---
    // MC2_HDRI_SKY_FLIP_H / FLIP_V are intentionally NOT read here: the matrix
    // inverse needs no handedness toggle (they were camera-basis only).
    static const int   s_uvDebug = []() {
        const char* e = getenv("MC2_HDRI_SKY_UV_DEBUG");
        return (e && e[0] != '0') ? 1 : 0;
    }();
    static const bool  s_azOffsetSet = (getenv("MC2_HDRI_SKY_AZ_OFFSET") != nullptr);
    static const float s_azOffsetRad = []() {
        const char* e = getenv("MC2_HDRI_SKY_AZ_OFFSET");
        return e ? (float)(atof(e) * 3.14159265358979323846 / 180.0) : 0.0f;
    }();
    static const bool  s_stateProbe = (getenv("MC2_HDRI_SKY_STATE_PROBE") != nullptr);

    // skyYaw: auto-sync HDRI sun to terrain light direction when the scan found
    // a baked sun (hdriBakedSunValid_). The raw-MC2-frame equirect uses azimuth
    // = atan2(north, east) = atan2(ly, lx) and the frag rotates worldDir about
    // +Z (MC2 up) by skyYaw before atan2(y,x). MC2_HDRI_SKY_AZ_OFFSET adds a
    // manual trim on top of the auto-sync (or replaces it when scan failed).
    float skyYaw = 0.0f;
    if (hdriBakedSunValid_) {
        float lx = 0.0f, ly = 0.0f, lz = 0.0f;
        gos_GetTerrainLightDir(&lx, &ly, &lz);
        const float sunAzGL = atan2f(ly, lx);
        // hdriBakedSunAz_ is the azimuth of the anti-solar backscatter peak
        // (the brightest *diffuse* region in the upper half of the EXR, which
        // sits 180° opposite the actual sun disk). Add π to get the true disk.
        skyYaw = hdriBakedSunAz_ + 3.14159265f - sunAzGL;
    }
    if (s_azOffsetSet) {
        skyYaw += s_azOffsetRad;
    }
    {
        static bool s_loggedSync = false;
        if (!s_loggedSync) {
            s_loggedSync = true;
            float lx = 0.0f, ly = 0.0f, lz = 0.0f;
            gos_GetTerrainLightDir(&lx, &ly, &lz);
            std::fprintf(stderr,
                "[HDRI_SUN_SYNC] valid=%d antiSolarAz=%.1fdeg sunAzGL=%.1fdeg skyYaw=%.1fdeg\n",
                hdriBakedSunValid_ ? 1 : 0,
                hdriBakedSunAz_ * 57.29577951f,
                atan2f(ly, lx) * 57.29577951f,
                skyYaw * 57.29577951f);
            std::fflush(stderr);
        }
    }

    GLint probeFbo = 0, probeDrawBuf0 = 0;
    GLboolean probeMask0[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    if (s_stateProbe) {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &probeFbo);
        glGetIntegerv(GL_DRAW_BUFFER0, &probeDrawBuf0);
        glGetBooleani_v(GL_COLOR_WRITEMASK, 0, probeMask0);
    }

    // Bind shader + uniforms + texture (inverse-worldToClipGL path).
    skyYaw_ = skyYaw;  // WATER-HDRI-REFL-1: cache for water shader uniform
    hdriSkyboxProg_->apply();
    hdriSkyboxProg_->setMat4("invWorldToClipGL", invVP16);
    hdriSkyboxProg_->setInt("uvDebug", s_uvDebug);
    hdriSkyboxProg_->setFloat("skyYaw", skyYaw);
    hdriSkyboxProg_->setInt("frameFix", 2);   // 2 = inverse-VP unproject path
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hdriTex_);
    hdriSkyboxProg_->setInt("u_hdri", 0);

    if (s_uvDebug) {
        static bool s_loggedCenterInvVP = false;
        if (!s_loggedCenterInvVP) {
            s_loggedCenterInvVP = true;
            // Reconstruct the center-pixel ray CPU-side the same way the frag
            // does (vNdc=(0,0); near depth=1, far depth=0 reverse-Z), so the log
            // is independent of whether the gradient draw is visible. invVP16 is
            // column-major: world_i = sum_j invVP16[j*4+i] * clip_j.
            auto unproj = [&](float dz, float out[3]) {
                const float c[4] = { 0.0f, 0.0f, dz, 1.0f };
                float w4[4];
                for (int i = 0; i < 4; ++i) {
                    w4[i] = invVP16[0*4+i]*c[0] + invVP16[1*4+i]*c[1]
                          + invVP16[2*4+i]*c[2] + invVP16[3*4+i]*c[3];
                }
                const float iw = (w4[3] != 0.0f) ? (1.0f / w4[3]) : 0.0f;
                out[0] = w4[0]*iw; out[1] = w4[1]*iw; out[2] = w4[2]*iw;
            };
            float wn[3], wf[3];
            unproj(1.0f, wn);   // near (reverse-Z near=1)
            unproj(0.0f, wf);   // far  (reverse-Z far=0)
            float wd[3] = { wf[0]-wn[0], wf[1]-wn[1], wf[2]-wn[2] };
            float len = std::sqrt(wd[0]*wd[0] + wd[1]*wd[1] + wd[2]*wd[2]);
            if (len > 0.0f) { wd[0]/=len; wd[1]/=len; wd[2]/=len; }
            std::fprintf(stderr,
                "[HDRI_SKY invVP] uv_debug center_worldDir=(%.3f,%.3f,%.3f) "
                "(Z-up: z=elevation) bakedSunValid=%d bakedSunAz=%.4f skyYaw=%.4f\n",
                wd[0], wd[1], wd[2], hdriBakedSunValid_ ? 1 : 0,
                hdriBakedSunAz_, skyYaw);
            std::fflush(stderr);
        }
    }

    if (quadVAO_ != 0) {
        glBindVertexArray(quadVAO_);
    } else {
        if (hdriDummyVao_ == 0) glGenVertexArrays(1, &hdriDummyVao_);
        glBindVertexArray(hdriDummyVao_);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // --- Restore state (exact) ---
    glBindVertexArray(prevVAO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex2DBind);
    glActiveTexture(prevActiveTex);
    glUseProgram(prevProgram);

    for (int i = 0; i < nAtt; ++i) {
        glColorMaski((GLuint)i,
            prevMask[i][0], prevMask[i][1], prevMask[i][2], prevMask[i][3]);
    }

    glDepthMask(prevDepthMask);
    if (prevDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prevBlend)     glEnable(GL_BLEND);      else glDisable(GL_BLEND);
    if (prevCull)      glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);

    if (s_stateProbe) {
        GLboolean curDepthMask = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &curDepthMask);
        const GLboolean curDepthTest = glIsEnabled(GL_DEPTH_TEST);
        const GLboolean curBlend     = glIsEnabled(GL_BLEND);
        const GLboolean curCull      = glIsEnabled(GL_CULL_FACE);
        GLint curFbo = 0, curDrawBuf0 = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &curFbo);
        glGetIntegerv(GL_DRAW_BUFFER0, &curDrawBuf0);
        GLboolean curMask0[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        glGetBooleani_v(GL_COLOR_WRITEMASK, 0, curMask0);

        int mismatch = 0;
        if (curDepthMask != prevDepthMask) mismatch |= 1;
        if (curDepthTest != prevDepthTest) mismatch |= 2;
        if (curBlend     != prevBlend)     mismatch |= 4;
        if (curCull      != prevCull)      mismatch |= 8;
        if (curFbo       != probeFbo)      mismatch |= 16;
        if (curDrawBuf0  != probeDrawBuf0) mismatch |= 32;
        for (int k = 0; k < 4; ++k)
            if (curMask0[k] != probeMask0[k]) mismatch |= 64;

        if (mismatch != 0) {
            std::fprintf(stderr,
                "[HDRI_SKY invVP] state_probe MISMATCH bits=0x%02x\n",
                mismatch);
        } else {
            std::fprintf(stderr, "[HDRI_SKY invVP] state_probe ok\n");
        }
        std::fflush(stderr);
    }
}

void gosPostProcess::initShadows()
{
    // Static shadow map covers the whole playable map in one ortho frustum, so
    // texel density is shadowMapSize²/(mapHalfExtent*2)². 4096² = 16M texels vs 2048² = 4M;
    // quadruples per-texel density, directly reduces stair-step banding on cliffs.
    shadowMapSize_ = 4096;

    static const char* kShaderPrefix = "#version 430\n";
    shadowDepthProg_ = glsl_program::makeProgram("shadow_depth",
        "shaders/shadow_depth.vert", "shaders/shadow_depth.frag", kShaderPrefix);

    glGenFramebuffers(1, &shadowFBO_);
    RenderCore::framegraph::fboLedger().registerFbo(shadowFBO_, RenderCore::RenderResourceId::ShadowStaticMap);  // FBO-LEDGER-1
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFBO_);

    // TEX-CLASS: render-target -- STATIC shadow map depth (FBO attach)
    glGenTextures(1, &shadowDepthTex_);
    glBindTexture(GL_TEXTURE_2D, shadowDepthTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
        shadowMapSize_, shadowMapSize_, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    float borderColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowDepthTex_, 0);

    // Dummy color attachment — AMD drivers skip rasterization on depth-only FBOs
    // TEX-CLASS: render-target -- AMD dummy color for depth-only FBO
    glGenTextures(1, &shadowDummyColorTex_);
    glBindTexture(GL_TEXTURE_2D, shadowDummyColorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8,
        shadowMapSize_, shadowMapSize_, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shadowDummyColorTex_, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_NONE);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "gosPostProcess: shadow FBO incomplete\n");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Initialize with identity so shadow map reads white (no shadow)
    memset(staticLightSpaceMatrix_, 0, sizeof(staticLightSpaceMatrix_));
    staticLightSpaceMatrix_[0] = staticLightSpaceMatrix_[5] = staticLightSpaceMatrix_[10] = staticLightSpaceMatrix_[15] = 1.0f;
    staticLightMatrixBuilt_ = false;

    // Clear shadow map to max depth (1.0) so everything is "lit".
    // Reverse-Z (U2) state-safe partition: the scene sets glClearDepth(0);
    // the shadow path stays forward-Z, so set glClearDepth(1.0f)
    // explicitly here and restore the scene reverse-Z default after.
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFBO_);
    glClearDepth(1.0f);
    glClear(GL_DEPTH_BUFFER_BIT);
    glClearDepth(0.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::ShadowStaticMap;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.lifetime  = RenderCore::RenderResourceLifetime::Persistent;  // built outside dynamic frame order, long-lived
        d.format    = RenderCore::RenderResourceFormat::Depth24;
        d.debugName = "ShadowStaticMap";
        d.width     = static_cast<uint32_t>(shadowMapSize_);
        d.height    = static_cast<uint32_t>(shadowMapSize_);
        d.glName    = static_cast<uint32_t>(shadowDepthTex_);
        d.sizeBytes = static_cast<uint64_t>(shadowMapSize_) * static_cast<uint64_t>(shadowMapSize_) * 4u;
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }
}

void gosPostProcess::beginShadowPass()
{
    if (!shadowsEnabled_ || !shadowFBO_) return;

    glGetIntegerv(GL_VIEWPORT, savedViewport_);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFBO_);
    glViewport(0, 0, shadowMapSize_, shadowMapSize_);
    // Reverse-Z (U2) state-safe partition: shadow stays forward-Z; scene
    // set glClearDepth(0), so force 1.0f here and restore 0 after.
    glClearDepth(1.0f);
    glClear(GL_DEPTH_BUFFER_BIT);
    glClearDepth(0.0f);

    // Force depth test and writing ON
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    // Polygon offset to reduce shadow acne
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(shadowBiasFactor_, shadowBiasUnits_);

    // Only need depth — disable color writes
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    // Disable culling so both faces write depth
    glDisable(GL_CULL_FACE);
}

void gosPostProcess::beginShadowPassNoClear()
{
    if (!shadowsEnabled_ || !shadowFBO_) return;

    glGetIntegerv(GL_VIEWPORT, savedViewport_);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFBO_);
    glViewport(0, 0, shadowMapSize_, shadowMapSize_);
    // NO glClear — accumulate depth from previous frames

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(shadowBiasFactor_, shadowBiasUnits_);

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDisable(GL_CULL_FACE);
}

void gosPostProcess::endShadowPass()
{
    if (!shadowsEnabled_ || !shadowFBO_) return;

    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_); // restore to scene FBO
    glViewport(savedViewport_[0], savedViewport_[1], savedViewport_[2], savedViewport_[3]);

}

void gosPostProcess::destroyShadows()
{
    if (shadowFBO_) { glDeleteFramebuffers(1, &shadowFBO_); shadowFBO_ = 0; }
    if (shadowDepthTex_) { glDeleteTextures(1, &shadowDepthTex_); shadowDepthTex_ = 0; }
    if (shadowDummyColorTex_) { glDeleteTextures(1, &shadowDummyColorTex_); shadowDummyColorTex_ = 0; }
    if (shadowDepthProg_) {
        glsl_program::deleteProgram("shadow_depth");
        shadowDepthProg_ = nullptr;
    }

    RenderCore::RenderResourceDesc invalid;
    invalid.id = RenderCore::RenderResourceId::ShadowStaticMap;
    RenderCore::registerOrUpdateRenderResource(invalid);
}

// CP-1: per-mission reset of process-scoped static-shadow priming state.
// Resets the static light matrix built flag so the next mission rebuilds it
// against fresh blocks[]. The gos_*ShadowRebuild* one-shot-flag API was
// RETIRED with the move to the build-once full-map static shadow (see the
// note in gameos_graphics.cpp by gos_ResetStaticLightMatrix); the matrix
// rebuild alone re-primes the accumulation, no camera-motion trigger.
void gos_ResetStaticShadowPriming()
{
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->resetStaticLightMatrix();
}

// SHADOW-ROBUST-BASIS-1 (gate MC2_SHADOW_ROBUST_BASIS, DEFAULT ON; =0 kill).
// Build an orthonormal light-space basis (right/up) from the normalized sun
// forward (fx,fy,fz).
//
// The legacy up-hint pick is up=(0,0,1), switched to (0,1,0) when |fz|>0.9 (a
// single hard threshold). For all normal mission suns that pick yields a
// well-conditioned cross, so we KEEP it as the primary path -- the robust
// guard must NOT perturb the shadow texel-grid orientation of cases that
// already work. The guard fires ONLY when the legacy cross degenerates
// (length near zero -- the documented basis singularity at a sun aligned with
// the chosen up-axis): it then re-picks up as the world axis LEAST parallel to
// the sun, guaranteeing |dot(sun,up)| <= 1/sqrt(3) so the cross length is
// >= sqrt(2/3) ~ 0.816. Default-ON is thus byte-identical to legacy except at
// the singularity; =0 disables the guard entirely (pure legacy, can go
// singular).
static void mc2ComputeLightBasis(float fx, float fy, float fz,
                                 float& rx, float& ry, float& rz,
                                 float& ux, float& uy, float& uz)
{
    static const bool s_robustGuard = []() {
        const char* v = getenv("MC2_SHADOW_ROBUST_BASIS");
        return !(v && v[0] == '0');   // default ON
    }();

    // Legacy up-hint pick (primary path; unchanged behavior).
    ux = 0.0f; uy = 0.0f; uz = 1.0f;
    if (fabsf(fz) > 0.9f) { ux = 0.0f; uy = 1.0f; uz = 0.0f; }

    rx = fy * uz - fz * uy;
    ry = fz * ux - fx * uz;
    rz = fx * uy - fy * ux;
    float len = sqrtf(rx*rx + ry*ry + rz*rz);

    if (s_robustGuard && len < 1e-3f) {
        // Singularity: legacy up-hint is ~parallel to the sun. Re-pick up as
        // the world axis least parallel to the sun and recompute.
        const float ax = fabsf(fx), ay = fabsf(fy), az = fabsf(fz);
        ux = uy = uz = 0.0f;
        if (az <= ax && az <= ay)      uz = 1.0f;
        else if (ay <= ax && ay <= az) uy = 1.0f;
        else                           ux = 1.0f;
        rx = fy * uz - fz * uy;
        ry = fz * ux - fx * uz;
        rz = fx * uy - fy * ux;
        len = sqrtf(rx*rx + ry*ry + rz*rz);
    }

    if (len < 1e-6f) len = 1.0f;   // final paranoia guard (unreachable)
    rx /= len; ry /= len; rz /= len;

    ux = ry * fz - rz * fy;
    uy = rz * fx - rx * fz;
    uz = rx * fy - ry * fx;
}

void gosPostProcess::buildStaticLightMatrix(float sunDirX, float sunDirY, float sunDirZ,
                                             float mapHalfExtent)
{
    if (!shadowsEnabled_ || !shadowFBO_) return;
    if (staticLightMatrixBuilt_) return;

    // Build world-fixed orthographic light-space matrix centered at map origin
    // sunDir points light→scene (already negated by caller)
    float len = sqrtf(sunDirX*sunDirX + sunDirY*sunDirY + sunDirZ*sunDirZ);
    if (len < 0.001f) return;
    float fx = sunDirX/len, fy = sunDirY/len, fz = sunDirZ/len;

    // Map center is origin (0,0,0) in MC2 world space
    float r = mapHalfExtent * sqrtf(2.0f) * 1.05f;  // covers full map diagonal at any sun angle
    float lightPosX = -fx * r;
    float lightPosY = -fy * r;
    float lightPosZ = -fz * r;

    // Right = cross(forward, up_hint); Z-up for MC2 (robust basis, SHADOW-ROBUST-BASIS-1)
    float rx, ry, rz, ux, uy, uz;
    mc2ComputeLightBasis(fx, fy, fz, rx, ry, rz, ux, uy, uz);

    float view[16] = {
         rx,  ux, -fx, 0,
         ry,  uy, -fy, 0,
         rz,  uz, -fz, 0,
        -(rx*lightPosX + ry*lightPosY + rz*lightPosZ),
        -(ux*lightPosX + uy*lightPosY + uz*lightPosZ),
         (fx*lightPosX + fy*lightPosY + fz*lightPosZ),
        1
    };

    // Ortho covers full map; near/far envelope the full elevation range.
    // Z-row emits clip-z in [0,1] (near->0, far->1) to match the engine-global
    // glClipControl(GL_ZERO_TO_ONE) set in gameosmain.cpp. Mirrors the scene
    // ortho precedent camera.cpp:2032/2037; the classic [-1,1] form clipped the
    // near half of the light frustum away (wedge atlas / half-map shadow).
    float nearP = 1.0f, farP = 2.0f * r;
    float ortho[16] = {
        1.0f/r, 0, 0, 0,
        0, 1.0f/r, 0, 0,
        0, 0, -1.0f/(farP - nearP), 0,
        0, 0, -nearP/(farP - nearP), 1
    };

    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0;
            for (int k = 0; k < 4; k++)
                sum += ortho[k * 4 + row] * view[col * 4 + k];
            staticLightSpaceMatrix_[col * 4 + row] = sum;
        }
    }

    {
        RenderCore::EngineView sv;
        sv.id        = RenderCore::kShadowDirectional0ViewId;
        sv.kind      = RenderCore::ViewKind::ShadowStatic;
        sv.debugName = "ShadowDirectional0-Static";
        sv.viewport[2] = shadowMapSize_;
        sv.viewport[3] = shadowMapSize_;
        memcpy(sv.viewUniforms.worldToClipGL, staticLightSpaceMatrix_,
               sizeof(sv.viewUniforms.worldToClipGL));
        RenderCore::registerOrUpdateView(sv);
    }

    fprintf(stderr, "gosPostProcess: rendering static shadows (map half-extent=%.0f)\n", mapHalfExtent);

    // [SHADOWFRUSTUM v1] VPL-#shadow: prove the static-shadow CLIPPER is
    // correct (user suspected light/clipper; render-expert grep says it's
    // feed-scope, not the matrix). INERT, env MC2_DEBUG_SHADOW_FRUSTUM,
    // one-shot (function early-returns on staticLightMatrixBuilt_ latch).
    // Input scalars only -- NO staticLightSpaceMatrix_[] slot reads (the
    // matrix-index discipline lesson). If orthoHalf == mapHalfExtent*1.485
    // and the map's real half-size, the clipper covers the FULL map ->
    // the bug is definitively the FEED (camera-windowed terrain), not the
    // light/clipper. buildCount>1 would mean the latch resets (rebuild).
    if (getenv("MC2_DEBUG_SHADOW_FRUSTUM") != nullptr) {
        static int s_buildCount = 0;
        ++s_buildCount;
        fprintf(stderr,
            "[SHADOWFRUSTUM v1] event=build n=%d sunDirIn=(%.4f,%.4f,%.4f) "
            "sunDirNorm=(%.4f,%.4f,%.4f) mapHalfExtent=%.1f orthoHalf(r)=%.1f "
            "near=%.2f far=%.2f coversWorldXY=[-%.1f,%.1f]\n",
            s_buildCount, sunDirX, sunDirY, sunDirZ, fx, fy, fz,
            mapHalfExtent, r, nearP, farP, r, r);
    }

    // [SHADOWZRANGE v1] VPL-#10: prove the [0,1] ZERO_TO_ONE conversion is
    // correct. Transform map center + 4 corners (z=0) through the COMPOSITE
    // staticLightSpaceMatrix_ (column-major: [col*4+row]); every clip.z/clip.w
    // MUST land in [0,1]. A sign error in the ortho z-row shows here as an
    // out-of-range value BEFORE any GPU round-trip. Unconditional env-gated
    // fprintf (assert is a no-op under RelWithDebInfo); one-shot via the
    // staticLightMatrixBuilt_ latch.
    if (getenv("MC2_DEBUG_SHADOW_ZRANGE") != nullptr) {
        const float* M = staticLightSpaceMatrix_;
        const float pts[5][3] = {
            { 0.0f, 0.0f, 0.0f },
            {  0.95f*r,  0.95f*r, 0.0f }, { -0.95f*r,  0.95f*r, 0.0f },
            {  0.95f*r, -0.95f*r, 0.0f }, { -0.95f*r, -0.95f*r, 0.0f }
        };
        for (int i = 0; i < 5; i++) {
            float px = pts[i][0], py = pts[i][1], pz = pts[i][2];
            float cz = M[0*4+2]*px + M[1*4+2]*py + M[2*4+2]*pz + M[3*4+2];
            float cw = M[0*4+3]*px + M[1*4+3]*py + M[2*4+3]*pz + M[3*4+3];
            float ndc = (cw != 0.0f) ? cz / cw : 0.0f;
            fprintf(stderr,
                "[SHADOWZRANGE v1] event=static pt=%d world=(%.0f,%.0f,%.0f) "
                "clipZ=%.5f clipW=%.5f ndcZ=%.5f inRange=%d\n",
                i, px, py, pz, cz, cw, ndc, (ndc >= 0.0f && ndc <= 1.0f) ? 1 : 0);
        }
    }
}

void gosPostProcess::initDynamicShadows()
{
    // Dynamic shadow covers radius=1200 around frustum center, so at 2048²
    // each texel ≈ 1.17 world units — much bigger than a mech foot, hence blocky
    // mech shadow edges. 4096² → ~0.59 world units/texel (half the step).
    dynShadowMapSize_ = mc2ShadowMapSize();   // CSM-REDESIGN: env-tunable, default 8192

    glGenFramebuffers(1, &dynShadowFBO_);
    RenderCore::framegraph::fboLedger().registerFbo(dynShadowFBO_, RenderCore::RenderResourceId::ShadowDynamicMap);  // FBO-LEDGER-1
    glBindFramebuffer(GL_FRAMEBUFFER, dynShadowFBO_);

    // TEX-CLASS: render-target -- DYNAMIC shadow map depth (FBO attach)
    glGenTextures(1, &dynShadowDepthTex_);
    glBindTexture(GL_TEXTURE_2D, dynShadowDepthTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
        dynShadowMapSize_, dynShadowMapSize_, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    float borderColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, dynShadowDepthTex_, 0);

    // AMD dummy color attachment
    // TEX-CLASS: render-target -- AMD dummy color for depth-only FBO
    glGenTextures(1, &dynShadowDummyColorTex_);
    glBindTexture(GL_TEXTURE_2D, dynShadowDummyColorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8,
        dynShadowMapSize_, dynShadowMapSize_, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dynShadowDummyColorTex_, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_NONE);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "gosPostProcess: dynamic shadow FBO incomplete\n");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    memset(dynamicLightSpaceMatrix_, 0, sizeof(dynamicLightSpaceMatrix_));
    dynamicLightSpaceMatrix_[0] = dynamicLightSpaceMatrix_[5] = dynamicLightSpaceMatrix_[10] = dynamicLightSpaceMatrix_[15] = 1.0f;

    // Clear to max depth (fully lit). Reverse-Z (U2) state-safe partition:
    // dynamic shadow stays forward-Z; scene set glClearDepth(0), so force
    // 1.0f here and restore the scene reverse-Z default after.
    glBindFramebuffer(GL_FRAMEBUFFER, dynShadowFBO_);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0f);
    glClear(GL_DEPTH_BUFFER_BIT);
    glClearDepth(0.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::ShadowDynamicMap;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.lifetime  = RenderCore::RenderResourceLifetime::Persistent;  // long-lived dynamic shadow target, recreated on resize
        d.format    = RenderCore::RenderResourceFormat::Depth24;
        d.debugName = "ShadowDynamicMap";
        d.width     = static_cast<uint32_t>(dynShadowMapSize_);
        d.height    = static_cast<uint32_t>(dynShadowMapSize_);
        d.glName    = static_cast<uint32_t>(dynShadowDepthTex_);
        d.sizeBytes = static_cast<uint64_t>(dynShadowMapSize_) * static_cast<uint64_t>(dynShadowMapSize_) * 4u;
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    // === Item 1 P1: cascaded depth array (only when MC2_SHADOW_CSM is ON) ===
    // GL_DEPTH_COMPONENT24 GL_TEXTURE_2D_ARRAY (dynShadowMapSize_^2 x N) with the
    // EXACT sampler param set the legacy single dynamic map uses:
    // COMPARE_REF_TO_TEXTURE / GL_LEQUAL / CLAMP_TO_BORDER / border 1.0. The
    // border is load-bearing: missing it yields garbage edge cascades.
    if (mc2ShadowCsmEnabled()) {
        csmCount_ = mc2ShadowCsmCount();

        // Per-cascade shadow resolution: the array holds only the NEAR cascades
        // (0..csmCount_-2). The LAST cascade (csmCount_-1, map-centered full-map)
        // renders into the separate dynamicFullMapTex_ below. arrayLayers is the
        // near-cascade count, floored at 1 so the GL array texture is always
        // valid even when csmCount_==1 (no near cascades -> layer 0 unused).
        const int nearCascades = (csmCount_ > 1) ? (csmCount_ - 1) : 0;
        const int arrayLayers  = (nearCascades > 0) ? nearCascades : 1;

        // TEX-CLASS: render-target -- CSM near-cascade 2D_ARRAY (layered FBO attach)
        glGenTextures(1, &dynShadowArrayTex_);
        glBindTexture(GL_TEXTURE_2D_ARRAY, dynShadowArrayTex_);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24,
            dynShadowMapSize_, dynShadowMapSize_, arrayLayers,
            0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        float csmBorder[] = {1.0f, 1.0f, 1.0f, 1.0f};
        glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, csmBorder);

        // Dummy color array (7900 XTX FBO-completeness workaround, mirrors the
        // legacy dummy at the single-map FBO above). One layer per cascade.
        // TEX-CLASS: render-target -- AMD dummy color array
        glGenTextures(1, &dynShadowArrayDummyColorTex_);
        glBindTexture(GL_TEXTURE_2D_ARRAY, dynShadowArrayDummyColorTex_);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8,
            dynShadowMapSize_, dynShadowMapSize_, arrayLayers,
            0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

        // One FBO; per-cascade we attach a single layer via glFramebufferTextureLayer.
        glGenFramebuffers(1, &dynShadowArrayFBO_);
        RenderCore::framegraph::fboLedger().registerFbo(dynShadowArrayFBO_, RenderCore::RenderResourceId::ShadowDynamicMap);  // FBO-LEDGER-1
        glBindFramebuffer(GL_FRAMEBUFFER, dynShadowArrayFBO_);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, dynShadowArrayTex_, 0, 0);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, dynShadowArrayDummyColorTex_, 0, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_NONE);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "gosPostProcess: CSM dynamic shadow array FBO incomplete\n");

        // Forward-Z clear all layers to 1.0 (fully lit). Reverse-Z partition:
        // dynamic shadow stays forward-Z; restore scene clear-depth after.
        glDepthMask(GL_TRUE);
        glClearDepth(1.0f);
        for (int i = 0; i < arrayLayers; ++i) {
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, dynShadowArrayTex_, 0, i);
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, dynShadowArrayDummyColorTex_, 0, i);
            glClear(GL_DEPTH_BUFFER_BIT);
        }
        glClearDepth(0.0f);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // === Per-cascade shadow resolution: separate full-map (last) cascade ===
        // The last cascade (csmCount_-1) renders into this lower-res 2D depth
        // texture. Same sampler param set as the array (COMPARE_REF_TO_TEXTURE /
        // GL_LEQUAL / CLAMP_TO_BORDER / border 1.0). When the SEPARATE env is OFF
        // we force size == dynShadowMapSize_ (equivalent VRAM to old all-in-array).
        dynamicFullMapSize_ = mc2ShadowFullMapSeparate()
                              ? mc2ShadowFullMapSize()
                              : dynShadowMapSize_;

        // TEX-CLASS: render-target -- CSM full-map cascade depth (FBO attach)
        glGenTextures(1, &dynamicFullMapTex_);
        glBindTexture(GL_TEXTURE_2D, dynamicFullMapTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
            dynamicFullMapSize_, dynamicFullMapSize_, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        float fmBorder[] = {1.0f, 1.0f, 1.0f, 1.0f};
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, fmBorder);

        // 7900 XTX FBO-completeness dummy color (mirrors the array/single-map FBOs).
        // TEX-CLASS: render-target -- AMD dummy color
        glGenTextures(1, &dynamicFullMapDummyColorTex_);
        glBindTexture(GL_TEXTURE_2D, dynamicFullMapDummyColorTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8,
            dynamicFullMapSize_, dynamicFullMapSize_, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        glGenFramebuffers(1, &dynamicFullMapFbo_);
        RenderCore::framegraph::fboLedger().registerFbo(dynamicFullMapFbo_, RenderCore::RenderResourceId::DynamicFullMapFbo);  // FBO-LEDGER-EXTEND-1
        glBindFramebuffer(GL_FRAMEBUFFER, dynamicFullMapFbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, dynamicFullMapTex_, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dynamicFullMapDummyColorTex_, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_NONE);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "gosPostProcess: CSM dynamic full-map FBO incomplete\n");

        // Forward-Z clear to 1.0 (fully lit), restore scene reverse-Z default.
        glDepthMask(GL_TRUE);
        glClearDepth(1.0f);
        glClear(GL_DEPTH_BUFFER_BIT);
        glClearDepth(0.0f);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);

        // Cascade matrices start as identity so an early sample is a no-op.
        for (int i = 0; i < kMaxCsmCascades; ++i) {
            dynamicCascadeMatrices_[i*16 + 0]  = 1.0f;
            dynamicCascadeMatrices_[i*16 + 5]  = 1.0f;
            dynamicCascadeMatrices_[i*16 + 10] = 1.0f;
            dynamicCascadeMatrices_[i*16 + 15] = 1.0f;
        }

        fprintf(stderr, "[CSM] init nearLayers=%d arraySize=%d fullMapSize=%d separate=%d\n",
                nearCascades, dynShadowMapSize_, dynamicFullMapSize_,
                mc2ShadowFullMapSeparate() ? 1 : 0);
    }
}

void gosPostProcess::destroyDynamicShadows()
{
    if (dynShadowFBO_) { glDeleteFramebuffers(1, &dynShadowFBO_); dynShadowFBO_ = 0; }
    if (dynShadowDepthTex_) { glDeleteTextures(1, &dynShadowDepthTex_); dynShadowDepthTex_ = 0; }
    if (dynShadowDummyColorTex_) { glDeleteTextures(1, &dynShadowDummyColorTex_); dynShadowDummyColorTex_ = 0; }
    if (dynShadowArrayFBO_) { glDeleteFramebuffers(1, &dynShadowArrayFBO_); dynShadowArrayFBO_ = 0; }
    if (dynShadowArrayTex_) { glDeleteTextures(1, &dynShadowArrayTex_); dynShadowArrayTex_ = 0; }
    if (dynShadowArrayDummyColorTex_) { glDeleteTextures(1, &dynShadowArrayDummyColorTex_); dynShadowArrayDummyColorTex_ = 0; }
    if (dynamicFullMapFbo_) { glDeleteFramebuffers(1, &dynamicFullMapFbo_); dynamicFullMapFbo_ = 0; }
    if (dynamicFullMapTex_) { glDeleteTextures(1, &dynamicFullMapTex_); dynamicFullMapTex_ = 0; }
    if (dynamicFullMapDummyColorTex_) { glDeleteTextures(1, &dynamicFullMapDummyColorTex_); dynamicFullMapDummyColorTex_ = 0; }

    RenderCore::RenderResourceDesc invalid;
    invalid.id = RenderCore::RenderResourceId::ShadowDynamicMap;
    RenderCore::registerOrUpdateRenderResource(invalid);
}

// === Item 1 P1: per-cascade caster pass framebuffer binding ================
// Called once per cascade by the dynamic shadow caster loop (txmmgr.cpp). Binds
// the array FBO to layer i, sets viewport + forward-Z clear, and sets ALL depth/
// blend/cull state EXPLICITLY (do not inherit — chunk-transparency lesson). The
// caster batchers then upload getDynamicLightSpaceMatrix() which, because we set
// csmActiveCascade_ here, resolves to this cascade's matrix.
bool gosPostProcess::beginDynamicShadowCascade(int i)
{
    if (!mc2ShadowCsmEnabled() || !dynShadowArrayFBO_) return false;
    if (i < 0 || i >= csmCount_) return false;

    // SHADOW-CSM-STATE-1: capture the caller's full scene GL state ONCE, on the
    // first cascade of the loop. endDynamicShadowCascadePass() restores it. This
    // mirrors gosRenderer::beginDynamicShadowPass capturing s_dynamicPassEntry
    // (captureShadowGLState, gameos_graphics.cpp:5549) so the forward-Z 4096^2
    // array pass cannot leak FBO/viewport/depth/blend/cull into the reverse-Z
    // scene passes (props/actors/water) that draw after the loop. Without this,
    // endDynamicShadowCascadePass only did glBindFramebuffer(0) -> scene drew into
    // the default backbuffer at 4096^2 with forward-Z depth -> geometry vanished.
    if (!csmStateSaved_) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &csmSavedFBO_);
        glGetIntegerv(GL_VIEWPORT, csmSavedViewport_);
        glGetFloatv(GL_DEPTH_CLEAR_VALUE, &csmSavedClearDepth_);
        csmSavedDepthTest_ = glIsEnabled(GL_DEPTH_TEST);
        glGetIntegerv(GL_DEPTH_FUNC, &csmSavedDepthFunc_);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &csmSavedDepthMask_);
        csmSavedBlend_ = glIsEnabled(GL_BLEND);
        csmSavedCull_  = glIsEnabled(GL_CULL_FACE);
        csmStateSaved_ = true;
    }

    csmActiveCascade_ = i;

    // Per-cascade shadow resolution: the LAST cascade (csmCount_-1) renders into
    // the separate lower-res full-map 2D texture; the near cascades (0..N-2) go
    // to their array layer. getDynamicLightSpaceMatrix() resolves to
    // dynamicCascadeMatrices_[i] either way (no batcher edits needed).
    const bool isFullMap = (i == csmCount_ - 1);
    if (isFullMap) {
        glBindFramebuffer(GL_FRAMEBUFFER, dynamicFullMapFbo_);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glViewport(0, 0, dynamicFullMapSize_, dynamicFullMapSize_);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, dynShadowArrayFBO_);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, dynShadowArrayTex_, 0, i);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, dynShadowArrayDummyColorTex_, 0, i);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glViewport(0, 0, dynShadowMapSize_, dynShadowMapSize_);
    }

    // Explicit state (forward-Z shadow pass; scene uses reverse-Z clearDepth 0).
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glClearDepth(1.0f);
    glClear(GL_DEPTH_BUFFER_BIT);
    glClearDepth(0.0f);   // restore scene reverse-Z default
    return true;
}

void gosPostProcess::endDynamicShadowCascadePass()
{
    csmActiveCascade_ = -1;

    // SHADOW-CSM-STATE-1: restore the full scene GL state captured on the first
    // cascade (mirror of gosRenderer::endDynamicShadowPass ->
    // restoreShadowGLState(s_dynamicPassEntry), gameos_graphics.cpp:5564). Done
    // per-cascade-end and idempotent: each end restores to the same saved scene
    // state, the next begin re-binds the array FBO, and the final end leaves the
    // scene FBO/viewport/reverse-Z depth correct for the following passes. The
    // saved-state flag is cleared so the next frame's loop re-captures fresh.
    if (csmStateSaved_) {
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)csmSavedFBO_);
        glViewport(csmSavedViewport_[0], csmSavedViewport_[1],
                   csmSavedViewport_[2], csmSavedViewport_[3]);
        glClearDepth(csmSavedClearDepth_);
        if (csmSavedDepthTest_) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glDepthFunc((GLenum)csmSavedDepthFunc_);
        glDepthMask(csmSavedDepthMask_);
        if (csmSavedBlend_) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        if (csmSavedCull_)  glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        csmStateSaved_ = false;
    } else {
        // Defensive: no capture (should not happen) -> at least leave a valid FBO.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

void gosPostProcess::buildDynamicLightMatrix(float sunDirX, float sunDirY, float sunDirZ,
                                              const float camFitCornersMC2[8][3],
                                              const float shadowCenterXYZ[3], bool shadowCenterValid)
{
    if (!shadowsEnabled_ || !dynShadowFBO_) return;

    ZoneScopedN("Shadow.DynMatrixBuild");

    float len = sqrtf(sunDirX*sunDirX + sunDirY*sunDirY + sunDirZ*sunDirZ);
    if (len < 0.001f) return;
    float fx = sunDirX/len, fy = sunDirY/len, fz = sunDirZ/len;

    // SHADOW-UNIFIED-PROJECTION-1: fit shadow ortho in LIGHT SPACE.
    //
    // Prior XY-world-bbox approach: MC2 far clip = 61,555 WU, map r~12,184 WU,
    // so far-frustum corners ALWAYS blow past +-r and clamp. maxX was frozen at
    // r every frame; cx trailed halfway between camera and map edge, not where
    // the player looks. Near map edges cx oscillated (corners toggle clamp) ->
    // shadow "reflected off" the map edge on the minimap.
    //
    // Fix: compute the light-space basis first, project corners to light XY,
    // take the AABB in light space. Filter underground corners (MC2 Z < -200)
    // so deep far-plane corners don't blow out coverage. Center is now the
    // frustum centroid in light space -> tracks camera look-at stably.
    // camZ is back-projected from light-space centroid (removes camZ=0 hardcode).
    // BOUNDED_NEAR_CENTER block removed -- was a broken workaround for this bug.

    // Light basis (Gram-Schmidt on sun direction, Z-up world).
    // Identical to the view matrix construction below; computed here first
    // so we can project corners before building the matrix.
    float rx, ry, rz, ux, uy, uz;
    mc2ComputeLightBasis(fx, fy, fz, rx, ry, rz, ux, uy, uz);

    // Project each frustum corner to light space (light-X = dot(world,right),
    // light-Y = dot(world,up)). Filter corners deeper than -200 WU below sea
    // level -- those are underground far-plane artifacts in the 61k WU frustum.
    const float kSceneZFloor = -200.0f;
    float minLX = 1e30f, maxLX = -1e30f;
    float minLY = 1e30f, maxLY = -1e30f;
    int validCorners = 0;
    for (int c = 0; c < 8; ++c) {
        const float wx = camFitCornersMC2[c][0];
        const float wy = camFitCornersMC2[c][1];
        const float wz = camFitCornersMC2[c][2];
        if (wz < kSceneZFloor) continue;
        const float lx = rx*wx + ry*wy + rz*wz;
        const float ly = ux*wx + uy*wy + uz*wz;
        if (lx < minLX) minLX = lx;  if (lx > maxLX) maxLX = lx;
        if (ly < minLY) minLY = ly;  if (ly > maxLY) maxLY = ly;
        ++validCorners;
    }
    // SHADOW-ROBUST-BASIS-1: scarcity fallback. The wz<-200 filter drops
    // underground far-frustum corners; at some camera pitches it eats so many
    // that the surviving subset is a tiny/skewed sliver, which mis-centers the
    // light AABB and slides the shadow region off where the camera looks (the
    // "shadows vanish at angle" symptom). When corners are scarce, rebuild the
    // AABB from ALL 8 corners so the fit can't degenerate to a sliver. Legacy
    // only fell back at validCorners==0. Gated (default ON) via the same knob.
    static const int s_minValidCorners = []() {
        const char* g = getenv("MC2_SHADOW_ROBUST_BASIS");
        if (g && g[0] == '0') return 1;        // legacy: only the ==0 (i.e. <1) path
        return 4;                              // robust: fall back when <4 survive
    }();
    if (validCorners < s_minValidCorners) {
        minLX = 1e30f; maxLX = -1e30f;
        minLY = 1e30f; maxLY = -1e30f;
        for (int c = 0; c < 8; ++c) {
            const float lx = rx*camFitCornersMC2[c][0] + ry*camFitCornersMC2[c][1] + rz*camFitCornersMC2[c][2];
            const float ly = ux*camFitCornersMC2[c][0] + uy*camFitCornersMC2[c][1] + uz*camFitCornersMC2[c][2];
            if (lx < minLX) minLX = lx;  if (lx > maxLX) maxLX = lx;
            if (ly < minLY) minLY = ly;  if (ly > maxLY) maxLY = ly;
        }
    }

    // AABB center in light space -> back-project to world for the shadow origin.
    float cxL = 0.5f * (minLX + maxLX);
    float cyL = 0.5f * (minLY + maxLY);
    const float halfLX = 0.5f * (maxLX - minLX);
    const float halfLY = 0.5f * (maxLY - minLY);

    // SHADOW-FOCUS-CENTER-1 (default ON): center the shadow box on the camera's
    // GROUND FOCUS POINT -- where the center view ray hits the ground plane
    // (z = 0, sea level), not on the frustum-corner AABB centroid.
    //
    // PROBLEM with old approach (nearC + vd*focusDist): the near frustum
    // centroid sits at camera height (elevated, not on the ground). Stepping
    // 1500 WU along the view ray from there still leaves the box centered
    // somewhere in mid-air behind/below the visible terrain ahead. Half the
    // shadow-map coverage is wasted behind the camera.
    //
    // NEW approach -- per-corner ground-plane intercept:
    //   For each of the 4 near/far corner pairs, find where the line from
    //   nearCorner[c] to farCorner[c] crosses z = 0:
    //     t = -nz / (fz - nz)   (clamp t to [0,1] if line doesn't cross)
    //     groundPt = near + t * (far - near)
    //   Average the 4 ground intercept XY values -> the TRUE ground focus.
    //   Fall back: if all corner pairs have |fz - nz| < eps (nearly horizontal
    //   ray, or camera pitched up), drop the near-centroid z to 0 instead.
    //
    // The formula sets the shadow box center directly over where the camera
    // is looking on the ground, so coverage lands on the visible terrain ahead.
    //
    // Kill-switch: MC2_SHADOW_FOCUS_CENTER=0 reverts to the AABB centroid.
    // (MC2_SHADOW_FOCUS_DIST is retained for diagnostic logging but no longer
    //  drives the centering.)
    static const bool s_focusCenter = []() {
        const char* v = getenv("MC2_SHADOW_FOCUS_CENTER");
        return !(v && v[0] == '0');  // default ON; opt out with =0
    }();
    // CSM-REDESIGN: MC2_SHADOW_CSM_FULLMAP_LAST and MC2_SHADOW_CSM_NEAR_MAX are
    // RETIRED/IGNORED. The last cascade is always full-map+map-centered and the
    // near/mid cascades use fixed radii (mc2ShadowCsmR0/R1), so the old per-frame
    // catch-all flag and near-extent cap no longer apply. Unread envs are
    // harmless (old launch scripts that still set them just have no effect).
    bool focusApplied = false;
    float focusWorld[3] = {0.0f, 0.0f, 0.0f};
    float focusDistUsed = 0.0f;
    float nearSpread = 0.0f, farSpread = 0.0f;
    if (s_focusCenter) {
        // Robust near/far corner identification (same detector as before).
        // Corners 0-3 and 4-7 are the two frustum planes; detect near by smaller
        // spread (perspective: near corners are closer together than far corners).
        float cA[3] = {0,0,0}, cB[3] = {0,0,0};
        for (int c = 0; c < 4; ++c) {
            cA[0] += camFitCornersMC2[c][0]; cA[1] += camFitCornersMC2[c][1]; cA[2] += camFitCornersMC2[c][2];
        }
        for (int c = 4; c < 8; ++c) {
            cB[0] += camFitCornersMC2[c][0]; cB[1] += camFitCornersMC2[c][1]; cB[2] += camFitCornersMC2[c][2];
        }
        for (int k = 0; k < 3; ++k) { cA[k] *= 0.25f; cB[k] *= 0.25f; }
        float spreadA = 0.0f, spreadB = 0.0f;
        for (int c = 0; c < 4; ++c) {
            float dx = camFitCornersMC2[c][0]-cA[0], dy = camFitCornersMC2[c][1]-cA[1], dz = camFitCornersMC2[c][2]-cA[2];
            float d = sqrtf(dx*dx+dy*dy+dz*dz);
            if (d > spreadA) spreadA = d;
        }
        for (int c = 4; c < 8; ++c) {
            float dx = camFitCornersMC2[c][0]-cB[0], dy = camFitCornersMC2[c][1]-cB[1], dz = camFitCornersMC2[c][2]-cB[2];
            float d = sqrtf(dx*dx+dy*dy+dz*dz);
            if (d > spreadB) spreadB = d;
        }
        // nearC/farC = centroids; near has smaller spread.
        const float* nearC; const float* farC;
        const int nearBase2 = (spreadA <= spreadB) ? 0 : 4;
        const int farBase2  = (spreadA <= spreadB) ? 4 : 0;
        if (spreadA <= spreadB) { nearC = cA; farC = cB; nearSpread = spreadA; farSpread = spreadB; }
        else                    { nearC = cB; farC = cA; nearSpread = spreadB; farSpread = spreadA; }

        // FOCUS SOURCE. Preferred: the screen-center ground point passed in by
        // the caller (eye->screenToGroundPlaneApprox at resX/2,resY/2) = the
        // TRUE look-at the cascade should radiate from. Same MC2 world space as
        // camFitCornersMC2 (both invert worldToClipGL). No forward-cap needed:
        // it is already the real look-at, not a frustum-corner reconstruction.
        // FALLBACK (shadowCenterValid == false): the central view-axis z=0
        // intercept reconstructed from frustum corners, with the R1 forward cap.
        float t = 0.0f, d = 0.0f, cap = 0.0f;   // diag only
        const char* focusSrc;
        if (shadowCenterValid) {
            focusSrc = "camtarget";
            // Screen-center TERRAIN point: x=east, y=north, z=terrain elevation.
            // Carry Z through so the cascade center sits at the real look-at
            // height (not sea level). The light-space projection below now
            // includes the rz/uz*z term, and the back-projection reconstructs
            // the world center at this elevation (basis-consistent).
            focusWorld[0] = shadowCenterXYZ[0];
            focusWorld[1] = shadowCenterXYZ[1];
            focusWorld[2] = shadowCenterXYZ[2];
            focusApplied  = true;
        } else {
            focusSrc = "axis";
            // CENTRAL VIEW-AXIS ground focus (replaces per-corner z=0 averaging).
            // The per-corner average skewed forward when far corners rose above the
            // horizon (per-corner t clamped to 1.0) and fell back to under-camera
            // when no corner hit the ground. Instead intersect the CENTRAL view axis
            // -- the ray through the frustum center (near-centroid -> far-centroid),
            // i.e. the true screen-center look-at -- with z=0. This does not skew.
            // nearC/farC are the 4-near and 4-far corner centroids (xyz) from above.
            const float* nearCenter = nearC;
            const float* farCenter  = farC;
            float dzc = farCenter[2] - nearCenter[2];
            if (fabsf(dzc) > 1e-3f) { t = -nearCenter[2] / dzc; }
            else                    { t = 1.0f; }   // near-horizontal axis -> far center
            if (t < 0.0f) t = 0.0f;                 // behind camera -> use near center
            // allow t>1 (forward distance capped below via cascade-1 radius)
            float fx_f = nearCenter[0] + t * (farCenter[0] - nearCenter[0]);
            float fy_f = nearCenter[1] + t * (farCenter[1] - nearCenter[1]);
            // Carry the intercept Z through (for a z=0 plane solve this is ~0,
            // but keep the computed value so 4c/4d stay basis-consistent).
            float fz_f = nearCenter[2] + t * (farCenter[2] - nearCenter[2]);

            // ROBUST FORWARD CAP: never let the look-at sit beyond where cascade 1
            // reaches. camGround = near-plane center XY (~ under/just-ahead of cam).
            const float camGroundX = nearCenter[0];
            const float camGroundY = nearCenter[1];
            float ox = fx_f - camGroundX, oy = fy_f - camGroundY;
            d   = sqrtf(ox*ox + oy*oy);
            cap = mc2ShadowCsmR1();
            if (d > cap && d > 1e-3f) {
                float s = cap / d;
                fx_f = camGroundX + ox * s;
                fy_f = camGroundY + oy * s;
            }

            focusWorld[0] = fx_f;
            focusWorld[1] = fy_f;
            focusWorld[2] = fz_f;   // intercept Z (~0 for the z=0 plane solve)
            focusApplied  = true;   // central-axis always yields a usable focus
        }
        // Override light-space center with the focus point projection. INCLUDE
        // the Z term: focusWorld[2] is the terrain elevation (or z=0 plane in
        // the axis fallback), so the projection must carry rz/uz*z to land the
        // center at the real look-at height. Matches the full-3D frustum-corner
        // projection (lx = rx*wx+ry*wy+rz*wz) above.
        cxL = rx*focusWorld[0] + ry*focusWorld[1] + rz*focusWorld[2];
        cyL = ux*focusWorld[0] + uy*focusWorld[1] + uz*focusWorld[2];
        // focusDistUsed: XY distance from near-centroid to focus (diagnostic).
        float fdx = focusWorld[0]-nearC[0], fdy = focusWorld[1]-nearC[1];
        focusDistUsed = sqrtf(fdx*fdx + fdy*fdy);

        // [CSM focus] diag: gate on existing CSM frustum-diag env, else a
        // dedicated opt-in env. Cheap: only formats/prints when env is set.
        static const bool s_focusDiag =
            (getenv("MC2_SHADOW_FRUSTUM_DIAG") != nullptr) ||
            (getenv("MC2_SHADOW_FOCUS_DIAG")   != nullptr);
        if (s_focusDiag) {
            printf("[CSM focus] src=%s fx=%.1f fy=%.1f fz=%.1f t=%.3f d=%.1f cap=%.1f\n",
                   focusSrc, focusWorld[0], focusWorld[1], focusWorld[2], t, d, cap);
        }
        (void)nearBase2; (void)farBase2;  // near/far identified via nearC/farC
    }
    float fitRadius = (halfLX > halfLY ? halfLX : halfLY);
    if (fitRadius < 64.0f) fitRadius = 64.0f;
    float r = mapHalfExtent_ * sqrtf(2.0f) * 1.05f;   // full-map safety cap
    if (fitRadius > r) fitRadius = r;
    const float origFitRadius = fitRadius;

    // SHADOW-BOUNDED-NEAR-FIT-1 (gate, default OFF): cap fit radius for higher
    // near-shadow texel density. Center is now correct by construction (light-
    // space AABB); no separate center-override needed.
    {
        static const bool s_boundedNear = []() {
            const char* v = getenv("MC2_SHADOW_BOUNDED_NEAR_FIT");
            return !(v && v[0] == '0');  // default ON; opt out with =0
        }();
        if (s_boundedNear) {
            static const float s_boundedRadiusRaw = []() {
                const char* v = getenv("MC2_SHADOW_BOUNDED_NEAR_RADIUS");
                float rad = (v ? (float)atof(v) : 2500.0f);
                if (rad <= 0.0f) rad = 2500.0f;
                return rad;
            }();
            float boundedRadius = s_boundedRadiusRaw;
            if (boundedRadius < 512.0f) boundedRadius = 512.0f;
            if (boundedRadius > r)      boundedRadius = r;
            if (fitRadius > boundedRadius) fitRadius = boundedRadius;
        }
    }

    // Anti-shimmer: snap fit radius to power-of-2, snap center to texel grid
    // in light space (prevents shadow swimming on smooth camera motion).
    float xyRadius = 64.0f;
    while (xyRadius < fitRadius) xyRadius *= 2.0f;
    if (xyRadius > r) xyRadius = r;
    float worldUnitsPerTexel = (2.0f * xyRadius) / (float)dynShadowMapSize_;
    cxL = floorf(cxL / worldUnitsPerTexel) * worldUnitsPerTexel;
    cyL = floorf(cyL / worldUnitsPerTexel) * worldUnitsPerTexel;
    // Back-project snapped light-space center to world (replaces camZ=0 hardcode).
    float camX = cxL * rx + cyL * ux;
    float camY = cxL * ry + cyL * uy;
    float camZ = cxL * rz + cyL * uz;

    float depthDist = 5000.0f;

    // SHADOW-FRUSTUM-AUDIT-1: MC2_SHADOW_FRUSTUM_DIAG=1 logs per-frame coverage.
    {
        static const bool s_frustDiag = (getenv("MC2_SHADOW_FRUSTUM_DIAG") != nullptr);
        static int s_frustN = 0;
        if (s_frustDiag) {
            ++s_frustN;
            if (s_frustN <= 3 || (s_frustN % 300) == 0) {
                fprintf(stderr,
                    "[SHADOW_FRUSTUM_DIAG] frame=%d sunDir=(%.3f,%.3f,%.3f) "
                    "lightAABB=[%.0f..%.0f,%.0f..%.0f] worldCenter=(%.0f,%.0f,%.0f) "
                    "fitRadius=%.0f(orig=%.0f) xyRadius=%.0f mapClampR=%.0f "
                    "texelWU=%.3f orthoWH=%.0fx%.0f validCorners=%d mapSize=%d\n",
                    s_frustN, fx, fy, fz,
                    minLX, maxLX, minLY, maxLY, camX, camY, camZ,
                    fitRadius, origFitRadius, xyRadius, r,
                    worldUnitsPerTexel, 2.0f * xyRadius, 2.0f * xyRadius,
                    validCorners, dynShadowMapSize_);
                if (focusApplied) {
                    fprintf(stderr,
                        "[SHADOW_FRUSTUM_DIAG]   focusCenter=1 focusWorld=(%.0f,%.0f,%.0f) "
                        "focusXYDist=%.0f nearSpread=%.0f farSpread=%.0f\n",
                        focusWorld[0], focusWorld[1], focusWorld[2],
                        focusDistUsed, nearSpread, farSpread);
                }
                fflush(stderr);
            }
        }
    }

    float lightPosX = camX - fx * depthDist;
    float lightPosY = camY - fy * depthDist;
    float lightPosZ = camZ - fz * depthDist;

    float view[16] = {
         rx,  ux, -fx, 0,
         ry,  uy, -fy, 0,
         rz,  uz, -fz, 0,
        -(rx*lightPosX + ry*lightPosY + rz*lightPosZ),
        -(ux*lightPosX + uy*lightPosY + uz*lightPosZ),
         (fx*lightPosX + fy*lightPosY + fz*lightPosZ),
        1
    };

    // Z-row emits clip-z in [0,1] (near->0, far->1) for glClipControl
    // (GL_ZERO_TO_ONE), lockstep with buildStaticLightMatrix above and the
    // .xy-only sampler remap in shadow.hglsl / shadow_screen.frag.
    float nearP = 1.0f, farP = 2.0f * depthDist;
    // Stage 3: single-source the CSM ortho depth span for the shaders' texel bias.
    csmDepthSpan_ = farP - nearP;
    float ortho[16] = {
        1.0f/xyRadius, 0, 0, 0,
        0, 1.0f/xyRadius, 0, 0,
        0, 0, -1.0f/(farP - nearP), 0,
        0, 0, -nearP/(farP - nearP), 1
    };

    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0;
            for (int k = 0; k < 4; k++)
                sum += ortho[k * 4 + row] * view[col * 4 + k];
            dynamicLightSpaceMatrix_[col * 4 + row] = sum;
        }
    }

    {
        RenderCore::EngineView dv;
        dv.id        = RenderCore::kShadowDynamicViewId;
        dv.kind      = RenderCore::ViewKind::ShadowDynamic;
        dv.debugName = "ShadowDynamic";
        dv.viewport[2] = dynShadowMapSize_;
        dv.viewport[3] = dynShadowMapSize_;
        memcpy(dv.viewUniforms.worldToClipGL, dynamicLightSpaceMatrix_,
               sizeof(dv.viewUniforms.worldToClipGL));
        RenderCore::registerOrUpdateView(dv);
    }

    // === Item 1 P2: per-cascade matrix array ===============================
    // ONE basis (rx..uz), ONE sun (fx,fy,fz) — reused for every cascade. The
    // z-row of each cascade ortho is IDENTICAL to the legacy single matrix
    // (same nearP/farP/depthDist, same -1/(farP-nearP) z scale). Cascades
    // differ ONLY by their xy center + xy extent (and the texel snap). Each
    // off-center cascade gets a TRANSLATE via its own lightPos back-projected
    // from the cascade's snapped light-space center (not just an xy scale).
    // All math stays in raw MC2 Z-up light space (no GL-NDC/D3D stage).
    //
    // Split: partition the camera frustum near->splitFar (~8000 WU light depth
    // along the view axis) into csmCount_ slices, blended log/uniform by lambda.
    // Beyond ~8000 the world-fixed STATIC map covers (min-combine in shaders).
    if (mc2ShadowCsmEnabled() && dynShadowArrayTex_) {
        const int N = csmCount_;

        // CSM-REDESIGN: FIXED concentric cascade scheme. Near/mid cascades use
        // FIXED world-unit radii (mc2ShadowCsmR0/R1) centered on the ground
        // look-at focus -> no per-frame frustum-refit -> no popping. The LAST
        // cascade is ALWAYS full-map and MAP-CENTERED so props anywhere on the
        // map cast shadows even with the camera at a map edge. The old per-frame
        // sub-frustum AABB fit + log/uniform split + s_csmFullMapLast +
        // s_csmNearMax cap are retired (superseded by this fixed scheme).
        //
        // Shared ground look-at focus (reuse focusWorld = z=0 intercept computed
        // by the focus block above; fall back to the snapped camera world XY).
        const float gfx = focusApplied ? focusWorld[0] : camX;
        const float gfy = focusApplied ? focusWorld[1] : camY;
        // gfz: focus terrain elevation (camZ when no focus). camZ is the snapped
        // light-space center back-projected to world, already in scope above.
        const float gfz = focusApplied ? focusWorld[2] : camZ;

        for (int ci = 0; ci < N; ++ci) {
            float cCx, cCy, cHalf;
            if (ci == N - 1) {
                // Full-map, MAP-CENTERED. MC2 maps are center-origin -> world
                // (0,0) projects to light-space (0,0). Always stable, covers
                // edge props regardless of camera position.
                cCx = 0.0f; cCy = 0.0f; cHalf = r;
            } else {
                const float fixedRadius = (ci == 0) ? mc2ShadowCsmR0() : mc2ShadowCsmR1();
                // Project the terrain look-at point into light XY. INCLUDE the
                // Z term (rz/uz*gfz): gfz is the terrain elevation, so the
                // center lands at the real look-at height. SAME full-3D
                // projection as the focus/cView path and the frustum-corner fit.
                cCx = rx * gfx + ry * gfy + rz * gfz;
                cCy = ux * gfx + uy * gfy + uz * gfz;
                cHalf = fixedRadius;
            }
            if (cHalf < 64.0f) cHalf = 64.0f;
            if (cHalf > r)     cHalf = r;
            // Snap radius to pow2 + center to texel grid (anti-shimmer), same as legacy.
            float cRad = 64.0f;
            while (cRad < cHalf) cRad *= 2.0f;
            if (cRad > r) cRad = r;
            // Per-cascade shadow resolution: the LAST (full-map) cascade lives in
            // the separate dynamicFullMapSize_ texture, so its texel-snap grid +
            // world-texel footprint MUST use that resolution, not the array's.
            const bool isFullMapCascade = (ci == N - 1);
            const int  cascadeMapSize = isFullMapCascade ? dynamicFullMapSize_
                                                         : dynShadowMapSize_;
            float cTexel = (2.0f * cRad) / (float)cascadeMapSize;
            dynamicCascadeTexelWorld_[ci] = cTexel;
            if (isFullMapCascade) dynamicFullMapTexelWorld_ = cTexel;
            cCx = floorf(cCx / cTexel) * cTexel;
            cCy = floorf(cCy / cTexel) * cTexel;
            // Back-project snapped light-space center to world (the cascade origin).
            float wCx = cCx*rx + cCy*ux;
            float wCy = cCx*ry + cCy*uy;
            float wCz = cCx*rz + cCy*uz;
            // Per-cascade light position along the sun axis (shared depthDist ->
            // shared z-row). TRANSLATE column reflects this center.
            float lpX = wCx - fx*depthDist;
            float lpY = wCy - fy*depthDist;
            float lpZ = wCz - fz*depthDist;
            float cView[16] = {
                 rx,  ux, -fx, 0,
                 ry,  uy, -fy, 0,
                 rz,  uz, -fz, 0,
                -(rx*lpX + ry*lpY + rz*lpZ),
                -(ux*lpX + uy*lpY + uz*lpZ),
                 (fx*lpX + fy*lpY + fz*lpZ),
                1
            };
            // SAME z-row as legacy (nearP/farP/depthDist identical).
            float cOrtho[16] = {
                1.0f/cRad, 0, 0, 0,
                0, 1.0f/cRad, 0, 0,
                0, 0, -1.0f/(farP - nearP), 0,
                0, 0, -nearP/(farP - nearP), 1
            };
            float* dst = &dynamicCascadeMatrices_[ci * 16];
            for (int col = 0; col < 4; col++)
                for (int row = 0; row < 4; row++) {
                    float sum = 0;
                    for (int k = 0; k < 4; k++)
                        sum += cOrtho[k*4+row] * cView[col*4+k];
                    dst[col*4+row] = sum;
                }
        }

        // Per-cascade ZRANGE probe (center + corners must land in [0,1]).
        if (getenv("MC2_DEBUG_SHADOW_ZRANGE") != nullptr) {
            static int s_csmN = 0;
            if (++s_csmN <= 1) {
                for (int ci = 0; ci < N; ++ci) {
                    const float* M = &dynamicCascadeMatrices_[ci*16];
                    // sample the cascade's own center (col 3 back-projected approx via camX/Y/Z)
                    const float px=camX, py=camY, pz=camZ;
                    float cz = M[0*4+2]*px+M[1*4+2]*py+M[2*4+2]*pz+M[3*4+2];
                    float cw = M[0*4+3]*px+M[1*4+3]*py+M[2*4+3]*pz+M[3*4+3];
                    float ndc = (cw!=0.0f)?cz/cw:0.0f;
                    fprintf(stderr, "[SHADOWZRANGE v1] event=csm cascade=%d ndcZ=%.5f inRange=%d\n",
                            ci, ndc, (ndc>=0.0f&&ndc<=1.0f)?1:0);
                }
            }
        }
    }

    // [SHADOWZRANGE v1] VPL-#10: dynamic-path [0,1] verification. Rebuilds
    // per frame -> one-shot via static counter. Transforms the snapped camera
    // center + offsets through dynamicLightSpaceMatrix_; clip.z/clip.w MUST be
    // in [0,1] (lockstep with the static probe + the .xy-only sampler remap).
    if (getenv("MC2_DEBUG_SHADOW_ZRANGE") != nullptr) {
        static int s_dynN = 0;
        if (++s_dynN <= 1) {
            const float* M = dynamicLightSpaceMatrix_;
            const float pts[3][3] = {
                { camX, camY, camZ },
                { camX + 0.95f*xyRadius, camY + 0.95f*xyRadius, camZ },
                { camX - 0.95f*xyRadius, camY - 0.95f*xyRadius, camZ }
            };
            for (int i = 0; i < 3; i++) {
                float px = pts[i][0], py = pts[i][1], pz = pts[i][2];
                float cz = M[0*4+2]*px + M[1*4+2]*py + M[2*4+2]*pz + M[3*4+2];
                float cw = M[0*4+3]*px + M[1*4+3]*py + M[2*4+3]*pz + M[3*4+3];
                float ndc = (cw != 0.0f) ? cz / cw : 0.0f;
                fprintf(stderr,
                    "[SHADOWZRANGE v1] event=dynamic pt=%d world=(%.0f,%.0f,%.0f) "
                    "clipZ=%.5f clipW=%.5f ndcZ=%.5f inRange=%d\n",
                    i, px, py, pz, cz, cw, ndc, (ndc >= 0.0f && ndc <= 1.0f) ? 1 : 0);
            }
        }
    }
}

// ============================================================================
// FRAME-GRAPH-EXECUTOR-ISLAND-1
//
// Executor-owned PostProcess island: validate -> call pp->endScene() unchanged
// -> validate. Gate MC2_FRAMEGRAPH_EXECUTOR (default-OFF) -> byte-identical.
//
// Counters are standalone process-statics; NOT in gos_FrameCtx (read-only
// mirror — injecting executor state would break its mirror invariant).
// Exposed via extern "C" mc2_framegraph_executor_owned_passes() /
// mc2_framegraph_executor_validation_failures().
// ============================================================================

// --- gosPostProcess executor accessor implementations -----------------------

bool gosPostProcess::executorCompositeProgramValid() const
{
    return compositeProg_ && compositeProg_->is_valid();
}

bool gosPostProcess::executorSceneColorTexValid() const
{
    return sceneColorTex_ != 0;
}

// FRAME-GRAPH-EXECUTOR-ISLAND-2: sub-stage accessor implementations.
// Each WillRun() mirrors the exact early-return gates of the matching run*()
// function so the executor only counts/validates when the body actually draws.

bool gosPostProcess::executorEdgeFogWillRun() const
{
    return edgeFogEnabled_ && edgeFogProg_ && edgeFogProg_->is_valid()
        && mapHalfExtent_ > 0.0f && sceneHasTerrain_;
}

bool gosPostProcess::executorFogOobWillRun() const
{
    return fogOobEnabled_ && fogOobProg_ && fogOobProg_->is_valid()
        && sceneHasTerrain_;
}

bool gosPostProcess::executorSceneDepthTexValid() const
{
    return sceneDepthTex_ != 0;
}

// FRAME-GRAPH-EXECUTOR-ISLAND-3: sub-stage accessor implementations.
// WillRun() mirrors exact early-return gates of runShoreline() / runCloudShadow().
// EXECUTOR-ISLAND-SCREENSHADOW-1: executorScreenShadowWillRun() added below (leak fixed).

bool gosPostProcess::executorShorelineWillRun() const
{
    // TERRAIN-SHORELINE-MASK-1: mirrors runShoreline()'s suppression term.
    return shorelineEnabled_ && shorelineProg_ && shorelineProg_->is_valid()
        && sceneHasTerrain_ && !shorelineSuppressedByTerrainMask_;
}

bool gosPostProcess::executorCloudShadowWillRun() const
{
    return enableCloudShadow_ && cloudProg_ && cloudProg_->is_valid()
        && sceneHasTerrain_;
}

// EXECUTOR-ISLAND-SCREENSHADOW-1: mirrors runScreenShadow() early-return gates exactly
// (lines 2033-2036). Now owned: SCREENSHADOW-TEX-RESTORE-1 (a0b4189b) fixed unit-3 leak.
bool gosPostProcess::executorScreenShadowWillRun() const
{
    return screenShadowEnabled_ && sceneHasTerrain_
        && screenShadowProg_ && screenShadowProg_->is_valid()
        && shadowsEnabled_;
}

// --- Gate helper (read once, static lambda) ---------------------------------

static bool executorEnabled()
{
    static const bool s_on = []() {
        const char* v = ::getenv("MC2_FRAMEGRAPH_EXECUTOR");
        return v && v[0] == '1';
    }();
    return s_on;
}

// --- Standalone counters (process lifetime; never reset) --------------------

static unsigned long g_executorOwnedPasses       = 0;
static unsigned long g_executorValidationFailures = 0;
// FRAMEGRAPH-APPLY-STATE-ISLAND-1: incremented each time executorApplyEdgeFogState() fires.
// Exposed via mc2_framegraph_executor_apply_state_passes() so the advisor can assert > 0.
static unsigned long g_applyStatePasses          = 0;

// PER-PASS-APPLY-COUNTERS-1: per-apply-path counters keyed by ApplyPassId, so
// each apply site is independently observable. The legacy aggregate
// g_applyStatePasses above is no longer incremented; the reported aggregate is
// DERIVED as the sum over this array (no drift). All apply sites — the 5
// PostProcess sub-stage applies here plus the 3 top-level applies in other TUs
// (routed through mc2_framegraph_executor_bump_apply_state(id)) — bump exactly
// one slot.
static unsigned long g_applyStateByPass[(int)RenderCore::framegraph::ApplyPassId::Count] = {0};
static void applyStateBump(RenderCore::framegraph::ApplyPassId id) {
    ++g_applyStateByPass[(int)id];
}

// Max log lines for validation failures — prevent log flooding in pathological cases.
static constexpr unsigned kMaxExecutorFailureLog = 32u;

// --- Pre-call validation wrapper -------------------------------------------
//
// Checks the IslandContract preconditions for the PostProcess island.
// NON-FATAL: on any failure increments g_executorValidationFailures + logs.
// Returns true if all checks passed (caller proceeds normally either way;
// the pp->endScene() call is ALWAYS made by the caller).

static void executorOwnBegin(gosPostProcess* pp)
{
    if (!executorEnabled()) return;

    using namespace RenderCore::framegraph;
    const IslandContract* c = findIslandContract(ExecutorIslandId::PostProcess);
    if (!c) return;   // not an owned island (defensive; kExecutorIslands has the row)

    bool ok = true;

    // 1. Program must be compiled and valid.
    if (c->requiresProgramValid && !pp->executorCompositeProgramValid()) {
        if (g_executorValidationFailures < kMaxExecutorFailureLog)
            fprintf(stderr,
                "[EXECUTOR v1] FAIL island=PostProcess check=compositeProg_valid "
                "owned=%lu failures=%lu\n",
                g_executorOwnedPasses, g_executorValidationFailures + 1);
        ++g_executorValidationFailures;
        ok = false;
    }

    // 2. Scene color texture must be populated.
    if (c->requiresSceneColorTex && !pp->executorSceneColorTexValid()) {
        if (g_executorValidationFailures < kMaxExecutorFailureLog)
            fprintf(stderr,
                "[EXECUTOR v1] FAIL island=PostProcess check=sceneColorTex_valid "
                "owned=%lu failures=%lu\n",
                g_executorOwnedPasses, g_executorValidationFailures + 1);
        ++g_executorValidationFailures;
        ok = false;
    }

    // 3. Terrain latch: WARN (non-fatal) if absent — sub-stages bail gracefully
    //    in menus / frontend where terrain never drew.
    if (c->warnIfNoTerrainLatch && !pp->executorSceneHasTerrain()) {
        // One-shot warn to avoid per-frame log spam on menu screens.
        static unsigned s_warnCount = 0;
        if (s_warnCount < 4) {
            fprintf(stderr,
                "[EXECUTOR v1] WARN island=PostProcess sceneHasTerrain_=false "
                "(menu/frontend — sub-stages bail gracefully)\n");
            ++s_warnCount;
        }
        // NOT a failure — sub-stages handle absent terrain.
    }

    (void)ok;  // all failures are non-fatal; pp->endScene() always runs
}

// --- Post-call validation wrapper ------------------------------------------
//
// Checks the IslandContract postconditions for the PostProcess island.
// On success -> increments g_executorOwnedPasses (this island owned+validated).
// On failure -> increments g_executorValidationFailures + logs.
//
// NOTE: calls glGetError() which clears the GL error flag. Acceptable here:
// (a) we are post-endScene which already calls drainGLErrors("post_process"),
// (b) this code path is gated MC2_FRAMEGRAPH_EXECUTOR (default-OFF), so there
// is no production-run side effect.

static void executorOwnEnd(gosPostProcess* /*pp*/)
{
    if (!executorEnabled()) return;

    using namespace RenderCore::framegraph;
    const IslandContract* c = findIslandContract(ExecutorIslandId::PostProcess);
    if (!c) return;

    bool postOk = true;

    // 1. Default FBO must be bound after composite blit.
    if (c->postRequiresDefaultFbo) {
        GLint bound = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
        if (bound != 0) {
            if (g_executorValidationFailures < kMaxExecutorFailureLog)
                fprintf(stderr,
                    "[EXECUTOR v1] FAIL island=PostProcess postcond=defaultFbo "
                    "bound=%d (expected 0) owned=%lu failures=%lu\n",
                    (int)bound, g_executorOwnedPasses, g_executorValidationFailures + 1);
            ++g_executorValidationFailures;
            postOk = false;
        }
    }

    // 2. No GL errors after the composite blit.
    {
        const GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            if (g_executorValidationFailures < kMaxExecutorFailureLog)
                fprintf(stderr,
                    "[EXECUTOR v1] FAIL island=PostProcess postcond=glNoError "
                    "err=0x%X owned=%lu failures=%lu\n",
                    (unsigned)err, g_executorOwnedPasses, g_executorValidationFailures + 1);
            ++g_executorValidationFailures;
            postOk = false;
        }
    }

    if (postOk)
        ++g_executorOwnedPasses;
}

// --- FRAMEGRAPH-APPLY-STATE-ISLAND-1: EdgeFog pre-apply -----------------------
//
// Pre-applies the 4 GL state calls that runEdgeFog() makes at entry, in the
// SAME ORDER as the body (glBindFramebuffer → setSceneDrawBuffers → glViewport
// → applyPipeline). Body's own identical calls are idempotent re-sets this slice.
//
// Only called from executorOwnBeginSub(EdgeFog) when MC2_FRAMEGRAPH_EXECUTOR is
// ON and executorEdgeFogWillRun(). Increments g_applyStatePasses on each call.

void gosPostProcess::executorApplyEdgeFogState()
{
    // Exactly matches runEdgeFog() lines 2293-2306 entry sequence.
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::PostProcessEdgeFog),
        "PostProcessEdgeFog");
    applyStateBump(RenderCore::framegraph::ApplyPassId::PostProcessEdgeFog);
    // APPLY-STATE-REDUNDANT-BODY-REMOVE-1: signal to runEdgeFog() that the executor
    // already applied these 4 setup calls so the body should skip its own copies.
    edgeFogStateAppliedByExecutor_ = true;
}

// FRAMEGRAPH-APPLY-STATE-ISLAND-2: pre-apply declared GL state for FogOob.
// Exactly matches runFogOob() entry (lines 2349-2362): FBO bind + SingleColor + viewport + applyPipeline.
// No tex binds, no uniforms — those remain body-owned. Idempotent (body re-sets same state).
void gosPostProcess::executorApplyFogOobState()
{
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::PostProcessFogOob),
        "PostProcessFogOob");
    applyStateBump(RenderCore::framegraph::ApplyPassId::PostProcessFogOob);
    // APPLY-STATE-REDUNDANT-BODY-REMOVE-2: signal runFogOob() to skip its own copies.
    fogOobStateAppliedByExecutor_ = true;
}

// FRAMEGRAPH-APPLY-STATE-ISLAND-2: pre-apply declared GL state for Shoreline.
// Exactly matches runShoreline() entry (lines 2240-2252): FBO bind + SingleColor + viewport + applyPipeline.
// No tex binds (sceneDepthTex_/sceneNormalTex_), no uniforms — body-owned. Idempotent.
void gosPostProcess::executorApplyShorelineState()
{
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::PostProcessShoreline),
        "PostProcessShoreline");
    applyStateBump(RenderCore::framegraph::ApplyPassId::PostProcessShoreline);
    // APPLY-STATE-REDUNDANT-BODY-REMOVE-2: signal runShoreline() to skip its own copies.
    shorelineStateAppliedByExecutor_ = true;
}

// FRAMEGRAPH-APPLY-STATE-ISLAND-2: pre-apply declared GL state for CloudShadow.
// Exactly matches runCloudShadow() entry (lines 2186-2196): FBO bind + SingleColor + viewport + applyPipeline.
// No tex binds (sceneDepthTex_), no uniforms — body-owned. Idempotent.
void gosPostProcess::executorApplyCloudShadowState()
{
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::PostProcessCloudShadow),
        "PostProcessCloudShadow");
    applyStateBump(RenderCore::framegraph::ApplyPassId::PostProcessCloudShadow);
    // APPLY-STATE-REDUNDANT-BODY-REMOVE-2: signal runCloudShadow() to skip its own copies.
    cloudShadowStateAppliedByExecutor_ = true;
}

// APPLY-STATE-SCREENSHADOW-1: pre-apply declared GL state for ScreenShadow.
// Exactly matches runScreenShadow() entry (lines 2039-2052): FBO bind + SingleColor + viewport + applyPipeline.
// No tex binds, no uniforms, and NOT the screenShadowDebug_ glDisable(GL_BLEND) — those remain body-owned.
// Idempotent (body makes identical calls at entry when the flag is false).
void gosPostProcess::executorApplyScreenShadowState()
{
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::PostProcessScreenShadow),
        "PostProcessScreenShadow");
    applyStateBump(RenderCore::framegraph::ApplyPassId::PostProcessScreenShadow);
    // APPLY-STATE-REDUNDANT-BODY-REMOVE-2 pattern: signal runScreenShadow() to skip its own copies.
    screenShadowStateAppliedByExecutor_ = true;
}

// --- FRAME-GRAPH-EXECUTOR-ISLAND-2: sub-stage wrappers (EdgeFog + FogOob) ----
//
// Each pair gates on executorEnabled(). Begin: if WillRun()==false the sub-pass
// is gated off this frame -> return WITHOUT counting (legitimately not owned).
// End: validate postconditions declared in the IslandContract row; on success
// increment g_executorOwnedPasses (aggregate — sub-stages contribute).

static void executorOwnBeginSub(gosPostProcess* pp, RenderCore::framegraph::ExecutorIslandId islandId)
{
    if (!executorEnabled()) return;

    using namespace RenderCore::framegraph;
    const IslandContract* c = findIslandContract(islandId);
    if (!c) return;

    // Gate: if the sub-pass won't draw this frame, don't count/validate it.
    bool willRun = false;
    if (islandId == ExecutorIslandId::EdgeFog)       willRun = pp->executorEdgeFogWillRun();
    if (islandId == ExecutorIslandId::FogOob)        willRun = pp->executorFogOobWillRun();
    if (islandId == ExecutorIslandId::Shoreline)     willRun = pp->executorShorelineWillRun();
    if (islandId == ExecutorIslandId::CloudShadow)   willRun = pp->executorCloudShadowWillRun();
    if (islandId == ExecutorIslandId::ScreenShadow)  willRun = pp->executorScreenShadowWillRun();
    if (!willRun) return;

    // Island name for diagnostics.
    const char* name = "Unknown";
    if (islandId == ExecutorIslandId::EdgeFog)       name = "EdgeFog";
    if (islandId == ExecutorIslandId::FogOob)        name = "FogOob";
    if (islandId == ExecutorIslandId::Shoreline)     name = "Shoreline";
    if (islandId == ExecutorIslandId::CloudShadow)   name = "CloudShadow";
    if (islandId == ExecutorIslandId::ScreenShadow)  name = "ScreenShadow";

    // Pre-call validation (non-fatal).
    if (c->requiresProgramValid) {
        // Program validity is encoded in WillRun() — already proven above.
        // Defensive: no separate accessor needed (WillRun mirrors the gate).
    }
    if (c->requiresSceneDepthTex && !pp->executorSceneDepthTexValid()) {
        if (g_executorValidationFailures < kMaxExecutorFailureLog) {
            fprintf(stderr,
                "[EXECUTOR v1] FAIL island=%s check=sceneDepthTex_valid "
                "owned=%lu failures=%lu\n",
                name, g_executorOwnedPasses, g_executorValidationFailures + 1);
        }
        ++g_executorValidationFailures;
    }
    // warnIfNoTerrainLatch: can't happen (WillRun checks sceneHasTerrain_) but assert for symmetry.
    if (c->warnIfNoTerrainLatch && !pp->executorSceneHasTerrain()) {
        fprintf(stderr,
            "[EXECUTOR v1] ASSERT island=%s warnIfNoTerrainLatch but WillRun passed — logic error\n", name);
    }

    // FRAMEGRAPH-APPLY-STATE-ISLAND-1/2: pre-apply declared GL state before the body.
    // findSubStageState() guards: if the island is not in the table, we do nothing.
    // The pre-apply is idempotent: the body makes identical calls at entry.
    // ISLAND-1: EdgeFog wired. ISLAND-2: FogOob/Shoreline/CloudShadow wired.
    if (islandId == ExecutorIslandId::EdgeFog) {
        using namespace RenderCore::framegraph;
        if (findSubStageState(ExecutorIslandId::EdgeFog) != nullptr)
            pp->executorApplyEdgeFogState();
    }
    if (islandId == ExecutorIslandId::FogOob) {
        using namespace RenderCore::framegraph;
        if (findSubStageState(ExecutorIslandId::FogOob) != nullptr)
            pp->executorApplyFogOobState();
    }
    if (islandId == ExecutorIslandId::Shoreline) {
        using namespace RenderCore::framegraph;
        if (findSubStageState(ExecutorIslandId::Shoreline) != nullptr)
            pp->executorApplyShorelineState();
    }
    if (islandId == ExecutorIslandId::CloudShadow) {
        using namespace RenderCore::framegraph;
        if (findSubStageState(ExecutorIslandId::CloudShadow) != nullptr)
            pp->executorApplyCloudShadowState();
    }
    // APPLY-STATE-SCREENSHADOW-1: ScreenShadow wired.
    if (islandId == ExecutorIslandId::ScreenShadow) {
        using namespace RenderCore::framegraph;
        if (findSubStageState(ExecutorIslandId::ScreenShadow) != nullptr)
            pp->executorApplyScreenShadowState();
    }
}

static void executorOwnEndSub(gosPostProcess* pp, RenderCore::framegraph::ExecutorIslandId islandId)
{
    if (!executorEnabled()) return;

    using namespace RenderCore::framegraph;
    const IslandContract* c = findIslandContract(islandId);
    if (!c) return;

    // If the sub-pass didn't run (WillRun==false at Begin time) we returned early there.
    // Re-checking WillRun here would be racy if state changed; use a lightweight proxy:
    // if g_executorOwnedPasses hasn't changed since Begin, we are in the skip path.
    // Simpler: just guard with a local flag pair — but to keep this dependency-free we
    // replicate the WillRun check. This is safe: WillRun() is pure state read, no side effects.
    bool willRun = false;
    if (islandId == ExecutorIslandId::EdgeFog)       willRun = pp->executorEdgeFogWillRun();
    if (islandId == ExecutorIslandId::FogOob)        willRun = pp->executorFogOobWillRun();
    if (islandId == ExecutorIslandId::Shoreline)     willRun = pp->executorShorelineWillRun();
    if (islandId == ExecutorIslandId::CloudShadow)   willRun = pp->executorCloudShadowWillRun();
    if (islandId == ExecutorIslandId::ScreenShadow)  willRun = pp->executorScreenShadowWillRun();

    // NOTE: after run*() completes, sceneHasTerrain_ is still true (only cleared at beginScene).
    // WillRun() re-check is safe here.
    if (!willRun) return;

    const char* name = "Unknown";
    if (islandId == ExecutorIslandId::EdgeFog)       name = "EdgeFog";
    if (islandId == ExecutorIslandId::FogOob)        name = "FogOob";
    if (islandId == ExecutorIslandId::Shoreline)     name = "Shoreline";
    if (islandId == ExecutorIslandId::CloudShadow)   name = "CloudShadow";
    if (islandId == ExecutorIslandId::ScreenShadow)  name = "ScreenShadow";
    bool postOk = true;

    // 1. GL_BLEND must be disabled (both run*() call glDisable(GL_BLEND) on exit).
    if (c->postRequiresBlendDisabled) {
        if (glIsEnabled(GL_BLEND) != GL_FALSE) {
            if (g_executorValidationFailures < kMaxExecutorFailureLog)
                fprintf(stderr,
                    "[EXECUTOR v1] FAIL island=%s postcond=blendDisabled "
                    "owned=%lu failures=%lu\n",
                    name, g_executorOwnedPasses, g_executorValidationFailures + 1);
            ++g_executorValidationFailures;
            postOk = false;
        }
    }

    // 2. Active texture unit must be GL_TEXTURE0 (both run*() restore it on exit).
    if (c->postRequiresActiveTexture0) {
        GLint activeUnit = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeUnit);
        if (activeUnit != GL_TEXTURE0) {
            if (g_executorValidationFailures < kMaxExecutorFailureLog)
                fprintf(stderr,
                    "[EXECUTOR v1] FAIL island=%s postcond=activeTexture0 "
                    "activeUnit=0x%X owned=%lu failures=%lu\n",
                    name, (unsigned)activeUnit, g_executorOwnedPasses, g_executorValidationFailures + 1);
            ++g_executorValidationFailures;
            postOk = false;
        }
    }

    // 3. No GL errors after the sub-stage draw.
    {
        const GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            if (g_executorValidationFailures < kMaxExecutorFailureLog)
                fprintf(stderr,
                    "[EXECUTOR v1] FAIL island=%s postcond=glNoError "
                    "err=0x%X owned=%lu failures=%lu\n",
                    name, (unsigned)err, g_executorOwnedPasses, g_executorValidationFailures + 1);
            ++g_executorValidationFailures;
            postOk = false;
        }
    }

    if (postOk)
        ++g_executorOwnedPasses;
}

// --- Public entry points (called from gameosmain.cpp) -----------------------

void mc2_executor_own_begin_postprocess(gosPostProcess* pp)
{
    executorOwnBegin(pp);
}

void mc2_executor_own_end_postprocess(gosPostProcess* pp)
{
    executorOwnEnd(pp);
}

// --- extern "C" counter accessors for debug_state_dump.cpp -----------------

extern "C" unsigned long mc2_framegraph_executor_owned_passes()
{
    return g_executorOwnedPasses;
}

extern "C" unsigned long mc2_framegraph_executor_validation_failures()
{
    return g_executorValidationFailures;
}

// FRAMEGRAPH-APPLY-STATE-ISLAND-1: apply-state counter (0 until EdgeFog fires, then >0 per frame).
// Declared extern "C" so debug_state_dump.cpp and render_contract.cpp can forward-declare + call.
// PER-PASS-APPLY-COUNTERS-1: aggregate is DERIVED as the sum over the per-pass
// array. Back-compat: equals the former g_applyStatePasses total (every apply
// site bumps exactly one slot), so executor_apply_state_passes is byte-stable.
extern "C" unsigned long mc2_framegraph_executor_apply_state_passes_impl()
{
    unsigned long total = 0;
    for (int i = 0; i < (int)RenderCore::framegraph::ApplyPassId::Count; ++i)
        total += g_applyStateByPass[i];
    return total;
}

// PER-PASS-APPLY-COUNTERS-1: per-pass getter. Returns the count for one
// ApplyPassId, or 0 for an out-of-range id.
extern "C" unsigned long mc2_framegraph_executor_apply_state_by_pass_impl(unsigned id)
{
    return id < (unsigned)RenderCore::framegraph::ApplyPassId::Count ? g_applyStateByPass[id] : 0;
}

// PER-PASS-APPLY-COUNTERS-1: cross-TU bump of the per-pass apply-state array so a
// top-level apply-state pass (gosRenderer::executorApplyTerrainDecalState /
// ...OverlayState in gameos_graphics.cpp, executorApplyStaticPropOpaqueState in
// gos_static_prop_batcher.cpp) records into ITS OWN slot — independently
// observable, and aggregating into the sum-derived total without double-count.
extern "C" void mc2_framegraph_executor_bump_apply_state(unsigned id)
{
    applyStateBump((RenderCore::framegraph::ApplyPassId)id);
}
