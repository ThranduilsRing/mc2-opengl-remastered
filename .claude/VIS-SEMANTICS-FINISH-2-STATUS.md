# VIS-SEMANTICS-FINISH-2 — STATUS

Lane goal: make `MC2_VIS_SEMANTICS` real. Audit
`.claude/SESSION-AUDIT-2026-07-25.md` §3a found the ghost-cone leak is **default
behaviour in every binary that exists**, the replacement was never compiled, and
the stale-emitter guard was left behind on a parked gate.

Commit: **`ceb5bb06`** on `claude/nifty-mendeleev`.

---

## 1. DOES IT COMPILE, AND IS IT IN THE BINARY?

**Yes to both — verified by building, not by reading.**

The `mclib/mech3d.h` breakage reported in the audit does **not** exist at
`ce68d5e4`. A full `--target mc2` build of the tree as-found succeeded (exit 0)
before any edit of mine, and a string-grep of the resulting exe found
`MC2_VIS_SEMANTICS` present ×3. The 12:53-vs-12:55 exe/commit inversion the
audit spotted was real, but the underlying compile error was already resolved by
`ce68d5e4` (FORCE-ALL-VISIBLE-1); nobody had rebuilt to notice.

### Build attribution chain (the check that would have caught the original failure)

| step | evidence |
|---|---|
| clean worktree | `A:/Games/mc2-vissem2-clean` detached at `ceb5bb06440d`, `git status --porcelain -uno` empty |
| fingerprint | `MC2_BUILD_GIT_SHA "ceb5bb06440d"` · **`MC2_BUILD_GIT_DIRTY 0`** |
| deploy | `deploy_payload.py … --require-clean` → `manifest written … src_commit ceb5bb06440d` (804 rows) |
| md5 | build `d4704bff640165ab25aa74bdff59504a` == deployed `d4704bff640165ab25aa74bdff59504a` |
| verify | `--verify-only --strict` → `804 match, 0 stale, 0 missing` |
| **gate in the deployed exe** | `MC2_VIS_SEMANTICS` ×3 · `MC2_FORCE_ALL_VISIBLE` ×1 · `VIS-SEM v1` ×1 · `CONE_VIS_GATE v1` ×1 · literal `ceb5bb06440d` ×2 |

A separate clean worktree was required because `GameOS/gameos/gos_terrain_indirect.cpp`
is held uncommitted by the live **CEMENT-UNIFORM-2** lane; a build in
`A:/Games/mc2-main` would have been fingerprinted `-dirty` and therefore
unattributable. That file was never touched.

---

## 2. THE FIX

`3ab33aba` re-armed only the **alpha==0** half of job (a). The other half — the
stale-emitter turn guard in `QueueConeGlow` (`mclib/cone_glow.cpp`), whose own
comment describes the user's symptom verbatim (*"a lit beam hanging in mid-air
with no emitter under it"*) — still consulted `vis_gate_enabled()`, i.e. the
`MC2_LIGHT_CONE_VIS_GATE` that `89f0ed4f` **parked default-OFF**. So a culled,
LOD-dropped or off-map-staged mover still emitted its cone with
`MC2_VIS_SEMANTICS` on.

Both job-(a) guards now arm off one shared predicate:

```cpp
static bool vis_job_a_armed()
{
    if (mc2_vis::force_all_visible()) return false;
    return mc2_vis::enabled() || vis_gate_enabled();
}
```

Why this cannot re-create the regression that got the gates parked: the turn
guard is a **per-frame data-freshness test** (`rec->node->GetLastTurnTransformed()
!= turn`), not a fog-of-war test. It fires only when the emitter's own leaf was
never transformed this frame — i.e. the mesh is not on screen at all. The
regression came from the *sensorLevel fairness* predicate, which lives in
`mc2_vis::emitsLight()` and exempts friendlies outright via
`homeTeamRelationship` (0/1), a field written unconditionally every frame,
unlike `sensorLevel`, which is written only inside the `isOnGui` arm and goes
stale. See `.claude/VISIBILITY-SEMANTICS-1-STATUS.md` §1 for the measured proof
that no friendly loses emission.

Also corrected: comments in `vis_semantics.{h,cpp}` claiming the legacy gates
could switch the module off, and that `=0` routes through `mover_model_shown()`.
Neither was true. `mover_model_shown()` now has no live callers.

---

## 3. VERIFICATION

All runs: `MC2_FORCE_ALL_VISIBLE=0` **explicitly set** (see §0 of the sibling
doc — the smoke runner defaults it to 1 and blinds this entire bug class).
Deployed clean exe `ceb5bb06440d`.

### 3.1 Smoke — 5/5 tier1 PASS

mc2_01 / mc2_03 / mc2_10 / mc2_17 / mc2_24, 15 s each, `--keep-logs`, all PASS,
Δdestroys +0, 137–141 avg FPS.

### 3.2 The stale-emitter guard fires — direct A/B

`MC2_LIGHT_CONE_DIAG=1`, mc2_10, identical exe, only `MC2_VIS_SEMANTICS` differs:

| arm | `[CONE_VIS_GATE v1] event=reject_stale` | final reject counter |
|---|---:|---:|
| `MC2_VIS_SEMANTICS=1` (default, this fix) | fires | **721** in 15 s live play; **4801** / **5041** over the bookmark sweeps |
| `MC2_VIS_SEMANTICS=0` (today's shipped behaviour) | **0** | — |

Rejected emitters report `lastTurnTransformed=0 turn=238` — never transformed at
all, i.e. off-map-staged or unspawned units emitting cones. Exactly the class the
audit said was left behind.

### 3.3 THE FROZEN-BOOKMARK TRAP, REPRODUCED

The `tests/visual/bookmarks/mc2_10.json` set (`overview_center`,
`ridge_lowangle`, `highangle_wide`) produced **byte-identical frames in both
arms** — while the arm-1 log recorded **4801 stale-emitter rejects**. Inspection
shows why: those three poses are self-described "S9 v1 PROVISIONAL" map-centered
poses that frame **bare terrain with no unit and no structure in shot**.

This is how GHOST-CONE-VERIFY-4 "passed verification at four saved poses". The
poses are not merely unlucky — for mc2_10 they are useless. **Do not grade this
mission on `tests/visual/bookmarks/mc2_10.json`.**

The verdict set below therefore uses the user's own saved poses from
`scratch/allposes/bm/mc2_10.json` (read-only; that directory belongs to
REVIEW-ALLPOSES-1).

### 3.4 Direction A — no cone/pool from hidden, culled or off-map units

Full-frame, night, normal gameplay altitude, 14 user poses, A/B on the same exe.
**13 of 14 poses changed.** Artifacts:
`scratch/vissem2/caps/user_arm1/` vs `user_arm0/`.

| pose | changed px % | mean luma Δ (arm1−arm0) |
|---|---:|---:|
| saved_204937 | 28.79 | −8.20 |
| saved_204956 / _204958 | 15.30 | −3.94 |
| saved_212258 | 8.09 | −3.22 |
| saved_204753 | 6.77 | −0.62 |
| **saved_212019** — *the user's reported ghost-cone pose* | **5.82** | **−2.25** |
| saved_204907 | 2.29 | −0.34 |
| saved_192540 | 2.18 | −0.75 |
| saved_204523 | 1.83 | −0.60 |
| saved_204611 | 1.05 | −0.22 |
| lance_close / 205952 / 125552 | ≤0.36 | ≈0 |
| **saved_212140** | **0.000** | **0.0000** — natural control pose, no ghost emitter in view |

**Verdict shot — mc2_10 @ 212019, wide, night, gameplay altitude.** In arm 0 a
bright pale wedge hangs between the two towers in the lower-centre valley with
**no emitter mesh under it**. In arm 1 the wedge is gone; the towers, their
ground pools, the tree line and the lit compound on the ridge are all unchanged.
PASS.

### 3.5 Direction B — friendly units and building lamps stay lit and emitting

* **Measured**: all 1537 player-owned / allied / full-contact trace samples emit
  under the new predicate. Zero losses. (Sibling doc §1.)
* **Visual, wide airbase overview @ 204907**: the compound, tower lamps, runway
  and taxiway light strings and their ground glow are present and unchanged in
  both arms. Buildings are exempt by construction (`ownerIsMover=false`,
  `alphaValue 0xff`) and the pixels agree.
* **Control missions** — bookmark A/B, mc2_01 / mc2_03 / mc2_17 / mc2_24:

  | mission | poses byte-identical |
  |---|---|
  | mc2_03 | 3/3 |
  | mc2_17 | 3/3 |
  | mc2_24 | 3/3 |
  | mc2_01 | 9/12; the 3 differing poses change **0.000 % / 0.001 % / 0.010 %** of pixels (5 and 28 pixels respectively) — sub-noise, and attributable to the pre-existing `cone_geom_hidden_by_alpha` path from `3ab33aba`, not to this lane's turn guard |

* **Nothing is added, only removed**: across the 14 user poses, 8 have a global
  max brightening of 0. The 6 that brighten (max +75) are movers at
  `meshAlpha != 0` that the *armed-legacy* predicate suppressed and the new rule
  correctly lets emit ("emission follows the mesh exactly"). Against the
  *shipped-parked* baseline — which emits for everything — the user-visible delta
  is removal only.

---

## 4. FINAL GATE DEFAULTS

| gate | default | state |
|---|---|---|
| `MC2_VIS_SEMANTICS` | **ON** | armed; owns jobs (a)–(d) for movers; `=0` selects the armed-legacy sensorLevel predicate |
| `MC2_LIGHT_CONE_VIS_GATE` | OFF (parked, `89f0ed4f`) | superseded; `=1` still arms job (a) independently |
| `MC2_LIGHT_MOVER_VIS_GATE` | OFF (parked, `89f0ed4f`) | superseded; `mover_model_shown()` has no live callers |
| `MC2_FORCE_ALL_VISIBLE` | engine OFF; **runner forces 1** | diagnostic; disarms everything above |
| `MC2_VIS_SEMANTICS_TRACE` | OFF | measurement rig |
| `MC2_LIGHT_CONE_DIAG` | OFF | prints `event=reject_stale` |

No gate default was changed by this lane. `MC2_VIS_SEMANTICS` was already
default-ON in source; this lane made it reach the binary and made it cover the
half of job (a) it was missing.

---

## 5. OPEN / HANDED OFF

1. **`MC2_VIS_SEMANTICS` and `MC2_LIGHT_CONE_DIAG` are UNDOCUMENTED** —
   `gate_status` reports no `docs/tier1_env_vars.md` entry for either. Not fixed
   here: that file is claimed by two live lanes (EDGE-LIGHT-EMISSION-1,
   FORCE-ALL-VISIBLE-1). Whoever lands last should add both rows.
2. **`tests/visual/bookmarks/mc2_10.json` is unusable** (§3.3) — three provisional
   poses framing empty terrain. It should be re-authored from real in-game poses
   or deleted; leaving it invites another false PASS.
3. **`mclib/mech3d.cpp:4436`** still carries `visible && (sensorLevel > 4)` on the
   foot-poof / non-weapon-effects block — the same dead-for-vehicles,
   stale-for-home-mechs pattern, in a non-lighting path. Left alone: changing it
   alters visible gameplay FX beyond this lane's scope.
4. **Blip-only (`sensorLevel` 1..4) was never observed** in a 15 s window; that
   row of the measurement table is sourced, not measured.
5. The user's `HKCU:\Environment` pollution (audit §3e) is real and was observed
   live — the runner printed three `[ENV-DROP] WARNING` lines for
   `MC2_STATIC_POP_SPLIT`, `MC2_GPU_CULL_STATIC_FROZEN_RECORDS`,
   `MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE`. Because they are *not* in the smoke
   allowlist they were dropped, so the runs in this document are uncontaminated —
   but an Explorer double-click still inherits all three.
