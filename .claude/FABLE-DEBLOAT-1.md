# FABLE-DEBLOAT-1 — adversarial debloat pass over the 2026-07-25 → 07-30 corpus

Lane: FABLE-DEBLOAT-1 (user-assigned). Worktree `A:/Games/mc2-main`, branch
`claude/nifty-mendeleev`. Base for measurement: `ce68d5e4` (2026-07-25 13:21,
the commit the user's "~60k LOC" estimate matches). Never pushed. No build, no
deploy, no launch. A sibling lane was ACTIVELY committing the review-annotation
feature in this worktree during this pass; every commit below used explicit
paths only and touched nothing that lane had staged or modified.

## 1. The measured diff (the user asked for the real number)

`git diff ce68d5e4..HEAD` at lane start (`cb4cd5ef`):
**376 files, +59,199 / −1,679.** The 60k estimate was accurate.

| Category | Files | Added | Deleted |
|---|---|---|---|
| Status/audit docs (`.claude/*.md`) | 61 | +12,518 | −21 |
| Contract checks (`scripts/check-*`) | 26 | +9,501 | −29 |
| Review-pack manifest (`release_assets_src/0.5.2/review_pack.json`) | 1 | +8,957 | 0 |
| Evidence dumps (`.claude/` json/py/png) | 36 | +7,061 | 0 |
| Engine C++ (GameOS/code/mclib/RenderCore/…) | 69 | +6,393 | −544 |
| Tools (`tools/`) | 37 | +5,144 | −141 |
| Harness/ops scripts (non-check `scripts/`) | 38 | +4,323 | −240 |
| Docs (`docs/`) | 19 | +2,075 | −393 |
| Test fixtures/goldens (`tests/`) | 61 | +1,915 | −266 |
| Shaders | 12 | +1,016 | −25 |
| Tracked `scratch/` (committed by mistake) | 2 | +265 | 0 |
| Other | 14 | +32 | −21 |

Read that table adversarially: **engine + shader behavior change is ~12.5% of
the corpus.**
~64% (38k lines) is process artifact — docs, checks, evidence, the review pack.
Of the engine C++, +1,211 lines is one feature (the GAMEPLAY-DRIVER command
channel in `code/missiongui.cpp`/`mission.cpp`) and +572 is registry rows.

## 2. What this lane deleted (4 commits, ~6,290 lines, all recoverable)

| Commit | What | Why |
|---|---|---|
| `dac2400d` | `scripts/check-gate-defaults.py` (215 ln) | Report-only ("Exit 0 always"), never registered, zero callers; superseded by the enforcing `check-gate-default-agreement.py`, whose own docstring documents this tool's inertness. |
| `f4d01c45` | `scripts/check-harness-gate-drive.py` (132 ln) + its `run_check` row | **A live check whose failure mode cannot fire.** Its premise ("the runner silently strips unlisted MC2_* vars") is refuted at HEAD by reader source (`os.environ.copy()` in the runner and `smoke_lib/runner.py:179`) and is PINNED false by `check-smoke-env-inheritance.py` — two registered checks stood with directly contradictory docstrings. It was also comment-satisfiable: its allowlist was *any* MC2_* string literal anywhere in the runner's text, "deliberately permissive". The seam remains guarded by `smoke_env_inheritance` + `env_drive_guard`/`ratchet`. |
| `6ed1a0d2` | `.claude/shoreline-v0-falsify-1/` (29 files, ~5.5k ln JSON + 14 PNGs + 8 one-shot scripts) | Raw machine output for a design the lane itself graded **NO-GO**. The STATUS doc keeps the verdict and every load-bearing number, and now carries the recovery pointer (`git show 95ffdb9b -- …`). Only inbound reference was the doc itself. |
| `a4bd0ffa` | `scratch/cic1/build_sec19c.py`, `scratch/vissem2/analyze.py` (265 ln) | The only two `scratch/` files ever committed; one-shots whose output already lives in tracked files. |
| `efa2885e` | `.gitignore` +4 ln (an ADD) | Recurrence-proofing for the row above: `/scratch/` was never ignored, which is how the accident happened. |
| `062a71c9` | `scripts/check-env-vars-documented.py` (131 ln) | Second member of the dead report-only class: "Advisory: exit 0 always", zero callers in any executable surface since it landed 2026-06-11. Its job is done by the ENFORCED `env_registry` + `gate_default_agreement` (which sweeps `tier1_env_vars.md`) + `check-flag-registry-drift.sh`. |

**Contract suite, honest accounting.** Baseline measured by me at lane start,
in this worktree: **101 pass / 1 fail (102 ran)** — the FAIL is
`capture_provenance`, a real live catch (misattributed capture manifests),
pre-existing and not mine. My lane's entire registration delta is **minus one
row** (`harness_gate_drive`); the other two deleted scripts were never
registered. Expected end-state: 101 ran → 100 pass / 1 fail.

Measured end-state, in a CLEAN detached worktree at this lane's HEAD (the only
honest way to grade a lane in a shared worktree): **101 ran, 100 pass, 1 fail.**
The one FAIL is `doc_links` — and adversarial attribution shows it is NOT this
lane's: its 7 broken refs (`docs/beta/README_modder_beta.md`,
`docs/beta/known_issues.md`, two cxx17-recon refs, two staticprop-spec refs)
all point at files that exist ONLY UNTRACKED in the live worktree, verified
per-file with `git ls-files --error-unmatch`. Tracked docs reference
never-committed docs, so `doc_links` passes on this machine and fails on every
fresh clone — a pre-existing latent defect this lane's clean-run verification
surfaced, same never-committed class as the `review_anchor_stability` race.
`capture_provenance` passes clean (its evidence trees are untracked by design)
and still fails in the live tree, where its catch is real.

Attribution warning for anyone re-running in the SHARED worktree: a live-tree
run mid-lane showed 96/5 — the extra failures (`gate_default_agreement`,
`spirv_artifacts`, `split_path_contracts` + selftest) are sibling lanes'
uncommitted WIP in flight (their `check-gate-default-agreement.py` was +190
lines unstaged while its own suite entry ran), NOT regressions from these
deletions. This lane's entire suite delta remains: minus one registered row.

## 3. Cluster verdicts

### 3a. Contract checks (~101 → 101 registered; 37 added in range)

The suite is in better shape than the brief feared. The session's own hygiene
lanes (VERIFY-DEBT-1, SHADER-COMMENT-TRUTH-1, STOCK-DATA-TRUTH-1,
TREE-POP-TAG-1) already found and repaired three comment-satisfiable/inert
checks; `harness_gate_drive` was a fourth that escaped, now deleted. Every
incident-born check in `check-contracts.sh` carries its incident inline
(ENOSPC file truncation → `disk_guard`; hardlinked deploy dirs truncating each
other → `link_guard`/`iso_deploy`; 8 lanes smoking a stale exe →
`smoke_fingerprint_*`; 27% pixel noise read as signal → `capture_determinism`;
12 tools grading mission data the engine never ships → `stock_mission_data`;
silently-degrading beauty sidecars → `beauty_sidecars`; env contamination of
captures → `capture_env_*`; 800×600-vs-4K silent upscale → `capture_resolution`).
**KEEP all of those** — each names a specific expensive incident and ~40% of
the suite is selftests proving the other checks still fire, which is the
falsifiability discipline, not bloat.

Flagged, not deleted:

* **`review_anchor_stability`(+selftest) registration race — found, then
  resolved under me.** The `run_check` rows landed in `74267874` while the
  script itself was still untracked: every clone of the HEADs between
  `74267874` and `b47f8e19` (REVIEW-ANNOTATE-1, same day) gets 2 spurious
  suite FAILs from a check that does not exist. A sibling lane committed the
  script mid-way through this pass, so the tip is coherent again. Lesson worth
  keeping: an aggregator row must land in the same commit as its script. The
  pattern recurred LIVE while this lane ran — a sibling added
  `sim_identity_fieldsets` rows to the working-tree aggregator while
  `check-sim-identity-fieldsets.py` was still untracked. A tiny contract is
  possible: a selftest that fails if any `run_check` row names a script git
  does not track.
* **Merge candidates**: `smoke_env_inheritance` + `env_drive_guard` guard the
  same runner-env seam and could be one script. `terrain_daylight_consts`
  (+selftest) exists only because the daylight harness DUPLICATES camera.cpp's
  night-band constants; sharing a header would delete the duplication and both
  checks. Not done here (touches sibling-hot files / build system).
* **Unwired pre-session checks** (`check-env-vars-documented.py`,
  `check-env-allowlist.sh`, `check-flag-registry-drift.sh`,
  `check-new-gates.py`, …34 total not in the aggregator) — out of this range;
  several have callers in release/CI paths I did not exhaustively prove absent,
  so per the lane's own rule ("could not find a caller is not proof") they
  stay, listed for a dedicated pass.
* The suite has **no runtime budget**: the aggregator's header promises "under
  ~5 seconds" per check but total wall time is now minutes. Worth a timing
  column before the next 30 checks arrive.

### 3a-bis. The unwired-check census (verified against a concurrent review's claim)

A concurrent review claimed "34 of 117 check scripts are wired nowhere; 89 have
no selftest". My measured numbers, method stated: **119 `scripts/check-*`
scripts; 21 have zero callers in any executable surface** (aggregator, all of
`scripts/` including `scripts/mcp/repo_intel_server.py`, `tools/`, cmake,
`.claude/hooks/`, user hooks/settings; there is no CI directory in this repo) —
doc mentions were not counted as wiring. **87 of 119 have no selftest.** The
gap to "34" is method (I count any executable caller, not just the aggregator).

I then RAN every cheap unwired check once. That one action proved the
coordinating review's standard — "if this guard died today, what would
notice?" — empirically: **6 of 15 runnable unwired checks FAIL today and
nothing had noticed.** Per-item verdicts (adding commit in parentheses):

| Check | Ran today | Verdict |
|---|---|---|
| `check-env-vars-documented.py` (533ab274) | exit 0 by design | **DELETED** `062a71c9` — report-only, cannot fail |
| `check-memory-size.py` (cd1818c5) | **FAIL — real live catch** | KEEP + WIRE. The memory-file size ratchet is failing on 3 files (`INDEX-RENDERING.md` 27.8KB, `INDEX-RENDERING-archive.md` 27.4KB + 1): the guard died and memory bloat followed — this lane's theme in miniature. |
| `check-unified-projection-retirement.sh` (59fae278) | **FAIL — rot or regression** | ADJUDICATE in a render lane: a "CI gate" from 05-22 that was never in CI is now flagging bare-mvp/rhw sites. Either the retirement regressed unseen or the invariant moved; both readings are findings. |
| `check-frame-graph.py` (4f112101) | FAIL(127) — broken invocation (WSL path mangling into `run-unit-tests.sh`) | Fix-or-delete with the frame-graph arc owner; a gate that cannot execute is not a gate. |
| `check-brain-opord.py` (9f5032e8) | FAIL(2) — needs fixture/artifact | Artifact-dependent; siblings (`brain_fit_schema` etc.) ARE wired. Wire it behind a SKIP-when-absent guard or fold into the wired brain checks. |
| `check-claude-md-pointer.sh` (b1a36f36) | FAIL(2) here — it polices the ROOT repo's thin-pointer CLAUDE.md, not this worktree's | KEEP; root `CLAUDE.md` names it as the enforcement. Wire it where it applies. |
| `check-ambient-guard.py` (930ca97d) | FAIL — needs `latest_render_state.json` from a live run | By-design runtime check (same class as the header-documented `check-debug-state-json.py` exclusion). KEEP, manual. |
| `check-texture-resolution.py` (bcff6136) | FAIL — needs cook inventory | Cook-time validator; KEEP, manual, or wire into the cook scripts that produce its input. |
| `check-agents-md.sh`, `check-json-isolation.sh`, `check-mlr-leaves-gated.sh` (gate still live, 6 code refs), `check-particles-no-cpu-projection.sh`, `check-icon-atlases.py`, `check-object-mvp-currency.py`, `check-oracle-registry.py`, `check-asset-manifest-fixtures.py` | all PASS | Cheap, enforcing, green — **wire-or-delete decision for the owners**; wiring each is one aggregator line. `check-json-isolation.sh` in particular guards a live vendoring invariant nothing else covers. |
| `check-editor-build.sh`, `check-ktx2-probe.py`, `check-toolchain-bom.py`, `check_vk_nointerop_log.py`, `check-asset-pipeline-doc.sh` | not run (build/cook/VK-branch artifacts) | Heavy or branch-specific by design; manual invocation is their documented mode. |

### 3b. Status docs (61 files, +12.5k lines in range)

Verdict: **KEEP through the pending v0.5.2 user gate, then archive-sweep; do
not delete now.** I went in intending to delete and the evidence stopped me:

* Every doc I sampled for deletion turned out to be referenced — by the
  review-pack manifest the user will read (`GOLDEN-REBLESS-2`,
  `DARK-PROP-LIGHTING-6`, `SESSION-GATE-INVENTORY`), by live check-script
  comments as incident provenance (`CONTRACT-REPAIR-2` in
  `check-sampler-bindings*.py`, `GOLDEN-REBLESS-2` in
  `check-capture-determinism.py`/`check-visual-bookmark-honesty.py`), or by
  successor docs. Even the zero-inbound-reference docs (`ADVERSARIAL-VERIFY-2`,
  `VEHICLE-CLASS-TRUTH-1`, `GOLDEN-v051-v052-RETIREMENT-CASE`,
  `CEMENT-OUTLINE-4`) are refutation/decision records, not chatter.
* The brief's "stale doc caused a full re-investigation": confirmed, but the
  root cause cuts the other way. The trees "NO DEFECT" verdict that
  TREES-REOPEN-1 (`03519ea1`) had to refute **left no status doc at all** —
  the wrong conclusion lived only in summaries. And the "mechanism that cannot
  fire" (`submit_trees=0` read as evidence) was retracted **in place** in all
  three documents that asserted it (`9c71dff5`, `b631efe6`). The retraction
  convention works; absence of a doc is what cost a re-investigation.
* Concrete follow-up (post-release, mechanical): `git mv` closed-lane
  `*-STATUS.md` from this range into `.claude/archive-2026-07-30/` (existing
  convention: `.claude/archive-2026-07-05/`), keeping in place the audit chain
  (`ADVERSARIAL-VERIFY-*`, `AUDIT-TRUTH-1`), retraction records, and anything
  the review pack links. Also: 10 UNTRACKED `*-STATUS.md` sit in `.claude/`
  from 07-25-era lanes (`CEMENT-CLASSIFY-3`, `DROPPED-ITEMS-1`,
  `LIGHT-REALISM-FLIP-1`, `NIGHT-AMBIENT-BALANCE-1`, `PROP-AMBIENT-POLARITY-1`,
  `PROP-SHADOW-ENABLE-2`, `REVIEW-ALLPOSES-1`, `SSAO-RETUNE-1`,
  `VEHICLE-BAKED-TRUCK-3`, `VEHICLE-SELFLIT-5`) — commit or discard is an
  owner decision; deleting untracked files is irreversible so this lane left
  them.

### 3c. Diagnostic instrumentation

**The session made this BETTER, materially.** QUIET-DEFAULT-BUILD-1 gated 62
formerly-unconditional tags behind `MC2_DIAG_VERBOSE` and ratcheted the
default-build tag count 83 → 10 (`default_build_quiet` enforces it);
COUNTER-HONESTY-1 caught three instruments stating zeros they could not know
and now forbids un-assignable population enums and undeclared truncation.
Range adds **13 genuinely new tag families** (BAKEORD, CEMENT_* ×4,
FULLBAKE_DIAG, LIGHT_GRID, LIGHT_ROW_CENSUS, OVERLAY_CEMENT_ADOPT,
PAVED_OVERLAY_CLASS, PROPLATCH, PROPROW, TERRAIN_AMBIENT_FLOOR), all gated.
What's still missing is the catalog itself (no tool enumerates tag families →
sites → meaning); build it only when something will consume it.

### 3d. Gates (~48 new registry rows in range)

House convention is killswitch-per-default-ON-change and the default-ON set
(VEHICLE_AMBIENT_FLOOR, VEHICLE_LIT_PATH, TERRAIN_AMBIENT_FLOOR,
OVERLAY_CEMENT_ADOPT, SSAO_BLUR, STATIC_PROP_IBL_NIGHT_SCALE,
PROP_FOLIAGE_ALPHAFLAG…) earns its rows — several fix user-reported complaints
and are explicitly protected. A handful are back-registrations of vars that
already existed unregistered (SHADOW_ENABLE, LETTERBOX_OPAQUE, LIGHT_REALISM) —
that is debt paydown, not new surface. **Inline-and-delete candidates once
their investigation closes** (left in place: engine edits in a sibling-active
worktree, unverifiable without a build/run):

1. `MC2_FULLBAKE_DIAG` — counters introduced as "temporary" by their own
   commit (`b31d5413`); delete after the SPOTLIGHT-DIRECTION-1 user
   click-through.
2. `MC2_SPOTLIGHT_DIRECTION_FIX=2` falsification arm (`79360fdb`) — same
   trigger; a falsification arm outlives its purpose the day the ruling lands.
3. `MC2_PROP_LIGHT_LATCH_DIAG` (+`_INTERVAL`) — probe built for
   FOGLIGHT-LATCH-1, whose hypothesis is **REFUTED** (`cbf7dfdd`); the probe's
   question is answered.
4. `MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE` — one-question probe from the
   LIGHT-POS investigation; same shape.
5. The vestigial passthrough tuple in the smoke runner — every
   `os.environ`-copying row is a documented no-op; with `harness_gate_drive`
   gone nothing demands new rows, and the tuple should shrink to the genuinely
   forced/popped set (runner is sibling-hot; not touched here).

### 3e. Duplicated tooling

Real, but smaller than feared and partly self-corrected: the big overlap class
is **per-lane image-metric one-shots** (`.claude/cac2-evidence/metrics.py`, the
deleted shoreline scripts, `scratch/cic1/classify.py`, `scratch/fg1_metric.py`)
each reimplementing PNG-diff/percent-changed next to
`tools/visual_verify/verify.py`, which is the only graded path. Recommendation:
declare `tools/visual_verify` the sole metric authority for anything that feeds
a verdict; scratch analysis stays scratch (untracked — which the `a4bd0ffa` +
`efa2885e` commits restore as an invariant). Second class was env-gate
checkers: seven tools touched that seam; one (`check-gate-defaults.py`) deleted
here as superseded. Of the six left, two are enforced (`env_registry`,
`gate_default_agreement`) and four are unwired pre-session scripts
(`check-env-allowlist.sh`, `check-env-vars-documented.py`,
`check-new-gates.py`, `check-flag-registry-drift.sh`) with distinct nominal
jobs — a candidate set for the dedicated unwired-checker pass in §3a.

## 4. Adversarial self-review — where a future session could regret this lane

* **`harness_gate_drive`**: regret scenario = someone deliberately switches the
  runner to a strict-allowlist child env; then tuple completeness matters
  again. Mitigation: `smoke_env_inheritance` FAILS CLOSED in that world, and
  its failure text points at the redesign; the deleted check is one
  `git show f4d01c45^:scripts/check-harness-gate-drive.py` away.
* **Shoreline PNGs**: the user might want to eyeball the NO-GO evidence during
  review. The review pack does not link them (verified); the STATUS doc says
  exactly how to resurrect them.
* **`check-gate-defaults.py`**: no realistic regret; it could not fail.
* Deliberately NOT deleted despite being the biggest line items: the
  review-pack manifest (+8,957 — it IS the artifact under user review),
  `stock_missions.md5` (+1,367 — the digest corpus a live check reconciles
  against), and the status-doc corpus (evidence base of the pending release
  gate). Deleting evidence ahead of the user's click-through is this lane's
  named failure mode, and it would have been the easy way to a big number.
* This lane removed ~6.3k of ~59.2k lines (11%). The honest headline is not
  the percentage: the corpus is majority process-artifact, most of it is
  currently load-bearing for the open release gate, and its correct decay path
  is the post-release archive sweep plus the six inline-and-delete items in
  §3d — each of which has a named trigger, not a vague "later".
