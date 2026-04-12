#ifndef GOS_POSTPROCESS_H
#define GOS_POSTPROCESS_H

#include "utils/gl_utils.h"

struct glsl_program;

class gosPostProcess {
public:
    gosPostProcess();
    ~gosPostProcess();

    void init(int w, int h);
    void destroy();
    void resize(int w, int h);

    // Bind the HDR scene FBO so all rendering goes into it
    void beginScene();

    // Composite the HDR scene onto the default framebuffer via fullscreen quad
    void endScene();

    void renderSkybox(float sunDirX, float sunDirY, float sunDirZ);

    void runBloom();

    // Shadow mapping
    void initShadows();
    void destroyShadows();
    GLuint getShadowTexture() const { return shadowDepthTex_; }
    const float* getLightSpaceMatrix() const { return staticLightSpaceMatrix_; }
    GLuint getShadowFBO() const { return shadowFBO_; }
    void beginShadowPass();
    void endShadowPass();
    bool shadowsEnabled_;

    // Shadow debug overlay
    bool showShadowDebug_;        // master toggle for debug overlay
    int shadowDebugMode_;         // 0=static, 1=dynamic
    void drawShadowDebugOverlay();

    // Static world-fixed shadows: render once at map load
    void buildStaticLightMatrix(float sunDirX, float sunDirY, float sunDirZ,
                                float mapHalfExtent);
    bool staticShadowsRendered() const { return staticShadowsRendered_; }
    void markStaticShadowsRendered() { staticShadowsRendered_ = true; }
    void setMapHalfExtent(float extent) { mapHalfExtent_ = extent; }
    float getMapHalfExtent() const { return mapHalfExtent_; }

    // Dynamic object shadows: camera-centered, re-rendered every frame
    void initDynamicShadows();
    void destroyDynamicShadows();
    void buildDynamicLightMatrix(float sunDirX, float sunDirY, float sunDirZ,
                                 float camX, float camY, float camZ);
    GLuint getDynamicShadowTexture() const { return dynShadowDepthTex_; }
    GLuint getDynamicShadowFBO() const { return dynShadowFBO_; }
    const float* getDynamicLightSpaceMatrix() const { return dynamicLightSpaceMatrix_; }
    int getDynamicShadowMapSize() const { return dynShadowMapSize_; }

    // Toggles and parameters
    float exposure_;
    bool bloomEnabled_;
    bool fxaaEnabled_;
    bool tonemapEnabled_;
    float bloomIntensity_;
    float bloomThreshold_;

private:
    void createFBOs(int w, int h);
    void destroyFBOs();
    void createFullscreenQuad();
    void destroyFullscreenQuad();

    // Scene FBO (full resolution, HDR)
    GLuint sceneFBO_;
    GLuint sceneColorTex_;
    GLuint sceneDepthRBO_;

    // Bloom ping-pong FBOs (half resolution)
    GLuint bloomFBO_[2];
    GLuint bloomColorTex_[2];

    // Fullscreen quad
    GLuint quadVAO_;
    GLuint quadVBO_;

    // Composite shader
    glsl_program* compositeProg_;

    // Skybox
    glsl_program* skyboxProg_;

    // Bloom shaders
    glsl_program* bloomThresholdProg_;
    glsl_program* bloomBlurProg_;

    // Dimensions
    int width_;
    int height_;

    bool initialized_;

    // Shadow map
    GLuint shadowFBO_;
    GLuint shadowDepthTex_;
    GLuint shadowDummyColorTex_;  // AMD needs a color attachment for rasterization
    glsl_program* shadowDepthProg_;
    int shadowMapSize_;
    int savedViewport_[4];
    float staticLightSpaceMatrix_[16]; // world-fixed ortho, built once at map load
    bool staticShadowsRendered_;       // true after first (and only) shadow render
    float mapHalfExtent_;              // half the map size in world units

    // Dynamic object shadow FBO (1024x1024, camera-centered, per-frame)
    GLuint dynShadowFBO_;
    GLuint dynShadowDepthTex_;
    GLuint dynShadowDummyColorTex_;
    int dynShadowMapSize_;
    float dynamicLightSpaceMatrix_[16];
    glsl_program* shadowDebugProg_;
};

gosPostProcess* getGosPostProcess();

#endif // GOS_POSTPROCESS_H
