# LIGHT-BILLBOARD-GPU-1 — STATUS (master-FAIL #8, MED)

**Verdict: FIXED, pending user click-through.** The whole mc2_10 airbase light
population is now present at every camera angle tested. Supersedes
`EDGE-LIGHT-BILLBOARD-CULL-1-STATUS.md`, whose diagnosis was wrong on both
counts (see below). Lane `LIGHT-BILLBOARD-GPU-1`, worktree `A:/Games/mc2-main`,
branch `claude/nifty-mendeleev`. Released.

## The predecessor's diagnosis was falsified

EDGE-LIGHT-BILLBOARD-CULL-1 (`9f8c8086`) described these props as "a single flat
quad with no billboarding" that the camera backface-culls, and made the legacy
CPU `TG_Shape::Render` per-triangle backface loop two-sided under
`MC2_LIGHT_BILLBOARD_TWOSIDED`. It also specified the "real fix" as a two-sided
draw bucket in the GPU batcher. Two independent errors:

1. **They are not quads.** `data/tgl/runwaylight.ase` (and the red/green/blue
   beacon variants) is a **6-vertex / 7-face truncated triangular prism** ~2.85
   units tall — a little three-sided tapered post. The three side faces carry
   the dark post colour; the ONLY emissive element is the small **horizontal
   top-cap triangle** (~0.79 unit radius, vertex colour 1,1,0.435).
2. **They are not being backface-culled.** A prism is not a card; you can see
   its cap from above and its sides from the side. Two-sided drawing — CPU or
   GPU — is not the cure, so the deferred draw-bucket work would have failed
   too.

## Actual mechanism (reproduced stock)

Held the camera **position** fixed and varied only the pitch, mc2_10 night
airbase, gate OFF:

| Bookmark | rot / proj | Stock result |
|---|---|---|
| `204753` | -1.25 / **25.0** (higher angle) | every runway edge light visible as a dot |
| `204907` | -80 / **12.6** (grazing) | **runways completely bare** |
| `212258` | -91 / 77.9 (top-down, far) | **zero airbase lights** |

The emissive cap is horizontal, so its projected area falls off with the view
angle — and the entire fixture already subtends only 1-2 pixels at RTS ranges.
At grazing angles the cap drops below one pixel and is simply never rasterised,
so the whole population blinks out together. This is a **sub-pixel geometry**
problem.

## What shipped

`shaders/static_prop.vert`, under per-instance flag **bit 4** (bit 5 = debug
exaggeration):

1. **Rigid camera-facing billboard.** The fixture's LOCAL vertex coordinates are
   re-emitted in an eye-facing basis `(R1, U1, toCam)`, rotating the whole prop
   about its own origin so the cap points at the viewer. Both frames are
   right-handed, so shape, scale and triangle winding are preserved — and the
   cap additionally can never be back-face culled. Instance uniform scale is
   recovered from the model matrix.
2. **Minimum screen size.** `kLbMinNdc = 0.0012`; the fixture is uniformly
   scaled about its origin until its silhouette spans that in NDC. The clamp is
   `max(1.0, ...)`, so it is **inactive at close range** and near geometry is
   untouched.

Flag emission is at **both** static-prop instance sites — the batcher's
`submitMultiShape()` (`gos_static_prop_batcher.cpp`) and the persistent-static
recipe path (`mclib/bdactor.cpp`). A split there would have fixed only half the
population. Bit 3 is `kFlagDecalFill` (cliff decal) and was left alone.

**Failed first attempt, recorded so it is not retried:** rotating each vertex by
its own **per-vertex normal** tears a prism apart (every vertex gets a different
rotation). The exaggerated capture showed the lights vanishing *entirely*. The
rigid per-instance form is mandatory.

## Population covered

`runwaylight` + `red/green/bluebeaconlight` and their damage/LOD variants — node
names `*beaconlight*`, the existing `TG_Shape::isLightBillboard` classification.
**No widening was needed.** Audited every `*light*` node name across the 2947
`.ase` files in `mc2-srcdata/tgl`; the only other candidates are solid geometry
rather than light elements and are correctly excluded:

- `streetlight` / `streetlightdamaged` — 12 vert / 14 face pole
- `Light` inside `*spotlightcontrol` — 10 vert / 16 face lamp head
- `spotlight_*` — already skipped by the batcher (real `TG_Light` registrations)
- `*lightwall` / `*lightgate` / `davionlightpost` — building geometry

Flags and barrels are untouched, as required.

## Gate

`MC2_LIGHT_BILLBOARD_FACING` — killswitch, **default ON**.
- `0` — no instance carries the flag bit, so the shader branch is never entered.
- `2` — verify-by-exaggeration (fixtures at 4x).

Registered in `scripts/run_smoke.py` passthrough allowlist,
`scripts/check-env-registry.sh` ALLOWLIST, and `docs/tier1_env_vars.md`.
`MC2_LIGHT_BILLBOARD_TWOSIDED` (CPU path) is unchanged and still default-ON; it
remains near-inert and is now documented as such.

## Per-angle verdicts (mc2_10, night, full-zoom crops)

| Bookmark | angle | gate OFF | gate ON |
|---|---|---|---|
| `204753` | higher angle, far | lights present | present, crisper dots — **PASS** |
| `204907` | grazing, far | **runways bare** | full red/green population restored — **PASS** |
| `204937` | grazing, close (alt 1257) | present | present, unchanged in character, no leaning/oversize — **PASS** |
| `204956` | grazing, close | present | present, unchanged in character — **PASS** |
| `212258` | top-down, far | **zero lights** | full runway + taxiway population — **PASS** |

No angle regressed; the two blackout angles are cured.

## Measurement caveat — READ BEFORE ANY A/B HERE

The predecessor documented a ~6.8% run-to-run drift from animated night light
pools and prescribed `MC2_TERRAIN_LIGHT_POOLS=0`. **That is no longer
sufficient.** With pools off, two *gate-OFF* runs at these poses still differ by
**2-7%** of pixels — the `LIGHT-REALISM-1` commit (`45058613`, feathered light
cones + emitter-continuous pools) that landed on this branch introduces
animation that pools-off does not disable. Numeric full-frame A/B at this scene
is therefore **meaningless** until someone finds the additional killswitch.

The verdicts above are consequently **categorical/visual** (an entire light
population absent vs present), which is far above that noise floor, not
numeric-threshold based. Byte-identity of the gate-OFF path is likewise argued
**structurally** (the flag bit is never emitted at gate 0, so the shader branch
is never entered) rather than by capture, because capture cannot resolve it here.

## Verification

- Build clean; **tier1 5/5 PASS** on this exe (`f412423e`), MOVER-COHERENCE 0
  violations across all five missions. The `DEPLOY_FINGERPRINT` warning in that
  run is only because a sibling lane's docs commit advanced HEAD past mine in
  this shared worktree; the exe is exactly `f412423e`.
- 20 headless capture launches across 4 configurations, no crash.

## Commits

- `f9efedc1` — flag plumbing (tgl.h accessor, both flag-emission sites, gate,
  registries, docs) + the first (wrong, per-vertex-normal) shader form.
- `f412423e` — corrected shader (rigid billboard + min screen size), corrected
  diagnosis in code comments and docs.

## Files

- `shaders/static_prop.vert` (the fix), `GameOS/gameos/gos_static_prop_batcher.{cpp,h}`,
  `mclib/bdactor.cpp`, `mclib/tgl.h`.
- Registries: `scripts/run_smoke.py`, `scripts/check-env-registry.sh`,
  `docs/tier1_env_vars.md`.
- Evidence: `A:/Games/mc2-main/scratch/lbgpu/` — `OFF/`, `OFF_v3/` (stock),
  `ON_v3/` (shipped), `EXAG/` (the failed first form), plus
  `crop_<pose>_{OFF,ONv3}.png` full-zoom crops.

## What remains

- **User click-through** — never observed live, only in headless captures.
- `kLbMinNdc` (0.0012) was tuned by eye at 4K on five poses. If the dots read
  too large at 1080p, that constant is the single knob.
- The billboard rotation leans the post toward the camera. Invisible at the
  ranges tested (1-3 px); if a future close-flyby pose shows it, restrict the
  rotation to the cap vertices.
- Shadows still use `shadow_static_prop.vert` and are NOT billboarded
  (deliberate — these fixtures cast negligible shadow).
- `MC2_LIGHT_BILLBOARD_TWOSIDED` is now known-near-inert. Consider retiring it.
- Find the killswitch for the LIGHT-REALISM-1 animation so this scene becomes
  numerically A/B-able again.
