// GameOS/gameos/gos_terrain_patch_stream.cpp
#include "gos_terrain_patch_stream.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gameos.hpp"   // gos_VERTEX, gos_TERRAIN_EXTRA, DWORD
#include "gl/glew.h"    // OpenGL — same include the static-prop batcher uses
#include "utils/timing.h"  // timing::get_wall_time_ms()

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
    //   kPatchStreamMaxBuckets * 32 K verts * (sizeof(gos_VERTEX) + sizeof(gos_TERRAIN_EXTRA))
    //   = 64 * 32768 * (32 + 24) bytes ≈ 117 MB worst case if every
    //   bucket maxes out. Typical Wolfman: ~10 active buckets × ~24 K
    //   verts × 56 B ≈ 13 MB resident.
    for (auto& b : s_staging) {
        b.color.reserve(32 * 1024);
        b.extras.reserve(32 * 1024);
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

bool TerrainPatchStream::flush()        { return false; /* Task 6 */ }
