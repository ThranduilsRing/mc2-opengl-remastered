# Merge gate tiers (2026-11-06)

The gate is static tiers, not diff scoping. A stage outside the requested tier is absent from the plan (not SKIP), so the verdict stays PASS and `lane_merge --promote` accepts it. Only an operator `--stages`/`--skip` filter makes a gate PARTIAL.

| Command | Contents |
|---|---|
| `merge_gate.py --full` (every merge, target <= 15 min) | build (done at stage), harnesses/unit, contracts (rows that ever failed, plus the fast ones), deploy, ONE tier1 smoke batch (mc2_01/03/10/17/24) with `MC2_VULKAN_VALIDATION=1`, capture before/after plus compare |
| `--nightly` | per-merge set + all contract rows (`check-contracts.sh --nightly`), determinism (all maps), saveload (all maps), player-path |
| `--weekly` | nightly set + mods + video-decode |

Removed: the separate syncval/validation stages (validation rides the tier1 smoke), kit-visibility, quiet, box-gate and the box route, diff scoping (`gate_scope.py`, last-full-gate mark), the cheap-rows preflight, the tightened-baseline follow-up commit (ratchets only CHECK now; derived inventories are still regenerated), `dup_check` in the gate (it stays in `lane_merge --lint`).

Contracts: rows that never failed in the archived gate logs and take > 3 s are declared with `run_nightly` in `scripts/check-contracts.sh`; the rest run every merge.

GPU profiling (RGP) is a weekly orchestrator checkpoint, never a per-change proof. Video-decode and mods run weekly only.
