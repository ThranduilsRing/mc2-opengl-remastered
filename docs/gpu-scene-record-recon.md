# GPU Scene Record Recon — can terrain / props / mechs / VFX share ONE record model?

**Slice:** GPU-SCENE-RECORD-RECON-1
**Date:** 2026-06-11
**Branch:** `claude/nifty-mendeleev` worktree (terrain-pbr-mod session)
**Status:** READ-ONLY recon. No code changed. All file:line cites verified against this worktree.

**Question:** Can terrain chunks, static props, mechs, and VFX meshes share one
GPU-visible scene-record model (record ID, world bounds, material ID, mesh ID,
transform, visibility flags, sort key, owner type, debug name/object ID)?

**Short answer:** **Yes for the CULL/VISIBILITY record — and it already half-exists**
(`gpu_cull::GpuActorRecord`, binding 8 substrate, with the FROZEN-STATIC-CULL-RECORDS
static-prefix `[0,S)` + dynamic `[S,S+D)` model). **No for the per-lane DRAW PAYLOAD
record** — props need a full mat4, mechs need bone offsets instead of a matrix,
terrain has no per-instance transform at all, particles are stateless quads. The
correct shared model is a two-tier split: one unified scene/cull record + per-lane
payload SSBOs keyed by the scene record.

---

## (a) Per-lane record struct dump

### Lane 0 — the existing cross-lane cull substrate (the proto-scene-record)

`gpu_cull::GpuActorRecord` — **64 B std430**, `GameOS/gameos/gpu_cull_record.h:9-33`:

| field | offset | notes |
|---|---|---|
| `worldCenter[3]` | 0 | raw MC2 world coords |
| `boundingRadius` | 12 | |
| `worldAabbMin[3]` | 16 | |
| `category` | 28 | low 4 bits = `GpuActorCategory` (Mech/GroundVeh/Gate/Turret/StaticProp, `gpu_cull_record.h:44-52`); static props pack `typeID<<4` into upper bits (`gos_static_prop_batcher.cpp:4260-4261`) |
| `worldAabbMax[3]` | 32 | |
| `flags` | 44 | `Flag_AlwaysVisible / HasShadow / NeverShadow` (`gpu_cull_record.h:54-59`) |
| `actorId` | 48 | record ID (0 for dynamic-path props) |
| `prevVisibilityBit` | 52 | temporal visibility |
| `consumerFlags` | 56 | AIGate / WeaponSpawnNode / LifecycleGate / RenderGate (`gpu_cull_record.h:61-67`) |
| `blockIdx` | 60 | terrain block index for C1-RB rollup |

- **Binding:** SSBO **8** = `SUBSTRATE_SSBO_BINDING` (`gpu_cull_substrate.cpp:24`,
  shader `shaders/gpu_cull.comp:51`, rollup `gpu_cull_block_rollup.comp:44`).
  Header `GpuActorRecordHeader` 16 B (`gpu_cull_record.h:36-42`). Ring-buffered.
- **Producers:** dynamic actors `code/objmgr.cpp:400`
  (`substrate_submitDynamicActor`); static props via registry flush
  `gos_static_prop_registry.cpp:1201,1289` and dynamic-submit mirror
  `gos_static_prop_batcher.cpp:4229-4305` (`substrate_appendStaticPropRecord`).
- **FROZEN-STATIC-CULL-RECORDS (M1 spec):** frozen pool-aligned static prefix
  `[0,S)` (record-index == instance-pool slot) installed by
  `substrate_rebuildStaticPrefix` (`gpu_cull_substrate.h:72-78`, producer
  `gos_static_prop_registry.cpp:736-772`), dynamic records appended at `[S,S+D)`.
  Gated `MC2_GPU_CULL_STATIC_FROZEN_RECORDS`.
- **Debug-name precedent:** CPU-side parallel actor-ID array
  `substrate_getCpuActorIds` (`gpu_cull_substrate.h:92-98`) — names/IDs stay CPU,
  not in the GPU record.
- Cull aux bindings: VisibleIds 9, BucketCounts 10, Caps/IndirectCmds 11,
  ActorVis 12, BlockVis 13, readback 14, permutation 15
  (`gpu_cull_compute.cpp:43-49`; full table `docs/render-binding-registry.md`).

### Lane 1 — static props (draw payload)

`GpuStaticPropInstance` — **112 B std430**, `gos_static_prop_batcher.h:21-43`:
`modelMatrix[16]` (0), `typeID` (64), `firstColorOffset` (68), `flags` (72,
lightsOut/isWindow/isSpotlight), `lightDataIndex` (76), `aRGBHighlight[4]` (80),
`fogRGB[4]` (96).

- Bindings (restored `gos_static_prop_batcher.cpp:4904-4908`): SSBO 0 `Instances`
  (`shaders/static_prop.vert:55`), 1 `Colors`, 2 `PerType`, 3 `ParityOut`,
  4 `PerDrawData` = `PerDrawEntry` 32 B (`gos_static_prop_batcher.h:52-72`,
  carries `packetID/materialFlags/texArrayLayer/objectIdRaw/materialIdx`),
  5 `MaterialTable` (MaterialGpu), 16 `BaseInstanceByCmd`
  (`gos_static_prop_batcher.cpp:210`).
- CPU-only descriptors: `GpuStaticPropPacket` (`:75-85`, firstIndex/indexCount/
  baseVertex/textureSlot/materialFlags/owningTypeID) and `GpuStaticPropType`
  (`:133-150`, packet range + coalesce caps + `isOverride`).

### Lane 2 — mechs / dynamic skinned actors (draw payload)

`GpuMechInstance` — **64 B std430**, `gos_mech_batcher.h:35-70`:
`typeLodRecordIndex` (0), `baseBoneOffset` (4), `lightDataIndex` (8),
`renderFlags` (12), `aRGBHighlight[4]` (16), `fogRGB[4]` (32), `objectIdRaw` (48),
`materialIdx` (52), `visualDamage01` (56), `visualFlags` (60).

- **No transform in the instance** — transform = bone matrices:
  `GpuMechBone` 64 B (`gos_mech_batcher.h:76-79`).
- Bindings (restored `gos_mech_batcher.cpp:631-632,1535-1537`): SSBO 0
  `InstanceBuffer` (`shaders/mech.vert:46`), 1 `BoneBuffer` (`mech.vert:52`),
  2 mech material table (`gos_mech_batcher.cpp:1347`).
- CPU-only: `GpuMechTypeLodRecord` (`gos_mech_batcher.h:82-89`),
  `GpuMechPacket` (`:92-104`).

### Lane 3 — terrain (two sub-lanes, neither has per-instance records in the prop sense)

**3a. GPU-indirect quad lane** (`gos_terrain_indirect.cpp` / `gos_terrain_patch_stream.h`):
- `TerrainQuadRecipe` — **144 B** static geometry recipe (4 world-pos vec4 +
  4 normal vec4 + uvExt), `gos_terrain_patch_stream.h:87-99`; GLSL mirror
  `shaders/gpu_driven_terrain_solid.comp:61-65`.
- `TerrainQuadThinRecord` — **96 B** per-frame record (recipeIdx, terrainHandle,
  flags, cementWord, 4×lightRGB, 4×clip-space corner vec4),
  `gos_terrain_patch_stream.h:113-128`; GLSL `gpu_driven_terrain_solid.comp:79-92`.
- Bindings: multiplexed SSBO 0-7 within the terrain pass (Recipes 0, Lighting/Cmds 1,
  Window 2, Thin 3, bucket-header 6, PerCmd 7 — see `docs/render-binding-registry.md`
  rows; terrain also reuses 8 `CmdBuf` and 9 `SolidWin` at
  `gos_terrain_indirect.cpp:2920/2926`). **Granularity = quad, not object.**

**3b. LOD chunk lane** (`gos_terrain_lod_chunk.{h,cpp}`, default-on since `a7b090be`):
- `TerrainDrawCommand` — **16 B CPU struct** (blockOriginX/Y, lodStep,
  quadCountsPacked), `gos_terrain_lod_chunk.h:8-15`, submitted per frame via
  `gos_TerrainLodChunk_SubmitDrawCommands` (`:30-36`) with parallel CPU arrays
  (skirtDepths, edge masks, stitch words). **Not an SSBO record** — geometry is
  implicit from the heightfield SSBOs: height binding **23**, terrainType **24**,
  cement **25** (`gos_terrain_lod_chunk.h:17-19`).
- Chunk frustum cull is CPU-side (`Camera::extractFrustumPlanes` on
  `worldToClipGL`, fix `a280dde2`); no GPU cull record exists for chunks.

### Lane 4 — VFX

- **Billboards:** `mc2::particles::GpuParticle` — **64 B**
  (`mclib/particles/spec.h:48-58`: position+pad, color4, velocity3, kind_flags,
  lifetime, age, size, atlasIndex). SSBO binding **14** hard-coded
  (`gos_particle_bridge.cpp:310`, `shaders/particle_billboard.vert:26`) —
  known collision candidate with `READBACK_SSBO_BINDING=14`
  (`gpu_cull_readback.h:18`; `render-binding-registry.md` Known issue #1).
  No bounds, no material ID, no object ID — pure per-particle quad payload.
- **3D mesh VFX (Shape/ShapeCloud/DebrisCloud):** **NO GPU records exist.** Legacy
  path is dead (`MLRClipper::DrawScalableShape` gated off by default,
  `mlr_gate.cpp` `kDefaultDisabled=true`) — see
  `docs/vfx-3d-mesh-substrate-recon.md`. This lane is greenfield.

---

## (b) Field matrix (target scene-record fields × lanes)

Legend: ✔ exists, (✔) exists elsewhere/derivable, ✖ missing, ⚠ incompatible.

| Field | Cull substrate (`GpuActorRecord`) | Static prop (`GpuStaticPropInstance`) | Mech (`GpuMechInstance`) | Terrain quad/thin | Terrain chunk (`TerrainDrawCommand`) | VFX billboard (`GpuParticle`) | VFX 3D mesh |
|---|---|---|---|---|---|---|---|
| record ID | ✔ `actorId` (0 for dyn-path props) | ✖ (slot index implicit; frozen M1 makes slot==record) | (✔) `objectIdRaw` | ✖ (recipeIdx is geometry, not object) | ✖ | ✖ | ✖ (greenfield) |
| world bounds (center/radius/AABB) | ✔ | ✖ (cull substrate owns it; dyn-path uses fixed r=200, batcher.cpp:4250) | ✖ (substrate owns it) | (✔) corner positions ARE the bounds | (✔) derivable from origin+lodStep+heightfield, CPU-only | ✖ (position+size only) | ✖ |
| material ID | ✖ | (✔) `PerDrawEntry.materialIdx` (per-draw, not per-instance) | ✔ `materialIdx` | ⚠ `terrainHandle`+cementWord (texture slot, different namespace) | ✖ (splat from per-vertex type SSBO) | ⚠ atlasIndex | ✖ |
| mesh ID | ⚠ typeID packed in `category>>4` (props only) | ✔ `typeID` | ✔ `typeLodRecordIndex` | ⚠ recipeIdx (per quad) | ⚠ implicit heightfield window | n/a (quad) | ✖ |
| transform | ✖ (center only — by design) | ✔ full mat4 (112 B mostly matrix) | ⚠ NO matrix — `baseBoneOffset` into bone SSBO | ⚠ baked world/clip positions, no matrix | ⚠ grid origin ints, no matrix | ⚠ position+velocity, billboard in VS | ✖ |
| visibility flags | ✔ `flags` + `prevVisibilityBit` + `consumerFlags` | (✔) lightsOut/isWindow bits — render semantics, not visibility | (✔) renderFlags — render semantics | ✖ (window/admission is the visibility) | ✖ (CPU frustum + apron) | ✖ | ✖ |
| sort key | ✖ | ✖ (CPU bucket sort by type/alphaClass) | ✖ (CPU `BucketKey` sort, gos_mech_batcher.cpp:1491-1502) | ✖ (bucket headers) | ✖ | ✖ (group order) | ✖ |
| owner type | ✔ `category` enum | ✖ | ✖ | ✖ | ✖ | ⚠ kind_flags (particle kind) | ✖ |
| debug name/object ID | ✔ actorId + CPU parallel array (`substrate_getCpuActorIds`) | (✔) `PerDrawEntry.objectIdRaw` (per-draw) | ✔ `objectIdRaw` (MC2_OBJECT_ID_BUFFER) | ✖ | ✖ | ✖ | ✖ |

---

## (c) Which lanes CAN share, which must stay separate

**CAN share one layout — the scene/cull tier:**
- **Static props, mechs, vehicles, turrets, gates** already DO share
  `GpuActorRecord` at binding 8. This is the proven nucleus.
- **VFX 3D meshes (Shape/ShapeCloud/DebrisCloud)** can join cheaply: each effect
  instance has a bounding radius in its spec (`m_radius`) and a world transform —
  exactly a `GpuActorRecord` (new `Cat_Vfx`). Greenfield, gated off today, so
  zero regression surface.
- **Terrain chunks** can join the *cull* tier as records (center/AABB from block
  origin + heightfield min/max, `blockIdx` already a first-class field), which
  would unify chunk frustum/HZB cull with the actor cull dispatch. But this is
  optional: chunk CPU cull is O(blocks) and already correct post-`a280dde2`.

**MUST stay separate — the draw-payload tier:**
- **Prop payload (112 B, mat4-dominant)** vs **mech payload (64 B,
  bone-offset-dominant, NO matrix)** are structurally incompatible: skinned
  actors fundamentally index a bone SSBO; forcing a mat4 into the shared record
  wastes 64 B/record on every mech and still doesn't skin. Both also carry lane-
  specific per-instance color state (firstColorOffset vs visualDamage01).
- **Terrain quad/thin records** are per-QUAD (≈10⁴-10⁵/frame), not per-object;
  they bake clip-space corners (the "Fix B" MVP-misalignment rule,
  `gos_terrain_patch_stream.h:120-123`). Wrong granularity and wrong lifetime
  for an object record. Terrain chunk lane has *no* GPU draw record at all —
  geometry is implicit in the height SSBO. Only the cull tier can be shared.
- **Particle billboards** are per-particle transient state (age/velocity);
  bounds/material/sort fields would multiply memory ×N particles for no
  consumer. Keep out entirely; cull at emitter granularity if ever needed
  (emitter could own ONE scene record).

**Conclusion:** one shared **GpuSceneRecord** (= evolved `GpuActorRecord`) for
identity + bounds + visibility + owner type, with a per-record **payload link**
into the lane-private instance SSBO. Transform stays in the payload tier.

---

## (d) Proposed shared scene-record sketch

Keep the 64 B hot cull record byte-compatible; add a cold 16 B extension SSBO
(parallel array, same index) rather than growing the hot record — the cull
compute (`gpu_cull.comp`) never needs material/sort, so don't pay bandwidth there.

```c
// Tier 1 — HOT, unchanged 64 B, binding 8 (gpu_cull_record.h). Renamed view:
struct GpuSceneRecord {            // == GpuActorRecord today
    float    worldCenter[3];  float boundingRadius;
    float    worldAabbMin[3]; uint32_t category;     // ownerType in low 4 bits
    float    worldAabbMax[3]; uint32_t flags;        // visibility flags
    uint32_t recordId;        uint32_t prevVisibilityBit;  // recordId == actorId
    uint32_t consumerFlags;   uint32_t blockIdx;
};

// Tier 2 — COLD extension, NEW binding (next free per registry: SSBO 26).
struct GpuSceneRecordExt {         // 16 B std430, parallel to tier 1
    uint32_t meshId;       // lane-namespaced: prop typeID / mech typeLodRecordIndex /
                           // vfx shape id / terrain chunk id
    uint32_t materialIdx;  // index into the (future unified) MaterialGpu table
    uint32_t sortKey;      // pass<<28 | alphaClass<<24 | material/depth bits
    uint32_t payloadIndex; // slot in the lane's private instance SSBO
};

// Tier 3 — lane payloads, UNCHANGED layouts, keyed by payloadIndex:
//   props: GpuStaticPropInstance[112B] (frozen pool — M1 makes index==slot)
//   mechs: GpuMechInstance[64B] + GpuMechBone[]
//   vfx mesh (new): { mat3x4 transform; vec4 colorScale; uint flags; ... }
//   terrain chunk: none (heightfield SSBOs 23/24/25)
// Debug names: CPU-side parallel array only (substrate_getCpuActorIds pattern).
```

Category enum grows: `Cat_TerrainChunk = 6`, `Cat_VfxMesh = 7` (mask is 0xF,
room exists, `gpu_cull_record.h:51`) — and the prop `typeID<<4` packing moves to
`Ext.meshId`, un-overloading `category`.

---

## (e) Migration order (least-risk first)

1. **Formalize what exists (zero behavior change):** rename-in-docs
   `GpuActorRecord` → scene record tier 1; fix the two registry known-issues
   first (binding-14 particle/readback collision; magic literal at
   `gos_particle_bridge.cpp:310`). Land FROZEN-STATIC-CULL-RECORDS M1 (already
   specced; `substrate_rebuildStaticPrefix` plumbing exists) so static prop
   record-index == pool slot — that IS `payloadIndex` for the prop lane.
2. **Add the Ext SSBO for static props only** (binding 26, default-off env
   gate): populate meshId/materialIdx/payloadIndex from the frozen prefix
   builder (`gos_static_prop_registry.cpp:736-772`). Oracle: Ext.payloadIndex
   == record index for all `[0,S)`; Ext.materialIdx == `PerDrawEntry.materialIdx`.
3. **Mechs/dynamic actors:** fill Ext at `objmgr.cpp:400` submit time
   (typeLodRecordIndex + materialIdx already exist in `GpuMechInstance`).
   De-duplicates `objectIdRaw` (recordId becomes the authority).
4. **VFX 3D meshes (greenfield):** build the new lane ON the shared record from
   day 1 — submit `Cat_VfxMesh` records + a new small payload SSBO; no legacy
   parity needed because the legacy path renders nothing today.
5. **Terrain chunks (optional, last):** emit one `Cat_TerrainChunk` record per
   block so chunk culling can move into `gpu_cull.comp`/HZB later. Only do this
   when there is a concrete consumer (GPU chunk cull or unified pick); the CPU
   chunk cull is not a measured cost.
6. **Sort-key consumer:** only after 2-4, a cull-side bucket/sort using
   `Ext.sortKey` can replace the per-lane CPU bucket sorts — this is the first
   step that changes draw ordering, hence last.

## (f) Explicit non-goals

- **No renderer rewrite.** Lane draw paths (static_prop.vert, mech.vert, terrain
  solid/thin/chunk shaders, particle_billboard.vert) keep their private payload
  SSBOs and bindings unchanged.
- **No unified payload struct.** Mat4-vs-bones-vs-implicit-grid is intrinsic;
  do not flatten into one fat record.
- **No per-particle scene records.** Billboard particles stay in their own SSBO;
  at most one record per emitter, later, if ever.
- **No terrain quad/thin record changes.** The 144 B recipe / 96 B thin record
  layouts are load-bearing (baked clip corners = the MVP-misalignment fix).
- **No binding renumbering** of the multiplexed 0-7 slots; new tier-2 data takes
  a fresh slot (26+) per `docs/render-binding-registry.md` discipline (update
  the registry in the same commit).
- **No CPU gameplay migration** — AI/path/damage stay CPU
  (`docs/modernization-roadmap-2026-06-09.md` §B).

## Cross-references

- `docs/render-binding-registry.md` — authoritative binding tables.
- `docs/model-override-gpu-instancing-proposal.md` — prop geometry/instancing verdict.
- `docs/gpu-static-prop-cull-lessons.md` — why cull gates are load-bearing beyond visibility.
- `docs/vfx-3d-mesh-substrate-recon.md` — VFX mesh lane anatomy (greenfield).
- `docs/modernization-roadmap-2026-06-09.md` §B — "GPU scene records" target shape.
- HANDOFF_2026_06_04_gpucull_oracle_corrected_metafix — FROZEN-STATIC-CULL-RECORDS spec.
