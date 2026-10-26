# MLR/gosFX retirement P1 runtime inventory (2026-10-25)

Base `b66ad0b0983e` (current nifty when branched), Vulkan default. This is a source closure
plus the runtime evidence listed below, not a claim that all registered classes spawn in
stock missions. Prior inventory: `E:/mc2-archive-2026-10/plans/mlr-gosfx-inventory-2026-10-19.md`
(read in full). This update confirms its live routes against the actual base; archived
zero-caller claims are deletion candidates, not serialized/mod-corpus proofs.

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
  Tube path owns the real swept profiles. Update can still invoke GpuTrailEmitter when a
  gosFX spec is missing, so the emitter is not globally dead.

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


## P1 runtime verdict and handoff

Engine/proof commit `0b1ab506bab0`; subsequent harness/document edits do not change game
sources. Owner montage: `E:/mc2-archive-2026-10/proofs/mlr-retire-p1/owner-montage.png`.
Raw sidecars, commands, logs, repeat hashes and crop differences are retained alongside it.

- **No class accepted at parity. No master/render gate flip.** The fired PPC/flight/impact
  fixtures exercise Card, CardCloud and native Tube. PPC trails are blue and continuous,
  but head sprites differ. Candidate impact flashes are substantially reduced/missing,
  consistent with the render gate's explicit hit/miss/muzzle suppression. White panels
  in the flight scene also occur in the CPU control: they are not a GPU-only regression.
  Do not claim the UV clamp fix resolves every historical sheet symptom.
- All three scenes drift between repeats; PPC CPU repeat 1 timed out during cold load.
  Shape/ShapeCloud/DebrisCloud/PertCloud/PointCloud/ShardCloud have no accepted populated
  image fixture in this packet. Unit harvest coverage is not visual parity. P2 must pin
  per-effect age/state, identify the shared panel-producing spec/geometry, and fix sprite
  orientation/material/impact routing before promotion. Camera-dependent gosFX ordering
  and the legacy random stream remain relevant to repeatability.
- Default day smoke observed 23 Effect roots (`Vehicle_Dust_Cloud`) and 23 CardCloud
  starts (`Dust`), but zero MLR enqueue calls; starts do not establish visible draws.
  The source reachability table therefore remains broader than the observed fixture set.
- Build PASS; eight selected shader/VFX/gate rows PASS (70 Vulkan shaders); frozen VFX
  native tests PASS; oracle Python tests 137 PASS; lease/capture tests 53 PASS; scripts
  covering row 63 modules green. The lease edit's covering row took 185.84 s: add a
  targeted harness row rather than paying the aggregate for a one-line change.
- Vulkan validation was enabled in all 14 retained budgeted-capture/smoke game logs:
  zero errors and zero warnings. Smoke-lite: mc2_01 PASS (1,317 frames), mc2_10 timeout
  at 70.5 s before a frame. Do not call the combined smoke green.
- Deployment copied the requested 12 GiB base and payload, then **refused** the copied
  kit's extra `mods/mc2-industrial-kit/data/model_overrides/quonset/detail/2_orm.ktx2`.
  Per lane rule 2, no check was bypassed and deployment was not retried. Completeness
  check passes, but there is no deploy manifest: all captures are diagnostic evidence.
  Reconcile this pin drift through the owning lane before P2's release proofs.
- Determinism is **unproven**: captured logs have only 192 common frames (tool requires
  300; threshold unchanged). The requested 20 s fired CPU/GPU pair queued behind batch
  `w65-fixups-final`, whose two-core-per-slot lease is incompatible with this lane's
  required one-core slots. Only this lane's queued processes were stopped. Native
  deletion snapshots/RNG checks remain green; they are not a full mission chain proof.
- The old capture lease heuristic reserved twelve threads. Its first attempt was stopped;
  visual capture/replay now uses normal leases, with tests. Subsequent game affinity logs
  show two logical CPUs at idle priority with host slots=2, cores per slot=1.
- DrawShape retains a LAB_ONLY internal caller. DrawScreenQuads/sorter support is a
  zero-game-caller candidate; registered geometry/light IDs and vtable/codec closure need
  the all-target/corpus checkpoint before broad MLR deletion. No such class is declared
  globally dead merely from a stock-content observation.

P1 stops here. Keep only `E:/mc2-installs/claude-mlr-retire-p1`; source kit install and mods
are untouched. No auxiliary install/staging was created; disk freed: 0 GiB.


## Follow-up: authored cards (2026-10-26)

Engine `38f5d620e696`; harness `03415190f`. `p2-checks-summary.json` and
`p2-oracle.log` in the same proof directory are the new evidence. P1 captures
remain diagnostic controls; no new images were produced and no class is promoted.

Card/CardCloud now reuse the original draw's world corners, camera-axis alignment,
live UVs and combined material state through the existing 64-byte native geometry
ABI. The approximate billboard branches (radius/floors/spin/depth pull) are removed
for these classes. Weapon muzzle/hit/miss/water-hit submission no longer depends
on the render-oracle gate. This removes 257 engine lines, adds 107 (net -150).
CPU MLR Draw calls remain as reference arms pending visual acceptance. CPU Execute,
emission, random streams and combat completion are unchanged.

Build passes. Eight shader/VFX rows pass (70 shaders; no shader edits). Frozen
CPU and both GPU gate-vector tests pass: 10,931 CPU assertions and 11,066 per GPU
vector, including all four CardCloud alignment modes. Host-lease tests: 31 pass.
The native harness selected the cold VS tree and took 435 s; use the existing
cached tree rather than accidentally creating a cold compile checkpoint.

The earlier mc2_10 timeout log ends during cache-miss model imports before its
first frame. The copied kit also contained one unpinned quonset ORM; that file
was archived under this proof directory from THIS install only. The owning kit
install is untouched. Normal deployment now passes the original pin checks,
cooks all 693 pinned GLBs and writes its 3,751-row manifest for the engine commit.
Whether that resolves smoke timeout remains unverified because no game launched.

The foreign lease blocker recurred and is root-caused: older worktrees default
`MC2_HOST_CORES_PER_SLOT` to 2. A width-two reservation has affinity 0xF0 and
occupies both physical cores allowed by the owner's two width-one slots; allocating
this lane's second slot (0xC0) would overlap it. This checkout now defaults to one
core, with tests. It cannot change a running foreign holder's width. Oracle waited
180 s behind live batch `w65-pin-checkpoint` (PID 33648, mc2-ds4), then refused.
No foreign holder, record or process was changed; no game or child remains from
this attempt. Smoke/determinism/new validation were skipped under the same blocker.

Old repeat analysis: flight CPU repeats differ at 115 full-image pixels, impact
at 113; complete birth transcripts and common CardCloud draw-probe prefixes match.
Even duplicate frozen poses within the old impact controls differ (19 / 4 pixels),
including background samples. This narrows the old drift but does NOT establish
its rendering root cause or justify a tolerance. Do not mark repeatability fixed.

Next: obtain a permitted width-one game slot, run the existing p2 oracle plan
against the deployed engine, inspect heads and impact flashes, isolate the sparse
repeat differences with controls, and provide populated fixtures for the six
remaining unpictured leaf types. Only then promote supported classes and remove
their CPU Draw paths. Point/Shard proxies cannot ride a blanket master flip.
No gate flips, no new accepted class parity, no live CPU path removal in this follow-up.
One lane install remains; auxiliary installs created: zero; disk freed: zero.


## P2 relaunch: repeatable rejection and dead-route retirement (2026-10-26)

Rebased onto merged nifty `b91d03ebc` after window 67 brought the original
`bd8597a66` checkpoint in. New engine/proof base `254f9f5493db`; final selector
retirement `d2447791c`. Evidence: `p2-resume-summary.json`,
`owner-montage-p2.png`, `p2-accepted-base-oracle/`, `p2-all-targets.log`, and
`p2-determinism-final.log` under the existing P1 proof directory.

**No native class accepted; master remains OFF; no live CPU class renderer deleted.**
All six CPU/GPU arms are now byte-repeatable across launches, with clean matching
fingerprints and Vulkan present readbacks. The authored Card route still drops
PPC/laser head flares; flight/impact also differ visibly. Exact world-corner/UV
submission is insufficient. Native projection/depth/material behavior needs a
pass-level comparison against MLR before these CPU references can be removed.
No depth-disable workaround, size floor, tolerance or replacement look was adopted.
The master experiment deliberately held Tube/Shape selectors at zero to isolate
Cards: it does not establish new native Tube/Shape parity.

The repeat root cause found here is vegetation wind: `GameCamera::render` passed
wall-clock `gos_GetElapsedTime()` through `GosVegetation::flush` to the vegetation
vertex shader, even while the oracle froze simulation/gait. Wind now uses zero
under the existing `MC2_SMOKE_FREEZE_ANIM` switch; play retains its normal clock.
All 24 images (three scenes, two arms, two launches, two bookmarks) repeat exactly
at their corresponding bookmark. Within a sweep, the two frozen poses still
have deterministic beam differences (PPC CPU 13,505 pixels; flight 7,283;
impact 3). Their separate cause is not established; do not claim every render
input is frozen or grade a duplicate as an independent same-frame control.

Deletions in this relaunch:
- The uncalled `DrawScreenQuadsInformation` / `MLRClipper::DrawScreenQuads` /
  sorter `AddScreenQuads` closure and its quad draw/unimplemented alpha-sort
  dispatch: 174 engine lines deleted, 2 added. Internal SortData dispatch is
  compacted; serialized effect class IDs and MLR state formats are retained.
- Incorrect PointCloud square and ShardCloud billboard candidate branches:
  180 engine lines deleted, 2 added. Both selectors now retain their authored
  CPU primitives in every master setting, including all Shard alignment modes.
- The now-uncalled render-selector getter: 27 engine lines deleted. Removed
  `MC2_VFX_ORACLE_RENDER` from the typed gate table/readers/commands and retained
  feature ordinal 15 as Retired. Historical oracle records still validate their
  original selector value. This is a retired default-OFF gate, not a promotion.

The freshly built mc2fx tool inventories **904 stock specifications**: Card 136,
CardCloud 388, PointCloud 2, ShardCloud 24, Tube 31, Shape 10, ShapeCloud 9,
DebrisCloud 34, EffectCloud 39 and Effect 231; PertCloud 0. Point/Shard are not
rare: earlier force-fire traces instantiate Missile_Flare and Shards0008.
Shape/ShapeCloud/DebrisCloud have content but no populated fixture in this
force-fire plan. PertCloud remains loadable by mods and is not dead from its
zero stock count. No shutdown class totals were available in the new early-exit
capture logs; specification counts are not visible-coverage evidence.

Proof at `254f9f5493db`: RelWithDebInfo game build; nine covering shader/VFX/MLR
rows; frozen CPU 10,931 assertions and GPU 12,339 per vector; 128 oracle Python
tests. All-targets checkpoint passes game/editor/viewer/mc2fx/preview/GUI,
standalone projects and test builds. CPU/MLR deletion control: all 680 snapshots,
1,439,488 bytes, byte-identical before/after; SHA256
`d2312095ae3016048a372397ffde2a84ae3e588eb0c0e906c7ff679a6d8c36fa`.
All 12 oracle logs have validation enabled and zero errors/warnings/VUIDs.

The fired master OFF/ON pair matches **5,089 shared sim frames**, including strict
chain `5070196a9b861a6e` and RNG counts `8543/1168/154237`, animation RNG 27.
The tool's overall baseline verdict is FAIL: copied/pinned install data fingerprint
`174547931cb5fc53` differs from recorded `0dfcd001ffde73d0`; its stock component
also differs. Baselines were not changed. Camera-dependent legacy effect ordering
remains relevant to later simulation migration; no Execute/emission/RNG ordering
or weapon/Carnage completion logic changed here.

Next packet: isolate native Card projection/depth/material loss using the now
repeatable flare captures, then remove each accepted CPU Draw route and promote
only its supported routing. Implement original Point/Shard primitives next; retain
mesh/cloud/Pert fallbacks until their populated class fixtures exist. Keep the
P3-P5 codec, MLR/editor/tools and authoritative-lifetime plan above.

Final selector-retirement checks: engine `d2447791cb39` builds and deploys cleanly,
693 pinned GLBs verified/cooked, 3,764-row manifest. Five final gate/VFX/MLR rows
pass; 128 Python tests pass; corrected final frozen tests pass CPU 10,931 and
GPU 12,339 assertions. A stale typed override in the separate lifecycle test
driver was removed after the compiler caught it. The final CPU snapshot still
matches all 680 pre-deletion records exactly. This follow-up removes 382 engine
lines, adds 7 (net -375); no live CPU class renderer is retired.

**Smoke-lite FAIL:** mc2_01 reaches the 70.4 s wall cap before its first frame;
mc2_10 passes with 489 frames. Both validation logs have zero errors/warnings.
The advisory deploy-coherence scan also exceeds 30 s; the actual deploy's full
isolation/hash/kit/manifest checks and smoke's 3,764-file staleness check pass.
Raw smoke artifacts are copied into `p2-smoke-lite-artifacts/` in the proof
folder. No timeout, threshold or stored baseline was relaxed.

Tool slowness: the shared-header checkpoint rebuilds 1,502 root steps (472 s).
Removing one gate index then recompiles 484 engine steps; the importer's changed
binary identity forces another 693-GLB cook despite unchanged model inputs.
Separate the cook recipe identity from unrelated gate/header binary changes.
Only `E:/mc2-installs/claude-mlr-retire-p1` remains from this lane; auxiliary
installs created zero, disk freed zero. No game/build child remains at handoff.

## Packet 4 investigation (2026-10-26): composition mismatch, not accepted

Resumed `1fcfc2113` without repeating the earlier deletions. Diagnostic engine
`dab6e900587e`; proofs `p4-summary.json`, `owner-montage-p4.png`,
`p4-probe-oracle/`, `p4-cpu-oracle/` and `p4-smoke-lite-artifacts/` in the
existing P1 proof directory. Both PPC bookmarks repeat byte-for-byte in both
arms. Native Cards still lose the head flares. **No new class accepted, no
gate flipped, no CPU route deleted, zero retirement lines deleted. Packet 4
implementation is incomplete.**

The two arms do not share a composition contract:
- `GameCamera::render` wraps the deferred `theClipper->RenderNow()` in IsHUD
  and scale exemption. `gosRenderer::drawTris` records these triangles into
  `hudBatch_`, replayed through the Vulkan present tail after post-processing.
- `engine_present_vk.cpp` converts textured HUD vertices into
  `HudTexGpuVertex {x,y,argb,u,v}`. It discards Z and RHW. `hud_tex.vert`
  emits W=1 and Z=0; the HUD pass has only a color attachment. The fragment
  shader fetches clamped nearest RGBA8 texels and multiplies packed RGBA8 tint.
  UV interpolation is therefore affine. This is the actual CPU reference,
  irrespective of the MLR specification's depth/filter/address settings.
- Native Card/CardCloud triangles instead enter `vulkan_vfx_island` before
  post-processing, use the shared scene depth with GEQUAL, preserve world
  projection/W, interpolate float tint and UVs, and sample the texture through
  a sampler. Native routing also collapses material modes into alpha/ONE-ONE.
  Moving authored world corners did not migrate any of these pass semantics.

This is a concrete source-level mismatch and a mechanism for occluded/lost
flares; it is not an isolated raster/depth causality measurement. Disabling
depth alone would leave composition, sampling and ordering differences.
The temporary Card log is removed. Its world/color/state values were useful;
its `clip` column is **not valid projection evidence**: the public clipper
getter reads a member that StartDraw never populates (the live product uses
the parameter). Do not use that column to infer a projection defect.

PointCloud's CPU primitive is a screen-aligned right triangle, 2.4 pixels per
type unit (type zero also means 2.4), not a square or world-sized billboard.
ShardCloud uses the three authored aligned vertices and per-vertex color.
Neither has a native replacement in this packet. Their prior proxy deletions
remain intact; zero new Point/Shard parity is claimed.

Seven targeted shader/VFX/MLR rows and the RelWithDebInfo game build pass.
Smoke-lite now passes both maps: day 1,144 frames / 24,406 ms load, night 434
frames / 20,562 ms load; each reports zero validation errors and warnings.
The old day timeout did not reproduce. Its last marker was mission loading,
`texture_prewarm_ready` at 55.42 seconds, before the first FX draw. Timing/load
contention is a hypothesis, not an established root cause. No timeout changed.
No simulation code changed; the previous strict-chain proof is retained,
not counted as a newly executed pair. Runtime captures here cover PPC only.

## Packet 5: accepted Card/CardCloud, CPU renderer deleted (2026-10-26)

Acceptance now follows the owner's visibly-correct, scene-depth bar, superseding
the Packet 4 requirement for equivalent HUD composition. Engine `435a6394d9bc`
restores beam-head/impact flares with the existing billboard's size-based camera
lift applied to native additive card vertices. Geometry still uses scene GEQUAL
depth testing and no depth writes; alpha cards retain their actual world depth.
The actual GPU-matrix probe (`p5-depth-probe/`) replaces the invalid prior clipper
getter evidence. A tiny constant bias exposed only flare fragments. The attempted
axis-conversion removal in `625597db4ab7` was incorrect and is absent from the
accepted engine: the terrain MVP already bakes the MC2-to-render conversion.

- **Accepted:** Card and CardCloud. `MC2_GPU_PARTICLES` defaults ON; =0 suppresses
  their draws. Both CPU Draw routes, cloud ownership, four duplicated Card colors
  and the complete 925-line `MLRCardCloud` implementation/header are gone, together
  with its initialization, termination, include and CMake entry. The numeric class
  ID slot remains, preserving the other IDs. Engine diff from `f0ec4f452`: 1,010
  lines deleted / 33 added (net -977); generated gate views and tests are separate.
- **Already finished before P5:** snapshot spawners, inaccurate Point/Shard proxies,
  unused screen quad, `MC2_VFX_AGE_SAMPLE` and `MC2_VFX_ORACLE_RENDER` removal.
  Tube/Shape oracle gates were already default ON. Do not count those as new flips.
- **Not retired:** PointCloud has no native screen-sized triangle renderer;
  ShardCloud has no native authored triangle/per-vertex-color renderer. Tube still
  falls back for untextured/unsupported content; two pale strips in the missile
  scene appear in both CPU and native arms and are not claimed fixed. Shape's
  fallback/submaterials and ShapeCloud/DebrisCloud/PertCloud MLR routes remain.
  These are explicit remaining implementation/coverage gaps, not pixel-diff rejects.

The owner montage is `owner-montage-p5.png` in the existing proof directory. Left:
CPU reference `p5-card-oracle/` at `625597db4ab7` (master/Tube/Shape OFF). Right:
`p5-final-oracle/` at `435a6394d9bc` (native defaults including Tube/Shape ON).
Day PPC flares and night missile flight/impact visibly render; all six native logs
enable Vulkan validation with zero VUID/nonzero-error reports, and each bookmark
repeats byte-for-byte across processes. Alpha/additive appearance may differ from
the legacy HUD tail; texture UV/lifetime state still comes from live gosFX.

The final 11 covering gate/shader/VFX/MLR rows and game build pass. The all-target
checkpoint builds game/editor/viewer/FX tools, standalone tools and doctest corpus;
final targeted VFX tests pass, including 12,635 assertions in the GPU lifecycle
child. Temporary projection logging is removed. No Execute, emission, curve,
lifetime, authoritative completion or RNG ordering changes were made. See the
design doc's new next-packet section and `p5-summary.json` for final runtime checks.

Final runtime checks: smoke-lite passes mc2_01 and mc2_10 with zero validation
VUID/error/warning reports. A fresh default-duration forced-fire particles OFF/ON
pair matches 2,664 shared sim frames, graded/strict chain `cafa7791aad937ed`, full
chain `0d9fefb7dceb7e8c` and RNG counts `47/285/0`, animation RNG 6. No baseline
was modified. `p5-determinism/` and `p5-smoke-lite-artifacts/` retain the raw logs.
The deployed engine remains `435a6394d9bc`; the final commit only records these
notes. One lane install remains, `E:/mc2-installs/claude-mlr-retire-p1`; auxiliary
installs created zero, disk freed zero. The kit-parity install was left alone.

## Packet 6 runtime closure (2026-10-26)

Engine `6a39e5262e35`, window-68 base `b6a87c27b`. PointCloud and ShardCloud
join the accepted native Card/CardCloud stream. Their CPU draw submissions,
MLR owner allocation/registration and redundant Start/Destroy forwarding are
removed. MLRPointCloud is deleted; MLRTriangleCloud remains as Tube's parent.
MLRLineCloud had no constructor or serialization-factory callers and is deleted
with its build/registration/header/dispatch closure. Numeric class IDs remain
reserved. Serialized FX and particle layouts are unchanged.

Remaining live render callers:
- Tube::Draw -> MLRClipper::DrawEffect for zero textures and unsupported
  OneZero/opaque state; its indexed triangle owner keeps MLRTriangleCloud alive.
- Shape::Draw -> DrawScalableShape fallback for unsupported/missing geometry;
  its native upload still collapses primitive materials to the first material.
- ShapeCloud::Draw and DebrisCloud::Draw -> DrawScalableShape per instance.
- PertCloud::Draw -> MlrCloudDraw/MLRNGonCloud. Zero stock specs does not make
  this mod-loadable class dead.
- Native Card/Shard alignment and texture resolution still consume MLR camera,
  state and texture types; embedded shapes/state serialization and editor/FX
  tools retain the shared library closure. CPU effect simulation is unchanged.

Final native Point/Shard captures are repeated, fingerprinted Vulkan present
readbacks with validation enabled and zero VUID/error/overflow reports. The
existing frozen-cloud tests cover all alignments, clipping, expiry, per-vertex
geometry/tint and draw/flush RNG preservation. The fired master OFF/ON pair
matched 2,555 shared frames, chain `a64f58991586fb87`, full hash
`a60b5e5c0ff2a613`, RNG `1914/769/0`, anim RNG `66`; no baseline was rewritten.
See `p6-summary.json` and `owner-montage-p6.png` in the existing proof directory.


### Packet 7 partial checkpoint (2026-10-26)

Engine `9bd7b3c28`: deferred Tube texture pool index -> GOS image resolution;
authored blend factors retained; uncalled immediate ribbon API removed. Gross
engine diff: 95 lines deleted / 80 added. Gates unchanged, no new class accepted.
Untextured/OneZero fixture 9 was populated on CPU and candidate `6a0388903`;
native broad black/white panels fail acceptance. Its extension was withdrawn.
See `owner-montage-p7.png` and `p7-oracle-checks.json` in the existing proof dir.
Tube's CPU mesh owner and both triangle-cloud TUs remain; Shape submaterials,
ShapeCloud, DebrisCloud and mod-loadable PertCloud are still pending in that order.
MLR remains 50 compiled TUs / 23,373 cpp lines; its source/header inventory is
unchanged at 107 files / 36,317 physical lines. Camera/state/texture and legacy
asset decoding still serve native harvesting, game/editor and FX tools.

Final build/7 covering rows/21 Python tests/native frozen tests/smoke-lite pass;
Vulkan validation is clean. Final OFF/ON pair matches 1,788 sim frames, but the
committed-baseline stage fails because mc2_01 has no baseline in this checkout.
Do not present this as a passing committed-baseline check. Baselines unchanged.


### Packet 9 runtime update (2026-10-26; supersedes the opening runtime table)

Shape, ShapeCloud and DebrisCloud now submit per-primitive native meshes; their
CPU scaled submission and MLRClipper::DrawScalableShape are deleted. Primitive
materials combine the instance and authored state, polygon indices become fans,
and the deferred mesh queue resolves MLR texture indices after ForceLoadImages.
Pert emits live center/edge-colored fans and has no MLR owner; MLRNGonCloud is
removed. The LAB-only frustum caller and MLRClipper::DrawShape are also removed.
No gate flips. Stock mesh and synthetic Pert witnesses are in the P9 proof dir;
see the design's P9 section for the exact reference commits and scope.

The remaining live default MLR geometry route is opaque/untextured Tube ->
DrawEffect -> MLRIndexedTriangleCloud. Camera/state/texture setup and asset codecs
remain. Primitive clipping/lighting/sorter mesh queues are now deletion candidates,
not asset-codec deletion permission. MLR is 49 compiled C++ files / 20,821 lines,
105 source/header files / 33,677 physical lines. CPU effect simulation and its
camera-dependent cosmetic ordering remain unchanged.
