// GameOS/gameos/gos_terrain_patch_stream.cpp
#include "gos_terrain_patch_stream.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "gos_terrain_bridge.h"   // gos_terrain_bridge_* free functions (Task 0)
#include "../../RenderCore/terrain_path_telemetry.h"  // TERRAIN-PATH-TELEMETRY-1

#include <algorithm>  // std::sort (bucket sort/merge)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
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

    static const bool s_directBindOn =
        (getenv("MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND") != nullptr);
    static const bool s_directBindCheck =
        (getenv("MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND_CHECK") != nullptr);
    static bool s_directBindBannerSeen       = false;
    static bool s_directBindFirstDrawChecked = false;

    static const bool s_quadRecordsOn     = (getenv("MC2_PATCHSTREAM_QUAD_RECORDS")      != nullptr);
    static const bool s_quadRecordsDrawOn = (getenv("MC2_PATCHSTREAM_QUAD_RECORDS_DRAW") != nullptr);

    // Record SSBO — persistent-mapped, triple-buffered alongside color/extras VBOs.
    // Only allocated when s_quadRecordsOn. SSBO binding point 0.
    static GLuint    s_recordBuf        = 0;
    static void*     s_recordMap        = nullptr;
    static uint32_t  s_recordCount      = 0;  // records staged this frame
    static uint32_t  s_recordVertParity = 0;  // expected verts from records, for parity check
    static bool      s_recordBannerSeen = false;
    // CPU-side shadow of the record SSBO. appendQuadRecord writes here (cache-hot);
    // flush sorts from here then does one sequential memcpy to the GPU ring slot.
    // Not slot-indexed — written and consumed within the same frame.
    static TerrainQuadRecord s_recordShadow[kPatchStreamMaxRecordsPerSlot];

    static const bool s_thinRecordsOn     = (getenv("MC2_PATCHSTREAM_THIN_RECORDS")      != nullptr);
    static const bool s_thinRecordsDrawOn = (getenv("MC2_PATCHSTREAM_THIN_RECORDS_DRAW") != nullptr);
    static const bool s_fastPathOn        = (getenv("MC2_PATCHSTREAM_THIN_RECORD_FASTPATH") != nullptr);
    static const bool s_thinDebugOn       = (getenv("MC2_THIN_DEBUG") != nullptr);
    static bool       s_thinDebugPrinted  = false;

    // Recipe SSBO — single-buffered, persistent-mapped. Written once per new quad.
    // Not slot-indexed; recipeIdx is global across all ring slots.
    static GLuint    s_recipeBuf        = 0;
    static void*     s_recipeMap        = nullptr;
    static uint32_t  s_recipeCount      = 0;  // total recipes written (monotonic per mission)

    // CPU recipe index: key = packed float bits of (wx0, wy0), value = recipe slot.
    // Keyed by corner-0 world position (stable per terrain quad).
    static std::unordered_map<uint64_t, uint32_t> s_recipeIndex;

    // Thin-record SSBO — triple-buffered, persistent-mapped. Written per-frame.
    static GLuint    s_thinRecordBuf        = 0;
    static void*     s_thinRecordMap        = nullptr;
    static uint32_t  s_thinRecordCount      = 0;  // thin records staged this frame
    static uint32_t  s_thinRecordVertParity = 0;  // expected verts from thin records
    static bool      s_thinRecordBannerSeen = false;

    // CPU shadow of the thin-record SSBO (cache-hot staging; flushed sorted to SSBO).
    static TerrainQuadThinRecord s_thinRecordShadow[kPatchStreamMaxThinRecordsPerSlot];

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

static GLuint allocPersistentSSBO(GLsizeiptr totalBytes, void** outMappedPtr) {
    const GLbitfield flags = GL_MAP_WRITE_BIT
                           | GL_MAP_PERSISTENT_BIT
                           | GL_MAP_COHERENT_BIT;
    GLuint id = 0;
    glGenBuffers(1, &id);
    if (!id) return 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, id);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, totalBytes, nullptr, flags);
    if (glGetError() != GL_NO_ERROR) { glDeleteBuffers(1, &id); return 0; }
    void* p = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, totalBytes, flags);
    if (!p) { glDeleteBuffers(1, &id); return 0; }
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

    if (s_quadRecordsOn) {
        const GLsizeiptr recTotal =
            (GLsizeiptr)kPatchStreamRecordBytesPerSlot * kPatchStreamRingFrames;
        s_recordBuf = allocPersistentSSBO(recTotal, &s_recordMap);
        if (!s_recordBuf) {
            fprintf(stderr,
                "[PATCH_STREAM v1] event=record_ssbo_fail reason=alloc\n");
            fflush(stderr);
            // Non-fatal: record path disabled, expanded path continues.
        } else {
            fprintf(stderr,
                "[PATCH_STREAM v1] event=record_ssbo_ok bytes_per_slot=%u slots=%u\n",
                kPatchStreamRecordBytesPerSlot, kPatchStreamRingFrames);
            fflush(stderr);
        }
    }

    if (s_thinRecordsOn) {
        // Recipe SSBO — single-buffered, GL_MAP_WRITE_BIT | PERSISTENT | COHERENT.
        s_recipeBuf = allocPersistentSSBO((GLsizeiptr)kPatchStreamRecipeBytes, &s_recipeMap);
        if (!s_recipeBuf) {
            fprintf(stderr,
                "[PATCH_STREAM v1] event=recipe_ssbo_fail reason=alloc\n");
            fflush(stderr);
        } else {
            fprintf(stderr,
                "[PATCH_STREAM v1] event=recipe_ssbo_ok bytes=%u max_recipes=%u\n",
                kPatchStreamRecipeBytes, kPatchStreamMaxRecipesTotal);
            fflush(stderr);
        }

        // Thin-record SSBO — triple-buffered, same flags as fat-record SSBO.
        const GLsizeiptr thinTotal =
            (GLsizeiptr)kPatchStreamThinRecordBytesPerSlot * kPatchStreamRingFrames;
        s_thinRecordBuf = allocPersistentSSBO(thinTotal, &s_thinRecordMap);
        if (!s_thinRecordBuf) {
            fprintf(stderr,
                "[PATCH_STREAM v1] event=thin_record_ssbo_fail reason=alloc\n");
            fflush(stderr);
        } else {
            fprintf(stderr,
                "[PATCH_STREAM v1] event=thin_record_ssbo_ok bytes_per_slot=%u slots=%u\n",
                kPatchStreamThinRecordBytesPerSlot, kPatchStreamRingFrames);
            fflush(stderr);
        }
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0); // restore SSBO binding after allocPersistentSSBO
    restoreGLState(saved);

    if (!s_colorBuf || !s_extrasBuf) {
        // Init-fail path. Force killswitch off for the rest of the process.
        MC2_DIAGFE(stderr,
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
    MC2_DIAGFE(stderr,
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
    MC2_DIAGFE(stderr, "[PATCH_STREAM v1] event=shutdown\n");
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
    if (s_recordBuf) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_recordBuf);
        if (s_recordMap) { glUnmapBuffer(GL_SHADER_STORAGE_BUFFER); s_recordMap = nullptr; }
        glDeleteBuffers(1, &s_recordBuf);
        s_recordBuf = 0;
    }
    if (s_recipeBuf) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_recipeBuf);
        if (s_recipeMap) { glUnmapBuffer(GL_SHADER_STORAGE_BUFFER); s_recipeMap = nullptr; }
        glDeleteBuffers(1, &s_recipeBuf);
        s_recipeBuf = 0;
    }
    if (s_thinRecordBuf) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_thinRecordBuf);
        if (s_thinRecordMap) { glUnmapBuffer(GL_SHADER_STORAGE_BUFFER); s_thinRecordMap = nullptr; }
        glDeleteBuffers(1, &s_thinRecordBuf);
        s_thinRecordBuf = 0;
    }
    s_recipeIndex.clear();
    s_recipeCount = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    s_initOk = false;
}

bool TerrainPatchStream::isReady()      { return s_killswitch && s_initOk; }
bool TerrainPatchStream::isOverflowed() { return s_overflow; }

// TERRAIN-SPINE-0: read-only inspector accessors. Match the file-statics
// declared in the anonymous namespace at the top of this file.
uint32_t TerrainPatchStream::getLastFlushBucketCount()  { return s_drawBucketCount; }
uint32_t TerrainPatchStream::getLastFlushVertCount()    { return s_totalVerts; }
uint32_t TerrainPatchStream::getLastFlushThinRecCount() { return s_thinRecordCount; }
uint32_t TerrainPatchStream::getLastFlushRecipeCount()  { return s_recipeCount; }
bool     TerrainPatchStream::wasLastFlushOverflowed()   { return s_overflow; }
bool TerrainPatchStream::isThinRecordsActive() {
    return s_thinRecordsOn && (s_thinRecordBuf != 0);
}

bool TerrainPatchStream::isFastPathActive() {
    return s_fastPathOn &&
           s_thinRecordsOn &&
           s_thinRecordsDrawOn &&
           (s_thinRecordBuf != 0);
}

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
            MC2_DIAGFE(stderr,
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
    s_recordCount      = 0;
    s_recordVertParity = 0;
    s_thinRecordCount      = 0;
    s_thinRecordVertParity = 0;
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
        MC2_DIAGFE(stderr,
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

void TerrainPatchStream::appendQuad(
    DWORD terrainHandle,
    const gos_VERTEX* vColor1, const gos_TERRAIN_EXTRA* vExtra1, bool tri1Valid,
    const gos_VERTEX* vColor2, const gos_TERRAIN_EXTRA* vExtra2, bool tri2Valid)
{
    // PERF 2026-05-07: stripped hot Tracy zone PatchStream.AppendQuad.
    if (!s_initOk || !s_killswitch) return;
    if (s_overflow) return;
    // Thin-record path replaces expanded vertices — skip staging entirely.
    if (s_thinRecordsOn && s_thinRecordBuf) return;
    if (!tri1Valid && !tri2Valid) return;  // zero lookups when both clipped

    const uint32_t numVerts = (tri1Valid ? 3u : 0u) + (tri2Valid ? 3u : 0u);
    const uint32_t maxVertsThisSlot =
        kPatchStreamColorBytesPerSlot / (uint32_t)sizeof(gos_VERTEX);
    if (s_totalVerts + numVerts > maxVertsThisSlot) {
        MC2_DIAGFE(stderr,
            "[PATCH_STREAM v1] event=overflow slot=%u kind=byte_budget cursor=%u cap=%u\n",
            s_slot, s_totalVerts, maxVertsThisSlot);
        fflush(stderr);
        s_overflow = true;
        return;
    }

    PatchStagingBucket* bk = findOrCreateStagingBucket(terrainHandle);
    if (!bk) return;

    if (tri1Valid) {
        bk->color.insert(bk->color.end(), vColor1, vColor1 + 3);
        bk->extras.insert(bk->extras.end(), vExtra1, vExtra1 + 3);
    }
    if (tri2Valid) {
        bk->color.insert(bk->color.end(), vColor2, vColor2 + 3);
        bk->extras.insert(bk->extras.end(), vExtra2, vExtra2 + 3);
    }
    s_totalVerts += numVerts;
}

void TerrainPatchStream::appendQuadRecord(const TerrainQuadRecord& rec) {
    if (!s_quadRecordsOn || !s_recordBuf) return;
    if (!s_initOk || !s_killswitch) return;
    if (s_overflow) return;
    if (s_recordCount >= kPatchStreamMaxRecordsPerSlot) {
        if (s_recordCount == kPatchStreamMaxRecordsPerSlot) {
            MC2_DIAGFE(stderr,
                "[PATCH_STREAM v1] event=record_overflow slot=%u cap=%u\n",
                s_slot, kPatchStreamMaxRecordsPerSlot);
            fflush(stderr);
        }
        return;
    }
    s_recordShadow[s_recordCount] = rec;  // CPU-only; flushed to SSBO sorted at draw time
    ++s_recordCount;
}

void TerrainPatchStream::addRecordVertParity(uint32_t n) {
    if (!s_quadRecordsOn) return;
    s_recordVertParity += n;
}

uint32_t TerrainPatchStream::tryGetRecipeIdx(uint64_t key) {
    if (!s_thinRecordsOn || !s_thinRecordBuf || !s_recipeBuf) return UINT32_MAX;
    if (!s_initOk || !s_killswitch) return UINT32_MAX;
    auto it = s_recipeIndex.find(key);
    return (it != s_recipeIndex.end()) ? it->second : UINT32_MAX;
}

uint32_t TerrainPatchStream::ensureRecipeForQuad(uint64_t key,
                                                  const TerrainQuadRecipe& recipe) {
    if (!s_thinRecordsOn || !s_thinRecordBuf || !s_recipeBuf) return UINT32_MAX;
    if (!s_initOk || !s_killswitch) return UINT32_MAX;

    auto it = s_recipeIndex.find(key);
    if (it != s_recipeIndex.end()) {
        // Debug: if the cached recipe's positions differ, the key is non-unique.
        if (s_traceOn) {
            const TerrainQuadRecipe* base = (const TerrainQuadRecipe*)s_recipeMap;
            const TerrainQuadRecipe& cached = base[it->second];
            if (cached.wx0 != recipe.wx0 || cached.wy0 != recipe.wy0 ||
                cached.wz0 != recipe.wz0 || cached.wx1 != recipe.wx1) {
                fprintf(stderr,
                    "[PATCH_STREAM v1] event=recipe_key_collision slot=%u "
                    "key=0x%llx cached=(%.3f,%.3f) incoming=(%.3f,%.3f)\n",
                    it->second, (unsigned long long)key,
                    cached.wx0, cached.wy0, recipe.wx0, recipe.wy0);
                fflush(stderr);
            }
        }
        return it->second;
    }

    if (s_recipeCount >= kPatchStreamMaxRecipesTotal) {
        static bool s_recipeOverflowLogged = false;
        if (!s_recipeOverflowLogged) {
            s_recipeOverflowLogged = true;
            MC2_DIAGFE(stderr,
                "[PATCH_STREAM v1] event=recipe_overflow cap=%u\n",
                kPatchStreamMaxRecipesTotal);
            fflush(stderr);
        }
        return UINT32_MAX;
    }

    uint32_t slot = s_recipeCount++;
    TerrainQuadRecipe* recipeBase = static_cast<TerrainQuadRecipe*>(s_recipeMap);
    recipeBase[slot] = recipe;  // _wp0 carries packed terrainTypes
    s_recipeIndex[key] = slot;
    return slot;
}

void TerrainPatchStream::appendThinRecordDirect(const TerrainQuadThinRecord& tr) {
    if (!s_thinRecordsOn || !s_thinRecordBuf || !s_recipeBuf) return;
    if (!s_initOk || !s_killswitch) return;
    if (s_overflow) return;
    if (s_thinRecordCount >= kPatchStreamMaxThinRecordsPerSlot) {
        if (s_thinRecordCount == kPatchStreamMaxThinRecordsPerSlot) {
            MC2_DIAGFE(stderr,
                "[PATCH_STREAM v1] event=thin_record_overflow slot=%u cap=%u\n",
                s_slot, kPatchStreamMaxThinRecordsPerSlot);
            fflush(stderr);
        }
        return;
    }
    s_thinRecordShadow[s_thinRecordCount++] = tr;
    uint32_t pzTri1 = (tr.flags >> 1u) & 1u;
    uint32_t pzTri2 = (tr.flags >> 2u) & 1u;
    addThinRecordVertParity((pzTri1 ? 3u : 0u) + (pzTri2 ? 3u : 0u));
}

void TerrainPatchStream::appendThinRecord(
    DWORD terrainHandle,
    const TerrainQuadRecipe& recipe,
    uint32_t flags,
    uint32_t lightRGB0, uint32_t lightRGB1, uint32_t lightRGB2, uint32_t lightRGB3)
{
    if (!s_thinRecordsOn || !s_thinRecordBuf || !s_recipeBuf) return;
    if (!s_initOk || !s_killswitch) return;
    if (s_overflow) return;
    if (s_thinRecordCount >= kPatchStreamMaxThinRecordsPerSlot) {
        if (s_thinRecordCount == kPatchStreamMaxThinRecordsPerSlot) {
            MC2_DIAGFE(stderr,
                "[PATCH_STREAM v1] event=thin_record_overflow slot=%u cap=%u\n",
                s_slot, kPatchStreamMaxThinRecordsPerSlot);
            fflush(stderr);
        }
        return;
    }

    const uint64_t key = makeRecipeKey(recipe.wx0, recipe.wy0);
    uint32_t recipeSlot = ensureRecipeForQuad(key, recipe);
    if (recipeSlot == UINT32_MAX) return;

    // Write thin record to CPU shadow (cache-hot; sorted + uploaded in flush).
    // Parity is incremented by the caller's addThinRecordVertParity() call.
    TerrainQuadThinRecord& tr = s_thinRecordShadow[s_thinRecordCount++];
    tr.recipeIdx     = recipeSlot;
    tr.terrainHandle = static_cast<uint32_t>(terrainHandle);
    tr.flags         = flags;
    tr.cementWord    = 0u;  // Fix B rename: was _pad0; patch-stream path doesn't
                            // emit cement quads, so the field stays zero here.
    tr.lightRGB0     = lightRGB0;
    tr.lightRGB1     = lightRGB1;
    tr.lightRGB2     = lightRGB2;
    tr.lightRGB3     = lightRGB3;
    // Fix B 2026-05-14: clipPos[16] zeroed.  The patch-stream path uses a
    // separate VS (gos_terrain_patch.vert / its own shader), not the indirect
    // thin VS that reads clipPos.  Zero is the safe / degenerate value if any
    // future path picks up these records under the new VS.
    for (int k = 0; k < 16; ++k) tr.clipPos[k] = 0.0f;
}

void TerrainPatchStream::addThinRecordVertParity(uint32_t n) {
    if (!s_thinRecordsOn) return;
    s_thinRecordVertParity += n;
}

// TerrainPatchStream::flush() body deleted by TERRAIN-BRIDGE-BODY-DELETE-1.
// Sole caller (mclib/txmmgr.cpp) retired in PATCHSTREAM-THIN-RETIRE-1 (026e7276).
// Bridge helpers it called (beginBucketLoop, drawSingleBucket, endVertexDeclaration, end)
// are also deleted/decl-removed in this commit.
// Follow-on (later stage): delete append chain, init/destroy, getLastFlush* (snapshot), quad.cpp sites.
// TerrainPatchStream::flush() body removed (TERRAIN-BRIDGE-BODY-DELETE-1).
