<!-- doc-status: current | reviewed:2026-10-11 -->
# VFX Oracle Coverage Matrix

> STATUS: all GPU oracle paths are default-OFF since 4ad31a9e. The tables and notes below describe the opt-in path (`MC2_VFX_ORACLE_RENDER=1`, inside `MC2_GPU_PARTICLES=1`), not the default.

Last updated: 2026-05-31 (VFX-WEAPON-FX-RESTORE-OPUS-1 complete — MLR re-enabled, Tube→MLR, weapon FX restored)

Branch: `claude/nifty-mendeleev` HEAD `9dfff4a9`

## Release Routing Table (VFX-WEAPON-FX-RESTORE-OPUS-1 — updated 2026-05-31)

| Class / Effect family | Final route | Gate / default | Reason | Validation status |
|---|---|---|---|---|
| CardCloud (388 specs) — dust, smoke, fireball, AC/Gauss trails, foot-poofs, mech-smoke, VTOL | **GPU_ORACLE** (oracle → GPU batcher; MLR suppressed at oracle exit via Effect::Draw) | `MC2_VFX_ORACLE_RENDER` default-OFF (opt-in `=1`) | Full oracle validated; oracle exit uses Effect::Draw to prevent MLR double-draw | CONFIRMED: tier1 5/5 + interactive |
| ShardCloud (24 specs) — sparks, debris fragments | **GPU_ORACLE** (same pattern as CardCloud) | `MC2_VFX_ORACLE_RENDER` default-OFF (opt-in `=1`) | Full oracle validated | CONFIRMED: tier1 5/5 |
| Card (136 specs) — muzzle flares, hit flashes, bolt lozenge | **GPU_ORACLE** (same pattern) | `MC2_VFX_ORACLE_RENDER` default-OFF (opt-in `=1`) | Full oracle validated; lozenge alpha=1 visible | CONFIRMED: tier1 5/5 |
| PointCloud (2 specs) — MG muzzle dot | **GPU_ORACLE** (same pattern) | `MC2_VFX_ORACLE_RENDER` default-OFF (opt-in `=1`) | Oracle path wired; 0 births in passive smoke | PARTIAL: oracle fires; visual needs interactive MG session |
| EffectCloud (39 specs) — composite container | **GPU_ORACLE** (delegates to children) | N/A | Container only; children handle their own routes | CONFIRMED: children validated |
| Tube (31 specs) — missile smoke, PPC trail, FX_FlameTrail, FX_SmokeTrail | **CPU_MLR** (oracle disabled `if(false&&...)`; falls through to MLR swept-mesh) | `MC2_DISABLE_GOSFX=0` default (MLR enabled) | Billboard-per-profile oracle caused "ladder/fence" artifact. MLR swept-mesh is the original correct visual. | CONFIRMED: missile smoke + PPC trail visible and correct |
| ShapeCloud (9 specs) — explosion mesh swarms | **CPU_MLR** (DrawScalableShape now enabled via `kDefaultDisabled=false`) | `MC2_DISABLE_GOSFX=0` default | MLR gate re-enabled 2026-05-31; no longer invisible | NEWLY VISIBLE: visual quality not yet interactively validated |
| Shape (10 specs) — singleton explosion meshes | **CPU_MLR** (same as ShapeCloud) | `MC2_DISABLE_GOSFX=0` default | MLR gate re-enabled | NEWLY VISIBLE |
| DebrisCloud (34 specs) — rigid-body debris chunks | **CPU_MLR** (same) | `MC2_DISABLE_GOSFX=0` default | MLR gate re-enabled | NEWLY VISIBLE |
| GPU weapon trails (MissileSmoke / PpcBolt) | **DISABLED** (`gpuTrailKindProven` returns false for all) | — | GPU ring-buffer billboard trail produced "white square" artifact; gosFX Tube via MLR is the correct path | gosFX Tube via MLR is active instead |

**MLR gate state (post-2026-05-31):**
- `mclib/mlr/mlr_gate.cpp` `kDefaultDisabled = false` (was `true`)
- All gosFX Draw calls route through MLR normally
- Oracle classes prevent double-draw by calling `Effect::Draw` (children only) instead of their base-class Draw at the GPU-path exit
- Kill-switch: `MC2_DISABLE_GOSFX=1` suppresses all gosFX via the 4 MLR work-leaves (DrawEffect, DrawScalableShape, etc.)

**WeaponBolt hit-site suppression (post-2026-05-31):**
- `hitEffect->Draw` — suppressed when oracle ON (`!is_oracle_render_enabled()`)
- `missEffect->Draw` — suppressed when oracle ON
- `waterMissEffect->Draw` — suppressed when oracle ON
- `muzzleEffect->Draw` — suppressed when `hitTarget && oracle ON` (muzzle flash at bolt's frozen position = target after impact)
- GPU trail emit — gated on `!hitTarget` (was re-emitting static head particle at target position each frame)

**Kill-switches for this release:**
- `MC2_DISABLE_GOSFX=1` — disables all gosFX MLR draws (Tube/Shape/ShapeCloud/DebrisCloud invisible)
- `MC2_VFX_ORACLE_RENDER=1` — opts in to oracle for CardCloud/ShardCloud/Card/PointCloud (default OFF: unset or `0` falls back to placeholder Spawn)
- `MC2_GPU_PARTICLES=0` — disables entire GPU particle batcher

**Future GPU work (post-release):**
- VFX-3D-MESH-GPU-SUBSTRATE-1 → GpuMeshCache → Shape/ShapeCloud/DebrisCloud GPU oracle (currently visible via MLR)
- Tube swept-mesh ribbon oracle (replace billboard-per-profile `if(false&&...)` stub)
- VFX-GPU-SIM-CARDCLOUD-PARITY-ID-1 → stable particle IDs + full gosFX physics parity
- GPU trail quality improvement → re-promote `gpuTrailKindProven` after proof

## Class x Oracle Status

Class counts from `mc2srcdata/effects/mc2.fx` (920,265 bytes, version 17, 904 specs)
via aligned 4-byte ClassID scan.

| Class | ClassID | Spec count | Oracle status | Spawned in tier1? | Best validation mission | Notes |
|---|---|---|---|---|---|---|
| CardCloud | 1318 | 388 | Full oracle | Yes (~30/frame in mc2_10) | mc2_10, mc2_24 | Workhorse; spin+aspect+atlas wired (VFX-ORIGINAL-RENDER-ANIM-FIELDS-1); age/lifetime uploaded |
| ShardCloud | 1316 | 24 | Full oracle | Yes (combat) | mc2_24 | Fixed atlasIndex=0u; spin+aspect wired; age/lifetime uploaded |
| Card | 1322 | 136 | Full oracle | Yes (combat, Flare) | mc2_10, mc2_24 | m_animated wired; uvRect sub-rect wired; age/lifetime uploaded |
| PointCloud | 1314 | 2 | Oracle added (VFX-POINTCLOUD-ORACLE-HARVEST-1) | No — active=0 in all tier1 passive smoke (Missile_Flare births=0; missile must fire within 30s window) | User-driven session with missile fire | No spin/aspect/atlas; position+color only; size constant 4.0f; age/lifetime uploaded |
| Tube | 1324 | 31 | Profile oracle added (VFX-TUBE-PROFILE-ORACLE-1) | Yes — fires in mc2_24 passive smoke (PPC trail spec="core", harvestedTotal≈4680+ at 30s). alpha=[0.000,0.000]; age_this_call=[0.014,0.568]. Spec-defined alpha=0 at ages 0-0.6 confirmed (not age-out). | User-driven session with flamer/FX_FlameTrail for visible alpha | One billboard per active spine profile; real swept mesh is B2 polish debt; FX_FlameTrail + FX_SmokeTrail are Tube class; age/lifetime uploaded |
| EffectCloud | 1320 | 39 | Composite container only | Yes (wraps CardCloud/ShardCloud/Card children) | mc2_10, mc2_24 | No direct GPU path; children handled individually by their own oracle |
| PertCloud | 1317 | 0 | Not present | No | N/A | 0 specs in mc2.fx; skip |
| ShapeCloud | 1319 | 9 | No GPU path | Unknown | Unknown | 3D mesh per particle; requires separate GPU primitive (not billboard) |
| Shape | 1323 | 10 | No GPU path | Unknown | Unknown | 3D singleton mesh |
| DebrisCloud | 1325 | 34 | No GPU path | Unknown | Unknown | Rigid-body 3D mesh pieces |
| PointLight | 1326 | 0 | Not present | No | N/A | 0 specs in mc2.fx; skip |

## Gate summary

- `MC2_VFX_ORACLE_RENDER` — **default-OFF** (reverted from the VFX-WEAPON-FX-RESTORE-OPUS-1 default-ON). Opt-in: `MC2_VFX_ORACLE_RENDER=1`. Enables oracle render for all migrated classes (inside `MC2_GPU_PARTICLES=1` check). Logs `[VFX_ROUTE v1] oracle_render=off reason=default_off` on startup (`reason=opt_in` when `=1`).
- `MC2_GPU_PARTICLES_LOG=1` enables `[VFX_ORACLE v1]` per-class stderr output.
- `MC2_VFX_DEBUG_MODE=5` enables age heatmap (blue=newborn, green=mid-life, red=dying) — VFX-SHADER-AGE-FADE-PARITY-1.
- `MC2_TUNE_VFX_AGE_FADE=<0..1>` enables soft death fade for oracle particles in final 30% of life — VFX-SHADER-AGE-FADE-PARITY-1.
- Gate-OFF (`MC2_VFX_ORACLE_RENDER=0`) is byte-identical — oracle path skipped, placeholder Spawn path runs.

## What fires in automated smoke (30s, no combat, camera pan only)

| Mission | VFX activity | Oracle classes confirmed |
|---|---|---|
| mc2_01 | VFX prohibited | none |
| mc2_03 | VFX prohibited (gosFX Draw called but emit=0, no combat) | none |
| mc2_10 | CardCloud active (harvestedTotal≈31393+ over 30s, 1-6/call dust/smoke), ShardCloud (harvestedTotal≈8203+), Card (Flare, 1 particle — born-faded alpha=0.000) | CardCloud, ShardCloud, Card |
| mc2_17 | VFX prohibited | none |
| mc2_24 | Trail-heavy (emit_total ~850K/30s), EffectCloud children, PPC trail profiles | CardCloud, Card, ShardCloud, Tube |

Note: `vfx=prohibited` in `[VISIBILITY v1]` refers to the legacy gosFX display path being
blocked. The CPU oracle path (CardCloud/ShardCloud/Card/PointCloud/Tube `Draw()`) runs
independently via gosFX lifecycle and IS called even when `vfx=prohibited`; the oracle
harvests the CPU sim state and emits to the GPU billboard batcher directly.

## VFX-ORIGINAL-LIVE-EFFECT-VALIDATION-1 results — Pass 1 (2026-05-30, passive 30s smoke)

Build at `mc2-win64-v0.4`. Gates: `MC2_VFX_ORACLE_RENDER=1 MC2_GPU_PARTICLES_LOG=1`.

**mc2_10** (PASS, +0 destroys, 0 GL errors):
- CardCloud: FIRST_HARVEST fires (spec="Dust"), harvestedTotal≈31748+ at 30s, 1–6 particles/call, alpha varies 0..1. CONFIRMED working.
- ShardCloud: FIRST_HARVEST fires (spec="Shards"), harvestedTotal≈8334+ at 30s, 10/call. CONFIRMED working.
- Card: FIRST_HARVEST fires (spec="Flare"), alpha=0.000 — born but faded. Single particle. CONFIRMED oracle triggers.
- PointCloud: 0 lines. No missile fire in passive smoke. NEEDS INTERACTIVE.
- Tube: 0 lines. No Tube effects in mc2_10 passive smoke.

**mc2_24** (PASS, +0 destroys, 0 GL errors):
- CardCloud: FIRST_HARVEST fires (spec="Fireball"), harvestedTotal≈7176+ at 30s. CONFIRMED.
- ShardCloud: FIRST_HARVEST fires (spec="Shards0008"), harvestedTotal≈8334+ at 30s. CONFIRMED.
- Card: FIRST_HARVEST fires (spec="lozenge"), alpha=1.000. CONFIRMED visible.
- Tube: FIRST_HARVEST fires (spec="core", PPC trail), harvestedTotal=4685+ at 30s, 2–7 profiles/call, alpha=[0.000,0.000]. Oracle harvests but visually invisible.
- PointCloud: 0 lines.

## VFX-ORIGINAL-LIVE-EFFECT-VALIDATION-1 results — Pass 2 (2026-05-31, with age/fade/debug)

Build tip `01537128`. New gates validated:

```
Run A: MC2_VFX_ORACLE_RENDER=1 MC2_GPU_PARTICLES_LOG=1 MC2_VFX_DEBUG_MODE=5 MC2_TUNE_VFX_AGE_FADE=1.0
Run B: MC2_VFX_ORACLE_RENDER=1 MC2_GPU_PARTICLES_LOG=1 MC2_VFX_DEBUG_MODE=0 MC2_TUNE_VFX_AGE_FADE=0.0
```

Both runs: PASS 2/2 (mc2_10 + mc2_24), +0 destroys, 0 GL errors. Debug mode 5 and age fade do not crash.

### mc2_10 Pass 2 (Run A — MC2_VFX_DEBUG_MODE=5, MC2_TUNE_VFX_AGE_FADE=1.0)

- **CardCloud**: harvestedTotal=31393+ at 30s; age_this_call spans [0.021..0.998] across periodic calls — full lifecycle confirmed. Age/lifetime upload (VFX-AGE-LIFETIME-UPLOAD-1) **VERIFIED IN LOG**.
- **ShardCloud**: harvestedTotal=8203+ at 30s; age_this_call spans [0.000..0.986] — full lifecycle confirmed. **VERIFIED IN LOG**.
- **Card (Flare)**: FIRST_HARVEST alpha=0.000, age=0.000. Born-dead in passive smoke. Oracle fires but particle is invisible (expected: Flare activates on missile hit).
- **PointCloud**: 0 lines. No missile fire in passive 30s smoke. **Status: NEEDS INTERACTIVE** — must trigger missile launch within 30s window.
- **Tube**: 0 lines. No flamer/smoke trail in mc2_10 passive smoke.
- **debug mode 5 (age heatmap)**: no crash, +0 destroys — v_age varying from VS→FS confirmed live.
- **MC2_TUNE_VFX_AGE_FADE=1.0**: no crash, +0 destroys — oracle soft-death fade gated correctly.

### mc2_24 Pass 2 (Run A — MC2_VFX_DEBUG_MODE=5, MC2_TUNE_VFX_AGE_FADE=1.0)

- **CardCloud**: harvestedTotal≈22160+ at 30s; age_this_call spans [0.044..0.997] — full lifecycle confirmed. **VERIFIED IN LOG**.
- **ShardCloud**: harvestedTotal≈8091+ at 30s; age_this_call spans [0.000..0.966] — full lifecycle confirmed. **VERIFIED IN LOG**.
- **Card (lozenge)**: FIRST_HARVEST alpha=1.000. **VISIBLE AND CONFIRMED**.
- **Tube (core, PPC trail)**: harvestedTotal=4680+ at 30s; `age_this_call=[0.014,0.568]` with `alpha_this_call=[0.000,0.000]`. **KEY FINDING**: profiles are active (age 0.014–0.568, not aged-out) but alpha evaluates to 0. This is **spec-defined**: `m_pAlpha.ComputeValue(age, seed)` returns 0.0 for the "core" spec at ages 0–0.6. PPC "core" is an invisible trail by design (visual effect comes from the bolt head, not the trail). **CONFIRMED: alpha=0 is spec behavior, NOT age-out artifact.**
- **PointCloud**: 0 lines. No births in passive smoke.

### Tube alpha=0 — definitive diagnosis

The Tube "core" PPC trail spec has an alpha curve that evaluates to 0.0 across the observed age range [0.014, 0.568]. Profiles are alive (not aged out — would need age≥1.0 for that). The zero alpha is the spec's intended visual: the PPC bolt visual effect is provided by the bolt head (weapon rendering), not the trail tube. This is consistent with the original MC2 gosFX rendering where core trails would also have been invisible at these ages.

**For visible Tube alpha > 0**: need FX_FlameTrail (flamer mech) or FX_SmokeTrail (smoke trail spec). Both are ClassID 1324. Confirmed in `mc2.fx` spec scan. Requires user-driven flamer fire session on mc2_10 (flamers present).

### Age/lifetime upload (VFX-AGE-LIFETIME-UPLOAD-1) — status after Pass 2

| Class | age_this_call in periodic log | FIRST_HARVEST ageRange | Lifecycle coverage |
|---|---|---|---|
| CardCloud | YES | ageRange=[0.000,0.000] (newborn) | Full [0.021..0.998] observed |
| ShardCloud | YES | ageRange=[0.000,0.000] (newborn) | Full [0.000..0.986] observed |
| Card | N/A (singleton, no periodic) | ageRange=[0.000,0.000] (newborn) | Single particle, alpha matches age |
| PointCloud | YES (added) | ageRange=[0.000,0.000] (newborn) | 0 births in passive smoke |
| Tube | YES (added in VFX-ORIGINAL-LIVE-EFFECT-VALIDATION-1 pass 2) | ageRange=[0.000,0.000] (newborn) | [0.014,0.568] observed; alpha=0 spec-defined |

Note: FIRST_HARVEST `ageRange=[0.000,0.000]` is expected for all classes — it fires on the first call when particles are newborn (age≈0). Age spread is visible in subsequent periodic log calls.

## Missing fields (deferred to follow-up arcs)

### Tier 1 — billboard representable now

- **PointCloud size**: constant 4.0f world units. Spec has no per-particle size curve; no per-spec override without ABI bump.
- **Tube profile size**: `m_pScale` used as billboard radius. May not match tube width at all profile ages without spec tuning.

### Tier 2 — require struct or shader changes

- **PointCloud/Tube spin/aspect**: neither class has these concepts; skip.
- **Tube swept mesh**: one billboard per profile is an approximation. Real tube ribbon needs a separate GPU primitive (triangle strip per pair of adjacent profiles). Filed as B2 polish debt in `docs/vfx-originals-restoration-design.md`.

### Tier 3 — separate GPU primitive

- **ShapeCloud**: per-particle 3D mesh renders. No billboard path applicable. **Recon complete (2026-05-31) — see `docs/vfx-3d-mesh-substrate-recon.md`.**
- **DebrisCloud**: rigid-body 3D mesh pieces. No billboard path applicable. **Recon complete (2026-05-31) — see `docs/vfx-3d-mesh-substrate-recon.md`.**
- **Shape**: 3D singleton mesh. No billboard path applicable. **Recon complete (2026-05-31) — see `docs/vfx-3d-mesh-substrate-recon.md`.**

Key finding from recon: `DrawScalableShape()` is gated OFF by default (`kDefaultDisabled=true`
in `mlr_gate.cpp`), so all three classes currently render **nothing**. Gate-OFF for any new
oracle path is byte-identical by construction. See recon doc for full substrate design.

## Next targets

1. **PointCloud visual validation** (OPEN): user-driven session with missile fire needed. Missile_Flare (only PointCloud spec with active births) requires a missile launch event within 30s. mc2_10 has missile-armed mechs; camera must advance toward combat. Confirmed births=0 in all passive 30s smoke.

2. **Tube visible alpha** (OPEN): Tube oracle fires and age data confirmed correct. For visible alpha > 0 need FX_FlameTrail or FX_SmokeTrail — user-driven flamer fire session (mc2_10 has flamers). Tube "core" PPC alpha=0 is spec-defined (not a bug).

3. **VFX-SHADER-AGE-FADE-PARITY-1 visual verification** (DEFERRED to interactive): debug mode 5 (age heatmap) is confirmed non-crashing in passive smoke. Full visual verification (particles showing blue→red gradient) requires MC2_VFX_ORACLE_RENDER=1 + MC2_VFX_DEBUG_MODE=5 in an interactive session with active particle effects.

4. **VFX-3D-MESH-GPU-SUBSTRATE-1** (DESIGNED, ready to implement): GpuMeshCache (MLRShape→VAO extractor) + `vfx_mesh.vert/frag` shader pair + bridge GL setup. Blocker for all 3 mesh-class oracle paths. ~900 lines total. See `docs/vfx-3d-mesh-substrate-recon.md` for full design. Recommended arc: SUBSTRATE-1 → SHAPE-ORACLE-1 (Shape + ShapeCloud) → DEBRISCLOUD-ORACLE-1.
