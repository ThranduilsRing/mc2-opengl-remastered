# LOWS-CLUSTER-1 — master-FAIL LOW cluster (item 10) status

Lane: `lows-cluster-1` (worktree `A:/Games/mc2-main`, branch `claude/nifty-mendeleev`, HEAD `22f28800`).
Source: SESSION-LEDGER-2026-07-18 "OPUS VERIFICATION FLEET — MASTER RESULTS", FAIL item 10 (LOW):
> Palms cast ~no day shadow vs pines hard night shadows; night ambient pose-variance
> (204523 brighter+green); LOD impostor crossfade white flash.

Investigated in collision-mandated order **3, 1, 2** (shadow files claimed by
`mech-night-shadow-invert-1` per operating rules). No code landed — see per-item verdicts.

## Overarching finding

All three LOW items are **downstream symptoms of the three in-flight HIGH master-FAIL
items**, and every root-cause implementation site is **freshly claimed by the active lane
already working the parent HIGH item**. Independently fixing them here would be the S2
double-implementation / commit-scoop failure class the lane registry exists to prevent.
Verdict for the cluster: **DEFER to the parent-HIGH lanes**, with mechanism + routing below.
Additionally, no current build exists to reproduce against: deployed lane `0.4` exe is
sha `daa9451` (2026-07-10), fingerprint-mismatched vs HEAD `22f28800`.

---

## Item 3 — LOD impostor crossfade white flash — VERDICT: DEFER (owner: HIGH#5 / batch-8B; site owned by DEFERRED-BATCH-8C)

Mechanism (code-traced, `mclib/bdactor.cpp` `TreeAppearance::selectActiveLOD` ~6730 +
`registerStatic` ~7124): tree LODs are all pre-registered at mission load and **share
LOD0's light slot** (bdactor.cpp:7163-7168), so the LOD switch is a **hard hysteresis
flip** (`_near = 0.92·dist`, `_far = 1.08·dist`, bdactor.cpp:6749-6758) with valid light
data on both sides — there is **no true crossfade** and no light-data gap. The visible
"white flash" is therefore a **brightness mismatch at the pop**: the far/impostor card
renders brighter than the LOD0 model — i.e. the same defect as **HIGH master-FAIL #5
"Fullbright tree impostors at horizon (RGB 255 at night)"**. Fixing #5 (apply scene
lighting/haze to the impostor card) removes the flash; a separate dithered crossfade band
would only mask it.

Collision: the LOD selection site `mclib/bdactor.cpp` is **freshly claimed by
DEFERRED-BATCH-8C** (mc2-deferred-1). The vegetation-card path (`shaders/gos_vegetation_card.*`,
uncontested) already does a smooth Bayer `v_lodFade·v_distFade` crossfade and is NOT the
tree-impostor path — no flash originates there.

Recommendation: fold into the #5 fullbright-impostor fix (batch-8B). If a true crossfade
is still wanted after #5, add a dithered transition band in the static-prop batcher +
`selectActiveLOD` — but only in the 8C lane to avoid a bdactor.cpp collision.

## Item 1 — Palms cast ~no day shadow vs pines hard night shadow — VERDICT: DEFER (owner: HIGH#4 shadow sun-state-blind; site owned by mech-night-shadow-invert-1)

Mechanism: tree shadow casting is uniform in the engine (`TreeAppearance` treats all
species identically); the palm-vs-pine difference is **asset geometry meeting the shadow
depth pass**. Palms are authored as near-edge-on alpha cards (little projected area +
the shadow depth pass's alpha handling → ~no day shadow), pines as solid cone geometry
(full hard shadow). Far/impostor LODs are already `setRecipeNoShadow(true)`
(bdactor.cpp:7172, gos_static_prop_registry.cpp:1894); LOD0 casts. This is exactly the
shape described by **HIGH master-FAIL #4 "Shadow model sun-state-blind + dither-only
penumbra; small casters all-stipple no umbra core"** — the day/night polarity and the
per-species inconsistency are the same root cause (shadow depth pass alpha + sun-altitude
scaling).

Collision: the shadow-cast path (`mclib/shape.cpp`, `code/objectappearance.cpp`,
`shaders/*shadow*`) is **freshly claimed by mech-night-shadow-invert-1** (this worktree,
lane 0.5.1). Polled at session start — lane active.

Recommendation: absorb into mech-night-shadow-invert-1 (HIGH#4). Species consistency
should fall out of that lane's shadow-depth alpha + sun-altitude fade work; verify palms
and pines side-by-side at day AND night after that fix lands.

## Item 2 — Night ambient pose-variance (204523 brighter+green) — VERDICT: DEFER / UNREPRODUCIBLE HERE (no build; attribution needs live bookmark)

Mechanism ruled OUT: `Camera::getLightRed/Green/Blue` (camera.cpp:4333-4387) depend only
on `lightRed/ambientRed`+caller `intensity` (mission-static), **not camera pose** — so the
pose-variance is not in the ambient getter family. The remaining candidate is a
**view-dependent term in the night terrain/composite path** (terrain frag ambient, sky-tint
reflectance, or fog) that shifts with camera yaw/pitch — consistent with "204523 brighter
+green" being one specific pose. Attribution requires a live capture at bookmark 204523
(and neighbours) plus an A/B sweep, which needs a fresh build; the current deployed exe is
stale/fingerprint-mismatched. Coding blind into the shared terrain shaders
(`shaders/terrain_lod_chunk.frag`, `gos_terrain.frag`) without repro-based attribution
would be a guess.

Recommendation: attribute during the next live session at 204523 (rotate-in-place A/B) or
fold into HIGH#4's night-lighting composite pass. `camera.cpp` and the terrain shaders are
not currently fresh-claimed, so a targeted fix is landable once the pose-variant term is
identified — but do it repro-first, gated (`MC2_*` killswitch), not blind.

---

## Net

No commits. No gates added. Three DEFER verdicts routed to the parent HIGH lanes
(batch-8B / mech-night-shadow-invert-1) and the next live-repro session (item 2).
Rationale: collision-avoidance (all root sites fresh-claimed) + downstream-of-HIGH +
no reproduction build. Re-open each LOW after its parent HIGH lands and re-verify at
full zoom per the COMPLAINT-SHAPE protocol (palms+pines day/night; impostor pop during a
zoom sweep; night ambient across a yaw sweep at 204523).
