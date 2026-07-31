# FABLE-META-5 — independent adversarial + meta review, 2026-07-31 (second sitting)

Lane: standing independent review. Predecessors: `e4352279e` (META-3), `05f9d83a4`
(META-4). Scope: the thirteen-brief batch, `05f9d83a4..HEAD` — 42 commits when
this review began, 45 by the time it closed, five of them landed by other lanes
*while it ran* (see §4, §5). No engine, no
harness, no launch. Every claim carries a path, a sha, or a command whose output
is quoted.

Read order if you have two minutes: §1 (the design claim that is false in the
default config), §9 (both pending decisions are mis-framed, each in one specific
sentence), §10 (your first fifteen minutes).

---

## 1. UNNOTICED DEFECT — the mech freshness fix's fallback draws NOTHING in the default config. "Decline" is "drop" wearing its rival's argument.

`db221dcc2` (MECH-LIGHTROW-FRESHNESS-1, default-ON) rests its whole design on one
paragraph: re-gather risks mistint, **drop makes the mech VANISH — "unacceptable"**,
and decline is "strictly better than both" because *"the CPU MLR path draws the
mech at mech3d.cpp:3227-3233 with its own per-leaf lightData_"*.

That sentence is false in the shipped default configuration. The chain, every link
verified at HEAD:

1. The body transform dispatch (`mclib/mech3d.cpp:4567-4578`): with
   `g_useGpuMechs && g_useGpuMechLeafSkip && gos_IsModernShadowPathActive()` the
   body runs **`TransformMultiShape_HierarchyOnly`**. All three terms are true on
   a default launch: the first two are `envFlagDefaultOn(...)`
   (`gos_mech_batcher.cpp:134,214`), and the third is "shadow_terrain material
   loaded" (`gameos_graphics.cpp:10133`), which happens unconditionally at
   renderer init (`:5210`).
2. `TransformMultiShape_HierarchyOnly` **delegates to `_BuildRecipe`**
   (`mclib/msl.cpp:1997-2004`), whose per-shape loop does
   `if (s_buildRecipeOnly) continue;` (`msl.cpp:1910-1911`) — the per-leaf
   transform never runs, so each leaf's `lastTurnTransformed` never advances and
   its `listOfVertices` points into a pool that `Mission::update` reset this frame.
3. `TG_Shape::Render`'s staleness guard (`mclib/tgl.cpp:3191`,
   `lastTurnTransformed != turn`) **returns without drawing**. The engine documents
   this exact consequence itself — the PREVIEW-FIX comment at `mech3d.cpp:4560-4566`:
   *"The GPU fast-transform variants (_PositionsOnly / _HierarchyOnly) leave
   listOfVertices stale, so TG_Shape::Render early-returns and the preview goes
   blank."* The preview fix routes around it by forcing the full transform; the
   decline path does not.
4. So on a decline frame, `gpuMechSubmitted` stays false, the fallback at
   `mech3d.cpp:3367-3373` calls `mechShape->Render(true)` — and **nothing is
   drawn**. (Under the `_PositionsOnly` branch — leaf-skip off, modern shadows off
   — the mech draws, but with the per-vertex lighting kernel stripped, so "with
   its own lighting" is wrong there too: `.argb` is whatever the reset pool holds.)

**The documentation makes it worse, not better.** The tier1 row
(`docs/tier1_env_vars.md:1419`) contains a refutation the commit lacks and is
right about it: on the two named persistent-staleness shapes (held Ctrl+R
`doTransformMath`, Alt+D debug-draw), `Mission::update()` is skipped, hence
`clearArrays()` → `resetLightData()` (`code/mission.cpp:1146`) also never runs —
**the light table is not rebased on exactly those frames, so the "stale" index
still names its own correct row**. Pre-fix, mechs drew correctly there. Post-fix
they decline — and per the chain above, in default config they **disappear for as
long as the toggle is held**. The doc calls this arm "a perf regression (not a
correctness one)" because it too believed the CPU fallback draws. Two documents,
each catching the other's blind spot, neither catching the composition.

**Why a default launch still looks fine, stated as loudly as the defect.** The
decline cannot fire in steady state. Order within one frame value of
`g_mc2FrameCounter` (incremented only after `draw_screen()`,
`gameosmain.cpp:2207`): `Mission::update` start → `clearArrays`/`resetLightData`
(`mission.cpp:1146`) → `ObjectManager` update → `Mech3DAppearance::update`
stamps `cachedFrame_ = g_mc2FrameCounter` (`mech3d.cpp:5775` →
`msl.cpp:2050`) → render phase compares the same counter value → fresh. The
pause path still stamps (`updateAppearancesOnly` → `appearance->update(false)`,
`code/objmgr.cpp:4863+`, and the `CacheGpuLightData` call sits outside the one
`if (animate)` gate). The light table cannot fill up (it grows by 128,
`txmmgr.cpp:2029-2035`) so there is no overflow route either. Residual triggers:
the debug toggles above, a possible spawn/restore first-frame blink (the
`[MECHRESTORE v1]` instrument at `mech3d.cpp:3398+` watches exactly the
`gpuMechSubmitted=0` signature), and any future path that renders without a
same-frame update.

The fix is small and the slice's own siblings show it: either force the full
transform on a decline frame (the PREVIEW-FIX precedent), or re-stamp at update
time — which the tier1 row itself already names as "the right fix" if declines
are ever nonzero.

*Would he see this on a default launch?* **No** in steady state (the decline
cannot fire). **Yes** the moment anything makes it fire — as vanishing mechs, not
as the documented perf dip — and the design record that the next maintainer will
trust says the opposite.

---

## 2. UNNOTICED DEFECT — the instrument nominated to settle §1 is write-only

The commit names `s_mechStaleLightRowDeclines` as "the instrument that will say
whether this gate is load-bearing or inert." That counter is read in exactly one
place: **inside the once-per-mech-type log line** (`mech3d.cpp:3251`). No
per-mission summary, no periodic line, no debug-state dump, nothing at shutdown.
The `[MECHBATCHER v1]` fallback summary that carries `StaleLightRow` prints only
under `s_mechBatcherTrace` (env-gated, default off).

So on the first launch, a sustained decline storm prints roughly one line per
mech type — each with `declines_so_far` ≈ 1, because the line fires on the
*first* decline — and then counts silently forever. The plan "read the three
counters on the first launch" is executable for the static-prop sibling, whose
`stale_light_rows_regathered` rides the periodic `[OBJBATCHER v1]` line
(`gos_static_prop_batcher.cpp:2823`), and **not executable for the mech gate**.
One line on the OBJBATCHER pattern (or the mission-end summary) closes it.

---

## 3. LIVE RED — the batch broke its own newest check within nine minutes, and nobody has claimed it

`6f709d9cb` (08:52) armed `flag_registry_truth`: the tracked
`docs/flags.auto.yaml` must agree with `RendererFeatureRegistry.h` at HEAD.
`db221dcc2` (09:01) registered `MC2_MECH_LIGHT_FRESHNESS` in the four places its
commit message lists — and not in the fifth, nine minutes old:

```
$ py -3 scripts/check-flag-registry-truth.py ; echo REAL_EXIT=$?
[flag-registry-truth] FAIL: 1 disagreement(s) ...
  MC2_MECH_LIGHT_FRESHNESS is registered ... but is ABSENT from the doc
REAL_EXIT=1
```

The board is red on it right now (§7). The fix is the checker's own suggestion:
`py -3 scripts/flag-registry-auto-gen.py`, commit the regenerated doc. I did not
fix it — my brief commits one file — but it is a two-minute lane task and it is
exactly the cross-lane time-of-arming/time-of-landing gap the board exists to
catch. **Score it as the system working**: the check was armed at 08:52 and caught
its first real drift at 09:01.

(Found via a detour worth recording: my first measurement of this check said
`EXIT=0` — because I piped it through `tail` and read the pipeline's exit. The
brief warned that the orchestrator made this exact error today; I made it twice
before catching myself, the second time in a way that mattered — see §5.)

---

## 4. VERDICT ON NAMED RISK 2 — check-shader-schema.sh: the risk was real, it had been red for two months, and it was closed by another lane while this review ran

Measured before the fix landed:

```
$ bash scripts/check-shader-schema.sh ; # (exit captured by redirect)
[SHADER_SCHEMA v1] FAIL interface=GpuMechInstance field=visualDamage01 ...
[SHADER_SCHEMA v1] FAIL interface=GpuMechInstance field=visualFlags ...
EXIT=1
```

Red since `3552d0cb1` (**2026-05-29**) renamed `_pad2/_pad3` to
`visualDamage01/visualFlags` in `gos_mech_batcher.h` while `shaders/mech.vert`
kept the padding names. Byte-layout identical (both at offsets 56/60, size 64) —
benign today, a trap for whoever "reuses" padding that is already carrying live
per-mech visual state.

**Why it was dark, and the answer to the brief's "are there others in that
shape":** the header comment in `check-contracts.sh:23` excludes it in prose, and
`manual-checks.allowlist` — the mechanism built to refuse exactly that — does not
list it. It passed `check-contract-coverage.py` anyway because the coverage
checker's external-runner arm credits `CMakeLists.txt:831`'s
`add_custom_target(shader_schema)` — **a non-ALL custom target that no script,
hook, or dependency ever builds**. The credit is one level deep: it asks "does a
runner invoke it" and never "does anything invoke the runner". I enumerated all
nine externally-credited checks: the other eight are genuinely reachable
(`build_release.sh` ×2, `deploy_payload.py` ×2, the smoke harness ×2, a wired
selftest, and `check-env-registry.sh` which is *also* wired directly).
`check-shader-schema.sh` was the only dark one.

**Concurrent closure, verified:** `1fbaa8b31` (09:43:50, while this review was
running) renamed the GLSL fields, rebaked the four mech.vert SPIR-V variants,
wired the row (`check-contracts.sh:850` — now `PASS interfaces=3`, re-measured),
and documented the class-level hole as deliberately deferred ("owns the cmake
targets" is a design question). The finding stands as history, not as an open
item; the **class** — one-level runner credit — is the open item, and it is at
least now written down in the checker itself.

---

## 5. LIVE HAZARD — this review's own board run was corrupted by a concurrent lane, and my pipeline masked it

Timeline, from file mtimes and git:

- ~09:34 — I started `bash scripts/check-contracts.sh --quiet 2>&1 | tail -40`
  in the background.
- **09:38:20** — `scripts/check-contracts.sh` was rewritten (mtime), by the lane
  that landed `918f12799`/`1fbaa8b31` at 09:43.
- My run died with `scripts/check-contracts.sh: line 1364: syntax error near
  unexpected token '('` — line 1364 is a comment; the running bash re-read the
  growing file at a stale byte offset. **Rule 11.6 of the handoff, demonstrated
  on me.**
- The task reported **exit 0** — because `| tail` reports the pipeline's last
  command. The `$?`-after-pipeline trap, same day, third occurrence, this time
  eating a whole board run. (`918f12799`'s commit message independently warns:
  "exit codes captured by redirect (never `| tail`)". The lesson is converging
  from two directions on the same night.)

The lane was still live at measurement time (`scripts/check-delivery.py` modified
09:46:56, untracked `scripts/check-render-contract-alpha-classes.py`, shared
index mtime 09:44:03). And while this file was being written, the same collision
class fired **again, at commit level**: `1fbaa8b31`'s `git add
scripts/check-contracts.sh` swept another lane's (CONTRACT-TRUTH-2) two
uncommitted board rows into HEAD without their still-untracked script — taking
`contract_coverage` red at 148/153 — and `f1295881d` (09:48) un-published them
with a byte-exact restore and the correct lesson: *"the private-index discipline
protects the INDEX from other lanes; it does not protect the COMMIT from their
working-tree content."* That is the day's **fourth** sweep incident, and the
second time `check-contracts.sh` specifically was the collision surface in
fifteen minutes. Nothing wrong with any single lane's work — but **board numbers
taken while a contracts lane is editing the board are not numbers**, and any
lane quoting one should say when it ran. Two stale zero-byte
`index.stash.{30900,35996}.lock` files from Jul 25/26 also sit in the shared git
dir; harmless now, confusing later, delete when convenient.

---

## 6. VERDICT ON NAMED RISK 1 — the "silent perf regression" fear is refuted in steady state; what remains is §1 and §2

The author's stated fear — GPU mech batching quietly disabling itself — **cannot
happen in steady state**, and that is provable from ordering rather than hope
(full chain in §1: reset → stamp → check inside one counter epoch; pause still
stamps; the light table grows rather than fills). In the modes where the gate
*can* fire, the failure is not the feared quiet perf dip: it is **§1's vanishing
mechs**, with **§2's instrument unable to measure it**. So: right fear, wrong
failure mode, and the one instrument nominated to adjudicate is write-only. The
gate itself (`=0` reverts bit-for-bit) is correctly built and correctly
registered everywhere except the nine-minute-old doc (§3).

---

## 7. THE BOARD, measured by this lane

Two runs, because the first one is a finding:

- **Run 1 (~09:34):** died at `line 1364: syntax error near unexpected token '('`
  — a phantom, produced by `1fbaa8b31`'s 09:38 rewrite of the script mid-run
  (§5). My `| tail` reported exit 0 for that wreck; only reading the output file
  caught it.
- **Run 2 (started ~09:47, exit captured by redirect):**

  ```
  153 rows — 150 passed, 3 failed, 0 known-correct-failure   BOARD_EXIT=1
    flag_registry_truth            FAIL   (§3 — MC2_MECH_LIGHT_FRESHNESS absent from doc)
    flag_registry_truth_selftest   FAIL   (cascade; it refuses to grade its arms
                                          while the live doc disagrees — correct behaviour)
    capture_provenance             FAIL   (the standing user-decision red:
                                          scanned=144, unledgered=3, known=2)
  ```

The brief's relayed "150 rows, 148 passed" was true when taken; three commits and
three rows later it is 153/150/3, and both deltas are attributable: two new rows
from the schema/alpha wiring, two new fails from §3's nine-minute doc drift plus
its honest cascade. The row count moved four times while this review ran
(150 → 152 → 153 with a two-row un-publish and re-land in between). **Any board
number without a HEAD sha and a start time is already wrong on this repo.**

---

## 8. CONVERGING OR ACCRETING — both, in different layers, and the layer that is accreting names itself

**The truth layer is converging.** Evidence, not vibes: the batch refuted two of
its own premises with bytes (`87d574232` — the "detached HEAD" theory died and
the real cause was alignment filler in the *reader*; `235744ff8` — the shoreline
keep-rationale died because the only program receiving `MRT_ENABLED` is never
`apply()`'d, which I re-verified: `getTerrainMaterial()`'s only caller is a null
test at `gameos_graphics.cpp:10125`). `DELIVERY-REGRADE-1` re-graded 60 binaries
under old and new readers and moved exactly one verdict. The corrections in
`.claude/HANDOFF-2026-07-31.md` §10 are corrections *of the session by the
session*. That is what converging looks like: fewer wrong sentences per pass, and
each pass cheaper to check than the one before because the tools are committed.

**The guard layer is accreting mass faster than it accretes protection.** The
board went 137 → 152 `run_check` lines in roughly a day; wall time ~340s and
growing, which is the exact number `FAIL-REASON-1` cites as the reason red rows
became wallpaper. Tonight's additions are increasingly guards of guards:
`DOCTEST-COUNT-LOCK-ARM-1` guards a count lock; `DOCTEST-SELFTEST-WITNESS-1`
(`918f12799`) repairs **the witness of the selftest of the count-lock checker** —
level four — after `db221dcc2`'s legitimate enum append blinded four injection
arms because a *third* file encoded the number 62. That is the cost signature of
this architecture: every new pin multiplies copies of a fact, and every copy is a
future desync. Meanwhile 45 commits since META-4 contain — by my count — **one**
change a default launch could show him (`480ceaafa`, and §1's gate in its
cannot-fire sense), against ~33 contract/meta/docs commits.

**And yet the adversarial layer earned its rent tonight, twice over**, and I will
not pretend otherwise: it caught two proposed deletions that would have shipped
wrong vehicle lighting and removed terrain shadows (per brief); the parser-truth
check found 73 wrong tracked verdicts whose dominant cause (C string
concatenation, 99 of 112 dropped rows) nobody predicted; the coverage checker's
census concept is what let §4 be answered in twenty minutes; and `918f12799`
itself is *good* accretion — it replaced literal anchors with derived ones, so
that class of blindness cannot recur, and a blind arm is now a harness error.

**The judgment:** the corpus converges while its guard stack accretes, and the
stack's marginal catch is now mostly *about the stack*. The stopping rule should
be **incident-gated growth**: a new guard-of-a-guard row requires a demonstrated
firing (a real miss it would have caught), not a conceivable one. The three
first-order gaps that actually bound the system tonight are not coverage gaps at
all: (1) nothing has ever been *seen running*; (2) lanes collide in time — four
sweep incidents today (`690eabb1e`'s clobber, the orchestrator-protocol pair
named in my brief, `f1295881d`'s working-tree sweep), plus the 09:38 mid-run
rewrite that corrupted this review's own board run (§5); (3) documents go stale
within minutes of landing (§3, §9). More rows fix none of those. If one
structural change is bought with the time saved by the growth freeze, make it a
**single-writer rule for `scripts/check-contracts.sh`** — a lane lock on the one
file every contracts lane must touch — which addresses (2) at the exact point
where all four incidents happened.

---

## 9. THE TWO PENDING DECISIONS — each is mis-framed in one load-bearing sentence

### 9a. The nominated install / supervised launch

The handoff (`.claude/HANDOFF-2026-07-31.md` §1) frames it honestly as a decision
— CURSORFIX now vs one rebuild from HEAD — and the DELIVERY-REGRADE work behind
it is sound (product install genuinely 0/9, confirmed three ways). Two problems:

**Staleness (mechanical, expected):** "nine of ten default-ON changes" is now
nine of **eleven** (`db221dcc2` added an eleventh after the handoff's last
revision at 07:57), and "21 commits behind, four touching engine source" is now
~37 behind with three more engine-source commits (`db221dcc2`, `5efd553e0`,
`1fbaa8b31`). The rebuild option's value went up since the page was written.

**The misleading sentence (nobody has caught this):**
`AB-0-DEFAULT-baseline.bat` claims *"Every gate below is written out at its own
shipped default ... so launcher_env.json cannot reach any of them."* True for the
seven gates it pins — **and the engine applies `launcher_env.json` to every
`MC2_*` key not set in the shell** (`gameosmain.cpp:1106-1141`, "shell wins"
means *only* shell-set keys win). The CURSORFIX install's JSON (verified on disk)
persists **four engine-default-OFF gates at "1"** — `MC2_STATIC_PROP_PBR_V1`,
`MC2_STATIC_PROP_AMBIENT_V1`, `MC2_CLOUD_SHADOW`, `MC2_WATER_SKYTINT` — plus
`MC2_SHADOW_MAP_SIZE=8192`, `MC2_STATICPROP_TEX_TIER=512`, `MC2_FPS_CAP=165`.
So the "first human look at the ten default-ON changes" happens on an environment
that is **not the default**: four extra features ride along ON, one of which
(`MC2_WATER_SKYTINT`) *suppresses per-mission water art direction by presence*
(`visual_tuning_profile.cpp:182` — `cac9c48f1`'s own finding, discovered 08:40,
five hours **after** the install was built, which is why no one reconciled them).
The A/B arms remain internally valid — every arm shares the riders, so each
arm-vs-AB-0 delta still isolates its one gate. What is confounded is the
**absolute** judgment the handoff's §1 asks for: "do the defaults look right."
Fix is two `set` lines per bat (pin the four to empty), or one regenerated bat
set from HEAD — and it should ride whichever install option he picks.

### 9b. The launcher defaultVal flip "affecting four features"

`cac9c48f1` correctly refused to flip unilaterally and corrected the lying labels.
But the decision as framed — *flip defaultVal for four rows, yes or no* — is
mis-framed three ways:

1. **It bundles a bug with three preferences.** `MC2_WATER_SKYTINT`'s launcher-ON
   is not curation: via the presence test it silently kills per-mission
   `waterSkyTintStrength` art direction on every launcher launch. The commit
   itself proves this and then leaves it inside the same yes/no as three genuine
   taste calls. Unbundle it; its answer does not depend on his taste.
2. **The flip alone cannot heal a single deployed install.** Both the launcher
   (`LoadEnvJson`, `mc2_launcher.cpp:818-836` — JSON overrides `defaultVal`, then
   gets re-persisted) and the engine (`gameosmain.cpp:1106+`) read
   `launcher_env.json`, and every existing install's JSON already carries the four
   keys at "1" (verified on CURSORFIX). Flipping `defaultVal` changes fresh
   installs only; the decision must include migrating or regenerating the deployed
   JSONs, or it will look like it changed nothing.
3. **A third option was silently dropped:** flip the **engine** defaults ON for
   whichever of the four he actually wants always-on — the repo's own convention
   for wanted-by-default features — so shell, bat, and launcher launches finally
   agree. The framing assumes the launcher must move toward the engine; the
   engine moving toward the launcher is a legitimate answer for, e.g., cloud
   shadows, and only he can say which.

Also: this decision currently exists **only in a commit message and source
comments**. It is in no handoff, no review-pack row, no decision doc. A reader of
the designated resume page will not learn he owes this answer.

---

## 10. HIS FIRST FIFTEEN MINUTES, and what to stop

1. **Minutes 0-3 — let a lane close the two mechanical reds.** Regenerate
   `docs/flags.auto.yaml` (§3, one command + commit) and surface the two mech
   counters on the OBJBATCHER pattern (§2, a few lines). Both are agent work; he
   only needs to say go.
2. **Minutes 3-5 — pick the install with the corrected framing.** Given §9a, the
   rebuild option is now clearly better: one build from HEAD closes the cursor
   regression, the light-row gap, the eleventh gate, and permits a bat set whose
   DEFAULT arm actually is the default. Kick it off (agents build and deploy;
   only launching is blocked) and walk away.
3. **Minutes 5-15 — the one supervised launch, unchanged from the handoff but
   with three additions**: launch with `MC2_LOG=1` so the `[LAUNCHER_ENV v1]`
   line discloses exactly which JSON riders applied; confirm **zero**
   `[LIGHTROW_FRESHNESS v1] event=mech_stale_row_declined` lines (§1/§6 — one
   such line on a normal launch means the steady-state proof missed a path and
   the gate needs the re-stamp fix *before* anything else is graded); and read
   the `[OBJBATCHER v1]` stale/regather counters the static-prop fix planted.
   Then the handoff's §1 four-look checklist on mc2_03 stands as written.

**Stop / do not resume:**
- **Stop adding guard-of-guard rows without a firing incident** (§8). The next
  contracts lane should close §3 and then *not* invent a sixth arm.
- **Stop revising the handoff in place per-fact.** Three HANDOFF-FINAL-1 commits
  landed inside five minutes and the page still went stale within the hour
  (§9a). It should freeze until after the launch, then be rewritten once against
  what was *seen*.
- **The AB-1..7 arms are behind the launch, not beside it.** They grade
  default-OFF features; eleven default-ON changes have never been seen. Sequence
  accordingly.
- **The isolation arc stays parked** (unchanged from META-4; Session-0 is his
  measurement to authorize or not).

---

## 11. WHAT WAS DONE WELL — and I mean it

- **`DELIVERY-REGRADE-1` is the model verification lane of the batch.** It
  re-tested a fixed instrument against 60 binaries under both readers, moved
  exactly one verdict, ran negative controls (a 39-minute ancestry
  discrimination), stated its scope honestly ("half the disk"), and confirmed the
  uncomfortable number (the product install's 0/9) rather than softening it.
- **`235744ff8` corrected in place instead of deleting** — the false
  keep-rationale is preserved next to its refutation, so the reversal can be
  judged. And it proved its new census both ways by injection, including one
  injection its own first draft failed (counting its explanatory comments), which
  it reports. That is the standard.
- **`918f12799` is what good meta-work looks like**: it did not bump 62 to 63; it
  removed the class (derived anchors, blind-arm-is-harness-error) and named the
  pipeline-exit trap independently of this review.
- **`db221dcc2`'s registration discipline** — four places, COUNT pin, allowlist,
  tier1 row with an honest self-refutation — is why §1 could be found by reading
  at all. The defect in it is real, but it is a defect in a fix that documented
  its own uncertainty well enough to be audited. Its `=0` killswitch reverts
  bit-for-bit, which caps §1's blast radius at one env var.
- **The cross-lane catch rate is now hours, not days**: the parser-truth check
  armed at 08:52 caught real drift at 09:01 (§3); the schema gate red since May
  was found and fixed the same morning it was first named (§4); and both defects
  DELIVERY-REGRADE-1 reported against `find_installs` were fixed within the hour
  (`a251d6a6b`), alongside a false constant in the render-contract prose that had
  argued against itself for twenty lines (`0090e1457`). Findings in this repo
  currently have a shorter half-life than findings in most repos have a triage
  latency.
- **`f1295881d` is the healthiest failure of the day**: a lane caught its own
  sweep within five minutes, restored the foreign bytes exactly (sha-pinned),
  refused the quiet amend, and extracted the one lesson the house protocol was
  actually missing (§5).

---

## 12. ADVERSARIAL SELF-REVIEW

**Refuting my own top three.**

1. **§1 (decline draws nothing).** Strongest counter: *"the decline cannot fire
   in steady state — your own §6 proves it — so the fallback's quality is moot."*
   Half right. It reduces §1 from user-visible-today to
   wrong-under-the-exact-conditions-the-design-paragraph-argues-about: the commit
   chose decline *specifically because* drop "would not blink — it would stay
   gone" under Ctrl+R/Alt+D — and under those same toggles, in default config,
   decline stays gone too. A design rationale that is false precisely where it is
   load-bearing is a defect even if the code path is cold. Second counter: *"maybe
   Render(true) has a repair path I missed."* I checked the guard (`tgl.cpp:3191`)
   and the delegation chain to `_BuildRecipe`'s `continue`; the engine's own
   PREVIEW-FIX comment asserts the blank-render consequence in writing. I hold
   this, while noting honestly that it is exactly the kind of
   reasoned-from-source-never-executed claim the brief warns about — the first
   launch's `[LIGHTROW_FRESHNESS]` line count is the cheap decisive test, which
   is why §10 step 3 makes it a launch-day check.
2. **§9a (AB-0 is not the default).** Counter: *"the riders are disclosed in the
   handoff's §3 traps and shared by every arm, so the A/B is clean."* True for
   two of the four vars and for the relative comparisons; the handoff's §1
   question is absolute ("do the ten look right"), the bat's own header claims
   the JSON "cannot reach any of them", and the CLOUD_SHADOW/WATER_SKYTINT halves
   were only discovered after the install was built. The framing sentence is
   still false as written. I hold this.
3. **§8 (incident-gated growth).** Counter: *"tonight's level-four witness fix
   caught a real blindness — your rule would have blocked it."* No: the witness
   fix followed a demonstrated firing (`db221dcc2`'s append blinded the arms and
   the selftest went red on it). The rule permits exactly that and blocks only
   speculative guards. The honest cost of my rule: a class that has never fired
   gets no guard until it fires once. I accept that trade explicitly, because the
   observed failure mode of this repo is stale instruments, not unguarded ones.

**"Would the user still see this on a default launch?" — per finding.**

| finding | default-launch visible? |
|---|---|
| §1 decline==drop | **No** in steady state; **yes** (vanishing mechs) if any decline path fires. The design record is wrong either way. |
| §2 write-only counters | **No** — it blinds the *measurement* of §1 on the launch he is about to do. |
| §3 registry red | **No.** Board row; two-minute fix. |
| §4 schema dark-red | **No** — layout was identical; closed concurrently. |
| §5 mid-run rewrite / pipeline mask | **No** — but it invalidates board numbers quoted from that window, including any "148/150" relayed without a timestamp. |
| §9a AB-0 riders | **Yes, on the launch itself** — four non-default features in the frame he will be told is the default. |
| §9b launcher flip | **Yes, on every launcher launch** — and the flip as framed would visibly change nothing on existing installs. |

**Which recommendation costs most if wrong?** §10.2 — "rebuild rather than launch
CURSORFIX now." It delays the first human look by one build. If the build breaks,
the delay grows and CURSORFIX (which exists and is 9/9 on the delivery grid) was
the bird in hand. I still recommend the rebuild because §9a means a CURSORFIX
launch grades a non-default environment against a stale checklist, and because
the fallback is intact: CURSORFIX stays on disk; nothing in §10 modifies it.
Second-riskiest: §1 itself — if I am wrong about the dispatch default, I have
called a shipped fix's design paragraph false in public. The chain is quoted
line-by-line so a five-minute read settles it either way; that is deliberate.

---

## 13. REPRODUCE ME

```
# §1 chain
sed -n '4560,4580p' mclib/mech3d.cpp            # dispatch + PREVIEW-FIX comment
sed -n '1997,2004p' mclib/msl.cpp               # HierarchyOnly -> BuildRecipe
sed -n '1908,1912p' mclib/msl.cpp               # s_buildRecipeOnly continue
sed -n '3188,3195p' mclib/tgl.cpp               # Render staleness guard
grep -n "envFlagDefaultOn" GameOS/gameos/gos_mech_batcher.cpp | head -5

# §2
grep -rn "s_mechStaleLightRowDeclines" mclib/   # one read site, inside the log line

# §3
py -3 scripts/check-flag-registry-truth.py ; echo $?      # exit 1 until regenerated

# §4 (post-1fbaa8b31 it passes)
bash scripts/check-shader-schema.sh --quiet ; echo $?
py -3 - <<'EOF'
# the nine externally-credited checks and their callers
import importlib.util,sys
spec=importlib.util.spec_from_file_location('cov','scripts/check-contract-coverage.py')
m=importlib.util.module_from_spec(spec); sys.argv=['x']
try: spec.loader.exec_module(m)
except SystemExit: pass
checks=sorted(p.name for p in m.SCRIPTS.glob('check-*') if p.suffix in ('.py','.sh'))
print(m.externally_called(checks))
EOF

# §9a
cat "A:/Games/MC2-OpenGL-CURSORFIX-2026-07-31/launcher_env.json"
sed -n '1106,1141p' GameOS/gameos/gameosmain.cpp   # engine applies the JSON, shell wins
# AB-0-DEFAULT-baseline.bat pins 7 gates; the JSON's four default-OFF gates are not among them

# §7
bash scripts/check-contracts.sh --quiet > board.out 2>&1; echo $?   # NEVER `| tail`
```
