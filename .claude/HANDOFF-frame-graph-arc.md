# HANDOFF — Render-frame-graph arc (resume here)

**Date:** 2026-06-30 (REBOOT HANDOFF). **Branch:** `claude/nifty-mendeleev` (worktree, canonical).
**HEAD = `f60b7ea2`** (NOT the orphaned `ecfe38e2` — see INDEX-RACE INCIDENT below; branch off f60b7ea2).
**Primary task:** continue the FRAME-GRAPH arc. **Do frame-graph first; legacy-terrain
retirement is SECONDARY (after).**

**▶ RESUME POINTER (latest — 2026-06-30, HEAD `dea90c61`):** ★★**TIER-C LOOP CLOSED — FIRST MEASURED
REORDER EXPERIMENT RUN, CANDIDATE REJECTED, ORACLE STRENGTHENED.** Cleanups done (full unit suite now
**240 passed / 0 failed** — the long-standing "4 known-unrelated" red is GONE). The validator predicted
StaticProp↔Mech was the SOLE legal adjacent swap → a gated execution-changing experiment FALSIFIED it on
PARITY (depth-EQUAL tie-break between overlapping mech/prop fragments is order-dependent) → the oracle was
updated to **ZERO clean legal adjacent swaps**. The reorderer is **NOT shipped**; the experiment gate
`MC2_FRAMEGRAPH_REORDER_SPMECH` stays **default-OFF permanently** (capability artifact, NOT adopted).
★**DURABLE LESSON (now encoded in the oracle):** "no hard resource edge" ≠ "visually commutative" for
overlapping opaque passes — depth-equal tie-break is order-dependent. **Legal verdict ≠ adoption; parity
proof beats armchair reasoning; default-OFF stays OFF.** Era line: Tier-B practical COMPLETE @ `40e0f5af`;
tier-C VALIDATOR @ `4b4b8c9f`/`fd4cfd54`; first reject-experiment + oracle-strengthen @ `2461d37e`/
`dea90c61`. See the TIER-C FIRST EXECUTION EXPERIMENT block + entries 61–65 below.

★★ **NEXT-SESSION NORTH-STAR — GPU RESOURCE OWNERSHIP / RAW-GLOBAL MINIMIZATION (GPU-BUFFER-OWNER-RECON-1).**
The goal is NOT just enumerate/register — it is to **MINIMIZE the ~70% of GPU resources that are raw
file-static `GLuint` globals** (`s_heightSsbo`, `g_recipeSSBO`, `s_mechMaterialSsbo`, ring buffers, batcher
instance SSBOs, view UBO, …) by bringing them under registry/owner ownership. **Cheap entry points:**
(1) the **view-uniforms UBO** (1 clean site); (2) **LOD-chunk type/cement SSBOs** — the SOLE *live* terrain
renderer's own buffers are conspicuously UNregistered while the DEAD indirect-path buffers ARE registered;
close that inconsistency first. This maps to the real remaining modernization gap far better than more
reorder experiments. (This SUPERSEDES "more reorder experiments" as the primary next thrust.)

**(superseded resume pointer — 2026-06-30, HEAD `40e0f5af`):** ★★**TIER-B PRACTICAL COMPLETE.**
**Tier-B practical same-order frame graph COMPLETE: the graph validates, applies, enforces, and names/
lifetimes all ownable current-order render work.** (NOT scheduler, NOT backend, NOT Vulkan-ready, NOT
renderer-fully-optimized — tier-B = same-order ownership is boring + enforced.) Registry complete (24
ids, lifetimes on all 14 live registrations), enforcement = 5 raw-GL axis gates + the capstone meta-gate,
apply-state ownable set complete. (NEXT was tier-C VALIDATOR — now SHIPPED, see entries 59–60.) See entries 54–58.

**(superseded resume pointer — 2026-06-30 reboot, kept for trail):** ★**APPLY-STATE OWNABLE SET COMPLETE.** Top-level VALIDATE
10/10 + apply-state for every ownable pass is now runtime-proven-or-code-correct: PostProcess **6/6**
+ **StaticProp/Mech/Water** runtime-proven sole-setter + **Shadow now FULL render-target-mode owner**
(FBO+viewport+clear+pipeline, APPLY-STATE-SHADOW-2 `2a3b0967`). TerrainDecal/TerrainOverlay stay
code-correct; the earlier "CONTENT-UNEXERCISABLE in stock" read was WRONG ABOUT THE CAUSE — see
★CORRECTION below: roads/overlays are basically TURNED OFF in the v0.4/0.4c builds we smoke to; they DO
draw in the **v0.5 install**, so decal/overlay apply-state IS exercisable — deploy to the v0.5.0 path.

★**CORRECTION (2026-06-30, from user) — DECAL/OVERLAY ARE EXERCISABLE, DEPLOY TARGET WAS WRONG.**
The cause of `decal_vbo tris=0` on all stock missions is NOT "stock maps have no overlay content." Roads/
overlays simply **do not draw in the v0.4 / v0.4c builds/installs** we deploy to for smokes — they are
effectively turned off there. They **DO draw in the v0.5 install.** So to actually content-exercise
TerrainOverlay + TerrainDecal apply-state (light up `executor_apply_state_by_pass.TerrainOverlay` /
`.TerrainDecal` > 0), the NEXT session must DEPLOY TO:
`A:\Games\mc2-opengl\releases\0.5 testing\mc2-win64-v0.5.0` (note the SPACE in "0.5 testing") — NOT
v0.4/0.4c. Then run an executor-ON smoke on a road/overlay-bearing mission there and read the per-pass
apply counter. Also still add **MC2_DYNAMIC_DECALS** to run_smoke.py's env allowlist for the dynamic-decal
path. This removes the asterisk on decal/overlay apply-state being only "code-correct, unexercised." The single-applyPipeline limitation is GONE — FRAMEGRAPH-APPLY-STATE-EXTEND-1
(`6def9bd2`) added `ClearSpec` + the shared GL-free `applyTopLevelGenericAxes(desc,fbo,w,h)` helper
(FBO→viewport→clear, skip-sentinel, pipeline by caller; header `RenderCore/top_level_apply_axes.h`),
which unblocked Shadow's pipeline+depth-clear lift.
★**ENFORCEMENT = 5 RAW-GL axis gates LIVE** (depthFunc `7339dd90` / depthMask `33820a2f` / colormask
`ff9fed17` / blendFunc `00c61255` / **FBO-bind `8befeaa1`** — the FBO-bind gate is NOT a flat clone:
HYBRID taxonomy = file allowlist of 7 FBO-owner TUs + `// FBO-OWNER:` comment tag + auto-exempt prev/
saved-restore pattern; 82 sanctioned sites frozen-forward; all wired into `scripts/check-contracts.sh`).
**CAPSTONE remaining = RAW-GL-BYPASS-CAPSTONE-1** (meta-gate enumerating all 5 axis gates + asserting no
un-gated escape).
★**RESOURCE REGISTRY = terrain + material integrated** (REGISTRY-TERRAIN-SSBO-1 `49921b5b` +
REGISTRY-MATERIAL-SSBO-1 `f60b7ea2`): live count 6→**10** (default) / 11–12 (cement / MC2_MATERIAL_GPU
gate). Observe-only metadata, byte-identical. Remaining registry slices in NEXT-STEPS below.
**NEXT DECISION (tier-b finish order):** (a) RAW-GL-BYPASS-CAPSTONE-1, then (b) remaining RESOURCE-
REGISTRY slices (branch off f60b7ea2), then (c) tier-b practical 100% → THEN tier-C legal-reorder
VALIDATOR (recon refresh first; tier-C readiness ~15-20%, DO NOT build yet). See ▶ NEXT-STEPS section.
Per-pass apply counters: `executor_apply_state_by_pass` map; `get_executor_health` MCP now surfaces it
per-pass (`447a1de7`).

★★ **TWO OPERATING RULES (read before any parallel work):**
1. **ONE COMMITTER PER WORKTREE.** Even disjoint file sets race on the shared git INDEX (the INDEX-RACE
   incident below cost a commit-recovery). Parallel work MUST go in a SEPARATE worktree.
2. **SPARE-WORKTREE PARALLEL PATTERN VALIDATED** (`docs/build-parallel-and-tooling.md`): spare detached
   worktree (`mc2-nifty-land`) + spare build64 + spare deploy dir + lease-serialized smoke + cherry-pick
   the disjoint commits onto nifty. The registry slices proved it (zero races vs the Shadow lane).

★**APPLY-STATE CLASSIFICATION (2026-06-30 — OWNABLE SET COMPLETE):**
- **Runtime-proven sole-setter:** PostProcess **6/6** islands (EdgeFog/FogOob/Shoreline/CloudShadow/
  ScreenShadow + outer endScene) + **StaticPropOpaque (1890)** + **MechOpaque (1890)** + **Water (1784)**.
- **FULL render-target-mode owner (FBO+viewport+clear+pipeline), runtime-proven:** **Shadow** (=1608/
  frame, APPLY-STATE-SHADOW-2 `2a3b0967`; uses the EXTEND `applyTopLevelGenericAxes` helper).
- **Code-correct; exercisable on v0.5 (NOT v0.4):** TerrainDecal, TerrainOverlay. ★CORRECTION: roads/
  overlays are OFF in the v0.4/0.4c builds we smoke to — they DRAW in v0.5. Deploy to
  `A:\Games\mc2-opengl\releases\0.5 testing\mc2-win64-v0.5.0` + executor-ON smoke on a road/overlay
  mission to light the per-pass apply counter (+ add MC2_DYNAMIC_DECALS for the dynamic-decal path).
  Mechanism is StaticProp-proven.
- **Correctly validate/FBO-only (no single liftable pipeline):** VFX, UI, Vegetation, Terrain-main.

★**THREE-STATE APPLY-STATE DASHBOARD (advisor — original):**
1. **Runtime-proven SOLE-SETTER (body-skip):** PostProcess **6/6** islands
   (EdgeFog/FogOob/Shoreline/CloudShadow/ScreenShadow + outer endScene).
2. **Runtime-proven TOP-LEVEL apply-state:** **StaticPropOpaque** + **MechOpaque** + **Water** (named-
   counter proven: StaticProp/Mech=1890, Water=1784/tier1 run; apply attributed, render-correct +0 destroys).
   ★This is the RUNTIME PROOF of the shared top-level apply MECHANISM — the same infra TerrainDecal/
   TerrainOverlay use. So accept the code-faithful proof for decal/overlay (below).
3. **Declared + unit-tested + code-correct (live-site) + byte-identical-OFF, but CONTENT-UNEXERCISABLE
   in stock smoke:** **TerrainOverlay** (SITE-FIXED `bd645aa6` → now on the LIVE draw site
   `drawDecalStaticBatch`), **TerrainDecal**. ★KEY FINDING (corrects earlier "mc2_02=980 overlay tris"
   — that was a MISREAD of a different trace): NO stock smoke mission (mc2_01–24) exercises the overlay
   pass — ALL report `[TERRAIN_OVERLAY v1] decal_vbo_built tris=0`. The decal-static/overlay pass only
   draws true road/runway/bridge `&Overlays` tiles; stock maps have none. mc2_02's cement (75
   CEMENT_ATLAS tiles) is drawn by the **TERRAIN-SOLID** path (CEMENT-BAKE-INTO-TERRAIN, composited in
   the LOD-chunk terrain pass), NOT the TerrainOverlay pass → `executor_apply_state_by_pass.TerrainOverlay`
   stays 0 even with cement present. TerrainDecal needs combat craters/footprints (dynamic, +
   `MC2_DYNAMIC_DECALS` which is NOT in run_smoke.py's allowlist). **NOT a blocker** — the mechanism is
   StaticProp-proven.
   ★**CORRECTION (2026-06-30, user):** the `tris=0` cause above is WRONG. It is NOT "stock maps have no
   overlay content" — roads/overlays are **turned off in the v0.4/0.4c builds we smoke to** and DRAW in
   the **v0.5 install**. So decal/overlay ARE content-exercisable: deploy to
   `A:\Games\mc2-opengl\releases\0.5 testing\mc2-win64-v0.5.0` (space in "0.5 testing") and run an
   executor-ON smoke on a road/overlay-bearing mission, then read `executor_apply_state_by_pass.
   TerrainOverlay` / `.TerrainDecal`. (+ add MC2_DYNAMIC_DECALS for the dynamic-decal path.)

**NEXT (advisor):** pick the next apply-state candidate. Overlay/decal full content-exercise needs
(a) an editor/road map or mod mission with real road/runway/bridge tiles, and (b) adding
`MC2_DYNAMIC_DECALS` to run_smoke.py's mission-Popen env allowlist (decal). **Mech = GREEN-RECON-PARKED**
(recon clean at a4c9926d-era, dispatch at txmmgr:3273 NOT the begin seam — but **do NOT build until the
redline is lifted**). Water / Terrain-main / Shadow deferred; Terrain-main / Veg / VFX / UI likely-never
top-level apply-state (PipelineId::Invalid / pin-sensitive / runtime-dynamic). Side cleanup noted:
`kParticleEffectState` stale blend=Additive vs live alpha-blend (render_contract.cpp:315 vs
gos_particle_bridge.cpp:1164).
✓ TOOLING (FIXED `6f6d1454`): the recurring `stale_deploy_check` WARN in `verify_executor_slice.py` was a
benign parser quirk (read `src_commit` as a top-level line, not the per-row col-3) — now parses per-row;
genuine staleness still flagged. Also added `verify_executor_slice.py --assert-pass-fired NAME[:MIN]`
(reads `executor_apply_state_by_pass`; StaticPropOpaque:1→exit0, TerrainDecal:1→exit1, bogus→error w/
valid names) + grep-gate `scripts/check-apply-pass-bumped.py` (every ApplyPassId bumped exactly once
across engine TUs; wired into check-contracts.sh after raw_gl_blendfunc) + offline registration doctests.

---

## Operating mode (do this)
- **Caveman mode.** Start with `/caveman` (full). Terse, fragments OK, technical terms exact.
- **Subagent-heavy.** Fan out recon/build/review to parallel subagents (general-purpose).
  Use `mcp__mc2-repo-intel__repo_grep` / `repo_symbol` for search. Background long agents.
- **Automation rule: if you do it more than twice, automate it.** This session that produced
  `scripts/run-unit-tests.sh` (offline doctest runner) and the `check-*.py` gates. Keep doing it.
- **Measure-first, then enforce.** Every ambient axis shipped as a default-OFF probe FIRST,
  observed live (counters in the dump), and only promoted to a guard once 0-divergence proven.
  This caught two wrong textbook assumptions (blend, PostProcess-FBO). Do NOT declare a
  contract you have not measured against live GL.
- **Pure kernel → offline harness → engine integration → short smoke.** Logic lives in
  GL-free headers (`RenderCore/*.h`), verified by `bash scripts/run-unit-tests.sh` in seconds.
  Only integration needs build+deploy+smoke.

## ⚠ Shared-worktree hazard
Parallel lanes run `git add -A && commit` in this worktree — twice this session they swept my
staged work into THEIR commits (BUG2, OBJECT-SHADOW landed under unrelated messages; code is
correct in HEAD). Mitigation used: **commit immediately after edits** (before build/smoke), and
verify `git diff` is yours-only before staging. Do the same.

---

## Paths + commands
- **Worktree root:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
- **Deploy target (this session):** `A:/Games/mc2-opengl/mc2-win64-0.4c` (game exe; a complete
  7GB install). NOTE: `0.4c` (no `v`) is non-standard — the smoke alias `--deploy 0.4c` resolves
  to the *editor* `v0.4c`, so deploy/smoke this build with explicit `--exe ... --no-lease`.
  Standard game install is `A:/Games/mc2-opengl/mc2-win64-v0.4`.
- **CMake:** `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`
- **Build (always RelWithDebInfo):** `<cmake> --build build64 --config RelWithDebInfo --target mc2`
  — capture the REAL exit code (don't pipe to tail/head; that masks failures — bit me twice).
- **Deploy:** `py -3 scripts/deploy_payload.py "A:/Games/mc2-opengl/mc2-win64-0.4c" --source-root . --exe-name mc2.exe`
- **Smoke (2 missions, routine):** `MC2_DEBUG_STATE_DUMP=1 py -3 scripts/run_smoke.py --duration 30 --keep-logs --no-lease --exe "A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe" --mission mc2_01 --mission mc2_24`
  (full tier1 only for landing checks; 2 missions for iteration — NOT 5 every time.)
- **Offline logic tests:** `bash scripts/run-unit-tests.sh --ts=FrameGraph` (or ViewCurrency, or all).
- **Guard gate:** `MC2_DEPLOY_DIR=A:/Games/mc2-opengl/mc2-win64-0.4c py -3 scripts/check-ambient-guard.py`

## MCP verification (engine running with MC2_DEBUG_STATE_DUMP=1)
- `get_render_health` — frame_graph block: ambient/fbo samples+mismatches, terrain_path counts.
- `get_frame_context` — view-epoch currency telemetry.
- The dump file: `<deploy>/debug_state/latest_render_state.json` → `frame_graph` + `terrain_path` blocks.

---

## What shipped this session (all on nifty-mendeleev)
`git log --grep="FRAME-GRAPH\|VIEW-CURRENCY\|VIEW-EPOCH\|AMBIENT\|TERRAIN-\|FBO-LEDGER"` for SHAs.

Render-frame contract, in order:
1. **RENDER-VIEW-CURRENCY-1** + **VIEW-EPOCH-DEDUPE-1** — object/mech MVP currency keyed on a
   semantic view-content epoch (`g_viewContentEpoch`, bumps only on real camera-matrix change;
   `g_mvpDiagFrame` is the raw publish counter). Fixed BUG1/3/5 (zoom-wobble/rotate-flicker/
   cacti-vanish — one root: stale-view dispatch-MVP snapshot from commit 7f43ee37).
2. **RENDER-FRAME-CONTEXT-1** — `gos_FrameCtx()` additive read-only mirror over the authoritative
   globals (globals stay authoritative; authority-inversion is a deferred later slice).
3. **VIEW-CURRENCY-HARNESS-1** — pure kernel `RenderCore/view_currency.h` + offline doctest
   (incl. a counter-proof of the old "stale-every-frame" bug).
4. **FRAME-GRAPH-RECON-1** + **SKELETON-1** — resource-DAG validator (`RenderCore/frame_graph_validate.h`),
   `validateShippedFrameGraph()`, offline + dump + MCP + CI (`scripts/check-frame-graph.py`).
5. **AMBIENT-LEDGER-1/-2** + **AMBIENT-GUARD-1** — `RenderCore/ambient_contract.h` declares
   colorMask/depthFunc/depthWrite + the colorMask & terrain-latch handshakes. Default-ON runtime
   guard (`mclib/render_contract.cpp` samples live GL at the `noteRenderPass` seam, compares to
   declared; 0 divergence). blend DISPROVEN as per-pass (GL_BLEND is global in MC2) — left Inherit.
6. **AMBIENT-RUNTIME-1** — the live cross-check + samples/mismatch counters; gate
   `scripts/check-ambient-guard.py`; CI-fatal `MC2_FRAMEGRAPH_AMBIENT_FATAL=1`; disable
   `MC2_FRAMEGRAPH_AMBIENT_GUARD=0`.
7. **FBO-LEDGER-1** — `RenderCore/fbo_ledger.h` maps live FBO GLuints → logical RenderResourceId
   (registered at glGenFramebuffers in gos_postprocess); per-pass FBO guard (scene passes →
   MainColor, verified 0-divergence). New `RenderResourceId::Backbuffer`.

Recon docs (in `docs/render-backend-seams/`): `frame-graph-recon-1`, `-executor-staging-recon-1`,
`frame-graph-tex-latch-recon-1`, `terrain-subpass-recon-1`.

Terrain-subpass modeling (frame-graph step 1 — DONE):
10. **TERRAIN-SUBPASS-MODEL-1** (`d96735fa`) — models the 4-state terrain-solid machine as a GL-free
    sub-pass table (`RenderCore/terrain_subpass_contract.h`): per-branch reads[]/barrier/mvpSource/
    drawSite + 5 new RenderResourceIds (recipe/thin SSBO, cement atlas, transition-mask, height SSBO).
    Keeps the single `Terrain` parent row in `kFramePassOrder` (no enum churn). Latch-audit kernel
    (`allDeclaredLatchProducersImplemented()`) turns recon §4 HIGH into a TESTED tripwire: asserts
    IndirectBridge's `markTerrainDrawn` miss as current reality (`latchActuallyImplemented=false`) —
    flip to true + update asserts when the Indirect fix lands. Pure active-branch probe over the
    telemetry counters. 19/19 doctests; build+deploy+smoke 2/2 PASS, +0 destroys. Zero behavior change.

Frame-graph dry-run executor (step 2 — DONE):
11. **FRAME-GRAPH-EXECUTOR-DRYRUN-RECON-1** (`f1e8408f`) — mapped the per-frame observe seam
    (`noteRenderPass` render_contract.cpp:670; fires for 7/11 passes — Shadow/Water/VFX/Vegetation
    are INVISIBLE, `beginPass`/`endPass` only in tests). `docs/render-backend-seams/frame-graph-executor-dryrun-recon-1.md`.
12. **FRAME-GRAPH-EXECUTOR-DRYRUN-1** (`7f9e1fe8`) — per-frame observe-and-diff harness over
    `kFramePassOrder`. Pure GL-free kernel `RenderCore/frame_pass_trace.h` (`dryRunCompare`:
    fired-set / order / terrain-mutex / latch-miss) + 5 doctests; thin recorder at `noteRenderPass`
    self-gated **MC2_FRAMEGRAPH_DRYRUN** (default-OFF); frame-boundary compare in
    `renderPassTelemetryFrameTick`; `frame_graph_dryrun` dump block (6 `extern "C" mc2_framegraph_dryrun_*`).
    Unobserved passes classified UNOBSERVED ≠ diverged (zero false alarms default config). 24/24 doctests,
    build 0, smoke 2/2 OFF (byte-identical) AND ON. **REAL FINDING (not papered over):**
    `out_of_order ~2/frame`, firstOOO=Terrain — LOD-chunk draws `gamecam.cpp:508` BEFORE renderLists
    so Terrain fires ahead of StaticPropOpaque (declared slot 1). `kFramePassOrder` mismodels the
    terrain draw-site. mutex=0, latch_miss=0 (LODChunk default). → resolved by #13-15.
13. **DRYRUN-DRAWSITE-ORDER-RECON-1** (`bf44d9a0`) — recommended Option B (model draw-site, do NOT
    reorder); draw-site is config-variable; surfaced the stale Indirect-latch row + stale comment.
14. **TERRAIN-SUBPASS-LATCH-ROW-FIX-1** (`ea0b2337`) — corrected my own d96735fa: IndirectBridge latch
    WAS already implemented (`gos_terrain_indirect.cpp:3750`, fix 26ee9bdd predates the model). Flipped
    `latchActuallyImplemented=true` (all 4); latch-audit test now a forward REGRESSION guard.
15. **DRYRUN-DRAWSITE-ORDER-1** (`dddae49b`) — Option B `knownEarlyDrawSite`/`knownEarlySuppressed`
    driven from `TerrainSubPass::drawSite==Gamecam` (NOT a reorder). Also corrected two UNCONDITIONAL
    stale kFramePassOrder entries (MechOpaque<StaticProp, Overlay<Decal) — DAG-safe, validator green.
    25/25 doctests, build 0, smoke 2/2 OFF&ON; ON dump out_of_order=0, known_early_suppressed=1970.
16. **TERRAIN-LEGACY-RETIRE-RECON-1** (`4aeeb0b0`) — SECONDARY arc. All 3 legacy branches DEAD in
    gameplay/smoke/capture/editor. First slice = LEGACY-MLR-DELETE-1 (txmmgr.cpp:3062-3114). HIGH:
    retain `IsFrameSolidArmed()` (8+ non-terrain consumers). `terrain-legacy-retire-recon-1.md`.

Observe-coverage (advisor "observe fully before owning" — DONE):
17. **DRYRUN-OBSERVE-COVERAGE-RECON-1** (`5a01c949`) — scouted begin-sites + guard-safety for the 4
    invisible passes. All 4 PassIdentity values already exist. Shipped seam = `noteRenderPass`.
18. **DRYRUN-OBSERVE-COVERAGE-1** (`0ced0396`) — observe-only `noteRenderPass(begin)` added for
    **Water** (gameos_graphics renderWaterFastPath) + **VFX/ParticleEffect** (gamecam.cpp before
    Batcher::Flush, via `extern "C" mc2_note_particle_effect_pass()` shim — code/ TU can't include
    RenderPassContract.h) + **VegetationCards** (gos_vegetation, gate-default-OFF so unobserved in
    default smoke). **Shadow EXCLUDED (justified, documented at site → SHADOW-OBSERVE-2):** its
    ambient `colorMask=AllOff` is established later in per-batch draw (~:4473/:4688), not at
    beginShadowPrePass — instrumenting there would feed the DEFAULT-ON ambient guard AllOn-vs-AllOff
    and trip check-ambient-guard.py. New dump field `observed_pass_count`
    (`mc2_framegraph_dryrun_observed`). 25 doctests, build 0, smoke 2/2 OFF&ON, ambient mismatch=0
    both modes, out_of_order=0. Exact 11-slot accounting (observed 3589 + unobserved 3000 = 599×11).
    Advisor bar met: 11/11 OR justified exclusions (Shadow=justified, Veg=gated-off).

Terrain (bug + measurement, NOT retirement yet):
8. **TERRAIN-INDIRECT-LATCH-FIX-1** — the Indirect terrain branch never called `markTerrainDrawn()`
   (would silently kill 5 post passes on chunk-OFF+indirect). Fixed; verified.
9. **TERRAIN-PATH-TELEMETRY-1** — per-branch draw counters (`RenderCore/terrain_path_telemetry.h`),
   in dump + MCP. PROVEN: default = lod_chunk only (1994), MLR/indirect/patch = 0.

---

## NEXT — continue FRAME-GRAPH arc (primary)
Executor readiness ~35–45% (advisor scale). Path to a safe dry-run executor:

1. ~~**TERRAIN-SUBPASS modeling**~~ — DONE `d96735fa` (TERRAIN-SUBPASS-MODEL-1, shipped #10).
2. ~~**FRAME-GRAPH-EXECUTOR-DRYRUN-1**~~ — DONE `7f9e1fe8` (shipped #12). It surfaced the terrain
   draw-site out-of-order finding → drives the NEXT step below.

~~DRYRUN-DRAWSITE-ORDER-1~~ — DONE `dddae49b` (shipped #15); recon `bf44d9a0` (#13). Terrain draw-site
modeled (Option B knownEarly, not reorder); two unconditional stale entries corrected. ON dump now
out_of_order=0. Latch row corrected `ea0b2337` (#14). Legacy-retire recon `4aeeb0b0` (#16).

Executor island (advisor "first owned island = boring" — DONE):
19. **FRAME-GRAPH-EXECUTOR-ISLAND-RECON-1** (`83ba7b2e`) — PostProcess-whole NOT FBO-ownable (FBO
    ambiguous: sceneFBO_ at seam, FBO 0 mid-endScene; 5/12 sub-stages latch-consumers). Recommend
    wrapping the existing `pp->endScene()` in a validate→assert→call→validate shell (no FBO ownership).
20. **FRAME-GRAPH-EXECUTOR-ISLAND-1** (`d88cf49e`) — FIRST executor-OWNED island. Gate
    **MC2_FRAMEGRAPH_EXECUTOR** default-OFF. `RenderCore/frame_executor.h` GL-free `IslandContract`
    (1 PostProcess row) + offline test. `executorOwnBegin/End` (gos_postprocess.cpp) validate inputs
    (compositeProg valid, sceneColorTex!=0), warn if !sceneHasTerrain_, call endScene UNCHANGED,
    validate postconditions (FBO 0 bound, glGetError==NO_ERROR). Standalone counters
    `mc2_framegraph_executor_owned_passes` / `_validation_failures` (NOT in read-only gos_FrameCtx).
    29 doctests, build 0, smoke 2/2 OFF&ON; ON dump executor_owned_passes=1989, validation_failures=0.
    Byte-identical OFF. Proves the executor can own execution without a renderer rewrite.

NEXT (advisor sequence remaining):
3. **LEGACY-MLR-DELETE-1** (NOW; #16) — ⚠ RECON TARGET CORRECTED — DO NOT delete txmmgr.cpp:3062-3114
   wholesale. **HIGH: that range is the SHARED MC2_DRAWSOLID loop (3052-3114) serving ALL solid
   geometry — terrain AND non-terrain.** The else branch (:3065-3068) + the gos_RenderIndexedArray
   body draw NON-TERRAIN opaque nodes; deleting the range breaks non-terrain solid rendering. The
   legacy-MLR-TERRAIN draw is only reached when an MC2_ISTERRAIN node hits the loop with
   `modernHandled==false` (else-fallthrough). In default (chunk-on) terrain nodes already skip at
   `:3057` (`modernHandled && ISTERRAIN` → reset+continue). SURGICAL delete: at `:3057` make terrain
   nodes ALWAYS skip the MLR draw, add a reachability tripwire counter for the `!modernHandled &&
   ISTERRAIN` regression case (loud, not silent-no-terrain), then the now-dead ISTERRAIN branches
   (:3062-3064 terrain state, :3077-3081 terrain extras) can be simplified to non-terrain-only.
   precondition `terrain_path.legacy_mlr==0` CONFIRMED live. RETAIN `IsFrameSolidArmed()`.
   Re-scope/re-recon LEGACY-MLR-DELETE before coding — the 4aeeb0b0 recon's line range was wrong.
4. ~~**TEX-LATCH-RECON-1**~~ — DONE `edf4392f` (TEX-LATCH-EXECUTOR-RECON-1, executor-readiness angle,
   extends e1b28ee7). VERDICT: **DEFER** the texture-unit contract/ledger. Every scene pass binds its
   own units explicitly (per-pass-rebind = the executor safety invariant); no order-fragile ghost on
   the default path. Build trigger: executor REORDERS two passes AND an NVIDIA capture confirms a live
   2D/2D_ARRAY aliasing misfire on units 3/4/5 — neither true today. Confirmed pre-existing leak
   `shadowDebugOverlay` unit-0 2D_ARRAY no-restore (GLSTATE-SHADOWDEBUG-2DARRAY-1, default-OFF, 1-line
   fix) escapes executorOwnEnd→HUD under executor PostProcess ownership; LOW today.

Executor island 2 + hygiene (DONE):
21. **GLSTATE-SHADOWDEBUG-2DARRAY-1** (`92b028b3`) — 1-line: unbind leaked unit-0 2D_ARRAY at
    shadowDebugOverlay exit (default-OFF path; pre-empts leak under broader executor ownership).
22. **FRAME-GRAPH-EXECUTOR-ISLAND-2** (`0e8c57aa`) — executor owns EdgeFog + FogOob sub-stage islands.
    Re-keyed IslandContract to `ExecutorIslandId{PostProcess,EdgeFog,FogOob}`; wrapped unchanged
    runEdgeFog()/runFogOob() (:2466/:2470) with WillRun() gates (only actually-drawn sub-passes count)
    + postconditions (blend disabled, activeTexture0, no GL error). Same MC2_FRAMEGRAPH_EXECUTOR gate.
    30 doctests, build 0, smoke 2/2 OFF&ON; ON executor_owned_passes=4210 (~3/frame), failures=0.
    Proves the executor owns a deeper sub-island, not just the outer endScene wrapper.

Legacy terrain retirement (FIRST DELETE DONE):
23. **LEGACY-MLR-DELETE-1** (`55ba2736`) — surgical retirement of the legacy masterVertexNode terrain
    draw. txmmgr.cpp:`:3057` terrain nodes ALWAYS skip the shared MC2_DRAWSOLID loop; tripwire
    `noteTerrainPath(LegacyMLR)` on `!modernHandled && ISTERRAIN` (the legacy_mlr counter is now the
    live regression detector); dead ISTERRAIN loop-body branches simplified to non-terrain-only.
    SHARED solid loop + non-terrain draw + IsFrameSolidArmed PRESERVED. Byte-identical in default.
    Build 0, smoke 2/2 (+0 destroys), terrain_path lod_chunk=1069 / legacy_mlr=0 (tripwire silent),
    30 doctests. 1 file (txmmgr.cpp, 14+/17-). Pre-existing now-unreachable LegacyMLR bump at
    gameos_graphics.cpp:7309 (terrainDrawIndexedPatches) — no double-count. Follow-on: indirect/patch
    branch retirement + delete the now-dead :7309 + terrainDrawIndexedPatches when those go.

Shadow observation (DONE — dry-run now 11/11):
24. **SHADOW-OBSERVE-2-RECON-1** (`2231a52d`) + **SHADOW-OBSERVE-2** (`b794778e`) — active shadow
    pre-pass enforces depth-only via FBO attachment (DrawBufferSet::ShadowDepthOnly), NOT glColorMask
    (ground-truthed: :3490 legacy, :4478/:4693 terrain/water — none active shadow). So
    Shadow.colorMaskOnEntry=AllOff was MISMODELED → relaxed to Inherit (guard skips colorMask;
    disablesColorWrite stays true, no handshake weakened). noteRenderPass added at beginShadowPrePass
    :6270 (depthFunc=ShadowLess + depthWrite=On valid there). Shadow now OBSERVED. ambient mismatch 0
    both OFF&ON (Shadow sampled every frame), out_of_order 0, 31 doctests, smoke 2/2.
    ⚠ MODELING DEBT: instrumenting Shadow surfaced a static-shadow-build that fires inside renderLists
    (~txmmgr.cpp:2599) AFTER the MechOpaque preamble note (:2362) on the once-per-mission build frame.
    Handled by a RUNTIME heuristic in dryrunFrameBoundary (mark MechOpaque knownEarly when it observes
    Mech-before-Shadow) — narrow (fires only those frames), but a heuristic not a declared contract.
    FOLLOW-UP **SHADOW-STATIC-BUILD-MODEL**: model the static-shadow-build as a distinct occurrence
    instead of the runtime Mech-knownEarly suppression.

★ NORTH-STAR REACHED (draw level): LOD-chunk is the SOLE live terrain renderer.
25. **TERRAIN-INDIRECT-PATCH-RETIRE-RECON-1** (`3693b45e`) — precise surgical targets + retain-list.
26. **PATCHSTREAM-THIN-RETIRE-1** (`026e7276`) — txmmgr.cpp:3017-3021: flush() draw → noteTerrainPath
    (PatchStreamThin) tripwire, modernHandled stays false. flush() side-effect audit: ring advance is
    in beginFrame (not flush), snapshot getLastFlush* getters read 0 correctly when skipped,
    markTerrainDrawn correctly not called → no load-bearing side effect lost.
27. **INDIRECT-BRIDGE-RETIRE-1** (`11c1450e`) — txmmgr.cpp:3011-3016: DrawIndirect() caller →
    noteTerrainPath(IndirectBridge) tripwire + modernHandled=true. DrawIndirect() body RETAINED
    (RenderWaterReflectionPass calls it internally :3817); only the txmmgr SOLID caller retired.
    Both: build 0, smoke 2/2 +0 destroys, terrain_path lod_chunk=1987 / indirect=0 / patch_stream=0 /
    legacy_mlr=0 (ALL tripwires silent), 31 doctests. Only txmmgr.cpp. Byte-identical default.

28. **TERRAIN-DEADCODE-DELETE-1** (`75b8ebef`) — deleted the unreachable legacy gosRenderer tess-solid
    terrain draw. NOTE the recon's "no caller" was imprecise: terrainDrawIndexedPatches HAD a live call
    site at :7301 (dynamically unreachable, not reference-free). Replaced the dead :7291 true-branch
    draw with a skip+tripwire (preserves terrain-skip-not-basic-renderer semantics); deleted the
    callerless terrainDrawIndexedPatches function (:7052) + decl (:1752); updated stale comments
    (gos_terrain_bridge.h). 209 net lines removed. Build 0 (compile proves no other caller), smoke 2/2
    +0 destroys, terrain_path lod_chunk=2032 / all tripwires 0, 31 doctests. Shared callees
    (cacheTerrainUniformLocations / terrainBindThinUniformsForPatchStream / bindTerrainHeightTexUniforms
    / uploadDynamicShadowUniforms) RETAINED (used by shadow pre-pass + bridge). Byte-identical default.

29. **TERRAIN-BRIDGE-BODY-DELETE-1** (`1dc627fd`) — deleted dead PatchStream flush() + 4 flush-exclusive
    bridge helpers (beginBucketLoop/drawSingleBucket/endVertexDeclaration/end). ~729 net lines removed.
    Build 0 (compile proves exclusivity), smoke 2/2, terrain_path lod_chunk=2024 / all tripwires 0.
    RETAINED: getLastFlush* (snapshot), isReady/isOverflowed (txmmgr gate), beginFrame/init/destroy/
    append-chain, DrawIndirect (water reflection). FOLLOW-ON orphaned helpers (flush-exclusive, now dead,
    NOT yet deleted): gos_terrain_bridge_{drawSingleBucketTriangles:2945, endBucketLoop:2918,
    applyVertexDeclaration:2813, drawPatchStreamBucket:2833 (zero callers)} → TERRAIN-BRIDGE-BODY-DELETE-2.
- **FRAME-GRAPH-EXECUTOR-ISLAND-3** — another PP sub-stage (avoid composite/blit — inside owned endScene).
- **SHADOW-OBSERVE-2-REVISE** (recon `49ee249b`; ★KEY FINDING) — SHADOW-OBSERVE-2 instrumented the
  WRONG shadow site. Two shadow systems: STATIC (`gos_BeginShadowPrePass`/`beginShadowPrePass`, built
  ONCE per mission, gated `!gos_StaticLightMatrixBuilt()`, txmmgr:2599, world-fixed) vs DYNAMIC
  (`gos_BeginDynamicShadowPass`, txmmgr:2728 "RenderLists.DynamicShadowPass", PER-FRAME camera-fit).
  SHADOW-OBSERVE-2's noteRenderPass(ShadowCaster) is on the STATIC build (once/mission) → that is why
  it tripped ordering only on the one build frame + needed the runtime heuristic. FIX (Option A,
  refined from recon's Option C): (1) REMOVE noteRenderPass(ShadowCaster) from beginShadowPrePass
  (gameos_graphics.cpp:6217/6222 — wrong site, once/mission, doesn't belong in per-frame trace);
  (2) DELETE the runtime Mech-knownEarly heuristic (render_contract.cpp:762-786 — recon HIGH: it marks
  MechOpaque knownEarly on ANY Mech-before-Shadow frame, could mask a real steady-state regression);
  (3) KEEP the colorMaskOnEntry=Inherit relaxation (correct — shadow is FBO-depth-only). Shadow reverts
  to deferred per-frame observation (honest — the static-build observation was never per-frame). FUTURE
  SHADOW-OBSERVE-3: observe the PER-FRAME dynamic shadow at txmmgr:2728 / beginDynamicShadowPass
  (verify ambient colorMask/depthFunc/depthWrite valid there) AND resolve MechOpaque's preamble-note
  placement (:2362 fires before the actual mech draw, which is after shadow — the note site, not shadow,
  is the ordering root). ⚠ EDITS gameos_graphics.cpp + render_contract.cpp — serialize after the
  bridge-body-delete agent commits (it edits gameos_graphics.cpp).
30. **SHADOW-OBSERVE-2-REVISE-1** (`ee49ee48`, recon `49ee249b`) — SHADOW-OBSERVE-2 instrumented the
    WRONG shadow (static once-per-mission build, not the per-frame dynamic shadow). Ground-truthed:
    noteRenderPass(ShadowCaster) appeared ONCE (static build); beginDynamicShadowPass does NOT note.
    Removed the static-build note + the bug-masking Mech-knownEarly heuristic (render_contract.cpp);
    kept colorMaskOnEntry=Inherit. Verified out_of_order=0 from dump WITHOUT the heuristic
    (known_early_suppressed=1966=terrain-only), unobserved_total=9840 (Shadow honestly deferred).
    Build 0, smoke 2/2, 31 doctests. Net: dry-run model is now honest (no false 11/11, no masking).
    FUTURE **SHADOW-OBSERVE-3**: observe the per-frame dynamic shadow (gos_BeginDynamicShadowPass /
    beginDynamicShadowPass :6433, txmmgr:2728) — verify ambient axes valid there — AND fix MechOpaque's
    preamble-note placement (txmmgr:2362 fires before the actual mech draw which is after shadow; the
    note SITE is the ordering root, not shadow).

31. **FRAME-GRAPH-EXECUTOR-ISLAND-3** (`9bfbd3e5`) — owned **Shoreline** + **CloudShadow** (texture-safe:
    single call site, gated WillRun, restore blend+activeTexture0 on exit). SKIPPED **ScreenShadow**
    (units 0-4 incl 2D_ARRAY unit 3, no activeTexture0 restore — same leak class as shadowDebug; needs
    a body restore-fix first → SCREENSHADOW-RESTORE candidate). 33 doctests, build 0, smoke 2/2 OFF&ON,
    executor_owned_passes=16977 (~4/frame), validation_failures=0. 5 owned islands now: PostProcess
    (outer), EdgeFog, FogOob, Shoreline, CloudShadow.

32. **POSTPROCESS-SUBGRAPH-1-RECON-1** (`42a1ec0b`) + **POSTPROCESS-SUBGRAPH-1** (`142c7e41`) — Phase-4
    entry. New GL-free `RenderCore/postprocess_subgraph.h`: PostProcessSubpass table (6 rows in call
    order: CloudShadow:2458 → Shoreline:2464 → EdgeFog:2477 → FogOob:2484 → Composite:2489 →
    ShadowDebugOverlay:2653) + self-consistency validator + 6 offline tests. Added RenderResourceId
    ::MainNormal (=15) + ExecutorIslandId::{Composite,ShadowDebugOverlay}. 39 doctests, build 0, smoke
    2/2, modeling-only. Agent CORRECTED a recon error: Composite reads {MainColor} not {MainColor,
    Backbuffer} (writes backbuffer, doesn't read it). Flagged sceneObjectIdTex_ (unit 2) as another
    id-less intermediate. endScene full chain = 14 sub-stages (recon `42a1ec0b`); Slice 1 = these 6.

NEXT — advisor-confirmed order (resource identity → producer/consumer closure → subpass declaration →
owned island → state pack → same-order executor). RULE: every named subpass reads a named resource that
HAS a producer — do NOT let top-level PostProcess become a junk read-bucket; it should SUMMARIZE the
subgraph, not hide it.
33. **POSTPROCESS-MAINNORMAL-PRODUCER-1** (`24e5ade2`, HIGH-3) — MainNormal added to writes[] of
    StaticProp/Terrain/Mech (confirmed MainSceneMRT attachment-1; VegetationCards EXCLUDED unconfirmed)
    + PostProcess reads[]. 41 doctests.
34. **POSTPROCESS-SCENEDEPTHCOPY-RESOURCE-1** (`cee020e7`, HIGH-1) — ★cross-boundary: SceneDepthCopy
    (=16) produced by VFX (copySceneDepthForParticles, particle bridge; the gos_postprocess:1320 caller
    is a defensive re-copy inside drawBoxDecals) + added to PP subgraph external set. 45 doctests.
35. **POSTPROCESS-SCENEOBJECTID-RESOURCE-1** (`a9f6d613`) — SceneObjectId (=17, GBuffer2 COLOR_ATTACH2,
    gated IsObjectIdBufferEnabled) written by Mech+StaticProp (location=2; Terrain EXCLUDED — comment
    only) + Composite subpass reads[] + subgraph external. Closes the Composite-objectId gap. 50 doctests.
    → Active-pass PostProcess resource identities now COMPLETE (normal/depth-copy/object-id closed with
    producers). Remaining intermediates are default-OFF (Hzb/SSAO/compute) → SUBGRAPH-2.

★ SYNTHESIZED NEXT SEQUENCE (after both Phase-5 + Phase-7 recons landed `1209b544`/`ad2b1850`):
- **FRAMEGRAPH-STATEPACK-1-RECON-1** (`1209b544`): PipelineDesc ALREADY owns 8/13 axes incl SEMANTIC
  blend (BlendMode enum) + depth/cull/program. Only VAO/tex-units/scissor/stencil unmodeled (defer:
  tex-units intractable till NVIDIA-leak fix). First slice LOW-RISK = unify already-sampled axes into
  RenderStateDesc{pipelineId, colorMask, depthWrite, depthFunc, viewport, fboTarget}, reuse
  compareAmbient/fboMismatch. ★BUGS: 9 passes carry stale pipelineDescRegistered=false despite routing
  applyPipeline (fix in skeleton); UI blend runtime-dynamic → PipelineId::Invalid, executor skips.
- **SAME-ORDER-EXECUTOR-1-RECON-1** (`ad2b1850`): validate-only same-order ~45% ready, NEEDS NO STATEPACK
  (body sets own state). Wrappable NOW: StaticPropOpaque, Terrain(LOD-chunk), TerrainOverlay,
  TerrainDecal, VegetationCards (+PostProcess owned). Deferred slice-2: Shadow (1-line noteRenderPass
  gameos_graphics.cpp:6216 = SHADOW-OBSERVE-3), MechOpaque (HIGH: OpaqueObject PassIdentity lossy, no
  clean seam), Water/VFX/UI. HIGH cross-phase: markTerrainDrawn latch + g_dispatchMvp16 snapshot — safe
  under SAME-ORDER by construction (the reason we never reorder). Enabler: ~40-line
  executorOwnBeginTopLevel/EndTopLevel API + toplevel-owned bitmask counter.
- ★ADVISOR METRIC SPLIT (build into same-order slice): replace the lumped executor_owned_passes with
  executor_owned_wrappers / executor_validated_subpasses / executor_apply_state_subpasses /
  validation_failures — current ownership is "assert+call-unchanged"; keep counters honest.

CONCRETE ORDER (one slice at a time — do NOT branch the session):
1. ✓ **POSTPROCESS-SUBGRAPH-2** (`b756bfac`) — Phase 4 COMPLETE. Full 14-row endScene subgraph declared
   + validates offline. New ids HzbPyramid=18/SsaoOcclusion=19/SceneColorCopy=20. isCompute (3 rows:
   ClusterDepthPyramid/LightgridBuild/PostprocessComputeBlur) skipped by read-walk. 55 doctests.
   ★Ground-truth corrections: HzbReduce/HzbProbe are DRAW passes (hzbFBO_+glDrawArrays), not compute;
   ★ScreenShadow DOES restore glActiveTexture(GL_TEXTURE0):2153 — the real unsafe reason is the
   GL_TEXTURE_2D_ARRAY on UNIT 3 (CSM), so SCREENSHADOW-TEX-RESTORE-1 must unbind unit-3 2D_ARRAY (NOT
   activeTexture0; the frame_executor.h:19 note was stale, now corrected understanding).
2. ✓ **FRAMEGRAPH-STATEPACK-SKELETON-1** (`a6854d69`) — GL-free RenderStateDesc per top-level pass
   (RenderCore/render_state_desc.h) unifying ambient_contract + fbo_ledger + PipelineId; consistency
   validator proves 0 drift vs the per-axis ledgers; 62 doctests. PipelineId map (ground-truthed):
   Mech/StaticProp/Terrain/TerrainOverlay/TerrainDecal/Water = registered PipelineIds; Shadow/Veg/VFX/
   UI/PostProcess = Invalid (no single static pipeline; islands/sub-pipelines = right granularity).
   Fixed pipelineDescRegistered=true on 6 CONFIRMED (Terrain/Overlay/Decal/Water/VFX/PostProcess);
   Shadow/Veg/UI left false (confirmed not routed — recon's "9 stale" over-counted, 6 real).
   passHasStaticPipeline() tells the future apply-phase which passes have declarable pipeline state.
   (old scope note:) SCHEMA/VALIDATION ONLY (advisor redline): GL-free,
   NON-AUTHORITATIVE — defines vocabulary, does NOT drive the renderer. Scope: RenderStateDesc{pipelineId,
   colorMask, depthWrite, depthFunc, viewport, fboTarget} reusing PipelineDesc (semantic blend already in
   BlendMode), UI=PipelineId::Invalid, the stale-pipelineDescRegistered fix (9 passes), offline validator,
   ONE low-risk pass row. NON-GOALS (do NOT): glApplyState, texture bindings, VAO, scissor/stencil,
   compute DispatchStateDesc, any enforcement beyond existing probes.
3. ✓★ **SAME-ORDER-EXECUTOR-VALIDATE-1** (`0f4f2637`) — NORTH-STAR MILESTONE DONE+VERIFIED. Validate-only
   top-level executor ownership of 5 passes (StaticProp/Terrain-LODchunk/TerrainOverlay/TerrainDecal/
   Vegetation) via RAII begin/end guards (clean, additive — pin invariant held). New
   RenderCore/top_level_pass_executor.h + executorOwnBeginTopLevel/EndTopLevel (render_contract.cpp,
   reuses compareAmbient/fboMismatch). 70 doctests, build 0, smoke 2/2 OFF&ON. ★VERIFIED split metrics
   (after a clean rebuild — the agent's deploy had a stale debug_state_dump.obj; commit source correct):
   executor_validated_top_level_passes=4800 (~2/frame StaticProp+Terrain), apply_state=0, scheduled=0,
   validation_failures=0, owned_wrappers=9601. Deferred Shadow/Mech/Water/VFX/UI (slice 2).
   ⚠LESSON: verify the ACTUAL dump (rebuild+redeploy if fields missing), not just an agent's stderr claim
   — an agent build+deploy can leave a stale .obj for one TU. The graph now OWNS the top-level frame in
   current order, conservatively. (old scope note below.)
3b. **(was) SAME-ORDER-EXECUTOR-VALIDATE-1** — VALIDATE-ONLY (precise wording; NOT full/scheduler/state-owning).
   Body still sets its own state; executor validates + wraps. Top-level ownership of StaticPropOpaque,
   Terrain(LOD-chunk), TerrainOverlay, TerrainDecal, VegetationCards (+PostProcess owned).
   executorOwnBeginTopLevel/EndTopLevel API. ★PIN INVARIANT: must NOT disturb markTerrainDrawn latch
   timing, g_dispatchMvp16 snapshot timing, knownEarly terrain handling, body-owned state setup, or
   existing call order — THAT is why same-order is safe. ★SPLIT METRICS: executor_owned_wrappers /
   validated_top_level_passes / validated_subpasses / apply_state_passes / scheduled_passes /
   validation_failures / skipped_deferred_passes. Expected: apply_state=0, scheduled=0, failures=0.
   DEFERRED (do NOT include to inflate %): Shadow (dynamic-seam unmodeled — SHADOW-OBSERVE-3),
   MechOpaque (HIGH: OpaqueObject identity lossy, needs clean seam), Water/VFX/UI → slice 2.
   Gate OFF byte-identical, ON smoke tier1 green.
4. ✓ **FRAMEGRAPH-APPLY-STATE-ISLAND-1** (`477d69d3`) — FIRST state-APPLY. Executor pre-applies EdgeFog's
   bindFB(sceneFBO_)+SingleColor+viewport+applyPipeline(PostProcessEdgeFog) before the body (idempotent,
   matches runEdgeFog:2293-2306 exactly). New GL-free SubStageStateDesc table (4 rows: EdgeFog/FogOob/
   Shoreline/CloudShadow {islandId, pipelineId, fboTarget, viewport} — EdgeFog WIRED, other 3 declared).
   74 doctests, build 0, smoke 2/2 OFF&ON; VERIFIED actual dump executor_apply_state_passes=1999,
   validation_failures=0, validated_top_level=3998. Byte-identical (double-set is write-same-register).

DONE: FRAMEGRAPH-APPLY-STATE-ISLAND-2 (`d3f9362d`, FogOob/Shoreline/CloudShadow apply; dump
apply_state_passes=6081, failures=0). 4 sub-stages now apply state.

DONE: **APPLY-STATE-REDUNDANT-BODY-REMOVE-2** (`c706dde6`) — extends REMOVE-1 (EdgeFog `95f4d498`) to the
3 remaining ISLAND-2 sub-stages: FogOob/Shoreline/CloudShadow each got a per-island
`<island>StateAppliedByExecutor_` flag so their bodies SKIP the 4 redundant setup calls (bindFB/
SingleColor/viewport/applyPipeline) when the executor pre-applied them (MC2_FRAMEGRAPH_EXECUTOR ON);
byte-identical OFF (body still applies when the gate didn't run). Mirrors REMOVE-1's first-true-state-
ownership pattern → all 4 ISLAND-2 sub-stages now have the executor as SOLE state setter under the gate.
Directly serves the advisor "idempotent double-set is the ENEMY" goal: REMOVE-1 proved it for EdgeFog,
REMOVE-2 extends first-true-state-ownership to the other 3 islands. VERIFIED (build 0, deploy 0 to
mc2-win64-0.4c, src_commit c706dde6): gauntlet (verify_executor_slice.py --with-dryrun) PASS — OFF all
executor metrics=0 + smoke exit 0; ON validation_failures=0, owned_wrappers=9441, apply_state_passes=5664,
validated_top_level=7552, exit 0; DRYRUN out_of_order=0, ambient_mismatches=0, fbo_mismatches=0,
frames=2281, exit 0. Reviewer subagent PASS (all 7 checks; body/WillRun gates match → no flag leak).
79 doctests (FrameGraph suite, 0 failed).

DONE: **APPLY-STATE-SCREENSHADOW-1** (`6d316519`) — extends the body-skip ownership to the 6th PostProcess
island. The ScreenShadow island (previously owned validate-only, EXECUTOR-ISLAND-SCREENSHADOW-1 `fef58925`)
now APPLIES its declared GL state via the executor AND skips the body's 4 redundant setup calls (FBO/
SingleColor/viewport/applyPipeline) via a new per-island flag `screenShadowStateAppliedByExecutor_`,
mirroring ISLAND-2 + REMOVE-2. New PipelineId::PostProcessScreenShadow=21. Byte-identical OFF (body still
applies when the gate didn't run). VERIFIED (slice-preflight PASS, gate-match confirmed no flag leak,
build 0, deploy 0 to mc2-win64-0.4c src 6ace513f): gauntlet PASS — OFF all executor metrics=0 + smoke
exit 0; ON validation_failures=0 / owned_wrappers=9881 / apply_state_passes=7904 (up from 5664 —
ScreenShadow now applies) / validated_top_level=7904 / exit 0; DRYRUN out_of_order=0 /
ambient_probe_mismatches=0 / fbo_mismatches=0 / frames=1984 / exit 0. 80 doctests. ⇒ All 6 PostProcess
apply-state islands (EdgeFog/FogOob/Shoreline/CloudShadow/ScreenShadow + outer endScene) now
executor-applied; the 4 endScene sub-stages + ScreenShadow are executor-sole-setter (body-skip).

★ADVISOR REFINED ORDER (the push from "validated" → "owned"; idempotent double-set is now the ENEMY —
a transition strategy, NOT end-state. End-state = executor applies + body DRAWS ONLY):
- **SCREENSHADOW-TEX-RESTORE-1** (IN FLIGHT, w/ BRIDGE-DELETE-2) — unbind leaked unit-N CSM 2D_ARRAY at
  runScreenShadow exit. Fix the leak BEFORE owning the pass.
1. ✓ **EXECUTOR-ISLAND-SCREENSHADOW-1** (`fef58925`) — owned ScreenShadow (leak-fixed) as the 6th
   PostProcess island, validate-only. ✓ **APPLY-STATE-SCREENSHADOW-1** (`6d316519`) — DONE: ScreenShadow
   now executor-applies its declared state + body-skips the 4 redundant setup calls via
   `screenShadowStateAppliedByExecutor_` (PipelineId::PostProcessScreenShadow=21). All 6 PostProcess
   apply-state islands are now executor-applied; gauntlet PASS (ON owned_wrappers=9881 /
   apply_state_passes=7904 / failures=0; DRYRUN out_of_order=0; OFF=0), 80 doctests.
2. ✓ **APPLY-STATE-REDUNDANT-BODY-REMOVE-1** (`95f4d498`, EdgeFog ONLY) — DONE. ★the first TRUE
   state-ownership proof. EdgeFog's body NO LONGER applies its own FBO/drawBuffers/viewport/applyPipeline;
   the executor's pre-apply is the sole setter. Body STILL applies when gate OFF → byte-identical OFF.
   ✓ **APPLY-STATE-REDUNDANT-BODY-REMOVE-2** (`c706dde6`) — extended REMOVE-1 to the other 3 islands
   (FogOob/Shoreline/CloudShadow) via per-island `<island>StateAppliedByExecutor_` flags. All 4 ISLAND-2
   sub-stages now executor-sole-state-setter under the gate. Gauntlet PASS (see ledger entry above):
   ON validation_failures=0 / owned_wrappers=9441 / apply_state_passes=5664, DRYRUN out_of_order=0,
   byte-identical OFF, 79 doctests.
3. **SHADOW-OBSERVE-3 recon** — model the dynamic-shadow seam honestly (do NOT rush). 
4. **MECHOPAQUE-PASSIDENTITY-RECON-1** — disambiguate the lossy OpaqueObject identity (do NOT rush).
5. **SAME-ORDER-EXECUTOR-SLICE-2** — Shadow/Mech DONE (`d32e8990`); ✓ **Water DONE**
   (WATER-SAME-ORDER-VALIDATE-1 `33f590f8`+`110fb605` — 8th owned pass). Remaining top-level deferred:
   **VFX, UI** (UI blend runtime-dynamic → PipelineId::Invalid). NEXT: VFX same-order recon.
Updated north-star: same-order executor owns pass/subpass validation + declared STATE APPLICATION +
resource/FBO setup + ZERO hidden body-side state for owned islands. THEN: own all safe passes / ban raw-GL
bypasses (named exceptions) / centralize state+resource application. ONLY THEN talk reorder/scheduler.
STANDING RULE: verify the ACTUAL dump from a rebuilt+deployed exe, not agent claims (stale-.obj caught once).

★★ CRITICAL HARNESS BUG (caught `fef58925`, EXECUTOR-ISLAND-SCREENSHADOW-1): `MC2_FRAMEGRAPH_EXECUTOR`
was MISSING from run_smoke.py's mission-Popen env ALLOWLIST (scripts/run_smoke.py ~:1386) until that
commit — so ALL prior `MC2_FRAMEGRAPH_EXECUTOR=1` smoke runs (ISLAND-1/2/3, SAME-ORDER-VALIDATE-1,
APPLY-STATE-1/2) silently ran the executor GATE-OFF. The byte-identical-OFF safety property held (so
nothing shipped broken — executor is default-OFF), but the ON behavior was NEVER actually exercised by
those smokes. ⇒ Re-verified DEFINITIVELY post-fix (manual OFF vs ON, the corrected harness):
  OFF: all executor metrics 0, smoke PASS +0.   ON: owned_wrappers=9481, validated_top_level=4740,
  apply_state_passes=7110, validation_failures=0, scheduled=0, smoke PASS 2371 frames +0 destroys.
⇒ The ENTIRE executor stack (6 validate-islands + ScreenShadow, 4 apply-state sub-stages, 5 top-level
validate passes) is now GENUINELY verified ON, 0 failures, byte-identical OFF. All future executor
smokes are genuine. LESSON: a default-OFF env gate is only verified if the smoke harness actually
PASSES the var to the child — check the run_smoke.py allowlist for any new MC2_* gate.

✓ EXECUTOR-ISLAND-SCREENSHADOW-1 (`fef58925`) — 6th owned island (validate-only); postconditions
ground-truthed (blend disabled :2150, activeTexture0 :2160, sceneFBO_); 79 doctests; +the harness fix.
4z. **(was) FRAMEGRAPH-APPLY-STATE-ISLAND-1** (recon DONE `fccf9a82`) — first APPLY-state slice. Target =
   **EdgeFog** (PipelineId PostProcessEdgeFog=18 registered; FBO=sceneFBO_/MainColor; viewport full-scene;
   single tex unit 0; default-ON every terrain frame). Executor PRE-APPLIES before the body: applyPipeline
   (PostProcessEdgeFog) + glBindFramebuffer(sceneFBO_) + setSceneDrawBuffers(SingleColor) + glViewport.
   NOT pre-applied: program/uniforms/tex/teardown (body-owned). Byte-identical: all are write-same-register
   setters → OFF body-only identical, ON idempotent double-set identical. NEW `SubStageStateDesc` table in
   frame_executor.h keyed on ExecutorIslandId (4 rows EdgeFog/FogOob/Shoreline/CloudShadow {islandId,
   pipelineId, fboTarget=MainColor, viewport=MainScene}); kPassRenderState PostProcess row stays Invalid/
   Unknown (do NOT change). Needs pp->executor accessors (sceneFbo/width/height/doSingleColorDrawBuffers).
   executor_apply_state_passes increments (the split metric goes nonzero). Self-gated MC2_FRAMEGRAPH_EXECUTOR.
   Slice 2+: remove the body's now-redundant apply (executor owns it). Then expand validate-only / slice 2.

(old remaining list:)
4b. **POSTPROCESS-SUBGRAPH-2** — declare the remaining endScene sub-stages (Hzb/SSAO/compute/BoxDecals;
   ScreenShadow as ownedByExecutor=false) + intermediate ids (HzbPyramid/SsaoOcclusion/SceneColorCopy).
5. **FRAMEGRAPH-STATEPACK-1** (Phase 5) — per-pass RenderStateDesc packs; blend SEMANTIC not ON/OFF.
6. **SAME-ORDER-EXECUTOR-RECON-1** (Phase 7 north-star) — map executor walking kFramePassOrder owning
   each pass in order.
Side fixes (when convenient): **SCREENSHADOW-TEX-RESTORE-1** (add activeTexture0/2D_ARRAY-unbind restore
to screenShadow exit — KEEP it deferred from owned/subgraph-safe until then) → later EXECUTOR-ISLAND-
SCREENSHADOW-1; **TERRAIN-BRIDGE-BODY-DELETE-2** (4 orphaned helpers); **SHADOW-OBSERVE-3**.
- **SCREENSHADOW-RESTORE** — add the activeTexture0/2D_ARRAY-unbind restore to screenShadow's exit so it
  becomes a texture-safe ownable island (then ISLAND-4 can own it). Mirror GLSTATE-SHADOWDEBUG-2DARRAY-1.
- ~~**TERRAIN-BRIDGE-BODY-DELETE-2** — the 4 orphaned flush-exclusive helpers (#29 follow-on).~~
  ✓ **ALREADY-DONE** (stale line — the orphaned helpers + the now-unreachable LegacyMLR bump are gone).
- **SHADOW-OBSERVE-3** — observe the per-frame dynamic shadow + fix MechOpaque preamble-note placement.
- DELETION GATE: keep watching terrain_path counters == 0 across more real playtest/capture/editor
  before deleting the bridge BODIES (deletion gated on runtime 0, not grep — the tripwires now enforce it).

DEFERRED (do not build yet):
- **Texture-latch ledger** — recon proved the renderer is deferred-screen-shadow with ZERO inherited-
  binding ghosts; a ledger is LOW-value (regression-prevention only, catches no current bug). Skip.
- **Ambient authority-inversion** (globals → shims over gos_FrameCtx) — after the contract proves out.

## ✓ RESOLVED (2026-06-29) — SAME-ORDER-SLICE-2 is now GREEN
**MECHOPAQUE-ORDER-FIX-2 (`d32e8990`)** closed the RED: removed the enqueue-time OpaqueObject note at
`mclib/tgl.cpp:3016` (the trace-polluter both recons missed — OpaqueObject had TWO note sites; the
enqueue one fired before StaticProp's draw-flush) + swapped kFramePassOrder to [Shadow, StaticPropOpaque,
MechOpaque, ...] + updated dryrun tests (b/c/f). VERIFIED on the 0.4c dump via `verify_executor_slice.py
--parse-only`: **out_of_order 1248→0**, validation_failures=0, ambient_probe_mismatches=0, fbo_mismatches=0,
validated_top_level>0 (ownership intact). 79 doctests, build 0, byte-identical rendering. SAME-ORDER
ownership of Shadow + MechOpaque is now HONEST and complete (Water/VFX/UI remain deferred — real gaps).
⚠ TOOLING CAVEAT: the MCP `get_executor_health` reads the CANONICAL `mc2-win64-v0.4` install (hardcoded),
NOT this session's `mc2-win64-0.4c` deploy — so it shows a STALE pre-fix dump (out_of_order=1248). Use
`verify_executor_slice.py --parse-only <0.4c dump>` (takes a path arg) for the real verdict, OR
deploy d32e8990 to v0.4 + a dryrun smoke to refresh the canonical dump. FOLLOW-UP: give
get_executor_health a `dump_path` arg (or point it at the session deploy dir). The v0.4 install is the
shared primary — NOT redeployed this session by convention (we use 0.4c spare).

## ✓ WATER-SAME-ORDER-VALIDATE-1 (`33f590f8` + fixup `110fb605`) — 8th owned top-level pass
36. **WATER-SAME-ORDER-VALIDATE-1** — Water is now the **8th** validate-only top-level executor-owned
    pass (mirrors TerrainOverlay). Added Water rows: `kTopLevelExecutorPasses` (validateAmbient=true,
    validateFbo=true), `ambient_contract` (depthFunc=GEQUAL; colorMask/blend/depthWrite=Inherit),
    `fbo_ledger` (MainColor). `kTopLevelDeferredPassCount` 3→2 (now VFX, UI only). Touched
    `top_level_pass_executor.h` + `ambient_contract.h` + `fbo_ledger.h` + `gameos_graphics.cpp` +
    `test_frame_graph.cpp` (33f590f8); fixup 110fb605 = `ambient_contract.h` depthWrite On→Inherit.
    VERIFIED: 80 doctests pass, build 0, deploy 0 (src 110fb605). Gauntlet PASS — OFF all executor
    metrics=0; ON validation_failures=0 / validated_top_level=9805 (~+1/frame from 7904) /
    owned_wrappers=9806; DRYRUN out_of_order=0 / ambient_probe_mismatches=0 / fbo_mismatches=0; all
    exits 0. ★MEASURE-FIRST CATCH: first gauntlet had **ambient_probe_mismatches=1971** — a depthWrite
    mismatch (declared On vs live Off). The begin-ambient sample fires at function ENTRY, BEFORE
    applyPipeline(WaterArmed) AND before water's depth-mask-off → live dw=Off at the seam. Relaxed
    depthWrite→Inherit (depthFunc=GEQUAL kept; sampled clean). ★LESSON: the recon ground-truthed
    "depthWrite=On from the WaterArmed pipeline" — that is the ESTABLISHED-state value, NOT the
    seam-ENTRY-state value the begin-sample actually sees. begin-sample entry-state ≠ established-state;
    declare contracts against the seam's observed state, not the eventual pipeline state. Textbook
    measure-first win. ★ScreenShadow reconciliation DONE (clean rebuild, exe SHA match, apply_state=7976).

## ✓ VFX-FBO-ONLY-VALIDATE-1 (`15c5ca1e`) — 9th owned top-level pass (FBO-only)
37. **VFX-FBO-ONLY-VALIDATE-1** — VFX is now the **9th** validate-only top-level executor-owned pass,
    deliberately **FBO-ONLY** (mirrors VegetationCards). Added VFX rows: `kTopLevelExecutorPasses`
    (validateFbo=true→MainColor, **validateAmbient=FALSE**), `fbo_ledger` (MainColor). New
    `mc2_vfx_pass_begin`/`mc2_vfx_pass_end` C-shims (`render_contract.cpp`) forwarding to
    `executorOwnBeginTopLevel`/`EndTopLevel(PassIdentity::ParticleEffect → RenderPassId::VFX)`; RAII
    guard in `gamecam.cpp` (~:594) covers `Batcher::Flush` + `gos_tube_ribbon_flush_deferred`.
    `kTopLevelDeferredPassCount` 2→1 (now **UI only**). Touched `top_level_pass_executor.h` +
    `fbo_ledger.h` + `render_contract.cpp` + `gamecam.cpp` + `test_frame_graph.cpp` (15c5ca1e).
    ★WHY AMBIENT DEFERRED (deliberate scope): the VFX note seam at gamecam:594 fires BEFORE the particle
    body sets state AND in a DIFFERENT TU (reached via the C-shim), and it fires even on empty frames /
    over 2 distinct draw entrypoints — so ambient state is NOT honestly declarable at that seam (same
    rationale that kept VegetationCards FBO-only). VERIFIED: 80 doctests pass, build 0, deploy 0
    (src 15c5ca1e). Gauntlet PASS — OFF all executor metrics=0; ON validation_failures=0 /
    validated_top_level=11982 (up from ~9805) / owned_wrappers=9986; DRYRUN out_of_order=0 /
    fbo_mismatches=0 / ambient_probe_mismatches=0 / 2001 frames; all exits 0.
    FOLLOW-UPS: (a) **VFX-AMBIENT-VALIDATE** — needs a LIVE measurement of ambient at the gamecam:594
    seam (entry-state, not eventual pipeline state — cf. the WATER measure-first catch above) BEFORE
    declaring any ambient axis. (b) **UI** is now the LAST deferred top-level pass — advisor ranks it the
    HARDEST: runtime-dynamic blend/scissor, HUD/editor coupling, PipelineId::Invalid → needs its own recon.

## ✓ UI-SAME-ORDER-VALIDATE-1 (`85369151`) — 10th & LAST owned top-level pass — ★MILESTONE 10/10
38. **UI-SAME-ORDER-VALIDATE-1** — UI/HUD is now the **10th & LAST** top-level executor-owned pass,
    deliberately **FBO-ONLY** (Backbuffer — the **FIRST** `kPassFboTarget` row using **Backbuffer**;
    ambient is **DO_NOT_MODEL** — legacy per-draw gos state → `validateAmbient=FALSE`). Direct call
    (no C-shim — the GameOS TU includes the contract header directly); RAII guard mirrors Water.
    `kTopLevelDeferredPassCount` 1→**0**. Touched `top_level_pass_executor.h` + `fbo_ledger.h` +
    `gameos_graphics.cpp` + `test_frame_graph.cpp` (85369151).
    ★**MILESTONE — every top-level pass is executor-owned (10/10, 0 deferred). Same-order top-level
    VALIDATE coverage is COMPLETE.** VERIFIED: 80 doctests / 510 assertions pass, build 0, deploy 0
    (src 85369151). Gauntlet PASS — OFF all executor metrics=0; ON validation_failures=0 /
    validated_top_level=13903 (up from ~11982) / owned_wrappers=9931; DRYRUN out_of_order=0 /
    fbo_mismatches=0 (Backbuffer row matches — UI lands on FBO 0) / ambient_probe_mismatches=0;
    all exits 0. Byte-identical OFF.
    DECISIONS this session (for next session):
    - **VFX-AMBIENT-VALIDATE: DROPPED permanently.** The VFX begin-seam (gamecam:594) is structurally
      upstream of the cross-TU particle body that sets state; only depthFunc=GEQUAL would be honestly
      declarable (redundant w/ Water). VFX stays FBO-only. (Throwaway measurement recipe exists if needed.)
    - **Side finding (future one-liner cleanup, NOT done):** `kParticleEffectState`
      (render_contract.cpp:315) declares blend=Additive but the live particle body uses **alpha-blend**
      (SRC_ALPHA/ONE_MINUS_SRC_ALPHA, gos_particle_bridge.cpp:1164) — stale PassStateContract entry.

## ✓ Top-level APPLY-STATE rollout (entries 39–43) + per-pass counters
39. **APPLY-STATE-TERRAINDECAL-1** (`2bf12b30`) — FIRST top-level scene-pass apply-state; built the
    REUSABLE top-level apply infra: `kTopLevelStateDesc` / `findTopLevelStateDesc`
    (top_level_pass_executor.h), the `gosRenderer::executorApply<X>State` pattern, the
    `<x>StateAppliedByExecutor_` one-shot flag, `isTopLevelExecutorEnabled()` gate, and the
    `mc2_framegraph_executor_bump_apply_state()` counter shim. TerrainDecal = first consumer (body
    skips applyPipeline when the executor pre-applied). ⚠ **NOT tier1-runtime-exercised** — the decal
    pass does not run in tier1 maps (exercise via mc2_17 + `MC2_DYNAMIC_DECALS`).
40. **APPLY-STATE-TERRAINOVERLAY-1** (`4d87c563`) — second consumer, same infra. ⚠ **ALSO not
    tier1-exercised.** ⚠⚠ OPEN QUESTION: recon found the LIVE overlay draw is `drawDecalStaticBatch`
    (gameos_graphics.cpp:9617, default-ON `MC2_TERRAIN_INDIRECT_OVERLAY`) but the slice may have
    instrumented the DORMANT `drawTerrainOverlays` (:9798). An mc2_02 exercise-smoke is in flight to
    arbitrate via the per-pass counter — if the TerrainOverlay counter == 0 on mc2_02, a SITE-FIX
    slice is needed (move the hook to `drawDecalStaticBatch`).
41. **APPLY-STATE-STATICPROP-1** (`129df9c9`) — third consumer; ★**FIRST RUNTIME-PROVEN top-level
    apply-state** (StaticProp runs every tier1 frame). A/B build attribution: apply +0.46/frame from
    the dispatch (1.83→2.29/frame), render-correct +0 destroys, byte-identical OFF, 88 doctests.
    Single applyPipeline lift; FBO/MRT/objectId-SSBO/viewport inherited.
42. **RAW-GL diff-gates Phase 8** (entries 42a–42d; all script-only, regression-proof, freeze backlog,
    comment-safe, negative-tested, wired into `scripts/check-contracts.sh`):
    - 42a. **RAW-GL-DEPTHFUNC-DIFF-GATE-1** (`7339dd90`, 49 sites)
    - 42b. **RAW-GL-DEPTHMASK-DIFF-GATE-1** (`33820a2f`, 72 sites)
    - 42c. **RAW-GL-COLORMASK-DIFF-GATE-1** (`ff9fed17`, 33 sites incl. `glColorMaski`)
    - 42d. **RAW-GL-BLENDFUNC-DIFF-GATE-1** (`00c61255`, 35 sites — Func/FuncSeparate/Equation)
    4 raw-GL axes now enforced. **FBO-bind gating DEFERRED** (too numerous / context-sensitive —
    needs its own recon, NOT a clone of these).
43. **PER-PASS-APPLY-COUNTERS-1** (`0e0b582a`) — replaced the coarse aggregate with an
    `executor_apply_state_by_pass` map (8-entry `ApplyPassId` enum; aggregate DERIVED from the sum →
    no drift). 89 doctests. ★VERIFIED ON tier1 dump: StaticPropOpaque=1993, TerrainDecal=0,
    TerrainOverlay=0, EdgeFog/FogOob/Shoreline/ScreenShadow=1993, CloudShadow=0 (gated),
    aggregate==sum==9965. The decal/overlay false-inference is now structurally impossible.

## ✓ Overlay site-fix + content-unexercisable finding + verify/registration automation (entries 44–46)
44. **verify_executor_slice.py upgrades** (`6f6d1454`) — FIXED the recurring false `stale_deploy_check`
    WARN (now parses `src_commit` as the per-row column 3, not a top-level line; genuine staleness still
    flagged) + added `--assert-pass-fired NAME[:MIN]` (reads `executor_apply_state_by_pass`;
    StaticPropOpaque:1→exit0, TerrainDecal:1→exit1, bogus→error-with-valid-names). Automates the manual
    dump-reading + kills the WARN noise.
45. **APPLY-STATE-REGISTRATION-CHECK-1** (`20d89a3b`) — offline doctests (top-level apply ⇒ concrete
    non-Invalid pipelineId for TerrainDecal/Overlay/StaticProp; PostProcess apply ⇒ matching kSubStageState
    row + island in kExecutorIslands) [FrameGraph 89→91] + new `scripts/check-apply-pass-bumped.py`
    grep-gate (every ApplyPassId bumped exactly once across engine TUs; negative-tested; wired into
    check-contracts.sh after raw_gl_blendfunc). ⇒ Apply-state wiring can't half-ship.
46. **APPLY-STATE-TERRAINOVERLAY-SITE-FIX-1** (`bd645aa6`) — relocated the TerrainOverlay executor
    validate+apply hooks to the LIVE draw site `drawDecalStaticBatch` (gameos_graphics.cpp:9908;
    applyPipeline at :9970) — the dormant per-tri `drawTerrainOverlays` (:9799) is skipped under default-ON
    `MC2_TERRAIN_INDIRECT_OVERLAY` (this resolves the entry-40 OPEN QUESTION: yes, the hook was on the
    dormant site). Kept the dormant-site instrumentation too (mutually exclusive per the indirect gate →
    no double-own; preserves ownership in the gate-OFF editor path). Reused the existing kTopLevelStateDesc
    row + `executorApplyTerrainOverlayState()` + `overlayStateAppliedByExecutor_` flag. Tier1 gauntlet
    PASS (validated_top_level=13567, validation_failures=0; DRYRUN out_of_order/ambient/fbo=0), byte-
    identical OFF, 91 doctests. ★Site-correct now — but still CONTENT-UNEXERCISABLE in stock smoke (see
    the three-state dashboard finding above: stock maps have zero road/runway/bridge `&Overlays` tiles;
    mc2_02 cement is drawn by the TERRAIN-SOLID path, not the overlay pass → counter stays 0).

## ✓ Pipeline-only apply-state COMPLETE (entries 47–48) — Mech + Water
47. **APPLY-STATE-MECHOPAQUE-1** (`efa73c71`; +script-allowlist `189f0b75`) — 4th top-level scene-pass
    apply-state. Added `ApplyPassId::MechOpaque`. Dispatch is at the flush CALL SITE (txmmgr.cpp:3271-3273),
    **NOT the begin seam**; body-skip at gos_mech_batcher.cpp:2152; `flushShadow` untouched. ★RUNTIME-PROVEN:
    MechOpaque counter=**1890/frame** (== StaticProp), `verify_executor_slice.py --assert-pass-fired
    MechOpaque` exit 0, byte-identical OFF, 92 doctests, tier1 gauntlet GREEN. This lifts the GREEN-RECON-
    PARKED Mech (the redline is now cleared by the runtime proof at the dispatch site).
48. **APPLY-STATE-WATER-1** (`2b217ee9`) — 5th top-level apply-state. ★**BODY-SITE apply** (not the begin
    seam): the dispatch fires immediately BEFORE the existing `applyPipeline(WaterArmed)` at
    gameos_graphics.cpp:3276, AFTER the mid-body compute dispatch `ComputeDispatchAndBindThinRecords`:3232
    — mirrors the overlay SITE-FIX rationale (a seam pre-apply would be mistimed). Pipeline-only lift (Water
    inherits the scene FBO/viewport). `ApplyPassId::Water`. RUNTIME-PROVEN: Water counter=**1784/frame** on
    mc2_01 (water on-camera), `--assert-pass-fired Water:1` PASS, byte-identical OFF, 93 doctests, gauntlet
    GREEN. Also fixed a latent `check-apply-pass-bumped.py` MULTILINE comment-strip bug.

★**MILESTONE — PIPELINE-ONLY APPLY-STATE PHASE COMPLETE.** Every top-level pass whose state lift is a
single `applyPipeline` is now executor apply-state owned. See the UPDATED THREE-STATE CLASSIFICATION near
the resume pointer. The remaining apply-state target (Shadow) needs the EXTEND design below.

## ✓ APPLY-STATE-EXTEND + SHADOW FULL-MODE + REGISTRY (entries 49–53) — 2026-06-30 reboot batch
49. **FRAMEGRAPH-APPLY-STATE-EXTEND-1** (`6def9bd2`) — added `enum ClearSpec{None,DepthForwardZ}` + a
    `clear` field to `TopLevelStateDesc` + the shared GL-free helper `applyTopLevelGenericAxes(desc,fbo,w,h)`
    (new header `RenderCore/top_level_apply_axes.h`; def in gameos_graphics.cpp). The helper applies
    FBO→viewport→clear with a skip-sentinel per step; the pipeline is applied by the caller. ★SELF-PROVEN:
    re-expressed the StaticPropOpaque row with explicit MainColor/MainScene targets → **fbo_mismatches=0
    over 11496 samples** == byte-identical to the previous inheritance, +0 destroys, 94 doctests. The other
    4 pipeline-only consumers stay byte-identical (skip-sentinel defaults). ⇒ unblocks Shadow's
    pipeline+depth-clear lift.
50. **get_executor_health per-pass surface** (`447a1de7`) — the render-state MCP tool now surfaces
    `executor_apply_state_by_pass` per-pass (not just the aggregate).
51. **APPLY-STATE-SHADOW-1** (`6ea1a42a`; orig `075014bc` pre-rewrite — see INDEX-RACE) — Shadow owns
    pipeline + forward-Z depth-clear at the DYNAMIC seam (`beginDynamicShadowPass`) via the EXTEND helper.
    Added `ApplyPassId::Shadow`. Row fboTarget=Unknown / viewport=Inherit (deferred to SHADOW-2).
    ★Byte-identical shadow render (ON-vs-OFF `MC2_SHADOW_STATE_TRACE` character-identical:
    clearDepth=0/restored=1/0-leaks), Shadow counter=**1289/frame**, 95 doctests. Also hardened
    `check-apply-pass-bumped.py` (comment-strip-before-comma-split bug).
52. **APPLY-STATE-SHADOW-2** (`2a3b0967`) — Shadow is now the FULL render-target-mode owner
    (FBO+viewport+clear+pipeline). Row fboTarget=**ShadowDynamicMap** / viewport=**ShadowMap**; the helper
    applies all 4 in order. ★Dispatch at the FBO-bind position, with the AMD-feedback-unbind +
    `GL_TEXTURE_COMPARE_MODE` flip PRESERVED as a body preamble BEFORE it (shadow-texture state must
    precede the FBO bind). fbo_mismatches=0, trace character-identical, Shadow=**1608/frame**,
    byte-identical OFF, all redlines held.
53. **REGISTRY-TERRAIN-SSBO-1** (`49921b5b`) + **REGISTRY-MATERIAL-SSBO-1** (`f60b7ea2`) — registered the
    terrain SSBO/atlas ids (TerrainHeightSsbo=13 / TerrainRecipeBuffer=21 / TerrainThinBuffer=2220 /
    TransitionMaskArray=265 live glNames; CementAtlas on cement maps) + MaterialGpuBuffer (gate
    MC2_MATERIAL_GPU, glName=876). Registry live count 6→**10** (default) / 11–12 (cement / material-gate).
    Observe-only metadata, byte-identical. ★PARALLEL-BUILT in the spare worktree `mc2-nifty-land` (separate
    build64) CONCURRENTLY with the Shadow lane (smokes lease-serialized), then cherry-picked clean onto
    nifty — see the validated spare-worktree pattern.

## ✓ TIER-B FINISH BATCH (entries 54–58) — 2026-06-30, HEAD `40e0f5af`
54. **RAW-GL-BYPASS-CAPSTONE-1** (`567af186`) — Phase-8 enforcement capstone. New
    `scripts/check-raw-gl-capstone.py` asserts all 5 axis gates (depthFunc/depthMask/colormask/blendfunc/
    fbobind) EXIST + are wired into check-contracts.sh + are non-stub (negative-tested: renaming a gate →
    exit 1). Plus a KNOWN_DEFERRED axis ledger (viewport/activeTexture/scissor/stencil/cull — explicit-
    not-forgotten). ⇒ the enforcement set is now itself regression-proof (a deleted/renamed gate fails CI).
55. **REGISTRY-POSTPROCESS-FBO-1** (`9e7ed61e`) — registered MainColor (closed the FBO-ledger-only
    asymmetry) + MainNormal / SceneObjectId / SsaoOcclusion (live glNames) + SceneDepthCopy / HzbPyramid
    (gated-lazy). Registry live count 10→**14**.
56. **REGISTRY-COMPUTE-IDS-1** (`0c8102fa` spare → cherry-picked `0d7aa9fe`) — added 4 enum ids
    (ClusterDepthPyramid / LightgridGrid / LightgridIndex / PostprocessComputeBlur) + their gen-site
    registrations; default-OFF gated-absent. ★PARALLEL-BUILT in the spare worktree (validated pattern).
57. **REGISTRY-SCENECOLORCOPY-PRODUCER-1** (`7e407910`) — registered SceneColorCopy + added its producer
    edge (VFX writes[], 3/4 cap) — closed the LAST id-without-producer gap; `validateReadsSatisfied` clean.
58. **REGISTRY-LIFETIME-CLASS-1** (`40e0f5af`) — ★registry capstone. Added
    `RenderResourceLifetime{Unset,FrameLocal,Mission,Persistent,External}` on every registration:
    **Persistent** (6+): MainColor/Depth/Normal/SceneObjectId/Hzb/ShadowStatic/ShadowDynamic/
    MaterialGpuBuffer; **FrameLocal**: Ssao/SceneDepthCopy/SceneColorCopy/compute-ids; **Mission**: terrain
    ids + atlases; **External**: WaterReflection×2. Validator now FAILS on `Unset`; the dump emits lifetime.
    `kExternalResources` kept as a consistency-doctest (the External *concept* is broader than
    lifetime==External — sound, drift-guard added; full migration = follow-up).

## ★★ TIER-B PRACTICAL COMPLETE — MILESTONE (2026-06-30, HEAD `40e0f5af`)
**Tier-B practical same-order frame graph COMPLETE: the graph validates, applies, enforces, and names/
lifetimes all ownable current-order render work.**
★Scope discipline (what tier-B is NOT): **NOT scheduler, NOT backend, NOT Vulkan-ready, NOT renderer-
fully-optimized.** Tier-B = same-order ownership is boring + enforced. No pass moves; no reorder; no GPU
barrier planning; no transient aliasing. That is tier-C/D, deliberately not started.

**ALL-GREEN TIER-B GATE (the evidence):**
- **OFF** (`MC2_FRAMEGRAPH_EXECUTOR=0`): all executor metrics = **0**, byte-identical rendering.
- **ON** (`=1`): `validation_failures=0` across every owned pass.
- **DRYRUN**: `out_of_order=0`, `ambient_probe_mismatches=0`, `fbo_mismatches=0`.
- **Registry validator**: **0 missing lifetimes**, 14/14 live registrations carry a lifetime class.
- **RAW-GL capstone**: green (all 5 axis gates present+wired+non-stub; KNOWN_DEFERRED ledger explicit).
- **Byte-identical** OFF; **97 doctests** pass.

Concretely tier-B = the union of: VALIDATE 10/10 top-level passes; the ownable APPLY-STATE set (PostProcess
6/6 + StaticProp/Mech/Water runtime-proven sole-setter + Shadow full render-target-mode owner; decal/overlay
code-correct + content-unexercisable); ENFORCE = 5 raw-GL axis gates + capstone; NAME/LIFETIME = 24-id
registry, all live registrations lifetime-classed, validator fails on Unset.

**Remaining tier-B follow-ups (do NOT block the milestone):**
- `kExternalResources` FULL migration to the lifetime field (kept as a consistency-doctest for now).
- Decal/overlay **content-exercise** — ★deploy to `A:\Games\mc2-opengl\releases\0.5 testing\mc2-win64-v0.5.0`
  (roads/overlays are OFF in v0.4/0.4c, ON in v0.5) + `MC2_DYNAMIC_DECALS` in run_smoke.py's allowlist;
  executor-ON smoke on a road/overlay mission, read the per-pass apply counter.
- `kParticleEffectState` stale blend=Additive vs live alpha-blend (render_contract.cpp:315).
- Optional **viewport validator** (`GL_VIEWPORT` AmbientSample sampler).

## ★ TIER-C SCHEDULER RECON (SCHEDULER-LEGAL-REORDER-VALIDATOR-RECON-1 — ✓ NOW BUILT, see entries 59–60)
★Supersedes the old "~15-20% LOW" read below. **Validator readiness ~60-65%** — NOT because we can
reorder, but because the lifetime field + the ambient/terrain/dryrun tables already model most edges.
The legal-reorder **VALIDATOR** is buildable: GL-free, offline, **no reorderer**.
★★**REDLINE: tier-C is PROOF infrastructure, NOT a scheduler.** A "legal" verdict ≠ license to move a
pass. The reorderer + ANY pass movement + a runtime scheduler stay GATED — deferred soft-state edges
(tex-unit/FBO/clip-control the resource DAG can't see) mean even a correct legality verdict is not
sufficient to actually reorder. **60-65% ready to build the VALIDATOR, NOT 60-65% ready to reorder.**

**SLICE 1 — SCHEDULER-EDGE-CLASSIFY-1:** new GL-free `RenderCore/scheduler_legal_reorder.h`.
- `enum EdgeClass { HardResource, SoftState, LegacyLatch, KnownEarly, ContentConditional, ExternalNonEdge }`
- `classifyEdges()` + a **no-op baseline doctest** (the current `kFramePassOrder` proves legal — mirrors
  `validateShippedFrameGraph`).
- THREE hand-declared tables: `kContentConditionalEdges`, `kForbiddenReorderEdges`, `kDeferredSoftStateEdges`
  (the tex-unit / FBO / clip-control edges the resource DAG cannot see).
- ★Lifetime win: External/Mission/Persistent reads auto-EXCLUDED from in-frame edges (the `ExternalNonEdge`
  category makes that explicit in debug output); only **FrameLocal** producers create real in-frame edges.

**SLICE 2 — SCHEDULER-REORDER-ORACLE-1:** `isReorderLegal(permutation)` predicate.
- identity = legal; known-bad = illegal WITH the first-violated edge named.
- StaticProp↔Mech = "candidate legal **adjacent swap**" (NOT "approved reorder").
- the oracle distinguishes "resource-wise legal BUT blocked by a deferred soft-state edge".

## ✓ TIER-C LEGAL-REORDER VALIDATOR SHIPPED (entries 59–60) — 2026-06-30, HEAD `fd4cfd54`
★PROOF-ONLY: GL-free headers + doctests, **zero runtime callers, EXECUTION UNTOUCHED, byte-identical by
construction.** The reorderer + ANY pass movement + a runtime scheduler stay GATED.
59. **SCHEDULER-EDGE-CLASSIFY-1** (`4b4b8c9f`) — new GL-free `RenderCore/scheduler_legal_reorder.h`:
    `enum EdgeClass{HardResource,SoftState,LegacyLatch,KnownEarly,ContentConditional,ExternalNonEdge}` +
    `classifyEdges()` (producer-walk = last-in-frame FrameLocal writer; lifetime∈{Mission,Persistent,
    External} reads emitted as **ExternalNonEdge**, NOT real in-frame edges; KnownEarly LOD-chunk Gamecam
    suppression) + 3 hand-declared tables (`kContentConditionalEdges` / `kForbiddenReorderEdges` /
    `kDeferredSoftStateEdges` = the tex-unit/FBO/clip-control edges the resource DAG cannot see) +
    `isCurrentOrderLegal()` no-op baseline. ★CORRECTNESS: **last-writer semantics** (Vegetation produces
    VFX's MainDepth; UI produces PostProcess's MainColor). **103 doctests.**
60. **SCHEDULER-REORDER-ORACLE-1** (`fd4cfd54`) — 3-state `ReorderVerdict{Legal, ForbiddenEdgeViolated,
    BlockedByDeferredSoftState}` + `isReorderLegal(permutation, firstViolated)` + `legalAdjacentSwaps()`.
    ★RESULT: **exactly ONE legal adjacent swap = StaticPropOpaque↔MechOpaque**; all others forbidden or
    deferred-blocked. Known-bad rejected WITH the first-violated edge (StaticProp-before-Shadow → Shadow→
    StaticProp via ShadowDynamicMap; Terrain-after-PP → sceneHasTerrain latch). Deferred-soft-state pairs
    return BlockedByDeferredSoftState (no overclaim). ★Conservative wording enforced: "candidate legal
    adjacent swap (eligible for a future MEASURED reorder experiment)" — NEVER "approved"/"safe"/
    "scheduled". **109 doctests.** Byte-identical by construction (GL-free header + doctests, zero callers).

★★ **NEXT GATE (do NOT auto-build): a MEASURED reorder experiment** is the FIRST execution-changing step —
requires an **explicit decision** + a **parity/capture harness**; the reorderer stays GATED. A "legal"
verdict is NOT a license to move a pass: the deferred soft axes (tex-unit/FBO/clip-control) live outside
the resource DAG, so even a correct legality verdict is insufficient to actually reorder.

## ✓ CLEANUPS + TIER-C FIRST REORDER EXPERIMENT (entries 61–65) — 2026-06-30, HEAD `dea90c61`
61. **STATEPACK-TRUTH-REFRESH** (`e2422e1b`) — HYGIENE. Fixed 4 stale `pipelineDescRegistered` tests +
    stale comments (`render_state_desc.h` / `postprocess_subgraph.h`) to the current 8-true truth + stale
    `PipelineId::Count_` 5→25. ★**Full unit suite now 240 passed / 0 failed** — the long-standing "4
    known-unrelated" red is GONE.
62. **verify-executor-slice fix** (`b7fe89d3`) — `--with-dryrun` + `--assert-pass-fired` now assert against
    the ON dump (`fg_on`), not the post-dryrun OFF dump (was reading the wrong artifact).
63. **PARTICLE-CONTRACT-BLEND-FIX-1** (`2b55bbaa`) — `kParticleEffectState` declared blend Additive →
    **AlphaBlend** (matches live SRC_ALPHA/ONE_MINUS_SRC_ALPHA). Descriptive value, never asserted;
    byte-identical, smoke 5/5 +0.
64. ★**MEASURED-REORDER-SPMECH-1** (`2461d37e`) — **FIRST tier-C execution-changing experiment.** Gate
    `MC2_FRAMEGRAPH_REORDER_SPMECH` (default-OFF; added to run_smoke allowlist). Gate-OFF byte-identical
    (`out_of_order=0`); gate-ON swap took effect (dryrun `out_of_order=1953`). ★**RESULT: PARITY FAILS** —
    mc2_24 OFF↔ON pixel diff **0.10–0.32% changed, 3–6× above the established noise floor** (OFF-vs-OFF
    0.025–0.073%); cause = mech↔prop overlapping fragments resolve order-dependently under **depth-EQUAL
    ties**. Perf: noise-level (mechs GPU ~0 in mc2_24, no headroom). **VERDICT: resource-legal ≠
    visually-commutative; candidate REJECTED; gate stays default-OFF permanently (NOT adopted).** Capability
    proven end-to-end (classify→gate→capture→measure→reject). The gate code is RETAINED default-OFF as the
    capability artifact; the oracle (entry 65) is the authoritative guard against ever enabling it.
65. ★**ORACLE-STRENGTHEN-1** (`dea90c61`) — encoded the finding. Added the concrete **StaticProp↔Mech
    depth-tie edge** to `kDeferredSoftStateEdges` + documented the general overlapping-opaque-depth-tie
    principle + extended `crossesDeferredSoftState` to catch concrete-named pairs (region-scan alone MISSED
    it — both passes are before the Terrain boundary). Oracle now: **StaticProp↔Mech → BlockedByDeferred-
    SoftState (was Legal)**; `legalAdjacentSwaps()` → **0 (was 1)**; `isCurrentOrderLegal()` still true.
    **109 doctests.** The validator LEARNED from the execution experiment.

## ★★ TIER-C FIRST EXECUTION EXPERIMENT — candidate REJECTED, oracle strengthened (2026-06-30)
★**THE TIER-C LOOP CLOSED CORRECTLY.** Oracle predicted StaticProp↔Mech legal → gated experiment
(`MC2_FRAMEGRAPH_REORDER_SPMECH`) FALSIFIED it on PARITY → oracle updated to ZERO clean legal swaps.
- **The finding (depth-tie):** mc2_24 produced pixel diffs of **0.10–0.32%** between gate-OFF and gate-ON,
  **3–6× above the OFF-vs-OFF noise floor (0.025–0.073%)**. Root cause: StaticPropOpaque and MechOpaque
  fragments OVERLAP and resolve **order-dependently under depth-EQUAL tie-break** — swapping the two passes
  changes which fragment wins the tie. No hard resource edge exists between them, yet the result is NOT
  visually identical.
- **The lesson (now durable IN the oracle):** **"no hard resource edge" ≠ "visually commutative"** for
  overlapping opaque passes. A resource-DAG-legal verdict does NOT license a reorder when two passes share
  screen-space fragments at equal depth. This is the **mature tier-C posture: legal verdict ≠ adoption;
  parity proof beats armchair reasoning; default-OFF stays OFF.**
- **Disposition:** reorderer NOT shipped. Gate `MC2_FRAMEGRAPH_REORDER_SPMECH` retained **default-OFF
  permanently** as the capability artifact (proves classify→gate→capture→measure→reject works). The oracle
  (`legalAdjacentSwaps()` → 0) is the authoritative guard against ever enabling it.

## ★★ INDEX-RACE INCIDENT + LESSON (critical — read before any parallel committing)
Two COMMITTING agents in the SAME nifty worktree (FBO-gate lane + SHADOW-1 lane) raced on the shared git
INDEX: SHADOW-1's commit (`ecfe38e2`) SWEPT the gate's staged files into itself. Recovery: soft-reset,
rewrote SHADOW as `075014bc` (verified byte-identical minus the gate files; `ecfe38e2` is now
ORPHANED/unreachable — DO NOT branch off it), committed the gate separately as `8befeaa1`. (SHADOW-1's
final on-nifty SHA is `6ea1a42a`.)
★**LESSON — ONE COMMITTER PER WORKTREE.** Even DISJOINT file sets race on the shared index (a
`git add`/`commit` in one agent stages+sweeps whatever the other agent left staged). Parallel work MUST
go in a SEPARATE worktree. The registry slices PROVED the fix: spare `mc2-nifty-land` = separate index =
zero races. The spare-worktree parallel-build pattern (`docs/build-parallel-and-tooling.md`) is VALIDATED:
spare detached worktree + spare deploy dir + lease-serialized smoke + cherry-pick disjoint commits onto
nifty.

## ★ SCHEDULER / TIER-C RECON (SCHEDULER-LEGAL-REORDER-RECON-1, analysis-only — DO NOT build)
⚠ **SUPERSEDED by SCHEDULER-LEGAL-REORDER-VALIDATOR-RECON-1 (above, ~60-65% VALIDATOR-ready).** The
"15-20%" below was pre-lifetime-field; the registry + lifetime classes + ambient/terrain/dryrun tables now
model most edges. Kept for the reorder-DAG map (still accurate).
Tier-C (legal reorder/schedule) readiness ~**15-20% (LOW)** [STALE — see superseding recon]. Reorder DAG mapped:
- **Forbidden edges:** Shadow → all-geometry + PP (ShadowDynamicMap); opaque → blend/PP (MainColor/Depth/
  Normal); VFX → PP-BoxDecals (SceneDepthCopy); Terrain → overlay/decal + 5 PP sub-stages (sceneHasTerrain
  latch). **ONLY StaticProp↔Mech is truly reorder-safe.**
- **Dominant blocker:** the resource registry (was ~35% live; now 10/12 with terrain+material — keep
  filling it; that is the gating work for tier-C).
- **Barriers:** mostly tier-D (GL implicit ordering already covers same-FBO reorders).
- **First tier-C step (FUTURE):** a legal-reorder VALIDATOR (model-before-mutate, like the dry-run), NOT a
  reorderer. Refresh this recon before building.

## ▶ NEXT-STEPS (precise — tier-b finish order) — ✓ ALL DONE; tier-b practical COMPLETE (HEAD `40e0f5af`)
✓ **(a) RAW-GL-BYPASS-CAPSTONE-1** — DONE `567af186` (entry 54). Meta-gate enumerates all 5 axis gates +
asserts no un-gated escape + KNOWN_DEFERRED ledger. Enforcement is now a single regression-proof capstone.
✓ **(b) Remaining RESOURCE-REGISTRY slices** — DONE: REGISTRY-POSTPROCESS-FBO-1 `9e7ed61e` (entry 55),
REGISTRY-COMPUTE-IDS-1 `0d7aa9fe` (entry 56), REGISTRY-SCENECOLORCOPY-PRODUCER-1 `7e407910` (entry 57),
REGISTRY-LIFETIME-CLASS-1 `40e0f5af` (entry 58, capstone). Registry = 24 ids, all live registrations
lifetime-classed. ✓ **(c) tier-b practical 100%** — REACHED (see the TIER-B-COMPLETE milestone block).
✓ **(d) tier-C legal-reorder VALIDATOR** — DONE `4b4b8c9f`/`fd4cfd54` (entries 59–60). ✓ **(e) FIRST
measured reorder experiment** — DONE `2461d37e` (entry 64): StaticProp↔Mech → **PARITY-FAILED on depth-tie,
candidate REJECTED, gate default-OFF permanently** → oracle strengthened to 0 legal swaps `dea90c61`
(entry 65). ✓ Cleanups DONE (entries 61–63): suite 240/0, verify-slice fix, particle blend.
★★**NEXT = GPU RESOURCE OWNERSHIP / RAW-GLOBAL MINIMIZATION (GPU-BUFFER-OWNER-RECON-1)** — see the
NORTH-STAR block below; this SUPERSEDES further reorder experiments as the primary thrust.

## ★★ NEXT-SESSION NORTH-STAR — GPU-BUFFER-OWNER-RECON-1 (raw-global minimization)
> **STATUS 2026-07-05: cheap entries (1) view-UBO and (2) LOD-chunk type/cement SSBOs are DONE**
> (TERRAIN-VIEW-UBO-OWNER-1 / TERRAIN-LODCHUNK-SSBO-OWNER-1; 14 owner sites, check-gpu-buffer-owners.py
> exit 0, dead-indirect inversion closed). Identity-registration saturated. NEXT REAL SLICE =
> **TIER0-LODCHUNK RAII adoption** (thin owning GpuBuffer .cpp for the 9 lod-chunk statics — the
> "LATER slice" flagged in GpuBufferOwner.h:15), then TIER0-HUD (needs OD-4 decision).
**Goal:** not enumerate/register, but **MINIMIZE the ~70% of GPU resources that are raw file-static `GLuint`
globals** by bringing them under registry/owner ownership: `s_heightSsbo`, `g_recipeSSBO`,
`s_mechMaterialSsbo`, ring buffers, batcher instance SSBOs, view UBO, etc.
- **Cheap entry (1) — view-uniforms UBO:** a single clean site.
- **Cheap entry (2) — LOD-chunk type/cement SSBOs:** the SOLE **live** terrain renderer's own buffers are
  conspicuously UNregistered, while the DEAD indirect-path buffers ARE registered — close that
  inconsistency FIRST.
This maps to the real remaining modernization gap far better than more reorder experiments.

**(historical original plan — kept for trail):**
**(a) RAW-GL-BYPASS-CAPSTONE-1** — meta-gate that ENUMERATES all 5 axis gates (depthFunc/depthMask/
colormask/blendFunc/fbobind) + asserts there is no un-gated escape path. Comes AFTER the FBO gate (now
shipped). This is where Phase-8 enforcement becomes a single regression-proof capstone.

**(b) Remaining RESOURCE-REGISTRY slices** — ⚠ branch off the NEW nifty HEAD **`f60b7ea2`** (NOT the
orphaned `ecfe38e2`). The fully-file-disjoint ones (terrain, material) are ALREADY DONE and were the only
spare-parallelizable ones; the rest SERIALIZE on nifty (they share gos_postprocess.cpp / RenderPassContract.h):
  - **REGISTRY-MAINCOLOR-1** + **REGISTRY-GBUFFER-1** — register MainColor / MainNormal / SceneObjectId /
    SsaoOcclusion / SceneDepthCopy / HzbPyramid at `gos_postprocess.cpp` createFBOs. ⚠ shares
    gos_postprocess.cpp → run ON NIFTY serial, NOT spare.
  - **REGISTRY-SCENECOLORCOPY-PRODUCER-1** — touches RenderPassContract.h → serialize.
  - **REGISTRY-COMPUTE-IDS-1** — add 3 enum ids for the cluster / lightgrid / blur compute intermediates.
  - **REGISTRY-LIFETIME-CLASS-1** — add a FrameLocal/Mission/Persistent/External field — CAPSTONE,
    touches everything.

**(c)** = tier-b practical 100% ("boring") → **THEN tier-C legal-reorder VALIDATOR** (recon refresh first;
see the SCHEDULER recon above).

**Parked items (current):**
- ✓ `kParticleEffectState` stale blend — DONE `2b55bbaa` (entry 63).
- Decal/overlay content-exercise (PARKED): ★deploy to `A:\Games\mc2-opengl\releases\0.5 testing\mc2-win64-v0.5.0`
  (roads/overlays OFF in v0.4/0.4c, ON in v0.5) + **MC2_DYNAMIC_DECALS** already added to run_smoke.py's
  allowlist, then executor-ON smoke on a road/overlay mission and read the per-pass apply counter.
- `kExternalResources` FULL migration to the lifetime field — DEFERRED by design (kept as a
  consistency-doctest; the External *concept* is broader than the field).
- (optional) viewport validator: an `AmbientSample` `GL_VIEWPORT` sampler.

**Automation now in place (use it):** `verify_executor_slice.py --assert-pass-fired NAME[:MIN]` + the
fixed `stale_deploy_check`; `scripts/check-apply-pass-bumped.py` (apply-pass-bump completeness gate);
registration-completeness doctests; `get_executor_health` per-pass surface.

## ▶ (SUPERSEDED) PHASE BOUNDARY: FRAMEGRAPH-APPLY-STATE-EXTEND — ✓ DONE (entries 49–52 above)
Spec retained for reference. Shovel-ready spec to lift passes whose state is MORE than a single
applyPipeline (Shadow = pipeline + a depth clear + a distinct viewport/FBO).

**Descriptor** (top_level_pass_executor.h): add `enum ClearSpec { None, DepthForwardZ }` + field
`ClearSpec clear = None` to `TopLevelStateDesc`. **OMIT depthFunc/depthWrite** (the ambient ledger already
owns them — don't double-own). `ViewportKind::ShadowMap` + `RenderResourceId::ShadowDynamicMap` +
`PipelineId::ShadowMech` ALL already exist.

**Apply** — HYBRID: a shared `applyTopLevelGenericAxes(desc, fbo, w, h)` helper (FBO → viewport → pipeline
→ clear; each step skips on its sentinel) PLUS per-pass resolver fns (NOT fully generic — the FBO logical→
GLuint resolution is pass-specific; the glName is debug-only). Shadow's resolver fn resolves via
`getDynamicShadowFBO()` / `getDynamicShadowMapSize()`.

**Backward-compat:** the appended `clear` field defaults to the skip-sentinel → the 5 existing pipeline-only
consumers stay byte-identical.

**Validation:** FBO (fbo_ledger) + depth (ambient ledger) already covered. Viewport = an OPTIONAL new
`AmbientSample` field + a `GL_VIEWPORT` sampler. Clear = via the shadow-state trace (no live sampler).

**Sequence (one slice at a time):**
- **SLICE A — EXTEND-1:** add `ClearSpec` + the shared `applyTopLevelGenericAxes` helper. PROVE by
  re-expressing StaticPropOpaque with explicit MainColor/MainScene targets = byte-identical to the current
  inheritance, runtime-exercised in tier1.
- **SLICE B — APPLY-STATE-SHADOW-1:** Shadow lifts pipeline + clear ONLY. Dynamic seam =
  `beginDynamicShadowPass` (gameos_graphics.cpp:6445); lift :6467/6468/6474/6484-6486. KEEP in the body:
  :6452 (capture), :6458-60 (AMD-unbind), :6463-65 (compare-flip), :6480 (note), :6488-94 (material + lsm).
- **SLICE C (optional) — SHADOW-2:** lift the FBO + viewport too, AFTER the AMD-unbind ordering is proven.

## ▶ OPEN ITEMS — overlay/decal content-exercise + automation now in place
- **Overlay/decal full content-exercise (NOT a blocker):** ★CORRECTION (2026-06-30, user) — the prior
  read "stock maps have no overlay content" was WRONG ABOUT THE CAUSE. Roads/overlays are **OFF in the
  v0.4/0.4c builds we smoke to** and **DRAW in the v0.5 install**. So to light up
  `executor_apply_state_by_pass.TerrainOverlay` / `.TerrainDecal`: **DEPLOY TO**
  `A:\Games\mc2-opengl\releases\0.5 testing\mc2-win64-v0.5.0` (note the SPACE in "0.5 testing"), NOT
  v0.4/0.4c, then run an executor-ON smoke on a road/overlay-bearing mission and read the per-pass apply
  counter. Also add **`MC2_DYNAMIC_DECALS`** to `run_smoke.py`'s mission-Popen env allowlist (currently
  dropped → dynamic-decal pass never runs in smoke). The shared apply MECHANISM is already runtime-proven
  by StaticPropOpaque, so this is content coverage, not correctness.
- **Automation now in place (use it):** `verify_executor_slice.py --assert-pass-fired NAME[:MIN]`
  (per-pass apply-counter assertion, no manual dump-reading); `scripts/check-apply-pass-bumped.py`
  (ApplyPassId bump-exactly-once grep-gate, in check-contracts.sh); apply-state registration doctests
  (FrameGraph suite, 91). Apply-state wiring is now self-checking end-to-end.

## ▶ NEXT PHASE — top-level APPLY-STATE expansion + Phase 8 RAW-GL-BYPASS gate
Same-order top-level VALIDATE is done (10/10). Frontier moves to top-level **APPLY-STATE** (executor
becomes the sole state-setter for a pass, body-skips its `applyPipeline`):
- **IN FLIGHT (do not duplicate): APPLY-STATE-TERRAINDECAL-1** — first top-level scene-pass apply-state.
  Builds top-level apply infra (`kTopLevelStateDesc` / `findTopLevelStateDesc` +
  `executorApplyTerrainDecalState` + `decalStateAppliedByExecutor_`); decal body skips applyPipeline.
- **NEXT after decal: APPLY-STATE-TERRAINOVERLAY-1** — same infra, slice 2.
- **EXCLUDED from apply-state**: Terrain / Mech / Shadow — pin-sensitive / PipelineId::Invalid per recon.
- **IN FLIGHT (do not duplicate): RAW-GL-DEPTHFUNC-DIFF-GATE-1** — Phase 8 first enforcement gate;
  diff-based static check banning new raw `glDepthFunc`.

## ⚠ (RESOLVED above) prior RED note — SAME-ORDER-SLICE-2 was committed-but-RED

SAME-ORDER-EXECUTOR-SLICE-2 (`8fd69a92`/`7a93be9e`/`1d34b845`) is COMMITTED but RED: executor OWNERSHIP
of Shadow+MechOpaque is VERIFIED-GOOD (validated_top_level=9584, validation_failures=0, game renders
+0 destroys, ambient/fbo mismatch 0), BUT the dry-run order MODEL is wrong: **out_of_order≈1248/frame,
firstOOO=MechOpaque**. (Dryrun is default-OFF, so rendering is fine — this is a model honesty bug.)

ROOT CAUSE (MECHOPAQUE-ORDER-FIX-1 agent diagnosis — it correctly REVERTED, did not commit a non-fix):
**OpaqueObject has TWO note sites.** (1) `mclib/tgl.cpp:3016` noteRenderPass(OpaqueObject,"TG_Shape_Render
(enqueue)") fires at ENQUEUE time inside the Render.3DObjects loop (txmmgr:2481), BEFORE the static-prop
flush (:3250/:5398) — semantically WRONG (enqueue ≠ draw), pollutes the trace. (2) `mclib/txmmgr.cpp:3271`
noteRenderPass(OpaqueObject,"GpuMechBatcher_flush(submit)") is the correct DRAW-time site (after static
flush). With TGL objects present, site (1) fires Mech early → Mech records before StaticProp → OOO. The
kFramePassOrder swap ALONE leaves OOO=1248 because site (1) still fires early.

**COMPLETE FIX = MECHOPAQUE-ORDER-FIX-2** (the next slice):
1. REMOVE (or suppress) the `tgl.cpp:3016` enqueue-time noteRenderPass(OpaqueObject) — it's an enqueue
   marker, not a draw; the txmmgr:3271 draw-time note is the authoritative one. (Verify nothing else
   relies on the tgl.cpp note; it's observe-only.)
2. SWAP kFramePassOrder to [Shadow, **StaticPropOpaque, MechOpaque**, Terrain, ...] (Static flush :3250
   precedes Mech GPU draw :3271). DAG-safe (both read ShadowDynamicMap from Shadow; independent writes).
3. UPDATE tests (b)(c)(f) in test_frame_graph.cpp that encode the old Mech-before-Static order.
VERIFY: dryrun out_of_order MUST be 0 (use verify_executor_slice.py --with-dryrun or --parse-only on the
actual dump), ambient/fbo mismatch 0, validation_failures 0, validated_top_level≥9584, smoke 2/2 OFF&ON.
★The "two note sites" was the recons' miss — both SHADOW-OBSERVE-3 + MECHOPAQUE-PASSIDENTITY recons
focused on txmmgr:2362/3271 and missed the tgl.cpp:3016 enqueue note. (verify_citations would NOT have
caught this — it's a missing-site, not a stale-citation. Lesson: grep ALL noteRenderPass(<id>) sites
before relocating one.)

## ★ META-TOOLING shipped this pause (cuts future churn; MCP picks up on console reboot):
- `1b8f8179` run_smoke.py `--require-gate` (fails if a gate's dump-counter is 0) + ENV-DROP warning
  (mission Popen uses a CLEAN env=env_extra allowlist — non-allowlisted MC2_* ARE dropped, confirmed;
  the warning is valid) + `scripts/verify_executor_slice.py` (canonical OFF/ON/dryrun gauntlet +
  stale-deploy src_commit-vs-HEAD guard; `--parse-only <dump>` reads metrics w/o a smoke).
- `17500527`/`48e76260` MCP `get_executor_health` (render-state: executor split metrics + dryrun
  out_of_order + CLEAN/FAIL verdict in one call). NOTE: my doc-based verify_citations was REDUNDANT
  (claims-based already on the docs branch `57bc95df`) — removed; only get_executor_health kept.
- `5eeb9291` docs/build-parallel-and-tooling.md — deploy-dir map (v0.4=primary; v0.4c/0.4c/v0.4d-rc1=
  spare smoke targets; v0.3=do-not-touch) + worktree-per-build64 isolation enabling PARALLEL build+smoke
  (spare worktree + spare deploy dir) + standing verification rules. Pointer in critical_inline_rules.md.

## ★ NORTH-STAR ROADMAP to "100% frame graph" (advisor, 2026-06-29)
Tiers of "100%": (a) 100% CONTRACT [close — every pass/subpass + resource read/write + ambient axis
declared or explicit-Inherit; runtime probes 0-divergence; CI catches drift], (b) 100% SAME-ORDER
EXECUTOR [NEXT BIG NORTH-STAR — graph owns every pass in current order, applies/validates state+
resources, no pass runs outside graph scope, NO reorder], (c) 100% SCHEDULED [much later — legal
reorder/merge/alias], (d) backend-ready [Vulkan/D3D — barriers/lifetimes/contracts].
Current maturity (advisor): contract ~95%, dry-run ~85-90%, owned-islands ~60-70%, full executor ~45-55%.
The gap is NOT "do we know the frame exists" (we do) — it's "can the graph apply/bind/own everything
the old renderer mutates implicitly."

Phase sequence (DONE marked; this session jumped ahead of the advisor's read):
1. ✓ Finish first executor islands — ISLAND-2 EdgeFog/FogOob DONE (#22).
2. ✓ Retire dead terrain paths — MLR(#23)/Patch(#26)/Indirect(#27) DONE; deadcode delete #28; bridge
   bodies in progress. North-star: Terrain = LOD-chunk authority; legacy = gone/diagnostic-only.
3. ✓ Shadow observability — SHADOW-OBSERVE-2 DONE (#24); SHADOW-STATIC-BUILD-MODEL refines it.
4. **EXECUTOR-ISLAND-3/4** (NOW) then **POSTPROCESS-SUBGRAPH-1** — decompose monolithic passes into
   NAMED subpasses (endScene→{EdgeFog,FogOob,Godrays,Composite,HUD-handoff}; Terrain→{LODChunkSolid,
   Overlay,Decal}; Objects→{StaticProps,Mechs,Vehicles}). The graph can't own what's hidden in giant
   funcs. Direction: monolithic pass → named subpass → declared resources/state → executor wrapper → owned.
5. **FRAMEGRAPH-STATEPACK-1** — GL state → declared per-pass RenderStateDesc packs (FBO/viewport/
   scissor/depth/colorMask/BLEND-SEMANTIC/stencil/cull/program/VAO/tex-bindings). Blend must be
   SEMANTIC not ON/OFF: {OpaqueEffective, AlphaBlend, Additive, Decal, UI, Inherit}. Unsolved packs:
   blend semantics, texture units/samplers, VAO/input, scissor/stencil, viewport edge cases.
6. **RESOURCE-REGISTRY-COMPLETE-1** — every GPU resource gets a logical identity (intermediates, tex
   arrays, atlases) → graph answers who-writes/reads, current/prev/persistent, view-stamped, pass-stamped.
   Texture-latch work returns HERE (when executor owns more tex-sensitive islands / starts reordering).
7. **FRAME-GRAPH-SAME-ORDER-EXECUTOR-1** — the practical 80/20 "100%": graph walks existing order,
   begins each pass, applies declared state/resources where safe, calls existing body, validates. NO
   reorder. Success: all observable passes executor-owned, same order, OFF byte-identical, ON green,
   validation_failures=0, 0 resource/state mismatch.
8. **RAW-GL-BYPASS-CHECKS-1** — ban unsanctioned mutation: no raw glBindFramebuffer/colorMask/depthFunc/
   texture-bind/dispatch-MVP-read outside FrameGraphApply*() / declared paths (named legacy exceptions).
   This is where "100%" becomes ENFORCEABLE + regression-proof.
9. **SCHEDULER-RECON-1** — ONLY after same-order is boring. Topo scheduling / pass merge / transient
   aliasing / barrier planning / Vulkan backend. Late-stage; do NOT jump here from "we have a table".

## SECONDARY — legacy terrain retirement (AFTER frame graph)
North-star: ONE terrain renderer = LOD-chunk. MLR + indirect + patch-stream are deprecation targets.
Telemetry (TERRAIN-PATH-TELEMETRY-1) already shows them at 0 in default + capture. Arc when ready:
PATH-CONTRACT (per-branch contract) → LODCHUNK-AUTHORITY (gate legacy behind env + warn) →
LEGACY-RETIRE (prove 0 across real playtest/capture/editor) → DELETE. **Deletion gated on the
runtime counters reading 0, not on grep.** Do this AFTER the frame-graph executor work.

## Open from the original shipped-build bug report
- **BUG4 magenta** (Bandit HQ) — separate root (`1d78f204` colorkey, partial). Needs
  `MC2_OVERLAY_MAGENTA_TRACE=1` at Bandit HQ to name the tile. Not started.
