# Indirect Terrain Stage 1 — Cost-Split Baseline (2026-04-30)

**Plan:** [docs/superpowers/plans/2026-04-30-indirect-terrain-draw-plan.md](../2026-04-30-indirect-terrain-draw-plan.md)
**Stage commit (pending):** Stage 1 cost-split RAII timers + per-frame steady_clock accumulators + 600-frame summary extension.

## Capture method

- Build: `--config RelWithDebInfo` (commit on top of Stage 0 `9bfcddc`).
- Smoke: `MC2_TERRAIN_COST_SPLIT=1 py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 30 --keep-logs`.
- Numbers below are from the LAST `[TERRAIN_INDIRECT_PARITY v1] event=summary` line per mission's `mc2_*.log` artifact at `tests/smoke/artifacts/2026-04-30T23-10-15/`.
- Bracketing locations (exhaustive enumeration; see "Hot path discovery" below for why):
  - **SOLID** clusters: 3 sites in `addTerrainTriangles` (file-scope helper), 2 dead-code sites in `setupTextures` (only fire under `MC2_MODERN_TERRAIN_PATCHES=0`).
  - **Detail/overlay** clusters: 2 sites in `addTerrainTriangles`, 2 sites in `enqueueTerrainMineState`, 6 dead-code sites in `setupTextures`.

## Captured numbers

| Mission | S (solid_branch_ns_per_frame) | D (detail_overlay_branch_ns_per_frame) | S+D | S share |
|---------|-------------------------------|----------------------------------------|-----|---------|
| mc2_01  | 23,290 ns ≈ 0.023 ms          | 52,577 ns ≈ 0.053 ms                   | 0.076 ms | 30.7% |
| mc2_03  | 18,248 ns ≈ 0.018 ms          | 17,604 ns ≈ 0.018 ms                   | 0.036 ms | 50.9% |
| mc2_10  | 63,247 ns ≈ 0.063 ms          | 134,000 ns ≈ 0.134 ms                  | 0.197 ms | 32.1% |
| mc2_17  | 48,544 ns ≈ 0.049 ms          | 111,058 ns ≈ 0.111 ms                  | 0.160 ms | 30.4% |
| mc2_24  | 10,144 ns ≈ 0.010 ms          | 13,993 ns ≈ 0.014 ms                   | 0.024 ms | 42.0% |

T (parent `Terrain::geometry quadSetupTextures` Tracy zone) ≈ 3.01 ms median per recon handoff baseline; live Tracy capture not run at this stage.

**Implication:** T − S − D ≈ 3.0 ms of "other" cost — texture handle resolution, recipe-build via Shape C cache lookup or inline path, mineResult cell-walks, the projectZ overlay diagnostic, and the per-vertex water/clip arithmetic that runs unconditionally. This is roughly 95-99% of `quadSetupTextures` cost. The SOLID admit-call alone is ~0.5%-2% of the parent zone.

## Hot path discovery (plan v2 V8 correction)

Plan v2 verification appendix [V8] cited paired SOLID admit clusters at `quad.cpp:466-467, 539-540`. Those are correct line numbers, but those clusters are inside the legacy inline branch of `setupTextures` that **only fires under `MC2_MODERN_TERRAIN_PATCHES=0`**. Under Shape C default-on (commit `aee39cc` 2026-04-29), `setupTextures` dispatches via `addTerrainTriangles(recipe)` (file-scope helper) at the cached-recipe branch.

Stage 1 brackets both paths:
- **Hot path:** `addTerrainTriangles` (SOLID + detail) and `enqueueTerrainMineState` (mine + blown).
- **Cold path:** the inline manual-emit clusters at the line numbers cited in V8 — kept bracketed for measurement reliability under `MC2_MODERN_TERRAIN_PATCHES=0`. No-op under Shape C default-on.

Initial Stage 1 wiring bracketed only the cold path; the smoke produced `solid_branch_ns_per_frame=0 detail_overlay_branch_ns_per_frame=0 frames_observed=3600`, surfacing the discrepancy. Re-bracketing the hot path produced the table above.

This is the kind of grep-cited-but-runtime-dispatched finding the plan-time discipline (`CLAUDE.md` "Documentation Discipline") is built to catch at execution time when verification-at-write-time misses it.

## Recalibrated Gate B target (per plan v2 Stage 1 Task 1.2 Step 3)

Per-mission target: post-armed `solid_branch_ns_per_frame ≤ 0.20 × Stage-1-S` (≥80% reduction on the SOLID branch).

| Mission | Stage-1 S | Gate B target (≤ 0.20 × S) | Aspirational (S ≈ 0) |
|---------|-----------|----------------------------|----------------------|
| mc2_01  | 23 µs     | ≤ 4.6 µs                   | ≈ 0                  |
| mc2_03  | 18 µs     | ≤ 3.6 µs                   | ≈ 0                  |
| mc2_10  | 63 µs     | ≤ 12.6 µs                  | ≈ 0                  |
| mc2_17  | 49 µs     | ≤ 9.7 µs                   | ≈ 0                  |
| mc2_24  | 10 µs     | ≤ 2.0 µs                   | ≈ 0                  |

Parent-zone (`Terrain::geometry quadSetupTextures`) delta expected ≈ S/T × 80% — sub-1% on every tier1 mission. Tracy may not visibly resolve a delta this small; the primary signal is the cost-split summary itself.

## Surfaced finding — escalation to operator

**The CPU-win value of indirect-terrain SOLID-only PR1 is small in absolute terms.** The per-frame SOLID admit-call cost is 10-63 µs across tier1, of which Stage 3 can theoretically retire ~80-100%. At 60 fps that's ~0.6-3.8 ms/sec — invisible at the FPS level on this hardware.

The architectural value of PR1 (stepping stone for the deferred detail/overlay/mine consolidation slice, which moves more of the "other" cost off the per-frame path; SSBO-pattern proven a third time after renderWater + vertexProjectLoop) is unchanged.

Decision points for the operator:
1. **Continue to Stages 2+3 anyway.** Architectural value justifies the work; PR1 ships as the foundation for the bigger consolidation slice.
2. **Pause and re-scope.** Re-evaluate whether the consolidation-slice scope (detail/overlay/mine + SOLID together) should be the actual PR1 instead of the SOLID-only narrowing.
3. **Pivot.** Find a different "other"-cost target inside `quadSetupTextures` (texture handle resolution, recipe build, mineResult walks) that has a bigger Stage-1-style baseline.

This finding does NOT invalidate the plan — it sharpens what Stage 3 will achieve in absolute terms. Surfacing it before Stages 2-3 invest 2-3 days of work.
