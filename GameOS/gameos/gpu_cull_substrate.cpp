#include "gpu_cull_substrate.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "gpu_cull_parity.h"
#include <memory>
#include <vector>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "gos_profiler.h"        // Tracy ZoneScopedN
#include "diagnostic_trace.h"
#include "Gates/gate.h"
#include "mc2fmt.h"

// MC2_GPU_CULL_SUBSTRATE_TRACE=1 — verbose per-frame lifecycle prints.
// Default off; the 600-frame summary + first-flush banner emit unconditionally.
static const bool s_substrateTrace = false;
#define SUBSTRATE_TRACE(fmt, ...) \
    do { if (s_substrateTrace) { MC2_DIAGF("[GPU_CULL v1] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while (0)

namespace gpu_cull {

// ---------------------------------------------------------------------------
// Ring constants
// ---------------------------------------------------------------------------

constexpr uint32_t RING_FRAMES             = 3;
// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

struct alignas(16) RecordBlock { std::byte bytes[16]; };
static std::vector<RecordBlock> s_recordStorage;
static void* s_recordData=nullptr;
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
static std::unique_ptr<GpuActorRecord[]> s_staticRecords;  // golden copy, sized maxActors (POD: safe at static teardown)
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

uint32_t substrate_getCpuVisibleCount() {
    return s_cpuVisibleCount;
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
    for (unsigned int & i : s_staticDirty) i = 0u;

    // Per-slot layout: [GpuActorRecordHeader][GpuActorRecord * maxActors]
    s_slotBytes = sizeof(GpuActorRecordHeader) + maxActors * sizeof(GpuActorRecord);
    const size_t totalBytes=RING_FRAMES*s_slotBytes;
    s_recordStorage.assign(totalBytes/sizeof(RecordBlock),RecordBlock{});
    s_recordData=s_recordStorage.data();

    // M1: golden copy for the frozen static-prop record prefix (sized maxActors).
    s_staticRecords = std::make_unique<GpuActorRecord[]>(maxActors);

    s_initialized = true;

    MC2_DIAGF("[GPU_CULL v1] event=substrate_init producer=cpu maxActors=%u slotBytes=%zu totalBytes=%zu\n",
              maxActors,s_slotBytes,totalBytes);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// substrate_shutdown
// ---------------------------------------------------------------------------

void substrate_shutdown() {
    if (!substrate_isEnabled()) return;

    // GPU submissions copy their inputs before returning; this ring has no GPU readers.
    s_recordData=nullptr;
    std::vector<RecordBlock>().swap(s_recordStorage);
    // M1: free the frozen static-prop golden copy.
    s_staticRecords.reset();
    s_staticPrefixCount        = 0u;
    s_staticPrefixVisibleCount = 0u;
    for (unsigned int & i : s_staticDirty) i = 0u;

    s_recordData      = nullptr;
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

static void substrate_resetSlotCounts();

void substrate_frameBegin() {
    if (!substrate_isEnabled() || !s_initialized) return;

    // Advance ring slot.
    s_frameSlot = (s_frameSlot + 1) % RING_FRAMES;

    substrate_resetSlotCounts();
    SUBSTRATE_TRACE("event=frame_begin slot=%u", s_frameSlot);
}

void substrate_frameRewind() {
    if (!substrate_isEnabled() || !s_initialized) return;
    substrate_resetSlotCounts();
    SUBSTRATE_TRACE("event=frame_rewind slot=%u", s_frameSlot);
}

// Reset the per-frame record count of the current slot.
static void substrate_resetSlotCounts() {
    if (s_staticFrozen && s_staticPrefixCount > 0u) {
        // M1: static records are a FROZEN prefix [0,S). Refill THIS ring slot
        // from the CPU golden copy if it is dirty (per-slot, lazy, after the
        // prior native snapshot so the GPU is done reading this slot), then start the
        // per-frame cursor AFTER the prefix so dynamic actors append into
        // [S,S+D). No per-frame static append (the registry skips it under the
        // gate). When S==0 or the gate is off, the else-branch keeps the
        // original reset-to-0 behavior — additive + byte-identical.
        const uint32_t S = s_staticPrefixCount;
        if (s_staticDirty[s_frameSlot]) {
            const size_t slotOffset = s_frameSlot * s_slotBytes;
            char* recBase = static_cast<char*>(s_recordData) + slotOffset
                            + sizeof(GpuActorRecordHeader);
            memcpy(recBase, s_staticRecords.get(),
                   static_cast<size_t>(S) * sizeof(GpuActorRecord));
            s_staticDirty[s_frameSlot] = 0u;
        }
        s_perFrameCount             = S;
        s_cpuVisibleCount           = s_staticPrefixVisibleCount;
    } else {
        s_perFrameCount = 0;
        s_cpuVisibleCount = 0;
    }
    s_overflowLoggedThisFrame = false;  // 2026-05-11 reset overflow log latch
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
    for (unsigned int & s : s_staticDirty) s = 1u;
    SUBSTRATE_TRACE("event=rebuild_static_prefix count=%u vis=%u", count, vis);
}

// ---------------------------------------------------------------------------
// substrate_writeRecord  (shared lockstep write core)
// ---------------------------------------------------------------------------
//
// Writes one record into the current ring slot's array and maintains the two
// lockstep per-frame counters (s_perFrameCount, s_cpuVisibleCount).
// PRECONDITION: caller has passed the s_perFrameCount >= s_maxActors overflow
// guard and verified s_recordData != nullptr. The cpuVisible increment is
// computed from the cache-hot `rec` parameter, never by reading back from the
// write-combined mapped buffer; it occurs AFTER the successful memcpy so the
// accumulator counts exactly records actually written into
// recs[0..s_perFrameCount). Replaces the deleted flush-side readback loop.
// Single-threaded: no atomics; inherits the existing s_perFrameCount
// externally-serialized-producer contract.

static inline void substrate_writeRecord(const GpuActorRecord& rec) {
    const size_t slotOffset = s_frameSlot * s_slotBytes;
    char* dest = static_cast<char*>(s_recordData)
                 + slotOffset
                 + sizeof(GpuActorRecordHeader)
                 + s_perFrameCount * sizeof(GpuActorRecord);
    memcpy(dest, &rec, sizeof(GpuActorRecord));
    if (rec.prevVisibilityBit) ++s_cpuVisibleCount;
    ++s_perFrameCount;
}

// ---------------------------------------------------------------------------
// substrate_submitDynamicActor
// ---------------------------------------------------------------------------

void substrate_submitDynamicActor(const GpuActorRecord& rec) {
    if (!substrate_isEnabled() || !s_initialized) return;
    if (!s_recordData) return;

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
                static_cast<char*>(s_recordData) + slotOffset);

        hdr->recordCount    = s_perFrameCount;
        hdr->recordCapacity = s_maxActors;
        hdr->visibleCount   = 0;   // written by compute (C1+); CPU sets 0
        hdr->_pad0          = 0;
    }
    // Snapshot CPU slot count to match hdr->recordCount.

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
// CPU records remain mutable until compute_dispatch snapshots the final span.

void substrate_appendStaticPropRecord(const GpuActorRecord& rec) {
    if (!substrate_isEnabled() || !s_initialized) return;
    if (!s_recordData) return;

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
    // copies the updated count from CPU memory; there is no GPU mapping — no explicit flush.
    const size_t slotOffset = s_frameSlot * s_slotBytes;
    GpuActorRecordHeader* hdr =
        reinterpret_cast<GpuActorRecordHeader*>(
            static_cast<char*>(s_recordData) + slotOffset);
    hdr->recordCount = s_perFrameCount;
    // Keep CPU slot count in sync with hdr->recordCount after each static-prop append.

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
    if (!substrate_isEnabled() || !s_initialized || !s_recordData) return;
    const size_t slotOffset = s_frameSlot * s_slotBytes;
    const GpuActorRecord* recs = reinterpret_cast<const GpuActorRecord*>(
        static_cast<const char*>(s_recordData) + slotOffset + sizeof(GpuActorRecordHeader));
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

// The native producer copies this span before any later CPU ring reuse.
uint32_t substrate_capacity() { return s_initialized ? s_maxActors : 0; }
std::span<const GpuActorRecord> substrate_currentRecords() {
    if (!s_initialized || !s_recordData) return {};
    const auto* bytes=static_cast<const char*>(s_recordData)+s_frameSlot*s_slotBytes+sizeof(GpuActorRecordHeader);
    return {reinterpret_cast<const GpuActorRecord*>(bytes),s_perFrameCount};
}

} // namespace gpu_cull
