// tools/asset_viewer/ModelPreviewEngineShader.cpp
// Backend-A v2: renders a static prop using the REAL engine static_prop.{vert,frag}
// compiled in a minimal config with standalone scene-binding stubs.
// HARD CONSTRAINT: never edits any shader file. Fails open on any error.
#include "ModelPreviewEngineShader.h"
#include "TglMeshLoader.h"
#include "ShaderIncludeResolver.h"
#include "imgui.h"
#include <GL/glew.h>
#include <cmath>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Math helpers (column-major; same conventions as MeshPreview3D)
// ---------------------------------------------------------------------------
static void perspectiveReverseZ(float fovyRad, float aspect, float zn, float zf, float m[16]) {
    // Reverse-Z: near -> depth 1, far -> depth 0; for GL_GEQUAL + glClearDepth(0).
    // Standard formula with z-range remapped: depth = (zn/z) remapped to [1,0].
    // Using the infinite reverse-Z approach: f = 1/(tan(fov/2)); m[10]=0, m[14]=zn.
    float f = 1.0f / std::tan(fovyRad * 0.5f);
    for (int i = 0; i < 16; i++) m[i] = 0;
    m[0]  = f / aspect;
    m[5]  = f;
    m[10] = 0.0f;          // reverse-Z infinite far
    m[11] = -1.0f;
    m[14] = zn;            // reverse-Z: maps zn to clip-depth=1 after perspective divide
}

static void lookAt(const float eye[3], const float ctr[3], const float up[3], float m[16]) {
    float f[3] = { ctr[0]-eye[0], ctr[1]-eye[1], ctr[2]-eye[2] };
    float fl = std::sqrt(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]);
    f[0]/=fl; f[1]/=fl; f[2]/=fl;
    float s[3] = { f[1]*up[2]-f[2]*up[1], f[2]*up[0]-f[0]*up[2], f[0]*up[1]-f[1]*up[0] };
    float sl = std::sqrt(s[0]*s[0]+s[1]*s[1]+s[2]*s[2]);
    s[0]/=sl; s[1]/=sl; s[2]/=sl;
    float u[3] = { s[1]*f[2]-s[2]*f[1], s[2]*f[0]-s[0]*f[2], s[0]*f[1]-s[1]*f[0] };
    m[0]=s[0]; m[4]=s[1]; m[8]=s[2];  m[12]=-(s[0]*eye[0]+s[1]*eye[1]+s[2]*eye[2]);
    m[1]=u[0]; m[5]=u[1]; m[9]=u[2];  m[13]=-(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]);
    m[2]=-f[0];m[6]=-f[1];m[10]=-f[2];m[14]=(f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]);
    m[3]=0; m[7]=0; m[11]=0; m[15]=1;
}

static void mul4(const float a[16], const float b[16], float o[16]) {
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) {
        o[c*4+r] = a[0*4+r]*b[c*4+0] + a[1*4+r]*b[c*4+1]
                 + a[2*4+r]*b[c*4+2] + a[3*4+r]*b[c*4+3];
    }
}

static void identity4(float m[16]) {
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void eulerRotMatrix(float xDeg, float yDeg, float zDeg, float m[16]) {
    const float pi = 3.14159265f;
    float rx = xDeg * pi / 180.0f, ry = yDeg * pi / 180.0f, rz = zDeg * pi / 180.0f;
    float cx = std::cos(rx), sx = std::sin(rx);
    float cy = std::cos(ry), sy = std::sin(ry);
    float cz = std::cos(rz), sz = std::sin(rz);
    float Rx[16]; identity4(Rx); Rx[5]=cx; Rx[9]=-sx; Rx[6]=sx; Rx[10]=cx;
    float Ry[16]; identity4(Ry); Ry[0]=cy; Ry[8]=sy;  Ry[2]=-sy;Ry[10]=cy;
    float Rz[16]; identity4(Rz); Rz[0]=cz; Rz[4]=-sz; Rz[1]=sz; Rz[5]=cz;
    float tmp[16]; mul4(Ry, Rx, tmp); mul4(Rz, tmp, m);
}

// ---------------------------------------------------------------------------
// buildReverseZViewProj: orbit camera (reverse-Z, GL_GEQUAL)
// ---------------------------------------------------------------------------
void ModelPreviewEngineShader::buildReverseZViewProj(int w, int h,
                                                      float out[16],
                                                      float camPos[3]) const {
    float cp = std::cos(pitch_), sp = std::sin(pitch_);
    float cy = std::cos(yaw_),   sy = std::sin(yaw_);
    float eye[3] = {
        center_[0] + dist_ * cp * sy,
        center_[1] + dist_ * sp,
        center_[2] + dist_ * cp * cy
    };
    float up[3] = {0.0f, 1.0f, 0.0f};
    float view[16], proj[16];
    lookAt(eye, center_, up, view);
    perspectiveReverseZ(0.8f, (float)w / (float)h, 0.1f, 1000.0f, proj);
    mul4(proj, view, out);
    camPos[0] = eye[0]; camPos[1] = eye[1]; camPos[2] = eye[2];
}

// ---------------------------------------------------------------------------
// ensureProgram: resolve+compile+link the engine shaders in minimal config
// ---------------------------------------------------------------------------
bool ModelPreviewEngineShader::ensureProgram() {
    if (triedProgram_) return report_.ok();
    triedProgram_ = true;

    report_.shaderRoot = shaderRoot_;
    report_.vertPath   = shaderRoot_ + "/static_prop.vert";
    report_.fragPath   = shaderRoot_ + "/static_prop.frag";
    report_.activeDefines =
        "(minimal: legacy lane; no MC2_USE_VIEW_UNIFORMS/MC2_COALESCE/"
        "MC2_STATICPROP_PBR_SLOTS/MC2_OBJECT_ID_BUFFER)";
    report_.textureMode          = "legacy sampler2D u_tex";
    report_.objectLightsStubActive = true;
    report_.shadowStubActive       = false;
    report_.fogDisabled            = true;

    // Define prefix matching the compile-contract (docs/asset-viewer-backend-a-shader-contract.md)
    static const char* kPrefix =
        "#version 430\n"
        "// Backend-A minimal config: legacy lane, no view-uniforms/coalesce/PBR-slots.\n";

    auto buildStage = [&](const char* file, GLenum type,
                          std::string& logOut,
                          std::vector<std::string>& incOut) -> unsigned {
        ShaderResolveResult r = ResolveShaderIncludes(shaderRoot_, file);
        incOut = r.includedFiles;
        if (!r.ok) {
            logOut = "resolve failed: " + r.error;
            if (!r.unresolved.empty()) logOut += " unresolved=" + r.unresolved.front();
            return 0;
        }
        std::string src = std::string(kPrefix) + r.source;
        unsigned sh = glCreateShader(type);
        const char* p = src.c_str();
        glShaderSource(sh, 1, &p, nullptr);
        glCompileShader(sh);
        GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        GLint len = 0; glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        if (len > 1) {
            std::string l(len, '\0');
            glGetShaderInfoLog(sh, len, nullptr, &l[0]);
            logOut = l;
        }
        if (!ok) { glDeleteShader(sh); return 0; }
        return sh;
    };

    std::string vlog, flog;
    std::vector<std::string> vinc, finc;
    unsigned vs = buildStage("static_prop.vert", GL_VERTEX_SHADER,   vlog, vinc);
    unsigned fs = buildStage("static_prop.frag", GL_FRAGMENT_SHADER, flog, finc);

    report_.compileOk = (vs != 0) && (fs != 0);
    report_.compileLog.clear();
    if (!vlog.empty()) report_.compileLog += "vert: " + vlog + "\n";
    if (!flog.empty()) report_.compileLog += "frag: " + flog + "\n";

    // Merge include lists
    for (auto& s : vinc) report_.includedFiles.push_back(s);
    for (auto& s : finc) report_.includedFiles.push_back(s);

    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        report_.lastError = "shader compile failed";
        return false;
    }

    unsigned prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint linkOk = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linkOk);
    GLint linkLen = 0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &linkLen);
    if (linkLen > 1) {
        std::string l(linkLen, '\0');
        glGetProgramInfoLog(prog, linkLen, nullptr, &l[0]);
        report_.linkLog = l;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);

    report_.linkOk = (linkOk != 0);
    if (!linkOk) {
        glDeleteProgram(prog);
        report_.lastError = "shader link failed";
        return false;
    }

    prog_ = prog;

    // Build scene UBO (binding 1, std140):
    // struct { float fogStart,fogFull,minHazeDistance,distanceFactor; vec4 cameraPos,fogColor,baseVertexColor; }
    // = 4 floats + 3 vec4 = 16+48 = 64 bytes
    struct SceneData {
        float fogStart, fogFull, minHazeDistance, distanceFactor; // 16 bytes
        float cameraPos[4];    // 16
        float fogColor[4];     // 16 (all zero = fog disabled)
        float baseVertexColor[4]; // 16
    };
    SceneData sd{};
    sd.fogStart = 1000.0f; sd.fogFull = 2000.0f;
    sd.fogColor[0] = sd.fogColor[1] = sd.fogColor[2] = sd.fogColor[3] = 0.0f;  // fog off
    sd.baseVertexColor[0] = sd.baseVertexColor[1] = sd.baseVertexColor[2] = sd.baseVertexColor[3] = 1.0f;
    glGenBuffers(1, &sceneUbo_);
    glBindBuffer(GL_UNIFORM_BUFFER, sceneUbo_);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(SceneData), &sd, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    // Build scene stubs once
    float lightDir[3] = { -0.4f, -0.7f, -0.5f };
    float lightCol[3] = { 1.0f, 1.0f, 1.0f };
    stubs_.build(lightDir, lightCol, 0.5f);
    stubsBuilt_ = true;

    return true;
}

// ---------------------------------------------------------------------------
// ensureFbo: MRT FBO with 2 color attachments + depth
// ---------------------------------------------------------------------------
void ModelPreviewEngineShader::ensureFbo(int w, int h) {
    if (fbo_ && fboW_ == w && fboH_ == h) return;

    // Delete old
    if (fbo_) {
        glDeleteFramebuffers(1, &fbo_); fbo_ = 0;
        glDeleteTextures(1, &color0_);  color0_ = 0;
        glDeleteTextures(1, &gbuffer1_); gbuffer1_ = 0;
        glDeleteRenderbuffers(1, &depthRbo_); depthRbo_ = 0;
    }

    fboW_ = w; fboH_ = h;
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);

    // Color attachment 0 (RGBA8)
    glGenTextures(1, &color0_);
    glBindTexture(GL_TEXTURE_2D, color0_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color0_, 0);

    // Color attachment 1 / GBuffer1 (RGBA8)
    glGenTextures(1, &gbuffer1_);
    glBindTexture(GL_TEXTURE_2D, gbuffer1_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, gbuffer1_, 0);

    // Depth renderbuffer
    glGenRenderbuffers(1, &depthRbo_);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRbo_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRbo_);

    // Enable MRT draw buffers
    GLenum drawBufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, drawBufs);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[ModelPreviewEngineShader] FBO incomplete: 0x%x\n", (unsigned)status);
        glDeleteFramebuffers(1, &fbo_); fbo_ = 0;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

// ---------------------------------------------------------------------------
// renderScene: stateless draw into fbo_, full save/restore
// ---------------------------------------------------------------------------
void ModelPreviewEngineShader::renderScene(int w, int h) {
    if (!fbo_ || !prog_ || !stubsBuilt_) return;

    // --- Save state ---
    GLint prevFbo = 0;
    GLint prevVp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevVp);
    GLboolean wasDepth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean wasCull  = glIsEnabled(GL_CULL_FACE);
    GLboolean wasBlend = glIsEnabled(GL_BLEND);
    GLint prevDepthFunc = GL_LESS, prevCullMode = GL_BACK;
    GLboolean prevDepthMask = GL_TRUE;
    GLfloat prevClear[4], prevClearDepth = 1.0f;
    glGetIntegerv(GL_DEPTH_FUNC,      &prevDepthFunc);
    glGetIntegerv(GL_CULL_FACE_MODE,  &prevCullMode);
    glGetFloatv(GL_COLOR_CLEAR_VALUE,  prevClear);
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &prevClearDepth);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);

    // --- Render ---
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, w, h);

    // StaticPropOpaque pipeline state (reverse-Z)
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);         // reverse-Z
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDisable(GL_BLEND);
    glClearDepth(0.0f);             // reverse-Z clear (NOT 1.0)
    glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (mesh_.valid()) {
        // Build MVP (reverse-Z)
        float mvp[16], camPos[3];
        buildReverseZViewProj(w, h, mvp, camPos);

        // Build model matrix: rotate about bounds center
        float modelMat[16]; identity4(modelMat);
        {
            float R[16];
            eulerRotMatrix(modelRotDeg_[0], modelRotDeg_[1], modelRotDeg_[2], R);
            // T(c) * R * T(-c)
            std::memcpy(modelMat, R, 64);
            modelMat[12] = -(R[0]*center_[0] + R[4]*center_[1] + R[8]*center_[2])  + center_[0];
            modelMat[13] = -(R[1]*center_[0] + R[5]*center_[1] + R[9]*center_[2])  + center_[1];
            modelMat[14] = -(R[2]*center_[0] + R[6]*center_[1] + R[10]*center_[2]) + center_[2];
            modelMat[15] = 1.0f;
        }

        // Update cameraPos in SceneUBO
        {
            // std140 layout: 4 floats (16B) then cameraPos vec4 at offset 16
            glBindBuffer(GL_UNIFORM_BUFFER, sceneUbo_);
            float camPosW[4] = {camPos[0], camPos[1], camPos[2], 1.0f};
            glBufferSubData(GL_UNIFORM_BUFFER, 16, sizeof(float)*4, camPosW);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);
        }
        // UBO binding 1
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, sceneUbo_);

        // SSBOs (Instances gets the model matrix)
        stubs_.bind(modelMat);

        glUseProgram(prog_);

        // Set all uniforms the contract lists to safe defaults
        auto setUniformLoc = [&](const char* name) { return glGetUniformLocation(prog_, name); };

        GLint locMVP = setUniformLoc("u_worldToClipGL");
        if (locMVP >= 0) glUniformMatrix4fv(locMVP, 1, GL_FALSE, mvp);

        GLint locTex = setUniformLoc("u_tex");
        if (locTex >= 0) glUniform1i(locTex, 0);   // unit 0

        GLint locFogV = setUniformLoc("u_fogValue");
        if (locFogV >= 0) glUniform1f(locFogV, 1.0f);  // fog value (fog is disabled via fogColor=0)

        GLint locMatFlags = setUniformLoc("u_materialFlags");
        if (locMatFlags >= 0) glUniform1i(locMatFlags, 0);

        GLint locDebugAddr = setUniformLoc("u_debugAddrMode");
        if (locDebugAddr >= 0) glUniform1i(locDebugAddr, 0);

        GLint locDebugMat = setUniformLoc("u_debugMaterialMode");
        if (locDebugMat >= 0) glUniform1i(locDebugMat, 0);

        GLint locMaxVID = setUniformLoc("u_maxLocalVertexID");
        if (locMaxVID >= 0) glUniform1i(locMaxVID, 0);

        GLint locPacket = setUniformLoc("u_packetID");
        if (locPacket >= 0) glUniform1i(locPacket, 0);

        GLint locAmbient = setUniformLoc("u_ambientV1Strength");
        if (locAmbient >= 0) glUniform1f(locAmbient, 0.0f);

        GLint locIbl = setUniformLoc("u_iblShStrength");
        if (locIbl >= 0) glUniform1f(locIbl, 0.0f);

        // u_iblSh is an array uniform; set all 7 entries to zero
        for (int i = 0; i < 7; i++) {
            char name[32]; std::snprintf(name, sizeof(name), "u_iblSh[%d]", i);
            GLint loc = glGetUniformLocation(prog_, name);
            if (loc >= 0) {
                float zero[4] = {};
                glUniform4fv(loc, 1, zero);
            }
        }

        // Feed attrib 3 (a_localVertexID) and 4 (a_aRGBLight) as constant integer attribs.
        // These are not in the VBO; disabling the attrib array and setting a constant suffices.
        glDisableVertexAttribArray(3);
        glDisableVertexAttribArray(4);
        glVertexAttribI4ui(3, 0, 0, 0, 0);
        glVertexAttribI4ui(4, 0, 0, 0, 0);

        // Draw all submeshes; mesh_.draw() binds each submesh VAO + albedo on unit 0 + glDrawElements.
        mesh_.draw();

        glUseProgram(0);
    }

    // --- Restore state ---
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);

    if (wasDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthFunc(prevDepthFunc);
    glDepthMask(prevDepthMask);
    if (wasCull)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);
    glCullFace(prevCullMode);
    if (wasBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
    glClearDepth(prevClearDepth);
}

// ---------------------------------------------------------------------------
// setSource: load mesh from TglMeshLoader and upload to GPU
// ---------------------------------------------------------------------------
void ModelPreviewEngineShader::setSource(const std::string& tglName) {
    tglName_ = tglName;
    if (!TglMeshLoader::ensureFastFile(deployDir_.c_str())) return;
    MeshData md = TglMeshLoader::loadMesh(tglName);
    if (!md.ok) return;
    mesh_.destroy();
    mesh_.upload(md, deployDir_, 512);
    if (!mesh_.valid()) return;

    // Auto-frame
    const float* bmin = mesh_.bmin();
    const float* bmax = mesh_.bmax();
    float dx = bmax[0]-bmin[0], dy = bmax[1]-bmin[1], dz = bmax[2]-bmin[2];
    float radius = 0.5f * std::sqrt(dx*dx+dy*dy+dz*dz);
    if (radius < 0.01f) radius = 0.01f;
    dist_ = 1.6f * radius;
    center_[0] = 0.5f*(bmin[0]+bmax[0]);
    center_[1] = 0.5f*(bmin[1]+bmax[1]);
    center_[2] = 0.5f*(bmin[2]+bmax[2]);
}

// ---------------------------------------------------------------------------
// draw (ImGui path)
// ---------------------------------------------------------------------------
void ModelPreviewEngineShader::draw(const ImVec2& availableSize) {
    if (!ensureProgram()) {
        ImGui::TextDisabled("Backend-A unavailable; see Shader Contract");
        return;
    }

    int w = (int)availableSize.x, h = (int)availableSize.y;
    if (w < 16) w = 16; if (h < 16) h = 16;

    ensureFbo(w, h);
    if (!fbo_) {
        ImGui::TextDisabled("Backend-A: FBO init failed");
        return;
    }

    renderScene(w, h);
    ImGui::Image((ImTextureID)(intptr_t)color0_, ImVec2((float)w, (float)h),
                 ImVec2(0, 1), ImVec2(1, 0));
}

// ---------------------------------------------------------------------------
// renderToPixels: headless test hook
// ---------------------------------------------------------------------------
bool ModelPreviewEngineShader::renderToPixels(int w, int h, std::vector<uint8_t>& rgbaOut) {
    if (!ensureProgram()) return false;
    ensureFbo(w, h);
    if (!fbo_) return false;
    renderScene(w, h);
    rgbaOut.resize((size_t)w * h * 4);
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    // Read from COLOR_ATTACHMENT0
    GLenum readBuf = GL_COLOR_ATTACHMENT0;
    glReadBuffer(readBuf);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgbaOut.data());
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    return true;
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
ModelPreviewEngineShader::~ModelPreviewEngineShader() {
    mesh_.destroy();
    stubs_.destroy();
    if (prog_) { glDeleteProgram(prog_); prog_ = 0; }
    if (fbo_)  { glDeleteFramebuffers(1, &fbo_); fbo_ = 0; }
    if (color0_)  { glDeleteTextures(1, &color0_); color0_ = 0; }
    if (gbuffer1_){ glDeleteTextures(1, &gbuffer1_); gbuffer1_ = 0; }
    if (depthRbo_){ glDeleteRenderbuffers(1, &depthRbo_); depthRbo_ = 0; }
    if (sceneUbo_){ glDeleteBuffers(1, &sceneUbo_); sceneUbo_ = 0; }
}
