# LIGHT-CONE-MODELS-1 status — 2026-07-23

Branch `claude/deferred-sweep-1`, worktree A:/Games/mc2-deferred-1.
Deployed to releases/mc2-win64-deferred-test (manifest src_commit `6a526e11`).
Evidence: `.claude/captures/lightcones/` (untracked, per capture convention).

## Investigation findings (cited)

- **Node naming**: `"SpotLight_"` case-insensitive 10-char prefix on TGL shape
  node names — tgl.cpp:345/561 (`isSpotlight` set at type instantiation);
  windows are `"LitWin_"`. Spotlight shapes get `noShadow=true` (tgl.cpp:349)
  and are skipped by RenderShadows (tgl.cpp:3961).
- **Stock cone rendering**: night-only (transform early-out `isSpotlight &&
  !isNight`, tgl.cpp:1873), submitted untextured via
  `addVertices(0xffffffff, MC2_ISSPOTLGT)` (tgl.cpp:3320), consumed in
  renderLists' dedicated ISSPOTLGT loop with **gos_Alpha_OneOne additive,
  ZCompare on / ZWrite off / fog off** (txmmgr.cpp VfxHud block ~3931-4036).
  Backface culling skipped for spotlight faces (tgl.cpp:2544).
- **Why cones were invisible**: BOTH GPU batchers deliberately suppress the
  geometry — gos_mech_batcher.cpp:1301 ("the cone is no longer drawn") and
  gos_static_prop_batcher.cpp:5006 — a T3.1 "substitutive completion"
  decision that replaced the cone billboard with the real TG_Light only.
- **Light registration**: per-class (E) blocks register a TG_LIGHT_POINT per
  SpotLight_ child — bdactor.cpp ~3192, gvactor.cpp ~2601, mech3d.cpp ~3964 —
  positioned at the node ORIGIN via getNodeIdPosition (shapeToWorld
  translation, msl.cpp:1394) + the MOVER-LIGHT-FORWARD-1 facing heuristic.
  Node ORIENTATION was never consumed.

## Shipped (commits)

| Commit | Content |
|---|---|
| `5ba0e3fc` | Stages A+B+C: mclib/cone_glow.{h,cpp} module; actor-site wiring (bdactor/gvactor/mech3d); FlushFrame hook at renderLists top; tgl.h GetMyType/GetShapeScalarValue accessors; gate allowlists |
| `6a526e11` | Registry: getenv literals sweep-visible, flags.auto.yaml regen (867 flags), check-env-registry PASS (865) |

### Stage A — SOURCE-FROM-NODE (in `5ba0e3fc`)
`GetNodeAim()` derives the beam axis from the cone's OWN type geometry
(node-local vertex centroid; apex authored at the node pivot) composed with
the node's live `TG_ShapeRec::shapeToWorld`, MAX→MC2 swizzle per
msl.cpp:1298-1302. shapeScalar honored. This replaces the facing heuristic
wherever a model actually has a cone; heuristic remains the fallback
(movers) / node-origin (buildings).

### Stage C — SPILL
Pool light position = node pos + aim × authored cone length ×
MC2_LIGHT_CONE_SPILL (clamp 200wu), grounded on terrain via
`land->getTerrainElevation`. Tower searchlights now light the compound they
aim INTO; headlight pools land where the beam lands. This also implements
the deferred batch-6 "tower-spotlight aim-direction bias" item.

### Stage B — CONE SELF-ILLUMINATION
`QueueConeGlow()` re-renders the authored cone mesh additively through the
still-alive legacy MC2_ISSPOTLGT consumer. Phase discipline: triangle
reservation (addTriangle) at queue time in the owner update loop — the same
phase legacy MultiTransformShape reserved from — vertex fill deferred to
`FlushFrame()` at the top of `MC_TextureManager::renderLists()` so the
lazily-allocated vertex block (txmmgr.h:1014-1020) sees the full frame
reservation. Beam alpha fades apex→far end (1 → 0.15), color = the light's
own aRGB (halogen movers / warm buildings), scaled by nightFactor. Tied to
the SAME `active` owner predicate as the pool light — shutdown / destroyed /
disabled / FoW-hidden / lights-out ⇒ no cone (LIGHT-CULL-RADIUS-1 semantics
inherited, incl. the S2b-verified shutdown gating).

## Gates (defaults ON; registered + run_smoke passthrough + check-env-registry)

| Var | Default | Meaning |
|---|---|---|
| MC2_LIGHT_CONE_NODES | ON | =0 restores heuristic/origin pool centers (batch-6 behavior) |
| MC2_LIGHT_CONE_GLOW | ON | =0 hides the cone beams (batch-6 behavior) |
| MC2_LIGHT_CONE_GLOW_ALPHA | 40 | peak vertex alpha 0..255 (255 = exaggeration probe) |
| MC2_LIGHT_CONE_SPILL | 1.0 | spill distance scale on authored cone length [0,3] |

## Evidence (mc2_10 night, MC2_CAMERA_PARK poses from batch-6)

- `lc_close_default/off` (3500,4600 alt-900 close-up): tower-searchlight pool
  MOVES from the tower base to the aimed far end; 80% px differ (pool
  relocation over open terrain). Composite still reads occupied-not-floodlit.
- `lc_close_exag` (ALPHA=255): beam wedge clearly visible at the tower
  complex — glow path proven live; default-40 renders the same wedge subtly.
- `lc_default/off/exag_204611` (base composite): 2.6% px differ at defaults;
  night composite unchanged in character (err-dimmer holds at distance).
- `lc_pad_default` (204753 pad pose): defaults-on reference.
- Logs: [SPOT_DIAG] confirms bldg+mech+gv registrations firing; build
  fingerprint `6a526e11` in-log; STOP[PACKET_OOB]=117 and TerrainLOD NaN DIAG
  lines are PRE-EXISTING (identical in batch-6 logs).

## Stage D — shadows assessment (deferred, honest)

The engine has NO per-light shadow infrastructure: every shadow system
(terrain-lighting compute shadow bit, projected mech/prop shadows,
SHADOW-DIST-FADE) is single-sun-direction. Real shadows from pool lights
would need a depth pass per light (pool cap 64) or a cube/atlas system —
neither cheap nor incremental here. The "cheap fake" (darkening opposite the
light) was considered and rejected: on the additive pool model it reads as a
dirt smear, not a shadow. DEFERRED with recommendation: revisit only if a
deferred-lighting pass ever lands.

## Open / user-eyeball

- Moving-vehicle headlight closeup + mech beam in live play (headless park
  poses can't guarantee a mover near camera); shutdown A/B cone-off check
  (predicate is shared with the already-user-verified pool gating).
- Taste pass on MC2_LIGHT_CONE_GLOW_ALPHA (40) and MC2_LIGHT_CONE_SPILL
  (1.0) — dial live; err dimmer.
- mech LOD-swap caveat: node ids resolved on the live mechShape at
  registration (same pattern getNodeIdPosition already relies on).
