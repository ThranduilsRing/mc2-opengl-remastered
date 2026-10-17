// tools/asset_viewer/MeshPreview3D.cpp
// Renders a loaded TGL prop on a turntable orbit camera with a simple lit shader.
// Vulkan: one offscreen pass per draw() into a private target (toolvk::Pass).
#include "MeshPreview3D.h"
#include "TglMeshLoader.h"
#include "imgui.h"
#include "tool_vk_gpu.h"
#include <cmath>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Math helpers (copied from MaterialPreviewPBR for self-containment)
// ---------------------------------------------------------------------------
static void perspective(float fovyRad, float aspect, float zn, float zf, float m[16]) {
    float f = 1.0f / std::tan(fovyRad * 0.5f);
    for (int i = 0; i < 16; i++) m[i] = 0;
    m[0] = f / aspect; m[5] = f;
    m[10] = zf / (zn - zf); m[11] = -1.0f;        // Vulkan [0,1] depth
    m[14] = (zf * zn) / (zn - zf);
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

// Identity 4×4 (column-major).
static void identity4(float m[16]) {
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// Build column-major rotation matrix from Euler angles in degrees.
// Order: Rx * Ry * Rz (X applied first, then Y, then Z) — i.e. vertices are
// first rotated by X, then by Y, then by Z.
// The matrix is ROTATION ONLY (no translation/scale).
static void eulerRotMatrix(float xDeg, float yDeg, float zDeg, float m[16]) {
    const float pi = 3.14159265f;
    float rx = xDeg * pi / 180.0f;
    float ry = yDeg * pi / 180.0f;
    float rz = zDeg * pi / 180.0f;

    float cx = std::cos(rx), sx = std::sin(rx);
    float cy = std::cos(ry), sy = std::sin(ry);
    float cz = std::cos(rz), sz = std::sin(rz);

    // Rx (column-major):
    float Rx[16]; identity4(Rx);
    Rx[5]=cx; Rx[9]=-sx;
    Rx[6]=sx; Rx[10]= cx;

    // Ry (column-major):
    float Ry[16]; identity4(Ry);
    Ry[0]=cy; Ry[8]=sy;
    Ry[2]=-sy;Ry[10]=cy;

    // Rz (column-major):
    float Rz[16]; identity4(Rz);
    Rz[0]=cz; Rz[4]=-sz;
    Rz[1]=sz; Rz[5]= cz;

    // Combined: Rz * Ry * Rx   (so vertex sees Rx first, then Ry, then Rz)
    float tmp[16];
    mul4(Ry, Rx, tmp);
    mul4(Rz, tmp, m);
}

// Build T(center) * R * T(-center) — rotate about bounds center.
static void rotateAboutCenter(const float cx, const float cy, const float cz,
                               float xDeg, float yDeg, float zDeg, float m[16]) {
    float R[16];
    eulerRotMatrix(xDeg, yDeg, zDeg, R);
    // Pre-apply T(-center) by shifting the translation column:
    // M = T(c) * R * T(-c)
    // Column-major: column 3 = R * (-c) + c
    // = (R[12..15] entries of R are 0,0,0,1 since R is pure rotation)
    // So result column 3 (index 12,13,14,15):
    //   tx = R[0]*(-cx) + R[4]*(-cy) + R[8]*(-cz)  + cx
    //   ty = R[1]*(-cx) + R[5]*(-cy) + R[9]*(-cz)  + cy
    //   tz = R[2]*(-cx) + R[6]*(-cy) + R[10]*(-cz) + cz
    //   tw = 1
    std::memcpy(m, R, 64);  // copy rotation part
    m[12] = -(R[0]*cx + R[4]*cy + R[8]*cz)  + cx;
    m[13] = -(R[1]*cx + R[5]*cy + R[9]*cz)  + cy;
    m[14] = -(R[2]*cx + R[6]*cy + R[10]*cz) + cz;
    m[15] = 1.0f;
}

// Shaders: shaders_vk/mesh_lit.{vert,frag} (N.L Lambert x albedo + ambient, gamma encode), compiled to SPIR-V at build time.
namespace {
// std140, mirrors the UBO in mesh_lit.vert / mesh_lit.frag.
struct MeshUbo {
    float viewProj[16];
    float model[16];
    float cameraPos[4];
    float lightDir[4];
    int32_t hasAlbedo[4];
};
static_assert(sizeof(MeshUbo) == 64 + 64 + 16 + 16 + 16, "MeshUbo std140 layout");
} // namespace

// ---------------------------------------------------------------------------
// MeshPreview3D lifecycle
// ---------------------------------------------------------------------------
MeshPreview3D::MeshPreview3D() = default;

MeshPreview3D::~MeshPreview3D() {
    mesh_.destroy();
    destroyFbo();
    toolvk::destroyPipeline(pipe_);
}

void MeshPreview3D::destroyFbo() {
    toolvk::destroyTarget(target_);
    fboComplete_ = false;
}

void MeshPreview3D::ensureGL(int w, int h) {
    if (!glReady_) {
        glReady_ = true;
        toolvk::PipelineDesc d;
        d.vertSpv = "mesh_lit.vert.spv";
        d.fragSpv = "mesh_lit.frag.spv";
        d.stride = sizeof(MeshVertex);
        d.attrs = {{0, 106 /*R32G32B32_SFLOAT*/, 0}, {1, 106, 12}, {2, 103 /*R32G32_SFLOAT*/, 24}};
        d.samplers = 1;
        d.cullBack = false;   // some MC2 props have inverted winding; render both sides
        backendOk_ = toolvk::createPipeline(pipe_, d);
        if (!backendOk_) std::fprintf(stderr, "[MeshPreview3D] pipeline creation failed\n");
    }

    if (w != target_.w || h != target_.h || !target_.impl) {
        fboComplete_ = toolvk::createTarget(target_, w, h);
        if (!fboComplete_) std::fprintf(stderr, "[MeshPreview3D] target creation failed\n");
    }
}

// ---------------------------------------------------------------------------
// setSource: load mesh from TglMeshLoader and upload to GPU
// ---------------------------------------------------------------------------
void MeshPreview3D::setSource(const std::string& tglName) {
    errorMsg_.clear();
    tglName_ = tglName;

    if (!TglMeshLoader::ensureFastFile(deployDir_.c_str())) {
        errorMsg_ = "FastFile init failed (deployDir: " + deployDir_ + ")";
        return;
    }

    MeshData md = TglMeshLoader::loadMesh(tglName);
    if (!md.ok) {
        errorMsg_ = md.error;
        return;
    }

    mesh_.destroy();
    mesh_.upload(md, deployDir_, tier_);

    if (!mesh_.valid()) {
        errorMsg_ = "MeshGpu upload produced no submeshes";
        return;
    }

    // Auto-frame: dist = 1.6 * radius; center at bounds center.
    const float* bmin = mesh_.bmin();
    const float* bmax = mesh_.bmax();
    float dx = bmax[0] - bmin[0];
    float dy = bmax[1] - bmin[1];
    float dz = bmax[2] - bmin[2];
    float radius = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
    if (radius < 0.01f) radius = 0.01f;
    dist_     = 1.6f * radius;
    center_[0] = 0.5f * (bmin[0] + bmax[0]);
    center_[1] = 0.5f * (bmin[1] + bmax[1]);
    center_[2] = 0.5f * (bmin[2] + bmax[2]);
}

// ---------------------------------------------------------------------------
// setMeshData: upload an arbitrary MeshData (GLB override) — untextured preview.
// Mirrors setSource's exact upload call; textures won't resolve (expected).
// ---------------------------------------------------------------------------
void MeshPreview3D::setMeshData(const MeshData& md) {
    if (!md.ok) {
        errorMsg_ = md.error.empty() ? "bad mesh" : md.error;
        return;
    }
    errorMsg_.clear();
    tglName_.clear();

    mesh_.destroy();
    mesh_.upload(md, deployDir_, tier_);

    if (!mesh_.valid()) {
        errorMsg_ = "MeshGpu upload produced no submeshes";
        return;
    }

    // Frame camera from MeshData bounds (same formula as setSource).
    float dx = md.bmax[0] - md.bmin[0];
    float dy = md.bmax[1] - md.bmin[1];
    float dz = md.bmax[2] - md.bmin[2];
    float radius = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
    if (radius < 0.01f) radius = 0.01f;
    dist_      = 1.6f * radius;
    center_[0] = 0.5f * (md.bmin[0] + md.bmax[0]);
    center_[1] = 0.5f * (md.bmin[1] + md.bmax[1]);
    center_[2] = 0.5f * (md.bmin[2] + md.bmax[2]);
}

// ---------------------------------------------------------------------------
// buildViewProj: orbit camera centered at center_
// ---------------------------------------------------------------------------
void MeshPreview3D::buildViewProj(int w, int h, const float center[3],
                                   float out[16], float camPosOut[3]) const {
    float cp = std::cos(pitch_), sp = std::sin(pitch_);
    float cy = std::cos(yaw_),   sy = std::sin(yaw_);
    float eye[3] = {
        center[0] + dist_ * cp * sy,
        center[1] + dist_ * sp,
        center[2] + dist_ * cp * cy
    };
    float up[3] = {0.0f, 1.0f, 0.0f};
    float view[16], proj[16];
    lookAt(eye, center, up, view);
    perspective(0.8f, (float)w / (float)h, 0.01f, 500.0f, proj);
    mul4(proj, view, out);
    camPosOut[0] = eye[0]; camPosOut[1] = eye[1]; camPosOut[2] = eye[2];
}

// ---------------------------------------------------------------------------
// renderScene: one offscreen pass into target_.
// ---------------------------------------------------------------------------
void MeshPreview3D::renderScene(int w, int h) {
    toolvk::Pass pass;
    const float clear[4] = {0.10f, 0.11f, 0.13f, 1.0f};
    if (!pass.begin(target_, clear)) return;
    if (mesh_.valid() && pipe_.impl) {
        MeshUbo u{};
        float cam[3];
        buildViewProj(w, h, center_, u.viewProj, cam);
        // T(center)*R*T(-center) so rotation pivots at the bounds center.
        rotateAboutCenter(center_[0], center_[1], center_[2], modelRotDeg_[0], modelRotDeg_[1], modelRotDeg_[2], u.model);
        std::memcpy(u.cameraPos, cam, 12);
        std::memcpy(u.lightDir, lightDir_, 12);
        pass.bind(pipe_);
        for (const GpuSubMesh& gs : mesh_.subs()) {
            if (gs.isSpotlight && !showLights_) continue;
            u.hasAlbedo[0] = gs.albedo != 0 ? 1 : 0;
            pass.draw(&u, sizeof u, {gs.albedo}, gs.vb, gs.ib, (uint32_t)gs.indexCount);
        }
    }
    pass.end();
}

// ---------------------------------------------------------------------------
// draw (ImGui path)
// ---------------------------------------------------------------------------
void MeshPreview3D::draw(const ImVec2& availableSize) {
    // -----------------------------------------------------------------------
    // Controls strip (parity with Materials mode)
    // -----------------------------------------------------------------------
    ImGui::SeparatorText("View");
    ImGui::SliderFloat("Orbit yaw",   &yaw_,   -3.14159f, 3.14159f);
    ImGui::SliderFloat("Orbit pitch", &pitch_, -1.5f, 1.5f);
    ImGui::SliderFloat("Zoom",        &dist_,   0.05f, 50.0f);
    // Model rotation: Euler X/Y/Z degrees; default -90 X stands props upright.
    // Rotation order applied to vertices: Rx first, then Ry, then Rz.
    ImGui::SliderFloat3("Model rot (deg)", modelRotDeg_, -180.0f, 180.0f);
    ImGui::SeparatorText("Light");
    ImGui::SliderFloat3("Light dir", lightDir_, -1.0f, 1.0f);

    // -----------------------------------------------------------------------
    // Texture resolution swap
    // -----------------------------------------------------------------------
    ImGui::SeparatorText("Texture");
    {
        static const int kTiers[] = { 128, 256, 512, 1024 };
        ImGui::Text("Resolution:");
        ImGui::SameLine();
        for (int ti = 0; ti < 4; ++ti) {
            int t = kTiers[ti];
            bool active = (tier_ == t);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            char lbl[16];
            std::snprintf(lbl, sizeof(lbl), "%d##tier", t);
            if (ImGui::Button(lbl) && !active) {
                tier_ = t;
                mesh_.reloadAlbedo(deployDir_, tier_);
            }
            if (active) ImGui::PopStyleColor();
            if (ti < 3) ImGui::SameLine();
        }
    }
    ImGui::Checkbox("Show lights", &showLights_);

    if (ImGui::Button("Reset view")) {
        yaw_   = 0.6f;
        pitch_ = 0.35f;
        modelRotDeg_[0] = -90.0f;
        modelRotDeg_[1] =   0.0f;
        modelRotDeg_[2] =   0.0f;
        showLights_ = false;
        // Re-frame dist_ from current bounds (same formula as setSource).
        const float* bmin = mesh_.bmin();
        const float* bmax = mesh_.bmax();
        float dx = bmax[0] - bmin[0];
        float dy = bmax[1] - bmin[1];
        float dz = bmax[2] - bmin[2];
        float radius = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
        if (radius < 0.01f) radius = 0.01f;
        dist_ = 1.6f * radius;
        // Reset tier to default and reload albedo at 512.
        if (tier_ != 512) {
            tier_ = 512;
            mesh_.reloadAlbedo(deployDir_, tier_);
        }
    }
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
        "Local preview (not exact MC2 shader)");
    ImGui::Separator();

    // Subtract controls height from available size for the 3D viewport.
    ImVec2 vpSize = availableSize;
    float usedH = ImGui::GetCursorPos().y - ImGui::GetWindowContentRegionMin().y;
    if (usedH > 0.0f && vpSize.y > usedH + 16.0f)
        vpSize.y -= usedH;

    int w = (int)vpSize.x, h = (int)vpSize.y;
    if (w < 16) w = 16; if (h < 16) h = 16;
    ensureGL(w, h);

    if (!errorMsg_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
            "Model load error: %s", errorMsg_.c_str());
        return;
    }

    if (!backendOk_ || !fboComplete_) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
            "Model preview unavailable (pipeline or target init failed). See stderr.");
        return;
    }

    renderScene(w, h);

    ImGui::Image((ImTextureID)toolvk::imguiTex(target_.colorTex),
                 ImVec2((float)w, (float)h),
                 ImVec2(0, 0), ImVec2(1, 1));

    // -----------------------------------------------------------------------
    // Mouse orbit + zoom (active while hovered over the Image above)
    // -----------------------------------------------------------------------
    if (ImGui::IsItemHovered()) {
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            yaw_   += io.MouseDelta.x * 0.01f;
            if (yaw_ >  3.14159f) yaw_ -= 6.28318f;     // wrap to slider range [-pi,pi]
            if (yaw_ < -3.14159f) yaw_ += 6.28318f;
            pitch_ += io.MouseDelta.y * 0.01f;
            const float lim = 1.55f;
            if (pitch_ >  lim) pitch_ =  lim;
            if (pitch_ < -lim) pitch_ = -lim;
        }
        if (io.MouseWheel != 0.0f) {
            dist_ *= (1.0f - io.MouseWheel * 0.1f);
            if (dist_ < 0.05f) dist_ = 0.05f;
        }
    }
}
