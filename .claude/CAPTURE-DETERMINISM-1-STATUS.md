# CAPTURE-DETERMINISM-1 — STATUS

Characterise, source, and where possible pin the run-to-run nondeterminism in
mc2_10 visual captures that was invalidating pixel-A/B evidence across lanes.

**Outcome: partially solved, and now visible + ratcheted.** One real source found
and fixed; one mitigated; one **still open and explicitly not pinned**. The
headline honest statement: **mc2_10 captures are still not reproducible.** What
changed is that this is now measured, documented, instrumented and enforced
instead of silently corrupting verdicts.

Exes: `c0111762` (pre-fix baseline) → `5737632b` (frameLength pin) →
`3b0f22d5` (sim_frame sidecar). All `MC2_BUILD_GIT_DIRTY=0`, deploy lane
`mc2-win64-v0.4c`. Contracts **69/69** at HEAD (68 + the new check).

---

## META REVIEW — premises checked against source, not status docs

| brief premise | verdict |
|---|---|
| mc2_10 captures differ up to 24 % across launches | **CONFIRMED, and slightly worse.** Independently reproduced: 3 launches, byte-identical env, worst pair **27.3 %** `pct_any` / 17.4 % at >8/255 (`saved_204937`). |
| The capture path lacks determinism machinery | **FALSE.** `run_visual_capture.py` already sets `MC2_SMOKE_MODE`, `MC2_SMOKE_SEED`, `MC2_SMOKE_FIXED_TIMESTEP` and already has a `--verify` 2-run byte-diff. The machinery exists; it was **incomplete and dishonest**, not absent. |
| Dominant source is animated night light pools / cloud shadows | **FALSE.** Those are pinned by the fixed clock + the sweep's sim-freeze. The drift is concentrated on **movers** (vehicles/mechs). Mover-free poses on the same map, same run, are byte-identical (0.000 %). |
| `MC2_CAMERA_PARK` is stripped by the runner, so pose-pinned runs measure the default camera | **NOT APPLICABLE to this path.** `run_visual_capture.py` does `env = os.environ.copy()` and strips nothing; the bookmark sweep pins the pose itself and re-applies it every settle frame. Sidecar `cap_pos/cap_rot/cap_alt` were identical across all runs. |
| A capture-determinism ratchet is missing | **CONFIRMED** — added. |
| Contracts 68/68 at HEAD | **CONFIRMED** (69/69 after this lane). |

---

## 1. The measured noise floor

3 launches per arm, identical environment, 3840x2160, exe `c0111762`.
`pct_any` = pixels differing at all, `pct_gt8` = differing by >8/255.

| map | pose | pct_any (worst pair) | pct_gt8 | verdict |
|---|---|---|---|---|
| mc2_10 | `saved_204937` | **27.3 %** | 17.4 % | unusable |
| mc2_10 | `lance_close` | **23.2 %** | 8.4 % | unusable |
| mc2_10 | `saved_204753` | 9.1 % | 6.3 % | unusable |
| mc2_10 | `saved_204523` | 0.008 % | 0.008 % | marginal |
| mc2_10 | `saved_212019` | 0.000 % | 0.000 % | clean |
| mc2_10 | `saved_212140` | 0.000 % † | 0.000 % | **not trustworthy — see below** |
| mc2_01 | all 5 poses | **≤ 0.004 %** | ≤ 0.003 % | clean |

† `saved_212140` was byte-identical across **five** consecutive launches and then
drifted on the sixth. **The noise is intermittent.** Any "N runs agreed"
stability claim — including `run_visual_capture.py --verify`'s 2-run check —
systematically *understates* the floor. This is the single most important
practical finding for other lanes.

**It is not uniform and not global.** It is:
- **map/content-specific** — mc2_01 is effectively clean; mc2_10 is not;
- **spatially concentrated on movers** — for `saved_204937` at settle 150 the
  entire bottom half of the frame is *exactly* 0.0 % and the drift sits in a
  compact blob (two grid cells at 86 % / 95 %). Cropping both runs to the
  >8/255 bounding box and viewing them side by side shows **vehicles at
  different positions**.

---

## 2. Sources

### (a) FIXED — `frameLength` was never pinned  (`5737632b`)

`code/mechcmd2.cpp:2245` computed `frameLength = 1.0 / frameRate` from the **real
measured framerate**, even under `MC2_SMOKE_FIXED_TIMESTEP`. `frameLength` drives
**all** mover / object / GOSFX motion integration.

So S9D pinned `scenarioTime` and S9E pinned the shader clocks — both runs reached
a given render frame with an identical `scenarioTime` and an identical
`deterministic: true` sidecar — while every mover had integrated a **different
per-frame dt** on the way there. Same frame, same pose, same clock, different unit
positions.

Now pinned to the same fixed step (1/30), placed before the sim-speed multiply and
deliberately left unmultiplied, matching `mission.cpp`'s fixed-timestep branch.
OFF path is byte-identical (one cached bool).

Effect: `lance_close` r1-vs-r2 fell **20.4 % → 0.269 %**. It did **not** fix the
map: r3 still sat 16.5 % from both r1 and r2.

### (b) MITIGATED, not pinned — render-side warmup convergence

The engine comment is accurate: settle "flushes render-side streaming / chunk-LOD
admission / shadow warmup / bloom history". Raising `--settle 30 → 150` cut
`lance_close` 20.4 % → 1.6 % and `saved_204937` 23.4 % → 15.5 %. It converges but
never reaches zero, and costs run time. Use more settle frames; do not rely on it.

### (c) OPEN — residual mover-position divergence  **NOT PINNED**

After (a), with `sim_frame`, render `frame`, camera pose, seed, `scenarioTime` and
per-frame dt all **verified identical across runs**, mover positions still differ
(`saved_204937` 12.4–15.5 %).

Explicitly **refuted** by measurement, so nobody re-runs these:
- *Sim/render frame skew* — hypothesised that a variable number of sim steps ran
  before the render-frame trigger. **Refuted:** the new `sim_frame` sidecar field
  reads `119` in every run at every bookmark, and render `frame` matches too.
- *Unconverged streaming/LOD* — **refuted** as the dominant term: settle 150 does
  not converge it.
- *Animated lights / cloud shadows / weather* — refuted: pinned by the fixed clock
  and the sim-freeze; and the drift is mover-shaped, not global.

Untested candidates: address/iteration-order dependence (ASLR) and job-system
scheduling. Both are consistent with the observed **bimodality** (r1 ≈ r2, r3 in a
different discrete state) and with intermittency.

---

## 3. Two traps that hid this

1. **`deterministic: true` does not mean reproducible.** It is literally
   `SmokeMode::fixedTimestepEnabled()` — "the gate was set". Frames differing by
   27 % carry it. `docs/visual-verify.md` previously claimed the engine "stamps
   each sidecar's `deterministic` flag honestly"; that claim is now corrected.
2. **`[SMOKE_CLOCK_PROBE]` agreeing across runs proves nothing.** It is indexed by
   *sim* frame (`g_fixedSimFrame`) and prints `scenarioTime`, which is a pure
   function of that index — it matches **tautologically**. The capture triggers on
   a *different* counter (`g_mc2FrameCounter`, render frames). Fixed by recording
   `sim_frame` next to `frame` in the sidecar (`3b0f22d5`) so the two counters can
   actually be compared. (That instrument is what refuted hypothesis (c)-skew.)

---

## 4. Deliverables

| item | where |
|---|---|
| `frameLength` fixed-timestep pin | `code/mechcmd2.cpp` (`5737632b`) |
| `sim_frame` sidecar field | `GameOS/gameos/gos_visual_capture.cpp` (`3b0f22d5`) |
| Ratchet check | `scripts/check-capture-determinism.py` |
| Wired into contract suite as `capture_determinism` | `scripts/check-contracts.sh` |
| Noise floor + lane guidance | `docs/visual-verify.md` §"Capture nondeterminism" |
| Gate honesty note | `docs/tier1_env_vars.md` |
| Raw evidence | `scratch/capdet1/` (`*_noise_report.json`, `diffcap.py`) |

The ratchet defaults to `--max-pct 0.0` because the capture path *claims*
byte-stability; any tolerance is a deliberate documented concession. In the
contract suite it grades the newest set under `scratch/capdet_ratchet/` or
`tests/visual/captures/` and SKIPs when none exists, so it never launches the
game from a contract run. It currently **passes** on a real pre-existing clean
mc2_01 set (not vacuously) and **fails** on every mc2_10 set produced this lane —
which is the correct verdict.

---

## 5. Guidance published for lanes that cannot get determinism

In `docs/visual-verify.md`, in descending preference:
1. **Numeric diag receipt over whole-frame delta** — immune to render
   nondeterminism. This is how CONE-SPILL-GRADE-1 reached a verdict at all.
2. **In-process A/B without relaunching** — one process state, no launch-to-launch
   divergence.
3. **Publish a noise floor first; claim only effects above it.** An unqualified
   "X % of pixels changed" is not evidence.
4. **Per-region masks excluding movers** — terrain/static-prop regions on mc2_10
   are byte-stable while vehicle regions are not.
5. **Pick a clean pose or map** — grading a terrain/lighting change on a pose full
   of moving vehicles is choosing the worst available instrument.

---

## 6. What I could NOT prove (adversarial self-review)

- **I did not fix mc2_10.** The map is still nondeterministic. The `frameLength`
  pin is a real, source-verified defect fix with a measured effect, but it is not
  sufficient, and I did not identify the residual mechanism.
- **The ratchet is opt-in in practice.** In the contract suite it SKIPs when no
  designated set exists, so it will not, by itself, catch a lane that never
  produces one. It catches instrument breakage *for lanes that run it*. Making it
  mandatory means running the game in the contract suite — deliberately not done.
- **The `frameLength` change alters sim evolution under the gate.** `saved_212019`
  changed sha (`787639e5…` → `4edfedcc…`) after the fix. It is still
  *self-consistent* across runs. I first wrote this up as a golden-staleness
  hazard; **on checking, that was wrong** — all 16 sets in
  `tests/visual/golden-sets.json` (8 blessed, 5 candidate, 3 retired) record
  `MC2_SMOKE_FIXED_TIMESTEP: "<unset>"` in their `env_delta`, so a gated change
  cannot invalidate any of them. No golden re-bless is needed.
  **Separate finding, not mine to fix:** that also means *every blessed golden was
  captured on the nondeterministic path* (fixed clock off), and several carry
  `git.dirty: true` — e.g. `baselineA-head-mc2_01` at `1f0419ac-dirty`. Those
  baselines are unattributable and were blessed without the determinism gate.
- **"Movers" is an attribution, not a controlled result.** It rests on (i) direct
  visual inspection of the >8/255 bounding-box crop from both runs, which shows
  vehicles at different positions, and (ii) the correlation that mover-free poses
  are byte-identical while mover-containing poses drift. I did **not** run a
  mover-disable A/B (e.g. via the FORCE-ALL-VISIBLE / visibility gates) to prove
  it causally. That is the cheapest next experiment for whoever picks this up.
- **Only 2 maps measured** (mc2_10, mc2_01), not the 4 the Standing Capture Rule
  asks for. mc2_24's user bookmark set contains one distinct pose plus a
  duplicate, so a 3-location noise floor is not obtainable there from existing
  fixtures. mc2_03 not measured.
- **Sample size is small.** 3 launches per arm; the `saved_212140` intermittency
  was caught only because a 6th run happened to exist. The true tail is unmeasured
  and the quoted floors are lower bounds, not bounds.
- **`--run` mode of the ratchet is only lightly exercised.** I validated
  `--set-dir` (FAIL=1, PASS=0, missing=SKIP=0) and the default scan directly; the
  `--run` path shares `run_visual_capture.py` invocation code that I used
  extensively by hand, but I did not execute `--run` end-to-end.
- **I did not test the OFF path for behaviour change.** The `frameLength` edit is
  inside `if (SmokeMode::fixedTimestepEnabled())` with the original statement
  moved into an `else`, so the unset path is unchanged by inspection, and tier1
  contracts pass — but I did not run a gameplay A/B to confirm no OFF-path drift.
