# Executor Handoff — Indirect Terrain Draw (SOLID-only) Plan v2

**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
**Plan:** [docs/superpowers/plans/2026-04-30-indirect-terrain-draw-plan.md](../2026-04-30-indirect-terrain-draw-plan.md)
**Status:** plan v2 reviewed by advisor + post-advisor revision passes; user signed off; ready for implementation.

You are an executor session. The plan is ship-ready and was iterated through brainstorm → recon handoff → design → plan v1 → adversarial review (3 CRITICAL + multiple major findings stop-the-lined v1) → plan v2 → advisor review (4 stop-the-line + 2 cleanup) → plan v2-revised. The user has signed off. **Your job is to execute Stage 0 → Stage 4. Do not relitigate the architecture, do not re-design, do not skip stages.**

## Required reading (do this BEFORE writing any code)

Read in this order. Each is small enough to fit comfortably; the verification appendix at the end of the plan is the executor's quick-reference for any cited symbol.

1. **`CLAUDE.md` (this worktree's, NOT the root pointer).** Critical rules — build flags, deploy convention, shader version, AMD driver rules, debug instrumentation discipline, smoke gate.
2. **The plan v2** — read end to end at least once. Verification appendix entries V1-V17 cite every grep'd symbol with file:line.
3. **The recon handoff** — [docs/superpowers/plans/progress/2026-04-30-indirect-terrain-recon-handoff.md](2026-04-30-indirect-terrain-recon-handoff.md). Items 1-9 resolved.
4. **The design doc** — [docs/superpowers/specs/2026-04-30-indirect-terrain-draw-design.md](../../specs/2026-04-30-indirect-terrain-draw-design.md). The 9 GPU-direct gotchas constraint section is load-bearing.
5. **Memory files referenced in the plan's "Cross-references" section.** Read on demand: `memory/water_ssbo_pattern.md`, `memory/gpu_direct_renderer_bringup_checklist.md`, `memory/m2_thin_record_cpu_reduction_results.md`, `memory/quadlist_is_camera_windowed.md`, `memory/clip_w_sign_trap.md`, `memory/mc2_argb_packing.md`, `memory/mc2_texture_handle_is_live.md`, `memory/render_order_post_renderlists_hook.md`, `memory/sampler_state_inheritance_in_fast_paths.md`, `memory/gpu_direct_depth_state_inheritance.md`, `memory/debug_instrumentation_rule.md`, `memory/feedback_offload_scope_stock_only.md`, `memory/feedback_smoke_duration.md`, `memory/tracy_profiler.md`.
6. **AMD driver rules** — [docs/amd-driver-rules.md](../../../docs/amd-driver-rules.md). Lines 5-7 (attribute 0, fragDepth, color attachment) all apply.
7. **The adversarial-plan-review skill** — [.claude/skills/adversarial-plan-review.md](../../../../.claude/skills/adversarial-plan-review.md). You will run this against your own implementation at Stage 3 close.

## Required sub-skills (Claude Code)

- **`superpowers:subagent-driven-development`** (recommended) — fresh subagent per task; review between tasks. Best for this plan because each stage has independent commits and clear validation checkpoints.
- **OR `superpowers:executing-plans`** — execute tasks in the same session. Better for small single-stage runs.
- **`superpowers:test-driven-development`** discipline applies wherever testable surfaces exist (parity-check assertions, counter increments, lifecycle prints).
- **`superpowers:verification-before-completion`** — before claiming any stage's checkpoint passed, RUN the smoke command and read the output. Evidence before assertions.

## Critical project rules (load-bearing — read CLAUDE.md for the full list)

- **Build:** ALWAYS `cmake --build build --config RelWithDebInfo`. Release crashes with GL_INVALID_ENUM.
- **Deploy:** NEVER `cp -r`. ALWAYS `cp -f` per file + `diff -q`. `cp -r` silently fails on Windows/MSYS2. Use the `/mc2-deploy` skill.
- **Deploy target:** `A:/Games/mc2-opengl/mc2-win64-v0.2/`. Game runs from there, NOT from the worktree's `run/`.
- **Git:** all work is local; no pushes to upstream. Commit per stage.
- **Shader #version:** never in shader files. Pass `"#version 430\n"` as prefix to `makeProgram()`.
- **Uniform API:** `setFloat`/`setInt` BEFORE `apply()`, not after.
- **`GL_FALSE` for `terrainMVP`:** direct-uploaded row-major matrices use `GL_FALSE` (`setMat4Direct`). Material cache (`mvp`/`projection_`) uses `GL_TRUE` (`setMat4Std`). Mirror the renderWater bridge precedent at `gameos_graphics.cpp:1991-2018`.
- **Smoke gate:** `py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 20` (default duration 20 — `--duration 8 --fail-fast` for tight iteration loop; `--duration 60` for stable Stage 1 baseline only). Tier1 missions are mc2_01, mc2_03, mc2_10, mc2_17, mc2_24.

## What plan v2 changed from plan v1 (you don't need to know v1, but heads-up if you've seen it)

Plan v1 had Stage 1 = depth-fudge addition; this is **DROPPED** in v2 (deferred to its own slice per design constraint #9). Plan v2's Stage 1 is now SOLID/detail-overlay cost-split measurement. Plan v1 grew the recipe schema; this is **DROPPED** — v2 uses the existing 144 B / 9-vec4 `TerrainQuadRecipe` verbatim. Plan v1 used `glMultiDrawElementsIndirect`; this is **WRONG for this codebase** — v2 uses `glMultiDrawArraysIndirect` because the thin VS uses `gl_VertexID` exclusively. Plan v1 had same-frame fallback after gate-off; this is **UNSAFE** — v2 lifts pack/build into preflight so arming proves drawability, and post-arm failure is a hard process-level error.

## Execution sequence

### Stage 0 — Scaffolding, parity printer, counters

Tasks 0.1 → 0.7. Lands env-gates, banner extension (`_cbbuf` 384→512), parity-printer skeleton, three N1 counters as **public-function API** (`Counters_Add*` / `Counters_Get*`), four env-allowlist additions (`MC2_TERRAIN_INDIRECT`, `MC2_TERRAIN_INDIRECT_PARITY_CHECK`, `MC2_TERRAIN_INDIRECT_TRACE`, `MC2_TERRAIN_COST_SPLIT`).

**Validation:** tier1 5/5 PASS unset; banner shows both new fields `=0`; no `[TERRAIN_INDIRECT_*` output.

### Stage 1 — Cost-split baseline (env-gated, zero-overhead default)

Tasks 1.1 → 1.3. Adds RAII steady_clock helpers in `quad.cpp` that early-out to a single bool read when `MC2_TERRAIN_COST_SPLIT` is unset. Run the capture command WITH the env flag and write captured S, D, T into both `progress/2026-04-30-indirect-terrain-stage1-baseline.md` and the Stage 1 commit message. **This sets Gate B's recalibrated target** (post-armed S ≤ 0.20 × Stage-1-S).

**Validation:** tier1 5/5 PASS unset (zero overhead); tier1 5/5 PASS with `MC2_TERRAIN_COST_SPLIT=1` (~5.6 ms/frame measurement overhead acceptable for baseline run only).

### Stage 2 — Dense recipe SSBO + per-mission Reset/Build + parity

Tasks 2.1 → 2.9. Recipe schema unchanged. `BuildDenseRecipe()`/`ResetDenseRecipe()` namespace-scope free functions in `gos_terrain_indirect.cpp`, mirror `WaterStream::Build/Reset` pattern. Wire build at `terrain.cpp:597-599` (sibling of WaterStream) gated on `IsEnabled() || IsParityCheckEnabled()`. Wire reset at `terrain.cpp:659+` (`Terrain::destroy`) — per-mission teardown via `Mission::destroy → land->destroy()` per [V6].

**Invalidation strategy (advisor stop-the-line #2 — precise XOR whole-map per site):**
- `mapdata.cpp:149` (in `MapData::destroy`): add `InvalidateAllRecipes()` next to existing `invalidateTerrainFaceCache()`.
- `mapdata.cpp:191` (in `MapData::newInit`): add `InvalidateAllRecipes()` next to existing call.
- `mapdata.cpp:1293+` (in `setTerrain`): add `InvalidateRecipeForVertexNum(blockX + blockY * realVerticesMapSide)` BEFORE the existing `invalidateTerrainFaceCache()` call.
- **`invalidateTerrainFaceCache(void)` body itself: NO new dense-recipe call.** Doing so would defeat the per-entry story.

**Validation:** tier1 5/5 PASS triple (unset / `INDIRECT=1` / `PARITY_CHECK=1`); zero parity mismatches; `event=recipe_build`/`event=recipe_reset` 5:5 paired across the tier1 sequenced run; RSS flat across mission boundaries.

### Stage 3 — Indirect SOLID draw + legacy SOLID gate-off (PR1 close)

**These ship in the SAME PR.** Per advisor stop-the-line #1, the plan is structured so failure-prone work runs in `ComputePreflight()` BEFORE the gate-off decision. After preflight, `armed=true` is a contract.

Tasks 3.1 → 3.8:
- 3.1: `PackThinRecordsForFrame()` — called from preflight, NOT from `DrawIndirect`.
- 3.2: `BuildIndirectCommands()` — `DrawArraysIndirectCommand` (16 B, 4 GLuints), bound to `GL_DRAW_INDIRECT_BUFFER`, NO EBO needed.
- 3.3: Bridge function — all 9 gotchas, AMD attr-0 via runtime `glEnableVertexAttribArray(0)` (user signed off on runtime form), `glColorMask` save/restore added to renderWater's set.
- 3.4: `DrawIndirect()` is a thin executor of preflight results. Bridge `false` return = `event=hard_failure` + `ForceDisableArmingForProcess()`. **No same-frame `flush()` fallback.**
- 3.5: `txmmgr.cpp:1330` hook with two branches — armed runs `DrawIndirect()` (no fallback), un-armed runs `flush()` (legacy admit ran normally).
- 3.6: Surgical SOLID-only gate-off in `setupTextures()` via `BeginLegacySolidCluster()/EndLegacySolidCluster()`. Counter API is per-quad, public-function. `ComputePreflight()` called once at `terrain.cpp:1679` BEFORE the per-quad loop.
- 3.7: 4-gate ladder + N1 counter check + Gate D negative checks (`event=hard_failure`, `event=zero_commands`, missing `event=first_draw`).
- 3.8: Commit + open PR1.

**Validation:** all four gates A/B/C/D green; N1 counters confirm gate-off worked (`legacy_solid_setup_quads ≈ 0` armed; `indirect_solid_packed_quads > 0` armed; `legacy_detail_overlay_quads > 0` always); Gate D negative checks all pass.

### Stage 4 — Default-on flip + N4 cross-mission Gate D quintuple (PR2)

Tasks 4.1 → 4.5. Soak gate: ~2 weeks of operator-driven default-on usage after PR1 lands. Then flip `IsEnabled()` semantics, run the **5-config quintuple** (warm-boot default-on, cold-start default-on, warm-boot killswitch, cold-start killswitch, warm-boot parity), ship `memory/indirect_terrain_solid_endpoint.md`, update orchestrator Status Board, queue the post-soak retirement + future detail/overlay/mine consolidation slices.

## Open follow-up the executor handles in-flight (not blocking)

- **V4 — AMD attr-0 mitigation.** Plan v2 picks runtime `glEnableVertexAttribArray(0)`. User signed off. If Stage 3 bring-up shows missing draws on AMD, escalate to user before switching to shader-side `layout(location = 0) in vec4 _attr0_dummy;` form.

## Anti-patterns to avoid (these will earn a code-review rejection)

- **Don't add fields to `TerrainQuadRecipe`.** It stays 144 B / 9 vec4s. Period. (If you find yourself wanting to add `overlayHandle`/`terrainDetailHandle`/`classifierFlags`, you're sliding into the deferred multi-bucket consolidation slice — escalate.)
- **Don't add the depth-fudge to `gos_terrain_thin.vert`.** It's its own slice, deliberately deferred.
- **Don't add same-frame fallback to `TerrainPatchStream::flush()` after gate-off.** The gate-off skipped admits; flush has nothing to flush. Hard failure is the only honest outcome.
- **Don't switch to `glMultiDrawElementsIndirect` "for compatibility."** The thin VS uses `gl_VertexID`; DrawArrays is correct.
- **Don't call `InvalidateAllRecipes()` from `invalidateTerrainFaceCache(void)`.** Precise XOR whole-map per site, never both.
- **Don't promote intermediate work to Tracy zones inside hot loops.** Per-frame steady_clock accumulators are the right pattern (advisor caught this one in v1).
- **Don't increment counters per addTriangle call.** Counters are per-quad (per cluster), not per-triangle.
- **Don't ship Stage 3a without 3b in the same PR.** N2 makes this explicit. Missing the gate-off would let perf go up while tier1 still PASSes.

## Anti-skip checklist (ship-readiness, before each commit)

1. ✅ Build is `RelWithDebInfo`?
2. ✅ Deploy used `cp -f` per file + `diff -q`?
3. ✅ Smoke gate ran (tier1 5/5 + menu canary)?
4. ✅ For Stage 3 specifically: greps in artifacts confirm `event=first_draw ≥ 1`, `event=hard_failure = 0`, `event=zero_commands = 0`, N1 counters tell the right story per [V10]?
5. ✅ For Stage 4 specifically: all 5 quintuple configs PASSed (not just default-on)?
6. ✅ Commit message includes captured numbers (Tracy delta, parity counts, N1 counter sample), not placeholders?
7. ✅ Lifecycle prints survive (gated off by default, NOT deleted) per `memory/debug_instrumentation_rule.md`?

## Escalation triggers (stop and ask user)

- Recipe parity surfaces > 0 mismatches after the standard 3-class triage (recipe coverage / blank-vertex skip / derived-byte) doesn't resolve.
- Gate B target (≥80% on `Terrain::SetupSolidBranch`) is missed by more than 20 percentage points after Stage 3 lands cleanly.
- AMD attr-0 runtime mitigation produces missing draws.
- Cross-mission validation (Stage 2 Task 2.8 OR Stage 4 quintuple) fails — likely a per-mission state cleanup bug per M6/V6.
- An uncited symbol turns up missing in source (executor's own grep at impl time disagrees with the plan's V1-V17 verification entries).

## What success looks like

- PR1 lands with Stages 0+1+2+3 in one branch. Default-OFF. Tracy delta on `Terrain::SetupSolidBranch` ≥80% reduction when armed. Zero parity mismatches. tier1 5/5 PASS triple. N1 counters confirm SOLID gate-off active. Killswitch (`MC2_TERRAIN_INDIRECT=0`) restores M2 path.
- After ~2 weeks soak, PR2 flips default-on. tier1 5/5 PASS quintuple. Memory closeout written. Orchestrator Status Board updated with both the Shipped row AND the queued follow-up rows (post-soak retirement + detail/overlay/mine consolidation).
- The next session opening the orchestrator sees clear queued rows for the follow-up work — the SOLID slice is closed but the arc continues.

## First action

Read the plan v2 from start to finish (about 30 min). Then start Stage 0 Task 0.1 with `superpowers:subagent-driven-development` or `superpowers:executing-plans`.

Good luck.
