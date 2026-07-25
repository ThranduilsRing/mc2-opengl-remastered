# LIGHT-REALISM-1 status — 2026-07-24

Lane LIGHT-REALISM-1, worktree `A:/Games/mc2-main`, branch `claude/nifty-mendeleev`.
Evidence install: `A:/Games/mc2-opengl/releases/mc2-win64-deferred-test`.
Review crops: `release_assets/0.5.2/REVIEW/light_realism_1/`.

## What the user rejected

Three successive framings, all the same underlying complaint:

1. "the cones are ultra visible, they need to be feathered / more transparent.
   and the light needs to spill from there, not be just in random spots... the
   light sources need to mimic LIGHT SOURCES (real ones)."
2. Guard towers throw hard-edged WHITE TRIANGULAR WEDGES: uniformly bright along
   their whole length, sharply cut at the edges, opaque, sitting *on top of* the
   ground as a shape and ending abruptly, with objects inside them not visibly
   lit.
3. "here the left side light cone doesn't actually radiate any light" — one
   fixture with a cone AND a ground pool sitting next to an identical fixture
   whose ground stays black. Emission and ground illumination were DECOUPLED.
4. Beacons (red/green/blue/runway) "emit NO light at all ... just bright unlit
   sprites floating on dark ground."

## Commits

| Commit | Content |
|---|---|
| `45058613` | Cone silhouette feather + opacity + smoother axial ramp; pool radius reshape (continuity + no hard disc) |
| `db625a12` | Beacon emission (first cut) |
| `cea05a2d` | Beacon fixes: owner class, transform, flat-shading + depth |
| `5213d257` | **LIGHT-POOL-CAP-1** — pool cap 64 -> 256 (`MC2_LIGHT_POOL_MAX`) + demand instrumentation |

## THE ROOT CAUSE (item 3) — a starvation bug, not a look problem

`gos_terrain_lighting.cpp` packed lights only while `numLights < s_maxLights`,
and both callers (`code/mission.cpp:4791`, `code/saveload.cpp:1193`) passed
**64**. Past 64 accepted lights, every further fixture was silently dropped, so
whether a lamp lit the ground depended on its ORDER in the terrain-light list —
nothing the player can see. This also explains the sibling lane's observation
that slots got *repacked* (different buildings lit) when hidden movers' lights
were dropped by the visibility gate: same starvation, reshuffled.

Measured demand at mc2_10 (new `[LIGHT_POOL_CAP v1]` counter, which keeps
scanning past the cap instead of breaking, so it reports real demand):

| Condition | peak wanted | packed | dropped |
|---|---|---|---|
| cap=64 (old default), free timestep | **142** | 64 | **78 (55% starved)** |
| cap=256 (new default) | 142 | 142 | 0 |
| cap=256, fixed timestep | 111 | 111 | 0 |

Per user directive ("ok just increase the number of lights available"), raised
to **256** — ~1.8x headroom over the observed 142 worst case. The cap is now
owned in ONE place (`mission_init`) and overridable at runtime via
`MC2_LIGHT_POOL_MAX` (clamp [1,4096]).

### Cost — and it is the per-fragment loop, not storage

Free-timestep A/B at `mc2_10_saved_204753`:

| Cap | avg FPS | p50 ms | p99 ms |
|---|---|---|---|
| 64  | 79.9 | 9.81  | 36.65 |
| 256 | 52.8 | 15.90 | 55.47 |

**+6.1 ms p50 / −34% avg FPS.** Being explicit about where that goes:

- **Storage is irrelevant.** The pool is an SSBO with an *unbounded* array
  (binding 28, `terrain_lod_chunk.frag`, mirrored in `terrain_overlay`,
  `mech`, `static_prop`) — not a uniform block, so no uniform-block limit
  applies. 256 entries is ~12 KB.
- **The per-fragment loop IS the cost driver.** Consumers loop to
  `u_numPoolLights`, the PACKED count. The regression is not "the cap is
  bigger", it is "142 lights are now actually being evaluated instead of 64" —
  i.e. it is the cost of rendering the lights that were supposed to be lit all
  along.
- **Follow-up that would buy it back:** tiled/clustered light assignment, so
  each fragment iterates only the lights touching its tile instead of all 142.
  On this scene that is roughly a 5-15x reduction in light-loop iterations for
  the same image. Not attempted here — the directive was to ship the cap raise
  now, and clustering is a separate arc.
- **Escape hatch:** `MC2_LIGHT_POOL_MAX=64` restores the old cost (and the old
  bug). A middle setting (~160) trades correctness headroom for frame time.

## The look fix (items 1, 2, 4) — gate `MC2_LIGHT_REALISM`, default OFF

### Cone silhouette feather (`mclib/cone_glow.cpp`)
Every vertex at a given axial position carried the same alpha, so the mesh
outline *was* the hard edge. Added the standard screen-space volumetric fake: a
view ray through a cone accumulates maximum thickness through the beam CENTRE
and ~zero at the silhouette, and the silhouette is exactly the set of vertices
projecting farthest perpendicular from the beam's screen-space axis. Fading by
that distance feathers the edge while keeping a bright core down the full beam.
Uses only screen coords the transform loop already computes.

Also: an opacity scalar, and a squared (not linear) axial decay so the beam is
brightest at the fixture and dies out along its length.

### Pool continuity (`gos_terrain_lighting.cpp`, producer-side, NO shader edit)
Two artefacts came out of the three packed radius fields, so both were fixed at
the producer — every consumer inherits the same coherent shape:

- **Hard disc:** falloff is literally `1.0` inside `closeDistance` — a flat-topped
  plateau, which *is* the hard bright disc. Shrinking `closeDistance` collapses
  the plateau into a smooth peak-and-decay bell.
- **Disconnected blob:** LIGHT-CONE-MODELS-1 Stage C moves the pool centre to
  the beam's far end; at stock radius the lit patch stops short of the lamp,
  leaving dark ground between pool and emitter. Widening `farDistance` stretches
  the pool back over the emitter so cone and pool read as ONE light.

`oneOverDistance` (the ramp slope) is recomputed in lockstep.

### Beacon emission (`mclib/cone_glow.cpp` + `mclib/genactor.cpp`)
Soft colored additive ground disc per beacon / runway-light card, colour from
the node name, population classified by **reusing** LIGHT-BILLBOARD-GPU-1's
`TG_Shape::GetIsLightBillboard()` — no edit to `mclib/tgl.cpp`.

Deliberately **not** a registered `TG_Light`: beacons number in the dozens per
map and would consume the very pool budget this lane just had to raise. The disc
rides the additive `MC2_ISSPOTLGT` batch the cone glow already owns, costing
zero light slots.

Three bugs found by instrumenting rather than assuming (`[BEACON_DIAG v1]`):
1. **Wrong owner class.** Hooked `BldgAppearance` first; a diag sweep showed
   `billboards=0` across all 400 calls — these props are `GenericAppearance`.
2. **Collapsed geometry.** Disc was a world-space ring projected with
   `s_worldToClip` *alone*. Decisive probe: identical render at radius 30 and
   radius 200 — it never scaled, because `s_worldToClip` is only half the legacy
   transform. Now anchored by projecting the node origin through the proven
   `shapeToWorld * s_worldToClip` path and built in screen space.
3. **Flat shading + depth clip.** Rendered as a solid hard-edged half-moon: this
   batch is FLAT-SHADED, so a centre-anchored fan gave every triangle one colour
   (the authored cone meshes escape this only by having many triangles along the
   beam), and the lower half was clipped by ground in front. Rebuilt as
   concentric rings (5 bands x 12 segments) with intensity **pre-multiplied into
   RGB** (the pass is `gos_Alpha_OneOne` and untextured) plus a small
   toward-camera depth bias.

## Gate + tunables

| Var | Default | Meaning |
|---|---|---|
| `MC2_LIGHT_POOL_MAX` | **256** (was hardcoded 64) | Pool slot budget. **Default-ON fix**, independent of the look gate. |
| `MC2_LIGHT_REALISM` | **OFF** | Master look gate. All look changes below are no-ops when off. |
| `MC2_LIGHT_REALISM_CONE_OPACITY` | 0.55 | Cone peak-alpha scale; lower = more transparent |
| `MC2_LIGHT_REALISM_CONE_FEATHER` | 1.5 | Silhouette feather exponent; 0 = no feather |
| `MC2_LIGHT_REALISM_POOL_SPILL` | 1.6 | Pool `farDistance` scale (reach back to emitter) |
| `MC2_LIGHT_REALISM_POOL_CORE` | 0.15 | Pool `closeDistance` scale (shrink the flat disc) |
| `MC2_LIGHT_REALISM_BEACON` | 45 | Beacon glow peak alpha |
| `MC2_LIGHT_REALISM_BEACON_RADIUS` | 28 | Beacon glow radius (world units) |

Diagnostics: `MC2_LIGHT_CONE_DIAG=1` -> `[BEACON_DIAG v1]`.
`[LIGHT_POOL_CAP v1]` is always-on but rate-limited to high-water transitions.

## Variants shipped for taste grading

- **A / conservative (the defaults above)** — recommended. Softer, continuous,
  clearly better than OFF without brightening the scene much.
- **B / stronger** — `CONE_OPACITY=0.40 CONE_FEATHER=2.5 POOL_SPILL=2.2
  POOL_CORE=0.0 BEACON=70 BEACON_RADIUS=40`. Light spills much further and the
  apron reads as genuinely lit. Closer to the user's stated target, but pushes
  against the project's "err dimmer" north star.

## Verification

- **Verify-by-exaggeration first**: at deliberately extreme settings the graded
  `lamp_cones` region changed 93.5% of pixels — effect is live and attributable.
- Region metrics OFF vs each arm: `.../light_realism_1/metrics.json`.
  Sanity signal: `distant_ridge_base` (a region with no light sources) is
  **0.00 mean delta / 0.0% changed** in both arms — only lit areas moved.
- Eyeballed at the user's own framings: 204753 airbase (beacons + apron) and
  212019 guard-tower compound (5+ cones in one shot).
- `check-env-registry.sh` **PASS** (972 vars).
- **tier1 PASS 5/5**, mover-coherence 0 violations, mc2_10 included.

## Residual / honest gaps

- **tier1 fingerprint gate could not be satisfied.** It HARD FAILs because HEAD
  keeps advancing under me — other lanes commit into this shared worktree
  between my build and the check. The run did execute my install
  (`releases/mc2-win64-deferred-test`, lease confirmed) and my commits are
  ancestors of HEAD, but this is NOT a clean attributable gate. Re-run after the
  worktree quiesces.
- **Perf regression is real and unaddressed** (−34% avg FPS at the busiest night
  pose). It is the cost of lighting the 142 fixtures that should always have been
  lit, but it wants the clustered-assignment follow-up.
- Beacon discs still show slight upper-half bias where ground geometry in front
  wins the depth compare; the depth bias reduced but did not eliminate it.
- Only building/generic fixtures were swept for beacons; mover-mounted beacons
  (if any) are not wired.
- All grading so far is mine plus headless captures — **user click-through
  pending**, and which variant ships is a taste call I deliberately left open.
- `shaders/terrain_lod_chunk.frag` was deliberately NOT touched (contended and
  dirty with lane RECAP-MECHSHADOW-RUNWAY-1's uncommitted work); the pool fix was
  done producer-side instead, which turned out to be the better seam anyway.
