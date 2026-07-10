# GATE-RETIREMENT-PLAN-1

**Status:** CLEARED-FOR-IMPLEMENTATION — passed adversarial + meta review with required
corrections (applied in this revision). Nothing was actioned before this doc was committed.
**Author:** read-only audit agent (branch `claude/nifty-mendeleev`, HEAD `3d13c2de`).
**Date:** 2026-07-10.
**Scope:** MC2_* env-gate inventory → retirement candidates. Firm/freeze direction.

---

## Method & framing

- Ran `tools/repo_intel/dead_gate_scan.py` (via `mcp__mc2-repo-intel__dead_gate_scan`,
  read-only advisory classifier). Output treated as **CANDIDATES ONLY** — the classifier
  has known precision issues (default-OFF is *not* dead; multi-line getenv assignments
  trip its `result_unused` heuristic; ledger-`REVERTED` proximity mislabels live gates).
- **Every TIER_A candidate was hand-verified** by reading its actual getenv site(s).
- Deadness classes used:
  - **(a) TRUE-DEAD** — value can never change behavior (dead branch / superseded /
    feature deleted / reader is only a retirement-warn or diagnostic print).
  - **(b) BLOCKED-ON-FLIP** — VK Tier-0/Retired set; retire only *after* the user's VK
    flip decision.
  - **(c) KEEP-BY-RULING** — proof-gates from closed slices; gate STAYS by explicit ruling.
  - **(d) DEFAULT-ON-CANDIDATE** — feature user-confirmed + soaked; USER decision only.
  - **(e) KEEP** — everything else (default: when in doubt, KEEP).

---

## Summary counts

Scanner totals (nifty): **910 gates** — TIER_A 17, TIER_B 105, TIER_C 788.

After hand verification of all 17 TIER_A:

| Class | Count | Meaning |
|---|---|---|
| (a) TRUE-DEAD — actionable | **4** | genuine cleanup: retirement-warn / doc-litter / removed-plumbing |
| (a) TRUE-DEAD — neutralized, low-value | **3** | HDR-post trio; already inert, names pinned in registry for index stability |
| (e) KEEP — TIER_A **false positives** | **10** | live feature/tool/kill-switch gates the classifier mislabeled |
| (b) BLOCKED-ON-FLIP | see §5 | VK sub-gate set (lives on `vk-interop-bridge`, not in nifty registry) |
| (c) KEEP-BY-RULING | 1 verified | `MC2_FRAMEGRAPH_REORDER_SPMECH` |
| TIER_B diagnostic-strip | 105 | optional batched sweep, §4 — NOT aggressive delete |

Net: of 17 flagged "dead," only **4 are worth a retirement slice**; 3 more are inert-but-pinned;
**10 are live gates** that must be KEPT. This confirms the classifier's precision caveat —
do not batch-delete from its raw output.

---

## §1. TRUE-DEAD — actionable retirements (class a)

| Gate | getenv site | Evidence | Blast radius | Cleanup deps |
|---|---|---|---|---|
| `MC2_TERRAIN_ACTIVE_AB` (+ paired `MC2_TERRAIN_SOLID_AB`) | `mclib/terrain.cpp:2306-2311` | Reader is a **retirement-warn printf only** — comment "8z-A1: retired (8a/8b/8c shipped, FN=0 validated)"; prints `[8Z_RETIRED_ENV] ... have no effect`. Value cannot change behavior. | ~6 lines (the `s_8zWarnedAB` lambda + `(void)` line) | `scripts/check-env-registry.sh` ALLOWLIST (both names; `registry_check_allowlisted=true`) **plus `docs/tier1_env_vars.md:489-490`** — the diag-tag table carries `TERRAIN_ACTIVE_AB` / `TERRAIN_SOLID_AB` rows (corrected: the earlier "Not in tier1 doc" note was wrong). Not smoke-allowlisted. |
| `MC2_DRAW_PACKET_STATIC_PROP_V6` | none (registry row `RendererFeatureRegistry.h:334` only) | Registry `kind=Retired`, doc: "v6 opt-in gate removed in v7.1. Gate plumbing deleted; setting this var has no effect." Confirmed by `gos_static_prop_batcher.cpp:208` comment. | 0 code LOC. Registry `Retired` row is **deliberately retained** (index stability) — leave it. | Remove from `run_smoke.py` passthrough allowlist (`smoke_allowlisted=true` = litter). Registry row stays. |
| `MC2_SHADOW_CSM_FULLMAP_LAST` (+ paired `MC2_SHADOW_CSM_NEAR_MAX`) | none (0 readers) | tier1 doc line already marks it **RETIRED** ("folded into the fixed-cascade rework"). GHOST: documented, no reader. | 0 code LOC | Remove the RETIRED line from `docs/tier1_env_vars.md` (doc litter only). |
| `MC2_TERRAIN_LOD_CHUNK_BRIDGE_OBJBLOCK` | none (0 readers) | GHOST: tier1-documented as a live feature but **no code reader** — LOD-chunk bridge shipped; reader removed. ⚠ Verify this is a removed-feature (not an accidental reader drop) before doc cleanup. | 0 code LOC | Remove tier1 doc entry after confirming the ObjBlockInfo.active bridge is unconditional now. |

**Highest confidence of the four:** `MC2_TERRAIN_ACTIVE_AB` (reader visibly a no-op warn)
and `MC2_DRAW_PACKET_STATIC_PROP_V6` (registry self-declares Retired/no-effect).

---

## §2. TRUE-DEAD — neutralized but pinned (class a, low-value)

`MC2_HDR_POST`, `MC2_BLOOM`, `MC2_TONEMAP_ACES` — the HDR-post FX stack, **deleted
2026-06-22 (DEAD-POST-FX-CLEANUP-1)** as "wrong-for-RTS." Registry entries
(`RendererFeatureRegistry.h:30/31/32` enum, rows 462/470/478) explicitly say
*"this env var now has NO effect. Entry retained to preserve registry indices."*

- Only live readers: the `MC2_LIGHTING_LINEAR_AUDIT` diagnostic print
  (`gos_postprocess.cpp:993-995`) which merely reports their string values, plus the
  registry rows and `tests/unit/test_rendercore.cpp:369-371` (enum-mapping test).
- **Recommendation: KEEP as-is.** The gate names are pinned in the enum for index
  stability; deleting them would shift `RendererFeature` indices and break the audit
  print + test. There is no runtime cost (they are inert). Retiring these buys nothing
  and risks the index churn the registry comment warns against. List for completeness;
  do **not** schedule a slice.

---

## §3. TIER_A false positives → KEEP (class e)

All 10 verified LIVE. Do not touch. The classifier flagged these on `ledger_removed`
proximity (a doc said the *feature default* was reverted, not that the gate was removed)
or on multi-line getenv assignment (`result_unused` false trip).

| Gate | Site | Why live |
|---|---|---|
| `MC2_ABL_TRACE` | `mclib/ablxstmt.cpp:128`, `gos_crashbundle.cpp:650` | Live trace gate (`s_ablTrace`); the `#if 0` readers in `ablmc2.cpp` are dead stubs but the gate itself is active. |
| `MC2_FRAME_JOBS_TRACE` | `code/frame_jobs.cpp:86-87` | Assigned to `g_trace` (multi-line `&&`); live trace. |
| `MC2_GPU_PARTICLES` | `mclib/particles/batcher.cpp:79` | Live opt-in (default CPU/MLR; `=1` opts into GPU FX). |
| `MC2_SCREENSHOT_AT_FRAME` | `gameosmain.cpp:1931-1932` | Live headless-screenshot oracle tool. |
| `MC2_SOAK_AUTOWIN` | `code/mission.cpp:644-645` (+5 more) | Live crash-soak harness gate. |
| `MC2_STATIC_PROP_LEGACY_DISPATCH` | `gos_static_prop_batcher.cpp:210-213` | Live **kill-switch** for DrawPacket v7 (default-ON feature). |
| `MC2_UI_DEFS_LOADSCREEN` | `gui/UiDefs.cpp:1748` | Live dev-iteration opt-in (loadscreen pinned legacy). |
| `MC2_UI_DEFS_RESULTS` | `gui/UiDefs.cpp:1756` | Live dev-iteration opt-in (results page pinned legacy). |
| `MC2_VFX_ORACLE_RENDER` | `mclib/particles/batcher.cpp:137` | Live opt-in (default OFF; `=1` GPU VFX route). |
| `MC2_VFX_ORACLE_TUBE` | `mclib/particles/batcher.cpp:194` | Live opt-in (default OFF; `=1` GPU tube ribbon). |

---

## §4. TIER_B — single-reader diagnostic traces (105)

All are `*_TRACE` / `*_DIAG` / `*_PROBE` / `*_DEBUG` gates with exactly one reader,
not in the registry or tier1 doc. Gated-OFF either way → low runtime risk. But per
firm/freeze + "when in doubt KEEP," **these are NOT proposed for aggressive deletion.**
Many are legitimate future-debugging affordances (e.g. `MC2_SHADOW_STATE_TRACE`,
`MC2_TERRAIN_INDIRECT_TRACE`, `MC2_MECH_LIGHT_TRACE`).

Proposed treatment: an **optional, opt-in batched strip** only for traces whose
subsystem is now frozen, decided per-subsystem by the user. Not part of the core ladder.

---

## §5. USER-DECISION list

1. **VK Tier-0 / Retired sub-gate set (class b) — BLOCKED-ON-FLIP.** Per
   `A:/Games/mc2-vk-interop-bridge/.claude/VK-FLIP-EVIDENCE-1.md`: the umbrella
   `MC2_VK_SHARED_GBUFFER` consolidation makes sub-gates (`MC2_VK_SHADOW_INTEROP`,
   `MC2_VK_TERRAIN_CSM`, `MC2_VK_TERRAIN_TIER1`, etc.) debug-overrides and parks Tier-0
   as Retired — **but only after the user's flip decision.** These gates live on the
   `vk-interop-bridge` branch (local-only, not merged to nifty), so they do **not** appear
   in nifty's scan. No retirement action on nifty; retirement is a VK-branch follow-up
   gated on the flip. **USER decides flip timing.**
2. **No DEFAULT-ON flips proposed (class d).** No TIER_A candidate is a soaked feature
   awaiting a default flip. (Firm/freeze default-ON candidates would come from a separate
   feature audit, not this dead-gate scan.)
3. **VK-branch cross-branch delta (follow-up, do NOT touch VK now).** GATE-RETIRE-1/2 create
   a delta against `A:/Games/mc2-vk-interop-bridge`, which carries the same readers plus two
   extra files not present (or differently shaped) on nifty:
   `GameOS/gameos/diagnostic_trace.cpp` and `scripts/mcp/mc2_render_state_server.py`. Those VK
   copies are NOT edited by this arc. Reconcile them as a **VK-branch follow-up scheduled
   post-flip** so the retirement lands on both branches without disturbing in-flight VK work.

---

## §6. KEEP-BY-RULING (class c)

- `MC2_FRAMEGRAPH_REORDER_SPMECH` — **REJECTED experiment, default-OFF, presence-gated**
  (`mclib/txmmgr.cpp:3444`). Ruling: parity-failed on depth-tie; gate STAYS default-OFF as
  a documented rejected-experiment marker. Scanner correctly did NOT flag it (TIER_C). Do
  not retire. Listed here so a future sweep does not re-propose it.

---

## §7. Proposed slice ladder

Each slice S-sized, single-subsystem, with an oracle. Deletion human-gated per standing rule.

**GATE-RETIRE-1 — terrain retired-warn removal.**
- Delete `MC2_TERRAIN_ACTIVE_AB` / `MC2_TERRAIN_SOLID_AB` warn block (`terrain.cpp:2306-2311`);
  remove both from `scripts/check-env-registry.sh` ALLOWLIST; **also remove the two diag-tag
  table rows in `docs/tier1_env_vars.md:489-490`** (`TERRAIN_ACTIVE_AB` / `TERRAIN_SOLID_AB`)
  — the tags describe stderr output the deleted warn block no longer emits.
- Oracle: `check-env-registry.sh` green **after** both the allowlist removal and the doc-row
  removal (documented-but-unallowlisted / allowlisted-but-undocumented drift must not appear) +
  build + tier1 smoke 5/5 (byte-identical; gate was inert). Grep proves 0 remaining readers.

**GATE-RETIRE-2 — draw-packet-v6 allowlist litter.**
- Remove `MC2_DRAW_PACKET_STATIC_PROP_V6` from `run_smoke.py` passthrough allowlist.
  Leave the registry `Retired` row (index stability).
- Oracle: `gate_status` no longer reports `smoke_allowlisted`; build + tier1 unaffected.

**GATE-RETIRE-3 — retired-gate doc litter (`docs/tier1_env_vars.md` ONLY).**
- Scope is **`docs/tier1_env_vars.md` exclusively.** Historical/spec docs (and the descriptive
  code comment at `gos_postprocess.cpp:5003-5007`, which explicitly states the vars are
  RETIRED/IGNORED and unread-envs-are-harmless — i.e. it does NOT imply the gates work)
  intentionally retain the names as context. Do not sweep them.
- Remove the RETIRED tier1 line for `MC2_SHADOW_CSM_FULLMAP_LAST` (+`_NEAR_MAX`) at `:139`.
- Remove the `MC2_TERRAIN_LOD_CHUNK_BRIDGE_OBJBLOCK` tier1 line at `:412-413` — deliberate
  Phase-4 removal per `e406bf92` (reader removed by design, not accidentally dropped), cleared
  by review.
- Oracle: doc-only; `check-env-registry.sh` + `env_var --undocumented` clean; no build needed.

(Batched: RETIRE-1..3 can land as one small commit if the user prefers; kept separate for
reviewability. TIER_B strip §4 is a deliberately separate, opt-in track — not laddered here.)

---

## Redlines

- Nothing in §2/§3/§6 is a retirement target. Only §1 (4 gates) is actionable.
- Registry `Retired` rows stay (index stability) — retirement removes *readers/doc/allowlist
  litter*, not the pinned registry names.
- **Rollback story:** each slice is one self-contained commit; rollback = `git revert <sha>`
  per slice. Because every target gate is already inert (dead reader / doc / allowlist litter),
  reverting restores the prior text with **no data or state migration** — the gates were
  no-ops in both directions.
