# CSM-VERIFY-TUNE-1 — runtime truth + stale-comment fix + proposed tuning

**Slice:** LIGHTING-STAGE1-TRIO deliverable 2 (`CSM-VERIFY-TUNE-1` in
`.claude/LIGHTING-MODERNIZATION-PROPOSAL-1.md` §3 / §7 row 1.2, `= V3 S5`).
**Status: comment fix APPLIED (2-line class, ledger-owner touch). Tuning
values in this doc are PROPOSED ONLY — not applied to any code or env default.**

## The contradiction (as stated in the proposal)

- `GameOS/gameos/gos_postprocess.cpp:37-38` (comment): *"MC2_SHADOW_CSM :
  master gate, DEFAULT OFF. OFF => legacy single dynamic map path is
  byte-identical (none of the CSM code runs)."*
- `GameOS/gameos/gos_postprocess.cpp:44-45` (code):
  ```cpp
  const char* v = getenv("MC2_SHADOW_CSM");
  return !(v && v[0] == '0' && v[1] == '\0');   // DEFAULT ON; only "0" disables
  ```

The code comment directly above the getenv already correctly said "DEFAULT
ON" — only the block header comment 7 lines up was stale. Same contradiction
duplicated at `GameOS/gameos/gos_postprocess.h:204-207` (public API doc
comment on the getter class).

## Runtime verification (this slice — not just re-reading source)

Grepped this worktree's own recent smoke artifacts (`tests/smoke/artifacts/`)
for `[CSM]` log lines, specifically looking for a run where **no**
`MC2_SHADOW_CSM*` env var appears in the run's recorded `env_gates`/`env_delta`
— i.e. a true default-config run, not an explicit opt-in.

**`gaea_peaks_01` adhoc run, `tests/smoke/artifacts/2026-07-01T19-18-22/`:**
- `manifest.json` → `env_gates` / `identity.env_delta` list: `MC2_DEBUG_STATE_DUMP`,
  `MC2_DIAGNOSTIC_TRACE_FILE`, `MC2_DIAG_TAGS`, `MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE`,
  `MC2_GPU_CULL_STATIC_FROZEN_RECORDS`, `MC2_STATIC_POP_SPLIT`,
  `MC2_TERRAIN_VISUAL_DISPLACE` — **no `MC2_SHADOW_CSM` anywhere in the list.**
- `gaea_peaks_01.log` (same run):
  ```
  [CSM] init nearLayers=2 arraySize=8192 fullMapSize=4096 separate=1
  [CSM] layers=3 casters_per_layer=0
  ```
- Result: `PASS`.

**Populated-scene confirmation** — `mc2_01`/`mc2_24` tier1 runs (same artifact
generation, `2026-07-01T19-15-08` etc.), also with no `MC2_SHADOW_CSM*` in
the recorded env, show non-zero caster counts, proving CSM is not just
initializing but actively shadow-casting real scene content:
```
mc2_01.log:  [CSM] init nearLayers=2 arraySize=8192 fullMapSize=4096 separate=1
mc2_01.log:  [CSM] layers=3 casters_per_layer=811
mc2_24.log:  [CSM] init nearLayers=2 arraySize=8192 fullMapSize=4096 separate=1
mc2_24.log:  [CSM] layers=3 casters_per_layer=1126
```

**Verdict: CONFIRMED.** CSM is default-ON and actively running (3 cascades,
8192 near-array, 4096 full-map, real caster population) in an unmodified
default smoke, matching `docs/tier1_env_vars.md:103` ("Default **ON**
(2026-06-18, `8ff13a36`)") and contradicting only the two stale inline
comments, both now fixed by this slice.

`gaea_peaks_01` showing `casters_per_layer=0` is expected, not a red flag:
it's a generated-terrain showcase mission with no populated mech/vehicle
roster in this run window — CSM still initializes correctly, it simply has
nothing dynamic to cast yet at that point in the 30s capture.

## Fixes applied (this slice)

1. `GameOS/gameos/gos_postprocess.cpp:37-40` — header comment corrected to
   state default-ON, cites `8ff13a36` and `docs/tier1_env_vars.md`.
2. `GameOS/gameos/gos_postprocess.h:204-207` — getter-block comment corrected
   to match.
3. `.claude/STALE-COMMENTS-LEDGER-1.md` rows #1/#2 marked RESOLVED (this
   worktree only — the ledger's canonical copy lives in the nifty worktree
   per its own header; this worktree's copy is the one this slice owns and
   touches per the task brief).

No behavior change: both edits are comment-only, verified by inspection (no
non-comment lines touched).

## Proposed tuning values (NOT APPLIED — analysis only, per task scope)

Current defaults (`gos_postprocess.cpp:61-162`): `MC2_SHADOW_CSM_COUNT=3`,
`MC2_SHADOW_CSM_LAMBDA=0.5`, `MC2_SHADOW_MAP_SIZE=8192`, `MC2_SHADOW_CSM_R0=512`,
`MC2_SHADOW_CSM_R1=4096`, `MC2_SHADOW_CSM_SOFTNESS=0.9`.

These defaults are already reasonably tuned for RTS-scale (per the inline
comment: R0=512 gives 0.25 WU/texel at 8192, a fairly tight near cascade).
The proposal's ask is "confirm engagement per-mission, tune R0/R1/lambda for
RTS altitude, confirm cascade-boundary stability" — below is the analysis,
staged as a proposal for a follow-up slice to A/B, not shipped here:

| Var | Current default | Proposed A/B candidate | Rationale |
|---|---|---|---|
| `MC2_SHADOW_CSM_R0` (near radius, WU) | 512 | try 384 for the most zoomed-in cutscene camera states, keep 512 for standard RTS top-down | Near cascade texel density scales inversely with R0; a smaller R0 sharpens close-up mech shadows (helps the "MW5 close-up gap" the proposal names in §3) at the cost of the near/mid cascade boundary sitting closer to camera — needs the boundary-stability check below before shipping. |
| `MC2_SHADOW_CSM_R1` (mid radius, WU) | 4096 | keep 4096; only revisit if R0 changes meaningfully (R1 should stay a clean multiple of R0 to keep the log/uniform blend at `MC2_SHADOW_CSM_LAMBDA=0.5` well-behaved) | No evidence in this pass that R1 is wrong; it's the far-field "before full-map takes over" radius and 8x the near radius is a conventional split. |
| `MC2_SHADOW_CSM_LAMBDA` | 0.5 | no change proposed | 0.5 (even log/uniform blend) is the standard default; nothing in the smoke evidence suggests cascade popping or boundary artifacts at this value. Would need a dedicated visual A/B (not achievable from log data alone) before recommending a shift either direction. |
| `MC2_SHADOW_CSM_SOFTNESS` | 0.9 | no change proposed for the general case; §3's own ask (PCSS-lite on cascade 0 only, cutscene/close-camera-only) is the correct scoped answer rather than a global softness bump | Global softness increases blur cost on every cascade every frame; the proposal already identifies the right scoped fix (cascade-0-only PCSS-lite gated to close camera states) rather than a blanket default change. |
| `MC2_SHADOW_MAP_SIZE` | 8192 | no change proposed | 8192 costs ~805MB VRAM for 3 layers (per the inline comment) but the smoke evidence shows no VRAM-pressure symptoms in tier1/gaea_peaks_01 runs (`crash_evidence.json` present only for the unrelated `gaea_mountain_01` run, not tied to shadow VRAM per that run's own log). Not worth trading resolution for VRAM headroom without a demonstrated budget problem. |

**Why proposal-only, not applied:** none of the smoke artifacts inspected in
this pass contain a *visual* comparison (pixel/screenshot) of cascade-boundary
stability or close-up penumbra quality — only `[CSM]` init/caster-count log
lines and PASS/FAIL verdicts. Tuning R0/lambda without a visual A/B risks
exactly the class of regression the proposal's own §7 gate table guards
against ("every flip needs full-mission A/B via MC2-VERIFY"). The
recommendation is to open a follow-up slice that runs the canonical tier1
smoke with each candidate R0 value + a manual/`MC2-VERIFY` screenshot compare
at a close mech-cam state, rather than ship a numeric change from log
evidence alone.

## Blob/SSAO interplay (per proposal §3, not built this slice)

Not evaluated in this pass: `MC2_GROUND_CONTACT_BLOB` (shipped, gate exists)
and SSAO (`MC2_SSAO`, gated OFF per proposal §0 table) interplay with CSM
tuning is explicitly out of scope for a data/docs-only slice — flagged here
only so the follow-up tuning slice doesn't forget the proposal's own
dependency note ("blob/SSAO interplay" in the roadmap row for this slice).
