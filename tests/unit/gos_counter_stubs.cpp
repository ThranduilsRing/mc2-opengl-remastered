// WHAT: zero apply-state counters for mc2_tests and test_render_contract_3.
// WHY: render-contract tests cover scope and resource ordering without the renderer,
// so no passes apply state and the renderer's counter implementations are absent.

extern "C" unsigned long mc2_framegraph_executor_apply_state_passes_impl()
{
    return 0ul;
}

extern "C" unsigned long mc2_framegraph_executor_apply_state_by_pass_impl(unsigned /*id*/)
{
    return 0ul;
}
