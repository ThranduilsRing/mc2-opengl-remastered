// gosPostProcess: frame-graph executor ownership accessors, apply-state wrappers and counters.
// Declared in the class headers; shared names in gos_postprocess_internal.h.
#include "gos_postprocess_internal.h"
#include "mc2fmt.h"


// ============================================================================
// FRAME-GRAPH-EXECUTOR-ISLAND-1
//
// Executor-owned PostProcess island: validate -> call pp->endScene() unchanged
// -> validate. Gate MC2_FRAMEGRAPH_EXECUTOR (default-OFF) -> byte-identical.
//
// Counters are standalone process-statics; NOT in gos_FrameCtx (read-only
// mirror — injecting executor state would break its mirror invariant).
// Exposed via extern "C" mc2_framegraph_executor_owned_passes() /
// mc2_framegraph_executor_validation_failures().
// ============================================================================


bool gosPostProcess::executorCompositeProgramValid() const
{
    return compositeProg_ && compositeProg_->is_valid();
}

bool gosPostProcess::executorSceneColorTexValid() const
{
    return sceneColorTex_ != 0;
}

static bool executorEnabled()
{
    static const bool s_on = mc2::gate::get<"MC2_FRAMEGRAPH_EXECUTOR">();
    return s_on;
}

// --- Standalone counters (process lifetime; never reset) --------------------

static unsigned long g_executorOwnedPasses       = 0;
static unsigned long g_executorValidationFailures = 0;
// FRAMEGRAPH-APPLY-STATE-ISLAND-1: incremented each time executorApplyEdgeFogState() fires.
// Exposed via mc2_framegraph_executor_apply_state_passes() so the advisor can assert > 0.
static unsigned long g_applyStatePasses          = 0;

// PER-PASS-APPLY-COUNTERS-1: per-apply-path counters keyed by ApplyPassId, so
// each apply site is independently observable. The legacy aggregate
// g_applyStatePasses above is no longer incremented; the reported aggregate is
// DERIVED as the sum over this array (no drift). All apply sites — the 5
// PostProcess sub-stage applies here plus the 3 top-level applies in other TUs
// (routed through mc2_framegraph_executor_bump_apply_state(id)) — bump exactly
// one slot.
static unsigned long g_applyStateByPass[(int)RenderCore::framegraph::ApplyPassId::Count] = {0};
static void applyStateBump(RenderCore::framegraph::ApplyPassId id) {
    ++g_applyStateByPass[(int)id];
}

// Max log lines for validation failures — prevent log flooding in pathological cases.
static constexpr unsigned kMaxExecutorFailureLog = 32u;

//
// Checks the IslandContract preconditions for the PostProcess island.
// NON-FATAL: on any failure increments g_executorValidationFailures + logs.
// Returns true if all checks passed (caller proceeds normally either way;
// the pp->endScene() call is ALWAYS made by the caller).

static void executorOwnBegin(gosPostProcess* pp)
{
    if (!executorEnabled()) return;

    using namespace RenderCore::framegraph;
    const IslandContract* c = findIslandContract(ExecutorIslandId::PostProcess);
    if (!c) return;   // not an owned island (defensive; kExecutorIslands has the row)

    bool ok = true;

    // 1. Program must be compiled and valid.
    if (c->requiresProgramValid && !pp->executorCompositeProgramValid()) {
        if (g_executorValidationFailures < kMaxExecutorFailureLog)
            mc2::fmt::print_err("[EXECUTOR v1] FAIL island=PostProcess check=compositeProg_valid owned={:d} failures={:d}\n", mc2::fmt::as_u32(g_executorOwnedPasses), mc2::fmt::as_u32(g_executorValidationFailures + 1));
        ++g_executorValidationFailures;
        ok = false;
    }

    // 2. Scene color texture must be populated.
    if (c->requiresSceneColorTex && !pp->executorSceneColorTexValid()) {
        if (g_executorValidationFailures < kMaxExecutorFailureLog)
            mc2::fmt::print_err("[EXECUTOR v1] FAIL island=PostProcess check=sceneColorTex_valid owned={:d} failures={:d}\n", mc2::fmt::as_u32(g_executorOwnedPasses), mc2::fmt::as_u32(g_executorValidationFailures + 1));
        ++g_executorValidationFailures;
        ok = false;
    }

    // 3. Terrain latch: WARN (non-fatal) if absent — sub-stages bail gracefully
    //    in menus / frontend where terrain never drew.
    if (c->warnIfNoTerrainLatch && !pp->executorSceneHasTerrain()) {
        // One-shot warn to avoid per-frame log spam on menu screens.
        static unsigned s_warnCount = 0;
        if (s_warnCount < 4) {
            mc2::fmt::print_err("[EXECUTOR v1] WARN island=PostProcess sceneHasTerrain_=false (menu/frontend — sub-stages bail gracefully)\n");
            ++s_warnCount;
        }
        // NOT a failure — sub-stages handle absent terrain.
    }

    (void)ok;  // all failures are non-fatal; pp->endScene() always runs
}

//
// Checks the IslandContract postconditions for the PostProcess island.
// On success -> increments g_executorOwnedPasses (this island owned+validated).
// On failure -> increments g_executorValidationFailures + logs.
//
// NOTE: calls glGetError() which clears the GL error flag. Acceptable here:
// (a) we are post-endScene,
// (b) this code path is gated MC2_FRAMEGRAPH_EXECUTOR (default-OFF), so there
// is no production-run side effect.

static void executorOwnEnd(gosPostProcess* /*pp*/)
{
    if (!executorEnabled()) return;

    using namespace RenderCore::framegraph;
    const IslandContract* c = findIslandContract(ExecutorIslandId::PostProcess);
    if (!c) return;

    bool postOk = true;

    // 1. Default FBO must be bound after composite blit.
    if (c->postRequiresDefaultFbo) {
        GLint bound = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
        if (bound != 0) {
            if (g_executorValidationFailures < kMaxExecutorFailureLog)
                mc2::fmt::print_err("[EXECUTOR v1] FAIL island=PostProcess postcond=defaultFbo bound={:d} (expected 0) owned={:d} failures={:d}\n", mc2::fmt::as_i32((int)bound), mc2::fmt::as_u32(g_executorOwnedPasses), mc2::fmt::as_u32(g_executorValidationFailures + 1));
            ++g_executorValidationFailures;
            postOk = false;
        }
    }

    {
        const GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            if (g_executorValidationFailures < kMaxExecutorFailureLog)
                mc2::fmt::print_err("[EXECUTOR v1] FAIL island=PostProcess postcond=glNoError err=0x{:X} owned={:d} failures={:d}\n", mc2::fmt::as_u32((unsigned)err), mc2::fmt::as_u32(g_executorOwnedPasses), mc2::fmt::as_u32(g_executorValidationFailures + 1));
            ++g_executorValidationFailures;
            postOk = false;
        }
    }

    if (postOk)
        ++g_executorOwnedPasses;
}

//
void mc2_executor_own_begin_postprocess(gosPostProcess* pp)
{
    executorOwnBegin(pp);
}

void mc2_executor_own_end_postprocess(gosPostProcess* pp)
{
    executorOwnEnd(pp);
}


extern "C" unsigned long mc2_framegraph_executor_owned_passes()
{
    return g_executorOwnedPasses;
}

extern "C" unsigned long mc2_framegraph_executor_validation_failures()
{
    return g_executorValidationFailures;
}

// FRAMEGRAPH-APPLY-STATE-ISLAND-1: apply-state counter (0 until EdgeFog fires, then >0 per frame).
// Declared extern "C" so debug_state_dump.cpp and render_contract.cpp can forward-declare + call.
// PER-PASS-APPLY-COUNTERS-1: aggregate is DERIVED as the sum over the per-pass
// array. Back-compat: equals the former g_applyStatePasses total (every apply
// site bumps exactly one slot), so executor_apply_state_passes is byte-stable.
extern "C" unsigned long mc2_framegraph_executor_apply_state_passes_impl()
{
    unsigned long total = 0;
    for (int i = 0; i < (int)RenderCore::framegraph::ApplyPassId::Count; ++i)
        total += g_applyStateByPass[i];
    return total;
}

// PER-PASS-APPLY-COUNTERS-1: per-pass getter. Returns the count for one
// ApplyPassId, or 0 for an out-of-range id.
extern "C" unsigned long mc2_framegraph_executor_apply_state_by_pass_impl(unsigned id)
{
    return id < (unsigned)RenderCore::framegraph::ApplyPassId::Count ? g_applyStateByPass[id] : 0;
}

// PER-PASS-APPLY-COUNTERS-1: cross-TU bump of the per-pass apply-state array so a
// top-level apply-state pass (gosRenderer::executorApplyTerrainDecalState /
// ...OverlayState in gameos_graphics.cpp, executorApplyStaticPropOpaqueState in
// gos_static_prop_batcher.cpp) records into ITS OWN slot — independently
// observable, and aggregating into the sum-derived total without double-count.
extern "C" void mc2_framegraph_executor_bump_apply_state(unsigned id)
{
    applyStateBump((RenderCore::framegraph::ApplyPassId)id);
}
