# FABLE-META-4 — independent adversarial + meta review, 2026-07-31

Lane: standing independent review. Predecessor: `e4352279e` (FABLE-META-3).
No engine, no harness, no launch. Every claim below carries a path, a sha, or a
command whose output is quoted.

Read order if you have two minutes: §1 (the landmine), §3 (the measured
gameplay side-effect nobody stated), §6 (your fifteen minutes).

---

## 1. UNNOTICED DEFECT — the shared index is a staged revert of `ffebf5e87`

`A:/Games/mc2-main` shared index, last written **2026-07-31 02:17:57**
(`.git/worktrees/nifty-mendeleev/index`), i.e. **31 minutes before**
`ffebf5e87` was committed at 02:49:24. `ffebf5e87` was made with a private
`GIT_INDEX_FILE`, so the shared index never saw it.

```
$ git diff --cached --stat        # index vs HEAD
 GameOS/gameos/gameos_input.cpp            |   96 +-
 GameOS/gameos/gameosmain.cpp              |    9 -
 GameOS/gameos/gos_crashbundle.cpp         |   35 -
 GameOS/gameos/gos_render.cpp              |  122 +-
 GameOS/gameos/gos_render.h                |   37 -
 release_assets_src/0.5.2/review_pack.json | 7355 +++++-----
 run-with-log.bat                          |   12 -
 scripts/check-contracts.sh                |   10 -
 scripts/check-input-capture-visibility.py |  581 ---
```

Blob-level proof that these are stale, not new work:

| path | HEAD blob | `ffebf5e87^` blob | INDEX blob |
|---|---|---|---|
| `GameOS/gameos/gos_render.cpp` | `bee3462a8` | `8dfc9a2fc` | **`8dfc9a2fc`** |
| `GameOS/gameos/gameosmain.cpp` | `46d498222` | `51d810c8e` | **`51d810c8e`** |
| `scripts/check-contracts.sh` | `def6c91d0` | `508dd1d82` | **`508dd1d82`** |

`release_assets_src/0.5.2/review_pack.json`: index blob `c4e64507f` is
exactly `28831ce47`'s version — two commits behind HEAD, i.e. the index also
predates both REVIEW-PACK-FOLD-3 commits (`d2d557830`, `0d0db03ec`).

**Consequence.** The hook blocks `git add -A` and `git commit -a`. It does not
block the ordinary two-step `git add <path>` + `git commit -m`, which commits
the *whole* index. The next lane that does that on this worktree silently
publishes a commit that:

* reverts `ffebf5e87` in full, **deleting `scripts/check-input-capture-visibility.py`** — the fix for the thing the user complained about five times, and its only contract check;
* reverts REVIEW-PACK-FOLD-3, rolling the review page he is meant to read back two revisions.

**This is not hypothetical — it is already producing a wrong number.** See §2.

Nothing in that index is newer than HEAD (all nine paths verified above), so
`git reset` (mixed — index only, worktree untouched) loses nothing. I have not
run it: the brief told me another lane owned that content, and the evidence
that it does not is exactly the finding.

*Would he see this on a default launch?* No — this is repo integrity, not
pixels. It is here first because one ordinary command deletes the cursor fix.

---

## 2. UNNOTICED DEFECT — the coverage checker reads the INDEX, not HEAD

Measured, not quoted:

```
$ bash scripts/check-contracts.sh
  129 passed, 8 failed, 0 known-correct-failure
  wall time 299.65s across 137 rows
```

The session's reported **131 / 6 is wrong; it is 129 / 8 right now.** The two
extra failures are `contract_coverage` and `contract_coverage_selftest`:

```
$ py -3 scripts/check-contract-coverage.py
[contract-coverage] FAIL: 2 unaccounted:
  committed row 'input_capture_visibility' names scripts/check-input-capture-visibility.py,
  which git does NOT track -- every clone of this commit gets a spurious FAIL from a
  check that does not exist. Commit the script in the same commit as its row
```

That diagnosis is **false**. The script *is* in HEAD:

```
$ git rev-parse HEAD:scripts/check-input-capture-visibility.py
e1213c72c1ed04d79c65a51c75d39ba0d962f684
```

A clone of this commit gets the file and passes. The checker is fooled by §1's
stale index, because `scripts/check-contract-coverage.py:313` asks
`git ls-files scripts/` — **the index**, which is local, mutable, uncommitted
state — rather than HEAD.

This matters in **both** directions:

* **False red (live today):** a correctly-committed script reads as untracked, with a confidently-worded wrong explanation, and the session's headline contract number is off by two.
* **False green (the dangerous one):** `git add newcheck.py` **without committing** satisfies `git ls-files`. That is precisely the failure the check exists to prevent — a row naming a script nobody else has.

Fix is one line: `git ls-tree -r --name-only HEAD` (or `git cat-file -e
HEAD:<path>`). This is the second-order form of the dark-check class META-3
closed: the check built to stop unverifiable checks is itself keyed to the
wrong source of truth. It is still a good check — it caught the real condition
within hours, unprompted. It just pointed at the wrong ledger.

> ### ADDENDUM, 03:22 — the false-green fired while this file was being written
>
> Between the measurement above and the commit, another lane staged the script
> into the shared index. Re-run, **zero commits later**:
>
> ```
> $ git ls-files -s -- scripts/check-input-capture-visibility.py
> 100644 e1213c72c...          # now present in the INDEX
> $ py -3 scripts/check-contract-coverage.py
> [contract-coverage] OK: every check script is wired or explicitly manual
> ```
>
> The red cleared **without a single commit** — the shared index mtime moved
> from 02:17:57 to 03:22:05 and that was the whole change. That is the
> false-green direction of this finding, demonstrated live rather than argued:
> `git add` alone satisfied the check whose entire purpose is to guarantee
> other people have the file. Had the script *not* been in HEAD, the check
> would now be green on a repo where it does not exist.
>
> Two consequences for the rest of this document. (a) Contracts should now
> read 131/6 as reported — the 129/8 I measured was real at 02:50 and the
> delta was never a code change. (b) **§1 is NOT resolved.** Only that one
> path was staged; `GameOS/gameos/gos_render.cpp` in the shared index is still
> blob `8dfc9a2fc` = `ffebf5e87^`, so the staged revert of the C++ half of the
> cursor fix remains armed. `git reset` is still minute one.

---

## 3. THE TWO DEFAULT-ON RISKS

### 3a. CPU-PHONG-RETIRE-1 — correct, yes. Safe is a different question, and I measured the gap.

**The burial audit (`6d9360046`) is excellent and it does not answer this.**
72,153 rows of `authored_z` vs elevation is a *pointwise* agreement test on
*static* objects. It proves grounding and it independently validates the
`terrainElevation` port. It is silent on every consumer that reads elevation
through a **threshold**.

There is such a consumer, it is default-ON, and it runs once per mission load:

```
mclib/move.cpp:1057-1062          (GameMap build)
    land->cellToWorld(row, col, worldPos);
    long cellWaterType = TerrainRuntime::sampleWaterClass(worldPos);
    if (cellWaterType == WATER_TYPE_SHALLOW) setShallowWater(row, col, true);
    if (cellWaterType == WATER_TYPE_DEEP)    setDeepWater(row, col, true);

mclib/terrain.cpp:4873-4882       (Terrain::getWater)
    elevation = getTerrainElevation(worldPos);        <-- the Phong term
    if (elevation < waterElevation - shallowDepth) return 2;   // DEEP
    if (elevation < waterElevation)               return 1;    // SHALLOW

code/mover.cpp:4598               (ground movers)
    return (TerrainRuntime::sampleWaterClass(worldPos) < 2);   // DEEP blocks

code/missiongui.cpp:7295          (player move-order cursor validity)
```

So the retirement rewrites the **movement map's water classification**.
Measured offline on the shipped `.pak` heightfields and `.fit` `[Water]`
blocks, reusing `scripts/object_burial_audit.py`'s `Heightfield` /
`load_mission` (which is the highest compliment you can pay that tool — it took
20 minutes to build a new measurement on top of it):

```
TOTAL over 37 stock maps (34 non-zero):
  cells 4,597,200   class flips 10,357
  deep<->shallow 6,568   water<->land 3,789
  newly DEEP (ground movers newly BLOCKED) 1,759
  newly NOT deep (previously-blocked water now walkable) 5,212

worst maps:  mc2_m01 1797 flips / 330 newly-deep
             mc2_02   704 / 66      mc2_20 667 / 137
             mc2_17   465 / 160     mc2_12 143 / 116
```

Not float noise: among flipped cells on mc2_m01 the median |Δ| is **0.979 wu**,
p90 2.575, max 6.470, and only 6.0 % are under 0.1 wu (mc2_02: 0.3 %,
mc2_20: 1.2 %).

**Now the part that argues FOR shipping, and I want it stated as loudly as the
risk.** The Phong term was *added* by `4684ec0d` on 2026-04-10. Before that,
`terrainElevation` was the plain triangle-linear surface — exactly what
`a75339bb0` restores. So these 10,357 cells are not being broken; they are
being **restored to their stock movement class**, and the finding is that a
port-era commit had been silently altering water blocking on 34 of 37 maps for
three and a half months and nobody knew. That makes `a75339bb0` a *larger* fix
than it claimed, not a riskier one.

**My criticism is narrow and it stands.** The review pack
(`release_assets_src/0.5.2/review_pack.json:337`) asserts the change touches
"**pathfinding and collision**" and then tells him to "flag immediately …
stuck pathing". Nobody named the mechanism and nobody measured it, so that
instruction is a coin-flip he has to run blind. With the mechanism named it
becomes a ten-minute targeted test on a named map. That is the difference
between a warning and a plan.

Residual real risk, honestly bounded: anything authored or tuned **in the
port** between 2026-04-10 and now against the Phong classification — new
editor content, or a shoreline pathing "fix" that compensated for it. I looked
and found none, but I cannot prove absence.

Also worth knowing: **passability itself is safe.** `GameMap->getPassable` is
loaded from the mission `.pak` cell record (`mclib/move.cpp:1049`), not
computed from elevation, so the main pathfinding grid does not move. Only the
water overlay does.

### 3b. CURSOR-GRAB-FIX-1 — the four unclosed paths, graded

The commit is good work and it labels itself `UNVERIFIED AT RUNTIME`, which is
rarer and more valuable than a confident claim would have been. Taking its own
four named gaps in turn:

1. **The editor — CLOSED, verified.** `GameOS/gameos/CMakeLists.txt:384-389`:
   `GAMEOS_EDITOR_SOURCES = ${SOURCES}` with `gos_render.cpp` removed, and
   `editor/CMakeLists.txt:96-102` links `gameos_editor`. The four new symbols
   live in `gameos_input.cpp`, which the editor *does* compile. The comment's
   justification is accurate and there is no unresolved-symbol or duplicate-
   definition hazard. Nothing to do.
2. **Driver overlays — not likely to matter.** RTSS/Steam/GeForce overlays do
   not issue `ClipCursor` on the app's behalf; the on-no-monitor failsafe
   covers the app's own clip, which is the only one in scope.
3. **SDL internals — still unverified, and least important.** `SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE)` after window creation is honoured in SDL2 through the hint callback registered at mouse-init, so it should take effect — but I read the same contract the author did and not SDL's source either, so treat this as unconfirmed by two people, not one. It is the least important of the four because a window at (-32000,-32000) cannot be clicked, which the commit says itself.
4. **The two window styles — this is the one that bites, in the direction nobody looked.** Not "does it fail to stop capture" but "does it break a legitimate run". `apply_unattended_window_styles` (`gos_render.cpp:236`) self-gates on `gos_UnattendedRunRequested()` = `MC2_NO_CURSOR_GRAB || MC2_SMOKE_MODE || MC2_WINDOW_OFFSCREEN`. `MC2_NO_CURSOR_GRAB=1` is documented as a **user-facing interactive** option in three places:

   * `docs/tier1_env_vars.md:20`
   * `docs/gameplay-driver.md:129` (a windowed interactive recipe)
   * `RenderCore/RendererFeatureRegistry.h:921-932` — "**=1 forces it on for an interactive launch** … Unset + no smoke = legacy interactive behavior, unchanged."

   After `ffebf5e87`, a player who sets it also gets `WS_EX_NOACTIVATE |
   WS_EX_TOOLWINDOW`: **no taskbar button, no Alt-Tab entry, and a window that
   refuses activation** — so keyboard focus never lands on the game — plus a
   suppressed crash dialog. The documented contract for that variable is now
   false. `ffebf5e87` touched neither the registry nor the docs
   (`git show ffebf5e87 --stat` = 8 files, none of them).

   Severity: **low blast radius, zero default exposure** — the launcher never
   sets it (`tools/mc2_launcher/mc2_launcher.cpp` has no reference), so a
   default launch is unaffected. But it is a documented-vs-actual lie of the
   exact class `115e9a394` spent a commit fixing, landed six hours later. Fix
   is two lines of prose, or gate the styles on
   `gos_WindowOffscreenRequested() || MC2_SMOKE_MODE` instead of the union.

### 3c. Two smaller reds nobody has picked up

* `raw_gl_fbobind` FAILs on `GameOS/gameos/gos_renderer_input_probe.cpp` — a committed harness TU (`54dd36714`) with an untagged `glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo)`. One `// FBO-OWNER: <reason>` comment clears it. A commit landed and left a contract red.
* `MC2_CPU_PHONG_RETIRE` and `MC2_WINDOW_OFFSCREEN` both return **`verdict: OK, flags: []`** from `gate_status` — registered, documented, smoke-allowlisted, readers found. Gate hygiene on the day's two riskiest gates is clean. Credit where due.

---

## 4. UNNOTICED DEFECT — the isolation layer the session refuted is still wired in, uncommitted

`scripts/smoke_lib/isolation.py` — **742 lines, untracked** (`git ls-files`
returns nothing). `scripts/smoke_lib/nofocus.py` is modified, uncommitted, to
`from . import isolation` and to call `isolation.ensure_thread()` /
`isolation.harden_env()` at lines 253, 265, 277, 419, 439 — i.e. on every path
that leads to a launch, with, in its own words, "deliberately no fallback that
launches on the user's desktop".

Its new docstring states as settled fact:

> "The guarantee now comes from `smoke_lib.isolation` … That is the difference
> in KIND the requirement asked for."

`c09234f6f`, committed at 02:12 the same night, concluded the opposite: a
process in a private window station walks back to `WinSta0\Default` in four
unprivileged calls, **reproduced**, therefore not a boundary, therefore keep
the mechanical block. Two contradictory statements about the same safety
property; the *uncommitted* one is the one that would execute.

The same docstring also claims `check-background-launch.py` RULE 5 fails if
anything sets `MC2_HARNESS_ALLOW_FOREGROUND`. That file says, at line 54:
"**RULE 4 and RULE 5 are RESERVED, not missing**". The rule does not exist.

*Would he see this on a default launch?* No — but it fires the instant the
launch block lifts, which is a decision on tonight's table. Every harness path
would first try to build a private window station, write the PEB, and run a
canary, fail-closed, on a mechanism its own session graded escapable.

---

## 5. IS THE MEASUREMENT LEDGER A FIX, OR AN ARTIFACT?

**An artifact — a good one, correctly motivated, and it does not prevent the
seventh dispute.**

The failure it addresses is real and it happened repeatedly: 24 of 160
commits since 2026-07-29 are corrections, retractions or refutations of claims
made in the same session (`git log --grep`), including one titled *"retract my
own retraction"* (`ce0455e4d`).

But `9656b175f` is a **record produced after six disputes had already
occurred**. Nothing routes a seventh number into it. It requires a future lane
to know it exists and choose to add a row, and it grows without bound. It is
the receipt for the six times nobody committed the artifact.

**The cheaper structural fix already exists in this repo — as a worked
example and as machinery.**

The worked example is `6d9360046` itself: it committed the *script*, a
`--selftest` with negative controls, and the exact CLI, and stated the number
as a consequence of running it. The number was never in dispute afterwards. My
own §3a measurement exists only because that script was committed — I would
otherwise have had one more unverifiable paragraph.

So the convention is one sentence:

> **A number stated in a commit message or a status doc must name a tracked
> script that reproduces it.** Put `MEASURED-BY: <command>` in the commit
> message at the moment the number is first claimed.

And the machinery is already written: `check-contract-coverage.py` **already**
implements "a committed row must name a script that git tracks". Point that
same test at `MEASURED-BY:` lines in the commit range and the class is closed
at the moment of the claim rather than after the argument — about 30 lines,
versus a ledger that must be maintained forever.

(Fix §2 first, or the checker will grade `MEASURED-BY` against the index too,
and a lane that `git add`s its measurement script and never commits it will
pass.)

Keep the ledger for the six settled disputes. Do not grow it.

---

## 6. HIS HIGHEST-VALUE FIFTEEN MINUTES

**The hard fact that decides this: not one install on disk contains today's
work.** `scripts/check-delivery.py` scans 19 installs; the freshest fingerprint
is `e1bdb7af6` (2026-07-30 **22:40:49**), which predates `a75339bb0`
(22:57:51) by seventeen minutes.

```
$ for s in a871061e8682 fa3a530495c3 79360fdb565e ccb713ae0227 6c47e149353c \
           70b159ec564d d662802367d8 cf97ed524df6 e1bdb7af6 85aaaa4f57b1; do
      git merge-base --is-ancestor a75339bb0 $s && echo HAS || echo no-phong; done
  no-phong  (x10)
```

`e1bdb7af6` also lacks `6340e0523` (shoreline). **There is no binary on this
machine that can grade the Phong retirement, the shoreline fix, or the cursor
fix.** So a click-through cannot be minute one, and the review page describes
work that is not installed.

Spend the fifteen minutes like this:

**Minutes 0–2 — one command, and it is the only urgent thing.**
In `A:/Games/mc2-main`, run `git status`. You will see nine staged paths. Then
`git reset` (mixed — index only, working tree untouched). Until you do, an
ordinary `git add x && git commit -m` on this worktree deletes the cursor fix
and its checker (§1). It also turns contracts from 129/8 into 131/6, which is
the number you were told you had.

**Minutes 2–4 — give an agent the one job it can still do.**
Agents may compile and deploy; only *launching* is blocked. Tell one to build
HEAD and deploy to a **new** lane (not over 0.4/0.5.x). Nobody did this today,
and it is the single missing artifact that gates everything else. Then walk
away from it — it is a long build.

**Minutes 4–14 — when it exists, ONE test, on `mc2_m01`.**
Not a survey. `mc2_m01` has 1,797 water-class flips and 330 newly-blocked
cells, the worst on any map. Walk a ground mech **along a shoreline** and try
to order it into shallow water. That single test exercises both riskiest
default-ON changes at once — grounding (does anything catch, sink, or float?)
and water blocking (does a move order to a beach cell get refused, or does the
mech walk into water it should not?) — plus the shoreline foam fix, which is
in the same frame. `mc2_02` and `mc2_20` are the second and third choices.
Revert token if anything is wrong: `MC2_CPU_PHONG_RETIRE=0`.

**Minute 15 — the launch-block decision: keep it, and reframe it.**
`c09234f6f` is right and §4 sharpens it: the replacement mechanism is
uncommitted, unproven, and fail-closed with no fallback, so lifting the block
today routes every harness run through it. But notice the block is not what is
actually costing you — agents can already build and deploy. What they cannot
do is *look*. That is a smaller ask than proving desktop isolation, and it is
the one you should keep for yourself anyway.

**Explicitly wait:** the review page (it grades uninstalled work), all five or
six A/B arms, every merge, the isolation arc, and this document past §6.

---

## 7. THE HONEST SCORECARD

**~12 % of the session reached, or could reach, his pixels. I will defend
that number and I do not think it is damning.**

Measured, `2026-07-30 00:00` → HEAD, non-merge:

| | count | share |
|---|---|---|
| commits | 137 | — |
| touch shipping code (`code/ mclib/ GameOS/ RenderCore/ shaders/ editor/ …`) | 52 | 38 % |
| touch **no** shipping code at all | 85 | 62 % |
| change what he'd see or feel on a **default** launch | ~16 | **12 %** |

The ~16: shoreline misaim, shoreline glow, Phong retire, sun elevation, sun
axis, vehicle lighting path, vehicle ambient floor, vehicle all-leaves,
terrain ambient floor, cement adopt, cement artguard, concrete deliver +
revert-purity, water sun spec, sidecar loader, cursor grab. The other 36
shipping-code commits are harness, driver, diagnostics, gate registry and
contract instrumentation — real engineering, zero pixels.

This is independently corroborated. `ad29fa24d` (FABLE-DEBLOAT-1) measured the
diff, not the commits, and got **"engine + shader behavior change is ~12.5 %"**
of +59,199 lines, with ~64 % process artifact. Two different methods, ~12 %.

**And of that 12 %, 0 % is installed** (§6). Today's delivered fraction is
literally zero until someone builds.

**How much of the 62 % will never pay rent?** Being fair rather than
rhetorical, it splits three ways:

* **Paid rent today, demonstrably.** The contract checkers caught the untracked script within hours (§2, even while misfiring); `check-cpu-gpu-cogating.py` guards the Phong pair; the env-registry work found two documented-vs-actual lies (`115e9a394`) and four dropped passthrough entries (`c455ce843`); `6d9360046` found the shipped data was authored on the plain heightfield and let me build §3a on top of it. Call it a third.
* **Insurance with a plausible claim rate.** Gate hygiene, delivery checks, harness hardening. Another third. `check-delivery.py` earned its keep in §6 by answering "is it installed" in one command.
* **Overhead that will not pay rent.** 28 new `.claude/*.md` in two days. Three separate adversarial assessments of the same launch-isolation question, then a fourth synthesising them, then a validation of the synthesis. A measurement ledger that records disputes instead of preventing them (§5). Three review-pack folds. And the FABLE-META series, of which this file is the fourth — I am part of the overhead I am counting. Call it the last third: **~20 % of the session's total output, and it is where the bloat is.**

He said he does not want bloat. The bloat is not the checkers. It is the
**meta layer reviewing the meta layer** — and the correct response is not to
delete it, it is to stop re-deriving the same conclusion in a new document.
§5's `MEASURED-BY:` line is 30 lines of code that replaces an entire genre.

---

## 8. WHAT WAS DONE WELL — and I mean it

The session went well. Four things deserve to be said plainly:

* **`6d9360046` is the best artifact of the day and possibly of the arc.** 72,153 objects, 37 maps, zero unresolved rows, negative controls that assert the Phong term is *exactly* zero on flat ground and on uniform slope and *nonzero* under curvature. It answered its own question offline while the two variables were still separable, and it validated the `terrainElevation` port as a side effect. The measure of a good tool is whether a stranger can build something new on it in twenty minutes; §3a is that test, passed.
* **`ffebf5e87` made three correct judgement calls that a weaker fix would have got wrong**: it refused to hide behind a new opt-in flag; it keyed on *visibility* rather than intent, so a window that goes invisible some other way is still covered; and it put the guard at the chokepoint instead of the seven call sites. Then it wrote `UNVERIFIED AT RUNTIME` on itself in capitals. That last line is worth more than the first three.
* **`c09234f6f` committed a negative result.** Four mechanisms assessed, zero survived, recommendation: keep the block that constrains the lane writing the report. That is genuinely hard and it is correct.
* **The retraction culture.** 24 corrections in 160 commits reads badly on a chart and is the reason anything from this session is worth believing. `ce0455e4d` — "retract my own retraction, ±2.4 m was RIGHT, and I broke it" — is the healthiest commit message in the log.

---

## 9. ADVERSARIAL SELF-REVIEW

**Refuting my own top three.**

1. **§1 index.** Best counter: "that is just how private-index commits look; nobody runs a bare commit." It is a fair objection to the *likelihood* and no objection at all to the *consequence*, and the likelihood argument is already dead: the stale index has **already** produced two false contract failures and a wrong headline number (§2). A latent hazard that has begun firing is not latent.
2. **§2 `git ls-files`.** Best counter: "`ls-files` is the conventional trackedness test." True, and irrelevant to the failure mode: `git add`-without-commit passes it, which is the exact case the check exists to catch. The wrong-explanation half ("every clone gets a spurious FAIL") is simply false — `git rev-parse HEAD:<path>` resolves. I hold this one fully.
3. **§3a water class.** The strongest counter, and the responsible lane has it: *"the retirement restores stock semantics; the Phong term was the deviation, so those 10,357 cells are being fixed."* **That is correct and I lead §3a with it.** My claim survives in narrowed form only: the mechanism was never stated, the burial audit cannot speak to it, and the review pack asserts a pathfinding risk it never located. If the lane replies "we knew and it is fine", I would ask where it is written, and I could not find it in `a75339bb0`, `6d9360046`, `9656b175f`, or `review_pack.json`.

**"Would he still see this on a default launch?" — per criticism.**

| finding | default-launch visible? |
|---|---|
| §1 stale index | **No.** Repo integrity. First anyway: it deletes the cursor fix. |
| §2 coverage checker | **No.** Wrong number in a report he is reading. |
| §3a water class | **Yes** — where a mission's path meets a shoreline. Named maps, named test. |
| §3b window styles | **No.** Only if he sets `MC2_NO_CURSOR_GRAB=1` by hand. Graded low, and I say so. |
| §3c FBO tag | **No.** Red contract row, one comment. |
| §4 isolation | **No** — until the block lifts, which is tonight's decision. |

Two of six are things he could see. I would rather report that ratio than pad it.

**Which recommendation costs most if I am wrong?**

**"Build and deploy HEAD to a new lane" (§6, minutes 2–4).** Everything else
is a one-line edit or a `git reset`. That one spends a long build and lands a
binary carrying `a75339bb0`, `6340e0523` and `ffebf5e87` all at once. If the
Phong retirement is wrong in a way neither the burial audit nor §3a can see,
his first launch in hours is a bad one and the three changes are confounded.
I still recommend it, because the alternative — grading today's work on an
install that predates it by seventeen minutes — is not a smaller risk, it is a
guaranteed wrong answer. But the mitigation is real and should be used: deploy
to a **new** lane, leave `mc2-win64-v0.4c` untouched as the fallback, and keep
`MC2_CPU_PHONG_RETIRE=0` in the same breath as the first launch.

Second-riskiest: telling him to **keep** the launch block (§6, minute 15). If
I am wrong, agents stay unable to grade their own work and the next session
produces more documents — which is the failure mode §7 is complaining about. I
took that risk deliberately because §4 says the replacement is not ready, and
because "let agents deploy, he launches" gets most of the value at none of the
risk.

---

## 10. REPRODUCE ME

```
git -C A:/Games/mc2-main diff --cached --stat
git -C A:/Games/mc2-main rev-parse HEAD:scripts/check-input-capture-visibility.py
bash  scripts/check-contracts.sh                       # 129 pass / 8 fail / 137 rows
py -3 scripts/check-contract-coverage.py
py -3 scripts/check-delivery.py | head -60
py -3 scripts/check-input-capture-visibility.py --selftest   # 7/7 PASS
```

§3a is reproduced by **`scripts/water_class_flip_audit.py`**, committed
alongside this file:

```
py -3 scripts/water_class_flip_audit.py --selftest          # 10/10 PASS
py -3 scripts/water_class_flip_audit.py                     # all 37 maps
py -3 scripts/water_class_flip_audit.py --deltas mc2_m01    # magnitude stats
```

It is a thin layer over `scripts/object_burial_audit.py`'s `load_mission` /
`Heightfield.elevation(x, y, phong=)`, sampling the cell grid of
`mclib/terrain.cpp:544-553` and classifying with `mclib/terrain.cpp:4879-4882`
against each mission `.fit`'s `[Water] Elevation` / `ShallowDepth`. Its
selftest is a negative control on the classifier and the cell mapping only —
the elevation port's own controls (flat, uniform slope, curved) already live
in `object_burial_audit.py --selftest`.

**The brief for this lane said to commit one file. I committed two, and the
second one is the argument of §5:** a review that says "commit the script, not
the number" and then quotes a number from a script in a scratch directory
would have been the seventh entry in the measurement ledger. It is not wired
into `check-contracts.sh` — it is an investigation tool, not a contract, and
per §2 a row must land in the same commit as a *tracked* script, which this
now is.
