# VERIFY-DEBT-1 — five verification defects, closed

Lane: VERIFY-DEBT-1 · worktree `A:/Games/mc2-main` · branch `claude/nifty-mendeleev`
Base HEAD at start: `58da9666` · contracts baseline **79/79 (run, not quoted)** → now **83/83**.

Premise for the whole lane: *a guard that has only ever run against good input is
not known to work.* Every fix below ships with a self-test that breaks the thing
on purpose and asserts the guard fires.

---

## 1. The blessed golden that could never fail — RESOLVED (refused)

`tests/visual/goldens/baseline-5ef261f4/mc2_10_saved_204907/distant_ridge_base.png`
— 300×100, 167 bytes, severity `critical`.

**Verdict: genuinely void, NOT a silent capture failure.** Evidence:

| crop (same rect, independent runs) | unique colours | mean |
|---|---|---|
| golden (r1) | **1** | 0.00 |
| `scratch/gr2/r2/crops/.../distant_ridge_base.png` | **1** | 0.00 |
| `scratch/gr2/r3/crops/.../distant_ridge_base.png` | **1** | 0.00 |
| sibling `spotlight_towers` (same frames) | 36 545 / 36 533 | 38.5 |
| sibling `near_base_lighting` (same frames) | 6 518 / 6 515 | 26.2 |

A failed capture does not fail identically three times while its neighbours in the
*same frames* come back rich. The rect (`px 1300,200 300×100 @ref 3840×2160`, i.e.
the top 9–14 % of a NIGHT pose) sits in void sky **above** the ridge it claims to
frame. Disposition per the mission's own rule: **refused, crop deleted**, recorded
as a coverage hole with the subject it was protecting.

**Guard:** `DEGENERATE-CROP-REFUSAL-1` in `tools/visual_verify/bless.py`
(`degeneracy_reason`): refuse any crop with < 8 unique colours or luma stddev < 0.5.

**Calibration (not asserted — measured):** run over all 28 members of
baseline-5ef261f4, exactly **1** was refused — the known-bad one. Tightest
*surviving* crop: 421 unique colours / stddev 3.4747
(`mc2_10_saved_204611/night_base_lighting`) = **52× and 6.9× margin**. The tightest
crops are NIGHT poses — the exact false-positive risk case — and they clear easily.

**★ Finding beyond the brief:** the same degenerate crop is in **four** golden
sets, including **`v0.5.2-baseline-PROPOSED`** — the set awaiting user promotion as
the release golden. Not edited here (see §5); surfaced at the promotion gate.

## 2. The three-run all-pairs rule was prose — NOW CODE

Nothing enforced it, and the cited evidence dir (`scratch/gr3/r1/capture_run.json`)
**does not exist** (confirmed: `scratch/gr3/` holds only logs/reports, no `r1/`).

`bless.py` now takes repeated `--run` and refuses below `MIN_RUNS = 3`, grading all
C(N,2) pairs of every region against that region's own verify thresholds. Also
refuses duplicate run dirs (self-comparison) and per-region instability
(`THREE-RUN-ALL-PAIRS-1`) — a region whose run-to-run noise already exceeds the
thresholds verify grades candidates against makes every future verify a coin flip.

## 3. `aoRadius_(24.0f)` — REVERT ADDED, deliberately NOT slaved to the blur gate

**Correction at source.** The brief's premise is imprecise in two ways:
`MC2_SSAO_BLUR` is not what makes the radius inert — `MC2_SSAO` is (`runSSAO`
returns at `if (!ssaoEnabled_)`); and "no revert" was already false — ImGui and the
per-mission visual-tuning-profile `aoRadius` key both move it.

The **real** defect is narrower and worse: SSAO-BLUR-1 changed *two coupled things*
(added the denoise pass **and** raised aoRadius 3.0 → 24.0) but shipped a killswitch
for only one. So `MC2_SSAO_BLUR=0`, which claims to "restore the pre-SSAO-BLUR-1
two-draw chain", silently keeps the 8× radius — **radius-24-without-denoise, a
configuration that never shipped**, and precisely the speckle-on-flat-faces failure
SSAO-RETUNE-1 documented.

**Neither offered option was taken, and here is why.** Slaving the radius to
`MC2_SSAO_BLUR` (either direction) breaks that gate's stated purpose — A/B the
*blur* on one exe — by making it a two-variable change. And because 3.0 is
structurally dead (mean tap 0.87 wu against a 42.67 wu map cell; measured **0.0000 %**
of AO pixels below 250/255), auto-reverting to it would *silently switch AO off* for
anyone setting `MC2_SSAO_BLUR=0` — a worse failure than the one being fixed, and the
exact trap the mission warned against.

**Shipped instead:** `MC2_SSAO_RADIUS` — an explicit, independent launch-time revert
(clamped `[0.1,64.0]`, matching `gos_SetSsaoRadius`) — plus a loud `[SSAO v1]
WARNING` when the known-bad pairing is actually selected, naming both ways out.
Registered in `RendererFeatureRegistry.h` and documented in `docs/tier1_env_vars.md`.

## 4. `DEMOTED_ANCHORS` was inert — REPLACED by an enforced per-pass anchor

Nothing read the tuple; demoting `ssaoTex` took enforced anchors 3 → 2 and left a
comment behind. The insight: **a name multiplexed across N passes is not
un-anchorable, it is anchorable N times.**

`MULTIPLEX_ANCHORS` in `check-sampler-bindings.py` pins the whole multiplex set —
every `(glsl decl file, C++ receiver, unit)` triple — verified against the source:

| pass | decl | receiver | unit |
|---|---|---|---|
| apply | `shaders/ssao_apply.frag` | `ssaoApplyProg_` | 0 |
| blur  | `shaders/ssao_blur.frag`  | `ssaoBlurProg_`  | 1 |

This is **strictly stronger** than the anchor it replaces: each pass keeps the old
protection, the second pass gains protection it never had, and a de-multiplexed name
now *demands promotion back* to `HARD_ANCHORS` (the reverse of the self-demoting
rule, previously a one-way door to the un-anchored WARN census). `DEMOTED_ANCHORS`
survives as a derived alias so it cannot drift from what is enforced.

## 5. The 11 refused regions — RECORDED AND ENFORCED (honest hole, not fake cover)

They were correctly refused for instability (floors to 25.18 %); re-blessing them is
forbidden, and authoring replacement poses needs fresh capture runs (game launch —
out of lane). Taking the mission's third option, but making it a **guard rather than
a note**: `GOLDEN-COVERAGE-LEDGER-1` (`scripts/check-golden-coverage.py`) requires
every region of every bookmark a golden set touches to be **either blessed or listed
in `golden.json`'s `refused` ledger with a rule, a reason, and the subject it
protected**. Silent coverage loss now fails a contract. It also re-applies the
degeneracy floors to already-committed crops and verifies member sha256s.

baseline-5ef261f4 is now **27 blessed + 12 recorded holes** (11 unstable + 1
degenerate). Bookmark `mc2_10_saved_204907` has **ZERO** blessed regions — stated
explicitly in `coverage_note`.

Three pre-ledger sets are **waived with reasons**, not quietly excluded, and a stale
waiver is itself a FAIL. `v0.5.2-baseline-PROPOSED` is waived as **USER-GATED**:
editing the set a human is about to promote is not an agent's call.

## ★ `bless.py` provenance — now tool-emitted

`blessed_commit` (+ `blessed_tree_dirty`) from git, `capture_env` from each frame's
own IHDR and the capture manifests, `gate_state` from the manifests' effective env,
and `provenance_emitted_by` naming the emitter. Schema bumped to `schema_v: 2`.

**Honest limit on "verify the current set's blocks match what the tool would now
produce":** the source frames for baseline-5ef261f4 have been deleted (`scratch/gr2/r*/`
retains only logs and crops; `scratch/gr3/r1/` never existed), so the set **cannot be
regenerated to compare byte-for-byte**. What was verified instead: its hand-authored
`capture_env` already carries `resolution`/`runs: 3`/`trigger_frame`/`settle`, which the
tool now emits itself; its `blessed_commit` was `null`, which the tool would now fill;
and its per-member `reproducibility` blocks already record 3 pairs, which the tool now
computes rather than trusting. The `refused` ledger is new and tool-shaped. **Full
byte-equality is unprovable without a re-capture** — flagged rather than asserted.

---

## Proof each guard fires (synthetic bad input, all in contracts)

`tools/visual_verify/bless_selftest.py` — 18 assertions, all PASS:
- **T1** two-run set → refused (`minimum is 3`)
- **T2** duplicated run dir → refused (self-comparison)
- **T3** all-black region → refused, **no crop written**, uniq=1 cited
- **T4** region agreeing on r1/r2 but diverging on r1/r3 → refused, with
  `r1_vs_r2 = 0.0` / `r1_vs_r3 = 100.0` — **literally the case a two-run bless
  passes.** This is the standing justification for `MIN_RUNS = 3`.
- **T5** mixed capture resolution → refused
- **T6** clean 3-run set blesses, provenance tool-emitted, refusals recorded

`scripts/check-sampler-bindings-selftest.py` — 6 assertions, all PASS:
- **M0** clean synthetic root → no FAIL, anchor evaluated (1 name / 2 passes)
- **M1** `ssao_blur.frag` unit comments INVERTED → FAIL. **This is the real
  historical bug (CONTRACT-REPAIR-2), and the whole-name check is blind to it** —
  the unit *set* is `{0,1}` either way. Only the per-pass pin sees it.
- **M2** `ssaoBlurProg_` rebound to unit 2 → FAIL
- **M3** blur pass deleted (de-multiplexed) → FAIL demanding re-promotion
- **M4** unrecorded third pass declaring `ssaoTex` → FAIL

`scripts/check-golden-coverage.py --selftest` — 6 assertions, all PASS
(C0 accounted set passes · C1 unaccounted region · C2 reasonless hole ·
C3 both-blessed-and-holed · C4/C5 degenerate committed crop named un-failable).

The env-registry guard also fired **on my own change** (`MC2_SSAO_RADIUS`
unregistered) before I registered it — an unplanned live proof of that contract.

## Contracts

`bash scripts/check-contracts.sh --quiet` → **83 passed, 0 failed**, run by me.
79 baseline + 4 new (`sampler_bindings_selftest`, `golden_coverage`,
`golden_coverage_selftest`, `bless_selftest`).

## Open / user-gated

1. **`v0.5.2-baseline-PROPOSED` carries the degenerate `distant_ridge_base` crop**
   (severity `critical`) and blesses all 11 later-refused regions. **Raise at the
   promotion gate.**
2. `baseline-6c47e149` has 12 unaccounted regions + the same crop — re-bless or retire.
3. `mc2_10_saved_204907` has zero coverage. Re-authoring its rects needs a capture run.
4. The `MC2_SSAO_RADIUS` warning path is compile-verified, not runtime-observed
   (`MC2_SSAO` is default-OFF; no SSAO smoke pose exists).
