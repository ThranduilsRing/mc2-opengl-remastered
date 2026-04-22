#include "gos_postprocess.h"
#include "utils/shader_builder.h"
#include "utils/gl_utils.h"
#include "gos_profiler.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <SDL2/SDL.h>

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
    , screenShadowEnabled_(false)
    , screenShadowDebug_(0)
    , ssaoProg_(nullptr)
    , ssaoBlurProg_(nullptr)
    , ssaoApplyProg_(nullptr)
    , ssaoFBO_(0)
    , ssaoColorTex_(0)
    , ssaoBlurFBO_(0)
    , ssaoBlurTex_(0)
    , ssaoNoiseTex_(0)
    , ssaoEnabled_(false)
    , ssaoRadius_(40.0f)
    , ssaoBias_(1.0f)
    , ssaoPower_(1.5f)
    , sceneHasTerrain_(false)
    , prevFrameHadTerrain_(false)
    , grassEnabled_(false)
    , grassProg_(nullptr)
    , godrayEnabled_(false)  // disabled: no visible sky at RTS zoom. RAlt+6 to test.
    , godrayProg_(nullptr)
    , godrayFBO_(0)
    , godrayColorTex_(0)
    , shorelineEnabled_(true)
    , shorelineProg_(nullptr)
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

    ssaoProg_ = glsl_program::makeProgram("ssao",
        "shaders/postprocess.vert", "shaders/ssao.frag", kShaderPrefix);
    if (!ssaoProg_ || !ssaoProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile ssao shader\n");

    ssaoBlurProg_ = glsl_program::makeProgram("ssao_blur",
        "shaders/postprocess.vert", "shaders/ssao_blur.frag", kShaderPrefix);
    if (!ssaoBlurProg_ || !ssaoBlurProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile ssao_blur shader\n");

    ssaoApplyProg_ = glsl_program::makeProgram("ssao_apply",
        "shaders/postprocess.vert", "shaders/ssao_apply.frag", kShaderPrefix);
    if (!ssaoApplyProg_ || !ssaoApplyProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile ssao_apply shader\n");

    // Grass is deprecated; keep the path disabled and skip shader setup entirely.
    grassProg_ = nullptr;

    godrayProg_ = glsl_program::makeProgram("godray",
        "shaders/postprocess.vert", "shaders/godray.frag", kShaderPrefix);
    if (!godrayProg_ || !godrayProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile godray shader\n");

    shorelineProg_ = glsl_program::makeProgram("shoreline",
        "shaders/postprocess.vert", "shaders/shoreline.frag", kShaderPrefix);
    if (!shorelineProg_ || !shorelineProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile shoreline shader\n");

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

    if (ssaoProg_) {
        glsl_program::deleteProgram("ssao");
        ssaoProg_ = nullptr;
    }
    if (ssaoBlurProg_) {
        glsl_program::deleteProgram("ssao_blur");
        ssaoBlurProg_ = nullptr;
    }
    if (ssaoApplyProg_) {
        glsl_program::deleteProgram("ssao_apply");
        ssaoApplyProg_ = nullptr;
    }

    grassProg_ = nullptr;

    if (godrayProg_) {
        glsl_program::deleteProgram("godray");
        godrayProg_ = nullptr;
    }

    if (shorelineProg_) {
        glsl_program::deleteProgram("shoreline");
        shorelineProg_ = nullptr;
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

    // MRT: draw to both color attachments
    GLenum drawBuffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, drawBuffers);

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

    // --- SSAO FBOs (half resolution, single channel) ---
    {
        // SSAO raw output
        glGenFramebuffers(1, &ssaoFBO_);
        glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO_);

        glGenTextures(1, &ssaoColorTex_);
        glBindTexture(GL_TEXTURE_2D, ssaoColorTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, halfW, halfH, 0, GL_RED, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ssaoColorTex_, 0);

        status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "gosPostProcess: SSAO FBO incomplete (0x%x)\n", status);

        // SSAO blur target
        glGenFramebuffers(1, &ssaoBlurFBO_);
        glBindFramebuffer(GL_FRAMEBUFFER, ssaoBlurFBO_);

        glGenTextures(1, &ssaoBlurTex_);
        glBindTexture(GL_TEXTURE_2D, ssaoBlurTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, halfW, halfH, 0, GL_RED, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ssaoBlurTex_, 0);

        status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "gosPostProcess: SSAO blur FBO incomplete (0x%x)\n", status);
    }

    // --- 4x4 SSAO noise texture ---
    {
        // Random tangent-space rotation vectors
        float noiseData[4 * 4 * 3];
        // Deterministic noise pattern (reproducible)
        unsigned int seed = 42;
        for (int i = 0; i < 16; i++) {
            seed = seed * 1103515245 + 12345;
            float x = ((seed >> 16) & 0x7FFF) / 16383.5f - 1.0f;
            seed = seed * 1103515245 + 12345;
            float y = ((seed >> 16) & 0x7FFF) / 16383.5f - 1.0f;
            // Encode as [0,1] for RGB texture
            noiseData[i * 3 + 0] = x * 0.5f + 0.5f;
            noiseData[i * 3 + 1] = y * 0.5f + 0.5f;
            noiseData[i * 3 + 2] = 0.5f;
        }
        glGenTextures(1, &ssaoNoiseTex_);
        glBindTexture(GL_TEXTURE_2D, ssaoNoiseTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, 4, 4, 0, GL_RGB, GL_FLOAT, noiseData);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
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
    if (sceneDepthTex_) {
        glDeleteTextures(1, &sceneDepthTex_);
        sceneDepthTex_ = 0;
    }
    if (sceneNormalTex_) {
        glDeleteTextures(1, &sceneNormalTex_);
        sceneNormalTex_ = 0;
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
    if (ssaoFBO_) { glDeleteFramebuffers(1, &ssaoFBO_); ssaoFBO_ = 0; }
    if (ssaoColorTex_) { glDeleteTextures(1, &ssaoColorTex_); ssaoColorTex_ = 0; }
    if (ssaoBlurFBO_) { glDeleteFramebuffers(1, &ssaoBlurFBO_); ssaoBlurFBO_ = 0; }
    if (ssaoBlurTex_) { glDeleteTextures(1, &ssaoBlurTex_); ssaoBlurTex_ = 0; }
    if (ssaoNoiseTex_) { glDeleteTextures(1, &ssaoNoiseTex_); ssaoNoiseTex_ = 0; }
    if (godrayColorTex_) { glDeleteTextures(1, &godrayColorTex_); godrayColorTex_ = 0; }
    if (godrayFBO_) { glDeleteFramebuffers(1, &godrayFBO_); godrayFBO_ = 0; }
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
    // Start with single draw buffer — MRT only during terrain rendering
    // (AMD RX 7900 corrupts color output if non-terrain shaders write location=1)
    if (sceneNormalTex_) {
        // Briefly enable both draw buffers so we can clear the normal buffer
        GLenum drawBuffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glDrawBuffers(2, drawBuffers);
    }
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

void gosPostProcess::enableMRT()
{
    if (sceneNormalTex_) {
        GLenum drawBuffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glDrawBuffers(2, drawBuffers);
    }
}

void gosPostProcess::disableMRT()
{
    GLenum singleBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &singleBuf);
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
    GLenum singleBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &singleBuf);
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
    screenShadowProg_->setFloat("time", (float)SDL_GetTicks() * 0.001f);
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

void gosPostProcess::runSSAO()
{
    ZoneScopedN("Render.SSAO");
    TracyGpuZone("Render.SSAO");

    if (!ssaoEnabled_) return;
    if (!ssaoProg_ || !ssaoProg_->is_valid()) return;
    if (!ssaoBlurProg_ || !ssaoBlurProg_->is_valid()) return;

    int hw = width_ / 2, hh = height_ / 2;
    if (hw < 1) hw = 1;
    if (hh < 1) hh = 1;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);

    // Pass 1: SSAO sampling at half resolution
    glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO_);
    glViewport(0, 0, hw, hh);
    glClear(GL_COLOR_BUFFER_BIT);

    ssaoProg_->setInt("sceneDepthTex", 0);
    ssaoProg_->setInt("sceneNormalTex", 1);
    ssaoProg_->setInt("noiseTex", 2);
    ssaoProg_->setFloat("ssaoRadius", ssaoRadius_);
    ssaoProg_->setFloat("ssaoBias", ssaoBias_);
    ssaoProg_->setFloat("ssaoPower", ssaoPower_);
    float screenSz[2] = { (float)width_, (float)height_ };
    ssaoProg_->setFloat2("screenSize", screenSz);
    ssaoProg_->apply();

    // Upload matrices via direct GL
    GLint loc;
    loc = glGetUniformLocation(ssaoProg_->shp_, "viewProj");
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, viewProj_);
    loc = glGetUniformLocation(ssaoProg_->shp_, "inverseViewProj");
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, inverseViewProj_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sceneNormalTex_);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ssaoNoiseTex_);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Pass 2: Bilateral blur — horizontal
    float texelSz[2] = { 1.0f / (float)hw, 1.0f / (float)hh };

    glBindFramebuffer(GL_FRAMEBUFFER, ssaoBlurFBO_);
    ssaoBlurProg_->setInt("ssaoTex", 0);
    ssaoBlurProg_->setInt("sceneDepthTex", 1);
    ssaoBlurProg_->setFloat2("texelSize", texelSz);
    ssaoBlurProg_->setInt("blurHorizontal", 1);
    ssaoBlurProg_->apply();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ssaoColorTex_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Pass 3: Bilateral blur — vertical (back into ssaoColorTex_)
    glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO_);
    ssaoBlurProg_->setInt("blurHorizontal", 0);
    ssaoBlurProg_->apply();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ssaoBlurTex_);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Pass 4+5: Second blur iteration for smoother result
    glBindFramebuffer(GL_FRAMEBUFFER, ssaoBlurFBO_);
    ssaoBlurProg_->setInt("blurHorizontal", 1);
    ssaoBlurProg_->apply();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ssaoColorTex_);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO_);
    ssaoBlurProg_->setInt("blurHorizontal", 0);
    ssaoBlurProg_->apply();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ssaoBlurTex_);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Result is in ssaoColorTex_ — apply to scene via multiplicative blending
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    GLenum singleBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &singleBuf);
    glViewport(0, 0, width_, height_);

    glEnable(GL_BLEND);
    glBlendFunc(GL_DST_COLOR, GL_ZERO);

    ssaoApplyProg_->setInt("ssaoTex", 0);
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

    float elapsed = (float)SDL_GetTicks() / 1000.0f;

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
    GLenum singleBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &singleBuf);
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
    GLenum singleBuf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &singleBuf);
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
    float elapsed = (float)SDL_GetTicks() / 1000.0f;
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

    // Post-process shadow pass: covers terrain, objects, and overlays in one
    // pass, with reduced terrain darkening to avoid obvious double-shadowing.
    runScreenShadow();

    // Shoreline foam pass (brightens water pixels adjacent to terrain)
    runShoreline();

    // SSAO pass (half-res, bilateral blurred, multiplicative)
    runSSAO();

    // God rays pass (radial light scattering, additive)
    runGodRays();

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

    // Shadow debug overlay (draws on top of composite)
    drawShadowDebugOverlay();

    // Re-enable depth test
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
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
    glPolygonOffset(2.0f, 4.0f);

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

    // Right = cross(forward, up_hint); Z-up for MC2
    float ux = 0, uy = 0, uz = 1;
    if (fabsf(fz) > 0.9f) { ux = 0; uy = 1; uz = 0; }

    float rx = fy * uz - fz * uy;
    float ry = fz * ux - fx * uz;
    float rz = fx * uy - fy * ux;
    len = sqrtf(rx*rx + ry*ry + rz*rz);
    rx /= len; ry /= len; rz /= len;

    ux = ry * fz - rz * fy;
    uy = rz * fx - rx * fz;
    uz = rx * fy - ry * fx;

    float view[16] = {
         rx,  ux, -fx, 0,
         ry,  uy, -fy, 0,
         rz,  uz, -fz, 0,
        -(rx*lightPosX + ry*lightPosY + rz*lightPosZ),
        -(ux*lightPosX + uy*lightPosY + uz*lightPosZ),
         (fx*lightPosX + fy*lightPosY + fz*lightPosZ),
        1
    };

    // Ortho covers full map; near/far envelope the full elevation range
    float nearP = 1.0f, farP = 2.0f * r;
    float ortho[16] = {
        1.0f/r, 0, 0, 0,
        0, 1.0f/r, 0, 0,
        0, 0, -2.0f/(farP - nearP), 0,
        0, 0, -(farP + nearP)/(farP - nearP), 1
    };

    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0;
            for (int k = 0; k < 4; k++)
                sum += ortho[k * 4 + row] * view[col * 4 + k];
            staticLightSpaceMatrix_[col * 4 + row] = sum;
        }
    }

    fprintf(stderr, "gosPostProcess: rendering static shadows (map half-extent=%.0f)\n", mapHalfExtent);
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

    // Clear to max depth (fully lit)
    glBindFramebuffer(GL_FRAMEBUFFER, dynShadowFBO_);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gosPostProcess::destroyDynamicShadows()
{
    if (dynShadowFBO_) { glDeleteFramebuffers(1, &dynShadowFBO_); dynShadowFBO_ = 0; }
    if (dynShadowDepthTex_) { glDeleteTextures(1, &dynShadowDepthTex_); dynShadowDepthTex_ = 0; }
    if (dynShadowDummyColorTex_) { glDeleteTextures(1, &dynShadowDummyColorTex_); dynShadowDummyColorTex_ = 0; }
}

void gosPostProcess::buildDynamicLightMatrix(float sunDirX, float sunDirY, float sunDirZ,
                                              float camX, float camY, float camZ)
{
    if (!shadowsEnabled_ || !dynShadowFBO_) return;

    ZoneScopedN("Shadow.DynMatrixBuild");

    float len = sqrtf(sunDirX*sunDirX + sunDirY*sunDirY + sunDirZ*sunDirZ);
    if (len < 0.001f) return;
    float fx = sunDirX/len, fy = sunDirY/len, fz = sunDirZ/len;

    float xyRadius = 2400.0f;  // half-extent of the dynamic shadow ortho. Covers the
                                // zoomed-out camera view plus enough margin for
                                // off-screen casters. At 4096² map, texel density is
                                // (2*xyRadius)/4096 units/texel. 2400 → ~1.17 u/tex.
    float depthDist = 5000.0f;              // large depth to envelope all elevations

    // Texel snapping: quantize camera position to shadow texel grid
    float worldUnitsPerTexel = (2.0f * xyRadius) / (float)dynShadowMapSize_;
    camX = floorf(camX / worldUnitsPerTexel) * worldUnitsPerTexel;
    camY = floorf(camY / worldUnitsPerTexel) * worldUnitsPerTexel;
    // Don't clamp Z — keep true camera elevation for depth centering

    float lightPosX = camX - fx * depthDist;
    float lightPosY = camY - fy * depthDist;
    float lightPosZ = camZ - fz * depthDist;

    float ux = 0, uy = 0, uz = 1;
    if (fabsf(fz) > 0.9f) { ux = 0; uy = 1; uz = 0; }

    float rx = fy * uz - fz * uy;
    float ry = fz * ux - fx * uz;
    float rz = fx * uy - fy * ux;
    len = sqrtf(rx*rx + ry*ry + rz*rz);
    rx /= len; ry /= len; rz /= len;

    ux = ry * fz - rz * fy;
    uy = rz * fx - rx * fz;
    uz = rx * fy - ry * fx;

    float view[16] = {
         rx,  ux, -fx, 0,
         ry,  uy, -fy, 0,
         rz,  uz, -fz, 0,
        -(rx*lightPosX + ry*lightPosY + rz*lightPosZ),
        -(ux*lightPosX + uy*lightPosY + uz*lightPosZ),
         (fx*lightPosX + fy*lightPosY + fz*lightPosZ),
        1
    };

    float nearP = 1.0f, farP = 2.0f * depthDist;
    float ortho[16] = {
        1.0f/xyRadius, 0, 0, 0,
        0, 1.0f/xyRadius, 0, 0,
        0, 0, -2.0f/(farP - nearP), 0,
        0, 0, -(farP + nearP)/(farP - nearP), 1
    };

    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0;
            for (int k = 0; k < 4; k++)
                sum += ortho[k * 4 + row] * view[col * 4 + k];
            dynamicLightSpaceMatrix_[col * 4 + row] = sum;
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
