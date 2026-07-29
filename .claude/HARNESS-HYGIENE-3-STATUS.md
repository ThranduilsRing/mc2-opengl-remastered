# HARNESS-HYGIENE-3 — STATUS

Lane: HARNESS-HYGIENE-3 · branch `claude/nifty-mendeleev` · commit `f54ba819`
(base `248cecc2`). No build, no deploy, no smoke run. NEVER pushed.

**Contracts verified by running the suite: 79 passed, 0 failed** (was 74/74;
the 5 new checks are registered, so the suite did not break).

---

## Bookend: META REVIEW from reader source

Every claim below was checked against source, not against the brief. **Two of
the four briefs were materially wrong**, and one instrument gap was worse than
described. Those corrections are the main finding of this lane.

| # | Brief said | Reader source says |
|---|---|---|
| 1 | Vars are "silently dropped by the Popen env-replacement" | **False.** `runner.py:83` is `os.environ.copy()`; `:119` is `.update()`. Nothing is stripped. The brief already suspected this and was right. |
| 3 | `--deploy 0.3` "silently fell through to auto-selection" | **False.** `auto_acquire` *raises* `LeaseError` on an unknown name and `run_smoke` exits 6. The fallthrough is operator-driven, not code-driven — but the divergence is real and **worse** than briefed. |
| 4 | 620/69 vs 571/14 | **Different things, both undercounted.** Both used the narrow reader scan (defect 2's blind spot). |
| ★ | No tool asserts capture resolution | **Confirmed, and bless.py never wrote `capture_env` at all** — every golden was uncheckable by construction. |

---

## Defect 1 — ENV-DROP guard on a false premise

**Real cause.** The guard flagged "not in the env_extra allowlist" and called
that a drop. The child env is `os.environ.copy()` + `.update()`, so ambient
`MC2_*` reaches `mc2.exe` untouched; the allowlist rows that copy `os.environ`
values into `env_extra` are literal no-ops. Cost: legitimate A/B arms (an OFF
arm setting `MC2_TERRAIN_LOD_CHUNK=0`) were hard-stopped at exit 2.

**What genuinely cannot be driven** — now declared in
`scripts/smoke_lib/runner.py` as three tables with an `ENV COMPOSITION
CONTRACT` docblock:

- `FORCED_ENV` (6): `MC2_SMOKE_MODE`, `MC2_LOG`, `MC2_HEARTBEAT`,
  `MC2_TGL_POOL_TRACE`, `MC2_ASSET_SCALE_TRACE`, `MC2_NO_CURSOR_GRAB` —
  ambient value discarded.
- `POPPED_ENV` (1): `MC2_GL_ERROR_DRAIN_SILENT` — deleted.
- `SOFT_ENV` (1): `MC2_FORCE_ALL_VISIBLE` — `setdefault`, so ambient **wins**
  (fully driveable; a visibility test can still turn it off).
- Plus any `env_extra` key whose computed value differs from the ambient one.

**What now fires.** `undriveable_ambient()` splits the two cases. UNDRIVEABLE →
exit 2 (fail-closed, unchanged). DELIBERATE → an `[ENV-DECLARED]` line and a
new `env_manifest.ambient_mc2_env` block in `report.json`, so the run is
self-describing instead of refused.

**Env composition is byte-identical** to the pre-refactor literal sequence
(verified by differential test against the original statement order).

**Proof it fires** — `scripts/check-env-drive-guard.py` (+`--selftest`,
registered as `env_drive_guard` / `env_drive_ratchet`):
- positive: `MC2_TERRAIN_LOD_CHUNK=0` and `MC2_GPU_MECHS=0` are **not** refused;
- negative: every `FORCED_ENV` name with a *conflicting* value **is** refused,
  while agreeing values are not; every `POPPED_ENV` name is refused; a
  conflicting `env_extra` is refused, an agreeing one is not;
- ratchet negatives: emptying `FORCED_ENV`, emptying `POPPED_ENV`, and
  demoting `MC2_FORCE_ALL_VISIBLE` from `SOFT_ENV` into `FORCED_ENV` are each
  caught (the last would silently break visibility tests);
- a static assertion that the refuted "Popen env-replacement" sentence cannot
  come back into `run_smoke.py`.

> Not executed: the end-to-end exit-2 path inside a live `run_smoke` invocation
> (that needs a lease + a game launch). Covered at unit level and by static
> text assertions.

---

## Defect 2 — `check-env-registry.sh` blind to 134 readers

**Real cause.** Step 4 matched exactly four accessor names
(`getenv|envFlag|selftestEnvFlag|envFlagDefaultOn`) as a literal prefix before
`(`. The codebase reads env through 25+ idioms. The sharpest illustration:
`envFlagDefaultOn` was listed, **`envFlagDefaultOff` was invisible**. Others
missed: `mc2vkgate::envTriState` (18 vars), `mc2vkgate::subGate` (15),
`parseEnvBoolWithDefault`, `ParseEnvBool`, `envFloat`, `envInt`, `envF`, `envf`,
`tglc_envOn`, `tglc_envStrength`, `mc2AuditLatch`, `HudImGui::envGateOn`,
`read_env_flag`, `lightClampEnvFlagDefaultOff`, `gvEnvFlagDefaultOn`,
`bdForceDynamicDefault`, `directChainGate`, `GetEnvironmentVariableA`. Two more
vars were missed for unscanned dirs (`tests/`, `RenderCore/`).

A name whitelist cannot keep up, and each gap is silent and **fail-open**.

**Fix.** Match *any* identifier immediately followed by an `"MC2_*"` literal,
subtract known env **writers** (`_putenv_s`/`putenv`/`setenv`/
`SetEnvironmentVariable*`), and widen dirs + extensions. Over-reports rather
than under-reports: a false positive costs one allowlist row; a false negative
is an unregistered gate CI swears is accounted for.

**Before / after.**

| | before | after |
|---|---|---|
| vars visible to the scan | **1010** | **1144** (+134) |
| verdict | PASS (wrongly) | PASS (after triage) |
| newly-unregistered found | — | **66** |

**Yes — newly-visible readers were genuinely unregistered.** All 66, now
allowlisted as *batch 10* with per-var reader idiom + file. They are not new
code, only newly *visible*, and they include real shipping gates: the entire VK
native-post family (`MC2_VK_POST_SSAO/_FOG/_SCREENFX*/_HZB*`,
`MC2_VK_HUD*`, `MC2_VK_TERRAIN_*`, `MC2_VK_WATER_*`), `MC2_LIGHTGRID_*`,
`MC2_CLUSTER_DEPTH_PYRAMID_*`, `MC2_POSTPROCESS_COMPUTE_BLUR_*`,
`MC2_STATIC_PROP_{LATE_SPAWN,MISSION_LOAD}_REG`, `MC2_GPU_GV_SHADOW_SKIP`.
Two are not real gates and are labelled as such (`MC2_FOO_AUDIT` — a macro doc
example; `MC2_HOSTSVC_DEFINITELY_UNSET_KEY_XYZ` — a test fixture).

**Proof it fires.** Injected `mclib/_hh3_probe.cpp` reading
`envFlagDefaultOff("MC2_HH3_SYNTHETIC_BLINDSPOT_PROBE")`. New scan → **exit 1**,
naming the var. Counterfactual on the same file: the **old** four-name pattern
produced **no match at all**. Probe removed; suite back to PASS.

---

## Defect 3 — two deploy-lane registries disagree

**Real cause, corrected.** `deploy_status` (`tools/repo_intel/deploy_status.py::
_lane_names`) merges `deploy_lease.DEPLOY_FOLDERS` with
`check-deploy-target.GOOD_DIRS` via `setdefault` and answers for the **union**.
`--deploy` resolves against `DEPLOY_FOLDERS` **only**. The brief's "silently
fell through" is not what the code does — `auto_acquire` raises and `run_smoke`
exits 6. The hazard is the operator loop: MCP says the lane is healthy, the
runner hard-stops, the operator drops `--deploy`, and *then* auto-selection
picks an LRU-free folder.

**The MCP server is inside this worktree** (`.mcp.json` →
`A:/Games/mc2-main/scripts/mcp/repo_intel_server.py`), so it was editable — the
brief's "may live outside" caveat did not apply. No edit was needed: the merge
is legitimate; the registries were not.

**Found worse than briefed** (all pre-existing, none in the brief):

- **`0.4d-rc1` maps to TWO different trees** — `deploy_lease` →
  `releases/mc2-win64-v0.4d-rc1`, `GOOD_DIRS` → `mc2-win64-v0.4d-rc1`. **Both
  exist on disk with their own `mc2.exe`.** Strictly worse than a missing
  name: nothing errors, two tools just use different binaries.
- `0.4c`, `0.5.0`, `0.5.1` are leasable but unknown to the deploy-target
  validator.

**Fix.** `scripts/check-deploy-lane-parity.py` — declares
`deploy_lease.DEPLOY_FOLDERS` the source of truth for leasing and enforces:
same name ⇒ same path; one-sided names must be documented; the MCP union may
not invent a name. Read-only by design — retargeting a deploy lane is a user
decision, not a lint autofix. `0.3` is documented as intentionally
**not leasable** (compromised tree). Registered as `deploy_lane_parity`
(with `--allow-unresolved`, so the 0.4d-rc1 conflict is a **loud WARN on every
contract run**) + `deploy_lane_ratchet`.

**Proof it fires.** 6 synthetic controls, all correct: clean pass;
same-name/different-path FAILS; `GOOD_DIRS`-only undocumented FAILS;
documented one-sided passes; lease-only undocumented FAILS; union-invented lane
FAILS. Strict mode on the live tree still exits 1 on the real 0.4d-rc1 conflict.

**⚠ USER GATE:** which tree is the real `0.4d-rc1`? `releases/` is user-gated,
so no tool may pick unilaterally.

---

## Defect 4 — ENV_DROP_RISK accounting

**Both prior measurements are explicable; neither was right.**

- They **measure different things**: 620/69 counts the default-ON *population*;
  571/14 counts the default-ON *at-risk subset* (risk ∩ default-ON).
- They **share an error**: both used the narrow reader scan — defect 2's blind
  spot — so both undercount the reader set.

Reproduced under one method (`risk`= readers − run_smoke allowlist):

| reader scan | readers | ENV_DROP_RISK | default-ON at risk |
|---|---|---|---|
| narrow (old method) | 1011 | 555 | **14** ← reproduces the "571/14" lane |
| **broadened (corrected)** | **1144** | **608** | **16** |

**Published number: ENV_DROP_RISK = 608, of which 16 are default-ON**, out of
**43** live default-ON gates (39 registry `Feature`/`Trace` with default `true`,
minus 1 `Retired`, plus 8 detected via the `envFlagDefaultOn` code idiom).
(An earlier pass of mine printed 723/20 from a truncated allowlist window —
corrected; the window must end at the ENV-DROP guard, not at the first
unrelated `SMOKE-GATE-GUARD-1` comment.)

**Does the exit-2 refusal cover the default-ON subset?** **The question is
void as posed, and that is the point of defect 1.** "ENV_DROP_RISK" measures
absence from an allowlist that does not gate anything — those 608 vars are
*not* dropped, so there is no silent stripping to cover. The set that genuinely
needs covering is the 7 forced/popped names plus conflicting `env_extra`, and
the corrected guard covers exactly that, fail-closed.

**Both named gates handled and confirmed:**

| gate | allowlisted | registry default | driveable today | guard verdict |
|---|---|---|---|---|
| `MC2_GPU_MECHS` | no (confirmed) | ON (`Retired`) | **yes** | not refused — reaches the engine, recorded in `env_manifest` |
| `MC2_TERRAIN_LOD_CHUNK` | no (confirmed — absent from `run_smoke.py` entirely; only `..._DIAG` appears) | ON (`Feature`) | **yes** | same |

Both are asserted by name in `check-env-drive-guard.py`, so a future change
that makes either undriveable fails CI.

---

## ★ Capture-resolution assertion

**Real cause.** Region rects are normalized fractions
(`tools/visual_verify/regions.py`), and `verify.py::diff_crops` silently
nearest-neighbour-upscales a mismatched candidate to the golden's dims:

```python
if (gw, gh) != (cw, ch):
    c_rgb = _nearest_resize_rgb(c_rgb, cw, ch, gw, gh)
```

So an 800x600 capture crops, grades and **blesses** against a 4K golden with
~17x fewer real pixels. **Additional finding: `bless.py` never wrote
`capture_env` at all** — the 4K reference in the existing golden was
hand-added by the blessing lane, so every tool-minted golden was uncheckable.

**Fix.** `scripts/check-capture-resolution.py`:
- candidate resolution measured from each frame's **PNG IHDR**, not a sidecar
  (`capture_run.json` records no resolution — a sidecar claim goes stale);
- reference from golden `capture_env.resolution`; a golden that declares none
  is itself a **failure**, not a pass;
- wired into `verify_run()` **before any crop is graded**, so it cannot be
  bypassed by the grading path;
- `bless.py` now records the measured resolution and **refuses to mint** a
  golden whose source frames have missing or inconsistent resolutions.

**Proof it fires** (registered as `capture_resolution_ratchet`): positive
control 4K-vs-4K passes; **800x600 vs 4K FAILS** (the exact field incident);
1920x1080 vs 4K FAILS despite identical aspect ratio; 3840x**2159** vs 4K FAILS
(one axis, off by one); a golden with no declared resolution FAILS.

---

## Files

| file | change |
|---|---|
| `scripts/smoke_lib/runner.py` | `FORCED_ENV`/`SOFT_ENV`/`POPPED_ENV` tables + `undriveable_ambient()` + composition contract docblock; behaviour identical |
| `scripts/run_smoke.py` | corrected ENV-DROP guard; `env_manifest` in `report.json` |
| `scripts/check-env-registry.sh` | idiom-agnostic scan; batch-10 allowlist (66 rows) |
| `scripts/check-env-drive-guard.py` | **new** ratchet |
| `scripts/check-deploy-lane-parity.py` | **new** ratchet |
| `scripts/check-capture-resolution.py` | **new** ratchet |
| `tools/visual_verify/verify.py` | resolution assertion before grading |
| `tools/visual_verify/bless.py` | records `capture_env.resolution`; refuses uncheckable goldens |
| `scripts/check-contracts.sh` | 5 new checks registered (74 → 79) |

## Open / user-gated

1. **`0.4d-rc1` path conflict** — needs a ruling on which tree is canonical.
2. **66 newly-visible gates are allowlisted, not registered.** The VK
   native-post family and the lightgrid/cluster/blur trios are real feature
   gates that should be promoted into `RendererFeatureRegistry.h`.
3. **Existing goldens** predate the bless fix; `baseline-5ef261f4` carries a
   hand-added `capture_env.resolution` and passes. Any golden lacking one will
   now fail loudly — by design.
