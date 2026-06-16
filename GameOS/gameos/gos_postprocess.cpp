#include "gos_postprocess.h"
#include "utils/shader_builder.h"
#include "utils/gl_utils.h"
#include "utils/vec.h"
#include "gos_hdri.h"
#include "gos_profiler.h"
#include "gos_validate.h"  // drainGLErrors (Tier-1 instr §4)
#include "gos_smoke.h"     // S9E: SmokeMode fixed deterministic render-shader clock
#include "gameos.hpp"      // gos_InvalidateRenderStateCache (RENDER_STATES v1)
#include "../../RenderWorld/RenderWorld.h"  // M1.5: IsObjectIdBufferEnabled
#include "../../RenderCore/RenderResourceRegistry.h"
#include "../../RenderCore/EngineView.h"
#include "view_uniforms_gl.h"
#include "gos_static_prop_registry.h"   // HZB-STATICPROP-CULL-RECON-1: real bounds for the probe

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <SDL2/SDL.h>

extern bool g_force43Aspect;

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
}

} // namespace

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

bool gos_IsHdrPostEnabled() { return s_postProcess && s_postProcess->hdrPostEnabled_; }

// BLOOM-MVP-1 tunables (profile + ImGui). Clamped to safe conservative ranges.
void gos_SetBloomThreshold(float v) {
    if (s_postProcess) s_postProcess->bloomThreshold_ = (v < 0.0f ? 0.0f : (v > 4.0f ? 4.0f : v));
}
void gos_SetBloomIntensity(float v) {
    if (s_postProcess) s_postProcess->bloomIntensity_ = (v < 0.0f ? 0.0f : (v > 4.0f ? 4.0f : v));
}
float gos_GetBloomThreshold() { return s_postProcess ? s_postProcess->bloomThreshold_ : 1.2f; }
float gos_GetBloomIntensity() { return s_postProcess ? s_postProcess->bloomIntensity_ : 0.15f; }

// SSAO-GTAO-LITE-MVP-1 tunables (clamped to conservative ranges).
bool  gos_IsSsaoEnabled() { return s_postProcess && s_postProcess->ssaoEnabled_; }
void  gos_SetSsaoRadius(float v)   { if (s_postProcess) s_postProcess->aoRadius_   = (v < 0.1f ? 0.1f : (v > 64.0f ? 64.0f : v)); }
void  gos_SetSsaoStrength(float v) { if (s_postProcess) s_postProcess->aoStrength_ = (v < 0.0f ? 0.0f : (v > 2.0f  ? 2.0f  : v)); }
void  gos_SetSsaoBias(float v)     { if (s_postProcess) s_postProcess->aoBias_     = (v < 0.0f ? 0.0f : (v > 0.1f  ? 0.1f  : v)); }
float gos_GetSsaoRadius()   { return s_postProcess ? s_postProcess->aoRadius_   : 3.0f; }
float gos_GetSsaoStrength() { return s_postProcess ? s_postProcess->aoStrength_ : 0.7f; }
float gos_GetSsaoBias()     { return s_postProcess ? s_postProcess->aoBias_     : 0.0025f; }

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
    , bloomEnabled_(false)
    , fxaaEnabled_(false)
    , tonemapEnabled_(false)
    , bloomIntensity_(0.15f)  // BLOOM-MVP-1 tuned 2026-05-29 (was 0.3; halved)
    , bloomThreshold_(1.2f)   // BLOOM-MVP-1 tuned 2026-05-29 (was 0.6; doubled)
    , hdrPostEnabled_(false)
    , sceneFBO_(0)
    , sceneColorTex_(0)
    , sceneDepthTex_(0)
    , sceneNormalTex_(0)
    , quadVAO_(0)
    , quadVBO_(0)
    , compositeProg_(nullptr)
    , skyboxProg_(nullptr)
    , bloomThresholdProg_(nullptr)
    , bloomBlurProg_(nullptr)
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
    , screenShadowProg_(nullptr)
    , screenShadowEnabled_(true)
    , screenShadowDebug_(0)
    , sceneHasTerrain_(false)
    , prevFrameHadTerrain_(false)
    , godrayEnabled_(false)  // disabled: no visible sky at RTS zoom. RAlt+6 to test.
    , godrayProg_(nullptr)
    , godrayFBO_(0)
    , godrayColorTex_(0)
    , shorelineEnabled_(true)
    , shorelineProg_(nullptr)
    , ssaoEnabled_(false)
    , ssaoDebug_(0)
    , aoRadius_(3.0f)
    , aoStrength_(0.7f)
    , aoBias_(0.0025f)
    , aoPower_(1.5f)
{
    bloomFBO_[0] = bloomFBO_[1] = 0;
    bloomColorTex_[0] = bloomColorTex_[1] = 0;
    memset(staticLightSpaceMatrix_, 0, sizeof(staticLightSpaceMatrix_));
    memset(dynamicLightSpaceMatrix_, 0, sizeof(dynamicLightSpaceMatrix_));
    memset(savedViewport_, 0, sizeof(savedViewport_));
    memset(inverseViewProj_, 0, sizeof(inverseViewProj_));
    memset(viewProj_, 0, sizeof(viewProj_));
    showShadowDebug_ = false;
    shadowDebugMode_ = 0;
    sunScreenPos_[0] = 0.5f;
    sunScreenPos_[1] = 0.5f;
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
            const char* hdrPath = "data/hdr/DaySkyHDRI063B_4K.exr";
            hdriTex_ = loadHdriTexture(hdrPath);  // logs failures internally

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

    // HDR-POST-SCAFFOLD-1 (Track V) master gate. Resolved once from env.
    // Default OFF: scene is already RGBA16F, but bloom + ACES tonemap stay
    // force-disabled so output is byte-identical to legacy. =1 enables the
    // post stack; the sub-features have their own gates (MC2_BLOOM /
    // MC2_TONEMAP_ACES) read in beginScene-time wiring elsewhere.
    {
        const char* hdrEnv = getenv("MC2_HDR_POST");
        hdrPostEnabled_ = (hdrEnv && hdrEnv[0] && hdrEnv[0] != '0');
        std::fprintf(stderr, "[HDR_POST v1] enabled=%d (MC2_HDR_POST=%s)\n",
                     hdrPostEnabled_ ? 1 : 0, hdrEnv ? hdrEnv : "(unset)");

        // BLOOM-MVP-1 (Track V, MC2_BLOOM): sub-feature of the HDR post stack.
        // Sets bloomEnabled_; only takes effect when hdrPostEnabled_ (enforced
        // in runBloom + composite). Conservative member defaults (threshold
        // 0.6 / intensity 0.3) are ImGui- and profile-tunable.
        const char* bloomEnv = getenv("MC2_BLOOM");
        if (bloomEnv && bloomEnv[0] && bloomEnv[0] != '0')
            bloomEnabled_ = true;
        std::fprintf(stderr, "[BLOOM v1] enabled=%d (MC2_BLOOM=%s, requires MC2_HDR_POST)\n",
                     (hdrPostEnabled_ && bloomEnabled_) ? 1 : 0,
                     bloomEnv ? bloomEnv : "(unset)");

        // TONEMAP-ACES-MVP-1 (Track V, MC2_TONEMAP_ACES): sub-feature of the
        // HDR post stack. Sets tonemapEnabled_; the composite forces
        // enableTonemap=0 unless hdrPostEnabled_, so this is inert without the
        // master gate. ACES curve (postprocess.frag ACESFilm) already present;
        // exposure is tunable via gos_SetExposure / profile 'exposure'.
        const char* tonemapEnv = getenv("MC2_TONEMAP_ACES");
        if (tonemapEnv && tonemapEnv[0] && tonemapEnv[0] != '0')
            tonemapEnabled_ = true;
        std::fprintf(stderr, "[TONEMAP_ACES v1] enabled=%d (MC2_TONEMAP_ACES=%s, requires MC2_HDR_POST)\n",
                     (hdrPostEnabled_ && tonemapEnabled_) ? 1 : 0,
                     tonemapEnv ? tonemapEnv : "(unset)");
    }

    bloomThresholdProg_ = glsl_program::makeProgram("bloom_threshold",
        "shaders/postprocess.vert", "shaders/bloom_threshold.frag", kShaderPrefix);
    if (!bloomThresholdProg_ || !bloomThresholdProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile bloom_threshold shader\n");

    bloomBlurProg_ = glsl_program::makeProgram("bloom_blur",
        "shaders/postprocess.vert", "shaders/bloom_blur.frag", kShaderPrefix);
    if (!bloomBlurProg_ || !bloomBlurProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile bloom_blur shader\n");

    shadowDebugProg_ = glsl_program::makeProgram("shadow_debug",
        "shaders/postprocess.vert", "shaders/shadow_debug.frag", kShaderPrefix);
    if (!shadowDebugProg_ || !shadowDebugProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile shadow_debug shader\n");

    screenShadowProg_ = glsl_program::makeProgram("shadow_screen",
        "shaders/postprocess.vert", "shaders/shadow_screen.frag", kShaderPrefix);
    if (!screenShadowProg_ || !screenShadowProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile shadow_screen shader\n");

    godrayProg_ = glsl_program::makeProgram("godray",
        "shaders/postprocess.vert", "shaders/godray.frag", kShaderPrefix);
    if (!godrayProg_ || !godrayProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile godray shader\n");

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

    if (bloomThresholdProg_) {
        glsl_program::deleteProgram("bloom_threshold");
        bloomThresholdProg_ = nullptr;
    }
    if (bloomBlurProg_) {
        glsl_program::deleteProgram("bloom_blur");
        bloomBlurProg_ = nullptr;
    }

    if (shadowDebugProg_) {
        glsl_program::deleteProgram("shadow_debug");
        shadowDebugProg_ = nullptr;
    }

    if (screenShadowProg_) {
        glsl_program::deleteProgram("shadow_screen");
        screenShadowProg_ = nullptr;
    }

    if (godrayProg_) {
        glsl_program::deleteProgram("godray");
        godrayProg_ = nullptr;
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
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);

    // Color attachment: RGBA16F
    glGenTextures(1, &sceneColorTex_);
    glBindTexture(GL_TEXTURE_2D, sceneColorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sceneColorTex_, 0);

    // Depth/stencil texture (sampleable for post-process depth reconstruction)
    glGenTextures(1, &sceneDepthTex_);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, sceneDepthTex_, 0);

    // Normal buffer: MRT attachment 1 (rgb=world normal encoded, a=shadow skip flag)
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
        dd.format    = RenderCore::RenderResourceFormat::Depth24;
        dd.debugName = "MainDepth";
        dd.width     = (uint32_t)w;
        dd.height    = (uint32_t)h;
        dd.glName    = sceneDepthTex_;
        dd.valid     = (status == GL_FRAMEBUFFER_COMPLETE);
        RenderCore::registerOrUpdateRenderResource(dd);
    }

    // --- Bloom ping-pong FBOs (half resolution) ---
    int halfW = w / 2;
    int halfH = h / 2;
    if (halfW < 1) halfW = 1;
    if (halfH < 1) halfH = 1;

    for (int i = 0; i < 2; ++i) {
        glGenFramebuffers(1, &bloomFBO_[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO_[i]);

        glGenTextures(1, &bloomColorTex_[i]);
        glBindTexture(GL_TEXTURE_2D, bloomColorTex_[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, halfW, halfH, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bloomColorTex_[i], 0);

        status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "gosPostProcess: bloom FBO[%d] incomplete (0x%x)\n", i, status);
        }
    }

    // --- SSAO FBO (half resolution, single-channel R16F) ---
    {
        ssaoW_ = w / 2; if (ssaoW_ < 1) ssaoW_ = 1;
        ssaoH_ = h / 2; if (ssaoH_ < 1) ssaoH_ = 1;

        glGenTextures(1, &ssaoColorTex_);
        glBindTexture(GL_TEXTURE_2D, ssaoColorTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, ssaoW_, ssaoH_, 0, GL_RED, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &ssaoFBO_);
        glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ssaoColorTex_, 0);

        GLenum aoStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (aoStatus != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "gosPostProcess: SSAO FBO incomplete (0x%x)\n", aoStatus);
    }

    // --- God ray FBO (half resolution) ---
    {
        int ghw = w / 2, ghh = h / 2;
        if (ghw < 1) ghw = 1;
        if (ghh < 1) ghh = 1;

        glGenTextures(1, &godrayColorTex_);
        glBindTexture(GL_TEXTURE_2D, godrayColorTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, ghw, ghh, 0, GL_RGBA, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &godrayFBO_);
        glBindFramebuffer(GL_FRAMEBUFFER, godrayFBO_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, godrayColorTex_, 0);

        GLenum grStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (grStatus != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "gosPostProcess: god ray FBO incomplete (0x%x)\n", grStatus);
    }

    // --- WATER-REFLECTION-RESOURCE-1: quarter-res reflection target ---
    // Substrate only: allocated + registered, but NO producer renders into it
    // until Phase C, so the texture reads black. Color (RGBA16F) + depth (D24).
    {
        waterReflW_ = w / 4; if (waterReflW_ < 1) waterReflW_ = 1;
        waterReflH_ = h / 4; if (waterReflH_ < 1) waterReflH_ = 1;

        glGenTextures(1, &waterReflColorTex_);
        glBindTexture(GL_TEXTURE_2D, waterReflColorTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, waterReflW_, waterReflH_, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

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
        // The destination level texture is bound to COLOR_ATTACHMENT0 per pass
        // in runHzbReduce(). No dedicated registry slot yet -- surfaced via the
        // getHzb* accessors.

        std::fprintf(stderr, "[HZB_BUILD v1] allocated %dx%d mips=%d (R32F, per-level textures)\n",
                     hzbW_, hzbH_, hzbMipCount_);
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
        glGenTextures(1, &sceneDepthCopyTex_);
        glBindTexture(GL_TEXTURE_2D, sceneDepthCopyTex_);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, width_, height_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // Texture-to-texture copy of the whole depth image. Ordered after the
    // opaque scene's depth writes; particles never write depth so the source
    // is stable at flush time.
    glCopyImageSubData(sceneDepthTex_,     GL_TEXTURE_2D, 0, 0, 0, 0,
                       sceneDepthCopyTex_, GL_TEXTURE_2D, 0, 0, 0, 0,
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
    if (sceneNormalTex_) {
        glDeleteTextures(1, &sceneNormalTex_);
        sceneNormalTex_ = 0;
    }
    if (sceneObjectIdTex_) {
        glDeleteTextures(1, &sceneObjectIdTex_);
        sceneObjectIdTex_ = 0;
    }
    if (sceneDepthCopyTex_) {  // VFX-SOFT-PARTICLES-MVP-1
        glDeleteTextures(1, &sceneDepthCopyTex_);
        sceneDepthCopyTex_ = 0;
    }
    for (int i = 0; i < 2; ++i) {
        if (bloomFBO_[i]) {
            glDeleteFramebuffers(1, &bloomFBO_[i]);
            bloomFBO_[i] = 0;
        }
        if (bloomColorTex_[i]) {
            glDeleteTextures(1, &bloomColorTex_[i]);
            bloomColorTex_[i] = 0;
        }
    }
    if (godrayColorTex_) { glDeleteTextures(1, &godrayColorTex_); godrayColorTex_ = 0; }
    if (godrayFBO_) { glDeleteFramebuffers(1, &godrayFBO_); godrayFBO_ = 0; }

    // SSAO-GTAO-LITE-MVP-1: free half-res AO target.
    if (ssaoColorTex_) { glDeleteTextures(1, &ssaoColorTex_); ssaoColorTex_ = 0; }
    if (ssaoFBO_)      { glDeleteFramebuffers(1, &ssaoFBO_);   ssaoFBO_ = 0; }
    ssaoW_ = ssaoH_ = 0;

    // HZB-DEPTH-PYRAMID-MVP-1: free the per-level pyramid textures + FBO
    // (resize re-allocates if the gate is on).
    for (int i = 0; i < kHzbMaxLevels; ++i) {
        if (hzbLevelTex_[i]) { glDeleteTextures(1, &hzbLevelTex_[i]); hzbLevelTex_[i] = 0; }
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

void gosPostProcess::beginScene()
{
    if (!initialized_)
        return;

    prevFrameHadTerrain_ = sceneHasTerrain_;  // save for clear color decision
    sceneHasTerrain_ = false;  // reset each frame; set by markTerrainDrawn()

    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
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

void gosPostProcess::runBloom()
{
    if (!hdrPostEnabled_) return;  // HDR-POST-SCAFFOLD-1: master gate
    if (!bloomEnabled_ || !bloomThresholdProg_ || !bloomBlurProg_) return;
    if (!bloomThresholdProg_->is_valid() || !bloomBlurProg_->is_valid()) return;

    int hw = width_ / 2, hh = height_ / 2;
    if (hw < 1) hw = 1;
    if (hh < 1) hh = 1;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    // Pass 1: Threshold — extract bright pixels from scene into bloomFBO_[0]
    glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO_[0]);
    glViewport(0, 0, hw, hh);
    glClear(GL_COLOR_BUFFER_BIT);

    bloomThresholdProg_->setInt("sceneTex", 0);
    bloomThresholdProg_->setFloat("threshold", bloomThreshold_);
    bloomThresholdProg_->apply();  // flush uniforms + bind program

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneColorTex_);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Pass 2+: Ping-pong Gaussian blur (2 iterations = 4 passes)
    float texelSize[2] = { 1.0f / (float)hw, 1.0f / (float)hh };
    bloomBlurProg_->setFloat2("texelSize", texelSize);
    bloomBlurProg_->setInt("image", 0);

    bool horiz = true;
    for (int i = 0; i < 4; i++) {
        int src = horiz ? 0 : 1;
        int dst = horiz ? 1 : 0;

        glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO_[dst]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, bloomColorTex_[src]);
        bloomBlurProg_->setInt("horizontal", horiz ? 1 : 0);
        bloomBlurProg_->apply();  // flush uniforms + bind program each pass
        glDrawArrays(GL_TRIANGLES, 0, 6);

        horiz = !horiz;
    }
    // Result is in bloomColorTex_[0]

    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
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

    if (ssaoDebug_ == 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_DST_COLOR, GL_ZERO);   // scene *= ao
    } else {
        glDisable(GL_BLEND);                  // overwrite with AO grayscale
    }

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

    // Render to sceneFBO_ color-only (no normal write) with multiplicative blending
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    // M1.5: single-color composite. Helper preserves env-OFF/ON parity
    // (the postprocess composite never writes attachment-2 regardless).
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    // Multiplicative blending: dst * src (shadow darkening)
    // In debug mode, overwrite scene color entirely
    if (screenShadowDebug_ == 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_DST_COLOR, GL_ZERO);
    } else {
        glDisable(GL_BLEND);
    }

    // Set uniforms BEFORE apply()
    screenShadowProg_->setInt("sceneDepthTex", 0);
    screenShadowProg_->setInt("sceneNormalTex", 1);
    screenShadowProg_->setInt("shadowMap", 2);
    screenShadowProg_->setInt("dynamicShadowMap", 3);
    screenShadowProg_->setInt("overlayPass", 0);
    screenShadowProg_->setInt("enableShadows", shadowsEnabled_ ? 1 : 0);
    screenShadowProg_->setInt("enableDynamicShadows", (dynShadowDepthTex_ != 0) ? 1 : 0);
    screenShadowProg_->setFloat("shadowSoftness", 0.9f);  // match terrain default
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
    loc = glGetUniformLocation(screenShadowProg_->shp_, "dynamicLightSpaceMatrix");
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, dynamicLightSpaceMatrix_);

    // Bind textures
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sceneNormalTex_);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, shadowDepthTex_);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, dynShadowDepthTex_);

    // Draw fullscreen quad — pass 1: normal (skip terrain)
    // Draw fullscreen quad - single pass for terrain, objects, and overlays.
    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindVertexArray(0);

    // Restore state
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
}

void gosPostProcess::runGodRays()
{
    ZoneScopedN("Render.GodRays");
    TracyGpuZone("Render.GodRays");

    if (!godrayEnabled_ || !sceneHasTerrain_ || !godrayProg_ || !godrayProg_->is_valid()) {
        return;
    }
    static int gr_run = 0;
    if (gr_run++ < 3)
        fprintf(stderr, "GodRays RUNNING: sunPos=%.2f,%.2f halfRes=%dx%d\n",
            sunScreenPos_[0], sunScreenPos_[1], width_/2, height_/2);

    int hw = width_ / 2, hh = height_ / 2;
    if (hw < 1) hw = 1;
    if (hh < 1) hh = 1;

    // Pass 1: Render god rays into half-res FBO
    glBindFramebuffer(GL_FRAMEBUFFER, godrayFBO_);
    glViewport(0, 0, hw, hh);
    glClear(GL_COLOR_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    float elapsed = SmokeMode::fixedTimestepEnabled()
                        ? (float)SmokeMode::fixedClockSeconds()
                        : (float)SDL_GetTicks() / 1000.0f;

    godrayProg_->setInt("sceneDepthTex", 0);
    godrayProg_->setInt("sceneColorTex", 1);
    godrayProg_->setFloat2("sunScreenPos", sunScreenPos_);
    godrayProg_->setFloat("time", elapsed);
    godrayProg_->apply();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sceneColorTex_);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Pass 2: Additive composite onto scene at full res
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    // M1.5: single-color composite (additive); helper preserves env shape.
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);  // Additive

    // Use bloom threshold shader as pass-through (threshold = -1 passes everything)
    bloomThresholdProg_->setInt("sceneTex", 0);
    bloomThresholdProg_->setFloat("threshold", -1.0f);
    bloomThresholdProg_->apply();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, godrayColorTex_);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
}

void gosPostProcess::runShoreline()
{
    ZoneScopedN("Render.Shoreline");
    TracyGpuZone("Render.Shoreline");

    if (!shorelineEnabled_ || !sceneHasTerrain_ || !shorelineProg_ || !shorelineProg_->is_valid()) return;

    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    // M1.5: single-color multiplicative composite; helper preserves env shape.
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    // Multiplicative blend: values > 1.0 brighten water at shoreline
    glEnable(GL_BLEND);
    glBlendFunc(GL_DST_COLOR, GL_ZERO);

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

void gosPostProcess::endScene()
{
    ZoneScopedN("Render.PostProcess");
    TracyGpuZone("Render.PostProcess");

    if (!initialized_)
        return;

    // HZB-DEPTH-PYRAMID-MVP-1: build the reverse-Z Hi-Z pyramid from the
    // resolved scene depth before any post pass. Gated (MC2_HZB_BUILD), no
    // consumers, no draw suppression -> no-op + byte-identical when OFF.
    runHzbReduce();
    runHzbProbe();   // diagnostic-only; reads the pyramid, suppresses no draws

    // Post-process shadow pass: covers terrain, objects, and overlays in one
    // pass, with reduced terrain darkening to avoid obvious double-shadowing.
    runScreenShadow();

    // Shoreline foam pass (brightens water pixels adjacent to terrain)
    runShoreline();

    // God rays pass (radial light scattering, additive)
    runGodRays();

    // SSAO grounding pass (multiplicative darkening into scene color). Before
    // bloom so bloom extracts from the AO-darkened scene. Default-OFF (gated).
    runSSAO();

    runBloom();

    // Bind default framebuffer and blit scene into a centered 4:3 viewport
    // (pillarbox on ultrawide, letterbox on tall displays).
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    const int screenW = Environment.drawableWidth;
    const int screenH = Environment.drawableHeight;
    if (g_force43Aspect) {
        const float kTargetAspect = 4.0f / 3.0f;
        int blitW, blitH, blitX, blitY;
        if ((float)screenW / (float)screenH > kTargetAspect) {
            blitH = screenH;
            blitW = (int)(screenH * kTargetAspect);
            blitX = (screenW - blitW) / 2;
            blitY = 0;
        } else {
            blitW = screenW;
            blitH = (int)(screenW / kTargetAspect);
            blitX = 0;
            blitY = (screenH - blitH) / 2;
        }

        glViewport(0, 0, screenW, screenH);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glViewport(blitX, blitY, blitW, blitH);
    } else {
        glViewport(0, 0, width_, height_);
    }

    // Disable depth test and face culling for fullscreen quad
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    // gosFX/MLR additive draws (gos_Alpha_OneOne) leak GL_BLEND+GL_ONE/GL_ONE
    // state into the composite. With an RGBA8 backbuffer that clamps at 1.0,
    // the additive accumulation saturates to white over ~1s — pylon power
    // generator effect on mc2_05/mc2_24 was the canary. Composite is meant
    // to fully overwrite the backbuffer; force opaque.
    glDisable(GL_BLEND);

    // Draw fullscreen quad with composite shader
    if (compositeProg_ && compositeProg_->is_valid()) {
        // Set uniforms BEFORE apply() — apply() binds program + flushes dirty uniforms
        compositeProg_->setInt("sceneTex", 0);
        compositeProg_->setInt("bloomTex", 1);
        // HDR-POST-SCAFFOLD-1: master gate. When the HDR post stack is OFF,
        // force bloom + ACES tonemap off regardless of their member flags so
        // the default path is byte-identical to legacy (exposure stays 1.0
        // no-op; the unconditional sunset grade in postprocess.frag is the
        // pre-existing default look and is untouched).
        const bool hdrOn = hdrPostEnabled_;
        compositeProg_->setFloat("exposure", exposure_);
        compositeProg_->setInt("enableBloom", (hdrOn && bloomEnabled_) ? 1 : 0);
        compositeProg_->setInt("enableFXAA", fxaaEnabled_ ? 1 : 0);
        compositeProg_->setInt("enableTonemap", (hdrOn && tonemapEnabled_) ? 1 : 0);
        compositeProg_->setFloat("bloomIntensity", bloomIntensity_);

        float invSize[2] = { 1.0f / (float)width_, 1.0f / (float)height_ };
        compositeProg_->setFloat2("inverseScreenSize", invSize);

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

        // Bind scene color texture to unit 0
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sceneColorTex_);

        // Bind bloom texture to unit 1 (unused for now, bind first bloom tex)
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, bloomColorTex_[0]);

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
}

void gosPostProcess::drawShadowDebugOverlay()
{
    if (!showShadowDebug_ || !shadowDebugProg_ || !shadowDebugProg_->is_valid())
        return;
    if (!initialized_)
        return;

    GLuint tex = (shadowDebugMode_ == 0) ? shadowDepthTex_ : dynShadowDepthTex_;
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
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

    shadowDebugProg_->setInt("shadowDebugMap", 0);
    shadowDebugProg_->apply();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // CRITICAL: restore comparison mode so PCF sampling works next frame
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);

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

    // Compute sun screen position by projecting sun direction through VP matrix
    {
        float sunWorld[4] = { sunDirX * 100000.0f, sunDirY * 100000.0f, sunDirZ * 100000.0f, 1.0f };
        float clip[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        // viewProj_ is column-major
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                clip[r] += viewProj_[c * 4 + r] * sunWorld[c];
        if (clip[3] > 0.0f) {
            sunScreenPos_[0] = (clip[0] / clip[3]) * 0.5f + 0.5f;
            sunScreenPos_[1] = (clip[1] / clip[3]) * 0.5f + 0.5f;
        }
    }
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

    // Bind shader + uniforms + texture.
    hdriSkyboxProg_->apply();
    hdriSkyboxProg_->setMat4("invProj", invProjArray);
    hdriSkyboxProg_->setMat3("invViewRot", invViewRot);
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

    // Note: do NOT call glDrawBuffers anywhere in this function.
    // setSceneDrawBuffers owns the FBO draw-buffer array.
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
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFBO_);

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
    dynShadowMapSize_ = 4096;

    glGenFramebuffers(1, &dynShadowFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, dynShadowFBO_);

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
        d.format    = RenderCore::RenderResourceFormat::Depth24;
        d.debugName = "ShadowDynamicMap";
        d.width     = static_cast<uint32_t>(dynShadowMapSize_);
        d.height    = static_cast<uint32_t>(dynShadowMapSize_);
        d.glName    = static_cast<uint32_t>(dynShadowDepthTex_);
        d.sizeBytes = static_cast<uint64_t>(dynShadowMapSize_) * static_cast<uint64_t>(dynShadowMapSize_) * 4u;
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }
}

void gosPostProcess::destroyDynamicShadows()
{
    if (dynShadowFBO_) { glDeleteFramebuffers(1, &dynShadowFBO_); dynShadowFBO_ = 0; }
    if (dynShadowDepthTex_) { glDeleteTextures(1, &dynShadowDepthTex_); dynShadowDepthTex_ = 0; }
    if (dynShadowDummyColorTex_) { glDeleteTextures(1, &dynShadowDummyColorTex_); dynShadowDummyColorTex_ = 0; }

    RenderCore::RenderResourceDesc invalid;
    invalid.id = RenderCore::RenderResourceId::ShadowDynamicMap;
    RenderCore::registerOrUpdateRenderResource(invalid);
}

void gosPostProcess::buildDynamicLightMatrix(float sunDirX, float sunDirY, float sunDirZ,
                                              const float camFitCornersMC2[8][3])
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

    // SHADOW-FOCUS-CENTER-1 (gate, default OFF): center the shadow box on the
    // camera's near-ground FOCUS POINT instead of the frustum-corner AABB
    // centroid. For an oblique near-ground camera the far/horizon corners drag
    // the AABB centroid off-map (measured worldCenter=(-17131,-35844,7526)
    // while the scene is at ~(+-300,3349,635)) and oscillate near map edges
    // -> "shadows reflect off the map edge". Focus-point centering decouples
    // the center from the far corners: box tracks where the player looks.
    // KEEP the radius fit (halfLX/halfLY -> fitRadius) unchanged below; ONLY
    // the center (cxL/cyL) is overridden, BEFORE the texel-snap + back-project.
    static const bool s_focusCenter = []() {
        const char* v = getenv("MC2_SHADOW_FOCUS_CENTER");
        return (v && v[0] == '1');
    }();
    bool focusApplied = false;
    float focusWorld[3] = {0.0f, 0.0f, 0.0f};
    float focusDistUsed = 0.0f;
    float nearSpread = 0.0f, farSpread = 0.0f;
    if (s_focusCenter) {
        static const float s_focusDist = []() {
            const char* v = getenv("MC2_SHADOW_FOCUS_DIST");
            float d = (v ? (float)atof(v) : 1500.0f);
            if (d < 256.0f)  d = 256.0f;
            if (d > 8000.0f) d = 8000.0f;
            return d;
        }();
        // Robust near/far corner identification. Corners 0-3 have GL-NDC z=0,
        // 4-7 have z=1 (txmmgr.cpp:2259-2277), but we do NOT assume which is
        // the near plane -- we detect by SPREAD. A perspective frustum's near
        // corners are closer together (smaller max corner-to-centroid dist)
        // than the far corners. The set with the smaller spread is NEAR.
        float cA[3] = {0,0,0}, cB[3] = {0,0,0};   // A = corners[0..3], B = corners[4..7]
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
        // Pick near = smaller spread.
        const float* nearC; const float* farC;
        if (spreadA <= spreadB) { nearC = cA; farC = cB; nearSpread = spreadA; farSpread = spreadB; }
        else                    { nearC = cB; farC = cA; nearSpread = spreadB; farSpread = spreadA; }

        float vd[3] = { farC[0]-nearC[0], farC[1]-nearC[1], farC[2]-nearC[2] };
        float vlen = sqrtf(vd[0]*vd[0] + vd[1]*vd[1] + vd[2]*vd[2]);
        if (vlen > 1e-3f) {
            vd[0] /= vlen; vd[1] /= vlen; vd[2] /= vlen;
            focusWorld[0] = nearC[0] + vd[0] * s_focusDist;
            focusWorld[1] = nearC[1] + vd[1] * s_focusDist;
            focusWorld[2] = nearC[2] + vd[2] * s_focusDist;
            // Override light-space center with the focus projection.
            cxL = rx*focusWorld[0] + ry*focusWorld[1] + rz*focusWorld[2];
            cyL = ux*focusWorld[0] + uy*focusWorld[1] + uz*focusWorld[2];
            focusDistUsed = s_focusDist;
            focusApplied = true;
        }
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
            return (v && v[0] == '1');
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
                        "focusDist=%.0f nearSpread=%.0f farSpread=%.0f\n",
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

    // Compute sun screen position for god rays (project sun direction through VP matrix)
    float sunWorld[4] = { fx * 100000.0f, fy * 100000.0f, fz * 100000.0f, 1.0f };
    float clip[4] = {0, 0, 0, 0};
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            clip[r] += viewProj_[c * 4 + r] * sunWorld[c];
    if (clip[3] > 0.0f) {
        sunScreenPos_[0] = (clip[0] / clip[3]) * 0.5f + 0.5f;
        sunScreenPos_[1] = (clip[1] / clip[3]) * 0.5f + 0.5f;
    }
}
