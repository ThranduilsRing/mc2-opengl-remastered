# DEFERRED-BATCH-8C status — 2026-07-23

Branch `claude/deferred-sweep-1`, worktree A:/Games/mc2-deferred-1.
Deploy lane `A:/Games/mc2-opengl/releases/mc2-win64-deferred-test`.
Evidence: `.claude/captures/batch8c/` (full-res crops, never thumbnails).

Two agents worked this branch concurrently this session:
- **THIS agent (batch-8C)** landed Item C (`7c43e469`) and independently
  VERIFIED Item A.
- A **peer agent** landed Item A (`73755184`) on top of `7c43e469` mid-session
  (its WIP first appeared as uncommitted edits in my worktree, then committed).

HEAD chain: `73755184` (peer, Item A) → `7c43e469` (mine, Item C) → `e485f8e1`.

---

## Item C — 212019 "cream wedge" cone leaf-name — RESOLVED (identified + guarded), commit `7c43e469`

**The wedge is LEGITIMATE textured geometry; the skip correctly must not eat it
(per the mission directive).** Landed the assigned experiment (a build-free
static-prop leaf-NAME census) that blocked batch-7/8 for two sessions, plus a
DO-NOT-WIDEN invariant guard.

### What it is (ground truth, no more guessing)
`MC2_CONE_LEAF_DUMP=1` (new default-OFF diagnostic, allowlisted) dumps every
static-prop recipe leaf's node name + flags at `BldgAppearance::registerStatic`.
mc2_10 census (`.claude/captures/batch8c/C_leafdump/`):

- The standalone **`Spotlight` prop** has leaves `L_SpotLight` / `L_SpotRotar` /
  `L_SpotBattery` — **all `isSpot=0`**. `GetIsSpotlight()` keys on the exact
  10-char `"SpotLight_"` prefix (tgl.cpp:345/561); `"L_SpotLight"` does not match.
- `L_SpotLight` is a **TEXTURED 16-vert / 20-face lamp head** (asset
  `tgl/spotlight.ase`, bitmap `L_Spotlight.tga`) — the real searchlight lamp.
- Building-attached ghost cones DO get `isSpot=1` and ARE skipped as intended:
  `Spotlight_Building01` (AmmoDump), `SpotLight_Building` (TinShack),
  `Spotlight_SensorCon` (S_HQ), `Spotlight_TurretCon` (S_TurretControl).

So GHOST-CONE-RECIPE-SKIP-1 (09dafc9f/ae2b4811) already leaves `L_SpotLight`
alone. Its wrong **cream** look (visible at `.claude/captures/batch8c/crop_wedge_212019.png`)
is the SAME unlit-static-prop root as Item A/B (unlit geometry showing raw art
colour), NOT a reason to skip a real prop.

### What landed
- `MC2_CONE_LEAF_DUMP` diagnostic (default OFF, allowlisted trace in
  check-env-registry.sh — check PASS 872 vars).
- A **CONE-LEAF-NAME INVARIANT** comment at the recipe-skip predicate:
  DO NOT widen `GetIsSpotlight()`/the name test to "SpotLight"/"L_Spot"/a
  cone-shape heuristic to chase the wedge — that would EAT the legit
  `L_SpotLight` lamp (and `L_Spot*` / `LitWin_Spot*` leaves).
- No behavioural change (diagnostic default OFF). tier1 5/5 PASS on `7c43e469`.

**Verdict: RESOLVED.** The mission's "must not eat it" is satisfied — the wedge
is confirmed legit and provably not skipped; a future regression is guarded.

---

## Item A — far-backdrop night black band (CRITICAL) — peer fix `73755184` VERIFIED **INEFFECTIVE**

Full-res A/B at mc2_10 saved_205952, **both runs on the peer's fix exe
`737551841c8b`**, `MC2_BACKDROP_NIGHT_LIGHT` default-ON vs `=0`:

- `.claude/captures/batch8c/crop_bandAB_ON.png` vs `crop_bandAB_OFF.png`:
  **the horizon ridge stays PURE BLACK with the fix ON** — visually identical.
- Black-ridge region (y280-420): near-black share 29.84% ON vs 29.71% OFF;
  pixels that are pure black with the fix OFF are lifted to mean **0.31/255**
  with it ON (i.e. NOT lifted). The `0.10/0.11/0.14` floor (~26/28/36 in 8-bit)
  never reaches the ridge geometry.
- Total deterministic change is ~0: the only differing pixels (0.15%) are green
  vegetation mid-tones that got **darker** ON ([48,97,47]→[26,70,27]) — a `max()`
  floor CANNOT darken, so those are run-to-run non-deterministic tree flicker,
  not the fix.

**Why it fails:** `73755184` floors the **untextured gos_vertex** path
(`applyBackdropNightFloor`, gated on `gos_State_Texture==0 && ZCompare!=0`). But
the black ridge is **not drawn by that path** — it is the **textured far
terrain/impostor backdrop composited into the scene FBO** (batch-8B RenderDoc:
eid 12756 tonemap blit samples a scene FBO already dark in the far region; the
band region carries tree impostors, `MC2_LIGHTING_DEBUG_VIEW=albedo` does not
repaint it ⇒ not chunk terrain). The peer commit's premise ("gos_vertex tint
paints the whole ridge") is contradicted by this deterministic A/B: flooring
gos_vertex does nothing to the ridge. This is exactly the disproven
gos_vertex-overlay attribution the mission flagged (8A/8B).

**Verdict: NOT FIXED.** The landed fix is a no-op for its stated target (I did
not revert it — that is a coordinator call; the gate defaults ON but is
byte-neutral on the band and tier1-safe). Correct fix must light the **textured
far-backdrop terrain + far tree-impostor** pass (give it the mission fog/ambient
so the ridge takes the grey horizon colour instead of raw-art black), not the
untextured gos_vertex program. Next experiment to finally settle the draw path:
RenderDoc the 205952 ridge pixels and read the bound program + whether the source
node is textured (resolves the standing 8A-vs-8B contradiction before ANY further
code).

## Item B — fullbright white impostors + LOD-crossfade white flash — NOT FIXED (same root, same miss)

Per the coordinator's scope add: the "LOD crossfade white flash" LOW is the same
defect — far tree impostors render unlit/fullbright (trees pre-register all LODs
sharing LOD0's light slot; `selectActiveLOD`/`registerStatic`, mclib/bdactor.cpp),
so the LOD pop flashes bright. This is the same unlit-far-geometry family as Item
A (raw art colour: white where the impostor art is bright, black at dark poses).
Tree impostors are **textured**, so the peer's untextured-gos_vertex floor
`73755184` does not touch them either → Item B and the LOD flash are **NOT fixed**
by the landed Item A commit. A full-zoom camera-sweep LOD-transition A/B was not
run (deploy lane was actively contended by the peer's process; exe held busy).
Fix is unified with the Item A textured-impostor-lighting slice above.

---

## Housekeeping
- Temporary `gosvtx_bt_probe` (MC2_GOSVTX_BT) in gameos_graphics.cpp: **removed**
  (reverted the file to HEAD; the probe's submitter question was already answered
  in batch-8B).
- Deploy lane carries `737551841c8b` (peer's combined exe) — the branch (my
  Item C + peer's Item A) compiles + links clean; tier1 5/5 PASS on `7c43e469`.
- Lane note: a background writer intermittently re-applied the far-backdrop WIP
  into my worktree mid-session (gameos_graphics.cpp, gos_vertex.frag), then
  committed it as `73755184`. Flagged as concurrent-agent collision on one branch.

## Gates
- `MC2_CONE_LEAF_DUMP` (NEW, default OFF, allowlisted diagnostic) — mine.
- `MC2_BACKDROP_NIGHT_LIGHT` (peer's `73755184`, default ON) — VERIFIED no-op on
  the band; recommend revisit/revert pending the correct textured-path fix.
