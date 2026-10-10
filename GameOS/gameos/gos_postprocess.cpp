// gosPostProcess core: the draw-buffer policy, the singleton and its exposure/SSAO/sky setters, construction, init,
// the fullscreen quad, beginScene and endScene. The other passes live in gos_postprocess_{gates,diag,fbo,effects,region,
// skybox,shadow,executor}.cpp; the names they share are declared in gos_postprocess_internal.h.


#include "gos_postprocess_internal.h"



void setSceneDrawBuffers(SceneDrawBufferMode mode,
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

gosPostProcess* s_postProcess = nullptr;

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
float gos_GetSsaoRadius()   { return s_postProcess ? s_postProcess->aoRadius_   : 24.0f; }
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

    MC2_DIAGFE(stderr,
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
        MC2_DIAGFE(stderr,
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
    // SSAO-BLUR-1: was 3.0, which is structurally dead. One map cell is 42.67 wu
    // and the baked kernel's mean |v| is 0.289, so radius 3 put the mean tap
    // 0.87 wu from the centre -- sub-pixel, every tap reading the same depth
    // texel -- while rangeCheck = smoothstep(0,1, aoRadius/dist) independently
    // drove any occluder past 3 wu to zero weight. Measured result: 0.0000% of
    // AO-buffer pixels below 250/255. 24 follows SSAO-RETUNE-1's 12-24 band
    // (40 measurably invents occlusion on flat faces) and matches the value the
    // separate .claude/vk-post-ssao prior art independently landed on.
    // MC2_SSAO is default-OFF, so this changes no default launch.
    , aoRadius_(24.0f)
    , aoStrength_(0.7f)
    , aoBias_(0.0025f)
    , aoPower_(1.5f)
    // SSAO-BLUR-1: blur on by default whenever SSAO runs (the un-blurred chain
    // is not a shippable configuration -- see shaders/ssao_blur.frag header).
    , ssaoBlurEnabled_(true)
    // SSAO-BLUR-1: 0.10 chosen by measured sweep (mc2_01, AO buffer, half-res
    // HF-noise metric, vs blur OFF on cliff_wall_sideon -- the pose that
    // actually carries noise): 0.02 -> -52.5%, 0.10 -> -68.6%, 0.40 -> -69.3%.
    // 0.10 takes essentially all the available denoise while keeping a real
    // edge-stop; 0.40 buys +0.7pp more and bleeds more occlusion outward
    // (%<250 21.98 -> 22.04 vs 19.39 with blur off).
    , aoBlurDepthSigma_(0.10f)
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
        const char* hzbEnv = mc2::gate::raw<"MC2_HZB_BUILD">();   // echoed as typed
        hzbEnabled_ = mc2::gate::get<"MC2_HZB_BUILD">();
        MC2_DIAGFE(stderr, "[HZB_BUILD v1] enabled=%d (MC2_HZB_BUILD=%s)\n",
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
        hdriEnabled_ = mc2::gate::get<"MC2_HDRI_SKY">();

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
                MC2_DIAGFE(stderr,
                    "[HDRI_SKY v1] enabled=0 reason=init_failed "
                    "tex=%u prog=%p valid=%d\n",
                    hdriTex_, (void*)hdriSkyboxProg_,
                    hdriSkyboxProg_ ? (int)hdriSkyboxProg_->is_valid() : 0);
            }
        } else {
            MC2_DIAGFE(stderr,
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
    // SHADOW-DIST-FADE-1: object/prop receivers get the same distance-scaled
    // dynamic-shadow penumbra + fade as the terrain paths (lockstep).
    if (mc2ShadowDistFadeWu() > 0.0f) {
        char fadeDef[240];
        snprintf(fadeDef, sizeof(fadeDef),
                 "#define MC2_SHADOW_DIST_FADE_WU %.1f\n"
                 "#define MC2_SHADOW_DIST_FADE_MAX_F %.2f\n"
                 "#define MC2_SHADOW_FADE_CONT_F %d\n"
                 "#define MC2_SHADOW_FADE_RAMP0_F %d\n"
                 "#define MC2_SHADOW_ALT_SCALE_F %d\n",
                 mc2ShadowDistFadeWu(), mc2ShadowDistFadeMax(),
                     mc2ShadowFadeCont() ? 1 : 0,
                     mc2ShadowFadeRamp0() ? 1 : 0,
                     mc2ShadowAltScale() ? 1 : 0);
        screenShadowPrefix += fadeDef;
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
    // SSAO-BLUR-1: depth-aware bilateral blur (run separably H then V).
    ssaoBlurProg_ = glsl_program::makeProgram("ssao_blur",
        "shaders/postprocess.vert", "shaders/ssao_blur.frag", kShaderPrefix);
    if (!ssaoBlurProg_ || !ssaoBlurProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile ssao_blur shader\n");

    // BT2018-BOX-DECAL-1: screen-space box-decal program. Its OWN vert (cube volume),
    // not postprocess.vert. Default-OFF gate resolved once from env (SSAO style).
    boxDecalProg_ = glsl_program::makeProgram("box_decal",
        "shaders/box_decal.vert", "shaders/box_decal.frag", kShaderPrefix);
    if (!boxDecalProg_ || !boxDecalProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile box_decal shader\n");
    {
        const char* e = mc2::gate::raw<"MC2_BOX_DECAL">();   // echoed as typed
        boxDecalEnabled_ = mc2::gate::get<"MC2_BOX_DECAL">();
        MC2_DIAGFE(stderr, "[BOX_DECAL v1] enabled=%d (MC2_BOX_DECAL=%s)\n",
                     boxDecalEnabled_ ? 1 : 0, e ? e : "(unset)");
        // DECAL-INTEGRATE-1: MC2_PROJECTED_DECALS (default OFF). When ON, the box-decal
        // pass is fed by the dynamic_decal_ring impact slots instead of the v1 test box.
        const char* pd = mc2::gate::raw<"MC2_PROJECTED_DECALS">();   // echoed as typed
        projectedDecalsEnabled_ = mc2::gate::get<"MC2_PROJECTED_DECALS">();
        std::fprintf(stderr, "[PROJECTED_DECALS] enabled=%d (MC2_PROJECTED_DECALS=%s)\n",
                     projectedDecalsEnabled_ ? 1 : 0, pd ? pd : "(unset)");
    }

    // SSAO gate + debug, resolved once from env. Default OFF -> runSSAO()
    // skipped entirely (byte-identical). aoRadius/strength/bias keep their
    // member defaults (ImGui + per-mission profile adjustable).
    {
        const char* ssaoEnv = mc2::gate::raw<"MC2_SSAO">();   // echoed as typed
        ssaoEnabled_ = mc2::gate::get<"MC2_SSAO">();
        ssaoDebug_ = mc2::gate::get<"MC2_SSAO_DEBUG">();
        // SSAO-BLUR-1: blur defaults ON with SSAO; MC2_SSAO_BLUR=0 restores the
        // pre-SSAO-BLUR-1 two-draw chain (no denoise) for A/B on one exe.
        ssaoBlurEnabled_ = mc2::gate::get<"MC2_SSAO_BLUR">();
        // SSAO-BLUR-1: sigma MUST be env-tunable. Measured 2026-07-28: at the
        // initial 0.02 (2% relative depth) the blur was strongly effective on
        // flat near-constant-depth faces (cliff_wall_sideon, HF noise -52.5%)
        // but COMPLETELY INERT on oblique/grazing views (cross_map_view,
        // oblique_fog, oblique_horizon: HF delta ~0.0%, maxAbs 1-4). On a
        // grazing surface adjacent half-res texels differ in depth by far more
        // than 2%, so every neighbour tap was rejected and the result collapsed
        // to the centre pixel -- a blur that silently does nothing exactly where
        // the AO buffer is noisiest.
        // VERIFY-DEBT-1 (SSAO-RADIUS-REVERT-1). SSAO-BLUR-1 changed TWO coupled
        // things: it added the denoise pass AND raised aoRadius 3.0 -> 24.0. Only
        // the first had a killswitch. That left MC2_SSAO_BLUR=0 claiming to
        // "restore the pre-SSAO-BLUR-1 two-draw chain" while silently keeping the
        // 8x radius -- i.e. radius-24-without-denoise, a combination that has
        // never shipped and is exactly the speckle-on-flat-faces failure
        // SSAO-RETUNE-1 documented. The radius needed its own revert.
        //
        // Why the radius is NOT simply slaved to the blur gate: the stated
        // purpose of MC2_SSAO_BLUR=0 is to A/B the BLUR on one exe. Flipping the
        // radius with it makes that a two-variable change and stops being a blur
        // A/B at all. Worse, 3.0 is STRUCTURALLY DEAD -- one map cell is 42.67 wu
        // and the baked kernel's mean |v| is 0.289, so radius 3 puts the mean tap
        // 0.87 wu out (sub-pixel) while rangeCheck zeroes any occluder past 3 wu;
        // measured occlusion is identically 0.0000%. Auto-reverting to 3.0 would
        // therefore silently turn AO OFF for anyone who set MC2_SSAO_BLUR=0,
        // which is a worse failure than the one being fixed.
        //
        // So: an explicit, independent revert (MC2_SSAO_RADIUS, any float in the
        // gos_SetSsaoRadius clamp range 0.1..64), plus a loud diagnostic when the
        // known-bad pairing is actually selected. The radius keeps its own
        // runtime reverts too (ImGui, and the per-mission visual-tuning-profile
        // `aoRadius` key) -- this adds the launch-time one those cannot give.
        const char* radEnv = mc2::gate::raw<"MC2_SSAO_RADIUS">();   // echoed as typed
        bool radiusOverridden = false;
        if (mc2::gate::isNonEmpty<"MC2_SSAO_RADIUS">()) {
            aoRadius_ = mc2::gate::get<"MC2_SSAO_RADIUS">();   // row clamps to [0.1, 64]
            radiusOverridden = true;
        }
        if (mc2::gate::isNonEmpty<"MC2_SSAO_BLUR_SIGMA">())
            aoBlurDepthSigma_ = mc2::gate::get<"MC2_SSAO_BLUR_SIGMA">();   // row clamps to [0.001, 1]
        MC2_DIAGFE(stderr, "[SSAO v1] enabled=%d debug=%d (MC2_SSAO=%s) radius=%.2f (MC2_SSAO_RADIUS=%s) strength=%.2f bias=%.4f blur=%d sigma=%.4f\n",
                     ssaoEnabled_ ? 1 : 0, ssaoDebug_,
                     ssaoEnv ? ssaoEnv : "(unset)", aoRadius_,
                     radEnv && radEnv[0] ? radEnv : "(unset)",
                     aoStrength_, aoBias_,
                     ssaoBlurEnabled_ ? 1 : 0, aoBlurDepthSigma_);
        // The one combination that is known-bad rather than merely untuned.
        if (ssaoEnabled_ && !ssaoBlurEnabled_ && !radiusOverridden && aoRadius_ > 3.0f) {
            std::fprintf(stderr,
                "[SSAO v1] WARNING: MC2_SSAO_BLUR=0 with the SSAO-BLUR-1 retuned "
                "aoRadius=%.2f. This pairing NEVER shipped: the 16-sample "
                "stochastic estimator's noise scales with radius and nothing is "
                "denoising it, so expect speckle on flat unoccluded faces. This is "
                "NOT the pre-SSAO-BLUR-1 chain -- that also had aoRadius=3.0. For "
                "the historical chain set MC2_SSAO_RADIUS=3 as well (note it "
                "measures 0.0000%% occlusion, i.e. AO effectively off); to A/B the "
                "blur alone, keep this radius and ignore this warning.\n",
                aoRadius_);
        }
    }

    // POST-FX-FXAA-1: FXAA gate + tunables, resolved once from env. Default OFF
    // -> composite uploads u_fxaaEnabled=0 -> single tap (byte-identical).
    // Defaults = "sharper" preset (subpix 0.25 / edge 0.166 / edgeMin 0.0833).
    {
        const char* fxEnv = mc2::gate::raw<"MC2_FXAA">();   // echoed as typed
        fxaaEnabled_         = mc2::gate::get<"MC2_FXAA">();
        fxaaSubpix_          = 0.25f;
        fxaaEdgeThreshold_   = 0.166f;
        fxaaEdgeThresholdMin_= 0.0833f;
        if (mc2::gate::isSet<"MC2_FXAA_SUBPIX">())            { const float v = mc2::gate::get<"MC2_FXAA_SUBPIX">(); if (v >= 0.0f) fxaaSubpix_ = v; }
        if (mc2::gate::isSet<"MC2_FXAA_EDGE_THRESHOLD">())    { const float v = mc2::gate::get<"MC2_FXAA_EDGE_THRESHOLD">(); if (v >  0.0f) fxaaEdgeThreshold_ = v; }
        if (mc2::gate::isSet<"MC2_FXAA_EDGE_THRESHOLD_MIN">()){ const float v = mc2::gate::get<"MC2_FXAA_EDGE_THRESHOLD_MIN">(); if (v >  0.0f) fxaaEdgeThresholdMin_ = v; }
        MC2_DIAGFE(stderr, "[FXAA v1] enabled=%d subpix=%.3f edge=%.3f edgeMin=%.4f (MC2_FXAA=%s)\n",
                     fxaaEnabled_ ? 1 : 0, fxaaSubpix_, fxaaEdgeThreshold_, fxaaEdgeThresholdMin_,
                     fxEnv ? fxEnv : "(unset)");
    }

    // POST-FX-CAS-1: AMD FidelityFX Contrast Adaptive Sharpening gate + sharpness,
    // resolved once from env. Default OFF -> composite uploads u_casEnabled=0 ->
    // CAS skipped in the shader (byte-identical). Sharpness clamped 0..1, default
    // 0.5. Applied to the final graded LDR color, after FXAA/grade, before UI.
    {
        const char* casEnv = mc2::gate::raw<"MC2_POST_CAS">();   // echoed as typed
        casEnabled_   = mc2::gate::get<"MC2_POST_CAS">();
        casSharpness_ = 0.5f;
        if (mc2::gate::isSet<"MC2_POST_CAS_SHARPNESS">())
            casSharpness_ = mc2::gate::get<"MC2_POST_CAS_SHARPNESS">();   // row clamps to [0, 1]
        MC2_DIAGFE(stderr, "[CAS v1] enabled=%d sharpness=%.3f (MC2_POST_CAS=%s)\n",
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
        if (mc2::gate::isSet<"MC2_TONEMAP_V2">()) {
            const std::string text = mc2::gate::str<"MC2_TONEMAP_V2">();   // "" when empty: no mode matches
            const char* v = text.c_str();
            if      (v[0]=='1' || !strcmp(v,"agx"))      tonemapMode_ = 1;
            else if (v[0]=='2' || !strcmp(v,"hable"))    tonemapMode_ = 2;
            else if (v[0]=='3' || !strcmp(v,"reinhard")) tonemapMode_ = 3;
        }
        MC2_DIAGFE(stderr, "[TONEMAP v2] mode=%d (MC2_TONEMAP_V2=%s)\n",
                     tonemapMode_, mc2::gate::raw<"MC2_TONEMAP_V2">() ? mc2::gate::raw<"MC2_TONEMAP_V2">() : "(unset)");
    }

    // OOB-FOG-1: far-plane fog over the out-of-bounds region. Default ON.
    {
        fogOobProg_ = glsl_program::makeProgram("fog_oob",
            "shaders/postprocess.vert", "shaders/fog_oob.frag", kShaderPrefix);
        if (!fogOobProg_ || !fogOobProg_->is_valid())
            std::fprintf(stderr, "gosPostProcess: failed to compile fog_oob shader\n");
        fogOobEnabled_ = mc2::gate::get<"MC2_OOB_FOG">()
                      && fogOobProg_ && fogOobProg_->is_valid();
        if (mc2::gate::isSet<"MC2_OOB_FOG_COLOR">()) {
            const std::string colorEnv = mc2::gate::str<"MC2_OOB_FOG_COLOR">();
            float r, g, b;
            if (std::sscanf(colorEnv.c_str(), "%f,%f,%f", &r, &g, &b) == 3) {
                oobFogColor_[0] = r; oobFogColor_[1] = g; oobFogColor_[2] = b;
            }
        }
        MC2_DIAGFE(stderr, "[OOB_FOG v1] enabled=%d color=(%.2f,%.2f,%.2f)\n",
            fogOobEnabled_ ? 1 : 0, oobFogColor_[0], oobFogColor_[1], oobFogColor_[2]);
    }

    // EDGE-FOG-1: world-space map-edge fog on geometry pixels. Default ON.
    {
        edgeFogProg_ = glsl_program::makeProgram("edge_fog",
            "shaders/postprocess.vert", "shaders/edge_fog.frag", kShaderPrefix);
        if (!edgeFogProg_ || !edgeFogProg_->is_valid())
            std::fprintf(stderr, "gosPostProcess: failed to compile edge_fog shader\n");
        edgeFogEnabled_ = mc2::gate::get<"MC2_EDGE_FOG">()
                       && edgeFogProg_ && edgeFogProg_->is_valid();
        if (mc2::gate::isSet<"MC2_EDGE_FOG_START">())  edgeFogStart_  = mc2::gate::get<"MC2_EDGE_FOG_START">();
        if (mc2::gate::isSet<"MC2_EDGE_FOG_HEIGHT">()) edgeFogHeight_ = mc2::gate::get<"MC2_EDGE_FOG_HEIGHT">();
        if (mc2::gate::isSet<"MC2_EDGE_FOG_MAX">())    edgeFogMax_    = mc2::gate::get<"MC2_EDGE_FOG_MAX">();
        MC2_DIAGFE(stderr, "[EDGE_FOG v3] enabled=%d start=%.0f height=%.0f max=%.2f\n",
            edgeFogEnabled_ ? 1 : 0, edgeFogStart_, edgeFogHeight_, edgeFogMax_);
    }

    // BACKDROP-BLIT-ATTRIB-1: night-dim gate for the horizon cloud bank. RenderDoc
    // pixel-history on the pre-composite HDR scene FBO pinned the mc2_10 night
    // "white horizon band" to this edge-fog pass painting the hardcoded bright grey
    // edgeFogColor_ (never night-dimmed). Default-OFF -> unscaled -> byte-identical.
    {
        nightFogDimEnabled_ = mc2::gate::get<"MC2_NIGHT_FOG_DIM">();
        if (mc2::gate::isNonEmpty<"MC2_NIGHT_FOG_DIM_SCALE">()) {
            float s = mc2::gate::get<"MC2_NIGHT_FOG_DIM_SCALE">();
            if (s < 0.0f) s = 0.0f;
            nightFogDimScale_ = s;
        }
        std::fprintf(stderr, "[NIGHT_FOG_DIM] enabled=%d scale=%.3f\n",
            nightFogDimEnabled_ ? 1 : 0, nightFogDimScale_);
    }

    // FOG-HORIZON-CLAMP-1: elevation profile shared by OOB + edge fog. Rides the
    // existing (default-ON) fog gates -- this is a tuning of the fog's SHAPE, not
    // a new feature. Kill-switch MC2_FOG_HORIZON_CLAMP=0 restores the previous
    // worldDir.z 0.22 exclusion band. Knobs are in DEGREES of elevation above the
    // horizon; we precompute their sines here so the frag stays trig-free
    // (elevSin = -worldDir.z for a normalized view ray).
    {
        fogHorizonClampEnabled_ = mc2::gate::get<"MC2_FOG_HORIZON_CLAMP">();   // row default ON, like the member
        if (mc2::gate::isSet<"MC2_FOG_HORIZON_FADE_START">())
            fogHorizonFadeStartDeg_ = mc2::gate::get<"MC2_FOG_HORIZON_FADE_START">();
        if (mc2::gate::isSet<"MC2_FOG_HORIZON_FADE_END">())
            fogHorizonFadeEndDeg_   = mc2::gate::get<"MC2_FOG_HORIZON_FADE_END">();
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
        MC2_DIAGFE(stderr,
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
        const char* probeEnv = mc2::gate::raw<"MC2_HZB_PROBE">();   // echoed as typed
        hzbProbeEnabled_ = hzbEnabled_ && mc2::gate::get<"MC2_HZB_PROBE">();
        MC2_DIAGFE(stderr, "[HZB_PROBE v1] enabled=%d (MC2_HZB_PROBE=%s, requires MC2_HZB_BUILD)\n",
                     hzbProbeEnabled_ ? 1 : 0, probeEnv ? probeEnv : "(unset)");
    }

    // SHADOW-ENV-DEBUG-MODE-1: select shadow debug overlay from env var so
    // automated capture can request it without ImGui interaction.
    // Default unset/0/off -> showShadowDebug_ stays false (byte-identical).
    // RAlt+F2 hotkey and ImGui checkbox still override at runtime.
    {
        const std::string sdText = mc2::gate::str<"MC2_SHADOW_DEBUG_MODE">();   // "" when unset or empty
        const char* sdEnv = sdText.c_str();
        if (sdEnv[0]) {
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
        const char* vmEnv = mc2::gate::raw<"MC2_VIEWMODE_FRAMEWORK">();   // echoed as typed
        s_viewmodeFrameworkEnabled = mc2::gate::get<"MC2_VIEWMODE_FRAMEWORK">();

        // MC2_VIEW_MODE seeds the startup mode (numeric 0..5 matching
        // RenderCore::ViewMode, or a name) so headless capture can request a
        // mode without ImGui. Only meaningful when the framework gate is ON;
        // the ImGui combo overrides it live.
        const char* modeEnvRaw = mc2::gate::raw<"MC2_VIEW_MODE">();   // echoed as typed
        const std::string modeText = mc2::gate::str<"MC2_VIEW_MODE">();   // "" when unset or empty
        const char* modeEnv = modeText.c_str();
        int initialMode = 0;
        if (modeEnv[0]) {
            if (modeEnv[0] >= '0' && modeEnv[0] <= '9') initialMode = atoi(modeEnv);
            else if (!strcmp(modeEnv, "visual"))   initialMode = 0;
            else if (!strcmp(modeEnv, "objectid")) initialMode = 1;
            else if (!strcmp(modeEnv, "tactical")) initialMode = 2;
            else if (!strcmp(modeEnv, "thermal"))  initialMode = 3;
            else if (!strcmp(modeEnv, "infrared")) initialMode = 4;
            else if (!strcmp(modeEnv, "lowlight")) initialMode = 5;
        }
        gos_SetSelectedViewMode(initialMode);
        MC2_DIAGFE(stderr, "[VIEWMODE v1] framework=%d initialMode=%d (MC2_VIEWMODE_FRAMEWORK=%s MC2_VIEW_MODE=%s)\n",
                     s_viewmodeFrameworkEnabled ? 1 : 0, s_selectedViewMode,
                     vmEnv ? vmEnv : "(unset)", modeEnvRaw ? modeEnvRaw : "(unset)");

        // LOWLIGHT-NIGHTVISION-MVP-1: seed night-vision tunables from env.
        // Inert unless the selected ViewMode is LowLight (5). Direct member
        // writes — s_postProcess is not yet assigned during init(), so the
        // gos_SetLowLight* setters would no-op here.
        if (mc2::gate::isNonEmpty<"MC2_VIEWMODE_LOWLIGHT_GAIN">())
            lowLightGain_ = mc2::gate::get<"MC2_VIEWMODE_LOWLIGHT_GAIN">();   // row clamps to [0.1, 16]
        const std::string tintEnv = mc2::gate::str<"MC2_VIEWMODE_LOWLIGHT_TINT">();   // "" when unset or empty
        if (!tintEnv.empty())
            sscanf(tintEnv.c_str(), "%f,%f,%f", &lowLightTint_[0], &lowLightTint_[1], &lowLightTint_[2]);
        MC2_DIAGFE(stderr, "[VIEWMODE_LOWLIGHT v1] gain=%.2f tint=(%.2f,%.2f,%.2f)\n",
                     lowLightGain_, lowLightTint_[0], lowLightTint_[1], lowLightTint_[2]);
    }

    initShadows();
    initDynamicShadows();

    s_postProcess = this;
    initialized_ = true;
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
    lostFrameBegin();
    if (sceneGlProducerQueryEnabled() && sceneGlProducerQuery_ == 0)
        glGenQueries(1, &sceneGlProducerQuery_);
    if (sceneGlProducerQuery_ != 0 && !sceneGlProducerQueryActive_) {
        glBeginQuery(GL_ANY_SAMPLES_PASSED, sceneGlProducerQuery_);
        sceneGlProducerQueryActive_ = true;
    }

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
    static const bool s_resDiag = mc2::gate::get<"MC2_RES_DIAG">();
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

// Forward declarations for ISLAND-2 sub-stage wrappers (defined below the executor section).
void executorOwnBeginSub(gosPostProcess*, RenderCore::framegraph::ExecutorIslandId);
void executorOwnEndSub(gosPostProcess*, RenderCore::framegraph::ExecutorIslandId);

void gosPostProcess::endScene()
{
    ZoneScopedN("Render.PostProcess");
    TracyGpuZone("Render.PostProcess");

    if (!initialized_)
        return;

    lostFrameEnd();
    if (sceneGlProducerQueryActive_) {
        glEndQuery(GL_ANY_SAMPLES_PASSED);
        GLuint anySamples = 0;
        glGetQueryObjectuiv(sceneGlProducerQuery_, GL_QUERY_RESULT, &anySamples);
        std::fprintf(stderr,
            "[VK_SCENE_GL_PRODUCER_QUERY v1] any_samples=%u frame_sceneFBO=%u\n",
            (unsigned)anySamples, (unsigned)sceneFBO_);
        sceneGlProducerQueryActive_ = false;
    }

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
    // VK-POST-HZB-NATIVE-1 (rung 8.4): highest precedence at the HZB site.
    // When MC2_VK_POST_HZB=1, the shared-engine Vulkan path tries first; on any
    // decline/failure it falls straight back to the unchanged GL runHzbReduce()
    // body so the HZB probe stays coherent. Only when the native gate is OFF do
    // we consider the older region-iface route (VK-REGION-HZB-1) or direct GL.
    // Precedence: native > region-iface > GL.
#ifdef MC2_VULKAN_ISLAND
    if (vkPostHzbNativeEnabled()) {
        if (!runVkPostHzbNative())
            runHzbReduce();
    } else
#endif
    if (regionIfaceEnabled()) {
        runHzbRegionThroughBackend();
    } else {
        runHzbReduce();
    }
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

    // VK-POST-SCREENFX-NATIVE-1 (rung 8.3): HIGHEST precedence at the cloud +
    // shoreline sites. When MC2_VK_POST_SCREENFX=1 and the shared-GBuffer
    // substrate is live, the armed subset of {cloud-shadow, shoreline} renders
    // natively on the shared engine context (one submit, multiply into shared
    // color0 in place) and EXACTLY those GL pass bodies are skipped (bitmask:
    // bit0=cloud, bit1=shoreline). Any not-ready condition returns 0 -> the
    // unchanged GL chain below runs (fail-soft). Screen-shadow above is NOT
    // ported in this slice (no zero-copy CSM-array producer until rung 2).
    unsigned vkScreenFxDone = 0;
#ifdef MC2_VULKAN_ISLAND
    if (vkPostScreenFxNativeEnabled())
        vkScreenFxDone = runVkPostScreenFxNative();
#endif

    // Procedural cloud shadows: single fullscreen multiplicative pass over all
    // non-sky pixels (replaces the four inline cloud blocks). After the screen
    // shadow so cloud darkens the already-shadowed scene color.
    // FRAME-GRAPH-EXECUTOR-ISLAND-3: validate->call-unchanged->validate (default-OFF).
    if (!(vkScreenFxDone & 1u)) {
        executorOwnBeginSub(this, RenderCore::framegraph::ExecutorIslandId::CloudShadow);
        runCloudShadow();
        executorOwnEndSub(this, RenderCore::framegraph::ExecutorIslandId::CloudShadow);
    }

    // Shoreline foam pass (brightens water pixels adjacent to terrain)
    // FRAME-GRAPH-EXECUTOR-ISLAND-3: validate->call-unchanged->validate (default-OFF).
    if (!(vkScreenFxDone & 2u)) {
        executorOwnBeginSub(this, RenderCore::framegraph::ExecutorIslandId::Shoreline);
        runShoreline();
        executorOwnEndSub(this, RenderCore::framegraph::ExecutorIslandId::Shoreline);
    }

    // RENDER-BACKEND-REGION-IFACE-1 (Layer-6 ENTRY): master gate read ONCE, shared by
    // the SSAO region below and the fog region further down (hoisted here so both
    // sites can gate on the same value without re-reading getenv per-site).
    const bool regionRouted = regionIfaceEnabled();

    // SSAO grounding pass (multiplicative darkening into scene color). Default-OFF
    // (MC2_SSAO gates whether it runs at all, unchanged). VK-REGION-SSAO-1: when the
    // SAME master gate as fog (MC2_RENDER_BACKEND_REGION_IFACE) is ON, route through
    // IRenderBackend::runRegion (MC2_POSTPROCESS_BACKEND=gl|vk picks the impl, VK
    // falls back to GL on failure) instead of calling runSSAO() directly. Gate OFF ->
    // the original direct call runs verbatim (byte-identical).
    // VK-POST-SSAO-NATIVE-1 (rung 8.2): HIGHEST precedence at the SSAO site. When
    // MC2_VK_POST_SSAO=1 and the shared-GBuffer substrate is live, SSAO gen+apply
    // render natively on the shared engine context (zero copies, zero readbacks)
    // and the GL SSAO site is skipped. runVkPostSsaoNative() returns false on ANY
    // not-ready condition (incl. ssaoDebug_!=0) -> the unchanged region-iface/GL
    // chain below runs (fail-soft). Precedence: native > region-iface > GL.
#ifdef MC2_VULKAN_ISLAND
    if (vkPostSsaoNativeEnabled() && runVkPostSsaoNative()) {
        // native path did gen+apply; GL SSAO site skipped (no double-apply).
    } else
#endif
    if (regionRouted) {
        runSSAORegionThroughBackend();
    } else {
        runSSAO();
    }

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
    // (regionRouted computed once above, shared with the SSAO region site.)
    bool subgraphRanThisFrame = false;   // scope spans to the FogOob site below
    (void)subgraphRanThisFrame;          // read only under MC2_VULKAN_ISLAND
    if (regionRouted) {
        runFogRegionThroughBackend();   // does BOTH edge + oob (GL wrapper or VK subgraph)
    } else {
#ifdef MC2_VULKAN_ISLAND
    // VK-POST-FOG-NATIVE-1 (rung 8.1): HIGHEST precedence at the fog site. When
    // MC2_VK_POST_FOG_NATIVE=1 and the shared-GBuffer substrate is live, BOTH fog
    // effects render natively on the shared engine context (zero copies, zero
    // readbacks) and BOTH GL sites are skipped. runVkPostFogNative() returns false
    // on ANY not-ready condition -> the unchanged GL chain below runs (fail-soft).
    if (vkPostFogNativeEnabled() && runVkPostFogNative()) {
        subgraphRanThisFrame = true;    // -> OOB site is skipped below (no double-apply)
    } else
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
    // If native fog already ran this frame it ALSO did the oob fog, so this site
    // is skipped entirely (no GL, no island).
    if (subgraphRanThisFrame) {
        // native fog did edge+oob; nothing to do here.
    } else
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
    static const bool s_backendIface = mc2::gate::get<"MC2_RENDER_BACKEND_IFACE">();
    // VK-P14b (MC2_VK_PRESENT_DIRECT_FBO, copy-1 elimination): when the Vulkan
    // backend owns present AND the direct-composite gate is on, redirect this
    // single backbuffer bind to the FBO wrapping the shared exportable present
    // image, so the WHOLE composite + FORCE-43 pillarbox + HUD + ImGui render
    // straight into the shared image (presentGLFrame then SKIPS copy-1 and does
    // the Y-flip in copy-2). Everything downstream in endScene inherits this bind.
    // presentDirectFbo() returns 0 (fail-soft to FBO-0 + copy-1) unless the gate
    // is on AND the present path is valid AND the FBO exists; the ==0 case leaves
    // the original GL path byte-identical. NON-default FBO target: the shared FBO
    // is COLOR_ATTACHMENT0-complete (RGBA8, no default sRGB encoding) so the
    // inherited glClear / glReadBuffer(GL_BACK is only used inside copy-1, which is
    // now skipped) all target this attachment correctly (see FBO-0-assumption scan).
    GLuint directFbo = 0;
    bool directVkOnly = false;
#ifdef MC2_VULKAN
    if (mc2::renderBackend() == mc2::RenderBackend::Vulkan) {
        mc2vk::EnginePresentVk& present = mc2vk::getOrCreateEnginePresentVk();
        directFbo = (GLuint)present.presentDirectFbo();
        if (directFbo == 0)
            directVkOnly = present.presentDirectVkOnly();
    }
#endif
    if (directFbo != 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, directFbo);
    } else if (s_backendIface) {
        RenderCore::getGLBackend().bindBackbuffer();
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    int compositeVpX = 0, compositeVpY = 0, compositeVpW = width_, compositeVpH = height_;
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
        static const bool s_oobLetterbox = mc2::gate::get<"MC2_OOB_LETTERBOX">();
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
            compositeVpX = bx; compositeVpY = by; compositeVpW = bw; compositeVpH = bh;
        } else {
            // RENDER-BACKEND-IFACE-VIEWPORT-1: route the non-pillarbox composite viewport.
            if (s_backendIface) {
                RenderCore::getGLBackend().setViewport(0, 0, width_, height_);
            } else {
                glViewport(0, 0, width_, height_);
            }
            compositeVpX = 0; compositeVpY = 0; compositeVpW = width_; compositeVpH = height_;
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

    bool nativeCompositeDone = false;
    int effectiveModeForComposite = gos_GetSelectedViewMode();
    if (effectiveModeForComposite == 1 && sceneObjectIdTex_ == 0)
        effectiveModeForComposite = 0;
#ifdef MC2_VULKAN
    if ((directFbo != 0 || directVkOnly) && mc2::renderBackend() == mc2::RenderBackend::Vulkan &&
        (effectiveModeForComposite == 0 || effectiveModeForComposite == 1 ||
         effectiveModeForComposite == 3 || effectiveModeForComposite == 5)) {
        nativeCompositeDone = mc2vk::getOrCreateEnginePresentVk().renderNativeCompositeVisual(
            exposure_, fxaaEnabled_, fxaaSubpix_, fxaaEdgeThreshold_, fxaaEdgeThresholdMin_,
            effectiveModeForComposite,
            sceneObjectIdTex_ != 0 ? (int)RenderWorld::MechHandleIndexBase() : 0,
            lowLightGain_, lowLightTint_,
            compositeVpX, compositeVpY, compositeVpW, compositeVpH);
    }
#endif

    // Draw fullscreen quad with composite shader
    if (!nativeCompositeDone && compositeProg_ && compositeProg_->is_valid()) {
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
        int effectiveMode = effectiveModeForComposite;
        if (effectiveMode == 1 && sceneObjectIdTex_ == 0) {
            static bool s_warnedOidMissing = false;
            if (!s_warnedOidMissing) {
                MC2_DIAGFE(stderr,
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
