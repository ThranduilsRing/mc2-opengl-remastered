<!-- doc-status: stale | reviewed:2026-10-11 -->
# gosFX Effect Animation — Full Lifecycle Recon (load → spawn → animate → hit → unload)

**Goal:** clean path to replacing animations. **Date:** 2026-06-10. Branch: nifty-mendeleev worktree.

## TL;DR
- gosFX = two-layer: `mclib/gosfx/` (CPU sim, the **animation authority**, gameplay-load-bearing) + `mclib/particles/` (render bridge: CPU-oracle harvests live sim → GPU billboards).
- **Animation is defined entirely by `Effect::Specification` curve members** (fcurve.cpp) loaded from one binary blob `data/effects/mc2.fx`. Replace an animation = edit/replace the Specification curves. That is the clean seam.
- **GPU migration is NOT a prerequisite.** Sim/animation lives on CPU and is the source the GPU billboards already draw from. Authoring/replacing animation is a CPU/spec-side concern; the oracle→billboard bridge picks up changes for free for the 5 routed classes. GPU-sim compute is a downstream perf track (compare-only CardCloud probe, do-not-touch per trackv arc).

---

## STAGE 1 — LOAD / PARSE (disk → in-mem template)
- Entry: `code/mechcmd2.cpp:~1648` opens `data/effects/mc2.fx` → `EffectLibrary::Instance->Load(stream)`.
- FourCC `'GFX#'` (`gosfx.cpp:8`), version 17 (`gosfx.hpp:38`), `ReadGFXVersion` `gosfx.cpp:124`.
- Real parser: `mclib/particles/spec_library.cpp:53-68` — loop `Effect::Specification::Create(stream,ver)` (factory dispatch `effect.cpp:299-318` by ClassID), stored in `SpecLibrary::m_effects[]`, `m_effectID=index`.
- **Animation params = Specification curve members**, all parsed via `Curve::Load` (`fcurve.cpp:102-174`). Curve types: Constant/Linear/Spline/Complex + `SeededCurveOf<>` (per-instance random variance).

### Where animation actually lives (edit these to swap an animation)
| Channel | Spec member | File |
|---|---|---|
| Effect lifetime | `m_lifeSpan` | effect.hpp:65-161 (base) |
| Color/alpha (cloud) | `m_pRed/Green/Blue/Alpha` | particlecloud.hpp:50-81 |
| Color/alpha (single) | `m_red/green/blue/alpha` | singleton.hpp:50-61 |
| Scale/size | `m_scale` / `m_size` / `m_halfHeight,m_aspectRatio` | singleton/shardcloud/card.hpp |
| Texture-frame anim | `m_index`(+`m_animated`), `m_UOffset/VOffset/USize/VSize` | card.hpp:62-75 |
| Particle physics | `m_startingSpeed,m_pAcceleration*,m_pDrag,m_pLifeSpan` | particlecloud.hpp |
| Emission rate | `m_particlesPerSecond,m_startingPopulation` | particlecloud.hpp |

**Spec is immutable template.** Runtime instances hold ptr to spec + evaluate curves. Replace = modify spec curves + re-serialize `.Save()`.

---

## STAGE 2 — SPAWN / ACTIVATION (template → live instance)
- Game spawns via `EffectLibrary::MakeEffect(name,flags)` (`effectlibrary.cpp:97-119`) → factory `new` (NO pool). Then `Effect::Start(ExecuteInfo)` (`effect.cpp:525`) seeds age/seed/world-transform (`m_localToParent*parentToWorld`, effect.cpp:587).
- Call sites: weapon trail/muzzle/hit/miss `weaponbolt.cpp:2530/2550/2570/2590`; mech dust-poof `mech3d.cpp:1604/1625`; jumpjet `mech3d.cpp:1662`; artillery `artlry.cpp:797/815`; bldg destruct `terrobj.cpp:1299`.
- Attach = world matrix passed each frame (foot node `mclib/mech3d_render.cpp:283`, jumpjet node `mclib/mech3d_render.cpp:310`). EffectCloud children follow via `m_localToParent` (`effectcloud.cpp:205`).
- **No global active list.** Effect stored on owner object (WeaponBolt/Mech field); owner re-drives it each frame guarded by `IsExecuted()`. Children live in parent `m_children` chain.
- **Variant selection = which spec name at spawn.** No mid-flight swap; trail→hit = kill+delete one, spawn another instance (`code/weaponbolt_update.cpp:204-208`). Seed (`-1`→random, effect.cpp:557) = only per-instance variation knob.

---

## STAGE 3 — PER-FRAME ANIMATE + RENDER (the motion compute = replace target)
- Tick: `Effect::Execute(ExecuteInfo)` `mclib/gosfx/effect.cpp:603/601` — called by game objects with `scenarioTime` (e.g. artlry.cpp:894+). ParticleCloud override `particlecloud.cpp:358` loops particles → `AnimateParticle(i,matrix,time)`.
- **Curve eval per particle per frame** (CPU): `ComputeValue(age,seed)` `fcurve.hpp:131/187/246`. age=lifetime 0..1, seed=per-particle.
  - SpinningCloud base physics: drag/ether/accel `spinningcloud.cpp:454-511`, integrate `519-522`.
  - CardCloud: RGBA+UV+sprite-index `cardcloud.cpp:418-454`. ShardCloud: RGBA `shardcloud.cpp:249-252`. PointCloud: ether/accel `pointcloud.cpp:344-377`.
- Draw (per-cloud, once/frame): build billboard verts CPU (`cardcloud.cpp:534-866`) → `info->m_clipper->DrawEffect(&dInfo)` → MLR cloud GPU object → OpenGL.

### CPU-vs-GPU (current)
- **CPU:** ALL curve eval, physics integration, billboard matrix, vertex transform.
- **GPU:** vertex-buffer storage (MLR cloud objs), rasterize/blend only.
- **Render bridge:** `MC2_VFX_ORACLE_RENDER` default-OFF (`batcher.cpp:137-141`; opt-in `=1`) — oracle harvests live CPU sim each frame → GPU billboards. 5 classes routed (`spawn.cpp:81-105`): CardCloud, ShardCloud, Card (confirmed), PointCloud (partial), Tube (routed but render falls back CPU-MLR — billboard gave "ladder" artifact).
- GPU **compute sim**: CardCloud only, compare-only, default-OFF (`cardcloud_sim.comp` = age+pos only, no curves). Not a render consumer.

---

## STAGE 4 — HIT/COLLISION
- **gosFX has NO collision.** Zero terrain/ground/raycast in gosfx/*. "Hit" = weapon system (game-side physics) detects impact, spawns separate hitEffect (`weaponbolt.cpp:628`), kills trail. Effects run to lifetime expiry only. Replacement **must not** expect terrain-hit callbacks.

## STAGE 5 — UNLOAD / DESTRUCTION
- Death = `HasFinished()` (`effect.cpp:832-851`): age≥1.0 AND no children. Execute() at age≥1 → loop or `Kill()` (`effect.cpp:742`).
- Destroy = direct `delete`, NO pool. dtor deletes m_children (`effect.cpp:484-495`), bumps `g_effectDestroys`. EffectCloud deletes particle->m_effect on expiry (`effectcloud.cpp:264-291`).
- Owner = game side for top-level (weaponbolt/gameobj); parent for children.
- Mission-end: `code/mechcmd2_frame.cpp:238 TerminateClasses` → `SpecLibrary::Shutdown` deletes all specs (`spec_library.cpp:43-50`).

---

## CLEAN PATH TO REPLACE ANIMATIONS (the seam)
The replaceable unit is the **`Effect::Specification` + its `Curve` members**, loaded from `data/effects/mc2.fx`. Three escalating options:

1. **Data-only swap (lowest risk):** author new curve values in a Specification, re-serialize the `.fx` blob (`.Save()` path exists). No code change. Flows through CPU sim → oracle → GPU billboards automatically for the 5 routed classes. Good for retuning color/size/lifetime/emission of existing effect types.

2. **New curve/motion model (medium):** add curve types or replace `ComputeValue` semantics in `fcurve.*` + the `AnimateParticle` consumers (`spinningcloud/cardcloud/shardcloud`). Still CPU-authoritative; parity preserved through oracle. Required if new animation needs motion the current Constant/Linear/Spline/Complex curves can't express.

3. **Full animation backend replacement (high):** keep the 6 load-bearing contracts and replace the sim internals:
   - `MakeEffect`/`Start(ExecuteInfo)` spawn signature
   - `Execute()` returns `IsExecuted()` (false ⇒ parent deletes) — load-bearing cleanup signal
   - `HasFinished()` lifetime contract (age≥1 && childless)
   - event-driven child birth on timeline (`effect.cpp:664-670`)
   - EffectCloud 1:1 particle→child-effect nesting
   - `Kill()` orphans children
   - Draw handoff `m_clipper->DrawEffect` (or new bridge)

**Recommendation:** GPU-sim is NOT a prerequisite — orthogonal perf track, do-not-touch (trackv arc). Build animation replacement on the **CPU spec/curve authority**; the existing oracle→billboard bridge renders it. Start at option 1/2 (spec+curve seam); reserve option 3 only if the curve model itself is being thrown out.

## Open / verify-before-acting
- `.Save()` round-trip path for `.fx` — confirm editor/cook tooling can re-emit (asset-viewer mod workbench may already do spec authoring; see HANDOFF asset_viewer).
- Tube + 3 mesh classes (ShapeCloud/Shape/DebrisCloud) render CPU-MLR, not billboard — animation swaps there bypass the oracle GPU path.
