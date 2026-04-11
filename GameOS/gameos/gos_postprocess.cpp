#include "gos_postprocess.h"
#include "utils/shader_builder.h"
#include "utils/gl_utils.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>

static gosPostProcess* s_postProcess = nullptr;

gosPostProcess* getGosPostProcess()
{
    return s_postProcess;
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
    , bloomEnabled_(false)
    , fxaaEnabled_(false)
    , tonemapEnabled_(false)
    , bloomIntensity_(0.3f)
    , bloomThreshold_(0.6f)
    , sceneFBO_(0)
    , sceneColorTex_(0)
    , sceneDepthRBO_(0)
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
    , shadowMapSize_(2048)
    , shadowsEnabled_(true)
    , shadowCacheMoveThreshold_(500.0f)
    , shadowCacheDirty_(true)
    , cachedShadowCenterX_(0.0f)
    , cachedShadowCenterY_(0.0f)
    , cachedShadowCenterZ_(0.0f)
    , shadowCenterX_(0.0f)
    , shadowCenterY_(0.0f)
    , shadowCenterZ_(0.0f)
{
    bloomFBO_[0] = bloomFBO_[1] = 0;
    bloomColorTex_[0] = bloomColorTex_[1] = 0;
    memset(lightSpaceMatrix_, 0, sizeof(lightSpaceMatrix_));
    memset(cachedLightSpaceMatrix_, 0, sizeof(cachedLightSpaceMatrix_));
    memset(savedViewport_, 0, sizeof(savedViewport_));
}

gosPostProcess::~gosPostProcess()
{
    if (initialized_)
        destroy();
}

void gosPostProcess::init(int w, int h)
{
    assert(!initialized_);

    width_ = w;
    height_ = h;

    createFBOs(w, h);
    createFullscreenQuad();

    // Load shaders — version provided via prefix (shader files must NOT have #version)
    static const char* kShaderPrefix = "#version 420\n";

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

    bloomThresholdProg_ = glsl_program::makeProgram("bloom_threshold",
        "shaders/postprocess.vert", "shaders/bloom_threshold.frag", kShaderPrefix);
    if (!bloomThresholdProg_ || !bloomThresholdProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile bloom_threshold shader\n");

    bloomBlurProg_ = glsl_program::makeProgram("bloom_blur",
        "shaders/postprocess.vert", "shaders/bloom_blur.frag", kShaderPrefix);
    if (!bloomBlurProg_ || !bloomBlurProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile bloom_blur shader\n");

    initShadows();

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

    if (bloomThresholdProg_) {
        glsl_program::deleteProgram("bloom_threshold");
        bloomThresholdProg_ = nullptr;
    }
    if (bloomBlurProg_) {
        glsl_program::deleteProgram("bloom_blur");
        bloomBlurProg_ = nullptr;
    }

    destroyShadows();

    s_postProcess = nullptr;
    initialized_ = false;
}

bool gosPostProcess::shouldRenderShadows()
{
    if (!shadowsEnabled_) return false;
    if (shadowCacheDirty_) {
        shadowCacheDirty_ = false;
        cachedShadowCenterX_ = shadowCenterX_;
        cachedShadowCenterY_ = shadowCenterY_;
        cachedShadowCenterZ_ = shadowCenterZ_;
        memcpy(cachedLightSpaceMatrix_, lightSpaceMatrix_, sizeof(lightSpaceMatrix_));
        return true;
    }
    float dx = shadowCenterX_ - cachedShadowCenterX_;
    float dy = shadowCenterY_ - cachedShadowCenterY_;
    float dz = shadowCenterZ_ - cachedShadowCenterZ_;
    float dist2 = dx*dx + dy*dy + dz*dz;
    if (dist2 > shadowCacheMoveThreshold_ * shadowCacheMoveThreshold_) {
        cachedShadowCenterX_ = shadowCenterX_;
        cachedShadowCenterY_ = shadowCenterY_;
        cachedShadowCenterZ_ = shadowCenterZ_;
        memcpy(cachedLightSpaceMatrix_, lightSpaceMatrix_, sizeof(lightSpaceMatrix_));
        return true;
    }
    return false;
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

    // Depth/stencil renderbuffer
    glGenRenderbuffers(1, &sceneDepthRBO_);
    glBindRenderbuffer(GL_RENDERBUFFER, sceneDepthRBO_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, sceneDepthRBO_);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gosPostProcess: scene FBO incomplete (0x%x)\n", status);
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

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
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
    if (sceneDepthRBO_) {
        glDeleteRenderbuffers(1, &sceneDepthRBO_);
        sceneDepthRBO_ = 0;
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

    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    glViewport(0, 0, width_, height_);
}

void gosPostProcess::runBloom()
{
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

void gosPostProcess::endScene()
{
    if (!initialized_)
        return;

    runBloom();

    // Bind default framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);

    // Disable depth test and face culling for fullscreen quad
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    // Draw fullscreen quad with composite shader
    if (compositeProg_ && compositeProg_->is_valid()) {
        // Set uniforms BEFORE apply() — apply() binds program + flushes dirty uniforms
        compositeProg_->setInt("sceneTex", 0);
        compositeProg_->setInt("bloomTex", 1);
        compositeProg_->setFloat("exposure", exposure_);
        compositeProg_->setInt("enableBloom", bloomEnabled_ ? 1 : 0);
        compositeProg_->setInt("enableFXAA", fxaaEnabled_ ? 1 : 0);
        compositeProg_->setInt("enableTonemap", tonemapEnabled_ ? 1 : 0);
        compositeProg_->setFloat("bloomIntensity", bloomIntensity_);

        float invSize[2] = { 1.0f / (float)width_, 1.0f / (float)height_ };
        compositeProg_->setFloat2("inverseScreenSize", invSize);
        compositeProg_->apply();

        // Bind scene color texture to unit 0
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sceneColorTex_);

        // Bind bloom texture to unit 1 (unused for now, bind first bloom tex)
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, bloomColorTex_[0]);

        // Draw the fullscreen quad
        glBindVertexArray(quadVAO_);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        glActiveTexture(GL_TEXTURE0);
    }

    // Re-enable depth test
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

void gosPostProcess::initShadows()
{
    shadowMapSize_ = 4096;

    static const char* kShaderPrefix = "#version 420\n";
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
    memset(lightSpaceMatrix_, 0, sizeof(lightSpaceMatrix_));
    lightSpaceMatrix_[0] = lightSpaceMatrix_[5] = lightSpaceMatrix_[10] = lightSpaceMatrix_[15] = 1.0f;

    // Clear shadow map to max depth (1.0) so everything is "lit"
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFBO_);
    glClear(GL_DEPTH_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gosPostProcess::beginShadowPass()
{
    if (!shadowsEnabled_ || !shadowFBO_) return;

    glGetIntegerv(GL_VIEWPORT, savedViewport_);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFBO_);
    glViewport(0, 0, shadowMapSize_, shadowMapSize_);
    glClear(GL_DEPTH_BUFFER_BIT);

    // Force depth test and writing ON
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    // Polygon offset to reduce shadow acne
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0f, 4.0f);

    // Only need depth — disable color writes
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    // Disable culling so both faces write depth
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
}

void gosPostProcess::updateLightMatrix(float sunDirX, float sunDirY, float sunDirZ,
                                        float camX, float camY, float camZ, float radius)
{
    if (!shadowsEnabled_) return;

    // Save current frame shadow center for shouldRenderShadows()
    shadowCenterX_ = camX;
    shadowCenterY_ = camY;
    shadowCenterZ_ = camZ;

    // Normalize sun direction
    float len = sqrtf(sunDirX*sunDirX + sunDirY*sunDirY + sunDirZ*sunDirZ);
    if (len < 0.001f) return;
    // Forward = from light toward scene (= sunDir direction)
    float fx = sunDirX/len, fy = sunDirY/len, fz = sunDirZ/len;

    // Light "position" behind the scene (along -forward from camera)
    float r = radius;
    float lightPosX = camX - fx * r;
    float lightPosY = camY - fy * r;
    float lightPosZ = camZ - fz * r;

    // Right = cross(forward, up_hint)
    float ux = 0, uy = 0, uz = 1;  // Z-up for MC2 world coordinates
    if (fabsf(fz) > 0.9f) { ux = 0; uy = 1; uz = 0; }

    float rx = fy * uz - fz * uy;
    float ry = fz * ux - fx * uz;
    float rz = fx * uy - fy * ux;
    len = sqrtf(rx*rx + ry*ry + rz*rz);
    rx /= len; ry /= len; rz /= len;

    // True up = cross(right, forward)
    ux = ry * fz - rz * fy;
    uy = rz * fx - rx * fz;
    uz = rx * fy - ry * fx;

    // Standard OpenGL lookAt view matrix (column-major)
    // Camera looks down -Z, so Z-row = -forward
    float view[16] = {
         rx,  ux, -fx, 0,
         ry,  uy, -fy, 0,
         rz,  uz, -fz, 0,
        -(rx*lightPosX + ry*lightPosY + rz*lightPosZ),
        -(ux*lightPosX + uy*lightPosY + uz*lightPosZ),
         (fx*lightPosX + fy*lightPosY + fz*lightPosZ),
        1
    };

    // Ortho projection: maps eye-space z ∈ [-farP, -nearP] to NDC [-1, 1]
    float nearP = 1.0f, farP = 2.0f * r;
    float ortho[16] = {
        1.0f/r, 0, 0, 0,
        0, 1.0f/r, 0, 0,
        0, 0, -2.0f/(farP - nearP), 0,
        0, 0, -(farP + nearP)/(farP - nearP), 1
    };

    // lightSpaceMatrix = ortho * view
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0;
            for (int k = 0; k < 4; k++)
                sum += ortho[k * 4 + row] * view[col * 4 + k];
            lightSpaceMatrix_[col * 4 + row] = sum;
        }
    }
}
