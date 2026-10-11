// CLUSTER-DEPTH-PYRAMID-NATIVE-1 — see gos_cluster_depth_pyramid.h for intent.

#include "gos_cluster_depth_pyramid.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "gos_gpu_sync.h"
#include "../../RenderCore/RenderResourceRegistry.h"  // REGISTRY-COMPUTE-IDS-1: ClusterDepthPyramid

#include <GL/glew.h>
#include <GL/gl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include "Gates/gate.h"
#include "mc2fmt.h"

namespace cluster_depth_pyramid {

namespace {

// --- Tile size -------------------------------------------------------------
// 32x32 = 1024 invocations, which is the GL 4.3 MINIMUM guarantee for
// GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, and well within the AMD RX 7900 XTX
// target's limits. We use 32 unconditionally on this target; a 16x16 fallback
// is documented as the safe floor for any device that reports < 1024
// invocations. We assert the target supports it at init and stick with 32.
constexpr int kTileSize = 32;

// --- State -----------------------------------------------------------------
bool   s_gateResolved = false;
bool   s_enabled      = false;
bool   s_verify       = false;
bool   s_plant        = false;

bool   s_shaderBad    = false;
GLuint s_program      = 0;
GLint  s_uDepthSize   = -1;

// Output RG32F tile image, sized to the tile grid. Reallocated when the scene
// dimensions change.
GLuint s_tileTex      = 0;
int    s_tileW        = 0;   // ceil(width / kTileSize)
int    s_tileH        = 0;
int    s_srcW         = 0;   // scene depth dims this tile tex was sized for
int    s_srcH         = 0;

bool   s_verifyDone   = false;  // one-shot parity check latch

// Image binding unit for the output (this pass owns unit 0 inside its dispatch).
constexpr GLuint kTileImageUnit = 0;
// Texture unit for the input depth sampler.
constexpr GLuint kDepthTexUnit  = 0;

char* loadTextFile(const char* fname) {
    FILE* f = fopen(fname, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return nullptr; }
    char* buf = new char[sz + 1];
    if ((long)fread(buf, 1, sz, f) != sz) { fclose(f); delete[] buf; return nullptr; }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

GLuint buildProgram() {
    char* fileSrc = loadTextFile("shaders/cluster_depth_pyramid.comp");
    if (!fileSrc) {
        MC2_DIAGFE(stderr, "[CLUSTER_DEPTH_PYRAMID v1] shader source not found: "
                        "shaders/cluster_depth_pyramid.comp\n");
        return 0;
    }

    // Project rule: no #version in the shader file — prepend it here, plus the
    // TILE_SIZE define so the GLSL local size matches the C++ dispatch exactly.
    const char* kVersion = "#version 430\n";
    std::string tileDef  = "#define TILE_SIZE " + std::to_string(kTileSize) + "\n";

    const char* strs[3] = { kVersion, tileDef.c_str(), fileSrc };

    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(sh, 3, strs, nullptr);
    glCompileShader(sh);
    delete[] fileSrc;

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint n = 0; glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &n);
        std::vector<char> log(n > 1 ? n : 1);
        glGetShaderInfoLog(sh, (GLsizei)log.size(), nullptr, log.data());
        MC2_DIAGFE(stderr, "[CLUSTER_DEPTH_PYRAMID v1] compile error:\n%s\n", log.data());
        glDeleteShader(sh);
        return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, sh);
    glLinkProgram(prog);
    glDeleteShader(sh);

    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint n = 0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &n);
        std::vector<char> log(n > 1 ? n : 1);
        glGetProgramInfoLog(prog, (GLsizei)log.size(), nullptr, log.data());
        MC2_DIAGFE(stderr, "[CLUSTER_DEPTH_PYRAMID v1] link error:\n%s\n", log.data());
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

void ensureTileTexture(int width, int height) {
    int tw = (width  + kTileSize - 1) / kTileSize;
    int th = (height + kTileSize - 1) / kTileSize;
    if (s_tileTex != 0 && tw == s_tileW && th == s_tileH) return;

    if (s_tileTex != 0) { glDeleteTextures(1, &s_tileTex); s_tileTex = 0; }

    // TEX-CLASS: render-target -- cluster/froxel depth tile (FBO attach)
    glGenTextures(1, &s_tileTex);
    glBindTexture(GL_TEXTURE_2D, s_tileTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG32F, tw, th);
    glBindTexture(GL_TEXTURE_2D, 0);

    s_tileW = tw; s_tileH = th; s_srcW = width; s_srcH = height;

    // REGISTRY-COMPUTE-IDS-1: register the live tile min/max texture
    // (observe-only metadata; never read by the draw path). Gated/default-OFF,
    // so this only fires when the cluster-depth-pyramid compute path runs.
    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::ClusterDepthPyramid;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.lifetime  = RenderCore::RenderResourceLifetime::FrameLocal;  // transient compute intermediate, per-frame
        d.format    = RenderCore::RenderResourceFormat::Unknown;  // RG32F (no enum case)
        d.debugName = "ClusterDepthPyramid";
        d.width     = static_cast<uint32_t>(tw);
        d.height    = static_cast<uint32_t>(th);
        d.glName    = static_cast<uint32_t>(s_tileTex);
        d.sizeBytes = static_cast<uint64_t>(tw) * th * 8;  // RG32F = 8 bytes/texel
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    MC2_DIAGFE(stderr, "[CLUSTER_DEPTH_PYRAMID v1] tile image %dx%d (RG32F) for "
                    "scene %dx%d, tile=%d\n", tw, th, width, height, kTileSize);
}

// CPU reference parity check. Reads the scene depth back, recomputes per-tile
// numeric min/max on the CPU, and compares against a GPU image readback.
// One-shot (latched). Reversed-Z aware only in commentary — the comparison is
// over raw numeric extents, exactly what the GPU stores.
void runParityCheck(GLuint sceneDepthTex, int width, int height) {
    // --- Read scene depth to CPU (DEPTH24_STENCIL8 -> float depth). ----------
    // glGetTexImage of a packed depth/stencil with GL_DEPTH_COMPONENT/GL_FLOAT
    // returns the normalized depth in [0,1] — exactly what the GLSL
    // texelFetch(...).r returns, so the CPU and GPU read the SAME source value.
    std::vector<float> cpuDepth((size_t)width * height);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, cpuDepth.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // --- CPU per-tile min/max. ----------------------------------------------
    std::vector<float> cpuMin((size_t)s_tileW * s_tileH, 1.0f);
    std::vector<float> cpuMax((size_t)s_tileW * s_tileH, 0.0f);
    for (int y = 0; y < height; ++y) {
        int ty = y / kTileSize;
        for (int x = 0; x < width; ++x) {
            int tx = x / kTileSize;
            float d = cpuDepth[(size_t)y * width + x];
            size_t ti = (size_t)ty * s_tileW + tx;
            if (d < cpuMin[ti]) cpuMin[ti] = d;
            if (d > cpuMax[ti]) cpuMax[ti] = d;
        }
    }

    // PLANTED-ERROR self-test: corrupt one CPU tile so the comparison SHOULD
    // mismatch. Proves the checker is capable of reporting FAIL.
    if (s_plant && !cpuMin.empty()) {
        cpuMin[0] = -123.0f;  // a value the GPU can never produce
        mc2::fmt::print_err("[CLUSTER_DEPTH_PYRAMID v1] PLANT: corrupted CPU tile 0 min -> -123.0 (expecting a mismatch below)\n");
    }

    // --- GPU image readback (RG32F: R=min, G=max). --------------------------
    // The dispatch already issued a ComputeImageWrite->TextureReadback barrier
    // before this call; glGetTexImage now sees the imageStore results.
    std::vector<float> gpuRG((size_t)s_tileW * s_tileH * 2);
    glBindTexture(GL_TEXTURE_2D, s_tileTex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_FLOAT, gpuRG.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // --- Compare. ------------------------------------------------------------
    // Tolerance: the GPU stores RG32F (full fp32) and the CPU reads GL_FLOAT
    // from the SAME depth texture, so values are bit-for-bit identical in
    // principle. We allow a tiny epsilon (1e-6) purely to absorb any driver-side
    // fp normalization noise in the DEPTH24->float conversion; in practice the
    // observed delta is 0. A mismatch beyond epsilon is a real failure.
    const float kEps = 1e-6f;
    size_t nTiles = (size_t)s_tileW * s_tileH;
    size_t mismatches = 0;
    float  worstDelta = 0.0f;
    for (size_t i = 0; i < nTiles; ++i) {
        float gMin = gpuRG[i * 2 + 0];
        float gMax = gpuRG[i * 2 + 1];
        float dMin = std::fabs(gMin - cpuMin[i]);
        float dMax = std::fabs(gMax - cpuMax[i]);
        if (dMin > worstDelta) worstDelta = dMin;
        if (dMax > worstDelta) worstDelta = dMax;
        if (dMin > kEps || dMax > kEps) ++mismatches;
    }

    const bool pass = (mismatches == 0);
    MC2_DIAGFE(stderr,
            "[CLUSTER_DEPTH_PYRAMID v1] PARITY %s tiles=%zu mismatches=%zu "
            "worst_delta=%.9g tol=%.1e plant=%d (%s)\n",
            pass ? "PASS" : "FAIL", nTiles, mismatches, worstDelta, kEps,
            s_plant ? 1 : 0,
            s_plant ? "planted-error expects FAIL" : "expects PASS");
    fflush(stderr);
}

}  // namespace

bool IsEnabled() {
    if (!s_gateResolved) {
        s_gateResolved = true;
        s_enabled = mc2::gate::get<"MC2_CLUSTER_DEPTH_PYRAMID">();
        s_verify  = s_enabled && mc2::gate::get<"MC2_CLUSTER_DEPTH_PYRAMID_VERIFY">();
        s_plant   = s_verify  && mc2::gate::get<"MC2_CLUSTER_DEPTH_PYRAMID_PLANT">();
        MC2_DIAGFE(stderr, "[CLUSTER_DEPTH_PYRAMID v1] enabled=%d verify=%d plant=%d "
                        "(MC2_CLUSTER_DEPTH_PYRAMID)\n",
                s_enabled ? 1 : 0, s_verify ? 1 : 0, s_plant ? 1 : 0);
    }
    return s_enabled;
}

void Run(unsigned int sceneDepthTex, int width, int height) {
    if (!IsEnabled()) return;                 // master gate OFF => true no-op
    if (sceneDepthTex == 0 || width <= 0 || height <= 0) return;
    if (s_shaderBad) return;

    if (s_program == 0) {
        s_program = buildProgram();
        if (s_program == 0) { s_shaderBad = true; return; }
        s_uDepthSize = glGetUniformLocation(s_program, "u_depthSize");
    }

    ensureTileTexture(width, height);
    if (s_tileTex == 0) return;

    glUseProgram(s_program);

    // Input depth as a sampler2D on texture unit 0.
    glActiveTexture(GL_TEXTURE0 + kDepthTexUnit);
    glBindTexture(GL_TEXTURE_2D, (GLuint)sceneDepthTex);

    // Output image (write-only RG32F) on image unit 0.
    glBindImageTexture(kTileImageUnit, s_tileTex, 0, GL_FALSE, 0,
                       GL_WRITE_ONLY, GL_RG32F);

    if (s_uDepthSize >= 0) glUniform2i(s_uDepthSize, width, height);

    // One workgroup per tile (each workgroup is kTileSize x kTileSize threads).
    glDispatchCompute((GLuint)s_tileW, (GLuint)s_tileH, 1);

    if (s_verify && !s_verifyDone) {
        s_verifyDone = true;
        // Order the imageStore writes before the CPU glGetTexImage readback.
        gpuSyncBarrier(GpuProducer::ComputeImageWrite, GpuConsumer::TextureReadback,
                       "cluster_depth_pyramid:image->readback");
        runParityCheck((GLuint)sceneDepthTex, width, height);
    }

    // Leave no leaked binds for the next pass. (Substrate slice has no GPU
    // consumer of the tile image, so no TextureSample barrier is needed here.)
    glBindImageTexture(kTileImageUnit, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG32F);
    glActiveTexture(GL_TEXTURE0 + kDepthTexUnit);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

void Shutdown() {
    if (s_tileTex) { glDeleteTextures(1, &s_tileTex); s_tileTex = 0; }
    if (s_program) { glDeleteProgram(s_program); s_program = 0; }
    s_tileW = s_tileH = s_srcW = s_srcH = 0;

    // REGISTRY-COMPUTE-IDS-1: mark the slot unavailable on teardown.
    RenderCore::RenderResourceDesc invalid;
    invalid.id = RenderCore::RenderResourceId::ClusterDepthPyramid;
    RenderCore::registerOrUpdateRenderResource(invalid);
}

// --- Accessors (MC2-LIGHTGRID-BUILD-NATIVE-1) -------------------------------
unsigned int TileTexture() { return (unsigned int)s_tileTex; }
int TileGridW()            { return s_tileW; }
int TileGridH()            { return s_tileH; }
int TileSize()             { return kTileSize; }

}  // namespace cluster_depth_pyramid
