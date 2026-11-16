// RenderWorld/RenderWorld.cpp
// M1 forwards static props to GpuStaticPropRegistry::registerRecipe.
// Engine and game adapters translate int32_t(-1) <-> invalid handles at
// their boundaries (spec Section 10 amendment 2026-05-22).

#include "RenderWorld.h"
#include "ScreenPick.h"
#include "mc2_diag_verbose.h"
#include "Gates/gate.h"
#include "RenderWorld_internal.h"

// C1 fix: this TU MUST NOT include gos_static_prop_batcher.h (it
// transitively pulls GL + Stuff). All translation between
// StaticPropInstanceDesc <-> GpuStaticPropInstance and all calls into
// GpuStaticPropRegistry::registerRecipe live in
// RenderWorld/legacy/static_prop_backend.cpp.
#include "legacy/static_prop_backend.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "../GameOS/gameos/gos_postprocess.h"
#include "../GameOS/gameos/vk_scene_readback.h"
#include "../GameOS/gameos/view_registry.h"

// M2-pre: forward-declare the gameplay-pick self-test entry point.
// Lives in code/gameplay_pick.h; full include would drag the game-side
// types into a substrate-side TU unnecessarily. Symbol is resolved by
// the linker at full-relink.
// Firewall direction note: this is the FIRST engine-side -> game-side
// reach in the codebase. scripts/check-include-firewall.sh:22 SCOPE_DIRS
// excludes code/ AND does NOT enforce reach direction; the forward-decl
// avoids including code/gameplay_pick.h from a substrate-side TU. If the
// firewall script is ever extended to enforce direction, this site will
// need an explicit allowlist entry.
void RunGameplayPickSelfTest();
// M2.5 (Q1): separate mech-substrate self-test. Mirrors the M2-pre
// precedent ([GAMEPLAY_PICK_SELFTEST v1] separate from
// [RENDER_WORLD_SELFTEST v1]). Validates that registerMech allocates
// a Mech-kind handle and destroyMech bumps the generation correctly.
void RunMechObjectIdSelfTest();
// M2.6: end-to-end mech-pick self-test. Hosted in
// GameAdapters/MechRenderAdapter.cpp because the test reaches into
// game-side BattleMech via findMechByHandle (RenderWorld must not
// include code/mech.h). Forward-decl matches existing pattern; the
// definition lives in the adapter TU.
void RunMechPickSelfTest();

namespace RenderWorld { namespace detail {
std::mutex objectRecordsMutex;
std::vector<RenderObjectRecord> objectRecords;
std::atomic<uint64_t> mechsAlive{0};
} }

namespace {

// Anonymous-namespace state per Decision D3.A. Adapter does NOT see
// these; the only public surface is RenderWorld:: free functions.
std::atomic<uint64_t> s_upsertOk{0};
std::atomic<uint64_t> s_upsertFail{0};
std::atomic<uint64_t> s_destroyCalls{0};
std::atomic<uint64_t> s_markVisibleCalls{0};
uint32_t s_staticPropRecordEnd = 0; // Guarded by s_objectRecordsMutex; excludes mech storage.
std::atomic<uint64_t> s_frameCounter{0};

// M1.5: cached env-flag accessor. First call reads MC2_OBJECT_ID_BUFFER
// from getenv(); subsequent calls return the cached bool. Default ON --
// set MC2_OBJECT_ID_BUFFER=0 to disable.
bool readObjectIdBufferEnv() {
    return mc2::gate::get<"MC2_OBJECT_ID_BUFFER">();
}

uint32_t recipeIndexToHandleIndex(int32_t r) {
    // M1: generation is always 1 (no slot recycle yet). Index is the
    // raw recipe slot. -1 -> invalid (index=0, generation=0).
    if (r < 0) return 0;
    // 20-bit clamp; assert if registry ever overflows (it cannot under
    // current configuration; this is a future-proofing guard).
    return static_cast<uint32_t>(r) & 0x000FFFFFu;
}

int32_t handleToRecipeIndex(RenderCore::RenderObjectHandle h) {
    if (!h.isValid()) return -1;
    return static_cast<int32_t>(h.index());
}

// M1.5: per-slot inspection table. Always populated (M1 decision);
// ~127 KB peak at tier1 mc2_24 = 2641 props (48 bytes/record with M2
// kind+debugCookie fields). Indexed by handle.index(); resized lazily
// on upsert. mutex guards resize + write; reads in lookupAtPixel
// acquire the same lock (cheap, click-rate).
std::mutex&                                  s_objectRecordsMutex = RenderWorld::detail::objectRecordsMutex;
std::vector<RenderWorld::RenderObjectRecord>& s_objectRecords = RenderWorld::detail::objectRecords;

// M2.6 (META-FIX of the M1.6 per-kind selection slot): most-recent gameplay
// pick debug state. Single mutex-guarded slot updated by
// setLastGameplayPick from both the static-prop and mech callers.
// Mutex-guarded because get/set may interleave on a future off-thread
// HUD consumer; today all callers are main-thread.
//
// Spec: 2026-05-23-renderworld-slice-m2-6-mech-pickup-spec.md sec 4.3.
std::mutex                                       s_lastGameplayPickMutex;
RenderWorld::GameplaySelectionDebugState         s_lastGameplayPick;

// M2: engine-side mech counters. Separate from the adapter-side counters
// (adapter lives in MechRenderAdapter.cpp). s_mechs_alive_rw is sourced
// from the registry record table (not adapter delta) so the banner
// stays honest even if teardown paths skip the adapter.
static std::atomic<uint64_t>& s_mechs_alive_rw = RenderWorld::detail::mechsAlive;

using RenderWorld::detail::kMechHandleBase;

// kTerrainHandleBase: M3 v1 reservation (2026-05-24). RESERVED — no code
// allocates from this base in v1. If/when a future M3.1 implementation
// slice ships per-quad terrain identity (editor-driven; see resolutions
// sidecar), this is the base. Range [0x00070000, 0x0007FFFF] reserves
// 65536 slots for a future terrain scheme (v1 has no terrain writer). Leaves
// [0x00040000..0x0006FFFF] (196608 slots) as mech-expansion headroom.
// Tripwire-protected: see lookupAtPixel below. Spec:
// docs/superpowers/specs/2026-05-23-renderworld-slice-m3-terrain-spec.md.
using RenderWorld::detail::kTerrainHandleBase;

// M4: VFX handle base. RESERVED PER SPEC; NO WRITER ISSUES HANDLES IN v1.
//
// VFX shaders are PROHIBITED from writing color-attachment-2 (the M1.5
// R32_UINT objectID substrate). Integer color attachments do not blend
// (GL 4.5 §17.3.6) — additive/alpha-blended particles would last-write-wins
// clobber any mech/static-prop ID underneath, silently breaking M2.6
// mech-pick on any mech occluded by a translucent particle. The
// prohibition is enforced by scripts/check-vfx-no-objectid.sh.
//
// This constant exists ONLY for handle-range allocation bookkeeping so a
// future slice (if any) that opts in to per-emitter handles has a stable
// reserved base to use. The current recon recommends "never use this."
//
// Full corrected partitioning (do NOT re-derive; see migration guide §12):
//   StaticProp:  0x00000..0x3FFFF   (262144 slots; allocation refuses above)
//   Mech:        0x40000..0x6FFFF   (196608 slots, unchanged capacity)
//   Terrain:     0x70000..0x7FFFF   (reserved by M3; no writer in v1)
//   Vfx:         0x80000..0xBFFFF   (this slot; PROHIBITED writers per §3.6)
//   Overlay:     0xC0000..0xFFFFE   (reserved/deferred per M5 sidecar)
//   Sentinel:    0xFFFFF            (bug-bait; never allocate)
//
// 20-bit index mask (RenderCore/Handle.h:34) limits the absolute max to
// 0xFFFFF (1,048,575). A prior draft proposed 0x200000 — that OVERFLOWS
// the mask and would silently truncate to index=0, colliding with
// static-prop slot 0. Do not repeat that trap.
[[maybe_unused]] static constexpr uint32_t kVfxHandleBase = 0x00080000u;

void populateRecord(uint32_t handleIndex,
                    uint16_t generation,
                    uint32_t gameObjectId)
{
    std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
    if (handleIndex >= s_objectRecords.size()) {
        // Grow with headroom; doubling beyond demand to amortize.
        const size_t want = static_cast<size_t>(handleIndex) + 1;
        const size_t cap  = (want * 3) / 2 + 16;
        s_objectRecords.resize(cap);
    }
    auto& rec = s_objectRecords[handleIndex];
    if (handleIndex >= s_staticPropRecordEnd)
        s_staticPropRecordEnd = handleIndex + 1;
    rec.generation         = generation;
    rec.flags              = RenderWorld::kRenderObjectFlagAlive;
    rec.meshHandleBits     = 0;           // M1.5: unknown (no MeshHandle producer)
    rec.materialHandleBits = 0;           // M1.5: unknown
    rec.lodLevel           = 0xFFu;       // M1.5: unknown
    rec.pipelineId         = 0;           // M1.5 sentinel
    rec.drawPacketIndex    = 0xFFFFFFFFu; // M1.5 sentinel
    rec.pathReasonCode     = 0;           // M1.5 sentinel
    rec.gameObjectId       = gameObjectId;
    rec.kind               = RenderWorld::RenderObjectKind::StaticProp;
    rec.debugCookie        = 0;
}

void retireRecord(uint32_t handleIndex)
{
    std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
    if (handleIndex >= s_objectRecords.size()) return;
    auto& rec = s_objectRecords[handleIndex];
    rec.flags &= static_cast<uint16_t>(~RenderWorld::kRenderObjectFlagAlive);
    // Bump generation so the next upsert at this index produces a
    // distinct handle and stale pixels read back as invalid via the
    // generation check in lookupAtPixel.
    rec.generation = static_cast<uint16_t>(rec.generation + 1u);
}

// M1.5 T9 / OBJECT-ID-1 (OBJECT_ID_PASSIVE_CANARY): real-rendering validator, one passive log line at canary
// frame N under MC2_DIAG_VERBOSE. Samples a grid over the output, expects at least one valid
// static-prop hit and at least one background miss, validates every valid handle against s_objectRecords
// (generation match + alive), and proves recycled-handle rejection: a handle with the generation bumped must be
// refused by getObjectRecordView (the record table is not mutated).
bool s_canaryRan = false;

void runPassiveStableFrameCanary() {
    if (s_canaryRan) return;
    s_canaryRan = true;
    if (!RenderWorld::IsObjectIdBufferEnabled()) {
        std::fprintf(stderr,
            "[OBJECT_ID_SELFTEST v1] mode=passive_stable_frame result=SKIPPED reason=env_disabled\n");
        return;
    }

    gosPostProcess* pp = getGosPostProcess();
    if (!pp) {
        std::fprintf(stderr,
            "[OBJECT_ID_SELFTEST v1] mode=passive_stable_frame result=SKIPPED reason=no_postprocess\n");
        return;
    }

    const int ow = pp->getOutputWidth();
    const int oh = pp->getOutputHeight();
    int sampled = 0, validHits = 0, invalidHits = 0, backgroundMisses = 0;
    bool genMismatch = false, deadSlot = false;
    bool staleChecked = false, staleRejected = false;
    uint32_t firstRaw = 0u, firstGameObjectId = 0u;
    int firstKind = -1;
    for (int gy = 0; gy < 6; ++gy) {
        for (int gx = 0; gx < 8; ++gx) {
            const int px = (ow * (2 * gx + 1)) / 16;
            const int py = (oh * (2 * gy + 1)) / 12;
            RenderWorld::LookupResult res = RenderWorld::lookupAtPixel(px, py);
            ++sampled;
            if (res.isValid) {
                ++validHits;
                if (firstRaw == 0u) {
                    firstRaw = res.rawObjectId;
                    firstGameObjectId = res.gameObjectId;
                    firstKind = static_cast<int>(res.kind);
                }
                std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
                if (res.handle.index() < s_objectRecords.size()) {
                    const auto& rec = s_objectRecords[res.handle.index()];
                    if (rec.generation != static_cast<uint16_t>(res.handle.generation())) genMismatch = true;
                    if ((rec.flags & RenderWorld::kRenderObjectFlagAlive) == 0u) deadSlot = true;
                }
            } else {
                ++invalidHits;
                if (res.rawObjectId == 0u) ++backgroundMisses;
            }
        }
    }
    if (firstRaw != 0u) {
        RenderCore::RenderObjectHandle live;
        live.bits = firstRaw;
        RenderCore::RenderObjectHandle stale = RenderCore::RenderObjectHandle::make(live.index(), live.generation() + 1u);
        RenderWorld::ObjectRecordView view;
        staleChecked = true;
        // The live handle resolves, the same index with a bumped generation (a recycled slot) must not.
        staleRejected = RenderWorld::getObjectRecordView(live, &view) && !RenderWorld::getObjectRecordView(stale, &view);
    }

    const char* result;
    if (genMismatch) {
        result = "GENERATION_MISMATCH";
    } else if (deadSlot) {
        result = "DEAD_SLOT_HIT";
    } else if (validHits == 0) {
        result = "NO_STATIC_PROP_HIT";
    } else if (backgroundMisses == 0) {
        result = "NO_BACKGROUND_MISS";
    } else if (!staleChecked || !staleRejected) {
        result = "STALE_HANDLE_ACCEPTED";
    } else {
        result = "PASS";
    }

    std::fprintf(stderr,
        "[OBJECT_ID_SELFTEST v1] mode=passive_stable_frame result=%s "
        "sampled=%d valid_hits=%d invalid_hits=%d background_misses=%d stale_rejected=%d "
        "first_raw=0x%08X first_kind=%d first_gameObjectId=%u\n",
        result, sampled, validHits, invalidHits, backgroundMisses, staleRejected ? 1 : 0,
        firstRaw, firstKind, firstGameObjectId);
}

// M1.5 T10: substrate self-test. Exercises the record table directly
// (no rendering required): synthesize handle, populate record, retire,
// validate alive=false and generation bump. Gated by
// MC2_RENDER_WORLD_SELFTEST=1; runs once at RenderWorld::init.
//
// Result lines:
//   [RENDER_WORLD_SELFTEST v1] result=PASS step=N
//   [RENDER_WORLD_SELFTEST v1] result=FAIL step=N reason=<...>
//
// FAIL is a STOP: indicates the record table is corrupt or the
// generation-bump logic is broken.
void runSubstrateSelfTest() {
    if (true) return;

    // Use a high handle index unlikely to collide with real registrations.
    const uint32_t kTestIndex = 0xFFFFEu;  // 20-bit max minus one
    // populate
    populateRecord(kTestIndex, 1u, 0xCAFEu);

    {
        std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
        if (kTestIndex >= s_objectRecords.size()) {
            std::fprintf(stderr,
                "[RENDER_WORLD_SELFTEST v1] result=FAIL step=1 reason=resize_failed\n");
            return;
        }
        const auto& rec = s_objectRecords[kTestIndex];
        if ((rec.flags & RenderWorld::kRenderObjectFlagAlive) == 0u) {
            std::fprintf(stderr,
                "[RENDER_WORLD_SELFTEST v1] result=FAIL step=1 reason=alive_not_set_after_populate\n");
            return;
        }
        if (rec.generation != 1u) {
            std::fprintf(stderr,
                "[RENDER_WORLD_SELFTEST v1] result=FAIL step=1 reason=wrong_generation_%u\n",
                static_cast<unsigned>(rec.generation));
            return;
        }
        if (rec.gameObjectId != 0xCAFEu) {
            std::fprintf(stderr,
                "[RENDER_WORLD_SELFTEST v1] result=FAIL step=1 reason=gameObjectId_lost\n");
            return;
        }
    }

    // retire
    retireRecord(kTestIndex);

    {
        std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
        const auto& rec = s_objectRecords[kTestIndex];
        if ((rec.flags & RenderWorld::kRenderObjectFlagAlive) != 0u) {
            std::fprintf(stderr,
                "[RENDER_WORLD_SELFTEST v1] result=FAIL step=2 reason=alive_set_after_retire\n");
            return;
        }
        if (rec.generation != 2u) {
            std::fprintf(stderr,
                "[RENDER_WORLD_SELFTEST v1] result=FAIL step=2 reason=generation_not_bumped_%u\n",
                static_cast<unsigned>(rec.generation));
            return;
        }
    }

    // re-populate with new generation
    populateRecord(kTestIndex, 2u, 0xBEEFu);

    {
        std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
        const auto& rec = s_objectRecords[kTestIndex];
        if ((rec.flags & RenderWorld::kRenderObjectFlagAlive) == 0u) {
            std::fprintf(stderr,
                "[RENDER_WORLD_SELFTEST v1] result=FAIL step=3 reason=alive_not_set_after_repopulate\n");
            return;
        }
        if (rec.generation != 2u) {
            std::fprintf(stderr,
                "[RENDER_WORLD_SELFTEST v1] result=FAIL step=3 reason=wrong_repopulate_generation_%u\n",
                static_cast<unsigned>(rec.generation));
            return;
        }
        if (rec.gameObjectId != 0xBEEFu) {
            std::fprintf(stderr,
                "[RENDER_WORLD_SELFTEST v1] result=FAIL step=3 reason=repopulate_gameObjectId_lost\n");
            return;
        }
    }

    // Cleanup: retire the test slot so it cannot poison subsequent
    // canary sampling. (lookupAtPixel only triggers on real pixel
    // values; index 0xFFFFE is not reachable by any real recipe.)
    retireRecord(kTestIndex);

    std::fprintf(stderr,
        "[RENDER_WORLD_SELFTEST v1] result=PASS step=all\n");
}

} // namespace

// M2.5 (Q1): mech-substrate self-test. Synthetic-only -- no GL state,
// no real readback. Exercises the M2 registerMech / destroyMech /
// s_objectRecords path that M2.5 GPU writes feed. Gated by
// MC2_MECH_OBJECT_ID_SELFTEST=1; runs once at RenderWorld::init() after
// RunGameplayPickSelfTest().
//
// Why synthetic: real GPU-readback validation requires a live mission
// frame + a known mech-on-cursor pixel; that path is exercised
// per-frame by RunGameplayPickSelfTest's mech-side extension in M2.6.
// M2.5's job is to prove the substrate (handle allocation, record kind,
// generation roundtrip) is intact -- a pure-CPU self-test suffices.
//
// Placement note: defined at file scope (not in anonymous namespace) to
// match the external-linkage forward declaration at line 45 so init()
// can call it. References to anonymous-namespace s_objectRecords /
// s_objectRecordsMutex remain valid since they are visible from the
// enclosing translation unit.
//
// Result lines:
//   [MECH_OBJECT_ID_SELFTEST v1] result=PASS step=all kind=1 gen=N handle=0xNN
//   [MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=N reason=<...>
//   [MECH_OBJECT_ID_SELFTEST v1] result=SKIPPED reason=env_disabled
//
// FAIL is a STOP: the mech substrate is broken; M2.6 pickup will be
// unreliable.
void RunMechObjectIdSelfTest() {
    // The old dedicated env switch was retired; use the existing smoke gate.
    if (!mc2::gate::get<"MC2_SMOKE_ACTIVE">()) {
        MC2_DIAGFE(stderr,
            "[MECH_OBJECT_ID_SELFTEST v1] result=SKIPPED reason=env_disabled\n");
        return;
    }

    // register a synthetic mech, validate handle + record.
    RenderWorld::RenderMechDesc desc;
    desc.mechTypeId   = 0u;
    desc.gameObjectId = 0xC0FFEEu;
    desc.debugCookie  = 0u;
    RenderCore::RenderObjectHandle h = RenderWorld::registerMech(desc);
    if (!h.isValid()) {
        MC2_DIAGFE(stderr,
            "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=1 reason=registerMech_returned_invalid\n");
        return;
    }
    const uint32_t idx = h.index();
    const uint16_t gen = static_cast<uint16_t>(h.generation());

    // Exercise the real table at the old overlap and the last valid prop slot.
    for (uint32_t propIndex : {65535u, 65536u, kMechHandleBase - 1u}) {
        RenderWorld::RenderObjectRecord saved;
        {
            std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
            saved = s_objectRecords[propIndex];
        }
        const auto prop = RenderWorld::detail::staticPropHandle(propIndex);
        populateRecord(prop.index(), 1u, propIndex);
        bool ok;
        {
            std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
            ok = s_objectRecords[propIndex].kind == RenderWorld::RenderObjectKind::StaticProp
                && s_objectRecords[idx].kind == RenderWorld::RenderObjectKind::Mech
                && s_objectRecords[idx].gameObjectId == desc.gameObjectId;
            s_objectRecords[propIndex] = saved;
        }
        if (!ok || RenderWorld::detail::staticPropHandle(kMechHandleBase).isValid()) {
            std::fprintf(stderr, "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=boundary index=%u\n", propIndex);
            RenderWorld::destroyMech(h);
            return;
        }
    }

    // validate record kind=Mech and alive=true.
    {
        std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
        if (idx >= s_objectRecords.size()) {
            MC2_DIAGFE(stderr,
                "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=2 reason=record_index_out_of_range_%u\n",
                (unsigned)idx);
            return;
        }
        const auto& rec = s_objectRecords[idx];
        if (rec.kind != RenderWorld::RenderObjectKind::Mech) {
            MC2_DIAGFE(stderr,
                "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=2 reason=wrong_kind_%u\n",
                (unsigned)rec.kind);
            RenderWorld::destroyMech(h);
            return;
        }
        if ((rec.flags & RenderWorld::kRenderObjectFlagAlive) == 0u) {
            MC2_DIAGFE(stderr,
                "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=2 reason=alive_not_set_after_register\n");
            RenderWorld::destroyMech(h);
            return;
        }
        if (rec.generation != gen) {
            MC2_DIAGFE(stderr,
                "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=2 reason=generation_mismatch_record_%u_handle_%u\n",
                (unsigned)rec.generation, (unsigned)gen);
            RenderWorld::destroyMech(h);
            return;
        }
        if (rec.gameObjectId != 0xC0FFEEu) {
            MC2_DIAGFE(stderr,
                "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=2 reason=gameObjectId_lost\n");
            RenderWorld::destroyMech(h);
            return;
        }
    }

    // Decode exactly as lookupAtPixel does, retaining both index and generation.
    const uint32_t raw = h.raw();
    const auto h2 = RenderWorld::detail::decodeObjectId(raw);
    if (h2.index() != idx || h2.generation() != gen || h2.raw() != raw) {
        MC2_DIAGFE(stderr,
            "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=3 reason=roundtrip_raw_%08X_vs_%08X\n",
            (unsigned)h2.raw(), (unsigned)raw);
        RenderWorld::destroyMech(h);
        return;
    }

    // destroyMech bumps generation and clears alive.
    RenderWorld::destroyMech(h);
    {
        std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
        const auto& rec = s_objectRecords[idx];
        if ((rec.flags & RenderWorld::kRenderObjectFlagAlive) != 0u) {
            MC2_DIAGFE(stderr,
                "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=4 reason=alive_set_after_destroy\n");
            return;
        }
        if (rec.generation != static_cast<uint16_t>(gen + 1u)) {
            MC2_DIAGFE(stderr,
                "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=4 reason=generation_not_bumped_%u\n",
                (unsigned)rec.generation);
            return;
        }
    }

    // P3 recycle: a slot retired in frame F is not handed out again until F completed; the stale handle stays refused after the reuse.
    {
        RenderWorld::advanceFrame(1000u, 998u);
        RenderCore::RenderObjectHandle a = RenderWorld::registerMech(desc);
        RenderWorld::destroyMech(a);                               // retired in frame 1000, which has not completed
        RenderCore::RenderObjectHandle b = RenderWorld::registerMech(desc);
        const bool earlyReuse = a.isValid() && b.isValid() && b.index() == a.index();
        RenderWorld::advanceFrame(1001u, 1000u);                   // frame 1000 completed
        RenderCore::RenderObjectHandle c = RenderWorld::registerMech(desc);
        const bool lateReuse = c.isValid() && c.index() == a.index();
        const bool staleRefused = c.isValid() && c.generation() != a.generation();
        RenderWorld::destroyMech(b);
        RenderWorld::destroyMech(c);
        if (earlyReuse || !lateReuse || !staleRefused) {
            MC2_DIAGFE(stderr,
                "[MECH_OBJECT_ID_SELFTEST v1] result=FAIL step=5 reason=recycle early=%d late=%d stale_refused=%d\n",
                earlyReuse ? 1 : 0, lateReuse ? 1 : 0, staleRefused ? 1 : 0);
            return;
        }
    }

    std::fprintf(stderr,
        "[MECH_OBJECT_ID_SELFTEST v1] result=PASS step=all kind=1 gen=%u handle=0x%08X\n",
        (unsigned)gen, (unsigned)raw);
}

// V1A: per-frame mech submit count from GpuMechBatcher::flush() latch.
// Forward-declared at global scope to avoid including gos_mech_batcher.h
// (pulls Stuff/tgl/mech3d). Defined in GameOS/gameos/gos_mech_batcher.cpp.
uint64_t batcher_getLastFlushSubmitCount();

namespace RenderWorld {

void init() {
    s_upsertOk.store(0);
    s_upsertFail.store(0);
    s_destroyCalls.store(0);
    s_markVisibleCalls.store(0);
    s_frameCounter.store(0);
    const bool oid = IsObjectIdBufferEnabled();
    MC2_DIAGFE(stderr, "[RENDER_WORLD v1] event=init objectid_buffer=%s\n",
                 oid ? "on" : "off");
    if (oid) {
        // Once-per-process; helps log readers correlate the banner with
        // the integer-MRT attachment lifecycle in gos_postprocess.cpp.
        MC2_DIAGFE(stderr,
            "[OBJECT_ID v1] event=enabled format=R32UI attachment=GL_COLOR_ATTACHMENT2\n");
    }
    // M1.6 + M2.6: pick-wiring banner. Always emitted; useful to log
    // readers diagnosing "why did Shift+click do nothing". M2.6 extends
    // the line with the three mech-pick gate states under the unified
    // [GAMEPLAY_PICK v1] schema name.
    MC2_DIAGFE(stderr,
                 "[GAMEPLAY_PICK v1] static_prop_enabled=%d static_prop_debug=%d "
                 "mech_enabled=%d mech_debug=%d mech_pierce_fog=%d\n",
                 IsStaticPropPickEnabled() ? 1 : 0,
                 IsStaticPropPickDebugEnabled() ? 1 : 0,
                 IsMechPickEnabled() ? 1 : 0,
                 IsMechPickDebugEnabled() ? 1 : 0,
                 IsMechPickPierceFogEnabled() ? 1 : 0);
    // M1.5 T10: substrate self-test (gated by MC2_RENDER_WORLD_SELFTEST=1).
    runSubstrateSelfTest();
    // M2-pre: gameplay-pick self-test (gated by MC2_GAMEPLAY_PICK_SELFTEST=1
    // + MC2_OBJECT_ID_BUFFER=1). Validates the extracted spine has not
    // diverged from M1.6 gate semantics. Mirrors substrate self-test shape.
    RunGameplayPickSelfTest();
    // M2.5 (Q1): mech-substrate self-test (gated by
    // MC2_MECH_OBJECT_ID_SELFTEST=1). Validates registerMech / destroyMech
    // / record-table generation + kind plumbing. Synthetic; no GL state.
    RunMechObjectIdSelfTest();
    // M2.6: mech-pick self-test (gated by MC2_MECH_PICK_SELFTEST=1).
    // Validates findMechByHandle scan well-formed-ness; hosted in
    // GameAdapters/MechRenderAdapter.cpp because it reaches into
    // game-side BattleMech (RenderWorld must not include code/mech.h).
    RunMechPickSelfTest();
}

void destroy() {
    // M1.6 Q2: pick state does not survive mission-end. Clearing here
    // prevents a stale handle from pointing into a cleared s_objectRecords
    // table -- the generation check would catch it on read, but the
    // resulting "valid=true handle=N" -> "lookup returns invalid" gap
    // would confuse anyone grepping the log post-load. Cheap; one
    // mutex-guarded scalar reset per mission end.
    clearLastGameplayPick();
    MC2_DIAGFE(stderr,
        "[RENDER_WORLD v1] event=destroy upsert_ok=%llu upsert_fail=%llu "
        "destroy_calls=%llu mark_visible=%llu\n",
        static_cast<unsigned long long>(s_upsertOk.load()),
        static_cast<unsigned long long>(s_upsertFail.load()),
        static_cast<unsigned long long>(s_destroyCalls.load()),
        static_cast<unsigned long long>(s_markVisibleCalls.load()));
}

RenderCore::RenderObjectHandle upsertStaticProp(RenderCore::StaticPropDesc desc) {
    // M1.5: capture POD fields before std::move; vector is moved out
    // but scalars survive on the moved-from object regardless, this
    // is just defensive.
    uint32_t gameObjectId = desc.gameObjectId;
    const int32_t r = legacy::registerStaticPropRecipe(std::move(desc));
    if (r < 0) {
        s_upsertFail.fetch_add(1, std::memory_order_relaxed);
        return RenderCore::RenderObjectHandle::invalid();
    }
    s_upsertOk.fetch_add(1, std::memory_order_relaxed);
    RenderCore::RenderObjectHandle h = RenderCore::RenderObjectHandle::make(
        recipeIndexToHandleIndex(r), 1u);
    // adapter doesn't thread a game-object id through syncStaticProp,
    // so echo the handle index (== recipe index) when no id was supplied.
    // This makes lookupAtPixel return a stable, nonzero gameObjectId for every
    // static prop/building with recipe index > 0. Recipe 0 edge case: index==0
    // stays 0, which is rare and acceptable (getRecipeShapeName still works).
    if (gameObjectId == 0u) gameObjectId = h.index();
    // M1.5: populate the always-on record table (mission/upsert-time
    // metadata, ~85 KB peak; M1 decision).
    populateRecord(h.index(),
                   static_cast<uint16_t>(h.generation()),
                   gameObjectId);
    return h;
}

RenderCore::RenderObjectHandle adoptStaticPropRecipe(int32_t recipeIndex) {
    // m5 fix: wrap an existing registry slot in a Handle without
    // creating a new recipe entry. Counter is incremented so
    // [RENDER_WORLD v1] objects stays honest for the late-spawn path.
    if (!detail::staticPropHandle(recipeIndex).isValid()) {
        return RenderCore::RenderObjectHandle::invalid();
    }
    s_upsertOk.fetch_add(1, std::memory_order_relaxed);
    RenderCore::RenderObjectHandle h = RenderCore::RenderObjectHandle::make(
        recipeIndexToHandleIndex(recipeIndex), 1u);
    // M1.5: late-spawn populates the record table too.
    // echo handle index as gameObjectId (same convention as upsertStaticProp).
    populateRecord(h.index(),
                   static_cast<uint16_t>(h.generation()),
                   h.index());
    return h;
}

void destroy(RenderCore::RenderObjectHandle h) {
    if (!h.isValid()) return;
    legacy::invalidateStaticProp(handleToRecipeIndex(h));
    s_destroyCalls.fetch_add(1, std::memory_order_relaxed);
    // M1.5: retire the record (clears alive flag, bumps generation).
    retireRecord(h.index());
}

void markVisible(RenderCore::RenderObjectHandle h,
                 uint32_t lightDataIndex, float extentRadius) {
    if (!h.isValid()) return;
    legacy::markVisibleStaticProp(handleToRecipeIndex(h),
                                  lightDataIndex, extentRadius);
    if (mc2DiagVerbose()) s_markVisibleCalls.fetch_add(1, std::memory_order_relaxed);
}

bool isReady(RenderCore::RenderObjectHandle h) {
    if (!h.isValid()) return false;
    return legacy::isReadyStaticProp(handleToRecipeIndex(h));
}

bool IsObjectIdBufferEnabled() {
    // Function-local static: thread-safe initialization (C++11 magic
    // statics); init runs exactly once at first call.
    static const bool s_enabled = readObjectIdBufferEnv();
    return s_enabled;
}

// GAMEADAPTERS-VISUAL-STATE-BRIDGE: expose the mech handle index base so a
// postprocess pass can classify object-ID-buffer pixels as mechs. See the
// header for the handle-opacity-exception rationale + mech-only scope.
//
// Guard the kHandleIndexMask published in the header against Handle.h drift:
// the mask must be exactly the index-field width, i.e. it round-trips through
// the index field and (mask+1) overflows out of it. If RenderCore::Handle's
// [19:0] split ever changes, these fail the build (M1: shader 0xFFFFF literal
// otherwise diverges silently).
static_assert(RenderCore::RenderObjectHandle::make(kHandleIndexMask, 0u).index() == kHandleIndexMask,
              "kHandleIndexMask must equal the RenderObjectHandle index-field width");
static_assert(RenderCore::RenderObjectHandle::make(kHandleIndexMask + 1u, 0u).index() == 0u,
              "kHandleIndexMask is not the full index-field width — Handle.h layout drifted");
static_assert(kMechHandleBase <= kHandleIndexMask,
              "kMechHandleBase must fit within the handle index field");
static_assert(kMechHandleBase >= 0x40000u && kTerrainHandleBase - kMechHandleBase >= 0x30000u,
              "static props need 4x headroom without shrinking mech capacity");
static_assert(kMechHandleBase < kTerrainHandleBase && kTerrainHandleBase < kVfxHandleBase
              && kVfxHandleBase < 0xC0000u && 0xC0000u < kHandleIndexMask,
              "reserved ranges must remain ordered below the sentinel");
uint32_t MechHandleIndexBase() {
    return kMechHandleBase;
}

// M1.6 + M2.6: master enable for the static-prop pick wiring (missiongui
// Shift+click -> lookupAtPixel -> setLastGameplayPick). Default OFF.
bool IsStaticPropPickEnabled() {
    static const bool s_enabled = mc2::gate::get<"MC2_STATIC_PROP_PICK">();
    return s_enabled;
}

// M1.6 + M2.6: verbose-log enable. Gates the
// `[GAMEPLAY_PICK v1] miss kind=StaticProp` line only; the `hit` line
// is unconditional.
bool IsStaticPropPickDebugEnabled() {
    static const bool s_enabled = false;
    return s_enabled;
}

// M2.6: cached env-flag accessors. Same lifetime/discipline as
// IsStaticPropPickEnabled / IsStaticPropPickDebugEnabled. Process-
// lifetime cached; restart required to flip.
bool IsMechPickEnabled() {
    static const bool s_enabled = false;
    return s_enabled;
}

bool IsMechPickDebugEnabled() {
    static const bool s_enabled = false;
    return s_enabled;
}

bool IsMechPickPierceFogEnabled() {
    static const bool s_enabled = false;
    return s_enabled;
}

bool IsGpuGroundPickParityEnabled() {
    static const bool s_enabled = false;
    return s_enabled;
}

uint32_t objectIdRawForStaticPropRecipe(int32_t recipeIndex) {
    // M1.5 C1: centralized Handle bit encoding for the batcher
    // producer. Mirrors the recipeIndex -> Handle convention used by
    // upsertStaticProp/adoptStaticPropRecipe (generation=1, 20-bit
    // index). Returns 0 (invalid raw) outside the static-prop range.
    return detail::staticPropHandle(recipeIndex).raw();
}

VisibilityResult queryVisibility(VisibilityRequest req) {
    // V0+V1A: reporting-only. Does not drive culling, draw submission,
    // gameplay selection, or object lifetime.
    // req.kindMask / req.layerMask present for API stability; unused in V0+V1A.
    // F1-4B: req.viewId is accepted but view resolution is intentionally deferred
    // to the caller (frameBannerTick owns resolveView + view_valid logging).
    VisibilityResult r{};
    // V0: lifecycle counts.
    r.static_props = legacy::getStaticPropActiveCount();
    r.mechs        = s_mechs_alive_rw.load(std::memory_order_relaxed);
    // V1A: per-frame visible/submitted counts from flush-entry latches.
    r.static_props_visible = legacy::getStaticPropLastFlushLiveCount();
    r.mechs_visible        = ::batcher_getLastFlushSubmitCount();
    // gpu_visible_valid stays false (V1B); gpu_visible stays 0.
    // valid flags default true (set in VisibilityResult initializer).
    return r;
}

void frameBannerTick() {
    const uint64_t f = s_frameCounter.fetch_add(1, std::memory_order_relaxed) + 1;

    // M1.5 T9: passive canary fires once at frame 60 (after mission stable).
    // Each pixel lookup waits for the GPU; keep this diagnostic off the default path.
    if (f == 60u && mc2DiagVerbose()) {
        runPassiveStableFrameCanary();
    }

    if ((f % 600u) != 0u) return;
    // m4 fix: source the active prop count from the registry's own
    // active-recipe accessor, NOT from the adapter-side delta. Adapter
    // delta drifts if the registry tombstones via paths the adapter
    // never sees; registry count is canonical.
    // F1-4B: always query with kMainSceneViewId so view resolution is exercised.
    VisibilityRequest visReq{};
    visReq.viewId = RenderCore::kMainSceneViewId;
    const VisibilityResult vis  = queryVisibility(visReq);
    const RenderCore::EngineView* view = RenderCore::resolveView(visReq.viewId);
    const int viewValid = (view != nullptr) ? 1 : 0;
    const uint64_t total        = vis.static_props + vis.mechs;
    const char* oidTok = IsObjectIdBufferEnabled() ? "on" : "off";
    MC2_DIAGFE(stderr,
        "[RENDER_WORLD v1] frame=%llu objects=%llu static_props=%llu mechs=%llu "
        "visible=0 packets=0 views=1 objectid_buffer=%s\n",
        static_cast<unsigned long long>(f), static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(vis.static_props), static_cast<unsigned long long>(vis.mechs), oidTok);
    MC2_DIAGFE(stderr,
        "[VISIBILITY v1] frame=%llu static_props=%llu mechs=%llu "
        "sp_visible=%llu sp_valid=%d "
        "mech_visible=%llu mech_valid=%d "
        "gpu_visible=%llu gpu_valid=%d "
        "terrain=deferred vfx=prohibited "
        "view_id=%u view_valid=%d\n",
        static_cast<unsigned long long>(f),
        static_cast<unsigned long long>(vis.static_props),
        static_cast<unsigned long long>(vis.mechs),
        static_cast<unsigned long long>(vis.static_props_visible), vis.static_props_visible_valid ? 1 : 0,
        static_cast<unsigned long long>(vis.mechs_visible),        vis.mechs_visible_valid        ? 1 : 0,
        static_cast<unsigned long long>(vis.static_props_gpu_visible), vis.gpu_visible_valid      ? 1 : 0,
        (unsigned)visReq.viewId, viewValid);
}

LookupResult lookupAtPixel(int screenX, int screenY) {
    LookupResult out;
    if (!IsObjectIdBufferEnabled()) {
        static bool warned = false;
        if (!warned) {
            MC2_DIAGFE(stderr,
                "[RENDER_WORLD v1] WARN: lookupAtPixel called with MC2_OBJECT_ID_BUFFER=0\n");
            warned = true;
        }
        out.lookupFailReason = "OID_BUFFER_DISABLED";
        return out;
    }
    gosPostProcess* pp = getGosPostProcess();
    if (!pp) {
        static bool warned = false;
        if (!warned) {
            MC2_DIAGFE(stderr,
                "[RENDER_WORLD v1] WARN: lookupAtPixel called before postprocess init\n");
            warned = true;
        }
        out.lookupFailReason = "NO_POSTPROCESS";
        return out;
    }
    if (!screenPickScenePixel(screenX, screenY, pp->getOutputWidth(), pp->getOutputHeight(),
                              pp->getWidth(), pp->getHeight())) {
        out.lookupFailReason = "NATIVE_READBACK_UNAVAILABLE";
        return out;
    }
    mc2vk::ScenePixel pixel;
    if (!mc2vk::readScenePixels(screenX, screenY, 1, 1,
                              mc2vk::SceneDepth | mc2vk::SceneObjectId, &pixel, 1)) {
        out.lookupFailReason = "NATIVE_READBACK_UNAVAILABLE";
        return out;
    }
    const uint32_t raw = pixel.objectId;
    const float depthSample = pixel.depth;

    // Capture raw pixel regardless of validity — inspector shows it even for failures.
    out.rawObjectId = raw;

    if (raw == 0u) {
        out.lookupFailReason = "BACKGROUND_PIXEL";
        return out;
    }

    const auto h = detail::decodeObjectId(raw);

    // Look up the record under the mutex; copy out under the lock.
    RenderObjectRecord rec;
    {
        std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
        if (h.index() >= s_objectRecords.size()) {
            out.lookupFailReason = "INDEX_OUT_OF_RANGE";
            return out;
        }
        rec = s_objectRecords[h.index()];
    }

    // Generation check: stale pixel (rendered before slot recycle)
    // returns invalid even though the raw value parses to a Handle.
    if (rec.generation != static_cast<uint16_t>(h.generation())) {
        out.lookupFailReason = "GENERATION_MISMATCH";
        return out;
    }
    if ((rec.flags & kRenderObjectFlagAlive) == 0u) {
        out.lookupFailReason = "SLOT_DEAD";
        return out;
    }

    // M3 v1 trip-wire (2026-05-24): no writer should produce a Terrain
    // record in v1 (kTerrainHandleBase is unused; no terrain frag shader
    // writes attachment-2). If we see one, an unintended writer has
    // slipped in. Log once and downgrade to invalid. Removing this branch
    // is part of the M3.1 implementation slice if it ever ships.
    if (rec.kind == RenderObjectKind::Terrain) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            MC2_DIAGFE(stderr,
                "[RENDER_WORLD v1] WARN: lookupAtPixel returned kind=Terrain but no writer should produce it (M3 reservation; M3.1 would change this)\n");
        }
        out.lookupFailReason = "TERRAIN_RESERVED";
        return out;
    }

    out.isValid            = true;
    out.handle             = h;
    out.meshHandleBits     = rec.meshHandleBits;
    out.materialHandleBits = rec.materialHandleBits;
    out.lodLevel           = rec.lodLevel;
    out.pipelineId         = rec.pipelineId;
    out.drawPacketIndex    = rec.drawPacketIndex;
    out.pathReasonCode     = rec.pathReasonCode;
    out.gameObjectId       = rec.gameObjectId;
    out.kind               = rec.kind;  // M2.6: kind discriminator

    // Unproject depth sample to world position using the inverse VP stored
    // by the render loop each frame. depthSample==0 means far plane / sky.
    if (depthSample > 0.f) {
        const float* ivp = pp->getInverseViewProj();
        const int    vw  = pp->getWidth();
        const int    vh  = pp->getHeight();
        if (ivp && vw > 0 && vh > 0) {
            // NDC in [-1,1]^3. Reverse-Z: near=1, far=0 — standard formula holds.
            const float nx = 2.f * static_cast<float>(screenX) / static_cast<float>(vw) - 1.f;
            const float ny = 2.f * static_cast<float>(screenY) / static_cast<float>(vh) - 1.f;
            const float nz = 2.f * depthSample - 1.f;
            // Column-major multiply: world_h = ivp * (nx, ny, nz, 1)
            const float wx = ivp[0]*nx + ivp[4]*ny + ivp[8]*nz  + ivp[12];
            const float wy = ivp[1]*nx + ivp[5]*ny + ivp[9]*nz  + ivp[13];
            const float wz = ivp[2]*nx + ivp[6]*ny + ivp[10]*nz + ivp[14];
            const float ww = ivp[3]*nx + ivp[7]*ny + ivp[11]*nz + ivp[15];
            if (ww != 0.f) {
                out.worldPosValid = true;
                out.worldX = wx / ww;
                out.worldY = wy / ww;
                out.worldZ = wz / ww;
            }
        }
    }
    return out;
}

bool getObjectRecordView(RenderCore::RenderObjectHandle handle,
                         ObjectRecordView* out) {
    if (!out) return false;
    *out = ObjectRecordView{};
    if (!handle.isValid()) return false;

    std::lock_guard<std::mutex> guard(s_objectRecordsMutex);
    const uint32_t idx = handle.index();
    if (idx >= s_objectRecords.size()) return false;
    const RenderObjectRecord& rec = s_objectRecords[idx];
    if (rec.generation != static_cast<uint16_t>(handle.generation())) return false;
    if (!(rec.flags & kRenderObjectFlagAlive)) return false;

    out->valid              = true;
    out->kind               = rec.kind;
    out->handle             = handle;
    out->meshHandleBits     = rec.meshHandleBits;
    out->materialHandleBits = rec.materialHandleBits;
    out->lod                = static_cast<uint32_t>(rec.lodLevel);
    out->pipelineId         = static_cast<uint32_t>(rec.pipelineId);
    out->drawPacketIndex    = rec.drawPacketIndex;
    out->pathReasonCode     = rec.pathReasonCode;
    out->gameObjId          = rec.gameObjectId;
    return true;
}

void setLastGameplayPick(RenderObjectKind kind,
                         const LookupResult& res,
                         int32_t mouseX, int32_t mouseY,
                         int32_t glX,    int32_t glY)
{
    // Callers MUST filter on res.isValid before calling. We do not assert
    // here (release-mode safety) but a misuse populates a "valid pick"
    // with an invalid handle, which the next get() consumer will see
    // and either skip or log-spam. Filter at the call site.
    //
    // M2.6: kind drives payload semantics. recipeIndex is set only when
    // kind==StaticProp; mech kind leaves recipeIndex at -1 (the handle
    // identifies the mech; callers re-resolve via findMechByHandle).
    std::lock_guard<std::mutex> lk(s_lastGameplayPickMutex);
    s_lastGameplayPick.valid              = res.isValid;
    s_lastGameplayPick.kind               = kind;
    s_lastGameplayPick.handle             = res.handle;
    s_lastGameplayPick.recipeIndex        =
        (res.isValid && kind == RenderObjectKind::StaticProp)
            ? handleToRecipeIndex(res.handle)
            : -1;
    s_lastGameplayPick.lastPickMouseX     = mouseX;
    s_lastGameplayPick.lastPickMouseY     = mouseY;
    s_lastGameplayPick.lastPickGlX        = glX;
    s_lastGameplayPick.lastPickGlY        = glY;
    s_lastGameplayPick.lastPickFrameIndex =
        s_frameCounter.load(std::memory_order_relaxed);
}

void clearLastGameplayPick() {
    std::lock_guard<std::mutex> lk(s_lastGameplayPickMutex);
    s_lastGameplayPick = GameplaySelectionDebugState{};
}

GameplaySelectionDebugState getLastGameplayPick() {
    std::lock_guard<std::mutex> lk(s_lastGameplayPickMutex);
    return s_lastGameplayPick;  // copy out; struct is tiny
}

uint64_t getMechsAliveCount() {
    return s_mechs_alive_rw.load(std::memory_order_relaxed);
}

uint32_t getStaticPropSlotCount() {
    std::lock_guard<std::mutex> lock(s_objectRecordsMutex);
    const uint32_t end = std::min<uint32_t>(s_staticPropRecordEnd, static_cast<uint32_t>(s_objectRecords.size()));
    uint32_t count = 0;
    for (uint32_t i = 0; i < end; ++i) {
        const auto& rec = s_objectRecords[i];
        if (rec.kind == RenderObjectKind::StaticProp) ++count;
    }
    return count;
}

uint32_t fillStaticPropSlots(StaticPropRecordView* out, uint32_t capacity) {
    std::lock_guard<std::mutex> lock(s_objectRecordsMutex);
    const uint32_t end = std::min<uint32_t>(s_staticPropRecordEnd, static_cast<uint32_t>(s_objectRecords.size()));
    uint32_t total   = 0;
    uint32_t written = 0;
    for (uint32_t i = 0; i < end; ++i) {
        const RenderObjectRecord& rec = s_objectRecords[i];
        if (rec.kind != RenderObjectKind::StaticProp) continue;
        ++total;
        if (written < capacity) {
            const bool alive   = (rec.flags & kRenderObjectFlagAlive) != 0;
            const int32_t ridx = alive ? static_cast<int32_t>(i) : -1;
            RenderCore::RenderObjectHandle h = alive
                ? RenderCore::RenderObjectHandle::make(i, rec.generation)
                : RenderCore::RenderObjectHandle::invalid();
            out[written++] = StaticPropRecordView{ h, ridx, alive, /*generationValid=*/alive };
        }
    }
    return total;
}

} // namespace RenderWorld
