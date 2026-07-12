# VFX Originals Lockdown — VFX-ORIGINALS-LOCKDOWN-1

Locked 2026-05-31. Branch `claude/nifty-mendeleev` HEAD `2715941f`.

This document records the authoritative ownership of each gosFX class/effect family
after the VFX-WEAPON-FX-RESTORE-OPUS-1 arc. It exists to prevent future GPU migration
work from regressing the restored original visuals.

**Rule:** a class must not be moved from CPU MLR to GPU without a full visual parity
proof in an interactive combat session. Smoke-only (passive 30s) is insufficient for
weapon-hit classes. The prior regression (missiles/PPC invisible/wrong for weeks)
was caused by exactly this: platform migration without parity proof.

---

## Ownership table

| gosFX class / effect family | Owner | Render path | Why | Migration requirement |
|---|---|---|---|---|
| CardCloud | GPU oracle billboard | CPU sim → oracle harvests particles → GPU batcher SSBO → `particle_billboard.vert` | Billboard-compatible; full oracle validated (spin, aspect, atlas, age). | — already migrated |
| ShardCloud | GPU oracle billboard | same | Approximate billboard, spin/aspect restored. | — already migrated |
| Card | GPU oracle singleton/card | same | Card-compatible; animated UV sub-rect wired. | — already migrated |
| PointCloud | GPU oracle billboard | same | Billboard-compatible; oracle wired. Live births=0 in passive smoke. | — already migrated; interactive MG/missile session pending |
| EffectCloud | delegates to children | no direct draw | Container; children draw themselves. | n/a |
| **Tube** | **CPU MLR swept-mesh** | gosFX Execute (CPU) → Tube::Draw falls through to MLRClipper::DrawEffect → gos_DrawTriangles | Billboard-per-profile oracle produced "ladder/fence" artifact (one card per spine profile ≠ ribbon mesh). Original swept-mesh is visually correct for missile smoke and PPC trail. | GPU swept-mesh ribbon oracle — emit one oriented quad per consecutive spine-profile pair via batcher; must pass interactive missile+PPC combat session before re-promotion. |
| **Shape** | **CPU MLR** | gosFX Execute (CPU) → Shape::Draw → MLRClipper::DrawScalableShape | 3D singleton mesh; not billboard-approximable. | GPU per-instance mesh draw (VFX-3D-MESH-GPU-SUBSTRATE-1). |
| **ShapeCloud** | **CPU MLR** | same as Shape | 3D mesh per particle. | Same substrate. |
| **DebrisCloud** | **CPU MLR** | same | Rigid-body 3D mesh chunks. | Same substrate. |
| **Missile smoke** (srm_trail, lrm_trail, Swarm_lrm_trail, TBolt_trail) | **gosFX Tube → CPU MLR** | gosFX Tube spec; no GPU ring-buffer trail | GpuTrailKind::MissileSmoke demoted — GPU ring-buffer billboards were visually inferior to the original Tube swept-mesh. | Re-promote `gpuTrailKindProven(MissileSmoke)` only after GPU swept-mesh ribbon oracle passes interactive combat parity check. |
| **PPC bolt trail** (ppc_trail, er_ppc_trail) | **gosFX Tube → CPU MLR** | gosFX Tube spec; no GPU ring-buffer trail | GpuTrailKind::PpcBolt demoted — GPU ring-buffer billboards were "white square" artifacts. | Same requirement as MissileSmoke above. |
| **hit/miss/muzzle effect cards** (hitEffect, missEffect, waterMissEffect) | **suppressed** (Draw gated when oracle ON) | gosEffect sim runs; Draw blocked by `!is_oracle_render_enabled()` | These effects rendered as oversized bright cards via oracle; original MC2 had them invisible (MLR-gated). The game's `createExplosion()` system provides the impact visual. | Re-enable when effect specs are tuned for GPU billboard atlas at correct scale. |
| **muzzle flash** (muzzleEffect) | **oracle/MLR when in flight; suppressed on impact** | muzzleEffect Draw allowed during flight (hitTarget=FALSE); suppressed when `hitTarget && oracle_ON` | Muzzle flash renders at bolt's frozen `laserPosition` (= target mech) after impact → produces static card at mech. | Fix: render at owner weapon-node position instead of bolt position; then unsuppress. |

---

## Gate summary

| Gate | Default | Effect |
|---|---|---|
| `MC2_VFX_ORACLE_RENDER` | ON | Enables oracle for CardCloud/ShardCloud/Card/PointCloud. `=0` falls back to Spawn placeholder. |
| `MC2_GPU_PARTICLES` | ON | Enables GPU batcher. `=0` disables batcher; oracle classes fall to placeholder Spawn. |
| `MC2_DISABLE_GOSFX` | OFF | `=1` suppresses all MLR work-leaves (DrawEffect/DrawScalableShape). Kills Tube/Shape/ShapeCloud/DebrisCloud. Does NOT affect oracle classes. |
| `gpuTrailKindProven(*)` | returns false | Controls whether a weapon bolt uses GPU ring-buffer trail or gosFX Tube. `PpcBolt` and `MissileSmoke` both return false — gosFX Tube is active. |

---

## Double-draw prevention

Oracle classes exit the GPU-path block via `Effect::Draw(info)` (propagates to child
effects only) rather than their base-class Draw (`SpinningCloud::Draw`, `Singleton::Draw`,
`ParticleCloud::Draw`). The base-class Draw would re-submit to MLR, causing a double
render now that `mlr_gate.cpp` `kDefaultDisabled = false`.

Files changed: `mclib/gosfx/cardcloud.cpp`, `shardcloud.cpp`, `card.cpp`, `pointcloud.cpp`.

If another oracle class is added in the future, the same pattern must be applied.

---

## MLR gate state

`mclib/mlr/mlr_gate.cpp` `kDefaultDisabled = false` (restored 2026-05-31).
The four gated functions in `mlrclipper.cpp` are: `DrawShape`, `DrawScalableShape`,
`DrawEffect`, `DrawScreenQuads`. These are gosFX-specific; no non-VFX code uses them.

Kill-switch `MC2_DISABLE_GOSFX=1` re-disables them. Oracle classes are unaffected.

---

## Migration path to full GPU (delete MLR)

1. **Tube GPU oracle** (VFX-TUBE-SWEPT-MESH-ORACLE-1): emit oriented quad-strips from
   spine profile ring-buffer. Validate interactively on missile smoke + PPC trail.
   Then: `gpuTrailKindProven` can be re-promoted + Tube oracle `if(false&&...)` removed.

2. **Shape/ShapeCloud/DebrisCloud GPU mesh** (VFX-3D-MESH-GPU-SUBSTRATE-1):
   per-instance transform SSBO + indirect mesh draw from pre-uploaded gosFX mesh geometry.

3. Once both are GPU: `mlr_gate.cpp`, `mlrclipper.cpp`, the four gate macros, and
   `MC2_DISABLE_GOSFX` can all be deleted.

---

## Validated state (post-lockdown)

- Tier1 5/5 PASS, +0 destroys — confirmed post-weapon-fx-restore arc.
- Missile smoke: Tube swept-mesh visible in mc2_10/mc2_24.
- PPC trail: Tube swept-mesh, no GPU ring-buffer squares.
- PPC hit card: absent (GPU trail `!hitTarget` gate prevents static blue head particle).
- Oracle classes: no double-draw (Effect::Draw exit confirmed).
- Shape/ShapeCloud/DebrisCloud: visible via CPU MLR (kDefaultDisabled=false).
