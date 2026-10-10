// gosPostProcess: skybox and HDRI skybox rendering.
// Declared in the class headers; shared names in gos_postprocess_internal.h.
#include "gos_postprocess_internal.h"


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

// MC2_HDRI_SKY_AZ_OFFSET is a manual sun-sync trim in DEGREES; every sky path wants it in radians.
static float hdriAzOffsetRadians()
{
    return static_cast<float>(static_cast<double>(mc2::gate::get<"MC2_HDRI_SKY_AZ_OFFSET">()) * 3.14159265358979323846 / 180.0);
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

#ifdef MC2_VULKAN_ISLAND
    // VK-PARITY-F3: under the VK umbrella the VK terrain island is the native first owner
    // and the presented image is VK-composited, so a GL draw here lands in a discarded
    // buffer (VK showed the flat 0.55/0.62/0.72 clear instead of this HDRI sky). Hand the
    // exact same inputs (inverse projection, inverse view rotation, sun-sync yaw, GL HDRI
    // tex id) to the VK sky island, which draws the identical equirect lookup behind the
    // terrain, and skip the GL draw. Gate off -> this block is dead and GL is unchanged.
    if (mc2_vk_sky_island_enabled()) {
        // Sun-sync yaw: LOCKSTEP with the GL path below (manual trim + baked-sun align).
        static const float s_vkAzOffsetRad = hdriAzOffsetRadians();
        float vkSkyYaw = s_vkAzOffsetRad;
        if (hdriBakedSunValid_) {
            float lx = 0.0f, ly = 0.0f, lz = 0.0f;
            gos_GetTerrainLightDir(&lx, &ly, &lz);
            vkSkyYaw += hdriBakedSunAz_ - atan2f(ly, -lx);
        }
        skyYaw_ = vkSkyYaw;  // WATER-HDRI-REFL-1: cache for the water shader uniform (same as GL path)
        mc2_vk_sky_submit(invProjArray, invViewRot, vkSkyYaw, (unsigned int)hdriTex_);
        return;
    }
#endif

    // Query runtime cap on color attachments so save/mask/restore
    // adapts to whichever buffers are bound (ObjectID attachment 2
    // only exists when MC2_OBJECT_ID_BUFFER is set).
    GLint maxDrawBuffers = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
    const int nAtt = (maxDrawBuffers < 3) ? maxDrawBuffers : 3;

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
    // The sun-sync rotation is only applied when the AZ offset is set OR the
    // baked-sun scan succeeded AND a non-zero offset/sync makes skyYaw != 0;
    // when skyYaw resolves to exactly 0.0 the frag path is byte-identical.
    static const int s_uvDebug = mc2::gate::get<"MC2_HDRI_SKY_UV_DEBUG">();
    // MC2_HDRI_SKY_FRAME_FIX: when set (any non-"0" value), applies the
    // kAxisSwapMC2toGL conversion to worldDir in the frag shader before equirect
    // lookup.  Default OFF => frameFix=0 => byte-identical baseline.
    static const int s_frameFix = mc2::gate::get<"MC2_HDRI_SKY_FRAME_FIX">();
    static const bool  s_azOffsetSet = mc2::gate::isSet<"MC2_HDRI_SKY_AZ_OFFSET">();
    static const float s_azOffsetRad = hdriAzOffsetRadians();

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
            MC2_DIAGFE(stderr,
                "[HDRI_SKY v1] uv_debug center_worldDir=(%.3f,%.3f,%.3f) "
                "bakedSunValid=%d bakedSunAz=%.4f skyYaw=%.4f\n",
                wd[0], wd[1], wd[2], hdriBakedSunValid_ ? 1 : 0,
                hdriBakedSunAz_, skyYaw);
            std::fflush(stderr);
        }
    }

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
                MC2_DIAGFE(stderr,
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
                MC2_DIAGFE(stderr,
                    "[SKYBOX_FOG_EXCLUDE v2] enabled=1 stencil_tag_pass=active "
                    "threshold=worldDir.y>0.22 (elevation; v1 wedge bug fixed)\n");
                std::fflush(stderr);
                return true;
            }();
            (void)s_tagLogOnce;
        }
    }

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

    static const int s_uvDebug = mc2::gate::get<"MC2_HDRI_SKY_UV_DEBUG">();
    static const bool  s_azOffsetSet = mc2::gate::isSet<"MC2_HDRI_SKY_AZ_OFFSET">();
    static const float s_azOffsetRad = hdriAzOffsetRadians();

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
}

// HDRI-SKY frame fix (MC2_HDRI_SKY_FRAME_FIX ON path, one-proven-matrix).
// Reconstructs the sky ray by UNPROJECTING NDC through the inverse of
// worldToClipGL — the EXACT matrix the GPU rasterizes terrain with
// (= kAxisSwapMC2toGL * worldToCameraMatrix * cameraToClipGL). Because that matrix's leftmost factor is the MC2->GL
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
    static const int s_uvDebug = mc2::gate::get<"MC2_HDRI_SKY_UV_DEBUG">();
    static const bool  s_azOffsetSet = mc2::gate::isSet<"MC2_HDRI_SKY_AZ_OFFSET">();
    static const float s_azOffsetRad = hdriAzOffsetRadians();

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
}
