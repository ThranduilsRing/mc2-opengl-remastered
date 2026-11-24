# Merge gate tiers (2026-11-17)

The gate is static tiers (`stage_tier` / `tier_includes` in `scripts/merge_gate.py`), plus a few diff promotions. A stage outside the requested tier is absent from the plan (not SKIP), so the verdict stays PASS and `lane_merge --promote` accepts it. Only an operator `--stages`/`--skip` filter makes a gate PARTIAL.

| Command | Contents |
|---|---|
| `merge_gate.py --full` (every merge, 20 min advisory budget) | editor (path-scoped), harnesses/unit, contracts (path-triggered rows, cache on), full-verify deploy, tier1 on mc2_01 (day) + mc2_10 (night) with `MC2_VULKAN_VALIDATION=sync`; one committed build precedes the gate |
| `--nightly` | per-merge set + all contract rows (uncached), determinism (all nightly maps), saveload (mc2_01, mc2_10), player-path, captures before/after with blocking compare, and **mods-slim** (below) |
| `--weekly` | nightly set + the full `mods` battery (replaces mods-slim) + video-decode + tier1-full (all five tier1 maps) + quiet + saveload mc2_03/16/24 |

Diff promotions at a per-merge gate (`promoted_stages`): a sim-sensitive diff adds exactly headless determinism-mc2_24 (a pair, 2600 ticks) and saveload-mc2_01. A front-end/logistics/video path adds player-path, which decodes real comm video through the player flow. Mission names and mod/data paths never promote additional rows. Visual capture and compare run per merge only with `--declared-view`: capture-after plus advisory compare when an explicitly validated `--before-dir` or verified automatic capture reuse supplies the before set. Otherwise the gate reports after-only. It never shoots capture-before at tier 0; nightly comparison remains blocking.

Checks run by default only for touched logic: `scripts/contracts-cache-inputs.txt` is the contract trigger table; unscoped rows run nightly, and explicit `--rows` still runs an offline proof. The comparison base selects Python modules; full suites run nightly. Precheck receipts are informational. Precheck links only with `--with-link` and records whether sim rows are required without starting games. The merged tree gets one build and the gate's sim rows. Ratchet debt, a loosened log-tag census and a lowered gate population floor are merge review notes; generators re-record the current baselines and must successfully recheck them. Generator/tool execution failures remain errors. Lane-tip lint still checks the lane's own ratchet delta. Stale generated outputs are deferred only for known stale-only diagnostics, then regenerated and successfully rechecked at batch end.

`gate-verdict.json` records wall seconds, per-stage seconds, stage reasons and measured host lease wait separately. A tier-0 gate over 20 minutes excluding host lease wait prints advisory `[GATE_BUDGET] OVER` and its three longest stages; the orchestrator opens an infrastructure lane when it fires. Logs are UTF-8. It also carries `stage_table` (name, status, seconds, scheduler wait, longest first).

## Pare-down rules (owner 2026-11-23)

- Scoped pytest: an unmapped path no longer selects the full suite (`scripts/run_tools_tests.py` prints the unmapped list; an unmapped script or tool module named X selects tests whose name contains X). lane_merge verify runs the scoped Python tests in parallel with the build. The full suites run nightly; run them in the background when the last nightly is more than 24 h old.
- The gate runs runtime rows concurrently with the offline stages, and a runtime row is reused when the hash of the engine-domain input tree and the exe fingerprint are unchanged (unknown paths count as engine). An offline red fixed by a tests-only commit is re-proved by rerunning that one row, not the whole gate.
- The editor stage and the player-path row (except front-end, campaign, video and save diffs) are nightly. No merge-install smoke-lite: gate tier1 covers it. The doctest runs once, in the gate harness tree.
- v0.4 promotion: re-check owner state right before the deploy; `py -3 scripts/deploy_payload.py --target game --ninja --verdict <slot>/build64-ninja/gate-verdict.json`; then one smoke-lite with gate priority: `MC2_IN_GATE=1 py -3 scripts/run_smoke.py --lite --keep-logs --exe A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe`. No lane exclusive or Tracy capture until it ends.
- The gate install is the persistent `E:/mc2-installs/merge-gate`, reseeded weekly (private save dirs per window).
- Dirty README.md (owner edits) never blocks the clean-tree check.

## Publishing to v0.4 (`deploy_payload.py --target game`)

The publish refuses an exe that is not the one the newest gate PASSed for HEAD. By default it reads `<src>/build64-ninja/gate-verdict.json`; a PASS from a stage or lane tree is passed explicitly with `--verdict <path>/gate-verdict.json`. It is accepted only if `verdict` is PASS, its `head` equals the deploying tree's HEAD and `artifacts.exe_sha256` equals the sha256 of the exe being deployed; anything else refuses. The gate keys on the resolved target (the v0.4 preset dir with a game exe), so `--target game`, `--target game --allow-new-target` and the positional v0.4 path are all gated, and a v0.4 publish always implies `--require-clean`. The refusal says to deploy the exe the gate tested or pass `--verdict`; `--allow-unverified-exe` stays in `--help` but on v0.4 it logs a WARNING and writes `.deployed_manifest.unverified.json` beside the manifest. Install DLLs that are hardlinks into the shared pool are never written through: when the replace of such a file is refused (an open handle with delete sharing on this install's own path) the old entry is renamed aside to `<name>.old-<pid>-<n>` and a private copy takes its place. An aside that cannot be deleted is listed in `<install>/.deploy-asides.json`; every deploy into that install deletes the listed ones right after taking the folder lease and again before releasing it (so failed deploys are covered). Only paths matching `.old-<pid>[-<n>]` inside the install are deleted; other `*.old-*` files are never touched. If even the rename fails the deploy stops naming the file as hardlinked and in use by another process.

## Mods (owner 2026-11-08)

"We can add a SLIM mod check to the merge gates at night, but they are not critical. I'd rather let them break and do a dedicated fix."

- `mods-slim` (nightly only): `tools/mod_smoke.py --battery` over the six mods (MC2X-CVE-G, MC2X-REAP3, Exodus, MagicExpansion, MechCommanderOmnitech, MCO-Carver5), one mission per mod, 10 s, no front-end walk, `--grace-s 480` for the cold per-stage model cook, and `--ref-exe` set to the exe under test.
- It is **non-blocking**. The gate prints `mods-slim per-mod: <mod>=<result>, ...`, and the verdict's `executed["mods-slim"]` holds the status and a `mods` map. A red row reads `ADVISORY ...`. It never enters `failed`, never changes the verdict, and promotion (`lane_merge_stage.nightly_unmet`) does not require it. A timeout (run_smoke's walltime cap, or mod_smoke's own) is `INFRA(timeout)`, not a mod FAIL.
- A red mods-slim gets a dedicated fix lane. It does not block merges.
- The weekly full `mods` battery is unchanged and blocking.

## Schedule

The PC task `scripts/nightly_gate.task.xml` (daily 04:30) launches `py -3 tools/laptop_gate.py nightly`; Sundays select weekly.
The PC only provisions inputs and pulls results: the farm laptop builds published nifty and runs contracts (including nightly rows), deploy and the tier's game stages. The six mod-battery overlays are seeded once from read-only PC mod inputs; their data, assets and mission archives stay outside git on the laptop.
Stock `.pak`/`.fit` bytes are verified on the PC, copied once by hash into `C:/mc2gate/stock-missions`, and passed through `MC2_STOCK_MISSIONS_DIR` to every laptop gate row.
Verdicts land under `E:/mc2-archive-2026-10/proofs/merge-gate/nightly-<date>/` (`--isolated-verdict`); reruns with an existing verdict append a time suffix. No nightly payload is packed, and window evidence is untouched.
The detached job in `G:/mc2-tmp/jobs` and the laptop farm queue serialize runs; `<window> -- --nightly` also selects this dated nightly path. Only the orchestrator registers the task:

```
py -3 A:\Games\mc2-main\scripts\nightly_gate_task.py   # renders the template with the full py.exe path (the task's PATH has no py); prints the line below
schtasks /Create /TN "\mc2-nightly-gate" /XML %TEMP%\mc2-nightly-gate.task.xml /F
schtasks /Delete /TN "\mc2-smoke-overnight" /F
```

## Laptop merge gate: one job, detached

`py -3 tools/laptop_gate.py W` is ONE laptop job: it pushes, builds, runs the gate and adds the diff-triggered sim rows after PASS on the same deployed candidate. Sim-sensitive diffs require determinism-mc2_01/03/10/24 and saveload-mc2_03/10; a red sim fails the run. `--no-sim` skips these rows; `--sim` alone reruns them without push or build. The detached job (`tools/detached_job.py`, state `G:/mc2-tmp/jobs/laptop-gate-W.json`, log beside it) survives the launching session and re-attaches on relaunch; `py -3 tools/detached_job.py wait laptop-gate-W` resumes the wait. `scripts/gate_wait.py` uses the same mechanism.

## Conflict preview (merge windows)

`py -3 tools/lane_merge.py --batch A B C --dry-run --conflict-dir <dir>` previews every conflict before the real batch, so nobody hand-merges to see them. Each tip, in batch order, is merged virtually (`git merge-tree --write-tree`) against the running virtual head; nothing in any worktree, index or ref changes. For each conflicted path it writes `<dir>/<NN>-<tipname>/<path>` (the file with conflict markers) plus `summary.txt` (tip, sha, conflicted paths with hunk counts, which earlier tip touched the same file). The summary also lists "same file, both tips" pairs: a heuristic (same path edited by two tips, generated files excluded) that flags possible duplicate fixes such as W138f's two empty-diff refusals; it also lists unrelated edits. The run prints one `CONFLICT-PREVIEW` line and exits 0; the merger reads the folder. Tips already contained in the running head get no folder.

## Manual smoke (orchestrator / merge gate)

```bash
py -3 scripts/run_smoke.py --lite --keep-logs --deploy 0.4      # smoke-lite: mc2_10 night + mc2_01 day, 10 s each
MC2_DEBUG_STATE_DUMP=1 MC2_DIAG_TAGS=CONFIG,BUILD,DEVICE py -3 scripts/run_smoke.py --tier tier1 --keep-logs --deploy 0.4   # full tier1, before milestones
```

The merge gate's `tier1` covers mc2_01 and mc2_10 with synchronization validation. Evidence includes `[DEPLOY_FINGERPRINT] OK: exe sha=... matches worktree HEAD` and both missions passing. Don't set `MC2_DIAGNOSTIC_TRACE_FILE`: the runner owns it and refuses the run. GATE_QUIET applies only to the PC gate fallback.

Removed: the separate syncval/validation stages (validation rides the tier1 smoke), kit-visibility, box-gate and the box route, diff scoping (`gate_scope.py`, last-full-gate mark), the cheap-rows preflight, and `dup_check` in the gate (it stays in `lane_merge --lint`).

Contracts: `run_nightly` rows and unscoped checks run nightly; the remaining rows run only on their declared trigger paths.

## GPU profiling

RGP is a weekly orchestrator checkpoint, never a per-change proof. Lanes do not capture `.rgp` to prove a change. The orchestrator captures weekly at the worst-case poses (mc2_01 `cross_map_view`, mc2_03 `scout_*`, mc2_10/24 `wolfman`) and the default pose. Suite: `C:/Users/Joe/Downloads/RadeonDeveloperToolSuite-2026-02-02-1757` (see `tools/host_inventory.py`). Video-decode runs weekly only.
