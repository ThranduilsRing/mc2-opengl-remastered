<!-- doc-status: current | reviewed:2026-10-11 -->
# Perf budget + oracle harness

The shared vocabulary that turns smoke runs into **comparable** results. Seeded
from Baseline A (`82add3ca`, post-8z, captured off `mc2-win64-0.4c`). Every
structural lane (GlStateGuard, Tube, HZB, IBL/PBR, service-lane) runs its before
/after against this, instead of "looks okay in smoke."

## Pieces

| File | Role |
|---|---|
| `scripts/smoke_lib/oracleparse.py` | Render-oracle parser + `judge_oracles` (hard correctness asserts). Separate from `logparse.py` so adding an oracle can't destabilize the tier1 fault gate. |
| `oracle_report.py` (retired 2026-10-09) | Was the CLI: logs dir → per-mission oracle + `[PERF v1]` table, judged vs this budget. No gate or runner called it; recover it from git history if needed. |
| `docs/perf-budget.json` | Per-mission floors/ceilings. |
| this doc | Vocabulary, thresholds, classification rules. |

## Usage

> Retired 2026-10-09: `oracle_report.py` (and `render_pass_report.py`) are deleted; the usage below is historical. `smoke_lib/oracleparse.py` remains.

```bat
py -3 oracle_report.py --logs <artifact_or_log_dir> ^
    --budget docs/perf-budget.json --strict --md report.md --json report.json
```

Logs are `<mission>.out.log` / `<mission>.err.log` pairs (e.g. the Baseline A
archive at `.claude/baseline-A-logs/`). Run a fresh capture with the headless
pattern from `docs/baseline-A-post-8z.md`, then point the reporter at it.

## Budget vocabulary

| Metric | Source | Meaning | Budgeted? |
|---|---|---|---|
| `avg_fps` | `[PERF v1]` | whole-run mean | yes — floor |
| `p50_ms` | `[PERF v1]` | median frame (steady CPU+GPU) | reference |
| `p99_ms` | `[PERF v1]` | **steady-state tail** — the hitch metric that matters | yes — ceiling |
| `p1low_fps` | `[PERF v1]` | 1%-low fps (hitch severity) | yes — floor (noisy) |
| `peak_ms` | `[PERF v1]` | worst single frame | **NOT budgeted** — captures one-time load/first-frame spike (242–816 ms in Baseline A); not steady-state |

**Why p99_ms, not peak_ms:** `peak_ms` is dominated by the mission-load / first-frame
hitch and is not representative of render-loop cost. `p99_ms` is the steady tail.
A lane that regresses the render loop shows up in `p99_ms` and `p1low_fps`.

Floors/ceilings carry margin (headless idle fly-throughs are noisy):
`avg_fps_floor ≈ 0.85×`, `p99_ms_ceil ≈ 1.5×`, `p1low_fps_floor ≈ 0.7×` of baseline.

## Baseline A reference (the seed)

| Mission | avg_fps | p50_ms | p99_ms | p1low_fps | peak_ms |
|---|---|---|---|---|---|
| mc2_01 | 110.0 | 6.63 | 61.96 | 16.1 | 242.5 |
| mc2_03 | 128.1 | 6.96 | 28.00 | 35.7 | 270.4 |
| mc2_10 | 131.4 | 6.58 | 26.00 | 38.5 | 816.1 |
| mc2_17 | 132.2 | 6.88 | 20.37 | 49.1 | 251.0 |
| mc2_24 | 132.1 | 6.73 | 19.61 | 51.0 | 308.8 |

## Hard oracle asserts (must stay clean post-8z)

`judge_oracles` FAILs on any of: `FASTPATH_DROP≠0`, `terrain_arm_path≠gpu`,
TerrainLOD `parity MISMATCH≠0`, `slimVerts≠0` (legacy slimReduce resurrected),
`fullyArmed=0`, RENDER_SNAPSHOT `fallback`/`count`/`pkt`/`meta`-mismatch≠0,
TEX_RESOLVE `mismatches`/`oob`≠0, MECH_MATERIAL_GPU `mismatches≠0`,
OBJBATCHER `submit_legacy≠0`, GPU_CULL indirect `overflow≠0`.

### OBJBATCHER cpu_fallback classification (the "classify, not panic" rule)

`cpu_fallback` is non-zero in Baseline A on mc2_01 (1456) and mc2_24 (3941) — but
in both `cpu_fallback == late_register_recovery_skips` and `fallback_rate ≤ 0.0007`.
These are **late-register recovery skips** (HUD nodes like `compass` registering
after the batch window), not real geometry on the CPU path.

- `cpu_fallback == late_register_recovery_skips` → **WARN** (classified benign).
- `cpu_fallback  > late_register_recovery_skips` → **FAIL** (`real cpu_fallback` =
  the excess) — real instances fell back to CPU.

This is the structural answer to "OBJBATCHER cpu_fallback needs classification,
not panic": the harness encodes the classification so future runs auto-distinguish.

## Cost-split drill-down (CPU per-pass)

When a whole-frame budget regression fires (`avg_fps`/`p99_ms`/`p1low_fps`),
re-run the mission with the RDTSC cost-split env gates to attribute it to a CPU
subsystem — no Tracy needed, smoke-captured:

```bat
set MC2_MISSION_SPLIT=1 & set MC2_GEOM_PHASE_SPLIT=1 & set MC2_MIF_SPLIT=1
set MC2_TERRAIN_COST_SPLIT=1 & set MC2_STATIC_PROP_FLUSH_COST_SPLIT=1
set MC2_TOBJ_COST_SPLIT=1 & set MC2_LIGHT_COST_SPLIT=1
mc2.exe --profile stock --mission <stem> --duration 30   (+ MC2_SMOKE_MODE=1)
```

Read the `event=summary`/`event=shutdown` `avg_us` fields (ignore `max_us` —
load-hitch). Baseline A §6a holds the filled mc2_01/mc2_24 reference numbers.

## Open residual

**GPU per-pass times (terrain solid / shadow / 3D / post / present) are NOT in
this budget.** Not a headless limit — the engine only wraps `gpu_cull` + water
in a GL `TIME_ELAPSED` query; the main passes have no per-pass timer. Closing it
= a small instrumentation lane (one coarse `glBeginQuery(GL_TIME_ELAPSED)` per
pass, summary-emit like `GPU_CULL`), then re-run the cost-split smoke and fold a
second (GPU) budget tier. See Baseline A §6b. Until then this budget gates
**correctness oracles + whole-frame perf + CPU per-pass attribution**, which is
enough to make GlStateGuard / Tube / HZB before-after comparable.
