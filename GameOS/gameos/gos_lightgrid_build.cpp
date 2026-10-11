// MC2-LIGHTGRID-BUILD-NATIVE-1 — see gos_lightgrid_build.h for intent.
//
// Clean-room MC2-native light-bin grid builder. INERT: no shading consumer, no
// visual change. Mirrors the CLUSTER-DEPTH-PYRAMID-NATIVE-1 build+parity shape.

#include "gos_lightgrid_build.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "gos_cluster_depth_pyramid.h"
#include "gos_gpu_sync.h"
#include "../../mclib/object_light_row.h"  // kMaxLights (ObjectLights ABI cap)
#include "../../RenderCore/gpu_abi/GpuObjectLights.h"
#include "../../RenderCore/RenderResourceRegistry.h"  // REGISTRY-COMPUTE-IDS-1: LightgridGrid/LightgridIndex

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

namespace lightgrid_build {

namespace {

// --- Constants -------------------------------------------------------------
// Light cap == the ObjectLights record stride, from the single-source GPU record
// (RenderCore/gpu_abi/GpuObjectLights.h). It is injected into lightgrid_build.comp as
// LIGHTGRID_MAX_LIGHTS, which sizes the work group; the shader refuses to compile if
// it disagrees with the generated ObjectLights array length. It also bounds the
// per-tile index-pool stride.
constexpr int kMaxLights = (int)object_light_row::kMaxLights;

// ObjectLights SSBO binding (LIGHT_DATA_SSBO_BINDING in lighting.hglsl). The
// sphere-build stage reads it read-only.
constexpr GLuint kObjectLightsBinding = 20;

// std430 byte offsets inside the live ObjectLights record, from the single-source
// GPU record. Used only by the CPU parity reference, which reads the SAME bytes the
// GPU sphere stage reads.
constexpr int kOffLightToWorld = static_cast<int>(offsetof(GpuObjectLights, lightToWorld));  // [N][16] floats
constexpr int kOffLightDir     = static_cast<int>(offsetof(GpuObjectLights, lightDir));      // [N][4] floats (.w = type)
constexpr int kOffLightColor   = static_cast<int>(offsetof(GpuObjectLights, lightColor));    // [N][4] floats
constexpr int kOffLightFalloff = static_cast<int>(offsetof(GpuObjectLights, lightFalloff));  // [N][4] floats (.y = far = radius)
constexpr int kOffNumLights    = static_cast<int>(offsetof(GpuObjectLights, numLights_));    // int
constexpr int kObjectLightsRecordBytes = static_cast<int>(sizeof(GpuObjectLights));

// Sphere record stride in bytes (std430): vec4 + 4*uint = 16 + 16 = 32.
constexpr int kSphereStride = 32;

// Bind points inside THIS pass's own dispatch bind sets (slots are multiplexed
// per-pass; these are local to the lightgrid passes).
constexpr GLuint kSphereSsboSlot   = 0;  // stage0 out / stage1 in
constexpr GLuint kSphereCountSlot  = 1;  // stage0 survivor count
constexpr GLuint kHeaderImageUnit  = 0;  // stage1 RG32UI header image
constexpr GLuint kIndexPoolSlot    = 1;  // stage1 compact index pool
constexpr GLuint kCursorSlot       = 2;  // stage1 global atomic cursor
constexpr GLuint kTileMinMaxUnit   = 1;  // stage1 depth pyramid sampler unit

// --- Gate state ------------------------------------------------------------
bool s_gateResolved = false;
bool s_enabled      = false;
bool s_verify       = false;
bool s_plant        = false;

bool s_shaderBad    = false;
GLuint s_progSphere = 0;   // LIGHTGRID_STAGE 0
GLuint s_progGrid   = 0;   // LIGHTGRID_STAGE 1

// stage1 grid uniforms
GLint s_uTileGrid    = -1;
GLint s_uSceneSize   = -1;
GLint s_uTileSize    = -1;
GLint s_uLightCount  = -1;
GLint s_uPoolStride  = -1;
GLint s_uInvViewProj = -1;

// --- GPU buffers -----------------------------------------------------------
GLuint s_sphereSsbo  = 0;
GLuint s_sphereCount = 0;
GLuint s_indexPool   = 0;
GLuint s_cursorBuf   = 0;
GLuint s_headerTex   = 0;
int    s_headerW     = 0;
int    s_headerH     = 0;

bool   s_verifyDone  = false;
int    s_verifyWaited = 0;       // frames waited for numLights>0 before verify
// Wait up to this many dispatched frames for a lit frame before verifying the
// empty case. ~30s missions at 140fps give thousands of frames; combat/weapon
// flashes well within this window. Generous so menus/load don't time us out.
constexpr int kVerifyWaitFrames = 1200;

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

GLuint buildStage(int stage) {
    char* fileSrc = loadTextFile("shaders/lightgrid_build.comp");
    if (!fileSrc) {
        MC2_DIAGFE(stderr, "[LIGHTGRID_BUILD v1] shader source not found: "
                        "shaders/lightgrid_build.comp\n");
        return 0;
    }
    const char* kVersion = "#version 430\n";
    std::string defs = "#define LIGHTGRID_STAGE " + std::to_string(stage) + "\n" +
                       "#define LIGHTGRID_MAX_LIGHTS " + std::to_string(kMaxLights) + "\n";
    const char* strs[3] = { kVersion, defs.c_str(), fileSrc };

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
        MC2_DIAGFE(stderr, "[LIGHTGRID_BUILD v1] stage %d compile error:\n%s\n",
                stage, log.data());
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
        MC2_DIAGFE(stderr, "[LIGHTGRID_BUILD v1] stage %d link error:\n%s\n",
                stage, log.data());
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

void ensureBuffers(int tileW, int tileH) {
    const int nTiles = tileW * tileH;

    if (s_sphereSsbo == 0) {
        // TIER2-EXCLUDED: substrate-gated
        glGenBuffers(1, &s_sphereSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_sphereSsbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)kSphereStride * kMaxLights,
                     nullptr, GL_DYNAMIC_COPY);
    }
    if (s_sphereCount == 0) {
        // TIER2-EXCLUDED: substrate-gated
        glGenBuffers(1, &s_sphereCount);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_sphereCount);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GLuint), nullptr, GL_DYNAMIC_COPY);
    }
    if (s_cursorBuf == 0) {
        // TIER2-EXCLUDED: substrate-gated
        glGenBuffers(1, &s_cursorBuf);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_cursorBuf);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GLuint), nullptr, GL_DYNAMIC_COPY);
    }

    // Index pool: up to kMaxLights indices per tile (a tile can bin every light).
    const GLsizeiptr poolBytes = (GLsizeiptr)sizeof(GLuint) * nTiles * kMaxLights;
    if (s_indexPool == 0 || tileW != s_headerW || tileH != s_headerH) {
        // TIER2-EXCLUDED: substrate-gated
        if (s_indexPool == 0) glGenBuffers(1, &s_indexPool);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_indexPool);
        glBufferData(GL_SHADER_STORAGE_BUFFER, poolBytes, nullptr, GL_DYNAMIC_COPY);
    }

    // Header image RG32UI, one texel per tile (offset,count).
    if (s_headerTex == 0 || tileW != s_headerW || tileH != s_headerH) {
        if (s_headerTex != 0) { glDeleteTextures(1, &s_headerTex); s_headerTex = 0; }
        // TEX-CLASS: per-pass-rebind -- clustered-light grid header image (compute-written, sampled per pass; Vk needs image+layout)
        glGenTextures(1, &s_headerTex);
        glBindTexture(GL_TEXTURE_2D, s_headerTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG32UI, tileW, tileH);
        glBindTexture(GL_TEXTURE_2D, 0);
        MC2_DIAGFE(stderr, "[LIGHTGRID_BUILD v1] header image %dx%d (RG32UI), "
                        "index pool %d tiles x %d = %d uints\n",
                tileW, tileH, nTiles, kMaxLights, nTiles * kMaxLights);
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    s_headerW = tileW; s_headerH = tileH;

    // REGISTRY-COMPUTE-IDS-1: register the live lightgrid SSBOs (observe-only
    // metadata; never read by the draw path). Gated/default-OFF, so this only
    // fires when the lightgrid-build compute path runs.
    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::LightgridGrid;
        d.kind      = RenderCore::RenderResourceKind::Buffer;
        d.lifetime  = RenderCore::RenderResourceLifetime::FrameLocal;  // transient lightgrid compute output, per-frame
        d.format    = RenderCore::RenderResourceFormat::BufferRaw;
        d.debugName = "LightgridGrid";
        d.glName    = static_cast<uint32_t>(s_sphereSsbo);
        d.sizeBytes = static_cast<uint64_t>(kSphereStride) * kMaxLights;
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }
    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::LightgridIndex;
        d.kind      = RenderCore::RenderResourceKind::Buffer;
        d.lifetime  = RenderCore::RenderResourceLifetime::FrameLocal;  // transient lightgrid compute output, per-frame
        d.format    = RenderCore::RenderResourceFormat::BufferRaw;
        d.debugName = "LightgridIndex";
        d.glName    = static_cast<uint32_t>(s_indexPool);
        d.sizeBytes = static_cast<uint64_t>(poolBytes);
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }
}

// --- CPU reference + parity ------------------------------------------------
// world = invViewProj_GL_FALSE * clip, i.e. the row-major data interpreted
// column-major (transpose). world[r] = sum_c M[c*4+r] * clip[c]. Identical to
// what the GPU does (u_invViewProj uploaded GL_FALSE).
void unproject(const float* M, float nx, float ny, float nz, float out[3]) {
    float clip[4] = { nx, ny, nz, 1.0f };
    float w[4];
    for (int r = 0; r < 4; ++r) {
        float s = 0.0f;
        for (int c = 0; c < 4; ++c) s += M[c * 4 + r] * clip[c];
        w[r] = s;
    }
    float iw = (w[3] != 0.0f) ? 1.0f / w[3] : 0.0f;
    out[0] = w[0] * iw; out[1] = w[1] * iw; out[2] = w[2] * iw;
}

// Mirror of the shader's sphereInTile: 6 inward planes from 8 corners.
bool sphereInTile(const float c[3], float r, const float corners[8][3]) {
    float ref[3] = { 0, 0, 0 };
    for (int i = 0; i < 8; ++i) { ref[0] += corners[i][0]; ref[1] += corners[i][1]; ref[2] += corners[i][2]; }
    ref[0] *= 0.125f; ref[1] *= 0.125f; ref[2] *= 0.125f;
    static const int faces[6][3] = {
        {0,1,2},{4,6,5},{0,4,5},{2,6,7},{0,3,7},{1,5,6}
    };
    for (int f = 0; f < 6; ++f) {
        const float* a = corners[faces[f][0]];
        const float* b = corners[faces[f][1]];
        const float* d = corners[faces[f][2]];
        float ab[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
        float ad[3] = { d[0]-a[0], d[1]-a[1], d[2]-a[2] };
        float nrm[3] = {
            ab[1]*ad[2] - ab[2]*ad[1],
            ab[2]*ad[0] - ab[0]*ad[2],
            ab[0]*ad[1] - ab[1]*ad[0]
        };
        float len = std::sqrt(nrm[0]*nrm[0] + nrm[1]*nrm[1] + nrm[2]*nrm[2]);
        if (len < 1e-12f) continue;
        nrm[0] /= len; nrm[1] /= len; nrm[2] /= len;
        float toRef = nrm[0]*(ref[0]-a[0]) + nrm[1]*(ref[1]-a[1]) + nrm[2]*(ref[2]-a[2]);
        if (toRef < 0.0f) { nrm[0] = -nrm[0]; nrm[1] = -nrm[1]; nrm[2] = -nrm[2]; }
        float dist = nrm[0]*(c[0]-a[0]) + nrm[1]*(c[1]-a[1]) + nrm[2]*(c[2]-a[2]);
        if (dist < -r) return false;
    }
    return true;
}

void runParityCheck(const float* invVP, int sceneW, int sceneH,
                    int tileW, int tileH, int tileSize, GLuint tileMinMaxTex) {
    const int nTiles = tileW * tileH;

    // --- Read back the GPU sphere buffer (what the grid stage consumed). -----
    std::vector<unsigned char> sphereBytes((size_t)kSphereStride * kMaxLights);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_sphereSsbo);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                       (GLsizeiptr)sphereBytes.size(), sphereBytes.data());
    GLuint gpuSphereCount = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_sphereCount);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(GLuint), &gpuSphereCount);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    int nLights = (int)gpuSphereCount;
    if (nLights > kMaxLights) nLights = kMaxLights;

    // Decode spheres (parity ref reads the SAME GPU-built records).
    struct Sphere { float c[3]; float r; unsigned idx; };
    std::vector<Sphere> spheres(nLights);
    for (int i = 0; i < nLights; ++i) {
        const unsigned char* p = sphereBytes.data() + (size_t)i * kSphereStride;
        float cr[4]; memcpy(cr, p, 16);
        unsigned idx; memcpy(&idx, p + 16, 4);
        spheres[i].c[0] = cr[0]; spheres[i].c[1] = cr[1]; spheres[i].c[2] = cr[2];
        spheres[i].r = cr[3]; spheres[i].idx = idx;
    }

    // --- Read back the depth pyramid tile (min,max). -------------------------
    std::vector<float> tileMM((size_t)nTiles * 2);
    glBindTexture(GL_TEXTURE_2D, tileMinMaxTex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_FLOAT, tileMM.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // --- Read back GPU grid header + index pool. -----------------------------
    std::vector<GLuint> gpuHeader((size_t)nTiles * 2);
    glBindTexture(GL_TEXTURE_2D, s_headerTex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RG_INTEGER, GL_UNSIGNED_INT, gpuHeader.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    std::vector<GLuint> gpuPool((size_t)nTiles * kMaxLights);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_indexPool);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                       (GLsizeiptr)(gpuPool.size() * sizeof(GLuint)), gpuPool.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // --- CPU reference: recompute per-tile survivor SET. ---------------------
    // We compare SETS (membership), not pool order/offsets: the global atomic
    // reserve order across tiles is GPU-schedule-nondeterministic, so offsets
    // are not a stable parity surface. The light SET binned to each tile IS
    // deterministic and is what a shading consumer would read. PLANT corrupts a
    // CPU tile's set so the comparison must FAIL.
    std::vector<unsigned char> cpuMask((size_t)nTiles * kMaxLights, 0);
    std::vector<int> cpuCount(nTiles, 0);

    for (int ty = 0; ty < tileH; ++ty) {
        for (int tx = 0; tx < tileW; ++tx) {
            const int ti = ty * tileW + tx;
            float p0x = (float)tx * tileSize;
            float p0y = (float)ty * tileSize;
            float p1x = p0x + tileSize; if (p1x > sceneW) p1x = (float)sceneW;
            float p1y = p0y + tileSize; if (p1y > sceneH) p1y = (float)sceneH;
            float nMinX = (p0x / sceneW) * 2.0f - 1.0f;
            float nMinY = (p0y / sceneH) * 2.0f - 1.0f;
            float nMaxX = (p1x / sceneW) * 2.0f - 1.0f;
            float nMaxY = (p1y / sceneH) * 2.0f - 1.0f;
            float near = tileMM[(size_t)ti * 2 + 1];  // G = MAX = nearest (reversed-Z)
            float far  = tileMM[(size_t)ti * 2 + 0];  // R = MIN = farthest
            float corners[8][3];
            unproject(invVP, nMinX, nMaxY, near, corners[0]);
            unproject(invVP, nMaxX, nMaxY, near, corners[1]);
            unproject(invVP, nMaxX, nMinY, near, corners[2]);
            unproject(invVP, nMinX, nMinY, near, corners[3]);
            unproject(invVP, nMinX, nMaxY, far,  corners[4]);
            unproject(invVP, nMaxX, nMaxY, far,  corners[5]);
            unproject(invVP, nMaxX, nMinY, far,  corners[6]);
            unproject(invVP, nMinX, nMinY, far,  corners[7]);
            for (int s = 0; s < nLights; ++s) {
                if (spheres[s].r > 0.0f &&
                    sphereInTile(spheres[s].c, spheres[s].r, corners)) {
                    cpuMask[(size_t)ti * kMaxLights + spheres[s].idx] = 1;
                    cpuCount[ti]++;
                }
            }
        }
    }

    // PLANT: corrupt one CPU tile's membership set so the comparison MUST fail.
    if (s_plant && nTiles > 0) {
        int pt = 0;
        // flip one light's membership bit + bump the count so both count and set
        // disagree with the GPU.
        cpuMask[(size_t)pt * kMaxLights + 0] ^= 1;
        cpuCount[pt] += (cpuMask[(size_t)pt * kMaxLights + 0] ? 1 : -1);
        mc2::fmt::print_err("[LIGHTGRID_BUILD v1] PLANT: corrupted CPU tile {:d} membership set (expecting a mismatch below)\n", mc2::fmt::as_i32(pt));
    }

    // --- Compare GPU grid vs CPU reference (count + membership set). ----------
    size_t mismatches = 0;
    int worstCountDelta = 0;
    for (int ti = 0; ti < nTiles; ++ti) {
        GLuint gOff = gpuHeader[(size_t)ti * 2 + 0];
        GLuint gCnt = gpuHeader[(size_t)ti * 2 + 1];

        if ((int)gCnt != cpuCount[ti]) {
            int d = std::abs((int)gCnt - cpuCount[ti]);
            if (d > worstCountDelta) worstCountDelta = d;
            ++mismatches;
            continue;
        }
        // membership: build GPU set from pool span, compare to cpuMask.
        unsigned char gpuSet[kMaxLights]; memset(gpuSet, 0, sizeof(gpuSet));
        bool spanOk = true;
        for (GLuint k = 0; k < gCnt; ++k) {
            size_t pi = (size_t)gOff + k;
            if (pi >= gpuPool.size()) { spanOk = false; break; }
            GLuint lidx = gpuPool[pi];
            if (lidx < (GLuint)kMaxLights) gpuSet[lidx] = 1;
        }
        if (!spanOk) { ++mismatches; continue; }
        bool setEq = true;
        for (int l = 0; l < kMaxLights; ++l) {
            if (gpuSet[l] != cpuMask[(size_t)ti * kMaxLights + l]) { setEq = false; break; }
        }
        if (!setEq) ++mismatches;
    }

    const bool pass = (mismatches == 0);
    MC2_DIAGFE(stderr,
            "[LIGHTGRID_BUILD v1] PARITY %s tiles=%d lights=%d mismatches=%zu "
            "worst_count_delta=%d plant=%d (%s)\n",
            pass ? "PASS" : "FAIL", nTiles, nLights, mismatches, worstCountDelta,
            s_plant ? 1 : 0,
            s_plant ? "planted-error expects FAIL" : "expects PASS");
    fflush(stderr);
}

}  // namespace

bool IsEnabled() {
    if (!s_gateResolved) {
        s_gateResolved = true;
        s_enabled = mc2::gate::get<"MC2_LIGHTGRID_BUILD">();
        s_verify  = s_enabled && mc2::gate::get<"MC2_LIGHTGRID_VERIFY">();
        s_plant   = s_verify  && mc2::gate::get<"MC2_LIGHTGRID_PLANT">();
        MC2_DIAGFE(stderr, "[LIGHTGRID_BUILD v1] enabled=%d verify=%d plant=%d "
                        "(MC2_LIGHTGRID_BUILD)\n",
                s_enabled ? 1 : 0, s_verify ? 1 : 0, s_plant ? 1 : 0);
    }
    return s_enabled;
}

void Run(const float* invViewProj16, int sceneW, int sceneH) {
    if (!IsEnabled()) return;                 // master gate OFF => true no-op
    if (s_shaderBad) return;
    if (invViewProj16 == nullptr || sceneW <= 0 || sceneH <= 0) return;

    // The depth pyramid is our depth input. If it didn't run / produced nothing
    // (e.g. its own gate is OFF), we have no per-tile Z range to bin against.
    const GLuint tileMinMax = (GLuint)cluster_depth_pyramid::TileTexture();
    const int tileW = cluster_depth_pyramid::TileGridW();
    const int tileH = cluster_depth_pyramid::TileGridH();
    const int tileSize = cluster_depth_pyramid::TileSize();
    if (tileMinMax == 0 || tileW <= 0 || tileH <= 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            MC2_DIAGFE(stderr, "[LIGHTGRID_BUILD v1] no depth pyramid tile texture "
                            "(set MC2_CLUSTER_DEPTH_PYRAMID=1) — pass skipped\n");
        }
        return;
    }

    if (s_progSphere == 0) {
        s_progSphere = buildStage(0);
        s_progGrid   = buildStage(1);
        if (s_progSphere == 0 || s_progGrid == 0) { s_shaderBad = true; return; }
        s_uTileGrid    = glGetUniformLocation(s_progGrid, "u_tileGrid");
        s_uSceneSize   = glGetUniformLocation(s_progGrid, "u_sceneSize");
        s_uTileSize    = glGetUniformLocation(s_progGrid, "u_tileSize");
        s_uLightCount  = glGetUniformLocation(s_progGrid, "u_lightCount");
        s_uPoolStride  = glGetUniformLocation(s_progGrid, "u_poolStride");
        s_uInvViewProj = glGetUniformLocation(s_progGrid, "u_invViewProj");
    }

    ensureBuffers(tileW, tileH);

    // ===== STAGE 0 — sphere build from live ObjectLights (binding 20). =======
    // ObjectLights is already bound at binding 20 by gos_LightDataSsbo_Upload
    // this frame; the stage reads it read-only and writes the sphere SSBO.
    glUseProgram(s_progSphere);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSphereSsboSlot,  s_sphereSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSphereCountSlot, s_sphereCount);
    glDispatchCompute(1, 1, 1);   // one workgroup of kMaxLights threads

    // Order the sphere writes before the grid stage reads them (compute->compute).
    gpuSyncBarrier(GpuProducer::ComputeShader, GpuConsumer::ComputeShader,
                   "lightgrid:spheres->grid");

    // ===== STAGE 1 — grid build (one workgroup per tile). ====================
    // Reset the global pool cursor to 0 before this frame's reservations.
    {
        GLuint zero = 0;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_cursorBuf);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(GLuint), &zero);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        // ClearBuffer-style update -> compute read of the cursor.
        gpuSyncBarrier(GpuProducer::ClearBuffer, GpuConsumer::ComputeShader,
                       "lightgrid:cursor-reset->grid");
    }

    glUseProgram(s_progGrid);

    // Depth pyramid sampler on its own unit; header image on image unit 0.
    glActiveTexture(GL_TEXTURE0 + kTileMinMaxUnit);
    glBindTexture(GL_TEXTURE_2D, tileMinMax);
    glBindImageTexture(kHeaderImageUnit, s_headerTex, 0, GL_FALSE, 0,
                       GL_WRITE_ONLY, GL_RG32UI);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSphereSsboSlot, s_sphereSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kIndexPoolSlot,  s_indexPool);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kCursorSlot,     s_cursorBuf);

    // The depth pyramid wrote the tile texture via imageStore earlier this frame
    // (in cluster_depth_pyramid::Run). Order that imageStore before our sample.
    gpuSyncBarrier(GpuProducer::ComputeImageWrite, GpuConsumer::TextureSample,
                   "lightgrid:pyramid->grid-sample");

    if (s_uTileGrid    >= 0) glUniform2i(s_uTileGrid, tileW, tileH);
    if (s_uSceneSize   >= 0) glUniform2i(s_uSceneSize, sceneW, sceneH);
    if (s_uTileSize    >= 0) glUniform1i(s_uTileSize, tileSize);
    if (s_uPoolStride  >= 0) glUniform1i(s_uPoolStride, kMaxLights);
    // u_lightCount is just an upper bound (== cap). The grid stage skips
    // inactive slots via the sphere's radius>0 test, so NO per-frame CPU readback
    // of the active light count is needed -- keeping the steady-state hot path
    // free of GPU stalls. (An earlier per-frame glGetBufferSubData here caused a
    // mc2_24 heartbeat freeze.)
    if (s_uLightCount >= 0) glUniform1i(s_uLightCount, kMaxLights);
    // invViewProj: GL_FALSE (row-major as-is) — same convention as every post
    // pass; the GLSL mat4 becomes its transpose -> correct clip->world.
    if (s_uInvViewProj >= 0)
        glUniformMatrix4fv(s_uInvViewProj, 1, GL_FALSE, invViewProj16);

    glDispatchCompute((GLuint)tileW, (GLuint)tileH, 1);

    // One-shot verify: prefer a frame that actually has lights so the parity
    // exercises the sphere/frustum cull with real geometry (an empty-set frame
    // only proves the plumbing). We wait up to kVerifyWaitFrames frames for
    // numLights>0; if none ever appear we fall back to verifying the empty case
    // (still a valid parity surface) on the last waited frame.
    if (s_verify && !s_verifyDone) {
        GLuint cnt = 0;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_sphereCount);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(GLuint), &cnt);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        ++s_verifyWaited;
        const bool haveLights = (cnt > 0);
        const bool timedOut   = (s_verifyWaited >= kVerifyWaitFrames);
        if (!haveLights && !timedOut) {
            // not yet — finish cleanup below and try again next frame.
        } else {
        s_verifyDone = true;
        // Order the grid's image/SSBO writes before the CPU readbacks.
        gpuSyncBarrier(GpuProducer::ComputeImageWrite, GpuConsumer::TextureReadback,
                       "lightgrid:header->readback");
        gpuSyncBarrier(GpuProducer::ComputeShader, GpuConsumer::BufferReadback,
                       "lightgrid:pool->readback");
        runParityCheck(invViewProj16, sceneW, sceneH, tileW, tileH, tileSize, tileMinMax);
        }
    }

    // Leave no leaked binds for the next pass.
    glBindImageTexture(kHeaderImageUnit, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG32UI);
    glActiveTexture(GL_TEXTURE0 + kTileMinMaxUnit);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSphereSsboSlot, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSphereCountSlot, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kIndexPoolSlot, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kCursorSlot, 0);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
}

void Shutdown() {
    if (s_sphereSsbo)  { glDeleteBuffers(1, &s_sphereSsbo);  s_sphereSsbo = 0; }
    if (s_sphereCount) { glDeleteBuffers(1, &s_sphereCount); s_sphereCount = 0; }
    if (s_indexPool)   { glDeleteBuffers(1, &s_indexPool);   s_indexPool = 0; }
    if (s_cursorBuf)   { glDeleteBuffers(1, &s_cursorBuf);   s_cursorBuf = 0; }
    if (s_headerTex)   { glDeleteTextures(1, &s_headerTex);  s_headerTex = 0; }
    if (s_progSphere)  { glDeleteProgram(s_progSphere);      s_progSphere = 0; }
    if (s_progGrid)    { glDeleteProgram(s_progGrid);        s_progGrid = 0; }
    s_headerW = s_headerH = 0;

    // REGISTRY-COMPUTE-IDS-1: mark the slots unavailable on teardown.
    RenderCore::RenderResourceDesc invalid;
    invalid.id = RenderCore::RenderResourceId::LightgridGrid;
    RenderCore::registerOrUpdateRenderResource(invalid);
    invalid.id = RenderCore::RenderResourceId::LightgridIndex;
    RenderCore::registerOrUpdateRenderResource(invalid);
}

}  // namespace lightgrid_build
