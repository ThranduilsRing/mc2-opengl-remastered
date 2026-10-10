// tools/render_state_contract_harness/render_state_contract_harness.cpp
// SUBSYSTEM-HARNESS-ARC / RENDER-STATE-CONTRACT-HARNESS-1
//
// Exercises the REAL render-pass contract logic (mclib/render_contract.cpp) with
// NO GL context, NO game window, NO mission data, NO ObjectManager. It links the
// production TU directly (same as tests/unit/test_render_contract_3.cpp) and
// drives its public API — it does NOT duplicate the contract logic.
//
// Two subsystems, both pure-CPU (no GL on these paths):
//   * Pass-scope tracking (RENDER-PASS-CONTRACT-ENFORCEMENT-1): begin/end as a
//     scope stack. Detects end-without-begin, owner-mismatch, missing-end at the
//     frame boundary, and depth-cap nesting. getPassScopeViolationCount() is
//     CUMULATIVE process-wide, so every test asserts a DELTA, not an absolute.
//   * Ordering audit (CONTRACT-3): frameBegin/beginPass/endPass with a
//     per-frame getFrameViolationCount() (resets each frameBegin).
//
// Scope tracking is inert unless MC2_RENDER_PASS_CONTRACT_ASSERT/_TRACE is set;
// the harness sets ASSERT=1 before initRenderPassScope(). The order audit needs
// _test_forceOrderAudit(true) (env-independent).
//
// Build (standalone):
//   cmake -S tools/render_state_contract_harness -B build64-renderstate -G "Visual Studio 17 2022" -A x64
//   cmake --build build64-renderstate --config RelWithDebInfo --target render_state_contract_harness

#include "contract_harness.h"

#include <cstdlib>
#if defined(_WIN32)
#  include <stdlib.h>   // _putenv_s
#endif

#include "render_contract.h"
#include "RenderCore/RenderPassContract.h"
#include "RenderCore/RenderResourceRegistry.h"
#include "Gates/gate_cold.h"   // invalidateAll() after _putenv

using namespace contract_harness;
namespace rc = render_contract;
using PI  = render_contract::PassIdentity;
using RPI = RenderCore::RenderPassId;

// ---- scope-subsystem tests (delta on cumulative violation counter) ---------

// Balanced begin/end produces no violation.
static bool test_balanced_begin_end(TestCtx& t) {
    const uint32_t before = rc::getPassScopeViolationCount();
    rc::beginPassScope(PI::PostProcess, "harness");
    rc::endPassScope(PI::PostProcess, "harness");
    CH_CHECK(t, rc::getPassScopeViolationCount() - before == 0);
    return t.failures == 0;
}

// end without a matching begin is detected (+1).
static bool test_end_without_begin(TestCtx& t) {
    const uint32_t before = rc::getPassScopeViolationCount();
    rc::endPassScope(PI::UI, "harness");   // nothing open
    CH_CHECK(t, rc::getPassScopeViolationCount() - before == 1);
    return t.failures == 0;
}

// Closing a pass that is not the innermost open scope is an owner mismatch (+1).
// Recovery pops the innermost so the stack still drains.
static bool test_owner_mismatch(TestCtx& t) {
    const uint32_t before = rc::getPassScopeViolationCount();
    rc::beginPassScope(PI::TerrainOverlay, "outer");
    rc::endPassScope(PI::PostProcess, "wrong-owner");   // innermost is TerrainOverlay
    CH_CHECK(t, rc::getPassScopeViolationCount() - before == 1);
    rc::renderPassScopeFrameBoundary();   // drain any residue, isolate next test
    return t.failures == 0;
}

// Properly nested scopes (within the depth cap) produce no violation.
static bool test_nested_scopes_ok(TestCtx& t) {
    const uint32_t before = rc::getPassScopeViolationCount();
    rc::beginPassScope(PI::TerrainOverlay, "outer");
    rc::beginPassScope(PI::TerrainDecal, "inner");
    rc::endPassScope(PI::TerrainDecal, "inner");
    rc::endPassScope(PI::TerrainOverlay, "outer");
    CH_CHECK(t, rc::getPassScopeViolationCount() - before == 0);
    return t.failures == 0;
}

// A scope still open at the frame boundary is reported as missing-end (+>=1).
static bool test_missing_end_at_frame_boundary(TestCtx& t) {
    const uint32_t before = rc::getPassScopeViolationCount();
    rc::beginPassScope(PI::PostProcess, "leaked");
    rc::renderPassScopeFrameBoundary();   // flushes open scopes as violations + resets
    CH_CHECK(t, rc::getPassScopeViolationCount() - before >= 1);
    return t.failures == 0;
}

// The cumulative counter strictly increases across a known violation.
static bool test_violation_counter_increments(TestCtx& t) {
    const uint32_t before = rc::getPassScopeViolationCount();
    rc::endPassScope(PI::DebugOverlay, "underflow");
    CH_CHECK(t, rc::getPassScopeViolationCount() > before);
    return t.failures == 0;
}

// ---- ordering-audit subsystem (CONTRACT-3, per-frame count) -----------------

// Correct dependency order: no ordering violation this frame.
static bool test_order_correct_no_violation(TestCtx& t) {
    rc::frameBegin();
    rc::beginPass(RPI::Shadow);     rc::endPass(RPI::Shadow);
    rc::beginPass(RPI::MechOpaque); rc::endPass(RPI::MechOpaque);
    CH_CHECK(t, rc::getFrameViolationCount() == 0);
    return t.failures == 0;
}

// MechOpaque reads ShadowDynamicMap before Shadow wrote it: one ordering violation.
static bool test_order_missing_writer_violation(TestCtx& t) {
    rc::frameBegin();
    rc::beginPass(RPI::MechOpaque); rc::endPass(RPI::MechOpaque);
    CH_CHECK(t, rc::getFrameViolationCount() == 1);
    return t.failures == 0;
}

static void enableScopeTracking() {
#if defined(_WIN32)
    _putenv_s("MC2_RENDER_PASS_CONTRACT_ASSERT", "1");
#else
    setenv("MC2_RENDER_PASS_CONTRACT_ASSERT", "1", 1);
#endif
    mc2::gate::invalidateAll();         // gates cached by static initialisers predate the _putenv above
    rc::initRenderPassScope();          // reads the env we just set
    rc::initRenderPassOrder();
    rc::_test_forceOrderAudit(true);    // run order audit without MC2_RENDER_PASS_ORDER
}

int main(int argc, char** argv) {
    mc2::gate::markMainEntered();   // gate reads resolved before this line count as static-init (pre-main) reads
    enableScopeTracking();

    Harness h("render_state_contract_harness");
    // scope subsystem
    h.add("balanced_begin_end",            test_balanced_begin_end);
    h.add("end_without_begin",             test_end_without_begin);
    h.add("owner_mismatch",                test_owner_mismatch);
    h.add("nested_scopes_ok",              test_nested_scopes_ok);
    h.add("missing_end_at_frame_boundary", test_missing_end_at_frame_boundary);
    h.add("violation_counter_increments",  test_violation_counter_increments);
    // ordering-audit subsystem
    h.add("order_correct_no_violation",    test_order_correct_no_violation);
    h.add("order_missing_writer_violation", test_order_missing_writer_violation);
    return h.run(argc, argv);
}
