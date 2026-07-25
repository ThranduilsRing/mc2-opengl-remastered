// tools/render_state_contract_harness/gos_counter_stubs.cpp
// CONTRACT-HARNESS-REPAIR-1
//
// mclib/render_contract.cpp exposes two PUBLIC apply-state counters
// (FRAMEGRAPH-APPLY-STATE-ISLAND-1 / PER-PASS-APPLY-COUNTERS-1) as thin
// forwarders onto *_impl symbols that are DEFINED ONE LAYER UP, in
// GameOS/gameos/gos_postprocess.cpp:
//
//     mc2_framegraph_executor_apply_state_passes_impl()
//     mc2_framegraph_executor_apply_state_by_pass_impl(unsigned)
//
// In the game build both TUs link and this resolves. Any STANDALONE link of
// render_contract.cpp does not pull in gos_postprocess.cpp (a ~7k-line TU with
// the whole renderer behind it), so the forwarders become unresolved externals.
// That is what broke this harness at link time (LNK2019 x2) — it is a layering
// seam in production code, not a bug in the contract logic under test.
//
// This harness exercises the render-state SCOPE and ORDER paths; it never reads
// the apply-state counters (grep: no `apply_state` reference in
// render_state_contract_harness.cpp). So the correct fix at harness scope is the
// project's established pattern for unlinked upper-layer symbols — a stub TU,
// same shape as tools/asset_viewer/engine_stubs.cpp,
// tools/tg_import_dump/stubs.cpp and tools/brain_dispatch_harness/stubs/.
//
// If a future harness test DOES assert on these counters, replace the returns
// below with a settable fake rather than linking gos_postprocess.cpp.
//
// NOTE (deliberately not fixed here): tests/unit/CMakeLists.txt's
// test_render_contract_3 target links the same TU set and therefore carries the
// identical latent LNK2019. It is not built by the default `mc2_tests` target,
// so it is currently green by omission. Building it will need this same stub.

extern "C" unsigned long mc2_framegraph_executor_apply_state_passes_impl()
{
    return 0ul;  // no renderer linked in the harness; no passes ever applied
}

extern "C" unsigned long mc2_framegraph_executor_apply_state_by_pass_impl(unsigned /*id*/)
{
    return 0ul;
}
