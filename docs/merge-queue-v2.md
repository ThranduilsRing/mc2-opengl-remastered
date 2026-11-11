# Merge gate tiers (2026-11-06; updated 2026-11-08)

The gate is static tiers (`stage_tier` / `tier_includes` in `scripts/merge_gate.py`), plus a few diff promotions. A stage outside the requested tier is absent from the plan (not SKIP), so the verdict stays PASS and `lane_merge --promote` accepts it. Only an operator `--stages`/`--skip` filter makes a gate PARTIAL.

| Command | Contents |
|---|---|
| `merge_gate.py --full` (every merge, target <= 15 min) | build (done at stage), harnesses/unit, contracts (rows that ever failed, plus the fast ones), deploy, ONE tier1 smoke batch on mc2_01 (day) + mc2_10 (night) with `MC2_VULKAN_VALIDATION=sync`, capture before/after plus compare |
| `--nightly` | per-merge set + all contract rows (`check-contracts.sh --nightly`), determinism (all maps), saveload (mc2_01, mc2_10), player-path, and **mods-slim** (below) |
| `--weekly` | nightly set + the full `mods` battery (replaces mods-slim) + video-decode + tier1-full (all five tier1 maps) + quiet + saveload mc2_03/16/24 |

Diff promotions at a per-merge gate (`promoted_stages`): a sim-sensitive diff adds the determinism/saveload rows; a path naming a mission adds that map's rows; a front-end/video diff adds player-path; a mod-content diff (`data/`, `mods/`, `examples/mods/`, `tests/mods/`, `tools/mod_smoke*`, `tools/mod_*.py`, `tools/mc2x_import/`, `tools/mco_import/`) adds the full `mods` battery. Engine C++, shaders, the VFS and loaders never promote mods.

## Mods (owner 2026-11-08)

"We can add a SLIM mod check to the merge gates at night, but they are not critical. I'd rather let them break and do a dedicated fix."

- `mods-slim` (nightly only): `tools/mod_smoke.py --battery` over the six mods (MC2X-CVE-G, MC2X-REAP3, Exodus, MagicExpansion, MechCommanderOmnitech, MCO-Carver5), one mission per mod, 10 s, no front-end walk, `--grace-s 480` for the cold per-stage model cook, and `--ref-exe` set to the exe under test.
- It is **non-blocking**. The gate prints `mods-slim per-mod: <mod>=<result>, ...`, and the verdict's `executed["mods-slim"]` holds the status and a `mods` map. A red row reads `ADVISORY ...`. It never enters `failed`, never changes the verdict, and promotion (`lane_merge_stage.nightly_unmet`) does not require it. A timeout (run_smoke's walltime cap, or mod_smoke's own) is `INFRA(timeout)`, not a mod FAIL.
- A red mods-slim gets a dedicated fix lane. It does not block merges.
- The weekly full `mods` battery is unchanged and blocking.

## Schedule

`scripts/nightly_gate.py` runs the nightly (Sundays: weekly) gate against the published nifty in its own worktree `A:/Games/mc2-nightly` (`MC2_NIGHTLY_ROOT`; created on first run with `git worktree add --detach`, `git checkout --detach claude/nifty-mendeleev` and a build in its own `build64-ninja` each night), with `--isolated-verdict` into `E:/mc2-archive-2026-10/proofs/merge-gate/nightly-<date>/`. mc2-main's branch and dirty state do not matter; it refuses only when the nightly worktree has tracked changes. A host lease still held after 60 min at start drops the game rows (printed `NOT RUN`) and runs the CPU rows. The Windows task is defined in `scripts/nightly_gate.task.xml` (daily 04:30). Only the orchestrator registers it:

```
py -3 A:\Games\mc2-main\scripts\nightly_gate_task.py   # renders the template with the full py.exe path (the task's PATH has no py); prints the line below
schtasks /Create /TN "\mc2-nightly-gate" /XML %TEMP%\mc2-nightly-gate.task.xml /F
schtasks /Delete /TN "\mc2-smoke-overnight" /F
```

## Laptop merge gate: one job, detached

`py -3 tools/laptop_gate.py W` is ONE laptop job: it pushes, builds, runs the gate and then, on a gate PASS, the sim rows (determinism-mc2_01/03/10/24, saveload-mc2_03/10) on the same deployed candidate, so the payload and sim manifests are identical by construction; the sim result lands in summary.json as `sim` and a red sim fails the run. `--no-sim` skips the sim rows; `--sim` alone is a rerun-only form (no push or build). The run is a detached job (`tools/detached_job.py`, state `G:/mc2-tmp/jobs/laptop-gate-W.json`, log beside it): it survives the launching session, a relaunch re-attaches to the running job, and a resumed agent waits with `py -3 tools/detached_job.py wait laptop-gate-W` (`status` shows running, done rc=N or lost). `scripts/gate_wait.py` runs its command the same way (`--job NAME` to re-attach).

## Conflict preview (merge windows)

`py -3 tools/lane_merge.py --batch A B C --dry-run --conflict-dir <dir>` previews every conflict before the real batch, so nobody hand-merges to see them. Each tip, in batch order, is merged virtually (`git merge-tree --write-tree`) against the running virtual head; nothing in any worktree, index or ref changes. For each conflicted path it writes `<dir>/<NN>-<tipname>/<path>` (the file with conflict markers) plus `summary.txt` (tip, sha, conflicted paths with hunk counts, which earlier tip touched the same file). The summary also lists "same file, both tips" pairs: a heuristic (same path edited by two tips, generated files excluded) that flags possible duplicate fixes such as W138f's two empty-diff refusals; it also lists unrelated edits. The run prints one `CONFLICT-PREVIEW` line and exits 0; the merger reads the folder. Tips already contained in the running head get no folder.

## Manual smoke (orchestrator / merge gate)

```bash
py -3 scripts/run_smoke.py --lite --keep-logs --deploy 0.4      # smoke-lite: mc2_10 night + mc2_01 day, 10 s each
MC2_DEBUG_STATE_DUMP=1 MC2_DIAG_TAGS=CONFIG,BUILD,DEVICE py -3 scripts/run_smoke.py --tier tier1 --keep-logs --deploy 0.4   # full tier1, before milestones
```

`tier1` = mc2_01/03/10/17/24. Evidence is the `[DEPLOY_FINGERPRINT] OK: exe sha=... matches worktree HEAD` line plus 5/5 PASS. Don't set `MC2_DIAGNOSTIC_TRACE_FILE`: the runner owns it and refuses the run. GATE_QUIET applies only to the PC gate fallback.

Removed: the separate syncval/validation stages (validation rides the tier1 smoke), kit-visibility, box-gate and the box route, diff scoping (`gate_scope.py`, last-full-gate mark), the cheap-rows preflight, the tightened-baseline follow-up commit (ratchets only CHECK now; derived inventories are still regenerated), `dup_check` in the gate (it stays in `lane_merge --lint`).

Contracts: rows that never failed in the archived gate logs and take > 3 s are declared with `run_nightly` in `scripts/check-contracts.sh`; the rest run every merge.

## GPU profiling

RGP is a weekly orchestrator checkpoint, never a per-change proof. Lanes do not capture `.rgp` to prove a change. The orchestrator captures weekly at the worst-case poses (mc2_01 `cross_map_view`, mc2_03 `scout_*`, mc2_10/24 `wolfman`) and the default pose. Suite: `C:/Users/Joe/Downloads/RadeonDeveloperToolSuite-2026-02-02-1757` (see `tools/host_inventory.py`). Video-decode runs weekly only.
