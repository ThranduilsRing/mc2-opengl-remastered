#include "gpu_cull_substrate.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "gpu_cull_parity.h"
#include <GL/glew.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "gos_profiler.h"        // Tracy ZoneScopedN
#include "diagnostic_trace.h"
#include "Gates/gate.h"

// MC2_GPU_CULL_SUBSTRATE_TRACE=1 — verbose per-frame lifecycle prints.
// Default off; the 600-frame summary + first-flush banner emit unconditionally.
static const bool s_substrateTrace = mc2::gate::get<"MC2_GPU_CULL_SUBSTRATE_TRACE">();
#define SUBSTRATE_TRACE(fmt, ...) \
    do { if (s_substrateTrace) { MC2_DIAGF("[GPU_CULL v1] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while (0)

namespace gpu_cull {

// ---------------------------------------------------------------------------
// Ring constants
// ---------------------------------------------------------------------------

constexpr uint32_t RING_FRAMES             = 3;
// Binding point 8: 0-3 are static-prop, 5-6 are water. 4 and 7 are unused.
// 8 is the first binding clearly clear of all existing paths.
constexpr uint32_t SUBSTRATE_SSBO_BINDING  = 8;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static GLuint    s_instanceSsbo   = 0;
static void*     s_mappedPtr      = nullptr;
static GLsync    s_fence[RING_FRAMES] = {0, 0, 0};
static uint32_t  s_frameSlot      = 0;

static uint32_t  s_maxActors      = 0;   // set at init, immutable after
static uint32_t  s_perFrameCount  = 0;   // records submitted this frame
static bool      s_initialized    = false;
// 2026-05-11: once-per-frame latch for overflow log. The s_perFrameCount==cap
// check alone doesn't gate repeats: the overflow branch returns before
// incrementing, so every excess submit re-fires the print. Pause cycles
// hammer this with hundreds of submits → console lag → input stuck. Latch
// resets in substrate_frameBegin().
static bool      s_overflowLoggedThisFrame = false;

// 600-frame summary counter.
static uint32_t  s_flushCount     = 0;
static bool      s_firstFlushDone = false;

// C1a: CPU-visible count from last flushUpload() — count of prevVisibilityBit==1 records.
static uint32_t  s_cpuVisibleCount = 0;

// Byte size of one ring slot: header + record array.
static size_t    s_slotBytes      = 0;

// PERF-GPU-CULL-READBACK-ID-CACHE-1: CPU-side actor ID cache.
// Parallel to GPU SSBO records: cpuActorIds[slot][i] == records[slot][i].actorId.
// Avoids PCIe/BAR cache misses in readback_buildActorVisSnapshot.
static uint32_t* s_cpuActorIds[RING_FRAMES]  = {nullptr, nullptr, nullptr};
static uint32_t  s_cpuSlotCount[RING_FRAMES] = {0u, 0u, 0u};

// ---------------------------------------------------------------------------
// M1 FROZEN-STATIC-CULL-RECORDS (gate MC2_GPU_CULL_STATIC_FROZEN_RECORDS).
// CPU golden copy of the static-prop cull records, built on registry-generation
// dirty in persistent-pool order (so record-index == instance-pool slot). Frozen
// between dirty events; memcpy'd into each ring slot lazily on its next
// frameBegin (per-slot dirty), so the static prefix [0,S) is RESIDENT before
// dynamic actors append [S,S+D). This decouples static record PRODUCTION from
// the per-frame timeline (the dynamic-first append-order constraint). When the
// gate is off, OR before the registry has populated the prefix (S==0), frameBegin
// behaves exactly as before (cursor resets to 0) — additive + byte-identical.
// REQUIRES the registry-side step (skip the per-frame static append + call
// substrate_rebuildStaticPrefix on dirty) before the gate may be turned on;
// otherwise statics would be double-counted (prefix + per-frame append).
static const bool s_staticFrozen = mc2::gate::get<"MC2_GPU_CULL_STATIC_FROZEN_RECORDS">();
static GpuActorRecord* s_staticRecords            = nullptr;  // golden copy, sized maxActors
static uint32_t        s_staticPrefixCount        = 0u;       // S
static uint32_t        s_staticPrefixVisibleCount = 0u;       // # prevVisibilityBit==1 in [0,S)
static uint32_t        s_staticDirty[RING_FRAMES] = {0u, 0u, 0u};  // per-slot: refill needed

// ---------------------------------------------------------------------------
// Env probe (lazy, cached)
// ---------------------------------------------------------------------------

static bool substrate_isEnabled_impl() {
    return mc2::gate::get<"MC2_GPU_CULL_SUBSTRATE">();
}

bool substrate_isEnabled() {
    static bool s_enabled = substrate_isEnabled_impl();
    return s_enabled;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

GLuint substrate_getInstanceSsboName() {
    return s_initialized ? s_instanceSsbo : 0;
}

uint32_t substrate_getInstanceSsboBindingPoint() {
    return SUBSTRATE_SSBO_BINDING;
}

uint32_t substrate_getCpuVisibleCount() {
    return s_cpuVisibleCount;
}

GLintptr substrate_getCurrentSlotOffset() {
    if (!s_initialized) return 0;
    return static_cast<GLintptr>(s_frameSlot * s_slotBytes);
}

GLsizeiptr substrate_getSlotBytes() {
    if (!s_initialized) return 0;
    return static_cast<GLsizeiptr>(s_slotBytes);
}

// ---------------------------------------------------------------------------
// substrate_init
// ---------------------------------------------------------------------------

void substrate_init(uint32_t maxActors) {
    if (!substrate_isEnabled()) return;
    if (s_initialized) {
        // Reinit after a prior mission — shut down first.
        substrate_shutdown();
    }

    s_maxActors      = maxActors;
    s_perFrameCount  = 0;
    s_cpuVisibleCount = 0;
    s_frameSlot      = 0;
    s_flushCount     = 0;
    s_firstFlushDone = false;
    s_staticPrefixCount        = 0u;   // M1: no frozen prefix until the registry rebuilds it
    s_staticPrefixVisibleCount = 0u;
    for (uint32_t i = 0u; i < RING_FRAMES; ++i) s_staticDirty[i] = 0u;

    // Per-slot layout: [GpuActorRecordHeader][GpuActorRecord * maxActors]
    s_slotBytes = sizeof(GpuActorRecordHeader) + maxActors * sizeof(GpuActorRecord);
    const GLsizeiptr totalBytes = static_cast<GLsizeiptr>(RING_FRAMES * s_slotBytes);

    const GLbitfield storageFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    const GLbitfield mapFlags     = storageFlags;

    // TIER2-EXCLUDED: substrate-gated
    glGenBuffers(1, &s_instanceSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_instanceSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, totalBytes, nullptr, storageFlags);
    s_mappedPtr = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, totalBytes, mapFlags);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (!s_mappedPtr) {
        MC2_DIAGFE(stderr, "[GPU_CULL v1] FATAL: persistent map failed\n");
        fflush(stderr);
        abort();
    }

    // Zero the entire buffer so unwritten records read as zero.
    memset(s_mappedPtr, 0, static_cast<size_t>(totalBytes));

    // Allocate CPU actor ID cache — plain system memory, one slot per ring frame.
    for (uint32_t i = 0u; i < RING_FRAMES; ++i) {
        s_cpuActorIds[i]  = new uint32_t[maxActors]();
        s_cpuSlotCount[i] = 0u;
    }
    // M1: golden copy for the frozen static-prop record prefix (sized maxActors).
    s_staticRecords = new GpuActorRecord[maxActors]();

    s_initialized = true;

    MC2_DIAGF("[GPU_CULL v1] event=substrate_init maxActors=%u slotBytes=%zu totalBytes=%lld binding=%u\n",
           maxActors, s_slotBytes, (long long)totalBytes, SUBSTRATE_SSBO_BINDING);
    fflush(stdout);
    if (mc2_diag::tagEnabled("GPU_CULL")) {
        char diag_buf[256];
        snprintf(diag_buf, sizeof(diag_buf),
            "{\"event\":\"substrate_init\",\"maxActors\":%u,\"slotBytes\":%zu,"
            "\"totalBytes\":%lld,\"binding\":%u}",
            maxActors, s_slotBytes, (long long)totalBytes, SUBSTRATE_SSBO_BINDING);
        mc2_diag::writeEvent("GPU_CULL", 1, 0, diag_buf);
    }
}

// ---------------------------------------------------------------------------
// substrate_shutdown
// ---------------------------------------------------------------------------

void substrate_shutdown() {
    if (!substrate_isEnabled()) return;

    // Wait for all in-flight slots before releasing.
    for (uint32_t i = 0; i < RING_FRAMES; ++i) {
        if (s_fence[i]) {
            glClientWaitSync(s_fence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_fence[i]);
            s_fence[i] = 0;
        }
    }

    if (s_instanceSsbo) {
        if (s_mappedPtr) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_instanceSsbo);
            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            s_mappedPtr = nullptr;
        }
        glDeleteBuffers(1, &s_instanceSsbo);
        s_instanceSsbo = 0;
    }
    // Free CPU actor ID cache.
    for (uint32_t i = 0u; i < RING_FRAMES; ++i) {
        delete[] s_cpuActorIds[i];
        s_cpuActorIds[i]  = nullptr;
        s_cpuSlotCount[i] = 0u;
    }
    // M1: free the frozen static-prop golden copy.
    delete[] s_staticRecords;
    s_staticRecords            = nullptr;
    s_staticPrefixCount        = 0u;
    s_staticPrefixVisibleCount = 0u;
    for (uint32_t i = 0u; i < RING_FRAMES; ++i) s_staticDirty[i] = 0u;

    s_mappedPtr      = nullptr;
    s_maxActors      = 0;
    s_perFrameCount  = 0;
    s_cpuVisibleCount = 0;
    s_frameSlot      = 0;
    s_slotBytes      = 0;
    s_initialized    = false;
    s_firstFlushDone = false;

    MC2_DIAGF("[GPU_CULL v1] event=substrate_shutdown\n");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// substrate_frameBegin
// ---------------------------------------------------------------------------

void substrate_frameBegin() {
    if (!substrate_isEnabled() || !s_initialized) return;

    // Advance ring slot.
    s_frameSlot = (s_frameSlot + 1) % RING_FRAMES;

    // Wait on this slot's fence if it has not yet been signalled.
    // At 60 Hz with 3-deep ring this should never stall, but be defensive.
    if (s_fence[s_frameSlot]) {
        ZoneScopedN("SubFrameBegin.RingWait");
        glClientWaitSync(s_fence[s_frameSlot], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(s_fence[s_frameSlot]);
        s_fence[s_frameSlot] = 0;
    }

    // Reset the per-frame record count for this slot.
    if (s_staticFrozen && s_staticPrefixCount > 0u) {
        // M1: static records are a FROZEN prefix [0,S). Refill THIS ring slot
        // from the CPU golden copy if it is dirty (per-slot, lazy, AFTER the
        // fence wait above so the GPU is done reading this slot), then start the
        // per-frame cursor AFTER the prefix so dynamic actors append into
        // [S,S+D). No per-frame static append (the registry skips it under the
        // gate). When S==0 or the gate is off, the else-branch keeps the
        // original reset-to-0 behavior — additive + byte-identical.
        const uint32_t S = s_staticPrefixCount;
        if (s_staticDirty[s_frameSlot]) {
            const size_t slotOffset = s_frameSlot * s_slotBytes;
            char* recBase = static_cast<char*>(s_mappedPtr) + slotOffset
                            + sizeof(GpuActorRecordHeader);
            memcpy(recBase, s_staticRecords,
                   static_cast<size_t>(S) * sizeof(GpuActorRecord));
            for (uint32_t k = 0u; k < S; ++k)
                s_cpuActorIds[s_frameSlot][k] = s_staticRecords[k].actorId;
            s_staticDirty[s_frameSlot] = 0u;
        }
        s_perFrameCount             = S;
        s_cpuVisibleCount           = s_staticPrefixVisibleCount;
        s_cpuSlotCount[s_frameSlot] = S;
    } else {
        s_perFrameCount = 0;
        s_cpuVisibleCount = 0;
        s_cpuSlotCount[s_frameSlot] = 0u;
    }
    s_overflowLoggedThisFrame = false;  // 2026-05-11 reset overflow log latch

    SUBSTRATE_TRACE("event=frame_begin slot=%u", s_frameSlot);
}

// ---------------------------------------------------------------------------
// substrate_rebuildStaticPrefix  (M1 FROZEN-STATIC-CULL-RECORDS)
// ---------------------------------------------------------------------------
//
// Installs the frozen static-prop cull-record prefix [0,S) into the CPU golden
// copy, in persistent-pool order (so record-index == instance-pool slot). Call
// on a registry-generation dirty event (the static set or a cull-relevant field
// changed). Marks all ring slots dirty; each slot is memcpy'd from the golden
// copy into its mapped record array lazily at its next substrate_frameBegin().
// Maintained regardless of the gate; only frameBegin's consumption is gated.
void substrate_rebuildStaticPrefix(const GpuActorRecord* recs, uint32_t count) {
    if (!substrate_isEnabled() || !s_initialized || !s_staticRecords) return;
    if (count > s_maxActors) count = s_maxActors;  // clamp; shortfall surfaces as cull overflow
    uint32_t vis = 0u;
    for (uint32_t k = 0u; k < count; ++k) {
        s_staticRecords[k] = recs[k];
        if (recs[k].prevVisibilityBit) ++vis;
    }
    s_staticPrefixCount        = count;
    s_staticPrefixVisibleCount = vis;
    for (uint32_t s = 0u; s < RING_FRAMES; ++s) s_staticDirty[s] = 1u;
    SUBSTRATE_TRACE("event=rebuild_static_prefix count=%u vis=%u", count, vis);
}

// ---------------------------------------------------------------------------
// substrate_writeRecord  (shared lockstep write core)
// ---------------------------------------------------------------------------
//
// Writes one record into the current ring slot's array and maintains the two
// lockstep per-frame counters (s_perFrameCount, s_cpuVisibleCount).
// PRECONDITION: caller has passed the s_perFrameCount >= s_maxActors overflow
// guard and verified s_mappedPtr != nullptr. The cpuVisible increment is
// computed from the cache-hot `rec` parameter, never by reading back from the
// write-combined mapped buffer; it occurs AFTER the successful memcpy so the
// accumulator counts exactly records actually written into
// recs[0..s_perFrameCount). Replaces the deleted flush-side readback loop.
// Single-threaded: no atomics; inherits the existing s_perFrameCount
// externally-serialized-producer contract.

static inline void substrate_writeRecord(const GpuActorRecord& rec) {
    const size_t slotOffset = s_frameSlot * s_slotBytes;
    char* dest = static_cast<char*>(s_mappedPtr)
                 + slotOffset
                 + sizeof(GpuActorRecordHeader)
                 + s_perFrameCount * sizeof(GpuActorRecord);
    memcpy(dest, &rec, sizeof(GpuActorRecord));
    // Cache actor ID in CPU memory so readback_buildActorVisSnapshot avoids
    // reading recs[i].actorId from GPU-mapped (BAR/PCIe) memory.
    s_cpuActorIds[s_frameSlot][s_perFrameCount] = rec.actorId;
    if (rec.prevVisibilityBit) ++s_cpuVisibleCount;
    ++s_perFrameCount;
}

// ---------------------------------------------------------------------------
// substrate_submitDynamicActor
// ---------------------------------------------------------------------------

void substrate_submitDynamicActor(const GpuActorRecord& rec) {
    if (!substrate_isEnabled() || !s_initialized) return;
    if (!s_mappedPtr) return;

    if (s_perFrameCount >= s_maxActors) {
        // Overflow: clamp and log once per frame. Same latch fix as
        // substrate_appendStaticPropRecord (2026-05-11).
        if (!s_overflowLoggedThisFrame) {
            s_overflowLoggedThisFrame = true;
            MC2_DIAGF("[GPU_CULL v1] event=substrate_overflow at=%u cap=%u\n",
                   s_perFrameCount, s_maxActors);
            fflush(stdout);
        }
        return;
    }

    substrate_writeRecord(rec);
}

// ---------------------------------------------------------------------------
// substrate_flushUpload
// ---------------------------------------------------------------------------

void substrate_flushUpload() {
    if (!substrate_isEnabled() || !s_initialized) return;

    // Write the header for this slot. (s_cpuVisibleCount is now accumulated
    // at record-write time in substrate_writeRecord; the old flush-side
    // readback count-loop is deleted — see the write-side accumulation spec.)
    {
        ZoneScopedN("SubFlush.HeaderWrite");
        const size_t slotOffset = s_frameSlot * s_slotBytes;
        GpuActorRecordHeader* hdr =
            reinterpret_cast<GpuActorRecordHeader*>(
                static_cast<char*>(s_mappedPtr) + slotOffset);

        hdr->recordCount    = s_perFrameCount;
        hdr->recordCapacity = s_maxActors;
        hdr->visibleCount   = 0;   // written by compute (C1+); CPU sets 0
        hdr->_pad0          = 0;
    }
    // Snapshot CPU slot count to match hdr->recordCount.
    s_cpuSlotCount[s_frameSlot] = s_perFrameCount;

    // Buffer is coherent (GL_MAP_COHERENT_BIT) — no explicit glFlushMappedBufferRange needed.

    // Insert a fence for this slot so the next visit to this ring slot
    // waits until the GPU has finished reading it.
    { ZoneScopedN("SubFlush.FenceInsert"); s_fence[s_frameSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0); }

    // 600-frame summary + first-flush banner.
    ++s_flushCount;
    const bool firstFlush = !s_firstFlushDone;
    s_firstFlushDone = true;

    if (firstFlush || (s_flushCount % 600) == 0) {
        MC2_DIAGF("[GPU_CULL v1] event=substrate_ready records=%u capacity=%u slot=%u\n",
               s_perFrameCount, s_maxActors, s_frameSlot);
        fflush(stdout);
    }

    gpu_cull::parity_flushSummary();

    SUBSTRATE_TRACE("event=flush_upload records=%u slot=%u", s_perFrameCount, s_frameSlot);
}

// ---------------------------------------------------------------------------
// substrate_appendStaticPropRecord (C1b GPU authority flip)
// ---------------------------------------------------------------------------
//
// Appends one static prop record to the ALREADY-FLUSHED current ring slot and
// updates hdr->recordCount in-place. Called from GpuStaticPropRegistry::flush()
// between substrate_flushUpload() and compute_dispatch() so the compute shader
// sees both dynamic actors AND static prop records in the same dispatch.
//
// Safety: the fence placed by substrate_flushUpload() gates GPU reads from the
// PREVIOUS visit to this ring slot, not the current CPU-write session. Writes to
// the persistent-mapped coherent buffer are immediately visible to the GPU so
// compute_dispatch() (glCopyBufferSubData) will copy the updated header + records.

void substrate_appendStaticPropRecord(const GpuActorRecord& rec) {
    if (!substrate_isEnabled() || !s_initialized) return;
    if (!s_mappedPtr) return;

    if (s_perFrameCount >= s_maxActors) {
        // Overflow: log once per frame (latch resets in substrate_frameBegin).
        // 2026-05-11: prior implementation tested s_perFrameCount==s_maxActors
        // but never incremented past cap in the overflow branch, so every excess
        // submit re-fired the print. On pause this produced hundreds of prints
        // per frame and visibly stalled the game.
        if (!s_overflowLoggedThisFrame) {
            s_overflowLoggedThisFrame = true;
            MC2_DIAGF("[GPU_CULL v1] event=static_prop_overflow at=%u cap=%u\n",
                   s_perFrameCount, s_maxActors);
            fflush(stdout);
        }
        return;
    }

    substrate_writeRecord(rec);

    // Update hdr->recordCount in-place so compute_dispatch's glCopyBufferSubData
    // copies the updated count. Buffer is GL_MAP_COHERENT_BIT — no explicit flush.
    const size_t slotOffset = s_frameSlot * s_slotBytes;
    GpuActorRecordHeader* hdr =
        reinterpret_cast<GpuActorRecordHeader*>(
            static_cast<char*>(s_mappedPtr) + slotOffset);
    hdr->recordCount = s_perFrameCount;
    // Keep CPU slot count in sync with hdr->recordCount after each static-prop append.
    s_cpuSlotCount[s_frameSlot] = s_perFrameCount;

    SUBSTRATE_TRACE("event=append_static type=%u count=%u",
                    (rec.category >> 4), s_perFrameCount);
}

#if defined(MC2_SUBSTRATE_COUNT_PARITY)
// PROOF-ONLY: recompute the all-records legacy count over the FINAL
// post-static-append population and assert the write-side accumulator
// matches. Default-OFF compile gate; never in a shipping/default build;
// removed in the post-proof cleanup. Caller MUST invoke this AFTER the last
// static-prop append for the frame (pre compute_dispatch), NOT inside flush.
void substrate_countParityCheck() {
    if (!substrate_isEnabled() || !s_initialized || !s_mappedPtr) return;
    const size_t slotOffset = s_frameSlot * s_slotBytes;
    const GpuActorRecord* recs = reinterpret_cast<const GpuActorRecord*>(
        static_cast<const char*>(s_mappedPtr) + slotOffset + sizeof(GpuActorRecordHeader));
    uint32_t legacy = 0;
    for (uint32_t i = 0; i < s_perFrameCount; ++i)
        if (recs[i].prevVisibilityBit) ++legacy;
    if (legacy != s_cpuVisibleCount) {
        MC2_DIAGF("[GPU_CULL v1] event=cpuvis_parity_mismatch legacy=%u accum=%u count=%u\n",
               legacy, s_cpuVisibleCount, s_perFrameCount);
        fflush(stdout);
    }
}
#endif

// ---------------------------------------------------------------------------
// substrate_getCurrentRecordCount
// ---------------------------------------------------------------------------

uint32_t substrate_getCurrentRecordCount() {
    return s_perFrameCount;
}

// ---------------------------------------------------------------------------
// substrate_getSlotRecords (C3)
// ---------------------------------------------------------------------------
//
// Returns the GpuActorRecord array for the given ring slot. The returned
// pointer points directly into the persistent-mapped SSBO; it is valid as
// long as the substrate is initialized. The CPU may safely read this range
// after the matching fence for that slot has been consumed (i.e. the GPU has
// finished reading it and signalled the fence). Because readback slot K ==
// substrate slot K, callers should only query a slot that the readback ring
// has confirmed is no longer in flight on the GPU.
//
// outCount is set to hdr->recordCount for the slot (may be 0 if no frame has
// been flushed yet, or if the substrate was just reset).

const GpuActorRecord* substrate_getSlotRecords(uint32_t slot, uint32_t* outCount) {
    if (!s_initialized || !s_mappedPtr) {
        if (outCount) *outCount = 0u;
        return nullptr;
    }
    if (slot >= RING_FRAMES) {
        if (outCount) *outCount = 0u;
        return nullptr;
    }

    const size_t slotOffset = slot * s_slotBytes;
    const char* slotBase = static_cast<const char*>(s_mappedPtr) + slotOffset;

    const GpuActorRecordHeader* hdr =
        reinterpret_cast<const GpuActorRecordHeader*>(slotBase);
    if (outCount) *outCount = hdr->recordCount;

    return reinterpret_cast<const GpuActorRecord*>(slotBase + sizeof(GpuActorRecordHeader));
}

// ---------------------------------------------------------------------------
// substrate_getCpuActorIds (PERF-GPU-CULL-READBACK-ID-CACHE-1)
// ---------------------------------------------------------------------------
//
// Returns the CPU-side actor ID array for the given ring slot.
// Parallel to substrate_getSlotRecords: cpuIds[i] == records[i].actorId for
// every valid index i < *outCount. Reading from this array avoids the ~4000
// PCIe/BAR cache misses that reading GpuActorRecord.actorId from the
// persistent-mapped SSBO incurs in readback_buildActorVisSnapshot.
// outCount mirrors s_cpuSlotCount[slot], kept in sync with hdr->recordCount.

const uint32_t* substrate_getCpuActorIds(uint32_t slot, uint32_t* outCount) {
    if (!s_initialized || slot >= RING_FRAMES || !s_cpuActorIds[slot]) {
        if (outCount) *outCount = 0u;
        return nullptr;
    }
    if (outCount) *outCount = s_cpuSlotCount[slot];
    return s_cpuActorIds[slot];
}

} // namespace gpu_cull
