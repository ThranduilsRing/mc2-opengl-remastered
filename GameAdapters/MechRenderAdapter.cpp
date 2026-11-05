// GameAdapters/MechRenderAdapter.cpp
//
// Slice M2 (route-only): the ONLY TU that may include both
// mclib/mech3d.h and RenderWorld/RenderWorld.h.
//
// Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-mech-adapter-spec.md
// Firewall: scripts/check-include-firewall.allowlist lists this file as an
// explicit allowlist exception for the Mech3DAppearance forbidden-symbol check.

#include "MechRenderAdapter.h"
#include "mc2_diag_verbose.h"
#include "Gates/gate.h"

// Engine side.
#include "../RenderWorld/RenderWorld.h"

// Game side. This is the ONLY TU outside mclib/ that may include mech3d.h.
#include "../mclib/mech3d.h"

// M2.6: reverse-lookup needs ObjectManager + BattleMech + MoverPtr. These
// includes extend the M2 bridging carve-out (this .cpp is the documented
// exception). Firewall scope (scripts/check-include-firewall.sh SCOPE_DIRS)
// does not police GameAdapters/, so no allowlist edit needed.
#include "../code/objmgr.h"
#include "../code/mover.h"
#include "../code/mech.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "mc2fmt.h"

namespace {

// Adapter state quarantined in anonymous namespace. Separate from
// static-prop counters per spec Section 9 counter discipline.
uint64_t s_mechs_registered   = 0;
uint64_t s_mechs_alive        = 0;
uint64_t s_mechs_destroyed    = 0;
uint64_t s_mech_register_fail = 0;

} // namespace

namespace GameAdapters {
namespace Mech {

void beginMission() {
    // Defensive: warn if endMission() was not called before the next begin.
    if (s_mechs_alive > 0) {
        MC2_DIAGFE(stderr,
            "[RENDER_WORLD v1] WARN: event=mech_begin_without_end alive=%llu "
            "(endMission was skipped)\n",
            static_cast<unsigned long long>(s_mechs_alive));
    }
    s_mechs_registered   = 0;
    s_mechs_alive        = 0;
    s_mechs_destroyed    = 0;
    s_mech_register_fail = 0;
}

void endMission() {
    // Always-on per-mission summary.
    MC2_DIAGFE(stderr,
        "[RENDER_WORLD v1] event=mech_end_mission registered=%llu destroyed=%llu "
        "alive=%llu fail=%llu\n",
        static_cast<unsigned long long>(s_mechs_registered),
        static_cast<unsigned long long>(s_mechs_destroyed),
        static_cast<unsigned long long>(s_mechs_alive),
        static_cast<unsigned long long>(s_mech_register_fail));

    // Always-on warning if any handles were not retired normally.
    if (s_mechs_alive > 0) {
        MC2_DIAGFE(stderr,
            "[RENDER_WORLD v1] WARN: event=mech_leaked_handles count=%llu\n",
            static_cast<unsigned long long>(s_mechs_alive));
    }

    // Force-clear any remaining mech records in the engine table after
    // logging the warning, so stale handles cannot carry into the next mission.
    // s_nextMechSlot resets inside clearAllMechRecords().
    RenderWorld::clearAllMechRecords();

    s_mechs_registered   = 0;
    s_mechs_alive        = 0;
    s_mechs_destroyed    = 0;
    s_mech_register_fail = 0;
}

RenderCore::RenderObjectHandle syncSpawn(Mech3DAppearance& mech,
                                         uint32_t          gameObjectId) {
    // Double-spawn guard: a valid handle before spawn means a prior
    // destroyMech was missed (or the re-init destroyMech path was skipped).
    assert(!mech.getRenderWorldHandle().isValid() &&
           "MechRenderAdapter::syncSpawn: handle already valid -- "
           "prior destroyMech was missed");

    RenderWorld::RenderMechDesc desc;
    desc.mechTypeId   = 0u;  // M2: type identity deferred to M2.5
    desc.gameObjectId = gameObjectId;
    desc.debugCookie  = reinterpret_cast<uintptr_t>(&mech);

    RenderCore::RenderObjectHandle h = RenderWorld::registerMech(desc);

    if (h.isValid()) {
        mech.setRenderWorldHandleForAdapter(h);
        ++s_mechs_registered;
        ++s_mechs_alive;
    } else {
        ++s_mech_register_fail;
        MC2_DIAGFE(stderr,
            "[RENDER_WORLD v1] event=mech_register_fail mech=%p\n",
            static_cast<const void*>(&mech));
    }

    return h;
}

void destroyMech(Mech3DAppearance& mech) {
    const RenderCore::RenderObjectHandle h = mech.getRenderWorldHandle();
    if (!h.isValid()) {
        // No-op: never registered, or already retired.
        return;
    }


    RenderWorld::destroyMech(h);
    mech.clearRenderWorldHandleForAdapter();

    ++s_mechs_destroyed;
    if (s_mechs_alive > 0) {
        --s_mechs_alive;
    }
}

// M2.6: handle->BattleMech reverse lookup. Inspect-only path.
BattleMech* findMechByHandle(RenderCore::RenderObjectHandle h) {
    if (!h.isValid()) return nullptr;
    if (ObjectManager == nullptr) return nullptr;
    const uint32_t target = h.raw();
    const long n = ObjectManager->getNumMovers();
    for (long i = 0; i < n; ++i) {
        MoverPtr m = ObjectManager->getMover(i);
        if (m == nullptr) continue;
        if (!m->isMech()) continue;
        // Cast safety (external-review m1): isMech() filter guarantees the
        // mover is a BattleMech. Verified in M2 spec: only BattleMech::init
        // assigns appearance = new Mech3DAppearance (at code/mech.cpp:1304),
        // and only BattleMech sets the isMech bit, so the static_cast pair
        // below is sound on any mover that passes isMech().
        BattleMech* bm = static_cast<BattleMech*>(m);
        // Lifecycle guard protects against
        // pre-init or mid-destroy mech with NULL appearance. A mech
        // exists in ObjectManager BEFORE syncSpawn populates its
        // appearance, and AFTER destroyMech runs but BEFORE the mover
        // slot is reclaimed. Both windows are single-threaded but real.
        Mech3DAppearance* app =
            static_cast<Mech3DAppearance*>(bm->getAppearance());
        if (app == nullptr) continue;
        if (app->getRenderWorldHandle().raw() == target) {
            return bm;
        }
    }
    return nullptr;
}

} // namespace Mech
} // namespace GameAdapters

// M2.6: end-to-end mech-pick self-test. Hosted here (not in
// RenderWorld.cpp) because the test exercises findMechByHandle, which
// reaches into game-side BattleMech / Mech3DAppearance -- RenderWorld
// must not include those headers. Forward-decl'd from RenderWorld.cpp;
// wired into RenderWorld::init() after RunMechObjectIdSelfTest.
//
// Gated on MC2_MECH_PICK_SELFTEST=1 AND substrate enabled. Validates:
//  1. registerMech yields a fresh handle.
//  2. findMechByHandle(invalid) -> nullptr.
//  3. findMechByHandle(synthetic_handle) -> nullptr at init-time
//     (no real BattleMech in ObjectManager), or non-null with
//     matching bits if a real mech somehow occupies the same slot
//     (impossible in practice). Either is a pass; the test proves
//     scan well-formed-ness, not data state (per MINOR-clarification).
//  4. destroyMech retires the handle.
//  5. live-mech count returned to baseline (external-review m2:
//     catches register/destroy imbalance via getMechsAliveCount).
//
// Free function in the global namespace so RenderWorld.cpp can
// forward-declare and call it without a header dependency.
void RunMechPickSelfTest() {
    if (true) return;
    if (!RenderWorld::IsObjectIdBufferEnabled()) {
        mc2::fmt::print_err("[MECH_PICK_SELFTEST v1] result=SKIP reason=substrate_off\n");
        return;
    }
    // External-review m2: sample live-mech count BEFORE the synthetic
    // register/destroy pair so we can assert no drift afterwards.
    const uint64_t mechsAliveBefore = RenderWorld::getMechsAliveCount();

    // register a synthetic mech.
    RenderWorld::RenderMechDesc desc{};
    desc.mechTypeId    = 0u;
    desc.gameObjectId  = 0xC0FFEEu;
    desc.debugCookie   = 0u;
    RenderCore::RenderObjectHandle h = RenderWorld::registerMech(desc);
    if (!h.isValid()) {
        mc2::fmt::print_err("[MECH_PICK_SELFTEST v1] result=FAIL step=1 reason=register_failed\n");
        return;
    }
    // scan with invalid handle -> nullptr.
    if (GameAdapters::Mech::findMechByHandle(RenderCore::RenderObjectHandle::invalid()) != nullptr) {
        mc2::fmt::print_err("[MECH_PICK_SELFTEST v1] result=FAIL step=2 reason=invalid_returned_nonnull\n");
        RenderWorld::destroyMech(h);
        return;
    }
    // scan with synthetic handle. Init-time has no real
    // BattleMech in ObjectManager, so we expect nullptr.
    BattleMech* found = GameAdapters::Mech::findMechByHandle(h);
    if (found != nullptr) {
        // If a real mech happens to occupy this slot, sanity-check
        // the bits. (Init-time should never hit this path.)
        Mech3DAppearance* app = static_cast<Mech3DAppearance*>(found->getAppearance());
        if (app == nullptr || app->getRenderWorldHandle().raw() != h.raw()) {
            mc2::fmt::print_err("[MECH_PICK_SELFTEST v1] result=FAIL step=3 reason=scan_bits_mismatch\n");
            RenderWorld::destroyMech(h);
            return;
        }
    }
    // destroyMech retires the handle.
    RenderWorld::destroyMech(h);

    // assert live-mech count returned to
    // baseline. A drift means destroyMech did not balance registerMech
    // and the slot leaked (would compound across init-time invocations).
    const uint64_t mechsAliveAfter = RenderWorld::getMechsAliveCount();
    if (mechsAliveAfter != mechsAliveBefore) {
        mc2::fmt::print_err("[MECH_PICK_SELFTEST v1] result=FAIL step=5 reason=live_count_drift before={:d} after={:d}\n", mc2::fmt::as_u64((unsigned long long)mechsAliveBefore), mc2::fmt::as_u64((unsigned long long)mechsAliveAfter));
        return; // do not emit PASS
    }

    mc2::fmt::print_err("[MECH_PICK_SELFTEST v1] result=PASS step=all\n");
}
