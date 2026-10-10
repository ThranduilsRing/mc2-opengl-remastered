// gos_vegetation.cpp — GPU instanced vegetation card batcher.
//
// Two crossed screen-aligned quads per instance, rendered with
// glDrawElementsInstanced. Enabled by MC2_VEGETATION_CARDS env var.
//
// Vertex layout (static VBO, stride 20):
//   location 0: vec3(horizontal, vertical, yawOffset)  offset  0
//   location 1: vec2(uvX, uvY)                         offset 12
//
// Instance layout (instance VBO, stride 28 = sizeof(Instance)):
//   location 2: vec3(x,y,z)        offset  0, divisor 1
//   location 3: float yaw          offset 12, divisor 1
//   location 4: float scale        offset 16, divisor 1
//   location 5: uint atlasFrame    offset 20, divisor 1
//   location 6: float seed         offset 24, divisor 1

#include "gos_vegetation.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "gos_static_prop_killswitch.h"  // gos_GetTerrainMVPMat4()
#include "../../mclib/render_contract.h"
#include "utils/shader_builder.h"
#include "utils/gl_utils.h"
#include <GL/glew.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

// ---------------------------------------------------------------------------
// File-local state
// ---------------------------------------------------------------------------
namespace {

static GLuint       s_vao           = 0;
static GLuint       s_staticVbo     = 0;
static GLuint       s_ibo           = 0;
static GLuint       s_instanceVbo   = 0;
static GLuint       s_atlasTexId    = 0;
static GLuint       s_blockVisSsbo  = 0;
static glsl_program* s_prog         = nullptr;
static uint32_t     s_instanceCount = 0;
static bool         s_initialized   = false;

// VEG-FLUSH-REASON-1: per-frame stats updated by flush().
// Accumulated across the session for VEG_SUMMARY (emitSummary()).
static const char*  s_flushReason        = "no_call";  // last flush() exit reason
static int          s_frameDrawCalls     = 0;          // draw calls in most recent flush()
static int          s_sessionDrawCalls   = 0;          // total draw calls since init()
static int          s_sessionFrames      = 0;          // flush() calls since init()
static uint32_t     s_peakInstanceCount  = 0;          // highest uploadInstances count seen
static bool         s_summaryEmitted     = false;      // guard: emitSummary() fires exactly once

// Vertex for static crossed-quad geometry.
struct CardVert {
    float horizontal;
    float vertical;
    float yawOffset;
    float uvX;
    float uvY;
};
static_assert(sizeof(CardVert) == 20, "CardVert stride must be 20 bytes");

// 16 vertices: two crossed quads, each subdivided into 3 height segments.
// Heights y ∈ {0, 1/3, 2/3, 1}. Shader applies quadratic sway (y*y) so each
// row curves by a different amount — base stays fixed, tip bends the most.
static const CardVert k_verts[16] = {
    // Quad 1 (yawOffset = 0)
    {-0.5f, 0.000000f, 0.0f,       0.0f, 0.000000f},  //  0: row0 L
    { 0.5f, 0.000000f, 0.0f,       1.0f, 0.000000f},  //  1: row0 R
    {-0.5f, 0.333333f, 0.0f,       0.0f, 0.333333f},  //  2: row1 L
    { 0.5f, 0.333333f, 0.0f,       1.0f, 0.333333f},  //  3: row1 R
    {-0.5f, 0.666667f, 0.0f,       0.0f, 0.666667f},  //  4: row2 L
    { 0.5f, 0.666667f, 0.0f,       1.0f, 0.666667f},  //  5: row2 R
    {-0.5f, 1.000000f, 0.0f,       0.0f, 1.000000f},  //  6: row3 L
    { 0.5f, 1.000000f, 0.0f,       1.0f, 1.000000f},  //  7: row3 R
    // Quad 2 (yawOffset = PI/2)
    {-0.5f, 0.000000f, 1.5707963f, 0.0f, 0.000000f},  //  8: row0 L
    { 0.5f, 0.000000f, 1.5707963f, 1.0f, 0.000000f},  //  9: row0 R
    {-0.5f, 0.333333f, 1.5707963f, 0.0f, 0.333333f},  // 10: row1 L
    { 0.5f, 0.333333f, 1.5707963f, 1.0f, 0.333333f},  // 11: row1 R
    {-0.5f, 0.666667f, 1.5707963f, 0.0f, 0.666667f},  // 12: row2 L
    { 0.5f, 0.666667f, 1.5707963f, 1.0f, 0.666667f},  // 13: row2 R
    {-0.5f, 1.000000f, 1.5707963f, 0.0f, 1.000000f},  // 14: row3 L
    { 0.5f, 1.000000f, 1.5707963f, 1.0f, 1.000000f},  // 15: row3 R
};

// 36 indices: 2 quads × 3 strips × 2 tris × 3 indices.
static const uint16_t k_indices[36] = {
    // Quad 1
     0,  1,  2,   1,  3,  2,   // row 0-1
     2,  3,  4,   3,  5,  4,   // row 1-2
     4,  5,  6,   5,  7,  6,   // row 2-3
    // Quad 2
     8,  9, 10,   9, 11, 10,   // row 0-1
    10, 11, 12,  11, 13, 12,   // row 1-2
    12, 13, 14,  13, 15, 14,   // row 2-3
};

// Default atlas path; can be overridden via setAtlasPath() before init().
static const char* s_atlasPathOverride = nullptr;

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool GosVegetation::isEnabled() {
    static int cached = -1;
    if (cached < 0) {
        cached = mc2::gate::get<"MC2_VEGETATION_CARDS">();
        MC2_DIAGFE(stderr, "[VEG v1] feature enabled=%d reason=%s\n",
            cached,
            !mc2::gate::isSet<"MC2_VEGETATION_CARDS">() ? "env_missing" :
            !cached ? "env_zero"  : "env_set");
        fflush(stderr);
    }
    return cached == 1;
}

uint32_t GosVegetation::instanceCount() {
    return s_instanceCount;
}

void GosVegetation::setAtlasPath(const char* path) {
    // Must be called before init(); the pointer must remain valid through init().
    s_atlasPathOverride = path;
}

void GosVegetation::init() {
    if (s_initialized) {
        return;
    }

    // --- Shader ---
    s_prog = glsl_program::makeProgram(
        "vegetation_card",
        "shaders/gos_vegetation_card.vert",
        "shaders/gos_vegetation_card.frag",
        "#version 430\n");
    if (!s_prog || !s_prog->is_valid()) {
        MC2_DIAGFE(stderr,
            "[VEG v1] event=init_warn msg=shader_load_failed "
            "vert=shaders/gos_vegetation_card.vert "
            "frag=shaders/gos_vegetation_card.frag\n");
        s_prog = nullptr;
    }

    // --- Static VBO (crossed quads) ---
    glGenBuffers(1, &s_staticVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_staticVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(sizeof(k_verts)),
                 k_verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // --- IBO ---
    glGenBuffers(1, &s_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(sizeof(k_indices)),
                 k_indices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    // --- Instance VBO (starts empty; populated by uploadInstances) ---
    glGenBuffers(1, &s_instanceVbo);

    // --- VAO ---
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);

    // Bind static vertex buffer and configure per-vertex attribs.
    glBindBuffer(GL_ARRAY_BUFFER, s_staticVbo);

    // attr 0: vec3(horizontal, vertical, yawOffset), offset 0, stride 20
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20,
                          reinterpret_cast<const void*>(0));

    // attr 1: vec2(uvX, uvY), offset 12, stride 20
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20,
                          reinterpret_cast<const void*>(12));

    // Bind instance VBO and configure per-instance attribs.
    // stride = sizeof(Instance) = 28
    constexpr GLsizei kInstStride = static_cast<GLsizei>(sizeof(GosVegetation::Instance));
    glBindBuffer(GL_ARRAY_BUFFER, s_instanceVbo);

    // attr 2: vec3(x,y,z), offset 0, divisor 1
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, kInstStride,
                          reinterpret_cast<const void*>(0));
    glVertexAttribDivisor(2, 1);

    // attr 3: float yaw, offset 12, divisor 1
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, kInstStride,
                          reinterpret_cast<const void*>(12));
    glVertexAttribDivisor(3, 1);

    // attr 4: float scale, offset 16, divisor 1
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, kInstStride,
                          reinterpret_cast<const void*>(16));
    glVertexAttribDivisor(4, 1);

    // attr 5: uint atlasFrame, offset 20, divisor 1
    glEnableVertexAttribArray(5);
    glVertexAttribIPointer(5, 1, GL_UNSIGNED_INT, kInstStride,
                           reinterpret_cast<const void*>(20));
    glVertexAttribDivisor(5, 1);

    // attr 6: float seed, offset 24, divisor 1
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, kInstStride,
                          reinterpret_cast<const void*>(24));
    glVertexAttribDivisor(6, 1);

    // Bind IBO into VAO state.
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    // --- Block visibility SSBO (binding 12) ---
    // Pre-allocated to 1024 slots at 2u (LOD0 = draw) so OOB reads
    // on the first frame (before setBlockVisibility fires) show all cards.
    // TIER2-EXCLUDED: substrate-gated
    glGenBuffers(1, &s_blockVisSsbo);
    {
        uint32_t initVis[1024];
        for (int i = 0; i < 1024; ++i) initVis[i] = 2u;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_blockVisSsbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     static_cast<GLsizeiptr>(1024 * sizeof(uint32_t)),
                     initVis, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // --- Atlas texture ---
    const char* atlasPath = s_atlasPathOverride
                          ? s_atlasPathOverride
                          : "data/textures/vegetation_atlas_v2.png";

    // Allow env-var override at runtime too.
    const std::string envPath = mc2::gate::str<"MC2_VEGETATION_ATLAS">();   // "" when unset or empty
    if (!envPath.empty()) {
        atlasPath = envPath.c_str();
    }

    Texture atlasTex = load_texture_from_file(atlasPath);
    if (atlasTex.isValid()) {
        s_atlasTexId = atlasTex.id;
    } else {
        MC2_DIAGFE(stderr,
            "[VEG v1] event=atlas_load_failed path=%s "
            "(flush will no-op until uploadInstances + valid atlas)\n",
            atlasPath);
        s_atlasTexId = 0;
    }

    // VEG-SMOKE-FLOOR-1: register atexit so emitSummary() fires at process exit
    // even when GosVegetation::shutdown() is not wired into the shutdown sequence.
    // emitSummary() is guarded against double-emit (s_summaryEmitted).
    std::atexit([]() { GosVegetation::emitSummary(); });

    s_initialized = true;
}

void GosVegetation::shutdown() {
    // VEG-SMOKE-FLOOR-1: emit session summary before state is cleared.
    // Must fire before zeroing s_peakInstanceCount / s_sessionDrawCalls.
    emitSummary();

    if (s_vao)          { glDeleteVertexArrays(1, &s_vao);        s_vao = 0; }
    if (s_staticVbo)    { glDeleteBuffers(1, &s_staticVbo);       s_staticVbo = 0; }
    if (s_ibo)          { glDeleteBuffers(1, &s_ibo);              s_ibo = 0; }
    if (s_instanceVbo)  { glDeleteBuffers(1, &s_instanceVbo);     s_instanceVbo = 0; }
    if (s_blockVisSsbo) { glDeleteBuffers(1, &s_blockVisSsbo);    s_blockVisSsbo = 0; }
    if (s_atlasTexId)   { glDeleteTextures(1, &s_atlasTexId);     s_atlasTexId = 0; }
    if (s_prog)         { glsl_program::deleteProgram("vegetation_card"); s_prog = nullptr; }
    s_instanceCount      = 0;
    s_peakInstanceCount  = 0;
    s_sessionDrawCalls   = 0;
    s_sessionFrames      = 0;
    s_frameDrawCalls     = 0;
    s_flushReason        = "no_call";
    s_initialized        = false;
}

void GosVegetation::uploadInstances(const Instance* instances, uint32_t count) {
    if (!s_initialized) {
        return;
    }
    s_instanceCount = count;
    // VEG-FLUSH-REASON-1: track peak instance count across uploads for emitSummary().
    if (count > s_peakInstanceCount) s_peakInstanceCount = count;
    if (count == 0 || instances == nullptr) {
        return;
    }

    const GLsizeiptr byteSize =
        static_cast<GLsizeiptr>(count) *
        static_cast<GLsizeiptr>(sizeof(Instance));

    glBindBuffer(GL_ARRAY_BUFFER, s_instanceVbo);
    glBufferData(GL_ARRAY_BUFFER, byteSize, instances, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GosVegetation::setBlockVisibility(const uint32_t* visible, uint32_t blockCount) {
    if (!s_blockVisSsbo || !visible || blockCount == 0) return;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_blockVisSsbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(blockCount * sizeof(uint32_t)),
                 visible, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void GosVegetation::flush(float lightDirX, float lightDirY, float lightDirZ, float lightDirW,
                          float time,
                          float camChunkX, float camChunkY, float camChunkZ,
                          float mapHalfWU, float blockSideWU, int chunkSide) {
    // VEG-FLUSH-REASON-1: reset per-frame draw-call counter and set default reason.
    s_frameDrawCalls = 0;
    s_sessionFrames++;

    if (!isEnabled()) {
        s_flushReason = "disabled";
        return;
    }
    if (s_instanceCount == 0) {
        s_flushReason = "no_instances";
        return;
    }
    if (s_atlasTexId == 0) {
        s_flushReason = "no_atlas";
        return;
    }
    if (!s_prog || !s_prog->is_valid()) {
        s_flushReason = "no_program";
        return;
    }

    // SAME-ORDER-EXECUTOR-VALIDATE-1: top-level validate-only wrapper (gate MC2_FRAMEGRAPH_EXECUTOR).
    // No-op when gate unset (byte-identical). VegetationCards has no AmbientContract row and no
    // declared FBO target -> ambient/FBO probe skips it (validateAmbient=false, validateFbo=false).
    // MC2_VEGETATION_CARDS default-OFF -> this is a legit 0-fires-per-frame-in-smoke pass.
    // PIN INVARIANT: additive only — no GL state change, no reorder.
    render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::VegetationCards,
                                              "GosVegetation_flush");
    struct TopLevelGuard_ {
        ~TopLevelGuard_() {
            render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::VegetationCards,
                                                    "GosVegetation_flush");
        }
    } _tlGuard;

    const float* mvp = gos_GetTerrainMVPMat4();

    static int s_flushCount = 0;
    const bool firstFlush = (s_flushCount++ == 0);
    if (firstFlush) {
        MC2_DIAGFE(stderr,
            "[VEG v1] flush: instances=%u atlas=%u prog=%s mvp=%s "
            "chunkSide=%d blockSideWU=%.0f mapHalfWU=%.0f\n",
            s_instanceCount, s_atlasTexId,
            (s_prog && s_prog->is_valid()) ? "OK" : "BAD",
            mvp ? "OK" : "NULL",
            chunkSide, static_cast<double>(blockSideWU),
            static_cast<double>(mapHalfWU));
        fflush(stderr);
    }

    if (!mvp) {
        s_flushReason = "missing_buffers";  // terrain MVP not ready yet
        return;
    }

    // --- Explicit GL state for vegetation card pass ---
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);    // depth write OFF (cards alpha-test but don't occlude each other)
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);  // both faces visible
    // VEGETATION-DEPTH-GREATER: terrain has TERRAIN_DEPTH_FUDGE (pushed back,
    // depth D-0.004). Static props have no fudge (depth D, true depth).
    // Vegetation VS at depth D−0.002 (1× TERRAIN_DEPTH_FUDGE bias, half of terrain's 2×).
    // GL_GREATER: (D−0.002) > (D−0.004)=TRUE → veg over coplanar terrain ✓
    //             (D−0.002) > D        =FALSE → veg behind static props   ✓
    // Ghost zone (cliff just-in-front within 0.002 NDC): acceptable for typical geometry.
    glDepthFunc(GL_GREATER);

    // [RENDER_CONTRACT:Pass=VegetationCards]
    render_contract::assertPassContract(
        render_contract::PassIdentity::VegetationCards,
        "GosVegetation::flush");
    // DRYRUN-OBSERVE-COVERAGE-1: observe-only. VegetationCards has no AmbientContract
    // row and no declared FBO target -> ambient/FBO probe skips it. Outer adapter
    // early-returns when MC2_VEGETATION_CARDS (default-OFF) is unset, so this is a
    // legitimate UNOBSERVED-this-frame in default smoke. No GL state / draw change.
    render_contract::noteRenderPass(
        render_contract::PassIdentity::VegetationCards,
        "GosVegetation::flush");

    s_prog->apply();
    const GLuint progId = s_prog->shp_;

    {
        const GLint loc = glGetUniformLocation(progId, "u_worldToClipGL");
        glProgramUniformMatrix4fv(progId, loc, 1, GL_FALSE, mvp);
    }
    {
        const GLint loc = glGetUniformLocation(progId, "u_cameraPos");
        glProgramUniform3f(progId, loc, camChunkX, camChunkY, camChunkZ);
    }
    {
        const GLint loc = glGetUniformLocation(progId, "u_terrainLightDir");
        glProgramUniform4f(progId, loc, lightDirX, lightDirY, lightDirZ, lightDirW);
    }
    {
        const GLint loc = glGetUniformLocation(progId, "u_time");
        glProgramUniform1f(progId, loc, time);
    }
    {
        const GLint loc = glGetUniformLocation(progId, "u_mapHalfWU");
        glProgramUniform1f(progId, loc, mapHalfWU);
    }
    {
        const GLint loc = glGetUniformLocation(progId, "u_blockSideWU");
        glProgramUniform1f(progId, loc, blockSideWU);
    }
    {
        const GLint loc = glGetUniformLocation(progId, "u_chunkSide");
        glProgramUniform1i(progId, loc, chunkSide);
    }

    // MC2_VEG_DEBUG_FORCE_VISIBLE=1 — override blockVis/LOD cull AND distance fade in shader.
    {
        static int s_forceVis = -1;
        if (s_forceVis < 0) {
            s_forceVis = mc2::gate::get<"MC2_VEG_DEBUG_FORCE_VISIBLE">();
        }
        const GLint loc = glGetUniformLocation(progId, "u_forceVisible");
        if (loc >= 0) glProgramUniform1i(progId, loc, s_forceVis);
    }

    // Distance-based fade: MC2_VEG_MAX_DIST sets the hard cull distance (wu), default 8192.
    // Fade begins at 70% of max. Force-visible bypasses distFade in the shader via u_forceVisible.
    {
        static float s_vegMaxDist = -1.0f;
        if (s_vegMaxDist < 0.0f) {
            s_vegMaxDist = mc2::gate::get<"MC2_VEG_MAX_DIST">();   // row default 4096
        }
        const float vegFadeStart = s_vegMaxDist * 0.50f;  // clump cull starts at half max (4096 for default 8192)
        {
            const GLint loc = glGetUniformLocation(progId, "u_vegMaxDist");
            if (loc >= 0) glProgramUniform1f(progId, loc, s_vegMaxDist);
        }
        {
            const GLint loc = glGetUniformLocation(progId, "u_vegFadeStart");
            if (loc >= 0) glProgramUniform1f(progId, loc, vegFadeStart);
        }
    }

    // Atlas texture on unit 0.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_atlasTexId);
    {
        const GLint loc = glGetUniformLocation(progId, "u_atlas");
        glProgramUniform1i(progId, loc, 0);
    }

    // Block visibility SSBO at binding 12.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 12, s_blockVisSsbo);

    // Draw.
    glBindVertexArray(s_vao);
    glDrawElementsInstanced(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT,
                            nullptr,
                            static_cast<GLsizei>(s_instanceCount));
    // VEG-FLUSH-REASON-1: record successful draw submission.
    s_flushReason = "submitted";
    s_frameDrawCalls = 1;
    s_sessionDrawCalls++;

    // --- Restore critical state ---
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 12, 0);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
}

// VEG-FLUSH-REASON-1: return stats from the most recent flush() call.
GosVegetation::VegFrameStats GosVegetation::getFrameStats() {
    VegFrameStats stats;
    stats.instance_count = static_cast<int>(s_instanceCount);
    stats.draw_calls     = s_frameDrawCalls;
    stats.flush_reason   = s_flushReason;
    return stats;
}

// VEG-SMOKE-FLOOR-1: emit a VEG_SUMMARY line to stderr for smoke log parsing.
// Called at shutdown (before state is cleared).
// Format (stable — smoke assertion parses this):
//   VEG_SUMMARY instance_count=N draw_calls=N flush_reason=<reason>
// instance_count = peak count uploaded this session (persists after missionUnloaded clears live count).
void GosVegetation::emitSummary() {
    if (!isEnabled()) return;
    if (s_summaryEmitted) return;     // guard: emit exactly once per process
    s_summaryEmitted = true;
    fprintf(stderr,
        "VEG_SUMMARY instance_count=%u draw_calls=%d flush_reason=%s\n",
        s_peakInstanceCount,
        s_sessionDrawCalls,
        s_flushReason);
    fflush(stderr);
}
