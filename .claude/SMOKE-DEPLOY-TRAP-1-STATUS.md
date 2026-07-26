# SMOKE-DEPLOY-TRAP-1 — status

**Branch** `claude/nifty-mendeleev` · **HEAD** `285d7338` (+ this doc)
**Commits** `4a003544`, `285d7338` · **Deploy lane** `0.4` · never pushed

---

## META REVIEW — two of the brief's four premises were already fixed

Verified from source, not from status docs.

| Brief item | Claim | Reality |
|---|---|---|
| 1 | "Make a fingerprint mismatch fatal" | **Already done.** `DEPLOY-FINGERPRINT-DEFAULT-ON-1` (`e01e8ac1`, 2026-07-02, ancestor of HEAD) set `MC2_SMOKE_REQUIRE_FINGERPRINT` default `"1"` — opt-**OUT**. |
| 2 | "Unknown lane names like `0.3` must be a hard error, not a fall-through" | **Already done.** `deploy_lease.resolve_folder()` raises `LeaseError("unknown deploy name …")` before any fall-through. |
| 3 | "Deploy invocation documented wrong in three docs" | **Already fixed** by `5c33fb48`; all live docs now say `--build-dir build64/RelWithDebInfo`. |
| 4 | "Add a contract check" | Genuinely absent. Built. |
| 5 | "Two harnesses FAIL in the runner but exit 0 standalone" | Reproduced, but **not** a runner artifact — see below. |

The brief's framing ("the last big one still active") was three weeks stale. What
follows is what was *actually* still broken.

---

## What now fails that did not before

### 1. Three silent bypasses of the fingerprint gate (`4a003544`)

The gate was default-ON but trivially and silently defeatable:

- **Broken import.** `from scripts.smoke_lib import fingerprint` failing set
  `_fingerprint = None` and skipped the entire check with no message. A 3-line
  edit disabled the guard and the run still printed PASS. → now aborts, exit 2.
- **Raising check.** An exception inside the per-mission check printed
  `check skipped` and left `_fp_hard_fail` untouched. → now counts as a
  mismatch. *An exe that cannot be identified is a failed verification, not a
  skipped one.*
- **No ratchet.** Nothing stopped the default being flipped back to opt-in.

### 2. The comments lied about the code (`4a003544`)

`run_smoke.py` and `fingerprint.py` still said *"advisory by default"* and
*"MC2_SMOKE_REQUIRE_FINGERPRINT=1 = hard fail"* three weeks after the default
flipped. A lane reading them concludes its green run was ungated, or that it
must opt in before its result means anything. Corrected. The genuinely-advisory
neighbours (`_REQUIRE_FRESH`, `_REQUIRE_VISUAL`, `_REQUIRE_DIAG_STATE`) were
left alone — see the false-positive note below.

### 3. ★ The report artifact still said PASS on a wrong-binary run (`285d7338`)

**Found by adversarial self-review of my own first commit.** tier1 against a
deliberately stale exe exited 1 with a loud HARD FAIL — and still wrote:

```
# Smoke run 2026-07-26T09-14-33  tier=tier1  profile=stock  result=PASS (5/5 passed)
```

`report.render_markdown/render_json` were called ~15 lines **before** `passed`
was computed and before the fingerprint verdict was applied. So `report.md`,
`report.json`, and the `get_latest_smoke_report()` MCP tool all certified a run
on the wrong binary. **The exit code was honest; the artifact everyone actually
quotes was not** — and quoting the artifact is exactly what the eight trapped
lanes did. This is the literal defect in the mission statement, and it survived
the fix that was supposed to have killed it.

Now: run-level failures flow into both renderers, `report.json` gains an
explicit top-level `overall_result` + `run_failures`, and the markdown carries a
**RUN INVALID** banner.

### 4. Harness false negatives — NOT a runner artifact (`4a003544`)

BUILDING-FACE-POSE-1 recorded that `shader_contract_harness` and
`ibl_registry_contract_harness` "exit 0 when executed directly, so the runner
verdict is a harness-runner artifact." Half right, and the wrong half matters.

`tools/run_contract_tests.py` returned the **first** match from a hardcoded dir
list naming `build64-shader` / `build64-ibl` but **not** `build64-h-<name>`,
where the canonical builder `tools/build_contract_harnesses.py` writes. It was
grading **2026-06-22** binaries. Measured:

| harness | stale copy | exit | current copy | exit |
|---|---|---|---|---|
| `shader_contract_harness` | `build64-shader/…` (06-22) | **1** | `build64-h-shader_contract_harness/…` (07-10) | **0** |
| `ibl_registry_contract_harness` | `build64-ibl/…` (06-22) | **1** | `build64-h-ibl_registry_contract_harness/…` (07-10) | **0** |

So the runner was *honestly reporting a real failure of a stale binary* — the
same stale-artifact defect as the smoke trap, only it fails green work instead
of passing broken work. Fixed: searches `build64-h-<name>` first, prefers the
**newest** copy, and prints which binary it graded and what it ignored.
**12/16 → 16/16** (also resolves the two spurious `MISSING`).

### 5. Docs (`4a003544`)

`CLAUDE.md`'s canonical copy-paste smoke command has no `--exe`/`--deploy`, so
it auto-selects a lane — the exact command behind the eight-lane trap. It now
documents that the fingerprint gate is what makes it safe, that the
`DEPLOY_FINGERPRINT OK` line (**not** `PASS`) is the evidence to quote, and how
to pin a lane by name.

---

## The contract check, and proof it fires

`scripts/check-smoke-fingerprint-honesty.py` — 8 checks, wired into
`check-contracts.sh` as `smoke_fingerprint_honesty` + `smoke_fingerprint_ratchet`.

`--selftest` mutates a copy of the source back to each pre-fix shape and
**requires the check to report FAIL**:

```
[selftest] baseline: real tree passes all 8 checks
[selftest] FIRES  fp_report_wired  (injected: stop feeding the verdict to the report)
[selftest] FIRES  fp_default_on  (injected: flip the gate back to opt-in)
[selftest] FIRES  fp_clears_verdict  (injected: print the message but not fail)
[selftest] FIRES  fp_exc_is_failure  (injected: swallow the check exception)
[selftest] FIRES  fp_import_fatal  (injected: skip silently on broken import)
[selftest] FIRES  fp_docs_honest  (injected: reintroduce 'advisory by default')

[selftest] OK: all 6 guards fire on injected regressions
```

The two behavioural checks (`fp_behaviour`, `fp_report_honest`) drive the real
functions rather than the source text, and `fp_report_honest` additionally
asserts a **clean** run still renders PASS, so it cannot pass vacuously.

**The checker earned its keep twice while being written:**

1. On its first run it failed on a stale `advisory by default` comment in
   `run_smoke.py` that I had missed by hand.
2. The selftest reported `fp_clears_verdict` as not firing. That was a bug in
   **my injection**, not the guard: anchored on `if _fp_require and
   _fp_hard_fail:`, a lazy match ran past the second such block into
   VEG-SMOKE-FLOOR's `passed = False`, mutilating an unrelated guard. Re-anchored
   on the HARD FAIL message. A ratchet that grades its own injections catches
   this; one that only runs on good input does not.

**A false positive I caused and withdrew.** My first `fp_docs_honest` banned the
phrase "advisory by default" anywhere in the file. That flagged three *correct*
comments describing `MC2_SMOKE_REQUIRE_FRESH` / `_REQUIRE_VISUAL` /
`_REQUIRE_DIAG_STATE`, which really are advisory. A proximity window did not
help — one of them legitimately cites `DEPLOY_FINGERPRINT` as the pattern it
mirrors. Narrowed to two precise rules (no `…=1` opt-in framing; no comment
paragraph naming the knob while calling it advisory). Both stale comments this
arc removed are caught by the first rule. **A guard that fails correct
documentation trains the next lane to rewrite accurate docs to appease it.**

---

## VERIFY — tier1 run both ways

**Positive** — `--exe A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe` (deployed at HEAD):

```
RUNNER EXIT = 0
[runner] [DEPLOY_FINGERPRINT] OK: exe sha=285d7338ba00 matches worktree HEAD   ×5
# Smoke run 2026-07-26T09-21-26  tier=tier1  profile=stock  result=PASS (5/5 passed)
report.json: overall_result= PASS   run_failures= []
```

**Negative** — `--exe …/releases/mc2-win64-v0.5.1/mc2.exe` (sha `fa3a530495c3`):

```
RUNNER EXIT = 1
[runner] [DEPLOY_FINGERPRINT] MISMATCH: exe sha=fa3a530495c3 expected=285d7338ba00   ×5
# Smoke run 2026-07-26T09-24-56  tier=tier1  profile=stock  result=FAIL (5/5 passed)
> **RUN INVALID — the missions' results do not count.**
> - DEPLOY_FINGERPRINT: the smoked exe does not match this worktree HEAD …
[runner] [DEPLOY_FINGERPRINT] HARD FAIL: …
report.json: overall_result= FAIL   run_failures= ['DEPLOY_FINGERPRINT: …']
```

`result=FAIL (5/5 passed)` is the intended reading: every mission passed, and
the run is still invalid.

**Gates:** contracts **72/72** (70 + my 2) · harnesses **16/16** ·
`--selftest` 6/6 fire · deploy `0.4` verdict OK, manifest not stale.

---

## Residual risk / what I did NOT do

- **The negative case used a read-only lane.** `0.5.1`'s exe was run as the
  stale binary; nothing under `releases/` or `release_assets/` was written.
  `0.4c` (blessed golden) untouched. No 30th build tree; built in `build64`.
- **Auto-selection still exists.** I did not make `--exe`/`--deploy` mandatory.
  With the fingerprint gate fatal in both the exit code and the report, a
  wrong-lane auto-selection now fails loudly rather than lying, and forcing an
  explicit target would break every documented invocation and every sibling
  lane mid-session. **This is a judgement call the user may want to revisit** —
  the brief's option "refuse to run" is still available and is a one-line change.
- **Lane-table divergence, unfixed.** The MCP `deploy_status` tool reports
  `known_lanes` including `"0.3"`, but `deploy_lease.DEPLOY_FOLDERS` has no
  `0.3`. Two registries, one stale — this is precisely why `--deploy 0.3` was
  "not a name the runner knows". The MCP server is outside this worktree;
  flagged, not fixed.
- **Other report consumers unaudited.** I fixed the producer and the two
  renderers. Anything that reads `report.json` and still derives PASS from
  `rows` alone, rather than the new `overall_result`, keeps the old blind spot.
- **`build64-shader` / `build64-ibl` etc. still exist** with months-old
  binaries. The runner now ignores them, but deleting them would remove the
  trap outright (and reclaim disk on a volume at 99%).
- The two failing harnesses were **never rebuilt** — they were correct all
  along; only the selection was wrong.
- Preflight reported `safe_to_touch=false` from 27 untracked `scratch/` dirs
  belonging to sibling lanes. None were touched; all my edits are in
  `scripts/`, `tools/`, `CLAUDE.md`.
