<!-- doc-status: current | reviewed:2026-10-11 -->
# MC2 Telemetry Debugger / Oracle Cockpit Architecture

**Status:** Strategy / design doc (no code yet), 2026-06-10.
**Scope:** A tool-facing architecture that makes engine runtime health visible, queryable, and comparable across builds — consuming logs and artifacts from mc2.exe, smoke runs, editor playtest sessions, and (later) runtime-bridge sessions.
**Siblings:** `runtime-bridge-architecture.md` (process model + stdout protocol v0 that this doc's live lane rides on), `mod-packaging-deploy-architecture.md` (deploy lane), `mc2-modding-toolchain-architecture.md` (tool ownership). This doc owns the **diagnostics/observability lane**.

---

## 1. North star

> **Every diagnostic the engine emits is a structured, append-only fact about one run. The cockpit is a pure consumer: it indexes facts, joins them against versioned baselines and budgets, and renders verdicts a human (engineer *or* modder) can act on — without ever touching the engine's authority over what happened or run_smoke's authority over pass/fail.**

Load-bearing consequences:

1. **mc2.exe is the only producer of truth.** The engine already emits versioned bracketed tags (`[MECH_MATERIAL_GPU v1]` in `GameOS/gameos/gos_mech_batcher_geometry.cpp:421-513`, `[FASTPATH_DROP]` in `mclib/terrain.cpp:3728-3773`, `[OBJBATCHER v1]` in `GameOS/gameos/gos_static_prop_batcher_state.cpp:903`, `[GPU_CULL v1]` readback diagnostics, `[SPFLUSH_COST_SPLIT v1]`, `[SNAPSHOT_BRIDGE_COMPARE v1]` — see `docs/tier1_env_vars.md`). The cockpit never computes a counter the engine could emit; it only aggregates, diffs, and displays.
2. **run_smoke's verdict path is sacred.** `scripts/smoke_lib/gates.py::evaluate` (LogSummary → buckets → Verdict) and `scripts/smoke_lib/report.py` (report.md/report.json) remain the *sole* pass/fail authority for smoke gates. The cockpit reads the same artifacts; it never inserts itself between `run_smoke.py` and its exit code, and adding cockpit features must never change a smoke verdict.
3. **Files first, sockets later.** v1 is entirely file/log based: a run is a folder of artifacts (`tests/smoke/artifacts/<timestamp>/` today). The live lane (editor Playtest, bridge sessions) reuses the *same schema* over the stdout pipe defined in `runtime-bridge-architecture.md` §3 — one event grammar, two transports.
4. **Headless-useful by construction.** Every cockpit conclusion must be derivable from artifacts on disk by a script or an agent with no GUI — the panels are a *view*, never the only access path. (The existing `mc2-render-state` MCP server, `scripts/mcp/mc2_render_state_server.py`, is the proof-of-pattern: JSON on disk, tools as readers.)

---

## 2. Telemetry taxonomy

Five concept types, with crisp definitions. Everything the engine emits maps to exactly one.

| Concept | Definition | Existing examples |
|---|---|---|
| **Event** | A timestamped fact that something *happened once*. No expected value; interesting for sequencing and attribution. | `[TIMING v1] event=mission_ready` (bridge protocol), `[FASTPATH_DROP] frame=N transition=... reason=...` (mclib/terrain_geometry.cpp:34), `[OBJBATCHER v1] event=legacy_toggle_blocked` (gameosmain.cpp:389), crash-handler banner |
| **Counter** | A monotonic or per-interval numeric emitted periodically or at summary time. Interesting as a *value*, compared against bands/baselines. | `[FX_COUNT v1]` per-site draw counts, `[OBJBATCHER v1] event=summary frames=...`, FPS samples parsed by `smoke_lib/logparse.py`, `destroys` total |
| **Oracle** | A *self-checking* counter: the engine computes both the fast path and an independent reference and emits the **mismatch count**. Expected value is axiomatically 0. | `[MECH_MATERIAL_GPU v1] mismatches=0`, `[MATERIAL_GPU v4] mismatches=0`, `[TEX_RESOLVE v1] mismatches=0 oob=0`, `[RENDER_SNAPSHOT v3] count_mismatch=0 ...`, `[TERRAIN_INDIRECT_PARITY v1] total_mismatches=0`, `[SNAPSHOT_BRIDGE_COMPARE v1] immutableMismatch=0`, TerrainLOD 8a/8b FN counters, VFX oracle (`docs/vfx-oracle-coverage.md`) |
| **Budget** | An externally declared bound on a counter (perf or resource), versioned outside the engine. The engine doesn't know budgets exist. | `tests/smoke/baselines.json` destroys mean (keyed `<profile>@<stem>@<tier>@<duration>`, `smoke_lib/baselines.py`), avg_fps tracking band in `docs/oracle-dynamic-pipeline-gate.md`, future per-pass GPU-ms budgets from Baseline A |
| **Warning / Failure** | A *judgment* — the result of evaluating events/counters/oracles against gates and budgets. Failures are produced only by gate code (`smoke_lib/gates.py` buckets: `gl_error`, `pool_null`, `asset_oob`, `crash_silent`, `timeout`, ...); warnings are advisory annotations the cockpit may add. | smoke fail buckets; future cockpit advisories ("p1% fps 12% below baseline") |

Rule of thumb: **engine emits events/counters/oracles; repo files declare budgets; gate code (and only gate code) declares failures; the cockpit may declare warnings.**

### What stays raw text

Not everything deserves schema. Stays raw, kept verbatim in the artifact folder:

- Full stdout/stderr logs (`mc2_01.log` etc.) — the forensic ground truth; the structured index always links back to a log line number.
- Crash-handler stack dumps, DbgHelp stackwalks (editor watchdog), shader compiler error text.
- One-off printf diagnostics behind env gates that haven't earned a versioned tag yet. Promotion path: raw printf → bracketed `[TAG v1] key=value` line → (only if a tool needs it) JSON sidecar. Most tags should stop at stage two.
- Tracy captures, RenderDoc captures, screenshots — binary artifacts, referenced by path from the run manifest, never parsed.

---

## 3. Proposed event schema (canonical JSON)

The engine keeps emitting **bracketed key=value lines** (the format proven by `gos_smoke.cpp` and inherited by bridge protocol v0). The canonical JSON is produced by a **deterministic line-to-JSON lift** in tooling (extending `smoke_lib/logparse.py`), not by the engine — this is what keeps coupling non-invasive. One JSON object per line, NDJSON, append-only:

```json
{
  "v": 1,
  "tag": "MECH_MATERIAL_GPU",
  "tag_v": 1,
  "kind": "oracle",
  "ts_ms": 184223,
  "frame": 11052,
  "session": "2026-06-10T14-03-22_mc2_01",
  "source": "smoke",
  "fields": { "event": "compare", "mechs": 12, "mismatches": 0 },
  "raw_line": 48211
}
```

- `tag`/`tag_v`: from the bracket — `[MECH_MATERIAL_GPU v1]` → `("MECH_MATERIAL_GPU", 1)`. Unversioned tags (`[FASTPATH_DROP]`) lift as `tag_v: 0`.
- `kind`: `event | counter | oracle | gate` — assigned by a **tag registry** file (`tests/telemetry/tag-registry.json`, new) that maps each known tag to its kind, its oracle fields (which keys must be 0), its counter fields, and a one-line modder-facing description. Unknown tags lift as `kind: "event"` and are flagged in the cockpit as unregistered.
- `frame`: present when the line carries `frame=`; else null.
- `session`/`source`: stamped by the lifter from the run manifest (`source ∈ smoke | playtest | bridge | manual`).
- `raw_line`: line number into the verbatim log — every JSON fact is one click from its forensic origin.
- NDJSON file: `telemetry.ndjson` per run, written next to `report.json`. Append-only, never rewritten.

The same grammar covers the periodic state dump consumed by the `mc2-render-state` MCP server (`MC2_DEBUG_STATE_DUMP=1`, every 300 frames): that JSON file is treated as a *snapshot-type counter source* and joined into the run by session id. The MCP server stays exactly what it is — a live reader/bridge — and gains one tool later (`get_telemetry_tail`) that tails `telemetry.ndjson` instead of duplicating parsing.

---

## 4. Artifact folder structure

Extends — does not replace — today's `tests/smoke/artifacts/<timestamp>/`:

```
tests/smoke/artifacts/<timestamp>/        # one RUN (immutable once finalized)
  manifest.json          # NEW: run identity — exe path+mtime+git describe, deploy target
                         #   (v0.4 vs 0.4c — the known stale-exe trap), mission list, tier,
                         #   duration, env-var deltas (MC2_* set), source (smoke/playtest/bridge)
  <mission>.log          # verbatim stdout/stderr (unchanged, ground truth)
  report.json / report.md# unchanged — run_smoke verdict authority (smoke_lib/report.py)
  telemetry.ndjson       # NEW: lifted structured facts (one file per run, all missions,
                         #   each record carries mission stem in fields)
  oracle_summary.json    # NEW: per-oracle final values + verdict vs registry (see §9)
  budget_eval.json       # NEW: counter values vs budget file in effect (see §7)
  captures/              # screenshots, debug-state JSON snapshots, RenderDoc paths

tests/telemetry/                          # versioned config (in git)
  tag-registry.json      # tag → kind/fields/description/owner-doc link
  budgets/<profile>.json # budget files (see §7)
tests/smoke/baselines.json                # existing destroys/perf baselines (unchanged)
```

Playtest and bridge sessions write the same folder shape under `tests/smoke/artifacts/playtest/<timestamp>/` (shadow-pak path from `runtime-bridge-architecture.md` §2 recorded in manifest). One folder shape = one cockpit loader.

---

## 5. Panels / views

The cockpit ships in two skins over one data layer: **standalone HTML report** (v1 — generated per run / per comparison, viewable headless-produced, zero runtime deps) and **editor ImGui panels** (v2 — same JSON, docked next to the Playtest session panel). Panels:

1. **Run Overview** — manifest + smoke verdict (verbatim from report.json), bucket list, per-mission PASS/FAIL table. Mirrors report.md; adds links into everything below.
2. **Oracle Board** — one row per registered oracle tag: final mismatch counts, green/red, sparkline over the run's periodic emits, link to owning doc (e.g. `docs/oracle-dynamic-pipeline-gate.md`, `docs/vfx-oracle-coverage.md`). "Vacuous" detection: an oracle that emitted zero compare events (e.g. `[FX_COUNT v1]` in idle smokes, which is expected — smoke missions are fly-throughs) is shown grey, not green.
3. **Counter Trends** — FPS/p1%, destroys, OBJBATCHER summary fields, cost-split buckets (`[SPFLUSH_COST_SPLIT v1]` ns averages) plotted per frame-window, with budget bands overlaid.
4. **Event Timeline** — frame-ordered events (FASTPATH_DROP transitions, GPU_CULL `readback_stale_reset`, mission_ready, crash banner) on a shared frame axis with counter inflections — the attribution view ("what happened right before p1% cratered").
5. **Run Compare (A/B)** — see §6.
6. **Modder View** — see §8; a filtered, plain-language rendering of warnings/failures.
7. **(editor only) Live Tail** — during Playtest, tail of decoded events from the stdout pipe; same registry, same severity colors; promotes to a finalized run folder on session exit.

---

## 6. Baseline comparison model (how engineers compare two runs)

A comparison is `compare(runA, runB) → diff.json + diff.html`, pure function of two run folders:

- **Identity diff first:** manifest vs manifest — exe build, git describe, env-var deltas, deploy target. Half the historical false alarms in this repo were stale-exe or wrong-deploy-target (MEMORY: v0.4 vs 0.4c trap); the cockpit surfaces this *before* any numeric diff and stamps the comparison `SUSPECT` if exe mtime < claimed fix commit time.
- **Oracle diff:** any oracle 0→nonzero is the headline, full stop.
- **Counter diff:** per-counter `(A, B, Δ, Δ%)` with noise bands from `baselines.json` history (mean ± observed spread, extending `smoke_lib/baselines.py` which already stores per-key means). The FPS-while-minimized caveat (SDL ~100fps cap, documented in `report.py`) is carried as a per-counter annotation so nobody re-discovers it.
- **Event diff:** set/sequence diff of event tags (e.g. "B has 14 FASTPATH_DROP transitions, A has 0").
- **Designated-baseline runs:** a run folder can be blessed by writing its timestamp into `tests/telemetry/budgets/<profile>.json` as `baseline_run` (this is the formalization of "Baseline A off 0.4c" from the roadmap — golden frames/per-pass timings/oracle counters become a blessed run folder, not a wiki note).

Headless contract: `py -3 scripts/telemetry_compare.py <runA> <runB>` prints the markdown diff and exits 0/1/2 (same / advisory deltas / oracle or gate regressions) — agent-consumable.

---

## 7. Budget model + versioning

Budgets live in git, never in the engine:

```json
// tests/telemetry/budgets/stock.json
{
  "schema_v": 1,
  "baseline_run": "2026-06-09T19-27-36",
  "baseline_commit": "43e22401",
  "budgets": {
    "perf.avg_fps@mc2_01@tier1@30":   { "min": 70, "severity": "advisory" },
    "perf.p1low_fps@mc2_01@tier1@30": { "min": 40, "severity": "advisory" },
    "counter.destroys_delta":          { "abs_max": 0, "severity": "hard" },
    "counter.SPFLUSH.submit_loop_ns":  { "max": 500000, "severity": "advisory" }
  }
}
```

- **Versioned by git history** — a budget change is a reviewed commit citing the run folder that justified it (same discipline as `baselines.json` updates today). The `baseline_commit` field makes "budget vs which code" answerable forever.
- **Keyed like baselines** — reuse `smoke_lib/baselines.py::key` (`<profile>@<stem>@<tier>@<duration>`) so perf budgets are mission/duration-specific and the minimized-window FPS cap doesn't poison cross-config comparisons.
- **Per-mod budgets later:** a mod may ship `telemetry-budgets.json` in its project folder (packaging lane, `mod-packaging-deploy-architecture.md`) to declare *its own* perf expectations (e.g. a 1K-map mod accepts lower fps); these only ever apply to runs with that mod active and only at advisory severity.

---

## 8. Severity model — hard gates vs advisory warnings, and the modder view

Three severities, strictly ordered by who is allowed to produce them:

| Severity | Producer | Effect | Examples |
|---|---|---|---|
| **FAIL (hard gate)** | `smoke_lib/gates.py` only (engine-reported fail, crash, gl_error, pool_null, asset_oob, shader_error, missing_file, timeout, instrumentation_missing) + oracle-mismatch≠0 on *registered slice-gate oracles* | run_smoke exit ≠ 0; blocks slice merge | existing buckets; `RENDER_SNAPSHOT` mismatch>0 |
| **WARN (advisory)** | cockpit budget evaluation | annotates report, never changes exit code | fps below band, cost-split bucket growth, unregistered tag seen |
| **INFO** | everything else | timeline/trends only | FASTPATH_DROP transitions (env-gated diag), FX counts |

**Promotion rule:** an advisory becomes a hard gate only by (a) adding it to a gate in `gates.py` or to the oracle registry's `gate: true` set, (b) with a baseline run proving it's stable at 0/in-band across tier1 5/5. The cockpit cannot promote anything by itself — that's how we guarantee the run_smoke verdict path is never destabilized by cockpit evolution.

**Modder-facing errors without engine logs:** the tag registry carries, per tag and per gate bucket, a `modder_text` template — plain language, names the *asset or content* not the subsystem, and a next step. Examples: `missing_file` → "Your mission references `data/tgl/foo.ini` which isn't in your mod or base data. Add the file or fix the reference in the appearance entry." `asset_oob` → "An icon atlas index is out of range — usually a mech variant pointing at a missing icon slot (campaign mods hit this with Clan mechs; see allow_asset_oob)." The Modder View panel shows *only* FAIL/WARN entries rendered through these templates, with the raw log line behind a "details" expander. This is the same philosophy as the Mission Validator checklist in the bridge doc: judgments up front, forensics on demand.

---

## 9. Bridge to oracle_report

"Oracle report" today is distributed: gate tables in `docs/oracle-dynamic-pipeline-gate.md` (the canonical oracle-item ↔ log-tag ↔ pass-criterion table), per-domain coverage matrices (`docs/vfx-oracle-coverage.md`), and the numbers buried in `<mission>.log` + `report.json`. The cockpit formalizes this as:

1. The **tag registry** (§3) ingests the gate doc's table — each oracle row becomes a registry entry (`tag`, `fields_must_be_zero`, `gate: true/false`, `doc: "docs/oracle-dynamic-pipeline-gate.md"`). A check script (`scripts/check-oracle-registry.py`, pattern of existing `check-*.sh` invariant scripts) fails pre-commit if a doc gate table and the registry drift.
2. `run_smoke.py` finalization (after verdict — never before) runs the lifter + writes `oracle_summary.json`: per-oracle final value, pass/vacuous/fail, baseline value from the blessed run. This file is what agents grep instead of reconstructing the gate table by hand each session.
3. The gate docs remain the human narrative (why each oracle exists, capture caveats like "baseline MUST be the dirty tree"); the registry is the machine projection. Doc owns *why*, registry owns *what*.

## 10. Bridge to editor Playtest (one-click)

Per `runtime-bridge-architecture.md`: PlaytestSession already owns a telemetry decoder over the stdout pipe. Integration:

- The supervisor sets the run up exactly like run_smoke does (env vars from a per-session profile; writes `manifest.json` at launch into a fresh playtest artifact folder).
- The line decoder is the *same* lifter library (shared Python is fine for smokes; the editor gets a small C++ line-lifter implementing the identical grammar — grammar is the contract, not the code).
- Live Tail panel (§5.7) shows decoded events during the session; on `Exited/Crashed` the folder is finalized (oracle_summary + budget_eval) and immediately comparable against any smoke run — **a playtest session and a smoke run are the same artifact species**, which is the whole point.
- Crash flow: crash banner event + shadow pak + log + telemetry.ndjson in one folder = a complete repro bundle a modder can zip and attach.

---

## 11. Anti-goals (binding)

- **No live debugger attachment** — no ptrace/DbgHelp-attach from tooling; the editor watchdog stays an in-process engine facility.
- **No second verdict engine.** Cockpit never overrides, re-derives, or races `smoke_lib/gates.py`. Cockpit code paths run strictly after verdict finalization.
- **No engine-side JSON emission on hot paths.** The engine emits printf lines under env gates as today (zero cost when off — the `MC2_GPU_CULL_READBACK_TRACE` discipline); structuring is the tooling's job.
- **No telemetry database/server in v1.** Folders + NDJSON + git-versioned config. Sockets are the bridge doc's lane; a queryable store is earn-it-later.
- **No mandatory cockpit step in the inner loop.** run_smoke without cockpit artifacts must keep working forever (lifter failures are warnings, never smoke failures).
- **No parsing of binary artifacts** (Tracy, RenderDoc) — reference by path only.

## 12. Implementation phases

- **Phase 0 — Registry + lifter (foundation).** `tests/telemetry/tag-registry.json` seeded from the oracle-gate doc + tier1_env_vars tags; `scripts/telemetry_lift.py` (reuses `smoke_lib/logparse.py` patterns) producing `telemetry.ndjson` from any existing log. Works retroactively on old artifact folders.
- **Phase 1 — Run manifest + post-verdict hook.** `run_smoke.py` writes `manifest.json` and invokes lifter + `oracle_summary.json` after verdict. Verdict path untouched (hook wrapped in try/except → warning).
- **Phase 2 — Compare + HTML report.** `telemetry_compare.py` + single-file HTML report generator (Run Overview, Oracle Board, Compare). Bless Baseline A run here.
- **Phase 3 — Budgets.** `tests/telemetry/budgets/stock.json` + `budget_eval.json` + WARN lane in the HTML report. All advisory.
- **Phase 4 — Editor integration.** C++ line-lifter in PlaytestSession, Live Tail + Oracle Board ImGui panels, playtest artifact folders. Modder View templates.
- **Phase 5 — Promotions + MCP.** Promote proven advisories into gates.py (per §8 rule); `mc2-render-state` MCP gains `get_telemetry_tail`/`compare_runs` tools; consider socket transport only now.

## 13. First 5 concrete slices

1. **S1 — tag-registry.json v1** covering the 9 oracle-gate rows + FASTPATH_DROP, OBJBATCHER, GPU_CULL, SPFLUSH_COST_SPLIT, SNAPSHOT_BRIDGE_COMPARE, TIMING; plus `scripts/check-oracle-registry.py` drift check. (Pure data + check script; zero engine/smoke risk.)
2. **S2 — telemetry_lift.py**: log → telemetry.ndjson, validated against the 2026-06-09T19-27-36 baseline artifact folder (golden test: known counts of each tag).
3. **S3 — manifest.json in run_smoke** (exe path/mtime/git describe/env deltas/deploy target), written before launch, finalized after — directly kills the stale-exe false-alarm class.
4. **S4 — oracle_summary.json + post-verdict hook** in `run_smoke.py` (try/except-isolated), then update `docs/oracle-dynamic-pipeline-gate.md` capture section to point agents at the JSON.
5. **S5 — telemetry_compare.py** A/B over two artifact folders, markdown out, exit codes 0/1/2; smoke-test it on baseline-vs-HEAD.

## 14. Follow-up prompts for Opus/Codex

- "Implement S1+S2 from `docs/superpowers/strategy/telemetry-oracle-cockpit-architecture.md`: create `tests/telemetry/tag-registry.json` seeded from the oracle table in `docs/oracle-dynamic-pipeline-gate.md` and `docs/tier1_env_vars.md` tags, plus `scripts/telemetry_lift.py` reusing `scripts/smoke_lib/logparse.py` regexes; golden-test against `tests/smoke/artifacts/2026-06-09T19-27-36/`. Do not modify run_smoke.py."
- "Audit every bracketed `[TAG vN]` printf in GameOS/, mclib/, code/ against the new tag registry; list unregistered tags with file:line, proposed kind (event/counter/oracle), and whether the emit is env-gated. Output a table, change no code."
- "Implement S3+S4 (manifest.json + post-verdict oracle_summary.json hook in scripts/run_smoke.py) with the hard constraint that any exception in the new code is swallowed to a warning and the smoke exit code is byte-identical for the existing tier1 flow; prove with a tier1 run."
- "Design the C++ line-lifter for PlaytestSession (Phase 4): same grammar as telemetry_lift.py, fed from EditorTaskRunner::HandleLine per runtime-bridge-architecture.md §3; propose the ImGui Oracle Board panel layout consistent with the existing EditorInterface dock column."
- "Propose the budget set for `tests/telemetry/budgets/stock.json` from the blessed Baseline A run: which counters are stable enough for bands, which are minimized-window-FPS-poisoned, and which oracle tags meet the §8 promotion rule for gates.py today."
