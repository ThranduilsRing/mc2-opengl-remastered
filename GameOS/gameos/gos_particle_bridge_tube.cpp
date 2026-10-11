// GPU particle bridge: the MC2_VFX_ORACLE_TUBE ribbon bridge: ribbon program / buffers, gos_tube_ribbon_flush, enqueue and the deferred flush.
// Declared in the class headers; shared names in gos_particle_bridge_internal.h.
#include "gos_particle_bridge_internal.h"
#include "mc2fmt.h"


// ── MC2_VFX_ORACLE_TUBE slice 1: ribbon mesh bridge state ─────────────────
// Separate program + 3 SSBOs (pos/color/uv) + element buffer. Shares NOTHING
// with the billboard path above; the ribbon is an indexed swept-quad mesh, not
// gl_VertexID-expanded cards.
namespace {

// MC2_VFX_ORACLE_TUBE_COVERAGE=1: wrap the deferred ribbon draw in a
// GL_SAMPLES_PASSED occlusion query and log per-frame coverage + the bound
// draw-FBO. This is the DETERMINISTIC, nondeterminism-immune proof that the
// oracle tube fragments actually rasterize + pass depth into the composited
// scene FBO (cross-launch pixel-diff cannot validate combat FX -- mech-pose
// noise ~2.3% swamps it; an in-frame sample count does not depend on pose).
// samples>0 with fbo=scene target => tube pixels reach the frame. Diagnostic,
// default-OFF; the GL_QUERY_RESULT read stalls, acceptable when gated on.
bool s_tubeCoverage_initialized = false;
bool s_tubeCoverage_value       = false;
bool tubeCoverageEnabled() {
    if (!s_tubeCoverage_initialized) {
        s_tubeCoverage_value = mc2::gate::get<"MC2_VFX_ORACLE_TUBE_COVERAGE">();
        s_tubeCoverage_initialized = true;
    }
    return s_tubeCoverage_value;
}
GLuint s_tubeCoverageQuery = 0;  // lazily created occlusion query object

GLuint s_tubePosSsbo   = 0;   // binding=14: vec4 worldpos
GLuint s_tubeColSsbo   = 0;   // binding=15: vec4 rgba
GLuint s_tubeUvSsbo    = 0;   // binding=16: vec2 uv
GLuint s_tubeIbo       = 0;   // GL_ELEMENT_ARRAY_BUFFER (unsigned short)
GLuint s_tubeVao       = 0;   // VAO owning the IBO binding
GLuint s_tubeSampler   = 0;   // CLAMP_TO_EDGE + LINEAR
const ::glsl_program* s_tubeProg = nullptr;
bool s_tubeInitFailed  = false;

GLint s_tloc_worldToClipGL = -2;
GLint s_tloc_uAtlas        = -2;
GLint s_tloc_uAdditive     = -2;  // blend-aware discard: 1=additive (PPC), 0=alpha ribbon

GLsizei s_tubePosCap = 0, s_tubeColCap = 0, s_tubeUvCap = 0, s_tubeIdxCap = 0;

void tubeEnsureInitialized() {
    if (s_tubeInitFailed) return;
    if (s_tubeProg && s_tubeVao && s_tubeSampler) return;

    if (s_tubeVao == 0) glGenVertexArrays(1, &s_tubeVao);
    if (s_tubeSampler == 0) {
        // TEX-CLASS: per-pass-rebind -- particle-tube sampler object
        glGenSamplers(1, &s_tubeSampler);
        glSamplerParameteri(s_tubeSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_tubeSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_tubeSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s_tubeSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    if (s_tubeProg == nullptr) {
        static const char* kPrefix = "#version 430\n";
        s_tubeProg = ::glsl_program::makeProgram(
            "tube_ribbon",
            "shaders/tube_ribbon.vert",
            "shaders/tube_ribbon.frag",
            kPrefix);
        if (!s_tubeProg || !s_tubeProg->shp_) {
            s_tubeInitFailed = true;
            MC2_DIAGFE(stderr,
                "[VFX_ORACLE_TUBE v1] event=prog_compile_fail — ribbon bridge disabled\n");
            std::fflush(stderr);
            s_tubeProg = nullptr;
            return;
        }
        MC2_DIAGFE(stderr,
            "[VFX_ORACLE_TUBE v1] event=prog_compiled prog=%u\n",
            (unsigned)s_tubeProg->shp_);
        std::fflush(stderr);
        s_tloc_worldToClipGL = glGetUniformLocation(s_tubeProg->shp_, "u_worldToClipGL");
        s_tloc_uAtlas        = glGetUniformLocation(s_tubeProg->shp_, "uAtlas");
        s_tloc_uAdditive     = glGetUniformLocation(s_tubeProg->shp_, "uAdditive");
    }
}

void tubeEnsureBuffer(GLuint& buf, GLsizei& cap, GLenum target,
                      GLsizei needBytes, GLenum binding /*0=none*/) {
    if (buf == 0) glGenBuffers(1, &buf);
    if (needBytes > cap) {
        GLsizei newCap = (needBytes < 4096) ? 4096 : needBytes;
        glBindBuffer(target, buf);
        glBufferData(target, (GLsizeiptr)newCap, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(target, 0);
        cap = newCap;
    }
    (void)binding;
}

// ── VK-OPT-1: tube-ribbon upload instrumentation ──────────────────────────
// Automatable redundancy detection for the class-D tube upload path. Counts
// bytes pushed to the GPU + heap allocations per frame so a regression that
// re-introduces per-record scratch allocs or a fatter vertex format is
// measurable (and catchable in a future gate). Behind MC2_VFX_TUBE_UPLOAD_STATS
// so it is zero-cost when off. Frame totals are dumped from the deferred flush.
struct TubeUploadStats {
    unsigned long long uploadBytes   = 0;  // total glBufferSubData payload bytes this frame (opt)
    unsigned long long baselineBytes = 0;  // what the pre-opt vec4-padded format would upload
    unsigned long long heapAllocs    = 0;  // per-record scratch allocations this frame (0 with opt)
    unsigned int       records       = 0;  // ribbon records flushed this frame
    unsigned int       posVerts      = 0;  // total position verts uploaded (for the 12B vs 16B split)
};
TubeUploadStats s_tubeStats;

bool tubeUploadStatsEnabled() {
    static int cached = -1;
    if (cached < 0) {
        cached = mc2::gate::get<"MC2_VFX_TUBE_UPLOAD_STATS">();
    }
    return cached != 0;
}

// Orphan-then-upload: rename the buffer's storage (glBufferData NULL) before
// writing, so the driver does NOT implicit-sync-stall waiting for the previous
// frame's draw to finish reading the same buffer (class-D fix). The orphan
// reuses the buffer's existing capacity (`cap`), so it is a cheap rename, not a
// reallocation. Reusable template for the other class-D buffers (#4/#13).
void tubeOrphanUpload(GLuint buf, GLsizei cap, GLenum target,
                      GLsizeiptr bytes, const void* data) {
    glBindBuffer(target, buf);
    glBufferData(target, (GLsizeiptr)cap, nullptr, GL_DYNAMIC_DRAW);  // orphan
    glBufferSubData(target, 0, bytes, data);
    if (tubeUploadStatsEnabled()) s_tubeStats.uploadBytes += (unsigned long long)bytes;
}

}  // namespace (tube ribbon)

extern "C" void gos_tube_ribbon_flush(const float*          positions,
                                      const float*          colors,
                                      const float*          uvs,
                                      unsigned int          numVerts,
                                      const unsigned short* indices,
                                      unsigned int          numIndices,
                                      unsigned int          gosHandle,
                                      int                   blendMode) {
    mc2::ScopedFlushTimer _pfst(1, "tube");  // PARTICLE-FLUSH-STALL-MEASURE-1
    if (numVerts == 0 || numIndices == 0 ||
        positions == nullptr || colors == nullptr ||
        uvs == nullptr || indices == nullptr) return;

    tubeEnsureInitialized();
    if (s_tubeInitFailed || s_tubeProg == nullptr || s_tubeProg->shp_ == 0) {
        static bool s_failLogged = false;
        if (!s_failLogged) {
            s_failLogged = true;
            MC2_DIAGFE(stderr, "[VFX_ORACLE_TUBE v1] ERROR ribbon_init_failed\n");
            std::fflush(stderr);
        }
        return;
    }

    // Resolve the gos texture handle to a GL name; skip if not resident.
    const GLuint glTex = (gosHandle != 0) ? (GLuint)gos_GetGLTextureName(gosHandle) : 0u;
    if (glTex == 0) {
        if (groupLogEnabled()) {
            static std::unordered_set<uint32_t> s_loggedMissing;
            if (s_loggedMissing.insert(gosHandle).second) {
                MC2_DIAGFE(stderr,
                    "[VFX_ORACLE_TUBE v1] skip missing_texture handle=%u\n", gosHandle);
                std::fflush(stderr);
            }
        }
        return;
    }

    // First-flush banner.
    {
        static bool s_banner = false;
        if (!s_banner) {
            s_banner = true;
            MC2_DIAGFE(stderr,
                "[VFX_ORACLE_TUBE v1] enabled=1 verts=%u indices=%u tex=%u blend=%s\n",
                numVerts, numIndices, gosHandle,
                blendMode == 1 ? "additive" : "alpha");
            std::fflush(stderr);
        }
    }

    // ── State save (GLSTATE-GUARD-ADOPTION-4: RAII, replaces the hand-rolled
    // save/restore + trailing manual invalidate; declaration order = reverse
    // of the old restore order, invalidate guard first so it fires last; the
    // GlScopedTextureUnit ctor also leaves GL_TEXTURE0 active, matching the
    // old explicit glActiveTexture(GL_TEXTURE0) here) ────────────────────
    mc2gl::GlScopedRenderStateInvalidate stateResync;
    mc2gl::GlScopedVertexArray  guardVao;
    mc2gl::GlScopedProgram      guardProgram;
    mc2gl::GlScopedCapability   guardBlend(GL_BLEND);
    mc2gl::GlScopedBlendFunc    guardBlendFunc;
    mc2gl::GlScopedCapability   guardDepthTest(GL_DEPTH_TEST);
    mc2gl::GlScopedDepthState   guardDepth;
    mc2gl::GlScopedSampler      guardSampler0(0);
    mc2gl::GlScopedTextureUnit  guardTexUnit0(0);
    mc2gl::GlScopedCapability   guardCullFace(GL_CULL_FACE);

    // ── Bind VAO + IBO (trap #4: AMD drops draws on VAO=0) ─────────────
    glBindVertexArray(s_tubeVao);

    // ── Upload mesh into the 3 SSBOs + the element buffer ──────────────
    // VK-OPT-1: tight float[] position layout (see deferred path) — keeps this
    // dead-but-live-shader path consistent with the shared tube_ribbon.vert.
    tubeEnsureBuffer(s_tubePosSsbo, s_tubePosCap, GL_SHADER_STORAGE_BUFFER,
                     (GLsizei)(numVerts * 3u * sizeof(float)), 0);
    tubeEnsureBuffer(s_tubeColSsbo, s_tubeColCap, GL_SHADER_STORAGE_BUFFER,
                     (GLsizei)(numVerts * 4u * sizeof(float)), 0);
    tubeEnsureBuffer(s_tubeUvSsbo,  s_tubeUvCap,  GL_SHADER_STORAGE_BUFFER,
                     (GLsizei)(numVerts * 2u * sizeof(float)), 0);
    tubeEnsureBuffer(s_tubeIbo,     s_tubeIdxCap, GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizei)(numIndices * sizeof(unsigned short)), 0);

    // Orphan-then-upload native data (no expansion / no scratch alloc).
    tubeOrphanUpload(s_tubePosSsbo, s_tubePosCap, GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)(numVerts * 3u * sizeof(float)), positions);
    tubeOrphanUpload(s_tubeColSsbo, s_tubeColCap, GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)(numVerts * 4u * sizeof(float)), colors);
    tubeOrphanUpload(s_tubeUvSsbo,  s_tubeUvCap,  GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)(numVerts * 2u * sizeof(float)), uvs);
    tubeOrphanUpload(s_tubeIbo,     s_tubeIdxCap, GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)(numIndices * sizeof(unsigned short)), indices);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 14, s_tubePosSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, s_tubeColSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 16, s_tubeUvSsbo);

    // ── Program + uniforms ─────────────────────────────────────────────
    glUseProgram(s_tubeProg->shp_);
    {
        const float* mvp = gos_GetTerrainMVPMat4();  // row-major direct (GL_FALSE)
        if (mvp && s_tloc_worldToClipGL >= 0)
            glUniformMatrix4fv(s_tloc_worldToClipGL, 1, GL_FALSE, mvp);
    }
    if (s_tloc_uAtlas >= 0) glUniform1i(s_tloc_uAtlas, 0);
    // Blend-aware fragment discard: additive (PPC/ER-PPC) ribbons carry low/zero
    // per-vertex alpha (additive uses RGB, not alpha coverage), so the alpha
    // discard written for alpha ribbons must NOT apply to them. blendMode 1 =
    // additive (GL_SRC_ALPHA,GL_ONE below), 0 = alpha.
    if (s_tloc_uAdditive >= 0) glUniform1i(s_tloc_uAdditive, blendMode == 1 ? 1 : 0);

    // ── Texture + sampler on unit 0 ────────────────────────────────────
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, glTex);
    glBindSampler(0, s_tubeSampler);

    // ── Depth + blend (depth-test ON, depth-write OFF; alpha blend) ────
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);  // reverse-Z convention
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    if (blendMode == 1) glBlendFunc(GL_ONE, GL_ONE);               // pure additive = legacy OneOneMode parity (dead path; deferred is live)
    else                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // Ribbon is a swept double-sided strip; disable face culling so it shows
    // from both sides regardless of profile winding.
    glDisable(GL_CULL_FACE);

    glDrawElements(GL_TRIANGLES, (GLsizei)numIndices, GL_UNSIGNED_SHORT, (const void*)0);

    // ── Unbind SSBO slots (mirror billboard flush hygiene) ─────────────
    // (bind-then-zero HYGIENE, not save/restore — deliberately NOT converted
    // to restore-previous guards, per the epilogue recon: a restore-previous
    // guard here would change semantics, the C1B-SSBO-CLEANUP class.)
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 14, 0u);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, 0u);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 16, 0u);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    // State restore + cache invalidate: RAII scope exit (guards at the top).
}

// ── TUBE-DEFERRED-FLUSH-1: per-frame ribbon queue ─────────────────────────
// Records are deep-copied at enqueue time (mclib pointers are transient).
// Positions are already in MC2 WORLD space (local_to_world applied by caller).
// The queue is drained once per frame from gos_tube_ribbon_flush_deferred().
namespace {

struct RibbonRecord {
    std::vector<float>          positions;   // numVerts * 3 floats, MC2 world space
    std::vector<float>          colors;      // numVerts * 4 floats RGBA
    std::vector<float>          uvs;         // numVerts * 2 floats
    std::vector<unsigned short> indices;     // numIndices uint16
    unsigned int                gosHandle;
    int                         blendMode;   // 0=alpha, 1=additive
};

std::vector<RibbonRecord> s_ribbonQueue;

}  // namespace (tube deferred queue)

extern "C" void gos_tube_ribbon_enqueue(const float*          positions,
                                        const float*          colors,
                                        const float*          uvs,
                                        unsigned int          numVerts,
                                        const unsigned short* indices,
                                        unsigned int          numIndices,
                                        unsigned int          gosHandle,
                                        int                   blendMode) {
    // gosHandle==0 must NOT be enqueued: untextured ribbons belong to the legacy
    // MLR path (DrawEffect via the MLR sorter, which is correctly post-renderLists).
    // The caller leaves ribbonSubmitted=false in that case so DrawEffect runs.
    if (gosHandle == 0) return;
    if (numVerts == 0 || numIndices == 0 ||
        positions == nullptr || colors == nullptr ||
        uvs == nullptr || indices == nullptr) return;

    RibbonRecord rec;
    rec.gosHandle  = gosHandle;
    rec.blendMode  = blendMode;
    rec.positions.assign(positions, positions + (size_t)numVerts * 3u);
    rec.colors.assign   (colors,    colors    + (size_t)numVerts * 4u);
    rec.uvs.assign      (uvs,       uvs       + (size_t)numVerts * 2u);
    rec.indices.assign  (indices,   indices   + (size_t)numIndices);
    s_ribbonQueue.push_back(std::move(rec));
}

extern "C" void gos_tube_ribbon_flush_deferred(void) {
    // Q2-S0: this drain is called once per WALL frame from the unconditional
    // particlesFlush phase (code/gamecam.cpp ~454), so it is the per-frame tick
    // for the [FX_COST v1] summary. Tick BEFORE the empty-queue early-return so
    // the 600-frame denominator counts wall frames, not just tube-active ones.
    mc2::fx_cost_split::roll_frame_and_maybe_emit();

    if (s_ribbonQueue.empty()) return;
    mc2::ScopedFlushTimer _pfst(2, "tube_deferred");  // PARTICLE-FLUSH-STALL-MEASURE-1

    tubeEnsureInitialized();
    if (s_tubeInitFailed || s_tubeProg == nullptr || s_tubeProg->shp_ == 0) {
        s_ribbonQueue.clear();
        return;
    }

    // Q2-S0 instrumentation only (no behavior change). Coarse zone + bucket for
    // the per-frame ribbon SSBO upload + draw. Placed after the guards so empty
    // frames are not instrumented.
    ZoneScopedN("gos.TubeRibbon.FlushDeferred");
    mc2::fx_cost_split::Scope _fxcs(mc2::fx_cost_split::B_TUBE_BRIDGE_FLUSH);

    // VK-OPT-1: reset per-frame upload counters (heapAllocs is 0 by construction
    // now — the per-record scratch vector is gone; the field stays so a future
    // regression that re-adds a scratch alloc shows up non-zero).
    if (tubeUploadStatsEnabled()) s_tubeStats = TubeUploadStats{};

    // ── State save (GLSTATE-GUARD-ADOPTION-4: RAII, same set as the
    // immediate flush; see the immediate flush for the ordering rationale) ──
    mc2gl::GlScopedRenderStateInvalidate stateResync;
    mc2gl::GlScopedVertexArray  guardVao;
    mc2gl::GlScopedProgram      guardProgram;
    mc2gl::GlScopedCapability   guardBlend(GL_BLEND);
    mc2gl::GlScopedBlendFunc    guardBlendFunc;
    mc2gl::GlScopedCapability   guardDepthTest(GL_DEPTH_TEST);
    mc2gl::GlScopedDepthState   guardDepth;
    mc2gl::GlScopedSampler      guardSampler0(0);
    mc2gl::GlScopedTextureUnit  guardTexUnit0(0);
    mc2gl::GlScopedCapability   guardCullFace(GL_CULL_FACE);

    // ── Bind VAO (trap #4: AMD drops draws on VAO=0) ───────────────────
    glBindVertexArray(s_tubeVao);

    // ── Program + shared uniforms (set ONCE for the whole batch) ──────
    glUseProgram(s_tubeProg->shp_);
    {
        const float* mvp = gos_GetTerrainMVPMat4();
        if (mvp && s_tloc_worldToClipGL >= 0)
            glUniformMatrix4fv(s_tloc_worldToClipGL, 1, GL_FALSE, mvp);
    }
    if (s_tloc_uAtlas >= 0) glUniform1i(s_tloc_uAtlas, 0);

    // ── Shared depth + blend state; per-record blend overridden in loop ─
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);  // reverse-Z convention
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glDisable(GL_CULL_FACE); // ribbon is double-sided

    glBindSampler(0, s_tubeSampler);

    // MRT FIX (root cause of "tubes draw but invisible"): the scene FBO has
    // multiple active draw buffers (color + normal + R32UI objectId — the 3rd
    // attachment landed with the RenderWorld arc, which is exactly when the
    // oracle broke). tube_ribbon.frag writes only location 0; with GL_BLEND
    // enabled AND an integer (R32UI) attachment in the active draw-buffer list,
    // AMD suppresses the COLOR0 write -> ribbons invisible. Probe confirmed
    // drawBufs=3 at draw time. Force a single COLOR_ATTACHMENT0 draw buffer for
    // the ribbon draws (tubes are transparent VFX — they must not write normal
    // or objectId anyway), restore the full MRT list after.
    // (GLSTATE-GUARD-ADOPTION-4: declared AFTER the state guards above, so it
    // destructs FIRST — the MRT list is restored before the state restores,
    // exactly like the old manual epilogue.)
    mc2gl::GlScopedDrawBuffers guardDrawBufs;
    {
        const GLenum single[1] = { GL_COLOR_ATTACHMENT0 };
        glDrawBuffers(1, single);
    }

    // First-deferred-flush banner (once).
    {
        static bool s_banner = false;
        if (!s_banner) {
            s_banner = true;
            mc2::fmt::print_err("[VFX_ORACLE_TUBE deferred] enabled=1 queue={:d}\n", mc2::fmt::as_u32((unsigned)s_ribbonQueue.size()));
            std::fflush(stderr);
        }
    }

    // MC2_VFX_ORACLE_TUBE_COVERAGE: begin an occlusion query over the ribbon
    // draws + capture the bound draw-FBO (scene target). samples>0 with a
    // non-default fbo proves tube fragments rasterize + pass depth into the
    // composited scene FBO -- deterministic, immune to mech-pose nondeterminism.
    const bool covOn  = tubeCoverageEnabled();
    GLint      covFbo = 0;
    if (covOn) {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &covFbo);
        if (s_tubeCoverageQuery == 0) glGenQueries(1, &s_tubeCoverageQuery);
        glBeginQuery(GL_SAMPLES_PASSED, s_tubeCoverageQuery);
    }

    // ── Per-record draw loop ───────────────────────────────────────────
    for (const RibbonRecord& rec : s_ribbonQueue) {
        const unsigned numVerts   = (unsigned)(rec.positions.size() / 3u);
        const unsigned numIndices = (unsigned)(rec.indices.size());
        if (numVerts == 0 || numIndices == 0) continue;

        // Resolve texture; skip records whose handle won't resolve at draw time
        // (rare edge — dominant untextured gosHandle==0 case already blocked at
        // enqueue; non-zero handle that vanished is acceptable to skip).
        const GLuint glTex = (GLuint)gos_GetGLTextureName(rec.gosHandle);
        if (glTex == 0) continue;

        // Ensure SSBOs and IBO are large enough, then upload. VK-OPT-1: the
        // position SSBO is now a TIGHT float[] (3 floats/vert), not a padded
        // vec4[] — no CPU expansion loop, no per-record scratch heap alloc, and
        // 25% fewer position bytes. The w=1 expansion moved into the VS.
        tubeEnsureBuffer(s_tubePosSsbo, s_tubePosCap, GL_SHADER_STORAGE_BUFFER,
                         (GLsizei)(numVerts * 3u * sizeof(float)), 0);
        tubeEnsureBuffer(s_tubeColSsbo, s_tubeColCap, GL_SHADER_STORAGE_BUFFER,
                         (GLsizei)(numVerts * 4u * sizeof(float)), 0);
        tubeEnsureBuffer(s_tubeUvSsbo,  s_tubeUvCap,  GL_SHADER_STORAGE_BUFFER,
                         (GLsizei)(numVerts * 2u * sizeof(float)), 0);
        tubeEnsureBuffer(s_tubeIbo,     s_tubeIdxCap, GL_ELEMENT_ARRAY_BUFFER,
                         (GLsizei)(numIndices * sizeof(unsigned short)), 0);

        // Class-D fix: orphan-then-upload the native record data (no expansion,
        // no scratch alloc) so the driver renames storage instead of
        // implicit-sync-stalling on the previous frame's read.
        tubeOrphanUpload(s_tubePosSsbo, s_tubePosCap, GL_SHADER_STORAGE_BUFFER,
                         (GLsizeiptr)(numVerts * 3u * sizeof(float)), rec.positions.data());
        tubeOrphanUpload(s_tubeColSsbo, s_tubeColCap, GL_SHADER_STORAGE_BUFFER,
                         (GLsizeiptr)(numVerts * 4u * sizeof(float)), rec.colors.data());
        tubeOrphanUpload(s_tubeUvSsbo,  s_tubeUvCap,  GL_SHADER_STORAGE_BUFFER,
                         (GLsizeiptr)(numVerts * 2u * sizeof(float)), rec.uvs.data());
        tubeOrphanUpload(s_tubeIbo,     s_tubeIdxCap, GL_ELEMENT_ARRAY_BUFFER,
                         (GLsizeiptr)(numIndices * sizeof(unsigned short)), rec.indices.data());
        if (tubeUploadStatsEnabled()) {
            ++s_tubeStats.records;
            s_tubeStats.posVerts += numVerts;
        }

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 14, s_tubePosSsbo);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, s_tubeColSsbo);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 16, s_tubeUvSsbo);

        // Blend-aware fragment discard uniform (per-record, matches immediate path).
        if (s_tloc_uAdditive >= 0)
            glUniform1i(s_tloc_uAdditive, rec.blendMode == 1 ? 1 : 0);

        // Per-record blend func (alpha vs additive). Additive PPC/ER-PPC Tubes
        // are legacy MLRState::OneOneMode = pure additive GL_ONE,GL_ONE. Using
        // GL_SRC_ALPHA,GL_ONE double-attenuated the bolt (frag already folds
        // alpha into RGB via tex*v_color, then SRC_ALPHA scales by the small
        // age-ramped per-vertex alpha again) -> bright core + faint ghost ribbon
        // = the "extra tube / partial transparency" artifact. Match legacy.
        // VFX-APPLYPIPELINE-ROUTING-1: per-record blend via VfxTube row
        // (additive = AdditiveOneOne = ONE/ONE; alpha = SRC_ALPHA/ONE_MINUS_SRC_ALPHA).
        // Same GL state as the old hand-set; program(0)=skip keeps the tube program.
        pipeline_binder::applyPipeline(
            RenderCore::getPipelineDesc(rec.blendMode == 1
                ? RenderCore::PipelineId::VfxTubeAdditive
                : RenderCore::PipelineId::VfxTubeAlpha),
            rec.blendMode == 1 ? "VfxTubeAdditive" : "VfxTubeAlpha");

        glBindTexture(GL_TEXTURE_2D, glTex);
        glDrawElements(GL_TRIANGLES, (GLsizei)numIndices, GL_UNSIGNED_SHORT, (const void*)0);
    }

    // MC2_VFX_ORACLE_TUBE_COVERAGE: end the query, read the sample count (this
    // stalls -- acceptable for a gated diagnostic) and log per-frame coverage.
    // sceneDrawBufs = the scene FBO's MRT attachment count (3 with the
    // RenderWorld arc R32UI objectId attachment) -- confirms the target is the
    // composited scene FBO, not a scratch target.
    if (covOn) {
        glEndQuery(GL_SAMPLES_PASSED);
        GLuint covSamples = 0;
        glGetQueryObjectuiv(s_tubeCoverageQuery, GL_QUERY_RESULT, &covSamples);
        mc2::fmt::print_err("[VFX_ORACLE_TUBE coverage] ribbons={:d} samples={:d} fbo={:d} sceneDrawBufs={:d}\n", mc2::fmt::as_u32((unsigned)s_ribbonQueue.size()), mc2::fmt::as_u32(covSamples), mc2::fmt::as_i32((int)covFbo), mc2::fmt::as_i32(guardDrawBufs.savedCount()));
        std::fflush(stderr);
    }

    // ── Unbind SSBO/IBO slots ──────────────────────────────────────────
    // (bind-then-zero HYGIENE, not save/restore — deliberately NOT converted
    // to restore-previous guards, per the epilogue recon: a restore-previous
    // guard here would change semantics, the C1B-SSBO-CLEANUP class.)
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 14, 0u);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, 0u);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 16, 0u);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    // State restore (MRT list, cull/tex/sampler/depth/blend/program/VAO) +
    // cache invalidate: RAII scope exit (guards declared at function top).

    // VK-OPT-1: per-frame upload/alloc summary (gated). Prints total bytes
    // pushed to the GPU + per-record scratch allocs so the redundancy is
    // measurable and a regression is catchable. With the fix heapAllocs=0 and
    // uploadBytes reflects the tight (pos=12B/vert) format.
    if (tubeUploadStatsEnabled()) {
        // baselineBytes = opt bytes + the 4 bytes/vert of position padding the
        // old vec4-format uploaded (16B/vert) but the tight float[] (12B/vert)
        // does not. saved = baselineBytes - uploadBytes.
        s_tubeStats.baselineBytes =
            s_tubeStats.uploadBytes + (unsigned long long)s_tubeStats.posVerts * 4ull;
        const unsigned long long saved =
            s_tubeStats.baselineBytes - s_tubeStats.uploadBytes;
        mc2::fmt::print_err("[VFX_TUBE_UPLOAD_STATS] records={:d} posVerts={:d} uploadBytes={:d} baselineBytes={:d} savedBytes={:d} heapAllocs={:d}\n", mc2::fmt::as_u32(s_tubeStats.records), mc2::fmt::as_u32(s_tubeStats.posVerts), mc2::fmt::as_u64(s_tubeStats.uploadBytes), mc2::fmt::as_u64(s_tubeStats.baselineBytes), mc2::fmt::as_u64(saved), mc2::fmt::as_u64(s_tubeStats.heapAllocs));
        std::fflush(stderr);
    }

    s_ribbonQueue.clear();
}
