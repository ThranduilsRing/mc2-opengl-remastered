# CAPTURE-ENV-PIN-1 — STATUS

Branch `claude/nifty-mendeleev`. Commits `1bbb1f9c` (harness pin + ratchet),
`8f4876ed` (bake order-vs-time probe). Lane deploy `0.4d-rc1`.
Verification: **tier1 5/5, `DEPLOY_FINGERPRINT OK` exe sha=67b25f4791ad**;
contracts **74/74**.

## VERDICT IN ONE LINE

Task A shipped and is demonstrated firing. Task B **settled the order-vs-time
question and refuted BOTH candidate explanations**, including the one the brief
favoured — so **no fix shipped for the prop-lighting defect**, because neither
proposed cause survived contact with the discriminating probe.

---

## TASK A — capture harnesses now pin the MC2_* environment

### The defect

`run_visual_capture.py` launched the engine with `os.environ.copy()`. Every
stray `MC2_*` in the operator's shell rode into the captured frame, and the
manifest recorded only a hardcoded 5-name subset that could not have caught it.

`run_smoke.py` was already immune two ways: it builds its child env from scratch
behind an explicit allowlist, **and** it hard-refuses to start on an
unallowlisted `MC2_*` (`[ENV-DROP] FATAL`). I initially mis-graded that refusal
as absent — it exists; it fired on me in this very lane (see Receipts).

### What now happens

`scripts/mc2_env_pin.py` — `build_pinned_env()`:

- Removes **every** ambient `MC2_*` from the child environment. Non-`MC2_*`
  (PATH, TEMP, SystemRoot) pass through untouched.
- Applies what the harness itself must set, then explicit declarations.
- Returns a JSON record: `effective` (the full `MC2_*` set the engine saw),
  `harness_set`, `declared`, `ambient_scrubbed`, `clean_shell`.

Deviation requires an explicit opt-in, never an inherited one:

| channel | for | shape |
|---|---|---|
| `--env-declare KEY=VALUE` | a human at a shell | repeatable |
| `MC2_CAPTURE_ENV_DECLARE=NAME1,NAME2` | a programmatic caller | comma list |

A short `TRANSPORT_PASSTHROUGH` set (diagnostic-trace path, diag tags,
debug-state dump, bookmark dir) survives the scrub — output routing only, never
engine behaviour. It is the one hole in the rule and it is documented as such.

**Sidecars now record the full effective `MC2_*` set** plus what was scrubbed,
via `identity_block(env=...)` rather than the old 5-name allowlist.

### The catch that nearly inverted the fix

`pipeline_visual_gate.py` and `run_golden_parity.py` drive A/B comparisons by
setting gate vars in a child env and invoking `run_visual_capture.py`. A naive
scrub would have **dropped the knob under test and silently collapsed both arms
into the same run** — the identical silent-inertness class this lane exists to
kill, merely inverted. Both callers now declare their gate names through the
channel. Proven by unit assertion (Receipts B).

### Ratchet

`scripts/check-capture-env-declared.py`, registered in `check-contracts.sh` as
`capture_env_declared` + `capture_env_ratchet`. Two independent layers:

- **CODE** — a capture harness must not build a launch env with
  `os.environ.copy()` and must import `mc2_env_pin`. Stops a *new* harness
  reintroducing silent inheritance.
- **ARTIFACT** — no capture manifest may record a non-empty `ambient_scrubbed`,
  and `policy` must be `pinned-shipped`. Catches a capture taken **today** from
  a contaminated shell.

`--self-test` injects a contaminated manifest, requires rejection, then requires
a clean manifest to pass (negative control, so the ratchet cannot be a constant
failure). Both layers demonstrated firing — see Receipts.

A non-empty `ambient_scrubbed` does **not** mean the capture is wrong: the pin
scrubbed the contaminant, so the frame is shipped-config. It means the
**operator's shell** is dirty and will corrupt any harness not yet pinned.

---

## WHICH PRIOR EVIDENCE IS NOW SUSPECT

**Anything captured via `run_visual_capture.py` on this machine while those
three vars were live — which is every already-open shell tonight, and therefore
most lanes' capture evidence.** Specifically:

- `MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE=1` is **default-OFF** in the shipped
  game. Set, it seeds every static prop with a flat `0.3,0.3,0.3` ambient
  fallback: **0** never-baked light slots with it, **35** without it. Any
  prop-lighting capture taken through it measured a configuration the user never
  runs. This is the one with teeth.
- `MC2_GPU_CULL_STATIC_FROZEN_RECORDS=1`, `MC2_STATIC_POP_SPLIT=1` — same
  inheritance, effect not separately quantified here.

Grading rule for old artifacts: a capture manifest with **no `mc2_env` block**
predates this lane and its environment is **unknowable after the fact**. It is
not "probably fine" — it is ungraded. `--strict-missing` lists them. Anything
load-bearing should be recaptured; captures via `run_smoke.py` are unaffected
(it dropped the vars, and would have refused outright).

Note the honest limit: the vars were **dropped** by `run_smoke.py`, so
smoke-derived evidence is clean. It is specifically the **visual-capture** lane
of evidence that is suspect.

---

## TASK B — order vs time: BOTH hypotheses refuted

The brief was explicit that the probe must log bake **order** *and* bake **time
relative to the `LIGHTBAKE-REBAKE-ON-READY-1` invalidation**, or it would
confirm a hypothesis it could not distinguish. It logs both, per bake:

`[BAKEORD v1] ri= ord= frame= postInvalidate= sunActive= nl= amb= sun=`

3033 bakes / 2574 recipes on mc2_10, probe confirmed engaged (`MC2_DIAG_VERBOSE=1`
is required — without it `MC2_DIAGF` emits nothing).

### Naive reading — both factors look necessary

| | BRIGHT | DIM |
|---|---|---|
| `sunActive=0` | **0** | 1676 |
| `sunActive=1` | 250 | 531 |
| `postInvalidate=0` | 250 | 1697 |
| `postInvalidate=1` | **0** | 510 |

Both look necessary, neither sufficient. Stopping here would have let either
hypothesis be declared "strongly supported". That is the trap the brief named.

### The discriminating cut

Hold **both** the sun state and the light count fixed and vary only the frame:

| frame | sunActive | nl | n | BRIGHT | DIM |
|---|---|---|---|---|---|
| 0 | 1 | 2 | 250 | **250** | 0 |
| 1 | 1 | 2 | 145 | **0** | 145 |
| 2 | 1 | 2 | 43 | **0** | 43 |

**Identical sun-active state, identical 2-light row, opposite outcome.**

- **ORDER (tree sun-off window) — REFUTED as the cause of the split.** The
  bright/dim boundary occurs *with the sun active on both sides*. `sunActive=0`
  never yields BRIGHT (0/2243), so the tree window is *sufficient for DIM*, but
  it is demonstrably **not** what separates the 250 bright bakes from the 145
  dim ones. Structural corroboration: the sun-off window
  (`bdactor.cpp:6854`→`6970`) contains **no early return**, so in a
  single-threaded update loop a non-tree bake cannot fall inside a tree's window
  at all.
- **TIME as stated — REFUTED.** The invalidation fires at **frame 9**
  (`[LIGHTBAKE v1] event=rebake_on_ready frame=9`). The split is at **frame 0 →
  frame 1**, six frames earlier. `postInvalidate` correlates only because it is
  a coarse proxy for "later".

### What is actually going on (and it inverts the prior reading)

Of the 250 recipes that baked BRIGHT at frame 0, only **78 stay BRIGHT**. The
other **172 are re-baked DIM** — at frames 120/152/184/216, and **171 of the 172
after the invalidation**, all with `sunActive=1`.

So `LIGHTBAKE-REBAKE-ON-READY-1` is implicated in the **opposite** direction
from the brief: it is not the origin of the split, it is the mechanism that
converts stale-bright rows to dim — i.e. it is doing its stated job, re-baking
to the mission's settled lighting.

Which means the polarity assumed by PROP-RECIPE-BAKE-1 is likely backwards:
**`0.2471` looks like the mission's real settled ambient, and `0.7529` the
stale frame-0 boot ambient.** The 78 still-bright recipes are then the anomaly
(frozen boot lighting), not the dim majority.

**I am not asserting that as proven.** It is what these numbers imply, and it is
a different investigation from the one I was given. What *is* proven is the
order-vs-time verdict above, which is what was asked.

### No fix shipped

The brief said "if proven, fix it". Neither proposed cause was proven; both were
refuted. Shipping either suggested fix (force the terrain colour to the sun
slot; suppress the tree window's global effect) would have targeted a mechanism
the data excludes. On a default launch **the user still sees the defect.**

Next lane should start from the frame-0-vs-frame-1 ambient transition and the
inverted polarity, not from bake order.

---

## RECEIPTS

**A. Ratchet fires (artifact layer)** — `--self-test`:
```
[self-test] ratchet fired as required (1):
  ...captured from a CONTAMINATED SHELL -- undeclared ambient MC2_* present at
     capture time: MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE=1...
[self-test] clean manifest passes (no false positive)
[self-test] PASS
```

**B. Ratchet fires (code layer)** — `os.environ.copy()` reintroduced into
`run_visual_capture.py`, ratchet run, defect reverted:
```
--- with defect reintroduced ---   FAIL  rc=1
--- restored ---                   OK    rc=0
```

**C. The pin works live** — real capture run from this contaminated shell:
```
[env-pin] WARNING: 3 ambient MC2_* variable(s) were present in this shell and
have been SCRUBBED so the capture runs the shipped configuration:
MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE=1, MC2_GPU_CULL_STATIC_FROZEN_RECORDS=1,
MC2_STATIC_POP_SPLIT=1.
[viscap] env-declare: MC2_DIAG_VERBOSE=1, MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE=1
```
The contaminants were dropped; the two deliberately declared vars survived.

**D. `run_smoke.py`'s refusal is real** — it fired on my own tier1 attempt:
```
[runner] [ENV-DROP] FATAL: 3 MC2_* var(s) set in the environment cannot reach
mc2.exe: MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE, ...
[runner] Refusing to run -- a PASS here would grade a configuration that was
never applied.
```

**E. Two-run measurement floor** (required before claiming any effect):

| run | bakes | recipes | frame0/sun1 BRIGHT | frame1/sun1 BRIGHT / DIM | final BRIGHT |
|---|---|---|---|---|---|
| run1 | 3033 | 2574 | 250 / 250 | 0 / 145 | 78 |
| run2 | 3031 | 2574 | 250 / 250 | 0 / 145 | 78 |

All five capture hashes byte-identical across runs. The 2-bake delta is late
idle re-bakes and does not touch the discriminator. PNGs 24.9 MB each
(non-zero — the zero-byte-PNG disk failure did not recur).

**F. Verification** — tier1 `PASS (5/5)`, `[DEPLOY_FINGERPRINT] OK: exe
sha=67b25f4791ad matches worktree HEAD`; contracts 74/74.

---

## HYGIENE / HANDOFF

- I **scooped three files** from the water lane's staged index on my first
  commit (`gameos_graphics.cpp`, `gos_terrain_water_stream.h`,
  `gos_terrain_water_mdi.frag` — the shared index picked them up during
  `git add`). Caught immediately, reset out, worktree content verified unchanged
  by md5 before and after. The final commit `1bbb1f9c` contains **only** my six
  files. Sibling lost the staged bit, not the content. Lesson: on this repo,
  `git add <paths>` is not sufficient isolation when siblings stage concurrently
  — diff the commit before trusting it.
- The probe reuses the already-registered
  `MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE` gate. No new env var, no allowlist
  edit, no contention with lanes claiming `run_smoke.py` / `docs/tier1_env_vars.md`.
- HEAD moved twice under me mid-lane (siblings committing to the same branch).
  The deployed exe is `67b25f4791ad`, which contains both my commits.
- Not done, deliberately: `capture_baseline.py`, `camera_sweep_smoke.py`,
  `mech_shot.py`, `quick_shot.py` and others still use `os.environ.copy()`.
  They are not in `PINNED_HARNESSES` yet, so the ratchet does not cover them.
  Extending the pin to those is a clean follow-up slice; add each to
  `PINNED_HARNESSES` as it is converted.
