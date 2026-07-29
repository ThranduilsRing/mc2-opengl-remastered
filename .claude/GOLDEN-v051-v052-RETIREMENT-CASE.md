# Retirement case: `v0.5.1-baseline` and `v0.5.2-baseline-PROPOSED`

**Status: NOT retired. This is evidence for a user decision, nothing was changed.**
Prepared by GOLDEN-6C47-AUDIT-1, 2026-07-29, at HEAD `43846054`.
All findings below are read out of the sets' own `golden.json` on disk.

---

## The one-line version

Both sets bless 39 regions. **Eleven of those 39 are regions that the very next
baseline measured as run-to-run unstable and refused**, one more is a crop that
physically cannot fail, and neither set carries the poses needed to reproduce
itself. As references they will produce failures that mean nothing and pass
things that should fail. `baseline-5ef261f4` (27 blessed + 12 recorded holes)
already covers the same ground honestly.

## Evidence

**1. Eleven of the 39 blessed regions are known-unstable.** Verified, not
inherited: `baseline-5ef261f4/golden.json` refuses exactly 11 regions under rule
`THREE-RUN-ALL-PAIRS-1` — worst-pair `changed_pct` exceeded each region's own
verify thresholds (floors up to **25.18%**). Both v0.5.1 and v0.5.2-PROPOSED
bless **11 of 11**. A verify against these is a coin flip on 28% of the set.

**2. A crop that cannot fail, registered `critical`.**
`mc2_10_saved_204907/distant_ridge_base` — 300x100, **167 bytes, exactly one
unique colour (0,0,0)**. Its rect (px 1300,200 300x100 @ ref 3840x2160) sits in
void night sky *above* the ridge it claims to frame, while sibling regions in the
same frames carry 6.5k–36.5k unique colours. It is registered at severity
`critical`, so it reads as the set's strongest guarantee and is in fact its only
guaranteed pass. `scripts/check-golden-coverage.py` reports this today as a
`[WAIVED]` warning against both sets.

**3. Single-run bless — the rule they predate is now enforced in code.** Both
sets' `source_capture` is one `capture_run.json`, and their members carry no
`reproducibility` field (5ef261f4's members do). Neither has a `bless_rule` or
`bless_policy` key at all. The three-run all-pairs rule is now **enforced, not
advisory**:
- `tools/visual_verify/bless.py:85` — `MIN_RUNS = 3`
- `tools/visual_verify/bless.py:169-181` — R1 raises `SystemExit` before writing
  anything, both for fewer than three runs and for the same run directory passed
  twice ("self-comparison, not reproducibility")
- `tools/visual_verify/bless.py:246` — R1/R2 grade **all** pairs via
  `itertools.combinations`, worst governs
- `tools/visual_verify/bless_selftest.py` T1/T4/T6 prove each clause fires
- registered in `scripts/check-contracts.sh:110,111,114`

Why it matters concretely, from 5ef261f4's own `bless_rule.why_all_pairs`:
`light_pools_sourcing` and `runway_grid_complete` measured **0.0000%** on one
pair while moving **9.07%** and **5.16%** on both others. Neither of these two
sets could have caught that.

**4. No vendored poses — they cannot be re-derived.** Neither set has a `poses/`
directory or a `pose_provenance` key; `baseline-5ef261f4` has both (10 poses).
This is not hypothetical: 5ef261f4's `pose_provenance` records that the install
**had only 8 of the 10 poses** — `mc2_10_saved_204523` and `mc2_10_saved_212258`
were absent and had to be restored from the vendored set. Under the same
conditions v0.5.1 / v0.5.2-PROPOSED would be unreproducible, with no record of
what the camera was looking at.

**5. Unattributable, and the exe path no longer identifies a binary.** Both
record `blessed_commit: null`. Both record the **same** exe path
`A:\Games\mc2-opengl\releases\mc2-win64-v0.5.1\mc2.exe` — including the set named
for v0.5.2 — with different `exe_mtime` (`2026-07-23T04:20:42Z` vs
`2026-07-23T23:50:29Z`). The binary was replaced in place, so the recorded path
cannot tell you which build produced either set. Their crops genuinely differ
(**28 of 39 sha256s differ**), so they are two different renders that both claim
the same provenance string.

## What is lost by retiring, honestly

The 39-region headline count, and the only committed artifact of what v0.5.1
shipped looking like. If the sets are wanted as **historical record** rather than
as references, the low-cost middle path is to keep the pixels and stop them being
used for grading — e.g. move them under an `archive/` prefix the verify tooling
does not scan, with a note pointing at this file. That preserves the shipped-look
record while removing the coin-flip regions and the un-failable crop from any
future pass/fail decision.

## Recommendation

- **`v0.5.2-baseline-PROPOSED`: do not promote it to release golden as-is.**
  Promotion would make 11 unstable regions and one un-failable crop the standard
  the project is measured against. If a v0.5.2 golden is wanted, re-bless from
  three runs on a clean-fingerprint v0.5.2 exe; the tooling now refuses to mint a
  worse one.
- **`v0.5.1-baseline`: archive rather than delete**, per the middle path above.
- Either way the LEGACY_WAIVERS entries in `scripts/check-golden-coverage.py`
  must be removed in the same change — the checker FAILs on a waiver whose set is
  no longer on disk, which is what forces this to stay an explicit decision.
