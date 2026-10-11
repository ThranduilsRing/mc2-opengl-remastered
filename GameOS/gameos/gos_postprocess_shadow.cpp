// gosPostProcess: shadow debug overlay, static and cascaded shadow maps and their light matrices.
// Declared in the class headers; shared names in gos_postprocess_internal.h.
#include "gos_postprocess_internal.h"
#include "mc2fmt.h"


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
#if defined(MC2_VULKAN) && defined(MC2_VULKAN_ISLAND)
    if (csmDynamic && mc2::renderBackend() == mc2::RenderBackend::Vulkan &&
        mc2_vk_shadow_island_csm_array_ready()) {
        int layer = csmDebugLayer_;
        if (layer < 0) layer = 0;
        if (layer >= csmCount_) layer = csmCount_ - 1;
        void* rawView = mc2_vk_shadow_island_get_csm_array_view();
        if (rawView &&
            mc2vk::getOrCreateEnginePresentVk().renderNativeCsmShadowDebugOverlay(rawView, layer)) {
            return;
        }
    }
#endif
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
    // The static shadow map is always the plain-GL GL_DEPTH_COMPONENT24 allocation.
#ifdef MC2_VULKAN_ISLAND
    mc2VkStaticShadowMapDeprecationNote();
#endif
    if (!shadowDepthTex_) {
        glGenTextures(1, &shadowDepthTex_);
    }
    glBindTexture(GL_TEXTURE_2D, shadowDepthTex_);
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
            shadowMapSize_, shadowMapSize_, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    }
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
        mc2::fmt::print_err("gosPostProcess: shadow FBO incomplete\n");

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

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    // Polygon offset to reduce shadow acne
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(shadowBiasFactor_, shadowBiasUnits_);

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
// SHADOW-SUN-ALT-CLAMP-1 (2026-07-21, mc2_10 night streak-shadows): missions
// that author the sun AT (or below) the horizon (mc2_10 LightDirPitch=180 ->
// lightDirection.z = sin(180 deg) = 0) feed a near-HORIZONTAL light direction
// into the shadow projection matrices. Every caster (tree, fence, tower,
// terrain ridge) then smears an absurdly elongated silhouette across up-facing
// slopes — the user-reported "black streak fans"/giant tree silhouettes.
// Clamp the EFFECTIVE sun altitude used by the shadow matrices to a minimum
// (default 15 deg), preserving azimuth. Day suns above the clamp are
// untouched (byte-identical). Applies to BOTH the static full-map matrix and
// the dynamic cascade matrices (trees are static casters, movers dynamic —
// both degenerate identically at grazing altitude). Shading light dir
// (shadow_screen lightDir, terrain N-dot-L) is NOT touched.
// Tunable/killswitch: MC2_SHADOW_MIN_SUN_ALT=<degrees>, 0 disables.
// f = normalized light->scene direction: fz < 0 = descending (day sun).
// TERRAIN-SHADOW-GUARD-1: single source of truth for the effective minimum sun
// altitude. Resolved once at process start. The terrain shader family needs the
// SAME number at shader-compile time (it applies the identical clamp in GLSL for
// its back-face guard), so this accessor is exported rather than kept static.
float mc2ShadowMinSunAltDeg()
{
    static const float s_minAltDeg = mc2::gate::get<"MC2_SHADOW_MIN_SUN_ALT">();
    return s_minAltDeg;
}

void mc2ClampShadowSunAltitude(float& fx, float& fy, float& fz)
{
    const float s_minAltDeg = mc2ShadowMinSunAltDeg();
    if (s_minAltDeg <= 0.0f) return;
    const float minDown = sinf(s_minAltDeg * 0.01745329252f);
    const float down = -fz;              // >0 when pointing downward
    if (down >= minDown) return;         // healthy day sun: untouched
    const float horiz = sqrtf(fx * fx + fy * fy);
    const float ch = sqrtf(1.0f - minDown * minDown);
    if (horiz > 1e-6f) {
        fx = fx / horiz * ch;
        fy = fy / horiz * ch;
    } else {
        fx = ch; fy = 0.0f;              // degenerate straight-up/down input
    }
    fz = -minDown;
}

static void mc2ComputeLightBasis(float fx, float fy, float fz,
                                 float& rx, float& ry, float& rz,
                                 float& ux, float& uy, float& uz)
{
    static const bool s_robustGuard = mc2::gate::get<"MC2_SHADOW_ROBUST_BASIS">();

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
    mc2ClampShadowSunAltitude(fx, fy, fz);   // SHADOW-SUN-ALT-CLAMP-1

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

    mc2::fmt::print_err("gosPostProcess: rendering static shadows (map half-extent={:.0f})\n", mapHalfExtent);

    // [SHADOWFRUSTUM v1] VPL-#shadow: prove the static-shadow CLIPPER is
    // correct (user suspected light/clipper; render-expert grep says it's
    // feed-scope, not the matrix). INERT, env MC2_DEBUG_SHADOW_FRUSTUM,
    // one-shot (function early-returns on staticLightMatrixBuilt_ latch).
    // Input scalars only -- NO staticLightSpaceMatrix_[] slot reads (the
    // matrix-index discipline lesson). If orthoHalf == mapHalfExtent*1.485
    // and the map's real half-size, the clipper covers the FULL map ->
    // the bug is definitively the FEED (camera-windowed terrain), not the
    // light/clipper. buildCount>1 would mean the latch resets (rebuild).
    if (mc2::gate::get<"MC2_DEBUG_SHADOW_FRUSTUM">()) {
        static int s_buildCount = 0;
        ++s_buildCount;
        mc2::fmt::print_err("[SHADOWFRUSTUM v1] event=build n={:d} sunDirIn=({:.4f},{:.4f},{:.4f}) sunDirNorm=({:.4f},{:.4f},{:.4f}) mapHalfExtent={:.1f} orthoHalf(r)={:.1f} near={:.2f} far={:.2f} coversWorldXY=[-{:.1f},{:.1f}]\n", mc2::fmt::as_i32(s_buildCount), sunDirX, sunDirY, sunDirZ, fx, fy, fz, mapHalfExtent, r, nearP, farP, r, r);
    }

    // [SHADOWZRANGE v1] VPL-#10: prove the [0,1] ZERO_TO_ONE conversion is
    // correct. Transform map center + 4 corners (z=0) through the COMPOSITE
    // staticLightSpaceMatrix_ (column-major: [col*4+row]); every clip.z/clip.w
    // MUST land in [0,1]. A sign error in the ortho z-row shows here as an
    // out-of-range value BEFORE any GPU round-trip. Unconditional env-gated
    // fprintf (assert is a no-op under RelWithDebInfo); one-shot via the
    // staticLightMatrixBuilt_ latch.
    if (mc2::gate::get<"MC2_DEBUG_SHADOW_ZRANGE">()) {
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
            mc2::fmt::print_err("[SHADOWZRANGE v1] event=static pt={:d} world=({:.0f},{:.0f},{:.0f}) clipZ={:.5f} clipW={:.5f} ndcZ={:.5f} inRange={:d}\n", mc2::fmt::as_i32(i), px, py, pz, cz, cw, ndc, mc2::fmt::as_i32((ndc >= 0.0f && ndc <= 1.0f) ? 1 : 0));
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
        mc2::fmt::print_err("gosPostProcess: dynamic shadow FBO incomplete\n");

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

        glGenFramebuffers(1, &dynShadowArrayFBO_);
        RenderCore::framegraph::fboLedger().registerFbo(dynShadowArrayFBO_, RenderCore::RenderResourceId::ShadowDynamicMap);  // FBO-LEDGER-1
        glBindFramebuffer(GL_FRAMEBUFFER, dynShadowArrayFBO_);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, dynShadowArrayTex_, 0, 0);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, dynShadowArrayDummyColorTex_, 0, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_NONE);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            mc2::fmt::print_err("gosPostProcess: CSM dynamic shadow array FBO incomplete\n");

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

        // SHADOW-VRAM-ACCOUNT-1: register the near-cascade array (observe-only
        // metadata; never read by the draw path). Makes the per-cascade shadow
        // VRAM split (13365011) auditable from the debug-state dump: default =
        // (csmCount-1) layers x 8192^2 x Depth24 here + the separate 4096^2
        // full-map row below (dummy R8 color attachments not accounted).
        {
            RenderCore::RenderResourceDesc d;
            d.id        = RenderCore::RenderResourceId::ShadowCsmNearArray;
            d.kind      = RenderCore::RenderResourceKind::Texture2DArray;
            d.lifetime  = RenderCore::RenderResourceLifetime::Persistent;
            d.format    = RenderCore::RenderResourceFormat::Depth24;
            d.debugName = "ShadowCsmNearArray";
            d.width     = static_cast<uint32_t>(dynShadowMapSize_);
            d.height    = static_cast<uint32_t>(dynShadowMapSize_);
            d.layers    = static_cast<uint32_t>(arrayLayers);
            d.glName    = static_cast<uint32_t>(dynShadowArrayTex_);
            d.sizeBytes = static_cast<uint64_t>(dynShadowMapSize_) * static_cast<uint64_t>(dynShadowMapSize_)
                          * 4u * static_cast<uint64_t>(arrayLayers);
            d.valid     = true;
            RenderCore::registerOrUpdateRenderResource(d);
        }

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
            mc2::fmt::print_err("gosPostProcess: CSM dynamic full-map FBO incomplete\n");

        // Forward-Z clear to 1.0 (fully lit), restore scene reverse-Z default.
        glDepthMask(GL_TRUE);
        glClearDepth(1.0f);
        glClear(GL_DEPTH_BUFFER_BIT);
        glClearDepth(0.0f);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);

        // SHADOW-VRAM-ACCOUNT-1: register the far/full-map cascade (observe-only;
        // 4096^2 default, == dynShadowMapSize_ when MC2_SHADOW_FULLMAP_SEPARATE=0).
        {
            RenderCore::RenderResourceDesc d;
            d.id        = RenderCore::RenderResourceId::ShadowCsmFullMap;
            d.kind      = RenderCore::RenderResourceKind::Texture2D;
            d.lifetime  = RenderCore::RenderResourceLifetime::Persistent;
            d.format    = RenderCore::RenderResourceFormat::Depth24;
            d.debugName = "ShadowCsmFullMap";
            d.width     = static_cast<uint32_t>(dynamicFullMapSize_);
            d.height    = static_cast<uint32_t>(dynamicFullMapSize_);
            d.glName    = static_cast<uint32_t>(dynamicFullMapTex_);
            d.sizeBytes = static_cast<uint64_t>(dynamicFullMapSize_) * static_cast<uint64_t>(dynamicFullMapSize_) * 4u;
            d.valid     = true;
            RenderCore::registerOrUpdateRenderResource(d);
        }

        // Cascade matrices start as identity so an early sample is a no-op.
        for (int i = 0; i < kMaxCsmCascades; ++i) {
            dynamicCascadeMatrices_[i*16 + 0]  = 1.0f;
            dynamicCascadeMatrices_[i*16 + 5]  = 1.0f;
            dynamicCascadeMatrices_[i*16 + 10] = 1.0f;
            dynamicCascadeMatrices_[i*16 + 15] = 1.0f;
        }

        mc2::fmt::print_err("[CSM] init nearLayers={:d} arraySize={:d} fullMapSize={:d} separate={:d}\n", mc2::fmt::as_i32(nearCascades), mc2::fmt::as_i32(dynShadowMapSize_), mc2::fmt::as_i32(dynamicFullMapSize_), mc2::fmt::as_i32(mc2ShadowFullMapSeparate() ? 1 : 0));
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

    // SHADOW-VRAM-ACCOUNT-1: mark the two CSM accounting rows unavailable too.
    invalid.id = RenderCore::RenderResourceId::ShadowCsmNearArray;
    RenderCore::registerOrUpdateRenderResource(invalid);
    invalid.id = RenderCore::RenderResourceId::ShadowCsmFullMap;
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
    mc2ClampShadowSunAltitude(fx, fy, fz);   // SHADOW-SUN-ALT-CLAMP-1

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
    static const int s_minValidCorners =
        mc2::gate::get<"MC2_SHADOW_ROBUST_BASIS">() ? 4    // robust: fall back when <4 survive
                                                    : 1;   // legacy: only the ==0 (i.e. <1) path
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
    static const bool s_focusCenter = mc2::gate::get<"MC2_SHADOW_FOCUS_CENTER">();
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

        // [CSM focus] diag: gated on the CSM frustum-diag env.
        static const bool s_focusDiag = mc2::gate::get<"MC2_SHADOW_FRUSTUM_DIAG">();
        if (s_focusDiag) {
            mc2::fmt::print_out("[CSM focus] src={} fx={:.1f} fy={:.1f} fz={:.1f} t={:.3f} d={:.1f} cap={:.1f}\n", mc2::fmt::safe(focusSrc), focusWorld[0], focusWorld[1], focusWorld[2], t, d, cap);
        }
        (void)nearBase2; (void)farBase2;  // near/far identified via nearC/farC
    }
    float fitRadius = (halfLX > halfLY ? halfLX : halfLY);
    if (fitRadius < 64.0f) fitRadius = 64.0f;
    float r = mapHalfExtent_ * sqrtf(2.0f) * 1.05f;   // full-map safety cap
    if (fitRadius > r) fitRadius = r;
    const float origFitRadius = fitRadius;

    // SHADOW-BOUNDED-NEAR-FIT-1 (gate, DEFAULT ON -- opt out with
    // MC2_SHADOW_BOUNDED_NEAR_FIT=0): cap fit radius for higher near-shadow
    // texel density. Center is now correct by construction (light-space AABB);
    // no separate center-override needed.
    //
    // GATE-DEFAULT-TRUTH-1: this comment used to say "default OFF" while the
    // reader four lines below implemented default ON, and the registry entry and
    // docs/tier1_env_vars.md agreed with the comment rather than with the code.
    // A comment is not a default. If you change the idiom below, change this
    // line, RendererFeatureRegistry.h and the tier1 doc in the SAME commit --
    // scripts/check-gate-defaults.py now fails the build if they disagree.
    {
        static const bool s_boundedNear = mc2::gate::get<"MC2_SHADOW_BOUNDED_NEAR_FIT">();
        if (s_boundedNear) {
            static const float s_boundedRadiusRaw = mc2::gate::get<"MC2_SHADOW_BOUNDED_NEAR_RADIUS">();
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
        static const bool s_frustDiag = mc2::gate::get<"MC2_SHADOW_FRUSTUM_DIAG">();
        static int s_frustN = 0;
        if (s_frustDiag) {
            ++s_frustN;
            if (s_frustN <= 3 || (s_frustN % 300) == 0) {
                mc2::fmt::print_err("[SHADOW_FRUSTUM_DIAG] frame={:d} sunDir=({:.3f},{:.3f},{:.3f}) lightAABB=[{:.0f}..{:.0f},{:.0f}..{:.0f}] worldCenter=({:.0f},{:.0f},{:.0f}) fitRadius={:.0f}(orig={:.0f}) xyRadius={:.0f} mapClampR={:.0f} texelWU={:.3f} orthoWH={:.0f}x{:.0f} validCorners={:d} mapSize={:d}\n", mc2::fmt::as_i32(s_frustN), fx, fy, fz, minLX, maxLX, minLY, maxLY, camX, camY, camZ, fitRadius, origFitRadius, xyRadius, r, worldUnitsPerTexel, 2.0f * xyRadius, 2.0f * xyRadius, mc2::fmt::as_i32(validCorners), mc2::fmt::as_i32(dynShadowMapSize_));
                if (focusApplied) {
                    mc2::fmt::print_err("[SHADOW_FRUSTUM_DIAG]   focusCenter=1 focusWorld=({:.0f},{:.0f},{:.0f}) focusXYDist={:.0f} nearSpread={:.0f} farSpread={:.0f}\n", focusWorld[0], focusWorld[1], focusWorld[2], focusDistUsed, nearSpread, farSpread);
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
        if (mc2::gate::get<"MC2_DEBUG_SHADOW_ZRANGE">()) {
            static int s_csmN = 0;
            if (++s_csmN <= 1) {
                for (int ci = 0; ci < N; ++ci) {
                    const float* M = &dynamicCascadeMatrices_[ci*16];
                    // sample the cascade's own center (col 3 back-projected approx via camX/Y/Z)
                    const float px=camX, py=camY, pz=camZ;
                    float cz = M[0*4+2]*px+M[1*4+2]*py+M[2*4+2]*pz+M[3*4+2];
                    float cw = M[0*4+3]*px+M[1*4+3]*py+M[2*4+3]*pz+M[3*4+3];
                    float ndc = (cw!=0.0f)?cz/cw:0.0f;
                    mc2::fmt::print_err("[SHADOWZRANGE v1] event=csm cascade={:d} ndcZ={:.5f} inRange={:d}\n", mc2::fmt::as_i32(ci), ndc, mc2::fmt::as_i32((ndc>=0.0f&&ndc<=1.0f)?1:0));
                }
            }
        }
    }

    // [SHADOWZRANGE v1] VPL-#10: dynamic-path [0,1] verification. Rebuilds
    // per frame -> one-shot via static counter. Transforms the snapped camera
    // center + offsets through dynamicLightSpaceMatrix_; clip.z/clip.w MUST be
    // in [0,1] (lockstep with the static probe + the .xy-only sampler remap).
    if (mc2::gate::get<"MC2_DEBUG_SHADOW_ZRANGE">()) {
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
                mc2::fmt::print_err("[SHADOWZRANGE v1] event=dynamic pt={:d} world=({:.0f},{:.0f},{:.0f}) clipZ={:.5f} clipW={:.5f} ndcZ={:.5f} inRange={:d}\n", mc2::fmt::as_i32(i), px, py, pz, cz, cw, ndc, mc2::fmt::as_i32((ndc >= 0.0f && ndc <= 1.0f) ? 1 : 0));
            }
        }
    }
}
