// tools/render_state_contract_harness/gos_counter_stubs.cpp
//
// Standalone render-contract checks need the apply-state counter symbols
// without the renderer dependency. These zero-valued stubs are valid because
// this harness tests render-state scope and order, not counter values.

extern "C" unsigned long mc2_framegraph_executor_apply_state_passes_impl()
{
    return 0ul;  // no renderer linked in the harness; no passes ever applied
}

extern "C" unsigned long mc2_framegraph_executor_apply_state_by_pass_impl(unsigned /*id*/)
{
    return 0ul;
}
