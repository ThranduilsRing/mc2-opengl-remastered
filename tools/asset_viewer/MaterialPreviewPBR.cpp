// tools/asset_viewer/MaterialPreviewPBR.cpp
#include "MaterialPreviewPBR.h"
#include "imgui.h"
#include "tool_vk_gpu.h"
#include <cmath>
#include <vector>

static void perspective(float fovyRad, float aspect, float zn, float zf, float m[16]) {
    float f = 1.0f / std::tan(fovyRad * 0.5f);
    for (int i=0;i<16;i++) m[i]=0;
    m[0]=f/aspect; m[5]=f; m[10]=zf/(zn-zf); m[11]=-1.0f; m[14]=(zf*zn)/(zn-zf);   // Vulkan [0,1] depth
}
static void lookAt(const float eye[3], const float ctr[3], const float up[3], float m[16]) {
    float f[3]={ctr[0]-eye[0],ctr[1]-eye[1],ctr[2]-eye[2]};
    float fl=std::sqrt(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]); f[0]/=fl;f[1]/=fl;f[2]/=fl;
    float s[3]={f[1]*up[2]-f[2]*up[1], f[2]*up[0]-f[0]*up[2], f[0]*up[1]-f[1]*up[0]};
    float sl=std::sqrt(s[0]*s[0]+s[1]*s[1]+s[2]*s[2]); s[0]/=sl;s[1]/=sl;s[2]/=sl;
    float u[3]={s[1]*f[2]-s[2]*f[1], s[2]*f[0]-s[0]*f[2], s[0]*f[1]-s[1]*f[0]};
    m[0]=s[0]; m[4]=s[1]; m[8]=s[2];  m[12]=-(s[0]*eye[0]+s[1]*eye[1]+s[2]*eye[2]);
    m[1]=u[0]; m[5]=u[1]; m[9]=u[2];  m[13]=-(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]);
    m[2]=-f[0];m[6]=-f[1];m[10]=-f[2];m[14]=(f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]);
    m[3]=0;m[7]=0;m[11]=0;m[15]=1;
}
static void mul4(const float a[16], const float b[16], float o[16]) {  // o = a*b (column-major)
    for (int c=0;c<4;c++) for (int r=0;r<4;r++) {
        o[c*4+r] = a[0*4+r]*b[c*4+0] + a[1*4+r]*b[c*4+1] + a[2*4+r]*b[c*4+2] + a[3*4+r]*b[c*4+3];
    }
}

MaterialPreviewPBR::MaterialPreviewPBR() : backend_(std::make_unique<LocalPbrMaterialBackend>()) {}
MaterialPreviewPBR::~MaterialPreviewPBR() {
    if (slots_.baseColor) toolvk::destroyTexture(slots_.baseColor);
    if (slots_.normal)    toolvk::destroyTexture(slots_.normal);
    if (slots_.orm)       toolvk::destroyTexture(slots_.orm);
    if (slots_.emissive)  toolvk::destroyTexture(slots_.emissive);
    destroyFbo();
    if (glReady_) { mesh_.destroyGL(); backend_->shutdown(); }
}

void MaterialPreviewPBR::setSource(const std::string& path) { fitPath_ = path; /* Task 8 */ }

void MaterialPreviewPBR::setSlotTexture(MaterialSlotKind kind, uint32_t tex) {
    uint32_t* dst = nullptr;
    switch (kind) {
      case MaterialSlotKind::BaseColor: dst = &slots_.baseColor; break;
      case MaterialSlotKind::Normal:    dst = &slots_.normal;    break;
      case MaterialSlotKind::Orm:       dst = &slots_.orm;       break;
      case MaterialSlotKind::Emissive:  dst = &slots_.emissive;  break;
    }
    if (dst) { if (*dst) toolvk::destroyTexture(*dst); *dst = tex; }
}

void MaterialPreviewPBR::ensureGL(int w, int h) {
    if (!glReady_) {
        backendOk_ = backend_->init();                 // review fix MAJOR 6: capture result
        mesh_.generate(1.0f, 48, 96); mesh_.uploadGL();
        glReady_ = true;
    }
    if (w != target_.w || h != target_.h || !target_.impl) {
        fboComplete_ = toolvk::createTarget(target_, w, h);
        if (!fboComplete_) fprintf(stderr, "[MaterialPreviewPBR] target creation failed\n");
    }
}

void MaterialPreviewPBR::destroyFbo() { toolvk::destroyTarget(target_); fboComplete_ = false; }

void MaterialPreviewPBR::buildViewProj(int w, int h, float out[16], float camPosOut[3]) const {
    float cp = std::cos(pitch_), sp = std::sin(pitch_), cy = std::cos(yaw_), sy = std::sin(yaw_);
    float eye[3] = { dist_*cp*sy, dist_*sp, dist_*cp*cy };
    float ctr[3] = {0,0,0}, up[3] = {0,1,0};
    float view[16], proj[16];
    lookAt(eye, ctr, up, view);
    perspective(0.8f, (float)w/(float)h, 0.05f, 50.0f, proj);
    mul4(proj, view, out);
    camPosOut[0]=eye[0]; camPosOut[1]=eye[1]; camPosOut[2]=eye[2];
}

// Shared core: render into fbo_ with full GL-state containment.
static void renderContained(MaterialRenderBackend* backend, SphereMesh* mesh, toolvk::Target& target, int w, int h,
                            const float viewProj[16], const float camPos[3], const float lightDir[3]) {
    toolvk::Pass pass;
    const float clear[4] = {0.10f, 0.11f, 0.13f, 1.0f};
    if (!pass.begin(target, clear)) return;
    RenderInputs in{};
    in.mesh = mesh;
    for (int i=0;i<16;i++) in.viewProj[i] = viewProj[i];
    for (int i=0;i<3;i++) { in.cameraPosWorld[i]=camPos[i]; in.lightDirWorld[i]=lightDir[i]; }
    in.lightColor[0]=in.lightColor[1]=in.lightColor[2]=3.0f;
    in.ambient[0]=in.ambient[1]=in.ambient[2]=0.15f;
    in.fboWidth=w; in.fboHeight=h;
    backend->render(in, pass);
    pass.end();
}

void MaterialPreviewPBR::draw(const ImVec2& availableSize) {
    int w = (int)availableSize.x, h = (int)availableSize.y;
    if (w < 16) w = 16; if (h < 16) h = 16;
    ensureGL(w, h);
    if (!backendOk_ || !fboComplete_) {   // review fix MAJOR 6: surface init failure instead of a misleading blank
        ImGui::TextColored(ImVec4(1.0f,0.4f,0.3f,1.0f),
            "Material preview unavailable (pipeline or target init failed). See stderr.");
        return;
    }
    backend_->setMaterial(slots_);
    float vp[16], cam[3];
    buildViewProj(w, h, vp, cam);
    renderContained(backend_.get(), &mesh_, target_, w, h, vp, cam, lightDir_);

    ImGui::Image((ImTextureID)toolvk::imguiTex(target_.colorTex), ImVec2((float)w, (float)h), ImVec2(0,0), ImVec2(1,1));
    if (backend_->isApproximate())
        ImGui::TextColored(ImVec4(1.0f,0.7f,0.2f,1.0f),
            "Preview mode: Local PBR approximation, not exact MC2 shader.");
}
