// gosPostProcess core: the draw-buffer policy, the singleton and its exposure/sky setters, construction, init,
// the fullscreen quad, beginScene and endScene. The other passes live in gos_postprocess_{gates,diag,fbo,effects,region,
// skybox,shadow,executor}.cpp; the names they share are declared in gos_postprocess_internal.h.


#include "gos_postprocess_internal.h"
#include "mc2fmt.h"



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
            mc2::fmt::print_err("[DRAWBUF] set=SingleColor0 attachments={{0}}\n");
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
        mc2::fmt::print_err("[DRAWBUF] set=MainSceneMRT attachments={{0,1{}}}\n", mc2::fmt::safe(oid ? ",2" : ""));
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

    float newSunAz = 0.0f;
    bool  newSunValid = false;
    loadHdri(newPath, &newSunAz, &newSunValid);   // deletes the old texture first

    if (hdriTex_) {
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

void gosPostProcess::loadHdri(const char* path, float* outSunAz, bool* outSunValid)
{
    if (hdriTex_) {
        glDeleteTextures(1, &hdriTex_);
        hdriTex_ = 0;
    }
    hdriCpu_.reset();
    auto cpu = std::make_shared<HdriCpuImage>();
    hdriTex_ = loadHdriTexture(path, outSunAz, outSunValid, cpu.get());
    if (hdriTex_ && cpu && !cpu->levels.empty()) {
        cpu->generation = ++hdriCpuGeneration_;
        hdriCpu_ = std::move(cpu);
    }
}

const HdriCpuImage* gos_GetHdriCpuImage()
{
    gosPostProcess* pp = getGosPostProcess();
    return pp ? pp->getHdriCpu() : nullptr;
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

// Player post-FX options (options.cfg): kept here so they survive until the post-processor is created.
static struct PostFxOptions {
    bool  casEnabled   = false;
    float casSharpness = 0.5f;
    int   tonemapMode  = 0;
} s_postFxOptions;

void gos_SetPostFxOptions(int casEnabled, float casSharpness, int tonemapMode)
{
    s_postFxOptions.casEnabled   = (casEnabled != 0);
    // A non-finite value passes every comparison below, so it is replaced first: the default keeps the sharpening mild instead of reaching the shader as NaN.
    if (!std::isfinite(casSharpness)) casSharpness = 0.5f;
    s_postFxOptions.casSharpness = casSharpness < 0.0f ? 0.0f : (casSharpness > 1.0f ? 1.0f : casSharpness);
    s_postFxOptions.tonemapMode  = (tonemapMode < 0 || tonemapMode > 3) ? 0 : tonemapMode;
    if (s_postProcess)
        s_postProcess->setPostFxOptions(s_postFxOptions.casEnabled, s_postFxOptions.casSharpness, s_postFxOptions.tonemapMode);
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
    , shadowMapSize_(4096)
    , shadowsEnabled_(true)
    , staticLightMatrixBuilt_(false)
    , mapHalfExtent_(0.0f)
    , dynShadowMapSize_(2048)
    , csmCount_(0)
    , csmDebugLayer_(0)
    , screenShadowEnabled_(true)
    , screenShadowDebug_(0)
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
{
    memset(staticLightSpaceMatrix_, 0, sizeof(staticLightSpaceMatrix_));
    memset(dynamicLightSpaceMatrix_, 0, sizeof(dynamicLightSpaceMatrix_));
    dynamicFullMapSize_ = 4096;
    dynamicFullMapTexelWorld_ = 0.0f;
    memset(dynamicCascadeMatrices_, 0, sizeof(dynamicCascadeMatrices_));
    memset(dynamicCascadeTexelWorld_, 0, sizeof(dynamicCascadeTexelWorld_));
    csmDepthSpan_ = 1.0f;
    memset(savedViewport_, 0, sizeof(savedViewport_));
    memset(inverseViewProj_, 0, sizeof(inverseViewProj_));
    memset(viewProj_, 0, sizeof(viewProj_));
    showShadowDebug_ = false;
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
        mc2::fmt::print_err("gosPostProcess: failed to compile postprocess shader\n");
    }

    skyboxProg_ = glsl_program::makeProgram("skybox",
        "shaders/skybox.vert", "shaders/skybox.frag", kShaderPrefix);
    if (!skyboxProg_ || !skyboxProg_->is_valid())
        mc2::fmt::print_err("gosPostProcess: failed to compile skybox shader\n");

    // HDRI-SKY-1 init. Gate read once; default enabled unless env var == "0".
    {
        hdriEnabled_ = mc2::gate::get<"MC2_HDRI_SKY">();

        if (hdriEnabled_) {
            // HDRI-SKY-NUMBER-1: default path (overridden at mission load via setSkyNumber).
            const char* hdrPath = "data/hdr/DaySkyHDRI063B_4K.exr";
            // Item 2: also scan for the baked sun azimuth (read-only; used only
            // when sun-sync is enabled at draw time).
            loadHdri(hdrPath, &hdriBakedSunAz_, &hdriBakedSunValid_);  // logs failures internally
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

    // POST-FX-CAS-1: AMD FidelityFX Contrast Adaptive Sharpening, from options.cfg
    // (PostCAS, PostCASSharpness). Off by default; sharpness 0..1, default 0.5. Applied
    // to the final graded LDR color, after FXAA/grade, before UI.
    {
        casEnabled_   = s_postFxOptions.casEnabled;
        casSharpness_ = s_postFxOptions.casSharpness;
        MC2_DIAGFE(stderr, "[CAS v1] enabled=%d sharpness=%.3f (options.cfg)\n",
                     casEnabled_ ? 1 : 0, casSharpness_);
    }

    // POST-FX-TONEMAP-V2 (SCOUR-1 #3): optional filmic tonemap curve on the final
    // composited colour, from options.cfg (TonemapMode): 0 off (default), 1 AgX,
    // 2 Hable, 3 Reinhard. Visual-only; no geometry/depth/object-id effect.
    {
        tonemapMode_ = s_postFxOptions.tonemapMode;
        MC2_DIAGFE(stderr, "[TONEMAP v2] mode=%d (options.cfg)\n", tonemapMode_);
    }

    // OOB-FOG-1: far-plane fog over the out-of-bounds region. Default ON.
    {
        fogOobEnabled_ = mc2::gate::get<"MC2_OOB_FOG">();
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
        edgeFogEnabled_ = mc2::gate::get<"MC2_EDGE_FOG">();
        if (mc2::gate::isSet<"MC2_EDGE_FOG_START">())  edgeFogStart_  = mc2::gate::get<"MC2_EDGE_FOG_START">();
        if (mc2::gate::isSet<"MC2_EDGE_FOG_HEIGHT">()) edgeFogHeight_ = mc2::gate::get<"MC2_EDGE_FOG_HEIGHT">();
        if (mc2::gate::isSet<"MC2_EDGE_FOG_MAX">())    edgeFogMax_    = mc2::gate::get<"MC2_EDGE_FOG_MAX">();
        MC2_DIAGFE(stderr, "[EDGE_FOG v3] enabled=%d start=%.0f height=%.0f max=%.2f\n",
            edgeFogEnabled_ ? 1 : 0, edgeFogStart_, edgeFogHeight_, edgeFogMax_);
    }

    // FOG-HORIZON-CLAMP-1: elevation profile shared by OOB + edge fog. Rides the
    // existing (default-ON) fog gates -- this is a tuning of the fog's SHAPE, not
    // a new feature. The band is in DEGREES of elevation above the horizon
    // (members fogHorizonFadeStartDeg_ / fogHorizonFadeEndDeg_); we precompute
    // their sines here so the frag stays trig-free (elevSin = -worldDir.z for a
    // normalized view ray).
    {
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
        mc2::fmt::print_err("gosPostProcess: failed to compile hzb_reduce shader\n");
    {
        // HZB-OCCLUSION-PROBE-1 gate (diagnostic only; requires the build gate).
        const char* probeEnv = mc2::gate::raw<"MC2_HZB_PROBE">();   // echoed as typed
        hzbProbeEnabled_ = hzbEnabled_ && mc2::gate::get<"MC2_HZB_PROBE">();
        MC2_DIAGFE(stderr, "[HZB_PROBE v1] enabled=%d (MC2_HZB_PROBE=%s, requires MC2_HZB_BUILD)\n",
                     hzbProbeEnabled_ ? 1 : 0, probeEnv ? probeEnv : "(unset)");
    }

    // SHADOW-ENV-DEBUG-MODE-1: select the shadow debug overlay (the Vulkan CSM near-cascade view)
    // from env var so automated capture can request it without ImGui interaction.
    // Default unset/0/off -> showShadowDebug_ stays false. RAlt+F2 hotkey and ImGui checkbox still override at runtime.
    {
        const std::string sdText = mc2::gate::str<"MC2_SHADOW_DEBUG_MODE">();   // "" when unset or empty
        const char* sdEnv = sdText.c_str();
        if (sdEnv[0]) {
            if (sdEnv[0] == '1' || sdEnv[0] == '2' || sdEnv[0] == 's' || sdEnv[0] == 'd') {
                showShadowDebug_ = true;
                mc2::fmt::print_err("[SHADOW_DEBUG] MC2_SHADOW_DEBUG_MODE on (CSM view)\n");
            } else {
                mc2::fmt::print_err("[SHADOW_DEBUG] MC2_SHADOW_DEBUG_MODE={} (unrecognized, using OFF)\n", mc2::fmt::safe(sdEnv));
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

        // LOWLIGHT-NIGHTVISION-MVP-1: the night-vision gain and tint keep their
        // member defaults; the ImGui setters change them at runtime.
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
        mc2::fmt::print_err("[RES_DIAG v1] sceneFBO={:d}x{:d}  screenWidth={:d}x{:d} (HUD canvas)  drawable={:d}x{:d} (native)\n", mc2::fmt::as_i32(width_), mc2::fmt::as_i32(height_), mc2::fmt::as_i32(Environment.screenWidth), mc2::fmt::as_i32(Environment.screenHeight), mc2::fmt::as_i32(Environment.drawableWidth), mc2::fmt::as_i32(Environment.drawableHeight));
        fflush(stderr);
    }
}

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
    {
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

#ifdef MC2_VULKAN_ISLAND
    // VK-POST-SCREENFX-NATIVE-1: the screen-space shadow receiver, the cloud shadow and the shoreline foam render natively on the shared engine
    // context (one submit each, multiplied into the shared colour in place); the GL passes are gone, a decline leaves the pass out for the frame.
    if (vkPostScreenFxNativeEnabled()) {
        runVkPostScreenShadowStaticNative();
        runVkPostScreenFxNative();
    }
#endif


#ifdef MC2_VULKAN_ISLAND
    // VK-POST-FOG-NATIVE-1: edge fog and OOB fog render natively on the shared engine context (the GL passes are gone; a decline leaves the
    // fog out for the frame).
    runVkPostFogNative();
#endif

    // Bind default framebuffer (the backbuffer) for the composite output edge.
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
    {
        mc2vk::EnginePresentVk& present = mc2vk::getOrCreateEnginePresentVk();
        directFbo = (GLuint)present.presentDirectFbo();
        if (directFbo == 0)
            directVkOnly = present.presentDirectVkOnly();
    }
#endif
    if (directFbo != 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, directFbo);
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
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            if (prevScissor) glEnable(GL_SCISSOR_TEST);
        }
        int bx, by, bw, bh;
        if (gos_Compute43Box(width_, height_, &bx, &by, &bw, &bh)) {
            glViewport(0, 0, width_, height_);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glViewport(bx, by, bw, bh);
            compositeVpX = bx; compositeVpY = by; compositeVpW = bw; compositeVpH = bh;
        } else {
            glViewport(0, 0, width_, height_);
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
    if ((directFbo != 0 || directVkOnly) &&
        (effectiveModeForComposite == 0 || effectiveModeForComposite == 1 ||
         effectiveModeForComposite == 3 || effectiveModeForComposite == 5)) {
        nativeCompositeDone = mc2vk::getOrCreateEnginePresentVk().renderNativeCompositeVisual(
            exposure_, fxaaEnabled_, fxaaSubpix_, fxaaEdgeThreshold_, fxaaEdgeThresholdMin_,
            effectiveModeForComposite,
            sceneObjectIdTex_ != 0 ? (int)RenderWorld::MechHandleIndexBase() : 0,
            lowLightGain_, lowLightTint_,
            casEnabled_, casSharpness_, tonemapMode_,
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


    // GLSTATE-VAO-RESTORE-1: restore the caller's VAO (see capture above).
    glBindVertexArray((GLuint)prevVAO);
}
