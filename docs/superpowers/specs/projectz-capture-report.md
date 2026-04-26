# PROJECTZ Capture Report

**Date:** 2026-04-26
**Branch:** `projectz-hunting`
**Spec:** [`2026-04-25-projectz-containment-design.md`](2026-04-25-projectz-containment-design.md)
**Commits consumed (1-4):**
- `eb2e9f4` projectz: add LegacyProjectionResult sidecar to projectZ
- `d8f3fd4` projectz: tag callsites by intent and add inventory
- `906fcac` projectz: env-gated diagnostic instrumentation (PROJECTZ v1)
- `d0a7b9c` projectz: fix trace-mode perf (file output) and guard_px default
- `a61a9db` projectz: document MC2_PROJECTZ_TRACE=1 FPS cost in startup banner
- `8d25885` projectz: debug overlay (RAlt+P)

**Capture environment:** Windows 10 22H2, AMD RX 7900 XTX, MC2 RelWithDebInfo,
deploy `A:/Games/mc2-opengl/mc2-projectz-hunting/`, screen 1920×1080.

**Prior artifacts referenced:**
- [`projectz-callsite-inventory.md`](projectz-callsite-inventory.md) — 99 callsites tagged across 7 categories
- [`projectz-validation-phase3.md`](projectz-validation-phase3.md) — 5-frame proof-of-concept capture; first-pass numbers superseded by the data here

---

## TL;DR — Verdict

**Outcome C selected.** No global projectZ replacement exists. The capture
data rejects every candidate predicate as a drop-in replacement and supports
a category-split policy: preserve legacy admission for terrain, lighting,
picking, and inverse-projection pairs; safely modernize ScreenXYOracle and
DebugOnly callsites; keep `legacyRectFinite` as a permanent free safety
invariant. `rectGuard` is a strict superset of legacy admission but its
permissive admissions are concentrated in terrain — the known wedge-risk
vector — so it remains investigational, not a replacement candidate, until
a future visual-overlay pass disambiguates whether those extra admissions
are over-cull-fixes or wedge-producers.

### Per-category verdict

| Category | Recommendation | Confidence |
|---|---|---|
| `BoolAdmission` / terrain | Keep legacy `projectZ` admission | High |
| `SelectionPicking` | Keep legacy for now; investigate `rectGuard` separately as a UX-improvement spec | Medium |
| `LightingShadow` | Keep legacy for now; possible category-specific modern predicate later | Medium |
| `ScreenXYOracle` | Safe to split from bool admission | High |
| `DebugOnly` | Safe to modernize or simplify | High |
| `InverseProjectionPair` | Keep legacy until picking/TacMap are separately audited | High |
| `legacyRectFinite` | Keep as free safety invariant (co-running) | Very high |

See **§ 5 Verdict thesis** for the full reasoning, and § 6 Open questions
for follow-up specs.

---

## 1. Methodology

### Captures performed

| Tier | What | Tool | Env vars | Result |
|------|------|------|----------|--------|
| Baseline | tier1 × 5 missions, 60s default-camera | `scripts/run_smoke.py --tier tier1 --kill-existing --keep-logs` (no menu canary) | `MC2_PROJECTZ_SUMMARY=1 MC2_PROJECTZ_HEATMAP=1` | 4/5 PASS; mc2_17 hit walltime cap during load (unrelated to PROJECTZ — 0 in-mission frames) |
| Stress | 30-90s manual drives on mc2_01, mc2_03, mc2_06, mc2_10 (edge-pan all four corners + rapid rotation) | `scripts/projectz_capture_stress.py` (manual, user-driven) | `MC2_PROJECTZ_TRACE=1 SUMMARY=1 HEATMAP=1` | 4/4 captured. Trace logs ranged 2.5-7 GB; aggregator processed line-by-line in 1-3 min each. mc2_06 added for breadth (non-tier1) |
| GUARD=64 | mc2_01, two scope variants | `scripts/projectz_capture_stress.py` with `MC2_PROJECTZ_GUARD_PX=64` | `TRACE+SUMMARY+HEATMAP` (30s) and `SUMMARY+HEATMAP` (60s) | Both captured. **Two distinct captures, different aggregation scopes — see § 4 footnote** |
| Custom campaigns | Not run — out of scope for this report | n/a | n/a | Documented gap; cross-mission convergence (§ 3) makes it low-priority |

**Trace-mode FPS cost.** With `MC2_PROJECTZ_TRACE=1` the engine writes ~3300
formatted records per perspective frame to `mc2_projectz.log`, dropping
framerate from 142+ FPS to ~35 FPS. This is documented at
[mclib/projectz_trace.cpp:184](../../../mclib/projectz_trace.cpp). The
baseline tier1 run uses SUMMARY+HEATMAP only (full speed) so per-mission
perf gates don't fail; stress runs use full TRACE because the user is
driving and perf does not need to clear a gate.

**Per-callsite resolution caveat.** Summary-mode logs only emit homogClip
and rectSignedW disagreement counts per callsite (commit-4 outlier-line
format). Trace-mode logs give all five predicates per callsite. The
aggregator handles both modes; per-callsite × all-five-predicates rows are
only available for scenarios that ran with TRACE on.

### Aggregator

[`scripts/projectz_aggregate.py`](../../../scripts/projectz_aggregate.py) — stdlib-only Python.

Input: any log containing `[PROJECTZ v1]` records.
Output: one CSV per (mission, scenario):

```
callsiteId, predicate, agree, disagree_permissive, disagree_restrictive,
finite_violation, total_calls
```

Optional second CSV with per-callsite triangle counts (total / contains-rejected /
purple). Trace mode wins when records are present; summary blocks are the
fallback. Idempotent (rows sorted; same input → byte-identical output).

### Data sidecars

Per-scenario data lives under `docs/superpowers/specs/projectz-captures/`:

| File | Provenance |
|------|------------|
| `<mission>__<scenario>.stdout.log` | Captured stdout (summary blocks) |
| `<mission>__<scenario>.trace.log` | `mc2_projectz.log` copied from deploy dir (only when TRACE on) |
| `<mission>__<scenario>.csv` | Per-callsite × per-predicate aggregate |
| `<mission>__<scenario>.tri.csv` | Per-callsite triangle aggregate |
| `<mission>__guard64.summary.txt` | 60s GUARD=64 console-summary block (verbatim) |

**Captures included in this report (all under `projectz-captures/`):**

Baselines (CSV + tri.csv): `mc2_01__baseline`, `mc2_03__baseline`,
`mc2_10__baseline`, `mc2_17__baseline`, `mc2_24__baseline`.

Stress (CSV + tri.csv + trace.log): `mc2_01__stress_edges_rotation`,
`mc2_03__stress_edges_rotation`, `mc2_06__stress_edges_rotation`,
`mc2_10__stress_edges_rotation`. (`mc2_01__stress_zoom_out.trace.log`
present from earlier exploratory pass; not in main analysis.)

GUARD=64: `mc2_01__guard64.csv` + `.tri.csv` + `.trace.log` (30s capture)
and `mc2_01__guard64.summary.txt` (60s console summary, verbatim).

Trace logs (multi-GB) are committed where practical via git LFS-style
restraint; overflow logs may be replaced with a `.large.txt` note in
follow-up commits. Smaller logs commit verbatim so the verdict is
reproducible.

---

## 2. Per-predicate aggregate (baseline tier1 × 5 missions)

Numbers below come from the **shutdown summary line** of each baseline log.
All five missions ran SUMMARY+HEATMAP for ~60s each.

`legacyRectFinite` and `rectGuard` are omitted from the table — `legacyRectFinite`
shows 0.00% disagreement on every mission (legacy never produces non-finite
output); `rectGuard` with default `MC2_PROJECTZ_GUARD_PX=0` is definitionally
identical to legacyRect. Their presence is the sanity check.

| Mission | persp_calls | homogClip dis% | rectSignedW dis% | rectNearFar dis% | tris total | red% | purple% |
|---------|------------:|---------------:|-----------------:|-----------------:|-----------:|-----:|--------:|
| mc2_01  |  47,195,214 |  9.19%         |  9.18%           |  9.18%           | 14,153,860 | 70.0% | 36.9% |
| mc2_03  |  42,308,871 | 50.92%         | 50.91%           | 50.92%           | 15,128,830 | 21.8% | 34.8% |
| mc2_10  |  98,858,023 | 31.37%         | 15.59%           | 15.67%           | 32,916,512 | 19.7% | 18.0% |
| mc2_17  | 158,328,958 | 28.46%         | 10.42%           | 14.07%           | 43,551,296 | 52.1% | 11.0% |
| mc2_24  |  65,891,640 |  4.98%         |  4.98%           |  4.98%           | 10,557,190 | 37.4% | 49.7% |

Disagreement is **always restrictive** (`disagree_perm=0` for rectSignedW
and rectNearFar in all 5 missions). The two exceptions where homogClip
shows permissive disagreement are mc2_10 and mc2_17. mc2_03's rectSignedW
disagreement of 50.91% is the strongest single-mission signal that no
modern predicate is a viable drop-in.

### Cross-mission patterns (baseline)

1. **rectSignedW ≈ rectNearFar** within ≤4pp on every mission.
2. **homogClip ≥ rectSignedW** on every mission, sometimes substantially
   (mc2_17: 28% vs 10%). Delta is `disagree_perm` — homogClip accepts
   off-screen-but-in-front-of-camera vertices that legacy rejects.
3. **disagree_restrictive is 5–51% across baseline missions.** Mission
   content drives the variance; this gets converged out under stress
   conditions (§ 3).
4. **`legacyRectFinite` 0% in 5/5 missions.** Legacy admission never
   produces non-finite screen coordinates. The `fabs(rhw)` line *destroys
   the W sign* but does not produce NaN/Inf.
5. **Red triangle % is high (20–70%) even at baseline.** Most submitted
   terrain triangles already contain at least one legacy-rejected vertex;
   `pol_any` (any vertex passes) is the active triangle policy.

### Why these numbers strongly suggest Outcome A or C, not B

For Outcome B (a single modern predicate replaces legacy), the spec
requires >99.9% agreement on the wedge-prone scenarios. **No predicate
clears 99.9% on any mission, much less on stress.** rectSignedW comes
closest on mc2_24 (95.02%) and worst on mc2_03 (49.09%). A 50.91pp gap
on a single mission rules out drop-in substitution.

---

## 3. Stress capture findings

### Cross-mission terrain admission convergence

Across four very different missions (snowy / urban / open desert / mixed),
under uniform geometric stress (edge-pan all four corners + rapid rotation),
the homogClip restrictive disagreement on `terrain_cpu_vert_admit` lands in
a tight band:

```
terrain_cpu_vert_admit homogClip restrictive disagreement %:
  mc2_01 stress: 37.7%
  mc2_03 stress: 36.0%
  mc2_06 stress: 34.1%
  mc2_10 stress: 38.4%
```

**34-38% range across four missions.** Mission content drove baseline
variance (5-51%), but uniform geometric stress overrides it. This means
**"modern predicates over-cull ~36% of currently-admitted terrain calls"
is a cross-content architectural constant**, not a mission-specific
number. This is the single most important finding for the verdict: no
matter how representative or unrepresentative any single baseline is, the
~36% over-cull is what happens once you actually drive the camera into
the wedge-prone configurations.

### Weather (mc2_10 only — the rain mission)

| Callsite | Calls | homogClip restrictive % |
|----------|------:|------------------------:|
| `weather_raindrop_top` | 477,000 | 69.8% |
| `weather_raindrop_bot` | 332,742 | **99.9%** |

Modern predicates would over-cull virtually all rain. Third subsystem
(after terrain admission and picking) requiring legacy retention.

### Picking (all stress missions)

- `picking_closest_cell_center` 81-96% restrictive across all stress
  missions. Strongly content-independent — the picking math hits the
  modern predicates at near-uniform rates everywhere.
- `picking_closest_vertex_fallback` mostly restrictive, but
  **mc2_06 anomaly: 10.7% permissive on this site only.** Cross-mission
  outlier; documented as a per-mission footnote, not a general claim.
  Possible cause: mc2_06's terrain shape produces fallback iterations
  that land in the off-screen-but-in-front geometry homogClip accepts
  but legacy rejects. Worth a closer look in any future
  picking-precision audit.

### `legacyRectFinite` confirmed across 6 captures

(4 stress + 1 GUARD trace + 1 baseline cross-section): zero disagreement
everywhere, zero finite violations. **Free upgrade.** Whatever shape the
legacy admission output takes, it is always finite. Adding the
`isfinite()` check as a co-running invariant costs essentially nothing
and would catch the nightmare scenario (NaN/Inf in admitted output) for
free.

### Triangle metrics — important wording

Across stress runs, `tri_contains_rejected` ("red") ranged 16-53% and
`tri_purple` (NaN/Inf or extreme projected area) ranged 3.7-10.1%.

**These are not "wedge suspects."** Those triangles are submitted in the
currently-rendering game, which is not visibly broken. Re-frame:

> "Red and purple are triage signals. Purple is closer to actual visual
> risk (NaN/Inf or extreme projected area); red identifies triangles
> whose vertex-admission state diverges from the submitted-cluster index
> set, which needs interpretation against the actual submission policy
> rather than treated as an outright wedge marker."

The 70% baseline → 53% stress contains_rejected drop and the 37% baseline
→ 7% stress purple drop both confirm these are camera-condition / content
artifacts, not wedge predictors.

---

## 4. GUARD=64 findings

### Strict-superset property

With `MC2_PROJECTZ_GUARD_PX=64` (viewport expanded by 64 px on every
side), `rectGuard` produces **zero restrictive disagreement** across all
callsites:

- 60s capture (3602 frames, 50.7M perspective calls):
  `rectGuard agree=48,431,295 disagree_perm=2,317,859 disagree_restr=0 (4.57%)`
- 30s capture (952 frames, 4.1M perspective calls):
  `rectGuard agree=3,931,471 disagree_perm=185,024 disagree_restr=0 (4.49%)`

Whatever legacy admits, rectGuard also admits. The strict-superset
property holds at both scopes; the bounded permissive rate (4.49-4.57%)
is stable.

All extras are finite (`legacyRectFinite finite_viol=0` chains through).
All extras are within 64 px of the screen edge by construction.

### Per-callsite rectGuard permissive distribution (30s trace)

```
terrain_cpu_vert_admit:        103,186  (5.65% of site total)
terrain_quad_vert2_admit:       32,399  (4.87%)
unknown:                        45,736  (3.37%)
terrain_quad_vert{0,1,3}:        ~1,400  (~2%)
picking_closest_cell_center:       451  (11.24% — highest %, tiny absolute)
picking_closest_vertex_fallback: 1,888  (0.99%)
```

**74% of permissive admissions concentrate on terrain admission sites.**
Picking contributes only ~2,300 absolute (1.3% of total). This answers
the question the global summary couldn't: terrain admission is exactly
where rectGuard adds extra admissions, which is exactly the wedge-risk
vector (terrain admission feeds tessellation control points).

### Wording — DO NOT say "rectGuard causes wedges"

> rectGuard's extra admissions are concentrated in the callsite category
> that feeds tessellation control points — the known wedge-risk vector.
> Without visual overlay confirmation, we cannot distinguish harmless
> near-edge recovery (legacy was over-culling) from wedge-producing
> over-admission (legacy was correctly culling). The current data proves
> **risk concentration**, not visual failure.

### Aggregation-scope footnote

> Two GUARD=64 captures of differing length and aggregation scope appear
> in this report. The console-summary capture (3602 frames, 50.7M
> perspective calls) reports global predicate aggregates only. The
> full-trace capture (~30s, 4.1M perspective calls) reports per-callsite
> breakdowns. Both point to the same conclusion (rectGuard is a strict
> superset with bounded permissive concentrated on terrain) but their
> raw counts are not directly comparable — different scopes. The 60s
> summary lives verbatim at
> [`projectz-captures/mc2_01__guard64.summary.txt`](projectz-captures/mc2_01__guard64.summary.txt);
> the 30s per-callsite data is in `mc2_01__guard64.csv` /
> `.tri.csv` / `.trace.log`.

---

## 5. Verdict thesis

> The capture data rejects a single global projectZ replacement.
> `homogClip`, `rectSignedW`, and `rectNearFar` are too restrictive to
> replace legacy admission (~36% over-cull on terrain admission, 80-99%
> on picking and weather). `rectGuard` is a strict superset of
> `legacyRect`, but its permissive admissions are concentrated in
> terrain admission callsites, which are the known wedge-risk vector.
> Therefore terrain `BoolAdmission` must remain on the legacy
> screen-rect contract until a visual overlay pass proves a broader
> guard-band policy is safe.
>
> The correct next architecture is category split: preserve legacy
> admission for terrain, separate screen-XY oracle uses, keep legacy for
> picking and inverse-projection pairs until separately audited, and
> retain `legacyRectFinite` as a co-running diagnostic invariant.

### Why not Outcome A (legacy required everywhere, forever)

Outcome A is consistent with the data but **strictly stronger than the
data requires**. The 51 ScreenXYOracle and 25 DebugOnly callsites do
not consume the legacy bool and cannot exhibit the wedge condition by
construction. Treating them as legacy-bound is a wider blast radius
than necessary and contaminates downstream renderer modernization.

### Why not Outcome B (single modern predicate)

Outcome B requires a single modern predicate that agrees with legacy at
≥99.9%. The closest candidate (rectSignedW) hits 49.09% on mc2_03
baseline and 62-66% on stress runs across all four stress missions. The
data is unambiguous: no candidate clears the bar.

### Conditions under which the verdict could become A

If a future visual-overlay pass proves rectGuard's terrain permissive
admissions correlate with visible wedge artifacts, even ScreenXYOracle
migration becomes risky and the verdict tightens to A. The overlay GL
bug (§ 6) currently blocks this disambiguation.

### What the next spec should consume from this report

1. The per-category policy table at the top.
2. The cross-mission terrain over-cull constant (~36%) as the empirical
   floor any replacement predicate must clear before substitution
   discussion is reopened.
3. A picking-precision verification task (drag-select near screen edges
   under legacy vs `rectGuard`; compare hit sets) — see § 6.
4. An inverse-projection / TacMap audit task — see § 6.
5. Note that custom-campaign content is not represented; cross-mission
   stress convergence makes this low-priority but should be acknowledged.

---

## 6. Open questions / follow-up specs

1. **Overlay GL bug.** RAlt+P triggers `GL_INVALID_OPERATION` at
   `gameos_graphics.cpp:577`. Blocks visual disambiguation of rectGuard's
   terrain permissive cases. Filed as a separate task; **NOT a blocker
   for this report.** Likely cause: VAO not bound when the overlay calls
   `gos_DrawTriangles` between `pp->endScene()` and
   `gos_RendererFlushHUDBatch`. Worktree
   [`docs/amd-driver-rules.md`](../../amd-driver-rules.md) may contain
   relevant traps.
2. **Picking-precision UX experiment with rectGuard.**
   `picking_closest_cell_center` showed 11.24% permissive rate (highest
   per-site %, tiny absolute volume). This suggests rectGuard could
   admit cursor probes legacy currently rejects — possibly fixing
   edge-case UX where terrain near the screen edge can't be selected.
   Separate spec.
3. **Inverse-projection (TacMap) audit.** The inventory marks 4
   `InverseProjectionPair` callsites in `code/gametacmap.cpp`; they were
   not part of admission disagreement testing in this spec. Separate
   audit needed before any changes there.
4. **Custom campaign coverage** (CVE-G, Carver5O, Omnitech, Wolfman) —
   not captured. Cross-mission terrain rate convergence (34-38%) on four
   diverse stock missions makes this a low-priority gap, but should be
   acknowledged.
5. **mc2_06 picking_closest_vertex_fallback permissive anomaly (10.7%).**
   Per-mission outlier vs all other stress missions. Worth a closer look
   in any future picking-precision audit; not material to the verdict
   here.
6. **mc2_17 baseline did not produce gameplay frames.** The 158M
   persp_calls in its summary represent loader/editor projectZ activity,
   not in-mission. Treated as suspect for in-mission patterns; the
   stress matrix did not include mc2_17 either.

---

## Appendix A — Reproducing the baseline

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/projectz-hunting

MC2_PROJECTZ_SUMMARY=1 MC2_PROJECTZ_HEATMAP=1 \
    py -3 scripts/run_smoke.py --tier tier1 --kill-existing --keep-logs \
        --exe A:/Games/mc2-opengl/mc2-projectz-hunting/mc2.exe

ls -lat tests/smoke/artifacts/ | head -3

for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
    py -3 scripts/projectz_aggregate.py \
        tests/smoke/artifacts/<TIMESTAMP>/${m}.log \
        --out  docs/superpowers/specs/projectz-captures/${m}__baseline.csv \
        --tri-out docs/superpowers/specs/projectz-captures/${m}__baseline.tri.csv
done
```

## Appendix B — Reproducing one stress capture

```bash
py -3 scripts/projectz_capture_stress.py mc2_01 stress_edges_rotation

py -3 scripts/projectz_aggregate.py \
    docs/superpowers/specs/projectz-captures/mc2_01__stress_edges_rotation.trace.log \
    --out docs/superpowers/specs/projectz-captures/mc2_01__stress_edges_rotation.csv \
    --tri-out docs/superpowers/specs/projectz-captures/mc2_01__stress_edges_rotation.tri.csv
```

## Appendix C — Reproducing the GUARD=64 capture

```bash
# 60s console-summary scope:
MC2_PROJECTZ_SUMMARY=1 MC2_PROJECTZ_HEATMAP=1 MC2_PROJECTZ_GUARD_PX=64 \
    A:/Games/mc2-opengl/mc2-projectz-hunting/mc2.exe \
    > docs/superpowers/specs/projectz-captures/mc2_01__guard64.summary.txt

# 30s per-callsite trace scope:
MC2_PROJECTZ_TRACE=1 MC2_PROJECTZ_SUMMARY=1 MC2_PROJECTZ_HEATMAP=1 \
MC2_PROJECTZ_GUARD_PX=64 \
    py -3 scripts/projectz_capture_stress.py mc2_01 guard64

py -3 scripts/projectz_aggregate.py \
    docs/superpowers/specs/projectz-captures/mc2_01__guard64.trace.log \
    --out docs/superpowers/specs/projectz-captures/mc2_01__guard64.csv \
    --tri-out docs/superpowers/specs/projectz-captures/mc2_01__guard64.tri.csv
```

---

*Report final v1 (verdict locked: Outcome C). Closes commit 5 of the
projectZ containment spec. Replacement-policy work begins under a
separate spec.*
