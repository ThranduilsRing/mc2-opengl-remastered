// gosPostProcess: frame-graph executor ownership accessors, apply-state wrappers and counters.
// Declared in the class headers; shared names in gos_postprocess_internal.h.
#include "gos_postprocess_internal.h"


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

// FRAME-GRAPH-EXECUTOR-ISLAND-2: sub-stage accessor implementations.
// Each WillRun() mirrors the exact early-return gates of the matching run*()
// function so the executor only counts/validates when the body actually draws.

bool gosPostProcess::executorEdgeFogWillRun() const
{
    return edgeFogEnabled_ && edgeFogProg_ && edgeFogProg_->is_valid()
        && mapHalfExtent_ > 0.0f && sceneHasTerrain_;
}

bool gosPostProcess::executorFogOobWillRun() const
{
    return fogOobEnabled_ && fogOobProg_ && fogOobProg_->is_valid()
        && sceneHasTerrain_;
}

bool gosPostProcess::executorSceneDepthTexValid() const
{
    return sceneDepthTex_ != 0;
}

// FRAME-GRAPH-EXECUTOR-ISLAND-3: sub-stage accessor implementations.
// WillRun() mirrors exact early-return gates of runShoreline() / runCloudShadow().
// EXECUTOR-ISLAND-SCREENSHADOW-1: executorScreenShadowWillRun() added below (leak fixed).

bool gosPostProcess::executorShorelineWillRun() const
{
    // TERRAIN-SHORELINE-MASK-1: mirrors runShoreline()'s suppression term.
    return shorelineEnabled_ && shorelineProg_ && shorelineProg_->is_valid()
        && sceneHasTerrain_ && !shorelineSuppressedByTerrainMask_;
}

bool gosPostProcess::executorCloudShadowWillRun() const
{
    return enableCloudShadow_ && cloudProg_ && cloudProg_->is_valid()
        && sceneHasTerrain_;
}

// EXECUTOR-ISLAND-SCREENSHADOW-1: mirrors runScreenShadow() early-return gates exactly
// (lines 2033-2036). Now owned: SCREENSHADOW-TEX-RESTORE-1 (a0b4189b) fixed unit-3 leak.
bool gosPostProcess::executorScreenShadowWillRun() const
{
    return screenShadowEnabled_ && sceneHasTerrain_
        && screenShadowProg_ && screenShadowProg_->is_valid()
        && shadowsEnabled_;
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
            fprintf(stderr,
                "[EXECUTOR v1] FAIL island=PostProcess check=compositeProg_valid "
                "owned=%lu failures=%lu\n",
                g_executorOwnedPasses, g_executorValidationFailures + 1);
        ++g_executorValidationFailures;
        ok = false;
    }

    // 2. Scene color texture must be populated.
    if (c->requiresSceneColorTex && !pp->executorSceneColorTexValid()) {
        if (g_executorValidationFailures < kMaxExecutorFailureLog)
            fprintf(stderr,
                "[EXECUTOR v1] FAIL island=PostProcess check=sceneColorTex_valid "
                "owned=%lu failures=%lu\n",
                g_executorOwnedPasses, g_executorValidationFailures + 1);
        ++g_executorValidationFailures;
        ok = false;
    }

    // 3. Terrain latch: WARN (non-fatal) if absent — sub-stages bail gracefully
    //    in menus / frontend where terrain never drew.
    if (c->warnIfNoTerrainLatch && !pp->executorSceneHasTerrain()) {
        // One-shot warn to avoid per-frame log spam on menu screens.
        static unsigned s_warnCount = 0;
        if (s_warnCount < 4) {
            fprintf(stderr,
                "[EXECUTOR v1] WARN island=PostProcess sceneHasTerrain_=false "
                "(menu/frontend — sub-stages bail gracefully)\n");
            ++s_warnCount;
        }
        // NOT a failure — sub-stages handle absent terrain.
    }

    (void)ok;  // all failures are non-fatal; pp->endScene() always runs
}

//
//
// NOTE: calls glGetError() which clears the GL error flag. Acceptable here:
// (a) we are post-endScene which already calls drainGLErrors("post_process"),
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
                fprintf(stderr,
                    "[EXECUTOR v1] FAIL island=PostProcess postcond=defaultFbo "
                    "bound=%d (expected 0) owned=%lu failures=%lu\n",
                    (int)bound, g_executorOwnedPasses, g_executorValidationFailures + 1);
            ++g_executorValidationFailures;
            postOk = false;
        }
    }

    {
        const GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            if (g_executorValidationFailures < kMaxExecutorFailureLog)
                fprintf(stderr,
                    "[EXECUTOR v1] FAIL island=PostProcess postcond=glNoError "
                    "err=0x%X owned=%lu failures=%lu\n",
                    (unsigned)err, g_executorOwnedPasses, g_executorValidationFailures + 1);
            ++g_executorValidationFailures;
            postOk = false;
        }
    }

    if (postOk)
        ++g_executorOwnedPasses;
}

//
// Pre-applies the 4 GL state calls that runEdgeFog() makes at entry, in the
// SAME ORDER as the body (glBindFramebuffer → setSceneDrawBuffers → glViewport
// → applyPipeline). Body's own identical calls are idempotent re-sets this slice.
//
// Only called from executorOwnBeginSub(EdgeFog) when MC2_FRAMEGRAPH_EXECUTOR is
// ON and executorEdgeFogWillRun(). Increments g_applyStatePasses on each call.

void gosPostProcess::executorApplyEdgeFogState()
{
    // Exactly matches runEdgeFog() lines 2293-2306 entry sequence.
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::PostProcessEdgeFog),
        "PostProcessEdgeFog");
    applyStateBump(RenderCore::framegraph::ApplyPassId::PostProcessEdgeFog);
    // APPLY-STATE-REDUNDANT-BODY-REMOVE-1: signal to runEdgeFog() that the executor
    // already applied these 4 setup calls so the body should skip its own copies.
    edgeFogStateAppliedByExecutor_ = true;
}

// FRAMEGRAPH-APPLY-STATE-ISLAND-2: pre-apply declared GL state for FogOob.
// Exactly matches runFogOob() entry (lines 2349-2362): FBO bind + SingleColor + viewport + applyPipeline.
// No tex binds, no uniforms — those remain body-owned. Idempotent (body re-sets same state).
void gosPostProcess::executorApplyFogOobState()
{
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::PostProcessFogOob),
        "PostProcessFogOob");
    applyStateBump(RenderCore::framegraph::ApplyPassId::PostProcessFogOob);
    // APPLY-STATE-REDUNDANT-BODY-REMOVE-2: signal runFogOob() to skip its own copies.
    fogOobStateAppliedByExecutor_ = true;
}

// FRAMEGRAPH-APPLY-STATE-ISLAND-2: pre-apply declared GL state for Shoreline.
// Exactly matches runShoreline() entry (lines 2240-2252): FBO bind + SingleColor + viewport + applyPipeline.
// No tex binds (sceneDepthTex_/sceneNormalTex_), no uniforms — body-owned. Idempotent.
void gosPostProcess::executorApplyShorelineState()
{
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::PostProcessShoreline),
        "PostProcessShoreline");
    applyStateBump(RenderCore::framegraph::ApplyPassId::PostProcessShoreline);
    // APPLY-STATE-REDUNDANT-BODY-REMOVE-2: signal runShoreline() to skip its own copies.
    shorelineStateAppliedByExecutor_ = true;
}

// FRAMEGRAPH-APPLY-STATE-ISLAND-2: pre-apply declared GL state for CloudShadow.
// Exactly matches runCloudShadow() entry (lines 2186-2196): FBO bind + SingleColor + viewport + applyPipeline.
// No tex binds (sceneDepthTex_), no uniforms — body-owned. Idempotent.
void gosPostProcess::executorApplyCloudShadowState()
{
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::PostProcessCloudShadow),
        "PostProcessCloudShadow");
    applyStateBump(RenderCore::framegraph::ApplyPassId::PostProcessCloudShadow);
    // APPLY-STATE-REDUNDANT-BODY-REMOVE-2: signal runCloudShadow() to skip its own copies.
    cloudShadowStateAppliedByExecutor_ = true;
}

// APPLY-STATE-SCREENSHADOW-1: pre-apply declared GL state for ScreenShadow.
// Exactly matches runScreenShadow() entry (lines 2039-2052): FBO bind + SingleColor + viewport + applyPipeline.
// No tex binds, no uniforms, and NOT the screenShadowDebug_ glDisable(GL_BLEND) — those remain body-owned.
// Idempotent (body makes identical calls at entry when the flag is false).
void gosPostProcess::executorApplyScreenShadowState()
{
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::PostProcessScreenShadow),
        "PostProcessScreenShadow");
    applyStateBump(RenderCore::framegraph::ApplyPassId::PostProcessScreenShadow);
    // APPLY-STATE-REDUNDANT-BODY-REMOVE-2 pattern: signal runScreenShadow() to skip its own copies.
    screenShadowStateAppliedByExecutor_ = true;
}

//
// Each pair gates on executorEnabled(). Begin: if WillRun()==false the sub-pass
// is gated off this frame -> return WITHOUT counting (legitimately not owned).
// End: validate postconditions declared in the IslandContract row; on success
// increment g_executorOwnedPasses (aggregate — sub-stages contribute).

void executorOwnBeginSub(gosPostProcess* pp, RenderCore::framegraph::ExecutorIslandId islandId)
{
    if (!executorEnabled()) return;

    using namespace RenderCore::framegraph;
    const IslandContract* c = findIslandContract(islandId);
    if (!c) return;

    // Gate: if the sub-pass won't draw this frame, don't count/validate it.
    bool willRun = false;
    if (islandId == ExecutorIslandId::EdgeFog)       willRun = pp->executorEdgeFogWillRun();
    if (islandId == ExecutorIslandId::FogOob)        willRun = pp->executorFogOobWillRun();
    if (islandId == ExecutorIslandId::Shoreline)     willRun = pp->executorShorelineWillRun();
    if (islandId == ExecutorIslandId::CloudShadow)   willRun = pp->executorCloudShadowWillRun();
    if (islandId == ExecutorIslandId::ScreenShadow)  willRun = pp->executorScreenShadowWillRun();
    if (!willRun) return;

    const char* name = "Unknown";
    if (islandId == ExecutorIslandId::EdgeFog)       name = "EdgeFog";
    if (islandId == ExecutorIslandId::FogOob)        name = "FogOob";
    if (islandId == ExecutorIslandId::Shoreline)     name = "Shoreline";
    if (islandId == ExecutorIslandId::CloudShadow)   name = "CloudShadow";
    if (islandId == ExecutorIslandId::ScreenShadow)  name = "ScreenShadow";

    if (c->requiresProgramValid) {
        // Program validity is encoded in WillRun() — already proven above.
        // Defensive: no separate accessor needed (WillRun mirrors the gate).
    }
    if (c->requiresSceneDepthTex && !pp->executorSceneDepthTexValid()) {
        if (g_executorValidationFailures < kMaxExecutorFailureLog) {
            fprintf(stderr,
                "[EXECUTOR v1] FAIL island=%s check=sceneDepthTex_valid "
                "owned=%lu failures=%lu\n",
                name, g_executorOwnedPasses, g_executorValidationFailures + 1);
        }
        ++g_executorValidationFailures;
    }
    // warnIfNoTerrainLatch: can't happen (WillRun checks sceneHasTerrain_) but assert for symmetry.
    if (c->warnIfNoTerrainLatch && !pp->executorSceneHasTerrain()) {
        fprintf(stderr,
            "[EXECUTOR v1] ASSERT island=%s warnIfNoTerrainLatch but WillRun passed — logic error\n", name);
    }

    // FRAMEGRAPH-APPLY-STATE-ISLAND-1/2: pre-apply declared GL state before the body.
    // findSubStageState() guards: if the island is not in the table, we do nothing.
    // The pre-apply is idempotent: the body makes identical calls at entry.
    // ISLAND-1: EdgeFog wired. ISLAND-2: FogOob/Shoreline/CloudShadow wired.
    if (islandId == ExecutorIslandId::EdgeFog) {
        using namespace RenderCore::framegraph;
        if (findSubStageState(ExecutorIslandId::EdgeFog) != nullptr)
            pp->executorApplyEdgeFogState();
    }
    if (islandId == ExecutorIslandId::FogOob) {
        using namespace RenderCore::framegraph;
        if (findSubStageState(ExecutorIslandId::FogOob) != nullptr)
            pp->executorApplyFogOobState();
    }
    if (islandId == ExecutorIslandId::Shoreline) {
        using namespace RenderCore::framegraph;
        if (findSubStageState(ExecutorIslandId::Shoreline) != nullptr)
            pp->executorApplyShorelineState();
    }
    if (islandId == ExecutorIslandId::CloudShadow) {
        using namespace RenderCore::framegraph;
        if (findSubStageState(ExecutorIslandId::CloudShadow) != nullptr)
            pp->executorApplyCloudShadowState();
    }
    if (islandId == ExecutorIslandId::ScreenShadow) {
        using namespace RenderCore::framegraph;
        if (findSubStageState(ExecutorIslandId::ScreenShadow) != nullptr)
            pp->executorApplyScreenShadowState();
    }
}

void executorOwnEndSub(gosPostProcess* pp, RenderCore::framegraph::ExecutorIslandId islandId)
{
    if (!executorEnabled()) return;

    using namespace RenderCore::framegraph;
    const IslandContract* c = findIslandContract(islandId);
    if (!c) return;

    // If the sub-pass didn't run (WillRun==false at Begin time) we returned early there.
    // Re-checking WillRun here would be racy if state changed; use a lightweight proxy:
    // if g_executorOwnedPasses hasn't changed since Begin, we are in the skip path.
    // Simpler: just guard with a local flag pair — but to keep this dependency-free we
    // replicate the WillRun check. This is safe: WillRun() is pure state read, no side effects.
    bool willRun = false;
    if (islandId == ExecutorIslandId::EdgeFog)       willRun = pp->executorEdgeFogWillRun();
    if (islandId == ExecutorIslandId::FogOob)        willRun = pp->executorFogOobWillRun();
    if (islandId == ExecutorIslandId::Shoreline)     willRun = pp->executorShorelineWillRun();
    if (islandId == ExecutorIslandId::CloudShadow)   willRun = pp->executorCloudShadowWillRun();
    if (islandId == ExecutorIslandId::ScreenShadow)  willRun = pp->executorScreenShadowWillRun();

    // NOTE: after run*() completes, sceneHasTerrain_ is still true (only cleared at beginScene).
    // WillRun() re-check is safe here.
    if (!willRun) return;

    const char* name = "Unknown";
    if (islandId == ExecutorIslandId::EdgeFog)       name = "EdgeFog";
    if (islandId == ExecutorIslandId::FogOob)        name = "FogOob";
    if (islandId == ExecutorIslandId::Shoreline)     name = "Shoreline";
    if (islandId == ExecutorIslandId::CloudShadow)   name = "CloudShadow";
    if (islandId == ExecutorIslandId::ScreenShadow)  name = "ScreenShadow";
    bool postOk = true;

    // 1. GL_BLEND must be disabled (both run*() call glDisable(GL_BLEND) on exit).
    if (c->postRequiresBlendDisabled) {
        if (glIsEnabled(GL_BLEND) != GL_FALSE) {
            if (g_executorValidationFailures < kMaxExecutorFailureLog)
                fprintf(stderr,
                    "[EXECUTOR v1] FAIL island=%s postcond=blendDisabled "
                    "owned=%lu failures=%lu\n",
                    name, g_executorOwnedPasses, g_executorValidationFailures + 1);
            ++g_executorValidationFailures;
            postOk = false;
        }
    }

    // 2. Active texture unit must be GL_TEXTURE0 (both run*() restore it on exit).
    if (c->postRequiresActiveTexture0) {
        GLint activeUnit = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeUnit);
        if (activeUnit != GL_TEXTURE0) {
            if (g_executorValidationFailures < kMaxExecutorFailureLog)
                fprintf(stderr,
                    "[EXECUTOR v1] FAIL island=%s postcond=activeTexture0 "
                    "activeUnit=0x%X owned=%lu failures=%lu\n",
                    name, (unsigned)activeUnit, g_executorOwnedPasses, g_executorValidationFailures + 1);
            ++g_executorValidationFailures;
            postOk = false;
        }
    }

    {
        const GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            if (g_executorValidationFailures < kMaxExecutorFailureLog)
                fprintf(stderr,
                    "[EXECUTOR v1] FAIL island=%s postcond=glNoError "
                    "err=0x%X owned=%lu failures=%lu\n",
                    name, (unsigned)err, g_executorOwnedPasses, g_executorValidationFailures + 1);
            ++g_executorValidationFailures;
            postOk = false;
        }
    }

    if (postOk)
        ++g_executorOwnedPasses;
}


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
