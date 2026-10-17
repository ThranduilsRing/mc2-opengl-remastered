// tools/asset_viewer/StandaloneSceneStubs.cpp
#include "StandaloneSceneStubs.h"
#include <GL/glew.h>
#include <cstdint>

namespace {
struct GpuInstance { float modelMatrix[16]; uint32_t typeID, firstColorOffset, flags, lightDataIndex;
                     float aRGBHighlight[4]; float fogRGB[4]; };
struct GpuPerType  { float hotPink[4], hotYellow[4], hotGreen[4]; };
struct GpuObjectLights {            // MUST match lighting.hglsl ObjectLights (std430)
    float   light_to_world[16][16]; // mat4[16]
    float   light_dir[16][4];
    float   light_color[16][4];
    float   light_falloff[16][4];
    int32_t numLights[4];           // ivec4
};
}

void StandaloneSceneStubs::build(const float dir[3], const float col[3], float ambient) {
    GpuInstance inst{};
    for (int i=0;i<16;++i) inst.modelMatrix[i] = (i%5==0)?1.0f:0.0f;  // identity; lightDataIndex 0
    GpuObjectLights L{};
    L.numLights[0] = 2;                                  // [0]=AMBIENT, [1]=INFINITE
    L.light_color[0][0]=ambient; L.light_color[0][1]=ambient; L.light_color[0][2]=ambient;
    L.light_dir[0][3]=0;                                 // type AMBIENT (w=0)
    L.light_dir[1][0]=dir[0]; L.light_dir[1][1]=dir[1]; L.light_dir[1][2]=dir[2];
    L.light_dir[1][3]=1;                                 // type INFINITE (w=1)
    L.light_color[1][0]=col[0]; L.light_color[1][1]=col[1]; L.light_color[1][2]=col[2];
    GpuPerType pt{}; uint32_t color0 = 0xFFFFFFFFu;
    auto mk=[&](unsigned& b, const void* d, GLsizeiptr n){ glGenBuffers(1,&b);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER,b); glBufferData(GL_SHADER_STORAGE_BUFFER,n,d,GL_STATIC_DRAW); };
    mk(instancesSsbo_, &inst, sizeof inst);
    mk(colorsSsbo_,    &color0, sizeof color0);
    mk(perTypeSsbo_,   &pt,   sizeof pt);
    mk(lightsSsbo_,    &L,    sizeof L);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glGenTextures(1, &shadowTex_);                       // unused in minimal config; future seam
    glBindTexture(GL_TEXTURE_2D, shadowTex_);
    float one = 1.0f;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 1, 1, 0, GL_DEPTH_COMPONENT, GL_FLOAT, &one);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    built_ = true;
}

void StandaloneSceneStubs::bind(const float modelMatrix[16]) const {
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, instancesSsbo_);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(float)*16, modelMatrix);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, instancesSsbo_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, colorsSsbo_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, perTypeSsbo_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 20, lightsSsbo_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void StandaloneSceneStubs::destroy() {
    unsigned b[4] = {instancesSsbo_, colorsSsbo_, perTypeSsbo_, lightsSsbo_};
    glDeleteBuffers(4, b);
    if (shadowTex_) glDeleteTextures(1, &shadowTex_);
    instancesSsbo_=colorsSsbo_=perTypeSsbo_=lightsSsbo_=shadowTex_=0; built_=false;
}
