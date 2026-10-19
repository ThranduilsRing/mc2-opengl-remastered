// POSTPROCESS-COMPUTE-BLUR-1 — see gos_postprocess_blur.h for intent.

#include <memory>
#include "gos_postprocess_blur.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "gos_gpu_sync.h"
#include "../../RenderCore/RenderResourceRegistry.h"  // REGISTRY-COMPUTE-IDS-1: PostprocessComputeBlur

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

namespace postprocess_blur {

namespace {

// --- Workgroup tile ---------------------------------------------------------
// 8x8 = 64 invocations, comfortably within the GL 4.3 minimum guarantee and the
// AMD RX 7900 XTX target. Used for all three stages; the dispatch covers the
// destination grid with ceil(dim / tile).
constexpr int kTileX = 8;
constexpr int kTileY = 8;

// --- Authored separable Gaussian weights (binomial 5-tap, sum = 1.0) ---------
// MUST match shaders/postprocess_blur.comp W0/W1/W2 exactly.
constexpr float kW0 = 0.0625f; // 1/16  (offset +/-2)
constexpr float kW1 = 0.25f;   // 4/16  (offset +/-1)
constexpr float kW2 = 0.375f;  // 6/16  (center)

// --- Controlled test pattern dimensions (full-res). Half-res = /2. -----------
// Chosen non-power-of-two-friendly but even so the 2x2 downsample maps cleanly.
constexpr int kTestSrcW = 64;
constexpr int kTestSrcH = 64;

// --- State -----------------------------------------------------------------
bool   s_gateResolved = false;
bool   s_enabled      = false;
bool   s_verify       = false;
bool   s_plant        = false;

bool   s_shaderBad    = false;
GLuint s_progDown     = 0;  // MODE_DOWNSAMPLE
GLuint s_progBlurH    = 0;  // MODE_BLUR_H
GLuint s_progBlurV    = 0;  // MODE_BLUR_V

GLint  s_uDstSizeDown = -1;
GLint  s_uDstSizeH    = -1;
GLint  s_uDstSizeV    = -1;

// Half-res ping-pong RGBA16F images. A receives downsample + final V output;
// B receives the H output. Reallocated when the half-res size changes.
GLuint s_pingA        = 0;
GLuint s_pingB        = 0;
int    s_halfW        = 0;
int    s_halfH        = 0;

// Owned controlled test-pattern texture (full-res RGBA16F).
GLuint s_testTex      = 0;

bool   s_verifyDone   = false;  // one-shot parity latch

constexpr GLuint kDstImageUnit = 0;
constexpr GLuint kSrcTexUnit   = 0;

std::unique_ptr<char[]> loadTextFile(const char* fname) {
    FILE* f = fopen(fname, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return nullptr; }
    auto bufOwner = std::make_unique_for_overwrite<char[]>(sz + 1);
    char* buf = bufOwner.get();
    if (static_cast<long>(fread(buf, 1, sz, f)) != sz) { fclose(f); bufOwner.reset(); return nullptr; }
    buf[sz] = '\0';
    fclose(f);
    return bufOwner;
}

// Build one compute program from the shared .comp with the given MODE define.
GLuint buildProgram(const char* modeDefine) {
    auto fileSrcOwner = loadTextFile("shaders/postprocess_blur.comp");
    char* fileSrc = fileSrcOwner.get();
    if (!fileSrc) {
        MC2_DIAGFE(stderr, "[POSTPROCESS_BLUR v1] shader source not found: "
                        "shaders/postprocess_blur.comp\n");
        return 0;
    }

    // Project rule: no #version in the shader file — prepend it here, plus the
    // tile-size defines and the MODE selector so GLSL local size matches dispatch.
    const char* kVersion = "#version 430\n";
    std::string tileDef  = "#define TILE_X " + std::to_string(kTileX) + "\n"
                         + "#define TILE_Y " + std::to_string(kTileY) + "\n";
    std::string modeStr  = std::string("#define ") + modeDefine + " 1\n";

    const char* strs[4] = { kVersion, tileDef.c_str(), modeStr.c_str(), fileSrc };

    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(sh, 4, strs, nullptr);
    glCompileShader(sh);
    fileSrcOwner.reset();

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint n = 0; glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &n);
        std::vector<char> log(n > 1 ? n : 1);
        glGetShaderInfoLog(sh, (GLsizei)log.size(), nullptr, log.data());
        MC2_DIAGFE(stderr, "[POSTPROCESS_BLUR v1] compile error (%s):\n%s\n",
                modeDefine, log.data());
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
        MC2_DIAGFE(stderr, "[POSTPROCESS_BLUR v1] link error (%s):\n%s\n",
                modeDefine, log.data());
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

bool ensurePrograms() {
    if (s_shaderBad) return false;
    if (s_progDown && s_progBlurH && s_progBlurV) return true;
    if (!s_progDown)  s_progDown  = buildProgram("MODE_DOWNSAMPLE");
    if (!s_progBlurH) s_progBlurH = buildProgram("MODE_BLUR_H");
    if (!s_progBlurV) s_progBlurV = buildProgram("MODE_BLUR_V");
    if (!s_progDown || !s_progBlurH || !s_progBlurV) { s_shaderBad = true; return false; }
    s_uDstSizeDown = glGetUniformLocation(s_progDown,  "u_dstSize");
    s_uDstSizeH    = glGetUniformLocation(s_progBlurH, "u_dstSize");
    s_uDstSizeV    = glGetUniformLocation(s_progBlurV, "u_dstSize");
    return true;
}

GLuint makeHalfResImage(int w, int h) {
    GLuint t = 0;
    // TEX-CLASS: render-target -- bloom/blur ping-pong target (FBO attach)
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, w, h);
    glBindTexture(GL_TEXTURE_2D, 0);
    return t;
}

void ensurePingPong(int halfW, int halfH) {
    if (s_pingA != 0 && halfW == s_halfW && halfH == s_halfH) return;
    if (s_pingA) { glDeleteTextures(1, &s_pingA); s_pingA = 0; }
    if (s_pingB) { glDeleteTextures(1, &s_pingB); s_pingB = 0; }
    s_pingA = makeHalfResImage(halfW, halfH);
    s_pingB = makeHalfResImage(halfW, halfH);
    s_halfW = halfW; s_halfH = halfH;

    // REGISTRY-COMPUTE-IDS-1: register the live compute-blur output substrate
    // (s_pingA receives the final V-pass output). Observe-only metadata; never
    // read by the draw path. Gated/default-OFF, so this only fires when the
    // compute-blur path runs.
    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::PostprocessComputeBlur;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.lifetime  = RenderCore::RenderResourceLifetime::FrameLocal;  // transient compute-blur substrate, per-frame
        d.format    = RenderCore::RenderResourceFormat::RGBA16F;
        d.debugName = "PostprocessComputeBlur";
        d.width     = static_cast<uint32_t>(halfW);
        d.height    = static_cast<uint32_t>(halfH);
        d.glName    = static_cast<uint32_t>(s_pingA);
        d.sizeBytes = static_cast<uint64_t>(halfW) * halfH * 8;  // RGBA16F = 8 bytes/texel
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    MC2_DIAGFE(stderr, "[POSTPROCESS_BLUR v1] ping-pong %dx%d (RGBA16F x2)\n",
            halfW, halfH);
}

// Deterministic full-res test pattern: a smooth gradient + a couple of sharp
// features so the blur has high-frequency content to attenuate. Uploaded once.
void ensureTestPattern() {
    if (s_testTex != 0) return;
    std::vector<float> px(static_cast<size_t>(kTestSrcW) * kTestSrcH * 4);
    for (int y = 0; y < kTestSrcH; ++y) {
        for (int x = 0; x < kTestSrcW; ++x) {
            size_t i = (static_cast<size_t>(y) * kTestSrcW + x) * 4;
            float gx = static_cast<float>(x) / static_cast<float>((kTestSrcW - 1));
            float gy = static_cast<float>(y) / static_cast<float>((kTestSrcH - 1));
            // R: horizontal gradient. G: vertical gradient. B: checker spikes.
            // A: a single bright dot region (high frequency).
            float checker = (((x >> 2) ^ (y >> 2)) & 1) ? 1.0f : 0.0f;
            float dot = (x == 17 && y == 41) ? 4.0f : 0.0f;
            px[i + 0] = gx;
            px[i + 1] = gy;
            px[i + 2] = checker;
            px[i + 3] = 0.5f + dot;
        }
    }
    s_testTex = makeHalfResImage(kTestSrcW, kTestSrcH); // RGBA16F, sampler-usable
    glBindTexture(GL_TEXTURE_2D, s_testTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kTestSrcW, kTestSrcH,
                    GL_RGBA, GL_FLOAT, px.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

// Dispatch one stage: program, src texture, dst image, dst dims, dst-size uniform.
void dispatchStage(GLuint prog, GLuint srcTex, GLuint dstImg,
                   int dstW, int dstH, GLint uDstSize) {
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0 + kSrcTexUnit);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    glBindImageTexture(kDstImageUnit, dstImg, 0, GL_FALSE, 0,
                       GL_WRITE_ONLY, GL_RGBA16F);
    if (uDstSize >= 0) glUniform2i(uDstSize, dstW, dstH);
    GLuint gx = (GLuint)((dstW + kTileX - 1) / kTileX);
    GLuint gy = (GLuint)((dstH + kTileY - 1) / kTileY);
    glDispatchCompute(gx, gy, 1);
}

// --- CPU reference: separable 5-tap Gaussian over an RGBA float image. -------
// Mirrors the GLSL exactly: CLAMP_TO_EDGE, identical weights, H then V.
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void cpuBlurSeparable(const std::vector<float>& in, std::vector<float>& out,
                      int w, int h) {
    std::vector<float> tmp(static_cast<size_t>(w) * h * 4);
    const float wt[5] = { kW0, kW1, kW2, kW1, kW0 };
    const int   off[5] = { -2, -1, 0, 1, 2 };
    // Horizontal.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c) {
                float acc = 0.0f;
                for (int k = 0; k < 5; ++k) {
                    int sx = clampi(x + off[k], 0, w - 1);
                    acc += wt[k] * in[(static_cast<size_t>(y) * w + sx) * 4 + c];
                }
                tmp[(static_cast<size_t>(y) * w + x) * 4 + c] = acc;
            }
    // Vertical.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c) {
                float acc = 0.0f;
                for (int k = 0; k < 5; ++k) {
                    int sy = clampi(y + off[k], 0, h - 1);
                    acc += wt[k] * tmp[(static_cast<size_t>(sy) * w + x) * 4 + c];
                }
                out[(static_cast<size_t>(y) * w + x) * 4 + c] = acc;
            }
}

// One-shot CPU-vs-GPU parity check on the controlled test pattern.
void runParityCheck() {
    const int sw = kTestSrcW, sh = kTestSrcH;
    const int hw = sw / 2, hh = sh / 2;

    // CPU readback of the SAME test pattern the GPU sampled.
    std::vector<float> src(static_cast<size_t>(sw) * sh * 4);
    glBindTexture(GL_TEXTURE_2D, s_testTex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, src.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // CPU 2x2 box downsample -> half-res.
    std::vector<float> half(static_cast<size_t>(hw) * hh * 4);
    for (int y = 0; y < hh; ++y)
        for (int x = 0; x < hw; ++x)
            for (int c = 0; c < 4; ++c) {
                int bx = x * 2, by = y * 2;
                float s = src[(static_cast<size_t>((by + 0)) * sw + (bx + 0)) * 4 + c]
                        + src[(static_cast<size_t>((by + 0)) * sw + (bx + 1)) * 4 + c]
                        + src[(static_cast<size_t>((by + 1)) * sw + (bx + 0)) * 4 + c]
                        + src[(static_cast<size_t>((by + 1)) * sw + (bx + 1)) * 4 + c];
                half[(static_cast<size_t>(y) * hw + x) * 4 + c] = s * 0.25f;
            }

    // CPU separable Gaussian on the half-res.
    std::vector<float> cpuOut(static_cast<size_t>(hw) * hh * 4);
    cpuBlurSeparable(half, cpuOut, hw, hh);

    if (s_plant && !cpuOut.empty()) {
        cpuOut[0] = -123.0f;  // value the GPU can never produce
        mc2::fmt::print_err("[POSTPROCESS_BLUR v1] PLANT: corrupted CPU texel 0 -> -123.0 (expecting a mismatch below)\n");
    }

    // GPU output lives in ping A (downsample -> blurH -> blurV ends in A). The
    // dispatch already issued the ComputeImageWrite->TextureReadback barrier.
    std::vector<float> gpuOut(static_cast<size_t>(hw) * hh * 4);
    glBindTexture(GL_TEXTURE_2D, s_pingA);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, gpuOut.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // TOLERANCE: 1e-3 relative-ish absolute. Justification — the GPU stores the
    // ping-pong images as RGBA16F (half-float, ~11 bits mantissa => ~5e-4 ulp at
    // unit magnitude), and the Gaussian accumulates in a different float order on
    // GPU (fma/SIMD) vs the CPU's strict left-to-right fp32. Both sources of error
    // are well under 1e-3 for values in [0,4]. A mismatch beyond 1e-3 is a real
    // failure, not numerical noise.
    const float kTol = 1e-3f;
    size_t n = static_cast<size_t>(hw) * hh * 4;
    size_t mismatches = 0;
    float  worst = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        float d = std::fabs(gpuOut[i] - cpuOut[i]);
        if (d > worst) worst = d;
        if (d > kTol) ++mismatches;
    }

    const bool pass = (mismatches == 0);
    MC2_DIAGFE(stderr,
            "[POSTPROCESS_BLUR v1] PARITY %s half=%dx%d texels=%zu mismatches=%zu "
            "worst_delta=%.9g tol=%.1e plant=%d (%s)\n",
            pass ? "PASS" : "FAIL", hw, hh, n / 4, mismatches, worst, kTol,
            s_plant ? 1 : 0,
            s_plant ? "planted-error expects FAIL" : "expects PASS");
    fflush(stderr);
}

// Run the 3-stage pipeline for a given full-res source texture + dims, leaving
// the blurred half-res result in ping A. Issues typed sync edges between stages.
void runPipeline(GLuint srcTex, int fullW, int fullH) {
    int hw = fullW / 2, hh = fullH / 2;
    if (hw <= 0 || hh <= 0) return;
    ensurePingPong(hw, hh);
    if (s_pingA == 0 || s_pingB == 0) return;

    // Stage 1: full-res src -> half-res ping A (2x2 box downsample).
    dispatchStage(s_progDown, srcTex, s_pingA, hw, hh, s_uDstSizeDown);
    // Order the downsample imageStore before blurH samples ping A.
    gpuSyncBarrier(GpuProducer::ComputeImageWrite, GpuConsumer::TextureSample,
                   "postprocess_blur:down->blurH");

    // Stage 2: ping A -> ping B (horizontal Gaussian).
    dispatchStage(s_progBlurH, s_pingA, s_pingB, hw, hh, s_uDstSizeH);
    gpuSyncBarrier(GpuProducer::ComputeImageWrite, GpuConsumer::TextureSample,
                   "postprocess_blur:blurH->blurV");

    // Stage 3: ping B -> ping A (vertical Gaussian). Final result in ping A.
    dispatchStage(s_progBlurV, s_pingB, s_pingA, hw, hh, s_uDstSizeV);

    // Register the producer->consumer edge for a FUTURE consumer that would
    // sample the blurred output as a texture. No consumer exists yet (substrate),
    // so this orders nothing visible — it documents the contract via a typed edge.
    gpuSyncBarrier(GpuProducer::ComputeImageWrite, GpuConsumer::TextureSample,
                   "postprocess_blur:blurV->consumer");

    // Leave no leaked binds for the next pass.
    glBindImageTexture(kDstImageUnit, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glActiveTexture(GL_TEXTURE0 + kSrcTexUnit);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

}  // namespace

bool IsEnabled() {
    if (!s_gateResolved) {
        s_gateResolved = true;
        s_enabled = mc2::gate::get<"MC2_POSTPROCESS_COMPUTE_BLUR">();
        s_verify  = s_enabled && mc2::gate::get<"MC2_POSTPROCESS_COMPUTE_BLUR_VERIFY">();
        s_plant   = s_verify  && mc2::gate::get<"MC2_POSTPROCESS_COMPUTE_BLUR_PLANT">();
        MC2_DIAGFE(stderr, "[POSTPROCESS_BLUR v1] enabled=%d verify=%d plant=%d "
                        "(MC2_POSTPROCESS_COMPUTE_BLUR)\n",
                s_enabled ? 1 : 0, s_verify ? 1 : 0, s_plant ? 1 : 0);
    }
    return s_enabled;
}

void Run(unsigned int srcTex, int width, int height) {
    if (!IsEnabled()) return;          // master gate OFF => true no-op
    if (!ensurePrograms()) return;

    // LIVE ON path: if a feedback-safe scene-color copy is available, blur it.
    // Substrate only — nothing reads the result. Skipped silently when absent.
    if (srcTex != 0 && width > 1 && height > 1) {
        runPipeline((GLuint)srcTex, width, height);
    }

    // PROVABLE acceptance: always blur the controlled test pattern and run the
    // one-shot parity check. Independent of any scene RNG / consumer.
    if (s_verify && !s_verifyDone) {
        s_verifyDone = true;
        ensureTestPattern();
        if (s_testTex != 0) {
            runPipeline(s_testTex, kTestSrcW, kTestSrcH);
            // Order the final imageStore (ping A) before the CPU glGetTexImage.
            gpuSyncBarrier(GpuProducer::ComputeImageWrite,
                           GpuConsumer::TextureReadback,
                           "postprocess_blur:image->readback");
            runParityCheck();
        }
    }
}

void Shutdown() {
    if (s_pingA)   { glDeleteTextures(1, &s_pingA);  s_pingA  = 0; }
    if (s_pingB)   { glDeleteTextures(1, &s_pingB);  s_pingB  = 0; }
    if (s_testTex) { glDeleteTextures(1, &s_testTex); s_testTex = 0; }

    // REGISTRY-COMPUTE-IDS-1: mark the slot unavailable on teardown.
    RenderCore::RenderResourceDesc invalid;
    invalid.id = RenderCore::RenderResourceId::PostprocessComputeBlur;
    RenderCore::registerOrUpdateRenderResource(invalid);
    if (s_progDown)  { glDeleteProgram(s_progDown);  s_progDown  = 0; }
    if (s_progBlurH) { glDeleteProgram(s_progBlurH); s_progBlurH = 0; }
    if (s_progBlurV) { glDeleteProgram(s_progBlurV); s_progBlurV = 0; }
    s_halfW = s_halfH = 0;
}

}  // namespace postprocess_blur
