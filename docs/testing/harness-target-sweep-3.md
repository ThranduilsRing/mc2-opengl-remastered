# HARNESS-TARGET-SWEEP-3

**Status:** RECON ONLY — no code, no production edits. Next wave of harness /
fix+extraction targets now that the zero-touch sweep-2 targets are shipped.
**Method:** main-session sweep over the new categories + one parallel subagent for
LOGISTICS-CSV-TOKENIZER-RECON-1 (verdict in its own section below).
**Parent arc:** [subsystem-harness-arc-1.md](subsystem-harness-arc-1.md).

## Shipped / DONE (do not duplicate)
contract framework (C++ & Python) · shader · render-state (+clean-link) ·
deploy-asset · deploy-release-tree · objmgr watch-policy · IBL registry (+external
manifest +strict) · render-pass-table · icon-atlas (+8-site bug fix) · mech-GLB
external-pack manifest (+strict) · mech-import tg-dump/bone-parity/roster CLI ·
smoke gates bucket coverage. Tested smoke_lib modules: gates, logparse, manifest,
report, runner, selection. `tests/visual/test_visual_diff.py` exists.

## New finds (this sweep)

### GREEN zero-touch — untested verdict/evidence Python modules
`scripts/smoke_lib/` has pure-Python modules with **no test coverage**. These are
the same value class as the just-shipped SMOKE-GATES-BUCKET-COVERAGE-1: untested
*verdict/evidence* logic means weaker smoke triage. All pure (synthetic inputs,
zero production touch, no game).

| Module | Funcs | Bug class caught | Verdict |
|---|---|---|---|
| `oracleparse.py` (241L) | `parse_oracles(text)`, `judge_oracles(r, allow_late_register=)` | verdict-layer: oracle text→PASS/FAIL misclassification (esp. the late-register knob) — a regression silently mis-judges every oracle run | **GREEN** |
| `fingerprint.py` (64L) | `parse_fingerprint(log)`, `check_fingerprint(fp, expected_sha)` | build/config fingerprint drift — "ran the wrong build/tree" (pairs with deploy-release-tree) | **GREEN** |
| `crash_evidence.py` (162L) | pure helpers `_tail`/`_heartbeat_tail`/`_find_minidumps` (capture() shells out) | crash-bundle tail/minidump-discovery parsing | **YELLOW** (capture() is IO; only the pure helpers are cleanly testable) |
| `baselines.py`, `cockpit.py` | — | (not yet inspected in depth; likely GREEN top-ups) | candidate |

### Carry-over / re-confirmed from sweep-2
| Target | Verdict | Notes |
|---|---|---|
| **Logistics CSV tokenizer** | **FIX+HARNESS (GO-w/caveats)** | real live bug — see dedicated section |
| GLB→MC2 texname derivation | YELLOW (extraction; ride BT2018-1B) | wrong-derived-name magenta; `assimp_importer.cpp:266` |
| Camera frustum/pillarbox pure math | GREEN (small extraction) | `quadAabbInFrustum`/`s_pointInScreenTri`/`gos_Compute43Box`; recon `camera-harness-recon-1.md` still valid |
| Path normalizer divergence | YELLOW (doctest assert; merge = own refactor) | 3 lowercasing rules → Linux case bug |
| Texture ext-fallback / missing-texture policy | RED | txmmgr/GL-welded |
| Campaign/mission metadata, sound, movie, logistics purchase/pilot, save/load | RED | FitIni:File / FST |
| blit-OOB centralization | DEFER | touches mechicon.cpp (just changed 6 sites — give it air) |

### ALREADY-COVERED (don't rebuild)
gates/logparse/manifest/report/runner/selection (smoke_lib tests); visual_diff
(`test_visual_diff.py`); IBL strict + mech-GLB strict (shipped this arc).

## Ranked next 5

1. **LOGISTICS-CSV-TOKENIZER-FIX-1 + LOGISTICS-CSV-TOKENIZER-HARNESS-1** —
   FIX+HARNESS, **GO-with-caveats** (subagent-confirmed live bug). Strongest
   target: a real OOB on malformed/truncated mod CSVs, tiny pure extraction, real
   helper testable game-free. Tier1-gated (production path). See section below.
2. **SMOKE-ORACLEPARSE-COVERAGE-1** — GREEN zero-touch. Add `parse_oracles` /
   `judge_oracles` tests to `tests/smoke/` (synthetic oracle text + the
   `allow_late_register` branches). Hardens the verdict layer, exactly like the
   gates coverage. Trivial, no production.
3. **SMOKE-FINGERPRINT-COVERAGE-1** — GREEN zero-touch. Test `parse_fingerprint`
   + `check_fingerprint` (matching/mismatching/absent sha). Closes the "ran the
   wrong build" gap; complements deploy-release-tree. Trivial.
4. **CAMERA-FRUSTUM-HARNESS-1** — YELLOW (small behavior-preserving extraction).
   Frustum split-brain is a real recurring on-screen-cull class; pure math, IBL
   tier once extracted. Recon already done.
5. **GLB-TEXNAME-DERIVE-EXTRACT-1 + HARNESS** — YELLOW. Wrong-derived-name magenta
   bugs; extraction has forward value on the active BT2018-1B milestone. Tier1 +
   a `MC2_ASSIMP_MECH_IMPORT=1` visual confirm.

Trivial side-options after #2/#3: crash_evidence pure-helper coverage; baselines/
cockpit coverage if pure.

## LOGISTICS-CSV-TOKENIZER-RECON-1 — verdict (subagent)

**HEAD `570cfab4`; NOT fixed by any lane (`git log -G extractString` → only initial commit). Bug is live.**

- **Vulnerable:** `code/logisticscomponent.cpp:182-207` `extractString`. The
  `gosASSERT(i < bufferLength)` at :199 is a **release no-op**
  (`gameos.hpp:105` → `((void)0)` without `_ARMOR`); the scan loop caps at `i<512`
  (:186), not `bufferLength`.
- **THE live bug (b) — past-NUL walk:** on a `'\0'` break (last/truncated field),
  `pFileLine += i + 1` (:203) steps **one byte past the terminator**. `init()`
  does ~24 sequential extracts (:87-171); a row with fewer fields and no trailing
  delimiter walks `pFileLine` into unallocated heap → OOB read, with garbage
  feeding `new char[strlen(...)]`/`atoi`.
- **(a) field-overrun is NOT currently reachable** (correction to sweep-2): every
  caller passes ≥1024-byte buffers and the `i<512` cap bounds the memcpy. Harden
  the contract anyway, but (b) is the real defect.
- **Callers:** `extractString/Int/Float` are private, called ONLY within
  `logisticscomponent.cpp` (`init`); top caller `logisticsdata.cpp:242-272`
  (`BYTE line[1024]` + `readLine`). Zero external refs.
- **Extraction:** TINY + PURE — the three helpers touch no class members/globals,
  pure `char*` cursor ops. Move to a leaf TU `code/logistics_csv.{h,cpp}`;
  `init()`'s `cLoadString`/member-alloc tail stays put (out of scope).
- **Helper API:** `int extractField(const char*& cursor, char* out, size_t outCap)`
  (+ `extractInt`/`extractFloat`), bounded by `outCap-1`, advancing past `,`/`\n`
  but **never past NUL**, preserving the `i`/`-1` return contract so well-formed
  parse is byte-identical.
- **Harness:** game-free, links the real `logistics_csv.cpp`. Tests: golden-row
  parity (mandatory), over-long field clamp/no-overrun, truncated-line no past-NUL
  walk, empty/no-trailing-delim, Int/Float on empty/non-numeric.
- **Tier1 REQUIRED** (production component-load path); behavior-preserving so no
  gate needed (optional `MC2_LOGISTICS_CSV_SAFE` default-ON if risk-averse).
- **Verdict: GO-WITH-CAVEATS** → ship as `LOGISTICS-CSV-TOKENIZER-FIX-1` +
  `LOGISTICS-CSV-TOKENIZER-HARNESS-1`. Caveats: bill (b) as the defect (not the
  unreachable (a)); golden-row parity test mandatory; keep the 512/buffer clamp
  magnitude; new TU into both `mc2` and harness CMake targets.

## Recommendation
Take **#1 (CSV fix+harness)** as the next real slice — it's the strongest
remaining target and the recon already de-risked it. Pair it with the two trivial
GREEN verdict-layer top-ups (#2 oracleparse, #3 fingerprint) which need no
production touch and further harden the smoke verdict. Camera-frustum and
GLB-texname remain the YELLOW extraction options after.
