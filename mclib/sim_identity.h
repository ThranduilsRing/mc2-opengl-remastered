#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

// SIM-IDENTITY-HASH-1 -- GL-free, engine-free, header-only world-state hash.
//
// WHY THIS EXISTS
// ---------------
// Mover nondeterminism was being chased as a *metrics* problem. It is a DESYNC
// -- the class lockstep games (OpenTTD, OpenRCT2, Dolphin FIFO, libTAS)
// industrialized decades ago. Their fix is not a better pixel diff: it is a
// per-sim-frame world-state hash plus a refusal to grade arms whose worlds
// differ. This header is the hash half.
//
// It is deliberately PURE (no engine types, no I/O, no globals) so it can be
// unit-tested offline -- same discipline as mclib/det_rng.h. The engine-side
// collector that walks ObjectManager lives in code/sim_identity.cpp.
//
// ORDER STABILITY -- BY CONSTRUCTION, NOT BY OBSERVATION
// -----------------------------------------------------
// DETERMINISM-RATCHET-2 *measured* ObjectManager's moverList order to be
// identical across three mc2_10 runs. That is an observation on one map, and an
// observation is not an invariant. digestWorld() therefore SORTS records into a
// total order (id, then every field lexicographically) before folding, so the
// digest is invariant to iteration order full stop -- including any future
// change to buildMoverLists(), and including duplicate ids, which would
// otherwise leave an unstable sort free to permute equal keys.
// The sort key is partId, which GameObjectManager::calcPartId derives from
// (commander, group, mate) indices or a monotonic reinforcement counter -- never
// from an allocation address. No pointer value reaches this hash.
//
// QUANTIZATION -- AND ITS HONEST LIMIT
// ------------------------------------
// Two digests are produced from the same records:
//   * GRADED  -- positions/angles/fractions quantized to the declared quanta
//                below. This is the digest the refuse-to-grade gate consults.
//   * STRICT  -- raw IEEE-754 bits. Maximally sensitive; diagnostic only.
// Grading on STRICT would refuse on 1-ULP float noise that cannot move a pixel,
// which is exactly how a gate becomes wallpaper.
//
// LIMIT, STATED PLAINLY: quantization does not eliminate false divergence, it
// only makes it rare. Two values an epsilon apart can straddle a quantum
// boundary and land in different buckets. That is why the collector also emits
// the RAW per-mover field values into the manifest: the Python comparator prints
// the true delta beside the quantum, so a boundary straddle (delta << quantum)
// is instantly distinguishable from real divergence (delta >> quantum) instead
// of both arriving as an opaque "hashes differ".
//
// FIELD VISIBILITY -- THE PRIMARY ANTI-WALLPAPER MECHANISM
// --------------------------------------------------------
// Fields are split VISUAL vs NON-VISUAL. A VISUAL field, changed alone, can
// change the rendered frame (position, rotation, gesture, anim frame, damage,
// team, status). A NON-VISUAL field (target, brain state, move state, path
// cursor) steers FUTURE motion but moves no pixel on the frame it first differs.
//
// This split is grounded in a real measurement, not a guess. DETERMINISM-RATCHET-2
// pinned the mc2_10 residual to: at t=34.1s exactly one of 48 movers differed,
// in `target` ONLY, with every position identical across all three runs; the
// first POSITION divergence came 0.3s later at t=34.4s. Under a
// visual-field-blind gate both instants refuse identically. Under this split,
// t=34.1s grades (and reports LATENT divergence -- the early warning the prior
// lane had to hand-derive from ~58k log lines), and t=34.4s refuses. The gate
// fires when the picture can actually differ, which is the only time a pixel
// comparison was ever meaningful.

namespace mc2_sim_identity {

// --- DECLARED QUANTA -------------------------------------------------------
// Emitted into every manifest. The digest folds them, so a digest built with
// one quantum set can never silently compare equal to one built with another.
constexpr float kPosQuantum  = 1.0f / 64.0f;    // world units (~0.0156)
constexpr float kAngQuantum  = 1.0f / 64.0f;    // degrees
constexpr float kFracQuantum = 1.0f / 1024.0f;  // 0..1 fractions (hp)

constexpr uint32_t kSchemaVersion = 1;

// --- record ----------------------------------------------------------------
// One live mover. Raw floats are kept alongside so the comparator can report a
// true delta; the graded digest quantizes them at fold time.
struct MoverRecord {
    int32_t id        = 0;   // partId -- sort key, allocation-independent
    int32_t team      = 0;   // VISUAL (team colour)
    float   px        = 0.0f;  // VISUAL
    float   py        = 0.0f;  // VISUAL
    float   pz        = 0.0f;  // VISUAL
    float   rot       = 0.0f;  // VISUAL
    float   hp        = 0.0f;  // VISUAL (damage state drives visible damage)
    int32_t gesture   = 0;   // VISUAL (appearance->getCurrentGestureId())
    int32_t animFrame = 0;   // VISUAL (appearance->getFrameNumber())
    int32_t status    = 0;   // VISUAL (dead/ejected render differently)
    int32_t target    = -1;  // NON-VISUAL
    int32_t brainState = -1; // NON-VISUAL
    int32_t moveState  = -1; // NON-VISUAL
    int32_t pathStep   = -1; // NON-VISUAL
    int32_t pathSteps  = 0;  // NON-VISUAL
};

constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime  = 1099511628211ULL;

inline uint64_t fnvByte(uint64_t h, uint8_t b) {
    h ^= (uint64_t)b;
    h *= kFnvPrime;
    return h;
}

inline uint64_t fnvI32(uint64_t h, int32_t v) {
    const uint32_t u = (uint32_t)v;
    h = fnvByte(h, (uint8_t)(u & 0xFFu));
    h = fnvByte(h, (uint8_t)((u >> 8) & 0xFFu));
    h = fnvByte(h, (uint8_t)((u >> 16) & 0xFFu));
    h = fnvByte(h, (uint8_t)((u >> 24) & 0xFFu));
    return h;
}

// Raw IEEE bits, for the STRICT digest. Normalizes the two zeroes (-0.0f and
// +0.0f compare equal but have different bit patterns) so a sign-of-zero flip
// -- which no renderer can display -- never reports as strict divergence.
inline uint64_t fnvF32Bits(uint64_t h, float f) {
    if (f == 0.0f) f = 0.0f;  // collapse -0.0 -> +0.0
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return fnvI32(h, (int32_t)bits);
}

// --- quantization ----------------------------------------------------------
// Deterministic, symmetric, and total: every float (including NaN/Inf) maps to
// a defined bucket. NaN gets its own sentinel rather than being folded in as
// whatever garbage the cast produced -- so "one arm went NaN" is a visible,
// reproducible divergence instead of an unstable one.
constexpr int32_t kQuantNaN     = INT32_MIN;
constexpr int32_t kQuantPosInf  = INT32_MAX;
constexpr int32_t kQuantNegInf  = INT32_MIN + 1;

inline int32_t quantize(float v, float q) {
    if (!(v == v))            return kQuantNaN;      // NaN
    if (v >  3.0e38f)         return kQuantPosInf;
    if (v < -3.0e38f)         return kQuantNegInf;
    const double s = (double)v / (double)q;
    // Round half away from zero -- symmetric about 0, no banker's-rounding
    // asymmetry between a value and its negation.
    const double r = (s >= 0.0) ? (s + 0.5) : (s - 0.5);
    if (r >=  2147483000.0)   return kQuantPosInf;
    if (r <= -2147483000.0)   return kQuantNegInf;
    return (int32_t)r;
}

// --- total order -----------------------------------------------------------
// Returns <0 / 0 / >0. Orders by id first, then by every remaining field, so
// two records are ordered identically on every machine even if ids collide.
// (An id collision should not happen; relying on it not happening is how an
// unstable sort silently permutes and the digest goes nondeterministic.)
inline int compareRecords(const MoverRecord& a, const MoverRecord& b) {
    if (a.id != b.id) return a.id < b.id ? -1 : 1;
    if (a.team != b.team) return a.team < b.team ? -1 : 1;
    // Compare floats by quantized bucket then raw bits: total, NaN-safe, and
    // never relies on `<` over NaN (which is false in both directions and would
    // make the comparator non-transitive).
    const float qa[5] = {a.px, a.py, a.pz, a.rot, a.hp};
    const float qb[5] = {b.px, b.py, b.pz, b.rot, b.hp};
    const float qq[5] = {kPosQuantum, kPosQuantum, kPosQuantum,
                         kAngQuantum, kFracQuantum};
    for (int i = 0; i < 5; ++i) {
        const int32_t ia = quantize(qa[i], qq[i]);
        const int32_t ib = quantize(qb[i], qq[i]);
        if (ia != ib) return ia < ib ? -1 : 1;
        uint32_t ba = 0, bb = 0;
        float fa = qa[i], fb = qb[i];
        if (fa == 0.0f) fa = 0.0f;
        if (fb == 0.0f) fb = 0.0f;
        std::memcpy(&ba, &fa, sizeof(ba));
        std::memcpy(&bb, &fb, sizeof(bb));
        if (ba != bb) return ba < bb ? -1 : 1;
    }
    const int32_t ra[8] = {a.gesture, a.animFrame, a.status, a.target,
                           a.brainState, a.moveState, a.pathStep, a.pathSteps};
    const int32_t rb[8] = {b.gesture, b.animFrame, b.status, b.target,
                           b.brainState, b.moveState, b.pathStep, b.pathSteps};
    for (int i = 0; i < 8; ++i)
        if (ra[i] != rb[i]) return ra[i] < rb[i] ? -1 : 1;
    return 0;
}

// Insertion sort into the total order above. n is small (tens to low hundreds
// of movers); the point here is determinism, not asymptotics -- std::sort is
// unstable and would be free to permute records that compare equal.
inline void sortRecords(MoverRecord* recs, size_t n) {
    for (size_t i = 1; i < n; ++i) {
        MoverRecord key = recs[i];
        size_t j = i;
        while (j > 0 && compareRecords(recs[j - 1], key) > 0) {
            recs[j] = recs[j - 1];
            --j;
        }
        recs[j] = key;
    }
}

// --- digests ---------------------------------------------------------------

// Per-mover GRADED digest: quantized VISUAL + exact NON-VISUAL fields.
inline uint64_t digestMoverGraded(const MoverRecord& r) {
    uint64_t h = kFnvOffset;
    h = fnvI32(h, r.id);
    h = fnvI32(h, r.team);
    h = fnvI32(h, quantize(r.px,  kPosQuantum));
    h = fnvI32(h, quantize(r.py,  kPosQuantum));
    h = fnvI32(h, quantize(r.pz,  kPosQuantum));
    h = fnvI32(h, quantize(r.rot, kAngQuantum));
    h = fnvI32(h, quantize(r.hp,  kFracQuantum));
    h = fnvI32(h, r.gesture);
    h = fnvI32(h, r.animFrame);
    h = fnvI32(h, r.status);
    h = fnvI32(h, r.target);
    h = fnvI32(h, r.brainState);
    h = fnvI32(h, r.moveState);
    h = fnvI32(h, r.pathStep);
    h = fnvI32(h, r.pathSteps);
    return h;
}

// Per-mover GRADED digest over VISUAL fields ONLY. The refuse-to-grade gate's
// load-bearing digest: if these agree, the two arms' movers are drawn the same
// way this frame, whatever their intentions differ about.
inline uint64_t digestMoverVisual(const MoverRecord& r) {
    uint64_t h = kFnvOffset;
    h = fnvI32(h, r.id);
    h = fnvI32(h, r.team);
    h = fnvI32(h, quantize(r.px,  kPosQuantum));
    h = fnvI32(h, quantize(r.py,  kPosQuantum));
    h = fnvI32(h, quantize(r.pz,  kPosQuantum));
    h = fnvI32(h, quantize(r.rot, kAngQuantum));
    h = fnvI32(h, quantize(r.hp,  kFracQuantum));
    h = fnvI32(h, r.gesture);
    h = fnvI32(h, r.animFrame);
    h = fnvI32(h, r.status);
    return h;
}

// Per-mover STRICT digest: raw float bits, nothing quantized.
inline uint64_t digestMoverStrict(const MoverRecord& r) {
    uint64_t h = kFnvOffset;
    h = fnvI32(h, r.id);
    h = fnvI32(h, r.team);
    h = fnvF32Bits(h, r.px);
    h = fnvF32Bits(h, r.py);
    h = fnvF32Bits(h, r.pz);
    h = fnvF32Bits(h, r.rot);
    h = fnvF32Bits(h, r.hp);
    h = fnvI32(h, r.gesture);
    h = fnvI32(h, r.animFrame);
    h = fnvI32(h, r.status);
    h = fnvI32(h, r.target);
    h = fnvI32(h, r.brainState);
    h = fnvI32(h, r.moveState);
    h = fnvI32(h, r.pathStep);
    h = fnvI32(h, r.pathSteps);
    return h;
}

// Header folded into every world digest: schema + quanta + population count.
// Folding the quanta means a digest can never compare equal across a quantum
// change; folding the count means a vanished mover changes the digest even if
// every survivor is identical.
inline uint64_t foldHeader(uint64_t h, size_t n) {
    h = fnvI32(h, (int32_t)kSchemaVersion);
    h = fnvI32(h, (int32_t)n);
    h = fnvI32(h, quantize(kPosQuantum,  1.0f / 1048576.0f));
    h = fnvI32(h, quantize(kAngQuantum,  1.0f / 1048576.0f));
    h = fnvI32(h, quantize(kFracQuantum, 1.0f / 1048576.0f));
    return h;
}

enum class DigestKind { DIGEST_GRADED = 0, DIGEST_VISUAL = 1, DIGEST_STRICT = 2 };
using enum DigestKind;

// World digest. SORTS `recs` in place (caller's buffer is scratch) so the
// result is invariant to the order movers were walked in.
inline uint64_t digestWorld(MoverRecord* recs, size_t n, DigestKind kind) {
    sortRecords(recs, n);
    uint64_t h = foldHeader(kFnvOffset, n);
    h = fnvI32(h, (int32_t)kind);
    for (size_t i = 0; i < n; ++i) {
        uint64_t m = (kind == DIGEST_STRICT) ? digestMoverStrict(recs[i])
                   : (kind == DIGEST_VISUAL) ? digestMoverVisual(recs[i])
                                             : digestMoverGraded(recs[i]);
        for (int b = 0; b < 8; ++b)
            h = fnvByte(h, (uint8_t)((m >> (b * 8)) & 0xFFu));
    }
    return h;
}

// 16-char lowercase hex, zero-padded. Fixed width so log lines column-align and
// a truncated write is obvious.
inline void toHex16(uint64_t v, char out[17]) {
    static const char* d = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) { out[i] = d[v & 0xFu]; v >>= 4; }
    out[16] = '\0';
}

} // namespace mc2_sim_identity
