// gpu_cull_compute.cpp — Track C: GPU cull compute pipeline.
//
// C1a (shadow mode): compute shader runs every frame alongside CPU path.
//   CPU still renders normally — zero change to rendering output.
//   Parity summary: [GPU_CULL v1] event=parity_summary every 600 frames.
//
// C1b (GPU render authority): extends C1a with indirect draw infrastructure.
//   buildIndirectBuffer() called at mission load after static prop registration.
//   compute_dispatch() runs three kernels: cull, patch, rollup.
//   GpuStaticPropBatcher::flush() uses glMultiDrawElementsIndirect.
//
// Default ON. Killswitch: MC2_GPU_CULL=0 disables compute cull.
// Opt-out:    MC2_GPU_CULL=0 and/or MC2_GPU_CULL_SUBSTRATE=0

#include "gpu_cull_compute.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "gpu_cull_substrate.h"
#include "gpu_cull_record.h"
#include "gpu_cull_readback.h"
#include "gos_profiler.h"   // ZoneScopedN / TracyGpuZone (cull-dispatch split)
#include "gos_static_prop_killswitch.h"   // gos_GetTerrainMVPMat4()
#include "gos_static_prop_batcher.h"      // batcher_getTypeCount(), batcher_getTypeDrawInfo()
#include "gl_state_guard.h"               // GlStateGuard slice 1: mc2gl::GlScopedSsboBinding
#include "gos_gpu_sync.h"

#include <GL/glew.h>
#include <gameos.hpp>                     // STOP()
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <string>
#include <vector>
#include "diagnostic_trace.h"
#include "Gates/gate.h"
#include "mc2fmt.h"

// ---------------------------------------------------------------------------
// Trace macro (env-gated, default off)
// ---------------------------------------------------------------------------
static const bool s_computeTrace = mc2::gate::get<"MC2_GPU_CULL_COMPUTE_TRACE">();

// Ownership-parity oracle (Slice A, measurement-only, default off).
// Set MC2_GPU_CULL_OWNERSHIP_PARITY=1 to enable.
static const bool s_ownershipParity = mc2::gate::get<"MC2_GPU_CULL_OWNERSHIP_PARITY">();
#define COMPUTE_TRACE(fmt, ...) \
    do { if (s_computeTrace) { printf("[GPU_CULL v1] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while (0)

namespace gpu_cull {

// ---------------------------------------------------------------------------
// SSBO binding constants
// ---------------------------------------------------------------------------
static constexpr uint32_t DEBUG_SSBO_BINDING      = 9u;   // C1a debug / C1b visibleIds
static constexpr uint32_t BUCKET_COUNTS_BINDING   = 10u;  // C1b perBucketCount + overflow
static constexpr uint32_t BUCKET_CAPS_BINDING     = 11u;  // C1b read-only bucketCapacity + bucketBase
static constexpr uint32_t ACTOR_VIS_BINDING       = 12u;  // C1b per-actor visibility bits
static constexpr uint32_t BLOCK_VIS_BINDING       = 13u;  // C1b per-block visibility bits
static constexpr uint32_t INDIRECT_CMD_BINDING    = 11u;  // patch shader writes indirect cmds (same slot OK — different shader)
static constexpr uint32_t CULL_UBO_BINDING        = 2u;

// ---------------------------------------------------------------------------
// Module state — C1a
// ---------------------------------------------------------------------------
static GLuint    s_computeProgram    = 0;  // C1a cull shader
static GLuint    s_debugSsbo         = 0;  // C1a debug SSBO
static GLuint    s_frustumUbo        = 0;
static GLuint    s_stagingSsbo       = 0;  // staging copy of current ring slot
static uint32_t  s_maxActors         = 0;
static size_t    s_stagingBytes      = 0;
static bool      s_initialized       = false;

// GL timer query.
static GLuint    s_timerQuery        = 0;
static bool      s_timerPending      = false;
static uint64_t  s_lastDispatchNs    = 0;

// 600-frame summary state (C1a).
static uint32_t  s_summaryCount      = 0;
static bool      s_firstSummaryDone  = false;
static uint64_t  s_accFalseNeg       = 0;
static uint64_t  s_accFalsePos       = 0;
static uint32_t  s_dispatchFrames    = 0;

// ---------------------------------------------------------------------------
// Module state — C1b
// ---------------------------------------------------------------------------
static GLuint    s_c1bCullProgram    = 0;  // cull shader with GPU_CULL_C1B_INDIRECT define
static GLuint    s_patchProgram      = 0;  // patch dispatch
static GLuint    s_rollupProgram     = 0;  // block-active rollup

static GLuint    s_indirectCmdBuf    = 0;  // GL_DRAW_INDIRECT_BUFFER
static GLuint    s_visibleIdsBuf     = 0;  // visibleIds[] SSBO (binding 9 in C1b)
static GLuint    s_bucketCountsBuf   = 0;  // perBucketCount[] + overflowCount (binding 10)
static GLuint    s_bucketCapsBuf     = 0;  // bucketCapacity[] + bucketBase[] (binding 11)
static GLuint    s_actorVisBuf       = 0;  // per-actor vis bits (binding 12)
static GLuint    s_blockVisBuf       = 0;  // per-block vis bits (binding 13)

static uint32_t  s_bucketCount       = 0;  // = typeCount at mission load
static uint32_t  s_blockCount        = 0;  // = Terrain::blocksMapSide^2
static bool      s_c1bInitialized    = false;

// ---------------------------------------------------------------------------
// Lazy env probe
// ---------------------------------------------------------------------------
bool compute_isEnabled() {
    static bool s_initialized_flag = false;
    static bool s_enabled = false;
    if (!s_initialized_flag) {
        s_initialized_flag = true;
        s_enabled = mc2::gate::get<"MC2_GPU_CULL">();
    }
    return s_enabled;
}

// Frustum dilation knob (motion-tolerance slice).
// Default 0.08 (8% per-plane half-extent inflation). MC2_GPU_CULL_FRUSTUM_DILATION=0
// reproduces the strict legacy cull. Lazy-evaluated static (canonical shape, see
// gos_terrain_indirect.cpp env wiring).
static float compute_getFrustumDilation() {
    static bool  s_inited = false;
    static float s_value  = 0.08f;
    if (!s_inited) {
        s_inited = true;
        s_value = mc2::gate::get<"MC2_GPU_CULL_FRUSTUM_DILATION">();   // row: default 0.08, outside [0, 1] reads the default
    }
    // Design v4 §6.1 coalesce-armed zero-dilation gate removed.
    // Rationale: this function feeds the actor-level compute shader (gpu_cull.comp);
    // it has no effect on static prop draw commands (GpuStaticPropRegistry path).
    // Zeroing dilation when coalesce armed was targeting the wrong system and
    // eliminated the temporal buffer that absorbs 1-frame readback lag for mechs,
    // causing intro-camera flicker whenever mechs crossed the frustum boundary.
    return s_value;
}

// ---------------------------------------------------------------------------
// Helpers: load + compile a compute shader from file
// ---------------------------------------------------------------------------

static char* load_text_file(const char* fname) {
    FILE* f = fopen(fname, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return nullptr; }
    char* buf = new char[sz + 1];
    if ((long)fread(buf, 1, sz, f) != sz) {
        fclose(f); delete[] buf; return nullptr;
    }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

static GLuint compile_compute_shader(const char** strings, int count) {
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    if (!sh) return 0;

    glShaderSource(sh, count, strings, nullptr);
    glCompileShader(sh);

    GLint status = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    if (!status) {
        GLint logLen = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 0) {
            char* log = new char[logLen];
            glGetShaderInfoLog(sh, logLen, nullptr, log);
            MC2_DIAGF("[GPU_CULL v1] compute shader compile error:\n%s\n", log);
            fflush(stdout);
            delete[] log;
        }
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint link_compute_program(GLuint shader) {
    GLuint prog = glCreateProgram();
    if (!prog) return 0;

    glAttachShader(prog, shader);
    glLinkProgram(prog);

    GLint status = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        GLint logLen = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 0) {
            char* log = new char[logLen];
            glGetProgramInfoLog(prog, logLen, nullptr, log);
            MC2_DIAGF("[GPU_CULL v1] compute program link error:\n%s\n", log);
            fflush(stdout);
            delete[] log;
        }
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// Build a compute program from a source file with optional preamble strings.
// preambles are prepended BEFORE the file content.
static GLuint build_compute_program_from_file(
        const char* fname,
        const std::string* preambles, int nPreambles,
        const char* debugName)
{
    char* fileSrc = load_text_file(fname);
    if (!fileSrc) {
        MC2_DIAGF("[GPU_CULL v1] %s source not found: %s\n", debugName, fname);
        fflush(stdout);
        return 0;
    }

    // Build source string array: version prefix + preambles + file.
    const char* kVersionPrefix = "#version 430\n";
    std::vector<const char*> srcStrs;
    srcStrs.push_back(kVersionPrefix);
    for (int j = 0; j < nPreambles; ++j)
        srcStrs.push_back(preambles[j].c_str());
    srcStrs.push_back(fileSrc);

    GLuint sh = compile_compute_shader(srcStrs.data(), (int)srcStrs.size());
    delete[] fileSrc;
    if (!sh) {
        MC2_DIAGF("[GPU_CULL v1] %s compile failed\n", debugName);
        fflush(stdout);
        return 0;
    }
    GLuint prog = link_compute_program(sh);
    glDeleteShader(sh);
    if (!prog) {
        MC2_DIAGF("[GPU_CULL v1] %s link failed\n", debugName);
        fflush(stdout);
    }
    return prog;
}

// ---------------------------------------------------------------------------
// Build the gpu_cull.comp program with inline #include substitution.
// preambles are prepended after the version string.
// ---------------------------------------------------------------------------
static GLuint build_cull_program(const std::string* preambles, int nPreambles,
                                  const char* debugName)
{
    const char* kPrefix = "#version 430\n";
    char* predicateSrc  = load_text_file("shaders/gpu_cull_predicate.glsl");
    char* computeSrc    = load_text_file("shaders/gpu_cull.comp");
    if (!predicateSrc || !computeSrc) {
        MC2_DIAGF("[GPU_CULL v1] %s source not found (predicate=%s, comp=%s)\n",
               debugName,
               predicateSrc ? "ok" : "MISSING",
               computeSrc   ? "ok" : "MISSING");
        fflush(stdout);
        delete[] predicateSrc;
        delete[] computeSrc;
        return 0;
    }

    // Inline the predicate include.
    static const char* kIncludeLine = "#include <gpu_cull_predicate.glsl>";
    char* incPos = strstr(computeSrc, kIncludeLine);
    std::string fullSrc;
    if (incPos) {
        fullSrc.append(computeSrc, incPos - computeSrc);
        fullSrc += "\n// --- begin gpu_cull_predicate.glsl ---\n";
        fullSrc += predicateSrc;
        fullSrc += "\n// --- end gpu_cull_predicate.glsl ---\n";
        const char* afterInc = incPos + strlen(kIncludeLine);
        fullSrc += afterInc;
    } else {
        fullSrc = computeSrc;
    }
    delete[] predicateSrc;
    delete[] computeSrc;

    // Inject binding constant so shader uses the same value as the C++ side.
    // Single source of truth: gpu_cull_readback.h::READBACK_SSBO_BINDING.
    std::string bindingDef = "#define READBACK_SSBO_BINDING " +
                             std::to_string(gpu_cull::READBACK_SSBO_BINDING) + "\n";

    // Build source list: version + binding-constant + preambles + full source.
    std::vector<const char*> srcStrs;
    srcStrs.push_back(kPrefix);
    srcStrs.push_back(bindingDef.c_str());
    for (int j = 0; j < nPreambles; ++j)
        srcStrs.push_back(preambles[j].c_str());
    srcStrs.push_back(fullSrc.c_str());

    GLuint sh = compile_compute_shader(srcStrs.data(), (int)srcStrs.size());
    if (!sh) { return 0; }

    // Debug: dump concatenated source to file for inspection.
    if (mc2::gate::get<"MC2_GPU_CULL_DUMP_SHADER">()) {
        char fname[64];
        mc2::fmt::to_buf(fname, "shader_dump_{}.glsl", mc2::fmt::safe(debugName ? debugName : "unknown"));
        if (FILE* dumpf = fopen(fname, "w")) {
            for (int k = 0; k < (int)srcStrs.size(); ++k)
                fputs(srcStrs[k], dumpf);
            fclose(dumpf);
            MC2_DIAGF("[GPU_CULL v1] shader dumped to %s\n", fname);
            fflush(stdout);
        }
    }

    GLuint prog = link_compute_program(sh);
    glDeleteShader(sh);
    return prog;
}

// ---------------------------------------------------------------------------
// compute_init
// ---------------------------------------------------------------------------
bool compute_init() {
    if (!compute_isEnabled()) return true;
    if (!substrate_isEnabled()) {
        MC2_DIAGF("[GPU_CULL v1] event=compute_init_skip reason=substrate_disabled\n");
        fflush(stdout);
        if (mc2_diag::tagEnabled("GPU_CULL"))
            mc2_diag::writeEvent("GPU_CULL", 1, 0,
                "{\"event\":\"compute_init_skip\",\"reason\":\"substrate_disabled\"}");
        return true;
    }

    if (s_initialized) {
        compute_shutdown();
    }

    // --- GL version probe ---
    const char* glVer = (const char*)glGetString(GL_VERSION);
    int glMaj = 0, glMin = 0;
    if (glVer) sscanf(glVer, "%d.%d", &glMaj, &glMin);
    const bool computeOk = (glMaj > 4) || (glMaj == 4 && glMin >= 3);
    MC2_DIAGF("[GPU_CULL v1] event=gl_probe version=%d.%d compute=%s\n",
           glMaj, glMin, computeOk ? "ok" : "fail");
    fflush(stdout);
    if (mc2_diag::tagEnabled("GPU_CULL")) {
        char diag_buf[128];
        mc2::fmt::to_buf(diag_buf, "{{\"event\":\"gl_probe\",\"version_major\":{:d},\"version_minor\":{:d},\"compute\":\"{}\"}}", mc2::fmt::as_i32(glMaj), mc2::fmt::as_i32(glMin), mc2::fmt::safe(computeOk ? "ok" : "fail"));
        mc2_diag::writeEvent("GPU_CULL", 1, 0, diag_buf);
    }
    if (!computeOk) {
        STOP(("[GPU_CULL] GL 4.3+ required for compute shaders (got %d.%d)", glMaj, glMin));
        return false;
    }

    // --- C1a cull shader (no C1b define) ---
    // When readback is enabled, also define GPU_CULL_C2_READBACK so the
    // shader writes the readback SSBO in C1a (shadow/diagnostic) mode too.
    {
        std::string c1aPreamble;
        int nC1aPreambles = 0;
        if (readback_isEnabled()) {
            c1aPreamble = "#define GPU_CULL_C2_READBACK 1\n";
            nC1aPreambles = 1;
        }
        GLuint prog = build_cull_program(
            nC1aPreambles > 0 ? &c1aPreamble : nullptr,
            nC1aPreambles, "cull_C1a");
        if (!prog) {
            STOP(("[GPU_CULL] C1a cull compile failed; see log above"));
            return false;
        }
        s_computeProgram = prog;
    }

    // --- C1b cull shader (with GPU_CULL_C1B_INDIRECT define) ---
    // When readback is also enabled, add GPU_CULL_C2_READBACK so the shader
    // writes the secondary readback SSBO at binding 9 (ranged).
    {
        std::string preambles[2];
        int nPreambles = 0;
        preambles[nPreambles++] = "#define GPU_CULL_C1B_INDIRECT 1\n";
        if (readback_isEnabled()) {
            preambles[nPreambles++] = "#define GPU_CULL_C2_READBACK 1\n";
        }
        GLuint prog = build_cull_program(preambles, nPreambles, "cull_C1b");
        if (!prog) {
            MC2_DIAGF("[GPU_CULL v1] event=c1b_cull_compile_fail — C1b indirect draw disabled\n");
            fflush(stdout);
            if (mc2_diag::tagEnabled("GPU_CULL"))
                mc2_diag::writeEvent("GPU_CULL", 1, 0, "{\"event\":\"c1b_cull_compile_fail\"}");
            // Non-fatal: fall back to C1a shadow mode.
        } else {
            MC2_DIAGF("[GPU_CULL v1] event=c1b_cull_ok c2_readback=%d\n",
                   (int)(nPreambles >= 2));
            fflush(stdout);
            if (mc2_diag::tagEnabled("GPU_CULL")) {
                char diag_buf[64];
                mc2::fmt::to_buf(diag_buf, "{{\"event\":\"c1b_cull_ok\",\"c2_readback\":{:d}}}", mc2::fmt::as_i32((int)(nPreambles >= 2)));
                mc2_diag::writeEvent("GPU_CULL", 1, 0, diag_buf);
            }
        }
        s_c1bCullProgram = prog;
    }

    // --- Patch shader (gpu_cull_patch.comp) ---
    {
        GLuint prog = build_compute_program_from_file("shaders/gpu_cull_patch.comp",
                                                       nullptr, 0, "patch");
        if (!prog) {
            MC2_DIAGF("[GPU_CULL v1] event=patch_compile_fail — C1b patch disabled\n");
            fflush(stdout);
        }
        s_patchProgram = prog;
    }

    // --- Rollup shader (gpu_cull_block_rollup.comp) ---
    {
        GLuint prog = build_compute_program_from_file("shaders/gpu_cull_block_rollup.comp",
                                                       nullptr, 0, "rollup");
        if (!prog) {
            MC2_DIAGF("[GPU_CULL v1] event=rollup_compile_fail — C1-RB disabled\n");
            fflush(stdout);
        }
        s_rollupProgram = prog;
    }

    // --- Debug SSBO for C1a mode (binding 9) ---
    // C1b GPU authority flip: static prop records are now appended to the substrate
    // before compute_dispatch(). Size the staging SSBO to match the substrate slot
    // (which includes both dynamic actors AND static prop instances). Use
    // substrate_getSlotBytes() if the substrate is initialized; fall back to 8192
    // records for safety (handles ~100 dynamic + ~8000 static props at wolfman zoom).
    {
        const GLsizeiptr substrateSlotBytes = substrate_getSlotBytes();
        if (substrateSlotBytes > 0) {
            // Derive max actors from slot size: (slotBytes - headerBytes) / recordBytes
            const size_t recordAreaBytes =
                static_cast<size_t>(substrateSlotBytes) - sizeof(GpuActorRecordHeader);
            s_maxActors = static_cast<uint32_t>(recordAreaBytes / sizeof(GpuActorRecord));
        } else {
            // Substrate not yet initialized (C1a init order edge case); use generous default.
            s_maxActors = 8192u;
        }
    }
    const GLsizeiptr debugSsboBytes =
        static_cast<GLsizeiptr>(2 * sizeof(uint32_t) + s_maxActors * sizeof(uint32_t));
    // TIER2-EXCLUDED: substrate-gated
    glGenBuffers(1, &s_debugSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_debugSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, debugSsboBytes, nullptr, GL_DYNAMIC_STORAGE_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // --- Staging SSBO ---
    s_stagingBytes = sizeof(GpuActorRecordHeader) + s_maxActors * sizeof(GpuActorRecord);
    // TIER2-EXCLUDED: substrate-gated
    glGenBuffers(1, &s_stagingSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_stagingSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
                    static_cast<GLsizeiptr>(s_stagingBytes), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // --- Frustum UBO (binding 2) ---
    // Layout (std140): mat4 viewProj (64 B) + 4× uint slot info (16 B) +
    //                  4× float dilation block (16 B) = 96 B total.
    // TIER2-EXCLUDED: substrate-gated
    glGenBuffers(1, &s_frustumUbo);
    glBindBuffer(GL_UNIFORM_BUFFER, s_frustumUbo);
    glBufferStorage(GL_UNIFORM_BUFFER, 96, nullptr, GL_DYNAMIC_STORAGE_BIT);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glBindBufferBase(GL_UNIFORM_BUFFER, CULL_UBO_BINDING, s_frustumUbo);

    // Log dilation setting once at init.
    {
        const float d = compute_getFrustumDilation();
        MC2_DIAGF("[GPU_CULL v1] event=frustum_dilation_init value=%.4f\n", d);
        fflush(stdout);
    }

    // --- GL timer query ---
    glGenQueries(1, &s_timerQuery);
    s_timerPending = false;

    s_initialized      = true;
    s_summaryCount     = 0;
    s_firstSummaryDone = false;
    s_accFalseNeg      = 0;
    s_accFalsePos      = 0;
    s_dispatchFrames   = 0;

    MC2_DIAGF("[GPU_CULL v1] event=compute_selftest result=ok maxActors=%u debugSsboBytes=%lld\n",
           s_maxActors, (long long)debugSsboBytes);
    fflush(stdout);
    if (mc2_diag::tagEnabled("GPU_CULL")) {
        char diag_buf[128];
        mc2::fmt::to_buf(diag_buf, "{{\"event\":\"compute_selftest\",\"result\":\"ok\",\"maxActors\":{:d},\"debugSsboBytes\":{:d}}}", mc2::fmt::as_u32(s_maxActors), mc2::fmt::as_i64((long long)debugSsboBytes));
        mc2_diag::writeEvent("GPU_CULL", 1, 0, diag_buf);
    }
    return true;
}

// ---------------------------------------------------------------------------
// compute_shutdown
// ---------------------------------------------------------------------------
void compute_shutdown() {
    if (!compute_isEnabled()) return;

    // --- C1b buffers ---
    if (s_indirectCmdBuf) { glDeleteBuffers(1, &s_indirectCmdBuf); s_indirectCmdBuf = 0; }
    if (s_visibleIdsBuf)  { glDeleteBuffers(1, &s_visibleIdsBuf);  s_visibleIdsBuf  = 0; }
    if (s_bucketCountsBuf){ glDeleteBuffers(1, &s_bucketCountsBuf);s_bucketCountsBuf= 0; }
    if (s_bucketCapsBuf)  { glDeleteBuffers(1, &s_bucketCapsBuf);  s_bucketCapsBuf  = 0; }
    if (s_actorVisBuf)    { glDeleteBuffers(1, &s_actorVisBuf);    s_actorVisBuf    = 0; }
    if (s_blockVisBuf)    { glDeleteBuffers(1, &s_blockVisBuf);    s_blockVisBuf    = 0; }
    if (s_c1bCullProgram) { glDeleteProgram(s_c1bCullProgram);     s_c1bCullProgram = 0; }
    if (s_patchProgram)   { glDeleteProgram(s_patchProgram);       s_patchProgram   = 0; }
    if (s_rollupProgram)  { glDeleteProgram(s_rollupProgram);      s_rollupProgram  = 0; }
    s_bucketCount      = 0;
    s_blockCount       = 0;
    s_c1bInitialized   = false;

    // --- C1a resources ---
    if (s_timerQuery) {
        if (s_timerPending) {
            GLuint64 dummy = 0;
            glGetQueryObjectui64v(s_timerQuery, GL_QUERY_RESULT, &dummy);
        }
        glDeleteQueries(1, &s_timerQuery);
        s_timerQuery = 0;
        s_timerPending = false;
    }

    if (s_frustumUbo)  { glDeleteBuffers(1, &s_frustumUbo);  s_frustumUbo  = 0; }
    if (s_debugSsbo)   { glDeleteBuffers(1, &s_debugSsbo);   s_debugSsbo   = 0; }
    if (s_stagingSsbo) { glDeleteBuffers(1, &s_stagingSsbo); s_stagingSsbo = 0; }
    if (s_computeProgram) { glDeleteProgram(s_computeProgram); s_computeProgram = 0; }
    s_stagingBytes = 0;

    s_initialized = false;
    s_maxActors   = 0;

    MC2_DIAGF("[GPU_CULL v1] event=compute_shutdown\n");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// compute_buildIndirectBuffer (C1b)
// ---------------------------------------------------------------------------
bool compute_buildIndirectBuffer(uint32_t typeCount) {
    if (!compute_isEnabled() || !s_initialized) return false;
    if (!s_c1bCullProgram || !s_patchProgram) {
        MC2_DIAGF("[GPU_CULL v1] event=c1b_build_skip reason=shaders_missing\n");
        fflush(stdout);
        return false;
    }
    if (typeCount == 0) {
        MC2_DIAGF("[GPU_CULL v1] event=c1b_build_skip reason=no_types\n");
        fflush(stdout);
        // C1b temporal-superset (R8 / Gate2 MAJOR-1): a props -> props-less
        // mission ordering early-returns HERE, before the free at :524-530
        // and the post-:749 one-shot zero, so mission-A's s_blockVisBuf
        // would survive with stale stamps AND s_blockCount!=0 — the shader
        // M3 clamp (uBlockCount!=0) would then be BYPASSED and read stale
        // temporal data. Free the buffer and zero s_blockCount so the M3
        // clamp correctly engages (uBlockCount=0 => effVisible=false).
        // s_c1bInitialized DELIBERATELY stays true (Gate1 MINOR-1):
        // s_blockCount=0 is the intended M3-clamp trigger, NOT a
        // s_c1bInitialized reset. CO-DEPENDENCY: this free is only safe
        // WITH the Slice-2 per-frame-clear deletion in the SAME commit
        // (R10) — a surviving clear would hit s_blockVisBuf==0 and raise
        // per-frame GL_INVALID_OPERATION on a props->props-less mission.
        if (s_blockVisBuf) { glDeleteBuffers(1, &s_blockVisBuf); s_blockVisBuf = 0; }
        s_blockCount = 0;
        return true;  // not an error — mission has no static props
    }

    // Free any previous allocation (e.g., from a previous mission).
    if (s_indirectCmdBuf) { glDeleteBuffers(1, &s_indirectCmdBuf); s_indirectCmdBuf = 0; }
    if (s_visibleIdsBuf)  { glDeleteBuffers(1, &s_visibleIdsBuf);  s_visibleIdsBuf  = 0; }
    if (s_bucketCountsBuf){ glDeleteBuffers(1, &s_bucketCountsBuf);s_bucketCountsBuf= 0; }
    if (s_bucketCapsBuf)  { glDeleteBuffers(1, &s_bucketCapsBuf);  s_bucketCapsBuf  = 0; }
    if (s_actorVisBuf)    { glDeleteBuffers(1, &s_actorVisBuf);    s_actorVisBuf    = 0; }
    if (s_blockVisBuf)    { glDeleteBuffers(1, &s_blockVisBuf);    s_blockVisBuf    = 0; }

    s_bucketCount = typeCount;

    // --- DrawElementsIndirectCommand struct (20 bytes) ---
    // count, instanceCount, firstIndex, baseVertex, baseInstance
    struct DrawCmd {
        GLuint count;
        GLuint instanceCount;  // GPU writes this each frame via patch dispatch
        GLuint firstIndex;
        GLint  baseVertex;
        GLuint baseInstance;   // cumulative base into visibleIds[] for this bucket
    };
    static_assert(sizeof(DrawCmd) == 20, "DrawElementsIndirectCommand must be 20 bytes");

    // 2026-05-11 per-packet rework: in the SORTED (coalesce) branch, cmds[]
    // holds one entry per PACKET (not per type). cmd count = sum of all
    // types' packetCount. In the NATURAL branch, cmds[] still holds one
    // per type. Allocate up-front using the coalesce-layout's packet count
    // when available; the natural branch resizes back to typeCount below.
    uint32_t cmdCount = typeCount;
    if (batcher_isCoalesceLayoutReady()) {
        const uint32_t pktCount = batcher_getSortedPacketCount();
        if (pktCount > 0u) cmdCount = pktCount;
    }
    std::vector<DrawCmd> cmds(cmdCount);
    std::vector<uint32_t> bucketCaps(typeCount);
    std::vector<uint32_t> bucketBases(typeCount);  // packed into second half of bucketCapsBuf

    // Plan v3.8 Step 9.1 — function-scope cumBase. BOTH the sorted (coalesce)
    // and natural (legacy) branches accumulate into the SAME counter so the
    // visibleIds[] allocation at totalVisibleSlots below is correct under
    // either layout.
    uint32_t cumBase = 0;

    if (batcher_isCoalesceLayoutReady()) {
        // ---- SORTED (coalesce) branch — per-packet rework ----
        // One DrawCmd per packet in s_sortedPacketOrder. All packets of one
        // type share the same baseInstance (group-relative cap accumulator),
        // since they read from the same per-frame instance range. The patch
        // shader writes per-type instanceCount into all packet-cmds of that
        // type via the cmd_to_bucket lookup (built CPU-side here, used by
        // gpu_cull_patch.comp at GPU dispatch).
        const uint32_t* sortedPackets = batcher_getSortedPacketOrder();
        const uint32_t  N_offCmds     = batcher_getAlphaOffCmdCount();

        // Per-type group-relative baseInstance (Step 4.9: only computed under legacy;
        // under global-pool the patch shader writes baseInstance per-frame).
        std::vector<uint32_t> baseInstanceForType(typeCount, 0u);
        if (batcher_isGlobalPoolLegacy()) {
            const uint32_t* sortedTypes = batcher_getSortedTypeOrder();
            const uint32_t  N_offTypes  = batcher_getAlphaOffCount();
            uint32_t cumCapOff = 0, cumCapOn = 0;
            for (uint32_t i = 0; i < typeCount; ++i) {
                const uint32_t typeID = sortedTypes ? sortedTypes[i] : i;
                if (typeID >= typeCount) continue;
                const uint32_t coalesceInstanceCap = batcher_getInstanceCap(typeID);
                if (i < N_offTypes) {
                    baseInstanceForType[typeID] = cumCapOff;
                    cumCapOff += coalesceInstanceCap;
                } else {
                    baseInstanceForType[typeID] = cumCapOn;
                    cumCapOn += coalesceInstanceCap;
                }
            }
        }
        // else: baseInstanceForType stays 0 — patched per-frame by patch shader via slot 16.

        // Build per-type bucket layout for the cull stage.
        for (uint32_t t = 0; t < typeCount; ++t) {
            uint32_t indexCountSum = 0, firstIdx0 = 0;
            int32_t  baseVtx0 = 0;
            uint32_t legacyVisibleIdsCap = 0;
            const bool ok = batcher_getTypeDrawInfo(t, &indexCountSum,
                                                    &firstIdx0, &baseVtx0,
                                                    &legacyVisibleIdsCap);
            (void)indexCountSum; (void)firstIdx0; (void)baseVtx0;
            if (!ok) {
                bucketCaps[t]  = 0;
                bucketBases[t] = cumBase;
                continue;
            }
            // Step 4.9 — global-pool mode: all buckets get globalInstanceCap so
            // visibleIds[] can hold up to globalCap IDs per type.
            if (!batcher_isGlobalPoolLegacy()) {
                bucketCaps[t] = batcher_getGlobalInstanceCap();
            } else {
                bucketCaps[t] = legacyVisibleIdsCap;
            }
            bucketBases[t] = cumBase;
            cumBase       += bucketCaps[t];
        }

        // Build per-packet cmds.
        const uint32_t pktCount = batcher_getSortedPacketCount();
        for (uint32_t i = 0; i < pktCount; ++i) {
            const uint32_t globalPktIdx = sortedPackets ? sortedPackets[i] : i;
            uint32_t pktIdxCount = 0, pktFirstIdx = 0, owningTypeID = 0;
            int32_t  pktBaseVtx = 0;
            const bool pkOk = batcher_getPacketDrawInfo(globalPktIdx,
                                                        &pktIdxCount,
                                                        &pktFirstIdx,
                                                        &pktBaseVtx,
                                                        &owningTypeID);
            DrawCmd& cmd = cmds[i];
            if (!pkOk) {
                cmd = {0, 0, 0, 0, 0};
                continue;
            }
            cmd.count         = pktIdxCount;
            cmd.instanceCount = 0;       // GPU patches per-frame
            cmd.firstIndex    = pktFirstIdx;
            cmd.baseVertex    = pktBaseVtx;
            // Step 4.9: under global-pool mode, baseInstance is 0 at build time
            // and patched per-frame by the patch shader via baseInstanceByCmd[].
            cmd.baseInstance  = batcher_isGlobalPoolLegacy()
                                  ? ((owningTypeID < typeCount) ? baseInstanceForType[owningTypeID] : 0u)
                                  : 0u;
            COMPUTE_TRACE("c1b_build pkt sortedSlot=%u type=%u globalPkt=%u "
                          "idxCount=%u firstIdx=%u baseVtx=%d baseInst=%u group=%s",
                          i, owningTypeID, globalPktIdx,
                          cmd.count, cmd.firstIndex, cmd.baseVertex,
                          cmd.baseInstance, i < N_offCmds ? "off" : "on");
        }
    } else {
        // ---- NATURAL (legacy / coalesce-disabled) branch — Step 9.3 ----
        // Semantically preserved from prior `:548-572`. Local rename
        // `instanceCap` → `legacyVisibleIdsCap` per §5.6a.
        for (uint32_t t = 0; t < typeCount; ++t) {
            uint32_t indexCount = 0, firstIndex = 0;
            uint32_t legacyVisibleIdsCap = 0;
            int32_t  baseVertex = 0;
            if (!batcher_getTypeDrawInfo(t, &indexCount, &firstIndex,
                                         &baseVertex, &legacyVisibleIdsCap)) {
                // Type has no geometry (empty type registered). Fill zeros.
                cmds[t]       = {0, 0, 0, 0, cumBase};
                bucketCaps[t] = 0;
                bucketBases[t]= cumBase;
                COMPUTE_TRACE("c1b_build type=%u EMPTY (natural)", t);
                continue;
            }
            cmds[t].count         = indexCount;
            cmds[t].instanceCount = 0;        // GPU writes per-frame
            cmds[t].firstIndex    = firstIndex;
            cmds[t].baseVertex    = baseVertex;
            cmds[t].baseInstance  = cumBase;  // base offset into visibleIds[]

            bucketCaps[t]  = legacyVisibleIdsCap;
            bucketBases[t] = cumBase;
            cumBase       += legacyVisibleIdsCap;

            COMPUTE_TRACE("c1b_build type=%u idxCount=%u firstIdx=%u baseVtx=%d "
                          "legacyCap=%u visBase=%u (natural)",
                          t, indexCount, firstIndex, baseVertex,
                          legacyVisibleIdsCap, cmds[t].baseInstance);
        }
    }

    const uint32_t totalVisibleSlots = cumBase;  // sum of all per-type visibleIds caps

    // --- Indirect command buffer (GL_DRAW_INDIRECT_BUFFER, GL_DYNAMIC_DRAW) ---
    // instanceCount is overwritten every frame by the patch dispatch.
    // 2026-05-11: cmds.size() is now per-packet under coalesce-armed (per-type
    // under coalesce-disarmed/natural). Use cmds.size() instead of typeCount.
    const GLsizeiptr indirectBytes = static_cast<GLsizeiptr>(cmds.size() * sizeof(DrawCmd));
    // TIER2-EXCLUDED: substrate-gated
    glGenBuffers(1, &s_indirectCmdBuf);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, s_indirectCmdBuf);
    glBufferData(GL_DRAW_INDIRECT_BUFFER, indirectBytes, cmds.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

    // --- VisibleIds SSBO (binding 9 in C1b cull shader) ---
    // Sized: sum of all per-type instance caps × sizeof(uint32_t).
    // Written per-frame by the cull scatter shader (atomicAdd-based slot
    // assignment). NOT cleared per-frame — stale slots past the current
    // frame's bucket counts are never read (the draw path iterates only
    // up to the cleared bucketCounts[b] entries). Initial nullptr content
    // is safe for the same reason: the cleared counts on frame 0 bound
    // any reads to slots the shader actually wrote.
    const GLsizeiptr visIdsBytes = static_cast<GLsizeiptr>(
        (totalVisibleSlots > 0 ? totalVisibleSlots : 1) * sizeof(uint32_t));
    // TIER2-EXCLUDED: substrate-gated
    glGenBuffers(1, &s_visibleIdsBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_visibleIdsBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, visIdsBytes, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // --- BucketCounts SSBO (binding 10) ---
    // Layout: [perBucketCount[0..N-1], overflowCount]  (N+1 uints)
    const GLsizeiptr countsBytes = static_cast<GLsizeiptr>((typeCount + 1) * sizeof(uint32_t));
    // TIER2-EXCLUDED: substrate-gated
    glGenBuffers(1, &s_bucketCountsBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_bucketCountsBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, countsBytes, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // --- BucketCaps SSBO (binding 11 in C1b cull shader, readonly) ---
    // Layout: [bucketCapacity[0..N-1], bucketBase[0..N-1]]  (2*N uints)
    // Combined into one buffer; the shader addresses the second half via
    // the declared layout (see gpu_cull.comp BucketCaps block).
    std::vector<uint32_t> capsAndBases(typeCount * 2);
    for (uint32_t t = 0; t < typeCount; ++t) {
        capsAndBases[t]            = bucketCaps[t];
        capsAndBases[typeCount + t]= bucketBases[t];
    }
    const GLsizeiptr capsBytes = static_cast<GLsizeiptr>(typeCount * 2 * sizeof(uint32_t));
    // TIER2-EXCLUDED: substrate-gated
    glGenBuffers(1, &s_bucketCapsBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_bucketCapsBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, capsBytes, capsAndBases.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // --- ActorVis SSBO (binding 12) ---
    // Per-actor visibility bit written by cull shader.
    const GLsizeiptr actorVisBytes = static_cast<GLsizeiptr>(s_maxActors * sizeof(uint32_t));
    // TIER2-EXCLUDED: substrate-gated
    glGenBuffers(1, &s_actorVisBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_actorVisBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, actorVisBytes, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // --- BlockVis SSBO (binding 13) ---
    // One uint per terrain block. Sized conservatively; Terrain::blocksMapSide
    // is set at mission load. For typical MC2 maps blocksMapSide=10 → 100 blocks.
    // Allocate for the worst-case 128×128 blocks (same headroom as s_maxActors).
    s_blockCount = 128u * 128u;
    const GLsizeiptr blockVisBytes = static_cast<GLsizeiptr>(s_blockCount * sizeof(uint32_t));
    // TIER2-EXCLUDED: substrate-gated
    glGenBuffers(1, &s_blockVisBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_blockVisBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, blockVisBytes, nullptr, GL_DYNAMIC_DRAW);
    // C1b STICKY-BIT INVARIANT (v3 §2.5.3): this is the SOLE clearing point
    // for blockVisBits[] during normal mission lifecycle. Under sticky-bit
    // (atomicOr(...,1u)), the per-frame zero is INTENTIONALLY ABSENT — a
    // per-frame zero would degrade sticky to strict-only and re-arm every
    // block to "never seen" every frame. The only other paths that reset
    // state are the props-less free at :541-542 (s_blockVisBuf=0) and the
    // Mission::destroy free at :483-488. compute_buildIndirectBuffer is
    // per-mission (free-prev above + realloc here), so this zero fires once
    // per mission boundary including savegame restore (after Mission::load
    // gained the matching compute_buildIndirectBuffer call at saveload.cpp
    // §0 prerequisite). Do NOT fold this clear into a per-frame clear; do
    // NOT remove it.
    {
        const GLuint zero = 0u;
        glClearNamedBufferSubData(s_blockVisBuf, GL_R32UI, 0,
                                  blockVisBytes,
                                  GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    s_c1bInitialized = true;

    MC2_DIAGF("[GPU_CULL v1] event=c1b_build_ok"
           " typeCount=%u totalVisSlots=%u"
           " indirectBytes=%lld visIdsBytes=%lld\n",
           typeCount, totalVisibleSlots,
           (long long)indirectBytes, (long long)visIdsBytes);
    fflush(stdout);
    return true;
}

GLuint compute_getIndirectCmdBuf() {
    return s_indirectCmdBuf;
}

GLuint compute_getBucketCountsBuf() {
    return s_bucketCountsBuf;
}

uint32_t compute_getBucketCount() {
    return s_bucketCount;
}

bool compute_getTypeBucketInfo(uint32_t typeID,
                                uint32_t* outIndexCount,
                                uint32_t* outFirstIndex,
                                int32_t*  outBaseVertex,
                                uint32_t* outInstanceCap) {
    return batcher_getTypeDrawInfo(typeID, outIndexCount, outFirstIndex, outBaseVertex, outInstanceCap);
}

// ---------------------------------------------------------------------------
// compute_dispatch
// ---------------------------------------------------------------------------
void compute_dispatch() {
    if (!compute_isEnabled() || !s_initialized) return;

    // C2 frame begin: non-blocking tryConsume of last frame's readback.
    // Must be called BEFORE the compute shader is dispatched.
    if (readback_isEnabled()) {
        TracyGpuZone("Cull.Readback");
        readback_tryConsume();
    }

    // C2: static frame counter for readback_frameEnd().
    static uint32_t s_readbackFrameCounter = 0u;

    const float* mvp = gos_GetTerrainMVPMat4();
    if (!mvp) {
        COMPUTE_TRACE("event=dispatch_skip reason=mvp_null");
        return;
    }

    // Upload view-projection matrix to frustum UBO.
    glBindBuffer(GL_UNIFORM_BUFFER, s_frustumUbo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, 16 * sizeof(float), mvp);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    COMPUTE_TRACE("mvp row3=(%.4f,%.4f,%.4f,%.4f)",
                  mvp[3],mvp[7],mvp[11],mvp[15]);

    // Copy active ring slot to staging SSBO (offset 0).
    GLuint substrateSsbo = substrate_getInstanceSsboName();
    if (!substrateSsbo || !s_stagingSsbo) {
        COMPUTE_TRACE("event=dispatch_skip reason=ssbo_not_ready");
        return;
    }
    const GLintptr   slotOffset = substrate_getCurrentSlotOffset();
    const GLsizeiptr slotBytes  = substrate_getSlotBytes();
    if (slotBytes == 0) {
        COMPUTE_TRACE("event=dispatch_skip reason=slot_bytes_zero");
        return;
    }
    const GLsizeiptr copyBytes =
        (slotBytes <= (GLsizeiptr)s_stagingBytes) ? slotBytes : (GLsizeiptr)s_stagingBytes;

    // NVIDIA-COHERENCE-1: the substrate buffer is CPU-written through a persistent
    // GL_MAP_COHERENT_BIT mapping (substrate_appendStaticPropRecord). Coherent
    // mapping makes those writes visible without an explicit flush, but ordering
    // them BEFORE this server-side glCopyBufferSubData read still requires
    // GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT. AMD RDNA3 happens to serialize it;
    // NVIDIA may copy pre-write (stale/zero) record bytes -> the cull frustum-test
    // rejects every static prop -> instanceCount=0 -> trees invisible in the COLOR
    // pass while still casting shadows (the shadow path is cull-independent).
    // Candidate fix for the NVIDIA invisible-trees report; AMD-safe (no-op when
    // already coherent), near-zero cost.
    glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
    { TracyGpuZone("Cull.CopyStaging");
    glBindBuffer(GL_COPY_READ_BUFFER,  substrateSsbo);
    glBindBuffer(GL_COPY_WRITE_BUFFER, s_stagingSsbo);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                        slotOffset, 0, copyBytes);
    glBindBuffer(GL_COPY_READ_BUFFER,  0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    }

    // Record count: read the CPU-side counter directly. The value was just
    // copied from substrate's persistent-mapped buffer (where it lives in
    // hdr->recordCount, written from CPU by substrate_appendStaticPropRecord
    // each leaf). Reading via glGetBufferSubData here forced a full GPU sync
    // because the just-issued glCopyBufferSubData hadn't completed — that's a
    // ~6 ms/frame stall on a busy substrate-coalesce frame (mc2_10 substrate=ON
    // dropped ~135 fps → ~62 fps before this fix; 2026-05-11). The header is
    // 4 bytes on both sides; CPU side is authoritative.
    const uint32_t recordCount = substrate_getCurrentRecordCount();
    if (recordCount == 0) {
        COMPUTE_TRACE("event=dispatch_skip reason=record_count_zero");
        return;
    }

    // Bind staging SSBO at binding 8.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,
                     substrate_getInstanceSsboBindingPoint(), s_stagingSsbo);

    // Bind frustum UBO.
    {
        uint32_t slotInfo[4] = { 0u, recordCount, 0u, 0u };
        const float dilation = compute_getFrustumDilation();
        const float dilationBlock[4] = { dilation, 0.0f, 0.0f, 0.0f };
        glBindBuffer(GL_UNIFORM_BUFFER, s_frustumUbo);
        glBufferSubData(GL_UNIFORM_BUFFER, 64, sizeof(slotInfo),     slotInfo);
        glBufferSubData(GL_UNIFORM_BUFFER, 80, sizeof(dilationBlock),dilationBlock);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, CULL_UBO_BINDING, s_frustumUbo);

    // C2: bind readback SSBO at binding 14 (ranged, per-slot).
    // Binding READBACK_SSBO_BINDING (defined in gpu_cull_readback.h) — single source of truth.
    // Zero the rb_visibleCount in the mapped header before dispatch so the shader's
    // atomicAdd accumulates from 0 each frame. The buffer is GL_MAP_COHERENT_BIT so
    // the write is immediately visible to the GPU.
    if (readback_isEnabled()) {
        const GLuint     rbBuf    = readback_getSsboBuf();
        const GLintptr   rbOff    = readback_getCurrentSlotOffset();
        const GLsizeiptr rbSz     = readback_getSlotBytes();
        if (rbBuf && rbSz > 0) {
            // Zero rb_visibleCount in the GPU SSBO via glClearNamedBufferSubData.
            readback_zeroCurrentSlotVisibleCount();
            // GL_SHADER_STORAGE_BARRIER_BIT: ensure the glClear write is visible
            // to the compute shader before it dispatches. Without this barrier,
            // on AMD the clear and dispatch may be re-ordered such that atomicAdd
            // sees the un-cleared (garbage) value or the write is discarded.
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            // Drain any prior GL errors so the check below is clean.
            while (glGetError() != GL_NO_ERROR) {}
            gpuBindSsboRange(gpu_cull::READBACK_SSBO_BINDING, (unsigned int)rbBuf,
                             (long long)rbOff, (long long)rbSz,
                             "gpu_cull.readback");
            // Check for GL_INVALID_VALUE (misalignment, out-of-range, etc).
            const GLenum bindErr = glGetError();
            if (bindErr != GL_NO_ERROR) {
                MC2_DIAGF("[GPU_CULL v1] WARN: glBindBufferRange binding=14 error=0x%X rbOff=%lld rbSz=%lld\n",
                       bindErr, (long long)rbOff, (long long)rbSz);
                fflush(stdout);
            }
            COMPUTE_TRACE("event=c2_bind rbBuf=%u rbOff=%lld rbSz=%lld binding=%u bindErr=0x%X",
                           rbBuf, (long long)rbOff, (long long)rbSz, gpu_cull::READBACK_SSBO_BINDING, bindErr);
        }
    }

    // Begin GL timer query.
    // C-03: check availability before reading to avoid a blocking stall when the
    // GPU is behind.  s_lastDispatchNs retains the previous frame's value when the
    // result is not yet ready — acceptable for telemetry (perf-stat log only).
    if (s_timerPending) {
        GLint available = 0;
        glGetQueryObjectiv(s_timerQuery, GL_QUERY_RESULT_AVAILABLE, &available);
        if (available) {
            glGetQueryObjectui64v(s_timerQuery, GL_QUERY_RESULT, &s_lastDispatchNs);
            s_timerPending = false;
        }
    }
    glBeginQuery(GL_TIME_ELAPSED, s_timerQuery);

    const uint32_t cullGroups = (recordCount + 63u) / 64u;

    if (s_c1bInitialized && s_c1bCullProgram && s_patchProgram) {
        // ========================================================
        // C1b path: full indirect draw authority
        // ========================================================

        // 1. Reset per-bucket counters (perBucketCount[] + overflowCount) + actorVisBits[].
        // NOTE (R10/Gate2 MAJOR-1 co-dependency): the per-frame
        // glClearNamedBufferSubData(s_blockVisBuf,...) that USED to live
        // here was DELETED in 056c365 and STAYS DELETED under sticky-bit
        // (v3 §2.5) — a per-frame zero would degrade sticky to strict-only
        // by re-arming every block to "never seen" each frame. The one-shot
        // per-mission zero in compute_buildIndirectBuffer
        // (post-glBufferData) is the sole initializer. Deleting this clear
        // is also what makes the typeCount==0 free (s_blockVisBuf=0) safe —
        // a surviving clear would raise per-frame GL_INVALID_OPERATION on
        // props->props-less.
        {
            TracyGpuZone("Cull.Clears");
            const GLuint zero = 0u;
            const uint32_t effectiveCount = recordCount <= s_maxActors ? recordCount : s_maxActors;
            glClearNamedBufferSubData(s_bucketCountsBuf, GL_R32UI, 0,
                                      static_cast<GLsizeiptr>((s_bucketCount + 1) * sizeof(GLuint)),
                                      GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
            glClearNamedBufferSubData(s_actorVisBuf, GL_R32UI, 0,
                                      static_cast<GLsizeiptr>(effectiveCount * sizeof(GLuint)),
                                      GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        }

        // NVIDIA-CLEAR-RACE-1: order the counter clears above before the cull
        // dispatch below. glClearNamedBufferSubData is a buffer-update that the
        // dispatch's atomicAdd(bucketCountData) reads; without this barrier NVIDIA
        // may reorder the clear after the dispatch -> atomics see stale (large)
        // counts -> slot>=cap -> nothing scattered into visibleIds -> the patch
        // shader writes instanceCount=0 -> color pass draws no props (shadows,
        // which are cull-independent, still draw them). The C2 readback path
        // already issues this barrier after its identical clear; the C1b path was
        // missing it. AMD-safe. Candidate fix for the NVIDIA invisible-trees bug.
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

        // 2. Bind C1b SSBOs and cull dispatch.
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, DEBUG_SSBO_BINDING,    s_visibleIdsBuf);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BUCKET_COUNTS_BINDING, s_bucketCountsBuf);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BUCKET_CAPS_BINDING,   s_bucketCapsBuf);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ACTOR_VIS_BINDING,     s_actorVisBuf);
        // C1b temporal-superset (R1): the cull program reads binding 13;
        // its bind site previously bound NO block buffer (only the rollup
        // at :1064 did). When s_blockVisBuf==0 the shader's M3 clamp
        // (uBlockCount==0) means binding 13 is never indexed — safe to
        // bind 0 here.
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BLOCK_VIS_BINDING,     s_blockVisBuf);

        glUseProgram(s_c1bCullProgram);
        {
            const GLint locNB = glGetUniformLocation(s_c1bCullProgram, "u_nBuckets");
            if (locNB >= 0)
                glUniform1i(locNB, (int)s_bucketCount);
            // C1b sticky-bit temporal-superset M3 fail-OPEN clamp: uBlockCount
            // is s_blockCount only when the buffer is allocated, else 0 — the
            // shader-side short-circuit && on rec.blockIdx < uint(uBlockCount)
            // guarantees binding 13 is never indexed when the block buffer is
            // unallocated. The K-window uniforms (uFrameStamp + uHistoryK) and
            // their lockstep s_cullFrameIdx counter retired under v3 §2.5;
            // sticky-bit is bound-by-construction (atomicOr(...,1u)) and
            // needs no per-dispatch stamp.
            const GLint locBC = glGetUniformLocation(s_c1bCullProgram, "uBlockCount");
            if (locBC >= 0)
                glUniform1i(locBC, s_blockVisBuf ? (int)s_blockCount : 0);
        }
        { TracyGpuZone("Cull.CullShader"); glDispatchCompute(cullGroups, 1, 1); }
        glUseProgram(0);

        // 3. Barrier: SSBO writes (visibleIds, perBucketCount, actorVisBits) ready.
        // DO NOT add GL_ATOMIC_COUNTER_BARRIER_BIT — no ACBOs (Q12 contract).
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

        // 3a. Ownership-parity oracle (Slice A — MEASUREMENT ONLY, no behavior change).
        // Quantifies the H4 false-negative rate: the GPU computes a compacted
        // visibleIds[] set but the draw renders the first-N instances in SSBO
        // order; GPU-visible instances at high indices get cut off on camera
        // motion.  This readback+log measures how many.
        // Env gate: MC2_GPU_CULL_OWNERSHIP_PARITY (default off).
        // Readback stalls the GPU — fire at most every 300 calls.
        if (s_ownershipParity &&
            s_visibleIdsBuf && s_bucketCountsBuf && s_bucketCapsBuf)
        {
            static uint32_t s_parityCallCounter = 0u;
            ++s_parityCallCounter;
            if (s_parityCallCounter >= 300u)
            {
                s_parityCallCounter = 0u;

                const int typeCount = static_cast<int>(s_bucketCount);
                if (typeCount > 0)
                {
                    // --- CHEAP CUT-OFF UPPER-BOUND (no record->slot map) ---
                    // Recon H4: the draw renders the FIRST `count` instances of
                    // each type's FROZEN instance pool (binding 0); the GPU cull
                    // selects a SCATTERED subset (visibleIds, which the VS never
                    // reads). record-index space (binding 8, per-frame, scene
                    // order) and instance-pool-slot space (binding 0, frozen,
                    // per-type sorted) are DIVERGED, so a sound per-instance
                    // false-negative count needs an authoritative
                    // record->draw-slot map (NOT done here; see the spec note
                    // 2026-06-04-gpu-cull-oracle-fix-notes.md). The prior
                    // "100% false-neg" / "garbage base" prints were artifacts of
                    // comparing record-index against a visibleIds buffer-offset.
                    //
                    // This is the cheap NECESSARY-CONDITION proxy: per type the
                    // draw can only cut a prop off when the GPU-visible count is
                    // LESS than the pool span (it renders count of pool, omitting
                    // the rest). omittedSlots/pool bounds how much cut-off is even
                    // POSSIBLE. ~0% => cull renders ~everything => H4 immaterial.
                    // Reads only perBucketCount (binding 10) + a CPU per-type pool
                    // count; no big readback, no index-space mixing.
                    std::vector<uint32_t> countsAndOverflow(
                        static_cast<size_t>(typeCount) + 1u, 0u);
                    glGetNamedBufferSubData(s_bucketCountsBuf,
                                           0,
                                           static_cast<GLsizeiptr>((typeCount + 1) * (int)sizeof(uint32_t)),
                                           countsAndOverflow.data());
                    const uint32_t overflow = countsAndOverflow[static_cast<size_t>(typeCount)];

                    uint32_t totalPool = 0u, totalVisible = 0u, totalOmitted = 0u;
                    int      typesCutoffPossible = 0, typesEmptyPoolButVisible = 0;
                    uint32_t worstOmit = 0u; int worstType = -1;
                    uint32_t worstPool = 0u, worstVis = 0u;

                    for (int t = 0; t < typeCount; ++t)
                    {
                        const uint32_t vis = countsAndOverflow[static_cast<size_t>(t)];
                        // t == typeID: the cull build passes t to
                        // batcher_getTypeDrawInfo, and the shader bucket =
                        // category>>4 == typeID. So index the pool by t directly.
                        const uint32_t pool = batcher_getTypeUploadedInstanceCount(
                                                  static_cast<uint32_t>(t));
                        if (pool == 0u) { if (vis > 0u) ++typesEmptyPoolButVisible; continue; }
                        totalPool    += pool;
                        totalVisible += (vis < pool) ? vis : pool;  // clamp stale-pool frame
                        if (vis < pool)
                        {
                            ++typesCutoffPossible;
                            const uint32_t omit = pool - vis;  // pool slots the draw omits
                            totalOmitted += omit;
                            if (omit > worstOmit) { worstOmit = omit; worstType = t; worstPool = pool; worstVis = vis; }
                        }
                    }

                    mc2::fmt::print_out("[GPU_CULL_CUTOFF_UPPERBOUND v2] types={:d} pool={:d} visible={:d} omittedSlots={:d} ({:.1f}% of pool) typesCutoffPossible={:d} emptyPoolButVisible={:d} overflow={:d} | worst type={:d} pool={:d} vis={:d} omit={:d}\n", mc2::fmt::as_i32(typeCount), mc2::fmt::as_u32(totalPool), mc2::fmt::as_u32(totalVisible), mc2::fmt::as_u32(totalOmitted), totalPool ? 100.0 * totalOmitted / totalPool : 0.0, mc2::fmt::as_i32(typesCutoffPossible), mc2::fmt::as_i32(typesEmptyPoolButVisible), mc2::fmt::as_u32(overflow), mc2::fmt::as_i32(worstType), mc2::fmt::as_u32(worstPool), mc2::fmt::as_u32(worstVis), mc2::fmt::as_u32(worstOmit));
                    fflush(stdout);
                }
            }
        }

        // 4. Patch dispatch: perBucketCount[] → cmds[].instanceCount
        {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BUCKET_COUNTS_BINDING, s_bucketCountsBuf);
            // Bind indirect cmd buf as SSBO for patch shader (binding 11).
            // The patch shader writes via binding 11; the draw later uses
            // GL_DRAW_INDIRECT_BUFFER binding. Both can alias the same buffer.
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, INDIRECT_CMD_BINDING, s_indirectCmdBuf);

            // ---- Plan v3.8 Step 10.3: slot-15 save → bind → dispatch → restore ----
            // Slot 15 history: 13 collided with BlockVis at
            // gpu_cull_block_rollup.comp:58; 14 collided with the
            // diagnostic readback at :855 (READBACK_SSBO_BINDING). Slot
            // 15 is private to gpu_cull_patch.comp; this rebind is
            // mandatory for legacy-fallback correctness (patch shader
            // writes through permutation[] in both armed and disarmed
            // modes, so the permutation SSBO must be bound every patch
            // dispatch). The save/restore envelope honors spec §3.X /
            // §9 binding hygiene at the slot-15 mutation site — the
            // draw branch in flush() does NOT bind slot 15, so its
            // 11.7.a/j envelope covers slot 4 + 2D_ARRAY only.
            //
            // No per-frame barrier needed for the permutation SSBO:
            // s_permutationSsbo is finalize-uploaded by Step 2.6's
            // glBufferData (and at most overwritten once by Step 5.12's
            // glBufferSubData in the same finalize pass) and is read-only
            // thereafter. GL ordering guarantees the upload is visible to
            // all subsequent dispatches without an explicit
            // glMemoryBarrier. The cull→patch barrier above (line 848) and
            // the post-patch barrier below cover bucketCountData[] /
            // cmds[].instanceCount cross-dispatch ordering; do not add a
            // GL_BUFFER_UPDATE_BARRIER_BIT for permutation.
            // GlStateGuard slice 1: scoped restore of slot 15 (was hand-rolled
            // prevSsbo15 glGetIntegeri_v + rebind at block end). Destructs at the
            // enclosing block's '}' — same restore point, same value, no behavior
            // change.
            mc2gl::GlScopedSsboBinding guardSsbo15(15);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, batcher_getPermutationSsbo());

            // 2026-05-11 per-packet rework: bind cmd_to_bucket SSBO at
            // binding 7 and run patch in per-packet mode when batcher's
            // packet-layout is built. Falls back to legacy per-type when
            // unavailable (coalesce-disarmed flow).
            // GlStateGuard slice 1: scoped restore of slot 7. Captures the
            // current binding unconditionally; the original only restored slot 7
            // when perPacketPatch rebound it, but restoring an unchanged slot to
            // its captured value is a no-op rebind — GL-identical.
            mc2gl::GlScopedSsboBinding guardSsbo7(7);
            const GLuint cmdToBucketSsbo = batcher_getCmdToBucketSsbo();
            const uint32_t pktCmdCount   = batcher_getSortedPacketCount();
            const bool perPacketPatch    = (cmdToBucketSsbo != 0u && pktCmdCount > 0u);
            if (perPacketPatch) {
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, cmdToBucketSsbo);
            }

            // Step 4.2 — bind base-instance SSBO at slot 16 BEFORE glUseProgram
            // (buffer bind is program-independent; uniform must be after).
            batcher_bindBaseInstanceByCmdSsboForPatch();

            glUseProgram(s_patchProgram);
            const GLint locNBuckets        = glGetUniformLocation(s_patchProgram, "u_nBuckets");
            const GLint locNCmds           = glGetUniformLocation(s_patchProgram, "u_nCmds");
            const GLint locUseCmdToBucket  = glGetUniformLocation(s_patchProgram, "u_useCmdToBucket");
            if (locNBuckets >= 0)
                glUniform1i(locNBuckets, (int)s_bucketCount);
            if (locNCmds >= 0)
                glUniform1i(locNCmds, (int)(perPacketPatch ? pktCmdCount : s_bucketCount));
            if (locUseCmdToBucket >= 0)
                glUniform1i(locUseCmdToBucket, perPacketPatch ? 1 : 0);
            // Step 4.2 — u_legacyBaseInstance MUST be after glUseProgram (operates on bound program).
            {
                const GLint locLegacy = glGetUniformLocation(s_patchProgram, "u_legacyBaseInstance");
                const bool  isLegacy  = batcher_isGlobalPoolLegacy();
                if (locLegacy >= 0) {
                    glUniform1i(locLegacy, isLegacy ? 1 : 0);
                }
            }
            const uint32_t patchInvocations =
                perPacketPatch ? pktCmdCount : s_bucketCount;
            const uint32_t patchGroups = (patchInvocations + 63u) / 64u;
            { TracyGpuZone("Cull.PatchShader"); glDispatchCompute(patchGroups, 1, 1); }
            glUseProgram(0);

            batcher_unbindBaseInstanceByCmdSsboForPatch();  // restore slot 16

            // Slots 7 and 15 are restored here by guardSsbo7 / guardSsbo15
            // destructing at this closing brace (reverse construction order:
            // 7 then 15 — same order as the prior hand-rolled restores).
        }

        // 5. Barrier: indirect command buffer ready for draw + SSBO writes visible.
        // GL_COMMAND_BARRIER_BIT: ensures cmds[].instanceCount writes are visible to
        // glMultiDrawElementsIndirect (Q12: first GL_COMMAND_BARRIER_BIT in engine).
        // DO NOT add GL_ATOMIC_COUNTER_BARRIER_BIT (Q12 contract).
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

        // C2 immediate post-dispatch diagnostic (C1b path) — env-gated.
        // Reads from GPU SSBO directly (after SHADER_STORAGE barrier above).
        if (s_computeTrace && readback_isEnabled()) {
            const GLuint rbGpu = readback_getSsboBuf();
            const GLintptr rbOff = readback_getCurrentSlotOffset();
            if (rbGpu) {
                uint32_t rbVisGpu = 0xAAAAu;
                uint32_t rbMarker = 0xAAAAu;
                glGetNamedBufferSubData(rbGpu, rbOff,                      sizeof(uint32_t), &rbVisGpu);
                glGetNamedBufferSubData(rbGpu, rbOff +   sizeof(uint32_t), sizeof(uint32_t), &rbMarker);
                GLint bound14 = 0;
                glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 14, &bound14);

                // Also read bucket0 count to see if C1b cull found any visible actors.
                uint32_t bucket0Count = 0xAAAAu;
                if (s_bucketCountsBuf)
                    glGetNamedBufferSubData(s_bucketCountsBuf, 0, sizeof(uint32_t), &bucket0Count);

                MC2_DIAGF("[GPU_CULL v1] C2_DIAG(c1b) gpuBuf=%u rbOff=%lld bound14=%d rb_vis=%u rb_marker=0x%X bucket0=%u recordCount=%u\n",
                       rbGpu, (long long)rbOff, bound14, rbVisGpu, rbMarker,
                       bucket0Count, recordCount);
                fflush(stdout);
            }
        }

        // 6. Block-active rollup (C1-RB).
        if (s_rollupProgram) {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, substrate_getInstanceSsboBindingPoint(), s_stagingSsbo);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ACTOR_VIS_BINDING,  s_actorVisBuf);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BLOCK_VIS_BINDING,  s_blockVisBuf);

            glUseProgram(s_rollupProgram);
            const GLint locRC = glGetUniformLocation(s_rollupProgram, "u_recordCount");
            if (locRC >= 0)
                glUniform1i(locRC, (int)recordCount);
            // Sticky-bit rollup (v3 §2.5): rollup is uniform-free for the
            // temporal-superset path — it issues atomicOr(...,1u) on each
            // visible actor's block bit. The K-window uFrameStamp upload
            // retired with the counter.
            glDispatchCompute(cullGroups, 1, 1);
            glUseProgram(0);

            // Barrier: blockVisBits[] ready for C3 consumers.
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        }

        // C1B-SSBO-CLEANUP: unbind all SSBO slots set during the C1b dispatch block.
        // Slots 9..14 are bound to GPU-cull-internal buffers; leaving them active leaks
        // into subsequent rendering passes (water GPU compute, terrain, post-process)
        // that use the same binding indices for their own data. This was the root cause
        // of water disappearing on camera-move frames after d65552ab made GPU cull active.
        // Unbind unconditionally — cost is 6 GL calls, negligible vs dispatch overhead.
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, DEBUG_SSBO_BINDING,      0u); // slot 9
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BUCKET_COUNTS_BINDING,   0u); // slot 10
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, INDIRECT_CMD_BINDING,    0u); // slot 11
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ACTOR_VIS_BINDING,       0u); // slot 12
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BLOCK_VIS_BINDING,       0u); // slot 13
        // Slot 14 (READBACK_SSBO_BINDING) was bound via glBindBufferRange — unbind via base.
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, READBACK_SSBO_BINDING,   0u); // slot 14
        // Slot 11 also used as GL_DRAW_INDIRECT_BUFFER: ensure that is unbound too.
        // (The C1b draw in flush() rebinds it explicitly; this prevents stale binding
        //  from bleeding into any pass between compute_dispatch() and flush().)
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0u);

        // 7. Overflow check (async — read overflow counter without stalling).
        // Non-blocking: will be checked next frame. Flag checked in emitParitySummary.
        // For immediate detection, log synchronously every 600 frames.
        ++s_dispatchFrames;

        glEndQuery(GL_TIME_ELAPSED);
        s_timerPending = true;

        // Overflow logging (async readback, deferred to emitParitySummary).
        COMPUTE_TRACE("event=dispatch_ok mode=c1b dispatched=%u buckets=%u", recordCount, s_bucketCount);

    } else {
        // ========================================================
        // C1a path: shadow/diagnostic mode
        // ========================================================
        const uint32_t effectiveCount = (recordCount <= s_maxActors) ? recordCount : s_maxActors;
        const GLsizeiptr clearBytes =
            static_cast<GLsizeiptr>(2 * sizeof(uint32_t) + effectiveCount * sizeof(uint32_t));
        const GLuint clearZero = 0u;
        glClearNamedBufferSubData(s_debugSsbo, GL_R32UI, 0, clearBytes, GL_RED_INTEGER, GL_UNSIGNED_INT, &clearZero);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, DEBUG_SSBO_BINDING, s_debugSsbo);

        glUseProgram(s_computeProgram);
        glDispatchCompute(cullGroups, 1, 1);
        glUseProgram(0);

        // Barrier: SSBO writes visible before CPU readback.
        // NOT GL_ATOMIC_COUNTER_BARRIER_BIT — no ACBOs.
        // NOT GL_COMMAND_BARRIER_BIT — no indirect draws in C1a.
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

        // C2 immediate post-dispatch diagnostic (C1a path): read rb_visibleCount
        // from the GPU SSBO directly (after SHADER_STORAGE_BARRIER_BIT).
        if (s_computeTrace && readback_isEnabled()) {
            const GLuint rbGpu = readback_getSsboBuf();
            const GLintptr rbOff = readback_getCurrentSlotOffset();
            if (rbGpu) {
                uint32_t rbVisGpu = 0xDEADU;
                glGetNamedBufferSubData(rbGpu, rbOff, sizeof(uint32_t), &rbVisGpu);
                GLint bound14 = 0;
                glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 14, &bound14);
                MC2_DIAGF("[GPU_CULL v1] C2_DIAG(c1a) gpuBuf=%u rbOff=%lld bound14=%d rbVisGpu=%u recordCount=%u\n",
                       rbGpu, (long long)rbOff, bound14, rbVisGpu, recordCount);
                fflush(stdout);
            }
        }

        ++s_dispatchFrames;

        glEndQuery(GL_TIME_ELAPSED);
        s_timerPending = true;

        COMPUTE_TRACE("event=dispatch recordCount=%u groups=%u", recordCount, cullGroups);
    }

    // C2 frame end: place GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT + glFenceSync for
    // this frame's readback slot. Called AFTER all GPU commands are issued.
    if (readback_isEnabled()) {
        readback_frameEnd(s_readbackFrameCounter);
    }
    ++s_readbackFrameCounter;
}

// ---------------------------------------------------------------------------
// compute_emitParitySummary (C1a mode; no-op in C1b)
// ---------------------------------------------------------------------------
void compute_emitParitySummary() {
    if (!compute_isEnabled() || !s_initialized) return;
    if (s_dispatchFrames == 0) return;

    // Collect timer result.
    uint64_t dispatchNs = s_lastDispatchNs;
    if (s_timerPending) {
        GLint available = 0;
        glGetQueryObjectiv(s_timerQuery, GL_QUERY_RESULT_AVAILABLE, &available);
        if (available) {
            glGetQueryObjectui64v(s_timerQuery, GL_QUERY_RESULT, &dispatchNs);
            s_lastDispatchNs = dispatchNs;
            s_timerPending = false;
        }
    }
    const uint32_t dispatchUs = (uint32_t)(dispatchNs / 1000u);

    if (s_c1bInitialized) {
        // C1b mode: emit indirect draw summary + check overflow.
        ++s_summaryCount;
        const bool doEmit = !s_firstSummaryDone || ((s_summaryCount % 600) == 0);
        if (doEmit) {
            s_firstSummaryDone = true;

            // Async read overflow counter (non-blocking readback from bucketCountsBuf).
            uint32_t overflow = 0;
            if (s_bucketCountsBuf) {
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_bucketCountsBuf);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                                   static_cast<GLintptr>(s_bucketCount * sizeof(uint32_t)),
                                   sizeof(uint32_t), &overflow);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            }

            if (overflow > 0) {
                MC2_DIAGF("[GPU_CULL v1] event=overflow bucket=all count=%u\n", overflow);
                fflush(stdout);
            }

            MC2_DIAGF("[GPU_CULL v1] event=indirect_draw buckets=%u overflow=%u elapsed_us=%u flush=%u\n",
                   s_bucketCount, overflow, dispatchUs, s_dispatchFrames);
            fflush(stdout);
            s_dispatchFrames = 0;
        }
        return;
    }

    // C1a mode: full parity readback.
    uint32_t gpuVis = 0, gpuCull = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_debugSsbo);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(uint32_t), &gpuVis);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, sizeof(uint32_t), sizeof(uint32_t), &gpuCull);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    const uint32_t cpuVis = substrate_getCpuVisibleCount();
    const uint32_t totalActors = gpuVis + gpuCull;
    uint32_t falseNeg = 0, falsePos = 0;

    if (totalActors > 0 && totalActors <= s_maxActors) {
        const uint32_t visResultOffset = 2 * sizeof(uint32_t);
        std::vector<uint32_t> gpuBits(totalActors);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_debugSsbo);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                           static_cast<GLintptr>(visResultOffset),
                           static_cast<GLsizeiptr>(totalActors * sizeof(uint32_t)),
                           gpuBits.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        if (s_stagingSsbo) {
            std::vector<uint32_t> cpuBits(totalActors);
            static const size_t kRecordStride     = 64u;
            static const size_t kPrevVisBitOffset = 52u;
            static const size_t kHeaderBytes      = sizeof(GpuActorRecordHeader);

            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_stagingSsbo);
            for (uint32_t i = 0; i < totalActors; ++i) {
                GLintptr off = static_cast<GLintptr>(
                    kHeaderBytes + i * kRecordStride + kPrevVisBitOffset);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                                   off, sizeof(uint32_t), &cpuBits[i]);
            }
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

            for (uint32_t i = 0; i < totalActors; ++i) {
                const bool cpuVisible = (cpuBits[i] != 0u);
                const bool gpuVisible = (gpuBits[i] != 0u);
                if (cpuVisible && !gpuVisible) ++falseNeg;
                if (!cpuVisible && gpuVisible) ++falsePos;
            }
        }
    }

    s_accFalseNeg += falseNeg;
    s_accFalsePos += falsePos;
    ++s_summaryCount;

    const bool doEmit = !s_firstSummaryDone || ((s_summaryCount % 600) == 0);
    if (doEmit) {
        s_firstSummaryDone = true;
        MC2_DIAGF("[GPU_CULL v1] event=parity_summary"
               " gpu_visible=%u cpu_visible=%u"
               " total_actors=%u"
               " false_neg=%u false_pos=%u"
               " dispatch_us=%u flush=%u\n",
               gpuVis, cpuVis,
               totalActors,
               falseNeg, falsePos,
               dispatchUs, s_dispatchFrames);
        fflush(stdout);
        s_accFalseNeg    = 0;
        s_accFalsePos    = 0;
        s_dispatchFrames = 0;
    }
}

} // namespace gpu_cull
