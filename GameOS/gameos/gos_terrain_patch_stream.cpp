// GameOS/gameos/gos_terrain_patch_stream.cpp
#include "gos_terrain_patch_stream.h"
#include "gos_terrain_bridge.h"   // gos_terrain_bridge_* free functions (Task 0)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gameos.hpp"   // gos_VERTEX, gos_TERRAIN_EXTRA, DWORD, gos_SetRenderState, gos_State_*
#include "gl/glew.h"    // OpenGL — same include the static-prop batcher uses
#include "utils/timing.h"  // timing::get_wall_time_ms()

// tex_resolve() — lazy per-frame memoization of terrain texture handles.
// Defined in mclib but the inline is in the header; pull that header in.
// tex_resolve_table.h includes txmmgr.h for mcTextureManager + MC_MAXTEXTURES.
#include "../../mclib/tex_resolve_table.h"

// Load-bearing invariant: flush() computes ONE slotFirstVert from the color
// ring's vertex pitch and uses it to index BOTH the color VBO and the
// extras VBO via glDrawArrays' `first` parameter (which applies uniformly
// to all bound vertex attributes). This requires the two rings to have
// identical per-slot vertex capacity. If the constants are ever tuned
// independently, this assert fires at build time before silent
// misalignment can corrupt extras data. Fix via either:
//   (a) keep constants in lockstep so the math is identical, or
//   (b) refactor flush() to use per-attrib base offsets instead of
//       glDrawArrays' shared `first` (more invasive).
static_assert(
    kPatchStreamColorBytesPerSlot  / sizeof(gos_VERTEX) ==
    kPatchStreamExtrasBytesPerSlot / sizeof(gos_TERRAIN_EXTRA),
    "Color and extras rings must have equal per-slot vertex capacity; "
    "slotFirstVert is shared between them via glDrawArrays' `first` arg");

namespace {
    bool s_killswitch = false;
    bool s_initOk     = false;
    bool s_traceOn    = false;

    // GL handles. Two separate buffers — one for color, one for extras.
    GLuint s_colorBuf  = 0;
    GLuint s_extrasBuf = 0;

    // Persistent-mapped CPU pointers. Indexed by [slot * bytesPerSlot + offset].
    void* s_colorMap   = nullptr;
    void* s_extrasMap  = nullptr;

    // Fences per slot. Created at end of flush(); consumed at beginFrame() before
    // re-using the slot. NULL means "no fence yet" (first N frames).
    GLsync s_fence[kPatchStreamRingFrames] = { 0, 0, 0 };

    uint32_t s_slot = 0;  // index of the slot currently being written

    // Per-texture CPU staging. Fixed-size array of buckets — capacity is
    // retained across frames (clear() empties contents but keeps each
    // bucket's std::vector backing storage). beginFrame() resets
    // s_stagingCount and clear()s the live buckets only; the bucket
    // vectors never get destroyed during normal operation.
    //
    // Linear scan lookup over s_stagingCount entries (count is small —
    // ~5-40 distinct textures per frame), cheaper than unordered_map +
    // zero per-frame heap churn after warmup.
    struct PatchStagingBucket {
        DWORD                          textureIndex = 0;
        std::vector<gos_VERTEX>        color;
        std::vector<gos_TERRAIN_EXTRA> extras;
    };

    PatchStagingBucket s_staging[kPatchStreamMaxBuckets];
    uint32_t           s_stagingCount = 0;
    uint32_t           s_totalVerts   = 0;
    bool               s_overflow     = false;

    // Filled at flush() time from the staging buckets — this is what
    // issueDraws walks for per-bucket glDrawArrays. PatchStreamBucket
    // is declared in the header.
    PatchStreamBucket s_drawBuckets[kPatchStreamMaxBuckets];
    uint32_t          s_drawBucketCount = 0;

    // Telemetry
    bool s_firstFlushSeen = false;

    // Drop GL state we touched, mirroring gos_static_prop_batcher's save/restore.
    struct SavedGLState {
        GLint  arrayBuf      = 0;
        GLint  vao           = 0;
        GLboolean blend      = GL_FALSE;
        GLboolean depthTest  = GL_FALSE;
    };

    void saveGLState(SavedGLState& s) {
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING,  &s.arrayBuf);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING,  &s.vao);
        s.blend     = glIsEnabled(GL_BLEND);
        s.depthTest = glIsEnabled(GL_DEPTH_TEST);
    }

    void restoreGLState(const SavedGLState& s) {
        glBindBuffer(GL_ARRAY_BUFFER, s.arrayBuf);
        glBindVertexArray(s.vao);
        if (s.blend)     glEnable (GL_BLEND);     else glDisable(GL_BLEND);
        if (s.depthTest) glEnable (GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    }

    // Linear-scan lookup over the active prefix of s_staging.
    // Returns nullptr on overflow.
    PatchStagingBucket* findOrCreateStagingBucket(DWORD textureIndex) {
        for (uint32_t i = 0; i < s_stagingCount; ++i) {
            if (s_staging[i].textureIndex == textureIndex) return &s_staging[i];
        }
        if (s_stagingCount >= kPatchStreamMaxBuckets) {
            fprintf(stderr,
                "[PATCH_STREAM v1] event=overflow slot=%u kind=bucket_count count=%u cap=%u\n",
                s_slot, s_stagingCount, kPatchStreamMaxBuckets);
            fflush(stderr);
            s_overflow = true;
            return nullptr;
        }
        PatchStagingBucket& nb = s_staging[s_stagingCount++];
        nb.textureIndex = textureIndex;
        // nb.color / nb.extras are cleared (size 0) but their reserved
        // capacity from init() is intact. No allocation here.
        return &nb;
    }
}

static GLuint allocPersistentBuffer(GLsizeiptr totalBytes, void** outMappedPtr) {
    const GLbitfield flags = GL_MAP_WRITE_BIT
                           | GL_MAP_PERSISTENT_BIT
                           | GL_MAP_COHERENT_BIT;

    GLuint id = 0;
    glGenBuffers(1, &id);
    if (!id) return 0;

    glBindBuffer(GL_ARRAY_BUFFER, id);
    glBufferStorage(GL_ARRAY_BUFFER, totalBytes, nullptr, flags);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteBuffers(1, &id);
        return 0;
    }

    void* p = glMapBufferRange(GL_ARRAY_BUFFER, 0, totalBytes, flags);
    if (!p) {
        glDeleteBuffers(1, &id);
        return 0;
    }
    *outMappedPtr = p;
    return id;
}

bool TerrainPatchStream::init()
{
    const char* env = getenv("MC2_MODERN_TERRAIN_SURFACE");
    s_killswitch = (env != nullptr) && (env[0] == '1');
    s_traceOn    = (getenv("MC2_PATCH_STREAM_TRACE") != nullptr);

    if (!s_killswitch) return true;

    // Step 4 bailout — env-gated forced init failure for testing the
    // fallback path without driver stress. Default off; intentional debug
    // instrumentation, leave in tree (memory/debug_instrumentation_rule.md).
    if (getenv("MC2_PATCH_STREAM_FORCE_INIT_FAIL")) {
        fprintf(stderr,
            "[PATCH_STREAM v1] event=init_fail reason=force_env\n");
        fflush(stderr);
        s_killswitch = false;
        return true;  // engine continues on legacy
    }

    SavedGLState saved;
    saveGLState(saved);

    const GLsizeiptr colorTotal  = (GLsizeiptr)kPatchStreamColorBytesPerSlot  * kPatchStreamRingFrames;
    const GLsizeiptr extrasTotal = (GLsizeiptr)kPatchStreamExtrasBytesPerSlot * kPatchStreamRingFrames;

    s_colorBuf  = allocPersistentBuffer(colorTotal,  &s_colorMap);
    s_extrasBuf = allocPersistentBuffer(extrasTotal, &s_extrasMap);

    restoreGLState(saved);

    if (!s_colorBuf || !s_extrasBuf) {
        // Init-fail path. Force killswitch off for the rest of the process.
        fprintf(stderr,
            "[PATCH_STREAM v1] event=init_fail reason=glBufferStorage_or_map "
            "colorBuf=%u extrasBuf=%u\n", s_colorBuf, s_extrasBuf);
        fflush(stderr);
        if (s_colorBuf) {
            glBindBuffer(GL_ARRAY_BUFFER, s_colorBuf);
            if (s_colorMap) { glUnmapBuffer(GL_ARRAY_BUFFER); s_colorMap = nullptr; }
            glDeleteBuffers(1, &s_colorBuf);
            s_colorBuf = 0;
        }
        if (s_extrasBuf) {
            glBindBuffer(GL_ARRAY_BUFFER, s_extrasBuf);
            if (s_extrasMap) { glUnmapBuffer(GL_ARRAY_BUFFER); s_extrasMap = nullptr; }
            glDeleteBuffers(1, &s_extrasBuf);
            s_extrasBuf = 0;
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);  // leave clean
        s_killswitch = false;
        return true;  // engine continues on legacy path
    }

    // One-shot reserve so each bucket's std::vector never reallocates
    // during steady-state frames. Total CPU staging RAM at full capacity:
    //   kPatchStreamMaxBuckets * 4 K verts * (sizeof(gos_VERTEX) + sizeof(gos_TERRAIN_EXTRA))
    //   = 512 * 4096 * (32 + 24) bytes ≈ 115 MB worst case if every
    //   bucket maxes out. Typical mc2_01 standard zoom: ~64-128 active
    //   buckets × ~600-1500 verts × 56 B ≈ 5-10 MB resident.
    //
    // The per-bucket reserve was lowered from 32K to 4K when the bucket
    // cap was raised from 64 to 512 (post-Task-5 verification revealed
    // raw terrainHandle counts of 64+ per frame on mc2_01, far exceeding
    // the audit-derived 5-15 estimate which was the post-mcTextureManager
    // node count, not the raw callsite count). Total memory budget
    // unchanged; just redistributed across more, smaller buckets.
    for (auto& b : s_staging) {
        b.color.reserve(4 * 1024);
        b.extras.reserve(4 * 1024);
    }

    s_initOk = true;
    fprintf(stderr,
        "[PATCH_STREAM v1] event=init slots=%u colorBytes=%u extrasBytes=%u "
        "colorBuf=%u extrasBuf=%u trace=%d\n",
        kPatchStreamRingFrames,
        kPatchStreamColorBytesPerSlot,
        kPatchStreamExtrasBytesPerSlot,
        s_colorBuf, s_extrasBuf, (int)s_traceOn);
    fflush(stderr);
    return true;
}

void TerrainPatchStream::destroy()
{
    if (!s_initOk) return;
    fprintf(stderr, "[PATCH_STREAM v1] event=shutdown\n");
    fflush(stderr);

    for (uint32_t i = 0; i < kPatchStreamRingFrames; ++i) {
        if (s_fence[i]) { glDeleteSync(s_fence[i]); s_fence[i] = 0; }
    }
    if (s_colorBuf) {
        glBindBuffer(GL_ARRAY_BUFFER, s_colorBuf);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glDeleteBuffers(1, &s_colorBuf);
        s_colorBuf = 0; s_colorMap = nullptr;
    }
    if (s_extrasBuf) {
        glBindBuffer(GL_ARRAY_BUFFER, s_extrasBuf);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glDeleteBuffers(1, &s_extrasBuf);
        s_extrasBuf = 0; s_extrasMap = nullptr;
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    s_initOk = false;
}

bool TerrainPatchStream::isReady()      { return s_killswitch && s_initOk; }
bool TerrainPatchStream::isOverflowed() { return s_overflow; }

void TerrainPatchStream::beginFrame()
{
    if (!s_initOk || !s_killswitch) return;

    s_slot = (s_slot + 1) % kPatchStreamRingFrames;

    // Wait on the slot's fence (only the second time we visit a slot,
    // when it has been signaled by an earlier flush). With 3 slots the
    // GPU has typically finished with slot N by the time the CPU comes
    // back around, so this is normally a near-instant signal check —
    // but `GL_TIMEOUT_IGNORED` does mean an indefinite block if the GPU
    // is genuinely behind. We accept the blocking wait for safety in M0b
    // (better to stall the CPU than to write into a slot the GPU is
    // still reading), and log when the wait actually takes nontrivial
    // time so we can spot stalls in profiling.
    if (s_fence[s_slot]) {
        const uint64_t t0 = timing::get_wall_time_ms();
        glClientWaitSync(s_fence[s_slot], GL_SYNC_FLUSH_COMMANDS_BIT,
                         GL_TIMEOUT_IGNORED);
        const uint64_t waitedMs = timing::get_wall_time_ms() - t0;
        glDeleteSync(s_fence[s_slot]);
        s_fence[s_slot] = nullptr;
        if (waitedMs >= 1) {
            fprintf(stderr,
                "[PATCH_STREAM v1] event=fence_stall slot=%u waited_ms=%llu\n",
                s_slot, (unsigned long long)waitedMs);
            fflush(stderr);
        }
    }

    // Reset per-frame state. Buckets are clear()ed (contents emptied)
    // but their reserved capacity from init() is retained — no
    // allocator churn after warmup. s_stagingCount goes to 0 so the
    // linear-scan lookup in findOrCreateStagingBucket only walks
    // currently-active buckets.
    for (uint32_t i = 0; i < s_stagingCount; ++i) {
        s_staging[i].color.clear();
        s_staging[i].extras.clear();
    }
    s_stagingCount    = 0;
    s_totalVerts      = 0;
    s_drawBucketCount = 0;
    s_overflow        = false;
}

void TerrainPatchStream::appendTriangle(DWORD textureIndex,
                                        const gos_VERTEX* vColor,
                                        const gos_TERRAIN_EXTRA* vExtra)
{
    if (!s_initOk || !s_killswitch) return;
    if (s_overflow) return;  // sticky for the whole frame

    constexpr uint32_t vertsPerTri = 3;

    // Per-slot capacity in *vertices* — same as how flush() will copy out.
    const uint32_t maxVertsThisSlot =
        kPatchStreamColorBytesPerSlot / (uint32_t)sizeof(gos_VERTEX);

    if (s_totalVerts + vertsPerTri > maxVertsThisSlot) {
        fprintf(stderr,
            "[PATCH_STREAM v1] event=overflow slot=%u kind=byte_budget cursor=%u cap=%u\n",
            s_slot, s_totalVerts, maxVertsThisSlot);
        fflush(stderr);
        s_overflow = true;
        return;
    }

    PatchStagingBucket* bk = findOrCreateStagingBucket(textureIndex);
    if (!bk) return;  // overflow already logged

    bk->color.insert(bk->color.end(),  vColor, vColor + vertsPerTri);
    bk->extras.insert(bk->extras.end(), vExtra, vExtra + vertsPerTri);
    s_totalVerts += vertsPerTri;
}

bool TerrainPatchStream::flush()
{
    if (!s_initOk || !s_killswitch) return false;
    if (s_overflow) {
        // Caller falls through to legacy. No fence emitted — no draws
        // were issued, so the slot is unchanged.
        return false;
    }
    if (s_stagingCount == 0 || s_totalVerts == 0) {
        // Nothing to draw — treat as success so caller skips legacy too.
        return true;
    }

    SavedGLState saved;
    saveGLState(saved);

    // 1. Consolidate staging into the persistent ring at the active slot.
    //    Walk staging buckets in deterministic order (insertion order =
    //    first-append-per-texture order), copy each bucket's color +
    //    extras into contiguous regions, record firstVertex / vertexCount
    //    for per-bucket draws.
    const uint32_t slotFirstVert =
        s_slot * (kPatchStreamColorBytesPerSlot / (uint32_t)sizeof(gos_VERTEX));
    gos_VERTEX*        colorSlot  = (gos_VERTEX*)s_colorMap  + slotFirstVert;
    gos_TERRAIN_EXTRA* extrasSlot = (gos_TERRAIN_EXTRA*)s_extrasMap + slotFirstVert;

    uint32_t cursor = 0;
    s_drawBucketCount = 0;
    for (uint32_t i = 0; i < s_stagingCount; ++i) {
        const PatchStagingBucket& sb = s_staging[i];
        if (sb.color.empty()) continue;
        const uint32_t n = (uint32_t)sb.color.size();   // == sb.extras.size()

        memcpy(colorSlot  + cursor, sb.color.data(),  n * sizeof(gos_VERTEX));
        memcpy(extrasSlot + cursor, sb.extras.data(), n * sizeof(gos_TERRAIN_EXTRA));

        PatchStreamBucket& db = s_drawBuckets[s_drawBucketCount++];
        db.textureIndex = sb.textureIndex;
        db.firstVertex  = cursor;
        db.vertexCount  = n;
        cursor += n;
    }

    // 2. Consolidated per-frame upload of terrain_extra_vb_ for grass + any
    //    legacy reader (§7.5 Option A). ONE glBufferData call regardless of
    //    bucket count, sourced from the contiguous extras region we just
    //    wrote into the persistent slot.
    const GLsizeiptr extrasBytes = (GLsizeiptr)cursor * sizeof(gos_TERRAIN_EXTRA);
    GLuint legacyExtraVB = (GLuint)gos_terrain_bridge_getExtraVB();
    if (legacyExtraVB && extrasBytes > 0) {
        glBindBuffer(GL_ARRAY_BUFFER, legacyExtraVB);
        glBufferData(GL_ARRAY_BUFFER, extrasBytes, extrasSlot, GL_DYNAMIC_DRAW);
    }

    // 3. Bind uniforms via the engine bridge — sets the program, samplers,
    //    terrainMVP GL_FALSE, all tess + splatting + shadow uniforms.
    //    apply() inside the bridge calls glUseProgram, after which the
    //    direct glUniform* calls land on the right program (AMD rule).
    gosRenderMaterial* mat = gos_terrain_bridge_getMaterial();
    if (!mat) {
        // No terrain material — abort modern path. Caller falls back to legacy.
        restoreGLState(saved);
        return false;
    }
    gos_terrain_bridge_bindUniforms(mat);

    // 4. Bind our persistent color ring as GL_ARRAY_BUFFER and issue
    //    applyVertexDeclaration so locations 0-3 read from it.
    glBindBuffer(GL_ARRAY_BUFFER, s_colorBuf);
    gos_terrain_bridge_applyVertexDeclaration(mat);

    // 5. Bind our persistent extras ring at locations 4-5 (worldPos / worldNorm).
    //    Cache attrib locations on first use to avoid per-draw glGetAttribLocation stall.
    static GLint locWorldPos  = -1;
    static GLint locWorldNorm = -1;
    if (locWorldPos < 0 || locWorldNorm < 0) {
        GLuint shp = (GLuint)gos_terrain_bridge_getShaderProgram();
        if (shp) {
            locWorldPos  = glGetAttribLocation(shp, "worldPos");
            locWorldNorm = glGetAttribLocation(shp, "worldNorm");
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, s_extrasBuf);
    // CRITICAL: attribute pointer offset is 0 (just the field offset
    // within gos_TERRAIN_EXTRA). The slot offset is applied EXACTLY ONCE
    // via the `first` argument of glDrawArrays below. If we also baked
    // it into the pointer offset, the GPU would read from
    // (slotFirstVert + firstVertex) for color but
    // (2*slotFirstVert + firstVertex) for extras — desyncing worldPos /
    // worldNorm from screen-space color data.
    if (locWorldPos >= 0) {
        glEnableVertexAttribArray(locWorldPos);
        glVertexAttribPointer(locWorldPos, 3, GL_FLOAT, GL_FALSE,
            sizeof(gos_TERRAIN_EXTRA),
            (void*)0);
    }
    if (locWorldNorm >= 0) {
        glEnableVertexAttribArray(locWorldNorm);
        glVertexAttribPointer(locWorldNorm, 3, GL_FLOAT, GL_FALSE,
            sizeof(gos_TERRAIN_EXTRA),
            (void*)(3 * sizeof(float)));
    }

    glPatchParameteri(GL_PATCH_VERTICES, 3);

    // 6. Per-bucket draws. Each bucket = one texture change. The slot offset
    //    is added to the `first` argument of glDrawArrays — this is the ONE
    //    place the slot offset is applied, for both the color VBO and the
    //    extras VBO simultaneously (since glDrawArrays' `first` is
    //    passed to every bound vertex attribute, both rings advance in lockstep).
    for (uint32_t b = 0; b < s_drawBucketCount; ++b) {
        const PatchStreamBucket& bk = s_drawBuckets[b];
        gos_SetRenderState(gos_State_TextureAddress, gos_TextureClamp);
        gos_SetRenderState(gos_State_Terrain, 1);
        gos_SetRenderState(gos_State_Texture, tex_resolve(bk.textureIndex));

        glDrawArrays(GL_PATCHES,
                     (GLint)(slotFirstVert + bk.firstVertex),
                     (GLsizei)bk.vertexCount);
    }

    if (locWorldPos  >= 0) glDisableVertexAttribArray(locWorldPos);
    if (locWorldNorm >= 0) glDisableVertexAttribArray(locWorldNorm);
    gos_terrain_bridge_endVertexDeclaration(mat);
    gos_terrain_bridge_end(mat);

    // 7. Fence the slot.
    if (s_fence[s_slot]) glDeleteSync(s_fence[s_slot]);
    s_fence[s_slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    restoreGLState(saved);

    if (!s_firstFlushSeen) {
        s_firstFlushSeen = true;
        fprintf(stderr,
            "[PATCH_STREAM v1] event=first_flush slot=%u verts=%u buckets=%u\n",
            s_slot, cursor, s_drawBucketCount);
        fflush(stderr);
    }
    if (s_traceOn) {
        fprintf(stderr,
            "[PATCH_STREAM v1] event=draw_count slot=%u verts=%u buckets=%u\n",
            s_slot, cursor, s_drawBucketCount);
        fflush(stderr);
    }

    return true;
}
