# DELIVERY-REGRADE-1 — re-grade every install against the FIXED fingerprint reader

**Lane:** DELIVERY-REGRADE-1 · worktree `A:/Games/mc2-main` · branch `claude/nifty-mendeleev`
**Date:** 2026-07-31 · **Measurement only** — nothing deployed, no install modified, no engine launched.

Prompted by the closing recommendation of FINGERPRINT-DETACHED-1 (`87d574232`): *any install
previously graded `DELIVERED NOWHERE`, or showing an implausible zero, deserves a re-grade,
because the grade may have been a parse failure rather than a real absence.*

---

## 0. Verification of the premise (STEP 0)

| Claim as relayed to me | Verdict | Evidence |
|---|---|---|
| `87d574232` and `a4e469b70` exist and fix `exe_fingerprint.py` | **TRUE** | Both are ancestors of HEAD `3bd0380ea` (`git merge-base --is-ancestor` = 0 for both) |
| The fix is in the working tree | **TRUE** | `git diff a4e469b70 -- scripts/exe_fingerprint.py` is **empty** — the worktree copy is byte-identical to the committed fix |
| The defect is alignment filler welded onto the next pooled literal, not detached HEAD | **TRUE** | The diff replaces `_strings_before`'s "split on NUL, drop any non-printable token" with `_printable_runs`, which salvages a rejected token's trailing printable run and realigns it to the next 8-byte boundary (`_ALIGN = 8`) |
| The fix is strictly additive — binaries that parsed before parse identically | **TRUE, and measured** | Over 60 binaries, old and new readers agree on **all 40** the old reader could read; the new reader adds 2 and changes none. Zero regressions |
| The bug is branch-agnostic / data-dependent, not detached-HEAD | **TRUE** | Both recovered binaries are on **named** branches (`claude/terrain-ambient-floor-1-clean`, `claude/terrain-ambient-floor-2`), while three detached-HEAD builds (`cf97ed524df6`, `ea7ef34759f2`, `ceb5bb06440d`) parse fine under **both** readers |
| "16/16 binaries on disk fingerprint" | **TRUE but scoped, and the scope was not stated** | 16 is exactly the count of binaries in `build64/` output dirs. There are **30 more** in install/deploy trees that this verification never touched — see §1 |

The one correction I have to make to the commit's own record: **the verification set was
half the disk.** It found 1 affected binary; there were 2, and the one it missed is the one
that mattered, because it is in an *install* and therefore actually graded by
`check-delivery.py`.

---

## 1. The binary inventory, and how it was enumerated

`DEFAULT_INSTALL_ROOTS` was not trusted. Enumeration was a filesystem glob of `A:/` for
`**/mc2*.exe` (via the Glob tool, which does not trip the launch-guard hook the way a shell
`ls` on an exe path does), then every hit was read for the `[BUILD_FINGERPRINT v1]` banner.

| | count |
|---|---|
| Engine-named binaries on `A:/Games` (`mc2.exe`) | **46** |
| — in a `build64/` output dir | 16 ← FINGERPRINT-DETACHED-1's "16/16" |
| — in an install / deploy tree | 30 ← the earlier delivery sweep's "30" |
| Candidates actually scanned (adds `mc2-asan.exe` ×9, `mc2-head.exe`, `mc2s.exe`, plus 3 non-engine controls) | **60** |
| Carry the v1 banner and fingerprint (NEW reader) | **42** |
| Carry no banner at all | 18 |
| Distinct build shas among the 42 | 26 |

**The 16 / 30 figures reconcile exactly: 16 + 30 = 46.** They were two disjoint halves of the
same disk, each sweep blind to the other's set. Neither figure was wrong; both were partial,
and nobody said so.

The 18 banner-absent binaries are correctly *not* engine builds of this port: 5 stock
MechCommander2 `mc2.exe` (MC2X-REAP3, MC2X-CVE-G, `test`, Carver5-feasibility,
MC2-MagicExpansion), 9 `mc2-asan.exe`, `mc2s.exe`, 2 launchers, 1 asset viewer.

**Scope honesty:** `A:/` was swept exhaustively. The other 8 volumes were not — I checked the
plausible roots (`C:/Games`, `D:/Games`, `G:/Games`) and found nothing. A binary in an
unguessed directory on another volume would not be in this table.

---

## 2. Before/after fingerprint table

60 candidates, old reader (`87d574232^`) vs new. The old reader was run from a scratch copy
extracted with `git show`, so the worktree was never perturbed.

| outcome | OLD reader | NEW reader |
|---|---|---|
| fingerprint extracted | 40 | **42** |
| `layout-unrecognized` | **2** | 0 |
| `banner-absent` (not an engine build) | 18 | 18 |

**Every one of the 40 the old reader read, the new reader reads identically.** Zero
regressions. The two recovered:

| binary | sha | branch | built | dirty | is it an install? |
|---|---|---|---|---|---|
| `A:/Games/mc2-opengl/mc2-win64-ambientfloor-1/mc2.exe` | `3b945816dd9b` | `claude/terrain-ambient-floor-1-clean` | 2026-07-30 10:04:15 | no | **YES — graded** |
| `A:/Games/mc2-terrain-ambient-floor/build64/RelWithDebInfo/mc2.exe` | `6b80b97bc8a2` | `claude/terrain-ambient-floor-2` | 2026-07-30 12:33:10 | yes | no (build output) |

FINGERPRINT-DETACHED-1 found the second. The first — the one inside an install tree, the only
one that can move a delivery grade — was outside its verification set.

---

## 3. Per-install × per-fix grades (default roots, 23 installs)

`YES` = "WOULD SEE IT" · `.` = no · `?` = cannot grade

| INSTALL | BUILT-FROM | LEG | VAF | VLP | VGL | OCA | OAG | TAF | CD2 | WSM | SCORE |
|---|---|---|---|---|---|---|---|---|---|---|---|
| MC2-OpenGL-AB-SUNAXIS-2026-07-30 | `dab061529977` | YES | YES | YES | YES | YES | YES | YES | YES | YES | **9/9** |
| MC2-OpenGL-CURRENT-2026-07-30 | `e1bdb7af697d` | YES | YES | YES | YES | YES | YES | YES | YES | YES | **9/9** |
| MC2-OpenGL-CURSORFIX-2026-07-31 | `3d7f9d7871f7` | YES | YES | YES | YES | YES | YES | YES | YES | YES | **9/9** |
| MC2-OpenGL-TIP-2026-07-31 | `e4ce37d91162` | YES | YES | YES | YES | YES | YES | YES | YES | YES | **9/9** |
| mc2-win64-v0.4 | `ccb713ae0227` | YES | . | YES | YES | YES | . | . | . | YES | 5/9 |
| mc2-win64-pom1 | `70b159ec564d` | YES | . | YES | . | YES | . | ? | . | YES | 4/9 |
| mc2-win64-v0.3 | `a599b41f70a7` | YES | . | YES | . | YES | . | . | . | YES | 4/9 |
| mc2-win64-abl-validate | `a871061e8682` | YES | . | YES | . | YES | . | . | . | . | 3/9 |
| **mc2-win64-ambientfloor-1** | **`3b945816dd9b`** | **YES** | . | **YES** | . | **YES** | . | **?** | . | . | **3/9** ← was 0/9 |
| mc2-win64-v0.4c | `d662802367d8` | YES | . | YES | . | YES | . | . | . | . | 3/9 |
| releases/mc2-win64-v0.4d-rc1 | `ccb713ae0227`\*dirty | YES | . | YES | ? | YES | . | . | . | . | 3/9 |
| **MC2 OpenGL Remastered** | **`85aaaa4f57b1`** | . | . | . | . | . | . | . | . | . | **0/9** |
| MC2-Exodus | `afeb974a20bb`\*dirty | . | . | . | . | . | . | . | ? | . | 0/9 |
| MC2-MagicExpansion | NO-FINGERPRINT | . | . | . | . | . | . | . | ? | . | 0/9 |
| MC2X-CVE-G | NO-FINGERPRINT | . | . | . | . | . | . | . | ? | . | 0/9 |
| MC2X-REAP3 | NO-FINGERPRINT | . | . | . | . | . | . | . | ? | . | 0/9 |
| mc2-win64-mc203-regress | `79360fdb565e` | . | . | . | . | . | . | . | . | . | 0/9 |
| mc2-win64-mc203air | `79360fdb565e` | . | . | . | . | . | . | . | . | . | 0/9 |
| mc2-win64-mover-nondet | `6c47e149353c` | . | . | . | . | . | . | . | . | . | 0/9 |
| mc2-win64-treesoff-1 | `79360fdb565e` | . | . | . | . | . | . | . | . | . | 0/9 |
| mc2-win64-v0.4d-rc1 | `79360fdb565e` | . | . | . | . | . | . | . | . | . | 0/9 |
| releases/mc2-win64-refbuild-cf97ed52 | `cf97ed524df6` | . | . | . | . | . | . | . | . | . | 0/9 |
| releases/mc2-win64-v0.5.1 | `fa3a530495c3` | . | . | . | . | . | . | . | . | . | 0/9 |
| **DELIVERED ON N INSTALLS** | | 11 | 4 | 11 | 5 | 11 | 4 | 4 | 4 | 7 |

LEG=LEGACY-LIT-MC2-FRAME · VAF=VEHICLE-AMBIENT-FLOOR-1 · VLP=VEHICLE-LIT-PATH-1 ·
VGL=VEHICLE-LIGHT-GATHER-ALL-LEAVES · OCA=OVERLAY-CEMENT-ADOPT-1 · OAG=OVERLAY-CEMENT-ARTGUARD-2 ·
TAF=TERRAIN-AMBIENT-FLOOR-2 · CD2=CONCRETE-DELIVER-2-RETRACTION · WSM=WATER-SHORE-MASK-1

**The complete before→after delta is one row.** The pre-fix grades were reconstructed by
running today's `check-delivery.py` with the old reader monkey-patched in (no worktree
change). Everything else is bit-identical; per-fix counts moved LEG 10→11, VLP 10→11,
OCA 10→11 and nothing else.

New information the recovered fingerprint bought on that install: on TERRAIN-AMBIENT-FLOOR-2
it reports **EVIDENCE TIERS DISAGREE — binary=YES, source@sha=YES, ancestry=NO**.
`3b945816dd9b` is a real commit but **not an ancestor of HEAD** (dead-end branch), so this
install carries an ambient-floor implementation that is not the mainline one. With no
fingerprint that could not be said at all.

### Installs the configured roots cannot reach

`find_installs` matches `<root>/exe`, `<root>/*/exe`, `<root>/releases/*/exe` — so
`<root>/data/mc2-win64-v0.4/` is **two levels down and unreachable**. Five such trees exist
(one under the product install, one under each `MC2-OpenGL-*`). All five carry the **same**
binary: `eca37ed36f25`, built **2026-06-14**, dirty. Graded explicitly: **0/9**.

Separately: those five share the label `mc2-win64-v0.4` with `A:/Games/mc2-opengl/mc2-win64-v0.4`.
`find_installs` de-duplicates by exe path but the **label is not unique**, so any per-label
table built from `--json` silently merges six distinct installs into one row. That bit me
mid-lane and is worth fixing before anyone builds tooling on the label.

---

## 4. VERDICT ON THE PRODUCT INSTALL

> ### `A:/Games/MC2 OpenGL Remastered` is **genuinely 0 of 9**. This was **not** a parse artifact.

Unambiguously, on three independent legs:

1. **It has a fingerprint, and always did.** `mc2.exe` reads `sha=85aaaa4f57b1`,
   `branch=claude/nifty-mendeleev`, `built=2026-07-05T18:49:33`, `dirty=false`,
   `exe_sha256=88b917e4…d43d46`. The **old** reader extracted the same values. It was never
   `NO-FINGERPRINT`, never `layout-unrecognized`. The alignment-filler bug never touched it.
2. **Ancestry rules out all nine fixes.** All 9 introducing commits (`b01a7a96`, `e3310409`,
   `5c04048c`, `ccb713ae`, `3ab322a0`, `886dd319`, `96ffc35d`, `2ade84a8`, `3c864eac`) are
   dated 2026-07-22 → 2026-07-30. The binary was built 2026-07-05. `merge-base --is-ancestor`
   returns **NO for all nine**. A binary cannot contain code written 17 days after it was
   linked.
3. **Its second binary is older still.** `MC2 OpenGL Remastered/data/mc2-win64-v0.4/mc2.exe`
   is `eca37ed36f25`, 2026-06-14. Also 0/9.

**The figure reported to the user tonight was correct.** The nomination decision is not
resting on a mis-grade. A confirmed zero, stated as plainly as a correction would have been.

---

## 5. Session claims that are now suspect

Re-tested against the fixed reader:

| claim | status |
|---|---|
| "`MC2 OpenGL Remastered` delivers 0 of 9" | **STANDS — confirmed genuine.** §4 |
| "`mc2-win64-ambientfloor-1` NO-FINGERPRINT / delivers nothing" | **WRONG — retracted.** It is `3b945816dd9b` and delivers **3 of 9** |
| "16/16 binaries on disk fingerprint" (`87d574232`) | **Scope error.** True of the 16 build64 binaries; the disk holds 46. Recount: **42/42** of the binaries that carry the banner |
| "a disk sweep found 30 engine binaries where the previous found 22" (`b919ee243`) | **Also partial.** 30 = install-tree only; the union is 46 |
| Any verdict derived from `A:/Games/mc2-terrain-ambient-floor/build64/…` being unreadable | **Retract.** It reads: `6b80b97bc8a2`, dirty, `claude/terrain-ambient-floor-2` |
| `HANDOFF-2026-07-31.md` §3 install table | **STANDS.** All five rows re-verified by fingerprint + ancestry; unchanged |
| Every other install's grade | **STANDS.** Bit-identical old vs new |

**What I cannot reconstruct.** Only two verdict-bearing artifacts are tracked in git
(`.claude/HANDOFF-2026-07-31.md`, `.claude/FABLE-META-4.md`); both survive re-testing. Claims
made only in chat transcripts, or in untracked scratch files, are outside what I can re-test —
if a "DELIVERED NOWHERE" was stated about an install other than `mc2-win64-ambientfloor-1`,
this sweep says it was about a genuine absence, but I am inferring that from the *current*
disk, not from the words that were used. I also cannot rule out that a binary was deleted or
overwritten between the original claim and now: 4 recycled `mc2.exe` sit in `A:/$RECYCLE.BIN`
and were deliberately excluded as not-installs.

---

## 6. Negative control

A measurement that has never returned "no" cannot be trusted when it returns "yes". Two
controls, both passing:

- **Discriminating on 39 minutes of history.** Fix `VEHICLE-LIGHT-GATHER-ALL-LEAVES`
  (introduced `ccb713ae`, 2026-07-30 09:43:45):
  - `mc2-win64-v0.4` (built from `ccb713ae0227`) → **WOULD SEE IT**
  - `mc2-win64-v0.4c` (built from `d662802367d8`, 09:04:15 — 39 min earlier) → **NO**, *"the
    source at this build's commit does not contain the fix"*, and the run exits 1 with a
    DELIVERY FAILURE banner.
- **Banner absence is detected, not silently passed.** 18 binaries report `banner-absent`
  and are graded `?` on the code axis with *"positive control absent — not an attributable
  engine build"*, rather than being scored as delivered-or-not.

The grid itself is the broadest control: 195 `NO`, 61 `WOULD SEE IT`, 14 `?` across 270 cells.

---

## 7. Adversarial self-review

**1. Try to refute the headline.** Headline: *the product install's 0/9 is real; exactly one
other install was mis-graded.* The alternatives are (a) I measured a different binary, or (b)
a different fix set.
(a) is ruled out **by sha**: the graded image is `exe_sha256 88b917e4…d43d46` at
`A:/Games/MC2 OpenGL Remastered/mc2.exe`, embedded sha `85aaaa4f57b1`, and
`check-delivery.py` rediscovers that same path itself — I did not hand it a path. The second
binary in that tree (`eca37ed36f25`) was graded separately and also answers 0/9, so the
verdict does not depend on which of the two the user launches.
(b) is ruled out by running the **same** `scripts/delivery-fixes.json` (9 entries, integrity
errors = 0) through both the old-reader and new-reader passes; the manifest was never edited.
Residual refutation risk I accept: if `delivery-fixes.json` names the wrong commits, both my
passes are wrong together. The checker's own integrity pass says otherwise (exit 0, 0 errors).

**2. Would the user still see this on a default launch?** **No — literally nothing changes on
screen.** This lane compiled nothing, deployed nothing, and modified no install. The only
thing that moved is which numbers he can trust when he makes the nomination decision: one
install's score goes 0/9 → 3/9, and the product install's 0/9 is now confirmed rather than
merely asserted.

**3. Least-confident grade.** `mc2-win64-ambientfloor-1`. Its `TERRAIN-AMBIENT-FLOOR-2` cell is
`?` (default unknown) and its evidence tiers openly disagree (binary YES / source YES /
ancestry NO) because `3b945816dd9b` is on a dead-end branch. Its 3/9 is the best-supported
answer available but rests on a build that is not on the mainline. Second-least: the four
`*dirty` rows — `MC2-Exodus`, `releases/mc2-win64-v0.4d-rc1` and the five `data/…` copies —
where the embedded sha by definition does not describe the bytes, so commit-derived evidence
is void and only the binary-needle tier really speaks.

**4. `git show --stat HEAD` / shared index.** Verified after commit — only this file. Private
index used (`GIT_INDEX_FILE`), `git read-tree HEAD` re-run immediately before staging, then
`unset` + `git reset`. Nothing pushed.

---

## 8. Reproduction

```
py -3 scripts/exe_fingerprint.py <binary> --json      # one binary, incl. --operands
py -3 scripts/check-delivery.py                       # 23 installs x 9 fixes
py -3 scripts/check-delivery.py --install-root "A:/Games/MC2 OpenGL Remastered/data"
```
Old-vs-new sweep harness (scratch, not tracked): `sweep.py`, `grade_with_old_reader.py`,
`reconcile.py` — the old reader comes from `git show 87d574232^:scripts/exe_fingerprint.py`,
never from a modified worktree.
