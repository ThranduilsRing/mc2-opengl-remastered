// gosPostProcess: HZB reduce (the screen-space shadow, cloud shadow, shoreline, edge fog and fog-OOB passes are Vulkan native only).
// Declared in the class headers; shared names in gos_postprocess_internal.h.
#include "gos_postprocess_internal.h"
#include "mc2fmt.h"


// BT2018-BOX-DECAL-1: unit cube [-0.5,0.5]^3, 8 corners / 36 indices (12 tris).
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
