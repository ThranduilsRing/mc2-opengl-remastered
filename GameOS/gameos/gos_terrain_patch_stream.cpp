// GameOS/gameos/gos_terrain_patch_stream.cpp
#include "gos_terrain_patch_stream.h"
#include "gos_terrain_bridge.h"   // gos_terrain_bridge_* free functions (Task 0)

#include <algorithm>  // std::sort (bucket sort/merge)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gameos.hpp"   // gos_VERTEX, gos_TERRAIN_EXTRA, DWORD, gos_SetRenderState, gos_State_*
#include "gl/glew.h"    // OpenGL — same include the static-prop batcher uses
#include "gos_profiler.h"
#include "utils/timing.h"  // timing::get_wall_time_ms()
#include "gos_postprocess.h"

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
    // O(1) lookup via open-addressing hash table (s_bucketHash) mapping
    // textureIndex → s_staging[] index. beginFrame() resets the table with
    // a single memset. kHashTableSize >= 2 × kPatchStreamMaxBuckets keeps
    // load factor < 0.5 and guarantees probe termination.
    struct PatchStagingBucket {
        DWORD                          textureIndex = 0;
        std::vector<gos_VERTEX>        color;
        std::vector<gos_TERRAIN_EXTRA> extras;
    };

    PatchStagingBucket s_staging[kPatchStreamMaxBuckets];
    uint32_t           s_stagingCount = 0;
    uint32_t           s_totalVerts   = 0;
    bool               s_overflow     = false;

    // Open-addressing hash table mapping textureIndex → s_staging[] index.
    // Power-of-2 size ≥ 2 × kPatchStreamMaxBuckets keeps load factor < 0.5,
    // guaranteeing probe termination. kHashEmpty is the vacant sentinel.
    constexpr uint32_t kHashTableSize = 1024u;
    constexpr uint32_t kHashEmpty     = 0xFFFFFFFFu;
    uint32_t           s_bucketHash[kHashTableSize];

    // Filled at flush() time from the staging buckets — this is what
    // issueDraws walks for per-bucket glDrawArrays. PatchStreamBucket
    // is declared in the header.
    PatchStreamBucket s_drawBuckets[kPatchStreamMaxBuckets];
    uint32_t          s_drawBucketCount = 0;

    // Telemetry
    bool s_firstFlushSeen = false;

    // ------------------------------------------------------------------
    // Bucket-census instrumentation (env-gated MC2_BUCKET_CENSUS=1).
    //
    // Computed once per flush() call, consumed by emitCensus() which runs
    // from txmmgr.cpp Render.TerrainSolid after the legacy-eligible count
    // is also known. Per-frame line is grep-friendly:
    //
    //   [BUCKET_CENSUS v1] frame=N raw=R unique=U sentinel=S canon=C legacy=L
    //
    // raw      — s_drawBucketCount (distinct raw `terrainHandle` values
    //            with at least one appended triangle this frame)
    // unique   — count of distinct tex_resolve(handle) values across
    //            those buckets (i.e., the size of an Option-B post-
    //            canonicalization bucket set)
    // sentinel — count of buckets where tex_resolve returns 0xFFFFFFFFu
    //            (unloaded / invalid texture nodes)
    // canon    — count of merged ranges if buckets were sorted by
    //            tex_resolve key and contiguous-same-key runs were
    //            coalesced (i.e., what Option A's draw count would be).
    //            For Option B (append-time resolve), this equals `unique`
    //            because every same-key bucket merges. For Option A
    //            applied to current append order, this can be HIGHER
    //            than `unique` since spatial traversal may interleave keys.
    // legacy   — number of masterVertexNodes that the legacy DRAWSOLID
    //            loop would have drawn this frame (filter:
    //            DRAWSOLID|ISTERRAIN flags, vertices != NULL,
    //            currentVertex > vertices). Computed in txmmgr.cpp.
    bool       s_censusOn         = false;
    uint64_t   s_censusFrameId    = 0;

    // Per-frame snapshot from flush(); read by emitCensus().
    uint32_t   s_lastCensusRaw      = 0;
    uint32_t   s_lastCensusUnique   = 0;
    uint32_t   s_lastCensusSentinel = 0;
    uint32_t   s_lastCensusCanon    = 0;

    // 600-frame rolling summary state. min initialized lazily on first
    // sample so it doesn't anchor at UINT32_MAX in the printout.
    bool       s_summaryHasSample = false;
    uint64_t   s_summaryFramesAcc = 0;       // frames in current window
    uint64_t   s_summaryRawSum    = 0;
    uint64_t   s_summaryUniqueSum = 0;
    uint64_t   s_summaryCanonSum  = 0;
    uint64_t   s_summaryLegacySum = 0;
    uint64_t   s_summarySentSum   = 0;
    uint32_t   s_summaryRawMin    = 0xFFFFFFFFu, s_summaryRawMax    = 0;
    uint32_t   s_summaryUniqueMin = 0xFFFFFFFFu, s_summaryUniqueMax = 0;
    uint32_t   s_summaryCanonMin  = 0xFFFFFFFFu, s_summaryCanonMax  = 0;
    uint32_t   s_summaryLegacyMin = 0xFFFFFFFFu, s_summaryLegacyMax = 0;
    // Cumulative (whole-run) stats for the shutdown summary.
    uint64_t   s_runFramesAcc = 0;
    uint64_t   s_runRawSum    = 0;
    uint64_t   s_runUniqueSum = 0;
    uint64_t   s_runCanonSum  = 0;
    uint64_t   s_runLegacySum = 0;
    uint64_t   s_runSentSum   = 0;
    uint32_t   s_runRawMax    = 0;
    uint32_t   s_runUniqueMax = 0;
    uint32_t   s_runCanonMax  = 0;
    uint32_t   s_runLegacyMax = 0;

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

    // O(1) hash table lookup over s_staging. Uses open-addressing with
    // Knuth multiplicative hash. kHashTableSize >= 2 × kPatchStreamMaxBuckets
    // keeps load factor < 0.5, guaranteeing probe termination.
    // Returns nullptr on overflow (bucket_count or hash_full).
    PatchStagingBucket* findOrCreateStagingBucket(DWORD textureIndex) {
        const uint32_t startSlot =
            (static_cast<uint32_t>(textureIndex) * 2654435761u) & (kHashTableSize - 1u);
        for (uint32_t probe = 0; probe < kHashTableSize; ++probe) {
            const uint32_t idx    = (startSlot + probe) & (kHashTableSize - 1u);
            const uint32_t stored = s_bucketHash[idx];
            if (stored == kHashEmpty) {
                if (s_stagingCount >= kPatchStreamMaxBuckets) {
                    fprintf(stderr,
                        "[PATCH_STREAM v1] event=overflow slot=%u kind=bucket_count "
                        "count=%u cap=%u\n",
                        s_slot, s_stagingCount, kPatchStreamMaxBuckets);
                    fflush(stderr);
                    s_overflow = true;
                    return nullptr;
                }
                s_bucketHash[idx] = s_stagingCount;
                PatchStagingBucket& nb = s_staging[s_stagingCount++];
                nb.textureIndex = textureIndex;
                return &nb;
            }
            if (s_staging[stored].textureIndex == textureIndex) {
                return &s_staging[stored];
            }
        }
        // Table exhausted without finding key — shouldn't happen if
        // kHashTableSize >= 2 × kPatchStreamMaxBuckets (load factor < 0.5).
        fprintf(stderr,
            "[PATCH_STREAM v1] event=overflow slot=%u kind=hash_full "
            "count=%u cap=%u\n",
            s_slot, s_stagingCount, kPatchStreamMaxBuckets);
        fflush(stderr);
        s_overflow = true;
        return nullptr;
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
    s_killswitch = (env == nullptr) || (env[0] != '0');
    s_traceOn    = (getenv("MC2_PATCH_STREAM_TRACE") != nullptr);
    s_censusOn   = (getenv("MC2_BUCKET_CENSUS") != nullptr);

    if (s_censusOn) {
        fprintf(stderr,
            "[BUCKET_CENSUS v1] event=startup gated_on=MC2_BUCKET_CENSUS "
            "killswitch=%d\n", (int)s_killswitch);
        fflush(stderr);
    }

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
    memset(s_bucketHash, 0xFF, sizeof(s_bucketHash));

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

void TerrainPatchStream::emitCensus(uint32_t legacyEligible)
{
    if (!s_censusOn) return;

    // When killswitch=0, flush() never runs, so modern stats are zero.
    // We still print a line so legacy_eligible can be tracked across
    // both killswitch states from the same instrumentation. Per-frame
    // line is gated by the env var, not by killswitch, intentionally.
    const uint32_t raw      = s_killswitch ? s_lastCensusRaw      : 0;
    const uint32_t unique   = s_killswitch ? s_lastCensusUnique   : 0;
    const uint32_t sentinel = s_killswitch ? s_lastCensusSentinel : 0;
    const uint32_t canon    = s_killswitch ? s_lastCensusCanon    : 0;

    fprintf(stderr,
        "[BUCKET_CENSUS v1] frame=%llu raw=%u unique=%u sentinel=%u "
        "canon_nosort=%u legacy=%u\n",
        (unsigned long long)s_censusFrameId,
        raw, unique, sentinel, canon, legacyEligible);
    fflush(stderr);

    // Update rolling 600-frame and run-cumulative stats.
    if (!s_summaryHasSample) {
        s_summaryHasSample = true;
        s_summaryRawMin    = raw;
        s_summaryUniqueMin = unique;
        s_summaryCanonMin  = canon;
        s_summaryLegacyMin = legacyEligible;
    } else {
        if (raw            < s_summaryRawMin)    s_summaryRawMin    = raw;
        if (unique         < s_summaryUniqueMin) s_summaryUniqueMin = unique;
        if (canon          < s_summaryCanonMin)  s_summaryCanonMin  = canon;
        if (legacyEligible < s_summaryLegacyMin) s_summaryLegacyMin = legacyEligible;
    }
    if (raw            > s_summaryRawMax)    s_summaryRawMax    = raw;
    if (unique         > s_summaryUniqueMax) s_summaryUniqueMax = unique;
    if (canon          > s_summaryCanonMax)  s_summaryCanonMax  = canon;
    if (legacyEligible > s_summaryLegacyMax) s_summaryLegacyMax = legacyEligible;

    s_summaryRawSum    += raw;
    s_summaryUniqueSum += unique;
    s_summaryCanonSum  += canon;
    s_summaryLegacySum += legacyEligible;
    s_summarySentSum   += sentinel;
    s_summaryFramesAcc += 1;

    s_runFramesAcc += 1;
    s_runRawSum    += raw;
    s_runUniqueSum += unique;
    s_runCanonSum  += canon;
    s_runLegacySum += legacyEligible;
    s_runSentSum   += sentinel;
    if (raw            > s_runRawMax)    s_runRawMax    = raw;
    if (unique         > s_runUniqueMax) s_runUniqueMax = unique;
    if (canon          > s_runCanonMax)  s_runCanonMax  = canon;
    if (legacyEligible > s_runLegacyMax) s_runLegacyMax = legacyEligible;

    s_censusFrameId += 1;

    // 600-frame rolling summary tick.
    if (s_summaryFramesAcc >= 600u) {
        const double inv = 1.0 / (double)s_summaryFramesAcc;
        fprintf(stderr,
            "[BUCKET_CENSUS v1] event=summary kind=window frames=%llu "
            "raw_min=%u raw_avg=%.1f raw_max=%u "
            "unique_min=%u unique_avg=%.1f unique_max=%u "
            "canon_min=%u canon_avg=%.1f canon_max=%u "
            "legacy_min=%u legacy_avg=%.1f legacy_max=%u "
            "sentinel_avg=%.2f\n",
            (unsigned long long)s_summaryFramesAcc,
            s_summaryRawMin,    (double)s_summaryRawSum    * inv, s_summaryRawMax,
            s_summaryUniqueMin, (double)s_summaryUniqueSum * inv, s_summaryUniqueMax,
            s_summaryCanonMin,  (double)s_summaryCanonSum  * inv, s_summaryCanonMax,
            s_summaryLegacyMin, (double)s_summaryLegacySum * inv, s_summaryLegacyMax,
            (double)s_summarySentSum * inv);
        fflush(stderr);
        // Reset window counters but KEEP run-cumulative.
        s_summaryHasSample = false;
        s_summaryFramesAcc = 0;
        s_summaryRawSum = s_summaryUniqueSum = s_summaryCanonSum = 0;
        s_summaryLegacySum = s_summarySentSum = 0;
        s_summaryRawMin = s_summaryUniqueMin = s_summaryCanonMin =
            s_summaryLegacyMin = 0xFFFFFFFFu;
        s_summaryRawMax = s_summaryUniqueMax = s_summaryCanonMax =
            s_summaryLegacyMax = 0;
    }
}

void TerrainPatchStream::destroy()
{
    if (s_censusOn && s_runFramesAcc > 0) {
        const double inv = 1.0 / (double)s_runFramesAcc;
        fprintf(stderr,
            "[BUCKET_CENSUS v1] event=summary kind=run frames=%llu "
            "raw_avg=%.1f raw_max=%u "
            "unique_avg=%.1f unique_max=%u "
            "canon_avg=%.1f canon_max=%u "
            "legacy_avg=%.1f legacy_max=%u "
            "sentinel_avg=%.2f\n",
            (unsigned long long)s_runFramesAcc,
            (double)s_runRawSum    * inv, s_runRawMax,
            (double)s_runUniqueSum * inv, s_runUniqueMax,
            (double)s_runCanonSum  * inv, s_runCanonMax,
            (double)s_runLegacySum * inv, s_runLegacyMax,
            (double)s_runSentSum   * inv);
        fflush(stderr);
    }

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
    // allocator churn after warmup. s_stagingCount goes to 0 and
    // s_bucketHash is memset to 0xFF (kHashEmpty) so the hash table
    // starts fresh each frame with no stale entries.
    for (uint32_t i = 0; i < s_stagingCount; ++i) {
        s_staging[i].color.clear();
        s_staging[i].extras.clear();
    }
    s_stagingCount    = 0;
    s_totalVerts      = 0;
    s_drawBucketCount = 0;
    s_overflow        = false;
    memset(s_bucketHash, 0xFF, sizeof(s_bucketHash));
}

void TerrainPatchStream::appendTriangle(DWORD textureIndex,
                                        const gos_VERTEX* vColor,
                                        const gos_TERRAIN_EXTRA* vExtra)
{
    ZoneScopedN("PatchStream.AppendTriangle");
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

    PatchStagingBucket* bk = nullptr;
    {
    ZoneScopedN("PatchStream.Append.LookupBucket");
    bk = findOrCreateStagingBucket(textureIndex);
    }
    if (!bk) return;  // overflow already logged

    {
    ZoneScopedN("PatchStream.Append.InsertColor");
    bk->color.insert(bk->color.end(),  vColor, vColor + vertsPerTri);
    }
    {
    ZoneScopedN("PatchStream.Append.InsertExtras");
    bk->extras.insert(bk->extras.end(), vExtra, vExtra + vertsPerTri);
    }
    s_totalVerts += vertsPerTri;
}

bool TerrainPatchStream::flush()
{
    ZoneScopedN("PatchStream.Flush");
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

    {
    ZoneScopedN("PatchStream.MemoryBarrier");
    glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
    }

    // --- Census snapshot from raw staging (before any sort/merge) ---
    // Must run before sort so raw/unique/canonNoSort measure the unsorted
    // staging array, not the post-merge draw buckets.
    if (s_censusOn) {
        const uint32_t N = s_stagingCount;
        DWORD resolved[kPatchStreamMaxBuckets];
        uint32_t sentinelCount = 0;
        for (uint32_t b = 0; b < N; ++b) {
            const DWORD r = tex_resolve(s_staging[b].textureIndex);
            resolved[b] = r;
            if (r == 0xFFFFFFFFu) ++sentinelCount;
        }
        uint32_t unique = 0;
        for (uint32_t b = 0; b < N; ++b) {
            bool seen = false;
            for (uint32_t k = 0; k < b; ++k) {
                if (resolved[k] == resolved[b]) { seen = true; break; }
            }
            if (!seen) ++unique;
        }
        uint32_t canonNoSort = (N > 0) ? 1u : 0u;
        for (uint32_t b = 1; b < N; ++b) {
            if (resolved[b] != resolved[b - 1]) ++canonNoSort;
        }
        s_lastCensusRaw      = N;
        s_lastCensusUnique   = unique;
        s_lastCensusSentinel = sentinelCount;
        s_lastCensusCanon    = canonNoSort;
    }

    // --- Sort staging by resolved texture handle ---
    // Sorting groups same-texture staging buckets together so the merge step
    // can coalesce them into one contiguous ring range (one draw call).
    // tie-break on stagingIdx preserves append order within same texture for
    // deterministic output.
    struct BucketSortEntry { DWORD gosHandle; uint32_t stagingIdx; };
    BucketSortEntry sortBuf[kPatchStreamMaxBuckets];
    {
    ZoneScopedN("PatchStream.BucketSort");
    for (uint32_t i = 0; i < s_stagingCount; ++i) {
        sortBuf[i] = { tex_resolve(s_staging[i].textureIndex), i };
    }
    std::sort(sortBuf, sortBuf + s_stagingCount,
        [](const BucketSortEntry& a, const BucketSortEntry& b) {
            if (a.gosHandle != b.gosHandle) return a.gosHandle < b.gosHandle;
            return a.stagingIdx < b.stagingIdx;
        });
    }

    // --- Consolidate sorted staging into persistent ring, merging same-texture ranges ---
    uint32_t cursor = 0;
    s_drawBucketCount = 0;
    {
    ZoneScopedN("PatchStream.Consolidate");
    for (uint32_t i = 0; i < s_stagingCount; ++i) {
        const BucketSortEntry&    se = sortBuf[i];
        const PatchStagingBucket& sb = s_staging[se.stagingIdx];
        if (sb.color.empty()) continue;
        const uint32_t n = (uint32_t)sb.color.size();  // == sb.extras.size()

        {
        ZoneScopedN("PatchStream.Consolidate.CopyColor");
        memcpy(colorSlot  + cursor, sb.color.data(),  n * sizeof(gos_VERTEX));
        }
        {
        ZoneScopedN("PatchStream.Consolidate.CopyExtras");
        memcpy(extrasSlot + cursor, sb.extras.data(), n * sizeof(gos_TERRAIN_EXTRA));
        }

        // Merge with previous draw bucket if resolved handle matches.
        // Data is contiguous in the ring so [firstVertex..firstVertex+vertexCount)
        // covers the full merged range correctly.
        if (s_drawBucketCount > 0 &&
            s_drawBuckets[s_drawBucketCount - 1].gosHandle == se.gosHandle) {
            s_drawBuckets[s_drawBucketCount - 1].vertexCount += n;
        } else {
            PatchStreamBucket& db = s_drawBuckets[s_drawBucketCount++];
            db.gosHandle   = se.gosHandle;
            db.firstVertex = cursor;  // slot-relative; slotFirstVert added at draw time
            db.vertexCount = n;
        }
        cursor += n;
    }
    }
    TracyPlot("PatchStream verts", (int64_t)cursor);
    TracyPlot("PatchStream buckets", (int64_t)s_drawBucketCount);

    // 2. Bind uniforms via the engine bridge — sets the program, samplers,
    //    terrainMVP GL_FALSE, all tess + splatting + shadow uniforms.
    //    apply() inside the bridge calls glUseProgram, after which the
    //    direct glUniform* calls land on the right program (AMD rule).
    gosRenderMaterial* mat = gos_terrain_bridge_getMaterial();
    if (!mat) {
        // No terrain material — abort modern path. Caller falls back to legacy.
        restoreGLState(saved);
        return false;
    }
    {
    ZoneScopedN("PatchStream.BindUniforms");
    gos_terrain_bridge_bindUniforms(mat);
    }

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
    static int s_bucketErrFramesChecked = 0;
    const bool checkBucketErrors = s_traceOn && s_bucketErrFramesChecked < 2;
    {
    ZoneScopedN("PatchStream.DrawBuckets");
    for (uint32_t b = 0; b < s_drawBucketCount; ++b) {
        const PatchStreamBucket& bk = s_drawBuckets[b];
        const DWORD gosHandle = bk.gosHandle;  // tex_resolve already applied at consolidate time
        const GLuint glTex =
            (GLuint)gos_terrain_bridge_glTextureForGosHandle((unsigned int)gosHandle);

        // One-shot pre-draw state dump so we can see what GL thinks the
        // moment GL_INVALID_OPERATION fires. Fires once per process when
        // MC2_PATCH_STREAM_TRACE=1.
        static bool s_predrawOnce = false;
        if (!s_predrawOnce && s_traceOn) {
            s_predrawOnce = true;
            GLint program = 0, vao = 0, arrayBuf = 0, elemBuf = 0;
            GLint patchVerts = 0, maxPatchVerts = 0;
            GLint activeTex = 0, tex0 = 0;
            GLint drawFbo = 0, readFbo = 0;

            glGetIntegerv(GL_CURRENT_PROGRAM,              &program);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING,         &vao);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING,         &arrayBuf);
            glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elemBuf);
            glGetIntegerv(GL_PATCH_VERTICES,               &patchVerts);
            glGetIntegerv(GL_MAX_PATCH_VERTICES,           &maxPatchVerts);
            glGetIntegerv(GL_ACTIVE_TEXTURE,               &activeTex);
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D,           &tex0);
            glActiveTexture((GLenum)activeTex);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,     &drawFbo);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,     &readFbo);

            fprintf(stderr,
                "[PATCH_STREAM v1] event=predraw_state prog=%d vao=%d "
                "arrayBuf=%d elemBuf=%d patchVerts=%d maxPatchVerts=%d "
                "activeTex=0x%X tex0=%d drawFbo=%d readFbo=%d "
                "first=%d count=%d expected_colorBuf=%u expected_extrasBuf=%u\n",
                program, vao, arrayBuf, elemBuf, patchVerts, maxPatchVerts,
                (unsigned)activeTex, tex0, drawFbo, readFbo,
                (int)(slotFirstVert + bk.firstVertex),
                (int)bk.vertexCount,
                (unsigned)s_colorBuf, (unsigned)s_extrasBuf);

            // Per-attrib state for locs 0..5.
            for (int a = 0; a < 6; ++a) {
                GLint enabled = 0, size = 0, type = 0, stride = 0,
                      normalized = 0, buffer = 0;
                void* ptr = nullptr;
                glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
                glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
                glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
                glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
                glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &normalized);
                glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
                glGetVertexAttribPointerv(a, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
                fprintf(stderr,
                    "[PATCH_STREAM v1] event=predraw_attrib loc=%d enabled=%d "
                    "size=%d type=0x%X stride=%d norm=%d buf=%d offset=%lld\n",
                    a, enabled, size, (unsigned)type, stride, normalized,
                    buffer, (long long)(intptr_t)ptr);
            }

            // Drain any pending GL error so the next glDrawArrays gives us
            // clean attribution.
            GLenum preErr;
            while ((preErr = glGetError()) != GL_NO_ERROR) {
                fprintf(stderr,
                    "[PATCH_STREAM v1] event=predraw_pending_err err=0x%X\n",
                    (unsigned)preErr);
            }
            fflush(stderr);
        }

        gos_terrain_bridge_drawPatchStreamBucket(
            (unsigned int)gosHandle,
            slotFirstVert + bk.firstVertex,
            bk.vertexCount);

        // Per-bucket post-draw error capture. Rate-limited to first 8
        // erroring buckets per process so we can attribute the bug.
        if (checkBucketErrors) {
            static int s_errLogged = 0;
            if (s_errLogged < 8) {
                GLenum drawErr = glGetError();
                if (drawErr != GL_NO_ERROR) {
                    s_errLogged++;
                    GLint texBind = 0;
                    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texBind);
                    fprintf(stderr,
                        "[PATCH_STREAM v1] event=bucket_err err=0x%X "
                        "bucket_idx=%u/%u gosHandle=%lu glTex=%u "
                        "tex0_bound=%d firstVertex=%u vertexCount=%u\n",
                        (unsigned)drawErr, b, s_drawBucketCount,
                        (unsigned long)bk.gosHandle, (unsigned)glTex,
                        texBind, bk.firstVertex, bk.vertexCount);
                    fflush(stderr);
                }
            }
        }
    }
    if (s_traceOn && s_bucketErrFramesChecked < 2) {
        ++s_bucketErrFramesChecked;
    }
    }

    {
    ZoneScopedN("PatchStream.Cleanup");
    if (locWorldPos  >= 0) glDisableVertexAttribArray(locWorldPos);
    if (locWorldNorm >= 0) glDisableVertexAttribArray(locWorldNorm);
    gos_terrain_bridge_endVertexDeclaration(mat);
    gos_terrain_bridge_end(mat);
    }
    if (gosPostProcess* pp = getGosPostProcess()) {
        pp->markTerrainDrawn();
    }

    // 7. Fence the slot.
    {
    ZoneScopedN("PatchStream.FenceRestore");
    if (s_fence[s_slot]) glDeleteSync(s_fence[s_slot]);
    s_fence[s_slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    restoreGLState(saved);
    }

    if (!s_firstFlushSeen) {
        s_firstFlushSeen = true;
        fprintf(stderr,
            "[PATCH_STREAM v1] event=first_flush slot=%u verts=%u buckets=%u\n",
            s_slot, cursor, s_drawBucketCount);
        fflush(stderr);
    }
    if (s_traceOn) {
        static int s_drawCountLogged = 0;
        if (s_drawCountLogged++ < 16) {
        fprintf(stderr,
            "[PATCH_STREAM v1] event=draw_count slot=%u verts=%u buckets=%u\n",
            s_slot, cursor, s_drawBucketCount);
        fflush(stderr);
        }
    }

    return true;
}
