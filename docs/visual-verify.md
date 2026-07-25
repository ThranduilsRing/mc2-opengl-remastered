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

If you ever need **frame-level** goldens (full 4K), keep them in an external dir
(they are ~5-10 MB each — do not commit) and record the path in `golden.json`'s
`frames_dir`. For **regression goldens** the default path is crop-only. (This is
*not* a licence to grade a fix on crops — see the Standing Capture Rule above.)

Bless is deliberate and reviewable:

```powershell
py -3 tools/visual_verify/bless.py --run <capture-dir> --to tests/visual/goldens/<name>
# then: git add tests/visual/goldens/<name> && commit (set blessed_commit)
```

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
  sidecar's `deterministic` flag honestly.

## Relationship to the existing tooling

- `scripts/run_visual_capture.py` — the engine launcher this pipeline drives
  (Baseline-A byte-stability, golden-set materialization). `capture.py` reuses its
  `run_one` in-process.
- `tools/visual/visual_diff.py` — whole-frame layered diff (identity/hash/pixel)
  + triptych HTML. Complementary: use it for full-frame byte-regression; use
  `verify.py` for **region-level, checklist-aligned** grading.
- `tools/visual/png_io.py` — the shared stdlib PNG codec both use.
