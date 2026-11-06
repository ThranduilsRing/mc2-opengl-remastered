// gosPostProcess core: the draw-buffer policy, the singleton and its exposure/sky setters, construction, init,
// the fullscreen quad, beginScene and endScene. The other passes live in gos_postprocess_{gates,diag,fbo,effects,region,
// skybox,shadow,executor}.cpp; the names they share are declared in gos_postprocess_internal.h.


#include "mc2_czstring.h"
#include "gos_postprocess_internal.h"
#include "mc2fmt.h"





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

    if (hdriCpu_) {
        hdriBakedSunAz_   = newSunAz;
        hdriBakedSunValid_ = newSunValid;
        std::strncpy(hdriCurrentPath_, newPath, sizeof(hdriCurrentPath_) - 1);
        hdriCurrentPath_[sizeof(hdriCurrentPath_) - 1] = '\0';
        hdriReady_ = true;
    } else {
        // Load failed — keep hdriReady_=false so sky renders black rather than crash.
        hdriReady_ = false;
        MC2_DIAGFE(stderr,
            "[HDRI_SKY v1] setSkyNumber sky=%d load_failed path=%s\n",
            skyNumber, newPath);
    }
}

void gosPostProcess::loadHdri(mc2::czstring path, float* outSunAz, bool* outSunValid)
{
    hdriCpu_.reset();
    auto cpu = std::make_shared<HdriCpuImage>();
    const bool decoded = decodeHdriSource(path, outSunAz, outSunValid, cpu.get());
    if (decoded && !cpu->levels.empty()) {
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

gosPostProcess::gosPostProcess()
    : exposure_(1.0f)
    , width_(0)
    , height_(0)
    , initialized_(false)
    , shadowsEnabled_(true)
    , mapHalfExtent_(0.0f)
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

    createSceneTargets(w, h);

    // HDRI-SKY-1 init. Gate read once; default enabled unless env var == "0".
    {
        hdriEnabled_ = mc2::gate::get<"MC2_HDRI_SKY">();

        if (hdriEnabled_) {
            // HDRI-SKY-NUMBER-1: default path (overridden at mission load via setSkyNumber).
            const char* hdrPath = "data/hdr/DaySkyHDRI063B_4K.exr";
            // Item 2: also scan for the baked sun azimuth (read-only; used only
            // when sun-sync is enabled at draw time).
            loadHdri(hdrPath, &hdriBakedSunAz_, &hdriBakedSunValid_);  // logs failures internally
            if (hdriCpu_) {
                std::strncpy(hdriCurrentPath_, hdrPath, sizeof(hdriCurrentPath_) - 1);
                hdriCurrentPath_[sizeof(hdriCurrentPath_) - 1] = '\0';
            }

            hdriReady_ = static_cast<bool>(hdriCpu_);

        } else {
            MC2_DIAGFE(stderr,
                "[HDRI_SKY v1] enabled=0 reason=env_gate MC2_HDRI_SKY=0\n");
        }
    }

    // POST-FX-CAS-1: AMD FidelityFX Contrast Adaptive Sharpening, from options.cfg
    // (PostCAS, PostCASSharpness). Off by default; sharpness 0..1, default 0.5. Applied
    // to the final graded LDR color, after grade, before spatial AA and UI.
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
        fogOobEnabled_ = true;
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

    // SHADOW-ENV-DEBUG-MODE-1: select the shadow debug overlay (the Vulkan CSM near-cascade view)
    // from env var so automated capture can request it without ImGui interaction.
    // Default unset/0/off -> showShadowDebug_ stays false. RAlt+F2 hotkey and ImGui checkbox still override at runtime.
    {
        const std::string sdText = mc2::gate::str<"MC2_SHADOW_DEBUG_MODE">();   // "" when unset or empty
        const char* sdEnv = sdText.c_str();
        if (sdEnv[0]) {
            if (sdEnv[0] == 'm') {
                // Harness toggle for the motion-vector debug inset (temporal track).
                showMotionVectors_ = true;
                mc2::fmt::print_err("[SHADOW_DEBUG] MC2_SHADOW_DEBUG_MODE=m: motion-vector inset on\n");
            } else if (sdEnv[0] == '1' || sdEnv[0] == '2' || sdEnv[0] == 's' || sdEnv[0] == 'd') {
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

    s_postProcess = this;
    initialized_ = true;
}





void gosPostProcess::beginScene()
{
    if (!initialized_) return;
    prevFrameHadTerrain_ = sceneHasTerrain_;
    sceneHasTerrain_ = false;
    // Scene clears belong to the native terrain prefix or host menu clear.

}

void gosPostProcess::endScene()
{
    ZoneScopedN("Render.PostProcess");
    if (!initialized_) return;
#ifdef MC2_VULKAN_ISLAND
    if (vkPostScreenFxNativeEnabled()) {
        runVkPostScreenShadowStaticNative();
        runVkPostScreenFxNative();
    }
    runVkPostFogNative();
#endif
    int x=0,y=0,w=outputWidth_,h=outputHeight_;
    int bx=0,by=0,bw=0,bh=0;
    if (gos_Compute43Box(outputWidth_,outputHeight_,&bx,&by,&bw,&bh)) { x=bx; y=by; w=bw; h=bh; }
    int mode=gos_GetSelectedViewMode();
    if (mode==1 && !sceneObjectIdReady()) mode=0;
#ifdef MC2_VULKAN
    auto& present=mc2vk::getOrCreateEnginePresentVk();
    present.setMotionVectorsDebug(showMotionVectors_);
    if (present.beginNativeComposite()) (void)present.renderNativeCompositeVisual(
        exposure_,mode,
        sceneObjectIdReady() ? static_cast<int>(RenderWorld::MechHandleIndexBase()) : 0,
        lowLightGain_,lowLightTint_,casEnabled_,casSharpness_,tonemapMode_,x,y,w,h);
#endif
    drawShadowDebugOverlay();
    gos_InvalidateRenderStateCache();
}

float mc2ShadowMinSunAltDeg()
{
    static const float s_minAltDeg = 15.0f;
    return s_minAltDeg;
}

void gosPostProcess::drawShadowDebugOverlay() {
    if (!showShadowDebug_ || !initialized_) return;
#if defined(MC2_VULKAN) && defined(MC2_VULKAN_ISLAND)
    if (mc2_vk_shadow_island_movers_ready())
        if (void* view=mc2_vk_shadow_island_get_mover_debug_view())
            mc2vk::getOrCreateEnginePresentVk().renderNativeCsmShadowDebugOverlay(view,0);
#endif
}
