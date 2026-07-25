# SESSION GATE INVENTORY — 2026-07-24

Lane: `WORKTREE-HYGIENE-1`. Branch `claude/nifty-mendeleev`, verified at commit
`56e7a4d9` (clean tree, `MC2_BUILD_GIT_DIRTY 0`).

Every `MC2_*` env gate this session added or changed, with its **verified**
default read from the C++ reader (not from a status doc), its registry /
docs / smoke-allowlist state, and a default-ON safety call.

**How to read the DEFAULT column** — defaults were confirmed against the reader
source, because several status docs disagreed with the code:
- `if (!e || e[0] != '1') return false;` → **OFF**
- `return !(v && v[0] == '0');` → **ON** (killswitch shape)

---

## 1. Gates currently default-ON (3 — was 4)

> **AUDIT-TRUTH-1 (2026-07-25) update:** `MC2_LIGHT_BILLBOARD_TWOSIDED` was retired
> to default-**OFF** and is no longer a default-ON gate. Its row is kept below for
> the audit trail. Defaults in this file were re-verified against reader source on
> 2026-07-25; rows changed by AUDIT-TRUTH-1 are marked inline.

These ship enabled **today**. If the user disagrees with any, it needs a code
change, not just an approval.

| Gate | What it fixes | Default | Registered | Documented | Smoke-allowlisted | Default-ON safe? |
|---|---|---|---|---|---|---|
| `MC2_LIGHT_BILLBOARD_TWOSIDED` | Renders beacon / runway edge-light glow cards two-sided (kills a legacy backface cull that hid them from one side) | **OFF** (retired from ON by AUDIT-TRUTH-1) | no | yes | yes | ✅ **RESOLVED — retired to default-OFF.** Was default-ON while non-stock, never user-approved and called falsified/near-inert by its own successor lane. OFF == stock legacy cull. Kept as opt-in lever. |
| `MC2_LIGHT_MOVER_VIS_GATE` | Gates mover light-model emission to the sensor-level fairness rule (`=0` restores legacy always-emit) | **ON** | no | **no** | yes | Likely yes — correctness fix (stops seeing unsensed movers by their lights). Unregistered + undocumented. |
| `MC2_LIGHT_CONE_VIS_GATE` | Same visibility predicate applied to light-cone emission | **ON** | no | **no** | yes | Likely yes — same correctness class as above. Unregistered + undocumented. |
| `MC2_LETTERBOX_OPAQUE` | Letterbox bars stay fully opaque black through the cinematic swoop-in instead of ramping alpha | **ON** | no | **no** | **no** | Likely yes — cosmetic, restores intended cinematic framing. Unregistered, undocumented, and **not smoke-allowlisted**. |

**Note A — `MC2_LIGHT_BILLBOARD_TWOSIDED`: RESOLVED 2026-07-25 (AUDIT-TRUTH-1).**
It *was* default-ON despite a **marker-light regression** attributed to the
two-sided treatment, with follow-up work moved to `MC2_LIGHT_BILLBOARD_FACING`
(deliberately default-**OFF**). It was non-stock, never user-approved, and its
own successor lane called it falsified and near-inert. It is now **default-OFF**,
which is exactly the stock/legacy backface cull — the gate only ever *added*
faces, so OFF cannot remove anything stock drew. Kept as an opt-in lever rather
than deleted (two call sites consume it; it is the only way to A/B the
hypothesis). The GPU-batcher two-sided path remains a *user-present follow-up*
(master-FAIL item #8, partial).

---

## 2. Gates default-OFF, awaiting a promote/hold decision (19)

> **AUDIT-TRUTH-1 (2026-07-25):** two rows below are no longer "awaiting a
> decision" — `MC2_WATER_SHORE_MEANDER` now defaults to the user-approved **75**,
> and `MC2_CEMENT_TONE_MATCH` is **user-REJECTED**, not a candidate.

| Gate | What it fixes | Default | Registered | Documented | Smoke-allowlisted | Default-ON safe? |
|---|---|---|---|---|---|---|
| `MC2_WATER_SHORE_MEANDER` | Organic coastline via shore-contour meander (cures the straight/staircase shoreline) — `4be51440` | **75** (user-approved default, AUDIT-TRUTH-1; was 0.0 = off) | no | yes | yes | ✅ **BAKED IN** — user approved A=75; it previously shipped disabled with the approval living only in a status doc. Explicit `=0` restores the staircase. Watch: mc2_01 waterline creeps ~10% up the beach. |
| `MC2_WATER_SHORE_FEATHER` | **SUPERSEDED** by MEANDER. Widened water-edge feather band | OFF | no | yes | yes | **No — leave OFF, superseded** |
| `MC2_RUNWAY_EDGE_BLEND` | Feathers the hard cement/runway-pad edge by **dilating** concrete outward — `8e59de64`, reach knob `3c6aad71` | OFF | yes | yes | yes | Candidate — terrain-indirect path, rev-2 crops in REVIEW pack |
| `MC2_MOVER_CONTACT_SHADOW` | Altitude-faded contact shadow under movers / airborne helos — `9590805f` | OFF (strength 0..1) | no | **no** | yes | Candidate — grounding fix; unregistered + undocumented |
| `MC2_LIGHT_BILLBOARD_FACING` | Camera-facing billboard treatment for glow cards on the GPU batcher path (`=2` exaggerates) | OFF | no | yes | yes | Hold — supersedes TWOSIDED work but not user-verified |
| `MC2_NIGHT_FOG_DIM` | **The black-band root cause**: edge/OOB fog was never night-dimmed, so it stayed bright against a dark HDRI sky — `16c2244a` | OFF | yes | yes | yes | **Strong candidate** — this is a genuine bug fix, 3 rival theories disproven |
| `MC2_NIGHT_FOG_DIM_SCALE` | Per-channel fog-colour multiplier under NIGHT_FOG_DIM (default 0.35) | OFF (tunable) | yes | yes | yes | Tunable — ships with NIGHT_FOG_DIM |
| `MC2_FAR_BACKDROP_LIT` | Night-shades far/impostor static-prop LODs that lack a per-vertex night light bake | OFF | yes | yes | yes | Candidate — fixes bright far props at night |
| `MC2_LIGHT_REALISM` | Feathered translucent light cones + emitter-continuous light pools — `45058613`, `db625a12`, `cea05a2d` | OFF (gate) — but its six constants are now **variant B**, the arm the user approved (AUDIT-TRUTH-1) | no | **no** | yes | Hold — flipping ON costs −34% avg night FPS, which is the user's call. Constants no longer misreport: enabling it now yields variant B (0.40/2.5/2.2/0.0/70/40), not variant A. |
| `MC2_LIGHT_POOL_MAX` | Raises the terrain pool-light cap 64 → 256 — `5213d257` | OFF (built-in cap already 256; env overrides 1–4096) | no | **no** | yes | Already effective — the 256 cap is compiled in; the env only overrides. Unregistered + undocumented |
| `MC2_CEMENT_TONE_MATCH` | Normalizes cement-catalog cell tone; *attempts* the concrete patchwork — `7be02947` | OFF | yes | yes | yes | ⛔ **REJECTED BY USER 2026-07-25** — "after is WAY worse"; ON arm flattens authored material variation into a pale wash. **Not a promotion candidate. Keep OFF.** |
| `MC2_CEMENT_TRANSITION_FALLBACK` | Fills dropped road/transition cement tiles from an adjacent baked neighbour (missing road swaths) — `8d68684c` | OFF | yes | yes | yes | Candidate |
| `MC2_CEMENT_ATLAS_MAGENTA_KEY` | Colour-keys leftover magenta texels out of solid cement atlas cells | OFF | yes | yes | yes | Candidate |
| `MC2_PROP_SHADOW_RECEIVE` | Static props receive the dynamic CSM cascade (cures flat/cardboard props) — doc `1f7f81de` | OFF | yes | yes | yes | Candidate — root-caused + remedy verified, perf cost unmeasured |
| `MC2_SSAO` | Half-res screen-space AO grounding pass before tonemap | OFF | yes | yes | yes | Hold — perf cost, broad visual drift |
| `MC2_LEGACY_LIT_MC2_FRAME` | Coordinate-frame fix so ground vehicles receive point/spot lights at night — `b01a7a96` | OFF | yes | yes | **no** | Hold — `da3a5b78` records the hypothesis as **disproven**, vehicle bug still open |
| `MC2_GV_LIGHTSOUT_CLEAR` | Adds the missing else-branch clearing the GV `SetLightsOut` latch on repair/recycle — `7c059585` | OFF | yes | yes | **no** | Candidate — narrow, obviously-correct latch fix; not smoke-allowlisted |
| `MC2_NO_CURSOR_GRAB` | Suppresses all OS cursor manipulation for smoke/automation — `d0f1f0e2`, `183928c6` | OFF | yes (kind=Trace) | yes | **no** | **Keep OFF for players** — harness-only. Intentional. |
| `MC2_TERRAIN_LOD_CHUNK_DIAG` | Bitmask frag-viz diagnostics for the terrain LOD chunk shader | OFF (`0`) | yes | yes | yes | **Keep OFF — INTENTIONAL diag**, registered and zero-cost when 0. Do not remove. |

---

## 3. Undeclared tunables shipped this session (7) — needs cleanup

These are read by code but appear in **neither** the renderer feature registry
**nor** `docs/tier1_env_vars.md`. They are smoke-allowlisted, so they work, but
`scripts/check-env-registry.sh` conventions treat unregistered gates as drift.

| Gate | Sibling of | Registered | Documented |
|---|---|---|---|
| `MC2_LIGHT_REALISM_BEACON` | `MC2_LIGHT_REALISM` | no | no |
| `MC2_LIGHT_REALISM_BEACON_RADIUS` | `MC2_LIGHT_REALISM` | no | no |
| `MC2_LIGHT_REALISM_CONE_FEATHER` | `MC2_LIGHT_REALISM` | no | no |
| `MC2_LIGHT_REALISM_CONE_OPACITY` | `MC2_LIGHT_REALISM` | no | no |
| `MC2_LIGHT_REALISM_POOL_CORE` | `MC2_LIGHT_REALISM` | no | no |
| `MC2_LIGHT_REALISM_POOL_SPILL` | `MC2_LIGHT_REALISM` | no | no |
| `MC2_WATER_SHORE_MEANDER_FREQ` | `MC2_WATER_SHORE_MEANDER` | no | yes (shared entry) |

---

## 4. Flagged problems

1. **Unregistered + undocumented** (6 gates): `MC2_MOVER_CONTACT_SHADOW`,
   `MC2_LIGHT_MOVER_VIS_GATE`, `MC2_LIGHT_CONE_VIS_GATE`,
   `MC2_LETTERBOX_OPAQUE`, `MC2_LIGHT_REALISM`, `MC2_LIGHT_POOL_MAX`.
   Two of these (`*_VIS_GATE`, `LETTERBOX_OPAQUE`) are **default-ON** — they
   change shipped behaviour with no registry or docs entry at all.
2. **Not smoke-allowlisted** (4 gates): `MC2_LETTERBOX_OPAQUE`,
   `MC2_LEGACY_LIT_MC2_FRAME`, `MC2_GV_LIGHTSOUT_CLEAR`, `MC2_NO_CURSOR_GRAB`.
   `run_smoke.py` will **drop these from the child env**, so any gate-ON smoke
   of them silently ran gate-OFF. Any "verified via smoke" claim for
   `MC2_LEGACY_LIT_MC2_FRAME` or `MC2_GV_LIGHTSOUT_CLEAR` is **not valid**.
3. ~~**Contested default**: `MC2_LIGHT_BILLBOARD_TWOSIDED` is default-ON despite an
   attributed marker-light regression and superseding `..._FACING` work.~~
   **RESOLVED 2026-07-25 (AUDIT-TRUTH-1)** — retired to default-OFF. See note A.
4. **Undeclared tunables**: the 7 in section 3.

---

## 5. Verification basis

- Defaults read from reader source, file:line captured per gate.
- Registry state cross-checked against `RendererFeatureRegistry.h`.
- Docs state cross-checked against `docs/tier1_env_vars.md`.
- Allowlist state cross-checked against the `scripts/run_smoke.py` passthrough.
- Tree clean at `56e7a4d9`; exe + manifest fingerprint verified `OK` on the
  `0.4` lane before the tier1 run that backs this document.
