// gosPostProcess: box decals, HZB reduce, SSAO, screen-space shadow, cloud shadow, shoreline, edge fog and fog-OOB passes.
// Declared in the class headers; shared names in gos_postprocess_internal.h.
#include "gos_postprocess_internal.h"


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
    if (mc2::gate::isSet<"MC2_BOX_DECAL_STRENGTH">()) { const float v = mc2::gate::get<"MC2_BOX_DECAL_STRENGTH">(); if (v >= 0.0f) strength = v; }

    // Vertical span of the AABB on the reconstruct-frame up axis (+Y). The box is made
    // tall so it drapes across terrain elevation under the impact; the FS world-AABB clip
    // does the real coverage. Overridable for tuning.
    float ySpan = 4000.0f;
    if (mc2::gate::isSet<"MC2_BOX_DECAL_YSPAN">()) { const float v = mc2::gate::get<"MC2_BOX_DECAL_YSPAN">(); if (v > 1.0f) ySpan = v; }

    // Normal-reject: SHIPPED DISABLED by default. The terrain GBuffer1 normal is a
    // constant flat-up sentinel in a Z-up frame while the box up-axis here is Y-up, so a
    // live dot()-reject would discard ALL terrain pixels (see DECAL-INTEGRATE-1 doc, the
    // up-axis TODO). A threshold <= -1.5 makes the reject a no-op (never discards). Do
    // NOT raise this without first reconciling u_decalUpAxis against the reconstruct frame.
    float normalRejectCos = -1.5f;
    if (mc2::gate::isSet<"MC2_BOX_DECAL_NORMAL_REJECT">()) normalRejectCos = mc2::gate::get<"MC2_BOX_DECAL_NORMAL_REJECT">();

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

    // --- Pass 1b/1c: SSAO-BLUR-1 separable depth-aware bilateral blur ---
    //
    // H into ssaoBlurTex_, then V back into ssaoColorTex_, so ssaoColorTex_
    // still holds the FINAL AO when the (unchanged) apply pass and the
    // SsaoOcclusion resource id read it. Both draws stay at half res.
    //
    // Depth stays bound on unit 0 from the gen pass; the AO source goes on
    // unit 1. Skipped entirely when MC2_SSAO_BLUR=0 or the program failed to
    // compile, which reproduces the pre-SSAO-BLUR-1 chain exactly.
    if (ssaoBlurEnabled_ && ssaoBlurProg_ && ssaoBlurProg_->is_valid() &&
        ssaoBlurFBO_ && ssaoBlurTex_)
    {
        TracyGpuZone("Render.SSAO.Blur");

        const float hTexel[2] = { 1.0f / (float)ssaoW_, 0.0f };
        const float vTexel[2] = { 0.0f, 1.0f / (float)ssaoH_ };

        ssaoBlurProg_->setInt("ssaoTex", 1);
        ssaoBlurProg_->setInt("sceneDepthTex", 0);
        ssaoBlurProg_->setFloat("depthSigma", aoBlurDepthSigma_);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);

        glBindFramebuffer(GL_FRAMEBUFFER, ssaoBlurFBO_);
        glViewport(0, 0, ssaoW_, ssaoH_);
        ssaoBlurProg_->setFloat2("blurDir", hTexel);
        ssaoBlurProg_->apply();
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, ssaoColorTex_);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO_);
        glViewport(0, 0, ssaoW_, ssaoH_);
        ssaoBlurProg_->setFloat2("blurDir", vTexel);
        ssaoBlurProg_->apply();
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, ssaoBlurTex_);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Leave unit 1 clean for the apply pass (which binds only unit 0).
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
    }

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


void gosPostProcess::runScreenShadow()
{
    ZoneScopedN("Render.ScreenShadow");
    TracyGpuZone("Render.ScreenShadow");

    if (!screenShadowEnabled_) return;
    if (!sceneHasTerrain_) return;
    if (!screenShadowProg_ || !screenShadowProg_->is_valid()) return;
    if (!shadowsEnabled_) return;

#ifdef MC2_VULKAN_ISLAND
    if (vkPostScreenFxNativeEnabled() && runVkPostScreenShadowStaticNative())
        return;
#endif

    // APPLY-STATE-SCREENSHADOW-1: skip the 4 setup calls when the executor already
    // applied them (MC2_FRAMEGRAPH_EXECUTOR ON). Reset one-shot before draw.
    if (!screenShadowStateAppliedByExecutor_) {
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
        // SHADOW-SUNCLAMP-1: apply the SAME altitude clamp the shadow matrices
        // used (gameos_graphics.cpp:10197 passes this identical -raw expression
        // into buildStaticLightMatrix, which clamps it). Without this the
        // back-face guard rejects surfaces the shadow map has real occlusion for
        // whenever the mission authors a near-horizontal sun. No-op on maps whose
        // sun is already above the 15deg minimum (byte-identical).
        if (mc2ShadowGuardClampedSun())
            mc2ClampShadowSunAltitude(lightDirVec[0], lightDirVec[1], lightDirVec[2]);
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

    GLuint screenShadowCanaryQuery = 0;
    const bool screenShadowCanary = screenShadowGlCanaryEnabled();
    if (screenShadowCanary) {
        glGenQueries(1, &screenShadowCanaryQuery);
        glBeginQuery(GL_ANY_SAMPLES_PASSED, screenShadowCanaryQuery);
    }
    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    if (screenShadowCanary) {
        glEndQuery(GL_ANY_SAMPLES_PASSED);
        GLuint anySamples = 0;
        glGetQueryObjectuiv(screenShadowCanaryQuery, GL_QUERY_RESULT, &anySamples);
        std::fprintf(stderr,
            "[VK_SCREENSHADOW_GL_CANARY v1] any_samples=%u sceneFBO=%u csm=%d debug=%d\n",
            (unsigned)anySamples, (unsigned)sceneFBO_, csmActive ? 1 : 0, screenShadowDebug_);
        glDeleteQueries(1, &screenShadowCanaryQuery);
    }

    glBindVertexArray(0);

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

    if (!enableCloudShadow_) return;               // env/ImGui gate -> skip entirely
    if (!sceneHasTerrain_) return;
    if (!cloudProg_ || !cloudProg_->is_valid()) return;

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

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
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

// BACKDROP-BLIT-ATTRIB-1: night-dim the horizon cloud-bank fog color. Gate OFF
// (default) -> straight copy -> byte-identical. Shared by the legacy runEdgeFog
// upload and the frame-graph executor context so both backends stay consistent.
void gosPostProcess::effectiveFogColor(const float in[3], float out[3]) const
{
    const float s = nightFogDimEnabled_ ? nightFogDimScale_ : 1.0f;
    out[0] = in[0] * s;
    out[1] = in[1] * s;
    out[2] = in[2] * s;
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
    float edgeFogColorEff[3];
    effectiveFogColor(edgeFogColor_, edgeFogColorEff);   // BACKDROP-BLIT-ATTRIB-1
    edgeFogProg_->setFloat3("u_fogColor",        edgeFogColorEff);
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
        static const bool s_backendIface = mc2::gate::get<"MC2_RENDER_BACKEND_IFACE">();
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
    float oobFogColorEff[3];                              // BACKDROP-BLIT-ATTRIB-1
    effectiveFogColor(oobFogColor_, oobFogColorEff);
    fogOobProg_->setFloat3("u_fogColor",  oobFogColorEff);
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
