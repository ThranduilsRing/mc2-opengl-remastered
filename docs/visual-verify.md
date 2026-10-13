<!-- doc-status: current | reviewed:2026-10-11 -->
# Visual-verify pipeline (`tools/visual_verify/`)

One command set that automates what a manual verification fleet does by hand:
**capture** every saved camera pose from a release build, **crop** the known
artifact regions at full resolution, **diff** them against a blessed golden, and
emit a **verdict report + contact sheets** an agent or human can Read for
judgment.

It replaces the ad-hoc loop of "launch the game, replay bookmarks, hand-crop the
horizon / shore / shadow / light-cluster strips, grade each against the complaint
checklist." The artifact regions the fleet actually used are committed under
`tools/visual_verify/regions/` (seeded from the 2026-07 11-agent session), so the
grade is reproducible instead of re-derived every session.

Python-only, stdlib-only (reuses the shared PNG codec in `tools/visual/png_io.py`).
No C++ build. Builds directly on the hardened engine launcher
`scripts/run_visual_capture.py` (own-PID reaping, cursor-parking, deterministic
fixed-timestep clock, MC2_LOG live stdout).

---

## The one command a future "verify this build" invocation looks like

```powershell
# 1. capture every pose from the build under test
py -3 tools/visual_verify/capture.py `
    --install "A:/Games/mc2-opengl/releases/mc2-win64-<VER>" `
    --bookmarks all --frame 431 `
    --out <scratch>/vv/run_<VER>

# 2. grade the capture against a blessed golden
py -3 tools/visual_verify/verify.py `
    --run <scratch>/vv/run_<VER> `
    --golden tests/visual/goldens/v0.5.1-baseline
```

`verify.py` writes `report.md` (verdict table), `report.json` (machine report),
`crops/<bookmark>/<region>.png` (full-res region crops), and
`contact_sheets/<bookmark>/<region>__candidate.png` + `__golden.png` (side-by-side
for eyeball judgment). Exit code: `0` all PASS/NEW, `1` a region FAILed (drifted
past its threshold), `2` SUSPECT (missing capture / decode error).

If the build has no golden yet, drop `--golden` (crops only) or seed one with
`--baseline --golden <dir>`.

---

## The four tools

| Tool | Role |
|---|---|
| `capture.py` | Sequential bookmark sweep. Discovers `<install>/saved_bookmarks/*.json`, replays each pose via the hardened launcher (one game instance at a time), writes per-pose dirs + `capture_run.json`. Lane-safe: refuses to start if an mc2.exe from the *target install* is already running; reaps only its own children. |
| `regions.py` | Region-manifest loader + crop extractor. `coverage` subcommand prints the manifest table; `crop` extracts one region (debug). |
| `verify.py` | Per pose: emit full-res region crops; if a golden exists, per-region diff (`mean_delta`, `changed_pct`, `struct_delta`) graded against manifest thresholds → PASS/FAIL/NEW/SUSPECT. Writes `report.md`, `report.json`, contact sheets. |
| `bless.py` | Copy a capture run's region crops into `tests/visual/goldens/<name>/` + write `golden.json` (identity + per-crop sha256). The blessed dir is what a human commits. |

### Region manifests

`tools/visual_verify/regions/<bookmark>.json` — one per pose. Each names the
artifact rectangles (`rect` in normalized fractions or reference-pixel coords), a
plain-English `check`, a `severity`, and per-region diff `thresholds` (a
candidate-vs-golden **drift budget** — captures are deterministic so a true match
is ~0; the budget absorbs dither and trips on a real geometry/lighting change).
See `regions/_README.md` for the schema. Coverage:

```
py -3 tools/visual_verify/regions.py coverage
```

---

## THE STANDING CAPTURE RULE (user rule — mandatory for every gate evaluation)

> **User, 2026-07-25:** *"this is too zoomed in for you to even see the travesties
> it inflicted elsewhere."*

Tight crops **hide collateral damage**. A crop can only ever show you the thing you
already suspected. Every gate/fix evaluation put in front of the user MUST grade on
all four of the following. This is the *grading* protocol; it is not optional and it
is not satisfied by a crop pair.

1. **The VERDICT shot is a FULL-FRAME WIDE VIEW at normal gameplay altitude** —
   one per map. The wide shot is what decides pass/fail. **Crops are detail only**
   and may never stand in for it.
2. **≥3 distinct locations per map**, and at least one of them must be a
   **CONTROL AREA containing none of the affected feature** — that is what catches
   collateral damage the fix was never supposed to touch.
3. **Day AND night.** A night-only or day-only grade is not a grade.
4. **Across `mc2_01` + `mc2_03` + `mc2_10` + `mc2_24`** — not one map.

Applies to all lanes, permanently. If you cannot meet it, say so explicitly and
label the result provisional; do not quietly grade on less and report "verified".

**Anti-patterns that have already burned us:**
- Grading on four frozen bookmark poses, one map, night only (GHOST-CONE-VERIFY-4) —
  and the defect in question was reachable in **live play only**, so those poses
  could never have shown it.
- Grading from a folder *named* for a defect (`ghost_wedge_over_road`) whose image
  was actually clean. **Grade the pixels, not the filename.**
- Grading from a `-dirty` / unattributable build. See "Evidence must be
  attributable" below.

### This does NOT contradict crop-only goldens — they are different jobs

| | Automated regression goldens | Human verdict grading |
|---|---|---|
| Purpose | detect drift vs a blessed baseline | decide whether a fix is good |
| Unit | **per-region crops** (cheap, exact, in-repo) | **full-frame wide shots** + supporting crops |
| Audience | `verify.py` | the user |
| Rule | the section below | the Standing Capture Rule above |

Keep crops for goldens. **Never** hand the user a crop as the verdict.

---

## Goldens: crops, not frames

> Scope: this section governs **automated regression goldens only**. For anything
> the user is asked to grade, the Standing Capture Rule above applies and the
> verdict shot is a full frame.

Goldens are stored as **per-region crop PNGs** under
`tests/visual/goldens/<name>/<bookmark>/<region>.png`, not full 4K frames. The
committed regions are small strips (a whole bookmark set is a few hundred KB), so
they live in-repo cheaply and diff exactly. Each set carries a `golden.json`
identity block (install, exe, frame, per-crop sha256, source capture manifest).

**Resolution.** Captures are pinned to 1920x1080 (`scripts/smoke_lib/render_resolution.py`). The golden set `tests/visual/goldens/reset-50331d5` was
blessed at 3840x2160, so `check-capture-resolution.py` refuses every capture against it until the owner re-blesses at the pinned size (capture three
runs with `tools/visual_verify/capture.py`, then `bless.py` below). The render options (`render_options` in `capture_run.json`) are an input of every
capture too; `capture_ab.py compare` refuses sets whose options differ unless `--vary Key,Key` names them.

If you ever need **frame-level** goldens (full 4K), keep them in an external dir
(they are ~5-10 MB each — do not commit) and record the path in `golden.json`'s
`frames_dir`. For **regression goldens** the default path is crop-only. (This is
*not* a licence to grade a fix on crops — see the Standing Capture Rule above.)

Bless is deliberate and reviewable, and takes **three** independent capture runs
(THREE-RUN-ALL-PAIRS-1 — two cannot distinguish "reproducible" from "happened to
agree once"):

```powershell
py -3 tools/visual_verify/bless.py --run <cap1> --run <cap2> --run <cap3> `
    --to tests/visual/goldens/<name>
# then: git add tests/visual/goldens/<name> && commit
```

**Do not hand-add provenance.** An earlier version of this page said "commit (set
`blessed_commit`)". That field no longer exists, and what it held was the
*blesser's worktree HEAD* — the lane that happened to run bless.py, which is
routinely neither the lane that built the exe nor the commit that rendered the
crops, and blessing can happen days after capture. A golden now carries:

| field | means |
|---|---|
| `identity.binary_provenance.commit` | the commit **compiled into the binary that rendered these crops**, read out of its own `.rdata` build stamp |
| `identity.exe.sha256` | that binary by **content**. A path plus an mtime is not an identity |
| `source_worktree_head_at_bless` | who ran bless.py. Carries `describes_binary: false` **in the JSON** so it cannot be misread as provenance |

All three are tool-emitted (GOLDEN-PROVENANCE-TRUTH-1). `bless.py` **refuses** to
mint a golden whose source captures cannot name their binary, whose runs measured
*different* binaries, or whose runs point at the same PNG.
`verify.py` **refuses to grade** against a golden that cannot name its binary —
drop `--golden` if you just want crops without a verdict. There is no override
flag on either. `scripts/check-capture-provenance.py` scans
`tests/visual/goldens/**/golden.json` and fails any golden that cannot.

---

## Lane discipline (game runs)

Inherited from `scripts/run_visual_capture.py` and enforced by `capture.py`:

- **No foreign game clobber** — `capture.py` aborts if an mc2.exe launched from
  the *target install* is already running (pass `--allow-running` to override).
- **Own PIDs only** — the launcher reaps only the children it spawned; other
  installs' mc2.exe are untouched.
- **Sequential** — one game instance at a time; the next launch waits for the
  previous child to exit. Never parallel.
- **`MC2_LOG=1`** — live engine stdout to a per-pose `<mission>_capture.log`.
- **Pre-created capture dirs** — every output dir is created before launch.
- **Determinism** — `MC2_SMOKE_MODE=1`, fixed seed, `MC2_SMOKE_FIXED_TIMESTEP=1`,
  cursor parked center (kills RTS edge-scroll drift). The engine stamps each
  sidecar's `deterministic` flag — but see the warning below: **that flag means
  "the fixed clock was engaged", NOT "this frame is reproducible."** On mc2_10 it
  reads `true` on frames that differ by 27% of pixels between launches.

## Relationship to the existing tooling

- `scripts/run_visual_capture.py` — the engine launcher this pipeline drives
  (Baseline-A byte-stability, golden-set materialization). `capture.py` reuses its
  `run_one` in-process.
- `tools/visual/visual_diff.py` — whole-frame layered diff (identity/hash/pixel)
  + triptych HTML. Complementary: use it for full-frame byte-regression; use
  `verify.py` for **region-level, checklist-aligned** grading.
- `tools/visual/png_io.py` — the shared stdlib PNG codec both use.

---

## Capture nondeterminism — the measured noise floor, and what to do about it

**CAPTURE-DETERMINISM-1, 2026-07-26.** Repeated launches at a fixed pose with
byte-identical environment do **not** always produce identical frames. Measure
before you grade.

### Measured noise floor (3 launches, same arm, same env, 3840x2160)

`pct_any` = share of pixels differing at all; `pct_gt8` = differing by >8/255.

| map | pose | pct_any (worst pair) | pct_gt8 |
|---|---|---|---|
| mc2_10 | `saved_204937` | **27.3 %** | 17.4 % |
| mc2_10 | `lance_close` | **23.2 %** | 8.4 % |
| mc2_10 | `saved_204753` | 9.1 % | 6.3 % |
| mc2_10 | `saved_204523` | 0.008 % | 0.008 % |
| mc2_10 | `saved_212019` | 0.000 % | 0.000 % |
| mc2_10 | `saved_212140` | 0.000 % * | 0.000 % |
| mc2_01 | all 5 poses | **≤ 0.004 %** | ≤ 0.003 % |

\* `saved_212140` was byte-identical across five consecutive launches and then
drifted on the sixth. **The noise is intermittent.** A pose that passed a 2- or
3-run check is not proven stable; repeat counts understate the floor.

**It is map- and content-specific, not global.** mc2_01 is effectively clean.
On mc2_10 the drift is spatially concentrated on **movers** (vehicles/mechs) —
poses framing only terrain and static props are byte-identical, poses containing
vehicles are the ones that swing 10–27 %. It is not "animated night light pools"
or cloud shadows: those are pinned by the fixed clock and the sweep's sim-freeze.

### What is pinned, and what is not

| source | status |
|---|---|
| `scenarioTime` (sim clock) | **pinned** — S9D, `MC2_SMOKE_FIXED_TIMESTEP` |
| shader/render clocks | **pinned** — S9E |
| camera pose | **pinned** — bookmark re-applied each settle frame + `freezeScriptedCameraForCapture` |
| `frameLength` (drives ALL mover/object/GOSFX motion integration) | **pinned as of CAPTURE-DETERMINISM-1** — was still `1.0/frameRate` from the *real* measured framerate under the fixed-timestep gate |
| render-side warmup (streaming, chunk-LOD admission, shadow, bloom history) | **converges with settle frames** — raising `--settle 30 → 150` cut `lance_close` from 20.4 % to 1.6 % |
| residual mover-position divergence | **NOT PINNED — open** |

The residual is real and unexplained. With `sim_frame`, render `frame`, pose,
seed, `scenarioTime` and per-frame dt all now *verified identical* across runs,
mover positions still differ. Two candidate mechanisms remain untested:
address/iteration-order dependence (ASLR) and job-system scheduling. Do not
assume mc2_10 is reproducible.

### Two traps that made this invisible for so long

1. **`deterministic: true` in the sidecar does not mean reproducible.** It is
   `SmokeMode::fixedTimestepEnabled()` — i.e. "the gate was set". Frames that
   differ by 27 % carry it.
2. **`[SMOKE_CLOCK_PROBE]` agreeing across runs proves nothing.** It is indexed
   by *sim* frame (`g_fixedSimFrame`) and prints `scenarioTime` — which is a
   pure function of that index. It matches tautologically. The capture triggers
   on a *different* counter (`g_mc2FrameCounter`, render frames). The sidecar now
   records `sim_frame` alongside `frame` so the two can actually be compared.

### What a lane should do when it cannot get determinism

In descending order of preference:

1. **Prefer a numeric diag receipt over a whole-frame delta.** A counter printed
   by the code under test (`MC2_LIGHT_CONE_DIAG`-style) is immune to render
   nondeterminism. CONE-SPILL-GRADE-1 could only reach a verdict this way: its
   pixel A/B was swamped, its diag counted "7 of 9 pools deleted" exactly.
2. **A/B in-process, without relaunching.** Two poses in one sweep share one
   process state, so they do not carry launch-to-launch divergence.
3. **Publish a noise floor and only claim effects above it.** Run the same arm
   ≥3 times *first*, quote the worst pair, and refuse to claim any effect
   smaller. An unqualified "X % of pixels changed" is not evidence.
4. **Mask the animated features out.** Grade per-region with movers excluded —
   on mc2_10 the terrain/static-prop regions are byte-stable while the vehicle
   regions are not.
5. **Pick a clean pose or a clean map.** mc2_01, and the mover-free mc2_10 poses,
   are reproducible. Grading a terrain/lighting change on a pose full of moving
   vehicles is choosing the worst available instrument.

### The ratchet

`scripts/check-capture-determinism.py`, wired into `scripts/check-contracts.sh`
as **`capture_determinism`**. It compares repeated captures of the same arm and
fails when they differ beyond `--max-pct` (default **0.0**, because the capture
path claims byte-stability).

```powershell
# grade an existing multi-run set (r1/, r2/, ... from --runs N)
py -3 scripts/check-capture-determinism.py --set-dir <dir>

# perform the captures and grade them
py -3 scripts/check-capture-determinism.py --run `
    --exe "A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe" `
    --mission mc2_10 --bookmarks <bookmarks.json> --runs 3
```

In the contract suite it grades the newest set under `scratch/capdet_ratchet/`
or `tests/visual/captures/` and **SKIPs** when none exists — it never launches
the game from a contract run.

**Before trusting any capture-based verdict, run this against your own capture
set.** If it fails, your instrument is broken and the verdict is not evidence.
