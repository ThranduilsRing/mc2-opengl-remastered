# GHOST-CONE-VERIFY-4 — status

Lane: GHOST-CONE-VERIFY-4 · worktree `A:/Games/mc2-main` · branch `claude/nifty-mendeleev`
Deploy/test lane: `A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1`
Verdict: **STOOD DOWN — gates PARKED default-OFF, handed to VISIBILITY-SEMANTICS-1**

---

## TL;DR

1. The user's graded images were **not** stale in the sense of "predate the fix" —
   but they were captured from **dirty, unattributable builds**. On a clean
   attributable build (`11ec0d5a`) all four graded poses are **free of ghost
   mover cones**.
2. The user is still right that the bug exists. I found the emission site the
   previous lane missed: the cone **GEOMETRY** (the drawn wedge mesh), which is
   a third, independent path from the pool light and the additive glow.
3. Fixing it default-ON made things worse in the other direction (reported:
   "one of the smokes now has no mech lights on").
4. Per the user's call — *"that's why you can't possibly fix it in that lane"* —
   both gates are now **parked default-OFF** and the problem is handed to
   **VISIBILITY-SEMANTICS-1**, which is already live in this worktree.

---

## 1. Were the user's images stale?

Partly. The distinction matters:

| build | src_commit | has GHOST-LIGHT-VISIBILITY-1 fix? | attributable? |
|---|---|---|---|
| `mc2-win64-v0.5.1` | `fa3a5304` | **no** (pre-fix) | clean |
| `mc2-win64-v0.4d-rc1` (as found) | `24f8daea`**-dirty** | yes | **no** |
| `mc2-win64-shoreline-test` | `24f8daea`**-dirty** | yes | **no** |
| `mc2-win64-issuefix-test` | `f412423e`**-dirty** | ? | **no** |
| `mc2-win64-deferred-test` | `25aa6180`**-dirty** | ? | **no** |

The REVIEW pack section `light_realism_1` was captured 2026-07-24 19:47–19:48,
*after* the fix commits (18:46–18:57) — so those crops did include the fix. But
every candidate exe from that window is `-dirty`, i.e. built from a tree
carrying other lanes' in-progress edits. Its exact source state is not
reproducible, so nothing graded from it is safely attributable.

Note also that the review-pack region **named** `ghost_wedge_over_road` is a
**stale label** carried over from the pre-fix lane — the region is clean in the
image. Grading by folder name would mislead.

### Clean-build re-capture (the actual answer)

Built + deployed `11ec0d5a` with `--require-clean`, manifest `src_commit
11ec0d5ad212`, **no `-dirty` suffix**, exe fingerprint matched. Captured all
four poses (`--trigger-frame 431 --settle 30`, deterministic).

| pose | verdict on the clean build |
|---|---|
| **204753** (airbase wide) | CLEAN — no cone/glow/pool without an emitter |
| **212019** (compound) | CLEAN — only the guard-tower lamps (static, legitimate) |
| **204907** (airbase oblique) | CLEAN |
| **205952** | CLEAN |

Crops: `scratch/gcv4/CLEAN_<pose>/` (full-res) and `scratch/gcv4/half_<pose>.png`.

**So at these four frozen bookmark poses the shipped fix does hold.** That is
exactly why the bug kept surviving "verification" while the user kept seeing it
in live play — see §2.

---

## 2. What the earlier lane missed — cone GEOMETRY (hypothesis (b), confirmed)

There are **three independent** mover night-light emissions. GHOST-LIGHT-VISIBILITY-1
gated two of them and left the third wide open:

| # | emission | gated by prior lane? |
|---|---|---|
| 1 | terrain **pool / spot light** | yes — `mover_model_shown(sensorLevel)` |
| 2 | additive **cone glow** billboard | yes — inherits (1) via `if (active)` |
| 3 | **cone GEOMETRY** — the drawn wedge mesh | **NO** |

### Root cause of the geometry leak

A sensor-blip-only enemy is **not** hidden via the appearance `visible` flag.
`code/mech.cpp:6690-6755` and `code/gvehicl.cpp:3775-3830` call
`setVisibility(true,true)` on **every** contact branch and hide the model by
**fading `alphaValue` to 0**.

Ordinary leaves honour that — `TG_Shape::Render` and `MultiTransformShape`
route through `MC2_DRAWALPHA` whenever `alphaValue != 0xff`. But the
`isSpotlight` branch is tested **first** and emits at hardcoded `0xffffffff`
opacity:

- `mclib/tgl.cpp:2787` — `if (isSpotlight) mcTextureManager->addTriangle(0xffffffff, MC2_ISSPOTLGT);`
- `mclib/tgl.cpp:3363` — `if (isSpotlight) mcTextureManager->addVertices(0xffffffff, gVertex, MC2_ISSPOTLGT);`

So the searchlight wedge of a fully-faded, invisible mover kept drawing at full
brightness. **That is the user's "light cones from invisible vehicles".**

Critically, **vehicles never submit through the GPU static-prop or mech
batchers** (both of which strip `SpotLight_` leaves —
`gos_mech_batcher.cpp:1424`, `gos_static_prop_batcher.cpp:5246` / `:2507`), so
this legacy CPU leaf draw is the **default and only** path for every vehicle /
helicopter / VTOL / elemental cone.

Why the bookmark poses did not show it: at those four frozen camera positions no
enemy mover happens to be mid-fade in view. The leak is reachable in live play,
not in the frozen pose set. **Any successor verification must not rely on these
four poses alone.**

### The fix I landed (now parked)

`7cd8dd13` — `cone_geom_hidden_by_alpha(alphaValue)`, consulted from the three
`isSpotlight && !isNight` early-outs in `mclib/tgl.cpp`
(`MultiTransformShape`, `MultiTransformShape_PositionsOnly`, `Render` — all
three have `alphaValue` in scope). Predicate is **alpha-based, not
sensorLevel-based**, because `tgl.cpp` has no `sensorLevel` and alpha is the
actual mechanism that hides the model; it also covers contact fade-in/fade-out
transitions for free. Strict `== 0` leaves every static building lamp
(`alphaValue 0xff`) untouched.

**I still believe this specific predicate is safe for player-owned units**
(a player mech is at `alphaValue 0xff`, never 0) — it is the one piece of this
lane worth re-examining first. But it is gated under
`MC2_LIGHT_CONE_VIS_GATE`, so parking that gate parks it too. Deliberate: the
successor should re-arm and A/B it on its own terms rather than inherit an
armed default from a lane that got stood down.

---

## 3. The regression that caused the stand-down

User, watching smoke runs: *"oh one of the smokes now has no mech lights on."*
Over-suppression — the opposite failure of the ghost-cone bug.

I did **not** get to isolate which tier1 mission, and I did not measure a
player mech's live `sensorLevel`. What I did establish from source, and what
almost certainly explains it:

**`setSensorLevel(5)` is reached only inside `if (isOnGui)`** —
`code/mech.cpp:6745-6752`, `code/gvehicl.cpp:3823-3830`:

```
else
{
    if (isOnGui)
    {
        alphaValue = 0xff;
        appearance->setSensorLevel(5);
        appearance->setAlphaValue(alphaValue);
    }
}
```

If a unit is neither an enemy contact nor `isOnGui`, **nothing is written** and
`sensorLevel` keeps its previous value (zero-initialised at
`mech3d.cpp:1444` / `gvactor.cpp:847`). The `sensorLevel == 0` arm of
`mover_model_shown` was presumably meant to cover exactly this — but the whole
scheme depends on a field that is only conditionally written, which is fragile
in precisely the way a smoke run would expose.

**Unmeasured and needed by the successor:** what `sensorLevel` a player-owned
mech actually reports, per mission, at the moment its lights are expected.

---

## 4. Parking (what is in the tree now)

`89f0ed4f` — both gates flipped **default-ON → default-OFF**:

| gate | default now | `=1` re-arms |
|---|---|---|
| `MC2_LIGHT_MOVER_VIS_GATE` | **OFF** | mover pool light + cone glow suppressed for blip-only enemies |
| `MC2_LIGHT_CONE_VIS_GATE` | **OFF** | cone-glow staleness guard **and** the alpha-based cone-GEOMETRY suppression |

No code deleted — everything is re-armable with an env var. Registry comments
updated in `scripts/check-env-registry.sh:780-781`; `check-env-registry.sh`
**PASS** (983 vars).

Rationale: a cosmetic enemy-position leak is strictly less harmful than
blinding the player's own mechs mid-mission.

### Commits (branch `claude/nifty-mendeleev`, never pushed)

| sha | what |
|---|---|
| `7cd8dd13` | cone GEOMETRY honours the fade-to-invisible alpha (the missed third emission) |
| `89f0ed4f` | **park both gates default-OFF** + registry comments |

---

## 5. HANDOFF → VISIBILITY-SEMANTICS-1

That lane is **already live in this worktree** (observed mid-session:
`mclib/vis_semantics.h`, `mc2_vis::MoverVis`, `Mech3DAppearance::buildVisState()`
at `mclib/mech3d.h:826`, `mc2_vis::emitsLight()` replacing the
`mover_model_shown` callers, gate `MC2_VIS_SEMANTICS`). It has already absorbed
my parked gates as its legacy fallback. Below is the site map it needs.

### The entanglement, stated precisely

`visible` + `sensorLevel` + `alphaValue` are doing **four** different jobs:

| question | where it is actually decided today |
|---|---|
| (a) is the MESH drawn? | **`alphaValue`** faded to 0 — NOT `visible`. `visible` is `true` on every contact branch. |
| (b) is the unit KNOWN to the player? | `sensorLevel` 1..4 = blip only; 0 = visual contact; >=5 = friendly |
| (c) does the mover EMIT light/cone? | three separate predicates, listed below |
| (d) is the unit itself LIT / self-illuminated? | not audited by this lane — open |

### (a) Mesh-draw decisions

- `code/mech.cpp:6690-6755` — `setVisibility(true,true)` on **every** branch; hide is via `setAlphaValue(0)`
- `code/gvehicl.cpp:3775-3830` — same shape, same `setVisibility(true,true)` at `:3833`
- `mclib/gvactor.cpp:2192-2212` — `if (gvShouldRender) { ... if (visible) gvShape->Render(true); }`
- `mclib/gvactor.cpp:2432` — blip draw: `if ((sensorLevel > 0) && (sensorLevel < 5))`
- `mclib/mech3d.cpp:3220` — `mechShape->Render(true)` under `if (!gpuMechSubmitted && g_drawMechs)`
- `mclib/mech3d.cpp:4613` — the precedent: sensor shapes render only for `sensorLevel ∈ [1,4]`; skip is the exact inverse
- alpha plumbing: `TG_MultiShape::alphaValue` → `msl.cpp:1902` / `:1906` / `:2148` → `TG_Shape::MultiTransformShape` / `_PositionsOnly` / `Render`

### (b/c) POOL / SPOT light registration — every `addWorldLight` site

| site | predicate | consults `mover_model_shown`? |
|---|---|---|
| `mclib/mech3d.cpp:4072` | registration, `if (!pointLight && eye->isNight)` | n/a |
| `mclib/mech3d.cpp:4091` | anubis pointLight active: `visible && (sensorLevel > 4) && !InEditor && status != DESTROYED/DISABLED/SHUTDOWN` | **no** (stricter — excludes `sensorLevel==0`) |
| `mclib/mech3d.cpp:4141` | generalized `SpotLight_` registration loop | **no** (slot consumption, not emission) |
| `mclib/mech3d.cpp:4295` | `SpotLight_` active flag | **yes** |
| `mclib/mech3d.cpp:5546` | off-screen freshness `ownerOn` | **yes** |
| `mclib/mech3d.cpp:5552` | off-screen `pointLight->ownerEnabled` (`sensorLevel > 4`) | **no** |
| `mclib/gvactor.cpp:2618` | `SpotLight_` registration loop | **no** |
| `mclib/gvactor.cpp:2647` | `addWorldLight` | n/a |
| `mclib/gvactor.cpp:2772` | active flag | **yes** |
| `mclib/gvactor.cpp:3090` | off-screen freshness `ownerOn` | **yes** |
| `mclib/bdactor.cpp:3160` / `:3239` / active at `:3335` | `eye && eye->isNight && visible && !forceLightsOut` | **no — by design** (static lamps) |
| `code/turret.cpp:827` | `active && isNight && !pointLight && status != DESTROYED` | **no** (turrets are building-class) |
| `code/carnage.cpp:694`, `code/weaponbolt.cpp:1372` | transient FX lights | **no** — a bolt fired by a blip-only enemy still lights terrain |

**Inconsistency worth resolving:** the anubis `pointLight` uses `sensorLevel > 4`
while the generalized `SpotLight_` vector uses `sensorLevel == 0 || >= 5`. A
visually-contacted enemy (`sensorLevel == 0`, mesh drawn) gets its spot pools
but no anubis pool.

### (c) CONE GLOW — all five `Queue*Glow` call sites in the tree

| site | predicate | `ownerIsMover` |
|---|---|---|
| `mclib/mech3d.cpp:4306` | `if (spotlightLights_[k]->active)` | `true` |
| `mclib/gvactor.cpp:2785` | `if (spotlightLights_[k]->active)` | `true` |
| `mclib/bdactor.cpp:3348` | `if (spotlightLights_[k]->active)` | default `false` |
| `mclib/bdactor.cpp:3363` | beacons, `isNight && visible && !forceLightsOut` | — |
| `mclib/genactor.cpp:1178` | beacons, `if (eye && eye->isNight)` — **no visibility predicate at all** | — |

Internal gates in `QueueConeGlow` (`mclib/cone_glow.cpp:331+`): `glow_enabled()`,
`nightFactor > 0.001`, the mover-only staleness guard
(`ownerIsMover && vis_gate_enabled() && node->GetLastTurnTransformed() != turn`),
and an untransformed-origin sentinel. Flushed unconditionally at
`mclib/txmmgr.cpp:2528`.

**`ownerIsMover` is load-bearing.** The prior lane's first cut applied the
staleness guard to all callers and silently ate **every building lamp cone**
across the airbase — caught only by A/B. Building cone leaves are stripped from
the static-prop recipe (GHOST-CONE-RECIPE-SKIP-1) and never individually
transformed on the GPU prop path, so their staleness stamp is meaningless.

### (c) CONE GEOMETRY — the leaf mesh (all gated only on `isNight` + `lightsOut`)

Classification: `mclib/tgl.cpp:376` / `:595` —
`isSpotlight = (S_strnicmp(nodeName, "SpotLight_", 10) == 0)`. Exact 10-char
prefix, so `L_SpotLight` / `L_SpotRotar` do **not** match.

- `mclib/tgl.cpp:1910` (`MultiTransformShape`) and `:2915` (`_PositionsOnly`) — `if (isSpotlight && !isNight) { listOfVertices = NULL; return 1; }`
- `mclib/tgl.cpp:3197` (`Render`) — `if (isSpotlight && !isNight) return;`
- `mclib/tgl.cpp:2787` / `:3363` — the unconditional `0xffffffff` emissions
- `mclib/tgl.cpp:2584` / `:3039` — cone faces force-accepted two-sided
- `mclib/tgl.cpp:1866` / `:2819` / `:3405` / `:3416` — `isSpotlight` carved out of the shader/indexed fast paths, which is why cones always land in the legacy immediate batch

These are the three sites `7cd8dd13` extended with `cone_geom_hidden_by_alpha`.

### Misclassification check — hypothesis (d): NEGATIVE

No mover-owned cone can be misattributed as a building lamp. `bdactor.cpp`
only ever handles `bldgShape`/`treeShape`; GV and mech shapes are
`registerMultiShape`'d (`gvactor.cpp:1163`) but **never** `submitMultiShape`'d
(the only submitters are `bdactor.cpp:2202/2261/6255/6302` and
`genactor.cpp:839/863`). Slot tagging is correct: `mc2_spotlight_diag::tag_slot`
with `Mech` (`mech3d.cpp:4077`, `:4176`), `Gv` (`gvactor.cpp:2653`), `Bldg`
(`bdactor.cpp:3247`), consumed at `mclib/camera.cpp:2663-2685` / `:2738-2743`.

### Real hole found in passing

`code/artlry.cpp:1597` and `:1664` construct `GVAppearance` for the airstrike
bomber. Those objects are **not `Mover`s**, so `setSensorLevel` is never called
and `sensorLevel` stays 0 — `mover_model_shown(0)` returns `true`
unconditionally, leaving the fairness gate permanently open for a bomber's
`SpotLight_` nodes.

---

## 6. Open / not done

- **Which tier1 mission lost its mech lights — NOT isolated.** Highest-value
  next measurement.
- **Player-owned `sensorLevel` — NOT measured live.** Source analysis (§3) says
  it is only written under `isOnGui`; confirm empirically.
- **(d) "is the unit itself lit/shaded" — not audited at all** by this lane.
- The alpha-based geometry predicate (`7cd8dd13`) was never A/B-verified in a
  scene containing a mid-fade mover; the four bookmark poses cannot show it.
- REVIEW pack was **not** refreshed with clean-build crops — superseded by the
  stand-down. Clean-build evidence lives in `scratch/gcv4/`.

## 7. Deploy attribution caveat (read before trusting the install)

`mc2-win64-v0.4d-rc1` currently holds the **parked** build, verified
empirically: at pose 212019 it differs from the armed `11ec0d5a` build by
62,715 px (0.756%) — i.e. the mover lights are back — and from the armed
geometry-fix build by 5,062 px.

But its manifest `src_commit` reads `6aaba337`, which is **wrong**: a
concurrent lane (`VISIBILITY-SEMANTICS-1`) was committing and had left
`mclib/mech3d.h` in a non-compiling intermediate state, so the post-commit
rebuild failed and the previously-linked (parked-source) exe was deployed while
HEAD had already moved. The exe's *source* is the parked state; the recorded
sha is not meaningful.

**The successor lane should re-deploy from its own clean build before grading
anything.** Do not trust this install's manifest sha.
