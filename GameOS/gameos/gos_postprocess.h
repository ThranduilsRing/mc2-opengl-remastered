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
    void updateLightMatrix(float sunDirX, float sunDirY, float sunDirZ,
                           float camX, float camY, float camZ, float radius);
    GLuint getShadowTexture() const { return shadowDepthTex_; }
    const float* getLightSpaceMatrix() const { return lightSpaceMatrix_; }
    bool shadowsEnabled_;

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
    glsl_program* shadowDepthProg_;
    int shadowMapSize_;
    float lightSpaceMatrix_[16];
};

gosPostProcess* getGosPostProcess();

#endif // GOS_POSTPROCESS_H
