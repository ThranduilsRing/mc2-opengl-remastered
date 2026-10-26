# MLR/gosFX retirement P1 runtime inventory (2026-10-25)

Base `b66ad0b0983e` (current nifty when branched), Vulkan default. This is a source closure
plus the runtime evidence listed below, not a claim that all registered classes spawn in
stock missions. The requested older `mlr-gosfx-inventory` was not present in main's
`.claude`/`docs` or the supplied memory tree (including ignored files); this inventory
updates the retirement design against the actual base.

## Default routes and class-specific blockers

| Class | Default Vulkan route and call sites | P1 promotion/removal limit |
|---|---|---|
| Card | `card.cpp::Draw` -> MLRCardCloud -> `MLRClipper::DrawEffect`; master=1 harvests CPU color/size/UV to Batcher. | UV sampling fixed; camera-axis alignment, size floors and shader depth pull/brightening still differ. |
| CardCloud | `cardcloud.cpp::DrawState` -> `MlrCloudDraw::submit` -> MLRCardCloud; master=1 harvests live particles. | UV sampling fixed, including per-particle scrolling/animated rectangles; arbitrary 3D particle orientation still reduced to billboard spin. |
| PointCloud | `pointcloud.cpp::Draw` -> MlrCloudDraw -> MLRPointCloud. | GPU route emits fixed-size billboard proxies; no populated per-class image proof. |
| ShardCloud | `shardcloud.cpp::Draw` -> MlrCloudDraw -> MLRTriangleCloud. | GPU route replaces authored triangle geometry/per-vertex colors with a billboard and one color; cannot promote on packet-state equality alone. |
| Tube | `tube.cpp::Draw` -> native swept mesh when supported and textured; fallback to MLRIndexedTriangleCloud otherwise. Tube gate already ON independently of particle master. | P1 preserves authored wrap/clamp UVs. Missing texture/untextured fallback, complete profile/blend coverage and clip-plane proof remain. Never substitute profile billboards. |
| Shape | `shape.cpp::Draw` -> native cached indexed mesh (shape gate already ON), else `MLRClipper::DrawScalableShape`. | First-material aggregation, unsupported primitives and missing material/mesh fallback prevent complete parity. |
| ShapeCloud | `shapecloud.cpp::Draw` -> DrawScalableShape per live particle/orientation. | No GPU particle-cloud mesh route; all material spans/particle transforms need migration. |
| DebrisCloud | `debriscloud.cpp::Draw` -> DrawScalableShape. | Still CPU geometry and MLR shape submission; must retain lifecycle/bounds as well as debris appearance. |
| PertCloud | `pertcloud.cpp::Draw` -> MlrCloudDraw -> MLRNGonCloud. | Stock zero-spec memory is not a mod-format reachability proof. Class remains loadable and covered by frozen tests; retain it. |
| EffectCloud | Child effects per live particle; base Effect::Draw recursively draws children with combined state/transform. | Preserve nesting and execution ordering when moving to GPU. |
| PointLight | CPU curves -> particles::LightManager sentinel/no-op ChangeLight/DeleteLight. | No working scene-light replacement; retain wire class until codec migration. TG sun/ambient and weapon/explosion lights are independent. |

`GameCamera::render` starts MLR with the camera, draws objects/effects, resolves MLR texture
pool indices, flushes GPU billboards, deferred tubes and meshes after renderLists, then
flushes the MLR sorter in the composited Vulkan VFX tail (`code/gamecam.cpp`). The MLR
sorter still submits CPU-transformed triangles to GameOS; it is not a pixel rasterizer.
`MC2_DISABLE_GOSFX` defaults false, contrary to the historical known-issues note.

Cloud paths use effect-to-clip transform, clip flags and cloud-specific Clip/triangulation
(`mlrcardcloud`, `mlrpointcloud`, `mlrtrianglecloud`, `mlrngoncloud`, `mlrindexedtrianglecloud`).
Scalable shapes use shape transform, backface/light/material processing and indexed
TransformAndClip/TransformNoClip in immediate or deferred sorter paths. Shape, ShapeCloud
and DebrisCloud supply zero MLR active lights. MLRLightMap init is commented out; compiled
lightmap helpers are not evidence of a live lightmap draw. MLR classes/state/texture pool
also remain serialization dependencies of gosFX, editor, mc2fx and preview.

## Proven dead and removed

- `particles::Spawn`, its five SpawnCard/CardCloud/Point/Shard/Tube helpers and twelve
  source/header files had no external caller. The master draw branches already harvested
  live CPU particles regardless of `MC2_VFX_ORACLE_RENDER`; no live branch needs snapshots.
- The sole `MC2_VFX_AGE_SAMPLE` reader was the dead snapshot age helper. Removed its gate
  row and generated readers; preserved the retired feature ordinal for compatibility.
- All MLR `.cpp` files in this base are listed in its CMake source closure. Registered
  geometry/lighting classes are not deleted merely because stock effects omit them.
- `gpuTrailKindProven` returns false: the alternate GpuTrailEmitter ownership path is
  dormant for proven weapon classes. Do not enable it during master promotion; the gosFX
  Tube path owns the real swept profiles. It remains a separate retirement candidate.

`MC2_VFX_ORACLE_RENDER` still suppresses hit/miss/water-hit and post-hit muzzle Draw calls
in `code/weaponbolt_render.cpp`; it is not the live-harvest selector described in old docs.
Promoting it changes the visible effect set. P2 must remove/tune these workarounds with
impact fixtures rather than marking this default-OFF gate harmless.

## Root causes addressed and proof limits

The active GPU Card/CardCloud code sampled UV offset/size curves at `(age,seed)=(0,0)` and
ignored animated width==1 atlases. CPU Execute had already baked the correct UVs; P1 now
submits those live rectangles without another curve/frame evaluation. Adjacent equal
rectangles coalesce in the batcher without reordering particles.

Vulkan's fragment shader unconditionally clamped tube UVs, while Tube::AnimateProfile
advances U by `profile_index * pUSize + UBias`. Out-of-range repeated coordinates therefore
sampled the edge texel across whole panels: a concrete flat-sheet mechanism. P1 carries
the authored clamp bit through the existing bridge ABI; repeating tubes use the repeat
sampler. This identifies a current defect, not an assertion that every 2026-06-15 symptom
had that single cause. Existing native indexed topology/textured Vulkan submission already
superseded the historical GL billboard-per-profile/white-sheet paths.

CPU MLR projection/clipping retains historical conventions (`mlrclipper.cpp::StartDraw` and
`MLRClippingState::Clip4dVertex`); image differences against a broken CPU reference cannot
be blessed as parity. Render-state tests alone are insufficient for a master default flip.

P1 deletion control: 680 frozen CPU/MLR snapshots, 1,439,488 bytes, byte-identical before
and after dead-spawner removal; SHA256 `1bc5ee1321df5395e17c8169abc64745223fe7b5889baa785332ec511c821708`.
No authoritative simulation, gosFX Execute/emission/RNG ordering or completion logic is
changed. Existing effects can be camera-dependent; frozen draw/flush/clip tests check
that rendering consumes zero additional legacy RNG. Runtime sim proof is recorded below.
