# FABLE-META-3 — independent adversarial + meta review, 2026-07-31

Lane: standing adversarial review. No engine launches (hard-blocked; none attempted).
Every claim below carries a path, a sha, or a command I ran with its output. Worktree
read: `A:/Games/mc2-main` @ `49de784c5`.

---

## 1. ERRORS AND GAPS NOBODY HAD CAUGHT

### 1.1 The harness carrying the session's headline finding is itself a dark check

`tools/class_light_band_harness/` — 70 tests including Stage F's "17 of 24 missions
lit backwards" measurement and the tip commit's semantic sign check (`49de784c5`) —
is **not registered anywhere**:

- Not in `REGISTERED_HARNESSES` (`tools/run_contract_tests.py:33-48`). Same-day
  sibling harnesses `object_light_row_harness` (LIGHT-ROW-PURE-1, `8de4b1ec6`) and
  `terrain_daylight_harness` ARE registered.
- Not in `scripts/check-contracts.sh` (grep: no match).
- Invisible to the coverage guard: `scripts/check-contract-coverage.py` contains
  zero occurrences of "harness" — C++ harness registration is outside its contract.

So on the very day UNWIRED-CHECKS-CLOSE-1 closed "the 21 checks nobody ran"
(`8994e41da`) and then found its own guard dark (`c961dded5`), the newest and most
consequential harness was left in exactly that state. If anyone edits the SH-IBL
rotation, **nothing runs the sign check**. The class recurred a third time, same day.

I built nothing; the binary at `build64-h-class_light_band_harness/RelWithDebInfo/`
matches source mtime (both 07-31 00:56). I ran it: **PASS, 70 tests, 0 failures,
49.8 s**. The arithmetic the session leans on is real — it is just unguarded.
(The 49.8 s runtime may be why it was left out of the timed suite; if so, that
reason is written down nowhere, which is the same defect with better manners.)

User-visible? No — repo-only rot risk.

### 1.2 No deployed binary contains the last three hours of engine commits; the
### headline IBL finding cannot be graded with what is deployed

From the manifests (single `src_commit` per install, no `-dirty`):

| install | src_commit | built |
|---|---|---|
| `MC2-OpenGL-CURRENT-2026-07-30` | `e1bdb7af6` | 07-30 22:44 |
| `MC2-OpenGL-AB-SUNAXIS-2026-07-30` | `dab061529` (branch `deploy/ab-sunaxis-2026-07-30`) | 07-31 00:12 |

Verified by `git merge-base --is-ancestor`:

- **CURRENT** has the six earlier default-ON fixes (`5c04048c`, `ccb713ae`,
  `e3310409a`, `a8ea9be5`, `886dd319`, `ea7ef347`) but **not** `a75339bb0`
  (CPU Phong retire), **not** `6340e0523` (shoreline suppression), not
  `9411c1687`, not any of the gated sun/IBL commits.
- **AB-SUNAXIS** has `a75339bb0` + the sun-axis/elevation gates, but **not**
  `6340e0523` and **not** `f16b49e96` (`MC2_STATIC_PROP_IBL_SUN_ROTATE`/`_FLOOR`).

Consequences, graded honestly:

- The A/B kit is better than it first looks. `AB-3` (=1 arm in the old binary)
  faithfully previews the shoreline default flip — in `dab061529` the suppression
  composes `IsShorelineMaskActive() && true`, and the mask accessor resolves to
  default-ON `MC2_TERRAIN_SHORELINE` after `b13019479`, so the =1 arm equals the
  tip default's pixels. `AB-4` gives the Phong revert arm. The AB README
  (`READ-ME-FIRST-AB-TESTS.md`) explicitly discloses the cross-install Phong
  confound. This is good work.
- **But the strongest technical finding of the session — the probe rotation —
  has no reader in either deployed exe.** `f16b49e96` landed 00:51, after both
  builds. Setting `MC2_STATIC_PROP_IBL_SUN_ROTATE=1` in either install is a
  silent no-op. If the user's next session is the click-through, the one fix
  backed by "probe brighter on the wall facing away from the sun on 17 of 24
  missions" cannot be judged at all.
- **CURRENT carries no README.** The install named like the primary has no
  statement of what it contains; only the AB install documents the divergence.
  My own lane briefing called CURRENT "9/9 fixes, the baseline" while listing
  Phong + shoreline as landed default-ON — both absent from CURRENT. That
  framing, relayed forward, reproduces in miniature the day's headline failure
  (the user grading a binary that lacks the fixes he was told about).

User-visible? Yes — it determines what his next session can verify.

### 1.3 A pre-committed launcher bug waiting at merge time

`db8a35e0e` (branch `deploy/ab-sunaxis-2026-07-30`, NOT on nifty) adds launcher row
`{ "MC2_SHORELINE_LEGACY_SUPPRESS", ..., true, "", "Lighting A/B" }` — opt-in shape,
no `offVal`. `6340e0523` (nifty, later) flipped that gate default-ON. When the
launcher branch merges with nifty, unchecking the box emits UNSET, the engine takes
its ON default, and the checkbox can never restore the legacy pass — **the exact
"could never turn the feature off" defect KILLSWITCH-CHECKBOX-1 fixed for eight
other rows in the same commit that introduced this row.** The row needs
`offVal "0"` (the `MC2_CPU_PHONG_RETIRE` row in the same table is the correct
pattern). One line, but it is the bug class of the day, minted fresh.

### 1.4 Small but real: the Phong commit's premise is overstated, and a dead slider

`a75339bb0` says the ground is drawn "VS+FS, no TCS/TES". In fact
`gos_terrain_lod_chunk.cpp:1412-1413` loads `terrain_lod_chunk.tesc/.tese` behind
`s_cliffTessGate`. The conclusion survives — that TES is pure barycentric plane
interpolation ("no displacement... SAME plane", `shaders/terrain_lod_chunk.tese`),
so no drawn configuration Phong-smooths — but the stated premise is falsifiable by
a grep, which is exactly the kind of overstatement the day kept catching elsewhere.
Also: `GuiRuntime/GraphicsOptionsWindow.cpp:336-343` still exposes the Phong
slider + reset button; its only default-path consumer is now retired, so the
control is dead unless `MC2_CPU_PHONG_RETIRE=0`. Cosmetic UI rot.

### 1.5 Claims I attacked that HELD (verified at source, this lane, independently)

- **Shoreline producer census** — I re-derived it from scratch: all 14 fragment
  shaders writing attachment 1 route through `rc_gbuffer1_*`; the only in-band
  (0.15–0.35) producers on the default stack are `static_prop.frag:658/660`
  (0.1875 / 0.25). Water writes 0.0, chunk terrain 1.0, clear 0.0. The shader
  math claims check exactly (`shaders/shoreline.frag`: band at :22, r≤3 ×2 → 6
  texels, `*0.4` → 1.4× max, `foamCoord = TexCoord*screenSize*0.02` screen-anchored,
  `sin(time*2.0 + length(TexCoord-0.5)*50)` ring centred on the monitor).
- **The CPU Phong term was live** — `terrain_phong_alpha_ = 0.5f` default
  (`gameos_graphics.cpp:2154`), only mutated by the ImGui options window. Not a
  retirement of an already-inert term.
- **Retiring it restores stock semantics** — the block was ADDED 2026-04-10
  (`4684ec0d`, `git log -S`); before that, `terrainElevation()` had no Phong for
  the game's whole life. Unset = pre-April = 20-years-soaked arithmetic. The
  commit never states this, and it is its own strongest ship argument.
- **`a9a1ab896` "zero functional lines"** — I strip-diffed the shader and
  txmmgr hunks myself: empty set. Comment-only, as claimed.
- **`45d9edfb4`** is a genuine tightening, not a silencing: negation-scope rule
  measured against all 412 attributable claims (5 hits, 4 false skips), and the
  disputed comment was restated positively so it is now CHECKED (306/306).
- **Stage F / sign check** — ran the harness: 70/70, including
  `after_the_fix_the_sunward_wall_is_the_bright_one` and the F5 `wrongWay == 17`
  assertion; the 106.2° median is asserted (`CH_CHECK(t, median > 90.0)` at
  :2193 with the measured value in the comment).

---

## 2. SHIP / DON'T-SHIP ON THE DEFAULT-ON SET

### `a75339bb0` CPU-PHONG-RETIRE-1 — **SHIP** (with the tier1 smoke it already demands)

The scary framing ("moves object grounding up to ~3.8 m, touches pathfinding and
collision on every map") inverts the risk. What ships default-ON is the REMOVAL of
a 2026-04-10 addition whose sole justification (mirroring `gos_terrain.tese`) died
on 2026-06-29 (`75b8ebef`). Unset = the exact elevation arithmetic stock MC2
shipped with, now again matching the drawn surface (default LOD-chunk program is
VS+FS, `gos_terrain_lod_chunk.cpp:1256`; the gated tess variant is planar). The
=0 revert token is exact, the AB install carries the revert arm (`AB-4`), and the
new `check-cpu-gpu-cogating.py` fails red on re-divergence with 4 injected
regressions caught. Residual risk — anything tuned since April against
Phong-adjusted elevation — is real but small: placement z in the paks is authored
2001 data, movers re-snap per frame. The commit's own "needs a tier1 run before
merge" is correct and cheap; do it, then ship.

### `6340e0523` LEGACY-SHORELINE-MISAIM-1 — **SHIP**, one asterisk

The technical case is as solid as anything this session produced, and I verified
it independently (census above). The pass cannot reach a water pixel on the
default stack; what it does is animate a screen-anchored shimmer along static
building/fence silhouettes. That is an artifact by construction — no capture
would have graded it "keep". The dissent is preserved verbatim in the registry
row, the revert token is exact, and the legacy path (`MC2_TERRAIN_LOD_CHUNK=0`)
keeps the pass where it actually works. Asterisk: it removes up to 1.4× edge
brightness on props while a dark-props report is open — disclosed in the commit;
`AB-3` exists precisely to let him see it. Second asterisk is §1.3: fix the
launcher row before the branches meet.

### The six already-deployed default-ONs (in CURRENT)

`e3310409a` (+ its honest correction `a9a1ab896`), `5c04048c`/`ccb713ae`,
`a8ea9be5`/`886dd319`, `ea7ef347`, `e1bdb7af6` — no new defect found by this
lane. But say it plainly: **several of these are large, never-observed visual
swings** (ambient floor worth +22.9–31.6% on 19 missions; vehicles going from
raw-albedo-unlit to fully lit; "magnitude still unmeasured" stands in the
remediation ledger for the vehicle row). Shipping them default-ON into a build
whose stated next step is the user's click-through is a defensible bet — the
click-through IS the grading — but nobody should call them "verified". They are
argued, not seen.

---

## 3. VALUE OR ARTIFACTS — the +655-line question

The remediation's own ledger (`.claude/INCAPABLE-GATE-REMEDIATION.md`) self-reports
≈+780/−125, net ≈+655, ~29 executable lines removed, one gate deleted.

**The verdict is: real value, wrong currency, and the ledger itself already knows it.**

- The two averted deletions are worth the sweep on their own. `MC2_LEGACY_LIT_MC2_FRAME`
  and `MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN` are default-ON and live on every
  launch; executing the audit as written would have shipped wrong vehicle lighting
  and removed terrain shadows. An audit with an under-50% hit rate on its §3 was
  neutralized before it did damage. That is not bloat; that is the one thing a
  review layer is for.
- The durable outputs that fit the user's "no bloat" preference DID land: one gate
  deleted at all 10 sites, a dead setter removed, the float-gate blindness of
  `check-gate-default-agreement.py` documented, the cogating checker with
  selftests, and the finding that redirects the whole dark-props hunt (the
  unrotated probe).
- But the knowledge was banked mostly as prose: 437 lines of ledger, ~+192 of
  comment-only annotation (`158e8e9e6`), registry rows that now run to a page
  each. The user reads pixels and diffs, not `.claude/` dossiers. The right
  compression target for "what not to delete" is the checker and the registry
  row — which happened — plus ONE page for him — which `bc51fb4fd`
  ("put the decisions where he will meet them") started. The 437-line ledger is
  for us, and we should be honest that he is paying for it in tree weight he
  explicitly said he does not want. Cap it: statuses to `archive/` on close,
  synthesis docs get a hard page budget.
- One deletion with real payoff is already identified and parked with a correct
  reason (the shoreline pass + its 5 registry entries; needs a lane that can run
  the frame-graph tests). That is the difference between restraint and hoarding —
  it has an owner and a precondition, not a shrug.

---

## 4. WHAT NEXT, AND WHAT TO ABANDON

**Do next (one thing): cut ONE fresh install from nifty tip after a tier1 smoke,
with two batch arms for `MC2_STATIC_PROP_IBL_SUN_ROTATE`(+`_FLOOR`), and retire
both 07-30 installs from his desk.** Everything else the session produced is
already A/B-able where it sits; the probe rotation — the largest measured
wrongness (7.71:1 swing aimed 106° off the median mission sun, backwards on
17/24) — is the only major fix with no reader in any binary he can launch, and it
is default-OFF so a tip build adds no ungraded default risk beyond what §2
already ships. This also collapses the CURRENT/AB confusion (one install, one
README, one baseline) instead of patching it with documentation.

**Before any further turret work, run the offline decisive check that four lanes
never ran** (`.claude/META-RIGHT-QUESTION-2026-07-30.md` §5a): parse `mc2_03.pak`
packet-1 turret records, compare placed z against packet-0 terrain elevation at
(x,y) — both parsers exist (`scratch/treesreopen1/objlist.py`, the PostcompVertex
reader in `scripts/terrain_height_audit.py`). Do it twice, with and without the
Phong term, because `a75339bb0` just moved gameplay elevation by up to p99 6.4 wu
on curved ground — the same order as a "sunk" read — so every prior turret
observation now has a live confound, and every future one will until this table
exists.

**Abandon / freeze:**

- **Sunk-turret lanes as launched investigations** — frozen until the offline
  z-table above exists. Four lanes, mechanism unproven; the next data point costs
  a script run, not a session.
- **Black quads as a lighting question** — the meta doc already orders the
  frozen-snapshot hypothesis dead for mc2_03; the standing question is the
  zero-light-row invariant + the D5 caster-killswitch A/B, both cheap, both
  needing a permitted run. No new lanes until then.
- **Runway colour via lighting/tone** — refuted by the user twice; every
  lighting-side lever is proven structurally incapable. The only live lever is
  albedo-side `MC2_CEMENT_RAMP_WORD` (default-OFF) and it needs his eyes, not
  another lane.
- **mc2_17 waterline mesh-side experiments** — meander is a measured hazard
  (deletes 11.2 pp of frame water), feather user-rejected. Park until he grades
  what is already deployed.

---

## 5. WHAT WAS DONE WELL — kept, and meant

- **The correction culture is the best thing in this tree.** `a9a1ab896` retracts
  its own commit's "provably 0" with a byte-level scan of all 8 vehicle binaries
  and records the truncation pitfall that produced two earlier false scans.
  `48e99d0cd` retracts its author's own colormap claim. `ef12fbb45`/`158e8e9e6`
  retract audit entries. Corrections name what does NOT survive. Nobody defended
  a wrong claim to keep a commit tidy.
- **The A/B kit is properly engineered for a non-expert grader**: per-arm batch
  files that warn "DO NOT TEST ON mc2_10 — the delta there is EXACTLY ZERO by
  arithmetic", a README that discloses its own confound, and stderr logs per arm.
  That is how you prevent false-negative gradings before they happen.
- **`49de784c5` is model test design**: it guards the rotation's sign with an
  assertion that never mentions a rotation (sunward wall must be the bright one),
  precisely because two derivations sharing a convention error agree with each
  other. Proven to fire by re-injecting the flip (5 red), restored byte-identical
  (sha256 recorded).
- **KILLSWITCH-CHECKBOX-1** found eight launcher checkboxes that could never turn
  their features off — a whole silent class, fixed at the mechanism.
- **`e1bdb7af6`** — the only default-ON sidecar loader was the only silent one;
  found and made loud before deploy.
- The remediation's skeptic layer earned its cost twice over (§3).

## 6. ADVERSARIAL SELF-REVIEW

- **Tried to refute 1.1**: "50 s is too slow for the timed suite" is the best
  defence; it fails because the exclusion is written nowhere and same-day sibling
  harnesses were registered. Downgraded from "error" to "dark check" (it was run
  manually at authoring time — the binary is fresh); kept, because rot is the
  whole point of registration.
- **Tried to refute 1.2**: the AB README genuinely discloses most of it, and
  AB-3/AB-4 faithfully preview the two headline flips — I verified the AB-3
  composition logic in the deployed source rather than assuming. What survived
  refutation is narrow and hard: no IBL-rotation reader exists in any launchable
  binary, and CURRENT self-describes as nothing at all. Kept at reduced severity.
- **"Would the user see it?"** — 1.2 shapes his next session; 2's ships are his
  pixels; 1.1/1.3/1.4 are repo-only.
- **Costliest-if-wrong recommendation**: the SHIP on `a75339bb0`. If something
  was silently tuned against Phong-adjusted elevation since April, objects
  float/sink visibly. Hedged: tier1 smoke before merge (the commit demands it
  anyway), revert token + AB-4 arm exist, and the offline turret z-table would
  catch exactly this class on real data.
- This review found the session's substance sound. The errors found are errors of
  *delivery and guardianship* — what reached a binary, what guards the new truth —
  not of arithmetic. Given fifteen relayed-false claims upstream, I attacked the
  arithmetic first and it held everywhere I drilled. That is worth trusting.

## 7. COMMIT

This file, single-file commit via private index; sha recorded in the commit line
below after landing.
