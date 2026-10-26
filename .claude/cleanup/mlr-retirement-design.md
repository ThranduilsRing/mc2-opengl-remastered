# R2: GPU particle parity oracle and full MLR retirement
Design only, 2026-10-13; source snapshot `254751f589429aaf9f33e2c46249fec3c97159be`, A:/Games/mc2-codex.
Target: retain gosFX CPU effect simulation, prove GPU rendering, promote the default, then remove the complete MLR dependency.

## Scope and decision

This implements R2 after R1's trustworthy capture oracle (`.claude/cleanup/target-architecture-2026-10-12.md:247`).
No implementation, builds, launches, commits, index writes, or changes outside this document are part of this task.
The final release has one Vulkan game renderer; retain GL comparison arms only while that backend exists.
Keep both mc2fx tools and mod content; do not equate deletion of game GL with deletion of a modder tool's GL UI.
Owner constraints: one final release, editor priority, videos stay, malformed readers log/reject without crashing
(`.claude/cleanup/ORCHESTRATOR-HANDOFF.md:25`, `.claude/cleanup/ORCHESTRATOR-HANDOFF.md:68`, `.claude/cleanup/HANDOFF-LOG.md:13`).
Proposed owners: lane 1 handles gameplay separation and gosFX/assets; lane 2 handles render packets, Vulkan and captures;
orchestrator sequences joint proofs and deletion commits. No concurrent edits of shared surfaces.

## Current runtime and size

MLR is a CPU geometry/state/sorting layer feeding gos draws, not a replacement software pixel rasterizer:
`code/gamecam.cpp:742` starts it, `code/gamecam.cpp:1014` flushes it, `mclib/mlr/mlrsorter.cpp:55` submits triangles.
Game initialization constructs its sorter, texture pool and clipper (`code/mechcmd2.cpp:1008`, `code/mechcmd2.cpp:1016`, `code/mechcmd2.cpp:1019`).
The editor also constructs it (`editor/Editor.cpp:291`, `editor/Editor.cpp:300`); game-only deletion is insufficient.

| Disjoint source/header group | Physical lines | Runtime responsibility / evidence |
|---|---:|---|
| `mlrclipper.*`, `mlrclippingstate.*` | 1,406 | Camera transforms, clip flags, scaled shapes and effect dispatch (`mclib/mlr/mlrclipper.cpp:428`, `mclib/mlr/mlrclipper.cpp:634`, `mclib/mlr/mlrclipper.cpp:699`; `mclib/mlr/mlrclippingstate.hpp:1`). |
| `mlrlight.*`, ambient/infinite/infinite-with-falloff/lookup/point/spot light pairs | 2,331 | Registered light classes (`mclib/mlr/mlr.cpp:136`, `mclib/mlr/mlr.cpp:147`, `mclib/mlr/mlr.cpp:185`); shape-space lighting (`mclib/mlr/mlrclipper.cpp:441`, `mclib/mlr/mlrclipper.cpp:512`). |
| `mlrlightmap.*` | 1,786 | Streamed textured light polygons, CPU clipping and triangulation, sorter emission (`mclib/mlr/mlrlightmap.cpp:182`, `mclib/mlr/mlrlightmap.cpp:240`, `mclib/mlr/mlrlightmap.cpp:675`, `mclib/mlr/mlrlightmap.cpp:1206`). |
| `mlrsorter.*`, `mlrsortbyorder.*` | 1,690 | Deferred lighting/clip work, state/texture ordering and alpha triangles (`mclib/mlr/mlrsortbyorder.cpp:321`, `mclib/mlr/mlrsortbyorder.cpp:325`, `mclib/mlr/mlrsortbyorder.cpp:605`). |
| Remaining geometry/clouds/state/textures/images/vertices/registration | 31,499 | Explicit library closure (`mclib/mlr/CMakeLists.txt:5`, `mclib/mlr/CMakeLists.txt:39`, `mclib/mlr/CMakeLists.txt:42`, `mclib/mlr/CMakeLists.txt:49`). |
| Total: 113 `.cpp/.hpp/.h` files | **38,712** | Library is still linked into the game (`mclib/mlr/CMakeLists.txt:62`, `CMakeLists.txt:697`). |

Count command used, from this worktree (physical lines including comments/blanks; excludes CMake/data; not executed-line counts):
```powershell
@'
from pathlib import Path
groups = {'clip': {'mlrclipper','mlrclippingstate'}, 'lights': {'mlrlight','mlrambientlight','mlrinfinitelight','mlrinfinitelightwithfalloff','mlrlookuplight','mlrpointlight','mlrspotlight'}, 'lightmap': {'mlrlightmap'}, 'sort': {'mlrsorter','mlrsortbyorder'}}
for root in ['mclib/mlr','mclib/gosfx','mclib/particles']:
    fs = [p for p in Path(root).rglob('*') if p.suffix in {'.cpp','.hpp','.h'}]
    print(root, len(fs), sum(len(p.read_bytes().splitlines()) for p in fs))
    if root == 'mclib/mlr':
        for group, stems in groups.items():
            print(group, sum(len(p.read_bytes().splitlines()) for p in fs if p.stem in stems))
'@ | py -3 -
```
Result: MLR 38,712; gosFX 16,291 in 38 files; particles 2,992 in 22 files
(`mclib/gosfx/effect.cpp:1`, `mclib/particles/batcher.cpp:1`). Remaining MLR group = total minus the four named groups.
Additional count command used: Python `re.findall(r'^\s*([A-Za-z0-9_]+[.]cpp)\s*$', Path('mclib/mlr/CMakeLists.txt').read_text(), re.M)`,
then sum `len((Path('mclib/mlr')/name).read_bytes().splitlines())`: **53 explicit CMake TUs, 25,447 lines**
(`mclib/mlr/CMakeLists.txt:4`). This is a source-size deletion opportunity, not a predicted executable-size saving.

Important limits on the lights claim:
- Shape/ShapeCloud/DebrisCloud submit **zero active lights** (`mclib/gosfx/shape.cpp:326`, `mclib/gosfx/shapecloud.cpp:326`, `mclib/gosfx/debriscloud.cpp:830`).
- Light-map calls remain conditional on active light maps in immediate/deferred shape paths (`mclib/mlr/mlrclipper.cpp:501`, `mclib/mlr/mlrclipper.cpp:558`, `mclib/mlr/mlrsortbyorder.cpp:376`).
  Light-map class initialization/termination is commented out (`mclib/mlr/mlr.cpp:152`, `mclib/mlr/mlr.cpp:298`); do not claim an observed light-map draw from link reachability.
- gosFX PointLight evaluates light curves (`mclib/gosfx/pointlight.cpp:295`), but its host returns a sentinel and change/delete are empty
  (`mclib/particles/light_manager.cpp:17`, `mclib/particles/light_manager.cpp:21`, `mclib/particles/light_manager.cpp:29`). This is not a working GPU light replacement.
- Camera sun/ambient and explosion/weapon lights are separate TG lights (`mclib/camera.cpp:369`, `mclib/camera.cpp:396`, `code/carnage.cpp:729`, `code/weaponbolt_update.cpp:1005`). Preserve them when retiring MLR.
- Software-era branches survive: renderer 3 disables detail, maximum-UV capability can disable light maps
  (`mclib/mlr/mlrclipper.cpp:133`, `mclib/mlr/mlrclipper.cpp:141`). Deferred CPU transforms/lighting are compiled via `CalDraw`
  (`mclib/mlr/mlrsorter.hpp:69`, `mclib/mlr/mlrsortbyorder.cpp:321`). These overlap the counts above; do not add another software subtotal.

## Gate/readers and actual parity gaps

`MC2_GPU_PARTICLES` defaults false in the typed table and feature view (`Gates/gate_table.cpp:387`, `RenderCore/RendererFeatureRegistry.h:890`).
Direct runtime reads: routing cache (`mclib/particles/batcher.cpp:78`) and effect diagnostics (`mclib/gosfx/effect.cpp:44`).
Routing consumers: Card/CardCloud/ShardCloud/PointCloud Draw (`mclib/gosfx/card.cpp:508`, `mclib/gosfx/cardcloud.cpp:508`,
`mclib/gosfx/shardcloud.cpp:325`, `mclib/gosfx/pointcloud.cpp:482`); emit/group/flush (`mclib/particles/batcher.cpp:212`,
`mclib/particles/batcher.cpp:233`, `mclib/particles/batcher.cpp:311`); card/cardcloud/point/shard/tube spawn helpers
(`mclib/particles/spawn_card.cpp:61`, `mclib/particles/spawn_cardcloud.cpp:73`, `mclib/particles/spawn_point.cpp:73`,
`mclib/particles/spawn_shard.cpp:97`, `mclib/particles/spawn_tube.cpp:86`); trail submission (`mclib/particles/gpu_trail.cpp:118`).
Weapon trail ownership consults the helper only after promotion, currently always false (`code/weaponbolt.cpp:432`, `code/weaponbolt.cpp:458`).
Camera resolves textures and flushes packets/ribbons (`code/gamecam.cpp:934`, `code/gamecam.cpp:952`, `code/gamecam.cpp:961`).

| Current gap | Required parity work |
|---|---|
| Master ON alone takes placeholder Spawn unless CPU harvesting is separately enabled (`mclib/gosfx/card.cpp:597`, `mclib/gosfx/cardcloud.cpp:747`, `mclib/particles/batcher.cpp:114`); all three render-oracle gates are false (`Gates/gate_table.cpp:1222`, `Gates/gate_table.cpp:1223`, `Gates/gate_table.cpp:1227`). | Adopt live gosFX state as the sole submission source; remove placeholder resimulation and consolidate routing before promotion. |
| Tube has a real swept-mesh route, with unsupported/texture-failure MLR fallback (`mclib/gosfx/tube.cpp:1373`, `mclib/gosfx/tube.cpp:1458`, `mclib/gosfx/tube.cpp:1469`). | Preserve ribbons and polygon profiles, winding, UVs and age; implement Vulkan indexed tubes, then prove every supported content profile. Do not replace trails with spaced cards. |
| Shape route accepts indexed primitives and falls back; first texture/blend is used (`mclib/gosfx/shape.cpp:397`, `mclib/gosfx/shape.cpp:442`, `mclib/gosfx/shape.cpp:551`). ShapeCloud/DebrisCloud/PertCloud still submit MLR (`mclib/gosfx/shapecloud.cpp:394`, `mclib/gosfx/debriscloud.cpp:851`, `mclib/gosfx/pertcloud.cpp:678`). | All leaf types, submesh materials, per-particle transforms/colors, child nesting and polygon tessellation need native packets. Count zero fallback by class/spec. |
| VK island descriptors supply UBO+SSBO; fragment output is a radial blob, no sprite texture (`GameOS/gameos/vulkan_vfx_island.cpp:351`, `shaders/vulkan/vfx_particle.frag:22`). | Textures, atlas animation, subrect UVs, addressing/filtering, cutoffs, age fade and authored shape silhouettes; port selected soft/lit/bloom participation with controlled options. |
| Tubes/mesh have GL drawing bridges (`GameOS/gameos/gos_particle_bridge_tube.cpp:470`, `GameOS/gameos/gos_vfx_mesh_bridge.cpp:307`); VK-declined billboard work falls back to discarded GL output (`GameOS/gameos/gos_particle_bridge.cpp:394`). | Native VK ribbons/meshes, correct scene color/depth ownership and explicit failure status. Never grade discarded fallback as rendered work. |
| Two-value blend classification collapses OneOne/AlphaOne (`mclib/gosfx/card.cpp:523`, `mclib/gosfx/tube.cpp:1386`); current tube pipeline is ONE/ONE while VK billboard uses SRC_ALPHA (`RenderCore/PipelineRegistry.cpp:306`, `GameOS/gameos/vulkan_vfx_island.cpp:203`). | Typed blend recipes derived from content and reference behavior; prove alpha/additive separately, including RGB at alpha zero. |
| Texture resolution and serialized state/geometry still require MLR (`mclib/particles/batcher.cpp:254`, `mclib/gosfx/effect.cpp:199`, `mclib/gosfx/shapecloud.cpp:34`, `mclib/gosfx/debriscloud.cpp:217`). | Independent bounded FX codec and asset texture/mesh IDs; deleting clipping alone cannot delete MLR. |
| Batcher drops at budget and is an intentionally leaked singleton (`mclib/particles/batcher.cpp:234`, `mclib/particles/batcher.cpp:345`); shape cache uses pointer IDs (`mclib/gosfx/shape.cpp:434`). | RenderWorld-owned arenas/cache with stable generation IDs, explicit teardown, bounded growth and logged overload behavior. Stress proof must report zero unexpected drops. |

Input naming note: `Get-ChildItem GameOS/gameos -Filter 'gos_fx*'` found no files. Actual bridges are the particle/tube/mesh files cited above.
Old oracle prose is not acceptance evidence: e.g. default-ON language in `tests/smoke/matrices/vfx.json:56` conflicts with `Gates/gate_table.cpp:1222`.
An existing CPU-harvest route is an implementation aid, not proof of effects-heavy parity.

## gosFX retention, gameplay boundary and RNG

Retain authored curve evaluation, emission accumulators, live particles/profile history, child scheduling, transforms/bounds and cosmetic completion
(`mclib/gosfx/particlecloud.cpp:370`, `mclib/gosfx/particlecloud.cpp:518`, `mclib/gosfx/effect.cpp:679`, `mclib/gosfx/tube.cpp:1007`).
Target: CPU gosFX emits billboard/indexed-mesh/light packets to a renderer-owned sink; it neither clips/project vertices on CPU nor owns GPU resources.
Keep simulation on CPU for R2. GPU compute migration is a separate decision after this oracle; compare-only CardCloud plumbing does not establish whole-system parity
(`mclib/gosfx/cardcloud.cpp:517`, `mclib/particles/batcher.cpp:128`, `mclib/particles/batcher.cpp:310`).

**There are current gameplay-side reads of effect state.** Hit detection itself uses travel distance or beam time,
not gosFX completion (`code/weaponbolt_update.cpp:283`, `code/weaponbolt_update.cpp:550`); weapon damage is applied in the hit path
(`code/weaponbolt_update.cpp:476`, `code/weaponbolt_update.cpp:540`). However:
- Execute completion clears bolt `inView`, and update returns it (`code/weaponbolt_update.cpp:1057`, `code/weaponbolt_update.cpp:1067`, `code/weaponbolt_update.cpp:1296`).
  The object manager destroys weapons on update false (`code/objmgr_update.cpp:1315`). Even pre-hit trail completion is read (`code/weaponbolt_update.cpp:863`).
- Carnage Execute false ends the object (`code/carnage.cpp:781`, `code/carnage.cpp:795`, `code/objmgr_update.cpp:1326`);
  a separate explosion timer enables tangibility (`code/carnage.cpp:755`), and collision applies damage (`code/carnage.cpp:350`, `code/carnage.cpp:367`).
  Thus a short cosmetic lifetime could interact with collision lifetime; this must be characterized, not silently declared presentation-only.
- Smoke/sensor grep found no gosFX or smoke references in `code/contact.cpp`/`code/team_los.cpp`.
  Contact logic uses ECM/range/LOS (`code/contact.cpp:309`, `code/contact.cpp:333`); LOS tests terrain height/forest (`code/team_los.cpp:389`, `code/team_los.cpp:405`).
  This is a bounded source audit, not a promise about unsurveyed script/mod semantics.

Before extraction, lane 1 separates authoritative projectile travel, impact/damage, explosion collision and pool retirement into kernel-owned timelines.
Use content-derived logical deadlines/completion recipes if compatibility requires them; never receive completion callbacks from RenderWorld.
If a lifetime choice affects damage, it belongs to authoritative rules/Physical RNG, not a VFX draw; record any intended rule change and prove headless equivalence.
Keep the old CPU closure until this dependency is removed; R2 may develop rendering independently but cannot claim gosFX is render-only before that proof.

Use S1's **VFX presentation stream**, fixed ID `0x56465852`, owned by RenderWorld; optional Presentation save record, excluded from authoritative digest
(`.claude/cleanup/sim-kernel-design.md:165`, `.claude/cleanup/sim-kernel-design.md:187`, `.claude/cleanup/sim-kernel-design.md:205`).
Apply its defined LCG/explicit shaping and rootSeed XOR streamId; capture manifests record initialSeed/state/u64 drawCount/algorithmVersion.
Current gosFX uses Stuff::Random at effect/child/particle/profile births (`mclib/gosfx/effect.cpp:552`, `mclib/gosfx/effect.cpp:684`,
`mclib/gosfx/particlecloud.cpp:570`, `mclib/gosfx/tube.cpp:1039`); Stuff seeds its buffer with gos_rand (`mclib/stuff/random.cpp:54`).
Mission reseeding covers appearance and gated gos RNG, not a complete FX stream reset (`code/mission_init.cpp:78`, `code/mission_init.cpp:81`).
Therefore a smoke seed alone is insufficient: route every cosmetic gosFX random draw through the owned VFX stream, preserving a captured reference recipe during migration.
Record a legacy birth-random tape in batch 0 and feed it to both CPU evaluators for transitional parity; new LCG draws need not recreate the old Stuff sequence.
Any randomness deciding authoritative retirement belongs to its kernel rule stream; never move or duplicate the VFX stream into Sim.
Capture sequence uses stable effect IDs, ordered spawn events, explicit seed/state and fixed effect update steps; draw/flush/culling must consume zero RNG.
Assert identical authoritative digests across effects off/on, cameras, backend and render rates; allow presentation counters to differ only when explicitly declared.

## VFX parity oracle

User-specified coverage floor: among ordinary tier1 missions only **mc2_10** exercises the gosFX particle path.
Tier1 5/5 is boot/regression evidence, not a VFX promotion verdict. Add explicit fired scenes; the existing fixture can fire weapon banks
and arm event-relative capture (`code/warrior.cpp:2103`, `code/warrior.cpp:2130`, `code/warrior.cpp:2149`).
Reuse region manifests/cropping and RenderDoc capture machinery (`tools/visual_verify/regions.py:52`, `tools/visual_verify/regions.py:138`, `scripts/renderdoc_capture.py:201`).

| Required scene | Samples and discriminating observations |
|---|---|
| Missile volley in mc2_10, including miss and water impact | Birth, midflight, impact, dissipating tail, empty cleanup: continuous swept smoke, textured head/muzzle, no second trail. |
| PPC/ER-PPC fired fixture, e.g. mc2_24 or an authored micro-scene | Same phases: additive energy silhouette, branching/nested flashes, impact timing; forced firing supplies coverage absent from tier1. |
| Explosion with adjacent actors/buildings | Flash, debris and expanding cloud: all mesh/cloud/Pert leaves, light pool, depth intersections, damage transcript independently unchanged. |
| Persistent and overlapping smoke, near ground and behind a building | Emission, steady state and shutdown: atlas frames, spin/aspect, alpha ordering, fog, occlusion, light response, no square/ladder artifacts. |

Each family needs day/night, close/wide views, overlapping alpha/additive effects and a clip-plane crossing.
Pin 1920x1080, camera, assets/mod hashes, render options/tonemap/exposure, pipeline capability selection, executable fingerprint and spawn-event/tick sequence.
Freeze mech/building animation with `MC2_SMOKE_FREEZE_ANIM=1` (`mclib/anim_freeze.h:12`); step effects to chosen ages, then freeze the sampled state.
Use explicit mission and VFX seeds; restore stream state before births. Proposed sample offsets: 0/3/8/15/30/60 effect ticks, plus asset-specific expiry.
Reject missing effects, zero births/zero draws in required regions, unset RNG metadata, overflow, routing fallback, backend mismatch or differing scenario transcripts.

Arms while GL exists: legacy GL (GPU/oracle-render/tube/shape all 0), candidate GL (all four 1), legacy VK, candidate VK.
Keep GPU-simulation experiments OFF. Each candidate must match the same recorded CPU simulation state; compare GL legacy-to-candidate first,
then VK-to-canonical legacy appearance, diagnosing backend differences independently. After GL deletion use archived reference captures/packets with their metadata.
VK arms require the umbrella as well as the selector (`tools/visual_verify/capture_ab.py:90`); require requested=drawn backend and `vk_present` readback,
following the false-VK capture ruling (`.claude/cleanup/HANDOFF-LOG.md:19`). No re-bless conceals a mismatch.

Acceptance has three layers:
1. Canonical effect-state/packet digest: stable IDs, live counts, age/color/UV/transform, mesh topology/material spans, textures and light recipes.
   Same-arm repeated runs must match exactly. CPU state must agree before any image comparison.
2. Per-effect region digest of decoded RGBA pixels plus coverage/depth and difference maps; exact equality for unchanged same-backend deletion batches.
   For new projection/rasterization paths, commit justified per-region max-error/changed-pixel bounds from repeat controls before grading candidates.
   Review every effect montage and time sequence; full-frame MAD and nonzero draw counters cannot pass missing silhouettes.
3. RenderDoc pass/resource diff on every unexplained region failure: locate first divergent VFX color/depth/texture/blend result.
   Different draw topology is expected during replacement; do not require identical command streams. R1 replay remains an additional integrity proof
   (`.claude/cleanup/vk-best-practices-review-2026-10-12.md:173`).

Validate the oracle with negative controls: suppress one required effect, bind white for its atlas, flatten a swept trail and perturb its blend.
Every control must fail its named region/pass and identify the responsible effect/material. Missing validation layers or messenger fails the VK proof tier.
Record a compact per-set index and manifest containing all pins, verdict/reason, crops, RenderDoc links, reviewed artifacts and actual backend.

## Migration batches and proof at each boundary

All implementation batches: RelWithDebInfo, offline contracts/doctest first, all affected targets, lease-serialized background harnesses,
agent-viewed captures, per-batch review; orchestrator commits and runs Bash-only checks. This document claims no executed runtime proofs.

| Batch / responsible lane | Deliverable | Required proof before advancing |
|---|---|---|
| 0: oracle baseline / lane 2 with lane 1 fixture support | R1 capture oracle dependency; deterministic spawn replay, stream reset, regions and manifests; inventory every stock/mod FX leaf/state/geometry kind. | Repeat legacy controls exact; four effect families visibly populated; all negative controls fail; real VK readback and validation reported. Corpus inventory binds coverage to asset hashes. |
| 1: gameplay/RNG boundary / lane 1 | Extract combat/collision/retirement timelines; RenderWorld owns CPU gosFX and VFX RNG; remove presentation completion reads from kernel. No renderer default change. | Reference hit/damage/contact/retirement transcripts, short-FX-before-impact and early-expiring explosion cases; headless/windowed and effects/camera/rate invariant sim digests. VFX seed/state restore and draw-count proof; mark approved semantic changes. |
| 2: billboard parity / lane 1+2 sequential | Harvest live Card/CardCloud/ShardCloud/PointCloud into typed packets; native textured VK materials/atlases; remove placeholder Spawn duplication. | CPU/packet equality; GL/VK scene regions and montage sequences; birth/expiry/nesting, alpha-zero additive, depth/fog and selected render options; no double submission/dropped records. |
| 3: non-billboard parity / lane 1+2 sequential | Native swept tubes, Shape/ShapeCloud/DebrisCloud/PertCloud, submesh materials, validated triangulation. MLR remains reference only. | Every inventoried kind has a fixture; missile/PPC/explosion/smoke sequence and clip-plane passes; zero unsupported/missing-texture fallbacks; native VK color/depth draws; malformed geometry rejects without crash. |
| 4: assets/lights/tools closure / lane 1+2 | Bounded .fx codec decodes legacy state/mesh bytes into modern values; independent texture/mesh resolution, render lighting, owned caches; both tools use the same codec. | Old/new catalog and full dumps byte-identical; rebuild/curve samples unchanged for stock+named mod corpus; light-map/lighting fixtures or documented non-use per content; ASan malformed corpus, init/unwind/resize/reload/teardown checks. |
| 5: promote / orchestrator | With batches 0-4 green, set master default ON, make complete harvested route the default, retain one temporary rollback switch with retirement batch 7. | Unset/default output equals explicit candidate=1; =0 reference still works during overlap. Full tier1 plus fired VFX matrix, editor FX workflow, mod corpus and Tracy/VRAM deltas. No passive-smoke-only promotion. |
| 6: remove MLR rendering / lane 1+2 | Remove clipper/sorter/cloud transforms/software branches, light classes/light-map drawing after replacement consumers; preserve TG scene lights. Keep only codec migration residue until batch 7. | Exact same-backend region digests vs batch 5, sim transcripts unchanged; no legacy render calls in link/map; lights/depth and editor captures; all targets and malformed-content checks green. Recount removed lines. |
| 7: complete retirement / orchestrator with lane edits | Remove residual MLR state/shape/texture types, initialization, includes, build/glob closure and losing gates; delete mclib/mlr after archive/tag and read-only deletion audit. | No MLR dependency in game/editor/mc2fx/preview link maps or runtime; corpus dump/rebuild identity, curve preview/editor workflows, fired VK matrix, tier1 and full contracts/doctest. Report gross deletion and net replacement counts separately. |

Do not execute R1 game-GL deletion before the legacy FX reference is archived and all native VK leaf types pass.
R2 uses R1's device/capture/ownership/synchronization seams; no new island-private static owner or per-effect fence/idle wait.
Native VFX must honor R1 capability-selected Vulkan 1.2 paths, frame dependencies and deferred resource retirement
(`.claude/cleanup/vk-best-practices-review-2026-10-12.md:141`, `.claude/cleanup/vk-best-practices-review-2026-10-12.md:175`).
GPU compute/bindless/indirect submission may follow measured need; none substitutes for the CPU-gosFX visual parity proof.

## mc2fx, content and deletion contract

Both tools compile MLR sources today (`tools/mc2fx/CMakeLists.txt:15`, `tools/mc2fx/CMakeLists.txt:25`,
`tools/mc2fx_preview/CMakeLists.txt:18`, `tools/mc2fx_preview/CMakeLists.txt:27`); headless core even initializes MLR and a texture pool
(`tools/mc2fx/mc2fx_core.cpp:342`, `tools/mc2fx/mc2fx_core.cpp:345`). Rebuild them in the API-changing batch, not after deletion.
Retain dump/full dump, rebuild, edit/clone/new and seed-dependent curve preview workflows
(`tools/mc2fx/main.cpp:237`, `tools/mc2fx/main.cpp:145`, `tools/mc2fx/main.cpp:199`, `tools/mc2fx_preview/main.cpp:88`).
Preview is a curve grapher with a sample-only fallback, not a proven in-game FX viewport
(`tools/mc2fx_preview/CMakeLists.txt:76`, `tools/mc2fx_preview/main.cpp:239`); require GUI and fallback proofs separately.

Preserve supported .fx bytes/class IDs/version fields, texture names, curve values, events and embedded geometry.
Implement a dedicated bounded serialization schema plus typed runtime materials/geometry; do not retain MLR classes under new names.
Legacy wire fields belong to the codec, never to render ownership. Unknown/unbounded/invalid records log an explicit rejection before allocation.
Tool sessions own codec/libraries directly, without renderer startup, singleton registries or static-duration owners.
Corpus includes stock plus MC2X-CVE-G, MC2X-REAP3, Exodus, MagicExpansion, MechCommanderOmnitech and MCO-Carver5 where installed;
missing corpus members are named unproven coverage, never counted as passing.
Proof recipes for implementers: `py -3 tools/build_all_targets.py`; `py -3 tools/build_contract_harnesses.py --run --unit`;
orchestrator `bash scripts/check-contracts.sh`; harness tier1 and fired captures; `mc2fx dump --full` and `mc2fx rebuild` against archived bytes.
The rebuild command already distinguishes identical from changed output (`tools/mc2fx/main.cpp:104`); require exact identity on supported unchanged assets.

## Risks and owner question

- Lifecycle separation can change damage/pool reuse even when images match: batch 1 blocks render-only completion claims.
- Sparse/transient FX and false VK readback can hide loss: populated fixtures, manifest backend checks and negative controls block promotion.
- Texture/blend/profile/submesh differences can look plausible but lose authored content: asset inventory and per-family temporal crops are mandatory.
- Compiled light-map code is not proof it works; preserve surveyed content requirements, isolate repair fixtures, and never silently replace a light map with a point light.
- Buffer/texture reuse, stale pointer IDs, bounds and partial-init errors require ownership/failure/ASan proof; overload policy must never affect combat.
- R1 ordering and editor/tool closures can leave hidden MLR links: deletion includes every target and cannot be accepted from the game link alone.

1. Must post-impact WeaponBolt/Carnage retirement retain timing equivalent to gosFX completion, or may batch 1 use explicit combat deadlines after impact/collision? Proposal: combat-owned deadlines; preserving timing requires a kernel-owned content lifetime recipe and compatibility fixtures. Keep the existing CPU dependency isolated until this policy is resolved (`code/weaponbolt_update.cpp:1067`, `code/carnage.cpp:795`, `code/objmgr_update.cpp:1315`).

## Design-task verification limits

Read-only inspection and physical-line census only. No builds, contract suite, doctest, pytest, tier1, capture, RenderDoc, tool rebuild or runtime parity were run.
These are future batch proofs, not necessary for this Markdown output; Bash/index actions are also forbidden in this task's sandbox.
No owner approval is inferred from writing this design; only the numbered gameplay policy question changes the proposed architecture.


## Retirement packet P1 (2026-10-25) and P2+

P1 base: `b66ad0b0983e`, branch `claude/mlr-retire-p1`. The historical design above is a
snapshot, not the present gate inventory. See `mlr-gosfx-inventory-p1.md` for the current
runtime closure and per-class promotion blockers. Proofs live under
`E:/mc2-archive-2026-10/proofs/mlr-retire-p1/`; only observed coverage counts as parity.

- P1 deletes the uncalled snapshot spawners and retires `MC2_VFX_AGE_SAMPLE`.
  All surviving GPU billboard draws harvest live CPU gosFX state. Card/CardCloud now
  reuse the CPU's animated UV rectangles; swept tubes preserve authored texture addressing.
  Tube and Shape already default ON at this base; the master remains OFF until every
  routed class has populated, repeatable A/B evidence. The render-oracle gate currently
  suppresses weapon impact/miss/muzzle draws; it is not a harmless harvesting selector.
- P2: finish Card/CardCloud exact world corners, orientation, size and atlas parity;
  replace PointCloud square proxies and ShardCloud billboard proxies with their original
  primitives. Remove floors, head brightening/depth pull and impact suppression after
  populated day/night birth/flight/impact/expiry, alpha/additive and clip-crossing proofs.
  Compare sim chains across camera/effect settings; render/flush must consume zero RNG.
  Then enable the master and retire the redundant render selector; retain CPU reference
  rendering only for classes still awaiting their own proof. Do not delete CPU Execute.
- P3: finish Tube profiles/material blends/texture failures and Shape submesh materials;
  migrate ShapeCloud, DebrisCloud and PertCloud to native indexed packets. Cover nested
  effects and named mod catalogs, not just stock fixtures. Remove each CPU draw/clip path
  only after its A/B regions and lifecycle snapshots pass; zero unexplained fallback.
- P4: separate bounded legacy FX/state/shape decoding and texture lookup from MLR;
  retain mc2fx dump/rebuild/preview and exact supported-content bytes. Replace no-op FX
  PointLight plumbing only with an explicit lighting contract; retain independent TG lights.
  Then delete clipper/sorter/cloud geometry/lights and MLR initialization/link closure
  across game, editor and tools. Check all targets at that API checkpoint.
- P5: retire CPU gosFX only after emission, curves, child ordering and cosmetic retirement
  have equivalent owned GPU recipes. Weapon/Carnage authoritative timelines must remain
  independent. Effect scheduling/legacy randomness is camera-dependent in existing code;
  moving it to GPU must not consume or reorder authoritative RNG. Do not count CPU render
  deletion as GPU simulation migration. Final proof includes unchanged sim chains, named
  mod corpus, reload/teardown, validation and smoke, with gross deleted/net added counts.


### Follow-up checkpoint (2026-10-26)

`38f5d620e696` replaces Card/CardCloud billboard approximations with native
triangles from the authored corners/alignment/UVs and restores impact submission.
Frozen geometry/RNG checks and normal pinned deployment pass; see the inventory's
follow-up section and `p2-checks-summary.json`. Visual promotion remains blocked
by a live foreign width-two batch lease; the local harness default is fixed to
one core (`03415190f`). Do not erase the CPU reference or enable the master until
new populated captures pass. Keep Point/Shard on their CPU primitives when the
master eventually promotes cards; their current GPU proxies are not parity.


### P2 relaunch checkpoint and next packet (2026-10-26)

Window 67 base `b91d03ebc`; authored-card proof engine `254f9f5493db`.
See the inventory's relaunch section and `owner-montage-p2.png` in the existing
proof directory. Vegetation wind used wall time during frozen captures; honoring
the existing animation-freeze switch makes all six arms repeat byte-for-byte.
Within-sweep beam differences remain deterministic and unexplained. Native Cards
still lose head/impact flares, so no class is promoted and the CPU Draw references
remain. Point/Shard's inaccurate GPU proxies and the dead screen-quad API are
removed. The unused `MC2_VFX_ORACLE_RENDER` selector is retired, ordinal retained.

P2 next: resolve the first native Card color/depth/projection divergence in these
repeatable captures; accept silhouettes/materials before deleting their CPU Draw
routes and promoting the master. Then implement exact Point/Shard primitives.
Stock Shape (10), ShapeCloud (9), DebrisCloud (34) and mod-loadable PertCloud
(zero stock specs) need populated fixtures; their serialization/runtime closure
stays until that evidence exists. Tube's 31 stock specs need supported-profile,
material and fallback coverage; the current master-only A/B held Tube/Shape OFF.
Continue P3-P5 above for mesh/cloud draw retirement, independent asset decoding,
MLR removal across editor/tools, then GPU simulation only after authoritative
lifetimes and camera-dependent effect ordering are separated. The fired OFF/ON
pair's 5,089 shared strict sim frames match, but its stored data baseline rejects
the copied kit install; that baseline was not changed.

### Packet 4 pass-contract finding (2026-10-26)

The repeatable PPC rejection is not just authored corner extraction. The CPU
reference goes through the postprocessed Vulkan HUD tail: no depth attachment,
Z/RHW dropped, affine UVs, clamped nearest RGBA8 texels and packed tint. Native
Cards draw before post-processing with scene depth, world clip W and sampled
texture. See the inventory's Packet 4 section and `owner-montage-p4.png`.
No class is accepted or CPU route retired; the requested Packet 4 implementation
remains incomplete. A depth-disable patch alone cannot establish equivalence.

Next implementation must put GPU-projected Card/CardCloud geometry in the
equivalent composition phase and preserve mixed alpha ordering/material
semantics, then prove the populated day/night captures before deletion/default
promotion. PointCloud needs its original 2.4-pixel right triangles; ShardCloud
needs its authored aligned triangles/per-vertex tint. Keep those references
until populated captures pass. The old day timeout did not reproduce in the
unchanged smoke budget (both maps pass); its loading-stage cause is unproven.

### Packet 5: native visual acceptance and first live renderer retirement (2026-10-26)

The owner's revised bar supersedes Packet 4's pass-equivalence requirement:
correct scene-depth composition, live authored textures/animation and sensible
blending are sufficient; pixel identity with the depthless HUD reference is not.
Engine `435a6394d9bc` accepts **Card and CardCloud**, enables `MC2_GPU_PARTICLES`
by default, and deletes their CPU submission/ownership plus `MLRCardCloud`
(implementation, header, registration and build entry). The kill-switch now
suppresses accepted cards; it cannot restore the retired renderer. Engine diff
from `f0ec4f452`: **1,010 lines deleted, 33 added, net -977**.

The missing flares intersected scene depth. A tiny constant bias only exposed
small fragments; the accepted route carries live card extent and applies the
existing billboard's size-based camera lift to additive cards, retaining GEQUAL
scene-depth testing and no depth writes. Alpha cards keep authored world depth.
The terrain MVP already includes its axis conversion: the initial attempt to
remove the native shader's inverse conversion was rejected and corrected.
Earlier P1 work already removed snapshot resimulation, preserved live atlas UVs
and fixed repeating Tube UV addressing; those finished steps were not repeated.

Proofs: `E:/mc2-archive-2026-10/proofs/mlr-retire-p1/owner-montage-p5.png`,
`p5-final-oracle/`, `p5-final-native.log`, `p5-final-rows.log`, `p5-all-targets.log`
and `p5-summary.json`. The montage preserves the CPU reference at `625597db4ab7`
and shows native day PPC plus night missile flight/impact at the accepted engine.
Two pale trail strips also occur in the CPU reference; full Tube acceptance and
retirement are explicitly withheld. No CPU Execute/emission/curve/RNG migration
is claimed; legacy camera-dependent effect ordering remains relevant later.

Next packet: implement PointCloud's screen-sized right triangles and ShardCloud's
authored aligned triangles, accept populated day/night scenes using this same
visual bar, then delete their MLR leaves. Finish Tube's untextured/unsupported
blend paths and investigate the inherited pale strips before deleting its CPU
fallback. Migrate Shape submaterials and ShapeCloud/DebrisCloud/PertCloud next.
Then separate legacy asset/state decoding from MLR, remove clipper/sorter/lights
across game/editor/tools, and finally migrate cosmetic simulation with independent
authoritative lifetimes and unchanged sim chains. Weapon visual redesign stays
in the later modernization arc.

### Packet 6: Point/Shard retirement checkpoint (2026-10-26)

Rebased onto window 68 `b6a87c27b`; engine `6a39e5262e35` accepts PointCloud
and ShardCloud at the owner's visible-correctness bar. Point uses its 2.4-pixel
right triangle; Shard preserves authored aligned corners and per-vertex tint.
Opaque primitives ignore vertex alpha. Both harvest live CPU particle state.
Their CPU submission/MLR owners, MLRPointCloud and point sorter dispatch are
deleted. The unconstructed, unserialized MLRLineCloud and line dispatch are
also deleted. Engine diff: 1,092 deleted, 101 added, net -991. No gate flip;
the already-ON master now suppresses these accepted classes when set to zero.

Proofs: `E:/mc2-archive-2026-10/proofs/mlr-retire-p1/p6-final-oracle/`,
`p6-primitives-oracle/`, `p6-opaque-native.log`, `p6-determinism.log` and
`owner-montage-p6.png`. CPU references are pinned to `cc19193edd25`; do not
use the retired classes' current master=0 arm as a CPU reference. Ordinary
stock volleys did not populate Point: existing force-FX fixture 9 now enables
cyan Point emission in memory, without changing asset bytes. Frame 200 covers
stock Shards. Native vertex expansion retains the original 4,096-particle
capacity with 24,576 records; the final captures have no overflow.

Packet 6 remains partial. Next: finish Tube's untextured and opaque/OneZero
fallbacks; equal-or-better pale strips shared with the reference are acceptable.
Use master=1 in both Tube controls so accepted cards/points/shards stay enabled.
Then preserve Shape submaterials and triangulation, implement ShapeCloud and
DebrisCloud, and populate a mod PertCloud fixture (zero stock specs) before
removing their CPU routes. MLR still has 50 compiled TUs / 23,373 source lines
(107 source/header files / 36,317 physical lines), including camera/state/texture
support used by native harvesting and legacy asset decoding in game/editor/tools.
After those consumers migrate, remove clipper/sorter/geometry/lights and the
codec/build closure. CPU Execute/emission/lifetimes remain; later GPU simulation
must preserve authoritative lifetimes and separate camera-dependent FX ordering.
