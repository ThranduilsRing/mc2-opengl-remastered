// gosPostProcess: box decals, HZB reduce and SSAO (the screen-space shadow, cloud shadow, shoreline, edge fog and fog-OOB passes are Vulkan native only).
// Declared in the class headers; shared names in gos_postprocess_internal.h.
#include "gos_postprocess_internal.h"
#include "mc2fmt.h"


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
                mc2::fmt::print_err("[HZB_BUILD v1] FBO incomplete at level {:d} (0x{:x})\n", mc2::fmt::as_i32(level), mc2::fmt::as_u32(st));
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
            mc2::fmt::print_err("[HZB_BUILD v1] built pyramid {:d}x{:d} mips={:d} (count={:d})\n", mc2::fmt::as_i32(hzbW_), mc2::fmt::as_i32(hzbH_), mc2::fmt::as_i32(hzbMipCount_), mc2::fmt::as_u64((unsigned long long)hzbBuildCount_));
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
    // unit 1. Skipped when the blur is unticked in the options window or the
    // program failed to compile, which reproduces the pre-SSAO-BLUR-1 chain exactly.
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
