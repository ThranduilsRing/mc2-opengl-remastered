// GameOS/gameos/gos_object_parity.cpp
//
// Slice 2 (object-offload) — Stage 2.D parity readback harness.
//
// See gos_object_parity.h for the slice overview, the schema reservations,
// the default-off contract, and the public API.
//
// Stage 2.D.1 landed: env-gate readers, SSBO allocation, fence-ring
// readback (bytes discarded), counter infrastructure, 600-frame summary.
//
// Stage 2.D.1.1 landed: slot-overflow accounting, uniform location cache.
//
// Stage 2.D.2 (this file revision): P3 dual-emit state machine + per-type
// snapshot collection + bytewise compare.
//
// Dual-emit flow summary (2.D.2):
//   Frame N  (state=Armed):
//     - bdactor/genactor::update: for each GPU-eligible actor, runs BOTH
//       TransformMultiShape_PositionsOnly (normal GPU) AND
//       TransformMultiShape (full, to populate listOfTriangles[].aRGBLight).
//     - GpuStaticPropBatcher::submitMultiShape: for each accepted leaf,
//       RecordInstanceSnapshot captures per-vertex CPU aRGBLight in
//       submission order (matches parity SSBO instance order).
//     - GpuStaticPropBatcher::flush per-type loop: for each type that gets
//       a parity write, RecordParityTypeRange stores the cursor offset,
//       instanceCount, and vertexCount.
//     - After the fence: AdvanceDualEmitToWaiting() → state=WaitingForReadback.
//   Frame N+1 (state=WaitingForReadback):
//     - GpuStaticPropBatcher::flush readback: CompareAndReport() decodes the
//       readback bytes and compares against the snapshots. Throttled mismatch
//       lines + 600-frame summary.
//     - AdvanceDualEmitToDone() → state=Done. Snapshot and range map freed.

#include "gos_object_parity.h"

// Stage 2.D.1.1 (Item 4, Option A): include the batcher header so kRingFrames
// can reference STATIC_PROP_RING_FRAMES directly rather than duplicating the
// literal 3. No circular dependency: gos_object_parity.h has no batcher.h
// dependency; the coupling is one-directional (parity .cpp → batcher .h).
#include "gos_static_prop_batcher.h"
#include "gos_mech_batcher.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_map>
#include <vector>
#include "Gates/gate.h"
#include "mc2fmt.h"

namespace gos_object_parity {

// ---------------------------------------------------------------------------
// Env-gate readers
// ---------------------------------------------------------------------------

bool IsParityCheckEnabled() {
    static const bool s = mc2::gate::get<"MC2_OBJECT_PARITY_CHECK">();
    return s;
}

bool IsParityTraceEnabled() {
    static const bool s = mc2::gate::get<"MC2_OBJECT_PARITY_TRACE">();
    return s;
}

}  // namespace gos_object_parity

// ---------------------------------------------------------------------------
// Counter storage (TU-private; cross-TU access via public Add/Get)
// ---------------------------------------------------------------------------
namespace {
uint64_t s_vertices_written_total = 0;
uint64_t s_vertices_written_this_frame = 0;
uint64_t s_readback_bytes_total = 0;
uint64_t s_readback_bytes_this_frame = 0;

// Stage 2.D.1.1 (Item 1): slot-overflow tracking.
uint64_t s_slot_overflows_total = 0;
uint64_t s_slot_overflows_this_frame = 0;

// Once-per-type overflow print guard. Tracks which typeIds have already had
// their "slot_overflow" line emitted. Grows unboundedly only if there are
// unbounded unique typeIds, but the type count is fixed at map-load time
// (bounded by the number of registered TG_TypeShape objects, typically <100).
std::set<uint32_t> s_overflowedTypes;
}  // namespace

// ---------------------------------------------------------------------------
// Stage 2.D.2 — Dual-emit state machine storage.
// ---------------------------------------------------------------------------
namespace {

enum class DualEmitState { Armed, WaitingForReadback, Done };
DualEmitState s_dualEmitState = DualEmitState::Done;

// Stage 2.D.3 — arm mode. Bootstrap (Armed+All) preserves the 2.D.2 first-
// frame "all eligible actors dual-emit" behavior on mission load. The sampler
// flips to Armed+SampledOnly for steady-state per-frame round-robin coverage.
enum class ArmMode { All, SampledOnly };
ArmMode s_armMode = ArmMode::All;

// Sampler state (Stage 2.D.3).
//
// s_currentSampledShape: the per-actor multishape pointer the sampler picked
//   for THIS frame's update phase. Set by PickNextSampleIfReady() at the
//   end of flush() (frame N) so the next frame's update gates (frame N+1)
//   can compare against it. Reset to nullptr in OnMissionLoad().
//
// s_observedShapesThisFrame: accumulator. submitMultiShape() pushes here for
//   every actor whose registered-types gate passed (i.e. EVERY actor that
//   would have been a candidate for the legacy 2.D.2 dual-emit-everyone
//   gate). At the end of flush(), if state==Done we rotate this into
//   s_observedShapesPriorFrame and pick from there.
//
// s_observedShapesPriorFrame: pool the sampler picks from. Frozen for the
//   duration of the next frame so PickNextSampleIfReady can be called while
//   the next frame's submit accumulator is being built.
//
// s_samplerCursor: round-robin index. % pool.size() at pick time.
// s_sampledComparesTotal: number of compares performed in SampledOnly mode.
const TG_MultiShape* s_currentSampledShape = nullptr;
std::vector<const TG_MultiShape*> s_observedShapesThisFrame;
std::vector<const TG_MultiShape*> s_observedShapesPriorFrame;
size_t   s_samplerCursor          = 0;
uint64_t s_sampledComparesTotal   = 0;

// The ring slot (s_frameSlot value) on which the Armed frame wrote its parity
// data. CompareAndReport should only run when the ring revisits this slot
// (i.e. when the GPU-complete fence wait clears that slot's writes).
// RING_FRAMES = 3, so that slot comes back 3 flushes later.
uint32_t s_dualEmitSlot = 0xFFFFFFFFu;  // invalid sentinel

// Per-type CPU snapshot.
//
// Stage 2.D.2 stored a flat cpuARGB vector keyed implicitly by submission
// order; that worked when ALL eligible actors dual-emitted (every bucket
// instance had a matching snapshot entry).
//
// Stage 2.D.3 SampledOnly mode breaks that 1:1 invariant — only ONE actor
// per type contributes a snapshot, but the bucket may hold many instances
// from other actors. To compare correctly the sidecar must remember which
// instance index in the bucket the snapshot belongs to.
//
// Storage: per typeId, a small vector of (instanceIdx, perVertexARGB)
// pairs. CompareAndReport uses each pair's instanceIdx to compute the
// offset into the readback bytes. Bootstrap arm (armMode=All) records one
// entry per submission with instanceIdx==current bucket size — equivalent
// to the prior implicit-index storage but explicit. SampledOnly arm
// records one entry per sampled leaf with the matching bucket index.
struct DualEmitInstanceSnapshot {
    uint32_t              instanceIdx;
    std::vector<uint32_t> perVertexARGB;
};
struct DualEmitTypeSnapshot {
    uint32_t                              vertexCount;
    std::vector<DualEmitInstanceSnapshot> instances;
};
std::unordered_map<uint32_t, DualEmitTypeSnapshot> s_dualEmitSnapshots;

// Per-type SSBO byte range recorded at draw time (frame N); used for decode
// at compare time (frame N+1). cursorOffset is the byte offset within the
// parity slot (NOT the absolute buffer offset — already slot-relative).
struct DualEmitTypeRange {
    size_t   cursorOffset;   // slot-relative byte offset
    uint32_t instanceCount;
    uint32_t vertexCount;
};
std::unordered_map<uint32_t, DualEmitTypeRange> s_dualEmitRanges;

// Stage 2.D.2 counters (compare totals across dual-emit + future P1).
uint64_t s_compared_total   = 0;
uint64_t s_passed_total     = 0;
uint64_t s_mismatched_total = 0;

// Mismatch throttle + summary counters (moved from the per-printer namespace
// block so that ParityPrintMismatchByIndex can access them before the
// existing ParityPrintMismatch definition).
int       s_parityMismatchesThisFrame = 0;
long long s_paritySummaryFrames       = 0;
long long s_paritySummaryMismatches   = 0;

}  // namespace

namespace gos_object_parity {

void Counters_AddVerticesWrittenThisFrame(uint64_t n) {
    s_vertices_written_this_frame += n;
}

void Counters_AddReadbackBytesThisFrame(uint64_t n) {
    s_readback_bytes_this_frame += n;
}

void Counters_AddSlotOverflowThisFrame(uint32_t typeId,
                                        size_t   needBytes,
                                        size_t   budgetBytes) {
    ++s_slot_overflows_this_frame;
    // Once-per-type printf so a busy scene doesn't spam the log.
    if (s_overflowedTypes.find(typeId) == s_overflowedTypes.end()) {
        s_overflowedTypes.insert(typeId);
        mc2::fmt::print_err("[OBJECT_PARITY v1] event=slot_overflow type={:d} need={:d} budget={:d}\n", mc2::fmt::as_u32(typeId), mc2::fmt::as_size(needBytes), mc2::fmt::as_size(budgetBytes));
        std::fflush(stderr);
    }
}

uint64_t Counters_GetVerticesWrittenTotal()  { return s_vertices_written_total; }
uint64_t Counters_GetReadbackBytesTotal()    { return s_readback_bytes_total; }
uint64_t Counters_GetSlotOverflowsTotal()    { return s_slot_overflows_total; }

// Stage 2.D.2 compare counters.
uint64_t Counters_GetComparedTotal()    { return s_compared_total; }
uint64_t Counters_GetPassedTotal()      { return s_passed_total; }
uint64_t Counters_GetMismatchedTotal()  { return s_mismatched_total; }
void     Counters_AddCompared(uint64_t n)    { s_compared_total   += n; }
void     Counters_AddPassed(uint64_t n)      { s_passed_total     += n; }
void     Counters_AddMismatched(uint64_t n)  { s_mismatched_total += n; }

void Counters_ResetSlotOverflowsThisFrame() {
    // Called from ParityFrameTick(); not for external use.
    s_slot_overflows_total     += s_slot_overflows_this_frame;
    s_slot_overflows_this_frame = 0;
}

}  // namespace gos_object_parity

// ---------------------------------------------------------------------------
// Parity-output SSBO storage.
//
// Single GL buffer sized RING_FRAMES * s_slotBytes. Lazy-allocated on the
// first call to EnsureParityOutputSSBO() when IsParityCheckEnabled() is
// true. Slice 1's batcher already runs a 3-frame fence ring on
// s_fence[s_frameSlot]; we mirror its RING_FRAMES so a one-frame async
// readback (at top of the next visit to a slot) is naturally synchronized
// by the same fence — no parallel fence ring per the advisor's load-bearing
// rule in the hand-off prompt.
// ---------------------------------------------------------------------------
namespace {

// Stage 2.D.1.1 (Item 4, Option A): use STATIC_PROP_RING_FRAMES from the
// batcher header instead of duplicating the literal. A mismatched ring depth
// would corrupt the async-readback handshake (the batcher's fence and the
// parity SSBO ring must advance in lock-step).
constexpr unsigned kRingFrames = STATIC_PROP_RING_FRAMES;

GLuint   s_paritySSBO  = 0;
size_t   s_slotBytes   = 0;

}  // namespace

namespace gos_object_parity {

GLuint EnsureParityOutputSSBO(size_t bytesPerSlot) {
    if (!IsParityCheckEnabled()) return 0;
    if (bytesPerSlot == 0) return s_paritySSBO;

    // Re-create on grow. 2.D.1 batcher edits pass a fixed kSlotBytes so
    // this branch only fires on the very first call; future stages may
    // ratchet it up as P1 sampling expands per-frame coverage.
    if (s_paritySSBO != 0 && bytesPerSlot > s_slotBytes) {
        glDeleteBuffers(1, &s_paritySSBO);
        s_paritySSBO = 0;
        s_slotBytes  = 0;
    }

    if (s_paritySSBO == 0) {
        // TIER2-EXCLUDED: substrate-gated
        glGenBuffers(1, &s_paritySSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_paritySSBO);
        // glBufferStorage with DYNAMIC_STORAGE makes the buffer eligible
        // for glGetBufferSubData (the Stage 2.D.1 readback path) while
        // still allowing the GPU to write to it via the std430 binding.
        // No persistent map — we only want one client-side read per frame
        // at the top of flush(), which glGetBufferSubData handles directly.
        const GLsizeiptr totalBytes =
            static_cast<GLsizeiptr>(bytesPerSlot) * static_cast<GLsizeiptr>(kRingFrames);
        glBufferStorage(GL_SHADER_STORAGE_BUFFER, totalBytes, nullptr,
                        GL_DYNAMIC_STORAGE_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        s_slotBytes = bytesPerSlot;

        if (IsParityTraceEnabled()) {
            mc2::fmt::print_err("[OBJECT_PARITY v1] event=ssbo_alloc bytes_per_slot={:d} ring_frames={:d} total_bytes={:d} handle={:d}\n", mc2::fmt::as_size(bytesPerSlot), mc2::fmt::as_u32(kRingFrames), mc2::fmt::as_i64(static_cast<long long>(totalBytes)), mc2::fmt::as_u32(static_cast<unsigned>(s_paritySSBO)));
            std::fflush(stderr);
        }
    }
    return s_paritySSBO;
}

size_t   SlotBytes()  { return s_slotBytes; }
unsigned RingFrames() { return kRingFrames; }

void ReleaseParityOutputSSBO() {
    if (s_paritySSBO) {
        glDeleteBuffers(1, &s_paritySSBO);
        s_paritySSBO = 0;
        s_slotBytes  = 0;
    }
}

}  // namespace gos_object_parity

// ---------------------------------------------------------------------------
// Stage 2.D.2 — Dual-emit state machine implementations.
// ---------------------------------------------------------------------------
namespace gos_object_parity {

void OnMissionLoad() {
    if (!IsParityCheckEnabled()) return;
    // Bootstrap arm: state=Armed + armMode=All preserves 2.D.2's first-frame
    // "every eligible actor dual-emits" behavior so the bootstrap compare
    // matches the existing baseline. After that compare goes through Done,
    // the round-robin sampler kicks in (PickNextSampleIfReady flips armMode
    // to SampledOnly and picks one actor per cycle).
    s_dualEmitState = DualEmitState::Armed;
    s_armMode       = ArmMode::All;
    s_currentSampledShape = nullptr;
    s_observedShapesThisFrame.clear();
    s_observedShapesPriorFrame.clear();
    s_samplerCursor       = 0;
    s_dualEmitSnapshots.clear();
    s_dualEmitRanges.clear();
    if (IsParityTraceEnabled()) {
        mc2::fmt::print_err("[OBJECT_PARITY v1] event=dual_emit_armed mode=bootstrap\n");
        std::fflush(stderr);
    }
}

bool IsDualEmitArmedAnyActor() {
    if (!IsParityCheckEnabled()) return false;
    return s_dualEmitState == DualEmitState::Armed;
}

bool IsDualEmitArmedForActor(const TG_MultiShape* shape) {
    if (!IsParityCheckEnabled()) return false;
    if (s_dualEmitState != DualEmitState::Armed) return false;
    if (s_armMode == ArmMode::All) return true;     // bootstrap: every actor
    // SampledOnly: only the picked actor returns true. Pointer equality is
    // sufficient — the multishape pointer is stable for the lifetime of the
    // owning actor (allocated at appearance load, freed at unload).
    if (!shape || !s_currentSampledShape) return false;
    return shape == s_currentSampledShape;
}

bool IsDualEmitReadyForSlot(uint32_t slotNow) {
    if (!IsParityCheckEnabled()) return false;
    if (s_dualEmitState != DualEmitState::WaitingForReadback) return false;
    return slotNow == s_dualEmitSlot;
}

void AdvanceDualEmitToWaiting(uint32_t slotUsed) {
    if (!IsParityCheckEnabled()) return;
    if (s_dualEmitState != DualEmitState::Armed) return;
    s_dualEmitSlot  = slotUsed;
    s_dualEmitState = DualEmitState::WaitingForReadback;
    if (IsParityTraceEnabled()) {
        mc2::fmt::print_err("[OBJECT_PARITY v1] event=dual_emit_waiting slot={:d} types_snapshotted={:d} types_ranged={:d}\n", mc2::fmt::as_u32(slotUsed), mc2::fmt::as_size(s_dualEmitSnapshots.size()), mc2::fmt::as_size(s_dualEmitRanges.size()));
        std::fflush(stderr);
    }
}

void AdvanceDualEmitToDone() {
    if (!IsParityCheckEnabled()) return;
    // Stage 2.D.3: count this compare in the sampler-coverage tally only when
    // we're advancing OUT of a SampledOnly arm (the bootstrap arm advances
    // through this same path on the first compare; we count it too because
    // it's one compare worth of coverage even though the pick wasn't via
    // round-robin). The summary field name is parity_compared_actors and
    // includes both the bootstrap compare and the sampled compares.
    if (s_dualEmitState == DualEmitState::WaitingForReadback) {
        ++s_sampledComparesTotal;
    }
    s_dualEmitState = DualEmitState::Done;
    // Free snapshot and range maps — not needed for the rest of this mission.
    s_dualEmitSnapshots.clear();
    s_dualEmitRanges.clear();
}

void ObserveSubmittedShape(const TG_MultiShape* shape) {
    // Default-off short-circuit: when MC2_OBJECT_PARITY_CHECK is unset, no
    // observation is needed (the sampler never runs, and the per-actor gate
    // returns false for every shape regardless).
    if (!IsParityCheckEnabled()) return;
    if (!shape) return;
    // Append unconditionally; duplicate-multishape submissions per frame are
    // not expected in stock content (each actor calls submitMultiShape at
    // most once via *Appearance::render). If a duplicate ever occurred, the
    // sampler would just pick the same actor twice — harmless coverage cost
    // but not a correctness issue.
    s_observedShapesThisFrame.push_back(shape);
}

void PickNextSampleIfReady() {
    if (!IsParityCheckEnabled()) return;

    // Rotate this-frame accumulator into prior-frame pool exactly once per
    // flush cycle. We do this REGARDLESS of state so the pool always reflects
    // the most recent frame's observations — even if we're not picking this
    // frame (e.g. WaitingForReadback), the pool stays fresh for when we are.
    if (!s_observedShapesThisFrame.empty()) {
        s_observedShapesPriorFrame.swap(s_observedShapesThisFrame);
        s_observedShapesThisFrame.clear();
    } else {
        // No observations this frame — keep the prior pool as-is so we still
        // have something to pick from on the next Done transition. This is
        // a defensive choice for ramp-up missions where update→render hasn't
        // been called yet for some types in the very first few frames.
    }

    // Only pick when the latch is Done (compare complete and machinery idle).
    if (s_dualEmitState != DualEmitState::Done) return;
    if (s_observedShapesPriorFrame.empty()) return;

    // Round-robin pick.
    const size_t poolSize = s_observedShapesPriorFrame.size();
    const size_t idx = s_samplerCursor % poolSize;
    const TG_MultiShape* picked = s_observedShapesPriorFrame[idx];
    s_samplerCursor = (s_samplerCursor + 1) % poolSize;

    if (!picked) return;  // shouldn't happen — ObserveSubmittedShape rejects null

    s_currentSampledShape = picked;
    s_armMode             = ArmMode::SampledOnly;
    s_dualEmitState       = DualEmitState::Armed;

    if (IsParityTraceEnabled()) {
        mc2::fmt::print_err("[OBJECT_PARITY v1] event=dual_emit_armed mode=sampled shape={:016X} pool_size={:d} cursor={:d}\n", mc2::fmt::as_ptr((const void*)picked), mc2::fmt::as_size(poolSize), mc2::fmt::as_size(idx));
        std::fflush(stderr);
    }
}

uint64_t Counters_GetSampledCompares() {
    return s_sampledComparesTotal;
}

}  // namespace gos_object_parity

// ---------------------------------------------------------------------------
// Stage 2.D.2 — Snapshot collection and type-range record.
// ---------------------------------------------------------------------------
namespace gos_object_parity {

void RecordInstanceSnapshot(uint32_t typeId,
                             uint32_t instanceIdx,
                             const uint32_t* perVertexCpuARGB,
                             uint32_t vertexCount) {
    // Should only be called when Armed; caller gates on IsDualEmitArmedForActor().
    // Double-check defensively.
    if (s_dualEmitState != DualEmitState::Armed) return;
    if (!perVertexCpuARGB || vertexCount == 0) return;

    DualEmitTypeSnapshot& snap = s_dualEmitSnapshots[typeId];
    snap.vertexCount = vertexCount;
    snap.instances.emplace_back();
    DualEmitInstanceSnapshot& entry = snap.instances.back();
    entry.instanceIdx = instanceIdx;
    entry.perVertexARGB.assign(perVertexCpuARGB,
                               perVertexCpuARGB + vertexCount);
}

void RecordParityTypeRange(uint32_t typeId,
                           size_t   cursorOffset,
                           uint32_t instanceCount,
                           uint32_t vertexCount) {
    // Should only be called when Armed; caller gates on IsDualEmitArmed().
    if (s_dualEmitState != DualEmitState::Armed) return;
    DualEmitTypeRange& rng = s_dualEmitRanges[typeId];
    rng.cursorOffset  = cursorOffset;
    rng.instanceCount = instanceCount;
    rng.vertexCount   = vertexCount;
}

}  // namespace gos_object_parity

// ---------------------------------------------------------------------------
// Stage 2.D.2 — CompareAndReport.
//
// readbackBytes is the raw bytes glGetBufferSubData'd from the parity SSBO
// slot for frame N. The byte range is slot-relative (offset 0 = start of the
// slot). Each recorded type range's cursorOffset is already slot-relative
// (recorded as the cursor within the slot by the draw loop).
//
// GPU parity SSBO packs per-vertex uint32 as b8|(g8<<8)|(r8<<16)|(a8<<24)
// (matching the CPU ARGB format (A<<24)|(R<<16)|(G<<8)|B — same bit layout,
// same uint32 value).
//
// ULP tolerance: ±2 LSB per channel independently.
// ---------------------------------------------------------------------------
namespace gos_object_parity {

static bool channelsWithin2(uint32_t cpu, uint32_t gpu) {
    // Extract each channel and test.
    const int channels[4] = {
        static_cast<int>(cpu & 0xFF)        - static_cast<int>(gpu & 0xFF),
        static_cast<int>((cpu >>  8) & 0xFF) - static_cast<int>((gpu >>  8) & 0xFF),
        static_cast<int>((cpu >> 16) & 0xFF) - static_cast<int>((gpu >> 16) & 0xFF),
        static_cast<int>((cpu >> 24) & 0xFF) - static_cast<int>((gpu >> 24) & 0xFF),
    };
    for (int d : channels) {
        if (d < -2 || d > 2) return false;
    }
    return true;
}

void CompareAndReport(const uint8_t* readbackBytes,
                      size_t         readbackSize,
                      unsigned       frameNum) {
    if (!IsParityCheckEnabled()) return;
    if (s_dualEmitState != DualEmitState::WaitingForReadback) return;
    if (!readbackBytes || readbackSize == 0) return;

    uint64_t totalCompared   = 0;
    uint64_t totalPassed     = 0;
    uint64_t totalMismatched = 0;

    // Iterate every type that has both a snapshot and a range record.
    for (const auto& snapKv : s_dualEmitSnapshots) {
        const uint32_t typeId = snapKv.first;
        const DualEmitTypeSnapshot& snap = snapKv.second;

        auto rangeIt = s_dualEmitRanges.find(typeId);
        if (rangeIt == s_dualEmitRanges.end()) {
            // No GPU range recorded — slot overflow or type got no parity write.
            // Skip silently: these are counted by the overflow counter elsewhere.
            continue;
        }
        const DualEmitTypeRange& rng = rangeIt->second;

        // Sanity: vertexCount must agree between snapshot and range record.
        if (snap.vertexCount == 0 || rng.vertexCount == 0) continue;
        if (snap.vertexCount != rng.vertexCount) {
            // Geometry changed between submit and draw — skip (shouldn't happen).
            std::fprintf(stderr,
                "[OBJECT_PARITY v1] event=vertex_count_mismatch type=%u "
                "snap_verts=%u range_verts=%u -- skipping type\n",
                typeId, snap.vertexCount, rng.vertexCount);
            std::fflush(stderr);
            continue;
        }

        const uint32_t verts = snap.vertexCount;
        const uint32_t instCount = rng.instanceCount;

        // Verify bounds against readbackSize.
        // Each instance occupies verts * sizeof(uint32_t) bytes in the SSBO.
        const size_t typeByteCount = (size_t)instCount * (size_t)verts * sizeof(uint32_t);
        if (rng.cursorOffset + typeByteCount > readbackSize) {
            // Readback buffer is smaller than expected. Skip this type —
            // don't read out-of-bounds.
            if (IsParityTraceEnabled()) {
                mc2::fmt::print_err("[OBJECT_PARITY v1] event=readback_bounds_miss type={:d} cursor={:d} need={:d} avail={:d}\n", mc2::fmt::as_u32(typeId), mc2::fmt::as_size(rng.cursorOffset), mc2::fmt::as_size(rng.cursorOffset + typeByteCount), mc2::fmt::as_size(readbackSize));
                std::fflush(stderr);
            }
            continue;
        }

        // Walk each (instance, vertex) pair. Stage 2.D.3: iterate the per-
        // instance snapshot list and use each entry's explicit instanceIdx
        // to address the GPU readback (matches the bucket position the
        // shader wrote at). For bootstrap arm (armMode=All), every bucket
        // instance has a snapshot entry with sequential instanceIdx; for
        // SampledOnly arm, the list typically has 1 entry per type whose
        // instanceIdx is wherever the sampled actor's leaf landed.
        const uint32_t* gpuBase = reinterpret_cast<const uint32_t*>(
            readbackBytes + rng.cursorOffset);

        for (const DualEmitInstanceSnapshot& entry : snap.instances) {
            const uint32_t inst = entry.instanceIdx;
            if (inst >= instCount) {
                // Out of bucket bounds — geometry changed between submit and
                // draw, or sampled actor never reached the bucket. Skip.
                continue;
            }
            if (entry.perVertexARGB.size() != verts) {
                // Defensive: per-vertex blob size must match the type's vertex count.
                continue;
            }
            for (uint32_t v = 0; v < verts; ++v) {
                const uint32_t cpuARGB = entry.perVertexARGB[v];
                const uint32_t gpuARGB = gpuBase[inst * verts + v];
                ++totalCompared;
                if (channelsWithin2(cpuARGB, gpuARGB)) {
                    ++totalPassed;
                } else {
                    ++totalMismatched;
                    ParityPrintMismatchByIndex(frameNum, typeId, inst, v,
                                               cpuARGB, gpuARGB);
                }
            }
        }
    }

    Counters_AddCompared(totalCompared);
    Counters_AddPassed(totalPassed);
    Counters_AddMismatched(totalMismatched);

    if (IsParityTraceEnabled() || totalMismatched > 0) {
        mc2::fmt::print_err("[OBJECT_PARITY v1] event=dual_emit_compare frame={:d} compared={:d} passed={:d} mismatched={:d}\n", mc2::fmt::as_u32(frameNum), mc2::fmt::as_u64(static_cast<unsigned long long>(totalCompared)), mc2::fmt::as_u64(static_cast<unsigned long long>(totalPassed)), mc2::fmt::as_u64(static_cast<unsigned long long>(totalMismatched)));
        std::fflush(stderr);
    }
}

void ParityPrintMismatchByIndex(unsigned frame,
                                uint32_t typeId,
                                uint32_t instIdx,
                                uint32_t vertIdx,
                                uint32_t cpuARGB,
                                uint32_t gpuARGB) {
    if (s_parityMismatchesThisFrame >= 16) return;  // throttle 16/frame
    ++s_parityMismatchesThisFrame;
    mc2::fmt::print_err("[OBJECT_PARITY v1] event=lighting_mismatch frame={:d} typeId={:d} inst={:d} vert={:d} cpu=0x{:08X} gpu=0x{:08X}\n", mc2::fmt::as_u32(frame), mc2::fmt::as_u32(typeId), mc2::fmt::as_u32(instIdx), mc2::fmt::as_u32(vertIdx), mc2::fmt::as_u32(cpuARGB), mc2::fmt::as_u32(gpuARGB));
    std::fflush(stderr);
}

}  // namespace gos_object_parity

// ---------------------------------------------------------------------------
// Mismatch printer + 600-frame summary.
//
// Stage 2.D.1: ParityPrintMismatch is a SKELETON (matching the spec schema)
// that no caller invokes yet. ParityFrameTick is invoked unconditionally
// from GpuStaticPropBatcher::flush(); when the env-gate is off it is a
// no-op (single branch-predicted bool load).
// Stage 2.D.2: ParityPrintMismatchByIndex defined earlier (in the CompareAndReport
// section) since it shares s_parityMismatchesThisFrame. Storage for that counter
// consolidated in the Stage 2.D.2 anonymous namespace block above.
// ---------------------------------------------------------------------------
namespace gos_object_parity {

void ParityPrintMismatch(int frame, const char* actor, int tri, int corner,
                         uint32_t cpuARGB, uint32_t gpuARGB) {
    if (s_parityMismatchesThisFrame >= 16) return;  // throttle 16/frame
    ++s_parityMismatchesThisFrame;
    mc2::fmt::print_err("[OBJECT_PARITY v1] event=lighting_mismatch frame={:d} actor={} tri={:d} corner={:d} cpu=0x{:08X} gpu=0x{:08X}\n", mc2::fmt::as_i32(frame), mc2::fmt::safe(actor ? actor : "?"), mc2::fmt::as_i32(tri), mc2::fmt::as_i32(corner), mc2::fmt::as_u32(cpuARGB), mc2::fmt::as_u32(gpuARGB));
    std::fflush(stderr);
}

void ParityFrameTick() {
    // Default-off short-circuit: when MC2_OBJECT_PARITY_CHECK is unset, no
    // counters were ticked, no SSBO was allocated, and no summary line is
    // useful. Single bool load = same cost as the unconditional call site.
    if (!IsParityCheckEnabled()) return;

    ++s_paritySummaryFrames;
    s_paritySummaryMismatches += s_parityMismatchesThisFrame;
    s_parityMismatchesThisFrame = 0;

    // Roll per-frame counters into totals.
    s_vertices_written_total += s_vertices_written_this_frame;
    s_readback_bytes_total   += s_readback_bytes_this_frame;
    s_vertices_written_this_frame = 0;
    s_readback_bytes_this_frame   = 0;
    // Stage 2.D.1.1 (Item 1): roll per-frame slot-overflow count.
    Counters_ResetSlotOverflowsThisFrame();

    if (s_paritySummaryFrames % 600 == 0) {
        // Stage 2.D.3: extended summary with sampler coverage and late-reg
        // event accounting.
        //
        // - parity_skipped_allowed_late_reg / parity_unexpected_late_reg are
        //   sourced from the slice 1 batcher (advisor decision A2). The
        //   sampler iterates registered types only, so allowlisted late-reg
        //   actors (skybox Cylinder01, compassplane) never appear in the
        //   sampler pool. The event-counter on the batcher side tells the
        //   operator the exclusion is functioning.
        // - parity_compared_actors counts the bootstrap compare + each
        //   sampled compare (one per Done transition).
        const uint64_t allowedLateReg =
            GpuStaticPropBatcher::getAllowedLateRegEventCount();
        const uint64_t unexpectedLateReg =
            GpuStaticPropBatcher::getDisallowedLateRegEventCount();
        mc2::fmt::print_err("[OBJECT_PARITY v1] event=summary frames={:d} vertices_written={:d} readback_bytes={:d} mismatches={:d} slot_overflows={:d} compared={:d} passed={:d} mismatched={:d} parity_skipped_allowed_late_reg={:d} parity_unexpected_late_reg={:d} parity_compared_actors={:d}\n", mc2::fmt::as_i64(s_paritySummaryFrames), mc2::fmt::as_u64(static_cast<unsigned long long>(s_vertices_written_total)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_readback_bytes_total)), mc2::fmt::as_i64(s_paritySummaryMismatches), mc2::fmt::as_u64(static_cast<unsigned long long>(s_slot_overflows_total)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_compared_total)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_passed_total)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_mismatched_total)), mc2::fmt::as_u64(static_cast<unsigned long long>(allowedLateReg)), mc2::fmt::as_u64(static_cast<unsigned long long>(unexpectedLateReg)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_sampledComparesTotal)));

        // GPU mech batcher late-registration counter (Slice A).
        const uint64_t mechDisallowed = GpuMechBatcher::getDisallowedLateRegEventCount();
        if (mechDisallowed > 0) {
            mc2::fmt::print_err("[OBJECT_PARITY v1] event=mech_late_reg_disallowed count={:d}\n", mc2::fmt::as_u64(static_cast<unsigned long long>(mechDisallowed)));
        }
        std::fflush(stderr);
    }
}

}  // namespace gos_object_parity
